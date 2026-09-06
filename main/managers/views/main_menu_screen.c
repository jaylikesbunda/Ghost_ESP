#include "managers/views/main_menu_screen.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lvgl.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/haptic_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/asset_pack.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "gui/lvgl_safe.h"
#include "gui/main_menu_layout.h"
#include "gui/large_builtin_icons.h"
#include "gui/menu_catalog.h"
#include "gui/menu_item_style.h"
#include "gui/screen_layout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "managers/views/clock_screen.h"
#include "managers/views/settings_screen.h"
#include "managers/views/lockscreen.h"
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include "gui/toast.h"
#include "managers/status_display_manager.h"
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/views/audio_player_screen.h"
#endif
#ifdef CONFIG_HAS_NFC
#include "managers/views/nfc_view.h"
#endif
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
#include "managers/views/badusb_view.h"
#endif
#ifdef CONFIG_HAS_BADBLE
#include "managers/views/badble_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
#include "managers/views/accelerometer_screen.h"
#endif
#include "managers/views/ethernet_screen.h"

#ifdef CONFIG_HAS_ENVIII
#include "managers/views/enviii_screen.h"
#endif

#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/views/audio_player_screen.h"
#endif


static void handle_menu_item_selection(int item_index);
static void scroll_launcher_card_to_view(int item_index);
static int visible_index_to_menu_index(int visible_index, bool dual_comm_connected);
static void select_menu_item_with_scroll(int index, bool slide_left, lv_anim_enable_t scroll_anim);

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

LV_IMG_DECLARE(dualcomm);
LV_IMG_DECLARE(lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
LV_IMG_DECLARE(nrf24);
LV_IMG_DECLARE(subghz);
LV_IMG_DECLARE(lock);
LV_IMG_DECLARE(rave);

static const char *TAG = "MainMenu";


static inline int get_anim_duration(void) {
    return settings_get_reduced_motion(&G_Settings) ? 0 : 60;
}

#define ANIM_DURATION get_anim_duration()

lv_obj_t *menu_container;
static int selected_item_index = 0;
static int touch_start_x;
static int touch_start_y;
static int touch_last_x;
static int touch_last_y;
static bool touch_started = false;
static bool touch_dragged = false;
static int touch_drag_axis = 0;
static lv_obj_t *touch_scroll_target = NULL;
static bool is_animating = false;
// touch gesture thresholds
#define SWIPE_THRESHOLD 50
#define TAP_THRESHOLD 14
#define DRAG_AXIS_THRESHOLD 14
#define DRAG_AXIS_BIAS 4
#define DRAG_DELTA_DEADZONE 1
#define DRAG_MAX_STEP 64
static main_menu_layout_kind_t current_layout = MAIN_MENU_LAYOUT_CAROUSEL;
static lv_color_t menu_bg_color;
static lv_color_t menu_surface_color;
static lv_color_t menu_text_color;

static inline bool card_bg_enabled(void) {
    return settings_get_menu_card_bg(&G_Settings);
}

static inline void apply_card_style(lv_obj_t *obj, lv_color_t surface, lv_color_t border, int border_w, int shadow_w) {
    gui_menu_card_apply(obj, card_bg_enabled(), surface, border, border_w, shadow_w);
}

static inline void apply_card_selection_style(lv_obj_t *obj, lv_color_t accent) {
    gui_menu_card_apply_selected(obj, card_bg_enabled(), accent);
}

static inline bool is_compact_layout(void) {
    return current_layout == MAIN_MENU_LAYOUT_COMPACT;
}

static void apply_compact_menu_tile(lv_obj_t *obj, bool selected) {
    if (!obj) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t accent_text = theme_palette_is_bright(theme) ? lv_color_black() : lv_color_white();
    lv_obj_t *label = lv_obj_get_child(obj, 0);
    gui_menu_compact_tile_apply(obj, label, selected, card_bg_enabled(),
                                menu_surface_color, menu_text_color, accent, accent_text);
}

static void apply_menu_launcher_selection_style(lv_obj_t *card, bool selected) {
    if (!card) return;
    selected = selected && gui_menu_focus_visible();

    lv_color_t label_color = menu_text_color;
    if (selected) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        gui_menu_launcher_tile_apply_selected(card, card_bg_enabled(), accent);
        label_color = accent;
    } else {
        gui_menu_launcher_tile_apply(card, card_bg_enabled(), menu_surface_color);
    }

    lv_obj_t *label = lv_obj_get_child(card, -1);
    if (label) lv_obj_set_style_text_color(label, label_color, 0);
}

static int grid_rows = 0;
static int grid_cols = 0;

// Launcher tile layout variables
static lv_obj_t *grid_cards_container = NULL;
static lv_obj_t **grid_cards = NULL;
static int grid_card_width = 0;
static int grid_card_height = 0;
static lv_obj_t *launcher_page_indicator = NULL;
static int launcher_current_page = -1;

// List layout variables
static lv_obj_t **list_buttons = NULL;

typedef menu_catalog_item_t menu_item_t;
static menu_item_t *menu_items;
static int total_menu_items;
static int num_items;
lv_obj_t *current_item_obj = NULL;
static bool carousel_next_slide_left = false;

// Add navigation button objects at file scope
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;
static lv_obj_t *carousel_prev_obj = NULL;
static lv_obj_t *carousel_next_obj = NULL;
static lv_obj_t *hero_pip_container = NULL;

static inline bool is_carousel_like_layout(void) {
    return current_layout == MAIN_MENU_LAYOUT_CAROUSEL || current_layout == MAIN_MENU_LAYOUT_HERO;
}

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *label;
    const lv_img_dsc_t *icon_src;
    const char *label_text;
    lv_color_t border_color;
    bool icon_recolor_enabled;
    int item_index;
} carousel_card_cache_t;

static carousel_card_cache_t carousel_cache = {0};

static int clamp_drag_delta(int delta) {
    if (abs(delta) <= DRAG_DELTA_DEADZONE) return 0;
    if (delta > DRAG_MAX_STEP) return DRAG_MAX_STEP;
    if (delta < -DRAG_MAX_STEP) return -DRAG_MAX_STEP;
    return delta;
}

static int resolve_drag_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < DRAG_AXIS_THRESHOLD && abs_dy < DRAG_AXIS_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + DRAG_AXIS_BIAS) return 1;
    if (abs_dx >= abs_dy + DRAG_AXIS_BIAS) return 2;
    return 0;
}

static int get_total_menu_items(void) {
    return total_menu_items;
}

static const lv_img_dsc_t *menu_item_icon(int menu_index) {
    if (menu_index < 0 || menu_index >= get_total_menu_items()) return NULL;
    if (strncmp(menu_items[menu_index].id, "plugin:", 7) == 0) {
        const plugin_app_manifest_t *app = plugin_manager_find(menu_items[menu_index].id + 7);
        const lv_img_dsc_t *icon = app ? plugin_manager_get_icon(app) : NULL;
        return gui_large_builtin_icon(asset_pack_get_app_icon(icon ? icon : menu_items[menu_index].icon));
    }
    return gui_large_builtin_icon(asset_pack_get_icon(menu_items[menu_index].asset_key,
                                                      menu_items[menu_index].icon));
}

static bool menu_item_icon_should_recolor(int menu_index, const lv_img_dsc_t *icon) {
    return menu_index >= 0 && menu_index < get_total_menu_items() &&
           icon == gui_large_builtin_icon(menu_items[menu_index].icon);
}

