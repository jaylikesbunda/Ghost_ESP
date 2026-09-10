// cmd_settings.c
// Settings, time, timezone, web auth, WebUI AP, and config load commands.

#include "core/commands.h"
#include "core/glog.h"
#include "gui/theme_palette_api.h"
#include "managers/config_manager.h"
#include "managers/settings_manager.h"
#include "scans/wifi/hop_profile.h"
#include "managers/settings_sd_backup.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "sdkconfig.h"
#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// settings registry to avoid ridiculously long strcmp chains, fuck that lmaooo.
typedef enum {
    ST_I32,
    ST_U32,
    ST_U16,
    ST_U8,
    ST_BOOL,
    ST_FLOAT,
    ST_ENUM8,
    ST_STRING,
    ST_COLOR_HEX
} SettingType;

typedef struct {
    const char *name;
    SettingType type;
    size_t offset;
    const char *category;
    uint16_t str_capacity; // only for ST_STRING
    int min_i; // for *_U8/_U16/_I32/_ENUM8 bounds (simple)
    int max_i;
} SettingDescriptor;

#define OFF(field) offsetof(FSettings, field)

static const SettingDescriptor k_settings_desc[] = {
    {"rgb_mode", ST_ENUM8, OFF(rgb_mode), "RGB", 0, 0, 2},
    {"rgb_speed", ST_U8, OFF(rgb_speed), "RGB", 0, 0, 255},
    {"rgb_data_pin", ST_I32, OFF(rgb_data_pin), "RGB", 0, 0, 0},
    {"rgb_red_pin", ST_I32, OFF(rgb_red_pin), "RGB", 0, 0, 0},
    {"rgb_green_pin", ST_I32, OFF(rgb_green_pin), "RGB", 0, 0, 0},
    {"rgb_blue_pin", ST_I32, OFF(rgb_blue_pin), "RGB", 0, 0, 0},
    {"neopixel_bright", ST_U8, OFF(neopixel_max_brightness), "RGB", 0, 0, 100},

    {"ap_ssid", ST_STRING, OFF(ap_ssid), "WiFi", 33, 0, 0},
    {"ap_password", ST_STRING, OFF(ap_password), "WiFi", 65, 0, 0},
    {"ap_enabled", ST_BOOL, OFF(ap_enabled), "WiFi", 0, 0, 0},
    {"sta_ssid", ST_STRING, OFF(sta_ssid), "WiFi", 65, 0, 0},
    {"sta_password", ST_STRING, OFF(sta_password), "WiFi", 65, 0, 0},

    {"portal_url", ST_STRING, OFF(portal_url), "Portal", 129, 0, 0},
    {"portal_ssid", ST_STRING, OFF(portal_ssid), "Portal", 33, 0, 0},
    {"portal_password", ST_STRING, OFF(portal_password), "Portal", 65, 0, 0},
    {"portal_ap_ssid", ST_STRING, OFF(portal_ap_ssid), "Portal", 33, 0, 0},
    {"portal_domain", ST_STRING, OFF(portal_domain), "Portal", 65, 0, 0},
    {"portal_offline", ST_BOOL, OFF(portal_offline_mode), "Portal", 0, 0, 0},

    {"printer_ip", ST_STRING, OFF(printer_ip), "Printer", 16, 0, 0},
    {"printer_text", ST_STRING, OFF(printer_text), "Printer", 257, 0, 0},
    {"printer_font_size", ST_U8, OFF(printer_font_size), "Printer", 0, 1, 255},
    {"printer_alignment", ST_ENUM8, OFF(printer_alignment), "Printer", 0, 0, 4},

    {"display_timeout", ST_U32, OFF(display_timeout_ms), "Display", 0, 0, 0},
    {"max_bright", ST_U8, OFF(max_screen_brightness), "Display", 0, 0, 100},
    {"invert_colors", ST_BOOL, OFF(invert_colors), "Display", 0, 0, 0},
    {"terminal_color", ST_COLOR_HEX, OFF(terminal_text_color), "Display", 0, 0, 0},
    {"terminal_font_size", ST_U8, OFF(terminal_font_size), "Display", 0, 0, 2},
    {"menu_theme", ST_U8, OFF(menu_theme), "Display", 0, 0, THEME_PALETTE_THEME_COUNT - 1},
    {"font_size", ST_U8, OFF(font_size), "Display", 0, 0, 2},
    {"row_height", ST_U8, OFF(row_height), "Display", 0, 0, MENU_ROW_HEIGHT_OPTION_COUNT - 1},
    {"reduce_motion", ST_BOOL, OFF(reduced_motion), "Display", 0, 0, 0},
    {"repeat_speed", ST_U8, OFF(input_repeat_speed), "Display", 0, 0, 2},
    {"high_contrast", ST_BOOL, OFF(high_contrast), "Display", 0, 0, 0},
    {"sun_mode", ST_BOOL, OFF(sun_mode), "Display", 0, 0, 0},
    {"theme_bg_fx", ST_BOOL, OFF(theme_background_effects), "Display", 0, 0, 0},
    {"channel_delay", ST_FLOAT, OFF(channel_delay), "System", 0, 0, 0},
    {"broadcast_speed", ST_U16, OFF(broadcast_speed), "System", 0, 0, 65535},
    {"hop_mode", ST_ENUM8, OFF(hop_mode), "System", 0, 0, HOP_MODE_COUNT - 1},
    {"hop_custom", ST_STRING, OFF(hop_custom_channels), "System", 129, 0, 0},
    {"gps_rx_pin", ST_I32, OFF(gps_rx_pin), "System", 0, 0, 0},
    {"gps_baud_rate", ST_U32, OFF(gps_baud_rate), "System", 0, 0, 0},
    {"ir_tx_pin", ST_I32, OFF(ir_tx_pin), "System", 0, -1, 127},
    {"ir_rx_pin", ST_I32, OFF(ir_rx_pin), "System", 0, -1, 127},
    {"power_save", ST_BOOL, OFF(power_save_enabled), "System", 0, 0, 0},
    {"zebra_menus", ST_BOOL, OFF(zebra_menus_enabled), "System", 0, 0, 0},
    {"nav_buttons", ST_BOOL, OFF(nav_buttons_enabled), "System", 0, 0, 0},
    {"menu_layout", ST_U8, OFF(menu_layout), "System", 0, 0, 4},
    {"infrared_easy", ST_BOOL, OFF(infrared_easy_mode), "System", 0, 0, 0},
    {"web_auth", ST_BOOL, OFF(web_auth_enabled), "System", 0, 0, 0},
    {"usb_msc", ST_BOOL, OFF(usb_msc_enabled), "System", 0, 0, 0},
    {"rts_enabled", ST_BOOL, OFF(rts_enabled), "System", 0, 0, 0},
    {"third_ctrl", ST_BOOL, OFF(third_control_enabled), "System", 0, 0, 0},
    {"auto_save_scans", ST_BOOL, OFF(auto_save_scans), "System", 0, 0, 0},

    {"flappy_name", ST_STRING, OFF(flappy_ghost_name), "Personalisation", 65, 0, 0},
    {"timezone", ST_STRING, OFF(selected_timezone), "Date & Time", 25, 0, 0},
    {"accent_color", ST_STRING, OFF(selected_hex_accent_color), "Personalisation", 25, 0, 0},
    {"io_btn_p10_cmd", ST_STRING, OFF(io_btn_p10_cmd), "IO Button", 129, 0, 0},
    {"io_btn_p11_cmd", ST_STRING, OFF(io_btn_p11_cmd), "IO Button", 129, 0, 0},
    {"io_btn_p12_cmd", ST_STRING, OFF(io_btn_p12_cmd), "IO Button", 129, 0, 0},
};

