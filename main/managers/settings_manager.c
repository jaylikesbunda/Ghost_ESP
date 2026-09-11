#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "managers/display_manager.h"
#include "mbedtls/base64.h"  // For base64 decoding
#include "managers/ghostchi_manager.h"
#include "managers/rgb_manager.h"
#include <esp_log.h>
#include <string.h>
#include <time.h>
#include <nvs.h>
#include "sdkconfig.h"
#include "driver/gpio.h"

#define S_TAG "SETTINGS"

static bool settings_should_use_noop_dualcomm_pins(void) {
#if defined(CONFIG_CROWPANEL_ADVANCED_P4) && defined(CONFIG_HAS_LORA)
  // CrowPanel P4 wireless-module mode: the GhostLink UART defaults (TX GPIO6 /
  // RX GPIO7) are the SX1262 SPI MOSI/MISO. Park them instead of fighting the
  // radio; this board's expansion UART is on GPIO47/48 (J2).
  return true;
#elif defined(CONFIG_CROWPANEL_ADVANCE_S3_LCD)
  // Standalone SD profile: the generic UART pins overlap SD MOSI and RGB R0.
  // Override saved defaults too, so existing installations need no NVS erase.
  return true;
#elif defined(CONFIG_HAS_LORA) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
  // Heltec V3 + LoRa: GhostLink UART RX (GPIO7) is the LoRa NSS pin in
  // hardware. A single headless board has no GhostLink peer anyway, so park
  // both pins rather than fight the radio (and break old saved 6/7 too).
  return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Heltec WiFi Kit 32 V3") == 0;
#elif defined(CONFIG_BUILD_CONFIG_TEMPLATE)
  return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Pancake") == 0 ||
         strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "MarauderV8") == 0;
#else
  return false;
#endif
}

// NVS Keys
static const char *NVS_RGB_MODE_KEY = "rgb_mode";
static const char *NVS_CHANNEL_DELAY_KEY = "channel_delay";
static const char *NVS_BROADCAST_SPEED_KEY = "broadcast_speed";
static const char *NVS_HOP_MODE_KEY = "hop_mode";
static const char *NVS_HOP_CUSTOM_KEY = "hop_custom";
static const char *NVS_AP_SSID_KEY = "ap_ssid";
static const char *NVS_AP_PASSWORD_KEY = "ap_password";
static const char *NVS_RGB_SPEED_KEY = "rgb_speed";
static const char *NVS_PORTAL_URL_KEY = "portal_url";
static const char *NVS_PORTAL_SSID_KEY = "portal_ssid";
static const char *NVS_PORTAL_PASSWORD_KEY = "portal_password";
static const char *NVS_PORTAL_AP_SSID_KEY = "portal_ap_ssid";
static const char *NVS_PORTAL_DOMAIN_KEY = "portal_domain";
static const char *NVS_PORTAL_OFFLINE_KEY = "portal_offline";
static const char *NVS_PRINTER_IP_KEY = "printer_ip";
static const char *NVS_PRINTER_TEXT_KEY = "printer_text";
static const char *NVS_PRINTER_FONT_SIZE_KEY = "pntr_ft_size";
static const char *NVS_PRINTER_ALIGNMENT_KEY = "pntr_alignment";
static const char *NVS_PRINTER_CONNECTED_KEY = "pntr_connected";
static const char *NVS_BOARD_TYPE_KEY = "board_type";
static const char *NVS_CUSTOM_PIN_CONFIG_KEY = "custom_pin_config";
static const char *NVS_FLAPPY_GHOST_NAME = "flap_name";
static const char *NVS_TIMEZONE_NAME = "sel_tz";
static const char *NVS_ACCENT_COLOR = "sel_ac";
static const char *NVS_GPS_RX_PIN = "gps_rx_pin";
static const char *NVS_GPS_BAUD_KEY = "gps_baud";
static const char *NVS_DISPLAY_TIMEOUT_KEY = "disp_timeout";
static const char *NVS_ENABLE_RTS_KEY = "rts_enable";
static const char *NVS_STA_SSID_KEY = "sta_ssid";
static const char *NVS_STA_PASSWORD_KEY = "sta_password";
static const char *NVS_WIFI_AUTO_RECONNECT_KEY = "sta_auto_rec";
static const char *NVS_RGB_DATA_PIN_KEY = "rgb_data_pin";
static const char *NVS_RGB_RED_PIN_KEY = "rgb_red_pin";
static const char *NVS_RGB_GREEN_PIN_KEY = "rgb_green_pin";
static const char *NVS_RGB_BLUE_PIN_KEY = "rgb_blue_pin";
static const char *NVS_THIRD_CTRL_KEY = "third_ctrl";
static const char *NVS_MENU_THEME_KEY = "menu_theme";
static const char *NVS_TERMINAL_TEXT_COLOR_KEY = "term_color";
static const char *NVS_TERMINAL_FONT_SIZE_KEY = "term_font";
static const char *NVS_INVERT_COLORS_KEY = "invert_colors";
static const char *NVS_INFRARED_EASY_MODE_KEY = "ir_easy_mode";
static const char *NVS_IR_TX_PIN_KEY = "ir_tx_pin";
static const char *NVS_IR_RX_PIN_KEY = "ir_rx_pin";
static const char *NVS_WEB_AUTH_KEY = "web_auth";
static const char *NVS_USB_MSC_KEY = "usb_msc";
static const char *NVS_WEBUI_AP_ONLY_KEY = "webui_ap";
static const char *NVS_ESP_COMM_TX_PIN_KEY = "esp_comm_tx";
static const char *NVS_ESP_COMM_RX_PIN_KEY = "esp_comm_rx";
static const char *NVS_AP_ENABLED_KEY = "ap_enabled";
static const char *NVS_POWER_SAVE_KEY = "power_save";
static const char *NVS_ZEBRA_MENUS_KEY = "zebra_menus";
static const char *NVS_MAX_SCREEN_BRIGHTNESS_KEY = "max_bright";
static const char *NVS_NAV_BUTTONS_KEY = "nav_buttons";
static const char *NVS_MENU_LAYOUT_KEY = "menu_layout";
static const char *NVS_CAROUSEL_INVERT_KEY = "carr_inv";
static const char *NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY = "neopixel_bright";
static const char *NVS_RGB_LED_COUNT_KEY = "rgb_led_cnt";
static const char *NVS_ENCODER_INVERT_KEY = "enc_inv";
static const char *NVS_AUTO_SAVE_SCANS_KEY = "auto_save_sc";
static const char *NVS_SETUP_COMPLETE_KEY = "setup_done";
static const char *NVS_WIFI_COUNTRY_KEY = "wifi_country";
static const char *NVS_WIGLE_API_KEY = "wigle_api_key";
static const char *NVS_WIGLE_DONATE_KEY = "wigle_donate";
static const char *NVS_WIGLE_AUTO_UPLOAD_KEY = "wigle_auto_ul";
static const char *NVS_OTA_CHANNEL_KEY = "ota_channel";
static const char *NVS_OTA_UPDATE_AVAIL_KEY = "ota_avail";
static const char *NVS_OTA_LAST_CHECK_KEY = "ota_last_chk";
#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *NVS_STATUS_IDLE_ANIM_KEY = "idle_anim"; // nvs keys must be <=15 chars
static const char *NVS_STATUS_IDLE_TIMEOUT_KEY = "idle_to_ms";
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
static const char *NVS_BADUSB_VID_KEY = "bu_vid";
static const char *NVS_BADUSB_PID_KEY = "bu_pid";
static const char *NVS_BADUSB_MFR_KEY = "bu_mfr";
static const char *NVS_BADUSB_PROD_KEY = "bu_prod";
static const char *NVS_BADUSB_RAND_KEY = "bu_rand";
static const char *NVS_BADUSB_KB_KEY = "bu_kb_layout";
#endif
static const char *NVS_IO_BTN_P10_CMD_KEY = "io_btn_p10";
static const char *NVS_IO_BTN_P11_CMD_KEY = "io_btn_p11";
static const char *NVS_IO_BTN_P12_CMD_KEY = "io_btn_p12";

// MIC RGB Visualizer NVS keys
static const char *NVS_MIC_VISUALIZER_MODE_KEY = "mic_vis_mode";
static const char *NVS_MIC_COLOR_MODE_KEY = "mic_color";
static const char *NVS_MIC_SENSITIVITY_KEY = "mic_sens";
static const char *NVS_MIC_SMOOTHING_KEY = "mic_smooth";
static const char *NVS_MIC_CONTRAST_KEY = "mic_contrast";
static const char *NVS_MIC_MIRROR_MODE_KEY = "mic_mirror";

static const char *NVS_GHOSTLINK_SPLIT_VIEW_KEY = "glink_split";
static const char *NVS_MENU_BG_SHADE_KEY = "menu_bg_shd";
static const char *NVS_THEME_SCHEMA_KEY = "theme_schema";
static const char *NVS_THEME_BG_EFFECTS_KEY = "theme_bg_fx";
static const char *NVS_MENU_ROUNDED_KEY = "menu_rounded";
static const char *NVS_EPILEPSY_WARNING_KEY = "epil_warn";
static const char *NVS_FONT_SIZE_KEY = "font_size";
static const char *NVS_ROW_HEIGHT_KEY = "row_height";
static const char *NVS_REDUCED_MOTION_KEY = "reduce_motion";
static const char *NVS_INPUT_REPEAT_SPEED_KEY = "repeat_spd";
static const char *NVS_HIGH_CONTRAST_KEY = "high_contrast";
static const char *NVS_SUN_MODE_KEY = "sun_mode";
static const char *NVS_SUN_MODE_SAVED_BRIGHTNESS_KEY = "sun_mode_pb";
static const char *NVS_LOG_LEVEL_KEY = "log_level";
static const char *NVS_FAVORITES_KEY = "favs";
static const char *NVS_FAVS_BYPASS_KEY = "favs_bypass";
static const char *NVS_MENU_ITEM_BORDERS_KEY = "menu_itm_brd";
static const char *NVS_MENU_CARD_BG_KEY = "menu_card_bg";
static const char *NVS_TOUCH_DRAG_SCROLL_KEY = "touch_drg_scr";

// Lockscreen NVS keys
static const char *NVS_LOCKSCREEN_ENABLED_KEY = "ls_en";
static const char *NVS_LOCKSCREEN_TYPE_KEY = "ls_type";
static const char *NVS_LOCKSCREEN_OBF_KEY = "ls_obf";
static const char *NVS_LOCKSCREEN_TIMEOUT_KEY = "ls_tout";
static const char *NVS_LOCKSCREEN_WAKE_KEY = "ls_wake";

// Wardriving NVS keys
static const char *NVS_WD_HOP_PRIMARY_KEY = "wd_hop_prim";
static const char *NVS_WD_HOP_HELPER_KEY = "wd_hop_help";
static const char *NVS_WD_WEIGHTED_5G_KEY = "wd_w5g";

static const char *TAG = "SettingsManager";

static nvs_handle_t nvsHandle;
FSettings G_Settings;

void settings_init(FSettings *settings) {
  settings_set_defaults(settings);
  esp_err_t err = nvs_flash_init();

  if (err == ESP_ERR_NVS_NO_FREE_PAGES || 
      err == ESP_ERR_NVS_NEW_VERSION_FOUND ||
      err == ESP_ERR_NVS_NOT_FOUND) {
      printf("NVS corrupt - erasing partition...");
      esp_err_t erase_err = nvs_flash_erase();
      if (erase_err != ESP_OK) {
          printf("Erase failed: %s", esp_err_to_name(erase_err));
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart(); // Hard reset if erase fails
      }
      err = nvs_flash_init();
  }

  if (err != ESP_OK) {
      printf("NVS FATAL: %s - Rebooting", esp_err_to_name(err));
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
  }

  err = nvs_open("storage", NVS_READWRITE, &nvsHandle);
  if (err == ESP_OK) {
    settings_load(settings);
    printf("Settings loaded successfully.\n");
    settings_print_nvs_stats();
  } else {
    printf("Failed to open NVS handle: %s\n", esp_err_to_name(err));
  }
}

void settings_deinit(void) { nvs_close(nvsHandle); }