static void update_carousel_preview(lv_obj_t **preview_ptr, int visible_index, int x_offset,
                                    const main_menu_layout_metrics_t *layout) {
    if (!preview_ptr || !layout || !layout->carousel_show_previews) return;

    lv_obj_t *preview = *preview_ptr;
    if (!preview) {
        preview = lv_obj_create(menu_container);
        *preview_ptr = preview;
        lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(preview, GUI_RADIUS_LG, LV_PART_MAIN);
    }

    lv_obj_set_size(preview, layout->carousel_preview_size, layout->carousel_preview_size);
    lv_obj_set_style_bg_color(preview, menu_surface_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, card_bg_enabled() ? LV_OPA_50 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(preview, LV_OPA_60, LV_PART_MAIN);
    lv_obj_align(preview, LV_ALIGN_CENTER, x_offset, 0);

    /* Reuse the preview's single icon child in place; avoids object-churn
     * (alloc/free + ~10 style calls) on every carousel step. */
    lv_obj_t *icon = lv_obj_get_child(preview, 0);
    if (!icon) icon = lv_img_create(preview);

    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(visible_index, connected);
    const lv_img_dsc_t *item_icon = menu_item_icon(menu_index);
    lv_img_set_src(icon, item_icon);
    lv_img_set_antialias(icon, false);
    gui_menu_image_fit(icon, item_icon, layout->carousel_preview_icon_target, 256);
#if GUI_LARGE_SCREEN
    gui_menu_image_fit(icon, item_icon, layout->carousel_preview_icon_target, GUI_IMAGE_ZOOM_MAX);
#endif
    if (menu_item_icon_should_recolor(menu_index, item_icon)) {
        lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
    }
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
}

static void update_carousel_previews(void) {
    if (current_layout != MAIN_MENU_LAYOUT_CAROUSEL) return;
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_CAROUSEL, num_items, &layout);
    if (!layout.carousel_show_previews || num_items < 2) return;

    int previous = (selected_item_index + num_items - 1) % num_items;
    int next = (selected_item_index + 1) % num_items;
    update_carousel_preview(&carousel_prev_obj, previous, -layout.carousel_preview_offset, &layout);
    update_carousel_preview(&carousel_next_obj, next, layout.carousel_preview_offset, &layout);
    if (current_item_obj) lv_obj_move_foreground(current_item_obj);
}

static bool touch_started_and_ended_in(lv_obj_t *obj, const lv_point_t *end) {
    if (!obj || !end) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return touch_start_x >= area.x1 && touch_start_x <= area.x2 &&
           touch_start_y >= area.y1 && touch_start_y <= area.y2 &&
           end->x >= area.x1 && end->x <= area.x2 &&
           end->y >= area.y1 && end->y <= area.y2;
}

static void scroll_launcher_card_to_view(int item_index) {
    if (!grid_cards_container || !grid_cards || item_index < 0 || item_index >= num_items) return;
    lv_obj_t *card = grid_cards[item_index];
    if (!card || !lv_obj_is_valid(card)) return;

    /* layout.screen_width/page_capacity come from pure arithmetic on the
     * screen size, not live geometry, so forcing a full flex relayout of
     * every page/card here (previously on every navigation press) bought
     * nothing but cost. */
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);
    int page = item_index / layout.page_capacity;
    bool page_changed = page != launcher_current_page;
    bool animate = launcher_current_page >= 0 && page_changed &&
                   !settings_get_reduced_motion(&G_Settings);
    if (page_changed) {
        gui_menu_scroll_to_x(grid_cards_container, page * layout.container_width, animate);
    }
    launcher_current_page = page;
    if (launcher_page_indicator && page_changed) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        gui_menu_page_indicator_update(launcher_page_indicator, page, layout.page_count,
                                       lv_color_hex(theme_palette_get_accent(theme)), menu_text_color);
    }
}

static bool is_menu_index_visible(int menu_index, bool dual_comm_connected) {
    return menu_index >= 0 && menu_index < total_menu_items &&
           menu_catalog_available(&menu_items[menu_index], dual_comm_connected);
}

static int get_visible_menu_count(bool dual_comm_connected) {
    int count = 0;
    int total = get_total_menu_items();
    for (int i = 0; i < total; ++i) {
        if (is_menu_index_visible(i, dual_comm_connected)) count++;
    }
    return count;
}

static int visible_index_to_menu_index(int visible_index, bool dual_comm_connected) {
    int visible = 0;
    int total = get_total_menu_items();
    for (int i = 0; i < total; ++i) {
        if (!is_menu_index_visible(i, dual_comm_connected)) continue;
        if (visible == visible_index) return i;
        visible++;
    }
    return 0;
}

static void refresh_menu_surface_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    menu_bg_color = lv_color_hex(theme_palette_get_background(theme));
    menu_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    menu_text_color = lv_color_hex(theme_palette_get_text(theme));
}

static void init_menu_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    refresh_menu_surface_colors();

    bool connected = esp_comm_manager_is_connected();
    lv_color_t icon_color = lv_color_hex(theme_palette_get_accent(theme));
    for (int visible = 0; visible < num_items; visible++) {
        int menu_index = visible_index_to_menu_index(visible, connected);
        menu_items[menu_index].border_color = icon_color;
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    /* avoid redundant calls: only update when position actually changed */
    lv_coord_t curr_x = lv_obj_get_x(o);
    if (curr_x == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    /* read current opacity and skip update when identical to reduce paint churn */
    lv_opa_t curr = lv_obj_get_style_opa(o, 0);
    if (curr == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

// Timer callback to restore button color
// Restore label color timer callback (used since buttons are transparent)
static void restore_label_color_cb(lv_timer_t *timer) {
    lv_obj_t *label_obj = (lv_obj_t *)timer->user_data;
    lv_obj_set_style_text_color(label_obj, menu_text_color, 0);
    lv_timer_del(timer);
}

// Helper function to animate navigation button press by highlighting the arrow label
static void animate_nav_button_press(lv_obj_t *btn) {
    // Find first child (the label containing the arrow) - child id 0
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) return;

    // Set a temporary highlight color for the label
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t highlight = lv_color_hex(theme_palette_get_accent(theme));
    lv_obj_set_style_text_color(label, highlight, 0);

    // Return label to original color after a short delay
    lv_timer_create(restore_label_color_cb, 80, label);
}

static lv_obj_t *create_carousel_card(const main_menu_layout_metrics_t *layout,
                                      int x_offset, lv_opa_t opacity) {
    lv_obj_t *card = lv_btn_create(menu_container);
    gui_apply_pressed_style(card);
    carousel_cache = (carousel_card_cache_t){0};
    carousel_cache.card = card;
    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(selected_item_index, connected);

    bool show_borders = settings_get_menu_item_borders(&G_Settings);
    int card_border_w = show_borders ? 2 : 0;
    apply_card_style(card, menu_surface_color, menu_items[menu_index].border_color, card_border_w, 0);
    lv_obj_set_style_radius(card, GUI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, false, 0);
    lv_obj_set_style_opa(card, opacity, 0);
    carousel_cache.border_color = menu_items[menu_index].border_color;
    carousel_cache.item_index = selected_item_index;

    lv_obj_set_size(card, layout->carousel_button_size, layout->carousel_button_size);
    lv_obj_align(card, LV_ALIGN_CENTER, x_offset, 0);

    lv_obj_t *icon = lv_img_create(card);
    const lv_img_dsc_t *item_icon = menu_item_icon(menu_index);
    lv_img_set_src(icon, item_icon);
    carousel_cache.icon = icon;
    carousel_cache.icon_src = item_icon;
    lv_img_set_antialias(icon, false);
    gui_menu_image_fit(icon, item_icon, layout->carousel_icon_target, 256);
#if GUI_LARGE_SCREEN
    gui_menu_image_fit(icon, item_icon, layout->carousel_icon_target, GUI_IMAGE_ZOOM_MAX);
#endif
    bool recolor_enabled = menu_item_icon_should_recolor(menu_index, item_icon);
    if (recolor_enabled) {
        lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
    }
    carousel_cache.icon_recolor_enabled = recolor_enabled;
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, layout->carousel_icon_y_offset);

    if (layout->carousel_show_label) {
        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, menu_items[menu_index].name);
        lv_obj_set_style_text_font(label, accessibility_get_font_body(), 0);
        lv_obj_set_style_text_color(label, menu_text_color, 0);
        if (asset_pack_is_loaded()) {
            lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_60, 0);
            lv_obj_set_style_radius(label, 3, 0);
            lv_obj_set_style_pad_hor(label, 6, 0);
            lv_obj_set_style_pad_ver(label, 1, 0);
        }
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
        carousel_cache.label = label;
    }

    carousel_cache.label_text = menu_items[menu_index].name;
    return card;
}

