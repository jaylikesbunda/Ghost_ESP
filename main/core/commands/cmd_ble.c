// cmd_ble.c
// BLE, AirTag, Flipper, GATT, Chameleon, and BLE spam commands.

#include "core/commands.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/ouis.h"
#include "core/esp_comm_manager.h"
#include "managers/chameleon_manager.h"
#include "managers/gps_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#include "managers/ble_manager.h"
#include "attacks/ble/ble_spam.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/device_detect_scan.h"
#include "scans/ble/flipper_scan.h"
#include "host/ble_gap.h"
#endif

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

typedef struct {
    int last_percent;
    int last_total;
} chameleon_cli_progress_state_t;

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

static void chameleon_cli_progress_cb(int current, int total, void *user) {
    chameleon_cli_progress_state_t *state = (chameleon_cli_progress_state_t *)user;
    if (!state || total <= 0) return;
    if (current < 0) current = 0;
    if (current > total) current = total;
    if (total != state->last_total) state->last_percent = -1;
    int percent = (int)((current * 100) / total);
    if (percent != state->last_percent) {
        glog("Classic dictionary progress: %d%% (%d/%d)\n", percent, current, total);
        state->last_percent = percent;
        state->last_total = total;
    }
}

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        glog("Starting Find the Flippers.\n");
        flipper_scan_start();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-ds") == 0) {
        glog("Starting BLE Spam Detector.\n");
        ble_start_blespam_detector();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        glog("Starting AirTag Scanner.\n");
        ble_start_airtag_scanner();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        glog("Scanning for Raw Packets\n");
        ble_start_raw_ble_packetscan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-g") == 0) {
        glog("Starting GATT Device Scan.\n");
        ble_start_gatt_scan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-adv") == 0) {
        glog("Starting BLE Advertiser Scan.\n");
        advertiser_scan_start();
        return;
    }

    if (argc > 2 && strcmp(argv[1], "-oui") == 0) {
        uint8_t oui[3];
        if (!ouis_parse_prefix(argv[2], oui)) {
            glog("Invalid OUI prefix. Use 6 hex digits, e.g. 00:1A:2B.\n");
            return;
        }
        glog("Starting BLE OUI scan for %02X:%02X:%02X.\n", oui[0], oui[1], oui[2]);
        advertiser_scan_start_oui_prefix(oui);
        return;
    }

    if (argc > 2 && strcmp(argv[1], "-vendor") == 0) {
        glog("Starting BLE OUI vendor scan for %s.\n", argv[2]);
        advertiser_scan_start_vendor(argv[2]);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping BLE Scan.\n");
        bool detect_active = ble_device_detect_is_active();
        if (detect_active) {
            ble_device_detect_stop();
        }
        bool advertiser_active = advertiser_scan_is_active();
        advertiser_scan_stop();
        if (!advertiser_active && !detect_active) {
            ble_stop();
        }
        ble_stop_gatt_scan();
        return;
    }

    glog("Invalid Command Syntax.\n");
}

static void ble_detect_print_devices(void) {
    int count = ble_device_detect_get_count();
    if (count <= 0) {
        glog("No BLE devices detected. Run 'bledetect' first.\n");
        return;
    }

    glog("--- BLE Devices (%d) ---\n", count);
    for (int i = 0; i < count; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) != 0) {
            continue;
        }

        char variant[32];
        if (info.subtype[0] != '\0') {
            snprintf(variant, sizeof(variant), " (%s)", info.subtype);
        } else {
            variant[0] = '\0';
        }

        char identity[40];
        if (info.name[0] != '\0') {
            snprintf(identity, sizeof(identity), "%s", info.name);
        } else {
            snprintf(identity, sizeof(identity), "%02X:%02X:%02X:%02X:%02X:%02X",
                     info.mac[0], info.mac[1], info.mac[2],
                     info.mac[3], info.mac[4], info.mac[5]);
        }

        glog("  [%d]%s %s%s | %s | %d dBm\n", i, info.tracking ? " *" : "",
             ble_device_detect_type_to_string(info.type), variant, identity, info.rssi);
    }
}

static bool ble_detect_parse_index(const char *text, int *out_index) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0) {
        return false;
    }
    *out_index = (int)value;
    return true;
}

static void ble_detect_print_usage(void) {
    glog("Usage: bledetect [-s|-l|-c|-i|-t <index>|-u|-sp <index>|-h]\n");
    glog("    no argument : Start scanning for trackers, skimmers and beacons\n");
    glog("    -s          : Stop the scan, keeping discovered devices\n");
    glog("    -l          : List discovered devices (index, type, name/MAC, RSSI)\n");
    glog("    -c          : Clear results (scan must be stopped first)\n");
    glog("    -i          : Show scan state, device count and tracking info\n");
    glog("    -t <index>  : Track a device and log its live RSSI\n");
    glog("    -u          : Stop tracking\n");
    glog("    -sp <index> : Spoof a detected AirTag; 'stopspoof' ends it\n");
    glog("    -h          : Show this help\n");
}

