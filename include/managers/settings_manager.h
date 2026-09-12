#ifndef SETTINGS_MANAGER_H
#define SETTINGS_MANAGER_H

#include "core/utils.h"
#include <nvs.h>
#include <nvs_flash.h>
#include <stdbool.h>
#include <stdint.h>
#include "gui/menu_config.h"

// Enum for RGB Modes
typedef enum {
    RGB_MODE_NORMAL = 0,
    RGB_MODE_RAINBOW = 1,
    RGB_MODE_STEALTH = 2,
    RGB_MODE_KNIGHT_RIDER = 3,
    RGB_MODE_RED = 4,
    RGB_MODE_GREEN = 5,
    RGB_MODE_BLUE = 6,
    RGB_MODE_YELLOW = 7,
    RGB_MODE_PURPLE = 8,
    RGB_MODE_CYAN = 9,
    RGB_MODE_ORANGE = 10,
    RGB_MODE_WHITE = 11,
    RGB_MODE_PINK = 12,
    RGB_MODE_MIC_VISUALIZER = 13  // Receives amplitude from GhostLink peer
    // ...add more modes here if needed
} RGBMode;

// Enum for MIC RGB Visualizer Modes (sensory_bridge inspired)
typedef enum {
    MIC_MODE_4BAND_SPECTRUM = 0,    // Current 4-band spectrum analyzer
    MIC_MODE_VU_METER,              // Single dot VU meter
    MIC_MODE_KALEIDOSCOPE,          // Perlin noise patterns modulated by audio
    MIC_MODE_WAVEFORM,              // Oscilloscope-style waveform
    MIC_MODE_BLOOM,                 // Center-expanding trails
    MIC_MODE_PEAK_METER,            // Peak level meter with decay
    MIC_MODE_COUNT
} MicVisualizerMode;

// Enum for MIC RGB Color Modes
typedef enum {
    MIC_COLOR_RAINBOW = 0,          // Fixed rainbow gradient (current)
    MIC_COLOR_CHROMATIC,            // Musical note-based colors
    MIC_COLOR_SINGLE_HUE,           // User-defined single hue
    MIC_COLOR_PALETTE_FIRE,         // Fire palette
    MIC_COLOR_PALETTE_OCEAN,        // Ocean palette
    MIC_COLOR_PALETTE_FOREST,       // Forest palette
    MIC_COLOR_PALETTE_HEAT,         // Heat palette
    MIC_COLOR_COUNT
} MicColorMode;

#ifdef CONFIG_WITH_STATUS_DISPLAY
// idle/status oled animation mode
typedef enum {
  IDLE_ANIM_GAME_OF_LIFE = 0,
  IDLE_ANIM_GHOST = 1,
  IDLE_ANIM_STARFIELD = 2,
  IDLE_ANIM_HUD = 3,
  IDLE_ANIM_MATRIX = 4,
  IDLE_ANIM_FLYING_GHOSTS = 5,
  IDLE_ANIM_SPIRAL = 6,
  IDLE_ANIM_FALLING_LEAVES = 7,
  IDLE_ANIM_BOUNCING_TEXT = 8
} IdleAnimation;
#endif

// Keyboard layout for BadUSB
typedef enum {
    KB_LAYOUT_US = 0,
    KB_LAYOUT_DE,
    KB_LAYOUT_FR,
    KB_LAYOUT_UK,
    KB_LAYOUT_ES,
    KB_LAYOUT_COUNT
} KeyboardLayout;

