#include "managers/views/lockscreen.h"
#include "gui/menu_catalog.h"
#include "managers/settings_manager.h"
#include "managers/ghostchi_mood.h"
#include "managers/views/main_menu_screen.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "gui/popup.h"
#include "gui/accessibility_fonts.h"
#include "gui/menu_item_style.h"
#include "managers/display_manager.h"
#include "managers/views/options_screen.h"
#include "managers/views/infrared_view.h"
#include "managers/views/nfc_view.h"
#include "managers/views/subghz_view.h"
#include "managers/views/badusb_view.h"
#include "managers/views/ghostscript_runner_view.h"
#include "managers/views/plugin_runner_view.h"
#include "gui/gui_router.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "sdkconfig.h"

#ifdef CONFIG_WITH_SCREEN
extern const lv_img_dsc_t tired_50x50;
extern const lv_img_dsc_t what2_50x50;
extern const lv_img_dsc_t angry_50x50;
extern const lv_img_dsc_t happy_50x50;
extern const lv_img_dsc_t love_50x50;
extern const lv_img_dsc_t evil_50x50;
extern const lv_img_dsc_t sleep_50x50;
extern const lv_img_dsc_t surpised_50x50;
#define LOCKSCREEN_SPRITE(name) (&name)
#else
#define LOCKSCREEN_SPRITE(name) ((const lv_img_dsc_t *)NULL)
#endif

#define MAX_INPUT_LEN 31
#define STORED_PIN_MARKER 0x80
#define STORED_PIN_LEN_MASK 0x7F
#define NUMPAD_COLS 3
#define NUMPAD_ROWS 4
#define NUMPAD_BTNS 12

static const char * const k_numpad_labels[NUMPAD_BTNS] = {
    "1", "2", "3",
    "4", "5", "6",
    "7", "8", "9",
    LV_SYMBOL_BACKSPACE, "0", "OK"
};

static const char k_numpad_chars[NUMPAD_BTNS] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '\b', '0', '\r'
};

typedef enum {
    GHOST_SLEEPING,
    GHOST_TYPING,
    GHOST_ERROR,
    GHOST_UNLOCKED
} GhostState;

static lv_obj_t *s_root;
static lv_obj_t *s_content;
static lv_obj_t *s_ghost;
static lv_obj_t *s_prompt;
static lv_obj_t *s_dots;
static lv_obj_t *s_numpad_cont;
static lv_obj_t *s_numpad_btns[NUMPAD_BTNS];
static lv_timer_t *s_idle_timer;
static lv_timer_t *s_bob_timer;
static lv_timer_t *s_unlock_timer;

static int s_focus_idx;
static bool s_touch_started;
static int s_touch_pressed_idx;
static int s_suppress_click_idx;
static int64_t s_suppress_click_until_ms;
static int64_t s_ignore_input_until_ms;
static char s_input[MAX_INPUT_LEN + 1];
static uint8_t s_input_len;
static bool s_setup_mode;
static bool s_setup_confirm;
static bool s_no_pin_mode;
static bool s_overlay_mode;
static char s_setup_first[MAX_INPUT_LEN + 1];
static GhostState s_ghost_state;
static int s_ghost_base_y;

// Favorites overlay state (responsive, pre-PIN accessible)
static lv_obj_t *s_fav_overlay = NULL;
static lv_obj_t *s_fav_list = NULL;
static lv_obj_t **s_fav_btns = NULL;
static int s_fav_count = 0;
static int s_fav_selected = 0;
static bool s_fav_active = false;
static lv_obj_t *s_fav_pill = NULL;
static lv_obj_t *s_fav_hint = NULL;
static const char *s_pending_fav_launch = NULL;
static bool s_fav_pill_focused = false;
// Status-bar title shown before the Favorites overlay opened (restored on hide).
static char s_fav_saved_status_title[48] = {0};
// Layout tracking so the favorites pill can be placed relative to the
// (dynamic) numpad position instead of a fixed offset.
static int s_numpad_y = 94;
static bool s_landscape_layout = false;

// Extra invisible touch padding around the Favorites pill/hint so the
// touch target is comfortable to hit without growing the visuals. Kept
// at 6px so the portrait pill (sitting 6px above the numpad) never
// overlaps the keypad: 22px pill + 12px padding = a ~34px effective
// target that fills the dead gap between pill and numpad exactly.
#define LS_FAV_TOUCH_PAD 6

#ifdef CONFIG_USE_TOUCHSCREEN
// Standard bottom touch bar for the favorites overlay (scroll up / Back /
// scroll down), matching the other options-style views.
// Geometry now comes from gui/touch_bar.h; the LS_FAV_* defines below are
// kept as aliases (same values).
#define LS_FAV_SCROLL_BTN_SIZE 28
#define LS_FAV_SCROLL_BTN_PADDING 3
#define LS_FAV_TOUCH_BAR_HEIGHT (LS_FAV_SCROLL_BTN_SIZE + LS_FAV_SCROLL_BTN_PADDING * 2)
static gui_touch_bar_t s_fav_tb;
static lv_obj_t *s_fav_touch_bar = NULL;
static lv_obj_t *s_fav_scroll_up_btn = NULL;
static lv_obj_t *s_fav_scroll_down_btn = NULL;
static lv_obj_t *s_fav_touch_back_btn = NULL;
// Live drag-scroll state for the favorites list.
static touch_drag_t s_fav_touch_drag;
#endif

static void lockscreen_show_favorites(void);
static void lockscreen_hide_favorites(void);
static void lockscreen_fav_set_selected(int idx);
static void lockscreen_fav_activate_selected(void);
static bool lockscreen_handle_favorites_input(bool up, bool down, bool left, bool right, bool select, bool back);
static void lockscreen_create_fav_pill(void);
static void lockscreen_destroy_fav_pill(void);
static void lockscreen_fav_launch(const char *name);
#ifdef CONFIG_USE_TOUCHSCREEN
static void lockscreen_fav_update_scroll_buttons(void);
#endif

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
static const char *lockscreen_fav_type(const char *name) {
    if (!name) return "Favorite";
    if (strncasecmp(name, "ir:", 3) == 0) return "IR Remote";
    if (strncasecmp(name, "nfc:", 4) == 0) return "NFC Tag";
    if (strncasecmp(name, "subghz:", 7) == 0) return "SubGHz File";
    if (strncasecmp(name, "app:", 4) == 0) return "App";
    if (strncasecmp(name, "gs:", 3) == 0) return "Script";
    if (strncasecmp(name, "badusb:", 7) == 0) return "Payload";
    return "Main Menu";
}

static const char *lockscreen_fav_display_name(const char *name) {
    if (!name || !name[0]) return "Favorite";
    const char *display = name;
    const char *colon = strchr(name, ':');
    if (colon && colon[1]) display = colon + 1;
    const char *slash = strrchr(display, '/');
    if (slash && slash[1]) display = slash + 1;
    return display;
}
#endif

static void lockscreen_clear_input(void);
static void lockscreen_add_char(char c);
static void lockscreen_delete_last(void);
static void lockscreen_submit(void);
static void lockscreen_on_wrong(void);
static void lockscreen_on_correct(void);
static void lockscreen_update_dots(void);
static void lockscreen_update_ghost(bool immediate);
static void lockscreen_apply_ghost_bob(void);
static void lockscreen_set_prompt(const char *text);
static void lockscreen_build_numpad(void);
static void lockscreen_build_companion_layout(int content_w, int content_h);
static const lv_img_dsc_t *lockscreen_companion_sprite(void);
static void lockscreen_idle_cb(lv_timer_t *timer);
static void lockscreen_bob_cb(lv_timer_t *timer);
static void lockscreen_unlock_cb(lv_timer_t *timer);
static void lockscreen_numpad_cb(lv_event_t *e);
static void lockscreen_normalize_input(InputEvent *event, bool *up, bool *down, bool *left, bool *right, bool *select, bool *back);
static void lockscreen_input_handler(InputEvent *event);
static void lockscreen_focus_btn(int idx);
static void lockscreen_move_focus(int dx, int dy);

static void derive_key(uint8_t *out, size_t len) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        for (size_t i = 0; i < len; i++) {
            out[i] = mac[i % 6] ^ (uint8_t)(0xA5 + i);
        }
    } else {
        for (size_t i = 0; i < len; i++) {
            out[i] = (uint8_t)(0x42 + i);
        }
    }
}

static void derive_legacy_fallback_key(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(0x42 + i);
    }
}

static bool verify_with_key(const char *input, const uint8_t *key, size_t key_len) {
    const uint8_t *stored = (const uint8_t *)G_Settings.lockscreen_obfuscated;
    if ((stored[0] & STORED_PIN_MARKER) == 0) return false;
    size_t stored_len = stored[0] & STORED_PIN_LEN_MASK;
    size_t in_len = strlen(input);
    if (in_len != stored_len) return false;
    for (size_t i = 0; i < in_len; i++) {
        char decrypted = (char)(stored[i + 1] ^ key[i % key_len]);
        if (decrypted != input[i]) return false;
    }
    return true;
}

static bool verify_input(const char *input) {
    uint8_t key[32];
    derive_key(key, sizeof(key));
    if (verify_with_key(input, key, sizeof(key))) {
        return true;
    }

    derive_legacy_fallback_key(key, sizeof(key));
    return verify_with_key(input, key, sizeof(key));
}

static void save_obfuscated(const char *input) {
    uint8_t key[32];
    derive_key(key, sizeof(key));
    size_t len = strlen(input);
    if (len > sizeof(G_Settings.lockscreen_obfuscated) - 1) {
        len = sizeof(G_Settings.lockscreen_obfuscated) - 1;
    }
    memset(G_Settings.lockscreen_obfuscated, 0, sizeof(G_Settings.lockscreen_obfuscated));
    G_Settings.lockscreen_obfuscated[0] = (char)(STORED_PIN_MARKER | len);
    uint8_t *stored = (uint8_t *)G_Settings.lockscreen_obfuscated;
    for (size_t i = 0; i < len; i++) {
        stored[i + 1] = (uint8_t)input[i] ^ key[i % 32];
    }
    settings_persist_setting(SETTING_LOCKSCREEN_CHANGE_PIN);
}

static int lockscreen_ghost_bob_offset(void) {
    static const int8_t sine_tbl[24] = {0, 3, 5, 6, 5, 3, 0, -3, -5, -6, -5, -3,
                                         0, 3, 5, 6, 5, 3, 0, -3, -5, -6, -5, -3};
    uint32_t phase = (uint32_t)((esp_timer_get_time() / 70000ULL) % 24ULL);
    return sine_tbl[phase];
}

