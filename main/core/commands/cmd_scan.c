// cmd_scan.c
// WiFi/BLE scan-all and sweep commands.

#include "core/callbacks.h"
#include "core/commands.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "scans/ble/flipper_scan.h"
#include "attacks/wifi/dhcp_starvation.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/ap_manager.h"
#include "managers/ble_manager.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "managers/zigbee_manager.h"
#include "sdkconfig.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void wifi_manager_scanall_chart();

// Sweep async state
static sweep_result_t g_sweep_result = {0};
static int g_sweep_wifi_seconds = 10;
static int g_sweep_ble_seconds = 10;

static int get_next_sweep_file_index(void);
static const char* sweep_get_auth_str(wifi_auth_mode_t auth);
static const char* sweep_get_cipher_str(wifi_cipher_type_t cipher);
static void sweep_get_phy_modes(wifi_ap_record_t *ap, char *buf, size_t len);
static void sweep_write_csv_escaped(FILE *f, const char *str);

static void sweep_refresh_timestamp(char *out, size_t out_len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info && tm_info->tm_year >= 120) {
        strftime(out, out_len, "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        out[0] = '\0';
    }
}

void sweep_clear_result(void) {
    memset(&g_sweep_result, 0, sizeof(g_sweep_result));
}

const sweep_result_t* sweep_get_result(void) {
    return &g_sweep_result;
}