typedef enum {
    SETTING_RGB_MODE = 0,
    SETTING_DISPLAY_TIMEOUT,
    SETTING_MENU_THEME,
    SETTING_THIRD_CONTROL,
    SETTING_TERMINAL_COLOR,
    SETTING_INVERT_COLORS,
    SETTING_WEB_AUTH,
    SETTING_WEBUI_AP_ONLY,
    SETTING_AP_ENABLED,
    SETTING_COUNTRY,
    SETTING_HOP_CHANNELS,
    SETTING_POWER_SAVE,
    SETTING_MAX_BRIGHTNESS,
    SETTING_NEOPIXEL_BRIGHTNESS,
    SETTING_ZEBRA_MENUS,
    SETTING_NAV_BUTTONS,
    SETTING_MENU_LAYOUT,
    SETTING_AUTO_SAVE_SCANS,
#ifdef CONFIG_WITH_STATUS_DISPLAY
    SETTING_IDLE_ANIMATION,
    SETTING_IDLE_ANIM_DELAY,
#endif
#ifdef CONFIG_USE_ENCODER
    SETTING_ENCODER_INVERT,
    SETTING_ENCODER_LATCH,
#endif
#if CONFIG_IDF_TARGET_ESP32S3
    SETTING_USB_HOST_MODE,
#ifdef CONFIG_HAS_USB_MSC_SD
    SETTING_USB_MSC,
#endif
#endif
    SETTING_RUN_SETUP_WIZARD,
    SETTING_I2C_SCAN,
    SETTING_GL_BENCH,
    SETTING_FACTORY_RESET,
    SETTING_SETUP_COMPLETE,
    SETTING_WIGLE_API_KEY,
    SETTING_WIGLE_AUTO_UPLOAD,
    SETTING_WIGLE_DONATE,
    SETTING_LOAD_CONFIG,
    SETTING_WIGLE_TEST_API,
    SETTING_WIGLE_HELP,
    SETTING_WIGLE_MANUAL_UPLOAD,
    SETTING_WIGLE_STATS,
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    SETTING_BADUSB_VID,
    SETTING_BADUSB_PID,
    SETTING_BADUSB_MANUFACTURER,
    SETTING_BADUSB_PRODUCT,
    SETTING_BADUSB_RANDOMIZE,
    SETTING_BADUSB_KB_LAYOUT,
#endif
    // MIC RGB Visualizer settings
    SETTING_MIC_VISUALIZER_MODE,
    SETTING_MIC_COLOR_MODE,
    SETTING_MIC_SENSITIVITY,
    SETTING_MIC_SMOOTHING,
    SETTING_MIC_CONTRAST,
    SETTING_MIC_MIRROR_MODE,
    SETTING_MIC_CALIBRATE,
    SETTING_GHOSTLINK_SPLIT_VIEW,
    SETTING_MENU_BG_SHADE,
    SETTING_MENU_ROUNDED,
    SETTING_EPILEPSY_WARNING,
    SETTING_FONT_SIZE,
    SETTING_ROW_HEIGHT,
    SETTING_REDUCED_MOTION,
    SETTING_INPUT_REPEAT_SPEED,
    SETTING_HIGH_CONTRAST,
    SETTING_MENU_ITEM_BORDERS,
    SETTING_MENU_CARD_BG,
    SETTING_TOUCH_DRAG_SCROLL,
    SETTING_TERMINAL_FONT_SIZE,
    SETTING_RELOAD_ASSET_PACK,
    SETTING_CAROUSEL_INVERT_DIRECTION,
    SETTING_EXPORT_SETTINGS_SD,
    SETTING_IMPORT_SETTINGS_SD,
    // Lockscreen settings
    SETTING_LOCKSCREEN_ENABLED,
    SETTING_LOCKSCREEN_WAKE,
    SETTING_LOCKSCREEN_TYPE,
    SETTING_LOCKSCREEN_TIMEOUT,
    SETTING_LOCKSCREEN_CHANGE_PIN,
    // Wardriving settings
    SETTING_WD_HOP_PRIMARY,
    SETTING_WD_HOP_HELPER,
    SETTING_WD_WEIGHTED_5G,
    SETTING_GPS_BAUD_RATE,
    // On-device edit actions for existing NVS-backed fields
    SETTING_AP_SSID,
    SETTING_AP_PASSWORD,
    SETTING_STA_SSID,
    SETTING_STA_PASSWORD,
    SETTING_WIFI_AUTO_RECONNECT,
    // Timezone quick-edit
    SETTING_TIMEZONE,
    // Clock display
    SETTING_CLOCK_STYLE,
    SETTING_STATUS_BAR_CLOCK,
    // OTA firmware update
    SETTING_OTA_CHANNEL,
    SETTING_OTA_UPDATE_AVAILABLE,
    SETTING_OTA_LAST_CHECK_TIME,
    // OTA UI actions (not NVS-backed -- handled entirely in options_screen.c)
    SETTING_OTA_CHECK_NOW,
    SETTING_OTA_INSTALL_UPDATE,
    SETTING_OTA_CHECK_PEER,
    SETTING_OTA_UPDATE_PEER,
    SETTING_OTA_INSTALL_FROM_SD,
    SETTING_SUN_MODE,
    SETTING_LOG_LEVEL,
    SETTING_FAVORITES,
    SETTING_FAVORITES_BYPASS,
    SETTING_MANAGE_FAVORITES,
    SETTING_THEME_BACKGROUND_EFFECTS,
    SETTING_MENU_CONFIG,
    SETTING_MAIN_MENU_ITEMS,
    SETTING_APPS_MENU_ITEMS,
    // Runtime-configurable hardware pins (-1 = use compiled CONFIG_* default)
    SETTING_IR_TX_PIN,
    SETTING_IR_RX_PIN,
    // Read-only "enabled devices" status rows (not NVS-backed)
    SETTING_DEVICE_IR,
    SETTING_DEVICE_GPS,
    SETTING_DEVICE_SUBGHZ,
    SETTING_DEVICE_NRF24,
    SETTING_DEVICE_SD,
    SETTING_DEVICE_RGB,
} SettingsType;