static lv_timer_t *s_shake_timer;
static int s_shake_remaining;

static void lockscreen_shake_cb(lv_timer_t *timer) {
    if (!s_dots || !lv_obj_is_valid(s_dots)) {
        lv_timer_del(timer);
        s_shake_timer = NULL;
        return;
    }
    if (s_shake_remaining <= 0) {
        lv_obj_set_x(s_dots, lv_obj_get_x(s_dots) % 2 ? lv_obj_get_x(s_dots) + 1 : lv_obj_get_x(s_dots));
        lv_timer_del(timer);
        s_shake_timer = NULL;
        return;
    }
    int offset = (s_shake_remaining % 2) ? GUI_SHAKE_AMPLITUDE : -GUI_SHAKE_AMPLITUDE;
    lv_obj_set_x(s_dots, lv_obj_get_x(s_dots) + offset);
    s_shake_remaining--;
}

static void lockscreen_start_shake(void) {
    if (s_shake_timer) {
        lv_timer_del(s_shake_timer);
        s_shake_timer = NULL;
    }
    s_shake_remaining = GUI_SHAKE_CYCLES * 2;
    s_shake_timer = lv_timer_create(lockscreen_shake_cb, GUI_SHAKE_PERIOD_MS / 2, NULL);
    lv_timer_set_repeat_count(s_shake_timer, s_shake_remaining + 1);
}

bool lockscreen_is_configured(void) {
    uint8_t stored_len = (uint8_t)G_Settings.lockscreen_obfuscated[0];
    return (stored_len & STORED_PIN_MARKER) != 0 &&
           (stored_len & STORED_PIN_LEN_MASK) > 0 &&
           (stored_len & STORED_PIN_LEN_MASK) <= MAX_INPUT_LEN;
}

void lockscreen_reset_input(void) {
    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0;
    s_setup_mode = false;
    s_setup_confirm = false;
    s_no_pin_mode = false;
    s_overlay_mode = false;
    s_setup_first[0] = '\0';
    s_ghost_state = GHOST_SLEEPING;
    s_focus_idx = 0;
    s_touch_started = false;
    s_touch_pressed_idx = -1;
    s_suppress_click_idx = -1;
    s_suppress_click_until_ms = 0;
    s_ignore_input_until_ms = (esp_timer_get_time() / 1000) + 350;
}

void lockscreen_enter_setup(void) {
    lockscreen_reset_input();
    s_setup_mode = true;
}

void lockscreen_set_overlay_mode(bool on) {
    s_overlay_mode = on;
}

static void lockscreen_clear_input(void) {
    memset(s_input, 0, sizeof(s_input));
    s_input_len = 0;
    lockscreen_update_dots();
}

static void lockscreen_add_char(char c) {
    if (s_input_len >= MAX_INPUT_LEN) return;
    if (c < '0' || c > '9') return;
    s_input[s_input_len++] = c;
    s_input[s_input_len] = '\0';
    s_ghost_state = GHOST_TYPING;
    lockscreen_update_ghost(true);
    lockscreen_update_dots();
    if (s_idle_timer) lv_timer_reset(s_idle_timer);
}

static void lockscreen_delete_last(void) {
    if (s_input_len > 0) {
        s_input[--s_input_len] = '\0';
        s_ghost_state = GHOST_TYPING;
        lockscreen_update_ghost(true);
        lockscreen_update_dots();
        if (s_idle_timer) lv_timer_reset(s_idle_timer);
    }
}

static void lockscreen_set_prompt(const char *text) {
    if (s_prompt && lv_obj_is_valid(s_prompt)) {
        lv_label_set_text(s_prompt, text);
    }
}

static void lockscreen_update_dots(void) {
    if (!s_dots || !lv_obj_is_valid(s_dots)) return;
    char dots[(MAX_INPUT_LEN * 4) + 1];
    int offset = 0;
    for (int i = 0; i < (int)s_input_len && i < MAX_INPUT_LEN; i++) {
        strcpy(dots + offset, LV_SYMBOL_BULLET);
        offset += strlen(LV_SYMBOL_BULLET);
    }
    dots[offset] = '\0';
    if (s_input_len == 0) {
        lv_label_set_text(s_dots, "");
    } else {
        lv_label_set_text(s_dots, dots);
    }
}

static void lockscreen_update_ghost(bool immediate) {
    if (!s_ghost || !lv_obj_is_valid(s_ghost)) return;
    const lv_img_dsc_t *src = LOCKSCREEN_SPRITE(tired_50x50);
    switch (s_ghost_state) {
        case GHOST_SLEEPING:  src = s_no_pin_mode ? lockscreen_companion_sprite() : LOCKSCREEN_SPRITE(tired_50x50); break;
        case GHOST_TYPING:    src = LOCKSCREEN_SPRITE(what2_50x50); break;
        case GHOST_ERROR:     src = LOCKSCREEN_SPRITE(angry_50x50); break;
        case GHOST_UNLOCKED:  src = LOCKSCREEN_SPRITE(happy_50x50); break;
    }
    lv_img_set_src(s_ghost, src);
    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    if (!s_no_pin_mode && LV_HOR_RES > LV_VER_RES && content_h <= 146) {
        int ghost_sz = 60;
        lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);
    }
    lockscreen_apply_ghost_bob();
    (void)immediate;
}

static void lockscreen_apply_ghost_bob(void) {
    if (!s_ghost || !lv_obj_is_valid(s_ghost)) return;
    lv_obj_set_y(s_ghost, s_ghost_base_y + lockscreen_ghost_bob_offset());
}

static void lockscreen_idle_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_ghost_state == GHOST_TYPING || s_ghost_state == GHOST_ERROR) {
        s_ghost_state = GHOST_SLEEPING;
        lockscreen_update_ghost(true);
    }
}

static void lockscreen_bob_cb(lv_timer_t *timer) {
    (void)timer;
    lockscreen_apply_ghost_bob();
}

static void lockscreen_unlock_cb(lv_timer_t *timer) {
    s_unlock_timer = NULL;
    lv_timer_del(timer);

    /* Overlay mode: the view underneath was never destroyed and is still
     * running (e.g. wardriving). Just tear down the floating lockscreen and
     * hand input back to it — no view switch, no capture restart. */
    if (s_overlay_mode) {
        lockscreen_destroy();
        display_manager_clear_lockscreen_overlay();
        return;
    }

    View *return_view = display_manager_get_lockscreen_return_view();
    if (return_view == NULL || return_view == &lockscreen_view) {
        return_view = &main_menu_view;
    }
    display_manager_clear_lockscreen_return_view();
    display_manager_switch_view(return_view);
}

static void lockscreen_on_wrong(void) {
    s_ghost_state = GHOST_ERROR;
    lockscreen_update_ghost(true);
    lockscreen_set_prompt("Wrong PIN");
    lockscreen_start_shake();
    lockscreen_clear_input();
}

static void lockscreen_on_correct(void) {
    s_ghost_state = GHOST_UNLOCKED;
    lockscreen_update_ghost(true);
    // If a favorite was pending behind PIN, launch it directly after unlock
    if (s_pending_fav_launch && s_pending_fav_launch[0]) {
        char pending_copy[FAVORITE_NAME_LEN];
        strncpy(pending_copy, s_pending_fav_launch, FAVORITE_NAME_LEN - 1);
        pending_copy[FAVORITE_NAME_LEN - 1] = '\0';
        s_pending_fav_launch = NULL;
        lockscreen_set_prompt("Unlocked!");
        if (s_unlock_timer) { lv_timer_del(s_unlock_timer); s_unlock_timer = NULL; }
        // Bypass normal unlock timer to launch favorite immediately
        bool is_overlay = s_overlay_mode;
        View *return_view = display_manager_get_lockscreen_return_view();
        if (is_overlay) {
            lockscreen_destroy();
            display_manager_clear_lockscreen_overlay();
        } else {
            View *ret = return_view && return_view != &lockscreen_view ? return_view : NULL;
            lockscreen_destroy();
            display_manager_clear_lockscreen_return_view();
            if (ret) display_manager_switch_view(ret);
        }
        lockscreen_fav_launch(pending_copy);
        return;
    }
    lockscreen_set_prompt("Unlocked!");
    if (s_unlock_timer) {
        lv_timer_del(s_unlock_timer);
        s_unlock_timer = NULL;
    }
    s_unlock_timer = lv_timer_create(lockscreen_unlock_cb, 600, NULL);
}

static void lockscreen_submit(void) {
    if (s_setup_mode) {
        if (s_input_len == 0) {
            save_obfuscated("");
            s_setup_mode = false;
            s_setup_confirm = false;
            s_setup_first[0] = '\0';
            lockscreen_set_prompt("No PIN saved");
            lockscreen_on_correct();
            return;
        }

        if (!s_setup_confirm) {
            // First entry
            strncpy(s_setup_first, s_input, sizeof(s_setup_first) - 1);
            s_setup_first[sizeof(s_setup_first) - 1] = '\0';
            s_setup_confirm = true;
            lockscreen_clear_input();
            lockscreen_set_prompt("Confirm PIN");
            return;
        } else {
            // Confirmation
            if (strcmp(s_input, s_setup_first) == 0) {
                save_obfuscated(s_input);
                s_setup_mode = false;
                lockscreen_set_prompt("Saved!");
                lockscreen_on_correct();
                return;
            } else {
                s_setup_confirm = false;
                s_setup_first[0] = '\0';
                lockscreen_clear_input();
                lockscreen_set_prompt("Mismatch! Set again");
                s_ghost_state = GHOST_ERROR;
                lockscreen_update_ghost(true);
                if (s_idle_timer) lv_timer_reset(s_idle_timer);
                return;
            }
        }
    }

    if (s_no_pin_mode) {
        lockscreen_on_correct();
        return;
    }

    if (s_input_len == 0) return;

    if (verify_input(s_input)) {
        lockscreen_on_correct();
    } else {
        lockscreen_on_wrong();
    }
}

static int lockscreen_hit_test(int x, int y) {
    for (int i = 0; i < NUMPAD_BTNS; i++) {
        if (!s_numpad_btns[i] || !lv_obj_is_valid(s_numpad_btns[i])) continue;
        lv_area_t area;
        lv_obj_get_coords(s_numpad_btns[i], &area);
        if (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2) {
            return i;
        }
    }
    return -1;
}