void handle_ble_detect_cmd(int argc, char **argv) {
    if (argc < 2) {
        ble_device_detect_start();
        if (!ble_device_detect_is_active()) {
            glog("Failed to start BLE device detect scan.\n");
            status_display_show_status("BLE Detect Fail");
            return;
        }
        glog("BLE device detect started. Use 'bledetect -l' to review results.\n");
        status_display_show_status("BLE Detect On");
        return;
    }

    if (strcmp(argv[1], "-h") == 0) {
        ble_detect_print_usage();
        return;
    }

    if (strcmp(argv[1], "-s") == 0) {
        ble_device_detect_stop();
        glog("BLE device detect stopped. %d device(s) retained.\n",
             ble_device_detect_get_count());
        status_display_show_status("BLE Detect Off");
        return;
    }

    if (strcmp(argv[1], "-l") == 0) {
        ble_detect_print_devices();
        status_display_show_status("List BLE Detect");
        return;
    }

    if (strcmp(argv[1], "-c") == 0) {
        if (ble_device_detect_is_active()) {
            glog("Scan is active. Run 'bledetect -s' before clearing.\n");
            return;
        }
        ble_device_detect_clear_results();
        glog("BLE device detect results cleared.\n");
        return;
    }

    if (strcmp(argv[1], "-i") == 0) {
        bool tracking = ble_device_detect_is_tracking();
        glog("scan_active=%s\n", ble_device_detect_is_active() ? "true" : "false");
        glog("devices=%d\n", ble_device_detect_get_count());
        glog("tracking=%s\n", tracking ? "true" : "false");
        if (tracking) {
            int8_t rssi = 0;
            bool fresh = false;
            if (ble_device_detect_get_track_status(&rssi, &fresh)) {
                glog("track_rssi=%d dBm\n", rssi);
                glog("track_fresh=%s\n", fresh ? "true" : "false");
            }
        }
        return;
    }

    if (strcmp(argv[1], "-u") == 0) {
        ble_device_detect_stop_tracking();
        glog("Stopped BLE device tracking.\n");
        return;
    }

    if (strcmp(argv[1], "-t") == 0 || strcmp(argv[1], "-sp") == 0) {
        int index = 0;
        if (argc != 3 || !ble_detect_parse_index(argv[2], &index)) {
            glog("Usage: bledetect %s <index>\n", argv[1]);
            return;
        }

        if (strcmp(argv[1], "-t") == 0) {
            if (!ble_device_detect_start_tracking(index)) {
                glog("Failed to track device %d. Run 'bledetect -l' first.\n", index);
                status_display_show_status("BLE Track Fail");
                return;
            }
            glog("Tracking device %d. Live RSSI is logged; use 'bledetect -u' to stop.\n",
                 index);
            status_display_show_status("BLE Track On");
            return;
        }

        if (!ble_device_detect_start_airtag_spoof(index)) {
            glog("Failed to spoof device %d. Only detected AirTags can be spoofed.\n", index);
            status_display_show_status("BLE Spoof Fail");
            return;
        }
        glog("Spoofing AirTag %d. Use 'stopspoof' to stop.\n", index);
        status_display_show_status("BLE Spoof On");
        return;
    }

    glog("Unknown bledetect argument '%s'.\n", argv[1]);
    ble_detect_print_usage();
}

void handle_ble_wardriving(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        ble_stop();
        if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        gps_manager_deinit(&g_gpsManager);
        gps_manager_set_peer_gps_preferred(false);
        gps_manager_clear_peer_fix();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
        status_display_show_status("BLE Drive Off");
    } else {
        if (csv_file_is_open()) {
            printf("A wardriving CSV session is already active.\n");
            return;
        }
        bool peer_connected = esp_comm_manager_is_connected();
        gps_manager_set_peer_gps_preferred(peer_connected);
        if (!peer_connected) {
            gps_manager_clear_peer_fix();
        }
        if (!peer_connected && !g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        } else if (peer_connected && g_gpsManager.isinitilized) {
            gps_manager_deinit(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            status_display_show_status("CSV Open Fail");
            return;
        }

        ble_start_scanning();
        ble_register_handler(ble_wardriving_callback);
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
        if (peer_connected) {
            printf("BLE wardriving GPS source: peer stream preferred.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: peer stream preferred.\n");
        } else {
            printf("BLE wardriving GPS source: local parser.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: local parser.\n");
        }
        status_display_show_status("BLE Drive On");
    }
}

