#include "gui/progress_bar_view.h"

#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);

struct progress_bar_view_t {
    lv_obj_t *container;
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *track;
    lv_obj_t *fill;
    lv_obj_t *percent;
    lv_obj_t *subtext;
    lv_obj_t *touch_bar;
    gui_touch_bar_t tb;
    void (*on_cancel)(void *);
    void *cancel_user_data;
    bool active;
    bool indeterminate; // true while total size is unknown (sliding segment instead of a fake 0%)
};

static const lv_font_t *progress_title_font(void) {
    return LV_VER_RES <= 135 ? accessibility_get_font_body() : accessibility_get_font_title();
}

static const lv_font_t *progress_body_font(void) {
    return LV_VER_RES <= 135 ? accessibility_get_font_small() : accessibility_get_font_body();
}

/* Geometry alias: canonical value now lives in gui/touch_bar.h
 * (GUI_TOUCH_BAR_HEIGHT, same 34px). */
#define PROGRESS_TOUCH_BAR_H 34

static void progress_cancel_btn_cb(lv_event_t *e) {
    progress_bar_view_t *view = lv_event_get_user_data(e);
    if (view && view->on_cancel) view->on_cancel(view->cancel_user_data);
}

progress_bar_view_t *progress_bar_view_create(const char *title) {
    return progress_bar_view_create_with_cancel(title, NULL, NULL);
}