static void lockscreen_focus_btn(int idx) {
    if (idx < 0 || idx >= NUMPAD_BTNS) return;
    if (!s_numpad_cont || !lv_obj_is_valid(s_numpad_cont)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t focus_text = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
    for (int i = 0; i < NUMPAD_BTNS; i++) {
        if (!s_numpad_btns[i] || !lv_obj_is_valid(s_numpad_btns[i])) continue;
        bool focused = (i == idx);
        lv_obj_set_style_border_width(s_numpad_btns[i], focused ? 3 : 1, 0);
        lv_obj_set_style_border_color(s_numpad_btns[i], focused ? accent : lv_color_hex(0x444444), 0);
        lv_obj_set_style_bg_color(s_numpad_btns[i], focused ? accent : lv_color_hex(0x222222), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_numpad_btns[i], 0);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, focused ? focus_text : lv_color_hex(0xFFFFFF), 0);
        }
    }
}

static void lockscreen_move_focus(int dx, int dy) {
    int col = s_focus_idx % NUMPAD_COLS;
    int row = s_focus_idx / NUMPAD_COLS;
    col += dx;
    row += dy;
    if (col < 0) col = NUMPAD_COLS - 1;
    if (col >= NUMPAD_COLS) col = 0;
    if (row < 0) row = NUMPAD_ROWS - 1;
    if (row >= NUMPAD_ROWS) row = 0;
    s_focus_idx = row * NUMPAD_COLS + col;
    lockscreen_focus_btn(s_focus_idx);
}

static void lockscreen_numpad_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (idx == s_suppress_click_idx && now_ms < s_suppress_click_until_ms) {
        s_suppress_click_idx = -1;
        return;
    }
    if (idx < 0 || idx >= NUMPAD_BTNS) return;
    s_focus_idx = idx;
    lockscreen_focus_btn(s_focus_idx);
    char c = k_numpad_chars[idx];
    if (c == '\b') {
        lockscreen_delete_last();
    } else if (c == '\r') {
        lockscreen_submit();
    } else {
        lockscreen_add_char(c);
    }
}

