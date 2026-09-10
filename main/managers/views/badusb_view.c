#include "sdkconfig.h"

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)

#include "managers/views/badusb_view.h"
#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/trackpad_view.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/popup.h"
#include "gui/lvgl_safe.h"
#include "gui/gui_router.h"
#include "managers/views/error_popup.h"
#include "esp_attr.h"
#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "core/esp_comm_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

#include "managers/sd_card_manager.h"
#include "managers/badusb_builtin_script.h"

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#endif

static const char *TAG = "badusb_view";

// JIT SD helpers for configs that unmount SD after init (e.g. somethingsomething)
static bool badusb_sd_begin(bool *display_was_suspended)
{
    return sd_card_jit_begin(display_was_suspended, false);
}

static void badusb_sd_end(bool display_was_suspended)
{
    sd_card_jit_end(display_was_suspended);
}

typedef enum {
    BADUSB_MENU_MAIN,
    BADUSB_MENU_SCRIPT_SELECT,
    BADUSB_MENU_SETTINGS,
} BadUsbMenuState;

static BadUsbMenuState current_menu_state = BADUSB_MENU_MAIN;

static const char *badusb_main_options[] = {
    "Run Script",
    "USB Keyboard",
    "Mouse Jiggler",
    "Trackpad",
    "Settings",
    "< Back",
    NULL
};

#define BADUSB_SETTINGS_COUNT 8
EXT_RAM_BSS_ATTR static char settings_labels[BADUSB_SETTINGS_COUNT][80];
static const char *settings_options[BADUSB_SETTINGS_COUNT + 1];
static const char *kb_layout_names[] = {"US", "DE", "FR", "UK", "ES"};

static void populate_settings_labels(void) {
    snprintf(settings_labels[0], 80, "VID: 0x%04X", settings_get_badusb_vid(&G_Settings));
    snprintf(settings_labels[1], 80, "PID: 0x%04X", settings_get_badusb_pid(&G_Settings));
    snprintf(settings_labels[2], 80, "Manufacturer: %s", settings_get_badusb_manufacturer(&G_Settings));
    snprintf(settings_labels[3], 80, "Product: %s", settings_get_badusb_product(&G_Settings));
    uint8_t layout = settings_get_badusb_kb_layout(&G_Settings);
    if (layout >= KB_LAYOUT_COUNT) layout = KB_LAYOUT_US;
    snprintf(settings_labels[4], 80, "Layout: %s", kb_layout_names[layout]);
    snprintf(settings_labels[5], 80, "Randomize: %s", settings_get_badusb_randomize(&G_Settings) ? "On" : "Off");
    strcpy(settings_labels[6], "Reset To Defaults");
    strcpy(settings_labels[7], "< Back");
    for (int i = 0; i < BADUSB_SETTINGS_COUNT; i++) {
        settings_options[i] = settings_labels[i];
    }
    settings_options[BADUSB_SETTINGS_COUNT] = NULL;
}

#define MAX_SCRIPTS 32
#define MAX_SCRIPT_NAME 64
EXT_RAM_BSS_ATTR static char script_names[MAX_SCRIPTS][MAX_SCRIPT_NAME];
static const char *script_options[MAX_SCRIPTS + 2];
static int script_count = 0;

static lv_obj_t *root = NULL;
static options_view_t *g_ov = NULL;
static lv_obj_t *menu_container = NULL;
static int selected_item_index = 0;
static int num_items = 0;

#ifdef CONFIG_USE_TOUCHSCREEN
static touch_drag_t badusb_touch_drag = {0};
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int BADUSB_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int BADUSB_SWIPE_THRESHOLD_RATIO = 10;
#endif
#endif

static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static gui_touch_bar_t s_touch_tb = {0};

static lv_obj_t *badusb_running_popup = NULL;
static lv_obj_t *badusb_popup_title_lbl = NULL;
static lv_obj_t *badusb_popup_body_lbl = NULL;
static popup_confirm_t *badusb_confirm_popup = NULL;
static lv_timer_t *vsense_poll_timer = NULL;
static char vsense_pending_script[MAX_SCRIPT_NAME];

