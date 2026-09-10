#include "scans/ble/advertiser_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/glog.h"
#include "core/ouis.h"
#include "core/scan_saver.h"
#include "core/utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "managers/ble_manager.h"
#include "managers/gps_manager.h"
#include "managers/rgb_manager.h"
#include "managers/status_display_manager.h"
#include "nimble/ble.h"

#ifndef BLE_HCI_ADV_RPT_EVTYPE_ADV_IND
#define BLE_HCI_ADV_RPT_EVTYPE_ADV_IND 0x00
#endif
#ifndef BLE_HCI_ADV_RPT_EVTYPE_DIR_IND
#define BLE_HCI_ADV_RPT_EVTYPE_DIR_IND 0x01
#endif
#ifndef BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND 0x02
#endif
#ifndef BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND
#define BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND 0x03
#endif
#ifndef BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP
#define BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP 0x04
#endif

#ifdef CONFIG_SPIRAM
#define ADV_SCAN_MAX_DEVICES 64
#else
#define ADV_SCAN_MAX_DEVICES 32
#endif
#define ADV_SCAN_INITIAL_CAPACITY 8
#define ADV_SCAN_MAX_UUID16 4
#define ADV_SCAN_NAME_LEN 24
#define ADV_TRACK_LOG_INTERVAL_MS 1500

#define BLE_AD_TYPE_FLAGS                0x01
#define BLE_AD_TYPE_16BIT_UUID_PARTIAL   0x02
#define BLE_AD_TYPE_16BIT_UUID_COMPLETE  0x03
#define BLE_AD_TYPE_32BIT_UUID_PARTIAL   0x04
#define BLE_AD_TYPE_32BIT_UUID_COMPLETE  0x05
#define BLE_AD_TYPE_128BIT_UUID_PARTIAL  0x06
#define BLE_AD_TYPE_128BIT_UUID_COMPLETE 0x07
#define BLE_AD_TYPE_TX_POWER             0x0A
#define BLE_AD_TYPE_SERVICE_DATA_16BIT   0x16
#define BLE_AD_TYPE_APPEARANCE           0x19
#define BLE_AD_TYPE_MANUFACTURER         0xFF

#define APPLE_COMPANY_ID 0x004C

typedef struct {
    ble_addr_t addr;
    uint16_t seen_count;
    uint16_t manufacturer_id;
    uint16_t service_uuids[ADV_SCAN_MAX_UUID16];
    uint16_t service_data_uuid;
    uint16_t appearance;
    uint16_t ibeacon_major;
    uint16_t ibeacon_minor;
    uint8_t service_uuid_count;
    uint8_t service_data_frame;
    uint8_t event_type;
    uint8_t flags;
    uint8_t adv_flags;
    int8_t rssi;
    int8_t tx_power;
    int8_t ibeacon_measured_power;
    uint8_t ibeacon_uuid[16];
    char name[ADV_SCAN_NAME_LEN];
} AdvertiserDevice;

enum {
    ADV_FLAG_HAS_FLAGS = 1u << 0,
    ADV_FLAG_HAS_TX_POWER = 1u << 1,
    ADV_FLAG_HAS_MANUFACTURER = 1u << 2,
    ADV_FLAG_IS_IBEACON = 1u << 3,
    ADV_FLAG_HAS_32BIT_UUID = 1u << 4,
    ADV_FLAG_HAS_128BIT_UUID = 1u << 5,
    ADV_FLAG_HAS_APPEARANCE = 1u << 6,
};

typedef struct {
    bool active;
    ble_addr_t addr;
    TickType_t last_log_tick;
    int8_t last_rssi;
    rssi_median_t med;
    int64_t last_rx_us;   // esp_timer timestamp of the last matching advertisement
} AdvertiserTrackingState;

typedef enum {
    ADV_FILTER_NONE = 0,
    ADV_FILTER_OUI_PREFIX,
    ADV_FILTER_VENDOR,
} AdvertiserFilterType;

typedef struct {
    AdvertiserFilterType type;
    uint8_t oui[3];
    char vendor[64];
    char label[80];
} AdvertiserFilter;

static const char *TAG = "AdvScan";
static AdvertiserDevice *s_advertisers = NULL;
static uint8_t s_advertiser_count = 0;
static uint8_t s_advertiser_capacity = 0;
static bool s_scan_active = false;
static AdvertiserTrackingState s_tracking = {0};
static AdvertiserFilter s_filter = {0};

