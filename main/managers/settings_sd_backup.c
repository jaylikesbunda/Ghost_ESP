#include "managers/settings_sd_backup.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "managers/sd_card_manager.h"
#include "managers/display_manager.h"
#include "managers/wifi_manager.h"
#include "cJSON.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PIN_CLAMP_LO (-1)
#define PIN_CLAMP_HI 127

static const char *TAG = "settings_sd_backup";
static const char k_format[] = "ghost_esp_settings";
static const int k_version = 1;

static void jstrcpy_field(char *dst, size_t dstsz, const cJSON *root, const char *key) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it || !cJSON_IsString(it) || !it->valuestring) {
    return;
  }
  strncpy(dst, it->valuestring, dstsz - 1);
  dst[dstsz - 1] = '\0';
}

static int jget_int_clamp(const cJSON *root, const char *key, int def, int lo, int hi) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it || !cJSON_IsNumber(it)) {
    return def;
  }
  int v = (int)cJSON_GetNumberValue(it);
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static uint32_t jget_u32_clamp(const cJSON *root, const char *key, uint32_t def, uint32_t lo,
                               uint32_t hi) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it || !cJSON_IsNumber(it)) {
    return def;
  }
  double d = cJSON_GetNumberValue(it);
  if (d < 0) {
    return def;
  }
  uint32_t v = (uint32_t)d;
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static float jget_float(const cJSON *root, const char *key, float def) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it || !cJSON_IsNumber(it)) {
    return def;
  }
  return (float)cJSON_GetNumberValue(it);
}

static bool jget_bool(const cJSON *root, const char *key, bool def) {
  const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!it) {
    return def;
  }
  if (cJSON_IsBool(it)) {
    return cJSON_IsTrue(it);
  }
  if (cJSON_IsNumber(it)) {
    return cJSON_GetNumberValue(it) != 0;
  }
  if (cJSON_IsString(it) && it->valuestring) {
    if (strcasecmp(it->valuestring, "true") == 0 || strcmp(it->valuestring, "1") == 0) {
      return true;
    }
    if (strcasecmp(it->valuestring, "false") == 0 || strcmp(it->valuestring, "0") == 0) {
      return false;
    }
  }
  return def;
}

