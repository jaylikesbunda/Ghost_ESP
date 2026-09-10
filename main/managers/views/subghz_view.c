#include "managers/views/subghz_view.h"
#include "sdkconfig.h"
#include "esp_attr.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "gui/gui_router.h"

#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)

#include "core/esp_comm_manager.h"
#include "gui/lvgl_safe.h"
#include "gui/options_view.h"
#include "gui/popup.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "managers/sd_card_manager.h"
#include "managers/status_display_manager.h"
#include "managers/subghz_decoders.h"
#include "managers/subghz_remote_manager.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/options_screen.h"

#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_driver.h"
#endif

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifndef CONFIG_SUBGHZ_BASE_FREQ_MHZ
#define CONFIG_SUBGHZ_BASE_FREQ_MHZ 43392
#endif

#define SUBGHZ_BASE_FREQ_HZ ((int)CONFIG_SUBGHZ_BASE_FREQ_MHZ * 10000)

#ifndef CONFIG_SUBGHZ_CHANNEL_STEP_KHZ
#define CONFIG_SUBGHZ_CHANNEL_STEP_KHZ 200
#endif

#ifndef CONFIG_SUBGHZ_ANALYZER_SETTLE_US
#define CONFIG_SUBGHZ_ANALYZER_SETTLE_US 250
#endif

#ifndef CONFIG_SUBGHZ_ANALYZER_CHANNELS_PER_TICK
#define CONFIG_SUBGHZ_ANALYZER_CHANNELS_PER_TICK 8
#endif

#ifdef CONFIG_USE_TOUCHSCREEN
/* Touch bar sizing now owned by gui/touch_bar.h (GUI_TOUCH_BAR_*). */
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
static const int SUBGHZ_SWIPE_THRESHOLD_RATIO = 1;
#else
static const int SUBGHZ_SWIPE_THRESHOLD_RATIO = 10;
#endif
#endif

typedef enum {
    SUBGHZ_POPUP_START = 0,
    SUBGHZ_POPUP_STOP,
    SUBGHZ_POPUP_BACK,
    SUBGHZ_POPUP_COUNT
} subghz_popup_control_t;

typedef enum {
    SUBGHZ_CAPTURE_MODE_NORMAL = 0,
    SUBGHZ_CAPTURE_MODE_RAW,
} subghz_capture_mode_t;

typedef enum {
    SUBGHZ_CAPTURE_POPUP_SAVE = 0,
    SUBGHZ_CAPTURE_POPUP_FREQ,
    SUBGHZ_CAPTURE_POPUP_BACK,
    SUBGHZ_CAPTURE_POPUP_COUNT
} subghz_capture_popup_control_t;

typedef enum {
    SUBGHZ_SAVED_POPUP_REPLAY = 0,
    SUBGHZ_SAVED_POPUP_DELETE,
    SUBGHZ_SAVED_POPUP_BACK,
    SUBGHZ_SAVED_POPUP_COUNT
} subghz_saved_popup_control_t;

typedef enum {
    SUBGHZ_FA_POPUP_START = 0,
    SUBGHZ_FA_POPUP_STOP,
    SUBGHZ_FA_POPUP_BACK,
    SUBGHZ_FA_POPUP_COUNT
} subghz_fa_popup_control_t;

#define SUBGHZ_FA_BAND_COUNT 5
#define SUBGHZ_FA_HISTORY_MAX 4
#define SUBGHZ_FA_DETECT_THRESHOLD 50
#define SUBGHZ_FA_DETECT_HITS 3

typedef enum {
    SUBGHZ_PENDING_NONE = 0,
    SUBGHZ_PENDING_CAPTURE,
    SUBGHZ_PENDING_SAVE,
    SUBGHZ_PENDING_REPLAY,
    SUBGHZ_PENDING_LIST
} subghz_pending_action_t;

static const char *TAG = "SubGHzView";

static lv_obj_t *s_root = NULL;
static options_view_t *s_ov = NULL;
static int s_root_selected_index = 0;
static lv_obj_t *s_scan_row = NULL;
static lv_obj_t *s_capture_row = NULL;
static lv_obj_t *s_raw_capture_row = NULL;
static lv_obj_t *s_freq_analyzer_row = NULL;
static lv_obj_t *s_back_row = NULL;

#ifdef CONFIG_USE_TOUCHSCREEN
static lv_obj_t *s_scroll_up_btn = NULL;
static lv_obj_t *s_scroll_down_btn = NULL;
static lv_obj_t *s_back_btn = NULL;
static gui_touch_bar_t s_touch_tb = {0};
static touch_drag_t s_touch_drag = {0};
#endif

static lv_obj_t *s_popup = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_freq_label = NULL;
static lv_obj_t *s_graph = NULL;
static lv_obj_t *s_start_btn = NULL;
static lv_obj_t *s_stop_btn = NULL;
static lv_obj_t *s_popup_back_btn = NULL;
static lv_timer_t *s_timer = NULL;

static lv_obj_t *s_capture_popup = NULL;
static lv_obj_t *s_capture_status_label = NULL;
static lv_obj_t *s_capture_signal_label = NULL;
static lv_obj_t *s_capture_save_btn = NULL;
static lv_obj_t *s_capture_freq_btn = NULL;
static lv_obj_t *s_capture_back_btn = NULL;
static bool s_capture_waiting_signal = false;
static bool s_capture_ready = false;
static int s_capture_signal_hits = 0;
static bool s_capture_was_running = false;
static bool s_capture_was_running_known = false;
static bool s_capture_remote_arm_pending = false;
static bool s_capture_defer_popup_open = false;
static bool s_capture_stop_pending = false;
static int64_t s_capture_stop_deadline_us = 0;
static int64_t s_capture_popup_opened_us = 0;
static bool s_capture_popup_open_pending = false;
static bool s_capture_popup_sync_pending = false;
static subghz_capture_popup_control_t s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
static char s_capture_freq_label[16] = "433.92";
static subghz_capture_mode_t s_capture_mode = SUBGHZ_CAPTURE_MODE_NORMAL;

static lv_obj_t *s_saved_popup = NULL;
static lv_obj_t *s_saved_status_label = NULL;
static lv_obj_t *s_saved_title_label = NULL;
static lv_obj_t *s_saved_replay_btn = NULL;
static lv_obj_t *s_saved_delete_btn = NULL;
static lv_obj_t *s_saved_back_btn = NULL;
static char **s_saved_file_paths = NULL;
static int s_saved_file_count = 0;
static int s_saved_page = 0;
static int s_saved_index = 0;
static bool s_in_saved_list = false;
static subghz_saved_popup_control_t s_saved_popup_selected = SUBGHZ_SAVED_POPUP_REPLAY;
static popup_confirm_t *s_saved_delete_confirm = NULL;

static lv_obj_t *s_fa_popup = NULL;
static lv_obj_t *s_fa_freq_label = NULL;
static lv_obj_t *s_fa_graph = NULL;
static lv_obj_t *s_fa_history_label = NULL;
static lv_obj_t *s_fa_start_btn = NULL;
static lv_obj_t *s_fa_stop_btn = NULL;
static lv_obj_t *s_fa_back_btn = NULL;
static subghz_fa_popup_control_t s_fa_popup_selected = SUBGHZ_FA_POPUP_START;
static bool s_fa_scanning = false;
static uint32_t s_fa_detected_freq_hz = 0;
static bool s_fa_signal_detected = false;
static int s_fa_detect_hits = 0;
static uint32_t s_fa_history_freq[SUBGHZ_FA_HISTORY_MAX] = {0};
static uint8_t s_fa_history_count[SUBGHZ_FA_HISTORY_MAX] = {0};
static int s_fa_history_entries = 0;
static uint8_t s_fa_band_levels[SUBGHZ_FA_BAND_COUNT] = {0};
static uint8_t s_fa_band_peaks[SUBGHZ_FA_BAND_COUNT] = {0};
static uint8_t s_fa_active_band = 2;

#define WF_CHANNELS SUBGHZ_SCANNER_CHANNEL_COUNT
#define WF_COMPOSITE_CHANNELS (SUBGHZ_FA_BAND_COUNT * SUBGHZ_SCANNER_CHANNEL_COUNT)
#define WF_BAND_MASK_ALL ((1U << SUBGHZ_FA_BAND_COUNT) - 1U)
#define WF_BAND_SEPARATOR_PX 1
#define WF_ROUND_DIV(a, b) (((a) + (b) / 2) / (b))

static lv_obj_t *s_wf_popup = NULL;
static lv_obj_t *s_wf_graph = NULL;
static lv_obj_t *s_wf_status_label = NULL;
static lv_obj_t *s_wf_legend_labels[SUBGHZ_FA_BAND_COUNT] = {0};
static lv_obj_t *s_wf_start_btn = NULL;
static lv_obj_t *s_wf_stop_btn = NULL;
static lv_obj_t *s_wf_back_btn = NULL;
static lv_obj_t *s_waterfall_row = NULL;
static lv_obj_t *s_wf_canvas = NULL;

typedef enum {
    SUBGHZ_WF_POPUP_START = 0,
    SUBGHZ_WF_POPUP_STOP,
    SUBGHZ_WF_POPUP_BACK,
    SUBGHZ_WF_POPUP_COUNT
} subghz_wf_popup_control_t;

static subghz_wf_popup_control_t s_wf_popup_selected = SUBGHZ_WF_POPUP_START;
static bool s_wf_scanning = false;

