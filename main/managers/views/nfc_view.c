 #include "gui/screen_layout.h"
#include "managers/display_manager.h"
#include "managers/views/nfc_view.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/keyboard_screen.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/options_view.h"
#include "gui/ios_toggle.h"
#include "gui/toast.h"
#include "managers/status_display_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "managers/views/error_popup.h"
#include "gui/popup.h"
#include "gui/lvgl_safe.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "gui/gui_router.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <strings.h>
#include <errno.h>
#include <dirent.h>
#include "managers/sd_card_manager.h"
#include "managers/fuel_gauge_manager.h"
#include "core/glog.h"
#include "core/commands.h"

// popup helper forward declarations
lv_obj_t *popup_create_container(lv_obj_t *parent, int width, int height, bool fullscreen);
lv_obj_t *popup_create_container_with_offset(lv_obj_t *parent, int width, int height, lv_coord_t y_offset, bool fullscreen);

lv_obj_t *popup_add_styled_button(lv_obj_t *container,
	const char *label_text,
    int btn_w, int btn_h,
    lv_align_t align, lv_coord_t x_ofs, lv_coord_t y_ofs,
    const lv_font_t *font, lv_event_cb_t cb, void *user_data);

lv_obj_t *popup_create_title_label(lv_obj_t *container, const char *title, const lv_font_t *font, lv_coord_t y_ofs);

lv_obj_t *popup_create_body_label(lv_obj_t *container, const char *text, lv_coord_t width, bool wrap, const lv_font_t *font, lv_coord_t y_ofs);
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
#define NFC_HAS_LOCAL_READER 1
#endif

#if defined(NFC_HAS_LOCAL_READER) || defined(CONFIG_NFC_CHAMELEON)
#include "managers/nfc/mifare_classic.h"
#include "managers/nfc/mifare_attack.h"
#include "managers/nfc/flipper_nfc_compat.h"
#include "managers/nfc/nfc_backend.h"
#endif
#include "managers/chameleon_manager.h"
#include "managers/nfc/ndef.h"
#include "managers/ghostscript_runtime.h"

// Forward declaration for nfc_get_detected_title
static const char* nfc_get_detected_title(void);

// freeRTOS used regardless of backend
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef NFC_HAS_LOCAL_READER
#include "pn532.h"
#include "driver/i2c_types.h"
#include "pn532_driver.h"
#ifdef CONFIG_NFC_PN532
#include "pn532_driver_i2c.h"
#endif
#ifdef CONFIG_NFC_ST25R3916
#include "st25r3916_adapter.h"
#include "st25r3916_iso15693.h"
#include "st25r3916.h"
#include "st25r3916_reg.h"
#include "managers/nfc/picopass.h"
#endif
#endif

// always needed for parsing .nfc files and displaying details, even without PN532
#include "managers/nfc/ntag_t2.h"
#include "managers/nfc/write_ntag.h"
#include "managers/nfc/desfire.h"
#include "managers/nfc/emv.h"
#include "managers/nfc/ndef_builder.h"
#include "managers/nfc/ndef_tag_gen.h"

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation declared later after static variables are defined)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);
void mfc_ui_set_cache_mode(bool on);
void mfc_ui_set_paused(bool on);
bool nfc_is_scan_cancelled(void);
bool nfc_is_dict_skip_requested(void);

// Forward declaration of this view instance for internal references
extern View nfc_view;

static const char *TAG = "NFCView";

// touch nav button sizing now owned by gui/touch_bar.h (GUI_TOUCH_BAR_*)

static options_view_t *g_nfc_ov = NULL;

void nfc_option_event_cb(lv_event_t *e);
void nfc_view_input_cb(InputEvent *event);

#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
static void nfc_backend_event_cb(lv_event_t *e);
static lv_obj_t *nfc_add_backend_item(void);
#endif

static void scroll_nfc_up(lv_event_t *e);
static void scroll_nfc_down(lv_event_t *e);
static void update_nfc_scroll_buttons_visibility(void);

static lv_obj_t *root = NULL;
static lv_obj_t *menu_container = NULL;
static lv_obj_t *scan_btn = NULL;
static lv_obj_t *emulate_btn = NULL;
static lv_obj_t *backend_btn = NULL;
static lv_obj_t *scroll_up_btn = NULL;
static lv_obj_t *scroll_down_btn = NULL;
static lv_obj_t *back_btn = NULL;
static gui_touch_bar_t s_touch_tb = {0};
static int selected_index = 0;
static int num_items = 0; // will be set when building menu

// write file list state
static bool in_write_list = false;
static bool in_emulate_list = false;
static bool in_mfc_menu = false;
static bool in_tools_menu = false;
static char **nfc_file_paths = NULL;
static size_t nfc_file_count = 0;
static char **nfc_emu_file_paths = NULL;
static size_t nfc_emu_file_count = 0;

// generate-tag flow state
static bool in_generate_list = false;
static char g_gen_field1[128] = {0}; // SSID / vCard name
static char g_gen_field2[128] = {0}; // WiFi password / vCard phone
static char g_gen_field3[128] = {0}; // vCard email

// saved file list state
static bool in_saved_list = false;
static char **saved_file_paths = NULL;
static size_t saved_file_count = 0;

#ifdef CONFIG_USE_TOUCHSCREEN
static touch_drag_t nfc_touch_drag = {0};
static touch_drag_t nfc_credits_drag = {0};
static touch_drag_t saved_details_drag = {0};
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int NFC_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int NFC_SWIPE_THRESHOLD_RATIO = 10;
#endif
#endif
static bool nfc_option_invoked = false;
static unsigned long nfc_created_time_ms = 0;

// NFC write popup
static lv_obj_t *nfc_write_popup = NULL;
static lv_obj_t *nfc_write_cancel_btn = NULL;
static lv_obj_t *nfc_write_go_btn = NULL;
static lv_obj_t *nfc_write_title_label = NULL;
static lv_obj_t *nfc_write_details_label = NULL;
static int nfc_write_popup_selected = 0; // 0=Cancel, 1=Write
static volatile bool nfc_write_cancel = false;
static volatile bool nfc_write_in_progress = false;
static bool g_write_image_valid = false;
#ifdef NFC_HAS_LOCAL_READER
static ntag_file_image_t g_write_image;
#endif
static char g_write_image_path[256] = {0};

// NFC emulate popup
static lv_obj_t *nfc_emu_popup = NULL;
static lv_obj_t *nfc_emu_cancel_btn = NULL;
static lv_obj_t *nfc_emu_title_label = NULL;
static lv_obj_t *nfc_emu_details_label = NULL;
static int nfc_emu_popup_selected = 0;
static bool nfc_emu_active = false;

// jit sd helpers for somethingsomething template (mirror infrared behavior)
static bool nfc_sd_begin(bool *display_was_suspended)
{
    return sd_card_jit_begin(display_was_suspended, true);
}

static void nfc_sd_end(bool display_was_suspended)
{
    sd_card_jit_end(display_was_suspended);
}

// saved details popup
static lv_obj_t *saved_popup = NULL;
static lv_obj_t *saved_close_btn = NULL;
static lv_obj_t *saved_rename_btn = NULL;
static lv_obj_t *saved_delete_btn = NULL;
static lv_obj_t *saved_title_label = NULL;
static lv_obj_t *saved_details_label = NULL;
static lv_obj_t *saved_scroll = NULL;
static int saved_popup_selected = 0;
static bool saved_details_parsed_view = false;
static bool saved_has_extra_details = false;
static char *saved_details_text = NULL;
static char g_saved_current_path[256] = {0};
// Deep-link request (favorite launch): saved tag to open on create/live view.
static char *s_pending_saved_open = NULL;
static popup_confirm_t *saved_delete_confirm_popup = NULL;

// user mfc keys popup
static lv_obj_t *keys_popup = NULL;
static lv_obj_t *keys_close_btn = NULL;
static lv_obj_t *keys_title_label = NULL;
static lv_obj_t *keys_details_label = NULL;
static lv_obj_t *keys_up_btn = NULL;
static lv_obj_t *keys_down_btn = NULL;
static lv_obj_t *keys_scroll = NULL;
static int keys_popup_selected = 0;
static lv_obj_t *nfc_credits_popup = NULL;
static lv_obj_t *nfc_credits_close_btn = NULL;
static lv_obj_t *nfc_credits_scroll = NULL;

// UI hook from MIFARE Classic layer to indicate sector/block/key phase
// (implementation moved below after static phase variables are declared)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys);

// NFC scan popup (modeled after IR learning popup)
static lv_obj_t *nfc_scan_popup = NULL;
static lv_obj_t *nfc_btn_bar = NULL;
static lv_obj_t *nfc_scan_cancel_btn = NULL;
static lv_obj_t *nfc_scan_more_btn = NULL;
static lv_obj_t *nfc_scan_save_btn = NULL;
static lv_obj_t *nfc_scan_scroll_btn = NULL;
static lv_obj_t *nfc_title_label = NULL;
static lv_obj_t *nfc_uid_label = NULL;
static lv_obj_t *nfc_type_label = NULL;
static lv_obj_t *nfc_details_label = NULL;
static lv_obj_t *nfc_details_scroll = NULL;
// Progress bar removed; we will update title and text instead
// Track dictionary brute-force phase for richer UI status
static int mfc_phase_sector = -1;
static int mfc_phase_first_block = -1;
static bool mfc_phase_key_b = false;
static int mfc_phase_total = 0;
static int nfc_popup_selected = 0; // 0 = Cancel, 1 = More (when available)
static int nfc_details_view_mode = 0; // 0=Summary, 1=Basic, 2=Full
static bool nfc_more_visible = false;
static bool nfc_details_visible = false;
static bool nfc_save_visible = false;
// When true, the MFC layer is performing a second-pass cache fill (live-read) after bruteforce.
static bool nfc_cache_fill_phase = false;
static bool nfc_hardnested_phase = false;  // true while collecting hardnested nonces
// When true, UI requests to skip dictionary attempts (basic read only)
static bool nfc_dict_skip_requested = false;
// When true, a tag was removed and we're waiting for re-present
static bool nfc_paused = false;
// When true, NFC details are ready and scan is complete
static bool nfc_details_ready = false;
// Active scan session to filter out stale async UI events
static uint32_t nfc_scan_session = 0;
// Simple boolean event payload for async calls
typedef struct { bool on; uint32_t session; } nfc_bool_evt_t;
// PN532 UID event payload for async label updates
typedef struct { uint32_t session; uint8_t uid[10]; uint8_t uid_len; } nfc_uid_evt_t;
// Dictionary progress payload for async calls
typedef struct { int c; int t; uint32_t s; } dict_prog_t;
// Static event pool to eliminate per-event malloc
#define NFC_EVENT_POOL_SIZE 8
static nfc_bool_evt_t nfc_bool_pool[NFC_EVENT_POOL_SIZE];
static dict_prog_t nfc_dict_pool[NFC_EVENT_POOL_SIZE];
static uint32_t nfc_bool_pool_mask = 0;
static uint32_t nfc_dict_pool_mask = 0;
static bool nfc_skip_label_applied = false;

static const char* get_details_split_point(const char *text) {
    if (!text) return NULL;
    const char *p = strstr(text, "Keys ");
    if (!p) return NULL;
    p = strchr(p, '\n');
    if (p) return p + 1;
    return NULL;
}

static bool has_extra_details(const char *text) {
    const char *p = get_details_split_point(text);
    return (p && *p != '\0');
}

// Pool allocation helpers
static nfc_bool_evt_t* nfc_bool_pool_alloc(void) {
    for (int i = 0; i < NFC_EVENT_POOL_SIZE; i++) {
        if (!(nfc_bool_pool_mask & (1U << i))) {
            nfc_bool_pool_mask |= (1U << i);
            return &nfc_bool_pool[i];
        }
    }
    return NULL;
}
static void nfc_bool_pool_free(nfc_bool_evt_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_bool_pool;
    if (idx >= 0 && idx < NFC_EVENT_POOL_SIZE) {
        nfc_bool_pool_mask &= ~(1U << idx);
    }
}

// Forward declarations for emulate flow (defined later in file)
static void nfc_enter_emulate_list(void);
static void nfc_emu_cancel_cb(lv_event_t *e);
static void cleanup_nfc_emu_popup(void *obj);
static void create_nfc_emu_popup(const char *path, bool test_ndef);
static void nfc_emulate_test_cb(lv_event_t *e);
static void nfc_emulate_file_item_cb(lv_event_t *e);
static dict_prog_t* nfc_dict_pool_alloc(void) {
    for (int i = 0; i < NFC_EVENT_POOL_SIZE; i++) {
        if (!(nfc_dict_pool_mask & (1U << i))) {
            nfc_dict_pool_mask |= (1U << i);
            return &nfc_dict_pool[i];
        }
    }
    return NULL;
}
static void nfc_dict_pool_free(dict_prog_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_dict_pool;
    if (idx >= 0 && idx < NFC_EVENT_POOL_SIZE) {
        nfc_dict_pool_mask &= ~(1U << idx);
    }
}

// Async setter for paused title/state
static void nfc_set_paused_async(void *ptr) {
    nfc_bool_evt_t *ev = (nfc_bool_evt_t*)ptr;
    if (!ev) return;
    if (ev->session != nfc_scan_session) { nfc_bool_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_bool_pool_free(ev); return; }
    nfc_paused = ev->on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (ev->on) {
            lv_label_set_text(nfc_title_label, "Paused - present tag to continue");
        } else {
            if (nfc_cache_fill_phase) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
            else if (!nfc_details_visible) lv_label_set_text(nfc_title_label, "Unlocking card... 0%");
            else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
        }
    }
    nfc_bool_pool_free(ev);
}

// Exposed to MFC layer
void mfc_ui_set_paused(bool on) {
    if (!display_manager_is_available()) return;
    nfc_bool_evt_t *ev = nfc_bool_pool_alloc();
    if (!ev) return;
    ev->on = on;
    ev->session = nfc_scan_session;
    display_manager_lvgl_async_call(nfc_set_paused_async, ev);
}

// Async setter for cache fill phase title/state
static void nfc_set_cache_mode_async(void *ptr) {
    nfc_bool_evt_t *ev = (nfc_bool_evt_t*)ptr;
    if (!ev) return;
    if (ev->session != nfc_scan_session) { nfc_bool_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_bool_pool_free(ev); return; }
    bool on = ev->on;
    nfc_cache_fill_phase = on;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        if (on) lv_label_set_text(nfc_title_label, "Reading sectors... 0%");
        else { lv_label_set_text(nfc_title_label, "NFC Tag"); lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22); }
    }
    nfc_bool_pool_free(ev);
}

// Exposed to MFC layer to toggle cache-fill phase
void mfc_ui_set_cache_mode(bool on) {
    if (!display_manager_is_available()) return;
    nfc_bool_evt_t *ev = nfc_bool_pool_alloc();
    if (!ev) return;
    ev->on = on;
    ev->session = nfc_scan_session;
    display_manager_lvgl_async_call(nfc_set_cache_mode_async, ev);
}

// Exposed for mifare_classic.c to honor UI skip request (weak extern there)
bool nfc_is_dict_skip_requested(void) { return nfc_dict_skip_requested; }

#if defined(NFC_HAS_LOCAL_READER) || defined(CONFIG_NFC_CHAMELEON)
static const mfc_attack_hooks_t nfc_ui_attack_hooks = {
    .on_phase = mfc_ui_set_phase,
    .on_cache_mode = mfc_ui_set_cache_mode,
    .on_paused = mfc_ui_set_paused,
    .should_cancel = nfc_is_scan_cancelled,
    .should_skip_dict = nfc_is_dict_skip_requested
};
#endif

static void nfc_scan_cancel_cb(lv_event_t *e);
static void nfc_scan_more_cb(lv_event_t *e);
static void nfc_scan_save_cb(lv_event_t *e);
static void nfc_scan_scroll_cb(lv_event_t *e);
static void create_nfc_scan_popup(void);
void cleanup_nfc_scan_popup(void *obj);
static void update_nfc_popup_selection(void);
static void update_nfc_buttons_layout(void);
static void nfc_show_details_view(bool show);
static bool write_flipper_nfc_file(void);
// Worker task helpers
static void nfc_save_task(void *arg);
static void nfc_save_done_async(void *ptr);
// Deferred scan start if previous scan task hasn't exited yet (legacy - no longer used)
static void nfc_try_start_scan_timer_cb(lv_timer_t *t);
// Guard timer to avoid creating multiple retry timers (legacy - no longer used)
static lv_timer_t *nfc_scan_retry_timer = NULL;

// Write flow (file list and popup)
static void nfc_enter_write_list(void);
static void nfc_clear_write_list(void);
static void nfc_file_item_cb(lv_event_t *e);
static void back_to_root_menu(void);
static void create_nfc_write_popup(const char *path);
void cleanup_nfc_write_popup(void *obj);
static void nfc_write_cancel_cb(lv_event_t *e);
static void nfc_write_go_cb(lv_event_t *e);
static void update_nfc_write_popup_selection(void);
static void update_nfc_write_buttons_layout(void);
// Generate-tag flow (NDEF record builder UI)
static void nfc_enter_generate_list(void);
static void nfc_clear_generate_list(void);
static void nfc_enter_mfc_menu(void);
static void nfc_enter_tools_menu(void);
// saved flow
static void saved_enter_list(void);
void saved_clear_list(void);
static void saved_file_item_cb(lv_event_t *e);
static void create_saved_details_popup(const char *path);
void cleanup_saved_details_popup(void *obj);
static void saved_close_cb(lv_event_t *e);
static void saved_more_cb(lv_event_t *e);
static void update_saved_popup_selection(void);
static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w);
static void layout_popup_buttons_row(lv_obj_t *popup, lv_obj_t **btns, int count, lv_coord_t min_w, lv_coord_t max_w, lv_coord_t min_threshold, lv_coord_t gap, lv_coord_t yoff);
static void update_saved_buttons_layout(void);
static void update_saved_buttons_layout(void);
static char* build_mfc_details_from_file(const char *path, char **out_title);
static char* build_desfire_details_from_file(const char *path, char **out_title);
static char* build_emv_details_from_file(const char *path, char **out_title);
static void saved_rename_cb(lv_event_t *e);
static void saved_delete_cb(lv_event_t *e);
static void saved_rename_keyboard_callback(const char *name);
typedef struct {
    char old_path[256];
    char new_path[256];
    int success;
} saved_rename_job_t;
static void saved_rename_ui_done_cb(void *param);
static void saved_rename_task(void *arg);
// keys popup
static void create_keys_popup(void);
static void cleanup_keys_popup(void *obj);
static void keys_close_cb(lv_event_t *e);
static void update_keys_popup_selection(void);
static void update_keys_buttons_layout(void);
static void update_keys_buttons_layout(void);
static void keys_scroll_up_cb(lv_event_t *e);
static void keys_scroll_down_cb(lv_event_t *e);
static void create_nfc_credits_popup(void);
static void cleanup_nfc_credits_popup(void *obj);
static void nfc_credits_close_cb(lv_event_t *e);

// chameleon ultra popup (basic controls)
static lv_obj_t *cu_popup = NULL;
static lv_obj_t *cu_title_label = NULL;
static lv_obj_t *cu_details_label = NULL;
static lv_obj_t *cu_close_btn = NULL;
static lv_obj_t *cu_connect_btn = NULL;
static lv_obj_t *cu_disconnect_btn = NULL;
static lv_obj_t *cu_reader_btn = NULL;
static lv_obj_t *cu_scan_hf_btn = NULL;
static lv_obj_t *cu_save_hf_btn = NULL;
static lv_obj_t *cu_more_btn = NULL;
static int cu_popup_selected = 0;
static bool cu_save_visible = false;
static volatile bool cu_busy = false;
static bool cu_more_expanded = false;
static void cu_state_timer_cb(lv_timer_t *t);
static lv_timer_t *cu_state_timer = NULL;

static void create_cu_popup(void);
void cleanup_cu_popup(void *obj);
static void update_cu_buttons_layout(void);
static void update_cu_popup_selection(void);
static void cu_close_cb(lv_event_t *e);
static void cu_connect_cb(lv_event_t *e);
static void cu_disconnect_cb(lv_event_t *e);
static void cu_reader_cb(lv_event_t *e);
static void cu_scan_hf_cb(lv_event_t *e);
static void cu_save_hf_cb(lv_event_t *e);
static void cu_more_cb(lv_event_t *e);
static void cu_connect_task(void *arg);
static void cu_disconnect_task(void *arg);
static void cu_reader_task(void *arg);
static void cu_scan_hf_task(void *arg);
static void cu_save_hf_task(void *arg);
static void cu_bool_done_async(void *ptr);
#ifdef NFC_HAS_LOCAL_READER
static bool ensure_pn532_ready(void);
static void nfc_write_task(void *arg);
typedef struct { int current; int total; } nfc_wr_prog_t;
static bool nfc_write_progress_cb(int current, int total, void *user);
static void nfc_write_progress_async(void *ptr);
static void nfc_write_done_async(void *ptr);
#endif

// Dictionary progress callback -> UI updater
static void nfc_progress_update_async(void *ptr);
// PN532 UID/type updater forward declaration
static void nfc_update_labels_async(void *ptr);
// UI hook from MIFARE Classic layer to indicate sector/block/key phase (implementation)
void mfc_ui_set_phase(int sector, int first_block, bool key_b, int total_keys) {
    if (!display_manager_is_available()) return;
    mfc_phase_sector = sector;
    mfc_phase_first_block = first_block;
    mfc_phase_key_b = key_b;
    mfc_phase_total = total_keys;
    
    // Add detailed status messages for MFC dictionary attack phases
    if (sector == 0 && first_block == 0) {
        status_display_show_status("MFC Attack Start");
    } else if (sector >= 0) {
        char status_msg[32];
        snprintf(status_msg, sizeof(status_msg), "MFC Sec %d Key %c", sector, key_b ? 'B' : 'A');
        status_display_show_status(status_msg);
    }
    
    dict_prog_t *dp = nfc_dict_pool_alloc();
    if (dp) { dp->c = 0; dp->t = total_keys; dp->s = nfc_scan_session; display_manager_lvgl_async_call(nfc_progress_update_async, dp); }
}
static void mfc_dict_progress_cb(int current, int total, void *user) {
    (void)user;
    if (!display_manager_is_available()) return;
    if (total <= 0) return;
    int percent = (current * 100) / total;
    if (percent < 0) { percent = 0; }
    if (percent > 100) { percent = 100; }
    static int last_percent = -1;
    if (percent == last_percent) return;
    last_percent = percent;
    
    // Add status display messages for NFC scanning phases
    if (nfc_cache_fill_phase) {
        status_display_show_status("NFC Reading...");
    } else if (!nfc_details_visible && mfc_phase_total > 0) {
        // Show progress milestones for dictionary attack
        if (percent == 0) {
            status_display_show_status("MFC Dict Start");
        } else if (percent == 25) {
            status_display_show_status("MFC 25%");
        } else if (percent == 50) {
            status_display_show_status("MFC 50%");
        } else if (percent == 75) {
            status_display_show_status("MFC 75%");
        } else if (percent == 100) {
            status_display_show_status("MFC Dict Done");
        }
    } else if (nfc_dict_skip_requested) {
        status_display_show_status("NFC Basic Read");
    }
    
    dict_prog_t *dp = nfc_dict_pool_alloc();
    if (!dp) return;
    dp->c = current; dp->t = total; dp->s = nfc_scan_session;
    display_manager_lvgl_async_call(nfc_progress_update_async, dp);
}

// Static pool for UID events to eliminate malloc
#define NFC_UID_POOL_SIZE 4
static nfc_uid_evt_t nfc_uid_pool[NFC_UID_POOL_SIZE];
static uint32_t nfc_uid_pool_mask = 0;

static nfc_uid_evt_t* nfc_uid_pool_alloc(void) {
    for (int i = 0; i < NFC_UID_POOL_SIZE; i++) {
        if (!(nfc_uid_pool_mask & (1U << i))) {
            nfc_uid_pool_mask |= (1U << i);
            return &nfc_uid_pool[i];
        }
    }
    return NULL;
}
static void nfc_uid_pool_free(nfc_uid_evt_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_uid_pool;
    if (idx >= 0 && idx < NFC_UID_POOL_SIZE) {
        nfc_uid_pool_mask &= ~(1U << idx);
    }
}

// Async updater for PN532 UID/type summary lines
static void nfc_update_labels_async(void *ptr) {
    if (!ptr) return;
    nfc_uid_evt_t *ev = (nfc_uid_evt_t*)ptr;
    if (ev->session != nfc_scan_session) { nfc_uid_pool_free(ev); return; }
    if (!display_manager_is_available()) { nfc_uid_pool_free(ev); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { nfc_uid_pool_free(ev); return; }
    char uid_text[64]; int pos = 0; pos += snprintf(uid_text, sizeof(uid_text), "UID:");
    for (int i = 0; i < ev->uid_len && pos < (int)sizeof(uid_text) - 4; ++i) {
        pos += snprintf(uid_text + pos, sizeof(uid_text) - pos, " %02X", ev->uid[i]);
    }
    if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
        lv_label_set_text(nfc_uid_label, uid_text);
    }
    if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
        lv_label_set_text(nfc_type_label, "Type: ISO14443A");
    }
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    nfc_uid_pool_free(ev);
}

static void nfc_progress_update_async(void *ptr) {
    if (!ptr) return;
    dict_prog_t *dp = (dict_prog_t*)ptr;
    if (dp->s != nfc_scan_session) { nfc_dict_pool_free(dp); return; }
    if (!display_manager_is_available()) { nfc_dict_pool_free(dp); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { nfc_dict_pool_free(dp); return; }
    int percent = 0;
    if (dp->t > 0) percent = (dp->c * 100) / dp->t;
    if (percent > 100) percent = 100;
    if (percent < 0) percent = 0;
    char phase[40];
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        snprintf(phase, sizeof(phase), " | Sec %d Blk %d Key %c", mfc_phase_sector, mfc_phase_first_block, mfc_phase_key_b ? 'B' : 'A');
    } else {
        phase[0] = '\0';
    }
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        char title[80];
        bool bruteforce_active = (!nfc_paused && !nfc_cache_fill_phase && !nfc_dict_skip_requested && (mfc_phase_total > 0));
        if (nfc_paused) snprintf(title, sizeof(title), "Paused - present tag to continue");
        else if (nfc_cache_fill_phase) snprintf(title, sizeof(title), "Reading sectors... %d%%", percent);
        else if (nfc_dict_skip_requested) snprintf(title, sizeof(title), "Basic read (skipping dict) ...");
        else if (nfc_hardnested_phase) snprintf(title, sizeof(title), "Collecting nonces... %d%%", percent);
        else if (bruteforce_active) snprintf(title, sizeof(title), "Unlocking card... %d%%", percent);
        else if (nfc_details_ready) snprintf(title, sizeof(title), "%s", nfc_get_detected_title());
        else snprintf(title, sizeof(title), "Scanning NFC...");
        lv_label_set_text(nfc_title_label, title);
        if (nfc_details_ready && !bruteforce_active && !nfc_cache_fill_phase) lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    if (!nfc_paused && !nfc_cache_fill_phase && !nfc_dict_skip_requested && !nfc_details_ready && mfc_phase_total > 0 && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        if (!nfc_skip_label_applied) {
            lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
            nfc_more_visible = true;
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Skip");
            update_nfc_buttons_layout();
            update_nfc_popup_selection();
            nfc_skip_label_applied = true;
        }
    }
    if (mfc_phase_sector >= 0 && mfc_phase_first_block >= 0) {
        if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
            char l1[32];
            snprintf(l1, sizeof(l1), "Sec %d Blk %d", mfc_phase_sector, mfc_phase_first_block);
            lv_label_set_text(nfc_uid_label, l1);
        }
        if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
            char l2[16];
            snprintf(l2, sizeof(l2), "Key %c", mfc_phase_key_b ? 'B' : 'A');
            lv_label_set_text(nfc_type_label, l2);
        }
    }
    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        char info[96];
        if (dp->t > 0) snprintf(info, sizeof(info), "MFC progress: %d/%d (%d%%)%s", dp->c, dp->t, percent, phase);
        else snprintf(info, sizeof(info), "MFC progress: %d (unknown total)%s", dp->c, phase);
        lv_label_set_text(nfc_details_label, info);
        lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    nfc_dict_pool_free(dp);
}

static volatile bool nfc_scan_cancel = false;
static volatile bool nfc_save_in_progress = false;
static volatile bool nfc_attack_in_progress = false;
#ifdef CONFIG_NFC_ST25R3916
static volatile bool nfc_scan_picopass_only = false;
#endif

// Expose cancel status to MIFARE Classic layer (cooperative cancellation)
bool nfc_is_scan_cancelled(void) { return nfc_scan_cancel; }


#ifdef NFC_HAS_LOCAL_READER
static pn532_io_handle_t g_pn532 = NULL;
static pn532_io_t g_pn532_instance;
/* Kept separate from the UI reader so this facade never enters UI scan paths. */
static pn532_io_handle_t nfc_t2_extension_reader = NULL;
static TaskHandle_t nfc_t2_extension_task = NULL;
static volatile bool nfc_t2_extension_cancel = false;
static bool nfc_t2_extension_session = false;
static nfc_view_t2_tag_info_t nfc_t2_extension_info;
static uint8_t nfc_t2_extension_ndef[NFC_VIEW_T2_NDEF_MAX];
static size_t nfc_t2_extension_ndef_len = 0;
#endif
static TaskHandle_t nfc_scan_task_handle = NULL;
static char *nfc_details_text = NULL;
static char nfc_detected_title[64] = {0};
static uint32_t nfc_details_session = 0;
static uint8_t g_uid[10] = {0};
static uint8_t g_uid_len = 0;
static uint16_t g_atqa = 0;
static uint8_t g_sak = 0;
/* Cached EMV read so the Save button works without re-tapping the card. */
static EmvData g_emv;
static bool g_is_emv = false;
#ifdef NFC_HAS_LOCAL_READER
static NTAG2XX_MODEL g_model = NTAG2XX_UNKNOWN;
#endif


typedef struct {
    char *text;      // allocated details text
    size_t text_len; // length of text
    uint32_t session; // scan session
} ndef_details_result_t;

