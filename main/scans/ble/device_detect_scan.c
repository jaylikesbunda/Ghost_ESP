#include "scans/ble/device_detect_scan.h"

#include <string.h>
#include <stdlib.h>

#include "core/glog.h"
#include "core/utils.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "managers/ble_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/flipper_scan.h"
#include "nimble/ble.h"

#ifdef CONFIG_SPIRAM
#define MAX_BLE_DETECT_DEVICES 64
#else
#define MAX_BLE_DETECT_DEVICES 24
#endif

#define BLE_DETECT_TRACK_LOG_INTERVAL_MS 1500

#define FLIPPER_UUID_WHITE       0x3082
#define FLIPPER_UUID_BLACK       0x3081
#define FLIPPER_UUID_TRANSPARENT 0x3083

// Verified tracker / accessory identifiers.
// Apple/Samsung/Tile company IDs are Bluetooth SIG-assigned. Service UUIDs
// FD59/FD5A (SmartTag), FEED/FEEC (Tile), FE2C (Fast Pair), FDF0 (ASHA),
// FD6F (GAEN) are SIG-assigned member UUIDs. Apple Continuity subtype bytes
// (071901 AirPods, 1005 Watch, 1219 FindMy) are reverse-engineered and
// cross-checked against Theengs Decoder and Flipper Zero ble_spam.
#define APPLE_COMPANY_ID   0x004C
#define SAMSUNG_COMPANY_ID 0x0075
#define TILE_COMPANY_ID_A  0x00D8
#define TILE_COMPANY_ID_B  0x00C7
#define CHIPOLO_COMPANY_ID 0x0231

#define UUID_TILE_FEED    0xFEED
#define UUID_TILE_FEEC    0xFEEC
#define UUID_SAMSUNG_FD59 0xFD59
#define UUID_SAMSUNG_FD5A 0xFD5A
#define UUID_FASTPAIR     0xFE2C
#define UUID_ASHA         0xFDF0
#define UUID_GAEN         0xFD6F

#define BLE_AD_TYPE_16BIT_UUID_COMPLETE  0x03
#define BLE_AD_TYPE_16BIT_UUID_PARTIAL   0x02
#define BLE_AD_TYPE_32BIT_UUID_COMPLETE  0x05
#define BLE_AD_TYPE_32BIT_UUID_PARTIAL   0x04
#define BLE_AD_TYPE_128BIT_UUID_COMPLETE 0x07
#define BLE_AD_TYPE_128BIT_UUID_PARTIAL  0x06

static const char *s_suspicious_skimmer_names[] = {
    "HC-03",
    "HC-05",
    "HC-06",
    "HC-08",
    "BT-HC05",
    "JDY-31",
    "AT-09",
    "HM-10",
    "CC41-A",
    "MLT-BT05",
    "SPP-CA",
    "FFD0",
};

typedef struct {
    ble_addr_t addr;
    BLEDetectDeviceType type;
    int8_t rssi;
    char name[32];
    char subtype[20];
    uint8_t payload[BLE_HS_ADV_MAX_SZ];
    size_t payload_len;
} BLEDetectDevice;

typedef struct {
    bool active;
    ble_addr_t addr;
    BLEDetectDeviceType type;
    TickType_t last_log_tick;
    int8_t last_rssi;
    int64_t last_rx_us;   // esp_timer timestamp of the last matching advertisement
} BLEDetectTrackingState;

static const char *TAG = "BLEDetect";
static BLEDetectDevice *s_devices;
static int s_device_count = 0;
static bool s_scan_active = false;
static BLEDetectTrackingState s_tracking = {0};

extern RGBManager_t rgb_manager;

static const char *flipper_type_from_uuid(uint16_t uuid) {
    switch (uuid) {
    case FLIPPER_UUID_WHITE:
        return "White";
    case FLIPPER_UUID_BLACK:
        return "Black";
    case FLIPPER_UUID_TRANSPARENT:
        return "Transparent";
    default:
        return NULL;
    }
}

static bool is_flipper_uuid(uint16_t value) {
    return value == FLIPPER_UUID_WHITE || value == FLIPPER_UUID_BLACK ||
           value == FLIPPER_UUID_TRANSPARENT;
}

