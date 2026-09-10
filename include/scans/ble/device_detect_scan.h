#ifndef DEVICE_DETECT_SCAN_H
#define DEVICE_DETECT_SCAN_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_DETECT_DEVICE_UNKNOWN = 0,
    BLE_DETECT_DEVICE_AIRTAG,
    BLE_DETECT_DEVICE_FLIPPER,
    BLE_DETECT_DEVICE_SKIMMER,
    BLE_DETECT_DEVICE_TILE,
    BLE_DETECT_DEVICE_SMARTTAG,
    BLE_DETECT_DEVICE_CHIPOLO,
    BLE_DETECT_DEVICE_AIRPODS,
    BLE_DETECT_DEVICE_APPLE_WATCH,
    BLE_DETECT_DEVICE_FINDMY,
    BLE_DETECT_DEVICE_FASTPAIR,
    BLE_DETECT_DEVICE_HEARING_AID,
    BLE_DETECT_DEVICE_GAEN,
    BLE_DETECT_DEVICE_CHAMELEON,
} BLEDetectDeviceType;

typedef struct {
    BLEDetectDeviceType type;
    uint8_t mac[6];
    int8_t rssi;
    char name[32];
    char subtype[20];
    bool tracking;
} BLEDetectDeviceInfo;

void ble_device_detect_start(void);
void ble_device_detect_stop(void);
void ble_device_detect_clear_results(void);
bool ble_device_detect_is_active(void);

int ble_device_detect_get_count(void);
int ble_device_detect_get_device(int index, BLEDetectDeviceInfo *out_info);

bool ble_device_detect_start_tracking(int index);
bool ble_device_detect_start_airtag_spoof(int index);
void ble_device_detect_stop_tracking(void);
bool ble_device_detect_is_tracking(void);

/*
 * Live RSSI status for the tracked detect device, mirroring
 * wifi_manager_get_track_status(): returns false when no device is being
 * tracked. *out_rssi receives the last seen RSSI and *out_fresh is true when a
 * matching advertisement arrived recently (so the meter can dim when stale).
 */
bool ble_device_detect_get_track_status(int8_t *out_rssi, bool *out_fresh);

const char *ble_device_detect_type_to_string(BLEDetectDeviceType type);

#endif