static const SettingDescriptor *find_setting_desc(const char *name) {
    for (size_t i = 0; i < (sizeof(k_settings_desc)/sizeof(k_settings_desc[0])); ++i) {
        if (strcmp(k_settings_desc[i].name, name) == 0) return &k_settings_desc[i];
    }
    return NULL;
}

static void print_setting_value(const SettingDescriptor *d, const FSettings *s) {
    const uint8_t *base = (const uint8_t *)s;
    const void *ptr = base + d->offset;
    if (d->type == ST_STRING) {
        glog("%s = \"%s\"\n", d->name, (const char *)ptr);
        return;
    }
    switch (d->type) {
        case ST_BOOL: {
            bool v = *(const bool *)ptr;
            glog("%s = %s\n", d->name, v ? "true" : "false");
        } break;
        case ST_U8: {
            glog("%s = %d\n", d->name, *(const uint8_t *)ptr);
        } break;
        case ST_U16: {
            glog("%s = %d\n", d->name, *(const uint16_t *)ptr);
        } break;
        case ST_U32: {
            glog("%s = %lu\n", d->name, (unsigned long)*(const uint32_t *)ptr);
        } break;
        case ST_I32: {
            glog("%s = %ld\n", d->name, (long)*(const int32_t *)ptr);
        } break;
        case ST_FLOAT: {
            glog("%s = %.2f\n", d->name, *(const float *)ptr);
        } break;
        case ST_ENUM8: {
            glog("%s = %d\n", d->name, *(const uint8_t *)ptr);
        } break;
        case ST_COLOR_HEX: {
            unsigned long v = (unsigned long)*(const uint32_t *)ptr;
            glog("%s = 0x%06lX\n", d->name, v);
        } break;
        default: {
            glog("%s = ?\n", d->name);
        } break;
    }
}

