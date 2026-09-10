/**
 * @file gatt_scan.c
 * @brief GATT service enumeration scan implementation
 * 
 * This module handles BLE scanning for connectable devices and GATT service
 * enumeration including:
 * - Starting and stopping GATT device discovery scans
 * - Managing discovered device storage
 * - Listing and selecting discovered devices
 * - Enumerating GATT services on selected devices
 * - Tracking selected devices via RSSI monitoring
 * 
 * Memory Efficiency Strategy:
 * - Lazy allocation: Device array allocated only when first device found
 * - Dynamic service storage: Allocated per-device only when enumerating
 * - Compact structures: Bitfields, packed structs, smaller integer types
 * - String optimization: Short names inline, pool for longer names
 */

#include "scans/ble/gatt_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Configuration Constants
// ============================================================================

#define MAX_GATT_DEVICES 20
#define MAX_GATT_SERVICES_PER_DEVICE 8
#define MAX_CHRS_TO_READ 10
#define GATT_READ_VALUE_MAX 64

// Name storage: short names inline (16 bytes), longer names use pool
#define NAME_INLINE_LEN 16
#define NAME_POOL_SIZE 128  // Pool for names longer than 16 bytes

// Timeout constants (in 100ms ticks)
#define TIMEOUT_CHAR_DISCOVERY 50   // 5 seconds
#define TIMEOUT_READ 20             // 2 seconds
#define TIMEOUT_SVC_DISCOVERY 100   // 10 seconds
#define TIMEOUT_ENCRYPTION 100      // 10 seconds

// ============================================================================
// Data Structures (Memory Optimized)
// ============================================================================

/**
 * @brief Tracker device type enumeration (fits in 3 bits)
 */
typedef enum {
    TRACKER_NONE = 0,
    TRACKER_APPLE_AIRTAG,
    TRACKER_APPLE_FINDMY,
    TRACKER_SAMSUNG_SMARTTAG,
    TRACKER_TILE,
    TRACKER_CHIPOLO,
    TRACKER_GENERIC_FINDMY,
} TrackerType;

/**
 * @brief GATT service structure - packed, only stores what's needed
 * Uses 16-bit UUID for known services, 128-bit only for unknown
 */
typedef struct {
    union {
        uint16_t uuid16;                    // For known 16-bit UUIDs
        uint8_t uuid128[12];                // For unknown 128-bit UUIDs (first 12 bytes only)
    } uuid_data;
    uint16_t start_handle;
    uint16_t end_handle;
    uint8_t uuid_type;                     // BLE_UUID_TYPE_16 or BLE_UUID_TYPE_128
} GattService;  // 6 bytes for 16-bit, 18 bytes for 128-bit

/**
 * @brief GATT device structure - heavily optimized
 * 
 * Memory layout:
 * - addr: 7 bytes (6 MAC + 1 type)
 * - name_inline: 16 bytes (short names stored directly)
 * - rssi: 1 byte
 * - flags: 1 byte (bitfields)
 * - services: 4 bytes (pointer or count)
 * 
 * Total: ~29 bytes for device with short name
 * vs ~48 bytes before = 40% reduction
 */
typedef struct {
    ble_addr_t addr;                        // 7 bytes
    union {
        char inline_name[NAME_INLINE_LEN];  // 16 bytes for short names
        uint8_t name_pool_offset;           // 1 byte index into name pool
    } name_data;
    int8_t rssi;                            // 1 byte
    // Bitfields packed into 1 byte
    TrackerType tracker_type : 3;
    uint8_t name_is_pooled : 1;             // 0 = inline, 1 = in pool
    uint8_t services_count : 4;             // 0-15 services (stored in upper nibble of services_ptr)
    // Services storage: either pointer or inline for small counts
    GattService *services_ptr;              // 4 bytes (NULL if not enumerated)
} GattDevice;  // ~29 bytes

/**
 * @brief Characteristic read entry (packed)
 */
typedef struct __attribute__((packed)) {
    uint16_t handle;
    uint16_t uuid;
} ChrReadEntry;

/**
 * @brief Device tracking state (consolidated)
 */
typedef struct {
    ble_addr_t addr;
    int8_t last_rssi;
    int8_t min_rssi;
    int8_t max_rssi;
    rssi_median_t med;
    int64_t last_rx_us;   // esp_timer timestamp of the last matching advertisement
    bool active;
} TrackingState;

/**
 * @brief Enumeration state flags (consolidated into bitfield struct)
 * Note: svc_discovery_done, encryption_done, chr_discovery_done, and read_done
 * are regular bools (not bit-fields) because their addresses are taken for
 * wait_for_flag().
 */
typedef struct {
    bool svc_discovery_done;    // Regular bool - address taken
    bool encryption_done;       // Regular bool - address taken
    bool chr_discovery_done;    // Regular bool - address taken
    bool read_done;             // Regular bool - address taken
    bool enum_in_progress : 1;
    bool scan_active : 1;
    int8_t encryption_status;   // Separate byte for status code
} EnumState;

// ============================================================================
// Static Variables (Lazy Allocation)
// ============================================================================

// Name pool for long device names
static char g_name_pool[NAME_POOL_SIZE];
static uint8_t g_name_pool_next = 0;

// Discovered GATT devices storage (lazy allocated)
static GattDevice *discovered_gatt_devices = NULL;
static uint8_t discovered_gatt_device_count = 0;
static uint8_t discovered_gatt_device_capacity = 0;

// Selected device index
static int8_t selected_gatt_device_index = -1;

// Connection handle for GATT operations
static uint16_t gatt_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// Enumeration state (consolidated)
static EnumState g_enum_state = {0};

