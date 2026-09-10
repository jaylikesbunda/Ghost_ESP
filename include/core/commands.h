// commands.h
// Shared declarations for CLI command handlers that live outside commandline.c.

#ifndef COMMANDS_H
#define COMMANDS_H

#include "sdkconfig.h"
#include <stdbool.h>
#include "core/commandline.h"
#include "core/shell.h"

#ifdef __cplusplus
extern "C" {
#endif

// WiFi scanning and selection
void cmd_wifi_scan_start(int argc, char **argv);
void cmd_wifi_scan_stop(int argc, char **argv);
void cmd_wifi_scan_results(int argc, char **argv);
void handle_list(int argc, char **argv);
void handle_sta_scan(int argc, char **argv);
void handle_select_cmd(int argc, char **argv);
void handle_wifi_connection(int argc, char **argv);
void handle_wifi_disconnect(int argc, char **argv);
void handle_wifi_status(int argc, char **argv);
void handle_wifi_autoreconnect_cmd(int argc, char **argv);
#if defined(CONFIG_IDF_TARGET_ESP32P4)
void handle_p4_slave_ota_cmd(int argc, char **argv);
#endif
void handle_ip_lookup(int argc, char **argv);
void handle_track_ap_cmd(int argc, char **argv);
void handle_track_sta_cmd(int argc, char **argv);

// WiFi attacks
void handle_attack_cmd(int argc, char **argv);
void handle_beaconspam(int argc, char **argv);
void handle_hop_cmd(int argc, char **argv);
void handle_beaconadd(int argc, char **argv);
void handle_beaconremove(int argc, char **argv);
void handle_beaconclear(int argc, char **argv);
void handle_beaconshow(int argc, char **argv);
void handle_beaconspamlist(int argc, char **argv);
void handle_stop_spam(int argc, char **argv);
void handle_stop_deauth(int argc, char **argv);
void handle_sae_flood_cmd(int argc, char **argv);
void handle_stop_sae_flood_cmd(int argc, char **argv);
void handle_sae_flood_help_cmd(int argc, char **argv);

// Settings, time, timezone, web auth, WebUI AP, and config load
void handle_settings_cmd(int argc, char **argv);
void handle_settime_cmd(int argc, char **argv);
void handle_time_cmd(int argc, char **argv);
void handle_timezone_cmd(int argc, char **argv);
void handle_loadconfig_cmd(int argc, char **argv);
void handle_web_auth_cmd(int argc, char **argv);
void handle_webuiap_cmd(int argc, char **argv);

// BadUSB and USB keyboard host
void handle_badusb_cmd(int argc, char **argv);
void handle_usb_kbd_cmd(int argc, char **argv);
#ifdef CONFIG_HAS_USB_MSC_SD
void handle_usbsd_cmd(int argc, char **argv);
#endif
#ifdef CONFIG_HAS_BADBLE
void handle_badble_cmd(int argc, char **argv);
#endif

#if CONFIG_ENABLE_GHOSTSCRIPT
// GhostScript
void handle_script_cmd(int argc, char **argv);
#endif

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
// BLE, AirTag, Flipper, GATT, Chameleon, and BLE spam
void handle_ble_scan_cmd(int argc, char **argv);
void handle_ble_detect_cmd(int argc, char **argv);
void handle_ble_wardriving(int argc, char **argv);
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
void handle_dualwd(int argc, char **argv);
#endif
void handle_list_airtags_cmd(int argc, char **argv);
void handle_select_airtag(int argc, char **argv);
void handle_spoof_airtag(int argc, char **argv);
void handle_stop_spoof(int argc, char **argv);
void handle_list_flippers_cmd(int argc, char **argv);
void handle_select_flipper_cmd(int argc, char **argv);
void handle_list_gatt_cmd(int argc, char **argv);
void handle_select_gatt_cmd(int argc, char **argv);
void handle_enum_gatt_cmd(int argc, char **argv);
void handle_track_gatt_cmd(int argc, char **argv);
void handle_list_advertisers_cmd(int argc, char **argv);
void handle_ble_spam_cmd(int argc, char **argv);
void handle_chameleon_cmd(int argc, char **argv);
void ble_bridge_handle_command(int argc, char **argv);
#endif

// Network reconnaissance: port, ARP, SSH, NetBIOS, HTTP banner, SNMP,
// congestion, and probe listening.
void handle_scan_ports(int argc, char **argv);
void handle_scan_arp(int argc, char **argv);
void handle_scan_ssh(int argc, char **argv);
void handle_netbios_scan(int argc, char **argv);
void handle_http_banner_scan(int argc, char **argv);
void handle_snmp_probe(int argc, char **argv);
void handle_enum_scan(int argc, char **argv);
void handle_congestion_cmd(int argc, char **argv);
void handle_listen_probes_cmd(int argc, char **argv);
void handle_mdns_sniff(int argc, char **argv);
void handle_dhcpstarve_cmd(int argc, char **argv);

// Scan cancellation helpers (used by stop-all handler)
void port_scan_cancel(void);
void ssh_scan_cancel(void);
void netbios_scan_cancel(void);
void http_banner_scan_cancel(void);
void snmp_scan_cancel(void);

// Portal and DNS sinkhole commands
void handle_start_portal(int argc, char **argv);
void stop_portal(int argc, char **argv);
void handle_listportals(int argc, char **argv);
void handle_evilportal(int argc, char **argv);
void handle_sinkhole_cmd(int argc, char **argv);

// GPS commands
void handle_gps_info(int argc, char **argv);
void handle_gps_pin(int argc, char **argv);
void handle_gps_baud(int argc, char **argv);

#ifdef CONFIG_WITH_ETHERNET
// Ethernet
void handle_eth_up_cmd(int argc, char **argv);
void handle_eth_down_cmd(int argc, char **argv);
void handle_eth_fingerprint_cmd(int argc, char **argv);
void handle_eth_info_cmd(int argc, char **argv);
void handle_eth_arp_cmd(int argc, char **argv);
void handle_eth_ports_cmd(int argc, char **argv);
void handle_eth_ping_cmd(int argc, char **argv);
void handle_eth_dns_cmd(int argc, char **argv);
void handle_eth_trace_cmd(int argc, char **argv);
void handle_eth_stats_cmd(int argc, char **argv);
void handle_eth_config_cmd(int argc, char **argv);
void handle_eth_mac_cmd(int argc, char **argv);
void handle_eth_serv_cmd(int argc, char **argv);
void handle_eth_ntp_cmd(int argc, char **argv);
void handle_eth_http_cmd(int argc, char **argv);
void handle_eth_poison_cmd(int argc, char **argv);
void eth_cmd_set_scan_cancel(bool cancel);
#endif

// Wardriver streaming command
void handle_wdstream_cmd(int argc, char **argv);
bool wdstream_stop_and_wait(const char *reason);

// Diagnostics commands
void handle_stop_flipper(int argc, char **argv);
void handle_dial_command(int argc, char **argv);
void handle_mem_cmd(int argc, char **argv);
void handle_nfc_cmd(int argc, char **argv);
void handle_nfctest_cmd(int argc, char **argv);
bool nfc_cli_stop(void);
void handle_tp_link_test(int argc, char **argv);
void handle_wol_cmd(int argc, char **argv);
void handle_govee_cmd(int argc, char **argv);
void handle_status_idle_cmd(int argc, char **argv);
void handle_unknown_command(const char *cmd);

// System commands
void handle_reboot(int argc, char **argv);
void handle_startwd(int argc, char **argv);
void handle_crash(int argc, char **argv);
void handle_coredump_cmd(int argc, char **argv);
void handle_wpa3_compliance(int argc, char **argv);
void handle_pineap_detection(int argc, char **argv);
void handle_ap_enable_cmd(int argc, char **argv);
void handle_chip_info_cmd(int argc, char **argv);
void handle_devices_cmd(int argc, char **argv);
void handle_ir_pin_cmd(int argc, char **argv);
void handle_mirror_cmd(int argc, char **argv);
void handle_apps_cmd(int argc, char **argv);
void handle_log_level_cmd(int argc, char **argv);
void handle_fav_cmd(int argc, char **argv);
void handle_setcountry(int argc, char **argv);

// Capture commands
void handle_capture_scan(int argc, char **argv);
void handle_capture(int argc, char **argv);

// Help command
void handle_help(int argc, char **argv);

// AP credentials command
void handle_apcred(int argc, char **argv);

// RGB effect commands
void handle_rgb_mode(int argc, char **argv);
void handle_setrgb(int argc, char **argv);
void handle_setrgbcount(int argc, char **argv);

// Scan-all and sweep commands
void handle_scanall(int argc, char **argv);
void handle_sweep_cmd(int argc, char **argv);

typedef struct {
    int ap_count;
    int station_count;
    int flipper_count;
    int gatt_count;
    int zigbee_count;
    int current_phase;
    int total_phases;
    bool done;
    bool running;
} sweep_result_t;

void sweep_start_async(int wifi_seconds, int ble_seconds);
bool sweep_check_done(void);
void sweep_finish_async(void);
bool sweep_is_running(void);
const sweep_result_t* sweep_get_result(void);
void sweep_clear_result(void);

// Audio commands
void handle_audio_cmd(int argc, char **argv);
#ifdef CONFIG_HAS_MIC
void handle_mic_cal_cmd(int argc, char **argv);
#endif

// WiGLE commands
void handle_wigle_cmd(int argc, char **argv);

// Karma commands
void handle_karma_cmd(int argc, char **argv);

// RGB and Neopixel commands
void handle_set_rgb_mode_cmd(int argc, char **argv);
void handle_set_neopixel_brightness_cmd(int argc, char **argv);
void handle_get_neopixel_brightness_cmd(int argc, char **argv);

// Input / IO button / visualizer commands
void handle_raveport_cmd(int argc, char **argv);
void handle_rave_cmd(int argc, char **argv);
void handle_identify_cmd(int argc, char **argv);
void handle_input_cmd(int argc, char **argv);
void handle_iobtn_cmd(int argc, char **argv);

// Communication commands
void handle_comm_discovery(int argc, char **argv);
void handle_comm_connect(int argc, char **argv);
void handle_comm_send(int argc, char **argv);
void handle_comm_status(int argc, char **argv);
void handle_comm_disconnect(int argc, char **argv);
void handle_comm_setpins(int argc, char **argv);
void cmd_comm_register_callback(void);

// GhostLink peer-flashing commands (see managers/peer_ota_manager.c)
void handle_otarecv_cmd(int argc, char **argv);
void handle_otastatus_cmd(int argc, char **argv);
void handle_otaabort_cmd(int argc, char **argv);
void handle_otainfo_cmd(int argc, char **argv);

// Aerial and Flock commands
void handle_aerial_scan_cmd(int argc, char **argv);
void handle_aerial_list_cmd(int argc, char **argv);
void handle_aerial_track_cmd(int argc, char **argv);
void handle_aerial_stop_cmd(int argc, char **argv);
void handle_aerial_spoof_cmd(int argc, char **argv);
void handle_aerial_spoof_stop_cmd(int argc, char **argv);
void handle_flock_scan_cmd(int argc, char **argv);
void handle_flock_list_cmd(int argc, char **argv);
void handle_flock_stop_cmd(int argc, char **argv);

#ifdef CONFIG_HAS_CAMERA
// Camera commands
void handle_motion_cmd(int argc, char **argv);
void handle_camerastream_cmd(int argc, char **argv);
#endif

// SD card commands
void handle_sd_cmd(int argc, char **argv);
void handle_sd_config(int argc, char **argv);
void handle_sd_pins_mmc(int argc, char **argv);
void handle_sd_pins_spi(int argc, char **argv);
void handle_sd_save_config(int argc, char **argv);

// Infrared commands
void handle_ir_cmd(int argc, char **argv);
bool cmd_ir_stop_universal_send(void);

// SubGHz
void handle_subghz_cmd(int argc, char **argv);

// NRF24 analyzer
void handle_nrf24_cmd(int argc, char **argv);

// LoRa (SX1262/SX1276/LLCC68, Meshtastic-lite target)
void handle_lora_cmd(int argc, char **argv);

// Printer command
void handle_printer_command(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif // COMMANDS_H
