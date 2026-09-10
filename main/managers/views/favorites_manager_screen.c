#include "managers/views/favorites_manager_screen.h"
#include "managers/settings_manager.h"
#include "managers/display_manager.h"
#include "managers/views/options_screen.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "gui/toast.h"
#include "gui/touch_bar.h"
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include "managers/plugin_manager.h"

// Favorites manager built on the shared options_view list widget so it looks
// and behaves like every other options-style screen (Settings, IR, NFC, ...).
// Row model mirrors ghostscript_browser_view.c: each visible row maps to a
// descriptor, and keyboard / joystick / encoder / touch all funnel into
// activate_row().

typedef enum {
    ROW_NONE = 0,   // informational only, activation is a no-op
    ROW_FAV,        // index = favorite slot
    ROW_ADD,        // open the add-favorite picker
    ROW_CLEAR,      // remove all favorites
    ROW_CATEGORY,   // index = k_fav_categories entry
    ROW_ITEM,       // index = filtered picker entry
    ROW_BACK,
} row_type_t;

typedef struct {
    row_type_t type;
    int index;
} fav_row_t;

#define FAV_FILES_MAX 32
#define FAV_LIST_ROWS_MAX  (FAVORITES_MAX + 4)          // favs + info + Add/Clear/Back
#define FAV_PICKER_ROWS_MAX (FAV_FILES_MAX + 2)         // items + info + Back
#define FAV_ROWS_MAX (FAV_LIST_ROWS_MAX > FAV_PICKER_ROWS_MAX ? FAV_LIST_ROWS_MAX : FAV_PICKER_ROWS_MAX)

static lv_obj_t *s_root = NULL;
static options_view_t *s_ov = NULL;
#ifdef CONFIG_USE_TOUCHSCREEN
// Standard bottom touch bar (scroll up / Back / scroll down), like NFC/SubGHz.
static gui_touch_bar_t s_fav_touch_tb = {0};
static lv_obj_t *s_scroll_up_btn = NULL;
static lv_obj_t *s_scroll_down_btn = NULL;
static lv_obj_t *s_touch_back_btn = NULL;
#endif
// Heap-backed row/file buffers: 2 KB+ of static arrays would permanently
// burn internal RAM on boards without PSRAM (esp32s2 etc.); these are only
// needed while the screen is open.
static fav_row_t *s_rows = NULL;
static int s_row_count = 0;
static bool s_picker_mode = false;
static int s_picker_stage = 0;   // 0 = category chooser, 1 = item chooser
static int s_picker_category = 0;
static char (*s_file_paths)[FAVORITE_NAME_LEN] = NULL;
static int s_file_count = 0;
static bool s_touch_started = false;
#ifdef CONFIG_USE_TOUCHSCREEN
// Shared live drag-scroll state (same smooth scrolling the native views use).
static touch_drag_t s_favmgr_touch_drag;
#endif
// Cursor memory: restored on next open, like the native list views.
static int s_resume_row = -1;

// Pin-able main menu items (same set the lockscreen launcher understands).
static const char * const k_pin_options[] = {
    "WiFi", "BLE", "GPS", "Infrared", "NFC", "NRF24", "SubGHz", "BadUSB",
    "GhostLink", "Ethernet", "Apps", "Settings", "Terminal", "Lock",
#ifdef CONFIG_HAS_AUDIO_PLAYER
    "Audio",
#endif
};
static const int k_pin_options_count = sizeof(k_pin_options) / sizeof(k_pin_options[0]);

static const char * const k_fav_categories[] = {
    "Main Menu", "IR Remote", "NFC Tag", "SubGHz File", "App",
    "Script", "Payload"
};
static const int k_fav_categories_count = sizeof(k_fav_categories) / sizeof(k_fav_categories[0]);

static void build_main_list(int select_row);
static void build_category_picker(void);
static void build_item_picker(int category);
static void activate_row(int selected);
static bool favorites_manager_handle_back(void);
static void favorites_manager_input(InputEvent *event);
void row_click_cb(lv_event_t *e);
#ifdef CONFIG_USE_TOUCHSCREEN
static void fav_update_scroll_buttons(void);
static void fav_scroll_up(void);
static void fav_scroll_down(void);
static void fav_touch_go_back(void);
#endif

// --- helpers -------------------------------------------------------------