static cJSON *settings_to_json_object(const FSettings *s) {
  cJSON *o = cJSON_CreateObject();
  if (!o) {
    return NULL;
  }
  cJSON_AddStringToObject(o, "format", k_format);
  cJSON_AddNumberToObject(o, "version", k_version);

  cJSON_AddNumberToObject(o, "rgb_mode", (double)s->rgb_mode);
  cJSON_AddNumberToObject(o, "channel_delay", (double)s->channel_delay);
  cJSON_AddNumberToObject(o, "broadcast_speed", (double)s->broadcast_speed);
  cJSON_AddStringToObject(o, "ap_ssid", s->ap_ssid);
  cJSON_AddStringToObject(o, "ap_password", s->ap_password);
  cJSON_AddNumberToObject(o, "rgb_speed", (double)s->rgb_speed);
  cJSON_AddNumberToObject(o, "rgb_led_count", (double)s->rgb_led_count);

  cJSON_AddStringToObject(o, "portal_url", s->portal_url);
  cJSON_AddStringToObject(o, "portal_ssid", s->portal_ssid);
  cJSON_AddStringToObject(o, "portal_password", s->portal_password);
  cJSON_AddStringToObject(o, "portal_ap_ssid", s->portal_ap_ssid);
  cJSON_AddStringToObject(o, "portal_domain", s->portal_domain);
  cJSON_AddBoolToObject(o, "portal_offline_mode", s->portal_offline_mode);

  cJSON_AddStringToObject(o, "printer_ip", s->printer_ip);
  cJSON_AddStringToObject(o, "printer_text", s->printer_text);
  cJSON_AddNumberToObject(o, "printer_font_size", (double)s->printer_font_size);
  cJSON_AddNumberToObject(o, "printer_alignment", (double)s->printer_alignment);

  cJSON_AddStringToObject(o, "flappy_ghost_name", s->flappy_ghost_name);
  cJSON_AddStringToObject(o, "selected_timezone", s->selected_timezone);
  cJSON_AddStringToObject(o, "selected_hex_accent_color", s->selected_hex_accent_color);
  cJSON_AddNumberToObject(o, "gps_rx_pin", (double)s->gps_rx_pin);
  cJSON_AddNumberToObject(o, "gps_baud_rate", (double)s->gps_baud_rate);
  cJSON_AddNumberToObject(o, "display_timeout_ms", (double)s->display_timeout_ms);
  cJSON_AddBoolToObject(o, "rts_enabled", s->rts_enabled);

  cJSON_AddStringToObject(o, "sta_ssid", s->sta_ssid);
  cJSON_AddStringToObject(o, "sta_password", s->sta_password);

  cJSON_AddNumberToObject(o, "rgb_data_pin", (double)s->rgb_data_pin);
  cJSON_AddNumberToObject(o, "rgb_red_pin", (double)s->rgb_red_pin);
  cJSON_AddNumberToObject(o, "rgb_green_pin", (double)s->rgb_green_pin);
  cJSON_AddNumberToObject(o, "rgb_blue_pin", (double)s->rgb_blue_pin);

  cJSON_AddBoolToObject(o, "third_control_enabled", s->third_control_enabled);
  cJSON_AddNumberToObject(o, "menu_theme", (double)s->menu_theme);
  cJSON_AddNumberToObject(o, "terminal_text_color", (double)s->terminal_text_color);
  cJSON_AddNumberToObject(o, "terminal_font_size", (double)s->terminal_font_size);
  cJSON_AddBoolToObject(o, "invert_colors", s->invert_colors);
  cJSON_AddBoolToObject(o, "web_auth_enabled", s->web_auth_enabled);
  cJSON_AddBoolToObject(o, "webui_restrict_to_ap", s->webui_restrict_to_ap);
  cJSON_AddNumberToObject(o, "esp_comm_tx_pin", (double)s->esp_comm_tx_pin);
  cJSON_AddNumberToObject(o, "esp_comm_rx_pin", (double)s->esp_comm_rx_pin);
  cJSON_AddBoolToObject(o, "ap_enabled", s->ap_enabled);
  cJSON_AddBoolToObject(o, "power_save_enabled", s->power_save_enabled);
  cJSON_AddBoolToObject(o, "zebra_menus_enabled", s->zebra_menus_enabled);
  cJSON_AddNumberToObject(o, "max_screen_brightness", (double)s->max_screen_brightness);
  cJSON_AddBoolToObject(o, "infrared_easy_mode", s->infrared_easy_mode);
  cJSON_AddBoolToObject(o, "nav_buttons_enabled", s->nav_buttons_enabled);
  cJSON_AddNumberToObject(o, "menu_layout", (double)s->menu_layout);
  cJSON *menu_items = cJSON_AddArrayToObject(o, "menu_items");
  for (int i = 0; menu_items && i < s->menu_config.count && i < MENU_CONFIG_MAX; ++i) {
    const menu_config_entry_t *entry = &s->menu_config.entries[i];
    cJSON *item = cJSON_CreateObject();
    if (!item) break;
    cJSON_AddStringToObject(item, "id", entry->id);
    cJSON_AddNumberToObject(item, "placement", entry->placement);
    cJSON_AddNumberToObject(item, "main_order", entry->order[0]);
    cJSON_AddNumberToObject(item, "apps_order", entry->order[1]);
    cJSON_AddItemToArray(menu_items, item);
  }
  cJSON_AddBoolToObject(o, "carousel_invert_direction", s->carousel_invert_direction);
  cJSON_AddNumberToObject(o, "neopixel_max_brightness", (double)s->neopixel_max_brightness);
  cJSON_AddBoolToObject(o, "encoder_invert_direction", s->encoder_invert_direction);
  cJSON_AddBoolToObject(o, "auto_save_scans", s->auto_save_scans);
  cJSON_AddBoolToObject(o, "setup_complete", s->setup_complete);
  cJSON_AddNumberToObject(o, "wifi_country", (double)s->wifi_country);

  cJSON_AddStringToObject(o, "wigle_api_key", s->wigle_api_key);
  cJSON_AddBoolToObject(o, "wigle_auto_upload", s->wigle_auto_upload);
  cJSON_AddBoolToObject(o, "wigle_donate", s->wigle_donate);
  cJSON_AddBoolToObject(o, "wifi_auto_reconnect", s->wifi_auto_reconnect);

  cJSON_AddStringToObject(o, "io_btn_p10_cmd", s->io_btn_p10_cmd);
  cJSON_AddStringToObject(o, "io_btn_p11_cmd", s->io_btn_p11_cmd);
  cJSON_AddStringToObject(o, "io_btn_p12_cmd", s->io_btn_p12_cmd);

  cJSON_AddNumberToObject(o, "mic_visualizer_mode", (double)s->mic_visualizer_mode);
  cJSON_AddNumberToObject(o, "mic_color_mode", (double)s->mic_color_mode);
  cJSON_AddNumberToObject(o, "mic_sensitivity", (double)s->mic_sensitivity);
  cJSON_AddNumberToObject(o, "mic_smoothing", (double)s->mic_smoothing);
  cJSON_AddNumberToObject(o, "mic_contrast", (double)s->mic_contrast);
  cJSON_AddBoolToObject(o, "mic_mirror_mode", s->mic_mirror_mode);
  cJSON_AddBoolToObject(o, "ghostlink_split_view", s->ghostlink_split_view);
  cJSON_AddNumberToObject(o, "menu_bg_shade", (double)s->menu_bg_shade);
  cJSON_AddNumberToObject(o, "theme_schema", 1);
  cJSON_AddBoolToObject(o, "theme_background_effects", s->theme_background_effects);
  cJSON_AddBoolToObject(o, "high_contrast", s->high_contrast);
  cJSON_AddBoolToObject(o, "sun_mode", s->sun_mode);
  cJSON_AddBoolToObject(o, "menu_rounded", s->menu_rounded);
  cJSON_AddBoolToObject(o, "menu_item_borders", s->menu_item_borders);
  cJSON_AddBoolToObject(o, "menu_card_bg", s->menu_card_bg);
  cJSON_AddNumberToObject(o, "row_height", (double)s->row_height);

#ifdef CONFIG_WITH_STATUS_DISPLAY
  cJSON_AddNumberToObject(o, "status_idle_animation", (double)s->status_idle_animation);
  cJSON_AddNumberToObject(o, "status_idle_timeout_ms", (double)s->status_idle_timeout_ms);
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  cJSON_AddNumberToObject(o, "badusb_vid", (double)s->badusb_vid);
  cJSON_AddNumberToObject(o, "badusb_pid", (double)s->badusb_pid);
  cJSON_AddStringToObject(o, "badusb_manufacturer", s->badusb_manufacturer);
  cJSON_AddStringToObject(o, "badusb_product", s->badusb_product);
  cJSON_AddBoolToObject(o, "badusb_randomize", s->badusb_randomize);
  cJSON_AddNumberToObject(o, "badusb_kb_layout", (double)s->badusb_kb_layout);
#endif
  return o;
}

