#include "managers/views/clock_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "gui/accessibility_fonts.h"
#include "gui/touch_bar.h"
#include "lvgl.h"
#include <math.h>
#include <time.h>
#include "managers/settings_manager.h"
#include "gui/lvgl_safe.h"
#include "gui/asset_pack.h"
#include "gui/theme_palette_api.h"
#include "esp_log.h"

uint32_t theme_palette_get_background(uint8_t theme);

#ifndef M_PI
#define M_PI 3.14159265f
#endif

static lv_obj_t *clock_container;
static lv_obj_t *content;
static lv_obj_t *clock_body;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *year_label;
static lv_obj_t *tz_label;
static lv_obj_t *analog_container;
static lv_obj_t *clock_toggle_btn;
static lv_obj_t *hour_hand;
static lv_obj_t *minute_hand;
static lv_obj_t *second_hand;
static lv_obj_t *analog_pivot;

/* Clock faces: 0 = Digital, 1 = Analog, 2 = Segment. */
enum { CLOCK_STYLE_DIGITAL = 0, CLOCK_STYLE_ANALOG = 1, CLOCK_STYLE_SEGMENT = 2 };
static uint8_t clock_style = CLOCK_STYLE_DIGITAL;

/* Seven-segment display: 6 digit cells (HH:MM:SS) + 2 colons + an AM/PM tag.
 * Segment order is a,b,c,d,e,f,g — clockwise from the top, then the middle. */
enum { SEG_A = 0, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F, SEG_G, SEG_COUNT };
#define CLOCK_SEG_DIGITS 6
typedef struct {
  lv_obj_t *seg[SEG_COUNT];
} clock_seg_cell_t;
static clock_seg_cell_t s_seg_cells[CLOCK_SEG_DIGITS];
#define CLOCK_SEG_COLON_DOTS 4 /* two colons x two dots */
static lv_obj_t *s_seg_colon[CLOCK_SEG_COLON_DOTS];
static lv_obj_t *s_seg_ampm;

lv_timer_t *clock_timer = NULL;

/* Geometry + palette snapshot taken at create time, reused by the rebuild. */
static lv_coord_t s_content_h;
static const lv_font_t *s_time_font;
static const lv_font_t *s_secondary_font;
static const lv_font_t *s_small_font;
static int s_row_gap;
static bool s_show_year_tz;
static bool s_asset_bg;
static lv_color_t s_text_color;
static lv_color_t s_muted_color;
static lv_color_t s_accent_color;
static lv_color_t s_border_color;
static lv_coord_t s_face_cx;
static lv_coord_t s_face_cy;
static lv_coord_t s_hour_len;
static lv_coord_t s_min_len;
static lv_coord_t s_sec_len;

static void clock_rebuild_body(void);
static void clock_update_segments(const struct tm *t);

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

static bool clock_point_in_obj(lv_obj_t *obj, lv_coord_t x, lv_coord_t y) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* Point on the dial: 0 deg at 3 o'clock, increasing clockwise (screen y down),
 * so 12 o'clock is -90 deg. */
static void clock_face_point(float angle_deg, lv_coord_t radius, lv_coord_t *out_x, lv_coord_t *out_y) {
    float a = angle_deg * (float)M_PI / 180.0f;
    *out_x = s_face_cx + (lv_coord_t)(cosf(a) * (float)radius);
    *out_y = s_face_cy + (lv_coord_t)(sinf(a) * (float)radius);
}