static const char *scan_for_flipper_uuid(const uint8_t *data, size_t len, size_t step) {
    const char *found_type = NULL;

    for (size_t i = 0; i + step <= len; i += step) {
        uint16_t uuid = (step == 2) ? read_u16_le(data + i)
                                    : (uint16_t)(read_u32_le(data + i) & 0xFFFF);

        if (!is_flipper_uuid(uuid)) {
            continue;
        }

        const char *type = flipper_type_from_uuid(uuid);
        if (type == NULL) {
            continue;
        }

        if (found_type == NULL || strcmp(type, "White") == 0 ||
            (strcmp(type, "Transparent") == 0 && strcmp(found_type, "Black") == 0)) {
            found_type = type;
        }
    }

    return found_type;
}

static const char *scan_128bit_for_flipper(const uint8_t *data) {
    for (int i = 0; i <= 14; i++) {
        uint16_t uuid = read_u16_le(data + i);
        if (is_flipper_uuid(uuid)) {
            return flipper_type_from_uuid(uuid);
        }
    }

    return NULL;
}

static const char *detect_flipper_type_from_adv(const uint8_t *data, size_t len) {
    const uint8_t *p = data;
    size_t remaining = len;
    const char *found_type = NULL;

    while (remaining > 1) {
        uint8_t field_len = p[0];
        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }

        uint8_t field_type = p[1];
        const uint8_t *payload = p + 2;
        uint8_t payload_len = (field_len >= 1) ? (uint8_t)(field_len - 1) : 0;
        const char *type = NULL;

        if ((field_type == BLE_AD_TYPE_16BIT_UUID_PARTIAL ||
             field_type == BLE_AD_TYPE_16BIT_UUID_COMPLETE) &&
            payload_len >= 2) {
            type = scan_for_flipper_uuid(payload, payload_len, 2);
        } else if ((field_type == BLE_AD_TYPE_32BIT_UUID_PARTIAL ||
                    field_type == BLE_AD_TYPE_32BIT_UUID_COMPLETE) &&
                   payload_len >= 4) {
            type = scan_for_flipper_uuid(payload, payload_len, 4);
        } else if ((field_type == BLE_AD_TYPE_128BIT_UUID_PARTIAL ||
                    field_type == BLE_AD_TYPE_128BIT_UUID_COMPLETE) &&
                   payload_len >= 16) {
            for (size_t i = 0; i + 16 <= payload_len; i += 16) {
                type = scan_128bit_for_flipper(payload + i);
                if (type != NULL) {
                    break;
                }
            }
        }

        if (type != NULL &&
            (found_type == NULL || strcmp(type, "White") == 0 ||
             (strcmp(type, "Transparent") == 0 && strcmp(found_type, "Black") == 0))) {
            found_type = type;
        }

        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }

    return found_type;
}

static bool is_airtag_pattern(const uint8_t *payload, size_t len) {
    if (payload == NULL || len < 4) {
        return false;
    }

    for (size_t i = 0; i <= len - 4; i++) {
        if ((payload[i] == 0x1E && payload[i + 1] == 0xFF && payload[i + 2] == 0x4C &&
             payload[i + 3] == 0x00) ||
            (payload[i] == 0x4C && payload[i + 1] == 0x00 && payload[i + 2] == 0x12 &&
             payload[i + 3] == 0x19)) {
            return true;
        }
    }

    return false;
}

static bool adv_has_uuid16(const uint8_t *data, size_t len, uint16_t uuid) {
    if (data == NULL || len < 2) {
        return false;
    }
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 1) {
        uint8_t field_len = p[0];
        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }
        uint8_t field_type = p[1];
        const uint8_t *payload = p + 2;
        size_t payload_len = (size_t)(field_len - 1);
        if (field_type == BLE_AD_TYPE_16BIT_UUID_PARTIAL ||
            field_type == BLE_AD_TYPE_16BIT_UUID_COMPLETE) {
            for (size_t i = 0; i + 2 <= payload_len; i += 2) {
                if (read_u16_le(payload + i) == uuid) {
                    return true;
                }
            }
        } else if (field_type == BLE_AD_TYPE_SERVICE_DATA_16BIT && payload_len >= 2) {
            if (read_u16_le(payload) == uuid) {
                return true;
            }
        }
        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }
    return false;
}