static lv_color_t s_wf_palette[256];
static lv_color_t *s_wf_fb = NULL;
static lv_coord_t s_wf_fb_w = 0;
static lv_coord_t s_wf_fb_h = 0;
static uint8_t *s_wf_line = NULL;
static lv_color_t *s_wf_prev_row = NULL;
static bool s_wf_have_prev = false;
static bool s_wf_center_zero = false;
static const uint8_t s_wf_display_band_order[SUBGHZ_FA_BAND_COUNT] = { 0, 1, 2, 3, 4 };
static uint8_t s_wf_band_lines[SUBGHZ_FA_BAND_COUNT][SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
static uint8_t s_wf_band_raw_peak[SUBGHZ_FA_BAND_COUNT] = {0};
static uint8_t s_wf_band_mask = 0;
static uint8_t s_wf_pending_composite[WF_COMPOSITE_CHANNELS] = {0};
static uint16_t s_wf_pending_composite_count = 0;
static uint16_t s_wf_pending_composite_seq = 0;
static uint16_t s_wf_rendered_composite_seq = 0;
static uint8_t s_wf_last_peak_band = 0;
static uint8_t s_wf_last_peak_ch = 0;
static uint8_t s_wf_last_peak_level = 0;
static portMUX_TYPE s_wf_composite_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_wf_remote_line[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
static uint8_t s_wf_remote_build_line[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
static uint8_t s_wf_remote_count = 0;
static uint8_t s_wf_remote_freq_idx = 2;
static uint8_t s_wf_remote_last_cursor = 0;
static uint16_t s_wf_remote_seq = 0;
static uint16_t s_wf_remote_build_seq = 0;
static uint8_t s_wf_remote_build_count = 0;
static uint8_t s_wf_remote_build_received = 0;
static uint8_t s_wf_remote_build_mask = 0;
static uint16_t s_wf_rendered_remote_seq = 0;
static bool s_wf_remote_ready = false;
static bool s_wf_remote_have_cursor = false;

static uint8_t s_levels[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
static uint8_t s_peaks[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
static uint8_t s_cursor = 0;
EXT_RAM_BSS_ATTR static int32_t s_capture_raw[SUBGHZ_RAW_MAX_DURATIONS] = {0};
static size_t s_capture_raw_count = 0;
static uint8_t s_capture_preview_cursor = 0;
static bool s_capture_buffer_valid = false;
static char s_capture_name[SUBGHZ_SNAPSHOT_NAME_MAX] = "capture";
EXT_RAM_BSS_ATTR static int32_t s_remote_raw_work[SUBGHZ_RAW_MAX_DURATIONS] = {0};
static size_t s_remote_raw_expected = 0;
static size_t s_remote_raw_received = 0;
static subghz_decoded_signal_t s_remote_decoded = {0};

#define SUBGHZ_CAPTURE_SIGNAL_THRESHOLD 65
#define SUBGHZ_CAPTURE_SIGNAL_HITS      2
#define SUBGHZ_SNAPSHOT_DIR             "/mnt/ghostesp/subghz"
#define SUBGHZ_SNAPSHOT_EXT             ".sub"

static bool s_remote_mode = false;
static volatile bool s_remote_stream_online = false;
static volatile bool s_remote_error = false;
static volatile bool s_remote_paused = true;
static volatile uint8_t s_remote_freq_idx = 2;
static const uint32_t s_scan_freqs_hz[] = {
    315000000U, 390000000U, 433920000U, 868350000U, 915000000U
};
static const char *s_scan_freq_labels[] = {
    "315 MHz", "390 MHz", "433.92 MHz", "868.35 MHz", "915 MHz"
};
static const char *s_scan_freq_short[] = {
    "315", "390", "433.92", "868.35", "915"
};
static subghz_popup_control_t s_popup_selected = SUBGHZ_POPUP_START;
static subghz_pending_action_t s_pending_action = SUBGHZ_PENDING_NONE;
static int64_t s_pending_action_deadline_us = 0;

static void subghz_open_capture_popup(void);
static void subghz_open_saved_popup(void);
static void subghz_open_freq_analyzer_popup(void);
static void subghz_close_freq_analyzer_popup(void);
static void subghz_open_waterfall_popup(void);
static void subghz_close_waterfall_popup(void);
static void subghz_waterfall_row_cb(lv_event_t *e);
static void subghz_wf_start_btn_cb(lv_event_t *e);
static void subghz_wf_stop_btn_cb(lv_event_t *e);
static void subghz_wf_back_btn_cb(lv_event_t *e);
static void subghz_saved_list_reload(void);
static void subghz_scan_row_cb(lv_event_t *e);
static void subghz_capture_row_cb(lv_event_t *e);
static void subghz_raw_capture_row_cb(lv_event_t *e);
static void subghz_freq_analyzer_row_cb(lv_event_t *e);
static void subghz_back_row_cb(lv_event_t *e);
static void subghz_saved_back_btn_cb(lv_event_t *e);
static void subghz_capture_primary_btn_cb(lv_event_t *e);
static void subghz_capture_freq_btn_cb(lv_event_t *e);
static void subghz_capture_back_btn_cb(lv_event_t *e);
static void subghz_capture_mark_ready(void);
static void subghz_capture_mark_ready_with_decoded(const subghz_decoded_signal_t *decoded);
static void subghz_capture_popup_update_buttons(void);
static void subghz_capture_refresh_frequency_label(void);
static void subghz_capture_reset_state_for_new_attempt(void);
static void subghz_capture_apply_waiting_ui(bool arming_remote);
static bool subghz_capture_popup_ignore_activation(void);
static void subghz_capture_continue_waiting(void);
static uint8_t subghz_capture_get_frequency_index(void);
static void subghz_capture_set_frequency_index(uint8_t idx);
static bool subghz_capture_begin_remote(bool defer_popup_open);
static bool subghz_build_snapshot_detail_text(const char *name, char *out, size_t out_len);
#ifdef CONFIG_USE_TOUCHSCREEN
static void subghz_update_scroll_buttons_visibility(void);
#endif

static bool subghz_is_remote_mode(void) {
#if defined(CONFIG_HAS_SUBGHZ_REMOTE) && !defined(CONFIG_HAS_SUBGHZ)
    return true;
#else
    return false;
#endif
}

static bool point_inside_obj(lv_obj_t *obj, lv_coord_t x, lv_coord_t y) {
    if (!obj || !lv_obj_is_valid(obj)) {
        return false;
    }

    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return (x >= area.x1 && x <= area.x2 && y >= area.y1 && y <= area.y2);
}

static void subghz_apply_popup_selection(void) {
    popup_set_button_selected(s_start_btn, s_popup_selected == SUBGHZ_POPUP_START);
    popup_set_button_selected(s_stop_btn, s_popup_selected == SUBGHZ_POPUP_STOP);
    popup_set_button_selected(s_popup_back_btn, s_popup_selected == SUBGHZ_POPUP_BACK);
}

static void subghz_show_action_status(const char *msg) {
    if (!msg || msg[0] == '\0') {
        return;
    }

    status_display_show_status(msg);

    if (s_status_label && lv_obj_is_valid(s_status_label)) {
        lv_label_set_text(s_status_label, msg);
    }
}

static void subghz_show_feedback_popup(const char *title, const char *detail) {
    char popup_msg[220];
    if (title && detail && detail[0] != '\0') {
        snprintf(popup_msg, sizeof(popup_msg), "%s\n%s", title, detail);
    } else if (title) {
        snprintf(popup_msg, sizeof(popup_msg), "%s", title);
    } else if (detail) {
        snprintf(popup_msg, sizeof(popup_msg), "%s", detail);
    } else {
        snprintf(popup_msg, sizeof(popup_msg), "SubGHz update");
    }

    error_popup_create(popup_msg);
}

static const char *subghz_pending_action_label(subghz_pending_action_t action) {
    switch (action) {
    case SUBGHZ_PENDING_CAPTURE:
        return "capture";
    case SUBGHZ_PENDING_SAVE:
        return "save";
    case SUBGHZ_PENDING_REPLAY:
        return "replay";
    case SUBGHZ_PENDING_LIST:
        return "list";
    default:
        return "request";
    }
}

static void subghz_set_pending_action(subghz_pending_action_t action, uint32_t timeout_ms) {
    s_pending_action = action;
    s_pending_action_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
}

static void subghz_clear_pending_action(void) {
    s_pending_action = SUBGHZ_PENDING_NONE;
    s_pending_action_deadline_us = 0;
}

static void subghz_fail_pending_action(const char *reason) {
    if (s_pending_action == SUBGHZ_PENDING_NONE) {
        return;
    }

    bool deferred_capture_without_popup = s_capture_defer_popup_open &&
        (!s_capture_popup || !lv_obj_is_valid(s_capture_popup));
    s_capture_remote_arm_pending = false;
    s_capture_defer_popup_open = false;
    if (deferred_capture_without_popup) {
        s_capture_was_running_known = false;
    }

    const char *label = subghz_pending_action_label(s_pending_action);
    char msg[128];
    if (reason && reason[0] != '\0') {
        snprintf(msg, sizeof(msg), "Remote %s failed: %s", label, reason);
    } else {
        snprintf(msg, sizeof(msg), "Remote %s failed", label);
    }

    subghz_show_action_status(msg);
    subghz_show_feedback_popup("SubGHz error", msg);
    s_remote_error = true;
    subghz_clear_pending_action();
}

static void subghz_capture_refresh_frequency_label(void) {
    const char *label = NULL;

    if (s_remote_mode) {
        if (s_capture_freq_label[0] != '\0' && strcmp(s_capture_freq_label, "N/A") != 0) {
            return;
        }
        uint8_t idx = (s_remote_freq_idx < SUBGHZ_FA_BAND_COUNT) ? s_remote_freq_idx : 2;
        label = s_scan_freq_short[idx];
    } else {
        const char *manager_label = subghz_remote_manager_get_frequency_label();
        if (manager_label && manager_label[0] != '\0' && strcmp(manager_label, "N/A") != 0) {
            uint8_t idx = subghz_capture_get_frequency_index();
            label = s_scan_freq_short[idx];
        }
    }

    if (!label) {
        if (s_capture_freq_label[0] != '\0' && strcmp(s_capture_freq_label, "N/A") != 0) {
            return;
        }
        label = s_scan_freq_short[2];
    }

    snprintf(s_capture_freq_label, sizeof(s_capture_freq_label), "%s", label);
}

static void subghz_capture_reset_state_for_new_attempt(void) {
    s_capture_waiting_signal = false;
    s_capture_ready = false;
    s_capture_signal_hits = 0;
    s_capture_popup_selected = (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW)
        ? SUBGHZ_CAPTURE_POPUP_SAVE : SUBGHZ_CAPTURE_POPUP_BACK;
    s_capture_buffer_valid = false;
    s_capture_raw_count = 0;
    s_remote_raw_expected = 0;
    s_remote_raw_received = 0;
    s_capture_stop_pending = false;
    s_capture_stop_deadline_us = 0;
    s_capture_popup_open_pending = false;
    s_capture_popup_sync_pending = false;
    if (s_remote_decoded.preserved_extra) {
        free(s_remote_decoded.preserved_extra);
        s_remote_decoded.preserved_extra = NULL;
    }
    memset(&s_remote_decoded, 0, sizeof(s_remote_decoded));
}

static void subghz_capture_apply_waiting_ui(bool arming_remote) {
    if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
        if (arming_remote) {
            lv_label_set_text(s_capture_status_label, "Arming remote capture...");
        } else if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
            char status[64];
            snprintf(status, sizeof(status), "Raw: %s", s_capture_freq_label);
            lv_label_set_text(s_capture_status_label, status);
        } else {
            char status[64];
            snprintf(status, sizeof(status), "Listening: %s", s_capture_freq_label);
            lv_label_set_text(s_capture_status_label, status);
        }
    }

    if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label)) {
        lv_label_set_text(s_capture_signal_label,
                          arming_remote ? "Waiting for peer acknowledgement" : "Signal threshold: 65%");
    }

    subghz_capture_popup_update_buttons();
}

static bool subghz_capture_popup_ignore_activation(void) {
    if (s_capture_popup_opened_us <= 0) {
        return false;
    }
    int64_t age_us = esp_timer_get_time() - s_capture_popup_opened_us;
    bool ignore = age_us < 400000LL;
    if (ignore) {
        ESP_LOGI(TAG, "ignoring capture popup activation age=%lldus", (long long)age_us);
    }
    return ignore;
}

static void subghz_capture_popup_update_buttons(void) {
    if (!s_capture_popup || !lv_obj_is_valid(s_capture_popup)) {
        return;
    }

    bool arming_remote = s_capture_remote_arm_pending || s_capture_stop_pending;
    bool capture_stopped_without_data = s_capture_ready && !s_capture_buffer_valid;

    if (s_capture_save_btn && lv_obj_is_valid(s_capture_save_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(s_capture_save_btn, 0);
        if (lbl && lv_obj_is_valid(lbl)) {
            if (arming_remote) {
                lv_label_set_text(lbl, "Wait");
                lv_obj_add_state(s_capture_save_btn, LV_STATE_DISABLED);
            } else if (s_capture_ready) {
                if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
                    if (capture_stopped_without_data) {
                        lv_label_set_text(lbl, "Start");
                        lv_obj_clear_state(s_capture_save_btn, LV_STATE_DISABLED);
                    } else {
                        lv_label_set_text(lbl, "Save");
                        lv_obj_clear_state(s_capture_save_btn, LV_STATE_DISABLED);
                    }
                } else {
                    lv_label_set_text(lbl, "Save");
                    if (s_capture_buffer_valid) {
                        lv_obj_clear_state(s_capture_save_btn, LV_STATE_DISABLED);
                    } else {
                        lv_obj_add_state(s_capture_save_btn, LV_STATE_DISABLED);
                    }
                }
            } else {
                lv_label_set_text(lbl, "Stop");
                lv_obj_clear_state(s_capture_save_btn, LV_STATE_DISABLED);
            }
        }
    }

    if (s_capture_freq_btn && lv_obj_is_valid(s_capture_freq_btn)) {
        lv_obj_t *lbl = lv_obj_get_child(s_capture_freq_btn, 0);
        if (lbl && lv_obj_is_valid(lbl)) {
            if (arming_remote) {
                lv_label_set_text(lbl, s_capture_freq_label);
                lv_obj_add_state(s_capture_freq_btn, LV_STATE_DISABLED);
            } else if (s_capture_ready && s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
                lv_label_set_text(lbl, "Start");
                lv_obj_clear_state(s_capture_freq_btn, LV_STATE_DISABLED);
            } else {
                lv_label_set_text(lbl, s_capture_freq_label);
                lv_obj_clear_state(s_capture_freq_btn, LV_STATE_DISABLED);
            }
        } else {
            lv_obj_clear_state(s_capture_freq_btn, LV_STATE_DISABLED);
        }
    }

    popup_set_button_selected(s_capture_save_btn, s_capture_popup_selected == SUBGHZ_CAPTURE_POPUP_SAVE);
    popup_set_button_selected(
        s_capture_freq_btn,
        s_capture_popup_selected == SUBGHZ_CAPTURE_POPUP_FREQ);
    popup_set_button_selected(s_capture_back_btn, s_capture_popup_selected == SUBGHZ_CAPTURE_POPUP_BACK);
}

static void subghz_saved_popup_update_buttons(void) {
    popup_set_button_selected(s_saved_replay_btn, s_saved_popup_selected == SUBGHZ_SAVED_POPUP_REPLAY);
    popup_set_button_selected(s_saved_delete_btn, s_saved_popup_selected == SUBGHZ_SAVED_POPUP_DELETE);
    popup_set_button_selected(s_saved_back_btn, s_saved_popup_selected == SUBGHZ_SAVED_POPUP_BACK);
}

static void subghz_saved_popup_refresh_text(void) {
    if (!s_saved_status_label || !lv_obj_is_valid(s_saved_status_label)) {
        return;
    }

    if (s_saved_file_count <= 0 || !s_saved_file_paths) {
        lv_label_set_text(s_saved_status_label, "No captures on SD\n/mnt/ghostesp/subghz");
        if (s_saved_replay_btn && lv_obj_is_valid(s_saved_replay_btn)) {
            lv_obj_add_state(s_saved_replay_btn, LV_STATE_DISABLED);
        }
        if (s_saved_delete_btn && lv_obj_is_valid(s_saved_delete_btn)) {
            lv_obj_add_state(s_saved_delete_btn, LV_STATE_DISABLED);
        }
        return;
    }

    if (s_saved_index < 0) {
        s_saved_index = 0;
    }
    if (s_saved_index >= s_saved_file_count) {
        s_saved_index = s_saved_file_count - 1;
    }

    if (s_saved_replay_btn && lv_obj_is_valid(s_saved_replay_btn)) {
        lv_obj_clear_state(s_saved_replay_btn, LV_STATE_DISABLED);
    }
    if (s_saved_delete_btn && lv_obj_is_valid(s_saved_delete_btn)) {
        lv_obj_clear_state(s_saved_delete_btn, LV_STATE_DISABLED);
    }

    char msg[2048];
    if (!subghz_build_snapshot_detail_text(s_saved_file_paths[s_saved_index], msg, sizeof(msg))) {
        snprintf(msg, sizeof(msg), "%d/%d\nFailed to read capture details", s_saved_index + 1, s_saved_file_count);
    }
    lv_label_set_text(s_saved_status_label, msg);
}

static void subghz_sanitize_snapshot_name(const char *name_hint, char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    size_t pos = 0;
    if (name_hint) {
        for (size_t i = 0; name_hint[i] != '\0' && pos < out_len - 1; i++) {
            char c = name_hint[i];
            if ((c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '_' || c == '-') {
                out[pos++] = c;
            } else if (c == ' ' || c == '.') {
                out[pos++] = '_';
            }
        }
    }

    if (pos == 0) {
        snprintf(out, out_len, "capture_%08X", (unsigned)(esp_timer_get_time() / 1000ULL));
        return;
    }

    out[pos] = '\0';
}

static bool subghz_sd_begin(bool *display_was_suspended) {
    if (display_was_suspended) {
        *display_was_suspended = false;
    }

    if (sd_card_needs_jit_mount()) {
        esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
        if (mount_err != ESP_OK) {
            return false;
        }
        (void)sd_card_setup_directory_structure();
    }

    return true;
}

static void subghz_sd_end(bool display_was_suspended) {
    if (sd_card_needs_jit_mount()) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
}

static bool subghz_local_save_snapshot(const char *name_hint,
                                       const int32_t *durations,
                                       size_t duration_count,
                                       const subghz_decoded_signal_t *decoded,
                                       char *out_name,
                                       size_t out_name_len,
                                       char *out_error,
                                       size_t out_error_len) {
    if ((!durations || duration_count == 0) && !(decoded && decoded->decoded)) {
        if (out_error && out_error_len) snprintf(out_error, out_error_len, "No capture data");
        return false;
    }

    bool display_was_suspended = false;
    if (!subghz_sd_begin(&display_was_suspended)) {
        if (out_error && out_error_len) snprintf(out_error, out_error_len, "SD mount failed");
        return false;
    }

    if (mkdir(SUBGHZ_SNAPSHOT_DIR, 0777) != 0 && errno != EEXIST) {
        if (out_error && out_error_len) snprintf(out_error, out_error_len, "Failed to create subghz dir");
        subghz_sd_end(display_was_suspended);
        return false;
    }

    char safe_name[SUBGHZ_SNAPSHOT_NAME_MAX];
    subghz_sanitize_snapshot_name(name_hint, safe_name, sizeof(safe_name));

    char path[192];
    snprintf(path, sizeof(path), "%s/%s%s", SUBGHZ_SNAPSHOT_DIR, safe_name, SUBGHZ_SNAPSHOT_EXT);

    FILE *f = fopen(path, "w");
    if (!f) {
        if (out_error && out_error_len) snprintf(out_error, out_error_len, "Failed to open snapshot file");
        subghz_sd_end(display_was_suspended);
        return false;
    }

    fprintf(f, "Filetype: Flipper SubGhz Key File\n");
    fprintf(f, "Version: 1\n");
    int save_freq = (decoded && decoded->decoded && decoded->frequency_hz > 0)
                        ? decoded->frequency_hz
                        : SUBGHZ_BASE_FREQ_HZ;
    fprintf(f, "Frequency: %d\n", save_freq);

    const char *preset_str = "FuriHalSubGhzPresetOok270Async";
    if (decoded && decoded->decoded && decoded->preset_name[0] != '\0') {
        preset_str = decoded->preset_name;
    } else if (decoded && decoded->decoded) {
        subghz_preset_t dp = SUBGHZ_PRESET_OOK270_ASYNC;
        /* infer from decoded if no preset_name */
        (void)dp;
    }
    fprintf(f, "Preset: %s\n", preset_str);

    if (decoded && decoded->decoded && decoded->custom_preset_blob[0] != '\0') {
        fputs(decoded->custom_preset_blob, f);
        fputc('\n', f);
    }

    if (decoded && decoded->decoded) {
        int bits = subghz_normalize_decoded_bits(decoded->protocol, decoded->bits);
        fprintf(f, "Protocol: %s\n", decoded->protocol);
        fprintf(f, "Bit: %d\n", bits);
        int num_bytes = (bits + 7) / 8;
        fputs("Key: ", f);
        for (int i = num_bytes - 1; i >= 0; i--) {
            fprintf(f, "%02X", (unsigned)((decoded->code >> (i * 8)) & 0xFF));
            if (i > 0) fputc(' ', f);
        }
        fputc('\n', f);
        if (decoded->te > 0) {
            fprintf(f, "TE: %d\n", decoded->te);
        }
        fprintf(f, "Manufacture: Unknown\n");
        if (decoded->preserved_extra) {
            fputs(decoded->preserved_extra, f);
        }
    } else {
        fprintf(f, "Protocol: RAW\n");

        int values_on_line = 0;
        bool wrote_prefix = false;

        for (size_t i = 0; i < duration_count; i++) {
            if ((values_on_line % 512) == 0) {
                fprintf(f, wrote_prefix ? "\nRAW_Data: " : "RAW_Data: ");
                wrote_prefix = true;
            }
            fprintf(f, "%ld", (long)durations[i]);
            if (i + 1 < duration_count) {
                fputc(' ', f);
            }
            values_on_line++;
        }
        fputc('\n', f);
    }

    fclose(f);
    subghz_sd_end(display_was_suspended);

    if (out_name && out_name_len > 0) {
        snprintf(out_name, out_name_len, "%s", safe_name);
    }
    if (out_error && out_error_len > 0) {
        out_error[0] = '\0';
    }
    return true;
}

static int subghz_local_list_snapshots(char names[][SUBGHZ_SNAPSHOT_NAME_MAX], int max_names) {
    if (!names || max_names <= 0) {
        return 0;
    }

    bool display_was_suspended = false;
    if (!subghz_sd_begin(&display_was_suspended)) {
        return 0;
    }

    DIR *dir = opendir(SUBGHZ_SNAPSHOT_DIR);
    if (!dir) {
        subghz_sd_end(display_was_suspended);
        return 0;
    }

    int count = 0;
    struct dirent *ent = NULL;
    const size_t ext_len = strlen(SUBGHZ_SNAPSHOT_EXT);
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (!name || name[0] == '.') {
            continue;
        }

        size_t len = strlen(name);
        if (len <= ext_len) {
            continue;
        }
        if (strcmp(name + (len - ext_len), SUBGHZ_SNAPSHOT_EXT) != 0) {
            continue;
        }

        if (count < max_names) {
            size_t copy_len = len - ext_len;
            if (copy_len >= SUBGHZ_SNAPSHOT_NAME_MAX) {
                copy_len = SUBGHZ_SNAPSHOT_NAME_MAX - 1;
            }
            memcpy(names[count], name, copy_len);
            names[count][copy_len] = '\0';
        }
        count++;
    }

    closedir(dir);
    subghz_sd_end(display_was_suspended);
    return count;
}

static void subghz_saved_list_clear(void) {
    if (s_saved_file_paths) {
        for (int i = 0; i < s_saved_file_count; i++) {
            free(s_saved_file_paths[i]);
        }
        free(s_saved_file_paths);
    }
    s_saved_file_paths = NULL;
    s_saved_file_count = 0;
    s_saved_page = 0;
    s_saved_index = 0;
}

static void subghz_saved_list_item_cb(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (!path) {
        return;
    }

    for (int i = 0; i < s_saved_file_count; i++) {
        if (s_saved_file_paths[i] && strcmp(s_saved_file_paths[i], path) == 0) {
            s_saved_index = i;
            break;
        }
    }
    subghz_open_saved_popup();
}

static void subghz_back_to_root_menu(void) {
    s_in_saved_list = false;
    subghz_saved_list_clear();
    if (!s_ov) {
        return;
    }

    options_view_clear(s_ov);
    options_view_set_title(s_ov, "SubGHz");
    s_scan_row = options_view_add_item(s_ov, "Capture", subghz_scan_row_cb, NULL);
    s_raw_capture_row = options_view_add_item(s_ov, "Raw Capture", subghz_raw_capture_row_cb, NULL);
    s_capture_row = options_view_add_item(s_ov, "Saved", subghz_capture_row_cb, NULL);
    s_freq_analyzer_row = options_view_add_item(s_ov, "Freq Analyzer", subghz_freq_analyzer_row_cb, NULL);
    s_waterfall_row = options_view_add_item(s_ov, "Waterfall", subghz_waterfall_row_cb, NULL);
    s_back_row = options_view_add_back_row(s_ov, subghz_back_row_cb, NULL);
    options_view_set_selected(s_ov, 0);

#ifdef CONFIG_USE_TOUCHSCREEN
    subghz_update_scroll_buttons_visibility();
#endif
}

static void subghz_saved_list_prev_page_cb(lv_event_t *e) {
    (void)e;
    if (s_saved_page > 0) {
        s_saved_page--;
        subghz_saved_list_reload();
    }
}

static void subghz_saved_list_next_page_cb(lv_event_t *e) {
    (void)e;
    const int per_page = 7;
    int max_page = (s_saved_file_count <= 0) ? 0 : ((s_saved_file_count - 1) / per_page);
    if (s_saved_page < max_page) {
        s_saved_page++;
        subghz_saved_list_reload();
    }
}

static void subghz_saved_list_reload(void) {
    if (!s_ov) {
        return;
    }

    options_view_clear(s_ov);
    s_in_saved_list = true;
    options_view_set_title(s_ov, "Saved Signals");

    if (!s_saved_file_paths) {
        bool susp = false;
        bool did = subghz_sd_begin(&susp);
        DIR *d = opendir(SUBGHZ_SNAPSHOT_DIR);
        if (d) {
            struct dirent *de;
            int count = 0;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t len = strlen(de->d_name);
                size_t ext_len = strlen(SUBGHZ_SNAPSHOT_EXT);
                if (len > ext_len && strcmp(de->d_name + len - ext_len, SUBGHZ_SNAPSHOT_EXT) == 0) {
                    count++;
                }
            }
            rewinddir(d);
            if (count > 0) {
                s_saved_file_paths = (char **)calloc((size_t)count, sizeof(char *));
            }
            int idx = 0;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                size_t len = strlen(de->d_name);
                size_t ext_len = strlen(SUBGHZ_SNAPSHOT_EXT);
                if (!(len > ext_len && strcmp(de->d_name + len - ext_len, SUBGHZ_SNAPSHOT_EXT) == 0)) continue;
                size_t need = strlen(SUBGHZ_SNAPSHOT_DIR) + 1 + len + 1;
                char *copy = (char *)malloc(need);
                if (!copy) continue;
                snprintf(copy, need, "%s/%s", SUBGHZ_SNAPSHOT_DIR, de->d_name);
                if (s_saved_file_paths && idx < count) {
                    s_saved_file_paths[idx++] = copy;
                } else {
                    free(copy);
                }
            }
            s_saved_file_count = idx;
            closedir(d);
        }
        if (did) {
            subghz_sd_end(susp);
        }
    }

    const int per_page = 7;
    int max_page = (s_saved_file_count <= 0) ? 0 : ((s_saved_file_count - 1) / per_page);
    if (s_saved_page > max_page) {
        s_saved_page = max_page;
    }
    int start = s_saved_page * per_page;
    int end = start + per_page;
    if (end > s_saved_file_count) {
        end = s_saved_file_count;
    }

    if (s_saved_file_count == 0) {
        options_view_add_item(s_ov, "No .sub files", NULL, NULL);
    } else {
        for (int i = start; i < end; i++) {
            const char *name = strrchr(s_saved_file_paths[i], '/');
            name = name ? (name + 1) : s_saved_file_paths[i];
            options_view_add_item(s_ov, name, subghz_saved_list_item_cb, s_saved_file_paths[i]);
        }
    }

    if (s_saved_page > 0) {
        options_view_add_item(s_ov, "< Prev Page", subghz_saved_list_prev_page_cb, NULL);
    }
    if (s_saved_page < max_page) {
        options_view_add_item(s_ov, "Next Page >", subghz_saved_list_next_page_cb, NULL);
    }
    s_back_row = options_view_add_back_row(s_ov, subghz_back_row_cb, NULL);
    options_view_set_selected(s_ov, 0);

#ifdef CONFIG_USE_TOUCHSCREEN
    subghz_update_scroll_buttons_visibility();
#endif
}

static bool subghz_key_match(const char *line, const char *key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < klen; i++) {
        char a = line[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static const char *subghz_skip_key_colon(const char *line, size_t key_len) {
    const char *p = line + key_len;
    while (*p == ' ' || *p == ':') p++;
    return p;
}

static subghz_preset_t subghz_parse_preset(const char *v) {
    if (strstr(v, "Ook650") || strstr(v, "OOK650") || strstr(v, "ook650"))
        return SUBGHZ_PRESET_OOK650_ASYNC;
    if (strstr(v, "2FSK") && strstr(v, "Dev238"))
        return SUBGHZ_PRESET_2FSK_DEV238_ASYNC;
    if (strstr(v, "2FSK") && strstr(v, "Dev476"))
        return SUBGHZ_PRESET_2FSK_DEV476_ASYNC;
    if (strstr(v, "Custom") || strstr(v, "custom"))
        return SUBGHZ_PRESET_CUSTOM;
    return SUBGHZ_PRESET_OOK270_ASYNC;
}

static void subghz_trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' '))
        s[--len] = '\0';
}

static void subghz_analyze_raw_signal(const int32_t *dur, size_t count, char *out, size_t out_len) {
    if (!dur || count < 4 || !out || out_len == 0) {
        if (out && out_len > 0) out[0] = '\0';
        return;
    }

    subghz_decoded_signal_t decoded;
    if (subghz_decode_signal(dur, count, &decoded) && decoded.decoded) {
        snprintf(out, out_len, "%s", decoded.info);
        return;
    }

    int32_t min_hi = INT32_MAX, max_hi = 0, min_lo = INT32_MAX, max_lo = 0;
    int pulse_count = 0;
    for (size_t j = 0; j < count && pulse_count < 100; j++) {
        int32_t v = dur[j] > 0 ? dur[j] : -dur[j];
        if (dur[j] > 0) {
            if (v < min_hi) min_hi = v;
            if (v > max_hi) max_hi = v;
        } else {
            if (v < min_lo) min_lo = v;
            if (v > max_lo) max_lo = v;
        }
        pulse_count++;
    }
    if (pulse_count > 4) {
        snprintf(out, out_len, "RAW %zu pulses\nHi:%d-%d Lo:%d-%d",
                 count, (int)min_hi, (int)max_hi, (int)min_lo, (int)max_lo);
    } else {
        snprintf(out, out_len, "RAW %zu pulses", count);
    }
}