// GATT service discovery state
static uint16_t gatt_read_svc_uuid = 0;

// Characteristic read state (small static buffer)
static ChrReadEntry gatt_chrs_to_read[MAX_CHRS_TO_READ];
static uint8_t gatt_chrs_to_read_count = 0;

// Device tracking state (consolidated)
static TrackingState g_tracking = {0};

// External RGB manager
extern RGBManager_t rgb_manager;

// Forward declarations
static void gatt_scan_callback(struct ble_gap_event *event, size_t len);
static void gatt_track_scan_callback(struct ble_gap_event *event, size_t len);
static const char* gatt_svc_uuid_to_name(uint8_t uuid_type, uint16_t uuid16);

// ============================================================================
// GATT Service UUID Definitions
// ============================================================================

#define GATT_SVC_DEVICE_INFO  0x180A
#define GATT_SVC_BATTERY      0x180F
#define GATT_SVC_CURRENT_TIME 0x1805

#define GATT_CHR_MANUFACTURER  0x2A29
#define GATT_CHR_MODEL_NUMBER  0x2A24
#define GATT_CHR_SERIAL_NUMBER 0x2A25
#define GATT_CHR_FIRMWARE_REV  0x2A26
#define GATT_CHR_HARDWARE_REV  0x2A27
#define GATT_CHR_SOFTWARE_REV  0x2A28
#define GATT_CHR_BATTERY_LEVEL 0x2A19
#define GATT_CHR_CURRENT_TIME  0x2A2B

// ============================================================================
// Memory Management Helpers
// ============================================================================

/**
 * @brief Store a device name, using inline storage or pool
 * @return true if stored successfully
 */
static bool store_device_name(GattDevice *dev, const char *name, size_t name_len) {
    if (name_len == 0 || name[0] == '\0') {
        // Empty name - use inline with placeholder
        dev->name_data.inline_name[0] = '\0';
        dev->name_is_pooled = 0;
        return true;
    }
    
    // Truncate if needed
    if (name_len >= NAME_INLINE_LEN) {
        name_len = NAME_INLINE_LEN - 1;
    }
    
    // Use inline storage for short names
    if (name_len < NAME_INLINE_LEN) {
        memcpy(dev->name_data.inline_name, name, name_len);
        dev->name_data.inline_name[name_len] = '\0';
        dev->name_is_pooled = 0;
        return true;
    }
    
    // Use pool for longer names (if space available)
    size_t pool_needed = name_len + 1;
    if (g_name_pool_next + pool_needed > NAME_POOL_SIZE) {
        // Pool full, truncate to inline
        memcpy(dev->name_data.inline_name, name, NAME_INLINE_LEN - 1);
        dev->name_data.inline_name[NAME_INLINE_LEN - 1] = '\0';
        dev->name_is_pooled = 0;
        return true;
    }
    
    // Store in pool
    memcpy(&g_name_pool[g_name_pool_next], name, name_len);
    g_name_pool[g_name_pool_next + name_len] = '\0';
    dev->name_data.name_pool_offset = g_name_pool_next;
    g_name_pool_next += pool_needed;
    dev->name_is_pooled = 1;
    return true;
}

/**
 * @brief Get device name from storage
 */
static inline const char* get_device_name(const GattDevice *dev) {
    if (dev->name_is_pooled) {
        return &g_name_pool[dev->name_data.name_pool_offset];
    }
    return dev->name_data.inline_name[0] ? dev->name_data.inline_name : "<unknown>";
}

/**
 * @brief Allocate services array for a device (lazy allocation)
 */
static GattService* alloc_device_services(uint8_t count) {
    if (count == 0) return NULL;
    return (GattService*)malloc(count * sizeof(GattService));
}

/**
 * @brief Free device services (called when device is cleared)
 */
static void free_device_services(GattDevice *dev) {
    if (dev->services_ptr) {
        free(dev->services_ptr);
        dev->services_ptr = NULL;
        dev->services_count = 0;
    }
}

// ============================================================================
// Reusable Helper Functions
// ============================================================================

/**
 * @brief Format MAC address to uppercase string (uses shared utility)
 */
static inline void format_addr_mac_upper(const uint8_t *addr, char *buf) {
    format_mac_address(addr, buf, 18, true);
}

/**
 * @brief Convert tracker type to string (lookup table for efficiency)
 */
static const char* tracker_type_to_string(TrackerType type) {
    static const char* const tracker_names[] = {
        [TRACKER_APPLE_AIRTAG]     = "AirTag",
        [TRACKER_APPLE_FINDMY]     = "Apple FindMy",
        [TRACKER_SAMSUNG_SMARTTAG] = "SmartTag",
        [TRACKER_TILE]             = "Tile",
        [TRACKER_CHIPOLO]          = "Chipolo",
        [TRACKER_GENERIC_FINDMY]   = "FindMy Clone",
    };
    
    if (type >= sizeof(tracker_names) / sizeof(tracker_names[0]) || type == TRACKER_NONE) {
        return NULL;
    }
    return tracker_names[type];
}

/**
 * @brief Check if UUID matches any known service
 */
static uint16_t get_known_service_type(uint16_t uuid) {
    switch (uuid) {
        case GATT_SVC_DEVICE_INFO:  return GATT_SVC_DEVICE_INFO;
        case GATT_SVC_BATTERY:      return GATT_SVC_BATTERY;
        case GATT_SVC_CURRENT_TIME: return GATT_SVC_CURRENT_TIME;
        default: return 0;
    }
}

/**
 * @brief Check if characteristic UUID should be read for a service
 */