static bool adv_mfg_match(const uint8_t *data, size_t len, uint16_t company,
                          const uint8_t *prefix, size_t prefix_len) {
    if (data == NULL || prefix == NULL || prefix_len == 0) {
        return false;
    }
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 1) {
        uint8_t field_len = p[0];
        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }
        if (p[1] == 0xFF && field_len >= 3) {
            uint16_t cid = read_u16_le(p + 2);
            size_t mfg_len = (size_t)(field_len - 3);
            const uint8_t *mfg = p + 4;
            if (cid == company && mfg_len >= prefix_len && memcmp(mfg, prefix, prefix_len) == 0) {
                return true;
            }
        }
        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }
    return false;
}

static bool adv_has_company(const uint8_t *data, size_t len, uint16_t company) {
    if (data == NULL) {
        return false;
    }
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 1) {
        uint8_t field_len = p[0];
        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }
        if (p[1] == 0xFF && field_len >= 3 && read_u16_le(p + 2) == company) {
            return true;
        }
        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }
    return false;
}

// Apple Continuity subtype prefixes (after the 0x4C00 company ID).
static const uint8_t s_apple_airpods_prefix[] = {0x07, 0x19, 0x01};
static const uint8_t s_apple_watch_prefix[] = {0x10, 0x05};
// Generic (non-Apple-vendor) FindMy-compatible beacon prefix.
static const uint8_t s_generic_findmy_prefix[] = {0x12};
#define GENERIC_FINDMY_COMPANY_ID 0x004F

static const struct {
    uint16_t id;
    const char *name;
} s_airpods_models[] = {
    {0x0220, "AirPods 1st gen"},
    {0x0F20, "AirPods 2nd gen"},
    {0x1320, "AirPods 3rd gen"},
    {0x0E20, "AirPods Pro"},
    {0x1420, "AirPods Pro 2"},
    {0x2420, "AirPods Pro 2 USB-C"},
    {0x0A20, "AirPods Max"},
    {0x0620, "Beats Solo3"},
    {0x0520, "BeatsX"},
};

static const char *airpods_model_name(const uint8_t *data, size_t len) {
    if (data == NULL) {
        return "AirPods";
    }
    const uint8_t *p = data;
    size_t remaining = len;
    while (remaining > 1) {
        uint8_t field_len = p[0];
        if (field_len == 0 || (size_t)(field_len + 1) > remaining) {
            break;
        }
        if (p[1] == 0xFF && field_len >= 8 && read_u16_le(p + 2) == APPLE_COMPANY_ID &&
            p[4] == 0x07 && p[5] == 0x19 && p[6] == 0x01) {
            uint16_t model = read_u16_le(p + 7);
            for (size_t i = 0; i < sizeof(s_airpods_models) / sizeof(s_airpods_models[0]); i++) {
                if (s_airpods_models[i].id == model) {
                    return s_airpods_models[i].name;
                }
            }
            return "AirPods";
        }
        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }
    return "AirPods";
}

static bool is_suspicious_skimmer_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < sizeof(s_suspicious_skimmer_names) / sizeof(s_suspicious_skimmer_names[0]); i++) {
        if (strcasecmp(name, s_suspicious_skimmer_names[i]) == 0) {
            return true;
        }
    }

    return false;
}

const char *ble_device_detect_type_to_string(BLEDetectDeviceType type) {
    switch (type) {
    case BLE_DETECT_DEVICE_AIRTAG:
        return "AirTag";
    case BLE_DETECT_DEVICE_FLIPPER:
        return "Flipper";
    case BLE_DETECT_DEVICE_SKIMMER:
        return "Skimmer Suspect";
    case BLE_DETECT_DEVICE_TILE:
        return "Tile";
    case BLE_DETECT_DEVICE_SMARTTAG:
        return "SmartTag";
    case BLE_DETECT_DEVICE_CHIPOLO:
        return "Chipolo";
    case BLE_DETECT_DEVICE_AIRPODS:
        return "AirPods";
    case BLE_DETECT_DEVICE_APPLE_WATCH:
        return "Apple Watch";
    case BLE_DETECT_DEVICE_FINDMY:
        return "FindMy";
    case BLE_DETECT_DEVICE_FASTPAIR:
        return "Fast Pair";
    case BLE_DETECT_DEVICE_HEARING_AID:
        return "Hearing Aid";
    case BLE_DETECT_DEVICE_GAEN:
        return "Exposure Beacon";
    case BLE_DETECT_DEVICE_CHAMELEON:
        return "Chameleon";
    default:
        return "BLE Device";
    }
}