static bool badusb_is_remote(void) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
    return true;
#else
    return false;
#endif
}

static void select_item(int index) {
    if (index < 0) index = num_items - 1;
    if (index >= num_items) index = 0;
    selected_item_index = index;
    if (g_ov) {
        options_view_set_selected(g_ov, selected_item_index);
    }
}

static void handle_option(const char *option);
static bool badusb_confirm_handle_input(InputEvent *event);
static void badusb_reset_defaults_confirm_cb(void *user_data);

static void on_option_click(lv_event_t *e) {
    const char *opt = (const char *)lv_event_get_user_data(e);
    if (opt) handle_option(opt);
}

static void populate_script_list(void) {
    script_count = 0;

    strncpy(script_names[script_count], BADUSB_BUILTIN_SCRIPT_NAME, MAX_SCRIPT_NAME - 1);
    script_names[script_count][MAX_SCRIPT_NAME - 1] = '\0';
    script_count++;

    bool display_was_suspended = false;
    if (badusb_sd_begin(&display_was_suspended)) {
        const char *dir_path = "/mnt/ghostesp/badusb";
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
        badusb_sd_end(display_was_suspended);
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

static bool badusb_confirm_handle_input(InputEvent *event) {
    if (!popup_confirm_is_open(badusb_confirm_popup)) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) return popup_confirm_handle_touch(&badusb_confirm_popup, &event->data.touch_data);
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        popup_confirm_cancel(&badusb_confirm_popup);
        return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 1) popup_confirm_select(&badusb_confirm_popup);
        else if (button == 0) popup_confirm_set_selected(badusb_confirm_popup, 0);
        else if (button == 3) popup_confirm_set_selected(badusb_confirm_popup, 1);
        else if (button == 2 || button == 4) popup_confirm_move(badusb_confirm_popup, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == 13 || key == LV_KEY_ENTER) popup_confirm_select(&badusb_confirm_popup);
        else if (key == 27 || key == 29 || key == LV_KEY_ESC || key == '`') popup_confirm_cancel(&badusb_confirm_popup);
        else if (key == 'h' || key == 'l' || key == 'k' || key == 'j' || key == ',' || key == '.' || key == ';' || key == '/' ||
                 key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) popup_confirm_move(badusb_confirm_popup, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) popup_confirm_select(&badusb_confirm_popup);
        else if (event->data.encoder.direction != 0) popup_confirm_move(badusb_confirm_popup, event->data.encoder.direction);
        return true;
    }

    return true;
}

static void badusb_reset_defaults_confirm_cb(void *user_data) {
    (void)user_data;
    settings_reset_badusb_defaults(&G_Settings);
    settings_persist_setting(SETTING_BADUSB_VID);
    settings_persist_setting(SETTING_BADUSB_PID);
    settings_persist_setting(SETTING_BADUSB_MANUFACTURER);
    settings_persist_setting(SETTING_BADUSB_PRODUCT);
    settings_persist_setting(SETTING_BADUSB_RANDOMIZE);
    settings_persist_setting(SETTING_BADUSB_KB_LAYOUT);
    populate_settings_labels();
    rebuild_menu();
}

static void back_btn_cb(lv_event_t *e) {
    (void)e;
    go_back();
}

