#include "gui/detail_view.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static const char *DV_TAG = "dv_nav";

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);
bool theme_palette_is_bright(uint8_t theme);

static inline bool get_menu_rounded(void) {
    return settings_get_menu_rounded(&G_Settings);
}

typedef struct {
    lv_obj_t *obj;
    detail_row_type_t type;
    bool selectable;
} detail_row_t;

typedef struct {
    char *label;
    char *value;
} detail_info_item_t;

typedef enum {
    DETAIL_NAV_REGION_INFO = 0,
    DETAIL_NAV_REGION_ACTIONS = 1
} detail_nav_region_t;

struct detail_view_t {
    lv_obj_t *backdrop;
    lv_obj_t *container;
    lv_obj_t *info_panel;
    lv_obj_t *info_canvas;
    lv_obj_t *action_list;
    lv_style_t style_item;
    lv_style_t style_item_alt;
    lv_style_t style_selected;
    lv_style_t style_header;
    lv_style_t style_divider;
    detail_row_t *rows;
    int count;
    int capacity;
    int selected;
    int btn_h;
    int first_selectable;
    int info_count;
    detail_info_item_t *info_items;
    int info_capacity;
    lv_coord_t content_h;
    lv_coord_t item_radius;
    bool compact_layout;
    detail_nav_region_t nav_region;
    bool wrap_pending_down;
    bool wrap_pending_up;
    uint32_t last_step_ms;
};

static inline bool detail_view_should_use_compact_layout(int w, int h) {
    return (w > h && h <= 160);
}

static inline bool ensure_capacity(detail_view_t *dv, int need) {
    if (dv->capacity >= need) return true;
    int newcap = dv->capacity ? dv->capacity * 2 : 16;
    if (newcap < need) newcap = need;
    detail_row_t *new_rows = (detail_row_t *)realloc(dv->rows, sizeof(detail_row_t) * newcap);
    if (!new_rows) return false;
    dv->rows = new_rows;
    dv->capacity = newcap;
    return true;
}

static bool ensure_info_capacity(detail_view_t *dv, int need) {
    if (dv->info_capacity >= need) return true;
    int newcap = dv->info_capacity ? dv->info_capacity * 2 : 16;
    if (newcap < need) newcap = need;
    detail_info_item_t *new_items = (detail_info_item_t *)realloc(dv->info_items, sizeof(detail_info_item_t) * newcap);
    if (!new_items) return false;
    for (int i = dv->info_capacity; i < newcap; i++) {
        new_items[i].label = NULL;
        new_items[i].value = NULL;
    }
    dv->info_items = new_items;
    dv->info_capacity = newcap;
    return true;
}

static char *detail_strdup(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src) + 1;
    char *out = (char *)malloc(len);
    if (!out) return NULL;
    memcpy(out, src, len);
    return out;
}

static void detail_view_free_info_items(detail_view_t *dv) {
    if (!dv || !dv->info_items) return;
    for (int i = 0; i < dv->info_count; i++) {
        free(dv->info_items[i].label);
        free(dv->info_items[i].value);
        dv->info_items[i].label = NULL;
        dv->info_items[i].value = NULL;
    }
}

static inline lv_style_t *get_zebra_style(detail_view_t *dv, int idx) {
    bool zebra = settings_get_zebra_menus_enabled(&G_Settings);
    if (!zebra) return &dv->style_item;
    return (idx % 2 == 0) ? &dv->style_item : &dv->style_item_alt;
}

static inline const lv_font_t *get_item_font(const detail_view_t *dv) {
    if (dv->compact_layout) return accessibility_get_font_small();
    return (dv->btn_h <= 40) ? accessibility_get_font_body() : accessibility_get_font_title();
}

static inline const lv_font_t *get_value_font(const detail_view_t *dv) {
    if (dv->compact_layout) return accessibility_get_font_small();
    return (dv->btn_h <= 40) ? accessibility_get_font_body() : accessibility_get_font_title();
}

static inline lv_coord_t get_info_row_height(const detail_view_t *dv) {
    if (dv->compact_layout) return 14;
    int h = dv->btn_h - 10;
    if (h < 18) h = 18;
    return (lv_coord_t)h;
}

static inline lv_coord_t get_action_row_height(const detail_view_t *dv) {
    if (dv->compact_layout) return 20;
    return (lv_coord_t)(dv->btn_h + 4);
}

static inline lv_coord_t get_detail_row_display_height(const detail_view_t *dv, detail_row_type_t type) {
    switch (type) {
        case DETAIL_ROW_ACTION:
            return get_action_row_height(dv);
        case DETAIL_ROW_HEADER:
            return (lv_coord_t)dv->btn_h;
        case DETAIL_ROW_DIVIDER:
            return 2;
        case DETAIL_ROW_INFO:
        default:
            return 0;
    }
}

static lv_coord_t get_action_reserved_height(const detail_view_t *dv) {
    if (!dv || dv->count <= 0) return 0;

    int action_count = 0;
    for (int i = 0; i < dv->count; i++) {
        if (dv->rows[i].type == DETAIL_ROW_ACTION) action_count++;
    }
    if (action_count <= 0) return 0;

    int target_actions = action_count >= 2 ? 2 : 1;
    int seen_actions = 0;
    lv_coord_t reserved_h = 0;

    for (int i = 0; i < dv->count; i++) {
        if (dv->rows[i].type == DETAIL_ROW_INFO) continue;
        reserved_h += get_detail_row_display_height(dv, dv->rows[i].type);
        if (dv->rows[i].type == DETAIL_ROW_ACTION) {
            seen_actions++;
            if (seen_actions >= target_actions) break;
        }
    }

    lv_coord_t min_h = get_action_row_height(dv) * target_actions;
    if (reserved_h < min_h) reserved_h = min_h;

    lv_coord_t max_h = dv->compact_layout ? (dv->content_h * 3) / 5 : (dv->content_h * 2) / 5;
    if (reserved_h > max_h) reserved_h = max_h;

    lv_coord_t min_info_h = dv->info_count > 0 ? get_info_row_height(dv) : 0;
    lv_coord_t max_reserved_h = dv->content_h - min_info_h;
    if (max_reserved_h < 0) max_reserved_h = 0;
    if (reserved_h > max_reserved_h) reserved_h = max_reserved_h;

    return reserved_h;
}