static bool should_read_chr(uint16_t svc_uuid, uint16_t chr_uuid) {
    switch (svc_uuid) {
        case GATT_SVC_DEVICE_INFO:
            return (chr_uuid == GATT_CHR_MANUFACTURER ||
                    chr_uuid == GATT_CHR_MODEL_NUMBER ||
                    chr_uuid == GATT_CHR_SERIAL_NUMBER ||
                    chr_uuid == GATT_CHR_FIRMWARE_REV ||
                    chr_uuid == GATT_CHR_HARDWARE_REV ||
                    chr_uuid == GATT_CHR_SOFTWARE_REV);
        case GATT_SVC_BATTERY:
            return (chr_uuid == GATT_CHR_BATTERY_LEVEL);
        case GATT_SVC_CURRENT_TIME:
            return (chr_uuid == GATT_CHR_CURRENT_TIME);
        default:
            return false;
    }
}

/**
 * @brief Wait for a flag with timeout (reusable pattern)
 */
static bool wait_for_flag(volatile bool *flag_ptr, uint8_t timeout_ticks) {
    for (uint8_t i = 0; i < timeout_ticks; i++) {
        if (*flag_ptr) return true;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/**
 * @brief Print a single GATT device entry in formatted style
 */
static void print_gatt_device_formatted(uint16_t idx, const GattDevice *dev, const char *flags) {
    char mac_str[18];
    format_addr_mac_upper(dev->addr.val, mac_str);
    const char *name = get_device_name(dev);
    const char *tracker = tracker_type_to_string(dev->tracker_type);
    
    glog("[%u] Name: %s,\n"
         "     MAC: %s,\n"
         "     RSSI: %d,\n",
         idx, name, mac_str, dev->rssi);
    
    if (tracker) {
        glog("     Type: %s,\n", tracker);
    }
    
    if (flags && flags[0]) {
        glog("     Status: %s\n", flags);
    }
}

/**
 * @brief Print service info to log (formatted style)
 */
static void print_service_formatted(uint16_t idx, const GattService *svc) {
    char uuid_str[32];
    
    if (svc->uuid_type == BLE_UUID_TYPE_16) {
        snprintf(uuid_str, sizeof(uuid_str), "0x%04X", svc->uuid_data.uuid16);
    } else {
        snprintf(uuid_str, sizeof(uuid_str), "%02X%02X%02X%02X-...",
                 svc->uuid_data.uuid128[0], svc->uuid_data.uuid128[1],
                 svc->uuid_data.uuid128[2], svc->uuid_data.uuid128[3]);
    }
    
    const char *svc_name = gatt_svc_uuid_to_name(svc->uuid_type, 
                              svc->uuid_type == BLE_UUID_TYPE_16 ? svc->uuid_data.uuid16 : 0);
    
    glog("[%u] Service: %s,\n"
         "     UUID: %s,\n"
         "     Handles: %d-%d\n",
         idx, svc_name ? svc_name : "Unknown", uuid_str,
         svc->start_handle, svc->end_handle);
}

// ============================================================================
// Tracker Detection (Optimized)
// ============================================================================

/**
 * @brief Check name-based tracker detection
 */
static TrackerType detect_tracker_by_name(const char *name) {
    if (!name || !name[0]) return TRACKER_NONE;
    
    switch (name[0]) {
        case 'T': case 't':
            if (strstr(name, "Tile") || strstr(name, "TILE")) return TRACKER_TILE;
            break;
        case 'C': case 'c':
            if (strstr(name, "Chipolo")) return TRACKER_CHIPOLO;
            break;
        case 'S': case 's':
            if (strstr(name, "SmartTag")) return TRACKER_SAMSUNG_SMARTTAG;
            break;
        case 'F': case 'f':
            if (strstr(name, "FindMy") || strstr(name, "Find My")) return TRACKER_GENERIC_FINDMY;
            break;
    }
    return TRACKER_NONE;
}

/**
 * @brief Detect tracker type from BLE advertisement data
 */
static TrackerType detect_tracker_type(const uint8_t *data, size_t len, const char *name) {
    if (!data || len < 4) {
        return detect_tracker_by_name(name);
    }
    
    TrackerType detected = TRACKER_NONE;
    
    for (size_t i = 0; i < len; ) {
        uint8_t field_len = data[i];
        if (field_len == 0 || i + field_len >= len) break;
        
        uint8_t field_type = data[i + 1];
        
        if ((field_type == 0x02 || field_type == 0x03 || field_type == 0x16) && field_len >= 3) {
            uint16_t svc_uuid = data[i + 2] | (data[i + 3] << 8);
            if (svc_uuid == 0xFEED || svc_uuid == 0xFEEC) {
                return TRACKER_TILE;
            }
        }
        
        if (field_type == 0xFF && field_len >= 3) {
            uint16_t company_id = data[i + 2] | (data[i + 3] << 8);
            const uint8_t *mfg_data = &data[i + 4];
            uint8_t mfg_len = field_len - 3;
            
            switch (company_id) {
                case 0x00D8: return TRACKER_TILE;
                case 0x0075: detected = TRACKER_SAMSUNG_SMARTTAG; break;
                case 0x0231: detected = TRACKER_CHIPOLO; break;
                case 0x004C:
                    if (mfg_len >= 3) {
                        uint8_t type_byte = mfg_data[0];
                        uint8_t type_len = mfg_data[1];
                        if (type_byte == 0x12 && type_len == 0x19 && mfg_len >= 25) {
                            detected = TRACKER_APPLE_AIRTAG;
                        } else if (type_byte == 0x07 || type_byte == 0x10) {
                            detected = TRACKER_APPLE_FINDMY;
                        }
                    }
                    break;
                case 0x004F:
                    if (mfg_len >= 2 && mfg_data[0] == 0x12) {
                        detected = TRACKER_GENERIC_FINDMY;
                    }
                    break;
            }
        }
        
        i += field_len + 1;
    }
    
    if (detected != TRACKER_NONE) return detected;
    return detect_tracker_by_name(name);
}

// ============================================================================
// UUID Conversion Functions
// ============================================================================

/**
 * @brief Get human-readable name for a GATT service UUID
 */
static const char* gatt_svc_uuid_to_name(uint8_t uuid_type, uint16_t uuid16) {
    if (uuid_type != BLE_UUID_TYPE_16) return NULL;
    
    static const struct {
        uint16_t uuid;
        const char *name;
    } service_names[] = {
        {0x180A, "Device Information"},
        {0x180F, "Battery Service"},
        {0x1805, "Current Time Service"},
        {0x1800, "Generic Access"},
        {0x1801, "Generic Attribute"},
        {0x1812, "HID Service"},
        {0x1802, "Immediate Alert"},
        {0x1803, "Link Loss"},
        {0x1804, "Tx Power"},
        {0x1811, "Alert Notification"},
        {0xFEE0, "Huawei"}, {0xFEE1, "Huawei"},
        {0xFEE7, "Tencent"}, {0xFEE8, "Xiaomi"},
        {0xFEED, "Tile"}, {0xFEEC, "Tile"},
    };
    
    for (size_t i = 0; i < sizeof(service_names) / sizeof(service_names[0]); i++) {
        if (service_names[i].uuid == uuid16) {
            return service_names[i].name;
        }
    }
    return NULL;
}

// ============================================================================
// GATT Characteristic Decoders
// ============================================================================

/**
 * @brief Decode and print device info characteristic
 */
static void decode_device_info_char(uint16_t chr_uuid, const uint8_t *data, uint16_t len) {
    if (len == 0 || data == NULL) return;
    
    static const struct { uint16_t uuid; const char *label; } chr_labels[] = {
        {GATT_CHR_MANUFACTURER,  "Manufacturer"},
        {GATT_CHR_MODEL_NUMBER,  "Model"},
        {GATT_CHR_SERIAL_NUMBER, "Serial"},
        {GATT_CHR_FIRMWARE_REV,  "Firmware"},
        {GATT_CHR_HARDWARE_REV,  "Hardware"},
        {GATT_CHR_SOFTWARE_REV,  "Software"},
    };
    
    for (size_t i = 0; i < sizeof(chr_labels) / sizeof(chr_labels[0]); i++) {
        if (chr_labels[i].uuid == chr_uuid) {
            char str[48];
            size_t copy_len = (len < sizeof(str) - 1) ? len : sizeof(str) - 1;
            memcpy(str, data, copy_len);
            str[copy_len] = '\0';
            glog("     %s: %s\n", chr_labels[i].label, str);
            return;
        }
    }
}

/**
 * @brief Decode characteristic value based on service type
 */
static void decode_chr_value(uint16_t svc_uuid, uint16_t chr_uuid, 
                             const uint8_t *data, uint16_t len) {
    switch (svc_uuid) {
        case GATT_SVC_DEVICE_INFO:
            decode_device_info_char(chr_uuid, data, len);
            break;
        case GATT_SVC_BATTERY:
            if (chr_uuid == GATT_CHR_BATTERY_LEVEL && len > 0) {
                glog("     Battery: %d%%\n", data[0]);
            }
            break;
        case GATT_SVC_CURRENT_TIME:
            if (chr_uuid == GATT_CHR_CURRENT_TIME && len >= 7) {
                uint16_t year = data[0] | (data[1] << 8);
                glog("     Time: %04d-%02d-%02d %02d:%02d:%02d\n",
                     year, data[2], data[3], data[4], data[5], data[6]);
            }
            break;
    }
}

// ============================================================================
// GATT Callbacks
// ============================================================================

/**
 * @brief Callback for GATT characteristic read operations
 */
static int gatt_read_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
    uint16_t chr_uuid = (uint16_t)(uintptr_t)arg;
    
    if (error->status == 0 && attr != NULL) {
        uint16_t len = OS_MBUF_PKTLEN(attr->om);
        uint16_t copy_len = len > GATT_READ_VALUE_MAX ? GATT_READ_VALUE_MAX : len;
        uint8_t data[GATT_READ_VALUE_MAX];
        if (copy_len > 0) {
            os_mbuf_copydata(attr->om, 0, copy_len, data);
            decode_chr_value(gatt_read_svc_uuid, chr_uuid, data, copy_len);
        }
    } else {
        glog("Read failed for uuid 0x%04x: status=%d\n", chr_uuid, error->status);
    }
    
    g_enum_state.read_done = true;
    return 0;
}

/**
 * @brief Callback for GATT characteristic discovery
 */
static int gatt_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg) {
    if (error->status == 0 && chr != NULL) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            uint16_t uuid16 = chr->uuid.u16.value;
            
            if (should_read_chr(gatt_read_svc_uuid, uuid16) &&
                (chr->properties & BLE_GATT_CHR_PROP_READ)) {
                if (gatt_chrs_to_read_count < MAX_CHRS_TO_READ) {
                    gatt_chrs_to_read[gatt_chrs_to_read_count].handle = chr->val_handle;
                    gatt_chrs_to_read[gatt_chrs_to_read_count].uuid = uuid16;
                    gatt_chrs_to_read_count++;
                }
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        g_enum_state.chr_discovery_done = true;
    }
    return 0;
}