void settings_set_defaults(FSettings *settings) {
  menu_config_reset(&settings->menu_config);
  settings->rgb_mode = RGB_MODE_NORMAL;
  settings->channel_delay = 1.0f;
  settings->broadcast_speed = 1;
  settings->hop_mode = 0; // HOP_MODE_DEFAULT
  settings->hop_custom_channels[0] = '\0';
  // default to the 'Bright' palette (index 3)
  settings->menu_theme = 3;
  strcpy(settings->ap_ssid, "GhostNet");
  strcpy(settings->ap_password, "GhostNet");
  settings->rgb_speed = 15;

  // Evil Portal defaults
  strcpy(settings->portal_url, "/default/path");
  strcpy(settings->portal_ssid, "EvilPortal");
  strcpy(settings->portal_password, "");
  strcpy(settings->portal_ap_ssid, "EvilAP");
  strcpy(settings->portal_domain, "portal.local");
  settings->portal_offline_mode = false;

  // Power Printer defaults
  strcpy(settings->printer_ip, "192.168.1.100");
  strcpy(settings->printer_text, "Default Text");
  settings->printer_font_size = 12;
  settings->printer_alignment = ALIGNMENT_CM;
  strcpy(settings->flappy_ghost_name, "Bob");
  strcpy(settings->selected_hex_accent_color, "#ffffff");
  strcpy(settings->selected_timezone, "MST7MDT,M3.2.0,M11.1.0");
  settings->gps_rx_pin = 0;
  settings->gps_baud_rate = 0; // 0 = use CONFIG_GPS_UART_BAUD_RATE
  settings->display_timeout_ms = 30000; // Default to 30 seconds
  settings->rts_enabled = false;
  strcpy(settings->sta_ssid, ""); // Default empty station SSID
  strcpy(settings->sta_password, ""); // Default empty station password
  settings->wifi_auto_reconnect = true; // Auto-reconnect on involuntary disconnect
  settings->rgb_data_pin = -1;
  settings->rgb_red_pin = -1;
  settings->rgb_green_pin = -1;
  settings->rgb_blue_pin = -1;
  settings->third_control_enabled = false;
  settings->terminal_text_color = 0xFFFFFF; // White
  settings->terminal_font_size = 0; // Small (0=Small, 1=Normal, 2=Large)
  settings->invert_colors = false;
  settings->web_auth_enabled = false;
  settings->usb_msc_enabled = false;
  settings->webui_restrict_to_ap = true;
  if (settings_should_use_noop_dualcomm_pins()) {
    settings->esp_comm_tx_pin = -1;
    settings->esp_comm_rx_pin = -1;
  } else {
#ifdef CONFIG_IDF_TARGET_ESP32
    settings->esp_comm_tx_pin = 17;
    settings->esp_comm_rx_pin = 16;
#else
    settings->esp_comm_tx_pin = 6;
    settings->esp_comm_rx_pin = 7;
#endif
  }
  settings->ap_enabled = true; // Default to enabled
  settings->power_save_enabled = false;
  settings->zebra_menus_enabled = false; // or true if you want it enabled by default
  settings->max_screen_brightness = 100; // Default to 100% brightness
  settings->infrared_easy_mode = false; // Default to disabled
  settings->ir_tx_pin = -1;             // -1 = use CONFIG_INFRARED_LED_PIN
  settings->ir_rx_pin = -1;             // -1 = use CONFIG_INFRARED_RX_PIN
  settings->nav_buttons_enabled = true; // Default to enabled
  settings->menu_layout = 1; // Default to adaptive paginated grid
  settings->carousel_invert_direction = false; // Default to non-inverted carousel slide direction
  settings->neopixel_max_brightness = 100; // Default to 100% brightness
  settings->encoder_invert_direction = false;
  settings->rgb_led_count = CONFIG_NUM_LEDS;
  settings->auto_save_scans = true;
  settings->setup_complete = false;
  settings->wifi_country = 0;
  strcpy(settings->wigle_api_key, "");
  settings->wigle_auto_upload = false; // Default to off
  settings->wigle_donate = true; // Default to donating
  settings->ota_channel = 0; // Default to stable channel
  settings->ota_update_available = false;
  settings->ota_last_check_time = 0;
  settings->io_btn_p10_cmd[0] = '\0';
  settings->io_btn_p11_cmd[0] = '\0';
  settings->io_btn_p12_cmd[0] = '\0';
  // MIC RGB Visualizer defaults
  settings->mic_visualizer_mode = MIC_MODE_4BAND_SPECTRUM;
  settings->mic_color_mode = MIC_COLOR_RAINBOW;
  settings->mic_sensitivity = 50; // 50% default
  settings->mic_smoothing = 30; // 30% default
  settings->mic_contrast = 2; // Medium contrast
  settings->mic_mirror_mode = false;
  settings->ghostlink_split_view = true; // Default to split view
  settings->menu_bg_shade = 1;
  settings->theme_background_effects = true;
  settings->menu_rounded = true;
  settings->epilepsy_warning_enabled = true;
  settings->font_size = 1; // Normal (0=Small, 1=Normal, 2=Large)
#ifdef CONFIG_IS_ATOMS3R
  settings->font_size = 0; // Small is the default on the AtomS3R display
#endif
  settings->row_height = 1; // Normal options-list row size
  settings->reduced_motion = false;
  settings->input_repeat_speed = 1; // Normal (0=Slow, 1=Normal, 2=Fast)
  settings->high_contrast = false;
  settings->sun_mode = false;
  settings->sun_mode_saved_brightness = 100;
  settings->log_level = ESP_LOG_WARN;
  settings->favorites_count = 0;
  memset(settings->favorites, 0, sizeof(settings->favorites));
  settings->favorites_bypass_pin = false;
  settings->menu_item_borders = false;
  settings->menu_card_bg = false;
  settings->touch_drag_scroll = true;

  // Wardriving defaults
  settings->wd_hop_primary_ms = 125;
  settings->wd_hop_helper_ms = 125;
  settings->wd_weighted_5g = true;

  // Lockscreen defaults (disabled by default)
  settings->lockscreen_enabled = false;
  settings->lockscreen_type = 1;         // PIN-only for now
  memset(settings->lockscreen_obfuscated, 0, sizeof(settings->lockscreen_obfuscated));
  settings->lockscreen_timeout_sec = 0;    // Off
  settings->lockscreen_wake_lock = true;  // Default to locking on wake

#ifdef CONFIG_WITH_STATUS_DISPLAY
  settings->status_idle_animation = IDLE_ANIM_GAME_OF_LIFE;
  settings->status_idle_timeout_ms = 5000; // default 5s
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  settings->badusb_vid = 0x1209;
  settings->badusb_pid = 0x0001;
  strcpy(settings->badusb_manufacturer, "USB Device");
  strcpy(settings->badusb_product, "HID Keyboard");
  settings->badusb_randomize = false;
  settings->badusb_kb_layout = KB_LAYOUT_US;
#endif
}

void settings_load(FSettings *settings) {
  esp_err_t err;
  uint8_t value_u8;
  uint16_t value_u16;
  uint32_t value_u32;
  float value_float;
  size_t str_size;

  // Load RGB Mode
  err = nvs_get_u8(nvsHandle, NVS_RGB_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->rgb_mode = (RGBMode)value_u8;
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(S_TAG, "Using default RGB mode");
  }

  size_t required_size = sizeof(value_float); // Set the size of the buffer
  err = nvs_get_blob(nvsHandle, NVS_CHANNEL_DELAY_KEY, &value_float,
                     &required_size);
  if (err == ESP_OK) {
    settings->channel_delay = value_float;
  } else {
    printf("Failed to load Channel Delay: %s\n", esp_err_to_name(err));
  }

  // Load Broadcast Speed
  err = nvs_get_u16(nvsHandle, NVS_BROADCAST_SPEED_KEY, &value_u16);
  if (err == ESP_OK) {
    settings->broadcast_speed = value_u16;
  }

  // Load Hop channel profile
  err = nvs_get_u8(nvsHandle, NVS_HOP_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->hop_mode = value_u8;
  }
  str_size = sizeof(settings->hop_custom_channels);
  err = nvs_get_str(nvsHandle, NVS_HOP_CUSTOM_KEY, settings->hop_custom_channels,
                    &str_size);
  if (err != ESP_OK) {
    settings->hop_custom_channels[0] = '\0';
  }

  // Load AP SSID
  str_size = sizeof(settings->ap_ssid);
  err = nvs_get_str(nvsHandle, NVS_AP_SSID_KEY, settings->ap_ssid, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load AP SSID\n");
  }

  // Load AP Password
  str_size = sizeof(settings->ap_password);
  err = nvs_get_str(nvsHandle, NVS_AP_PASSWORD_KEY, settings->ap_password,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load AP Password\n");
  }

  // Load RGB Speed
  err = nvs_get_u8(nvsHandle, NVS_RGB_SPEED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->rgb_speed = value_u8;
  }

  // Load RGB LED Count
  err = nvs_get_u16(nvsHandle, NVS_RGB_LED_COUNT_KEY, &value_u16);
  if (err == ESP_OK && value_u16 != 0) {
    settings->rgb_led_count = value_u16;
  }

  // Load Evil Portal settings
  str_size = sizeof(settings->portal_url);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_URL_KEY, settings->portal_url,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal URL\n");
  }

  str_size = sizeof(settings->portal_ssid);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_SSID_KEY, settings->portal_ssid,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal SSID\n");
  }

  str_size = sizeof(settings->portal_password);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_PASSWORD_KEY,
                    settings->portal_password, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal Password\n");
  }

  str_size = sizeof(settings->portal_ap_ssid);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_AP_SSID_KEY, settings->portal_ap_ssid,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal AP SSID\n");
  }

  str_size = sizeof(settings->portal_domain);
  err = nvs_get_str(nvsHandle, NVS_PORTAL_DOMAIN_KEY, settings->portal_domain,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Portal Domain\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_PORTAL_OFFLINE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->portal_offline_mode = value_u8;
  }

  // Load Power Printer settings
  str_size = sizeof(settings->printer_ip);
  err = nvs_get_str(nvsHandle, NVS_PRINTER_IP_KEY, settings->printer_ip,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Printer IP\n");
  }

  str_size = sizeof(settings->printer_text);
  err = nvs_get_str(nvsHandle, NVS_PRINTER_TEXT_KEY, settings->printer_text,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Printer Text\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_PRINTER_FONT_SIZE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->printer_font_size = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_PRINTER_ALIGNMENT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->printer_alignment = (PrinterAlignment)value_u8;
  }

  str_size = sizeof(settings->flappy_ghost_name);
  err = nvs_get_str(nvsHandle, NVS_FLAPPY_GHOST_NAME,
                    settings->flappy_ghost_name, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Flappy Ghost Name\n");
  }

  str_size = sizeof(settings->selected_timezone);
  err = nvs_get_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone,
                    &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Timezone String\n");
  }

  str_size = sizeof(settings->selected_hex_accent_color);
  err = nvs_get_str(nvsHandle, NVS_ACCENT_COLOR,
                    settings->selected_hex_accent_color, &str_size);
  if (err != ESP_OK) {
    printf("Failed to load Hex Accent Color String\n");
  }

  err = nvs_get_u8(nvsHandle, NVS_GPS_RX_PIN, &value_u8);
  if (err == ESP_OK) {
    settings->gps_rx_pin = value_u8;
  }

  err = nvs_get_u32(nvsHandle, NVS_GPS_BAUD_KEY, &value_u32);
  if (err == ESP_OK) {
    settings->gps_baud_rate = value_u32;
  }

  uint32_t timeout_value;
  err = nvs_get_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, &timeout_value);
  if (err == ESP_OK) {
    settings->display_timeout_ms = timeout_value;
  } else {
    settings->display_timeout_ms = 30000; // Default to 30 seconds if not found
  }

  uint8_t rtsenabledvalue;
  err = nvs_get_u8(nvsHandle, NVS_ENABLE_RTS_KEY, &rtsenabledvalue);
  if (err == ESP_OK) {
    settings->rts_enabled = rtsenabledvalue;
  } else {
    settings->rts_enabled = false;
  }

  uint8_t thirdenabledvalue;
  err = nvs_get_u8(nvsHandle, NVS_THIRD_CTRL_KEY, &thirdenabledvalue);
  if (err == ESP_OK) {
    settings->third_control_enabled = thirdenabledvalue;
  } else {
    settings->third_control_enabled = false;
  }

  // Load Station SSID
  str_size = sizeof(settings->sta_ssid);
  err = nvs_get_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load STA SSID: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->sta_ssid, ""); // Ensure it's empty if not found
  }

  // Load Station Password
  str_size = sizeof(settings->sta_password);
  err = nvs_get_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load STA Password: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->sta_password, ""); // Ensure it's empty if not found
  }

  // Load WiFi auto-reconnect setting
  uint8_t wifi_auto_reconnect_val = 1;
  err = nvs_get_u8(nvsHandle, NVS_WIFI_AUTO_RECONNECT_KEY, &wifi_auto_reconnect_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load WiFi auto-reconnect setting: %s\n", esp_err_to_name(err));
  }
  settings->wifi_auto_reconnect = (wifi_auto_reconnect_val != 0);

  // Load Wigle API key (format: APIName:APIToken)
  str_size = sizeof(settings->wigle_api_key);
  err = nvs_get_str(nvsHandle, NVS_WIGLE_API_KEY, settings->wigle_api_key, &str_size);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle API key: %s\n", esp_err_to_name(err));
  } else if (err == ESP_ERR_NVS_NOT_FOUND) {
    strcpy(settings->wigle_api_key, "");
  }

  // Load Wigle donate setting
  uint8_t donate_val = 1;
  err = nvs_get_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, &donate_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle donate setting: %s\n", esp_err_to_name(err));
  }
  settings->wigle_donate = (donate_val != 0);

  // Load Wigle auto-upload setting
  uint8_t auto_upload_val = 0;
  err = nvs_get_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, &auto_upload_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load Wigle auto-upload setting: %s\n", esp_err_to_name(err));
  }
  settings->wigle_auto_upload = (auto_upload_val != 0);

  // Load OTA settings
  uint8_t ota_channel_val = 0;
  err = nvs_get_u8(nvsHandle, NVS_OTA_CHANNEL_KEY, &ota_channel_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load OTA channel setting: %s\n", esp_err_to_name(err));
  }
  settings->ota_channel = ota_channel_val;

  uint8_t ota_avail_val = 0;
  err = nvs_get_u8(nvsHandle, NVS_OTA_UPDATE_AVAIL_KEY, &ota_avail_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load OTA update-available setting: %s\n", esp_err_to_name(err));
  }
  settings->ota_update_available = (ota_avail_val != 0);

  uint32_t ota_last_check_val = 0;
  err = nvs_get_u32(nvsHandle, NVS_OTA_LAST_CHECK_KEY, &ota_last_check_val);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    printf("Failed to load OTA last-check setting: %s\n", esp_err_to_name(err));
  }
  settings->ota_last_check_time = ota_last_check_val;

  str_size = sizeof(settings->io_btn_p10_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P10_CMD_KEY, settings->io_btn_p10_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p10_cmd[0] = '\0';
  str_size = sizeof(settings->io_btn_p11_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P11_CMD_KEY, settings->io_btn_p11_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p11_cmd[0] = '\0';
  str_size = sizeof(settings->io_btn_p12_cmd);
  err = nvs_get_str(nvsHandle, NVS_IO_BTN_P12_CMD_KEY, settings->io_btn_p12_cmd, &str_size);
  if (err != ESP_OK) settings->io_btn_p12_cmd[0] = '\0';

  printf("Settings loaded from NVS.\n");
  int32_t tmp;
  err = nvs_get_i32(nvsHandle, NVS_RGB_DATA_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_data_pin = tmp;
  } else {
    settings->rgb_data_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_RED_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_red_pin = tmp;
  } else {
    settings->rgb_red_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_GREEN_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_green_pin = tmp;
  } else {
    settings->rgb_green_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_RGB_BLUE_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->rgb_blue_pin = tmp;
  } else {
    settings->rgb_blue_pin = -1;
  }

  err = nvs_get_u8(nvsHandle, NVS_MENU_THEME_KEY, &value_u8);
  if (err == ESP_OK) {
    /* Schema 0 = pre-descriptor accent presets; remap once to palette IDs. */
    uint8_t schema = 0;
    bool migrate_theme = nvs_get_u8(nvsHandle, NVS_THEME_SCHEMA_KEY, &schema) != ESP_OK || schema == 0;
    settings->menu_theme = migrate_theme ? theme_palette_migrate_legacy(value_u8)
                                         : theme_palette_clamp_id(value_u8);
    if (migrate_theme) {
      if (nvs_set_u8(nvsHandle, NVS_THEME_SCHEMA_KEY, 1) == ESP_OK) {
        nvs_commit(nvsHandle);
      }
    }
  } else {
    /* Fresh device: stamp the current schema so nothing remaps later. */
    uint8_t schema = 0;
    if (nvs_get_u8(nvsHandle, NVS_THEME_SCHEMA_KEY, &schema) != ESP_OK) {
      nvs_set_u8(nvsHandle, NVS_THEME_SCHEMA_KEY, 1);
      nvs_commit(nvsHandle);
    }
  }
  err = nvs_get_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, &value_u32);
  if (err == ESP_OK) {
    settings->terminal_text_color = value_u32;
  }
  err = nvs_get_u8(nvsHandle, NVS_TERMINAL_FONT_SIZE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->terminal_font_size = value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_INVERT_COLORS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->invert_colors = (value_u8 != 0);
  }

  err = nvs_get_u8(nvsHandle, NVS_WEB_AUTH_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->web_auth_enabled = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_USB_MSC_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->usb_msc_enabled = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->webui_restrict_to_ap = value_u8;
  }

  err = nvs_get_u8(nvsHandle, NVS_AP_ENABLED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->ap_enabled = (value_u8 != 0);
  } else {
    settings->ap_enabled = true; // Default to enabled if not found
  }

  err = nvs_get_u8(nvsHandle, NVS_POWER_SAVE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->power_save_enabled = (value_u8 != 0);
  } else {
    settings->power_save_enabled = false; // Default to disabled if not found
  }

  err = nvs_get_i32(nvsHandle, NVS_ESP_COMM_TX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->esp_comm_tx_pin = tmp;
  } else {
#ifdef CONFIG_IDF_TARGET_ESP32
    settings->esp_comm_tx_pin = 17;
#else
    settings->esp_comm_tx_pin = 6;
#endif
  }

  err = nvs_get_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->esp_comm_rx_pin = tmp;
  } else {
#ifdef CONFIG_IDF_TARGET_ESP32
    settings->esp_comm_rx_pin = 16;
#else
    settings->esp_comm_rx_pin = 7;
#endif
  }

  if (settings_should_use_noop_dualcomm_pins()) {
    settings->esp_comm_tx_pin = -1;
    settings->esp_comm_rx_pin = -1;
  }

  err = nvs_get_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->zebra_menus_enabled = (value_u8 != 0);
  } else {
    settings->zebra_menus_enabled = false;
  } // Default to disabled if not found
  // Load Max Screen Brightness
  err = nvs_get_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->max_screen_brightness = value_u8;
  } else {
    settings->max_screen_brightness = 100; // Default to 100% if not found
  }

  // Load Infrared Easy Mode
  err = nvs_get_u8(nvsHandle, NVS_INFRARED_EASY_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->infrared_easy_mode = (bool)value_u8;
  } else {
    settings->infrared_easy_mode = false; // Default to disabled if not found
  }

  // Load IR pin overrides (-1 = use compiled default)
  err = nvs_get_i32(nvsHandle, NVS_IR_TX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->ir_tx_pin = tmp;
  } else {
    settings->ir_tx_pin = -1;
  }
  err = nvs_get_i32(nvsHandle, NVS_IR_RX_PIN_KEY, &tmp);
  if (err == ESP_OK) {
    settings->ir_rx_pin = tmp;
  } else {
    settings->ir_rx_pin = -1;
  }

  // Load Navigation Buttons Enabled
  err = nvs_get_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->nav_buttons_enabled = (bool)value_u8;
  } else {
    settings->nav_buttons_enabled = true; // Default to enabled if not found
  }

  // Load Auto Save Scans
  err = nvs_get_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->auto_save_scans = (bool)value_u8;
  } else {
    settings->auto_save_scans = true; // Default to enabled if not found
  }

  // Load Menu Layout
  err = nvs_get_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_layout = value_u8;
  } else {
    settings->menu_layout = 1; // Default to adaptive paginated grid if not found
  }

  // Load carousel slide direction inversion
  err = nvs_get_u8(nvsHandle, NVS_CAROUSEL_INVERT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->carousel_invert_direction = (bool)value_u8;
  } else {
    settings->carousel_invert_direction = false;
  }

  // Load Neopixel Max Brightness
  err = nvs_get_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->neopixel_max_brightness = value_u8;
  } else {
    settings->neopixel_max_brightness = 100; // Default to 100% if not found
  }

  // Load encoder direction inversion
  err = nvs_get_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->encoder_invert_direction = (bool)value_u8;
  } else {
    settings->encoder_invert_direction = false;
  }

  err = nvs_get_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->setup_complete = (bool)value_u8;
  } else {
    settings->setup_complete = false;
  }

  err = nvs_get_u8(nvsHandle, NVS_WIFI_COUNTRY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->wifi_country = value_u8;
  } else {
    settings->wifi_country = 0;
  }