#define NFC_NDEF_POOL_SIZE 4
static ndef_details_result_t nfc_ndef_pool[NFC_NDEF_POOL_SIZE];
static uint32_t nfc_ndef_pool_mask = 0;
static ndef_details_result_t* nfc_ndef_pool_alloc(void) {
    for (int i = 0; i < NFC_NDEF_POOL_SIZE; i++) {
        if (!(nfc_ndef_pool_mask & (1U << i))) {
            nfc_ndef_pool_mask |= (1U << i);
            return &nfc_ndef_pool[i];
        }
    }
    return NULL;
}
static void nfc_ndef_pool_free(ndef_details_result_t *ptr) {
    if (!ptr) return;
    int idx = ptr - nfc_ndef_pool;
    if (idx >= 0 && idx < NFC_NDEF_POOL_SIZE) {
        nfc_ndef_pool_mask &= ~(1U << idx);
    }
}

static const char* nfc_get_detected_title(void) {
    return (nfc_detected_title[0] != '\0') ? nfc_detected_title : "NFC Tag";
}

bool nfc_api_get_last_uid(uint8_t *uid_out, uint8_t *uid_len_out) {
    if (!uid_out || !uid_len_out || g_uid_len == 0) return false;
    memcpy(uid_out, g_uid, g_uid_len);
    *uid_len_out = g_uid_len;
    return true;
}

static void nfc_update_title_from_details(const char *details) {
    if (!details) return;
    const char *card = strstr(details, "Card:");
    size_t prefix_len = 5;
    if (!card) {
        card = strstr(details, "Type:");
        prefix_len = 5;
    }
    if (!card) return;
    card += prefix_len;
    while (*card == ' ' || *card == '\t') card++;
    if (!*card) return;
    size_t idx = 0;
    while (card[idx] && card[idx] != '\n' && card[idx] != '|' && idx < sizeof(nfc_detected_title) - 1) {
        nfc_detected_title[idx] = card[idx];
        idx++;
    }
    nfc_detected_title[idx] = '\0';
}

static void nfc_set_details_async(void *ptr) {
    if (!ptr) return;
    ndef_details_result_t *res = (ndef_details_result_t *)ptr;
    if (res->session != nfc_scan_session) { if (res->text) free(res->text); nfc_ndef_pool_free(res); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { if (res->text) free(res->text); nfc_ndef_pool_free(res); return; }
    // Replace old details if any
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_text = res->text;
    nfc_details_ready = true;
    nfc_update_title_from_details(nfc_details_text);
    if (nfc_detected_title[0] == '\0') {
        snprintf(nfc_detected_title, sizeof(nfc_detected_title), "NFC Tag");
    }
    
    // Show success status when NFC scanning completes
    status_display_show_status("NFC Scan Done");
    
    // reset phase state and update summary labels to indicate completion
    mfc_phase_sector = -1;
    mfc_phase_first_block = -1;
    mfc_phase_total = 0;
    nfc_hardnested_phase = false;
    // Revert label back to More after bruteforce completes
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
        lv_obj_clear_state(nfc_scan_more_btn, LV_STATE_DISABLED);
    }
    // don't stomp the title here; let scan/progress or details phases set it to avoid flicker
    // If already showing details, update label
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, nfc_detected_title);
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    if (nfc_details_visible && nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_label_set_text(nfc_details_label, nfc_details_text);
        lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    // Reset dict-skip flag for next scans
    nfc_dict_skip_requested = false;
    nfc_skip_label_applied = false;

    // ensure the More button is available once details are ready (ntag and classic)
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
        lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
        if (lbl) lv_label_set_text(lbl, "More");
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }

    // Reveal Save button now that details (and cache) are ready
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && !nfc_save_visible) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }

    // Resume normal I2C activity now that scanning/bruteforce has finished
    display_manager_set_low_i2c_mode(false);

    // res is a slot in the static nfc_ndef_pool, not a heap allocation.
    // res->text ownership was transferred to nfc_details_text above, so don't free it here.
    nfc_ndef_pool_free(res);
}



#ifdef NFC_HAS_LOCAL_READER
#ifdef CONFIG_NFC_ST25R3916
/* Classify the credential the way the Flipper picopass read-success screen
 * does, from the booleans the parser already sets. SE/SIO and elite take
 * priority over a plain legacy/standard credential. */
static const char *picopass_card_type_str(const PicopassPacs *pacs) {
    if (pacs->se_enabled || pacs->sio) return "iCLASS SE";
    if (pacs->elite_kdf) return "iCLASS Elite";
    if (pacs->legacy) return "iCLASS Legacy";
    return "PicoPass";
}

// Render the read-success summary into `w`. Shared by the live scan path and
// the saved-.picopass viewer so both show identical info.
static void picopass_format_summary(const PicopassDeviceData *dev_data, char *w, size_t cap) {
    const PicopassPacs *pacs = &dev_data->pacs;

    /* Card type header. */
    int n = snprintf(w, cap, "%s\n", picopass_card_type_str(pacs));
    if (n > 0) { w += n; cap -= n; }

    /* Credential first: decoded Wiegand when available, else the raw block so
     * the user still sees something on non-26-bit / undecoded formats. */
    if (pacs->record.valid) {
        n = snprintf(w, cap, "Facility Code: %u\n", pacs->record.FacilityCode);
        if (n > 0) { w += n; cap -= n; }
        n = snprintf(w, cap, "Card Number: %u\n", pacs->record.CardNumber);
        if (n > 0) { w += n; cap -= n; }
        n = snprintf(w, cap, "Format: %u-bit\n", pacs->record.bitLength);
        if (n > 0) { w += n; cap -= n; }
    } else if (!picopass_is_memset(pacs->credential, 0x00, PICOPASS_BLOCK_LEN)) {
        n = snprintf(w, cap, "Credential:");
        if (n > 0) { w += n; cap -= n; }
        for (int i = 0; i < PICOPASS_BLOCK_LEN && cap > 3; i++) {
            n = snprintf(w, cap, " %02X", pacs->credential[i]);
            if (n > 0) { w += n; cap -= n; }
        }
        n = snprintf(w, cap, "\n");
        if (n > 0) { w += n; cap -= n; }
    }

    /* CSN */
    n = snprintf(w, cap, "CSN:");
    if (n > 0) { w += n; cap -= n; }
    for (int i = 0; i < PICOPASS_UID_LEN && cap > 3; i++) {
        n = snprintf(w, cap, " %02X", dev_data->AA1[PICOPASS_CSN_BLOCK_INDEX].data[i]);
        if (n > 0) { w += n; cap -= n; }
    }
    n = snprintf(w, cap, "\n");
    if (n > 0) { w += n; cap -= n; }
}

static void nfc_build_and_set_details_picopass(PicopassDeviceData *dev_data) {
    char *text = (char *)malloc(512);
    if (!text) return;
    picopass_format_summary(dev_data, text, 512);

    ndef_details_result_t *res = nfc_ndef_pool_alloc();
    if (!res) { free(text); return; }
    res->text = text;
    res->text_len = strlen(text);
    res->session = nfc_scan_session;
    if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
    else { free(text); nfc_ndef_pool_free(res); }
}

// Parse the `Block N: XX XX ...` lines of a Flipper .picopass file back into
// AA1 blocks, then derive PACS the same way the live read path does. Returns
// false if the file has no parseable blocks. (Assumes PICOPASS_BLOCK_LEN == 8,
// matching the Flipper block width.)
static bool picopass_load_and_parse_file(const char *path, PicopassDeviceData *out) {
    FILE *pf = fopen(path, "r");
    if (!pf) return false;
    memset(out, 0, sizeof(*out));
    char line[256];
    bool any = false;
    while (fgets(line, sizeof(line), pf)) {
        int idx = -1;
        if (sscanf(line, "Block %d:", &idx) != 1) continue;
        if (idx < 0 || idx >= PICOPASS_MAX_APP_LIMIT) continue;
        const char *p = strchr(line, ':');
        if (!p) continue;
        unsigned int b[PICOPASS_BLOCK_LEN];
        if (sscanf(p + 1, " %x %x %x %x %x %x %x %x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != PICOPASS_BLOCK_LEN)
            continue;
        for (int j = 0; j < PICOPASS_BLOCK_LEN; j++) out->AA1[idx].data[j] = (uint8_t)b[j];
        any = true;
    }
    fclose(pf);
    if (!any) return false;

    picopass_parse_credential(out->AA1, &out->pacs);
    /* legacy/SE flags are set by picopass_detect(), not parse_credential, so
     * reproduce them here from the app-issuer area (block 5). */
    out->pacs.legacy = picopass_is_memset(out->AA1[5].data, 0xFF, PICOPASS_BLOCK_LEN);
    out->pacs.se_enabled = (memcmp(out->AA1[5].data, "\xff\xff\xff\x00\x06\xff\xff\xff", 8) == 0);
    if (!picopass_is_memset(out->pacs.credential, 0x00, PICOPASS_BLOCK_LEN)) {
        picopass_parse_wiegand(out->pacs.credential, &out->pacs.record);
    }
    return true;
}
#endif

static void nfc_build_and_set_details(pn532_io_handle_t io, const uint8_t *uid, uint8_t uid_len) {
    // Prefer MIFARE Classic summary if SAK indicates Classic
    if (mfc_is_classic_sak(g_sak)) {
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        // Early exit for Banshee: show basic info only to avoid NFC instability
        // Commented out to allow MFC dict attack
        // size_t cap = 256;
        // ndef_details_result_t *res = (ndef_details_result_t*)malloc(sizeof(*res));
        // if (!res) return;
        // res->text = (char*)malloc(cap);
        // if (!res->text) { free(res); return; }
        // res->text_len = cap; res->session = nfc_scan_session;
        // char *w = res->text;
        // snprintf(w, cap, "MIFARE Classic\nUID:");
        // size_t used = strlen(w); w += used; cap -= used;
        // for (uint8_t i = 0; i < uid_len && cap > 3; ++i) {
        //     int n = snprintf(w, cap, " %02X", uid[i]);
        //     if (n > 0) { w += n; cap -= n; }
        // }
        // snprintf(w, cap, "\nATQA: %04X SAK: %02X", g_atqa, g_sak);
        // // snprintf(w, cap, "\nATQA: %04X SAK: %02X\nNFC unstable on Banshee", g_atqa, g_sak);
        // if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
        // else { if (res->text) free(res->text); free(res); }
        // return;
    }

        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "Unlocking card... 0%");
        // Reduce I2C contention during PN532 scanning/bruteforce
        display_manager_set_low_i2c_mode(true);
        mfc_user_dict_begin_batch();
        char *text = mfc_build_details_summary(io, uid, uid_len, g_atqa, g_sak);
        mfc_user_dict_end_batch();  // persist all keys found this attack in one mount
        // Check if scan was cancelled during MIFARE processing (e.g., while paused)
        if (nfc_scan_cancel || !text) {
            if (text) free(text);
            mfc_set_progress_callback(NULL, NULL);
            return;
        }

        uint8_t known_block = 0, target_block = 0, known_key[6] = {0};
        bool known_key_b = false, target_key_b = false;
        if (!nfc_scan_cancel && mfc_has_unread_blocks() &&
            mfc_get_hardnested_defaults(&known_block, &known_key_b, known_key, &target_block, &target_key_b)) {
            nfc_hardnested_phase = true;  // progress handler now shows "Collecting nonces..."
            if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
                lv_label_set_text(nfc_title_label, "Collecting nonces... 0%");
            }
            char path[192] = {0};
            /* No outer mount held: the capture self-mounts per chunk (see
             * mfc_nested_write_sd) so the display stays live during the long
             * RF collection instead of freezing for the whole capture. */
            bool ok = mfc_hardnested_capture_missing_file(io, uid, uid_len, g_atqa, g_sak,
                                                          4096, "/mnt/ghostesp/nfc",
                                                          path, sizeof(path));
            nfc_hardnested_phase = false;
            const char *line = ok ? "Nested log: /nfc/.nested.log\n" : "Nested log: capture incomplete\n";
            size_t old_len = strlen(text);
            size_t line_len = strlen(line);
            char *with_log = (char*)malloc(old_len + line_len + 1);
            if (with_log) {
                memcpy(with_log, text, old_len);
                memcpy(with_log + old_len, line, line_len + 1);
                free(text);
                text = with_log;
            }
        }

        ndef_details_result_t *res = nfc_ndef_pool_alloc();
        if (!res) { free(text); return; }
        res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
        if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
        else { free(text); nfc_ndef_pool_free(res); }
        mfc_set_progress_callback(NULL, NULL);
        return;
    }

    // Try DESFire summary (Type 4) when ATQA/SAK look like DESFire
    if (desfire_is_desfire_candidate(g_atqa, g_sak)) {
        desfire_version_t ver;
        bool have_ver = desfire_get_version(io, &ver);

        // If GET_VERSION failed, this might be an EMV payment card
        // (same ATQA/SAK, different protocol).
        if (!have_ver) {
            EmvData emv;
            memset(&emv, 0, sizeof(emv));
            if (emv_read_card(io, &emv)) {
                g_emv = emv;       /* cache for the Save button */
                g_is_emv = true;
                char *plugin_text = flipper_nfc_try_parse_emv(&emv);
                if (plugin_text) {
                    size_t cap = 512;
                    char *text = (char *)malloc(cap);
                    if (text) {
                        snprintf(text, cap, "Type: EMV Payment Card\nUID:");
                        size_t pos = strlen(text);
                        for (uint8_t i = 0; i < uid_len && pos < cap - 4; ++i) {
                            pos += snprintf(text + pos, cap - pos, " %02X", uid[i]);
                        }
                        pos += snprintf(text + pos, cap - pos,
                                        "\nATQA: %02X %02X  SAK: %02X\n",
                                        (g_atqa >> 8) & 0xFF, g_atqa & 0xFF, g_sak);

                        ndef_details_result_t *res = nfc_ndef_pool_alloc();
                        if (!res) { free(text); free(plugin_text); return; }
                        size_t h_len = strlen(text);
                        size_t p_len = strlen(plugin_text);
                        char *combined = (char *)malloc(h_len + 1 + p_len + 1);
                        if (combined) {
                            memcpy(combined, text, h_len);
                            combined[h_len] = '\n';
                            memcpy(combined + h_len + 1, plugin_text, p_len + 1);
                            free(text);
                            free(plugin_text);
                            text = combined;
                        } else {
                            free(plugin_text);
                        }
                        res->text = text;
                        res->text_len = strlen(text);
                        res->session = nfc_scan_session;
                        if (display_manager_is_available())
                            display_manager_lvgl_async_call(nfc_set_details_async, res);
                        else { free(text); nfc_ndef_pool_free(res); }
                        return;
                    }
                    free(plugin_text);
                }
            }
            // Neither DESFire nor EMV — show minimal Type 4 info
            char *text = desfire_build_details_summary(NULL, uid, uid_len, g_atqa, g_sak);
            if (!text) return;
            ndef_details_result_t *res = nfc_ndef_pool_alloc();
            if (!res) { free(text); return; }
            res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
            if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
            else { free(text); nfc_ndef_pool_free(res); }
            return;
        }

        const desfire_version_t *ver_ptr = have_ver ? &ver : NULL;
        char *text = desfire_build_details_summary(ver_ptr, uid, uid_len, g_atqa, g_sak);
        if (!text) {
            return;
        }

        // Walk the DESFire application/file tree (plaintext files only) and
        // let any registered supported-card parser (e.g. myki) annotate the
        // summary with card-specific info.
        if (have_ver) {
            MfDesfireData *tree = desfire_tree_alloc();
            if (tree) {
                if (desfire_read_tree(io, tree)) {
                    char *plugin_text = flipper_nfc_try_parse_mfdesfire(tree);
                    if (plugin_text) {
                        size_t h_len = strlen(text);
                        size_t p_len = strlen(plugin_text);
                        char *combined = (char *)malloc(h_len + 1 + p_len + 1);
                        if (combined) {
                            memcpy(combined, text, h_len);
                            combined[h_len] = '\n';
                            memcpy(combined + h_len + 1, plugin_text, p_len + 1);
                            free(text);
                            text = combined;
                        }
                        free(plugin_text);
                    }
                }
                desfire_tree_free(tree);
            }
        }

        ndef_details_result_t *res = nfc_ndef_pool_alloc();
        if (!res) {
            free(text);
            return;
        }
        res->text = text;
        res->text_len = strlen(text);
        res->session = nfc_scan_session;
        if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
        else {
            free(text);
            nfc_ndef_pool_free(res);
        }
        return;
    }

    // ISO14443-4 fallback: SAK bit 5 set but not caught by the strict DESFire
    // ATQA check above (e.g. EMV payment cards with ATQA 0x0004, 0x0002, etc.).
    // Try DESFire GET_VERSION first, then EMV SELECT PPSE.
    if (desfire_sak_is_iso14443_4(g_sak) && !desfire_is_desfire_candidate(g_atqa, g_sak)) {
        desfire_version_t ver;
        bool have_ver = desfire_get_version(io, &ver);

        if (!have_ver) {
            // Not a DESFire — try EMV payment card
            EmvData emv;
            memset(&emv, 0, sizeof(emv));
            if (emv_read_card(io, &emv)) {
                g_emv = emv;       /* cache for the Save button */
                g_is_emv = true;
                char *plugin_text = flipper_nfc_try_parse_emv(&emv);
                if (plugin_text) {
                    size_t cap = 512;
                    char *text = (char *)malloc(cap);
                    if (text) {
                        snprintf(text, cap, "Type: EMV Payment Card\nUID:");
                        size_t pos = strlen(text);
                        for (uint8_t i = 0; i < uid_len && pos < cap - 4; ++i)
                            pos += snprintf(text + pos, cap - pos, " %02X", uid[i]);
                        pos += snprintf(text + pos, cap - pos,
                                        "\nATQA: %02X %02X  SAK: %02X\n",
                                        (g_atqa >> 8) & 0xFF, g_atqa & 0xFF, g_sak);
                        size_t h_len = strlen(text);
                        size_t p_len = strlen(plugin_text);
                        char *combined = (char *)malloc(h_len + 1 + p_len + 1);
                        if (combined) {
                            memcpy(combined, text, h_len);
                            combined[h_len] = '\n';
                            memcpy(combined + h_len + 1, plugin_text, p_len + 1);
                            free(text);
                            text = combined;
                        }
                        free(plugin_text);
                        ndef_details_result_t *res = nfc_ndef_pool_alloc();
                        if (!res) { free(text); return; }
                        res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
                        if (display_manager_is_available())
                            display_manager_lvgl_async_call(nfc_set_details_async, res);
                        else { free(text); nfc_ndef_pool_free(res); }
                        return;
                    }
                    free(plugin_text);
                }
            }
        }
        // DESFire GET_VERSION succeeded but not ATQA=0x0344 — show as generic Type 4
        if (have_ver) {
            char *text = desfire_build_details_summary(&ver, uid, uid_len, g_atqa, g_sak);
            if (!text) return;
            ndef_details_result_t *res = nfc_ndef_pool_alloc();
            if (!res) { free(text); return; }
            res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
            if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
            else { free(text); nfc_ndef_pool_free(res); }
            return;
        }
    }

    // Otherwise try NTAG/Ultralight (Type 2)
    uint8_t *mem = NULL; size_t mem_len = 0; NTAG2XX_MODEL model = NTAG2XX_UNKNOWN;
    ntag_t2_info_t t2_info = {0};
    bool have_t2_info = ntag_t2_read_user_memory_fast(io, &mem, &mem_len, &t2_info);
    model = t2_info.model;
    if (!have_t2_info) {
        size_t cap = 256;
        ndef_details_result_t *res = nfc_ndef_pool_alloc();
        if (!res) return;
        res->text = (char*)malloc(cap);
        res->text_len = cap; res->session = nfc_scan_session;
        if (!res->text) { nfc_ndef_pool_free(res); return; }
        char *w = res->text; snprintf(w, cap, "UID:"); size_t used = strlen(w); w += used; cap -= used;
        for (uint8_t i = 0; i < uid_len && cap > 3; ++i) { int n = snprintf(w, cap, " %02X", uid[i]); if (n > 0) { w += n; cap -= n; } }
        snprintf(w, cap, "\nNo NDEF data\n");
        if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
        else { free(res->text); nfc_ndef_pool_free(res); }
        return;
    }
    char *text = ntag_t2_build_details_from_mem_info(mem, mem_len, uid, uid_len, &t2_info);
    free(mem);
    if (!text) return;
    g_model = model;
    snprintf(nfc_detected_title, sizeof(nfc_detected_title), "%s", ntag_t2_model_str(model));
    ndef_details_result_t *res = nfc_ndef_pool_alloc();
    if (!res) { free(text); return; }
    res->text = text; res->text_len = strlen(text); res->session = nfc_scan_session;
    if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_set_details_async, res);
    else { if (res->text) free(res->text); nfc_ndef_pool_free(res); }
    return;
}
#endif

// backend switch helper
static bool using_chameleon_backend(void) {
#if defined(CONFIG_NFC_CHAMELEON)
    return chameleon_manager_is_ready();
#else
    return false;
#endif
}

#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
// The backend row is an iOS-style toggle: off = PN532, on = ST25R3916. The
// choice is persisted in NVS by nfc_backend_set(). AUTO (if ever set via CLI)
// reads as off here so the toggle always shows a concrete backend.
static bool nfc_backend_is_st25r(void) {
    return nfc_backend_get() == NFC_BACKEND_ST25R3916;
}

static void nfc_backend_item_text(char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    snprintf(out, out_len, "%s", nfc_backend_is_st25r() ? "Using ST25R" : "Using PN532");
}

static lv_obj_t *nfc_backend_find_toggle(void) {
    if (!backend_btn || !lv_obj_is_valid(backend_btn)) return NULL;
    uint32_t n = lv_obj_get_child_cnt(backend_btn);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(backend_btn, i);
        if (child && lv_obj_get_user_data(child) == IOS_TOGGLE_USER_DATA) return child;
    }
    return NULL;
}

static void nfc_update_backend_item(bool animate) {
    if (!backend_btn || !lv_obj_is_valid(backend_btn)) return;
    lv_obj_t *label = lv_obj_get_child(backend_btn, 0);
    if (label) {
        char text[32];
        nfc_backend_item_text(text, sizeof(text));
        lv_label_set_text(label, text);
    }
    lv_obj_t *toggle = nfc_backend_find_toggle();
    if (toggle) ios_toggle_set_value(toggle, nfc_backend_is_st25r(), animate);
}

static void nfc_backend_event_cb(lv_event_t *e) {
    nfc_backend_set(nfc_backend_is_st25r() ? NFC_BACKEND_PN532 : NFC_BACKEND_ST25R3916);
    // Touch events carry `e` (animate the knob); encoder/keyboard events pass
    // NULL, so snap without animation the same way the IR settings rows do.
    nfc_update_backend_item(e != NULL);
}

static lv_obj_t *nfc_add_backend_item(void) {
    char text[32];
    nfc_backend_item_text(text, sizeof(text));
    backend_btn = options_view_add_item(g_nfc_ov, text, nfc_backend_event_cb, NULL);
    if (backend_btn) {
        lv_obj_set_flex_flow(backend_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(backend_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t *label = lv_obj_get_child(backend_btn, 0);
        if (label) {
            lv_obj_set_flex_grow(label, 1);
            lv_obj_set_width(label, LV_SIZE_CONTENT);
        }
        lv_obj_t *toggle = ios_toggle_create(backend_btn);
        lv_obj_update_layout(backend_btn);
        ios_toggle_set_value(toggle, nfc_backend_is_st25r(), false);
    }
    return backend_btn;
}
#endif

// chameleon ultra scan result -> ui
typedef struct {
    uint32_t session;
    uint8_t uid[10];
    uint8_t uid_len;
    uint16_t atqa;
    uint8_t sak;
} cu_scan_result_t;

#if defined(CONFIG_NFC_CHAMELEON)
static void nfc_refresh_cu_details_from_cache(void) {
    uint32_t current = chameleon_manager_get_cached_details_session();
    if (nfc_details_ready && nfc_details_text && nfc_details_session == current) {
        return;
    }
    if (nfc_details_text) {
        free(nfc_details_text);
        nfc_details_text = NULL;
    }
    const char *cached = chameleon_manager_get_cached_details();
    if (cached) {
        nfc_details_text = strdup(cached);
        nfc_details_ready = (nfc_details_text != NULL);
        if (nfc_details_ready) {
            nfc_update_title_from_details(nfc_details_text);
        }
    } else {
        nfc_details_ready = false;
        nfc_detected_title[0] = '\0';
    }
    nfc_details_session = current;

    if (nfc_details_ready) {
        // Reset dict/skip state now that details are ready (mirror PN532 flow)
        mfc_phase_sector = -1;
        mfc_phase_first_block = -1;
        mfc_phase_total = 0;
        nfc_dict_skip_requested = false;
        nfc_skip_label_applied = false;

        // Ensure the More button is restored after any Skip state
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
            nfc_more_visible = true;
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "More");
            lv_obj_clear_state(nfc_scan_more_btn, LV_STATE_DISABLED);
        }

        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, nfc_get_detected_title());
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
        }
        // Refresh layout/selection now that button set has changed
        update_nfc_buttons_layout();
        update_nfc_popup_selection();
    }
}
#endif

// Static pool for Chameleon scan results to eliminate malloc
#define CU_SCAN_POOL_SIZE 4
static cu_scan_result_t cu_scan_pool[CU_SCAN_POOL_SIZE];
static uint32_t cu_scan_pool_mask = 0;

static cu_scan_result_t* cu_scan_pool_alloc(void) {
    for (int i = 0; i < CU_SCAN_POOL_SIZE; i++) {
        if (!(cu_scan_pool_mask & (1U << i))) {
            cu_scan_pool_mask |= (1U << i);
            return &cu_scan_pool[i];
        }
    }
    return NULL;
}
static void cu_scan_pool_free(cu_scan_result_t *ptr) {
    if (!ptr) return;
    int idx = ptr - cu_scan_pool;
    if (idx >= 0 && idx < CU_SCAN_POOL_SIZE) {
        cu_scan_pool_mask &= ~(1U << idx);
    }
}

static void nfc_set_cu_scan_async(void *ptr) {
    cu_scan_result_t *r = (cu_scan_result_t*)ptr;
    if (!r) return;
    if (r->session != nfc_scan_session) { cu_scan_pool_free(r); return; }
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) { cu_scan_pool_free(r); return; }
    // Update summary labels
    if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
        char uid_text[64]; int pos = 0; pos += snprintf(uid_text, sizeof(uid_text), "UID:");
        for (int i = 0; i < r->uid_len && pos < (int)sizeof(uid_text) - 4; ++i) pos += snprintf(uid_text + pos, sizeof(uid_text) - pos, " %02X", r->uid[i]);
        lv_label_set_text(nfc_uid_label, uid_text);
    }
    if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
        lv_label_set_text(nfc_type_label, "Type: ISO14443A");
    }
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        const char *title = "NFC Tag";
        if (desfire_is_desfire_candidate(r->atqa, r->sak)) {
            title = desfire_model_str(DESFIRE_MODEL_UNKNOWN);
        }
        lv_label_set_text(nfc_title_label, title);
        lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
    }
    // Reveal buttons: More and Save
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        lv_obj_clear_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_more_visible = true;
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
        nfc_save_visible = true;
    }
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
    cu_scan_pool_free(r);
}

// cu worker tasks
static void nfc_scan_cu_task(void *arg) {
    (void)arg;
    if (nfc_scan_cancel) { vTaskDelete(NULL); return; }
    bool ok = chameleon_manager_scan_hf();
    if (ok && !nfc_scan_cancel) {
        uint8_t uid[10] = {0}; uint8_t ul = 0; uint16_t atqa = 0; uint8_t sak = 0;
        if (chameleon_manager_get_last_hf_scan(uid, &ul, &atqa, &sak)) {
            cu_scan_result_t *res = cu_scan_pool_alloc();
            if (res) {
                res->session = nfc_scan_session;
                res->uid_len = ul; if (ul > sizeof(res->uid)) res->uid_len = sizeof(res->uid);
                memcpy(res->uid, uid, res->uid_len);
                res->atqa = atqa; res->sak = sak;
                display_manager_lvgl_async_call(nfc_set_cu_scan_async, res);
            }
            // If MIFARE Classic (0x08/0x18/0x09), perform dict-based read on CU
#if defined(CONFIG_NFC_CHAMELEON)
            if (sak == 0x08 || sak == 0x18 || sak == 0x09) {
                chameleon_manager_set_attack_hooks(&nfc_ui_attack_hooks);
                chameleon_manager_set_progress_callback(mfc_dict_progress_cb, NULL);
                (void)chameleon_manager_mf1_read_classic_with_dict(false);
                // Refresh details text from CU cache
                nfc_refresh_cu_details_from_cache();
                // Ensure Save button is visible
                if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
                    lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
                    nfc_save_visible = true;
                    update_nfc_buttons_layout();
                    update_nfc_popup_selection();
                }
            }
#endif
        }
    }
    vTaskDelete(NULL);
}

static void nfc_save_cu_task(void *arg) {
    (void)arg;
    bool ok = false;
    // Prefer cached NTAG dump (works without tag present)
    if (chameleon_manager_has_cached_ntag_dump()) {
        ok = chameleon_manager_save_ntag_dump(NULL);
    } else if (chameleon_manager_last_scan_is_ntag()) {
        // Fallback: try to read now if possible
        if (chameleon_manager_read_ntag_card()) ok = chameleon_manager_save_ntag_dump(NULL);
    } else if (chameleon_manager_mf1_has_cache()) {
        ok = chameleon_manager_mf1_save_flipper_dump(NULL);
    } else {
        // Try reading Classic now, then save
        (void)chameleon_manager_mf1_read_classic_with_dict(false);
        if (chameleon_manager_mf1_has_cache()) ok = chameleon_manager_mf1_save_flipper_dump(NULL);
    }
    if (!ok) {
        ok = chameleon_manager_save_last_hf_scan(NULL);
    }
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(nfc_save_done_async, res); }
    else { display_manager_lvgl_async_call(nfc_save_done_async, NULL); }
    nfc_save_in_progress = false;
    vTaskDelete(NULL);
}

