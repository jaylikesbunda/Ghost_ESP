#include "managers/views/setup_wizard_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/keyboard_screen.h"
#include "managers/settings_manager.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/accessibility_fonts.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "gui/lvgl_safe.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "gui/design_tokens.h"

static const char *TAG = "SetupWizard";

typedef enum {
    SETUP_STEP_WELCOME = 0,
    SETUP_STEP_AP_SSID,
    SETUP_STEP_AP_PASSWORD,
    SETUP_STEP_STA_SSID,
    SETUP_STEP_STA_PASSWORD,
    SETUP_STEP_COUNTRY,
    SETUP_STEP_TIMEZONE,
    SETUP_STEP_DISPLAY_TIMEOUT,
    SETUP_STEP_THEME,
    SETUP_STEP_MENU_LAYOUT,
    SETUP_STEP_TERMINAL_COLOR,
    SETUP_STEP_EPILEPSY_WARNING,
    SETUP_STEP_ACCESSIBILITY,
#ifdef CONFIG_WITH_STATUS_DISPLAY
    SETUP_STEP_IDLE_ANIMATION,
#endif
    SETUP_STEP_COMPLETE,
    SETUP_STEP_COUNT
} SetupStep;

typedef struct {
    const char *code;
    const char *name;
} WifiCountry;

static const WifiCountry wifi_countries[] = {
    {"US", "Americas"},
    {"GB", "Europe"},
    {"JP", "Japan"},
    {"AU", "Australia"},
    {"CN", "Asia"},
    {"01", "World Safe"},
};
#define COUNTRY_COUNT (sizeof(wifi_countries) / sizeof(wifi_countries[0]))

static lv_obj_t *root = NULL;
static SetupStep current_step = SETUP_STEP_WELCOME;
static int selected_country_index = 0;
static char temp_ap_ssid[33] = {0};
static char temp_ap_password[65] = {0};
static char temp_sta_ssid[65] = {0};
static char temp_sta_password[65] = {0};

static lv_obj_t *country_list = NULL;
static int country_cursor = 0;
static int welcome_btn_focus = 0; // 0=Start, 1=Skip
static lv_obj_t *welcome_start_btn = NULL;
static lv_obj_t *welcome_skip_btn = NULL;

static lv_obj_t *option_list = NULL;
static int option_cursor = 0;
static lv_obj_t *s_option_title_label = NULL;
static lv_obj_t *s_option_hint_label = NULL;
static bool wizard_exiting = false;
static bool touch_started = false;
static int touch_start_y = 0;
static int touch_start_x = 0;
static uint8_t temp_theme = 0;
static uint8_t temp_menu_layout = 0;
static uint8_t temp_terminal_color = 0;
static uint8_t temp_timezone = 0;
static uint8_t temp_display_timeout = 0;
static uint8_t temp_epilepsy_warning = 1;
static uint8_t temp_font_size = 1;
static uint8_t temp_reduced_motion = 0;
static uint8_t temp_high_contrast = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
static uint8_t temp_idle_animation = 0;
#endif

static uint8_t wizard_theme(void) {
    if (current_step == SETUP_STEP_THEME && option_list &&
        option_cursor >= 0 && option_cursor < THEME_PALETTE_THEME_COUNT) {
        return (uint8_t)option_cursor;
    }
    return theme_palette_clamp_id(temp_theme);
}

static lv_color_t wizard_color(uint32_t color) {
    return lv_color_hex(color);
}

/* Paints palette swatches directly onto the option button during DRAW_MAIN
 * instead of creating child widgets (~51 fewer objects per list build;
 * widget-based chips exhausted LVGL's heap on RAM-constrained boards). */
static void wizard_theme_swatch_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) return;
    lv_obj_t *btn = lv_event_get_target(e);
    int option_index = (int)(intptr_t)lv_event_get_user_data(e);
    if (!btn || !lv_obj_is_valid(btn)) return;
    if (option_index < 0 || option_index >= THEME_PALETTE_THEME_COUNT) return;
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) return;

    const theme_descriptor_t *theme = theme_palette_get_descriptor((uint8_t)option_index);
    const uint32_t colors[3] = {theme->background, theme->accent, theme->text};

    lv_area_t btn_area;
    lv_obj_get_coords(btn, &btn_area);
    lv_coord_t chip_w = 6;
    lv_coord_t chip_h = LV_MIN(lv_area_get_height(&btn_area) / 3, 12);
    if (chip_h < 5) return;
    lv_coord_t y1 = lv_area_get_height(&btn_area) / 2 + btn_area.y1 - chip_h / 2;
    lv_coord_t x2 = btn_area.x2 - 4;

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.border_width = 1;
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_color = lv_color_hex(theme->border);
    dsc.radius = 1;

    for (int i = 2; i >= 0; --i) {
        dsc.bg_color = lv_color_hex(colors[i]);
        lv_area_t chip = {(lv_coord_t)(x2 - chip_w), y1, x2, (lv_coord_t)(y1 + chip_h - 1)};
        lv_draw_rect(draw_ctx, &dsc, &chip);
        x2 -= chip_w;
    }
}

static const char *timezone_options[] = {
    "UTC", "EST5EDT", "CST6CDT", "MST7MDT", "PST8PDT",
    "GMT0", "CET-1CEST", "EET-2EEST", "IST-5:30", "JST-9",
    "AEST-10AEDT", "AWST-8", "NZST-12NZDT"
};
static const char *timezone_values[] = {
    "UTC0", "EST5EDT,M3.2.0,M11.1.0", "CST6CDT,M3.2.0,M11.1.0", "MST7MDT,M3.2.0,M11.1.0", "PST8PDT,M3.2.0,M11.1.0",
    "GMT0", "CET-1CEST,M3.5.0,M10.5.0", "EET-2EEST,M3.5.0,M10.5.0", "IST-5:30", "JST-9",
    "AEST-10AEDT,M10.1.0,M4.1.0", "AWST-8", "NZST-12NZDT,M9.5.0,M4.1.0"
};
#define TIMEZONE_COUNT 13

static const char *display_timeout_options[] = {"5s", "10s", "30s", "60s", "Never"};
static const uint32_t display_timeout_values[] = {5000, 10000, 30000, 60000, UINT32_MAX};
#define DISPLAY_TIMEOUT_COUNT 5

static const char *theme_options[THEME_PALETTE_THEME_COUNT];
#define THEME_COUNT THEME_PALETTE_THEME_COUNT

static const char *menu_layout_options[] = {"Carousel", "Grid", "List", "Compact", "Hero"};
#define MENU_LAYOUT_COUNT 5

static const char *terminal_color_options[] = {"Green", "White", "Red", "Blue", "Yellow", "Cyan", "Magenta", "Orange"};
static const uint32_t terminal_color_values[] = {0x00FF00, 0xFFFFFF, 0xFF0000, 0x0000FF, 0xFFFF00, 0x00FFFF, 0xFF00FF, 0xFFA500};
#define TERMINAL_COLOR_COUNT 8

#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_animation_options[] = {"Game of Life", "Ghost", "Starfield", "HUD", "Matrix", "Flying Ghosts", "Spiral", "Falling Leaves", "Bouncing Text"};
#define IDLE_ANIMATION_COUNT 9
#endif

static const char *epilepsy_warning_options[] = {"Off", "On"};
#define EPILEPSY_WARNING_COUNT 2

static const char *font_size_options[] = {"Small", "Normal", "Large"};
#define FONT_SIZE_COUNT 3