static void lockscreen_build_numpad(void) {
    if (!s_content || !lv_obj_is_valid(s_content)) return;
    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        lv_obj_del(s_numpad_cont);
    }
    s_numpad_cont = lv_obj_create(s_content);
    lv_obj_set_style_bg_opa(s_numpad_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_numpad_cont, 0, 0);
    lv_obj_set_style_pad_all(s_numpad_cont, 0, 0);
    lv_obj_clear_flag(s_numpad_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_numpad_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_numpad_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    int content_w = lv_obj_get_width(s_content);
    if (content_w <= 0) content_w = LV_HOR_RES;
    bool landscape = (content_w > content_h && content_h <= 146);

    int gap = 2;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    gap = 8;
#endif
    lv_obj_set_style_pad_row(s_numpad_cont, gap, 0);
    lv_obj_set_style_pad_column(s_numpad_cont, gap, 0);
    int btn_h, btn_w, numpad_x, numpad_y;

    if (landscape) {
        int left_col_w = content_w / 2;
        int numpad_area_x = left_col_w;
        int numpad_area_w = content_w - numpad_area_x;
        numpad_y = 0;
        int numpad_h = content_h;
        btn_h = (numpad_h - (NUMPAD_ROWS + 1) * gap) / NUMPAD_ROWS;
        if (btn_h > 30) btn_h = 30;
        if (btn_h < 14) btn_h = 14;
        btn_w = btn_h;
        int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
        numpad_x = numpad_area_x + (numpad_area_w - grid_w) / 2;
        if (numpad_x < numpad_area_x) numpad_x = numpad_area_x;
    } else {
        int min_numpad_y = 94;
        int bottom_margin = 10;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        min_numpad_y = content_h / 4;
        bottom_margin = 20;
#endif
        int numpad_h = content_h - min_numpad_y - bottom_margin;
        if (numpad_h < 36) numpad_h = 36;
        btn_h = (numpad_h - (NUMPAD_ROWS - 1) * gap) / NUMPAD_ROWS;
        if (LV_VER_RES > 240) {
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
            if (btn_h > GUI_CONTROL_H) btn_h = GUI_CONTROL_H;
#else
            if (btn_h > 42) btn_h = 42;
#endif
        } else {
            if (btn_h > 30) btn_h = 30;
        }
        if (btn_h < 14) btn_h = 14;
        btn_w = btn_h;
        int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * gap;
        numpad_y = content_h - grid_h - bottom_margin;
        if (numpad_y < min_numpad_y) numpad_y = min_numpad_y;
        int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
        numpad_x = (content_w - grid_w) / 2;
        if (numpad_x < 0) numpad_x = 0;
    }

    int grid_w = NUMPAD_COLS * btn_w + (NUMPAD_COLS - 1) * gap;
    int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * gap;
    int grid_y_offset = landscape ? ((content_h - grid_h) / 2) : 0;

    lv_obj_set_size(s_numpad_cont, grid_w, grid_h);
    lv_obj_set_pos(s_numpad_cont, numpad_x, numpad_y + grid_y_offset);
    s_numpad_y = numpad_y;
    s_landscape_layout = landscape;

    for (int i = 0; i < NUMPAD_BTNS; i++) {
        s_numpad_btns[i] = lv_btn_create(s_numpad_cont);
        gui_apply_pressed_style(s_numpad_btns[i]);
        lv_obj_set_size(s_numpad_btns[i], btn_w, btn_h);
        lv_obj_add_event_cb(s_numpad_btns[i], lockscreen_numpad_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_set_style_bg_color(s_numpad_btns[i], lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(s_numpad_btns[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(s_numpad_btns[i], GUI_RADIUS_SM / 2, 0);
        lv_obj_set_style_border_width(s_numpad_btns[i], 1, 0);
        lv_obj_set_style_border_color(s_numpad_btns[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_shadow_width(s_numpad_btns[i], 0, 0);
        lv_obj_set_style_shadow_color(s_numpad_btns[i], lv_color_hex(0x000000), 0);
        lv_obj_set_style_shadow_opa(s_numpad_btns[i], LV_OPA_TRANSP, 0);
        lv_obj_t *lbl = lv_label_create(s_numpad_btns[i]);
        lv_label_set_text(lbl, k_numpad_labels[i]);
        const lv_font_t *f;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        f = btn_h >= 56 ? gui_font_title() : gui_font_body();
#else
        f = (btn_h < 20) ? &lv_font_montserrat_10 : (btn_h >= 36 ? &lv_font_montserrat_16 : &lv_font_montserrat_12);
#endif
        lv_obj_set_style_text_font(lbl, f, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
    }
    s_focus_idx = 0;
    lockscreen_focus_btn(0);
}

static void lockscreen_build_companion_layout(int content_w, int content_h) {
    const int ghost_base_sz = 50;
    int ghost_sz = 72;
    if (content_h <= 120) {
        ghost_sz = 64;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    } else if (content_h >= 480) {
        ghost_sz = 160;
    } else if (content_h >= 320) {
        ghost_sz = 128;
#endif
    } else if (content_h >= 220) {
        ghost_sz = 96;
    }
    if (ghost_sz > content_w - 16) ghost_sz = content_w - 16;
    if (ghost_sz < 50) ghost_sz = 50;

    // Reserve room at the bottom for the '▼ Favorites' hint so the
    // ghost + "Press to unlock" group never overlaps it.
    int bottom_reserve = 18;
    int usable_h = content_h - bottom_reserve;
    if (usable_h < 60) usable_h = content_h;

    int ghost_y = (usable_h - ghost_base_sz) / 2;
    if (ghost_y < 0) ghost_y = 0;
    s_ghost_base_y = ghost_y;

    s_ghost = lv_img_create(s_content);
    lv_img_set_src(s_ghost, lockscreen_companion_sprite());
    lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);
    lv_obj_set_pos(s_ghost, (content_w - ghost_base_sz) / 2, s_ghost_base_y + lockscreen_ghost_bob_offset());

    s_prompt = lv_label_create(s_content);
    lv_obj_set_style_text_font(s_prompt, gui_font_caption(), 0);
    lv_obj_set_style_text_color(s_prompt, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_prompt, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_prompt, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_prompt, 3, 0);
    lv_obj_set_style_pad_hor(s_prompt, 6, 0);
    lv_obj_set_style_pad_ver(s_prompt, 1, 0);
    int prompt_y = ghost_y + ghost_base_sz + ((ghost_sz - ghost_base_sz) / 2) + 10;
    if (prompt_y > usable_h - 18) prompt_y = usable_h - 18;
    if (prompt_y < 0) prompt_y = 0;
    lv_obj_align(s_prompt, LV_ALIGN_TOP_MID, 0, prompt_y);

    s_dots = NULL;
}

static const lv_img_dsc_t *lockscreen_companion_sprite(void) {
#ifdef CONFIG_WITH_SCREEN
    ghostchi_mood_snapshot_t mood = {0};
    ghostchi_mood_get_snapshot(&mood);
    switch (mood.mood) {
        case GHOSTCHI_MOOD_CELEBRATE:
        case GHOSTCHI_MOOD_LOVE:
            return &love_50x50;
        case GHOSTCHI_MOOD_HAPPY:
        case GHOSTCHI_MOOD_EXCITED:
            return &happy_50x50;
        case GHOSTCHI_MOOD_FOCUSED:
        case GHOSTCHI_MOOD_SURPRISED:
            return &surpised_50x50;
        case GHOSTCHI_MOOD_AGGRESSIVE:
            return &evil_50x50;
        case GHOSTCHI_MOOD_ANGRY:
            return &angry_50x50;
        case GHOSTCHI_MOOD_TIRED:
            return &tired_50x50;
        case GHOSTCHI_MOOD_SLEEPY:
            return &sleep_50x50;
        case GHOSTCHI_MOOD_CONFUSED:
            return &what2_50x50;
        case GHOSTCHI_MOOD_NEUTRAL:
        default:
            return &love_50x50;
    }
#else
    return NULL;
#endif
}

static void lockscreen_destroy_numpad(void) {
    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        lv_obj_del(s_numpad_cont);
        s_numpad_cont = NULL;
    }
    for (int i = 0; i < NUMPAD_BTNS; i++) s_numpad_btns[i] = NULL;
}

// --- Favorites overlay (responsive, theme-consistent) ---

// Selection domain is [0 .. s_fav_count]; index s_fav_count is the Back row.
// A joystick can therefore navigate to "Back" and press SELECT to leave the
// overlay, exactly like the other options-style lists.
static void lockscreen_fav_set_selected(int idx) {
    if (!s_fav_btns) return;
    if (idx < 0) idx = s_fav_count;
    if (idx > s_fav_count) idx = 0;
    s_fav_selected = idx;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    for (int i = 0; i <= s_fav_count; i++) {
        lv_obj_t *btn = s_fav_btns[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        bool sel = (i == s_fav_selected);
        if (sel) {
            // Accent fill + contrast text, matching the numpad focus style.
            lv_obj_set_style_bg_color(btn, accent, 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_border_color(btn, accent, 0);
            lv_obj_set_style_shadow_width(btn, 0, 0);
            lv_obj_set_style_shadow_opa(btn, LV_OPA_TRANSP, 0);
        } else {
            gui_menu_card_apply(btn, true, surface, surface, 0, 0);
        }
        lv_color_t selected_text = theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
        uint32_t child_count = lv_obj_get_child_cnt(btn);
        for (uint32_t child_idx = 0; child_idx < child_count; child_idx++) {
            lv_obj_t *child = lv_obj_get_child(btn, (int32_t)child_idx);
            if (child) lv_obj_set_style_text_color(child, sel ? selected_text : text, 0);
        }
    }
    if (s_fav_btns[s_fav_selected] && lv_obj_is_valid(s_fav_btns[s_fav_selected])) {
        lv_obj_scroll_to_view(s_fav_btns[s_fav_selected], LV_ANIM_ON);
    }
}

static void lockscreen_fav_activate_selected(void) {
    // The last row is Back: pressing SELECT on it closes the overlay.
    if (s_fav_count <= 0 || s_fav_selected < 0 || s_fav_selected >= s_fav_count) {
        lockscreen_hide_favorites();
        return;
    }
    const char *name = settings_get_favorite(&G_Settings, s_fav_selected);
    if (!name || !name[0]) { lockscreen_hide_favorites(); return; }
    // Save pending launch before hiding overlay (overlay destroy clears state)
    static char pending[FAVORITE_NAME_LEN];
    strncpy(pending, name, FAVORITE_NAME_LEN - 1);
    pending[FAVORITE_NAME_LEN - 1] = '\0';
    s_pending_fav_launch = pending;
    bool need_pin = lockscreen_is_configured() && !settings_get_favorites_bypass(&G_Settings) && !s_no_pin_mode;
    lockscreen_hide_favorites();
    if (!need_pin) {
        lockscreen_fav_launch(pending);
        s_pending_fav_launch = NULL;
    } else {
        // Stay on lockscreen; next correct PIN will auto-launch pending favorite
        lockscreen_set_prompt("Enter PIN for Favorite");
    }
}

static bool lockscreen_handle_favorites_input(bool up, bool down, bool left, bool right, bool select, bool back) {
    if (!s_fav_active) return false;
    (void)left; (void)right;
    if (up) { lockscreen_fav_set_selected(s_fav_selected - 1);
#ifdef CONFIG_USE_TOUCHSCREEN
        lockscreen_fav_update_scroll_buttons();
#endif
    }
    else if (down) { lockscreen_fav_set_selected(s_fav_selected + 1);
#ifdef CONFIG_USE_TOUCHSCREEN
        lockscreen_fav_update_scroll_buttons();
#endif
    }
    else if (select) lockscreen_fav_activate_selected();
    else if (back) lockscreen_hide_favorites();
    else return false;
    return true;
}

static void lockscreen_fav_btn_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_fav_selected = idx;
    lockscreen_fav_set_selected(idx);
    lockscreen_fav_activate_selected();
}

#ifdef CONFIG_USE_TOUCHSCREEN
static void lockscreen_fav_update_scroll_buttons(void) {
    gui_touch_bar_update_visibility(&s_fav_tb, s_fav_list);
}

static void lockscreen_fav_scroll_up(void) {
    if (!s_fav_list || !lv_obj_is_valid(s_fav_list)) return;
    lv_coord_t amt = lv_obj_get_height(s_fav_list) / 2;
    lv_obj_scroll_by_bounded(s_fav_list, 0, amt, LV_ANIM_OFF);
    lockscreen_fav_update_scroll_buttons();
}

static void lockscreen_fav_scroll_down(void) {
    if (!s_fav_list || !lv_obj_is_valid(s_fav_list)) return;
    lv_coord_t amt = lv_obj_get_height(s_fav_list) / 2;
    lv_obj_scroll_by_bounded(s_fav_list, 0, -amt, LV_ANIM_OFF);
    lockscreen_fav_update_scroll_buttons();
}

static void lockscreen_fav_scroll_up_cb(lv_event_t *e) {
    (void)e;
    lockscreen_fav_scroll_up();
}

static void lockscreen_fav_scroll_down_cb(lv_event_t *e) {
    (void)e;
    lockscreen_fav_scroll_down();
}

static void lockscreen_fav_touch_back_cb(lv_event_t *e) {
    (void)e;
    lockscreen_hide_favorites();
}
#endif

static void lockscreen_show_favorites(void) {
    if (s_fav_active) return;
    s_fav_active = true;
    s_fav_selected = 0;
    s_fav_count = settings_get_favorites_count(&G_Settings);
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&s_fav_touch_drag);
#endif
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
#endif
    // Reserve the touch bar height only when the bar is actually shown.
    // gui_touch_bar_height() returns 0 on large-screen targets where the bar
    // is hidden, so the list no longer leaves a 34px gap there.
    const int touch_h = gui_touch_bar_height();
    // Overlay on lv_layer_top above lockscreen: one flat background color.
    s_fav_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_fav_overlay, LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_H);
    lv_obj_align(s_fav_overlay, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);
    lv_obj_set_style_bg_color(s_fav_overlay, bg, 0);
    lv_obj_set_style_bg_opa(s_fav_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fav_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_fav_overlay, 0, 0);
    lv_obj_clear_flag(s_fav_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_fav_overlay);
    // No in-overlay header: the status bar carries the "Favorites" title.
    // Save whatever it showed so hide can restore it.
    {
        const char *cur = display_manager_get_status_title();
        if (cur && cur[0]) {
            strncpy(s_fav_saved_status_title, cur, sizeof(s_fav_saved_status_title) - 1);
            s_fav_saved_status_title[sizeof(s_fav_saved_status_title) - 1] = '\0';
        } else {
            s_fav_saved_status_title[0] = '\0';
        }
        display_manager_add_status_bar("Favorites");
    }
    // Integrated full-width list: no card chrome, same background as the view.
    // No top header: the status bar above the overlay carries the title, so
    // the list starts flush at the top of the overlay (only 8px bottom margin
    // before the touch bar).
    int margin = (LV_HOR_RES <= 160) ? 6 : 10;
    int list_w = LV_HOR_RES - margin * 2;
    int list_h = LV_VER_RES - GUI_STATUS_BAR_H - 8 - touch_h;
    if (list_h < 60) list_h = 60;
    s_fav_list = lv_obj_create(s_fav_overlay);
    lv_obj_set_size(s_fav_list, list_w, list_h);
    lv_obj_align(s_fav_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(s_fav_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_fav_list, 0, 0);
    lv_obj_set_style_radius(s_fav_list, 0, 0);
    lv_obj_set_style_pad_all(s_fav_list, 2, 0);
    lv_obj_set_style_pad_row(s_fav_list, 3, 0);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    // Landscape P4 panels have enough width for a touch-friendly launcher
    // grid. Keep the cards bounded so the 1024px panels do not become a
    // stretched single-column list.
    int fav_columns = (LV_HOR_RES >= 960) ? 4 : 2;
    int fav_gap = GUI_GRID;
    int fav_card_w = (list_w - 4 - (fav_columns - 1) * fav_gap) / fav_columns;
    lv_obj_set_style_pad_row(s_fav_list, fav_gap, 0);
    lv_obj_set_style_pad_column(s_fav_list, fav_gap, 0);
    lv_obj_set_style_pad_left(s_fav_list, 2, 0);
    lv_obj_set_style_pad_right(s_fav_list, 2, 0);
    lv_obj_set_style_pad_top(s_fav_list, 4, 0);
    lv_obj_set_style_pad_bottom(s_fav_list, 4, 0);
    lv_obj_set_flex_flow(s_fav_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_fav_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
#else
    lv_obj_set_flex_flow(s_fav_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_fav_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#endif
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    // Empty state
    if (s_fav_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_fav_list);
        lv_label_set_text(lbl, "No favorites yet\nSettings > Favorites > Manage\nAdd from Main Menu, IR, NFC,\nSubGHz or Apps");
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, list_w - 16);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, gui_font_caption(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(theme_palette_get_text_muted(theme)), 0);
        lv_obj_center(lbl);
    }
    // One extra slot for the Back row.
    s_fav_btns = (lv_obj_t **)malloc(sizeof(lv_obj_t *) * (s_fav_count + 1));
    if (s_fav_btns) {
        for (int i = 0; i < s_fav_count; i++) {
        const char *name = settings_get_favorite(&G_Settings, i);
#ifndef CONFIG_CROWPANEL_ADVANCED_P4
        const char *display = name ? name : "";
        // Strip prefix for display: "ir:/path" -> basename, "menu:WiFi" -> "WiFi"
        const char *colon = name ? strchr(name, ':') : NULL;
        if (colon && colon[1]) {
            if (strncasecmp(name, "ir:", 3)==0 || strncasecmp(name, "nfc:", 4)==0 || strncasecmp(name, "subghz:", 7)==0 || strncasecmp(name, "app:",4)==0 || strncasecmp(name, "menu:",5)==0 || strncasecmp(name, "gs:", 3)==0 || strncasecmp(name, "badusb:", 7)==0) {
                display = colon + 1;
                // For file paths, show basename only
                const char *slash = strrchr(display, '/');
                if (slash && slash[1]) display = slash + 1;
            }
        }
        if (!display || !display[0]) display = name ? name : "";
#endif
        lv_obj_t *btn = lv_btn_create(s_fav_list);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        lv_obj_set_width(btn, fav_card_w);
        lv_obj_set_height(btn, 112);
        lv_obj_set_style_pad_all(btn, 12, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
#else
        lv_obj_set_width(btn, list_w - 4);
        int row_h = (LV_VER_RES <= 160 || LV_HOR_RES <= 160) ? 26 : 34;
        lv_obj_set_height(btn, row_h);
#endif
        lv_obj_set_style_radius(btn, GUI_RADIUS_SM, 0);
        gui_apply_pressed_style(btn);
        // Flat options-style row: plain surface, no border chrome.
        gui_menu_card_apply(btn, true, surface, surface, 0, 0);
        lv_obj_t *lbl = lv_label_create(btn);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        lv_label_set_text(lbl, lockscreen_fav_display_name(name));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, fav_card_w - 24);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, gui_font_body(), 0);
        lv_obj_set_style_text_color(lbl, text, 0);

        lv_obj_t *type_lbl = lv_label_create(btn);
        lv_label_set_text(type_lbl, lockscreen_fav_type(name));
        lv_label_set_long_mode(type_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(type_lbl, fav_card_w - 24);
        lv_obj_set_style_text_align(type_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(type_lbl, gui_font_caption(), 0);
        lv_obj_set_style_text_color(type_lbl, lv_color_hex(theme_palette_get_text_muted(theme)), 0);

        lv_obj_t *open_lbl = lv_label_create(btn);
        lv_label_set_text(open_lbl, LV_SYMBOL_PLAY "  Tap to launch");
        lv_obj_set_width(open_lbl, fav_card_w - 24);
        lv_obj_set_style_text_align(open_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(open_lbl, gui_font_micro(), 0);
        lv_obj_set_style_text_color(open_lbl, lv_color_hex(theme_palette_get_accent(theme)), 0);
#else
        lv_label_set_text(lbl, display);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, list_w - 24);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_body(), 0);
        lv_obj_center(lbl);
#endif
        lv_obj_add_event_cb(btn, lockscreen_fav_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_fav_btns[i] = btn;
        }

        // Back row: identical styling to the favorite rows so joystick users
        // can scroll to it and press SELECT to leave the overlay.
        lv_obj_t *back_btn = lv_btn_create(s_fav_list);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        lv_obj_set_width(back_btn, list_w - 4);
        lv_obj_set_height(back_btn, 56);
        lv_obj_set_style_pad_all(back_btn, 8, 0);
#else
        lv_obj_set_width(back_btn, list_w - 4);
        int back_h = (LV_VER_RES <= 160 || LV_HOR_RES <= 160) ? 26 : 34;
        lv_obj_set_height(back_btn, back_h);
#endif
        lv_obj_set_style_radius(back_btn, GUI_RADIUS_SM, 0);
        gui_apply_pressed_style(back_btn);
        gui_menu_card_apply(back_btn, true, surface, surface, 0, 0);
        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
        lv_label_set_long_mode(back_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(back_lbl, list_w - 24);
        lv_obj_set_style_text_align(back_lbl, LV_TEXT_ALIGN_CENTER, 0);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        lv_obj_set_style_text_font(back_lbl, gui_font_body(), 0);
#else
        lv_obj_set_style_text_font(back_lbl, accessibility_get_font_body(), 0);
#endif
        lv_obj_center(back_lbl);
        lv_obj_add_event_cb(back_btn, lockscreen_fav_btn_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)s_fav_count);
        s_fav_btns[s_fav_count] = back_btn;

        lockscreen_fav_set_selected(0);
    }
#ifdef CONFIG_USE_TOUCHSCREEN
    // Standard bottom touch bar (scroll up / Back / scroll down) via helper.
    // Gating is handled inside gui_touch_bar_create().
    s_fav_tb = gui_touch_bar_create(s_fav_overlay);
    s_fav_touch_bar = s_fav_tb.bar;
    s_fav_scroll_up_btn = s_fav_tb.up_btn;
    s_fav_touch_back_btn = s_fav_tb.back_btn;
    s_fav_scroll_down_btn = s_fav_tb.down_btn;
    gui_touch_bar_set_callbacks(&s_fav_tb,
                                lockscreen_fav_scroll_up_cb, NULL,
                                lockscreen_fav_touch_back_cb, NULL,
                                lockscreen_fav_scroll_down_cb, NULL);

    lockscreen_fav_update_scroll_buttons();
#endif
}

static void lockscreen_hide_favorites(void) {
    if (!s_fav_active) return;
    s_fav_active = false;
    if (s_fav_btns) { free(s_fav_btns); s_fav_btns = NULL; }
    s_fav_count = 0;
    s_fav_selected = 0;
#ifdef CONFIG_USE_TOUCHSCREEN
    gui_touch_bar_destroy(&s_fav_tb);
    s_fav_touch_bar = NULL;
    s_fav_scroll_up_btn = NULL;
    s_fav_scroll_down_btn = NULL;
    s_fav_touch_back_btn = NULL;
    touch_drag_reset(&s_fav_touch_drag);
#endif
    // Swallow the tail of a double-tap: the second tap of a quick double-tap
    // on Back/Close must not fall through onto the freshly exposed lockscreen
    // (in Ghostchi mode any stray tap would unlock the device).
    s_ignore_input_until_ms = (esp_timer_get_time() / 1000) + 350;
    // Restore whatever the status bar showed before the overlay opened.
    if (s_fav_saved_status_title[0]) {
        display_manager_add_status_bar(s_fav_saved_status_title);
        s_fav_saved_status_title[0] = '\0';
    } else {
        display_manager_add_status_bar(s_no_pin_mode ? "Ghostchi" : "Locked");
    }
    lvgl_obj_del_safe(&s_fav_overlay);
    s_fav_list = NULL;
}

static void lockscreen_fav_launch(const char *name) {
    if (!name || !name[0]) return;
    // Map favorite name to view/actions mirroring main_menu_screen.c:1127
    // This keeps favorites responsive without depending on main_menu internals.
    extern View options_menu_view;
    extern View lockscreen_view;
    #ifdef CONFIG_HAS_NFC
    extern View nfc_view;
    #endif
    #if CONFIG_HAS_INFRARED
    extern View infrared_view;
    #endif
    #if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    extern View subghz_view;
    #endif
    #if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    extern View badusb_view;
    #endif
    extern View apps_menu_view;
    extern View ethernet_screen_view;
    #ifdef CONFIG_HAS_AUDIO_PLAYER
    extern View audio_player_view;
    #endif
    // Helper to switch: unlock first if needed, then switch
    bool is_overlay = s_overlay_mode;
    // If favorite is Lock, just stay locked
    if (strcmp(name, "Lock") == 0) return;
    // Generic prefix handling for file-based favorites (IR, NFC, SubGHz, Apps)
    View *target = NULL;
    EOptionsMenuType opt_type = OT_Settings;
    bool is_options = false;
    if (strncasecmp(name, "ir:", 3) == 0) {
#if CONFIG_HAS_INFRARED
        target = &infrared_view;
        infrared_view_open_remote(name + 3); // deep-link into the remote
#else
        target = &options_menu_view; opt_type = OT_Settings; is_options = true;
#endif
    } else if (strncasecmp(name, "nfc:", 4) == 0) {
#ifdef CONFIG_HAS_NFC
        target = &nfc_view;
        nfc_view_open_saved(name + 4); // deep-link into the saved tag
#else
        target = &options_menu_view; opt_type = OT_Settings; is_options = true;
#endif
    } else if (strncasecmp(name, "subghz:", 7) == 0) {
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
        target = &subghz_view;
        subghz_view_open_capture(name + 7); // deep-link into the capture
#else
        target = &options_menu_view; opt_type = OT_SubGhz; is_options = true;
#endif
    } else if (strncasecmp(name, "app:", 4) == 0) {
        plugin_runner_set_app(name + 4); // launch the app itself
        target = &plugin_runner_view;
    } else if (strncasecmp(name, "gs:", 3) == 0) {
        // GhostScript: run it in the runner view.
        ghostscript_runner_set_script(name + 3);
        target = &ghostscript_runner_view;
    } else if (strncasecmp(name, "badusb:", 7) == 0) {
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
        // Payload script: the view addresses payloads by bare name.
        const char *slash = strrchr(name + 7, '/');
        badusb_view_open_script(slash ? slash + 1 : name + 7);
        target = &badusb_view;
#else
        // Board without BadUSB support: fall back like the other file types.
        target = &options_menu_view; opt_type = OT_Settings; is_options = true;
#endif
    } else if (strncasecmp(name, "menu:", 5) == 0) {
        const char *m = name + 5;
        if (strcasecmp(m, "WiFi")==0) { target = &options_menu_view; opt_type = OT_Wifi; is_options = true; }
        else if (strcasecmp(m, "BLE")==0) { target = &options_menu_view; opt_type = OT_Bluetooth; is_options = true; }
        else if (strcasecmp(m, "GPS")==0) { target = &options_menu_view; opt_type = OT_GPS; is_options = true; }
        else target = &options_menu_view;
    } else if (strcmp(name, "WiFi") == 0) { target = &options_menu_view; opt_type = OT_Wifi; is_options = true; }
    else if (strcmp(name, "BLE") == 0) { target = &options_menu_view; opt_type = OT_Bluetooth; is_options = true; }
    else if (strcmp(name, "GPS") == 0) { target = &options_menu_view; opt_type = OT_GPS; is_options = true; }
    else if (strcmp(name, "Infrared") == 0) {
#if CONFIG_HAS_INFRARED
        target = &infrared_view;
#endif
    } else if (strcmp(name, "NFC") == 0) {
#ifdef CONFIG_HAS_NFC
        target = &nfc_view;
#endif
    } else if (strcmp(name, "NRF24") == 0) { target = &options_menu_view; opt_type = OT_NRF24; is_options = true; }
    else if (strcmp(name, "SubGHz") == 0) {
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
        target = &subghz_view;
#else
        target = &options_menu_view; opt_type = OT_SubGhz; is_options = true;
#endif
    } else if (strcmp(name, "BadUSB") == 0) {
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
        target = &badusb_view;
#endif
    } else if (strcmp(name, "GhostLink") == 0) { target = &options_menu_view; opt_type = OT_DualComm; is_options = true; }
    else if (strcmp(name, "Ethernet") == 0) target = &ethernet_screen_view;
    else if (strcmp(name, "Apps") == 0) target = &apps_menu_view;
    else if (strcmp(name, "Settings") == 0) { target = &options_menu_view; opt_type = OT_Settings; is_options = true; }
    else if (strcmp(name, "Terminal") == 0) target = &terminal_view;
    else if (strcmp(name, "Audio") == 0) {
#ifdef CONFIG_HAS_AUDIO_PLAYER
        target = &audio_player_view;
#endif
    }
    /* Built-ins moved from Apps to Main retain the existing favorite shortcut.
     * Do not change overlay/PIN handling or the established file prefixes. */
    if (!target) {
        menu_catalog_item_t item;
        for (int i = 0; i < menu_catalog_count(); ++i) {
            if (menu_catalog_get(i, &item) && strncmp(item.id, "plugin:", 7) != 0 &&
                strcmp(item.name, name) == 0) {
                target = item.view;
                opt_type = item.options_type;
                is_options = target == &options_menu_view;
                break;
            }
        }
    }
    if (!target) { target = &options_menu_view; opt_type = OT_Settings; is_options = true; }
    if (is_options) SelectedMenuType = opt_type;
    // Favourite launched from lockscreen: backing out of the target
    // must return to the lockscreen, not to the view underneath.
    if (is_overlay) {
        // Overlay was floating over dm.current_view (kept alive for captures).
        // Tear the overlay down, then push the lockscreen as a real view so
        // the router history becomes: … -> underlying -> lockscreen -> target.
        // Back from target therefore lands on the lockscreen.
        lockscreen_destroy();
        display_manager_clear_lockscreen_overlay();
        lockscreen_set_overlay_mode(false);
        {
            gui_route_t r_lock = { .id = GUI_ROUTE_VIEW, .view = &lockscreen_view };
            gui_router_navigate_immediate(&r_lock);
        }
    } else {
        // Fullscreen lockscreen is dm.current_view. Keep it in history so
        // Back from the favourite returns to it — do NOT restore return_view.
        display_manager_clear_lockscreen_return_view();
        // lockscreen_destroy will be handled by the navigate to target
        // (router keeps the lockscreen route underneath).
    }
    // Defer switch to next LVGL tick to avoid destroying during event
    display_manager_switch_view(target);
}

static void lockscreen_create_fav_pill(void) {
    lockscreen_destroy_fav_pill();
    if (s_fav_hint && lv_obj_is_valid(s_fav_hint)) { lv_obj_del(s_fav_hint); s_fav_hint = NULL; }
    // Show pill/hint depending on mode
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    if (s_no_pin_mode) {
        int content_w = s_content && lv_obj_is_valid(s_content) ? (int)lv_obj_get_width(s_content) : LV_HOR_RES;
        if (content_w <= 0) content_w = LV_HOR_RES;
        s_fav_hint = lv_label_create(s_content);
        lv_label_set_text(s_fav_hint, LV_SYMBOL_DOWN "  Favorites");
        lv_obj_set_style_text_font(s_fav_hint, gui_font_micro(), 0);
        lv_obj_set_style_text_color(s_fav_hint, lv_color_hex(theme_palette_get_text_muted(theme)), 0);
        lv_obj_set_style_bg_color(s_fav_hint, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_fav_hint, LV_OPA_50, 0);
        lv_obj_set_style_radius(s_fav_hint, 6, 0);
        lv_obj_set_style_pad_hor(s_fav_hint, 6, 0);
        lv_obj_set_style_pad_ver(s_fav_hint, 2, 0);
        lv_obj_align(s_fav_hint, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 6));
        lv_obj_add_flag(s_fav_hint, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_fav_hint, (lv_event_cb_t)lockscreen_show_favorites, LV_EVENT_CLICKED, NULL);
        lv_obj_set_ext_click_area(s_fav_hint, LS_FAV_TOUCH_PAD);
    } else {
        if (!s_content || !lv_obj_is_valid(s_content)) return;
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
        bool cardputer_no_pill = s_landscape_layout;
#else
        bool cardputer_no_pill = false;
#endif
        if (!cardputer_no_pill) {
            s_fav_pill = lv_btn_create(s_content);
            int content_w = (int)lv_obj_get_width(s_content);
            if (content_w <= 0) content_w = LV_HOR_RES;
            int pill_w;
            int pill_x = 0;
            int pill_y;
            int pill_h = 22;
            if (s_landscape_layout) {
                // Landscape: place pill in the left column below the prompt/dots,
                // clamped so it stays fully on-screen even on short panels
                // (e.g. Cardputer's 135px-tall display).
                int left_col_w = content_w / 2;
                int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
                pill_w = left_col_w - 16;
                if (pill_w > 200) pill_w = 200;
                if (pill_w < 80) pill_w = 80;
                pill_x = (left_col_w - pill_w) / 2;
                int dots_bottom = s_ghost_base_y + 60 + 25 + 16; // ghost + gap + PIN dots row
                int desired = dots_bottom + 4;
                int max_y = content_h - pill_h - 4;
                if (desired > max_y) desired = max_y;
                if (desired < 2) desired = 2;
                pill_y = desired;
            } else {
                // Portrait: place pill just above the dynamic numpad position so it
                // never overlaps the keypad regardless of screen height.
                int content_w2 = (int)lv_obj_get_width(s_content);
                if (content_w2 <= 0) content_w2 = LV_HOR_RES;
                pill_w = content_w2 * 70 / 100;
                if (pill_w > 200) pill_w = 200;
                if (pill_w < 100) pill_w = 100;
                pill_x = (content_w2 - pill_w) / 2;
                pill_y = s_numpad_y - pill_h - 6;
                if (pill_y < 2) pill_y = 2;
            }
            lv_obj_set_size(s_fav_pill, pill_w, pill_h);
            lv_obj_set_pos(s_fav_pill, pill_x, pill_y);
            lv_obj_set_style_radius(s_fav_pill, GUI_RADIUS_SM, 0);
            gui_apply_pressed_style(s_fav_pill);
            lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
            gui_menu_card_apply(s_fav_pill, true, surface, lv_color_hex(0x3a3a3a), 1, 0);
            lv_obj_t *lbl = lv_label_create(s_fav_pill);
            lv_label_set_text(lbl, LV_SYMBOL_DOWN " Favorites");
            lv_obj_set_style_text_font(lbl, gui_font_micro(), 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(theme_palette_get_text(theme)), 0);
            lv_obj_center(lbl);
            lv_obj_add_event_cb(s_fav_pill, (lv_event_cb_t)lockscreen_show_favorites, LV_EVENT_CLICKED, NULL);
            // Bigger invisible touch target around the pill (visual stays 22px).
            lv_obj_set_ext_click_area(s_fav_pill, LS_FAV_TOUCH_PAD);
            // Focus ring handling via s_fav_pill_focused
            s_fav_pill_focused = false;
        } else {
            s_fav_pill = NULL;
            s_fav_pill_focused = false;
        }

#ifdef CONFIG_USE_TOUCHSCREEN
#else
        // Keyboard-only landscape boards (e.g. Cardputer): hint under the
        // "Enter PIN" prompt. On Cardputer the pill above is suppressed, so
        // this is the sole affordance and lives just below the PIN dots
        // inside the left column, not at the screen bottom.
        if (s_landscape_layout && !s_fav_hint) {
            int hint_content_w = s_content && lv_obj_is_valid(s_content) ? (int)lv_obj_get_width(s_content) : LV_HOR_RES;
            if (hint_content_w <= 0) hint_content_w = LV_HOR_RES;
            int hint_content_h = LV_VER_RES - GUI_STATUS_BAR_H;
            int hint_left_w = hint_content_w / 2;
            // Dots are at group_y + 60 + 25, ~15px tall — place just below.
            int hint_y = s_ghost_base_y + 60 + 42;
            if (hint_y + 12 > hint_content_h) hint_y = hint_content_h - 12;
            if (hint_y < 2) hint_y = 2;
            s_fav_hint = lv_label_create(s_content);
            lv_label_set_text(s_fav_hint, LV_SYMBOL_DOWN "  Favorites");
            lv_obj_set_style_text_font(s_fav_hint, gui_font_micro(), 0);
            lv_obj_set_style_text_color(s_fav_hint, lv_color_hex(theme_palette_get_text_muted(theme)), 0);
            lv_obj_set_style_bg_color(s_fav_hint, lv_color_hex(0x000000), 0);
            lv_obj_set_style_bg_opa(s_fav_hint, LV_OPA_50, 0);
            lv_obj_set_style_radius(s_fav_hint, 6, 0);
            lv_obj_set_style_pad_hor(s_fav_hint, 6, 0);
            lv_obj_set_style_pad_ver(s_fav_hint, 2, 0);
            // Center in the left column (where ghost/prompt/dots live)
            int x_off = hint_left_w / 2 - hint_content_w / 2;
            lv_obj_align(s_fav_hint, LV_ALIGN_TOP_MID, x_off, hint_y);
            lv_obj_add_flag(s_fav_hint, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(s_fav_hint, (lv_event_cb_t)lockscreen_show_favorites, LV_EVENT_CLICKED, NULL);
            lv_obj_set_ext_click_area(s_fav_hint, LS_FAV_TOUCH_PAD);
        }
#endif
    }
}

static void lockscreen_destroy_fav_pill(void) {
    if (s_fav_pill && lv_obj_is_valid(s_fav_pill)) lv_obj_del(s_fav_pill);
    s_fav_pill = NULL;
    s_fav_pill_focused = false;
    if (s_fav_hint && lv_obj_is_valid(s_fav_hint)) lv_obj_del(s_fav_hint);
    s_fav_hint = NULL;
}

static void lockscreen_normalize_input(InputEvent *event, bool *up, bool *down, bool *left, bool *right, bool *select, bool *back) {
    *up = *down = *left = *right = *select = *back = false;
    if (!event) return;
    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            if (!event->data.joystick_pressed) return;
            if (event->data.joystick_index == 2) *up = true;
            else if (event->data.joystick_index == 4) *down = true;
            else if (event->data.joystick_index == 0) *left = true;
            else if (event->data.joystick_index == 3) *right = true;
            else if (event->data.joystick_index == 1) *select = true;
            break;
        case INPUT_TYPE_KEYBOARD:
            if (event->data.key_value == LV_KEY_UP || event->data.key_value == ';' || event->data.key_value == 'k') *up = true;
            else if (event->data.key_value == LV_KEY_DOWN || event->data.key_value == '.' || event->data.key_value == 'j') *down = true;
            else if (event->data.key_value == LV_KEY_LEFT || event->data.key_value == ',' || event->data.key_value == 'h') *left = true;
            else if (event->data.key_value == LV_KEY_RIGHT || event->data.key_value == '/' || event->data.key_value == 'l') *right = true;
            else if (event->data.key_value == LV_KEY_ENTER || event->data.key_value == '\n' || event->data.key_value == '\r' || event->data.key_value == 13) *select = true;
            else if (event->data.key_value == LV_KEY_ESC || event->data.key_value == 27 || event->data.key_value == 29 || event->data.key_value == '`') *back = true;
            else if (event->data.key_value == '\b') *back = true;
            break;
        case INPUT_TYPE_ENCODER:
            if (event->data.encoder.button) *select = true;
            else if (event->data.encoder.direction < 0) *up = true;
            else if (event->data.encoder.direction > 0) *down = true;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            *back = true;
            break;
        default:
            break;
    }
}

static void lockscreen_update_fav_pill_focus(bool focused) {
    s_fav_pill_focused = focused;
    if (!s_fav_pill || !lv_obj_is_valid(s_fav_pill)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    if (focused) {
        // Accent fill + contrast text, matching the numpad focus style.
        lv_obj_set_style_bg_color(s_fav_pill, accent, 0);
        lv_obj_set_style_bg_opa(s_fav_pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_fav_pill, 2, 0);
        lv_obj_set_style_border_color(s_fav_pill, accent, 0);
        lv_obj_set_style_shadow_width(s_fav_pill, 0, 0);
        lv_obj_set_style_shadow_opa(s_fav_pill, LV_OPA_TRANSP, 0);
    } else {
        gui_menu_card_apply(s_fav_pill, true, surface, lv_color_hex(0x3a3a3a), 1, 0);
    }
    lv_obj_t *lbl = lv_obj_get_child(s_fav_pill, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, focused ? (theme_palette_is_bright(theme) ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF)) : text, 0);
    // Dim numpad focus when pill focused
    if (focused) {
        for (int i = 0; i < NUMPAD_BTNS; i++) if (s_numpad_btns[i] && lv_obj_is_valid(s_numpad_btns[i])) {
            lv_obj_set_style_border_width(s_numpad_btns[i], 1, 0);
            lv_obj_set_style_border_color(s_numpad_btns[i], lv_color_hex(0x444444), 0);
        }
    } else {
        lockscreen_focus_btn(s_focus_idx);
    }
}

static void lockscreen_input_handler(InputEvent *event) {
    if ((esp_timer_get_time() / 1000) < s_ignore_input_until_ms) {
        return;
    }
    bool up, down, left, right, select, back;
    lockscreen_normalize_input(event, &up, &down, &left, &right, &select, &back);
#ifdef CONFIG_USE_TOUCHSCREEN
    // Live-drag samples are only meaningful while the favorites overlay is
    // up; dropping them here keeps the numpad's press/release logic exactly
    // as it was before drag events started arriving.
    if (event->type == INPUT_TYPE_TOUCH && event->is_touch_move && !s_fav_active) {
        return;
    }
#endif
    // Favorites overlay has priority over everything
    if (s_fav_active) {
#ifdef CONFIG_USE_TOUCHSCREEN
        if (event->type == INPUT_TYPE_TOUCH) {
            /* No LVGL indev runs on this platform - bar buttons and rows are
             * hit-tested and dispatched here, same approach as the NFC and
             * SubGHz touch bars. */
            lv_indev_data_t *td = &event->data.touch_data;
            // Live drag scroll while the finger is down.
            if (event->is_touch_move) {
                if (!s_fav_touch_drag.started || !s_fav_list || !lv_obj_is_valid(s_fav_list)) return;
                touch_drag_update(&s_fav_touch_drag, td, s_fav_list);
                return;
            }
            if (td->state == LV_INDEV_STATE_PR) {
                if (gui_touch_bar_hit(s_fav_scroll_up_btn, td->point.x, td->point.y)) {
                    lockscreen_fav_scroll_up();
                    return;
                }
                if (gui_touch_bar_hit(s_fav_scroll_down_btn, td->point.x, td->point.y)) {
                    lockscreen_fav_scroll_down();
                    return;
                }
                if (gui_touch_bar_hit(s_fav_touch_back_btn, td->point.x, td->point.y)) {
                    lockscreen_hide_favorites();
                    return;
                }
                // Begin drag tracking when the press lands inside the list.
                bool in_list = false;
                if (s_fav_list && lv_obj_is_valid(s_fav_list)) {
                    lv_area_t la; lv_obj_get_coords(s_fav_list, &la);
                    in_list = (td->point.x >= la.x1 && td->point.x <= la.x2 && td->point.y >= la.y1 && td->point.y <= la.y2);
                }
                if (in_list) touch_drag_begin(&s_fav_touch_drag, td);
                else touch_drag_reset(&s_fav_touch_drag);
            } else if (td->state == LV_INDEV_STATE_REL) {
                if (touch_drag_release(&s_fav_touch_drag, td)) {
                    // Drag finished: suppress tap handling.
                    display_manager_flush_pending_scroll();
                    lockscreen_fav_update_scroll_buttons();
                    return;
                }
                // Tap on a favorite row: select and open it (index s_fav_count
                // is the Back row, handled by activate_selected()).
                for (int i = 0; i <= s_fav_count; i++) {
                    if (!s_fav_btns || !s_fav_btns[i] || !lv_obj_is_valid(s_fav_btns[i])) continue;
                    lv_area_t ra; lv_obj_get_coords(s_fav_btns[i], &ra);
                    if (td->point.x >= ra.x1 && td->point.x <= ra.x2 && td->point.y >= ra.y1 && td->point.y <= ra.y2) {
                        s_fav_selected = i;
                        lockscreen_fav_set_selected(i);
                        lockscreen_fav_activate_selected();
                        return;
                    }
                }
                bool over_list = false;
                if (s_fav_list && lv_obj_is_valid(s_fav_list)) {
                    lv_area_t la; lv_obj_get_coords(s_fav_list, &la);
                    over_list = (td->point.x >= la.x1 && td->point.x <= la.x2 && td->point.y >= la.y1 && td->point.y <= la.y2);
                }
                bool over_bar = false;
                if (s_fav_touch_bar && lv_obj_is_valid(s_fav_touch_bar)) {
                    lv_area_t ta; lv_obj_get_coords(s_fav_touch_bar, &ta);
                    over_bar = (td->point.x >= ta.x1 && td->point.x <= ta.x2 && td->point.y >= ta.y1 && td->point.y <= ta.y2);
                }
                // Tap anywhere else on the overlay dismisses it.
                if (!over_list && !over_bar) { lockscreen_hide_favorites(); }
            }
            return; // other touch states ignored while the overlay is up
        }
#endif
        if (lockscreen_handle_favorites_input(up, down, left, right, select, back)) return;
        // Encoder linear already handled; fallback
        if (event->type == INPUT_TYPE_ENCODER && !up && !down && !select && !back) return;
        return;
    }
    // Global hotkey 'f' opens favorites from any lockscreen mode
    if (event->type == INPUT_TYPE_KEYBOARD && (event->data.key_value == 'f' || event->data.key_value == 'F')) {
        lockscreen_show_favorites();
        return;
    }

    if (s_no_pin_mode) {
        if (down) { lockscreen_show_favorites(); return; }
        if (event->type != INPUT_TYPE_TOUCH || event->data.touch_data.state == LV_INDEV_STATE_REL) {
            // Check if touch was on favorites hint
            if (event->type == INPUT_TYPE_TOUCH) {
                lv_indev_data_t *td = &event->data.touch_data;
                if (s_fav_hint && lv_obj_is_valid(s_fav_hint)) {
                    lv_area_t ha; lv_obj_get_click_area(s_fav_hint, &ha);
                    if (td->point.x >= ha.x1 && td->point.x <= ha.x2 && td->point.y >= ha.y1 && td->point.y <= ha.y2) {
                        lockscreen_show_favorites(); return;
                    }
                }
            }
            ghostchi_mood_record_event(GHOSTCHI_MOOD_EVENT_WAKE, 4);
            lockscreen_on_correct();
        }
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        // Check pill tap first (click area includes the invisible touch padding)
        if (s_fav_pill && lv_obj_is_valid(s_fav_pill)) {
            lv_area_t pa; lv_obj_get_click_area(s_fav_pill, &pa);
            bool on_pill = (data->point.x >= pa.x1 && data->point.x <= pa.x2 && data->point.y >= pa.y1 && data->point.y <= pa.y2);
            if (on_pill && data->state == LV_INDEV_STATE_REL) { lockscreen_show_favorites(); return; }
        }
        if (s_fav_hint && lv_obj_is_valid(s_fav_hint)) {
            lv_area_t ha; lv_obj_get_click_area(s_fav_hint, &ha);
            bool on_hint = (data->point.x >= ha.x1 && data->point.x <= ha.x2 && data->point.y >= ha.y1 && data->point.y <= ha.y2);
            if (on_hint && data->state == LV_INDEV_STATE_REL) { lockscreen_show_favorites(); return; }
        }
        if (data->state == LV_INDEV_STATE_PR) {
            int idx = lockscreen_hit_test(data->point.x, data->point.y);
            s_touch_started = (idx >= 0);
            s_touch_pressed_idx = idx;
            if (idx >= 0) {
                s_focus_idx = idx;
                if (s_fav_pill_focused) lockscreen_update_fav_pill_focus(false);
                lockscreen_focus_btn(s_focus_idx);
            }
        } else if (data->state == LV_INDEV_STATE_REL && s_touch_started) {
            int idx = lockscreen_hit_test(data->point.x, data->point.y);
            if (idx >= 0 && idx == s_touch_pressed_idx) {
                s_suppress_click_idx = idx;
                s_suppress_click_until_ms = (esp_timer_get_time() / 1000) + 250;
                char c = k_numpad_chars[idx];
                if (c == '\b') lockscreen_delete_last();
                else if (c == '\r') lockscreen_submit();
                else lockscreen_add_char(c);
            }
            s_touch_started = false;
            s_touch_pressed_idx = -1;
        }
        return;
    }

    // Reuse already-normalized up/down/left/right/select/back
    // Keyboard direct typing for PIN entry
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t kv = event->data.key_value;
        if (kv >= '0' && kv <= '9') {
            if (s_fav_pill_focused) lockscreen_update_fav_pill_focus(false);
            lockscreen_add_char((char)kv);
            return;
        }
        if (kv == '\b') {
            lockscreen_delete_last();
            return;
        }
        if (kv == '\r' || kv == '\n' || kv == 13) {
            if (s_fav_pill_focused) { lockscreen_show_favorites(); return; }
            lockscreen_submit();
            return;
        }
    }

    // Pill navigation (PIN mode) - UP on top row focuses pill (if present)
    if (s_fav_pill && lv_obj_is_valid(s_fav_pill)) {
        if (s_fav_pill_focused) {
            if (down) { lockscreen_update_fav_pill_focus(false); return; }
            if (select) { lockscreen_show_favorites(); return; }
            if (back) { lockscreen_update_fav_pill_focus(false); return; }
            if (up || left || right) { lockscreen_update_fav_pill_focus(false); return; }
            return;
        } else {
            if (up && s_focus_idx >= 0 && s_focus_idx < NUMPAD_COLS) {
                lockscreen_update_fav_pill_focus(true);
                return;
            }
        }
    }
    // Bottom row + DOWN opens Favorites - works even without pill
    // (Cardputer landscape shows only the bottom hint).
    if (!s_fav_pill_focused && down && s_focus_idx >= NUMPAD_BTNS - NUMPAD_COLS) {
        lockscreen_show_favorites();
        return;
    }

    if (s_numpad_cont && lv_obj_is_valid(s_numpad_cont)) {
        // Encoder gets linear navigation (CW = next, CCW = prev)
        if (event->type == INPUT_TYPE_ENCODER) {
            if (s_fav_pill_focused) {
                if (select) { lockscreen_show_favorites(); return; }
                else if (event->data.encoder.direction > 0) { lockscreen_update_fav_pill_focus(false); return; }
                else if (event->data.encoder.direction < 0) { lockscreen_update_fav_pill_focus(false); return; }
                return;
            }
            if (select) {
                char c = k_numpad_chars[s_focus_idx];
                if (c == '\b') lockscreen_delete_last();
                else if (c == '\r') lockscreen_submit();
                else lockscreen_add_char(c);
            } else if (event->data.encoder.direction > 0) {
                s_focus_idx++;
                if (s_focus_idx >= NUMPAD_BTNS) s_focus_idx = 0;
                lockscreen_focus_btn(s_focus_idx);
            } else if (event->data.encoder.direction < 0) {
                s_focus_idx--;
                if (s_focus_idx < 0) s_focus_idx = NUMPAD_BTNS - 1;
                lockscreen_focus_btn(s_focus_idx);
            }
            return;
        }
        if (s_fav_pill_focused) return;
        if (up) lockscreen_move_focus(0, -1);
        if (down) lockscreen_move_focus(0, 1);
        if (left) lockscreen_move_focus(-1, 0);
        if (right) lockscreen_move_focus(1, 0);
        if (select) {
            char c = k_numpad_chars[s_focus_idx];
            if (c == '\b') lockscreen_delete_last();
            else if (c == '\r') lockscreen_submit();
            else lockscreen_add_char(c);
        }
        if (back) lockscreen_delete_last();
    }
}

void lockscreen_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg_color = lv_color_hex(theme_palette_get_background(theme));

    s_no_pin_mode = !s_setup_mode && !lockscreen_is_configured();

    /* Overlay mode floats above the still-live view on the top layer; we must
     * not paint the active screen (that belongs to the view underneath) and we
     * leave the shared status bar alone by passing a NULL title. The opaque
     * full-screen root hides everything below it on its own. */
    if (!s_overlay_mode) {
        display_manager_fill_screen(bg_color);
    }
    lv_obj_t *root_parent = s_overlay_mode ? lv_layer_top() : NULL;
    const char *root_title = s_overlay_mode ? NULL : (s_no_pin_mode ? "Ghostchi" : "Locked");
    s_root = gui_screen_create_root(root_parent, root_title, bg_color, LV_OPA_COVER);
    /* Only register as the live view when we ARE the view; an overlay must not
     * masquerade as the current view in the view system. */
    if (!s_overlay_mode) {
        lockscreen_view.root = s_root;
    }
    s_content = gui_screen_create_content(s_root, GUI_STATUS_BAR_H);

    int content_h = LV_VER_RES - GUI_STATUS_BAR_H;
    int content_w = lv_obj_get_width(s_content);
    if (content_w <= 0) content_w = LV_HOR_RES;
    bool landscape = (content_w > content_h && content_h <= 146);

    if (s_no_pin_mode) {
        lockscreen_build_companion_layout(content_w, content_h);
    } else if (landscape) {
        int left_col_w = content_w / 2;
        int ghost_sz = 60;
        int ghost_x = (left_col_w - ghost_sz) / 2;
        int group_h = ghost_sz + 8 + 13 + 4 + 17;
        int group_y = (content_h - group_h) / 2;
        if (group_y < 0) group_y = 0;
        group_y += 8;
        s_ghost_base_y = group_y;

        s_ghost = lv_img_create(s_content);
        lv_img_set_src(s_ghost, LOCKSCREEN_SPRITE(tired_50x50));
        lv_obj_set_pos(s_ghost, ghost_x, s_ghost_base_y + lockscreen_ghost_bob_offset());
        lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);

        s_prompt = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_prompt, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_prompt, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(s_prompt, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_prompt, LV_OPA_60, 0);
        lv_obj_set_style_radius(s_prompt, 3, 0);
        lv_obj_set_style_pad_hor(s_prompt, 4, 0);
        lv_obj_set_style_pad_ver(s_prompt, 1, 0);
        lv_obj_set_style_text_align(s_prompt, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_prompt, left_col_w);
        lv_obj_align(s_prompt, LV_ALIGN_TOP_LEFT, 0, group_y + ghost_sz + 8);

        s_dots = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_dots, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_dots, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(s_dots, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_dots, LV_OPA_60, 0);
        lv_obj_set_style_radius(s_dots, 3, 0);
        lv_obj_set_style_pad_hor(s_dots, 4, 0);
        lv_obj_set_style_pad_ver(s_dots, 1, 0);
        lv_obj_set_style_text_align(s_dots, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_dots, left_col_w);
        lv_obj_align(s_dots, LV_ALIGN_TOP_LEFT, 0, group_y + ghost_sz + 25);
    } else {
        int min_numpad_y = 94;
        int bottom_margin = 10;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        min_numpad_y = content_h / 4;
        bottom_margin = 20;
#endif
        int numpad_gap = 2;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        numpad_gap = 8;
#endif
        int numpad_h = content_h - min_numpad_y - bottom_margin;
        if (numpad_h < 36) numpad_h = 36;
        int btn_h = (numpad_h - (NUMPAD_ROWS - 1) * numpad_gap) / NUMPAD_ROWS;
        if (LV_VER_RES > 240) {
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
            if (btn_h > GUI_CONTROL_H) btn_h = GUI_CONTROL_H;
#else
            if (btn_h > 42) btn_h = 42;
#endif
        } else {
            if (btn_h > 30) btn_h = 30;
        }
        if (btn_h < 14) btn_h = 14;
        int grid_h = NUMPAD_ROWS * btn_h + (NUMPAD_ROWS - 1) * numpad_gap;
        int numpad_y = content_h - grid_h - bottom_margin;
        if (numpad_y < min_numpad_y) numpad_y = min_numpad_y;

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        int ghost_sz = content_h >= 480 ? 160 : 112;
        int group_h = ghost_sz + 54;
        int icon_y = (numpad_y - group_h) / 2;
        if (icon_y < 8) icon_y = 8;
        int prompt_y_offset = icon_y + ghost_sz + 8;
        int dots_y_offset = icon_y + ghost_sz + 26;
#else
        int icon_y = (numpad_y - 88) / 2;
        if (icon_y < 2) icon_y = 2;
        int prompt_y_offset = icon_y + 56;
        int dots_y_offset = icon_y + 72;
#endif
        s_ghost_base_y = icon_y;

        s_ghost = lv_img_create(s_content);
        lv_img_set_src(s_ghost, LOCKSCREEN_SPRITE(tired_50x50));
        lv_obj_set_pos(s_ghost, (content_w - 50) / 2, s_ghost_base_y + lockscreen_ghost_bob_offset());
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        lv_img_set_zoom(s_ghost, (ghost_sz * 256) / 50);
#endif

        s_prompt = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_prompt, gui_font_caption(), 0);
        lv_obj_set_style_text_color(s_prompt, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(s_prompt, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_prompt, LV_OPA_60, 0);
        lv_obj_set_style_radius(s_prompt, 3, 0);
        lv_obj_set_style_pad_hor(s_prompt, 6, 0);
        lv_obj_set_style_pad_ver(s_prompt, 1, 0);
        lv_obj_align(s_prompt, LV_ALIGN_TOP_MID, 0, prompt_y_offset);

        s_dots = lv_label_create(s_content);
        lv_obj_set_style_text_font(s_dots,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                                   gui_font_body(),
#else
                                   &lv_font_montserrat_16,
#endif
                                   0);
        lv_obj_set_style_text_color(s_dots, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_bg_color(s_dots, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_dots, LV_OPA_60, 0);
        lv_obj_set_style_radius(s_dots, 3, 0);
        lv_obj_set_style_pad_hor(s_dots, 6, 0);
        lv_obj_set_style_pad_ver(s_dots, 1, 0);
        lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, dots_y_offset);
    }

    // Explicit PIN setup is separate from the no-PIN companion lockscreen.
    if (s_setup_mode) {
        s_setup_mode = true;
        s_setup_confirm = false;
        s_setup_first[0] = '\0';
        lockscreen_set_prompt("Set PIN/OK none");
    } else if (s_no_pin_mode) {
        lockscreen_set_prompt("Press to unlock");
    } else {
        lockscreen_set_prompt("Enter PIN");
    }

    if (!s_no_pin_mode) {
        lockscreen_build_numpad();
    }
    // Favorites pill/hint (responsive, pre-PIN)
    lockscreen_create_fav_pill();

    lockscreen_update_dots();
    s_ghost_state = GHOST_SLEEPING;
    lockscreen_update_ghost(true);

    if (s_idle_timer) {
        lv_timer_del(s_idle_timer);
        s_idle_timer = NULL;
    }
    s_idle_timer = lv_timer_create(lockscreen_idle_cb, 3000, NULL);
    if (s_bob_timer) {
        lv_timer_del(s_bob_timer);
        s_bob_timer = NULL;
    }
    s_bob_timer = lv_timer_create(lockscreen_bob_cb, 250, NULL);
}

void lockscreen_destroy(void) {
    if (s_idle_timer) {
        lv_timer_del(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_bob_timer) {
        lv_timer_del(s_bob_timer);
        s_bob_timer = NULL;
    }
    if (s_unlock_timer) {
        lv_timer_del(s_unlock_timer);
        s_unlock_timer = NULL;
    }
    if (s_shake_timer) {
        lv_timer_del(s_shake_timer);
        s_shake_timer = NULL;
    }
    lockscreen_destroy_numpad();
    lockscreen_hide_favorites();
    lockscreen_destroy_fav_pill();
    s_pending_fav_launch = NULL;
    s_touch_started = false;
    s_touch_pressed_idx = -1;
    s_suppress_click_idx = -1;
    s_suppress_click_until_ms = 0;
    s_no_pin_mode = false;
    s_overlay_mode = false;
    lvgl_obj_del_safe(&s_root);
    lockscreen_view.root = NULL;
    s_content = NULL;
    s_ghost = NULL;
    s_prompt = NULL;
    s_dots = NULL;
}

View lockscreen_view = {
    .root = NULL,
    .create = lockscreen_create,
    .destroy = lockscreen_destroy,
    .input_callback = lockscreen_input_handler,
    .name = "Lockscreen",
};