void handle_list_airtags_cmd(int argc, char **argv) {
    ble_list_airtags();
    status_display_show_status("List AirTags");
}

void handle_select_airtag(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectairtag <number>\n");
        status_display_show_status("AirTag Usage");
        return;
    }

    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_airtag(num);
        status_display_show_status("AirTag Select");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("AirTag Invalid");
    }
}

void handle_spoof_airtag(int argc, char **argv) {
    ble_start_spoofing_selected_airtag();
    status_display_show_status("AirTag Spoof");
}

void handle_stop_spoof(int argc, char **argv) {
    ble_stop_spoofing();
    status_display_show_status("Spoof Stop");
}

void handle_list_flippers_cmd(int argc, char **argv) {
    flipper_scan_print_results();
    status_display_show_status("List Flipper");
}

void handle_select_flipper_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectflipper <index>\n");
        status_display_show_status("Flipper Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        flipper_scan_select(num);
        status_display_show_status("Flipper Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("Flipper Bad");
    }
}

void handle_list_gatt_cmd(int argc, char **argv) {
    ble_list_gatt_devices();
    status_display_show_status("List GATT");
}

void handle_select_gatt_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectgatt <index>\n");
        status_display_show_status("GATT Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_gatt_device(num);
        status_display_show_status("GATT Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("GATT Bad");
    }
}

void handle_enum_gatt_cmd(int argc, char **argv) {
    ble_enumerate_gatt_services();
    status_display_show_status("GATT Enum");
}

void handle_track_gatt_cmd(int argc, char **argv) {
    ble_track_gatt_device();
}

void handle_list_advertisers_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    advertiser_scan_print_devices();
    status_display_show_status("List BLE Adv");
}

void handle_ble_spam_cmd(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-apple") == 0) {
            glog("Starting Apple BLE spam...\n");
            ble_spam_start(BLE_SPAM_APPLE);
            return;
        }
        if (strcmp(argv[1], "-ms") == 0 || strcmp(argv[1], "-microsoft") == 0) {
            glog("Starting Microsoft BLE spam...\n");
            ble_spam_start(BLE_SPAM_MICROSOFT);
            return;
        }
        if (strcmp(argv[1], "-samsung") == 0) {
            glog("Starting Samsung BLE spam...\n");
            ble_spam_start(BLE_SPAM_SAMSUNG);
            return;
        }
        if (strcmp(argv[1], "-google") == 0) {
            glog("Starting Google BLE spam...\n");
            ble_spam_start(BLE_SPAM_GOOGLE);
            return;
        }
        if (strcmp(argv[1], "-random") == 0) {
            glog("Starting Random BLE spam...\n");
            ble_spam_start(BLE_SPAM_RANDOM);
            return;
        }
        if (strcmp(argv[1], "-s") == 0) {
            glog("Stopping BLE spam...\n");
            ble_spam_stop();
            return;
        }
    }
    glog("Usage: blespam [-apple|-ms|-samsung|-google|-random|-s]\n");
}
#endif // CONFIG_IDF_TARGET_ESP32S2