static void clock_update_hands(const struct tm *t) {
    if (!hour_hand || !minute_hand || !second_hand) return;

    float sec = (float)t->tm_sec;
    float minute = (float)t->tm_min + sec / 60.0f;
    float hour = (float)(t->tm_hour % 12) + minute / 60.0f;

    static lv_point_t h_pts[2];
    static lv_point_t m_pts[2];
    static lv_point_t s_pts[2];

    lv_coord_t x, y;
    h_pts[0].x = s_face_cx; h_pts[0].y = s_face_cy;
    clock_face_point(hour * 30.0f - 90.0f, s_hour_len, &x, &y);
    h_pts[1].x = x; h_pts[1].y = y;
    lv_line_set_points(hour_hand, h_pts, 2);

    m_pts[0].x = s_face_cx; m_pts[0].y = s_face_cy;
    clock_face_point(minute * 6.0f - 90.0f, s_min_len, &x, &y);
    m_pts[1].x = x; m_pts[1].y = y;
    lv_line_set_points(minute_hand, m_pts, 2);

    s_pts[0].x = s_face_cx; s_pts[0].y = s_face_cy;
    clock_face_point(sec * 6.0f - 90.0f, s_sec_len, &x, &y);
    s_pts[1].x = x; s_pts[1].y = y;
    lv_line_set_points(second_hand, s_pts, 2);
}

