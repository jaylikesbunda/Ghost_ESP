// wifi_manager.h

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stddef.h>

#ifndef DNS_SERVER_HANDLE_T_DEFINED
typedef struct dns_server_handle *dns_server_handle_t;
#define DNS_SERVER_HANDLE_T_DEFINED
#endif

#define RANDOM_SSID_LEN 8
#define BEACON_INTERVAL 0x0064 // 100 Time Units (TU)
#define CAPABILITY_INFO 0x0411 // Capability information (ESS)
#define MAX_STATIONS 50
#define BEACON_LIST_MAX 16

typedef struct {
  uint8_t station_mac[6]; // MAC address of the station (client)
  uint8_t ap_bssid[6];    // BSSID (MAC address) of the access point
} station_ap_pair_t;

extern station_ap_pair_t station_ap_list[MAX_STATIONS];
extern int station_count;
extern bool manual_disconnect;
extern wifi_ap_record_t *scanned_aps;
extern wifi_ap_record_t selected_ap;
extern wifi_ap_record_t *selected_aps;
extern int selected_ap_count;
extern uint16_t ap_count;
extern volatile bool ap_sta_has_ip;

// Channel list for deauth and wireshark (country-appropriate)
extern uint8_t wireshark_channels[50];
extern size_t wireshark_channels_count;

// Note: Use wifi_channels_build_country_list() from scans/wifi/wifi_channels.h
// to build country-appropriate channel lists

// WiFi event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_CONNECTING_BIT BIT1

typedef struct {
  uint8_t frame_control[2]; // Frame Control
  uint16_t duration;        // Duration
  uint8_t dest_addr[6];     // Destination Address
  uint8_t src_addr[6];      // Source Address (AP MAC)
  uint8_t bssid[6];         // BSSID (AP MAC)
  uint16_t seq_ctrl;        // Sequence Control
  uint64_t timestamp;       // Timestamp (microseconds since the AP started)
  uint16_t beacon_interval; // Beacon Interval (in Time Units)
  uint16_t cap_info;        // Capability Information
} __attribute__((packed)) wifi_beacon_frame_t;

typedef struct {
  unsigned protocol_version : 2;
  unsigned type : 2;
  unsigned subtype : 4;
  unsigned to_ds : 1;
  unsigned from_ds : 1;
  unsigned more_frag : 1;
  unsigned retry : 1;
  unsigned pwr_mgmt : 1;
  unsigned more_data : 1;
  unsigned protected_frame : 1;
  unsigned order : 1;
} wifi_ieee80211_frame_ctrl_t;

typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl;  // 2 bytes (raw Frame Control field)
    uint16_t duration_id;                    // 2 bytes
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint16_t seq_ctrl;
} wifi_ieee80211_hdr_t;

typedef struct {
  uint8_t type;
  uint8_t code;
  uint16_t checksum;
  uint16_t id;
  uint16_t seqno;
} __attribute__((__packed__)) icmp_packet_t;

typedef struct {
  uint16_t frame_ctrl;  // Frame control field
  uint16_t duration_id; // Duration field
  uint8_t addr1[6];     // Receiver address (RA)
  uint8_t addr2[6];     // Transmitter address (TA)
  uint8_t addr3[6];     // BSSID or destination address
  uint16_t seq_ctrl;    // Sequence control field
} wifi_ieee80211_mac_hdr_t;

typedef struct {
  wifi_ieee80211_hdr_t hdr; // The 802.11 header
  uint8_t payload[];        // Variable-length payload (data)
} wifi_ieee80211_packet_t;

typedef struct {
  char ip[16];
  uint16_t open_ports[64];
  uint8_t num_open_ports;
} host_result_t;

typedef struct {
  const char *ssid;
  const char *password;
} wifi_credentials_t;

typedef struct {
  char subnet_prefix[16];
  host_result_t *results;
  size_t max_results;
  size_t num_active_hosts;
} scanner_ctx_t;

// Note: arp_host_t and arp_scanner_ctx_t are now defined in scans/wifi/arp_scan.h

typedef void (*wifi_promiscuous_cb_t_t)(void *buf,
                                        wifi_promiscuous_pkt_type_t type);

// Initialize WiFiManager
void wifi_manager_init(void);
void wifi_manager_release_stream_buffer(void);

// Start scanning for available networks
void wifi_manager_start_scan();