static bool set_setting_value(const SettingDescriptor *d, FSettings *s, const char *value) {
    uint8_t *base = (uint8_t *)s;
    void *ptr = base + d->offset;
    switch (d->type) {
        case ST_STRING: {
            if (d->str_capacity == 0) return false;
            strncpy((char *)ptr, value, d->str_capacity - 1);
            ((char *)ptr)[d->str_capacity - 1] = '\0';
            return true;
        }
        case ST_BOOL: {
            if (strcmp(value, "true") == 0) {
                *(bool *)ptr = true; return true;
            } else if (strcmp(value, "false") == 0) {
                *(bool *)ptr = false; return true;
            }
            return false;
        }
        case ST_U8: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint8_t *)ptr = (uint8_t)v; return true;
        }
        case ST_U16: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint16_t *)ptr = (uint16_t)v; return true;
        }
        case ST_U32: {
            unsigned long v = strtoul(value, NULL, 10);
            *(uint32_t *)ptr = (uint32_t)v; return true;
        }
        case ST_I32: {
            long v = strtol(value, NULL, 10);
            *(int32_t *)ptr = (int32_t)v; return true;
        }
        case ST_FLOAT: {
            float v = atof(value);
            *(float *)ptr = v; return true;
        }
        case ST_ENUM8: {
            int v = atoi(value);
            if (d->max_i > d->min_i) {
                if (v < d->min_i || v > d->max_i) return false;
            }
            *(uint8_t *)ptr = (uint8_t)v; return true;
        }
        case ST_COLOR_HEX: {
            unsigned long v = strtoul(value, NULL, 16);
            *(uint32_t *)ptr = (uint32_t)v; return true;
        }
        default:
            return false;
    }
}

static void reset_setting_value(const SettingDescriptor *d, FSettings *s, const FSettings *defaults) {
    const uint8_t *db = (const uint8_t *)defaults;
    const void *src = db + d->offset;
    uint8_t *sb = (uint8_t *)s;
    void *dst = sb + d->offset;
    switch (d->type) {
        case ST_STRING:
            strncpy((char *)dst, (const char *)src, d->str_capacity - 1);
            ((char *)dst)[d->str_capacity - 1] = '\0';
            break;
        case ST_BOOL:
            *(bool *)dst = *(const bool *)src; break;
        case ST_U8:
            *(uint8_t *)dst = *(const uint8_t *)src; break;
        case ST_U16:
            *(uint16_t *)dst = *(const uint16_t *)src; break;
        case ST_U32:
            *(uint32_t *)dst = *(const uint32_t *)src; break;
        case ST_I32:
            *(int32_t *)dst = *(const int32_t *)src; break;
        case ST_FLOAT:
            *(float *)dst = *(const float *)src; break;
        case ST_ENUM8:
            *(uint8_t *)dst = *(const uint8_t *)src; break;
        case ST_COLOR_HEX:
            *(uint32_t *)dst = *(const uint32_t *)src; break;
        default: break;
    }
}