static void badusb_vid_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid VID");
        return;
    }
    uint16_t val = (uint16_t)strtol(text, NULL, 16);
    settings_set_badusb_vid(&G_Settings, val);
    settings_persist_setting(SETTING_BADUSB_VID);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_pid_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid PID");
        return;
    }
    uint16_t val = (uint16_t)strtol(text, NULL, 16);
    settings_set_badusb_pid(&G_Settings, val);
    settings_persist_setting(SETTING_BADUSB_PID);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_mfr_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid name");
        return;
    }
    settings_set_badusb_manufacturer(&G_Settings, text);
    settings_persist_setting(SETTING_BADUSB_MANUFACTURER);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_prod_kb_cb(const char *text) {
    if (!text || strlen(text) == 0) {
        error_popup_create("Invalid name");
        return;
    }
    settings_set_badusb_product(&G_Settings, text);
    settings_persist_setting(SETTING_BADUSB_PRODUCT);
    keyboard_view_set_submit_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_type_kb_cb(const char *text) {
    keyboard_view_set_submit_callback(NULL);
    keyboard_view_set_immediate_callback(NULL);
    display_manager_switch_view(&badusb_view);
}

static void badusb_key_immediate_cb(char c) {
    char cmd[32];
    if (c == '\b') {
        snprintf(cmd, sizeof(cmd), "keysend 0 0x2A");
    } else if (c == '\n' || c == '\r') {
        snprintf(cmd, sizeof(cmd), "keysend 0 0x28");  // Enter key
    } else {
        snprintf(cmd, sizeof(cmd), "type_char %u", (unsigned char)c);
    }
#ifdef CONFIG_HAS_BADUSB_REMOTE
    esp_comm_manager_send_command("badusb", cmd);
#elif defined(CONFIG_HAS_BADUSB)
    if (c == '\b') badusb_manager_send_keypress(0, 0x2A);
    else if (c == '\n' || c == '\r') badusb_manager_send_keypress(0, 0x28);
    else {
        char one[2] = {c, '\0'};
        badusb_manager_send_text(one);
    }
#endif
}

static void badusb_cancel_cb(lv_event_t *e) {
    (void)e;
    bool remote = badusb_is_remote();
    if (remote) {
        simulateCommand("commsend badusb stop");
    } else {
#ifdef CONFIG_HAS_BADUSB
        badusb_manager_stop();
#endif
    }
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;
    error_popup_create("BadUSB stopped");
}

static void show_running_popup_ex(const char *script_name, bool waiting_for_usb) {
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;

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

    badusb_running_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset, true);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    const char *title = waiting_for_usb ? "Waiting for USB..." : "BadUSB Running";
    badusb_popup_title_lbl = popup_create_title_label(badusb_running_popup, title, title_font, 12);

    char body[80];
    if (waiting_for_usb) {
        snprintf(body, sizeof(body), "Plug in to execute\n%s", script_name);
    } else {
        snprintf(body, sizeof(body), "Script: %s", script_name);
    }
    
    // smaller screens need tighter spacing to avoid overlap with cancel button
    int body_y_offset = (LV_VER_RES < 160) ? 32 : ((LV_VER_RES <= 200) ? 35 : 40);
    badusb_popup_body_lbl = popup_create_body_label(badusb_running_popup, body, popup_w - 20, true, body_font, body_y_offset);
    if (badusb_popup_body_lbl) {
        lv_obj_set_style_text_align(badusb_popup_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    // Store script name for status updates
    strncpy(vsense_pending_script, script_name, MAX_SCRIPT_NAME - 1);
    vsense_pending_script[MAX_SCRIPT_NAME - 1] = '\0';

    int btn_w = 90, btn_h = 30;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 28; }
    lv_obj_t *cancel_btn = popup_add_styled_button(badusb_running_popup, "Cancel", btn_w, btn_h,
                                                   LV_ALIGN_BOTTOM_MID, 0, -10, body_font,
                                                   badusb_cancel_cb, NULL);
    if (cancel_btn) {
        popup_set_button_selected(cancel_btn, true);
    }
}

static void show_running_popup(const char *script_name) {
    show_running_popup_ex(script_name, false);
}

// Update the popup in-place when receiving status from S3 (remote) or VSENSE poll (standalone)
static void badusb_popup_set_running(void) {
    if (!badusb_running_popup || !lv_obj_is_valid(badusb_running_popup)) return;
    if (badusb_popup_title_lbl && lv_obj_is_valid(badusb_popup_title_lbl)) {
        lv_label_set_text(badusb_popup_title_lbl, "BadUSB Running");
    }
    if (badusb_popup_body_lbl && lv_obj_is_valid(badusb_popup_body_lbl)) {
        char body[80];
        snprintf(body, sizeof(body), "Script: %s", vsense_pending_script);
        lv_label_set_text(badusb_popup_body_lbl, body);
    }
}

static void badusb_popup_set_done(void) {
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;
}

#ifdef CONFIG_HAS_BADUSB
// LVGL timer callback for standalone VSENSE polling
static void vsense_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (badusb_vsense_connected()) {
        // VBUS detected - update popup to "Running"
        badusb_popup_set_running();
        // Stop polling
        if (vsense_poll_timer) {
            lv_timer_del(vsense_poll_timer);
            vsense_poll_timer = NULL;
        }
    }
}
#endif

