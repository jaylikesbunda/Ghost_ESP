#include "managers/views/clock_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/accessibility_fonts.h"
#include "lvgl.h"
#include <time.h>
#include "managers/settings_manager.h"
#include "gui/lvgl_safe.h"
#include "gui/asset_pack.h"
#include "gui/theme_palette_api.h"
#include "esp_log.h"

uint32_t theme_palette_get_background(uint8_t theme);

static lv_obj_t *clock_container;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *year_label;
static lv_obj_t *tz_label;
lv_timer_t *clock_timer = NULL;

// Get friendly timezone name
static const char* get_friendly_timezone_name(const char *tz) {
    if (!tz) return "Local";
    
    // Map common timezone strings to friendly names
    if (strstr(tz, "UTC") || strstr(tz, "GMT")) return "UTC";
    if (strstr(tz, "EST5EDT")) return "Eastern";
    if (strstr(tz, "CST6CDT")) return "Central";
    if (strstr(tz, "MST7MDT")) return "Mountain";
    if (strstr(tz, "PST8PDT")) return "Pacific";
    if (strstr(tz, "AWST-8")) return "Western Australia";
    if (strstr(tz, "America/New_York")) return "Eastern";
    if (strstr(tz, "America/Chicago")) return "Central";
    if (strstr(tz, "America/Denver")) return "Mountain";
    if (strstr(tz, "America/Los_Angeles")) return "Pacific";
    if (strstr(tz, "Europe/London")) return "London";
    if (strstr(tz, "Europe/Paris")) return "Paris";
    if (strstr(tz, "Asia/Tokyo")) return "Tokyo";
    if (strstr(tz, "Australia/Sydney")) return "Sydney";
    
    // Extract just the timezone abbreviation if it's a complex format
    if (strstr(tz, "EST")) return "EST";
    if (strstr(tz, "CST")) return "CST";
    if (strstr(tz, "MST")) return "MST";
    if (strstr(tz, "PST")) return "PST";
    
    // Return first part if it contains comma (POSIX format)
    char *tz_copy = strdup(tz);
    if (tz_copy) {
        char *comma = strchr(tz_copy, ',');
        if (comma) {
            *comma = '\0';
            const char *result = strdup(tz_copy);
            free(tz_copy);
            if (!result) return "UTC";
            return result;
        }
        free(tz_copy);
    }
    
    return tz;
}

// Check if timezone name needs to be freed (was dynamically allocated)
static bool should_free_timezone(const char *friendly_tz, const char *original_tz) {
    if (friendly_tz == original_tz) return false;
    
    // Check against static string literals
    const char *static_names[] = {
        "Local", "UTC", "Eastern", "Central", "Mountain", "Pacific",
        "Western Australia", "London", "Paris", "Tokyo", "Sydney", "EST", "CST", "MST", "PST"
    };
    
    for (int i = 0; i < sizeof(static_names) / sizeof(static_names[0]); i++) {
        if (friendly_tz == static_names[i]) {
            return false;
        }
    }
    
    return true;
}

static void digital_clock_cb(lv_timer_t *timer) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    
    // 12-hour format with AM/PM
    char buf[32];
    int hour_12 = timeinfo.tm_hour;
    const char *am_pm = "AM";
    
    if (hour_12 >= 12) {
        am_pm = "PM";
        if (hour_12 > 12) {
            hour_12 -= 12;
        }
    } else if (hour_12 == 0) {
        hour_12 = 12;
    }
    
    snprintf(buf, sizeof(buf), "%d:%02d:%02d %s", hour_12, timeinfo.tm_min, timeinfo.tm_sec, am_pm);
    lv_label_set_text(time_label, buf);
    
    char buf_date[32];
    strftime(buf_date, sizeof(buf_date), "%A, %B %d", &timeinfo);
    lv_label_set_text(date_label, buf_date);
    
    char buf_year[8];
    strftime(buf_year, sizeof(buf_year), "%Y", &timeinfo);
    lv_label_set_text(year_label, buf_year);
    
    // Update timezone label with friendly name
    const char *tz = settings_get_timezone_str(&G_Settings);
    const char *friendly_tz = get_friendly_timezone_name(tz);
    char tz_buf[32];
    snprintf(tz_buf, sizeof(tz_buf), "TZ: %s", friendly_tz ? friendly_tz : "Unknown");
    lv_label_set_text(tz_label, tz_buf);
    
    // Free memory if get_friendly_timezone_name allocated it
    if (should_free_timezone(friendly_tz, tz)) {
        free((void*)friendly_tz);
    }
}