/* 16 slots x 64B names. The NVS blob is [count][FAVORITES_MAX x 64]; older
 * 8-slot layouts are migrated on load (see settings_manager.c). */
#define FAVORITES_MAX 16
#define FAVORITE_NAME_LEN 64

/* Number of presets offered by the options-list Row Height setting. */
#define MENU_ROW_HEIGHT_OPTION_COUNT 4

#define GPS_BAUD_AUTO 1U


typedef enum {
  ALIGNMENT_CM, // Center Middle
  ALIGNMENT_TL, // Top Left
  ALIGNMENT_TR, // Top Right
  ALIGNMENT_BR, // Bottom Right
  ALIGNMENT_BL  // Bottom Left
} PrinterAlignment;

// Enum for Supported Boards
typedef enum {
  FLIPPER_DEV_BOARD = 0,
  AWOK_DUAL_MINI = 1,
  AWOK_DUAL = 2,
  MARAUDER_V6 = 3,
  CARDPUTER = 4,
  DEVBOARD_PRO = 5,
  CUSTOM = 6
} SupportedBoard;

// Struct for advanced pin configuration
typedef struct {
  int8_t neopixel_pin;
  int8_t sd_card_spi_miso;
  int8_t sd_card_spi_mosi;
  int8_t sd_card_spi_clk;
  int8_t sd_card_spi_cs;
  int8_t sd_card_mmc_cmd;
  int8_t sd_card_mmc_clk;
  int8_t sd_card_mmc_d0;
  int8_t gps_tx_pin;
  int8_t gps_rx_pin;
} PinConfig;

