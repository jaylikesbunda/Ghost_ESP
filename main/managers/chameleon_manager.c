/**
 * @file chameleon_manager.c
 * @brief Manager for Chameleon Ultra BLE communication using BLE manager
 */

#include "managers/chameleon_manager.h"
#include "managers/nfc/mifare_attack.h"
#include "managers/ble_manager.h"
#include "managers/ghostchi_manager.h"

#ifdef CONFIG_NFC_CHAMELEON
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_sm.h"
#endif
#include "esp_log.h"
#include "esp_err.h"
#include <esp_wifi.h>
#include "managers/wifi_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "managers/views/terminal_screen.h"
#include "managers/sd_card_manager.h"
#include "managers/display_manager.h"
#include "managers/ap_manager.h"
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/mifare_classic.h"
#include "managers/nfc/ndef.h"
#include "managers/nfc/flipper_nfc_compat.h"
#include "managers/nfc/desfire.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include "esp_heap_caps.h"
#include "core/glog.h"

static const char *TAG = "ChameleonManager";

#ifdef CONFIG_NFC_CHAMELEON

static const char *cu_mfc_type_str(MFC_TYPE t) {
    switch (t) {
        case MFC_MINI: return "Mifare Classic Mini";
        case MFC_1K:   return "Mifare Classic 1K";
        case MFC_4K:   return "Mifare Classic 4K";
        default:       return "Mifare Classic";
    }
}

#ifdef CONFIG_HAS_NFC
extern const uint8_t _binary_mf_classic_dict_nfc_start[] asm("_binary_mf_classic_dict_nfc_start");
extern const uint8_t _binary_mf_classic_dict_nfc_end[]   asm("_binary_mf_classic_dict_nfc_end");
#endif

// ui hooks from nfc view (weak)
extern void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) __attribute__((weak));
extern void mfc_ui_set_cache_mode(bool on) __attribute__((weak));

static const mfc_attack_hooks_t *g_cu_attack_hooks = NULL;

static inline void cu_call_on_phase(int sector, int first_block, bool key_b, int total_keys) {
    if (g_cu_attack_hooks && g_cu_attack_hooks->on_phase) g_cu_attack_hooks->on_phase(sector, first_block, key_b, total_keys);
}
static inline void cu_call_on_cache_mode(bool on) {
    if (g_cu_attack_hooks && g_cu_attack_hooks->on_cache_mode) g_cu_attack_hooks->on_cache_mode(on);
}
static inline bool cu_call_should_cancel(void) {
    return (g_cu_attack_hooks && g_cu_attack_hooks->should_cancel) ? g_cu_attack_hooks->should_cancel() : false;
}
static inline bool cu_call_should_skip_dict(void) {
    return (g_cu_attack_hooks && g_cu_attack_hooks->should_skip_dict) ? g_cu_attack_hooks->should_skip_dict() : false;
}

// Global state
static bool g_is_initialized = false;
static bool g_device_found = false;
static bool g_is_connected = false;
static bool g_scanning = false;
static struct ble_gap_disc_desc g_discovered_device;

static bool g_cu_backdoor_checked = false;
static bool g_cu_backdoor_enabled = false;

static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_tx_char_handle = 0;
static uint16_t g_rx_char_handle = 0;
static SemaphoreHandle_t g_scan_sem = NULL;
static SemaphoreHandle_t g_connect_sem = NULL;
static SemaphoreHandle_t g_response_sem = NULL;
static SemaphoreHandle_t g_disconnect_sem = NULL;

// PIN support
char g_chameleon_pin[7] = {0}; // Store PIN as string (max 6 digits + null terminator)
bool g_pin_required = false;

// Track current device HW mode to avoid redundant mode switches
static uint8_t g_cached_hw_mode = 0xFF; // unknown

static bool g_ap_was_running = false;
static bool g_wifi_was_running = false;
static wifi_mode_t g_prev_wifi_mode = WIFI_MODE_NULL;

static bool g_cu_nfc_dir_ready = false;

static void chameleon_suspend_ap(void) {
    bool server_running = false;
    ap_manager_get_status(&server_running, NULL, NULL);
    wifi_mode_t _cur_mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&_cur_mode) == ESP_OK) {
        g_prev_wifi_mode = _cur_mode;
    } else {
        g_prev_wifi_mode = WIFI_MODE_NULL;
    }
    if (server_running) {
        ESP_LOGI(TAG, "Suspending GhostNet AP services for Chameleon");
        printf("Suspending GhostNet AP...\n");
        TERMINAL_VIEW_ADD_TEXT("Suspending GhostNet AP...\n");
        ap_manager_deinit();
        g_ap_was_running = true;
        g_wifi_was_running = false;
    } else {
        g_ap_was_running = false;
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_NULL) {
            ESP_LOGI(TAG, "Suspending Wi-Fi for Chameleon (AP disabled)");
            esp_wifi_stop();
            esp_wifi_deinit();
            g_wifi_was_running = true;
        } else {
            g_wifi_was_running = false;
        }
    }
}

static void chameleon_resume_ap(void) {
    if (g_ap_was_running) {
        ESP_LOGI(TAG, "Restoring GhostNet AP services after Chameleon session");
        printf("Restoring GhostNet AP...\n");
        TERMINAL_VIEW_ADD_TEXT("Restoring GhostNet AP...\n");
        ble_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_err_t err_init = ap_manager_init();
        if (err_init == ESP_OK) {
            wifi_manager_configure_sta_from_settings();
            (void)ap_manager_restore_after_attack("chameleon resume");
        }
        if (err_init != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore GhostNet AP services: 0x%X", (unsigned int)err_init);
        }
        g_ap_was_running = false;
    } else if (g_wifi_was_running) {
        ESP_LOGI(TAG, "Restoring Wi-Fi after Chameleon session");
        ble_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // Re-init just the Wi-Fi driver (netif/event loop already exist)
        esp_err_t err = ap_manager_ensure_wifi_init();
        if (err == ESP_OK) {
            wifi_mode_t m = (g_prev_wifi_mode == WIFI_MODE_NULL) ? WIFI_MODE_STA : g_prev_wifi_mode;
            esp_wifi_set_mode(m);
            esp_wifi_start();
            if ((m & WIFI_MODE_STA) != 0) {
                wifi_manager_configure_sta_from_settings();
            }
        } else {
            ESP_LOGE(TAG, "Failed to reinit Wi-Fi driver: 0x%X", (unsigned int)err);
        }
        g_prev_wifi_mode = WIFI_MODE_NULL;
        g_wifi_was_running = false;
    }
}

// Chameleon Ultra command constants (from official protocol documentation)
// Basic device commands (1000-1099)
#define CMD_GET_APP_VERSION     1000  // 0x03E8 - Get firmware version
#define CMD_CHANGE_DEVICE_MODE  1001  // 0x03E9 - Change device mode
#define CMD_GET_DEVICE_MODE     1002  // 0x03EA - Get current device mode
#define CMD_SET_ACTIVE_SLOT     1003  // 0x03EB - Set active slot
#define CMD_GET_DEVICE_CHIP_ID  1011  // 0x03F3 - Get device chip ID
#define CMD_GET_DEVICE_ADDRESS  1012  // 0x03F4 - Get device BLE address
#define CMD_GET_GIT_VERSION     1017  // 0x03F9 - Get git version info
#define CMD_GET_ACTIVE_SLOT     1018  // 0x03FA - Get active slot
#define CMD_GET_SLOT_INFO       1019  // 0x03FB - Get slot information
#define CMD_GET_BATTERY_INFO    1025  // 0x0401 - Get battery information
#define CMD_GET_DEVICE_MODEL    1033  // 0x0409 - Get device model
#define CMD_GET_DEVICE_SETTINGS 1034  // 0x040A - Get device settings
#define CMD_GET_DEVICE_CAPABILITIES 1035 // 0x040B - Get device capabilities

// HF commands (2000-2999)
#define CMD_HF14A_SCAN          2000  // 0x07D0 - Official HF14A_SCAN command
#define CMD_MF1_DETECT_SUPPORT  2001  // 0x07D1 - Detect MIFARE Classic support
#define CMD_MF1_DETECT_PRNG     2002  // 0x07D2 - Detect PRNG type
#define CMD_MF1_AUTH_ONE_KEY_BLOCK  2007  // 0x07D7 - Authenticate with key for one block
#define CMD_MF1_READ_ONE_BLOCK      2008  // 0x07D8 - Read MIFARE Classic block SUCCESS: OFFICIAL
#define CMD_MF1_WRITE_ONE_BLOCK     2009  // 0x07D9 - Write MIFARE Classic block SUCCESS: OFFICIAL  
#define CMD_HF14A_RAW           2010  // 0x07DA - Send raw HF command
// toolbox helpers
#define CMD_MF1_CHECK_KEYS_ON_BLOCK 2015  // 0x07DF - Check a list of keys on a single block

// LF commands (3000-3999)
#define CMD_EM410X_SCAN         3000  // 0x0BB8 - Official EM410X_SCAN command
#define CMD_HIDPROX_SCAN        3002  // 0x0BBA - HID Prox scan

// Emulation commands (4000-4999) - FOR PROPER RF-BASED KEY RECOVERY
#define CMD_MF1_SET_DETECTION_ENABLE 4004  // 0x0FA4 - Enable MFKey32 detection/logging
#define CMD_MF1_GET_DETECTION_COUNT  4005  // 0x0FA5 - Get detection count
#define CMD_MF1_GET_DETECTION_LOG    4006  // 0x0FA6 - Get detection log (nonces)
#define CMD_MF1_GET_DETECTION_ENABLE 4007  // 0x0FA7 - Get detection enable status
#define CMD_HF14A_SET_ANTI_COLL_DATA 4001  // 0x0FA1 - Set anticollision data (UID, etc.)

// NTAG specific commands (using HF14A_RAW for NTAG operations)
#define NTAG_READ_CMD           0x30  // NTAG READ command
#define NTAG_WRITE_CMD          0xA2  // NTAG WRITE command  
#define NTAG_PWD_AUTH_CMD       0x1B  // NTAG password authentication
#define NTAG_GET_VERSION_CMD    0x60  // NTAG GET_VERSION command

// NTAG card memory sizes and page counts
#define NTAG213_TOTAL_PAGES     45   // 180 bytes total
#define NTAG215_TOTAL_PAGES     135  // 540 bytes total  
#define NTAG216_TOTAL_PAGES     231  // 924 bytes total
#define NTAG_PAGE_SIZE          4    // Each page is 4 bytes

// NTAG memory map constants
#define NTAG_HEADER_PAGES       4    // Pages 0-3: Header (UID, etc.)
#define NTAG_USER_START_PAGE    4    // User data starts at page 4
#define NTAG_CC_PAGE            3    // Capability Container at page 3

// NTAG default passwords removed in simplified version

// Mode constants
#define HW_MODE_READER          0x01
#define HW_MODE_EMULATOR        0x00

// Status codes
#define STATUS_SUCCESS          0x68
#define STATUS_HF_TAG_OK        0x00
#define STATUS_HF_TAG_NO        0x01
#define STATUS_LF_TAG_OK        0x00
#define STATUS_LF_TAG_NO        0x01

// Last scan data storage
typedef struct {
    bool valid;
    uint8_t uid[20];  // Max UID size
    uint8_t uid_size;
    char tag_type[32];
    time_t timestamp;
    uint16_t atqa;
    uint8_t sak;
} last_scan_data_t;

// Full card dump storage
#define MAX_CARD_BLOCKS 256
#define BLOCK_SIZE 16

typedef struct {
    bool valid;
    uint8_t uid[20];
    uint8_t uid_size;
    char tag_type[32];
    time_t timestamp;
    
    // Card data
    uint8_t *blocks;
    bool *block_valid;
    uint16_t total_blocks_read;
    uint16_t card_size_blocks;
    
    // MIFARE Classic specific
    uint16_t atqa;
    uint8_t sak;
} card_dump_data_t;

// NTAG-specific dump structure
typedef struct {
    bool valid;
    uint8_t uid[10];  // NTAG UIDs can be up to 10 bytes
    uint8_t uid_size;
    char card_type[32];  // NTAG213, NTAG215, NTAG216
    time_t timestamp;
    
    // NTAG memory structure
    uint8_t pages[NTAG216_TOTAL_PAGES][NTAG_PAGE_SIZE];  // Use largest possible size
    bool page_valid[NTAG216_TOTAL_PAGES];
    uint16_t total_pages;
    uint16_t readable_pages;
    uint16_t protected_pages;
    
    // NTAG-specific fields
    uint8_t version_data[8];  // GET_VERSION response
    bool version_valid;
    bool password_protected;
    uint32_t password;        // If password authentication succeeded
    bool password_found;
    
    // Memory map information
    uint16_t user_memory_start;  // Usually page 4
    uint16_t user_memory_end;    // Varies by NTAG type
    uint16_t config_pages_start; // Configuration area
    uint16_t lock_bytes_page;    // Lock bytes location
    
    // NDEF information (if present)
    bool ndef_present;
    uint16_t ndef_size;
    uint16_t ndef_start_page;
} ntag_dump_data_t;

static last_scan_data_t g_last_hf_scan = {0};
static last_scan_data_t g_last_lf_scan = {0};
static card_dump_data_t g_last_card_dump = {0};
static ntag_dump_data_t g_last_ntag_dump = {0};

// progress callback for long-running classic ops
static chameleon_progress_cb_t g_progress_cb = NULL;
static void *g_progress_user = NULL;
void chameleon_manager_set_progress_callback(chameleon_progress_cb_t cb, void *user) {
    g_progress_cb = cb;
    g_progress_user = user;
}

// Key types for authentication
#define MF_KEY_A 0x60
#define MF_KEY_B 0x61

// Enhanced card dump with recovered keys
typedef struct {
    uint8_t key[6];
    bool valid;
} sector_key_t;

typedef struct {
    sector_key_t key_a;
    sector_key_t key_b;
    bool auth_success_a;
    bool auth_success_b;
} sector_auth_t;

// Sector authentication tracking (16 sectors for MIFARE Classic 1K)
#define MAX_SECTORS 16
static sector_auth_t g_sector_auth[MAX_SECTORS] = {0};

// classic cache for CU path (separate from PN532 internals)
static struct {
    bool valid;
    MFC_TYPE type;
    uint8_t uid[10];
    uint8_t uid_len;
    uint16_t atqa;
    uint8_t sak;
    int total_blocks;
    uint8_t *blocks;     // total_blocks * 16
    uint8_t *known_bits; // bitset for blocks
    uint8_t *key_a;      // sectors*6
    uint8_t *key_b;      // sectors*6
    uint8_t *key_a_valid;// bitset per sector
    uint8_t *key_b_valid;// bitset per sector
} g_cu_mfc_cache = {0};

static bool cu_details_append(char **buf, size_t *cap, size_t *len, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return false;
    }
    size_t required = *len + (size_t)needed + 1;
    if (required > *cap) {
        size_t new_cap = *cap ? *cap : 256;
        while (required > new_cap) new_cap *= 2;
        char *n = (char*)realloc(*buf, new_cap);
        if (!n) {
            va_end(args);
            return false;
        }
        *buf = n;
        *cap = new_cap;
    }
    vsnprintf(*buf + *len, *cap - *len, fmt, args);
    *len += (size_t)needed;
    va_end(args);
    return true;
}

static void cu_format_key_hex(const uint8_t *key, char out[13]) {
    for (int i = 0; i < 6; ++i) {
        snprintf(out + (i * 2), 3, "%02X", key[i]);
    }
    out[12] = '\0';
}

static void cu_mfc_cache_reset(void) {
    g_cu_mfc_cache.valid = false;
    if (g_cu_mfc_cache.blocks) { free(g_cu_mfc_cache.blocks); g_cu_mfc_cache.blocks = NULL; }
    if (g_cu_mfc_cache.known_bits) { free(g_cu_mfc_cache.known_bits); g_cu_mfc_cache.known_bits = NULL; }
    if (g_cu_mfc_cache.key_a) { free(g_cu_mfc_cache.key_a); g_cu_mfc_cache.key_a = NULL; }
    if (g_cu_mfc_cache.key_b) { free(g_cu_mfc_cache.key_b); g_cu_mfc_cache.key_b = NULL; }
    if (g_cu_mfc_cache.key_a_valid) { free(g_cu_mfc_cache.key_a_valid); g_cu_mfc_cache.key_a_valid = NULL; }
    if (g_cu_mfc_cache.key_b_valid) { free(g_cu_mfc_cache.key_b_valid); g_cu_mfc_cache.key_b_valid = NULL; }
    g_cu_mfc_cache.total_blocks = 0;
    g_cu_mfc_cache.type = MFC_UNKNOWN;
    g_cu_mfc_cache.uid_len = 0;
    g_cu_mfc_cache.atqa = 0;
    g_cu_mfc_cache.sak = 0;
}

static void cu_mfc_cache_begin(MFC_TYPE t, const uint8_t *uid, uint8_t uid_len, uint16_t atqa, uint8_t sak) {
    cu_mfc_cache_reset();
    g_cu_backdoor_checked = false;
    g_cu_backdoor_enabled = false;
    g_cu_mfc_cache.type = t;
    g_cu_mfc_cache.uid_len = uid_len;
    if (uid && uid_len) memcpy(g_cu_mfc_cache.uid, uid, uid_len);
    g_cu_mfc_cache.atqa = atqa;
    g_cu_mfc_cache.sak = sak;
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    int blocks = 0; for (int s = 0; s < sectors; ++s) blocks += mfc_blocks_in_sector(t, s);
    g_cu_mfc_cache.total_blocks = blocks;
    g_cu_mfc_cache.blocks = (uint8_t*)calloc((size_t)blocks, 16);
    int known_bytes = (blocks + 7) >> 3;
    g_cu_mfc_cache.known_bits = (uint8_t*)calloc((size_t)known_bytes, 1);
    g_cu_mfc_cache.key_a = (uint8_t*)calloc((size_t)sectors, 6);
    g_cu_mfc_cache.key_b = (uint8_t*)calloc((size_t)sectors, 6);
    int sec_bits = (sectors + 7) >> 3;
    g_cu_mfc_cache.key_a_valid = (uint8_t*)calloc((size_t)sec_bits, 1);
    g_cu_mfc_cache.key_b_valid = (uint8_t*)calloc((size_t)sec_bits, 1);
    if (!g_cu_mfc_cache.blocks || !g_cu_mfc_cache.known_bits ||
        !g_cu_mfc_cache.key_a || !g_cu_mfc_cache.key_b ||
        !g_cu_mfc_cache.key_a_valid || !g_cu_mfc_cache.key_b_valid) {
        cu_mfc_cache_reset();
    }
}