static bool subghz_build_snapshot_detail_text(const char *name, char *out, size_t out_len) {
    if (!name || !out || out_len == 0) {
        return false;
    }

    char path[192];
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        snprintf(path, sizeof(path), "%s", name);
    } else {
        snprintf(path, sizeof(path), "/mnt/ghostesp/subghz/%s.sub", name);
    }

    bool display_was_suspended = false;
    if (!subghz_sd_begin(&display_was_suspended)) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        subghz_sd_end(display_was_suspended);
        return false;
    }

    int frequency_hz = SUBGHZ_BASE_FREQ_HZ;
    char protocol[64] = "RAW";
    int bit_count = 0;
    char key_hex[128] = {0};
    int values = 0;
    size_t raw_cap = SUBGHZ_RAW_INITIAL_CAP;
    int32_t *raw_vals = (int32_t *)malloc(raw_cap * sizeof(int32_t));
    size_t raw_count = 0;
    bool raw_truncated = false;
    bool in_raw_data = false;

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        subghz_trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') {
            in_raw_data = false;
            continue;
        }

        if (subghz_key_match(line, "Frequency:")) {
            in_raw_data = false;
            frequency_hz = atoi(subghz_skip_key_colon(line, 10));
        } else if (subghz_key_match(line, "Protocol:")) {
            in_raw_data = false;
            const char *val = subghz_skip_key_colon(line, 9);
            snprintf(protocol, sizeof(protocol), "%.63s", val);
        } else if (subghz_key_match(line, "Bit:")) {
            in_raw_data = false;
            bit_count = atoi(subghz_skip_key_colon(line, 4));
        } else if (subghz_key_match(line, "Key:")) {
            in_raw_data = false;
            const char *val = subghz_skip_key_colon(line, 4);
            snprintf(key_hex, sizeof(key_hex), "%.127s", val);
        } else if (subghz_key_match(line, "RAW_Data:")) {
            in_raw_data = true;
            char *csv = (char *)subghz_skip_key_colon(line, 9);
            char *saveptr = NULL;
            char *tok = strtok_r(csv, " ,", &saveptr);
            while (tok) {
                if (*tok != '\0') {
                    values++;
                    if (raw_count < raw_cap) {
                        raw_vals[raw_count++] = (int32_t)strtol(tok, NULL, 10);
                    } else if (raw_cap < (size_t)SUBGHZ_RAW_MAX_CAP) {
                        size_t new_cap = raw_cap * 2;
                        if (new_cap > (size_t)SUBGHZ_RAW_MAX_CAP) new_cap = SUBGHZ_RAW_MAX_CAP;
                        int32_t *tmp = (int32_t *)realloc(raw_vals, new_cap * sizeof(int32_t));
                        if (tmp) {
                            raw_vals = tmp;
                            raw_cap = new_cap;
                            raw_vals[raw_count++] = (int32_t)strtol(tok, NULL, 10);
                        } else {
                            raw_truncated = true;
                        }
                    } else {
                        raw_truncated = true;
                    }
                }
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else if (in_raw_data) {
            char *saveptr = NULL;
            char *tok = strtok_r(line, " ,", &saveptr);
            while (tok) {
                if (*tok != '\0') {
                    values++;
                    if (raw_count < raw_cap) {
                        raw_vals[raw_count++] = (int32_t)strtol(tok, NULL, 10);
                    } else if (raw_cap < (size_t)SUBGHZ_RAW_MAX_CAP) {
                        size_t new_cap = raw_cap * 2;
                        if (new_cap > (size_t)SUBGHZ_RAW_MAX_CAP) new_cap = SUBGHZ_RAW_MAX_CAP;
                        int32_t *tmp = (int32_t *)realloc(raw_vals, new_cap * sizeof(int32_t));
                        if (tmp) {
                            raw_vals = tmp;
                            raw_cap = new_cap;
                            raw_vals[raw_count++] = (int32_t)strtol(tok, NULL, 10);
                        } else {
                            raw_truncated = true;
                        }
                    } else {
                        raw_truncated = true;
                    }
                }
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else {
            in_raw_data = false;
        }
    }
    fclose(f);
    subghz_sd_end(display_was_suspended);

    char decode_buf[128] = {0};
    if (strcmp(protocol, "RAW") == 0 && raw_count > 0) {
        subghz_analyze_raw_signal(raw_vals, raw_count, decode_buf, sizeof(decode_buf));
    }

    char freq_str[24];
    snprintf(freq_str, sizeof(freq_str), "%.3f MHz", (double)frequency_hz / 1000000.0);

    if (strcmp(protocol, "RAW") != 0 && key_hex[0] != '\0') {
        snprintf(out, out_len, "%s %dbit\n%s\n%s",
                 protocol, bit_count, key_hex, freq_str);
    } else if (decode_buf[0] != '\0') {
        if (raw_truncated) {
            snprintf(out, out_len, "%s\n%s\n%zu pulses, truncated",
                     decode_buf, freq_str, raw_count);
        } else {
            snprintf(out, out_len, "%s\n%s",
                     decode_buf, freq_str);
        }
    } else {
        if (raw_truncated) {
            snprintf(out, out_len, "RAW\n%s\n%zu pulses, truncated", freq_str, raw_count);
        } else {
            snprintf(out, out_len, "RAW\n%s", freq_str);
        }
    }

    free(raw_vals);
    return true;
}

static bool subghz_parse_raw_file(const char *path, int32_t *out, size_t max_count, size_t *out_count, uint32_t *out_frequency_hz, subghz_preset_t *out_preset) {
    if (!path || !out || max_count == 0) {
        return false;
    }

    bool display_was_suspended = false;
    if (!subghz_sd_begin(&display_was_suspended)) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        subghz_sd_end(display_was_suspended);
        return false;
    }

    size_t count = 0;
    uint32_t freq = 0;
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    bool in_raw_data = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        subghz_trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') {
            in_raw_data = false;
            continue;
        }

        if (subghz_key_match(line, "Frequency:")) {
            in_raw_data = false;
            freq = (uint32_t)atoi(subghz_skip_key_colon(line, 10));
        } else if (subghz_key_match(line, "Preset:")) {
            in_raw_data = false;
            preset = subghz_parse_preset(subghz_skip_key_colon(line, 7));
        } else if (subghz_key_match(line, "RAW_Data:")) {
            in_raw_data = true;
            char *saveptr = NULL;
            char *tok = strtok_r((char *)subghz_skip_key_colon(line, 9), " ,", &saveptr);
            while (tok && count < max_count) {
                out[count++] = (int32_t)strtol(tok, NULL, 10);
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else if (in_raw_data) {
            char *saveptr = NULL;
            char *tok = strtok_r(line, " ,", &saveptr);
            while (tok && count < max_count) {
                if (*tok != '\0') {
                    out[count++] = (int32_t)strtol(tok, NULL, 10);
                }
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else {
            in_raw_data = false;
        }
    }
    fclose(f);
    subghz_sd_end(display_was_suspended);
    if (out_count) {
        *out_count = count;
    }
    if (out_frequency_hz) {
        *out_frequency_hz = freq;
    }
    if (out_preset) {
        *out_preset = preset;
    }
    return count > 0;
}

static bool subghz_parse_decoded_file(const char *path, subghz_decoded_signal_t *out_decoded, subghz_preset_t *out_preset) {
    if (!path || !out_decoded) {
        return false;
    }

    bool display_was_suspended = false;
    if (!subghz_sd_begin(&display_was_suspended)) {
        return false;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        subghz_sd_end(display_was_suspended);
        return false;
    }

    memset(out_decoded, 0, sizeof(*out_decoded));
    char protocol[SUBGHZ_DECODED_PROTO_MAX] = {0};
    int bits = 0;
    int frequency_hz = 0;
    int te = 0;
    uint64_t code = 0;
    int key_bytes = 0;
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    char preset_name[SUBGHZ_PRESET_NAME_MAX] = {0};
    char extra_buf[SUBGHZ_PRESERVED_EXTRA_MAX] = {0};
    size_t extra_len = 0;
    bool in_custom_preset = false;
    size_t custom_blob_len = 0;

    char line[384];
    while (fgets(line, sizeof(line), f)) {
        subghz_trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        if (in_custom_preset) {
            if (subghz_key_match(line, "Custom_preset_module:") ||
                subghz_key_match(line, "Custom_preset_data:")) {
                size_t line_len = strlen(line);
                if (custom_blob_len + line_len + 1 < SUBGHZ_CUSTOM_PRESET_BLOB_MAX) {
                    memcpy(out_decoded->custom_preset_blob + custom_blob_len, line, line_len);
                    custom_blob_len += line_len;
                    out_decoded->custom_preset_blob[custom_blob_len++] = '\n';
                }
            } else {
                in_custom_preset = false;
            }
        }

        if (subghz_key_match(line, "Frequency:")) {
            frequency_hz = atoi(subghz_skip_key_colon(line, 10));
        } else if (subghz_key_match(line, "Protocol:")) {
            const char *val = subghz_skip_key_colon(line, 9);
            snprintf(protocol, sizeof(protocol), "%.*s", (int)sizeof(protocol) - 1, val);
        } else if (subghz_key_match(line, "Preset:")) {
            const char *val = subghz_skip_key_colon(line, 7);
            preset = subghz_parse_preset(val);
            snprintf(preset_name, sizeof(preset_name), "%.*s", (int)sizeof(preset_name) - 1, val);
            if (preset == SUBGHZ_PRESET_CUSTOM) {
                in_custom_preset = true;
            }
        } else if (subghz_key_match(line, "Bit:")) {
            bits = atoi(subghz_skip_key_colon(line, 4));
        } else if (subghz_key_match(line, "TE:")) {
            te = atoi(subghz_skip_key_colon(line, 3));
        } else if (subghz_key_match(line, "Key:")) {
            const char *val = subghz_skip_key_colon(line, 4);
            char *saveptr = NULL;
            char *tok = strtok_r((char *)val, " ", &saveptr);
            while (tok) {
                unsigned long b = strtoul(tok, NULL, 16);
                code = (code << 8) | (uint64_t)(b & 0xFFUL);
                key_bytes++;
                tok = strtok_r(NULL, " ", &saveptr);
            }
        } else if (subghz_key_match(line, "Manufacture:") ||
                   subghz_key_match(line, "Repeat:") ||
                   subghz_key_match(line, "Serial:") ||
                   subghz_key_match(line, "Btn:") ||
                   subghz_key_match(line, "Cnt:") ||
                   subghz_key_match(line, "Seed:") ||
                   subghz_key_match(line, "CounterMode:") ||
                   subghz_key_match(line, "Key_Long:") ||
                   subghz_key_match(line, "Filetype:") ||
                   subghz_key_match(line, "Version:")) {
            /* recognized but not consumed into code — preserve for round-trip */
        } else if (!subghz_key_match(line, "RAW_Data:") &&
                   !subghz_key_match(line, "Custom_preset_module:") &&
                   !subghz_key_match(line, "Custom_preset_data:")) {
            size_t line_len = strlen(line);
            if (extra_len + line_len + 2 < SUBGHZ_PRESERVED_EXTRA_MAX) {
                memcpy(extra_buf + extra_len, line, line_len);
                extra_len += line_len;
                extra_buf[extra_len++] = '\n';
                extra_buf[extra_len] = '\0';
            }
        }
    }

    fclose(f);
    subghz_sd_end(display_was_suspended);

    if (protocol[0] == '\0' || strcmp(protocol, "RAW") == 0 || key_bytes <= 0) {
        return false;
    }

    out_decoded->decoded = true;
    out_decoded->code = code;
    out_decoded->bits = subghz_normalize_decoded_bits(protocol, (bits > 0) ? bits : (key_bytes * 8));
    out_decoded->frequency_hz = (frequency_hz > 0) ? frequency_hz : SUBGHZ_BASE_FREQ_HZ;
    out_decoded->te = (te > 0) ? te : (int)subghz_protocol_te(protocol);
    snprintf(out_decoded->protocol, sizeof(out_decoded->protocol), "%s", protocol);
    snprintf(out_decoded->preset_name, sizeof(out_decoded->preset_name), "%s", preset_name);
    if (extra_len > 0) {
        out_decoded->preserved_extra = (char *)malloc(extra_len + 1);
        if (out_decoded->preserved_extra) {
            memcpy(out_decoded->preserved_extra, extra_buf, extra_len + 1);
        }
    }
    if (out_decoded->bits > 32) {
        snprintf(out_decoded->info, sizeof(out_decoded->info), "%s %dbit\nCode:0x%016llX",
                 out_decoded->protocol, out_decoded->bits, (unsigned long long)out_decoded->code);
    } else {
        snprintf(out_decoded->info, sizeof(out_decoded->info), "%s %dbit\nCode:0x%08llX",
                 out_decoded->protocol, out_decoded->bits, (unsigned long long)out_decoded->code);
    }
    if (out_preset) {
        *out_preset = preset;
    }
    return true;
}

static size_t subghz_sanitize_pulses(int32_t *durations, size_t count) {
    if (!durations || count == 0) return 0;
    size_t write = 0;
    for (size_t i = 0; i < count; i++) {
        if (durations[i] == 0) continue;
        durations[write++] = durations[i];
    }
    return write;
}

static bool subghz_send_remote_replay(const int32_t *durations, size_t count, uint32_t freq_hz, subghz_preset_t preset) {
    if (!esp_comm_manager_is_connected() || !durations || count == 0) {
        return false;
    }

    uint8_t preset_byte = 0;
    if (preset == SUBGHZ_PRESET_OOK650_ASYNC) preset_byte = 1;
    else if (preset == SUBGHZ_PRESET_2FSK_DEV238_ASYNC) preset_byte = 2;
    else if (preset == SUBGHZ_PRESET_2FSK_DEV476_ASYNC) preset_byte = 3;
    else if (preset == SUBGHZ_PRESET_CUSTOM) preset_byte = 4;

    uint8_t start_pkt[13] = {
        SUBGHZ_STREAM_VERSION, 0x10,
        (uint8_t)(count & 0xFF), (uint8_t)((count >> 8) & 0xFF),
        (uint8_t)((count >> 16) & 0xFF), (uint8_t)((count >> 24) & 0xFF),
        (uint8_t)(freq_hz & 0xFF), (uint8_t)((freq_hz >> 8) & 0xFF),
        (uint8_t)((freq_hz >> 16) & 0xFF), (uint8_t)((freq_hz >> 24) & 0xFF),
        preset_byte, 0, 0
    };
    if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, start_pkt, sizeof(start_pkt))) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    size_t offset = 0;
    while (offset < count) {
        size_t chunk = count - offset;
        if (chunk > 13) {
            chunk = 13;
        }
        uint8_t pkt[5 + 13 * 4] = {0};
        pkt[0] = SUBGHZ_STREAM_VERSION;
        pkt[1] = 0x11;
        pkt[2] = (uint8_t)(offset & 0xFF);
        pkt[3] = (uint8_t)((offset >> 8) & 0xFF);
        pkt[4] = (uint8_t)chunk;
        for (size_t i = 0; i < chunk; i++) {
            int32_t v = durations[offset + i];
            size_t base = 5 + i * 4;
            pkt[base + 0] = (uint8_t)(v & 0xFF);
            pkt[base + 1] = (uint8_t)((v >> 8) & 0xFF);
            pkt[base + 2] = (uint8_t)((v >> 16) & 0xFF);
            pkt[base + 3] = (uint8_t)((v >> 24) & 0xFF);
        }
        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, pkt, 5 + chunk * 4)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        offset += chunk;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t end_pkt[2] = { SUBGHZ_STREAM_VERSION, 0x12 };
    return esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, end_pkt, sizeof(end_pkt));
}

static void subghz_graph_draw_event(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    if (!obj || obj != s_graph) {
        return;
    }

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) {
        return;
    }

    lv_area_t area;
    lv_obj_get_coords(obj, &area);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_opa = LV_OPA_COVER;
    bg.bg_color = lv_color_hex(theme_palette_get_surface(settings_get_menu_theme(&G_Settings)));
    bg.border_width = 0;
    bg.radius = 4;
    lv_draw_rect(draw_ctx, &bg, &area);

    lv_coord_t width = (area.x2 - area.x1 + 1);
    lv_coord_t height = (area.y2 - area.y1 + 1);
    if (width <= 4 || height <= 4) {
        return;
    }

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bar_color = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t peak_color = lv_color_hex(theme_palette_get_text_muted(theme));

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.border_width = 0;
    bar.radius = 1;
    bar.bg_color = bar_color;
    bar.bg_opa = LV_OPA_90;

    lv_draw_rect_dsc_t peak;
    lv_draw_rect_dsc_init(&peak);
    peak.border_width = 0;
    peak.radius = 0;
    peak.bg_color = peak_color;
    peak.bg_opa = LV_OPA_COVER;

    for (int ch = 0; ch < SUBGHZ_SCANNER_CHANNEL_COUNT; ch++) {
        lv_coord_t x1 = area.x1 + (ch * width) / SUBGHZ_SCANNER_CHANNEL_COUNT;
        lv_coord_t x2 = area.x1 + ((ch + 1) * width) / SUBGHZ_SCANNER_CHANNEL_COUNT - 1;
        if (x2 < x1) {
            x2 = x1;
        }

        int lvl = s_levels[ch];
        if (lvl < 0) lvl = 0;
        if (lvl > 100) lvl = 100;
        lv_coord_t bar_h = (lv_coord_t)((lvl * (height - 2)) / 100);
        if (bar_h < 1) {
            bar_h = 1;
        }

        lv_area_t bar_area = {
            .x1 = x1,
            .y1 = area.y2 - bar_h + 1,
            .x2 = x2,
            .y2 = area.y2,
        };
        lv_draw_rect(draw_ctx, &bar, &bar_area);

        int peak_lvl = s_peaks[ch];
        if (peak_lvl < 0) peak_lvl = 0;
        if (peak_lvl > 100) peak_lvl = 100;
        lv_coord_t peak_h = (lv_coord_t)((peak_lvl * (height - 2)) / 100);
        lv_coord_t peak_y = area.y2 - peak_h;
        if (peak_y < area.y1) {
            peak_y = area.y1;
        }

        lv_area_t peak_area = {
            .x1 = x1,
            .y1 = peak_y,
            .x2 = x2,
            .y2 = peak_y,
        };
        lv_draw_rect(draw_ctx, &peak, &peak_area);
    }

    lv_draw_line_dsc_t cursor;
    lv_draw_line_dsc_init(&cursor);
    cursor.color = lv_color_hex(theme_palette_get_text(theme));
    cursor.width = 1;
    lv_coord_t cursor_x = area.x1 + (s_cursor * width) / SUBGHZ_SCANNER_CHANNEL_COUNT;
    lv_point_t p1 = { .x = cursor_x, .y = area.y1 };
    lv_point_t p2 = { .x = cursor_x, .y = area.y2 };
    lv_draw_line(draw_ctx, &cursor, &p1, &p2);
}

static void subghz_refresh_status_labels(void) {
    if (!s_status_label || !lv_obj_is_valid(s_status_label)) {
        return;
    }

    if (s_remote_mode) {
        if (s_remote_error) {
            lv_label_set_text(s_status_label, "Peer error");
        } else if (!esp_comm_manager_is_connected()) {
            lv_label_set_text(s_status_label, "GhostLink disconnected");
        } else if (!s_remote_stream_online) {
            lv_label_set_text(s_status_label, "Waiting for peer stream...");
        } else {
            lv_label_set_text(s_status_label, s_remote_paused ? "Remote scan paused" : "Remote scan active");
        }
    } else if (!subghz_remote_manager_is_running()) {
        lv_label_set_text(s_status_label, "Scanner stopped");
    } else {
        lv_label_set_text(s_status_label,
                          subghz_remote_manager_is_paused() ? "Scanner paused" : "Scanner active");
    }

    if (s_freq_label && lv_obj_is_valid(s_freq_label)) {
        uint32_t base_hz;
        if (s_remote_mode) {
            base_hz = s_scan_freqs_hz[s_remote_freq_idx];
        } else {
            base_hz = subghz_remote_manager_get_frequency_hz();
        }
        uint32_t ch_hz = base_hz + (uint32_t)s_cursor * (uint32_t)CONFIG_SUBGHZ_CHANNEL_STEP_KHZ * 1000U;
        uint32_t mhz_int = ch_hz / 1000000U;
        uint32_t khz_frac = (ch_hz % 1000000U) / 1000U;
        char msg[64];
        snprintf(msg, sizeof(msg), "Ch %u  %lu.%03lu MHz", (unsigned)s_cursor,
                 (unsigned long)mhz_int, (unsigned long)khz_frac);
        lv_label_set_text(s_freq_label, msg);
    }
}

static void subghz_capture_snapshot_action(void) {
    if (s_remote_mode) {
        s_capture_was_running = s_remote_stream_online && !s_remote_paused;
        s_capture_was_running_known = true;
        subghz_capture_refresh_frequency_label();
        ESP_LOGI(TAG,
                 "remote capture request mode=%s freq=%s defer_popup=1",
                 (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
                 s_capture_freq_label);
        if (!subghz_capture_begin_remote(true)) {
            s_capture_was_running_known = false;
        }
        return;
    }

    subghz_open_capture_popup();
    if (s_capture_popup && lv_obj_is_valid(s_capture_popup)) {
        subghz_capture_continue_waiting();
    }
}

static void subghz_save_snapshot_action(void) {
    subghz_show_feedback_popup("SubGHz", "Use Capture and wait for a signal, then Save from the capture popup");
}

static void subghz_load_snapshot_action(void) {
    if (s_saved_file_count <= 0 || !s_saved_file_paths || !s_saved_file_paths[s_saved_index]) {
        subghz_show_feedback_popup("SubGHz", "No saved capture selected");
        return;
    }

    int32_t durations[SUBGHZ_RAW_MAX_DURATIONS];
    size_t count = 0;
    uint32_t file_freq_hz = 0;
    subghz_decoded_signal_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    bool have_raw = false;

    if (subghz_parse_raw_file(s_saved_file_paths[s_saved_index], durations, SUBGHZ_RAW_MAX_DURATIONS, &count, &file_freq_hz, &preset)) {
        if (file_freq_hz == 0) file_freq_hz = (uint32_t)SUBGHZ_BASE_FREQ_HZ;
        have_raw = true;
    } else if (subghz_parse_decoded_file(s_saved_file_paths[s_saved_index], &decoded, &preset)) {
        file_freq_hz = (uint32_t)decoded.frequency_hz;
        if (file_freq_hz == 0) file_freq_hz = (uint32_t)SUBGHZ_BASE_FREQ_HZ;
        if (!subghz_build_raw_from_decoded(decoded.protocol, decoded.code, decoded.bits,
                                           durations, SUBGHZ_RAW_MAX_DURATIONS, &count)) {
            if (decoded.preserved_extra) { free(decoded.preserved_extra); decoded.preserved_extra = NULL; }
            subghz_show_feedback_popup("SubGHz error", "Replay unsupported for this protocol");
            return;
        }
    } else {
        subghz_show_feedback_popup("SubGHz error", "Failed to parse snapshot file");
        return;
    }

    count = subghz_sanitize_pulses(durations, count);
    if (count == 0) {
        if (decoded.preserved_extra) { free(decoded.preserved_extra); decoded.preserved_extra = NULL; }
        subghz_show_feedback_popup("SubGHz error", "No valid pulses to replay");
        return;
    }

    if (s_remote_mode) {
        if (subghz_send_remote_replay(durations, count, file_freq_hz, preset)) {
            subghz_show_feedback_popup("SubGHz", "Replay streamed to remote radio");
        } else {
            subghz_show_feedback_popup("SubGHz error", "Failed to stream replay to remote");
        }
        if (decoded.preserved_extra) { free(decoded.preserved_extra); decoded.preserved_extra = NULL; }
        return;
    }

    if (subghz_remote_manager_transmit_raw(durations, count, file_freq_hz, preset)) {
        subghz_show_feedback_popup("SubGHz", "Replay transmitted");
    } else {
        subghz_show_feedback_popup("SubGHz error", subghz_remote_manager_get_last_error());
    }
    if (decoded.preserved_extra) { free(decoded.preserved_extra); decoded.preserved_extra = NULL; }
}

static void subghz_list_snapshots_action(void) {
    subghz_saved_list_reload();
}

static void subghz_close_popup(bool stop_scan) {
    if (stop_scan) {
        if (s_remote_mode) {
            if (esp_comm_manager_is_connected()) {
                (void)esp_comm_manager_send_command("subghz", "stop");
            }
        } else {
            subghz_remote_manager_stop();
        }
    }

    lvgl_obj_del_safe(&s_popup);
    s_status_label = NULL;
    s_freq_label = NULL;
    s_graph = NULL;
    s_start_btn = NULL;
    s_stop_btn = NULL;
    s_popup_back_btn = NULL;
    s_popup_selected = SUBGHZ_POPUP_START;
}

static void subghz_start_scan(void) {
    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            s_remote_error = true;
            subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
            subghz_refresh_status_labels();
            return;
        }
        if (esp_comm_manager_send_command("subghz", "start")) {
            s_remote_stream_online = false;
            s_remote_error = false;
            s_remote_paused = false;
        } else {
            s_remote_error = true;
        }
    } else {
        if (!subghz_remote_manager_start(false)) {
            ESP_LOGW(TAG, "Failed to start local scanner: %s", subghz_remote_manager_get_last_error());
            subghz_show_feedback_popup("SubGHz error", subghz_remote_manager_get_last_error());
        }
    }

    subghz_refresh_status_labels();
}

static void subghz_stop_scan(void) {
    if (s_remote_mode) {
        if (esp_comm_manager_is_connected()) {
            (void)esp_comm_manager_send_command("subghz", "stop");
        }
        s_remote_paused = true;
        s_remote_stream_online = false;
    } else {
        subghz_remote_manager_stop();
    }

    subghz_refresh_status_labels();
}

static void subghz_close_capture_popup(void) {
    if (!s_capture_popup || !lv_obj_is_valid(s_capture_popup)) {
        return;
    }

    if (s_remote_mode) {
        if (esp_comm_manager_is_connected()) {
            (void)esp_comm_manager_send_command("subghz", "capture_off");
        }
    } else {
        subghz_remote_manager_set_raw_capture_enabled(false);
    }

    if (!s_capture_was_running) {
        if (s_remote_mode) {
            if (esp_comm_manager_is_connected()) {
                (void)esp_comm_manager_send_command("subghz", "stop");
            }
        } else {
            subghz_remote_manager_stop();
        }
    } else if (s_capture_ready) {
        if (s_remote_mode) {
            if (esp_comm_manager_is_connected()) {
                (void)esp_comm_manager_send_command("subghz", "resume");
            }
        } else {
            subghz_remote_manager_set_paused(false);
        }
    }

    lvgl_obj_del_safe(&s_capture_popup);
    s_capture_status_label = NULL;
    s_capture_signal_label = NULL;
    s_capture_save_btn = NULL;
    s_capture_freq_btn = NULL;
    s_capture_back_btn = NULL;
    s_capture_waiting_signal = false;
    s_capture_ready = false;
    s_capture_signal_hits = 0;
    s_capture_was_running = false;
    s_capture_was_running_known = false;
    s_capture_remote_arm_pending = false;
    s_capture_defer_popup_open = false;
    s_capture_stop_pending = false;
    s_capture_stop_deadline_us = 0;
    s_capture_popup_opened_us = 0;
    s_capture_popup_open_pending = false;
    s_capture_popup_sync_pending = false;
    s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
}

static void subghz_capture_continue_waiting(void) {
    bool should_resume = s_capture_ready;
    if (!should_resume) {
        if (s_remote_mode) {
            should_resume = s_capture_was_running && s_remote_paused;
        } else {
            should_resume = s_capture_was_running && subghz_remote_manager_is_paused();
        }
    }

    subghz_capture_reset_state_for_new_attempt();

    if (s_remote_mode) {
        s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
        if (!subghz_capture_begin_remote(false)) {
            s_capture_waiting_signal = false;
            s_capture_ready = true;
            s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
            if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
                lv_label_set_text(s_capture_status_label, "Failed to arm remote capture");
            }
            subghz_capture_popup_update_buttons();
            return;
        }
        subghz_capture_apply_waiting_ui(true);
        return;
    } else {
        if (should_resume) {
            subghz_remote_manager_set_paused(false);
        } else if (!subghz_remote_manager_is_running()) {
            if (!subghz_remote_manager_start(false)) {
                s_capture_waiting_signal = false;
                s_capture_ready = true;
                s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
                if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
                    lv_label_set_text(s_capture_status_label, subghz_remote_manager_get_last_error());
                }
                subghz_capture_popup_update_buttons();
                return;
            }
        }
        subghz_remote_manager_set_raw_capture_enabled(true);
    }

    s_capture_waiting_signal = true;
    subghz_capture_apply_waiting_ui(false);
}

