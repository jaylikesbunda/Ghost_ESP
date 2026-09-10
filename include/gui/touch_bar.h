#pragma once

#include "lvgl.h"
#include "gui/design_tokens.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* In-tree standard bottom touch bar (Up / Back / Down).
 *
 * This is the single canonical implementation for firmware views.
 * It is intentionally NOT permission-gated (unlike plugin_api_ui_touch_bar_*,
 * which is for sandboxed plugins). All views under main/managers/views
 * should use this instead of copy-pasting lv_obj_create + 3x lv_btn_create.
 *
 * Geometry matches the long-standing convention:
 *   bar  = LV_HOR_RES x (28 + 3*2) = 34px, BOTTOM_MID
 *   up   = 28x28 CIRCLE, LEFT_MID + pad
 *   back = 52x28 r5, CENTER
 *   down = 28x28 CIRCLE, RIGHT_MID - pad
 * Colors come from the active theme palette (background / surface_alt / text).
 */

#define GUI_TOUCH_BAR_BTN_SIZE 28
#define GUI_TOUCH_BAR_PADDING 3
#define GUI_TOUCH_BAR_HEIGHT (GUI_TOUCH_BAR_BTN_SIZE + GUI_TOUCH_BAR_PADDING * 2)

/* Reserved height for layout math. Returns 0 when the bar is not shown
 * (no touchscreen, or large-screen where GUI_LEGACY_TOUCH_BAR == 0).
 * Use as: list_h = LV_VER_RES - GUI_STATUS_BAR_H - gui_touch_bar_height(); */
static inline int gui_touch_bar_height(void) {
#ifdef CONFIG_USE_TOUCHSCREEN
#if GUI_LEGACY_TOUCH_BAR
    return GUI_TOUCH_BAR_HEIGHT;
#else
    return 0;
#endif
#else
    return 0;
#endif
}

/* True when the bar should be created on this build/target. */
static inline bool gui_touch_bar_should_show(void) {
    return gui_touch_bar_height() > 0;
}

typedef struct {
    lv_obj_t *bar;
    lv_obj_t *up_btn;
    lv_obj_t *back_btn;
    lv_obj_t *down_btn;
} gui_touch_bar_t;

/* Create bar + 3 buttons. Up/down start HIDDEN (standard), back visible.
 * Returns zeroed struct when parent is NULL/invalid or bar should not show.
 * Caller owns the bar (child of parent); destroy with gui_touch_bar_destroy().
 * Callbacks are NOT attached; use gui_touch_bar_set_callbacks() or attach
 * manually with lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user). */
gui_touch_bar_t gui_touch_bar_create(lv_obj_t *parent);

/* Attach CLICKED callbacks. Any cb may be NULL (button kept, no handler).
 * Safe to call with NULL bar/buttons. */
void gui_touch_bar_set_callbacks(gui_touch_bar_t *tb,
                                 lv_event_cb_t up_cb, void *up_user,
                                 lv_event_cb_t back_cb, void *back_user,
                                 lv_event_cb_t down_cb, void *down_user);

/* Hide up/down permanently (Back-only variant, e.g. audio player).
 * Keeps layout identical, just never shows arrows. */
void gui_touch_bar_hide_arrows(gui_touch_bar_t *tb);

/* Show/hide up/down based on scrollable content overflow.
 * Mirrors the update_*_visibility() copies in badusb/badble/nfc/etc:
 * both arrows visible when content overflows, hidden otherwise.
 * Also move_foreground()s visible buttons (matches existing behavior). */
void gui_touch_bar_update_visibility(gui_touch_bar_t *tb, lv_obj_t *scrollable);

/* Scroll by half a page. up=true scrolls up (positive dy), else down.
 * No-op on NULL/invalid scrollable. */
void gui_touch_bar_scroll_page(lv_obj_t *scrollable, bool up);

/* Manual hit-test for views that do PR-stage dispatch alongside LVGL CLICKED
 * (drag discrimination). Returns true when point is inside the given button
 * (and button is valid + not hidden). */
bool gui_touch_bar_hit(const lv_obj_t *btn, int x, int y);

/* Convenience: hit-test all three buttons. Any out-param may be NULL. */
void gui_touch_bar_hit_test(const gui_touch_bar_t *tb, int x, int y,
                            bool *hit_up, bool *hit_back, bool *hit_down);

/* Delete bar (cascades to buttons) and NULL all pointers. */
void gui_touch_bar_destroy(gui_touch_bar_t *tb);

/* Re-apply theme colors to an existing bar (call on theme change). */
void gui_touch_bar_refresh_styles(gui_touch_bar_t *tb);

#ifdef __cplusplus
}
#endif