static int find_device_index(const ble_addr_t *addr, BLEDetectDeviceType type) {
    for (int i = 0; i < s_device_count; i++) {
        if (s_devices[i].type == type && s_devices[i].addr.type == addr->type &&
            memcmp(s_devices[i].addr.val, addr->val, sizeof(addr->val)) == 0) {
            return i;
        }
    }

    return -1;
}

static void log_tracking_update(const BLEDetectDevice *device) {
    TickType_t now = xTaskGetTickCount();
    if (s_tracking.last_log_tick != 0 &&
        (now - s_tracking.last_log_tick) < pdMS_TO_TICKS(BLE_DETECT_TRACK_LOG_INTERVAL_MS)) {
        return;
    }

    char mac[18];
    format_mac_address(device->addr.val, mac, sizeof(mac), false);

    glog("Tracking %s: RSSI %d dBm (%s)\n"
         "     MAC: %s\n",
         ble_device_detect_type_to_string(device->type), device->rssi,
         rssi_to_proximity(device->rssi), mac);

    s_tracking.last_log_tick = now;
}

static bool is_tracking_addr(const ble_addr_t *addr) {
    return s_tracking.active && addr != NULL && s_tracking.addr.type == addr->type &&
           memcmp(s_tracking.addr.val, addr->val, sizeof(addr->val)) == 0;
}