// Public: called from command handler when S3 sends "badusb status <state>" to C5
void badusb_view_update_status(const char *status) {
    if (!status) return;
    if (strcmp(status, "waiting") == 0) {
        // S3 is waiting for VBUS - show waiting popup if not already showing
        if (!badusb_running_popup || !lv_obj_is_valid(badusb_running_popup)) {
            show_running_popup_ex(vsense_pending_script, true);
        }
    } else if (strcmp(status, "running") == 0) {
        badusb_popup_set_running();
    } else if (strcmp(status, "jiggling") == 0) {
        if (!badusb_running_popup || !lv_obj_is_valid(badusb_running_popup)) {
            show_running_popup_ex("Mouse Jiggler", false);
        }
        if (badusb_popup_title_lbl && lv_obj_is_valid(badusb_popup_title_lbl)) {
            lv_label_set_text(badusb_popup_title_lbl, "Mouse Jiggler");
        }
        if (badusb_popup_body_lbl && lv_obj_is_valid(badusb_popup_body_lbl)) {
            lv_label_set_text(badusb_popup_body_lbl, "Jiggling mouse...");
        }
    } else if (strcmp(status, "keyboard") == 0) {
        // Keyboard mode uses keyboard_view directly; do not cover it with a popup.
        if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
            lv_obj_del(badusb_running_popup);
            badusb_running_popup = NULL;
        }
        badusb_popup_title_lbl = NULL;
        badusb_popup_body_lbl = NULL;
    } else if (strcmp(status, "trackpad") == 0) {
        // Trackpad mode is driven by the remote view itself; do not pop a
        // popup over it. This status is informational only.
        if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
            lv_obj_del(badusb_running_popup);
            badusb_running_popup = NULL;
        }
        badusb_popup_title_lbl = NULL;
        badusb_popup_body_lbl = NULL;
    } else if (strcmp(status, "done") == 0) {
        badusb_popup_set_done();
    }
}

#ifdef CONFIG_HAS_BADUSB_REMOTE
#define STREAM_CHUNK_SIZE 56
#define BADUSB_SETTINGS_SEND_DELAY_MS 50