#ifdef NFC_HAS_LOCAL_READER
static bool nfc_init_local_reader_st25r(const char *tag) {
#ifdef CONFIG_NFC_ST25R3916
    g_pn532 = &g_pn532_instance;
#ifdef CONFIG_NFC_ST25R3916_SPI
    ESP_LOGI(tag, "attempting ST25R3916 on SPI host %d", CONFIG_NFC_ST25R3916_SPI_HOST);
    if (st25r3916_new_driver_spi(
            CONFIG_NFC_ST25R3916_SPI_HOST,
            (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_MOSI_PIN,
            (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_MISO_PIN,
            (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_SCLK_PIN,
            (gpio_num_t)CONFIG_NFC_ST25R3916_SPI_CS_PIN,
            (gpio_num_t)CONFIG_NFC_RST_PIN,
            (gpio_num_t)CONFIG_NFC_IRQ_PIN,
            CONFIG_NFC_ST25R3916_SPI_CLOCK_HZ,
            g_pn532) != ESP_OK) {
        ESP_LOGE(tag, "st25r3916_new_driver_spi failed");
        g_pn532 = NULL;
        return false;
    }
#else
    ESP_LOGI(tag, "attempting ST25R3916 on I2C");
    if (st25r3916_new_driver_i2c(
            (gpio_num_t)CONFIG_NFC_SDA_PIN,
            (gpio_num_t)CONFIG_NFC_SCL_PIN,
            (gpio_num_t)CONFIG_NFC_RST_PIN,
            (gpio_num_t)CONFIG_NFC_IRQ_PIN,
            CONFIG_NFC_ST25R3916_I2C_PORT,
            CONFIG_NFC_ST25R3916_I2C_ADDR,
            g_pn532) != ESP_OK) {
        ESP_LOGE(tag, "st25r3916_new_driver_i2c failed");
        g_pn532 = NULL;
        return false;
    }
#endif
    if (st25r3916_adapter_init(g_pn532) == ESP_OK) {
        pn532_set_passive_activation_retries(g_pn532, 0xFF);
        ESP_LOGI(tag, "ST25R3916 initialized");
        return true;
    }
    ESP_LOGE(tag, "ST25R3916 init failed");
    pn532_release(g_pn532);
    pn532_delete_driver(g_pn532);
    g_pn532 = NULL;
    return false;
#else
    (void)tag;
    return false;
#endif
}

static bool nfc_init_local_reader_pn532(const char *tag) {
#ifdef CONFIG_NFC_PN532
    g_pn532 = &g_pn532_instance;
    // Prefer a single I2C controller for all devices sharing the same pins.
    // Match the Fuel Gauge manager's chosen port by target to avoid two controllers
    // driving the same physical SDA/SCL.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
    i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
#elif defined(I2C_NUM_1)
    i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_1 };
#else
    i2c_port_t try_ports[2] = { I2C_NUM_0, I2C_NUM_0 };
#endif
    for (int pi = 0; pi < 2; ++pi) {
        i2c_port_t port = try_ports[pi];
        ESP_LOGI(tag, "attempting PN532 on I2C port %d", (int)port);
        if (pn532_new_driver_i2c(
                (gpio_num_t)CONFIG_NFC_SDA_PIN,
                (gpio_num_t)CONFIG_NFC_SCL_PIN,
                (gpio_num_t)CONFIG_NFC_RST_PIN,
                (gpio_num_t)CONFIG_NFC_IRQ_PIN,
                port,
                g_pn532) != ESP_OK) {
            ESP_LOGE(tag, "pn532_new_driver_i2c failed (port=%d)", (int)port);
            pn532_delete_driver(g_pn532);
            continue;
        }
        if (pn532_init(g_pn532) == ESP_OK) {
            pn532_set_passive_activation_retries(g_pn532, 0xFF);
            ESP_LOGI(tag, "PN532 initialized on port %d", (int)port);
            return true;
        }
        ESP_LOGE(tag, "pn532_init failed (port=%d)", (int)port);
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
    }
    g_pn532 = NULL;
    return false;
#else
    (void)tag;
    return false;
#endif
}

static bool nfc_init_local_reader(const char *tag) {
    if (g_pn532) return true;

    nfc_backend_t backend = nfc_backend_get();
    if (backend == NFC_BACKEND_PN532) return nfc_init_local_reader_pn532(tag);
    if (backend == NFC_BACKEND_ST25R3916) return nfc_init_local_reader_st25r(tag);

    if (nfc_init_local_reader_pn532(tag)) return true;
    return nfc_init_local_reader_st25r(tag);
}

static void nfc_t2_extension_release_reader(void) {
    if (nfc_t2_extension_reader) {
        pn532_release(nfc_t2_extension_reader);
        pn532_delete_driver(nfc_t2_extension_reader);
        nfc_t2_extension_reader = NULL;
    }
}

static bool nfc_t2_extension_poll_uid(uint8_t uid[10], uint8_t *uid_len) {
    if (!nfc_t2_extension_reader || !uid || !uid_len) return false;
    uint16_t atqa = 0;
    uint8_t sak = 0;
    uint8_t len = 0;
    if (pn532_read_passive_target_id_ex(nfc_t2_extension_reader, 0x00, uid, &len,
                                        &atqa, &sak, 300) != ESP_OK ||
        len == 0 || len > 10) {
        return false;
    }
    *uid_len = len;
    return true;
}

static void nfc_t2_extension_emit_tag(void) {
    char event[64];
    int pos = snprintf(event, sizeof(event), "%s|", nfc_t2_extension_info.model);
    for (uint8_t i = 0; i < nfc_t2_extension_info.uid_len && pos >= 0 &&
                        pos < (int)sizeof(event) - 3; i++) {
        pos += snprintf(event + pos, sizeof(event) - (size_t)pos, "%02X",
                        nfc_t2_extension_info.uid[i]);
    }
    if (pos >= 0) ghostscript_emit_event_escaped("nfc_tag", event);
}

static void nfc_t2_extension_scan_task(void *arg) {
    (void)arg;
    if (!nfc_init_local_reader("NFCT2Extension")) goto done;

    /* nfc_init_local_reader owns the UI handle; transfer it before polling. */
    nfc_t2_extension_reader = g_pn532;
    g_pn532 = NULL;
    while (!nfc_t2_extension_cancel) {
        uint8_t uid[10] = {0};
        uint8_t uid_len = 0;
        if (!nfc_t2_extension_poll_uid(uid, &uid_len)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t *memory = NULL;
        size_t memory_len = 0;
        ntag_t2_info_t tag_info = {0};
        if (!ntag_t2_read_user_memory_fast(nfc_t2_extension_reader, &memory,
                                           &memory_len, &tag_info)) {
            free(memory);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* A Type-2 NDEF tag must advertise a valid capability container. */
        if (!tag_info.cc_valid || tag_info.user_bytes == 0) {
            free(memory);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Fast reads provide the payload; the full info read supplies lock/auth state. */
        if (!ntag_t2_read_info(nfc_t2_extension_reader, &tag_info)) {
            free(memory);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (nfc_t2_extension_cancel) {
            free(memory);
            break;
        }

        size_t ndef_offset = 0;
        size_t ndef_len = 0;
        bool ndef_present = ntag_t2_find_ndef(memory, memory_len, &ndef_offset, &ndef_len);
        if (ndef_present && (ndef_len > NFC_VIEW_T2_NDEF_MAX ||
                             ndef_offset > memory_len || ndef_len > memory_len - ndef_offset)) {
            free(memory);
            continue;
        }

        memset(&nfc_t2_extension_info, 0, sizeof(nfc_t2_extension_info));
        memcpy(nfc_t2_extension_info.uid, uid, uid_len);
        nfc_t2_extension_info.uid_len = uid_len;
        snprintf(nfc_t2_extension_info.model, sizeof(nfc_t2_extension_info.model), "%s",
                 ntag_t2_model_str(tag_info.model));
        nfc_t2_extension_info.user_bytes = tag_info.user_bytes;
        nfc_t2_extension_info.ndef_present = ndef_present;
        nfc_t2_extension_info.ndef_length = ndef_present ? (uint16_t)ndef_len : 0;
        nfc_t2_extension_info.read_only = tag_info.cc_read_only;
        nfc_t2_extension_info.password_protected = tag_info.password_protected;
        nfc_t2_extension_info.static_locked = tag_info.static_locked;
        nfc_t2_extension_info.dynamic_locked = tag_info.dynamic_locked;
        nfc_t2_extension_ndef_len = ndef_present ? ndef_len : 0;
        if (ndef_present) memcpy(nfc_t2_extension_ndef, memory + ndef_offset, ndef_len);
        free(memory);

        nfc_t2_extension_session = true;
        nfc_t2_extension_emit_tag();
        break;
    }

done:
    if (!nfc_t2_extension_session) nfc_t2_extension_release_reader();
    nfc_t2_extension_task = NULL;
    vTaskDelete(NULL);
}

bool nfc_view_t2_scan_start(void) {
    /* Never contend with the UI scanner or a reader it retains for UI writes. */
    if (nfc_scan_task_handle || g_pn532 || nfc_t2_extension_task ||
        nfc_t2_extension_reader || nfc_t2_extension_session) {
        return false;
    }
    nfc_t2_extension_cancel = false;
    nfc_t2_extension_ndef_len = 0;
    memset(&nfc_t2_extension_info, 0, sizeof(nfc_t2_extension_info));
    if (xTaskCreate(nfc_t2_extension_scan_task, "nfc_t2_scan", 4096, NULL, 5,
                    &nfc_t2_extension_task) != pdPASS) {
        nfc_t2_extension_task = NULL;
        return false;
    }
    return true;
}

bool nfc_view_t2_scan_stop(void) {
    bool had_session = nfc_t2_extension_task || nfc_t2_extension_reader || nfc_t2_extension_session;
    nfc_t2_extension_cancel = true;
    uint32_t waited_ms = 0;
    while (nfc_t2_extension_task && waited_ms < 2000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (nfc_t2_extension_task) return false;
    nfc_t2_extension_release_reader();
    nfc_t2_extension_session = false;
    nfc_t2_extension_ndef_len = 0;
    memset(&nfc_t2_extension_info, 0, sizeof(nfc_t2_extension_info));
    return had_session;
}

bool nfc_view_t2_scan_active(void) {
    return nfc_t2_extension_task != NULL;
}

bool nfc_view_t2_read(nfc_view_t2_tag_info_t *out_info, uint8_t *ndef_out,
                      size_t max_ndef_bytes, size_t *ndef_bytes_out) {
    if (!nfc_t2_extension_session || !nfc_t2_extension_reader || !out_info) return false;
    *out_info = nfc_t2_extension_info;
    size_t copied = nfc_t2_extension_ndef_len;
    if (copied > max_ndef_bytes) copied = max_ndef_bytes;
    if (copied > 0 && !ndef_out) return false;
    if (copied > 0) memcpy(ndef_out, nfc_t2_extension_ndef, copied);
    if (ndef_bytes_out) *ndef_bytes_out = copied;
    return true;
}

bool nfc_view_t2_write_ndef(const uint8_t *ndef, size_t ndef_len) {
    if (!nfc_t2_extension_session || !nfc_t2_extension_reader ||
        (!ndef && ndef_len != 0) || ndef_len > NFC_VIEW_T2_NDEF_MAX ||
        nfc_t2_extension_info.read_only || nfc_t2_extension_info.password_protected ||
        nfc_t2_extension_info.static_locked || nfc_t2_extension_info.dynamic_locked) {
        return false;
    }

    size_t tlv_len = ndef_len <= 254 ? ndef_len + 3 : ndef_len + 5;
    if (tlv_len > nfc_t2_extension_info.user_bytes) return false;

    uint8_t uid[10] = {0};
    uint8_t uid_len = 0;
    if (!nfc_t2_extension_poll_uid(uid, &uid_len) ||
        uid_len != nfc_t2_extension_info.uid_len ||
        memcmp(uid, nfc_t2_extension_info.uid, uid_len) != 0) {
        return false;
    }

    uint8_t last_page = nfc_t2_extension_info.user_bytes >= 4
                            ? (uint8_t)(3 + nfc_t2_extension_info.user_bytes / 4) : 0;
    if (last_page < 4) return false;
    ntag_file_image_t image = {0};
    image.model = NTAG2XX_UNKNOWN;
    image.uid_len = uid_len;
    memcpy(image.uid, uid, uid_len);
    image.first_user_page = 4;
    image.pages_total = 4 + (int)((tlv_len + 3) / 4);
    image.full_pages = calloc((size_t)image.pages_total, 4);
    if (!image.full_pages) return false;

    uint8_t *tlv = &image.full_pages[16]; /* Page 4 is the first writable page. */
    size_t pos = 0;
    tlv[pos++] = 0x03;
    if (ndef_len <= 254) {
        tlv[pos++] = (uint8_t)ndef_len;
    } else {
        tlv[pos++] = 0xFF;
        tlv[pos++] = (uint8_t)(ndef_len >> 8);
        tlv[pos++] = (uint8_t)ndef_len;
    }
    if (ndef_len) memcpy(tlv + pos, ndef, ndef_len);
    tlv[pos + ndef_len] = 0xFE;
    bool ok = ntag_write_to_tag(nfc_t2_extension_reader, &image, NULL, NULL);
    ntag_file_free(&image);
    return ok;
}

static void nfc_scan_task(void *arg) {
    const char *TAGT = "NFCScan";
    if (nfc_t2_extension_task || nfc_t2_extension_reader || nfc_t2_extension_session) {
        ESP_LOGW(TAGT, "Type-2 extension session owns the local NFC reader");
        nfc_scan_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAGT, "scan_task: start (cancel=%d)", nfc_scan_cancel);
    g_is_emv = false;
    mfc_set_attack_hooks(&nfc_ui_attack_hooks);
    if (!nfc_init_local_reader(TAGT)) {
        ESP_LOGE(TAGT, "local NFC reader init failed");
        nfc_scan_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

#ifdef CONFIG_NFC_ST25R3916
    if (nfc_scan_picopass_only) {
        /* Dedicated PicoPass/iCLASS scan: stay in NFC-V mode for the whole
         * scan instead of interleaving with ISO14443A polling. */
        st25r3916_set_mode_picopass();
        while (!nfc_scan_cancel) {
            vTaskDelay(pdMS_TO_TICKS(20)); /* Give card time to power up */

            PicopassDeviceData *pp_data =
                heap_caps_calloc(1, sizeof(PicopassDeviceData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!pp_data) pp_data = calloc(1, sizeof(PicopassDeviceData));
            if (pp_data) {
                esp_err_t pp_err = picopass_detect(pp_data);
                ESP_LOGI(TAGT, "scan_task: picopass_detect returned %s", esp_err_to_name(pp_err));
                if (pp_err == ESP_OK && !nfc_scan_cancel) {
                    ESP_LOGI(TAGT, "scan_task: PicoPass/iCLASS detected");
                    status_display_show_status("iCLASS Tag Found");

                    esp_err_t auth_err = picopass_auth_and_read(pp_data);
                    if (auth_err == ESP_OK) {
                        picopass_parse_credential(pp_data->AA1, &pp_data->pacs);
                        picopass_parse_wiegand(pp_data->pacs.credential, &pp_data->pacs.record);
                    } else {
                        ESP_LOGW(TAGT, "scan_task: PicoPass auth failed: %s", esp_err_to_name(auth_err));
                    }

                    /* Store CSN as the "UID" for save compatibility */
                    g_uid_len = PICOPASS_UID_LEN;
                    memcpy(g_uid, pp_data->AA1[PICOPASS_CSN_BLOCK_INDEX].data, PICOPASS_UID_LEN);
                    g_atqa = 0; g_sak = 0; g_model = NTAG2XX_UNKNOWN;

                    if (nfc_uid_label && lv_obj_is_valid(nfc_uid_label)) {
                        char csn_text[64];
                        int pos = snprintf(csn_text, sizeof(csn_text), "CSN:");
                        for (int i = 0; i < PICOPASS_UID_LEN && pos < (int)sizeof(csn_text) - 4; i++) {
                            pos += snprintf(csn_text + pos, sizeof(csn_text) - pos, " %02X",
                                            pp_data->AA1[PICOPASS_CSN_BLOCK_INDEX].data[i]);
                        }
                        lv_label_set_text(nfc_uid_label, csn_text);
                    }
                    if (nfc_type_label && lv_obj_is_valid(nfc_type_label)) {
                        lv_label_set_text(nfc_type_label, "Type: PicoPass/iCLASS");
                    }

                    nfc_build_and_set_details_picopass(pp_data);

                    /* Save reference for save button */
                    EXT_RAM_BSS_ATTR static PicopassDeviceData s_picopass_data;
                    memcpy(&s_picopass_data, pp_data, sizeof(PicopassDeviceData));
                    free(pp_data);
                    break;
                }
                free(pp_data);
            }
        }
        goto scan_task_done;
    }
#endif

    while (!nfc_scan_cancel) {
        uint8_t uid[8] = {0};
        uint8_t uid_len = 0;
        uint16_t atqa = 0; uint8_t sak = 0;
        esp_err_t r = pn532_read_passive_target_id_ex(g_pn532, 0x00, uid + 1, &uid_len, &atqa, &sak, 200);
        if (r == ESP_OK && uid_len > 0 && uid_len <= 7) {
            uid[0] = uid_len;
            nfc_uid_evt_t *ev = nfc_uid_pool_alloc();
            if (ev) {
                ev->session = nfc_scan_session;
                ev->uid_len = uid_len;
                if (uid_len > sizeof(ev->uid)) ev->uid_len = sizeof(ev->uid);
                memcpy(ev->uid, uid + 1, ev->uid_len);
                if (display_manager_is_available()) display_manager_lvgl_async_call(nfc_update_labels_async, ev);
                else nfc_uid_pool_free(ev);
            }
            if (nfc_scan_cancel) break;
            g_uid_len = uid_len; memcpy(g_uid, uid + 1, uid_len); g_atqa = atqa; g_sak = sak; g_model = NTAG2XX_UNKNOWN;
            ESP_LOGI(TAGT, "scan_task: UID found, building details (len=%u)", uid_len);
            status_display_show_status("NFC Tag Found");
            nfc_build_and_set_details(g_pn532, uid + 1, uid_len);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

#ifdef CONFIG_NFC_ST25R3916
scan_task_done:
#endif
    if (g_pn532) {
        if (nfc_scan_cancel) {
            ESP_LOGI(TAGT, "scan_task: releasing PN532 (cancel=%d)", nfc_scan_cancel);
            pn532_release(g_pn532);
            pn532_delete_driver(g_pn532);
            g_pn532 = NULL;
        } else {
            ESP_LOGI(TAGT, "scan_task: keeping PN532 for Save (cancel=%d)", nfc_scan_cancel);
        }
    }
    nfc_scan_task_handle = NULL;
    ESP_LOGI(TAGT, "scan_task: exit");
    vTaskDelete(NULL);
}
#endif

#ifndef NFC_HAS_LOCAL_READER
bool nfc_view_t2_scan_start(void) { return false; }
bool nfc_view_t2_scan_stop(void) { return false; }
bool nfc_view_t2_scan_active(void) { return false; }
bool nfc_view_t2_read(nfc_view_t2_tag_info_t *out_info, uint8_t *ndef_out,
                      size_t max_ndef_bytes, size_t *ndef_bytes_out) {
    (void)out_info; (void)ndef_out; (void)max_ndef_bytes; (void)ndef_bytes_out;
    return false;
}
bool nfc_view_t2_write_ndef(const uint8_t *ndef, size_t ndef_len) {
    (void)ndef; (void)ndef_len;
    return false;
}
#endif

static void execute_selected(void) { /* no bottom status text in this view */ }

static void highlight_selected(void) {
    if (g_nfc_ov) options_view_set_selected(g_nfc_ov, selected_index);
}

static bool nfc_is_submenu_open(void) {
    return in_write_list || in_saved_list || in_emulate_list || in_generate_list || in_mfc_menu || in_tools_menu;
}

// forward declare back_event_cb so it can be used before its definition
static void back_event_cb(lv_event_t *e);
// forward declare option dispatcher used by multiple input paths
void nfc_option_event_cb(lv_event_t *e);

static void saved_delete_confirm_cb(void *user_data) {
    (void)user_data;
    if (g_saved_current_path[0] == '\0') return;
    bool susp = false; nfc_sd_begin(&susp);
    if (remove(g_saved_current_path) == 0) {
        ESP_LOGI(TAG, "deleted file: %s", g_saved_current_path);
    } else {
        ESP_LOGE(TAG, "failed delete: %s", g_saved_current_path);
    }
    nfc_sd_end(susp);
    cleanup_saved_details_popup(NULL);
    if (!in_saved_list) saved_enter_list(); else saved_enter_list();
}

static bool saved_delete_confirm_handle_input(InputEvent *event) {
    if (!popup_confirm_is_open(saved_delete_confirm_popup)) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) return popup_confirm_handle_touch(&saved_delete_confirm_popup, &event->data.touch_data);
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        popup_confirm_cancel(&saved_delete_confirm_popup);
        return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int ji = event->data.joystick_index;
        if (ji == 1) popup_confirm_select(&saved_delete_confirm_popup);
        else if (ji == 0) popup_confirm_set_selected(saved_delete_confirm_popup, 0);
        else if (ji == 3) popup_confirm_set_selected(saved_delete_confirm_popup, 1);
        else if (ji == 2 || ji == 4) popup_confirm_move(saved_delete_confirm_popup, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) popup_confirm_select(&saved_delete_confirm_popup);
        else if (event->data.encoder.direction != 0) popup_confirm_move(saved_delete_confirm_popup, event->data.encoder.direction);
        return true;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13 || kv == 10 || kv == LV_KEY_ENTER) popup_confirm_select(&saved_delete_confirm_popup);
        else if (kv == 27 || kv == 29 || kv == LV_KEY_ESC || kv == '`' || kv == 'c' || kv == 'C') popup_confirm_cancel(&saved_delete_confirm_popup);
        else if (kv == 9 || kv == ',' || kv == '.' || kv == ';' || kv == '/' || kv == 'h' || kv == 'l' || kv == 'k' || kv == 'j' ||
                 kv == LV_KEY_LEFT || kv == LV_KEY_RIGHT || kv == LV_KEY_UP || kv == LV_KEY_DOWN) popup_confirm_move(saved_delete_confirm_popup, 1);
        return true;
    }

    return true;
}

void nfc_view_input_cb(InputEvent *event) {
    if (!root) return;
    if (saved_delete_confirm_handle_input(event)) return;

    // Handle NFC scan popup input first
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return; // handle on release for consistency with this view
            // Cancel button
            if (nfc_scan_cancel_btn && lv_obj_is_valid(nfc_scan_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_scan_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) {
                    nfc_scan_cancel_cb(NULL);
                    return;
                }
            }
            // More button (if visible)
            if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
                lv_area_t b; lv_obj_get_coords(nfc_scan_more_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) {
                    nfc_scan_more_cb(NULL);
                    return;
                }
            }
            // Right action button: Scroll in parsed view, otherwise Save
            if (nfc_details_view_mode == 2) {
                if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn) &&
                    !lv_obj_has_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN)) {
                    lv_area_t c; lv_obj_get_coords(nfc_scan_scroll_btn, &c);
                    if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) {
                        nfc_scan_scroll_cb(NULL);
                        return;
                    }
                }
            } else if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
                lv_area_t c; lv_obj_get_coords(nfc_scan_save_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) {
                    nfc_scan_save_cb(NULL);
                    return;
                }
            }
            update_nfc_popup_selection();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the NFC scan popup
            nfc_scan_cancel_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (ji == 0) { // left
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                update_nfc_popup_selection();
            } else if (ji == 3) { // right
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                update_nfc_popup_selection();
            } else if (ji == 1) { // select/press
                if (nfc_popup_selected == 0) {
                    nfc_scan_cancel_cb(NULL);
                } else if (nfc_more_visible && nfc_popup_selected == 1) {
                    nfc_scan_more_cb(NULL);
                } else {
                    // Right button: either Save or Scroll
                    if (nfc_details_view_mode == 2) nfc_scan_scroll_cb(NULL);
                    else if (nfc_save_visible) nfc_scan_save_cb(NULL);
                }
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else {
                    if (nfc_details_view_mode == 2) nfc_scan_scroll_cb(NULL);
                    else if (nfc_save_visible) nfc_scan_save_cb(NULL);
                }
                return;
            }
            // Rotation toggles selection when More is available
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (event->data.encoder.direction != 0 && total > 1) {
                // invert encoder rotation: clockwise (direction>0) should move selection up
                if (event->data.encoder.direction > 0) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                else nfc_popup_selected = (nfc_popup_selected + 1) % total;
            }
            update_nfc_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            int total = 1 + (nfc_more_visible ? 1 : 0) + (nfc_save_visible ? 1 : 0);
            if (kv == 9) {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total; else nfc_popup_selected = 0;
                update_nfc_popup_selection();
            } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + total - 1) % total;
                update_nfc_popup_selection();
            } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') {
                if (total > 1) nfc_popup_selected = (nfc_popup_selected + 1) % total;
                update_nfc_popup_selection();
            } else if (kv == 's' || kv == 'S') {
                if (nfc_save_visible) { nfc_scan_save_cb(NULL); return; }
            } else if (kv == 'm' || kv == 'M') {
                if (nfc_more_visible) { nfc_scan_more_cb(NULL); return; }
            } else if (kv == 13 || kv == 10) {
                if (nfc_popup_selected == 0) nfc_scan_cancel_cb(NULL);
                else if (nfc_more_visible && nfc_popup_selected == 1) nfc_scan_more_cb(NULL);
                else if (nfc_save_visible && ((nfc_more_visible && nfc_popup_selected == 2) || (!nfc_more_visible && nfc_popup_selected == 1))) nfc_scan_save_cb(NULL);
                return;
            } else if (kv == 27 || kv == 'c' || kv == 'C') {
                nfc_scan_cancel_cb(NULL);
                return;
            } else {
                update_nfc_popup_selection();
            }
        }
        return; // consume input while popup is open
    }
    // Handle saved details popup input
    if (saved_popup && lv_obj_is_valid(saved_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) {
                /* Dragging over the body scrolls it; a press that starts on a
                 * button is a tap, never a drag. */
                bool on_btn = false;
                lv_area_t ab;
                if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) {
                    lv_obj_get_coords(saved_close_btn, &ab);
                    if (d->point.x >= ab.x1 && d->point.x <= ab.x2 && d->point.y >= ab.y1 && d->point.y <= ab.y2) on_btn = true;
                }
                if (!on_btn && saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) {
                    lv_obj_get_coords(saved_rename_btn, &ab);
                    if (d->point.x >= ab.x1 && d->point.x <= ab.x2 && d->point.y >= ab.y1 && d->point.y <= ab.y2) on_btn = true;
                }
                if (!on_btn && saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) {
                    lv_obj_get_coords(saved_delete_btn, &ab);
                    if (d->point.x >= ab.x1 && d->point.x <= ab.x2 && d->point.y >= ab.y1 && d->point.y <= ab.y2) on_btn = true;
                }
#ifdef CONFIG_USE_TOUCHSCREEN
                if (!on_btn && saved_scroll && lv_obj_is_valid(saved_scroll)) {
                    if (!saved_details_drag.started) touch_drag_begin(&saved_details_drag, d);
                    else touch_drag_update(&saved_details_drag, d, saved_scroll);
                } else {
                    touch_drag_reset(&saved_details_drag);
                }
#endif
                return;
            }
            /* Release */
#ifdef CONFIG_USE_TOUCHSCREEN
            bool was_dragged = saved_details_drag.started ? touch_drag_release(&saved_details_drag, d) : false;
            if (was_dragged) { display_manager_flush_pending_scroll(); return; }
