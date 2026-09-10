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

    /* Virtual (windowed) list state. When virtual_mode is set, `items` holds a
     * fixed pool of rows and `count` equals the pool size; the backing data is
     * described by the callbacks instead of one object per item. */
    bool virtual_mode;
    int visible_rows;
    int window_start;
    int virtual_count;
    int virtual_selected;
    options_view_count_fn virtual_count_fn;
    options_view_fill_fn virtual_fill_fn;
    options_view_activate_fn virtual_activate_fn;
    void *virtual_user_data;
} options_view_t;

static void virtual_bind_rows(options_view_t *ov);
static void virtual_row_event_cb(lv_event_t *e);
static int compute_visible_rows(const options_view_t *ov);

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

/* Row height presets offered by Settings. Percentages are used rather than
 * absolute pixels so the proportional design intent survives on both the
 * small panels and the large ones. */
static const uint8_t k_row_height_percent[MENU_ROW_HEIGHT_OPTION_COUNT] = {80, 100, 120, 140};

int options_view_scale_row_height(int base) {
    if (base <= 0) return base;
    uint8_t preset = settings_get_row_height(&G_Settings);
    if (preset >= MENU_ROW_HEIGHT_OPTION_COUNT) preset = 1;
    int scaled = (base * k_row_height_percent[preset] + 50) / 100;
    if (scaled < 24) scaled = 24;
    if (scaled > 120) scaled = 120;
    return scaled;
}

/* Row height policy shared with options_screen.c so every menu-style list
 * agrees: 40 px on the small panels, 55 px on the normal ones, 32 px on the
 * 128 px Atom. Large panels keep their own control height. The user's Row
 * Height setting then scales whichever base applies. Views that need a
 * different row (hero rows, dense logs) call options_view_set_item_height()
 * before adding items. */
static int default_row_height(void) {
#if GUI_LARGE_SCREEN
    return options_view_scale_row_height(GUI_CONTROL_H);
#elif defined(CONFIG_IS_ATOMS3R)
    return options_view_scale_row_height(32);
#else
    int w = LV_HOR_RES;
    int h = LV_VER_RES;
    bool small = (w <= 240 || h <= 240);
    return options_view_scale_row_height(small ? 40 : 55);
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
    if (ov->virtual_mode) {
        options_view_virtual_select(ov, index);
        return;
    }
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
    if (ov->virtual_mode) {
        options_view_virtual_move(ov, delta);
        return;
    }
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
    ov->virtual_mode = false;
    ov->visible_rows = 0;
    ov->window_start = 0;
    ov->virtual_count = 0;
    ov->virtual_selected = 0;
}

int options_view_get_item_count(const options_view_t *ov) {
    if (!ov) return 0;
    return ov->virtual_mode ? ov->virtual_count : ov->count;
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
    if (!ov) return;
    if (ov->virtual_mode) {
        int row = ov->virtual_selected - ov->window_start;
        if (row >= 0 && row < ov->count) apply_selected_style(ov, ov->items[row], true);
        return;
    }
    if (ov->selected < 0 || ov->selected >= ov->count) return;
    apply_selected_style(ov, ov->items[ov->selected], true);
}

/* ---------------------------------------------------------------------------
 * Virtual (windowed) lists
 * ------------------------------------------------------------------------- */

#define OPTIONS_VIEW_LABEL_CHARS 128

/* Zebra striping follows the item index, not the pool row, so the alternating
 * pattern stays stable while rows are rebound on scroll. */
static void virtual_apply_row_style(options_view_t *ov, lv_obj_t *row, int index) {
    lv_obj_remove_style(row, &ov->style_item, 0);
    lv_obj_remove_style(row, &ov->style_item_alt, 0);
    lv_obj_remove_style(row, &ov->style_item_pressed, LV_STATE_PRESSED);
    lv_obj_remove_style(row, &ov->style_item_alt_pressed, LV_STATE_PRESSED);
    lv_style_t *zebra = get_zebra_style(ov, index);
    lv_obj_add_style(row, zebra, 0);
    lv_obj_add_style(row, zebra == &ov->style_item ? &ov->style_item_pressed
                                                   : &ov->style_item_alt_pressed,
                     LV_STATE_PRESSED);
}