#ifdef CONFIG_WITH_STATUS_DISPLAY
  err = nvs_get_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->status_idle_animation = (IdleAnimation)value_u8;
  } else {
    // try legacy key that exceeded length (for migration, if it ever existed)
    const char *legacy_key = "status_idle_anim";
    esp_err_t err2 = nvs_get_u8(nvsHandle, legacy_key, &value_u8);
    if (err2 == ESP_OK) {
      settings->status_idle_animation = (IdleAnimation)value_u8;
      // re-save under the new shorter key
      nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, value_u8);
    } else {
      settings->status_idle_animation = IDLE_ANIM_GAME_OF_LIFE;
    }
  }
  // load idle timeout
  err = nvs_get_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, &value_u32);
  if (err == ESP_OK) {
    settings->status_idle_timeout_ms = value_u32;
  } else {
    settings->status_idle_timeout_ms = 5000; // default 5s
  }
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  err = nvs_get_u16(nvsHandle, NVS_BADUSB_VID_KEY, &value_u16);
  if (err == ESP_OK) settings->badusb_vid = value_u16;

  err = nvs_get_u16(nvsHandle, NVS_BADUSB_PID_KEY, &value_u16);
  if (err == ESP_OK) settings->badusb_pid = value_u16;

  str_size = sizeof(settings->badusb_manufacturer);
  err = nvs_get_str(nvsHandle, NVS_BADUSB_MFR_KEY, settings->badusb_manufacturer, &str_size);

  str_size = sizeof(settings->badusb_product);
  err = nvs_get_str(nvsHandle, NVS_BADUSB_PROD_KEY, settings->badusb_product, &str_size);

  err = nvs_get_u8(nvsHandle, NVS_BADUSB_RAND_KEY, &value_u8);
  if (err == ESP_OK) settings->badusb_randomize = (bool)value_u8;

  err = nvs_get_u8(nvsHandle, NVS_BADUSB_KB_KEY, &value_u8);
  if (err == ESP_OK) settings->badusb_kb_layout = value_u8;
#endif

  // Load MIC RGB Visualizer settings
  err = nvs_get_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_visualizer_mode = (MicVisualizerMode)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_color_mode = (MicColorMode)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_sensitivity = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_smoothing = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_contrast = value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->mic_mirror_mode = (bool)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->ghostlink_split_view = (bool)value_u8;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_bg_shade = value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_THEME_BG_EFFECTS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->theme_background_effects = value_u8 != 0;
  }
  
  err = nvs_get_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_rounded = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_EPILEPSY_WARNING_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->epilepsy_warning_enabled = (bool)value_u8;
  } else {
    settings->epilepsy_warning_enabled = true; // Default to enabled
  }

  err = nvs_get_u8(nvsHandle, NVS_FONT_SIZE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->font_size = value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_ROW_HEIGHT_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->row_height = value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_REDUCED_MOTION_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->reduced_motion = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_INPUT_REPEAT_SPEED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->input_repeat_speed = value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_HIGH_CONTRAST_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->high_contrast = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_SUN_MODE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->sun_mode = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_SUN_MODE_SAVED_BRIGHTNESS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->sun_mode_saved_brightness = value_u8;
  }
  settings_normalize_modes(settings);
  err = nvs_get_u8(nvsHandle, NVS_LOG_LEVEL_KEY, &value_u8);
  if (err == ESP_OK && value_u8 <= ESP_LOG_VERBOSE) {
    settings->log_level = value_u8;
  }
  size_t menu_config_size = sizeof(settings->menu_config);
  menu_config_reset(&settings->menu_config);
  err = nvs_get_blob(nvsHandle, "menu_config", &settings->menu_config, &menu_config_size);
  if (err != ESP_OK || menu_config_size != menu_config_storage_size(&settings->menu_config)) {
    menu_config_reset(&settings->menu_config);
  }
  menu_config_validate(&settings->menu_config);

  size_t req_sz = 0;
  err = nvs_get_blob(nvsHandle, NVS_FAVORITES_KEY, NULL, &req_sz);
  if (err == ESP_OK) {
    /* Blob layouts seen in the wild (oldest first). Legacy sizes are pinned
     * to the original 8-slot array so raising FAVORITES_MAX never makes an
     * old blob match a new-format size check:
     *   [8 x 32B]              = 256   raw, no count
     *   [count][8 x 32B]       = 257
     *   [8 x 64B]              = 512   raw, no count
     *   [count][8 x 64B]       = 513
     *   [count][MAX x 64B]     = current format */
    int legacy_slots = -1;
    int slot_len = 0;
    bool has_count = false;
    if (req_sz == (size_t)(1 + sizeof(settings->favorites))) {
      uint8_t *blob = (uint8_t *)malloc(req_sz);
      if (blob) {
        size_t r = req_sz;
        if (nvs_get_blob(nvsHandle, NVS_FAVORITES_KEY, blob, &r) == ESP_OK && r == req_sz) {
          uint8_t c = blob[0];
          if (c <= FAVORITES_MAX) {
            settings->favorites_count = c;
            memcpy(settings->favorites, blob + 1, sizeof(settings->favorites));
          }
        }
        free(blob);
      }
    } else if (req_sz == sizeof(settings->favorites)) {
      // Same generation but without the leading count byte.
      size_t r = req_sz;
      if (nvs_get_blob(nvsHandle, NVS_FAVORITES_KEY, settings->favorites, &r) == ESP_OK) {
        uint8_t cnt = 0;
        for (int i = 0; i < FAVORITES_MAX; i++) if (settings->favorites[i][0]) cnt++;
        settings->favorites_count = cnt;
      }
    } else if (req_sz == 1 + 8 * 64) { legacy_slots = 8; slot_len = 64; has_count = true; }
    else if (req_sz == 8 * 64)      { legacy_slots = 8; slot_len = 64; }
    else if (req_sz == 1 + 8 * 32)  { legacy_slots = 8; slot_len = 32; has_count = true; }
    else if (req_sz == 8 * 32)      { legacy_slots = 8; slot_len = 32; }

    if (legacy_slots > 0 && slot_len > 0) {
      // Migrate an 8-slot blob into the current FAVORITES_MAX-slot layout.
      uint8_t *blob = (uint8_t *)malloc(req_sz);
      if (blob) {
        size_t r = req_sz;
        if (nvs_get_blob(nvsHandle, NVS_FAVORITES_KEY, blob, &r) == ESP_OK && r == req_sz) {
          memset(settings->favorites, 0, sizeof(settings->favorites));
          int c;
          if (has_count) {
            c = blob[0];
            if (c > legacy_slots) c = legacy_slots;
            if (c > FAVORITES_MAX) c = FAVORITES_MAX;
            for (int i = 0; i < c; i++) {
              const char *src = (const char *)blob + 1 + i * slot_len;
              size_t cp = (slot_len < FAVORITE_NAME_LEN) ? (size_t)slot_len : (size_t)(FAVORITE_NAME_LEN - 1);
              memcpy(settings->favorites[i], src, cp);
              settings->favorites[i][cp] = '\0';
            }
          } else {
            // Raw layout: carry over every non-empty slot in order.
            c = 0;
            for (int i = 0; i < legacy_slots && c < FAVORITES_MAX; i++) {
              const char *src = (const char *)blob + i * slot_len;
              if (!src[0]) continue;
              size_t cp = (slot_len < FAVORITE_NAME_LEN) ? (size_t)slot_len : (size_t)(FAVORITE_NAME_LEN - 1);
              memcpy(settings->favorites[c], src, cp);
              settings->favorites[c][cp] = '\0';
              c++;
            }
          }
          settings->favorites_count = (uint8_t)c;
        }
        free(blob);
      }
    }
  }
  err = nvs_get_u8(nvsHandle, NVS_FAVS_BYPASS_KEY, &value_u8);
  if (err == ESP_OK) settings->favorites_bypass_pin = (bool)value_u8;
  err = nvs_get_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_item_borders = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_MENU_CARD_BG_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->menu_card_bg = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_TOUCH_DRAG_SCROLL_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->touch_drag_scroll = (bool)value_u8;
  }

  // Load lockscreen settings
  err = nvs_get_u8(nvsHandle, NVS_LOCKSCREEN_ENABLED_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->lockscreen_enabled = (bool)value_u8;
  }
  err = nvs_get_u8(nvsHandle, NVS_LOCKSCREEN_TYPE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->lockscreen_type = value_u8;
  }
  size_t blob_size = sizeof(settings->lockscreen_obfuscated);
  err = nvs_get_blob(nvsHandle, NVS_LOCKSCREEN_OBF_KEY, settings->lockscreen_obfuscated, &blob_size);
  if (err != ESP_OK) {
    memset(settings->lockscreen_obfuscated, 0, sizeof(settings->lockscreen_obfuscated));
  }
  settings->lockscreen_type = 1;
  uint8_t lockscreen_stored_len = (uint8_t)settings->lockscreen_obfuscated[0];
  if ((lockscreen_stored_len & 0x80) != 0 &&
      (lockscreen_stored_len & 0x7F) >= sizeof(settings->lockscreen_obfuscated)) {
    memset(settings->lockscreen_obfuscated, 0, sizeof(settings->lockscreen_obfuscated));
  }
  value_u16 = 0;
  err = nvs_get_u16(nvsHandle, NVS_LOCKSCREEN_TIMEOUT_KEY, &value_u16);
  if (err == ESP_OK) {
    settings->lockscreen_timeout_sec = value_u16;
  }
  err = nvs_get_u8(nvsHandle, NVS_LOCKSCREEN_WAKE_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->lockscreen_wake_lock = (bool)value_u8;
  }

  // Load wardriving settings
  uint16_t value_u16_wd = 0;
  err = nvs_get_u16(nvsHandle, NVS_WD_HOP_PRIMARY_KEY, &value_u16_wd);
  if (err == ESP_OK) {
    settings->wd_hop_primary_ms = value_u16_wd;
  }
  err = nvs_get_u16(nvsHandle, NVS_WD_HOP_HELPER_KEY, &value_u16_wd);
  if (err == ESP_OK) {
    settings->wd_hop_helper_ms = value_u16_wd;
  }
  err = nvs_get_u8(nvsHandle, NVS_WD_WEIGHTED_5G_KEY, &value_u8);
  if (err == ESP_OK) {
    settings->wd_weighted_5g = (bool)value_u8;
  }
}

