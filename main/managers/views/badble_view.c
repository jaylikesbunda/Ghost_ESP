// badble_view.c
// Dedicated LVGL view for the BLE HID keyboard ("BadBLE") feature.
// Mirrors the BadUSB view flow: script picker + running popup, live keyboard
// typing, name setup, and status/stop.

#include "sdkconfig.h"

#ifdef CONFIG_HAS_BADBLE

#include "managers/views/badble_view.h"
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/popup.h"
#include "gui/lvgl_safe.h"
#include "gui/gui_router.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "managers/views/error_popup.h"
#include "managers/views/keyboard_screen.h"
#include "managers/badble_manager.h"
#include "managers/badusb_builtin_script.h"
#include "managers/sd_card_manager.h"
#include "core/serial_manager.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>

// JIT SD helper for configs that unmount SD after init (e.g. somethingsomething)
static bool badble_sd_begin(bool *display_was_suspended)
{
    return sd_card_jit_begin(display_was_suspended, false);
}

static void badble_sd_end(bool display_was_suspended)
{
    sd_card_jit_end(display_was_suspended);
}

typedef enum {
    BADBLE_MENU_MAIN,
    BADBLE_MENU_SCRIPT_SELECT,
} BadBleMenuState;

static BadBleMenuState current_menu_state = BADBLE_MENU_MAIN;

static const char *badble_main_options[] = {
    "Run Script",
    "BLE Keyboard",
    "Set Name",
    "Status",
    "Stop BadBLE",
    "< Back",
    NULL
};

#define MAX_SCRIPTS 32
#define MAX_SCRIPT_NAME 64
static char (*script_names)[MAX_SCRIPT_NAME];
static const char *script_options[MAX_SCRIPTS + 2];
static int script_count = 0;

static lv_obj_t *root = NULL;
static options_view_t *g_ov = NULL;
static lv_obj_t *menu_container = NULL;
static int selected_item_index = 0;
static int num_items = 0;

#ifdef CONFIG_USE_TOUCHSCREEN
static touch_drag_t badble_touch_drag = {0};
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int BADBLE_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int BADBLE_SWIPE_THRESHOLD_RATIO = 10;
#endif
#endif

static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static gui_touch_bar_t s_touch_tb = {0};

static lv_obj_t *badble_running_popup = NULL;
static lv_obj_t *badble_popup_title_lbl = NULL;
static lv_obj_t *badble_popup_body_lbl = NULL;
static lv_timer_t *badble_poll_timer = NULL;
static bool badble_popup_was_running = false;
static char badble_current_script[MAX_SCRIPT_NAME] = {0};

static void select_item(int index) {
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;
    selected_item_index = index;
    if (g_ov) {
        options_view_set_selected(g_ov, selected_item_index);
    }
}

static void handle_option(const char *option);
static void badble_dismiss_popup(void);

static void on_option_click(lv_event_t *e) {
    const char *opt = (const char *)lv_event_get_user_data(e);
    if (opt) handle_option(opt);
}

static void populate_script_list(void) {
    script_count = 0;

    free(script_names);
    script_names = calloc(MAX_SCRIPTS, sizeof(*script_names));
    if (!script_names) {
        script_options[0] = "< Back";
        script_options[1] = NULL;
        return;
    }

    strncpy(script_names[script_count], BADUSB_BUILTIN_SCRIPT_NAME, MAX_SCRIPT_NAME - 1);
    script_names[script_count][MAX_SCRIPT_NAME - 1] = '\0';
    script_count++;

    bool display_was_suspended = false;
    if (badble_sd_begin(&display_was_suspended)) {
        const char *dir_path = "/mnt/ghostesp/badble";
        DIR *dir = opendir(dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && script_count < MAX_SCRIPTS) {
                size_t len = strlen(entry->d_name);
                if (len > 4 && strcmp(entry->d_name + len - 4, ".txt") == 0) {
                    strncpy(script_names[script_count], entry->d_name, MAX_SCRIPT_NAME - 1);
                    script_names[script_count][MAX_SCRIPT_NAME - 1] = '\0';
                    script_count++;
                }
            }
            closedir(dir);
        }
        badble_sd_end(display_was_suspended);
    }

    for (int i = 0; i < script_count; i++) {
        script_options[i] = script_names[i];
    }
    script_options[script_count] = "< Back";
    script_options[script_count + 1] = NULL;
}