// Friendly display name: strips any "type:" prefix (menu:, ir:, nfc:, ...)
// and reduces file paths to their basename ("ir:/mnt/.../TV.ir" -> "TV.ir").
static const char *fav_display_name(const char *fav) {
    if (!fav || !fav[0]) return "";
    const char *name = strchr(fav, ':');
    name = name ? name + 1 : fav;
    const char *slash = strrchr(name, '/');
    if (slash && slash[1]) name = slash + 1;
    return name;
}

static bool point_in_obj(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point->x >= area.x1 && point->x <= area.x2 && point->y >= area.y1 && point->y <= area.y2;
}

static lv_obj_t *find_clickable_at(lv_obj_t *obj, const lv_point_t *point) {
    if (!obj || !point || !lv_obj_is_valid(obj)) return NULL;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || !point_in_obj(obj, point)) return NULL;
    uint32_t child_count = lv_obj_get_child_cnt(obj);
    for (int32_t i = (int32_t)child_count - 1; i >= 0; --i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        lv_obj_t *hit = find_clickable_at(child, point);
        if (hit) return hit;
    }
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) ? obj : NULL;
}

static bool handle_touch(InputEvent *event) {
    if (!event || event->type != INPUT_TYPE_TOUCH) return false;
    lv_indev_data_t *data = &event->data.touch_data;
#ifdef CONFIG_USE_TOUCHSCREEN
    // Live drag: content follows the finger (or applies one release scroll
    // when the touch_drag_scroll setting is off) via the shared helper.
    if (event->is_touch_move) {
        if (!s_favmgr_touch_drag.started) return true;
        lv_obj_t *list = options_view_get_list(s_ov);
        if (!list || !lv_obj_is_valid(list)) return true;
        touch_drag_update(&s_favmgr_touch_drag, data, list);
        return true;
    }
#endif
    if (data->state == LV_INDEV_STATE_PR) {
        s_touch_started = true;
#ifdef CONFIG_USE_TOUCHSCREEN
        // Bar buttons act on press (NFC pattern) so sloppy taps still land.
        bool hit_up = false, hit_back = false, hit_down = false;
        gui_touch_bar_hit_test(&s_fav_touch_tb, data->point.x, data->point.y,
                               &hit_up, &hit_back, &hit_down);
        if (hit_up) { fav_scroll_up(); s_touch_started = false; return true; }
        if (hit_down) { fav_scroll_down(); s_touch_started = false; return true; }
        if (hit_back) { fav_touch_go_back(); s_touch_started = false; return true; }
#endif
        // Begin drag tracking when the press lands inside the list.
        lv_obj_t *list = options_view_get_list(s_ov);
        bool in_list = false;
        if (list && lv_obj_is_valid(list)) {
            lv_area_t la;
            lv_obj_get_coords(list, &la);
            in_list = (data->point.x >= la.x1 && data->point.x <= la.x2 &&
                       data->point.y >= la.y1 && data->point.y <= la.y2);
        }
#ifdef CONFIG_USE_TOUCHSCREEN
        if (in_list) touch_drag_begin(&s_favmgr_touch_drag, data);
        else touch_drag_reset(&s_favmgr_touch_drag);
#else
        (void)in_list;
#endif
        return false;
    }
    if (data->state != LV_INDEV_STATE_REL || !s_touch_started) return false;
    s_touch_started = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    if (touch_drag_release(&s_favmgr_touch_drag, data)) {
        // A drag was in progress (or a release-on-release scroll applied):
        // suppress tap handling.
        display_manager_flush_pending_scroll();
        fav_update_scroll_buttons();
        return true;
    }
#endif
    // Tap: forward to the clicked widget (rows).
    lv_obj_t *target = find_clickable_at(s_root, &data->point);
    if (target) {
        lv_event_send(target, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return false;
}

// --- SD / plugin scans ----------------------------------------------------

static int scan_ir_files(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    const char *dirs[] = { "/mnt/ghostesp/infrared/remotes", "/mnt/ghostesp/infrared/universals" };
    for (int d = 0; d < 2 && s_file_count < FAV_FILES_MAX; d++) {
        DIR *dir = opendir(dirs[d]);
        if (!dir) continue;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && s_file_count < FAV_FILES_MAX) {
            if (ent->d_name[0] == '.') continue;
            size_t len = strlen(ent->d_name);
            if (len < 3 || strcasecmp(ent->d_name + len - 3, ".ir") != 0) continue;
            int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "ir:%s/%s", dirs[d], ent->d_name);
            if (n < 0 || n >= FAVORITE_NAME_LEN) continue; // path too long for a favorite slot
            s_file_count++;
        }
        closedir(dir);
    }
    return s_file_count;
}