static void update_rainbow_effect(const FSettings *settings) {
#ifndef CONFIG_WITH_SCREEN
  return;
#endif

  if (settings_get_rgb_mode(settings) == RGB_MODE_RAINBOW) {
    display_manager_set_rainbow_mode(true);
  } else {
    display_manager_set_rainbow_mode(false);
  }
}


void settings_restart_rgb_effect(void) {
    ESP_LOGI(TAG, "Restarting RGB effect...");
    
    // 1. Signal any existing task to stop
    // 1. Signal any existing task to stop
    if (rgb_effect_task_handle != NULL) {
        rgb_manager_signal_rainbow_exit();
        vTaskDelay(pdMS_TO_TICKS(50));

        rgb_effect_task_handle = NULL;
    }
    
    // Force cleanup of status bar rainbow effect if we are switching away from RAINBOW
    // Use the *new* value directly from G_Settings to be sure.
    if (settings_get_rgb_mode(&G_Settings) != RGB_MODE_RAINBOW) {
        // We call update_rainbow_effect to ensure the timer is deleted
        update_rainbow_effect(&G_Settings);
        // And explicitly force the status bar color update just in case
        display_manager_update_status_bar_color();
    }

    // 4. Start new task based on mode
    RGBMode mode = settings_get_rgb_mode(&G_Settings);
    if (mode == RGB_MODE_RAINBOW) {
#if RGB_EFFECT_USE_PINNED_API
        xTaskCreatePinnedToCore(rainbow_task, "Rainbow Task", 3072, &rgb_manager,
                                RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle,
                                RGB_EFFECT_TASK_CORE);
#else
        xTaskCreate(rainbow_task, "Rainbow Task", 3072, &rgb_manager,
                    RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
#endif
    } else if (mode == RGB_MODE_KNIGHT_RIDER) {
         xTaskCreate(knightrider_task, "Knight Rider Task", 3072, &rgb_manager,
                    RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
    } else if (mode == RGB_MODE_STEALTH) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    } else if (mode == RGB_MODE_RED) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 0, 0, false);
    } else if (mode == RGB_MODE_GREEN) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 255, 0, false);
    } else if (mode == RGB_MODE_BLUE) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 255, false);
    } else if (mode == RGB_MODE_YELLOW) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 255, 0, false);
    } else if (mode == RGB_MODE_PURPLE) {
        rgb_manager_set_color(&rgb_manager, -1, 115, 0, 225, false);
    } else if (mode == RGB_MODE_CYAN) {
        rgb_manager_set_color(&rgb_manager, -1, 0, 255, 255, false);
    } else if (mode == RGB_MODE_ORANGE) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 165, 0, false);
    } else if (mode == RGB_MODE_WHITE) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 255, 255, false);
    } else if (mode == RGB_MODE_PINK) {
        rgb_manager_set_color(&rgb_manager, -1, 255, 192, 203, false);
    } else if (mode == RGB_MODE_MIC_VISUALIZER) {
        // MIC visualizer mode - LEDs are controlled by GhostLink stream
        // Just clear LEDs here, the stream handler will take over
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
        ESP_LOGI(TAG, "RGB Mode: MIC Visualizer (controlled via GhostLink)");
    } else {
        // Normal mode
        rgb_manager_set_color(&rgb_manager, -1, 0, 0, 0, false);
    }

    update_rainbow_effect(&G_Settings); // Start/stop global timer for status bar if needed
}

void settings_persist_setting(SettingsType setting) {
    esp_err_t err = ESP_OK;
    const char *key = NULL;

    switch (setting) {
        case SETTING_RGB_MODE:
            err = nvs_set_u8(nvsHandle, NVS_RGB_MODE_KEY, (uint8_t)G_Settings.rgb_mode);
            key = NVS_RGB_MODE_KEY;
            break;
        case SETTING_DISPLAY_TIMEOUT:
            err = nvs_set_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, G_Settings.display_timeout_ms);
            key = NVS_DISPLAY_TIMEOUT_KEY;
            break;
        case SETTING_MENU_THEME:
            err = nvs_set_u8(nvsHandle, NVS_MENU_THEME_KEY, G_Settings.menu_theme);
            key = NVS_MENU_THEME_KEY;
            break;
        case SETTING_THIRD_CONTROL:
            err = nvs_set_u8(nvsHandle, NVS_THIRD_CTRL_KEY, G_Settings.third_control_enabled);
            key = NVS_THIRD_CTRL_KEY;
            break;
        case SETTING_TERMINAL_COLOR:
            err = nvs_set_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, G_Settings.terminal_text_color);
            key = NVS_TERMINAL_TEXT_COLOR_KEY;
            break;
        case SETTING_TERMINAL_FONT_SIZE:
            err = nvs_set_u8(nvsHandle, NVS_TERMINAL_FONT_SIZE_KEY, G_Settings.terminal_font_size);
            key = NVS_TERMINAL_FONT_SIZE_KEY;
            break;
        case SETTING_INVERT_COLORS:
            err = nvs_set_u8(nvsHandle, NVS_INVERT_COLORS_KEY, G_Settings.invert_colors);
            key = NVS_INVERT_COLORS_KEY;
            break;
        case SETTING_WEB_AUTH:
            err = nvs_set_u8(nvsHandle, NVS_WEB_AUTH_KEY, G_Settings.web_auth_enabled);
            key = NVS_WEB_AUTH_KEY;
            break;
#ifdef CONFIG_HAS_USB_MSC_SD
        case SETTING_USB_MSC:
            err = nvs_set_u8(nvsHandle, NVS_USB_MSC_KEY, G_Settings.usb_msc_enabled);
            key = NVS_USB_MSC_KEY;
            break;
#endif
        case SETTING_WEBUI_AP_ONLY:
            err = nvs_set_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, G_Settings.webui_restrict_to_ap);
            key = NVS_WEBUI_AP_ONLY_KEY;
            break;
        case SETTING_AP_ENABLED:
            err = nvs_set_u8(nvsHandle, NVS_AP_ENABLED_KEY, G_Settings.ap_enabled);
            key = NVS_AP_ENABLED_KEY;
            break;
        case SETTING_COUNTRY:
            err = nvs_set_u8(nvsHandle, NVS_WIFI_COUNTRY_KEY, G_Settings.wifi_country);
            key = NVS_WIFI_COUNTRY_KEY;
            break;
        case SETTING_POWER_SAVE:
            err = nvs_set_u8(nvsHandle, NVS_POWER_SAVE_KEY, G_Settings.power_save_enabled);
            key = NVS_POWER_SAVE_KEY;
            break;
        case SETTING_MAX_BRIGHTNESS:
            err = nvs_set_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, G_Settings.max_screen_brightness);
            key = NVS_MAX_SCREEN_BRIGHTNESS_KEY;
            break;
        case SETTING_NEOPIXEL_BRIGHTNESS:
            err = nvs_set_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, G_Settings.neopixel_max_brightness);
            key = NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY;
            break;
        case SETTING_ZEBRA_MENUS:
            err = nvs_set_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, G_Settings.zebra_menus_enabled);
            key = NVS_ZEBRA_MENUS_KEY;
            break;
        case SETTING_NAV_BUTTONS:
            err = nvs_set_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, G_Settings.nav_buttons_enabled);
            key = NVS_NAV_BUTTONS_KEY;
            break;
        case SETTING_AUTO_SAVE_SCANS:
            err = nvs_set_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, G_Settings.auto_save_scans);
            key = NVS_AUTO_SAVE_SCANS_KEY;
            break;
        case SETTING_MENU_LAYOUT:
            err = nvs_set_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, G_Settings.menu_layout);
            key = NVS_MENU_LAYOUT_KEY;
            break;
        case SETTING_CAROUSEL_INVERT_DIRECTION:
            err = nvs_set_u8(nvsHandle, NVS_CAROUSEL_INVERT_KEY, G_Settings.carousel_invert_direction);
            key = NVS_CAROUSEL_INVERT_KEY;
            break;
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETTING_IDLE_ANIMATION:
            err = nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, (uint8_t)G_Settings.status_idle_animation);
            key = NVS_STATUS_IDLE_ANIM_KEY;
            break;
        case SETTING_IDLE_ANIM_DELAY:
            err = nvs_set_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, G_Settings.status_idle_timeout_ms);
            key = NVS_STATUS_IDLE_TIMEOUT_KEY;
            break;
#endif
#ifdef CONFIG_USE_ENCODER
        case SETTING_ENCODER_INVERT:
            err = nvs_set_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, G_Settings.encoder_invert_direction);
            key = NVS_ENCODER_INVERT_KEY;
            break;
#endif
#if CONFIG_IDF_TARGET_ESP32S3
        case SETTING_USB_HOST_MODE:
             break;
#endif
        case SETTING_RUN_SETUP_WIZARD:
        case SETTING_I2C_SCAN:
        case SETTING_GL_BENCH:
        case SETTING_WIGLE_TEST_API:
        case SETTING_WIGLE_HELP:
        case SETTING_WIGLE_MANUAL_UPLOAD:
        case SETTING_WIGLE_STATS:
        case SETTING_LOAD_CONFIG:
        case SETTING_EXPORT_SETTINGS_SD:
        case SETTING_IMPORT_SETTINGS_SD:
        case SETTING_FACTORY_RESET:
        case SETTING_MANAGE_FAVORITES:
             // Actions, not saved
             return;
        case SETTING_SETUP_COMPLETE:
            err = nvs_set_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, G_Settings.setup_complete ? 1 : 0);
            key = NVS_SETUP_COMPLETE_KEY;
            break;
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
        case SETTING_BADUSB_VID:
            err = nvs_set_u16(nvsHandle, NVS_BADUSB_VID_KEY, G_Settings.badusb_vid);
            key = NVS_BADUSB_VID_KEY;
            break;
        case SETTING_BADUSB_PID:
            err = nvs_set_u16(nvsHandle, NVS_BADUSB_PID_KEY, G_Settings.badusb_pid);
            key = NVS_BADUSB_PID_KEY;
            break;
        case SETTING_BADUSB_MANUFACTURER:
            err = nvs_set_str(nvsHandle, NVS_BADUSB_MFR_KEY, G_Settings.badusb_manufacturer);
            key = NVS_BADUSB_MFR_KEY;
            break;
        case SETTING_BADUSB_PRODUCT:
            err = nvs_set_str(nvsHandle, NVS_BADUSB_PROD_KEY, G_Settings.badusb_product);
            key = NVS_BADUSB_PROD_KEY;
            break;
        case SETTING_BADUSB_RANDOMIZE:
            err = nvs_set_u8(nvsHandle, NVS_BADUSB_RAND_KEY, G_Settings.badusb_randomize ? 1 : 0);
            key = NVS_BADUSB_RAND_KEY;
            break;
        case SETTING_BADUSB_KB_LAYOUT:
            err = nvs_set_u8(nvsHandle, NVS_BADUSB_KB_KEY, G_Settings.badusb_kb_layout);
            key = NVS_BADUSB_KB_KEY;
            break;