static void json_apply_to_settings(FSettings *s, const cJSON *root) {
  s->rgb_mode = (RGBMode)jget_int_clamp(root, "rgb_mode", (int)s->rgb_mode, 0, 31);
  s->channel_delay = jget_float(root, "channel_delay", s->channel_delay);
  if (s->channel_delay < 0.0f) {
    s->channel_delay = 0.0f;
  }
  if (s->channel_delay > 60000.0f) {
    s->channel_delay = 60000.0f;
  }
  s->broadcast_speed = (uint16_t)jget_int_clamp(root, "broadcast_speed", s->broadcast_speed, 0, 65535);
  jstrcpy_field(s->ap_ssid, sizeof(s->ap_ssid), root, "ap_ssid");
  jstrcpy_field(s->ap_password, sizeof(s->ap_password), root, "ap_password");
  s->rgb_speed = (uint8_t)jget_int_clamp(root, "rgb_speed", s->rgb_speed, 0, 255);
  s->rgb_led_count = (uint16_t)jget_int_clamp(root, "rgb_led_count", s->rgb_led_count, 1, 4096);

  jstrcpy_field(s->portal_url, sizeof(s->portal_url), root, "portal_url");
  jstrcpy_field(s->portal_ssid, sizeof(s->portal_ssid), root, "portal_ssid");
  jstrcpy_field(s->portal_password, sizeof(s->portal_password), root, "portal_password");
  jstrcpy_field(s->portal_ap_ssid, sizeof(s->portal_ap_ssid), root, "portal_ap_ssid");
  jstrcpy_field(s->portal_domain, sizeof(s->portal_domain), root, "portal_domain");
  if (cJSON_GetObjectItemCaseSensitive(root, "portal_offline_mode")) {
    s->portal_offline_mode = jget_bool(root, "portal_offline_mode", s->portal_offline_mode);
  }

  jstrcpy_field(s->printer_ip, sizeof(s->printer_ip), root, "printer_ip");
  jstrcpy_field(s->printer_text, sizeof(s->printer_text), root, "printer_text");
  s->printer_font_size = (uint8_t)jget_int_clamp(root, "printer_font_size", s->printer_font_size, 1, 255);
  s->printer_alignment =
      (PrinterAlignment)jget_int_clamp(root, "printer_alignment", (int)s->printer_alignment, 0, 4);

  jstrcpy_field(s->flappy_ghost_name, sizeof(s->flappy_ghost_name), root, "flappy_ghost_name");
  jstrcpy_field(s->selected_timezone, sizeof(s->selected_timezone), root, "selected_timezone");
  jstrcpy_field(s->selected_hex_accent_color, sizeof(s->selected_hex_accent_color), root,
                "selected_hex_accent_color");
  s->gps_rx_pin = jget_int_clamp(root, "gps_rx_pin", s->gps_rx_pin, -1, 255);
  s->gps_baud_rate = (uint32_t)jget_u32_clamp(root, "gps_baud_rate", s->gps_baud_rate, 0u, 4000000u);
  s->display_timeout_ms =
      jget_u32_clamp(root, "display_timeout_ms", s->display_timeout_ms, 0, 86400000u);
  if (cJSON_GetObjectItemCaseSensitive(root, "rts_enabled")) {
    s->rts_enabled = jget_bool(root, "rts_enabled", s->rts_enabled);
  }

  jstrcpy_field(s->sta_ssid, sizeof(s->sta_ssid), root, "sta_ssid");
  jstrcpy_field(s->sta_password, sizeof(s->sta_password), root, "sta_password");

  s->rgb_data_pin =
      (int32_t)jget_int_clamp(root, "rgb_data_pin", (int)s->rgb_data_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);
  s->rgb_red_pin =
      (int32_t)jget_int_clamp(root, "rgb_red_pin", (int)s->rgb_red_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);
  s->rgb_green_pin =
      (int32_t)jget_int_clamp(root, "rgb_green_pin", (int)s->rgb_green_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);
  s->rgb_blue_pin =
      (int32_t)jget_int_clamp(root, "rgb_blue_pin", (int)s->rgb_blue_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);

  if (cJSON_GetObjectItemCaseSensitive(root, "third_control_enabled")) {
    s->third_control_enabled = jget_bool(root, "third_control_enabled", s->third_control_enabled);
  }
  s->menu_theme =
      (uint8_t)jget_int_clamp(root, "menu_theme", s->menu_theme, 0, THEME_PALETTE_THEME_COUNT - 1);
  s->terminal_text_color =
      jget_u32_clamp(root, "terminal_text_color", s->terminal_text_color, 0, 0xFFFFFFu);
  s->terminal_font_size =
      (uint8_t)jget_int_clamp(root, "terminal_font_size", s->terminal_font_size, 0, 2);
  if (cJSON_GetObjectItemCaseSensitive(root, "invert_colors")) {
    s->invert_colors = jget_bool(root, "invert_colors", s->invert_colors);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "web_auth_enabled")) {
    s->web_auth_enabled = jget_bool(root, "web_auth_enabled", s->web_auth_enabled);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "webui_restrict_to_ap")) {
    s->webui_restrict_to_ap = jget_bool(root, "webui_restrict_to_ap", s->webui_restrict_to_ap);
  }
  s->esp_comm_tx_pin =
      (int32_t)jget_int_clamp(root, "esp_comm_tx_pin", (int)s->esp_comm_tx_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);
  s->esp_comm_rx_pin =
      (int32_t)jget_int_clamp(root, "esp_comm_rx_pin", (int)s->esp_comm_rx_pin, PIN_CLAMP_LO, PIN_CLAMP_HI);
  if (cJSON_GetObjectItemCaseSensitive(root, "ap_enabled")) {
    s->ap_enabled = jget_bool(root, "ap_enabled", s->ap_enabled);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "power_save_enabled")) {
    s->power_save_enabled = jget_bool(root, "power_save_enabled", s->power_save_enabled);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "zebra_menus_enabled")) {
    s->zebra_menus_enabled = jget_bool(root, "zebra_menus_enabled", s->zebra_menus_enabled);
  }
  s->max_screen_brightness =
      (uint8_t)jget_int_clamp(root, "max_screen_brightness", s->max_screen_brightness, 0, 100);
  if (cJSON_GetObjectItemCaseSensitive(root, "infrared_easy_mode")) {
    s->infrared_easy_mode = jget_bool(root, "infrared_easy_mode", s->infrared_easy_mode);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "nav_buttons_enabled")) {
    s->nav_buttons_enabled = jget_bool(root, "nav_buttons_enabled", s->nav_buttons_enabled);
  }
  s->menu_layout = (uint8_t)jget_int_clamp(root, "menu_layout", s->menu_layout, 0, 4);
  const cJSON *menu_items = cJSON_GetObjectItemCaseSensitive(root, "menu_items");
  if (cJSON_IsArray(menu_items)) {
    menu_config_reset(&s->menu_config);
    const cJSON *item;
    cJSON_ArrayForEach(item, menu_items) {
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");
      if (!cJSON_IsString(id) || !id->valuestring || strlen(id->valuestring) >= MENU_CONFIG_ID_LEN) continue;
      int placement = jget_int_clamp(item, "placement", MENU_PLACE_APPS, 1, 3);
      if (!menu_config_set(&s->menu_config, id->valuestring, placement)) continue;
      menu_config_order(&s->menu_config, id->valuestring, placement, MENU_PLACE_MAIN,
                        jget_int_clamp(item, "main_order", MENU_ORDER_DEFAULT, 0, 255));
      menu_config_order(&s->menu_config, id->valuestring, placement, MENU_PLACE_APPS,
                        jget_int_clamp(item, "apps_order", MENU_ORDER_DEFAULT, 0, 255));
    }
    menu_config_validate(&s->menu_config);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "carousel_invert_direction")) {
    s->carousel_invert_direction =
        jget_bool(root, "carousel_invert_direction", s->carousel_invert_direction);
  }
  s->neopixel_max_brightness =
      (uint8_t)jget_int_clamp(root, "neopixel_max_brightness", s->neopixel_max_brightness, 0, 100);
  if (cJSON_GetObjectItemCaseSensitive(root, "encoder_invert_direction")) {
    s->encoder_invert_direction =
        jget_bool(root, "encoder_invert_direction", s->encoder_invert_direction);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "auto_save_scans")) {
    s->auto_save_scans = jget_bool(root, "auto_save_scans", s->auto_save_scans);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "setup_complete")) {
    s->setup_complete = jget_bool(root, "setup_complete", s->setup_complete);
  }
  s->wifi_country = (uint8_t)jget_int_clamp(root, "wifi_country", s->wifi_country, 0, 255);

  jstrcpy_field(s->wigle_api_key, sizeof(s->wigle_api_key), root, "wigle_api_key");
  if (cJSON_GetObjectItemCaseSensitive(root, "wigle_auto_upload")) {
    s->wigle_auto_upload = jget_bool(root, "wigle_auto_upload", s->wigle_auto_upload);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "wigle_donate")) {
    s->wigle_donate = jget_bool(root, "wigle_donate", s->wigle_donate);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "wifi_auto_reconnect")) {
    s->wifi_auto_reconnect = jget_bool(root, "wifi_auto_reconnect", s->wifi_auto_reconnect);
  }

  jstrcpy_field(s->io_btn_p10_cmd, sizeof(s->io_btn_p10_cmd), root, "io_btn_p10_cmd");
  jstrcpy_field(s->io_btn_p11_cmd, sizeof(s->io_btn_p11_cmd), root, "io_btn_p11_cmd");
  jstrcpy_field(s->io_btn_p12_cmd, sizeof(s->io_btn_p12_cmd), root, "io_btn_p12_cmd");

  s->mic_visualizer_mode = (MicVisualizerMode)jget_int_clamp(
      root, "mic_visualizer_mode", (int)s->mic_visualizer_mode, 0, (int)MIC_MODE_COUNT - 1);
  s->mic_color_mode =
      (MicColorMode)jget_int_clamp(root, "mic_color_mode", (int)s->mic_color_mode, 0,
                                   (int)MIC_COLOR_COUNT - 1);
  s->mic_sensitivity = (uint8_t)jget_int_clamp(root, "mic_sensitivity", s->mic_sensitivity, 0, 100);
  s->mic_smoothing = (uint8_t)jget_int_clamp(root, "mic_smoothing", s->mic_smoothing, 0, 100);
  s->mic_contrast = (uint8_t)jget_int_clamp(root, "mic_contrast", s->mic_contrast, 1, 5);
  if (cJSON_GetObjectItemCaseSensitive(root, "mic_mirror_mode")) {
    s->mic_mirror_mode = jget_bool(root, "mic_mirror_mode", s->mic_mirror_mode);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "ghostlink_split_view")) {
    s->ghostlink_split_view = jget_bool(root, "ghostlink_split_view", s->ghostlink_split_view);
  }
  s->menu_bg_shade = (uint8_t)jget_int_clamp(root, "menu_bg_shade", s->menu_bg_shade, 0, 3);
  if (!cJSON_GetObjectItemCaseSensitive(root, "theme_schema")) {
    /* Backup predates palette descriptors: remap legacy accent IDs. */
    s->menu_theme = theme_palette_migrate_legacy(s->menu_theme);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "theme_background_effects")) {
    s->theme_background_effects = jget_bool(root, "theme_background_effects", s->theme_background_effects);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "high_contrast")) {
    s->high_contrast = jget_bool(root, "high_contrast", s->high_contrast);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "sun_mode")) {
    s->sun_mode = jget_bool(root, "sun_mode", s->sun_mode);
  }
  settings_normalize_modes(s);
  if (cJSON_GetObjectItemCaseSensitive(root, "menu_rounded")) {
    s->menu_rounded = jget_bool(root, "menu_rounded", s->menu_rounded);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "menu_item_borders")) {
    s->menu_item_borders = jget_bool(root, "menu_item_borders", s->menu_item_borders);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "menu_card_bg")) {
    s->menu_card_bg = jget_bool(root, "menu_card_bg", s->menu_card_bg);
  }
  s->row_height = (uint8_t)jget_int_clamp(root, "row_height", s->row_height, 0,
                                          MENU_ROW_HEIGHT_OPTION_COUNT - 1);