/**
 * @brief Read known services from connected device
 */
static void gatt_read_known_services(uint16_t conn_handle, GattDevice *dev) {
    if (!dev->services_ptr) return;
    
    for (int i = 0; i < dev->services_count; i++) {
        GattService *svc = &dev->services_ptr[i];
        if (svc->uuid_type != BLE_UUID_TYPE_16) continue;
        
        uint16_t svc_uuid = svc->uuid_data.uuid16;
        if (!get_known_service_type(svc_uuid)) continue;
        
        gatt_read_svc_uuid = svc_uuid;
        glog("  Reading %s data:\n", gatt_svc_uuid_to_name(BLE_UUID_TYPE_16, svc_uuid));
        
        gatt_chrs_to_read_count = 0;
        g_enum_state.chr_discovery_done = false;
        
        int rc = ble_gattc_disc_all_chrs(conn_handle, svc->start_handle, svc->end_handle, 
                                gatt_disc_chr_cb, NULL);
        if (rc != 0) {
            glog("Failed to start char discovery: %d\n", rc);
            continue;
        }
        
        if (!wait_for_flag(&g_enum_state.chr_discovery_done, TIMEOUT_CHAR_DISCOVERY)) {
            glog("Char discovery timed out\n");
            continue;
        }
        
        for (int j = 0; j < gatt_chrs_to_read_count; j++) {
            g_enum_state.read_done = false;
            rc = ble_gattc_read(conn_handle, gatt_chrs_to_read[j].handle, 
                              gatt_read_chr_cb, (void*)(uintptr_t)gatt_chrs_to_read[j].uuid);
            
            if (rc == 0) {
                wait_for_flag(&g_enum_state.read_done, TIMEOUT_READ);
            } else {
                glog("Failed to read 0x%04x: %d\n", gatt_chrs_to_read[j].uuid, rc);
            }
            
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

/**
 * @brief Get error message for BLE status codes
 */
static const char* get_ble_error_msg(int status) {
    switch (status) {
        case BLE_HS_ENOTCONN:  return "Disconnected";
        case BLE_HS_ETIMEOUT:  return "Timeout";
        case BLE_HS_EOS:       return "OS error";
        case BLE_HS_ECONTROLLER: return "Controller error";
        case BLE_HS_ENOTSUP:   return "Not supported";
        default:
            if (status >= 0x100 && status < 0x200) return "ATT error (pairing required)";
            return "Unknown";
    }
}

/**
 * @brief Callback for GATT service discovery
 */
static int gatt_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg) {
    if (error->status == 0 && service != NULL) {
        if (selected_gatt_device_index >= 0 && selected_gatt_device_index < discovered_gatt_device_count) {
            GattDevice *dev = &discovered_gatt_devices[selected_gatt_device_index];
            
            // Lazy allocate services array
            if (!dev->services_ptr) {
                dev->services_ptr = alloc_device_services(MAX_GATT_SERVICES_PER_DEVICE);
                if (!dev->services_ptr) return 0;  // Out of memory
            }
            
            if (dev->services_count < MAX_GATT_SERVICES_PER_DEVICE) {
                GattService *svc = &dev->services_ptr[dev->services_count];
                
                // Store UUID efficiently
                svc->uuid_type = service->uuid.u.type;
                if (service->uuid.u.type == BLE_UUID_TYPE_16) {
                    svc->uuid_data.uuid16 = service->uuid.u16.value;
                } else if (service->uuid.u.type == BLE_UUID_TYPE_128) {
                    // Store only first 12 bytes (usually enough for identification)
                    memcpy(svc->uuid_data.uuid128, &service->uuid.u128.value[4], 12);
                }
                svc->start_handle = service->start_handle;
                svc->end_handle = service->end_handle;
                
                char uuid_str[32];
                if (svc->uuid_type == BLE_UUID_TYPE_16) {
                    snprintf(uuid_str, sizeof(uuid_str), "0x%04X", svc->uuid_data.uuid16);
                } else {
                    snprintf(uuid_str, sizeof(uuid_str), "%02X%02X%02X%02X-...",
                             svc->uuid_data.uuid128[0], svc->uuid_data.uuid128[1],
                             svc->uuid_data.uuid128[2], svc->uuid_data.uuid128[3]);
                }
                
                const char *svc_name = gatt_svc_uuid_to_name(svc->uuid_type, 
                    svc->uuid_type == BLE_UUID_TYPE_16 ? svc->uuid_data.uuid16 : 0);
                
                glog("  Service: %s (%s) handles %d-%d\n", 
                     svc_name ? svc_name : "Unknown", uuid_str,
                     svc->start_handle, svc->end_handle);
                
                dev->services_count++;
            }
        }
    } else if (error->status == BLE_HS_EDONE) {
        glog("Service discovery complete. Found %d services.\n", 
             selected_gatt_device_index >= 0 ? 
             discovered_gatt_devices[selected_gatt_device_index].services_count : 0);
        g_enum_state.svc_discovery_done = true;
    } else {
        glog("Service discovery failed: %s (code %d)\n", get_ble_error_msg(error->status), error->status);
        g_enum_state.enum_in_progress = false;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/**
 * @brief GAP event callback for GATT connections
 */
static int gatt_gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            gatt_conn_handle = event->connect.conn_handle;
            glog("Connected! Discovering services...\n");
            int rc = ble_gattc_disc_all_svcs(gatt_conn_handle, gatt_disc_svc_cb, NULL);
            if (rc != 0) {
                glog("Failed to start service discovery: %d\n", rc);
                g_enum_state.enum_in_progress = false;
                ble_gap_terminate(gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            glog("Connection failed: %d\n", event->connect.status);
            g_enum_state.enum_in_progress = false;
            gatt_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        }
        break;
        
    case BLE_GAP_EVENT_DISCONNECT:
        glog("Disconnected.\n");
        gatt_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_enum_state.enum_in_progress = false;
        break;
        
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io pk;
        pk.action = event->passkey.params.action;
        
        switch (event->passkey.params.action) {
        case BLE_SM_IOACT_NUMCMP:
            glog("Numeric comparison: %lu - auto-confirming\n", 
                 (unsigned long)event->passkey.params.numcmp);
            pk.numcmp_accept = 1;
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
            break;
        case BLE_SM_IOACT_INPUT:
            glog("PIN input required - using default 000000\n");
            pk.passkey = 0;
            ble_sm_inject_io(event->passkey.conn_handle, &pk);
            break;
        default:
            break;
        }
        break;
    }
    
    case BLE_GAP_EVENT_ENC_CHANGE:
        g_enum_state.encryption_status = event->enc_change.status;
        g_enum_state.encryption_done = true;
        if (event->enc_change.status == 0) {
            glog("Encryption enabled\n");
        } else {
            glog("Encryption failed: %d\n", event->enc_change.status);
        }
        break;
        
    default:
        break;
    }
    return 0;
}

// ============================================================================
// BLE Scan Callbacks
// ============================================================================

/**
 * @brief BLE callback for GATT device detection during scan
 */
static void gatt_scan_callback(struct ble_gap_event *event, size_t len) {
    if (event->type != BLE_GAP_EVENT_DISC) return;
    
    uint8_t event_type = event->disc.event_type;
    bool connectable = (event_type == BLE_HCI_ADV_RPT_EVTYPE_ADV_IND || 
                        event_type == BLE_HCI_ADV_RPT_EVTYPE_DIR_IND);
    
    if (!connectable) return;
    
    // Check if device already discovered
    for (int i = 0; i < discovered_gatt_device_count; i++) {
        if (memcmp(discovered_gatt_devices[i].addr.val, event->disc.addr.val, 6) == 0) {
            discovered_gatt_devices[i].rssi = event->disc.rssi;
            return;
        }
    }
    
    // Lazy allocate device array
    if (!discovered_gatt_devices) {
        discovered_gatt_device_capacity = MAX_GATT_DEVICES;
        discovered_gatt_devices = (GattDevice *)calloc(discovered_gatt_device_capacity, sizeof(GattDevice));
        if (!discovered_gatt_devices) return;
    }
    
    // Add new device
    if (discovered_gatt_device_count < discovered_gatt_device_capacity) {
        GattDevice *dev = &discovered_gatt_devices[discovered_gatt_device_count];
        memset(dev, 0, sizeof(GattDevice));
        
        memcpy(&dev->addr, &event->disc.addr, sizeof(ble_addr_t));
        dev->rssi = event->disc.rssi;
        
        // Parse and store name efficiently
        char temp_name[33];
        parse_ble_device_name(event->disc.data, event->disc.length_data, temp_name, sizeof(temp_name));
        store_device_name(dev, temp_name, strlen(temp_name));
        
        dev->tracker_type = detect_tracker_type(event->disc.data, event->disc.length_data, get_device_name(dev));
        
        print_gatt_device_formatted(discovered_gatt_device_count, dev, NULL);
        discovered_gatt_device_count++;
    }
}

/**
 * @brief BLE callback for device tracking during scan
 */
static void gatt_track_scan_callback(struct ble_gap_event *event, size_t len) {
    if (event->type != BLE_GAP_EVENT_DISC || !g_tracking.active) return;
    
    if (memcmp(event->disc.addr.val, g_tracking.addr.val, 6) == 0) {
        int8_t rssi = event->disc.rssi;

        if (rssi > g_tracking.max_rssi) g_tracking.max_rssi = rssi;
        if (rssi < g_tracking.min_rssi) g_tracking.min_rssi = rssi;

        rssi_median_push(&g_tracking.med, rssi);
        int8_t med = rssi_median_get(&g_tracking.med);
        int8_t delta = med - g_tracking.last_rssi;
        g_tracking.last_rssi = med;

        const char *direction = (delta > 5) ? "CLOSER" : (delta < -5) ? "FARTHER" : "";

        static const char * const bars[] = {"", "#", "##", "###", "####", "#####"};
        int bar_idx = (med > -50) ? 5 : (med > -60) ? 4 : (med > -70) ? 3 :
                      (med > -80) ? 2 : (med > -90) ? 1 : 0;

        int close_pct = -1;
        if (g_tracking.max_rssi > g_tracking.min_rssi) {
            close_pct = (med - g_tracking.min_rssi) * 100 /
                        (g_tracking.max_rssi - g_tracking.min_rssi);
            if (close_pct < 0) close_pct = 0;
            if (close_pct > 100) close_pct = 100;
        }

        if (close_pct >= 0) {
            glog("[%s] RSSI: %d dBm, Min: %d, Max: %d, Close: %d%%, %s\n",
                 bars[bar_idx], med, g_tracking.min_rssi, g_tracking.max_rssi,
                 close_pct, direction);
        } else {
            glog("[%s] RSSI: %d dBm, Min: %d, Max: %d, %s\n",
                 bars[bar_idx], med, g_tracking.min_rssi, g_tracking.max_rssi, direction);
        }
        g_tracking.last_rx_us = esp_timer_get_time();
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

/**
 * @brief Start scanning for connectable BLE devices
 */
bool gatt_scan_start(void) {
    if (!ble_is_initialized()) {
        ble_init();
    }
    
    // Reset name pool
    g_name_pool_next = 0;
    
    // Free previous results
    if (discovered_gatt_devices) {
        for (int i = 0; i < discovered_gatt_device_count; i++) {
            free_device_services(&discovered_gatt_devices[i]);
        }
        free(discovered_gatt_devices);
        discovered_gatt_devices = NULL;
    }
    discovered_gatt_device_count = 0;
    selected_gatt_device_index = -1;
    
    g_enum_state.enum_in_progress = false;
    g_enum_state.scan_active = true;
    
    glog("GATT Scan started\n");
    glog("Please wait for scan to complete...\n");
    status_display_show_status("GATT Scanning");
    
    if (ble_register_handler(gatt_scan_callback) != ESP_OK) {
        g_enum_state.scan_active = false;
        return false;
    }
    if (!ble_start_scanning()) {
        g_enum_state.scan_active = false;
        ble_unregister_handler(gatt_scan_callback);
        return false;
    }
    return true;
}

/**
 * @brief Stop scanning for BLE devices
 */
void gatt_scan_stop(void) {
    bool was_scanning = g_enum_state.scan_active;
    bool had_connection = gatt_conn_handle != BLE_HS_CONN_HANDLE_NONE;
    bool was_enumerating = g_enum_state.enum_in_progress;
    bool was_tracking = g_tracking.active;

    if (!was_scanning && !had_connection && !was_enumerating && !was_tracking) {
        return;
    }

    g_enum_state.scan_active = false;
    g_enum_state.enum_in_progress = false;
    g_tracking.active = false;
    ble_unregister_handler(gatt_scan_callback);
    ble_unregister_handler(gatt_track_scan_callback);
    
    if (had_connection) {
        ble_gap_terminate(gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        gatt_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    }
    
    // Save results to file
    if (was_scanning && discovered_gatt_devices && discovered_gatt_device_count > 0) {
        scan_file_t sf = SCAN_FILE_INIT;
        if (scan_file_open(&sf, "gatt_scan", "txt") == ESP_OK) {
            scan_file_printf(&sf, "--- GATT Devices (%u) ---\n", discovered_gatt_device_count);
            for (int i = 0; i < discovered_gatt_device_count; i++) {
                const GattDevice *dev = &discovered_gatt_devices[i];
                char mac_str[18];
                format_addr_mac_upper(dev->addr.val, mac_str);
                const char *tracker = tracker_type_to_string(dev->tracker_type);
                
                scan_file_printf(&sf, "[%u] Name: %s, MAC: %s, RSSI: %d",
                                 i, get_device_name(dev), mac_str, dev->rssi);
                if (tracker) scan_file_printf(&sf, ", Type: %s", tracker);
                scan_file_printf(&sf, "\n");
            }
            scan_file_close(&sf);
        }
    }
    
    if (was_scanning) {
        glog("GATT scan stopped. Found %u devices.\n", discovered_gatt_device_count);
        status_display_show_status("GATT Stopped");
    }
}

/**
 * @brief Get the count of discovered BLE devices
 */
int gatt_scan_get_device_count(void) {
    return discovered_gatt_device_count;
}

/**
 * @brief Print the list of discovered BLE devices
 */
void gatt_scan_print_devices(void) {
    if (!discovered_gatt_devices || discovered_gatt_device_count == 0) {
        glog("No GATT devices discovered. Run 'blescan -g' first.\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "gatt_scan", "txt") == ESP_OK);
    
    glog("--- GATT Devices (%u) ---\n", discovered_gatt_device_count);
    if (saving) scan_file_printf(&sf, "--- GATT Devices (%u) ---\n", discovered_gatt_device_count);
    
    for (uint16_t i = 0; i < discovered_gatt_device_count; i++) {
        const GattDevice *dev = &discovered_gatt_devices[i];
        
        char flags[24] = "";
        if ((int8_t)i == selected_gatt_device_index) strcat(flags, "Selected");
        if ((int8_t)i == selected_gatt_device_index && dev->services_ptr) {
            strcat(flags, flags[0] ? ", Enumerated" : "Enumerated");
        }
        
        print_gatt_device_formatted(i, dev, flags);
        
        if (saving) {
            char mac_str[18];
            format_addr_mac_upper(dev->addr.val, mac_str);
            const char *tracker = tracker_type_to_string(dev->tracker_type);
            
            scan_file_printf(&sf, "[%u] Name: %s, MAC: %s, RSSI: %d",
                             i, get_device_name(dev), mac_str, dev->rssi);
            if (tracker) scan_file_printf(&sf, ", Type: %s", tracker);
            if (flags[0]) scan_file_printf(&sf, ", Status: %s", flags);
            scan_file_printf(&sf, "\n");
        }
    }
    if (saving) scan_file_close(&sf);
}

/**
 * @brief Check if a GATT scan is currently active
 */
bool gatt_scan_is_active(void) {
    return g_enum_state.scan_active;
}

/**
 * @brief Select a device for service enumeration
 */
void gatt_scan_select_device(int index) {
    if (!discovered_gatt_devices || index < 0 || index >= discovered_gatt_device_count) {
        glog("Invalid index %d. Use 'listgatt' to see valid indices.\n", index);
        selected_gatt_device_index = -1;
        return;
    }
    
    selected_gatt_device_index = index;
    GattDevice *dev = &discovered_gatt_devices[index];
    
    char mac_str[18];
    format_addr_mac_upper(dev->addr.val, mac_str);
    
    glog("Selected GATT device [%d]:\n"
         "     Name: %s,\n"
         "     MAC: %s\n",
         index, get_device_name(dev), mac_str);
}

/**
 * @brief Enumerate GATT services on the selected device
 */
void gatt_scan_enumerate_services(void) {
    if (!discovered_gatt_devices || selected_gatt_device_index < 0 || 
        selected_gatt_device_index >= discovered_gatt_device_count) {
        glog("No GATT device selected. Use 'selectgatt <index>' first.\n");
        return;
    }
    
    if (g_enum_state.enum_in_progress) {
        glog("Service enumeration already in progress.\n");
        return;
    }
    
    GattDevice *dev = &discovered_gatt_devices[selected_gatt_device_index];
    
    // Check if already enumerated
    if (dev->services_ptr && dev->services_count > 0) {
        glog("Services already enumerated for this device:\n");
        for (uint16_t i = 0; i < dev->services_count; i++) {
            print_service_formatted(i, &dev->services_ptr[i]);
        }
        return;
    }
    
    ble_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (!ble_is_initialized()) {
        ble_init();
    }
    
    if (!ble_wait_for_ready()) {
        glog("BLE stack not ready\n");
        return;
    }
    
    g_enum_state.enum_in_progress = true;
    g_enum_state.svc_discovery_done = false;
    g_enum_state.encryption_done = false;
    g_enum_state.encryption_status = -1;
    
    char mac_str[18];
    format_addr_mac_upper(dev->addr.val, mac_str);
    glog("Connecting to device for service enumeration...\n"
         "     Name: %s,\n"
         "     MAC: %s\n",
         get_device_name(dev), mac_str);
    status_display_show_status("GATT Connect");
    
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        glog("Failed to infer address type: %d\n", rc);
        g_enum_state.enum_in_progress = false;
        return;
    }
    
    rc = ble_gap_connect(own_addr_type, &dev->addr, 10000, NULL, gatt_gap_event_cb, NULL);
    if (rc != 0) {
        glog("Failed to initiate connection: %d\n", rc);
        g_enum_state.enum_in_progress = false;
        return;
    }
    
    if (wait_for_flag(&g_enum_state.svc_discovery_done, TIMEOUT_SVC_DISCOVERY)) {
        if (gatt_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            struct ble_gap_conn_desc conn_desc;
            rc = ble_gap_conn_find(gatt_conn_handle, &conn_desc);
            
            if (rc != 0 || !conn_desc.sec_state.encrypted) {
                glog("Initiating pairing...\n");
                rc = ble_gap_security_initiate(gatt_conn_handle);
                if (rc == 0) {
                    wait_for_flag(&g_enum_state.encryption_done, TIMEOUT_ENCRYPTION);
                }
            }
            
            gatt_read_known_services(gatt_conn_handle, dev);
            ble_gap_terminate(gatt_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    
    g_enum_state.enum_in_progress = false;
}

/**
 * @brief Start tracking the selected device
 */
void gatt_scan_track_device(void) {
    if (selected_gatt_device_index < 0 || selected_gatt_device_index >= discovered_gatt_device_count) {
        glog("No GATT device selected. Use 'selectgatt <index>' first.\n");
        return;
    }
    
    GattDevice *dev = &discovered_gatt_devices[selected_gatt_device_index];
    
    char mac_str[18];
    format_addr_mac_upper(dev->addr.val, mac_str);
    
    const char *tracker_str = tracker_type_to_string(dev->tracker_type);
    
    glog("=== Tracking Device ===\n"
         "     Name: %s,\n"
         "     MAC: %s\n",
         get_device_name(dev), mac_str);
    if (tracker_str) glog("     Type: %s\n", tracker_str);
    glog("Move closer to increase signal. Press back to stop.\n\n");
    
    ble_stop();
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (!ble_is_initialized()) {
        ble_init();
    }
    
    memcpy(&g_tracking.addr, &dev->addr, sizeof(ble_addr_t));
    g_tracking.last_rssi = dev->rssi;
    g_tracking.min_rssi = dev->rssi;
    g_tracking.max_rssi = dev->rssi;
    rssi_median_reset(&g_tracking.med);
    rssi_median_push(&g_tracking.med, dev->rssi);
    g_tracking.last_rx_us = esp_timer_get_time();
    g_tracking.active = true;
    
    status_display_show_status("Tracking...");
    ble_register_handler(gatt_track_scan_callback);
    ble_start_scanning();
}

/**
 * @brief Stop tracking the current device
 */
void gatt_scan_stop_tracking(void) {
    if (g_tracking.active) {
        g_tracking.active = false;
        ble_unregister_handler(gatt_track_scan_callback);
        ble_stop();
        glog("Tracking stopped.\n");
        status_display_show_status("Track Stopped");
    }
}

/**
 * @brief Get live RSSI status for the tracked device
 */
bool gatt_scan_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    if (!g_tracking.active) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = g_tracking.last_rssi;
    }
    if (out_fresh) {
        int64_t now = esp_timer_get_time();
        // Consider the reading "live" if a matching advertisement arrived recently.
        *out_fresh = (g_tracking.last_rx_us != 0) && ((now - g_tracking.last_rx_us) < 1500000);
    }
    return true;
}

/**
 * @brief Get data for a discovered device
 */
int gatt_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi, char *name, size_t name_len) {
    if (index < 0 || index >= discovered_gatt_device_count) return -1;
    
    GattDevice *dev = &discovered_gatt_devices[index];
    
    if (mac) memcpy(mac, dev->addr.val, 6);
    if (rssi) *rssi = dev->rssi;
    if (name && name_len > 0) {
        strncpy(name, get_device_name(dev), name_len - 1);
        name[name_len - 1] = '\0';
    }
    return 0;
}