#endif
        case SETTING_WIGLE_API_KEY:
            err = nvs_set_str(nvsHandle, NVS_WIGLE_API_KEY, G_Settings.wigle_api_key);
            key = NVS_WIGLE_API_KEY;
            break;
        case SETTING_WIGLE_AUTO_UPLOAD:
            err = nvs_set_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, G_Settings.wigle_auto_upload ? 1 : 0);
            key = NVS_WIGLE_AUTO_UPLOAD_KEY;
            break;
        case SETTING_WIGLE_DONATE:
            err = nvs_set_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, G_Settings.wigle_donate ? 1 : 0);
            key = NVS_WIGLE_DONATE_KEY;
            break;
        case SETTING_OTA_CHANNEL:
            err = nvs_set_u8(nvsHandle, NVS_OTA_CHANNEL_KEY, G_Settings.ota_channel);
            key = NVS_OTA_CHANNEL_KEY;
            break;
        case SETTING_OTA_UPDATE_AVAILABLE:
            err = nvs_set_u8(nvsHandle, NVS_OTA_UPDATE_AVAIL_KEY, G_Settings.ota_update_available ? 1 : 0);
            key = NVS_OTA_UPDATE_AVAIL_KEY;
            break;
        case SETTING_OTA_LAST_CHECK_TIME:
            err = nvs_set_u32(nvsHandle, NVS_OTA_LAST_CHECK_KEY, G_Settings.ota_last_check_time);
            key = NVS_OTA_LAST_CHECK_KEY;
            break;
        case SETTING_MIC_VISUALIZER_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, (uint8_t)G_Settings.mic_visualizer_mode);
            key = NVS_MIC_VISUALIZER_MODE_KEY;
            break;
        case SETTING_MIC_COLOR_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, (uint8_t)G_Settings.mic_color_mode);
            key = NVS_MIC_COLOR_MODE_KEY;
            break;
        case SETTING_MIC_SENSITIVITY:
            err = nvs_set_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, G_Settings.mic_sensitivity);
            key = NVS_MIC_SENSITIVITY_KEY;
            break;
        case SETTING_MIC_SMOOTHING:
            err = nvs_set_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, G_Settings.mic_smoothing);
            key = NVS_MIC_SMOOTHING_KEY;
            break;
        case SETTING_MIC_CONTRAST:
            err = nvs_set_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, G_Settings.mic_contrast);
            key = NVS_MIC_CONTRAST_KEY;
            break;
        case SETTING_MIC_MIRROR_MODE:
            err = nvs_set_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, G_Settings.mic_mirror_mode ? 1 : 0);
            key = NVS_MIC_MIRROR_MODE_KEY;
            break;
        case SETTING_MIC_CALIBRATE:
            // Action only, not persisted
            return;
        case SETTING_GHOSTLINK_SPLIT_VIEW:
            err = nvs_set_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, G_Settings.ghostlink_split_view ? 1 : 0);
            key = NVS_GHOSTLINK_SPLIT_VIEW_KEY;
            break;
        case SETTING_MENU_BG_SHADE:
            err = nvs_set_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, G_Settings.menu_bg_shade);
            key = NVS_MENU_BG_SHADE_KEY;
            break;
        case SETTING_THEME_BACKGROUND_EFFECTS:
            err = nvs_set_u8(nvsHandle, NVS_THEME_BG_EFFECTS_KEY, G_Settings.theme_background_effects ? 1 : 0);
            key = NVS_THEME_BG_EFFECTS_KEY;
            break;
        case SETTING_MENU_ROUNDED:
            err = nvs_set_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, G_Settings.menu_rounded ? 1 : 0);
            key = NVS_MENU_ROUNDED_KEY;
            break;
        case SETTING_EPILEPSY_WARNING:
            err = nvs_set_u8(nvsHandle, NVS_EPILEPSY_WARNING_KEY, G_Settings.epilepsy_warning_enabled ? 1 : 0);
            key = NVS_EPILEPSY_WARNING_KEY;
            break;
        case SETTING_FONT_SIZE:
            err = nvs_set_u8(nvsHandle, NVS_FONT_SIZE_KEY, G_Settings.font_size);
            key = NVS_FONT_SIZE_KEY;
            break;
        case SETTING_ROW_HEIGHT:
            err = nvs_set_u8(nvsHandle, NVS_ROW_HEIGHT_KEY, G_Settings.row_height);
            key = NVS_ROW_HEIGHT_KEY;
            break;
        case SETTING_REDUCED_MOTION:
            err = nvs_set_u8(nvsHandle, NVS_REDUCED_MOTION_KEY, G_Settings.reduced_motion ? 1 : 0);
            key = NVS_REDUCED_MOTION_KEY;
            break;
        case SETTING_INPUT_REPEAT_SPEED:
            err = nvs_set_u8(nvsHandle, NVS_INPUT_REPEAT_SPEED_KEY, G_Settings.input_repeat_speed);
            key = NVS_INPUT_REPEAT_SPEED_KEY;
            break;
        case SETTING_HIGH_CONTRAST:
            err = nvs_set_u8(nvsHandle, NVS_HIGH_CONTRAST_KEY, G_Settings.high_contrast ? 1 : 0);
            key = NVS_HIGH_CONTRAST_KEY;
            break;
        case SETTING_SUN_MODE:
            err = nvs_set_u8(nvsHandle, NVS_SUN_MODE_KEY, G_Settings.sun_mode ? 1 : 0);
            key = NVS_SUN_MODE_KEY;
            nvs_set_u8(nvsHandle, NVS_SUN_MODE_SAVED_BRIGHTNESS_KEY, G_Settings.sun_mode_saved_brightness);
            break;
        case SETTING_LOG_LEVEL:
            err = nvs_set_u8(nvsHandle, NVS_LOG_LEVEL_KEY, G_Settings.log_level);
            key = NVS_LOG_LEVEL_KEY;
            break;
        case SETTING_FAVORITES: {
            uint8_t blob[1 + sizeof(G_Settings.favorites)];
            blob[0] = G_Settings.favorites_count;
            memcpy(blob + 1, G_Settings.favorites, sizeof(G_Settings.favorites));
            err = nvs_set_blob(nvsHandle, NVS_FAVORITES_KEY, blob, sizeof(blob));
            key = NVS_FAVORITES_KEY;
            break;
        }
        case SETTING_FAVORITES_BYPASS:
            err = nvs_set_u8(nvsHandle, NVS_FAVS_BYPASS_KEY, G_Settings.favorites_bypass_pin ? 1 : 0);
            key = NVS_FAVS_BYPASS_KEY;
            break;
        case SETTING_MENU_CONFIG:
            err = nvs_set_blob(nvsHandle, "menu_config", &G_Settings.menu_config, menu_config_storage_size(&G_Settings.menu_config));
            key = "menu_config";
            break;
        case SETTING_MENU_ITEM_BORDERS:
            err = nvs_set_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, G_Settings.menu_item_borders ? 1 : 0);
            key = NVS_MENU_ITEM_BORDERS_KEY;
            break;
        case SETTING_MENU_CARD_BG:
            err = nvs_set_u8(nvsHandle, NVS_MENU_CARD_BG_KEY, G_Settings.menu_card_bg ? 1 : 0);
            key = NVS_MENU_CARD_BG_KEY;
            break;
        case SETTING_TOUCH_DRAG_SCROLL:
            err = nvs_set_u8(nvsHandle, NVS_TOUCH_DRAG_SCROLL_KEY, G_Settings.touch_drag_scroll ? 1 : 0);
            key = NVS_TOUCH_DRAG_SCROLL_KEY;
            break;
        case SETTING_LOCKSCREEN_ENABLED:
            err = nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_ENABLED_KEY, G_Settings.lockscreen_enabled ? 1 : 0);
            key = NVS_LOCKSCREEN_ENABLED_KEY;
            break;
        case SETTING_LOCKSCREEN_WAKE:
            err = nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_WAKE_KEY, G_Settings.lockscreen_wake_lock ? 1 : 0);
            key = NVS_LOCKSCREEN_WAKE_KEY;
            break;
        case SETTING_LOCKSCREEN_TYPE:
            err = nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_TYPE_KEY, G_Settings.lockscreen_type);
            key = NVS_LOCKSCREEN_TYPE_KEY;
            break;
        case SETTING_LOCKSCREEN_TIMEOUT:
            err = nvs_set_u16(nvsHandle, NVS_LOCKSCREEN_TIMEOUT_KEY, G_Settings.lockscreen_timeout_sec);
            key = NVS_LOCKSCREEN_TIMEOUT_KEY;
            break;
        case SETTING_LOCKSCREEN_CHANGE_PIN:
            err = nvs_set_blob(nvsHandle, NVS_LOCKSCREEN_OBF_KEY, G_Settings.lockscreen_obfuscated, sizeof(G_Settings.lockscreen_obfuscated));
            key = NVS_LOCKSCREEN_OBF_KEY;
            break;
        case SETTING_WD_HOP_PRIMARY:
            err = nvs_set_u16(nvsHandle, NVS_WD_HOP_PRIMARY_KEY, G_Settings.wd_hop_primary_ms);
            key = NVS_WD_HOP_PRIMARY_KEY;
            break;
        case SETTING_WD_HOP_HELPER:
            err = nvs_set_u16(nvsHandle, NVS_WD_HOP_HELPER_KEY, G_Settings.wd_hop_helper_ms);
            key = NVS_WD_HOP_HELPER_KEY;
            break;
        case SETTING_WD_WEIGHTED_5G:
            err = nvs_set_u8(nvsHandle, NVS_WD_WEIGHTED_5G_KEY, G_Settings.wd_weighted_5g ? 1 : 0);
            key = NVS_WD_WEIGHTED_5G_KEY;
            break;
        case SETTING_GPS_BAUD_RATE:
            err = nvs_set_u32(nvsHandle, NVS_GPS_BAUD_KEY, G_Settings.gps_baud_rate);
            key = NVS_GPS_BAUD_KEY;
            break;
        case SETTING_IR_TX_PIN:
            err = nvs_set_i32(nvsHandle, NVS_IR_TX_PIN_KEY, G_Settings.ir_tx_pin);
            key = NVS_IR_TX_PIN_KEY;
            break;
        case SETTING_IR_RX_PIN:
            err = nvs_set_i32(nvsHandle, NVS_IR_RX_PIN_KEY, G_Settings.ir_rx_pin);
            key = NVS_IR_RX_PIN_KEY;
            break;
        case SETTING_AP_SSID:
            err = nvs_set_str(nvsHandle, NVS_AP_SSID_KEY, G_Settings.ap_ssid);
            key = NVS_AP_SSID_KEY;
            break;
        case SETTING_AP_PASSWORD:
            err = nvs_set_str(nvsHandle, NVS_AP_PASSWORD_KEY, G_Settings.ap_password);
            key = NVS_AP_PASSWORD_KEY;
            break;
        case SETTING_STA_SSID:
        case SETTING_STA_PASSWORD:
            // Save both together since they're usually updated together
            err = nvs_set_str(nvsHandle, NVS_STA_SSID_KEY, G_Settings.sta_ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(nvsHandle, NVS_STA_PASSWORD_KEY, G_Settings.sta_password);
            }
            key = NVS_STA_SSID_KEY;
            break;
        case SETTING_WIFI_AUTO_RECONNECT:
            err = nvs_set_u8(nvsHandle, NVS_WIFI_AUTO_RECONNECT_KEY, G_Settings.wifi_auto_reconnect ? 1 : 0);
            key = NVS_WIFI_AUTO_RECONNECT_KEY;
            break;
        case SETTING_TIMEZONE:
            err = nvs_set_str(nvsHandle, NVS_TIMEZONE_NAME, G_Settings.selected_timezone);
            key = NVS_TIMEZONE_NAME;
            break;
        default:
            ESP_LOGW(TAG, "Unknown setting type to persist: %d", setting);
            return;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NVS val for %s: %s", key ? key : "unknown", esp_err_to_name(err));
    } else if (key) {
        err = nvs_commit(nvsHandle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS for %s: %s", key, esp_err_to_name(err));
        } else {
            ESP_LOGD(TAG, "Persisted setting %s", key);
        }
    }
}

// Core Settings Getters and Setters
void settings_set_rgb_mode(FSettings *settings, RGBMode mode) {
  settings->rgb_mode = mode;
}

void settings_set_rts_enabled(FSettings *settings, bool enabled) {
  settings->rts_enabled = enabled;
}

bool settings_get_rts_enabled(const FSettings *settings) {
  return settings->rts_enabled;
}

RGBMode settings_get_rgb_mode(const FSettings *settings) {
  return settings->rgb_mode;
}

void settings_set_channel_delay(FSettings *settings, float delay_ms) {
  settings->channel_delay = delay_ms;
}

float settings_get_channel_delay(const FSettings *settings) {
  return settings->channel_delay;
}

void settings_set_broadcast_speed(FSettings *settings, uint16_t speed) {
  settings->broadcast_speed = speed;
}

uint16_t settings_get_broadcast_speed(const FSettings *settings) {
  return settings->broadcast_speed;
}

void settings_set_hop_mode(FSettings *settings, uint8_t mode) {
  settings->hop_mode = mode;
}

uint8_t settings_get_hop_mode(const FSettings *settings) {
  return settings->hop_mode;
}

void settings_set_hop_custom_channels(FSettings *settings, const char *channels) {
  if (!settings || !channels) return;
  strncpy(settings->hop_custom_channels, channels,
          sizeof(settings->hop_custom_channels) - 1);
  settings->hop_custom_channels[sizeof(settings->hop_custom_channels) - 1] = '\0';
}

const char *settings_get_hop_custom_channels(const FSettings *settings) {
  return settings->hop_custom_channels;
}

void settings_set_flappy_ghost_name(FSettings *settings, const char *Name) {
  strncpy(settings->flappy_ghost_name, Name,
          sizeof(settings->flappy_ghost_name) - 1);
  settings->flappy_ghost_name[sizeof(settings->flappy_ghost_name) - 1] = '\0';
}

const char *settings_get_flappy_ghost_name(const FSettings *settings) {
  return settings->flappy_ghost_name;
}

void settings_set_timezone_str(FSettings *settings, const char *Name) {
  strncpy(settings->selected_timezone, Name,
          sizeof(settings->selected_timezone) - 1);
  settings->selected_timezone[sizeof(settings->selected_timezone) - 1] = '\0';
}

const char *settings_get_timezone_str(const FSettings *settings) {
  return settings->selected_timezone;
}

void settings_set_accent_color_str(FSettings *settings, const char *Name) {
  strncpy(settings->selected_hex_accent_color, Name,
          sizeof(settings->selected_hex_accent_color) - 1);
  settings->selected_hex_accent_color[sizeof(settings->selected_hex_accent_color) - 1] = '\0';
}

const char *settings_get_accent_color_str(const FSettings *settings) {
  return settings->selected_hex_accent_color;
}