static void sweep_run_internal(void) {
    int wifi_seconds = g_sweep_wifi_seconds;
    int ble_seconds = g_sweep_ble_seconds;

    glog("=== Starting Full Environment Sweep ===\n");
    status_display_show_status("Sweep Start");

    FILE *report = NULL;
    char report_path[64] = {0};
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[32] = "";
    if (tm_info && tm_info->tm_year >= 120) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    int open_networks = 0, weak_networks = 0, secure_networks = 0;

    if (sd_card_exists(SD_GHOSTESP_ROOT)) {
        (void)sd_card_create_directory(SD_DIR_SWEEPS);
        int idx = get_next_sweep_file_index();
        snprintf(report_path, sizeof(report_path), SD_DIR_SWEEPS "/sweep_%d.csv", idx);
        report = fopen(report_path, "w");
        if (report) {
            fprintf(report, "Type,Name,MAC,Associated MAC,Channel,Frequency,RSSI,Auth,Cipher,802.11,WPS,Latitude,Longitude,Altitude,First Seen\n");
            glog("Saving report to: %s\n", report_path);
        }
    }

    // --- WiFi AP Scan ---
    g_sweep_result.current_phase = 1;
    glog("\n--- Phase 1: WiFi AP Scan (%ds) ---\n", wifi_seconds);

    wifi_manager_start_scan_with_time(wifi_seconds);

    uint16_t ap_cnt = 0;
    wifi_ap_record_t *aps = NULL;
    wifi_manager_get_scan_results_data(&ap_cnt, &aps);
    g_sweep_result.ap_count = ap_cnt;

    glog("Found %d access points\n", ap_cnt);

    uint16_t limit = ap_cnt > 100 ? 100 : ap_cnt;
    for (uint16_t i = 0; i < limit && aps; i++) {
        char ssid_safe[33];
        strncpy(ssid_safe, (char*)aps[i].ssid, 32);
        ssid_safe[32] = '\0';
        for (int j = 0; ssid_safe[j]; j++) {
            if (ssid_safe[j] < 32 || ssid_safe[j] > 126) ssid_safe[j] = '?';
        }
        if (ssid_safe[0] == '\0') strcpy(ssid_safe, "");

        const char *auth = sweep_get_auth_str(aps[i].authmode);
        const char *cipher = sweep_get_cipher_str(aps[i].pairwise_cipher);
        char phy_modes[24];
        sweep_get_phy_modes(&aps[i], phy_modes, sizeof(phy_modes));
        int freq = aps[i].primary > 14 ? 5000 + (aps[i].primary * 5) : 2407 + (aps[i].primary * 5);

        if (aps[i].authmode == WIFI_AUTH_OPEN) open_networks++;
        else if (aps[i].authmode == WIFI_AUTH_WEP || aps[i].authmode == WIFI_AUTH_WPA_PSK) weak_networks++;
        else secure_networks++;

        if (report) {
            sweep_refresh_timestamp(timestamp, sizeof(timestamp));
            fprintf(report, "WiFi AP,");
            sweep_write_csv_escaped(report, ssid_safe);
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,%d,%d,%d,%s,%s,%s,%s,",
                    aps[i].bssid[0], aps[i].bssid[1], aps[i].bssid[2],
                    aps[i].bssid[3], aps[i].bssid[4], aps[i].bssid[5],
                    aps[i].primary, freq, aps[i].rssi, auth, cipher, phy_modes,
                    aps[i].wps ? "Yes" : "No");
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }

    // --- WiFi Station Scan ---
    g_sweep_result.current_phase = 2;
    glog("\n--- Phase 2: WiFi Station Scan (%ds) ---\n", wifi_seconds);

    station_count = 0;
    wifi_manager_start_station_scan();
    vTaskDelay(pdMS_TO_TICKS(wifi_seconds * 1000));
    wifi_manager_stop_monitor_mode();
    g_sweep_result.station_count = station_count;

    glog("Found %d stations\n", station_count);

    for (int i = 0; i < station_count; i++) {
        if (report) {
            sweep_refresh_timestamp(timestamp, sizeof(timestamp));
            fprintf(report, "WiFi Client,,%02X:%02X:%02X:%02X:%02X:%02X,%02X:%02X:%02X:%02X:%02X:%02X,,,,,,,",
                    station_ap_list[i].station_mac[0], station_ap_list[i].station_mac[1],
                    station_ap_list[i].station_mac[2], station_ap_list[i].station_mac[3],
                    station_ap_list[i].station_mac[4], station_ap_list[i].station_mac[5],
                    station_ap_list[i].ap_bssid[0], station_ap_list[i].ap_bssid[1],
                    station_ap_list[i].ap_bssid[2], station_ap_list[i].ap_bssid[3],
                    station_ap_list[i].ap_bssid[4], station_ap_list[i].ap_bssid[5]);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    // --- BLE Scans ---
    g_sweep_result.current_phase = 3;
    glog("\n--- Phase 3: BLE Flipper Scan (%ds) ---\n", ble_seconds);

    flipper_scan_start();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    flipper_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(500));

    flipper_scan_print_results();

    int flipper_cnt = flipper_scan_get_count();
    g_sweep_result.flipper_count = flipper_cnt;
    for (int i = 0; i < flipper_cnt && report; i++) {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (flipper_scan_get_device_data(i, mac, &rssi, name, sizeof(name)) == 0) {
            sweep_refresh_timestamp(timestamp, sizeof(timestamp));
            fprintf(report, "Flipper,");
            sweep_write_csv_escaped(report, name[0] ? name : "");
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,,,%d,,,,",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }

    g_sweep_result.current_phase = 4;
    glog("\n--- Phase 4: BLE GATT Device Scan (%ds) ---\n", ble_seconds);

    ble_start_gatt_scan();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    ble_stop_gatt_scan();
    vTaskDelay(pdMS_TO_TICKS(500));

    ble_list_gatt_devices();

    int gatt_cnt = ble_get_gatt_device_count();
    g_sweep_result.gatt_count = gatt_cnt;
    for (int i = 0; i < gatt_cnt && report; i++) {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (ble_get_gatt_device_data(i, mac, &rssi, name, sizeof(name)) == 0) {
            sweep_refresh_timestamp(timestamp, sizeof(timestamp));
            fprintf(report, "BLE Device,");
            sweep_write_csv_escaped(report, name[0] ? name : "");
            fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X,,,,%d,,,,",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi);
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }

    g_sweep_result.current_phase = 5;
    glog("\n--- Phase 5: BLE Raw Packet Scan (%ds) ---\n", ble_seconds);
    vTaskDelay(pdMS_TO_TICKS(500));

    ble_start_raw_ble_packetscan();
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    ble_stop();
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    g_sweep_result.current_phase = 6;
    glog("\n--- Phase 6: 802.15.4 Scan (%ds) ---\n", ble_seconds);
    vTaskDelay(pdMS_TO_TICKS(500));

    zigbee_manager_clear_devices();
    zigbee_manager_start_capture(0);
    vTaskDelay(pdMS_TO_TICKS(ble_seconds * 1000));
    zigbee_manager_stop_capture();

    int zb_cnt = zigbee_manager_get_device_count();
    g_sweep_result.zigbee_count = zb_cnt;
    glog("Found %d 802.15.4 devices\n", zb_cnt);

    for (int i = 0; i < zb_cnt && report; i++) {
        zigbee_device_t dev;
        if (zigbee_manager_get_device_data(i, &dev) == 0) {
            sweep_refresh_timestamp(timestamp, sizeof(timestamp));
            fprintf(report, "802.15.4,");
            if (dev.addr_len == 8) {
                fprintf(report, ",%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X,,%d,,%d,,,,",
                        dev.addr[0], dev.addr[1], dev.addr[2], dev.addr[3],
                        dev.addr[4], dev.addr[5], dev.addr[6], dev.addr[7],
                        dev.channel, dev.rssi);
            } else {
                fprintf(report, ",%02X:%02X,,%d,,%d,,,,",
                        dev.addr[0], dev.addr[1], dev.channel, dev.rssi);
            }
            if (gps && gps->valid) {
                fprintf(report, "%.6f,%.6f,%.1f,%s\n", gps->latitude, gps->longitude, gps->altitude, timestamp);
            } else {
                fprintf(report, ",,,%s\n", timestamp);
            }
        }
    }
#endif

    if (report) {
        fclose(report);
        glog("\nReport saved to: %s\n", report_path);
    }

    (void)ap_manager_restore_after_attack("sweep");
    glog("\n=== Sweep Complete ===\n");
    glog("WiFi: %d APs, %d stations | Security: %d open, %d weak, %d secure\n",
         ap_cnt, station_count, open_networks, weak_networks, secure_networks);
    status_display_show_status("Sweep Done");
    g_sweep_result.done = true;
}

static void sweep_task(void *pvParameters) {
    (void)pvParameters;
    sweep_run_internal();
    vTaskDeleteWithCaps(NULL);
}

void sweep_start_async(int wifi_seconds, int ble_seconds) {
    if (g_sweep_result.running) return;
    sweep_clear_result();
    g_sweep_wifi_seconds = wifi_seconds < 1 ? 10 : wifi_seconds;
    g_sweep_ble_seconds = ble_seconds < 1 ? 10 : ble_seconds;
    g_sweep_result.running = true;
    g_sweep_result.total_phases = 6;
    xTaskCreate_psram(sweep_task, "sweep", 8192, NULL, 5, NULL);
}

bool sweep_check_done(void) {
    return g_sweep_result.done;
}

void sweep_finish_async(void) {
    g_sweep_result.running = false;
}

bool sweep_is_running(void) {
    return g_sweep_result.running && !g_sweep_result.done;
}

void handle_scanall(int argc, char **argv) {
    int total_seconds = 10; // Default total duration: 10 seconds
    if (argc > 1) {
        char *endptr;
        long sec = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && sec > 0) {
            total_seconds = (int)sec;
        } else {
            glog("Invalid duration: '%s'. Using default %d seconds.\n", argv[1], total_seconds);
            status_display_show_status("ScanAll Usage");
        }
    }

    int ap_scan_seconds = total_seconds / 2;
    int sta_scan_seconds = total_seconds - ap_scan_seconds; // Use remaining time

    glog("Starting combined scan (%d sec AP, %d sec STA)...\n", ap_scan_seconds, sta_scan_seconds);
    status_display_show_status("ScanAll Start");

    // 1. Perform AP Scan
    glog("--- Starting AP Scan (%d seconds) ---\n", ap_scan_seconds);
    wifi_manager_start_scan_with_time(ap_scan_seconds);
    // Results are now in scanned_aps and ap_count

    // 2. Perform Station Scan
    glog("--- Starting Station Scan (%d seconds) ---\n", sta_scan_seconds);
    station_count = 0; // Reset station list before new scan
    wifi_manager_start_station_scan(); // Starts monitor mode + channel hopping
    glog("Station scan running for %d seconds...\n", sta_scan_seconds);
    vTaskDelay(pdMS_TO_TICKS(sta_scan_seconds * 1000));
    wifi_manager_stop_monitor_mode(); // Stops monitor mode + channel hopping
    // Results are now in station_ap_list and station_count

    glog("--- Scan Complete ---\n");

    // 3. Print Combined Results
    wifi_manager_scanall_chart();

    (void)ap_manager_restore_after_attack("scanall");
    status_display_show_status("ScanAll Done");
}

static int get_next_sweep_file_index(void) {
    int next = 0;
    char path[64];
    while (next < 9999) {
        snprintf(path, sizeof(path), SD_DIR_SWEEPS "/sweep_%d.csv", next);
        FILE *f = fopen(path, "r");
        if (!f) break;
        fclose(f);
        next++;
    }
    return next;
}

static const char* sweep_get_auth_str(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Unknown";
    }
}

static const char* sweep_get_cipher_str(wifi_cipher_type_t cipher) {
    switch (cipher) {
        case WIFI_CIPHER_TYPE_NONE: return "None";
        case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
        case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
        case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
        case WIFI_CIPHER_TYPE_CCMP: return "CCMP";
        case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP/CCMP";
        case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
        case WIFI_CIPHER_TYPE_GCMP256: return "GCMP256";
        default: return "Unknown";
    }
}

static void sweep_get_phy_modes(wifi_ap_record_t *ap, char *buf, size_t len) {
    buf[0] = '\0';
    if (ap->phy_11ax) strlcat(buf, "ax/", len);
    if (ap->phy_11ac) strlcat(buf, "ac/", len);
    if (ap->phy_11n) strlcat(buf, "n/", len);
    if (ap->phy_11a) strlcat(buf, "a/", len);
    if (ap->phy_11g) strlcat(buf, "g/", len);
    if (ap->phy_11b) strlcat(buf, "b/", len);
    size_t l = strlen(buf);
    if (l > 0) buf[l - 1] = '\0';
}

static void sweep_write_csv_escaped(FILE *f, const char *str) {
    bool needs_quote = false;
    for (const char *p = str; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n') { needs_quote = true; break; }
    }
    if (needs_quote) {
        fputc('"', f);
        for (const char *p = str; *p; p++) {
            if (*p == '"') fputc('"', f);
            fputc(*p, f);
        }
        fputc('"', f);
    } else {
        fputs(str, f);
    }
}

void handle_sweep_cmd(int argc, char **argv) {
    int wifi_seconds = 10;
    int ble_seconds = 10;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            wifi_seconds = atoi(argv[++i]);
            if (wifi_seconds < 1) wifi_seconds = 10;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            ble_seconds = atoi(argv[++i]);
            if (ble_seconds < 1) ble_seconds = 10;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "help") == 0) {
            glog("Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
            glog("  -w: WiFi scan duration per phase (default 10s)\n");
            glog("  -b: BLE scan duration per phase (default 10s)\n");
            glog("Performs AP scan, STA scan, BLE scans and saves to SD.\n");
            return;
        }
    }

    sweep_start_async(wifi_seconds, ble_seconds);
}

void handle_dhcpstarve_cmd(int argc, char **argv) {
    if (argc < 2) {
        dhcp_starvation_help();
        status_display_show_status("DHCP Usage");
    } else if (strcmp(argv[1], "start") == 0) {
        int thr = (argc >= 3) ? atoi(argv[2]) : 1;
        dhcp_starvation_start(thr);
        status_display_show_status("DHCP Start");
    } else if (strcmp(argv[1], "stop") == 0) {
        dhcp_starvation_stop();
        status_display_show_status("DHCP Stop");
    } else if (strcmp(argv[1], "display") == 0) {
        dhcp_starvation_display();
        status_display_show_status("DHCP Stats");
    } else {
        dhcp_starvation_help();
        status_display_show_status("DHCP Usage");
    }
}
