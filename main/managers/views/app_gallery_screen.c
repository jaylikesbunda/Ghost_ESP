#include "managers/views/app_gallery_screen.h"
#include "sdkconfig.h"
#ifdef CONFIG_WITH_SCREEN
#include "managers/views/ghostchi_screen.h"
#endif
#if CONFIG_ENABLE_GHOSTSCRIPT
#include "managers/views/ghostscript_browser_view.h"
#endif
#include "managers/views/main_menu_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/plugin_runner_view.h"
#include "managers/views/sd_browser_screen.h"
#include "managers/views/terminal_screen.h"
#include "gui/gui_router.h"
#ifdef CONFIG_HAS_COMPASS
#include "managers/views/compass_screen.h"
#endif
#ifdef CONFIG_HAS_ENVIII
#include "managers/views/enviii_screen.h"
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
#include "managers/views/accelerometer_screen.h"
#endif
#include "managers/views/clock_screen.h"
#include "managers/views/cloud_store_screen.h"
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/views/audio_player_screen.h"
#endif

LV_IMG_DECLARE(speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
LV_IMG_DECLARE(enviii);
LV_IMG_DECLARE(accelerometer_icon);
LV_IMG_DECLARE(folder);
LV_IMG_DECLARE(description);
LV_IMG_DECLARE(storefront);

#include "managers/plugin_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/asset_pack.h"
#include "gui/main_menu_layout.h"
#include "gui/large_builtin_icons.h"
#include "gui/menu_catalog.h"
#include "gui/menu_item_style.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "gui/toast.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "core/memory_debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

static const char *TAG = "AppGalleryScreen";

static const lv_font_t *app_symbol_font_for_target(int target) {
#if LV_FONT_MONTSERRAT_32
    if (target > 40) return &lv_font_montserrat_32;
#else
    (void)target;
#endif
    return &lv_font_montserrat_24;
}

static inline int get_app_anim_duration(void) {
    return settings_get_reduced_motion(&G_Settings) ? 0 : 60;
}

#define ANIM_DURATION get_app_anim_duration()

static inline bool app_card_bg_enabled(void) {
    return settings_get_menu_card_bg(&G_Settings);
}

static inline void apply_app_card_style(lv_obj_t *obj, lv_color_t surface, lv_color_t border, int border_w, int shadow_w) {
    gui_menu_card_apply(obj, app_card_bg_enabled(), surface, border, border_w, shadow_w);
}

static inline void apply_app_card_selection_style(lv_obj_t *obj, lv_color_t accent) {
    gui_menu_card_apply_selected(obj, app_card_bg_enabled(), accent);
}

static void select_app_item(int index, bool slide_left);
static void apps_plugin_reload_done(void *arg);
static void scroll_app_launcher_card_to_view(int index);

lv_obj_t *apps_container;
static lv_obj_t *apps_root = NULL;
static lv_obj_t *current_app_obj = NULL;
static int selected_app_index = 0;
static bool s_native_apps_psram_warning_shown = false;

typedef struct {
    char name[PLUGIN_APP_NAME_MAX];
    char catalog_id[MENU_CONFIG_ID_LEN];
    const char *asset_key;
    const char *symbol_icon;
    const lv_img_dsc_t *icon;
    int palette_index;
    lv_color_t border_color;
    View *view;
    char plugin_id[PLUGIN_APP_ID_MAX];
    char accent_color[PLUGIN_APP_ACCENT_COLOR_MAX];
    bool disabled;
    bool is_category_folder;
    char category[PLUGIN_APP_CATEGORY_MAX];
} app_item_t;

#define MAX_APP_GALLERY_ITEMS (MENU_CONFIG_MAX + PLUGIN_APP_MAX_COUNT + 1)
static app_item_t *app_items = NULL;
static int s_app_items_capacity = 0;
static int num_apps = 0;
lv_obj_t *back_button = NULL;

static bool in_submenu = false;
static char current_category[PLUGIN_APP_CATEGORY_MAX] = "";

// Add navigation button objects
static lv_obj_t *left_nav_btn = NULL;
static lv_obj_t *right_nav_btn = NULL;
static lv_obj_t *carousel_prev_obj = NULL;
static lv_obj_t *carousel_next_obj = NULL;
static lv_obj_t *hero_pip_container = NULL;
static int touch_start_x;
static int touch_start_y;
static int touch_last_x;
static int touch_last_y;
static bool touch_started = false;
static bool touch_dragged = false;
static int touch_drag_axis = 0;
static const int SWIPE_THRESHOLD = 50;
static const int TAP_THRESHOLD = 14;
static const int DRAG_AXIS_THRESHOLD = 14;
static const int DRAG_AXIS_BIAS = 4;
static const int DRAG_DELTA_DEADZONE = 1;
static const int DRAG_MAX_STEP = 36;

static bool menu_item_selected = false;

static main_menu_layout_kind_t apps_layout = MAIN_MENU_LAYOUT_CAROUSEL;
static lv_obj_t **apps_grid_cards = NULL;
static lv_obj_t **apps_list_buttons = NULL;
static lv_obj_t *grid_cards_container = NULL;
static int apps_grid_cols = 1;
static lv_obj_t *launcher_page_indicator = NULL;
static int launcher_current_page = -1;
static lv_color_t apps_bg_color;
static lv_color_t apps_surface_color;
static lv_color_t apps_text_color;
static volatile bool apps_plugin_reload_in_progress = false;
static bool apps_allow_plugin_icon_load = false;
static StackType_t *apps_plugin_reload_stack = NULL;
static StaticTask_t *apps_plugin_reload_tcb = NULL;

static void apply_app_launcher_selection_style(lv_obj_t *card, bool selected) {
    if (!card) return;
    selected = selected && gui_menu_focus_visible();

    lv_color_t label_color = apps_text_color;
    if (selected) {
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        gui_menu_launcher_tile_apply_selected(card, app_card_bg_enabled(), accent);
        label_color = accent;
    } else {
        gui_menu_launcher_tile_apply(card, app_card_bg_enabled(), apps_surface_color);
    }

    lv_obj_t *label = lv_obj_get_child(card, -1);
    if (label) lv_obj_set_style_text_color(label, label_color, 0);
}

static inline bool apps_is_compact_layout(void) {
    return apps_layout == MAIN_MENU_LAYOUT_COMPACT;
}

static inline bool apps_is_carousel_like_layout(void) {
    return apps_layout == MAIN_MENU_LAYOUT_CAROUSEL || apps_layout == MAIN_MENU_LAYOUT_HERO;
}

static void apply_compact_app_tile(lv_obj_t *obj, bool selected) {
    if (!obj) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t accent_text = theme_palette_is_bright(theme) ? lv_color_black() : lv_color_white();
    lv_obj_t *label = lv_obj_get_child(obj, 0);
    gui_menu_compact_tile_apply(obj, label, selected, app_card_bg_enabled(),
                                apps_surface_color, apps_text_color, accent, accent_text);
}

#define PLUGIN_RELOAD_STACK_BYTES 32768

static bool apps_native_plugins_enabled(void) {
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
}

static int app_items_capacity_for_board(void) {
    return apps_native_plugins_enabled() ? MAX_APP_GALLERY_ITEMS : (menu_catalog_count() + 1);
}

static bool ensure_app_items(void) {
    int capacity = app_items_capacity_for_board();
    if (app_items) return true;
    app_items = spiram_calloc((size_t)capacity, sizeof(*app_items));
    if (!app_items) {
        ESP_LOGE(TAG, "Failed to allocate app gallery items");
        return false;
    }
    s_app_items_capacity = capacity;
    return true;
}

static void select_app_item(int index, bool slide_left);

static const lv_img_dsc_t *app_item_icon(int index) {
    if (!app_items || index < 0 || index >= num_apps) return NULL;
    const lv_img_dsc_t *fallback = app_items[index].icon;
    if (apps_allow_plugin_icon_load && app_items[index].plugin_id[0] != '\0') {
        const plugin_app_manifest_t *app = plugin_manager_find(app_items[index].plugin_id);
        const lv_img_dsc_t *plugin_icon = plugin_manager_get_icon(app);
        if (plugin_icon) fallback = plugin_icon;
    }
    const lv_img_dsc_t *icon = asset_pack_get_icon(app_items[index].asset_key, fallback);
    icon = gui_large_builtin_icon(icon);
    if (icon == fallback && fallback) {
        icon = gui_large_builtin_icon(asset_pack_get_app_icon(fallback));
    }
    return icon;
}

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

static bool app_item_icon_should_recolor(int index, const lv_img_dsc_t *icon) {
    if (!app_items || index < 0 || index >= num_apps || !icon) return false;
    if (app_items[index].plugin_id[0] != '\0' && apps_allow_plugin_icon_load) {
        const plugin_app_manifest_t *app = plugin_manager_find(app_items[index].plugin_id);
        const lv_img_dsc_t *plugin_icon = plugin_manager_get_icon(app);
        if (plugin_icon && icon == plugin_icon) {
            const lv_img_dsc_t *pack_icon = asset_pack_get_icon(app_items[index].asset_key, plugin_icon);
            pack_icon = gui_large_builtin_icon(pack_icon);
            return icon == pack_icon;
        }
    }
    return icon == gui_large_builtin_icon(app_items[index].icon);
}

static const char *app_item_symbol_icon(int index) {
    if (!app_items || index < 0 || index >= num_apps) return NULL;
    return app_items[index].symbol_icon;
}

static lv_obj_t *create_app_symbol_icon(lv_obj_t *parent, const char *symbol, lv_color_t color, const lv_font_t *font) {
    if (!parent || !symbol) return NULL;
    lv_obj_t *icon = lv_label_create(parent);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, color, 0);
    lv_obj_set_style_text_font(icon, font ? font : &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return icon;
}

static void update_app_carousel_preview(lv_obj_t **preview_ptr, int app_index, int x_offset,
                                        const main_menu_layout_metrics_t *layout) {
    if (!preview_ptr || !layout || !layout->carousel_show_previews ||
        app_index < 0 || app_index >= num_apps) return;

    lv_obj_t *preview = *preview_ptr;
    if (!preview) {
        preview = lv_obj_create(apps_container);
        *preview_ptr = preview;
        lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_border_width(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(preview, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(preview, GUI_RADIUS_LG, LV_PART_MAIN);
    } else {
        lv_obj_clean(preview);
    }

    lv_obj_set_size(preview, layout->carousel_preview_size, layout->carousel_preview_size);
    lv_obj_set_style_bg_color(preview, apps_surface_color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(preview, app_card_bg_enabled() ? LV_OPA_50 : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(preview, app_items[app_index].disabled ? LV_OPA_30 : LV_OPA_60, LV_PART_MAIN);
    lv_obj_align(preview, LV_ALIGN_CENTER, x_offset, 0);

    const char *symbol = app_item_symbol_icon(app_index);
    if (symbol) {
        const lv_font_t *symbol_font =
            app_symbol_font_for_target(layout->carousel_preview_icon_target);
        lv_obj_t *icon = create_app_symbol_icon(preview, symbol, app_items[app_index].border_color,
                                                 symbol_font);
        if (icon) lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    const lv_img_dsc_t *item_icon = app_item_icon(app_index);
    if (!item_icon) return;
    lv_obj_t *icon = lv_img_create(preview);
    lv_img_set_src(icon, item_icon);
    lv_img_set_antialias(icon, false);
    int max_zoom = GUI_LARGE_SCREEN ? GUI_IMAGE_ZOOM_MAX : 512;
    gui_menu_image_fit(icon, item_icon, layout->carousel_preview_icon_target, max_zoom);
    if (app_item_icon_should_recolor(app_index, item_icon)) {
        lv_obj_set_style_img_recolor(icon, app_items[app_index].border_color, 0);
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
    }
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 0);
}

static void update_app_carousel_previews(void) {
    if (apps_layout != MAIN_MENU_LAYOUT_CAROUSEL) return;
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_CAROUSEL, num_apps, &layout);
    if (!layout.carousel_show_previews || num_apps < 2) return;

    int previous = (selected_app_index + num_apps - 1) % num_apps;
    int next = (selected_app_index + 1) % num_apps;
    update_app_carousel_preview(&carousel_prev_obj, previous, -layout.carousel_preview_offset, &layout);
    update_app_carousel_preview(&carousel_next_obj, next, layout.carousel_preview_offset, &layout);
    if (current_app_obj) lv_obj_move_foreground(current_app_obj);
}

static bool app_touch_started_and_ended_in(lv_obj_t *obj, const lv_point_t *end) {
    if (!obj || !end) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return touch_start_x >= area.x1 && touch_start_x <= area.x2 &&
           touch_start_y >= area.y1 && touch_start_y <= area.y2 &&
           end->x >= area.x1 && end->x <= area.x2 &&
           end->y >= area.y1 && end->y <= area.y2;
}

static void refresh_apps_surface_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    apps_bg_color = lv_color_hex(theme_palette_get_background(theme));
    apps_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    apps_text_color = lv_color_hex(theme_palette_get_text(theme));
}

static bool parse_accent_color(const char *text, lv_color_t *out) {
    if (!text || !out) return false;
    if (text[0] == '#') text++;
    if (strlen(text) != 6) return false;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 16);
    if (!end || *end != '\0' || value > 0xFFFFFFUL) return false;
    *out = lv_color_hex((uint32_t)value);
    return true;
}

static void add_back_app_item(void) {
    if (!app_items || num_apps >= s_app_items_capacity) return;
    strcpy(app_items[num_apps].name, "Back");
    app_items[num_apps].icon = NULL;
    app_items[num_apps].palette_index = 0;
    app_items[num_apps].view = NULL;
    num_apps++;
}

static void add_plugin_app_item(const plugin_app_manifest_t *app) {
    snprintf(app_items[num_apps].name, sizeof(app_items[num_apps].name), "%s", app->name);
    snprintf(app_items[num_apps].catalog_id, sizeof(app_items[num_apps].catalog_id), "plugin:%s", app->id);
    app_items[num_apps].asset_key = NULL;
    app_items[num_apps].icon = &GESPAppGallery;
    app_items[num_apps].palette_index = 3;
    app_items[num_apps].view = &plugin_runner_view;
    app_items[num_apps].disabled = false;
    strncpy(app_items[num_apps].plugin_id, app->id, sizeof(app_items[num_apps].plugin_id) - 1);
    strncpy(app_items[num_apps].accent_color, app->accent_color, sizeof(app_items[num_apps].accent_color) - 1);
    strncpy(app_items[num_apps].category, app->category, sizeof(app_items[num_apps].category) - 1);
    num_apps++;
}

static void add_plugin_category_folders(void) {
    if (in_submenu) return;
    char (*category_names)[PLUGIN_APP_CATEGORY_MAX] =
        calloc(PLUGIN_APP_MAX_COUNT, sizeof(*category_names));
    if (!category_names) return;
    bool has_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
    int plugin_count = plugin_manager_count();

    int num_categories = 0;
    for (int i = 0; i < plugin_count; ++i) {
        const plugin_app_manifest_t *app = plugin_manager_get(i);
        if (!app) continue;
        if (app->requires_psram && !has_psram) continue;
        if (app->category[0] == '\0') continue;
        char id[MENU_CONFIG_ID_LEN];
        snprintf(id, sizeof(id), "plugin:%s", app->id);
        if (menu_config_find(&G_Settings.menu_config, id)) continue;

        bool already_seen = false;
        for (int j = 0; j < num_categories; ++j) {
            if (strcmp(category_names[j], app->category) == 0) {
                already_seen = true;
                break;
            }
        }
        if (!already_seen && num_categories < PLUGIN_APP_MAX_COUNT) {
            strncpy(category_names[num_categories], app->category, PLUGIN_APP_CATEGORY_MAX - 1);
            category_names[num_categories][PLUGIN_APP_CATEGORY_MAX - 1] = '\0';
            num_categories++;
        }
    }

    for (int j = 0; j < num_categories && num_apps < s_app_items_capacity - 1; ++j) {
        strncpy(app_items[num_apps].category, category_names[j], sizeof(app_items[num_apps].category) - 1);
        snprintf(app_items[num_apps].name, sizeof(app_items[num_apps].name), "%s", category_names[j]);
        app_items[num_apps].asset_key = "folder";
        app_items[num_apps].icon = &folder;
        app_items[num_apps].palette_index = 1;
        app_items[num_apps].view = NULL;
        app_items[num_apps].disabled = false;
        app_items[num_apps].is_category_folder = true;
        num_apps++;
    }
    free(category_names);
}

static void add_plugin_app_items_flat(void) {
    bool has_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
    int plugin_count = plugin_manager_count();

    if (in_submenu) {
        for (int i = 0; i < plugin_count && num_apps < s_app_items_capacity - 1; ++i) {
            const plugin_app_manifest_t *app = plugin_manager_get(i);
            if (!app) continue;
            if (app->requires_psram && !has_psram) continue;
            if (strcmp(app->category, current_category) != 0) continue;
            char id[MENU_CONFIG_ID_LEN];
            snprintf(id, sizeof(id), "plugin:%s", app->id);
            if (menu_config_find(&G_Settings.menu_config, id)) continue;
            add_plugin_app_item(app);
        }
        return;
    }

    for (int i = 0; i < plugin_count && num_apps < s_app_items_capacity - 1; ++i) {
        const plugin_app_manifest_t *app = plugin_manager_get(i);
        if (!app) continue;
        if (app->requires_psram && !has_psram) continue;
        if (app->category[0] != '\0') continue;
        add_plugin_app_item(app);
    }
}

static void rebuild_app_items(bool include_loaded_plugins) {
    num_apps = 0;
    if (!ensure_app_items()) return;
    memset(app_items, 0, sizeof(*app_items) * (size_t)s_app_items_capacity);

    if (in_submenu) {
        add_back_app_item();
        if (include_loaded_plugins) add_plugin_app_items_flat();
        return;
    }

    /* Preserve default folder-first behavior. Explicitly customized plugins
     * appear at the gallery root, where their saved order can be honored. */
    bool customized = false;
    for (int i = 0; i < G_Settings.menu_config.count; ++i) {
        if (G_Settings.menu_config.entries[i].order[1] != MENU_ORDER_DEFAULT) customized = true;
    }
    if (include_loaded_plugins && !customized) add_plugin_category_folders();

    int count = 0;
    menu_catalog_item_t *items = menu_catalog_collect(MENU_PLACE_APPS, true, &count);
    for (int i = 0; items && i < count && num_apps < s_app_items_capacity - 1; ++i) {
        const menu_catalog_item_t *item = &items[i];
        if (strncmp(item->id, "plugin:", 7) == 0) {
            if (!include_loaded_plugins) continue;
            const plugin_app_manifest_t *app = plugin_manager_find(item->id + 7);
            if (!app) continue;
            if (app->category[0] && !menu_config_find(&G_Settings.menu_config, item->id)) continue;
            add_plugin_app_item(app);
            continue;
        }
        app_item_t *dst = &app_items[num_apps++];
        snprintf(dst->name, sizeof(dst->name), "%s", item->name);
        snprintf(dst->catalog_id, sizeof(dst->catalog_id), "%s", item->id);
        dst->asset_key = item->asset_key;
        dst->icon = item->icon;
        dst->view = item->view;
    }
    free(items);
    if (include_loaded_plugins && customized) add_plugin_category_folders();
    add_back_app_item();
}

static void plugin_reload_task_fn(void *arg) {
    (void)arg;
    plugin_manager_reload();
    display_manager_run_on_lvgl(apps_plugin_reload_done, NULL);
    vTaskDelete(NULL);
}

static void start_plugin_reload_async(void) {
    if (apps_plugin_reload_in_progress) {
        toast_show_duration("Still scanning SD apps...", TOAST_INFO, 900);
        return;
    }
    apps_plugin_reload_in_progress = true;
    toast_show_duration("Scanning SD apps...", TOAST_INFO, 1200);
    if (!apps_plugin_reload_stack) {
        apps_plugin_reload_stack = spiram_malloc(PLUGIN_RELOAD_STACK_BYTES);
    }
    if (!apps_plugin_reload_stack) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to allocate plugin reload task stack");
        toast_show_duration("App scan failed: no task memory", TOAST_ERROR, 2200);
        return;
    }
    if (!apps_plugin_reload_tcb) {
        apps_plugin_reload_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!apps_plugin_reload_tcb) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to allocate plugin reload TCB");
        toast_show_duration("App scan failed: no task memory", TOAST_ERROR, 2200);
        return;
    }
    if (xTaskCreateStatic(plugin_reload_task_fn, "plugin_reload", PLUGIN_RELOAD_STACK_BYTES, NULL, 5,
                          apps_plugin_reload_stack, apps_plugin_reload_tcb) == NULL) {
        apps_plugin_reload_in_progress = false;
        ESP_LOGE(TAG, "Failed to create plugin reload task");
        toast_show_duration("App scan failed: task start", TOAST_ERROR, 2200);
    }
}