// Stop scanning for networks
void wifi_manager_stop_scan();

// Print the scan results with BSSID to company mapping
void wifi_manager_print_scan_results_with_oui();

// Function to provide access to the last scan results
void wifi_manager_get_scan_results_data(uint16_t *count, wifi_ap_record_t **aps);

// Select an access point from the scan results based on index
void wifi_manager_select_ap(int index);

// Select multiple access points from the scan results based on indices array
void wifi_manager_select_multiple_aps(int *indices, int count);

// Get access to the selected APs array for commands that support multiple APs
void wifi_manager_get_selected_aps(wifi_ap_record_t **aps, int *count);

// Select a station from the station list based on index
void wifi_manager_select_station(int index);

// Deauthenticate the selected station or fallback to global deauth if none selected
void wifi_manager_deauth_station(void);

// Stop station deauth background task and restart AP if running, return true if stopped
bool wifi_manager_stop_deauth_station(void);

// broadcast ap beacon with optional ssid
esp_err_t wifi_manager_broadcast_ap(const char *ssid);

void wifi_manager_start_beacon(const char *ssid);

void wifi_manager_auto_deauth();

void wifi_manager_stop_beacon();

void wifi_manager_set_manual_disconnect(bool disconnect);

void wifi_manager_configure_sta_from_settings(void);

void wifi_manager_start_sntp(void);

// Returns true once the system clock is valid for HTTPS, waiting for SNTP
// when necessary.
bool wifi_manager_wait_for_valid_time(uint32_t timeout_ms);

void wifi_manager_start_ip_lookup();

#ifdef CONFIG_SPIRAM
#define MDNS_MAX_DEVICES 128
#else
#define MDNS_MAX_DEVICES 48
#endif

typedef struct {
    char hostname[64];
    char ip[16];
    uint16_t port;
    char service_type[32];
} mdns_device_t;

esp_err_t wifi_manager_start_ip_lookup_async(void);
bool wifi_manager_ip_lookup_check_done(void);
void wifi_manager_ip_lookup_finish_async(void);
bool wifi_manager_ip_lookup_is_running(void);
int wifi_manager_ip_lookup_get_count(void);
const mdns_device_t* wifi_manager_ip_lookup_get_device(int index);
void wifi_manager_ip_lookup_clear(void);

void wifi_manager_connect_wifi(const char *ssid, const char *password);

void wifi_manager_cancel_connect(void);

void wifi_manager_stop_reconnect(void);
void wifi_manager_set_reconnect_hold(bool hold);

void wifi_manager_start_visualizer(bool for_screen);

void wifi_manager_stop_visualizer(void);

void wifi_manager_stop_monitor_mode();

void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback);

void wifi_manager_list_stations();

// Start station scanning with channel hopping
void wifi_manager_start_station_scan();

// Wireshark capture channel hopping
void wifi_manager_start_wireshark_channel_hop(void);
void wifi_manager_stop_wireshark_channel_hop(void);
esp_err_t wifi_manager_start_wireshark_channel_list(const uint8_t *channels, size_t count);

// Set fixed channel for Wireshark capture
esp_err_t wifi_manager_set_wireshark_fixed_channel(uint8_t channel);

// Lock any monitor-mode capture (probe/deauth/beacon/raw/eapol/pwn/wps) to a
// fixed WiFi channel. Stops any active channel hopping and sets the channel
// via esp_wifi_set_channel. Returns ESP_ERR_INVALID_ARG for out-of-range values.
esp_err_t wifi_manager_set_capture_channel_lock(uint8_t channel);

void wifi_manager_start_deauth();

void wifi_manager_stop_deauth();

void wifi_stations_sniffer_callback(void *buf,
                                    wifi_promiscuous_pkt_type_t type);

void wifi_manager_stop_evil_portal();
void wifi_manager_stop_evil_portal_keep_wifi(void);

esp_err_t wifi_manager_start_evil_portal(const char *URL, const char *SSID,
                                         const char *Password,
                                         const char *ap_ssid,
                                         const char *domain);
bool wifi_manager_is_evil_portal_active(void);

void screen_music_visualizer_task(void *pvParameters);

void rgb_visualizer_server_task(void *pvParameters);

void animate_led_based_on_amplitude(void *pvParameters);

void wifi_manager_scan_for_open_ports();

bool get_subnet_prefix(scanner_ctx_t *ctx);