static uint8_t subghz_capture_get_frequency_index(void) {
    for (uint8_t i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
        if (strcmp(s_capture_freq_label, s_scan_freq_short[i]) == 0) {
            return i;
        }
    }

    if (s_remote_mode && s_remote_freq_idx < SUBGHZ_FA_BAND_COUNT) {
        return s_remote_freq_idx;
    }

    return 2;
}

static void subghz_capture_set_frequency_index(uint8_t idx) {
    if (idx >= SUBGHZ_FA_BAND_COUNT) {
        idx = 2;
    }
    s_remote_freq_idx = idx;
    snprintf(s_capture_freq_label, sizeof(s_capture_freq_label), "%s", s_scan_freq_short[idx]);
}

static bool subghz_capture_begin_remote(bool defer_popup_open) {
    if (!esp_comm_manager_is_connected()) {
        s_capture_remote_arm_pending = false;
        s_capture_defer_popup_open = false;
        subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
        return false;
    }

    uint8_t freq_idx = subghz_capture_get_frequency_index();
    uint32_t frequency_hz = s_scan_freqs_hz[freq_idx];
    char cmd[64];
    snprintf(cmd,
             sizeof(cmd),
             "capture_begin %s %lu",
             (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
             (unsigned long)frequency_hz);

    ESP_LOGI(TAG, "sending remote capture cmd='%s' defer_popup=%d", cmd, defer_popup_open ? 1 : 0);

    if (!esp_comm_manager_send_command("subghz", cmd)) {
        s_capture_remote_arm_pending = false;
        s_capture_defer_popup_open = false;
        s_remote_error = true;
        subghz_show_feedback_popup("SubGHz error", "Failed to send remote capture command");
        return false;
    }

    s_capture_remote_arm_pending = true;
    s_capture_defer_popup_open = defer_popup_open;
    s_remote_error = false;
    s_remote_paused = false;
    subghz_set_pending_action(SUBGHZ_PENDING_CAPTURE, 3000);
    subghz_show_action_status("Arming remote capture...");
    return true;
}

static void subghz_fa_apply_popup_selection(void) {
    popup_set_button_selected(s_fa_start_btn, s_fa_popup_selected == SUBGHZ_FA_POPUP_START);
    popup_set_button_selected(s_fa_stop_btn, s_fa_popup_selected == SUBGHZ_FA_POPUP_STOP);
    popup_set_button_selected(s_fa_back_btn, s_fa_popup_selected == SUBGHZ_FA_POPUP_BACK);
}

static uint8_t subghz_fa_freq_to_band_idx(uint32_t freq_hz) {
    for (int i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
        if (freq_hz == s_scan_freqs_hz[i]) return (uint8_t)i;
    }
    uint32_t min_diff = UINT32_MAX;
    uint8_t closest = 2;
    for (int i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
        uint32_t diff = (freq_hz > s_scan_freqs_hz[i]) ? (freq_hz - s_scan_freqs_hz[i]) : (s_scan_freqs_hz[i] - freq_hz);
        if (diff < min_diff) { min_diff = diff; closest = (uint8_t)i; }
    }
    return closest;
}

static void subghz_fa_add_to_history(uint32_t freq_hz) {
    if (freq_hz == 0) return;

    uint32_t rounded = (freq_hz / 10000) * 10000;

    for (int i = 0; i < s_fa_history_entries; i++) {
        uint32_t hist_rounded = (s_fa_history_freq[i] / 10000) * 10000;
        if (hist_rounded == rounded) {
            if (s_fa_history_count[i] < 255) s_fa_history_count[i]++;
            if (i > 0) {
                uint32_t tmp_freq = s_fa_history_freq[i];
                uint8_t tmp_cnt = s_fa_history_count[i];
                for (int j = i; j > 0; j--) {
                    s_fa_history_freq[j] = s_fa_history_freq[j - 1];
                    s_fa_history_count[j] = s_fa_history_count[j - 1];
                }
                s_fa_history_freq[0] = tmp_freq;
                s_fa_history_count[0] = tmp_cnt;
            }
            return;
        }
    }

    if (s_fa_history_entries < SUBGHZ_FA_HISTORY_MAX) {
        s_fa_history_entries++;
    }
    for (int i = s_fa_history_entries - 1; i > 0; i--) {
        s_fa_history_freq[i] = s_fa_history_freq[i - 1];
        s_fa_history_count[i] = s_fa_history_count[i - 1];
    }
    s_fa_history_freq[0] = rounded;
    s_fa_history_count[0] = 1;
}

static void subghz_fa_update_history_label(void) {
    if (!s_fa_history_label || !lv_obj_is_valid(s_fa_history_label)) return;

    char buf[128];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "History:");
    for (int i = 0; i < s_fa_history_entries && pos < sizeof(buf) - 20; i++) {
        uint32_t f = s_fa_history_freq[i];
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "\n%lu.%03lu MHz",
                        (unsigned long)(f / 1000000U),
                        (unsigned long)((f % 1000000U) / 1000U));
        if (s_fa_history_count[i] > 1) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " x%d", s_fa_history_count[i]);
        }
    }
    lv_label_set_text(s_fa_history_label, buf);
}

static void subghz_fa_graph_draw_event(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    if (!obj || obj != s_fa_graph) return;

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t bg_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t text_muted = lv_color_hex(theme_palette_get_text_muted(theme));

    lv_draw_rect_dsc_t rect_dsc;
    lv_draw_rect_dsc_init(&rect_dsc);
    rect_dsc.bg_opa = LV_OPA_COVER;
    rect_dsc.bg_color = bg;
    rect_dsc.border_width = 1;
    rect_dsc.border_color = bg_alt;
    rect_dsc.radius = 4;
    lv_draw_rect(draw_ctx, &rect_dsc, &coords);

    lv_coord_t pad_x = 6;
    lv_coord_t pad_y = 4;
    lv_coord_t x_axis_h = 14;
    lv_coord_t plot_x1 = coords.x1 + pad_x;
    lv_coord_t plot_x2 = coords.x2 - pad_x;
    lv_coord_t plot_y1 = coords.y1 + pad_y;
    lv_coord_t plot_y2 = coords.y2 - pad_y - x_axis_h;
    lv_coord_t plot_w = plot_x2 - plot_x1 + 1;
    lv_coord_t plot_h = plot_y2 - plot_y1 + 1;

    if (plot_w <= 0 || plot_h <= 0) return;

    rect_dsc.radius = 0;
    rect_dsc.border_width = 0;
    rect_dsc.bg_color = bg_alt;
    for (int g = 1; g <= 3; ++g) {
        lv_coord_t gy = plot_y2 - (plot_h * g) / 4;
        lv_area_t line_area = { .x1 = plot_x1, .y1 = gy, .x2 = plot_x2, .y2 = gy };
        lv_draw_rect(draw_ctx, &rect_dsc, &line_area);
    }

    int bar_gap = (plot_w > 200) ? 6 : (plot_w > 100) ? 4 : 2;
    int total_gap = bar_gap * (SUBGHZ_FA_BAND_COUNT - 1);
    int bar_w = (plot_w - total_gap) / SUBGHZ_FA_BAND_COUNT;
    if (bar_w < 4) { bar_w = 4; bar_gap = (plot_w - bar_w * SUBGHZ_FA_BAND_COUNT) / (SUBGHZ_FA_BAND_COUNT - 1); if (bar_gap < 0) bar_gap = 0; }

    static const char *band_labels[SUBGHZ_FA_BAND_COUNT] = { "315", "390", "433", "868", "915" };

    for (int b = 0; b < SUBGHZ_FA_BAND_COUNT; b++) {
        lv_coord_t bx1 = plot_x1 + b * (bar_w + bar_gap);
        lv_coord_t bx2 = bx1 + bar_w - 1;

        int lvl = s_fa_band_levels[b];
        if (lvl > 100) lvl = 100;
        if (lvl > 0) {
            lv_coord_t bar_h = (lv_coord_t)((lvl * plot_h) / 100);
            if (bar_h < 1) bar_h = 1;
            lv_area_t bar_area = { .x1 = bx1, .y1 = plot_y2 - bar_h + 1, .x2 = bx2, .y2 = plot_y2 };
            rect_dsc.bg_color = (b == s_fa_active_band) ? accent : text_muted;
            rect_dsc.bg_opa = LV_OPA_90;
            lv_draw_rect(draw_ctx, &rect_dsc, &bar_area);
        }

        if (s_fa_band_peaks[b] > 0) {
            int pk = s_fa_band_peaks[b];
            if (pk > 100) pk = 100;
            lv_coord_t peak_y = plot_y2 - (lv_coord_t)((pk * plot_h) / 100);
            if (peak_y < plot_y1) peak_y = plot_y1;
            lv_area_t peak_area = { .x1 = bx1, .y1 = peak_y, .x2 = bx2, .y2 = peak_y };
            rect_dsc.bg_color = text;
            rect_dsc.bg_opa = LV_OPA_COVER;
            lv_draw_rect(draw_ctx, &rect_dsc, &peak_area);
        }

        lv_draw_label_dsc_t lbl;
        lv_draw_label_dsc_init(&lbl);
        lbl.color = (b == s_fa_active_band) ? text : text_muted;
        lbl.font = accessibility_get_font_small();
        lv_area_t lbl_area = { .x1 = bx1 - 2, .y1 = plot_y2 + 2, .x2 = bx2 + 2, .y2 = coords.y2 - 1 };
        lv_draw_label(draw_ctx, &lbl, &lbl_area, band_labels[b], NULL);
    }
}

static void subghz_fa_start_scan(void) {
    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            s_remote_error = true;
            subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
            return;
        }
        if (esp_comm_manager_send_command("subghz", "start")) {
            s_remote_stream_online = false;
            s_remote_error = false;
            s_remote_paused = false;
        } else {
            s_remote_error = true;
        }
    } else {
        if (!subghz_remote_manager_start(false)) {
            subghz_show_feedback_popup("SubGHz error", subghz_remote_manager_get_last_error());
        }
    }
    s_fa_scanning = true;
    s_fa_signal_detected = false;
    s_fa_detect_hits = 0;
    s_fa_detected_freq_hz = 0;
}

static void subghz_fa_stop_scan(void) {
    if (s_remote_mode) {
        if (esp_comm_manager_is_connected()) {
            (void)esp_comm_manager_send_command("subghz", "stop");
        }
        s_remote_paused = true;
        s_remote_stream_online = false;
    } else {
        subghz_remote_manager_stop();
    }
    s_fa_scanning = false;
    s_fa_signal_detected = false;
    s_fa_detect_hits = 0;
}

static void subghz_close_freq_analyzer_popup(void) {
    if (s_fa_scanning) {
        subghz_fa_stop_scan();
    }

    lvgl_obj_del_safe(&s_fa_popup);
    s_fa_freq_label = NULL;
    s_fa_graph = NULL;
    s_fa_history_label = NULL;
    s_fa_start_btn = NULL;
    s_fa_stop_btn = NULL;
    s_fa_back_btn = NULL;
    s_fa_popup_selected = SUBGHZ_FA_POPUP_START;
    s_fa_scanning = false;
    s_fa_signal_detected = false;
    s_fa_detect_hits = 0;
    s_fa_detected_freq_hz = 0;
    memset(s_fa_band_levels, 0, sizeof(s_fa_band_levels));
    memset(s_fa_band_peaks, 0, sizeof(s_fa_band_peaks));
    s_fa_active_band = 2;
}

static void subghz_fa_start_btn_cb(lv_event_t *e) {
    (void)e;
    s_fa_popup_selected = SUBGHZ_FA_POPUP_START;
    subghz_fa_apply_popup_selection();
    subghz_fa_start_scan();
}

static void subghz_fa_stop_btn_cb(lv_event_t *e) {
    (void)e;
    s_fa_popup_selected = SUBGHZ_FA_POPUP_STOP;
    subghz_fa_apply_popup_selection();
    subghz_fa_stop_scan();
}

static void subghz_fa_back_btn_cb(lv_event_t *e) {
    (void)e;
    s_fa_popup_selected = SUBGHZ_FA_POPUP_BACK;
    subghz_fa_apply_popup_selection();
    subghz_close_freq_analyzer_popup();
}

static void subghz_open_freq_analyzer_popup(void) {
    if (s_fa_popup && lv_obj_is_valid(s_fa_popup)) {
        subghz_close_freq_analyzer_popup();
    }

    memset(s_fa_band_levels, 0, sizeof(s_fa_band_levels));
    memset(s_fa_band_peaks, 0, sizeof(s_fa_band_peaks));
    s_fa_active_band = 2;
    s_fa_scanning = false;
    s_fa_signal_detected = false;
    s_fa_detect_hits = 0;
    s_fa_detected_freq_hz = 0;
    s_fa_popup_selected = SUBGHZ_FA_POPUP_START;

    bool tiny = (LV_HOR_RES <= 128 || LV_VER_RES <= 80);
    bool small = (LV_VER_RES <= 170 && !tiny);

    lv_coord_t popup_w = tiny ? LV_HOR_RES : (LV_HOR_RES <= 240) ? (LV_HOR_RES - 10) : (LV_HOR_RES - 20);
    lv_coord_t popup_h = tiny ? LV_VER_RES : (LV_VER_RES <= 200) ? (LV_VER_RES - 10) : (LV_VER_RES - 20);
    if (!tiny && popup_w < 220) popup_w = 220;
    if (!tiny && popup_h < 180) popup_h = 180;

    s_fa_popup = popup_create_container(lv_scr_act(), popup_w, popup_h, true);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));

    const lv_font_t *title_font = tiny ? accessibility_get_font_small() : accessibility_get_font_body();
    popup_create_title_label(s_fa_popup, "Freq Analyzer", title_font, tiny ? 2 : 6);

    const lv_font_t *freq_font = tiny ? accessibility_get_font_small() : accessibility_get_font_title();
    s_fa_freq_label = lv_label_create(s_fa_popup);
    lv_label_set_text(s_fa_freq_label, "---.--- MHz");
    lv_obj_set_style_text_color(s_fa_freq_label, text, 0);
    lv_obj_set_style_text_font(s_fa_freq_label, freq_font, 0);
    lv_label_set_long_mode(s_fa_freq_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_fa_freq_label, LV_ALIGN_TOP_MID, 0, tiny ? 14 : 26);

    int graph_top = tiny ? 26 : (small ? 36 : 44);
    int btn_h = tiny ? 16 : (small ? 24 : 28);
    int btn_margin = tiny ? -2 : -6;
    bool show_history = !tiny;
    int history_h = show_history ? (small ? 32 : 48) : 0;
    int graph_bottom = btn_h + history_h + (tiny ? 4 : 16);
    int graph_h = popup_h - graph_top - graph_bottom;
    if (graph_h < (tiny ? 20 : 50)) graph_h = tiny ? 20 : 50;

    lv_coord_t inner_pad = tiny ? 2 : 6;
    lv_coord_t inner_w = popup_w - inner_pad * 2 - 4;  // 4 = border 2*2
    if (inner_w < 10) inner_w = 10;

    s_fa_graph = lv_obj_create(s_fa_popup);
    lv_obj_set_pos(s_fa_graph, inner_pad, graph_top);
    lv_obj_set_size(s_fa_graph, inner_w, graph_h);
    lv_obj_set_style_bg_opa(s_fa_graph, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_fa_graph, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_fa_graph, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_fa_graph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_fa_graph, subghz_fa_graph_draw_event, LV_EVENT_DRAW_MAIN, NULL);

    if (show_history) {
        s_fa_history_label = lv_label_create(s_fa_popup);
        lv_label_set_text(s_fa_history_label, "History:");
        lv_obj_set_style_text_color(s_fa_history_label, text, 0);
        lv_obj_set_style_text_font(s_fa_history_label, small ? accessibility_get_font_small() : accessibility_get_font_small(), 0);
        lv_obj_align(s_fa_history_label, LV_ALIGN_TOP_LEFT, 8, graph_top + graph_h + 4);
        lv_obj_set_style_max_height(s_fa_history_label, history_h, 0);
    }

    const lv_font_t *btn_font = tiny ? accessibility_get_font_small() : (small ? accessibility_get_font_small() : accessibility_get_font_body());
    int btn_w = tiny ? 40 : (small ? 58 : 68);
    s_fa_start_btn = popup_add_styled_button(
        s_fa_popup, "Start", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, tiny ? 2 : 8, btn_margin, btn_font, subghz_fa_start_btn_cb, NULL);
    s_fa_stop_btn = popup_add_styled_button(
        s_fa_popup, "Stop", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, btn_margin, btn_font, subghz_fa_stop_btn_cb, NULL);
    s_fa_back_btn = popup_add_styled_button(
        s_fa_popup, "Back", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, tiny ? -2 : -8, btn_margin, btn_font, subghz_fa_back_btn_cb, NULL);

    lv_obj_t *fa_btns[3] = { s_fa_start_btn, s_fa_stop_btn, s_fa_back_btn };
    popup_layout_buttons_responsive(s_fa_popup, fa_btns, 3, btn_margin, NULL);

    subghz_fa_apply_popup_selection();
}

static void subghz_freq_analyzer_row_cb(lv_event_t *e) {
    (void)e;
    if (s_ov) {
        options_view_set_selected(s_ov, 3);
    }
    subghz_open_freq_analyzer_popup();
}

static void subghz_wf_init_palette(void) {
    static bool initialized = false;
    if (initialized) return;
    for (int i = 0; i < 256; i++) {
        uint8_t r, g, b;
        if (i == 0) {
            r = 0; g = 0; b = 0;
        } else if (i < 12) {
            r = 0; g = 0; b = 0;
        } else if (i < 56) {
            int t = i - 12;
            r = 0;
            g = 0;
            b = (uint8_t)(12 + (t * 68) / 43);
        } else if (i < 104) {
            int t = i - 56;
            r = (uint8_t)(8 + (t * 92) / 47);
            g = 0;
            b = (uint8_t)(80 + (t * 120) / 47);
        } else if (i < 152) {
            int t = i - 104;
            r = (uint8_t)(100 + (t * 155) / 47);
            g = 0;
            b = (uint8_t)(200 - (t * 40) / 47);
        } else if (i < 208) {
            int t = i - 152;
            r = 255;
            g = (uint8_t)((t * 128) / 55);
            b = (uint8_t)(160 - (t * 160) / 55);
        } else {
            int t = i - 208;
            r = 255;
            g = (uint8_t)(128 + (t * 127) / 47);
            b = (uint8_t)((t * 255) / 47);
        }
        s_wf_palette[i] = lv_color_make(r, g, b);
    }
    initialized = true;
}

static void subghz_wf_store_band_line(uint8_t freq_idx, const uint8_t *levels, uint8_t count) {
    if (!levels || count < 2 || freq_idx >= SUBGHZ_FA_BAND_COUNT) {
        return;
    }
    if (count > SUBGHZ_SCANNER_CHANNEL_COUNT) {
        count = SUBGHZ_SCANNER_CHANNEL_COUNT;
    }

    uint8_t resampled[SUBGHZ_SCANNER_CHANNEL_COUNT];
    uint8_t raw_peak = 0;
    for (uint16_t x = 0; x < SUBGHZ_SCANNER_CHANNEL_COUNT; x++) {
        uint16_t src = (uint16_t)((x * ((uint16_t)count - 1U) * 256U) / (SUBGHZ_SCANNER_CHANNEL_COUNT - 1U));
        uint8_t ch = (uint8_t)(src >> 8);
        uint8_t frac = (uint8_t)(src & 0xFF);
        if (ch >= count - 1U) {
            ch = (uint8_t)(count - 2U);
            frac = 255;
        }
        int level = ((int)levels[ch] * (256 - frac) + (int)levels[ch + 1] * frac) >> 8;
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        resampled[x] = (uint8_t)level;
        if ((uint8_t)level > raw_peak) raw_peak = (uint8_t)level;
    }
    s_wf_band_raw_peak[freq_idx] = raw_peak;

    portENTER_CRITICAL(&s_wf_composite_lock);
    memcpy(s_wf_band_lines[freq_idx], resampled, sizeof(resampled));
    s_wf_band_mask |= (uint8_t)(1U << freq_idx);

    if (s_wf_band_mask == WF_BAND_MASK_ALL) {
        uint16_t dst = 0;
        uint8_t peak_band = 0;
        uint8_t peak_level = 0;
        for (uint8_t i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
            uint8_t band = s_wf_display_band_order[i];
            memcpy(&s_wf_pending_composite[dst], s_wf_band_lines[band], SUBGHZ_SCANNER_CHANNEL_COUNT);
            if (s_wf_band_raw_peak[band] >= peak_level) {
                peak_level = s_wf_band_raw_peak[band];
                peak_band = band;
            }
            dst = (uint16_t)(dst + SUBGHZ_SCANNER_CHANNEL_COUNT);
        }
        s_wf_pending_composite_count = dst;
        s_wf_pending_composite_seq++;
        s_wf_last_peak_band = peak_band;
        s_wf_last_peak_ch = 0;
        s_wf_last_peak_level = peak_level;
        s_wf_band_mask = 0;
    }
    portEXIT_CRITICAL(&s_wf_composite_lock);
}

static bool subghz_wf_take_composite(uint8_t *out_levels, uint16_t max_levels, uint16_t *out_count) {
    if (!out_levels || max_levels == 0 || !out_count) {
        return false;
    }

    bool ready = false;
    portENTER_CRITICAL(&s_wf_composite_lock);
    if (s_wf_pending_composite_seq != s_wf_rendered_composite_seq && s_wf_pending_composite_count > 0) {
        uint16_t copy_len = s_wf_pending_composite_count;
        if (copy_len > max_levels) {
            copy_len = max_levels;
        }
        memcpy(out_levels, s_wf_pending_composite, copy_len);
        *out_count = copy_len;
        s_wf_rendered_composite_seq = s_wf_pending_composite_seq;
        ready = true;
    }
    portEXIT_CRITICAL(&s_wf_composite_lock);
    return ready;
}

static void subghz_wf_reset_composite(void) {
    portENTER_CRITICAL(&s_wf_composite_lock);
    memset(s_wf_band_lines, 0, sizeof(s_wf_band_lines));
    memset(s_wf_band_raw_peak, 0, sizeof(s_wf_band_raw_peak));
    memset(s_wf_pending_composite, 0, sizeof(s_wf_pending_composite));
    s_wf_band_mask = 0;
    s_wf_pending_composite_count = 0;
    s_wf_pending_composite_seq = 0;
    s_wf_rendered_composite_seq = 0;
    s_wf_last_peak_band = 0;
    s_wf_last_peak_ch = 0;
    s_wf_last_peak_level = 0;
    portEXIT_CRITICAL(&s_wf_composite_lock);
}