/* How many rows the list can show at once. Derived from the list geometry and
 * the current row height so it tracks the Row Height setting and panel size
 * instead of a per-menu constant. */
static int compute_visible_rows(const options_view_t *ov) {
    if (!ov || !ov->list || !lv_obj_is_valid(ov->list)) return 1;

    /* An object's height lives in its coordinates, which are only valid after a
     * layout pass. Force one so the pool size is right the first time the view
     * is shown rather than collapsing to a single row. */
    lv_obj_update_layout(ov->list);

    lv_coord_t list_h = lv_obj_get_height(ov->list);
    if (list_h <= 0) list_h = LV_VER_RES - GUI_STATUS_BAR_H;

    lv_coord_t avail = list_h
                     - lv_obj_get_style_pad_top(ov->list, 0)
                     - lv_obj_get_style_pad_bottom(ov->list, 0);
    lv_coord_t gap = lv_obj_get_style_pad_row(ov->list, 0);
    lv_coord_t row_h = ov->btn_h > 0 ? ov->btn_h : 1;
    if (gap < 0) gap = 0;

    /* Round UP so the pool includes the partially visible row at the bottom of
     * the viewport. Sizing to whole rows only would end the list short and
     * leave dead space below it; a scrolling list clips that last row at the
     * viewport edge instead, and the row pool has to do the same to look right.
     * The extra row costs one more LVGL object, never more, so the object count
     * stays constant in the number of items. */
    lv_coord_t stride = row_h + gap;
    int rows = (avail <= 0) ? 1 : (int)((avail - 1) / stride) + 1;
    if (rows < 1) rows = 1;
    return rows;
}

static void virtual_bind_rows(options_view_t *ov) {
    if (!ov || !ov->virtual_mode) return;
    char buf[OPTIONS_VIEW_LABEL_CHARS];

    for (int r = 0; r < ov->visible_rows && r < ov->count; ++r) {
        lv_obj_t *row = ov->items[r];
        if (!row || !lv_obj_is_valid(row)) continue;
        lv_obj_t *lbl = lv_obj_get_child(row, 0);

        if (ov->virtual_count == 0) {
            if (r == 0) {
                if (lbl) lv_label_set_text(lbl, "No items found");
                lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_user_data(row, (void *)(intptr_t)-1);
                virtual_apply_row_style(ov, row, 0);
            } else {
                if (lbl) lv_label_set_text(lbl, "");
                lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_user_data(row, (void *)(intptr_t)-1);
            }
            continue;
        }

        int index = ov->window_start + r;
        if (index < ov->virtual_count) {
            buf[0] = '\0';
            if (ov->virtual_fill_fn) {
                ov->virtual_fill_fn(index, buf, sizeof(buf), ov->virtual_user_data);
            }
            if (lbl) lv_label_set_text(lbl, buf);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_user_data(row, (void *)(intptr_t)index);
            virtual_apply_row_style(ov, row, index);
        } else {
            if (lbl) lv_label_set_text(lbl, "");
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_user_data(row, (void *)(intptr_t)-1);
        }
    }
}

static void virtual_row_event_cb(lv_event_t *e) {
    lv_obj_t *row = lv_event_get_target(e);
    options_view_t *ov = (options_view_t *)lv_event_get_user_data(e);
    if (!ov || !ov->virtual_mode || !row) return;
    int index = (int)(intptr_t)lv_obj_get_user_data(row);
    if (index < 0 || index >= ov->virtual_count) return;
    options_view_virtual_select(ov, index);
    if (ov->virtual_activate_fn) ov->virtual_activate_fn(index, ov->virtual_user_data);
}