static inline void cu_bitset_set(uint8_t *arr, int idx) { arr[(unsigned)idx >> 3] |= (uint8_t)(1u << (idx & 7)); }
static inline bool cu_bitset_test(const uint8_t *arr, int idx) { return (arr[(unsigned)idx >> 3] & (uint8_t)(1u << (idx & 7))) != 0; }

static bool cu_parse_key_line(const char* s, const char* e, uint8_t out[6]);

static void cu_mfc_cache_store_block(int abs_block, const uint8_t data[16]) {
    if (!g_cu_mfc_cache.blocks || !g_cu_mfc_cache.known_bits) return;
    if (abs_block < 0 || abs_block >= g_cu_mfc_cache.total_blocks) return;
    memcpy(&g_cu_mfc_cache.blocks[abs_block * 16], data, 16);
    cu_bitset_set(g_cu_mfc_cache.known_bits, abs_block);
}

static void cu_record_working_key(const uint8_t key[6], bool use_key_b);

static void cu_mfc_cache_record_sector_key(int sector, bool usedB, const uint8_t key[6]) {
    int sectors = mfc_sector_count(g_cu_mfc_cache.type); if (sectors == 0) sectors = 16;
    if (sector < 0 || sector >= sectors) return;
    if (usedB) {
        if (!g_cu_mfc_cache.key_b) return;
        memcpy(&g_cu_mfc_cache.key_b[sector * 6], key, 6);
        cu_bitset_set(g_cu_mfc_cache.key_b_valid, sector);
    } else {
        if (!g_cu_mfc_cache.key_a) return;
        memcpy(&g_cu_mfc_cache.key_a[sector * 6], key, 6);
        cu_bitset_set(g_cu_mfc_cache.key_a_valid, sector);
    }
    cu_record_working_key(key, usedB);
}

static void cu_record_working_key(const uint8_t key[6], bool use_key_b) {
    if (!key) return;
    const char *path = "/mnt/ghostesp/nfc/mfc_user_dict.nfc";
    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_card_needs_jit_mount() && !sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK) {
            mounted_here = true;
        } else {
            ESP_LOGW(TAG, "CU dict: skip append, mount failed");
            return;
        }
    }
    if (sd_card_manager.is_initialized && !g_cu_nfc_dir_ready) {
        if (sd_card_create_directory("/mnt/ghostesp/nfc") == ESP_OK) {
            g_cu_nfc_dir_ready = true;
        }
    }

    FILE *f = fopen(path, "a+");
    if (!f) {
        ESP_LOGW(TAG, "CU dict: failed to open %s", path);
        if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
        return;
    }
    bool present = false;
    fseek(f, 0, SEEK_SET);
    char line[96]; uint8_t parsed[6];
    while (fgets(line, sizeof(line), f)) {
        if (cu_parse_key_line(line, line + strlen(line), parsed)) {
            if (memcmp(parsed, key, 6) == 0) {
                present = true;
                break;
            }
        }
    }
    if (!present) {
        fprintf(f, "%02X%02X%02X%02X%02X%02X\n", key[0], key[1], key[2], key[3], key[4], key[5]);
        ESP_LOGI(TAG, "CU dict: added new %c key", use_key_b ? 'B' : 'A');
    }
    fclose(f);
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

static bool cu_mf1_auth_one_block(uint8_t block, bool use_key_b, const uint8_t key[6]);
static bool cu_mf1_read_one_block(uint8_t block, bool use_key_b, const uint8_t key[6], uint8_t out16[16]);
static bool cu_mf1_check_keys_on_block(uint8_t block, bool use_key_b, const uint8_t *keys6, uint8_t keys_len, uint8_t out_key[6]);
static void cu_key_reuse_sweep(MFC_TYPE t, bool use_key_b, const uint8_t key[6], int current_sector);
static bool cu_sector_key_known(int sector, bool key_b);
static void cu_progress(int current, int total);
static int cu_known_key_attempts(MFC_TYPE t, int exclude_sector);
static bool cu_try_known_keys_first(MFC_TYPE t, int target_sector, uint8_t auth_blk,
                                    bool *authed, bool *usedB, const uint8_t **used_key,
                                    int *tried, int *total_attempts);
static bool wait_for_cmd_data(uint16_t cmd, size_t min_len, uint32_t timeout_ms);
static bool cu_send_hf14a_raw(const uint8_t *tx, uint16_t tx_len,
                              bool activate_rf, bool keep_rf, bool auto_select,
                              bool append_crc, bool wait_resp, bool check_crc,
                              uint16_t timeout_ms, uint16_t bitlen_override);
static bool cu_query_battery_status(uint16_t *out_mv, uint8_t *out_percent);
extern bool nfc_is_scan_cancelled(void) __attribute__((weak));
extern bool nfc_is_dict_skip_requested(void) __attribute__((weak));

static uint8_t g_swept_keys_a[32][6];
static uint8_t g_swept_keys_b[32][6];
static int g_swept_a_count = 0;
static int g_swept_b_count = 0;
static bool cu_key_in_list(const uint8_t list[][6], int count, const uint8_t key[6]){
    for (int i = 0; i < count; ++i) { if (memcmp(list[i], key, 6) == 0) return true; }
    return false;
}
static bool cu_sweep_already(bool use_key_b, const uint8_t key[6]){
    return use_key_b ? cu_key_in_list(g_swept_keys_b, g_swept_b_count, key)
                     : cu_key_in_list(g_swept_keys_a, g_swept_a_count, key);
}
static void cu_sweep_mark(bool use_key_b, const uint8_t key[6]){
    if (use_key_b) {
        if (g_swept_b_count < 32 && !cu_key_in_list(g_swept_keys_b, g_swept_b_count, key)) {
            memcpy(g_swept_keys_b[g_swept_b_count++], key, 6);
        }
    } else {
        if (g_swept_a_count < 32 && !cu_key_in_list(g_swept_keys_a, g_swept_a_count, key)) {
            memcpy(g_swept_keys_a[g_swept_a_count++], key, 6);
        }
    }
}
static void cu_sweep_reset(void){ g_swept_a_count = 0; g_swept_b_count = 0; }

static int cu_known_key_attempts(MFC_TYPE t, int exclude_sector) {
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    int count = 0;
    if (g_cu_mfc_cache.key_a_valid) {
        for (int s = 0; s < sectors; ++s) {
            if (s == exclude_sector) continue;
            if (cu_sector_key_known(s, false)) count++;
        }
    }
    if (g_cu_mfc_cache.key_b_valid) {
        for (int s = 0; s < sectors; ++s) {
            if (s == exclude_sector) continue;
            if (cu_sector_key_known(s, true)) count++;
        }
    }
    return count;
}

static bool cu_try_known_keys_first(MFC_TYPE t, int target_sector, uint8_t auth_blk,
                                    bool *authed, bool *usedB, const uint8_t **used_key,
                                    int *tried, int *total_attempts) {
    if (!authed || !usedB || !used_key) return false;
    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    for (int type_idx = 0; type_idx < 2 && !*authed; ++type_idx) {
        bool is_b = (type_idx == 1);
        if (is_b && !g_cu_mfc_cache.key_b_valid) continue;
        if (!is_b && !g_cu_mfc_cache.key_a_valid) continue;
        for (int s = 0; s < sectors && !*authed; ++s) {
            if (cu_call_should_cancel()) return true;
            if (cu_call_should_skip_dict()) return true;
            if (s == target_sector) continue;
            if (!cu_sector_key_known(s, is_b)) continue;
            const uint8_t *kptr = is_b ? &g_cu_mfc_cache.key_b[s * 6] : &g_cu_mfc_cache.key_a[s * 6];
            if (!kptr) continue;
            if (total_attempts && (*total_attempts) <= (tried ? (*tried) : 0)) (*total_attempts)++;
            if (cu_mf1_auth_one_block(auth_blk, is_b, kptr)) {
                *authed = true;
                *usedB = is_b;
                *used_key = kptr;
            }
            if (tried) {
                (*tried)++;
                cu_progress(*tried, (total_attempts && *total_attempts > 0) ? *total_attempts : *tried);
            }
        }
    }
    return false;
}

static void cu_try_find_complementary_user_key(MFC_TYPE t, int sector, uint8_t auth_blk,
                                               bool usedB_cur, const uint8_t *user_keys,
                                               int user_count) {
    if (!user_keys || user_count <= 0) return;
    if (cu_sector_key_known(sector, !usedB_cur)) return;
    for (int i = 0; i < user_count; ++i) {
        if (cu_call_should_cancel() || cu_call_should_skip_dict()) break;
        const uint8_t *key = &user_keys[i * 6];
        if (cu_mf1_auth_one_block(auth_blk, !usedB_cur, key)) {
            cu_mfc_cache_record_sector_key(sector, !usedB_cur, key);
            break;
        }
        if ((i & 0x3F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool cu_sector_key_known(int sector, bool key_b) {
    int sectors = mfc_sector_count(g_cu_mfc_cache.type); if (sectors == 0) sectors = 16;
    if (sector < 0 || sector >= sectors) return false;
    if (key_b) {
        if (!g_cu_mfc_cache.key_b_valid) return false;
        return cu_bitset_test(g_cu_mfc_cache.key_b_valid, sector);
    }
    if (!g_cu_mfc_cache.key_a_valid) return false;
    return cu_bitset_test(g_cu_mfc_cache.key_a_valid, sector);
}

static bool cu_sector_any_key_known(int sector) {
    return cu_sector_key_known(sector, false) || cu_sector_key_known(sector, true);
}

static uint8_t cu_auth_target_block(MFC_TYPE t, int sector) {
    int first = mfc_first_block_of_sector(t, sector);
    int blocks = mfc_blocks_in_sector(t, sector);
    int target = first;
    if (sector == 0) {
        target = first + 1;
    }
    int trailer = first + blocks - 1;
    if (target == trailer) {
        target = (blocks >= 2) ? first : trailer;
        if (sector == 0 && target == first) target = first + 1;
    }
    return (uint8_t)target;
}

static void cu_read_sector_blocks(MFC_TYPE t, int sector) {
    int first = mfc_first_block_of_sector(t, sector);
    int blocks = mfc_blocks_in_sector(t, sector);
    if (blocks <= 0) return;
    uint8_t trailer[16]; bool have_trailer = false;
    /* cache-fill UI handled at higher level to avoid flicker between sectors */
    for (int b = 0; b < blocks; ++b) {

        uint8_t outb[16];
        uint8_t blk = (uint8_t)(first + b);
        // Choose known key for this sector; prefer A if available
        bool haveA = g_cu_mfc_cache.key_a_valid && cu_bitset_test(g_cu_mfc_cache.key_a_valid, sector);
        bool haveB = g_cu_mfc_cache.key_b_valid && cu_bitset_test(g_cu_mfc_cache.key_b_valid, sector);
        const uint8_t *kA = haveA ? &g_cu_mfc_cache.key_a[sector * 6] : NULL;
        const uint8_t *kB = haveB ? &g_cu_mfc_cache.key_b[sector * 6] : NULL;
        bool ok = false;
        if (kA) ok = cu_mf1_read_one_block(blk, false, kA, outb);
        if (!ok && kB) ok = cu_mf1_read_one_block(blk, true, kB, outb);
        if (ok) {
            cu_mfc_cache_store_block(first + b, outb);
            if (b == blocks - 1) { memcpy(trailer, outb, 16); have_trailer = true; }
        }
        if ((b & 0x7) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
    /* cache-fill UI handled at higher level to avoid flicker between sectors */
    if (have_trailer) {
        uint8_t key_a[6]; memcpy(key_a, trailer, 6);
        uint8_t key_b[6]; memcpy(key_b, trailer + 10, 6);
        int verify_blk = cu_auth_target_block(t, sector);
        if (verify_blk >= 0 && verify_blk < g_cu_mfc_cache.total_blocks) {
            if (cu_mf1_auth_one_block((uint8_t)verify_blk, false, key_a)) {
                cu_mfc_cache_record_sector_key(sector, false, key_a);
                cu_key_reuse_sweep(t, false, key_a, sector);
            }
            if (cu_mf1_auth_one_block((uint8_t)verify_blk, true, key_b)) {
                cu_mfc_cache_record_sector_key(sector, true, key_b);
                cu_key_reuse_sweep(t, true, key_b, sector);
            }
        }
    }
}

static void cu_key_reuse_sweep(MFC_TYPE t, bool use_key_b, const uint8_t key[6], int current_sector) {
    if (!key) return;
    if (cu_sweep_already(use_key_b, key)) return;
    cu_sweep_mark(use_key_b, key);

    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    for (int s = 0; s < sectors; ++s) {
        if (cu_call_should_cancel()) break;
        if (s == current_sector) continue;
        if (cu_sector_key_known(s, use_key_b)) continue;
        int auth_blk = cu_auth_target_block(t, s);
        if (cu_mf1_auth_one_block((uint8_t)auth_blk, use_key_b, key)) {
            cu_mfc_cache_record_sector_key(s, use_key_b, key);
            cu_call_on_phase(s, mfc_first_block_of_sector(t, s), use_key_b, 0);
            cu_read_sector_blocks(t, s);
        }

        if ((s & 0x3) == 0) vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static char *g_cached_details_text = NULL;
static uint32_t g_cached_details_session = 0;

// Service and characteristic UUIDs for Chameleon Ultra
// Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
// TX: 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (write to this)
// RX: 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (notifications from this)
static const ble_uuid128_t g_service_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t g_tx_char_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t g_rx_char_uuid = BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

// Response structure
typedef struct {
    uint16_t command;
    uint8_t status;
    uint8_t data_size;
    uint8_t data[200];
} chameleon_response_t;

static chameleon_response_t g_last_response;
static bool g_response_received = false;

static bool cu_send_magic_raw(const uint8_t *cmd, size_t len, bool append_crc, uint16_t timeout_ms) {
    if (!cmd || len == 0) return false;
    g_response_received = false;
    if (!cu_send_hf14a_raw(cmd, (uint16_t)len, true, true, true, append_crc, true, false, timeout_ms, 0)) {
        return false;
    }
    if (!wait_for_cmd_data(CMD_HF14A_RAW, 0, timeout_ms)) {
        return false;
    }
    return (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK);
}

static void cu_try_magic_backdoor_once(void) {
    if (g_cu_backdoor_checked) return;
    g_cu_backdoor_checked = true;
    if (!chameleon_manager_is_ready() || !g_last_hf_scan.valid) return;
    uint8_t halt_cmd[2] = {0x50, 0x00};
    (void)cu_send_magic_raw(halt_cmd, sizeof(halt_cmd), true, 600);
    uint8_t cmd40 = 0x40;
    uint8_t cmd43 = 0x43;
    bool ok40 = cu_send_magic_raw(&cmd40, 1, false, 600);
    bool ok43 = cu_send_magic_raw(&cmd43, 1, false, 600);
    if (ok40 && ok43) {
        g_cu_backdoor_enabled = true;
        ESP_LOGI(TAG, "Magic backdoor likely enabled");
    }
}

// Forward declarations
static void chameleon_ble_scan_callback(struct ble_gap_event *event, size_t len);
static int chameleon_gap_event_handler(struct ble_gap_event *event, void *arg);
static int chameleon_service_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg);
static int chameleon_char_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg);
static int chameleon_notification_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
static bool send_command(uint16_t cmd, uint8_t *data, size_t data_len);
static bool wait_for_cmd_data(uint16_t cmd, size_t min_len, uint32_t timeout_ms);
static uint8_t calculate_lrc(const uint8_t *data, size_t length);
static void start_service_discovery(void);
static int chameleon_sm_io_cb(uint16_t conn_handle, const struct ble_sm_io *io, void *arg);

// build and send a proper hf14a raw frame per protocol (options|timeout|bitlen|data)
static bool cu_send_hf14a_raw(const uint8_t *tx, uint16_t tx_len,
                              bool activate_rf, bool keep_rf, bool auto_select,
                              bool append_crc, bool wait_resp, bool check_crc,
                              uint16_t timeout_ms, uint16_t bitlen_override) {
    if (!tx || tx_len == 0) return false;
    uint8_t buf[5 + 32];
    if ((size_t)(5 + tx_len) > sizeof(buf)) return false;
    uint8_t opts = 0;
    if (activate_rf) opts |= 0x80;
    if (wait_resp)   opts |= 0x40;
    if (append_crc)  opts |= 0x20;
    if (auto_select) opts |= 0x10;
    if (keep_rf)     opts |= 0x08;
    if (check_crc)   opts |= 0x04;
    buf[0] = opts;
    buf[1] = (uint8_t)((timeout_ms >> 8) & 0xFF);
    buf[2] = (uint8_t)(timeout_ms & 0xFF);
    uint16_t bitlen = bitlen_override ? bitlen_override : (uint16_t)(tx_len * 8);
    buf[3] = (uint8_t)((bitlen >> 8) & 0xFF);
    buf[4] = (uint8_t)(bitlen & 0xFF);
    memcpy(&buf[5], tx, tx_len);
    return send_command(CMD_HF14A_RAW, buf, (size_t)(5 + tx_len));
}

bool chameleon_manager_is_ready(void) {
    return g_is_connected && (g_tx_char_handle != 0);
}

static uint8_t calculate_lrc(const uint8_t *data, size_t length) {
    uint8_t lrc = 0;
    for (size_t i = 0; i < length; i++) {
        lrc += data[i];
    }
    lrc = 0x100 - (lrc & 0xff);
    return lrc;
}

static int chameleon_notification_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Check for null pointer
    if (!ctxt || !ctxt->om || !ctxt->om->om_data) {
        ESP_LOGE(TAG, "Invalid notification context");
        return BLE_ATT_ERR_INVALID_HANDLE;
    }
    
    uint16_t data_len = ctxt->om->om_len;
    ESP_LOGD(TAG, "Notification received, length: %d", data_len);
    
    // Minimum Chameleon Ultra response is 10 bytes
    if (data_len >= 10) {
        uint8_t *data = ctxt->om->om_data;
        
        // Parse response: [0x11, 0xef, cmd_hi, cmd_lo, status_hi, status_lo, len_hi, len_lo, header_lrc, data..., data_lrc]
        g_last_response.command = (data[2] << 8) | data[3];
        g_last_response.status = data[5];
        g_last_response.data_size = data[7];
        
        ESP_LOGD(TAG, "Response - Command: 0x%04X, Status: 0x%02X, Data size: %d", 
                g_last_response.command, g_last_response.status, g_last_response.data_size);
        
        // Safely copy data if present and valid
        if (g_last_response.data_size > 0 && 
            g_last_response.data_size <= 200 && 
            data_len >= (9 + g_last_response.data_size)) {
            memcpy(g_last_response.data, data + 9, g_last_response.data_size);
        } else {
            g_last_response.data_size = 0; // Reset if invalid
        }
        
        g_response_received = true;
        
        // Signal response received
        if (g_response_sem) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xSemaphoreGiveFromISR(g_response_sem, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR();
            }
        }
    } else {
        ESP_LOGW(TAG, "Received notification too short: %d bytes", data_len);
    }
    
    return 0;
}

static int chameleon_service_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "Service discovered");
        // Start characteristic discovery
        int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle, chameleon_char_discovery_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "Failed to start characteristic discovery: %d", rc);
        }
    } else {
        ESP_LOGE(TAG, "Service discovery failed: %d", error->status);
    }
    return 0;
}

