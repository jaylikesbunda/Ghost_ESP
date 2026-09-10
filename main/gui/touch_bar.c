#include "gui/touch_bar.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"

static void touch_bar_style_button(lv_obj_t *btn, bool round) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t ctrl = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_obj_set_style_bg_color(btn, ctrl, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, round ? LV_RADIUS_CIRCLE : 5, LV_PART_MAIN);
    if (!round) {
        lv_obj_set_style_pad_hor(btn, 8, LV_PART_MAIN);
    }
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
}

static lv_obj_t *touch_bar_create_button(lv_obj_t *bar, const char *label,
                                         bool round, lv_align_t align,
                                         lv_coord_t x_ofs, bool hidden) {
    lv_obj_t *btn = lv_btn_create(bar);
    if (!btn) return NULL;
    gui_apply_pressed_style(btn);
    int w = round ? GUI_TOUCH_BAR_BTN_SIZE : GUI_TOUCH_BAR_BTN_SIZE + 24;
    lv_obj_set_size(btn, w, GUI_TOUCH_BAR_BTN_SIZE);
    lv_obj_align(btn, align, x_ofs, 0);
    touch_bar_style_button(btn, round);
    lv_obj_t *lbl = lv_label_create(btn);
    if (lbl) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_label_set_text(lbl, label ? label : "");
        lv_obj_set_style_text_color(lbl, lv_color_hex(theme_palette_get_text(theme)), 0);
        lv_obj_center(lbl);
    }
    if (hidden) lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    return btn;
}

gui_touch_bar_t gui_touch_bar_create(lv_obj_t *parent) {
    gui_touch_bar_t tb = {0};
    if (!parent || !lv_obj_is_valid(parent)) return tb;
    if (!gui_touch_bar_should_show()) return tb;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));

    lv_obj_t *bar = lv_obj_create(parent);
    if (!bar) return tb;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LV_HOR_RES, GUI_TOUCH_BAR_HEIGHT);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, bg, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(bar);

    tb.bar = bar;
    tb.up_btn = touch_bar_create_button(bar, LV_SYMBOL_UP, true,
                                        LV_ALIGN_LEFT_MID, GUI_TOUCH_BAR_PADDING, true);
    tb.back_btn = touch_bar_create_button(bar, "Back", false,
                                          LV_ALIGN_CENTER, 0, false);
    tb.down_btn = touch_bar_create_button(bar, LV_SYMBOL_DOWN, true,
                                          LV_ALIGN_RIGHT_MID, -GUI_TOUCH_BAR_PADDING, true);
    return tb;
}

void gui_touch_bar_set_callbacks(gui_touch_bar_t *tb,
                                 lv_event_cb_t up_cb, void *up_user,
                                 lv_event_cb_t back_cb, void *back_user,
                                 lv_event_cb_t down_cb, void *down_user) {
    if (!tb) return;
    if (tb->up_btn && lv_obj_is_valid(tb->up_btn) && up_cb) {
        lv_obj_add_event_cb(tb->up_btn, up_cb, LV_EVENT_CLICKED, up_user);
    }
    if (tb->back_btn && lv_obj_is_valid(tb->back_btn) && back_cb) {
        lv_obj_add_event_cb(tb->back_btn, back_cb, LV_EVENT_CLICKED, back_user);
    }
    if (tb->down_btn && lv_obj_is_valid(tb->down_btn) && down_cb) {
        lv_obj_add_event_cb(tb->down_btn, down_cb, LV_EVENT_CLICKED, down_user);
    }
}

void gui_touch_bar_hide_arrows(gui_touch_bar_t *tb) {
    if (!tb) return;
    if (tb->up_btn && lv_obj_is_valid(tb->up_btn)) {
        lv_obj_add_flag(tb->up_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (tb->down_btn && lv_obj_is_valid(tb->down_btn)) {
        lv_obj_add_flag(tb->down_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

void gui_touch_bar_update_visibility(gui_touch_bar_t *tb, lv_obj_t *scrollable) {
    if (!tb) return;
    if (!scrollable || !lv_obj_is_valid(scrollable)) return;
    lv_obj_update_layout(scrollable);
    lv_coord_t top = lv_obj_get_scroll_top(scrollable);
    lv_coord_t bottom = lv_obj_get_scroll_bottom(scrollable);
    bool needs = (top > 0) || (bottom > 0);
    if (tb->up_btn && lv_obj_is_valid(tb->up_btn)) {
        if (needs) {
            lv_obj_clear_flag(tb->up_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(tb->up_btn);
        } else {
            lv_obj_add_flag(tb->up_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (tb->down_btn && lv_obj_is_valid(tb->down_btn)) {
        if (needs) {
            lv_obj_clear_flag(tb->down_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(tb->down_btn);
        } else {
            lv_obj_add_flag(tb->down_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (tb->back_btn && lv_obj_is_valid(tb->back_btn)) {
        lv_obj_move_foreground(tb->back_btn);
    }
}

void gui_touch_bar_scroll_page(lv_obj_t *scrollable, bool up) {
    if (!scrollable || !lv_obj_is_valid(scrollable)) return;
    lv_coord_t h = lv_obj_get_height(scrollable);
    lv_coord_t amt = h / 2;
    if (amt <= 0) return;
    lv_obj_scroll_by_bounded(scrollable, 0, up ? amt : -amt, LV_ANIM_OFF);
}

bool gui_touch_bar_hit(const lv_obj_t *btn, int x, int y) {
    if (!btn || !lv_obj_is_valid((lv_obj_t *)btn)) return false;
    if (lv_obj_has_flag(btn, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)btn, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

void gui_touch_bar_hit_test(const gui_touch_bar_t *tb, int x, int y,
                            bool *hit_up, bool *hit_back, bool *hit_down) {
    bool u = tb ? gui_touch_bar_hit(tb->up_btn, x, y) : false;
    bool b = tb ? gui_touch_bar_hit(tb->back_btn, x, y) : false;
    bool d = tb ? gui_touch_bar_hit(tb->down_btn, x, y) : false;
    if (hit_up) *hit_up = u;
    if (hit_back) *hit_back = b;
    if (hit_down) *hit_down = d;
}

void gui_touch_bar_destroy(gui_touch_bar_t *tb) {
    if (!tb) return;
    /* Buttons are children of bar; deleting bar cascades. NULL all. */
    if (tb->bar && lv_obj_is_valid(tb->bar)) lv_obj_del(tb->bar);
    tb->bar = NULL;
    tb->up_btn = NULL;
    tb->back_btn = NULL;
    tb->down_btn = NULL;
}

void gui_touch_bar_refresh_styles(gui_touch_bar_t *tb) {
    if (!tb || !tb->bar || !lv_obj_is_valid(tb->bar)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_bg_color(tb->bar,
                              lv_color_hex(theme_palette_get_background(theme)), 0);
    lv_obj_t *btns[3] = {tb->up_btn, tb->back_btn, tb->down_btn};
    for (int i = 0; i < 3; i++) {
        if (!btns[i] || !lv_obj_is_valid(btns[i])) continue;
        touch_bar_style_button(btns[i], i != 1);
        uint32_t cnt = lv_obj_get_child_cnt(btns[i]);
        for (uint32_t c = 0; c < cnt; c++) {
            lv_obj_t *ch = lv_obj_get_child(btns[i], (int32_t)c);
            if (ch) {
                lv_obj_set_style_text_color(ch,
                    lv_color_hex(theme_palette_get_text(theme)), 0);
            }
        }
    }
}