static const char *repeat_speed_options[] = {"Slow", "Normal", "Fast"};
#define REPEAT_SPEED_COUNT 3

static const char *accessibility_bool_options[] = {"Off", "On"};
#define ACCESSIBILITY_BOOL_COUNT 2

static void setup_wizard_create(void);
static void setup_wizard_destroy(void);
static void setup_wizard_input_callback(InputEvent *event);
static void show_welcome_screen(void);
static void show_country_screen(void);
static void show_complete_screen(void);
static void finish_setup(void);
static void skip_setup(void);

#define STATUS_BAR_H 18
#define USABLE_H (LV_VER_RES - STATUS_BAR_H)
#define USABLE_W LV_HOR_RES

static void style_wizard_btn(lv_obj_t *btn, lv_color_t bg, lv_coord_t radius) {
    lv_obj_set_style_bg_color(btn, bg, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, radius, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

static int get_card_width(int list_pad) {
    int item_w = USABLE_W - list_pad * 4 - 10;
    int max_w = USABLE_W * 86 / 100;
    if (item_w > max_w) item_w = max_w;
    if (item_w < 70) item_w = USABLE_W - list_pad * 2;
    return item_w;
}

static void ap_ssid_callback(const char *text) {
    if (text && strlen(text) > 0) {
        strncpy(temp_ap_ssid, text, sizeof(temp_ap_ssid) - 1);
        temp_ap_ssid[sizeof(temp_ap_ssid) - 1] = '\0';
    } else if (temp_ap_ssid[0] == '\0') {
        const char *def = settings_get_ap_ssid(&G_Settings);
        if (def && def[0]) {
            strncpy(temp_ap_ssid, def, sizeof(temp_ap_ssid) - 1);
            temp_ap_ssid[sizeof(temp_ap_ssid) - 1] = '\0';
        } else {
            strncpy(temp_ap_ssid, "GhostESP", sizeof(temp_ap_ssid) - 1);
        }
    }
    current_step = SETUP_STEP_AP_PASSWORD;
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&setup_wizard_view);
}

static void ap_password_callback(const char *text) {
    if (text && strlen(text) > 0) {
        strncpy(temp_ap_password, text, sizeof(temp_ap_password) - 1);
        temp_ap_password[sizeof(temp_ap_password) - 1] = '\0';
    } else if (temp_ap_password[0] == '\0') {
        const char *def = settings_get_ap_password(&G_Settings);
        if (def && def[0]) {
            strncpy(temp_ap_password, def, sizeof(temp_ap_password) - 1);
            temp_ap_password[sizeof(temp_ap_password) - 1] = '\0';
        } else {
            strncpy(temp_ap_password, "GhostESP", sizeof(temp_ap_password) - 1);
        }
    }
    current_step = SETUP_STEP_STA_SSID;
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&setup_wizard_view);
}

static void sta_ssid_callback(const char *text) {
    if (text && strlen(text) > 0) {
        strncpy(temp_sta_ssid, text, sizeof(temp_sta_ssid) - 1);
        temp_sta_ssid[sizeof(temp_sta_ssid) - 1] = '\0';
        current_step = SETUP_STEP_STA_PASSWORD;
    } else {
        temp_sta_ssid[0] = '\0';
        temp_sta_password[0] = '\0';
        current_step = SETUP_STEP_COUNTRY;
    }
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&setup_wizard_view);
}

static void sta_password_callback(const char *text) {
    if (text && strlen(text) > 0) {
        strncpy(temp_sta_password, text, sizeof(temp_sta_password) - 1);
        temp_sta_password[sizeof(temp_sta_password) - 1] = '\0';
    } else {
        temp_sta_password[0] = '\0';
    }
    current_step = SETUP_STEP_COUNTRY;
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&setup_wizard_view);
}

static void start_btn_event_cb(lv_event_t *e) {
    (void)e;
    current_step = SETUP_STEP_AP_SSID;
    const char *cur_ssid = settings_get_ap_ssid(&G_Settings);
    if (cur_ssid && cur_ssid[0]) {
        strncpy(temp_ap_ssid, cur_ssid, sizeof(temp_ap_ssid) - 1);
        temp_ap_ssid[sizeof(temp_ap_ssid) - 1] = '\0';
    } else {
        temp_ap_ssid[0] = '\0';
    }
    keyboard_view_set_placeholder("Device AP SSID");
    keyboard_view_set_initial_text(temp_ap_ssid);
    keyboard_view_set_submit_callback(ap_ssid_callback);
    keyboard_view_set_return_view(&setup_wizard_view);
    display_manager_switch_view(&keyboard_view);
}

static void skip_btn_event_cb(lv_event_t *e) {
    (void)e;
    skip_setup();
}

static void update_welcome_btn_focus(void) {
    if (welcome_start_btn && welcome_skip_btn) {
        lv_obj_t *start_label = lv_obj_get_child(welcome_start_btn, 0);
        lv_obj_t *skip_label = lv_obj_get_child(welcome_skip_btn, 0);
        uint8_t theme = wizard_theme();
        lv_color_t accent = wizard_color(theme_palette_get_accent(theme));
        lv_color_t on_accent = wizard_color(theme_palette_get_on_accent(theme));
        lv_color_t surface = wizard_color(theme_palette_get_surface_alt(theme));
        lv_color_t text = wizard_color(theme_palette_get_text(theme));
        if (welcome_btn_focus == 0) {
            lv_obj_set_style_bg_color(welcome_start_btn, accent, 0);
            if (start_label) lv_obj_set_style_text_color(start_label, on_accent, 0);
            lv_obj_set_style_bg_color(welcome_skip_btn, surface, 0);
            if (skip_label) lv_obj_set_style_text_color(skip_label, text, 0);
        } else {
            lv_obj_set_style_bg_color(welcome_start_btn, surface, 0);
            if (start_label) lv_obj_set_style_text_color(start_label, text, 0);
            lv_obj_set_style_bg_color(welcome_skip_btn, accent, 0);
            if (skip_label) lv_obj_set_style_text_color(skip_label, on_accent, 0);
        }
    }
}