// Struct for settings
typedef struct {
  RGBMode rgb_mode;
  float channel_delay;
  uint16_t broadcast_speed;
  uint8_t hop_mode;            // Hop channel profile mode (hop_mode_t in hop_profile.h)
  char hop_custom_channels[129]; // Custom hop channel list as "1,6,11"
  char ap_ssid[33];     // Max SSID length is 32 bytes + null terminator
  char ap_password[65]; // Max password length is 64 bytes + null terminator
  uint8_t rgb_speed;

  // Evil Portal settings
  char portal_url[129];     // URL or file path for offline mode
  char portal_ssid[33];     // SSID for the Evil Portal
  char portal_password[65]; // Password for the Evil Portal
  char portal_ap_ssid[33];  // AP SSID for the Evil Portal
  char portal_domain[65];   // Domain for the Evil Portal
  bool portal_offline_mode; // Toggle for offline/online mode

  // Power Printer settings
  char printer_ip[16];       // Printer IP address (IPv4)
  char printer_text[257];    // Last printed text (max 256 characters + null
                             // terminator)
  uint8_t printer_font_size; // Font size for printing
  PrinterAlignment printer_alignment; // Text alignment
  char flappy_ghost_name[65];
  char selected_timezone[25];
  uint8_t clock_style;   // Clock view face: 0 = Digital, 1 = Analog
  bool status_bar_clock; // Show the clock in the status bar centre
  char selected_hex_accent_color[25];
  int gps_rx_pin;
  uint32_t gps_baud_rate;      // 0 = use Kconfig default (CONFIG_GPS_UART_BAUD_RATE)
  uint32_t display_timeout_ms; // Display timeout in milliseconds
  bool rts_enabled;
  char sta_ssid[65];     // New field for Station SSID (Max 64 + null)
  char sta_password[65]; // New field for Station Password (Max 64 + null)
  bool wifi_auto_reconnect; // Auto-reconnect to saved STA after involuntary disconnect

  // Add RGB pin configuration fields
  int32_t rgb_data_pin; // Single-pin LED data pin, -1 if not used
  int32_t rgb_red_pin;  // Separate-pin RGB: red pin, -1 if not used
  int32_t rgb_green_pin; // Separate-pin RGB: green pin, -1 if not used
  int32_t rgb_blue_pin;  // Separate-pin RGB: blue pin, -1 if not used
  bool third_control_enabled;  // Enable third-screen tap control
  uint32_t terminal_text_color; // Terminal text color in 0xRRGGBB
  uint8_t terminal_font_size;   // 0=Small, 1=Normal, 2=Large
  uint8_t menu_theme;  // Theme for main menu colors (0=Default)
  bool invert_colors; // Invert screen colors
  bool web_auth_enabled;
  bool webui_restrict_to_ap;
  bool usb_msc_enabled; // Remember USB SD passthrough toggle (never auto-starts at boot)
  
  int32_t esp_comm_tx_pin; // ESP communication TX pin
  int32_t esp_comm_rx_pin; // ESP communication RX pin
  bool ap_enabled; // Enable/disable AP across reboots
  bool power_save_enabled;
  bool zebra_menus_enabled;
  uint8_t max_screen_brightness; // Max screen brightness (0-100)

  // Infrared settings
  bool infrared_easy_mode; // Easy learn mode toggle
  int32_t ir_tx_pin;       // IR transmit pin override, -1 = use CONFIG_INFRARED_LED_PIN
  int32_t ir_rx_pin;       // IR receive pin override, -1 = use CONFIG_INFRARED_RX_PIN
  
  // Navigation buttons setting
  bool nav_buttons_enabled; // Toggle for main menu navigation buttons
  uint8_t menu_layout; // Menu layout type (0=Carousel, 1=Grid Cards, 2=List, 3=Compact, 4=Hero)
  bool carousel_invert_direction; // Invert main menu carousel slide direction
  
  // Neopixel settings
  uint8_t neopixel_max_brightness; // Max neopixel brightness (0-100)
  uint16_t rgb_led_count; // Number of LEDs configured for RGB manager
#ifdef CONFIG_WITH_STATUS_DISPLAY
  IdleAnimation status_idle_animation; // idle animation for status display
  uint32_t status_idle_timeout_ms; // delay before starting idle animation
#endif
  bool encoder_invert_direction;
#ifdef CONFIG_USE_ENCODER
  bool encoder_legacy_latch;
#endif
  bool setup_complete;
  bool auto_save_scans;
  uint8_t wifi_country;

  // Wigle API key for wardriving upload (format: "APIName:APIToken" from wigle.net/account)
  char wigle_api_key[129];
  bool wigle_auto_upload; // Auto-upload CSVs at boot when WiFi connected
  bool wigle_donate; // Whether to donate uploads to Wigle

  // OTA firmware update (8MB/16MB boards only, see ota_manager)
  uint8_t ota_channel;          // 0=stable, 1=prerelease
  bool ota_update_available;    // set by the background update-check task
  uint32_t ota_last_check_time; // unix timestamp of last background check
  // IO expander programmable buttons (P10, P11, P12) - command to run when pressed, empty = send as joystick
  char io_btn_p10_cmd[129];
  char io_btn_p11_cmd[129];
  char io_btn_p12_cmd[129];
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  uint16_t badusb_vid;
  uint16_t badusb_pid;
  char badusb_manufacturer[33];
  char badusb_product[33];
    bool badusb_randomize;
    uint8_t badusb_kb_layout;
#endif
    // MIC RGB Visualizer settings
    MicVisualizerMode mic_visualizer_mode;
    MicColorMode mic_color_mode;
    uint8_t mic_sensitivity;        // 0-100 (AGC gain control)
    uint8_t mic_smoothing;          // 0-100 (temporal smoothing)
    uint8_t mic_contrast;           // 1-5 (square iterations)
    bool mic_mirror_mode;           // Mirror visualizer center-out
    bool ghostlink_split_view;      // Split GhostLink terminal into two columns
    uint8_t menu_bg_shade;          // 0=Darker, 1=Palette, 2=Lighter, 3=Lightest
    bool theme_background_effects;  // Show the selected palette's subtle background treatment
    bool menu_rounded;              // Rounded corners on menu items
    bool epilepsy_warning_enabled;  // Show warning before flashing LED effects
    uint8_t font_size;              // 0=Small, 1=Normal, 2=Large
    uint8_t row_height;             // Options-list row size preset (0=Compact, 1=Normal, 2=Large, 3=Extra large)
    bool reduced_motion;            // Disable animations
    uint8_t input_repeat_speed;     // 0=Slow, 1=Normal, 2=Fast
    bool high_contrast;             // High contrast color overrides
    bool menu_item_borders;          // Borders around main menu items
    bool menu_card_bg;               // Card background fill/shadow on main menu items
    bool touch_drag_scroll;          // Drag-to-scroll on the options screen
    bool sun_mode;                   // Outdoor visibility: forces max brightness + high contrast
    uint8_t sun_mode_saved_brightness; // Brightness to restore when Sun Mode is turned off
    uint8_t log_level;                 // ESP-IDF global log level (esp_log_level_t)
    uint8_t favorites_count;
    char favorites[FAVORITES_MAX][FAVORITE_NAME_LEN];
    bool favorites_bypass_pin;
    menu_config_t menu_config;

    // Lockscreen settings
    bool lockscreen_enabled;
    uint8_t lockscreen_type;        // 1=PIN; kept for persisted settings compatibility
    char lockscreen_obfuscated[32]; // Length-prefixed obfuscated PIN blob
    uint16_t lockscreen_timeout_sec;   // Auto-lock after inactivity (0=off)
    bool lockscreen_wake_lock;        // Lock on wake-from-sleep

    // Wardriving settings
    uint16_t wd_hop_primary_ms;      // Primary chip hop interval (50-500ms)
    uint16_t wd_hop_helper_ms;       // Helper chip hop interval (50-500ms)
    bool wd_weighted_5g;             // Weighted 5GHz channel hopping
} FSettings;

