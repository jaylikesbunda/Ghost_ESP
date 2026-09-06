#include "gui/toast.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/lvgl_safe.h"
#include "managers/haptic_manager.h"
#include "managers/settings_manager.h"
#include "managers/display_manager.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

#define TOAST_QUEUE_SIZE   1
#define TOAST_SLIDE_IN_MS  250
#define TOAST_SLIDE_OUT_MS 200
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
#define TOAST_MIN_HEIGHT   48
#define TOAST_RADIUS       GUI_RADIUS_SM
#define TOAST_PAD_H        16
#define TOAST_PAD_V        10
#define TOAST_ICON_GAP     10
#define TOAST_Y_TARGET     (GUI_STATUS_BAR_H + 4)
#elif defined(CONFIG_CROWPANEL_1P28_ROTARY)
#define TOAST_MIN_HEIGHT   32
#define TOAST_RADIUS       16
#define TOAST_PAD_H        10
#define TOAST_PAD_V        6
#define TOAST_ICON_GAP     6
#define TOAST_Y_TARGET     56
#else
#define TOAST_MIN_HEIGHT   32
#define TOAST_RADIUS       8
#define TOAST_PAD_H        10
#define TOAST_PAD_V        6
#define TOAST_ICON_GAP     6
#define TOAST_Y_TARGET     24
#endif

typedef struct {
    char text[TOAST_MAX_TEXT_LEN + 1];
    uint16_t duration_ms;
    uint8_t type;
} toast_slot_t;

static toast_slot_t s_queue[TOAST_QUEUE_SIZE];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;
static uint8_t s_count = 0;

static lv_obj_t *s_container = NULL;
static lv_obj_t *s_icon = NULL;
static lv_obj_t *s_label = NULL;
static lv_obj_t *s_accent_bar = NULL;
static lv_obj_t *s_inner = NULL;
static lv_timer_t *s_timer = NULL;

static volatile bool s_showing = false;
static uint16_t s_current_duration_ms = 0;
static toast_slot_t s_current;

static uint32_t toast_accent_for_type(uint8_t type) {
    switch (type) {
        case TOAST_SUCCESS: return 0x4CAF50;
        case TOAST_WARN:    return 0xFF9800;
        case TOAST_ERROR:   return 0xF44336;
        default:            return theme_palette_get_accent(settings_get_menu_theme(&G_Settings));
    }
}

static const char *toast_icon_for_type(uint8_t type) {
    switch (type) {
        case TOAST_SUCCESS: return LV_SYMBOL_OK;
        case TOAST_WARN:    return LV_SYMBOL_WARNING;
        case TOAST_ERROR:   return LV_SYMBOL_CLOSE;
        default:            return LV_SYMBOL_BELL;
    }
}

static bool q_empty(void) { return s_count == 0; }
static bool q_full(void) { return s_count >= TOAST_QUEUE_SIZE; }

static bool toast_matches(const toast_slot_t *slot, const char *text, uint8_t type) {
    return slot->type == type && strcmp(slot->text, text) == 0;
}

static bool q_contains(const char *text, uint8_t type) {
    for (uint8_t i = 0; i < s_count; i++) {
        const toast_slot_t *slot = &s_queue[(s_head + i) % TOAST_QUEUE_SIZE];
        if (toast_matches(slot, text, type)) return true;
    }
    return false;
}

static void q_clear(void) {
    s_head = 0;
    s_tail = 0;
    s_count = 0;
}

static bool q_push(const char *text, uint8_t type, uint16_t dur) {
    if (q_full()) q_clear();
    toast_slot_t *s = &s_queue[s_tail];
    strncpy(s->text, text ? text : "", TOAST_MAX_TEXT_LEN);
    s->text[TOAST_MAX_TEXT_LEN] = '\0';
    s->type = type;
    s->duration_ms = dur > 0 ? dur : TOAST_DEFAULT_DURATION_MS;
    s_tail = (s_tail + 1) % TOAST_QUEUE_SIZE;
    s_count++;
    return true;
}

static bool q_pop(toast_slot_t *out) {
    if (q_empty()) return false;
    *out = s_queue[s_head];
    s_head = (s_head + 1) % TOAST_QUEUE_SIZE;
    s_count--;
    return true;
}