static void ble_device_detect_callback(struct ble_gap_event *event, size_t len) {
    (void)len;

    if (!s_scan_active || event == NULL || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    /* While tracking, update RSSI for the tracked MAC unconditionally (like
     * advertiser_scan) and ignore all other advertisements to keep the list
     * stable under the RSSI meter overlay. */
    if (s_tracking.active) {
        if (!is_tracking_addr(&event->disc.addr)) {
            return;
        }
        s_tracking.last_rssi = event->disc.rssi;
        s_tracking.last_rx_us = esp_timer_get_time();
        /* Keep the stored device entry in sync so the list shows latest RSSI. */
        for (int i = 0; i < s_device_count; i++) {
            if (s_devices[i].addr.type == event->disc.addr.type &&
                memcmp(s_devices[i].addr.val, event->disc.addr.val, sizeof(event->disc.addr.val)) == 0) {
                s_devices[i].rssi = event->disc.rssi;
                if (s_devices[i].type == BLE_DETECT_DEVICE_AIRTAG) {
                    char mac[18];
                    format_mac_address(s_devices[i].addr.val, mac, sizeof(mac), false);
                    glog("Tracking AirTag: RSSI %d dBm (%s)\n"
                         "     MAC: %s\n",
                         s_devices[i].rssi, rssi_to_proximity(s_devices[i].rssi), mac);
                } else {
                    log_tracking_update(&s_devices[i]);
                }
                break;
            }
        }
        return;
    }

    BLEDetectDeviceType detected_type = BLE_DETECT_DEVICE_UNKNOWN;
    const char *detected_subtype = NULL;
    char adv_name[32] = {0};

    parse_ble_device_name(event->disc.data, event->disc.length_data, adv_name, sizeof(adv_name));
    detected_subtype = detect_flipper_type_from_adv(event->disc.data, event->disc.length_data);
    if (detected_subtype != NULL) {
        detected_type = BLE_DETECT_DEVICE_FLIPPER;
    } else if (adv_mfg_match(event->disc.data, event->disc.length_data, APPLE_COMPANY_ID,
                             s_apple_airpods_prefix, sizeof(s_apple_airpods_prefix))) {
        detected_type = BLE_DETECT_DEVICE_AIRPODS;
        detected_subtype = airpods_model_name(event->disc.data, event->disc.length_data);
    } else if (adv_mfg_match(event->disc.data, event->disc.length_data, APPLE_COMPANY_ID,
                             s_apple_watch_prefix, sizeof(s_apple_watch_prefix))) {
        detected_type = BLE_DETECT_DEVICE_APPLE_WATCH;
        detected_subtype = "Watch";
    } else if (is_airtag_pattern(event->disc.data, event->disc.length_data)) {
        detected_type = BLE_DETECT_DEVICE_AIRTAG;
    } else if (adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_TILE_FEED) ||
               adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_TILE_FEEC) ||
               adv_has_company(event->disc.data, event->disc.length_data, TILE_COMPANY_ID_A) ||
               adv_has_company(event->disc.data, event->disc.length_data, TILE_COMPANY_ID_B) ||
               (adv_name[0] != '\0' && strstr(adv_name, "Tile") != NULL)) {
        detected_type = BLE_DETECT_DEVICE_TILE;
        detected_subtype = "Tracker";
    } else if (adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_SAMSUNG_FD59) ||
               adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_SAMSUNG_FD5A) ||
               (adv_name[0] != '\0' && (strstr(adv_name, "SmartTag") != NULL ||
                                        strstr(adv_name, "Smart Tag") != NULL))) {
        detected_type = BLE_DETECT_DEVICE_SMARTTAG;
        detected_subtype = "SmartTag";
    } else if (adv_has_company(event->disc.data, event->disc.length_data, CHIPOLO_COMPANY_ID) ||
               (adv_name[0] != '\0' && strstr(adv_name, "Chipolo") != NULL)) {
        detected_type = BLE_DETECT_DEVICE_CHIPOLO;
        detected_subtype = "Spot";
    } else if (adv_mfg_match(event->disc.data, event->disc.length_data, GENERIC_FINDMY_COMPANY_ID,
                             s_generic_findmy_prefix, sizeof(s_generic_findmy_prefix))) {
        detected_type = BLE_DETECT_DEVICE_FINDMY;
        detected_subtype = "Clone";
    } else if (adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_FASTPAIR)) {
        detected_type = BLE_DETECT_DEVICE_FASTPAIR;
        detected_subtype = "Fast Pair";
    } else if (adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_ASHA)) {
        detected_type = BLE_DETECT_DEVICE_HEARING_AID;
        detected_subtype = "ASHA";
    } else if (adv_has_uuid16(event->disc.data, event->disc.length_data, UUID_GAEN)) {
        detected_type = BLE_DETECT_DEVICE_GAEN;
        detected_subtype = "Exposure";
    } else if (adv_name[0] != '\0' && strstr(adv_name, "ChameleonUltra") != NULL) {
        detected_type = BLE_DETECT_DEVICE_CHAMELEON;
        detected_subtype = "Ultra";
    } else if (is_suspicious_skimmer_name(adv_name)) {
        detected_type = BLE_DETECT_DEVICE_SKIMMER;
        detected_subtype = "Name Match";
    } else {
        return;
    }

    int index = find_device_index(&event->disc.addr, detected_type);

    if (index >= 0) {
        BLEDetectDevice *existing = &s_devices[index];
        existing->rssi = event->disc.rssi;
        if (adv_name[0] != '\0') {
            strncpy(existing->name, adv_name, sizeof(existing->name) - 1);
            existing->name[sizeof(existing->name) - 1] = '\0';
        }
        if (detected_subtype != NULL) {
            strncpy(existing->subtype, detected_subtype, sizeof(existing->subtype) - 1);
            existing->subtype[sizeof(existing->subtype) - 1] = '\0';
        }
        if (event->disc.length_data > 0) {
            existing->payload_len = (event->disc.length_data > BLE_HS_ADV_MAX_SZ)
                                        ? BLE_HS_ADV_MAX_SZ
                                        : event->disc.length_data;
            memcpy(existing->payload, event->disc.data, existing->payload_len);
        }

        if (s_tracking.active && existing->type == s_tracking.type &&
            existing->addr.type == s_tracking.addr.type &&
            memcmp(existing->addr.val, s_tracking.addr.val, sizeof(existing->addr.val)) == 0) {
            s_tracking.last_rssi = existing->rssi;
            s_tracking.last_rx_us = esp_timer_get_time();
            if (existing->type == BLE_DETECT_DEVICE_AIRTAG) {
                char mac[18];
                format_mac_address(existing->addr.val, mac, sizeof(mac), false);
                glog("Tracking AirTag: RSSI %d dBm (%s)\n"
                     "     MAC: %s\n",
                     existing->rssi, rssi_to_proximity(existing->rssi), mac);
            } else {
                log_tracking_update(existing);
            }
        }
        return;
    }

    if (s_device_count >= MAX_BLE_DETECT_DEVICES) {
        return;
    }

    BLEDetectDevice *device = &s_devices[s_device_count++];
    memset(device, 0, sizeof(*device));
    memcpy(&device->addr, &event->disc.addr, sizeof(device->addr));
    device->type = detected_type;
    device->rssi = event->disc.rssi;

    if (adv_name[0] != '\0') {
        strncpy(device->name, adv_name, sizeof(device->name) - 1);
        device->name[sizeof(device->name) - 1] = '\0';
    }
    if (detected_subtype != NULL) {
        strncpy(device->subtype, detected_subtype, sizeof(device->subtype) - 1);
        device->subtype[sizeof(device->subtype) - 1] = '\0';
    }
    device->payload_len = (event->disc.length_data > BLE_HS_ADV_MAX_SZ)
                              ? BLE_HS_ADV_MAX_SZ
                              : event->disc.length_data;
    if (device->payload_len > 0) {
        memcpy(device->payload, event->disc.data, device->payload_len);
    }

    char mac[18];
    format_mac_address(device->addr.val, mac, sizeof(mac), false);
    glog("[%d] %s found\n"
         "     MAC: %s\n"
         "     RSSI: %d dBm (%s)\n",
         s_device_count - 1, ble_device_detect_type_to_string(device->type), mac, device->rssi,
         rssi_to_proximity(device->rssi));
    if (device->name[0] != '\0') {
        glog("     Name: %s\n", device->name);
    }
    if (device->subtype[0] != '\0') {
        glog("     Variant: %s\n", device->subtype);
    }

    // Keep the NimBLE callback path non-blocking so scan stop/deinit can complete promptly.
    if (device->type == BLE_DETECT_DEVICE_FLIPPER) {
        rgb_manager_pulse_async(&rgb_manager, 255, 165, 0);
    } else if (device->type == BLE_DETECT_DEVICE_AIRTAG) {
        rgb_manager_pulse_async(&rgb_manager, 0, 0, 255);
    } else if (device->type == BLE_DETECT_DEVICE_SKIMMER) {
        rgb_manager_pulse_async(&rgb_manager, 255, 0, 0);
    } else if (device->type == BLE_DETECT_DEVICE_TILE ||
               device->type == BLE_DETECT_DEVICE_SMARTTAG ||
               device->type == BLE_DETECT_DEVICE_CHIPOLO ||
               device->type == BLE_DETECT_DEVICE_FINDMY) {
        rgb_manager_pulse_async(&rgb_manager, 0, 255, 255);
    } else if (device->type == BLE_DETECT_DEVICE_AIRPODS ||
               device->type == BLE_DETECT_DEVICE_APPLE_WATCH) {
        rgb_manager_pulse_async(&rgb_manager, 255, 255, 255);
    } else if (device->type == BLE_DETECT_DEVICE_FASTPAIR ||
               device->type == BLE_DETECT_DEVICE_HEARING_AID ||
               device->type == BLE_DETECT_DEVICE_GAEN) {
        rgb_manager_pulse_async(&rgb_manager, 0, 255, 0);
    } else if (device->type == BLE_DETECT_DEVICE_CHAMELEON) {
        rgb_manager_pulse_async(&rgb_manager, 255, 0, 255);
    }

    if (s_tracking.active && device->type == s_tracking.type &&
        device->addr.type == s_tracking.addr.type &&
        memcmp(device->addr.val, s_tracking.addr.val, sizeof(device->addr.val)) == 0) {
        s_tracking.last_rssi = device->rssi;
        s_tracking.last_rx_us = esp_timer_get_time();
        log_tracking_update(device);
    }
}