static int chameleon_char_discovery_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        // Check if this is TX or RX characteristic
        if (ble_uuid_cmp(&chr->uuid.u, &g_tx_char_uuid.u) == 0) {
            g_tx_char_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found TX characteristic, handle: 0x%04X", g_tx_char_handle);
        } else if (ble_uuid_cmp(&chr->uuid.u, &g_rx_char_uuid.u) == 0) {
            g_rx_char_handle = chr->val_handle;
            ESP_LOGI(TAG, "Found RX characteristic, handle: 0x%04X", g_rx_char_handle);
            
            // Enable notifications by writing to the CCCD (Client Characteristic Configuration Descriptor)
            // CCCD is typically at handle + 1
            uint8_t notify_enable[2] = {0x01, 0x00}; // Enable notifications
            int rc = ble_gattc_write_flat(conn_handle, chr->val_handle + 1, notify_enable, sizeof(notify_enable), NULL, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to enable notifications: %d", rc);
            } else {
                ESP_LOGI(TAG, "Successfully enabled notifications");
            }
        }
    } else if (error->status != 0) {
        ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
    }
    return 0;
}

static void start_service_discovery(void) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    
    ESP_LOGI(TAG, "Starting service discovery");
    int rc = ble_gattc_disc_svc_by_uuid(g_conn_handle, &g_service_uuid.u, chameleon_service_discovery_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start service discovery: %d", rc);
    }
}

static bool send_command(uint16_t cmd, uint8_t *data, size_t data_len) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_tx_char_handle == 0) {
        ESP_LOGW(TAG, "Not connected or no TX characteristic");
        return false;
    }

    uint8_t payload[200] = {
        0x11, 0xef,
        (cmd >> 8) & 0xFF, cmd & 0xFF,
        0x00, 0x00, (data_len >> 8) & 0xFF, data_len & 0xFF,
        0x00,  // LRC will be calculated
        0x00   // Data LRC will be calculated
    };
    
    // Calculate header LRC
    payload[8] = calculate_lrc(payload + 2, 6);
    
    // Copy data if any
    if (data_len > 0 && data != NULL) {
        memcpy(payload + 9, data, data_len);
    }
    
    // Calculate data LRC
    payload[9 + data_len] = calculate_lrc(payload + 9, data_len);
    
    size_t total_len = 10 + data_len;
    
    ESP_LOGD(TAG, "Sending command 0x%04X with %d bytes data", cmd, (int)data_len);
    
    // Reset response flag
    g_response_received = false;
    
    int rc = ble_gattc_write_no_rsp_flat(g_conn_handle, g_tx_char_handle, payload, total_len);
    return (rc == 0);
}

// some firmwares ack with a short frame first (status 0x60, len 0) and send data in a follow-up notification
static bool wait_for_cmd_data(uint16_t cmd, size_t min_len, uint32_t timeout_ms) {
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) return false;
        TickType_t slice = pdMS_TO_TICKS(200);
        if (slice > (deadline - now)) slice = (deadline - now);
        if (xSemaphoreTake(g_response_sem, slice) == pdTRUE) {
            // allow a small yield for notification copy completion
            vTaskDelay(pdMS_TO_TICKS(10));
            if (g_last_response.command == cmd) {
                if (g_last_response.data_size >= min_len) return true;
                // got ack or short frame; keep waiting for the data frame
            }
        }
    }
}

static void chameleon_ble_scan_callback(struct ble_gap_event *event, size_t len) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        // Check if this is a Chameleon Ultra device
        if (event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND ||
            event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP) {
            
            // Look for device name "ChameleonUltra" in advertisement data
            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
            if (rc == 0 && fields.name != NULL && fields.name_len >= 13) {
                if (memcmp(fields.name, "ChameleonUltra", 13) == 0) {
                    ESP_LOGI(TAG, "Found Chameleon Ultra device");
                    printf("Found Chameleon Ultra device!\n");
                    TERMINAL_VIEW_ADD_TEXT("Found Chameleon Ultra device!\n");
                    
                    g_discovered_device = event->disc;
                    g_device_found = true;
                    g_scanning = false;
                    
                    // Stop scanning
                    ble_gap_disc_cancel();
                    
                    // Signal scan completion
                    if (g_scan_sem) {
                        xSemaphoreGive(g_scan_sem);
                    }
                    return;
                }
            }
        }
    }
}

static int chameleon_gap_event_handler(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ESP_LOGI(TAG, "Connected to Chameleon Ultra");
                printf("Connected to Chameleon Ultra!\n");
                TERMINAL_VIEW_ADD_TEXT("Connected to Chameleon Ultra!\n");
                
                g_conn_handle = event->connect.conn_handle;
                g_is_connected = true;
                
                // Start service discovery to find the correct characteristic handles
                start_service_discovery();
                
                if (g_connect_sem) {
                    xSemaphoreGive(g_connect_sem);
                }
            } else {
                ESP_LOGE(TAG, "Connection failed with status %d", event->connect.status);
                printf("Connection failed\n");
                TERMINAL_VIEW_ADD_TEXT("Connection failed\n");
                g_is_connected = false;
                if (g_connect_sem) {
                    xSemaphoreGive(g_connect_sem);
                }
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected from Chameleon Ultra");
            printf("Disconnected from Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");

            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_tx_char_handle = 0;
            g_rx_char_handle = 0;
            g_is_connected = false;
            g_cached_hw_mode = 0xFF; // unknown
            chameleon_resume_ap();
            if (g_disconnect_sem) {
                xSemaphoreGive(g_disconnect_sem);
            }
            break;
            
        case BLE_GAP_EVENT_NOTIFY_RX:
            ESP_LOGD(TAG, "Notification received on handle 0x%04X", event->notify_rx.attr_handle);
            
            // Check if this is from our RX characteristic
            if (event->notify_rx.attr_handle == g_rx_char_handle) {
                // Process the notification data
                if (event->notify_rx.om) {
                    struct ble_gatt_access_ctxt ctxt = {
                        .op = BLE_GATT_ACCESS_OP_READ_CHR,
                        .om = event->notify_rx.om
                    };
                    chameleon_notification_cb(g_conn_handle, event->notify_rx.attr_handle, &ctxt, NULL);
                }
            }
            break;
            
        default:
            break;
    }
    
    return 0;
}

void chameleon_manager_init(void) {
    if (g_is_initialized) {
        return;
    }
    
    // Check available memory before initialization
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Free heap before Chameleon init: %d bytes", (int)free_heap);
    
    if (free_heap < 20480) { // 20KB minimum
        ESP_LOGE(TAG, "Insufficient memory for Chameleon manager: %d bytes available", (int)free_heap);
        printf("ERROR: Insufficient memory (%d bytes). Need at least 20KB.\n", (int)free_heap);
        TERMINAL_VIEW_ADD_TEXT("ERROR: Insufficient memory for Chameleon manager\n");
        return;
    }
    
    // Create semaphores
    g_scan_sem = xSemaphoreCreateBinary();
    g_connect_sem = xSemaphoreCreateBinary();
    g_response_sem = xSemaphoreCreateBinary();
    g_disconnect_sem = xSemaphoreCreateBinary();

    if (!g_scan_sem || !g_connect_sem || !g_response_sem || !g_disconnect_sem) {
        ESP_LOGE(TAG, "Failed to create semaphores");
        printf("Failed to initialize Chameleon Ultra manager\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to initialize Chameleon Ultra manager\n");
        if (g_scan_sem) { vSemaphoreDelete(g_scan_sem); g_scan_sem = NULL; }
        if (g_connect_sem) { vSemaphoreDelete(g_connect_sem); g_connect_sem = NULL; }
        if (g_response_sem) { vSemaphoreDelete(g_response_sem); g_response_sem = NULL; }
        if (g_disconnect_sem) { vSemaphoreDelete(g_disconnect_sem); g_disconnect_sem = NULL; }
        return;
    }
    
    // Initialize BLE if not already done
    ble_init();
    
    g_is_initialized = true;
    printf("Chameleon Ultra manager initialized\n");
    TERMINAL_VIEW_ADD_TEXT("Chameleon Ultra manager initialized\n");
    
    free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Free heap after Chameleon init: %d bytes", (int)free_heap);
}

bool chameleon_manager_connect(uint32_t timeout_seconds, const char* pin) {
    chameleon_suspend_ap();

    if (!g_is_initialized) {
        chameleon_manager_init();
        if (!g_is_initialized) {
            chameleon_resume_ap();
            return false;
        }
    }

    // Ensure BLE stack is initialized (we may have deinitialized it after last session)
    ble_init();

    if (g_is_connected) {
        printf("Already connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Already connected to Chameleon Ultra\n");
        chameleon_resume_ap();
        return true;
    }
    
    // Handle PIN parameter
    if (pin != NULL && strlen(pin) > 0) {
        // Validate PIN (should be 4-6 digits)
        int pin_len = strlen(pin);
        if (pin_len < 4 || pin_len > 6) {
            printf("Invalid PIN: must be 4-6 digits\n");
            TERMINAL_VIEW_ADD_TEXT("Invalid PIN: must be 4-6 digits\n");
            chameleon_resume_ap();
            return false;
        }
        
        // Check if PIN contains only digits
        for (int i = 0; i < pin_len; i++) {
            if (pin[i] < '0' || pin[i] > '9') {
                printf("Invalid PIN: must contain only digits\n");
                TERMINAL_VIEW_ADD_TEXT("Invalid PIN: must contain only digits\n");
                chameleon_resume_ap();
                return false;
            }
        }
        
        // Store PIN for authentication
        strncpy(g_chameleon_pin, pin, sizeof(g_chameleon_pin) - 1);
        g_chameleon_pin[sizeof(g_chameleon_pin) - 1] = '\0';
        g_pin_required = true;
        
        printf("PIN set for Chameleon Ultra authentication\n");
        TERMINAL_VIEW_ADD_TEXT("PIN set for authentication\n");
    } else {
        // Clear PIN if not provided
        memset(g_chameleon_pin, 0, sizeof(g_chameleon_pin));
        g_pin_required = false;
    }
    
    printf("Searching for Chameleon Ultra device...\n");
    TERMINAL_VIEW_ADD_TEXT("Searching for Chameleon Ultra device...\n");
    
    // Reset state
    g_device_found = false;
    g_scanning = true;
    
    // Register our scan callback
    esp_err_t err = ble_register_handler(chameleon_ble_scan_callback);
    if (err != ESP_OK) {
        printf("Failed to register BLE handler\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to register BLE handler\n");
        chameleon_resume_ap();
        return false;
    }
    
    // Start BLE scanning using the existing BLE manager
    ble_start_scanning();
    
    // Wait for device to be found
    if (xSemaphoreTake(g_scan_sem, pdMS_TO_TICKS(timeout_seconds * 1000)) == pdTRUE) {
        if (g_device_found) {
            printf("Chameleon Ultra found! Connecting...\n");
            TERMINAL_VIEW_ADD_TEXT("Chameleon Ultra found! Connecting...\n");
            
            // Unregister scan callback
            ble_unregister_handler(chameleon_ble_scan_callback);

            // Ensure scanning is fully stopped before attempting to connect
            (void)ble_gap_disc_cancel();
            for (int i = 0; i < 10; ++i) { // wait up to ~500ms
                if (!ble_gap_disc_active()) break;
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            // Give controller a bit more time to free resources
            vTaskDelay(pdMS_TO_TICKS(100));

            // Try to connect (infer own address type to avoid rc=519 errors on some boards)
            uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
            int rc_id = ble_hs_id_infer_auto(0, &own_addr_type);
            if (rc_id != 0) {
                ESP_LOGW(TAG, "ble_hs_id_infer_auto failed rc=%d, falling back to PUBLIC", rc_id);
                own_addr_type = BLE_OWN_ADDR_PUBLIC;
            }

            // Log internal/spiram free memory around connect for debugging
            size_t free_int = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "pre-connect free: int=%u psram=%u", (unsigned)free_int, (unsigned)free_psram);

            int rc = ble_gap_connect(own_addr_type, &g_discovered_device.addr,
                                   30000, NULL, chameleon_gap_event_handler, NULL);
            if (rc == 0) {
                // Wait for connection
                if (xSemaphoreTake(g_connect_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
                    if (g_is_connected) {
                        printf("Successfully connected to Chameleon Ultra\n");
                        TERMINAL_VIEW_ADD_TEXT("Successfully connected to Chameleon Ultra\n");
                        // After UID discovery attempt a full dump to populate caches
                        if (g_last_hf_scan.valid) {
                            (void)chameleon_manager_read_ntag_card();
                            if (!chameleon_manager_has_cached_ntag_dump()) {
                                (void)chameleon_manager_read_hf_card();
                            }
                            if (g_cached_details_text) {
                                free(g_cached_details_text);
                                g_cached_details_text = NULL;
                            }
                            g_cached_details_text = chameleon_manager_build_cached_details();
                            g_cached_details_session++;
                        }
                        uint16_t batt_mv = 0;
                        uint8_t batt_pct = 0;
                        if (cu_query_battery_status(&batt_mv, &batt_pct)) {
                            glog("Chameleon Ultra battery: %dmV (%d%%)\n", batt_mv, batt_pct);
                        }
                        return true;
                    }
                }
            } else {
                ESP_LOGE(TAG, "Failed to start connection, rc=%d", rc);
                // quick retry with alternate own address type after cooldown
                vTaskDelay(pdMS_TO_TICKS(600));
                uint8_t retry_addr_type = own_addr_type;
                uint8_t alt_addr_type = retry_addr_type;
                int rc_id2 = ble_hs_id_infer_auto(1, &alt_addr_type);
                if (rc_id2 != 0) {
                    alt_addr_type = BLE_OWN_ADDR_RANDOM;
                }
                if (alt_addr_type == retry_addr_type) {
                    alt_addr_type = (retry_addr_type == BLE_OWN_ADDR_PUBLIC) ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
                }
                ESP_LOGW(TAG, "Retrying connect with own_addr_type=%u", (unsigned)alt_addr_type);
                rc = ble_gap_connect(alt_addr_type, &g_discovered_device.addr,
                                     30000, NULL, chameleon_gap_event_handler, NULL);
                if (rc == 0) {
                    if (xSemaphoreTake(g_connect_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
                        if (g_is_connected) {
                            printf("Successfully connected to Chameleon Ultra\n");
                            TERMINAL_VIEW_ADD_TEXT("Successfully connected to Chameleon Ultra\n");
                            return true;
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Retry connect failed, rc=%d", rc);
                }
                free_int = heap_caps_get_free_size(MALLOC_CAP_8BIT);
                free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
                ESP_LOGI(TAG, "post-fail free: int=%u psram=%u", (unsigned)free_int, (unsigned)free_psram);
            }
        }
    }
    
    // Cleanup on failure
    ble_unregister_handler(chameleon_ble_scan_callback);
    ble_deinit();
    chameleon_resume_ap();

    printf("Failed to connect to Chameleon Ultra\n");
    TERMINAL_VIEW_ADD_TEXT("Failed to connect to Chameleon Ultra\n");
    return false;
}

bool chameleon_manager_get_last_hf_scan(uint8_t *uid, uint8_t *uid_len,
                                        uint16_t *atqa, uint8_t *sak) {
    if (!g_last_hf_scan.valid) return false;
    if (uid && uid_len) {
        *uid_len = g_last_hf_scan.uid_size;
        if (*uid_len > 0) memcpy(uid, g_last_hf_scan.uid, *uid_len);
    } else if (uid_len) {
        *uid_len = g_last_hf_scan.uid_size;
    }
    if (atqa) *atqa = g_last_hf_scan.atqa;
    if (sak) *sak = g_last_hf_scan.sak;
    return true;
}

void chameleon_manager_disconnect(void) {
    if (g_is_connected && g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if (g_disconnect_sem) {
            xSemaphoreTake(g_disconnect_sem, 0);
        }
        int rc = ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0) {
            if (rc == BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "ble_gap_terminate already pending");
            } else if (rc == BLE_HS_ENOTCONN) {
                ESP_LOGW(TAG, "ble_gap_terminate: not connected");
                g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                g_tx_char_handle = 0;
                g_rx_char_handle = 0;
                g_is_connected = false;
                g_cached_hw_mode = 0xFF;
                chameleon_resume_ap();
                printf("Disconnected from Chameleon Ultra\n");
                TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
                return;
            } else {
                ESP_LOGW(TAG, "ble_gap_terminate failed rc=%d", rc);
            }
        }
        if (g_disconnect_sem) {
            if (xSemaphoreTake(g_disconnect_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ESP_LOGW(TAG, "Timeout waiting for disconnect event");
                struct ble_gap_conn_desc desc;
                int fr = ble_gap_conn_find(g_conn_handle, &desc);
                if (fr != 0) {
                    // Not connected according to stack; clear local state
                    g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                    g_tx_char_handle = 0;
                    g_rx_char_handle = 0;
                    g_is_connected = false;
                    g_cached_hw_mode = 0xFF;
                    if (g_cached_details_text) {
                        free(g_cached_details_text);
                        g_cached_details_text = NULL;
                        g_cached_details_session++;
                    }
                    chameleon_resume_ap();
                    printf("Disconnected from Chameleon Ultra\n");
                    TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
                } else {
                    ESP_LOGW(TAG, "Connection still active after terminate request");
                    return;
                }
            } else {
                printf("Disconnected from Chameleon Ultra\n");
                TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
            }
        } else {
            // No semaphore; best-effort local cleanup
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_tx_char_handle = 0;
            g_rx_char_handle = 0;
            g_is_connected = false;
            g_cached_hw_mode = 0xFF;
            if (g_cached_details_text) {
                free(g_cached_details_text);
                g_cached_details_text = NULL;
                g_cached_details_session++;
            }
            chameleon_resume_ap();
            printf("Disconnected from Chameleon Ultra\n");
            TERMINAL_VIEW_ADD_TEXT("Disconnected from Chameleon Ultra\n");
        }
    }
}

bool chameleon_manager_is_connected(void) {
    // report BLE link state; command readiness is exposed via chameleon_manager_is_ready()
    return g_is_connected;
}

bool chameleon_manager_scan_hf(void) {
    if (!g_is_connected || g_tx_char_handle == 0) {
        printf("Chameleon not ready (connection or TX characteristic missing)\n");
        TERMINAL_VIEW_ADD_TEXT("Chameleon not ready; please wait a moment and try again.\n");
        return false;
    }
    
    // Ensure reader mode only if not already set
    if (g_cached_hw_mode != HW_MODE_READER) {
        printf("Setting reader mode...\n");
        TERMINAL_VIEW_ADD_TEXT("Setting reader mode...\n");
        g_response_received = false;
        uint8_t mode_data = HW_MODE_READER;
        if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode_data, 1)) {
            printf("Failed to set reader mode\n");
            TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
            return false;
        }
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received) {
            printf("Mode change timeout\n");
            TERMINAL_VIEW_ADD_TEXT("Mode change timeout\n");
            return false;
        }
        if (g_last_response.status != STATUS_SUCCESS) {
            printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
            TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
            return false;
        }
        g_cached_hw_mode = HW_MODE_READER;
    }
    printf("Scanning for HF tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Scanning for HF tags...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_HF14A_SCAN, NULL, 0);
    if (result) {
        printf("HF scan command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("HF scan command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_HF14A_SCAN) {
                if (g_last_response.status == 0x00 || g_last_response.status == 0x68) { // HF_TAG_OK - tag found
                    if (g_last_response.data_size >= 7) {
                        uint8_t uid_size = g_last_response.data[0];
                        printf("HF Tag found!\n");
                        printf("UID (%d bytes): ", uid_size);
                        TERMINAL_VIEW_ADD_TEXT("HF Tag found!\nUID (%d bytes): ", uid_size);
                        
                        for (uint8_t i = 0; i < uid_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[1 + i]);
                        }
                        printf("\n");
                        
                        uint8_t atqa_lo = g_last_response.data[1 + uid_size];
                        uint8_t atqa_hi = g_last_response.data[2 + uid_size];
                        uint8_t sak = g_last_response.data[3 + uid_size];
                        
                        printf("ATQA: %02X %02X\n", atqa_hi, atqa_lo);
                        printf("SAK: %02X\n", sak);
                        
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < uid_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[1 + i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\nATQA: %02X %02X\nSAK: %02X\n", uid_str, atqa_hi, atqa_lo, sak);
                        
                        // Store scan data for saving
                        g_last_hf_scan.valid = true;
                        g_last_hf_scan.uid_size = uid_size;
                        memcpy(g_last_hf_scan.uid, &g_last_response.data[1], uid_size);
                        g_last_hf_scan.atqa = ((uint16_t)atqa_hi << 8) | atqa_lo;
                        g_last_hf_scan.sak = sak;
                        
                        // Identify card type based on ATQA and SAK
                        uint16_t atqa = (atqa_hi << 8) | atqa_lo;
                        if (atqa == 0x0044 && sak == 0x00) {
                            // This is likely an NTAG card - ATQA 0x0044 and SAK 0x00 are NTAG characteristics
                            snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type),
                                     "NTAG (ATQA:%02X%02X SAK:%02X)", atqa_hi, atqa_lo, sak);
                        } else if (desfire_is_desfire_candidate(atqa, sak)) {
                            // Keep this label short enough for the 32-byte tag_type buffer
                            snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type),
                                     "MIFARE DESFire");
                        } else {
                            snprintf(g_last_hf_scan.tag_type, sizeof(g_last_hf_scan.tag_type),
                                     "HF-14A (ATQA:%02X%02X SAK:%02X)", atqa_hi, atqa_lo, sak);
                        }
                        
                        g_last_hf_scan.timestamp = time(NULL);

                        bool details_refreshed = false;
                        if (chameleon_manager_last_scan_is_ntag()) {
                            if (chameleon_manager_read_ntag_card()) {
                                details_refreshed = true;
                            } else {
                                memset(&g_last_ntag_dump, 0, sizeof(g_last_ntag_dump));
                            }
                        } else {
                            // Not NTAG: do not start Classic dict read from within scan to avoid nested scans on CLI
                            memset(&g_last_ntag_dump, 0, sizeof(g_last_ntag_dump));
                        }

                        if (!details_refreshed) {
                            if (g_cached_details_text) {
                                free(g_cached_details_text);
                                g_cached_details_text = NULL;
                            }
                            g_cached_details_text = chameleon_manager_build_cached_details();
                            g_cached_details_session++;
                        }

                        return true;
                    }
                } else if (g_last_response.status == 0x01) { // HF_TAG_NO
                    printf("No HF tag found\n");
                    TERMINAL_VIEW_ADD_TEXT("No HF tag found\n");
                } else if (g_last_response.status == 0x66) { // Status we've been seeing
                    printf("HF scan failed: Possibly wrong mode or device state (0x66)\n");
                    TERMINAL_VIEW_ADD_TEXT("HF scan failed: Possibly wrong mode or device state (0x66)\n");
                } else {
                    printf("HF scan failed with status: 0x%02X\n", g_last_response.status);
                    TERMINAL_VIEW_ADD_TEXT("HF scan failed with status: 0x%02X\n", g_last_response.status);
                }
            }
        } else {
            printf("HF scan command timed out\n");
            TERMINAL_VIEW_ADD_TEXT("HF scan command timed out\n");
        }
    } else {
        printf("Failed to send HF scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send HF scan command\n");
    }
    
    return false;
}