static lv_coord_t offscreen_y(void) {
    return -(TOAST_MIN_HEIGHT + 8);
}

static void toast_set_y_anim_cb(void *obj, int32_t v) {
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void toast_ensure_objects(void) {
    if (s_container) return;

    if (!display_manager_is_available()) return;
    lv_obj_t *parent = lv_layer_top();
    if (!parent) return;

    s_container = lv_obj_create(parent);
    lv_obj_remove_style_all(s_container);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
    const lv_coord_t toast_width = 176;
    const lv_coord_t toast_x = (LV_HOR_RES - toast_width) / 2;
#else
    const lv_coord_t toast_width = LV_HOR_RES - GUI_SAFEAREA_HOR * 2;
    const lv_coord_t toast_x = GUI_SAFEAREA_HOR;
#endif
    lv_obj_set_size(s_container, toast_width, TOAST_MIN_HEIGHT);
    lv_obj_set_pos(s_container, toast_x, offscreen_y());
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_container, TOAST_RADIUS, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_clip_corner(s_container, true, 0);
    lv_obj_set_style_shadow_color(s_container, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(s_container, 4, 0);
    lv_obj_set_style_shadow_opa(s_container, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(s_container, 2, 0);

    s_accent_bar = lv_obj_create(s_container);
    lv_obj_remove_style_all(s_accent_bar);
    lv_obj_set_size(s_accent_bar, 3, TOAST_MIN_HEIGHT);
    lv_obj_align(s_accent_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_accent_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_accent_bar, 0, 0);

    s_inner = lv_obj_create(s_container);
    lv_obj_remove_style_all(s_inner);
    lv_obj_set_size(s_inner, toast_width - 3 - TOAST_PAD_H * 2, TOAST_MIN_HEIGHT);
    lv_obj_align(s_inner, LV_ALIGN_LEFT_MID, 3 + TOAST_PAD_H, 0);
    lv_obj_set_flex_flow(s_inner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_inner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_inner, TOAST_ICON_GAP, 0);
    lv_obj_clear_flag(s_inner, LV_OBJ_FLAG_SCROLLABLE);

    s_icon = lv_label_create(s_inner);
    lv_obj_set_style_text_font(s_icon, gui_font_caption(), 0);

    s_label = lv_label_create(s_inner);
    lv_obj_set_style_text_font(s_label, gui_font_body(), 0);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
}

static void toast_apply_style(uint8_t type) {
    if (!s_container) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t accent = toast_accent_for_type(type);
    lv_color_t ac = lv_color_hex(accent);

    lv_obj_set_style_bg_color(s_container, lv_color_hex(theme_palette_get_surface_alt(theme)), 0);
    lv_obj_set_style_bg_color(s_accent_bar, ac, 0);
    lv_obj_set_style_text_color(s_icon, ac, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(theme_palette_get_text(theme)), 0);
    lv_label_set_text(s_icon, toast_icon_for_type(type));
}

static void toast_slide_out(void);
static void toast_present_next_cb(void *arg);

static void toast_auto_dismiss_cb(lv_timer_t *timer) {
    (void)timer;
    s_timer = NULL;
    toast_slide_out();
}

static void slide_in_ready_cb(lv_anim_t *a) {
    (void)a;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_timer = lv_timer_create(toast_auto_dismiss_cb, s_current_duration_ms, NULL);
    lv_timer_set_repeat_count(s_timer, 1);
}

static void slide_out_ready_cb(lv_anim_t *a) {
    (void)a;
    if (s_container) lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    s_showing = false;

    if (!q_empty()) {
        display_manager_run_on_lvgl(toast_present_next_cb, NULL);
    }
}

static void toast_present_next(void) {
    toast_slot_t slot;
    if (!q_pop(&slot)) return;

    toast_ensure_objects();
    if (!s_container) return;
    s_showing = true;
    s_current = slot;
    s_current_duration_ms = slot.duration_ms;

    lv_label_set_text(s_label, slot.text);

    int text_w = LV_HOR_RES - GUI_SAFEAREA_HOR * 2 - 3 - TOAST_PAD_H * 2 - TOAST_ICON_GAP - 16;
    if (text_w < 40) text_w = 40;
    lv_obj_set_width(s_label, text_w);

    const lv_font_t *font = gui_font_body();
    lv_point_t txt_size;
    lv_txt_get_size(&txt_size, slot.text, font,
                    lv_obj_get_style_text_letter_space(s_label, 0),
                    lv_obj_get_style_text_line_space(s_label, 0),
                    text_w, LV_TEXT_FLAG_NONE);

    lv_coord_t h = txt_size.y + TOAST_PAD_V * 2;
    if (h < TOAST_MIN_HEIGHT) h = TOAST_MIN_HEIGHT;

    lv_obj_set_height(s_container, h);
    lv_obj_set_height(s_accent_bar, h);
    lv_obj_set_height(s_inner, h);

    toast_apply_style(slot.type);

    lv_coord_t start_y = -(h + 8);

    lv_obj_set_style_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_container);
    lv_obj_set_y(s_container, start_y);

    lv_anim_del(s_container, NULL);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_container);
    lv_anim_set_values(&anim, start_y, TOAST_Y_TARGET);
    lv_anim_set_time(&anim, TOAST_SLIDE_IN_MS);
    lv_anim_set_exec_cb(&anim, toast_set_y_anim_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&anim, slide_in_ready_cb);
    lv_anim_start(&anim);
}

static void toast_present_next_cb(void *arg) {
    (void)arg;
    toast_present_next();
}

static void toast_slide_out(void) {
    if (!s_container) return;

    lv_anim_del(s_container, NULL);

    lv_coord_t cur_y = lv_obj_get_y(s_container);
    lv_coord_t end_y = -(lv_obj_get_height(s_container) + 8);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_container);
    lv_anim_set_values(&anim, cur_y, end_y);
    lv_anim_set_time(&anim, TOAST_SLIDE_OUT_MS);
    lv_anim_set_exec_cb(&anim, toast_set_y_anim_cb);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&anim, slide_out_ready_cb);
    lv_anim_start(&anim);
}