static void badusb_send_settings_to_peer(void) {
    char cmd[128];

    snprintf(cmd, sizeof(cmd), "set_vid 0x%04X", settings_get_badusb_vid(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_pid 0x%04X", settings_get_badusb_pid(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_mfr \"%s\"", settings_get_badusb_manufacturer(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_prod \"%s\"", settings_get_badusb_product(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_rand %u", settings_get_badusb_randomize(&G_Settings) ? 1 : 0);
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));

    snprintf(cmd, sizeof(cmd), "set_layout %u", settings_get_badusb_kb_layout(&G_Settings));
    esp_comm_manager_send_command("badusb", cmd);
    vTaskDelay(pdMS_TO_TICKS(BADUSB_SETTINGS_SEND_DELAY_MS));
}

static bool badusb_send_script_to_peer(const char *name) {
    bool display_was_suspended = false;
    if (!badusb_sd_begin(&display_was_suspended)) {
        error_popup_create("Failed to mount SD");
        return false;
    }

    char path[128];
    snprintf(path, sizeof(path), "/mnt/ghostesp/badusb/%s", name);

    FILE *f = fopen(path, "r");
    if (!f) {
        badusb_sd_end(display_was_suspended);
        error_popup_create("Failed to open script");
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        badusb_sd_end(display_was_suspended);
        error_popup_create("Invalid script size");
        return false;
    }

    char exec_data[32];
    snprintf(exec_data, sizeof(exec_data), "exec %ld", file_size);
    if (!esp_comm_manager_send_command("badusb", exec_data)) {
        fclose(f);
        badusb_sd_end(display_was_suspended);
        error_popup_create("Failed to send command");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t chunk[STREAM_CHUNK_SIZE];
    size_t total_sent = 0;
    bool ok = true;

    while (total_sent < (size_t)file_size) {
        size_t n = fread(chunk, 1, STREAM_CHUNK_SIZE, f);
        if (n == 0) break;

        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_BADUSB, chunk, n)) {
            ok = false;
            break;
        }
        total_sent += n;

        vTaskDelay(pdMS_TO_TICKS(15));
    }

    fclose(f);
    badusb_sd_end(display_was_suspended);

    if (!ok || total_sent != (size_t)file_size) {
        error_popup_create("Script transfer failed");
        return false;
    }

    ESP_LOGI(TAG, "Streamed %zu bytes to peer", total_sent);
    return true;
}
static bool badusb_send_builtin_to_peer(void) {
    size_t script_size = BADUSB_BUILTIN_SCRIPT_LEN;

    char exec_data[32];
    snprintf(exec_data, sizeof(exec_data), "exec %zu", script_size);
    if (!esp_comm_manager_send_command("badusb", exec_data)) {
        error_popup_create("Failed to send command");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    const uint8_t *data = (const uint8_t *)badusb_builtin_script;
    size_t total_sent = 0;
    bool ok = true;

    while (total_sent < script_size) {
        size_t remaining = script_size - total_sent;
        size_t n = (remaining < STREAM_CHUNK_SIZE) ? remaining : STREAM_CHUNK_SIZE;

        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_BADUSB, data + total_sent, n)) {
            ok = false;
            break;
        }
        total_sent += n;
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (!ok || total_sent != script_size) {
        error_popup_create("Script transfer failed");
        return false;
    }

    ESP_LOGI(TAG, "Streamed %zu bytes (built-in) to peer", total_sent);
    return true;
}
#endif // CONFIG_HAS_BADUSB_REMOTE

static void handle_option(const char *option) {
    if (!option) return;
    bool remote = badusb_is_remote();

    if (current_menu_state == BADUSB_MENU_MAIN) {
        if (strcmp(option, "Settings") == 0) {
            populate_settings_labels();
            current_menu_state = BADUSB_MENU_SETTINGS;
            rebuild_menu();
        } else if (strcmp(option, "Run Script") == 0) {
            if (remote && !esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to peer");
                return;
            }
            populate_script_list();
            if (script_count == 0) {
                error_popup_create("No scripts found");
                return;
            }
            current_menu_state = BADUSB_MENU_SCRIPT_SELECT;
            rebuild_menu();
        } else if (strcmp(option, "USB Keyboard") == 0) {
            if (remote && !esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to peer");
                return;
            }
            if (remote) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
                esp_comm_manager_send_command("badusb", "keyboard_start");
                keyboard_view_set_return_view(&badusb_view);
                keyboard_view_set_submit_callback(badusb_type_kb_cb);
                keyboard_view_set_immediate_callback(badusb_key_immediate_cb);
                keyboard_view_set_placeholder("Type text to send...");
                keyboard_view_set_initial_text("");
                keyboard_view_set_start_caps(false);
                display_manager_switch_view(&keyboard_view);
#endif
            } else {
#ifdef CONFIG_HAS_BADUSB
                esp_err_t ret = badusb_manager_keyboard_mode_start();
                if (ret != ESP_OK) {
                    error_popup_create("Failed to start keyboard");
                    return;
                }
                keyboard_view_set_return_view(&badusb_view);
                keyboard_view_set_submit_callback(badusb_type_kb_cb);
                keyboard_view_set_immediate_callback(badusb_key_immediate_cb);
                keyboard_view_set_placeholder("Type text to send...");
                keyboard_view_set_initial_text("");
                keyboard_view_set_start_caps(false);
                display_manager_switch_view(&keyboard_view);
#endif
            }
        } else if (strcmp(option, "Mouse Jiggler") == 0) {
            if (remote && !esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to peer");
                return;
            }
            if (remote) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
                esp_comm_manager_send_command("badusb", "jiggle_start");
                show_running_popup_ex("Mouse Jiggler", false);
#endif
            } else {
#ifdef CONFIG_HAS_BADUSB
                esp_err_t ret = badusb_manager_mouse_jiggle_start();
                if (ret != ESP_OK) {
                    error_popup_create("Failed to start jiggler");
                    return;
                }
                show_running_popup_ex("Mouse Jiggler", false);
#endif
            }
        } else if (strcmp(option, "Trackpad") == 0) {
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_TOUCHSCREEN) && !defined(CONFIG_USE_CARDPUTER) && !defined(CONFIG_USE_CARDPUTER_ADV) && !defined(CONFIG_USE_TDECK) && !defined(CONFIG_USE_JOYSTICK)
            error_popup_create("No trackpad input");
            return;
#else
            if (remote && !esp_comm_manager_is_connected()) {
                error_popup_create("Not connected to peer");
                return;
            }
            trackpad_view_set_return_view(&badusb_view);
            if (remote) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
                esp_comm_manager_send_command("badusb", "trackpad_start");
                display_manager_switch_view(&trackpad_view);
#endif
            } else {
#ifdef CONFIG_HAS_BADUSB
                esp_err_t ret = badusb_manager_trackpad_start();
                if (ret != ESP_OK) {
                    error_popup_create("Failed to start trackpad");
                    return;
                }
                display_manager_switch_view(&trackpad_view);
#endif
            }
