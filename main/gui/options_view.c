#include "gui/options_view.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/asset_pack.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "gui/ios_toggle.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

typedef struct options_view_t {
    lv_obj_t *list;
    lv_style_t style_item;
    lv_style_t style_item_alt;
    lv_style_t style_item_pressed;
    lv_style_t style_item_alt_pressed;
    lv_style_t style_selected;
    lv_style_t style_row;
    lv_style_t style_label;
    lv_obj_t **items;
    int count;
    int capacity;
    int selected;
    int btn_h;
    bool use_asset_pack_background;
} options_view_t;

static inline bool ensure_capacity(options_view_t *ov, int need) {
    if (ov->capacity >= need) return true;
    int newcap = ov->capacity ? ov->capacity * 2 : 16;
    if (newcap < need) newcap = need;
    lv_obj_t **new_items = (lv_obj_t **)realloc(ov->items, sizeof(lv_obj_t *) * newcap);
    if (!new_items) return false;
    ov->items = new_items;
    ov->capacity = newcap;
    return true;
}

static inline lv_style_t *get_zebra_style(options_view_t *ov, int idx) {
    bool zebra = settings_get_zebra_menus_enabled(&G_Settings);
    if (!zebra) return &ov->style_item;
    return (idx % 2 == 0) ? &ov->style_item : &ov->style_item_alt;
}

static inline bool get_menu_rounded(void) {
    return settings_get_menu_rounded(&G_Settings);
}

/* Row height policy shared with options_screen.c so every menu-style list
 * agrees: 40 px on the small panels, 55 px on the normal ones, 32 px on the
 * 128 px Atom. Large panels keep their own control height. Views that need a
 * different row (hero rows, dense logs) call options_view_set_item_height()
 * before adding items. */
static int default_row_height(void) {
#if GUI_LARGE_SCREEN
    return GUI_CONTROL_H;
#elif defined(CONFIG_IS_ATOMS3R)
    return 32;
#else
    int w = LV_HOR_RES;
    int h = LV_VER_RES;
    bool small = (w <= 240 || h <= 240);
    return small ? 40 : 55;
#endif
}

static inline const lv_font_t *get_item_font(const options_view_t *ov) {
#if GUI_LARGE_SCREEN
    return accessibility_get_font_body();
#else
    uint8_t fs = settings_get_font_size(&G_Settings);
    if (ov->btn_h <= 40) {
        return fs == 0 ? &lv_font_montserrat_10 : (fs == 1 ? &lv_font_montserrat_12 : &lv_font_montserrat_14);
    }
    return fs == 0 ? &lv_font_montserrat_12 : (fs == 1 ? &lv_font_montserrat_14 : &lv_font_montserrat_16);
#endif
}

static inline void get_theme_surface_colors(lv_color_t *bg, lv_color_t *surface, lv_color_t *surface_alt, lv_color_t *text) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (bg) *bg = lv_color_hex(theme_palette_get_background(theme));
    if (surface) *surface = lv_color_hex(theme_palette_get_surface(theme));
    if (surface_alt) *surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    if (text) *text = lv_color_hex(theme_palette_get_text(theme));
}

static void apply_selected_style(options_view_t *ov, lv_obj_t *item, bool on) {
    if (!item || !lv_obj_is_valid(item)) return;

    uint32_t child_cnt = lv_obj_get_child_cnt(item);
    lv_obj_t *lbl = NULL;
    for (uint32_t i = 0; i < child_cnt; ++i) {
        lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
        if (!child) continue;
        if (lv_obj_get_user_data(child) == (void *)1) {
            lbl = child;
            break;
        }
    }
    if (!lbl && child_cnt > 0) {
        lbl = lv_obj_get_child(item, 0);
    }

    if (on) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t c = lv_color_hex(theme_palette_get_accent(theme));
        lv_color_t txt = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
        lv_style_set_bg_color(&ov->style_selected, c);
        lv_style_set_bg_grad_color(&ov->style_selected, c);
        lv_style_set_border_width(&ov->style_selected, 0);
        lv_obj_add_style(item, &ov->style_selected, 0);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, txt, 0);
            }
            if (ud == (void *)2) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_color_t normal_txt;
        get_theme_surface_colors(NULL, NULL, NULL, &normal_txt);
        lv_obj_remove_style(item, &ov->style_selected, 0);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, normal_txt, 0);
            }
            if (ud == (void *)2) {
#ifdef CONFIG_USE_TOUCHSCREEN
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
#else
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
#endif
            }
        }
    }
}