extern RGBManager_t rgb_manager;

static const char *adv_event_type_to_string(uint8_t event_type) {
    switch (event_type) {
    case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
        return "Connectable";
    case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
        return "Directed";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
        return "Scannable";
    case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
        return "NonConn";
    case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
        return "ScanRsp";
    default:
        return "Unknown";
    }
}

static const char *addr_type_to_string(uint8_t addr_type) {
    switch (addr_type) {
    case BLE_ADDR_PUBLIC:
        return "Public";
    case BLE_ADDR_RANDOM:
        return "Random";
    default:
        return "Unknown";
    }
}

static const char *manufacturer_name(uint16_t company_id) {
    switch (company_id) {
    case 0x0006:
        return "Microsoft";
    case APPLE_COMPANY_ID:
        return "Apple";
    case 0x0075:
        return "Samsung";
    case 0x00E0:
        return "Google";
    default:
        return NULL;
    }
}

static const char *service_uuid16_name(uint16_t uuid) {
    switch (uuid) {
    case 0x180A:
        return "Device Info";
    case 0x180D:
        return "Heart Rate";
    case 0x180F:
        return "Battery";
    case 0x181A:
        return "Environmental";
    case 0xFEAA:
        return "Eddystone";
    case 0xFE95:
        return "Xiaomi";
    case 0xFE9F:
        return "Google Nearby";
    default:
        return NULL;
    }
}

static const char *appearance_name(uint16_t appearance) {
    switch (appearance) {
    case 0x0000:
        return "Unknown";
    case 0x0040:
        return "Generic Phone";
    case 0x0080:
        return "Generic Computer";
    case 0x00C0:
        return "Generic Watch";
    case 0x0340:
        return "Heart Rate Sensor";
    case 0x0480:
        return "Generic Tag";
    case 0x0540:
        return "Generic Keyring";
    default:
        return NULL;
    }
}

static void append_text(char *buf, size_t buf_size, const char *text) {
    if (buf == NULL || buf_size == 0 || text == NULL || text[0] == '\0') {
        return;
    }
    size_t used = strlen(buf);
    if (used >= buf_size - 1) {
        return;
    }
    snprintf(buf + used, buf_size - used, "%s%s", used > 0 ? ", " : "", text);
}

static void append_uuid16_text(char *buf, size_t buf_size, uint16_t uuid) {
    char item[32];
    const char *name = service_uuid16_name(uuid);
    if (name != NULL) {
        snprintf(item, sizeof(item), "%04X %s", uuid, name);
    } else {
        snprintf(item, sizeof(item), "%04X", uuid);
    }
    append_text(buf, buf_size, item);
}

static bool add_uuid16(AdvertiserDevice *device, uint16_t uuid) {
    for (uint8_t i = 0; i < device->service_uuid_count; i++) {
        if (device->service_uuids[i] == uuid) {
            return true;
        }
    }
    if (device->service_uuid_count >= ADV_SCAN_MAX_UUID16) {
        return false;
    }
    device->service_uuids[device->service_uuid_count++] = uuid;
    return true;
}