// Function declarations
void settings_init(FSettings *settings);
void settings_deinit(void);
void settings_load(FSettings *settings);
esp_err_t settings_save(const FSettings *settings);
void settings_save_sta_credentials(const FSettings *settings);
void settings_set_defaults(FSettings *settings);

// Optimized Persistence and Task Management
void settings_persist_setting(SettingsType setting);
void settings_restart_rgb_effect(void);

// Getters and Setters for core settings
void settings_set_rgb_mode(FSettings *settings, RGBMode mode);
RGBMode settings_get_rgb_mode(const FSettings *settings);

void settings_set_channel_delay(FSettings *settings, float delay_ms);
float settings_get_channel_delay(const FSettings *settings);

void settings_set_broadcast_speed(FSettings *settings, uint16_t speed);
uint16_t settings_get_broadcast_speed(const FSettings *settings);

void settings_set_hop_mode(FSettings *settings, uint8_t mode);
uint8_t settings_get_hop_mode(const FSettings *settings);
void settings_set_hop_custom_channels(FSettings *settings, const char *channels);
const char *settings_get_hop_custom_channels(const FSettings *settings);

void settings_set_flappy_ghost_name(FSettings *settings, const char *Name);
const char *settings_get_flappy_ghost_name(const FSettings *settings);

void settings_set_rts_enabled(FSettings *settings, bool enabled);
bool settings_get_rts_enabled(const FSettings *settings);

void settings_set_timezone_str(FSettings *settings, const char *Name);
const char *settings_get_timezone_str(const FSettings *settings);

void settings_set_clock_style(FSettings *settings, uint8_t style);
uint8_t settings_get_clock_style(const FSettings *settings);
void settings_set_status_bar_clock(FSettings *settings, bool enabled);
bool settings_get_status_bar_clock(const FSettings *settings);

void settings_set_accent_color_str(FSettings *settings, const char *Name);
const char *settings_get_accent_color_str(const FSettings *settings);

void settings_set_ap_ssid(FSettings *settings, const char *ssid);
const char *settings_get_ap_ssid(const FSettings *settings);

void settings_set_ap_password(FSettings *settings, const char *password);
const char *settings_get_ap_password(const FSettings *settings);

void settings_set_rgb_speed(FSettings *settings, uint8_t speed);
uint8_t settings_get_rgb_speed(const FSettings *settings);

void settings_set_zebra_menus_enabled(FSettings *settings, bool enabled);
bool settings_get_zebra_menus_enabled(const FSettings *settings);

// Getters and Setters for Evil Portal
void settings_set_portal_url(FSettings *settings, const char *url);
const char *settings_get_portal_url(const FSettings *settings);

void settings_set_portal_ssid(FSettings *settings, const char *ssid);
const char *settings_get_portal_ssid(const FSettings *settings);

void settings_set_gps_rx_pin(FSettings *settings, uint8_t RxPin);
uint8_t settings_get_gps_rx_pin(const FSettings *settings);

// IR pin overrides (validated: -1 = use compiled default)
bool settings_set_ir_tx_pin(FSettings *settings, int32_t pin);
int32_t settings_get_ir_tx_pin(const FSettings *settings);
bool settings_set_ir_rx_pin(FSettings *settings, int32_t pin);
int32_t settings_get_ir_rx_pin(const FSettings *settings);

void settings_set_gps_baud_rate(FSettings *settings, uint32_t baud);
uint32_t settings_get_gps_baud_rate(const FSettings *settings);

void settings_set_portal_password(FSettings *settings, const char *password);
const char *settings_get_portal_password(const FSettings *settings);

void settings_set_portal_ap_ssid(FSettings *settings, const char *ap_ssid);
const char *settings_get_portal_ap_ssid(const FSettings *settings);

void settings_set_portal_domain(FSettings *settings, const char *domain);
const char *settings_get_portal_domain(const FSettings *settings);