#endif
        } else if (strcmp(option, "< Back") == 0) {
            go_back();
        }
    } else if (current_menu_state == BADUSB_MENU_SETTINGS) {
        if (strncmp(option, "VID:", 4) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_vid_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "PID:", 4) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_pid_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Manufacturer:", 13) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_mfr_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Product:", 8) == 0) {
            keyboard_view_set_return_view(&badusb_view);
            keyboard_view_set_submit_callback(badusb_prod_kb_cb);
            display_manager_switch_view(&keyboard_view);
        } else if (strncmp(option, "Layout:", 7) == 0) {
            uint8_t layout = settings_get_badusb_kb_layout(&G_Settings);
            layout = (layout + 1) % KB_LAYOUT_COUNT;
            settings_set_badusb_kb_layout(&G_Settings, layout);
            settings_persist_setting(SETTING_BADUSB_KB_LAYOUT);
            snprintf(settings_labels[4], 80, "Layout: %s", kb_layout_names[layout]);
            options_view_update_item_text(g_ov, 4, settings_labels[4]);
        } else if (strncmp(option, "Randomize:", 10) == 0) {
            bool enabled = !settings_get_badusb_randomize(&G_Settings);
            settings_set_badusb_randomize(&G_Settings, enabled);
            settings_persist_setting(SETTING_BADUSB_RANDOMIZE);
            snprintf(settings_labels[5], 80, "Randomize: %s", enabled ? "On" : "Off");
            options_view_update_item_text(g_ov, 5, settings_labels[5]);
        } else if (strcmp(option, "Reset To Defaults") == 0) {
            popup_confirm_show(&badusb_confirm_popup, lv_layer_top(), "Reset Defaults?",
                               "Reset BadUSB settings to firmware defaults?",
                               "Reset", "Cancel", badusb_reset_defaults_confirm_cb, NULL);
        } else if (strcmp(option, "< Back") == 0) {
            go_back();
        }
    } else if (current_menu_state == BADUSB_MENU_SCRIPT_SELECT) {
        if (strcmp(option, "< Back") == 0) {
            go_back();
            return;
        }
        bool is_builtin = (strcmp(option, BADUSB_BUILTIN_SCRIPT_NAME) == 0);
        if (remote) {
#ifdef CONFIG_HAS_BADUSB_REMOTE
            badusb_send_settings_to_peer();
            bool ok = is_builtin ? badusb_send_builtin_to_peer()
                                 : badusb_send_script_to_peer(option);
            if (ok) {
                show_running_popup(option);
            }
#endif
        } else {
#ifdef CONFIG_HAS_BADUSB
            bool has_vsense = badusb_has_vsense();
            bool already_connected = has_vsense && badusb_vsense_connected();

            if (is_builtin) {
                char *buf = strdup(badusb_builtin_script);
                if (buf) {
                    badusb_manager_execute_buffer(buf, BADUSB_BUILTIN_SCRIPT_LEN);
                }
            } else {
                char cmd[128];
                snprintf(cmd, sizeof(cmd), "badusb run %s", option);
                simulateCommand(cmd);
            }

            if (has_vsense && !already_connected) {
                show_running_popup_ex(option, true);
                if (vsense_poll_timer) {
                    lv_timer_del(vsense_poll_timer);
                }
                vsense_poll_timer = lv_timer_create(vsense_poll_timer_cb, 100, NULL);
            } else {
                show_running_popup(option);
            }
#endif
        }
    }
}