static options_view_t *options_view_create_internal(lv_obj_t *parent, const char *title,
                                                     bool use_asset_pack_background) {
    if (!parent) parent = lv_scr_act();
    options_view_t *ov = (options_view_t *)calloc(1, sizeof(options_view_t));
    if (!ov) return NULL;
    ov->use_asset_pack_background = use_asset_pack_background;

    int h = LV_VER_RES;
    int status_bar_h = GUI_STATUS_BAR_H;
    ov->btn_h = default_row_height();

    lv_color_t bg, surface, surface_alt, text;
    get_theme_surface_colors(&bg, &surface, &surface_alt, &text);

    ov->list = lv_list_create(parent);
    int list_w = GUI_OPTIONS_LIST_WIDTH;
    lv_obj_set_size(ov->list, list_w, h - status_bar_h);
    lv_obj_align(ov->list, LV_ALIGN_TOP_MID, 0, status_bar_h);
    lv_obj_set_style_bg_color(ov->list, bg, 0);
    lv_obj_set_style_bg_opa(ov->list,
                            ov->use_asset_pack_background && asset_pack_get_background_tile()
                                ? LV_OPA_TRANSP : LV_OPA_COVER,
                            0);
    lv_obj_set_style_pad_left(ov->list, GUI_OPTIONS_LIST_PAD_HOR, 0);
    lv_obj_set_style_pad_right(ov->list, GUI_OPTIONS_LIST_PAD_HOR, 0);
    lv_obj_set_style_pad_top(ov->list, GUI_SAFEAREA_VER, 0);
    lv_obj_set_style_pad_bottom(ov->list, GUI_SAFEAREA_VER + GUI_HOME_SAFE_H, 0);
    lv_obj_set_style_border_width(ov->list, 0, 0);
    lv_obj_set_style_radius(ov->list, 0, 0);

    bool rounded = get_menu_rounded();
    lv_coord_t item_radius = rounded ? GUI_RADIUS_SM : 0;

    lv_obj_set_style_pad_row(ov->list, GUI_GRID, 0);

    lv_style_init(&ov->style_item);
    lv_style_set_bg_color(&ov->style_item, surface);
    lv_style_set_bg_opa(&ov->style_item, LV_OPA_COVER);
    lv_style_set_border_width(&ov->style_item, 0);
    lv_style_set_radius(&ov->style_item, item_radius);

    lv_style_init(&ov->style_item_alt);
    lv_style_set_bg_color(&ov->style_item_alt, surface_alt);
    lv_style_set_bg_opa(&ov->style_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&ov->style_item_alt, 0);
    lv_style_set_radius(&ov->style_item_alt, item_radius);

    lv_style_init(&ov->style_item_pressed);
    lv_style_set_bg_color(&ov->style_item_pressed, lv_color_darken(surface, LV_OPA_30));
    lv_style_set_bg_opa(&ov->style_item_pressed, LV_OPA_COVER);
    lv_style_set_transform_width(&ov->style_item_pressed, 0);
    lv_style_set_transform_height(&ov->style_item_pressed, 0);

    lv_style_init(&ov->style_item_alt_pressed);
    lv_style_set_bg_color(&ov->style_item_alt_pressed, lv_color_darken(surface_alt, LV_OPA_30));
    lv_style_set_bg_opa(&ov->style_item_alt_pressed, LV_OPA_COVER);
    lv_style_set_transform_width(&ov->style_item_alt_pressed, 0);
    lv_style_set_transform_height(&ov->style_item_alt_pressed, 0);

    lv_style_init(&ov->style_selected);
    lv_style_set_bg_opa(&ov->style_selected, LV_OPA_COVER);
    lv_style_set_radius(&ov->style_selected, item_radius);
    lv_style_set_bg_grad_dir(&ov->style_selected, LV_GRAD_DIR_NONE);

    /* Row geometry is identical for every item, so it lives in one shared
     * style instead of a per-item local style. */
    lv_style_init(&ov->style_row);
    lv_style_set_height(&ov->style_row, ov->btn_h);
    lv_style_set_pad_top(&ov->style_row, 0);
    lv_style_set_pad_bottom(&ov->style_row, 0);
    lv_style_set_pad_left(&ov->style_row, GUI_SAFEAREA_HOR);
    lv_style_set_pad_right(&ov->style_row, GUI_SAFEAREA_VER);
    lv_style_set_flex_flow(&ov->style_row, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&ov->style_row, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&ov->style_row, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&ov->style_row, LV_FLEX_ALIGN_CENTER);

    lv_style_init(&ov->style_label);
    lv_style_set_text_font(&ov->style_label, get_item_font(ov));
    lv_style_set_text_align(&ov->style_label, LV_TEXT_ALIGN_LEFT);
    lv_style_set_text_color(&ov->style_label, text);
    lv_style_set_width(&ov->style_label, LV_PCT(100));

    ov->selected = -1;

    if (title && *title) display_manager_add_status_bar(title);

    return ov;
}