#endif
            if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) {
                lv_area_t a; lv_obj_get_coords(saved_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { saved_more_cb(NULL); return; }
            }
            if (saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) {
                lv_area_t b; lv_obj_get_coords(saved_rename_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { saved_rename_cb(NULL); return; }
            }
            if (saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) {
                lv_area_t c; lv_obj_get_coords(saved_delete_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) { saved_delete_cb(NULL); return; }
            }
            update_saved_popup_selection();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the saved details popup
            saved_close_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 0) { // left
                saved_popup_selected = (saved_popup_selected + 3 - 1) % 3;
                update_saved_popup_selection();
            } else if (ji == 3) { // right
                saved_popup_selected = (saved_popup_selected + 1) % 3;
                update_saved_popup_selection();
            } else if (ji == 1) { // select
                if (saved_popup_selected == 0) saved_more_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            } else if (ji == 2 && saved_scroll && lv_obj_is_valid(saved_scroll)) { // down
                lv_obj_scroll_by_bounded(saved_scroll, 0, 40, LV_ANIM_OFF);
            } else if (ji == 4 && saved_scroll && lv_obj_is_valid(saved_scroll)) { // up
                lv_obj_scroll_by_bounded(saved_scroll, 0, -40, LV_ANIM_OFF);
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (saved_popup_selected == 0) saved_more_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            }
            if (event->data.encoder.direction != 0) {
                // invert encoder rotation: clockwise (direction>0) moves selection up
                if (event->data.encoder.direction > 0) saved_popup_selected = (saved_popup_selected + 3 - 1) % 3;
                else saved_popup_selected = (saved_popup_selected + 1) % 3;
                update_saved_popup_selection();
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 9) { saved_popup_selected = (saved_popup_selected + 1) % 3; update_saved_popup_selection(); }
            else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') { saved_popup_selected = (saved_popup_selected + 3 - 1) % 3; update_saved_popup_selection(); }
            else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') { saved_popup_selected = (saved_popup_selected + 1) % 3; update_saved_popup_selection(); }
            else if (kv == 13 || kv == 10) {
                if (saved_popup_selected == 0) saved_more_cb(NULL);
                else if (saved_popup_selected == 1) saved_rename_cb(NULL);
                else saved_delete_cb(NULL);
                return;
            } else if (kv == 27 || kv == 'c' || kv == 'C') { saved_close_cb(NULL); return; }
        }
        return;
    }
    // Handle credits popup input
    if (nfc_credits_popup && lv_obj_is_valid(nfc_credits_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) {
                /* Determine if the press started on the Close button; if so we
                 * treat it as a tap and never start a drag. */
                bool on_close = false;
                if (nfc_credits_close_btn && lv_obj_is_valid(nfc_credits_close_btn)) {
                    lv_area_t a; lv_obj_get_coords(nfc_credits_close_btn, &a);
                    if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) on_close = true;
                }
#ifdef CONFIG_USE_TOUCHSCREEN
                if (!on_close && nfc_credits_scroll && lv_obj_is_valid(nfc_credits_scroll)) {
                    if (!nfc_credits_drag.started) touch_drag_begin(&nfc_credits_drag, d);
                    else touch_drag_update(&nfc_credits_drag, d, nfc_credits_scroll);
                } else {
                    touch_drag_reset(&nfc_credits_drag);
                }
#endif
                return;
            } else {
                /* Release */
#ifdef CONFIG_USE_TOUCHSCREEN
                bool was_dragged = nfc_credits_drag.started ? touch_drag_release(&nfc_credits_drag, d) : false;
                if (was_dragged) { display_manager_flush_pending_scroll(); return; }
#endif
                if (nfc_credits_close_btn && lv_obj_is_valid(nfc_credits_close_btn)) {
                    lv_area_t a; lv_obj_get_coords(nfc_credits_close_btn, &a);
                    if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { nfc_credits_close_cb(NULL); return; }
                }
                return;
            }
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            nfc_credits_close_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            if (event->data.joystick_index == 1 || event->data.joystick_index == 0) { nfc_credits_close_cb(NULL); return; }
            if (event->data.joystick_index == 2 && nfc_credits_scroll) lv_obj_scroll_by_bounded(nfc_credits_scroll, 0, 40, LV_ANIM_OFF);
            if (event->data.joystick_index == 4 && nfc_credits_scroll) lv_obj_scroll_by_bounded(nfc_credits_scroll, 0, -40, LV_ANIM_OFF);
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) { nfc_credits_close_cb(NULL); return; }
            if (event->data.encoder.direction != 0 && nfc_credits_scroll) {
                lv_obj_scroll_by_bounded(nfc_credits_scroll, 0, event->data.encoder.direction > 0 ? -40 : 40, LV_ANIM_OFF);
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 13 || kv == 10 || kv == 27 || kv == 'c' || kv == 'C') { nfc_credits_close_cb(NULL); return; }
            if ((kv == 44 || kv == ',' || kv == 59 || kv == ';') && nfc_credits_scroll) lv_obj_scroll_by_bounded(nfc_credits_scroll, 0, 40, LV_ANIM_OFF);
            if ((kv == 47 || kv == '/' || kv == 46 || kv == '.') && nfc_credits_scroll) lv_obj_scroll_by_bounded(nfc_credits_scroll, 0, -40, LV_ANIM_OFF);
        }
        return;
    }


    // Handle keys popup input
    if (keys_popup && lv_obj_is_valid(keys_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (keys_close_btn && lv_obj_is_valid(keys_close_btn)) {
                lv_area_t a; lv_obj_get_coords(keys_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { keys_close_cb(NULL); return; }
            }
            update_keys_popup_selection();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the keys popup
            keys_close_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            // left/right switch focus Up <-> Close <-> Down, press activates
            if (ji == 0) { // left
                keys_popup_selected = (keys_popup_selected + 3 - 1) % 3;
                update_keys_popup_selection();
            } else if (ji == 3) { // right
                keys_popup_selected = (keys_popup_selected + 1) % 3;
                update_keys_popup_selection();
            } else if (ji == 1) { // press
                if (keys_popup_selected == 0) keys_scroll_up_cb(NULL);
                else if (keys_popup_selected == 1) { keys_close_cb(NULL); return; }
                else keys_scroll_down_cb(NULL);
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                // activate focused button: 0=Up,1=Close,2=Down
                if (keys_popup_selected == 0) keys_scroll_up_cb(NULL);
                else if (keys_popup_selected == 1) { keys_close_cb(NULL); return; }
                else keys_scroll_down_cb(NULL);
            }
            if (event->data.encoder.direction != 0) {
                // invert encoder rotation: clockwise (direction>0) moves selection up
                if (event->data.encoder.direction > 0) keys_popup_selected = (keys_popup_selected + 3 - 1) % 3;
                else keys_popup_selected = (keys_popup_selected + 1) % 3;
                update_keys_popup_selection();
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 13 || kv == 10 || kv == 27 || kv == 'c' || kv == 'C') { keys_close_cb(NULL); return; }
            else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') { keys_scroll_up_cb(NULL); }
            else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') { keys_scroll_down_cb(NULL); }
        }
        return;
    }
    // Handle Chameleon Ultra popup input
    if (cu_popup && lv_obj_is_valid(cu_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) {
                lv_area_t a; lv_obj_get_coords(cu_close_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { cu_close_cb(NULL); return; }
            }
            if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) {
                lv_area_t b; lv_obj_get_coords(cu_connect_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { cu_connect_cb(NULL); return; }
            }
            if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) {
                lv_area_t c; lv_obj_get_coords(cu_disconnect_btn, &c);
                if (d->point.x >= c.x1 && d->point.x <= c.x2 && d->point.y >= c.y1 && d->point.y <= c.y2) { cu_disconnect_cb(NULL); return; }
            }
            if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn)) {
                lv_area_t r; lv_obj_get_coords(cu_reader_btn, &r);
                if (d->point.x >= r.x1 && d->point.x <= r.x2 && d->point.y >= r.y1 && d->point.y <= r.y2) { cu_reader_cb(NULL); return; }
            }
            if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn)) {
                lv_area_t s; lv_obj_get_coords(cu_scan_hf_btn, &s);
                if (d->point.x >= s.x1 && d->point.x <= s.x2 && d->point.y >= s.y1 && d->point.y <= s.y2) { cu_scan_hf_cb(NULL); return; }
            }
            if (cu_save_visible && cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn)) {
                lv_area_t sv; lv_obj_get_coords(cu_save_hf_btn, &sv);
                if (d->point.x >= sv.x1 && d->point.x <= sv.x2 && d->point.y >= sv.y1 && d->point.y <= sv.y2) { cu_save_hf_cb(NULL); return; }
            }
            update_cu_popup_selection();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) { cu_close_cb(NULL); return; }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            lv_obj_t *btns[8];
            int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (cu_more_btn && lv_obj_is_valid(cu_more_btn)) btns[total++] = cu_more_btn;
            if (cu_more_expanded && chameleon_manager_is_connected()) {
                if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn) && !lv_obj_has_flag(cu_reader_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_reader_btn;
                if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn) && !lv_obj_has_flag(cu_scan_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_scan_hf_btn;
                if (cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn) && !lv_obj_has_flag(cu_save_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_save_hf_btn;
            }
            if (ji == 0) { if (total > 1) { cu_popup_selected = (cu_popup_selected + total - 1) % total; update_cu_popup_selection(); } }
            else if (ji == 3) { if (total > 1) { cu_popup_selected = (cu_popup_selected + 1) % total; update_cu_popup_selection(); } }
            else if (ji == 1) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            lv_obj_t *btns[8]; int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (cu_more_btn && lv_obj_is_valid(cu_more_btn)) btns[total++] = cu_more_btn;
            if (cu_more_expanded && chameleon_manager_is_connected()) {
                if (cu_reader_btn && lv_obj_is_valid(cu_reader_btn) && !lv_obj_has_flag(cu_reader_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_reader_btn;
                if (cu_scan_hf_btn && lv_obj_is_valid(cu_scan_hf_btn) && !lv_obj_has_flag(cu_scan_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_scan_hf_btn;
                if (cu_save_hf_btn && lv_obj_is_valid(cu_save_hf_btn) && !lv_obj_has_flag(cu_save_hf_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_save_hf_btn;
            }
            if (event->data.encoder.button) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            }
            if (event->data.encoder.direction != 0 && total > 1) {
                if (event->data.encoder.direction > 0) cu_popup_selected = (cu_popup_selected + total - 1) % total;
                else cu_popup_selected = (cu_popup_selected + 1) % total;
            }
            update_cu_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            lv_obj_t *btns[8]; int total = 0;
            if (cu_close_btn && lv_obj_is_valid(cu_close_btn) && !lv_obj_has_flag(cu_close_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_close_btn;
            if (chameleon_manager_is_connected()) {
                if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_disconnect_btn;
            } else {
                if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[total++] = cu_connect_btn;
            }
            if (kv == 9) {
                if (total > 1) cu_popup_selected = (cu_popup_selected + 1) % total;
                update_cu_popup_selection();
            } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') {
                if (total > 1) cu_popup_selected = (cu_popup_selected + total - 1) % total;
                update_cu_popup_selection();
            } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') {
                if (total > 1) cu_popup_selected = (cu_popup_selected + 1) % total;
                update_cu_popup_selection();
            } else if (kv == 13 || kv == 10) {
                if (cu_popup_selected >= 0 && cu_popup_selected < total) { lv_event_send(btns[cu_popup_selected], LV_EVENT_CLICKED, NULL); return; }
            } else if (kv == 27 || kv == 'c' || kv == 'C') { cu_close_cb(NULL); return; }
        }
        return;
    }

    // Handle NFC write popup input
    if (nfc_write_popup && lv_obj_is_valid(nfc_write_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (nfc_write_cancel_btn && lv_obj_is_valid(nfc_write_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_write_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { nfc_write_cancel_cb(NULL); return; }
            }
            if (nfc_write_go_btn && lv_obj_is_valid(nfc_write_go_btn)) {
                lv_area_t b; lv_obj_get_coords(nfc_write_go_btn, &b);
                if (d->point.x >= b.x1 && d->point.x <= b.x2 && d->point.y >= b.y1 && d->point.y <= b.y2) { nfc_write_go_cb(NULL); return; }
            }
            update_nfc_write_popup_selection();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            // Hardware back button closes the NFC write popup
            nfc_write_cancel_cb(NULL);
            return;
        }
#endif
        else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 0 || ji == 3) {
                nfc_write_popup_selected = (nfc_write_popup_selected ^ 1);
                update_nfc_write_popup_selection();
            } else if (ji == 1) {
                if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) { if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL); return; }
            if (event->data.encoder.direction != 0) { nfc_write_popup_selected = (nfc_write_popup_selected ^ 1); }
            update_nfc_write_popup_selection();
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 9) { nfc_write_popup_selected = (nfc_write_popup_selected ^ 1); update_nfc_write_popup_selection(); }
            else if (kv == 13 || kv == 10) { if (nfc_write_popup_selected == 0) nfc_write_cancel_cb(NULL); else nfc_write_go_cb(NULL); return; }
            else if (kv == 27 || kv == 'c' || kv == 'C') { nfc_write_cancel_cb(NULL); return; }
        }
        return;
    }

    // Handle NFC emulate popup input (Cancel-only)
    if (nfc_emu_popup && lv_obj_is_valid(nfc_emu_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_PR) return;
            if (nfc_emu_cancel_btn && lv_obj_is_valid(nfc_emu_cancel_btn)) {
                lv_area_t a; lv_obj_get_coords(nfc_emu_cancel_btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) { nfc_emu_cancel_cb(NULL); return; }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 1 || ji == 0) { nfc_emu_cancel_cb(NULL); return; }
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) { nfc_emu_cancel_cb(NULL); return; }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 13 || kv == 10 || kv == 27 || kv == 'c' || kv == 'C' || kv == 29 || kv == '`') { nfc_emu_cancel_cb(NULL); return; }
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
        else if (event->type == INPUT_TYPE_EXIT_BUTTON) { nfc_emu_cancel_cb(NULL); return; }
#endif
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
#ifdef CONFIG_USE_TOUCHSCREEN
        if (d->state == LV_INDEV_STATE_PR) {
            if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (nfc_write_popup && lv_obj_is_valid(nfc_write_popup)) {
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (nfc_emu_popup && lv_obj_is_valid(nfc_emu_popup)) {
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(scroll_up_btn, d->point.x, d->point.y)) {
                scroll_nfc_up(NULL);
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(scroll_down_btn, d->point.x, d->point.y)) {
                scroll_nfc_down(NULL);
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (gui_touch_bar_hit(back_btn, d->point.x, d->point.y)) {
                back_event_cb(NULL);
                touch_drag_reset(&nfc_touch_drag);
                return;
            }
            if (!nfc_touch_drag.started) {
                touch_drag_begin(&nfc_touch_drag, d);
            } else {
                // Move event - apply live drag or remember target for release
                lv_area_t cont_area;
                if (menu_container && lv_obj_is_valid(menu_container)) {
                    lv_obj_get_coords(menu_container, &cont_area);
                    bool started_in_container = (nfc_touch_drag.start_x >= cont_area.x1 && nfc_touch_drag.start_x <= cont_area.x2 &&
                                                 nfc_touch_drag.start_y >= cont_area.y1 && nfc_touch_drag.start_y <= cont_area.y2);
                    if (started_in_container) {
                        touch_drag_update(&nfc_touch_drag, d, menu_container);
                    }
                }
            }
            return;
        }
        if (d->state == LV_INDEV_STATE_REL) {
            if (!nfc_touch_drag.started) return;

            if (!menu_container || !lv_obj_is_valid(menu_container)) {
                touch_drag_reset(&nfc_touch_drag);
                return;
            }

            int thr_x = LV_HOR_RES / NFC_SWIPE_THRESHOLD_RATIO;
            int dx = d->point.x - nfc_touch_drag.start_x;

            // Let the shared touch_drag helper handle release-on-release
            // (it applies a single scroll when the live setting is off) and
            // tell us if a drag was in progress so we can skip tap handling.
            bool was_dragged = touch_drag_release(&nfc_touch_drag, d);
            if (was_dragged) {
                display_manager_flush_pending_scroll();
                update_nfc_scroll_buttons_visibility();
                return;
            }
            if (abs(dx) > thr_x) return;

            lv_area_t cont_area;
            lv_obj_get_coords(menu_container, &cont_area);
            if (d->point.x < cont_area.x1 || d->point.x > cont_area.x2 ||
                d->point.y < cont_area.y1 || d->point.y > cont_area.y2) {
                return;
            }

            int cnt = g_nfc_ov ? options_view_get_item_count(g_nfc_ov) : 0;
            for (int i = 0; i < cnt; ++i) {
                lv_obj_t *btn = lv_obj_get_child(menu_container, i);
                if (!btn) continue;
                lv_area_t a;
                lv_obj_get_coords(btn, &a);
                if (d->point.x >= a.x1 && d->point.x <= a.x2 &&
                    d->point.y >= a.y1 && d->point.y <= a.y2) {
                    selected_index = i;
                    if (g_nfc_ov) options_view_set_selected(g_nfc_ov, i);
                    lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                    return;
                }
            }
            return;
        }
#else
        if (d->state == LV_INDEV_STATE_PR) return;
        int x = d->point.x;
        int y = d->point.y;
        int cnt = g_nfc_ov ? options_view_get_item_count(g_nfc_ov) : 0;
        for (int i = 0; i < cnt; ++i) {
            lv_obj_t *btn = lv_obj_get_child(menu_container, i);
            if (!btn) continue;
            lv_area_t a;
            lv_obj_get_coords(btn, &a);
            if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
                selected_index = i;
                if (g_nfc_ov) options_view_set_selected(g_nfc_ov, i);
                lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                return;
            }
        }
        if (nfc_is_submenu_open()) back_to_root_menu(); else display_manager_go_back();
#endif
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 2) {
            if (g_nfc_ov) { options_view_move_selection(g_nfc_ov, -1); selected_index = options_view_get_selected(g_nfc_ov); }
        } else if (btn == 4) {
            if (g_nfc_ov) { options_view_move_selection(g_nfc_ov, 1); selected_index = options_view_get_selected(g_nfc_ov); }
        } else if (btn == 1) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
        } else if (btn == 0) {
            if (nfc_is_submenu_open()) back_to_root_menu(); else display_manager_go_back();
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            lv_obj_t *sel = lv_obj_get_child(menu_container, selected_index);
            if (sel) lv_event_send(sel, LV_EVENT_CLICKED, NULL);
        } else {
            if (g_nfc_ov) {
                options_view_move_selection(g_nfc_ov, event->data.encoder.direction > 0 ? 1 : -1);
                selected_index = options_view_get_selected(g_nfc_ov);
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13) {
            lv_obj_t *selected_obj = lv_obj_get_child(menu_container, selected_index);
            if (selected_obj) lv_event_send(selected_obj, LV_EVENT_CLICKED, NULL);
        } else if (kv == 44 || kv == ',' || kv == 59 || kv == ';') {
            if (g_nfc_ov) { options_view_move_selection(g_nfc_ov, -1); selected_index = options_view_get_selected(g_nfc_ov); }
        } else if (kv == 47 || kv == '/' || kv == 46 || kv == '.') {
            if (g_nfc_ov) { options_view_move_selection(g_nfc_ov, 1); selected_index = options_view_get_selected(g_nfc_ov); }
        } else if (kv == 29 || kv == '`') {
            if (nfc_is_submenu_open()) back_to_root_menu(); else display_manager_go_back();
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (nfc_is_submenu_open()) back_to_root_menu(); else display_manager_go_back();
#endif
    }
}

void nfc_option_event_cb(lv_event_t *e) {
    if (nfc_option_invoked) return;
    nfc_option_invoked = true;

    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    if (now_ms - nfc_created_time_ms <= 500) {
        nfc_option_invoked = false;
        return;
    }

    const char *opt = (const char *)lv_event_get_user_data(e);
    if (!opt) { nfc_option_invoked = false; return; }
    if (strcmp(opt, "__BACK_OPTION__") == 0) {
        back_event_cb(NULL);
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Scan") == 0) {
#ifdef CONFIG_NFC_ST25R3916
        nfc_scan_picopass_only = false;
#endif
        create_nfc_scan_popup();
        nfc_option_invoked = false;
        return;
    }

#ifdef CONFIG_NFC_ST25R3916
    if (strcmp(opt, "iCLASS / PicoPass") == 0) {
        nfc_scan_picopass_only = true;
        create_nfc_scan_popup();
        nfc_option_invoked = false;
        return;
    }
#endif

    if (strcmp(opt, "MIFARE Classic") == 0) {
        nfc_enter_mfc_menu();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Tools") == 0) {
        nfc_enter_tools_menu();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Emulate") == 0) {
        nfc_enter_emulate_list();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Write") == 0) {
        nfc_enter_write_list();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Generate NDEF") == 0) {
        nfc_enter_generate_list();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Saved") == 0) {
        saved_enter_list();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "User Keys") == 0) {
        create_keys_popup();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "Chameleon Ultra") == 0) {
        create_cu_popup();
        nfc_option_invoked = false;
        return;
    }

    if (strcmp(opt, "NFC Credits") == 0) {
        create_nfc_credits_popup();
        nfc_option_invoked = false;
        return;
    }

    nfc_option_invoked = false;
}

static void update_nfc_scroll_buttons_visibility(void) {
    gui_touch_bar_update_visibility(&s_touch_tb, menu_container);
}

static void scroll_nfc_up(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, scroll_amt, LV_ANIM_OFF);
    update_nfc_scroll_buttons_visibility();
}
static void scroll_nfc_down(lv_event_t *e) {
    if (!menu_container) return;
    lv_coord_t scroll_amt = lv_obj_get_height(menu_container) / 2;
    lv_obj_scroll_by_bounded(menu_container, 0, -scroll_amt, LV_ANIM_OFF);
    update_nfc_scroll_buttons_visibility();
}
static void back_event_cb(lv_event_t *e) {
    if (nfc_is_submenu_open()) back_to_root_menu();
    else display_manager_go_back();
}

void cleanup_nfc_scan_popup(void *obj) {
#ifdef NFC_HAS_LOCAL_READER
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: begin (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
#else
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: begin");
#endif
    if (nfc_scan_popup) {
        lvgl_obj_del_safe(&nfc_scan_popup);
        nfc_btn_bar = NULL;
        nfc_scan_cancel_btn = NULL;
        nfc_scan_more_btn = NULL;
        nfc_scan_save_btn = NULL;
        nfc_title_label = NULL;
        nfc_uid_label = NULL;
        nfc_type_label = NULL;
        nfc_details_label = NULL;
        nfc_details_scroll = NULL;
    }
    // restore bottom nav buttons on touch builds
#ifdef CONFIG_USE_TOUCHSCREEN
    if (scroll_up_btn && lv_obj_is_valid(scroll_up_btn)) lv_obj_clear_flag(scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
    if (scroll_down_btn && lv_obj_is_valid(scroll_down_btn)) lv_obj_clear_flag(scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
    if (back_btn && lv_obj_is_valid(back_btn)) lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef NFC_HAS_LOCAL_READER
    // Signal the scan task to exit gracefully (avoid calling LVGL from that task)
    nfc_scan_cancel = true;
    // Always restore normal I2C activity on popup close to avoid UI/input issues on some boards
    display_manager_set_low_i2c_mode(false);
#ifdef CONFIG_HAS_FUEL_GAUGE
    // Pause fuel gauge while the NFC scan task winds down to avoid I2C contention
    fuel_gauge_manager_set_paused(true);
#endif
    // If a deferred retry timer exists, delete it now (legacy cleanup)
    lvgl_timer_del_safe(&nfc_scan_retry_timer);
    // Wait briefly for the scan task to exit and release PN532 itself
    uint32_t waited_ms = 0;
    while (nfc_scan_task_handle != NULL && waited_ms < 800) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited_ms += 20;
    }
    if (nfc_scan_task_handle != NULL) {
        ESP_LOGW(TAG, "cleanup_nfc_scan_popup: scan task still running after %ums (skipping force delete)", (unsigned)waited_ms);
    }
    // If for any reason PN532 is still held here and the task has exited, release as a safety net
    if (nfc_scan_task_handle == NULL && g_pn532) {
        ESP_LOGI(TAG, "cleanup_nfc_scan_popup: releasing PN532 resources (post-exit)");
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
    nfc_detected_title[0] = '\0';
    // Clear paused/cache flags so a new scan doesn't inherit a stale state
    nfc_paused = false;
    nfc_cache_fill_phase = false;
    // Synchronously update UI to clear any paused state immediately
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "Cancelled");
    }
    mfc_set_progress_callback(NULL, NULL);
#endif
#ifdef NFC_HAS_LOCAL_READER
    // Resume fuel gauge after task has had time to stop
#ifdef CONFIG_HAS_FUEL_GAUGE
    fuel_gauge_manager_set_paused(false);
#endif
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: end (task=%p, cancel=%d)", (void*)nfc_scan_task_handle, nfc_scan_cancel);
#else
    ESP_LOGI(TAG, "cleanup_nfc_scan_popup: end");
#endif
}

static void nfc_scan_cancel_cb(lv_event_t *e) {
    status_display_show_status("NFC Scan Canceled");
    cleanup_nfc_scan_popup(NULL);
}

static void nfc_scan_scroll_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI("NFC", "Scroll button pressed");
    if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) {
        ESP_LOGW("NFC", "Scroll container invalid or null");
        return;
    }
    lv_obj_t *scroller = nfc_details_scroll;

    lv_coord_t h = lv_obj_get_height(scroller);
    lv_coord_t y_before = lv_obj_get_scroll_y(scroller);
    lv_coord_t step = (h > 40) ? (h - 40) : (h / 2);
    if (step < 10) step = 10;

    ESP_LOGI("NFC", "Scroll before: y=%d, h=%d, step=%d", y_before, h, step);

    lv_obj_scroll_by_bounded(scroller, 0, -step, LV_ANIM_OFF);

    lv_coord_t y_after = lv_obj_get_scroll_y(scroller);
    ESP_LOGI("NFC", "Scroll after: y=%d", y_after);

    if (y_after == y_before) {
        ESP_LOGI("NFC", "Reached bottom or no scroll possible; wrapping to top");
        lv_obj_scroll_to_y(scroller, 0, LV_ANIM_ON);
    }
}

static void nfc_scan_more_cb(lv_event_t *e) {
    (void)e;
    // If bruteforcing is active, treat More as Skip (basic read)
    if (!nfc_dict_skip_requested && mfc_phase_sector >= 0) {
        nfc_dict_skip_requested = true;
        status_display_show_status("MFC Dict Skipped");
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "Basic read (skipping dict) ...");
        }
        // Update button to reflect action taken
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "Skipping...");
            lv_obj_add_state(nfc_scan_more_btn, LV_STATE_DISABLED);
        }
        return;
    }
    
    // Toggle/Cycle details view
    #if defined(CONFIG_NFC_CHAMELEON)
    if (using_chameleon_backend() && !nfc_details_visible) {
        nfc_refresh_cu_details_from_cache();
    }
    #endif

    if (nfc_details_view_mode == 0) {
        // Summary -> Basic
        nfc_details_view_mode = 1;
        nfc_show_details_view(true);
    } else if (nfc_details_view_mode == 1) {
        // Basic -> Full (if available)
        if (has_extra_details(nfc_details_text)) {
            nfc_details_view_mode = 2;
            nfc_show_details_view(true);
        } else {
            // No extra details, go to Summary
            nfc_details_view_mode = 0;
            nfc_show_details_view(false);
        }
    } else {
        // Full -> Summary
        nfc_details_view_mode = 0;
        nfc_show_details_view(false);
    }
}

static void nfc_scan_save_cb(lv_event_t *e) {
    if (nfc_save_in_progress) return;
    nfc_save_in_progress = true;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, "Saving...");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_add_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
#if defined(CONFIG_NFC_CHAMELEON)
    if (using_chameleon_backend()) {
        BaseType_t rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 6144, NULL, 5, NULL);
        if (rc != pdPASS) rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 4096, NULL, 5, NULL);
        if (rc != pdPASS) rc = xTaskCreate(nfc_save_cu_task, "nfc_save_cu", 3072, NULL, 5, NULL);
        if (rc != pdPASS) {
            nfc_save_in_progress = false;
            if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "NFC Tag");
            if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
            ESP_LOGE(TAG, "nfc_save_cu_task create failed");
        }
        return;
    }
#endif
#ifdef NFC_HAS_LOCAL_READER
    mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
    BaseType_t rc = xTaskCreate(nfc_save_task, "nfc_save", 6144, NULL, 5, NULL);
    if (rc != pdPASS) rc = xTaskCreate(nfc_save_task, "nfc_save", 4096, NULL, 5, NULL);
    if (rc != pdPASS) {
        nfc_save_in_progress = false;
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) lv_label_set_text(nfc_title_label, "NFC Tag");
        if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
        mfc_set_progress_callback(NULL, NULL);
        ESP_LOGE(TAG, "nfc_save_task create failed");
    }
#endif
}