void ble_device_detect_start(void) {
    free(s_devices);
    s_devices = calloc(MAX_BLE_DETECT_DEVICES, sizeof(*s_devices));
    s_device_count = 0;
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
    s_tracking.last_rx_us = 0;

    if (!ble_is_initialized()) {
        ble_init();
    }

    if (!ble_wait_for_ready()) {
        ESP_LOGE(TAG, "BLE stack not ready for detect scan");
        status_display_show_status("BLE Not Ready");
        free(s_devices);
        s_devices = NULL;
        return;
    }

    if (s_devices == NULL) {
        status_display_show_status("BLE Detect No Mem");
        return;
    }

    ble_unregister_handler(ble_device_detect_callback);
    ble_register_handler(ble_device_detect_callback);
    s_scan_active = true;

    if (!ble_start_scanning()) {
        s_scan_active = false;
        ble_unregister_handler(ble_device_detect_callback);
        free(s_devices);
        s_devices = NULL;
        status_display_show_status("BLE Scan Fail");
        return;
    }

    glog("BLE device detection started\n");
    status_display_show_status("BLE Detect On");
}

void ble_device_detect_clear_results(void) {
    if (s_scan_active || s_tracking.active) return;
    free(s_devices);
    s_devices = NULL;
    s_device_count = 0;
}