void settings_set_portal_offline_mode(FSettings *settings, bool offline_mode);
bool settings_get_portal_offline_mode(const FSettings *settings);

// Getters and Setters for Power Printer
void settings_set_printer_ip(FSettings *settings, const char *ip);
const char *settings_get_printer_ip(const FSettings *settings);

void settings_set_printer_text(FSettings *settings, const char *text);
const char *settings_get_printer_text(const FSettings *settings);

void settings_set_printer_font_size(FSettings *settings, uint8_t font_size);
uint8_t settings_get_printer_font_size(const FSettings *settings);

void settings_set_printer_alignment(FSettings *settings,
                                    PrinterAlignment alignment);
PrinterAlignment settings_get_printer_alignment(const FSettings *settings);

void settings_set_display_timeout(FSettings *settings, uint32_t timeout_ms);
uint32_t settings_get_display_timeout(const FSettings *settings);

// Station Mode Credentials
void settings_set_sta_ssid(FSettings *settings, const char *ssid);
const char *settings_get_sta_ssid(const FSettings *settings);
void settings_set_sta_password(FSettings *settings, const char *password);
const char *settings_get_sta_password(const FSettings *settings);

// WiFi auto-reconnect on involuntary disconnect
void settings_set_wifi_auto_reconnect(FSettings *settings, bool enabled);
bool settings_get_wifi_auto_reconnect(const FSettings *settings);

// Functions to get/set RGB pin configuration
void settings_set_rgb_data_pin(FSettings *settings, int32_t pin);
int32_t settings_get_rgb_data_pin(const FSettings *settings);
void settings_set_rgb_separate_pins(FSettings *settings, int32_t red, int32_t green, int32_t blue);
void settings_get_rgb_separate_pins(const FSettings *settings, int32_t *red, int32_t *green, int32_t *blue);
void settings_set_rgb_led_count(FSettings *settings, uint16_t count);
uint16_t settings_get_rgb_led_count(const FSettings *settings);

void settings_set_thirds_control_enabled(FSettings *settings, bool enabled);
bool settings_get_thirds_control_enabled(const FSettings *settings);

void settings_set_menu_theme(FSettings *settings, uint8_t theme);
uint8_t settings_get_menu_theme(const FSettings *settings);

void settings_set_terminal_text_color(FSettings *settings, uint32_t color);
uint32_t settings_get_terminal_text_color(const FSettings *settings);
void settings_set_terminal_font_size(FSettings *settings, uint8_t size);
uint8_t settings_get_terminal_font_size(const FSettings *settings);
void settings_set_invert_colors(FSettings *settings, bool enabled);
bool settings_get_invert_colors(const FSettings *settings);

// Getter and Setter for web auth
void settings_set_web_auth_enabled(FSettings *settings, bool enabled);
bool settings_get_web_auth_enabled(const FSettings *settings);
void settings_set_usb_msc_enabled(FSettings *settings, bool enabled);
bool settings_get_usb_msc_enabled(const FSettings *settings);
void settings_set_webui_restrict_to_ap(FSettings *settings, bool enabled);
bool settings_get_webui_restrict_to_ap(const FSettings *settings);

void settings_set_esp_comm_pins(FSettings *settings, int32_t tx_pin, int32_t rx_pin);
void settings_get_esp_comm_pins(const FSettings *settings, int32_t *tx_pin, int32_t *rx_pin);

// NVS Storage Monitoring Functions
void settings_get_nvs_stats(nvs_stats_t *stats);
size_t settings_get_nvs_used_entries(void);
size_t settings_get_nvs_free_entries(void);
size_t settings_get_nvs_total_entries(void);
float settings_get_nvs_usage_percentage(void);
void settings_print_nvs_stats(void);
size_t settings_get_namespace_used_entries(const char *namespace_name);
void settings_print_namespace_stats(const char *namespace_name);

// Getter and Setter for AP enabled state
void settings_set_ap_enabled(FSettings *settings, bool enabled);
bool settings_get_ap_enabled(const FSettings *settings);

// Getter and Setter for power save enabled state
bool settings_get_power_save_enabled(const FSettings *settings);
void settings_set_power_save_enabled(FSettings *settings, bool enabled);

// Brightness settings
void settings_set_max_screen_brightness(FSettings *settings, uint8_t value);
uint8_t settings_get_max_screen_brightness(const FSettings *settings);

// Infrared settings
void settings_set_infrared_easy_mode(FSettings *settings, bool enabled);
bool settings_get_infrared_easy_mode(const FSettings *settings);

// Navigation buttons settings
void settings_set_nav_buttons_enabled(FSettings *settings, bool enabled);
bool settings_get_nav_buttons_enabled(const FSettings *settings);