#ifdef CONFIG_WITH_STATUS_DISPLAY
  if (cJSON_GetObjectItemCaseSensitive(root, "status_idle_animation")) {
    s->status_idle_animation = (IdleAnimation)jget_int_clamp(
        root, "status_idle_animation", (int)s->status_idle_animation, 0, (int)IDLE_ANIM_BOUNCING_TEXT);
  }
  if (cJSON_GetObjectItemCaseSensitive(root, "status_idle_timeout_ms")) {
    s->status_idle_timeout_ms =
        jget_u32_clamp(root, "status_idle_timeout_ms", s->status_idle_timeout_ms, 0, 86400000u);
  }
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  s->badusb_vid = (uint16_t)jget_int_clamp(root, "badusb_vid", s->badusb_vid, 0, 65535);
  s->badusb_pid = (uint16_t)jget_int_clamp(root, "badusb_pid", s->badusb_pid, 0, 65535);
  jstrcpy_field(s->badusb_manufacturer, sizeof(s->badusb_manufacturer), root, "badusb_manufacturer");
  jstrcpy_field(s->badusb_product, sizeof(s->badusb_product), root, "badusb_product");
  if (cJSON_GetObjectItemCaseSensitive(root, "badusb_randomize")) {
    s->badusb_randomize = jget_bool(root, "badusb_randomize", s->badusb_randomize);
  }
  s->badusb_kb_layout =
      (uint8_t)jget_int_clamp(root, "badusb_kb_layout", s->badusb_kb_layout, 0, (int)KB_LAYOUT_COUNT - 1);