esp_err_t settings_save(const FSettings *settings) {
    if (!settings) return ESP_ERR_INVALID_ARG;

    esp_err_t err = ESP_OK;
    esp_err_t tmp;

#define NVS_SET(call) do { if (err == ESP_OK) { tmp = (call); if (tmp != ESP_OK) { err = tmp; } } } while(0)

    NVS_SET(nvs_set_u8(nvsHandle, NVS_RGB_MODE_KEY, (uint8_t)settings->rgb_mode));
    float ch_delay = settings->channel_delay;
    NVS_SET(nvs_set_blob(nvsHandle, NVS_CHANNEL_DELAY_KEY, &ch_delay, sizeof(ch_delay)));
    NVS_SET(nvs_set_u16(nvsHandle, NVS_BROADCAST_SPEED_KEY, settings->broadcast_speed));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_HOP_MODE_KEY, settings->hop_mode));
    NVS_SET(nvs_set_str(nvsHandle, NVS_HOP_CUSTOM_KEY, settings->hop_custom_channels));
    NVS_SET(nvs_set_str(nvsHandle, NVS_AP_SSID_KEY, settings->ap_ssid));
    NVS_SET(nvs_set_str(nvsHandle, NVS_AP_PASSWORD_KEY, settings->ap_password));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_RGB_SPEED_KEY, settings->rgb_speed));
    NVS_SET(nvs_set_u16(nvsHandle, NVS_RGB_LED_COUNT_KEY, settings->rgb_led_count));

    NVS_SET(nvs_set_str(nvsHandle, NVS_PORTAL_URL_KEY, settings->portal_url));
    NVS_SET(nvs_set_str(nvsHandle, NVS_PORTAL_SSID_KEY, settings->portal_ssid));
    NVS_SET(nvs_set_str(nvsHandle, NVS_PORTAL_PASSWORD_KEY, settings->portal_password));
    NVS_SET(nvs_set_str(nvsHandle, NVS_PORTAL_AP_SSID_KEY, settings->portal_ap_ssid));
    NVS_SET(nvs_set_str(nvsHandle, NVS_PORTAL_DOMAIN_KEY, settings->portal_domain));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_PORTAL_OFFLINE_KEY, settings->portal_offline_mode ? 1 : 0));

    NVS_SET(nvs_set_str(nvsHandle, NVS_PRINTER_IP_KEY, settings->printer_ip));
    NVS_SET(nvs_set_str(nvsHandle, NVS_PRINTER_TEXT_KEY, settings->printer_text));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_PRINTER_FONT_SIZE_KEY, settings->printer_font_size));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_PRINTER_ALIGNMENT_KEY, (uint8_t)settings->printer_alignment));

    NVS_SET(nvs_set_str(nvsHandle, NVS_FLAPPY_GHOST_NAME, settings->flappy_ghost_name));
    NVS_SET(nvs_set_str(nvsHandle, NVS_ACCENT_COLOR, settings->selected_hex_accent_color));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_GPS_RX_PIN, (uint8_t)settings->gps_rx_pin));
    NVS_SET(nvs_set_u32(nvsHandle, NVS_DISPLAY_TIMEOUT_KEY, settings->display_timeout_ms));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_ENABLE_RTS_KEY, settings->rts_enabled ? 1 : 0));

    NVS_SET(nvs_set_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid));
    NVS_SET(nvs_set_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WIFI_AUTO_RECONNECT_KEY, settings->wifi_auto_reconnect ? 1 : 0));

    NVS_SET(nvs_set_i32(nvsHandle, NVS_RGB_DATA_PIN_KEY, settings->rgb_data_pin));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_RGB_RED_PIN_KEY, settings->rgb_red_pin));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_RGB_GREEN_PIN_KEY, settings->rgb_green_pin));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_RGB_BLUE_PIN_KEY, settings->rgb_blue_pin));

    NVS_SET(nvs_set_u8(nvsHandle, NVS_THIRD_CTRL_KEY, settings->third_control_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_THEME_KEY, settings->menu_theme));
    NVS_SET(nvs_set_u32(nvsHandle, NVS_TERMINAL_TEXT_COLOR_KEY, settings->terminal_text_color));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_TERMINAL_FONT_SIZE_KEY, settings->terminal_font_size));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_INVERT_COLORS_KEY, settings->invert_colors ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WEB_AUTH_KEY, settings->web_auth_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_USB_MSC_KEY, settings->usb_msc_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WEBUI_AP_ONLY_KEY, settings->webui_restrict_to_ap ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_AP_ENABLED_KEY, settings->ap_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_POWER_SAVE_KEY, settings->power_save_enabled ? 1 : 0));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_ESP_COMM_TX_PIN_KEY, settings->esp_comm_tx_pin));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_ESP_COMM_RX_PIN_KEY, settings->esp_comm_rx_pin));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_ZEBRA_MENUS_KEY, settings->zebra_menus_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MAX_SCREEN_BRIGHTNESS_KEY, settings->max_screen_brightness));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_INFRARED_EASY_MODE_KEY, settings->infrared_easy_mode ? 1 : 0));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_IR_TX_PIN_KEY, settings->ir_tx_pin));
    NVS_SET(nvs_set_i32(nvsHandle, NVS_IR_RX_PIN_KEY, settings->ir_rx_pin));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_NAV_BUTTONS_KEY, settings->nav_buttons_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_AUTO_SAVE_SCANS_KEY, settings->auto_save_scans ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_LAYOUT_KEY, (uint8_t)settings->menu_layout));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_CAROUSEL_INVERT_KEY, settings->carousel_invert_direction ? 1 : 0));
    NVS_SET(nvs_set_str(nvsHandle, NVS_TIMEZONE_NAME, settings->selected_timezone));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WIFI_COUNTRY_KEY, settings->wifi_country));
    NVS_SET(nvs_set_str(nvsHandle, NVS_WIGLE_API_KEY, settings->wigle_api_key));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WIGLE_DONATE_KEY, settings->wigle_donate ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WIGLE_AUTO_UPLOAD_KEY, settings->wigle_auto_upload ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_OTA_CHANNEL_KEY, settings->ota_channel));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_OTA_UPDATE_AVAIL_KEY, settings->ota_update_available ? 1 : 0));
    NVS_SET(nvs_set_u32(nvsHandle, NVS_OTA_LAST_CHECK_KEY, settings->ota_last_check_time));
    NVS_SET(nvs_set_str(nvsHandle, NVS_IO_BTN_P10_CMD_KEY, settings->io_btn_p10_cmd));
    NVS_SET(nvs_set_str(nvsHandle, NVS_IO_BTN_P11_CMD_KEY, settings->io_btn_p11_cmd));
    NVS_SET(nvs_set_str(nvsHandle, NVS_IO_BTN_P12_CMD_KEY, settings->io_btn_p12_cmd));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_NEOPIXEL_MAX_BRIGHTNESS_KEY, settings->neopixel_max_brightness));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_ENCODER_INVERT_KEY, settings->encoder_invert_direction ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_SETUP_COMPLETE_KEY, settings->setup_complete ? 1 : 0));

#ifdef CONFIG_WITH_STATUS_DISPLAY
    NVS_SET(nvs_set_u8(nvsHandle, NVS_STATUS_IDLE_ANIM_KEY, (uint8_t)settings->status_idle_animation));
    NVS_SET(nvs_set_u32(nvsHandle, NVS_STATUS_IDLE_TIMEOUT_KEY, settings->status_idle_timeout_ms));
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    NVS_SET(nvs_set_u16(nvsHandle, NVS_BADUSB_VID_KEY, settings->badusb_vid));
    NVS_SET(nvs_set_u16(nvsHandle, NVS_BADUSB_PID_KEY, settings->badusb_pid));
    NVS_SET(nvs_set_str(nvsHandle, NVS_BADUSB_MFR_KEY, settings->badusb_manufacturer));
    NVS_SET(nvs_set_str(nvsHandle, NVS_BADUSB_PROD_KEY, settings->badusb_product));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_BADUSB_RAND_KEY, settings->badusb_randomize ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_BADUSB_KB_KEY, settings->badusb_kb_layout));
#endif

    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_VISUALIZER_MODE_KEY, (uint8_t)settings->mic_visualizer_mode));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_COLOR_MODE_KEY, (uint8_t)settings->mic_color_mode));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_SENSITIVITY_KEY, settings->mic_sensitivity));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_SMOOTHING_KEY, settings->mic_smoothing));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_CONTRAST_KEY, settings->mic_contrast));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MIC_MIRROR_MODE_KEY, settings->mic_mirror_mode ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_GHOSTLINK_SPLIT_VIEW_KEY, settings->ghostlink_split_view ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_BG_SHADE_KEY, settings->menu_bg_shade));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_THEME_BG_EFFECTS_KEY, settings->theme_background_effects ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_ROUNDED_KEY, settings->menu_rounded ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_EPILEPSY_WARNING_KEY, settings->epilepsy_warning_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_FONT_SIZE_KEY, settings->font_size));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_ROW_HEIGHT_KEY, settings->row_height));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_REDUCED_MOTION_KEY, settings->reduced_motion ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_INPUT_REPEAT_SPEED_KEY, settings->input_repeat_speed));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_HIGH_CONTRAST_KEY, settings->high_contrast ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_SUN_MODE_KEY, settings->sun_mode ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_SUN_MODE_SAVED_BRIGHTNESS_KEY, settings->sun_mode_saved_brightness));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_LOG_LEVEL_KEY, settings->log_level));
    {
        uint8_t blob[1 + sizeof(settings->favorites)];
        blob[0] = settings->favorites_count;
        memcpy(blob + 1, settings->favorites, sizeof(settings->favorites));
        NVS_SET(nvs_set_blob(nvsHandle, NVS_FAVORITES_KEY, blob, sizeof(blob)));
    }
    NVS_SET(nvs_set_u8(nvsHandle, NVS_FAVS_BYPASS_KEY, settings->favorites_bypass_pin ? 1 : 0));
    NVS_SET(nvs_set_blob(nvsHandle, "menu_config", &settings->menu_config, menu_config_storage_size(&settings->menu_config)));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_ITEM_BORDERS_KEY, settings->menu_item_borders ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_MENU_CARD_BG_KEY, settings->menu_card_bg ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_TOUCH_DRAG_SCROLL_KEY, settings->touch_drag_scroll ? 1 : 0));

    NVS_SET(nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_ENABLED_KEY, settings->lockscreen_enabled ? 1 : 0));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_TYPE_KEY, settings->lockscreen_type));
    NVS_SET(nvs_set_blob(nvsHandle, NVS_LOCKSCREEN_OBF_KEY, settings->lockscreen_obfuscated, sizeof(settings->lockscreen_obfuscated)));
    NVS_SET(nvs_set_u16(nvsHandle, NVS_LOCKSCREEN_TIMEOUT_KEY, settings->lockscreen_timeout_sec));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_LOCKSCREEN_WAKE_KEY, settings->lockscreen_wake_lock ? 1 : 0));

    NVS_SET(nvs_set_u16(nvsHandle, NVS_WD_HOP_PRIMARY_KEY, settings->wd_hop_primary_ms));
    NVS_SET(nvs_set_u16(nvsHandle, NVS_WD_HOP_HELPER_KEY, settings->wd_hop_helper_ms));
    NVS_SET(nvs_set_u8(nvsHandle, NVS_WD_WEIGHTED_5G_KEY, settings->wd_weighted_5g ? 1 : 0));

    NVS_SET(nvs_set_u32(nvsHandle, NVS_GPS_BAUD_KEY, settings->gps_baud_rate));

#undef NVS_SET

    if (err == ESP_OK) {
        err = nvs_commit(nvsHandle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit settings_save: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "Failed to write settings before commit: %s", esp_err_to_name(err));
    }
    ghostchi_manager_add_xp(1);
    return err;
}

void settings_save_sta_credentials(const FSettings *settings) {
    if (!settings) return;

    esp_err_t err = nvs_set_str(nvsHandle, NVS_STA_SSID_KEY, settings->sta_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvsHandle, NVS_STA_PASSWORD_KEY, settings->sta_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvsHandle);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save STA credentials: %s", esp_err_to_name(err));
    }
}

void settings_set_ap_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->ap_ssid, ssid, sizeof(settings->ap_ssid) - 1);
  settings->ap_ssid[sizeof(settings->ap_ssid) - 1] = '\0';
}

const char *settings_get_ap_ssid(const FSettings *settings) {
  return settings->ap_ssid;
}

void settings_set_ap_password(FSettings *settings, const char *password) {
  strncpy(settings->ap_password, password, sizeof(settings->ap_password) - 1);
  settings->ap_password[sizeof(settings->ap_password) - 1] = '\0';
}

const char *settings_get_ap_password(const FSettings *settings) {
  return settings->ap_password;
}

void settings_set_gps_rx_pin(FSettings *settings, uint8_t RxPin) {
  settings->gps_rx_pin = RxPin;
}

uint8_t settings_get_gps_rx_pin(const FSettings *settings) {
  return settings->gps_rx_pin;
}

void settings_set_gps_baud_rate(FSettings *settings, uint32_t baud) {
  settings->gps_baud_rate = baud;
}

uint32_t settings_get_gps_baud_rate(const FSettings *settings) {
  return settings->gps_baud_rate;
}

void settings_set_rgb_speed(FSettings *settings, uint8_t speed) {
  settings->rgb_speed = speed;
}

uint8_t settings_get_rgb_speed(const FSettings *settings) {
  return settings->rgb_speed;
}

// Evil Portal Getters and Setters
void settings_set_portal_url(FSettings *settings, const char *url) {
  strncpy(settings->portal_url, url, sizeof(settings->portal_url) - 1);
  settings->portal_url[sizeof(settings->portal_url) - 1] = '\0';
}

const char *settings_get_portal_url(const FSettings *settings) {
  return settings->portal_url;
}

void settings_set_portal_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->portal_ssid, ssid, sizeof(settings->portal_ssid) - 1);
  settings->portal_ssid[sizeof(settings->portal_ssid) - 1] = '\0';
}

const char *settings_get_portal_ssid(const FSettings *settings) {
  return settings->portal_ssid;
}

void settings_set_portal_password(FSettings *settings, const char *password) {
  strncpy(settings->portal_password, password,
          sizeof(settings->portal_password) - 1);
  settings->portal_password[sizeof(settings->portal_password) - 1] = '\0';
}

const char *settings_get_portal_password(const FSettings *settings) {
  return settings->portal_password;
}

void settings_set_portal_ap_ssid(FSettings *settings, const char *ap_ssid) {
  strncpy(settings->portal_ap_ssid, ap_ssid,
          sizeof(settings->portal_ap_ssid) - 1);
  settings->portal_ap_ssid[sizeof(settings->portal_ap_ssid) - 1] = '\0';
}

const char *settings_get_portal_ap_ssid(const FSettings *settings) {
  return settings->portal_ap_ssid;
}

void settings_set_portal_domain(FSettings *settings, const char *domain) {
  strncpy(settings->portal_domain, domain, sizeof(settings->portal_domain) - 1);
  settings->portal_domain[sizeof(settings->portal_domain) - 1] = '\0';
}

const char *settings_get_portal_domain(const FSettings *settings) {
  return settings->portal_domain;
}

void settings_set_portal_offline_mode(FSettings *settings, bool offline_mode) {
  settings->portal_offline_mode = offline_mode;
}

bool settings_get_portal_offline_mode(const FSettings *settings) {
  return settings->portal_offline_mode;
}

// Power Printer Getters and Setters
void settings_set_printer_ip(FSettings *settings, const char *ip) {
  strncpy(settings->printer_ip, ip, sizeof(settings->printer_ip) - 1);
  settings->printer_ip[sizeof(settings->printer_ip) - 1] = '\0';
}

const char *settings_get_printer_ip(const FSettings *settings) {
  return settings->printer_ip;
}

void settings_set_printer_text(FSettings *settings, const char *text) {
  strncpy(settings->printer_text, text, sizeof(settings->printer_text) - 1);
  settings->printer_text[sizeof(settings->printer_text) - 1] = '\0';
}

const char *settings_get_printer_text(const FSettings *settings) {
  return settings->printer_text;
}

void settings_set_printer_font_size(FSettings *settings, uint8_t font_size) {
  settings->printer_font_size = font_size;
}

uint8_t settings_get_printer_font_size(const FSettings *settings) {
  return settings->printer_font_size;
}

void settings_set_printer_alignment(FSettings *settings,
                                    PrinterAlignment alignment) {
  settings->printer_alignment = alignment;
}

PrinterAlignment settings_get_printer_alignment(const FSettings *settings) {
  return settings->printer_alignment;
}

void settings_set_display_timeout(FSettings *settings, uint32_t timeout_ms) {
  ESP_LOGI(TAG, "Setting display timeout from %lu to %lu ms",
           settings->display_timeout_ms, timeout_ms);
  if (timeout_ms == 0) { // "Never" option
      settings->display_timeout_ms = UINT32_MAX;
  } else {
      settings->display_timeout_ms = timeout_ms;
  }
}