// Menu layout settings
void settings_set_menu_layout(FSettings *settings, uint8_t layout);
uint8_t settings_get_menu_layout(const FSettings *settings);

// Carousel slide direction inversion settings
void settings_set_carousel_invert_direction(FSettings *settings, bool enabled);
bool settings_get_carousel_invert_direction(const FSettings *settings);

// Neopixel brightness settings
void settings_set_neopixel_max_brightness(FSettings *settings, uint8_t brightness);
uint8_t settings_get_neopixel_max_brightness(const FSettings *settings);

// Encoder direction inversion settings
void settings_set_encoder_invert_direction(FSettings *settings, bool enabled);
bool settings_get_encoder_invert_direction(const FSettings *settings);
#ifdef CONFIG_USE_ENCODER
void settings_set_encoder_legacy_latch(FSettings *settings, bool enabled);
bool settings_get_encoder_legacy_latch(const FSettings *settings);
#endif

void settings_set_auto_save_scans(FSettings *settings, bool enabled);
bool settings_get_auto_save_scans(const FSettings *settings);

// Setup wizard settings
void settings_set_setup_complete(FSettings *settings, bool complete);
bool settings_get_setup_complete(const FSettings *settings);
void settings_set_wifi_country(FSettings *settings, uint8_t country);
uint8_t settings_get_wifi_country(const FSettings *settings);

void settings_set_wigle_auto_upload(FSettings *settings, bool enabled);
bool settings_get_wigle_auto_upload(const FSettings *settings);

void settings_set_ota_channel(FSettings *settings, uint8_t channel);
uint8_t settings_get_ota_channel(const FSettings *settings);
void settings_set_ota_update_available(FSettings *settings, bool available);
bool settings_get_ota_update_available(const FSettings *settings);
void settings_set_ota_last_check_time(FSettings *settings, uint32_t timestamp);
uint32_t settings_get_ota_last_check_time(const FSettings *settings);
void settings_set_wigle_donate(FSettings *settings, bool enabled);
bool settings_get_wigle_donate(const FSettings *settings);

#ifdef CONFIG_WITH_STATUS_DISPLAY
// Status display idle animation accessors
void settings_set_status_idle_animation(FSettings *settings, IdleAnimation anim);
IdleAnimation settings_get_status_idle_animation(const FSettings *settings);
void settings_set_status_idle_timeout_ms(FSettings *settings, uint32_t timeout_ms);
uint32_t settings_get_status_idle_timeout_ms(const FSettings *settings);
#endif

// IO expander programmable buttons (P10, P11, P12)
const char *settings_get_io_btn_p10_cmd(const FSettings *settings);
void settings_set_io_btn_p10_cmd(FSettings *settings, const char *cmd);
const char *settings_get_io_btn_p11_cmd(const FSettings *settings);
void settings_set_io_btn_p11_cmd(FSettings *settings, const char *cmd);
const char *settings_get_io_btn_p12_cmd(const FSettings *settings);
void settings_set_io_btn_p12_cmd(FSettings *settings, const char *cmd);

// BadUSB emulation settings
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
void settings_set_badusb_vid(FSettings *settings, uint16_t vid);
uint16_t settings_get_badusb_vid(const FSettings *settings);
void settings_set_badusb_pid(FSettings *settings, uint16_t pid);
uint16_t settings_get_badusb_pid(const FSettings *settings);
void settings_set_badusb_manufacturer(FSettings *settings, const char *name);
const char *settings_get_badusb_manufacturer(const FSettings *settings);
void settings_set_badusb_product(FSettings *settings, const char *name);
const char *settings_get_badusb_product(const FSettings *settings);
void settings_set_badusb_randomize(FSettings *settings, bool enabled);
bool settings_get_badusb_randomize(const FSettings *settings);
void settings_set_badusb_kb_layout(FSettings *settings, uint8_t layout);
uint8_t settings_get_badusb_kb_layout(const FSettings *settings);
void settings_reset_badusb_defaults(FSettings *settings);
#endif

// MIC RGB Visualizer getters and setters
void settings_set_mic_visualizer_mode(FSettings *settings, MicVisualizerMode mode);
MicVisualizerMode settings_get_mic_visualizer_mode(const FSettings *settings);
void settings_set_mic_color_mode(FSettings *settings, MicColorMode mode);
MicColorMode settings_get_mic_color_mode(const FSettings *settings);
void settings_set_mic_sensitivity(FSettings *settings, uint8_t sensitivity);
uint8_t settings_get_mic_sensitivity(const FSettings *settings);
void settings_set_mic_smoothing(FSettings *settings, uint8_t smoothing);
uint8_t settings_get_mic_smoothing(const FSettings *settings);
void settings_set_mic_contrast(FSettings *settings, uint8_t contrast);
uint8_t settings_get_mic_contrast(const FSettings *settings);
void settings_set_mic_mirror_mode(FSettings *settings, bool enabled);
bool settings_get_mic_mirror_mode(const FSettings *settings);
void settings_set_mic_calibrate(FSettings *settings, bool calibrate);
bool settings_get_mic_calibrate(const FSettings *settings);