static int scan_nfc_files(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    const char *dirp = "/mnt/ghostesp/nfc";
    DIR *dir = opendir(dirp);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < FAV_FILES_MAX) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcasecmp(ent->d_name + len - 4, ".nfc") != 0) continue;
        int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "nfc:%s/%s", dirp, ent->d_name);
        if (n < 0 || n >= FAVORITE_NAME_LEN) continue;
        s_file_count++;
    }
    closedir(dir);
    return s_file_count;
}

static int scan_subghz_files(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    const char *dirp = "/mnt/ghostesp/subghz";
    DIR *dir = opendir(dirp);
    if (!dir) {
        dirp = "/mnt/ghostesp/subghz/captures";
        dir = opendir(dirp);
        if (!dir) return 0;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < FAV_FILES_MAX) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcasecmp(ent->d_name + len - 4, ".sub") != 0) continue;
        int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "subghz:%s/%s", dirp, ent->d_name);
        if (n < 0 || n >= FAVORITE_NAME_LEN) continue;
        s_file_count++;
    }
    closedir(dir);
    return s_file_count;
}

static int scan_app_ids(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    int cnt = plugin_manager_count();
    for (int i = 0; i < cnt && s_file_count < FAV_FILES_MAX; i++) {
        const plugin_app_manifest_t *app = plugin_manager_get(i);
        if (!app || !app->id[0]) continue;
        int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "app:%s", app->id);
        if (n < 0 || n >= FAVORITE_NAME_LEN) continue;
        s_file_count++;
    }
    return s_file_count;
}

static int scan_script_files(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    const char *dirp = "/mnt/ghostesp/scripts";
    DIR *dir = opendir(dirp);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < FAV_FILES_MAX) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 4 || strcasecmp(ent->d_name + len - 3, ".gs") != 0) continue;
        int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "gs:%s/%s", dirp, ent->d_name);
        if (n < 0 || n >= FAVORITE_NAME_LEN) continue;
        s_file_count++;
    }
    closedir(dir);
    return s_file_count;
}

static int scan_badusb_files(void) {
    s_file_count = 0;
    if (!s_file_paths) return 0;
    const char *dirp = "/mnt/ghostesp/badusb";
    DIR *dir = opendir(dirp);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_file_count < FAV_FILES_MAX) {
        if (ent->d_name[0] == '.') continue;
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".txt") != 0) continue;
        // Store the bare script name: the BadUSB view addresses payloads by
        // name, and the launcher derives it from the path anyway.
        int n = snprintf(s_file_paths[s_file_count], FAVORITE_NAME_LEN, "badusb:%s", ent->d_name);
        if (n < 0 || n >= FAVORITE_NAME_LEN) continue;
        s_file_count++;
    }
    closedir(dir);
    return s_file_count;
}

// --- list building ---------------------------------------------------------

static void add_row(row_type_t type, int index, const char *label) {
    if (!s_rows || s_row_count >= FAV_ROWS_MAX) return;
    if (!options_view_add_item(s_ov, label, row_click_cb, NULL)) return;
    s_rows[s_row_count].type = type;
    s_rows[s_row_count].index = index;
    s_row_count++;
}

static void build_main_list(int select_row) {
    s_picker_mode = false;
    s_picker_stage = 0;
    s_picker_category = 0;
    s_file_count = 0;
    options_view_clear(s_ov);
    s_row_count = 0;

    int count = settings_get_favorites_count(&G_Settings);
    char title[32];
    snprintf(title, sizeof(title), "Favorites %d/%d", count, FAVORITES_MAX);
    options_view_set_title(s_ov, title);

    if (count == 0) {
        add_row(ROW_NONE, -1, "No favorites yet");
    }
    for (int i = 0; i < count; i++) {
        add_row(ROW_FAV, i, fav_display_name(settings_get_favorite(&G_Settings, i)));
    }
    add_row(ROW_ADD, -1, LV_SYMBOL_PLUS " Add Favorite");
    if (count > 0) add_row(ROW_CLEAR, -1, LV_SYMBOL_TRASH " Clear All");
    add_row(ROW_BACK, -1, LV_SYMBOL_LEFT " Back");

    if (select_row < 0 || select_row >= s_row_count) select_row = 0;
    // Never rest the cursor on an informational row.
    if (s_rows && s_rows[select_row].type == ROW_NONE) select_row = s_row_count > 1 ? 1 : 0;
    options_view_set_selected(s_ov, select_row);
#ifdef CONFIG_USE_TOUCHSCREEN
    fav_update_scroll_buttons();
#endif
}