// Use the theme accent for all app borders/icons so the gallery reads as a
// single color; plugin-provided accent colors still take precedence.
static void init_app_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    refresh_apps_surface_colors();
    lv_color_t icon_color = lv_color_hex(theme_palette_get_accent(theme));
    for (int i = 0; i < num_apps; ++i) {
        if (!parse_accent_color(app_items[i].accent_color, &app_items[i].border_color)) {
            app_items[i].border_color = icon_color;
        }
    }
}

// Animation callback wrapper
static void anim_set_x(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_coord_t curr = lv_obj_get_x(o);
    if (curr == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

static void anim_set_opa(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    lv_opa_t curr = lv_obj_get_style_opa(o, 0);
    if (curr == (lv_opa_t)v) return;
    lv_obj_set_style_opa(o, v, 0);
}

static bool apps_carousel_next_slide_left = false;
static bool apps_is_animating = false;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *icon;
    lv_obj_t *label;
    const lv_img_dsc_t *icon_src;
    const char *symbol_src;
    const char *label_text;
    lv_color_t border_color;
    bool icon_recolor_enabled;
    bool icon_is_symbol;
    int item_index;
} apps_carousel_cache_t;

static apps_carousel_cache_t apps_carousel_cache = {0};

static void apps_cancel_carousel_anims(void) {
    if (current_app_obj) lv_anim_del(current_app_obj, NULL);
    if (apps_carousel_cache.card && apps_carousel_cache.card != current_app_obj) {
        lv_anim_del(apps_carousel_cache.card, NULL);
    }
}

static void apps_cleanup_layout(void) {
    if (apps_grid_cards) {
        free(apps_grid_cards);
        apps_grid_cards = NULL;
    }
    if (apps_list_buttons) {
        free(apps_list_buttons);
        apps_list_buttons = NULL;
    }
    grid_cards_container = NULL;
    launcher_page_indicator = NULL;
    launcher_current_page = -1;
    carousel_prev_obj = NULL;
    carousel_next_obj = NULL;
    hero_pip_container = NULL;
}

static void scroll_app_launcher_card_to_view(int index) {
    if (!grid_cards_container || !apps_grid_cards || index < 0 || index >= num_apps) return;

    lv_obj_t *card = apps_grid_cards[index];
    if (!card || !lv_obj_is_valid(card)) return;

    /* layout.screen_width/page_capacity come from pure arithmetic on the
     * screen size, not live geometry, so forcing a full flex relayout of
     * every page/card here (previously on every navigation press) bought
     * nothing but cost. */
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);
    int page = index / layout.page_capacity;
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
                                       lv_color_hex(theme_palette_get_accent(theme)), apps_text_color);
    }
}