void ble_device_detect_stop(void) {
    bool was_active = s_scan_active || s_tracking.active;

    s_scan_active = false;
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
    s_tracking.last_rx_us = 0;
    ble_unregister_handler(ble_device_detect_callback);

    if (was_active) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    }

    if (was_active && ble_is_initialized()) {
        ble_stop();
    }
}

bool ble_device_detect_is_active(void) {
    return s_scan_active;
}

int ble_device_detect_get_count(void) {
    return s_device_count;
}

int ble_device_detect_get_device(int index, BLEDetectDeviceInfo *out_info) {
    if (s_devices == NULL || out_info == NULL || index < 0 || index >= s_device_count) {
        return -1;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->type = s_devices[index].type;
    out_info->rssi = s_devices[index].rssi;
    out_info->tracking = s_tracking.active && s_devices[index].type == s_tracking.type &&
                         s_devices[index].addr.type == s_tracking.addr.type &&
                         memcmp(s_devices[index].addr.val, s_tracking.addr.val,
                                sizeof(s_devices[index].addr.val)) == 0;
    memcpy(out_info->mac, s_devices[index].addr.val, sizeof(out_info->mac));
    memcpy(out_info->name, s_devices[index].name, sizeof(out_info->name));
    memcpy(out_info->subtype, s_devices[index].subtype, sizeof(out_info->subtype));
    return 0;
}

bool ble_device_detect_start_tracking(int index) {
    if (index < 0 || index >= s_device_count) {
        glog("Invalid BLE device index %d\n", index);
        return false;
    }

    BLEDetectDevice dev;
    memcpy(&dev, &s_devices[index], sizeof(dev));

    if (!s_scan_active) {
        if (!ble_is_initialized()) {
            ble_init();
        }
        if (!ble_wait_for_ready()) {
            ESP_LOGE(TAG, "BLE stack not ready for detect scan");
            return false;
        }
        ble_unregister_handler(ble_device_detect_callback);
        ble_register_handler(ble_device_detect_callback);
        if (!ble_start_scanning()) {
            ble_unregister_handler(ble_device_detect_callback);
            return false;
        }
        s_scan_active = true;
    }

    s_tracking.active = true;
    s_tracking.type = dev.type;
    memcpy(&s_tracking.addr, &dev.addr, sizeof(s_tracking.addr));
    s_tracking.last_log_tick = 0;
    s_tracking.last_rssi = dev.rssi;
    s_tracking.last_rx_us = esp_timer_get_time();

    char mac[18];
    format_mac_address(dev.addr.val, mac, sizeof(mac), false);
    glog("=== Tracking %s ===\n"
         "     MAC: %s\n"
         "     RSSI: %d dBm (%s)\n",
         ble_device_detect_type_to_string(dev.type), mac, dev.rssi,
         rssi_to_proximity(dev.rssi));
    if (dev.name[0] != '\0') {
        glog("     Name: %s\n", dev.name);
    }
    if (dev.subtype[0] != '\0') {
        glog("     Variant: %s\n", dev.subtype);
    }
    glog("Move closer to increase signal. Press back to stop.\n\n");

    status_display_show_status("BLE Tracking");
    log_tracking_update(&dev);
    return true;
}

bool ble_device_detect_start_airtag_spoof(int index) {
    if (index < 0 || index >= s_device_count) {
        glog("Invalid BLE device index %d\n", index);
        return false;
    }

    BLEDetectDevice *tag = &s_devices[index];
    if (tag->type != BLE_DETECT_DEVICE_AIRTAG) {
        glog("Selected device is not an AirTag\n");
        return false;
    }

    return airtag_scan_spoof_device(tag->addr.val, tag->addr.type, tag->payload, tag->payload_len,
                                    tag->rssi);
}

void ble_device_detect_stop_tracking(void) {
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
    s_tracking.last_rx_us = 0;
}

bool ble_device_detect_is_tracking(void) {
    return s_tracking.active;
}

bool ble_device_detect_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    if (!s_tracking.active) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = s_tracking.last_rssi;
    }
    if (out_fresh) {
        int64_t now = esp_timer_get_time();
        // AirTags and similar beacons advertise infrequently (~2 s), so allow a
        // longer window than the 1.5 s used for dense Wi-Fi/BLE-adv streams.
        *out_fresh = (s_tracking.last_rx_us != 0) && ((now - s_tracking.last_rx_us) < 3500000);
    }
    return true;
}