static void format_ibeacon_uuid(const uint8_t uuid[16], char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    if (uuid == NULL || out_size < 37) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static void build_services_summary(const AdvertiserDevice *device, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (device == NULL) {
        return;
    }
    for (uint8_t i = 0; i < device->service_uuid_count; i++) {
        append_uuid16_text(out, out_size, device->service_uuids[i]);
    }
    if (device->adv_flags & ADV_FLAG_HAS_32BIT_UUID) {
        append_text(out, out_size, "32-bit UUID");
    }
    if (device->adv_flags & ADV_FLAG_HAS_128BIT_UUID) {
        append_text(out, out_size, "128-bit UUID");
    }
}

static void build_service_data_summary(const AdvertiserDevice *device, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (device == NULL || device->service_data_uuid == 0) {
        return;
    }
    append_uuid16_text(out, out_size, device->service_data_uuid);
    if (device->service_data_uuid == 0xFEAA) {
        if (device->service_data_frame == 0x00) {
            append_text(out, out_size, "UID");
        } else if (device->service_data_frame == 0x10) {
            append_text(out, out_size, "URL");
        } else if (device->service_data_frame == 0x20) {
            append_text(out, out_size, "TLM");
        }
    }
}

static void free_results(void) {
    free(s_advertisers);
    s_advertisers = NULL;
    s_advertiser_count = 0;
    s_advertiser_capacity = 0;
}

static bool ensure_capacity(void) {
    if (s_advertiser_count < s_advertiser_capacity) {
        return true;
    }
    if (s_advertiser_capacity >= ADV_SCAN_MAX_DEVICES) {
        return false;
    }

    uint8_t new_capacity = s_advertiser_capacity == 0 ? ADV_SCAN_INITIAL_CAPACITY
                                                      : (uint8_t)(s_advertiser_capacity * 2);
    if (new_capacity > ADV_SCAN_MAX_DEVICES) {
        new_capacity = ADV_SCAN_MAX_DEVICES;
    }

    AdvertiserDevice *new_devices = realloc(s_advertisers,
                                            (size_t)new_capacity * sizeof(AdvertiserDevice));
    if (new_devices == NULL) {
        return false;
    }

    memset(new_devices + s_advertiser_capacity, 0,
           (size_t)(new_capacity - s_advertiser_capacity) * sizeof(AdvertiserDevice));
    s_advertisers = new_devices;
    s_advertiser_capacity = new_capacity;
    return true;
}

static int find_advertiser_index(const ble_addr_t *addr) {
    if (addr == NULL) {
        return -1;
    }
    for (uint8_t i = 0; i < s_advertiser_count; i++) {
        if (s_advertisers[i].addr.type == addr->type &&
            memcmp(s_advertisers[i].addr.val, addr->val, sizeof(addr->val)) == 0) {
            return i;
        }
    }
    return -1;
}

static void clear_filter(void) {
    memset(&s_filter, 0, sizeof(s_filter));
    s_filter.type = ADV_FILTER_NONE;
}

static bool vendor_matches_filter(const char *vendor) {
    if (vendor == NULL || vendor[0] == '\0' || s_filter.vendor[0] == '\0') {
        return false;
    }
    return strcasecmp(vendor, s_filter.vendor) == 0;
}

static bool advertiser_matches_filter(const ble_addr_t *addr, char *out_vendor, size_t vendor_size) {
    if (s_filter.type == ADV_FILTER_NONE) {
        return true;
    }
    if (addr == NULL) {
        return false;
    }

    if (s_filter.type == ADV_FILTER_OUI_PREFIX) {
        return memcmp(addr->val, s_filter.oui, sizeof(s_filter.oui)) == 0;
    }

    char vendor[64] = {0};
    if (!ouis_lookup_vendor_bytes(addr->val, vendor, sizeof(vendor))) {
        return false;
    }
    if (!vendor_matches_filter(vendor)) {
        return false;
    }
    if (out_vendor != NULL && vendor_size > 0) {
        strncpy(out_vendor, vendor, vendor_size - 1);
        out_vendor[vendor_size - 1] = '\0';
    }
    return true;
}

static bool is_tracking_addr(const ble_addr_t *addr) {
    return s_tracking.active && addr != NULL && s_tracking.addr.type == addr->type &&
           memcmp(s_tracking.addr.val, addr->val, sizeof(s_tracking.addr.val)) == 0;
}

static void log_tracking_update(const AdvertiserDevice *device) {
    if (device == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (s_tracking.last_log_tick != 0 &&
        (now - s_tracking.last_log_tick) < pdMS_TO_TICKS(ADV_TRACK_LOG_INTERVAL_MS)) {
        return;
    }

    char mac[18];
    format_mac_address(device->addr.val, mac, sizeof(mac), false);
    glog("Tracking BLE advertiser: RSSI %d dBm (%s)\n"
         "     MAC: %s\n",
         device->rssi, rssi_to_proximity(device->rssi), mac);
    if (device->name[0] != '\0') {
        glog("     Name: %s\n", device->name);
    }
    s_tracking.last_log_tick = now;
}

static void parse_adv_fields(AdvertiserDevice *device, const uint8_t *data, size_t len) {
    if (device == NULL || data == NULL || len == 0) {
        return;
    }

    char parsed_name[ADV_SCAN_NAME_LEN] = {0};
    parse_ble_device_name(data, len, parsed_name, sizeof(parsed_name));
    if (parsed_name[0] != '\0') {
        strncpy(device->name, parsed_name, sizeof(device->name) - 1);
        device->name[sizeof(device->name) - 1] = '\0';
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

        switch (field_type) {
        case BLE_AD_TYPE_FLAGS:
            if (payload_len >= 1) {
                device->adv_flags |= ADV_FLAG_HAS_FLAGS;
                device->flags = payload[0];
            }
            break;
        case BLE_AD_TYPE_TX_POWER:
            if (payload_len >= 1) {
                device->adv_flags |= ADV_FLAG_HAS_TX_POWER;
                device->tx_power = (int8_t)payload[0];
            }
            break;
        case BLE_AD_TYPE_16BIT_UUID_PARTIAL:
        case BLE_AD_TYPE_16BIT_UUID_COMPLETE:
            for (size_t i = 0; i + 2 <= payload_len; i += 2) {
                add_uuid16(device, read_u16_le(payload + i));
            }
            break;
        case BLE_AD_TYPE_32BIT_UUID_PARTIAL:
        case BLE_AD_TYPE_32BIT_UUID_COMPLETE:
            device->adv_flags |= ADV_FLAG_HAS_32BIT_UUID;
            break;
        case BLE_AD_TYPE_128BIT_UUID_PARTIAL:
        case BLE_AD_TYPE_128BIT_UUID_COMPLETE:
            device->adv_flags |= ADV_FLAG_HAS_128BIT_UUID;
            break;
        case BLE_AD_TYPE_SERVICE_DATA_16BIT:
            if (payload_len >= 2) {
                device->service_data_uuid = read_u16_le(payload);
                device->service_data_frame = payload_len >= 3 ? payload[2] : 0;
            }
            break;
        case BLE_AD_TYPE_APPEARANCE:
            if (payload_len >= 2) {
                device->adv_flags |= ADV_FLAG_HAS_APPEARANCE;
                device->appearance = read_u16_le(payload);
            }
            break;
        case BLE_AD_TYPE_MANUFACTURER:
            if (payload_len >= 2) {
                device->adv_flags |= ADV_FLAG_HAS_MANUFACTURER;
                device->manufacturer_id = read_u16_le(payload);
                if (device->manufacturer_id == APPLE_COMPANY_ID && payload_len >= 25 &&
                    payload[2] == 0x02 && payload[3] == 0x15) {
                    device->adv_flags |= ADV_FLAG_IS_IBEACON;
                    memcpy(device->ibeacon_uuid, payload + 4, sizeof(device->ibeacon_uuid));
                    device->ibeacon_major = ((uint16_t)payload[20] << 8) | payload[21];
                    device->ibeacon_minor = ((uint16_t)payload[22] << 8) | payload[23];
                    device->ibeacon_measured_power = (int8_t)payload[24];
                }
            }
            break;
        default:
            break;
        }

        remaining -= (size_t)(field_len + 1);
        p += (size_t)(field_len + 1);
    }
}

static void print_advertiser_line(int index, const AdvertiserDeviceInfo *info) {
    char mac[18];
    format_mac_address(info->mac, mac, sizeof(mac), false);
    glog("[%d] %s | %s | %d dBm | %s", index, info->is_ibeacon ? "iBeacon" : "Advertiser",
         mac, info->rssi, info->adv_type);
    if (info->name[0] != '\0') {
        glog(" | %s", info->name);
    }
    if (info->oui_vendor[0] != '\0') {
        glog(" | OUI %s", info->oui_vendor);
    }
    if (info->manufacturer[0] != '\0') {
        glog(" | MFG %s", info->manufacturer);
    }
    if (info->services[0] != '\0') {
        glog(" | SVC %s", info->services);
    }
    if (info->is_ibeacon) {
        glog(" | Major %u Minor %u", info->ibeacon_major, info->ibeacon_minor);
    }
    glog("\n");
}

static void print_advertiser_detail(scan_file_t *sf, int index, const AdvertiserDeviceInfo *info) {
    char mac[18];
    format_mac_address(info->mac, mac, sizeof(mac), false);

    const char *label = info->is_ibeacon ? "iBeacon" : "BLE Advertiser";
    glog("[%d] %s\n", index, label);
    glog("     MAC: %s\n", mac);
    glog("     Address Type: %s\n", addr_type_to_string(info->addr_type));
    glog("     RSSI: %d dBm (%s), seen %lu\n", info->rssi, rssi_to_proximity(info->rssi),
         (unsigned long)info->seen_count);
    glog("     Adv Type: %s\n", info->adv_type);
    if (info->name[0] != '\0') {
        glog("     Name: %s\n", info->name);
    }
    if (info->has_flags) {
        glog("     Flags: 0x%02X\n", info->flags);
    }
    if (info->has_tx_power) {
        glog("     TX Power: %d dBm\n", info->tx_power);
    }
    if (info->oui_vendor[0] != '\0') {
        glog("     OUI Vendor: %s\n", info->oui_vendor);
    }
    if (info->manufacturer[0] != '\0') {
        glog("     Manufacturer: %s\n", info->manufacturer);
    }
    if (info->has_appearance) {
        const char *appearance = appearance_name(info->appearance);
        glog("     Appearance: 0x%04X%s%s\n", info->appearance,
             appearance ? " " : "", appearance ? appearance : "");
    }
    if (info->services[0] != '\0') {
        glog("     Services: %s\n", info->services);
    }
    if (info->service_data[0] != '\0') {
        glog("     Service Data: %s\n", info->service_data);
    }
    if (info->is_ibeacon) {
        glog("     iBeacon UUID: %s\n", info->ibeacon_uuid);
        glog("     iBeacon Major: %u\n", info->ibeacon_major);
        glog("     iBeacon Minor: %u\n", info->ibeacon_minor);
        glog("     Measured Power: %d dBm\n", info->ibeacon_measured_power);
    }

    if (sf != NULL) {
        scan_file_printf(sf, "[%d] %s, MAC: %s, RSSI: %d, Adv: %s", index, label, mac,
                         info->rssi, info->adv_type);
        scan_file_printf(sf, ", Addr Type: %s", addr_type_to_string(info->addr_type));
        if (info->name[0] != '\0') scan_file_printf(sf, ", Name: %s", info->name);
        if (info->oui_vendor[0] != '\0') scan_file_printf(sf, ", OUI: %s", info->oui_vendor);
        if (info->manufacturer[0] != '\0') scan_file_printf(sf, ", MFG: %s", info->manufacturer);
        if (info->has_appearance) scan_file_printf(sf, ", Appearance: 0x%04X", info->appearance);
        if (info->services[0] != '\0') scan_file_printf(sf, ", Services: %s", info->services);
        if (info->service_data[0] != '\0') scan_file_printf(sf, ", Service Data: %s", info->service_data);
        if (info->is_ibeacon) {
            scan_file_printf(sf, ", iBeacon UUID: %s, Major: %u, Minor: %u, Measured Power: %d",
                             info->ibeacon_uuid, info->ibeacon_major, info->ibeacon_minor,
                             info->ibeacon_measured_power);
        }
        scan_file_printf(sf, "\n");
    }
}

static void advertiser_scan_callback(struct ble_gap_event *event, size_t len) {
    (void)len;
    if (!s_scan_active || event == NULL || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    if (!advertiser_matches_filter(&event->disc.addr, NULL, 0)) {
        return;
    }

    // While tracking a single advertiser, ignore all other advertisements so
    // the terminal output only shows RSSI updates for the tracked device and
    // no new devices are added to the result list.
    if (s_tracking.active) {
        if (!is_tracking_addr(&event->disc.addr)) {
            return;
        }
        int tracked_index = find_advertiser_index(&event->disc.addr);
        if (tracked_index < 0) {
            return;
        }
        AdvertiserDevice *tracked = &s_advertisers[tracked_index];
        tracked->rssi = event->disc.rssi;
        tracked->event_type = event->disc.event_type;
        if (tracked->seen_count < UINT16_MAX) {
            tracked->seen_count++;
        }
        parse_adv_fields(tracked, event->disc.data, event->disc.length_data);
        rssi_median_push(&s_tracking.med, event->disc.rssi);
        s_tracking.last_rssi = rssi_median_get(&s_tracking.med);
        tracked->rssi = s_tracking.last_rssi;
        s_tracking.last_rx_us = esp_timer_get_time();
        log_tracking_update(tracked);
        return;
    }

    int index = find_advertiser_index(&event->disc.addr);
    bool is_new = false;
    if (index < 0) {
        if (!ensure_capacity()) {
            return;
        }
        index = s_advertiser_count++;
        AdvertiserDevice *device = &s_advertisers[index];
        memset(device, 0, sizeof(*device));
        memcpy(&device->addr, &event->disc.addr, sizeof(device->addr));
        is_new = true;
    }

    AdvertiserDevice *device = &s_advertisers[index];
    device->rssi = event->disc.rssi;
    device->event_type = event->disc.event_type;
    if (device->seen_count < UINT16_MAX) {
        device->seen_count++;
    }
    parse_adv_fields(device, event->disc.data, event->disc.length_data);

    if (is_new) {
        AdvertiserDeviceInfo info;
        if (advertiser_scan_get_device(index, &info) == 0) {
            print_advertiser_line(index, &info);
        }
        if (s_filter.type != ADV_FILTER_NONE) {
            rgb_manager_pulse_async(&rgb_manager, 0, 255, 255);
        }
    }
}

static void advertiser_scan_start_common(void) {
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
    free_results();

    if (!ble_is_initialized()) {
        ble_init();
    }
    if (!ble_wait_for_ready()) {
        ESP_LOGE(TAG, "BLE stack not ready for advertiser scan");
        status_display_show_status("BLE Not Ready");
        return;
    }

    ble_unregister_handler(advertiser_scan_callback);
    ble_register_handler(advertiser_scan_callback);
    s_scan_active = true;

    if (!ble_start_scanning()) {
        s_scan_active = false;
        ble_unregister_handler(advertiser_scan_callback);
        status_display_show_status("BLE Adv Fail");
        return;
    }

    if (s_filter.type == ADV_FILTER_NONE) {
        glog("BLE advertiser scan started. Run 'listadv' for parsed results.\n");
        status_display_show_status("BLE Adv Scan");
    } else {
        glog("BLE OUI scan started: %s. Run 'listadv' for matching advertisers.\n",
             s_filter.label);
        status_display_show_status("BLE OUI Scan");
    }
}

void advertiser_scan_start(void) {
    clear_filter();
    advertiser_scan_start_common();
}

bool advertiser_scan_start_oui_prefix(const uint8_t oui[3]) {
    if (oui == NULL) {
        return false;
    }
    clear_filter();
    s_filter.type = ADV_FILTER_OUI_PREFIX;
    memcpy(s_filter.oui, oui, sizeof(s_filter.oui));
    snprintf(s_filter.label, sizeof(s_filter.label), "OUI %02X:%02X:%02X",
             oui[0], oui[1], oui[2]);
    advertiser_scan_start_common();
    return advertiser_scan_is_active();
}

bool advertiser_scan_start_vendor(const char *vendor) {
    if (vendor == NULL || vendor[0] == '\0') {
        return false;
    }
    clear_filter();
    s_filter.type = ADV_FILTER_VENDOR;
    strncpy(s_filter.vendor, vendor, sizeof(s_filter.vendor) - 1);
    s_filter.vendor[sizeof(s_filter.vendor) - 1] = '\0';
    snprintf(s_filter.label, sizeof(s_filter.label), "Vendor %s", s_filter.vendor);
    advertiser_scan_start_common();
    return advertiser_scan_is_active();
}

void advertiser_scan_stop(void) {
    bool was_active = s_scan_active;
    s_scan_active = false;
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
    ble_unregister_handler(advertiser_scan_callback);

    if (was_active && ble_is_initialized()) {
        ble_stop();
    }
    if (was_active) {
        glog("BLE advertiser scan stopped. Found %u advertisers.\n", s_advertiser_count);
        status_display_show_status("BLE Adv Stop");
    }
}

bool advertiser_scan_is_active(void) {
    return s_scan_active;
}

bool advertiser_scan_is_filtered(void) {
    return s_filter.type != ADV_FILTER_NONE;
}

const char *advertiser_scan_get_filter_label(void) {
    return (s_filter.type == ADV_FILTER_NONE || s_filter.label[0] == '\0') ? NULL : s_filter.label;
}

int advertiser_scan_get_count(void) {
    return s_advertiser_count;
}

int advertiser_scan_get_device(int index, AdvertiserDeviceInfo *out_info) {
    if (out_info == NULL || index < 0 || index >= s_advertiser_count || s_advertisers == NULL) {
        return -1;
    }

    const AdvertiserDevice *device = &s_advertisers[index];
    memset(out_info, 0, sizeof(*out_info));
    memcpy(out_info->mac, device->addr.val, sizeof(out_info->mac));
    out_info->addr_type = device->addr.type;
    out_info->rssi = device->rssi;
    out_info->event_type = device->event_type;
    out_info->seen_count = device->seen_count;
    out_info->has_flags = (device->adv_flags & ADV_FLAG_HAS_FLAGS) != 0;
    out_info->flags = device->flags;
    out_info->has_tx_power = (device->adv_flags & ADV_FLAG_HAS_TX_POWER) != 0;
    out_info->tx_power = device->tx_power;
    out_info->has_appearance = (device->adv_flags & ADV_FLAG_HAS_APPEARANCE) != 0;
    out_info->appearance = device->appearance;
    out_info->has_manufacturer_id = (device->adv_flags & ADV_FLAG_HAS_MANUFACTURER) != 0;
    out_info->manufacturer_id = device->manufacturer_id;
    out_info->is_ibeacon = (device->adv_flags & ADV_FLAG_IS_IBEACON) != 0;
    out_info->ibeacon_major = device->ibeacon_major;
    out_info->ibeacon_minor = device->ibeacon_minor;
    out_info->ibeacon_measured_power = device->ibeacon_measured_power;
    strncpy(out_info->name, device->name, sizeof(out_info->name) - 1);
    strncpy(out_info->adv_type, adv_event_type_to_string(device->event_type),
            sizeof(out_info->adv_type) - 1);
    build_services_summary(device, out_info->services, sizeof(out_info->services));
    build_service_data_summary(device, out_info->service_data, sizeof(out_info->service_data));

    if (out_info->is_ibeacon) {
        format_ibeacon_uuid(device->ibeacon_uuid, out_info->ibeacon_uuid,
                            sizeof(out_info->ibeacon_uuid));
    }
    if (out_info->has_manufacturer_id) {
        const char *mfg = manufacturer_name(device->manufacturer_id);
        if (mfg != NULL) {
            snprintf(out_info->manufacturer, sizeof(out_info->manufacturer), "0x%04X %s",
                     device->manufacturer_id, mfg);
        } else {
            snprintf(out_info->manufacturer, sizeof(out_info->manufacturer), "0x%04X",
                     device->manufacturer_id);
        }
    }

    char mac[18];
    format_mac_address(device->addr.val, mac, sizeof(mac), false);
    ouis_lookup_vendor(mac, out_info->oui_vendor, sizeof(out_info->oui_vendor));

    return 0;
}

bool advertiser_scan_start_tracking(int index) {
    if (index < 0 || index >= s_advertiser_count || s_advertisers == NULL) {
        glog("Invalid advertiser index %d\n", index);
        return false;
    }

    AdvertiserDevice dev;
    memcpy(&dev, &s_advertisers[index], sizeof(dev));

    if (!s_scan_active) {
        if (!ble_is_initialized()) {
            ble_init();
        }
        if (!ble_wait_for_ready()) {
            ESP_LOGE(TAG, "BLE stack not ready for advertiser tracking");
            return false;
        }
        ble_unregister_handler(advertiser_scan_callback);
        ble_register_handler(advertiser_scan_callback);
        if (!ble_start_scanning()) {
            ble_unregister_handler(advertiser_scan_callback);
            return false;
        }
        s_scan_active = true;
    }

    s_tracking.active = true;
    memcpy(&s_tracking.addr, &dev.addr, sizeof(s_tracking.addr));
    s_tracking.last_log_tick = 0;
    s_tracking.last_rssi = dev.rssi;
    rssi_median_reset(&s_tracking.med);
    rssi_median_push(&s_tracking.med, dev.rssi);
    s_tracking.last_rx_us = esp_timer_get_time();

    char mac[18];
    format_mac_address(dev.addr.val, mac, sizeof(mac), false);
    glog("=== Tracking BLE Advertiser ===\n"
         "     MAC: %s\n"
         "     RSSI: %d dBm (%s)\n",
         mac, dev.rssi, rssi_to_proximity(dev.rssi));
    if (dev.name[0] != '\0') {
        glog("     Name: %s\n", dev.name);
    }
    glog("Move closer to increase signal. Press back to stop.\n\n");
    status_display_show_status("BLE Adv Track");
    log_tracking_update(&dev);
    return true;
}

void advertiser_scan_stop_tracking(void) {
    s_tracking.active = false;
    s_tracking.last_log_tick = 0;
}

bool advertiser_scan_is_tracking(void) {
    return s_tracking.active;
}

bool advertiser_scan_get_track_status(int8_t *out_rssi, bool *out_fresh) {
    if (!s_tracking.active) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = s_tracking.last_rssi;
    }
    if (out_fresh) {
        int64_t now = esp_timer_get_time();
        // Consider the reading "live" if a matching advertisement arrived recently.
        *out_fresh = (s_tracking.last_rx_us != 0) && ((now - s_tracking.last_rx_us) < 1500000);
    }
    return true;
}

void advertiser_scan_print_devices(void) {
    if (s_advertiser_count == 0) {
        glog("No BLE advertisers discovered. Run 'blescan -adv' first.\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "ble_advertisers", "txt") == ESP_OK);

    glog("--- BLE Advertisers (%u) ---\n", s_advertiser_count);
    if (saving) {
        scan_file_printf(&sf, "--- BLE Advertisers (%u) ---\n", s_advertiser_count);
    }

    for (uint8_t i = 0; i < s_advertiser_count; i++) {
        AdvertiserDeviceInfo info;
        if (advertiser_scan_get_device(i, &info) == 0) {
            print_advertiser_detail(saving ? &sf : NULL, i, &info);
        }
    }

    if (saving) {
        scan_file_close(&sf);
    }
}

static bool advertiser_scan_get_gps(gps_t *out_gps) {
    if (out_gps == NULL) {
        return false;
    }
    if (!g_gpsManager.isinitilized) {
        return false;
    }
    return gps_manager_get_local_gps_snapshot(out_gps) && out_gps->valid;
}

static void write_gps_header(scan_file_t *sf) {
    if (sf == NULL) {
        return;
    }
    gps_t gps;
    if (!advertiser_scan_get_gps(&gps)) {
        return;
    }
    scan_file_printf(sf, "GPS: lat=%.6f, lon=%.6f, alt=%.1fm, sats=%u\n",
                     (double)gps.latitude, (double)gps.longitude, (double)gps.altitude,
                     (unsigned)gps.sats_in_use);
}

static bool write_advertiser_record(scan_file_t *sf, int index, const AdvertiserDeviceInfo *info,
                                    bool include_header) {
    if (sf == NULL || info == NULL) {
        return false;
    }

    char mac[18];
    format_mac_address(info->mac, mac, sizeof(mac), false);

    const char *label = info->is_ibeacon ? "iBeacon" : "BLE Advertiser";
    if (include_header) {
        scan_file_printf(sf, "--- %s (%s) ---\n", label, mac);
        write_gps_header(sf);
    }
    scan_file_printf(sf, "[%d] %s, MAC: %s, Addr Type: %s, RSSI: %d, Adv: %s", index, label, mac,
                     addr_type_to_string(info->addr_type), info->rssi, info->adv_type);
    if (info->name[0] != '\0') scan_file_printf(sf, ", Name: %s", info->name);
    if (info->oui_vendor[0] != '\0') scan_file_printf(sf, ", OUI: %s", info->oui_vendor);
    if (info->manufacturer[0] != '\0') scan_file_printf(sf, ", MFG: %s", info->manufacturer);
    if (info->services[0] != '\0') scan_file_printf(sf, ", Services: %s", info->services);
    if (info->service_data[0] != '\0') scan_file_printf(sf, ", Service Data: %s", info->service_data);
    if (info->has_appearance) scan_file_printf(sf, ", Appearance: 0x%04X", info->appearance);
    if (info->is_ibeacon) {
        scan_file_printf(sf, ", iBeacon UUID: %s, Major: %u, Minor: %u, Measured Power: %d",
                         info->ibeacon_uuid, info->ibeacon_major, info->ibeacon_minor,
                         info->ibeacon_measured_power);
    }
    scan_file_printf(sf, "\n");
    return true;
}

bool advertiser_scan_save_to_sd(int index) {
    if (s_advertiser_count == 0) {
        glog("No BLE advertisers discovered. Run 'blescan -adv' first.\n");
        return false;
    }

    const char *prefix = (index < 0) ? "ble_advertisers" : "ble_advertiser";
    scan_file_t sf = SCAN_FILE_INIT;
    if (scan_file_open(&sf, prefix, "txt") != ESP_OK) {
        glog("SD card unavailable; cannot save advertisers\n");
        return false;
    }

    if (index < 0) {
        scan_file_printf(&sf, "--- BLE Advertisers (%u) ---\n", s_advertiser_count);
        write_gps_header(&sf);
        for (uint8_t i = 0; i < s_advertiser_count; i++) {
            AdvertiserDeviceInfo info;
            if (advertiser_scan_get_device(i, &info) == 0) {
                write_advertiser_record(&sf, i, &info, false);
            }
        }
    } else {
        if (index < 0 || index >= s_advertiser_count) {
            glog("Invalid advertiser index %d\n", index);
            scan_file_close(&sf);
            return false;
        }
        AdvertiserDeviceInfo info;
        if (advertiser_scan_get_device(index, &info) != 0) {
            scan_file_close(&sf);
            return false;
        }
        write_advertiser_record(&sf, index, &info, true);
    }

    scan_file_close(&sf);
    return true;
}