static bool write_flipper_nfc_file(void) {
    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    sd_card_create_directory(dir);
#ifdef NFC_HAS_LOCAL_READER
    if (g_uid_len == 0 || g_pn532 == NULL) {
        ESP_LOGW(TAG, "No NFC UID/driver to save");
        if (did) nfc_sd_end(susp);
        return false;
    }

    // Build filename: <Model>_<UID>.nfc
    char uid_part[40] = {0};
    int up = 0;
    for (uint8_t i = 0; i < g_uid_len && up < (int)sizeof(uid_part) - 3; ++i) {
        up += snprintf(uid_part + up, sizeof(uid_part) - up, "%02X", g_uid[i]);
        if (i + 1 < g_uid_len) up += snprintf(uid_part + up, sizeof(uid_part) - up, "-");
    }
    char path[192];

    /* EMV payment cards: save the cached read (no card re-tap required). */
    if (g_is_emv) {
        bool ok = emv_save_flipper_file(&g_emv, dir, g_uid, g_uid_len, g_atqa, g_sak, NULL, 0);
        if (did) nfc_sd_end(susp);
        return ok;
    }

    if (mfc_is_classic_sak(g_sak)) {
        // Prefer cached save (io=NULL) so user can save without card present
        bool ok = mfc_save_flipper_file(NULL, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        if (!ok && g_pn532) {
            ESP_LOGW(TAG, "Offline save failed; retrying with live PN532");
            ok = mfc_save_flipper_file(g_pn532, g_uid, g_uid_len, g_atqa, g_sak, dir, NULL, 0);
        }
        if (!ok) {
            ESP_LOGE(TAG, "Failed to save Mifare Classic file");
            if (did) nfc_sd_end(susp);
            return false;
        }
        ESP_LOGI(TAG, "Mifare Classic file saved");
        if (did) nfc_sd_end(susp);
        return true;
    }

    if (desfire_is_desfire_candidate(g_atqa, g_sak)) {
        snprintf(path, sizeof(path), "%s/Desfire_%s.nfc", dir, uid_part);

        desfire_version_t ver;
        bool have_ver = desfire_get_version(g_pn532, &ver);

        /* Re-read the full application/file tree so the saved dump includes
         * all plaintext files (e.g. myki card number, Clipper balance). */
        MfDesfireData *tree = desfire_tree_alloc();
        if (tree) {
            (void)desfire_read_tree(g_pn532, tree);
        }

        char *buf = desfire_build_flipper_text(tree,
                                                g_uid, g_uid_len,
                                                g_atqa, g_sak,
                                                have_ver ? &ver : NULL);
        if (tree) desfire_tree_free(tree);

        if (!buf) {
            ESP_LOGE(TAG, "Failed to build DESFire dump");
            if (did) nfc_sd_end(susp);
            return false;
        }

        size_t buflen = strlen(buf);
        if (sd_card_write_file(path, buf, buflen) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write DESFire dump: %s", path);
            free(buf);
            if (did) nfc_sd_end(susp);
            return false;
        }
        ESP_LOGI(TAG, "Mifare DESFire dump saved (%u bytes): %s", (unsigned)buflen, path);
        free(buf);
        if (did) nfc_sd_end(susp);
        return true;
    }

#ifdef CONFIG_NFC_ST25R3916
    /* PicoPass/iCLASS save: g_atqa==0 && g_sak==0 && uid_len==8 indicates PicoPass */
    if (g_atqa == 0 && g_sak == 0 && g_uid_len == PICOPASS_UID_LEN) {
        snprintf(path, sizeof(path), "%s/picopass_%s.picopass", dir, uid_part);

        /* Re-detect and re-auth to get fresh data for save */
        PicopassDeviceData *pp_save =
            heap_caps_calloc(1, sizeof(PicopassDeviceData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pp_save) pp_save = calloc(1, sizeof(PicopassDeviceData));
        if (pp_save) {
            st25r3916_field_off();
            st25r3916_set_mode_picopass();
            st25r3916_field_on();
            if (picopass_detect(pp_save) == ESP_OK) {
                picopass_auth_and_read(pp_save);
                picopass_parse_credential(pp_save->AA1, &pp_save->pacs);
                picopass_parse_wiegand(pp_save->pacs.credential, &pp_save->pacs.record);
            }
            bool pp_ok = (picopass_save_file(path, pp_save) == ESP_OK);
            free(pp_save);
            st25r3916_set_mode_nfca();
            if (did) nfc_sd_end(susp);
            if (pp_ok) ESP_LOGI(TAG, "PicoPass file saved: %s", path);
            else ESP_LOGE(TAG, "Failed to save PicoPass file");
            return pp_ok;
        }
        if (did) nfc_sd_end(susp);
        return false;
    }
#endif

    // Non-Classic (NTAG/Ultralight) path
    const char *model_str = ntag_t2_model_str(g_model);
    snprintf(path, sizeof(path), "%s/%s_%s.nfc", dir, model_str, uid_part);

    // Determine total pages by model (NTAG/Ultralight)
    int pages_total = 0;
    switch (g_model) {
        case NTAG2XX_NTAG213: pages_total = 45; break;
        case NTAG2XX_NTAG215: pages_total = 135; break;
        case NTAG2XX_NTAG216: pages_total = 231; break;
        default: pages_total = 135; break;
    }

    // Header
    char buf[512];
    int pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Filetype: Flipper NFC device\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Version: 4\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Device type: NTAG/Ultralight\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "UID:");
    for (uint8_t i = 0; i < g_uid_len && pos < (int)sizeof(buf) - 4; ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", g_uid[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "ATQA: %02X %02X\n", (g_atqa >> 8) & 0xFF, g_atqa & 0xFF);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "SAK: %02X\n", g_sak);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data format version: 2\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "NTAG/Ultralight type: %s\n", model_str);

    if (sd_card_write_file(path, buf, (size_t)pos) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write header: %s", path);
        if (did) nfc_sd_end(susp);
        return false;
    }

    // batch metadata writes: signature, version, counters
    char *meta = NULL; size_t meta_cap = 512; size_t mpos = 0;
    meta = (char*)malloc(meta_cap);
    if (!meta) {
        // fallback: original per-section appends
        uint8_t sig[32];
        if (ntag2xx_read_signature(g_pn532, sig) != ESP_OK) memset(sig, 0, sizeof(sig));
        pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Signature:");
        for (int i = 0; i < 32 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", sig[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
        sd_card_append_file(path, buf, (size_t)pos);
        uint8_t ver[8]; if (ntag2xx_get_version(g_pn532, ver) != ESP_OK) memset(ver, 0, sizeof(ver));
        pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Mifare version:");
        for (int i = 0; i < 8 && pos < (int)sizeof(buf) - 4; ++i) pos += snprintf(buf + pos, sizeof(buf) - pos, " %02X", ver[i]);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
        sd_card_append_file(path, buf, (size_t)pos);
        for (int ci = 0; ci < 3; ++ci) {
            uint32_t cv = 0; uint8_t tr = 0;
            esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv);
            esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
            pos = 0; pos += snprintf(buf + pos, sizeof(buf) - pos, "Counter %d: %u\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0);
            pos += snprintf(buf + pos, sizeof(buf) - pos, "Tearing %d: %02X\n", ci, (ert == ESP_OK) ? tr : 0);
            sd_card_append_file(path, buf, (size_t)pos);
        }
    } else {
        // signature
        uint8_t sig[32];
        if (ntag2xx_read_signature(g_pn532, sig) != ESP_OK) memset(sig, 0, sizeof(sig));
        int w = snprintf(NULL, 0, "Signature:");
        if (mpos + (size_t)w + 1 > meta_cap) { size_t nc = meta_cap * 2 + (size_t)w + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        memcpy(meta + mpos, "Signature:", (size_t)w); mpos += (size_t)w;
        for (int i = 0; i < 32; ++i) {
            char tmp[4]; int tp = snprintf(tmp, sizeof(tmp), " %02X", sig[i]);
            if (mpos + (size_t)tp + 1 > meta_cap) { size_t nc = meta_cap * 2 + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, tmp, (size_t)tp); mpos += (size_t)tp;
        }
        if (mpos + 1 > meta_cap) { size_t nc = meta_cap + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        meta[mpos++] = '\n';
        // version
        uint8_t ver[8]; if (ntag2xx_get_version(g_pn532, ver) != ESP_OK) memset(ver, 0, sizeof(ver));
        w = snprintf(NULL, 0, "Mifare version:");
        if (mpos + (size_t)w + 1 > meta_cap) { size_t nc = meta_cap * 2 + (size_t)w + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        memcpy(meta + mpos, "Mifare version:", (size_t)w); mpos += (size_t)w;
        for (int i = 0; i < 8; ++i) {
            char tmp[4]; int tp = snprintf(tmp, sizeof(tmp), " %02X", ver[i]);
            if (mpos + (size_t)tp + 1 > meta_cap) { size_t nc = meta_cap * 2 + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, tmp, (size_t)tp); mpos += (size_t)tp;
        }
        if (mpos + 1 > meta_cap) { size_t nc = meta_cap + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
        meta[mpos++] = '\n';
        // counters and tearing flags
        for (int ci = 0; ci < 3; ++ci) {
            uint32_t cv = 0; uint8_t tr = 0;
            esp_err_t erc = ntag2xx_read_counter(g_pn532, (uint8_t)ci, &cv);
            esp_err_t ert = ntag2xx_read_tearing(g_pn532, (uint8_t)ci, &tr);
            char line[64];
            int lp = snprintf(line, sizeof(line), "Counter %d: %u\nTearing %d: %02X\n", ci, (erc == ESP_OK) ? (unsigned)cv : 0, ci, (ert == ESP_OK) ? tr : 0);
            if (mpos + (size_t)lp > meta_cap) { size_t nc = meta_cap * 2 + (size_t)lp + 64; char *n = (char*)realloc(meta, nc); if (!n) { free(meta); meta = NULL; goto meta_fallback; } meta = n; meta_cap = nc; }
            memcpy(meta + mpos, line, (size_t)lp); mpos += (size_t)lp;
        }
        if (mpos > 0) sd_card_append_file(path, meta, mpos);
        free(meta); meta = NULL;
    }
meta_fallback: ;

    // Read all pages and build page dump
    size_t cap = (size_t)pages_total * 48 + 64;
    char *pages = (char*)malloc(cap);
    if (!pages) {
        ESP_LOGE(TAG, "OOM building page dump");
        if (did) nfc_sd_end(susp);
        return false;
    }
    int ppos = 0; int pages_read = 0;
    for (int pg = 0; pg < pages_total; pg += 4) {
        uint8_t block[16] = {0};
        if (ntag2xx_read_page(g_pn532, (uint8_t)pg, block, 16) == ESP_OK) {
            int chunk = (pages_total - pg >= 4) ? 4 : (pages_total - pg);
            pages_read += chunk;
        }
        // format up to 4 pages from this block
        for (int off = 0; off < 4 && pg + off < pages_total; ++off) {
            uint8_t *data = &block[off * 4];
            ppos += snprintf(pages + ppos, cap - ppos, "Page %d: %02X %02X %02X %02X\n",
                             pg + off, data[0], data[1], data[2], data[3]);
            if (ppos >= (int)cap - 64) break;
        }
        if (ppos >= (int)cap - 64) break;
    }

    // Pages meta then pages dump
    pos = snprintf(buf, sizeof(buf), "Pages total: %d\nPages read: %d\n", pages_total, pages_read);
    sd_card_append_file(path, buf, (size_t)pos);
    sd_card_append_file(path, pages, (size_t)ppos);
    free(pages);

    // Footer
    const char *footer = "Failed authentication attempts: 0\n";
    sd_card_append_file(path, footer, strlen(footer));

    ESP_LOGI(TAG, "NFC file saved: %s", path);
    if (did) nfc_sd_end(susp);
    return true;
#else
    ESP_LOGW(TAG, "NFC not enabled; nothing to save");
    return false;
#endif
}

static void create_nfc_scan_popup(void) {
    ESP_LOGI(TAG, "create_nfc_scan_popup");
    if (nfc_scan_popup && lv_obj_is_valid(nfc_scan_popup)) {
        cleanup_nfc_scan_popup(NULL);
    }
    if (!root || !lv_obj_is_valid(root)) return;
    // We'll reset the cancel flag right before (re)starting the scan task
    nfc_dict_skip_requested = false;
    // Ensure fresh UI state (avoid showing stale paused/cache states)
    nfc_paused = false;
    nfc_cache_fill_phase = false;
    nfc_details_ready = false;
    // New scan session: invalidate any stale async events from prior scan
    nfc_scan_session++;
    // scale to screen, leave margin for edges
    popup_calc_size_t geom;
    popup_calc_size(&geom);
    nfc_scan_popup = popup_create_container_with_offset(lv_scr_act(), geom.width, geom.height, geom.y_offset, true);
    if (nfc_scan_popup) lv_obj_add_flag(nfc_scan_popup, LV_OBJ_FLAG_CLICKABLE);

    // Fonts
    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const char *scan_title = "Scanning NFC...";
#ifdef CONFIG_NFC_ST25R3916
    if (nfc_scan_picopass_only) scan_title = "Scanning PicoPass...";
#endif
    nfc_title_label = popup_create_title_label(nfc_scan_popup, scan_title, title_font, 22);

    // Placeholder fields (UID / Type)
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();
    nfc_uid_label = popup_create_body_label(nfc_scan_popup, "UID: -- -- -- -- -- -- -- --", 0, false, body_font, 40);
    if (nfc_uid_label) lv_obj_set_style_text_color(nfc_uid_label, lv_color_hex(0xCCCCCC), 0);

    nfc_type_label = popup_create_body_label(nfc_scan_popup, "Type: --", 0, false, body_font, 60);
    if (nfc_type_label) lv_obj_set_style_text_color(nfc_type_label, lv_color_hex(0xCCCCCC), 0);

    // Progress indicators removed; we will update the title and details text instead

    // Cancel button
    int btn_w = 90, btn_h = 34;
    if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    nfc_scan_cancel_btn = popup_add_styled_button(nfc_scan_popup, "Cancel", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, nfc_scan_cancel_cb, NULL);

    // More button (hidden until a tag is scanned)
    nfc_scan_more_btn = popup_add_styled_button(nfc_scan_popup, "More", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, nfc_scan_more_cb, NULL);
    if (nfc_scan_more_btn) lv_obj_add_flag(nfc_scan_more_btn, LV_OBJ_FLAG_HIDDEN);

    // Save button (hidden until a tag is scanned)
    nfc_scan_save_btn = popup_add_styled_button(nfc_scan_popup, "Save", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_scan_save_cb, NULL);
    if (nfc_scan_save_btn) lv_obj_add_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);

    // Scroll button (hidden until Parsed view)
    nfc_scan_scroll_btn = popup_add_styled_button(nfc_scan_popup, "Scroll", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_scan_scroll_cb, NULL);
    if (nfc_scan_scroll_btn) lv_obj_add_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);

    // Initial state: only cancel visible, centered
    nfc_more_visible = false;
    nfc_save_visible = false;
    nfc_popup_selected = 0;
    nfc_details_visible = false;
    update_nfc_buttons_layout();
    update_nfc_popup_selection();
#if defined(CONFIG_NFC_CHAMELEON)
    if (chameleon_manager_is_ready()) {
        ESP_LOGI(TAG, "create_nfc_scan_popup: starting CU scan task");
        nfc_scan_cancel = false;
        xTaskCreate(nfc_scan_cu_task, "nfc_scan_cu", 4096, NULL, 5, NULL);
        return;
    }
#endif
#ifdef NFC_HAS_LOCAL_READER
    // Since we force-delete stuck tasks in cleanup, we should never have a running task here
    if (nfc_scan_task_handle != NULL) {
        ESP_LOGE(TAG, "create_nfc_scan_popup: unexpected running task, force cleaning up");
        vTaskDelete(nfc_scan_task_handle);
        nfc_scan_task_handle = NULL;
        if (g_pn532) {
            pn532_release(g_pn532);
            pn532_delete_driver(g_pn532);
            g_pn532 = NULL;
        }
    }
    ESP_LOGI(TAG, "create_nfc_scan_popup: starting scan task");
    status_display_show_status("NFC Scanning...");
    nfc_scan_cancel = false;
    mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
    xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
#endif
}

#ifdef NFC_HAS_LOCAL_READER
static void nfc_try_start_scan_timer_cb(lv_timer_t *t) {
    if (nfc_scan_task_handle == NULL) {
        ESP_LOGI(TAG, "nfc_try_start_scan_timer_cb: starting scan task after prior exit");
        nfc_scan_cancel = false;
        mfc_set_progress_callback(mfc_dict_progress_cb, NULL);
        xTaskCreate(nfc_scan_task, "nfc_scan", 6144, NULL, 5, &nfc_scan_task_handle);
        if (t) {
            lv_timer_del(t);
            nfc_scan_retry_timer = NULL;
        }
    }
}
#endif

// Run heavy save on a worker task to avoid blocking LVGL thread
static void nfc_save_task(void *arg) {
    bool ok = write_flipper_nfc_file();
    // Notify UI on completion with result
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(nfc_save_done_async, res); }
    else { display_manager_lvgl_async_call(nfc_save_done_async, NULL); }
    nfc_save_in_progress = false;
    vTaskDelete(NULL);
}

static void nfc_save_done_async(void *ptr) {
    bool ok = (ptr != NULL) ? *((bool*)ptr) : false;
    if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
        lv_label_set_text(nfc_title_label, ok ? "Saved!" : "Save failed");
    }
    if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
        lv_obj_clear_state(nfc_scan_save_btn, LV_STATE_DISABLED);
    }
    
    // Add status display messages for NFC save result
    if (ok) {
        status_display_show_status("NFC Saved");
    } else {
        status_display_show_status("NFC Save Fail");
    }
#ifdef NFC_HAS_LOCAL_READER
    mfc_set_progress_callback(NULL, NULL);
#endif
    if (ptr) nfc_bool_pool_free(ptr);
}

static void update_nfc_popup_selection(void) {
    if (!nfc_scan_cancel_btn) return;
    
    // Update Cancel button
    popup_set_button_selected(nfc_scan_cancel_btn, nfc_popup_selected == 0);
    
    // Update More button if visible
    if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn) && nfc_more_visible) {
        popup_set_button_selected(nfc_scan_more_btn, nfc_popup_selected == 1);
    }
    // Update right-side action button: Save (summary/basic) or Scroll (parsed view)
    int right_index = nfc_more_visible ? 2 : 1;
    if (nfc_details_view_mode != 2) {
        if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn) && nfc_save_visible) {
            popup_set_button_selected(nfc_scan_save_btn, nfc_popup_selected == right_index);
        }
    } else {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn) &&
            !lv_obj_has_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN)) {
            popup_set_button_selected(nfc_scan_scroll_btn, nfc_popup_selected == right_index);
        }
    }
    // Re-apply button layout after selection/style changes so sizes stay consistent
    update_nfc_buttons_layout();
}

static void update_nfc_buttons_layout(void) {
    if (!nfc_scan_cancel_btn || !nfc_scan_popup) return;

    int yoff = nfc_details_visible ? -8 : -10;
    lv_obj_t *btns[3];
    int count = 0;

    btns[count++] = nfc_scan_cancel_btn;

    if (nfc_more_visible && nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
        btns[count++] = nfc_scan_more_btn;
    }

    // In Parsed view (Mode 2), show Scroll button instead of Save
    if (nfc_details_view_mode == 2) {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn)) {
            lv_obj_clear_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);
            if (nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) lv_obj_add_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
            btns[count++] = nfc_scan_scroll_btn;
        }
    } else {
        if (nfc_scan_scroll_btn && lv_obj_is_valid(nfc_scan_scroll_btn)) lv_obj_add_flag(nfc_scan_scroll_btn, LV_OBJ_FLAG_HIDDEN);
        if (nfc_save_visible && nfc_scan_save_btn && lv_obj_is_valid(nfc_scan_save_btn)) {
            lv_obj_clear_flag(nfc_scan_save_btn, LV_OBJ_FLAG_HIDDEN);
            btns[count++] = nfc_scan_save_btn;
        }
    }

    popup_layout_buttons_responsive(nfc_scan_popup, btns, count, yoff, NULL);
}

static void update_saved_buttons_layout(void) {
    if (!saved_close_btn || !saved_popup) return;

    lv_obj_t *btns[3];
    int count = 0;

    if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) btns[count++] = saved_close_btn;
    if (saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) btns[count++] = saved_rename_btn;
    if (saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) btns[count++] = saved_delete_btn;

    if (count == 0) return;

    popup_layout_buttons_responsive(saved_popup, btns, count, -8, NULL);
}

static void saved_update_button_labels(void) {
    if (saved_close_btn && lv_obj_is_valid(saved_close_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(saved_close_btn, 0);
        if (lbl) {
            if (!saved_has_extra_details) {
                lv_label_set_text(lbl, "Cancel");
            } else {
                lv_label_set_text(lbl, saved_details_parsed_view ? "Close" : "More");
            }
        }
    }
    if (saved_rename_btn && lv_obj_is_valid(saved_rename_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(saved_rename_btn, 0);
        if (lbl) lv_label_set_text(lbl, saved_details_parsed_view ? "Less" : "Rename");
    }
    if (saved_delete_btn && lv_obj_is_valid(saved_delete_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(saved_delete_btn, 0);
        if (lbl) lv_label_set_text(lbl, saved_details_parsed_view ? "Scroll" : "Delete");
    }
}

static void saved_update_details_label(bool parsed) {
    if (!saved_details_label || !lv_obj_is_valid(saved_details_label)) return;
    const char *src = saved_details_text;
    if (!src) {
        lv_label_set_text(saved_details_label, "");
        return;
    }

    const char *final_text = src;
    char *tmp = NULL;

    if (!parsed) {
        const char *split = get_details_split_point(src);
        if (split) {
            size_t len = (size_t)(split - src);
            tmp = (char*)malloc(len + 1);
            if (tmp) {
                memcpy(tmp, src, len);
                tmp[len] = '\0';
                final_text = tmp;
            }
        }
    } else {
        const char *split = get_details_split_point(src);
        if (split) {
            final_text = split;
            if (final_text[0] == '#') {
                const char *nl = strchr(final_text, '\n');
                if (nl) final_text = nl + 1;
            }
            while (final_text[0] == '\n' || final_text[0] == '\r') final_text++;
        }
    }

    lv_label_set_text(saved_details_label, final_text);
    // Match scan popup: wrapped, left-aligned text inside the scroll area
    lv_label_set_long_mode(saved_details_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(saved_details_label, LV_TEXT_ALIGN_LEFT, 0);
    if (saved_scroll && lv_obj_is_valid(saved_scroll)) {
        lv_coord_t scroll_w = lv_obj_get_width(saved_scroll);
        if (scroll_w > 4) {
            lv_obj_set_width(saved_details_label, scroll_w - 4);
        }
        lv_obj_align(saved_details_label, LV_ALIGN_TOP_MID, 0, 0);
    }
    if (tmp) free(tmp);
}

static void saved_show_parsed_view(bool parsed) {
    saved_details_parsed_view = parsed;
    if (saved_scroll && lv_obj_is_valid(saved_scroll)) {
        lv_obj_clear_flag(saved_scroll, LV_OBJ_FLAG_HIDDEN);
    }
    if (saved_details_label && lv_obj_is_valid(saved_details_label)) {
        lv_obj_clear_flag(saved_details_label, LV_OBJ_FLAG_HIDDEN);
    }
    saved_update_details_label(parsed);
    saved_update_button_labels();
    saved_popup_selected = parsed ? 1 : 0;
    update_saved_popup_selection();
}

static void update_keys_buttons_layout(void) {
    if (!keys_close_btn || !keys_popup) return;

    lv_obj_t *btns[3];
    int count = 0;

    if (keys_up_btn && lv_obj_is_valid(keys_up_btn)) btns[count++] = keys_up_btn;
    if (keys_close_btn && lv_obj_is_valid(keys_close_btn)) btns[count++] = keys_close_btn;
    if (keys_down_btn && lv_obj_is_valid(keys_down_btn)) btns[count++] = keys_down_btn;

    if (count == 0) return;

    PopupButtonLayoutConfig cfg = {0};
    cfg.min_w = (LV_HOR_RES <= 240) ? 48 : 54;
    cfg.max_w = (LV_HOR_RES <= 240) ? 100 : 130;
    cfg.min_threshold = 40;
    popup_layout_buttons_responsive(keys_popup, btns, count, -8, &cfg);
}

static void nfc_update_details_scroll_layout(void) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) return;
    if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) return;
    if (!nfc_title_label || !lv_obj_is_valid(nfc_title_label)) return;
    if (!nfc_scan_cancel_btn || !lv_obj_is_valid(nfc_scan_cancel_btn)) return;

    lv_obj_update_layout(nfc_scan_popup);

    lv_area_t popup_a;
    lv_area_t title_a;
    lv_area_t btn_a;
    lv_obj_get_coords(nfc_scan_popup, &popup_a);
    lv_obj_get_coords(nfc_title_label, &title_a);
    lv_obj_get_coords(nfc_scan_cancel_btn, &btn_a);

    lv_coord_t popup_w = lv_obj_get_width(nfc_scan_popup);
    lv_coord_t popup_h = lv_obj_get_height(nfc_scan_popup);

    lv_coord_t top_y = title_a.y2 - popup_a.y1 + 2;
    if (top_y < 0) top_y = 0;

    lv_coord_t bottom_y = btn_a.y1 - 4;
    if (bottom_y > popup_h - 4) bottom_y = popup_h - 4;

    lv_coord_t scroll_h = bottom_y - top_y;
    if (scroll_h <= 0) return;

    const lv_font_t *font = NULL;
    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        font = lv_obj_get_style_text_font(nfc_details_label, LV_PART_MAIN);
    }
    lv_coord_t line_h = font ? lv_font_get_line_height(font) : 0;
    if (line_h > 0 && scroll_h > line_h) {
        // Make the viewport slightly shorter than the text height so scrolling always has effect
        scroll_h -= line_h;
    }

    lv_coord_t scroll_w = popup_w - 20;
    if (scroll_w < 20) scroll_w = popup_w;

    lv_obj_set_size(nfc_details_scroll, scroll_w, scroll_h);
    lv_obj_align(nfc_details_scroll, LV_ALIGN_TOP_MID, 0, top_y);

    if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
        lv_obj_set_width(nfc_details_label, scroll_w - 4);
        lv_obj_align(nfc_details_label, LV_ALIGN_TOP_LEFT, 0, 0);
    }

    lv_obj_update_layout(nfc_details_scroll);
}

static void nfc_show_details_view(bool show) {
    if (!nfc_scan_popup || !lv_obj_is_valid(nfc_scan_popup)) return;
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();
    if (show) {
        // Hide summary fields
        if (nfc_uid_label) lv_obj_add_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_add_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        // Title and button label
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, "NFC Details");
            // Details title slightly down from top for spacing
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 4);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            const char *btn_text = "Less";
            if (nfc_details_view_mode == 1) {
                // In basic mode, button leads to Parsed if available, else back to Summary (Less)
                if (has_extra_details(nfc_details_text)) btn_text = "Parsed";
            }
            if (lbl) lv_label_set_text(lbl, btn_text);
        }
        // Create details scroll container if needed
        if (!nfc_details_scroll || !lv_obj_is_valid(nfc_details_scroll)) {
            lv_coord_t popup_w = lv_obj_get_width(nfc_scan_popup);
            lv_coord_t popup_h = lv_obj_get_height(nfc_scan_popup);
            lv_coord_t scroll_h = popup_h - 90; // Leave a bit more room for title and buttons
            if (scroll_h < 50) scroll_h = 50;
            nfc_details_scroll = popup_create_scroll_area(nfc_scan_popup, popup_w - 20, scroll_h, LV_ALIGN_TOP_MID, 0, 35);
        }

        // Create details label inside scroll container
        if (!nfc_details_label || !lv_obj_is_valid(nfc_details_label)) {
            lv_coord_t text_w = lv_obj_get_width(nfc_details_scroll) - 4;
            nfc_details_label = popup_create_body_label(nfc_details_scroll, "", text_w, true, body_font, 0);
        } else if (lv_obj_get_parent(nfc_details_label) != nfc_details_scroll) {
            // Reparent if label was created in a different view mode
            lv_obj_set_parent(nfc_details_label, nfc_details_scroll);
            lv_coord_t text_w = lv_obj_get_width(nfc_details_scroll) - 4;
            lv_obj_set_width(nfc_details_label, text_w);
        }

        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
            lv_obj_align(nfc_details_label, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_label_set_long_mode(nfc_details_label, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(nfc_details_label, LV_TEXT_ALIGN_CENTER, 0);
        }
        // Set details text
        const char *source_text = NULL;
        #if defined(CONFIG_NFC_CHAMELEON)
        if (using_chameleon_backend()) {
            source_text = nfc_details_text;
        } else
        #endif
        {
            #ifdef NFC_HAS_LOCAL_READER
            source_text = nfc_details_text;
            #endif
        }

        const char *final_text = "Reading tag data...";
        char *tmp_text = NULL;

        if (nfc_details_ready && source_text) {
            final_text = source_text;
            // specific logic for mode 1 (Basic) truncation
            if (nfc_details_view_mode == 1) {
                const char *split = get_details_split_point(source_text);
                if (split) {
                    size_t len = split - source_text;
                    tmp_text = (char*)malloc(len + 1);
                    if (tmp_text) {
                        memcpy(tmp_text, source_text, len);
                        tmp_text[len] = '\0';
                        final_text = tmp_text;
                    }
                }
            }
            // specific logic for mode 2 (Parsed) - show only the tail
            else if (nfc_details_view_mode == 2) {
                const char *split = get_details_split_point(source_text);
                if (split) {
                    final_text = split;
                    // Remove header line (e.g. #SmartRider) if present
                    if (final_text[0] == '#') {
                        const char *nl = strchr(final_text, '\n');
                        if (nl) final_text = nl + 1;
                    }
                    // Trim leading newlines to remove extra gap
                    while (final_text[0] == '\n' || final_text[0] == '\r') {
                        final_text++;
                    }
                }
            }
        } else {
            #ifndef NFC_HAS_LOCAL_READER
            #ifndef CONFIG_NFC_CHAMELEON
            final_text = "NFC not available";
            #endif
            #endif
        }

        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) {
            lv_label_set_text(nfc_details_label, final_text);
        }
        if (tmp_text) free(tmp_text);

        if (nfc_details_scroll && lv_obj_is_valid(nfc_details_scroll)) {
            lv_obj_scroll_to_y(nfc_details_scroll, 0, LV_ANIM_OFF);
            lv_obj_clear_flag(nfc_details_scroll, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        nfc_details_visible = true;
        nfc_popup_selected = 1; // focus Less button
        update_nfc_popup_selection();
        nfc_update_details_scroll_layout();
    } else {
        nfc_details_view_mode = 0; // Reset mode when hiding
        // Hide details, show summary (keep spacing consistent for 240x320 too)
        if (nfc_details_scroll && lv_obj_is_valid(nfc_details_scroll)) lv_obj_add_flag(nfc_details_scroll, LV_OBJ_FLAG_HIDDEN);
        if (nfc_details_label && lv_obj_is_valid(nfc_details_label)) lv_obj_add_flag(nfc_details_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_uid_label) lv_obj_clear_flag(nfc_uid_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_type_label) lv_obj_clear_flag(nfc_type_label, LV_OBJ_FLAG_HIDDEN);
        if (nfc_title_label && lv_obj_is_valid(nfc_title_label)) {
            lv_label_set_text(nfc_title_label, nfc_get_detected_title());
            lv_obj_align(nfc_title_label, LV_ALIGN_TOP_MID, 0, 22);
        }
        if (nfc_scan_more_btn && lv_obj_is_valid(nfc_scan_more_btn)) {
            lv_obj_t *lbl = lv_obj_get_child(nfc_scan_more_btn, 0);
            if (lbl) lv_label_set_text(lbl, "More");
        }
        nfc_details_visible = false;
        nfc_popup_selected = 0; // focus Cancel
        update_nfc_popup_selection();
        // Update buttons layout for summary spacing
        update_nfc_buttons_layout();
    }
}

// ---- Write Flow Implementation ----
static bool has_nfc_ext(const char *name) {
    if (!name) return false;
    size_t len = strlen(name);
    if (len >= 4) {
        const char *ext = name + (len - 4);
        if (ext[0] == '.' && (ext[1] == 'n' || ext[1] == 'N') && (ext[2] == 'f' || ext[2] == 'F') && (ext[3] == 'c' || ext[3] == 'C'))
            return true;
    }
    if (len >= 9) {
        const char *ext = name + (len - 9);
        if (strcasecmp(ext, ".picopass") == 0)
            return true;
    }
    return false;
}

static void nfc_clear_write_list(void) {
    if (nfc_file_paths) {
        for (size_t i = 0; i < nfc_file_count; ++i) {
            free(nfc_file_paths[i]);
        }
        free(nfc_file_paths);
    }
    nfc_file_paths = NULL;
    nfc_file_count = 0;
}

static void nfc_clear_emulate_list(void) {
    if (nfc_emu_file_paths) {
        for (size_t i = 0; i < nfc_emu_file_count; ++i) {
            free(nfc_emu_file_paths[i]);
        }
        free(nfc_emu_file_paths);
    }
    nfc_emu_file_paths = NULL;
    nfc_emu_file_count = 0;
}

static void nfc_file_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    create_nfc_write_popup(path);
}

static void nfc_emulate_file_item_cb(lv_event_t *e);
static void nfc_emulate_test_cb(lv_event_t *e);