static void go_back(void) {
    if (current_menu_state != BADUSB_MENU_MAIN) {
        current_menu_state = BADUSB_MENU_MAIN;
        rebuild_menu();
    } else {
        display_manager_go_back();
    }
}

// Deep-link support for favorites: run a specific script by name (as shown
// in the Select Script list). Safe to call before the view exists - the
// request is consumed by badusb_view_create().
static char *s_pending_script = NULL;
static void badusb_view_apply_pending_open(void);

void badusb_view_open_script(const char *name) {
    if (!name || !name[0]) return;
    free(s_pending_script);
    s_pending_script = strdup(name);
    // View already live (e.g. lockscreen overlay on top of it): apply now.
    if (g_ov && lv_obj_is_valid(g_ov)) {
        badusb_view_apply_pending_open();
    }
}

static void badusb_view_apply_pending_open(void) {
    if (!s_pending_script) return;
    char name[MAX_SCRIPT_NAME];
    strncpy(name, s_pending_script, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    free(s_pending_script);
    s_pending_script = NULL;

    if (badusb_is_remote() && !esp_comm_manager_is_connected()) {
        error_popup_create("Not connected to peer");
        return;
    }
    populate_script_list();
    current_menu_state = BADUSB_MENU_SCRIPT_SELECT;
    rebuild_menu();
    handle_option(name); // executes the payload exactly as the picker would
}

static void rebuild_menu(void) {
    if (!g_ov) return;

    options_view_clear(g_ov);
    selected_item_index = 0;
    num_items = 0;

    const char **options = NULL;
    const char *title = "BadUSB";

    switch (current_menu_state) {
        case BADUSB_MENU_MAIN:
            options = badusb_main_options;
            break;
        case BADUSB_MENU_SCRIPT_SELECT:
            options = script_options;
            title = "Select Script";
            break;
        case BADUSB_MENU_SETTINGS:
            options = settings_options;
            title = "Settings";
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

void badusb_view_create(void) {
    display_manager_fill_screen(lv_color_hex(GUI_DEFAULT_BG_COLOR));
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    root = gui_screen_create_root_default(NULL, NULL);
    badusb_view.root = root;

    g_ov = options_view_create(root, "BadUSB");
    menu_container = options_view_get_list(g_ov);

#ifdef CONFIG_USE_TOUCHSCREEN
    int screen_height = LV_VER_RES;
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    const int BUTTON_AREA_HEIGHT = gui_touch_bar_height();
    int container_height = screen_height - STATUS_BAR_HEIGHT - BUTTON_AREA_HEIGHT;
    lv_obj_set_size(menu_container, GUI_OPTIONS_LIST_WIDTH, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    /* current_menu_state/selected_item_index only reset on a genuine fresh
     * entry from the Main Menu; returning here (e.g. after a hardware
     * shortcut jumped elsewhere and back) restores whichever submenu and
     * row were active instead of always landing back on the root menu. */
    if (gui_router_previous_view() == &main_menu_view) {
        current_menu_state = BADUSB_MENU_MAIN;
        selected_item_index = 0;
    }
    num_items = 0;

    const char *title = "BadUSB";
    const char **options = NULL;
    switch (current_menu_state) {
        case BADUSB_MENU_MAIN:
            options = badusb_main_options;
            break;
        case BADUSB_MENU_SCRIPT_SELECT:
            options = script_options;
            title = "Select Script";
            break;
        case BADUSB_MENU_SETTINGS:
            options = settings_options;
            title = "Settings";
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

    // Consume any pending favorite deep-link (badusb_view_open_script).
    badusb_view_apply_pending_open();
}

void badusb_view_destroy(void) {
    if (vsense_poll_timer) {
        lv_timer_del(vsense_poll_timer);
        vsense_poll_timer = NULL;
    }
    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        lv_obj_del(badusb_running_popup);
        badusb_running_popup = NULL;
    }
    badusb_popup_title_lbl = NULL;
    badusb_popup_body_lbl = NULL;
    popup_confirm_close(&badusb_confirm_popup);

    if (g_ov) {
        options_view_destroy(g_ov);
        g_ov = NULL;
    }

    lvgl_obj_del_safe(&root);
    badusb_view.root = NULL;
    menu_container = NULL;
    gui_touch_bar_destroy(&s_touch_tb);
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    /* selected_item_index/current_menu_state deliberately not reset here --
     * they need to survive the destroy() -> create() cycle so create() can
     * restore them when returning rather than always resetting to root. */
    num_items = 0;
}

static void get_badusb_callback(void **callback) {
    *callback = badusb_view.input_callback;
}

void badusb_view_input_cb(InputEvent *event) {
    if (badusb_confirm_handle_input(event)) {
        return;
    }

    if (badusb_running_popup && lv_obj_is_valid(badusb_running_popup)) {
        if (event->type == INPUT_TYPE_KEYBOARD) {
            uint8_t key = event->data.key_value;
            if (key == 13 || key == 10 || key == 27 || key == 29 || key == 'c' || key == 'C') {
                badusb_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            if (event->data.joystick_index == 0 || event->data.joystick_index == 1) {
                badusb_cancel_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                badusb_cancel_cb(NULL);
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
                touch_drag_reset(&badusb_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(scroll_down_btn, data->point.x, data->point.y)) {
                scroll_down_cb(NULL);
                touch_drag_reset(&badusb_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(back_btn, data->point.x, data->point.y)) {
                go_back();
                touch_drag_reset(&badusb_touch_drag);
                return;
            }
            if (!badusb_touch_drag.started) {
                touch_drag_begin(&badusb_touch_drag, data);
            } else {
                lv_area_t cont_area;
                if (menu_container && lv_obj_is_valid(menu_container)) {
                    lv_obj_get_coords(menu_container, &cont_area);
                    bool started_in_container = (badusb_touch_drag.start_x >= cont_area.x1 && badusb_touch_drag.start_x <= cont_area.x2 &&
                                                 badusb_touch_drag.start_y >= cont_area.y1 && badusb_touch_drag.start_y <= cont_area.y2);
                    if (started_in_container) {
                        touch_drag_update(&badusb_touch_drag, data, menu_container);
                    }
                }
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!badusb_touch_drag.started) return;

            if (!menu_container || !lv_obj_is_valid(menu_container)) {
                touch_drag_reset(&badusb_touch_drag);
                return;
            }

            int thr_x = LV_HOR_RES / BADUSB_SWIPE_THRESHOLD_RATIO;
            int dx = data->point.x - badusb_touch_drag.start_x;

            int saved_start_x = badusb_touch_drag.start_x;
            int saved_start_y = badusb_touch_drag.start_y;
            bool was_dragged = touch_drag_release(&badusb_touch_drag, data);
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

View badusb_view = {
    .root = NULL,
    .create = badusb_view_create,
    .destroy = badusb_view_destroy,
    .input_callback = badusb_view_input_cb,
    .name = "BadUSB",
    .get_hardwareinput_callback = get_badusb_callback
};

#endif // CONFIG_HAS_BADUSB || CONFIG_HAS_BADUSB_REMOTE