static void clock_update_cb(lv_timer_t *timer) {
    (void)timer;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (clock_style == CLOCK_STYLE_ANALOG) {
        clock_update_hands(&timeinfo);
    } else if (clock_style == CLOCK_STYLE_SEGMENT) {
        clock_update_segments(&timeinfo);
    } else if (time_label && lv_obj_is_valid(time_label)) {
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
    }

    if (date_label && lv_obj_is_valid(date_label)) {
        char buf_date[32];
        strftime(buf_date, sizeof(buf_date), "%A, %B %d", &timeinfo);
        lv_label_set_text(date_label, buf_date);
    }

    if (year_label && lv_obj_is_valid(year_label)) {
        char buf_year[8];
        strftime(buf_year, sizeof(buf_year), "%Y", &timeinfo);
        lv_label_set_text(year_label, buf_year);
    }

    if (tz_label && lv_obj_is_valid(tz_label)) {
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
}

static void clock_build_digital(void) {
    lv_obj_t *label_stack = lv_obj_create(content);
    clock_body = label_stack;
    lv_obj_set_style_bg_opa(label_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(label_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label_stack, 0, 0);
    lv_obj_set_size(label_stack, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(label_stack, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_layout(label_stack, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(label_stack, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(label_stack, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(label_stack, s_row_gap, 0);

    time_label = create_clock_label(label_stack, "12:00:00 AM", s_time_font, s_text_color, s_asset_bg);

    date_label = create_clock_label(label_stack, "Wednesday, January 01", s_secondary_font, s_text_color, s_asset_bg);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(date_label, LV_PCT(95));
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);

    year_label = create_clock_label(label_stack, "2025", s_secondary_font, s_text_color, s_asset_bg);
    if (!s_show_year_tz) lv_obj_add_flag(year_label, LV_OBJ_FLAG_HIDDEN);

    tz_label = create_clock_label(label_stack, "TZ: UTC", s_small_font, s_text_color, s_asset_bg);
    if (!s_show_year_tz) lv_obj_add_flag(tz_label, LV_OBJ_FLAG_HIDDEN);
}

/* Bottom date/TZ stack shared by the analog and segment faces. Pinned to the
 * bottom of its body and inset by GUI_SAFEAREA_VER. Returns its measured
 * height (0 when hidden) so callers can size the face around it. */
static lv_coord_t clock_build_footer(lv_obj_t *body) {
    if (!s_show_year_tz) return 0;

    lv_obj_t *footer = lv_obj_create(body);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -GUI_SAFEAREA_VER);
    lv_obj_set_layout(footer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(footer, s_row_gap, 0);

    date_label = create_clock_label(footer, "Wednesday, January 01", s_secondary_font, s_text_color, s_asset_bg);
    lv_label_set_long_mode(date_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(date_label, LV_PCT(95));
    lv_obj_set_style_text_align(date_label, LV_TEXT_ALIGN_CENTER, 0);

    tz_label = create_clock_label(footer, "TZ: UTC", s_small_font, s_text_color, s_asset_bg);

    lv_obj_update_layout(footer);
    return lv_obj_get_height(footer);
}

/* --- Seven-segment face ------------------------------------------------ */

static void clock_seg_set(lv_obj_t *seg, bool lit) {
    if (!seg || !lv_obj_is_valid(seg)) return;
    /* Unlit segments stay just barely visible, like a real LCD. */
    lv_obj_set_style_bg_color(seg, lit ? s_text_color : s_muted_color, 0);
    lv_obj_set_style_bg_opa(seg, lit ? LV_OPA_COVER : LV_OPA_10, 0);
}

/* Build the 7 bars of one digit inside `parent`, which must be exactly the
 * digit cell: bars are laid out in cell-local coordinates. */
static void clock_seg_build_digit(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                                  clock_seg_cell_t *cell) {
    lv_coord_t t = w / 5;              // bar thickness
    if (t < 2) t = 2;
    lv_coord_t hl = w - 2 * t;         // horizontal bar length
    lv_coord_t vl = (h - 3 * t) / 2;   // vertical bar length
    if (vl < 2) vl = 2;
    lv_coord_t mid_y = (h - t) / 2;
    lv_coord_t bot_y = h - t;
    lv_coord_t vbot_y = mid_y + t;

    const lv_coord_t geo[SEG_COUNT][4] = {
        [SEG_A] = { t,       0,        hl, t  },
        [SEG_B] = { w - t,   t,        t,  vl },
        [SEG_C] = { w - t,   vbot_y,   t,  vl },
        [SEG_D] = { t,       bot_y,    hl, t  },
        [SEG_E] = { 0,       vbot_y,   t,  vl },
        [SEG_F] = { 0,       t,        t,  vl },
        [SEG_G] = { t,       mid_y,    hl, t  },
    };

    for (int i = 0; i < SEG_COUNT; i++) {
        lv_obj_t *seg = lv_obj_create(parent);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, geo[i][0], geo[i][1]);
        lv_obj_set_size(seg, geo[i][2], geo[i][3]);
        lv_obj_set_style_radius(seg, t / 2, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        cell->seg[i] = seg;
    }
}

static void clock_build_segment(void) {
    lv_obj_t *body = lv_obj_create(content);
    clock_body = body;
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);

    lv_coord_t footer_h = clock_build_footer(body);
    lv_coord_t reserve_h = footer_h + (footer_h > 0 ? GUI_SAFEAREA_VER : 0);

    /* GUI_SAFEAREA_HOR (not GUI_GRID) so the digits keep a visible margin
     * instead of stretching edge to edge. */
    lv_coord_t avail_w = LV_HOR_RES - 2 * GUI_SAFEAREA_HOR;
    lv_coord_t avail_h = s_content_h - reserve_h - 2 * GUI_GRID;
    if (avail_h < 40) avail_h = 40;

    /* 6 digits + 2 colons (half-width) + 7 gaps (sixth-width) = 8.1667 digit
     * widths across. Keep a ~2.2:1 digit aspect, shrinking both axes when the
     * height is what binds so the digits never go squat. */
    lv_coord_t w = (lv_coord_t)((float)avail_w / 8.1667f);
    lv_coord_t h = (lv_coord_t)((float)w * 2.2f);
    if (h > avail_h) {
        h = avail_h;
        w = (lv_coord_t)((float)h / 2.2f);
    }
    if (w < 6) w = 6;
    /* Floor the height against the same bar thickness the builder will use, so
     * three stacked bars always fit. */
    lv_coord_t t_est = w / 5;
    if (t_est < 2) t_est = 2;
    if (h < 3 * t_est + 4) h = 3 * t_est + 4;

    lv_coord_t colon_w = w / 2;
    lv_coord_t gap = w / 6;
    if (gap < 2) gap = 2;
    lv_coord_t total_w = 6 * w + 2 * colon_w + 7 * gap;

    lv_obj_t *disp = lv_obj_create(body);
    lv_obj_set_style_bg_opa(disp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(disp, 0, 0);
    lv_obj_set_style_pad_all(disp, 0, 0);
    lv_obj_set_size(disp, total_w, h);
    lv_obj_align(disp, LV_ALIGN_CENTER, 0, -(reserve_h / 2));

    lv_coord_t dot = w / 5;
    if (dot < 2) dot = 2;

    lv_coord_t x = 0;
    for (int i = 0; i < CLOCK_SEG_DIGITS; i++) {
        lv_obj_t *cell = lv_obj_create(disp);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, w, h);
        lv_obj_set_pos(cell, x, 0);
        clock_seg_build_digit(cell, w, h, &s_seg_cells[i]);
        x += w + gap;

        if (i == 1 || i == 3) {
            lv_obj_t *c = lv_obj_create(disp);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, colon_w, h);
            lv_obj_set_pos(c, x, 0);
            int colon_base = (i == 1) ? 0 : 2;
            for (int d = 0; d < 2; d++) {
                lv_obj_t *dot_obj = lv_obj_create(c);
                lv_obj_remove_style_all(dot_obj);
                lv_obj_set_size(dot_obj, dot, dot);
                lv_obj_set_style_radius(dot_obj, dot / 2, 0);
                lv_obj_set_style_bg_opa(dot_obj, LV_OPA_COVER, 0);
                lv_obj_clear_flag(dot_obj, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_pos(dot_obj, (colon_w - dot) / 2,
                               (d == 0) ? (h / 3 - dot / 2) : (2 * h / 3 - dot / 2));
                s_seg_colon[colon_base + d] = dot_obj;
            }
            x += colon_w + gap;
        }
    }

    /* AM/PM sits top-left; the style toggle pill owns the top-right corner. */
    s_seg_ampm = create_clock_label(body, "AM", s_small_font, s_muted_color, s_asset_bg);
    lv_obj_align(s_seg_ampm, LV_ALIGN_TOP_LEFT, GUI_GRID, GUI_GRID);
}

static void clock_update_segments(const struct tm *t) {
    static const uint8_t masks[10] = {
        0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
    };

    int h12 = t->tm_hour % 12;
    if (h12 == 0) h12 = 12;

    const int digits[CLOCK_SEG_DIGITS] = {
        h12 / 10, h12 % 10,
        t->tm_min / 10, t->tm_min % 10,
        t->tm_sec / 10, t->tm_sec % 10
    };
    bool blank_first = (h12 < 10);

    for (int i = 0; i < CLOCK_SEG_DIGITS; i++) {
        uint8_t m = (i == 0 && blank_first) ? 0 : masks[digits[i]];
        for (int s = 0; s < SEG_COUNT; s++) {
            clock_seg_set(s_seg_cells[i].seg[s], (m >> s) & 1);
        }
    }

    bool colon_on = (t->tm_sec % 2) == 0;
    for (int i = 0; i < CLOCK_SEG_COLON_DOTS; i++) {
        clock_seg_set(s_seg_colon[i], colon_on);
    }

    if (s_seg_ampm && lv_obj_is_valid(s_seg_ampm)) {
        lv_label_set_text(s_seg_ampm, t->tm_hour >= 12 ? "PM" : "AM");
    }
}

static void clock_build_analog(void) {
    /* Body spans the content area so date/TZ labels can sit below the dial. */
    lv_obj_t *body = lv_obj_create(content);
    clock_body = body;
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_size(body, LV_PCT(100), LV_PCT(100));
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 0);

    lv_coord_t footer_h = clock_build_footer(body);

    /* Space the footer claims at the bottom: its own height plus the margin it
     * is inset by. Both must come out of the dial's budget, and the dial is
     * re-centred on what is left, so it gets an even gap above and below. */
    lv_coord_t reserve_h = footer_h + (footer_h > 0 ? GUI_SAFEAREA_VER : 0);

    lv_coord_t side = LV_HOR_RES - 2 * GUI_GRID;
    lv_coord_t vert = s_content_h - reserve_h - 2 * GUI_GRID;
    if (vert < 40) vert = 40;
    /* Never force the dial past the space above the footer, or it re-overlaps
     * the labels on short panels (e.g. a 160x80 T-Dongle). */
    lv_coord_t face = LV_MIN(side, vert);

    analog_container = lv_obj_create(body);
    lv_obj_set_style_bg_opa(analog_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(analog_container, 0, 0);
    lv_obj_set_style_pad_all(analog_container, 0, 0);
    lv_obj_set_size(analog_container, face, face);
    lv_obj_align(analog_container, LV_ALIGN_CENTER, 0, -(reserve_h / 2));

    s_face_cx = face / 2;
    s_face_cy = face / 2;
    lv_coord_t R = face / 2 - 2;
    s_hour_len = (lv_coord_t)(R * 0.50f);
    s_min_len = (lv_coord_t)(R * 0.72f);
    s_sec_len = (lv_coord_t)(R * 0.82f);

    /* Outer ring */
    lv_obj_t *ring = lv_obj_create(analog_container);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, face - 2, face - 2);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, s_border_color, 0);

    /* 60 minute ticks — skipped on short panels to keep the object count down. */
    if (s_content_h >= 180) {
        static lv_point_t min_pts[60][2];
        for (int i = 0; i < 60; i++) {
            if (i % 5 == 0) continue; /* covered by the hour ticks */
            lv_coord_t ix, iy, ox, oy;
            clock_face_point((float)i * 6.0f - 90.0f, R - 4, &ox, &oy);
            clock_face_point((float)i * 6.0f - 90.0f, R - 9, &ix, &iy);
            min_pts[i][0].x = ix; min_pts[i][0].y = iy;
            min_pts[i][1].x = ox; min_pts[i][1].y = oy;
            lv_obj_t *tick = lv_line_create(analog_container);
            lv_line_set_points(tick, min_pts[i], 2);
            lv_obj_set_style_line_color(tick, s_muted_color, 0);
            lv_obj_set_style_line_width(tick, 1, 0);
            lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    /* 12 hour ticks */
    static lv_point_t hour_pts[12][2];
    for (int i = 0; i < 12; i++) {
        lv_coord_t ix, iy, ox, oy;
        clock_face_point((float)i * 30.0f - 90.0f, R - 3, &ox, &oy);
        clock_face_point((float)i * 30.0f - 90.0f, R - 15, &ix, &iy);
        hour_pts[i][0].x = ix; hour_pts[i][0].y = iy;
        hour_pts[i][1].x = ox; hour_pts[i][1].y = oy;
        lv_obj_t *tick = lv_line_create(analog_container);
        lv_line_set_points(tick, hour_pts[i], 2);
        lv_obj_set_style_line_color(tick, s_text_color, 0);
        lv_obj_set_style_line_width(tick, 3, 0);
        lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    }

    /* Numerals 1-12 — omitted on small dials, where they would crowd the
     * ticks and each other into an unreadable ring. */
    lv_coord_t num_r = (lv_coord_t)(R * 0.76f);
    for (int i = 0; face >= 110 && i < 12; i++) {
        lv_coord_t nx, ny;
        clock_face_point((float)i * 30.0f - 90.0f, num_r, &nx, &ny);
        lv_obj_t *num = lv_label_create(analog_container);
        lv_label_set_text_fmt(num, "%d", (i == 0) ? 12 : i);
        lv_obj_set_style_text_color(num, s_text_color, 0);
        lv_obj_set_style_text_font(num, s_small_font, 0);
        lv_obj_update_layout(num);
        lv_obj_set_pos(num, nx - lv_obj_get_width(num) / 2, ny - lv_obj_get_height(num) / 2);
    }

    /* Hands */
    hour_hand = lv_line_create(analog_container);
    lv_obj_set_style_line_width(hour_hand, 3, 0);
    lv_obj_set_style_line_color(hour_hand, s_text_color, 0);
    lv_obj_set_style_line_rounded(hour_hand, true, 0);
    lv_obj_clear_flag(hour_hand, LV_OBJ_FLAG_CLICKABLE);

    minute_hand = lv_line_create(analog_container);
    lv_obj_set_style_line_width(minute_hand, 2, 0);
    lv_obj_set_style_line_color(minute_hand, s_text_color, 0);
    lv_obj_set_style_line_rounded(minute_hand, true, 0);
    lv_obj_clear_flag(minute_hand, LV_OBJ_FLAG_CLICKABLE);

    second_hand = lv_line_create(analog_container);
    lv_obj_set_style_line_width(second_hand, 1, 0);
    lv_obj_set_style_line_color(second_hand, s_accent_color, 0);
    lv_obj_set_style_line_rounded(second_hand, true, 0);
    lv_obj_clear_flag(second_hand, LV_OBJ_FLAG_CLICKABLE);

    analog_pivot = lv_obj_create(analog_container);
    lv_obj_remove_style_all(analog_pivot);
    lv_obj_set_size(analog_pivot, 8, 8);
    lv_obj_set_style_bg_color(analog_pivot, s_accent_color, 0);
    lv_obj_set_style_bg_opa(analog_pivot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(analog_pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(analog_pivot, LV_ALIGN_CENTER, 0, 0);
}

static void clock_rebuild_body(void) {
    lvgl_obj_del_safe(&clock_body);
    time_label = NULL;
    date_label = NULL;
    year_label = NULL;
    tz_label = NULL;
    analog_container = NULL;
    hour_hand = NULL;
    minute_hand = NULL;
    second_hand = NULL;
    analog_pivot = NULL;
    for (int i = 0; i < CLOCK_SEG_DIGITS; i++) {
        for (int s = 0; s < SEG_COUNT; s++) s_seg_cells[i].seg[s] = NULL;
    }
    for (int i = 0; i < CLOCK_SEG_COLON_DOTS; i++) s_seg_colon[i] = NULL;
    s_seg_ampm = NULL;

    if (clock_style == CLOCK_STYLE_ANALOG) {
        clock_build_analog();
    } else if (clock_style == CLOCK_STYLE_SEGMENT) {
        clock_build_segment();
    } else {
        clock_build_digital();
    }

    if (clock_toggle_btn && lv_obj_is_valid(clock_toggle_btn)) {
        static const char *const names[] = {"Digital", "Analog", "Segment"};
        uint8_t idx = clock_style < 3 ? clock_style : 0;
        lv_label_set_text(clock_toggle_btn, names[idx]);
        lv_obj_move_foreground(clock_toggle_btn);
    }

    clock_update_cb(NULL);
}

static void clock_toggle_style(void) {
    clock_style = (uint8_t)((clock_style + 1) % 3); // Digital -> Analog -> Segment
    settings_set_clock_style(&G_Settings, clock_style);
    settings_persist_setting(SETTING_CLOCK_STYLE);
    clock_rebuild_body();
}

static void clock_event_handler(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            if (clock_point_in_obj(clock_toggle_btn,
                                   event->data.touch_data.point.x,
                                   event->data.touch_data.point.y)) {
                clock_toggle_style();
            } else {
                display_manager_go_back();
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        if (!event->data.joystick_pressed) return;
        if (event->data.joystick_index == 1) {
            clock_toggle_style();
        } else {
            display_manager_go_back();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (event->is_touch_move) return; // ignore key release
        uint8_t key = event->data.key_value;
        if (key == LV_KEY_ENTER || key == '\n' || key == '\r' || key == 13 || key == ' ') {
            clock_toggle_style();
        } else {
            display_manager_go_back();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            display_manager_go_back(); // push exits
        } else if (event->data.encoder.direction != 0) {
            clock_toggle_style(); // rotate cycles the clock face
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
#endif
    }
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
    s_asset_bg = asset_pack_get_background_tile() != NULL;
    s_text_color = lv_color_hex(theme_palette_get_text(theme));
    s_muted_color = lv_color_hex(theme_palette_get_text_muted(theme));
    s_accent_color = lv_color_hex(theme_palette_get_accent(theme));
    s_border_color = lv_color_hex(theme_palette_get_border(theme));

    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));
    display_manager_fill_screen(bg_color);
    clock_container = gui_screen_create_root(NULL, "Clock", bg_color, s_asset_bg ? LV_OPA_TRANSP : LV_OPA_COVER);
    clock_view.root = clock_container;

    /* Keep the footer clear of the large-screen home bar and the legacy touch
     * bar. The two are complementary: large screens reserve the home bar and
     * report no touch bar, small touch boards the reverse. */
    lv_coord_t bottom_reserved = GUI_HOME_SAFE_H + gui_touch_bar_height();
    content = gui_screen_create_content_with_bottom_reserved(clock_container, GUI_STATUS_BAR_HEIGHT,
                                                             bottom_reserved);
    /* Compute the height directly rather than reading it back from the object:
     * lv_obj_get_height() only reflects coordinates after the next layout pass,
     * so reading it here returns a stale 0 and collapses the dial. */
    s_content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT - bottom_reserved;
    if (s_content_h < 0) s_content_h = 0;

    // Pick fonts and row gap based on usable content height so nothing clips
    if (s_content_h < 80) {
        s_time_font      = accessibility_get_font_display();
        s_secondary_font = accessibility_get_font_small();
        s_small_font     = accessibility_get_font_small();
        s_row_gap        = 2;
        s_show_year_tz   = false;
    } else if (s_content_h < 120) {
        s_time_font      = accessibility_get_font_display();
        s_secondary_font = accessibility_get_font_body();
        s_small_font     = accessibility_get_font_small();
        s_row_gap        = 4;
        s_show_year_tz   = true;
    } else {
        s_time_font      = accessibility_get_font_display();
        s_secondary_font = accessibility_get_font_body();
        s_small_font     = accessibility_get_font_body();
        s_row_gap        = 6;
        s_show_year_tz   = true;
    }

    // Small tappable pill that switches the face; also the touch target.
    clock_toggle_btn = lv_label_create(content);
    lv_obj_set_style_text_font(clock_toggle_btn, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(clock_toggle_btn, s_muted_color, 0);
    lv_obj_set_style_bg_color(clock_toggle_btn, lv_color_hex(theme_palette_get_surface(theme)), 0);
    lv_obj_set_style_bg_opa(clock_toggle_btn, LV_OPA_70, 0);
    lv_obj_set_style_radius(clock_toggle_btn, 4, 0);
    lv_obj_set_style_pad_hor(clock_toggle_btn, 6, 0);
    lv_obj_set_style_pad_ver(clock_toggle_btn, 2, 0);
    lv_obj_align(clock_toggle_btn, LV_ALIGN_TOP_RIGHT, -GUI_GRID, GUI_GRID);

    clock_style = settings_get_clock_style(&G_Settings);
    if (clock_style > CLOCK_STYLE_SEGMENT) clock_style = CLOCK_STYLE_DIGITAL;
    clock_rebuild_body();

    clock_timer = lv_timer_create(clock_update_cb, 1000, NULL);
}

void clock_destroy(void) {
    lvgl_timer_del_safe(&clock_timer);
    lvgl_obj_del_safe(&clock_body);
    lvgl_obj_del_safe(&clock_toggle_btn);
    if (clock_container) {
        lv_obj_clean(clock_container);
        lvgl_obj_del_safe(&clock_container);
        clock_view.root = NULL;
    }
    content = NULL;
    analog_container = NULL;
    time_label = NULL;
    date_label = NULL;
    year_label = NULL;
    tz_label = NULL;
    hour_hand = NULL;
    minute_hand = NULL;
    second_hand = NULL;
    analog_pivot = NULL;
    for (int i = 0; i < CLOCK_SEG_DIGITS; i++) {
        for (int s = 0; s < SEG_COUNT; s++) s_seg_cells[i].seg[s] = NULL;
    }
    for (int i = 0; i < CLOCK_SEG_COLON_DOTS; i++) s_seg_colon[i] = NULL;
    s_seg_ampm = NULL;
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