static bool subghz_wf_render_line(const uint8_t *levels, uint16_t count) {
    if (!s_wf_fb || !s_wf_line || s_wf_fb_w <= 0 || s_wf_fb_h <= 0) return false;
    if (!levels || count < 2) return false;

    int scroll_rows = 1;
    if (scroll_rows >= s_wf_fb_h) scroll_rows = s_wf_fb_h - 1;
    if (scroll_rows > 0) {
        memmove(&s_wf_fb[scroll_rows * s_wf_fb_w], &s_wf_fb[0], (s_wf_fb_h - scroll_rows) * s_wf_fb_w * sizeof(lv_color_t));
    }
    bool had_prev = s_wf_have_prev;

    lv_coord_t half_w = s_wf_fb_w / 2;
    for (lv_coord_t x = 0; x < s_wf_fb_w; x++) {
        int src;
        if (s_wf_center_zero) {
            int dist = (int)(x <= half_w ? half_w - x : x - half_w);
            int max_dist = (int)(s_wf_fb_w <= 1 ? 1 : half_w);
            src = (dist * ((int)count - 1) * 256) / max_dist;
        } else {
            src = (x * ((int)count - 1) * 256) / (s_wf_fb_w - 1);
        }
        int ch = src >> 8;
        int frac = src & 0xFF;
        if (ch >= (int)count - 1) {
            ch = (int)count - 2;
            frac = 255;
        }

        int level = ((int)levels[ch] * (256 - frac) + (int)levels[ch + 1] * frac) >> 8;
        if (had_prev) {
            level = ((int)s_wf_line[x] * 7 + level) >> 3;
        }
        s_wf_line[x] = (uint8_t)level;

        int adj = (level - 25) * 2;
        if (adj < 0) adj = 0;
        if (adj > 100) adj = 100;
        int intensity = (adj * 255) / 100;
        if (intensity < 0) intensity = 0;
        if (intensity > 255) intensity = 255;
        lv_color_t color = s_wf_palette[intensity];

        lv_color_t prev_color = s_wf_prev_row ? s_wf_prev_row[x] : lv_color_black();
        if (color.full != prev_color.full || !had_prev) {
            for (int r = 0; r < scroll_rows; r++) {
                s_wf_fb[r * s_wf_fb_w + x] = color;
            }
        }
        if (s_wf_prev_row) {
            s_wf_prev_row[x] = color;
        }
    }

    s_wf_have_prev = true;
    return true;
}

static bool subghz_wf_update_frame(void) {
    uint8_t line[SUBGHZ_SCANNER_CHANNEL_COUNT];
    uint8_t composite[WF_COMPOSITE_CHANNELS];
    uint8_t count = 0;
    uint16_t composite_count = 0;
    uint8_t freq_idx = 0;

    if (s_remote_mode) {
        if (!subghz_wf_take_composite(composite, sizeof(composite), &composite_count)) {
            return false;
        }
    } else if (!subghz_remote_manager_take_waterfall_line(line, sizeof(line), &count, &freq_idx, NULL)) {
        return false;
    } else {
        subghz_wf_store_band_line(freq_idx, line, count);
        if (!subghz_wf_take_composite(composite, sizeof(composite), &composite_count)) {
            return false;
        }
    }

    if (s_wf_status_label && lv_obj_is_valid(s_wf_status_label)) {
        lv_label_set_text_fmt(s_wf_status_label, "Peak: %s ch %u Lv %u",
                              s_scan_freq_labels[s_wf_last_peak_band],
                              (unsigned)s_wf_last_peak_ch,
                              (unsigned)s_wf_last_peak_level);
    }

    return subghz_wf_render_line(composite, composite_count);
}

static void subghz_wf_set_freq_label(uint8_t freq_idx) {
    (void)freq_idx;
    if (s_wf_status_label && lv_obj_is_valid(s_wf_status_label)) {
        lv_label_set_text(s_wf_status_label, "Peak: ---.-- MHz ch -- Lv --");
    }
}

static void subghz_wf_apply_popup_selection(void) {
    popup_set_button_selected(s_wf_start_btn, s_wf_popup_selected == SUBGHZ_WF_POPUP_START);
    popup_set_button_selected(s_wf_stop_btn, s_wf_popup_selected == SUBGHZ_WF_POPUP_STOP);
    popup_set_button_selected(s_wf_back_btn, s_wf_popup_selected == SUBGHZ_WF_POPUP_BACK);
}

static void subghz_wf_start_scan(void) {
    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            s_remote_error = true;
            subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
            subghz_wf_set_freq_label(s_remote_freq_idx);
            return;
        }
        if (esp_comm_manager_send_command("subghz", "waterfall_start")) {
            s_remote_stream_online = false;
            s_remote_error = false;
            s_remote_paused = false;
        } else {
            s_remote_error = true;
        }
    } else {
        if (!subghz_remote_manager_start_waterfall(false)) {
            ESP_LOGW(TAG, "Failed to start local waterfall scanner: %s", subghz_remote_manager_get_last_error());
            subghz_show_feedback_popup("SubGHz error", subghz_remote_manager_get_last_error());
        }
    }
    s_wf_scanning = true;
    s_wf_have_prev = false;
#ifdef CONFIG_HAS_MIC
    mic_pause();
#endif
    s_wf_remote_ready = false;
    s_wf_remote_count = 0;
    s_wf_remote_seq = 0;
    s_wf_remote_build_seq = 0;
    s_wf_remote_build_count = 0;
    s_wf_remote_build_received = 0;
    s_wf_remote_build_mask = 0;
    s_wf_rendered_remote_seq = 0;
    s_wf_remote_have_cursor = false;
    subghz_wf_reset_composite();
    subghz_wf_set_freq_label(0);
    if (s_wf_fb && s_wf_line && s_wf_fb_w > 0 && s_wf_fb_h > 0) {
        lv_color_t black = lv_color_black();
        for (int i = 0; i < s_wf_fb_w * s_wf_fb_h; i++) {
            s_wf_fb[i] = black;
        }
        memset(s_wf_line, 0, (size_t)s_wf_fb_w * sizeof(uint8_t));
        if (s_wf_prev_row) {
            for (int i = 0; i < s_wf_fb_w; i++) {
                s_wf_prev_row[i] = black;
            }
        }
        if (s_wf_canvas && lv_obj_is_valid(s_wf_canvas)) {
            lv_obj_invalidate(s_wf_canvas);
        }
    }
}

static void subghz_wf_stop_scan(void) {
    if (s_remote_mode) {
        if (esp_comm_manager_is_connected()) {
            (void)esp_comm_manager_send_command("subghz", "waterfall_stop");
        }
        s_remote_paused = true;
        s_remote_stream_online = false;
    } else {
        subghz_remote_manager_stop();
    }
    s_wf_scanning = false;
#ifdef CONFIG_HAS_MIC
    mic_resume();
#endif
}

static void subghz_open_waterfall_popup(void) {
    if (s_wf_popup && lv_obj_is_valid(s_wf_popup)) {
        subghz_close_waterfall_popup();
    }

    subghz_wf_init_palette();
    s_wf_scanning = false;
    s_wf_popup_selected = SUBGHZ_WF_POPUP_START;

    bool tiny = (LV_HOR_RES <= 128 || LV_VER_RES <= 80);
    bool small = (LV_VER_RES <= 170 && !tiny);

    lv_coord_t popup_w = tiny ? LV_HOR_RES : (LV_HOR_RES <= 240) ? (LV_HOR_RES - 10) : (LV_HOR_RES - 20);
    lv_coord_t popup_h = tiny ? LV_VER_RES : (LV_VER_RES <= 200) ? (LV_VER_RES - 10) : (LV_VER_RES - 20);
    if (!tiny && popup_w < 220) popup_w = 220;
    if (!tiny && popup_h < 180) popup_h = 180;

    s_wf_popup = popup_create_container(lv_scr_act(), popup_w, popup_h, true);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));

    const lv_font_t *title_font = tiny ? &lv_font_montserrat_10 : &lv_font_montserrat_14;
    popup_create_title_label(s_wf_popup, "Waterfall", title_font, tiny ? 2 : 6);

    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    const lv_font_t *status_font = tiny ? &lv_font_montserrat_8 : &lv_font_montserrat_10;
    s_wf_status_label = lv_label_create(s_wf_popup);
    lv_label_set_text(s_wf_status_label, "Peak: ---.-- MHz ch -- Lv --");
    lv_obj_set_style_text_color(s_wf_status_label, text, 0);
    lv_obj_set_style_text_font(s_wf_status_label, status_font, 0);
    lv_label_set_long_mode(s_wf_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_wf_status_label, LV_ALIGN_TOP_MID, 0, tiny ? 14 : (small ? 26 : 28));

    int graph_top = tiny ? 40 : (small ? 58 : 66);
    int btn_h = tiny ? 16 : (small ? 22 : 26);
    int btn_margin = tiny ? -2 : -8;
    int graph_bottom = btn_h + (tiny ? 14 : (small ? 28 : 34));
    int graph_h = popup_h - graph_top - graph_bottom;
    int min_graph_h = tiny ? 18 : 40;
    if (graph_h < min_graph_h) graph_h = min_graph_h;
    int max_graph_h = popup_h - graph_top - btn_h - (tiny ? 8 : 18);
    if (graph_h > max_graph_h) graph_h = max_graph_h;
    if (graph_h < 10) graph_h = 10;

    lv_coord_t inner_pad = tiny ? 2 : 8;
    lv_coord_t inner_w = popup_w - inner_pad * 2;
    if (inner_w < 10) inner_w = 10;

    s_wf_graph = lv_obj_create(s_wf_popup);
    lv_obj_set_size(s_wf_graph, inner_w, graph_h);
    lv_obj_align(s_wf_graph, LV_ALIGN_TOP_MID, 0, graph_top);
    lv_obj_set_style_bg_color(s_wf_graph, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(s_wf_graph, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wf_graph, 0, 0);
    lv_obj_set_style_pad_all(s_wf_graph, 0, 0);
    lv_obj_set_style_radius(s_wf_graph, 4, 0);
    lv_obj_set_style_clip_corner(s_wf_graph, true, 0);
    lv_obj_clear_flag(s_wf_graph, LV_OBJ_FLAG_SCROLLABLE);

    const char *legend_text[SUBGHZ_FA_BAND_COUNT] = { "315", "390", "433", "868", "915" };
    for (int i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
        lv_coord_t x1 = (lv_coord_t)WF_ROUND_DIV(i * inner_w, SUBGHZ_FA_BAND_COUNT);
        lv_coord_t x2 = (lv_coord_t)WF_ROUND_DIV((i + 1) * inner_w, SUBGHZ_FA_BAND_COUNT);
        lv_coord_t legend_w = x2 - x1;
        if (legend_w < 1) legend_w = 1;
        s_wf_legend_labels[i] = lv_label_create(s_wf_popup);
        lv_label_set_text(s_wf_legend_labels[i], legend_text[i]);
        lv_obj_set_style_text_color(s_wf_legend_labels[i], accent, 0);
        lv_obj_set_style_text_font(s_wf_legend_labels[i], status_font, 0);
        lv_label_set_long_mode(s_wf_legend_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_wf_legend_labels[i], legend_w);
        lv_obj_set_style_text_align(s_wf_legend_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align_to(s_wf_legend_labels[i], s_wf_graph, LV_ALIGN_OUT_TOP_LEFT,
                        x1, -(tiny ? 2 : 5));
    }

    s_wf_canvas = lv_canvas_create(s_wf_graph);
    s_wf_fb_w = inner_w;
    s_wf_fb_h = graph_h;
    free(s_wf_fb);
    free(s_wf_line);
    free(s_wf_prev_row);
    s_wf_fb = heap_caps_calloc((size_t)s_wf_fb_w * (size_t)s_wf_fb_h, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wf_line = heap_caps_calloc((size_t)s_wf_fb_w, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_wf_prev_row = heap_caps_calloc((size_t)s_wf_fb_w, sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wf_fb) s_wf_fb = calloc((size_t)s_wf_fb_w * (size_t)s_wf_fb_h, sizeof(lv_color_t));
    if (!s_wf_line) s_wf_line = calloc((size_t)s_wf_fb_w, sizeof(uint8_t));
    if (!s_wf_prev_row) s_wf_prev_row = calloc((size_t)s_wf_fb_w, sizeof(lv_color_t));
    if (s_wf_fb && s_wf_line) {
        lv_color_t black = lv_color_black();
        for (int i = 0; i < s_wf_fb_w * s_wf_fb_h; i++) {
            s_wf_fb[i] = black;
        }
        lv_canvas_set_buffer(s_wf_canvas, s_wf_fb, s_wf_fb_w, s_wf_fb_h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_wf_canvas, s_wf_fb_w, s_wf_fb_h);
        lv_img_set_antialias(s_wf_canvas, false);
    } else {
        free(s_wf_fb);
        free(s_wf_line);
        free(s_wf_prev_row);
        s_wf_fb = NULL;
        s_wf_line = NULL;
        s_wf_prev_row = NULL;
        s_wf_fb_w = 0;
        s_wf_fb_h = 0;
        lv_obj_add_flag(s_wf_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(s_wf_canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_wf_canvas, LV_OBJ_FLAG_SCROLLABLE);

    const lv_font_t *btn_font = tiny ? &lv_font_montserrat_8 : (small ? &lv_font_montserrat_10 : &lv_font_montserrat_12);
    int btn_w = tiny ? 36 : (small ? 52 : 60);
    s_wf_start_btn = popup_add_styled_button(
        s_wf_popup, "Start", btn_w, btn_h, LV_ALIGN_BOTTOM_LEFT, tiny ? 2 : 8, btn_margin, btn_font, subghz_wf_start_btn_cb, NULL);
    s_wf_stop_btn = popup_add_styled_button(
        s_wf_popup, "Stop", btn_w, btn_h, LV_ALIGN_BOTTOM_MID, 0, btn_margin, btn_font, subghz_wf_stop_btn_cb, NULL);
    s_wf_back_btn = popup_add_styled_button(
        s_wf_popup, "Back", btn_w, btn_h, LV_ALIGN_BOTTOM_RIGHT, tiny ? -2 : -8, btn_margin, btn_font, subghz_wf_back_btn_cb, NULL);

    lv_obj_t *wf_btns[3] = { s_wf_start_btn, s_wf_stop_btn, s_wf_back_btn };
    popup_layout_buttons_responsive(s_wf_popup, wf_btns, 3, btn_margin, NULL);

    subghz_wf_apply_popup_selection();
    subghz_wf_start_scan();
}

static void subghz_close_waterfall_popup(void) {
    if (s_wf_scanning) {
        subghz_wf_stop_scan();
    }
    lvgl_obj_del_safe(&s_wf_popup);
    s_wf_status_label = NULL;
    memset(s_wf_legend_labels, 0, sizeof(s_wf_legend_labels));
    s_wf_graph = NULL;
    s_wf_start_btn = NULL;
    s_wf_stop_btn = NULL;
    s_wf_back_btn = NULL;
    s_wf_canvas = NULL;
    s_wf_popup_selected = SUBGHZ_WF_POPUP_START;
    s_wf_scanning = false;
    s_wf_have_prev = false;
    s_wf_remote_ready = false;
    s_wf_remote_count = 0;
    s_wf_remote_seq = 0;
    s_wf_remote_build_seq = 0;
    s_wf_remote_build_count = 0;
    s_wf_remote_build_received = 0;
    s_wf_remote_build_mask = 0;
    s_wf_rendered_remote_seq = 0;
    subghz_wf_reset_composite();
    free(s_wf_fb);
    free(s_wf_line);
    free(s_wf_prev_row);
    s_wf_fb = NULL;
    s_wf_line = NULL;
    s_wf_prev_row = NULL;
    s_wf_fb_w = 0;
    s_wf_fb_h = 0;
}

static void subghz_wf_start_btn_cb(lv_event_t *e) {
    (void)e;
    s_wf_popup_selected = SUBGHZ_WF_POPUP_START;
    subghz_wf_apply_popup_selection();
    subghz_wf_start_scan();
}

static void subghz_wf_stop_btn_cb(lv_event_t *e) {
    (void)e;
    s_wf_popup_selected = SUBGHZ_WF_POPUP_STOP;
    subghz_wf_apply_popup_selection();
    subghz_wf_stop_scan();
}

static void subghz_wf_back_btn_cb(lv_event_t *e) {
    (void)e;
    s_wf_popup_selected = SUBGHZ_WF_POPUP_BACK;
    subghz_wf_apply_popup_selection();
    subghz_close_waterfall_popup();
}

static void subghz_waterfall_row_cb(lv_event_t *e) {
    (void)e;
    if (s_ov) {
        options_view_set_selected(s_ov, 4);
    }
    subghz_open_waterfall_popup();
}

static void subghz_capture_primary_btn_cb(lv_event_t *e) {
    ESP_LOGI(TAG,
             "capture primary pressed src=%s ready=%d mode=%s stop_pending=%d raw_count=%lu",
             e ? "lvgl" : "internal",
             s_capture_ready ? 1 : 0,
             (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
             s_capture_stop_pending ? 1 : 0,
             (unsigned long)s_capture_raw_count);
    if (subghz_capture_popup_ignore_activation()) {
        return;
    }
    if (s_capture_ready) {
        if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
            s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
            subghz_capture_popup_update_buttons();

            if (!s_capture_buffer_valid) {
                subghz_capture_continue_waiting();
                return;
            }

            char saved_name[SUBGHZ_SNAPSHOT_NAME_MAX];
            char save_err[96];
            const char *fmt_msg = "Raw capture saved in Flipper RAW format";
            if (subghz_local_save_snapshot(
                    s_capture_name,
                    s_capture_raw,
                    s_capture_raw_count,
                    NULL,
                    saved_name,
                    sizeof(saved_name),
                    save_err,
                    sizeof(save_err))) {
                snprintf(s_capture_name, sizeof(s_capture_name), "%s", saved_name);
                subghz_show_feedback_popup("SubGHz", fmt_msg);
            } else {
                subghz_show_feedback_popup("SubGHz error", save_err);
            }
            return;
        }

        s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
        subghz_capture_popup_update_buttons();

        if (!s_capture_buffer_valid) {
            subghz_show_feedback_popup("SubGHz error", "No capture buffered");
            return;
        }

        char saved_name[SUBGHZ_SNAPSHOT_NAME_MAX];
        char save_err[96];
        const char *fmt_msg = "Capture saved in Flipper RAW format";
        if (subghz_local_save_snapshot(
                s_capture_name,
                s_capture_raw,
                s_capture_raw_count,
                s_remote_decoded.decoded ? &s_remote_decoded : NULL,
                saved_name,
                sizeof(saved_name),
                save_err,
                sizeof(save_err))) {
            snprintf(s_capture_name, sizeof(s_capture_name), "%s", saved_name);
            if (s_remote_decoded.decoded) fmt_msg = "Decoded signal saved";
            subghz_show_feedback_popup("SubGHz", fmt_msg);
        } else {
            subghz_show_feedback_popup("SubGHz error", save_err);
        }
    } else {
        if (s_remote_mode) {
            if (esp_comm_manager_is_connected()) {
                ESP_LOGI(TAG,
                         "remote capture stop requested mode=%s raw_count=%lu",
                         (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
                         (unsigned long)s_capture_raw_count);
                (void)esp_comm_manager_send_command("subghz", "capture_off");
                (void)esp_comm_manager_send_command("subghz", "pause");
            }
            if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
                s_capture_stop_pending = true;
                s_capture_stop_deadline_us = esp_timer_get_time() + 1500000LL;
                ESP_LOGI(TAG, "waiting for remote raw stream stop_deadline=%lld", (long long)s_capture_stop_deadline_us);
                if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
                    lv_label_set_text(s_capture_status_label, "Waiting for raw capture...");
                }
                if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label)) {
                    lv_label_set_text(s_capture_signal_label, "Finishing remote raw capture");
                }
                subghz_capture_popup_update_buttons();
                return;
            }
        } else {
            subghz_remote_manager_set_raw_capture_enabled(false);
            subghz_remote_manager_set_paused(true);
        }

        size_t raw_count = 0;
        bool got = false;
        if (s_remote_mode) {
            got = (s_capture_raw_count > 0);
            raw_count = s_capture_raw_count;
        } else {
            got = subghz_remote_manager_take_raw_capture(s_capture_raw, SUBGHZ_RAW_MAX_DURATIONS, &raw_count);
        }

        if (got && raw_count > 0) {
            s_capture_raw_count = raw_count;
            s_capture_buffer_valid = true;
            subghz_capture_mark_ready();
        } else {
            s_capture_ready = true;
            s_capture_buffer_valid = false;
            s_capture_waiting_signal = false;
            s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
            if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
                lv_label_set_text(s_capture_status_label, "No signal captured");
            }
            subghz_capture_popup_update_buttons();
        }
    }
}

static void subghz_capture_freq_btn_cb(lv_event_t *e) {
    ESP_LOGI(TAG,
             "capture freq pressed src=%s ready=%d mode=%s label=%s",
             e ? "lvgl" : "internal",
             s_capture_ready ? 1 : 0,
             (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
             s_capture_freq_label);
    if (subghz_capture_popup_ignore_activation()) {
        return;
    }

    if (s_capture_ready && s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
        s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_FREQ;
        subghz_capture_popup_update_buttons();

        subghz_capture_continue_waiting();
        return;
    }

    s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_FREQ;
    subghz_capture_popup_update_buttons();

    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
            return;
        }
        uint8_t next_idx = (uint8_t)((subghz_capture_get_frequency_index() + 1) % SUBGHZ_FA_BAND_COUNT);
        subghz_capture_set_frequency_index(next_idx);
    } else {
        subghz_remote_manager_cycle_frequency();
        subghz_capture_refresh_frequency_label();
    }

    if (s_capture_freq_btn && lv_obj_is_valid(s_capture_freq_btn)) {
        lv_obj_t *btn_label = lv_obj_get_child(s_capture_freq_btn, 0);
        if (btn_label) lv_label_set_text(btn_label, s_capture_freq_label);
    }
    if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
        char status[64];
        snprintf(status, sizeof(status), "Listening: %s", s_capture_freq_label);
        lv_label_set_text(s_capture_status_label, status);
    }

    if (s_capture_waiting_signal || s_capture_ready) {
        subghz_capture_continue_waiting();
    }
}