static void clock_event_handler(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        display_manager_go_back();
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        display_manager_go_back();
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        display_manager_go_back();
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
        display_manager_go_back();
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
#endif
    }
}

static lv_obj_t *create_clock_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color, bool asset_bg) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    /* Keep labels readable over artwork using the pack/theme surface pair. */
    if (asset_bg) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_obj_set_style_bg_color(label, lv_color_hex(theme_palette_get_surface(theme)), 0);
        lv_obj_set_style_bg_opa(label, LV_OPA_80, 0);
        lv_obj_set_style_radius(label, 3, 0);
        lv_obj_set_style_pad_hor(label, 6, 0);
        lv_obj_set_style_pad_ver(label, 1, 0);
    }
    return label;
}

void clock_create(void) {
    // Apply user's timezone for localtime
    const char *tz = settings_get_timezone_str(&G_Settings);
    if (tz) {
        setenv("TZ", tz, 1);
        tzset();
    }
    
    // Asset-pack manifests can provide a text color suited to their artwork.
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    bool asset_bg = asset_pack_get_background_tile() != NULL;
    lv_color_t text_color = lv_color_hex(theme_palette_get_text(theme));

    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));
    display_manager_fill_screen(bg_color);
    clock_container = gui_screen_create_root(NULL, "Clock", bg_color, asset_bg ? LV_OPA_TRANSP : LV_OPA_COVER);
    clock_view.root = clock_container;
    lv_obj_t *content = gui_screen_create_content(clock_container, GUI_STATUS_BAR_HEIGHT);

    // Pick fonts and row gap based on usable content height so nothing clips
    int content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
    const lv_font_t *time_font;
    const lv_font_t *secondary_font;
    const lv_font_t *small_font;
    int row_gap;
    bool show_year_tz;

    if (content_h < 80) {
        time_font      = accessibility_get_font_display();
        secondary_font = accessibility_get_font_small();
        small_font     = accessibility_get_font_small();
        row_gap        = 2;
        show_year_tz   = false;
    } else if (content_h < 120) {
        time_font      = accessibility_get_font_display();
        secondary_font = accessibility_get_font_body();
        small_font     = accessibility_get_font_small();
        row_gap        = 4;
        show_year_tz   = true;
    } else {
        time_font      = accessibility_get_font_display();
        secondary_font = accessibility_get_font_body();
        small_font     = accessibility_get_font_body();
        row_gap        = 6;
        show_year_tz   = true;
    }

    // Flex column — labels stack vertically and the whole group stays centered
    lv_obj_t *label_stack = lv_obj_create(content);
    lv_obj_set_style_bg_opa(label_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(label_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label_stack, 0, 0);
    lv_obj_set_size(label_stack, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(label_stack, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_layout(label_stack, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(label_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(label_stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(label_stack, row_gap, 0);

    time_label = create_clock_label(label_stack, "12:00:00 AM", time_font, text_color, asset_bg);

    date_label = create_clock_label(label_stack, "Wednesday, January 01", secondary_font, text_color, asset_bg);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(date_label, LV_PCT(95));
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);

    year_label = create_clock_label(label_stack, "2025", secondary_font, text_color, asset_bg);
    if (!show_year_tz) lv_obj_add_flag(year_label, LV_OBJ_FLAG_HIDDEN);

    tz_label = create_clock_label(label_stack, "TZ: UTC", small_font, text_color, asset_bg);
    if (!show_year_tz) lv_obj_add_flag(tz_label, LV_OBJ_FLAG_HIDDEN);

    clock_timer = lv_timer_create(digital_clock_cb, 1000, NULL);
    digital_clock_cb(NULL);
}

void clock_destroy(void) {
    lvgl_timer_del_safe(&clock_timer);
    if (clock_container) {
        lv_obj_clean(clock_container);
        lvgl_obj_del_safe(&clock_container);
        clock_view.root = NULL;
    }
    time_label = NULL;
    date_label = NULL;
    year_label = NULL;
    tz_label = NULL;
}

void get_clock_callback(void **callback) {
    if (callback) *callback = (void *)clock_event_handler;
}

View clock_view = {
    .root = NULL,
    .create = clock_create,
    .destroy = clock_destroy,
    .input_callback = clock_event_handler,
    .name = "Clock",
    .get_hardwareinput_callback = get_clock_callback
};