static void add_options_items(options_view_t *ov, const char **labels) {
    if (!ov || !labels) return;
    for (int i = 0; labels[i]; i++) {
        options_view_add_item(ov, labels[i], on_option_click, (void *)labels[i]);
    }
}

static void scroll_up_cb(lv_event_t *e) {
    (void)e;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
        lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
    }
}

static void scroll_down_cb(lv_event_t *e) {
    (void)e;
    if (menu_container && lv_obj_is_valid(menu_container)) {
        lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
        lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
    }
}

static void update_scroll_buttons_visibility(void) {
    gui_touch_bar_update_visibility(&s_touch_tb, menu_container);
}

static void rebuild_menu(void);
static void go_back(void);

static void back_btn_cb(lv_event_t *e) {
    (void)e;
    go_back();
}

static void badble_dismiss_popup(void) {
    if (badble_poll_timer) {
        lv_timer_del(badble_poll_timer);
        badble_poll_timer = NULL;
    }
    if (badble_running_popup && lv_obj_is_valid(badble_running_popup)) {
        lv_obj_del(badble_running_popup);
        badble_running_popup = NULL;
    }
    badble_popup_title_lbl = NULL;
    badble_popup_body_lbl = NULL;
    badble_popup_was_running = false;
}

/* Mirrors the BadUSB VSENSE poll: watches the manager state and flips the
 * popup from "Waiting for host" to "Running" once a host is connected and
 * the script task is executing, then auto-dismisses when the script ends. */
static void badble_popup_poll_cb(lv_timer_t *timer) {
    (void)timer;
    if (!badble_running_popup || !lv_obj_is_valid(badble_running_popup)) {
        badble_dismiss_popup();
        return;
    }
    if (!badble_manager_is_running()) {
        badble_dismiss_popup(); // stopped/cancelled
        return;
    }

    bool connected = badble_manager_is_connected();
    bool script_running = badble_manager_is_script_running();

    if (connected && !badble_popup_was_running) {
        badble_popup_was_running = true;
        if (badble_popup_title_lbl && lv_obj_is_valid(badble_popup_title_lbl)) {
            lv_label_set_text(badble_popup_title_lbl, "BadBLE Running");
        }
        if (badble_popup_body_lbl && lv_obj_is_valid(badble_popup_body_lbl)) {
            char body[96];
            snprintf(body, sizeof(body), "Script: %s", badble_current_script);
            lv_label_set_text(badble_popup_body_lbl, body);
        }
        return;
    }

    if (badble_popup_was_running && !script_running) {
        badble_dismiss_popup(); // script finished
    }
}

static void badble_cancel_cb(lv_event_t *e) {
    (void)e;
    (void)badble_manager_stop();
    badble_dismiss_popup();
    error_popup_create("BadBLE stopped");
}