static void subghz_capture_back_btn_cb(lv_event_t *e) {
    ESP_LOGI(TAG,
             "capture back pressed src=%s ready=%d mode=%s",
             e ? "lvgl" : "internal",
             s_capture_ready ? 1 : 0,
             (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal");
    if (subghz_capture_popup_ignore_activation()) {
        return;
    }
    s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_BACK;
    subghz_capture_popup_update_buttons();
    subghz_close_capture_popup();
}

static void subghz_open_capture_popup(void) {
    if (s_capture_popup && lv_obj_is_valid(s_capture_popup)) {
        subghz_close_capture_popup();
    }

    lv_coord_t popup_w = (LV_HOR_RES <= 240) ? (LV_HOR_RES - 20) : (LV_HOR_RES - 30);
    lv_coord_t popup_h = (LV_VER_RES <= 200) ? (LV_VER_RES - 30) : 190;
    if (popup_w < 220) popup_w = 220;
    if (popup_h < 150) popup_h = 150;

    s_capture_popup = popup_create_container(lv_scr_act(), popup_w, popup_h, true);
    s_capture_popup_opened_us = esp_timer_get_time();
    ESP_LOGI(TAG,
             "capture popup opened mode=%s popup=%p freq=%s",
             (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "raw" : "normal",
             (void *)s_capture_popup,
             s_capture_freq_label);

    const char *title = (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "Raw Capture" : "Capture";
    popup_create_title_label(s_capture_popup, title, accessibility_get_font_body(), 8);
    s_capture_status_label = popup_create_body_label(
        s_capture_popup,
        "Waiting for signal...",
        popup_w - 20,
        false,
        accessibility_get_font_small(),
        32);
    s_capture_signal_label = popup_create_body_label(
        s_capture_popup,
        "Signal threshold: 65%",
        popup_w - 20,
        true,
        accessibility_get_font_small(),
        52);
    lv_obj_set_style_text_align(s_capture_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(s_capture_signal_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_max_height(s_capture_signal_label, 60, 0);
    lv_obj_set_scrollbar_mode(s_capture_signal_label, LV_SCROLLBAR_MODE_AUTO);

    const lv_font_t *btn_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();
    s_capture_save_btn = popup_add_styled_button(
        s_capture_popup,
        "Stop",
        72,
        30,
        LV_ALIGN_BOTTOM_LEFT,
        8,
        -8,
        btn_font,
        subghz_capture_primary_btn_cb,
        NULL);
    s_capture_freq_btn = popup_add_styled_button(
        s_capture_popup,
        s_capture_freq_label,
        70,
        30,
        LV_ALIGN_BOTTOM_MID,
        0,
        -8,
        btn_font,
        subghz_capture_freq_btn_cb,
        NULL);
    s_capture_back_btn = popup_add_styled_button(
        s_capture_popup,
        "Back",
        72,
        30,
        LV_ALIGN_BOTTOM_RIGHT,
        -8,
        -8,
        btn_font,
        subghz_capture_back_btn_cb,
        NULL);

    lv_obj_t *btns[3] = { s_capture_save_btn, s_capture_freq_btn, s_capture_back_btn };
    popup_layout_buttons_responsive(s_capture_popup, btns, 3, -8, NULL);

    if (!s_capture_was_running_known) {
        s_capture_was_running = s_remote_mode ? (s_remote_stream_online && !s_remote_paused)
                                              : subghz_remote_manager_is_running();
        s_capture_was_running_known = true;
    }

    subghz_capture_refresh_frequency_label();

    if (s_remote_mode && !esp_comm_manager_is_connected()) {
        subghz_show_feedback_popup("SubGHz error", "GhostLink disconnected");
        subghz_close_capture_popup();
        return;
    }
}

static void subghz_close_saved_popup(void) {
    lvgl_obj_del_safe(&s_saved_popup);
    s_saved_title_label = NULL;
    s_saved_status_label = NULL;
    s_saved_replay_btn = NULL;
    s_saved_delete_btn = NULL;
    s_saved_back_btn = NULL;
    s_saved_index = 0;
    s_saved_popup_selected = SUBGHZ_SAVED_POPUP_REPLAY;
}

static void subghz_saved_replay_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_saved_file_count <= 0 || !s_saved_file_paths || !s_saved_file_paths[s_saved_index]) return;
    s_saved_popup_selected = SUBGHZ_SAVED_POPUP_REPLAY;
    subghz_saved_popup_update_buttons();
    subghz_load_snapshot_action();
}

static void subghz_saved_delete_confirm_cb(void *user_data) {
    (void)user_data;
    if (s_saved_file_count <= 0 || !s_saved_file_paths || !s_saved_file_paths[s_saved_index]) return;
    if (remove(s_saved_file_paths[s_saved_index]) == 0) {
        subghz_show_feedback_popup("SubGHz", "Capture deleted");
        subghz_close_saved_popup();
        subghz_saved_list_reload();
    } else {
        subghz_show_feedback_popup("SubGHz error", "Failed to delete capture");
    }
}

static void subghz_saved_delete_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_saved_file_count <= 0 || !s_saved_file_paths || !s_saved_file_paths[s_saved_index]) return;
    s_saved_popup_selected = SUBGHZ_SAVED_POPUP_DELETE;
    subghz_saved_popup_update_buttons();
    popup_confirm_show(&s_saved_delete_confirm, lv_layer_top(), "Delete Capture?",
                       "This saved SubGHz capture will be permanently deleted.",
                       "Delete", "Cancel", subghz_saved_delete_confirm_cb, NULL);
}

static void subghz_capture_mark_ready(void) {
    s_capture_stop_pending = false;
    s_capture_stop_deadline_us = 0;
    s_capture_buffer_valid = true;
    subghz_sanitize_snapshot_name(NULL, s_capture_name, sizeof(s_capture_name));
    s_capture_preview_cursor = s_cursor;

    if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
        lv_label_set_text(s_capture_status_label,
                          (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) ? "Raw capture buffered" : "Signal captured");
    }
    if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label)) {
        char msg[128];
        if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
            snprintf(msg, sizeof(msg), "Captured %lu pulses", (unsigned long)s_capture_raw_count);
        } else {
            char decode_buf[96] = {0};
            if (s_capture_raw_count > 0) {
                subghz_analyze_raw_signal(s_capture_raw, s_capture_raw_count, decode_buf, sizeof(decode_buf));
            }
            if (decode_buf[0] != '\0') {
                snprintf(msg, sizeof(msg), "%s", decode_buf);
            } else {
                snprintf(msg, sizeof(msg), "Capture %s", s_capture_name);
            }
        }
        lv_label_set_text(s_capture_signal_label, msg);
    }

    s_capture_waiting_signal = false;
    s_capture_ready = true;
    s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
    subghz_capture_popup_update_buttons();
}

static void subghz_capture_mark_ready_with_decoded(const subghz_decoded_signal_t *decoded) {
    s_capture_stop_pending = false;
    s_capture_stop_deadline_us = 0;
    if (decoded) {
        if (s_remote_decoded.preserved_extra) {
            free(s_remote_decoded.preserved_extra);
            s_remote_decoded.preserved_extra = NULL;
        }
        s_remote_decoded = *decoded;
        /* Transfer ownership of preserved_extra to s_remote_decoded */
        ((subghz_decoded_signal_t *)decoded)->preserved_extra = NULL;
    }
    s_capture_buffer_valid = true;
    subghz_sanitize_snapshot_name(NULL, s_capture_name, sizeof(s_capture_name));
    s_capture_preview_cursor = s_cursor;

    if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
        lv_label_set_text(s_capture_status_label, "Signal decoded");
    }
    if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label) && decoded) {
        char msg[256];
        if (decoded->frequency_hz > 0) {
            snprintf(msg, sizeof(msg), "%.3f MHz\n%.230s",
                     (double)decoded->frequency_hz / 1000000.0, decoded->info);
        } else {
            snprintf(msg, sizeof(msg), "%.244s", decoded->info);
        }
        lv_label_set_text(s_capture_signal_label, msg);
    }

    s_capture_waiting_signal = false;
    s_capture_ready = true;
    s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
    subghz_capture_popup_update_buttons();

    if (s_remote_mode) {
        if (esp_comm_manager_is_connected()) {
            (void)esp_comm_manager_send_command("subghz", "pause");
        }
    } else {
        subghz_remote_manager_set_paused(true);
    }
}

static void subghz_saved_back_btn_cb(lv_event_t *e) {
    (void)e;
    subghz_close_saved_popup();
}

static void subghz_open_saved_popup(void) {
    if (s_saved_popup && lv_obj_is_valid(s_saved_popup)) {
        subghz_close_saved_popup();
    }

    lv_coord_t popup_w = (LV_HOR_RES <= 240) ? (LV_HOR_RES - 16) : (LV_HOR_RES - 26);
    lv_coord_t popup_h = (LV_VER_RES <= 200) ? (LV_VER_RES - 24) : 190;
    if (popup_w < 220) popup_w = 220;
    if (popup_h < 150) popup_h = 150;

    s_saved_popup = popup_create_container(lv_scr_act(), popup_w, popup_h, true);

    s_saved_title_label = popup_create_title_label(s_saved_popup, "Saved Capture", accessibility_get_font_body(), 8);
    s_saved_status_label = popup_create_body_label(
        s_saved_popup,
        "Loading...",
        popup_w - 20,
        true,
        accessibility_get_font_small(),
        38);
    lv_obj_set_style_text_align(s_saved_status_label, LV_TEXT_ALIGN_CENTER, 0);

    const lv_font_t *btn_font = (LV_VER_RES <= 240) ? accessibility_get_font_small() : accessibility_get_font_body();
    s_saved_replay_btn = popup_add_styled_button(
        s_saved_popup,
        "Replay",
        68,
        30,
        LV_ALIGN_BOTTOM_LEFT,
        6,
        -8,
        btn_font,
        subghz_saved_replay_btn_cb,
        NULL);
    s_saved_delete_btn = popup_add_styled_button(
        s_saved_popup,
        "Delete",
        68,
        30,
        LV_ALIGN_BOTTOM_LEFT,
        80,
        -8,
        btn_font,
        subghz_saved_delete_btn_cb,
        NULL);
    s_saved_back_btn = popup_add_styled_button(
        s_saved_popup,
        "Back",
        68,
        30,
        LV_ALIGN_BOTTOM_RIGHT,
        -6,
        -8,
        btn_font,
        subghz_saved_back_btn_cb,
        NULL);

    lv_obj_t *btns[3] = { s_saved_replay_btn, s_saved_delete_btn, s_saved_back_btn };
    popup_layout_buttons_responsive(s_saved_popup, btns, 3, -8, NULL);

    s_saved_popup_selected = SUBGHZ_SAVED_POPUP_REPLAY;
    subghz_saved_popup_refresh_text();
    subghz_saved_popup_update_buttons();
}

#ifdef CONFIG_USE_TOUCHSCREEN
static void subghz_scroll_up_cb(lv_event_t *e) {
    (void)e;
    options_view_move_selection(s_ov, -1);
    subghz_update_scroll_buttons_visibility();
}

static void subghz_scroll_down_cb(lv_event_t *e) {
    (void)e;
    options_view_move_selection(s_ov, 1);
    subghz_update_scroll_buttons_visibility();
}

static void subghz_back_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_in_saved_list) {
        subghz_back_to_root_menu();
    } else {
        display_manager_go_back();
    }
}

static void subghz_update_scroll_buttons_visibility(void) {
    lv_obj_t *list = (s_ov != NULL) ? options_view_get_list(s_ov) : NULL;
    gui_touch_bar_update_visibility(&s_touch_tb, list);
}
#endif

static void subghz_select_row(void) {
    if (s_in_saved_list) {
        int sel = options_view_get_selected(s_ov);
        int item_count = options_view_get_item_count(s_ov);
        if (item_count <= 0) {
            return;
        }
        if (sel == item_count - 1) {
            subghz_back_to_root_menu();
            return;
        }

        int visible_files = s_saved_file_count - s_saved_page * 7;
        if (visible_files > 7) visible_files = 7;
        int has_prev = (s_saved_page > 0) ? 1 : 0;
        int has_next = ((s_saved_page + 1) * 7 < s_saved_file_count) ? 1 : 0;
        if (sel < visible_files) {
            s_saved_index = s_saved_page * 7 + sel;
            subghz_open_saved_popup();
        } else if (has_prev && sel == visible_files) {
            subghz_saved_list_prev_page_cb(NULL);
        } else if (has_next && sel == visible_files + has_prev) {
            subghz_saved_list_next_page_cb(NULL);
        }
        return;
    }

    int sel = options_view_get_selected(s_ov);
    if (sel == 0) {
        s_capture_mode = SUBGHZ_CAPTURE_MODE_NORMAL;
        subghz_capture_snapshot_action();
    } else if (sel == 1) {
        s_capture_mode = SUBGHZ_CAPTURE_MODE_RAW;
        subghz_capture_snapshot_action();
    } else if (sel == 2) {
        subghz_list_snapshots_action();
    } else if (sel == 3) {
        subghz_open_freq_analyzer_popup();
    } else if (sel == 4) {
        subghz_open_waterfall_popup();
    } else {
        display_manager_go_back();
    }
}

static void subghz_scan_row_cb(lv_event_t *e) {
    (void)e;
    if (s_ov) {
        options_view_set_selected(s_ov, 0);
    }
    s_capture_mode = SUBGHZ_CAPTURE_MODE_NORMAL;
    subghz_capture_snapshot_action();
}

static void subghz_raw_capture_row_cb(lv_event_t *e) {
    (void)e;
    if (s_ov) {
        options_view_set_selected(s_ov, 1);
    }
    s_capture_mode = SUBGHZ_CAPTURE_MODE_RAW;
    subghz_capture_snapshot_action();
}

static void subghz_back_row_cb(lv_event_t *e) {
    (void)e;
    if (s_in_saved_list) {
        subghz_back_to_root_menu();
        return;
    }
    if (s_ov) {
        options_view_set_selected(s_ov, 5);
    }
    display_manager_go_back();
}

static void subghz_capture_row_cb(lv_event_t *e) {
    (void)e;
    if (s_ov) {
        options_view_set_selected(s_ov, 2);
    }
    subghz_saved_list_reload();
}

static void subghz_save_row_cb(lv_event_t *e) {
    (void)e;
    subghz_save_snapshot_action();
}

static void subghz_load_row_cb(lv_event_t *e) {
    (void)e;
    subghz_load_snapshot_action();
}

static void subghz_list_row_cb(lv_event_t *e) {
    (void)e;
    subghz_saved_list_reload();
}

static void subghz_timer_cb(lv_timer_t *timer) {
    (void)timer;

    bool scanner_popup_open = s_popup && lv_obj_is_valid(s_popup);
    bool capture_popup_open = s_capture_popup && lv_obj_is_valid(s_capture_popup);
    bool fa_popup_open = s_fa_popup && lv_obj_is_valid(s_fa_popup);

    if (s_capture_popup_open_pending && !capture_popup_open) {
        ESP_LOGI(TAG, "timer opening deferred capture popup");
        s_capture_popup_open_pending = false;
        subghz_open_capture_popup();
        capture_popup_open = s_capture_popup && lv_obj_is_valid(s_capture_popup);
    }
    if (s_capture_popup_sync_pending && capture_popup_open) {
        ESP_LOGI(TAG, "timer syncing deferred capture popup state");
        s_capture_popup_sync_pending = false;
        subghz_capture_apply_waiting_ui(false);
    }

    bool wf_popup_open = s_wf_popup && lv_obj_is_valid(s_wf_popup);

    if (!scanner_popup_open && !capture_popup_open && !fa_popup_open && !wf_popup_open) {
        return;
    }

    if (s_remote_mode) {
        if (!esp_comm_manager_is_connected()) {
            s_remote_stream_online = false;
            if (s_pending_action != SUBGHZ_PENDING_NONE) {
                subghz_fail_pending_action("GhostLink disconnected");
            }
        }
    } else {
        (void)subghz_remote_manager_get_levels(s_levels, sizeof(s_levels), &s_cursor);

        if (s_capture_popup && lv_obj_is_valid(s_capture_popup) && s_capture_waiting_signal && s_capture_mode != SUBGHZ_CAPTURE_MODE_RAW) {
            subghz_decoded_signal_t decoded;
            if (subghz_remote_manager_take_decode_result(&decoded)) {
                s_capture_raw_count = 0;
                s_capture_buffer_valid = true;
                subghz_capture_mark_ready_with_decoded(&decoded);
                goto done_local;
            }
        }

        size_t raw_count = 0;
        if (subghz_remote_manager_take_raw_capture(s_remote_raw_work, SUBGHZ_RAW_MAX_DURATIONS, &raw_count)) {
            if (raw_count > 0 && s_capture_popup && lv_obj_is_valid(s_capture_popup) && s_capture_waiting_signal) {
                if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
                    if (s_capture_raw_count + raw_count <= SUBGHZ_RAW_MAX_DURATIONS) {
                        memcpy(s_capture_raw + s_capture_raw_count, s_remote_raw_work, raw_count * sizeof(int32_t));
                        s_capture_raw_count += raw_count;
                    } else {
                        s_capture_raw_count = raw_count;
                        memcpy(s_capture_raw, s_remote_raw_work, raw_count * sizeof(int32_t));
                    }
                    s_capture_buffer_valid = true;
                    if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label)) {
                        char msg[64];
                        snprintf(msg, sizeof(msg), "Captured %zu pulses", s_capture_raw_count);
                        lv_label_set_text(s_capture_signal_label, msg);
                    }
                } else {
                    bool decoded_ok = false;
                    subghz_decoded_signal_t decoded;
                    memset(&decoded, 0, sizeof(decoded));
                    if (raw_count >= 16) {
                        if (subghz_decode_signal(s_remote_raw_work, raw_count, &decoded) && decoded.decoded) {
                            decoded_ok = true;
                            memcpy(s_capture_raw, s_remote_raw_work, raw_count * sizeof(int32_t));
                            s_capture_raw_count = raw_count;
                        }
                    }
                    if (!decoded_ok) {
                        if (s_capture_raw_count + raw_count <= SUBGHZ_RAW_MAX_DURATIONS) {
                            memcpy(s_capture_raw + s_capture_raw_count, s_remote_raw_work, raw_count * sizeof(int32_t));
                            s_capture_raw_count += raw_count;
                        } else {
                            s_capture_raw_count = raw_count;
                            memcpy(s_capture_raw, s_remote_raw_work, raw_count * sizeof(int32_t));
                        }
                        if (s_capture_raw_count >= 16) {
                            if (subghz_decode_signal(s_capture_raw, s_capture_raw_count, &decoded) && decoded.decoded) {
                                decoded_ok = true;
                            }
                        }
                    }
                    if (decoded_ok) {
                        subghz_capture_mark_ready_with_decoded(&decoded);
                    }
                }
            }
        }
    }
    done_local:

    if (s_pending_action != SUBGHZ_PENDING_NONE && esp_timer_get_time() >= s_pending_action_deadline_us) {
        subghz_fail_pending_action("no peer response (unsupported or offline)");
    }

    if (s_capture_stop_pending && esp_timer_get_time() >= s_capture_stop_deadline_us) {
        s_capture_stop_pending = false;
        s_capture_stop_deadline_us = 0;
        if (s_capture_buffer_valid && s_capture_raw_count > 0) {
            ESP_LOGI(TAG, "remote raw stop completed with %lu pulses buffered", (unsigned long)s_capture_raw_count);
            subghz_capture_mark_ready();
        } else {
            ESP_LOGI(TAG, "remote raw stop timed out without buffered pulses");
            s_capture_ready = true;
            s_capture_buffer_valid = false;
            s_capture_waiting_signal = false;
            s_capture_popup_selected = SUBGHZ_CAPTURE_POPUP_SAVE;
            if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
                lv_label_set_text(s_capture_status_label, "No signal captured");
            }
            subghz_capture_popup_update_buttons();
        }
    }

    for (int i = 0; i < SUBGHZ_SCANNER_CHANNEL_COUNT; i++) {
        if (s_levels[i] > s_peaks[i]) {
            s_peaks[i] = s_levels[i];
        } else if (s_peaks[i] > 0) {
            s_peaks[i]--;
        }
    }

    if (s_capture_popup && lv_obj_is_valid(s_capture_popup) && s_capture_waiting_signal) {
        int strong = 0;
        for (int i = 0; i < SUBGHZ_SCANNER_CHANNEL_COUNT; i++) {
            if (s_levels[i] >= SUBGHZ_CAPTURE_SIGNAL_THRESHOLD) {
                strong++;
            }
        }

        if (strong > 0) {
            s_capture_signal_hits++;
            if (s_capture_signal_label && lv_obj_is_valid(s_capture_signal_label)) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Signal %d%%  hits %d/%d", s_levels[s_cursor], s_capture_signal_hits, SUBGHZ_CAPTURE_SIGNAL_HITS);
                lv_label_set_text(s_capture_signal_label, msg);
            }
        } else if (s_capture_signal_hits > 0) {
            s_capture_signal_hits--;
        }
    }

    if (scanner_popup_open) {
        subghz_refresh_status_labels();
    }
    if (scanner_popup_open && s_graph && lv_obj_is_valid(s_graph)) {
        lv_obj_invalidate(s_graph);
    }

    if (fa_popup_open) {
        if (!s_remote_mode && subghz_remote_manager_is_running()) {
            uint8_t cur_band_idx = subghz_fa_freq_to_band_idx(subghz_remote_manager_get_frequency_hz());
            s_fa_active_band = cur_band_idx;
            uint8_t tmp[SUBGHZ_SCANNER_CHANNEL_COUNT];
            uint8_t tmp_cur;
            if (subghz_remote_manager_get_levels(tmp, SUBGHZ_SCANNER_CHANNEL_COUNT, &tmp_cur)) {
                uint8_t max_lvl = 0;
                for (int i = 0; i < SUBGHZ_SCANNER_CHANNEL_COUNT; i++) {
                    if (tmp[i] > max_lvl) max_lvl = tmp[i];
                }
                s_fa_band_levels[cur_band_idx] = max_lvl;
                if (max_lvl > s_fa_band_peaks[cur_band_idx])
                    s_fa_band_peaks[cur_band_idx] = max_lvl;
            }
        }

        for (int b = 0; b < SUBGHZ_FA_BAND_COUNT; b++) {
            if (s_fa_band_peaks[b] > 0) s_fa_band_peaks[b]--;
            if (s_fa_band_levels[b] > 0 && b != s_fa_active_band) {
                if (s_fa_band_levels[b] > 2) s_fa_band_levels[b] -= 2;
                else s_fa_band_levels[b] = 0;
            }
        }

        int best_band = -1;
        uint8_t best_lvl = 0;
        uint16_t total_lvl = 0;
        for (int b = 0; b < SUBGHZ_FA_BAND_COUNT; b++) {
            total_lvl += s_fa_band_levels[b];
            if (s_fa_band_levels[b] > best_lvl) {
                best_lvl = s_fa_band_levels[b];
                best_band = b;
            }
        }
        uint8_t avg_lvl = (uint8_t)(total_lvl / SUBGHZ_FA_BAND_COUNT);

        bool is_outlier = (best_band >= 0 && best_lvl >= SUBGHZ_FA_DETECT_THRESHOLD
                          && best_lvl >= avg_lvl + 25);

        if (is_outlier) {
            s_fa_detect_hits++;
            if (s_fa_detect_hits >= SUBGHZ_FA_DETECT_HITS) {
                uint32_t new_freq = s_scan_freqs_hz[best_band];
                if (!s_fa_signal_detected || new_freq != s_fa_detected_freq_hz) {
                    s_fa_signal_detected = true;
                    s_fa_detected_freq_hz = new_freq;
                    subghz_fa_add_to_history(new_freq);
                    subghz_fa_update_history_label();
                }
            }
        } else {
            if (s_fa_detect_hits > 0) s_fa_detect_hits--;
            if (s_fa_detect_hits == 0) s_fa_signal_detected = false;
        }

        if (s_fa_freq_label && lv_obj_is_valid(s_fa_freq_label)) {
            if (s_fa_signal_detected && s_fa_detected_freq_hz > 0) {
                uint32_t mhz = s_fa_detected_freq_hz / 1000000U;
                uint32_t khz = (s_fa_detected_freq_hz % 1000000U) / 1000U;
                lv_label_set_text_fmt(s_fa_freq_label, "%lu.%03lu MHz",
                                     (unsigned long)mhz, (unsigned long)khz);
            } else if (best_band >= 0 && best_lvl > 0) {
                lv_label_set_text_fmt(s_fa_freq_label, "%s ~%d%%",
                                     s_scan_freq_labels[best_band], best_lvl);
            } else {
                lv_label_set_text(s_fa_freq_label, "---.--- MHz");
            }
        }


        if (s_fa_graph && lv_obj_is_valid(s_fa_graph)) {
            lv_obj_invalidate(s_fa_graph);
        }
    }

    if (wf_popup_open) {
        bool wf_updated = subghz_wf_update_frame();
        if (wf_updated && s_wf_canvas && lv_obj_is_valid(s_wf_canvas)) {
            lv_obj_invalidate(s_wf_canvas);
        }
    }
}

