#include "managers/views/ghostscript_runner_view.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"

#include "core/glog.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/toast.h"
#include "gui/asset_pack.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/ghostscript_manager.h"
#include "managers/ghostscript_runtime.h"
#include "managers/views/ghostscript_browser_view.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GS_RUNNER_OUTPUT_BUF_SIZE 2048
#define GS_RUNNER_TICK_MS 100
#define GS_RUNNER_TASK_STACK 8192
#define GS_RUNNER_SCROLL_THRESHOLD 12
#define GS_RUNNER_TOUCH_BTN_SIZE 36
#define GS_RUNNER_TOUCH_BTN_PADDING 6
#define GS_RUNNER_TOUCH_BAR_HEIGHT (GS_RUNNER_TOUCH_BTN_SIZE + GS_RUNNER_TOUCH_BTN_PADDING * 2)
#define GS_RUNNER_TITLE_BUF_SIZE 64
#define GS_RUNNER_STATUS_BUF_SIZE 96

static char *s_pending_path;
static lv_obj_t *s_root;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_output_scroll;
static lv_obj_t *s_output;
static lv_obj_t *s_touch_bar;
static lv_obj_t *s_stop_btn;
static gui_touch_bar_t s_touch_tb;
static lv_timer_t *s_launch_timer;
static TaskHandle_t s_script_task;
static char *s_output_buf;
static SemaphoreHandle_t s_output_mutex;
static ghostscript_runtime_t *s_rt;
static SemaphoreHandle_t s_runtime_mutex;
static SemaphoreHandle_t s_lifecycle_mutex;
static volatile bool s_runner_visible;
static volatile bool s_relaunch_pending;
static bool s_touch_started;
static bool s_touch_scrolling;
static bool s_follow_output;
static lv_point_t s_touch_start;
static lv_point_t s_touch_last;

typedef struct {
    char path[GHOSTSCRIPT_PATH_MAX];
} script_task_args_t;

static void runner_set_title(const char *title, void *user);
static void runner_set_status(const char *status);
static void runner_print(const char *text, void *user);
static void launch_now(void);
static void launch_when_ready(void *arg);

bool ghostscript_runner_stop_script(void) {
    if (!s_lifecycle_mutex) return false;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!s_runtime_mutex) {
        xSemaphoreGive(s_lifecycle_mutex);
        return false;
    }
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    bool active = s_rt && ghostscript_runtime_state(s_rt) == GHOSTSCRIPT_STATE_RUNNING;
    if (active) ghostscript_runtime_stop(s_rt);
    xSemaphoreGive(s_runtime_mutex);
    xSemaphoreGive(s_lifecycle_mutex);
    return active;
}

bool ghostscript_runner_is_script_active(void) {
    if (!s_lifecycle_mutex) return false;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    bool active = s_script_task != NULL;
    xSemaphoreGive(s_lifecycle_mutex);
    return active;
}