typedef struct {
    char text[TOAST_MAX_TEXT_LEN + 1];
    uint8_t type;
    uint16_t duration_ms;
} toast_async_arg_t;

static void toast_play_haptic(uint8_t type) {
    switch (type) {
        case TOAST_SUCCESS:
            haptic_manager_play(HAPTIC_EFFECT_SUCCESS);
            break;
        case TOAST_WARN:
            haptic_manager_play(HAPTIC_EFFECT_WARNING);
            break;
        case TOAST_ERROR:
            haptic_manager_play(HAPTIC_EFFECT_ERROR);
            break;
        default:
            haptic_manager_play(HAPTIC_EFFECT_NOTIFICATION);
            break;
    }
}

static void toast_async_cb(void *arg) {
    toast_async_arg_t *a = (toast_async_arg_t *)arg;
    if (!a) return;

    if ((s_showing && toast_matches(&s_current, a->text, a->type)) ||
        q_contains(a->text, a->type)) {
        free(a);
        return;
    }

    bool was_idle = !s_showing && q_empty();
    q_push(a->text, a->type, a->duration_ms);
    toast_play_haptic(a->type);

    if (was_idle) {
        toast_present_next();
    }
    free(a);
}

static void toast_post(const char *text, uint8_t type, uint16_t duration_ms) {
    if (!text || !text[0]) return;

    toast_async_arg_t *a = malloc(sizeof(toast_async_arg_t));
    if (!a) return;
    strncpy(a->text, text, TOAST_MAX_TEXT_LEN);
    a->text[TOAST_MAX_TEXT_LEN] = '\0';
    a->type = type;
    a->duration_ms = duration_ms > 0 ? duration_ms : TOAST_DEFAULT_DURATION_MS;

    display_manager_run_on_lvgl(toast_async_cb, a);
}

void toast_show(const char *text, uint8_t type) {
    toast_post(text, type, TOAST_DEFAULT_DURATION_MS);
}

void toast_show_duration(const char *text, uint8_t type, uint16_t duration_ms) {
    toast_post(text, type, duration_ms);
}

void toast_dismiss(void) {
    if (!s_showing) return;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    toast_slide_out();
}

void toast_cancel_all(void) {
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    toast_dismiss();
}

bool toast_is_visible(void) {
    return s_showing;
}