void options_view_virtual_start(options_view_t *ov,
                                options_view_count_fn count_fn,
                                options_view_fill_fn fill_fn,
                                options_view_activate_fn activate_fn,
                                void *user_data) {
    if (!ov || !ov->list) return;

    options_view_clear(ov);
    ov->virtual_mode = true;
    ov->virtual_count_fn = count_fn;
    ov->virtual_fill_fn = fill_fn;
    ov->virtual_activate_fn = activate_fn;
    ov->virtual_user_data = user_data;
    ov->window_start = 0;
    ov->virtual_selected = 0;
    ov->virtual_count = 0;

    ov->visible_rows = compute_visible_rows(ov);
    if (!ensure_capacity(ov, ov->visible_rows)) return;
    for (int i = 0; i < ov->visible_rows; ++i) {
        if (!options_view_add_item(ov, "", virtual_row_event_cb, ov)) break;
    }
    options_view_virtual_refresh(ov);
}

void options_view_virtual_refresh(options_view_t *ov) {
    if (!ov || !ov->virtual_mode) return;

    int n = ov->virtual_count_fn ? ov->virtual_count_fn(ov->virtual_user_data) : 0;
    if (n < 0) n = 0;
    ov->virtual_count = n;

    if (ov->virtual_selected > n - 1) ov->virtual_selected = (n > 0) ? n - 1 : 0;
    if (ov->virtual_selected < 0) ov->virtual_selected = 0;

    int max_start = n - ov->visible_rows;
    if (max_start < 0) max_start = 0;
    if (ov->virtual_selected < ov->window_start) ov->window_start = ov->virtual_selected;
    if (ov->window_start > max_start) ov->window_start = max_start;
    if (ov->window_start < 0) ov->window_start = 0;

    virtual_bind_rows(ov);
    ov->selected = ov->virtual_selected;
    for (int r = 0; r < ov->visible_rows; ++r) {
        lv_obj_t *row = ov->items[r];
        if (!row || !lv_obj_is_valid(row)) continue;
        apply_selected_style(ov, row, n > 0 && (ov->window_start + r) == ov->virtual_selected);
    }
}

void options_view_virtual_select(options_view_t *ov, int index) {
    if (!ov || !ov->virtual_mode || ov->virtual_count <= 0) return;
    if (index < 0) index = ov->virtual_count - 1;
    if (index >= ov->virtual_count) index = 0;

    ov->virtual_selected = index;
    int max_start = ov->virtual_count - ov->visible_rows;
    if (max_start < 0) max_start = 0;
    if (index < ov->window_start) {
        ov->window_start = index;
    } else if (index >= ov->window_start + ov->visible_rows) {
        ov->window_start = index - ov->visible_rows + 1;
    }
    if (ov->window_start > max_start) ov->window_start = max_start;
    if (ov->window_start < 0) ov->window_start = 0;

    virtual_bind_rows(ov);
    ov->selected = index;
    for (int r = 0; r < ov->visible_rows; ++r) {
        lv_obj_t *row = ov->items[r];
        if (!row || !lv_obj_is_valid(row)) continue;
        apply_selected_style(ov, row, (ov->window_start + r) == index);
    }
}

void options_view_virtual_move(options_view_t *ov, int delta) {
    if (!ov || !ov->virtual_mode) return;
    options_view_virtual_select(ov, ov->virtual_selected + delta);
}

bool options_view_is_virtual(const options_view_t *ov) {
    return ov ? ov->virtual_mode : false;
}

int options_view_virtual_count(const options_view_t *ov) {
    return (ov && ov->virtual_mode) ? ov->virtual_count : 0;
}

int options_view_virtual_visible_rows(const options_view_t *ov) {
    return (ov && ov->virtual_mode) ? ov->visible_rows : 0;
}

int options_view_virtual_selected(const options_view_t *ov) {
    return (ov && ov->virtual_mode) ? ov->virtual_selected : -1;
}