static const lv_font_t *get_title_font(void) {
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (LV_VER_RES <= 100) return fs == 0 ? &lv_font_montserrat_8 : (fs == 1 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
    if (LV_VER_RES <= 160) return fs == 0 ? &lv_font_montserrat_10 : (fs == 1 ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
    if (LV_VER_RES <= 280) return fs == 0 ? &lv_font_montserrat_12 : (fs == 1 ? &lv_font_montserrat_14 : &lv_font_montserrat_16);
    return fs == 0 ? &lv_font_montserrat_14 : (fs == 1 ? &lv_font_montserrat_16 : &lv_font_montserrat_18);
}

static const lv_font_t *get_body_font(void) {
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (LV_VER_RES <= 100) return fs == 0 ? &lv_font_montserrat_8 : (fs == 1 ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
    if (LV_VER_RES <= 200) return fs == 0 ? &lv_font_montserrat_8 : (fs == 1 ? &lv_font_montserrat_10 : &lv_font_montserrat_14);
    return fs == 0 ? &lv_font_montserrat_10 : (fs == 1 ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
}

static void show_welcome_screen(void) {
    welcome_btn_focus = 0;
    
    const lv_font_t *title_font = get_title_font();
    const lv_font_t *body_font = get_body_font();
    
    int title_y = STATUS_BAR_H + (USABLE_H * 8 / 100);
    int desc_y = STATUS_BAR_H + (USABLE_H * 30 / 100);
    int btn_y = LV_VER_RES - (USABLE_H * 18 / 100);
    
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Welcome to GhostESP!");
    lv_obj_set_style_text_color(title, wizard_color(theme_palette_get_text(wizard_theme())), 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);

    lv_obj_t *desc = lv_label_create(root);
    lv_label_set_text(desc, 
        "In this setup you can:\n"
        "Configure device AP credentials\n"
        "Save your home WiFi\n"
        "Set your region\n"
        "Customize device appearance");
    lv_obj_set_style_text_color(desc, wizard_color(theme_palette_get_text_muted(wizard_theme())), 0);
    lv_obj_set_style_text_font(desc, body_font, 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(desc, USABLE_W - 20);
    lv_obj_align(desc, LV_ALIGN_TOP_MID, 0, desc_y);

    int btn_w = USABLE_W * 22 / 100;
    if (btn_w < 40) btn_w = 40;
    if (btn_w > 80) btn_w = 80;
    int btn_h = USABLE_H * 10 / 100;
    if (btn_h < 20) btn_h = 20;
    if (btn_h > 35) btn_h = 35;
    int btn_gap = USABLE_W * 2 / 100;
    if (btn_gap < 2) btn_gap = 2;
    int total_btn_width = btn_w * 2 + btn_gap;
    int btn_offset = (total_btn_width > USABLE_W - 20) ? (USABLE_W - 20 - total_btn_width) / 2 : (btn_w / 2 + btn_gap);

    welcome_start_btn = lv_btn_create(root);
    gui_apply_pressed_style(welcome_start_btn);
    lv_obj_set_size(welcome_start_btn, btn_w, btn_h);
    lv_obj_align(welcome_start_btn, LV_ALIGN_TOP_MID, -btn_offset, btn_y);
    style_wizard_btn(welcome_start_btn, wizard_color(theme_palette_get_surface_alt(wizard_theme())), 5);
    lv_obj_t *start_label = lv_label_create(welcome_start_btn);
    lv_label_set_text(start_label, "Start");
    lv_obj_center(start_label);
    lv_obj_add_event_cb(welcome_start_btn, start_btn_event_cb, LV_EVENT_CLICKED, NULL);

    welcome_skip_btn = lv_btn_create(root);
    gui_apply_pressed_style(welcome_skip_btn);
    lv_obj_set_size(welcome_skip_btn, btn_w, btn_h);
    lv_obj_align(welcome_skip_btn, LV_ALIGN_TOP_MID, btn_offset, btn_y);
    style_wizard_btn(welcome_skip_btn, wizard_color(theme_palette_get_surface_alt(wizard_theme())), 5);
    lv_obj_t *skip_label = lv_label_create(welcome_skip_btn);
    lv_label_set_text(skip_label, "Skip");
    lv_obj_center(skip_label);
    lv_obj_add_event_cb(welcome_skip_btn, skip_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    update_welcome_btn_focus();
}

static void country_btn_event_cb(lv_event_t *e) {
    intptr_t index = (intptr_t)lv_event_get_user_data(e);
    selected_country_index = (int)index;
    current_step = SETUP_STEP_TIMEZONE;
    display_manager_switch_view(&setup_wizard_view);
}

static void timezone_btn_event_cb(lv_event_t *e) {
    (void)e;
    current_step = SETUP_STEP_DISPLAY_TIMEOUT;
    display_manager_switch_view(&setup_wizard_view);
}

static void update_option_selection(int count) {
    if (!option_list) return;
    uint8_t active_theme = wizard_theme();
    if (current_step == SETUP_STEP_THEME) {
        uint8_t preview = theme_palette_clamp_id((uint8_t)option_cursor);
        lv_color_t preview_bg = wizard_color(theme_palette_get_background(preview));
        lv_obj_set_style_bg_color(root, preview_bg, 0);
        gui_screen_apply_theme_background_for(root, preview);
        display_manager_update_status_bar_color();
        if (s_option_title_label && lv_obj_is_valid(s_option_title_label)) {
            lv_obj_set_style_text_color(s_option_title_label,
                                        wizard_color(theme_palette_get_text(preview)), 0);
        }
        if (s_option_hint_label && lv_obj_is_valid(s_option_hint_label)) {
            lv_obj_set_style_text_color(s_option_hint_label,
                                        wizard_color(theme_palette_get_text_muted(preview)), 0);
        }
    }
    uint32_t child_count = lv_obj_get_child_cnt(option_list);
    for (uint32_t i = 0; i < child_count && (int)i < count; i++) {
        lv_obj_t *btn = lv_obj_get_child(option_list, i);
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        uint8_t row_theme = current_step == SETUP_STEP_THEME ? (uint8_t)i : active_theme;
        if ((int)i == option_cursor) {
            lv_obj_set_style_bg_color(btn, wizard_color(theme_palette_get_accent(row_theme)), 0);
            if (label) lv_obj_set_style_text_color(label, wizard_color(theme_palette_get_on_accent(row_theme)), 0);
        } else {
            lv_obj_set_style_bg_color(btn, wizard_color(theme_palette_get_surface(row_theme)), 0);
            if (label) lv_obj_set_style_text_color(label, wizard_color(theme_palette_get_text(row_theme)), 0);
        }
    }
    lv_obj_t *selected_btn = lv_obj_get_child(option_list, option_cursor);
    if (selected_btn) {
        lv_obj_scroll_to_view(selected_btn, LV_ANIM_OFF);
    }
}

static void show_option_screen(const char *title_text, const char **options, int count, int current_value) {
    const lv_font_t *title_font = get_title_font();
    const lv_font_t *body_font = get_body_font();
    
    int title_y = STATUS_BAR_H + (USABLE_H * 3 / 100);
    int list_top = STATUS_BAR_H + (USABLE_H * 15 / 100);
    int list_height = USABLE_H * 70 / 100;
    int btn_h = USABLE_H * 12 / 100;
    if (btn_h < 22) btn_h = 22;
    if (btn_h > 36) btn_h = 36;
    int hint_bottom = USABLE_H * 3 / 100;
    if (hint_bottom < 3) hint_bottom = 3;
    
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, title_text);
    s_option_title_label = title;
    lv_obj_set_style_text_color(title, wizard_color(theme_palette_get_text(wizard_theme())), 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);

    int list_pad = USABLE_W * 2 / 100;
    if (list_pad < 3) list_pad = 3;
    option_list = lv_obj_create(root);
    lv_obj_set_size(option_list, USABLE_W - list_pad * 2, list_height);
    lv_obj_align(option_list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_style_bg_opa(option_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(option_list, 0, 0);
    lv_obj_set_style_shadow_width(option_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(option_list, list_pad, 0);
    lv_obj_set_flex_flow(option_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(option_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int item_w = get_card_width(list_pad);
    
    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = lv_btn_create(option_list);
        gui_apply_pressed_style(btn);
        lv_obj_set_size(btn, item_w, btn_h);
        uint8_t row_theme = current_step == SETUP_STEP_THEME ? (uint8_t)i : wizard_theme();
        style_wizard_btn(btn, wizard_color(theme_palette_get_surface(row_theme)), 4);
        
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, options[i]);
        lv_obj_set_style_text_font(label, body_font, 0);
        lv_obj_center(label);

        if (current_step == SETUP_STEP_THEME) {
            lv_obj_add_event_cb(btn, wizard_theme_swatch_draw_cb, LV_EVENT_DRAW_MAIN,
                                (void *)(intptr_t)i);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 4, 0);
        }
    }

    option_cursor = current_value;
    if (option_cursor >= count) option_cursor = 0;
    update_option_selection(count);

    lv_obj_t *hint = lv_label_create(root);
    s_option_hint_label = hint;
#ifdef CONFIG_USE_TOUCHSCREEN
    lv_label_set_text(hint, "Tap to select");
#else
    lv_label_set_text(hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " OK");
#endif
    lv_obj_set_style_text_color(hint, wizard_color(theme_palette_get_text_muted(wizard_theme())), 0);
    lv_obj_set_style_text_font(hint, body_font, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + hint_bottom));
}

static int prev_country_cursor = -1;
static void update_country_selection(void) {
    if (!country_list) return;
    
    if (prev_country_cursor >= 0 && prev_country_cursor != country_cursor) {
        lv_obj_t *old_btn = lv_obj_get_child(country_list, prev_country_cursor);
        if (old_btn) {
            lv_obj_set_style_bg_color(old_btn, wizard_color(theme_palette_get_surface(wizard_theme())), 0);
            lv_obj_t *old_label = lv_obj_get_child(old_btn, 0);
            if (old_label) lv_obj_set_style_text_color(old_label, wizard_color(theme_palette_get_text(wizard_theme())), 0);
        }
    }
    
    lv_obj_t *new_btn = lv_obj_get_child(country_list, country_cursor);
    if (new_btn) {
        lv_obj_set_style_bg_color(new_btn, wizard_color(theme_palette_get_accent(wizard_theme())), 0);
        lv_obj_t *new_label = lv_obj_get_child(new_btn, 0);
        if (new_label) lv_obj_set_style_text_color(new_label, wizard_color(theme_palette_get_on_accent(wizard_theme())), 0);
        lv_obj_scroll_to_view(new_btn, LV_ANIM_OFF);
    }
    
    prev_country_cursor = country_cursor;
}

static void show_country_screen(void) {
    const lv_font_t *title_font = get_title_font();
    const lv_font_t *body_font = get_body_font();
    
    int title_y = STATUS_BAR_H + (USABLE_H * 3 / 100);
    int list_top = STATUS_BAR_H + (USABLE_H * 15 / 100);
    int list_height = USABLE_H * 70 / 100;
    int btn_h = USABLE_H * 12 / 100;
    if (btn_h < 22) btn_h = 22;
    if (btn_h > 36) btn_h = 36;
    int hint_bottom = USABLE_H * 3 / 100;
    if (hint_bottom < 3) hint_bottom = 3;
    
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, "Select Region");
    lv_obj_set_style_text_color(title, wizard_color(theme_palette_get_text(wizard_theme())), 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);

    int list_pad = USABLE_W * 2 / 100;
    if (list_pad < 3) list_pad = 3;
    country_list = lv_obj_create(root);
    lv_obj_set_size(country_list, USABLE_W - list_pad * 2, list_height);
    lv_obj_align(country_list, LV_ALIGN_TOP_MID, 0, list_top);
    lv_obj_set_style_bg_opa(country_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(country_list, 0, 0);
    lv_obj_set_style_shadow_width(country_list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(country_list, list_pad, 0);
    lv_obj_set_flex_flow(country_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(country_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int item_w = get_card_width(list_pad);
    
    for (int i = 0; i < (int)COUNTRY_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(country_list);
        gui_apply_pressed_style(btn);
        lv_obj_set_size(btn, item_w, btn_h);
        style_wizard_btn(btn, wizard_color(theme_palette_get_surface(wizard_theme())), 4);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, wifi_countries[i].name);
        lv_obj_set_style_text_font(label, body_font, 0);
        lv_obj_center(label);

        lv_obj_add_event_cb(btn, country_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    country_cursor = selected_country_index;
    prev_country_cursor = -1;
    update_country_selection();

    lv_obj_t *hint = lv_label_create(root);
#ifdef CONFIG_USE_TOUCHSCREEN
    lv_label_set_text(hint, "Tap to select");
#else
    lv_label_set_text(hint, LV_SYMBOL_UP LV_SYMBOL_DOWN " OK");
#endif
    lv_obj_set_style_text_color(hint, wizard_color(theme_palette_get_text_muted(wizard_theme())), 0);
    lv_obj_set_style_text_font(hint, body_font, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + hint_bottom));
}

static void finish_btn_event_cb(lv_event_t *e) {
    (void)e;
    finish_setup();
}

static void show_complete_screen(void) {
    const lv_font_t *title_font = get_title_font();
    const lv_font_t *body_font = get_body_font();
    
    int title_y = STATUS_BAR_H + (USABLE_H * 5 / 100);
    int info_y = STATUS_BAR_H + (USABLE_H * 20 / 100);
    int btn_bottom = USABLE_H * 5 / 100;
    
    lv_obj_t *title = lv_label_create(root);
    lv_label_set_text(title, LV_SYMBOL_OK " Done!");
    lv_obj_set_style_text_color(title, wizard_color(theme_palette_get_text(wizard_theme())), 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, title_y);

    char summary[320];
#ifdef CONFIG_WITH_STATUS_DISPLAY
    snprintf(summary, sizeof(summary),
             "Device AP: %s\n"
             "Home WiFi: %s | Region: %s\n"
             "Theme: %s | Menu: %s\n"
             "Terminal: %s\n"
             "Animation: %s\n"
             "Epilepsy Warn: %s",
             temp_ap_ssid[0] ? temp_ap_ssid : "(default)",
             temp_sta_ssid[0] ? temp_sta_ssid : "Skipped",
             wifi_countries[selected_country_index].name,
             theme_options[temp_theme],
             menu_layout_options[temp_menu_layout],
             terminal_color_options[temp_terminal_color],
             idle_animation_options[temp_idle_animation],
             temp_epilepsy_warning ? "On" : "Off");
#else
    snprintf(summary, sizeof(summary),
             "Device AP: %s\n"
             "Home WiFi: %s | Region: %s\n"
             "Theme: %s | Menu: %s\n"
             "Terminal: %s\n"
             "Epilepsy Warn: %s",
             temp_ap_ssid[0] ? temp_ap_ssid : "(default)",
             temp_sta_ssid[0] ? temp_sta_ssid : "Skipped",
             wifi_countries[selected_country_index].name,
             theme_options[temp_theme],
             menu_layout_options[temp_menu_layout],
             terminal_color_options[temp_terminal_color],
             temp_epilepsy_warning ? "On" : "Off");
#endif

    lv_obj_t *info = lv_label_create(root);
    lv_label_set_text(info, summary);
    lv_obj_set_style_text_color(info, wizard_color(theme_palette_get_text_muted(wizard_theme())), 0);
    lv_obj_set_style_text_font(info, body_font, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(info, USABLE_W - 10);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, info_y);

    int btn_w = USABLE_W * 30 / 100;
    if (btn_w < 50) btn_w = 50;
    if (btn_w > 100) btn_w = 100;
    int btn_h = USABLE_H * 10 / 100;
    if (btn_h < 20) btn_h = 20;
    if (btn_h > 35) btn_h = 35;
    
    lv_obj_t *finish_btn = lv_btn_create(root);
    gui_apply_pressed_style(finish_btn);
    lv_obj_set_size(finish_btn, btn_w, btn_h);
    lv_obj_align(finish_btn, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + btn_bottom));
    style_wizard_btn(finish_btn, wizard_color(theme_palette_get_accent(wizard_theme())), 5);
    lv_obj_t *finish_label = lv_label_create(finish_btn);
    lv_label_set_text(finish_label, "Finish");
    lv_obj_set_style_text_color(finish_label, wizard_color(theme_palette_get_on_accent(wizard_theme())), 0);
    lv_obj_center(finish_label);
    lv_obj_add_event_cb(finish_btn, finish_btn_event_cb, LV_EVENT_CLICKED, NULL);
}

static void apply_wifi_country(int country_index) {
    const WifiCountry *country = &wifi_countries[country_index];
    ESP_LOGI(TAG, "Applying WiFi country: %s (%s)", country->code, country->name);

    esp_err_t err = esp_wifi_set_country_code(country->code, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set country code: %s", esp_err_to_name(err));
    }
}

static void finish_setup(void) {
    if (wizard_exiting) return;
    wizard_exiting = true;
    
    ESP_LOGI(TAG, "Finishing setup wizard");
    
    if (temp_ap_ssid[0]) {
        settings_set_ap_ssid(&G_Settings, temp_ap_ssid);
    }
    if (temp_ap_password[0]) {
        settings_set_ap_password(&G_Settings, temp_ap_password);
    }
    if (temp_sta_ssid[0]) {
        settings_set_sta_ssid(&G_Settings, temp_sta_ssid);
        settings_set_sta_password(&G_Settings, temp_sta_password);
    }
    settings_set_wifi_country(&G_Settings, (uint8_t)selected_country_index);
    apply_wifi_country(selected_country_index);
    settings_set_timezone_str(&G_Settings, timezone_values[temp_timezone]);
    // Apply timezone immediately
    setenv("TZ", timezone_values[temp_timezone], 1);
    tzset();
    settings_set_display_timeout(&G_Settings, display_timeout_values[temp_display_timeout]);
    settings_set_menu_theme(&G_Settings, temp_theme);
    settings_set_menu_layout(&G_Settings, temp_menu_layout);
    settings_set_terminal_text_color(&G_Settings, terminal_color_values[temp_terminal_color]);
    settings_set_epilepsy_warning_enabled(&G_Settings, temp_epilepsy_warning == 1);
    settings_set_font_size(&G_Settings, temp_font_size);
    settings_set_high_contrast(&G_Settings, temp_high_contrast == 1);
    settings_set_reduced_motion(&G_Settings, temp_reduced_motion == 1);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    settings_set_status_idle_animation(&G_Settings, (IdleAnimation)temp_idle_animation);
#endif
    settings_set_setup_complete(&G_Settings, true);
    settings_save(&G_Settings);
    
    ESP_LOGI(TAG, "Settings saved, switching to main menu");
    display_manager_switch_view(&main_menu_view);
}

static void skip_setup(void) {
    if (wizard_exiting) return;
    wizard_exiting = true;
    
    ESP_LOGI(TAG, "Skipping setup wizard");
    settings_set_setup_complete(&G_Settings, true);
    settings_set_menu_layout(&G_Settings, 2);
    settings_save(&G_Settings);
    display_manager_switch_view(&main_menu_view);
}

static void setup_wizard_create(void) {
    ESP_LOGI(TAG, "Creating setup wizard, step=%d", current_step);
    for (uint8_t theme = 0; theme < THEME_PALETTE_THEME_COUNT; ++theme) {
        theme_options[theme] = theme_palette_get_name(theme);
    }
    
    uint8_t theme = wizard_theme();
    lv_color_t background = wizard_color(theme_palette_get_background(theme));
    display_manager_fill_screen(background);

    root = gui_screen_create_root(NULL, "Setup", background, LV_OPA_COVER);
    setup_wizard_view.root = root;

    switch (current_step) {
        case SETUP_STEP_WELCOME:
            show_welcome_screen();
            break;
        case SETUP_STEP_AP_SSID: {
            if (temp_ap_ssid[0] == '\0') {
                const char *cur_ssid = settings_get_ap_ssid(&G_Settings);
                if (cur_ssid && cur_ssid[0]) {
                    strncpy(temp_ap_ssid, cur_ssid, sizeof(temp_ap_ssid) - 1);
                    temp_ap_ssid[sizeof(temp_ap_ssid) - 1] = '\0';
                }
            }
            keyboard_view_set_placeholder("Device AP SSID");
            keyboard_view_set_initial_text(temp_ap_ssid);
            keyboard_view_set_submit_callback(ap_ssid_callback);
            keyboard_view_set_return_view(&setup_wizard_view);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETUP_STEP_AP_PASSWORD: {
            if (temp_ap_password[0] == '\0') {
                const char *cur_pass = settings_get_ap_password(&G_Settings);
                if (cur_pass && cur_pass[0]) {
                    strncpy(temp_ap_password, cur_pass, sizeof(temp_ap_password) - 1);
                    temp_ap_password[sizeof(temp_ap_password) - 1] = '\0';
                }
            }
            keyboard_view_set_start_caps(false);
            keyboard_view_set_placeholder("Device AP Password");
            keyboard_view_set_initial_text(temp_ap_password);
            keyboard_view_set_submit_callback(ap_password_callback);
            keyboard_view_set_return_view(&setup_wizard_view);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETUP_STEP_STA_SSID: {
            if (temp_sta_ssid[0] == '\0') {
                const char *cur_ssid = settings_get_sta_ssid(&G_Settings);
                if (cur_ssid && cur_ssid[0]) {
                    strncpy(temp_sta_ssid, cur_ssid, sizeof(temp_sta_ssid) - 1);
                    temp_sta_ssid[sizeof(temp_sta_ssid) - 1] = '\0';
                }
            }
            keyboard_view_set_placeholder("Home WiFi SSID (blank skips)");
            keyboard_view_set_initial_text(temp_sta_ssid);
            keyboard_view_set_submit_callback(sta_ssid_callback);
            keyboard_view_set_return_view(&setup_wizard_view);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETUP_STEP_STA_PASSWORD: {
            if (temp_sta_password[0] == '\0') {
                const char *cur_pass = settings_get_sta_password(&G_Settings);
                if (cur_pass && cur_pass[0]) {
                    strncpy(temp_sta_password, cur_pass, sizeof(temp_sta_password) - 1);
                    temp_sta_password[sizeof(temp_sta_password) - 1] = '\0';
                }
            }
            keyboard_view_set_start_caps(false);
            keyboard_view_set_placeholder("Home WiFi Password");
            keyboard_view_set_initial_text(temp_sta_password);
            keyboard_view_set_submit_callback(sta_password_callback);
            keyboard_view_set_return_view(&setup_wizard_view);
            display_manager_switch_view(&keyboard_view);
            return;
        }
        case SETUP_STEP_COUNTRY:
            show_country_screen();
            break;
        case SETUP_STEP_TIMEZONE: {
            const char *cur_tz = settings_get_timezone_str(&G_Settings);
            temp_timezone = 0;
            for (int i = 0; i < TIMEZONE_COUNT; i++) {
                if (strcmp(timezone_values[i], cur_tz) == 0) {
                    temp_timezone = i;
                    break;
                }
            }
            show_option_screen("Select Timezone", timezone_options, TIMEZONE_COUNT, temp_timezone);
            break;
        }
        case SETUP_STEP_DISPLAY_TIMEOUT: {
            uint32_t cur_timeout = settings_get_display_timeout(&G_Settings);
            temp_display_timeout = 0;
            for (int i = 0; i < DISPLAY_TIMEOUT_COUNT; i++) {
                if (display_timeout_values[i] == cur_timeout) {
                    temp_display_timeout = i;
                    break;
                }
            }
            show_option_screen("Screen Timeout", display_timeout_options, DISPLAY_TIMEOUT_COUNT, temp_display_timeout);
            break;
        }
        case SETUP_STEP_THEME:
            temp_theme = settings_get_menu_theme(&G_Settings);
            show_option_screen("Select Theme", theme_options, THEME_COUNT, temp_theme);
            break;
        case SETUP_STEP_MENU_LAYOUT:
            temp_menu_layout = settings_get_menu_layout(&G_Settings);
            show_option_screen("Menu Layout", menu_layout_options, MENU_LAYOUT_COUNT, temp_menu_layout);
            break;
        case SETUP_STEP_TERMINAL_COLOR: {
            uint32_t cur_color = settings_get_terminal_text_color(&G_Settings);
            temp_terminal_color = 0;
            for (int i = 0; i < TERMINAL_COLOR_COUNT; i++) {
                if (terminal_color_values[i] == cur_color) {
                    temp_terminal_color = i;
                    break;
                }
            }
            show_option_screen("Terminal Color", terminal_color_options, TERMINAL_COLOR_COUNT, temp_terminal_color);
            break;
        }
        case SETUP_STEP_EPILEPSY_WARNING:
            temp_epilepsy_warning = settings_get_epilepsy_warning_enabled(&G_Settings) ? 1 : 0;
            show_option_screen("Epilepsy Warning", epilepsy_warning_options, EPILEPSY_WARNING_COUNT, temp_epilepsy_warning);
            break;
        case SETUP_STEP_ACCESSIBILITY:
            temp_font_size = settings_get_font_size(&G_Settings);
            temp_high_contrast = settings_get_high_contrast(&G_Settings);
            temp_reduced_motion = settings_get_reduced_motion(&G_Settings);
            show_option_screen("Text & Reading", font_size_options, FONT_SIZE_COUNT, temp_font_size);
            break;
#ifdef CONFIG_WITH_STATUS_DISPLAY
        case SETUP_STEP_IDLE_ANIMATION:
            temp_idle_animation = (uint8_t)settings_get_status_idle_animation(&G_Settings);
            show_option_screen("Idle Animation", idle_animation_options, IDLE_ANIMATION_COUNT, temp_idle_animation);
            break;
#endif
        case SETUP_STEP_COMPLETE:
            show_complete_screen();
            break;
        default:
            show_welcome_screen();
            break;
    }
}

static void setup_wizard_destroy(void) {
    lvgl_obj_del_safe(&root);
    setup_wizard_view.root = NULL;
    country_list = NULL;
    option_list = NULL;
    welcome_start_btn = NULL;
    welcome_skip_btn = NULL;
}

static void setup_wizard_input_callback(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_EXIT_BUTTON && event->data.exit_pressed) {
        skip_setup();
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            touch_started = true;
            touch_start_y = data->point.y;
            touch_start_x = data->point.x;
            return;
        }
        if (data->state == LV_INDEV_STATE_REL && touch_started) {
            touch_started = false;
            int dy = data->point.y - touch_start_y;
            int dx = data->point.x - touch_start_x;
            
            if (abs(dy) > 20) {
                if (current_step == SETUP_STEP_COUNTRY && country_list) {
                    lv_obj_scroll_by_bounded(country_list, 0, dy, LV_ANIM_OFF);
                } else if (option_list) {
                    lv_obj_scroll_by_bounded(option_list, 0, dy, LV_ANIM_OFF);
                }
                return;
            }
            
            if (abs(dx) < 20 && abs(dy) < 20) {
                if (current_step == SETUP_STEP_WELCOME && welcome_start_btn && welcome_skip_btn) {
                    lv_area_t start_area, skip_area;
                    lv_obj_get_coords(welcome_start_btn, &start_area);
                    lv_obj_get_coords(welcome_skip_btn, &skip_area);
                    
                    if (data->point.x >= start_area.x1 && data->point.x <= start_area.x2 &&
                        data->point.y >= start_area.y1 && data->point.y <= start_area.y2) {
                        start_btn_event_cb(NULL);
                        return;
                    }
                    if (data->point.x >= skip_area.x1 && data->point.x <= skip_area.x2 &&
                        data->point.y >= skip_area.y1 && data->point.y <= skip_area.y2) {
                        skip_setup();
                        return;
                    }
                } else if (current_step == SETUP_STEP_COMPLETE) {
                    // Find the finish button by looking for any button with "Finish" text
                    lv_obj_t *root = setup_wizard_view.root;
                    if (root) {
                        uint32_t child_count = lv_obj_get_child_cnt(root);
                        for (uint32_t i = 0; i < child_count; i++) {
                            lv_obj_t *child = lv_obj_get_child(root, i);
                            if (child && lv_obj_check_type(child, &lv_btn_class)) {
                                lv_obj_t *label = lv_obj_get_child(child, 0);
                                if (label) {
                                    const char *text = lv_label_get_text(label);
                                    if (text && strcmp(text, "Finish") == 0) {
                                        lv_area_t btn_area;
                                        lv_obj_get_coords(child, &btn_area);
                                        if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                            data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                            finish_setup();
                                            return;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else if (current_step == SETUP_STEP_COUNTRY && country_list) {
                    uint32_t child_count = lv_obj_get_child_cnt(country_list);
                    for (uint32_t i = 0; i < child_count; i++) {
                        lv_obj_t *btn = lv_obj_get_child(country_list, i);
                        if (btn) {
                            lv_area_t btn_area;
                            lv_obj_get_coords(btn, &btn_area);
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                country_cursor = (int)i;
                                selected_country_index = country_cursor;
                                current_step = SETUP_STEP_TIMEZONE;
                                display_manager_switch_view(&setup_wizard_view);
                                return;
                            }
                        }
                    }
                } else if (option_list) {
                    uint32_t child_count = lv_obj_get_child_cnt(option_list);
                    for (uint32_t i = 0; i < child_count; i++) {
                        lv_obj_t *btn = lv_obj_get_child(option_list, i);
                        if (btn) {
                            lv_area_t btn_area;
                            lv_obj_get_coords(btn, &btn_area);
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                option_cursor = (int)i;
                                update_option_selection(child_count);
                                
                                if (current_step == SETUP_STEP_TIMEZONE) temp_timezone = option_cursor;
                                else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) temp_display_timeout = option_cursor;
                                else if (current_step == SETUP_STEP_THEME) temp_theme = option_cursor;
                                else if (current_step == SETUP_STEP_MENU_LAYOUT) temp_menu_layout = option_cursor;
                                else if (current_step == SETUP_STEP_TERMINAL_COLOR) temp_terminal_color = option_cursor;
                                else if (current_step == SETUP_STEP_EPILEPSY_WARNING) temp_epilepsy_warning = option_cursor;
                                else if (current_step == SETUP_STEP_ACCESSIBILITY) temp_font_size = option_cursor;
#ifdef CONFIG_WITH_STATUS_DISPLAY
                                else if (current_step == SETUP_STEP_IDLE_ANIMATION) temp_idle_animation = option_cursor;
#endif
                                
                                SetupStep next_step = SETUP_STEP_COMPLETE;
                                SetupStep prev_step = SETUP_STEP_COUNTRY;
                                
                                if (current_step == SETUP_STEP_TIMEZONE) {
                                    next_step = SETUP_STEP_DISPLAY_TIMEOUT;
                                    prev_step = SETUP_STEP_COUNTRY;
                                } else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) {
                                    next_step = SETUP_STEP_THEME;
                                    prev_step = SETUP_STEP_TIMEZONE;
                                } else if (current_step == SETUP_STEP_THEME) {
                                    next_step = SETUP_STEP_MENU_LAYOUT;
                                    prev_step = SETUP_STEP_DISPLAY_TIMEOUT;
                                } else if (current_step == SETUP_STEP_MENU_LAYOUT) {
                                    next_step = SETUP_STEP_TERMINAL_COLOR;
                                    prev_step = SETUP_STEP_THEME;
                                } else if (current_step == SETUP_STEP_TERMINAL_COLOR) {
                                    next_step = SETUP_STEP_EPILEPSY_WARNING;
                                    prev_step = SETUP_STEP_MENU_LAYOUT;
                                } else if (current_step == SETUP_STEP_EPILEPSY_WARNING) {
                                    next_step = SETUP_STEP_ACCESSIBILITY;
                                    prev_step = SETUP_STEP_TERMINAL_COLOR;
                                } else if (current_step == SETUP_STEP_ACCESSIBILITY) {
#ifdef CONFIG_WITH_STATUS_DISPLAY
                                    next_step = SETUP_STEP_IDLE_ANIMATION;
#else
                                    next_step = SETUP_STEP_COMPLETE;
#endif
                                    prev_step = SETUP_STEP_EPILEPSY_WARNING;
#ifdef CONFIG_WITH_STATUS_DISPLAY
                                } else if (current_step == SETUP_STEP_IDLE_ANIMATION) {
                                    next_step = SETUP_STEP_COMPLETE;
                                    prev_step = SETUP_STEP_ACCESSIBILITY;
#endif
                                }
                                
                                current_step = next_step;
                                display_manager_switch_view(&setup_wizard_view);
                                return;
                            }
                        }
                    }
                }
            }
            return;
        }
    }

    if (current_step == SETUP_STEP_WELCOME) {
        if (event->type == INPUT_TYPE_JOYSTICK) {
            int idx = event->data.joystick_index;
            if (idx == 0) { // Left - move to Start
                if (welcome_btn_focus != 0) {
                    welcome_btn_focus = 0;
                    update_welcome_btn_focus();
                }
            } else if (idx == 3) { // Right - move to Skip
                if (welcome_btn_focus != 1) {
                    welcome_btn_focus = 1;
                    update_welcome_btn_focus();
                }
            } else if (idx == 4) { // Down - cycle the two choices
                welcome_btn_focus = (welcome_btn_focus + 1) % 2;
                update_welcome_btn_focus();
            } else if (idx == 1) { // Select - activate focused button
                if (welcome_btn_focus == 0) {
                    start_btn_event_cb(NULL);
                } else {
                    skip_setup();
                }
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == LV_KEY_LEFT || kv == 44 || kv == ',' || kv == 'h') { // left/comma/h
                welcome_btn_focus = 0;
                update_welcome_btn_focus();
            } else if (kv == LV_KEY_RIGHT || kv == 47 || kv == '/' || kv == 'l') { // right/slash/l
                welcome_btn_focus = 1;
                update_welcome_btn_focus();
            } else if (kv == LV_KEY_ENTER || kv == 13) { // enter
                if (welcome_btn_focus == 0) {
                    start_btn_event_cb(NULL);
                } else {
                    skip_setup();
                }
            } else if (kv == LV_KEY_ESC || kv == 29 || kv == '`') { // esc
                skip_setup();
            }
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_ENCODER) {
            int dir = event->data.encoder.direction;
            if (dir != 0) {
                welcome_btn_focus = (welcome_btn_focus + 1) % 2;
                update_welcome_btn_focus();
            }
            if (event->data.encoder.button) {
                if (welcome_btn_focus == 0) {
                    start_btn_event_cb(NULL);
                } else {
                    skip_setup();
                }
            }
        }
#endif
    } else if (current_step == SETUP_STEP_COUNTRY && country_list) {
        if (event->type == INPUT_TYPE_JOYSTICK) {
            int idx = event->data.joystick_index;
            if (idx == 2) { // Up
                if (country_cursor > 0) {
                    country_cursor--;
                    update_country_selection();
                }
            } else if (idx == 4) { // Down
                if (country_cursor < (int)COUNTRY_COUNT - 1) {
                    country_cursor++;
                    update_country_selection();
                }
            } else if (idx == 1 || idx == 3) { // Select/OK or Right
                selected_country_index = country_cursor;
                current_step = SETUP_STEP_TIMEZONE;
                display_manager_switch_view(&setup_wizard_view);
            } else if (idx == 0) { // Left/Back
                current_step = SETUP_STEP_STA_SSID;
                display_manager_switch_view(&setup_wizard_view);
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == LV_KEY_UP || kv == 'k' || kv == ';') { // up/k/semicolon
                if (country_cursor > 0) {
                    country_cursor--;
                    update_country_selection();
                }
            } else if (kv == LV_KEY_DOWN || kv == 'j' || kv == '.') { // down/j/period
                if (country_cursor < (int)COUNTRY_COUNT - 1) {
                    country_cursor++;
                    update_country_selection();
                }
            } else if (kv == LV_KEY_ENTER || kv == 13) { // enter
                selected_country_index = country_cursor;
                current_step = SETUP_STEP_TIMEZONE;
                display_manager_switch_view(&setup_wizard_view);
            } else if (kv == LV_KEY_ESC || kv == 29 || kv == '`') { // esc
                current_step = SETUP_STEP_STA_SSID;
                display_manager_switch_view(&setup_wizard_view);
            }
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_ENCODER) {
            int dir = event->data.encoder.direction;
            if (dir != 0) {
                country_cursor = (country_cursor + dir + (int)COUNTRY_COUNT) % (int)COUNTRY_COUNT;
                update_country_selection();
            }
            if (event->data.encoder.button) {
                selected_country_index = country_cursor;
                current_step = SETUP_STEP_TIMEZONE;
                display_manager_switch_view(&setup_wizard_view);
            }
        }
#endif
    } else if ((current_step == SETUP_STEP_TIMEZONE || current_step == SETUP_STEP_DISPLAY_TIMEOUT || current_step == SETUP_STEP_THEME || current_step == SETUP_STEP_MENU_LAYOUT ||
                current_step == SETUP_STEP_TERMINAL_COLOR || current_step == SETUP_STEP_EPILEPSY_WARNING || current_step == SETUP_STEP_ACCESSIBILITY
#ifdef CONFIG_WITH_STATUS_DISPLAY
                || current_step == SETUP_STEP_IDLE_ANIMATION
#endif
               ) && option_list) {
        int count = 0;
        SetupStep next_step = SETUP_STEP_COMPLETE;
        SetupStep prev_step = SETUP_STEP_COUNTRY;
        
        if (current_step == SETUP_STEP_TIMEZONE) {
            count = TIMEZONE_COUNT;
            next_step = SETUP_STEP_DISPLAY_TIMEOUT;
            prev_step = SETUP_STEP_COUNTRY;
        } else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) {
            count = DISPLAY_TIMEOUT_COUNT;
            next_step = SETUP_STEP_THEME;
            prev_step = SETUP_STEP_TIMEZONE;
        } else if (current_step == SETUP_STEP_THEME) {
            count = THEME_COUNT;
            next_step = SETUP_STEP_MENU_LAYOUT;
            prev_step = SETUP_STEP_DISPLAY_TIMEOUT;
        } else if (current_step == SETUP_STEP_MENU_LAYOUT) {
            count = MENU_LAYOUT_COUNT;
            next_step = SETUP_STEP_TERMINAL_COLOR;
            prev_step = SETUP_STEP_THEME;
        } else if (current_step == SETUP_STEP_TERMINAL_COLOR) {
            count = TERMINAL_COLOR_COUNT;
            next_step = SETUP_STEP_EPILEPSY_WARNING;
            prev_step = SETUP_STEP_MENU_LAYOUT;
        } else if (current_step == SETUP_STEP_EPILEPSY_WARNING) {
            count = EPILEPSY_WARNING_COUNT;
            next_step = SETUP_STEP_ACCESSIBILITY;
            prev_step = SETUP_STEP_TERMINAL_COLOR;
        } else if (current_step == SETUP_STEP_ACCESSIBILITY) {
            count = FONT_SIZE_COUNT;
#ifdef CONFIG_WITH_STATUS_DISPLAY
            next_step = SETUP_STEP_IDLE_ANIMATION;
#else
            next_step = SETUP_STEP_COMPLETE;
#endif
            prev_step = SETUP_STEP_EPILEPSY_WARNING;
        }
#ifdef CONFIG_WITH_STATUS_DISPLAY
        else if (current_step == SETUP_STEP_IDLE_ANIMATION) {
            count = IDLE_ANIMATION_COUNT;
            next_step = SETUP_STEP_COMPLETE;
            prev_step = SETUP_STEP_ACCESSIBILITY;
        }
#endif

        if (event->type == INPUT_TYPE_JOYSTICK) {
            int idx = event->data.joystick_index;
            if (idx == 2 && option_cursor > 0) { // Up
                option_cursor--;
                update_option_selection(count);
            } else if (idx == 4 && option_cursor < count - 1) { // Down
                option_cursor++;
                update_option_selection(count);
            } else if (idx == 1 || idx == 3) { // Select
                if (current_step == SETUP_STEP_TIMEZONE) temp_timezone = option_cursor;
                else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) temp_display_timeout = option_cursor;
                else if (current_step == SETUP_STEP_THEME) temp_theme = option_cursor;
                else if (current_step == SETUP_STEP_MENU_LAYOUT) temp_menu_layout = option_cursor;
                else if (current_step == SETUP_STEP_TERMINAL_COLOR) temp_terminal_color = option_cursor;
                else if (current_step == SETUP_STEP_EPILEPSY_WARNING) temp_epilepsy_warning = option_cursor;
                else if (current_step == SETUP_STEP_ACCESSIBILITY) temp_font_size = option_cursor;
#ifdef CONFIG_WITH_STATUS_DISPLAY
                else if (current_step == SETUP_STEP_IDLE_ANIMATION) temp_idle_animation = option_cursor;
#endif
                current_step = next_step;
                display_manager_switch_view(&setup_wizard_view);
            } else if (idx == 0) { // Back
                current_step = prev_step;
                display_manager_switch_view(&setup_wizard_view);
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if ((kv == LV_KEY_UP || kv == 'k' || kv == ';') && option_cursor > 0) { // up/k/semicolon
                option_cursor--;
                update_option_selection(count);
            } else if ((kv == LV_KEY_DOWN || kv == 'j' || kv == '.') && option_cursor < count - 1) { // down/j/period
                option_cursor++;
                update_option_selection(count);
            } else if (kv == LV_KEY_ENTER || kv == 13) { // enter
                if (current_step == SETUP_STEP_TIMEZONE) temp_timezone = option_cursor;
                else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) temp_display_timeout = option_cursor;
                else if (current_step == SETUP_STEP_THEME) temp_theme = option_cursor;
                else if (current_step == SETUP_STEP_MENU_LAYOUT) temp_menu_layout = option_cursor;
                else if (current_step == SETUP_STEP_TERMINAL_COLOR) temp_terminal_color = option_cursor;
                else if (current_step == SETUP_STEP_EPILEPSY_WARNING) temp_epilepsy_warning = option_cursor;
                else if (current_step == SETUP_STEP_ACCESSIBILITY) temp_font_size = option_cursor;
#ifdef CONFIG_WITH_STATUS_DISPLAY
                else if (current_step == SETUP_STEP_IDLE_ANIMATION) temp_idle_animation = option_cursor;
#endif
                current_step = next_step;
                display_manager_switch_view(&setup_wizard_view);
            } else if (kv == LV_KEY_ESC || kv == 29 || kv == '`') { // esc
                current_step = prev_step;
                display_manager_switch_view(&setup_wizard_view);
            }
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_ENCODER) {
            int dir = event->data.encoder.direction;
            if (dir != 0) {
                option_cursor = (option_cursor + dir + count) % count;
                update_option_selection(count);
            }
            if (event->data.encoder.button) {
                if (current_step == SETUP_STEP_TIMEZONE) temp_timezone = option_cursor;
                else if (current_step == SETUP_STEP_DISPLAY_TIMEOUT) temp_display_timeout = option_cursor;
                else if (current_step == SETUP_STEP_THEME) temp_theme = option_cursor;
                else if (current_step == SETUP_STEP_MENU_LAYOUT) temp_menu_layout = option_cursor;
                else if (current_step == SETUP_STEP_TERMINAL_COLOR) temp_terminal_color = option_cursor;
                else if (current_step == SETUP_STEP_EPILEPSY_WARNING) temp_epilepsy_warning = option_cursor;
                else if (current_step == SETUP_STEP_ACCESSIBILITY) temp_font_size = option_cursor;
#ifdef CONFIG_WITH_STATUS_DISPLAY
                else if (current_step == SETUP_STEP_IDLE_ANIMATION) temp_idle_animation = option_cursor;
#endif
                current_step = next_step;
                display_manager_switch_view(&setup_wizard_view);
            }
        }
#endif
    } else if (current_step == SETUP_STEP_COMPLETE) {
        if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 1) {
            finish_setup();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == LV_KEY_ENTER || kv == 13) {
                finish_setup();
            }
        }
#ifdef CONFIG_USE_ENCODER
        else if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
            finish_setup();
        }
#endif
    }
}

static void get_setup_wizard_callback(void **callback) {
    if (callback) *callback = (void *)setup_wizard_input_callback;
}

void setup_wizard_reset_and_open(void) {
    wizard_exiting = false;
    current_step = SETUP_STEP_WELCOME;
    selected_country_index = 0;
    temp_ap_ssid[0] = '\0';
    temp_ap_password[0] = '\0';
    temp_sta_ssid[0] = '\0';
    temp_sta_password[0] = '\0';
    temp_timezone = 0;
    temp_display_timeout = 0;
    temp_theme = 0;
    temp_menu_layout = 0;
    temp_terminal_color = 0;
    temp_epilepsy_warning = 1;
    temp_font_size = 1;
#ifdef CONFIG_IS_ATOMS3R
    temp_font_size = 0;
#endif
    temp_reduced_motion = 0;
    temp_high_contrast = 0;
#ifdef CONFIG_WITH_STATUS_DISPLAY
    temp_idle_animation = 0;
#endif
    display_manager_switch_view(&setup_wizard_view);
}

View setup_wizard_view = {
    .root = NULL,
    .create = setup_wizard_create,
    .destroy = setup_wizard_destroy,
    .input_callback = setup_wizard_input_callback,
    .name = "Setup Wizard",
    .get_hardwareinput_callback = get_setup_wizard_callback
};