static lv_obj_t *create_hero_card(const main_menu_layout_metrics_t *layout,
                                  int x_offset, lv_opa_t opacity) {
    lv_obj_t *card = lv_btn_create(menu_container);
    gui_apply_pressed_style(card);
    carousel_cache = (carousel_card_cache_t){0};
    carousel_cache.card = card;
    bool connected = esp_comm_manager_is_connected();
    int menu_index = visible_index_to_menu_index(selected_item_index, connected);

    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(card, opacity, 0);
    carousel_cache.border_color = menu_items[menu_index].border_color;
    carousel_cache.item_index = selected_item_index;

    lv_obj_set_size(card, layout->screen_width, layout->screen_height);
    lv_obj_align(card, LV_ALIGN_CENTER, x_offset, 0);

    lv_obj_t *icon = lv_img_create(card);
    const lv_img_dsc_t *item_icon = menu_item_icon(menu_index);
    lv_img_set_src(icon, item_icon);
    carousel_cache.icon = icon;
    carousel_cache.icon_src = item_icon;
    lv_img_set_antialias(icon, false);
    gui_menu_image_fit(icon, item_icon, layout->hero_icon_target, 256);
#if GUI_LARGE_SCREEN
    gui_menu_image_fit(icon, item_icon, layout->hero_icon_target, GUI_IMAGE_ZOOM_MAX);
#endif
    bool recolor_enabled = menu_item_icon_should_recolor(menu_index, item_icon);
    if (recolor_enabled) {
        lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
    }
    carousel_cache.icon_recolor_enabled = recolor_enabled;
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, layout->hero_icon_y_offset);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, menu_items[menu_index].name);
    lv_obj_set_style_text_font(label, accessibility_get_font_title(), 0);
    lv_obj_set_style_text_color(label, menu_text_color, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0,
                 layout->hero_icon_y_offset + layout->hero_icon_target / 2 + GUI_GRID * 2);
    carousel_cache.label = label;

    carousel_cache.label_text = menu_items[menu_index].name;
    return card;
}

static lv_obj_t *create_current_card(const main_menu_layout_metrics_t *layout,
                                     int x_offset, lv_opa_t opacity) {
    if (current_layout == MAIN_MENU_LAYOUT_HERO) {
        return create_hero_card(layout, x_offset, opacity);
    }
    return create_carousel_card(layout, x_offset, opacity);
}

/* Warms the icon cache for the neighbours of the current selection so a
 * subsequent swipe never blocks on SD/decompress mid-animation. */
static void prefetch_neighbor_icons(void) {
    if (!is_carousel_like_layout() || num_items < 2) return;
    bool connected = esp_comm_manager_is_connected();
    int next_idx = visible_index_to_menu_index((selected_item_index + 1) % num_items, connected);
    int prev_idx = visible_index_to_menu_index((selected_item_index + num_items - 1) % num_items, connected);
    menu_item_icon(next_idx);
    menu_item_icon(prev_idx);
}

static void update_hero_position(void) {
    if (!hero_pip_container || current_layout != MAIN_MENU_LAYOUT_HERO || num_items < 1) return;

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_HERO, num_items, &layout);
    int cap = layout.hero_pip_count < 1 ? 1 : layout.hero_pip_count;
    int pip_count = num_items < cap ? num_items : cap;
    int current = (num_items <= 1) ? 0 : (selected_item_index * (pip_count - 1)) / (num_items - 1);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    lv_obj_t *row = hero_pip_container;
    lv_obj_clean(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 10);
#if GUI_LARGE_SCREEN
    lv_obj_set_height(row, 16);
#endif
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
#if GUI_LARGE_SCREEN
    lv_obj_set_style_pad_column(row, 8, LV_PART_MAIN);
#endif
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < pip_count; ++i) {
        bool selected = i == current;
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_set_size(dot, selected ? 6 : 4, selected ? 6 : 4);
#if GUI_LARGE_SCREEN
        lv_obj_set_size(dot, selected ? 22 : 8, 8);
#endif
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, selected ? accent : menu_text_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, selected ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static void create_hero_position_indicator(void) {
    if (current_layout != MAIN_MENU_LAYOUT_HERO) return;
    if (!hero_pip_container) {
        hero_pip_container = lv_obj_create(main_menu_view.root);
        lv_obj_align(hero_pip_container, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 8));
    } else if (lv_obj_is_valid(hero_pip_container)) {
        lv_obj_clean(hero_pip_container);
    }
    lv_obj_move_foreground(hero_pip_container);
    update_hero_position();
}

static void carousel_fade_in_ready_cb(lv_anim_t *a) {
    (void)a;
    is_animating = false;
    prefetch_neighbor_icons();
}

static void carousel_fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_t *old_card = (lv_obj_t *)a->var;
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);
    int start_x = carousel_next_slide_left ? layout.carousel_transition_distance :
                                             -layout.carousel_transition_distance;

    if (old_card && lv_obj_is_valid(old_card)) lv_obj_del(old_card);
    current_item_obj = create_current_card(&layout, start_x, LV_OPA_TRANSP);
    update_carousel_previews();

    lv_anim_t move_in;
    lv_anim_init(&move_in);
    lv_anim_set_var(&move_in, current_item_obj);
    lv_anim_set_values(&move_in, start_x, 0);
    lv_anim_set_time(&move_in, ANIM_DURATION);
    lv_anim_set_path_cb(&move_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&move_in, anim_set_x);
    lv_anim_start(&move_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, current_item_obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, carousel_fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void update_menu_item(bool move_left) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);

    if (!current_item_obj) {
        current_item_obj = create_current_card(&layout, 0, LV_OPA_COVER);
        update_carousel_previews();
        is_animating = false;
        return;
    }

    is_animating = true;
    carousel_next_slide_left = move_left;

    int duration = ANIM_DURATION;
    if (duration <= 0) {
        lv_obj_del(current_item_obj);
        current_item_obj = create_current_card(&layout, 0, LV_OPA_COVER);
        update_carousel_previews();
        is_animating = false;
        return;
    }

    int end_x = move_left ? -layout.carousel_transition_distance :
                            layout.carousel_transition_distance;
    lv_anim_t move_out;
    lv_anim_init(&move_out);
    lv_anim_set_var(&move_out, current_item_obj);
    lv_anim_set_values(&move_out, 0, end_x);
    lv_anim_set_time(&move_out, duration);
    lv_anim_set_path_cb(&move_out, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&move_out, anim_set_x);
    lv_anim_set_ready_cb(&move_out, carousel_fade_out_ready_cb);
    lv_anim_start(&move_out);

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, current_item_obj);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, duration);
    lv_anim_set_exec_cb(&fade_out, anim_set_opa);
    lv_anim_start(&fade_out);
}