static lv_obj_t *create_app_carousel_card(const main_menu_layout_metrics_t *layout,
                                          int x_offset, lv_opa_t opacity) {
    int app_idx = selected_app_index;
    lv_obj_t *card = lv_btn_create(apps_container);
    gui_apply_pressed_style(card);
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_carousel_cache.card = card;

    int card_border_w = settings_get_menu_item_borders(&G_Settings) ? 2 : 0;
    apply_app_card_style(card, apps_surface_color, app_items[app_idx].border_color, card_border_w, 8);
    lv_obj_set_style_radius(card, GUI_RADIUS_LG, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(card, false, 0);
    lv_obj_set_style_opa(card, opacity, 0);
    apps_carousel_cache.border_color = app_items[app_idx].border_color;
    apps_carousel_cache.item_index = selected_app_index;

    lv_obj_set_size(card, layout->carousel_button_size, layout->carousel_button_size);
    lv_obj_align(card, LV_ALIGN_CENTER, x_offset, 0);

    const char *item_symbol = app_item_symbol_icon(app_idx);
    const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(app_idx);
    if (item_symbol) {
        const lv_font_t *symbol_font =
            app_symbol_font_for_target(layout->carousel_icon_target);
        lv_obj_t *icon = create_app_symbol_icon(card, item_symbol, app_items[app_idx].border_color,
                                                 symbol_font);
        if (icon) {
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, layout->carousel_icon_y_offset);
            apps_carousel_cache.icon = icon;
            apps_carousel_cache.icon_src = NULL;
            apps_carousel_cache.symbol_src = item_symbol;
            apps_carousel_cache.icon_recolor_enabled = false;
            apps_carousel_cache.icon_is_symbol = true;
        }
    } else if (item_icon) {
        lv_obj_t *icon = lv_img_create(card);
        lv_img_set_src(icon, item_icon);
        lv_img_set_antialias(icon, false);
        bool recolor_enabled = app_item_icon_should_recolor(app_idx, item_icon);
        if (recolor_enabled) {
            lv_obj_set_style_img_recolor(icon, app_items[app_idx].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        }
        lv_obj_set_style_clip_corner(icon, false, 0);

        int max_zoom = GUI_LARGE_SCREEN ? GUI_IMAGE_ZOOM_MAX : 512;
        gui_menu_image_fit(icon, item_icon, layout->carousel_icon_target, max_zoom);
        int icon_x_offset = 0;
#ifdef CONFIG_WITH_SCREEN
        if (app_items[app_idx].view == &ghostchi_view) {
            icon_x_offset = 9;
        }
#endif
        lv_obj_align(icon, LV_ALIGN_CENTER, icon_x_offset, layout->carousel_icon_y_offset);
        apps_carousel_cache.icon = icon;
        apps_carousel_cache.icon_src = item_icon;
        apps_carousel_cache.symbol_src = NULL;
        apps_carousel_cache.icon_recolor_enabled = recolor_enabled;
        apps_carousel_cache.icon_is_symbol = false;
    }

    if (layout->carousel_show_label) {
        lv_obj_t *label = lv_label_create(card);
        const char *label_text = app_items[app_idx].name;
        if (app_items[app_idx].view == NULL && !app_items[app_idx].is_category_folder) label_text = "< Back";
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_font(label, accessibility_get_font_body(), 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        if (asset_pack_is_loaded()) {
            lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_60, 0);
            lv_obj_set_style_radius(label, 3, 0);
            lv_obj_set_style_pad_hor(label, 6, 0);
            lv_obj_set_style_pad_ver(label, 1, 0);
        }
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);
        apps_carousel_cache.label = label;
        apps_carousel_cache.label_text = label_text;
    }
    return card;
}