static inline lv_coord_t get_info_panel_height(const detail_view_t *dv) {
    lv_coord_t info_h = get_info_row_height(dv) * dv->info_count;
    if (info_h < 0) info_h = 0;
    if (info_h == 0) return 0;

    lv_coord_t action_reserved_h = get_action_reserved_height(dv);
    lv_coord_t available_h = dv->content_h - action_reserved_h;
    if (available_h < get_info_row_height(dv)) available_h = get_info_row_height(dv);

    lv_coord_t cap = available_h;

    if (dv->compact_layout) {
        cap = (dv->content_h * 2) / 5;
        if (cap < 36) cap = 36;
        if (cap > 52) cap = 52;
        if (cap > available_h) cap = available_h;
    } else if (action_reserved_h > 0) {
        lv_coord_t split_cap = dv->content_h / 2;
        if (cap > split_cap) cap = split_cap;
    }

    if (cap < get_info_row_height(dv)) cap = get_info_row_height(dv);
    if (info_h > cap) info_h = cap;
    return info_h;
}

static inline lv_coord_t get_info_scroll_step(const detail_view_t *dv) {
    lv_coord_t step = get_info_row_height(dv) * 2;
    if (step < 12) step = 12;
    return step;
}

#define DETAIL_VIEW_WRAP_TIMEOUT_MS 500

static inline void detail_view_clear_wrap_pending(detail_view_t *dv) {
    dv->wrap_pending_down = false;
    dv->wrap_pending_up = false;
    dv->last_step_ms = 0;
}

static inline void detail_view_set_wrap_pending(detail_view_t *dv, bool down) {
    if (down) dv->wrap_pending_down = true;
    else dv->wrap_pending_up = true;
    dv->last_step_ms = lv_tick_get();
}

static inline bool detail_view_check_wrap_pending(detail_view_t *dv, bool down) {
    bool *flag = down ? &dv->wrap_pending_down : &dv->wrap_pending_up;
    if (!*flag) return false;
    if ((int32_t)(lv_tick_get() - dv->last_step_ms) > DETAIL_VIEW_WRAP_TIMEOUT_MS) {
        *flag = false;
        return false;
    }
    return true;
}

static inline void detail_view_clear_wrap_pending_down(detail_view_t *dv) {
    dv->wrap_pending_down = false;
    if (!dv->wrap_pending_up) dv->last_step_ms = 0;
}

static inline void detail_view_clear_wrap_pending_up(detail_view_t *dv) {
    dv->wrap_pending_up = false;
    if (!dv->wrap_pending_down) dv->last_step_ms = 0;
}

static inline bool detail_view_info_can_scroll(detail_view_t *dv) {
    if (!dv || !dv->info_panel || !lv_obj_is_valid(dv->info_panel)) return false;
    if (dv->container && lv_obj_is_valid(dv->container)) {
        lv_obj_update_layout(dv->container);
    }
    lv_obj_update_layout(dv->info_panel);
    return lv_obj_get_scroll_top(dv->info_panel) > 0 || lv_obj_get_scroll_bottom(dv->info_panel) > 0;
}

static inline bool detail_view_scroll_info(detail_view_t *dv, int dir) {
    if (!detail_view_info_can_scroll(dv)) return false;
    lv_coord_t before_top = lv_obj_get_scroll_top(dv->info_panel);
    lv_coord_t before_bottom = lv_obj_get_scroll_bottom(dv->info_panel);
    lv_coord_t step = get_info_scroll_step(dv);

    /* Whether this step actually moves anything has to be decided from the
     * pre-scroll room, not a post-scroll readback: with LV_ANIM_ON the
     * scroll position updates over the animation, so re-querying it right
     * after issuing the call would still see the old value. */
    lv_coord_t delta;
    bool changed;
    if (dir > 0) {
        delta = (before_bottom <= step) ? -before_bottom : -step;
        changed = before_bottom > 0;
    } else {
        delta = (before_top <= step) ? before_top : step;
        changed = before_top > 0;
    }

    lv_obj_scroll_by_bounded(dv->info_panel, 0, delta, LV_ANIM_ON);
    return changed;
}

static inline bool detail_view_info_at_bottom(detail_view_t *dv) {
    if (!dv || !dv->info_panel || !lv_obj_is_valid(dv->info_panel)) return true;
    lv_obj_update_layout(dv->info_panel);
    return lv_obj_get_scroll_bottom(dv->info_panel) <= 0;
}

static inline bool detail_view_info_at_top(detail_view_t *dv) {
    if (!dv || !dv->info_panel || !lv_obj_is_valid(dv->info_panel)) return true;
    lv_obj_update_layout(dv->info_panel);
    return lv_obj_get_scroll_top(dv->info_panel) <= 0;
}

static inline int detail_view_restore_selection(detail_view_t *dv) {
    if (!dv) return -1;
    if (dv->selected >= 0 && dv->selected < dv->count && dv->rows[dv->selected].selectable) {
        return dv->selected;
    }
    return dv->first_selectable;
}

static inline bool detail_view_wrap_to_actions(detail_view_t *dv) {
    if (!dv) return false;
    int restore = detail_view_restore_selection(dv);
    if (restore < 0) return false;
    detail_view_set_selected(dv, restore);
    return true;
}

static inline int detail_view_get_last_selectable(detail_view_t *dv) {
    if (!dv || dv->count <= 0) return -1;
    for (int i = dv->count - 1; i >= 0; --i) {
        if (dv->rows[i].selectable) return i;
    }
    return -1;
}

static inline void get_theme_colors(lv_color_t *bg, lv_color_t *surface, lv_color_t *surface_alt, lv_color_t *text, lv_color_t *accent) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (bg) *bg = lv_color_hex(theme_palette_get_background(theme));
    if (surface) *surface = lv_color_hex(theme_palette_get_surface(theme));
    if (surface_alt) *surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    if (text) *text = lv_color_hex(theme_palette_get_text(theme));
    if (accent) *accent = lv_color_hex(theme_palette_get_accent(theme));
}

static bool detail_area_intersect(lv_area_t *out, const lv_area_t *a, const lv_area_t *b) {
    out->x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    out->y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    out->x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    out->y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    return out->x1 <= out->x2 && out->y1 <= out->y2;
}