static void build_category_picker(void) {
    s_picker_mode = true;
    s_picker_stage = 0;
    options_view_clear(s_ov);
    s_row_count = 0;

    options_view_set_title(s_ov, "Add Favorite");
    for (int i = 0; i < k_fav_categories_count; i++) {
        add_row(ROW_CATEGORY, i, k_fav_categories[i]);
    }
    add_row(ROW_BACK, -1, LV_SYMBOL_LEFT " Back");
    options_view_set_selected(s_ov, 0);
#ifdef CONFIG_USE_TOUCHSCREEN
    fav_update_scroll_buttons();
#endif
}

static void build_item_picker(int category) {
    s_picker_mode = true;
    s_picker_stage = 1;
    s_picker_category = category;
    options_view_clear(s_ov);
    s_row_count = 0;

    char title[40];
    snprintf(title, sizeof(title), "Add %s", k_fav_categories[category]);
    options_view_set_title(s_ov, title);

    int avail = 0;
    if (category == 0) {
        for (int i = 0; i < k_pin_options_count; i++) {
            if (settings_is_favorite(&G_Settings, k_pin_options[i])) continue;
            add_row(ROW_ITEM, i, k_pin_options[i]);
            avail++;
        }
    } else if (category == 5) {
        scan_script_files();
        for (int i = 0; i < s_file_count; i++) {
            if (settings_is_favorite(&G_Settings, s_file_paths[i])) continue;
            add_row(ROW_ITEM, i, fav_display_name(s_file_paths[i]));
            avail++;
        }
    } else if (category == 6) {
        scan_badusb_files();
        for (int i = 0; i < s_file_count; i++) {
            if (settings_is_favorite(&G_Settings, s_file_paths[i])) continue;
            add_row(ROW_ITEM, i, fav_display_name(s_file_paths[i]));
            avail++;
        }
    } else {
        if (category == 1) scan_ir_files();
        else if (category == 2) scan_nfc_files();
        else if (category == 3) scan_subghz_files();
        else if (category == 4) scan_app_ids();
        for (int i = 0; i < s_file_count; i++) {
            if (settings_is_favorite(&G_Settings, s_file_paths[i])) continue;
            add_row(ROW_ITEM, i, fav_display_name(s_file_paths[i]));
            avail++;
        }
    }
    if (avail == 0) {
        add_row(ROW_NONE, -1,
                category == 0 ? "All items favorited" :
                category == 5 ? "No scripts found" :
                category == 6 ? "No payloads found" : "No files found");
    }
    add_row(ROW_BACK, -1, LV_SYMBOL_LEFT " Back");
    options_view_set_selected(s_ov, 0);
#ifdef CONFIG_USE_TOUCHSCREEN
    fav_update_scroll_buttons();
#endif
}

// --- actions ---------------------------------------------------------------

static void remove_favorite(int slot) {
    const char *name = settings_get_favorite(&G_Settings, slot);
    if (!name) return;
    char copy[FAVORITE_NAME_LEN];
    strncpy(copy, name, FAVORITE_NAME_LEN - 1);
    copy[FAVORITE_NAME_LEN - 1] = '\0';
    settings_remove_favorite(&G_Settings, copy);
    settings_persist_setting(SETTING_FAVORITES);
    char msg[64];
    snprintf(msg, sizeof(msg), "Removed %s", fav_display_name(copy));
    toast_show(msg, TOAST_INFO);
    build_main_list(slot); // keep the cursor near where the user was
}

static void clear_all_favorites(void) {
    G_Settings.favorites_count = 0;
    memset(G_Settings.favorites, 0, sizeof(G_Settings.favorites));
    settings_persist_setting(SETTING_FAVORITES);
    toast_show("Favorites cleared", TOAST_INFO);
    build_main_list(0);
}