bool chameleon_manager_scan_lf(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Scanning for LF tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Scanning for LF tags...\n");
    
    // First, set device to reader mode for LF
    printf("Setting device to reader mode for LF scan...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting device to reader mode for LF scan...\n");
    
    uint8_t mode = HW_MODE_READER;
    bool mode_result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (!mode_result) {
        printf("Failed to set reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
        return false;
    }
    
    // Wait for mode change response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_CHANGE_DEVICE_MODE) {
            if (g_last_response.status != STATUS_SUCCESS) {
                printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
                return false;
            }
        } else {
            printf("Mode change command failed\n");
            TERMINAL_VIEW_ADD_TEXT("Mode change command failed\n");
            return false;
        }
    } else {
        printf("Mode change command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Mode change command timed out\n");
        return false;
    }
    
    // Clear response state for the scan command
    g_response_received = false;
    
    bool result = send_command(CMD_EM410X_SCAN, NULL, 0);
    if (result) {
        printf("LF scan command sent, waiting for response...\n");
        TERMINAL_VIEW_ADD_TEXT("LF scan command sent, waiting for response...\n");
        
        // Wait for response
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (g_response_received && g_last_response.command == CMD_EM410X_SCAN) {
                ESP_LOGI(TAG, "LF scan response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
                
                if (g_last_response.status == 0x40) { // LF_TAG_OK
                    if (g_last_response.data_size >= 5) {
                        printf("LF Tag found!\n");
                        printf("UID (%d bytes): ", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("LF Tag found!\nUID (%d bytes): ", g_last_response.data_size);
                        
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[i]);
                        }
                        printf("\n");
                        
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\n", uid_str);
                        
                        // Store scan data for saving
                        g_last_lf_scan.valid = true;
                        g_last_lf_scan.uid_size = g_last_response.data_size;
                        memcpy(g_last_lf_scan.uid, g_last_response.data, g_last_response.data_size);
                        snprintf(g_last_lf_scan.tag_type, sizeof(g_last_lf_scan.tag_type), "LF-EM410X");
                        g_last_lf_scan.timestamp = time(NULL);
                        
                        return true;
                    } else {
                        printf("LF Tag found but insufficient data: %d bytes\n", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("LF Tag found but insufficient data: %d bytes\n", g_last_response.data_size);
                    }
                } else if (g_last_response.status == 0x41) { // EM410X_TAG_NO_FOUND
                    printf("No EM410X LF tag found\n");
                    TERMINAL_VIEW_ADD_TEXT("No EM410X LF tag found\n");
                } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x68) {
                    // Some success status but might be a different tag type
                    printf("LF scan completed successfully but no EM410X tag detected\n");
                    TERMINAL_VIEW_ADD_TEXT("LF scan completed successfully but no EM410X tag detected\n");
                    if (g_last_response.data_size > 0) {
                        printf("Received %d bytes of data: ", g_last_response.data_size);
                        TERMINAL_VIEW_ADD_TEXT("Received %d bytes of data: ", g_last_response.data_size);
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            printf("%02X ", g_last_response.data[i]);
                        }
                        printf("\n");
                        char uid_str[64] = "";
                        for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                            char temp[4];
                            snprintf(temp, sizeof(temp), "%02X ", g_last_response.data[i]);
                            strcat(uid_str, temp);
                        }
                        TERMINAL_VIEW_ADD_TEXT("%s\n", uid_str);
                    }
                } else {
                    printf("LF scan failed with status: 0x%02X\n", g_last_response.status);
                    TERMINAL_VIEW_ADD_TEXT("LF scan failed with status: 0x%02X\n", g_last_response.status);
                }
            }
        } else {
            printf("LF scan command timed out\n");
            TERMINAL_VIEW_ADD_TEXT("LF scan command timed out\n");
        }
    } else {
        printf("Failed to send LF scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send LF scan command\n");
    }
    
    return false;
}

static bool cu_query_battery_status(uint16_t *out_mv, uint8_t *out_percent) {
    if (!g_is_connected) return false;
    g_response_received = false;
    if (!send_command(CMD_GET_BATTERY_INFO, NULL, 0)) return false;
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) != pdTRUE) return false;
    if (!g_response_received) return false;
    if (g_last_response.command != CMD_GET_BATTERY_INFO) return false;
    if (g_last_response.status != STATUS_SUCCESS) return false;
    if (g_last_response.data_size < 3) return false;
    if (out_mv) {
        *out_mv = (uint16_t)((g_last_response.data[0] << 8) | g_last_response.data[1]);
    }
    if (out_percent) {
        *out_percent = g_last_response.data[2];
    }
    return true;
}

bool chameleon_manager_get_battery_info(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting battery information...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting battery information...\n");
    
    uint16_t voltage = 0;
    uint8_t percentage = 0;
    if (cu_query_battery_status(&voltage, &percentage)) {
        printf("Battery: %dmV (%d%%)\n", voltage, percentage);
        TERMINAL_VIEW_ADD_TEXT("Battery: %dmV (%d%%)\n", voltage, percentage);
        return true;
    }

    printf("Failed to retrieve battery information\n");
    TERMINAL_VIEW_ADD_TEXT("Failed to retrieve battery information\n");
    return false;
}

bool chameleon_manager_query_battery(uint16_t *out_mv, uint8_t *out_percent) {
    return cu_query_battery_status(out_mv, out_percent);
}

bool chameleon_manager_set_reader_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Setting Chameleon Ultra to reader mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting Chameleon Ultra to reader mode...\n");
    
    uint8_t mode = HW_MODE_READER;
    bool result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (result) {
        printf("Reader mode command sent\n");
        TERMINAL_VIEW_ADD_TEXT("Reader mode command sent\n");
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE && g_response_received && g_last_response.status == STATUS_SUCCESS) {
            g_cached_hw_mode = HW_MODE_READER;
        }
    } else {
        printf("Failed to send reader mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send reader mode command\n");
    }
    
    return result;
}

bool chameleon_manager_set_emulator_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Setting Chameleon Ultra to emulator mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting Chameleon Ultra to emulator mode...\n");
    
    uint8_t mode = HW_MODE_EMULATOR;
    bool result = send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1);
    if (result) {
        printf("Emulator mode command sent\n");
        TERMINAL_VIEW_ADD_TEXT("Emulator mode command sent\n");
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE && g_response_received && g_last_response.status == STATUS_SUCCESS) {
            g_cached_hw_mode = HW_MODE_EMULATOR;
        }
    } else {
        printf("Failed to send emulator mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send emulator mode command\n");
    }
    
    return result;
}

