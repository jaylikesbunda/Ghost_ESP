#include "managers/views/options_screen.h"
#include "managers/views/lockscreen.h"
#include "managers/views/favorites_manager_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h"
#include "core/ouis.h"
#include "core/chip_info.h"
#include "core/ghostesp_version.h"
#include "managers/display_manager.h"
#include "gui/options_view.h"
#include "gui/touch_bar.h"
#include "core/screen_mirror.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/accessibility_fonts.h"
#include "gui/asset_pack.h"
#include "gui/theme_palette_api.h"
#include "esp_attr.h"
#include "gui/design_tokens.h"
#include "gui/ios_toggle.h"
#include "io_manager.h"
#include "esp_wifi.h"
#include "managers/views/airspace_monitor_screen.h"
#include "managers/views/channel_congestion_screen.h"
#include "managers/views/hop_profile_screen.h"
#include "managers/views/packet_monitor_screen.h"
#include "managers/views/wardriving_screen.h"
#include "managers/views/ethernet_screen.h"
#include "managers/wigle_manager.h"
#include "managers/config_manager.h"
#include "managers/settings_sd_backup.h"
#include "managers/ota_manager.h"
#include "managers/peer_ota_manager.h"
#include "managers/self_ota_manager.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "gui/popup.h"
#include "gui/toast.h"
#include "core/utils.h"
#include "managers/sd_card_manager.h"  /* MAX_PORTAL_NAME, sd_card_list_dir_paged */
#include "managers/usb_msc_manager.h"
#include "esp_err.h"
#include "gui/paged_menu.h"
#include "gui/scan_status.h"
#include "gui/detail_view.h"
#include "gui/rssi_meter.h"
#include "gui/nav_history.h"
#include "gui/gui_router.h"
#include "gui/select_overlay.h"
#include "managers/views/menu_editor_screen.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/wpa3_compliance.h"
#include "managers/ble_manager.h"
#include "managers/status_display_manager.h"
#include "managers/ble_bridge_manager.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/device_detect_scan.h"
#include "scans/ble/gatt_scan.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/arp_scan.h"
#include "scans/wifi/govee_scan.h"
#include "scans/wifi/enum4linux_scan.h"
#include "core/commands.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "core/dns_server.h"
#include "vendor/pcap.h"
#include "esp_heap_caps.h"
#include <dirent.h>

#define PORTAL_PAGE_SIZE 8    /* keep portal pages small to avoid LVGL stalls */
#define WIGLE_CSV_PAGE_SIZE 8
#define PCAP_CAPTURE_PAGE_SIZE 8

static detail_view_t *sinkhole_detail_view = NULL;
static void sinkhole_detail_back_cb(lv_event_t *e);
static popup_confirm_t *settings_confirm_popup = NULL;

static char selected_portal[MAX_PORTAL_NAME] = {0};
static char selected_karma_portal[MAX_PORTAL_NAME] = {0};

static char *evil_portal_names = NULL;   /* flat name storage for current page */
static const char **evil_portal_options = NULL; /* NULL-terminated pointer array  */
static int   portal_page_offset   = 0;   /* first file index of current page    */
static bool  portal_has_next_page = false;

static char *wigle_csv_names = NULL;
static const char **wigle_csv_options = NULL;
static int wigle_csv_page_offset = 0;
static bool wigle_csv_has_next_page = false;

static char *pcap_capture_names = NULL;
static const char **pcap_capture_options = NULL;
static int pcap_capture_page_offset = 0;

static char *blocklist_file_names = NULL;
static const char **blocklist_file_options = NULL;
static int blocklist_page_offset = 0;
static bool blocklist_has_next_page = false;

static void blocklist_free_cache(void);
static const char **blocklist_load_page(void);
#define BLOCKLIST_PAGE_SIZE 8
static bool wigle_csv_browser_active = false;
static char selected_wigle_csv[MAX_PORTAL_NAME] = {0};

#define STA_LIST_PAGE_SIZE 10
#define SCANALL_LIST_PAGE_SIZE 8
#define BLE_DETECT_LIST_PAGE_SIZE 8
#define BLE_ADV_LIST_PAGE_SIZE 8
#define BLE_GATT_LIST_PAGE_SIZE 8
#define BLE_OUI_VENDOR_MAX_RESULTS 24
#ifdef CONFIG_IDF_TARGET_ESP32C5
#define AP_SCAN_ESTIMATE_SECONDS 6
#else
#define AP_SCAN_ESTIMATE_SECONDS 5
#endif
#define STA_SCAN_MAX_DURATION_MS 45000
#define NAV_SCOPE_WIFI_DETAIL_RETURN 0x5744464Cu
#define NAV_SCOPE_OPTIONS_MENU       0x4F50544Eu
static scan_status_t *ap_scan_status = NULL;
static detail_view_t *ap_detail_view = NULL;

/* Set while a menu is being built for a windowed (virtual) list, so the
 * string-based list path is skipped and the batched row builder stays out of
 * the way. */
static bool s_vlist_pending = false;
static rssi_meter_t *track_meter = NULL; /* live RSSI ring overlay for Track AP/STA/BLE */

/* Which hardware source feeds the live RSSI ring overlay, so teardown stops the
 * right tracker and returns to the list it was launched from. */
typedef enum {
    TRACK_SRC_NONE = 0,
    TRACK_SRC_WIFI_AP,
    TRACK_SRC_WIFI_STA,
    TRACK_SRC_BLE_ADV,
    TRACK_SRC_BLE_GATT,
    TRACK_SRC_BLE_DETECT,
} track_source_t;
static track_source_t track_source = TRACK_SRC_NONE;
static int selected_ap_index = -1;
static char ap_connect_ssid[64] = {0};
static lv_timer_t *ap_scan_poll_timer = NULL;
static int64_t ap_scan_ui_start_time = 0;
static paged_menu_t *scanall_list_menu = NULL;
static paged_menu_t *sta_list_menu = NULL;
static scan_status_t *sta_scan_status = NULL;
static detail_view_t *sta_detail_view = NULL;
static paged_menu_t *ble_detect_list_menu = NULL;
static detail_view_t *ble_detect_detail_view = NULL;
static lv_timer_t *ble_detect_poll_timer = NULL;
static scan_status_t *ble_detect_status = NULL;
static paged_menu_t *ble_adv_list_menu = NULL;
static detail_view_t *ble_adv_detail_view = NULL;
static lv_timer_t *ble_adv_poll_timer = NULL;
static scan_status_t *ble_adv_status = NULL;
static paged_menu_t *ble_gatt_list_menu = NULL;
static detail_view_t *ble_gatt_detail_view = NULL;
static lv_timer_t *ble_gatt_poll_timer = NULL;
static scan_status_t *ble_gatt_status = NULL;
#if GHOSTESP_OTA_SUPPORTED
static scan_status_t *ota_status_overlay = NULL;
static lv_timer_t *ota_status_poll_timer = NULL;
static popup_confirm_t *ota_result_popup = NULL;
static int64_t ota_status_started_us = 0;
static bool ota_status_watch_device = false;
static bool ota_status_watch_peer = false;
static bool ota_status_watch_self = false;
static char ota_last_self_failure_notice[128] = {0};
typedef enum {
    OTA_UI_MODE_NONE = 0,
    OTA_UI_MODE_CHECK,
    OTA_UI_MODE_INSTALL,
    OTA_UI_MODE_PEER_CHECK,
    OTA_UI_MODE_PEER_INSTALL,
    OTA_UI_MODE_SD_INSTALL,
} ota_ui_mode_t;
static ota_ui_mode_t ota_ui_mode = OTA_UI_MODE_NONE;
#endif
static char gtk_abuse_ssid[33];
static scan_status_t *gtk_abuse_status = NULL;
static detail_view_t *gtk_abuse_detail_view = NULL;
static int ble_detect_last_count = -1;
static int selected_ble_detect_index = -1;
static int ble_adv_last_count = -1;
static int selected_ble_adv_index = -1;
static int ble_gatt_last_count = -1;
static int selected_ble_gatt_index = -1;
static char (*ble_oui_vendor_names)[64];
static const char *ble_oui_vendor_options[BLE_OUI_VENDOR_MAX_RESULTS + 2];
static int ble_oui_vendor_count = 0;
static int selected_station_index = -1;
static lv_timer_t *sta_scan_poll_timer = NULL;
static int64_t sta_scan_start_time = 0;
static int sta_scan_last_count = 0;
static bool sta_scan_stopped_by_user = false;
static bool scan_all_flow_active = false;
static bool scan_all_started_station_phase = false;
static bool station_scan_waiting_for_ap_scan = false;

static bool *g_ap_multi_selected = NULL;
static int g_ap_multi_count = 0;
static paged_menu_t *ap_multi_menu = NULL;

/* Detail/spinner UI can outlive options_menu_view.root, so lockscreen entry
 * snapshots logical state and tears down the transient LVGL objects. */
typedef enum {
    RESUME_NONE = 0,
    RESUME_AP_DETAIL,
    RESUME_STA_DETAIL,
    RESUME_BLE_DETECT_DETAIL,
    RESUME_BLE_ADV_DETAIL,
    RESUME_BLE_GATT_DETAIL,
} pending_detail_resume_t;

static pending_detail_resume_t s_pending_detail_resume = RESUME_NONE;
static int s_pending_detail_index = -1;
static int g_freeze_hook_id = -1;

static void options_menu_apply_pending_detail_resume(void);
static void options_menu_freeze_pre_lock(void);
static void close_all_scan_status_overlays(void);

static bool *g_sta_multi_selected = NULL;
static int g_sta_multi_count = 0;
static paged_menu_t *sta_multi_menu = NULL;

// ARP scan flow
#define ARP_LIST_PAGE_SIZE 10
static paged_menu_t *arp_list_menu = NULL;
static scan_status_t *arp_scan_status = NULL;
static detail_view_t *arp_detail_view = NULL;
static lv_timer_t *arp_scan_poll_timer = NULL;
static int selected_arp_index = -1;
static bool arp_scan_cancel_requested = false;

// Govee LAN discovery flow
#define GOVEE_LIST_PAGE_SIZE 8
static paged_menu_t *govee_list_menu = NULL;
static scan_status_t *govee_scan_status = NULL;
static detail_view_t *govee_detail_view = NULL;
static lv_timer_t *govee_scan_poll_timer = NULL;
static int selected_govee_index = -1;
static bool govee_scan_cancel_requested = false;

// mDNS discovery flow
#define MDNS_LIST_PAGE_SIZE 8
static paged_menu_t *mdns_list_menu = NULL;
static scan_status_t *mdns_scan_status = NULL;
static detail_view_t *mdns_detail_view = NULL;
static lv_timer_t *mdns_scan_poll_timer = NULL;
static int selected_mdns_index = -1;
static bool mdns_scan_cancel_requested = false;

// Enum4linux scan flow
#define ENUM_LIST_PAGE_SIZE 10
static paged_menu_t *enum_list_menu = NULL;
static scan_status_t *enum_scan_status = NULL;
static detail_view_t *enum_detail_view = NULL;
static lv_timer_t *enum_scan_poll_timer = NULL;
static int selected_enum_index = -1;
static bool enum_scan_cancel_requested = false;

// Sweep flow
static scan_status_t *sweep_scan_status = NULL;
static detail_view_t *sweep_detail_view = NULL;
static lv_timer_t *sweep_poll_timer = NULL;

static bool start_ap_scan_flow(void);
static void station_format_mac(const uint8_t mac[6], char *out, size_t out_size);
static void scanall_select_row(int row_idx);
static int scanall_total_rows(uint16_t ap_count, wifi_ap_record_t *aps);
static bool scanall_row_to_indices(int row_idx, uint16_t ap_count, wifi_ap_record_t *aps,
                                   bool *is_station_row_out, int *ap_index_out,
                                   int *station_index_out);
static void sanitize_recolor_text(char *text);
static void station_lookup_ap_ssid(const uint8_t ap_bssid[6], char *ssid_out,
                                   size_t ssid_out_size);

/* Windowed (virtual) result lists. Each kind is one scan-result list shown as a
 * fixed row pool instead of one button per record behind page rows. */
typedef enum {
    VLIST_NONE = 0,
    VLIST_AP,
    VLIST_STA,
    VLIST_SCANALL,
    VLIST_ARP,
    VLIST_GOVEE,
    VLIST_MDNS,
    VLIST_ENUM,
    VLIST_BLE_DETECT,
    VLIST_BLE_ADV,
    VLIST_BLE_GATT,
} vlist_kind_t;

static vlist_kind_t vlist_kind_for_state(void);
static void vlist_activate(int index, void *user_data);
static void vlist_show(void);
static const char **sta_list_get_options(void);
static const char **scanall_list_get_options(void);
static const char **ble_detect_list_get_options(void);
static const char **ble_adv_list_get_options(void);
static const char **ble_gatt_list_get_options(void);
static const char **ble_oui_vendor_list_get_options(void);
static void ble_detect_poll_timer_cb(lv_timer_t *timer);
static void ble_adv_poll_timer_cb(lv_timer_t *timer);
static void ble_gatt_poll_timer_cb(lv_timer_t *timer);

static bool start_arp_scan_flow(void);
static void arp_scan_poll_timer_cb(lv_timer_t *timer);
static void arp_scan_complete_callback(void);
static void arp_list_cleanup(void);
static const char **arp_list_get_options(void);
static void show_arp_detail(int index);

static bool start_govee_scan_flow(void);
static void govee_scan_poll_timer_cb(lv_timer_t *timer);
static void govee_scan_complete_callback(void);
static void govee_list_cleanup(void);
static const char **govee_list_get_options(void);
static void show_govee_detail(int index);
static void govee_brightness_kb_cb(const char *text);
static void govee_color_kb_cb(const char *text);

static bool start_mdns_scan_flow(void);
static void mdns_scan_poll_timer_cb(lv_timer_t *timer);
static void mdns_scan_complete_callback(void);
static void mdns_list_cleanup(void);
static const char **mdns_list_get_options(void);
static void show_mdns_detail(int index);

static bool start_enum_scan_flow(void);
static void enum_scan_poll_timer_cb(lv_timer_t *timer);
static void enum_scan_complete_callback(void);
static void enum_list_cleanup(void);
static const char **enum_list_get_options(void);
static void show_enum_detail(int index);

static bool start_sweep_flow(void);
static void sweep_poll_timer_cb(lv_timer_t *timer);
static void sweep_complete_callback(void);
static void show_sweep_detail(void);
static void ap_scan_complete_callback(void);
static void ap_detail_back_cb(lv_event_t *e);
static void ap_scan_poll_timer_cb(lv_timer_t *timer);
static void ap_list_cleanup(void);
static bool start_scan_all_flow(void);
static void scanall_list_cleanup(void);
static bool start_station_scan_flow(void);
static bool start_station_scan_with_ap_scan(void);
static void station_scan_poll_timer_cb(lv_timer_t *timer);
static void station_scan_complete_callback(void);
static void stop_station_scan_flow(void);
static bool should_stop_station_scan_on_input(const InputEvent *event);
static void station_detail_back_cb(lv_event_t *e);
static void stop_track_flow(void);
static void track_stop_current_source(void);
static void start_track_overlay(track_source_t src, const char *status_title,
                                const char *target_label,
                                rssi_meter_sample_cb sampler);
static bool track_meter_sample_ble_adv(void *user, int8_t *out_rssi);
static bool track_meter_sample_ble_gatt(void *user, int8_t *out_rssi);
static bool track_meter_sample_ble_detect(void *user, int8_t *out_rssi);
static bool track_exit_requested(const InputEvent *event);
static void show_station_detail(int station_index);
static void station_list_cleanup(void);
static bool start_ble_detect_flow(void);
static void stop_ble_detect_flow(void);
static void ble_detect_list_cleanup(void);
static void ble_detect_detail_back_cb(lv_event_t *e);
static void show_ble_detect_detail(int device_index);
static bool start_ble_adv_flow(void);
static bool start_ble_oui_prefix_flow(const uint8_t oui[3]);
static bool start_ble_oui_vendor_flow(const char *vendor);
static void stop_ble_adv_flow(void);
static void ble_adv_list_cleanup(void);
static void ble_oui_vendor_clear(void);
static void ble_adv_detail_back_cb(lv_event_t *e);
static void ble_adv_track_cb(lv_event_t *e);
static void ble_adv_save_cb(lv_event_t *e);
static void show_ble_adv_detail(int device_index);
static bool start_ble_gatt_flow(void);
static void stop_ble_gatt_flow(void);
static void ble_gatt_list_cleanup(void);
static void ble_gatt_detail_back_cb(lv_event_t *e);
static void ble_gatt_track_cb(lv_event_t *e);
static void ble_gatt_enum_cb(lv_event_t *e);
static void show_ble_gatt_detail(int device_index);
static void gtk_abuse_back_cb(lv_event_t *e);

static int ap_multi_select_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data);
static void ap_multi_select_toggle(int ap_index);
static void ap_multi_select_all(void);
static void ap_multi_select_none(void);
static void ap_multi_select_confirm(void);
static void ap_multi_select_cleanup(void);
static void ap_multi_select_back_cb(lv_event_t *e);
static void ap_multi_select_handle_selection(const char *option, void *user_data);
static const char **ap_multi_select_get_options(void);

static int sta_multi_select_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data);
static void sta_multi_select_toggle(int sta_index);
static void sta_multi_select_all(void);
static void sta_multi_select_none(void);
static void sta_multi_select_confirm(void);
static void sta_multi_select_cleanup(void);
static void sta_multi_select_back_cb(lv_event_t *e);
static void sta_multi_select_handle_selection(const char *option, void *user_data);
static const char **sta_multi_select_get_options(void);
static bool multi_select_option_is_toggled(int option_index, const char *option);
static void style_multi_select_row(lv_obj_t *btn, bool toggled);

static bool use_compact_wifi_detail_layout(void) {
    return (LV_HOR_RES > LV_VER_RES && LV_VER_RES <= 160);
}

static void arp_detail_back_cb(lv_event_t *e);
static void mdns_detail_back_cb(lv_event_t *e);
static void sweep_detail_back_cb(lv_event_t *e);
static void reserve_detail_touch_bar_space(detail_view_t *dv);

static bool handle_wifi_detail_keyboard(uint8_t key_value) {
    detail_view_t *active_detail = NULL;
    lv_event_cb_t back_cb = NULL;

    if (ap_detail_view) {
        active_detail = ap_detail_view;
        back_cb = ap_detail_back_cb;
    } else if (gtk_abuse_detail_view) {
        active_detail = gtk_abuse_detail_view;
        back_cb = gtk_abuse_back_cb;
    } else if (sta_detail_view) {
        active_detail = sta_detail_view;
        back_cb = station_detail_back_cb;
    } else if (ble_detect_detail_view) {
        active_detail = ble_detect_detail_view;
        back_cb = ble_detect_detail_back_cb;
    } else if (ble_adv_detail_view) {
        active_detail = ble_adv_detail_view;
        back_cb = ble_adv_detail_back_cb;
    } else if (ble_gatt_detail_view) {
        active_detail = ble_gatt_detail_view;
        back_cb = ble_gatt_detail_back_cb;
    } else if (arp_detail_view) {
        active_detail = arp_detail_view;
        back_cb = arp_detail_back_cb;
    } else if (mdns_detail_view) {
        active_detail = mdns_detail_view;
        back_cb = mdns_detail_back_cb;
    } else if (sweep_detail_view) {
        active_detail = sweep_detail_view;
        back_cb = sweep_detail_back_cb;
    }

    if (!active_detail) {
        return false;
    }

    if (key_value == LV_KEY_UP || key_value == 'k' || key_value == ';') {
        detail_view_step_up(active_detail);
        return true;
    }

    if (key_value == LV_KEY_DOWN || key_value == 'j' || key_value == '.') {
        detail_view_step_down(active_detail);
        return true;
    }

    if (key_value == LV_KEY_LEFT || key_value == 44 || key_value == ',' || key_value == 'h' ||
        key_value == LV_KEY_ESC || key_value == 29 || key_value == '`') {
        if (back_cb) back_cb(NULL);
        return true;
    }

    if (key_value == LV_KEY_RIGHT || key_value == 47 || key_value == '/' || key_value == 'l' ||
        key_value == LV_KEY_ENTER || key_value == 13) {
        lv_obj_t *obj = detail_view_get_selected_obj(active_detail);
        if (obj && lv_obj_is_valid(obj)) {
            lv_event_send(obj, LV_EVENT_CLICKED, NULL);
        }
        return true;
    }

    return false;
}

static bool start_scan_all_flow(void) {
    scanall_list_cleanup();
    station_list_cleanup();
    ap_list_cleanup();

    scan_all_flow_active = true;
    scan_all_started_station_phase = false;

    if (!start_ap_scan_flow()) {
        scan_all_flow_active = false;
        return false;
    }

    return true;
}

static bool start_ap_scan_flow(void) {
    ap_list_cleanup();
    ap_scan_status = scan_status_create("Scanning APs");
    if (ap_scan_status) {
        char wait_msg[48];
        snprintf(wait_msg, sizeof(wait_msg), "Please wait %d seconds", AP_SCAN_ESTIMATE_SECONDS);
        scan_status_set_subtext(ap_scan_status, wait_msg);
    }

    esp_err_t err = ap_scan_start_async();
    if (err != ESP_OK) {
        if (ap_scan_status) {
            scan_status_close(ap_scan_status);
            ap_scan_status = NULL;
        }
        return false;
    }

    ap_scan_ui_start_time = esp_timer_get_time();
    ap_scan_poll_timer = lv_timer_create(ap_scan_poll_timer_cb, 100, NULL);
    return true;
}

static void ap_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (ap_scan_status) {
        int64_t elapsed_ms = (esp_timer_get_time() - ap_scan_ui_start_time) / 1000;
        int elapsed_seconds = (int)(elapsed_ms / 1000);
        int remaining = AP_SCAN_ESTIMATE_SECONDS - elapsed_seconds;
        if (remaining < 1) {
            remaining = 1;
        }

        char wait_msg[48];
        snprintf(wait_msg, sizeof(wait_msg), "Please wait %d second%s", remaining, (remaining == 1) ? "" : "s");
        scan_status_set_subtext(ap_scan_status, wait_msg);
    }
    
    if (ap_scan_check_done()) {
        lv_timer_del(ap_scan_poll_timer);
        ap_scan_poll_timer = NULL;
        ap_scan_finish_async();
        ap_scan_complete_callback();
    }
}

static void station_scan_set_subtext(int found_count) {
    if (!sta_scan_status) {
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Use any input to finish scan\n%d found", found_count);
    scan_status_set_subtext(sta_scan_status, msg);
}

static bool start_station_scan_flow(void) {
    station_list_cleanup();
    station_scan_clear_results();

    sta_scan_status = scan_status_create("Scanning Stations");
    station_scan_set_subtext(0);

    station_scan_start();
    if (!station_scan_is_active()) {
        if (sta_scan_status) {
            scan_status_close(sta_scan_status);
            sta_scan_status = NULL;
        }
        return false;
    }

    sta_scan_start_time = esp_timer_get_time();
    sta_scan_last_count = 0;
    sta_scan_stopped_by_user = false;
    sta_scan_poll_timer = lv_timer_create(station_scan_poll_timer_cb, 100, NULL);
    return true;
}

static bool start_station_scan_with_ap_scan(void) {
    if (ap_scan_get_count() > 0) {
        return start_station_scan_flow();
    }

    station_list_cleanup();
    station_scan_clear_results();
    station_scan_waiting_for_ap_scan = true;
    if (start_ap_scan_flow()) {
        return true;
    }

    station_scan_waiting_for_ap_scan = false;
    return false;
}

static void station_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_ms = (now_us - sta_scan_start_time) / 1000;
    int count = station_scan_get_count();

    if (count != sta_scan_last_count) {
        sta_scan_last_count = count;
        station_scan_set_subtext(count);
    }

    if (!station_scan_is_active()) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
        station_scan_complete_callback();
        return;
    }

    if (elapsed_ms < STA_SCAN_MAX_DURATION_MS) {
        return;
    }

    if (station_scan_is_active()) {
        station_scan_stop();
    }

    lv_timer_del(sta_scan_poll_timer);
    sta_scan_poll_timer = NULL;
    station_scan_complete_callback();
}

static bool should_stop_station_scan_on_input(const InputEvent *event) {
    if (!event) {
        return false;
    }

    switch (event->type) {
        case INPUT_TYPE_TOUCH:
            return event->data.touch_data.state == LV_INDEV_STATE_PR;
        case INPUT_TYPE_JOYSTICK:
        case INPUT_TYPE_KEYBOARD:
        case INPUT_TYPE_EXIT_BUTTON:
            return true;
        case INPUT_TYPE_ENCODER:
            return event->data.encoder.button || (event->data.encoder.direction != 0);
        default:
            return false;
    }
}

static void stop_station_scan_flow(void) {
    sta_scan_stopped_by_user = true;
    if (station_scan_is_active()) {
        station_scan_stop();
    }
    if (sta_scan_poll_timer) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
    }
    station_scan_complete_callback();
}

static void ble_detect_set_subtext(int found_count) {
    if (!ble_detect_status) {
        return;
    }

    char msg[96];
    snprintf(msg, sizeof(msg), "Use any input to finish scan\n%d found", found_count);
    scan_status_set_subtext(ble_detect_status, msg);
}

static void ble_adv_set_subtext(int found_count) {
    if (!ble_adv_status) {
        return;
    }

    char msg[128];
    const char *filter = advertiser_scan_get_filter_label();
    if (filter != NULL) {
        snprintf(msg, sizeof(msg), "Use any input to finish scan\n%d matches\n%s",
                 found_count, filter);
    } else {
        snprintf(msg, sizeof(msg), "Use any input to finish scan\n%d advertisers", found_count);
    }
    scan_status_set_subtext(ble_adv_status, msg);
}


#include "managers/views/keyboard_screen.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/views/error_popup.h"
#include "core/esp_comm_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/number_pad_screen.h"
#include "managers/views/setup_wizard_screen.h"
#include "managers/wifi_manager.h"
#include "core/wpa_crypto.h"
#include "attacks/wifi/gtk_abuse.h"
#include "managers/settings_manager.h"
#include "managers/infrared_manager.h"
#include "managers/gps_manager.h"
#include "managers/rgb_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/nrf24_remote_manager.h"
#include "esp_log.h"
#include "core/glog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "managers/views/keyboard_screen.h"
#include "managers/usb_keyboard_manager.h"
#include "managers/views/badusb_view.h"
#ifdef CONFIG_HAS_BADBLE
#include "managers/views/badble_view.h"
#endif
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif
#include "managers/views/nfc_view.h"
#include "managers/views/compass_screen.h"
#include "managers/views/enviii_screen.h"
#include "managers/views/accelerometer_screen.h"
#include "managers/views/clock_screen.h"
#include "managers/views/app_gallery_screen.h"
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
#include "managers/views/nrf24_analyzer_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
extern const lv_img_dsc_t ghostesplogo;


#define KARMA_MAX_SSIDS 64

static const char *TAG = "optionsScreen";

#if GHOSTESP_OTA_SUPPORTED
static void ota_result_dismiss_cb(void *user_data) {
    (void)user_data;
}

static void ota_status_start_overlay(ota_ui_mode_t mode, const char *title, const char *subtext);
static void ota_status_show_result(const char *title, const char *body);

static bool ota_state_busy(OtaState state) {
    return state == OTA_STATE_CHECKING || state == OTA_STATE_DOWNLOADING ||
           state == OTA_STATE_VERIFYING;
}

static bool peer_ota_state_busy(PeerOtaState state) {
    return state == PEER_OTA_STATE_CHECKING || state == PEER_OTA_STATE_SENDING ||
           state == PEER_OTA_STATE_WAITING_PEER;
}

static bool self_ota_state_busy(SelfOtaState state) {
    return state == SELF_OTA_STATE_CHECKING || state == SELF_OTA_STATE_DOWNLOADING ||
           state == SELF_OTA_STATE_VERIFYING || state == SELF_OTA_STATE_FLASHING;
}

static const char *ota_build_relation(long available_build, long current_build) {
    if (available_build <= 0 || current_build < 0) return "available";
    if (available_build > current_build) return "newer";
    if (available_build < current_build) return "older";
    return "same build";
}

static void ota_append_available_line(char *body, size_t body_len, const char *label,
                                      const char *version, long available_build, long current_build) {
    if (!body || body_len == 0 || !label) return;
    size_t used = strlen(body);
    if (used >= body_len - 1) return;
    int n = snprintf(body + used, body_len - used, "%s%s: %s",
                     used > 0 ? "\n" : "", label,
                     (version && version[0]) ? version : "available");
    if (n < 0) return;
    used = strlen(body);
    if (used >= body_len - 1) return;
    const char *relation = ota_build_relation(available_build, current_build);
    if (available_build > 0) {
        snprintf(body + used, body_len - used, " (%s, build %ld)", relation, available_build);
    } else {
        snprintf(body + used, body_len - used, " (%s)", relation);
    }
}

static void ota_status_close_overlay(void) {
    if (ota_status_poll_timer) {
        lv_timer_del(ota_status_poll_timer);
        ota_status_poll_timer = NULL;
    }
    if (ota_status_overlay) {
        scan_status_close(ota_status_overlay);
        ota_status_overlay = NULL;
    }
    ota_ui_mode = OTA_UI_MODE_NONE;
    ota_status_started_us = 0;
    ota_status_watch_device = false;
    ota_status_watch_peer = false;
    ota_status_watch_self = false;
}

static bool ota_status_handle_cancel_input(InputEvent *event) {
    if (!event || !ota_status_overlay || ota_ui_mode != OTA_UI_MODE_INSTALL ||
        !ota_status_watch_device) {
        return false;
    }

    OtaStatus ota = ota_manager_get_status();
    if (ota.state != OTA_STATE_DOWNLOADING) {
        return false;
    }

    ota_manager_cancel_update();
    scan_status_set_subtext(ota_status_overlay, "Cancelling update...");
    ota_status_close_overlay();
    ota_status_show_result("Update Cancelled", "Firmware download cancelled.");
    return true;
}

static void ota_status_show_result(const char *title, const char *body) {
    popup_confirm_show(&ota_result_popup, lv_layer_top(), title, body, "Close", NULL,
                       ota_result_dismiss_cb, NULL);
}

static void ota_start_device_install(void) {
    if (!ota_manager_is_supported() && !self_ota_manager_is_supported()) {
        ota_status_show_result("Manual Flash Required", "This board reflashes manually. See the release notes.");
        return;
    }

    esp_err_t err = ota_manager_is_supported() ? ota_manager_start_update()
                                                : self_ota_manager_start_update();
    if (err != ESP_OK) {
        ota_status_show_result("No Update Ready", "Check for updates first.");
    } else if (self_ota_manager_is_supported()) {
        ota_status_start_overlay(OTA_UI_MODE_INSTALL, "Starting updater...",
                                 "Rebooting to updater. Keep powered on.");
    } else {
        ota_status_start_overlay(OTA_UI_MODE_INSTALL, "Installing update...", "Preparing firmware");
    }
}

static void ota_confirm_device_install_cb(void *user_data) {
    (void)user_data;
    ota_start_device_install();
}

static bool ota_sd_install_available(void) {
    return ota_manager_is_supported();
}

static void ota_start_sd_install(void) {
    esp_err_t err = ota_manager_start_update_from_sd();
    if (err == ESP_ERR_INVALID_STATE) {
        ota_status_show_result("SD Update", "SD card not mounted.");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        ota_status_show_result("SD Update Unavailable", "SD card firmware install is not available on this board.");
    } else if (err == ESP_OK) {
        ota_status_start_overlay(OTA_UI_MODE_SD_INSTALL, "Checking SD card...", "Looking for firmware_update.bin");
    } else {
        ota_status_show_result("SD Update", "Failed to start SD update.");
    }
}

static void ota_confirm_sd_install_cb(void *user_data) {
    (void)user_data;
    ota_start_sd_install();
}

static void ota_show_sd_install_confirm(void) {
    if (!ota_sd_install_available()) {
        ota_status_show_result("SD Update Unavailable", "SD card firmware install is not available on this board.");
        return;
    }
    if (!sd_card_manager.is_initialized) {
        ota_status_show_result("SD Update", "SD card not mounted.");
        return;
    }

    popup_confirm_show(&settings_confirm_popup, lv_layer_top(), "Install from SD Card?",
                       "Put firmware.bin in /mnt/ghostesp.\nDo not use merged.bin.\nKeep powered on.",
                       "Install", "Cancel", ota_confirm_sd_install_cb, NULL);
}

static void ota_format_device_install_body(char *body, size_t body_len) {
    if (!body || body_len == 0) return;

    body[0] = '\0';
    if (ota_manager_is_supported()) {
        OtaStatus ota = ota_manager_get_status();
        if (ota.state == OTA_STATE_UPDATE_AVAILABLE) {
            ota_append_available_line(body, body_len, "Device firmware", ota.latest_version,
                                      ota.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
        }
    } else if (self_ota_manager_is_supported()) {
        SelfOtaStatus self = self_ota_manager_get_status();
        if (self.state == SELF_OTA_STATE_UPDATE_AVAILABLE) {
            ota_append_available_line(body, body_len, "Device firmware", self.latest_version,
                                      self.latest_build_number, (long)GHOSTESP_BUILD_NUMBER);
        }
    }

    if (body[0] == '\0') {
        snprintf(body, body_len, "Install firmware update?");
    }

    size_t used = strlen(body);
    if (used < body_len - 1) {
        snprintf(body + used, body_len - used, "\nKeep powered on.");
    }
}

static void ota_show_device_install_confirm(void) {
    if (ota_manager_is_supported()) {
        // Gate on whether a download target is actually staged, not on the
        // volatile status enum: a failed download or a background re-check can
        // move the state out of OTA_STATE_UPDATE_AVAILABLE while the URL from a
        // successful check is still valid, which otherwise produced the
        // "checked, saw an update, install says none" mismatch.
        if (!ota_manager_has_update_ready()) {
            ota_status_show_result("No Update Ready", "Check for updates first.");
            return;
        }
    }

    char body[256];
    ota_format_device_install_body(body, sizeof(body));
    popup_confirm_show(&settings_confirm_popup, lv_layer_top(), "Install Device Update?",
                       body, "Install", "Cancel", ota_confirm_device_install_cb, NULL);
}

static void ota_status_show_pending_self_failure(void) {
    if (!self_ota_manager_is_supported()) return;

    SelfOtaStatus self = self_ota_manager_get_status();
    if (self.state != SELF_OTA_STATE_FAILED || self.error_msg[0] == '\0') return;
    if (strncmp(ota_last_self_failure_notice, self.error_msg,
                sizeof(ota_last_self_failure_notice)) == 0) {
        return;
    }

    strncpy(ota_last_self_failure_notice, self.error_msg,
            sizeof(ota_last_self_failure_notice) - 1);
    ota_last_self_failure_notice[sizeof(ota_last_self_failure_notice) - 1] = '\0';
    ota_status_show_result("Update Failed", self.error_msg);
}

static void ota_status_format_progress(char *out, size_t out_len,
                                       OtaStatus ota, PeerOtaStatus peer, SelfOtaStatus self) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    if (ota_status_watch_device && ota_state_busy(ota.state)) {
        if (ota.image_size > 0 && ota.bytes_downloaded > 0) {
            snprintf(out, out_len, "Device firmware\n%u / %u KB",
                     (unsigned)(ota.bytes_downloaded / 1024), (unsigned)(ota.image_size / 1024));
        } else {
            snprintf(out, out_len, "Device firmware");
        }
        return;
    }

    if (ota_status_watch_peer && peer_ota_state_busy(peer.state)) {
        if (peer.total_bytes > 0 && peer.bytes_sent > 0) {
            snprintf(out, out_len, "Peer firmware\n%u / %u KB",
                     (unsigned)(peer.bytes_sent / 1024), (unsigned)(peer.total_bytes / 1024));
        } else {
            snprintf(out, out_len, "Peer firmware");
        }
        return;
    }

    if (ota_status_watch_self && self_ota_state_busy(self.state)) {
        if (self.image_size > 0 && self.bytes_written > 0) {
            snprintf(out, out_len, "Device firmware\n%u / %u KB",
                     (unsigned)(self.bytes_written / 1024), (unsigned)(self.image_size / 1024));
        } else if (self.state == SELF_OTA_STATE_FLASHING) {
            snprintf(out, out_len, "Starting updater\nScreen will pause during update");
        } else if (self.state == SELF_OTA_STATE_CHECKING) {
            snprintf(out, out_len, "Checking device firmware");
        } else {
            snprintf(out, out_len, "Device firmware");
        }
    }
}

static void ota_status_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!ota_status_overlay) return;

    OtaStatus ota = ota_manager_get_status();
    PeerOtaStatus peer = peer_ota_manager_get_status();
    SelfOtaStatus self = self_ota_manager_get_status();

    char progress[96];
    ota_status_format_progress(progress, sizeof(progress), ota, peer, self);
    if (progress[0]) scan_status_set_subtext(ota_status_overlay, progress);

    bool busy = (ota_status_watch_device && ota_state_busy(ota.state)) ||
                (ota_status_watch_peer && peer_ota_state_busy(peer.state)) ||
                (ota_status_watch_self && self_ota_state_busy(self.state));
    if (busy) return;

    int64_t elapsed_us = esp_timer_get_time() - ota_status_started_us;
    if (elapsed_us < 1000000) return;

    char body[256];
    const char *title = "Update Status";
    body[0] = '\0';

    if (ota_status_watch_device && ota.state == OTA_STATE_FAILED) {
        title = "Update Failed";
        snprintf(body, sizeof(body), "%s", ota.error_msg[0] ? ota.error_msg : "Device update failed");
    } else if (ota_status_watch_peer && peer.state == PEER_OTA_STATE_FAILED) {
        title = "Peer Update Failed";
        snprintf(body, sizeof(body), "%s", peer.error_msg[0] ? peer.error_msg : "Peer update failed");
    } else if (ota_status_watch_self && self.state == SELF_OTA_STATE_FAILED) {
        title = "Update Failed";
        snprintf(body, sizeof(body), "%s", self.error_msg[0] ? self.error_msg : "Device update failed");
    } else if (ota_ui_mode == OTA_UI_MODE_CHECK || ota_ui_mode == OTA_UI_MODE_PEER_CHECK) {
        bool have_local = (ota_status_watch_device && (ota.state == OTA_STATE_UPDATE_AVAILABLE)) ||
                          (ota_status_watch_self && (self.state == SELF_OTA_STATE_UPDATE_AVAILABLE));
        bool have_peer = ota_status_watch_peer && (peer.state == PEER_OTA_STATE_UPDATE_AVAILABLE);
        if (have_local || have_peer) {
            const char *device_version = ota_status_watch_device ? ota.latest_version : self.latest_version;
            long device_build = ota_status_watch_device ? ota.latest_build_number : self.latest_build_number;
            title = "Firmware Available";
            if (have_local) {
                ota_append_available_line(body, sizeof(body), "Device firmware", device_version,
                                          device_build, (long)GHOSTESP_BUILD_NUMBER);
            }
            if (have_peer) {
                ota_append_available_line(body, sizeof(body), "Peer firmware", peer.peer_version,
                                          peer.peer_build_number, peer.peer_current_build_number);
            }
        } else {
            title = "No Firmware Found";
            snprintf(body, sizeof(body), ota_ui_mode == OTA_UI_MODE_PEER_CHECK ?
                     "No update for peer." :
                     "No update for this device.");
        }
    } else if (ota_status_watch_peer && peer.state == PEER_OTA_STATE_DONE) {
        title = "Peer Updated";
        snprintf(body, sizeof(body), "Peer updated. Rebooting.");
    } else if (ota_status_watch_device && ota.state == OTA_STATE_READY_TO_REBOOT) {
        title = "Update Installed";
        snprintf(body, sizeof(body), "Firmware installed. Rebooting into the new image.");
    } else if (ota_ui_mode == OTA_UI_MODE_INSTALL || ota_ui_mode == OTA_UI_MODE_PEER_INSTALL ||
               ota_ui_mode == OTA_UI_MODE_SD_INSTALL) {
        title = "No Update Started";
        snprintf(body, sizeof(body), "No update running. Check first.");
    } else {
        title = "Update Started";
        snprintf(body, sizeof(body), "Update started. Watch status.");
    }

    ota_status_close_overlay();
    ota_status_show_result(title, body);
}

static void ota_status_start_overlay(ota_ui_mode_t mode, const char *title, const char *subtext) {
    ota_status_close_overlay();
    popup_confirm_close(&ota_result_popup);
    ota_last_self_failure_notice[0] = '\0';
    ota_ui_mode = mode;
    ota_status_started_us = esp_timer_get_time();
    ota_status_watch_device = (mode == OTA_UI_MODE_CHECK || mode == OTA_UI_MODE_INSTALL ||
                               mode == OTA_UI_MODE_SD_INSTALL) && ota_manager_is_supported();
    // OTA_UI_MODE_INSTALL is this board's own firmware only -- it never
    // touches the peer (see SETTING_OTA_INSTALL_UPDATE), so it must not watch
    // peer state here either. Otherwise a stale peer.state left over from an
    // unrelated earlier peer check/update (or the automatic background
    // check at boot) could surface as "Peer Update Failed" on a self-only
    // install that never went near the peer.
    ota_status_watch_peer = (mode == OTA_UI_MODE_PEER_CHECK || mode == OTA_UI_MODE_PEER_INSTALL) &&
                            peer_ota_manager_is_supported();
    ota_status_watch_self = (mode == OTA_UI_MODE_CHECK || mode == OTA_UI_MODE_INSTALL) &&
                            self_ota_manager_is_supported();
    ota_status_overlay = scan_status_create(title ? title : "Firmware Update");
    if (!ota_status_overlay) {
        ota_status_close_overlay();
        ota_status_show_result("Firmware Update", "Update started. Watch status.");
        return;
    }
    if (subtext) scan_status_set_subtext(ota_status_overlay, subtext);
    ota_status_poll_timer = lv_timer_create(ota_status_poll_timer_cb, 350, NULL);
}
#endif

typedef enum {
    SETTINGS_CAT_DISPLAY = 0,
    SETTINGS_CAT_THEME_ASSETS,
    SETTINGS_CAT_MENU_STYLE,
    SETTINGS_CAT_LED_RGB,
    SETTINGS_CAT_NAVIGATION,
    SETTINGS_CAT_STATUS_DISPLAY,
    SETTINGS_CAT_NETWORK,
    SETTINGS_CAT_POWER,
    SETTINGS_CAT_DATE_TIME,
    SETTINGS_CAT_SYSTEM_TOOLS,
    SETTINGS_CAT_LOGGING,
    SETTINGS_CAT_FAVORITES,
    SETTINGS_CAT_BACKUP_RESET,
    SETTINGS_CAT_SCAN_SAVING,
    SETTINGS_CAT_WIGLE,
#ifdef CONFIG_USE_IO_EXPANDER
    SETTINGS_CAT_IO_BUTTONS,
#endif
#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
    SETTINGS_CAT_MIC_RGB,
#endif
    SETTINGS_CAT_GHOSTLINK,
    SETTINGS_CAT_ACCESSIBILITY,
    SETTINGS_CAT_TERMINAL,
    SETTINGS_CAT_LOCKSCREEN,
    SETTINGS_CAT_WARDRIVING,
    SETTINGS_CAT_GPS,
#if GHOSTESP_OTA_SUPPORTED
    SETTINGS_CAT_FIRMWARE_UPDATE,
#endif
    SETTINGS_CAT_DEVICES,
    SETTINGS_CAT_COUNT
} SettingsCategoryId;

typedef enum {
    SETTINGS_ROOT_INFO = 0,
    SETTINGS_ROOT_DISPLAY,
    SETTINGS_ROOT_NETWORK,
    SETTINGS_ROOT_CAPTURE,
    SETTINGS_ROOT_LIGHTS,
    SETTINGS_ROOT_DEVICE,
    SETTINGS_ROOT_ACCESS,
    SETTINGS_ROOT_SYSTEM,
    SETTINGS_ROOT_COUNT
} SettingsRootId;

typedef struct {
    const char *name;
    uint8_t id;
} SettingsRootCategory;

typedef struct {
    const char *name;
    uint8_t id;
    uint8_t root_id;
    bool conditional;
    const char *condition_config;
} SettingsCategory;

static SettingsRootCategory settings_root_categories[] = {
    {"About", SETTINGS_ROOT_INFO},
    {"Display & Text", SETTINGS_ROOT_DISPLAY},
    {"Network", SETTINGS_ROOT_NETWORK},
    {"Capture & Location", SETTINGS_ROOT_CAPTURE},
    {"Lights & Sound", SETTINGS_ROOT_LIGHTS},
    {"Hardware", SETTINGS_ROOT_DEVICE},
    {"Lock & Favorites", SETTINGS_ROOT_ACCESS},
    {"System", SETTINGS_ROOT_SYSTEM},
};

static SettingsCategory settings_categories[] = {
    {"Screen", SETTINGS_CAT_DISPLAY, SETTINGS_ROOT_DISPLAY, false, NULL},
    {"Theme", SETTINGS_CAT_THEME_ASSETS, SETTINGS_ROOT_DISPLAY, false, NULL},
    {"Menus", SETTINGS_CAT_MENU_STYLE, SETTINGS_ROOT_DISPLAY, false, NULL},
    {"Navigation", SETTINGS_CAT_NAVIGATION, SETTINGS_ROOT_DISPLAY, false, NULL},
    {"Text & Reading", SETTINGS_CAT_ACCESSIBILITY, SETTINGS_ROOT_DISPLAY, false, NULL},
    {"Terminal", SETTINGS_CAT_TERMINAL, SETTINGS_ROOT_DISPLAY, false, NULL},
#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Status Display", SETTINGS_CAT_STATUS_DISPLAY, SETTINGS_ROOT_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY"},
#endif
    {"Wi-Fi", SETTINGS_CAT_NETWORK, SETTINGS_ROOT_NETWORK, false, NULL},
    {"Radio", SETTINGS_CAT_WARDRIVING, SETTINGS_ROOT_NETWORK, false, NULL},
    {"GhostLink", SETTINGS_CAT_GHOSTLINK, SETTINGS_ROOT_NETWORK, false, NULL},
    {"WiGLE", SETTINGS_CAT_WIGLE, SETTINGS_ROOT_CAPTURE, false, NULL},
    {"Location", SETTINGS_CAT_GPS, SETTINGS_ROOT_CAPTURE, false, NULL},
    {"Saving", SETTINGS_CAT_SCAN_SAVING, SETTINGS_ROOT_CAPTURE, false, NULL},
    {"LEDs", SETTINGS_CAT_LED_RGB, SETTINGS_ROOT_LIGHTS, false, NULL},
#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
    {"Microphone", SETTINGS_CAT_MIC_RGB, SETTINGS_ROOT_LIGHTS, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER"},
#endif
    {"Power & USB", SETTINGS_CAT_POWER, SETTINGS_ROOT_DEVICE, false, NULL},
    {"Hardware", SETTINGS_CAT_DEVICES, SETTINGS_ROOT_DEVICE, false, NULL},
#ifdef CONFIG_USE_IO_EXPANDER
    {"Buttons", SETTINGS_CAT_IO_BUTTONS, SETTINGS_ROOT_DEVICE, true, "CONFIG_USE_IO_EXPANDER"},
#endif
    {"Lock Screen", SETTINGS_CAT_LOCKSCREEN, SETTINGS_ROOT_ACCESS, false, NULL},
    {"Favorites", SETTINGS_CAT_FAVORITES, SETTINGS_ROOT_ACCESS, false, NULL},
    {"Date & Time", SETTINGS_CAT_DATE_TIME, SETTINGS_ROOT_SYSTEM, false, NULL},
    {"Tools", SETTINGS_CAT_SYSTEM_TOOLS, SETTINGS_ROOT_SYSTEM, false, NULL},
    {"Logging", SETTINGS_CAT_LOGGING, SETTINGS_ROOT_SYSTEM, false, NULL},
    {"Backup & Reset", SETTINGS_CAT_BACKUP_RESET, SETTINGS_ROOT_SYSTEM, false, NULL},
#if GHOSTESP_OTA_SUPPORTED
    {"Firmware Update", SETTINGS_CAT_FIRMWARE_UPDATE, SETTINGS_ROOT_SYSTEM, true, "CONFIG_ESPTOOLPY_FLASHSIZE_8MB or CONFIG_ESPTOOLPY_FLASHSIZE_16MB"},
#endif
};

static int current_settings_root = -1;
static int current_settings_category = -1;
static int settings_submenu_depth = 0;

// Cached chip-info cards for the read-only custom Info page.
#define OPTIONS_INFO_CARDS_MAX 3
static chip_info_card_t *s_info_cards;
static bool             s_info_detail_active = false;
static lv_obj_t        *s_info_scroll = NULL;
static lv_obj_t        *s_info_saved_menu_container = NULL;
extern int selected_item_index;
extern int num_items;
extern lv_obj_t *menu_container;
static lv_timer_t *menu_build_timer;
static void update_scroll_buttons_visibility(void);

static int settings_category_index_for_id(SettingsCategoryId cat_id) {
    int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
    for (int i = 0; i < category_count; i++) {
        if (settings_categories[i].id == cat_id) {
            return i;
        }
    }
    return -1;
}

static SettingsRootId current_settings_root_id(void) {
    int root_count = sizeof(settings_root_categories) / sizeof(settings_root_categories[0]);
    if (current_settings_root < 0 || current_settings_root >= root_count) {
        return SETTINGS_ROOT_COUNT;
    }
    return settings_root_categories[current_settings_root].id;
}

static int settings_category_count_for_root(SettingsRootId root_id) {
    int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
    int visible_count = 0;
    for (int i = 0; i < category_count; i++) {
        if (settings_categories[i].root_id == root_id) {
            visible_count++;
        }
    }
    return visible_count;
}

static int settings_category_index_for_root_position(SettingsRootId root_id, int position) {
    int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
    int visible_index = 0;
    for (int i = 0; i < category_count; i++) {
        if (settings_categories[i].root_id != root_id) {
            continue;
        }
        if (visible_index == position) {
            return i;
        }
        visible_index++;
    }
    return -1;
}

static SettingsCategoryId current_settings_category_id(void) {
    int category_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
    if (current_settings_category < 0 || current_settings_category >= category_count) {
        return SETTINGS_CAT_COUNT;
    }
    return settings_categories[current_settings_category].id;
}

static lv_obj_t *options_info_add_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                                        lv_text_align_t align, lv_coord_t pad_top) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_line_space(label, 5, 0);
    lv_obj_set_style_pad_top(label, pad_top, 0);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static void options_info_add_section(lv_obj_t *parent, const char *title, const char *body,
                                     const lv_font_t *title_font, const lv_font_t *body_font,
                                     lv_coord_t top_pad) {
    lv_obj_t *heading = options_info_add_label(parent, title, title_font, LV_TEXT_ALIGN_CENTER, top_pad);
    lv_obj_set_style_text_color(heading, lv_color_hex(0xA7D8FF), 0);

    lv_obj_t *content = options_info_add_label(parent, body, body_font, LV_TEXT_ALIGN_LEFT, GUI_GRID);
    lv_obj_set_style_text_color(content, lv_color_hex(0xE6E6E6), 0);
}

static const char *OPTIONS_INFO_CONTRIBUTORS =
    "jaylikesbunda - project maintainer\n"
    "Spooks4576 - original GhostESP developer\n"
    "tototo31 - major project contributions\n"
    "Play2BReal - recent project contributions\n"
    "the1anonlypr3 - art and assets\n"
    "Billi-Green - audio and ENV-III support";

static const char *OPTIONS_INFO_UPSTREAM =
    "JustCallMeKoKo / ESP32Marauder - foundational development\n"
    "thibauts - CastV2 protocol insights\n"
    "MarcoLucidi01 - DIAL protocol integration\n"
    "SpacehuhnTech - reference deauthentication code\n"
    "WillyJL - Flipper functionality and BLE Spam code\n"
    "flipperdevices and contributors - core IR/NFC implementation\n"
    "Garag - core NFC library\n"
    "connornishijima / SensoryBridge - MIC RGB visualizer algorithms\n"
    "DarkFlippers - SubGHz protocol decoders\n"
    "xMasterX - SubGHz improvements";

static void options_show_info_detail(void) {
    if (!options_menu_view.root || !lv_obj_is_valid(options_menu_view.root)) return;

    lvgl_timer_del_safe(&menu_build_timer);
    if (s_info_scroll && lv_obj_is_valid(s_info_scroll)) {
        lv_obj_del(s_info_scroll);
        s_info_scroll = NULL;
    }

    s_info_saved_menu_container = menu_container;
    if (s_info_saved_menu_container && lv_obj_is_valid(s_info_saved_menu_container)) {
        lv_obj_add_flag(s_info_saved_menu_container, LV_OBJ_FLAG_HIDDEN);
    }

    s_info_detail_active = true;
    current_settings_root = -1;
    current_settings_category = -1;
    settings_submenu_depth = 0;
    selected_item_index = 0;
    num_items = 0;
    display_manager_add_status_bar("Info");

    lv_obj_set_style_bg_color(options_menu_view.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(options_menu_view.root, LV_OPA_COVER, 0);

    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
#ifdef CONFIG_USE_TOUCHSCREEN
    content_h -= 34;
#endif
    if (content_h < 80) content_h = LV_VER_RES - GUI_STATUS_BAR_H;

    s_info_scroll = lv_obj_create(options_menu_view.root);
    menu_container = s_info_scroll;
    lv_obj_remove_style_all(s_info_scroll);
    lv_obj_set_size(s_info_scroll, LV_HOR_RES, content_h);
    lv_obj_align(s_info_scroll, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_info_scroll, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_info_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(s_info_scroll, GUI_SAFEAREA_HOR * 2, 0);
    lv_obj_set_style_pad_right(s_info_scroll, GUI_SAFEAREA_HOR * 2, 0);
    lv_obj_set_style_pad_top(s_info_scroll, GUI_GRID * 3, 0);
    lv_obj_set_style_pad_bottom(s_info_scroll, GUI_GRID * 3, 0);
    lv_obj_set_scroll_dir(s_info_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_info_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_info_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_info_scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *logo = lv_img_create(s_info_scroll);
    lv_img_set_src(logo, &ghostesplogo);
    lv_obj_set_style_img_recolor(logo, lv_color_white(), 0);
    lv_obj_set_style_img_recolor_opa(logo, LV_OPA_0, 0);
    lv_obj_set_style_pad_bottom(logo, GUI_GRID * 2, 0);
    if (LV_HOR_RES <= 240 || LV_VER_RES <= 180) {
        lv_img_set_zoom(logo, 180);
    } else if (LV_HOR_RES <= 320) {
        lv_img_set_zoom(logo, 220);
    }

    bool small = (LV_VER_RES <= 180 || LV_HOR_RES <= 240);
    const lv_font_t *body_font = small ? &lv_font_montserrat_12 : &lv_font_montserrat_14;
    const lv_font_t *heading_font = small ? &lv_font_montserrat_14 : &lv_font_montserrat_18;
    const lv_font_t *credits_font = small ? &lv_font_montserrat_8 : &lv_font_montserrat_10;

    options_info_add_label(s_info_scroll, GHOSTESP_VERSION, heading_font, LV_TEXT_ALIGN_CENTER, 0);

    free(s_info_cards);
    s_info_cards = calloc(OPTIONS_INFO_CARDS_MAX, sizeof(*s_info_cards));
    int count = s_info_cards ? chip_info_collect_cards(s_info_cards, OPTIONS_INFO_CARDS_MAX) : 0;
    for (int i = 0; i < count; i++) {
        options_info_add_section(s_info_scroll, s_info_cards[i].title, s_info_cards[i].body,
                                 heading_font, body_font, GUI_GRID * 2);
    }

    options_info_add_section(s_info_scroll, "GhostESP Contributors", OPTIONS_INFO_CONTRIBUTORS,
                             body_font, credits_font, GUI_GRID * 2);
    options_info_add_section(s_info_scroll, "Upstream & References", OPTIONS_INFO_UPSTREAM,
                             body_font, credits_font, GUI_GRID);

    lv_obj_update_layout(s_info_scroll);
    update_scroll_buttons_visibility();
}

static void options_info_scroll_step(int direction) {
    if (!s_info_detail_active || !menu_container || !lv_obj_is_valid(menu_container)) return;
    lv_coord_t amount = lv_obj_get_height(menu_container) / 2;
    if (amount < 24) amount = 24;
    lv_obj_scroll_by_bounded(menu_container, 0, direction * amount, LV_ANIM_OFF);
    update_scroll_buttons_visibility();
}

typedef enum {
    WIFI_MENU_MAIN,
    WIFI_MENU_ATTACKS,
    WIFI_MENU_SCAN_SELECT,
    WIFI_MENU_ENVIRONMENT,
    WIFI_MENU_NETWORK,
    WIFI_MENU_CAPTURE,
    WIFI_MENU_EVIL_PORTAL,
    WIFI_MENU_CONNECTION,
    WIFI_MENU_MISC,
    WIFI_MENU_EVIL_PORTAL_SELECT,
    WIFI_MENU_KARMA_PORTAL_SELECT,
    WIFI_MENU_AP_LIST,
    WIFI_MENU_AP_DETAILS,
    WIFI_MENU_STA_LIST,
    WIFI_MENU_STA_DETAILS,
    WIFI_MENU_SCANALL_LIST,
    WIFI_MENU_AP_MULTI_SELECT,
    WIFI_MENU_STA_MULTI_SELECT,
    WIFI_MENU_DNS_SINKHOLE,
    WIFI_MENU_DNS_SINKHOLE_DOWNLOAD,
    WIFI_MENU_DNS_SINKHOLE_FILE_PICK,
    WIFI_MENU_DNS_SINKHOLE_DETAILS,
    WIFI_MENU_CAPTURE_BROWSER,
    WIFI_MENU_ARP_LIST,
    WIFI_MENU_ARP_DETAILS,
    WIFI_MENU_MDNS_LIST,
    WIFI_MENU_MDNS_DETAILS,
    WIFI_MENU_ENUM_LIST,
    WIFI_MENU_ENUM_DETAILS,
    WIFI_MENU_GOVEE,
    WIFI_MENU_GOVEE_LIST,
    WIFI_MENU_GOVEE_DETAILS
} WifiMenuState;

static WifiMenuState current_wifi_menu_state = WIFI_MENU_MAIN;
static WifiMenuState ap_detail_return_state = WIFI_MENU_AP_LIST;
static WifiMenuState sta_detail_return_state = WIFI_MENU_STA_LIST;
static bool suppress_wifi_state_reset_once = false;
static int io_btn_being_edited = 0;

static void nav_push_wifi_detail_return(WifiMenuState return_state) {
    gui_nav_state_t nav = {
        .scope = NAV_SCOPE_WIFI_DETAIL_RETURN,
        .value = (int32_t)return_state,
    };
    gui_nav_history_push(&nav);
}

static bool nav_pop_wifi_detail_return(WifiMenuState *return_state_out) {
    gui_nav_state_t nav;
    if (!gui_nav_history_peek(&nav) || nav.scope != NAV_SCOPE_WIFI_DETAIL_RETURN) {
        return false;
    }
    gui_nav_history_pop(&nav);
    if (return_state_out) {
        *return_state_out = (WifiMenuState)nav.value;
    }
    return true;
}

static const char * const wifi_attacks_options[] = {
    "Deauth Attack",
    "Handshake Capture + Deauth",
    "Channel Switch (CSA) Attack",
    "Beacon Spam - Random",
    "Beacon Spam - Rickroll",
    "Beacon Spam - List",
    "EAPOL Logoff",
    "Probe Request Flood",
    "Bad Msg Attack",
    "Auth Flood",
    "GTK Isolation Bypass Test",
    "DHCP Starvation",
    "Stop DHCP-Starve",
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    "SAE DoS Flood",
#endif
    "Karma Attack",
    "Karma Attack (Custom SSIDs)",
    "Karma Attack (Custom Portal)",
    "Stop Karma Attack",       
    NULL
};

static const char * const wifi_capture_options[] = {
    "Capture Probe", "Capture Deauth", "Capture Beacon", "Capture Raw (Monitor)", "Capture EAPOL",
    "Capture WPS", "Capture Pwnagotchi",
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    "Capture 802.15.4", "Capture 802.15.4 (Channel)",
#endif
    "Listen for Probes", "Export Handshakes (hc22000)", NULL
};

static const char * const wifi_scan_select_options[] = {
    "Scan APs", "Scan APs Live", "Scan Stations", "Scan APs + Clients",
    "List APs", "List Stations", "List APs + Clients",
    "Multi-Select APs", "Multi-Select Stations", "Wi-Fi Security Check", NULL
};

static const char * const wifi_environment_options[] = {
    "Environment Sweep", "Airspace Monitor", "PineAP Detection", "Flock Camera Detection", "Channel Congestion",
    "Packet Monitor", "Packet Visualizer", "Hop Channels", NULL
};

static const char * const wifi_network_options[] = {
    "mDNS Discovery", "ARP Sweep", "List Hosts (ARP)", "Scan Open Ports", "SSH Banner Scan",
    "NetBIOS Scan", "HTTP Banner Scan", "SNMP Probe",
    "SMB Enum (enum4linux)", "SNMP Walk",
    "NetBIOS Subnet...", "HTTP Banner Subnet...", "SNMP Probe Subnet...", "SNMP Walk Subnet...",
    NULL
};

static void switch_to_settings_root(int root_idx);
static void switch_to_settings_category(int cat_idx);
static void settings_activate_row(int row_index, bool increment);

static const char * const wifi_evil_portal_options[] = {
    "Evil Portal", "Custom Evil Portal", "Stop Evil Portal", NULL
};

static const char * const wifi_dns_sinkhole_options[] = {
    "Sinkhole", "Stop Sinkhole", "Sinkhole Status",
    "Download Blocklist", "Toggle Logging", NULL
};

static const char * const wifi_dns_sinkhole_download_options[] = {
    "Peter Lowe (3.5K ads)",
    "OISD Basic (40K mixed)",
    "StevenBlack (70K mixed)",
    NULL
};

static const char * const wifi_connection_options[] = {"Connect to WiFi", "Connect to saved WiFi", "Reset AP Credentials", NULL};

static const char * const wifi_misc_options[] = {
    "TV Cast (Dial Connect)", "Power Printer", "TP Link Test", "Wake on LAN", "Govee Lights", NULL
};

static const char * const wifi_govee_options[] = {"Scan Govee Devices", "List Govee Devices", NULL};

static const char * const wifi_main_options[] = {
    "Attacks", "Recon", "Monitor", "Network", "Capture", "Evil Portal", "DNS Sinkhole", "Connection", "Gadgets", NULL
};

static const char * const gps_options[] = {"Start Wardriving", "Stop Wardriving", "GPS Info",
                                    "BLE Wardriving", "BLE + WiFi Wardriving",   NULL};

#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
static const char *nrf24_options[] = {"Frequency Analyzer", NULL};
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
static const char * const subghz_options[] = {"SubGHz", NULL};
#endif

// Dual Comm is split into a small state machine with submenus to avoid
// one giant list that can starve LVGL.

typedef enum {
    DUALCOMM_MENU_MAIN = 0,
    DUALCOMM_MENU_SESSION,
    DUALCOMM_MENU_SCAN,
    DUALCOMM_MENU_WIFI,
    DUALCOMM_MENU_ATTACKS,
    DUALCOMM_MENU_CAPTURE,
    DUALCOMM_MENU_TOOLS,
    DUALCOMM_MENU_BLE,
    DUALCOMM_MENU_GPS,
    DUALCOMM_MENU_ETHERNET,
    DUALCOMM_MENU_KEYBOARD
} DualCommMenuState;

static DualCommMenuState current_dualcomm_menu_state = DUALCOMM_MENU_MAIN;

static const char * const dual_comm_main_options[] = {
    "Status",
    "Discovery / Session",
    "Scanning",
    "WiFi",
    "Attacks",
    "Capture",
    "Tools",
    "BLE",
    "GPS",
    "Ethernet",
    "Keyboard",
    NULL
};

static const char * const dual_comm_keyboard_options[] = {
    "USB Host On",
    "USB Host Off",
    "USB Host Status",
    NULL
};

static const char * const dual_comm_session_options[] = {
    "Status",
    "Start Discovery",
    "Connect to Peer",
    "Disconnect",
    "Send Remote Command",
    NULL
};

static const char * const dual_comm_scan_options[] = {
    "Scan Access Points",
    "Scan APs Live",
    "Scan Stations",
    "Scan AP + STA",
    "Sweep",
    "mDNS Discovery",
    "ARP Scan Network",
    "Scan Open Ports",
    "Scan SSH",
    "NetBIOS Scan",
    "HTTP Banner Scan",
    "SNMP Probe",
    "Scan SSH Host...",
    "NetBIOS Scan Host...",
    "HTTP Banner Host...",
    "SNMP Probe Host...",
    "NetBIOS Subnet...",
    "HTTP Banner Subnet...",
    "SNMP Probe Subnet...",
    "PineAP Detection",
    "Flock Detection",
    "Channel Congestion",
    "List Access Points",
    "List Stations",
    "Select AP",
    "Select Station",
    "Track AP",
    "Track Station",
    NULL
};

static const char * const dual_comm_wifi_options[] = {
    "Connect to WiFi",
    "Connect to saved WiFi",
    "Reset AP Credentials",
    "Set AP Credentials",
    "Enable AP",
    "Disable AP",
    NULL
};

static const char * const dual_comm_attacks_options[] = {
    "Start Deauth Attack",
    "Start Handshake+Deauth",
    "Start EAPOL Logoff",
    "Start DHCP-Starve",
    "Stop DHCP-Starve",
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    "Start SAE Flood",
#endif
    "Start Karma Attack",
    "Start Karma Attack (Custom SSIDs)",
    "Stop Karma Attack",
    NULL
};

static const char * const dual_comm_capture_options[] = {
    "Capture Deauth",
    "Capture Probe",
    "Capture Beacon",
    "Capture Raw",
    "Capture Eapol",
    "Capture WPS",
    "Capture PWN",
    "Listen for Probes",
    NULL
};

static const char * const dual_comm_tools_options[] = {
    "Start Evil Portal",
    "Stop Evil Portal",
    "Start Wardriving",
    "Stop Wardriving",
    "TV Cast (Dial Connect)",
    "Power Printer",
    "Scan SSH",
    "Toggle WebUI AP Only",
    NULL
};

static const char * const dual_comm_ble_options[] = {
    "BLE Bridge",
    "Start AirTag Scanner",
    "List AirTags",
    "Select AirTag",
    "Spoof Selected AirTag",
    "Stop Spoofing",
    "Find Flippers",
    "List Flippers",
    "Select Flipper",
    "Raw BLE Scanner",
    "BLE Spam - Apple",
    "BLE Spam - Microsoft",
    "BLE Spam - Samsung",
    "BLE Spam - Google",
    "BLE Spam - Random",
    "Stop BLE Spam",
    NULL
};

static const char * const dual_comm_gps_options[] = {
    "GPS Info",
    "BLE Wardriving",
    "BLE + WiFi Wardriving",
    NULL
};

static const char * const dual_comm_ethernet_options[] = {
    "Initialise",
    "Deinitialise",
    "Ethernet Info",
    "Fingerprint Scan",
    "ARP Scan",
    "Port Scan Local",
    "Port Scan All",
    "Ping Scan",
    "DNS Lookup",
    "Traceroute",
    "HTTP Request",
    "Sync NTP Time",
    "Network Stats",
    "Show Config",
    "ARP Poison",
    NULL
};

static void load_current_settings_values(void);

typedef enum {
    SETTING_WIDGET_VALUE_CYCLE = 0,   // text label + ◀ ▶ arrows (default)
    SETTING_WIDGET_TOGGLE,            // iOS-style on/off switch
} SettingWidgetType;

typedef struct {
    const char *label;
    int16_t setting_type;
    const char * const *value_options;
    uint8_t value_count;
    int16_t current_value;
    uint8_t category_id;
    bool conditional;
    const char *condition_config;
    SettingWidgetType widget;
} SettingsItem;

// RGB mode options - MIC Visualizer only available when enabled in config
#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
static const char * const rgb_mode_options[] = {"Normal", "Rainbow", "Stealth", "Knight Rider", "Red", "Green", "Blue", "Yellow", "TWH Purple", "Cyan", "Orange", "White", "Pink", "MIC Visualizer"};
#define RGB_MODE_COUNT 14
#else
static const char * const rgb_mode_options[] = {"Normal", "Rainbow", "Stealth", "Knight Rider", "Red", "Green", "Blue", "Yellow", "TWH Purple", "Cyan", "Orange", "White", "Pink"};
#define RGB_MODE_COUNT 13
#endif
static const char * const timeout_options[] = {"5s", "10s", "15s", "30s", "60s", "2m", "5m", "Never"};
static const char *theme_options[THEME_PALETTE_THEME_COUNT];
static const char * const bool_options[] = {"Off", "On"};
#ifdef CONFIG_USE_ENCODER
static const char * const encoder_latch_options[] = {"2 transitions", "4 transitions (legacy)"};
#endif
static const char * const log_level_options[] = {"None", "Error", "Warn", "Info", "Debug", "Verbose"};
static const char * const textcolor_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t textcolor_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
static const char * const menu_layout_options[] = {"Carousel", "Grid", "List", "Compact", "Hero"};
static const char * const bg_shade_options[] = {"Darker", "Palette", "Lighter", "Lightest"};
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char * const idle_animation_options[] = {"Game of Life", "Ghost", "Starfield", "HUD", "Matrix", "Flying Ghosts", "Spiral", "Falling Leaves", "Bouncing Text"};
static const char * const idle_delay_options[] = {"Never", "5s", "10s", "30s"};
#endif
static const char * const action_options[] = {"Press OK"};
#if GHOSTESP_OTA_SUPPORTED
static const char * const ota_channel_options[] = {"Stable", "Prerelease"};
#endif
static const char *asset_pack_options[ASSET_PACK_INSTALLED_MAX + 1];
static int asset_pack_option_count = 1;
static const char * const font_size_options[] = {"Small", "Normal", "Large"};
static const char * const row_height_options[] = {"Compact", "Normal", "Large", "Extra Large"};
static const char * const repeat_speed_options[] = {"Slow", "Normal", "Fast"};
static const char * const lockscreen_timeout_options[] = {"Off", "30s", "1m", "5m"};

static const char * const wd_hop_options[] = {
    "50ms", "75ms", "100ms", "125ms", "150ms", "175ms", "200ms", "250ms", "300ms", "400ms", "500ms"
};
static const uint16_t wd_hop_values[] = {50, 75, 100, 125, 150, 175, 200, 250, 300, 400, 500};
static const int wd_hop_count = sizeof(wd_hop_values) / sizeof(wd_hop_values[0]);

static const char * const gps_baud_options[] = {
    "Default", "Auto", "4800", "9600", "19200", "38400", "57600", "115200"
};
static const uint32_t gps_baud_values[] = {0, GPS_BAUD_AUTO, 4800, 9600, 19200, 38400, 57600, 115200};
static const int gps_baud_count = sizeof(gps_baud_values) / sizeof(gps_baud_values[0]);

// Timezone labels (display) and matching POSIX TZ strings (values).
// Kept in sync with setup_wizard_screen.c so users see the same list.
static const char * const timezone_options[] = {
    "UTC", "EST", "CST", "MST", "PST",
    "GMT", "CET", "EET", "IST (India)", "JST",
    "AEST", "AWST", "NZST"
};
static const char * const timezone_values[] = {
    "UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0",
    "GMT0", "CET-1CEST,M3.5.0,M10.5.0", "EET-2EEST,M3.5.0,M10.5.0", "IST-5:30", "JST-9",
    "AEST-10AEDT,M10.1.0,M4.1.0", "AWST-8", "NZST-12NZDT,M9.5.0,M4.1.0"
};
static const int timezone_count = sizeof(timezone_values) / sizeof(timezone_values[0]);

// Clock view face style; index matches G_Settings.clock_style (0 = Digital).
static const char * const clock_style_options[] = {"Digital", "Analog", "Segment"};

// WiFi country labels kept in sync with setup_wizard_screen.c; the index is
// the persisted G_Settings.wifi_country value.
static const char * const country_setting_options[] = {
    "US (Americas)", "GB (Europe)", "JP (Japan)", "AU (Australia)", "CN (Asia)", "01 (World Safe)"
};
static const char * const country_setting_codes[] = {"US", "GB", "JP", "AU", "CN", "01"};
static const int country_setting_count =
    sizeof(country_setting_options) / sizeof(country_setting_options[0]);

static const char * const brightness_options[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
static const char * const mic_visualizer_mode_options[] = {
    "4-Band Spectrum", 
    "VU Meter", 
    "Kaleidoscope", 
    "Waveform", 
    "Bloom",
    "Peak Meter"
};

static const char * const mic_color_mode_options[] = {
    "Rainbow",
    "Chromatic",
    "Single Hue",
    "Fire",
    "Ocean",
    "Forest",
    "Heat"
};

static const char * const mic_sensitivity_options[] = {
    "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static const char * const mic_smoothing_options[] = {
    "0%", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%"
};

static const char * const mic_contrast_options[] = {
    "1", "2", "3", "4", "5"
};
#endif

static SettingsItem settings_items[] = {
    {"Display Timeout", SETTING_DISPLAY_TIMEOUT, timeout_options, 8, 1, SETTINGS_CAT_DISPLAY, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
    {"Max Brightness", SETTING_MAX_BRIGHTNESS, brightness_options, 10, 9, SETTINGS_CAT_DISPLAY, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif
    {"Invert Colors", SETTING_INVERT_COLORS, bool_options, 2, 0, SETTINGS_CAT_DISPLAY, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Sun Mode", SETTING_SUN_MODE, bool_options, 2, 0, SETTINGS_CAT_DISPLAY, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Log Level", SETTING_LOG_LEVEL, log_level_options, 6, ESP_LOG_WARN, SETTINGS_CAT_LOGGING, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Open Without PIN", SETTING_FAVORITES_BYPASS, bool_options, 2, 0, SETTINGS_CAT_FAVORITES, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Manage Favorites", SETTING_MANAGE_FAVORITES, action_options, 1, 0, SETTINGS_CAT_FAVORITES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Terminal Font", SETTING_TERMINAL_FONT_SIZE, font_size_options, 3, 1, SETTINGS_CAT_TERMINAL, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

    {"Menu Theme", SETTING_MENU_THEME, theme_options, THEME_PALETTE_THEME_COUNT, 0, SETTINGS_CAT_THEME_ASSETS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Background Effects", SETTING_THEME_BACKGROUND_EFFECTS, bool_options, 2, 1, SETTINGS_CAT_THEME_ASSETS, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Asset Pack", SETTING_RELOAD_ASSET_PACK, (const char * const *)asset_pack_options, 1, 0, SETTINGS_CAT_THEME_ASSETS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Terminal Color", SETTING_TERMINAL_COLOR, textcolor_options, 8, 0, SETTINGS_CAT_TERMINAL, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Menu Layout", SETTING_MENU_LAYOUT, menu_layout_options, 5, 1, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Row Height", SETTING_ROW_HEIGHT, row_height_options, MENU_ROW_HEIGHT_OPTION_COUNT, 1, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Main Menu Items", SETTING_MAIN_MENU_ITEMS, action_options, 1, 0, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Apps Gallery Items", SETTING_APPS_MENU_ITEMS, action_options, 1, 0, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Zebra Menus", SETTING_ZEBRA_MENUS, bool_options, 2, 0, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Surface Tone", SETTING_MENU_BG_SHADE, bg_shade_options, 4, 1, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Rounded Menus", SETTING_MENU_ROUNDED, bool_options, 2, 0, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Item Borders", SETTING_MENU_ITEM_BORDERS, bool_options, 2, 0, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Card Background", SETTING_MENU_CARD_BG, bool_options, 2, 1, SETTINGS_CAT_MENU_STYLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Invert Carousel", SETTING_CAROUSEL_INVERT_DIRECTION, bool_options, 2, 0, SETTINGS_CAT_NAVIGATION, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Touch Drag Scroll", SETTING_TOUCH_DRAG_SCROLL, bool_options, 2, 1, SETTINGS_CAT_NAVIGATION, false, NULL, SETTING_WIDGET_TOGGLE},

    {"RGB Mode", SETTING_RGB_MODE, rgb_mode_options, RGB_MODE_COUNT, 0, SETTINGS_CAT_LED_RGB, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Neopixel Brightness", SETTING_NEOPIXEL_BRIGHTNESS, brightness_options, 10, 9, SETTINGS_CAT_LED_RGB, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

    {"Navigation Buttons", SETTING_NAV_BUTTONS, bool_options, 2, 1, SETTINGS_CAT_NAVIGATION, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Third Control", SETTING_THIRD_CONTROL, bool_options, 2, 0, SETTINGS_CAT_NAVIGATION, false, NULL, SETTING_WIDGET_TOGGLE},
#ifdef CONFIG_USE_ENCODER
    {"Invert Encoder", SETTING_ENCODER_INVERT, bool_options, 2, 0, SETTINGS_CAT_NAVIGATION, true, "CONFIG_USE_ENCODER", SETTING_WIDGET_TOGGLE},
    {"T-Embed Detent", SETTING_ENCODER_LATCH, encoder_latch_options, 2, 0, SETTINGS_CAT_NAVIGATION, true, "CONFIG_USE_ENCODER", SETTING_WIDGET_VALUE_CYCLE},
#endif

#ifdef CONFIG_WITH_STATUS_DISPLAY
    {"Idle Animation", SETTING_IDLE_ANIMATION, idle_animation_options, 9, 0, SETTINGS_CAT_STATUS_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY", SETTING_WIDGET_VALUE_CYCLE},
    {"Idle Anim Delay", SETTING_IDLE_ANIM_DELAY, idle_delay_options, 4, 0, SETTINGS_CAT_STATUS_DISPLAY, true, "CONFIG_WITH_STATUS_DISPLAY", SETTING_WIDGET_VALUE_CYCLE},
#endif

    {"Web Auth", SETTING_WEB_AUTH, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_TOGGLE},
    {"AP Enabled", SETTING_AP_ENABLED, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_TOGGLE},
    {"WebUI AP Only", SETTING_WEBUI_AP_ONLY, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_TOGGLE},
    {"AP SSID", SETTING_AP_SSID, action_options, 1, 0, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"AP Password", SETTING_AP_PASSWORD, action_options, 1, 0, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"STA SSID", SETTING_STA_SSID, action_options, 1, 0, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"STA Password", SETTING_STA_PASSWORD, action_options, 1, 0, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"WiFi Auto-Reconnect", SETTING_WIFI_AUTO_RECONNECT, bool_options, 2, 1, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Country", SETTING_COUNTRY, country_setting_options, country_setting_count, 0, SETTINGS_CAT_NETWORK, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"WiFi Hop Channels", SETTING_HOP_CHANNELS, action_options, 1, 0, SETTINGS_CAT_WARDRIVING, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

    {"Timezone", SETTING_TIMEZONE, timezone_options, 13, 0, SETTINGS_CAT_DATE_TIME, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Clock Style", SETTING_CLOCK_STYLE, clock_style_options, 3, 0, SETTINGS_CAT_DATE_TIME, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Status Bar Clock", SETTING_STATUS_BAR_CLOCK, bool_options, 2, 1, SETTINGS_CAT_DATE_TIME, false, NULL, SETTING_WIDGET_TOGGLE},

    {"Power Saving Mode", SETTING_POWER_SAVE, bool_options, 2, 0, SETTINGS_CAT_POWER, false, NULL, SETTING_WIDGET_TOGGLE},
#if CONFIG_IDF_TARGET_ESP32S3
    {"USB Host Mode", SETTING_USB_HOST_MODE, bool_options, 2, 0, SETTINGS_CAT_POWER, true, "CONFIG_IDF_TARGET_ESP32S3", SETTING_WIDGET_TOGGLE},
#ifdef CONFIG_HAS_USB_MSC_SD
    {"USB SD Passthrough", SETTING_USB_MSC, bool_options, 2, 0, SETTINGS_CAT_POWER, true, "CONFIG_HAS_USB_MSC_SD", SETTING_WIDGET_TOGGLE},
#endif
#endif
    {"Auto Save Scans", SETTING_AUTO_SAVE_SCANS, bool_options, 2, 1, SETTINGS_CAT_SCAN_SAVING, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Run Setup Wizard", SETTING_RUN_SETUP_WIZARD, action_options, 1, 0, SETTINGS_CAT_SYSTEM_TOOLS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"I2C Bus Scan", SETTING_I2C_SCAN, action_options, 1, 0, SETTINGS_CAT_SYSTEM_TOOLS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"GhostLink Benchmark", SETTING_GL_BENCH, action_options, 1, 0, SETTINGS_CAT_SYSTEM_TOOLS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Export Settings SD", SETTING_EXPORT_SETTINGS_SD, action_options, 1, 0, SETTINGS_CAT_BACKUP_RESET, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Import Settings SD", SETTING_IMPORT_SETTINGS_SD, action_options, 1, 0, SETTINGS_CAT_BACKUP_RESET, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Factory Reset", SETTING_FACTORY_RESET, action_options, 1, 0, SETTINGS_CAT_BACKUP_RESET, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#if GHOSTESP_OTA_SUPPORTED
    {"Update Channel", SETTING_OTA_CHANNEL, ota_channel_options, 2, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Check Device Update", SETTING_OTA_CHECK_NOW, action_options, 1, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Install Update", SETTING_OTA_INSTALL_UPDATE, action_options, 1, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Check Peer Update", SETTING_OTA_CHECK_PEER, action_options, 1, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Update Peer", SETTING_OTA_UPDATE_PEER, action_options, 1, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Install from SD Card", SETTING_OTA_INSTALL_FROM_SD, action_options, 1, 0, SETTINGS_CAT_FIRMWARE_UPDATE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif

    {"Auto Upload", SETTING_WIGLE_AUTO_UPLOAD, bool_options, 2, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Donate Data", SETTING_WIGLE_DONATE, bool_options, 2, 1, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Load Config from SD", SETTING_LOAD_CONFIG, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Test API Key", SETTING_WIGLE_TEST_API, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Help", SETTING_WIGLE_HELP, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Manual Upload", SETTING_WIGLE_MANUAL_UPLOAD, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"View WiGLE Stats", SETTING_WIGLE_STATS, action_options, 1, 0, SETTINGS_CAT_WIGLE, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
    {"Visualizer Mode", SETTING_MIC_VISUALIZER_MODE, mic_visualizer_mode_options, 6, 0, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
    {"Color Mode", SETTING_MIC_COLOR_MODE, mic_color_mode_options, 7, 0, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
    {"Sensitivity", SETTING_MIC_SENSITIVITY, mic_sensitivity_options, 10, 4, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
    {"Smoothing", SETTING_MIC_SMOOTHING, mic_smoothing_options, 11, 3, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
    {"Contrast", SETTING_MIC_CONTRAST, mic_contrast_options, 5, 1, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
    {"Mirror Mode", SETTING_MIC_MIRROR_MODE, bool_options, 2, 0, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_TOGGLE},
    {"Calibrate", SETTING_MIC_CALIBRATE, action_options, 1, 0, SETTINGS_CAT_MIC_RGB, true, "CONFIG_HAS_MIC or CONFIG_ENABLE_MIC_RGB_VISUALIZER", SETTING_WIDGET_VALUE_CYCLE},
#endif
    {"Split Terminal", SETTING_GHOSTLINK_SPLIT_VIEW, bool_options, 2, 1, SETTINGS_CAT_TERMINAL, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Font Size", SETTING_FONT_SIZE, font_size_options, 3, 1, SETTINGS_CAT_ACCESSIBILITY, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"High Contrast", SETTING_HIGH_CONTRAST, bool_options, 2, 0, SETTINGS_CAT_ACCESSIBILITY, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Reduced Motion", SETTING_REDUCED_MOTION, bool_options, 2, 0, SETTINGS_CAT_ACCESSIBILITY, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Epilepsy Warning", SETTING_EPILEPSY_WARNING, bool_options, 2, 1, SETTINGS_CAT_ACCESSIBILITY, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Input Repeat Speed", SETTING_INPUT_REPEAT_SPEED, repeat_speed_options, 3, 1, SETTINGS_CAT_ACCESSIBILITY, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

    {"Lockscreen", SETTING_LOCKSCREEN_ENABLED, bool_options, 2, 0, SETTINGS_CAT_LOCKSCREEN, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Lock on Wake", SETTING_LOCKSCREEN_WAKE, bool_options, 2, 1, SETTINGS_CAT_LOCKSCREEN, false, NULL, SETTING_WIDGET_TOGGLE},
    {"Auto-Lock", SETTING_LOCKSCREEN_TIMEOUT, lockscreen_timeout_options, 4, 0, SETTINGS_CAT_LOCKSCREEN, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Set PIN", SETTING_LOCKSCREEN_CHANGE_PIN, action_options, 1, 0, SETTINGS_CAT_LOCKSCREEN, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

    {"Primary Hop", SETTING_WD_HOP_PRIMARY, wd_hop_options, wd_hop_count, 2, SETTINGS_CAT_WARDRIVING, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Helper Hop", SETTING_WD_HOP_HELPER, wd_hop_options, wd_hop_count, 2, SETTINGS_CAT_WARDRIVING, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"Weighted 5GHz", SETTING_WD_WEIGHTED_5G, bool_options, 2, 1, SETTINGS_CAT_WARDRIVING, false, NULL, SETTING_WIDGET_TOGGLE},
    {"GPS Baud Rate", SETTING_GPS_BAUD_RATE, gps_baud_options, gps_baud_count, 0, SETTINGS_CAT_GPS, false, NULL, SETTING_WIDGET_VALUE_CYCLE},

#ifdef CONFIG_HAS_INFRARED
    {"IR TX Pin", SETTING_IR_TX_PIN, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif
#ifdef CONFIG_HAS_INFRARED_RX
    {"IR RX Pin", SETTING_IR_RX_PIN, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif
    {"Infrared", SETTING_DEVICE_IR, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"GPS", SETTING_DEVICE_GPS, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#ifdef CONFIG_HAS_SUBGHZ
    {"SubGHz", SETTING_DEVICE_SUBGHZ, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif
#ifdef CONFIG_HAS_NRF24
    {"NRF24", SETTING_DEVICE_NRF24, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
#endif
    {"SD Card", SETTING_DEVICE_SD, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
    {"RGB LED", SETTING_DEVICE_RGB, action_options, 1, 0, SETTINGS_CAT_DEVICES, false, NULL, SETTING_WIDGET_VALUE_CYCLE},
};

static const int settings_items_count = sizeof(settings_items) / sizeof(settings_items[0]);

static int settings_item_clamped_value(SettingsItem *item) {
    if (!item || item->value_count <= 0) return 0;
    if (item->current_value < 0 || item->current_value >= item->value_count) {
        item->current_value = 0;
    }
    return item->current_value;
}

static const char *settings_item_value_text(SettingsItem *item) {
    int value = settings_item_clamped_value(item);
    if (!item) return "";
    // For text-input settings, show the current value (masked for passwords)
    switch ((SettingsType)item->setting_type) {
        case SETTING_IR_TX_PIN: {
            static char ir_tx_buf[24];
            int32_t pin = settings_get_ir_tx_pin(&G_Settings);
            if (pin >= 0) {
                snprintf(ir_tx_buf, sizeof(ir_tx_buf), "GPIO%d", (int)pin);
            } else {
                snprintf(ir_tx_buf, sizeof(ir_tx_buf), "Auto (GPIO%d)", (int)infrared_get_tx_pin());
            }
            return ir_tx_buf;
        }
        case SETTING_IR_RX_PIN: {
            static char ir_rx_buf[24];
            int32_t pin = settings_get_ir_rx_pin(&G_Settings);
            if (pin >= 0) {
                snprintf(ir_rx_buf, sizeof(ir_rx_buf), "GPIO%d", (int)pin);
            } else {
                snprintf(ir_rx_buf, sizeof(ir_rx_buf), "Auto (GPIO%d)", (int)infrared_get_rx_pin());
            }
            return ir_rx_buf;
        }
        case SETTING_DEVICE_IR: {
#if defined(CONFIG_HAS_INFRARED) || defined(CONFIG_HAS_INFRARED_RX)
            bool ir_active = infrared_manager_rx_is_initialized() || infrared_manager_dazzler_is_active();
            return ir_active ? "Active" : "Idle";
#else
            return "Not built";
#endif
        }
        case SETTING_DEVICE_GPS:
            return g_gpsManager.isinitilized ? "Active" : "Idle";
#ifdef CONFIG_HAS_SUBGHZ
        case SETTING_DEVICE_SUBGHZ:
            return subghz_remote_manager_is_ready() ? "Active" : "Idle";
#endif
#ifdef CONFIG_HAS_NRF24
        case SETTING_DEVICE_NRF24:
            return nrf24_remote_manager_is_running() ? "Active" : "Idle";
#endif
        case SETTING_DEVICE_SD:
            return sd_card_manager.is_initialized ? "Mounted" : "Idle";
        case SETTING_DEVICE_RGB:
            return (rgb_manager.strip != NULL || rgb_manager.is_separate_pins) ? "Active" : "Idle";
        case SETTING_AP_SSID: {
            const char *cur = settings_get_ap_ssid(&G_Settings);
            return (cur && cur[0]) ? cur : "<set>";
        }
        case SETTING_AP_PASSWORD: {
            const char *cur = settings_get_ap_password(&G_Settings);
            return (cur && cur[0]) ? "********" : "<open>";
        }
        case SETTING_STA_SSID: {
            const char *cur = settings_get_sta_ssid(&G_Settings);
            return (cur && cur[0]) ? cur : "<not set>";
        }
        case SETTING_STA_PASSWORD: {
            const char *cur = settings_get_sta_password(&G_Settings);
            return (cur && cur[0]) ? "********" : "<empty>";
        }
#if GHOSTESP_OTA_SUPPORTED
        case SETTING_OTA_CHECK_NOW:
        case SETTING_OTA_INSTALL_UPDATE:
        case SETTING_OTA_CHECK_PEER:
        case SETTING_OTA_UPDATE_PEER:
        case SETTING_OTA_INSTALL_FROM_SD:
            return "";
#endif
        default:
            break;
    }
    if (!item->value_options || !item->value_options[value]) return "";
    return item->value_options[value];
}

static bool settings_item_is_visible(const SettingsItem *item) {
    if (!item) return false;
#ifdef CONFIG_USE_ENCODER
    if (item->setting_type == SETTING_ENCODER_LATCH) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") != 0) return false;
#else
        return false;
#endif
    }
#endif
#if GHOSTESP_OTA_SUPPORTED
    if (item->setting_type == SETTING_OTA_INSTALL_FROM_SD && !ota_sd_install_available()) {
        return false;
    }
    // Peer (GhostLink relay) update rows only make sense on boards that
    // actually relay to a peer -- currently only the somethingsomething C5.
    // Hide them everywhere else instead of showing rows that just error out.
    if ((item->setting_type == SETTING_OTA_CHECK_PEER ||
         item->setting_type == SETTING_OTA_UPDATE_PEER) &&
        !peer_ota_manager_is_supported()) {
        return false;
    }
#endif
    return true;
}

#define IO_BTN_EDIT_P10 0x1000
#define IO_BTN_EDIT_P11 0x1001
#define IO_BTN_EDIT_P12 0x1002

typedef struct {
    const char* name;
    const char* cmd_prefix;
    View* view;
} io_btn_preset_t;

static const io_btn_preset_t io_btn_presets[] = {
    {"WiFi", "view:wifi", &options_menu_view},
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    {"BLE", "view:ble", &options_menu_view},
#endif
#ifdef CONFIG_HAS_NFC
    {"NFC", "view:nfc", &nfc_view},
#endif
#if CONFIG_HAS_INFRARED
    {"Infrared", "view:ir", &infrared_view},
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    {"BadUSB", "view:badusb", &badusb_view},
#endif
#ifdef CONFIG_HAS_BADBLE
    {"BadBLE", "view:badble", &badble_view},
#endif
    {"GPS", "view:gps", &options_menu_view},
#ifdef CONFIG_HAS_COMPASS
    {"Compass", "view:compass", &compass_view},
#endif
#ifdef CONFIG_HAS_ENVIII
    {"ENV-III", "view:enviii", &enviii_view},
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    {"Accelerometer", "view:accel", &accelerometer_view},
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    {"NRF24", "view:nrf24", &options_menu_view},
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    {"SubGHz", "view:subghz", &subghz_view},
#endif
    {"Clock", "view:clock", &clock_view},
    {"Apps", "view:apps", &apps_menu_view},
    {"Settings", "view:settings", &options_menu_view},
    {"GhostLink", "view:ghostlink", &options_menu_view},
    {"Custom Command", "cmd:", NULL},
};

#define NUM_IO_BTN_PRESETS (sizeof(io_btn_presets) / sizeof(io_btn_presets[0]))

static const char* io_btn_preset_options[NUM_IO_BTN_PRESETS + 1];

static void build_io_btn_preset_options(void) {
    for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
        io_btn_preset_options[i] = io_btn_presets[i].name;
    }
    io_btn_preset_options[NUM_IO_BTN_PRESETS] = NULL;
}

static const char** get_io_btn_preset_options(void) {
    static bool initialized = false;
    if (!initialized) {
        build_io_btn_preset_options();
        initialized = true;
    }
    return io_btn_preset_options;
}

static int get_current_io_btn_action(const char* cmd) {
    if (!cmd || cmd[0] == '\0') return -1;
    for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
        const char* prefix = io_btn_presets[i].cmd_prefix;
        size_t prefix_len = strlen(prefix);
        if (strncmp(cmd, prefix, prefix_len) == 0) return i;
    }
    return -1;
}

static int get_settings_count_for_category(SettingsCategoryId cat_id) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (cat_id == SETTINGS_CAT_IO_BUTTONS) return 3;
#endif
    int count = 0;
    int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
    for (int i = 0; i < settings_count; i++) {
        if (settings_items[i].category_id == cat_id) {
            count++;
        }
    }
    return count;
}

static int get_setting_index_in_category(int position_in_category, SettingsCategoryId cat_id) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (cat_id == SETTINGS_CAT_IO_BUTTONS) {
        if (position_in_category == 0) return IO_BTN_EDIT_P10;
        if (position_in_category == 1) return IO_BTN_EDIT_P11;
        if (position_in_category == 2) return IO_BTN_EDIT_P12;
        return -1;
    }
#endif
    int current_pos = 0;
    int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
    for (int i = 0; i < settings_count; i++) {
        if (settings_items[i].category_id == cat_id) {
            if (current_pos == position_in_category) {
                return i;
            }
            current_pos++;
        }
    }
    return -1;
}

static bool is_settings_mode = false;

EOptionsMenuType SelectedMenuType = OT_Wifi;
int selected_item_index = 0;
lv_obj_t *root = NULL;
lv_obj_t *menu_container = NULL;
int num_items = 0;
unsigned long createdTimeInMs = 0;
static int opt_touch_start_x;
static int opt_touch_start_y;
static int opt_touch_last_x;
static int opt_touch_last_y;
static bool opt_touch_started = false;
static bool opt_touch_dragged = false;
static int opt_touch_drag_axis = 0;
static lv_obj_t *opt_touch_scroll_target = NULL;  // remembered for release-on-release scroll
static WifiMenuState opt_touch_wifi_state = WIFI_MENU_MAIN;
static int opt_touch_bluetooth_state = 0;
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int OPT_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int OPT_SWIPE_THRESHOLD_RATIO = 10;
#endif
static bool option_fired = false;
static bool option_invoked = false;
static int64_t option_input_blocked_until_us = 0;
static options_view_t *g_options_view = NULL;
static gui_select_overlay_t *settings_select_overlay = NULL;
static int settings_select_setting_index = -1;

// Add button declarations and constants
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static gui_touch_bar_t s_opt_touch_tb = {0};
static bool touch_on_scroll_btn = false; // Flag active between press and release on scroll buttons

// Add button declaration for back button
static lv_obj_t *back_btn = NULL;
static lv_obj_t *touch_bar = NULL;

// WiGLE help popup
static lv_obj_t *wigle_help_popup = NULL;
static lv_obj_t *wigle_help_close_btn = NULL;

// WiGLE manual-upload popup
static lv_obj_t *wigle_manual_popup = NULL;
static lv_obj_t *wigle_manual_upload_btn = NULL;
static lv_obj_t *wigle_manual_close_btn = NULL;
static lv_obj_t *wigle_manual_info_label = NULL;
static int wigle_manual_popup_selected = 0;

// WiGLE stats popup
static lv_obj_t *wigle_stats_popup = NULL;
static lv_obj_t *wigle_stats_down_btn = NULL;
static lv_obj_t *wigle_stats_close_btn = NULL;
static lv_obj_t *wigle_stats_body_label = NULL;
static lv_obj_t *wigle_stats_scroll = NULL;
static int wigle_stats_popup_selected = 1;

// --- Add Bluetooth submenu arrays and state ---
static const char * const bluetooth_main_options[] = {
    "Detect Devices", "List Detected Devices", "Advertiser Scan", "OUI Device Scan", "List Advertisers",
    "GATT Scan", "Aerial Detector", "Spam", "Raw", NULL
};
static const char * const bluetooth_oui_options[] = {
    "Enter OUI Prefix", "Search Vendors", NULL
};
static const char * const bluetooth_spam_options[] = {
    "BLE Spam - Apple", "BLE Spam - Microsoft", "BLE Spam - Samsung",
    "BLE Spam - Google", "BLE Spam - Random", "Stop BLE Spam", NULL
};
static const char * const bluetooth_raw_options[] = {
    "Raw BLE Scanner", NULL
};
static const char * const bluetooth_gatt_options[] = {
    "Start GATT Scan", "List GATT Devices", "Select GATT Device", "Enumerate Services", "Track Device", NULL
};
static const char * const bluetooth_aerial_options[] = {
    "Scan Aerial Devices", "List Aerial Devices", "Track Aerial Device", "Stop Aerial Scan", 
    "Spoof Test Drone", "Stop Spoofing", NULL
};

typedef enum {
    BLUETOOTH_MENU_MAIN,
    BLUETOOTH_MENU_DETECT_LIST,
    BLUETOOTH_MENU_DETECT_DETAILS,
    BLUETOOTH_MENU_ADV_LIST,
    BLUETOOTH_MENU_ADV_DETAILS,
    BLUETOOTH_MENU_GATT_LIST,
    BLUETOOTH_MENU_GATT_DETAILS,
    BLUETOOTH_MENU_OUI,
    BLUETOOTH_MENU_OUI_VENDOR_LIST,
    BLUETOOTH_MENU_SPAM,
    BLUETOOTH_MENU_RAW,
    BLUETOOTH_MENU_GATT,
    BLUETOOTH_MENU_AERIAL
} BluetoothMenuState;

static BluetoothMenuState current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;

typedef struct {
    bool valid;
    EOptionsMenuType menu_type;
    int selected;
    lv_coord_t scroll_y;
    WifiMenuState wifi_state;
    BluetoothMenuState bluetooth_state;
    DualCommMenuState dualcomm_state;
    int settings_root;
    int settings_category;
} options_menu_nav_state_t;

static options_menu_nav_state_t s_rendered_menu_state = {0};
static options_menu_nav_state_t s_resume_menu_state = {0};
static options_menu_nav_state_t s_pending_restore_state = {0};
static bool s_skip_history_capture_once = false;
static bool s_discard_resume_on_destroy = false;

static void rebuild_current_menu(void);

static options_menu_nav_state_t options_menu_capture_nav_state(void) {
    options_menu_nav_state_t state = {
        .valid = true,
        .menu_type = SelectedMenuType,
        .selected = selected_item_index,
        .wifi_state = current_wifi_menu_state,
        .bluetooth_state = current_bluetooth_menu_state,
        .dualcomm_state = current_dualcomm_menu_state,
        .settings_root = current_settings_root,
        .settings_category = current_settings_category,
    };
    if (menu_container && lv_obj_is_valid(menu_container)) {
        state.scroll_y = lv_obj_get_scroll_y(menu_container);
    }
    return state;
}

static bool options_menu_nav_states_match(const options_menu_nav_state_t *a,
                                          const options_menu_nav_state_t *b) {
    return a && b && a->valid && b->valid &&
           a->menu_type == b->menu_type &&
           a->wifi_state == b->wifi_state &&
           a->bluetooth_state == b->bluetooth_state &&
           a->dualcomm_state == b->dualcomm_state &&
           a->settings_root == b->settings_root &&
           a->settings_category == b->settings_category;
}

static void options_menu_push_rendered_state(void) {
    if (!s_rendered_menu_state.valid || s_skip_history_capture_once) {
        s_skip_history_capture_once = false;
        return;
    }

    options_menu_nav_state_t target = options_menu_capture_nav_state();
    if (options_menu_nav_states_match(&s_rendered_menu_state, &target)) {
        return;
    }

    options_menu_nav_state_t source = s_rendered_menu_state;
    source.selected = selected_item_index;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        source.scroll_y = lv_obj_get_scroll_y(menu_container);
    }
    gui_nav_state_t nav = {
        .scope = NAV_SCOPE_OPTIONS_MENU,
        .value = source.menu_type,
        .selection = source.selected,
        .scroll_y = source.scroll_y,
        .wifi_state = source.wifi_state,
        .bluetooth_state = source.bluetooth_state,
        .dualcomm_state = source.dualcomm_state,
        .settings_root = source.settings_root,
        .settings_category = source.settings_category,
    };
    gui_nav_history_push(&nav);
}

static bool options_menu_restore_previous_state(void) {
    gui_nav_state_t nav;
    while (gui_nav_history_peek(&nav) && nav.scope == NAV_SCOPE_OPTIONS_MENU) {
        /* A submenu can be rebuilt while its previous rendered state is still
         * on the stack. Discard that duplicate instead of making Back bounce
         * into the view the user just left. */
        if (nav.value == SelectedMenuType &&
            ((SelectedMenuType == OT_Wifi && nav.wifi_state == current_wifi_menu_state) ||
             (SelectedMenuType == OT_Bluetooth && nav.bluetooth_state == current_bluetooth_menu_state) ||
             (SelectedMenuType == OT_DualComm && nav.dualcomm_state == current_dualcomm_menu_state))) {
            gui_nav_history_pop(&nav);
            continue;
        }
        break;
    }
    if (!gui_nav_history_peek(&nav) || nav.scope != NAV_SCOPE_OPTIONS_MENU) {
        return false;
    }
    gui_nav_history_pop(&nav);

    SelectedMenuType = (EOptionsMenuType)nav.value;
    current_wifi_menu_state = (WifiMenuState)nav.wifi_state;
    current_bluetooth_menu_state = (BluetoothMenuState)nav.bluetooth_state;
    current_dualcomm_menu_state = (DualCommMenuState)nav.dualcomm_state;
    current_settings_root = nav.settings_root;
    current_settings_category = nav.settings_category;
    settings_submenu_depth = current_settings_category >= 0 ? 2 :
                             (current_settings_root >= 0 ? 1 : 0);
    is_settings_mode = SelectedMenuType == OT_Settings;
    s_pending_restore_state = (options_menu_nav_state_t){
        .valid = true,
        .menu_type = SelectedMenuType,
        .selected = nav.selection,
        .scroll_y = nav.scroll_y,
        .wifi_state = current_wifi_menu_state,
        .bluetooth_state = current_bluetooth_menu_state,
        .dualcomm_state = current_dualcomm_menu_state,
        .settings_root = current_settings_root,
        .settings_category = current_settings_category,
    };
    s_skip_history_capture_once = true;
    rebuild_current_menu();
    return true;
}

#define OPT_DRAG_AXIS_THRESHOLD 10
#define OPT_DRAG_AXIS_BIAS 4
#define OPT_DRAG_DELTA_DEADZONE 1
#define OPT_DRAG_MAX_STEP 64

static void opt_touch_reset(void) {
    opt_touch_started = false;
    opt_touch_dragged = false;
    opt_touch_drag_axis = 0;
    opt_touch_scroll_target = NULL;
}

static void opt_touch_begin(lv_indev_data_t *data) {
    opt_touch_started = true;
    opt_touch_dragged = false;
    opt_touch_drag_axis = 0;
    opt_touch_start_x = data->point.x;
    opt_touch_start_y = data->point.y;
    opt_touch_last_x = data->point.x;
    opt_touch_last_y = data->point.y;
    opt_touch_wifi_state = current_wifi_menu_state;
    opt_touch_bluetooth_state = current_bluetooth_menu_state;
}

static int opt_clamp_drag_delta(int delta) {
    if (abs(delta) <= OPT_DRAG_DELTA_DEADZONE) return 0;
    if (delta > OPT_DRAG_MAX_STEP) return OPT_DRAG_MAX_STEP;
    if (delta < -OPT_DRAG_MAX_STEP) return -OPT_DRAG_MAX_STEP;
    return delta;
}

static int opt_resolve_drag_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < OPT_DRAG_AXIS_THRESHOLD && abs_dy < OPT_DRAG_AXIS_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + OPT_DRAG_AXIS_BIAS) return 1;
    if (abs_dx >= abs_dy + OPT_DRAG_AXIS_BIAS) return 2;
    return 0;
}

/*
 * Pick the scrollable target inside a detail view based on where the touch
 * started. When the press began over the info panel we scroll that (the
 * overflow region); otherwise we fall back to the action list. This makes the
 * info/detail region touch-scrollable the same way the action list already is.
 */
static lv_obj_t *opt_detail_scroll_target(detail_view_t *dv, lv_coord_t start_x, lv_coord_t start_y) {
    if (!dv) return NULL;
    lv_obj_t *info = detail_view_get_info_panel(dv);
    if (info && lv_obj_is_valid(info)) {
        lv_area_t a;
        lv_obj_get_coords(info, &a);
        if (start_x >= a.x1 && start_x <= a.x2 && start_y >= a.y1 && start_y <= a.y2) {
            return info;
        }
    }
    return detail_view_get_list(dv);
}


// forward declaration for incremental builder callback
static void menu_builder_cb(lv_timer_t *t);
static void change_setting_value(int setting_index, bool increment); // Forward Declaration
static void apply_setting_change(int setting_index, int new_value);
static void settings_refresh_row_label(int setting_index);
static void ir_tx_pin_kb_cb(const char *text);
static void ir_rx_pin_kb_cb(const char *text);
static bool settings_select_overlay_is_open(void);
static void settings_select_open(int setting_index);
static void settings_select_close(void);
static bool settings_select_handle_input(InputEvent *event);
static bool settings_confirm_handle_input(InputEvent *event);
static void settings_confirm_import_cb(void *user_data);
static void settings_confirm_factory_reset_cb(void *user_data);

static lv_timer_t *menu_build_timer = NULL;
static bool s_back_option_added = false;
static const char * const *current_options_list = NULL;
static int build_item_index = 0;
static int button_height_global = 0;
static bool is_small_screen_global = false;

/* Panel-derived base row height before the user's Row Height preset is applied. */
static int base_button_height(void) {
    int h = is_small_screen_global ? 40 : 55;
#ifdef CONFIG_IS_ATOMS3R
    h = 32;
#endif
    return h;
}

static void rebuild_current_menu(void); // Forward declaration
static void portal_free_cache(void);    // Forward declaration

static void update_scroll_buttons_visibility(void);
const char *options_menu_type_to_string(EOptionsMenuType menuType);

// ============================================================================
// ARP Scan Flow
// ============================================================================

static void arp_list_cleanup(void) {
    if (arp_scan_poll_timer) {
        lv_timer_del(arp_scan_poll_timer);
        arp_scan_poll_timer = NULL;
    }
    if (arp_list_menu) {
        paged_menu_destroy(arp_list_menu);
        arp_list_menu = NULL;
    }
    if (arp_scan_status) {
        scan_status_close(arp_scan_status);
        arp_scan_status = NULL;
    }
    if (arp_detail_view) {
        detail_view_destroy(arp_detail_view);
        arp_detail_view = NULL;
    }
}

static int arp_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                             bool *has_more, void *user_data) {
    (void)user_data;
    int count = arp_scan_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }
    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        const arp_host_t *host = arp_scan_get_host(i);
        if (host) {
            char mac_str[18];
            char vendor[64] = {0};
            format_mac_address(host->mac, mac_str, sizeof(mac_str), true);
            ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s  %s",
                     host->ip, vendor[0] ? vendor : mac_str);
            loaded++;
        }
    }
    *has_more = (offset + loaded) < count;
    return loaded;
}

static const char **arp_list_get_options(void) {
    if (!arp_list_menu) {
        arp_list_menu = paged_menu_create(ARP_LIST_PAGE_SIZE, arp_list_load_fn, NULL);
    }
    return paged_menu_get_options(arp_list_menu);
}

static void arp_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (arp_detail_view) {
        detail_view_destroy(arp_detail_view);
        arp_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_ARP_LIST;
    rebuild_current_menu();
}

static void arp_host_scan_cb(lv_event_t *e) {
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    const arp_host_t *host = (selected_arp_index >= 0) ? arp_scan_get_host(selected_arp_index) : NULL;
    if (!host) {
        error_popup_create("Host not found");
        return;
    }

    char cmd[64];
    switch (action) {
        case 0: snprintf(cmd, sizeof(cmd), "scanports %s", host->ip); break;
        case 1: snprintf(cmd, sizeof(cmd), "scanssh %s", host->ip); break;
        case 2: snprintf(cmd, sizeof(cmd), "netbiosscan %s", host->ip); break;
        case 3: snprintf(cmd, sizeof(cmd), "httpbannerscan %s", host->ip); break;
        case 4: snprintf(cmd, sizeof(cmd), "snmpprobe %s", host->ip); break;
        case 5: snprintf(cmd, sizeof(cmd), "snmpprobe walk %s", host->ip); break;
        case 7: snprintf(cmd, sizeof(cmd), "mdnssniff %s", host->ip); break;
        default: snprintf(cmd, sizeof(cmd), "enumscan %s", host->ip); break;
    }

    if (arp_detail_view) {
        detail_view_destroy(arp_detail_view);
        arp_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_ARP_LIST;
    suppress_wifi_state_reset_once = true;
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
}

static void show_arp_detail(int index) {
    const arp_host_t *host = arp_scan_get_host(index);
    if (!host) {
        error_popup_create("Host not found");
        return;
    }
    selected_arp_index = index;

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    if (arp_detail_view) {
        detail_view_destroy(arp_detail_view);
    }
    arp_detail_view = detail_view_create(lv_scr_act(), "ARP Host");
    reserve_detail_touch_bar_space(arp_detail_view);

    char mac_str[18];
    format_mac_address(host->mac, mac_str, sizeof(mac_str), true);

    detail_view_add_info(arp_detail_view, "IP", host->ip);
    detail_view_add_info(arp_detail_view, "MAC", mac_str);

    char vendor[64] = {0};
    ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));
    if (vendor[0]) {
        detail_view_add_info(arp_detail_view, "Vendor", vendor);
    }

    bool compact_detail = use_compact_wifi_detail_layout();
    if (!compact_detail) {
        detail_view_add_info(arp_detail_view, "Actions:", "");
    }
    detail_view_add_action(arp_detail_view, "Scan Open Ports", arp_host_scan_cb, (void *)(intptr_t)0);
    detail_view_add_action(arp_detail_view, "SSH Banner Scan", arp_host_scan_cb, (void *)(intptr_t)1);
    detail_view_add_action(arp_detail_view, "NetBIOS Scan", arp_host_scan_cb, (void *)(intptr_t)2);
    detail_view_add_action(arp_detail_view, "HTTP Banner", arp_host_scan_cb, (void *)(intptr_t)3);
    detail_view_add_action(arp_detail_view, "SNMP Probe", arp_host_scan_cb, (void *)(intptr_t)4);
    detail_view_add_action(arp_detail_view, "SNMP Walk", arp_host_scan_cb, (void *)(intptr_t)5);
    detail_view_add_action(arp_detail_view, "SMB Enum", arp_host_scan_cb, (void *)(intptr_t)6);
    detail_view_add_action(arp_detail_view, "Sniff Local Names", arp_host_scan_cb, (void *)(intptr_t)7);

    detail_view_add_back(arp_detail_view, arp_detail_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_ARP_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void arp_scan_complete_callback(void) {
    if (arp_scan_status) {
        scan_status_close(arp_scan_status);
        arp_scan_status = NULL;
    }
    int count = arp_scan_get_count();
    if (count == 0) {
        error_popup_create("No hosts found");
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    if (arp_list_menu) {
        paged_menu_reset(arp_list_menu);
    }
    current_wifi_menu_state = WIFI_MENU_ARP_LIST;
    rebuild_current_menu();
}

static void arp_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (arp_scan_check_done()) {
        lv_timer_del(arp_scan_poll_timer);
        arp_scan_poll_timer = NULL;
        arp_scan_finish_async();
        if (arp_scan_cancel_requested) {
            arp_scan_cancel_requested = false;
            arp_scan_clear_results();
            return;
        }
        arp_scan_complete_callback();
        return;
    }
    // Update spinner with live progress
    int pass = 0, total_passes = 0, scanned = 0, total_hosts = 0, found = 0;
    arp_scan_get_progress(&pass, &total_passes, &scanned, &total_hosts, &found);
    if (total_hosts > 0 && arp_scan_status) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Pass %d/%d  %d/%d  %d found",
                 pass, total_passes, scanned, total_hosts, found);
        scan_status_set_subtext(arp_scan_status, buf);
    }
}

static void arp_scan_cancel_cleanup(void *arg) {
    (void)arg;
    if (arp_scan_status) {
        scan_status_close(arp_scan_status);
        arp_scan_status = NULL;
    }
    opt_touch_started = false;
    option_fired = false;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
}

static void arp_scan_cancel_cb(void) {
    if (arp_scan_cancel_requested) return;
    arp_scan_cancel_requested = true;
    option_input_blocked_until_us = esp_timer_get_time() + 500000;
    arp_scan_cancel();
    lv_async_call(arp_scan_cancel_cleanup, NULL);
}

static bool start_arp_scan_flow(void) {
    arp_list_cleanup();
    arp_scan_cancel_requested = false;
    arp_scan_status = scan_status_create("ARP Scanning");
    if (arp_scan_status) {
        scan_status_set_subtext(arp_scan_status, "Tap to cancel");
        scan_status_set_cancel_cb(arp_scan_status, arp_scan_cancel_cb);
    }
    esp_err_t err = arp_scan_start_async();
    if (err != ESP_OK) {
        if (arp_scan_status) {
            scan_status_close(arp_scan_status);
            arp_scan_status = NULL;
        }
        return false;
    }
    arp_scan_poll_timer = lv_timer_create(arp_scan_poll_timer_cb, 100, NULL);
    return true;
}

// ============================================================================
// Govee LAN Discovery Flow
// ============================================================================

static void govee_list_cleanup(void) {
    if (govee_scan_is_running()) {
        govee_scan_cancel();
    }
    if (govee_scan_poll_timer) {
        lv_timer_del(govee_scan_poll_timer);
        govee_scan_poll_timer = NULL;
    }
    if (govee_list_menu) {
        paged_menu_destroy(govee_list_menu);
        govee_list_menu = NULL;
    }
    if (govee_scan_status) {
        scan_status_close(govee_scan_status);
        govee_scan_status = NULL;
    }
    if (govee_detail_view) {
        detail_view_destroy(govee_detail_view);
        govee_detail_view = NULL;
    }
}

static int govee_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                              bool *has_more, void *user_data) {
    (void)user_data;
    int count = govee_scan_get_count();
    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        const govee_device_t *device = govee_scan_get_device(i);
        if (device) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s  %s",
                     device->sku[0] ? device->sku : "Govee Light", device->ip);
            loaded++;
        }
    }
    *has_more = (offset + loaded) < count;
    return loaded;
}

static const char **govee_list_get_options(void) {
    if (!govee_list_menu) {
        govee_list_menu = paged_menu_create(GOVEE_LIST_PAGE_SIZE, govee_list_load_fn, NULL);
    }
    return paged_menu_get_options(govee_list_menu);
}

static void govee_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (govee_detail_view) {
        detail_view_destroy(govee_detail_view);
        govee_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_GOVEE_LIST;
    rebuild_current_menu();
}

static void govee_detail_action_cb(lv_event_t *e) {
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    const govee_device_t *device = selected_govee_index >= 0 ?
                                        govee_scan_get_device(selected_govee_index) : NULL;
    if (!device) {
        error_popup_create("Govee device not found");
        return;
    }

    if (action == 0 || action == 1) {
        if (govee_set_power(device->ip, action == 0) == ESP_OK) {
            toast_show_duration(action == 0 ? "Govee turned on" : "Govee turned off", TOAST_SUCCESS, 1500);
        } else {
            toast_show_duration("Govee command failed", TOAST_ERROR, 1500);
        }
        return;
    }

    keyboard_view_set_return_view(&options_menu_view);
    keyboard_view_set_submit_callback(action == 2 ? govee_brightness_kb_cb : govee_color_kb_cb);
    keyboard_view_set_placeholder(action == 2 ? "Brightness 0-100" : "Color RRGGBB (e.g. FF6600)");
    keyboard_view_set_initial_text("");
    display_manager_switch_view(&keyboard_view);
}

static void show_govee_detail(int index) {
    const govee_device_t *device = govee_scan_get_device(index);
    if (!device) {
        error_popup_create("Govee device not found");
        return;
    }
    selected_govee_index = index;
    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }
    if (govee_detail_view) detail_view_destroy(govee_detail_view);
    govee_detail_view = detail_view_create(lv_scr_act(), "Govee Light");
    reserve_detail_touch_bar_space(govee_detail_view);
    detail_view_add_info(govee_detail_view, "Model", device->sku[0] ? device->sku : "Unknown");
    detail_view_add_info(govee_detail_view, "IP", device->ip);
    if (device->device[0]) detail_view_add_info(govee_detail_view, "Device", device->device);
    if (device->version[0]) detail_view_add_info(govee_detail_view, "Firmware", device->version);
    if (!use_compact_wifi_detail_layout()) detail_view_add_info(govee_detail_view, "Actions:", "");
    detail_view_add_action(govee_detail_view, "Turn On", govee_detail_action_cb, (void *)(intptr_t)0);
    detail_view_add_action(govee_detail_view, "Turn Off", govee_detail_action_cb, (void *)(intptr_t)1);
    detail_view_add_action(govee_detail_view, "Set Brightness", govee_detail_action_cb, (void *)(intptr_t)2);
    detail_view_add_action(govee_detail_view, "Set Color", govee_detail_action_cb, (void *)(intptr_t)3);
    detail_view_add_back(govee_detail_view, govee_detail_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_GOVEE_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void govee_scan_complete_callback(void) {
    if (govee_scan_status) {
        scan_status_close(govee_scan_status);
        govee_scan_status = NULL;
    }
    if (govee_scan_get_count() == 0) {
        error_popup_create("No Govee lights found\nEnable LAN Control in Govee Home");
        current_wifi_menu_state = WIFI_MENU_GOVEE;
        rebuild_current_menu();
        return;
    }
    if (govee_list_menu) paged_menu_reset(govee_list_menu);
    current_wifi_menu_state = WIFI_MENU_GOVEE_LIST;
    rebuild_current_menu();
}

static void govee_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (!govee_scan_check_done()) return;
    lv_timer_del(govee_scan_poll_timer);
    govee_scan_poll_timer = NULL;
    if (govee_scan_cancel_requested) {
        govee_scan_cancel_requested = false;
        govee_scan_clear_results();
        if (govee_scan_status) {
            scan_status_close(govee_scan_status);
            govee_scan_status = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_GOVEE;
        rebuild_current_menu();
        return;
    }
    govee_scan_complete_callback();
}

static void govee_scan_cancel_cb(void) {
    if (govee_scan_cancel_requested) return;
    govee_scan_cancel_requested = true;
    govee_scan_cancel();
}

static bool start_govee_scan_flow(void) {
    govee_list_cleanup();
    govee_scan_cancel_requested = false;
    govee_scan_status = scan_status_create("Scanning Govee Lights");
    if (govee_scan_status) {
        scan_status_set_subtext(govee_scan_status, "Tap to cancel");
        scan_status_set_cancel_cb(govee_scan_status, govee_scan_cancel_cb);
    }
    esp_err_t err = govee_scan_start_async();
    if (err != ESP_OK) {
        if (govee_scan_status) {
            scan_status_close(govee_scan_status);
            govee_scan_status = NULL;
        }
        return false;
    }
    govee_scan_poll_timer = lv_timer_create(govee_scan_poll_timer_cb, 100, NULL);
    return true;
}

// ============================================================================
// mDNS Discovery Flow
// ============================================================================

static void mdns_list_cleanup(void) {
    if (mdns_scan_poll_timer) {
        lv_timer_del(mdns_scan_poll_timer);
        mdns_scan_poll_timer = NULL;
    }
    if (mdns_list_menu) {
        paged_menu_destroy(mdns_list_menu);
        mdns_list_menu = NULL;
    }
    if (mdns_scan_status) {
        scan_status_close(mdns_scan_status);
        mdns_scan_status = NULL;
    }
    if (mdns_detail_view) {
        detail_view_destroy(mdns_detail_view);
        mdns_detail_view = NULL;
    }
    wifi_manager_ip_lookup_clear();
}

static int mdns_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                              bool *has_more, void *user_data) {
    (void)user_data;
    int count = wifi_manager_ip_lookup_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }
    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        const mdns_device_t *dev = wifi_manager_ip_lookup_get_device(i);
        if (dev) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s  %s", dev->hostname[0] ? dev->hostname : dev->ip, dev->ip);
            loaded++;
        }
    }
    *has_more = (offset + loaded) < count;
    return loaded;
}

static const char **mdns_list_get_options(void) {
    if (!mdns_list_menu) {
        mdns_list_menu = paged_menu_create(MDNS_LIST_PAGE_SIZE, mdns_list_load_fn, NULL);
    }
    return paged_menu_get_options(mdns_list_menu);
}

static void mdns_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (mdns_detail_view) {
        detail_view_destroy(mdns_detail_view);
        mdns_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_MDNS_LIST;
    rebuild_current_menu();
}

static void show_mdns_detail(int index) {
    const mdns_device_t *dev = wifi_manager_ip_lookup_get_device(index);
    if (!dev) {
        error_popup_create("Device not found");
        return;
    }
    selected_mdns_index = index;

    if (mdns_detail_view) {
        detail_view_destroy(mdns_detail_view);
    }
    mdns_detail_view = detail_view_create(lv_scr_act(), "mDNS Device");
    reserve_detail_touch_bar_space(mdns_detail_view);

    if (dev->hostname[0]) {
        detail_view_add_info(mdns_detail_view, "Hostname", dev->hostname);
    }
    detail_view_add_info(mdns_detail_view, "IP", dev->ip);
    if (dev->port > 0) {
        char port_str[8];
        snprintf(port_str, sizeof(port_str), "%u", dev->port);
        detail_view_add_info(mdns_detail_view, "Port", port_str);
    }
    if (dev->service_type[0]) {
        detail_view_add_info(mdns_detail_view, "Service", dev->service_type);
    }

    detail_view_add_back(mdns_detail_view, mdns_detail_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_MDNS_DETAILS;
}

static void mdns_scan_complete_callback(void) {
    if (mdns_scan_status) {
        scan_status_close(mdns_scan_status);
        mdns_scan_status = NULL;
    }
    int count = wifi_manager_ip_lookup_get_count();
    if (count == 0) {
        error_popup_create("No devices found");
        current_wifi_menu_state = WIFI_MENU_NETWORK;
        rebuild_current_menu();
        return;
    }
    if (mdns_list_menu) {
        paged_menu_reset(mdns_list_menu);
    }
    current_wifi_menu_state = WIFI_MENU_MDNS_LIST;
    rebuild_current_menu();
}

static void mdns_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (wifi_manager_ip_lookup_check_done()) {
        lv_timer_del(mdns_scan_poll_timer);
        mdns_scan_poll_timer = NULL;
        wifi_manager_ip_lookup_finish_async();
        if (mdns_scan_cancel_requested) {
            mdns_scan_cancel_requested = false;
            wifi_manager_ip_lookup_clear();
            return;
        }
        mdns_scan_complete_callback();
    }
}

static void mdns_scan_cancel_cleanup(void *arg) {
    (void)arg;
    if (mdns_scan_status) {
        scan_status_close(mdns_scan_status);
        mdns_scan_status = NULL;
    }
    opt_touch_started = false;
    option_fired = false;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
}

static void mdns_scan_cancel_cb(void) {
    if (mdns_scan_cancel_requested) return;
    mdns_scan_cancel_requested = true;
    option_input_blocked_until_us = esp_timer_get_time() + 500000;
    lv_async_call(mdns_scan_cancel_cleanup, NULL);
}

static bool start_mdns_scan_flow(void) {
    mdns_list_cleanup();
    mdns_scan_cancel_requested = false;
    mdns_scan_status = scan_status_create("mDNS Discovery");
    if (mdns_scan_status) {
        scan_status_set_subtext(mdns_scan_status, "Tap to cancel");
        scan_status_set_cancel_cb(mdns_scan_status, mdns_scan_cancel_cb);
    }
    esp_err_t err = wifi_manager_start_ip_lookup_async();
    if (err != ESP_OK) {
        if (mdns_scan_status) {
            scan_status_close(mdns_scan_status);
            mdns_scan_status = NULL;
        }
        return false;
    }
    mdns_scan_poll_timer = lv_timer_create(mdns_scan_poll_timer_cb, 100, NULL);
    return true;
}

// ============================================================================
// Enum4linux Scan Flow
// ============================================================================

static void enum_list_cleanup(void) {
    if (enum_scan_poll_timer) {
        lv_timer_del(enum_scan_poll_timer);
        enum_scan_poll_timer = NULL;
    }
    if (enum_list_menu) {
        paged_menu_destroy(enum_list_menu);
        enum_list_menu = NULL;
    }
    if (enum_scan_status) {
        scan_status_close(enum_scan_status);
        enum_scan_status = NULL;
    }
    if (enum_detail_view) {
        detail_view_destroy(enum_detail_view);
        enum_detail_view = NULL;
    }
    enum_scan_clear_results();
}

static int enum_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                              bool *has_more, void *user_data) {
    (void)user_data;
    int count = enum_scan_get_count();
    if (count <= 0) { *has_more = false; return 0; }
    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        const enum_host_t *host = enum_scan_get_host(i);
        if (host) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s  %s",
                     host->ip, host->hostname[0] ? host->hostname : "SMB");
            loaded++;
        }
    }
    *has_more = (offset + loaded) < count;
    return loaded;
}

static const char **enum_list_get_options(void) {
    if (!enum_list_menu) {
        enum_list_menu = paged_menu_create(ENUM_LIST_PAGE_SIZE, enum_list_load_fn, NULL);
    }
    return paged_menu_get_options(enum_list_menu);
}

static void enum_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (enum_detail_view) {
        detail_view_destroy(enum_detail_view);
        enum_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_ENUM_LIST;
    rebuild_current_menu();
}

static void show_enum_detail(int index) {
    const enum_host_t *host = enum_scan_get_host(index);
    if (!host) {
        error_popup_create("Host not found");
        return;
    }
    selected_enum_index = index;

    if (enum_detail_view) {
        detail_view_destroy(enum_detail_view);
    }
    enum_detail_view = detail_view_create(lv_scr_act(), "Enum Results");
    reserve_detail_touch_bar_space(enum_detail_view);

    detail_view_add_info(enum_detail_view, "IP", host->ip);
    if (host->hostname[0])
        detail_view_add_info(enum_detail_view, "Hostname", host->hostname);
    if (host->os_version[0])
        detail_view_add_info(enum_detail_view, "OS", host->os_version);
    if (host->domain[0])
        detail_view_add_info(enum_detail_view, "Domain", host->domain);

    if (host->share_count > 0) {
        detail_view_add_header(enum_detail_view, "Shares");
        for (int i = 0; i < host->share_count; i++) {
            detail_view_add_info(enum_detail_view, host->shares[i].name,
                                 host->shares[i].type);
        }
    }

    if (host->user_count > 0) {
        detail_view_add_header(enum_detail_view, "Users");
        char user_list[256] = {0};
        size_t pos = 0;
        for (int i = 0; i < host->user_count && pos < sizeof(user_list) - 1; i++) {
            int w = snprintf(&user_list[pos], sizeof(user_list) - pos,
                             "%s%s", i > 0 ? ", " : "", host->users[i]);
            if (w > 0) pos += (size_t)w;
        }
        detail_view_add_info(enum_detail_view, "Users", user_list);
    }

    detail_view_add_back(enum_detail_view, enum_detail_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_ENUM_DETAILS;
}

static void enum_scan_complete_callback(void) {
    if (enum_scan_status) {
        scan_status_close(enum_scan_status);
        enum_scan_status = NULL;
    }
    int count = enum_scan_get_count();
    if (count == 0) {
        error_popup_create("No SMB hosts found");
        current_wifi_menu_state = WIFI_MENU_NETWORK;
        rebuild_current_menu();
        return;
    }
    if (enum_list_menu) {
        paged_menu_reset(enum_list_menu);
    }
    current_wifi_menu_state = WIFI_MENU_ENUM_LIST;
    rebuild_current_menu();
}

static void enum_scan_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (enum_scan_check_done()) {
        lv_timer_del(enum_scan_poll_timer);
        enum_scan_poll_timer = NULL;
        enum_scan_finish_async();
        if (enum_scan_cancel_requested) {
            enum_scan_cancel_requested = false;
            enum_scan_clear_results();
            return;
        }
        enum_scan_complete_callback();
    }
}

static void enum_scan_cancel_cleanup(void *arg) {
    (void)arg;
    if (enum_scan_status) {
        scan_status_close(enum_scan_status);
        enum_scan_status = NULL;
    }
    opt_touch_started = false;
    option_fired = false;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
}

static void enum_scan_cancel_cb(void) {
    if (enum_scan_cancel_requested) return;
    enum_scan_cancel_requested = true;
    option_input_blocked_until_us = esp_timer_get_time() + 500000;
    enum_scan_cancel();
    lv_async_call(enum_scan_cancel_cleanup, NULL);
}

static bool start_enum_scan_flow(void) {
    enum_list_cleanup();
    enum_scan_cancel_requested = false;
    enum_scan_status = scan_status_create("Enum Scanning");
    if (enum_scan_status) {
        scan_status_set_subtext(enum_scan_status, "Tap to cancel");
        scan_status_set_cancel_cb(enum_scan_status, enum_scan_cancel_cb);
    }
    esp_err_t err = enum_scan_start_async();
    if (err != ESP_OK) {
        if (enum_scan_status) {
            scan_status_close(enum_scan_status);
            enum_scan_status = NULL;
        }
        return false;
    }
    enum_scan_poll_timer = lv_timer_create(enum_scan_poll_timer_cb, 100, NULL);
    return true;
}

// ============================================================================
// Sweep Flow
// ============================================================================

static void sweep_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (sweep_detail_view) {
        detail_view_destroy(sweep_detail_view);
        sweep_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_ENVIRONMENT;
    rebuild_current_menu();
}

static void show_sweep_detail(void) {
    const sweep_result_t *res = sweep_get_result();
    if (!res) return;

    if (sweep_detail_view) {
        detail_view_destroy(sweep_detail_view);
    }
    sweep_detail_view = detail_view_create(lv_scr_act(), "Sweep Results");
    reserve_detail_touch_bar_space(sweep_detail_view);

    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%d", res->ap_count);
    detail_view_add_info(sweep_detail_view, "WiFi APs", count_str);

    snprintf(count_str, sizeof(count_str), "%d", res->station_count);
    detail_view_add_info(sweep_detail_view, "Stations", count_str);

    snprintf(count_str, sizeof(count_str), "%d", res->flipper_count);
    detail_view_add_info(sweep_detail_view, "Flippers", count_str);

    snprintf(count_str, sizeof(count_str), "%d", res->gatt_count);
    detail_view_add_info(sweep_detail_view, "BLE Devices", count_str);

    if (res->zigbee_count > 0) {
        snprintf(count_str, sizeof(count_str), "%d", res->zigbee_count);
        detail_view_add_info(sweep_detail_view, "802.15.4", count_str);
    }

    detail_view_add_back(sweep_detail_view, sweep_detail_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_ENVIRONMENT;
}

static void sweep_complete_callback(void) {
    if (sweep_scan_status) {
        scan_status_close(sweep_scan_status);
        sweep_scan_status = NULL;
    }
    sweep_finish_async();
    show_sweep_detail();
}

static void sweep_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    const sweep_result_t *res = sweep_get_result();
    if (res && sweep_scan_status) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Phase %d/6...", res->current_phase);
        scan_status_set_subtext(sweep_scan_status, msg);
    }
    if (sweep_check_done()) {
        lv_timer_del(sweep_poll_timer);
        sweep_poll_timer = NULL;
        sweep_complete_callback();
    }
}

static bool start_sweep_flow(void) {
    if (sweep_scan_status) {
        scan_status_close(sweep_scan_status);
        sweep_scan_status = NULL;
    }
    if (sweep_poll_timer) {
        lv_timer_del(sweep_poll_timer);
        sweep_poll_timer = NULL;
    }
    if (sweep_detail_view) {
        detail_view_destroy(sweep_detail_view);
        sweep_detail_view = NULL;
    }
    sweep_clear_result();
    sweep_scan_status = scan_status_create("Environment Sweep");
    if (sweep_scan_status) {
        scan_status_set_subtext(sweep_scan_status, "Starting...");
    }
    sweep_start_async(10, 10);
    sweep_poll_timer = lv_timer_create(sweep_poll_timer_cb, 200, NULL);
    return true;
}

static void sinkhole_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (sinkhole_detail_view) {
        detail_view_destroy(sinkhole_detail_view);
        sinkhole_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE;
    SelectedMenuType = OT_Wifi;
    suppress_wifi_state_reset_once = true;
    rebuild_current_menu();
    option_invoked = false;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void update_settings_arrows_visibility(void) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;

    uint32_t child_count = lv_obj_get_child_cnt(menu_container);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *btn = lv_obj_get_child(menu_container, i);
        if (!btn || !lv_obj_is_valid(btn)) continue;

        // Check if this is the selected item
        bool is_selected = (i == (uint32_t)selected_item_index);

        // Iterate through all children to find arrows (user_data == 2)
        uint32_t btn_child_count = lv_obj_get_child_cnt(btn);
        for (uint32_t j = 0; j < btn_child_count; j++) {
            lv_obj_t *child = lv_obj_get_child(btn, (int32_t)j);
            if (!child || !lv_obj_is_valid(child)) continue;

            // Only affect arrows (marked with user_data == 2)
            if (lv_obj_get_user_data(child) == (void *)2) {
#ifdef CONFIG_USE_TOUCHSCREEN
                // On touch devices, always show arrows
                (void)is_selected;
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
#else
                // On non-touch devices, only show arrows on selected item
                if (is_selected) {
                    lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                }
#endif
            }
        }
    }
}

static void decorate_settings_row_with_arrows(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return;

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    lv_obj_t *left = lv_label_create(btn);
    lv_label_set_text(left, LV_SYMBOL_LEFT);

    lv_obj_t *right = lv_label_create(btn);
    lv_label_set_text(right, LV_SYMBOL_RIGHT);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, 8, 0);
    lv_obj_set_style_pad_right(btn, 8, 0);

    const lv_font_t *font = (button_height_global <= 40) ? accessibility_get_font_body() : accessibility_get_font_title();
    lv_obj_set_style_text_font(left, font, 0);
    lv_obj_set_style_text_font(right, font, 0);

    // Set arrow text color to white (will be adjusted by apply_selected_style for selected item)
    lv_obj_set_style_text_color(left, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(right, lv_color_hex(0xFFFFFF), 0);

    lv_obj_set_user_data(left, (void *)2);
    lv_obj_set_user_data(right, (void *)2);

    // Set flex properties: arrows don't grow, label takes remaining space
    lv_obj_set_flex_grow(left, 0);
    lv_obj_set_flex_grow(right, 0);
    lv_obj_set_flex_grow(label, 1);

    // Label should center its text
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);

    // Always create arrows as visible - update_settings_arrows_visibility()
    // will hide them appropriately for non-touch devices
    lv_obj_clear_flag(left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(right, LV_OBJ_FLAG_HIDDEN);

    lv_obj_move_to_index(left, 0);
    lv_obj_move_to_index(label, 1);

    // Force layout update to ensure children are positioned correctly
    lv_obj_update_layout(btn);
}

// Find the iOS toggle child of a settings row, or NULL if the row uses arrows.
static lv_obj_t *find_row_toggle(lv_obj_t *btn) {
    if (!btn || !lv_obj_is_valid(btn)) return NULL;
    uint32_t n = lv_obj_get_child_cnt(btn);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(btn, (int32_t)i);
        if (child && lv_obj_get_user_data(child) == IOS_TOGGLE_USER_DATA) {
            return child;
        }
    }
    return NULL;
}

// Set up a settings row that displays a single label on the left and an
// iOS-style toggle on the right. Skips the arrow decoration entirely.
static void decorate_settings_row_with_toggle(lv_obj_t *btn, bool initial_value) {
    if (!btn || !lv_obj_is_valid(btn)) return;

    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    lv_obj_t *toggle = ios_toggle_create(btn);

    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(btn, GUI_SAFEAREA_HOR, 0);
    lv_obj_set_style_pad_right(btn, GUI_SAFEAREA_HOR, 0);

    // Label takes all remaining space and is left-aligned; toggle sits flush right.
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_flex_grow(toggle, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_width(label, LV_SIZE_CONTENT);

    // Set the value AFTER the row's flex is configured so the toggle's
    // track width is finalized and the knob lands in the right place on
    // the first frame.
    lv_obj_update_layout(btn);
    ios_toggle_set_value(toggle, initial_value, false);
}

// helper to show/hide touch scroll buttons based on list overflow
static void update_scroll_buttons_visibility(void) {
    lv_obj_t *target = NULL;
    bool force_show = false;

    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        target = detail_view_get_list(ap_detail_view);
        force_show = true;
    } else if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        target = detail_view_get_list(sta_detail_view);
        force_show = true;
    } else if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
        target = detail_view_get_list(sinkhole_detail_view);
        force_show = true;
    } else if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
        target = detail_view_get_list(arp_detail_view);
        force_show = true;
    } else if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
        target = detail_view_get_list(ble_detect_detail_view);
        force_show = true;
    } else if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
        target = detail_view_get_list(ble_adv_detail_view);
        force_show = true;
    } else if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
        target = detail_view_get_list(ble_gatt_detail_view);
        force_show = true;
    } else if (g_options_view && options_view_is_virtual(g_options_view)) {
        /* A windowed list never overflows its viewport, so the usual overflow
         * test would hide the touch bar the list is now navigated with. */
        target = menu_container;
        force_show = true;
    } else {
        target = menu_container;
    }

    if (!target || !lv_obj_is_valid(target)) return;
    if (force_show) {
        if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) {
            lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_up_btn);
        }
        if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) {
            lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(scroll_down_btn);
        }
        if (back_btn && lv_obj_is_valid(back_btn)) {
            lv_obj_move_foreground(back_btn);
        }
        return;
    }
    gui_touch_bar_update_visibility(&s_opt_touch_tb, target);
}

static void reserve_detail_touch_bar_space(detail_view_t *dv) {
#ifdef CONFIG_USE_TOUCHSCREEN
    if (dv) {
        detail_view_set_bottom_reserved(dv, gui_touch_bar_height());
    }
#else
    (void)dv;
#endif
}

static void select_option_item(int index); // Forward Declaration
static void back_event_cb(lv_event_t *e); // Forward Declaration for back button callback
static void ap_list_cleanup(void); // Forward Declaration for AP list cleanup
static void station_list_cleanup(void); // Forward Declaration for station list cleanup
static void ap_scan_complete_callback(void); // Forward Declaration for AP scan complete
static void ap_detail_back_cb(lv_event_t *e); // Forward Declaration for AP detail back
static void show_ap_detail(int ap_index); // Forward Declaration for AP detail view
static void station_scan_complete_callback(void); // Forward Declaration for station scan complete
static void station_detail_back_cb(lv_event_t *e); // Forward Declaration for station detail back
static void show_station_detail(int station_index); // Forward Declaration for station detail view
static void wigle_help_close_cb(lv_event_t *e); // Forward Declaration for WiGLE help close
static void wigle_manual_popup_close_cb(lv_event_t *e);
static void wigle_manual_popup_upload_cb(lv_event_t *e);
static void wigle_manual_popup_update_selection(void);
static void wigle_stats_popup_open(void);
static void wigle_stats_popup_close_cb(lv_event_t *e);
static void wigle_stats_popup_scroll(int delta_y);
static void wigle_stats_popup_scroll_down_cb(lv_event_t *e);
static void wigle_stats_popup_update_selection(void);
static void wigle_stats_popup_activate_selected(void);
static void wigle_get_popup_geometry(int *popup_w, int *popup_h, int *y_offset);
static void wigle_test_result_cb(bool success, const char *message);
static void wigle_manual_upload_result_cb(bool success, const char *message);
static void wigle_stats_result_cb(bool success, const char *message);
static void wifi_connect_kb_cb(const char *text);
static void ssh_scan_kb_cb(const char *text);
static void wol_kb_cb(const char *text);
static void govee_brightness_kb_cb(const char *text);
static void govee_color_kb_cb(const char *text);
static void netbios_scan_kb_cb(const char *text);
static void http_banner_kb_cb(const char *text);
static void snmp_probe_kb_cb(const char *text);
static void netbios_subnet_kb_cb(const char *text);
static void http_banner_subnet_kb_cb(const char *text);
static void snmp_probe_subnet_kb_cb(const char *text);
static void snmp_walk_subnet_kb_cb(const char *text);
static void dual_comm_netbios_subnet_kb_cb(const char *text);
static void dual_comm_http_banner_subnet_kb_cb(const char *text);
static void dual_comm_snmp_probe_subnet_kb_cb(const char *text);
static void dual_comm_connect_kb_cb(const char *text);
static void dual_comm_send_kb_cb(const char *text);
static void dual_comm_wifi_connect_kb_cb(const char *text);
static void dual_comm_apcred_kb_cb(const char *text);
static void dual_comm_karma_custom_ssids_cb(const char *text);
static void karma_portal_ssids_cb(const char *input);
static void dual_comm_dns_lookup_kb_cb(const char *text);
static void dual_comm_traceroute_kb_cb(const char *text);
static void dual_comm_http_request_kb_cb(const char *text);
static void ble_oui_prefix_kb_cb(const char *text);
static void ble_oui_vendor_search_kb_cb(const char *text);
static void wigle_csv_free_cache(void);
static const char **wigle_csv_load_page(void);
static void pcap_capture_free_cache(void);
static const char **pcap_capture_load_page(void);
static void wigle_show_csv_details_popup(const char *filename);
#ifdef CONFIG_USE_IO_EXPANDER
static void iobtn_p10_kb_cb(const char *text);
static void iobtn_p11_kb_cb(const char *text);
static void iobtn_p12_kb_cb(const char *text);
#endif
static void ap_ssid_kb_cb(const char *text);
static void ap_password_kb_cb(const char *text);
static void sta_ssid_kb_cb(const char *text);
static void sta_password_kb_cb(const char *text);
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
static void zigbee_capture_kb_cb(const char *text);
#endif

static void evil_portal_ssid_cb(const char *input) {
    if (!input || !selected_portal[0]) return;
    char ssid[64] = {0};
    char pass[64] = {0};
    const char *space = strchr(input, ' ');
    if (space) {
        size_t ssid_len = space - input;
        if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
            error_popup_create("ssid too long");
            return;
        }
        memcpy(ssid, input, ssid_len);
        ssid[ssid_len] = '\0';
        const char *pw = space + 1;
        size_t pass_len = strlen(pw);
        if (pass_len > 0) {
            if (pass_len < 8) {
                error_popup_create("Password must be at least 8 chars");
                return;
            }
            if (pass_len >= sizeof(pass)) {
                error_popup_create("pass too long");
                return;
            }
            memcpy(pass, pw, pass_len);
            pass[pass_len] = '\0';
        }
    } else {
        size_t ssid_len = strlen(input);
        if (ssid_len == 0 || ssid_len >= sizeof(ssid)) {
            error_popup_create("ssid too long");
            return;
        }
        memcpy(ssid, input, ssid_len);
        ssid[ssid_len] = '\0';
    }
    char cmd[256];
    if (pass[0]) {
        snprintf(cmd, sizeof(cmd), "startportal %s %s %s", selected_portal, ssid, pass);
    } else {
        snprintf(cmd, sizeof(cmd), "startportal %s %s", selected_portal, ssid);
    }
terminal_set_return_view(&options_menu_view);
display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
    selected_portal[0] = '\0';
}

// Add scroll functions
static void scroll_options_up(lv_event_t *e) {
    (void)e;
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        detail_view_step_up(ap_detail_view);
        return;
    }
    if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
        detail_view_step_up(sinkhole_detail_view);
        return;
    }
    if (gtk_abuse_detail_view) {
        detail_view_step_up(gtk_abuse_detail_view);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        detail_view_step_up(sta_detail_view);
        return;
    }
    if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
        detail_view_step_up(ble_detect_detail_view);
        return;
    }
    if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
        detail_view_step_up(ble_adv_detail_view);
        return;
    }
    if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
        detail_view_step_up(ble_gatt_detail_view);
        return;
    }
    if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
        detail_view_step_up(arp_detail_view);
        return;
    }
    if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
        detail_view_step_up(mdns_detail_view);
        return;
    }
    if (sweep_detail_view) {
        detail_view_step_up(sweep_detail_view);
        return;
    }
    if (!menu_container) return;
    if (g_options_view && options_view_is_virtual(g_options_view)) {
        /* A windowed list keeps its rows inside the viewport, so there is
         * nothing to scroll: step the selection instead. */
        select_option_item(selected_item_index - 1);
        return;
    }
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
}

static void scroll_options_down(lv_event_t *e) {
    (void)e;
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        detail_view_step_down(ap_detail_view);
        return;
    }
    if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
        detail_view_step_down(sinkhole_detail_view);
        return;
    }
    if (gtk_abuse_detail_view) {
        detail_view_step_down(gtk_abuse_detail_view);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        detail_view_step_down(sta_detail_view);
        return;
    }
    if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
        detail_view_step_down(ble_detect_detail_view);
        return;
    }
    if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
        detail_view_step_down(ble_adv_detail_view);
        return;
    }
    if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
        detail_view_step_down(ble_gatt_detail_view);
        return;
    }
    if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
        detail_view_step_down(arp_detail_view);
        return;
    }
    if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
        detail_view_step_down(mdns_detail_view);
        return;
    }
    if (sweep_detail_view) {
        detail_view_step_down(sweep_detail_view);
        return;
    }
    if (!menu_container) return;
    if (g_options_view && options_view_is_virtual(g_options_view)) {
        /* A windowed list keeps its rows inside the viewport, so there is
         * nothing to scroll: step the selection instead. */
        select_option_item(selected_item_index + 1);
        return;
    }
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
}

static void touch_back_button_cb(lv_event_t *e) {
    (void)e;
    if (track_meter && rssi_meter_is_active(track_meter)) {
        stop_track_flow();
        return;
    }
    if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        ap_detail_back_cb(NULL);
        return;
    }
    if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
        sinkhole_detail_back_cb(NULL);
        return;
    }
    if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        station_detail_back_cb(NULL);
        return;
    }
    if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
        ble_detect_detail_back_cb(NULL);
        return;
    }
    if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
        ble_adv_detail_back_cb(NULL);
        return;
    }
    if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
        ble_gatt_detail_back_cb(NULL);
        return;
    }
    if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
        arp_detail_back_cb(NULL);
        return;
    }
    if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
        mdns_detail_back_cb(NULL);
        return;
    }
    if (sweep_detail_view) {
        sweep_detail_back_cb(NULL);
        return;
    }
    back_event_cb(NULL);
}

const char *options_menu_type_to_string(EOptionsMenuType menuType) {
    switch (menuType) {
    case OT_Wifi:
        return "Wi-Fi";
    case OT_Bluetooth:
        return "BLE";
    case OT_GPS:
        return "GPS";
    case OT_DualComm:
        return "GhostLink";
    case OT_NRF24:
        return "NRF24";
    case OT_SubGhz:
        return "SubGHz";
    case OT_Settings:
        return "Settings";
    case OT_IOButtonPresets:
        return "IO Button Action";
    case OT_WigleManualUpload:
        return "WiGLE Upload";
    default:
        return "Unknown";
    }
}

static void up_down_event_cb(lv_event_t *e) {
int direction = (int)(intptr_t)lv_event_get_user_data(e);
select_option_item(selected_item_index + direction);
}

/* Theme palette now centralized in display_manager; selection colors applied by options_view */

static void close_one_scan_status(scan_status_t **slot) {
    if (slot && *slot) {
        scan_status_close(*slot);
        *slot = NULL;
    }
}

static void close_all_scan_status_overlays(void) {
    close_one_scan_status(&ap_scan_status);
    close_one_scan_status(&sta_scan_status);
    close_one_scan_status(&arp_scan_status);
    close_one_scan_status(&govee_scan_status);
    close_one_scan_status(&mdns_scan_status);
    close_one_scan_status(&sweep_scan_status);
    if (display_manager_get_current_view() == &options_menu_view) {
        close_one_scan_status(&ble_detect_status);
        close_one_scan_status(&ble_adv_status);
        close_one_scan_status(&ble_gatt_status);
        close_one_scan_status(&gtk_abuse_status);
    }
#if GHOSTESP_OTA_SUPPORTED
    ota_status_close_overlay();
    popup_confirm_close(&ota_result_popup);
#endif
}

static void options_menu_freeze_pre_lock(void) {
    /* Detail views live outside options_menu_view.root; remove and rebuild. */
    s_pending_detail_resume = RESUME_NONE;
    s_pending_detail_index = -1;

    /* Live RSSI tracker lives outside options_menu_view.root; stop tracking and
     * drop the overlay so it doesn't survive the lockscreen swap. */
    if (track_meter) {
        track_stop_current_source();
        track_source = TRACK_SRC_NONE;
        rssi_meter_destroy(track_meter);
        track_meter = NULL;
    }

    pending_detail_resume_t resume_id = RESUME_NONE;
    int resume_index = -1;

    if (SelectedMenuType == OT_Wifi) {
        if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            resume_id = RESUME_AP_DETAIL;
            resume_index = selected_ap_index;
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        } else if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            resume_id = RESUME_STA_DETAIL;
            resume_index = selected_station_index;
            detail_view_destroy(sta_detail_view);
            sta_detail_view = NULL;
        } else if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
            detail_view_destroy(sinkhole_detail_view);
            sinkhole_detail_view = NULL;
        } else if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
            detail_view_destroy(arp_detail_view);
            arp_detail_view = NULL;
        } else if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
            detail_view_destroy(mdns_detail_view);
            mdns_detail_view = NULL;
        } else if (sweep_detail_view) {
            detail_view_destroy(sweep_detail_view);
            sweep_detail_view = NULL;
        }
    } else if (SelectedMenuType == OT_Bluetooth) {
        if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
            resume_id = RESUME_BLE_DETECT_DETAIL;
            resume_index = selected_ble_detect_index;
            detail_view_destroy(ble_detect_detail_view);
            ble_detect_detail_view = NULL;
        } else if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
            resume_id = RESUME_BLE_ADV_DETAIL;
            resume_index = selected_ble_adv_index;
            detail_view_destroy(ble_adv_detail_view);
            ble_adv_detail_view = NULL;
        } else if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
            resume_id = RESUME_BLE_GATT_DETAIL;
            resume_index = selected_ble_gatt_index;
            detail_view_destroy(ble_gatt_detail_view);
            ble_gatt_detail_view = NULL;
        }
    } else if (gtk_abuse_detail_view) {
        /* Gtk abuse detail is not currently routed via *_menu_state for
         * resume; just destroy the orphan widget. */
        detail_view_destroy(gtk_abuse_detail_view);
        gtk_abuse_detail_view = NULL;
    }

    s_pending_detail_resume = resume_id;
    s_pending_detail_index = resume_index;

    close_all_scan_status_overlays();

    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        suppress_wifi_state_reset_once = true;
    } else if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state != BLUETOOTH_MENU_MAIN) {
        suppress_wifi_state_reset_once = true;
    }
}

static void options_menu_apply_pending_detail_resume(void) {
    if (s_pending_detail_resume == RESUME_NONE || s_pending_detail_index < 0) {
        return;
    }
    pending_detail_resume_t to_resume = s_pending_detail_resume;
    int resume_index = s_pending_detail_index;
    s_pending_detail_resume = RESUME_NONE;
    s_pending_detail_index = -1;

    /* Run after the menu has been built so the back/scroll chrome is ready. */
    switch (to_resume) {
    case RESUME_AP_DETAIL:
        if (SelectedMenuType == OT_Wifi) {
            show_ap_detail(resume_index);
        }
        break;
    case RESUME_STA_DETAIL:
        if (SelectedMenuType == OT_Wifi) {
            show_station_detail(resume_index);
        }
        break;
    case RESUME_BLE_DETECT_DETAIL:
        if (SelectedMenuType == OT_Bluetooth) {
            show_ble_detect_detail(resume_index);
        }
        break;
    case RESUME_BLE_ADV_DETAIL:
        if (SelectedMenuType == OT_Bluetooth) {
            show_ble_adv_detail(resume_index);
        }
        break;
    case RESUME_BLE_GATT_DETAIL:
        if (SelectedMenuType == OT_Bluetooth) {
            show_ble_gatt_detail(resume_index);
        }
        break;
    default:
        break;
    }
}

void options_menu_create() {
    /* 
     * Performance Note: Submenu states are preserved across destroy/create cycles
     * (e.g., current_wifi_menu_state, current_bluetooth_menu_state, etc.)
     * This allows seamless return from terminal view to the correct submenu.
     * When navigating BETWEEN submenus, use rebuild_current_menu() instead of
     * destroy/create to avoid expensive LVGL operations and watchdog starvation.
     */
    ESP_LOGI(TAG, "options_menu_create: SelectedMenuType=%d (%s)", SelectedMenuType, options_menu_type_to_string(SelectedMenuType));
    /* Only restore the captured nav state when the menu state has not been
     * deliberately changed since the options view was torn down. Keyboard
     * submit callbacks (e.g. BLE OUI vendor search / prefix, settings) set
     * their own target state before switching back here; restoring the stale
     * capture would clobber it and dump the user on the wrong menu. */
    bool restoring_view = s_resume_menu_state.valid &&
                          s_resume_menu_state.menu_type == SelectedMenuType &&
                          s_resume_menu_state.wifi_state == current_wifi_menu_state &&
                          s_resume_menu_state.bluetooth_state == current_bluetooth_menu_state &&
                          s_resume_menu_state.dualcomm_state == current_dualcomm_menu_state &&
                          s_resume_menu_state.settings_root == current_settings_root &&
                          s_resume_menu_state.settings_category == current_settings_category &&
                          gui_router_previous_view() != &main_menu_view &&
                          gui_router_previous_view() != &apps_menu_view;
    if (!restoring_view) {
        s_resume_menu_state.valid = false;
        s_pending_restore_state.valid = false;
    }
    if (restoring_view) {
        current_wifi_menu_state = s_resume_menu_state.wifi_state;
        current_bluetooth_menu_state = s_resume_menu_state.bluetooth_state;
        current_dualcomm_menu_state = s_resume_menu_state.dualcomm_state;
        current_settings_root = s_resume_menu_state.settings_root;
        current_settings_category = s_resume_menu_state.settings_category;
        settings_submenu_depth = current_settings_category >= 0 ? 2 :
                                 (current_settings_root >= 0 ? 1 : 0);
        s_pending_restore_state = s_resume_menu_state;
        s_resume_menu_state.valid = false;
    }
    settings_select_close();
    s_info_detail_active = false;
    
    // Reset WiFi menu state when entering from main menu to ensure clean entry
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        // Only reset if we're coming from main menu (not from terminal return)
        // This is detected by checking if the options view root is NULL
        if (!restoring_view && !options_menu_view.root && !suppress_wifi_state_reset_once) {
            ESP_LOGI(TAG, "Resetting WiFi menu state to MAIN on fresh entry");
            current_wifi_menu_state = WIFI_MENU_MAIN;
        }
    }
    suppress_wifi_state_reset_once = false;
    
    option_invoked = false;
    opt_touch_started = false;
    selected_item_index = 0;  // Reset selection to first item for new menu
    int screen_width = LV_HOR_RES;
    int screen_height = LV_VER_RES;

    bool is_small_screen = (screen_width <= 240 || screen_height <= 240);
#ifdef CONFIG_IS_ATOMS3R
    is_small_screen = true;
#endif

    /* Styling handled by options_view */

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));

    display_manager_fill_screen(bg_color);
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root(NULL, NULL, bg_color, LV_OPA_COVER);
    options_menu_view.root = root;
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    g_options_view = options_view_create(root, options_menu_type_to_string(SelectedMenuType));
    menu_container = options_view_get_list(g_options_view);

    // Scroll button visibility is updated once after the menu is fully built

    const char * const *options = NULL;
    is_settings_mode = false;
    switch (SelectedMenuType) {
    case OT_Wifi:
        switch (current_wifi_menu_state) {
            case WIFI_MENU_MAIN: options = wifi_main_options; break;
            case WIFI_MENU_ATTACKS: options = wifi_attacks_options; break;
            case WIFI_MENU_SCAN_SELECT: options = wifi_scan_select_options; break;
            case WIFI_MENU_ENVIRONMENT: options = wifi_environment_options; break;
            case WIFI_MENU_NETWORK: options = wifi_network_options; break;
            case WIFI_MENU_CAPTURE: options = wifi_capture_options; break;
            case WIFI_MENU_EVIL_PORTAL: options = wifi_evil_portal_options; break;
            case WIFI_MENU_DNS_SINKHOLE: options = wifi_dns_sinkhole_options; break;
            case WIFI_MENU_DNS_SINKHOLE_DOWNLOAD: options = wifi_dns_sinkhole_download_options; break;
            case WIFI_MENU_DNS_SINKHOLE_FILE_PICK:
                options = blocklist_file_options;
                break;
            case WIFI_MENU_DNS_SINKHOLE_DETAILS:
                options = NULL;
                break;
            case WIFI_MENU_CONNECTION: options = wifi_connection_options; break;
            case WIFI_MENU_MISC: options = wifi_misc_options; break;
            case WIFI_MENU_GOVEE:
                options = wifi_govee_options;
                break;
            case WIFI_MENU_GOVEE_LIST:
            case WIFI_MENU_GOVEE_DETAILS:
                /* Windowed list: rows come from the scan results on demand. */
                options = NULL;
                s_vlist_pending = true;
                break;
            case WIFI_MENU_EVIL_PORTAL_SELECT:
            {
                // Portal population is now handled in rebuild_current_menu
                // Just set a placeholder to indicate we're in the right state
                ESP_LOGI(TAG, "Evil portal select menu state activated");
                options = evil_portal_options;
                break;
            }
            case WIFI_MENU_KARMA_PORTAL_SELECT:
            {
                // Same portal list as evil portal select — population in rebuild_current_menu
                ESP_LOGI(TAG, "Karma portal select menu state activated");
                options = evil_portal_options;
                break;
            }
            case WIFI_MENU_AP_LIST:
            case WIFI_MENU_AP_DETAILS:
                /* Windowed list: rows come from the scan results on demand. */
                options = NULL;
                s_vlist_pending = true;
                break;
            case WIFI_MENU_STA_LIST:
            case WIFI_MENU_STA_DETAILS:
            case WIFI_MENU_SCANALL_LIST:
            case WIFI_MENU_ARP_LIST:
            case WIFI_MENU_ARP_DETAILS:
            case WIFI_MENU_MDNS_LIST:
            case WIFI_MENU_MDNS_DETAILS:
            case WIFI_MENU_ENUM_LIST:
            case WIFI_MENU_ENUM_DETAILS:
                /* Windowed list: rows come from the scan results on demand. */
                options = NULL;
                s_vlist_pending = true;
                break;
            case WIFI_MENU_AP_MULTI_SELECT:
                options = ap_multi_select_get_options();
                break;
            case WIFI_MENU_STA_MULTI_SELECT:
                options = sta_multi_select_get_options();
                break;
            case WIFI_MENU_CAPTURE_BROWSER:
                options = pcap_capture_load_page();
                break;
        }
        break;
    case OT_Bluetooth:
        switch (current_bluetooth_menu_state) {
            case BLUETOOTH_MENU_MAIN: options = bluetooth_main_options; break;
            case BLUETOOTH_MENU_DETECT_LIST:
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
                if (ble_device_detect_is_tracking()) {
                    ble_device_detect_stop_tracking();
                }
                if (ble_device_detect_is_active() && !ble_is_initialized()) {
                    ble_device_detect_stop();
                }
                if (ble_device_detect_get_count() <= 0 && !ble_device_detect_is_active()) {
                    start_ble_detect_flow();
                }
#endif
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_DETECT_DETAILS:
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_ADV_LIST:
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
                if (advertiser_scan_get_count() <= 0 && !advertiser_scan_is_active()) {
                    start_ble_adv_flow();
                }
#endif
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_ADV_DETAILS:
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_GATT_LIST:
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (gatt_scan_get_device_count() <= 0 && !gatt_scan_is_active()) {
                    start_ble_gatt_flow();
                }
#endif
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_GATT_DETAILS:
                options = NULL;
                s_vlist_pending = true;
                break;
            case BLUETOOTH_MENU_OUI: options = bluetooth_oui_options; break;
            case BLUETOOTH_MENU_OUI_VENDOR_LIST: options = ble_oui_vendor_list_get_options(); break;
            case BLUETOOTH_MENU_SPAM: options = bluetooth_spam_options; break;
            case BLUETOOTH_MENU_RAW: options = bluetooth_raw_options; break;
            case BLUETOOTH_MENU_GATT: options = bluetooth_gatt_options; break;
            case BLUETOOTH_MENU_AERIAL: options = bluetooth_aerial_options; break;
        }
        break;
    case OT_GPS: options = gps_options; break;
    case OT_NRF24:
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
        options = nrf24_options;
#else
        options = NULL;
#endif
        break;
    case OT_SubGhz:
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
        options = subghz_options;
#else
        options = NULL;
#endif
        break;
    case OT_DualComm:
        switch (current_dualcomm_menu_state) {
            case DUALCOMM_MENU_MAIN:     options = dual_comm_main_options; break;
            case DUALCOMM_MENU_SESSION:  options = dual_comm_session_options; break;
            case DUALCOMM_MENU_SCAN:     options = dual_comm_scan_options; break;
            case DUALCOMM_MENU_WIFI:     options = dual_comm_wifi_options; break;
            case DUALCOMM_MENU_ATTACKS:  options = dual_comm_attacks_options; break;
            case DUALCOMM_MENU_CAPTURE:  options = dual_comm_capture_options; break;
            case DUALCOMM_MENU_TOOLS:    options = dual_comm_tools_options; break;
            case DUALCOMM_MENU_BLE:      options = dual_comm_ble_options; break;
            case DUALCOMM_MENU_GPS:      options = dual_comm_gps_options; break;
            case DUALCOMM_MENU_ETHERNET: options = dual_comm_ethernet_options; break;
            case DUALCOMM_MENU_KEYBOARD: options = dual_comm_keyboard_options; break;
        }
        break;
    case OT_Settings: 
        is_settings_mode = true;
        if (!restoring_view) {
            current_settings_root = -1;
            current_settings_category = -1;
            settings_submenu_depth = 0;
        }
        {
            int count = asset_pack_get_installed_count();
            if (count <= 0) {
                asset_pack_options[0] = "None";
                asset_pack_option_count = 1;
            } else {
                for (int i = 0; i < count && i < ASSET_PACK_INSTALLED_MAX; ++i) {
                    asset_pack_options[i] = asset_pack_get_installed_name(i);
                }
                asset_pack_option_count = count;
            }
            for (int i = 0; i < settings_items_count; ++i) {
                if (settings_items[i].setting_type == SETTING_RELOAD_ASSET_PACK) {
                    settings_items[i].value_count = asset_pack_option_count;
                    settings_items[i].value_options = (const char * const *)asset_pack_options;
                    settings_items[i].current_value = asset_pack_get_current_index();
                    if (settings_items[i].current_value >= asset_pack_option_count)
                        settings_items[i].current_value = 0;
                    break;
                }
            }
        }
        load_current_settings_values();
        break;
    case OT_IOButtonPresets:
        is_settings_mode = false;
        break;
    case OT_WigleManualUpload:
        is_settings_mode = false;
        options = wigle_csv_load_page();
        break;
    default: options = NULL; break;
    }

    if (!is_settings_mode && options == NULL && !s_vlist_pending) {
        if (s_pending_detail_resume == RESUME_NONE) {
            display_manager_switch_view(&main_menu_view);
            return;
        }
        switch (s_pending_detail_resume) {
        case RESUME_AP_DETAIL:
            current_wifi_menu_state = ap_detail_return_state;
            if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
                options = scanall_list_get_options();
            } else {
                /* Windowed list: rows come from the scan results on demand. */
                options = NULL;
                s_vlist_pending = true;
            }
            break;
        case RESUME_STA_DETAIL:
            current_wifi_menu_state = sta_detail_return_state;
            if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
                options = scanall_list_get_options();
            } else {
                /* Windowed list: rows come from the scan results on demand. */
                options = NULL;
                s_vlist_pending = true;
            }
            break;
        case RESUME_BLE_DETECT_DETAIL:
            current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
            options = NULL;
            s_vlist_pending = true;
            break;
        case RESUME_BLE_ADV_DETAIL:
            current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
            options = NULL;
            s_vlist_pending = true;
            break;
        case RESUME_BLE_GATT_DETAIL:
            current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
            options = NULL;
            s_vlist_pending = true;
            break;
        default:
            display_manager_switch_view(&main_menu_view);
            return;
        }
    }

    num_items = 0;
    is_small_screen_global = is_small_screen;
    int button_height = options_view_scale_row_height(base_button_height());
    button_height_global = button_height;
    
    if (is_settings_mode) {
        current_options_list = NULL;
        build_item_index = 0;
        s_back_option_added = false;
        menu_build_timer = lv_timer_create(menu_builder_cb, current_settings_category < 0 ? 20 : 15, NULL);
    } else {
        current_options_list = options;
        build_item_index = 0;
        s_back_option_added = false;
        // note: when returning from terminal, submenu states are preserved,
        // so we rebuild the correct submenu (e.g., wifi scanning) automatically
        menu_build_timer = lv_timer_create(menu_builder_cb, 15, NULL);
    }

    /* Status bar already handled by options_view_create */
#ifdef CONFIG_USE_TOUCHSCREEN
    const int BUTTON_AREA_HEIGHT = gui_touch_bar_height();
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, screen_width, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);

    s_opt_touch_tb = gui_touch_bar_create(lv_scr_act());
    touch_bar = s_opt_touch_tb.bar;
    scroll_up_btn = s_opt_touch_tb.up_btn;
    back_btn = s_opt_touch_tb.back_btn;
    scroll_down_btn = s_opt_touch_tb.down_btn;
    if (s_opt_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_opt_touch_tb,
                                    scroll_options_up, NULL,
                                    touch_back_button_cb, NULL,
                                    scroll_options_down, NULL);
    }
#endif
    if (g_freeze_hook_id < 0) {
        g_freeze_hook_id = display_manager_register_freeze_pre_lock(options_menu_freeze_pre_lock);
    }

    // Build the first batch synchronously so short menus appear instantly
    // (like the dedicated BadUSB/NFC views) instead of crawling in from the
    // top; the timer only fills overflow rows, keeping big lists responsive.
    // Done last so touch bar / scroll buttons / final container size exist.
    if (s_vlist_pending) {
        /* Windowed list: bind the row pool now that the list has its final
         * size, then let menu_builder_cb tear down the unused build timer. */
        s_vlist_pending = false;
        vlist_show();
        num_items = options_view_get_item_count(g_options_view);
        selected_item_index = 0;
    }
    menu_builder_cb(NULL);

    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static void load_current_settings_values(void) {
    for (uint8_t theme = 0; theme < THEME_PALETTE_THEME_COUNT; ++theme) {
        theme_options[theme] = theme_palette_get_name(theme);
    }
    for (int i = 0; i < sizeof(settings_items)/sizeof(settings_items[0]); i++) {
        switch (settings_items[i].setting_type) {
            case SETTING_RGB_MODE:
                settings_items[i].current_value = settings_get_rgb_mode(&G_Settings);
                break;
            case SETTING_DISPLAY_TIMEOUT: {
                uint32_t timeout = settings_get_display_timeout(&G_Settings);
                if (timeout == 0)                              settings_items[i].current_value = 7; // Never
                else if (timeout < 7500)                       settings_items[i].current_value = 0; // 5s
                else if (timeout < 12500)                      settings_items[i].current_value = 1; // 10s
                else if (timeout < 22500)                      settings_items[i].current_value = 2; // 15s
                else if (timeout < 45000)                      settings_items[i].current_value = 3; // 30s
                else if (timeout < 90000)                      settings_items[i].current_value = 4; // 60s
                else if (timeout < 210000)                     settings_items[i].current_value = 5; // 2m
                else                                            settings_items[i].current_value = 6; // 5m
                break;
            }
            case SETTING_MENU_THEME:
                settings_items[i].current_value = settings_get_menu_theme(&G_Settings);
                break;
            case SETTING_THIRD_CONTROL:
                settings_items[i].current_value = settings_get_thirds_control_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_TERMINAL_COLOR: {
                uint32_t term_color = settings_get_terminal_text_color(&G_Settings);
                settings_items[i].current_value = 0;
                for (int j = 0; j < settings_items[i].value_count; j++) {
                    if (term_color == textcolor_values[j]) {
                        settings_items[i].current_value = j;
                        break;
                    }
                }
                break;
            }
            case SETTING_TERMINAL_FONT_SIZE:
                settings_items[i].current_value = settings_get_terminal_font_size(&G_Settings);
                break;
            case SETTING_INVERT_COLORS:
                settings_items[i].current_value = settings_get_invert_colors(&G_Settings) ? 1 : 0;
                break;
            case SETTING_SUN_MODE:
                settings_items[i].current_value = settings_get_sun_mode(&G_Settings) ? 1 : 0;
                break;
            case SETTING_LOG_LEVEL:
                settings_items[i].current_value = settings_get_log_level(&G_Settings);
                break;
            case SETTING_FAVORITES_BYPASS:
                settings_items[i].current_value = settings_get_favorites_bypass(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MANAGE_FAVORITES:
            case SETTING_MAIN_MENU_ITEMS:
            case SETTING_APPS_MENU_ITEMS:
            case SETTING_HOP_CHANNELS:
                settings_items[i].current_value = 0;
                break;
            case SETTING_WEB_AUTH:
                settings_items[i].current_value = settings_get_web_auth_enabled(&G_Settings) ? 1 : 0;
                break;
#ifdef CONFIG_HAS_USB_MSC_SD
            case SETTING_USB_MSC:
                settings_items[i].current_value = settings_get_usb_msc_enabled(&G_Settings) ? 1 : 0;
                break;
#endif
            case SETTING_WEBUI_AP_ONLY:
                settings_items[i].current_value = settings_get_webui_restrict_to_ap(&G_Settings) ? 1 : 0;
                break;
            case SETTING_AP_ENABLED:
                settings_items[i].current_value = settings_get_ap_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_POWER_SAVE:
                settings_items[i].current_value = settings_get_power_save_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_ZEBRA_MENUS:
                settings_items[i].current_value = settings_get_zebra_menus_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_BG_SHADE:
                settings_items[i].current_value = settings_get_menu_bg_shade(&G_Settings);
                break;
            case SETTING_THEME_BACKGROUND_EFFECTS:
                settings_items[i].current_value = settings_get_theme_background_effects(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_ROUNDED:
                settings_items[i].current_value = settings_get_menu_rounded(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_ITEM_BORDERS:
                settings_items[i].current_value = settings_get_menu_item_borders(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_CARD_BG:
                settings_items[i].current_value = settings_get_menu_card_bg(&G_Settings) ? 1 : 0;
                break;
            case SETTING_TOUCH_DRAG_SCROLL:
                settings_items[i].current_value = settings_get_touch_drag_scroll(&G_Settings) ? 1 : 0;
                break;
            case SETTING_RELOAD_ASSET_PACK:
                settings_items[i].current_value = asset_pack_get_current_index();
                break;
            case SETTING_NAV_BUTTONS:
                settings_items[i].current_value = settings_get_nav_buttons_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_AUTO_SAVE_SCANS:
                settings_items[i].current_value = settings_get_auto_save_scans(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MENU_LAYOUT:
                settings_items[i].current_value = settings_get_menu_layout(&G_Settings);
                break;
            case SETTING_CAROUSEL_INVERT_DIRECTION:
                settings_items[i].current_value = settings_get_carousel_invert_direction(&G_Settings) ? 1 : 0;
                break;
            case SETTING_MAX_BRIGHTNESS:
                { int bv = (settings_get_max_screen_brightness(&G_Settings) / 10) - 1;
                  settings_items[i].current_value = (bv < 0) ? 0 : bv; }
                break;
            case SETTING_NEOPIXEL_BRIGHTNESS:
                { int nv = (settings_get_neopixel_max_brightness(&G_Settings) / 10) - 1;
                  settings_items[i].current_value = (nv < 0) ? 0 : nv; }
                break;
            case SETTING_EPILEPSY_WARNING:
                settings_items[i].current_value = settings_get_epilepsy_warning_enabled(&G_Settings) ? 1 : 0;
                break;
#ifdef CONFIG_USE_ENCODER
            case SETTING_ENCODER_INVERT:
                settings_items[i].current_value = settings_get_encoder_invert_direction(&G_Settings) ? 1 : 0;
                break;
            case SETTING_ENCODER_LATCH:
                settings_items[i].current_value = settings_get_encoder_legacy_latch(&G_Settings) ? 1 : 0;
                break;
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
            case SETTING_IDLE_ANIMATION:
                settings_items[i].current_value = (int)settings_get_status_idle_animation(&G_Settings);
                break;
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
            case SETTING_IDLE_ANIM_DELAY: {
                uint32_t ms = settings_get_status_idle_timeout_ms(&G_Settings);
                int idx = 0;
                if (ms == 0 || ms == UINT32_MAX) idx = 0;
                else if (ms < 7500) idx = 1; // 5s
                else if (ms < 20000) idx = 2; // 10s
                else idx = 3; // 30s
                settings_items[i].current_value = idx;
                break;
            }
#endif
#if CONFIG_IDF_TARGET_ESP32S3
            case SETTING_USB_HOST_MODE:
                settings_items[i].current_value = usb_keyboard_manager_is_host_mode() ? 1 : 0;
                break;
#endif
            case SETTING_WIGLE_AUTO_UPLOAD:
                settings_items[i].current_value = settings_get_wigle_auto_upload(&G_Settings) ? 1 : 0;
                break;
            case SETTING_WIGLE_DONATE:
                settings_items[i].current_value = settings_get_wigle_donate(&G_Settings) ? 1 : 0;
                break;
#if GHOSTESP_OTA_SUPPORTED
            case SETTING_OTA_CHANNEL:
                settings_items[i].current_value = settings_get_ota_channel(&G_Settings);
                break;
#endif
#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
            case SETTING_MIC_VISUALIZER_MODE:
                settings_items[i].current_value = (int)settings_get_mic_visualizer_mode(&G_Settings);
                break;
            case SETTING_MIC_COLOR_MODE:
                settings_items[i].current_value = (int)settings_get_mic_color_mode(&G_Settings);
                break;
            case SETTING_MIC_SENSITIVITY:
                settings_items[i].current_value = (settings_get_mic_sensitivity(&G_Settings) / 10) - 1;
                if (settings_items[i].current_value < 0) settings_items[i].current_value = 0;
                break;
            case SETTING_MIC_SMOOTHING:
                settings_items[i].current_value = settings_get_mic_smoothing(&G_Settings) / 10;
                break;
            case SETTING_MIC_CONTRAST:
                settings_items[i].current_value = settings_get_mic_contrast(&G_Settings) - 1;
                if (settings_items[i].current_value < 0) settings_items[i].current_value = 0;
                break;
            case SETTING_MIC_MIRROR_MODE:
                settings_items[i].current_value = settings_get_mic_mirror_mode(&G_Settings) ? 1 : 0;
                break;
#endif
            case SETTING_GHOSTLINK_SPLIT_VIEW:
                settings_items[i].current_value = settings_get_ghostlink_split_view(&G_Settings) ? 1 : 0;
                break;
            case SETTING_FONT_SIZE:
                settings_items[i].current_value = settings_get_font_size(&G_Settings);
                break;
            case SETTING_ROW_HEIGHT:
                settings_items[i].current_value = settings_get_row_height(&G_Settings);
                break;
            case SETTING_HIGH_CONTRAST:
                settings_items[i].current_value = settings_get_high_contrast(&G_Settings) ? 1 : 0;
                break;
            case SETTING_REDUCED_MOTION:
                settings_items[i].current_value = settings_get_reduced_motion(&G_Settings) ? 1 : 0;
                break;
            case SETTING_INPUT_REPEAT_SPEED:
                settings_items[i].current_value = settings_get_input_repeat_speed(&G_Settings);
                break;
            case SETTING_LOCKSCREEN_ENABLED:
                settings_items[i].current_value = settings_get_lockscreen_enabled(&G_Settings) ? 1 : 0;
                break;
            case SETTING_LOCKSCREEN_WAKE:
                settings_items[i].current_value = settings_get_lockscreen_wake_lock(&G_Settings) ? 1 : 0;
                break;
            case SETTING_LOCKSCREEN_TIMEOUT: {
                uint16_t tout = settings_get_lockscreen_timeout_sec(&G_Settings);
                if (tout == 0) settings_items[i].current_value = 0;
                else if (tout <= 30) settings_items[i].current_value = 1;
                else if (tout <= 60) settings_items[i].current_value = 2;
                else settings_items[i].current_value = 3;
                break;
            }
            case SETTING_LOCKSCREEN_CHANGE_PIN:
                settings_items[i].current_value = 0;
                break;
            case SETTING_WD_HOP_PRIMARY: {
                uint16_t hp = settings_get_wd_hop_primary_ms(&G_Settings);
                int idx = 2; // default 100ms
                for (int j = 0; j < wd_hop_count; j++) {
                    if (wd_hop_values[j] == hp) { idx = j; break; }
                }
                settings_items[i].current_value = idx;
                break;
            }
            case SETTING_WD_HOP_HELPER: {
                uint16_t hh = settings_get_wd_hop_helper_ms(&G_Settings);
                int idx = 2; // default 100ms
                for (int j = 0; j < wd_hop_count; j++) {
                    if (wd_hop_values[j] == hh) { idx = j; break; }
                }
                settings_items[i].current_value = idx;
                break;
            }
            case SETTING_WD_WEIGHTED_5G:
                settings_items[i].current_value = settings_get_wd_weighted_5g(&G_Settings) ? 1 : 0;
                break;
case SETTING_GPS_BAUD_RATE: {
                uint32_t baud = settings_get_gps_baud_rate(&G_Settings);
                int idx = 0;
                for (int j = 0; j < gps_baud_count; j++) {
                    if (gps_baud_values[j] == baud) { idx = j; break; }
                }
                settings_items[i].current_value = idx;
                break;
            }
            case SETTING_AP_SSID:
            case SETTING_AP_PASSWORD:
            case SETTING_STA_SSID:
            case SETTING_STA_PASSWORD:
                // action items; current_value index unused
                settings_items[i].current_value = 0;
                break;
            case SETTING_IR_TX_PIN:
            case SETTING_IR_RX_PIN:
            case SETTING_DEVICE_IR:
            case SETTING_DEVICE_GPS:
            case SETTING_DEVICE_SUBGHZ:
            case SETTING_DEVICE_NRF24:
            case SETTING_DEVICE_SD:
            case SETTING_DEVICE_RGB:
                // action items; live state is rendered via settings_item_value_text
                settings_items[i].current_value = 0;
                break;
            case SETTING_COUNTRY: {
                uint8_t country = settings_get_wifi_country(&G_Settings);
                settings_items[i].current_value =
                    (country < country_setting_count) ? country : 0;
                break;
            }
            case SETTING_WIFI_AUTO_RECONNECT:
                settings_items[i].current_value = settings_get_wifi_auto_reconnect(&G_Settings) ? 1 : 0;
                break;
            case SETTING_TIMEZONE: {
                const char *cur_tz = settings_get_timezone_str(&G_Settings);
                int idx = 0;
                if (cur_tz && cur_tz[0]) {
                    for (int j = 0; j < timezone_count; j++) {
                        if (strcmp(timezone_values[j], cur_tz) == 0) {
                            idx = j;
                            break;
                        }
                    }
                }
                settings_items[i].current_value = idx;
                break;
            }
            case SETTING_CLOCK_STYLE: {
                uint8_t cs = settings_get_clock_style(&G_Settings);
                settings_items[i].current_value = (cs < 3) ? cs : 0;
                break;
            }
            case SETTING_STATUS_BAR_CLOCK:
                settings_items[i].current_value = settings_get_status_bar_clock(&G_Settings) ? 1 : 0;
                break;
            default:
                settings_items[i].current_value = 0;
                break;
        }
    }
}

#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
static void mic_cal_done_timer_cb(lv_timer_t *timer) {
    lv_timer_del(timer);
    error_popup_destroy();
    error_popup_create("Calibration complete!");
}
#endif

/* Re-styles the persistent touch bar and its buttons from the active theme.
 * The bar lives on lv_scr_act() across menu rebuilds, so palette changes
 * must reach it explicitly or it keeps the previous theme's colors. */
static void settings_touch_bar_restyle(void) {
    gui_touch_bar_refresh_styles(&s_opt_touch_tb);
    if (touch_bar && lv_obj_is_valid(touch_bar)) {
        lv_obj_invalidate(touch_bar);
    }
}

static void apply_setting_change(int setting_index, int new_value) {
    SettingsItem *item = &settings_items[setting_index];
    item->current_value = new_value;

    switch (item->setting_type) {
        case SETTING_RGB_MODE:
            settings_set_rgb_mode(&G_Settings, new_value);
            settings_restart_rgb_effect(); // Immediate visual update
            display_manager_update_status_bar_color();
            break;
        case SETTING_DISPLAY_TIMEOUT: {
            // Indices: 0=5s, 1=10s, 2=15s, 3=30s, 4=60s, 5=2m, 6=5m, 7=Never
            uint32_t timeout_ms;
            switch (new_value) {
                case 0: timeout_ms = 5000;   break;  // 5s
                case 1: timeout_ms = 10000;  break;  // 10s
                case 2: timeout_ms = 15000;  break;  // 15s
                case 3: timeout_ms = 30000;  break;  // 30s
                case 4: timeout_ms = 60000;  break;  // 60s
                case 5: timeout_ms = 120000; break;  // 2m
                case 6: timeout_ms = 300000; break;  // 5m
                default: timeout_ms = 0;      break;  // Never
            }
            settings_set_display_timeout(&G_Settings, timeout_ms);
            break;
        }
        case SETTING_MENU_THEME:
            settings_set_menu_theme(&G_Settings, new_value);
            display_manager_update_status_bar_color();
            gui_screen_apply_theme_background(root);
            settings_touch_bar_restyle();
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_THEME_BACKGROUND_EFFECTS:
            settings_set_theme_background_effects(&G_Settings, new_value == 1);
            gui_screen_apply_theme_background(root);
            settings_touch_bar_restyle();
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
            }
            break;
        case SETTING_THIRD_CONTROL:
            settings_set_thirds_control_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_TERMINAL_COLOR:
            settings_set_terminal_text_color(&G_Settings, textcolor_values[new_value]);
            break;
        case SETTING_TERMINAL_FONT_SIZE:
            settings_set_terminal_font_size(&G_Settings, (uint8_t)new_value);
            break;
        case SETTING_INVERT_COLORS:
            settings_set_invert_colors(&G_Settings, new_value == 1);
            // Invert is read in the flush callback, so we need to force a
            // re-paint of the visible view for the change to take effect.
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
            }
            if (touch_bar && lv_obj_is_valid(touch_bar)) {
                lv_obj_invalidate(touch_bar);
            }
            break;
        case SETTING_SUN_MODE: {
            bool enabling = (new_value == 1);
            settings_set_sun_mode(&G_Settings, enabling);
            if (enabling) {
                settings_persist_setting(SETTING_HIGH_CONTRAST);
                G_Settings.sun_mode_saved_brightness = settings_get_max_screen_brightness(&G_Settings);
                settings_set_max_screen_brightness(&G_Settings, 100);
            } else {
                uint8_t restored = G_Settings.sun_mode_saved_brightness ? G_Settings.sun_mode_saved_brightness : 100;
                settings_set_max_screen_brightness(&G_Settings, restored);
            }
            set_backlight_brightness(100); // scaled by max brightness
            display_manager_update_status_bar_color();
            gui_screen_apply_theme_background(root);
            settings_touch_bar_restyle();
            load_current_settings_values();
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            for (int j = 0; j < settings_items_count; j++) {
                if (settings_items[j].setting_type == SETTING_MAX_BRIGHTNESS) {
                    int bv = (settings_get_max_screen_brightness(&G_Settings) / 10) - 1;
                    settings_items[j].current_value = (bv < 0) ? 0 : bv;
                }
            }
            break;
        }
        case SETTING_LOG_LEVEL:
            settings_set_log_level(&G_Settings, (uint8_t)new_value);
            esp_log_level_set("*", (esp_log_level_t)new_value);
            break;
        case SETTING_FAVORITES_BYPASS:
            settings_set_favorites_bypass(&G_Settings, new_value == 1);
            break;
        case SETTING_MANAGE_FAVORITES:
            display_manager_switch_view(&favorites_manager_view);
            return;
        case SETTING_HOP_CHANNELS:
            display_manager_switch_view(&hop_profile_view);
            return;
        case SETTING_MAIN_MENU_ITEMS:
            menu_editor_open(MENU_PLACE_MAIN);
            return;
        case SETTING_APPS_MENU_ITEMS:
            menu_editor_open(MENU_PLACE_APPS);
            return;
        case SETTING_WEB_AUTH:
            settings_set_web_auth_enabled(&G_Settings, new_value == 1);
            break;
#ifdef CONFIG_HAS_USB_MSC_SD
        case SETTING_USB_MSC:
            settings_set_usb_msc_enabled(&G_Settings, new_value == 1);
            if (new_value == 1 && !usb_msc_is_active()) {
                usb_msc_start_async();
            } else if (new_value == 0 && usb_msc_is_active()) {
                usb_msc_stop();
            }
            break;
#endif
        case SETTING_WEBUI_AP_ONLY:
            settings_set_webui_restrict_to_ap(&G_Settings, new_value == 1);
            break;
        case SETTING_AP_ENABLED:
            settings_set_ap_enabled(&G_Settings, new_value == 1);
            if (new_value == 1) {
                (void)ap_manager_restore_after_attack("ap enable");
            } else {
                ap_manager_stop_services();
            }
            break;
        case SETTING_POWER_SAVE:
            settings_set_power_save_enabled(&G_Settings, new_value == 1);
            apply_power_management_config(new_value == 1);
            break;
        case SETTING_ZEBRA_MENUS:
            settings_set_zebra_menus_enabled(&G_Settings, new_value == 1);
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_MENU_BG_SHADE:
            settings_set_menu_bg_shade(&G_Settings, (uint8_t)new_value);
            display_manager_update_status_bar_color();
            gui_screen_apply_theme_background(root);
            settings_touch_bar_restyle();
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_MENU_ROUNDED:
            settings_set_menu_rounded(&G_Settings, new_value == 1);
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_MENU_ITEM_BORDERS:
            settings_set_menu_item_borders(&G_Settings, new_value == 1);
            break;
        case SETTING_MENU_CARD_BG: {
            settings_set_menu_card_bg(&G_Settings, new_value == 1);
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        }
        case SETTING_TOUCH_DRAG_SCROLL:
            settings_set_touch_drag_scroll(&G_Settings, new_value == 1);
            break;
        case SETTING_RELOAD_ASSET_PACK: {
            ESP_LOGI(TAG, "asset pack setting changed to %d", new_value);
            settings_items[setting_index].current_value = new_value;
            asset_pack_switch_task(new_value);
            return;
        }
        case SETTING_NAV_BUTTONS:
            settings_set_nav_buttons_enabled(&G_Settings, new_value == 1);
            break;
        case SETTING_AUTO_SAVE_SCANS:
            settings_set_auto_save_scans(&G_Settings, new_value == 1);
            break;
        case SETTING_MENU_LAYOUT:
            settings_set_menu_layout(&G_Settings, (uint8_t)new_value);
            // The layout change will take effect on next menu creation
            break;
        case SETTING_CAROUSEL_INVERT_DIRECTION:
            settings_set_carousel_invert_direction(&G_Settings, new_value == 1);
            break;
        #ifdef CONFIG_LV_DISP_BACKLIGHT_PWM
        // This setting is only available if LV_DISP_BACKLIGHT_PWM is enabled
        case SETTING_MAX_BRIGHTNESS:
            settings_set_max_screen_brightness(&G_Settings, (uint8_t)((new_value + 1) * 10));
            set_backlight_brightness(100); // set to 100 since brightness becomes scaled by the max
            break;
        #endif
        case SETTING_NEOPIXEL_BRIGHTNESS:
            settings_set_neopixel_max_brightness(&G_Settings, (uint8_t)((new_value + 1) * 10));
            if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_NORMAL || 
                settings_get_rgb_mode(&G_Settings) == RGB_MODE_STEALTH) {
            } 
            // Restarting the effect applies the new brightness
            settings_restart_rgb_effect(); 
            break;
        case SETTING_EPILEPSY_WARNING:
            settings_set_epilepsy_warning_enabled(&G_Settings, new_value == 1);
            break;
        #ifdef CONFIG_USE_ENCODER
        case SETTING_ENCODER_INVERT:
            settings_set_encoder_invert_direction(&G_Settings, new_value == 1);
            break;
        case SETTING_ENCODER_LATCH:
            settings_set_encoder_legacy_latch(&G_Settings, new_value == 1);
            display_manager_apply_encoder_settings();
            break;
        #endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIMATION:
            settings_set_status_idle_animation(&G_Settings, (IdleAnimation)new_value);
            break;
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIM_DELAY: {
            uint32_t ms = 0;
            switch (new_value) {
                case 0: ms = UINT32_MAX; break; // Never
                case 1: ms = 5000; break;
                case 2: ms = 10000; break;
                case 3: ms = 30000; break;
                default: ms = 5000; break;
            }
            settings_set_status_idle_timeout_ms(&G_Settings, ms);
            break;
        }
#endif
#if CONFIG_IDF_TARGET_ESP32S3
        case SETTING_USB_HOST_MODE:
            usb_keyboard_manager_set_host_mode(new_value == 1);
            return;
#endif
        case SETTING_RUN_SETUP_WIZARD:
            setup_wizard_reset_and_open();
            return;
        case SETTING_I2C_SCAN:
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            io_manager_scan_i2c();
            return;
        case SETTING_GL_BENCH:
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("glbench");
            return;
        case SETTING_EXPORT_SETTINGS_SD: {
            esp_err_t err = settings_backup_export_to_sd();
            if (err == ESP_OK) {
                error_popup_create("Settings exported to SD\n\nghostesp/settings_backup.json");
            } else if (err == ESP_ERR_NOT_FOUND) {
                error_popup_create("SD card not available");
            } else {
                error_popup_create("Export failed\n\nCheck SD and ghostesp folder");
            }
            return;
        }
        case SETTING_IMPORT_SETTINGS_SD: {
            popup_confirm_show(&settings_confirm_popup, lv_layer_top(), "Import Settings?",
                               "This will overwrite current settings from the SD backup.",
                               "Import", "Cancel", settings_confirm_import_cb, NULL);
            return;
        }
        case SETTING_FACTORY_RESET:
            popup_confirm_show(&settings_confirm_popup, lv_layer_top(), "Factory Reset?",
                               "This erases saved settings and restarts the device.",
                               "Reset", "Cancel", settings_confirm_factory_reset_cb, NULL);
            return;
        case SETTING_WIGLE_AUTO_UPLOAD:
            settings_set_wigle_auto_upload(&G_Settings, new_value == 1);
            break;
        case SETTING_WIGLE_DONATE:
            settings_set_wigle_donate(&G_Settings, new_value == 1);
            break;
#if GHOSTESP_OTA_SUPPORTED
        case SETTING_OTA_CHANNEL:
            settings_set_ota_channel(&G_Settings, (uint8_t)new_value);
            settings_persist_setting(SETTING_OTA_CHANNEL);
            break;
        case SETTING_OTA_CHECK_NOW: {
            bool started = false;
            if (ota_manager_is_supported()) {
                started = (ota_manager_check_now() == ESP_OK);
            } else if (self_ota_manager_is_supported()) {
                started = (self_ota_manager_check_now() == ESP_OK);
            }
            if (started) {
                ota_status_start_overlay(OTA_UI_MODE_CHECK, "Checking device...", "Contacting update server");
            } else {
                ota_status_show_result("Device Update", "Device update check is not available on this board.");
            }
            return;
        }
        case SETTING_OTA_INSTALL_UPDATE: {
            // This installs THIS board's own firmware only -- it must never
            // touch the peer as a side effect. Updating the peer is a
            // separate, explicit action (SETTING_OTA_UPDATE_PEER) that the
            // user has to choose on its own.
            if (!ota_manager_is_supported() && !self_ota_manager_is_supported()) {
                ota_status_show_result("Manual Flash Required", "This board reflashes manually. See the release notes.");
                return;
            }
            ota_show_device_install_confirm();
            return;
        }
        case SETTING_OTA_CHECK_PEER: {
            if (!peer_ota_manager_is_supported()) {
                ota_status_show_result("Peer Update", "No GhostLink peer configured for this board.");
                return;
            }
            esp_err_t err = peer_ota_manager_check_now();
            if (err == ESP_OK) {
                ota_status_start_overlay(OTA_UI_MODE_PEER_CHECK, "Checking peer...", "Contacting update server");
            } else {
                ota_status_show_result("Peer Update", "GhostLink not connected.");
            }
            return;
        }
        case SETTING_OTA_UPDATE_PEER: {
            if (!peer_ota_manager_is_supported()) {
                ota_status_show_result("Peer Update", "No GhostLink peer configured for this board.");
                return;
            }
            esp_err_t err = peer_ota_manager_start_update();
            if (err == ESP_ERR_INVALID_STATE) {
                ota_status_show_result("Peer Update", "GhostLink not connected.");
            } else if (err == ESP_OK) {
                ota_status_start_overlay(OTA_UI_MODE_PEER_INSTALL, "Updating peer...", "Streaming firmware over GhostLink");
            } else {
                ota_status_show_result("Peer Update", "Failed to start peer update.");
            }
            return;
        }
        case SETTING_OTA_INSTALL_FROM_SD: {
            ota_show_sd_install_confirm();
            return;
        }
#endif
        case SETTING_LOAD_CONFIG: {
            // Load config from SD card
            esp_err_t config_err = config_manager_load_from_sd();
            
            if (config_err == ESP_OK) {
                // Build success message showing what was loaded
                char msg[256];
                int len = snprintf(msg, sizeof(msg), "Config Loaded!\n\n");
                
                const char *ssid = settings_get_sta_ssid(&G_Settings);
                if (ssid && ssid[0]) {
                    len += snprintf(msg + len, sizeof(msg) - len, "WiFi: %s\n", ssid);
                }
                
                if (G_Settings.wigle_api_key[0]) {
                    len += snprintf(msg + len, sizeof(msg) - len, "Wigle: Set\n");
                }
                
                len += snprintf(msg + len, sizeof(msg) - len, "Upload: %s\n",
                    settings_get_wigle_auto_upload(&G_Settings) ? "On" : "Off");
                len += snprintf(msg + len, sizeof(msg) - len, "Donate: %s\n",
                    settings_get_wigle_donate(&G_Settings) ? "On" : "Off");
                
                // Reconfigure WiFi
                wifi_manager_configure_sta_from_settings();
                
                error_popup_create(msg);
            } else if (config_err == ESP_ERR_NOT_FOUND) {
                error_popup_create("Config not found\n\nPlace config.cfg at:\n/ghostesp/config.cfg");
            } else {
                error_popup_create("Failed to load config");
            }
            return;
        }
        case SETTING_WIGLE_TEST_API: {
            if (wigle_is_test_in_progress()) {
                return;
            }
            if (!is_wifi_sta_connected()) {
                error_popup_create("Connect to WiFi first");
                return;
            }
            const char *api_key = wigle_get_api_key();
            if (!api_key || api_key[0] == '\0') {
                error_popup_create("No API key set\nUse CLI: wigle API <encoded|name:token>");
                return;
            }
            error_popup_create("Testing API key...");
            wigle_set_test_callback(wigle_test_result_cb);
            esp_err_t err = wigle_test_api_key();
            if (err != ESP_OK) {
                wigle_set_test_callback(NULL);
                error_popup_create("Failed to start test");
                return;
            }
            return;
        }
        case SETTING_WIGLE_HELP: {
            if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
                lvgl_obj_del_safe(&wigle_help_popup);
                return;
            }
            
            uint8_t theme = settings_get_menu_theme(&G_Settings);
            lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
            lv_color_t surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
            lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
            lv_color_t muted = lv_color_hex(theme_palette_get_text_muted(theme));
            int popup_w = LV_HOR_RES - 20;
            int popup_h = LV_VER_RES - 40;
            wigle_help_popup = popup_create_container(lv_layer_top(), popup_w, popup_h, true);
            lv_obj_set_style_bg_color(wigle_help_popup, surface, 0);
            lv_obj_add_flag(wigle_help_popup, LV_OBJ_FLAG_CLICKABLE);
            
            lv_obj_t *title = lv_label_create(wigle_help_popup);
            lv_label_set_text(title, "WiGLE Setup Help");
            lv_obj_set_style_text_color(title, text, 0);
            lv_obj_set_style_text_font(title, accessibility_get_font_body(), 0);
            lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);
            
            lv_obj_t *help_scroll = popup_create_scroll_area(wigle_help_popup, popup_w - 16, popup_h - 50, LV_ALIGN_TOP_MID, 0, 25);
            
            lv_obj_t *help_label = lv_label_create(help_scroll);
            lv_label_set_long_mode(help_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(help_label, popup_w - 20);
            lv_obj_set_style_text_color(help_label, muted, 0);
            
            const char *help_text = 
                "1. Create free account at wigle.net\n"
                "2. Account > API section\n"
                "3. Copy Encoded for Use token\n"
                "   (or API Name:Token)\n\n"
                "CLI: wigle API <encoded|name:token>\n"
                "Ex: wigle API QUJDMTIzOkRFRjQ1Ng==\n"
                "Ex: wigle API ABC123:DEF456\n\n"
                "Auto Upload: Upload CSV when WiFi connects\n"
                "Donate: Share scans publicly (recommended)\n\n"
                "Needs: GPS, SD card, WiFi, CSV files in /mnt/ghostesp/gps/";
            
            lv_label_set_text(help_label, help_text);
            lv_obj_set_style_text_font(help_label, accessibility_get_font_small(), 0);
            
            lv_obj_t *close_btn = lv_btn_create(wigle_help_popup);
            gui_apply_pressed_style(close_btn);
            wigle_help_close_btn = close_btn;
            lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(close_btn, 80, 30);
            lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, -5);
            lv_obj_set_style_bg_color(close_btn, surface_alt, 0);
            lv_obj_add_event_cb(close_btn, wigle_help_close_cb, LV_EVENT_CLICKED, NULL);
            
            lv_obj_t *btn_label = lv_label_create(close_btn);
            lv_label_set_text(btn_label, "Close");
            lv_obj_center(btn_label);
            lv_obj_set_style_text_color(btn_label, text, 0);
            
            return;
        }
        case SETTING_WIGLE_MANUAL_UPLOAD: {
            wigle_csv_page_offset = 0;
            wigle_csv_browser_active = true;
            SelectedMenuType = OT_WigleManualUpload;
            is_settings_mode = false;
            rebuild_current_menu();
            return;
        }
        case SETTING_WIGLE_STATS: {
            if (wigle_is_stats_in_progress()) {
                wigle_stats_popup_open();
                wigle_set_stats_callback(wigle_stats_result_cb);
                if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                    lv_label_set_text(wigle_stats_body_label, "Stats request already running...\nPress Close to exit.");
                }
                return;
            }
            wigle_stats_popup_open();
            if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                lv_label_set_text(wigle_stats_body_label, "Loading WiGLE stats...");
            }
            wigle_set_stats_callback(wigle_stats_result_cb);
            esp_err_t err = wigle_get_stats_async();
            if (err != ESP_OK) {
                wigle_set_stats_callback(NULL);
                if (wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
                    lv_label_set_text(wigle_stats_body_label, "Failed to start stats request");
                }
            }
            return;
        }
#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
        case SETTING_MIC_VISUALIZER_MODE:
            settings_set_mic_visualizer_mode(&G_Settings, (MicVisualizerMode)new_value);
            break;
        case SETTING_MIC_COLOR_MODE:
            settings_set_mic_color_mode(&G_Settings, (MicColorMode)new_value);
            break;
        case SETTING_MIC_SENSITIVITY:
            settings_set_mic_sensitivity(&G_Settings, (new_value + 1) * 10);
            break;
        case SETTING_MIC_SMOOTHING:
            settings_set_mic_smoothing(&G_Settings, new_value * 10);
            break;
        case SETTING_MIC_CONTRAST:
            settings_set_mic_contrast(&G_Settings, new_value + 1);
            break;
        case SETTING_MIC_MIRROR_MODE:
            settings_set_mic_mirror_mode(&G_Settings, new_value == 1);
            break;
#if defined(CONFIG_HAS_MIC) || defined(CONFIG_ENABLE_MIC_RGB_VISUALIZER)
        case SETTING_MIC_CALIBRATE:
#ifdef CONFIG_HAS_MIC
            settings_set_mic_calibrate(&G_Settings, true);
#else
            if (!esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to MIC device");
                return;
            }
            simulateCommand("commsend mic_cal");
#endif
            error_popup_create_persistent("Calibrating mic...\n\nPlease stay quiet!");
            lv_timer_create(mic_cal_done_timer_cb, 8500, NULL);
            return;
#endif
#endif
        case SETTING_GHOSTLINK_SPLIT_VIEW:
            settings_set_ghostlink_split_view(&G_Settings, new_value == 1);
            break;
        case SETTING_FONT_SIZE:
            settings_set_font_size(&G_Settings, (uint8_t)new_value);
            break;
        case SETTING_ROW_HEIGHT:
            settings_set_row_height(&G_Settings, (uint8_t)new_value);
            // Rows in a settings category all use button_height_global, so the
            // visible list can be re-heighted in place instead of rebuilt.
            button_height_global = options_view_scale_row_height(base_button_height());
            if (g_options_view) {
                options_view_set_item_height(g_options_view, button_height_global);
                options_view_refresh_selected_item(g_options_view);
            }
            break;
        case SETTING_HIGH_CONTRAST:
            settings_set_high_contrast(&G_Settings, new_value == 1);
            if (new_value == 1) settings_persist_setting(SETTING_SUN_MODE);
            display_manager_update_status_bar_color();
            gui_screen_apply_theme_background(root);
            settings_touch_bar_restyle();
            load_current_settings_values();
            if (g_options_view) {
                options_view_refresh_styles(g_options_view);
                update_settings_arrows_visibility();
            }
            break;
        case SETTING_REDUCED_MOTION:
            settings_set_reduced_motion(&G_Settings, new_value == 1);
            break;
        case SETTING_INPUT_REPEAT_SPEED:
            settings_set_input_repeat_speed(&G_Settings, (uint8_t)new_value);
            break;
        case SETTING_LOCKSCREEN_ENABLED:
            settings_set_lockscreen_enabled(&G_Settings, new_value == 1);
            if (new_value == 1) {
                settings_set_lockscreen_type(&G_Settings, 1);
                settings_persist_setting(SETTING_LOCKSCREEN_TYPE);
            }
            break;
        case SETTING_LOCKSCREEN_WAKE:
            settings_set_lockscreen_wake_lock(&G_Settings, new_value == 1);
            break;
        case SETTING_LOCKSCREEN_TIMEOUT: {
            uint16_t tout_sec = 0;
            switch (new_value) {
                case 0: tout_sec = 0; break;
                case 1: tout_sec = 30; break;
                case 2: tout_sec = 60; break;
                case 3: tout_sec = 300; break;
                default: tout_sec = 0; break;
            }
            settings_set_lockscreen_timeout_sec(&G_Settings, tout_sec);
            break;
        }
        case SETTING_LOCKSCREEN_CHANGE_PIN: {
            lockscreen_enter_setup();
            display_manager_switch_view(&lockscreen_view);
            return;
        }
        case SETTING_WD_HOP_PRIMARY:
            if (new_value >= 0 && new_value < wd_hop_count) {
                settings_set_wd_hop_primary_ms(&G_Settings, wd_hop_values[new_value]);
            }
            break;
        case SETTING_WD_HOP_HELPER:
            if (new_value >= 0 && new_value < wd_hop_count) {
                settings_set_wd_hop_helper_ms(&G_Settings, wd_hop_values[new_value]);
            }
            break;
        case SETTING_WD_WEIGHTED_5G:
            settings_set_wd_weighted_5g(&G_Settings, new_value == 1);
            break;
case SETTING_GPS_BAUD_RATE:
            if (new_value >= 0 && new_value < gps_baud_count) {
                settings_set_gps_baud_rate(&G_Settings, gps_baud_values[new_value]);
            }
            break;
        case SETTING_AP_SSID: {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("AP SSID (max 32 chars)");
            keyboard_view_set_initial_text(settings_get_ap_ssid(&G_Settings));
            keyboard_view_set_start_caps(true);
            keyboard_view_set_submit_callback(ap_ssid_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_IR_TX_PIN: {
            char initial[16];
            snprintf(initial, sizeof(initial), "%d", (int)settings_get_ir_tx_pin(&G_Settings));
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("IR TX pin (-1 = board default)");
            keyboard_view_set_initial_text(initial);
            keyboard_view_set_start_caps(false);
            keyboard_view_set_submit_callback(ir_tx_pin_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_IR_RX_PIN: {
            char initial[16];
            snprintf(initial, sizeof(initial), "%d", (int)settings_get_ir_rx_pin(&G_Settings));
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("IR RX pin (-1 = board default)");
            keyboard_view_set_initial_text(initial);
            keyboard_view_set_start_caps(false);
            keyboard_view_set_submit_callback(ir_rx_pin_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_DEVICE_IR:
        case SETTING_DEVICE_GPS:
        case SETTING_DEVICE_SUBGHZ:
        case SETTING_DEVICE_NRF24:
        case SETTING_DEVICE_SD:
        case SETTING_DEVICE_RGB:
            // Read-only status rows: refresh the live state and do not persist.
            settings_refresh_row_label(setting_index);
            return;
        case SETTING_AP_PASSWORD: {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("AP Password (8-63 chars, empty=open)");
            keyboard_view_set_initial_text(settings_get_ap_password(&G_Settings));
            keyboard_view_set_start_caps(false);
            keyboard_view_set_submit_callback(ap_password_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_STA_SSID: {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("Station SSID to connect to");
            keyboard_view_set_initial_text(settings_get_sta_ssid(&G_Settings));
            keyboard_view_set_start_caps(true);
            keyboard_view_set_submit_callback(sta_ssid_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_STA_PASSWORD: {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_placeholder("Station Password");
            keyboard_view_set_initial_text(settings_get_sta_password(&G_Settings));
            keyboard_view_set_start_caps(false);
            keyboard_view_set_submit_callback(sta_password_kb_cb);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETTING_WIFI_AUTO_RECONNECT:
            settings_set_wifi_auto_reconnect(&G_Settings, new_value == 1);
            break;
        case SETTING_TIMEZONE:
            if (new_value >= 0 && new_value < timezone_count) {
                settings_set_timezone_str(&G_Settings, timezone_values[new_value]);
                setenv("TZ", timezone_values[new_value], 1);
                tzset();
            }
            break;
        case SETTING_CLOCK_STYLE:
            settings_set_clock_style(&G_Settings, (uint8_t)new_value);
            break;
        case SETTING_STATUS_BAR_CLOCK:
            // Picked up live by the display manager's status timer.
            settings_set_status_bar_clock(&G_Settings, new_value == 1);
            break;
        case SETTING_COUNTRY:
            if (new_value >= 0 && new_value < country_setting_count) {
                settings_set_wifi_country(&G_Settings, (uint8_t)new_value);
                // Apply immediately so scans/attacks pick it up without a reboot;
                // main.c re-applies the persisted index on boot.
                esp_wifi_set_country_code(country_setting_codes[new_value], true);
            }
            break;
    }

    // Save only the changed setting to NVS (Granular Save)
    settings_persist_setting((SettingsType)item->setting_type);
}

static bool settings_select_overlay_is_open(void) {
    return gui_select_overlay_is_open(settings_select_overlay);
}

static void settings_refresh_row_label(int setting_index) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;
    if (setting_index < 0 || setting_index >= settings_items_count) return;

    SettingsItem *item = &settings_items[setting_index];
    uint32_t child_cnt = lv_obj_get_child_cnt(menu_container);
    for (uint32_t row_idx = 0; row_idx < child_cnt; row_idx++) {
        lv_obj_t *row = lv_obj_get_child(menu_container, (int32_t)row_idx);
        if (!row || !lv_obj_is_valid(row)) continue;
        if ((int)(intptr_t)lv_obj_get_user_data(row) != setting_index) continue;

        lv_obj_t *label = NULL;
        uint32_t row_child_cnt = lv_obj_get_child_cnt(row);
        for (uint32_t i = 0; i < row_child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(row, (int32_t)i);
            if (!child) continue;
            if (lv_obj_get_user_data(child) == (void *)1) {
                label = child;
                break;
            }
        }
        if (!label && row_child_cnt > 0) {
            label = lv_obj_get_child(row, 0);
        }
        if (label) {
            char buf[128];
            snprintf(buf, sizeof(buf), "%s: %s", item->label, settings_item_value_text(item));
            lv_label_set_text(label, buf);
        }
        return;
    }
}

static void settings_select_apply_value(int option_index, void *user_data) {
    (void)user_data;
    if (settings_select_setting_index < 0 || settings_select_setting_index >= settings_items_count) {
        settings_select_close();
        return;
    }

    SettingsItem *item = &settings_items[settings_select_setting_index];
    if (option_index < 0 || option_index >= item->value_count) {
        settings_select_close();
        return;
    }

    int setting_index = settings_select_setting_index;
    settings_select_close();
    apply_setting_change(setting_index, option_index);
    settings_refresh_row_label(setting_index);
    update_settings_arrows_visibility();
}

static void settings_select_dismiss(void *user_data) {
    (void)user_data;
    settings_select_close();
}

static void settings_select_close(void) {
    gui_select_overlay_destroy(&settings_select_overlay);
    settings_select_setting_index = -1;
}

/* Paints the 4-chip palette preview directly onto the row button during
 * DRAW_MAIN. Widget-based chips (~68 extra objects per overlay open)
 * exhausted LVGL's heap on RAM-constrained boards and crashed creation. */
static void settings_theme_swatch_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t *row = lv_event_get_target(e);
    int option_index = (int)(intptr_t)lv_event_get_user_data(e);
    if (!row || !lv_obj_is_valid(row)) return;
    if (option_index < 0 || option_index >= THEME_PALETTE_THEME_COUNT) return;
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) return;

    const theme_descriptor_t *theme = theme_palette_get_descriptor((uint8_t)option_index);
    const uint32_t colors[4] = {theme->background, theme->surface, theme->accent, theme->text};

    lv_area_t row_area;
    lv_obj_get_coords(row, &row_area);
    lv_coord_t chip_w = 7;
    lv_coord_t chip_h = LV_MIN(lv_area_get_height(&row_area) - 8, 14);
    if (chip_h < 6) return;
    lv_coord_t y1 = lv_area_get_height(&row_area) / 2 + row_area.y1 - chip_h / 2;
    lv_coord_t x2 = row_area.x2 - 6;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 1;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_color = lv_color_hex(theme->border);
    dsc.radius = 2;

    for (int i = 3; i >= 0; --i) {
        dsc.bg_color = lv_color_hex(colors[i]);
        lv_area_t chip = {(lv_coord_t)(x2 - chip_w), y1, x2, (lv_coord_t)(y1 + chip_h - 1)};
        lv_draw_rect(draw_ctx, &dsc, &chip);
        x2 -= chip_w;
    }
}

static void settings_theme_decorate_row(lv_obj_t *row, int option_index, void *user_data) {
    (void)user_data;
    if (!row || option_index < 0 || option_index >= THEME_PALETTE_THEME_COUNT) return;
    lv_obj_add_event_cb(row, settings_theme_swatch_draw_cb, LV_EVENT_DRAW_MAIN,
                        (void *)(intptr_t)option_index);
    lv_obj_t *label = lv_obj_get_child(row, 0);
    /* Row width isn't laid out yet, but the parent list is sized explicitly. */
    lv_obj_t *list = lv_obj_get_parent(row);
    lv_coord_t list_w = list ? lv_obj_get_width(list) : 0;
    if (label && list_w > 60) lv_obj_set_width(label, list_w - 44);
}

static void settings_select_open(int setting_index) {
    if (setting_index < 0 || setting_index >= settings_items_count) return;
    SettingsItem *item = &settings_items[setting_index];
    if (item->widget != SETTING_WIDGET_VALUE_CYCLE || item->value_count <= 1 || !item->value_options) return;

    settings_select_close();

    settings_select_setting_index = setting_index;

    int row_h = (button_height_global > 0) ? button_height_global - 8 : 40;
    if (row_h < 30) row_h = 30;
#if GUI_LARGE_SCREEN
    row_h = 64;
#endif

    lv_obj_t *row = NULL;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        uint32_t child_cnt = lv_obj_get_child_cnt(menu_container);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *candidate = lv_obj_get_child(menu_container, (int32_t)i);
            if (candidate && (int)(intptr_t)lv_obj_get_user_data(candidate) == setting_index) {
                row = candidate;
                break;
            }
        }
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);

    int bottom_reserved = 8;
#ifdef CONFIG_USE_TOUCHSCREEN
    bottom_reserved += gui_touch_bar_height();
#endif
#if GUI_LARGE_TOUCH_UI
    bottom_reserved = GUI_HOME_SAFE_H + 16;
#endif
    gui_select_overlay_config_t cfg = {
        .parent = lv_layer_top(),
        .anchor = row,
        .options = item->value_options,
        .option_count = item->value_count,
        .selected_index = settings_item_clamped_value(item),
        .row_height = row_h,
        .max_visible_rows = (LV_VER_RES <= 200) ? 4 : 5,
        .top_reserved = GUI_STATUS_BAR_H + 4,
        .bottom_reserved = bottom_reserved,
#if GUI_LARGE_SCREEN
        .min_width = LV_MIN(360, LV_HOR_RES - 64),
        .max_width = LV_MIN(520, LV_HOR_RES - 64),
#else
        .min_width = 90,
        .max_width = 230,
#endif
        .surface_color = lv_color_hex(theme_palette_get_surface_alt(theme)),
        .text_color = lv_color_hex(theme_palette_get_text(theme)),
        .muted_text_color = lv_color_hex(theme_palette_get_text_muted(theme)),
        .accent_color = lv_color_hex(theme_palette_get_accent(theme)),
        .font = (row_h <= 34) ? accessibility_get_font_small() : accessibility_get_font_body(),
        .on_select = settings_select_apply_value,
        .on_dismiss = settings_select_dismiss,
        .decorate_row = item->setting_type == SETTING_MENU_THEME ? settings_theme_decorate_row : NULL,
        .user_data = NULL,
    };
    settings_select_overlay = gui_select_overlay_create(&cfg);
    if (!settings_select_overlay) {
        settings_select_setting_index = -1;
    }
}

static bool settings_select_handle_input(InputEvent *event) {
    if (!settings_select_overlay_is_open()) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) {
        return gui_select_overlay_handle_touch(settings_select_overlay, &event->data.touch_data);
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        settings_select_close();
        return true;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2 || button == 0) {
            gui_select_overlay_move(settings_select_overlay, -1);
        } else if (button == 4 || button == 3) {
            gui_select_overlay_move(settings_select_overlay, 1);
        } else if (button == 1) {
            gui_select_overlay_select_current(settings_select_overlay);
        }
        return true;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == LV_KEY_UP || key == 'k' || key == ';' || key == ',' || key == 'h' || key == 44 || key == 59) {
            gui_select_overlay_move(settings_select_overlay, -1);
        } else if (key == LV_KEY_DOWN || key == 'j' || key == '.' || key == '/' || key == 'l' || key == 46 || key == 47) {
            gui_select_overlay_move(settings_select_overlay, 1);
        } else if (key == LV_KEY_ENTER || key == 13) {
            gui_select_overlay_select_current(settings_select_overlay);
        } else if (key == LV_KEY_ESC || key == 29 || key == '`') {
            settings_select_close();
        }
        return true;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            gui_select_overlay_select_current(settings_select_overlay);
        } else if (event->data.encoder.direction < 0) {
            gui_select_overlay_move(settings_select_overlay, -1);
        } else if (event->data.encoder.direction > 0) {
            gui_select_overlay_move(settings_select_overlay, 1);
        }
        return true;
    }

    return true;
}

static bool settings_confirm_handle_input(InputEvent *event) {
    popup_confirm_t **active_popup = NULL;
    popup_confirm_t *active = NULL;
    if (popup_confirm_is_open(settings_confirm_popup)) {
        active_popup = &settings_confirm_popup;
        active = settings_confirm_popup;
    }
#if GHOSTESP_OTA_SUPPORTED
    else if (popup_confirm_is_open(ota_result_popup)) {
        active_popup = &ota_result_popup;
        active = ota_result_popup;
    }
#endif
    if (!active_popup) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) return popup_confirm_handle_touch(active_popup, &event->data.touch_data);
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        popup_confirm_cancel(active_popup);
        return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 1) popup_confirm_select(active_popup);
        else if (button == 0) popup_confirm_set_selected(active, 0);
        else if (button == 3) popup_confirm_set_selected(active, 1);
        else if (button == 2 || button == 4) popup_confirm_move(active, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == LV_KEY_ENTER || key == 13) popup_confirm_select(active_popup);
        else if (key == LV_KEY_ESC || key == 29 || key == '`') popup_confirm_cancel(active_popup);
        else if (key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN ||
                 key == 'h' || key == 'l' || key == 'k' || key == 'j' || key == ',' || key == '.' || key == ';' || key == '/') {
            popup_confirm_move(active, 1);
        }
        return true;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) popup_confirm_select(active_popup);
        else if (event->data.encoder.direction != 0) popup_confirm_move(active, event->data.encoder.direction);
        return true;
    }

    return true;
}

static void settings_confirm_import_cb(void *user_data) {
    (void)user_data;
    esp_err_t err = settings_backup_import_from_sd();
    if (err == ESP_OK) {
        settings_backup_apply_runtime_after_import();
        error_popup_create("Settings imported\n\nSaved to NVS.\nReboot for full effect.");
    } else if (err == ESP_ERR_NOT_FOUND) {
        error_popup_create("Backup not found\n\nghostesp/settings_backup.json");
    } else if (err == ESP_ERR_INVALID_VERSION) {
        error_popup_create("Invalid backup file\n\nWrong format or version");
    } else if (err == ESP_ERR_INVALID_SIZE) {
        error_popup_create("Backup file invalid\n\nSize out of range");
    } else {
        error_popup_create("Import failed");
    }
}

static void settings_confirm_factory_reset_cb(void *user_data) {
    (void)user_data;
    nvs_flash_erase();
    esp_restart();
}

static void change_current_row(bool increment)
{
    if (!menu_container) return;
    /* Only valid when we are IN a settings submenu (not at category level) */
    if (current_settings_category < 0) return;

    lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
    if (!sel) return;
    void *udata = lv_obj_get_user_data(sel);
    if (udata == (void *)"__BACK_OPTION__") {
        // back isn't a setting
        return;
    }
    int setting_idx = (int)(intptr_t)udata;
#ifdef CONFIG_USE_IO_EXPANDER
    if (setting_idx == IO_BTN_EDIT_P10 || setting_idx == IO_BTN_EDIT_P11 || setting_idx == IO_BTN_EDIT_P12) {
        io_btn_being_edited = (setting_idx == IO_BTN_EDIT_P10) ? 0 : (setting_idx == IO_BTN_EDIT_P11) ? 1 : 2;
        SelectedMenuType = OT_IOButtonPresets;
        is_settings_mode = false;
        rebuild_current_menu();
        return;
    }
#endif
    change_setting_value(setting_idx, increment);
}

static void change_setting_value(int setting_index, bool increment) {
#ifdef CONFIG_USE_IO_EXPANDER
    if (setting_index == IO_BTN_EDIT_P10 || setting_index == IO_BTN_EDIT_P11 || setting_index == IO_BTN_EDIT_P12) {
        io_btn_being_edited = (setting_index == IO_BTN_EDIT_P10) ? 0 : (setting_index == IO_BTN_EDIT_P11) ? 1 : 2;
        SelectedMenuType = OT_IOButtonPresets;
        is_settings_mode = false;
        rebuild_current_menu();
        return;
    }
#endif
    SettingsItem *item = &settings_items[setting_index];
    if (item->value_count <= 0) return;

    int new_value = item->current_value;

    if (item->widget == SETTING_WIDGET_TOGGLE) {
        // Flip 0 <-> 1 regardless of the `increment` argument.
        new_value = (item->current_value == 0) ? 1 : 0;
    } else if (item->value_count > 1) {
        settings_select_open(setting_index);
        return;
    } else if (increment) {
        new_value = (new_value + 1) % item->value_count;
    } else {
        new_value = (new_value + item->value_count - 1) % item->value_count;
    }

    apply_setting_change(setting_index, new_value);

    if (!menu_container) return;

    lv_obj_t *current_item = lv_obj_get_child(menu_container, selected_item_index);
    if (!current_item) return;

    if (item->widget == SETTING_WIDGET_TOGGLE) {
        // Update the toggle widget's visual state to match the new value.
        lv_obj_t *toggle = find_row_toggle(current_item);
        if (toggle) {
            ios_toggle_set_value(toggle, new_value == 1, true);
        }
        return;
    }

    settings_refresh_row_label(setting_index);
}

static void settings_activate_row(int row_index, bool increment) {
    if (!menu_container || !lv_obj_is_valid(menu_container)) return;

    if (current_settings_root < 0) {
        switch_to_settings_root(row_index);
        return;
    }

    if (current_settings_category < 0) {
        switch_to_settings_category(row_index);
        return;
    }

    lv_obj_t *sel = lv_obj_get_child(menu_container, row_index);
    if (!sel) return;

    void *udata = lv_obj_get_user_data(sel);
    if (udata == (void *)"__BACK_OPTION__") {
        back_event_cb(NULL);
        return;
    }

    int setting_idx = (int)(intptr_t)udata;
    change_setting_value(setting_idx, increment);
}

static void select_option_item(int index) {
    ESP_LOGD(TAG, "select_option_item called with index: %d, num_items: %d\n", index, num_items);
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;
    selected_item_index = index;
    if (g_options_view) {
        options_view_set_selected(g_options_view, selected_item_index);
    }
    
    // Update arrow visibility based on new selection
    update_settings_arrows_visibility();
}

void handle_hardware_button_press_options(InputEvent *event) {
    if (esp_timer_get_time() < option_input_blocked_until_us) {
        if (event && event->type == INPUT_TYPE_TOUCH) opt_touch_reset();
        return;
    }

    if (settings_confirm_handle_input(event)) {
        return;
    }

#if GHOSTESP_OTA_SUPPORTED
    if (ota_status_handle_cancel_input(event)) {
        return;
    }
#endif

    bool arp_overlay_active = arp_scan_status && scan_status_is_active(arp_scan_status);
    bool mdns_overlay_active = mdns_scan_status && scan_status_is_active(mdns_scan_status);
    bool enum_overlay_active = enum_scan_status && scan_status_is_active(enum_scan_status);
    if (arp_overlay_active || mdns_overlay_active || enum_overlay_active) {
        if (event && event->type == INPUT_TYPE_TOUCH) {
            /* This display's raw touch path is authoritative. Cancel on press,
             * then swallow the release so it cannot activate the menu below. */
            if (event->data.touch_data.state == LV_INDEV_STATE_PR) {
                if (arp_overlay_active) {
                    arp_scan_cancel_cb();
                } else if (mdns_overlay_active) {
                    mdns_scan_cancel_cb();
                } else {
                    enum_scan_cancel_cb();
                }
            }
            opt_touch_reset();
            return;
        }
        if (should_stop_station_scan_on_input(event)) {
            if (arp_overlay_active) {
                arp_scan_cancel_cb();
            } else if (mdns_overlay_active) {
                mdns_scan_cancel_cb();
            } else {
                enum_scan_cancel_cb();
            }
        }
        return;
    }

    // Close wigle help popup on exit button or joystick back
    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON || 
            (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 0)) {
            wigle_help_close_cb(NULL);
            return;
        }
    }
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            wigle_manual_popup_close_cb(NULL);
            return;
        }
    }
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            wigle_stats_popup_close_cb(NULL);
            return;
        }
    }

    if (settings_select_handle_input(event)) {
        return;
    }

    /* Live RSSI tracking overlay. Back-like physical inputs leave; other
     * non-touch inputs are swallowed so they never leak to the menu underneath.
     * Touch is left to fall through to the shared options touch pipeline (same
     * as the detail-view overlay) so move/scroll samples still reach LVGL like
     * other views; ring taps are swallowed and the Back button handled there. */
    bool track_overlay_active = track_meter && rssi_meter_is_active(track_meter);
    if (track_overlay_active) {
        if (event->type != INPUT_TYPE_TOUCH) {
            if (track_exit_requested(event)) {
                stop_track_flow();
            }
            return;
        }
        /* TOUCH events must not be treated as scan-stop inputs while the
         * RSSI meter is up; fall through to the dedicated touch handler. */
    }

    bool station_scan_overlay_active = station_scan_is_active() ||
                                       (sta_scan_poll_timer != NULL) ||
                                       (sta_scan_status != NULL);
    if (!track_overlay_active && station_scan_overlay_active && should_stop_station_scan_on_input(event)) {
        stop_station_scan_flow();
        return;
    }

    bool ble_detect_overlay_active = ble_device_detect_is_active() ||
                                     (ble_detect_poll_timer != NULL) ||
                                     (ble_detect_status != NULL);
    if (!track_overlay_active && ble_detect_overlay_active && should_stop_station_scan_on_input(event)) {
        stop_ble_detect_flow();
        return;
    }

    bool ble_adv_overlay_active = advertiser_scan_is_active() ||
                                  (ble_adv_poll_timer != NULL) ||
                                  (ble_adv_status != NULL);
    if (!track_overlay_active && ble_adv_overlay_active && should_stop_station_scan_on_input(event)) {
        stop_ble_adv_flow();
        return;
    }

    bool ble_gatt_overlay_active = gatt_scan_is_active() ||
                                   (ble_gatt_poll_timer != NULL) ||
                                   (ble_gatt_status != NULL);
    if (!track_overlay_active && ble_gatt_overlay_active && should_stop_station_scan_on_input(event)) {
        stop_ble_gatt_flow();
        return;
    }

    if (s_info_detail_active && event->type != INPUT_TYPE_TOUCH) {
        if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            back_event_cb(NULL);
            return;
        }
        if (event->type == INPUT_TYPE_JOYSTICK) {
            int button = event->data.joystick_index;
            if (button == 2) {
                options_info_scroll_step(1);
            } else if (button == 4) {
                options_info_scroll_step(-1);
            } else if (button == 0 || button == 1) {
                back_event_cb(NULL);
            }
            return;
        }
        if (event->type == INPUT_TYPE_KEYBOARD) {
            uint8_t key = event->data.key_value;
            if (key == LV_KEY_UP || key == 'k' || key == ';' || key == ',' || key == 44) {
                options_info_scroll_step(1);
            } else if (key == LV_KEY_DOWN || key == 'j' || key == '.' || key == '/' || key == 46 || key == 47) {
                options_info_scroll_step(-1);
            } else if (key == LV_KEY_LEFT || key == LV_KEY_ESC || key == 29 || key == '`' || key == 'h') {
                back_event_cb(NULL);
            }
            return;
        }
        if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                back_event_cb(NULL);
            } else if (event->data.encoder.direction != 0) {
                options_info_scroll_step(event->data.encoder.direction > 0 ? -1 : 1);
            }
            return;
        }
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            // When popup is open, only handle close button touches
            if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
                if (wigle_help_close_btn && lv_obj_is_valid(wigle_help_close_btn)) {
                    lv_area_t area; lv_obj_get_coords(wigle_help_close_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        wigle_help_close_cb(NULL);
                    }
                }
                // Consume all other touches when popup is open
                opt_touch_started = false;
                return;
            }
            if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
                if (wigle_manual_close_btn && lv_obj_is_valid(wigle_manual_close_btn)) {
                    lv_area_t c_area; lv_obj_get_coords(wigle_manual_close_btn, &c_area);
                    if (data->point.x >= c_area.x1 && data->point.x <= c_area.x2 &&
                        data->point.y >= c_area.y1 && data->point.y <= c_area.y2) {
                        wigle_manual_popup_selected = 1;
                        wigle_manual_popup_update_selection();
                        wigle_manual_popup_close_cb(NULL);
                        opt_touch_started = false;
                        return;
                    }
                }
                if (wigle_manual_upload_btn && lv_obj_is_valid(wigle_manual_upload_btn)) {
                    lv_area_t u_area; lv_obj_get_coords(wigle_manual_upload_btn, &u_area);
                    if (data->point.x >= u_area.x1 && data->point.x <= u_area.x2 &&
                        data->point.y >= u_area.y1 && data->point.y <= u_area.y2) {
                        wigle_manual_popup_selected = 0;
                        wigle_manual_popup_update_selection();
                        wigle_manual_popup_upload_cb(NULL);
                        opt_touch_started = false;
                        return;
                    }
                }
                opt_touch_started = false;
                return;
            }
            if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
                if (wigle_stats_down_btn && lv_obj_is_valid(wigle_stats_down_btn)) {
                    lv_area_t d_area; lv_obj_get_coords(wigle_stats_down_btn, &d_area);
                    if (data->point.x >= d_area.x1 && data->point.x <= d_area.x2 &&
                        data->point.y >= d_area.y1 && data->point.y <= d_area.y2) {
                        wigle_stats_popup_selected = 0;
                        wigle_stats_popup_update_selection();
                        wigle_stats_popup_activate_selected();
                        opt_touch_started = false;
                        return;
                    }
                }
                if (wigle_stats_close_btn && lv_obj_is_valid(wigle_stats_close_btn)) {
                    lv_area_t s_area; lv_obj_get_coords(wigle_stats_close_btn, &s_area);
                    if (data->point.x >= s_area.x1 && data->point.x <= s_area.x2 &&
                        data->point.y >= s_area.y1 && data->point.y <= s_area.y2) {
                        wigle_stats_popup_selected = 1;
                        wigle_stats_popup_update_selection();
                        wigle_stats_popup_activate_selected();
                        opt_touch_started = false;
                        return;
                    }
                }
                opt_touch_started = false;
                return;
            }

            if (opt_touch_started) {
                int dy = data->point.y - opt_touch_last_y;
                opt_touch_last_x = data->point.x;
                opt_touch_last_y = data->point.y;

                if (!opt_touch_dragged) {
                    opt_touch_drag_axis = opt_resolve_drag_axis(data->point.x - opt_touch_start_x,
                                                               data->point.y - opt_touch_start_y);
                    opt_touch_dragged = opt_touch_drag_axis != 0;
                }

                if (opt_touch_dragged && opt_touch_drag_axis == 1) {
                    bool live = settings_get_touch_drag_scroll(&G_Settings);
                    lv_obj_t *scroll_target = NULL;
                    detail_view_t *active_detail_view = NULL;
                    if (ap_detail_view && opt_touch_wifi_state == WIFI_MENU_AP_DETAILS) {
                        active_detail_view = ap_detail_view;
                    } else if (sinkhole_detail_view && opt_touch_wifi_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
                        active_detail_view = sinkhole_detail_view;
                    } else if (gtk_abuse_detail_view) {
                        active_detail_view = gtk_abuse_detail_view;
                    } else if (sta_detail_view && opt_touch_wifi_state == WIFI_MENU_STA_DETAILS) {
                        active_detail_view = sta_detail_view;
                    } else if (arp_detail_view && opt_touch_wifi_state == WIFI_MENU_ARP_DETAILS) {
                        active_detail_view = arp_detail_view;
                    } else if (ble_detect_detail_view &&
                               opt_touch_bluetooth_state == BLUETOOTH_MENU_DETECT_DETAILS) {
                        active_detail_view = ble_detect_detail_view;
                    } else if (ble_adv_detail_view &&
                               opt_touch_bluetooth_state == BLUETOOTH_MENU_ADV_DETAILS) {
                        active_detail_view = ble_adv_detail_view;
                    } else if (ble_gatt_detail_view &&
                               opt_touch_bluetooth_state == BLUETOOTH_MENU_GATT_DETAILS) {
                        active_detail_view = ble_gatt_detail_view;
                    }

                    if (active_detail_view) {
                        scroll_target = opt_detail_scroll_target(active_detail_view, opt_touch_start_x, opt_touch_start_y);
                    } else if (menu_container && lv_obj_is_valid(menu_container)) {
                        lv_area_t cont_area;
                        lv_obj_get_coords(menu_container, &cont_area);
                        bool started_in_container = (opt_touch_start_x >= cont_area.x1 && opt_touch_start_x <= cont_area.x2 &&
                                                     opt_touch_start_y >= cont_area.y1 && opt_touch_start_y <= cont_area.y2);
                        if (started_in_container) scroll_target = menu_container;
                    }

                    if (scroll_target && lv_obj_is_valid(scroll_target)) {
                        if (live) {
                            dy = opt_clamp_drag_delta(dy);
                            if (dy) display_manager_queue_scroll(scroll_target, dy);
                        } else {
                            // Remember the target; the scroll is applied on release
                            // using the total drag distance (release-on-release).
                            opt_touch_scroll_target = scroll_target;
                        }
                    }
                }
                return;
            }

            /* While the RSSI tracker overlay is up, only its Back button is
             * actionable. Swallow every other press/move sample so it neither
             * scrolls nor taps the menu hidden underneath. */
            if (track_meter && rssi_meter_is_active(track_meter)) {
                if (gui_touch_bar_hit(back_btn, data->point.x, data->point.y)) {
                    touch_back_button_cb(NULL);
                }
                opt_touch_started = false;
                return;
            }

            // existing "press" logic unchanged...
            if (gui_touch_bar_hit(scroll_up_btn, data->point.x, data->point.y)) {
                scroll_options_up(NULL);
                opt_touch_started = false;
                return;
            }
            if (gui_touch_bar_hit(scroll_down_btn, data->point.x, data->point.y)) {
                scroll_options_down(NULL);
                opt_touch_started = false;
                return;
            }
            if (gui_touch_bar_hit(back_btn, data->point.x, data->point.y)) {
                touch_back_button_cb(NULL);
                opt_touch_started = false;
                return;
            }
            // Handle touch start for detail_view
            if ((ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) ||
                (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) ||
                gtk_abuse_detail_view ||
                (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) ||
                (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) ||
                (ble_detect_detail_view &&
                 current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) ||
                (ble_adv_detail_view &&
                 current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) ||
                (ble_gatt_detail_view &&
                 current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS)) {
                if (!opt_touch_started) {
                    opt_touch_begin(data);
                }
                return;
            }
            if (!opt_touch_started) {
                opt_touch_begin(data);
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!opt_touch_started) return;
            bool was_dragged = opt_touch_dragged;
            int release_dy = data->point.y - opt_touch_start_y;
            lv_obj_t *release_target = opt_touch_scroll_target;
            opt_touch_reset();
            opt_touch_scroll_target = NULL;
            if (was_dragged) {
                // Release-on-release: when live drag is off, apply the total
                // drag distance to the remembered target on release.
                if (release_target && lv_obj_is_valid(release_target) &&
                    !settings_get_touch_drag_scroll(&G_Settings) && release_dy) {
                    display_manager_queue_scroll(release_target, release_dy);
                }
                return;
            }

            if (s_info_detail_active) {
                return;
            }

            // Handle touch for detail_view (use saved state from touch start)
            detail_view_t *active_detail_view = NULL;
            if (ap_detail_view && opt_touch_wifi_state == WIFI_MENU_AP_DETAILS) {
                active_detail_view = ap_detail_view;
            } else if (sinkhole_detail_view && opt_touch_wifi_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
                active_detail_view = sinkhole_detail_view;
            } else if (gtk_abuse_detail_view) {
                active_detail_view = gtk_abuse_detail_view;
            } else if (sta_detail_view && opt_touch_wifi_state == WIFI_MENU_STA_DETAILS) {
                active_detail_view = sta_detail_view;
            } else if (arp_detail_view && opt_touch_wifi_state == WIFI_MENU_ARP_DETAILS) {
                active_detail_view = arp_detail_view;
            } else if (ble_detect_detail_view &&
                       opt_touch_bluetooth_state == BLUETOOTH_MENU_DETECT_DETAILS) {
                active_detail_view = ble_detect_detail_view;
            } else if (ble_adv_detail_view &&
                       opt_touch_bluetooth_state == BLUETOOTH_MENU_ADV_DETAILS) {
                active_detail_view = ble_adv_detail_view;
            } else if (ble_gatt_detail_view &&
                       opt_touch_bluetooth_state == BLUETOOTH_MENU_GATT_DETAILS) {
                active_detail_view = ble_gatt_detail_view;
            }

            if (active_detail_view) {
                lv_obj_t *scroll_target = opt_detail_scroll_target(active_detail_view, opt_touch_start_x, opt_touch_start_y);
                lv_obj_t *action_list = detail_view_get_list(active_detail_view);

                int dx = data->point.x - opt_touch_start_x;
                int dy = data->point.y - opt_touch_start_y;
                int thr_y = LV_VER_RES / 20;

                // Swipe scrolling: route to whichever region the press started in.
                if (abs(dy) > thr_y) {
                    if (scroll_target && lv_obj_is_valid(scroll_target)) {
                        display_manager_queue_scroll(scroll_target, dy);
                    }
                    return;
                }

                // Tap handling - only action rows are tappable. If the press
                // started over the info panel, swallow the tap (no action).
                if (abs(dy) <= thr_y && abs(dx) <= thr_y &&
                    action_list && lv_obj_is_valid(action_list) &&
                    scroll_target == action_list) {
                    uint32_t child_cnt = lv_obj_get_child_cnt(action_list);
                    for (uint32_t i = 0; i < child_cnt; i++) {
                        lv_obj_t *child = lv_obj_get_child(action_list, (int32_t)i);
                        if (!child) continue;

                        lv_area_t btn_area;
                        lv_obj_get_coords(child, &btn_area);

                        if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                            data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                            lv_event_send(child, LV_EVENT_CLICKED, NULL);
                            return;
                        }
                    }
                }
                return;
            }

            int dx = data->point.x - opt_touch_start_x;
            int dy = data->point.y - opt_touch_start_y;

            // Calculate swipe thresholds
            int thr_y = LV_VER_RES / OPT_SWIPE_THRESHOLD_RATIO;
            // Lower threshold for portal HTML lists (short lists need a lighter swipe)
            if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT ||
                current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT ||
                current_wifi_menu_state == WIFI_MENU_AP_LIST ||
                current_wifi_menu_state == WIFI_MENU_STA_LIST ||
                current_wifi_menu_state == WIFI_MENU_SCANALL_LIST ||
                current_wifi_menu_state == WIFI_MENU_CAPTURE_BROWSER ||
                (SelectedMenuType == OT_Bluetooth &&
                 (current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_LIST ||
                  current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_LIST ||
                  current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_LIST)) ||
                SelectedMenuType == OT_WigleManualUpload) {
                thr_y = LV_VER_RES / 20; // much more sensitive for short lists
            }
            int thr_x = LV_HOR_RES / OPT_SWIPE_THRESHOLD_RATIO;

            if (!menu_container || !lv_obj_is_valid(menu_container)) {
                return;
            }

            // Check if swipe started in menu container (allow release outside for natural swipes)
            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            bool started_in_container = (opt_touch_start_x >= cont_area.x1 && opt_touch_start_x <= cont_area.x2 &&
                                        opt_touch_start_y >= cont_area.y1 && opt_touch_start_y <= cont_area.y2);
            
            if (!started_in_container) {
                return;
            }

            // For tap gestures (not swipes), require release inside container
            if (abs(dy) <= thr_y && abs(dx) <= thr_x) {
                if (data->point.x < cont_area.x1 || data->point.x > cont_area.x2 ||
                    data->point.y < cont_area.y1 || data->point.y > cont_area.y2) {
                    return;
                }
            }

            // thirds-control special behavior within the menu list area
            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int container_h = (int)(cont_area.y2 - cont_area.y1);
                if (container_h > 0) {
                    int y_rel = (int)data->point.y - (int)cont_area.y1;
                    if (y_rel < container_h / 3) {
                        select_option_item(selected_item_index - 1);
                    } else if (y_rel > (container_h * 2) / 3) {
                        select_option_item(selected_item_index + 1);
                    } else {
                        // Middle third - handle selection
                        if (is_settings_mode) {
                            settings_activate_row(selected_item_index, true);
                        } else {
                            // Non-settings menus
                            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                            if (sel) handle_option_directly((const char*)lv_obj_get_user_data(sel));
                        }
                    }
                    return;
                }
            }

            // vertical swipe = scroll
            if (abs(dy) > thr_y) {
                display_manager_queue_scroll(menu_container, dy);
                return;
            }
            // horizontal swipe = ignore
            if (abs(dx) > thr_x) return;

            // now treat as tap inside the menu list (container bounds already verified)

            // find which button was tapped
            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    // highlight it
                    select_option_item(i);

                    if (is_settings_mode) {
                        int center_x = (btn_area.x1 + btn_area.x2) / 2;
                        settings_activate_row(i, data->point.x >= center_x);
                    } else {
                        // non-settings menus
                        const char *opt = (const char*)lv_obj_get_user_data(btn);
                        handle_option_directly(opt);
                    }
                    return;
                }
            }
            return;
        }
        return;
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        ESP_LOGI(TAG, "Joystick index = %d", button);

        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (button == 0 || button == 2 || button == 3 || button == 4) {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            } else if (button == 1) {
                if (wigle_manual_popup_selected == 0) {
                    wigle_manual_popup_upload_cb(NULL);
                } else {
                    wigle_manual_popup_close_cb(NULL);
                }
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (button == 0 || button == 2 || button == 3 || button == 4) {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            } else if (button == 1) {
                wigle_stats_popup_activate_selected();
            }
            return;
        }
        
        if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            if (button == 2) {
                detail_view_step_up(ap_detail_view);
            } else if (button == 4) {
                detail_view_step_down(ap_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(ap_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                ap_detail_back_cb(NULL);
            }
            return;
        }

        if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
            if (button == 2) {
                detail_view_step_up(sinkhole_detail_view);
            } else if (button == 4) {
                detail_view_step_down(sinkhole_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(sinkhole_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                sinkhole_detail_back_cb(NULL);
            }
            return;
        }

        if (gtk_abuse_detail_view) {
            if (button == 2) {
                detail_view_step_up(gtk_abuse_detail_view);
            } else if (button == 4) {
                detail_view_step_down(gtk_abuse_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(gtk_abuse_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                gtk_abuse_back_cb(NULL);
            }
            return;
        }

        if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            if (button == 2) {
                detail_view_step_up(sta_detail_view);
            } else if (button == 4) {
                detail_view_step_down(sta_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(sta_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                station_detail_back_cb(NULL);
            }
            return;
        }

        if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
            if (button == 2) {
                detail_view_step_up(ble_detect_detail_view);
            } else if (button == 4) {
                detail_view_step_down(ble_detect_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_detect_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                ble_detect_detail_back_cb(NULL);
            }
            return;
        }

        if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
            if (button == 2) {
                detail_view_step_up(ble_adv_detail_view);
            } else if (button == 4) {
                detail_view_step_down(ble_adv_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_adv_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                ble_adv_detail_back_cb(NULL);
            }
            return;
        }

        if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
            if (button == 2) {
                detail_view_step_up(ble_gatt_detail_view);
            } else if (button == 4) {
                detail_view_step_down(ble_gatt_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_gatt_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                ble_gatt_detail_back_cb(NULL);
            }
            return;
        }

        if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
            if (button == 2) {
                detail_view_step_up(arp_detail_view);
            } else if (button == 4) {
                detail_view_step_down(arp_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(arp_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                arp_detail_back_cb(NULL);
            }
            return;
        }

        if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
            if (button == 2) {
                detail_view_step_up(mdns_detail_view);
            } else if (button == 4) {
                detail_view_step_down(mdns_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(mdns_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                mdns_detail_back_cb(NULL);
            }
            return;
        }

        if (govee_detail_view && current_wifi_menu_state == WIFI_MENU_GOVEE_DETAILS) {
            if (button == 2) {
                detail_view_step_up(govee_detail_view);
            } else if (button == 4) {
                detail_view_step_down(govee_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(govee_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                govee_detail_back_cb(NULL);
            }
            return;
        }

        if (enum_detail_view && current_wifi_menu_state == WIFI_MENU_ENUM_DETAILS) {
            if (button == 2) {
                detail_view_step_up(enum_detail_view);
            } else if (button == 4) {
                detail_view_step_down(enum_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(enum_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                enum_detail_back_cb(NULL);
            }
            return;
        }

        if (sweep_detail_view) {
            if (button == 2) {
                detail_view_step_up(sweep_detail_view);
            } else if (button == 4) {
                detail_view_step_down(sweep_detail_view);
            } else if (button == 1) {
                lv_obj_t *obj = detail_view_get_selected_obj(sweep_detail_view);
                if (obj && lv_obj_is_valid(obj)) {
                    lv_event_send(obj, LV_EVENT_CLICKED, NULL);
                }
            } else if (button == 0 || button == 3) {
                sweep_detail_back_cb(NULL);
            }
            return;
        }
        
        /* Windowed result lists own their own navigation and activation: the
         * pool rows carry the item index and options_view rebinds them on
         * scroll, so there are no page rows to walk here. This sits ahead of
         * the per-list paged handling below. */
        if (vlist_kind_for_state() != VLIST_NONE && options_view_is_virtual(g_options_view)) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                int count = options_view_virtual_count(g_options_view);
                if (selected_item_index < 0 || selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }
                vlist_activate(selected_item_index, NULL);
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST && scanall_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(scanall_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];

                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(scanall_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(scanall_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(scanall_list_menu);
                        int skip = paged_menu_has_prev(scanall_list_menu) ? 1 : 0;
                        int row_idx = offset + (selected_item_index - skip);
                        scanall_select_row(row_idx);
                    }
                }
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_STA_LIST && sta_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(sta_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];

                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(sta_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(sta_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(sta_list_menu);
                        int skip = paged_menu_has_prev(sta_list_menu) ? 1 : 0;
                        int idx = offset + (selected_item_index - skip);
                        show_station_detail(idx);
                    }
                }
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_LIST &&
            ble_detect_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(ble_detect_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];
                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(ble_detect_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(ble_detect_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(ble_detect_list_menu);
                        int skip = paged_menu_has_prev(ble_detect_list_menu) ? 1 : 0;
                        show_ble_detect_detail(offset + (selected_item_index - skip));
                    }
                }
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_LIST &&
            ble_adv_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(ble_adv_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];
                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(ble_adv_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(ble_adv_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(ble_adv_list_menu);
                        int skip = paged_menu_has_prev(ble_adv_list_menu) ? 1 : 0;
                        show_ble_adv_detail(offset + (selected_item_index - skip));
                    }
                }
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_LIST &&
            ble_gatt_list_menu) {
            if (button == 2) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index <= 0) ? (num_items - 1) : (selected_item_index - 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 4) {
                if (num_items > 0) {
                    selected_item_index = (selected_item_index >= (num_items - 1)) ? 0 : (selected_item_index + 1);
                }
                select_option_item(selected_item_index);
            } else if (button == 1) {
                const char **opts = paged_menu_get_options(ble_gatt_list_menu);
                int count = 0;
                for (int i = 0; opts[i]; i++) count++;

                if (selected_item_index >= count) {
                    back_event_cb(NULL);
                    return;
                }

                const char *selected_option = opts[selected_item_index];
                if (selected_option) {
                    if (strcmp(selected_option, "< Prev") == 0) {
                        paged_menu_page_prev(ble_gatt_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "Next >") == 0) {
                        paged_menu_page_next(ble_gatt_list_menu);
                        rebuild_current_menu();
                    } else if (strcmp(selected_option, "No items found") != 0) {
                        int offset = paged_menu_get_page_offset(ble_gatt_list_menu);
                        int skip = paged_menu_has_prev(ble_gatt_list_menu) ? 1 : 0;
                        show_ble_gatt_detail(offset + (selected_item_index - skip));
                    }
                }
            } else if (button == 0 || button == 3) {
                back_event_cb(NULL);
            }
            return;
        }

        if (button == 2) {
            select_option_item(selected_item_index - 1);
        } else if (button == 4) {
            select_option_item(selected_item_index + 1);
        } else if (button == 1) { // Normal select button
            if (is_settings_mode) {
                settings_activate_row(selected_item_index, true);
            } else {
                // Non-settings menu selection
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else if (button == 0) { // left button
            if (is_settings_mode && current_settings_category >= 0) {
#if GUI_LARGE_TOUCH_UI
                /* The large-screen left-edge swipe is translated into joystick-left.
                 * It is a navigation gesture, so it must leave a settings
                 * submenu instead of changing the highlighted row's value. */
                ESP_LOGI(TAG, "System back swipe pressed, going back");
                back_event_cb(NULL);
#else
                // in settings submenu, check if we're on the back option
                lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                if (sel) {
                    void *udata = lv_obj_get_user_data(sel);
                    if (udata == (void *)"__BACK_OPTION__") {
                        // if on back option, go back
                        ESP_LOGI(TAG, "joystick left pressed on back option, going back");
                        back_event_cb(NULL);
                    } else {
                        // otherwise left decrements value
                        change_current_row(false);
                    }
                }
#endif
            } else {
                // otherwise left goes back
                ESP_LOGI(TAG, "joystick left pressed, going back");
                back_event_cb(NULL);
            }
        } else if (button == 3) { // Cardputer select button OR Right (increment) button for settings
            if (is_settings_mode && current_settings_category >= 0) {
                // in settings submenu, check if we're on the back option
                lv_obj_t *sel = lv_obj_get_child(menu_container, selected_item_index);
                if (sel) {
                    void *udata = lv_obj_get_user_data(sel);
                    if (udata == (void *)"__BACK_OPTION__") {
                        // if on back option, go back
                        ESP_LOGI(TAG, "joystick right pressed on back option, going back");
                        back_event_cb(NULL);
                    } else {
                        // otherwise right increments value
                        change_current_row(true);
                    }
                }
            }
            // For non-settings, button 3 doesn't have a defined action as per the problem description.
            // If it were a general 'select' for non-settings, it would need similar logic to button 1's 'else' block.
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (keyValue == 'h' || keyValue == 'l' || keyValue == ',' || keyValue == ';' || keyValue == '/' || keyValue == '.') {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            } else if (keyValue == 13) {
                if (wigle_manual_popup_selected == 0) wigle_manual_popup_upload_cb(NULL);
                else wigle_manual_popup_close_cb(NULL);
            } else if (keyValue == 29 || keyValue == '`') {
                wigle_manual_popup_close_cb(NULL);
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (keyValue == 'h' || keyValue == 'l') {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            } else if (keyValue == 'k' || keyValue == 44 || keyValue == ',' || keyValue == 59 || keyValue == ';') {
                wigle_stats_popup_scroll(-40);
            } else if (keyValue == 'j' || keyValue == 47 || keyValue == '/' || keyValue == 46 || keyValue == '.') {
                wigle_stats_popup_scroll(40);
            } else if (keyValue == 13) {
                wigle_stats_popup_activate_selected();
            } else if (keyValue == 29 || keyValue == '`') {
                wigle_stats_popup_close_cb(NULL);
            }
            return;
        }

        if (handle_wifi_detail_keyboard(keyValue)) {
            return;
        }

        // --- Vim keybinds ---
        if (keyValue == 'h') { // Vim left
            ESP_LOGI(TAG, "Vim 'h' pressed (left)");
            if (is_settings_mode) {
                change_current_row(false);
            } else {
                select_option_item(selected_item_index - 1);
            }
        } else if (keyValue == 'l') { // Vim right
            ESP_LOGI(TAG, "Vim 'l' pressed (right)");
            if (is_settings_mode) {
                change_current_row(true);
            } else {
                select_option_item(selected_item_index + 1);
            }
        } else if (keyValue == 'k') { // Vim up
            ESP_LOGI(TAG, "Vim 'k' pressed (up)");
            select_option_item(selected_item_index - 1);
        } else if (keyValue == 'j') { // Vim down
            ESP_LOGI(TAG, "Vim 'j' pressed (down)");
            select_option_item(selected_item_index + 1);
        }
        // --- Existing keybinds ---
        else if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) {
            ESP_LOGI(TAG, "Left/Up button pressed");
            if (is_settings_mode && (keyValue == 44 || keyValue == ',')) {
                change_current_row(false);
            } else {
                select_option_item(selected_item_index - 1);
            }
        } else if ((keyValue == 47 || keyValue == '/') || (keyValue == 46 || keyValue == '.')) {
            ESP_LOGI(TAG, "Right/Down button pressed");
            if (is_settings_mode && (keyValue == 47 || keyValue == '/')) {
                change_current_row(true);
            } else {
                select_option_item(selected_item_index + 1);
            }
        } else if (keyValue == 13) {
            ESP_LOGI(TAG, "Enter button pressed");
            if (is_settings_mode) {
                settings_activate_row(selected_item_index, true);
            } else {
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else if (keyValue == 29 || keyValue == '`') { // esc
            ESP_LOGI(TAG, "Esc button pressed");
            back_event_cb(NULL);
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
            if (event->data.encoder.button) {
                if (wigle_manual_popup_selected == 0) wigle_manual_popup_upload_cb(NULL);
                else wigle_manual_popup_close_cb(NULL);
            } else {
                wigle_manual_popup_selected = (wigle_manual_popup_selected + 1) % 2;
                wigle_manual_popup_update_selection();
            }
            return;
        }

        if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
            if (event->data.encoder.button) {
                wigle_stats_popup_activate_selected();
            } else if (event->data.encoder.direction != 0) {
                wigle_stats_popup_selected = (wigle_stats_popup_selected + 1) % 2;
                wigle_stats_popup_update_selection();
            }
            return;
        }

        if (ap_detail_view && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            ESP_LOGI("dv_nav", "ENC ap dir=%d btn=%d", event->data.encoder.direction, event->data.encoder.button);
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(ap_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(ap_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(ap_detail_view);
            }
            return;
        }
        if (sinkhole_detail_view && current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(sinkhole_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(sinkhole_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(sinkhole_detail_view);
            }
            return;
        }
        if (gtk_abuse_detail_view) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(gtk_abuse_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(gtk_abuse_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(gtk_abuse_detail_view);
            }
            return;
        }
        if (sta_detail_view && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(sta_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(sta_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(sta_detail_view);
            }
            return;
        }
        if (ble_detect_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_detect_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(ble_detect_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(ble_detect_detail_view);
            }
            return;
        }
        if (ble_adv_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_adv_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(ble_adv_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(ble_adv_detail_view);
            }
            return;
        }
        if (ble_gatt_detail_view && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(ble_gatt_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(ble_gatt_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(ble_gatt_detail_view);
            }
            return;
        }
        if (arp_detail_view && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(arp_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(arp_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(arp_detail_view);
            }
            return;
        }
        if (mdns_detail_view && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(mdns_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(mdns_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(mdns_detail_view);
            }
            return;
        }
        if (govee_detail_view && current_wifi_menu_state == WIFI_MENU_GOVEE_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(govee_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(govee_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(govee_detail_view);
            }
            return;
        }
        if (enum_detail_view && current_wifi_menu_state == WIFI_MENU_ENUM_DETAILS) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(enum_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(enum_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(enum_detail_view);
            }
            return;
        }
        if (sweep_detail_view) {
            if (event->data.encoder.button) {
                lv_obj_t *obj = detail_view_get_selected_obj(sweep_detail_view);
                if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
            } else if (event->data.encoder.direction < 0) {
                detail_view_step_up(sweep_detail_view);
            } else if (event->data.encoder.direction > 0) {
                detail_view_step_down(sweep_detail_view);
            }
            return;
        }

        /* Windowed result lists use a fixed pool of visible rows.  The
         * selected index is an index into the backing scan, not a child index
         * in menu_container, so route encoder activation through the same
         * callback used by the virtual row click handler. */
        if (vlist_kind_for_state() != VLIST_NONE && options_view_is_virtual(g_options_view)) {
            if (event->data.encoder.button) {
                int count = options_view_virtual_count(g_options_view);
                if (selected_item_index < 0 || selected_item_index >= count) {
                    back_event_cb(NULL);
                } else {
                    vlist_activate(selected_item_index, NULL);
                }
            } else if (event->data.encoder.direction < 0) {
                select_option_item(selected_item_index - 1);
            } else if (event->data.encoder.direction > 0) {
                select_option_item(selected_item_index + 1);
            }
            return;
        }

        if (event->data.encoder.button) {
            // Encoder button press - treat as select/enter/cycle
            if (is_settings_mode) {
                settings_activate_row(selected_item_index, true);
            } else {
                // Non-settings menus: button selects the item
                lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
                if (selected_obj) {
                    const char *selected_option = (const char *)lv_obj_get_user_data(selected_obj);
                    if (selected_option) {
                        handle_option_directly(selected_option);
                    }
                }
            }
        } else {
            // Encoder direction change (rotation) - always navigate/select item
            if (event->data.encoder.direction > 0) { // Clockwise (CW) - down/right
                select_option_item(selected_item_index + 1);
            } else { // Counter-clockwise (CCW) - up/left
                select_option_item(selected_item_index - 1);
            }
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, navigating back");
        back_event_cb(NULL);
#endif
    }
}

static void karma_custom_ssids_cb(const char *input) {
    if (!input || strlen(input) == 0) {
        error_popup_create("Please enter at least one SSID.");
        return;
    }

    // Parse comma-separated SSIDs
    const char *ssids[KARMA_MAX_SSIDS];
    // Heap-allocate to avoid blowing the LVGL task stack (2 KB+ on-stack otherwise).
    char *ssid_buf = malloc(33 * KARMA_MAX_SSIDS);
    if (!ssid_buf) {
        error_popup_create("Out of memory.");
        return;
    }
    int count = 0;

    // Copy input to buffer for strtok
    strncpy(ssid_buf, input, 33 * KARMA_MAX_SSIDS - 1);
    ssid_buf[33 * KARMA_MAX_SSIDS - 1] = '\0';

    char *token = strtok(ssid_buf, ",");
    while (token && count < KARMA_MAX_SSIDS) {
        // Trim leading/trailing spaces
        while (*token == ' ') token++;
        char *end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\n' || *end == '\r')) {
            *end = '\0';
            end--;
        }
        if (strlen(token) > 0 && strlen(token) < 33) {
            ssids[count++] = token;
        }
        token = strtok(NULL, ",");
    }

    if (count == 0) {
        free(ssid_buf);
        error_popup_create("No valid SSIDs entered.");
        return;
    }

    // Set SSID list and start Karma attack
    wifi_manager_set_karma_ssid_list(ssids, count);
    free(ssid_buf);
    wifi_manager_start_karma();

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    TERMINAL_VIEW_ADD_TEXT("Karma attack started with custom SSIDs\n");
    keyboard_view_set_submit_callback(NULL);
}

static void gtk_abuse_back_cb(lv_event_t *e) {
    (void)e;
    if (gtk_abuse_is_running()) {
        wifi_manager_stop_gtk_abuse();
    }
    if (gtk_abuse_status) {
        scan_status_close(gtk_abuse_status);
        gtk_abuse_status = NULL;
    }
    if (gtk_abuse_detail_view) {
        detail_view_destroy(gtk_abuse_detail_view);
        gtk_abuse_detail_view = NULL;
    }
    SelectedMenuType = OT_Wifi;
    suppress_wifi_state_reset_once = true;
    option_invoked = false;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
    current_wifi_menu_state = WIFI_MENU_ATTACKS;
}

static void gtk_abuse_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    if (gtk_abuse_is_running()) return;

    if (timer) lv_timer_del(timer);

    if (gtk_abuse_status) {
        scan_status_close(gtk_abuse_status);
        gtk_abuse_status = NULL;
    }

    SelectedMenuType = OT_Wifi;
    suppress_wifi_state_reset_once = true;
    keyboard_view_set_submit_callback(NULL);

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    if (gtk_abuse_detail_view) {
        detail_view_destroy(gtk_abuse_detail_view);
        gtk_abuse_detail_view = NULL;
    }

    const gtk_abuse_result_t *r = gtk_abuse_get_result();
    gtk_abuse_detail_view = detail_view_create(lv_scr_act(), "GTK Abuse Result");
    reserve_detail_touch_bar_space(gtk_abuse_detail_view);
    detail_view_t *dv = gtk_abuse_detail_view;
    bool compact_detail = use_compact_wifi_detail_layout();

    detail_view_add_info(dv, "SSID", r->ssid);
    detail_view_add_info(dv, "Target", r->gateway_ip[0] ? r->gateway_ip : "-");
    detail_view_add_info(dv, "Valid",
                         !r->gtk_validation_available ? "N/A" :
                         (r->gtk_validated ? "YES" : "NO"));

    if (r->isolation_broken) {
        detail_view_add_info(dv, "Verdict", "Broken");
        detail_view_add_info(dv, "Status", "Reply seen");
    } else if (r->frame_injected) {
        detail_view_add_info(dv, "Verdict", "Unconfirmed");
        detail_view_add_info(dv, "Status", "No reply seen");
    } else if (r->connected) {
        detail_view_add_info(dv, "Verdict", "Failed");
        detail_view_add_info(dv, "Status", "GTK extract failed");
    } else {
        detail_view_add_info(dv, "Verdict", "Failed");
        detail_view_add_info(dv, "Status", "Connection failed");
    }

    if (!compact_detail) {
        detail_view_add_info(dv, "Actions:", "");
    }

    detail_view_add_back(dv, gtk_abuse_back_cb, NULL);
    current_wifi_menu_state = WIFI_MENU_ATTACKS;
}

static void gtk_abuse_password_cb(const char *input) {
    SelectedMenuType = OT_Wifi;
    current_wifi_menu_state = WIFI_MENU_ATTACKS;
    suppress_wifi_state_reset_once = true;
    display_manager_switch_view(&options_menu_view);

    if (gtk_abuse_status) {
        scan_status_close(gtk_abuse_status);
    }
    gtk_abuse_status = scan_status_create("GTK Abuse Test");
    if (gtk_abuse_status) scan_status_set_subtext(gtk_abuse_status, "Connecting...");

    wifi_manager_start_gtk_abuse(gtk_abuse_ssid, input ? input : "");
    TERMINAL_VIEW_ADD_TEXT("GTK Abuse test started for %s\n", gtk_abuse_ssid);
    keyboard_view_set_submit_callback(NULL);

    lv_timer_create(gtk_abuse_poll_timer_cb, 500, NULL);
}

static void sae_flood_password_cb(const char *input) {
    static char cmd[80];
    snprintf(cmd, sizeof(cmd), "saeflood %s", input ? input : "");
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void gtk_abuse_ssid_cb(const char *input) {
    if (!input || strlen(input) == 0) {
        error_popup_create("Please enter an SSID.");
        return;
    }
    strlcpy(gtk_abuse_ssid, input, sizeof(gtk_abuse_ssid));
    keyboard_view_set_submit_callback(gtk_abuse_password_cb);
    keyboard_view_set_placeholder("Password");
    keyboard_view_set_initial_text("");
}

// Called after the user picks a portal file and optionally types SSIDs.
// selected_karma_portal holds the filename chosen from the SD card list.
static void karma_portal_ssids_cb(const char *input) {
    if (!selected_karma_portal[0]) {
        error_popup_create("No portal selected.");
        return;
    }

    // Keep this off the small LVGL task stack without reserving permanent DRAM.
    char *portal_path = malloc(320);
    if (!portal_path) {
        error_popup_create("Out of memory.");
        return;
    }
    if (strcmp(selected_karma_portal, "default") == 0) {
        strncpy(portal_path, "default", 320);
    } else {
        snprintf(portal_path, 320,
                  "/mnt/ghostesp/evil_portal/portals/%s", selected_karma_portal);
    }
    wifi_manager_set_karma_portal_file(portal_path);

    // Parse optional comma-separated SSIDs; blank = passive/auto mode.
    if (input && strlen(input) > 0) {
        const char *ssids[KARMA_MAX_SSIDS];
        // Heap-allocate to avoid blowing the LVGL task stack (2 KB+ on-stack otherwise).
        char *ssid_buf = malloc(33 * KARMA_MAX_SSIDS);
        if (!ssid_buf) {
            free(portal_path);
            error_popup_create("Out of memory.");
            return;
        }
        int count = 0;

        strncpy(ssid_buf, input, 33 * KARMA_MAX_SSIDS - 1);
        ssid_buf[33 * KARMA_MAX_SSIDS - 1] = '\0';

        char *token = strtok(ssid_buf, ",");
        while (token && count < KARMA_MAX_SSIDS) {
            while (*token == ' ') token++;
            char *end = token + strlen(token) - 1;
            while (end > token && (*end == ' ' || *end == '\n' || *end == '\r')) {
                *end = '\0';
                end--;
            }
            if (strlen(token) > 0 && strlen(token) < 33) {
                ssids[count++] = token;
            }
            token = strtok(NULL, ",");
        }
        if (count > 0) {
            wifi_manager_set_karma_ssid_list(ssids, count);
        }
        free(ssid_buf);
    }

    wifi_manager_start_karma();

    selected_karma_portal[0] = '\0';
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    TERMINAL_VIEW_ADD_TEXT("Karma attack started with custom portal: %s\n", portal_path);
    free(portal_path);
    keyboard_view_set_submit_callback(NULL);
}

void option_event_cb(lv_event_t *e) {
    if (esp_timer_get_time() < option_input_blocked_until_us) return;
    if (option_invoked) return;
    option_invoked = true;
    bool view_switched = false;

    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    
    if (now_ms - createdTimeInMs <= 500) {
        option_invoked = false; 
        return;
    }
    
    // stop incremental menu builder before any potential view switch
    lvgl_timer_del_safe(&menu_build_timer);
    
    if (is_settings_mode) {
        void *raw_udata = lv_event_get_user_data(e);

        if (raw_udata == (void *)"__BACK_OPTION__") {
            back_event_cb(NULL);
            option_invoked = false;
            return;
        }

        if (current_settings_root < 0) {
            switch_to_settings_root((int)(intptr_t)raw_udata);
            option_invoked = false;
            return;
        }

        if (current_settings_category < 0) {
            switch_to_settings_category((int)(intptr_t)raw_udata);
            option_invoked = false;
            return;
        }

        int setting_index = (int)(intptr_t)raw_udata;
#ifdef CONFIG_USE_IO_EXPANDER
        if (setting_index == IO_BTN_EDIT_P10 || setting_index == IO_BTN_EDIT_P11 || setting_index == IO_BTN_EDIT_P12) {
            io_btn_being_edited = (setting_index == IO_BTN_EDIT_P10) ? 0 : (setting_index == IO_BTN_EDIT_P11) ? 1 : 2;
            SelectedMenuType = OT_IOButtonPresets;
            is_settings_mode = false;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
#endif
        change_setting_value(setting_index, true);
        option_invoked = false;
        return;
    }

    const char *Selected_Option = (const char *)lv_event_get_user_data(e);

    // Handle the "Back" option specifically (for encoder/joystick modes)
    if (strcmp(Selected_Option, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_WigleManualUpload) {
        if (strcmp(Selected_Option, "No CSV files found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            wigle_csv_page_offset += WIGLE_CSV_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            wigle_csv_page_offset -= WIGLE_CSV_PAGE_SIZE;
            if (wigle_csv_page_offset < 0) wigle_csv_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        wigle_show_csv_details_popup(Selected_Option);
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_IOButtonPresets) {
#ifdef CONFIG_USE_IO_EXPANDER
        int preset_idx = -1;
        for (int i = 0; i < NUM_IO_BTN_PRESETS; i++) {
            if (strcmp(Selected_Option, io_btn_presets[i].name) == 0) {
                preset_idx = i;
                break;
            }
        }

        if (preset_idx >= 0) {
            const char* prefix = io_btn_presets[preset_idx].cmd_prefix;
            if (strcmp(prefix, "cmd:") == 0) {
                const char* cur = (io_btn_being_edited == 0) ? settings_get_io_btn_p10_cmd(&G_Settings)
                                 : (io_btn_being_edited == 1) ? settings_get_io_btn_p11_cmd(&G_Settings)
                                 : settings_get_io_btn_p12_cmd(&G_Settings);
                const char* cmd_start = cur ? cur : "";
                if (strncmp(cmd_start, "cmd:", 4) == 0) cmd_start += 4;
                keyboard_view_set_return_view(&options_menu_view);
                keyboard_view_set_placeholder("Command (e.g. nfc read)");
                keyboard_view_set_start_caps(false);
                keyboard_view_set_initial_text(cmd_start);
                keyboard_view_set_submit_callback(io_btn_being_edited == 0 ? iobtn_p10_kb_cb : io_btn_being_edited == 1 ? iobtn_p11_kb_cb : iobtn_p12_kb_cb);
                display_manager_switch_view(&keyboard_view);
            } else {
                if (io_btn_being_edited == 0) {
                    settings_set_io_btn_p10_cmd(&G_Settings, prefix);
                } else if (io_btn_being_edited == 1) {
                    settings_set_io_btn_p11_cmd(&G_Settings, prefix);
                } else {
                    settings_set_io_btn_p12_cmd(&G_Settings, prefix);
                }
                settings_save(&G_Settings);
                current_settings_root = SETTINGS_ROOT_DEVICE;
                current_settings_category = settings_category_index_for_id(SETTINGS_CAT_IO_BUTTONS);
                settings_submenu_depth = 2;
                SelectedMenuType = OT_Settings;
                is_settings_mode = true;
                rebuild_current_menu();
            }
        }
        option_invoked = false;
        return;
#else
        option_invoked = false;
        return;
#endif
    }

    if (SelectedMenuType == OT_DualComm) {
        if (current_dualcomm_menu_state == DUALCOMM_MENU_MAIN) {
            if (strcmp(Selected_Option, "Status") == 0) {
                // Allow quick access to Status from main
                terminal_set_return_view(&options_menu_view);
                terminal_set_dualcomm_filter(true);
                display_manager_switch_view(&terminal_view);
                simulateCommand("commsend commstatus");
                view_switched = true;
            } else if (strcmp(Selected_Option, "Discovery / Session") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_SESSION;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Scanning") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_SCAN;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "WiFi") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_WIFI;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Attacks") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_ATTACKS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Capture") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_CAPTURE;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Tools") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_TOOLS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "BLE") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_BLE;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "GPS") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_GPS;
                rebuild_current_menu();
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Ethernet") == 0) {
                ethernet_screen_set_return_view(&options_menu_view);
                display_manager_switch_view(&ethernet_screen_view);
                option_invoked = false;
                return;
            } else if (strcmp(Selected_Option, "Keyboard") == 0) {
                current_dualcomm_menu_state = DUALCOMM_MENU_KEYBOARD;
                rebuild_current_menu();
                option_invoked = false;
                return;
            }
        }

        if (strcmp(Selected_Option, "Status") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commstatus");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Discovery") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commdiscovery");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to Peer") == 0) {
            keyboard_view_set_submit_callback(dual_comm_connect_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Peer name (e.g. ESP_XXXXXX)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Disconnect") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend commdisconnect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Send Remote Command") == 0) {
            keyboard_view_set_submit_callback(dual_comm_send_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Command to run on peer");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Access Points") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan APs Live") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanap -live");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Stations") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scansta");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan AP + STA") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanall");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Sweep") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend sweep");
            view_switched = true;
        } else if (strcmp(Selected_Option, "mDNS Discovery") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanlocal");
            view_switched = true;
        } else if (strcmp(Selected_Option, "ARP Scan Network") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanarp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanports local -C");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan SSH") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanssh");
            view_switched = true;
        } else if (strcmp(Selected_Option, "NetBIOS Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend netbiosscan");
            view_switched = true;
        } else if (strcmp(Selected_Option, "HTTP Banner Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend httpbannerscan");
            view_switched = true;
        } else if (strcmp(Selected_Option, "SNMP Probe") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend snmpprobe");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan SSH Host...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(ssh_scan_kb_cb);
            keyboard_view_set_placeholder("IP address (e.g. 192.168.1.1)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "NetBIOS Scan Host...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(netbios_scan_kb_cb);
            keyboard_view_set_placeholder("IP address (e.g. 192.168.1.1)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "HTTP Banner Host...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(http_banner_kb_cb);
            keyboard_view_set_placeholder("IP address (e.g. 192.168.1.1)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "SNMP Probe Host...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(snmp_probe_kb_cb);
            keyboard_view_set_placeholder("IP address (e.g. 192.168.1.1)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "NetBIOS Subnet...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(dual_comm_netbios_subnet_kb_cb);
            keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "HTTP Banner Subnet...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(dual_comm_http_banner_subnet_kb_cb);
            keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "SNMP Probe Subnet...") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(dual_comm_snmp_probe_subnet_kb_cb);
            keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
            keyboard_view_set_initial_text("");
            display_manager_switch_view(&keyboard_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend pineap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Flock Detection") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend flockscan");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Channel Congestion") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend congestion");
            view_switched = true;
        } else if (strcmp(Selected_Option, "List Access Points") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend list -a");
            view_switched = true;
        } else if (strcmp(Selected_Option, "List Stations") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend list -s");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Select Station") == 0) {
            set_number_pad_mode(NP_MODE_STA_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "Select AP") == 0) {
            set_number_pad_mode(NP_MODE_AP_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
        } else if (strcmp(Selected_Option, "Track AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend trackap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Track Station") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend tracksta");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to WiFi") == 0) {
            keyboard_view_set_submit_callback(dual_comm_wifi_connect_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Connect to saved WiFi") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend connect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apcred -r");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Set AP Credentials") == 0) {
            keyboard_view_set_submit_callback(dual_comm_apcred_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Enable AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apenable on");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Disable AP") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend apenable off");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Deauth Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -d");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Handshake+Deauth") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -hsd");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Channel Switch Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -c");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start EAPOL Logoff") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend attack -e");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start DHCP-Starve") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dhcpstarve start");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop DHCP-Starve") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dhcpstarve stop");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Karma Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend karma start");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Karma Attack") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend karma stop");
            view_switched = true;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        } else if (strcmp(Selected_Option, "Start SAE Flood") == 0) {
            keyboard_view_set_return_view(&options_menu_view);
            keyboard_view_set_submit_callback(sae_flood_password_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Password");
            return;
#endif
        } else if (strcmp(Selected_Option, "Start Karma Attack (Custom SSIDs)") == 0) {
            keyboard_view_set_submit_callback(dual_comm_karma_custom_ssids_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("SSID1 SSID2 SSID3");
            return;
        } else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -deauth");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Probe") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -probe");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -beacon");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Raw") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -raw");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture Eapol") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -eapol");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture WPS") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -wps");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Capture PWN") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -pwn");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Listen for Probes") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listenprobes");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Evil Portal") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startportal default FreeWiFi");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Evil Portal") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend stopportal");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startwd");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend startwd -s");
            view_switched = true;
        } else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dialconnect");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Power Printer") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend powerprinter");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Scan SSH") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend scanssh");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Toggle WebUI AP Only") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend webuiap");
            view_switched = true;
        } else if (strcmp(Selected_Option, "BLE Bridge") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            bool enabled = ble_bridge_get_enabled() || ble_bridge_is_running();
            if (ble_bridge_set_enabled(!enabled)) {
                status_display_show_status(!enabled ? "BLE Bridge On" : "BLE Bridge Off");
                rebuild_current_menu();
                option_invoked = false;
                return;
            }
            error_popup_create("Failed to start BLE bridge");
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -a");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "List AirTags") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listairtags");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Select AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            set_number_pad_mode(NP_MODE_AIRTAG_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Spoof Selected AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend spoofairtag");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend stopspoof");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -f");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "List Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend listflippers");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Select Flipper") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            set_number_pad_mode(NP_MODE_FLIPPER_REMOTE);
            display_manager_switch_view(&number_pad_view);
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blescan -r");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend capture -skimmer");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Apple") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -apple");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Microsoft") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -ms");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Samsung") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -samsung");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Google") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -google");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE Spam - Random") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -random");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "Stop BLE Spam") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blespam -s");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "GPS Info") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend gpsinfo");
            view_switched = true;
        } else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend blewardriving");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Bluetooth...");
#endif
        } else if (strcmp(Selected_Option, "BLE + WiFi Wardriving") == 0) {
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32P4)
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend dualwd");
            view_switched = true;
#else
            error_popup_create("Device Does not Support Dual Wardriving...");
#endif
        } else if (strcmp(Selected_Option, "Initialise") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethup");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Deinitialise") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethdown");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Ethernet Info") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethinfo");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Fingerprint Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethfp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "ARP Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend etharp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Port Scan Local") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethports local");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Port Scan All") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethports local all");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Ping Scan") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethping");
            view_switched = true;
        } else if (strcmp(Selected_Option, "DNS Lookup") == 0) {
            keyboard_view_set_submit_callback(dual_comm_dns_lookup_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Hostname (e.g. google.com)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Traceroute") == 0) {
            keyboard_view_set_submit_callback(dual_comm_traceroute_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("Hostname or IP (e.g. 8.8.8.8)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "HTTP Request") == 0) {
            keyboard_view_set_submit_callback(dual_comm_http_request_kb_cb);
            display_manager_switch_view(&keyboard_view);
            keyboard_view_set_placeholder("URL (e.g. http://example.com or https://www.google.com)");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Sync NTP Time") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethntp");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Network Stats") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethstats");
            view_switched = true;
        } else if (strcmp(Selected_Option, "Show Config") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethconfig show");
        } else if (strcmp(Selected_Option, "ARP Poison") == 0) {
            terminal_set_return_view(&options_menu_view);
            terminal_set_dualcomm_filter(true);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend ethpoison start");
            view_switched = true;
        } else if (strcmp(Selected_Option, "USB Host On") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd on");
            view_switched = true;
        } else if (strcmp(Selected_Option, "USB Host Off") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd off");
            view_switched = true;
        } else if (strcmp(Selected_Option, "USB Host Status") == 0) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("commsend usbkbd status");
            view_switched = true;
        }

        if (!view_switched) {
            option_invoked = false;
        }
        return;
    }

#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    if (SelectedMenuType == OT_NRF24) {
        if (strcmp(Selected_Option, "Frequency Analyzer") == 0) {
            display_manager_switch_view(&nrf24_analyzer_view);
            view_switched = true;
        }

        if (!view_switched) {
            option_invoked = false;
        }
        return;
    }
#endif

#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    if (SelectedMenuType == OT_SubGhz) {
        if (strcmp(Selected_Option, "SubGHz") == 0) {
            display_manager_switch_view(&subghz_view);
            view_switched = true;
        }

        if (!view_switched) {
            option_invoked = false;
        }
        return;
    }
#endif

    if (SelectedMenuType == OT_Wifi) {
        if (current_wifi_menu_state == WIFI_MENU_MAIN) {
            if (strcmp(Selected_Option, "Attacks") == 0) current_wifi_menu_state = WIFI_MENU_ATTACKS;
            else if (strcmp(Selected_Option, "Recon") == 0) current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            else if (strcmp(Selected_Option, "Monitor") == 0) current_wifi_menu_state = WIFI_MENU_ENVIRONMENT;
            else if (strcmp(Selected_Option, "Network") == 0) current_wifi_menu_state = WIFI_MENU_NETWORK;
            else if (strcmp(Selected_Option, "Capture") == 0) current_wifi_menu_state = WIFI_MENU_CAPTURE;
            else if (strcmp(Selected_Option, "Evil Portal") == 0) current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
            else if (strcmp(Selected_Option, "DNS Sinkhole") == 0) current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE;
            else if (strcmp(Selected_Option, "Connection") == 0) current_wifi_menu_state = WIFI_MENU_CONNECTION;
            else if (strcmp(Selected_Option, "Gadgets") == 0) current_wifi_menu_state = WIFI_MENU_MISC;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_CAPTURE &&
            strcmp(Selected_Option, "Export Handshakes (hc22000)") == 0) {
            pcap_capture_page_offset = 0;
            current_wifi_menu_state = WIFI_MENU_CAPTURE_BROWSER;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (current_wifi_menu_state == WIFI_MENU_CAPTURE_BROWSER) {
            if (strcmp(Selected_Option, "No PCAP files found") == 0) {
                option_invoked = false;
                return;
            }
            if (strcmp(Selected_Option, "Next >") == 0) {
                pcap_capture_page_offset += PCAP_CAPTURE_PAGE_SIZE;
                rebuild_current_menu();
                option_invoked = false;
                return;
            }
            if (strcmp(Selected_Option, "< Prev") == 0) {
                pcap_capture_page_offset -= PCAP_CAPTURE_PAGE_SIZE;
                if (pcap_capture_page_offset < 0) pcap_capture_page_offset = 0;
                rebuild_current_menu();
                option_invoked = false;
                return;
            }

            const char *file_name = strchr(Selected_Option, ' ');
            file_name = file_name ? file_name + 1 : Selected_Option;

            bool jit_mounted = false;
            bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
            if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
                if (!sd_card_manager.is_initialized) {
                    if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                        jit_mounted = true;
                    }
                }
            }
#endif
            char out_path[MAX_FILE_NAME_LENGTH];
            int pmkid = 0;
            int handshakes = 0;
            esp_err_t err = pcap_export_hc22000(file_name, out_path, sizeof(out_path), &pmkid, &handshakes);
            if (jit_mounted) sd_card_unmount_after_flush(display_suspended);

            if (err == ESP_OK) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Exported: PMKID %d, M2/M3 %d", pmkid, handshakes);
                toast_show_duration(msg, TOAST_SUCCESS, 2500);
            } else if (err == ESP_ERR_NOT_FOUND) {
                toast_show_duration("No handshake found", TOAST_WARN, 2000);
            } else {
                toast_show_duration("Export failed", TOAST_ERROR, 2000);
            }
            option_invoked = false;
            return;
        }
    }

    // --- Bluetooth submenu navigation ---
    if (SelectedMenuType == OT_Bluetooth) {
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_MAIN) {
            if (strcmp(Selected_Option, "Detect Devices") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (!start_ble_detect_flow()) {
                    error_popup_create("Scan failed to start");
                }
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            if (strcmp(Selected_Option, "List Detected Devices") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (ble_device_detect_get_count() <= 0) {
                    error_popup_create("No detected devices");
                } else {
                    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
                    rebuild_current_menu();
                }
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            if (strcmp(Selected_Option, "Advertiser Scan") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (!start_ble_adv_flow()) {
                    error_popup_create("Scan failed to start");
                }
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            if (strcmp(Selected_Option, "OUI Device Scan") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                current_bluetooth_menu_state = BLUETOOTH_MENU_OUI;
                rebuild_current_menu();
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            if (strcmp(Selected_Option, "List Advertisers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (advertiser_scan_get_count() <= 0) {
                    error_popup_create("No advertisers found");
                } else {
                    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
                    rebuild_current_menu();
                }
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            if (strcmp(Selected_Option, "GATT Scan") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
                if (!start_ble_gatt_flow()) {
                    error_popup_create("Scan failed to start");
                }
                option_invoked = false;
                return;
#else
                error_popup_create("Device Does not Support Bluetooth...");
                option_invoked = false;
                return;
#endif
            }
            else if (strcmp(Selected_Option, "Aerial Detector") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_AERIAL;
            else if (strcmp(Selected_Option, "Spam") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_SPAM;
            else if (strcmp(Selected_Option, "Raw") == 0) current_bluetooth_menu_state = BLUETOOTH_MENU_RAW;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_OUI) {
        if (strcmp(Selected_Option, "Enter OUI Prefix") == 0) {
            keyboard_view_set_submit_callback(ble_oui_prefix_kb_cb);
            keyboard_view_set_placeholder("OUI prefix (e.g. 00:1A:2B)");
            display_manager_switch_view(&keyboard_view);
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Search Vendors") == 0) {
            keyboard_view_set_submit_callback(ble_oui_vendor_search_kb_cb);
            keyboard_view_set_placeholder("Vendor search (e.g. Apple)");
            display_manager_switch_view(&keyboard_view);
            option_invoked = false;
            return;
        }
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_OUI_VENDOR_LIST) {
        if (strcmp(Selected_Option, "Search Again") == 0) {
            keyboard_view_set_submit_callback(ble_oui_vendor_search_kb_cb);
            keyboard_view_set_placeholder("Vendor search (e.g. Apple)");
            display_manager_switch_view(&keyboard_view);
            option_invoked = false;
            return;
        }

        for (int i = 0; i < ble_oui_vendor_count; i++) {
            if (strcmp(Selected_Option, ble_oui_vendor_names[i]) == 0) {
                if (!start_ble_oui_vendor_flow(ble_oui_vendor_names[i])) {
                    error_popup_create("Scan failed to start");
                }
                option_invoked = false;
                return;
            }
        }

        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(ble_detect_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(ble_detect_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(ble_detect_list_menu);
        const char **opts = paged_menu_get_options(ble_detect_list_menu);
        int skip = paged_menu_has_prev(ble_detect_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_ble_detect_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(ble_adv_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(ble_adv_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(ble_adv_list_menu);
        const char **opts = paged_menu_get_options(ble_adv_list_menu);
        int skip = paged_menu_has_prev(ble_adv_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_ble_adv_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(ble_gatt_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(ble_gatt_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(ble_gatt_list_menu);
        const char **opts = paged_menu_get_options(ble_gatt_list_menu);
        int skip = paged_menu_has_prev(ble_gatt_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_ble_gatt_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    if (strcmp(Selected_Option, "Scan APs") == 0) {
        if (!start_ap_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }
    
    else if (vlist_kind_for_state() != VLIST_NONE && options_view_is_virtual(g_options_view)) {
        /* Windowed result lists dispatch taps through options_view's activate
         * callback, so there are no page rows or stable label strings here. */
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_STA_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(sta_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(sta_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(sta_list_menu);
        const char **opts = paged_menu_get_options(sta_list_menu);
        int skip = paged_menu_has_prev(sta_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (strcmp(opts[i], Selected_Option) == 0) {
                int idx = offset + (i - skip);
                show_station_detail(idx);
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_AP_MULTI_SELECT) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(ap_multi_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(ap_multi_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(ap_multi_menu);
        const char **opts = paged_menu_get_options(ap_multi_menu);
        int skip = paged_menu_has_prev(ap_multi_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                int idx = offset + (i - skip);
                ap_multi_select_toggle(idx);
                rebuild_current_menu();
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_STA_MULTI_SELECT) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(sta_multi_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(sta_multi_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(sta_multi_menu);
        const char **opts = paged_menu_get_options(sta_multi_menu);
        int skip = paged_menu_has_prev(sta_multi_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                int idx = offset + (i - skip);
                sta_multi_select_toggle(idx);
                rebuild_current_menu();
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_ARP_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(arp_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(arp_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(arp_list_menu);
        const char **opts = paged_menu_get_options(arp_list_menu);
        int skip = paged_menu_has_prev(arp_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_arp_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_GOVEE_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(govee_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(govee_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(govee_list_menu);
        const char **opts = paged_menu_get_options(govee_list_menu);
        int skip = paged_menu_has_prev(govee_list_menu) ? 1 : 0;
        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_govee_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_MDNS_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(mdns_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(mdns_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(mdns_list_menu);
        const char **opts = paged_menu_get_options(mdns_list_menu);
        int skip = paged_menu_has_prev(mdns_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_mdns_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_ENUM_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(enum_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(enum_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(enum_list_menu);
        const char **opts = paged_menu_get_options(enum_list_menu);
        int skip = paged_menu_has_prev(enum_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                show_enum_detail(offset + (i - skip));
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
        if (strcmp(Selected_Option, "No items found") == 0) {
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            paged_menu_page_prev(scanall_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "Next >") == 0) {
            paged_menu_page_next(scanall_list_menu);
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        int offset = paged_menu_get_page_offset(scanall_list_menu);
        const char **opts = paged_menu_get_options(scanall_list_menu);
        int skip = paged_menu_has_prev(scanall_list_menu) ? 1 : 0;

        for (int i = 0; opts[i]; i++) {
            if (opts[i] == Selected_Option || strcmp(opts[i], Selected_Option) == 0) {
                int row_idx = offset + (i - skip);
                scanall_select_row(row_idx);
                break;
            }
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Scan APs Live") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanap -live");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List APs") == 0) {
        uint16_t ap_count_local = ap_scan_get_count();
        if (ap_count_local > 0) {
            current_wifi_menu_state = WIFI_MENU_AP_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_ap_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Scan APs + Clients") == 0) {
        if (!start_scan_all_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Environment Sweep") == 0) {
        if (!start_sweep_flow()) {
            error_popup_create("Environment Sweep failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Deauth Attack") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -d");
        }
        view_switched = true; 
    }
    
    else if (strcmp(Selected_Option, "Handshake Capture + Deauth") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -hsd");
        }
        view_switched = true; 
    }
    
    else if (strcmp(Selected_Option, "Channel Switch (CSA) Attack") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -c");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Probe Request Flood") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -p");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Bad Msg Attack") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -b");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Auth Flood") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        if (!scanned_aps) {
            glog("No APs scanned. Please run 'Scan APs' first.\\n");
        } else {
            simulateCommand("attack -a");
        }
        view_switched = true; 
    }

    else if (strcmp(Selected_Option, "Scan Stations") == 0) {
        if (!start_station_scan_with_ap_scan()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List Stations") == 0) {
        int station_count_local = station_scan_get_count();
        if (station_count_local > 0) {
            if (sta_list_menu) {
                paged_menu_reset(sta_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_STA_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_station_scan_with_ap_scan()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List APs + Clients") == 0) {
        uint16_t ap_count_local = ap_scan_get_count();
        if (ap_count_local > 0) {
            if (scanall_list_menu) {
                paged_menu_reset(scanall_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_SCANALL_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_scan_all_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Multi-Select APs") == 0) {
        uint16_t ap_count_local = ap_scan_get_count();
        if (ap_count_local > 0) {
            ap_multi_select_cleanup();
            g_ap_multi_count = ap_count_local;
            g_ap_multi_selected = calloc(g_ap_multi_count, sizeof(bool));
            if (g_ap_multi_selected == NULL) {
                error_popup_create("Failed to allocate selection");
                g_ap_multi_count = 0;
            }
            current_wifi_menu_state = WIFI_MENU_AP_MULTI_SELECT;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        error_popup_create("No APs scanned");
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Wi-Fi Security Check") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        vTaskDelay(pdMS_TO_TICKS(100));
        wpa3_compliance_check_selected();
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Multi-Select Stations") == 0) {
        int sta_count_local = station_scan_get_count();
        if (sta_count_local > 0) {
            sta_multi_select_cleanup();
            g_sta_multi_count = sta_count_local;
            g_sta_multi_selected = calloc(g_sta_multi_count, sizeof(bool));
            if (g_sta_multi_selected == NULL) {
                error_popup_create("Failed to allocate selection");
                g_sta_multi_count = 0;
            }
            current_wifi_menu_state = WIFI_MENU_STA_MULTI_SELECT;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        error_popup_create("No stations scanned");
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Random") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - Rickroll") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("beaconspam -rr");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "mDNS Discovery") == 0) {
        if (!start_mdns_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "ARP Sweep") == 0) {
        if (!start_arp_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List Hosts (ARP)") == 0) {
        if (arp_scan_get_count() > 0) {
            if (arp_list_menu) {
                paged_menu_reset(arp_list_menu);
            }
            current_wifi_menu_state = WIFI_MENU_ARP_LIST;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }

        if (!start_arp_scan_flow()) {
            error_popup_create("Scan failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Beacon Spam - List") == 0) {
        if (scanned_aps) {
            terminal_set_return_view(&options_menu_view);
            display_manager_switch_view(&terminal_view);
            simulateCommand("beaconspam -l");
            view_switched = true;
        } else {
            error_popup_create("You Need to Scan AP's First...");
            
        }
    }

    else if (strcmp(Selected_Option, "Capture Deauth") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -deauth");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Probe") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -probe");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Beacon") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -beacon");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture Raw (Monitor)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -raw");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Capture EAPOL") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);

        simulateCommand("capture -eapol");
        view_switched = true;
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    else if (strcmp(Selected_Option, "Capture 802.15.4") == 0) {
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
        simulateCommand("capture -802154");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Capture 802.15.4 (Channel)") == 0) {
        keyboard_view_set_submit_callback(zigbee_capture_kb_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("Channel 11-26");
        return;
    }
#endif

    else if (strcmp(Selected_Option, "Listen for Probes") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listenprobes");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "EAPOL Logoff") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -e");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "GTK Isolation Bypass Test") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(gtk_abuse_ssid_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("Network SSID");
        return;
    }
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    else if (strcmp(Selected_Option, "SAE DoS Flood") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(sae_flood_password_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("Password");
        return;
    }
#endif

    else if (strcmp(Selected_Option, "Karma Attack") == 0) {
        wifi_manager_start_karma();
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        TERMINAL_VIEW_ADD_TEXT("Karma attack started\n");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Stop Karma Attack") == 0) {
        wifi_manager_stop_karma();
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        TERMINAL_VIEW_ADD_TEXT("Karma attack stopped\n");
        view_switched = true;
    }
    else if (strcmp(Selected_Option, "Karma Attack (Custom SSIDs)") == 0) {
        keyboard_view_set_submit_callback(karma_custom_ssids_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSID1,SSID2,SSID3");
        return;
    }
    else if (strcmp(Selected_Option, "Karma Attack (Custom Portal)") == 0) {
        portal_page_offset = 0;
        current_wifi_menu_state = WIFI_MENU_KARMA_PORTAL_SELECT;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT) {
        if (strcmp(Selected_Option, "No portal files found") == 0) {
            option_invoked = false;
            return;
        }
        /* Page navigation */
        if (strcmp(Selected_Option, "Next >") == 0) {
            portal_page_offset += PORTAL_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            portal_page_offset -= PORTAL_PAGE_SIZE;
            if (portal_page_offset < 0) portal_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        strncpy(selected_karma_portal, Selected_Option, MAX_PORTAL_NAME - 1);
        selected_karma_portal[MAX_PORTAL_NAME - 1] = '\0';
        keyboard_view_set_submit_callback(karma_portal_ssids_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSIDs (comma-sep, blank=auto)");
        return;
    }

    else if (strcmp(Selected_Option, "Capture WPS") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -wps");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TV Cast (Dial Connect)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("dialconnect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Wake on LAN") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(wol_kb_cb);
        keyboard_view_set_placeholder("MAC or IP (e.g. 192.168.1.10)");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Govee Lights") == 0) {
        current_wifi_menu_state = WIFI_MENU_GOVEE;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Scan Govee Devices") == 0) {
        if (!start_govee_scan_flow()) {
            error_popup_create("Connect to WiFi before scanning");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "List Govee Devices") == 0) {
        if (govee_scan_get_count() > 0) {
            if (govee_list_menu) paged_menu_reset(govee_list_menu);
            current_wifi_menu_state = WIFI_MENU_GOVEE_LIST;
            rebuild_current_menu();
        } else if (!start_govee_scan_flow()) {
            error_popup_create("Connect to WiFi before scanning");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Power Printer") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("powerprinter");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Evil Portal") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("startportal default FreeWiFi");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Evil Portal") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopportal");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Custom Evil Portal") == 0) {
        portal_page_offset = 0;
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL_SELECT;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        /* Non-selectable placeholder */
        if (strcmp(Selected_Option, "No portal files found") == 0) {
            option_invoked = false;
            return;
        }
        /* Page navigation */
        if (strcmp(Selected_Option, "Next >") == 0) {
            portal_page_offset += PORTAL_PAGE_SIZE;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        if (strcmp(Selected_Option, "< Prev") == 0) {
            portal_page_offset -= PORTAL_PAGE_SIZE;
            if (portal_page_offset < 0) portal_page_offset = 0;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        /* Prompt for SSID after selecting a portal file */
        strncpy(selected_portal, Selected_Option, MAX_PORTAL_NAME - 1);
        selected_portal[MAX_PORTAL_NAME - 1] = '\0';
        keyboard_view_set_submit_callback(evil_portal_ssid_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("SSID");
        return;
    }

    else if (strcmp(Selected_Option, "Sinkhole") == 0) {
        blocklist_page_offset = 0;
        blocklist_free_cache();
        const char **files = blocklist_load_page();
        if (files && files[0]) {
            current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE_FILE_PICK;
            rebuild_current_menu();
            option_invoked = false;
            return;
        }
        blocklist_free_cache();
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole start");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Sinkhole") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole stop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Sinkhole Status") == 0) {
        if (sinkhole_detail_view) {
            detail_view_destroy(sinkhole_detail_view);
            sinkhole_detail_view = NULL;
        }

        sinkhole_detail_view = detail_view_create(lv_scr_act(), "DNS Sinkhole");
        reserve_detail_touch_bar_space(sinkhole_detail_view);

        if (dns_sinkhole_is_running()) {
            uint32_t total = 0, blocked = 0;
            dns_sinkhole_get_stats(&total, &blocked);

            detail_view_add_info(sinkhole_detail_view, "Status", "Running");
            detail_view_add_infof(sinkhole_detail_view, "Queries", "%lu",
                                  (unsigned long)total);
            detail_view_add_infof(sinkhole_detail_view, "Blocked", "%lu",
                                  (unsigned long)blocked);
            if (total > 0) {
                char pct[16];
                snprintf(pct, sizeof(pct), "%.1f%%",
                         (float)blocked * 100.0f / (float)total);
                detail_view_add_info(sinkhole_detail_view, "Block Rate", pct);
            }
            detail_view_add_info(sinkhole_detail_view, "Logging",
                                 dns_sinkhole_get_logging() ? "ON" : "OFF");
        } else {
            detail_view_add_info(sinkhole_detail_view, "Status", "Stopped");
        }

        detail_view_add_info(sinkhole_detail_view, "Blocklist",
                             sd_card_exists(SINKHOLE_BLOCKLIST_PATH) ? "Present" : "None");

        detail_view_add_back(sinkhole_detail_view, sinkhole_detail_back_cb, NULL);
        current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
        update_scroll_buttons_visibility();
#endif
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Download Blocklist") == 0) {
        current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE_DOWNLOAD;
        rebuild_current_menu();
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "Toggle Logging") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole log");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Peter Lowe (3.5K ads)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole download 1");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "OISD Basic (40K mixed)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole download 2");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "StevenBlack (70K mixed)") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("sinkhole download 3");
        view_switched = true;
    }

    else if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_FILE_PICK &&
             strcmp(Selected_Option, "Next >") == 0) {
        blocklist_page_offset += BLOCKLIST_PAGE_SIZE;
        blocklist_free_cache();
        blocklist_load_page();
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_FILE_PICK &&
             strcmp(Selected_Option, "< Prev") == 0) {
        blocklist_page_offset -= BLOCKLIST_PAGE_SIZE;
        if (blocklist_page_offset < 0) blocklist_page_offset = 0;
        blocklist_free_cache();
        blocklist_load_page();
        rebuild_current_menu();
        option_invoked = false;
        return;
    }
    else if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_FILE_PICK &&
             Selected_Option && Selected_Option[0] != '\0') {
        char cmd[300];
        snprintf(cmd, sizeof(cmd), "sinkhole load %s", Selected_Option);
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand(cmd);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start Wardriving") == 0) {
        wardriving_view_set_scan_mode(true);
        display_manager_switch_view(&wardriving_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Wardriving") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand(wardriving_view_is_dual_mode() ? "dualwd -s" : "startwd -s");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Start AirTag Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -a");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Find Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -f");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    } else if (strcmp(Selected_Option, "List Flippers") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listflippers");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    } else if (strcmp(Selected_Option, "Select Flipper") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
         set_number_pad_mode(NP_MODE_FLIPPER);
         display_manager_switch_view(&number_pad_view);
         view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    } else if (strcmp(Selected_Option, "List AirTags") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listairtags");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    } else if (strcmp(Selected_Option, "Select AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        set_number_pad_mode(NP_MODE_AIRTAG);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    }

     else if (strcmp(Selected_Option, "Spoof Selected AirTag") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("spoofairtag");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("stopspoof");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }



    else if (strcmp(Selected_Option, "Capture Pwnagotchi") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -pwn");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "TP Link Test") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("tplinktest");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Raw BLE Scanner") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -r");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "BLE Skimmer Detect") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("capture -skimmer");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
        
#endif
    }

    else if (strcmp(Selected_Option, "Start GATT Scan") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blescan -g");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "List GATT Devices") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("listgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Select GATT Device") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        set_number_pad_mode(NP_MODE_GATT);
        display_manager_switch_view(&number_pad_view);
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Enumerate Services") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("enumgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Track Device") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("trackgatt");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Scan Aerial Devices") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialscan 60");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "List Aerial Devices") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aeriallist");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Track Aerial Device") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialtrack");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Aerial Scan") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialstop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Spoof Test Drone") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialspoof");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop Spoofing") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("aerialspoofstop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "GPS Info") == 0) {
        display_manager_switch_view(&wardriving_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Wardriving") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        wardriving_view_set_ble_mode(true);
        display_manager_switch_view(&wardriving_view);
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");

#endif
    }

    else if (strcmp(Selected_Option, "BLE + WiFi Wardriving") == 0) {
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(CONFIG_IDF_TARGET_ESP32P4)
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == 0) {
            error_popup_create("Dual Wardriving Requires PSRAM...");
        } else {
            wardriving_view_set_dual_mode(true);
            display_manager_switch_view(&wardriving_view);
            view_switched = true;
        }
#else
        error_popup_create("Device Does not Support Dual Wardriving...");

#endif
    }

    else if (strcmp(Selected_Option, "Airspace Monitor") == 0) {
        display_manager_switch_view(&airspace_monitor_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "PineAP Detection") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("pineap");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Flock Camera Detection") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("flockscan");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Scan Open Ports") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanports local -C");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "SSH Banner Scan") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanssh");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "NetBIOS Scan") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("netbiosscan");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "HTTP Banner Scan") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("httpbannerscan");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "SNMP Probe") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("snmpprobe");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "SMB Enum (enum4linux)") == 0) {
        if (!start_enum_scan_flow()) {
            error_popup_create("SMB enum failed to start");
        }
        option_invoked = false;
        return;
    }

    else if (strcmp(Selected_Option, "SNMP Walk") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("snmpprobe walk");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Packet Monitor") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("scanarp monitor");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Packet Visualizer") == 0) {
        display_manager_switch_view(&packet_monitor_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Hop Channels") == 0) {
        display_manager_switch_view(&hop_profile_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "NetBIOS Subnet...") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(netbios_subnet_kb_cb);
        keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "HTTP Banner Subnet...") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(http_banner_subnet_kb_cb);
        keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "SNMP Probe Subnet...") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(snmp_probe_subnet_kb_cb);
        keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "SNMP Walk Subnet...") == 0) {
        keyboard_view_set_return_view(&options_menu_view);
        keyboard_view_set_submit_callback(snmp_walk_subnet_kb_cb);
        keyboard_view_set_placeholder("Subnet prefix (e.g. 192.168.4.)");
        keyboard_view_set_initial_text("");
        display_manager_switch_view(&keyboard_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Reset AP Credentials") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("apcred -r");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Channel Congestion") == 0) {
        display_manager_switch_view(&channel_congestion_view);
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "DHCP Starvation") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("dhcpstarve start");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Stop DHCP-Starve") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("dhcpstarve stop");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Connect to WiFi") == 0) {
        keyboard_view_set_submit_callback(wifi_connect_kb_cb);
        display_manager_switch_view(&keyboard_view);
        keyboard_view_set_placeholder("\"SSID\" \"PASSWORD\"");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "Connect to saved WiFi") == 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("connect");
        view_switched = true;
    }

    else if (strcmp(Selected_Option, "BLE Spam - Apple") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -apple");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Microsoft") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -ms");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Samsung") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -samsung");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Google") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -google");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "BLE Spam - Random") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -random");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else if (strcmp(Selected_Option, "Stop BLE Spam") == 0) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("blespam -s");
        view_switched = true;
#else
        error_popup_create("Device Does not Support Bluetooth...");
#endif
    }

    else {
        ESP_LOGW(TAG, "Unhandled Option selected: %s\n", Selected_Option);
        
    }

    
    if (!view_switched) {
        option_invoked = false;
    }
}

void handle_option_directly(const char *Selected_Option) {
    if (is_settings_mode) {
        if (Selected_Option == (const char *)"__BACK_OPTION__") {
            // back is navigation, not a setting
            back_event_cb(NULL);
            return;
        }
        int row_data = (int)(intptr_t)Selected_Option;
        if (current_settings_root < 0) {
            switch_to_settings_root(row_data);
        } else if (current_settings_category < 0) {
            switch_to_settings_category(row_data);
        } else {
            change_setting_value(row_data, true);
        }
        return;
    }
    lv_event_t e;
    e.user_data = (void *)Selected_Option;
    option_event_cb(&e);
}

void options_menu_destroy() {
    if (!s_discard_resume_on_destroy && options_menu_view.root && lv_obj_is_valid(options_menu_view.root)) {
        s_resume_menu_state = options_menu_capture_nav_state();
    }
    s_discard_resume_on_destroy = false;
    s_rendered_menu_state.valid = false;
    free(s_info_cards);
    s_info_cards = NULL;
    opt_touch_started = false;
    popup_confirm_close(&settings_confirm_popup);
#if GHOSTESP_OTA_SUPPORTED
    ota_status_close_overlay();
    popup_confirm_close(&ota_result_popup);
#endif
    settings_select_close();
    /* Full-screen child views are temporary routes. Keep the logical options
     * ancestry so returning from Terminal/Keyboard can continue unwinding it. */
    scan_all_flow_active = false;
    scan_all_started_station_phase = false;
    station_scan_waiting_for_ap_scan = false;
    ap_list_cleanup();
    scanall_list_cleanup();
    station_list_cleanup();
    ble_detect_list_cleanup();
    arp_list_cleanup();
    govee_list_cleanup();
    mdns_list_cleanup();

    if (sweep_scan_status) {
        scan_status_close(sweep_scan_status);
        sweep_scan_status = NULL;
    }
    if (sweep_poll_timer) {
        lv_timer_del(sweep_poll_timer);
        sweep_poll_timer = NULL;
    }
    if (sweep_detail_view) {
        detail_view_destroy(sweep_detail_view);
        sweep_detail_view = NULL;
    }

    /* Detail views are parented to lv_scr_act(), not options_menu_view.root. */
    if (track_meter) {
        track_stop_current_source();
        track_source = TRACK_SRC_NONE;
        rssi_meter_destroy(track_meter);
        track_meter = NULL;
    }
    if (ap_detail_view) {
        detail_view_destroy(ap_detail_view);
        ap_detail_view = NULL;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
        ble_detect_detail_view = NULL;
    }
    if (ble_adv_detail_view) {
        detail_view_destroy(ble_adv_detail_view);
        ble_adv_detail_view = NULL;
    }
    if (sinkhole_detail_view) {
        detail_view_destroy(sinkhole_detail_view);
        sinkhole_detail_view = NULL;
    }
    if (arp_detail_view) {
        detail_view_destroy(arp_detail_view);
        arp_detail_view = NULL;
    }
    if (mdns_detail_view) {
        detail_view_destroy(mdns_detail_view);
        mdns_detail_view = NULL;
    }
    if (gtk_abuse_detail_view) {
        detail_view_destroy(gtk_abuse_detail_view);
        gtk_abuse_detail_view = NULL;
    }

    close_all_scan_status_overlays();

    gui_touch_bar_destroy(&s_opt_touch_tb);
    lvgl_obj_del_safe(&s_info_scroll);

    // Delete the root object (deletes all children recursively)
    lvgl_obj_del_safe(&options_menu_view.root);
    if (g_options_view) {
        options_view_destroy(g_options_view);
        g_options_view = NULL;
    }

    // Set all pointers to NULL
    menu_container = NULL;
    back_btn = NULL;
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    touch_bar = NULL;
    s_info_scroll = NULL;
    s_info_saved_menu_container = NULL;

    // Reset state variables
    selected_item_index = 0;
    num_items = 0;
    current_settings_root = -1;
    current_settings_category = -1;
    settings_submenu_depth = 0;
    s_info_detail_active = false;
    // note: wifi/bluetooth/dualcomm submenu states are intentionally NOT reset here
    // so when returning from terminal view, we resume at the correct submenu

    // Delete and clear any timers
    lvgl_timer_del_safe(&menu_build_timer);
    // Styles handled by options_view

    is_settings_mode = false;

    portal_page_offset = 0;
    portal_free_cache();

    wigle_csv_page_offset = 0;
    wigle_csv_browser_active = false;
    selected_wigle_csv[0] = '\0';
    wigle_csv_free_cache();
    wigle_manual_popup_close_cb(NULL);
    wigle_stats_popup_close_cb(NULL);

    if (g_freeze_hook_id > 0) {
        display_manager_unregister_freeze_pre_lock(g_freeze_hook_id);
        g_freeze_hook_id = -1;
    }
}

void options_menu_refresh_theme(void) {
    if (!options_menu_view.root || !lv_obj_is_valid(options_menu_view.root)) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));

    lv_obj_set_style_bg_color(options_menu_view.root, bg, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(options_menu_view.root,
                            asset_pack_get_background_tile() ? LV_OPA_TRANSP : LV_OPA_COVER,
                            LV_PART_MAIN);
    gui_screen_apply_background(options_menu_view.root);

    if (g_options_view) {
        options_view_refresh_styles(g_options_view);
        update_settings_arrows_visibility();
    }

    gui_touch_bar_refresh_styles(&s_opt_touch_tb);
}

void get_options_menu_callback(void **callback) { *callback = options_menu_view.input_callback; }

View options_menu_view = {.root = NULL,
                          .create = options_menu_create,
                          .destroy = options_menu_destroy,
                          .input_callback = handle_hardware_button_press_options,
                          .name = "Options Screen",
                          .get_hardwareinput_callback = get_options_menu_callback};

static void wigle_help_close_cb(lv_event_t *e) {
    (void)e;
    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        lvgl_obj_del_safe(&wigle_help_popup);
    }
    wigle_help_close_btn = NULL;
}

static void wigle_test_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    bool success = args[0];
    char *message = (char *)(&args[1]);
    wigle_set_test_callback(NULL);
    if (success) {
        error_popup_create(message);
    } else {
        error_popup_create(message);
    }
    free(data);
}

static void wigle_test_result_cb(bool success, const char *message) {
    // Must use lv_async_call since this runs in FreeRTOS task, not LVGL thread
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    display_manager_lvgl_async_call(wigle_test_result_async, args);
}

static void wigle_manual_upload_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    bool success = args[0];
    char *message = (char *)(&args[1]);
    wigle_set_manual_upload_callback(NULL);

    if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
        lv_label_set_text(wigle_manual_info_label, message);
    }
    if (success) {
        wigle_csv_free_cache();
        rebuild_current_menu();
    }
    free(data);
}

static void wigle_manual_upload_result_cb(bool success, const char *message) {
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    display_manager_lvgl_async_call(wigle_manual_upload_result_async, args);
}

static void wigle_stats_result_async(void *data) {
    uint8_t *args = (uint8_t *)data;
    (void)args[0];
    char *message = (char *)(&args[1]);
    wigle_set_stats_callback(NULL);

    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup) &&
        wigle_stats_body_label && lv_obj_is_valid(wigle_stats_body_label)) {
        lv_label_set_text(wigle_stats_body_label, message);
        if (wigle_stats_close_btn && lv_obj_is_valid(wigle_stats_close_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(wigle_stats_close_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Close");
        }
    }
    free(data);
}

static void wigle_stats_result_cb(bool success, const char *message) {
    size_t len = strlen(message) + 1;
    uint8_t *args = malloc(sizeof(bool) + len);
    if (!args) return;
    args[0] = success;
    memcpy(&args[1], message, len);
    display_manager_lvgl_async_call(wigle_stats_result_async, args);
}

static void back_event_cb(lv_event_t *e) {

    // Save settings when exiting options menu
    if (is_settings_mode) {
        settings_save(&G_Settings);
    }

    if (s_info_detail_active) {
        s_info_detail_active = false;
        free(s_info_cards);
        s_info_cards = NULL;
        if (s_info_scroll && lv_obj_is_valid(s_info_scroll)) {
            lv_obj_del(s_info_scroll);
        }
        s_info_scroll = NULL;
        if (s_info_saved_menu_container && lv_obj_is_valid(s_info_saved_menu_container)) {
            lv_obj_clear_flag(s_info_saved_menu_container, LV_OBJ_FLAG_HIDDEN);
            menu_container = s_info_saved_menu_container;
        }
        s_info_saved_menu_container = NULL;
        current_settings_root = -1;
        current_settings_category = -1;
        settings_submenu_depth = 0;
        rebuild_current_menu();
        return;
    }

    if (wigle_help_popup && lv_obj_is_valid(wigle_help_popup)) {
        wigle_help_close_cb(NULL);
        return;
    }
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        wigle_manual_popup_close_cb(NULL);
        return;
    }
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        wigle_stats_popup_close_cb(NULL);
        return;
    }

    if (SelectedMenuType == OT_WigleManualUpload || wigle_csv_browser_active) {
        wigle_csv_page_offset = 0;
        wigle_csv_browser_active = false;
        selected_wigle_csv[0] = '\0';
        wigle_csv_free_cache();
        SelectedMenuType = OT_Settings;
        is_settings_mode = true;
        current_settings_root = SETTINGS_ROOT_CAPTURE;
        current_settings_category = settings_category_index_for_id(SETTINGS_CAT_WIGLE);
        settings_submenu_depth = 2;
        rebuild_current_menu();
        return;
    }

    // If in Evil Portal select submenu, go back to Evil Portal menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT) {
        portal_page_offset = 0;
        portal_free_cache();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_EVIL_PORTAL;
        rebuild_current_menu();
        return;
    }
    // If in Karma portal select submenu, go back to Attacks menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT) {
        portal_page_offset = 0;
        portal_free_cache();
        selected_karma_portal[0] = '\0';
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_ATTACKS;
        rebuild_current_menu();
        return;
    }
    // If in AP details view, go back to AP list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
        ap_detail_back_cb(NULL);
        return;
    }
    // If in ARP host details view, go back to ARP list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ARP_DETAILS) {
        arp_detail_back_cb(NULL);
        return;
    }
    // If in station details view, go back to station list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
        station_detail_back_cb(NULL);
        return;
    }
    // If in GTK abuse status/result view, return to the attacks menu
    if (SelectedMenuType == OT_Wifi && (gtk_abuse_detail_view || gtk_abuse_status)) {
        gtk_abuse_back_cb(NULL);
        return;
    }
    // If in AP list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_LIST) {
        ap_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in station list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_LIST) {
        station_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in scan-all list view, go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
        scanall_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in ARP list view, go back to Network menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_GOVEE_DETAILS) {
        govee_detail_back_cb(NULL);
        return;
    }
    // If in Govee list view, go back to the Govee submenu.
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_GOVEE_LIST) {
        govee_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_GOVEE;
        rebuild_current_menu();
        return;
    }
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_GOVEE) {
        if (options_menu_restore_previous_state()) {
            return;
        }
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_MISC;
        rebuild_current_menu();
        return;
    }
    // Gadgets has direct child flows; do not replay a stale Govee state here.
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_MISC) {
        gui_nav_history_clear();
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in ARP list view, go back to Network menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ARP_LIST) {
        arp_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_NETWORK;
        rebuild_current_menu();
        return;
    }
    // If in capture browser, go back to Capture menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_CAPTURE_BROWSER) {
        pcap_capture_page_offset = 0;
        pcap_capture_free_cache();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_CAPTURE;
        rebuild_current_menu();
        return;
    }
    // If in AP multi-select view, confirm selection and go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_MULTI_SELECT) {
        ap_multi_select_confirm();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in station multi-select view, confirm selection and go back to Scan & Select menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_MULTI_SELECT) {
        sta_multi_select_confirm();
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    // If in Sweep results view, go back to the Environment menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ENVIRONMENT && sweep_detail_view) {
        sweep_detail_back_cb(NULL);
        return;
    }
    // If in mDNS detail view, go back to the mDNS list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_MDNS_DETAILS) {
        mdns_detail_back_cb(NULL);
        return;
    }
    // If in mDNS list view, go back to Network menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_MDNS_LIST) {
        mdns_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_NETWORK;
        rebuild_current_menu();
        return;
    }
    // If in Enum detail view, go back to the Enum list
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ENUM_DETAILS) {
        enum_detail_back_cb(NULL);
        return;
    }
    // If in Enum results list view, go back to Network menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ENUM_LIST) {
        enum_list_cleanup();
        if (options_menu_restore_previous_state()) {
            return;
        }
        s_skip_history_capture_once = true;
        current_wifi_menu_state = WIFI_MENU_NETWORK;
        rebuild_current_menu();
        return;
    }
    // If in a Wi-Fi submenu (but not main), go back to main Wi-Fi menu
    if (SelectedMenuType == OT_Wifi && current_wifi_menu_state != WIFI_MENU_MAIN) {
        if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DOWNLOAD) {
            if (options_menu_restore_previous_state()) {
                return;
            }
            current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE;
            rebuild_current_menu();
            return;
        }
        if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_FILE_PICK) {
            blocklist_free_cache();
            if (options_menu_restore_previous_state()) {
                return;
            }
            current_wifi_menu_state = WIFI_MENU_DNS_SINKHOLE;
            rebuild_current_menu();
            return;
        }
        if (current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_DETAILS) {
            sinkhole_detail_back_cb(NULL);
            return;
        }
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_wifi_menu_state = WIFI_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in BLE detail views, go back to the corresponding list
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
        ble_detect_detail_back_cb(NULL);
        return;
    }
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
        ble_adv_detail_back_cb(NULL);
        return;
    }
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
        ble_gatt_detail_back_cb(NULL);
        return;
    }
    // If in a Bluetooth submenu (but not main), go back to main Bluetooth menu
    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state != BLUETOOTH_MENU_MAIN) {
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_LIST ||
            current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_DETAILS) {
            ble_detect_list_cleanup();
        }
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_LIST ||
            current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_DETAILS) {
            ble_adv_list_cleanup();
        }
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_LIST ||
            current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_DETAILS) {
            ble_gatt_list_cleanup();
        }
        if (current_bluetooth_menu_state == BLUETOOTH_MENU_OUI_VENDOR_LIST) {
            ble_oui_vendor_clear();
        }
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in a Dual Comm submenu (but not main), go back to main Dual Comm menu
    if (SelectedMenuType == OT_DualComm && current_dualcomm_menu_state != DUALCOMM_MENU_MAIN) {
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_dualcomm_menu_state = DUALCOMM_MENU_MAIN;
        rebuild_current_menu();
        return;
    }
    // If in a settings submenu, go back to category selection
    if (is_settings_mode && current_settings_category >= 0) {
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_settings_category = -1;
        settings_submenu_depth = 1;
        rebuild_current_menu();
        return;
    }
    // If in a settings root section, go back to the settings root list
    if (is_settings_mode && current_settings_root >= 0) {
        if (options_menu_restore_previous_state()) {
            return;
        }
        current_settings_root = -1;
        current_settings_category = -1;
        settings_submenu_depth = 0;
        rebuild_current_menu();
        return;
    }
    // Otherwise, go back to main menu
    s_resume_menu_state.valid = false;
    s_pending_restore_state.valid = false;
    s_rendered_menu_state.valid = false;
    s_discard_resume_on_destroy = true;
    display_manager_go_back();
}

static void wigle_csv_free_cache(void) {
    if (wigle_csv_names) { free(wigle_csv_names); wigle_csv_names = NULL; }
    if (wigle_csv_options) { free(wigle_csv_options); wigle_csv_options = NULL; }
}

static const char **wigle_csv_load_page(void) {
    static const char *empty[] = {"No CSV files found", NULL};

    wigle_csv_free_cache();

    char (*file_names)[MAX_PORTAL_NAME] = malloc(WIGLE_CSV_PAGE_SIZE * MAX_PORTAL_NAME);
    if (!file_names) {
        ESP_LOGE(TAG, "wigle_csv_load_page: OOM for file name buffer");
        return empty;
    }

    int count = wigle_list_csv_files_paged(
        wigle_csv_page_offset,
        WIGLE_CSV_PAGE_SIZE,
        file_names,
        &wigle_csv_has_next_page);

    if (count < 0) {
        free(file_names);
        return empty;
    }

    bool show_prev = (wigle_csv_page_offset > 0);
    bool show_next = wigle_csv_has_next_page;
    int total = (show_prev ? 1 : 0) + count + (show_next ? 1 : 0);

    if (total == 0) {
        free(file_names);
        return empty;
    }

    wigle_csv_names = malloc(MAX_PORTAL_NAME * (size_t)total);
    wigle_csv_options = malloc(sizeof(char *) * ((size_t)total + 1));
    if (!wigle_csv_names || !wigle_csv_options) {
        free(file_names);
        wigle_csv_free_cache();
        return empty;
    }

    int idx = 0;
    if (show_prev) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, "< Prev");
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    for (int i = 0; i < count; i++) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, file_names[i]);
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_next) {
        strcpy(wigle_csv_names + idx * MAX_PORTAL_NAME, "Next >");
        wigle_csv_options[idx] = wigle_csv_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    wigle_csv_options[idx] = NULL;

    free(file_names);
    return wigle_csv_options;
}

static void pcap_capture_free_cache(void) {
    if (pcap_capture_names) { free(pcap_capture_names); pcap_capture_names = NULL; }
    if (pcap_capture_options) { free(pcap_capture_options); pcap_capture_options = NULL; }
}

static const char **pcap_capture_load_page(void) {
    static const char *empty[] = {"No PCAP files found", NULL};

    bool jit_mounted = false;
    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (!sd_card_manager.is_initialized) {
            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                jit_mounted = true;
            }
        }
    }
#endif

    pcap_capture_free_cache();

    static const char *pcap_dirs[] = {
        "/mnt/ghostesp/pcaps",
        "/mnt/ghostesp/ghostchi/pcaps",
    };
#define PCAP_NDIRS (sizeof(pcap_dirs) / sizeof(pcap_dirs[0]))

    int dir_counts[PCAP_NDIRS] = {0};
    int total_files = 0;

    for (int d = 0; d < (int)PCAP_NDIRS; d++) {
        DIR *dir = opendir(pcap_dirs[d]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len >= 6 && strcmp(entry->d_name + len - 5, ".pcap") == 0)
                dir_counts[d]++;
        }
        closedir(dir);
        total_files += dir_counts[d];
    }

    if (total_files == 0) {
        if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
        return empty;
    }

    bool show_prev = (pcap_capture_page_offset > 0);
    bool show_next = (pcap_capture_page_offset + PCAP_CAPTURE_PAGE_SIZE < total_files);
    int page_count = total_files - pcap_capture_page_offset;
    if (page_count > PCAP_CAPTURE_PAGE_SIZE) page_count = PCAP_CAPTURE_PAGE_SIZE;
    if (page_count < 0) page_count = 0;
    int total = (show_prev ? 1 : 0) + page_count + (show_next ? 1 : 0);

    if (total == 0) {
        if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
        return empty;
    }

    pcap_capture_names = malloc(MAX_FILE_NAME_LENGTH * (size_t)total);
    pcap_capture_options = malloc(sizeof(char *) * ((size_t)total + 1));
    if (!pcap_capture_names || !pcap_capture_options) {
        pcap_capture_free_cache();
        if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
        return empty;
    }

    int idx = 0;
    if (show_prev) {
        strcpy(pcap_capture_names + idx * MAX_FILE_NAME_LENGTH, "< Prev");
        pcap_capture_options[idx] = pcap_capture_names + idx * MAX_FILE_NAME_LENGTH;
        idx++;
    }

    int remaining = pcap_capture_page_offset;
    int filled = 0;
    for (int d = 0; d < (int)PCAP_NDIRS && filled < page_count; d++) {
        if (dir_counts[d] == 0) continue;
        if (remaining >= dir_counts[d]) {
            remaining -= dir_counts[d];
            continue;
        }
        char (*page_names)[MAX_PORTAL_NAME] = malloc(PCAP_CAPTURE_PAGE_SIZE * MAX_PORTAL_NAME);
        if (!page_names) break;
        int need = page_count - filled;
        int offset_in_dir = remaining;
        remaining = 0;
        int got = sd_card_list_dir_paged(pcap_dirs[d], ".pcap",
                                          offset_in_dir, need,
                                          page_names, NULL);
        if (got < 0) got = 0;
        for (int i = 0; i < got && filled < page_count; i++) {
            char full_path[MAX_FILE_NAME_LENGTH];
            snprintf(full_path, sizeof(full_path), "%s/%s", pcap_dirs[d], page_names[i]);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
            snprintf(pcap_capture_names + idx * MAX_FILE_NAME_LENGTH, MAX_FILE_NAME_LENGTH,
                     "%s %s", pcap_has_hc22000_material(full_path) ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, full_path);
#pragma GCC diagnostic pop
            pcap_capture_options[idx] = pcap_capture_names + idx * MAX_FILE_NAME_LENGTH;
            idx++;
            filled++;
        }
        free(page_names);
    }

    if (show_next) {
        strcpy(pcap_capture_names + idx * MAX_FILE_NAME_LENGTH, "Next >");
        pcap_capture_options[idx] = pcap_capture_names + idx * MAX_FILE_NAME_LENGTH;
        idx++;
    }
    pcap_capture_options[idx] = NULL;

    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
    return pcap_capture_options;
}

/* --- AP list as a windowed (virtual) list ------------------------------
 *
 * The AP list used to materialise one button per access point and page through
 * them with "< Prev" / "Next >" rows. It now describes its rows through
 * callbacks and lets options_view rebind a fixed row pool, so a 100-AP scan
 * costs the same LVGL objects as a 3-AP one and the batched build timer is not
 * needed at all.
 *
 * The trailing Back row is modelled as one extra item so encoder/joystick users
 * keep a reachable Back entry; touch-only boards rely on the touch bar instead,
 * matching the previous behaviour.
 */

static bool options_menu_needs_back_row(void) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    return true;
#else
    return screen_mirror_is_enabled();
#endif
}

/* Which result list, if any, the current menu state shows. */
static vlist_kind_t vlist_kind_for_state(void) {
    if (is_settings_mode) return VLIST_NONE;

    if (SelectedMenuType == OT_Wifi) {
        switch (current_wifi_menu_state) {
        /* The *_DETAILS states keep the list bound behind the detail view, so
         * they resolve to the same list kind as their *_LIST counterparts. */
        case WIFI_MENU_AP_LIST:
        case WIFI_MENU_AP_DETAILS:      return VLIST_AP;
        case WIFI_MENU_STA_LIST:
        case WIFI_MENU_STA_DETAILS:     return VLIST_STA;
        case WIFI_MENU_SCANALL_LIST:    return VLIST_SCANALL;
        case WIFI_MENU_ARP_LIST:
        case WIFI_MENU_ARP_DETAILS:     return VLIST_ARP;
        case WIFI_MENU_GOVEE_LIST:
        case WIFI_MENU_GOVEE_DETAILS:   return VLIST_GOVEE;
        case WIFI_MENU_MDNS_LIST:
        case WIFI_MENU_MDNS_DETAILS:    return VLIST_MDNS;
        case WIFI_MENU_ENUM_LIST:
        case WIFI_MENU_ENUM_DETAILS:    return VLIST_ENUM;
        default:                        return VLIST_NONE;
        }
    }

    if (SelectedMenuType == OT_Bluetooth) {
        switch (current_bluetooth_menu_state) {
        case BLUETOOTH_MENU_DETECT_LIST:
        case BLUETOOTH_MENU_DETECT_DETAILS: return VLIST_BLE_DETECT;
        case BLUETOOTH_MENU_ADV_LIST:
        case BLUETOOTH_MENU_ADV_DETAILS:    return VLIST_BLE_ADV;
        case BLUETOOTH_MENU_GATT_LIST:
        case BLUETOOTH_MENU_GATT_DETAILS:   return VLIST_BLE_GATT;
        default:                            return VLIST_NONE;
        }
    }

    return VLIST_NONE;
}

/* Real rows in a list, excluding the synthetic Back row. Each case mirrors the
 * count the old per-page loader walked. */
static int vlist_real_count(vlist_kind_t kind) {
    switch (kind) {
    case VLIST_AP:   return (int)ap_scan_get_count();
    case VLIST_STA:  return station_scan_get_count();
    case VLIST_ARP:  return arp_scan_get_count();
    case VLIST_GOVEE:return govee_scan_get_count();
    case VLIST_MDNS: return wifi_manager_ip_lookup_get_count();
    case VLIST_ENUM: return enum_scan_get_count();
    case VLIST_BLE_DETECT: return ble_device_detect_get_count();
    case VLIST_BLE_ADV:    return advertiser_scan_get_count();
    case VLIST_BLE_GATT:   return gatt_scan_get_device_count();
    case VLIST_SCANALL: {
        uint16_t ap_count = 0;
        wifi_ap_record_t *aps = NULL;
        ap_scan_get_results(&ap_count, &aps);
        if (!aps || ap_count == 0) return 0;
        return scanall_total_rows(ap_count, aps);
    }
    default: return 0;
    }
}

/* Format one row. These bodies are the old per-page loaders with the page
 * window removed: same accessors, same label text. */
static void vlist_format_row(vlist_kind_t kind, int index, char *out, size_t out_len) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));

    switch (kind) {
    case VLIST_AP: {
        uint16_t count = 0;
        wifi_ap_record_t *aps = NULL;
        ap_scan_get_results(&count, &aps);
        if (!aps || index < 0 || index >= (int)count) return;

        const char *band = (aps[index].primary >= 36) ? "5G" : "2.4G";
        if (aps[index].ssid[0] == 0) {
            snprintf(out, out_len, "Hidden Network %s %s Ch:%d#", color_code, band, aps[index].primary);
        } else {
            char ssid_trunc[28] = {0};
            strncpy(ssid_trunc, (const char *)aps[index].ssid, sizeof(ssid_trunc) - 1);
            snprintf(out, out_len, "%s %s %s Ch:%d#", ssid_trunc, color_code, band, aps[index].primary);
        }
    } break;

    case VLIST_STA: {
        int count = station_scan_get_count();
        if (index < 0 || index >= count) return;

        uint16_t ap_count = 0;
        wifi_ap_record_t *aps = NULL;
        ap_scan_get_results(&ap_count, &aps);

        char sta_mac[18];
        char sta_vendor[64] = {0};
        char ap_ssid[33];
        int ap_channel = 0;

        station_format_mac(station_ap_list[index].station_mac, sta_mac, sizeof(sta_mac));
        bool has_vendor = ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
        station_lookup_ap_ssid(station_ap_list[index].ap_bssid, ap_ssid, sizeof(ap_ssid));

        for (int j = 0; aps && j < (int)ap_count; j++) {
            if (memcmp(aps[j].bssid, station_ap_list[index].ap_bssid, 6) == 0) {
                ap_channel = aps[j].primary;
                break;
            }
        }

        const char *display_name = has_vendor ? sta_vendor : sta_mac;
        char display_name_trunc[40] = {0};
        char ap_ssid_trunc[28] = {0};
        strncpy(display_name_trunc, display_name, sizeof(display_name_trunc) - 1);
        strncpy(ap_ssid_trunc, ap_ssid, sizeof(ap_ssid_trunc) - 1);
        for (size_t k = 0; k < sizeof(ap_ssid_trunc) && ap_ssid_trunc[k] != '\0'; k++) {
            if (ap_ssid_trunc[k] == '#') ap_ssid_trunc[k] = '.';
        }

        if (ap_channel > 0) {
            snprintf(out, out_len, "%s -> %s%s Ch:%d#", display_name_trunc, color_code, ap_ssid_trunc, ap_channel);
        } else {
            snprintf(out, out_len, "%s -> %s%s#", display_name_trunc, color_code, ap_ssid_trunc);
        }
    } break;

    case VLIST_SCANALL: {
        uint16_t ap_count = 0;
        wifi_ap_record_t *aps = NULL;
        ap_scan_get_results(&ap_count, &aps);
        if (!aps || ap_count == 0) return;

        bool is_station_row = false;
        int ap_index = -1;
        int station_index = -1;
        if (!scanall_row_to_indices(index, ap_count, aps, &is_station_row, &ap_index, &station_index)) return;

        if (!is_station_row) {
            char ssid[33] = {0};
            if (aps[ap_index].ssid[0] == 0) {
                strncpy(ssid, "Hidden Network", sizeof(ssid) - 1);
            } else {
                strncpy(ssid, (const char *)aps[ap_index].ssid, sizeof(ssid) - 1);
            }
            sanitize_recolor_text(ssid);
            const char *band = (aps[ap_index].primary >= 36) ? "5G" : "2.4G";
            snprintf(out, out_len, "%.*s %sBand:%s Ch:%d#",
                     24, ssid, color_code, band, aps[ap_index].primary);
        } else if (station_index >= 0) {
            char sta_mac[18];
            char sta_vendor[64] = {0};
            station_format_mac(station_ap_list[station_index].station_mac, sta_mac, sizeof(sta_mac));
            const char *display_name = sta_mac;
            if (ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor)) && sta_vendor[0] != '\0') {
                sanitize_recolor_text(sta_vendor);
                display_name = sta_vendor;
            }
            snprintf(out, out_len, "-> %.*s", 36, display_name);
        } else {
            snprintf(out, out_len, "-> Unknown station");
        }
    } break;

    case VLIST_ARP: {
        const arp_host_t *host = arp_scan_get_host(index);
        if (!host) return;
        char mac_str[18];
        char vendor[64] = {0};
        format_mac_address(host->mac, mac_str, sizeof(mac_str), true);
        ouis_lookup_vendor(mac_str, vendor, sizeof(vendor));
        snprintf(out, out_len, "%s  %s", host->ip, vendor[0] ? vendor : mac_str);
    } break;

    case VLIST_GOVEE: {
        const govee_device_t *device = govee_scan_get_device(index);
        if (!device) return;
        snprintf(out, out_len, "%s  %s", device->sku[0] ? device->sku : "Govee Light", device->ip);
    } break;

    case VLIST_MDNS: {
        const mdns_device_t *dev = wifi_manager_ip_lookup_get_device(index);
        if (!dev) return;
        snprintf(out, out_len, "%s  %s", dev->hostname[0] ? dev->hostname : dev->ip, dev->ip);
    } break;

    case VLIST_ENUM: {
        const enum_host_t *host = enum_scan_get_host(index);
        if (!host) return;
        snprintf(out, out_len, "%s  %s", host->ip, host->hostname[0] ? host->hostname : "SMB");
    } break;

    case VLIST_BLE_DETECT: {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(index, &info) != 0) return;

        char title[48];
        const char *type = ble_device_detect_type_to_string(info.type);
        if (info.type == BLE_DETECT_DEVICE_FLIPPER && info.subtype[0] != '\0') {
            snprintf(title, sizeof(title), "%s %s", info.subtype, type);
        } else {
            snprintf(title, sizeof(title), "%s", type);
        }

        char label[40];
        if (info.name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", info.name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", info.mac[3], info.mac[4], info.mac[5]);
        }

        snprintf(out, out_len, "%s%s | %.*s | %d dBm",
                 info.tracking ? "* " : "", title, 20, label, info.rssi);
    } break;

    case VLIST_BLE_ADV: {
        AdvertiserDeviceInfo info;
        if (advertiser_scan_get_device(index, &info) != 0) return;

        char label[32];
        if (info.name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", info.name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", info.mac[3], info.mac[4], info.mac[5]);
        }

        snprintf(out, out_len, "%s | %.*s | %d dBm",
                 info.is_ibeacon ? "iBeacon" : info.adv_type, 20, label, info.rssi);
    } break;

    case VLIST_BLE_GATT: {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (gatt_scan_get_device_data(index, mac, &rssi, name, sizeof(name)) != 0) return;

        char label[32];
        if (name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
        }

        snprintf(out, out_len, "GATT | %.*s | %d dBm", 20, label, rssi);
    } break;

    default: break;
    }
}

static void vlist_activate_row(vlist_kind_t kind, int index) {
    switch (kind) {
    case VLIST_AP:         show_ap_detail(index); break;
    case VLIST_STA:        show_station_detail(index); break;
    case VLIST_SCANALL:    scanall_select_row(index); break;
    case VLIST_ARP:        show_arp_detail(index); break;
    case VLIST_GOVEE:      show_govee_detail(index); break;
    case VLIST_MDNS:       show_mdns_detail(index); break;
    case VLIST_ENUM:       show_enum_detail(index); break;
    case VLIST_BLE_DETECT: show_ble_detect_detail(index); break;
    case VLIST_BLE_ADV:    show_ble_adv_detail(index); break;
    case VLIST_BLE_GATT:   show_ble_gatt_detail(index); break;
    default: break;
    }
}

static int vlist_count(void *user_data) {
    (void)user_data;
    int n = vlist_real_count(vlist_kind_for_state());
    if (n <= 0) return 0; /* options_view renders "No items found" */
    return n + (options_menu_needs_back_row() ? 1 : 0);
}

static void vlist_fill(int index, char *out, size_t out_len, void *user_data) {
    (void)user_data;
    if (!out || out_len == 0) return;
    out[0] = '\0';

    vlist_kind_t kind = vlist_kind_for_state();
    if (index < 0) return;
    if (index >= vlist_real_count(kind)) {
        snprintf(out, out_len, LV_SYMBOL_LEFT " Back");
        return;
    }
    vlist_format_row(kind, index, out, out_len);
}

static void vlist_activate(int index, void *user_data) {
    (void)user_data;
    vlist_kind_t kind = vlist_kind_for_state();
    if (index < 0) return;
    if (index >= vlist_real_count(kind)) {
        back_event_cb(NULL);
        return;
    }
    vlist_activate_row(kind, index);
}

static void ap_list_cleanup(void) {
    bool had_ap_scan_ui = (ap_scan_poll_timer != NULL) || (ap_scan_status != NULL);

    if (ap_scan_poll_timer) {
        lv_timer_del(ap_scan_poll_timer);
        ap_scan_poll_timer = NULL;
    }
    if (ap_scan_status) {
        scan_status_close(ap_scan_status);
        ap_scan_status = NULL;
    }
    if (ap_detail_view) {
        detail_view_destroy(ap_detail_view);
        ap_detail_view = NULL;
    }
    if (had_ap_scan_ui && ap_scan_is_running()) {
        ap_scan_stop();
    }
}

/* (Re)bind the current result list as a windowed list. */
static void vlist_show(void) {
    if (!g_options_view) return;
    if (button_height_global > 0) {
        options_view_set_item_height(g_options_view, button_height_global);
    }
    options_view_virtual_start(g_options_view, vlist_count, vlist_fill, vlist_activate, NULL);
    num_items = options_view_get_item_count(g_options_view);
}

#define AP_MULTI_SELECT_PAGE_SIZE 10

static int ap_multi_select_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);

    if (!aps || count == 0 || g_ap_multi_selected == NULL) {
        *has_more = false;
        return 0;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    uint32_t accent_color = theme_palette_get_accent(theme);
    char muted_color_code[16];
    char accent_color_code[16];
    snprintf(muted_color_code, sizeof(muted_color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));
    snprintf(accent_color_code, sizeof(accent_color_code), "#%06X", (unsigned int)(accent_color & 0xFFFFFFu));

    int loaded = 0;
    for (int i = offset; i < (int)count && loaded < page_size; i++) {
        const char *band = (aps[i].primary >= 36) ? "5G" : "2.4G";

        if (aps[i].ssid[0] == 0) {
            if (g_ap_multi_selected[i]) {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s Hidden Network# %s %s Ch:%d#",
                         accent_color_code, muted_color_code, band, aps[i].primary);
            } else {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "Hidden Network %s %s Ch:%d#",
                         muted_color_code, band, aps[i].primary);
            }
        } else {
            char ssid_trunc[28] = {0};
            strncpy(ssid_trunc, (const char *)aps[i].ssid, sizeof(ssid_trunc) - 1);
            if (g_ap_multi_selected[i]) {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s %s# %s %s Ch:%d#",
                         accent_color_code, ssid_trunc, muted_color_code, band, aps[i].primary);
            } else {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s %s %s Ch:%d#",
                         ssid_trunc, muted_color_code, band, aps[i].primary);
            }
        }
        loaded++;
    }

    *has_more = (offset + loaded) < (int)count;
    return loaded;
}

static const char **ap_multi_select_get_options(void) {
    if (!ap_multi_menu) {
        ap_multi_menu = paged_menu_create(AP_MULTI_SELECT_PAGE_SIZE, ap_multi_select_load_fn, NULL);
        paged_menu_set_callbacks(ap_multi_menu, ap_multi_select_handle_selection, NULL, NULL, NULL);
    }
    return paged_menu_get_options(ap_multi_menu);
}

static void ap_multi_select_toggle(int ap_index) {
    if (g_ap_multi_selected == NULL || ap_index < 0 || ap_index >= g_ap_multi_count) {
        return;
    }
    g_ap_multi_selected[ap_index] = !g_ap_multi_selected[ap_index];
}

static void ap_multi_select_all(void) {
    if (g_ap_multi_selected == NULL) return;
    for (int i = 0; i < g_ap_multi_count; i++) {
        g_ap_multi_selected[i] = true;
    }
}

static void ap_multi_select_none(void) {
    if (g_ap_multi_selected == NULL) return;
    for (int i = 0; i < g_ap_multi_count; i++) {
        g_ap_multi_selected[i] = false;
    }
}

static void ap_multi_select_confirm(void) {
    if (g_ap_multi_selected == NULL) {
        ap_multi_select_cleanup();
        return;
    }

    int selected_count = 0;
    for (int i = 0; i < g_ap_multi_count; i++) {
        if (g_ap_multi_selected[i]) {
            selected_count++;
        }
    }

    if (selected_count > 0) {
        int *indices = malloc(selected_count * sizeof(int));
        if (indices != NULL) {
            int idx = 0;
            for (int i = 0; i < g_ap_multi_count; i++) {
                if (g_ap_multi_selected[i]) {
                    indices[idx++] = i;
                }
            }
            wifi_manager_select_multiple_aps(indices, selected_count);
            free(indices);
        }
    }

    ap_multi_select_cleanup();
}

static void ap_multi_select_cleanup(void) {
    if (ap_multi_menu) {
        paged_menu_destroy(ap_multi_menu);
        ap_multi_menu = NULL;
    }
    if (g_ap_multi_selected != NULL) {
        free(g_ap_multi_selected);
        g_ap_multi_selected = NULL;
    }
    g_ap_multi_count = 0;
}

static void ap_multi_select_back_cb(lv_event_t *e) {
    (void)e;
    ap_multi_select_confirm();
}

static void ap_multi_select_handle_selection(const char *option, void *user_data) {
    (void)user_data;

    if (strcmp(option, "< Prev") == 0) {
        paged_menu_page_prev(ap_multi_menu);
        rebuild_current_menu();
        return;
    }

    if (strcmp(option, "Next >") == 0) {
        paged_menu_page_next(ap_multi_menu);
        rebuild_current_menu();
        return;
    }

    uint16_t count = ap_scan_get_count();
    int page_offset = paged_menu_get_page_offset(ap_multi_menu);

    for (int i = 0; i < (int)count; i++) {
        char test_name[PAGED_MENU_NAME_MAX];
        (void)test_name;
        bool has_more = false;
        char names[1][PAGED_MENU_NAME_MAX];
        ap_multi_select_load_fn(page_offset + i, 1, names, &has_more, NULL);
        if (has_more == false && page_offset + i >= (int)count) {
            break;
        }
        if (strcmp(option, names[0]) == 0) {
            ap_multi_select_toggle(page_offset + i);
            rebuild_current_menu();
            return;
        }
    }
}

static void sanitize_recolor_text(char *text) {
    if (!text) {
        return;
    }
    for (char *p = text; *p; ++p) {
        if (*p == '#') {
            *p = '.';
        }
    }
}

static int scanall_get_station_count_for_ap(const uint8_t ap_bssid[6]) {
    int station_total = station_scan_get_count();
    int count = 0;
    for (int i = 0; i < station_total; i++) {
        if (memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) == 0) {
            count++;
        }
    }
    return count;
}

static bool scanall_get_station_for_ap_order(const uint8_t ap_bssid[6], int order, int *station_index_out) {
    if (!station_index_out || order < 0) {
        return false;
    }

    int station_total = station_scan_get_count();
    for (int i = 0; i < station_total; i++) {
        if (memcmp(station_ap_list[i].ap_bssid, ap_bssid, 6) != 0) {
            continue;
        }

        if (order == 0) {
            *station_index_out = i;
            return true;
        }
        order--;
    }

    return false;
}

static int scanall_total_rows(uint16_t ap_count, wifi_ap_record_t *aps) {
    int total_rows = 0;
    for (int i = 0; i < (int)ap_count; i++) {
        total_rows += 1 + scanall_get_station_count_for_ap(aps[i].bssid);
    }
    return total_rows;
}

static bool scanall_row_to_indices(int row_idx,
                                   uint16_t ap_count,
                                   wifi_ap_record_t *aps,
                                   bool *is_station_row_out,
                                   int *ap_index_out,
                                   int *station_index_out) {
    if (!aps || row_idx < 0 || !is_station_row_out || !ap_index_out || !station_index_out) {
        return false;
    }

    int cursor = 0;
    for (int ap_index = 0; ap_index < (int)ap_count; ap_index++) {
        if (cursor == row_idx) {
            *is_station_row_out = false;
            *ap_index_out = ap_index;
            *station_index_out = -1;
            return true;
        }
        cursor++;

        int station_count = scanall_get_station_count_for_ap(aps[ap_index].bssid);
        for (int order = 0; order < station_count; order++) {
            if (cursor == row_idx) {
                int station_index = -1;
                if (!scanall_get_station_for_ap_order(aps[ap_index].bssid, order, &station_index)) {
                    return false;
                }
                *is_station_row_out = true;
                *ap_index_out = ap_index;
                *station_index_out = station_index;
                return true;
            }
            cursor++;
        }
    }

    return false;
}

static void scanall_select_row(int row_idx) {
    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    bool is_station_row = false;
    int ap_index = -1;
    int station_index = -1;
    if (!scanall_row_to_indices(row_idx, ap_count, aps, &is_station_row, &ap_index, &station_index)) {
        error_popup_create("Item not found");
        return;
    }

    if (is_station_row) {
        show_station_detail(station_index);
    } else {
        show_ap_detail(ap_index);
    }
}

static int scanall_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    if (!aps || ap_count == 0) {
        *has_more = false;
        return 0;
    }

    int total_rows = scanall_total_rows(ap_count, aps);
    if (offset >= total_rows) {
        *has_more = false;
        return 0;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));

    int loaded = 0;
    int row_cursor = 0;
    for (int ap_index = 0; ap_index < (int)ap_count && loaded < page_size; ap_index++) {
        int station_count_for_ap = scanall_get_station_count_for_ap(aps[ap_index].bssid);

        char ssid[33] = {0};
        if (aps[ap_index].ssid[0] == 0) {
            strncpy(ssid, "Hidden Network", sizeof(ssid) - 1);
        } else {
            strncpy(ssid, (const char *)aps[ap_index].ssid, sizeof(ssid) - 1);
        }
        sanitize_recolor_text(ssid);

        const char *band = (aps[ap_index].primary >= 36) ? "5G" : "2.4G";

        if (row_cursor >= offset && loaded < page_size) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                     "%.*s %sBand:%s Ch:%d#",
                     24, ssid, color_code, band, aps[ap_index].primary);
            loaded++;
        }
        row_cursor++;

        for (int order = 0; order < station_count_for_ap && loaded < page_size; order++) {
            if (row_cursor >= offset) {
                int station_index = -1;
                if (scanall_get_station_for_ap_order(aps[ap_index].bssid, order, &station_index)) {
                    char sta_mac[18];
                    char sta_vendor[64] = {0};
                    station_format_mac(station_ap_list[station_index].station_mac, sta_mac, sizeof(sta_mac));

                    const char *display_name = sta_mac;
                    if (ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor)) && sta_vendor[0] != '\0') {
                        sanitize_recolor_text(sta_vendor);
                        display_name = sta_vendor;
                    }

                    snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                             "-> %.*s",
                             36, display_name);
                } else {
                    snprintf(names[loaded], PAGED_MENU_NAME_MAX,
                             "-> Unknown station");
                }
                loaded++;
            }
            row_cursor++;
        }
    }

    *has_more = (offset + loaded) < total_rows;
    return loaded;
}

static void scanall_list_cleanup(void) {
    if (scanall_list_menu) {
        paged_menu_destroy(scanall_list_menu);
        scanall_list_menu = NULL;
    }
}

static const char **scanall_list_get_options(void) {
    if (!scanall_list_menu) {
        scanall_list_menu = paged_menu_create(SCANALL_LIST_PAGE_SIZE, scanall_list_load_fn, NULL);
    }
    return paged_menu_get_options(scanall_list_menu);
}

static void ble_detect_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    int count = ble_device_detect_get_count();
    if (count == ble_detect_last_count) {
        if (!ble_device_detect_is_active()) {
            stop_ble_detect_flow();
        }
        return;
    }

    ble_detect_last_count = count;
    ble_detect_set_subtext(count);
    if (ble_detect_list_menu) {
        paged_menu_reset(ble_detect_list_menu);
    }

    if (!ble_device_detect_is_active()) {
        stop_ble_detect_flow();
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_DETECT_LIST) {
        rebuild_current_menu();
    }
}

static int ble_detect_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                                   bool *has_more, void *user_data) {
    (void)user_data;

    int count = ble_device_detect_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) != 0) {
            continue;
        }

        char title[48];
        const char *type = ble_device_detect_type_to_string(info.type);
        if (info.type == BLE_DETECT_DEVICE_FLIPPER && info.subtype[0] != '\0') {
            snprintf(title, sizeof(title), "%s %s", info.subtype, type);
        } else {
            snprintf(title, sizeof(title), "%s", type);
        }

        char label[40];
        if (info.name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", info.name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", info.mac[3], info.mac[4], info.mac[5]);
        }

        snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s%s | %.*s | %d dBm",
                 info.tracking ? "* " : "", title, 20, label, info.rssi);
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static void ble_detect_list_cleanup(void) {
    if (ble_detect_poll_timer) {
        lv_timer_del(ble_detect_poll_timer);
        ble_detect_poll_timer = NULL;
    }
    if (ble_detect_status) {
        scan_status_close(ble_detect_status);
        ble_detect_status = NULL;
    }
    if (ble_detect_list_menu) {
        paged_menu_destroy(ble_detect_list_menu);
        ble_detect_list_menu = NULL;
    }
    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
        ble_detect_detail_view = NULL;
    }

    selected_ble_detect_index = -1;
    ble_detect_last_count = -1;
    ble_device_detect_stop_tracking();
    if (ble_device_detect_is_active()) {
        ble_device_detect_stop();
    }
    ble_device_detect_clear_results();
}

static const char **ble_detect_list_get_options(void) {
    if (!ble_detect_list_menu) {
        ble_detect_list_menu = paged_menu_create(BLE_DETECT_LIST_PAGE_SIZE, ble_detect_list_load_fn, NULL);
    }
    return paged_menu_get_options(ble_detect_list_menu);
}

static bool start_ble_detect_flow(void) {
    ble_detect_list_cleanup();
    ble_device_detect_start();
    if (!ble_device_detect_is_active()) {
        return false;
    }

    ble_detect_status = scan_status_create("Detecting BLE Devices");
    ble_detect_set_subtext(0);
    ble_detect_last_count = ble_device_detect_get_count();
    ble_detect_poll_timer = lv_timer_create(ble_detect_poll_timer_cb, 750, NULL);
    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
    return true;
}

static void stop_ble_detect_flow(void) {
    if (ble_device_detect_is_active()) {
        ble_device_detect_stop();
    }
    if (ble_detect_poll_timer) {
        lv_timer_del(ble_detect_poll_timer);
        ble_detect_poll_timer = NULL;
    }
    if (ble_detect_status) {
        scan_status_close(ble_detect_status);
        ble_detect_status = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
    if (ble_detect_list_menu) {
        paged_menu_reset(ble_detect_list_menu);
    }

    if (ble_device_detect_get_count() <= 0) {
        error_popup_create("No BLE devices found");
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
    }

    rebuild_current_menu();
}

static void ble_detect_track_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_detect_index < 0) {
        error_popup_create("Track failed");
        return;
    }

    /* Build the subtext label (name, else MAC) before tearing down the detail. */
    char target_label[24] = {0};
    BLEDetectDeviceInfo info;
    if (ble_device_detect_get_device(selected_ble_detect_index, &info) == 0) {
        if (info.name[0] != '\0') {
            strncpy(target_label, info.name, sizeof(target_label) - 1);
        } else {
            snprintf(target_label, sizeof(target_label), "%02X:%02X:%02X:%02X:%02X:%02X",
                     info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        }
    }

    if (!ble_device_detect_start_tracking(selected_ble_detect_index)) {
        error_popup_create("Track failed");
        return;
    }

    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
        ble_detect_detail_view = NULL;
    }

    selected_ble_detect_index = -1;
    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
    start_track_overlay(TRACK_SRC_BLE_DETECT, "Track Detect", target_label,
                        track_meter_sample_ble_detect);
}

static void ble_detect_spoof_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_detect_index < 0 ||
        !ble_device_detect_start_airtag_spoof(selected_ble_detect_index)) {
        error_popup_create("Spoof failed");
        return;
    }

    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
        ble_detect_detail_view = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
}

static void ble_detect_detail_back_cb(lv_event_t *e) {
    (void)e;

    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
        ble_detect_detail_view = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_LIST;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void show_ble_detect_detail(int device_index) {
    BLEDetectDeviceInfo info;
    if (ble_device_detect_get_device(device_index, &info) != 0) {
        error_popup_create("Device not found");
        return;
    }

    selected_ble_detect_index = device_index;

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    if (ble_detect_detail_view) {
        detail_view_destroy(ble_detect_detail_view);
    }
    ble_detect_detail_view = detail_view_create(lv_scr_act(), NULL);
    reserve_detail_touch_bar_space(ble_detect_detail_view);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", info.mac[0], info.mac[1],
             info.mac[2], info.mac[3], info.mac[4], info.mac[5]);

    detail_view_add_info(ble_detect_detail_view, "Type", ble_device_detect_type_to_string(info.type));
    if (info.subtype[0] != '\0') {
        detail_view_add_info(ble_detect_detail_view, "Variant", info.subtype);
    }
    if (info.name[0] != '\0') {
        detail_view_add_info(ble_detect_detail_view, "Name", info.name);
    }
    detail_view_add_info(ble_detect_detail_view, "MAC", mac);
    detail_view_add_infof(ble_detect_detail_view, "RSSI", "%d dBm", info.rssi);
    detail_view_add_info(ble_detect_detail_view, "Actions:", "");
    detail_view_add_action(ble_detect_detail_view, "Track", ble_detect_track_cb, NULL);
    if (info.type == BLE_DETECT_DEVICE_AIRTAG) {
        detail_view_add_action(ble_detect_detail_view, "Spoof", ble_detect_spoof_cb, NULL);
    }
    detail_view_add_back(ble_detect_detail_view, ble_detect_detail_back_cb, NULL);

    current_bluetooth_menu_state = BLUETOOTH_MENU_DETECT_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ble_adv_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    int count = advertiser_scan_get_count();
    if (count == ble_adv_last_count) {
        if (!advertiser_scan_is_active()) {
            stop_ble_adv_flow();
        }
        return;
    }

    ble_adv_last_count = count;
    ble_adv_set_subtext(count);
    if (ble_adv_list_menu) {
        paged_menu_reset(ble_adv_list_menu);
    }

    if (!advertiser_scan_is_active()) {
        stop_ble_adv_flow();
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_ADV_LIST) {
        rebuild_current_menu();
    }
}

static int ble_adv_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                                bool *has_more, void *user_data) {
    (void)user_data;

    int count = advertiser_scan_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        AdvertiserDeviceInfo info;
        if (advertiser_scan_get_device(i, &info) != 0) {
            continue;
        }

        char label[32];
        if (info.name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", info.name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", info.mac[3], info.mac[4], info.mac[5]);
        }

        snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s | %.*s | %d dBm",
                 info.is_ibeacon ? "iBeacon" : info.adv_type, 20, label, info.rssi);
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static void ble_adv_list_cleanup(void) {
    if (ble_adv_poll_timer) {
        lv_timer_del(ble_adv_poll_timer);
        ble_adv_poll_timer = NULL;
    }
    if (ble_adv_status) {
        scan_status_close(ble_adv_status);
        ble_adv_status = NULL;
    }
    if (ble_adv_list_menu) {
        paged_menu_destroy(ble_adv_list_menu);
        ble_adv_list_menu = NULL;
    }
    if (ble_adv_detail_view) {
        detail_view_destroy(ble_adv_detail_view);
        ble_adv_detail_view = NULL;
    }

    selected_ble_adv_index = -1;
    ble_adv_last_count = -1;
    if (advertiser_scan_is_tracking()) {
        advertiser_scan_stop_tracking();
    }
    if (advertiser_scan_is_active()) {
        advertiser_scan_stop();
    }
}

static const char **ble_adv_list_get_options(void) {
    if (!ble_adv_list_menu) {
        ble_adv_list_menu = paged_menu_create(BLE_ADV_LIST_PAGE_SIZE, ble_adv_list_load_fn, NULL);
    }
    return paged_menu_get_options(ble_adv_list_menu);
}

static bool start_ble_adv_flow_with_filter(const uint8_t *oui, const char *vendor) {
    ble_adv_list_cleanup();
    if (oui != NULL) {
        advertiser_scan_start_oui_prefix(oui);
    } else if (vendor != NULL && vendor[0] != '\0') {
        advertiser_scan_start_vendor(vendor);
    } else {
        advertiser_scan_start();
    }
    if (!advertiser_scan_is_active()) {
        return false;
    }

    ble_adv_status = scan_status_create(advertiser_scan_is_filtered()
                                            ? "Scanning OUI Devices"
                                            : "Scanning BLE Advertisers");
    ble_adv_set_subtext(0);
    ble_adv_last_count = advertiser_scan_get_count();
    ble_adv_poll_timer = lv_timer_create(ble_adv_poll_timer_cb, 750, NULL);
    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
    return true;
}

static bool start_ble_adv_flow(void) {
    return start_ble_adv_flow_with_filter(NULL, NULL);
}

static bool start_ble_oui_prefix_flow(const uint8_t oui[3]) {
    return start_ble_adv_flow_with_filter(oui, NULL);
}

static bool start_ble_oui_vendor_flow(const char *vendor) {
    return start_ble_adv_flow_with_filter(NULL, vendor);
}

static void stop_ble_adv_flow(void) {
    if (advertiser_scan_is_active()) {
        advertiser_scan_stop();
    }
    if (ble_adv_poll_timer) {
        lv_timer_del(ble_adv_poll_timer);
        ble_adv_poll_timer = NULL;
    }
    if (ble_adv_status) {
        scan_status_close(ble_adv_status);
        ble_adv_status = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
    if (ble_adv_list_menu) {
        paged_menu_reset(ble_adv_list_menu);
    }

    if (advertiser_scan_get_count() <= 0) {
        error_popup_create(advertiser_scan_is_filtered() ? "No matching devices found" : "No advertisers found");
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
    }

    rebuild_current_menu();
}

static void ble_oui_vendor_clear(void) {
    free(ble_oui_vendor_names);
    ble_oui_vendor_names = NULL;
    memset(ble_oui_vendor_options, 0, sizeof(ble_oui_vendor_options));
    ble_oui_vendor_count = 0;
}

static bool ble_oui_vendor_collect_cb(const char *vendor, void *user_data) {
    (void)user_data;
    if (vendor == NULL || ble_oui_vendor_names == NULL ||
        ble_oui_vendor_count >= BLE_OUI_VENDOR_MAX_RESULTS) {
        return false;
    }

    strncpy(ble_oui_vendor_names[ble_oui_vendor_count], vendor,
            sizeof(ble_oui_vendor_names[ble_oui_vendor_count]) - 1);
    ble_oui_vendor_names[ble_oui_vendor_count][sizeof(ble_oui_vendor_names[ble_oui_vendor_count]) - 1] = '\0';
    ble_oui_vendor_options[ble_oui_vendor_count] = ble_oui_vendor_names[ble_oui_vendor_count];
    ble_oui_vendor_count++;
    return ble_oui_vendor_count < BLE_OUI_VENDOR_MAX_RESULTS;
}

static const char **ble_oui_vendor_list_get_options(void) {
    if (ble_oui_vendor_count <= 0) {
        static const char *fallback[] = {"Search Again", NULL};
        return fallback;
    }

    ble_oui_vendor_options[ble_oui_vendor_count] = "Search Again";
    ble_oui_vendor_options[ble_oui_vendor_count + 1] = NULL;
    return ble_oui_vendor_options;
}

static void ble_oui_prefix_kb_cb(const char *text) {
    uint8_t oui[3];
    keyboard_view_set_submit_callback(NULL);

    if (!ouis_parse_prefix(text, oui)) {
        error_popup_create("Invalid OUI prefix");
        return;
    }

    SelectedMenuType = OT_Bluetooth;
    current_bluetooth_menu_state = BLUETOOTH_MENU_OUI;
    display_manager_switch_view(&options_menu_view);
    if (!start_ble_oui_prefix_flow(oui)) {
        error_popup_create("Scan failed to start");
    }
}

static void ble_oui_vendor_search_kb_cb(const char *text) {
    keyboard_view_set_submit_callback(NULL);

    if (text == NULL || text[0] == '\0') {
        error_popup_create("Enter vendor search text");
        return;
    }

    ble_oui_vendor_clear();
    ble_oui_vendor_names = calloc(BLE_OUI_VENDOR_MAX_RESULTS, sizeof(*ble_oui_vendor_names));
    if (!ble_oui_vendor_names) {
        error_popup_create("Not enough memory");
        return;
    }
    ouis_foreach_unique_vendor(text, ble_oui_vendor_collect_cb, NULL, BLE_OUI_VENDOR_MAX_RESULTS);
    if (ble_oui_vendor_count <= 0) {
        error_popup_create("No vendors found");
        return;
    }

    SelectedMenuType = OT_Bluetooth;
    current_bluetooth_menu_state = BLUETOOTH_MENU_OUI_VENDOR_LIST;
    display_manager_switch_view(&options_menu_view);
}

static void ble_adv_detail_back_cb(lv_event_t *e) {
    (void)e;

    if (ble_adv_detail_view) {
        detail_view_destroy(ble_adv_detail_view);
        ble_adv_detail_view = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ble_adv_track_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_adv_index < 0) {
        error_popup_create("Track failed");
        return;
    }

    /* Build the subtext label (name, else MAC) before tearing down the detail. */
    char target_label[24] = {0};
    AdvertiserDeviceInfo info;
    if (advertiser_scan_get_device(selected_ble_adv_index, &info) == 0) {
        if (info.name[0] != '\0') {
            strncpy(target_label, info.name, sizeof(target_label) - 1);
        } else {
            snprintf(target_label, sizeof(target_label), "%02X:%02X:%02X:%02X:%02X:%02X",
                     info.mac[0], info.mac[1], info.mac[2], info.mac[3], info.mac[4], info.mac[5]);
        }
    }

    if (!advertiser_scan_start_tracking(selected_ble_adv_index)) {
        error_popup_create("Track failed");
        return;
    }

    if (ble_adv_detail_view) {
        detail_view_destroy(ble_adv_detail_view);
        ble_adv_detail_view = NULL;
    }

    selected_ble_adv_index = -1;
    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_LIST;
    start_track_overlay(TRACK_SRC_BLE_ADV, "Track Adv", target_label,
                        track_meter_sample_ble_adv);
}

static void ble_adv_save_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_adv_index < 0) {
        error_popup_create("Save failed");
        return;
    }

    if (!advertiser_scan_save_to_sd(selected_ble_adv_index)) {
        error_popup_create("Save failed");
        return;
    }
}

static void show_ble_adv_detail(int device_index) {
    AdvertiserDeviceInfo info;
    if (advertiser_scan_get_device(device_index, &info) != 0) {
        error_popup_create("Advertiser not found");
        return;
    }

    selected_ble_adv_index = device_index;

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    if (ble_adv_detail_view) {
        detail_view_destroy(ble_adv_detail_view);
    }
    ble_adv_detail_view = detail_view_create(lv_scr_act(), NULL);
    reserve_detail_touch_bar_space(ble_adv_detail_view);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", info.mac[0], info.mac[1],
             info.mac[2], info.mac[3], info.mac[4], info.mac[5]);

    detail_view_add_info(ble_adv_detail_view, "Type", info.is_ibeacon ? "iBeacon" : "BLE Advertiser");
    if (info.name[0] != '\0') {
        detail_view_add_info(ble_adv_detail_view, "Name", info.name);
    }
    detail_view_add_info(ble_adv_detail_view, "MAC", mac);
    detail_view_add_info(ble_adv_detail_view, "Address Type", info.addr_type == 0 ? "Public" : "Random");
    detail_view_add_infof(ble_adv_detail_view, "RSSI", "%d dBm", info.rssi);
    detail_view_add_info(ble_adv_detail_view, "Adv Type", info.adv_type);
    detail_view_add_infof(ble_adv_detail_view, "Seen", "%lu", (unsigned long)info.seen_count);
    if (info.has_flags) {
        detail_view_add_infof(ble_adv_detail_view, "Flags", "0x%02X", info.flags);
    }
    if (info.has_tx_power) {
        detail_view_add_infof(ble_adv_detail_view, "TX Power", "%d dBm", info.tx_power);
    }
    if (info.oui_vendor[0] != '\0') {
        detail_view_add_info(ble_adv_detail_view, "OUI Vendor", info.oui_vendor);
    }
    if (info.manufacturer[0] != '\0') {
        detail_view_add_info(ble_adv_detail_view, "Manufacturer", info.manufacturer);
    }
    if (info.has_appearance) {
        detail_view_add_infof(ble_adv_detail_view, "Appearance", "0x%04X", info.appearance);
    }
    if (info.services[0] != '\0') {
        detail_view_add_info(ble_adv_detail_view, "Services", info.services);
    }
    if (info.service_data[0] != '\0') {
        detail_view_add_info(ble_adv_detail_view, "Service Data", info.service_data);
    }
    if (info.is_ibeacon) {
        detail_view_add_info(ble_adv_detail_view, "iBeacon UUID", info.ibeacon_uuid);
        detail_view_add_infof(ble_adv_detail_view, "Major", "%u", info.ibeacon_major);
        detail_view_add_infof(ble_adv_detail_view, "Minor", "%u", info.ibeacon_minor);
        detail_view_add_infof(ble_adv_detail_view, "Measured Power", "%d dBm",
                              info.ibeacon_measured_power);
    }
    detail_view_add_info(ble_adv_detail_view, "Actions:", "");
    detail_view_add_action(ble_adv_detail_view, "Track", ble_adv_track_cb, NULL);
    detail_view_add_action(ble_adv_detail_view, "Save to SD", ble_adv_save_cb, NULL);
    detail_view_add_back(ble_adv_detail_view, ble_adv_detail_back_cb, NULL);

    current_bluetooth_menu_state = BLUETOOTH_MENU_ADV_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ble_gatt_set_subtext(int count) {
    if (!ble_gatt_status) {
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%d device%s found", count, count == 1 ? "" : "s");
    scan_status_set_subtext(ble_gatt_status, buf);
}

static void ble_gatt_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    int count = gatt_scan_get_device_count();
    if (count == ble_gatt_last_count) {
        if (!gatt_scan_is_active()) {
            stop_ble_gatt_flow();
        }
        return;
    }

    ble_gatt_last_count = count;
    ble_gatt_set_subtext(count);
    if (ble_gatt_list_menu) {
        paged_menu_reset(ble_gatt_list_menu);
    }

    if (!gatt_scan_is_active()) {
        stop_ble_gatt_flow();
        return;
    }

    if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_GATT_LIST) {
        rebuild_current_menu();
    }
}

static int ble_gatt_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX],
                                 bool *has_more, void *user_data) {
    (void)user_data;

    int count = gatt_scan_get_device_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        uint8_t mac[6];
        int8_t rssi;
        char name[32];
        if (gatt_scan_get_device_data(i, mac, &rssi, name, sizeof(name)) != 0) {
            continue;
        }

        char label[32];
        if (name[0] != '\0') {
            snprintf(label, sizeof(label), "%s", name);
        } else {
            snprintf(label, sizeof(label), "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
        }

        snprintf(names[loaded], PAGED_MENU_NAME_MAX, "GATT | %.*s | %d dBm", 20, label, rssi);
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static void ble_gatt_list_cleanup(void) {
    if (ble_gatt_poll_timer) {
        lv_timer_del(ble_gatt_poll_timer);
        ble_gatt_poll_timer = NULL;
    }
    if (ble_gatt_status) {
        scan_status_close(ble_gatt_status);
        ble_gatt_status = NULL;
    }
    if (ble_gatt_list_menu) {
        paged_menu_destroy(ble_gatt_list_menu);
        ble_gatt_list_menu = NULL;
    }
    if (ble_gatt_detail_view) {
        detail_view_destroy(ble_gatt_detail_view);
        ble_gatt_detail_view = NULL;
    }

    selected_ble_gatt_index = -1;
    ble_gatt_last_count = -1;
    if (gatt_scan_is_active()) {
        gatt_scan_stop();
    }
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (ble_is_initialized()) {
        ble_stop();
    }
#endif
}

static const char **ble_gatt_list_get_options(void) {
    if (!ble_gatt_list_menu) {
        ble_gatt_list_menu = paged_menu_create(BLE_GATT_LIST_PAGE_SIZE, ble_gatt_list_load_fn, NULL);
    }
    return paged_menu_get_options(ble_gatt_list_menu);
}

static bool start_ble_gatt_flow(void) {
    ble_gatt_list_cleanup();
    gatt_scan_start();
    if (!gatt_scan_is_active()) {
        return false;
    }

    ble_gatt_status = scan_status_create("Scanning GATT Devices");
    ble_gatt_set_subtext(0);
    ble_gatt_last_count = gatt_scan_get_device_count();
    ble_gatt_poll_timer = lv_timer_create(ble_gatt_poll_timer_cb, 750, NULL);
    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
    return true;
}

static void stop_ble_gatt_flow(void) {
    if (gatt_scan_is_active()) {
        gatt_scan_stop();
    }
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (ble_is_initialized()) {
        ble_stop();
    }
#endif
    if (ble_gatt_poll_timer) {
        lv_timer_del(ble_gatt_poll_timer);
        ble_gatt_poll_timer = NULL;
    }
    if (ble_gatt_status) {
        scan_status_close(ble_gatt_status);
        ble_gatt_status = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
    if (ble_gatt_list_menu) {
        paged_menu_reset(ble_gatt_list_menu);
    }

    if (gatt_scan_get_device_count() <= 0) {
        error_popup_create("No GATT devices found");
        current_bluetooth_menu_state = BLUETOOTH_MENU_MAIN;
    }

    rebuild_current_menu();
}

static void ble_gatt_detail_back_cb(lv_event_t *e) {
    (void)e;

    if (ble_gatt_detail_view) {
        detail_view_destroy(ble_gatt_detail_view);
        ble_gatt_detail_view = NULL;
    }

    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ble_gatt_track_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_gatt_index < 0) {
        error_popup_create("Track failed");
        return;
    }

    /* Build the subtext label (name, else MAC) before tearing down the detail. */
    char target_label[24] = {0};
    uint8_t mac_bytes[6];
    int8_t rssi;
    char name[32];
    if (gatt_scan_get_device_data(selected_ble_gatt_index, mac_bytes, &rssi, name,
                                  sizeof(name)) == 0) {
        if (name[0] != '\0') {
            strncpy(target_label, name, sizeof(target_label) - 1);
        } else {
            snprintf(target_label, sizeof(target_label), "%02X:%02X:%02X:%02X:%02X:%02X",
                     mac_bytes[0], mac_bytes[1], mac_bytes[2], mac_bytes[3], mac_bytes[4],
                     mac_bytes[5]);
        }
    }

    gatt_scan_select_device(selected_ble_gatt_index);
    gatt_scan_track_device();

    if (ble_gatt_detail_view) {
        detail_view_destroy(ble_gatt_detail_view);
        ble_gatt_detail_view = NULL;
    }

    selected_ble_gatt_index = -1;
    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
    start_track_overlay(TRACK_SRC_BLE_GATT, "Track GATT", target_label,
                        track_meter_sample_ble_gatt);
}

static void ble_gatt_enum_cb(lv_event_t *e) {
    (void)e;

    if (selected_ble_gatt_index < 0) {
        error_popup_create("Enumerate failed");
        return;
    }

    gatt_scan_select_device(selected_ble_gatt_index);
    if (ble_gatt_detail_view) {
        detail_view_destroy(ble_gatt_detail_view);
        ble_gatt_detail_view = NULL;
    }
    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_LIST;
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("enumgatt");
}

static void show_ble_gatt_detail(int device_index) {
    uint8_t mac_bytes[6];
    int8_t rssi;
    char name[32];
    if (gatt_scan_get_device_data(device_index, mac_bytes, &rssi, name, sizeof(name)) != 0) {
        error_popup_create("GATT device not found");
        return;
    }

    selected_ble_gatt_index = device_index;

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    if (ble_gatt_detail_view) {
        detail_view_destroy(ble_gatt_detail_view);
    }
    ble_gatt_detail_view = detail_view_create(lv_scr_act(), NULL);
    reserve_detail_touch_bar_space(ble_gatt_detail_view);

    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac_bytes[0], mac_bytes[1],
             mac_bytes[2], mac_bytes[3], mac_bytes[4], mac_bytes[5]);

    detail_view_add_info(ble_gatt_detail_view, "Type", "GATT Device");
    if (name[0] != '\0') {
        detail_view_add_info(ble_gatt_detail_view, "Name", name);
    }
    detail_view_add_info(ble_gatt_detail_view, "MAC", mac);
    detail_view_add_infof(ble_gatt_detail_view, "RSSI", "%d dBm", rssi);
    detail_view_add_info(ble_gatt_detail_view, "Actions:", "");
    detail_view_add_action(ble_gatt_detail_view, "Enumerate Services", ble_gatt_enum_cb, NULL);
    detail_view_add_action(ble_gatt_detail_view, "Track", ble_gatt_track_cb, NULL);
    detail_view_add_back(ble_gatt_detail_view, ble_gatt_detail_back_cb, NULL);

    current_bluetooth_menu_state = BLUETOOTH_MENU_GATT_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void station_format_mac(const uint8_t mac[6], char *out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void station_lookup_ap_ssid(const uint8_t ap_bssid[6], char *ssid_out, size_t ssid_out_size) {
    if (!ssid_out || ssid_out_size == 0) {
        return;
    }

    strncpy(ssid_out, "(Unknown AP)", ssid_out_size - 1);
    ssid_out[ssid_out_size - 1] = '\0';

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);

    if (!aps || count == 0) {
        return;
    }

    for (int i = 0; i < (int)count; i++) {
        if (memcmp(aps[i].bssid, ap_bssid, 6) == 0) {
            if (aps[i].ssid[0] == 0) {
                strncpy(ssid_out, "<Hidden>", ssid_out_size - 1);
                ssid_out[ssid_out_size - 1] = '\0';
            } else {
                strncpy(ssid_out, (const char *)aps[i].ssid, ssid_out_size - 1);
                ssid_out[ssid_out_size - 1] = '\0';
            }
            return;
        }
    }
}

static bool station_lookup_ap_channel_rssi(const uint8_t ap_bssid[6], int *channel_out, int *rssi_out) {
    if (!channel_out || !rssi_out) {
        return false;
    }

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    if (!aps || count == 0) {
        return false;
    }

    for (int i = 0; i < (int)count; i++) {
        if (memcmp(aps[i].bssid, ap_bssid, 6) == 0) {
            *channel_out = aps[i].primary;
            *rssi_out = aps[i].rssi;
            return true;
        }
    }

    return false;
}

static int sta_list_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    int count = station_scan_get_count();
    if (count <= 0) {
        *has_more = false;
        return 0;
    }

    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    char color_code[16];
    snprintf(color_code, sizeof(color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        char sta_mac[18];
        char sta_vendor[64] = {0};
        char ap_ssid[33];
        int ap_channel = 0;

        station_format_mac(station_ap_list[i].station_mac, sta_mac, sizeof(sta_mac));
        bool has_vendor = ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
        station_lookup_ap_ssid(station_ap_list[i].ap_bssid, ap_ssid, sizeof(ap_ssid));

        for (int j = 0; j < (int)ap_count; j++) {
            if (memcmp(aps[j].bssid, station_ap_list[i].ap_bssid, 6) == 0) {
                ap_channel = aps[j].primary;
                break;
            }
        }

        const char *display_name = has_vendor ? sta_vendor : sta_mac;
        char display_name_trunc[40] = {0};
        char ap_ssid_trunc[28] = {0};
        strncpy(display_name_trunc, display_name, sizeof(display_name_trunc) - 1);
        strncpy(ap_ssid_trunc, ap_ssid, sizeof(ap_ssid_trunc) - 1);

        for (size_t k = 0; k < sizeof(ap_ssid_trunc) && ap_ssid_trunc[k] != '\0'; k++) {
            if (ap_ssid_trunc[k] == '#') {
                ap_ssid_trunc[k] = '.';
            }
        }

        if (ap_channel > 0) {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s Ch:%d#",
                     display_name_trunc, color_code, ap_ssid_trunc, ap_channel);
        } else {
            snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s#",
                     display_name_trunc, color_code, ap_ssid_trunc);
        }
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static void station_list_cleanup(void) {
    bool had_station_flow_state = (sta_scan_poll_timer != NULL) || (sta_scan_status != NULL) ||
                                  (sta_list_menu != NULL) || (sta_detail_view != NULL);
    sta_scan_stopped_by_user = false;

    if (sta_scan_poll_timer) {
        lv_timer_del(sta_scan_poll_timer);
        sta_scan_poll_timer = NULL;
    }
    if (sta_list_menu) {
        paged_menu_destroy(sta_list_menu);
        sta_list_menu = NULL;
    }
    if (sta_scan_status) {
        scan_status_close(sta_scan_status);
        sta_scan_status = NULL;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    if (had_station_flow_state && station_scan_is_active()) {
        station_scan_stop();
    }
}

static const char **sta_list_get_options(void) {
    if (!sta_list_menu) {
        sta_list_menu = paged_menu_create(STA_LIST_PAGE_SIZE, sta_list_load_fn, NULL);
    }
    return paged_menu_get_options(sta_list_menu);
}

#define STA_MULTI_SELECT_PAGE_SIZE 10

static int sta_multi_select_load_fn(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data) {
    (void)user_data;

    int count = station_scan_get_count();
    if (count <= 0 || g_sta_multi_selected == NULL) {
        *has_more = false;
        return 0;
    }

    uint16_t ap_count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_count, &aps);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t muted_color = theme_palette_get_text_muted(theme);
    uint32_t accent_color = theme_palette_get_accent(theme);
    char muted_color_code[16];
    char accent_color_code[16];
    snprintf(muted_color_code, sizeof(muted_color_code), "#%06X", (unsigned int)(muted_color & 0xFFFFFFu));
    snprintf(accent_color_code, sizeof(accent_color_code), "#%06X", (unsigned int)(accent_color & 0xFFFFFFu));

    int loaded = 0;
    for (int i = offset; i < count && loaded < page_size; i++) {
        char sta_mac[18];
        char sta_vendor[64] = {0};
        char ap_ssid[33];
        int ap_channel = 0;

        station_format_mac(station_ap_list[i].station_mac, sta_mac, sizeof(sta_mac));
        bool has_vendor = ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
        station_lookup_ap_ssid(station_ap_list[i].ap_bssid, ap_ssid, sizeof(ap_ssid));

        for (int j = 0; j < (int)ap_count; j++) {
            if (memcmp(aps[j].bssid, station_ap_list[i].ap_bssid, 6) == 0) {
                ap_channel = aps[j].primary;
                break;
            }
        }

        const char *display_name = has_vendor ? sta_vendor : sta_mac;
        char display_name_trunc[28] = {0};
        char ap_ssid_trunc[20] = {0};
        strncpy(display_name_trunc, display_name, sizeof(display_name_trunc) - 1);
        strncpy(ap_ssid_trunc, ap_ssid, sizeof(ap_ssid_trunc) - 1);

        for (size_t k = 0; k < sizeof(ap_ssid_trunc) && ap_ssid_trunc[k] != '\0'; k++) {
            if (ap_ssid_trunc[k] == '#') {
                ap_ssid_trunc[k] = '.';
            }
        }

        if (ap_channel > 0) {
            if (g_sta_multi_selected[i]) {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s %s# -> %s%s Ch:%d#",
                         accent_color_code, display_name_trunc, muted_color_code, ap_ssid_trunc, ap_channel);
            } else {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s Ch:%d#",
                         display_name_trunc, muted_color_code, ap_ssid_trunc, ap_channel);
            }
        } else {
            if (g_sta_multi_selected[i]) {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s %s# -> %s%s#",
                         accent_color_code, display_name_trunc, muted_color_code, ap_ssid_trunc);
            } else {
                snprintf(names[loaded], PAGED_MENU_NAME_MAX, "%s -> %s%s#",
                         display_name_trunc, muted_color_code, ap_ssid_trunc);
            }
        }
        loaded++;
    }

    *has_more = (offset + loaded) < count;
    return loaded;
}

static const char **sta_multi_select_get_options(void) {
    if (!sta_multi_menu) {
        sta_multi_menu = paged_menu_create(STA_MULTI_SELECT_PAGE_SIZE, sta_multi_select_load_fn, NULL);
        paged_menu_set_callbacks(sta_multi_menu, sta_multi_select_handle_selection, NULL, NULL, NULL);
    }
    return paged_menu_get_options(sta_multi_menu);
}

static void sta_multi_select_toggle(int sta_index) {
    if (g_sta_multi_selected == NULL || sta_index < 0 || sta_index >= g_sta_multi_count) {
        return;
    }
    g_sta_multi_selected[sta_index] = !g_sta_multi_selected[sta_index];
}

static void sta_multi_select_all(void) {
    if (g_sta_multi_selected == NULL) return;
    for (int i = 0; i < g_sta_multi_count; i++) {
        g_sta_multi_selected[i] = true;
    }
}

static void sta_multi_select_none(void) {
    if (g_sta_multi_selected == NULL) return;
    for (int i = 0; i < g_sta_multi_count; i++) {
        g_sta_multi_selected[i] = false;
    }
}

static void sta_multi_select_confirm(void) {
    if (g_sta_multi_selected == NULL) {
        sta_multi_select_cleanup();
        return;
    }

    int selected_count = 0;
    for (int i = 0; i < g_sta_multi_count; i++) {
        if (g_sta_multi_selected[i]) {
            selected_count++;
        }
    }

    if (selected_count > 0) {
        int *indices = malloc(selected_count * sizeof(int));
        if (indices != NULL) {
            int idx = 0;
            for (int i = 0; i < g_sta_multi_count; i++) {
                if (g_sta_multi_selected[i]) {
                    indices[idx++] = i;
                }
            }
            station_scan_select_multiple(indices, selected_count);
            free(indices);
        }
    }

    sta_multi_select_cleanup();
}

static void sta_multi_select_cleanup(void) {
    if (sta_multi_menu) {
        paged_menu_destroy(sta_multi_menu);
        sta_multi_menu = NULL;
    }
    if (g_sta_multi_selected != NULL) {
        free(g_sta_multi_selected);
        g_sta_multi_selected = NULL;
    }
    g_sta_multi_count = 0;
}

static void sta_multi_select_back_cb(lv_event_t *e) {
    (void)e;
    sta_multi_select_confirm();
}

static void sta_multi_select_handle_selection(const char *option, void *user_data) {
    (void)user_data;

    if (strcmp(option, "< Prev") == 0) {
        paged_menu_page_prev(sta_multi_menu);
        rebuild_current_menu();
        return;
    }

    if (strcmp(option, "Next >") == 0) {
        paged_menu_page_next(sta_multi_menu);
        rebuild_current_menu();
        return;
    }

    int count = station_scan_get_count();
    int page_offset = paged_menu_get_page_offset(sta_multi_menu);

    for (int i = 0; i < count; i++) {
        char names[1][PAGED_MENU_NAME_MAX];
        bool has_more = false;
        sta_multi_select_load_fn(page_offset + i, 1, names, &has_more, NULL);
        if (has_more == false && page_offset + i >= count) {
            break;
        }
        if (strcmp(option, names[0]) == 0) {
            sta_multi_select_toggle(page_offset + i);
            rebuild_current_menu();
            return;
        }
    }
}

static bool multi_select_option_is_toggled(int option_index, const char *option) {
    if (!option || strcmp(option, "< Prev") == 0 || strcmp(option, "Next >") == 0 || strcmp(option, "No items found") == 0) {
        return false;
    }

    if (SelectedMenuType != OT_Wifi) {
        return false;
    }

    if (current_wifi_menu_state == WIFI_MENU_AP_MULTI_SELECT && ap_multi_menu && g_ap_multi_selected) {
        int skip = paged_menu_has_prev(ap_multi_menu) ? 1 : 0;
        int idx = paged_menu_get_page_offset(ap_multi_menu) + (option_index - skip);
        return idx >= 0 && idx < g_ap_multi_count && g_ap_multi_selected[idx];
    }

    if (current_wifi_menu_state == WIFI_MENU_STA_MULTI_SELECT && sta_multi_menu && g_sta_multi_selected) {
        int skip = paged_menu_has_prev(sta_multi_menu) ? 1 : 0;
        int idx = paged_menu_get_page_offset(sta_multi_menu) + (option_index - skip);
        return idx >= 0 && idx < g_sta_multi_count && g_sta_multi_selected[idx];
    }

    return false;
}

static void style_multi_select_row(lv_obj_t *btn, bool toggled) {
    if (!btn || !toggled) {
        return;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    lv_obj_set_style_bg_color(btn, accent, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

static const char *auth_mode_to_string(wifi_auth_mode_t mode) {
    switch (mode) {
        case WIFI_AUTH_OPEN: return "Open";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
        case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3-PSK";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "Unknown";
    }
}

static void ap_deauth_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_AP_LIST;
        suppress_wifi_state_reset_once = true;
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -d");
    }
}

static void ap_hs_deauth_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_AP_LIST;
        suppress_wifi_state_reset_once = true;
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -hsd");
    }
}

static void ap_probe_flood_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_AP_LIST;
        suppress_wifi_state_reset_once = true;
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -p");
    }
}

static void ap_bad_msg_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_AP_LIST;
        suppress_wifi_state_reset_once = true;
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -b");
    }
}

static void ap_auth_flood_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        ap_scan_select(selected_ap_index);
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }
        current_wifi_menu_state = WIFI_MENU_AP_LIST;
        suppress_wifi_state_reset_once = true;
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand("attack -a");
    }
}

/* Sampler for the live RSSI ring: pulls the latest tracking RSSI from the wifi
 * manager. Returns true when the reading is fresh (a recent matching packet). */
static bool track_meter_sample(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!wifi_manager_get_track_status(out_rssi, &fresh)) {
        return false;
    }
    return fresh;
}

/* Sampler for the BLE advertiser tracker: pulls the latest RSSI from the
 * advertiser scan. Returns true when a matching advertisement arrived recently. */
static bool track_meter_sample_ble_adv(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!advertiser_scan_get_track_status(out_rssi, &fresh)) {
        return false;
    }
    return fresh;
}

/* Sampler for the BLE GATT tracker: pulls the latest RSSI from the GATT scan. */
static bool track_meter_sample_ble_gatt(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!gatt_scan_get_track_status(out_rssi, &fresh)) {
        return false;
    }
    return fresh;
}

/* Sampler for the BLE detect tracker: pulls the latest RSSI from the detect
 * scan. Returns true when a matching advertisement arrived recently. */
static bool track_meter_sample_ble_detect(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!ble_device_detect_get_track_status(out_rssi, &fresh)) {
        return false;
    }
    return fresh;
}

/* Stop whichever hardware tracker currently feeds the RSSI ring overlay. */
static void track_stop_current_source(void) {
    switch (track_source) {
        case TRACK_SRC_WIFI_AP:
        case TRACK_SRC_WIFI_STA:
            wifi_manager_stop_tracking();
            break;
        case TRACK_SRC_BLE_ADV:
            advertiser_scan_stop_tracking();
            break;
        case TRACK_SRC_BLE_GATT:
            gatt_scan_stop_tracking();
            break;
        case TRACK_SRC_BLE_DETECT:
            ble_device_detect_stop_tracking();
            break;
        default:
            break;
    }
}

/* Which inputs leave the tracking view (mirrors the detail view's back keys). */
static bool track_exit_requested(const InputEvent *event) {
    if (!event) return false;
    switch (event->type) {
        case INPUT_TYPE_EXIT_BUTTON:
            return true;
        case INPUT_TYPE_JOYSTICK:
            return event->data.joystick_index == 0 || event->data.joystick_index == 1;
        case INPUT_TYPE_ENCODER:
            return event->data.encoder.button;
        case INPUT_TYPE_KEYBOARD: {
            uint8_t k = event->data.key_value;
            return k == LV_KEY_LEFT || k == LV_KEY_ESC || k == LV_KEY_ENTER ||
                   k == 13 || k == 'h' || k == '`' || k == 29 || k == ',' || k == 44;
        }
        default:
            return false;
    }
}

/* Launch the pulsating RSSI ring overlay for an already-started hardware
 * tracker. The options menu stays the current view (like the scan spinner /
 * detail view), so the shared touch bar remains available underneath. The
 * caller must have started the tracker named by `src` before calling. */
static void start_track_overlay(track_source_t src, const char *status_title,
                                const char *target_label,
                                rssi_meter_sample_cb sampler) {
    if (track_meter) {
        rssi_meter_destroy(track_meter);
        track_meter = NULL;
    }

    track_source = src;

    track_meter = rssi_meter_create(lv_scr_act(), status_title, target_label,
                                    sampler, NULL);
    if (!track_meter) {
        /* Allocation failed: don't leave hardware tracking running headless. */
        track_stop_current_source();
        track_source = TRACK_SRC_NONE;
        error_popup_create("Track failed");
        return;
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    if (touch_bar && lv_obj_is_valid(touch_bar)) {
        rssi_meter_set_bottom_reserved(track_meter, lv_obj_get_height(touch_bar));
    }
    /* Only the Back button is meaningful while tracking. */
    if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_add_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
    if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_add_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    if (back_btn && lv_obj_is_valid(back_btn)) lv_obj_move_foreground(back_btn);
#endif
}

/* Start the wifi AP/STA tracker and bring up the shared RSSI ring overlay. */
static void start_track_meter(bool is_ap, const char *target_label) {
    if (is_ap) {
        wifi_manager_track_ap();
    } else {
        wifi_manager_track_sta();
    }
    start_track_overlay(is_ap ? TRACK_SRC_WIFI_AP : TRACK_SRC_WIFI_STA,
                        is_ap ? "Track AP" : "Track STA",
                        target_label, track_meter_sample);
}

/* Stop tracking, tear down the ring overlay, and return to the list we came
 * from (the detail view was already destroyed at launch; its back handler
 * restores the list + touch bar). */
static void stop_track_flow(void) {
    track_source_t src = track_source;
    track_stop_current_source();
    track_source = TRACK_SRC_NONE;
    if (track_meter) {
        rssi_meter_destroy(track_meter);
        track_meter = NULL;
    }
    switch (src) {
        case TRACK_SRC_WIFI_STA:
            station_detail_back_cb(NULL);
            break;
        case TRACK_SRC_BLE_ADV:
            ble_adv_detail_back_cb(NULL);
            break;
        case TRACK_SRC_BLE_GATT:
            ble_gatt_detail_back_cb(NULL);
            break;
        case TRACK_SRC_BLE_DETECT:
            ble_detect_detail_back_cb(NULL);
            break;
        case TRACK_SRC_WIFI_AP:
        default:
            ap_detail_back_cb(NULL);
            break;
    }
}

static void ap_track_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        if (ap_scan_select(selected_ap_index) != ESP_OK) {
            error_popup_create("Failed to select AP");
            return;
        }
        wifi_manager_select_ap(selected_ap_index);
        if (ap_detail_view) {
            detail_view_destroy(ap_detail_view);
            ap_detail_view = NULL;
        }

        char ssid[33] = {0};
        if (selected_ap.ssid[0] == 0) {
            strcpy(ssid, "<Hidden>");
        } else {
            strncpy(ssid, (const char *)selected_ap.ssid, sizeof(ssid) - 1);
        }
        start_track_meter(true, ssid);
    }
}

static void ap_connect_password_cb(const char *text) {
    if (ap_connect_ssid[0] == '\0') {
        error_popup_create("SSID unavailable");
        keyboard_view_set_submit_callback(NULL);
        return;
    }

    const char *pass = text ? text : "";
    if (strlen(pass) >= 64) {
        error_popup_create("pass too long");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "connect \"%s\" \"%s\"", ap_connect_ssid, pass);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);

    ap_connect_ssid[0] = '\0';
    keyboard_view_set_submit_callback(NULL);
}

static void ap_connect_cb(lv_event_t *e) {
    (void)e;

    if (selected_ap_index < 0) {
        return;
    }

    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    if (!aps || selected_ap_index >= (int)count) {
        error_popup_create("AP not found");
        return;
    }

    wifi_ap_record_t *ap = &aps[selected_ap_index];
    if (ap->ssid[0] == 0) {
        error_popup_create("Hidden SSID unsupported");
        return;
    }

    if (ap_scan_select(selected_ap_index) != ESP_OK) {
        error_popup_create("Failed to select AP");
        return;
    }
    wifi_manager_select_ap(selected_ap_index);
    ap_detail_back_cb(NULL);

    char ssid[33] = {0};
    strncpy(ssid, (const char *)ap->ssid, sizeof(ssid) - 1);

    if (ap->authmode == WIFI_AUTH_OPEN) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "connect \"%s\" \"\"", ssid);
        terminal_set_return_view(&options_menu_view);
        display_manager_switch_view(&terminal_view);
        simulateCommand(cmd);
        return;
    }

    strncpy(ap_connect_ssid, ssid, sizeof(ap_connect_ssid) - 1);
    ap_connect_ssid[sizeof(ap_connect_ssid) - 1] = '\0';

    char placeholder[64];
    snprintf(placeholder, sizeof(placeholder), "Password for %.24s", ap_connect_ssid);
    keyboard_view_set_return_view(&options_menu_view);
    keyboard_view_set_submit_callback(ap_connect_password_cb);
    keyboard_view_set_placeholder(placeholder);
    display_manager_switch_view(&keyboard_view);
}

static void ap_select_cb(lv_event_t *e) {
    (void)e;
    if (selected_ap_index >= 0) {
        if (ap_scan_select(selected_ap_index) != ESP_OK) {
            error_popup_create("Failed to select AP");
            return;
        }
        wifi_manager_select_ap(selected_ap_index);
        ap_detail_back_cb(NULL);
    }
}

static void ap_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (ap_detail_view) {
        detail_view_destroy(ap_detail_view);
        ap_detail_view = NULL;
    }

    WifiMenuState return_state = ap_detail_return_state;
    nav_pop_wifi_detail_return(&return_state);

    SelectedMenuType = OT_Wifi;
    current_wifi_menu_state = return_state;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void show_ap_detail(int ap_index) {
    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&count, &aps);
    
    if (!aps || ap_index < 0 || ap_index >= (int)count) {
        error_popup_create("AP not found");
        return;
    }

    ap_detail_return_state = (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST)
                                 ? WIFI_MENU_SCANALL_LIST
                                 : WIFI_MENU_AP_LIST;
    nav_push_wifi_detail_return(ap_detail_return_state);

    selected_ap_index = ap_index;
    wifi_ap_record_t *ap = &aps[ap_index];
    bool compact_detail = use_compact_wifi_detail_layout();
    
    char ssid[33] = {0};
    if (ap->ssid[0] == 0) {
        strcpy(ssid, "<Hidden>");
    } else {
        strncpy(ssid, (const char *)ap->ssid, 32);
    }
    
    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }
    
    ap_detail_view = detail_view_create(lv_scr_act(), NULL);
    reserve_detail_touch_bar_space(ap_detail_view);
    
    detail_view_add_info(ap_detail_view, "SSID", ssid);
    
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X",
             ap->bssid[0], ap->bssid[1], ap->bssid[2],
             ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    detail_view_add_info(ap_detail_view, compact_detail ? "BSSID" : "BSSID", bssid);
    
    detail_view_add_infof(ap_detail_view, compact_detail ? "Ch/RSSI" : "Channel / RSSI", "%d / %d dBm", ap->primary, ap->rssi);
    detail_view_add_info(ap_detail_view, "Security", auth_mode_to_string(ap->authmode));

    if (!compact_detail) {
        detail_view_add_info(ap_detail_view, "Actions:", "");
    }
    detail_view_add_action(ap_detail_view, "Deauth", ap_deauth_cb, NULL);
    detail_view_add_action(ap_detail_view, "HS+Deauth", ap_hs_deauth_cb, NULL);
    detail_view_add_action(ap_detail_view, "Probe Flood", ap_probe_flood_cb, NULL);
    detail_view_add_action(ap_detail_view, "Bad Msg", ap_bad_msg_cb, NULL);
    detail_view_add_action(ap_detail_view, "Auth Flood", ap_auth_flood_cb, NULL);
    detail_view_add_action(ap_detail_view, "Connect", ap_connect_cb, NULL);
    detail_view_add_action(ap_detail_view, "Track AP", ap_track_cb, NULL);
    detail_view_add_action(ap_detail_view, "Select AP", ap_select_cb, NULL);
    detail_view_add_back(ap_detail_view, ap_detail_back_cb, NULL);
    
    current_wifi_menu_state = WIFI_MENU_AP_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void ap_scan_complete_callback(void) {
    if (ap_scan_status) {
        scan_status_close(ap_scan_status);
        ap_scan_status = NULL;
    }
    
    uint16_t count = ap_scan_get_count();

    if (station_scan_waiting_for_ap_scan) {
        station_scan_waiting_for_ap_scan = false;
        if (count == 0) {
            error_popup_create("No APs found");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
            return;
        }

        if (!start_station_scan_flow()) {
            error_popup_create("Station scan failed to start");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
        }
        return;
    }

    if (scan_all_flow_active && !scan_all_started_station_phase) {
        if (count == 0) {
            scan_all_flow_active = false;
            error_popup_create("No APs found");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
            return;
        }

        scan_all_started_station_phase = true;
        if (!start_station_scan_flow()) {
            scan_all_flow_active = false;
            scan_all_started_station_phase = false;
            error_popup_create("Station scan failed to start");
            current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
            rebuild_current_menu();
        }
        return;
    }

    if (count == 0) {
        error_popup_create("No APs found");
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        return;
    }
    
    /* rebuild_current_menu() re-binds the windowed AP list, which re-reads the
     * finished scan results into the visible rows. */
    current_wifi_menu_state = WIFI_MENU_AP_LIST;
    rebuild_current_menu();
}

static bool station_select_for_action(void) {
    if (selected_station_index < 0) {
        error_popup_create("No station selected");
        return false;
    }
    if (station_scan_select(selected_station_index) != ESP_OK) {
        error_popup_create("Failed to select station");
        return false;
    }
    return true;
}

static void station_deauth_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_STA_LIST;
    suppress_wifi_state_reset_once = true;
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("attack -d");
}

static void station_hs_deauth_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_STA_LIST;
    suppress_wifi_state_reset_once = true;
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand("attack -hsd");
}

static void station_track_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }

    char sta_mac[18] = "Station";
    if (selected_station_index >= 0 && selected_station_index < station_scan_get_count()) {
        station_format_mac(station_ap_list[selected_station_index].station_mac, sta_mac, sizeof(sta_mac));
    }
    start_track_meter(false, sta_mac);
}

static void station_select_cb(lv_event_t *e) {
    (void)e;
    if (!station_select_for_action()) {
        return;
    }
    station_detail_back_cb(NULL);
}

static void station_detail_back_cb(lv_event_t *e) {
    (void)e;
    if (sta_detail_view) {
        detail_view_destroy(sta_detail_view);
        sta_detail_view = NULL;
    }

    WifiMenuState return_state = sta_detail_return_state;
    nav_pop_wifi_detail_return(&return_state);

    SelectedMenuType = OT_Wifi;
    current_wifi_menu_state = return_state;
    suppress_wifi_state_reset_once = true;
    display_manager_add_status_bar(options_menu_type_to_string(SelectedMenuType));
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void show_station_detail(int station_index) {
    int count = station_scan_get_count();
    if (station_index < 0 || station_index >= count) {
        error_popup_create("Station not found");
        return;
    }

    sta_detail_return_state = (current_wifi_menu_state == WIFI_MENU_SCANALL_LIST)
                                  ? WIFI_MENU_SCANALL_LIST
                                  : WIFI_MENU_STA_LIST;
    nav_push_wifi_detail_return(sta_detail_return_state);

    selected_station_index = station_index;
    station_ap_pair_t *station = &station_ap_list[station_index];
    bool compact_detail = use_compact_wifi_detail_layout();

    char sta_mac[18];
    char ap_bssid[18];
    char ap_ssid[33];
    char sta_vendor[64] = "Unknown";
    char ap_vendor[64] = "Unknown";

    station_format_mac(station->station_mac, sta_mac, sizeof(sta_mac));
    station_format_mac(station->ap_bssid, ap_bssid, sizeof(ap_bssid));
    station_lookup_ap_ssid(station->ap_bssid, ap_ssid, sizeof(ap_ssid));

    ouis_lookup_vendor(sta_mac, sta_vendor, sizeof(sta_vendor));
    ouis_lookup_vendor(ap_bssid, ap_vendor, sizeof(ap_vendor));

    if (menu_build_timer) {
        lv_timer_del(menu_build_timer);
        menu_build_timer = NULL;
    }

    sta_detail_view = detail_view_create(lv_scr_act(), NULL);
    reserve_detail_touch_bar_space(sta_detail_view);
    detail_view_add_info(sta_detail_view, compact_detail ? "Station" : "Station MAC", sta_mac);
    detail_view_add_info(sta_detail_view, compact_detail ? "Vendor" : "Station Vendor", sta_vendor);
    detail_view_add_info(sta_detail_view, compact_detail ? "AP" : "Associated AP", ap_ssid);
    detail_view_add_info(sta_detail_view, "AP BSSID", ap_bssid);
    if (!compact_detail) {
        detail_view_add_info(sta_detail_view, "AP Vendor", ap_vendor);
        detail_view_add_info(sta_detail_view, "Actions:", "");
    }
    detail_view_add_action(sta_detail_view, "Deauth", station_deauth_cb, NULL);
    detail_view_add_action(sta_detail_view, "HS+Deauth", station_hs_deauth_cb, NULL);
    detail_view_add_action(sta_detail_view, "Track Station", station_track_cb, NULL);
    detail_view_add_action(sta_detail_view, "Select Station", station_select_cb, NULL);
    detail_view_add_back(sta_detail_view, station_detail_back_cb, NULL);

    current_wifi_menu_state = WIFI_MENU_STA_DETAILS;
#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

static void station_scan_complete_callback(void) {
    if (sta_scan_status) {
        scan_status_close(sta_scan_status);
        sta_scan_status = NULL;
    }

    int count = station_scan_get_count();

    if (scan_all_flow_active && scan_all_started_station_phase) {
        scan_all_flow_active = false;
        scan_all_started_station_phase = false;
        sta_scan_stopped_by_user = false;
        if (scanall_list_menu) {
            paged_menu_reset(scanall_list_menu);
        }
        current_wifi_menu_state = WIFI_MENU_SCANALL_LIST;
        rebuild_current_menu();
        return;
    }

    if (count <= 0) {
        if (!sta_scan_stopped_by_user) {
            error_popup_create("No stations found");
        }
        current_wifi_menu_state = WIFI_MENU_SCAN_SELECT;
        rebuild_current_menu();
        sta_scan_stopped_by_user = false;
        return;
    }

    sta_scan_stopped_by_user = false;
    current_wifi_menu_state = WIFI_MENU_STA_LIST;
    rebuild_current_menu();
}

static void wigle_manual_popup_close_cb(lv_event_t *e) {
    (void)e;
    wigle_set_manual_upload_callback(NULL);
    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        lvgl_obj_del_safe(&wigle_manual_popup);
    }
    wigle_manual_upload_btn = NULL;
    wigle_manual_close_btn = NULL;
    wigle_manual_info_label = NULL;
    wigle_manual_popup_selected = 0;
}

static void wigle_manual_popup_update_selection(void) {
    lv_obj_t *btns[2] = { wigle_manual_upload_btn, wigle_manual_close_btn };
    popup_update_selection(btns, 2, wigle_manual_popup_selected);
}

static void wigle_get_popup_geometry(int *popup_w, int *popup_h, int *y_offset) {
    popup_calc_size_t geom;
    popup_calc_size_ex(&geom, 120);
    if (popup_w) *popup_w = geom.width;
    if (popup_h) *popup_h = geom.height;
    if (y_offset) *y_offset = geom.y_offset;
}

static void wigle_stats_popup_close_cb(lv_event_t *e) {
    (void)e;
    wigle_set_stats_callback(NULL);
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        lvgl_obj_del_safe(&wigle_stats_popup);
    }
    wigle_stats_down_btn = NULL;
    wigle_stats_close_btn = NULL;
    wigle_stats_body_label = NULL;
    wigle_stats_scroll = NULL;
    wigle_stats_popup_selected = 1;
}

static void wigle_stats_popup_scroll(int delta_y) {
    if (!wigle_stats_scroll || !lv_obj_is_valid(wigle_stats_scroll) || delta_y == 0) {
        return;
    }

    lv_obj_update_layout(wigle_stats_scroll);
    lv_coord_t y = lv_obj_get_scroll_y(wigle_stats_scroll);
    if (delta_y > 0 && lv_obj_get_scroll_bottom(wigle_stats_scroll) <= 0) {
        lv_obj_scroll_to_y(wigle_stats_scroll, 0, LV_ANIM_OFF);
        return;
    }

    lv_obj_scroll_to_y(wigle_stats_scroll, y + delta_y, LV_ANIM_OFF);
}

static void wigle_stats_popup_scroll_down_cb(lv_event_t *e) {
    (void)e;
    wigle_stats_popup_scroll(40);
}

static void wigle_stats_popup_update_selection(void) {
    lv_obj_t *btns[2] = { wigle_stats_down_btn, wigle_stats_close_btn };
    popup_update_selection(btns, 2, wigle_stats_popup_selected);
}

static void wigle_stats_popup_activate_selected(void) {
    if (wigle_stats_popup_selected == 0) {
        wigle_stats_popup_scroll(40);
    } else {
        wigle_stats_popup_close_cb(NULL);
    }
}

static void wigle_stats_popup_open(void) {
    if (wigle_stats_popup && lv_obj_is_valid(wigle_stats_popup)) {
        return;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    int popup_w = 0;
    int popup_h = 0;
    int y_offset = 0;
    wigle_get_popup_geometry(&popup_w, &popup_h, &y_offset);
    wigle_stats_popup = popup_create_container_with_offset(lv_layer_top(), popup_w, popup_h, y_offset, true);
    lv_obj_set_style_bg_color(wigle_stats_popup, lv_color_hex(theme_palette_get_surface(theme)), 0);
    lv_obj_add_flag(wigle_stats_popup, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = popup_create_title_label(wigle_stats_popup, "WiGLE Stats", accessibility_get_font_body(), 5);
    (void)title;

    int scroll_h = popup_h - 76;
    if (scroll_h < 58) scroll_h = 58;
    wigle_stats_scroll = popup_create_scroll_area(wigle_stats_popup, popup_w - 16, scroll_h,
                                                   LV_ALIGN_TOP_MID, 0, 24);
    wigle_stats_body_label = lv_label_create(wigle_stats_scroll);
    lv_label_set_long_mode(wigle_stats_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wigle_stats_body_label, popup_w - 24);
    lv_obj_set_style_text_color(wigle_stats_body_label, lv_color_hex(theme_palette_get_text_muted(theme)), 0);
    lv_obj_set_style_text_font(wigle_stats_body_label,
                               (LV_VER_RES <= 200) ? accessibility_get_font_small() : accessibility_get_font_body(),
                               0);
    lv_obj_set_style_text_line_space(wigle_stats_body_label, 3, 0);
    lv_label_set_text(wigle_stats_body_label, "Loading WiGLE stats...");

    wigle_stats_down_btn = popup_add_styled_button(
        wigle_stats_popup, "Down", 88, 32,
        LV_ALIGN_BOTTOM_LEFT, 10, -8,
        accessibility_get_font_body(),
        wigle_stats_popup_scroll_down_cb, NULL);
    wigle_stats_close_btn = popup_add_styled_button(
        wigle_stats_popup, "Close", 96, 32,
        LV_ALIGN_BOTTOM_RIGHT, -10, -8,
        accessibility_get_font_body(),
        wigle_stats_popup_close_cb, NULL);

    lv_obj_t *btns[2] = { wigle_stats_down_btn, wigle_stats_close_btn };
    PopupButtonLayoutConfig cfg = {
        .min_w = 80,
        .max_w = 140,
        .min_threshold = 64,
        .gap = 10,
    };
    popup_layout_buttons_responsive(wigle_stats_popup, btns, 2, -8, &cfg);
    wigle_stats_popup_selected = 1;
    wigle_stats_popup_update_selection();
}

static void wigle_manual_popup_upload_cb(lv_event_t *e) {
    (void)e;
    if (!selected_wigle_csv[0]) {
        error_popup_create("No CSV selected");
        return;
    }
    if (wigle_is_manual_upload_in_progress()) {
        error_popup_create("Upload already in progress");
        return;
    }

    if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
        lv_label_set_text_fmt(wigle_manual_info_label,
                              "Name: %s\n\nUploading...", selected_wigle_csv);
    }
    wigle_set_manual_upload_callback(wigle_manual_upload_result_cb);
    esp_err_t err = wigle_upload_single_csv_async(selected_wigle_csv);
    if (err != ESP_OK) {
        wigle_set_manual_upload_callback(NULL);
        if (wigle_manual_info_label && lv_obj_is_valid(wigle_manual_info_label)) {
            lv_label_set_text(wigle_manual_info_label, "Failed to start upload.");
        }
        return;
    }
}

static void wigle_show_csv_details_popup(const char *filename) {
    if (!filename || !filename[0]) {
        error_popup_create("Invalid CSV name");
        return;
    }

    int wifi_rows = 0;
    int total_rows = 0;
    esp_err_t err = wigle_get_csv_info(filename, &wifi_rows, &total_rows);
    if (err != ESP_OK) {
        error_popup_create("Failed to read CSV info");
        return;
    }

    strncpy(selected_wigle_csv, filename, sizeof(selected_wigle_csv) - 1);
    selected_wigle_csv[sizeof(selected_wigle_csv) - 1] = '\0';

    if (wigle_manual_popup && lv_obj_is_valid(wigle_manual_popup)) {
        lvgl_obj_del_safe(&wigle_manual_popup);
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    int popup_w = 0;
    int popup_h = 0;
    int y_offset = 0;
    wigle_get_popup_geometry(&popup_w, &popup_h, &y_offset);
    wigle_manual_popup = popup_create_container_with_offset(lv_layer_top(), popup_w, popup_h, y_offset, true);
    lv_obj_set_style_bg_color(wigle_manual_popup, lv_color_hex(theme_palette_get_surface(theme)), 0);
    lv_obj_add_flag(wigle_manual_popup, LV_OBJ_FLAG_CLICKABLE);

    popup_create_title_label(wigle_manual_popup, "WiGLE Manual Upload", accessibility_get_font_body(), 5);

    int info_scroll_h = popup_h - 76;
    if (info_scroll_h < 58) info_scroll_h = 58;
    lv_obj_t *info_scroll = popup_create_scroll_area(wigle_manual_popup, popup_w - 16, info_scroll_h,
                                                     LV_ALIGN_TOP_MID, 0, 28);
    wigle_manual_info_label = lv_label_create(info_scroll);
    lv_label_set_long_mode(wigle_manual_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(wigle_manual_info_label, popup_w - 24);
    lv_obj_set_style_text_color(wigle_manual_info_label, lv_color_hex(theme_palette_get_text_muted(theme)), 0);
    lv_obj_set_style_text_font(wigle_manual_info_label,
                               (LV_VER_RES <= 200) ? accessibility_get_font_small() : accessibility_get_font_body(),
                               0);
    lv_obj_set_style_text_line_space(wigle_manual_info_label, 2, 0);

    char details[220];
    snprintf(details, sizeof(details),
             "Name: %s\nWiFi rows: %d\nTotal rows: %d",
             filename, wifi_rows, total_rows);
    lv_label_set_text(wigle_manual_info_label, details);

    wigle_manual_upload_btn = popup_add_styled_button(
        wigle_manual_popup, "Upload", 90, 32,
        LV_ALIGN_BOTTOM_LEFT, 10, -8,
        accessibility_get_font_body(),
        wigle_manual_popup_upload_cb, NULL);
    wigle_manual_close_btn = popup_add_styled_button(
        wigle_manual_popup, "Cancel", 90, 32,
        LV_ALIGN_BOTTOM_RIGHT, -10, -8,
        accessibility_get_font_body(),
        wigle_manual_popup_close_cb, NULL);

    lv_obj_t *btns[2] = { wigle_manual_upload_btn, wigle_manual_close_btn };
    PopupButtonLayoutConfig cfg = {
        .min_w = 80,
        .max_w = 140,
        .min_threshold = 64,
        .gap = 10,
    };
    popup_layout_buttons_responsive(wigle_manual_popup, btns, 2, -8, &cfg);
    wigle_manual_popup_selected = 0;
    wigle_manual_popup_update_selection();
}

/* -----------------------------------------------------------------------
 * Portal page helpers
 * ----------------------------------------------------------------------- */

/** Free the heap storage for the currently loaded portal page. */
static void portal_free_cache(void) {
    if (evil_portal_names)   { free(evil_portal_names);   evil_portal_names   = NULL; }
    if (evil_portal_options) { free(evil_portal_options); evil_portal_options = NULL; }
}

/**
 * Load one page of .html files from the portals directory into
 * evil_portal_names / evil_portal_options.
 *
 * Layout of the returned NULL-terminated options array:
 *   page 0 : [default]  [file0 … fileN]  [Next > if more]
 *   page 1+: [< Prev]   [file0 … fileN]  [Next > if more]
 *
 * Always frees any previously cached page first.
 * Returns evil_portal_options on success, a static fallback {"default",NULL}
 * on allocation or directory-open failure.
 *
 * The caller is responsible for JIT-mounting/unmounting the SD card around
 * this call on shared-SPI boards.
 */
static const char **portal_load_page(void) {
    static const char *fallback[] = {"default", NULL};

    portal_free_cache();

    /* ---- read one page from the SD card ---- */
    char (*file_names)[MAX_PORTAL_NAME] =
        malloc(PORTAL_PAGE_SIZE * MAX_PORTAL_NAME);
    if (!file_names) {
        ESP_LOGE(TAG, "portal_load_page: OOM for file name buffer");
        return fallback;
    }

    int count = sd_card_list_dir_paged(
        "/mnt/ghostesp/evil_portal/portals", ".html",
        portal_page_offset, PORTAL_PAGE_SIZE,
        file_names, &portal_has_next_page);

    if (count < 0) {
        ESP_LOGW(TAG, "portal_load_page: directory scan failed (offset=%d)", portal_page_offset);
        free(file_names);
        return fallback;
    }

    /* ---- determine optional prefix / suffix navigation items ---- */
    bool show_prev    = (portal_page_offset > 0);
    bool show_default = (portal_page_offset == 0);
    bool show_next    = portal_has_next_page;

    int total = (show_prev ? 1 : 0) + (show_default ? 1 : 0)
              + count + (show_next ? 1 : 0);

    if (total == 0) {
        /* Empty directory — show a non-selectable placeholder */
        free(file_names);
        static const char *empty[] = {"No portal files found", NULL};
        return empty;
    }

    /* ---- allocate final storage ---- */
    evil_portal_names   = malloc(MAX_PORTAL_NAME * (size_t)total);
    evil_portal_options = malloc(sizeof(char *) * ((size_t)total + 1));

    if (!evil_portal_names || !evil_portal_options) {
        ESP_LOGE(TAG, "portal_load_page: OOM for portal list (total=%d)", total);
        free(file_names);
        portal_free_cache();
        return fallback;
    }

    /* ---- fill options array ---- */
    int idx = 0;

    if (show_prev) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "< Prev");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_default) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "default");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    for (int i = 0; i < count; i++) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, file_names[i]);
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_next) {
        strcpy(evil_portal_names + idx * MAX_PORTAL_NAME, "Next >");
        evil_portal_options[idx] = evil_portal_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    evil_portal_options[idx] = NULL;

    free(file_names);

    ESP_LOGI(TAG, "portal page loaded: offset=%d files=%d prev=%d next=%d "
             "heap_used=%zu bytes",
             portal_page_offset, count, show_prev, show_next,
             (size_t)total * MAX_PORTAL_NAME + sizeof(char *) * ((size_t)total + 1));

    return evil_portal_options;
}

static void blocklist_free_cache(void) {
    if (blocklist_file_names) { free(blocklist_file_names); blocklist_file_names = NULL; }
    if (blocklist_file_options) { free(blocklist_file_options); blocklist_file_options = NULL; }
}

static const char **blocklist_load_page(void) {
    blocklist_free_cache();

    char (*file_names)[MAX_PORTAL_NAME] =
        malloc(BLOCKLIST_PAGE_SIZE * MAX_PORTAL_NAME);
    if (!file_names) return NULL;

    int raw_count = sd_card_list_dir_paged(
        SINKHOLE_DIR_PATH, ".txt",
        blocklist_page_offset * BLOCKLIST_PAGE_SIZE, BLOCKLIST_PAGE_SIZE + 1,
        file_names, &blocklist_has_next_page);

    if (raw_count < 0) {
        free(file_names);
        return NULL;
    }

    int count = 0;
    for (int i = 0; i < raw_count; i++) {
        if (strcmp(file_names[i], "stats.txt") == 0) continue;
        if (i != count) strcpy(file_names[count], file_names[i]);
        count++;
    }

    blocklist_has_next_page = (raw_count > BLOCKLIST_PAGE_SIZE);
    if (count > BLOCKLIST_PAGE_SIZE) {
        count = BLOCKLIST_PAGE_SIZE;
        blocklist_has_next_page = true;
    }

    bool show_prev = (blocklist_page_offset > 0);
    bool show_next = blocklist_has_next_page;
    int total = (show_prev ? 1 : 0) + count + (show_next ? 1 : 0);

    if (total == 0) {
        free(file_names);
        return NULL;
    }

    blocklist_file_names   = malloc(MAX_PORTAL_NAME * (size_t)total);
    blocklist_file_options = malloc(sizeof(char *) * ((size_t)total + 1));

    if (!blocklist_file_names || !blocklist_file_options) {
        free(file_names);
        blocklist_free_cache();
        return NULL;
    }

    int idx = 0;

    if (show_prev) {
        strcpy(blocklist_file_names + idx * MAX_PORTAL_NAME, "< Prev");
        blocklist_file_options[idx] = blocklist_file_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    for (int i = 0; i < count; i++) {
        strcpy(blocklist_file_names + idx * MAX_PORTAL_NAME, file_names[i]);
        blocklist_file_options[idx] = blocklist_file_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    if (show_next) {
        strcpy(blocklist_file_names + idx * MAX_PORTAL_NAME, "Next >");
        blocklist_file_options[idx] = blocklist_file_names + idx * MAX_PORTAL_NAME;
        idx++;
    }
    blocklist_file_options[idx] = NULL;

    free(file_names);
    return blocklist_file_options;
}

static void rebuild_current_menu(void) {
    options_menu_push_rendered_state();
    settings_select_close();
    lvgl_timer_del_safe(&menu_build_timer);
    s_back_option_added = false;
    
    if (g_options_view) {
        options_view_clear(g_options_view);
    } else if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_obj_clean(menu_container);
    }
    
    num_items = 0;
    build_item_index = 0;
    selected_item_index = 0;
    
    const char * const *options = NULL;
    int timer_period = 15;
    
    if (is_settings_mode) {
        current_options_list = NULL;
        timer_period = current_settings_category < 0 ? 20 : 15;
    } else {
        switch (SelectedMenuType) {
        case OT_Wifi:
            switch (current_wifi_menu_state) {
                case WIFI_MENU_MAIN: options = wifi_main_options; break;
                case WIFI_MENU_ATTACKS: options = wifi_attacks_options; break;
                case WIFI_MENU_SCAN_SELECT: options = wifi_scan_select_options; break;
                case WIFI_MENU_ENVIRONMENT: options = wifi_environment_options; break;
                case WIFI_MENU_NETWORK: options = wifi_network_options; break;
                case WIFI_MENU_CAPTURE: options = wifi_capture_options; break;
                case WIFI_MENU_EVIL_PORTAL: options = wifi_evil_portal_options; break;
                case WIFI_MENU_DNS_SINKHOLE: options = wifi_dns_sinkhole_options; break;
                case WIFI_MENU_DNS_SINKHOLE_DOWNLOAD: options = wifi_dns_sinkhole_download_options; break;
                case WIFI_MENU_DNS_SINKHOLE_FILE_PICK:
                    options = blocklist_file_options;
                    break;
                case WIFI_MENU_DNS_SINKHOLE_DETAILS:
                    options = NULL;
                    break;
                case WIFI_MENU_CONNECTION: options = wifi_connection_options; break;
                case WIFI_MENU_MISC: options = wifi_misc_options; break;
                case WIFI_MENU_GOVEE: options = wifi_govee_options; break;
                case WIFI_MENU_GOVEE_LIST:
                case WIFI_MENU_GOVEE_DETAILS:
                    /* Windowed list: rows come from the scan results on demand. */
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case WIFI_MENU_EVIL_PORTAL_SELECT:
                {
                    /* JIT-mount on shared-SPI boards before scanning SD */
                    bool jit_mounted = false;
                    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
                        if (!sd_card_manager.is_initialized) {
                            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                                jit_mounted = true;
                            }
                        }
                    }
#endif
                    options = portal_load_page();
                    timer_period = 25;
                    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
                    break;
                }
                case WIFI_MENU_KARMA_PORTAL_SELECT:
                {
                    /* Reuse same SD directory as evil portal. JIT-mount if needed. */
                    bool jit_mounted = false;
                    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
                    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
                        if (!sd_card_manager.is_initialized) {
                            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                                jit_mounted = true;
                            }
                        }
                    }
#endif
                    options = portal_load_page();
                    timer_period = 25;
                    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
                    break;
                }
                case WIFI_MENU_AP_LIST:
                    /* Windowed list: rows come from the scan results on demand. */
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case WIFI_MENU_AP_DETAILS:
                    options = NULL;
                    break;
                case WIFI_MENU_STA_LIST:
                case WIFI_MENU_STA_DETAILS:
                case WIFI_MENU_SCANALL_LIST:
                case WIFI_MENU_ARP_LIST:
                case WIFI_MENU_ARP_DETAILS:
                case WIFI_MENU_MDNS_LIST:
                case WIFI_MENU_MDNS_DETAILS:
                case WIFI_MENU_ENUM_LIST:
                case WIFI_MENU_ENUM_DETAILS:
                    /* Windowed list: rows come from the scan results on demand. */
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case WIFI_MENU_AP_MULTI_SELECT:
                    options = ap_multi_select_get_options();
                    timer_period = 25;
                    break;
                case WIFI_MENU_STA_MULTI_SELECT:
                    options = sta_multi_select_get_options();
                    timer_period = 25;
                    break;
                case WIFI_MENU_CAPTURE_BROWSER:
                    options = pcap_capture_load_page();
                    timer_period = 25;
                    break;
            }
            break;
        case OT_Bluetooth:
            switch (current_bluetooth_menu_state) {
                case BLUETOOTH_MENU_MAIN: options = bluetooth_main_options; break;
                case BLUETOOTH_MENU_DETECT_LIST:
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
                    if (ble_device_detect_is_tracking()) {
                        ble_device_detect_stop_tracking();
                    }
                    if (ble_device_detect_is_active() && !ble_is_initialized()) {
                        ble_device_detect_stop();
                    }
                    if (ble_device_detect_get_count() <= 0 && !ble_device_detect_is_active()) {
                        start_ble_detect_flow();
                    }
#endif
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_DETECT_DETAILS:
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_ADV_LIST:
#ifndef CONFIG_IDF_TARGET_ESP32S2
                    if (advertiser_scan_get_count() <= 0 && !advertiser_scan_is_active()) {
                        start_ble_adv_flow();
                    }
#endif
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_ADV_DETAILS:
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_GATT_LIST:
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
                    if (gatt_scan_get_device_count() <= 0 && !gatt_scan_is_active()) {
                        start_ble_gatt_flow();
                    }
#endif
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_GATT_DETAILS:
                    options = NULL;
                    s_vlist_pending = true;
                    break;
                case BLUETOOTH_MENU_OUI: options = bluetooth_oui_options; break;
                case BLUETOOTH_MENU_OUI_VENDOR_LIST:
                    options = ble_oui_vendor_list_get_options();
                    timer_period = 25;
                    break;
                case BLUETOOTH_MENU_SPAM: options = bluetooth_spam_options; break;
                case BLUETOOTH_MENU_RAW: options = bluetooth_raw_options; break;
                case BLUETOOTH_MENU_GATT: options = bluetooth_gatt_options; break;
                case BLUETOOTH_MENU_AERIAL: options = bluetooth_aerial_options; break;
            }
            break;
        case OT_GPS: options = gps_options; break;
        case OT_NRF24:
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
            options = nrf24_options;
#else
            options = NULL;
#endif
            break;
        case OT_SubGhz:
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
            options = subghz_options;
#else
            options = NULL;
#endif
            break;
        case OT_DualComm:
            switch (current_dualcomm_menu_state) {
                case DUALCOMM_MENU_MAIN:     options = dual_comm_main_options; break;
                case DUALCOMM_MENU_SESSION:  options = dual_comm_session_options; break;
                case DUALCOMM_MENU_SCAN:     options = dual_comm_scan_options; break;
                case DUALCOMM_MENU_WIFI:     options = dual_comm_wifi_options; break;
                case DUALCOMM_MENU_ATTACKS:  options = dual_comm_attacks_options; break;
                case DUALCOMM_MENU_CAPTURE:  options = dual_comm_capture_options; break;
                case DUALCOMM_MENU_TOOLS:    options = dual_comm_tools_options; break;
                case DUALCOMM_MENU_BLE:      options = dual_comm_ble_options; break;
                case DUALCOMM_MENU_GPS:      options = dual_comm_gps_options; break;
                case DUALCOMM_MENU_ETHERNET: options = dual_comm_ethernet_options; break;
                case DUALCOMM_MENU_KEYBOARD: options = dual_comm_keyboard_options; break;
            }
            break;
        case OT_IOButtonPresets:
            is_settings_mode = false;
            options = get_io_btn_preset_options();
            break;
        case OT_WigleManualUpload:
            options = wigle_csv_load_page();
            timer_period = 25;
            break;
        default: break;
        }
        current_options_list = options;
    }
    
    // update title
    if (is_settings_mode) {
        if (current_settings_category >= 0) {
            int cat_count = sizeof(settings_categories) / sizeof(settings_categories[0]);
            if (current_settings_category < cat_count) {
                options_view_set_title(g_options_view, settings_categories[current_settings_category].name);
            } else {
                options_view_set_title(g_options_view, "Settings");
            }
        } else if (current_settings_root >= 0) {
            int root_count = sizeof(settings_root_categories) / sizeof(settings_root_categories[0]);
            if (current_settings_root < root_count) {
                options_view_set_title(g_options_view, settings_root_categories[current_settings_root].name);
            } else {
                options_view_set_title(g_options_view, "Settings");
            }
        } else {
            options_view_set_title(g_options_view, "Settings");
        }
    } else {
        if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_LIST) {
            options_view_set_title(g_options_view, "APs Found");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_DETAILS) {
            options_view_set_title(g_options_view, "AP Details");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_LIST) {
            options_view_set_title(g_options_view, "Stations Found");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_DETAILS) {
            options_view_set_title(g_options_view, "Station Details");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
            options_view_set_title(g_options_view, "Scan All Results");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_ARP_LIST) {
            options_view_set_title(g_options_view, "ARP Hosts");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_AP_MULTI_SELECT) {
            options_view_set_title(g_options_view, "Select APs");
        } else if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_STA_MULTI_SELECT) {
            options_view_set_title(g_options_view, "Select Stations");
        } else if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_OUI) {
            options_view_set_title(g_options_view, "OUI Device Scan");
        } else if (SelectedMenuType == OT_Bluetooth && current_bluetooth_menu_state == BLUETOOTH_MENU_OUI_VENDOR_LIST) {
            options_view_set_title(g_options_view, "Select Vendor");
        } else {
            options_view_set_title(g_options_view, options_menu_type_to_string(SelectedMenuType));
        }
    }
    
    // Build the first batch synchronously so short menus (WiFi/BLE mains,
    // submenus) appear instantly like the dedicated BadUSB/NFC views instead
    // of crawling in from the top; the timer only fills overflow rows, which
    // keeps huge lists (AP/STA scans, portals, settings) non-blocking.
    if (s_vlist_pending) {
        /* Windowed list: bind the row pool directly, since there is no batched
         * build to hook into. Any pending selection restore is applied by
         * menu_builder_cb's windowed branch below. */
        s_vlist_pending = false;
        vlist_show();
        num_items = options_view_get_item_count(g_options_view);
    }
    menu_build_timer = lv_timer_create(menu_builder_cb, timer_period, NULL);
    menu_builder_cb(NULL);
}

static void switch_to_settings_root(int root_idx) {
    int root_count = sizeof(settings_root_categories) / sizeof(settings_root_categories[0]);
    if (root_idx < 0 || root_idx >= root_count) {
        ESP_LOGW(TAG,
                 "switch_to_settings_root: index %d outside [0..%d]; interpreting as Back action",
                 root_idx, root_count - 1);
        back_event_cb(NULL);
        return;
    }

    if (settings_root_categories[root_idx].id == SETTINGS_ROOT_INFO) {
        options_show_info_detail();
        return;
    }

    current_settings_root = root_idx;
    current_settings_category = -1;
    settings_submenu_depth = 1;
    rebuild_current_menu();
}

static void switch_to_settings_category(int cat_idx) {
    /* -------------------------------------------------------------------- *
     * SAFETY GUARD                                                         *
     *                                                                      *
     * The encoder can highlight the synthetic "← Back" row that is added   *
     * to the end of the Settings root list when CONFIG_USE_ENCODER is set. *
     * That row's index is outside the visible category array.              *
     *                                                                      *
     * If we let that bogus index through, the very next LVGL tick in       *
     * menu_builder_cb() dereferences                                        *
     *     settings_categories[current_settings_category]                   *
     * which explodes with a LoadProhibited panic.                          *
     *                                                                      *
     * Instead, treat any out-of-range index exactly like a Back press and  *
     * leave current_settings_category unchanged.                           *
     * ------------------------------------------------------------------ */
    SettingsRootId root_id = current_settings_root_id();
    int category_count = settings_category_count_for_root(root_id);
    if (cat_idx < 0 || cat_idx >= category_count) {
        ESP_LOGW(TAG,
                 "switch_to_settings_category: index %d outside [0..%d]; "
                 "interpreting as Back action",
                 cat_idx, category_count - 1);
        back_event_cb(NULL);
        return;
    }

    int actual_cat_idx = settings_category_index_for_root_position(root_id, cat_idx);
    if (actual_cat_idx < 0) {
        back_event_cb(NULL);
        return;
    }

    current_settings_category = actual_cat_idx;
    settings_submenu_depth = 2;
    rebuild_current_menu();
#if GHOSTESP_OTA_SUPPORTED
    if (settings_categories[actual_cat_idx].id == SETTINGS_CAT_FIRMWARE_UPDATE) {
        ota_status_show_pending_self_failure();
    }
#endif
}

#ifdef CONFIG_USE_IO_EXPANDER
static void iobtn_p10_kb_cb(const char *text) {
    settings_set_io_btn_p10_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_DEVICE;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_IO_BUTTONS);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
static void iobtn_p11_kb_cb(const char *text) {
    settings_set_io_btn_p11_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_DEVICE;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_IO_BUTTONS);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
static void iobtn_p12_kb_cb(const char *text) {
    settings_set_io_btn_p12_cmd(&G_Settings, text ? text : "");
    settings_save(&G_Settings);
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_DEVICE;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_IO_BUTTONS);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    display_manager_switch_view(&options_menu_view);
}
#endif

// IR pin override keyboard callbacks (Devices page)
static void ir_pin_kb_apply(const char *text, bool is_tx) {
    bool ok = false;
    if (!text || !text[0] || strcmp(text, "-1") == 0) {
        ok = is_tx ? settings_set_ir_tx_pin(&G_Settings, -1)
                   : settings_set_ir_rx_pin(&G_Settings, -1);
    } else {
        char *end = NULL;
        long pin = strtol(text, &end, 10);
        if (end && *end == '\0') {
            ok = is_tx ? settings_set_ir_tx_pin(&G_Settings, (int32_t)pin)
                       : settings_set_ir_rx_pin(&G_Settings, (int32_t)pin);
        }
    }
    if (ok) {
        settings_persist_setting(is_tx ? SETTING_IR_TX_PIN : SETTING_IR_RX_PIN);
    } else {
        error_popup_create(is_tx ? "Invalid IR TX pin (use -1 or a valid GPIO)"
                                 : "Invalid IR RX pin (use -1 or a valid GPIO)");
    }
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_DEVICE;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_DEVICES);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    load_current_settings_values();
    display_manager_switch_view(&options_menu_view);
}

static void ir_tx_pin_kb_cb(const char *text) {
    ir_pin_kb_apply(text, true);
}

static void ir_rx_pin_kb_cb(const char *text) {
    ir_pin_kb_apply(text, false);
}

// AP/STA credentials keyboard callbacks
static void ap_ssid_kb_cb(const char *text) {
    if (text && text[0]) {
        settings_set_ap_ssid(&G_Settings, text);
        settings_persist_setting(SETTING_AP_SSID);
        // Apply AP changes so new clients can connect with new SSID
        (void)ap_manager_restore_after_attack("ap ssid change");
    }
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_NETWORK;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_NETWORK);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    load_current_settings_values();
    display_manager_switch_view(&options_menu_view);
}

static void ap_password_kb_cb(const char *text) {
    // Allow empty string (= open AP); only reject null
    settings_set_ap_password(&G_Settings, text ? text : "");
    settings_persist_setting(SETTING_AP_PASSWORD);
    (void)ap_manager_restore_after_attack("ap password change");
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_NETWORK;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_NETWORK);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    load_current_settings_values();
    display_manager_switch_view(&options_menu_view);
}

static void sta_ssid_kb_cb(const char *text) {
    if (text && text[0]) {
        settings_set_sta_ssid(&G_Settings, text);
        settings_persist_setting(SETTING_STA_SSID);
        wifi_manager_configure_sta_from_settings();
    }
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_NETWORK;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_NETWORK);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    load_current_settings_values();
    display_manager_switch_view(&options_menu_view);
}

static void sta_password_kb_cb(const char *text) {
    settings_set_sta_password(&G_Settings, text ? text : "");
    settings_persist_setting(SETTING_STA_PASSWORD);
    wifi_manager_configure_sta_from_settings();
    keyboard_view_set_submit_callback(NULL);
    current_settings_root = SETTINGS_ROOT_NETWORK;
    current_settings_category = settings_category_index_for_id(SETTINGS_CAT_NETWORK);
    settings_submenu_depth = 2;
    SelectedMenuType = OT_Settings;
    is_settings_mode = true;
    load_current_settings_values();
    display_manager_switch_view(&options_menu_view);
}

static void ssh_scan_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a valid IP address");
        return;
    }
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "scanssh %s", text);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void wol_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a MAC address");
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wol %s", text);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void govee_return_to_list(void) {
    if (govee_detail_view) {
        detail_view_destroy(govee_detail_view);
        govee_detail_view = NULL;
    }
    current_wifi_menu_state = WIFI_MENU_GOVEE_LIST;
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&options_menu_view);
}

static void govee_brightness_kb_cb(const char *text) {
    char *end = NULL;
    long brightness = text ? strtol(text, &end, 10) : -1;
    const govee_device_t *device = selected_govee_index >= 0 ?
                                        govee_scan_get_device(selected_govee_index) : NULL;
    if (!device || !text || !*text || !end || *end != '\0' || brightness < 0 || brightness > 100) {
        error_popup_create("Enter brightness from 0 to 100");
        return;
    }
    esp_err_t err = govee_set_brightness(device->ip, (uint8_t)brightness);
    toast_show_duration(err == ESP_OK ? "Brightness command sent" : "Govee command failed",
                        err == ESP_OK ? TOAST_SUCCESS : TOAST_ERROR, 1500);
    govee_return_to_list();
}

static void govee_color_kb_cb(const char *text) {
    unsigned int color = 0;
    const govee_device_t *device = selected_govee_index >= 0 ?
                                        govee_scan_get_device(selected_govee_index) : NULL;
    if (!device || !text || strlen(text) != 6 || sscanf(text, "%06x", &color) != 1) {
        error_popup_create("Enter a six-digit RGB color");
        return;
    }
    esp_err_t err = govee_set_color(device->ip, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    toast_show_duration(err == ESP_OK ? "Color command sent" : "Govee command failed",
                        err == ESP_OK ? TOAST_SUCCESS : TOAST_ERROR, 1500);
    govee_return_to_list();
}

static void netbios_scan_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a valid IP address");
        return;
    }
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "netbiosscan %s", text);
    
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void http_banner_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a valid IP address");
        return;
    }
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "httpbannerscan %s", text);
    
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void snmp_probe_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a valid IP address");
        return;
    }
    
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "snmpprobe %s", text);
    
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void netbios_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "netbiosscan subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void http_banner_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "httpbannerscan subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void snmp_probe_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "snmpprobe subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void snmp_walk_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "snmpprobe walk subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_netbios_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "commsend netbiosscan subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_http_banner_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "commsend httpbannerscan subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_snmp_probe_subnet_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a subnet prefix");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "commsend snmpprobe subnet %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_connect_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter peer name");
        return;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "commsend commconnect %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_send_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter command to send");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
static void zigbee_capture_kb_cb(const char *text) {
    if (!text) {
        error_popup_create("Enter channel 11-26");
        return;
    }
    const char *p = text;
    while (*p == ' ' || *p == '\t') p++;
    if ((p[0] == 'c' || p[0] == 'C') && (p[1] == 'h' || p[1] == 'H')) {
        p += 2;
    }
    while (*p == ' ' || *p == '\t') p++;
    char *endptr = NULL;
    long ch = strtol(p, &endptr, 10);
    while (endptr && (*endptr == ' ' || *endptr == '\t')) endptr++;
    if (p[0] == '\0' || (endptr && *endptr != '\0') || ch < 11 || ch > 26) {
        error_popup_create("Channel must be 11-26");
        return;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "capture -802154 ch%ld", ch);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}
#endif


static void wifi_connect_kb_cb(const char *text){
    const char *p=text;
    while(*p && *p!='\"') p++;
    if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
    p++; const char *start=p;
    while(*p && *p!='\"') p++;
    if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
    size_t len=p-start; if(len==0||len>=64){error_popup_create("ssid too long"); return;}
    char ssid[64]={0}; memcpy(ssid,start,len); ssid[len]='\0';
    p++; while(*p==' '){p++;}
    char pass[64]={0};
    if(*p=='\"'){
        p++; start=p; while(*p && *p!='\"') p++; if(!*p){error_popup_create("format: \"SSID\" \"PASSWORD\""); return;}
        len=p-start; if(len>=64){error_popup_create("pass too long"); return;}
        memcpy(pass,start,len); pass[len]='\0';
    }
    char cmd[256];
    snprintf(cmd,sizeof(cmd),"connect \"%s\" \"%s\"",ssid,pass);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_wifi_connect_kb_cb(const char *text) {
    const char *p = text;
    while (*p && *p != '"') p++;
    if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
    p++; const char *start = p;
    while (*p && *p != '"') p++;
    if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
    size_t len = p - start; if (len == 0 || len >= 64) { error_popup_create("ssid too long"); return; }
    char ssid[64] = {0}; memcpy(ssid, start, len); ssid[len] = '\0';
    p++; while (*p == ' ') { p++; }
    char pass[64] = {0};
    if (*p == '"') {
        p++; start = p; while (*p && *p != '"') p++; if (!*p) { error_popup_create("format: \"SSID\" \"PASSWORD\""); return; }
        len = p - start; if (len >= 64) { error_popup_create("pass too long"); return; }
        memcpy(pass, start, len); pass[len] = '\0';
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend connect \"%s\" \"%s\"", ssid, pass);
    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_karma_custom_ssids_cb(const char *input) {
    if (!input || strlen(input) == 0) {
        error_popup_create("Please enter at least one SSID.");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend karma start %s", input);

    terminal_set_return_view(&options_menu_view);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_apcred_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter AP credentials");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend apcred %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_dns_lookup_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Enter a hostname (e.g., example.com)");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend ethdns %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_traceroute_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a hostname or IP address");
        return;
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "commsend ethtrace %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}

static void dual_comm_http_request_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Please enter a URL");
        return;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "commsend ethhttp %s", text);

    terminal_set_return_view(&options_menu_view);
    terminal_set_dualcomm_filter(true);
    display_manager_switch_view(&terminal_view);
    simulateCommand(cmd);
    keyboard_view_set_submit_callback(NULL);
}


/* item font/centering/styling handled inside options_view */


// build menu items in small batches so we don't starve the watchdog
static void menu_builder_cb(lv_timer_t *t)
{
    /* Windowed (virtual) menus rebind a fixed row pool on scroll, so there are
     * no rows to materialise in batches. Finish the menu setup the way the
     * normal completion path does, then tear the now-unused build timer down. */
    if (g_options_view && options_view_is_virtual(g_options_view)) {
        if (t) {
            lv_timer_del(t);
        } else {
            lvgl_timer_del_safe(&menu_build_timer);
        }
        menu_build_timer = NULL;
        /* Restore a pending selection the way the batched completion path does,
         * minus the scroll (a windowed list never overflows). */
        if (s_pending_restore_state.valid) {
            int restore_index = s_pending_restore_state.selected;
            if (num_items > 0) {
                if (restore_index < 0) restore_index = 0;
                if (restore_index >= num_items) restore_index = num_items - 1;
                select_option_item(restore_index);
            }
            s_pending_restore_state.valid = false;
        }
        if (menu_container && lv_obj_is_valid(menu_container)) {
            update_scroll_buttons_visibility();
            update_settings_arrows_visibility();
        }
        s_rendered_menu_state = options_menu_capture_nav_state();
        options_menu_apply_pending_detail_resume();
        return;
    }
    if (!menu_container || !lv_obj_is_valid(menu_container) || !g_options_view) {
        if (t) {
            lv_timer_del(t);
            menu_build_timer = NULL;
        } else {
            lvgl_timer_del_safe(&menu_build_timer);
        }
        return;
    }
    const bool is_portal_select =
        (!is_settings_mode) &&
        ((SelectedMenuType == OT_Wifi &&
          (current_wifi_menu_state == WIFI_MENU_EVIL_PORTAL_SELECT ||
           current_wifi_menu_state == WIFI_MENU_KARMA_PORTAL_SELECT ||
           current_wifi_menu_state == WIFI_MENU_AP_LIST ||
           current_wifi_menu_state == WIFI_MENU_STA_LIST ||
           current_wifi_menu_state == WIFI_MENU_SCANALL_LIST ||
           current_wifi_menu_state == WIFI_MENU_DNS_SINKHOLE_FILE_PICK)) ||
         SelectedMenuType == OT_WigleManualUpload);

    const int BATCH = is_portal_select ? 2 : 6;
    int built_this_tick = 0;
    bool all_current_options_processed = false;

    bool back_option_was_added_in_previous_tick = s_back_option_added;

    if (!back_option_was_added_in_previous_tick) {
        if (is_settings_mode) {
            if (current_settings_root < 0) {
                int root_count = sizeof(settings_root_categories) / sizeof(settings_root_categories[0]);
                while (build_item_index < root_count && built_this_tick < BATCH) {
                    SettingsRootCategory *root_cat = &settings_root_categories[build_item_index];
                    lv_obj_t *btn = options_view_add_item(g_options_view, root_cat->name, option_event_cb, (void *)(intptr_t)build_item_index);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)build_item_index);
                    lv_obj_set_height(btn, button_height_global * 1.2);
                    options_view_relayout_item(g_options_view, btn);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
                    if (num_items == 1) {
                        select_option_item(0);
                    }
                }
                if (build_item_index >= root_count) {
                    all_current_options_processed = true;
                }
            } else if (current_settings_category < 0) {
                SettingsRootId root_id = current_settings_root_id();
                int category_count = settings_category_count_for_root(root_id);
                while (build_item_index < category_count && built_this_tick < BATCH) {
                    int cat_idx = settings_category_index_for_root_position(root_id, build_item_index);
                    if (cat_idx < 0) break;
                    SettingsCategory *cat = &settings_categories[cat_idx];
                    lv_obj_t *btn = options_view_add_item(g_options_view, cat->name, option_event_cb, (void *)(intptr_t)build_item_index);
                    if (!btn) break;
                    lv_obj_set_user_data(btn, (void *)(intptr_t)build_item_index);
                    lv_obj_set_height(btn, button_height_global * 1.2);
                    options_view_relayout_item(g_options_view, btn);
                    num_items++;
                    built_this_tick++;
                    build_item_index++;
                    if (num_items == 1) {
                        select_option_item(0);
                    }
                }
                if (build_item_index >= category_count) {
                    all_current_options_processed = true;
                }
            } else {
                SettingsCategoryId category_id = current_settings_category_id();
                if (category_id == SETTINGS_CAT_COUNT) {
                    all_current_options_processed = true;
                }
#ifdef CONFIG_USE_IO_EXPANDER
                else if (category_id == SETTINGS_CAT_IO_BUTTONS) {
                    const char *p10 = settings_get_io_btn_p10_cmd(&G_Settings);
                    const char *p11 = settings_get_io_btn_p11_cmd(&G_Settings);
                    const char *p12 = settings_get_io_btn_p12_cmd(&G_Settings);
                    const char *cmds[] = { p10, p11, p12 };
                    static const char *io_btn_labels[] = { "Center", "Right", "Left" };
                    int indices[] = { IO_BTN_EDIT_P10, IO_BTN_EDIT_P11, IO_BTN_EDIT_P12 };
                    for (int k = 0; k < 3 && built_this_tick < BATCH; k++) {
                        if (build_item_index <= k) {
                            char row[128];
                            const char* display_name = "(none)";
                            if (cmds[k] && cmds[k][0]) {
                                int action_idx = get_current_io_btn_action(cmds[k]);
                                if (action_idx >= 0 && action_idx < NUM_IO_BTN_PRESETS) {
                                    display_name = io_btn_presets[action_idx].name;
                                } else {
                                    display_name = cmds[k];
                                }
                            }
                            snprintf(row, sizeof(row), "%s: %s", io_btn_labels[k], display_name);
                            if (strlen(row) > 100) { row[97] = '.'; row[98] = '.'; row[99] = '\0'; }
                            lv_obj_t *btn = options_view_add_item(g_options_view, row, option_event_cb, (void *)(intptr_t)indices[k]);
                            if (!btn) break;
                            lv_obj_set_user_data(btn, (void *)(intptr_t)indices[k]);
                            lv_obj_set_height(btn, button_height_global);
                            num_items++;
                            built_this_tick++;
                            build_item_index++;
                            if (num_items == 1) {
                                select_option_item(0);
                                options_view_refresh_selected_item(g_options_view);
                            }
                        }
                    }
                    if (build_item_index >= 3) all_current_options_processed = true;
                } else
#endif
                {
                int settings_count = sizeof(settings_items) / sizeof(settings_items[0]);
                int items_in_category = 0;
                
                for (int i = 0; i < settings_count; i++) {
                    if (settings_items[i].category_id == category_id && settings_item_is_visible(&settings_items[i])) {
                        items_in_category++;
                    }
                }
                
                int current_item_in_category = 0;
                for (int i = 0; i < settings_count && built_this_tick < BATCH; i++) {
                    if (settings_items[i].category_id == category_id && settings_item_is_visible(&settings_items[i])) {
                        if (current_item_in_category >= build_item_index) {
                            SettingsItem *item = &settings_items[i];
                            lv_obj_t *btn = NULL;
                            if (item->widget == SETTING_WIDGET_TOGGLE) {
                                // iOS-style toggle row: label on left, switch on right.
                                btn = options_view_add_item(g_options_view, item->label, option_event_cb, (void *)(intptr_t)i);
                                if (!btn) break;
                                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                                lv_obj_set_height(btn, button_height_global);
                                decorate_settings_row_with_toggle(btn, item->current_value == 1);
                            } else {
                                // Classic cycle row: "Label: value" with left/right arrows.
                                char buf[128];
                                snprintf(buf, sizeof(buf), "%s: %s", item->label, settings_item_value_text(item));
                                btn = options_view_add_item(g_options_view, buf, option_event_cb, (void *)(intptr_t)i);
                                if (!btn) break;
                                lv_obj_set_user_data(btn, (void *)(intptr_t)i);
                                lv_obj_set_height(btn, button_height_global);
                                // Select rows now open a compact overlay; keep the old arrow decoration available for rollback.
                                // decorate_settings_row_with_arrows(btn);
                            }
                            num_items++;
                            built_this_tick++;
                            build_item_index++;
                            if (num_items == 1) {
                                select_option_item(0);
                                options_view_refresh_selected_item(g_options_view);
                            }
                        }
                        current_item_in_category++;
                    }
                }
                
                if (build_item_index >= items_in_category) {
                    all_current_options_processed = true;
                }
                }
            }
        } else {
            while (current_options_list != NULL && current_options_list[build_item_index] != NULL && built_this_tick < BATCH) {
                const char *opt = current_options_list[build_item_index];
                if (strcmp(opt, "Download Blocklist") == 0 &&
                    heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) <= 300 * 1024) {
                    build_item_index++;
                    continue;
                }
                lv_obj_t *btn = options_view_add_item(g_options_view, opt, option_event_cb, (void *)opt);
                if (!btn) break;
                lv_obj_set_user_data(btn, (void *)opt);
                int row_height = button_height_global;
                if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_SCANALL_LIST) {
                    if (strncmp(opt, "-> ", 3) == 0) {
                        row_height = button_height_global - 10;
                        if (row_height < 18) {
                            row_height = 18;
                        }
                    }
                }
                lv_obj_set_height(btn, row_height);
                options_view_relayout_item(g_options_view, btn);
                if (SelectedMenuType == OT_DualComm && current_dualcomm_menu_state == DUALCOMM_MENU_BLE &&
                    strcmp(opt, "BLE Bridge") == 0) {
                    decorate_settings_row_with_toggle(btn, ble_bridge_get_enabled() || ble_bridge_is_running());
                }
                if (SelectedMenuType == OT_Wifi && current_wifi_menu_state == WIFI_MENU_CAPTURE_BROWSER) {
                    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
                    if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
                }
                style_multi_select_row(btn, multi_select_option_is_toggled(build_item_index, opt));
                num_items++;
                built_this_tick++;
                build_item_index++;
                if (num_items == 1) {
                    select_option_item(0);
                }
            }
            if (current_options_list == NULL || current_options_list[build_item_index] == NULL) {
                all_current_options_processed = true;
            }
        }
    }

    if (is_settings_mode && current_settings_category >= 0 && built_this_tick > 0) {
        update_settings_arrows_visibility();
    }

    if (all_current_options_processed) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
        bool need_back_button = true;
#else
        bool need_back_button = screen_mirror_is_enabled();
#endif
        if (need_back_button && !back_option_was_added_in_previous_tick) {
            lv_obj_t *btn = options_view_add_item(g_options_view, LV_SYMBOL_LEFT " Back", option_event_cb, (void *)"__BACK_OPTION__");
            if (btn) {
                lv_obj_set_user_data(btn, (void *)"__BACK_OPTION__");
                lv_obj_set_height(btn, button_height_global);
                if (is_settings_mode && current_settings_category < 0) {
                    lv_obj_t *label = lv_obj_get_child(btn, 0);
                    if (label) lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
                }
                num_items++;
                s_back_option_added = true;
            }
        }
        if (
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
            s_back_option_added
#else
            need_back_button ? s_back_option_added : true
#endif
        ) {
            if (t) {
                lv_timer_del(t);
            } else {
                lvgl_timer_del_safe(&menu_build_timer);
            }
            if (menu_container && lv_obj_is_valid(menu_container)) {
                update_scroll_buttons_visibility();
                update_settings_arrows_visibility();
            }
            menu_build_timer = NULL;
            if (s_pending_restore_state.valid) {
                int restore_index = s_pending_restore_state.selected;
                if (num_items > 0) {
                    if (restore_index < 0) restore_index = 0;
                    if (restore_index >= num_items) restore_index = num_items - 1;
                    select_option_item(restore_index);
                }
                if (menu_container && lv_obj_is_valid(menu_container)) {
                    lv_obj_update_layout(menu_container);
                    lv_obj_scroll_to_y(menu_container, s_pending_restore_state.scroll_y, LV_ANIM_OFF);
                    update_scroll_buttons_visibility();
                }
                s_pending_restore_state.valid = false;
            }
            s_rendered_menu_state = options_menu_capture_nav_state();
            options_menu_apply_pending_detail_resume();
        }
    }
}