static void subghz_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data || length < 2) {
        return;
    }
    uint8_t ver = data[0];
    if (ver != SUBGHZ_STREAM_VERSION && ver != 1) {
        return;
    }

    uint8_t packet_type = data[1];

    /* v2 stream begin (0x10), replay legacy (4), or raw capture legacy (1) */
    if (packet_type == 0x10 || packet_type == 4 || packet_type == 1) {
        if (packet_type == 0x10 && length >= 8) {
            s_remote_raw_expected = (size_t)data[2] | ((size_t)data[3] << 8) |
                                    ((size_t)data[4] << 16) | ((size_t)data[5] << 24);
        } else if (length >= 4) {
            s_remote_raw_expected = (size_t)data[2] | ((size_t)data[3] << 8);
        } else {
            return;
        }
        if (s_remote_raw_expected > SUBGHZ_RAW_MAX_DURATIONS) {
            s_remote_raw_expected = SUBGHZ_RAW_MAX_DURATIONS;
        }
        s_remote_raw_received = 0;
        return;
    }

    if (packet_type == 0x11 || packet_type == 5 || packet_type == 2) {
        if (length < 5) {
            return;
        }
        size_t offset = (size_t)data[2] | ((size_t)data[3] << 8);
        size_t count = (size_t)data[4];
        if (length < 5 + count * 4 || offset + count > SUBGHZ_RAW_MAX_DURATIONS) {
            return;
        }
        for (size_t i = 0; i < count; i++) {
            size_t base = 5 + i * 4;
            int32_t v = (int32_t)((uint32_t)data[base] |
                                  ((uint32_t)data[base + 1] << 8) |
                                  ((uint32_t)data[base + 2] << 16) |
                                  ((uint32_t)data[base + 3] << 24));
            s_remote_raw_work[offset + i] = v;
        }
        if (offset + count > s_remote_raw_received) {
            s_remote_raw_received = offset + count;
        }
        return;
    }

    if (packet_type == 0x12 || packet_type == 6 || packet_type == 3) {
        if (s_remote_raw_received > 0) {
            size_t new_count = s_remote_raw_received;
            if (new_count > SUBGHZ_RAW_MAX_DURATIONS) {
                new_count = SUBGHZ_RAW_MAX_DURATIONS;
            }
            if (s_capture_popup && lv_obj_is_valid(s_capture_popup) && s_capture_waiting_signal) {
                if (s_capture_mode == SUBGHZ_CAPTURE_MODE_RAW) {
                    ESP_LOGI(TAG,
                             "received remote raw end packet new_count=%lu stop_pending=%d",
                             (unsigned long)new_count,
                             s_capture_stop_pending ? 1 : 0);
                    if (s_capture_raw_count + new_count <= SUBGHZ_RAW_MAX_DURATIONS) {
                        memcpy(s_capture_raw + s_capture_raw_count, s_remote_raw_work, new_count * sizeof(int32_t));
                        s_capture_raw_count += new_count;
                    } else {
                        s_capture_raw_count = new_count;
                        memcpy(s_capture_raw, s_remote_raw_work, s_capture_raw_count * sizeof(int32_t));
                    }
                    s_capture_buffer_valid = true;
                    if (s_capture_stop_pending && s_capture_raw_count > 0) {
                        ESP_LOGI(TAG, "marking remote raw capture ready total_count=%lu", (unsigned long)s_capture_raw_count);
                        subghz_capture_mark_ready();
                    }
                } else {
                    bool decoded_ok = false;
                    subghz_decoded_signal_t decoded;
                    memset(&decoded, 0, sizeof(decoded));
                    if (new_count >= 16) {
                        if (subghz_decode_signal(s_remote_raw_work, new_count, &decoded) && decoded.decoded) {
                            decoded_ok = true;
                            memcpy(s_capture_raw, s_remote_raw_work, new_count * sizeof(int32_t));
                            s_capture_raw_count = new_count;
                        }
                    }
                    if (!decoded_ok) {
                        if (s_capture_raw_count + new_count <= SUBGHZ_RAW_MAX_DURATIONS) {
                            memcpy(s_capture_raw + s_capture_raw_count, s_remote_raw_work, new_count * sizeof(int32_t));
                            s_capture_raw_count += new_count;
                        } else {
                            s_capture_raw_count = new_count;
                            memcpy(s_capture_raw, s_remote_raw_work, s_capture_raw_count * sizeof(int32_t));
                        }
                        if (s_capture_raw_count >= 16) {
                            if (subghz_decode_signal(s_capture_raw, s_capture_raw_count, &decoded) && decoded.decoded) {
                                decoded_ok = true;
                            }
                        }
                    }
                    if (decoded_ok) {
                        subghz_capture_mark_ready_with_decoded(&decoded);
                    }
                }
            } else {
                s_capture_raw_count = new_count;
                memcpy(s_capture_raw, s_remote_raw_work, s_capture_raw_count * sizeof(int32_t));
            }
        }
        return;
    }

    if (packet_type == 8 || packet_type == 9) {
        if (length < 12) return;
        uint8_t name_len = data[2];
        if (length < (size_t)(3 + name_len + 8 + 1)) return;
        if (name_len >= SUBGHZ_DECODED_PROTO_MAX) name_len = SUBGHZ_DECODED_PROTO_MAX - 1;

        if (s_capture_popup && lv_obj_is_valid(s_capture_popup) && s_capture_waiting_signal &&
            s_capture_mode != SUBGHZ_CAPTURE_MODE_RAW) {
            subghz_decoded_signal_t decoded;
            memset(&decoded, 0, sizeof(decoded));
            memcpy(decoded.protocol, data + 3, name_len);
            decoded.protocol[name_len] = '\0';
            size_t code_pos = 3 + name_len;
            uint64_t code = 0;
            for (int i = 0; i < 8; i++) code |= ((uint64_t)data[code_pos + i]) << (i * 8);
            decoded.code = code;
            decoded.bits = subghz_normalize_decoded_bits(decoded.protocol, (int)data[code_pos + 8]);
            decoded.decoded = true;
            decoded.te = (int)subghz_protocol_te(decoded.protocol);

            if (packet_type == 9 && length >= (size_t)(3 + name_len + 8 + 1 + 4)) {
                size_t freq_pos = code_pos + 9;
                decoded.frequency_hz = (int)((uint32_t)data[freq_pos] |
                                             ((uint32_t)data[freq_pos + 1] << 8) |
                                             ((uint32_t)data[freq_pos + 2] << 16) |
                                             ((uint32_t)data[freq_pos + 3] << 24));
            } else {
                decoded.frequency_hz = SUBGHZ_BASE_FREQ_HZ;
            }

            if (decoded.bits > 32) {
                snprintf(decoded.info, sizeof(decoded.info), "%s %dbit\nCode:0x%016llX",
                         decoded.protocol, decoded.bits, (unsigned long long)decoded.code);
            } else {
                int pos = 0;
                pos += snprintf(decoded.info + pos, sizeof(decoded.info) - pos,
                                "%s %dbit\nCode:0x", decoded.protocol, decoded.bits);
                uint32_t c = (uint32_t)decoded.code;
                for (int i = decoded.bits - 4; i >= 0; i -= 4) {
                    if (pos < (int)sizeof(decoded.info) - 2)
                        pos += snprintf(decoded.info + pos, sizeof(decoded.info) - pos,
                                        "%X", (unsigned)((c >> i) & 0xF));
                }
            }

            s_capture_buffer_valid = true;
            subghz_capture_mark_ready_with_decoded(&decoded);
        }
        return;
    }

    if (packet_type == SUBGHZ_STREAM_WATERFALL_LINE) {
        if (length < 6) {
            return;
        }
        uint8_t count = data[2];
        if (count < 2 || count > SUBGHZ_SCANNER_CHANNEL_COUNT || (size_t)(6 + count) > length) {
            return;
        }
        memcpy(s_wf_remote_line, data + 6, count);
        s_wf_remote_count = count;
        s_wf_remote_freq_idx = data[3];
        if (s_wf_remote_freq_idx >= SUBGHZ_FA_BAND_COUNT) {
            s_wf_remote_freq_idx = 2;
        }
        s_wf_remote_seq = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        subghz_wf_store_band_line(s_wf_remote_freq_idx, s_wf_remote_line, count);
        if (s_wf_remote_seq == 1 || (s_wf_remote_seq % 32U) == 0U) {
            uint8_t peak = 0;
            for (uint8_t i = 0; i < count; i++) {
                if (s_wf_remote_line[i] > peak) {
                    peak = s_wf_remote_line[i];
                }
            }
            ESP_LOGI(TAG, "waterfall rx seq=%u freq_idx=%u bins=%u peak=%u", (unsigned)s_wf_remote_seq, (unsigned)s_wf_remote_freq_idx, (unsigned)count, (unsigned)peak);
        }
        s_wf_remote_ready = true;
        s_remote_stream_online = true;
        s_remote_error = false;
        return;
    }

    if (packet_type == SUBGHZ_STREAM_WATERFALL_CHUNK) {
        if (length < 8) {
            return;
        }
        uint8_t total = data[2];
        uint8_t freq_idx = data[3];
        uint16_t seq = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        uint8_t offset = data[6];
        uint8_t chunk = data[7];
        if (total < 2 || total > SUBGHZ_SCANNER_CHANNEL_COUNT || chunk == 0 || offset >= total ||
            (uint8_t)(offset + chunk) > total || (size_t)(8 + chunk) > length) {
            return;
        }
        if (freq_idx >= SUBGHZ_FA_BAND_COUNT) {
            freq_idx = 2;
        }
        if (seq != s_wf_remote_build_seq || total != s_wf_remote_build_count || offset == 0) {
            memset(s_wf_remote_build_line, 0, sizeof(s_wf_remote_build_line));
            s_wf_remote_build_seq = seq;
            s_wf_remote_build_count = total;
            s_wf_remote_build_received = 0;
            s_wf_remote_build_mask = 0;
        }
        uint8_t chunk_bit = (offset == 0) ? 1U : 2U;
        if (s_wf_remote_build_mask & chunk_bit) {
            return;
        }
        memcpy(s_wf_remote_build_line + offset, data + 8, chunk);
        s_wf_remote_build_received = (uint8_t)(s_wf_remote_build_received + chunk);
        s_wf_remote_build_mask |= chunk_bit;
        if (s_wf_remote_build_received >= total && s_wf_remote_build_mask != 0) {
            memcpy(s_wf_remote_line, s_wf_remote_build_line, total);
            s_wf_remote_count = total;
            s_wf_remote_freq_idx = freq_idx;
            s_wf_remote_seq = seq;
            s_wf_remote_ready = true;
            subghz_wf_store_band_line(s_wf_remote_freq_idx, s_wf_remote_line, total);
            if (s_wf_remote_seq == 1 || (s_wf_remote_seq % 32U) == 0U) {
                uint8_t peak = 0;
                for (uint8_t i = 0; i < total; i++) {
                    if (s_wf_remote_line[i] > peak) {
                        peak = s_wf_remote_line[i];
                    }
                }
                ESP_LOGI(TAG, "waterfall rx seq=%u freq_idx=%u bins=%u peak=%u", (unsigned)s_wf_remote_seq, (unsigned)s_wf_remote_freq_idx, (unsigned)total, (unsigned)peak);
            }
        }
        s_remote_stream_online = true;
        s_remote_error = false;
        return;
    }

    if (packet_type != 0) {
        return;
    }

    if (length < 7) {
        return;
    }

    uint8_t cursor = data[2];
    uint8_t start_ch = data[3];
    uint8_t count = data[4];
    s_remote_freq_idx = data[5];
    if (s_remote_freq_idx > 4) s_remote_freq_idx = 2;

    if (count == 0 || count > 32 || (size_t)(7 + count) > length) {
        return;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)((start_ch + i) % SUBGHZ_SCANNER_CHANNEL_COUNT);
        s_levels[ch] = data[7 + i];
        if (s_levels[ch] > s_peaks[ch]) {
            s_peaks[ch] = s_levels[ch];
        }
    }

    if (s_fa_popup && lv_obj_is_valid(s_fa_popup)) {
        uint8_t max_lvl = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (data[7 + i] > max_lvl) max_lvl = data[7 + i];
        }
        uint8_t bi = s_remote_freq_idx;
        if (bi < SUBGHZ_FA_BAND_COUNT) {
            s_fa_band_levels[bi] = max_lvl;
            s_fa_active_band = bi;
            if (max_lvl > s_fa_band_peaks[bi])
                s_fa_band_peaks[bi] = max_lvl;
        }
    }

    s_cursor = (uint8_t)(cursor % SUBGHZ_SCANNER_CHANNEL_COUNT);
    if (s_wf_remote_have_cursor && s_cursor < s_wf_remote_last_cursor) {
        memcpy(s_wf_remote_line, s_levels, sizeof(s_wf_remote_line));
        s_wf_remote_count = SUBGHZ_SCANNER_CHANNEL_COUNT;
        s_wf_remote_freq_idx = s_remote_freq_idx;
        s_wf_remote_seq++;
        s_wf_remote_ready = true;
    }
    s_wf_remote_last_cursor = s_cursor;
    s_wf_remote_have_cursor = true;
    s_remote_stream_online = true;
    s_remote_error = false;
}

void subghz_view_register_stream_handler(void) {
    (void)esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_SUBGHZ, subghz_stream_rx_cb, NULL);
}

void subghz_view_update_remote_state(const char *state) {
    if (!state) {
        return;
    }

    if (strcmp(state, "started") == 0 || strcmp(state, "waterfall_started") == 0 || strcmp(state, "running") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_remote_paused = false;
        subghz_clear_pending_action();
    } else if (strcmp(state, "paused") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_remote_paused = true;
        subghz_clear_pending_action();
    } else if (strcmp(state, "resumed") == 0) {
        s_remote_stream_online = true;
        s_remote_error = false;
        s_remote_paused = false;
        subghz_clear_pending_action();
    } else if (strncmp(state, "capture_begin_ok", 16) == 0) {
        ESP_LOGI(TAG, "remote state capture_begin_ok defer_popup=%d", s_capture_defer_popup_open ? 1 : 0);
        s_remote_stream_online = true;
        s_remote_error = false;
        s_remote_paused = false;
        subghz_clear_pending_action();
        s_capture_remote_arm_pending = false;
        bool should_open_popup = s_capture_defer_popup_open && (!s_capture_popup || !lv_obj_is_valid(s_capture_popup));
        s_capture_defer_popup_open = false;
        subghz_capture_reset_state_for_new_attempt();
        s_capture_waiting_signal = true;
        s_capture_popup_open_pending = should_open_popup;
        s_capture_popup_sync_pending = true;
    } else if (strncmp(state, "capture_begin_error", 19) == 0) {
        const char *reason = state + 19;
        while (*reason == ' ') {
            reason++;
        }
        ESP_LOGI(TAG, "remote state capture_begin_error reason='%s'", reason);
        subghz_fail_pending_action((*reason != '\0') ? reason : "capture arm failed");
    } else if (strcmp(state, "stopped") == 0) {
        s_remote_stream_online = false;
        s_remote_paused = true;
        if (s_capture_defer_popup_open && (!s_capture_popup || !lv_obj_is_valid(s_capture_popup))) {
            s_capture_was_running_known = false;
        }
        s_capture_remote_arm_pending = false;
        s_capture_defer_popup_open = false;
        subghz_clear_pending_action();
    } else if (strcmp(state, "error") == 0) {
        s_remote_error = true;
        s_remote_stream_online = false;
        s_remote_paused = true;
        subghz_fail_pending_action("peer returned error");
    } else if (strcmp(state, "capture_ok") == 0) {
        subghz_clear_pending_action();
        subghz_show_action_status("Remote capture complete");
        subghz_show_feedback_popup("SubGHz", "Remote capture complete");
    } else if (strcmp(state, "capture_error") == 0) {
        subghz_fail_pending_action("capture failed");
    } else if (strncmp(state, "freq_", 5) == 0) {
        const char *freq_text = state + 5;
        for (uint8_t i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
            if (strcmp(freq_text, s_scan_freq_labels[i]) == 0) {
                snprintf(s_capture_freq_label, sizeof(s_capture_freq_label), "%s", s_scan_freq_short[i]);
                break;
            }
        }
        if (s_capture_freq_btn && lv_obj_is_valid(s_capture_freq_btn)) {
            lv_obj_t *btn_label = lv_obj_get_child(s_capture_freq_btn, 0);
            if (btn_label) lv_label_set_text(btn_label, s_capture_freq_label);
        }
        if (s_capture_status_label && lv_obj_is_valid(s_capture_status_label)) {
            char status[64];
            snprintf(status, sizeof(status), "Listening: %s", s_capture_freq_label);
            lv_label_set_text(s_capture_status_label, status);
        }
        for (int i = 0; i < SUBGHZ_FA_BAND_COUNT; i++) {
            if (strcmp(state + 5, s_scan_freq_labels[i]) == 0) {
                s_remote_freq_idx = (uint8_t)i;
                subghz_wf_set_freq_label(s_remote_freq_idx);
                break;
            }
        }
    } else if (strcmp(state, "save_ok") == 0) {
        subghz_clear_pending_action();
        subghz_show_action_status("Remote save complete");
        subghz_show_feedback_popup("SubGHz", "Remote snapshot saved");
    } else if (strcmp(state, "save_error") == 0) {
        subghz_fail_pending_action("save failed");
    } else if (strcmp(state, "load_ok") == 0) {
        subghz_clear_pending_action();
        subghz_show_action_status("Remote replay complete");
        subghz_show_feedback_popup("SubGHz", "Remote replay complete");
    } else if (strcmp(state, "load_error") == 0) {
        subghz_fail_pending_action("replay failed");
    } else if (strcmp(state, "list_ok") == 0) {
        subghz_clear_pending_action();
        subghz_show_action_status("Remote list complete");
        subghz_show_feedback_popup("SubGHz", "Remote list complete (check terminal)");
    } else if (strcmp(state, "list_empty") == 0) {
        subghz_clear_pending_action();
        subghz_show_action_status("Remote list empty");
        subghz_show_feedback_popup("SubGHz", "No remote snapshots found");
    } else if (strcmp(state, "list_error") == 0) {
        subghz_fail_pending_action("list failed");
    }
}

static bool subghz_saved_delete_confirm_handle_input(InputEvent *event) {
    if (!popup_confirm_is_open(s_saved_delete_confirm)) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) return popup_confirm_handle_touch(&s_saved_delete_confirm, &event->data.touch_data);
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        popup_confirm_cancel(&s_saved_delete_confirm);
        return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int ji = event->data.joystick_index;
        if (ji == 1) popup_confirm_select(&s_saved_delete_confirm);
        else if (ji == 0) popup_confirm_set_selected(s_saved_delete_confirm, 0);
        else if (ji == 3) popup_confirm_set_selected(s_saved_delete_confirm, 1);
        else if (ji == 2 || ji == 4) popup_confirm_move(s_saved_delete_confirm, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) popup_confirm_select(&s_saved_delete_confirm);
        else if (event->data.encoder.direction != 0) popup_confirm_move(s_saved_delete_confirm, event->data.encoder.direction);
        return true;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == 13 || kv == 10 || kv == LV_KEY_ENTER) popup_confirm_select(&s_saved_delete_confirm);
        else if (kv == 27 || kv == LV_KEY_ESC || kv == 29 || kv == '`' || kv == 'c' || kv == 'C') popup_confirm_cancel(&s_saved_delete_confirm);
        else if (kv == ',' || kv == '.' || kv == ';' || kv == '/' || kv == 'h' || kv == 'l' || kv == 'k' || kv == 'j' ||
                 kv == LV_KEY_LEFT || kv == LV_KEY_RIGHT || kv == LV_KEY_UP || kv == LV_KEY_DOWN) popup_confirm_move(s_saved_delete_confirm, 1);
        return true;
    }

    return true;
}