// Device information functions
bool chameleon_manager_get_firmware_version(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting firmware version...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting firmware version...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_GET_APP_VERSION, NULL, 0);
    if (!result) {
        printf("Failed to send firmware version command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send firmware version command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_APP_VERSION) {
            if (g_last_response.status == STATUS_SUCCESS) {
                if (g_last_response.data_size >= 2) {
                    uint8_t major = g_last_response.data[0];
                    uint8_t minor = g_last_response.data[1];
                    if (g_last_response.data_size >= 3) {
                        uint8_t patch = g_last_response.data[2];
                        printf("Firmware Version: %d.%d.%d\n", major, minor, patch);
                        TERMINAL_VIEW_ADD_TEXT("Firmware Version: %d.%d.%d\n", major, minor, patch);
                    } else {
                        printf("Firmware Version: %d.%d\n", major, minor);
                        TERMINAL_VIEW_ADD_TEXT("Firmware Version: %d.%d\n", major, minor);
                    }
                    return true;
                }
            } else {
                printf("Failed to get firmware version, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get firmware version, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Firmware version command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Firmware version command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_device_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting device mode...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting device mode...\n");
    
    // Clear any previous response and reset flag
    memset(&g_last_response, 0, sizeof(g_last_response));
    g_response_received = false;
    
    bool result = send_command(CMD_GET_DEVICE_MODE, NULL, 0);
    if (!result) {
        printf("Failed to send device mode command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send device mode command\n");
        return false;
    }
    
    // Wait for response with longer timeout
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(8000)) == pdTRUE) {
        // Give a small delay for the response to be processed
        vTaskDelay(pdMS_TO_TICKS(100));
        
        ESP_LOGI(TAG, "After semaphore - Response received=%d", g_response_received);
        if (g_response_received) {
            ESP_LOGI(TAG, "Command received=0x%04X, expected=0x%04X", g_last_response.command, CMD_GET_DEVICE_MODE);
        }
        
        if (g_response_received && g_last_response.command == CMD_GET_DEVICE_MODE) {
            ESP_LOGI(TAG, "Status=0x%02X, Data size=%d", g_last_response.status, g_last_response.data_size);
            
            if (g_last_response.data_size >= 1) {
                uint8_t mode = g_last_response.data[0];
                ESP_LOGI(TAG, "Mode byte=0x%02X", mode);
                
                const char* mode_str;
                if (mode == HW_MODE_READER) {
                    mode_str = "Reader";
                } else if (mode == HW_MODE_EMULATOR) {
                    mode_str = "Emulator";
                } else {
                    mode_str = "Unknown";
                }
                g_cached_hw_mode = mode;
                
                printf("Device Mode: %s (0x%02X)\n", mode_str, mode);
                TERMINAL_VIEW_ADD_TEXT("Device Mode: %s (0x%02X)\n", mode_str, mode);
                return true;
            } else {
                printf("Failed to get device mode, status: 0x%02X, insufficient data\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get device mode, status: 0x%02X, insufficient data\n", g_last_response.status);
            }
        } else {
            ESP_LOGI(TAG, "Response received=%d, Command match=%d", g_response_received, 
                   g_response_received ? (g_last_response.command == CMD_GET_DEVICE_MODE) : 0);
        }
    } else {
        printf("Device mode command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Device mode command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_active_slot(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Getting active slot...\n");
    TERMINAL_VIEW_ADD_TEXT("Getting active slot...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_GET_ACTIVE_SLOT, NULL, 0);
    if (!result) {
        printf("Failed to send active slot command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send active slot command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_ACTIVE_SLOT) {
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 1) {
                uint8_t device_slot = g_last_response.data[0];
                uint8_t user_slot = device_slot + 1; // Convert 0-7 to 1-8
                printf("Active Slot: %d\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("Active Slot: %d\n", user_slot);
                return true;
            } else {
                printf("Failed to get active slot, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get active slot, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Active slot command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Active slot command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_set_active_slot(uint8_t slot) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    if (slot > 7) {
        printf("Invalid slot number: %d (must be 0-7)\n", slot);
        TERMINAL_VIEW_ADD_TEXT("Invalid slot number: %d (must be 0-7)\n", slot);
        return false;
    }
    
    uint8_t user_slot = slot + 1; // Convert 0-7 to 1-8 for display
    printf("Setting active slot to %d...\n", user_slot);
    TERMINAL_VIEW_ADD_TEXT("Setting active slot to %d...\n", user_slot);
    
    g_response_received = false;
    bool result = send_command(CMD_SET_ACTIVE_SLOT, &slot, 1);
    if (!result) {
        printf("Failed to send set active slot command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send set active slot command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_SET_ACTIVE_SLOT) {
            if (g_last_response.status == STATUS_SUCCESS) {
                printf("Active slot set to %d successfully\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("Active slot set to %d successfully\n", user_slot);
                return true;
            } else {
                printf("Failed to set active slot, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to set active slot, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Set active slot command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Set active slot command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_get_slot_info(uint8_t slot) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    if (slot > 7) {
        printf("Invalid slot number: %d (must be 0-7)\n", slot);
        TERMINAL_VIEW_ADD_TEXT("Invalid slot number: %d (must be 0-7)\n", slot);
        return false;
    }
    
    uint8_t user_slot = slot + 1; // Convert 0-7 to 1-8 for display
    printf("Getting slot %d information...\n", user_slot);
    TERMINAL_VIEW_ADD_TEXT("Getting slot %d information...\n", user_slot);
    
    g_response_received = false;
    bool result = send_command(CMD_GET_SLOT_INFO, &slot, 1);
    if (!result) {
        printf("Failed to send slot info command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send slot info command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(5000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_GET_SLOT_INFO) {
            if (g_last_response.status == STATUS_SUCCESS && g_last_response.data_size >= 3) {
                uint8_t hf_type = g_last_response.data[0];
                uint8_t lf_type = g_last_response.data[1];
                uint8_t enabled = g_last_response.data[2];
                
                printf("Slot %d Info:\n", user_slot);
                printf("  HF Type: 0x%02X\n", hf_type);
                printf("  LF Type: 0x%02X\n", lf_type);
                printf("  Enabled: %s\n", enabled ? "Yes" : "No");
                
                TERMINAL_VIEW_ADD_TEXT("Slot %d Info:\n", user_slot);
                TERMINAL_VIEW_ADD_TEXT("  HF Type: 0x%02X\n", hf_type);
                TERMINAL_VIEW_ADD_TEXT("  LF Type: 0x%02X\n", lf_type);
                TERMINAL_VIEW_ADD_TEXT("  Enabled: %s\n", enabled ? "Yes" : "No");
                
                return true;
            } else {
                printf("Failed to get slot info, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to get slot info, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("Slot info command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("Slot info command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_mf1_detect_support(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting MIFARE Classic support...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting MIFARE Classic support...\n");
    
    // First, run HF scan to detect and establish communication with tag
    printf("Running HF scan first to detect tag...\n");
    TERMINAL_VIEW_ADD_TEXT("Running HF scan first to detect tag...\n");
    
    if (!chameleon_manager_scan_hf()) {
        printf("No HF tag found - MIFARE detection requires a tag to be present\n");
        TERMINAL_VIEW_ADD_TEXT("No HF tag found - MIFARE detection requires a tag to be present\n");
        return false;
    }
    
    // Small delay to ensure scan is complete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_DETECT_SUPPORT, NULL, 0);
    if (!result) {
        printf("Failed to send MF1 detect support command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send MF1 detect support command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_DETECT_SUPPORT) {
            ESP_LOGI(TAG, "MF1 detect response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
            
            if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && g_last_response.data_size >= 1) {
                uint8_t support = g_last_response.data[0];
                const char* support_str;
                if (support) {
                    support_str = "Supported";
                } else {
                    support_str = "Not Supported";
                }
                printf("MIFARE Classic Support: %s\n", support_str);
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic Support: %s\n", support_str);
                return true;
            } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                // Success status but no data - interpret as supported
                printf("MIFARE Classic Support: Supported (no specific data returned)\n");
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic Support: Supported (no specific data returned)\n");
                return true;
            } else {
                printf("Failed to detect MF1 support, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to detect MF1 support, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("MF1 detect support command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("MF1 detect support command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_mf1_detect_prng(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting MIFARE Classic PRNG type...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting MIFARE Classic PRNG type...\n");
    
    // First, run HF scan to detect and establish communication with tag
    printf("Running HF scan first to detect tag...\n");
    TERMINAL_VIEW_ADD_TEXT("Running HF scan first to detect tag...\n");
    
    if (!chameleon_manager_scan_hf()) {
        printf("No HF tag found - MIFARE PRNG detection requires a tag to be present\n");
        TERMINAL_VIEW_ADD_TEXT("No HF tag found - MIFARE PRNG detection requires a tag to be present\n");
        return false;
    }
    
    // Small delay to ensure scan is complete
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_response_received = false;
    bool result = send_command(CMD_MF1_DETECT_PRNG, NULL, 0);
    if (!result) {
        printf("Failed to send MF1 detect PRNG command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send MF1 detect PRNG command\n");
        return false;
    }
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_MF1_DETECT_PRNG) {
            ESP_LOGI(TAG, "MF1 PRNG detect response - Status: 0x%02X, Data size: %d", g_last_response.status, g_last_response.data_size);
            
            if ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) && g_last_response.data_size >= 1) {
                uint8_t prng_type = g_last_response.data[0];
                const char* prng_str;
                switch (prng_type) {
                    case 0: prng_str = "Static"; break;
                    case 1: prng_str = "Weak"; break;
                    case 2: prng_str = "Hard"; break;
                    default: prng_str = "Unknown"; break;
                }
                printf("MIFARE Classic PRNG Type: %s (0x%02X)\n", prng_str, prng_type);
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic PRNG Type: %s (0x%02X)\n", prng_str, prng_type);
                return true;
            } else if (g_last_response.status == STATUS_SUCCESS || g_last_response.status == 0x00) {
                // Success status but no data - no MIFARE tag present for PRNG detection
                printf("MIFARE Classic PRNG detection successful, but no MIFARE tag present\n");
                TERMINAL_VIEW_ADD_TEXT("MIFARE Classic PRNG detection successful, but no MIFARE tag present\n");
                return true;
            } else {
                printf("Failed to detect PRNG type, status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("Failed to detect PRNG type, status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("MF1 detect PRNG command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("MF1 detect PRNG command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_scan_hidprox(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // First, ensure we're in reader mode for scanning
    printf("Setting reader mode for HID Prox scan...\n");
    TERMINAL_VIEW_ADD_TEXT("Setting reader mode for HID Prox scan...\n");
    
    g_response_received = false;
    uint8_t mode_data = HW_MODE_READER;
    if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode_data, 1)) {
        printf("Failed to set reader mode\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode\n");
        return false;
    }
    
    // Wait for mode change response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received) {
        printf("Mode change timeout\n");
        TERMINAL_VIEW_ADD_TEXT("Mode change timeout\n");
        return false;
    }
    
    if (g_last_response.status != STATUS_SUCCESS) {
        printf("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        TERMINAL_VIEW_ADD_TEXT("Failed to set reader mode, status: 0x%02X\n", g_last_response.status);
        return false;
    }
    
    printf("Reader mode set, scanning for HID Prox tags...\n");
    TERMINAL_VIEW_ADD_TEXT("Reader mode set, scanning for HID Prox tags...\n");
    
    g_response_received = false;
    bool result = send_command(CMD_HIDPROX_SCAN, NULL, 0);
    if (!result) {
        printf("Failed to send HID Prox scan command\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to send HID Prox scan command\n");
        return false;
    }
    
    printf("HID Prox scan command sent, waiting for response...\n");
    TERMINAL_VIEW_ADD_TEXT("HID Prox scan command sent, waiting for response...\n");
    
    // Wait for response
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(10000)) == pdTRUE) {
        if (g_response_received && g_last_response.command == CMD_HIDPROX_SCAN) {
            if (g_last_response.status == STATUS_LF_TAG_OK && g_last_response.data_size >= 5) {
                // Parse HID Prox data: typically 5 bytes
                printf("HID Prox Tag found!\n");
                printf("Tag Data: ");
                TERMINAL_VIEW_ADD_TEXT("HID Prox Tag found!\nTag Data: ");
                
                for (uint8_t i = 0; i < g_last_response.data_size && i < 10; i++) {
                    printf("%02X ", g_last_response.data[i]);
                }
                printf("\n");
                
                TERMINAL_VIEW_ADD_TEXT("\n");
                
                return true;
            } else if (g_last_response.status == STATUS_LF_TAG_NO) {
                printf("No HID Prox tag found\n");
                TERMINAL_VIEW_ADD_TEXT("No HID Prox tag found\n");
            } else {
                printf("HID Prox scan failed with status: 0x%02X\n", g_last_response.status);
                TERMINAL_VIEW_ADD_TEXT("HID Prox scan failed with status: 0x%02X\n", g_last_response.status);
            }
        }
    } else {
        printf("HID Prox scan command timed out\n");
        TERMINAL_VIEW_ADD_TEXT("HID Prox scan command timed out\n");
    }
    
    return false;
}

bool chameleon_manager_read_hf_card(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Starting basic HF card reading...\n");
    TERMINAL_VIEW_ADD_TEXT("Starting basic HF card reading...\n");
    
    // First scan to get card info
    if (!chameleon_manager_scan_hf()) {
        printf("Failed to scan HF card first\n");
        TERMINAL_VIEW_ADD_TEXT("Failed to scan HF card first\n");
        return false;
    }
    
    // Initialize data structures
    memset(&g_last_card_dump, 0, sizeof(g_last_card_dump));
    g_last_card_dump.valid = true;
    g_last_card_dump.timestamp = time(NULL);
    
    // Copy basic info from scan
    if (g_last_hf_scan.valid) {
        memcpy(g_last_card_dump.uid, g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_last_card_dump.uid_size = g_last_hf_scan.uid_size;
        strncpy(g_last_card_dump.tag_type, g_last_hf_scan.tag_type, sizeof(g_last_card_dump.tag_type));
        
        printf("Card detected: %s\n", g_last_card_dump.tag_type);
        printf("UID: ");
        for (int i = 0; i < g_last_card_dump.uid_size; i++) {
            printf("%02X", g_last_card_dump.uid[i]);
        }
        printf("\n");
        TERMINAL_VIEW_ADD_TEXT("Card detected: %s\n", g_last_card_dump.tag_type);
    }
    
    printf("Basic card information collected successfully.\n");
    printf("Note: For detailed MIFARE Classic analysis, use specialized tools.\n");
    TERMINAL_VIEW_ADD_TEXT("Basic card reading completed\n");
    
    if (g_cached_details_text) {
        free(g_cached_details_text);
        g_cached_details_text = NULL;
    }
    g_cached_details_text = chameleon_manager_build_cached_details();
    g_cached_details_session++;
    
    ghostchi_manager_add_xp(8);
    return true;
}

bool chameleon_manager_read_lf_card(void) {
    printf("LF card full dump not yet implemented\n");
    TERMINAL_VIEW_ADD_TEXT("LF card full dump not yet implemented\n");
    return false;
}

bool chameleon_manager_save_card_dump(const char* filename) {
    // Mirror UI save flow: prefer cached NTAG, then try NTAG read+save,
    // then Mifare Classic cache save (read+save if needed), finally HF header.
    bool ok = false;

    if (chameleon_manager_has_cached_ntag_dump()) {
        ok = chameleon_manager_save_ntag_dump(filename);
        if (ok) {
            glog("Saved NTAG dump from cache\n");
            return true;
        }
    }

    if (!ok && chameleon_manager_last_scan_is_ntag()) {
        if (chameleon_manager_read_ntag_card()) {
            ok = chameleon_manager_save_ntag_dump(filename);
            if (ok) {
                glog("Saved NTAG dump after rescan\n");
                return true;
            }
        }
    }

    if (!ok && chameleon_manager_mf1_has_cache()) {
        ok = chameleon_manager_mf1_save_flipper_dump(filename);
        if (ok) {
            glog("Saved Mifare Classic dump from cache\n");
            return true;
        }
    }

    if (!ok) {
        (void)chameleon_manager_mf1_read_classic_with_dict(false);
        if (chameleon_manager_mf1_has_cache()) {
            ok = chameleon_manager_mf1_save_flipper_dump(filename);
            if (ok) {
                glog("Saved Mifare Classic dump after dict read\n");
                return true;
            }
        }
    }

    // Fallback to minimal HF scan header if nothing else available
    ok = chameleon_manager_save_last_hf_scan(filename);
    if (ok) {
        glog("Saved minimal HF scan header\n");
    }
    return ok;
}

bool chameleon_manager_has_cached_ntag_dump(void) {
    return g_last_ntag_dump.valid;
}

bool chameleon_manager_has_cached_card_dump(void) {
    return g_last_card_dump.valid;
}

char *chameleon_manager_build_cached_details(void) {
    if (g_cu_mfc_cache.valid) {
        size_t cap = 512; size_t len = 0; char *out = (char*)malloc(cap); if (!out) return NULL; out[0] = '\0';
        char *plugin_text = NULL;
        MFC_TYPE t = g_cu_mfc_cache.type; int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;

        // Try Flipper parsers first (heap allocation to avoid stack overflow)
        MfClassicData* flipper_data = (MfClassicData*)calloc(1, sizeof(MfClassicData));
        if (!flipper_data) goto skip_flipper_parse;

        flipper_data->type = (t == MFC_4K) ? MfClassicType4k : (t == MFC_MINI) ? MfClassicTypeMini : MfClassicType1k;
        if (g_cu_mfc_cache.uid_len > 0) {
            uint8_t ulen = g_cu_mfc_cache.uid_len;
            if (ulen > sizeof(flipper_data->uid)) ulen = (uint8_t)sizeof(flipper_data->uid);
            flipper_data->uid_len = ulen;
            memcpy(flipper_data->uid, g_cu_mfc_cache.uid, ulen);
        }

        // Copy blocks
        if (g_cu_mfc_cache.blocks && g_cu_mfc_cache.known_bits) {
            int max_blocks = g_cu_mfc_cache.total_blocks;
            if (max_blocks > 256) max_blocks = 256;
            for (int b = 0; b < max_blocks; b++) {
                if (cu_bitset_test(g_cu_mfc_cache.known_bits, b)) {
                    memcpy(flipper_data->block[b].data, &g_cu_mfc_cache.blocks[b * 16], 16);
                    flipper_data->block_read_mask[b / 8] |= (1 << (b % 8));
                }
            }
        }
        // Copy keys into synthetic trailers
        for (int s = 0; s < sectors; s++) {
            int trailer_idx = mfc_first_block_of_sector(t, s) + mfc_blocks_in_sector(t, s) - 1;
            if (trailer_idx < 256) {
                bool haveA = g_cu_mfc_cache.key_a_valid && cu_bitset_test(g_cu_mfc_cache.key_a_valid, s);
                bool haveB = g_cu_mfc_cache.key_b_valid && cu_bitset_test(g_cu_mfc_cache.key_b_valid, s);
                if (haveA && g_cu_mfc_cache.key_a) {
                    memcpy(flipper_data->block[trailer_idx].data, &g_cu_mfc_cache.key_a[s * 6], 6);
                } else {
                    memcpy(flipper_data->block[trailer_idx].data, &g_cu_mfc_cache.blocks[trailer_idx * 16], 6);
                }
                flipper_data->block[trailer_idx].data[6] = 0xFF;
                flipper_data->block[trailer_idx].data[7] = 0x07;
                flipper_data->block[trailer_idx].data[8] = 0x80;
                flipper_data->block[trailer_idx].data[9] = 0x69;
                if (haveB && g_cu_mfc_cache.key_b) {
                    memcpy(flipper_data->block[trailer_idx].data + 10, &g_cu_mfc_cache.key_b[s * 6], 6);
                } else {
                    memcpy(flipper_data->block[trailer_idx].data + 10, &g_cu_mfc_cache.blocks[trailer_idx * 16 + 10], 6);
                }
                flipper_data->block_read_mask[trailer_idx / 8] |= (1 << (trailer_idx % 8));
            }
        }

        plugin_text = flipper_nfc_try_parse_mfclassic_from_cache(flipper_data);
        free(flipper_data); // Done with flipper_data
        flipper_data = NULL;

skip_flipper_parse:
        // Header like PN532
        if (!cu_details_append(&out, &cap, &len, "Card: %s | UID:", cu_mfc_type_str(t))) goto fail_out;

        for (uint8_t i = 0; i < g_cu_mfc_cache.uid_len; ++i) {
            if (!cu_details_append(&out, &cap, &len, " %02X", g_cu_mfc_cache.uid[i])) goto fail_out;
        }
        if (!cu_details_append(&out, &cap, &len, "\nATQA: %02X %02X | SAK: %02X\n",
                               (g_cu_mfc_cache.atqa >> 8) & 0xFF,
                               g_cu_mfc_cache.atqa & 0xFF,
                               g_cu_mfc_cache.sak)) goto fail_out;
        // Key and sector counts
        int found_a = 0, found_b = 0, readable = 0;
        for (int s = 0; s < sectors; ++s) {
            if (g_cu_mfc_cache.key_a_valid && cu_bitset_test(g_cu_mfc_cache.key_a_valid, s)) found_a++;
            if (g_cu_mfc_cache.key_b_valid && cu_bitset_test(g_cu_mfc_cache.key_b_valid, s)) found_b++;

            // Consider sector readable if any data block (non-trailer) is known
            int first = mfc_first_block_of_sector(t, s); int blocks = mfc_blocks_in_sector(t, s); if (blocks <= 1) continue;
            bool any_known = false;
            for (int b = 0; b < blocks - 1 && !any_known; ++b) {
                int absb = first + b;
                if (g_cu_mfc_cache.known_bits && cu_bitset_test(g_cu_mfc_cache.known_bits, absb)) any_known = true;
            }
            if (any_known) readable++;
        }
        int keys_total = sectors * 2;
        if (!cu_details_append(&out, &cap, &len, "Keys %d/%d | Sectors %d/%d\n", found_a + found_b, keys_total, readable, sectors)) goto fail_out;
        // Try to locate and summarize NDEF from data blocks (same approach as PN532)
        for (int s = 0; s < sectors; ++s) {
            if (s == 16 && sectors > 16) continue; // skip MAD2 on 4K
            int first = mfc_first_block_of_sector(t, s);
            int blocks = mfc_blocks_in_sector(t, s);

            int data_blocks = blocks - 1; if (data_blocks <= 0) continue;
            size_t sec_bytes = (size_t)data_blocks * 16;
            uint8_t *sec_buf = (uint8_t*)malloc(sec_bytes);
            if (!sec_buf) break;
            size_t woff = 0;
            for (int b = 0; b < data_blocks; ++b) {

                int absb = first + b;
                if (g_cu_mfc_cache.known_bits && cu_bitset_test(g_cu_mfc_cache.known_bits, absb)) memcpy(sec_buf + woff, &g_cu_mfc_cache.blocks[absb * 16], 16);
                else memset(sec_buf + woff, 0, 16);
                woff += 16;
            }
            size_t off = 0, mlen = 0;
            if (ntag_t2_find_ndef(sec_buf, sec_bytes, &off, &mlen) && off < sec_bytes && mlen > 0) {

                // Build contiguous window across subsequent sectors to cover full message
                size_t need = off + mlen; size_t have = sec_bytes; int ss = s + 1;
                while (have < need && ss < sectors) {
                    if (ss == 16 && sectors > 16) { ss++; continue; }
                    int bl2 = mfc_blocks_in_sector(t, ss);
                    have += (size_t)(bl2 - 1) * 16; ss++;
                }
                size_t total_cap = have;
                uint8_t *cat = (uint8_t*)malloc(total_cap);
                if (cat) {
                    memcpy(cat, sec_buf, sec_bytes);
                    size_t cat_off = sec_bytes;
                    for (int s2 = s + 1; cat_off < total_cap && s2 < sectors; ++s2) {
                        if (s2 == 16 && sectors > 16) continue;
                        int f2 = mfc_first_block_of_sector(t, s2);
                        int bl2 = mfc_blocks_in_sector(t, s2);
                        for (int b2 = 0; b2 < bl2 - 1 && cat_off < total_cap; ++b2) {

                            int absb2 = f2 + b2;
                            if (g_cu_mfc_cache.known_bits && cu_bitset_test(g_cu_mfc_cache.known_bits, absb2)) memcpy(cat + cat_off, &g_cu_mfc_cache.blocks[absb2 * 16], 16);
                            else memset(cat + cat_off, 0, 16);
                            cat_off += 16;
                        }
                    }
                    char *ndef_text = ndef_build_details_from_message(cat + off, mlen, g_cu_mfc_cache.uid, g_cu_mfc_cache.uid_len, cu_mfc_type_str(t));
                    if (ndef_text) {

                        // Extract and append first record single-line summary
                        const char *p = strstr(ndef_text, "\nR"); if (!p) { if (ndef_text[0] == 'R') p = ndef_text; } else { p++; }
                        if (p && p[0] == 'R') {
                            const char *colon = strchr(p, ':'); const char *start = NULL; if (colon) { start = colon + 1; if (*start == ' ') start++; }
                            if (start) {

                                const char *end = strchr(start, '\n'); size_t linelen = end ? (size_t)(end - start) : strlen(start);
                                if (!cu_details_append(&out, &cap, &len, "NDEF: ")) { free(ndef_text); free(cat); free(sec_buf); goto fail_out; }
                                if (linelen + 2 > cap - len) { size_t used = len; size_t newcap = (cap * 2) + linelen + 256; char *n = (char*)realloc(out, newcap); if (n) { out = n; cap = newcap; len = used; } }
                                size_t to_copy = (linelen < cap - len - 1) ? linelen : cap - len - 1; memcpy(out + len, start, to_copy); len += to_copy; out[len++] = '\n'; out[len] = '\0';
                            }
                        }
                        free(ndef_text); free(cat); free(sec_buf);
                        goto finalize_out; // same behavior as PN532: return after first found message
                    }
                    free(cat);
                }
            }
            free(sec_buf);
        }

finalize_out:
        if (plugin_text) {
            if (!cu_details_append(&out, &cap, &len, "\n%s", plugin_text)) goto fail_out;
            free(plugin_text);
            plugin_text = NULL;
        }
        return out;

fail_out:
        if (plugin_text) {
            free(plugin_text);
        }
        if (out) {
            free(out);
        }
        return NULL;
    }

    if (g_last_ntag_dump.valid) {
        uint8_t *buf = NULL;
        size_t mem_len = 0;
        if (g_last_ntag_dump.total_pages > 4) {
            mem_len = (size_t)(g_last_ntag_dump.total_pages - 4) * NTAG_PAGE_SIZE;
            buf = (uint8_t*)malloc(mem_len);
            if (buf) {
                memset(buf, 0x00, mem_len);
                for (uint16_t pg = 4; pg < g_last_ntag_dump.total_pages; ++pg) {
                    size_t off = (size_t)(pg - 4) * NTAG_PAGE_SIZE;
                    if (g_last_ntag_dump.page_valid[pg]) memcpy(buf + off, g_last_ntag_dump.pages[pg], NTAG_PAGE_SIZE);
                }
            }
        }
        char *ret = ntag_t2_build_details_from_mem(buf, mem_len,
                                                   g_last_ntag_dump.uid,
                                                   g_last_ntag_dump.uid_size,
                                                   NTAG2XX_UNKNOWN);
        if (buf) free(buf);
        return ret;
    }
    if (g_last_card_dump.valid) {
        size_t cap = 1024;
        char *text = (char*)malloc(cap);
        if (!text) return NULL;
        int len = snprintf(text, cap, "UID:");
        for (uint8_t i = 0; i < g_last_card_dump.uid_size && len < (int)cap - 4; ++i) {
            len += snprintf(text + len, cap - (size_t)len, " %02X", g_last_card_dump.uid[i]);
        }
        len += snprintf(text + len, cap - (size_t)len, "\nCard type: %s\n", g_last_card_dump.tag_type);
        if (len < (int)cap - 32) {
            len += snprintf(text + len, cap - (size_t)len, "Blocks read: %u/%u\n",
                            (unsigned)g_last_card_dump.total_blocks_read,
                            (unsigned)g_last_card_dump.card_size_blocks);
        }
        return text;
    }
    if (g_last_hf_scan.valid) {
        if (desfire_is_desfire_candidate(g_last_hf_scan.atqa, g_last_hf_scan.sak)) {
            return desfire_build_details_summary(NULL,
                                                 g_last_hf_scan.uid,
                                                 g_last_hf_scan.uid_size,
                                                 g_last_hf_scan.atqa,
                                                 g_last_hf_scan.sak);
        }

        size_t cap = 256;
        char *text = (char*)malloc(cap);
        if (!text) return NULL;
        int len = snprintf(text, cap, "UID:");
        for (uint8_t i = 0; i < g_last_hf_scan.uid_size && len < (int)cap - 4; ++i) {
            len += snprintf(text + len, cap - (size_t)len, " %02X", g_last_hf_scan.uid[i]);
        }
        snprintf(text + len, cap - (size_t)len, "\nATQA: %02X %02X | SAK: %02X\n",
                 (g_last_hf_scan.atqa >> 8) & 0xFF,
                 g_last_hf_scan.atqa & 0xFF,
                 g_last_hf_scan.sak);
        return text;
    }
    return NULL;
}

static bool cu_desfire_get_version(desfire_version_t *out) {
    if (!out) return false;
    if (!g_last_hf_scan.valid) return false;
    if (!desfire_is_desfire_candidate(g_last_hf_scan.atqa, g_last_hf_scan.sak)) return false;
    if (!chameleon_manager_is_ready()) return false;

    if (g_cached_hw_mode != HW_MODE_READER) {
        uint8_t mode = HW_MODE_READER;
        if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1)) return false;
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE ||
            !g_response_received ||
            g_last_response.status != STATUS_SUCCESS) {
            return false;
        }
        g_cached_hw_mode = HW_MODE_READER;
    }

    if (g_last_hf_scan.uid_size > 0 && g_last_hf_scan.uid_size <= 10) {
        uint8_t ac_buf[11];
        ac_buf[0] = g_last_hf_scan.uid_size;
        memcpy(&ac_buf[1], g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_response_received = false;
        (void)send_command(CMD_HF14A_SET_ANTI_COLL_DATA, ac_buf, (size_t)g_last_hf_scan.uid_size + 1);
        (void)xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(500));
    }

    memset(out, 0, sizeof(*out));

    uint8_t cmd = 0x60;
    size_t total = 0;

    for (int frame = 0; frame < 3 && total < DESFIRE_PICC_VERSION_MAX; ++frame) {
        g_response_received = false;
        if (!cu_send_hf14a_raw(&cmd, 1,
                               true,
                               true,
                               true,
                               true,
                               true,
                               false,
                               1500,
                               0)) {
            return false;
        }
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE) {
            return false;
        }
        if (!g_response_received ||
            g_last_response.command != CMD_HF14A_RAW ||
            !(g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) ||
            g_last_response.data_size == 0) {
            return false;
        }

        uint8_t copy_len = g_last_response.data_size;
        if (frame == 0 && copy_len < 7) {
            return false;
        }
        if ((size_t)copy_len > (DESFIRE_PICC_VERSION_MAX - total)) {
            copy_len = (uint8_t)(DESFIRE_PICC_VERSION_MAX - total);
        }
        if (copy_len == 0) {
            break;
        }

        memcpy(out->picc_version + total, g_last_response.data, copy_len);
        total += copy_len;

        cmd = 0xAF;
    }

    if (total < 7) {
        return false;
    }

    out->picc_version_len = (uint8_t)total;
    return true;
}

bool chameleon_manager_save_last_hf_scan(const char* filename) {
    if (!g_last_hf_scan.valid) {
        glog("No HF scan data to save\n");
        return false;
    }
    
    // Create filename if not provided (unified under /mnt/ghostesp/nfc)
    char file_path[192];
    if (filename == NULL || !*filename) {
        // Build PN532-style names: Classic{1K|4K|Mini}_<UID>.nfc or NTAG{213|215|216}_<UID>.nfc
        char uid_part[40] = {0};
        int up = 0;
        for (uint8_t i = 0; i < g_last_hf_scan.uid_size && up < (int)sizeof(uid_part) - 3; ++i) {
            up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_last_hf_scan.uid[i]);
            if (i + 1 < g_last_hf_scan.uid_size) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
        }
        const char *prefix = NULL; int pages_total = 0;
        bool is_desfire = desfire_is_desfire_candidate(g_last_hf_scan.atqa, g_last_hf_scan.sak);

        // Classic quick map
        if (g_last_hf_scan.sak == 0x08) { prefix = "Classic1K"; }
        else if (g_last_hf_scan.sak == 0x18) { prefix = "Classic4K"; }
        else if (g_last_hf_scan.sak == 0x09) { prefix = "ClassicMini"; }
        // NTAG: try to detect exact model via GET_VERSION
        if (!prefix && g_last_hf_scan.atqa == 0x0044 && g_last_hf_scan.sak == 0x00) {
            // Best effort refinement
            if (chameleon_manager_detect_ntag()) {
                if (strstr(g_last_hf_scan.tag_type, "NTAG213")) { prefix = "NTAG213"; pages_total = 45; }
                else if (strstr(g_last_hf_scan.tag_type, "NTAG215")) { prefix = "NTAG215"; pages_total = 135; }
                else if (strstr(g_last_hf_scan.tag_type, "NTAG216")) { prefix = "NTAG216"; pages_total = 231; }
                else { prefix = "NTAG"; pages_total = 135; }
            } else {
                prefix = "NTAG"; pages_total = 135;
            }
        }
        if (!prefix) {
            prefix = is_desfire ? "DESFire" : "HF14A";
        }

        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s_%s.nfc", prefix, uid_part);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s", filename);
    }
    
    // Ensure directory exists handled at boot
    
    // Flipper-format minimal header (same style as PN532 saves) — stream to disk to reduce RAM
    bool is_classic = (g_last_hf_scan.sak == 0x08 || g_last_hf_scan.sak == 0x18 || g_last_hf_scan.sak == 0x09);
    bool is_desfire = desfire_is_desfire_candidate(g_last_hf_scan.atqa, g_last_hf_scan.sak);
    const char *ntag_type = NULL; int pages_total = 0;
    if (!is_classic && !is_desfire) {
        if (strstr(g_last_hf_scan.tag_type, "NTAG213")) { ntag_type = "NTAG213"; pages_total = 45; }
        else if (strstr(g_last_hf_scan.tag_type, "NTAG215")) { ntag_type = "NTAG215"; pages_total = 135; }
        else if (strstr(g_last_hf_scan.tag_type, "NTAG216")) { ntag_type = "NTAG216"; pages_total = 231; }
        else { ntag_type = "NTAG"; pages_total = 135; }
    }

    // remount only on specific template; otherwise require SD already mounted
    bool display_was_suspended = false;
    bool did_mount = false;
    const bool require_jit = sd_card_needs_jit_mount();
    if (require_jit) {
        if (!sd_card_manager.is_initialized) {
            esp_err_t mret = sd_card_mount_for_flush(&display_was_suspended);
            did_mount = (mret == ESP_OK);
            if (!did_mount) {
                if (display_was_suspended) sd_card_unmount_after_flush(display_was_suspended);
                glog("Save failed: SD mount_for_flush error\n");
                return false;
            }
        }
    } else if (!sd_card_manager.is_initialized) {
        glog("Save failed: SD not mounted\n");
        return false;
    }
    bool mounted_now = did_mount; // true only when we JIT-mounted above

    // Stream header incrementally
    {
        char line[128]; int n;
        n = snprintf(line, sizeof(line), "Filetype: Flipper NFC device\n");
        if (sd_card_write_file(file_path, line, (size_t)n) != ESP_OK) { glog("Save failed: cannot create %s\n", file_path); if (did_mount) sd_card_unmount_after_flush(display_was_suspended); return false; }
        n = snprintf(line, sizeof(line), "Version: 4\n");
        sd_card_append_file(file_path, line, (size_t)n);
        const char *device_type = is_desfire ? "Mifare DESFire" :
                                  is_classic ? "Mifare Classic" :
                                               "NTAG/Ultralight";
        n = snprintf(line, sizeof(line), "Device type: %s\n", device_type);
        sd_card_append_file(file_path, line, (size_t)n);

        // UID
        n = snprintf(line, sizeof(line), "UID:");
        sd_card_append_file(file_path, line, (size_t)n);
        for (int i = 0; i < g_last_hf_scan.uid_size && i < 20; i++) {
            n = snprintf(line, sizeof(line), " %02X", g_last_hf_scan.uid[i]);
            sd_card_append_file(file_path, line, (size_t)n);
        }
        n = snprintf(line, sizeof(line), "\n");
        sd_card_append_file(file_path, line, (size_t)n);
        // ATQA / SAK
        n = snprintf(line, sizeof(line), "ATQA: %02X %02X\n", (g_last_hf_scan.atqa >> 8) & 0xFF, g_last_hf_scan.atqa & 0xFF);
        sd_card_append_file(file_path, line, (size_t)n);
        n = snprintf(line, sizeof(line), "SAK: %02X\n", g_last_hf_scan.sak);
        sd_card_append_file(file_path, line, (size_t)n);
        if (is_desfire) {
            desfire_version_t ver;
            if (cu_desfire_get_version(&ver) && ver.picc_version_len > 0) {
                char picc_line[128];
                if (desfire_build_picc_version_line(&ver, picc_line, sizeof(picc_line))) {
                    n = snprintf(line, sizeof(line), "%s\n", picc_line);
                    sd_card_append_file(file_path, line, (size_t)n);
                }
            }
        }
        if (!is_desfire) {
            n = snprintf(line, sizeof(line), "Data format version: 2\n");
            sd_card_append_file(file_path, line, (size_t)n);
            // NTAG meta so Saved parser can read it
            if (!is_classic) {
                n = snprintf(line, sizeof(line), "NTAG/Ultralight type: %s\n", ntag_type);
                sd_card_append_file(file_path, line, (size_t)n);
                n = snprintf(line, sizeof(line), "Pages total: %d\nPages read: 0\n", pages_total);
                sd_card_append_file(file_path, line, (size_t)n);
            }
        }
    }

    glog("HF scan saved to: %s\n", file_path);
    if (require_jit && mounted_now) sd_card_unmount_after_flush(display_was_suspended);
    return true;
}

bool chameleon_manager_save_last_lf_scan(const char* filename) {
    if (!g_last_lf_scan.valid) {
        printf("No LF scan data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No LF scan data to save\n");
        return false;
    }
    
    // Create filename if not provided (unified under /mnt/ghostesp/nfc)
    char file_path[192];
    if (filename == NULL) {
        struct tm* time_info = localtime(&g_last_lf_scan.timestamp);
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/CU_lf_scan_%04d%02d%02d_%02d%02d%02d.nfc",
                time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
                time_info->tm_hour, time_info->tm_min, time_info->tm_sec);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s", filename);
    }
    
    // Ensure directory exists (handled at boot; avoid per-save checks)
    bool display_was_suspended = false; bool did_mount = false;
    if (sd_card_needs_jit_mount()) {
        if (!sd_card_manager.is_initialized) {
            did_mount = (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK);
            if (!did_mount) {
                if (display_was_suspended) sd_card_unmount_after_flush(display_was_suspended);
                return false;
            }
        }
    } else if (!sd_card_manager.is_initialized) {
        return false;
    }
    
    // Create scan report content
    char content[1024];
    int len = snprintf(content, sizeof(content),
        "Chameleon Ultra LF Scan Report\n"
        "==============================\n"
        "Timestamp: %s"
        "Tag Type: %s\n"
        "UID Size: %d bytes\n"
        "UID: ",
        ctime(&g_last_lf_scan.timestamp),
        g_last_lf_scan.tag_type,
        g_last_lf_scan.uid_size);
    
    // Add UID
    for (int i = 0; i < g_last_lf_scan.uid_size && i < 20; i++) {
        len += snprintf(content + len, sizeof(content) - len, "%02X ", g_last_lf_scan.uid[i]);
    }
    
    len += snprintf(content + len, sizeof(content) - len, 
        "\nNote: Basic scan information only. For detailed analysis, use specialized tools.\n");
    
    // Write to file
    if (sd_card_write_file(file_path, content, len)) {
        printf("LF scan saved to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("LF scan saved successfully\n");
        if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
        return true;
    } else {
        printf("Failed to save LF scan to: %s\n", file_path);
        TERMINAL_VIEW_ADD_TEXT("Failed to save LF scan\n");
        if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
        return false;
    }
}

bool chameleon_manager_detect_ntag(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    printf("Detecting NTAG card...\n");
    TERMINAL_VIEW_ADD_TEXT("Detecting NTAG card...\n");
    
    // ensure we have recent HF scan; avoid rescanning if already valid
    if (!g_last_hf_scan.valid) {
        if (!chameleon_manager_scan_hf()) {
            printf("No HF card detected\n");
            TERMINAL_VIEW_ADD_TEXT("No HF card detected\n");
            return false;
        }
    }
    
    // Check if the detected card is an NTAG by looking at tag type
    if (strstr(g_last_hf_scan.tag_type, "NTAG") != NULL) {
        printf("NTAG card detected!\n");
        printf("UID: ");
        for (int i = 0; i < g_last_hf_scan.uid_size; i++) {
            printf("%02X", g_last_hf_scan.uid[i]);
        }
        printf("\n");
        
        // Try to get NTAG version to determine exact type
        printf("Reading NTAG version...\n");
        TERMINAL_VIEW_ADD_TEXT("Reading NTAG version...\n");
        
        // ensure reader mode is set before RAW
        if (g_cached_hw_mode != HW_MODE_READER) {
            uint8_t mode = HW_MODE_READER;
            if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1)) return false;
            if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received || g_last_response.status != STATUS_SUCCESS) return false;
            g_cached_hw_mode = HW_MODE_READER;
        }

        // Use HF14A_RAW to send the NTAG GET_VERSION command to the physical card
        uint8_t version_cmd[1] = { NTAG_GET_VERSION_CMD };
        g_response_received = false;
        if (cu_send_hf14a_raw(version_cmd, 1,
                              true,  /* activate_rf */
                              true,  /* keep_rf */
                              true,  /* auto_select */
                              true,  /* append_crc */
                              true,  /* wait_resp */
                              false, /* check_crc */
                              1500,  /* timeout */
                              0)) {  /* bitlen */
            if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE) {
                if (g_response_received && g_last_response.command == CMD_HF14A_RAW && 
                    (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 5) {
                    
                    uint8_t vendor_id = g_last_response.data[0];
                    uint8_t product_type = g_last_response.data[1];
                    uint8_t product_subtype = g_last_response.data[2];
                    uint8_t major_version = g_last_response.data[3];
                    uint8_t minor_version = g_last_response.data[4];
                    
                    printf("Version: %02X %02X %02X %02X %02X\n", 
                           vendor_id, product_type, product_subtype, major_version, minor_version);
                    
                    // Determine NTAG type based on version data
                    char ntag_type[32];
                    int total_pages = 45; // Default to NTAG213
                    
                    if (vendor_id == 0x04 && product_type == 0x04) {
                        if (product_subtype == 0x02) {
                            strcpy(ntag_type, "NTAG213");
                            total_pages = NTAG213_TOTAL_PAGES;
                        } else if (product_subtype == 0x0E) {
                            strcpy(ntag_type, "NTAG215");
                            total_pages = NTAG215_TOTAL_PAGES;
                        } else if (product_subtype == 0x0F) {
                            strcpy(ntag_type, "NTAG216");
                            total_pages = NTAG216_TOTAL_PAGES;
                        } else {
                            snprintf(ntag_type, sizeof(ntag_type), "NTAG (Unknown subtype: %02X)", product_subtype);
                        }
                    } else {
                        snprintf(ntag_type, sizeof(ntag_type), "NTAG (Unknown: %02X %02X)", vendor_id, product_type);
                    }
                    
                    printf("Card Type: %s\n", ntag_type);
                    printf("Memory Size: %d pages (%d bytes)\n", total_pages, total_pages * NTAG_PAGE_SIZE);
                    
                    // Update the stored tag type
                    strncpy(g_last_hf_scan.tag_type, ntag_type, sizeof(g_last_hf_scan.tag_type) - 1);
                    
                    TERMINAL_VIEW_ADD_TEXT("NTAG card detected!\n");
                    TERMINAL_VIEW_ADD_TEXT("Card Type: ");
                    TERMINAL_VIEW_ADD_TEXT(ntag_type);
                    TERMINAL_VIEW_ADD_TEXT("\n");
                    
                    return true;
                } else {
                    printf("Failed to read NTAG version, using basic detection\n");
                    TERMINAL_VIEW_ADD_TEXT("Failed to read NTAG version, using basic detection\n");
                }
            } else {
                printf("Timeout reading NTAG version, using basic detection\n");
                TERMINAL_VIEW_ADD_TEXT("Timeout reading NTAG version, using basic detection\n");
            }
        } else {
            printf("Failed to send version command, using basic detection\n");
            TERMINAL_VIEW_ADD_TEXT("Failed to send version command, using basic detection\n");
        }
        
        // Fallback to basic detection - estimate based on UID length
        int estimated_pages = 45; // Default to NTAG213
        char estimated_type[32] = "NTAG (Unknown)";
        
        if (g_last_hf_scan.uid_size == 4) {
            estimated_pages = 45; // NTAG213
            strcpy(estimated_type, "NTAG213 (estimated)");
        } else if (g_last_hf_scan.uid_size == 7) {
            estimated_pages = 135; // NTAG215
            strcpy(estimated_type, "NTAG215 (estimated)");
        } else {
            estimated_pages = 135; // Default to NTAG215 for other lengths
            strcpy(estimated_type, "NTAG (estimated)");
        }
        
        printf("Card Type: %s\n", estimated_type);
        printf("Memory Size: %d pages (%d bytes) - estimated\n", estimated_pages, estimated_pages * NTAG_PAGE_SIZE);
        
        // Update the stored tag type with our estimate
        strncpy(g_last_hf_scan.tag_type, estimated_type, sizeof(g_last_hf_scan.tag_type) - 1);
        
        TERMINAL_VIEW_ADD_TEXT("NTAG card detected!\n");
        TERMINAL_VIEW_ADD_TEXT("Card Type: ");
        TERMINAL_VIEW_ADD_TEXT(estimated_type);
        TERMINAL_VIEW_ADD_TEXT("\n");
        
        return true;
    } else {
        printf("Card detected but not an NTAG\n");
        printf("Card type: %s\n", g_last_hf_scan.tag_type);
        TERMINAL_VIEW_ADD_TEXT("Card detected but not an NTAG\n");
        TERMINAL_VIEW_ADD_TEXT("Card type: ");
        TERMINAL_VIEW_ADD_TEXT(g_last_hf_scan.tag_type);
        TERMINAL_VIEW_ADD_TEXT("\n");
        return false;
    }
}
    
bool chameleon_manager_ntag_authenticate(uint32_t password) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // NTAG authentication not implemented in simplified version
    printf("NTAG authentication not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("NTAG authentication not implemented\n");
        return false;
    }
    
bool chameleon_manager_read_ntag_card(void) {
    if (!chameleon_manager_is_ready()) {
        printf("Chameleon not ready\n");
        TERMINAL_VIEW_ADD_TEXT("Chameleon not ready\n");
        return false;
    }

    // Ensure we are in reader mode
    if (g_cached_hw_mode != HW_MODE_READER) {
        uint8_t mode = HW_MODE_READER;
        if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1)) return false;
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received || g_last_response.status != STATUS_SUCCESS) return false;
        g_cached_hw_mode = HW_MODE_READER;
    }

    // Pre-select/anticollision data to help some firmwares route RAW to the tag
    if (g_last_hf_scan.valid && g_last_hf_scan.uid_size > 0 && g_last_hf_scan.uid_size <= 10) {
        uint8_t ac_buf[11];
        ac_buf[0] = g_last_hf_scan.uid_size;
        memcpy(&ac_buf[1], g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_response_received = false;
        (void)send_command(CMD_HF14A_SET_ANTI_COLL_DATA, ac_buf, (size_t)g_last_hf_scan.uid_size + 1);
        // wait briefly for ack; ignore failure as some firmwares may not support this
        (void)xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(500));
    }

    // Try GET_VERSION to determine total pages
    int total_pages = NTAG215_TOTAL_PAGES; // default safe fallback
    uint8_t ver_cmd[1] = { NTAG_GET_VERSION_CMD };
    g_response_received = false;
    if (cu_send_hf14a_raw(ver_cmd, 1, true, true, true, true, true, false, 1500, 0)) {
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) == pdTRUE && g_response_received && g_last_response.command == CMD_HF14A_RAW && (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 5) {
            uint8_t product_type = g_last_response.data[1];
            uint8_t product_subtype = g_last_response.data[2];
            (void)product_type;
            if (product_subtype == 0x02) total_pages = NTAG213_TOTAL_PAGES;
            else if (product_subtype == 0x0E) total_pages = NTAG215_TOTAL_PAGES;
            else if (product_subtype == 0x0F) total_pages = NTAG216_TOTAL_PAGES;
        }
    }

    // Clear previous dump
    memset(&g_last_ntag_dump, 0, sizeof(g_last_ntag_dump));
    g_last_ntag_dump.total_pages = total_pages;
    g_last_ntag_dump.readable_pages = 0;
    g_last_ntag_dump.valid = false;

    // Read user memory first (pages >= 4) so NDEF is available early
    for (int page = 4; page < total_pages; page += 4) {
        uint8_t cmd[2] = { NTAG_READ_CMD, (uint8_t)page };
        g_response_received = false;
        if (!cu_send_hf14a_raw(cmd, 2, true, true, true, true, true, false, 2500, 0)) {
            continue;
        }
        // handle firmwares that first reply with 0x60/len=0 before sending the 16-byte read
        bool got = wait_for_cmd_data(CMD_HF14A_RAW, 16, 1800);
        if (got && (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 16) {
            for (int off = 0; off < 4 && (page + off) < total_pages; ++off) {
                memcpy(g_last_ntag_dump.pages[page + off], &g_last_response.data[off * 4], 4);
                g_last_ntag_dump.page_valid[page + off] = true;
                g_last_ntag_dump.readable_pages++;
            }
        }
    }

    // Then read header pages (0..3) last
    for (int page = 0; page < 4 && page < total_pages; page += 4) {
        uint8_t cmd[2] = { NTAG_READ_CMD, (uint8_t)page };
        g_response_received = false;
        if (!cu_send_hf14a_raw(cmd, 2, true, true, true, true, true, false, 2500, 0)) {
            continue;
        }
        bool got = wait_for_cmd_data(CMD_HF14A_RAW, 16, 1800);
        if (got && (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 16) {
            for (int off = 0; off < 4 && (page + off) < total_pages; ++off) {
                memcpy(g_last_ntag_dump.pages[page + off], &g_last_response.data[off * 4], 4);
                g_last_ntag_dump.page_valid[page + off] = true;
                g_last_ntag_dump.readable_pages++;
            }
        }
    }

    // Fill meta
    if (g_last_hf_scan.valid) {
        memcpy(g_last_ntag_dump.uid, g_last_hf_scan.uid, g_last_hf_scan.uid_size);
        g_last_ntag_dump.uid_size = g_last_hf_scan.uid_size;
        snprintf(g_last_ntag_dump.card_type, sizeof(g_last_ntag_dump.card_type), "%s", strstr(g_last_hf_scan.tag_type, "NTAG") ? g_last_hf_scan.tag_type : "NTAG/Ultralight");
    } else {
        snprintf(g_last_ntag_dump.card_type, sizeof(g_last_ntag_dump.card_type), "NTAG/Ultralight");
    }
    g_last_ntag_dump.timestamp = time(NULL);
    g_last_ntag_dump.valid = (g_last_ntag_dump.readable_pages > 0);
    if (g_last_ntag_dump.valid) {
        if (g_cached_details_text) {
            free(g_cached_details_text);
            g_cached_details_text = NULL;
        }
        g_cached_details_text = chameleon_manager_build_cached_details();
        g_cached_details_session++;
    }
    if (g_last_ntag_dump.valid) ghostchi_manager_add_xp(8);
    return g_last_ntag_dump.valid;
}

bool chameleon_manager_save_ntag_dump(const char* filename) {
    if (!g_last_ntag_dump.valid) {
        printf("No NTAG dump data to save\n");
        TERMINAL_VIEW_ADD_TEXT("No NTAG dump data to save\n");
        return false;
    }

    // Build filename using UID like other dumps: <Type>_<UID>.nfc
    char file_path[192];
    if (!filename || !*filename) {
        char uid_part[40] = {0};
        int up = 0;
        for (uint8_t i = 0; i < g_last_ntag_dump.uid_size && up < (int)sizeof(uid_part) - 3; ++i) {
            up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_last_ntag_dump.uid[i]);
            if (i + 1 < g_last_ntag_dump.uid_size) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
        }
        const char *prefix = "NTAG";
        if (strstr(g_last_ntag_dump.card_type, "NTAG213")) prefix = "NTAG213";
        else if (strstr(g_last_ntag_dump.card_type, "NTAG215")) prefix = "NTAG215";
        else if (strstr(g_last_ntag_dump.card_type, "NTAG216")) prefix = "NTAG216";
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s_%s.nfc", prefix, uid_part);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s", filename);
    }

    // JIT mount when required by specific template; otherwise require SD pre-mounted (same policy as HF save)
    bool display_was_suspended = false; bool did_mount = false;
    if (sd_card_needs_jit_mount()) {
        if (!sd_card_manager.is_initialized) {
            did_mount = (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK);
            if (!did_mount) {
                if (display_was_suspended) sd_card_unmount_after_flush(display_was_suspended);
                return false;
            }
        }
    } else if (!sd_card_manager.is_initialized) {
        return false;
    }

    char header[512]; int pos = 0;
    pos += snprintf(header + pos, sizeof(header) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "Version: 4\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "UID:");
    for (int i = 0; i < g_last_ntag_dump.uid_size && i < 10; ++i) pos += snprintf(header + pos, sizeof(header) - pos, " %02X", g_last_ntag_dump.uid[i]);
    pos += snprintf(header + pos, sizeof(header) - pos, "\n");
    if (g_last_hf_scan.valid) {
        pos += snprintf(header + pos, sizeof(header) - pos, "ATQA: %02X %02X\n", (g_last_hf_scan.atqa >> 8) & 0xFF, g_last_hf_scan.atqa & 0xFF);
        pos += snprintf(header + pos, sizeof(header) - pos, "SAK: %02X\n", g_last_hf_scan.sak);
    }
    pos += snprintf(header + pos, sizeof(header) - pos, "Data format version: 2\n");
    pos += snprintf(header + pos, sizeof(header) - pos, "NTAG/Ultralight type: %s\n", g_last_ntag_dump.card_type);
    if (sd_card_write_file(file_path, header, (size_t)pos) != ESP_OK) {
        if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
        return false;
    }

    // Pages meta
    char meta[128];
    int m = snprintf(meta, sizeof(meta), "Pages total: %u\nPages read: %u\n", (unsigned)g_last_ntag_dump.total_pages, (unsigned)g_last_ntag_dump.readable_pages);
    sd_card_append_file(file_path, meta, (size_t)m);

    // Dump pages
    char line[64];
    for (uint16_t pg = 0; pg < g_last_ntag_dump.total_pages; ++pg) {
        uint8_t *p = (uint8_t*)g_last_ntag_dump.pages[pg];
        int lp = snprintf(line, sizeof(line), "Page %u: %02X %02X %02X %02X\n", (unsigned)pg, p[0], p[1], p[2], p[3]);
        sd_card_append_file(file_path, line, (size_t)lp);
    }

    const char *footer = "Failed authentication attempts: 0\n";
    sd_card_append_file(file_path, footer, strlen(footer));
    if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
    printf("NTAG dump saved to: %s\n", file_path);
    TERMINAL_VIEW_ADD_TEXT("NTAG dump saved successfully\n");
    return true;
}

// ---- CU MIFARE Classic dictionary/read/save ----

static inline int cu_hexn(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return 10+(c-'a'); return -1; }
static bool cu_parse_key_line(const char* s,const char* e,uint8_t out[6]){
    uint8_t b[6]; int bi=0; int hi=-1; for(const char* p=s;p<e && bi<6; ++p){ int v=cu_hexn(*p); if(v<0){ if(*p=='#') return false; else continue; } if(hi<0){ hi=v; } else { b[bi++]=(uint8_t)((hi<<4)|v); hi=-1; } }
    if(bi==6){ for(int i=0;i<6;i++) out[i]=b[i]; return true; } return false;
}

static int cu_load_user_keys(uint8_t **keys_out){
    *keys_out = NULL; int count = 0;
    FILE *f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "r");
    if (!f) return 0;
    char line[96]; uint8_t key[6];
    long pos = ftell(f);
    while (fgets(line, sizeof(line), f)) { const char *ls=line; const char *le=line+strlen(line); if (cu_parse_key_line(ls, le, key)) count++; }
    if (count <= 0) { fclose(f); return 0; }
    *keys_out = (uint8_t*)malloc((size_t)count * 6); if (!*keys_out) { fclose(f); return 0; }
    fseek(f, pos, SEEK_SET); int idx = 0;
    while (fgets(line, sizeof(line), f) && idx < count) { const char *ls=line; const char *le=line+strlen(line); if (cu_parse_key_line(ls, le, key)) { memcpy(&(*keys_out)[idx*6], key, 6); idx++; } }
    fclose(f);
    return idx;
}

#ifdef CONFIG_HAS_NFC
static int cu_count_embedded_dict_lines(void){
    const char *s = (const char*)_binary_mf_classic_dict_nfc_start;
    const char *e = (const char*)_binary_mf_classic_dict_nfc_end;
    if (!s || !e || e <= s) return 0;
    int cnt = 0; const char *p = s; uint8_t tmp[6];
    while (p < e) { const char* nl = memchr(p, '\n', (size_t)(e - p)); const char* ln_end = nl ? nl : e; if (cu_parse_key_line(p, ln_end, tmp)) cnt++; p = nl ? nl + 1 : e; }
    return cnt;
}

static int cu_load_embedded_keys(uint8_t **keys_out){
    *keys_out = NULL; int total = cu_count_embedded_dict_lines(); if (total <= 0) return 0;
    *keys_out = (uint8_t*)malloc((size_t)total * 6); if (!*keys_out) return 0;
    const char *s = (const char*)_binary_mf_classic_dict_nfc_start;
    const char *e = (const char*)_binary_mf_classic_dict_nfc_end;
    const char *p = s; int idx = 0; uint8_t key[6];
    while (p < e && idx < total) {
        const char* nl = memchr(p, '\n', (size_t)(e - p)); const char* ln_end = nl ? nl : e;
        if (cu_parse_key_line(p, ln_end, key)) { memcpy(&(*keys_out)[idx * 6], key, 6); idx++; }
        p = nl ? nl + 1 : e;
    }
    return idx;
}
#endif

static const uint8_t CU_DEFAULT_KEYS[][6] = {
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF},
    {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5},
    {0x00,0x00,0x00,0x00,0x00,0x00},
    {0xD3,0xF7,0xD3,0xF7,0xD3,0xF7},
    {0x4D,0x3A,0x99,0xC3,0x51,0xDD},
    {0x1A,0x98,0x2C,0x7E,0x45,0x9A}
};

static bool cu_mf1_auth_one_block(uint8_t block, bool use_key_b, const uint8_t key[6]){
    // CU firmware expects payload: [type, block, key[6]]; type: 0=A, 1=B
    uint8_t payload[1 + 1 + 6];
    payload[0] = use_key_b ? MF_KEY_B : MF_KEY_A;
    payload[1] = block;
    memcpy(&payload[2], key, 6);
    if (!send_command(CMD_MF1_AUTH_ONE_KEY_BLOCK, payload, sizeof(payload))) return false;
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
    if (!g_response_received || g_last_response.command != CMD_MF1_AUTH_ONE_KEY_BLOCK) return false;
    bool ok = (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK);
    if (!ok) {
        // Fallback: some fw builds interpret type as 0/1 instead of 0x60/0x61
        vTaskDelay(pdMS_TO_TICKS(1));
        payload[0] = use_key_b ? 1 : 0;
        if (!send_command(CMD_MF1_AUTH_ONE_KEY_BLOCK, payload, sizeof(payload))) return false;
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(1500)) != pdTRUE) return false;
        if (!g_response_received || g_last_response.command != CMD_MF1_AUTH_ONE_KEY_BLOCK) return false;
        ok = (g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK);
    }
    return ok;
}