#endif
}

esp_err_t settings_backup_export_to_sd(void) {
  bool was_mounted = sd_card_manager.is_initialized;
  bool display_was_suspended = false;
  esp_err_t mount_err = sd_card_mount_for_flush(&display_was_suspended);
  if (mount_err != ESP_OK) {
    ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(mount_err));
    return mount_err;
  }
  bool did_mount = !was_mounted;

  cJSON *root = settings_to_json_object(&G_Settings);
  if (!root) {
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_NO_MEM;
  }

  char *printed = cJSON_Print(root);
  cJSON_Delete(root);
  if (!printed) {
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_NO_MEM;
  }

  size_t len = strlen(printed);
  esp_err_t w = sd_card_write_file(SETTINGS_SD_BACKUP_PATH, printed, len);
  free(printed);

  if (did_mount) {
    sd_card_unmount_after_flush(display_was_suspended);
  }

  if (w != ESP_OK) {
    ESP_LOGE(TAG, "write %s failed", SETTINGS_SD_BACKUP_PATH);
    return w;
  }
  ESP_LOGI(TAG, "exported to %s (%zu bytes)", SETTINGS_SD_BACKUP_PATH, len);
  return ESP_OK;
}

esp_err_t settings_backup_import_from_sd(void) {
  bool was_mounted = sd_card_manager.is_initialized;
  bool display_was_suspended = false;
  esp_err_t mount_err = sd_card_mount_for_flush(&display_was_suspended);
  if (mount_err != ESP_OK) {
    ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(mount_err));
    return mount_err;
  }
  bool did_mount = !was_mounted;

  FILE *f = fopen(SETTINGS_SD_BACKUP_PATH, "rb");
  if (!f) {
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_NOT_FOUND;
  }

  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 128 * 1024) {
    fclose(f);
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_INVALID_SIZE;
  }

  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_NO_MEM;
  }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
    free(buf);
    fclose(f);
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_FAIL;
  }
  buf[sz] = '\0';
  fclose(f);

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_FAIL;
  }

  const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
  const cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "version");
  if (!fmt || !cJSON_IsString(fmt) || !fmt->valuestring ||
      strcmp(fmt->valuestring, k_format) != 0 || !ver || !cJSON_IsNumber(ver) ||
      (int)cJSON_GetNumberValue(ver) != k_version) {
    cJSON_Delete(root);
    if (did_mount) {
      sd_card_unmount_after_flush(display_was_suspended);
    }
    return ESP_ERR_INVALID_VERSION;
  }

  json_apply_to_settings(&G_Settings, root);
  cJSON_Delete(root);

  settings_save(&G_Settings);

  if (did_mount) {
    sd_card_unmount_after_flush(display_was_suspended);
  }

  ESP_LOGI(TAG, "imported from %s", SETTINGS_SD_BACKUP_PATH);
  return ESP_OK;
}

void settings_backup_apply_runtime_after_import(void) {
  wifi_manager_configure_sta_from_settings();
  settings_restart_rgb_effect();
  display_manager_update_status_bar_color();
}