static void subghz_input_handler(InputEvent *event) {
    if (!event) {
        return;
    }

    if (subghz_saved_delete_confirm_handle_input(event)) {
        return;
    }

    if (s_capture_popup && lv_obj_is_valid(s_capture_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_REL) {
                if (s_capture_popup_opened_us > 0 &&
                    (esp_timer_get_time() - s_capture_popup_opened_us) < 250000LL) {
                    return;
                }
                if (point_inside_obj(s_capture_save_btn, d->point.x, d->point.y)) {
                    subghz_capture_primary_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_capture_freq_btn, d->point.x, d->point.y)) {
                    subghz_capture_freq_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_capture_back_btn, d->point.x, d->point.y)) {
                    subghz_capture_back_btn_cb(NULL);
                    return;
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER ||
                   event->type == INPUT_TYPE_KEYBOARD) {
            int dir = 0;
            bool activate = false;
            bool back = false;

            if (event->type == INPUT_TYPE_JOYSTICK) {
                int ji = event->data.joystick_index;
                if (ji == 0) dir = -1;
                else if (ji == 3) dir = 1;
                else if (ji == 1) activate = true;
            } else if (event->type == INPUT_TYPE_ENCODER) {
                if (event->data.encoder.button) activate = true;
                else dir = (event->data.encoder.direction > 0) ? -1 : ((event->data.encoder.direction < 0) ? 1 : 0);
            } else {
                int kv = event->data.key_value;
                if (kv == 13 || kv == 10) activate = true;
                else if (kv == 27 || kv == LV_KEY_ESC || kv == '`') back = true;
                else if (kv == ',' || kv == ';' || kv == LV_KEY_LEFT) dir = -1;
                else if (kv == '.' || kv == '/' || kv == LV_KEY_RIGHT) dir = 1;
            }

            if (back || event->type == INPUT_TYPE_EXIT_BUTTON) {
                subghz_capture_back_btn_cb(NULL);
                return;
            }

            if (dir != 0) {
                int selected = ((int)s_capture_popup_selected + dir + SUBGHZ_CAPTURE_POPUP_COUNT) % SUBGHZ_CAPTURE_POPUP_COUNT;
                s_capture_popup_selected = (subghz_capture_popup_control_t)selected;
                subghz_capture_popup_update_buttons();
                return;
            }

            if (activate) {
                if (s_capture_popup_selected == SUBGHZ_CAPTURE_POPUP_SAVE) subghz_capture_primary_btn_cb(NULL);
                else if (s_capture_popup_selected == SUBGHZ_CAPTURE_POPUP_FREQ) subghz_capture_freq_btn_cb(NULL);
                else subghz_capture_back_btn_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            subghz_capture_back_btn_cb(NULL);
            return;
        }

        return;
    }

    if (s_saved_popup && lv_obj_is_valid(s_saved_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_REL) {
                if (point_inside_obj(s_saved_replay_btn, d->point.x, d->point.y)) {
                    subghz_saved_replay_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_saved_delete_btn, d->point.x, d->point.y)) {
                    subghz_saved_delete_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_saved_back_btn, d->point.x, d->point.y)) {
                    subghz_saved_back_btn_cb(NULL);
                    return;
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER ||
                   event->type == INPUT_TYPE_KEYBOARD) {
            int dir = 0;
            bool activate = false;
            bool back = false;

            if (event->type == INPUT_TYPE_JOYSTICK) {
                int ji = event->data.joystick_index;
                if (ji == 0) dir = -1;
                else if (ji == 3) dir = 1;
                else if (ji == 1) activate = true;
            } else if (event->type == INPUT_TYPE_ENCODER) {
                if (event->data.encoder.button) activate = true;
                else dir = (event->data.encoder.direction > 0) ? -1 : ((event->data.encoder.direction < 0) ? 1 : 0);
            } else {
                int kv = event->data.key_value;
                if (kv == 13 || kv == 10) activate = true;
                else if (kv == 27 || kv == LV_KEY_ESC || kv == '`') back = true;
                else if (kv == ',' || kv == ';' || kv == LV_KEY_LEFT) dir = -1;
                else if (kv == '.' || kv == '/' || kv == LV_KEY_RIGHT) dir = 1;
            }

            if (back || event->type == INPUT_TYPE_EXIT_BUTTON) {
                subghz_saved_back_btn_cb(NULL);
                return;
            }

            if (dir != 0) {
                int selected = ((int)s_saved_popup_selected + dir + SUBGHZ_SAVED_POPUP_COUNT) % SUBGHZ_SAVED_POPUP_COUNT;
                s_saved_popup_selected = (subghz_saved_popup_control_t)selected;
                subghz_saved_popup_update_buttons();
                return;
            }

            if (activate) {
                if (s_saved_popup_selected == SUBGHZ_SAVED_POPUP_REPLAY) subghz_saved_replay_btn_cb(NULL);
                else if (s_saved_popup_selected == SUBGHZ_SAVED_POPUP_DELETE) subghz_saved_delete_btn_cb(NULL);
                else subghz_saved_back_btn_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            subghz_saved_back_btn_cb(NULL);
            return;
        }

        return;
    }

    if (s_fa_popup && lv_obj_is_valid(s_fa_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_REL) {
                if (point_inside_obj(s_fa_start_btn, d->point.x, d->point.y)) {
                    subghz_fa_start_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_fa_stop_btn, d->point.x, d->point.y)) {
                    subghz_fa_stop_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_fa_back_btn, d->point.x, d->point.y)) {
                    subghz_fa_back_btn_cb(NULL);
                    return;
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER ||
                   event->type == INPUT_TYPE_KEYBOARD) {
            int dir = 0;
            bool activate = false;
            bool back = false;

            if (event->type == INPUT_TYPE_JOYSTICK) {
                int ji = event->data.joystick_index;
                if (ji == 0) dir = -1;
                else if (ji == 3) dir = 1;
                else if (ji == 1) activate = true;
            } else if (event->type == INPUT_TYPE_ENCODER) {
                if (event->data.encoder.button) activate = true;
                else dir = (event->data.encoder.direction > 0) ? -1 : ((event->data.encoder.direction < 0) ? 1 : 0);
            } else {
                int kv = event->data.key_value;
                if (kv == 13 || kv == 10) activate = true;
                else if (kv == 27 || kv == LV_KEY_ESC || kv == '`') back = true;
                else if (kv == ',' || kv == ';' || kv == LV_KEY_LEFT) dir = -1;
                else if (kv == '.' || kv == '/' || kv == LV_KEY_RIGHT) dir = 1;
            }

            if (back || event->type == INPUT_TYPE_EXIT_BUTTON) {
                subghz_fa_back_btn_cb(NULL);
                return;
            }

            if (dir != 0) {
                int selected = ((int)s_fa_popup_selected + dir + SUBGHZ_FA_POPUP_COUNT) % SUBGHZ_FA_POPUP_COUNT;
                s_fa_popup_selected = (subghz_fa_popup_control_t)selected;
                subghz_fa_apply_popup_selection();
                return;
            }

            if (activate) {
                if (s_fa_popup_selected == SUBGHZ_FA_POPUP_START) subghz_fa_start_btn_cb(NULL);
                else if (s_fa_popup_selected == SUBGHZ_FA_POPUP_STOP) subghz_fa_stop_btn_cb(NULL);
                else subghz_fa_back_btn_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            subghz_fa_back_btn_cb(NULL);
            return;
        }

        return;
    }

    if (s_wf_popup && lv_obj_is_valid(s_wf_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_REL) {
                if (point_inside_obj(s_wf_start_btn, d->point.x, d->point.y)) {
                    subghz_wf_start_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_wf_stop_btn, d->point.x, d->point.y)) {
                    subghz_wf_stop_btn_cb(NULL);
                    return;
                }
                if (point_inside_obj(s_wf_back_btn, d->point.x, d->point.y)) {
                    subghz_wf_back_btn_cb(NULL);
                    return;
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER ||
                   event->type == INPUT_TYPE_KEYBOARD) {
            int dir = 0;
            bool activate = false;
            bool back = false;

            if (event->type == INPUT_TYPE_JOYSTICK) {
                int ji = event->data.joystick_index;
                if (ji == 0) dir = -1;
                else if (ji == 3) dir = 1;
                else if (ji == 1) activate = true;
            } else if (event->type == INPUT_TYPE_ENCODER) {
                if (event->data.encoder.button) activate = true;
                else dir = (event->data.encoder.direction > 0) ? -1 : ((event->data.encoder.direction < 0) ? 1 : 0);
            } else {
                int kv = event->data.key_value;
                if (kv == 13 || kv == 10) activate = true;
                else if (kv == 27 || kv == LV_KEY_ESC || kv == '`') back = true;
                else if (kv == ',' || kv == ';' || kv == LV_KEY_LEFT) dir = -1;
                else if (kv == '.' || kv == '/' || kv == LV_KEY_RIGHT) dir = 1;
            }

            if (back || event->type == INPUT_TYPE_EXIT_BUTTON) {
                subghz_wf_back_btn_cb(NULL);
                return;
            }

            if (dir != 0) {
                int selected = ((int)s_wf_popup_selected + dir + SUBGHZ_WF_POPUP_COUNT) % SUBGHZ_WF_POPUP_COUNT;
                s_wf_popup_selected = (subghz_wf_popup_control_t)selected;
                subghz_wf_apply_popup_selection();
                return;
            }

            if (activate) {
                if (s_wf_popup_selected == SUBGHZ_WF_POPUP_START) subghz_wf_start_btn_cb(NULL);
                else if (s_wf_popup_selected == SUBGHZ_WF_POPUP_STOP) subghz_wf_stop_btn_cb(NULL);
                else subghz_wf_back_btn_cb(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            subghz_wf_back_btn_cb(NULL);
            return;
        }

        return;
    }

    if (s_popup && lv_obj_is_valid(s_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *d = &event->data.touch_data;
            if (d->state == LV_INDEV_STATE_REL) {
                if (point_inside_obj(s_start_btn, d->point.x, d->point.y)) {
                    subghz_start_scan();
                    return;
                }
                if (point_inside_obj(s_stop_btn, d->point.x, d->point.y)) {
                    subghz_stop_scan();
                    return;
                }
                if (point_inside_obj(s_popup_back_btn, d->point.x, d->point.y)) {
                    subghz_close_popup(true);
                    return;
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK) {
            int ji = event->data.joystick_index;
            if (ji == 0) {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + SUBGHZ_POPUP_COUNT - 1) % SUBGHZ_POPUP_COUNT);
                subghz_apply_popup_selection();
            } else if (ji == 3) {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + 1) % SUBGHZ_POPUP_COUNT);
                subghz_apply_popup_selection();
            } else if (ji == 1) {
                if (s_popup_selected == SUBGHZ_POPUP_START) subghz_start_scan();
                else if (s_popup_selected == SUBGHZ_POPUP_STOP) subghz_stop_scan();
                else subghz_close_popup(true);
            }
            return;
        } else if (event->type == INPUT_TYPE_ENCODER) {
            if (event->data.encoder.button) {
                if (s_popup_selected == SUBGHZ_POPUP_START) subghz_start_scan();
                else if (s_popup_selected == SUBGHZ_POPUP_STOP) subghz_stop_scan();
                else subghz_close_popup(true);
                return;
            }

            if (event->data.encoder.direction > 0) {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + SUBGHZ_POPUP_COUNT - 1) % SUBGHZ_POPUP_COUNT);
            } else if (event->data.encoder.direction < 0) {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + 1) % SUBGHZ_POPUP_COUNT);
            }
            subghz_apply_popup_selection();
            return;
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            int kv = event->data.key_value;
            if (kv == 13 || kv == 10) {
                if (s_popup_selected == SUBGHZ_POPUP_START) subghz_start_scan();
                else if (s_popup_selected == SUBGHZ_POPUP_STOP) subghz_stop_scan();
                else subghz_close_popup(true);
                return;
            }
            if (kv == 27 || kv == 'c' || kv == 'C' || kv == '`') {
                subghz_close_popup(true);
                return;
            }
            if (kv == 's' || kv == 'S') {
                subghz_start_scan();
                return;
            }
            if (kv == 'x' || kv == 'X') {
                subghz_stop_scan();
                return;
            }
            if (kv == 44 || kv == ',' || kv == ';') {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + SUBGHZ_POPUP_COUNT - 1) % SUBGHZ_POPUP_COUNT);
                subghz_apply_popup_selection();
                return;
            }
            if (kv == 47 || kv == '/' || kv == '.') {
                s_popup_selected = (subghz_popup_control_t)((s_popup_selected + 1) % SUBGHZ_POPUP_COUNT);
                subghz_apply_popup_selection();
                return;
            }
        } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
            subghz_close_popup(true);
            return;
        }

        return;
    }

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        
#ifdef CONFIG_USE_TOUCHSCREEN
        if (d->state == LV_INDEV_STATE_PR) {
            if (gui_touch_bar_hit(s_scroll_up_btn, d->point.x, d->point.y)) {
                options_view_move_selection(s_ov, -1);
                touch_drag_reset(&s_touch_drag);
                return;
            }

            if (gui_touch_bar_hit(s_scroll_down_btn, d->point.x, d->point.y)) {
                options_view_move_selection(s_ov, 1);
                touch_drag_reset(&s_touch_drag);
                return;
            }

            if (gui_touch_bar_hit(s_back_btn, d->point.x, d->point.y)) {
                subghz_back_btn_cb(NULL);
                touch_drag_reset(&s_touch_drag);
                return;
            }

            if (!s_touch_drag.started) {
                touch_drag_begin(&s_touch_drag, d);
            } else {
                // Move event - apply live drag or remember target for release
                lv_obj_t *list = options_view_get_list(s_ov);
                if (list && lv_obj_is_valid(list)) {
                    lv_area_t list_area;
                    lv_obj_get_coords(list, &list_area);
                    bool started_in_list = (s_touch_drag.start_x >= list_area.x1 && s_touch_drag.start_x <= list_area.x2 &&
                                                 s_touch_drag.start_y >= list_area.y1 && s_touch_drag.start_y <= list_area.y2);
                    if (started_in_list) {
                        touch_drag_update(&s_touch_drag, d, list);
                    }
                }
            }
            return;
        }

        if (d->state == LV_INDEV_STATE_REL) {
            if (!s_touch_drag.started) return;

            int start_x = s_touch_drag.start_x;
            int start_y = s_touch_drag.start_y;
            int dx = (int)d->point.x - start_x;

            int thr_x = LV_HOR_RES / SUBGHZ_SWIPE_THRESHOLD_RATIO;

            // Let the shared touch_drag helper handle release-on-release
            // (it applies a single scroll when the live setting is off) and
            // tell us if a drag was in progress so we can skip tap handling.
            bool was_dragged = touch_drag_release(&s_touch_drag, d);
            if (was_dragged) return;

            lv_obj_t *list = options_view_get_list(s_ov);
            if (list && lv_obj_is_valid(list)) {
                lv_area_t list_area;
                lv_obj_get_coords(list, &list_area);
                bool started_in_list = (start_x >= list_area.x1 && start_x <= list_area.x2 &&
                                             start_y >= list_area.y1 && start_y <= list_area.y2);

                if (started_in_list) {
                    if (abs(dx) > thr_x) return;

                    if (settings_get_thirds_control_enabled(&G_Settings)) {
                        int list_h = (int)(list_area.y2 - list_area.y1);
                        if (list_h > 0) {
                            int y_rel = (int)d->point.y - (int)list_area.y1;
                            if (y_rel < list_h / 3) {
                                options_view_move_selection(s_ov, -1);
                                return;
                            } else if (y_rel > (list_h * 2) / 3) {
                                options_view_move_selection(s_ov, 1);
                                return;
                            }
                        }
                    }
                }
            }
        }
#endif

        if (d->state == LV_INDEV_STATE_REL) {
            if (s_in_saved_list && s_ov) {
                lv_obj_t *list = options_view_get_list(s_ov);
                if (list && lv_obj_is_valid(list)) {
                    int cnt = options_view_get_item_count(s_ov);
                    for (int i = 0; i < cnt; ++i) {
                        lv_obj_t *btn = lv_obj_get_child(list, i);
                        if (!btn || !lv_obj_is_valid(btn)) continue;
                        lv_area_t a;
                        lv_obj_get_coords(btn, &a);
                        if (d->point.x >= a.x1 && d->point.x <= a.x2 && d->point.y >= a.y1 && d->point.y <= a.y2) {
                            options_view_set_selected(s_ov, i);
                            lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                            return;
                        }
                    }
                }
            }
            if (!s_in_saved_list && point_inside_obj(s_scan_row, d->point.x, d->point.y)) {
                if (s_ov) options_view_set_selected(s_ov, 0);
                s_capture_mode = SUBGHZ_CAPTURE_MODE_NORMAL;
                subghz_capture_snapshot_action();
                return;
            }
            if (!s_in_saved_list && s_raw_capture_row && point_inside_obj(s_raw_capture_row, d->point.x, d->point.y)) {
                if (s_ov) options_view_set_selected(s_ov, 1);
                s_capture_mode = SUBGHZ_CAPTURE_MODE_RAW;
                subghz_capture_snapshot_action();
                return;
            }
            if (!s_in_saved_list && point_inside_obj(s_capture_row, d->point.x, d->point.y)) {
                if (s_ov) options_view_set_selected(s_ov, 2);
                subghz_list_snapshots_action();
                return;
            }
            if (!s_in_saved_list && s_freq_analyzer_row && point_inside_obj(s_freq_analyzer_row, d->point.x, d->point.y)) {
                if (s_ov) options_view_set_selected(s_ov, 3);
                subghz_open_freq_analyzer_popup();
                return;
            }
            if (!s_in_saved_list && s_waterfall_row && point_inside_obj(s_waterfall_row, d->point.x, d->point.y)) {
                if (s_ov) options_view_set_selected(s_ov, 4);
                subghz_open_waterfall_popup();
                return;
            }
            if (point_inside_obj(s_back_row, d->point.x, d->point.y)) {
                if (s_in_saved_list) {
                    subghz_back_to_root_menu();
                } else {
                    display_manager_go_back();
                }
                return;
            }
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int ji = event->data.joystick_index;
        if (ji == 2) {
            options_view_move_selection(s_ov, -1);
        } else if (ji == 4) {
            options_view_move_selection(s_ov, 1);
        } else if (ji == 1) {
            subghz_select_row();
        } else if (ji == 0) {
            if (s_in_saved_list) subghz_back_to_root_menu();
            else display_manager_go_back();
        }
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            subghz_select_row();
            return;
        }
        if (event->data.encoder.direction > 0) {
            options_view_move_selection(s_ov, -1);
        } else if (event->data.encoder.direction < 0) {
            options_view_move_selection(s_ov, 1);
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == LV_KEY_UP || kv == 'k' || kv == ';') {
            options_view_move_selection(s_ov, -1);
        } else if (kv == LV_KEY_DOWN || kv == 'j' || kv == '.') {
            options_view_move_selection(s_ov, 1);
        } else if (kv == 13 || kv == 10 || kv == LV_KEY_RIGHT || kv == '/') {
            subghz_select_row();
        } else if (kv == LV_KEY_ESC || kv == 27 || kv == LV_KEY_LEFT || kv == ',' || kv == '`') {
            if (s_in_saved_list) subghz_back_to_root_menu();
            else display_manager_go_back();
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (s_in_saved_list) subghz_back_to_root_menu();
        else display_manager_go_back();
    }
}

// Deep-link support for favorites: open a specific saved capture. Safe to
// call before the view exists - consumed by subghz_view_create(). Heap-backed
// so it does not burn 256 B of internal RAM on boards without PSRAM.
static char *s_pending_capture_open = NULL;
static void subghz_view_apply_pending_open(void);

void subghz_view_open_capture(const char *path) {
    if (!path || !path[0]) return;
    free(s_pending_capture_open);
    s_pending_capture_open = strdup(path);
    // View already live (e.g. lockscreen overlay on top of it): apply now.
    if (s_ov && lv_obj_is_valid(s_ov)) {
        subghz_view_apply_pending_open();
    }
}

static void subghz_view_apply_pending_open(void) {
    if (!s_pending_capture_open) return;
    char path[256];
    strncpy(path, s_pending_capture_open, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(s_pending_capture_open);
    s_pending_capture_open = NULL;
    s_saved_page = 0;
    subghz_saved_list_reload();
    for (int i = 0; i < s_saved_file_count; i++) {
        if (s_saved_file_paths[i] && strcmp(s_saved_file_paths[i], path) == 0) {
            s_saved_index = i;
            break;
        }
    }
    subghz_open_saved_popup();
}

void subghz_view_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));

    s_root = gui_screen_create_root(NULL, NULL, bg, LV_OPA_TRANSP);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    subghz_view.root = s_root;

    s_ov = options_view_create(s_root, "SubGHz");
    lv_obj_t *list = options_view_get_list(s_ov);

#ifdef CONFIG_USE_TOUCHSCREEN
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_HEIGHT;
    const int TOUCH_BAR_HEIGHT = gui_touch_bar_height();
    int list_h = LV_VER_RES - STATUS_BAR_HEIGHT - TOUCH_BAR_HEIGHT;
    lv_obj_set_size(list, GUI_OPTIONS_LIST_WIDTH, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    s_scan_row = options_view_add_item(s_ov, "Capture", subghz_scan_row_cb, NULL);
    s_raw_capture_row = options_view_add_item(s_ov, "Raw Capture", subghz_raw_capture_row_cb, NULL);
    s_capture_row = options_view_add_item(s_ov, "Saved", subghz_capture_row_cb, NULL);
    s_freq_analyzer_row = options_view_add_item(s_ov, "Freq Analyzer", subghz_freq_analyzer_row_cb, NULL);
    s_waterfall_row = options_view_add_item(s_ov, "Waterfall", subghz_waterfall_row_cb, NULL);
    s_back_row = options_view_add_back_row(s_ov, subghz_back_row_cb, NULL);
    if (gui_router_previous_view() == &main_menu_view) {
        s_root_selected_index = 0;
    }
    int root_item_count = options_view_get_item_count(s_ov);
    if (s_root_selected_index < 0 || s_root_selected_index >= root_item_count) {
        s_root_selected_index = 0;
    }
    options_view_set_selected(s_ov, s_root_selected_index);

#ifdef CONFIG_USE_TOUCHSCREEN
    s_touch_tb = gui_touch_bar_create(s_root);
    s_scroll_up_btn = s_touch_tb.up_btn;
    s_back_btn = s_touch_tb.back_btn;
    s_scroll_down_btn = s_touch_tb.down_btn;
    if (s_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_touch_tb, subghz_scroll_up_cb, NULL, subghz_back_btn_cb, NULL, subghz_scroll_down_cb, NULL);
    }
#endif

    s_remote_mode = subghz_is_remote_mode();
    s_remote_stream_online = false;
    s_remote_error = false;
    s_remote_paused = true;
    s_in_saved_list = false;
    subghz_clear_pending_action();
    s_cursor = 0;
    memset(s_levels, 0, sizeof(s_levels));
    memset(s_peaks, 0, sizeof(s_peaks));

    s_timer = lv_timer_create(subghz_timer_cb, 33, NULL);

    // Consume any pending favorite deep-link (subghz_view_open_capture).
    subghz_view_apply_pending_open();
}

void subghz_view_destroy(void) {
    /* Captured before any of the teardown below touches s_ov/s_in_saved_list,
     * so create() can restore the highlighted root-menu row if we're
     * returning here rather than arriving fresh from the Main Menu. Only
     * meaningful if we were actually on the root menu, not inside the
     * saved-files list. */
    if (s_ov && !s_in_saved_list) {
        s_root_selected_index = options_view_get_selected(s_ov);
    }

    subghz_close_popup(true);
    subghz_close_capture_popup();
    subghz_close_freq_analyzer_popup();
    subghz_close_waterfall_popup();
    popup_confirm_close(&s_saved_delete_confirm);
    subghz_close_saved_popup();
    subghz_saved_list_clear();
    s_in_saved_list = false;

    lvgl_timer_del_safe(&s_timer);

    if (s_ov) {
        options_view_destroy(s_ov);
        s_ov = NULL;
    }

    s_scan_row = NULL;
    s_capture_row = NULL;
    s_raw_capture_row = NULL;
    s_freq_analyzer_row = NULL;
    s_waterfall_row = NULL;
    s_back_row = NULL;

#ifdef CONFIG_USE_TOUCHSCREEN
    gui_touch_bar_destroy(&s_touch_tb);
    s_scroll_up_btn = NULL;
    s_scroll_down_btn = NULL;
    s_back_btn = NULL;
    touch_drag_reset(&s_touch_drag);
#endif

    lvgl_obj_del_safe(&s_root);
    subghz_view.root = NULL;

    memset(s_levels, 0, sizeof(s_levels));
    memset(s_peaks, 0, sizeof(s_peaks));
    s_cursor = 0;
    s_remote_stream_online = false;
    s_remote_error = false;
    s_remote_paused = true;
    subghz_clear_pending_action();
}

static void get_subghz_callback(void **callback) {
    if (callback) {
        *callback = subghz_view.input_callback;
    }
}

View subghz_view = {
    .root = NULL,
    .create = subghz_view_create,
    .destroy = subghz_view_destroy,
    .input_callback = subghz_input_handler,
    .name = "SubGHz",
    .get_hardwareinput_callback = get_subghz_callback
};

#endif