uint32_t settings_get_display_timeout(const FSettings *settings) {
  return settings->display_timeout_ms;
}

// Station Mode Credentials Implementation
void settings_set_sta_ssid(FSettings *settings, const char *ssid) {
  strncpy(settings->sta_ssid, ssid, sizeof(settings->sta_ssid) - 1);
  settings->sta_ssid[sizeof(settings->sta_ssid) - 1] = '\0';
}

const char *settings_get_sta_ssid(const FSettings *settings) {
  return settings->sta_ssid;
}

void settings_set_sta_password(FSettings *settings, const char *password) {
  strncpy(settings->sta_password, password, sizeof(settings->sta_password) - 1);
  settings->sta_password[sizeof(settings->sta_password) - 1] = '\0';
}

const char *settings_get_sta_password(const FSettings *settings) {
  return settings->sta_password;
}

void settings_set_rgb_data_pin(FSettings *settings, int32_t pin) {
  settings->rgb_data_pin = pin;
}

int32_t settings_get_rgb_data_pin(const FSettings *settings) {
  return settings->rgb_data_pin;
}

void settings_set_rgb_separate_pins(FSettings *settings, int32_t red, int32_t green, int32_t blue) {
  settings->rgb_red_pin = red;
  settings->rgb_green_pin = green;
  settings->rgb_blue_pin = blue;
}

void settings_get_rgb_separate_pins(const FSettings *settings, int32_t *red, int32_t *green, int32_t *blue) {
  if (red) *red = settings->rgb_red_pin;
  if (green) *green = settings->rgb_green_pin;
  if (blue) *blue = settings->rgb_blue_pin;
}

void settings_set_rgb_led_count(FSettings *settings, uint16_t count) {
  settings->rgb_led_count = count;
}

uint16_t settings_get_rgb_led_count(const FSettings *settings) {
  return settings->rgb_led_count;
}

void settings_set_thirds_control_enabled(FSettings *settings, bool enabled) {
  settings->third_control_enabled = enabled;
}

bool settings_get_thirds_control_enabled(const FSettings *settings) {
  return settings->third_control_enabled;
}

void settings_set_menu_theme(FSettings *settings, uint8_t theme) {
  settings->menu_theme = theme_palette_clamp_id(theme);
}

uint8_t settings_get_menu_theme(const FSettings *settings) {
  return settings->menu_theme;
}

void settings_set_terminal_text_color(FSettings *settings, uint32_t color) {
  settings->terminal_text_color = color;
}

uint32_t settings_get_terminal_text_color(const FSettings *settings) {
  return settings->terminal_text_color;
}

void settings_set_terminal_font_size(FSettings *settings, uint8_t size) {
  if (size > 2) size = 1;
  settings->terminal_font_size = size;
}

uint8_t settings_get_terminal_font_size(const FSettings *settings) {
  return settings ? settings->terminal_font_size : 0;
}

void settings_set_invert_colors(FSettings *settings, bool enabled) {
  settings->invert_colors = enabled;
}

bool settings_get_invert_colors(const FSettings *settings) {
  return settings->invert_colors;
}

void settings_set_web_auth_enabled(FSettings *settings, bool enabled) {
  settings->web_auth_enabled = enabled;
}

bool settings_get_web_auth_enabled(const FSettings *settings) {
  return settings->web_auth_enabled;
}

void settings_set_usb_msc_enabled(FSettings *settings, bool enabled) {
  settings->usb_msc_enabled = enabled;
}

bool settings_get_usb_msc_enabled(const FSettings *settings) {
  return settings->usb_msc_enabled;
}

void settings_set_webui_restrict_to_ap(FSettings *settings, bool enabled) {
  settings->webui_restrict_to_ap = enabled;
}

bool settings_get_webui_restrict_to_ap(const FSettings *settings) {
  return settings->webui_restrict_to_ap;
}

void settings_set_ap_enabled(FSettings *settings, bool enabled) {
  settings->ap_enabled = enabled;
}

bool settings_get_ap_enabled(const FSettings *settings) {
  return settings->ap_enabled;
}

void settings_set_power_save_enabled(FSettings *settings, bool enabled) {
  settings->power_save_enabled = enabled;
}

bool settings_get_power_save_enabled(const FSettings *settings) {
  return settings->power_save_enabled;
}

void settings_set_esp_comm_pins(FSettings *settings, int32_t tx_pin, int32_t rx_pin) {
  settings->esp_comm_tx_pin = tx_pin;
  settings->esp_comm_rx_pin = rx_pin;
}

void settings_get_esp_comm_pins(const FSettings *settings, int32_t *tx_pin, int32_t *rx_pin) {
  if (tx_pin) *tx_pin = settings->esp_comm_tx_pin;
  if (rx_pin) *rx_pin = settings->esp_comm_rx_pin;
}


void settings_set_max_screen_brightness(FSettings *settings, uint8_t value) {
    if (value > 100) value = 100;
    settings->max_screen_brightness = value;
}
uint8_t settings_get_max_screen_brightness(const FSettings *settings) {
    return settings->max_screen_brightness;
}


// Infrared Settings Getters and Setters
void settings_set_infrared_easy_mode(FSettings *settings, bool enabled) {
  settings->infrared_easy_mode = enabled;
}

bool settings_get_infrared_easy_mode(const FSettings *settings) {
  return settings->infrared_easy_mode;
}

// IR pin overrides — single point of validation for both CLI and UI.
// -1 = use compiled default; 0 is rejected (treated as "unset" elsewhere);
// TX additionally requires an output-capable GPIO.
static bool settings_pin_valid(int32_t pin, bool needs_output) {
  if (pin == -1) return true; // Use compiled CONFIG_* default
  if (pin <= 0 || pin >= GPIO_NUM_MAX) return false;
  if (needs_output && !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin)) return false;
  if (!needs_output && !GPIO_IS_VALID_GPIO((gpio_num_t)pin)) return false;
  return true;
}

static bool settings_pin_is_strapping(int32_t pin) {
#if CONFIG_IDF_TARGET_ESP32
  return pin == 2 || pin == 12 || pin == 15;
#else
  return pin == 12 || pin == 15;
#endif
}

static void settings_ir_pin_warn_strapping(int32_t pin, const char *which) {
  if (pin > 0 && settings_pin_is_strapping(pin)) {
    ESP_LOGW(TAG, "IR %s pin %ld is a strapping pin - device may not boot with it held", which, (long)pin);
  }
}

bool settings_set_ir_tx_pin(FSettings *settings, int32_t pin) {
  if (!settings_pin_valid(pin, true)) {
    ESP_LOGE(TAG, "Invalid IR TX pin %ld (must be an output-capable GPIO or -1)", (long)pin);
    return false;
  }
  settings_ir_pin_warn_strapping(pin, "TX");
  settings->ir_tx_pin = pin;
  return true;
}

int32_t settings_get_ir_tx_pin(const FSettings *settings) {
  return settings->ir_tx_pin;
}

bool settings_set_ir_rx_pin(FSettings *settings, int32_t pin) {
  if (!settings_pin_valid(pin, false)) {
    ESP_LOGE(TAG, "Invalid IR RX pin %ld (must be a valid GPIO or -1)", (long)pin);
    return false;
  }
  settings_ir_pin_warn_strapping(pin, "RX");
  settings->ir_rx_pin = pin;
  return true;
}

int32_t settings_get_ir_rx_pin(const FSettings *settings) {
  return settings->ir_rx_pin;
}

void settings_get_nvs_stats(nvs_stats_t *stats) {
  esp_err_t err = nvs_get_stats(NULL, stats);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get NVS stats: %s", esp_err_to_name(err));
    memset(stats, 0, sizeof(nvs_stats_t));
  }
}

size_t settings_get_nvs_used_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.used_entries;
}

size_t settings_get_nvs_free_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.free_entries;
}

size_t settings_get_nvs_total_entries(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  return stats.total_entries;
}

float settings_get_nvs_usage_percentage(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  if (stats.total_entries == 0) {
    return 0.0f;
  }
  return ((float)stats.used_entries / (float)stats.total_entries) * 100.0f;
}

void settings_print_nvs_stats(void) {
  nvs_stats_t stats;
  settings_get_nvs_stats(&stats);
  
  ESP_LOGI(TAG, "NVS Storage Statistics:");
  ESP_LOGI(TAG, "  Total entries: %zu", stats.total_entries);
  ESP_LOGI(TAG, "  Used entries: %zu", stats.used_entries);
  ESP_LOGI(TAG, "  Free entries: %zu", stats.free_entries);
  ESP_LOGI(TAG, "  Namespaces: %zu", stats.namespace_count);
  ESP_LOGI(TAG, "  Usage: %.1f%%", settings_get_nvs_usage_percentage());
  
  printf("NVS Storage: %zu/%zu entries used (%.1f%%)\n", 
         stats.used_entries, stats.total_entries, 
         settings_get_nvs_usage_percentage());
}

size_t settings_get_namespace_used_entries(const char *namespace_name) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS namespace '%s': %s", 
             namespace_name, esp_err_to_name(err));
    return 0;
  }
  
  size_t used_entries = 0;
  err = nvs_get_used_entry_count(handle, &used_entries);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get used entry count for namespace '%s': %s", 
             namespace_name, esp_err_to_name(err));
    used_entries = 0;
  }
  
  nvs_close(handle);
  return used_entries;
}

void settings_print_namespace_stats(const char *namespace_name) {
  size_t used_entries = settings_get_namespace_used_entries(namespace_name);
  ESP_LOGI(TAG, "Namespace '%s': %zu entries used", namespace_name, used_entries);
  printf("Namespace '%s': %zu entries used\n", namespace_name, used_entries);
}

void settings_set_zebra_menus_enabled(FSettings *settings, bool enabled) {
    settings->zebra_menus_enabled = enabled;
}

bool settings_get_zebra_menus_enabled(const FSettings *settings) {
    return settings->zebra_menus_enabled;
}

void settings_set_nav_buttons_enabled(FSettings *settings, bool enabled) {
    settings->nav_buttons_enabled = enabled;
}

bool settings_get_nav_buttons_enabled(const FSettings *settings) {
    return settings->nav_buttons_enabled;
}

void settings_set_auto_save_scans(FSettings *settings, bool enabled) {
    settings->auto_save_scans = enabled;
}

bool settings_get_auto_save_scans(const FSettings *settings) {
    return settings->auto_save_scans;
}

// Menu layout settings
void settings_set_menu_layout(FSettings *settings, uint8_t layout) {
    if (layout > 4) layout = 1;
    settings->menu_layout = layout;
}

uint8_t settings_get_menu_layout(const FSettings *settings) {
    return settings->menu_layout <= 4 ? settings->menu_layout : 1;
}

void settings_set_carousel_invert_direction(FSettings *settings, bool enabled) {
    settings->carousel_invert_direction = enabled;
}

bool settings_get_carousel_invert_direction(const FSettings *settings) {
    return settings->carousel_invert_direction;
}

// Neopixel brightness settings
void settings_set_neopixel_max_brightness(FSettings *settings, uint8_t brightness) {
    if (brightness > 100) brightness = 100;
    settings->neopixel_max_brightness = brightness;
}

uint8_t settings_get_neopixel_max_brightness(const FSettings *settings) {
    return settings->neopixel_max_brightness;
}

void settings_set_encoder_invert_direction(FSettings *settings, bool enabled) {
  settings->encoder_invert_direction = enabled;
}

bool settings_get_encoder_invert_direction(const FSettings *settings) {
  return settings->encoder_invert_direction;
}

void settings_set_setup_complete(FSettings *settings, bool complete) {
  settings->setup_complete = complete;
}

bool settings_get_setup_complete(const FSettings *settings) {
  return settings->setup_complete;
}

void settings_set_wifi_country(FSettings *settings, uint8_t country) {
  settings->wifi_country = country;
}

uint8_t settings_get_wifi_country(const FSettings *settings) {
  return settings->wifi_country;
}

void settings_set_wigle_auto_upload(FSettings *settings, bool enabled) {
  settings->wigle_auto_upload = enabled;
}

bool settings_get_wigle_auto_upload(const FSettings *settings) {
  return settings->wigle_auto_upload;
}

void settings_set_ota_channel(FSettings *settings, uint8_t channel) {
  settings->ota_channel = channel;
}

uint8_t settings_get_ota_channel(const FSettings *settings) {
  return settings->ota_channel;
}

void settings_set_ota_update_available(FSettings *settings, bool available) {
  settings->ota_update_available = available;
}

bool settings_get_ota_update_available(const FSettings *settings) {
  return settings->ota_update_available;
}

void settings_set_ota_last_check_time(FSettings *settings, uint32_t timestamp) {
  settings->ota_last_check_time = timestamp;
}

uint32_t settings_get_ota_last_check_time(const FSettings *settings) {
  return settings->ota_last_check_time;
}

void settings_set_wigle_donate(FSettings *settings, bool enabled) {
  settings->wigle_donate = enabled;
}

bool settings_get_wigle_donate(const FSettings *settings) {
  return settings->wigle_donate;
}

const char *settings_get_io_btn_p10_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p10_cmd : "";
}
void settings_set_io_btn_p10_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p10_cmd, cmd, sizeof(settings->io_btn_p10_cmd) - 1);
  settings->io_btn_p10_cmd[sizeof(settings->io_btn_p10_cmd) - 1] = '\0';
}
const char *settings_get_io_btn_p11_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p11_cmd : "";
}
void settings_set_io_btn_p11_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p11_cmd, cmd, sizeof(settings->io_btn_p11_cmd) - 1);
  settings->io_btn_p11_cmd[sizeof(settings->io_btn_p11_cmd) - 1] = '\0';
}
const char *settings_get_io_btn_p12_cmd(const FSettings *settings) {
  return settings ? settings->io_btn_p12_cmd : "";
}
void settings_set_io_btn_p12_cmd(FSettings *settings, const char *cmd) {
  if (!settings || !cmd) return;
  strncpy(settings->io_btn_p12_cmd, cmd, sizeof(settings->io_btn_p12_cmd) - 1);
  settings->io_btn_p12_cmd[sizeof(settings->io_btn_p12_cmd) - 1] = '\0';
}

#ifdef CONFIG_WITH_STATUS_DISPLAY
void settings_set_status_idle_animation(FSettings *settings, IdleAnimation anim) {
  settings->status_idle_animation = anim;
}

IdleAnimation settings_get_status_idle_animation(const FSettings *settings) {
  return settings->status_idle_animation;
}