static void show_running_popup(const char *script_name) {
    badble_dismiss_popup();

    strncpy(badble_current_script, script_name ? script_name : "", MAX_SCRIPT_NAME - 1);
    badble_current_script[MAX_SCRIPT_NAME - 1] = '\0';

    int popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 10;

    if (LV_VER_RES < 160) {
        popup_h = LV_VER_RES - 40;
        if (popup_h < 100) popup_h = 100;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 190) ? (LV_VER_RES - 40) : 130;
        if (popup_h < 110) popup_h = 110;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 130 : 140;
    }

    badble_running_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset, true);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    badble_popup_title_lbl = popup_create_title_label(badble_running_popup, "BadBLE Waiting", title_font, 12);

    char body[96];
    snprintf(body, sizeof(body), "Pair & connect, then run\n%s", script_name);

    int body_y_offset = (LV_VER_RES < 160) ? 32 : ((LV_VER_RES <= 200) ? 35 : 40);
    badble_popup_body_lbl = popup_create_body_label(badble_running_popup, body, popup_w - 20, true, body_font, body_y_offset);
    if (badble_popup_body_lbl) {
        lv_obj_set_style_text_align(badble_popup_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    int btn_w = 90, btn_h = 30;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 28; }
    lv_obj_t *cancel_btn = popup_add_styled_button(badble_running_popup, "Cancel", btn_w, btn_h,
                                                   LV_ALIGN_BOTTOM_MID, 0, -10, body_font,
                                                   badble_cancel_cb, NULL);
    if (cancel_btn) {
        popup_set_button_selected(cancel_btn, true);
    }

    badble_poll_timer = lv_timer_create(badble_popup_poll_cb, 100, NULL);
}

static void badble_set_name_kb_cb(const char *text) {
    keyboard_view_set_submit_callback(NULL);
    keyboard_view_set_immediate_callback(NULL);
    display_manager_switch_view(&badble_view);

    if (!text || text[0] == '\0') {
        error_popup_create("Enter a name");
        return;
    }
    if (badble_manager_set_name(text) != ESP_OK) {
        error_popup_create("Name rejected (max 31 chars)");
    } else {
        error_popup_create("BadBLE name saved");
    }
}

static void badble_key_immediate_cb(char c) {
    if (c == '\b') {
        (void)badble_manager_send_keypress(0, 0x2A); // Backspace
    } else if (c == '\n' || c == '\r') {
        (void)badble_manager_send_keypress(0, 0x28); // Enter
    } else {
        char one[2] = {c, '\0'};
        (void)badble_manager_send_text(one);
    }
}

static void badble_type_kb_cb(const char *text) {
    keyboard_view_set_submit_callback(NULL);
    keyboard_view_set_immediate_callback(NULL);
    display_manager_switch_view(&badble_view);
}

static void handle_option(const char *option) {
    if (!option) return;

    if (current_menu_state == BADBLE_MENU_MAIN) {
        if (strcmp(option, "Run Script") == 0) {
            populate_script_list();
            if (script_count == 0) {
                error_popup_create("No scripts found");
                return;
            }
            current_menu_state = BADBLE_MENU_SCRIPT_SELECT;
            rebuild_menu();
        } else if (strcmp(option, "BLE Keyboard") == 0) {
            esp_err_t ret = badble_manager_keyboard_start();
            if (ret != ESP_OK) {
                error_popup_create("Failed to start keyboard");
                return;
            }
            keyboard_view_set_return_view(&badble_view);
            keyboard_view_set_submit_callback(badble_type_kb_cb);
            keyboard_view_set_immediate_callback(badble_key_immediate_cb);
            keyboard_view_set_placeholder("Type text to send...");
            keyboard_view_set_initial_text("");
            keyboard_view_set_start_caps(false);
            display_manager_switch_view(&keyboard_view);
        } else if (strcmp(option, "Set Name") == 0) {
            keyboard_view_set_return_view(&badble_view);
            keyboard_view_set_submit_callback(badble_set_name_kb_cb);
            keyboard_view_set_immediate_callback(NULL);
            keyboard_view_set_placeholder("BadBLE name (max 31 chars)");
            keyboard_view_set_initial_text(badble_manager_get_name());
            keyboard_view_set_start_caps(false);
            display_manager_switch_view(&keyboard_view);
        } else if (strcmp(option, "Status") == 0) {
            char status[128];
            snprintf(status, sizeof(status),
                     "State: %s\nName: %s\nHost: %s",
                     badble_manager_is_running() ? "active" : "idle",
                     badble_manager_get_name(),
                     badble_manager_is_connected() ? "connected" : "not connected");
            error_popup_create(status);
        } else if (strcmp(option, "Stop BadBLE") == 0) {
            (void)badble_manager_stop();
            badble_dismiss_popup();
            error_popup_create("BadBLE stopped");
        } else if (strcmp(option, "< Back") == 0) {
            go_back();
        }
    } else if (current_menu_state == BADBLE_MENU_SCRIPT_SELECT) {
        if (strcmp(option, "< Back") == 0) {
            go_back();
            return;
        }
        if (strcmp(option, BADUSB_BUILTIN_SCRIPT_NAME) == 0) {
            badble_manager_run_builtin();
        } else {
            char cmd[128];
            snprintf(cmd, sizeof(cmd), "badble run %s", option);
            simulateCommand(cmd);
        }
        show_running_popup(option);
    }
}