// Move selection vertically for list and launcher layouts; direction: -1 up, +1 down.
static void navigate_vertical(int direction, lv_anim_enable_t scroll_anim) {
    if (direction == 0) return;
    if (current_layout == MAIN_MENU_LAYOUT_HERO) {
        select_menu_item_with_scroll(selected_item_index + (direction > 0 ? 1 : -1), false,
                                     scroll_anim);
        return;
    }
    if (current_layout == MAIN_MENU_LAYOUT_LIST) {
        select_menu_item_with_scroll(selected_item_index + (direction > 0 ? 1 : -1), false,
                                     scroll_anim);
        return;
    }
    if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) {
        if (grid_cols <= 0 || grid_rows <= 0) return;

        int row = selected_item_index / grid_cols;
        int col = selected_item_index % grid_cols;

        for (int tries = 0; tries < grid_rows; ++tries) {
            row = (row + (direction > 0 ? 1 : -1) + grid_rows) % grid_rows;
            int base = row * grid_cols;
            int candidate = base + col;
            if (candidate >= num_items) {
                candidate = num_items - 1;
                if (candidate < base) continue;
            }
            select_menu_item(candidate, false);
            return;
        }
    }
}

// Resolves the target index for a horizontal (left/right) press. For the
// paginated launcher/grid layout, a press at the row's edge column advances
// to the neighboring page instead of wrapping onto the next row of the same
// page -- mirrors the page/slot math the swipe-gesture handler already uses
// on touch release. Carousel/List are single-axis, so they keep the existing
// flat +/-1 step.
static int grid_horizontal_target(int direction) {
    if (current_layout == MAIN_MENU_LAYOUT_COMPACT) {
        return selected_item_index + direction;
    }
    if ((current_layout != MAIN_MENU_LAYOUT_LAUNCHER && current_layout != MAIN_MENU_LAYOUT_COMPACT) || num_items <= 0) {
        return selected_item_index + direction;
    }

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);
    if (layout.columns <= 0 || layout.page_capacity <= 0 || layout.page_count <= 0) {
        return selected_item_index + direction;
    }

    int page = selected_item_index / layout.page_capacity;
    int slot = selected_item_index % layout.page_capacity;
    int col  = slot % layout.columns;

    if (direction > 0 && col < layout.columns - 1 && selected_item_index + 1 < num_items) {
        return selected_item_index + 1; // room to the right within this row
    }
    if (direction < 0 && col > 0) {
        return selected_item_index - 1; // room to the left within this row
    }

    // At the row's edge column: move to the neighboring page, same slot.
    page += direction > 0 ? 1 : -1;
    if (page < 0) page = layout.page_count - 1;
    if (page >= layout.page_count) page = 0;
    int target = page * layout.page_capacity + slot;
    if (target >= num_items) target = num_items - 1;
    return target;
}

/**
 *  @brief handles keyboard button presses
 */

static void toggle_favorite_for_selected(void) {
    bool dual = esp_comm_manager_is_connected();
    int menu_idx = visible_index_to_menu_index(selected_item_index, dual);
    if (menu_idx < 0 || menu_idx >= get_total_menu_items()) return;
    const char *name = menu_items[menu_idx].name;
    char plugin_favorite[FAVORITE_NAME_LEN];
    if (strncmp(menu_items[menu_idx].id, "plugin:", 7) == 0) {
        snprintf(plugin_favorite, sizeof(plugin_favorite), "app:%s", menu_items[menu_idx].id + 7);
        name = plugin_favorite;
    }
    bool was_fav = settings_is_favorite(&G_Settings, name);
    bool ok;
    if (was_fav) ok = settings_remove_favorite(&G_Settings, name);
    else ok = settings_add_favorite(&G_Settings, name);
    if (ok) {
        settings_persist_setting(SETTING_FAVORITES);
        toast_show(was_fav ? "Removed from Favorites" : "Added to Favorites", TOAST_SUCCESS);
        // Haptic feedback if available
        #ifdef CONFIG_HAS_HAPTIC
        // haptic_manager_trigger(HAPTIC_TAP);
        #endif
    } else {
        toast_show(settings_get_favorites_count(&G_Settings) >= FAVORITES_MAX ? "Favorites full" : "Failed", TOAST_WARN);
    }
}