static bool cu_mf1_read_one_block(uint8_t block, bool use_key_b, const uint8_t key[6], uint8_t out16[16]){
    // CU firmware expects payload: [type, block, key[6]]; type: 0=A, 1=B
    uint8_t payload[1 + 1 + 6];
    payload[0] = use_key_b ? MF_KEY_B : MF_KEY_A;
    payload[1] = block;
    memcpy(&payload[2], key, 6);
    if (!send_command(CMD_MF1_READ_ONE_BLOCK, payload, sizeof(payload))) return false;
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    if (!g_response_received || g_last_response.command != CMD_MF1_READ_ONE_BLOCK) return false;
    bool ok = ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 16);
    if (!ok) {
        // Fallback type encoding 0/1
        vTaskDelay(pdMS_TO_TICKS(1));
        payload[0] = use_key_b ? 1 : 0;
        if (!send_command(CMD_MF1_READ_ONE_BLOCK, payload, sizeof(payload))) return false;
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
        if (!g_response_received || g_last_response.command != CMD_MF1_READ_ONE_BLOCK) return false;
        ok = ((g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK) && g_last_response.data_size >= 16);
    }
    if (!ok) return false;
    memcpy(out16, g_last_response.data, 16);
    return ok;
}

static bool cu_mf1_check_keys_on_block(uint8_t block, bool use_key_b, const uint8_t *keys6, uint8_t keys_len, uint8_t out_key[6]){
    if (!keys6 || keys_len == 0) return false;
    // payload: [block(1), key_type(1), keys_len(1), keys(keys_len*6)]
    uint8_t hdr[3] = { block, (use_key_b ? MF_KEY_B : MF_KEY_A), keys_len };
    size_t total = sizeof(hdr) + (size_t)keys_len * 6;
    if (total > 3 + 6 * 32) return false;
    uint8_t buf[3 + 6 * 32];
    memcpy(buf, hdr, 3);
    memcpy(buf + 3, keys6, (size_t)keys_len * 6);
    if (!send_command(CMD_MF1_CHECK_KEYS_ON_BLOCK, buf, total)) return false;
    if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
    if (!g_response_received || g_last_response.command != CMD_MF1_CHECK_KEYS_ON_BLOCK) return false;
    if (!(g_last_response.status == STATUS_SUCCESS || g_last_response.status == STATUS_HF_TAG_OK)) return false;
    if (g_last_response.data_size < 1) return false;
    uint8_t found = g_last_response.data[0];
    if (!found) return false;
    if (g_last_response.data_size >= 7 && out_key) memcpy(out_key, &g_last_response.data[1], 6);
    return true;
}