static void detail_view_sync_info_canvas(detail_view_t *dv) {
    if (!dv || !dv->info_canvas || !lv_obj_is_valid(dv->info_canvas)) return;
    lv_coord_t h = get_info_row_height(dv) * dv->info_count;
    if (h < 0) h = 0;
    lv_obj_set_height(dv->info_canvas, h);
    if (dv->info_panel && lv_obj_is_valid(dv->info_panel)) {
        lv_coord_t panel_h = get_info_panel_height(dv);
        lv_color_t accent;
        get_theme_colors(NULL, NULL, NULL, NULL, &accent);
        lv_obj_set_height(dv->info_panel, panel_h);
        lv_obj_set_style_border_width(dv->info_panel, panel_h > 0 && get_action_reserved_height(dv) > 0 ? 1 : 0, 0);
        lv_obj_set_style_border_side(dv->info_panel, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(dv->info_panel, accent, 0);
        lv_obj_set_style_border_opa(dv->info_panel, LV_OPA_40, 0);
        if (h > panel_h) {
            lv_obj_set_scrollbar_mode(dv->info_panel, LV_SCROLLBAR_MODE_AUTO);
        } else {
            lv_obj_scroll_to_y(dv->info_panel, 0, LV_ANIM_OFF);
            lv_obj_set_scrollbar_mode(dv->info_panel, LV_SCROLLBAR_MODE_OFF);
        }
    }
    lv_obj_invalidate(dv->info_canvas);
}

static void detail_view_info_draw_event(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    detail_view_t *dv = (detail_view_t *)lv_event_get_user_data(e);
    if (!obj || !dv || dv->info_count <= 0) return;

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) return;

    lv_area_t obj_coords;
    lv_obj_get_coords(obj, &obj_coords);

    lv_area_t panel_coords;
    if (!dv->info_panel || !lv_obj_is_valid(dv->info_panel)) return;
    lv_obj_get_coords(dv->info_panel, &panel_coords);

    lv_area_t visible_clip;
    if (!detail_area_intersect(&visible_clip, draw_ctx->clip_area, &panel_coords)) return;

    lv_color_t bg, surface, surface_alt, text;
    get_theme_colors(&bg, &surface, &surface_alt, &text, NULL);
    lv_color_t text_muted = lv_color_hex(theme_palette_get_text_muted(settings_get_menu_theme(&G_Settings)));

    lv_draw_rect_dsc_t row_dsc;
    lv_draw_rect_dsc_init(&row_dsc);
    row_dsc.bg_opa = LV_OPA_COVER;
    row_dsc.border_side = LV_BORDER_SIDE_NONE;
    row_dsc.border_width = 0;
    row_dsc.border_color = text;
    row_dsc.border_opa = LV_OPA_TRANSP;
    row_dsc.radius = 0;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = get_item_font(dv);
    label_dsc.color = text_muted;
    label_dsc.opa = LV_OPA_COVER;
    label_dsc.flag = LV_TEXT_FLAG_EXPAND;

    lv_draw_label_dsc_t value_dsc;
    lv_draw_label_dsc_init(&value_dsc);
    value_dsc.font = get_value_font(dv);
    value_dsc.color = text;
    value_dsc.opa = LV_OPA_COVER;
    value_dsc.flag = LV_TEXT_FLAG_EXPAND;

    lv_coord_t row_h = get_info_row_height(dv);
    lv_coord_t x_pad = dv->compact_layout ? 4 : 6;
    lv_coord_t row_w = lv_area_get_width(&obj_coords);
    lv_coord_t split_x = obj_coords.x1 + (row_w * (dv->compact_layout ? 34 : 40)) / 100;
    bool zebra = settings_get_zebra_menus_enabled(&G_Settings);

    for (int i = 0; i < dv->info_count; i++) {
        lv_coord_t y1 = obj_coords.y1 + i * row_h;
        lv_coord_t y2 = y1 + row_h - 1;
        if (y2 < visible_clip.y1 || y1 > visible_clip.y2) {
            continue;
        }

        lv_area_t row_area = {
            .x1 = obj_coords.x1,
            .y1 = y1,
            .x2 = obj_coords.x2,
            .y2 = y2
        };
        lv_area_t row_clip;
        if (!detail_area_intersect(&row_clip, &row_area, &visible_clip)) continue;
        const lv_area_t *old_clip = draw_ctx->clip_area;
        draw_ctx->clip_area = &row_clip;
        row_dsc.bg_color = (zebra && (i % 2 != 0)) ? surface_alt : bg;
        lv_draw_rect(draw_ctx, &row_dsc, &row_area);
        draw_ctx->clip_area = old_clip;

        const char *label = dv->info_items[i].label ? dv->info_items[i].label : "";
        const char *value = dv->info_items[i].value ? dv->info_items[i].value : "";
        bool full_width_value = label[0] == '\0';

        lv_coord_t label_x1 = obj_coords.x1 + x_pad;
        lv_coord_t label_x2 = split_x - x_pad;
        if (label_x2 < label_x1) label_x2 = label_x1;

        lv_coord_t value_x1 = full_width_value ? obj_coords.x1 + x_pad : split_x + x_pad;
        lv_coord_t value_x2 = obj_coords.x2 - x_pad;
        if (value_x2 < value_x1) value_x2 = value_x1;

        lv_coord_t label_max_w = label_x2 - label_x1 + 1;

        lv_point_t label_size;
        lv_txt_get_size(&label_size, label, label_dsc.font, label_dsc.letter_space, label_dsc.line_space,
                        label_max_w, label_dsc.flag);
        lv_coord_t label_y = y1 + (row_h - label_size.y) / 2;
        if (label_y < y1) label_y = y1;

        lv_area_t label_area = {
            .x1 = label_x1,
            .y1 = label_y,
            .x2 = label_x2,
            .y2 = y2
        };
        lv_area_t label_clip;
        if (detail_area_intersect(&label_clip, &label_area, &visible_clip)) {
            old_clip = draw_ctx->clip_area;
            draw_ctx->clip_area = &label_clip;
            lv_draw_label(draw_ctx, &label_dsc, &label_area, label, NULL);
            draw_ctx->clip_area = old_clip;
        }

        lv_point_t value_size;
        lv_txt_get_size(&value_size, value, value_dsc.font, value_dsc.letter_space, value_dsc.line_space,
                        LV_COORD_MAX, value_dsc.flag);
        lv_coord_t value_x = full_width_value ? value_x1 : value_x2 - value_size.x + 1;
        if (value_x < value_x1) value_x = value_x1;
        lv_coord_t value_y = y1 + (row_h - value_size.y) / 2;
        if (value_y < y1) value_y = y1;

        lv_area_t value_area = {
            .x1 = value_x,
            .y1 = value_y,
            .x2 = value_x2,
            .y2 = y2
        };
        lv_area_t value_clip;
        if (detail_area_intersect(&value_clip, &value_area, &visible_clip)) {
            old_clip = draw_ctx->clip_area;
            draw_ctx->clip_area = &value_clip;
            lv_draw_label(draw_ctx, &value_dsc, &value_area, value, NULL);
            draw_ctx->clip_area = old_clip;
        }
    }
}