void settings_set_ghostlink_split_view(FSettings *settings, bool enabled);
bool settings_get_ghostlink_split_view(const FSettings *settings);

void settings_set_menu_bg_shade(FSettings *settings, uint8_t shade);
uint8_t settings_get_menu_bg_shade(const FSettings *settings);
void settings_set_theme_background_effects(FSettings *settings, bool enabled);
bool settings_get_theme_background_effects(const FSettings *settings);
/* Enforce mutual exclusivity of accessibility display modes in-place. */
void settings_normalize_modes(FSettings *settings);
void settings_set_menu_rounded(FSettings *settings, bool enabled);
bool settings_get_menu_rounded(const FSettings *settings);
void settings_set_epilepsy_warning_enabled(FSettings *settings, bool enabled);
bool settings_get_epilepsy_warning_enabled(const FSettings *settings);

void settings_set_font_size(FSettings *settings, uint8_t size);
uint8_t settings_get_font_size(const FSettings *settings);
void settings_set_row_height(FSettings *settings, uint8_t height);
uint8_t settings_get_row_height(const FSettings *settings);
void settings_set_reduced_motion(FSettings *settings, bool enabled);
bool settings_get_reduced_motion(const FSettings *settings);
void settings_set_input_repeat_speed(FSettings *settings, uint8_t speed);
uint8_t settings_get_input_repeat_speed(const FSettings *settings);
void settings_set_high_contrast(FSettings *settings, bool enabled);
bool settings_get_high_contrast(const FSettings *settings);
void settings_set_sun_mode(FSettings *settings, bool enabled);
bool settings_get_sun_mode(const FSettings *settings);
void settings_set_log_level(FSettings *settings, uint8_t level);
uint8_t settings_get_log_level(const FSettings *settings);
bool settings_is_favorite(const FSettings *settings, const char *name);
bool settings_add_favorite(FSettings *settings, const char *name);
bool settings_remove_favorite(FSettings *settings, const char *name);
bool settings_toggle_favorite(FSettings *settings, const char *name);
uint8_t settings_get_favorites_count(const FSettings *settings);
const char *settings_get_favorite(const FSettings *settings, uint8_t idx);
void settings_set_favorites_bypass(FSettings *settings, bool bypass);
bool settings_get_favorites_bypass(const FSettings *settings);
void settings_set_menu_item_borders(FSettings *settings, bool enabled);
bool settings_get_menu_item_borders(const FSettings *settings);
void settings_set_menu_card_bg(FSettings *settings, bool enabled);
bool settings_get_menu_card_bg(const FSettings *settings);
void settings_set_touch_drag_scroll(FSettings *settings, bool enabled);
bool settings_get_touch_drag_scroll(const FSettings *settings);

// Lockscreen getters and setters
void settings_set_lockscreen_enabled(FSettings *settings, bool enabled);
bool settings_get_lockscreen_enabled(const FSettings *settings);
void settings_set_lockscreen_type(FSettings *settings, uint8_t type);
uint8_t settings_get_lockscreen_type(const FSettings *settings);
void settings_set_lockscreen_obfuscated(FSettings *settings, const char *obf);
const char *settings_get_lockscreen_obfuscated(const FSettings *settings);
void settings_set_lockscreen_timeout_sec(FSettings *settings, uint16_t sec);
uint16_t settings_get_lockscreen_timeout_sec(const FSettings *settings);
void settings_set_lockscreen_wake_lock(FSettings *settings, bool enabled);
bool settings_get_lockscreen_wake_lock(const FSettings *settings);

// Wardriving settings
void settings_set_wd_hop_primary_ms(FSettings *settings, uint16_t ms);
uint16_t settings_get_wd_hop_primary_ms(const FSettings *settings);
void settings_set_wd_hop_helper_ms(FSettings *settings, uint16_t ms);
uint16_t settings_get_wd_hop_helper_ms(const FSettings *settings);
void settings_set_wd_weighted_5g(FSettings *settings, bool enabled);
bool settings_get_wd_weighted_5g(const FSettings *settings);

extern FSettings G_Settings;

#endif // SETTINGS_MANAGER_H
