#ifndef CALLBACKS_H
#define CALLBACKS_H
#include "esp_wifi_types.h"
#include <esp_timer.h>
#include "vendor/GPS/MicroNMEA.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "host/ble_gap.h"
#endif

void wifi_wps_detection_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void wifi_beacon_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wifi_deauth_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wifi_pwn_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wifi_probe_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wifi_raw_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wifi_eapol_scan_callback(void* buf, wifi_promiscuous_pkt_type_t type);
void wardriving_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type);
void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data);


typedef enum {
    WPS_MODE_NONE = 0,   // No WPS support
    WPS_MODE_PBC,        // Push Button Configuration (PBC)
    WPS_MODE_PIN         // PIN method (Display or Keypad)
} wps_modes_t;

typedef struct {
    char ssid[33];        // SSID (max 32 characters + null terminator)
    uint8_t bssid[6];     // BSSID (MAC address)
    bool wps_enabled;     // True if WPS is enabled
    wps_modes_t wps_mode;  // WPS mode (PIN or PBC)
} wps_network_t;

extern gps_t *gps;
extern wps_network_t detected_wps_networks[MAX_WPS_NETWORKS];
extern int detected_network_count;
extern esp_timer_handle_t stop_timer;
extern int should_store_wps;
static uint8_t router_ip[4];

#endif