static void apply_selected_style(detail_view_t *dv, lv_obj_t *item, bool on) {
    if (!item || !lv_obj_is_valid(item)) return;
    
    lv_color_t text, accent;
    get_theme_colors(NULL, NULL, NULL, &text, &accent);
    
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t sel_text = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
    
    if (on) {
        lv_style_set_bg_color(&dv->style_selected, accent);
        lv_style_set_bg_grad_color(&dv->style_selected, accent);
        lv_obj_add_style(item, &dv->style_selected, 0);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(item);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, sel_text, 0);
            }
            if (ud == (void *)2) {
                lv_obj_clear_flag(child, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_remove_style(item, &dv->style_selected, 0);
        
        uint32_t child_cnt = lv_obj_get_child_cnt(item);
        for (uint32_t i = 0; i < child_cnt; ++i) {
            lv_obj_t *child = lv_obj_get_child(item, (int32_t)i);
            if (!child) continue;
            void *ud = lv_obj_get_user_data(child);
            if (ud == (void *)1 || ud == (void *)2) {
                lv_obj_set_style_text_color(child, text, 0);
            }
            if (ud == (void *)2) {
#ifndef CONFIG_USE_TOUCHSCREEN
                lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
#endif
            }
        }
    }
}

static int find_next_selectable(detail_view_t *dv, int start, int dir) {
    int idx = start;
    for (int i = 0; i < dv->count; i++) {
        idx += dir;
        if (idx < 0) idx = dv->count - 1;
        if (idx >= dv->count) idx = 0;
        if (dv->rows[idx].selectable) return idx;
    }
    return start;
}