static void script_task_fn(void *arg) {
    script_task_args_t *task_args = (script_task_args_t *)arg;
    ghostscript_manifest_t manifest;
    bool ok;
    if (!task_args || task_args->path[0] == '\0') {
        runner_print("No script selected\n", NULL);
        goto done;
    }
    if (ghostscript_manager_is_script_file(task_args->path)) {
        ok = ghostscript_manager_make_single_file_manifest(task_args->path, &manifest);
        if (!ok) ok = ghostscript_manager_load_manifest(task_args->path, &manifest);
    } else {
        ok = ghostscript_manager_load_manifest(task_args->path, &manifest);
    }
    free(task_args);
    task_args = NULL;
    if (!ok) {
        runner_set_title("Script Load Failed", NULL);
        runner_print(ghostscript_manager_last_error(), NULL);
        runner_print("\n", NULL);
        toast_show_duration("Script load failed", TOAST_ERROR, 2200);
        goto done;
    }
    runner_set_title(manifest.name, NULL);
    ghostscript_runtime_hooks_t hooks = { .print = runner_print, .set_title = runner_set_title };
    ghostscript_runtime_t *rt = ghostscript_runtime_create(&manifest, &hooks);
    if (!rt) {
        runner_print("Failed to create runtime\n", NULL);
        goto done;
    }
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    s_rt = rt;
    xSemaphoreGive(s_runtime_mutex);
    char msg[96];
    snprintf(msg, sizeof(msg), "Lua heap limit: %lu bytes\n", (unsigned long)manifest.memory_limit);
    runner_print(msg, NULL);
    snprintf(msg, sizeof(msg), "Starting | heap 0/%lu", (unsigned long)manifest.memory_limit);
    runner_set_status(msg);
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    bool started = ghostscript_runtime_start(s_rt);
    ghostscript_state_t state = ghostscript_runtime_state(s_rt);
    xSemaphoreGive(s_runtime_mutex);
    bool final_handled = false;
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    if (!started && state == GHOSTSCRIPT_STATE_FAILED) {
        runner_set_status("Failed");
        runner_print("Error: ", NULL);
        runner_print(ghostscript_runtime_error(s_rt), NULL);
        snprintf(msg, sizeof(msg), "\nLua heap: %lu/%lu bytes (peak %lu)\n", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_limit(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
        runner_print(msg, NULL);
        ghostscript_manager_record_failure(ghostscript_runtime_manifest(s_rt), ghostscript_runtime_error(s_rt));
        toast_show_duration("GhostScript failed", TOAST_ERROR, 2200);
        final_handled = true;
    } else if (state == GHOSTSCRIPT_STATE_DONE) {
        snprintf(msg, sizeof(msg), "Done | heap %lu/%lu peak %lu", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_limit(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
        runner_set_status(msg);
        snprintf(msg, sizeof(msg), "\nDone. Lua heap used: %lu bytes (peak %lu)\n", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
        runner_print(msg, NULL);
        ghostscript_manager_record_clean_exit(ghostscript_runtime_manifest(s_rt));
        final_handled = true;
    }
    xSemaphoreGive(s_runtime_mutex);
    while (true) {
        xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
        if (!s_rt || ghostscript_runtime_state(s_rt) != GHOSTSCRIPT_STATE_RUNNING) {
            xSemaphoreGive(s_runtime_mutex);
            break;
        }
        ghostscript_runtime_tick(s_rt, GS_RUNNER_TICK_MS);
        snprintf(msg, sizeof(msg), "Running | heap %lu/%lu", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_limit(s_rt));
        xSemaphoreGive(s_runtime_mutex);
        runner_set_status(msg);
        vTaskDelay(pdMS_TO_TICKS(GS_RUNNER_TICK_MS));
    }
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    if (s_rt && !final_handled) {
        state = ghostscript_runtime_state(s_rt);
        if (state == GHOSTSCRIPT_STATE_FAILED) {
            runner_set_status("Failed");
            runner_print("Error: ", NULL);
            runner_print(ghostscript_runtime_error(s_rt), NULL);
            snprintf(msg, sizeof(msg), "\nLua heap: %lu/%lu bytes (peak %lu)\n", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_limit(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
            runner_print(msg, NULL);
            ghostscript_manager_record_failure(ghostscript_runtime_manifest(s_rt), ghostscript_runtime_error(s_rt));
            toast_show_duration("GhostScript failed", TOAST_ERROR, 2200);
        } else if (state == GHOSTSCRIPT_STATE_DONE) {
            snprintf(msg, sizeof(msg), "Done | heap %lu/%lu peak %lu", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_limit(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
            runner_set_status(msg);
            snprintf(msg, sizeof(msg), "\nDone. Lua heap used: %lu bytes (peak %lu)\n", (unsigned long)ghostscript_runtime_memory_used(s_rt), (unsigned long)ghostscript_runtime_memory_peak(s_rt));
            runner_print(msg, NULL);
            ghostscript_manager_record_clean_exit(ghostscript_runtime_manifest(s_rt));
        }
    }
    xSemaphoreGive(s_runtime_mutex);
done:
    free(task_args);
    xSemaphoreTake(s_runtime_mutex, portMAX_DELAY);
    if (s_rt) {
        ghostscript_runtime_t *rt = s_rt;
        s_rt = NULL;
        ghostscript_runtime_destroy(rt);
    }
    xSemaphoreGive(s_runtime_mutex);
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    bool relaunch = s_runner_visible && s_relaunch_pending;
    if (!s_runner_visible) {
        if (s_output_mutex && xSemaphoreTake(s_output_mutex, portMAX_DELAY) == pdTRUE) {
            free(s_output_buf);
            s_output_buf = NULL;
            xSemaphoreGive(s_output_mutex);
            vSemaphoreDelete(s_output_mutex);
            s_output_mutex = NULL;
        }
        if (s_runtime_mutex) {
            vSemaphoreDelete(s_runtime_mutex);
            s_runtime_mutex = NULL;
        }
    }
    free(task_args);
    s_script_task = NULL;
    xSemaphoreGive(s_lifecycle_mutex);
    if (relaunch) {
        display_manager_run_on_lvgl(launch_when_ready, NULL);
    }
    vTaskDeleteWithCaps(NULL);
}

static volatile int s_ui_dirty;
static char *s_title_buf;
static char *s_status_buf;
static lv_timer_t *s_flush_timer;

static void flush_ui(void *arg) {
    (void)arg;
    s_ui_dirty = 0;
    if (s_title && lv_obj_is_valid(s_title) && s_title_buf && s_title_buf[0])
        lv_label_set_text(s_title, s_title_buf);
    if (s_status && lv_obj_is_valid(s_status) && s_status_buf && s_status_buf[0])
        lv_label_set_text(s_status, s_status_buf);
    if (s_output && lv_obj_is_valid(s_output) && s_output_buf) {
        if (s_output_mutex && xSemaphoreTake(s_output_mutex, 0) == pdTRUE) {
            bool follow_output = s_output_scroll && lv_obj_is_valid(s_output_scroll) &&
                                 s_follow_output && !s_touch_scrolling;
            lv_label_set_text(s_output, s_output_buf);
            if (follow_output)
                lv_obj_scroll_to_y(s_output_scroll, LV_COORD_MAX, LV_ANIM_OFF);
            xSemaphoreGive(s_output_mutex);
        }
    }
}

/* Periodic flush: 5 Hz. Ensures the title and last output line stay live
 * even when the script doesn't print anything. */
static void flush_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_title && lv_obj_is_valid(s_title) && s_title_buf && s_title_buf[0]) {
        /* Re-set text only if it differs; LVGL won't redraw otherwise. */
        const char *cur = lv_label_get_text(s_title);
        if (!cur || strcmp(cur, s_title_buf) != 0)
            lv_label_set_text(s_title, s_title_buf);
    }
    if (s_status && lv_obj_is_valid(s_status) && s_status_buf && s_status_buf[0]) {
        const char *cur = lv_label_get_text(s_status);
        if (!cur || strcmp(cur, s_status_buf) != 0)
            lv_label_set_text(s_status, s_status_buf);
    }
    if (s_output && lv_obj_is_valid(s_output) && s_output_buf) {
        if (s_output_mutex && xSemaphoreTake(s_output_mutex, 0) == pdTRUE) {
            const char *cur = lv_label_get_text(s_output);
            if (!cur || strcmp(cur, s_output_buf) != 0) {
                bool follow_output = s_output_scroll && lv_obj_is_valid(s_output_scroll) &&
                                     s_follow_output && !s_touch_scrolling;
                lv_label_set_text(s_output, s_output_buf);
                if (follow_output)
                    lv_obj_scroll_to_y(s_output_scroll, LV_COORD_MAX, LV_ANIM_OFF);
            }
            xSemaphoreGive(s_output_mutex);
        }
    }
}

static void runner_set_title(const char *title, void *user) {
    (void)user;
    if (!s_title_buf) return;
    snprintf(s_title_buf, GS_RUNNER_TITLE_BUF_SIZE, "%s", title ? title : "GhostScript");
    if (__sync_lock_test_and_set(&s_ui_dirty, 1) == 0)
        display_manager_run_on_lvgl(flush_ui, NULL);
}

static void runner_set_status(const char *status) {
    if (!s_status_buf) return;
    snprintf(s_status_buf, GS_RUNNER_STATUS_BUF_SIZE, "%s", status ? status : "Idle");
    if (__sync_lock_test_and_set(&s_ui_dirty, 1) == 0)
        display_manager_run_on_lvgl(flush_ui, NULL);
}

static void runner_print(const char *text, void *user) {
    (void)user;
    if (!s_output_buf || !s_output_mutex || !text) return;
    if (xSemaphoreTake(s_output_mutex, portMAX_DELAY) != pdTRUE) return;
    size_t cur = strlen(s_output_buf);
    size_t add = strlen(text);
    if (cur + add + 1 >= GS_RUNNER_OUTPUT_BUF_SIZE) {
        size_t keep = GS_RUNNER_OUTPUT_BUF_SIZE / 2;
        char *src = s_output_buf + (cur > keep ? cur - keep : 0);
        memmove(s_output_buf, src, strlen(src) + 1);
        cur = strlen(s_output_buf);
        if (cur + add + 1 >= GS_RUNNER_OUTPUT_BUF_SIZE) {
            size_t trim = (cur + add + 1) - GS_RUNNER_OUTPUT_BUF_SIZE;
            if (trim > cur) trim = cur;
            memmove(s_output_buf, s_output_buf + trim, cur - trim + 1);
            cur = strlen(s_output_buf);
        }
    }
    snprintf(s_output_buf + cur, GS_RUNNER_OUTPUT_BUF_SIZE - cur, "%s", text);
    xSemaphoreGive(s_output_mutex);
    glog("[GhostScript] %s", text);
    if (__sync_lock_test_and_set(&s_ui_dirty, 1) == 0)
        display_manager_run_on_lvgl(flush_ui, NULL);
}

static void scroll_output_by(lv_coord_t dy, lv_anim_enable_t anim) {
    if (!s_output_scroll || !lv_obj_is_valid(s_output_scroll) || dy == 0) return;
    lv_obj_scroll_by_bounded(s_output_scroll, 0, dy, anim);
    lv_obj_update_layout(s_output_scroll);
    s_follow_output = lv_obj_get_scroll_bottom(s_output_scroll) <= 4;
}

static void scroll_output_page(int dir) {
    if (!s_output_scroll || !lv_obj_is_valid(s_output_scroll)) return;
    lv_coord_t step = lv_obj_get_height(s_output_scroll) / 2;
    if (step < 24) step = 24;
    scroll_output_by(dir > 0 ? -step : step, LV_ANIM_OFF);
}

static void back_to_browser(void) {
    /* Reachable both from the script browser and directly via a CLI
     * "script run" command, so back must return to whichever actually
     * opened this, not always the browser. */
    display_manager_go_back();
}

static void stop_script(void) {
    if (ghostscript_runner_stop_script()) {
        runner_set_status("Stopped");
        runner_print("\nStopped by user.\n", NULL);
        toast_show_duration("GhostScript stopped", TOAST_INFO, 1200);
    }
}

static void touch_up_cb(lv_event_t *e) {
    (void)e;
    scroll_output_page(-1);
}

static void touch_down_cb(lv_event_t *e) {
    (void)e;
    scroll_output_page(1);
}

static void touch_back_cb(lv_event_t *e) {
    (void)e;
    back_to_browser();
}

static void touch_stop_cb(lv_event_t *e) {
    (void)e;
    stop_script();
}

static void dispatch_touch_bar_press(const lv_indev_data_t *data) {
    if (!data) return;
    int tx = data->point.x;
    int ty = data->point.y;
    bool hit_up = false, hit_back = false, hit_down = false;
    gui_touch_bar_hit_test(&s_touch_tb, tx, ty, &hit_up, &hit_back, &hit_down);
    if (hit_up) { scroll_output_page(-1); return; }
    if (hit_down) { scroll_output_page(1); return; }
    if (hit_back) { back_to_browser(); return; }
    if (gui_touch_bar_hit(s_stop_btn, tx, ty)) { stop_script(); return; }
}

void ghostscript_runner_set_script(const char *path) {
    char *pending_path = path ? strndup(path, GHOSTSCRIPT_PATH_MAX - 1) : NULL;
    free(s_pending_path);
    s_pending_path = pending_path;
}

static void launch_now(void) {
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (s_script_task) {
        s_relaunch_pending = true;
        xSemaphoreGive(s_lifecycle_mutex);
        return;
    }
    s_relaunch_pending = false;
    script_task_args_t *task_args = calloc(1, sizeof(*task_args));
    if (!task_args) {
        runner_set_title("Script Launch Failed", NULL);
        runner_print("Failed to allocate script task\n", NULL);
        xSemaphoreGive(s_lifecycle_mutex);
        return;
    }
    snprintf(task_args->path, sizeof(task_args->path), "%s", s_pending_path ? s_pending_path : "");
    BaseType_t ok = xTaskCreateWithCaps(script_task_fn, "gs_script", GS_RUNNER_TASK_STACK,
                                        task_args, 5, &s_script_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(script_task_fn, "gs_script", GS_RUNNER_TASK_STACK,
                                 task_args, 5, &s_script_task,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        free(task_args);
        runner_set_title("Script Launch Failed", NULL);
        runner_print("Failed to create script task\n", NULL);
        toast_show_duration("GhostScript launch failed", TOAST_ERROR, 2200);
    }
    xSemaphoreGive(s_lifecycle_mutex);
}

static void launch_when_ready(void *arg) {
    (void)arg;
    if (s_runner_visible && s_relaunch_pending) launch_now();
}

static void launch_cb(lv_timer_t *timer) {
    (void)timer;
    s_launch_timer = NULL;
    launch_now();
}

static bool is_back_event(InputEvent *event) {
    if (event->type == INPUT_TYPE_EXIT_BUTTON) return true;
    if (event->type == INPUT_TYPE_KEYBOARD && (event->data.key_value == LV_KEY_ESC || event->data.key_value == 29 || event->data.key_value == '`')) return true;
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed && event->data.joystick_index == 0) return true;
    return false;
}

static bool is_touch_in_touch_bar(const lv_indev_data_t *data) {
#ifdef CONFIG_USE_TOUCHSCREEN
    if (!data || !s_touch_bar || !lv_obj_is_valid(s_touch_bar)) return false;
    lv_area_t bar_area;
    lv_obj_get_coords(s_touch_bar, &bar_area);
    return data->point.x >= bar_area.x1 && data->point.x <= bar_area.x2 &&
           data->point.y >= bar_area.y1 && data->point.y <= bar_area.y2;
#else
    (void)data;
    return false;
#endif
}

static bool handle_output_scroll(InputEvent *event) {
    if (!event || !s_output_scroll || !lv_obj_is_valid(s_output_scroll)) return false;
    if (event->type == INPUT_TYPE_TOUCH) {
        const lv_indev_data_t *data = &event->data.touch_data;
        if (is_touch_in_touch_bar(data)) return false;
        if (data->state == LV_INDEV_STATE_PR) {
            if (!s_touch_started) {
                s_touch_started = true;
                s_touch_scrolling = false;
                s_touch_start = data->point;
                s_touch_last = data->point;
                return false;
            }
            int total_dx = data->point.x - s_touch_start.x;
            int total_dy = data->point.y - s_touch_start.y;
            int dy = data->point.y - s_touch_last.y;
            s_touch_last = data->point;
            if ((s_touch_scrolling || (abs(total_dy) > GS_RUNNER_SCROLL_THRESHOLD && abs(total_dy) >= abs(total_dx))) && dy != 0) {
                s_touch_scrolling = true;
                scroll_output_by(dy, LV_ANIM_OFF);
                return true;
            }
            return false;
        }
        if (data->state != LV_INDEV_STATE_REL || !s_touch_started) return false;
        s_touch_started = false;
        if (s_touch_scrolling) {
            s_touch_scrolling = false;
            return true;
        }
        int dx = data->point.x - s_touch_start.x;
        int dy = data->point.y - s_touch_start.y;
        if (abs(dy) > GS_RUNNER_SCROLL_THRESHOLD && abs(dy) >= abs(dx)) {
            scroll_output_by(dy, LV_ANIM_OFF);
            return true;
        }
    } else if (event->type == INPUT_TYPE_ENCODER && !event->data.encoder.button) {
        scroll_output_page(event->data.encoder.direction > 0 ? 1 : -1);
        return true;
    } else if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_pressed) {
        if (event->data.joystick_index == 2) {
            scroll_output_page(-1);
            return true;
        }
        if (event->data.joystick_index == 4) {
            scroll_output_page(1);
            return true;
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        int kv = event->data.key_value;
        if (kv == LV_KEY_UP || kv == 'k' || kv == ';') {
            scroll_output_page(-1);
            return true;
        }
        if (kv == LV_KEY_DOWN || kv == 'j' || kv == '.') {
            scroll_output_page(1);
            return true;
        }
    }
    return false;
}

static void event_handler(InputEvent *event) {
    if (!event) return;
    if (is_back_event(event)) {
        back_to_browser();
        return;
    }
    if (event->type == INPUT_TYPE_KEYBOARD && (event->data.key_value == 's' || event->data.key_value == 'S')) {
        stop_script();
        return;
    }
    if (event->type == INPUT_TYPE_TOUCH && is_touch_in_touch_bar(&event->data.touch_data)) {
        if (event->data.touch_data.state == LV_INDEV_STATE_PR)
            dispatch_touch_bar_press(&event->data.touch_data);
        return;
    }
    if (handle_output_scroll(event)) return;
    if (s_runtime_mutex && xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_rt) ghostscript_runtime_input(s_rt, event);
        xSemaphoreGive(s_runtime_mutex);
    }
}

void ghostscript_runner_view_create(void) {
    /* The previous view has already been destroyed. Drop decoded backgrounds
     * and icons before allocating the runner and Lua runtime; they reload on
     * demand when the user returns to the browser or app menu. */
    gui_screen_invalidate_bg_cache();
    asset_pack_release_cached_images();
    if (!s_title_buf) s_title_buf = malloc(GS_RUNNER_TITLE_BUF_SIZE);
    if (!s_status_buf) s_status_buf = malloc(GS_RUNNER_STATUS_BUF_SIZE);
    if (!s_title_buf || !s_status_buf) return;
    if (!s_lifecycle_mutex) {
        s_lifecycle_mutex = xSemaphoreCreateMutex();
        if (!s_lifecycle_mutex) return;
    }
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (!s_output_buf) {
        s_output_buf = heap_caps_malloc_prefer(GS_RUNNER_OUTPUT_BUF_SIZE, 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_output_buf) { xSemaphoreGive(s_lifecycle_mutex); return; }
    }
    if (!s_output_mutex) {
        s_output_mutex = xSemaphoreCreateMutex();
        if (!s_output_mutex) {
            free(s_output_buf);
            s_output_buf = NULL;
            xSemaphoreGive(s_lifecycle_mutex);
            return;
        }
    }
    if (!s_runtime_mutex) {
        s_runtime_mutex = xSemaphoreCreateMutex();
        if (!s_runtime_mutex) {
            vSemaphoreDelete(s_output_mutex);
            s_output_mutex = NULL;
            free(s_output_buf);
            s_output_buf = NULL;
            xSemaphoreGive(s_lifecycle_mutex);
            return;
        }
    }
    s_runner_visible = true;
    xSemaphoreTake(s_output_mutex, portMAX_DELAY);
    s_output_buf[0] = '\0';
    xSemaphoreGive(s_output_mutex);
    xSemaphoreGive(s_lifecycle_mutex);
    s_touch_started = false;
    s_touch_scrolling = false;
    s_follow_output = true;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t background = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t muted = lv_color_hex(theme_palette_get_text_muted(theme));
    s_root = gui_screen_create_root(NULL, "GhostScript", background, LV_OPA_COVER);
    ghostscript_runner_view.root = s_root;
    display_manager_add_status_bar("GhostScript");
    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
#ifdef CONFIG_USE_TOUCHSCREEN
    /* Reserve the custom bar height only when the bar is actually shown;
     * otherwise the content would leave a gap on large-screen targets. */
    lv_coord_t content_h = LV_VER_RES - GUI_STATUS_BAR_HEIGHT -
        (gui_touch_bar_should_show() ? GS_RUNNER_TOUCH_BAR_HEIGHT : 0);
    if (content_h < 32) content_h = 32;
    lv_obj_set_height(content, content_h);
#endif
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    s_title = lv_label_create(content);
    snprintf(s_title_buf, GS_RUNNER_TITLE_BUF_SIZE, "Loading script...");
    lv_label_set_text(s_title, s_title_buf);
    lv_obj_set_style_text_color(s_title, text, 0);
    lv_obj_set_style_text_font(s_title, accessibility_get_font_title(), 0);
    s_status = lv_label_create(content);
    snprintf(s_status_buf, GS_RUNNER_STATUS_BUF_SIZE, "Loading | autoscroll on");
    lv_label_set_text(s_status, s_status_buf);
    lv_obj_set_style_text_color(s_status, muted, 0);
    lv_obj_set_style_text_font(s_status, accessibility_get_font_small(), 0);
    s_output_scroll = lv_obj_create(content);
    lv_obj_set_width(s_output_scroll, LV_PCT(100));
    lv_obj_set_flex_grow(s_output_scroll, 1);
    lv_obj_set_style_pad_all(s_output_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_output_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_output_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_border_width(s_output_scroll, 0, 0);
    lv_obj_set_style_bg_opa(s_output_scroll, LV_OPA_TRANSP, 0);

    s_output = lv_label_create(s_output_scroll);
    lv_label_set_text(s_output, "");
    lv_label_set_long_mode(s_output, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_output, LV_PCT(100));
    lv_obj_set_style_text_color(s_output, text, 0);
    lv_obj_set_style_text_font(s_output, accessibility_get_font_small(), 0);
#ifdef CONFIG_USE_TOUCHSCREEN
    s_touch_tb = gui_touch_bar_create(s_root);
    s_touch_bar = s_touch_tb.bar;
    if (s_touch_bar && lv_obj_is_valid(s_touch_bar)) {
        /* This view uses a taller custom bar (48px) to fit the extra Stop
         * button; the helper bar is 34px, so grow it after creation. */
        if (GS_RUNNER_TOUCH_BAR_HEIGHT != GUI_TOUCH_BAR_HEIGHT)
            lv_obj_set_height(s_touch_bar, GS_RUNNER_TOUCH_BAR_HEIGHT);
        /* Shift Back left to make room for Stop on the right half. */
        if (s_touch_tb.back_btn && lv_obj_is_valid(s_touch_tb.back_btn))
            lv_obj_align(s_touch_tb.back_btn, LV_ALIGN_CENTER, -34, 0);
        gui_touch_bar_set_callbacks(&s_touch_tb,
                                    touch_up_cb, NULL,
                                    touch_back_cb, NULL,
                                    touch_down_cb, NULL);
        /* This view keeps the scroll arrows always visible (no overflow
         * gating); the helper starts them hidden, so un-hide here. */
        if (s_touch_tb.up_btn && lv_obj_is_valid(s_touch_tb.up_btn))
            lv_obj_clear_flag(s_touch_tb.up_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_touch_tb.down_btn && lv_obj_is_valid(s_touch_tb.down_btn))
            lv_obj_clear_flag(s_touch_tb.down_btn, LV_OBJ_FLAG_HIDDEN);

        /* Extra custom danger button (kept out of the helper): centered +34,
         * same styling as before. */
        uint8_t stop_theme = settings_get_menu_theme(&G_Settings);
        s_stop_btn = lv_btn_create(s_touch_bar);
        gui_apply_pressed_style(s_stop_btn);
        lv_obj_set_size(s_stop_btn, GS_RUNNER_TOUCH_BTN_SIZE + 24, GS_RUNNER_TOUCH_BTN_SIZE);
        lv_obj_align(s_stop_btn, LV_ALIGN_CENTER, 34, 0);
        lv_obj_set_style_bg_color(s_stop_btn, lv_color_hex(theme_palette_get_danger(stop_theme)), LV_PART_MAIN);
        lv_obj_set_style_radius(s_stop_btn, 5, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(s_stop_btn, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_stop_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(s_stop_btn, 0, LV_PART_MAIN);
        lv_obj_add_event_cb(s_stop_btn, touch_stop_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *stop_label = lv_label_create(s_stop_btn);
        lv_label_set_text(stop_label, "Stop");
        lv_obj_set_style_text_color(stop_label,
                                    lv_color_hex(theme_palette_get_contrast_text(theme_palette_get_danger(stop_theme))), 0);
        lv_obj_center(stop_label);
    }
#endif
    toast_show_duration("Running GhostScript...", TOAST_INFO, 1000);
    s_launch_timer = lv_timer_create(launch_cb, 50, NULL);
    if (s_launch_timer) lv_timer_set_repeat_count(s_launch_timer, 1);
    else launch_now();
    if (!s_flush_timer) {
        s_flush_timer = lv_timer_create(flush_timer_cb, 200, NULL);
    }
}

void ghostscript_runner_view_destroy(void) {
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    s_runner_visible = false;
    if (s_launch_timer) { lv_timer_del(s_launch_timer); s_launch_timer = NULL; }
    if (s_flush_timer) { lv_timer_del(s_flush_timer); s_flush_timer = NULL; }
    if (s_runtime_mutex && xSemaphoreTake(s_runtime_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_rt) ghostscript_runtime_stop(s_rt);
        xSemaphoreGive(s_runtime_mutex);
    }
    if (s_script_task) {
        /* The task may be blocked in a firmware API. Keep its runtime and
         * output buffer alive until it exits rather than freeing shared data. */
        lvgl_obj_del_safe(&s_root);
        s_title = NULL;
        s_status = NULL;
        s_output_scroll = NULL;
        s_output = NULL;
        gui_touch_bar_destroy(&s_touch_tb);
        s_touch_bar = NULL;
        s_stop_btn = NULL;
        ghostscript_runner_view.root = NULL;
        xSemaphoreGive(s_lifecycle_mutex);
        return;
    }
    lvgl_obj_del_safe(&s_root);
    s_title = NULL;
    s_status = NULL;
    s_output_scroll = NULL;
    s_output = NULL;
    gui_touch_bar_destroy(&s_touch_tb);
    s_touch_bar = NULL;
    s_stop_btn = NULL;
    s_touch_started = false;
    s_touch_scrolling = false;
    s_follow_output = true;
    free(s_output_buf);
    s_output_buf = NULL;
    if (s_output_mutex) {
        vSemaphoreDelete(s_output_mutex);
        s_output_mutex = NULL;
    }
    if (s_runtime_mutex) {
        vSemaphoreDelete(s_runtime_mutex);
        s_runtime_mutex = NULL;
    }
    ghostscript_runner_view.root = NULL;
    xSemaphoreGive(s_lifecycle_mutex);
}

static void get_cb(void **callback) { *callback = event_handler; }

View ghostscript_runner_view = {
    .root = NULL,
    .create = ghostscript_runner_view_create,
    .destroy = ghostscript_runner_view_destroy,
    .input_callback = event_handler,
    .name = "GhostScript Runner",
    .get_hardwareinput_callback = get_cb,
};