bool chameleon_manager_mf1_has_cache(void){
    return g_cu_mfc_cache.valid;
}

static void cu_progress(int current, int total){ if (g_progress_cb) g_progress_cb(current, total, g_progress_user); }

void chameleon_manager_set_attack_hooks(const mfc_attack_hooks_t *hooks) {
    g_cu_attack_hooks = hooks;
}

bool chameleon_manager_mf1_read_classic_with_dict(bool skip_dict){
    if (!chameleon_manager_is_ready()) return false;
    if (!g_last_hf_scan.valid) { if (!chameleon_manager_scan_hf()) return false; }
    if (!(g_last_hf_scan.sak == 0x08 || g_last_hf_scan.sak == 0x18 || g_last_hf_scan.sak == 0x09)) return false;
    if (g_cached_hw_mode != HW_MODE_READER) {
        uint8_t mode = HW_MODE_READER;
        if (!send_command(CMD_CHANGE_DEVICE_MODE, &mode, 1)) return false;
        if (xSemaphoreTake(g_response_sem, pdMS_TO_TICKS(3000)) != pdTRUE || !g_response_received || g_last_response.status != STATUS_SUCCESS) return false;
        g_cached_hw_mode = HW_MODE_READER;
    }
    MFC_TYPE t = mfc_type_from_sak(g_last_hf_scan.sak);
    cu_mfc_cache_begin(t, g_last_hf_scan.uid, g_last_hf_scan.uid_size, g_last_hf_scan.atqa, g_last_hf_scan.sak);
    cu_sweep_reset();
    cu_try_magic_backdoor_once();
    bool cache_mode_on = false;

    uint8_t *user_keys = NULL; int user_count = 0; bool user_loaded = false;
    uint8_t *emb_keys = NULL; int emb_count = 0; bool emb_loaded = false;

    int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    for (int s = 0; s < sectors; ++s) {
        if (cu_call_should_cancel()) break;
        if (cu_sector_any_key_known(s)) continue;
        int first = mfc_first_block_of_sector(t, s);
        uint8_t auth_blk = cu_auth_target_block(t, s);
        bool authed = false; bool usedB = false; const uint8_t *used_key = NULL;

        int def_count = (int)(sizeof(CU_DEFAULT_KEYS) / 6);
        int total_attempts = def_count * 2; if (total_attempts <= 0) total_attempts = 1;
        int known_attempts = cu_known_key_attempts(t, s);
        if (known_attempts > 0) total_attempts += known_attempts;
        int tried = 0;

        if (g_cu_backdoor_enabled) {
            authed = true;
            tried = total_attempts;
        }

        cu_call_on_phase(s, first, false, total_attempts);
        cu_progress(0, total_attempts);

        // Fast-path: ask firmware to try all default keys at once on trailer block
        if (!authed && def_count > 0) {
            uint8_t found_key[6];
            // Try Key A list quickly
            if (cu_mf1_check_keys_on_block(auth_blk, false, &CU_DEFAULT_KEYS[0][0], (uint8_t)def_count, found_key)) {
                cu_mfc_cache_record_sector_key(s, false, found_key);
                authed = true; usedB = false; used_key = &g_cu_mfc_cache.key_a[s * 6];
                tried += def_count; cu_progress(tried, total_attempts);
            } else if (cu_mf1_check_keys_on_block(auth_blk, true, &CU_DEFAULT_KEYS[0][0], (uint8_t)def_count, found_key)) {
                cu_mfc_cache_record_sector_key(s, true, found_key);
                authed = true; usedB = true; used_key = &g_cu_mfc_cache.key_b[s * 6];
                tried += def_count; cu_progress(tried, total_attempts);
            }
        }

        if (!authed) {
            cu_try_known_keys_first(t, s, auth_blk, &authed, &usedB, &used_key, &tried, &total_attempts);
            if (cu_call_should_cancel()) break;
            if (cu_call_should_skip_dict()) break;
        }

        for (int k = 0; !authed && k < def_count; ++k) {
            if (cu_call_should_cancel()) break;
            if (cu_mf1_auth_one_block(auth_blk, false, CU_DEFAULT_KEYS[k])) { authed = true; usedB = false; used_key = CU_DEFAULT_KEYS[k]; }
            tried++; cu_progress(tried, total_attempts);
            if (!authed) {
                if (cu_mf1_auth_one_block(auth_blk, true,  CU_DEFAULT_KEYS[k])) { authed = true; usedB = true;  used_key = CU_DEFAULT_KEYS[k]; }
                tried++; cu_progress(tried, total_attempts);
            }
            if ((tried & 0x3F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
        }

        if (!authed && !skip_dict) {
            if (!user_loaded) { user_count = cu_load_user_keys(&user_keys); user_loaded = true; total_attempts += (user_count * 2); if (total_attempts <= 0) total_attempts = tried > 0 ? tried : 1; cu_call_on_phase(s, first, false, total_attempts); }
#ifdef CONFIG_HAS_NFC
            if (!emb_loaded) { emb_count = cu_load_embedded_keys(&emb_keys); emb_loaded = true; total_attempts += (emb_count * 2); if (total_attempts <= 0) total_attempts = tried > 0 ? tried : 1; cu_call_on_phase(s, first, false, total_attempts); }
#endif
        }

        if (!authed && user_count > 0 && !skip_dict) {
            for (int i = 0; i < user_count && !authed; ++i) {
                if (cu_call_should_cancel() || cu_call_should_skip_dict()) break;
                if (cu_mf1_auth_one_block(auth_blk, false, &user_keys[i*6])) { authed = true; usedB = false; used_key = &user_keys[i*6]; }
                tried++; cu_progress(tried, total_attempts);
                if (!authed) {
                    if (cu_mf1_auth_one_block(auth_blk, true, &user_keys[i*6]))  { authed = true; usedB = true;  used_key = &user_keys[i*6]; }
                    tried++; cu_progress(tried, total_attempts);
                }
                if ((tried & 0x3F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
                if (authed) break;
            }
        }

#ifdef CONFIG_HAS_NFC
        if (!authed && emb_count > 0 && !skip_dict) {
            for (int i = 0; i < emb_count && !authed; ++i) {
                if (cu_call_should_cancel() || cu_call_should_skip_dict()) break;
                if (cu_mf1_auth_one_block(auth_blk, false, &emb_keys[i*6])) { authed = true; usedB = false; used_key = &emb_keys[i*6]; }
                tried++; cu_progress(tried, total_attempts);
                if (!authed) {
                    if (cu_mf1_auth_one_block(auth_blk, true,  &emb_keys[i*6])) { authed = true; usedB = true;  used_key = &emb_keys[i*6]; }
                    tried++; cu_progress(tried, total_attempts);
                }
                if ((tried & 0x3F) == 0) vTaskDelay(pdMS_TO_TICKS(1));
                if (authed) break;
            }
        }
#endif

        if (authed) {
            if (used_key) {
                cu_mfc_cache_record_sector_key(s, usedB, used_key);
                cu_record_working_key(used_key, usedB);
            }
            if (!cache_mode_on) { cu_call_on_cache_mode(true); cache_mode_on = true; }
            cu_call_on_phase(s, first, usedB, 0);
            cu_read_sector_blocks(t, s);
            if (!skip_dict && user_loaded && user_count > 0) {
                cu_try_find_complementary_user_key(t, s, auth_blk, usedB, user_keys, user_count);
            }
            if (used_key) {
                cu_key_reuse_sweep(t, usedB, used_key, s);
            }

            if (tried > 0 && total_attempts != tried) cu_progress(tried, total_attempts);
            int final_total = (tried > 0) ? tried : total_attempts;
            cu_progress(final_total, final_total);
        }
    }
    if (user_keys) free(user_keys);
    if (emb_keys) free(emb_keys);
    cu_call_on_phase(-1, -1, false, 0);
    g_cu_mfc_cache.valid = true;
    if (cache_mode_on) cu_call_on_cache_mode(false);
    if (g_cached_details_text) { free(g_cached_details_text); g_cached_details_text = NULL; }
    g_cached_details_text = chameleon_manager_build_cached_details();
    g_cached_details_session++;
    return true;
}

bool chameleon_manager_mf1_save_flipper_dump(const char* filename){
    if (!g_cu_mfc_cache.valid) return false;
    char file_path[192];
    if (!filename || !*filename) {
        char uid_part[40] = {0}; int up = 0;
        for (uint8_t i = 0; i < g_cu_mfc_cache.uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
            up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_cu_mfc_cache.uid[i]);
            if (i + 1 < g_cu_mfc_cache.uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
        }
        const char *mtype = "Classic";
        switch (g_cu_mfc_cache.type) { case MFC_MINI: mtype = "ClassicMini"; break; case MFC_1K: mtype = "Classic1K"; break; case MFC_4K: mtype = "Classic4K"; break; default: break; }
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s_%s.nfc", mtype, uid_part);
    } else {
        snprintf(file_path, sizeof(file_path), "/mnt/ghostesp/nfc/%s", filename);
    }

    // ensure sd present (same policy as NTAG save)
    bool display_was_suspended = false; bool did_mount = false;
    if (sd_card_needs_jit_mount()) {
        if (!sd_card_manager.is_initialized) {
            did_mount = (sd_card_mount_for_flush(&display_was_suspended) == ESP_OK);
            if (!did_mount) {
                if (display_was_suspended) sd_card_unmount_after_flush(display_was_suspended);
                return false;
            }
        }
    } else if (!sd_card_manager.is_initialized) {
        return false;
    }

    char buf[256]; int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: Mifare Classic\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < g_cu_mfc_cache.uid_len && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", g_cu_mfc_cache.uid[i]);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (g_cu_mfc_cache.atqa>>8)&0xFF, g_cu_mfc_cache.atqa&0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", g_cu_mfc_cache.sak);
    const char *tstr = (g_cu_mfc_cache.type==MFC_4K?"4K":(g_cu_mfc_cache.type==MFC_1K?"1K":(g_cu_mfc_cache.type==MFC_MINI?"Mini":"Unknown")));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare Classic type: %s\n", tstr);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    if (sd_card_write_file(file_path, buf, (size_t)pos) != ESP_OK) { if (did_mount) sd_card_unmount_after_flush(display_was_suspended); return false; }

    MFC_TYPE t = g_cu_mfc_cache.type; int sectors = mfc_sector_count(t); if (sectors == 0) sectors = 16;
    int total_blocks = 0;
    for (int s = 0; s < sectors; ++s) {
        int blocks = mfc_blocks_in_sector(t, s);
        if (blocks > 0) total_blocks += blocks;
    }
    if (total_blocks <= 0) total_blocks = sectors * 4;
    int known_blocks = 0;
    if (g_cu_mfc_cache.known_bits) {
        for (int b = 0; b < total_blocks; ++b) {
            if (cu_bitset_test(g_cu_mfc_cache.known_bits, b)) known_blocks++;
        }
    }
    int key_slots = sectors * 2;
    int keys_found = 0;
    if (g_cu_mfc_cache.key_a_valid) {
        for (int s = 0; s < sectors; ++s) if (cu_bitset_test(g_cu_mfc_cache.key_a_valid, s)) keys_found++;
    }
    if (g_cu_mfc_cache.key_b_valid) {
        for (int s = 0; s < sectors; ++s) if (cu_bitset_test(g_cu_mfc_cache.key_b_valid, s)) keys_found++;
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Blocks total: %d\n", total_blocks);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Blocks read: %d\n", known_blocks);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Keys found: %d/%d\n", keys_found, key_slots);
    if (sd_card_write_file(file_path, buf, (size_t)pos) != ESP_OK) { if (did_mount) sd_card_unmount_after_flush(display_was_suspended); return false; }

    // Append blocks
    for (int s = 0; s < sectors; ++s) {
        int first = mfc_first_block_of_sector(t, s);
        int blocks = mfc_blocks_in_sector(t, s);
        int trailer = first + blocks - 1;
        for (int b = 0; b < blocks; ++b) {
            int absb = first + b; char line[128]; int lpos = 0;
            lpos += snprintf(line + lpos, sizeof(line) - lpos, "Block %d:", absb);
            if (cu_bitset_test(g_cu_mfc_cache.known_bits, absb)) {
                uint8_t outb[16]; memcpy(outb, &g_cu_mfc_cache.blocks[absb * 16], 16);
                if (absb == trailer) {
                    if (cu_bitset_test(g_cu_mfc_cache.key_a_valid, s)) memcpy(outb + 0, &g_cu_mfc_cache.key_a[s * 6], 6);
                    if (cu_bitset_test(g_cu_mfc_cache.key_b_valid, s)) memcpy(outb + 10, &g_cu_mfc_cache.key_b[s * 6], 6);
                }
                for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " %02X", outb[i]);
            } else {
                for (int i = 0; i < 16 && lpos < (int)sizeof(line) - 4; ++i) lpos += snprintf(line + lpos, sizeof(line) - lpos, " ??");
            }
            lpos += snprintf(line + lpos, sizeof(line) - lpos, "\n");
            if (sd_card_append_file(file_path, line, (size_t)lpos) != ESP_OK) { if (did_mount) sd_card_unmount_after_flush(display_was_suspended); return false; }
        }
    }
    if (did_mount) sd_card_unmount_after_flush(display_was_suspended);
    return true;
}

bool chameleon_manager_test_auth(uint8_t block, uint8_t key_type, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Authentication testing not implemented in simplified version
    printf("Authentication testing not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Authentication testing not implemented\n");
        return false;
    }
    
bool chameleon_manager_test_both_keys(uint8_t block, const char* key_hex) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Both keys testing not implemented in simplified version
    printf("Both keys testing not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Both keys testing not implemented\n");
        return false;
}

bool chameleon_manager_last_scan_is_ntag(void) {
    if (!g_last_hf_scan.valid) return false;
    if (strstr(g_last_hf_scan.tag_type, "NTAG") != NULL) return true;
    if (g_last_hf_scan.atqa == 0x0044 && g_last_hf_scan.sak == 0x00) return true;
    return false;
}

const char *chameleon_manager_get_cached_details(void) {
    if (!g_cached_details_text) {
        char *t = chameleon_manager_build_cached_details();
        if (t) {
            g_cached_details_text = t;
            g_cached_details_session++;
        }
    }
    return g_cached_details_text;
}

uint32_t chameleon_manager_get_cached_details_session(void) {
    return g_cached_details_session;
}

bool chameleon_manager_enable_mfkey32_mode(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // MFKey32 mode not implemented in simplified version
    printf("MFKey32 mode not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("MFKey32 mode not implemented\n");
        return false;
    }
    
bool chameleon_manager_collect_nonces(void) {
    if (!g_is_connected) {
        printf("Not connected to Chameleon Ultra\n");
        TERMINAL_VIEW_ADD_TEXT("Not connected to Chameleon Ultra\n");
        return false;
    }
    
    // Nonce collection not implemented in simplified version
    printf("Nonce collection not implemented in simplified version\n");
    TERMINAL_VIEW_ADD_TEXT("Nonce collection not implemented\n");
        return false;
    }

#endif