options_view_t *options_view_create(lv_obj_t *parent, const char *title) {
    return options_view_create_internal(parent, title, true);
}

options_view_t *options_view_create_no_bg(lv_obj_t *parent, const char *title) {
    return options_view_create_internal(parent, title, false);
}

void options_view_destroy(options_view_t *ov) {
    if (!ov) return;
    if (ov->list && lv_obj_is_valid(ov->list)) lv_obj_del(ov->list);
    lv_style_reset(&ov->style_item);
    lv_style_reset(&ov->style_item_alt);
    lv_style_reset(&ov->style_item_pressed);
    lv_style_reset(&ov->style_item_alt_pressed);
    lv_style_reset(&ov->style_selected);
    lv_style_reset(&ov->style_row);
    lv_style_reset(&ov->style_label);
    free(ov->items);
    free(ov);
}

lv_obj_t *options_view_add_item(options_view_t *ov, const char *label, lv_event_cb_t on_click, void *user_data) {
    if (!ov || !ov->list) return NULL;
    if (!ensure_capacity(ov, ov->count + 1)) return NULL;
    lv_obj_t *btn = lv_list_add_btn(ov->list, NULL, label ? label : "");
    if (!btn) return NULL;
    /* lv_list_add_btn() sets a LOCAL height of LV_SIZE_CONTENT, and local
     * styles outrank the shared row style, so the height must be overridden
     * locally for the row to have any fixed height at all. */
    lv_obj_set_height(btn, ov->btn_h);
    lv_style_t *zebra = get_zebra_style(ov, ov->count);
    lv_obj_add_style(btn, &ov->style_row, 0);
    lv_obj_add_style(btn, zebra, 0);
    lv_obj_add_style(btn, zebra == &ov->style_item ? &ov->style_item_pressed
                                                   : &ov->style_item_alt_pressed,
                     LV_STATE_PRESSED);
    if (on_click) lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) {
        lv_obj_add_style(lbl, &ov->style_label, 0);
        lv_label_set_recolor(lbl, true);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_user_data(lbl, (void *)1);
    }
    ov->items[ov->count++] = btn;
    if (ov->selected < 0) {
        ov->selected = 0;
        apply_selected_style(ov, ov->items[0], true);
    }
    return btn;
}