void handle_keyboard_interactions(int keyValue){
    const bool inverted = settings_get_carousel_invert_direction(&G_Settings);
    // arrows and vim keys: h/j/k/l, plus , /
    if (keyValue == 44 || keyValue == ',' || keyValue == 'h') { // left
        ESP_LOGD(TAG, "Left button or 'h' pressed\n");
        select_menu_item(grid_horizontal_target(-1), inverted ? true : false);
    } else if (keyValue == 47 || keyValue == '/' || keyValue == 'l') { // right
        ESP_LOGD(TAG, "Right button or 'l' pressed\n");
        select_menu_item(grid_horizontal_target(1), inverted ? false : true);
    } else if (keyValue == LV_KEY_UP || keyValue == 'k' || keyValue == ';') { // up
        ESP_LOGD(TAG, "Up arrow or 'k' pressed\n");
        navigate_vertical(-1, LV_ANIM_ON);
    } else if (keyValue == LV_KEY_DOWN || keyValue == 'j' || keyValue == '.') { // down
        ESP_LOGD(TAG, "Down arrow or 'j' pressed\n");
        navigate_vertical(1, LV_ANIM_ON);
    } else if (keyValue == 'p' || keyValue == 'P') { // pin favorite
        ESP_LOGD(TAG, "Pin favorite pressed\n");
        toggle_favorite_for_selected();
    } else if (keyValue == LV_KEY_ENTER || keyValue == 13) { // enter/select
        ESP_LOGD(TAG, "Enter pressed\n");
        handle_menu_item_selection(selected_item_index);
    } else if (keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`') { // esc
        ESP_LOGD(TAG, "Esc pressed\n");
        // no-op on main menu
    }
}

/**
 * @brief Handles button click events for menu items.
 */
static void menu_button_click_handler(lv_event_t *event) {
    if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT ||
        current_layout == MAIN_MENU_LAYOUT_LIST) {
        int item_index = (int)(intptr_t)lv_event_get_user_data(event);
        if (item_index >= 0 && item_index < num_items) {
            handle_menu_item_selection(item_index);
        }
    }
}

/**
 * @brief Combined handler for menu item events.
 */
static void menu_item_event_handler(InputEvent *event) {
    if (gui_menu_set_touch_input(event->type == INPUT_TYPE_TOUCH) && selected_item_index >= 0 && selected_item_index < num_items) {
        if (grid_cards) {
            if (is_compact_layout()) apply_compact_menu_tile(grid_cards[selected_item_index], true);
            else apply_menu_launcher_selection_style(grid_cards[selected_item_index], true);
        } else if (list_buttons) {
            lv_obj_t *row = list_buttons[selected_item_index];
            lv_color_t accent = lv_color_hex(theme_palette_get_accent(settings_get_menu_theme(&G_Settings)));
            apply_card_style(row, menu_surface_color, accent, settings_get_menu_item_borders(&G_Settings) ? 2 : 0, 0);
            apply_card_selection_style(row, accent);
        }
    }
    if (event->type == INPUT_TYPE_TOUCH) {
        ESP_LOGD(TAG, "Touch event");
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
            if (!touch_started) {
                touch_started = true;
                touch_dragged = false;
                touch_drag_axis = 0;
                touch_start_x = data->point.x;
                touch_start_y = data->point.y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;
                touch_scroll_target = NULL;
            } else {
                int dx = data->point.x - touch_last_x;
                int dy = data->point.y - touch_last_y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;

                if (!touch_dragged) {
                    touch_drag_axis = resolve_drag_axis(data->point.x - touch_start_x, data->point.y - touch_start_y);
                    touch_dragged = touch_drag_axis != 0;
                }

                if (touch_dragged) {
                    lv_obj_t *target = NULL;
                    if (current_layout == MAIN_MENU_LAYOUT_LIST && menu_container && touch_drag_axis == 1) {
                        target = menu_container;
                    }
                    if (target) {
                        if (settings_get_touch_drag_scroll(&G_Settings)) {
                            dy = clamp_drag_delta(dy);
                            if (dy) display_manager_queue_scroll(target, dy);
                        } else {
                            touch_scroll_target = target;
                        }
                    }
                }
            }
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            lv_obj_t *release_target = touch_scroll_target;
            touch_scroll_target = NULL;

            if ((current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) &&
                abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                main_menu_layout_metrics_t layout;
                if (current_layout == MAIN_MENU_LAYOUT_COMPACT) {
                    select_menu_item(selected_item_index + (dx < 0 ? 1 : -1), dx < 0);
                    return;
                }
                main_menu_layout_get_metrics(current_layout, num_items, &layout);
                int page = selected_item_index / layout.page_capacity;
                int slot = selected_item_index % layout.page_capacity;
                page += dx < 0 ? 1 : -1;
                if (page < 0) page = layout.page_count - 1;
                if (page >= layout.page_count) page = 0;
                int target = page * layout.page_capacity + slot;
                if (target >= num_items) target = num_items - 1;
                touch_dragged = false;
                touch_drag_axis = 0;
                select_menu_item(target, dx < 0);
                return;
            }

            if (touch_dragged && !is_carousel_like_layout()) {
                if (release_target && !settings_get_touch_drag_scroll(&G_Settings) && dy) {
                    display_manager_queue_scroll(release_target, dy);
                }
                touch_dragged = false;
                touch_drag_axis = 0;
                return;
            }
            touch_dragged = false;
            touch_drag_axis = 0;

            // NOTE: nav button hit-tests were here previously, but that caused
            // accidental taps when a swipe ended over the nav button. We now
            // handle swipes first and perform stricter nav hit-tests below
            // (require both press and release inside the button and minimal movement).

            // Handle different layout types
            if (is_carousel_like_layout()) {
                // Prioritize swipe detection for carousel; if a horizontal
                // swipe is detected, act on it and return immediately so a
                // release-over-button doesn't trigger it.
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) { // Swipe detected
                    if (dx < 0) {
                        select_menu_item(selected_item_index + 1, true);
                    } else {
                        select_menu_item(selected_item_index - 1, false);
                    }
                    return;
                }

                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    if (touch_started_and_ended_in(carousel_prev_obj, &data->point)) {
                        select_menu_item(selected_item_index - 1, false);
                        return;
                    }
                    if (touch_started_and_ended_in(carousel_next_obj, &data->point)) {
                        select_menu_item(selected_item_index + 1, true);
                        return;
                    }
                }

                // If the touch both started and ended inside a nav button and
                // the movement was small, treat it as a nav tap. Checking this
                // here prevents the carousel tap handler from capturing nav
                // button presses.
                if (left_nav_btn && right_nav_btn) {
                    lv_area_t left_area, right_area;
                    lv_obj_get_coords(left_nav_btn, &left_area);
                    lv_obj_get_coords(right_nav_btn, &right_area);

                    bool start_in_left = (touch_start_x >= left_area.x1 && touch_start_x <= left_area.x2 &&
                                           touch_start_y >= left_area.y1 && touch_start_y <= left_area.y2);
                    bool end_in_left = (data->point.x >= left_area.x1 && data->point.x <= left_area.x2 &&
                                        data->point.y >= left_area.y1 && data->point.y <= left_area.y2);
                    if (start_in_left && end_in_left && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                        ESP_LOGD(TAG, "Left navigation button tapped (press+release inside)");
                        animate_nav_button_press(left_nav_btn);
                        select_menu_item(selected_item_index - 1, false);
                        return;
                    }

                    bool start_in_right = (touch_start_x >= right_area.x1 && touch_start_x <= right_area.x2 &&
                                            touch_start_y >= right_area.y1 && touch_start_y <= right_area.y2);
                    bool end_in_right = (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                                         data->point.y >= right_area.y1 && data->point.y <= right_area.y2);
                    if (start_in_right && end_in_right && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                        ESP_LOGD(TAG, "Right navigation button tapped (press+release inside)");
                        animate_nav_button_press(right_nav_btn);
                        select_menu_item(selected_item_index + 1, true);
                        return;
                    }
                }

                // If not a nav tap, treat a very small movement as a carousel tap
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) { // Tap detected
                    handle_menu_item_selection(selected_item_index);
                    return;
                }
                // fallthrough: small movement or non-horizontal movement - continue
                // to layout-specific hit-tests below
            } else if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) {
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    // Find which card was tapped
                    if (grid_cards) {
                        for (int i = 0; i < num_items; i++) {
                            if (grid_cards[i]) {
                                lv_area_t card_area;
                                lv_obj_get_coords(grid_cards[i], &card_area);
                                if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                    data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            } else if (current_layout == MAIN_MENU_LAYOUT_LIST) {
                // Handle vertical swipe for list scrolling
                if (abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                    if (menu_container) {
                        dy = clamp_drag_delta(dy);
                        if (dy) display_manager_queue_scroll(menu_container, dy);
                    }
                    return;
                }
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    if (list_buttons) {
                        for (int i = 0; i < num_items; i++) {
                            if (list_buttons[i]) {
                                lv_area_t btn_area;
                                lv_obj_get_coords(list_buttons[i], &btn_area);
                                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                    select_menu_item(i, false);
                                    handle_menu_item_selection(i);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        int button = event->data.joystick_index;
        handle_hardware_button_press(button);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_menu_item_selection(selected_item_index);
        } else {
            const bool inverted = settings_get_carousel_invert_direction(&G_Settings);
            if (event->data.encoder.direction > 0)
                select_menu_item(grid_horizontal_target(1), inverted ? false : true); // CW == right
            else
                select_menu_item(grid_horizontal_target(-1), inverted ? true : false);  // CCW == left
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        ESP_LOGI(TAG, "keyboard event");
        int kv = event->data.key_value;
        if (kv == 13) { // enter key
            handle_menu_item_selection(selected_item_index);
        } else {
            handle_keyboard_interactions(kv);
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, staying on main menu");
        // On main menu, the exit button doesn't do anything since we're already at the top level
#endif
    }
}

/**
 * @brief Handles hardware button presses for menu navigation.
 */
void handle_hardware_button_press(int ButtonPressed) {
    const bool inverted = settings_get_carousel_invert_direction(&G_Settings);
    if (ButtonPressed == 0) {
        select_menu_item_with_scroll(grid_horizontal_target(-1), inverted ? true : false, LV_ANIM_OFF);
    } else if (ButtonPressed == 3) {
        select_menu_item_with_scroll(grid_horizontal_target(1), inverted ? false : true, LV_ANIM_OFF);
    } else if (ButtonPressed == 2) { // up
        navigate_vertical(-1, LV_ANIM_OFF);
    } else if (ButtonPressed == 4) { // down
        navigate_vertical(1, LV_ANIM_OFF);
    } else if (ButtonPressed == 1) {
        handle_menu_item_selection(selected_item_index);
    }
}



/**
 * @brief Selects a menu item and updates the display.
 */
void select_menu_item(int index, bool slide_left) {
    select_menu_item_with_scroll(index, slide_left, LV_ANIM_ON);
}

static void select_menu_item_with_scroll(int index, bool slide_left, lv_anim_enable_t scroll_anim) {
    if (is_animating) return; // Block input during animation
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;

    // Update selection for different layouts
    if (is_carousel_like_layout()) {
        if (index == selected_item_index && current_item_obj) return;
        selected_item_index = index;
        update_menu_item(slide_left);
        if (current_layout == MAIN_MENU_LAYOUT_HERO) update_hero_position();
    } else if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) {
        // Update selection for the paginated launcher.
        if (grid_cards) {
            // Remove highlight from previous selection
            if (selected_item_index >= 0 && selected_item_index < num_items && grid_cards[selected_item_index]) {
                if (is_compact_layout()) {
                    apply_compact_menu_tile(grid_cards[selected_item_index], false);
                } else {
                    apply_menu_launcher_selection_style(grid_cards[selected_item_index], false);
                }
            }

            // Highlight new selection with theme accent
            selected_item_index = index;
            if (grid_cards[selected_item_index]) {
                if (is_compact_layout()) {
                    apply_compact_menu_tile(grid_cards[selected_item_index], true);
                } else {
                    apply_menu_launcher_selection_style(grid_cards[selected_item_index], true);
                }

                scroll_launcher_card_to_view(selected_item_index);
            }
        }
    } else if (current_layout == MAIN_MENU_LAYOUT_LIST) {
        bool show_borders_list = settings_get_menu_item_borders(&G_Settings);
        if (list_buttons) {
            if (selected_item_index >= 0 && selected_item_index < num_items && list_buttons[selected_item_index]) {
                lv_obj_t *old_btn = list_buttons[selected_item_index];
                bool connected = esp_comm_manager_is_connected();
                int menu_index_prev = visible_index_to_menu_index(selected_item_index, connected);
                apply_card_style(old_btn, menu_surface_color,
                                 menu_items[menu_index_prev].border_color,
                                 show_borders_list ? 2 : 0, 6);
            }
            selected_item_index = index;
            if (list_buttons[selected_item_index]) {
                lv_obj_t *btn = list_buttons[selected_item_index];
                uint8_t theme = settings_get_menu_theme(&G_Settings);
                lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
                apply_card_selection_style(btn, accent);
                lv_obj_scroll_to_view(btn, scroll_anim);
            }
        }
    }
}

/**
 * @brief Handles the selection of menu items.
 */
static void handle_menu_item_selection(int item_index) {
    if (is_animating || item_index < 0 || item_index >= num_items) return;
    int index = visible_index_to_menu_index(item_index, esp_comm_manager_is_connected());
    menu_catalog_launch(menu_items[index].id, &main_menu_view);
}

/**
 * @brief Creates the paginated launcher.
 */
static void create_launcher_menu(void) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);

    int screen_width = layout.container_width;
    int cols = layout.columns;
    int margin = layout.margin;
    int avail_height = layout.content_height - layout.page_indicator_height;

    grid_cols = layout.columns;
    grid_rows = layout.rows;
    grid_card_width = layout.card_width;
    grid_card_height = layout.card_height;

    // Pages are fixed to the viewport and selected without queued scroll animations.
    grid_cards_container = lv_obj_create(menu_container);
    lv_obj_set_size(grid_cards_container, screen_width, avail_height);
    lv_obj_set_style_bg_opa(grid_cards_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid_cards_container, 0, 0);
    lv_obj_set_style_radius(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_all(grid_cards_container, 0, 0);
    lv_obj_set_style_pad_column(grid_cards_container, 0, 0);
    lv_obj_align(grid_cards_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(grid_cards_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_clear_flag(grid_cards_container, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_set_flex_flow(grid_cards_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid_cards_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(grid_cards_container, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(grid_cards_container, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(grid_cards_container, LV_SCROLLBAR_MODE_OFF);

    // Allocate cards array
    grid_cards = malloc(num_items * sizeof(lv_obj_t*));
    if (!grid_cards) {
        ESP_LOGE(TAG, "Failed to allocate launcher cards array");
        return;
    }

    bool connected = esp_comm_manager_is_connected();
    lv_obj_t *current_row = NULL;
    lv_obj_t *current_page = NULL;

    /* Launcher pages show multiple icons simultaneously, which can exceed the
     * no-PSRAM icon cache's slot count; reset pins so this pass's icons can
     * evict anything left pinned by a previous, now-destroyed screen. */
    asset_pack_reset_icon_pins();

    for (int i = 0; i < num_items; i++) {
        int menu_index = visible_index_to_menu_index(i, connected);
        int slot = i % layout.page_capacity;

        if (slot == 0) {
            current_page = lv_obj_create(grid_cards_container);
            lv_obj_set_size(current_page, screen_width, avail_height);
            lv_obj_set_flex_flow(current_page, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(current_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
#if GUI_LARGE_SCREEN
            lv_obj_set_flex_align(current_page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
#endif
            lv_obj_set_style_pad_all(current_page, margin, 0);
            lv_obj_set_style_pad_row(current_page, margin, 0);
            lv_obj_set_style_bg_opa(current_page, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(current_page, 0, 0);
            lv_obj_set_style_radius(current_page, 0, 0);
            lv_obj_clear_flag(current_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }

        // Start a new flex row every `cols` items
        if (slot % cols == 0) {
            current_row = lv_obj_create(current_page);
            lv_obj_set_width(current_row, LV_PCT(100));
            lv_obj_set_height(current_row, grid_card_height);
            lv_obj_set_flex_flow(current_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(current_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(current_row, margin, 0);
            lv_obj_set_style_pad_all(current_row, 0, 0);
            lv_obj_set_style_bg_opa(current_row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(current_row, 0, 0);
            lv_obj_set_style_radius(current_row, 0, 0);
        }

        // Create card inside the current row
        grid_cards[i] = lv_btn_create(current_row);
        gui_apply_pressed_style(grid_cards[i]);
        lv_obj_set_width(grid_cards[i], grid_card_width);
        lv_obj_set_height(grid_cards[i], LV_PCT(100));

        // Style card
        gui_menu_launcher_tile_apply(grid_cards[i], card_bg_enabled(), menu_surface_color);
        lv_obj_set_style_radius(grid_cards[i], GUI_RADIUS_MD, LV_PART_MAIN);
        lv_obj_set_style_pad_all(grid_cards[i], 0, LV_PART_MAIN);

        if (!is_compact_layout()) {
            // Compact tiles intentionally contain no image object.
            lv_obj_t *icon = lv_img_create(grid_cards[i]);
            const lv_img_dsc_t *item_icon = menu_item_icon(menu_index);
            lv_img_set_src(icon, item_icon);
            int reserved_for_label = (grid_card_height <= 70 ? 12 : 20);
#if GUI_LARGE_SCREEN
            reserved_for_label = 40;
#endif
            int icon_area_h = grid_card_height - reserved_for_label;
            if (icon_area_h < 10) icon_area_h = grid_card_height - reserved_for_label;
            int icon_target = LV_MIN((int)(grid_card_width * 0.78f), (int)(icon_area_h * 0.78f));
            if (icon_target < 16) icon_target = LV_MIN(grid_card_width - 4, icon_area_h);
#if GUI_LARGE_SCREEN
            icon_target = LV_MIN(icon_target, 112);
#endif
            lv_img_set_antialias(icon, false);
            lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);

            if (menu_item_icon_should_recolor(menu_index, item_icon)) {
                lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
            }
            lv_obj_set_style_clip_corner(icon, false, 0);

            lv_coord_t img_w = item_icon ? item_icon->header.w : 0;
            lv_coord_t img_h = item_icon ? item_icon->header.h : 0;
            int zoom_w = (img_w > 0) ? (icon_target * 256) / img_w : 256;
            int zoom_h = (img_h > 0) ? (icon_target * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            if (zoom > GUI_IMAGE_ZOOM_MAX) zoom = GUI_IMAGE_ZOOM_MAX;
            if (zoom < 64)  zoom = 64;
            lv_img_set_zoom(icon, zoom);
            lv_obj_refresh_self_size(icon);

            int displayed_h = (img_h * zoom) / 256;
            int top_offset = (icon_area_h - displayed_h) / 2;
            if (top_offset < 0) top_offset = 0;
            lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);
        }

        // Add label
        lv_obj_t *label = lv_label_create(grid_cards[i]);
        lv_label_set_text(label, menu_items[menu_index].name);
        // smaller font on small tiles
        const lv_font_t *lbl_font = accessibility_get_font_small();
#if GUI_LARGE_SCREEN
        lbl_font = accessibility_get_font_body();
#endif
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, menu_text_color, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, grid_card_width - (is_compact_layout() ? 12 : 8));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        if (is_compact_layout()) {
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
        }

        if (is_compact_layout()) apply_compact_menu_tile(grid_cards[i], false);

        lv_obj_add_event_cb(grid_cards[i], menu_button_click_handler, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    if (!is_compact_layout() || layout.page_indicator_height > 0) {
        launcher_page_indicator = lv_obj_create(menu_container);
        lv_obj_align(launcher_page_indicator, LV_ALIGN_BOTTOM_MID, 0, -1);
    }

    // Highlight selected card with theme accent
    if (grid_cards[selected_item_index]) {
        if (is_compact_layout()) {
            apply_compact_menu_tile(grid_cards[selected_item_index], true);
        } else {
            apply_menu_launcher_selection_style(grid_cards[selected_item_index], true);
        }

        scroll_launcher_card_to_view(selected_item_index);
    }
}

static void create_list_menu(void) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_LIST, num_items, &layout);
    int button_height = layout.list_button_height;
    int icon_target = layout.list_icon_target;
    bool show_borders = settings_get_menu_item_borders(&G_Settings);

    lv_obj_set_flex_flow(menu_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menu_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(menu_container, layout.list_pad, 0);
    lv_obj_set_style_pad_row(menu_container, layout.list_row_gap, 0);
    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(menu_container, LV_DIR_VER);
    // No elastic spring-back and no scrollbar: scrolling redraws only the
    // moved rows instead of stretch frames plus a scrollbar strip, which
    // keeps drag/flick scrolling smooth on low-refresh displays.
    lv_obj_clear_flag(menu_container, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(menu_container, LV_SCROLLBAR_MODE_OFF);

    if (list_buttons) {
        free(list_buttons);
        list_buttons = NULL;
    }

    list_buttons = calloc(num_items, sizeof(lv_obj_t *));
    if (!list_buttons) {
        ESP_LOGE(TAG, "failed to alloc list buttons");
        return;
    }

    /* List shows every menu item's icon simultaneously; see create_launcher_menu. */
    asset_pack_reset_icon_pins();

    bool connected = esp_comm_manager_is_connected();
    for (int i = 0; i < num_items; i++) {
        int menu_index = visible_index_to_menu_index(i, connected);
        lv_obj_t *btn = lv_btn_create(menu_container);
        gui_apply_pressed_style(btn);
        list_buttons[i] = btn;
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, button_height);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        // Shadow-free rows: per-pixel shadow blending on every row is the
        // dominant cost when scrolling; the accent border keeps focus visible.
        apply_card_style(btn, menu_surface_color, menu_items[menu_index].border_color,
                         show_borders ? 2 : 0, 0);
        lv_obj_set_style_radius(btn, GUI_RADIUS_SM, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, layout.list_button_pad, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, layout.list_column_gap, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        lv_obj_t *icon = lv_img_create(btn);
        const lv_img_dsc_t *item_icon = menu_item_icon(menu_index);
        lv_img_set_src(icon, item_icon);
        lv_img_set_antialias(icon, false);
        if (menu_item_icon_should_recolor(menu_index, item_icon)) {
            lv_obj_set_style_img_recolor(icon, menu_items[menu_index].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        }
        lv_coord_t img_w = item_icon ? item_icon->header.w : 0;
        lv_coord_t img_h = item_icon ? item_icon->header.h : 0;
        int zoom_w = (img_w > 0) ? (icon_target * 256) / img_w : 256;
        int zoom_h = (img_h > 0) ? (icon_target * 256) / img_h : 256;
        int zoom = LV_MIN(zoom_w, zoom_h);
        if (zoom > GUI_IMAGE_ZOOM_MAX) zoom = GUI_IMAGE_ZOOM_MAX;
        if (zoom < 64) zoom = 64;
        lv_img_set_zoom(icon, zoom);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, menu_items[menu_index].name);
        lv_obj_set_style_text_color(label, menu_text_color, 0);
        const lv_font_t *lbl_font = accessibility_get_font_body();
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_add_event_cb(btn, menu_button_click_handler, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    select_menu_item(selected_item_index, false);
}

static void cleanup_layout_arrays(void) {
    if (grid_cards) {
        free(grid_cards);
        grid_cards = NULL;
    }
    if (list_buttons) {
        free(list_buttons);
        list_buttons = NULL;
    }
    current_item_obj = NULL;
    carousel_cache = (carousel_card_cache_t){0};
    grid_cards_container = NULL;
    launcher_page_indicator = NULL;
    launcher_current_page = -1;
    carousel_prev_obj = NULL;
    carousel_next_obj = NULL;
    hero_pip_container = NULL;
}

static lv_timer_t *menu_refresh_timer = NULL;
static bool was_dual_comm_connected = false;
static uint32_t was_asset_pack_version = 0;

static lv_obj_t *create_menu_container(lv_obj_t *root) {
    lv_obj_t *container = lv_obj_create(root);
    lv_obj_set_size(container, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(container, 0, LV_PART_MAIN);
    return container;
}

static void menu_refresh_timer_cb(lv_timer_t *t) {
    bool connected = esp_comm_manager_is_connected();
    uint32_t pack_version = asset_pack_get_version();
    if (connected != was_dual_comm_connected || pack_version != was_asset_pack_version) {
        was_dual_comm_connected = connected;
        was_asset_pack_version = pack_version;

        cleanup_layout_arrays();
        if (menu_container && lv_obj_is_valid(menu_container)) {
            gui_screen_invalidate_bg_cache();
            lv_obj_clean(menu_container);
        }
        current_item_obj = NULL;
        carousel_cache = (carousel_card_cache_t){0};

        num_items = get_visible_menu_count(connected);
        if (selected_item_index >= num_items) selected_item_index = num_items - 1;

        init_menu_colors();

        if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) create_launcher_menu();
        else if (current_layout == MAIN_MENU_LAYOUT_LIST) create_list_menu();
        else {
            select_menu_item(selected_item_index, false);
            if (current_layout == MAIN_MENU_LAYOUT_HERO) create_hero_position_indicator();
        }

        gui_screen_apply_background(main_menu_view.root);
    }
}

/**
 * @brief Creates the main menu screen view.
 */
void main_menu_create(void) {
    free(menu_items);
    menu_items = menu_catalog_collect(MENU_PLACE_MAIN, false, &total_menu_items);
    if (!menu_items) {
        toast_show("Not enough memory for menu", TOAST_ERROR);
        return;
    }
    refresh_menu_surface_colors();
    display_manager_fill_screen(menu_bg_color);
    bool dual_comm_connected = esp_comm_manager_is_connected();
    was_dual_comm_connected = dual_comm_connected;
    was_asset_pack_version = asset_pack_get_version();
    
    if (!menu_refresh_timer) {
        menu_refresh_timer = lv_timer_create(menu_refresh_timer_cb, 1000, NULL);
    }
    num_items = get_visible_menu_count(dual_comm_connected);
    if (selected_item_index >= num_items) selected_item_index = 0;
    init_menu_colors(); // Initialize colors at runtime

    main_menu_layout_kind_t configured_layout =
        main_menu_layout_from_setting(settings_get_menu_layout(&G_Settings));
#if GUI_EPAPER
    /* The panel has no touch/animation affordance and only exposes PREV/NEXT.
     * A dense vertical list makes those factory buttons map naturally to the
     * existing Up/Down navigation contract and keeps the current selection
     * visible after every refresh. */
    configured_layout = MAIN_MENU_LAYOUT_LIST;
#endif
    current_layout = main_menu_layout_resolve_for_size(configured_layout, LV_HOR_RES, LV_VER_RES);

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(current_layout, num_items, &layout);

    main_menu_view.root = gui_screen_create_root(NULL, NULL, menu_bg_color, LV_OPA_TRANSP);
    menu_container = create_menu_container(main_menu_view.root);

    // Create menu based on layout
    if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) {
        create_launcher_menu();
    } else if (current_layout == MAIN_MENU_LAYOUT_LIST) {
        create_list_menu();
    } else {
        // Default carousel layout and the Hero (carousel-like) layout
        update_menu_item(false);
    }

    if (current_layout == MAIN_MENU_LAYOUT_HERO) {
        create_hero_position_indicator();
    }

    // Check if navigation buttons should be shown based on user setting
    // Also respect the original logic for device capabilities
    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
    /* The panel is physically circular; keep the edge pixels clear and let
     * the encoder/touch swipe provide previous/next navigation. */
    should_show_nav_buttons = false;
#endif
#if GUI_LARGE_TOUCH_UI && defined(CONFIG_USE_TOUCHSCREEN)
    should_show_nav_buttons = false;
#endif

    // Only show if the user wants them and the device supports them.
    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        // Check screen dimensions at runtime
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    // Launcher tiles and lists provide their own navigation affordances.
    if (should_show_nav_buttons && current_layout == MAIN_MENU_LAYOUT_CAROUSEL &&
        layout.carousel_show_previews) {
        should_show_nav_buttons = false;
    }
    if (should_show_nav_buttons && (current_layout == MAIN_MENU_LAYOUT_LAUNCHER ||
                                    current_layout == MAIN_MENU_LAYOUT_COMPACT ||
                                    current_layout == MAIN_MENU_LAYOUT_LIST)) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        // Create left navigation button
        left_nav_btn = lv_btn_create(lv_scr_act());
        gui_apply_pressed_style(left_nav_btn);
        
        int btn_size = layout.nav_button_size;
        int btn_margin = layout.nav_button_margin;
        
        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);
        
        // Position left button vertically centered at left edge
        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);
        
        // Add left arrow icon/text
        // create arrow label only, style it and center within the transparent button
        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, LV_SYMBOL_LEFT);
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(left_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(left_label, menu_text_color, 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 2);

        // Create right navigation button
        right_nav_btn = lv_btn_create(lv_scr_act());
        gui_apply_pressed_style(right_nav_btn);
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        // make button transparent and remove shadows/border
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);
        
        // Position right button vertically centered at right edge
        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);
        
        // Add right arrow icon/text
        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, LV_SYMBOL_RIGHT);
        // increase arrow size for better visibility
        lv_obj_set_style_text_font(right_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(right_label, menu_text_color, 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 2);
        
        ESP_LOGI(TAG, "Navigation buttons created - size: %d, margin: %d", btn_size, btn_margin);
        // ensure nav buttons are above other screen content (e.g., scrollable menu_container)
        lv_obj_move_foreground(left_nav_btn);
        lv_obj_move_foreground(right_nav_btn);
    }

    display_manager_add_status_bar(LV_HOR_RES > 128 ? "Main Menu" : "");

    // Position the menu relative to the status bar
    int status_bar_height = layout.status_bar_height;
    if (menu_container) {
        lv_obj_align(menu_container, layout.container_align, layout.container_x, layout.container_y);
        if (current_layout == MAIN_MENU_LAYOUT_LAUNCHER || current_layout == MAIN_MENU_LAYOUT_COMPACT) {
            lv_obj_set_size(menu_container, layout.container_width, layout.container_height);
        }
#if GUI_LARGE_SCREEN
        else if (current_layout == MAIN_MENU_LAYOUT_LIST) {
            lv_obj_set_size(menu_container, layout.container_width, layout.container_height);
            lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
        }
#endif
    }

    // also shift nav buttons down so they remain vertically centered with the menu
    if (left_nav_btn && current_layout != MAIN_MENU_LAYOUT_HERO) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
        // ensure nav button remains on top after repositioning
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn && current_layout != MAIN_MENU_LAYOUT_HERO) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
        // ensure nav button remains on top after repositioning
        lv_obj_move_foreground(right_nav_btn);
    }

    // HERO layout: anchor the arrows to the big icon's vertical center (the
    // icon sits raised above the content-area middle), mirroring the app
    // gallery so both hero screens look consistent.
    if (current_layout == MAIN_MENU_LAYOUT_HERO) {
        lv_area_t icon_area;
        if (carousel_cache.icon && lv_obj_is_valid(carousel_cache.icon)) {
            lv_obj_get_coords(carousel_cache.icon, &icon_area);
            lv_coord_t icon_center_y = (icon_area.y1 + icon_area.y2) / 2;
            if (left_nav_btn) {
                lv_obj_set_y(left_nav_btn, icon_center_y - lv_obj_get_height(left_nav_btn) / 2);
            }
            if (right_nav_btn) {
                lv_obj_set_y(right_nav_btn, icon_center_y - lv_obj_get_height(right_nav_btn) / 2);
            }
        }
    }
}

/**
 * @brief Destroys the main menu screen view.
 */
void main_menu_destroy(void) {
    is_animating = false;
    touch_started = touch_dragged = false;
    touch_scroll_target = NULL;
    lvgl_timer_del_safe(&menu_refresh_timer);

    if (main_menu_view.root) {
        lv_obj_clean(main_menu_view.root);
        lvgl_obj_del_safe(&main_menu_view.root);
        menu_container = NULL;
        main_menu_view.root = NULL;
    }

    cleanup_layout_arrays();
    free(menu_items);
    menu_items = NULL;
    total_menu_items = num_items = 0;

    // Clean up navigation buttons
    lvgl_obj_del_safe(&left_nav_btn);
    lvgl_obj_del_safe(&right_nav_btn);
}

void get_main_menu_callback(void **callback) {
    *callback = main_menu_view.input_callback;
}



View main_menu_view = {
    .root = NULL,
    .create = main_menu_create,
    .destroy = main_menu_destroy,
    .input_callback = menu_item_event_handler,
    .name = "Main Menu",
    .get_hardwareinput_callback = get_main_menu_callback, // Corrected typo
};