// ---- Generate-tag flow -------------------------------------------------
// Builds an NDEF record from user-entered fields, wraps it in a blank
// NTAG215 image, and saves it as a .nfc file. The file lands in the same
// /mnt/ghostesp/nfc directory the Saved/Write/Emulate lists already scan,
// so no further plumbing is needed to use a generated tag.

typedef struct {
    uint8_t *ndef;
    size_t ndef_len;
    char name_hint[32];
} nfc_gen_job_t;

typedef struct {
    bool ok;
    char path[224];
} nfc_gen_result_t;

static void nfc_generate_done_cb(void *ptr) {
    nfc_gen_result_t *res = (nfc_gen_result_t *)ptr;
    if (!res) return;
    if (res->ok) {
        const char *slash = strrchr(res->path, '/');
        const char *base = slash ? slash + 1 : res->path;
        char name[64];
        size_t namelen = strlen(base);
        if (namelen >= sizeof(name)) namelen = sizeof(name) - 1;
        memcpy(name, base, namelen);
        name[namelen] = '\0';
        char msg[256];
        snprintf(msg, sizeof(msg), "Tag saved as %s\nUse Write or Emulate to use it.", name);
        error_popup_create(msg);
    } else {
        error_popup_create("Failed to generate NFC tag");
    }
    free(res);
    back_to_root_menu();
}

static void nfc_generate_task(void *arg) {
    nfc_gen_job_t *job = (nfc_gen_job_t *)arg;
    nfc_gen_result_t *res = (nfc_gen_result_t *)calloc(1, sizeof(nfc_gen_result_t));
    if (!job) { vTaskDelete(NULL); return; }
    if (res) {
        bool susp = false; bool did = nfc_sd_begin(&susp);
        res->ok = ndef_tag_gen_save_file(NTAG2XX_NTAG215, job->ndef, job->ndef_len,
                                         job->name_hint, res->path, sizeof(res->path));
        if (did) nfc_sd_end(susp);
        display_manager_lvgl_async_call(nfc_generate_done_cb, res);
    }
    free(job->ndef);
    free(job);
    vTaskDelete(NULL);
}

// Takes ownership of ndef (frees it) and always leaves the view on nfc_view.
static void nfc_generate_submit(uint8_t *ndef, size_t ndef_len, const char *name_hint) {
    display_manager_switch_view(&nfc_view);
    if (!ndef || ndef_len == 0) {
        error_popup_create("Could not build that NFC tag from the given input");
        return;
    }
    nfc_gen_job_t *job = (nfc_gen_job_t *)malloc(sizeof(nfc_gen_job_t));
    if (!job) { free(ndef); error_popup_create("Out of memory"); return; }
    job->ndef = ndef;
    job->ndef_len = ndef_len;
    strncpy(job->name_hint, name_hint ? name_hint : "tag", sizeof(job->name_hint) - 1);
    job->name_hint[sizeof(job->name_hint) - 1] = '\0';

    BaseType_t rc = xTaskCreate(nfc_generate_task, "nfc_gen", 4096, job, 5, NULL);
    if (rc != pdPASS) {
        free(job->ndef);
        free(job);
        error_popup_create("Failed to start tag generation");
    }
}

static void nfc_gen_url_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    char uri[192];
    // If the user didn't type a scheme, assume https:// so real phones treat it as a link.
    if (strstr(text, "://") || strchr(text, ':')) {
        strncpy(uri, text, sizeof(uri) - 1); uri[sizeof(uri) - 1] = '\0';
    } else {
        snprintf(uri, sizeof(uri), "https://%s", text);
    }
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_uri(uri, &ndef, &len);
    nfc_generate_submit(ndef, len, "url");
}
static void nfc_generate_url_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_gen_url_kb_cb);
    keyboard_view_set_placeholder("https://example.com");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

static void nfc_gen_text_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_text(text, &ndef, &len);
    nfc_generate_submit(ndef, len, "text");
}
static void nfc_generate_text_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_gen_text_kb_cb);
    keyboard_view_set_placeholder("Note text");
    keyboard_view_set_start_caps(true);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

#ifdef NFC_HAS_LOCAL_READER
// Manually add a MIFARE Classic key to the user dictionary via the keyboard.
static void nfc_add_key_kb_cb(const char *text) {
    if (text && *text) {
        if (mfc_add_user_key_hex(text)) {
            toast_show("Key added to user dict", TOAST_SUCCESS);
        } else {
            toast_show("Invalid key (need 12 hex)", TOAST_ERROR);
        }
    }
    display_manager_switch_view(&nfc_view);
}
static void nfc_add_key_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_add_key_kb_cb);
    keyboard_view_set_placeholder("A0A1A2A3A4A5");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
#endif

static void nfc_gen_phone_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    char uri[144];
    snprintf(uri, sizeof(uri), "tel:%s", text);
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_uri(uri, &ndef, &len);
    nfc_generate_submit(ndef, len, "phone");
}
static void nfc_generate_phone_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_gen_phone_kb_cb);
    keyboard_view_set_placeholder("+15551234567");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

static void nfc_gen_email_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    char uri[192];
    snprintf(uri, sizeof(uri), "mailto:%s", text);
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_uri(uri, &ndef, &len);
    nfc_generate_submit(ndef, len, "email");
}
static void nfc_generate_email_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_gen_email_kb_cb);
    keyboard_view_set_placeholder("name@example.com");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

static void nfc_gen_aar_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_aar(text, &ndef, &len);
    nfc_generate_submit(ndef, len, "app");
}
static void nfc_generate_aar_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_submit_callback(nfc_gen_aar_kb_cb);
    keyboard_view_set_placeholder("com.example.app");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

// Wi-Fi: SSID -> password -> security-type submenu -> generate.
static void nfc_gen_wifi_auth_cb(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    ndef_wifi_auth_t auth = NDEF_WIFI_AUTH_WPA2;
    if (label) {
        if (!strcmp(label, "Open")) auth = NDEF_WIFI_AUTH_OPEN;
        else if (!strcmp(label, "WEP")) auth = NDEF_WIFI_AUTH_WEP;
        else if (!strcmp(label, "WPA")) auth = NDEF_WIFI_AUTH_WPA;
        else auth = NDEF_WIFI_AUTH_WPA2;
    }
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_wifi(g_gen_field1, g_gen_field2, auth, &ndef, &len);
    nfc_generate_submit(ndef, len, "wifi");
}
static void nfc_gen_wifi_show_auth_menu(void) {
    if (!g_nfc_ov) { display_manager_switch_view(&nfc_view); return; }
    display_manager_switch_view(&nfc_view);
    in_generate_list = true;
    options_view_clear(g_nfc_ov);
    options_view_set_title(g_nfc_ov, "Wi-Fi Security");
    options_view_add_item(g_nfc_ov, "WPA/WPA2", nfc_gen_wifi_auth_cb, (void *)"WPA2");
    options_view_add_item(g_nfc_ov, "WPA", nfc_gen_wifi_auth_cb, (void *)"WPA");
    options_view_add_item(g_nfc_ov, "WEP", nfc_gen_wifi_auth_cb, (void *)"WEP");
    options_view_add_item(g_nfc_ov, "Open (no password)", nfc_gen_wifi_auth_cb, (void *)"Open");
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
}
static void nfc_gen_wifi_pass_kb_cb(const char *text) {
    strncpy(g_gen_field2, text ? text : "", sizeof(g_gen_field2) - 1);
    g_gen_field2[sizeof(g_gen_field2) - 1] = '\0';
    nfc_gen_wifi_show_auth_menu();
}
static void nfc_gen_wifi_ssid_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    strncpy(g_gen_field1, text, sizeof(g_gen_field1) - 1);
    g_gen_field1[sizeof(g_gen_field1) - 1] = '\0';
    keyboard_view_set_submit_callback(nfc_gen_wifi_pass_kb_cb);
    keyboard_view_set_placeholder("Password (blank = open)");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
static void nfc_generate_wifi_cb(lv_event_t *e) {
    (void)e;
    g_gen_field1[0] = '\0';
    g_gen_field2[0] = '\0';
    keyboard_view_set_submit_callback(nfc_gen_wifi_ssid_kb_cb);
    keyboard_view_set_placeholder("SSID");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

// Contact: name -> phone -> email -> generate.
static void nfc_gen_vcard_email_kb_cb(const char *text) {
    strncpy(g_gen_field3, text ? text : "", sizeof(g_gen_field3) - 1);
    g_gen_field3[sizeof(g_gen_field3) - 1] = '\0';
    uint8_t *ndef = NULL; size_t len = 0;
    ndef_builder_vcard(g_gen_field1, g_gen_field2, g_gen_field3, &ndef, &len);
    nfc_generate_submit(ndef, len, "contact");
}
static void nfc_gen_vcard_phone_kb_cb(const char *text) {
    strncpy(g_gen_field2, text ? text : "", sizeof(g_gen_field2) - 1);
    g_gen_field2[sizeof(g_gen_field2) - 1] = '\0';
    keyboard_view_set_submit_callback(nfc_gen_vcard_email_kb_cb);
    keyboard_view_set_placeholder("Email (optional)");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
static void nfc_gen_vcard_name_kb_cb(const char *text) {
    if (!text || !*text) { display_manager_switch_view(&nfc_view); return; }
    strncpy(g_gen_field1, text, sizeof(g_gen_field1) - 1);
    g_gen_field1[sizeof(g_gen_field1) - 1] = '\0';
    keyboard_view_set_submit_callback(nfc_gen_vcard_phone_kb_cb);
    keyboard_view_set_placeholder("Phone (optional)");
    keyboard_view_set_start_caps(false);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
static void nfc_generate_vcard_cb(lv_event_t *e) {
    (void)e;
    g_gen_field1[0] = '\0';
    g_gen_field2[0] = '\0';
    g_gen_field3[0] = '\0';
    keyboard_view_set_submit_callback(nfc_gen_vcard_name_kb_cb);
    keyboard_view_set_placeholder("Full name");
    keyboard_view_set_start_caps(true);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}

static void nfc_clear_generate_list(void) {
    g_gen_field1[0] = '\0';
    g_gen_field2[0] = '\0';
    g_gen_field3[0] = '\0';
}

static void nfc_enter_generate_list(void) {
    if (!g_nfc_ov) return;
    in_generate_list = true;
    in_tools_menu = false;
    nfc_clear_generate_list();
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif
    options_view_set_title(g_nfc_ov, "Generate Tag");
    options_view_add_item(g_nfc_ov, "URL / Link", nfc_generate_url_cb, NULL);
    options_view_add_item(g_nfc_ov, "Text Note", nfc_generate_text_cb, NULL);
    options_view_add_item(g_nfc_ov, "Phone Number", nfc_generate_phone_cb, NULL);
    options_view_add_item(g_nfc_ov, "Email", nfc_generate_email_cb, NULL);
    options_view_add_item(g_nfc_ov, "Wi-Fi Network", nfc_generate_wifi_cb, NULL);
    options_view_add_item(g_nfc_ov, "Contact (vCard)", nfc_generate_vcard_cb, NULL);
    options_view_add_item(g_nfc_ov, "Android App", nfc_generate_aar_cb, NULL);
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}
// ---- end generate-tag flow ----------------------------------------------

static void nfc_enter_mfc_menu(void) {
    if (!g_nfc_ov) return;
    in_mfc_menu = true;
    in_tools_menu = false;
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    options_view_set_title(g_nfc_ov, "MIFARE Classic");
#ifdef NFC_HAS_LOCAL_READER
    options_view_add_item(g_nfc_ov, "Add MFC Key", nfc_add_key_cb, NULL);
#endif
    options_view_add_item(g_nfc_ov, "User Keys", nfc_option_event_cb, (void *)"User Keys");
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}

static void nfc_enter_tools_menu(void) {
    if (!g_nfc_ov) return;
    in_tools_menu = true;
    in_mfc_menu = false;
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    options_view_set_title(g_nfc_ov, "NFC Tools");
    options_view_add_item(g_nfc_ov, "Generate NDEF", nfc_option_event_cb, (void *)"Generate NDEF");
    options_view_add_item(g_nfc_ov, "Emulate", nfc_option_event_cb, (void *)"Emulate");
    emulate_btn = options_view_add_item(g_nfc_ov, "Write", nfc_option_event_cb, (void *)"Write");
#if defined(CONFIG_NFC_CHAMELEON)
    options_view_add_item(g_nfc_ov, "Chameleon Ultra", nfc_option_event_cb, (void *)"Chameleon Ultra");
#endif
    options_view_add_item(g_nfc_ov, "NFC Credits", nfc_option_event_cb, (void *)"NFC Credits");
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}

static void back_to_root_menu(void) {
    if (!root || !g_nfc_ov) return;
    in_write_list = false;
    in_emulate_list = false;
    in_saved_list = false;
    in_generate_list = false;
    in_mfc_menu = false;
    in_tools_menu = false;
    nfc_clear_write_list();
    nfc_clear_emulate_list();
    nfc_clear_generate_list();
    saved_clear_list();
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    scan_btn = options_view_add_item(g_nfc_ov, "Scan", nfc_option_event_cb, (void *)"Scan");
    if (scan_btn) lv_obj_set_user_data(scan_btn, (void *)"Scan");
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    nfc_add_backend_item();
#endif
#ifdef CONFIG_NFC_ST25R3916
    options_view_add_item(g_nfc_ov, "iCLASS / PicoPass", nfc_option_event_cb, (void *)"iCLASS / PicoPass");
#endif
    options_view_add_item(g_nfc_ov, "Saved", nfc_option_event_cb, (void *)"Saved");
    options_view_add_item(g_nfc_ov, "MIFARE Classic", nfc_option_event_cb, (void *)"MIFARE Classic");
    options_view_add_item(g_nfc_ov, "Tools", nfc_option_event_cb, (void *)"Tools");
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, nfc_option_event_cb, (void *)"__BACK_OPTION__");
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}

static void nfc_enter_write_list(void) {
    if (!g_nfc_ov) return;
    in_write_list = true;
    nfc_clear_write_list();
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    DIR *d = did ? opendir(dir) : NULL;
    if (d) {
        struct dirent *de;
        size_t count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (has_nfc_ext(de->d_name)) count++;
        }
        rewinddir(d);
        if (count > 0) nfc_file_paths = (char**)calloc(count, sizeof(char*));
        size_t idx = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!has_nfc_ext(de->d_name)) continue;
            size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1;
            char *copy = (char*)malloc(need);
            if (!copy) continue;
            snprintf(copy, need, "%s/%s", dir, de->d_name);
            if (nfc_file_paths && idx < count) nfc_file_paths[idx++] = copy;
            ESP_LOGI(TAG, "nfc_enter_write_list: %s", copy);
            options_view_add_item(g_nfc_ov, de->d_name, nfc_file_item_cb, copy);
        }
        nfc_file_count = idx;
        ESP_LOGI(TAG, "nfc_enter_write_list: %u .nfc files", (unsigned)nfc_file_count);
        closedir(d);
    }

    if (nfc_file_count == 0) {
        options_view_add_item(g_nfc_ov, "No .nfc files", NULL, NULL);
    }

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    if (did) nfc_sd_end(susp);
}

static void nfc_enter_emulate_list(void) {
    if (!g_nfc_ov) return;
    in_emulate_list = true;
    nfc_clear_emulate_list();
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    options_view_add_item(g_nfc_ov, "NDEF URL Test", nfc_emulate_test_cb, NULL);

    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    DIR *d = did ? opendir(dir) : NULL;
    if (d) {
        struct dirent *de;
        size_t count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (has_nfc_ext(de->d_name)) count++;
        }
        rewinddir(d);
        if (count > 0) nfc_emu_file_paths = (char**)calloc(count, sizeof(char*));
        size_t idx = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!has_nfc_ext(de->d_name)) continue;
            size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1;
            char *copy = (char*)malloc(need);
            if (!copy) continue;
            snprintf(copy, need, "%s/%s", dir, de->d_name);
            if (nfc_emu_file_paths && idx < count) nfc_emu_file_paths[idx++] = copy;
            options_view_add_item(g_nfc_ov, de->d_name, nfc_emulate_file_item_cb, copy);
        }
        nfc_emu_file_count = idx;
        closedir(d);
    }
    if (did) nfc_sd_end(susp);

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}

void saved_clear_list(void) {
    if (saved_file_paths) {
        for (size_t i = 0; i < saved_file_count; ++i) free(saved_file_paths[i]);
        free(saved_file_paths);
    }
    saved_file_paths = NULL;
    saved_file_count = 0;
}

static void saved_file_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    create_saved_details_popup(path);
}

static void saved_enter_list(void) {
    if (!g_nfc_ov) return;
    in_saved_list = true;
    saved_clear_list();
    options_view_clear(g_nfc_ov);
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    backend_btn = NULL;
#endif

    const char *dir = "/mnt/ghostesp/nfc";
    bool susp = false; bool did = nfc_sd_begin(&susp);
    DIR *d = did ? opendir(dir) : NULL;
    if (d) {
        struct dirent *de; size_t count = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (has_nfc_ext(de->d_name)) count++;
        }
        rewinddir(d);
        if (count > 0) saved_file_paths = (char**)calloc(count, sizeof(char*));
        size_t idx = 0;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (!has_nfc_ext(de->d_name)) continue;
            size_t need = strlen(dir) + 1 + strlen(de->d_name) + 1;
            char *copy = (char*)malloc(need);
            if (!copy) continue;
            snprintf(copy, need, "%s/%s", dir, de->d_name);
            if (saved_file_paths && idx < count) saved_file_paths[idx++] = copy;
            ESP_LOGI(TAG, "saved_enter_list: %s", copy);
            options_view_add_item(g_nfc_ov, de->d_name, saved_file_item_cb, copy);
        }
        saved_file_count = idx;
        ESP_LOGI(TAG, "saved_enter_list: %u .nfc files", (unsigned)saved_file_count);
        closedir(d);
    }
    if (did) nfc_sd_end(susp);

    if (saved_file_count == 0) {
        options_view_add_item(g_nfc_ov, "No .nfc files", NULL, NULL);
    }

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, back_event_cb, NULL);
#endif
    num_items = options_view_get_item_count(g_nfc_ov);
    selected_index = 0;
    options_view_set_selected(g_nfc_ov, 0);
    update_nfc_scroll_buttons_visibility();
}

static void update_nfc_write_popup_selection(void) {
    // Update Cancel button
    if (nfc_write_cancel_btn && lv_obj_is_valid(nfc_write_cancel_btn)) {
        popup_set_button_selected(nfc_write_cancel_btn, nfc_write_popup_selected == 0);
    }
    
    // Update Write button
    if (nfc_write_go_btn && lv_obj_is_valid(nfc_write_go_btn)) {
        popup_set_button_selected(nfc_write_go_btn, nfc_write_popup_selected == 1);
    }
}

static void update_nfc_emu_popup_selection(void) {
    if (nfc_emu_cancel_btn && lv_obj_is_valid(nfc_emu_cancel_btn)) {
        popup_set_button_selected(nfc_emu_cancel_btn, nfc_emu_popup_selected == 0);
    }
}

static void cleanup_nfc_emu_popup(void *obj) {
    (void)obj;
    if (nfc_emu_active) {
#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)
        nfc_cli_stop();
#endif
        nfc_emu_active = false;
    }
    lvgl_obj_del_safe(&nfc_emu_popup);
    nfc_emu_cancel_btn = NULL;
    nfc_emu_title_label = NULL;
    nfc_emu_details_label = NULL;
    nfc_emu_popup_selected = 0;
    status_display_show_status("NFC Emu Stopped");
}

static void nfc_emu_cancel_cb(lv_event_t *e) {
    (void)e;
    cleanup_nfc_emu_popup(NULL);
}

static void create_nfc_emu_popup(const char *path, bool test_ndef) {
    if (!root) return;
    if (nfc_emu_popup && lv_obj_is_valid(nfc_emu_popup)) cleanup_nfc_emu_popup(NULL);

    popup_calc_size_t geom;
    popup_calc_size_ex(&geom, 100);
    nfc_emu_popup = popup_create_container_with_offset(lv_scr_act(), geom.width, geom.height, geom.y_offset, true);
    if (nfc_emu_popup) lv_obj_add_flag(nfc_emu_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();
    nfc_emu_title_label = popup_create_title_label(nfc_emu_popup, "NFC Emulation", title_font, 12);

    char details[220];
    if (test_ndef) {
        snprintf(details, sizeof(details), "Emulating NTAG213\nNDEF URL test\nHold phone near antenna");
    } else {
        const char *name = path ? strrchr(path, '/') : NULL;
        name = name ? name + 1 : (path ? path : "saved .nfc");
        snprintf(details, sizeof(details), "Emulating saved tag\n%s\nHold reader near antenna", name);
    }
    nfc_emu_details_label = popup_create_body_label(nfc_emu_popup, details, geom.width - 30, true, body_font, 36);

    int btn_w = 100, btn_h = 34;
    if (LV_HOR_RES <= 240) { btn_w = 86; btn_h = 30; }
    nfc_emu_cancel_btn = popup_add_styled_button(nfc_emu_popup, "Stop", btn_w, btn_h,
                                                LV_ALIGN_BOTTOM_MID, 0, -8, body_font,
                                                nfc_emu_cancel_cb, NULL);
    nfc_emu_popup_selected = 0;
    update_nfc_emu_popup_selection();

#ifdef CONFIG_NFC_ST25R3916
    if (test_ndef) {
        char *argv[] = {"nfc", "emulate", "ndef", "url", "https://ghostesp.net"};
        handle_nfc_cmd(5, argv);
        nfc_emu_active = true;
        status_display_show_status("NFC Emulating");
    } else if (path) {
        char *argv[] = {"nfc", "emulate", "file", (char *)path};
        handle_nfc_cmd(4, argv);
        nfc_emu_active = true;
        status_display_show_status("NFC Emulating");
    }
#else
    if (nfc_emu_title_label && lv_obj_is_valid(nfc_emu_title_label)) {
        lv_label_set_text(nfc_emu_title_label, "Unsupported");
    }
    if (nfc_emu_details_label && lv_obj_is_valid(nfc_emu_details_label)) {
        lv_label_set_text(nfc_emu_details_label, "NFC emulation requires ST25R3916 support");
    }
#endif
}

static void nfc_emulate_test_cb(lv_event_t *e) {
    (void)e;
    create_nfc_emu_popup(NULL, true);
}

static void nfc_emulate_file_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) return;
    create_nfc_emu_popup(path, false);
}

static void update_saved_popup_selection(void) {
    if (!saved_close_btn || !lv_obj_is_valid(saved_close_btn)) return;
    lv_obj_t *btns[3] = { saved_close_btn, saved_rename_btn, saved_delete_btn };
    popup_update_selection(btns, 3, saved_popup_selected);
    update_saved_buttons_layout();
}

static void keys_close_cb(lv_event_t *e) { (void)e; cleanup_keys_popup(NULL); }
static void keys_scroll_up_cb(lv_event_t *e);
static void keys_scroll_down_cb(lv_event_t *e);

static void cleanup_keys_popup(void *obj) {
    (void)obj;
    lvgl_obj_del_safe(&keys_popup);
    keys_close_btn = NULL;
    keys_title_label = NULL;
    keys_details_label = NULL;
    keys_popup_selected = 0;
}

static void update_keys_popup_selection(void) {
    // Use the popup_update_selection helper for the array of buttons
    lv_obj_t *btns[3] = { keys_up_btn, keys_close_btn, keys_down_btn };
    popup_update_selection(btns, 3, keys_popup_selected);
}

static void keys_scroll_up_cb(lv_event_t *e) {
    lv_obj_t *scroll = keys_scroll;
    if (e) {
        lv_obj_t *ud = (lv_obj_t *)lv_event_get_user_data(e);
        if (ud) scroll = ud;
    }
    if (!scroll || !lv_obj_is_valid(scroll)) return;
    lv_coord_t y = lv_obj_get_scroll_y(scroll);
    lv_obj_scroll_to_y(scroll, y - 40, LV_ANIM_OFF);
}

static void keys_scroll_down_cb(lv_event_t *e) {
    lv_obj_t *scroll = keys_scroll;
    if (e) {
        lv_obj_t *ud = (lv_obj_t *)lv_event_get_user_data(e);
        if (ud) scroll = ud;
    }
    if (!scroll || !lv_obj_is_valid(scroll)) return;
    lv_coord_t y = lv_obj_get_scroll_y(scroll);
    lv_obj_scroll_to_y(scroll, y + 40, LV_ANIM_OFF);
}

static void create_keys_popup(void) {
    if (!root) return;
    if (keys_popup && lv_obj_is_valid(keys_popup)) cleanup_keys_popup(NULL);
    int popup_w;
    if (LV_HOR_RES <= 240) popup_w = LV_HOR_RES - 20; else popup_w = LV_HOR_RES - 30;
    int popup_h;
    int y_offset = 0;

    if (LV_VER_RES <= 135) {
        popup_h = 130;
        y_offset = 0;
    } else if (LV_VER_RES <= 200) {
        popup_h = (LV_VER_RES < 200) ? (LV_VER_RES - 30) : 160;
        if (popup_h < 110) popup_h = 110;
        y_offset = 10;
    } else {
        popup_h = (LV_VER_RES <= 240) ? 140 : 170;
        y_offset = 10;
    }
    keys_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset, true);
    if (keys_popup) lv_obj_add_flag(keys_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    keys_title_label = popup_create_title_label(keys_popup, "User MFC Keys", title_font, 10);

    // Create scrollable container for keys and set fixed popup height
    keys_scroll = popup_create_scroll_area(keys_popup, LV_HOR_RES - 50, popup_h - 80, LV_ALIGN_TOP_MID, 0, 26);

    keys_details_label = popup_create_body_label(keys_scroll, "", LV_HOR_RES - 60, true, body_font, 0);
    if (keys_details_label) lv_obj_align(keys_details_label, LV_ALIGN_TOP_LEFT, 0, 0);

    // read keys file and show (build text on heap to avoid stack pressure)
    bool display_was_suspended = false;
    bool sd_ready = nfc_sd_begin(&display_was_suspended);
    size_t cap = 512; size_t pos = 0;
    char *buf = NULL;
    FILE *f = NULL;

    if (!sd_ready) {
        lv_label_set_text(keys_details_label, "No user keys file found");
        goto keys_cleanup;
    }

    buf = (char*)malloc(cap);
    if (!buf) {
        lv_label_set_text(keys_details_label, "(Out of memory)");
        goto keys_cleanup;
    }
    buf[0] = '\0';

    f = fopen("/mnt/ghostesp/nfc/mfc_user_dict.nfc", "r");
    if (!f) {
        lv_label_set_text(keys_details_label, "No user keys file found");
        goto keys_cleanup;
    }

    char line[256];
    int keys_on_line = 0;
    while (fgets(line, sizeof(line), f)) {
        // normalize: keep only hex chars, uppercase, and split into 12-length chunks
        char hexbuf[256]; size_t h = 0;
        for (char *p = line; *p && h < sizeof(hexbuf)-1; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'f') c -= 32;
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')) {
                hexbuf[h++] = c;
            }
        }
        hexbuf[h] = '\0';
        // output each 12-hex key; two per line: "KEY KEY\n"
        size_t i = 0;
        while (i + 12 <= h) {
            // ensure capacity, attempt write, grow if truncated
            for (;;) {
                int n;
                if (keys_on_line == 0) {
                    n = snprintf(buf + pos, cap - pos, "%.*s", 12, &hexbuf[i]);
                } else {
                    n = snprintf(buf + pos, cap - pos, " %.*s\n", 12, &hexbuf[i]);
                }
                if (n < 0) { break; }
                if ((size_t)n < (cap - pos)) { pos += (size_t)n; break; }
                size_t new_cap = cap * 2;
                char *nbuf = (char*)realloc(buf, new_cap);
                if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
                buf = nbuf; cap = new_cap;
            }
            keys_on_line = (keys_on_line == 0) ? 1 : 0;
            i += 12;
        }
        if (cap - pos < 64) {
            size_t new_cap = cap * 2;
            char *nbuf = (char*)realloc(buf, new_cap);
            if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
            buf = nbuf; cap = new_cap;
        }
    }

    // if one key left without its pair, terminate the line
    if (keys_on_line == 1) {
        for (;;) {
            int n = snprintf(buf + pos, cap - pos, "\n");
            if (n < 0) break;
            if ((size_t)n < (cap - pos)) { pos += (size_t)n; break; }
            size_t new_cap = cap * 2;
            char *nbuf = (char*)realloc(buf, new_cap);
            if (!nbuf) { lv_label_set_text(keys_details_label, "(Out of memory)"); goto keys_cleanup; }
            buf = nbuf; cap = new_cap;
        }
    }

    if (pos == 0) {
        lv_label_set_text(keys_details_label, "(Empty)");
    } else {
        lv_label_set_text(keys_details_label, buf);
    }

keys_cleanup:
    if (f) fclose(f);
    if (buf) free(buf);
    if (sd_ready) nfc_sd_end(display_was_suspended);

    // Bottom controls: Up | Close | Down
    int btn_w = 60, btn_h = 34; if (LV_VER_RES <= 240) { btn_w = 54; btn_h = 30; }
    keys_up_btn = popup_add_styled_button(keys_popup, LV_SYMBOL_UP, btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, keys_scroll_up_cb, keys_scroll);
    int close_w = 90; if (LV_VER_RES <= 240) close_w = 80;
    keys_close_btn = popup_add_styled_button(keys_popup, "Close", close_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, keys_close_cb, NULL);
    keys_down_btn = popup_add_styled_button(keys_popup, LV_SYMBOL_DOWN, btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, keys_scroll_down_cb, keys_scroll);

    update_keys_buttons_layout();
    keys_popup_selected = 1; // default focus on Close
    update_keys_popup_selection();
}

static void nfc_credits_close_cb(lv_event_t *e) { (void)e; cleanup_nfc_credits_popup(NULL); }

static void cleanup_nfc_credits_popup(void *obj) {
    (void)obj;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&nfc_credits_drag);
#endif
    lvgl_obj_del_safe(&nfc_credits_popup);
    nfc_credits_close_btn = NULL;
    nfc_credits_scroll = NULL;
}