#ifdef CONFIG_NFC_CHAMELEON
void handle_chameleon_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: chameleon <command>\n");
        glog("Commands:\n");
        glog("Connection:\n");
        glog("  connect [timeout] [pin] - Connect to Chameleon Ultra (default timeout: 10s)\n");
        glog("  disconnect        - Disconnect from Chameleon Ultra\n");
        glog("  status           - Check connection status\n");
        glog("Device Info:\n");
        glog("  firmware         - Get firmware version\n");
        glog("  devicemode       - Get current device mode\n");
        glog("  activeslot       - Get active slot number\n");
        glog("  setslot <1-8>    - Set active slot number\n");
        glog("  slotinfo <1-8>   - Get slot information\n");
        glog("  battery          - Get battery information\n");
        glog("Scanning:\n");
        glog("  scanhf           - Scan for HF tags\n");
        glog("  scanlf           - Scan for LF EM410X tags\n");
        glog("  scanlfall        - Scan for all LF tag types\n");
        glog("  scanhidprox      - Scan for HID Prox tags\n");
        glog("MIFARE Classic:\n");
        glog("  mfdetect         - Detect MIFARE Classic support\n");
        glog("  mfprng           - Detect MIFARE Classic PRNG type\n");
        glog("NTAG Cards:\n");
        glog("  ntagdetect       - Detect and identify NTAG card type\n");
        glog("  ntagdump         - Dump complete NTAG card data\n");
        glog("  saventag [filename] - Save NTAG dump to SD card\n");
        glog("Mode Control:\n");
        glog("  reader           - Set to reader mode\n");
        glog("  emulator         - Set to emulator mode\n");
        glog("Data Management:\n");
        glog("  savehf [filename] - Save last HF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        glog("  savelf [filename] - Save last LF scan to SD card (/mnt/ghostesp/chameleon/)\n");
        glog("  readhf           - Basic MIFARE Classic card detection and information collection\n");
        glog("  savedump [filename] - Save last card dump to SD card\n");
        return;
    }

    const char *subcommand = argv[1];

    if (strcmp(subcommand, "connect") == 0) {
        uint32_t timeout = 10; // Default timeout of 10 seconds
        const char* pin = NULL;
        
        // Parse arguments: connect [timeout] [pin]
        if (argc > 2) {
            // Check if second argument is a number (timeout) or PIN
            if (strlen(argv[2]) <= 2 && atoi(argv[2]) > 0) {
                // Second argument is timeout
                timeout = (uint32_t)atoi(argv[2]);
                if (timeout == 0) {
                    timeout = 10;
                }
                // Check for PIN as third argument
                if (argc > 3) {
                    pin = argv[3];
                }
            } else {
                // Second argument is PIN, use default timeout
                pin = argv[2];
            }
        }
        
        if (pin != NULL) {
            printf("Connecting to Chameleon Ultra with %lu second timeout and PIN...\n", (unsigned long)timeout);
            TERMINAL_VIEW_ADD_TEXT("Connecting to Chameleon Ultra with PIN...\n");
        } else {
            printf("Connecting to Chameleon Ultra with %lu second timeout...\n", (unsigned long)timeout);
            TERMINAL_VIEW_ADD_TEXT("Connecting to Chameleon Ultra...\n");
        }
        
        chameleon_manager_connect(timeout, pin);
    }
    else if (strcmp(subcommand, "disconnect") == 0) {
        printf("Disconnecting from Chameleon Ultra...\n");
        TERMINAL_VIEW_ADD_TEXT("Disconnecting from Chameleon Ultra...\n");
        chameleon_manager_disconnect();
    }
    else if (strcmp(subcommand, "status") == 0) {
        if (chameleon_manager_is_connected()) {
            printf("Status: Connected to Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Status: Connected to Chameleon Ultra\n");
        } else {
            printf("Status: Not connected to Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Status: Not connected to Chameleon Ultra\n");
        }
    }
    else if (strcmp(subcommand, "scanhf") == 0) {
        bool skip_dict = false;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--skip-dict") == 0 || strcmp(argv[i], "--skipdict") == 0) {
                skip_dict = true;
            } else {
                printf("Unknown option for scanhf: %s\n", argv[i]);
                TERMINAL_VIEW_ADD_TEXT("Unknown option for scanhf\n");
                return;
            }
        }

        if (!chameleon_manager_scan_hf()) {
            return;
        }

        bool classic_tag = false;
        uint8_t uid_len = 0;
        uint16_t atqa = 0;
        uint8_t sak = 0;
        if (chameleon_manager_get_last_hf_scan(NULL, &uid_len, &atqa, &sak)) {
            if (sak == 0x08 || sak == 0x09 || sak == 0x18) {
                classic_tag = true;
            }
        }

        if (classic_tag) {
            chameleon_cli_progress_state_t progress_state = {0};
            progress_state.last_percent = -1;
            chameleon_manager_set_progress_callback(chameleon_cli_progress_cb, &progress_state);

            if (skip_dict) {
                glog("Reading MIFARE Classic without dictionary brute-force...\n");
                glog("Dictionary brute-force skipped by user flag.\n");
            } else {
                glog("Reading MIFARE Classic with dictionary brute-force...\n");
            }

            bool classic_ok = chameleon_manager_mf1_read_classic_with_dict(skip_dict);
            chameleon_manager_set_progress_callback(NULL, NULL);

            if (!classic_ok) {
                glog("MIFARE Classic read failed.\n");
            } else {
                glog("MIFARE Classic read complete.\n");
            }
        } else if (chameleon_manager_last_scan_is_ntag()) {
            glog("Refreshing NTAG cache...\n");

            if (!chameleon_manager_read_ntag_card()) {
                glog("Failed to read NTAG card.\n");
            } else {
                glog("NTAG read complete.\n");
            }
        }

        const char *details = chameleon_manager_get_cached_details();
        if (details && details[0]) {
            glog("%s\n", details);
        }
    }
    else if (strcmp(subcommand, "scanlf") == 0) {
        chameleon_manager_scan_lf();
    }
    else if (strcmp(subcommand, "scanlfall") == 0) {
        // Try multiple LF scan types
        printf("Scanning for all LF tag types...\n");
        TERMINAL_VIEW_ADD_TEXT("Scanning for all LF tag types...\n");
        
        // First try EM410X
        printf("1. Trying EM410X scan...\n");
        TERMINAL_VIEW_ADD_TEXT("1. Trying EM410X scan...\n");
        if (chameleon_manager_scan_lf()) {
            return;  // Found something, stop here
        }
        
        // Then try HID Prox
        printf("2. Trying HID Prox scan...\n");
        TERMINAL_VIEW_ADD_TEXT("2. Trying HID Prox scan...\n");
        chameleon_manager_scan_hidprox();
    }
    else if (strcmp(subcommand, "battery") == 0) {
        chameleon_manager_get_battery_info();
    }
    else if (strcmp(subcommand, "reader") == 0) {
        chameleon_manager_set_reader_mode();
    }
    else if (strcmp(subcommand, "emulator") == 0) {
        chameleon_manager_set_emulator_mode();
    }
    else if (strcmp(subcommand, "savehf") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_last_hf_scan(filename);
    }
    else if (strcmp(subcommand, "savelf") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_last_lf_scan(filename);
    }
    else if (strcmp(subcommand, "readhf") == 0) {
        chameleon_manager_read_hf_card();
    }
    else if (strcmp(subcommand, "savedump") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_card_dump(filename);
    }
    else if (strcmp(subcommand, "firmware") == 0) {
        chameleon_manager_get_firmware_version();
    }
    else if (strcmp(subcommand, "devicemode") == 0) {
        chameleon_manager_get_device_mode();
    }
    else if (strcmp(subcommand, "activeslot") == 0) {
        chameleon_manager_get_active_slot();
    }
    else if (strcmp(subcommand, "setslot") == 0) {
        if (argc < 3) {
            printf("Usage: chameleon setslot <1-8>\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: chameleon setslot <1-8>\n");
            return;
        }
        uint8_t user_slot = (uint8_t)atoi(argv[2]);
        if (user_slot < 1 || user_slot > 8) {
            printf("Error: Slot must be between 1-8\n");
            TERMINAL_VIEW_ADD_TEXT("Error: Slot must be between 1-8\n");
            return;
        }
        uint8_t device_slot = user_slot - 1; // Convert 1-8 to 0-7
        chameleon_manager_set_active_slot(device_slot);
    }
    else if (strcmp(subcommand, "slotinfo") == 0) {
        if (argc < 3) {
            printf("Usage: chameleon slotinfo <1-8>\n");
            TERMINAL_VIEW_ADD_TEXT("Usage: chameleon slotinfo <1-8>\n");
            return;
        }
        uint8_t user_slot = (uint8_t)atoi(argv[2]);
        if (user_slot < 1 || user_slot > 8) {
            printf("Error: Slot must be between 1-8\n");
            TERMINAL_VIEW_ADD_TEXT("Error: Slot must be between 1-8\n");
            return;
        }
        uint8_t device_slot = user_slot - 1; // Convert 1-8 to 0-7
        chameleon_manager_get_slot_info(device_slot);
    }
    else if (strcmp(subcommand, "scanhidprox") == 0) {
        chameleon_manager_scan_hidprox();
    }
    else if (strcmp(subcommand, "mfdetect") == 0) {
        chameleon_manager_mf1_detect_support();
    }
    else if (strcmp(subcommand, "mfprng") == 0) {
        chameleon_manager_mf1_detect_prng();
    }
    else if (strcmp(subcommand, "ntagdetect") == 0) {
        chameleon_manager_detect_ntag();
    }
    else if (strcmp(subcommand, "ntagdump") == 0) {
        chameleon_manager_read_ntag_card();
    }
    else if (strcmp(subcommand, "saventag") == 0) {
        const char* filename = (argc > 2) ? argv[2] : NULL;
        chameleon_manager_save_ntag_dump(filename);
    }
    else {
        printf("Unknown chameleon command: %s\n", subcommand);
        TERMINAL_VIEW_ADD_TEXT("Unknown chameleon command: %s\n", subcommand);
        printf("Use 'chameleon' without arguments to see available commands.\n");
        TERMINAL_VIEW_ADD_TEXT("Use 'chameleon' without arguments to see available commands.\n");
    }
}
#else
void handle_chameleon_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("Chameleon support is disabled in this build.\n");
    TERMINAL_VIEW_ADD_TEXT("Chameleon support is disabled in this build.\n");
}
#endif