static lv_obj_t *create_app_hero_card(const main_menu_layout_metrics_t *layout,
                                      int x_offset, lv_opa_t opacity) {
    int app_idx = selected_app_index;
    lv_obj_t *card = lv_btn_create(apps_container);
    gui_apply_pressed_style(card);
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_carousel_cache.card = card;

    /* Flipper-style hero: one big icon + title, minimal chrome. No card
     * surface, no borders, no shadows. */
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_set_style_opa(card, opacity, 0);
    apps_carousel_cache.border_color = app_items[app_idx].border_color;
    apps_carousel_cache.item_index = selected_app_index;

    lv_obj_set_size(card, layout->screen_width, layout->screen_height);
    lv_obj_align(card, LV_ALIGN_CENTER, x_offset, 0);

    const char *item_symbol = app_item_symbol_icon(app_idx);
    const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(app_idx);
    if (item_symbol) {
        lv_obj_t *icon = create_app_symbol_icon(card, item_symbol, app_items[app_idx].border_color,
                                                &lv_font_montserrat_24);
        if (icon) {
            lv_obj_align(icon, LV_ALIGN_CENTER, 0, layout->hero_icon_y_offset);
            /* Symbol glyphs are fixed-size font glyphs; zoom the label around
             * its own center so they fill the same target box as image icons
             * and every hero icon renders at the same size. Keep the 24px
             * font because it contains the complete LVGL symbol set, including
             * folder symbols, then scale its glyph to the target. */
            lv_obj_set_style_transform_pivot_x(icon, lv_obj_get_width(icon) / 2, 0);
            lv_obj_set_style_transform_pivot_y(icon, lv_obj_get_height(icon) / 2, 0);
            lv_obj_set_style_transform_zoom(icon,
                                            (lv_coord_t)((layout->hero_icon_target * 256) / 24),
                                            0);
            apps_carousel_cache.icon = icon;
            apps_carousel_cache.icon_src = NULL;
            apps_carousel_cache.symbol_src = item_symbol;
            apps_carousel_cache.icon_recolor_enabled = false;
            apps_carousel_cache.icon_is_symbol = true;
        }
    } else if (item_icon) {
        lv_obj_t *icon = lv_img_create(card);
        lv_img_set_src(icon, item_icon);
        lv_img_set_antialias(icon, false);
        bool recolor_enabled = app_item_icon_should_recolor(app_idx, item_icon);
        if (recolor_enabled) {
            lv_obj_set_style_img_recolor(icon, app_items[app_idx].border_color, 0);
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
        }
        lv_obj_set_style_clip_corner(icon, false, 0);

        gui_menu_image_fit(icon, item_icon, layout->hero_icon_target, 512);
#if GUI_LARGE_SCREEN
        gui_menu_image_fit(icon, item_icon, layout->hero_icon_target, GUI_IMAGE_ZOOM_MAX);
#endif
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, layout->hero_icon_y_offset);
        apps_carousel_cache.icon = icon;
        apps_carousel_cache.icon_src = item_icon;
        apps_carousel_cache.symbol_src = NULL;
        apps_carousel_cache.icon_recolor_enabled = recolor_enabled;
        apps_carousel_cache.icon_is_symbol = false;
    }

    lv_obj_t *label = lv_label_create(card);
    const char *label_text = app_items[app_idx].name;
    if (app_items[app_idx].view == NULL && !app_items[app_idx].is_category_folder) {
        label_text = "< Back";
    }
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, accessibility_get_font_title(), 0);
    lv_obj_set_style_text_color(label, apps_text_color, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0,
                 layout->hero_icon_y_offset + layout->hero_icon_target / 2 + GUI_GRID * 2);
    apps_carousel_cache.label = label;
    apps_carousel_cache.label_text = label_text;

    return card;
}

static lv_obj_t *create_current_app_card(const main_menu_layout_metrics_t *layout,
                                         int x_offset, lv_opa_t opacity) {
    if (apps_layout == MAIN_MENU_LAYOUT_HERO) {
        return create_app_hero_card(layout, x_offset, opacity);
    }
    return create_app_carousel_card(layout, x_offset, opacity);
}

static void update_app_hero_position(void) {
    if (!hero_pip_container || apps_layout != MAIN_MENU_LAYOUT_HERO || num_apps < 1) return;

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_HERO, num_apps, &layout);
    int cap = layout.hero_pip_count < 1 ? 1 : layout.hero_pip_count;
    int pip_count = num_apps < cap ? num_apps : cap;
    int current = (num_apps <= 1) ? 0 : (selected_app_index * (pip_count - 1)) / (num_apps - 1);

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
        lv_obj_set_style_bg_color(dot, selected ? accent : apps_text_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, selected ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }
}

static void create_app_hero_position_indicator(void) {
    if (apps_layout != MAIN_MENU_LAYOUT_HERO) return;
    if (!hero_pip_container) {
        hero_pip_container = lv_obj_create(apps_menu_view.root);
        lv_obj_align(hero_pip_container, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 8));
    } else if (lv_obj_is_valid(hero_pip_container)) {
        lv_obj_clean(hero_pip_container);
    }
    lv_obj_move_foreground(hero_pip_container);
    update_app_hero_position();
}

static void apps_carousel_fade_in_ready_cb(lv_anim_t *a) {
    (void)a;
    apps_is_animating = false;
}

static void apps_carousel_fade_out_ready_cb(lv_anim_t *a) {
    lv_obj_t *old_card = (lv_obj_t *)a->var;
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);
    int start_x = apps_carousel_next_slide_left ? layout.carousel_transition_distance :
                                                  -layout.carousel_transition_distance;

    if (old_card && lv_obj_is_valid(old_card)) lv_obj_del(old_card);
    /* The deleted card no longer owns any cached asset images. Allow those
     * slots to be reused before resolving the incoming card's icon. */
    asset_pack_reset_icon_pins();
    current_app_obj = create_current_app_card(&layout, start_x, LV_OPA_TRANSP);
    update_app_carousel_previews();

    lv_anim_t move_in;
    lv_anim_init(&move_in);
    lv_anim_set_var(&move_in, current_app_obj);
    lv_anim_set_values(&move_in, start_x, 0);
    lv_anim_set_time(&move_in, ANIM_DURATION);
    lv_anim_set_path_cb(&move_in, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&move_in, anim_set_x);
    lv_anim_start(&move_in);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, current_app_obj);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, ANIM_DURATION);
    lv_anim_set_exec_cb(&fade_in, anim_set_opa);
    lv_anim_set_ready_cb(&fade_in, apps_carousel_fade_in_ready_cb);
    lv_anim_start(&fade_in);
}

static void update_app_item(bool move_left) {
    if (!app_items || num_apps <= 0) return;

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);

    if (!current_app_obj) {
        current_app_obj = create_current_app_card(&layout, 0, LV_OPA_COVER);
        update_app_carousel_previews();
        apps_is_animating = false;
        return;
    }

    apps_is_animating = true;
    apps_carousel_next_slide_left = move_left;

    int duration = ANIM_DURATION;
    if (duration <= 0) {
        lv_obj_del(current_app_obj);
        current_app_obj = create_current_app_card(&layout, 0, LV_OPA_COVER);
        update_app_carousel_previews();
        apps_is_animating = false;
        return;
    }

    int end_x = move_left ? -layout.carousel_transition_distance :
                            layout.carousel_transition_distance;
    lv_anim_t move_out;
    lv_anim_init(&move_out);
    lv_anim_set_var(&move_out, current_app_obj);
    lv_anim_set_values(&move_out, 0, end_x);
    lv_anim_set_time(&move_out, duration);
    lv_anim_set_path_cb(&move_out, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&move_out, anim_set_x);
    lv_anim_set_ready_cb(&move_out, apps_carousel_fade_out_ready_cb);
    lv_anim_start(&move_out);

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, current_app_obj);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, duration);
    lv_anim_set_exec_cb(&fade_out, anim_set_opa);
    lv_anim_start(&fade_out);
}