static void create_nfc_credits_popup(void) {
    if (!root) return;
    if (nfc_credits_popup && lv_obj_is_valid(nfc_credits_popup)) cleanup_nfc_credits_popup(NULL);

    /* Credits benefit from a tall, scrollable area. Use most of the screen
     * height rather than the shared popup_calc_size caps (which top out at
     * 140-160px) so the attribution is readable without constant scrolling. */
    lv_coord_t screen_w = LV_HOR_RES;
    lv_coord_t screen_h = LV_VER_RES;
    int popup_w = (screen_w <= 240) ? (screen_w - 20) : (screen_w - 30);
    int popup_h = screen_h - 24;
    if (popup_h < 120) popup_h = 120;
    if (popup_h > screen_h - 10) popup_h = screen_h - 10;
    int y_offset = (screen_h - popup_h) / 2;
    if (y_offset < 0) y_offset = 0;

    nfc_credits_popup = popup_create_container_with_offset(lv_scr_act(), popup_w, popup_h, y_offset, true);
    if (nfc_credits_popup) lv_obj_add_flag(nfc_credits_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    popup_create_title_label(nfc_credits_popup, "NFC Credits", title_font, 10);

    int scroll_h = popup_h - 76;
    if (scroll_h < 60) scroll_h = 60;
    nfc_credits_scroll = popup_create_scroll_area(nfc_credits_popup, popup_w - 24, scroll_h, LV_ALIGN_TOP_MID, 0, 30);

    const char *credits =
        "GhostESP NFC parser support is based on Next-Flip Momentum-Firmware "
        "and Flipper NFC work.\n\n"
        "MIFARE Classic hardnested/nested nonce collection is ported from "
        "Momentum-Firmware work by noproto.\n\n"
        "Momentum NFC/NDEF blame includes WillyJL, xMasterX, noproto, "
        "Methodius, gornekich, hedger, Leptopt1los, hazardousvoltage, YaBa, "
        "ted-logan, tomholford, luu176, and mxcdoam.\n\n"
        "EMV payment-card reading (PPSE/AID select, GPO, and record parsing) is "
        "ported from Momentum-Firmware's EMV poller, with the payment-card parser "
        "by Leptopt1los.\n\n"
        "DESFire application/file reads are adapted from the Momentum-Firmware "
        "MIFARE DESFire poller. Supported-card parsers include Opal by micolous, "
        "myki by Emily Trau, ITSO, and Gallagher utilities by Nick Mooney.\n\n"
        "ST25R3916 NFC-V and target-mode behavior was cross-referenced with "
        "Momentum/Flipper NFC HAL work.\n\n"
        "PicoPass/iCLASS support is based on bettse/picopass, carried by "
        "Momentum via Momentum-Apps. Includes holiman/loclass and "
        "RfidResearchGroup/proxmark3 work.\n\n"
        "PicoPass acknowledgements are preserved from bettse/picopass, including "
        "Iceman and the Proxmark3 community.\n\n"
        "GhostESP NFC integration and ports in this tree are by jaylikesbunda and deki.";

    lv_obj_t *body = popup_create_body_label(nfc_credits_scroll, credits, popup_w - 42, true, body_font, 0);
    if (body) {
        lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_LEFT, 0);
    }

    int btn_w = (LV_HOR_RES <= 240) ? 80 : 90;
    int btn_h = (LV_HOR_RES <= 240) ? 30 : 34;
    nfc_credits_close_btn = popup_add_styled_button(nfc_credits_popup, "Close", btn_w, btn_h,
                                                    LV_ALIGN_BOTTOM_MID, 0, -8, body_font,
                                                    nfc_credits_close_cb, NULL);
    if (nfc_credits_close_btn) popup_set_button_selected(nfc_credits_close_btn, true);
}

// ---- chameleon ultra basic popup ----
void cleanup_cu_popup(void *obj) {
    (void)obj;
    lvgl_obj_del_safe(&cu_popup);
    cu_title_label = NULL;
    cu_details_label = NULL;
    cu_close_btn = NULL;
    cu_connect_btn = NULL;
    cu_disconnect_btn = NULL;
    cu_reader_btn = NULL;
    cu_scan_hf_btn = NULL;
    cu_save_hf_btn = NULL;
    cu_more_btn = NULL;
    cu_popup_selected = 0;
    cu_save_visible = false;
    cu_busy = false;
    cu_more_expanded = false;
    lvgl_timer_del_safe(&cu_state_timer);
}

static void cu_close_cb(lv_event_t *e) { (void)e; cleanup_cu_popup(NULL); }

static void cu_bool_done_async(void *ptr) {
    cu_busy = false;

    if (cu_title_label && lv_obj_is_valid(cu_title_label)) {
        lv_label_set_text(cu_title_label, "Chameleon Ultra");
    }
    if (cu_details_label && lv_obj_is_valid(cu_details_label)) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
    }
    update_cu_buttons_layout();
    update_cu_popup_selection();
    if (ptr) nfc_bool_pool_free(ptr);
}

static void cu_connect_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_connect(10, NULL);
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(cu_bool_done_async, res); }
    else { display_manager_lvgl_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_disconnect_task(void *arg) {
    (void)arg;
    chameleon_manager_disconnect();
    bool ok = !chameleon_manager_is_connected();
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(cu_bool_done_async, res); }
    else { display_manager_lvgl_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_reader_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_set_reader_mode();
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(cu_bool_done_async, res); }
    else { display_manager_lvgl_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_scan_hf_task(void *arg) {
    (void)arg;
    bool ok = chameleon_manager_scan_hf();
    if (ok) cu_save_visible = true;
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(cu_bool_done_async, res); }
    else { display_manager_lvgl_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void cu_save_hf_task(void *arg) {
    (void)arg;
    bool ok = false;
    glog("Saving last HF scan header...");
    ok = chameleon_manager_save_last_hf_scan(NULL);
    bool *res = (bool*)nfc_bool_pool_alloc();
    if (res) { *res = ok; display_manager_lvgl_async_call(cu_bool_done_async, res); }
    else { display_manager_lvgl_async_call(cu_bool_done_async, NULL); }
    vTaskDelete(NULL);
}

static void update_cu_buttons_layout(void) {
    if (!cu_popup) return;
    bool connected = chameleon_manager_is_connected();
    if (connected) {
        if (cu_connect_btn) lv_obj_add_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
        if (cu_disconnect_btn) lv_obj_clear_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (cu_disconnect_btn) lv_obj_add_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        if (cu_connect_btn) lv_obj_clear_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
    }

    // collect visible buttons in order: Close, Connect/Disconnect
    lv_obj_t *btns[3]; int count = 0;
    if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) btns[count++] = cu_close_btn;
    if (connected) {
        if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_disconnect_btn;
    } else {
        if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_connect_btn;
    }
    if (count == 0) return;
    popup_layout_buttons_responsive(cu_popup, btns, count, -8, NULL);
}

static void update_cu_popup_selection(void) {
    lv_obj_t *btns[3]; int count = 0;
    if (cu_close_btn && lv_obj_is_valid(cu_close_btn)) btns[count++] = cu_close_btn;
    if (!chameleon_manager_is_connected()) {
        if (cu_connect_btn && lv_obj_is_valid(cu_connect_btn) && !lv_obj_has_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_connect_btn;
    } else {
        if (cu_disconnect_btn && lv_obj_is_valid(cu_disconnect_btn) && !lv_obj_has_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN)) btns[count++] = cu_disconnect_btn;
    }
    for (int i = 0; i < count; ++i) popup_set_button_selected(btns[i], cu_popup_selected == i);
    update_cu_buttons_layout();
}

static void cu_connect_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Connecting...");
    xTaskCreate(cu_connect_task, "cu_connect", 4096, NULL, 5, NULL);
}

static void cu_disconnect_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Chameleon Ultra");
    xTaskCreate(cu_disconnect_task, "cu_disconnect", 4096, NULL, 5, NULL);
}

static void cu_reader_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Setting reader mode...");
    xTaskCreate(cu_reader_task, "cu_reader", 4096, NULL, 5, NULL);
}

static void cu_scan_hf_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Scanning HF...");
    xTaskCreate(cu_scan_hf_task, "cu_scan_hf", 4096, NULL, 5, NULL);
}

static void cu_save_hf_cb(lv_event_t *e) {
    (void)e; if (cu_busy) return; cu_busy = true;
    if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Saving...");
    BaseType_t rc = xTaskCreate(cu_save_hf_task, "cu_save_hf", 4096, NULL, 5, NULL);
    if (rc != pdPASS) {
        rc = xTaskCreate(cu_save_hf_task, "cu_save_hf", 3072, NULL, 5, NULL);
    }
    if (rc != pdPASS) {
        cu_busy = false;
        if (cu_title_label && lv_obj_is_valid(cu_title_label)) lv_label_set_text(cu_title_label, "Save failed");
        ESP_LOGE(TAG, "cu_save_hf_task create failed");
    }
}

static void cu_more_cb(lv_event_t *e) { (void)e; }

static void create_cu_popup(void) {
    if (!root) return;
    if (cu_popup && lv_obj_is_valid(cu_popup)) cleanup_cu_popup(NULL);
    popup_calc_size_t geom;
    popup_calc_size(&geom);
    cu_popup = popup_create_container_with_offset(lv_scr_act(), geom.width, geom.height, geom.y_offset, true);
    if (cu_popup) lv_obj_add_flag(cu_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    cu_title_label = popup_create_title_label(cu_popup, "Chameleon Ultra", title_font, 10);
    cu_details_label = popup_create_body_label(cu_popup, "", LV_HOR_RES - 50, true, body_font, 26);
    if (cu_details_label) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
        lv_obj_set_style_text_align(cu_details_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(cu_details_label, LV_ALIGN_TOP_MID, 0, 26);
    }

    int btn_w = (LV_HOR_RES <= 240) ? 80 : 90; int btn_h = (LV_HOR_RES <= 240) ? 30 : 34;
    cu_close_btn = popup_add_styled_button(cu_popup, "Close", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, cu_close_cb, NULL);
    cu_connect_btn = popup_add_styled_button(cu_popup, "Connect", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, cu_connect_cb, NULL);
    cu_disconnect_btn = popup_add_styled_button(cu_popup, "Disconnect", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, cu_disconnect_cb, NULL);
    // strip advanced controls; chameleon popup is connect-only now

    if (!chameleon_manager_is_connected()) {
        if (cu_disconnect_btn) lv_obj_add_flag(cu_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (cu_connect_btn) lv_obj_add_flag(cu_connect_btn, LV_OBJ_FLAG_HIDDEN);
    }

    cu_popup_selected = 0;
    update_cu_buttons_layout();
    update_cu_popup_selection();

    // start lightweight state refresh timer to keep popup live without reopening
    lvgl_timer_del_safe(&cu_state_timer);
    cu_state_timer = lv_timer_create(cu_state_timer_cb, 300, NULL);
}

static void cu_state_timer_cb(lv_timer_t *t) {
    (void)t;
    if (!cu_popup || !lv_obj_is_valid(cu_popup)) return;
    if (cu_details_label && lv_obj_is_valid(cu_details_label)) {
        if (chameleon_manager_is_connected()) {
            uint16_t batt_mv = 0;
            uint8_t batt_pct = 0;
            if (chameleon_manager_query_battery(&batt_mv, &batt_pct)) {
                char status_text[64];
                snprintf(status_text, sizeof(status_text), "Connected\nBattery: %dmV (%d%%)", batt_mv, batt_pct);
                lv_label_set_text(cu_details_label, status_text);
            } else {
                lv_label_set_text(cu_details_label, "Connected");
            }
        } else {
            lv_label_set_text(cu_details_label, "Not connected");
        }
    }
    update_cu_buttons_layout();
}


void cleanup_nfc_write_popup(void *obj) {
    (void)obj;
    lvgl_obj_del_safe(&nfc_write_popup);
    nfc_write_cancel_btn = NULL; nfc_write_go_btn = NULL;
    nfc_write_title_label = NULL; nfc_write_details_label = NULL;
    nfc_write_popup_selected = 0;
    // Do not force-cancel here; caller controls cancel flag
    #ifdef NFC_HAS_LOCAL_READER
    if (g_write_image_valid && !nfc_write_in_progress) { ntag_file_free(&g_write_image); g_write_image_valid = false; }
    #else
    g_write_image_valid = false;
    #endif
}

static char* build_compact_write_details(const ntag_file_image_t *img) {
    if (!img) return NULL;
    size_t cap = 768;
    char *out = (char*)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    // UID | Type
    pos += snprintf(out + pos, cap - pos, "UID:");
    for (uint8_t i = 0; i < img->uid_len && pos < cap - 4; ++i) {
        pos += snprintf(out + pos, cap - pos, " %02X", img->uid[i]);
    }
    pos += snprintf(out + pos, cap - pos, " | Type: %s\n", ntag_t2_model_str(img->model));
    // Pages | First user page
    pos += snprintf(out + pos, cap - pos, "Pages: %d | First user: %d\n", img->pages_total, img->first_user_page);
    // NDEF summary
    const uint8_t *mem = NULL; size_t mem_len = 0;
    if (img->full_pages && img->pages_total > img->first_user_page) {
        mem = &img->full_pages[(size_t)img->first_user_page * 4];
        mem_len = (size_t)(img->pages_total - img->first_user_page) * 4;
    }
    if (mem && mem_len > 0) {
        size_t off = 0, len = 0;
        if (ntag_t2_find_ndef(mem, mem_len, &off, &len) && off + len <= mem_len) {
            char *full = ndef_build_details_from_message(mem + off, len, img->uid, img->uid_len, ntag_t2_model_str(img->model));
            if (full) {
                // Extract the first decoded record line (e.g., URL ..., Text ..., SmartPoster ...)
                const char *p = strstr(full, "\nR");
                if (!p) {
                    // handle if the very first line starts with R
                    if (full[0] == 'R') p = full;
                } else {
                    p++; // move to 'R'
                }
                if (p && p[0] == 'R') {
                    // find the colon after R# and a space after colon
                    const char *colon = strchr(p, ':');
                    const char *start = NULL;
                    if (colon) {
                        start = colon + 1;
                        if (*start == ' ') start++;
                    } else {
                        start = p; // fallback, include R# prefix
                    }
                    const char *endl = strchr(start, '\n');
                    if (!endl) endl = start + strlen(start);
                    pos += snprintf(out + pos, cap - pos, "%.*s\n", (int)(endl - start), start);
                } else {
                    // Fallback to size-only summary
                    pos += snprintf(out + pos, cap - pos, "NDEF: %uB\n", (unsigned)len);
                }
                free(full);
            } else {
                pos += snprintf(out + pos, cap - pos, "NDEF: %uB\n", (unsigned)len);
            }
        } else {
            pos += snprintf(out + pos, cap - pos, "NDEF: none\n");
        }
    } else {
        pos += snprintf(out + pos, cap - pos, "NDEF: unknown\n");
    }
    return out;
}

// Very lightweight Flipper MIFARE Classic parser for Saved popup
#if defined(NFC_HAS_LOCAL_READER) || defined(CONFIG_NFC_CHAMELEON)
static char* build_mfc_details_from_file(const char *path, char **out_title) {
    if (out_title) *out_title = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[256];
    bool is_mfc = false;
    char *title = NULL;
    char *plugin_text = NULL;
    // extract UID, ATQA, SAK, and type string
    uint8_t uid[10] = {0}; int uid_len = 0;
    unsigned atqa_hi = 0, atqa_lo = 0; unsigned sak = 0;
    char type_str[48] = {0};

    // First pass: basic metadata
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Device type:", 12) == 0 && strstr(line, "Mifare Classic")) {
            is_mfc = true;
        } else if (strncmp(line, "Mifare Classic type:", 21) == 0) {
            // includes size: 4K/1K/Mini
            char tmp[40] = {0};
            if (sscanf(line + 21, " %39[^\n\r]", tmp) == 1) {
                size_t prefix_len = strlen("MIFARE Classic ");
                size_t max_tmp = (sizeof(type_str) > prefix_len + 1) ? (sizeof(type_str) - prefix_len - 1) : 0;
                if (max_tmp > 0) snprintf(type_str, sizeof(type_str), "MIFARE Classic %.*s", (int)max_tmp, tmp);
                else type_str[0] = '\0';
            }
        } else if (strncmp(line, "UID:", 4) == 0) {
            const char *p = line + 4; int ncon = 0; unsigned b = 0;
            while (*p && uid_len < (int)sizeof(uid)) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (sscanf(p, " %2x%n", &b, &ncon) == 1) { uid[uid_len++] = (uint8_t)b; p += ncon; }
                else break;
            }
        } else if (strncmp(line, "ATQA:", 6) == 0) {
            sscanf(line + 6, " %2x %2x", &atqa_hi, &atqa_lo);
        } else if (strncmp(line, "SAK:", 4) == 0) {
            sscanf(line + 4, " %2x", &sak);
        }
    }
    if (!is_mfc) { fclose(f); return NULL; }
    if (type_str[0] == '\0') snprintf(type_str, sizeof(type_str), "MIFARE Classic");

    // Determine type and total sectors/keys
    MFC_TYPE mt = mfc_type_from_sak((uint8_t)sak);
    int sectors_total = mfc_sector_count(mt);
    if (sectors_total == 0) sectors_total = 16;
    int keys_total = sectors_total * 2;

    // Prepare block count and allocate storage for block data
    int blocks_total = 0;
    for (int s = 0; s < sectors_total; ++s) blocks_total += mfc_blocks_in_sector(mt, s);

    // Rewind and parse Block lines to assess readable blocks and keys
    rewind(f);
    // allocate array of int[blocks_total][16], -1 for unknown, 0-255 for bytes
    int *blocks = (int*)malloc((size_t)blocks_total * 16 * sizeof(int));
    if (!blocks) { fclose(f); return NULL; }
    for (int i = 0; i < blocks_total * 16; ++i) blocks[i] = -1;
    while (fgets(line, sizeof(line), f)) {
        int blk = -1;
        if (sscanf(line, "Block %d:", &blk) == 1 && blk >= 0 && blk < blocks_total) {
            const char *p = strchr(line, ':');
            if (!p) {
                continue;
            }
            p++;
            int off = blk * 16;
            // parse up to 16 tokens
            for (int bi = 0; bi < 16; ++bi) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (p[0] == '?' && p[1] == '?') { blocks[off + bi] = -1; p += 2; }
                else {
                    unsigned v = 0; int consumed = 0;
                    if (sscanf(p, "%2x%n", &v, &consumed) == 1) { blocks[off + bi] = (int)v; p += consumed; }
                    else { blocks[off + bi] = -1; while (*p && *p != ' ') ++p; }
                }
            }
        }
    }
    fclose(f);

    // Compute readable sectors and keys found
    int sectors_readable = 0;
    int keys_found = 0;
    for (int s = 0; s < sectors_total; ++s) {
        int first = mfc_first_block_of_sector(mt, s);
        int blocks_in_sec = mfc_blocks_in_sector(mt, s);
        bool sector_has_data = false;
        // scan blocks in sector
        for (int b = 0; b < blocks_in_sec; ++b) {
            int blk = first + b;
            if (blk < 0 || blk >= blocks_total) continue;
            int off = blk * 16;
            for (int bi = 0; bi < 16; ++bi) {
                if (blocks[off + bi] >= 0) { sector_has_data = true; break; }
            }
            if (sector_has_data) break;
        }
        if (sector_has_data) sectors_readable++;
        // check trailer for keys (last block)
        int trailer = first + blocks_in_sec - 1;
        if (trailer >= 0 && trailer < blocks_total) {
            int offt = trailer * 16;
            // Key A in bytes 0..5
            bool key_a_known = true; for (int k = 0; k < 6; ++k) if (blocks[offt + k] < 0) { key_a_known = false; break; }
            if (key_a_known) keys_found++;
            // Key B in bytes 10..15
            bool key_b_known = true; for (int k = 10; k < 16; ++k) if (blocks[offt + k] < 0) { key_b_known = false; break; }
            if (key_b_known) keys_found++;
        }
    }
    // build title
    title = (char*)malloc(48);
    if (title) snprintf(title, 48, "%s", type_str);

    // build details compact
    size_t cap = 512; char *out = (char*)malloc(cap);
    if (!out) {
        if (title) free(title);
        if (out_title) *out_title = NULL;
        free(blocks);
        return NULL;
    }
    int pos = 0;
    pos += snprintf(out + pos, cap - pos, "UID:");
    for (int i = 0; i < uid_len && pos < (int)cap - 4; ++i) pos += snprintf(out + pos, cap - pos, " %02X", uid[i]);
    pos += snprintf(out + pos, cap - pos, "\nATQA: %02X %02X | SAK: %02X\n", (unsigned)atqa_hi, (unsigned)atqa_lo, (unsigned)sak);
    // Match live scan summary: Keys line first, then Sectors, so get_details_split_point() works the same
    pos += snprintf(out + pos, cap - pos, "Keys %d/%d | Sectors %d/%d\n", keys_found, keys_total, sectors_readable, sectors_total);

    if (out_title) *out_title = title; else if (title) free(title);

    // Try to find NDEF in saved blocks and append a concise single-line summary
    for (int s = 0; s < sectors_total; ++s) {
        if (s == 16 && sectors_total > 16) continue; // skip MAD2 on 4K
        int first = mfc_first_block_of_sector(mt, s);
        int blocks_in_sec = mfc_blocks_in_sector(mt, s);
        int data_blocks = blocks_in_sec - 1;
        if (data_blocks <= 0) continue;
        size_t sec_bytes = (size_t)data_blocks * 16;
        uint8_t *sec_buf = (uint8_t*)malloc(sec_bytes);
        if (!sec_buf) break;
        size_t woff = 0;
        for (int b = 0; b < data_blocks; ++b) {
            int blk = first + b;
            int off = blk * 16;
            for (int bi = 0; bi < 16; ++bi) {
                int v = -1;
                if (blk >= 0 && blk < blocks_total) v = blocks[off + bi];
                sec_buf[woff + bi] = (v >= 0) ? (uint8_t)v : 0x00;
            }
            woff += 16;
        }
        #if defined(NFC_HAS_LOCAL_READER) || defined(CONFIG_NFC_CHAMELEON)
        size_t off = 0, mlen = 0;
        if (ntag_t2_find_ndef(sec_buf, sec_bytes, &off, &mlen) && off < sec_bytes && mlen > 0) {
            // assemble contiguous view across subsequent sectors to cover message
            size_t need = off + mlen;
            size_t have = sec_bytes;
            int ss = s + 1;
            while (have < need && ss < sectors_total) {
                if (ss == 16 && sectors_total > 16) { ss++; continue; }
                int bl2 = mfc_blocks_in_sector(mt, ss);
                have += (size_t)(bl2 - 1) * 16;
                ss++;
            }
            size_t total_cap = have;
            uint8_t *cat = (uint8_t*)malloc(total_cap);
            if (cat) {
                // copy first sector
                memcpy(cat, sec_buf, sec_bytes);
                size_t cat_off = sec_bytes;
                for (int s2 = s + 1; cat_off < total_cap && s2 < sectors_total; ++s2) {
                    if (s2 == 16 && sectors_total > 16) continue;
                    int f2 = mfc_first_block_of_sector(mt, s2);
                    int bl2 = mfc_blocks_in_sector(mt, s2);
                    for (int b2 = 0; b2 < bl2 - 1 && cat_off < total_cap; ++b2) {
                        int absb2 = f2 + b2;
                        int offb = absb2 * 16;
                        for (int bi = 0; bi < 16 && cat_off < total_cap; ++bi) {
                            int v = -1;
                            if (absb2 >= 0 && absb2 < blocks_total) v = blocks[offb + bi];
                            cat[cat_off++] = (v >= 0) ? (uint8_t)v : 0x00;
                        }
                    }
                }

                char *ndef_text = ndef_build_details_from_message(cat + off, mlen, uid, uid_len, type_str);
                if (ndef_text) {
                    // extract first record line
                    const char *p = strstr(ndef_text, "\nR");
                    if (!p) { if (ndef_text[0] == 'R') p = ndef_text; }
                    else p++;
                    if (p && p[0] == 'R') {
                        const char *colon = strchr(p, ':');
                        const char *start = NULL;
                        if (colon) { start = colon + 1; if (*start == ' ') start++; }
                        else start = p;
                        const char *endl = strchr(start, '\n');
                        if (!endl) endl = start + strlen(start);
                        // append directly after existing lines (no extra blank)
                        int napp = snprintf(out + pos, cap - pos, "NDEF: %.*s\n", (int)(endl - start), start);
                        if (napp > 0) { pos += napp; }
                    }
                    free(ndef_text);
                    free(cat);
                    free(sec_buf);
                    break; // stop after first found
                }
                free(cat);
            }
        }
        #endif
        free(sec_buf);
    }

    // Attempt Flipper parser for richer summaries
    if (blocks_total > 0) {
        MfClassicData *flipper_data = (MfClassicData*)calloc(1, sizeof(MfClassicData));
        if (flipper_data) {
            flipper_data->type = (mt == MFC_4K) ? MfClassicType4k : (mt == MFC_MINI) ? MfClassicTypeMini : MfClassicType1k;
            size_t copy_uid = (uid_len < sizeof(flipper_data->uid)) ? (size_t)uid_len : sizeof(flipper_data->uid);
            flipper_data->uid_len = (uint8_t)copy_uid;
            if (copy_uid > 0) memcpy(flipper_data->uid, uid, copy_uid);
            int max_blocks = blocks_total;
            if (max_blocks > 256) max_blocks = 256;
            for (int blk = 0; blk < max_blocks; ++blk) {
                bool block_complete = true;
                int off = blk * 16;
                for (int bi = 0; bi < 16; ++bi) {
                    int v = blocks[off + bi];
                    if (v < 0) {
                        block_complete = false;
                        v = 0;
                    }
                    flipper_data->block[blk].data[bi] = (uint8_t)v;
                }
                if (block_complete) {
                    flipper_data->block_read_mask[blk / 8] |= (1U << (blk % 8));
                }
            }
            plugin_text = flipper_nfc_try_parse_mfclassic_from_cache(flipper_data);
            free(flipper_data);
        }
    }

    free(blocks);

    if (plugin_text) {
        size_t base_len = strlen(out);
        size_t extra_len = strlen(plugin_text);
        char *combined = (char*)malloc(base_len + extra_len + 2);
        if (combined) {
            memcpy(combined, out, base_len);
            combined[base_len] = '\n';
            memcpy(combined + base_len + 1, plugin_text, extra_len);
            combined[base_len + 1 + extra_len] = '\0';
            free(out);
            out = combined;
        }
        free(plugin_text);
    }

    return out;
}
#endif

static char* build_desfire_details_from_file(const char *path, char **out_title) {
    if (out_title) *out_title = NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char line[256];
    bool is_desfire = false;
    uint8_t uid[10] = {0};
    int uid_len = 0;
    unsigned atqa_hi = 0, atqa_lo = 0;
    unsigned sak = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Device type:", 12) == 0 && strstr(line, "Mifare DESFire")) {
            is_desfire = true;
        } else if (strncmp(line, "UID:", 4) == 0) {
            const char *p = line + 4;
            int consumed = 0;
            unsigned b = 0;
            while (*p && uid_len < (int)sizeof(uid)) {
                while (*p == ' ') ++p;
                if (!*p || *p == '\n' || *p == '\r') break;
                if (sscanf(p, " %2x%n", &b, &consumed) == 1) {
                    uid[uid_len++] = (uint8_t)b;
                    p += consumed;
                } else {
                    break;
                }
            }
        } else if (strncmp(line, "ATQA:", 5) == 0) {
            sscanf(line + 5, " %2x %2x", &atqa_hi, &atqa_lo);
        } else if (strncmp(line, "SAK:", 4) == 0) {
            sscanf(line + 4, " %2x", &sak);
        }
    }
    fclose(f);

    if (!is_desfire) return NULL;

    uint16_t atqa = (uint16_t)(((atqa_hi & 0xFFu) << 8) | (atqa_lo & 0xFFu));

    /* Reconstruct the DESFire tree (and PICC version) from the file so the saved
     * view matches a live scan: the same supported-card parsers (Opal/myki/ITSO)
     * annotate the summary. Falls back to a plain summary if reconstruction or
     * parsing yields nothing. */
    desfire_version_t ver;
    bool have_ver = false;
    MfDesfireData *tree = desfire_load_flipper_file(path, &ver, &have_ver);

    char *text = desfire_build_details_summary(have_ver ? &ver : NULL,
                                               (uid_len > 0) ? uid : NULL,
                                               (uint8_t)uid_len,
                                               atqa,
                                               (uint8_t)sak);
    if (!text) {
        if (tree) desfire_tree_free(tree);
        return NULL;
    }

    if (tree) {
        char *plugin_text = flipper_nfc_try_parse_mfdesfire(tree);
        if (plugin_text) {
            size_t h_len = strlen(text);
            size_t p_len = strlen(plugin_text);
            char *combined = (char *)malloc(h_len + 1 + p_len + 1);
            if (combined) {
                memcpy(combined, text, h_len);
                combined[h_len] = '\n';
                memcpy(combined + h_len + 1, plugin_text, p_len + 1);
                free(text);
                text = combined;
            }
            free(plugin_text);
        }
        desfire_tree_free(tree);
    }

    if (out_title) {
        const char *label = desfire_model_str(have_ver ? ver.model : DESFIRE_MODEL_UNKNOWN);
        *out_title = strdup(label);
        if (!*out_title) {
            free(text);
            return NULL;
        }
    }

    return text;
}

/* Reconstruct a saved EMV card and render the same details a live scan shows. */
static char* build_emv_details_from_file(const char *path, char **out_title) {
    if (out_title) *out_title = NULL;

    EmvData emv;
    uint8_t uid[10];
    uint8_t uid_len = 0;
    uint16_t atqa = 0;
    uint8_t sak = 0;
    if (!emv_load_flipper_file(path, &emv, uid, &uid_len, &atqa, &sak)) return NULL;

    char *plugin_text = flipper_nfc_try_parse_emv(&emv);
    if (!plugin_text) return NULL;

    size_t cap = 512;
    char *text = (char *)malloc(cap);
    if (!text) { free(plugin_text); return NULL; }
    int pos = snprintf(text, cap, "Type: EMV Payment Card\nUID:");
    for (uint8_t i = 0; i < uid_len && pos < (int)cap - 4; ++i)
        pos += snprintf(text + pos, cap - pos, " %02X", uid[i]);
    pos += snprintf(text + pos, cap - pos, "\nATQA: %02X %02X  SAK: %02X\n",
                    (atqa >> 8) & 0xFF, atqa & 0xFF, sak);

    size_t h_len = strlen(text);
    size_t p_len = strlen(plugin_text);
    char *combined = (char *)malloc(h_len + 1 + p_len + 1);
    if (combined) {
        memcpy(combined, text, h_len);
        combined[h_len] = '\n';
        memcpy(combined + h_len + 1, plugin_text, p_len + 1);
        free(text);
        text = combined;
    }
    free(plugin_text);

    if (out_title) *out_title = strdup("EMV Payment Card");
    return text;
}