void options_view_add_items(options_view_t *ov, const char **labels, lv_event_cb_t on_click, void *user_data) {
    if (!ov || !labels) return;
    for (int i = 0; labels[i]; ++i) {
        options_view_add_item(ov, labels[i], on_click, user_data);
    }
}

lv_obj_t *options_view_add_back_row(options_view_t *ov, lv_event_cb_t on_click, void *user_data) {
    return options_view_add_item(ov, LV_SYMBOL_LEFT " Back", on_click, user_data);
}

void options_view_trigger_wipe(options_view_t *ov) {
    if (!ov || ov->count <= 0) return;
    gui_anim_list_wipe(ov->list, ov->items, ov->count, GUI_ANIM_TRANSITION);
}

void options_view_set_selected(options_view_t *ov, int index) {
    if (!ov || ov->count == 0) return;
    if (index < 0) index = ov->count - 1;
    if (index >= ov->count) index = 0;
    if (ov->selected == index) return;
    if (ov->selected >= 0 && ov->selected < ov->count) {
        apply_selected_style(ov, ov->items[ov->selected], false);
    }
    ov->selected = index;
    apply_selected_style(ov, ov->items[ov->selected], true);
    lv_obj_scroll_to_view(ov->items[ov->selected], LV_ANIM_OFF);
}

void options_view_set_item_height(options_view_t *ov, int height) {
    if (!ov || height <= 0 || ov->btn_h == height) return;
    ov->btn_h = height;
    lv_style_set_height(&ov->style_row, height);
    lv_style_set_text_font(&ov->style_label, get_item_font(ov));
    for (int i = 0; i < ov->count; ++i) {
        lv_obj_t *item = ov->items[i];
        if (!item || !lv_obj_is_valid(item)) continue;
        lv_obj_set_height(item, height);
    }
    lv_obj_report_style_change(&ov->style_row);
    lv_obj_report_style_change(&ov->style_label);
}

void options_view_move_selection(options_view_t *ov, int delta) {
    if (!ov || ov->count == 0) return;
    options_view_set_selected(ov, ov->selected + delta);
}

int options_view_get_selected(const options_view_t *ov) {
    return ov ? ov->selected : -1;
}

void options_view_update_item_text(options_view_t *ov, int index, const char *new_text) {
    if (!ov || index < 0 || index >= ov->count) return;
    lv_obj_t *btn = ov->items[index];
    lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : NULL;
    if (lbl) lv_label_set_text(lbl, new_text ? new_text : "");
}

void options_view_clear(options_view_t *ov) {
    if (!ov || !ov->list) return;
    lv_obj_clean(ov->list);
    for (int i = 0; i < ov->count; ++i) {
        ov->items[i] = NULL;
    }
    ov->count = 0;
    ov->selected = -1;
}

int options_view_get_item_count(const options_view_t *ov) {
    return ov ? ov->count : 0;
}

lv_obj_t *options_view_get_list(options_view_t *ov) {
    return ov ? ov->list : NULL;
}

void options_view_set_title(options_view_t *ov, const char *title) {
    (void)ov;
    if (title && *title) display_manager_add_status_bar(title);
}