static void log_set_confirmation(const SettingDescriptor *d, const FSettings *s) {
    const uint8_t *base = (const uint8_t *)s;
    const void *ptr = base + d->offset;
    switch (d->type) {
        case ST_STRING:
            glog("Set %s to \"%s\"\n", d->name, (const char *)ptr);
            break;
        case ST_BOOL:
            glog("Set %s to %s\n", d->name, (*(const bool *)ptr) ? "true" : "false");
            break;
        case ST_U8:
            glog("Set %s to %d\n", d->name, *(const uint8_t *)ptr);
            break;
        case ST_U16:
            glog("Set %s to %d\n", d->name, *(const uint16_t *)ptr);
            break;
        case ST_U32:
            glog("Set %s to %lu\n", d->name, (unsigned long)*(const uint32_t *)ptr);
            break;
        case ST_I32:
            glog("Set %s to %ld\n", d->name, (long)*(const int32_t *)ptr);
            break;
        case ST_FLOAT:
            glog("Set %s to %.2f\n", d->name, *(const float *)ptr);
            break;
        case ST_ENUM8:
            glog("Set %s to %d\n", d->name, *(const uint8_t *)ptr);
            break;
        case ST_COLOR_HEX: {
            unsigned long v = (unsigned long)*(const uint32_t *)ptr;
            glog("Set %s to 0x%06lX\n", d->name, v);
        } break;
        default:
            glog("Set %s\n", d->name);
            break;
    }
}

// settime - Set system time from a Unix timestamp
void handle_settime_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: settime <unix_timestamp>\n");
        glog("Example: settime 1704067200\n");
        status_display_show_status("SetTime Use");
        return;
    }
    
    long timestamp = strtol(argv[1], NULL, 10);
    if (timestamp < 0) {
        glog("Invalid timestamp: %s\n", argv[1]);
        status_display_show_status("SetTime Invalid");
        return;
    }
    
    struct timeval tv;
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    
    if (settimeofday(&tv, NULL) != 0) {
        glog("Failed to set system time: %s\n", strerror(errno));
        status_display_show_status("SetTime Fail");
        return;
    }
    
    // Display the set time
    time_t now = (time_t)timestamp;
    struct tm timeinfo;
    char strftime_buf[64];
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    
    glog("System time set successfully!\n");
    glog("Time: %s\n", strftime_buf);
    
#ifdef CONFIG_HAS_RTC_CLOCK
    // Save UTC time to RTC (not local time)
    struct tm utc_timeinfo;
    gmtime_r(&timestamp, &utc_timeinfo);
    RTC_Date rtc_time;
    rtc_time.year = utc_timeinfo.tm_year + 1900;
    rtc_time.month = utc_timeinfo.tm_mon + 1;
    rtc_time.day = utc_timeinfo.tm_mday;
    rtc_time.hour = utc_timeinfo.tm_hour;
    rtc_time.minute = utc_timeinfo.tm_min;
    rtc_time.second = utc_timeinfo.tm_sec;
    
    if (rtc_set_datetime(&rtc_time) == ESP_OK) {
        glog("UTC time saved to RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
             rtc_time.year, rtc_time.month, rtc_time.day, 
             rtc_time.hour, rtc_time.minute, rtc_time.second);
    } else {
        glog("Failed to save time to RTC\n");
    }
#endif
    
    status_display_show_status("Time Set OK");
}

// time - Display current system time
void handle_time_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    time_t now;
    struct tm timeinfo;
    char strftime_buf[64];
    
    time(&now);
    
    // Display local time
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    glog("Local time: %s\n", strftime_buf);
    
    // Display UTC time
    struct tm timeinfo_utc;
    gmtime_r(&now, &timeinfo_utc);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo_utc);
    glog("UTC time: %s\n", strftime_buf);
    
    // Display Unix timestamp
    glog("Unix timestamp: %ld\n", (long)now);
}

void handle_timezone_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: timezone <TZ_STRING>\n");
        status_display_show_status("Timezone Usage");
        return;
    }
    const char *tz = argv[1];
    settings_set_timezone_str(&G_Settings, tz);
    settings_save(&G_Settings);
    setenv("TZ", tz, 1);
    tzset();
    glog("Timezone set to: %s\n", tz);
    status_display_show_status("Timezone Set");
}