static void go_back(void) {
    if (current_menu_state != BADBLE_MENU_MAIN) {
        current_menu_state = BADBLE_MENU_MAIN;
        rebuild_menu();
    } else {
        display_manager_go_back();
    }
}

static void rebuild_menu(void) {
    if (!g_ov) return;

    options_view_clear(g_ov);
    selected_item_index = 0;
    num_items = 0;

    const char **options = NULL;
    const char *title = "BadBLE";

    switch (current_menu_state) {
        case BADBLE_MENU_MAIN:
            options = badble_main_options;
            break;
        case BADBLE_MENU_SCRIPT_SELECT:
            options = script_options;
            title = "Select Script";
            break;
    }

    if (options) {
        options_view_set_title(g_ov, title);
        add_options_items(g_ov, options);
        for (const char **p = options; *p; p++) num_items++;
    }

    menu_container = options_view_get_list(g_ov);
    if (num_items > 0) {
        select_item(0);
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    update_scroll_buttons_visibility();
#endif
}

void badble_view_create(void) {
    display_manager_fill_screen(lv_color_hex(GUI_DEFAULT_BG_COLOR));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root_default(NULL, NULL);
    badble_view.root = root;

    g_ov = options_view_create(root, "BadBLE");
    menu_container = options_view_get_list(g_ov);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    /* Keep the P4 canvas edge-to-edge while the shared list padding keeps
     * each option row visually inset from the panel edges. */
    lv_obj_set_width(menu_container, GUI_OPTIONS_LIST_WIDTH);
    lv_obj_set_style_bg_opa(menu_container, LV_OPA_COVER, 0);
#endif

#ifdef CONFIG_USE_TOUCHSCREEN
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    const int BUTTON_AREA_HEIGHT = gui_touch_bar_height();
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, GUI_OPTIONS_LIST_WIDTH, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    /* Restore the submenu/row that was active before the view was swapped
     * away; only a fresh entry from the main menu starts at the root. */
    if (gui_router_previous_view() == &main_menu_view) {
        current_menu_state = BADBLE_MENU_MAIN;
        selected_item_index = 0;
    } else if (gui_router_previous_view() == &keyboard_view) {
        /* Leaving the live typing screen ends the BadBLE session: tear the
         * BLE profile down so the radio and RAM are released immediately. */
        (void)badble_manager_stop();
    }
    num_items = 0;

    const char *title = "BadBLE";
    const char **options = NULL;
    switch (current_menu_state) {
        case BADBLE_MENU_MAIN:
            options = badble_main_options;
            break;
        case BADBLE_MENU_SCRIPT_SELECT:
            options = script_options;
            title = "Select Script";
            break;
    }
    if (options) {
        options_view_set_title(g_ov, title);
        add_options_items(g_ov, options);
        for (const char **p = options; *p; p++) num_items++;
    }
    if (num_items > 0) {
        if (selected_item_index < 0 || selected_item_index >= num_items) selected_item_index = 0;
        select_item(selected_item_index);
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    s_touch_tb = gui_touch_bar_create(root);
    scroll_up_btn = s_touch_tb.up_btn;
    back_btn = s_touch_tb.back_btn;
    scroll_down_btn = s_touch_tb.down_btn;
    if (s_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_touch_tb, scroll_up_cb, NULL, back_btn_cb, NULL, scroll_down_cb, NULL);
        update_scroll_buttons_visibility();
    }
#endif
}

void badble_view_destroy(void) {
    badble_dismiss_popup();

    /* Poll timer is started on entry paths that do not always go through the
     * popup-close handlers; deleting here prevents a leak when exiting while
     * polling (matches badusb_view_destroy). */
    lvgl_timer_del_safe(&badble_poll_timer);

    if (g_ov) {
        options_view_destroy(g_ov);
        g_ov = NULL;
    }

    lvgl_obj_del_safe(&root);
    free(script_names);
    script_names = NULL;
    badble_view.root = NULL;
    menu_container = NULL;
    gui_touch_bar_destroy(&s_touch_tb);
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    /* selected_item_index/current_menu_state deliberately not reset here -
     * they need to survive the destroy() -> create() cycle so create() can
     * restore them when returning rather than always resetting to root. */
    num_items = 0;
}

static void get_badble_callback(void **callback) {
    *callback = badble_view.input_callback;
}

void badble_view_input_cb(InputEvent *event) {
    if (badble_running_popup && lv_obj_is_valid(badble_running_popup)) {
        /* While the running/waiting popup is visible it owns all input. */
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_REL) {
                /* A tap on the Cancel button itself is delivered by LVGL as a
                 * normal click; any other tap cancels as well. */
                bool on_cancel_btn = false;
                lv_obj_t *btn = lv_obj_get_child(badble_running_popup, -1);
                if (btn && lv_obj_is_valid(btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        on_cancel_btn = true;
                    }
                }
                if (!on_cancel_btn) {
                    badble_cancel_cb(NULL);
                }
            }
            return;
        }
        if (event->type == INPUT_TYPE_KEYBOARD) {
            uint8_t key = event->data.key_value;
            if (key == 13 || key == 10 || key == 27 || key == 29 || key == 'c' || key == 'C') {
                badble_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            /* Single-button dialog: confirm (1) or back (0) cancels. */
            if (event->data.joystick_index == 0 || event->data.joystick_index == 1) {
                badble_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                badble_cancel_cb(NULL);
                return;
            }
        }
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
#ifdef CONFIG_USE_TOUCHSCREEN
        if (data->state == LV_INDEV_STATE_PR) {
            if (gui_touch_bar_hit(scroll_up_btn, data->point.x, data->point.y)) {
                scroll_up_cb(NULL);
                touch_drag_reset(&badble_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(scroll_down_btn, data->point.x, data->point.y)) {
                scroll_down_cb(NULL);
                touch_drag_reset(&badble_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(back_btn, data->point.x, data->point.y)) {
                go_back();
                touch_drag_reset(&badble_touch_drag);
                return;
            }
            if (!badble_touch_drag.started) {
                touch_drag_begin(&badble_touch_drag, data);
            } else {
                lv_area_t cont_area;
                if (menu_container && lv_obj_is_valid(menu_container)) {
                    lv_obj_get_coords(menu_container, &cont_area);
                    bool started_in_container = (badble_touch_drag.start_x >= cont_area.x1 && badble_touch_drag.start_x <= cont_area.x2 &&
                                                 badble_touch_drag.start_y >= cont_area.y1 && badble_touch_drag.start_y <= cont_area.y2);
                    if (started_in_container) {
                        touch_drag_update(&badble_touch_drag, data, menu_container);
                    }
                }
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!badble_touch_drag.started) return;

            if (!menu_container || !lv_obj_is_valid(menu_container)) {
                touch_drag_reset(&badble_touch_drag);
                return;
            }

            int thr_x = LV_HOR_RES / BADBLE_SWIPE_THRESHOLD_RATIO;
            int dx = data->point.x - badble_touch_drag.start_x;

            int saved_start_x = badble_touch_drag.start_x;
            int saved_start_y = badble_touch_drag.start_y;
            bool was_dragged = touch_drag_release(&badble_touch_drag, data);
            if (was_dragged) {
                display_manager_flush_pending_scroll();
                update_scroll_buttons_visibility();
                return;
            }

            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            bool started_in_container = (saved_start_x >= cont_area.x1 && saved_start_x <= cont_area.x2 &&
                                          saved_start_y >= cont_area.y1 && saved_start_y <= cont_area.y2);
            if (!started_in_container) return;
            if (abs(dx) > thr_x) return;

            if (settings_get_thirds_control_enabled(&G_Settings)) {
                int container_h = (int)(cont_area.y2 - cont_area.y1);
                if (container_h > 0) {
                    int y_rel = (int)data->point.y - (int)cont_area.y1;
                    if (y_rel < container_h / 3) {
                        if (g_ov) options_view_move_selection(g_ov, -1);
                        selected_item_index = g_ov ? options_view_get_selected(g_ov) : 0;
                        return;
                    } else if (y_rel > (container_h * 2) / 3) {
                        if (g_ov) options_view_move_selection(g_ov, 1);
                        selected_item_index = g_ov ? options_view_get_selected(g_ov) : 0;
                        return;
                    }
                }
            }

            for (int i = 0; i < num_items; i++) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                if (!btn) continue;
                lv_area_t btn_area;
                lv_obj_get_coords(btn, &btn_area);
                if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                    data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                    select_item(i);
                    lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
            return;
        }
#else
        if (data->state == LV_INDEV_STATE_PR) return;
        if (!menu_container || !g_ov) return;
        int cnt = options_view_get_item_count(g_ov);
        for (int i = 0; i < cnt; i++) {
            lv_obj_t *btn = lv_obj_get_child(menu_container, i);
            if (!btn) continue;
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            if (data->point.x >= a.x1 && data->point.x <= a.x2 &&
                data->point.y >= a.y1 && data->point.y <= a.y2) {
                select_item(i);
                lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                return;
            }
        }
        go_back();
#endif
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) {
            if (g_ov) { options_view_move_selection(g_ov, -1); selected_item_index = options_view_get_selected(g_ov); }
        } else if (button == 4) {
            if (g_ov) { options_view_move_selection(g_ov, 1); selected_item_index = options_view_get_selected(g_ov); }
        } else if (button == 1) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
        } else if (button == 0) {
            go_back();
        }
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if (keyValue == 'k' || keyValue == 59 || keyValue == ';' ||
            keyValue == 'h' || keyValue == 44 || keyValue == ',') {
            if (g_ov) { options_view_move_selection(g_ov, -1); selected_item_index = options_view_get_selected(g_ov); }
        } else if (keyValue == 'j' || keyValue == 46 || keyValue == '.' ||
                   keyValue == 'l' || keyValue == 47 || keyValue == '/') {
            if (g_ov) { options_view_move_selection(g_ov, 1); selected_item_index = options_view_get_selected(g_ov); }
        } else if (keyValue == 13) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
        } else if (keyValue == 29 || keyValue == '`') {
            go_back();
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_item_index);
            if (selected_obj) lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
        } else {
            if (g_ov) {
                options_view_move_selection(g_ov, event->data.encoder.direction > 0 ? 1 : -1);
                selected_item_index = options_view_get_selected(g_ov);
            }
        }
        return;
    }

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
    }
#endif
}

View badble_view = {
    .root = NULL,
    .create = badble_view_create,
    .destroy = badble_view_destroy,
    .input_callback = badble_view_input_cb,
    .name = "BadBLE",
    .get_hardwareinput_callback = get_badble_callback
};

#endif // CONFIG_HAS_BADBLE