static void create_nfc_write_popup(const char *path) {
    if (!root) return;
    // Load image
    #ifdef NFC_HAS_LOCAL_READER
    memset(&g_write_image, 0, sizeof(g_write_image));
    // jit sd mount only for somethingsomething template via nfc_sd_begin()
    bool susp_rd = false; bool did_rd = nfc_sd_begin(&susp_rd);
    bool is_desfire = false;
    FILE *fh = fopen(path, "r");
    if (fh) {
        char hdr[192];
        while (fgets(hdr, sizeof(hdr), fh)) {
            if (strncmp(hdr, "Device type:", 12) == 0 && strstr(hdr, "Mifare DESFire")) {
                is_desfire = true;
                break;
            }
        }
        fclose(fh);
    }
    if (!is_desfire) {
        g_write_image_valid = ntag_file_load(path, &g_write_image);
    } else {
        g_write_image_valid = false;
    }
    if (did_rd) nfc_sd_end(susp_rd);
    strncpy(g_write_image_path, path, sizeof(g_write_image_path) - 1);
    g_write_image_path[sizeof(g_write_image_path) - 1] = '\0';
    ESP_LOGI(TAG, "create_nfc_write_popup: path=%s valid=%d", g_write_image_path, (int)g_write_image_valid);
    #else
    g_write_image_valid = false;
    strncpy(g_write_image_path, path, sizeof(g_write_image_path) - 1);
    g_write_image_path[sizeof(g_write_image_path) - 1] = '\0';
    #endif

    if (nfc_write_popup && lv_obj_is_valid(nfc_write_popup)) cleanup_nfc_write_popup(NULL);
    popup_calc_size_t geom;
    popup_calc_size_ex(&geom, 120);
    nfc_write_popup = popup_create_container_with_offset(lv_scr_act(), geom.width, geom.height, geom.y_offset, true);
    if (nfc_write_popup) lv_obj_add_flag(nfc_write_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    const char *nfc_write_title_text = g_write_image_valid ? "Write Tag" :
    #ifdef NFC_HAS_LOCAL_READER
        "Invalid file"
    #else
        "NFC disabled"
    #endif
    ;
    nfc_write_title_label = popup_create_title_label(nfc_write_popup, nfc_write_title_text, title_font, 10);

    nfc_write_details_label = popup_create_body_label(nfc_write_popup, "", LV_HOR_RES - 50, true, body_font, 26);
    #ifdef NFC_HAS_LOCAL_READER
    if (g_write_image_valid) {
        char *det = build_compact_write_details(&g_write_image);
        if (det) {
            lv_label_set_text(nfc_write_details_label, det);
            ESP_LOGI(TAG, "write_popup details:\n%s", det);
            free(det);
        } else {
            lv_label_set_text(nfc_write_details_label, "File parsed");
        }
    } else {
        lv_label_set_text(nfc_write_details_label, "Failed to parse .nfc file");
    }
    #else
    lv_label_set_text(nfc_write_details_label, "Writing tags requires NFC hardware");
    #endif

    int btn_w = 90, btn_h = 34;
    if (LV_HOR_RES <= 240) { btn_w = 80; btn_h = 30; }

    nfc_write_cancel_btn = popup_add_styled_button(nfc_write_popup, "Cancel", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, nfc_write_cancel_cb, NULL);

    nfc_write_go_btn = popup_add_styled_button(nfc_write_popup, "Write", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, nfc_write_go_cb, NULL);
    if (!g_write_image_valid && nfc_write_go_btn) lv_obj_add_state(nfc_write_go_btn, LV_STATE_DISABLED);

    nfc_write_popup_selected = 0;
    update_nfc_write_popup_selection();
}

static void saved_scroll_cb(lv_event_t *e) {
    (void)e;
    if (!saved_scroll || !lv_obj_is_valid(saved_scroll)) return;
    lv_obj_t *scroller = saved_scroll;
    lv_coord_t h = lv_obj_get_height(scroller);
    lv_coord_t y_before = lv_obj_get_scroll_y(scroller);
    lv_coord_t step = (h > 40) ? (h - 40) : (h / 2);
    if (step < 10) step = 10;
    lv_obj_scroll_by_bounded(scroller, 0, -step, LV_ANIM_OFF);
    lv_coord_t y_after = lv_obj_get_scroll_y(scroller);
    if (y_after == y_before) lv_obj_scroll_to_y(scroller, 0, LV_ANIM_ON);
}

static void saved_close_cb(lv_event_t *e) { (void)e; cleanup_saved_details_popup(NULL); }

static void saved_more_cb(lv_event_t *e) {
    (void)e;
    if (!saved_has_extra_details) {
        saved_close_cb(NULL);
        return;
    }
    if (!saved_details_parsed_view) {
        saved_show_parsed_view(true);
    } else {
        saved_close_cb(NULL);
    }
}
static void saved_rename_cb(lv_event_t *e) {
    (void)e;
    if (saved_details_parsed_view) {
        saved_show_parsed_view(false);
        return;
    }
    if (g_saved_current_path[0] == '\0') return;
    // derive current base name without extension
    const char *slash = strrchr(g_saved_current_path, '/');
    const char *fname = slash ? slash + 1 : g_saved_current_path;
    char base[64] = {0};
    strncpy(base, fname, sizeof(base) - 1);
    char *dot = strrchr(base, '.'); if (dot) *dot = '\0';
    keyboard_view_set_submit_callback(saved_rename_keyboard_callback);
    // placeholder should be the current base name (without extension) so typing "example" results in example.nfc
    keyboard_view_set_placeholder(base[0] ? base : fname);
    keyboard_view_set_return_view(&nfc_view);
    display_manager_switch_view(&keyboard_view);
}
static void saved_delete_cb(lv_event_t *e) {
    (void)e;
    if (saved_details_parsed_view) {
        saved_scroll_cb(NULL);
        return;
    }
    if (g_saved_current_path[0] == '\0') return;
    popup_confirm_show(&saved_delete_confirm_popup, lv_layer_top(), "Delete NFC File?",
                       "This saved NFC file will be permanently deleted.",
                       "Delete", "Cancel", saved_delete_confirm_cb, NULL);
}
static void saved_rename_keyboard_callback(const char *name) {
    if (!name || !*name) { display_manager_switch_view(&nfc_view); return; }
    if (g_saved_current_path[0] == '\0') { display_manager_switch_view(&nfc_view); return; }
    // Build new path safely
    char dir[192]; strncpy(dir, g_saved_current_path, sizeof(dir)-1); dir[sizeof(dir)-1] = '\0';
    char *last = strrchr(dir, '/'); if (last) *last = '\0'; else dir[0] = '\0';
    char safe[200];
    size_t max_name = 180; // conservative limit
    size_t copy_len = (max_name < sizeof(safe) - 1) ? max_name : (sizeof(safe) - 1);
    strncpy(safe, name, copy_len);
    safe[copy_len] = '\0';
    for (size_t i = strlen(safe); i > 0 && (safe[i-1] == ' ' || safe[i-1] == '\r' || safe[i-1] == '\n' || safe[i-1] == '\t'); --i) safe[i-1] = '\0';
    for (char *p = safe; *p; ++p) { if (*p == '/' || *p == '\\') *p = '_'; }
    bool has_ext = false; size_t sl = strlen(safe);
    if (sl >= 4) { const char *ext = &safe[sl - 4]; if ((ext[0] == '.') && ((ext[1] | 0x20) == 'n') && ((ext[2] | 0x20) == 'f') && ((ext[3] | 0x20) == 'c')) has_ext = true; }

    saved_rename_job_t *job = (saved_rename_job_t*)malloc(sizeof(saved_rename_job_t));
    if (!job) { display_manager_switch_view(&nfc_view); return; }
    strncpy(job->old_path, g_saved_current_path, sizeof(job->old_path)-1); job->old_path[sizeof(job->old_path)-1] = '\0';
    {
        size_t N = sizeof(job->new_path);
        size_t dir_len = strlen(dir);
        size_t ext_len = has_ext ? 0 : 4; // ".nfc"
        if (dir_len + 1 + ext_len + 1 >= N) {
            // not enough room for any name; produce a safe fallback
            snprintf(job->new_path, N, "%s/renamed.nfc", dir);
        } else {
            int max_safe = (int)(N - dir_len - ext_len - 2); // room for '/', ext, and NUL
            if (max_safe < 0) max_safe = 0;
            if (has_ext) {
                snprintf(job->new_path, N, "%s/%.*s", dir, max_safe, safe);
            } else {
                snprintf(job->new_path, N, "%s/%.*s.nfc", dir, max_safe, safe);
            }
        }
    }
    job->success = 0;

    // Do the rename in a background task to avoid LVGL tick stack overflow
    xTaskCreate(saved_rename_task, "saved_rename", 4096, job, 5, NULL);
}

static void saved_rename_task(void *arg) {
    saved_rename_job_t *job = (saved_rename_job_t*)arg;
    if (!job) { vTaskDelete(NULL); return; }
    bool susp = false; bool did = nfc_sd_begin(&susp);
    int res = rename(job->old_path, job->new_path);
    job->success = (res == 0);
    if (did) nfc_sd_end(susp);
    display_manager_lvgl_async_call(saved_rename_ui_done_cb, job);
    vTaskDelete(NULL);
}

static void saved_rename_ui_done_cb(void *param) {
    saved_rename_job_t *job = (saved_rename_job_t*)param;
    if (!job) return;
    if (job->success) {
        ESP_LOGI(TAG, "renamed: %s -> %s", job->old_path, job->new_path);
        strncpy(g_saved_current_path, job->new_path, sizeof(g_saved_current_path)-1);
        g_saved_current_path[sizeof(g_saved_current_path)-1] = '\0';
    } else {
        ESP_LOGE(TAG, "rename failed: %s -> %s (errno=%d)", job->old_path, job->new_path, errno);
    }
    // Close popup and refresh list on UI thread
    cleanup_saved_details_popup(NULL);
    display_manager_switch_view(&nfc_view);
    saved_enter_list();
    free(job);
}

void cleanup_saved_details_popup(void *obj) {
    (void)obj;
    lvgl_obj_del_safe(&saved_popup);
    saved_close_btn = NULL;
    saved_rename_btn = NULL;
    saved_delete_btn = NULL;
    saved_title_label = NULL;
    saved_details_label = NULL;
    saved_scroll = NULL;
    saved_popup_selected = 0;
    saved_details_parsed_view = false;
    saved_has_extra_details = false;
    if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
}

static void create_saved_details_popup(const char *path) {
    if (!root) return;
    if (saved_popup && lv_obj_is_valid(saved_popup)) cleanup_saved_details_popup(NULL);
    popup_calc_size_t geom;
    popup_calc_size_ex(&geom, 120);
    saved_popup = popup_create_container_with_offset(lv_scr_act(), geom.width, geom.height, geom.y_offset, true);
    if (saved_popup) lv_obj_add_flag(saved_popup, LV_OBJ_FLAG_CLICKABLE);

    const lv_font_t *title_font = (LV_VER_RES <= 240) ? accessibility_get_font_body() : accessibility_get_font_title();
    const lv_font_t *body_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();

    saved_title_label = popup_create_title_label(saved_popup, "Saved Tag", title_font, 10);

    saved_scroll = popup_create_scroll_area(saved_popup, LV_HOR_RES - 50, geom.height - 80, LV_ALIGN_TOP_MID, 0, 26);
    saved_details_label = popup_create_body_label(saved_scroll, "", LV_HOR_RES - 60, true, body_font, 0);

    // store current path for rename/delete
    strncpy(g_saved_current_path, path, sizeof(g_saved_current_path) - 1);
    g_saved_current_path[sizeof(g_saved_current_path) - 1] = '\0';

    // reset stored details text
    if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
    saved_details_parsed_view = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&saved_details_drag);
#endif

    // parse file and show details (supports MIFARE Classic, DESFire, NTAG, and PicoPass)
    bool susp_load = false; bool did_load = nfc_sd_begin(&susp_load);
    char *title = NULL;
    char *mfc_det = NULL;
    char *df_det = NULL;

    /* Check if this is a .picopass file */
    size_t path_len = strlen(path);
    bool is_picopass = (path_len >= 9 && strcasecmp(path + path_len - 9, ".picopass") == 0);
    if (is_picopass) {
#ifdef CONFIG_NFC_ST25R3916
        /* Preferred: parse the blocks and render the same summary as a live
         * read. Falls through to the raw line dump below if parsing fails. */
        PicopassDeviceData *pp = (PicopassDeviceData *)malloc(sizeof(PicopassDeviceData));
        if (pp) {
            if (picopass_load_and_parse_file(path, pp)) {
                char *details = (char *)malloc(512);
                if (details) {
                    picopass_format_summary(pp, details, 512);
                    lv_label_set_text(saved_title_label, "PicoPass / iCLASS");
                    if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
                    saved_details_text = details;
                }
            }
            free(pp);
        }
        if (saved_details_text) {
            if (did_load) nfc_sd_end(susp_load);
            lv_label_set_text(saved_details_label, saved_details_text);
            return;
        }
#endif
        FILE *pf = fopen(path, "r");
        if (pf) {
            char line[256];
            char *details = (char *)malloc(1024);
            if (details) {
                details[0] = '\0';
                char *w = details;
                size_t cap = 1024;
                lv_label_set_text(saved_title_label, "PicoPass / iCLASS");
                while (fgets(line, sizeof(line), pf) && cap > 10) {
                    /* Skip header lines */
                    if (strncmp(line, "Filetype:", 9) == 0) continue;
                    if (strncmp(line, "Version:", 8) == 0) continue;
                    if (line[0] == '#') continue;
                    /* Trim trailing newline */
                    size_t llen = strlen(line);
                    while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = '\0';
                    if (llen == 0) continue;
                    int n = snprintf(w, cap, "%s\n", line);
                    if (n > 0) { w += n; cap -= n; }
                }
                fclose(pf);
                if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
                saved_details_text = details;
            } else {
                fclose(pf);
            }
        }
        if (saved_details_text) {
            if (did_load) nfc_sd_end(susp_load);
            lv_label_set_text(saved_details_label, saved_details_text);
            return;
        }
    }

#if defined(NFC_HAS_LOCAL_READER) || defined(CONFIG_NFC_CHAMELEON)
    mfc_det = build_mfc_details_from_file(path, &title);
#endif
    if (mfc_det) {
        if (title) { lv_label_set_text(saved_title_label, title); free(title); title = NULL; }
        if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
        saved_details_text = strdup(mfc_det);
        if (!saved_details_text) {
            lv_label_set_text(saved_details_label, mfc_det);
        }
        free(mfc_det);
    } else {
        df_det = build_desfire_details_from_file(path, &title);
        if (df_det) {
            if (title) { lv_label_set_text(saved_title_label, title); free(title); title = NULL; }
            if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
            saved_details_text = strdup(df_det);
            if (!saved_details_text) {
                lv_label_set_text(saved_details_label, df_det);
            }
            free(df_det);
        } else {
            char *emv_det = build_emv_details_from_file(path, &title);
            if (emv_det) {
                if (title) { lv_label_set_text(saved_title_label, title); free(title); title = NULL; }
                if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
                saved_details_text = strdup(emv_det);
                if (!saved_details_text) {
                    lv_label_set_text(saved_details_label, emv_det);
                }
                free(emv_det);
            } else {
                // Always allow NTAG file parsing, even without PN532
                ntag_file_image_t img; memset(&img, 0, sizeof(img));
                bool ok = ntag_file_load(path, &img);
                if (ok) {
                    const char *label = ntag_t2_model_str(img.model);
                    lv_label_set_text(saved_title_label, label);
                    char *det = build_compact_write_details(&img);
                    if (det) {
                        if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
                        saved_details_text = det;
                    } else {
                        if (saved_details_text) { free(saved_details_text); }
                        saved_details_text = strdup("File parsed");
                    }
                    ntag_file_free(&img);
                } else {
                    if (saved_details_text) { free(saved_details_text); saved_details_text = NULL; }
                    saved_details_text = strdup("Failed to parse .nfc file");
                }
            }
        }
    }
    if (did_load) nfc_sd_end(susp_load);

    saved_has_extra_details = has_extra_details(saved_details_text);

    if (saved_details_text) {
        // start in summary view using stored text
        saved_show_parsed_view(false);
    }

    int btn_w = 90, btn_h = 34; if (LV_VER_RES <= 240) { btn_w = 80; btn_h = 30; }
    // Buttons: More/Close (left), Rename/Less (mid), Delete/Scroll (right)
    saved_close_btn = popup_add_styled_button(saved_popup, "More", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, 10, -8, body_font, saved_more_cb, NULL);
    saved_rename_btn = popup_add_styled_button(saved_popup, "Rename", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, -8, body_font, saved_rename_cb, NULL);
    saved_delete_btn = popup_add_styled_button(saved_popup, "Delete", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, -10, -8, body_font, saved_delete_cb, NULL);

    saved_popup_selected = 0;
    saved_update_button_labels();
    update_saved_popup_selection();
}

static void nfc_write_cancel_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "nfc_write_cancel_cb: in_progress=%d", (int)nfc_write_in_progress);
    nfc_write_cancel = true;
    if (!nfc_write_in_progress) {
        cleanup_nfc_write_popup(NULL);
    } else {
        if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, "Cancelling...");
    }
}

#ifdef NFC_HAS_LOCAL_READER
static bool ensure_pn532_ready(void) {
    return nfc_init_local_reader(TAG);
}

static bool nfc_write_progress_cb(int current, int total, void *user) {
    (void)user;
    nfc_wr_prog_t *p = (nfc_wr_prog_t*)malloc(sizeof(nfc_wr_prog_t));
    if (p) { p->current = current; p->total = total; display_manager_lvgl_async_call(nfc_write_progress_async, p); }
    return !nfc_write_cancel;
}

static void nfc_write_progress_async(void *ptr) {
    if (!ptr) return;
    nfc_wr_prog_t *p = (nfc_wr_prog_t*)ptr;
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) {
        int percent = (p->total > 0) ? (p->current * 100) / p->total : 0;
        if (percent < 0) {
            percent = 0;
        }
        if (percent > 100) {
            percent = 100;
        }
        char t[48];
        snprintf(t, sizeof(t), "Writing... %d%%", percent);
        lv_label_set_text(nfc_write_title_label, t);
    }
    free(p);
}

static void nfc_write_done_async(void *ptr) {
    bool ok = (ptr != NULL) ? *((bool*)ptr) : false; if (ptr) free(ptr);
    nfc_write_in_progress = false;
    if (g_write_image_valid) { ntag_file_free(&g_write_image); g_write_image_valid = false; }
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, ok ? "Write complete" : "Write failed");
    ESP_LOGI(TAG, "nfc_write_done: %s", ok ? "success" : "fail");
    
    // Add status display messages for NFC write result
    if (ok) {
        status_display_show_status("NFC Written");
    } else {
        status_display_show_status("NFC Write Fail");
    }
}

static void nfc_write_task(void *arg) {
    (void)arg;
    bool ok = false;
    display_manager_set_low_i2c_mode(true);
    if (!ensure_pn532_ready()) {
        ok = false;
        goto done;
    }
    // Wait for tag presence
    ESP_LOGI(TAG, "nfc_write_task: waiting for tag...");
    for (;;) {
        if (nfc_write_cancel) { ok = false; goto done; }
        uint8_t uid[8] = {0}; uint8_t uid_len = 0; uint16_t atqa = 0; uint8_t sak = 0;
        if (pn532_read_passive_target_id_ex(g_pn532, 0x00, uid, &uid_len, &atqa, &sak, 200) == ESP_OK && uid_len > 0) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Write
    ESP_LOGI(TAG, "nfc_write_task: starting write file=%s", g_write_image_path);
    ok = ntag_write_to_tag(g_pn532, &g_write_image, nfc_write_progress_cb, NULL);

done:;
    display_manager_set_low_i2c_mode(false);
    if (g_pn532) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    bool *res = (bool*)malloc(sizeof(bool));
    if (res) { *res = ok; display_manager_lvgl_async_call(nfc_write_done_async, res); }
    else { display_manager_lvgl_async_call(nfc_write_done_async, NULL); }
    vTaskDelete(NULL);
}
#endif

static void nfc_write_go_cb(lv_event_t *e) {
    (void)e;
    if (!g_write_image_valid || nfc_write_in_progress) return;
    nfc_write_cancel = false;
    nfc_write_in_progress = true;
    ESP_LOGI(TAG, "nfc_write_go: %s", g_write_image_path);
    status_display_show_status("NFC Writing...");
    if (nfc_write_title_label && lv_obj_is_valid(nfc_write_title_label)) lv_label_set_text(nfc_write_title_label, "Present tag to write...");
#ifdef NFC_HAS_LOCAL_READER
    xTaskCreate(nfc_write_task, "nfc_write", 6144, NULL, 5, NULL);
#endif
}

// ---- End Write Flow ----

// Deep-link support for favorites: open the Saved list and show the details
// popup for a specific tag file. Safe to call before the view exists.
static void nfc_view_apply_pending_open(void);
void nfc_view_open_saved(const char *path) {
    if (!path || !path[0]) return;
    free(s_pending_saved_open);
    s_pending_saved_open = strdup(path);
    if (g_nfc_ov && lv_obj_is_valid(g_nfc_ov)) {
        nfc_view_apply_pending_open();
    }
}

static void nfc_view_apply_pending_open(void) {
    if (!s_pending_saved_open) return;
    char path[256];
    strncpy(path, s_pending_saved_open, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(s_pending_saved_open);
    s_pending_saved_open = NULL;
    saved_enter_list();
    create_saved_details_popup(path);
}

void nfc_view_create(void) {
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
    root = gui_screen_create_root_no_bg(NULL, NULL, lv_color_hex(GUI_DEFAULT_BG_COLOR), LV_OPA_TRANSP);
    nfc_view.root = root;
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    g_nfc_ov = options_view_create(root, "NFC");
    menu_container = options_view_get_list(g_nfc_ov);

#ifdef CONFIG_USE_TOUCHSCREEN
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_H;
    const int TOUCH_BAR_HEIGHT = gui_touch_bar_height();
    int container_height = LV_VER_RES - STATUS_BAR_HEIGHT - TOUCH_BAR_HEIGHT;
    lv_obj_set_size(menu_container, GUI_OPTIONS_LIST_WIDTH, container_height);
    lv_obj_align(menu_container, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);

    s_touch_tb = gui_touch_bar_create(root);
    scroll_up_btn = s_touch_tb.up_btn;
    back_btn = s_touch_tb.back_btn;
    scroll_down_btn = s_touch_tb.down_btn;
    if (s_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_touch_tb, scroll_nfc_up, NULL, back_event_cb, NULL, scroll_nfc_down, NULL);
    }
#endif

    scan_btn = options_view_add_item(g_nfc_ov, "Scan", nfc_option_event_cb, (void *)"Scan");
    if (scan_btn) lv_obj_set_user_data(scan_btn, (void *)"Scan");
#if defined(CONFIG_NFC_PN532) && defined(CONFIG_NFC_ST25R3916)
    nfc_add_backend_item();
#endif
#ifdef CONFIG_NFC_ST25R3916
    options_view_add_item(g_nfc_ov, "iCLASS / PicoPass", nfc_option_event_cb, (void *)"iCLASS / PicoPass");
#endif
    options_view_add_item(g_nfc_ov, "Saved", nfc_option_event_cb, (void *)"Saved");
    options_view_add_item(g_nfc_ov, "MIFARE Classic", nfc_option_event_cb, (void *)"MIFARE Classic");
    options_view_add_item(g_nfc_ov, "Tools", nfc_option_event_cb, (void *)"Tools");
    num_items = options_view_get_item_count(g_nfc_ov);

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK)
    options_view_add_back_row(g_nfc_ov, nfc_option_event_cb, (void *)"__BACK_OPTION__");
    num_items = options_view_get_item_count(g_nfc_ov);
#endif

    /* Root-menu highlight only resets on a genuine fresh entry from the
     * Main Menu; returning here (e.g. after a hardware shortcut jumped
     * elsewhere and back) restores the previously highlighted row instead
     * of always landing on the first item. Submenu state (in_saved_list
     * etc.) still always resets to root on re-entry, unchanged. */
    if (gui_router_previous_view() == &main_menu_view) {
        selected_index = 0;
    } else if (selected_index < 0 || selected_index >= num_items) {
        selected_index = 0;
    }
    if (g_nfc_ov) options_view_set_selected(g_nfc_ov, selected_index);

    nfc_created_time_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    nfc_option_invoked = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&nfc_touch_drag);
#endif

#ifdef CONFIG_USE_TOUCHSCREEN
    update_nfc_scroll_buttons_visibility();
#endif

    // Consume any pending favorite deep-link (nfc_view_open_saved).
    nfc_view_apply_pending_open();
}

void nfc_view_destroy(void) {
    ESP_LOGI(TAG, "nfc_view_destroy");
    // Ensure any running scan is cancelled and resources are released
    cleanup_nfc_scan_popup(NULL); // sets nfc_scan_cancel=true
    nfc_scan_cancel = true;
    // Cancel any active write and cleanup popup
    nfc_write_cancel = true;
    cleanup_nfc_write_popup(NULL);
    // Cleanup emulate popup/list
    cleanup_nfc_emu_popup(NULL);
    nfc_clear_emulate_list();
    in_emulate_list = false;
    // cleanup chameleon popup
    cleanup_cu_popup(NULL);
    cleanup_nfc_credits_popup(NULL);
    // Cleanup saved popup and list
    popup_confirm_close(&saved_delete_confirm_popup);
    cleanup_saved_details_popup(NULL);
    saved_clear_list();
    in_saved_list = false;
    in_generate_list = false;
    in_mfc_menu = false;
    in_tools_menu = false;
    nfc_option_invoked = false;
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&nfc_touch_drag);
#endif

    if (g_nfc_ov) { options_view_destroy(g_nfc_ov); g_nfc_ov = NULL; }
    lvgl_obj_del_safe(&root);
    nfc_view.root = NULL;
    menu_container = NULL;
    scan_btn = NULL;
    emulate_btn = NULL;
    gui_touch_bar_destroy(&s_touch_tb);
    scroll_up_btn = NULL;
    scroll_down_btn = NULL;
    back_btn = NULL;
    nfc_scan_popup = NULL;
    nfc_scan_cancel_btn = NULL;
    nfc_title_label = NULL;
    nfc_uid_label = NULL;
    nfc_type_label = NULL;
    nfc_details_label = NULL;

#ifdef NFC_HAS_LOCAL_READER
    // If scan task already exited, release local reader here as a safety net
    if (nfc_scan_task_handle == NULL && g_pn532) {
        pn532_release(g_pn532);
        pn532_delete_driver(g_pn532);
        g_pn532 = NULL;
    }
    if (nfc_details_text) { free(nfc_details_text); nfc_details_text = NULL; }
    nfc_details_ready = false;
    nfc_details_visible = false;
#endif
}

void get_nfc_callback(void **cb) {
    if (cb) *cb = nfc_view_input_cb;
}

View nfc_view = {
    .root = NULL,
    .create = nfc_view_create,
    .destroy = nfc_view_destroy,
    .input_callback = nfc_view_input_cb,
    .name = "NFC",
    .get_hardwareinput_callback = get_nfc_callback
};

static lv_coord_t clamp_button_width(lv_coord_t desired, lv_coord_t min_w, lv_coord_t max_w) {
    if (desired < min_w) return min_w;
    if (desired > max_w) return max_w;
    return desired;
}

static void layout_popup_buttons_row(
    lv_obj_t *popup,
    lv_obj_t **btns,
    int count,
    lv_coord_t min_w,
    lv_coord_t max_w,
    lv_coord_t min_threshold,
    lv_coord_t gap,
    lv_coord_t yoff
) {
    if (!popup || !btns || count <= 0) return;

    /* Respect the popup's own left/right padding so we don't double-count margins. */
    lv_coord_t popup_w = lv_obj_get_width(popup);
    lv_coord_t left_pad = lv_obj_get_style_pad_left(popup, LV_PART_MAIN);
    lv_coord_t right_pad = lv_obj_get_style_pad_right(popup, LV_PART_MAIN);
    if (left_pad == 0 && right_pad == 0) {
        /* fallback to prior behavior for older themes */
        left_pad = 10; right_pad = 10;
    }
    lv_coord_t available_w = popup_w - left_pad - right_pad;
    if (available_w < 0) available_w = popup_w;

    /* Compute a per-button width that fits the available area, honor min/max */
    lv_coord_t btn_w = (available_w - (gap * (count - 1))) / count;
    btn_w = clamp_button_width(btn_w, min_w, max_w);

    while (((btn_w * count) + (gap * (count - 1))) > available_w && btn_w > min_threshold) {
        btn_w--;
    }
    if (btn_w < min_threshold) btn_w = min_threshold;

    /* Center the group within available area (inside popup padding) */
    lv_coord_t total_w = (btn_w * count) + (gap * (count - 1));
    lv_coord_t start_x = left_pad;
    if (available_w > total_w) start_x += (available_w - total_w) / 2;

    lv_coord_t x = start_x;
    lv_coord_t btn_h = 0;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *btn = btns[i];
        if (!btn || !lv_obj_is_valid(btn)) continue;
        if (btn_h == 0) {
            btn_h = lv_obj_get_height(btn);
            if (btn_h <= 0) btn_h = (LV_HOR_RES <= 240) ? 30 : 34;
        }
        lv_obj_set_size(btn, btn_w, btn_h);
        lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, yoff);
        x += btn_w + gap;
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_obj_center(lbl);
    }
}