static void create_apps_launcher_menu(void) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);

    int screen_width = layout.container_width;
    int cols = layout.columns;
    int margin = layout.margin;
    int avail_height = layout.content_height - layout.page_indicator_height;
    int card_width = layout.card_width;
    int card_height = layout.card_height;
    apps_grid_cols = cols > 0 ? cols : 1;

    apps_cleanup_layout();

    grid_cards_container = lv_obj_create(apps_container);
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

    apps_grid_cards = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_grid_cards) {
        ESP_LOGE(TAG, "failed to alloc app launcher cards");
        return;
    }

    lv_obj_t *current_row = NULL;
    lv_obj_t *current_page = NULL;

    /* Launcher pages show multiple icons simultaneously, which can exceed the
     * no-PSRAM icon cache's slot count; reset pins so this pass's icons can
     * evict anything left pinned by a previous, now-destroyed screen. */
    asset_pack_reset_icon_pins();

    for (int i = 0; i < num_apps; ++i) {
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

        if (slot % cols == 0) {
            current_row = lv_obj_create(current_page);
            lv_obj_set_width(current_row, LV_PCT(100));
            lv_obj_set_height(current_row, card_height);
            lv_obj_set_flex_flow(current_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(current_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(current_row, margin, 0);
            lv_obj_set_style_pad_all(current_row, 0, 0);
            lv_obj_set_style_bg_opa(current_row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(current_row, 0, 0);
            lv_obj_set_style_radius(current_row, 0, 0);
        }

        lv_obj_t *card = lv_btn_create(current_row);
        gui_apply_pressed_style(card);
        apps_grid_cards[i] = card;
        lv_obj_set_width(card, card_width);
        lv_obj_set_height(card, LV_PCT(100));

        gui_menu_launcher_tile_apply(card, app_card_bg_enabled(), apps_surface_color);
        lv_obj_set_style_radius(card, GUI_RADIUS_MD, LV_PART_MAIN);
        lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);

        if (!apps_is_compact_layout()) {
            int reserved_for_label = (card_height <= 70 ? 12 : 20);
#if GUI_LARGE_SCREEN
            reserved_for_label = 40;
#endif
            int icon_area_h = card_height - reserved_for_label;
            if (icon_area_h < 10) icon_area_h = card_height - reserved_for_label;
            int icon_target = LV_MIN((int)(card_width * 0.78f), (int)(icon_area_h * 0.78f));
            if (icon_target < 16) icon_target = LV_MIN(card_width - 4, icon_area_h);
#if GUI_LARGE_SCREEN
            icon_target = LV_MIN(icon_target, 112);
#endif

            const char *item_symbol = app_item_symbol_icon(i);
            const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(i);
            if (item_symbol) {
                const lv_font_t *symbol_font = app_symbol_font_for_target(icon_target);
                lv_obj_t *icon = create_app_symbol_icon(card, item_symbol, app_items[i].border_color, symbol_font);
                if (icon) {
                    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, (icon_area_h - 24) / 2);
                }
            } else if (item_icon) {
                lv_obj_t *icon = lv_img_create(card);
                lv_img_set_src(icon, item_icon);
                lv_img_set_antialias(icon, false);
                lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
                if (strcmp(app_items[i].name, "Flap") && app_item_icon_should_recolor(i, item_icon)) {
                    lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
                } else {
                    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
                }
                lv_coord_t img_w = item_icon->header.w;
                lv_coord_t img_h = item_icon->header.h;
                int zoom_w = img_w > 0 ? (icon_target * 256) / img_w : 256;
                int zoom_h = img_h > 0 ? (icon_target * 256) / img_h : 256;
                int zoom = LV_MIN(zoom_w, zoom_h);
                int max_zoom = GUI_IMAGE_ZOOM_MAX;
                if (zoom > max_zoom) zoom = max_zoom;
                if (zoom < 64) zoom = 64;
                lv_img_set_zoom(icon, zoom);
                lv_obj_refresh_self_size(icon);

                int displayed_h = (img_h * zoom) / 256;
                int top_offset = (icon_area_h - displayed_h) / 2;
                if (top_offset < 0) top_offset = 0;
                lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, top_offset);
            }
        }

        lv_obj_t *label = lv_label_create(card);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL && !app_items[i].is_category_folder) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        const lv_font_t *lbl_font = accessibility_get_font_small();
#if GUI_LARGE_SCREEN
        lbl_font = accessibility_get_font_body();
#endif
        lv_obj_set_style_text_font(label, lbl_font, 0);
        lv_obj_set_style_text_color(label, apps_text_color, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, card_width - (apps_is_compact_layout() ? 12 : 8));
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        if (apps_is_compact_layout()) {
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -2);
        }
        if (apps_is_compact_layout()) apply_compact_app_tile(card, false);
    }

    if (!apps_is_compact_layout() || layout.page_indicator_height > 0) {
        launcher_page_indicator = lv_obj_create(apps_container);
        lv_obj_align(launcher_page_indicator, LV_ALIGN_BOTTOM_MID, 0, -1);
    }

    if (selected_app_index >= 0 && selected_app_index < num_apps && apps_grid_cards[selected_app_index]) {
        if (apps_is_compact_layout()) {
            apply_compact_app_tile(apps_grid_cards[selected_app_index], true);
        } else {
            apply_app_launcher_selection_style(apps_grid_cards[selected_app_index], true);
        }
        scroll_app_launcher_card_to_view(selected_app_index);
    }
}

