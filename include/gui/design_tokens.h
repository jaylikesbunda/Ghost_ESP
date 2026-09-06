#ifndef GUI_DESIGN_TOKENS_H
#define GUI_DESIGN_TOKENS_H

#include "lvgl.h"
#include "gui/ui_capabilities.h"

#if GUI_LARGE_SCREEN
#define GUI_GRID             8
#define GUI_STATUS_BAR_H     44
#define GUI_CONTROL_H        64
#define GUI_CONTENT_MAX_W    880
#define GUI_OPTIONS_LIST_WIDTH LV_HOR_RES
#define GUI_OPTIONS_LIST_PAD_HOR ((LV_HOR_RES > GUI_CONTENT_MAX_W) ? ((LV_HOR_RES - GUI_CONTENT_MAX_W) / 2) : GUI_SAFEAREA_HOR)
#define GUI_HOME_SAFE_H      48
#define GUI_LEGACY_TOUCH_BAR 0
#define GUI_RADIUS_SM        12
#define GUI_RADIUS_MD        18
#define GUI_RADIUS_LG        24
#else
#define GUI_GRID             4
#if defined(CONFIG_CROWPANEL_1P28_ROTARY)
/* The visible aperture is round: reserve enough top space for generic views
 * to begin below the centered status pill rather than behind its lower edge. */
#define GUI_STATUS_BAR_H     36
#else
#define GUI_STATUS_BAR_H     24
#endif
#define GUI_CONTROL_H        48
#define GUI_CONTENT_MAX_W    LV_HOR_RES
#define GUI_OPTIONS_LIST_WIDTH LV_MIN(LV_HOR_RES, GUI_CONTENT_MAX_W)
#define GUI_OPTIONS_LIST_PAD_HOR GUI_SAFEAREA_HOR
#define GUI_HOME_SAFE_H      0
#define GUI_RADIUS_SM        8
#define GUI_RADIUS_MD        12
#define GUI_RADIUS_LG        16
#define GUI_LEGACY_TOUCH_BAR 1
#endif
#define GUI_SAFEAREA_HOR    (GUI_GRID * 4)
#define GUI_SAFEAREA_VER    (GUI_GRID * 2)

#if GUI_EPAPER
#define GUI_ANIM_TRANSITION  0
#define GUI_ANIM_INTERACT    0
#define GUI_ANIM_MICRO       0
#define GUI_ANIM_BREATHE     0
#else
#define GUI_ANIM_TRANSITION  300
#define GUI_ANIM_INTERACT    200
#define GUI_ANIM_MICRO       120
#define GUI_ANIM_BREATHE     2000
#endif

#define GUI_INDICATOR_WIDTH  3
#define GUI_INDICATOR_RADIUS 2

#define GUI_SHAKE_AMPLITUDE  4
#define GUI_SHAKE_CYCLES     3
#define GUI_SHAKE_PERIOD_MS  60

static lv_color_filter_dsc_t gui_pressed_dark_filter_dsc;

static inline lv_color_t gui_pressed_dark_filter_cb(const lv_color_filter_dsc_t *dsc, lv_color_t color, lv_opa_t opa) {
    (void)dsc;
    return lv_color_darken(color, opa);
}

static inline void gui_apply_pressed_style(lv_obj_t *obj) {
    lv_color_filter_dsc_init(&gui_pressed_dark_filter_dsc, gui_pressed_dark_filter_cb);
    lv_obj_set_style_color_filter_dsc(obj, &gui_pressed_dark_filter_dsc, LV_STATE_PRESSED);
    lv_obj_set_style_color_filter_opa(obj, 35, LV_STATE_PRESSED);
#if GUI_LARGE_SCREEN
    /* Keep large-panel press feedback on the normal framebuffer path. Scaling the
     * parent tile makes LVGL render its icon/label into a temporary layer;
     * that path can drop the contents while pressed. Darkening above gives
     * feedback without a transformed layer or per-press allocation. */
    lv_obj_set_style_transform_zoom(obj, 256, LV_STATE_PRESSED);
    lv_obj_set_style_transform_zoom(obj, 256, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(obj, LV_PCT(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(obj, LV_PCT(50), LV_PART_MAIN);
#endif
}

static inline const lv_font_t *gui_font_title(void) {
#if GUI_LARGE_SCREEN
    return &lv_font_montserrat_24;
#else
    return &lv_font_montserrat_16;
#endif
}

static inline const lv_font_t *gui_font_body(void) {
#if GUI_LARGE_SCREEN
    return &lv_font_montserrat_18;
#else
    return &lv_font_montserrat_14;
#endif
}

static inline const lv_font_t *gui_font_caption(void) {
#if GUI_LARGE_SCREEN
    return &lv_font_montserrat_16;
#else
    return &lv_font_montserrat_12;
#endif
}

static inline const lv_font_t *gui_font_micro(void) {
#if GUI_LARGE_SCREEN
    return &lv_font_montserrat_14;
#else
    return &lv_font_montserrat_10;
#endif
}

static inline const lv_font_t *gui_font_for_height(lv_coord_t h) {
#if GUI_LARGE_SCREEN
    if (h <= 40) return &lv_font_montserrat_14;
    if (h <= 55) return &lv_font_montserrat_18;
    return &lv_font_montserrat_24;
#else
    if (h <= 40) return &lv_font_montserrat_12;
    if (h <= 55) return &lv_font_montserrat_14;
    return &lv_font_montserrat_16;
#endif
}

#endif