bool is_host_active(const char *ip_addr);

scanner_ctx_t *scanner_init(void);
void scanner_cleanup(scanner_ctx_t *ctx);

bool wifi_manager_scan_subnet();

void scan_ports_on_host(const char *target_ip, host_result_t *result);
void scan_ssh_on_host(const char *target_ip, host_result_t *result);

bool scan_ip_port_range(const char *target_ip, uint16_t start_port,
                        uint16_t end_port);

void scan_udp_ports_on_host(const char *target_ip, host_result_t *result);
bool scan_ip_udp_port_range(const char *target_ip, uint16_t start_port,
                            uint16_t end_port);

// ARP scan functions (wrapper that delegates to arp_scan module)
bool wifi_manager_arp_scan_subnet(void);

extern const uint16_t COMMON_PORTS[];
extern const size_t NUM_PORTS;

esp_err_t wifi_manager_start_scan_with_time(int seconds);

void wifi_manager_scanall_chart(void);

// Functions to manage a custom beacon SSID list
void wifi_manager_add_beacon_ssid(const char *ssid);
void wifi_manager_remove_beacon_ssid(const char *ssid);
void wifi_manager_clear_beacon_list(void);
void wifi_manager_show_beacon_list(void);
void wifi_manager_start_beacon_list(void);

void wifi_manager_start_live_ap_scan(void);

void wifi_manager_start_eapollogoff_attack(void);
void wifi_manager_stop_eapollogoff_attack(void);
void wifi_manager_eapollogoff_display(void);
void wifi_manager_eapollogoff_help(void);

// SAE Handshake Flooding Attack (ESP32-C5/C6 only)
void wifi_manager_start_sae_flood(const char *password);
void wifi_manager_stop_sae_flood(void);
void wifi_manager_sae_flood_help(void);

// GTK Abuse Test
void wifi_manager_start_gtk_abuse(const char *ssid, const char *password);
void wifi_manager_stop_gtk_abuse(void);
bool wifi_manager_gtk_abuse_is_running(void);
void wifi_manager_gtk_abuse_display(void);

// Channel Switch Attack (CSA)
void wifi_manager_start_channel_switch_attack(void);
void wifi_manager_stop_channel_switch_attack(void);
bool wifi_manager_is_channel_switch_attack_running(void);

// Probe Request Flood Attack
void wifi_manager_start_probe_flood(void);
void wifi_manager_stop_probe_flood(void);
bool wifi_manager_is_probe_flood_running(void);

// Bad Msg Attack (EAPOL key install + zero MIC)
void wifi_manager_start_bad_msg(void);
void wifi_manager_stop_bad_msg(void);
bool wifi_manager_is_bad_msg_running(void);

// Auth Flood Attack
void wifi_manager_start_auth_flood(void);
void wifi_manager_stop_auth_flood(void);
bool wifi_manager_is_auth_flood_running(void);

// Handshake + Deauth combined attack
void wifi_manager_start_handshake_deauth(void);
bool wifi_manager_stop_handshake_deauth(void);
bool wifi_manager_handshake_deauth_is_running(void);

// HTML buffer functions for evil portal
void wifi_manager_set_html_from_uart(void);
void wifi_manager_store_html_chunk(const char* data, size_t len, bool is_final);
void wifi_manager_clear_html_buffer(void);
void wifi_manager_clear_scan_results(void);

// Karma attack functions
void wifi_manager_start_karma(void);
void wifi_manager_stop_karma(void);
bool wifi_manager_karma_is_running(void);
void wifi_manager_set_karma_ssid_list(const char **ssids, int count);
void wifi_manager_set_karma_portal_file(const char *path);

// RSSI tracking functions
void wifi_manager_track_ap(void);
void wifi_manager_track_sta(void);
void wifi_manager_stop_tracking(void);

// Reports the latest tracking RSSI for the live RSSI meter view. Returns false
// when neither AP nor STA tracking is active. When active, *out_rssi receives the
// most recent matched RSSI and *out_fresh whether a packet arrived recently.
bool wifi_manager_get_track_status(int8_t *out_rssi, bool *out_fresh);

// Closeness of the smoothed tracking RSSI within this session's observed
// [min, max] range, 0-100. Returns -1 when not tracking or no range yet.
int wifi_manager_get_track_closeness(void);

dns_server_handle_t dns_handle_take(void);

#endif // WIFI_MANAGER_H
