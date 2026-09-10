/**
 * @file beacon_spam.c
 * @brief Beacon spam attack implementation
 * 
 * This module handles WiFi beacon frame spam attacks including:
 * - Single SSID beacon spam
 * - Random SSID beacon spam
 * - Rickroll mode (cycling through Rick Astley lyrics)
 * - AP list mode (broadcasting all scanned APs)
 * - Custom SSID list beacon spam
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/beacon_spam.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/ghostchi_manager.h"
#include "core/system_manager.h"
#include "managers/ghostscript_runtime.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/terminal_screen.h"
#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/hop_profile.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Constants
#define BEACON_LIST_MAX 16
#define BEACON_SSID_MAX_LEN 32
#define RANDOM_SSID_LEN 8
#define BEACON_RANDOM_BURST 4
#define BEACON_FRAME_MAX 104

// External globals from wifi_manager.c
extern RGBManager_t rgb_manager;
extern wifi_ap_record_t *scanned_aps;
extern uint16_t ap_count;
// ap_sta_has_ip is now declared in wifi_manager.h

// Settings
extern FSettings G_Settings;

// Local state
static char (*g_beacon_list)[BEACON_SSID_MAX_LEN + 1];
static int g_beacon_list_count = 0;
static TaskHandle_t beacon_task_handle = NULL;
static volatile bool beacon_task_running = false;
static char *beacon_task_ssid = NULL;

// Forward declarations
static void beacon_spam_task(void *param);
static void beacon_spam_list_task(void *param);
static void generate_random_ssid(char *ssid, size_t length);
static void generate_random_mac(uint8_t *mac);
static void configure_hidden_ap(void);

// Helper function: Generate random SSID
static void generate_random_ssid(char *ssid, size_t length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i < length - 1; i++) {
        int random_index = esp_random() % (sizeof(charset) - 1);
        ssid[i] = charset[random_index];
    }
    ssid[length - 1] = '\0'; // Null-terminate the SSID
}

// Helper function: Generate random MAC address
static void generate_random_mac(uint8_t *mac) {
    esp_fill_random(mac, 6); // Fill MAC address with random bytes
    mac[0] &= 0xFE; // Unicast MAC address (least significant bit of the first byte should be 0)
    mac[0] |= 0x02; // Locally administered MAC address (set the second least significant bit)
}

// Helper function: Configure hidden AP for beacon transmission
static void configure_hidden_ap(void) {
    wifi_config_t wifi_config;

    // Get the current AP configuration
    esp_err_t err = esp_wifi_get_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to get Wi-Fi config: %s\n", esp_err_to_name(err));
        return;
    }

    // Set the SSID to hidden while keeping the other settings unchanged
    wifi_config.ap.ssid_hidden = 1;
    wifi_config.ap.beacon_interval = 10000;
    wifi_config.ap.ssid_len = 0;

    // Apply the updated configuration
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        printf("Failed to set Wi-Fi config: %s\n", esp_err_to_name(err));
    } else {
        printf("Wi-Fi AP SSID hidden.\n");
    }
}

// Broadcast a beacon frame for a specific SSID
esp_err_t beacon_spam_broadcast(const char *ssid) {
    static const uint8_t beacon_template[] = {
        0x80, 0x00, 0x00, 0x00,                         // Frame Control, Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination address (broadcast)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // Source address (randomized later)
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,             // BSSID (randomized later)
        0xc0, 0x6c,                                     // Seq-ctl (sequence control)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp (set to 0)
        0x64, 0x00,                                     // Beacon interval (100 TU)
        0x11, 0x04,                                     // Capability info (ESS)
        0x00, 0x00,                                     // SSID IE tag, length (set later)
    };
    
    // if a station on the AP has an IP, don't hop channels; send on current channel only
    int start_channel = 1;
    int end_channel = 11;
    if (ap_sta_has_ip) {
        uint8_t primary_channel;
        wifi_second_chan_t second_channel;
        esp_wifi_get_channel(&primary_channel, &second_channel);
        start_channel = primary_channel;
        end_channel = primary_channel;
    }

    for (int ch = start_channel; ch <= end_channel; ch++) {
        // Check if we should stop
        if (!beacon_task_running) {
            return ESP_OK;
        }
        
        if (!ap_sta_has_ip) {
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        }

        // Burst multiple random SSIDs per channel hop to amortize the cost of
        // the channel switch; fixed SSIDs send once per channel
        int burst_count = (ssid == NULL) ? BEACON_RANDOM_BURST : 1;
        for (int b = 0; b < burst_count; b++) {
            if (!beacon_task_running) {
                return ESP_OK;
            }

            uint8_t packet[BEACON_FRAME_MAX];
            memcpy(packet, beacon_template, sizeof(beacon_template));

            generate_random_mac(&packet[10]);
            memcpy(&packet[16], &packet[10], 6);

            char ssid_buffer[RANDOM_SSID_LEN + 1];
            const char *ssid_to_use = ssid;
            if (ssid_to_use == NULL) {
                generate_random_ssid(ssid_buffer, RANDOM_SSID_LEN + 1);
                ssid_to_use = ssid_buffer;
            }

            uint8_t ssid_len = strlen(ssid_to_use);
            packet[37] = ssid_len;
            memcpy(&packet[38], ssid_to_use, ssid_len);

            uint8_t *supported_rates_ie = &packet[38 + ssid_len];
            supported_rates_ie[0] = 0x01; // Supported Rates IE tag
            supported_rates_ie[1] = 0x08; // Length (8 rates)
            supported_rates_ie[2] = 0x82; // 1 Mbps
            supported_rates_ie[3] = 0x84; // 2 Mbps
            supported_rates_ie[4] = 0x8B; // 5.5 Mbps
            supported_rates_ie[5] = 0x96; // 11 Mbps
            supported_rates_ie[6] = 0x24; // 18 Mbps
            supported_rates_ie[7] = 0x30; // 24 Mbps
            supported_rates_ie[8] = 0x48; // 36 Mbps
            supported_rates_ie[9] = 0x6C; // 54 Mbps

            uint8_t *ds_param_set_ie = &supported_rates_ie[10];
            ds_param_set_ie[0] = 0x03; // DS Parameter Set IE tag
            ds_param_set_ie[1] = 0x01; // Length (1 byte)
            ds_param_set_ie[2] = ch; // Set the current channel

            // Add HE Capabilities (for Wi-Fi 6 detection)
            uint8_t *he_capabilities_ie = &ds_param_set_ie[3];
            he_capabilities_ie[0] = 0xFF; // Vendor-Specific IE tag (802.11ax capabilities)
            he_capabilities_ie[1] = 0x0D; // Length of HE Capabilities (13 bytes)

            // Wi-Fi Alliance OUI (00:50:6f) for 802.11ax (Wi-Fi 6)
            he_capabilities_ie[2] = 0x50; // OUI byte 1
            he_capabilities_ie[3] = 0x6f; // OUI byte 2
            he_capabilities_ie[4] = 0x9A; // OUI byte 3 (OUI type)

            // Wi-Fi 6 HE Capabilities: a simplified example of capabilities
            he_capabilities_ie[5] = 0x00;  // HE MAC capabilities info (placeholder)
            he_capabilities_ie[6] = 0x08;  // HE PHY capabilities info (supports 80 MHz)
            he_capabilities_ie[7] = 0x00;  // Other HE PHY capabilities
            he_capabilities_ie[8] = 0x00;  // More PHY capabilities (placeholder)
            he_capabilities_ie[9] = 0x40;  // Spatial streams info (2x2 MIMO)
            he_capabilities_ie[10] = 0x00; // More PHY capabilities
            he_capabilities_ie[11] = 0x00; // Even more PHY capabilities
            he_capabilities_ie[12] = 0x01; // Final PHY capabilities (Wi-Fi 6 capabilities set)

            size_t packet_size = (38 + ssid_len + 10 + 3 + 13); // Adjust packet size

            esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
            if (err == ESP_ERR_NO_MEM) {
                vTaskDelay(pdMS_TO_TICKS(2));
                esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1));
        if (ap_sta_has_ip) break; // only one transmit when a client has IP
    }

    return ESP_OK;
}

// Broadcast a beacon frame for Karma attack (uses real AP MAC, hops channels)
esp_err_t beacon_spam_broadcast_karma(const char *ssid) {
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[BEACON_FRAME_MAX] = {
        0x80, 0x00, 0x00, 0x00,                         // Frame Control, Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,             // Destination address (broadcast)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // Source address (set to AP MAC)
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,             // BSSID (set to AP MAC)
        0xc0, 0x6c,                                     // Seq-ctl
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Timestamp
        0x64, 0x00,                                     // Beacon interval (100 TU)
        0x11, 0x04,                                     // Capability info (ESS)
    };

    // Get the real AP MAC address
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    memcpy(&packet[10], ap_mac, 6);  // Source
    memcpy(&packet[16], ap_mac, 6);  // BSSID

    // Get current channel for DS parameter set
    uint8_t primary_channel;
    wifi_second_chan_t second_channel;
    esp_wifi_get_channel(&primary_channel, &second_channel);

    uint8_t ssid_len = strlen(ssid);
    packet[37] = ssid_len;
    memcpy(&packet[38], ssid, ssid_len);

    uint8_t *supported_rates_ie = &packet[38 + ssid_len];
    supported_rates_ie[0] = 0x01; // Supported Rates IE tag
    supported_rates_ie[1] = 0x08; // Length (8 rates)
    supported_rates_ie[2] = 0x82; // 1 Mbps
    supported_rates_ie[3] = 0x84; // 2 Mbps
    supported_rates_ie[4] = 0x8B; // 5.5 Mbps
    supported_rates_ie[5] = 0x96; // 11 Mbps
    supported_rates_ie[6] = 0x24; // 18 Mbps
    supported_rates_ie[7] = 0x30; // 24 Mbps
    supported_rates_ie[8] = 0x48; // 36 Mbps
    supported_rates_ie[9] = 0x6C; // 54 Mbps

    uint8_t *ds_param_set_ie = &supported_rates_ie[10];
    ds_param_set_ie[0] = 0x03; // DS Parameter Set IE tag
    ds_param_set_ie[1] = 0x01; // Length (1 byte)
    ds_param_set_ie[2] = primary_channel; // Current channel

    // Add HE Capabilities (for Wi-Fi 6 detection)
    uint8_t *he_capabilities_ie = &ds_param_set_ie[3];
    he_capabilities_ie[0] = 0xFF; // Vendor-Specific IE tag
    he_capabilities_ie[1] = 0x0D; // Length of HE Capabilities
    he_capabilities_ie[2] = 0x50; // Wi-Fi Alliance OUI byte 1
    he_capabilities_ie[3] = 0x6f; // OUI byte 2
    he_capabilities_ie[4] = 0x9A; // OUI byte 3
    he_capabilities_ie[5] = 0x00;  // HE MAC capabilities
    he_capabilities_ie[6] = 0x08;  // HE PHY capabilities
    he_capabilities_ie[7] = 0x00;
    he_capabilities_ie[8] = 0x00;
    he_capabilities_ie[9] = 0x40;  // Spatial streams
    he_capabilities_ie[10] = 0x00;
    he_capabilities_ie[11] = 0x00;
    he_capabilities_ie[12] = 0x01; // Wi-Fi 6 capabilities

    size_t packet_size = (38 + ssid_len + 10 + 3 + 13);

    // If AP has connected client, only send on current channel
    if (ap_sta_has_ip) {
        esp_err_t err = esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
        return err;
    }

    // Build channel list: user hop profile when set, otherwise the
    // country-appropriate list.
    uint8_t channels[WIFI_CHANNELS_MAX];
    uint8_t channel_count = 0;
    size_t resolved_count = 0;
    hop_profile_resolve(channels, sizeof(channels), &resolved_count);
    if (resolved_count > 0) {
        channel_count = (uint8_t)resolved_count;
    } else {
        channel_count = wifi_channels_build_country_list(channels, sizeof(channels));
    }

    // Hop through the channel list
    for (int i = 0; i < channel_count; i++) {
        uint8_t ch = channels[i];
        if (!wifi_manager_karma_is_running()) {
            return ESP_OK;
        }

        // Set appropriate secondary channel for 5GHz HT40
        wifi_second_chan_t sec_chan = WIFI_SECOND_CHAN_NONE;
#if defined(CONFIG_IDF_TARGET_ESP32C5)
        if (ch > 14) {
            sec_chan = WIFI_SECOND_CHAN_ABOVE;
        }
#endif
        esp_wifi_set_channel(ch, sec_chan);

        // Update DS parameter set with current channel
        ds_param_set_ie[2] = ch;

        esp_wifi_80211_tx(WIFI_IF_AP, packet, packet_size, false);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Switch back to AP's operating channel
    esp_wifi_set_channel(primary_channel, second_channel);

    return ESP_OK;
}

// Beacon spam task (runs in its own thread)
static void beacon_spam_task(void *param) {
    const char *ssid = (const char *)param;

    // Array to store lines of the chorus
    static const char *rickroll_lyrics[] = {"Never gonna give you up",
                                     "Never gonna let you down",
                                     "Never gonna run around and desert you",
                                     "Never gonna make you cry",
                                     "Never gonna say goodbye",
                                     "Never gonna tell a lie and hurt you"};
    int num_lines = 6;
    int line_index = 0;

    int IsRickRoll = ssid != NULL ? (strcmp(ssid, "RICKROLL") == 0) : false;
    int IsAPList = ssid != NULL ? (strcmp(ssid, "APLISTMODE") == 0) : false;

    while (beacon_task_running) {
        if (IsRickRoll) {
            beacon_spam_broadcast(rickroll_lyrics[line_index]);

            line_index = (line_index + 1) % num_lines;
        } else if (IsAPList) {
            for (int i = 0; i < ap_count; i++) {
                if (!beacon_task_running) break;  // Check stop flag in inner loop
                beacon_spam_broadcast((const char *)scanned_aps[i].ssid);
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            beacon_spam_broadcast(ssid);
        }

        vTaskDelay(pdMS_TO_TICKS(settings_get_broadcast_speed(&G_Settings)));
    }
    
    // Clear handle before freeing so stop() never frees a param we already own
    beacon_task_handle = NULL;
    free(param);
    vTaskDeleteWithCaps(NULL);
}

// Beacon list spam task
static void beacon_spam_list_task(void *param) {
    (void)param;
    while (beacon_task_running) {
        for (int i = 0; i < g_beacon_list_count; ++i) {
            if (!beacon_task_running) break;  // Check stop flag in inner loop
            beacon_spam_broadcast(g_beacon_list[i]);
            vTaskDelay(pdMS_TO_TICKS(settings_get_broadcast_speed(&G_Settings)));
        }
    }
    // Clear handle before self-deletion to prevent race condition
    beacon_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

// Start beacon spam with a specific SSID
void beacon_spam_start(const char *ssid) {
    if (!beacon_task_running) {
        ap_manager_stop_services();
        glog("Starting beacon transmission...\n");
        status_display_show_status("Beacon Starting");
        ghostchi_manager_add_xp(3);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP)); // Set AP mode for 802.11 TX
        configure_hidden_ap();
        esp_wifi_start();
        beacon_task_running = true;
        char *ssid_copy = ssid ? strdup(ssid) : NULL;
        if (ssid && !ssid_copy) {
            glog("Failed to allocate SSID copy\n");
            status_display_show_status("Beacon Start Failed");
            beacon_task_running = false;
            esp_wifi_stop();
            ap_manager_init();
            return;
        }
        BaseType_t rc = xTaskCreate_psram(beacon_spam_task, "beacon_task", 3072, (void *)ssid_copy, 5, &beacon_task_handle);
        if (rc != pdPASS) {
            glog("Failed to start beacon task (%ld)\n", (long)rc);
            status_display_show_status("Beacon Start Failed");
            beacon_task_running = false;
            beacon_task_handle = NULL;
            free(ssid_copy);
            esp_wifi_stop();
            ap_manager_init();
            return;
        }
        beacon_task_ssid = ssid_copy;
        rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
        ghostscript_emit_event("attack_started", "beacon");
    } else {
        glog("Beacon transmission already running.\n");
        status_display_show_status("Beacon Active");
    }
}

// Stop beacon spam
void beacon_spam_stop(void) {
    if (beacon_task_running) {
        glog("Stopping beacon transmission...\n");

        // Signal the task to stop
        beacon_task_running = false;
        
        // Wait for the task to delete itself (task clears handle and calls vTaskDeleteWithCaps(NULL))
        // Maximum wait time: 2 seconds (task should exit much faster)
        int wait_count = 0;
        while (beacon_task_handle != NULL && wait_count < 20) {
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_count++;
        }

        if (beacon_task_handle != NULL) {
            vTaskDeleteWithCaps(beacon_task_handle);
            beacon_task_handle = NULL;
            if (beacon_task_ssid) {
                free(beacon_task_ssid);
                beacon_task_ssid = NULL;
            }
        }
        beacon_task_ssid = NULL;

        // Turn off RGB indicator
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);

        // Stop WiFi completely
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK) {
            glog("Beacon stop warning: failed to stop WiFi (%s)\n", esp_err_to_name(stop_err));
        }

        // Reset WiFi mode
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (mode_err != ESP_OK) {
            glog("Beacon stop warning: failed to reset WiFi mode (%s)\n", esp_err_to_name(mode_err));
        }

        // Now restart services
        ap_manager_init();
        status_display_show_status("Beacon Stopped");
        ghostscript_emit_event("attack_stopped", "beacon");
    }
}

// Start beacon spam using the saved list
void beacon_spam_start_list(void) {
    if (g_beacon_list_count == 0) {
        printf("No SSIDs in beacon list\n");
        return;
    }
    // Ensure any existing beacon spam is stopped
    beacon_spam_stop();
    // Notify user that list-based beacon spam is starting
    glog("Starting beacon spam list (%d SSIDs)...\n", g_beacon_list_count);
    
    ap_manager_stop_services();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    configure_hidden_ap();
    esp_wifi_start();
    
    // Launch the beacon list task
    beacon_task_running = true;
    BaseType_t rc = xTaskCreate_psram(beacon_spam_list_task, "beacon_list", 3072, NULL, 5, &beacon_task_handle);
    if (rc != pdPASS) {
        glog("Failed to start beacon list task (%ld)\n", (long)rc);
        status_display_show_status("Beacon Start Failed");
        beacon_task_running = false;
        beacon_task_handle = NULL;
        esp_wifi_stop();
        ap_manager_init();
        return;
    }
    rgb_manager_set_color(&rgb_manager, 0, 255, 0, 0, false);
}

// Add an SSID to the beacon list
void beacon_spam_add_ssid(const char *ssid) {
    if (g_beacon_list == NULL) {
        g_beacon_list = calloc(BEACON_LIST_MAX, sizeof(*g_beacon_list));
        if (g_beacon_list == NULL) {
            printf("Unable to allocate beacon list\n");
            return;
        }
    }
    if (g_beacon_list_count >= BEACON_LIST_MAX) {
        printf("Beacon list full\n");
        return;
    }
    if (strlen(ssid) > BEACON_SSID_MAX_LEN) {
        printf("SSID too long\n");
        return;
    }
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            printf("SSID already in list: %s\n", ssid);
            return;
        }
    }
    strcpy(g_beacon_list[g_beacon_list_count++], ssid);
    printf("Added SSID to beacon list: %s\n", ssid);
}

// Remove an SSID from the beacon list
void beacon_spam_remove_ssid(const char *ssid) {
    for (int i = 0; i < g_beacon_list_count; ++i) {
        if (strcmp(g_beacon_list[i], ssid) == 0) {
            for (int j = i; j < g_beacon_list_count - 1; ++j) {
                strcpy(g_beacon_list[j], g_beacon_list[j + 1]);
            }
            --g_beacon_list_count;
            printf("Removed SSID from beacon list: %s\n", ssid);
            return;
        }
    }
    printf("SSID not found in list: %s\n", ssid);
}

// Clear the beacon list
void beacon_spam_clear_list(void) {
    free(g_beacon_list);
    g_beacon_list = NULL;
    g_beacon_list_count = 0;
    printf("Cleared beacon list\n");
}

// Show the beacon list
void beacon_spam_show_list(void) {
    printf("Beacon list (%d entries):\n", g_beacon_list_count);
    for (int i = 0; i < g_beacon_list_count; ++i) {
        printf("  %d: %s\n", i, g_beacon_list[i]);
    }
}

// Check if beacon spam is running
bool beacon_spam_is_running(void) {
    return beacon_task_running;
}