detail_view_t *detail_view_create(lv_obj_t *parent, const char *title) {
    if (!parent) parent = lv_scr_act();
    detail_view_t *dv = (detail_view_t *)calloc(1, sizeof(detail_view_t));
    if (!dv) return NULL;
    
    int w = LV_HOR_RES;
    int h = LV_VER_RES;
    int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    bool small = (w <= 240 || h <= 240);
    dv->compact_layout = detail_view_should_use_compact_layout(w, h);
    dv->btn_h = dv->compact_layout ? 20 : (small ? 28 : GUI_CONTROL_H);
    bool rounded = get_menu_rounded();
    dv->item_radius = (!dv->compact_layout && rounded) ? GUI_RADIUS_SM : 0;
    dv->selected = -1;
    dv->first_selectable = -1;
    dv->info_count = 0;
    dv->nav_region = DETAIL_NAV_REGION_INFO;
    dv->wrap_pending_down = false;
    dv->wrap_pending_up = false;
    dv->last_step_ms = 0;
    
    lv_color_t bg, surface, surface_alt, text, accent;
    get_theme_colors(&bg, &surface, &surface_alt, &text, &accent);

    /* P4 detail content is intentionally capped, but the view must still
     * cover the complete display so the parent view cannot show at the sides. */
    lv_coord_t content_w = LV_MIN(w, GUI_CONTENT_MAX_W);
    if (content_w < w) {
        dv->backdrop = lv_obj_create(parent);
        if (dv->backdrop) {
            lv_obj_remove_style_all(dv->backdrop);
            lv_obj_set_size(dv->backdrop, w, h);
            lv_obj_set_pos(dv->backdrop, 0, 0);
            lv_obj_set_style_bg_color(dv->backdrop, bg, 0);
            lv_obj_set_style_bg_opa(dv->backdrop, LV_OPA_COVER, 0);
            lv_obj_clear_flag(dv->backdrop, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
    
    dv->container = lv_obj_create(parent);
    lv_coord_t content_h = h - STATUS_BAR_HEIGHT;
    if (content_h < 60) content_h = 60;
    dv->content_h = content_h;
    lv_obj_set_size(dv->container, content_w, content_h);
    lv_obj_align(dv->container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(dv->container, bg, 0);
    lv_obj_set_style_pad_all(dv->container, 0, 0);
    lv_obj_set_style_pad_row(dv->container, 0, 0);
    lv_obj_set_style_pad_column(dv->container, 0, 0);
    lv_obj_set_style_border_width(dv->container, 0, 0);
    lv_obj_set_style_radius(dv->container, 0, 0);
    lv_obj_set_scrollbar_mode(dv->container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(dv->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dv->container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    
    lv_coord_t info_pad_h = dv->compact_layout ? 2 : GUI_SAFEAREA_HOR;
    
    dv->info_panel = lv_obj_create(dv->container);
    lv_obj_set_width(dv->info_panel, LV_PCT(100));
    lv_obj_set_height(dv->info_panel, 0);
    lv_obj_set_style_bg_color(dv->info_panel, bg, 0);
    lv_obj_set_style_bg_opa(dv->info_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(dv->info_panel, 0, 0);
    lv_obj_set_style_pad_top(dv->info_panel, dv->compact_layout ? 0 : 1, 0);
    lv_obj_set_style_pad_bottom(dv->info_panel, dv->compact_layout ? 1 : 4, 0);
    lv_obj_set_style_pad_left(dv->info_panel, info_pad_h, 0);
    lv_obj_set_style_pad_right(dv->info_panel, info_pad_h, 0);
    lv_obj_set_style_pad_row(dv->info_panel, 0, 0);
    lv_obj_set_style_border_width(dv->info_panel, 0, 0);
    lv_obj_set_style_radius(dv->info_panel, 0, 0);
    lv_obj_set_scroll_dir(dv->info_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(dv->info_panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(dv->info_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(dv->info_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    dv->info_canvas = lv_obj_create(dv->info_panel);
    lv_obj_set_width(dv->info_canvas, LV_PCT(100));
    lv_obj_set_height(dv->info_canvas, 0);
    lv_obj_set_style_bg_opa(dv->info_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dv->info_canvas, 0, 0);
    lv_obj_set_style_radius(dv->info_canvas, 0, 0);
    lv_obj_set_style_pad_all(dv->info_canvas, 0, 0);
    lv_obj_add_flag(dv->info_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(dv->info_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(dv->info_canvas, detail_view_info_draw_event, LV_EVENT_DRAW_MAIN, dv);
    
    lv_coord_t action_pad_h = dv->compact_layout ? 2 : GUI_SAFEAREA_HOR;
    lv_coord_t action_pad_v = dv->compact_layout ? 1 : GUI_SAFEAREA_VER;
    lv_coord_t action_row_gap = dv->compact_layout ? 1 : GUI_GRID;
    
    dv->action_list = lv_obj_create(dv->container);
    lv_obj_set_width(dv->action_list, LV_PCT(100));
    lv_obj_set_flex_grow(dv->action_list, 1);
    lv_obj_set_style_bg_color(dv->action_list, bg, 0);
    lv_obj_set_style_pad_top(dv->action_list, action_pad_v, 0);
    lv_obj_set_style_pad_bottom(dv->action_list, action_pad_v + GUI_HOME_SAFE_H, 0);
    lv_obj_set_style_pad_left(dv->action_list, action_pad_h, 0);
    lv_obj_set_style_pad_right(dv->action_list, action_pad_h, 0);
    lv_obj_set_style_pad_row(dv->action_list, action_row_gap, 0);
    lv_obj_set_style_pad_column(dv->action_list, 0, 0);
    lv_obj_set_style_border_width(dv->action_list, 0, 0);
    lv_obj_set_style_radius(dv->action_list, 0, 0);
    lv_obj_set_flex_flow(dv->action_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dv->action_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(dv->action_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(dv->action_list, LV_SCROLLBAR_MODE_AUTO);
    
    lv_style_init(&dv->style_item);
    lv_style_set_bg_color(&dv->style_item, surface);
    lv_style_set_bg_opa(&dv->style_item, LV_OPA_COVER);
    lv_style_set_border_width(&dv->style_item, 0);
    lv_style_set_radius(&dv->style_item, dv->item_radius);
    
    lv_style_init(&dv->style_item_alt);
    lv_style_set_bg_color(&dv->style_item_alt, surface_alt);
    lv_style_set_bg_opa(&dv->style_item_alt, LV_OPA_COVER);
    lv_style_set_border_width(&dv->style_item_alt, 0);
    lv_style_set_radius(&dv->style_item_alt, dv->item_radius);
    
    lv_style_init(&dv->style_selected);
    lv_style_set_bg_opa(&dv->style_selected, LV_OPA_COVER);
    lv_style_set_radius(&dv->style_selected, dv->item_radius);
    lv_style_set_bg_grad_dir(&dv->style_selected, LV_GRAD_DIR_NONE);
    
    lv_style_init(&dv->style_header);
    lv_style_set_bg_color(&dv->style_header, surface);
    lv_style_set_bg_opa(&dv->style_header, LV_OPA_30);
    lv_style_set_border_width(&dv->style_header, 0);
    lv_style_set_radius(&dv->style_header, dv->item_radius);
    
    lv_style_init(&dv->style_divider);
    lv_style_set_bg_color(&dv->style_divider, accent);
    lv_style_set_bg_opa(&dv->style_divider, LV_OPA_40);
    lv_style_set_border_width(&dv->style_divider, 0);
    lv_style_set_radius(&dv->style_divider, dv->item_radius);
    
    display_manager_add_status_bar((title && title[0]) ? title : "Details");
    
    return dv;
}

void detail_view_destroy(detail_view_t *dv) {
    if (!dv) return;
    detail_view_free_info_items(dv);
    free(dv->info_items);
    if (dv->container && lv_obj_is_valid(dv->container)) lv_obj_del(dv->container);
    if (dv->backdrop && lv_obj_is_valid(dv->backdrop)) lv_obj_del(dv->backdrop);
    free(dv->rows);
    free(dv);
}

void detail_view_add_info(detail_view_t *dv, const char *label, const char *value) {
    if (!dv || !dv->info_panel) return;
    if (!ensure_info_capacity(dv, dv->info_count + 1)) return;
    if (!ensure_capacity(dv, dv->count + 1)) return;

    int info_idx = dv->info_count;
    dv->info_items[info_idx].label = detail_strdup(label ? label : "");
    dv->info_items[info_idx].value = detail_strdup(value ? value : "");
    if (!dv->info_items[info_idx].label || !dv->info_items[info_idx].value) {
        free(dv->info_items[info_idx].label);
        free(dv->info_items[info_idx].value);
        dv->info_items[info_idx].label = NULL;
        dv->info_items[info_idx].value = NULL;
        return;
    }
    
    dv->rows[dv->count].obj = NULL;
    dv->rows[dv->count].type = DETAIL_ROW_INFO;
    dv->rows[dv->count].selectable = false;
    dv->info_count++;
    dv->count++;

    detail_view_sync_info_canvas(dv);
}

void detail_view_add_infof(detail_view_t *dv, const char *label, const char *fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    detail_view_add_info(dv, label, buf);
}

lv_obj_t *detail_view_add_action(detail_view_t *dv, const char *label, lv_event_cb_t on_click, void *user_data) {
    if (!dv || !dv->action_list) return NULL;
    if (!ensure_capacity(dv, dv->count + 1)) return NULL;
    
    int zebra_idx = 0;
    for (int i = dv->info_count; i < dv->count; i++) {
        if (dv->rows[i].type == DETAIL_ROW_ACTION) zebra_idx++;
    }
    
    lv_obj_t *btn = lv_obj_create(dv->action_list);
    if (!btn) return NULL;
    
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, get_action_row_height(dv));
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(btn, 0, 0);
    lv_obj_set_style_pad_bottom(btn, 0, 0);
    lv_obj_set_style_pad_left(btn, dv->compact_layout ? 4 : GUI_SAFEAREA_VER, 0);
    lv_obj_set_style_pad_right(btn, dv->compact_layout ? 4 : GUI_SAFEAREA_VER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_style(btn, get_zebra_style(dv, zebra_idx), 0);
    
    if (on_click) {
        lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, user_data);
    }
    
    lv_obj_t *lbl = lv_label_create(btn);
    if (!lbl) {
        ESP_LOGW(DV_TAG, "add_action: label alloc failed ('%s')", label ? label : "");
        lv_obj_del(btn);
        return NULL;
    }
    lv_label_set_text(lbl, label ? label : "");
    lv_obj_set_style_text_font(lbl, get_item_font(dv), 0);
    lv_color_t text;
    get_theme_colors(NULL, NULL, NULL, &text, NULL);
    lv_obj_set_style_text_color(lbl, text, 0);
    lv_obj_set_user_data(lbl, (void *)1);
    lv_obj_set_flex_grow(lbl, 1);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    
#ifdef CONFIG_USE_TOUCHSCREEN
    /* Touch UIs show a trailing chevron per action row. */
    lv_obj_t *arrow = lv_label_create(btn);
    if (arrow) {
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(arrow, get_item_font(dv), 0);
        lv_color_t text2;
        get_theme_colors(NULL, NULL, NULL, &text2, NULL);
        lv_obj_set_style_text_color(arrow, text2, 0);
        lv_obj_set_user_data(arrow, (void *)2);
        lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);
    } else {
        /* LVGL pool exhausted: keep the row usable without its arrow. */
        ESP_LOGW(DV_TAG, "add_action: arrow label alloc failed ('%s')", label ? label : "");
    }
#else
    /* Non-touch (joystick/encoder) builds never show the arrow; it was created
     * only to be flagged LV_OBJ_FLAG_HIDDEN every row. Skip it entirely. */
    lv_obj_t *arrow = NULL;
#endif
    
    dv->rows[dv->count].obj = btn;
    dv->rows[dv->count].type = DETAIL_ROW_ACTION;
    dv->rows[dv->count].selectable = true;
    
    if (dv->first_selectable < 0) {
        dv->first_selectable = dv->count;
    }
    
    bool initialize_selection = dv->selected < 0 && dv->first_selectable == dv->count;
    if (initialize_selection) {
        dv->selected = dv->count;
    }

    dv->count++;
    detail_view_sync_info_canvas(dv);

    if (initialize_selection) {
        if (detail_view_info_can_scroll(dv)) {
            dv->nav_region = DETAIL_NAV_REGION_INFO;
        } else {
            dv->nav_region = DETAIL_NAV_REGION_ACTIONS;
            apply_selected_style(dv, btn, true);
        }
    }
    return btn;
}

void detail_view_add_header(detail_view_t *dv, const char *text) {
    if (!dv || !dv->action_list) return;
    if (!ensure_capacity(dv, dv->count + 1)) return;
    
    lv_obj_t *btn = lv_obj_create(dv->action_list);
    if (!btn) return;
    
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, dv->btn_h);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(btn, &dv->style_header, 0);
    
    lv_obj_t *lbl = lv_label_create(btn);
    if (lbl) {
        lv_label_set_text(lbl, text ? text : "");
        const lv_font_t *font = dv->compact_layout ? accessibility_get_font_small() : ((dv->btn_h <= 40) ? accessibility_get_font_body() : accessibility_get_font_title());
        lv_obj_set_style_text_font(lbl, font, 0);
        lv_color_t txt;
        get_theme_colors(NULL, NULL, NULL, &txt, NULL);
        lv_obj_set_style_text_color(lbl, txt, 0);
    }
    
    dv->rows[dv->count].obj = btn;
    dv->rows[dv->count].type = DETAIL_ROW_HEADER;
    dv->rows[dv->count].selectable = false;
    dv->count++;
    detail_view_sync_info_canvas(dv);
}

void detail_view_add_divider(detail_view_t *dv) {
    if (!dv || !dv->action_list) return;
    if (!ensure_capacity(dv, dv->count + 1)) return;
    
    lv_obj_t *line = lv_obj_create(dv->action_list);
    lv_obj_set_size(line, LV_PCT(100), dv->compact_layout ? 2 : 1);
    lv_obj_add_style(line, &dv->style_divider, 0);
    if (!dv->compact_layout) {
        lv_obj_set_style_pad_left(line, GUI_SAFEAREA_VER, 0);
        lv_obj_set_style_pad_right(line, GUI_SAFEAREA_VER, 0);
    }
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    
    dv->rows[dv->count].obj = line;
    dv->rows[dv->count].type = DETAIL_ROW_DIVIDER;
    dv->rows[dv->count].selectable = false;
    dv->count++;
    detail_view_sync_info_canvas(dv);
}

lv_obj_t *detail_view_add_back(detail_view_t *dv, lv_event_cb_t on_click, void *user_data) {
    int prev = dv->count;
    detail_view_add_action(dv, LV_SYMBOL_LEFT " Back", on_click, user_data);
    if (dv->count == prev) return NULL;
    return dv->rows[dv->count - 1].obj;
}

void detail_view_set_selected(detail_view_t *dv, int index) {
    if (!dv || dv->count == 0) return;
    
    index = find_next_selectable(dv, index - 1, 1);
    
    if (index < 0 || index >= dv->count) {
        index = dv->first_selectable >= 0 ? dv->first_selectable : 0;
    }
    if (!dv->rows[index].selectable) {
        index = find_next_selectable(dv, index, 1);
    }
    if (dv->selected == index && dv->nav_region == DETAIL_NAV_REGION_ACTIONS) return;
    
    if (dv->selected >= 0 && dv->selected < dv->count) {
        apply_selected_style(dv, dv->rows[dv->selected].obj, false);
    }
    
    dv->selected = index;
    dv->nav_region = DETAIL_NAV_REGION_ACTIONS;
    apply_selected_style(dv, dv->rows[dv->selected].obj, true);
    lv_obj_scroll_to_view(dv->rows[dv->selected].obj, LV_ANIM_ON);
}

void detail_view_move_selection(detail_view_t *dv, int delta) {
    if (!dv || dv->count == 0 || dv->first_selectable < 0) return;
    
    int new_idx = find_next_selectable(dv, dv->selected, delta > 0 ? 1 : -1);
    detail_view_set_selected(dv, new_idx);
}

bool detail_view_step_down(detail_view_t *dv) {
    if (!dv) return false;

    if (dv->nav_region == DETAIL_NAV_REGION_INFO) {
        ESP_LOGI(DV_TAG, "DOWN enter region=INFO selected=%d info_count=%d", dv->selected, dv->info_count);
        if (detail_view_scroll_info(dv, 1)) {
            detail_view_clear_wrap_pending(dv);
            bool at_bot = detail_view_info_at_bottom(dv);
            ESP_LOGI(DV_TAG, "DOWN scrolled top=%d bottom=%d at_boundary=%d",
                     (int)lv_obj_get_scroll_top(dv->info_panel),
                     (int)lv_obj_get_scroll_bottom(dv->info_panel),
                     at_bot);
            if (at_bot) {
                detail_view_wrap_to_actions(dv);
                ESP_LOGI(DV_TAG, "DOWN wrap->actions selected=%d", dv->selected);
            }
            return true;
        }
        if (detail_view_check_wrap_pending(dv, true)) {
            detail_view_clear_wrap_pending(dv);
            ESP_LOGI(DV_TAG, "DOWN wrap-pending fired -> actions selected=%d", dv->selected);
            return detail_view_wrap_to_actions(dv);
        }
        detail_view_set_wrap_pending(dv, true);
        ESP_LOGI(DV_TAG, "DOWN at boundary, wrap_pending set (no-op)");
        return true;
    }

    detail_view_clear_wrap_pending_up(dv);

    if (dv->selected < 0 && dv->first_selectable >= 0) {
        detail_view_set_selected(dv, dv->first_selectable);
        ESP_LOGI(DV_TAG, "DOWN init->%d", dv->first_selectable);
        return true;
    }

    int last_idx = detail_view_get_last_selectable(dv);
    if (last_idx < 0) return false;

    if (dv->selected != last_idx) {
        int next_idx = find_next_selectable(dv, dv->selected, 1);
        if (next_idx != dv->selected) {
            ESP_LOGI(DV_TAG, "DOWN action prev=%d -> next=%d", dv->selected, next_idx);
            detail_view_set_selected(dv, next_idx);
            return true;
        }
    }

    bool can_scroll_info = detail_view_info_can_scroll(dv);
    ESP_LOGI(DV_TAG, "DOWN at last action selected=%d info_count=%d info_can_scroll=%d",
             dv->selected, dv->info_count, can_scroll_info);

    if (dv->info_count > 0) {
        if (dv->selected >= 0 && dv->selected < dv->count) {
            apply_selected_style(dv, dv->rows[dv->selected].obj, false);
        }
        dv->nav_region = DETAIL_NAV_REGION_INFO;
        if (detail_view_scroll_info(dv, 1)) {
            ESP_LOGI(DV_TAG, "DOWN -> INFO, scrolled");
            return true;
        }
        ESP_LOGI(DV_TAG, "DOWN -> INFO, info doesn't need scrolling");
        return true;
    }

    if (last_idx >= 0 && last_idx != dv->selected) {
        if (dv->selected >= 0 && dv->selected < dv->count) {
            apply_selected_style(dv, dv->rows[dv->selected].obj, false);
        }
        detail_view_set_selected(dv, dv->first_selectable);
        ESP_LOGI(DV_TAG, "DOWN no-info wrap %d -> %d", dv->selected, dv->first_selectable);
        return true;
    }

    return false;
}

bool detail_view_step_up(detail_view_t *dv) {
    if (!dv) return false;

    if (dv->nav_region == DETAIL_NAV_REGION_ACTIONS) {
        detail_view_clear_wrap_pending_down(dv);

        if (dv->selected >= 0 && dv->selected != dv->first_selectable) {
            int prev_idx = find_next_selectable(dv, dv->selected, -1);
            if (prev_idx != dv->selected) {
                detail_view_set_selected(dv, prev_idx);
                ESP_LOGI(DV_TAG, "UP action %d -> %d", dv->selected, prev_idx);
                return true;
            }
        }

        ESP_LOGI(DV_TAG, "UP at first action selected=%d info_count=%d info_can_scroll=%d",
                 dv->selected, dv->info_count, detail_view_info_can_scroll(dv));

        if (dv->info_count > 0) {
            if (dv->selected >= 0 && dv->selected < dv->count) {
                apply_selected_style(dv, dv->rows[dv->selected].obj, false);
            }
            dv->nav_region = DETAIL_NAV_REGION_INFO;
            if (detail_view_scroll_info(dv, -1)) {
                ESP_LOGI(DV_TAG, "UP -> INFO, scrolled");
                return true;
            }
            ESP_LOGI(DV_TAG, "UP -> INFO, info doesn't need scrolling");
            return true;
        }

        int last_idx = detail_view_get_last_selectable(dv);
        if (last_idx >= 0 && last_idx != dv->selected) {
            if (dv->selected >= 0 && dv->selected < dv->count) {
                apply_selected_style(dv, dv->rows[dv->selected].obj, false);
            }
            detail_view_set_selected(dv, last_idx);
            ESP_LOGI(DV_TAG, "UP no-info wrap %d -> %d", dv->selected, last_idx);
            return true;
        }

        return false;
    }

    if (detail_view_scroll_info(dv, -1)) {
        detail_view_clear_wrap_pending(dv);
        bool at_top = detail_view_info_at_top(dv);
        ESP_LOGI(DV_TAG, "UP scrolled region=INFO top=%d at_boundary=%d",
                 (int)lv_obj_get_scroll_top(dv->info_panel), at_top);
        if (at_top) {
            detail_view_wrap_to_actions(dv);
            ESP_LOGI(DV_TAG, "UP wrap->actions selected=%d", dv->selected);
        }
        return true;
    }
    if (detail_view_check_wrap_pending(dv, false)) {
        detail_view_clear_wrap_pending(dv);
        ESP_LOGI(DV_TAG, "UP wrap-pending fired -> actions selected=%d", dv->selected);
        return detail_view_wrap_to_actions(dv);
    }
    detail_view_set_wrap_pending(dv, false);
    ESP_LOGI(DV_TAG, "UP at boundary, wrap_pending set (no-op)");
    return true;
}

int detail_view_get_selected(const detail_view_t *dv) {
    return dv ? dv->selected : -1;
}

int detail_view_get_count(const detail_view_t *dv) {
    return dv ? dv->count : 0;
}

detail_row_type_t detail_view_get_row_type(const detail_view_t *dv, int index) {
    if (!dv || index < 0 || index >= dv->count) return DETAIL_ROW_INFO;
    return dv->rows[index].type;
}

void detail_view_clear(detail_view_t *dv) {
    if (!dv) return;

    detail_view_free_info_items(dv);
    lv_obj_clean(dv->action_list);
    
    for (int i = 0; i < dv->count; ++i) {
        dv->rows[i].obj = NULL;
    }
    dv->count = 0;
    dv->selected = -1;
    dv->first_selectable = -1;
    dv->info_count = 0;
    detail_view_clear_wrap_pending(dv);
    detail_view_sync_info_canvas(dv);
}

void detail_view_set_bottom_reserved(detail_view_t *dv, lv_coord_t reserved_h) {
    if (!dv || !dv->container || !lv_obj_is_valid(dv->container)) return;
    if (reserved_h < 0) reserved_h = 0;

    lv_coord_t content_h = LV_VER_RES - GUI_STATUS_BAR_H - reserved_h;
    if (content_h < 60) content_h = 60;
    dv->content_h = content_h;
    lv_obj_set_height(dv->container, content_h);
    detail_view_sync_info_canvas(dv);
}

lv_obj_t *detail_view_get_list(detail_view_t *dv) {
    return dv ? dv->action_list : NULL;
}

lv_obj_t *detail_view_get_info_panel(detail_view_t *dv) {
    return dv ? dv->info_panel : NULL;
}

lv_obj_t *detail_view_get_selected_obj(detail_view_t *dv) {
    if (!dv || dv->selected < 0 || dv->selected >= dv->count) return NULL;
    return dv->rows[dv->selected].obj;
}

void detail_view_refresh_styles(detail_view_t *dv) {
    if (!dv) return;
    
    lv_color_t bg, surface, surface_alt, text, accent;
    get_theme_colors(&bg, &surface, &surface_alt, &text, &accent);
    
    bool rounded = get_menu_rounded();
    dv->item_radius = (!dv->compact_layout && rounded) ? GUI_RADIUS_SM : 0;
    
    lv_obj_set_style_bg_color(dv->container, bg, 0);
    if (dv->backdrop && lv_obj_is_valid(dv->backdrop)) {
        lv_obj_set_style_bg_color(dv->backdrop, bg, 0);
    }
    lv_obj_set_style_bg_color(dv->info_panel, bg, 0);
    lv_obj_set_style_bg_color(dv->action_list, bg, 0);
    
    lv_coord_t action_pad_h = dv->compact_layout ? 2 : GUI_SAFEAREA_HOR;
    lv_coord_t action_pad_v = dv->compact_layout ? 1 : GUI_SAFEAREA_VER;
    lv_coord_t action_row_gap = dv->compact_layout ? 1 : GUI_GRID;
    lv_obj_set_style_pad_top(dv->action_list, action_pad_v, 0);
    lv_obj_set_style_pad_bottom(dv->action_list, action_pad_v, 0);
    lv_obj_set_style_pad_left(dv->action_list, action_pad_h, 0);
    lv_obj_set_style_pad_right(dv->action_list, action_pad_h, 0);
    lv_obj_set_style_pad_row(dv->action_list, action_row_gap, 0);
    
    lv_coord_t info_pad_h = dv->compact_layout ? 2 : GUI_SAFEAREA_HOR;
    lv_obj_set_style_pad_left(dv->info_panel, info_pad_h, 0);
    lv_obj_set_style_pad_right(dv->info_panel, info_pad_h, 0);
    
    lv_style_set_bg_color(&dv->style_item, surface);
    lv_style_set_radius(&dv->style_item, dv->item_radius);
    lv_style_set_bg_color(&dv->style_item_alt, surface_alt);
    lv_style_set_radius(&dv->style_item_alt, dv->item_radius);
    lv_style_set_radius(&dv->style_selected, dv->item_radius);
    lv_style_set_bg_color(&dv->style_header, surface);
    lv_style_set_bg_opa(&dv->style_header, LV_OPA_30);
    lv_style_set_radius(&dv->style_header, dv->item_radius);
    lv_style_set_bg_color(&dv->style_divider, text);
    lv_style_set_bg_opa(&dv->style_divider, LV_OPA_20);
    lv_style_set_radius(&dv->style_divider, dv->item_radius);

    detail_view_sync_info_canvas(dv);
    
    int zebra_idx = 0;
    for (int i = 0; i < dv->count; i++) {
        lv_obj_t *obj = dv->rows[i].obj;
        if (!obj || !lv_obj_is_valid(obj)) continue;
        
        if (dv->rows[i].type == DETAIL_ROW_ACTION) {
            lv_obj_remove_style(obj, &dv->style_item, 0);
            lv_obj_remove_style(obj, &dv->style_item_alt, 0);
            lv_obj_add_style(obj, get_zebra_style(dv, zebra_idx), 0);
            zebra_idx++;
            
            lv_obj_set_style_pad_left(obj, dv->compact_layout ? 4 : GUI_SAFEAREA_VER, 0);
            lv_obj_set_style_pad_right(obj, dv->compact_layout ? 4 : GUI_SAFEAREA_VER, 0);
            
            uint32_t child_cnt = lv_obj_get_child_cnt(obj);
            for (uint32_t c = 0; c < child_cnt; c++) {
                lv_obj_t *child = lv_obj_get_child(obj, (int32_t)c);
                if (child && lv_obj_get_user_data(child)) {
                    lv_obj_set_style_text_color(child, text, 0);
                }
            }
        } else if (dv->rows[i].type == DETAIL_ROW_DIVIDER) {
            lv_obj_set_style_pad_left(obj, dv->compact_layout ? 0 : GUI_SAFEAREA_VER, 0);
            lv_obj_set_style_pad_right(obj, dv->compact_layout ? 0 : GUI_SAFEAREA_VER, 0);
        }
    }
    
    if (dv->selected >= 0 && dv->selected < dv->count) {
        apply_selected_style(dv, dv->rows[dv->selected].obj, true);
    }
}