static void add_favorite(const char *fav) {
    if (settings_add_favorite(&G_Settings, fav)) {
        settings_persist_setting(SETTING_FAVORITES);
        char msg[64];
        snprintf(msg, sizeof(msg), "Added %s", fav_display_name(fav));
        toast_show(msg, TOAST_SUCCESS);
    } else {
        char msg[48];
        snprintf(msg, sizeof(msg), "Favorites full (%d max)", FAVORITES_MAX);
        toast_show(msg, TOAST_WARN);
    }
    build_main_list(settings_get_favorites_count(&G_Settings));
}

static void pick_category(int category) {
    if (category < 0 || category >= k_fav_categories_count) return;
    build_item_picker(category);
}

static void pick_item(int row_index) {
    // Map the visible picker row back to its source entry.
    if (row_index < 0 || row_index >= s_row_count) return;
    int item_index = s_rows[row_index].index;
    const char *chosen = NULL;
    if (s_picker_category == 0) {
        if (item_index >= 0 && item_index < k_pin_options_count) chosen = k_pin_options[item_index];
    } else {
        if (s_file_paths && item_index >= 0 && item_index < s_file_count) chosen = s_file_paths[item_index];
    }
    if (chosen) add_favorite(chosen);
}

static void activate_row(int selected) {
    if (!s_ov || !s_rows || selected < 0 || selected >= s_row_count) return;
    fav_row_t row = s_rows[selected];
    switch (row.type) {
        case ROW_NONE:
            break;
        case ROW_FAV:
            remove_favorite(row.index);
            break;
        case ROW_ADD:
            build_category_picker();
            break;
        case ROW_CLEAR:
            clear_all_favorites();
            break;
        case ROW_CATEGORY:
            pick_category(row.index);
            break;
        case ROW_ITEM:
            pick_item(selected);
            break;
        case ROW_BACK:
            favorites_manager_handle_back();
            break;
    }
}

// --- input ------------------------------------------------------------------

static void get_cb(void **callback) { *callback = favorites_manager_input; }

void row_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    int selected = options_view_get_selected(s_ov);
    for (int i = 0; i < s_row_count; ++i) {
        if (target == lv_obj_get_child(options_view_get_list(s_ov), i)) { selected = i; break; }
    }
    activate_row(selected);
}

static bool is_back_event(InputEvent *event) {
    if (event->type == INPUT_TYPE_EXIT_BUTTON) return true;
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t kv = event->data.key_value;
        if (kv == LV_KEY_ESC || kv == 27 || kv == 29 || kv == '`' || kv == '\b') return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed &&
        event->data.joystick_index == 0) return true;
    return false;
}

static bool favorites_manager_handle_back(void) {
    if (!s_picker_mode) {
        display_manager_go_back();
        return true;
    }
    if (s_picker_stage == 0) {
        build_main_list(-1);
    } else {
        build_category_picker();
    }
    return true;
}

static void favorites_manager_input(InputEvent *event) {
    if (!s_ov) return;
    if (is_back_event(event)) {
        favorites_manager_handle_back();
        return;
    }
    if (handle_touch(event)) return;
    bool moved = false;
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        if (event->data.joystick_index == 2) { options_view_move_selection(s_ov, -1); moved = true; }
        else if (event->data.joystick_index == 4) { options_view_move_selection(s_ov, 1); moved = true; }
        else if (event->data.joystick_index == 1) activate_row(options_view_get_selected(s_ov));
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) activate_row(options_view_get_selected(s_ov));
        else { options_view_move_selection(s_ov, event->data.encoder.direction > 0 ? 1 : -1); moved = true; }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == LV_KEY_UP || kv == 'k' || kv == ';') { options_view_move_selection(s_ov, -1); moved = true; }
        else if (kv == LV_KEY_DOWN || kv == 'j' || kv == '.') { options_view_move_selection(s_ov, 1); moved = true; }
        else if (kv == LV_KEY_ENTER || kv == '\n' || kv == '\r' || kv == 13) activate_row(options_view_get_selected(s_ov));
    }
#ifdef CONFIG_USE_TOUCHSCREEN
    // Keep the bar arrows in sync when scrolling shifts with the selection.
    if (moved) fav_update_scroll_buttons();
#else
    (void)moved;
#endif
}

// --- view lifecycle ----------------------------------------------------------

#ifdef CONFIG_USE_TOUCHSCREEN
static void fav_scroll_up(void) {
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list || !lv_obj_is_valid(list)) return;
    lv_coord_t amt = lv_obj_get_height(list) / 2;
    lv_obj_scroll_by_bounded(list, 0, amt, LV_ANIM_OFF);
    fav_update_scroll_buttons();
}