static void create_apps_list_menu(void) {
    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(MAIN_MENU_LAYOUT_LIST, num_apps, &layout);
    int button_height = layout.list_button_height;
    int icon_target = layout.list_icon_target;

    lv_obj_set_flex_flow(apps_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(apps_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(apps_container, layout.list_pad, 0);
    lv_obj_set_style_pad_row(apps_container, layout.list_row_gap, 0);
    lv_obj_add_flag(apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(apps_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_AUTO);

    apps_cleanup_layout();
    apps_list_buttons = calloc(num_apps, sizeof(lv_obj_t *));
    if (!apps_list_buttons) {
        ESP_LOGE(TAG, "failed to alloc apps list buttons");
        return;
    }

    /* List shows every app's icon simultaneously; see create_apps_launcher_menu. */
    asset_pack_reset_icon_pins();

    for (int i = 0; i < num_apps; ++i) {
        lv_obj_t *btn = lv_btn_create(apps_container);
        gui_apply_pressed_style(btn);
        apps_list_buttons[i] = btn;
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, button_height);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        int btn_border_w = settings_get_menu_item_borders(&G_Settings) ? 2 : 0;
        apply_app_card_style(btn, apps_surface_color, app_items[i].border_color, btn_border_w, 6);
        lv_obj_set_style_radius(btn, GUI_RADIUS_SM, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, layout.list_button_pad, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, layout.list_column_gap, LV_PART_MAIN);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        const char *item_symbol = app_item_symbol_icon(i);
        const lv_img_dsc_t *item_icon = item_symbol ? NULL : app_item_icon(i);
        if (item_symbol) {
            const lv_font_t *symbol_font = app_symbol_font_for_target(icon_target);
            lv_obj_t *icon = create_app_symbol_icon(btn, item_symbol, app_items[i].border_color, symbol_font);
            if (icon) {
                lv_obj_set_width(icon, icon_target);
            }
        } else if (item_icon) {
            lv_obj_t *icon = lv_img_create(btn);
            lv_img_set_src(icon, item_icon);
            lv_img_set_antialias(icon, false);
            if (strcmp(app_items[i].name, "Flap") && app_item_icon_should_recolor(i, item_icon)) {
                lv_obj_set_style_img_recolor(icon, app_items[i].border_color, 0);
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_img_recolor_opa(icon, LV_OPA_TRANSP, 0);
            }
            lv_coord_t img_w = item_icon->header.w;
            lv_coord_t img_h = item_icon->header.h;
            int zoom_w = img_w > 0 ? (icon_target * 256) / img_w : 256;
            int zoom_h = img_h > 0 ? (icon_target * 256) / img_h : 256;
            int zoom = LV_MIN(zoom_w, zoom_h);
            int max_zoom = GUI_IMAGE_ZOOM_MAX;
            if (zoom > max_zoom) zoom = max_zoom;
            if (zoom < 64) zoom = 64;
            lv_img_set_zoom(icon, zoom);
            lv_img_set_size_mode(icon, LV_IMG_SIZE_MODE_REAL);
            lv_obj_refresh_self_size(icon);
        }

        lv_obj_t *label = lv_label_create(btn);
        const char *label_text = app_items[i].name;
        if (app_items[i].view == NULL && !app_items[i].is_category_folder) {
            label_text = "< Back";
        }
        lv_label_set_text(label, label_text);
        lv_obj_set_style_text_color(label, apps_text_color, 0);
        const lv_font_t *lbl_font = accessibility_get_font_body();
        lv_obj_set_style_text_font(label, lbl_font, 0);

        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(label, 1);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    }
}

static void move_app_nav_buttons_foreground(void) {
    if (left_nav_btn) lv_obj_move_foreground(left_nav_btn);
    if (right_nav_btn) lv_obj_move_foreground(right_nav_btn);
}

static void render_app_items(void) {
    if (!apps_container || !lv_obj_is_valid(apps_container)) return;

    apps_cancel_carousel_anims();
    lvgl_obj_del_safe(&hero_pip_container);
    lv_obj_clean(apps_container);
    apps_cleanup_layout();
    asset_pack_reset_icon_pins();
    current_app_obj = NULL;
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_is_animating = false;

    if (selected_app_index < 0) selected_app_index = 0;
    if (selected_app_index >= num_apps) selected_app_index = num_apps > 0 ? num_apps - 1 : 0;

    if (apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) {
        create_apps_launcher_menu();
        select_app_item(selected_app_index, false);
    } else if (apps_layout == MAIN_MENU_LAYOUT_LIST) {
        create_apps_list_menu();
        select_app_item(selected_app_index, false);
    } else {
        update_app_item(false);
    }

    if (apps_layout == MAIN_MENU_LAYOUT_HERO) {
        create_app_hero_position_indicator();
    }

    move_app_nav_buttons_foreground();
}

static void apps_plugin_reload_done(void *arg) {
    (void)arg;
    apps_plugin_reload_in_progress = false;

    if (display_manager_get_current_view() != &apps_menu_view) return;
    if (!apps_container || !lv_obj_is_valid(apps_container)) return;

    bool selected_back = false;
    if (app_items && selected_app_index >= 0 && selected_app_index < num_apps) {
        selected_back = (app_items[selected_app_index].view == NULL &&
                         !app_items[selected_app_index].is_category_folder);
    }

    bool plugins_enabled = apps_native_plugins_enabled();
    rebuild_app_items(plugins_enabled);
    init_app_colors();
    apps_allow_plugin_icon_load = plugins_enabled;
    if (selected_back) selected_app_index = num_apps > 0 ? num_apps - 1 : 0;
    render_app_items();
    gui_screen_apply_background(apps_menu_view.root);

    int plugin_count = plugin_manager_count();
    if (plugins_enabled && plugin_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d SD app%s ready", plugin_count, plugin_count == 1 ? "" : "s");
        toast_show_duration(msg, TOAST_SUCCESS, 1000);
    } else if (plugin_manager_last_error()[0] != '\0') {
        char msg[64];
        snprintf(msg, sizeof(msg), "App scan failed: %.45s", plugin_manager_last_error());
        toast_show_duration(msg, TOAST_WARN, 2200);
    }
}

/**
 * @brief Creates the apps menu screen view
 */
 void apps_menu_create(void) {
    bool plugins_enabled = apps_native_plugins_enabled();
    if (plugins_enabled) plugin_manager_init();
    int boot_count = plugin_manager_count();
    apps_allow_plugin_icon_load = plugins_enabled && (boot_count > 0);
    /* Only force a fresh top-level entry when actually arriving from the
     * Main Menu; returning here (e.g. after launching a native app or
     * plugin) should preserve which category/item was selected instead of
     * always snapping back to the first icon. */
    bool fresh_entry = (gui_router_previous_view() == &main_menu_view);
    if (fresh_entry) {
        in_submenu = false;
        current_category[0] = '\0';
        selected_app_index = 0;
    }
    rebuild_app_items(plugins_enabled && boot_count > 0);
    if (plugins_enabled && boot_count > 0) {
        char msg[48];
        snprintf(msg, sizeof(msg), "%d SD app%s ready", boot_count, boot_count == 1 ? "" : "s");
        toast_show_duration(msg, TOAST_SUCCESS, 1000);
    }
    refresh_apps_surface_colors();
    display_manager_fill_screen(apps_bg_color);

    if (!s_native_apps_psram_warning_shown &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) == 0) {
        s_native_apps_psram_warning_shown = true;
        toast_show_duration("Native SD apps require PSRAM", TOAST_WARN, 1500);
    }

    const char *title = (LV_VER_RES > 320 ? "Apps Menu" : "Apps");

    apps_root = gui_screen_create_root(NULL, title, apps_bg_color, LV_OPA_TRANSP);
    apps_menu_view.root = apps_root;

    apps_container = lv_obj_create(apps_root);
    lv_obj_set_size(apps_container, LV_HOR_RES, LV_VER_RES);
    lv_obj_clear_flag(apps_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(apps_container, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(apps_container, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(apps_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(apps_container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(apps_container, 0, LV_PART_MAIN);

    init_app_colors();

    main_menu_layout_kind_t configured_layout =
        main_menu_layout_from_setting(settings_get_menu_layout(&G_Settings));
#if GUI_EPAPER
    /* Match the board's PREV/NEXT/OK controls with a stable, one-axis list. */
    configured_layout = MAIN_MENU_LAYOUT_LIST;
#endif
    apps_layout = main_menu_layout_resolve_for_size(configured_layout, LV_HOR_RES, LV_VER_RES);

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);
    int status_bar_height = layout.status_bar_height;
    if (apps_container) {
        lv_obj_align(apps_container, layout.container_align, layout.container_x, layout.container_y);
        if (apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) {
            lv_obj_set_size(apps_container, layout.container_width, layout.container_height);
        }
#if GUI_LARGE_SCREEN
        else if (apps_layout == MAIN_MENU_LAYOUT_LIST) {
            lv_obj_set_size(apps_container, layout.container_width, layout.container_height);
            lv_obj_align(apps_container, LV_ALIGN_TOP_MID, 0, status_bar_height);
        }
#endif
    }

    bool should_show_nav_buttons = settings_get_nav_buttons_enabled(&G_Settings);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
    /* Side arrows land in the circular mask's corners.  Hero mode already
     * exposes the current position through the bottom pips. */
    should_show_nav_buttons = false;
#endif
#if GUI_LARGE_TOUCH_UI && defined(CONFIG_USE_TOUCHSCREEN)
    should_show_nav_buttons = false;
#endif

    if (should_show_nav_buttons) {
#ifdef CONFIG_LVGL_TOUCH
        should_show_nav_buttons = true;
#else
        int screen_width = lv_disp_get_hor_res(lv_disp_get_default());
        should_show_nav_buttons = (screen_width > 200);
#endif
    }

    if (should_show_nav_buttons && apps_layout == MAIN_MENU_LAYOUT_CAROUSEL &&
        layout.carousel_show_previews) {
        should_show_nav_buttons = false;
    }
    if (should_show_nav_buttons && (apps_layout == MAIN_MENU_LAYOUT_LAUNCHER ||
                                    apps_layout == MAIN_MENU_LAYOUT_COMPACT ||
                                    apps_layout == MAIN_MENU_LAYOUT_LIST)) {
        should_show_nav_buttons = false;
    }

    if (should_show_nav_buttons) {
        left_nav_btn = lv_btn_create(lv_scr_act());
        gui_apply_pressed_style(left_nav_btn);

        int btn_size = layout.nav_button_size;
        int btn_margin = layout.nav_button_margin;

        lv_obj_set_size(left_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(left_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(left_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(left_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(left_nav_btn, LV_ALIGN_LEFT_MID, btn_margin, 0);

        lv_obj_t *left_label = lv_label_create(left_nav_btn);
        lv_label_set_text(left_label, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_font(left_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(left_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(left_label, apps_text_color, 0);
        lv_obj_align(left_label, LV_ALIGN_CENTER, 0, 2);

        right_nav_btn = lv_btn_create(lv_scr_act());
        gui_apply_pressed_style(right_nav_btn);
        lv_obj_set_size(right_nav_btn, btn_size, btn_size);
        lv_obj_set_style_bg_opa(right_nav_btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(right_nav_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(right_nav_btn, 0, LV_PART_MAIN);

        lv_obj_align(right_nav_btn, LV_ALIGN_RIGHT_MID, -btn_margin, 0);

        lv_obj_t *right_label = lv_label_create(right_nav_btn);
        lv_label_set_text(right_label, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_font(right_label, accessibility_get_font_display(), 0);
        if (btn_size < 40) {
            lv_obj_set_style_text_font(right_label, accessibility_get_font_title(), 0);
        }
        lv_obj_set_style_text_color(right_label, apps_text_color, 0);
        lv_obj_align(right_label, LV_ALIGN_CENTER, 0, 2);

        ESP_LOGI(TAG, "Navigation buttons created for apps menu");

        lv_obj_move_foreground(left_nav_btn);
        lv_obj_move_foreground(right_nav_btn);
    }

    /* render_app_items() clamps selected_app_index against num_apps itself,
     * so a preserved index that's now out of range (e.g. a native app was
     * added/removed since we were last here) is handled there. */
    render_app_items();
    gui_screen_apply_background(apps_menu_view.root);

    if (left_nav_btn && apps_layout != MAIN_MENU_LAYOUT_HERO) {
        lv_coord_t old_y = lv_obj_get_y(left_nav_btn);
        lv_obj_set_y(left_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(left_nav_btn);
    }
    if (right_nav_btn && apps_layout != MAIN_MENU_LAYOUT_HERO) {
        lv_coord_t old_y = lv_obj_get_y(right_nav_btn);
        lv_obj_set_y(right_nav_btn, old_y + status_bar_height / 2);
        lv_obj_move_foreground(right_nav_btn);
    }

    /* HERO layout: the arrows are normally anchored to the content-area
     * middle, which on the minimal hero stage lands low on the screen, far
     * below the big icon and practically on top of the page dots. Re-anchor
     * them to the hero icon's vertical center so they read as next/prev
     * controls beside the icon (and title) instead. */
    if (apps_layout == MAIN_MENU_LAYOUT_HERO) {
        lv_area_t icon_area;
        if (apps_carousel_cache.icon && lv_obj_is_valid(apps_carousel_cache.icon)) {
            lv_obj_get_coords(apps_carousel_cache.icon, &icon_area);
            lv_coord_t icon_center_y = (icon_area.y1 + icon_area.y2) / 2;
            if (left_nav_btn) {
                lv_obj_set_y(left_nav_btn, icon_center_y - lv_obj_get_height(left_nav_btn) / 2);
            }
            if (right_nav_btn) {
                lv_obj_set_y(right_nav_btn, icon_center_y - lv_obj_get_height(right_nav_btn) / 2);
            }
        }
    }

    // No re-scan on open. The boot-time scan (run from the splash deferred
    // task) is the canonical app discovery — re-running it here blanks the
    // UI while plugin_manager_reload wipes s_apps[]. Users can refresh via
    // reboot or the `apps reload` CLI command.
}

/**
 * @brief Destroys the apps menu screen view
 */
void apps_menu_destroy(void) {
    apps_cancel_carousel_anims();
    if (apps_root) {
        lvgl_obj_del_safe(&apps_root);
        apps_container = NULL;
        apps_menu_view.root = NULL;
        current_app_obj = NULL;
        back_button = NULL;
    }
    apps_cleanup_layout();
    apps_carousel_cache = (apps_carousel_cache_t){0};
    apps_is_animating = false;
    lvgl_obj_del_safe(&left_nav_btn);
    lvgl_obj_del_safe(&right_nav_btn);

    if (!apps_plugin_reload_in_progress) {
        free(apps_plugin_reload_stack);
        apps_plugin_reload_stack = NULL;
        free(apps_plugin_reload_tcb);
        apps_plugin_reload_tcb = NULL;
    }

    /* selected_app_index/in_submenu/current_category are deliberately NOT
     * reset here: they need to survive the destroy() -> create() cycle so
     * apps_menu_create()'s fresh_entry check can tell "returning from a
     * launched app" (preserve position) apart from "arriving from the Main
     * Menu" (reset to the top). */
    touch_started = false;
    touch_dragged = false;
    touch_drag_axis = 0;
    touch_start_x = 0;
    touch_start_y = 0;
    touch_last_x = 0;
    touch_last_y = 0;
    free(app_items);
    app_items = NULL;
    s_app_items_capacity = 0;
    num_apps = 0;
}

/**
 * @brief Selects an app item and updates the display
 */
static void select_app_item(int index, bool slide_left) {
    if (!app_items || num_apps <= 0) return;
    if (apps_is_carousel_like_layout() && apps_is_animating) return;
    if (index < 0) index = num_apps - 1;
    if (index >= num_apps) index = 0;

    if ((apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) && apps_grid_cards) {
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_grid_cards[selected_app_index]) {
            lv_obj_t *old = apps_grid_cards[selected_app_index];
            if (apps_is_compact_layout()) {
                apply_compact_app_tile(old, false);
            } else {
                apply_app_launcher_selection_style(old, false);
            }
        }
        selected_app_index = index;
        if (apps_grid_cards[selected_app_index]) {
            lv_obj_t *card = apps_grid_cards[selected_app_index];
            if (apps_is_compact_layout()) {
                apply_compact_app_tile(card, true);
            } else {
                apply_app_launcher_selection_style(card, true);
            }
            scroll_app_launcher_card_to_view(selected_app_index);
        }
        return;
    }

    if (apps_layout == MAIN_MENU_LAYOUT_LIST && apps_list_buttons) {
        if (selected_app_index >= 0 && selected_app_index < num_apps && apps_list_buttons[selected_app_index]) {
            lv_obj_t *old = apps_list_buttons[selected_app_index];
            apply_app_card_style(old, apps_surface_color, app_items[selected_app_index].border_color,
                                 settings_get_menu_item_borders(&G_Settings) ? 2 : 0, 6);
        }
        selected_app_index = index;
        if (apps_list_buttons[selected_app_index]) {
            lv_obj_t *btn = apps_list_buttons[selected_app_index];
            uint8_t theme = settings_get_menu_theme(&G_Settings);
            lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
            apply_app_card_selection_style(btn, accent);
            lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
        }
        return;
    }

    selected_app_index = index;
    update_app_item(slide_left);
    if (apps_layout == MAIN_MENU_LAYOUT_HERO) update_app_hero_position();
}

static void apps_menu_go_back(void) {
    if (in_submenu) {
        in_submenu = false;
        current_category[0] = '\0';
        selected_app_index = 0;
        rebuild_app_items(apps_native_plugins_enabled());
        init_app_colors();
        render_app_items();
        gui_screen_apply_background(apps_menu_view.root);
        return;
    }
    display_manager_go_back();
}

static void navigate_apps_vertical(int direction) {
    if (direction == 0) return;

    if (apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) {
        if (apps_grid_cols <= 0 || num_apps <= 0) return;

        int rows = (num_apps + apps_grid_cols - 1) / apps_grid_cols;
        if (rows <= 0) return;

        int row = selected_app_index / apps_grid_cols;
        int col = selected_app_index % apps_grid_cols;

        for (int tries = 0; tries < rows; ++tries) {
            row = (row + (direction > 0 ? 1 : -1) + rows) % rows;
            int base = row * apps_grid_cols;
            int candidate = base + col;
            if (candidate >= num_apps) {
                candidate = num_apps - 1;
                if (candidate < base) continue;
            }
            select_app_item(candidate, false);
            return;
        }
        return;
    }

    select_app_item(selected_app_index + (direction > 0 ? 1 : -1), false);
}

static int apps_grid_horizontal_target(int direction) {
    if (apps_layout == MAIN_MENU_LAYOUT_COMPACT) {
        return selected_app_index + direction;
    }
    if ((apps_layout != MAIN_MENU_LAYOUT_LAUNCHER && apps_layout != MAIN_MENU_LAYOUT_COMPACT) || num_apps <= 0) {
        return selected_app_index + direction;
    }

    main_menu_layout_metrics_t layout;
    main_menu_layout_get_metrics(apps_layout, num_apps, &layout);
    if (layout.columns <= 0 || layout.page_capacity <= 0 || layout.page_count <= 0) {
        return selected_app_index + direction;
    }

    int page = selected_app_index / layout.page_capacity;
    int slot = selected_app_index % layout.page_capacity;
    int col = slot % layout.columns;
    if (direction > 0 && col < layout.columns - 1 && selected_app_index + 1 < num_apps) {
        return selected_app_index + 1;
    }
    if (direction < 0 && col > 0) return selected_app_index - 1;

    page += direction > 0 ? 1 : -1;
    if (page < 0) page = layout.page_count - 1;
    if (page >= layout.page_count) page = 0;
    int target = page * layout.page_capacity + slot;
    return target < num_apps ? target : num_apps - 1;
}

/**
 * @brief Handles the selection of app items
 */
static void handle_app_item_selection(int item_index) {
    if (item_index < 0 || item_index >= num_apps) return;

    if (app_items[item_index].is_category_folder) {
        strncpy(current_category, app_items[item_index].category, sizeof(current_category) - 1);
        current_category[sizeof(current_category) - 1] = '\0';
        in_submenu = true;
        rebuild_app_items(apps_native_plugins_enabled());
        init_app_colors();
        selected_app_index = (num_apps > 1) ? 1 : 0;
        render_app_items();
        gui_screen_apply_background(apps_menu_view.root);
        return;
    }

    if (app_items[item_index].view == NULL) {
        apps_menu_go_back();
        return;
    }

    menu_catalog_launch(app_items[item_index].catalog_id, &apps_menu_view);
}

/**
 * @brief Handles hardware button presses for app navigation
 */
static void handle_apps_button_press(int button) {
    if (apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) {
        if (button == 2) {
            navigate_apps_vertical(-1);
        } else if (button == 4) {
            navigate_apps_vertical(1);
        } else if (button == 0) {
            select_app_item(apps_grid_horizontal_target(-1), false);
        } else if (button == 3) {
            select_app_item(apps_grid_horizontal_target(1), true);
        } else if (button == 1) {
            handle_app_item_selection(selected_app_index);
        }
        return;
    }

    if (apps_layout == MAIN_MENU_LAYOUT_LIST) {
        if (button == 2) { // Up
            ESP_LOGD(TAG, "Up button pressed\n");
            select_app_item(selected_app_index - 1, false);
        } else if (button == 4) { // Down
            ESP_LOGD(TAG, "Down button pressed\n");
            select_app_item(selected_app_index + 1, false);
        } else if (button == 1) { // Select
            ESP_LOGD(TAG, "Select button pressed\n");
            handle_app_item_selection(selected_app_index);
        } else if (button == 0) { // Back/Left
            ESP_LOGD(TAG, "Back button pressed\n");
            apps_menu_go_back();
        }
        return;
    }

    if (button == 0) { // Left
        ESP_LOGD(TAG, "Left button pressed\n");
        select_app_item(selected_app_index - 1, false);
    } else if (button == 3) { // Right
        ESP_LOGD(TAG, "Right button pressed\n");
        select_app_item(selected_app_index + 1, true);
    } else if (button == 1) { // Select
        ESP_LOGD(TAG, "Select button pressed\n");
        handle_app_item_selection(selected_app_index);
    } else if (button == 2) { // Back
        ESP_LOGD(TAG, "Back button pressed\n");
        apps_menu_go_back();
    }
}

/**
 * @brief handles keyboard button presses
 */
static void handle_keyboard_interactions(int keyValue){

    // Vim keybinds and Cardputer controls
    if (keyValue == LV_KEY_LEFT || keyValue == 44 || keyValue == ',' || keyValue == 'h') { // Left
        ESP_LOGI(TAG, "Left button or 'h' pressed");
        select_app_item(apps_grid_horizontal_target(-1), false);
    } else if (keyValue == LV_KEY_RIGHT || keyValue == 47 || keyValue == '/' || keyValue == 'l') { // Right
        ESP_LOGI(TAG, "Right button or 'l' pressed");
        select_app_item(apps_grid_horizontal_target(1), true);
    } else if (keyValue == LV_KEY_UP || keyValue == 'k' || keyValue == ';') { // Up
        ESP_LOGI(TAG, "Up arrow or 'k' pressed");
        navigate_apps_vertical(-1);
    } else if (keyValue == LV_KEY_DOWN || keyValue == 'j' || keyValue == '.') { // Down
        ESP_LOGI(TAG, "Down arrow or 'j' pressed");
        navigate_apps_vertical(1);
    } else if (keyValue == LV_KEY_ENTER || keyValue == 13) { // Select
        ESP_LOGI(TAG, "Enter pressed (select)");
        handle_app_item_selection(selected_app_index);
    } else if (keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`') { // Back
        ESP_LOGI(TAG, "Esc or '`' pressed (back)");
        apps_menu_go_back();
    }
}

static bool apps_keyboard_activation_key(int keyValue) {
    return keyValue == LV_KEY_ENTER || keyValue == 13 ||
           keyValue == LV_KEY_ESC || keyValue == 29 || keyValue == '`';
}

/**
 * @brief Combined handler for app menu events
 */
void apps_menu_event_handler(InputEvent *event) {
    if (gui_menu_set_touch_input(event->type == INPUT_TYPE_TOUCH) && selected_app_index >= 0 && selected_app_index < num_apps) {
        if (apps_grid_cards) {
            if (apps_is_compact_layout()) apply_compact_app_tile(apps_grid_cards[selected_app_index], true);
            else apply_app_launcher_selection_style(apps_grid_cards[selected_app_index], true);
        } else if (apps_list_buttons) {
            lv_obj_t *row = apps_list_buttons[selected_app_index];
            lv_color_t accent = lv_color_hex(theme_palette_get_accent(settings_get_menu_theme(&G_Settings)));
            apply_app_card_style(row, apps_surface_color, accent, settings_get_menu_item_borders(&G_Settings) ? 2 : 0, 0);
            apply_app_card_selection_style(row, accent);
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
            } else {
                int dx = data->point.x - touch_last_x;
                int dy = data->point.y - touch_last_y;
                touch_last_x = data->point.x;
                touch_last_y = data->point.y;

                if (!touch_dragged) {
                    touch_drag_axis = resolve_drag_axis(data->point.x - touch_start_x, data->point.y - touch_start_y);
                    touch_dragged = touch_drag_axis != 0;
                }

                if (touch_dragged && touch_drag_axis == 1) {
                    dy = clamp_drag_delta(dy);
                    if (dy) {
                        if (apps_layout == MAIN_MENU_LAYOUT_LIST && apps_container) {
                            display_manager_queue_scroll(apps_container, dy);
                        }
                    }
                }
            }
        } else if (data->state == LV_INDEV_STATE_REL && touch_started) {
            int dx = data->point.x - touch_start_x;
            int dy = data->point.y - touch_start_y;
            touch_started = false;

            if ((apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) &&
                abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                main_menu_layout_metrics_t layout;
                if (apps_is_compact_layout()) {
                    select_app_item(selected_app_index + (dx < 0 ? 1 : -1), dx < 0);
                    return;
                }
                main_menu_layout_get_metrics(apps_layout, num_apps, &layout);
                int page = selected_app_index / layout.page_capacity;
                int slot = selected_app_index % layout.page_capacity;
                page += dx < 0 ? 1 : -1;
                if (page < 0) page = layout.page_count - 1;
                if (page >= layout.page_count) page = 0;
                int target = page * layout.page_capacity + slot;
                if (target >= num_apps) target = num_apps - 1;
                touch_dragged = false;
                touch_drag_axis = 0;
                select_app_item(target, dx < 0);
                return;
            }

            if (touch_dragged && !apps_is_carousel_like_layout()) {
                touch_dragged = false;
                touch_drag_axis = 0;
                return;
            }
            touch_dragged = false;
            touch_drag_axis = 0;

            if (apps_is_carousel_like_layout()) {
                if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
                    if (dx < 0) {
                        select_app_item(selected_app_index + 1, true);
                    } else {
                        select_app_item(selected_app_index - 1, false);
                    }
                    return;
                }
                if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    if (app_touch_started_and_ended_in(carousel_prev_obj, &data->point)) {
                        select_app_item(selected_app_index - 1, false);
                        return;
                    }
                    if (app_touch_started_and_ended_in(carousel_next_obj, &data->point)) {
                        select_app_item(selected_app_index + 1, true);
                        return;
                    }
                }
            }

            if (left_nav_btn && right_nav_btn) {
                lv_area_t left_area, right_area;
                lv_obj_get_coords(left_nav_btn, &left_area);
                lv_obj_get_coords(right_nav_btn, &right_area);

                bool start_in_left = (touch_start_x >= left_area.x1 && touch_start_x <= left_area.x2 &&
                                      touch_start_y >= left_area.y1 && touch_start_y <= left_area.y2);
                bool end_in_left = (data->point.x >= left_area.x1 && data->point.x <= left_area.x2 &&
                                    data->point.y >= left_area.y1 && data->point.y <= left_area.y2);
                if (start_in_left && end_in_left && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Left navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index - 1, false);
                    return;
                }

                bool start_in_right = (touch_start_x >= right_area.x1 && touch_start_x <= right_area.x2 &&
                                       touch_start_y >= right_area.y1 && touch_start_y <= right_area.y2);
                bool end_in_right = (data->point.x >= right_area.x1 && data->point.x <= right_area.x2 &&
                                     data->point.y >= right_area.y1 && data->point.y <= right_area.y2);
                if (start_in_right && end_in_right && abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                    ESP_LOGI(TAG, "Right navigation button tapped (press+release inside)");
                    select_app_item(selected_app_index + 1, true);
                    return;
                }
            }

            if (apps_layout == MAIN_MENU_LAYOUT_LIST && abs(dy) > SWIPE_THRESHOLD && abs(dy) > abs(dx)) {
                if (apps_container) {
                    dy = clamp_drag_delta(dy);
                    if (dy) display_manager_queue_scroll(apps_container, dy);
                }
                return;
            }

            if (abs(dx) < TAP_THRESHOLD && abs(dy) < TAP_THRESHOLD) {
                // Check which app button was actually tapped
                if (apps_layout == MAIN_MENU_LAYOUT_LIST && apps_list_buttons) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_list_buttons[i]) {
                            lv_area_t btn_area;
                            lv_obj_get_coords(apps_list_buttons[i], &btn_area);
                            if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                                data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else if ((apps_layout == MAIN_MENU_LAYOUT_LAUNCHER || apps_layout == MAIN_MENU_LAYOUT_COMPACT) && apps_grid_cards) {
                    for (int i = 0; i < num_apps; i++) {
                        if (apps_grid_cards[i]) {
                            lv_area_t card_area;
                            lv_obj_get_coords(apps_grid_cards[i], &card_area);
                            if (data->point.x >= card_area.x1 && data->point.x <= card_area.x2 &&
                                data->point.y >= card_area.y1 && data->point.y <= card_area.y2) {
                                select_app_item(i, false);
                                handle_app_item_selection(i);
                                return;
                            }
                        }
                    }
                } else {
                    // Carousel/Hero layout - use current selection
                    handle_app_item_selection(selected_app_index);
                }
                return;
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        ESP_LOGI(TAG, "Joystick event");
        handle_apps_button_press(event->data.joystick_index);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (event->is_repeat && apps_keyboard_activation_key(event->data.key_value)) return;
        ESP_LOGW(TAG, "keyboard event");
        handle_keyboard_interactions(event->data.key_value);
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            handle_app_item_selection(selected_app_index);
        } else {
            if (event->data.encoder.direction > 0) {
                select_app_item(apps_grid_horizontal_target(1), true);
            } else {
                select_app_item(apps_grid_horizontal_target(-1), false);
            }
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
        apps_menu_go_back();
#endif
    }
}

void get_apps_menu_callback(void **callback) {
    *callback = apps_menu_event_handler;
}

View apps_menu_view = {
    .root = NULL,
    .create = apps_menu_create,
    .destroy = apps_menu_destroy,
    .input_callback = apps_menu_event_handler,
    .name = "Apps Menu",
    .get_hardwareinput_callback = get_apps_menu_callback
};