void handle_web_auth_cmd(int argc, char **argv) {
    bool enabled = settings_get_web_auth_enabled(&G_Settings);

    if (argc == 1) {
        enabled = !enabled;
        settings_set_web_auth_enabled(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("Web authentication %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            enabled = true;
        } else if (strcmp(argv[1], "off") == 0) {
            enabled = false;
        } else if (strcmp(argv[1], "toggle") == 0) {
            enabled = !enabled;
        } else if (strcmp(argv[1], "status") == 0) {
            glog("Web authentication is %s.\n", enabled ? "enabled" : "disabled");
            return;
        } else {
            glog("Usage: webauth [on|off|toggle|status]\n");
            return;
        }

        settings_set_web_auth_enabled(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("Web authentication %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    glog("Usage: webauth [on|off|toggle|status]\n");
}

void handle_webuiap_cmd(int argc, char **argv) {
    bool enabled = settings_get_webui_restrict_to_ap(&G_Settings);

    if (argc == 1) {
        enabled = !enabled;
        settings_set_webui_restrict_to_ap(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("WebUI AP-only restriction %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    if (argc == 2) {
        if (strcmp(argv[1], "on") == 0) {
            enabled = true;
        } else if (strcmp(argv[1], "off") == 0) {
            enabled = false;
        } else if (strcmp(argv[1], "toggle") == 0) {
            enabled = !enabled;
        } else if (strcmp(argv[1], "status") == 0) {
            glog("WebUI AP-only restriction is %s.\n", enabled ? "enabled" : "disabled");
            return;
        } else {
            glog("Usage: webuiap [on|off|toggle|status]\n");
            return;
        }

        settings_set_webui_restrict_to_ap(&G_Settings, enabled);
        settings_save(&G_Settings);
        glog("WebUI AP-only restriction %s.\n", enabled ? "enabled" : "disabled");
        return;
    }

    glog("Usage: webuiap [on|off|toggle|status]\n");
}

// Settings command handler
void handle_settings_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Settings Management Commands:\n");
        glog("  settings list                    - List all available settings\n");
        glog("  settings get <setting>           - Get current value of a setting\n");
        glog("  settings set <setting> <value>   - Set a setting to a value\n");
        glog("  settings reset [setting]         - Reset setting(s) to defaults\n");
        glog("  settings backup export|import  - Full settings JSON on SD card\n");
        glog("  settings help                    - Show this help\n");
        return;
    }

    if (strcmp(argv[1], "help") == 0) {
        glog("Settings Management Commands:\n");
        glog("  settings list                    - List all available settings\n");
        glog("  settings get <setting>           - Get current value of a setting\n");
        glog("  settings set <setting> <value>   - Set a setting to a value\n");
        glog("  settings reset [setting]         - Reset setting(s) to defaults\n");
        glog("  settings backup export|import  - Full settings JSON on SD card\n");
        glog("  settings help                    - Show this help\n");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        glog("Available Settings:\n");
        glog("  RGB Settings:\n");
        glog("    rgb_mode          - RGB mode (0=Normal, 1=Rainbow, 2=Stealth)\n");
        glog("    rgb_speed         - RGB animation speed (0-255)\n");
        glog("    rgb_data_pin      - RGB data pin (-1 if not used)\n");
        glog("    rgb_red_pin       - RGB red pin (-1 if not used)\n");
        glog("    rgb_green_pin     - RGB green pin (-1 if not used)\n");
        glog("    rgb_blue_pin      - RGB blue pin (-1 if not used)\n");
        glog("    neopixel_bright   - Neopixel max brightness (0-100)\n");
        glog("  WiFi Settings:\n");
        glog("    ap_ssid           - Access Point SSID\n");
        glog("    ap_password       - Access Point password\n");
        glog("    ap_enabled        - Enable AP on boot (true/false)\n");
        glog("    sta_ssid          - Station mode SSID\n");
        glog("    sta_password      - Station mode password\n");
        glog("  Evil Portal Settings:\n");
        glog("    portal_url        - Portal URL or file path\n");
        glog("    portal_ssid       - Portal SSID\n");
        glog("    portal_password   - Portal password\n");
        glog("    portal_ap_ssid    - Portal AP SSID\n");
        glog("    portal_domain     - Portal domain\n");
        glog("    portal_offline    - Portal offline mode (true/false)\n");
        glog("  Printer Settings:\n");
        glog("    printer_ip        - Printer IP address\n");
        glog("    printer_text      - Last printed text\n");
        glog("    printer_font_size - Printer font size\n");
        glog("    printer_alignment - Printer alignment (0-4)\n");
        glog("  Display Settings:\n");
        glog("    display_timeout   - Display timeout in ms\n");
        glog("    max_bright        - Max screen brightness (0-100)\n");
        glog("    invert_colors     - Invert screen colors (true/false)\n");
        glog("    terminal_color    - Terminal text color (hex)\n");
        glog("    terminal_font_size - Terminal font size (0=Small,1=Normal,2=Large)\n");
        glog("    menu_theme        - Menu theme palette index\n");
        glog("    font_size         - Global font size (0=Small,1=Normal,2=Large)\n");
        glog("    row_height        - Options row height (0=Compact,1=Normal,2=Large,3=Extra Large)\n");
        glog("    reduce_motion     - Reduce animations (true/false)\n");
        glog("    repeat_speed      - Input repeat speed (0-2)\n");
        glog("    high_contrast     - High contrast mode (true/false)\n");
        glog("    sun_mode          - Sun mode (true/false)\n");
        glog("  System Settings:\n");
        glog("    channel_delay     - Channel delay in ms\n");
        glog("    broadcast_speed   - Broadcast speed\n");
        glog("    gps_rx_pin        - GPS RX pin\n");
        glog("    gps_baud_rate     - GPS UART baud rate (0 = Kconfig default)\n");
        glog("    ir_tx_pin         - IR TX pin (-1 = board default)\n");
        glog("    ir_rx_pin         - IR RX pin (-1 = board default)\n");
        glog("    power_save        - Power save mode (true/false)\n");
        glog("    zebra_menus       - Zebra menus (true/false)\n");
        glog("    nav_buttons       - Navigation buttons (true/false)\n");
        glog("    menu_layout       - Menu layout (0=Carousel, 1=Grid, 2=List, 3=Compact, 4=Hero)\n");
        glog("    infrared_easy     - Infrared easy mode (true/false)\n");
        glog("    web_auth          - Web authentication (true/false)\n");
        glog("    usb_msc           - USB SD passthrough toggle preference (true/false)\n");
        glog("    rts_enabled       - RTS enabled (true/false)\n");
        glog("    third_ctrl        - Third control enabled (true/false)\n");
        glog("    auto_save_scans   - Auto save scan results to SD (true/false)\n");
        glog("  Date & Time Settings:\n");
        glog("    timezone          - Selected timezone\n");
        glog("  Personalisation Settings:\n");
        glog("    flappy_name       - Flappy Ghost name\n");
        glog("    accent_color      - Accent color (hex)\n");
        glog("  IO expander buttons (P10/P11/P12):\n");
        glog("    io_btn_p10_cmd    - Command to run when P10 pressed (empty = joystick)\n");
        glog("    io_btn_p11_cmd    - Command to run when P11 pressed (empty = joystick)\n");
        glog("    io_btn_p12_cmd    - Command to run when P12 pressed (empty = joystick)\n");
        return;
    }

    if (strcmp(argv[1], "get") == 0) {
        if (argc < 3) { glog("Usage: settings get <setting>\n"); return; }
        const char *setting = argv[2];
        const SettingDescriptor *d = find_setting_desc(setting);
        if (!d) {
            glog("Unknown setting: %s\n", setting);
            glog("Use 'settings list' to see available settings\n");
            return;
        }
        print_setting_value(d, &G_Settings);
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 4) { glog("Usage: settings set <setting> <value>\n"); return; }
        const char *setting = argv[2];
        const char *value = argv[3];
        const SettingDescriptor *d = find_setting_desc(setting);
        if (!d) {
            glog("Unknown setting: %s\n", setting);
            glog("Use 'settings list' to see available settings\n");
            return;
        }
        FSettings *settings = &G_Settings;
        // GPIO validity for pin overrides: capture the old value, run the raw
        // write, then re-validate through the validated setter and revert.
        int32_t ir_pin_backup = 0;
        bool ir_pin_desc = strcmp(d->name, "ir_tx_pin") == 0 || strcmp(d->name, "ir_rx_pin") == 0;
        if (ir_pin_desc) {
            ir_pin_backup = *(const int32_t *)((const uint8_t *)settings + d->offset);
        }
        if (!set_setting_value(d, settings, value)) {
            if (d->type == ST_BOOL) {
                glog("Invalid %s. Use true or false\n", d->name);
            } else if (d->type == ST_U8 || d->type == ST_U16 || d->type == ST_ENUM8) {
                if (d->max_i > d->min_i) glog("Invalid %s. Use %d-%d\n", d->name, d->min_i, d->max_i);
                else glog("Invalid %s value\n", d->name);
            } else if (d->type == ST_COLOR_HEX) {
                glog("Invalid %s. Use hex like 00FF00\n", d->name);
            } else {
                glog("Invalid %s value\n", d->name);
            }
            return;
        }
        if (ir_pin_desc) {
            bool ok = strcmp(d->name, "ir_tx_pin") == 0
                          ? settings_set_ir_tx_pin(settings, settings->ir_tx_pin)
                          : settings_set_ir_rx_pin(settings, settings->ir_rx_pin);
            if (!ok) {
                *(int32_t *)((uint8_t *)settings + d->offset) = ir_pin_backup;
                glog("Invalid %s. Use -1 or a valid GPIO number\n", d->name);
                return;
            }
        }
        settings_normalize_modes(settings);
        settings_save(settings);
        log_set_confirmation(d, settings);
        return;
    }

    if (strcmp(argv[1], "reset") == 0) {
        if (argc == 2) {
            settings_set_defaults(&G_Settings);
            settings_save(&G_Settings);
            glog("Reset all settings to defaults\n");
        } else if (argc == 3) {
            const char *setting = argv[2];
            const SettingDescriptor *d = find_setting_desc(setting);
            if (!d) {
                glog("Unknown setting: %s\n", setting);
                glog("Use 'settings list' to see available settings\n");
                return;
            }
            // A local FSettings makes every settings command reserve several
            // KB of stack, including get/set. SerialTask also needs stack for
            // NVS calls, so allocate defaults only when resetting one field.
            FSettings *defaults = malloc(sizeof(*defaults));
            if (!defaults) {
                glog("Not enough memory to reset setting\n");
                return;
            }
            settings_set_defaults(defaults);
            reset_setting_value(d, &G_Settings, defaults);
            free(defaults);
            settings_save(&G_Settings);
            glog("Reset %s to default\n", d->name);
        } else {
            glog("Usage: settings reset [setting]\n");
        }
        return;
    }

    if (strcmp(argv[1], "backup") == 0) {
        if (argc < 3) {
            glog("Usage: settings backup export|import\n");
            glog("File: %s\n", SETTINGS_SD_BACKUP_PATH);
            return;
        }
        if (strcmp(argv[2], "export") == 0) {
            esp_err_t e = settings_backup_export_to_sd();
            glog("settings backup export: %s\n", esp_err_to_name(e));
            return;
        }
        if (strcmp(argv[2], "import") == 0) {
            esp_err_t e = settings_backup_import_from_sd();
            if (e == ESP_OK) {
                settings_backup_apply_runtime_after_import();
            }
            glog("settings backup import: %s\n", esp_err_to_name(e));
            return;
        }
        glog("Unknown: use export or import\n");
        return;
    }

    glog("Unknown settings command: %s\n", argv[1]);
    glog("Use 'settings help' for available commands\n");
}

void handle_loadconfig_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    glog("Loading configuration from SD card...\n");
    
    esp_err_t config_err = config_manager_load_from_sd();
    
    if (config_err == ESP_OK) {
        glog("\n=== Configuration Loaded Successfully ===\n");
        
        // Show what was loaded
        const char *ssid = settings_get_sta_ssid(&G_Settings);
        if (ssid && ssid[0]) {
            glog("WiFi SSID: %s\n", ssid);
            glog("WiFi Password: (set)\n");
        }
        
        if (G_Settings.wigle_api_key[0]) {
            glog("Wigle Token: (set)\n");
        }
        
        glog("Wigle Auto Upload: %s\n", settings_get_wigle_auto_upload(&G_Settings) ? "on" : "off");
        glog("Wigle Donate: %s\n", settings_get_wigle_donate(&G_Settings) ? "on" : "off");
        
        glog("\nSettings saved to NVS.\n");
        glog("Reconfiguring WiFi...\n");
        
        wifi_manager_configure_sta_from_settings();
        
        glog("Configuration applied successfully!\n");
    } else if (config_err == ESP_ERR_NOT_FOUND) {
        glog("Error: config.cfg not found on SD card\n");
        glog("Expected location: /mnt/ghostesp/config.cfg\n");
    } else {
        glog("Error: Failed to load config.cfg: %s\n", esp_err_to_name(config_err));
    }
}