static void fav_scroll_down(void) {
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list || !lv_obj_is_valid(list)) return;
    lv_coord_t amt = lv_obj_get_height(list) / 2;
    lv_obj_scroll_by_bounded(list, 0, -amt, LV_ANIM_OFF);
    fav_update_scroll_buttons();
}

static void fav_touch_go_back(void) {
    favorites_manager_handle_back();
}

static void fav_scroll_up_cb(lv_event_t *e) { (void)e; fav_scroll_up(); }
static void fav_scroll_down_cb(lv_event_t *e) { (void)e; fav_scroll_down(); }
static void fav_touch_back_cb(lv_event_t *e) { (void)e; fav_touch_go_back(); }

static void fav_update_scroll_buttons(void) {
    lv_obj_t *list = s_ov ? options_view_get_list(s_ov) : NULL;
    if (!list || !lv_obj_is_valid(list)) return;
    gui_touch_bar_update_visibility(&s_fav_touch_tb, list);
}
#endif

void favorites_manager_create(void) {
    const int touch_bar_h = gui_touch_bar_height();
    // Match the options-style screens (SubGHz/NFC): themed transparent root,
    // status-bar title comes from the options view itself.
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    s_root = gui_screen_create_root(NULL, NULL, bg, LV_OPA_TRANSP);
    favorites_manager_view.root = s_root;
    s_ov = options_view_create(s_root, "Favorites");

    lv_obj_t *list = options_view_get_list(s_ov);
    if (list && lv_obj_is_valid(list)) {
        int list_h = LV_VER_RES - GUI_STATUS_BAR_H - touch_bar_h;
        if (list_h < 40) list_h = 40;
        lv_obj_set_size(list, GUI_OPTIONS_LIST_WIDTH, list_h);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    // Standard bottom touch bar (scroll up / Back / scroll down).
    s_fav_touch_tb = gui_touch_bar_create(s_root);
    s_scroll_up_btn = s_fav_touch_tb.up_btn;
    s_touch_back_btn = s_fav_touch_tb.back_btn;
    s_scroll_down_btn = s_fav_touch_tb.down_btn;
    if (s_fav_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_fav_touch_tb,
                                    fav_scroll_up_cb, NULL,
                                    fav_touch_back_cb, NULL,
                                    fav_scroll_down_cb, NULL);
    }
#endif

    s_touch_started = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&s_favmgr_touch_drag);
#endif
    // Row/file buffers live on the heap for the lifetime of the screen so
    // low-RAM boards without PSRAM don't pay 2 KB+ of permanent BSS.
    free(s_rows);
    free(s_file_paths);
    s_rows = (fav_row_t *)malloc(sizeof(fav_row_t) * FAV_ROWS_MAX);
    s_file_paths = (char (*)[FAVORITE_NAME_LEN])malloc(FAV_FILES_MAX * FAVORITE_NAME_LEN);
    s_row_count = 0;
    s_file_count = 0;

    build_main_list(s_resume_row);
    // Entry animation, matching the other options-style lists.
    options_view_trigger_wipe(s_ov);
}

void favorites_manager_destroy(void) {
    // Remember where the cursor was for the next open.
    if (s_ov) s_resume_row = options_view_get_selected(s_ov);
    if (s_ov) { options_view_destroy(s_ov); s_ov = NULL; }
    lvgl_obj_del_safe(&s_root);
    favorites_manager_view.root = NULL;
#ifdef CONFIG_USE_TOUCHSCREEN
    gui_touch_bar_destroy(&s_fav_touch_tb);
    s_scroll_up_btn = NULL;
    s_scroll_down_btn = NULL;
    s_touch_back_btn = NULL;
#endif
    s_row_count = 0;
    free(s_rows);
    free(s_file_paths);
    s_rows = NULL;
    s_file_paths = NULL;
    s_picker_mode = false;
    s_picker_stage = 0;
    s_picker_category = 0;
    s_file_count = 0;
    s_touch_started = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&s_favmgr_touch_drag);
#endif
}

View favorites_manager_view = {
    .root = NULL,
    .create = favorites_manager_create,
    .destroy = favorites_manager_destroy,
    .input_callback = favorites_manager_input,
    .get_hardwareinput_callback = get_cb,
    .name = "Favorites Manager",
};