void options_view_refresh_styles(options_view_t *ov) {
    if (!ov) return;

    lv_color_t bg, surface, surface_alt, text;
    get_theme_surface_colors(&bg, &surface, &surface_alt, &text);

    bool rounded = get_menu_rounded();
    lv_coord_t item_radius = rounded ? GUI_RADIUS_SM : 0;

    if (ov->list && lv_obj_is_valid(ov->list)) {
        lv_obj_set_style_bg_color(ov->list, bg, 0);
        lv_obj_set_style_bg_opa(ov->list,
                                ov->use_asset_pack_background && asset_pack_get_background_tile()
                                    ? LV_OPA_TRANSP : LV_OPA_COVER,
                                0);
        lv_obj_set_style_pad_row(ov->list, GUI_GRID, 0);
        lv_obj_set_style_pad_left(ov->list, GUI_OPTIONS_LIST_PAD_HOR, 0);
        lv_obj_set_style_pad_right(ov->list, GUI_OPTIONS_LIST_PAD_HOR, 0);
        lv_obj_set_style_pad_top(ov->list, GUI_SAFEAREA_VER, 0);
        lv_obj_set_style_pad_bottom(ov->list, GUI_SAFEAREA_VER, 0);
    }

    lv_style_set_bg_color(&ov->style_item, surface);
    lv_style_set_radius(&ov->style_item, item_radius);
    lv_style_set_bg_color(&ov->style_item_alt, surface_alt);
    lv_style_set_radius(&ov->style_item_alt, item_radius);
    lv_style_set_radius(&ov->style_selected, item_radius);

    lv_style_set_bg_color(&ov->style_item_pressed, lv_color_darken(surface, LV_OPA_30));
    lv_style_set_radius(&ov->style_item_pressed, item_radius);
    lv_style_set_bg_color(&ov->style_item_alt_pressed, lv_color_darken(surface_alt, LV_OPA_30));
    lv_style_set_radius(&ov->style_item_alt_pressed, item_radius);
    lv_style_set_text_font(&ov->style_label, get_item_font(ov));
    lv_style_set_text_color(&ov->style_label, text);
    lv_obj_report_style_change(&ov->style_label);
    lv_obj_report_style_change(&ov->style_row);

    for (int i = 0; i < ov->count; ++i) {
        lv_obj_t *btn = ov->items[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        lv_obj_remove_style(btn, &ov->style_item, 0);
        lv_obj_remove_style(btn, &ov->style_item_alt, 0);
        lv_obj_remove_style(btn, &ov->style_item_pressed, LV_STATE_PRESSED);
        lv_obj_remove_style(btn, &ov->style_item_alt_pressed, LV_STATE_PRESSED);
        lv_style_t *zebra = get_zebra_style(ov, i);
        lv_obj_add_style(btn, zebra, 0);
        lv_obj_add_style(btn, zebra == &ov->style_item ? &ov->style_item_pressed
                                                       : &ov->style_item_alt_pressed,
                         LV_STATE_PRESSED);

        uint32_t child_cnt = lv_obj_get_child_cnt(btn);
        for (uint32_t j = 0; j < child_cnt; ++j) {
            lv_obj_t *child = lv_obj_get_child(btn, (int32_t)j);
            if (!child || !lv_obj_is_valid(child)) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, text, 0);
            } else if (ud == IOS_TOGGLE_USER_DATA) {
                // Theme change: re-apply the toggle's on-state color.
                ios_toggle_refresh_style(child);
            }
        }
    }
    for (int i = 0; i < ov->count; ++i) {
        apply_selected_style(ov, ov->items[i], i == ov->selected);
    }
}

void options_view_relayout_item(options_view_t *ov, lv_obj_t *item) {
    if (!ov || !item || !lv_obj_is_valid(item)) return;
    lv_obj_set_style_pad_top(item, 0, 0);
    lv_obj_set_style_pad_bottom(item, 0, 0);
    lv_obj_t *lbl = lv_obj_get_child(item, 0);
    if (!lbl) return;
    const lv_font_t *font = get_item_font(ov);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_coord_t left_pad = GUI_SAFEAREA_HOR;
    lv_obj_set_width(lbl, lv_obj_get_width(item) - left_pad - GUI_SAFEAREA_VER);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, left_pad, 0);
}

void options_view_refresh_selected_item(options_view_t *ov) {
    if (!ov || ov->selected < 0 || ov->selected >= ov->count) return;
    apply_selected_style(ov, ov->items[ov->selected], true);
}