void settings_set_status_idle_timeout_ms(FSettings *settings, uint32_t timeout_ms) {
  settings->status_idle_timeout_ms = timeout_ms;
}

uint32_t settings_get_status_idle_timeout_ms(const FSettings *settings) {
  return settings->status_idle_timeout_ms;
}
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
void settings_set_badusb_vid(FSettings *settings, uint16_t vid) {
  settings->badusb_vid = vid;
}
uint16_t settings_get_badusb_vid(const FSettings *settings) {
  return settings->badusb_vid;
}
void settings_set_badusb_pid(FSettings *settings, uint16_t pid) {
  settings->badusb_pid = pid;
}
uint16_t settings_get_badusb_pid(const FSettings *settings) {
  return settings->badusb_pid;
}
void settings_set_badusb_manufacturer(FSettings *settings, const char *name) {
  strncpy(settings->badusb_manufacturer, name, sizeof(settings->badusb_manufacturer) - 1);
  settings->badusb_manufacturer[sizeof(settings->badusb_manufacturer) - 1] = '\0';
}
const char *settings_get_badusb_manufacturer(const FSettings *settings) {
  return settings->badusb_manufacturer;
}
void settings_set_badusb_product(FSettings *settings, const char *name) {
  strncpy(settings->badusb_product, name, sizeof(settings->badusb_product) - 1);
  settings->badusb_product[sizeof(settings->badusb_product) - 1] = '\0';
}
const char *settings_get_badusb_product(const FSettings *settings) {
  return settings->badusb_product;
}
void settings_set_badusb_randomize(FSettings *settings, bool enabled) {
  settings->badusb_randomize = enabled;
}
bool settings_get_badusb_randomize(const FSettings *settings) {
  return settings->badusb_randomize;
}
void settings_set_badusb_kb_layout(FSettings *settings, uint8_t layout) {
  settings->badusb_kb_layout = layout;
}
uint8_t settings_get_badusb_kb_layout(const FSettings *settings) {
  return settings->badusb_kb_layout;
}

void settings_reset_badusb_defaults(FSettings *settings) {
  settings->badusb_vid = 0x1209;
  settings->badusb_pid = 0x0001;
  strcpy(settings->badusb_manufacturer, "USB Device");
  strcpy(settings->badusb_product, "HID Keyboard");
  settings->badusb_randomize = false;
  settings->badusb_kb_layout = KB_LAYOUT_US;
}
#endif

// MIC RGB Visualizer getters and setters
void settings_set_mic_visualizer_mode(FSettings *settings, MicVisualizerMode mode) {
  if (settings) {
    settings->mic_visualizer_mode = mode;
  }
}

MicVisualizerMode settings_get_mic_visualizer_mode(const FSettings *settings) {
  return settings ? settings->mic_visualizer_mode : MIC_MODE_4BAND_SPECTRUM;
}

void settings_set_mic_color_mode(FSettings *settings, MicColorMode mode) {
  if (settings) {
    settings->mic_color_mode = mode;
  }
}

MicColorMode settings_get_mic_color_mode(const FSettings *settings) {
  return settings ? settings->mic_color_mode : MIC_COLOR_RAINBOW;
}

void settings_set_mic_sensitivity(FSettings *settings, uint8_t sensitivity) {
  if (settings) {
    if (sensitivity > 100) sensitivity = 100;
    settings->mic_sensitivity = sensitivity;
  }
}

uint8_t settings_get_mic_sensitivity(const FSettings *settings) {
  return settings ? settings->mic_sensitivity : 50;
}

void settings_set_mic_smoothing(FSettings *settings, uint8_t smoothing) {
  if (settings) {
    if (smoothing > 100) smoothing = 100;
    settings->mic_smoothing = smoothing;
  }
}

uint8_t settings_get_mic_smoothing(const FSettings *settings) {
  return settings ? settings->mic_smoothing : 30;
}

void settings_set_mic_contrast(FSettings *settings, uint8_t contrast) {
  if (settings) {
    if (contrast < 1) contrast = 1;
    if (contrast > 5) contrast = 5;
    settings->mic_contrast = contrast;
  }
}

uint8_t settings_get_mic_contrast(const FSettings *settings) {
  return settings ? settings->mic_contrast : 2;
}

void settings_set_mic_mirror_mode(FSettings *settings, bool enabled) {
  if (settings) {
    settings->mic_mirror_mode = enabled;
  }
}

bool settings_get_mic_mirror_mode(const FSettings *settings) {
  return settings ? settings->mic_mirror_mode : false;
}

void settings_set_mic_calibrate(FSettings *settings, bool calibrate) {
  // This is an action trigger, actual calibration happens elsewhere
  if (settings && calibrate) {
    // Trigger calibration via goertzel_restart_cal() or similar
    extern void goertzel_restart_cal(void);
    goertzel_restart_cal();
  }
}

bool settings_get_mic_calibrate(const FSettings *settings) {
  // Calibration is a one-shot action, always returns false
  return false;
}

void settings_set_ghostlink_split_view(FSettings *settings, bool enabled) {
  if (settings) {
    settings->ghostlink_split_view = enabled;
  }
}

bool settings_get_ghostlink_split_view(const FSettings *settings) {
  return settings ? settings->ghostlink_split_view : true;
}

void settings_set_menu_bg_shade(FSettings *settings, uint8_t shade) {
  if (settings) {
    settings->menu_bg_shade = shade;
  }
}

uint8_t settings_get_menu_bg_shade(const FSettings *settings) {
  return settings ? settings->menu_bg_shade : 1;
}

void settings_set_theme_background_effects(FSettings *settings, bool enabled) {
  if (settings) settings->theme_background_effects = enabled;
}

bool settings_get_theme_background_effects(const FSettings *settings) {
  return settings ? settings->theme_background_effects : true;
}

void settings_set_menu_rounded(FSettings *settings, bool enabled) {
  if (settings) {
    settings->menu_rounded = enabled;
  }
}

bool settings_get_menu_rounded(const FSettings *settings) {
  return settings ? settings->menu_rounded : false;
}

void settings_set_row_height(FSettings *settings, uint8_t height) {
  if (settings) {
    settings->row_height = height < MENU_ROW_HEIGHT_OPTION_COUNT ? height : 1;
  }
}

uint8_t settings_get_row_height(const FSettings *settings) {
  if (!settings) return 1;
  return settings->row_height < MENU_ROW_HEIGHT_OPTION_COUNT ? settings->row_height : 1;
}

void settings_set_epilepsy_warning_enabled(FSettings *settings, bool enabled) {
  if (settings) {
    settings->epilepsy_warning_enabled = enabled;
  }
}

bool settings_get_epilepsy_warning_enabled(const FSettings *settings) {
  return settings ? settings->epilepsy_warning_enabled : true;
}

void settings_set_font_size(FSettings *settings, uint8_t size) {
  if (settings) {
    settings->font_size = size;
  }
}

uint8_t settings_get_font_size(const FSettings *settings) {
  return settings ? settings->font_size : 1;
}

void settings_set_reduced_motion(FSettings *settings, bool enabled) {
  if (settings) {
    settings->reduced_motion = enabled;
  }
}

bool settings_get_reduced_motion(const FSettings *settings) {
  return settings ? settings->reduced_motion : false;
}

void settings_set_input_repeat_speed(FSettings *settings, uint8_t speed) {
  if (settings) {
    settings->input_repeat_speed = speed;
  }
}

uint8_t settings_get_input_repeat_speed(const FSettings *settings) {
  return settings ? settings->input_repeat_speed : 1;
}

void settings_normalize_modes(FSettings *settings) {
  /* High Contrast and Sun Mode are exclusive; High Contrast wins. */
  if (settings && settings->high_contrast && settings->sun_mode) {
    settings->sun_mode = false;
  }
}

void settings_set_high_contrast(FSettings *settings, bool enabled) {
  if (settings) {
    settings->high_contrast = enabled;
    if (enabled) settings->sun_mode = false;
  }
}

bool settings_get_high_contrast(const FSettings *settings) {
  return settings ? settings->high_contrast : false;
}

void settings_set_sun_mode(FSettings *settings, bool enabled) {
  if (settings) {
    settings->sun_mode = enabled;
    if (enabled) settings->high_contrast = false;
  }
}

bool settings_get_sun_mode(const FSettings *settings) {
  return settings ? settings->sun_mode : false;
}

void settings_set_log_level(FSettings *settings, uint8_t level) {
  if (settings && level <= ESP_LOG_VERBOSE) {
    settings->log_level = level;
  }
}

uint8_t settings_get_log_level(const FSettings *settings) {
  return settings ? settings->log_level : ESP_LOG_WARN;
}

bool settings_is_favorite(const FSettings *settings, const char *name) {
  if (!settings || !name || !name[0]) return false;
  for (int i = 0; i < settings->favorites_count && i < FAVORITES_MAX; i++) {
    if (strncmp(settings->favorites[i], name, FAVORITE_NAME_LEN) == 0) return true;
  }
  return false;
}

bool settings_add_favorite(FSettings *settings, const char *name) {
  if (!settings || !name || !name[0]) return false;
  if (settings_is_favorite(settings, name)) return true;
  if (settings->favorites_count >= FAVORITES_MAX) return false;
  strncpy(settings->favorites[settings->favorites_count], name, FAVORITE_NAME_LEN - 1);
  settings->favorites[settings->favorites_count][FAVORITE_NAME_LEN - 1] = '\0';
  settings->favorites_count++;
  return true;
}

bool settings_remove_favorite(FSettings *settings, const char *name) {
  if (!settings || !name) return false;
  for (int i = 0; i < settings->favorites_count; i++) {
    if (strncmp(settings->favorites[i], name, FAVORITE_NAME_LEN) == 0) {
      for (int j = i; j < settings->favorites_count - 1; j++) {
        strncpy(settings->favorites[j], settings->favorites[j + 1], FAVORITE_NAME_LEN);
      }
      memset(settings->favorites[settings->favorites_count - 1], 0, FAVORITE_NAME_LEN);
      settings->favorites_count--;
      return true;
    }
  }
  return false;
}

bool settings_toggle_favorite(FSettings *settings, const char *name) {
  if (settings_is_favorite(settings, name)) return settings_remove_favorite(settings, name);
  return settings_add_favorite(settings, name);
}

uint8_t settings_get_favorites_count(const FSettings *settings) {
  return settings ? settings->favorites_count : 0;
}

const char *settings_get_favorite(const FSettings *settings, uint8_t idx) {
  if (!settings || idx >= settings->favorites_count) return NULL;
  return settings->favorites[idx];
}

void settings_set_favorites_bypass(FSettings *settings, bool bypass) {
  if (settings) settings->favorites_bypass_pin = bypass;
}

bool settings_get_favorites_bypass(const FSettings *settings) {
  return settings ? settings->favorites_bypass_pin : false;
}

void settings_set_menu_item_borders(FSettings *settings, bool enabled) {
  if (settings) {
    settings->menu_item_borders = enabled;
  }
}

bool settings_get_menu_item_borders(const FSettings *settings) {
  return settings ? settings->menu_item_borders : true;
}

void settings_set_menu_card_bg(FSettings *settings, bool enabled) {
  if (settings) {
    settings->menu_card_bg = enabled;
  }
}

bool settings_get_menu_card_bg(const FSettings *settings) {
  return settings ? settings->menu_card_bg : true;
}

void settings_set_touch_drag_scroll(FSettings *settings, bool enabled) {
  if (settings) {
    settings->touch_drag_scroll = enabled;
  }
}

bool settings_get_touch_drag_scroll(const FSettings *settings) {
  return settings ? settings->touch_drag_scroll : true;
}

// Lockscreen getters and setters
void settings_set_lockscreen_enabled(FSettings *settings, bool enabled) {
  if (settings) settings->lockscreen_enabled = enabled;
}
bool settings_get_lockscreen_enabled(const FSettings *settings) {
  return settings ? settings->lockscreen_enabled : false;
}
void settings_set_lockscreen_type(FSettings *settings, uint8_t type) {
  if (settings) settings->lockscreen_type = type;
}
uint8_t settings_get_lockscreen_type(const FSettings *settings) {
  return settings ? settings->lockscreen_type : 0;
}
void settings_set_lockscreen_obfuscated(FSettings *settings, const char *obf) {
  if (!settings || !obf) return;
  memcpy(settings->lockscreen_obfuscated, obf, sizeof(settings->lockscreen_obfuscated));
}
const char *settings_get_lockscreen_obfuscated(const FSettings *settings) {
  return settings ? settings->lockscreen_obfuscated : "";
}
void settings_set_lockscreen_timeout_sec(FSettings *settings, uint16_t sec) {
  if (settings) settings->lockscreen_timeout_sec = sec;
}
uint16_t settings_get_lockscreen_timeout_sec(const FSettings *settings) {
  return settings ? settings->lockscreen_timeout_sec : 0;
}
void settings_set_lockscreen_wake_lock(FSettings *settings, bool enabled) {
  if (settings) settings->lockscreen_wake_lock = enabled;
}
bool settings_get_lockscreen_wake_lock(const FSettings *settings) {
  return settings ? settings->lockscreen_wake_lock : true;
}

// Wardriving settings
void settings_set_wd_hop_primary_ms(FSettings *settings, uint16_t ms) {
  if (ms < 50) ms = 50;
  if (ms > 500) ms = 500;
  if (settings) settings->wd_hop_primary_ms = ms;
}
uint16_t settings_get_wd_hop_primary_ms(const FSettings *settings) {
  return settings ? settings->wd_hop_primary_ms : 125;
}
void settings_set_wd_hop_helper_ms(FSettings *settings, uint16_t ms) {
  if (ms < 50) ms = 50;
  if (ms > 500) ms = 500;
  if (settings) settings->wd_hop_helper_ms = ms;
}
uint16_t settings_get_wd_hop_helper_ms(const FSettings *settings) {
  return settings ? settings->wd_hop_helper_ms : 125;
}
void settings_set_wd_weighted_5g(FSettings *settings, bool enabled) {
  if (settings) settings->wd_weighted_5g = enabled;
}
bool settings_get_wd_weighted_5g(const FSettings *settings) {
  return settings ? settings->wd_weighted_5g : true;
}

void settings_set_wifi_auto_reconnect(FSettings *settings, bool enabled) {
  if (settings) settings->wifi_auto_reconnect = enabled;
}
bool settings_get_wifi_auto_reconnect(const FSettings *settings) {
  return settings ? settings->wifi_auto_reconnect : true;
}