progress_bar_view_t *progress_bar_view_create_with_cancel(const char *title, void (*on_cancel)(void *), void *user_data) {
    progress_bar_view_t *view = calloc(1, sizeof(*view));
    if (!view) return NULL;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    view->container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(view->container, LV_PCT(100), LV_VER_RES - GUI_STATUS_BAR_H);
    lv_obj_set_pos(view->container, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(view->container, bg, 0);
    lv_obj_set_style_bg_opa(view->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->container, 0, 0);
    lv_obj_set_style_radius(view->container, 0, 0);
    lv_obj_set_style_pad_all(view->container, 0, 0);
    lv_obj_clear_flag(view->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(view->container, LV_OBJ_FLAG_CLICKABLE);

    display_manager_add_status_bar("Progress");
    display_manager_raise_status_bar();

    // Fill the screen edge-to-edge like the shared confirm/NFC popups instead
    // of a floating centered card.
    int card_w = LV_HOR_RES;
    int card_h = LV_VER_RES - GUI_STATUS_BAR_H;

    view->card = lv_obj_create(view->container);
    lv_obj_set_size(view->card, card_w, card_h);
    lv_obj_align(view->card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(view->card, surface, 0);
    lv_obj_set_style_bg_opa(view->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->card, 0, 0);
    lv_obj_set_style_radius(view->card, 0, 0);
    lv_obj_set_style_pad_top(view->card, GUI_GRID * 3, 0);
    lv_obj_set_style_pad_bottom(view->card, GUI_GRID * 3, 0);
    // Match the shared confirm/NFC popups' safe-area inset instead of the
    // tighter GUI_GRID*3 used for top/bottom, so the track/labels don't run
    // flush to the screen edge.
    lv_obj_set_style_pad_left(view->card, GUI_SAFEAREA_HOR, 0);
    lv_obj_set_style_pad_right(view->card, GUI_SAFEAREA_HOR, 0);
    lv_obj_set_style_pad_row(view->card, GUI_GRID * 2, 0);
    lv_obj_set_flex_flow(view->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(view->card, LV_OBJ_FLAG_SCROLLABLE);

    view->title = lv_label_create(view->card);
    lv_obj_set_width(view->title, LV_PCT(100));
    lv_obj_set_style_text_font(view->title, progress_title_font(), 0);
    lv_obj_set_style_text_color(view->title, text, 0);
    lv_obj_set_style_text_align(view->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view->title, LV_LABEL_LONG_WRAP);
    lv_label_set_text(view->title, title ? title : "Working");

    view->track = lv_obj_create(view->card);
    // Sized to 100% of the card's own content width (after its left/right
    // safe-area padding above) rather than a second, separately-computed
    // inset, so the track's margin can't drift out of sync with the card's.
    lv_obj_set_width(view->track, LV_PCT(100));
    lv_obj_set_height(view->track, GUI_GRID * 2);
    lv_obj_set_style_bg_color(view->track, bg, 0);
    lv_obj_set_style_bg_opa(view->track, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->track, 0, 0);
    lv_obj_set_style_radius(view->track, 999, 0);
    lv_obj_set_style_pad_all(view->track, 0, 0);
    lv_obj_clear_flag(view->track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(view->track, LV_ALIGN_CENTER);

    view->fill = lv_obj_create(view->track);
    lv_obj_set_height(view->fill, LV_PCT(100));
    lv_obj_set_width(view->fill, LV_PCT(0));
    lv_obj_set_pos(view->fill, 0, 0);
    lv_obj_set_align(view->fill, LV_ALIGN_LEFT_MID);
    lv_obj_set_style_bg_color(view->fill, accent, 0);
    lv_obj_set_style_bg_opa(view->fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->fill, 0, 0);
    lv_obj_set_style_radius(view->fill, 999, 0);
    lv_obj_set_style_pad_all(view->fill, 0, 0);
    lv_obj_clear_flag(view->fill, LV_OBJ_FLAG_SCROLLABLE);

    view->percent = lv_label_create(view->card);
    lv_obj_set_width(view->percent, LV_PCT(100));
    lv_obj_set_style_text_font(view->percent, progress_body_font(), 0);
    lv_obj_set_style_text_color(view->percent, text, 0);
    lv_obj_set_style_text_align(view->percent, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(view->percent, "0%");

    view->subtext = lv_label_create(view->card);
    lv_obj_set_width(view->subtext, LV_PCT(100));
    lv_obj_set_style_text_font(view->subtext, progress_body_font(), 0);
    lv_obj_set_style_text_color(view->subtext, text, 0);
    lv_obj_set_style_text_align(view->subtext, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(view->subtext, LV_LABEL_LONG_WRAP);
    lv_label_set_text(view->subtext, "");
    lv_obj_add_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);

    view->active = true;
    view->on_cancel = on_cancel;
    view->cancel_user_data = user_data;
    view->touch_bar = NULL;

    /* Back-only touch bar via helper (arrows permanently hidden).
     * Created even when on_cancel is NULL — an empty bar, same as before —
     * just with no callback attached. Gating is handled inside
     * gui_touch_bar_create(), so no touchscreen ifdefs are needed here. */
    view->tb = gui_touch_bar_create(view->container);
    view->touch_bar = view->tb.bar;
    if (view->touch_bar && lv_obj_is_valid(view->touch_bar)) {
        // Shrink the card to leave room for the touch bar
        lv_obj_set_height(view->card, card_h - gui_touch_bar_height());
        gui_touch_bar_hide_arrows(&view->tb);
        gui_touch_bar_set_callbacks(&view->tb, NULL, NULL,
                                    on_cancel ? progress_cancel_btn_cb : NULL, view,
                                    NULL, NULL);
    }

    return view;
}

void progress_bar_view_update(progress_bar_view_t *view, const char *title) {
    if (!view || !view->active || !view->title || !title) return;
    lv_label_set_text(view->title, title);
}

void progress_bar_view_set_subtext(progress_bar_view_t *view, const char *subtext) {
    if (!view || !view->active || !view->subtext) return;
    if (!subtext || subtext[0] == '\0') {
        lv_label_set_text(view->subtext, "");
        lv_obj_add_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(view->subtext, subtext);
    lv_obj_clear_flag(view->subtext, LV_OBJ_FLAG_HIDDEN);
}

static void progress_fill_width_anim_cb(void *var, int32_t v) {
    lv_obj_set_width((lv_obj_t *)var, v);
}

static void progress_indeterminate_x_anim_cb(void *var, int32_t v) {
    lv_obj_set_x((lv_obj_t *)var, v);
}

// Smoothly tween the fill's pixel width to `target_w` instead of snapping,
// so successive progress updates (even widely-spaced ones on a slow/fast
// link) read as continuous motion rather than jump cuts.
static void progress_bar_animate_fill_width(progress_bar_view_t *view, lv_coord_t target_w) {
    if (!view->fill) return;
    lv_anim_del(view->fill, progress_fill_width_anim_cb);
    lv_coord_t start_w = lv_obj_get_width(view->fill);
    if (start_w == target_w) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, view->fill);
    lv_anim_set_exec_cb(&a, progress_fill_width_anim_cb);
    lv_anim_set_values(&a, start_w, target_w);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Total size is unknown for a lot of these transfers (chunked/proxied
// downloads never send Content-Length), so a real percentage isn't possible.
// Rather than pin the bar at a misleading 0% for the whole transfer, slide a
// fixed-width segment back and forth to show it's actively working.
static void progress_bar_start_indeterminate(progress_bar_view_t *view) {
    if (view->indeterminate || !view->track || !view->fill) return;
    view->indeterminate = true;
    lv_anim_del(view->fill, progress_fill_width_anim_cb);
    lv_coord_t track_w = lv_obj_get_width(view->track);
    lv_coord_t seg_w = track_w > 48 ? track_w / 3 : track_w;
    lv_obj_set_width(view->fill, seg_w);
    lv_coord_t travel = track_w > seg_w ? track_w - seg_w : 0;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, view->fill);
    lv_anim_set_exec_cb(&a, progress_indeterminate_x_anim_cb);
    lv_anim_set_values(&a, 0, travel);
    lv_anim_set_time(&a, 850);
    lv_anim_set_playback_time(&a, 850);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void progress_bar_stop_indeterminate(progress_bar_view_t *view) {
    if (!view->indeterminate) return;
    view->indeterminate = false;
    if (view->fill) {
        lv_anim_del(view->fill, progress_indeterminate_x_anim_cb);
        lv_obj_set_pos(view->fill, 0, 0);
    }
}

void progress_bar_view_set_progress(progress_bar_view_t *view, size_t current, size_t total) {
    if (!view || !view->active) return;

    if (total == 0) {
        progress_bar_start_indeterminate(view);
        if (view->percent) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%u KB downloaded", (unsigned)(current / 1024));
            lv_obj_clear_flag(view->percent, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(view->percent, buf);
        }
        return;
    }

    progress_bar_stop_indeterminate(view);
    if (current > total) current = total;
    int pct = (int)((current * 100u) / total);
    if (view->fill && view->track) {
        lv_coord_t track_w = lv_obj_get_width(view->track);
        lv_coord_t target_w = (lv_coord_t)((int32_t)track_w * pct / 100);
        progress_bar_animate_fill_width(view, target_w);
    }
    if (view->percent) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d%%  %u / %u KB", pct, (unsigned)(current / 1024), (unsigned)(total / 1024));
        lv_obj_clear_flag(view->percent, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(view->percent, buf);
    }
}

// Working with nothing measurable to report (e.g. the install/extract phase
// after a download finishes): slide the bar to show activity, but leave the
// byte counter blank instead of pinning it at a meaningless "0 KB downloaded".
void progress_bar_view_set_indeterminate(progress_bar_view_t *view) {
    if (!view || !view->active) return;
    progress_bar_start_indeterminate(view);
    if (view->percent) {
        lv_label_set_text(view->percent, "");
        lv_obj_add_flag(view->percent, LV_OBJ_FLAG_HIDDEN);
    }
}

void progress_bar_view_close(progress_bar_view_t *view) {
    if (!view) return;
    view->active = false;
    if (view->container && lv_obj_is_valid(view->container)) {
        lv_obj_del(view->container);
    }
    display_manager_restore_status_bar();
    free(view);
}

bool progress_bar_view_is_active(const progress_bar_view_t *view) {
    return view ? view->active : false;
}
