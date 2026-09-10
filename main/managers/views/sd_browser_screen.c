#include "managers/views/sd_browser_screen.h"
#include "gui/design_tokens.h"

#include "gui/detail_view.h"
#include "gui/touch_bar.h"
#include "gui/lvgl_safe.h"
#include "gui/options_view.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/toast.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/keyboard_screen.h"
#include "esp_log.h"
#include "lvgl.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SD_BROWSER_ROOT "/mnt"
#define SD_BROWSER_PAGE_SIZE 8
#define SD_BROWSER_EXTRA_ROWS 8
#define SD_BROWSER_PATH_MAX 256
#define SD_BROWSER_NAME_MAX 128
#define SD_BROWSER_COPY_BUFFER_SIZE 1024
#define SD_BROWSER_VIEW_MAX_BYTES 4096
#define SD_BROWSER_VIEW_MAX_LINES 64
#define SD_BROWSER_VIEW_LINE_MAX 96

typedef enum { SD_BROWSER_MODE_LIST, SD_BROWSER_MODE_DETAIL, SD_BROWSER_MODE_CONTENT } sd_browser_mode_t;
typedef enum { SD_ROW_ENTRY, SD_ROW_UP, SD_ROW_PREV, SD_ROW_NEXT, SD_ROW_REFRESH, SD_ROW_PASTE, SD_ROW_CANCEL_OP, SD_ROW_BACK } sd_browser_row_type_t;
typedef enum { SD_BROWSER_OP_NONE, SD_BROWSER_OP_COPY, SD_BROWSER_OP_MOVE } sd_browser_op_t;

typedef struct {
    char name[SD_BROWSER_NAME_MAX];
    bool is_dir;
    long size;
} sd_browser_entry_t;

typedef struct {
    sd_browser_row_type_t type;
    int entry_index;
} sd_browser_row_t;

static const char *TAG = "SDBrowser";

static lv_obj_t *browser_root = NULL;
static options_view_t *browser_options = NULL;
static detail_view_t *browser_detail = NULL;
static sd_browser_mode_t browser_mode = SD_BROWSER_MODE_LIST;
/* Page state lives in heap so no-PSRAM boards (e.g. CYDMicroUSB) don't push
 * ~1.8 KB of buffers into .dram0.bss and overflow the region. Allocated
 * on view create, freed on destroy. */
static char *current_dir = NULL;
static sd_browser_entry_t *page_entries = NULL;
static int page_entry_count = 0;
static int page_offset = 0;
static bool page_has_next = false;
static sd_browser_row_t *rows = NULL;
static int row_count = 0;
static sd_browser_entry_t *selected_entry = NULL;
static char *selected_path = NULL;
static sd_browser_op_t pending_op = SD_BROWSER_OP_NONE;
static char *pending_path = NULL;
static char *pending_name = NULL;
static int sd_touch_start_x, sd_touch_start_y;
static int sd_touch_last_x, sd_touch_last_y;
static bool sd_touch_started;
static bool sd_touch_dragged;
static int sd_touch_drag_axis;
static lv_obj_t *sd_touch_scroll_target;
static const int TAP_THRESHOLD = 14;
static const int SD_SWIPE_THRESHOLD_RATIO = 20;

#ifdef CONFIG_USE_TOUCHSCREEN
static gui_touch_bar_t s_sd_touch_tb = {0};
static lv_obj_t *sd_scroll_up_btn = NULL;
static lv_obj_t *sd_scroll_down_btn = NULL;
static lv_obj_t *sd_back_btn = NULL;
#endif

static int sd_resolve_drag_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < TAP_THRESHOLD && abs_dy < TAP_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + 4) return 1;
    if (abs_dx >= abs_dy + 4) return 2;
    return 0;
}

static int sd_clamp_drag_delta(int delta) {
    if (abs(delta) <= 1) return 0;
    if (delta > 36) return 36;
    if (delta < -36) return -36;
    return delta;
}

static void sd_touch_reset(void) {
    sd_touch_started = false;
    sd_touch_dragged = false;
    sd_touch_drag_axis = 0;
    sd_touch_scroll_target = NULL;
}

static lv_obj_t *sd_browser_scroll_target(void) {
    if (browser_mode == SD_BROWSER_MODE_LIST && browser_options) {
        return options_view_get_list(browser_options);
    }
    if (browser_mode == SD_BROWSER_MODE_CONTENT && browser_detail) {
        return detail_view_get_info_panel(browser_detail);
    }
    if (browser_mode == SD_BROWSER_MODE_DETAIL && browser_detail) {
        return detail_view_get_list(browser_detail);
    }
    return NULL;
}

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);

static void sd_browser_back(void);

#ifdef CONFIG_USE_TOUCHSCREEN
static void sd_update_scroll_buttons_visibility(void) {
    lv_obj_t *target = sd_browser_scroll_target();
    if (!target || !lv_obj_is_valid(target)) {
        if (sd_scroll_up_btn && lv_obj_is_valid(sd_scroll_up_btn))
            lv_obj_add_flag(sd_scroll_up_btn, LV_OBJ_FLAG_HIDDEN);
        if (sd_scroll_down_btn && lv_obj_is_valid(sd_scroll_down_btn))
            lv_obj_add_flag(sd_scroll_down_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    gui_touch_bar_update_visibility(&s_sd_touch_tb, target);
}

static void sd_scroll_up_cb(lv_event_t *e) {
    (void)e;
    lv_obj_t *target = sd_browser_scroll_target();
    if (!target || !lv_obj_is_valid(target)) return;
    lv_coord_t scroll_amt = lv_obj_get_height(target) / 2;
    lv_obj_scroll_by_bounded(target, 0, scroll_amt, LV_ANIM_OFF);
    sd_update_scroll_buttons_visibility();
}

static void sd_scroll_down_cb(lv_event_t *e) {
    (void)e;
    lv_obj_t *target = sd_browser_scroll_target();
    if (!target || !lv_obj_is_valid(target)) return;
    lv_coord_t scroll_amt = lv_obj_get_height(target) / 2;
    lv_obj_scroll_by_bounded(target, 0, -scroll_amt, LV_ANIM_OFF);
    sd_update_scroll_buttons_visibility();
}

static void sd_back_btn_cb(lv_event_t *e) {
    (void)e;
    sd_browser_back();
}
#endif

static bool sd_browser_is_root(void) {
    return strcmp(current_dir, SD_BROWSER_ROOT) == 0;
}

static const char *sd_browser_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static void sd_browser_title(char *out, size_t out_size) {
    if (sd_browser_is_root()) snprintf(out, out_size, "SD Card");
    else snprintf(out, out_size, "SD: %.80s", sd_browser_basename(current_dir));
}

static void sd_browser_copy(char *out, size_t out_size, const char *src) {
    if (!out || out_size == 0) return;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, src, len);
    out[len] = '\0';
}

static bool sd_browser_join_path(char *out, size_t out_size, const char *dir, const char *name) {
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    if (dir_len == 0 || dir_len + 1 + name_len >= out_size) return false;
    memcpy(out, dir, dir_len);
    out[dir_len] = '/';
    memcpy(out + dir_len + 1, name, name_len);
    out[dir_len + 1 + name_len] = '\0';
    return true;
}

static esp_err_t sd_browser_sd_begin(bool *mounted_here, bool *display_was_suspended) {
    if (mounted_here) *mounted_here = false;
    if (display_was_suspended) *display_was_suspended = false;
    if (sd_card_manager.is_initialized) return ESP_OK;

    esp_err_t err = sd_card_mount_for_flush(display_was_suspended);
    if (err == ESP_OK && mounted_here) *mounted_here = true;
    return err;
}

static void sd_browser_sd_end(bool mounted_here, bool display_was_suspended) {
    if (mounted_here) sd_card_unmount_after_flush(display_was_suspended);
}

static void sd_browser_parent_dir(void) {
    if (sd_browser_is_root()) return;
    char *slash = strrchr(current_dir, '/');
    if (!slash || slash <= current_dir + strlen(SD_BROWSER_ROOT)) {
        snprintf(current_dir, SD_BROWSER_PATH_MAX, SD_BROWSER_ROOT);
    } else {
        *slash = '\0';
    }
}

static bool sd_browser_entry_matches_pass(const char *path, int pass, bool *is_dir, long *size) {
    struct stat st;
    if (stat(path, &st) != 0) return false;
    bool dir = S_ISDIR(st.st_mode);
    if ((pass == 0 && !dir) || (pass == 1 && dir)) return false;
    if (is_dir) *is_dir = dir;
    if (size) *size = (long)st.st_size;
    return true;
}

static int sd_browser_load_page(void) {
    page_entry_count = 0;
    page_has_next = false;

    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_browser_sd_begin(&mounted_here, &display_was_suspended) != ESP_OK) return -1;

    int matched = 0;
    for (int pass = 0; pass < 2; pass++) {
        DIR *dir = opendir(current_dir);
        if (!dir) {
            ESP_LOGW(TAG, "opendir failed for %s: errno=%d", current_dir, errno);
            sd_browser_sd_end(mounted_here, display_was_suspended);
            return -1;
        }

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '\0' || strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

            char full_path[SD_BROWSER_PATH_MAX];
            if (!sd_browser_join_path(full_path, sizeof(full_path), current_dir, entry->d_name)) continue;

            bool is_dir = false;
            long size = 0;
            if (!sd_browser_entry_matches_pass(full_path, pass, &is_dir, &size)) continue;

            if (matched++ < page_offset) continue;
            if (page_entry_count >= SD_BROWSER_PAGE_SIZE) {
                page_has_next = true;
                closedir(dir);
                sd_browser_sd_end(mounted_here, display_was_suspended);
                return page_entry_count;
            }

            sd_browser_entry_t *out = &page_entries[page_entry_count++];
            snprintf(out->name, sizeof(out->name), "%.127s", entry->d_name);
            out->is_dir = is_dir;
            out->size = size;
        }
        closedir(dir);
    }

    sd_browser_sd_end(mounted_here, display_was_suspended);
    return page_entry_count;
}

static void sd_browser_show_list(void);
static void sd_browser_show_selected_file_detail(void);
static void sd_browser_detail_back_cb(lv_event_t *e);

static bool sd_browser_valid_new_name(const char *name) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

static bool sd_browser_has_pending_op(void) {
    return pending_op != SD_BROWSER_OP_NONE && pending_path && pending_path[0] && pending_name && pending_name[0];
}

static void sd_browser_clear_pending_op(void) {
    pending_op = SD_BROWSER_OP_NONE;
    if (pending_path) pending_path[0] = '\0';
    if (pending_name) pending_name[0] = '\0';
}

static void sd_browser_set_pending_op(sd_browser_op_t op) {
    if (!selected_path || !selected_entry || !pending_path || !pending_name) return;
    pending_op = op;
    sd_browser_copy(pending_path, SD_BROWSER_PATH_MAX, selected_path);
    sd_browser_copy(pending_name, SD_BROWSER_NAME_MAX, selected_entry->name);
    toast_show_duration(op == SD_BROWSER_OP_MOVE ? "Move ready: Paste Here" : "Copy ready: Paste Here", TOAST_SUCCESS, 1400);
    sd_browser_show_list();
}

static bool sd_browser_copy_file_data(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;

    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }

    uint8_t *buf = malloc(SD_BROWSER_COPY_BUFFER_SIZE);
    if (!buf) {
        fclose(out);
        fclose(in);
        remove(dst);
        return false;
    }

    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, SD_BROWSER_COPY_BUFFER_SIZE, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(in);
    free(buf);

    if (!ok) remove(dst);
    return ok;
}

static bool sd_browser_paste_pending(void) {
    if (!sd_browser_has_pending_op()) {
        toast_show_duration("Nothing to paste", TOAST_WARN, 1200);
        return false;
    }

    char dst_path[SD_BROWSER_PATH_MAX];
    if (!sd_browser_join_path(dst_path, sizeof(dst_path), current_dir, pending_name)) {
        toast_show_duration("Path too long", TOAST_WARN, 1600);
        return false;
    }
    if (strcmp(pending_path, dst_path) == 0) {
        toast_show_duration("Choose another folder", TOAST_WARN, 1600);
        return false;
    }

    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_browser_sd_begin(&mounted_here, &display_was_suspended) != ESP_OK) {
        toast_show_duration("SD mount failed", TOAST_WARN, 1600);
        return false;
    }

    struct stat st;
    if (stat(dst_path, &st) == 0) {
        sd_browser_sd_end(mounted_here, display_was_suspended);
        toast_show_duration("File already exists", TOAST_WARN, 1600);
        return false;
    }

    bool ok = false;
    if (pending_op == SD_BROWSER_OP_MOVE) {
        ok = rename(pending_path, dst_path) == 0;
    } else {
        ok = sd_browser_copy_file_data(pending_path, dst_path);
    }
    sd_browser_sd_end(mounted_here, display_was_suspended);

    if (!ok) {
        toast_show_duration(pending_op == SD_BROWSER_OP_MOVE ? "Move failed" : "Copy failed", TOAST_WARN, 1600);
        return false;
    }

    toast_show_duration(pending_op == SD_BROWSER_OP_MOVE ? "File moved" : "File copied", TOAST_SUCCESS, 1200);
    sd_browser_clear_pending_op();
    return true;
}

static bool sd_browser_rename_selected(const char *new_name) {
    if (!sd_browser_valid_new_name(new_name)) {
        toast_show_duration("Invalid file name", TOAST_WARN, 1600);
        return false;
    }

    char new_path[SD_BROWSER_PATH_MAX];
    if (!sd_browser_join_path(new_path, sizeof(new_path), current_dir, new_name)) {
        toast_show_duration("Name too long", TOAST_WARN, 1600);
        return false;
    }

    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_browser_sd_begin(&mounted_here, &display_was_suspended) != ESP_OK) {
        toast_show_duration("SD mount failed", TOAST_WARN, 1600);
        return false;
    }

    int rc = rename(selected_path, new_path);
    sd_browser_sd_end(mounted_here, display_was_suspended);

    if (rc != 0) {
        toast_show_duration("Rename failed", TOAST_WARN, 1600);
        return false;
    }

    toast_show_duration("File renamed", TOAST_SUCCESS, 1200);
    return true;
}

static void sd_browser_rename_submit_cb(const char *text) {
    keyboard_view_set_submit_callback(NULL);
    keyboard_view_set_start_caps(true);
    if (sd_browser_rename_selected(text)) {
        display_manager_switch_view(&sd_browser_view);
    } else {
        display_manager_switch_view(&sd_browser_view);
    }
}

static void sd_browser_rename_cb(lv_event_t *e) {
    (void)e;
    keyboard_view_set_return_view(&sd_browser_view);
    keyboard_view_set_placeholder("New file name");
    keyboard_view_set_initial_text(selected_entry->name);
    keyboard_view_set_start_caps(false);
    keyboard_view_set_submit_callback(sd_browser_rename_submit_cb);
    display_manager_switch_view(&keyboard_view);
}

static void sd_browser_confirm_delete_cb(lv_event_t *e) {
    (void)e;

    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_browser_sd_begin(&mounted_here, &display_was_suspended) != ESP_OK) {
        toast_show_duration("SD mount failed", TOAST_WARN, 1600);
        return;
    }

    int rc = remove(selected_path);
    sd_browser_sd_end(mounted_here, display_was_suspended);

    if (rc != 0) {
        toast_show_duration("Delete failed", TOAST_WARN, 1600);
        return;
    }

    toast_show_duration("File deleted", TOAST_SUCCESS, 1200);
    sd_browser_show_list();
}

static void sd_browser_delete_cb(lv_event_t *e) {
    (void)e;

    if (browser_detail) {
        detail_view_destroy(browser_detail);
        browser_detail = NULL;
    }

    browser_detail = detail_view_create(browser_root, "Delete File?");
    if (!browser_detail) return;

#ifdef CONFIG_USE_TOUCHSCREEN
    detail_view_set_bottom_reserved(browser_detail, gui_touch_bar_height());
#endif

    detail_view_add_info(browser_detail, "Name", selected_entry->name);
    detail_view_add_info(browser_detail, "Action", "This cannot be undone");
    detail_view_add_action(browser_detail, LV_SYMBOL_TRASH " Confirm Delete", sd_browser_confirm_delete_cb, NULL);
    detail_view_add_back(browser_detail, sd_browser_detail_back_cb, NULL);
    detail_view_set_selected(browser_detail, 0);
}

static void sd_browser_content_back_cb(lv_event_t *e) {
    (void)e;
    sd_browser_show_selected_file_detail();
}

static void sd_browser_flush_preview_line(detail_view_t *dv, int *line_no, int *line_count,
                                          char *line, size_t *line_len, bool *line_truncated) {
    if (!dv || !line_no || !line_count || !line || !line_len || !line_truncated) return;
    if (*line_count >= SD_BROWSER_VIEW_MAX_LINES) return;

    if (*line_truncated) {
        int dots = 3;
        while (dots-- > 0 && *line_len < SD_BROWSER_VIEW_LINE_MAX - 1) {
            line[(*line_len)++] = '.';
        }
    }
    line[*line_len] = '\0';

    char label[12];
    snprintf(label, sizeof(label), "%d", *line_no);
    detail_view_add_info(dv, label, line);
    (*line_no)++;
    (*line_count)++;
    *line_len = 0;
    *line_truncated = false;
}

static void sd_browser_add_file_preview(detail_view_t *dv) {
    bool mounted_here = false;
    bool display_was_suspended = false;
    if (sd_browser_sd_begin(&mounted_here, &display_was_suspended) != ESP_OK) {
        detail_view_add_info(dv, "Preview", "SD mount failed");
        return;
    }

    FILE *f = fopen(selected_path, "rb");
    if (!f) {
        sd_browser_sd_end(mounted_here, display_was_suspended);
        detail_view_add_info(dv, "Preview", "Open failed");
        return;
    }

    char line[SD_BROWSER_VIEW_LINE_MAX];
    size_t line_len = 0;
    int line_no = 1;
    int line_count = 0;
    size_t bytes = 0;
    bool binary = false;
    bool truncated = false;
    bool line_truncated = false;
    int c;

    while ((c = fgetc(f)) != EOF) {
        if (bytes >= SD_BROWSER_VIEW_MAX_BYTES || line_count >= SD_BROWSER_VIEW_MAX_LINES) {
            truncated = true;
            break;
        }
        bytes++;

        unsigned char ch = (unsigned char)c;
        if (ch == 0 || (ch < 32 && ch != '\n' && ch != '\r' && ch != '\t')) {
            binary = true;
            break;
        }
        if (ch == '\r') continue;
        if (ch == '\n') {
            sd_browser_flush_preview_line(dv, &line_no, &line_count, line, &line_len, &line_truncated);
            continue;
        }
        if (line_len < SD_BROWSER_VIEW_LINE_MAX - 4) {
            line[line_len++] = (char)ch;
        } else {
            line_truncated = true;
        }
    }

    if (!binary && line_count < SD_BROWSER_VIEW_MAX_LINES && line_len > 0) {
        sd_browser_flush_preview_line(dv, &line_no, &line_count, line, &line_len, &line_truncated);
    }

    fclose(f);
    sd_browser_sd_end(mounted_here, display_was_suspended);

    if (bytes == 0 && !binary) {
        detail_view_add_info(dv, "Preview", "Empty file");
    } else if (binary) {
        detail_view_add_info(dv, "Preview", "Binary or unsupported text");
    }
    if (truncated) {
        detail_view_add_info(dv, "Notice", "Preview truncated");
    }
}

static void sd_browser_view_contents_cb(lv_event_t *e) {
    (void)e;
    if (!selected_entry || !selected_path) return;

    if (browser_options) {
        options_view_destroy(browser_options);
        browser_options = NULL;
    }
    if (browser_detail) {
        detail_view_destroy(browser_detail);
        browser_detail = NULL;
    }

    browser_mode = SD_BROWSER_MODE_CONTENT;
    char title[96];
    snprintf(title, sizeof(title), "View: %.80s", selected_entry->name);
    browser_detail = detail_view_create(browser_root, title);
    if (!browser_detail) return;

#ifdef CONFIG_USE_TOUCHSCREEN
    detail_view_set_bottom_reserved(browser_detail, gui_touch_bar_height());
#endif

    sd_browser_add_file_preview(browser_detail);
    detail_view_add_back(browser_detail, sd_browser_content_back_cb, NULL);
    detail_view_set_selected(browser_detail, 0);

#ifdef CONFIG_USE_TOUCHSCREEN
    sd_update_scroll_buttons_visibility();
#endif
}

static void sd_browser_copy_cb(lv_event_t *e) {
    (void)e;
    sd_browser_set_pending_op(SD_BROWSER_OP_COPY);
}

static void sd_browser_move_cb(lv_event_t *e) {
    (void)e;
    sd_browser_set_pending_op(SD_BROWSER_OP_MOVE);
}

static void sd_browser_detail_back_cb(lv_event_t *e) {
    (void)e;
    sd_browser_show_list();
}

static void sd_browser_show_selected_file_detail(void) {
    if (!selected_entry || !selected_path) return;
    if (browser_options) {
        options_view_destroy(browser_options);
        browser_options = NULL;
    }
    if (browser_detail) {
        detail_view_destroy(browser_detail);
        browser_detail = NULL;
    }

    browser_mode = SD_BROWSER_MODE_DETAIL;
    char title[96];
    snprintf(title, sizeof(title), "File: %.80s", selected_entry->name);
    browser_detail = detail_view_create(browser_root, title);
    if (!browser_detail) return;

#ifdef CONFIG_USE_TOUCHSCREEN
    detail_view_set_bottom_reserved(browser_detail, gui_touch_bar_height());
#endif

    char folder[96];
    snprintf(folder, sizeof(folder), "%.80s", sd_browser_basename(current_dir));
    detail_view_add_info(browser_detail, "Name", selected_entry->name);
    detail_view_add_info(browser_detail, "Folder", folder);
    detail_view_add_infof(browser_detail, "Size", "%ld bytes", selected_entry->size);
    detail_view_add_action(browser_detail, LV_SYMBOL_EYE_OPEN " View Contents", sd_browser_view_contents_cb, NULL);
    detail_view_add_action(browser_detail, LV_SYMBOL_COPY " Copy", sd_browser_copy_cb, NULL);
    detail_view_add_action(browser_detail, LV_SYMBOL_CUT " Move", sd_browser_move_cb, NULL);
    detail_view_add_action(browser_detail, LV_SYMBOL_EDIT " Rename", sd_browser_rename_cb, NULL);
    detail_view_add_action(browser_detail, LV_SYMBOL_TRASH " Delete", sd_browser_delete_cb, NULL);
    detail_view_add_back(browser_detail, sd_browser_detail_back_cb, NULL);
    detail_view_set_selected(browser_detail, 0);

#ifdef CONFIG_USE_TOUCHSCREEN
    sd_update_scroll_buttons_visibility();
#endif
}

static void sd_browser_show_file_detail(int index) {
    if (index < 0 || index >= page_entry_count) return;
    if (selected_entry) *selected_entry = page_entries[index];
    if (!sd_browser_join_path(selected_path, SD_BROWSER_PATH_MAX, current_dir, selected_entry->name)) return;
    sd_browser_show_selected_file_detail();
}

static void sd_browser_open_entry(int index) {
    if (index < 0 || index >= page_entry_count) return;
    if (page_entries[index].is_dir) {
        char next_dir[SD_BROWSER_PATH_MAX];
        if (!sd_browser_join_path(next_dir, sizeof(next_dir), current_dir, page_entries[index].name)) return;
        sd_browser_copy(current_dir, SD_BROWSER_PATH_MAX, next_dir);
        page_offset = 0;
        sd_browser_show_list();
    } else {
        sd_browser_show_file_detail(index);
    }
}

static void sd_browser_handle_row(int row_index) {
    if (row_index < 0 || row_index >= row_count) return;

    switch (rows[row_index].type) {
        case SD_ROW_ENTRY:
            sd_browser_open_entry(rows[row_index].entry_index);
            break;
        case SD_ROW_UP:
            sd_browser_parent_dir();
            page_offset = 0;
            sd_browser_show_list();
            break;
        case SD_ROW_PREV:
            page_offset -= SD_BROWSER_PAGE_SIZE;
            if (page_offset < 0) page_offset = 0;
            sd_browser_show_list();
            break;
        case SD_ROW_NEXT:
            page_offset += SD_BROWSER_PAGE_SIZE;
            sd_browser_show_list();
            break;
        case SD_ROW_REFRESH:
            sd_browser_show_list();
            break;
        case SD_ROW_PASTE:
            if (sd_browser_paste_pending()) sd_browser_show_list();
            break;
        case SD_ROW_CANCEL_OP:
            sd_browser_clear_pending_op();
            toast_show_duration("File operation cancelled", TOAST_SUCCESS, 1200);
            sd_browser_show_list();
            break;
        case SD_ROW_BACK:
            display_manager_go_back();
            break;
    }
}

static void sd_browser_row_click_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    int row_index = (int)(intptr_t)lv_obj_get_user_data(target);
    sd_browser_handle_row(row_index);
}

static void sd_browser_add_row(const char *label, sd_browser_row_type_t type, int entry_index) {
    if (!browser_options || row_count >= SD_BROWSER_PAGE_SIZE + SD_BROWSER_EXTRA_ROWS) return;
    int row_index = row_count;
    rows[row_index].type = type;
    rows[row_index].entry_index = entry_index;
    lv_obj_t *btn = options_view_add_item(browser_options, label, sd_browser_row_click_cb, NULL);
    if (btn) {
        lv_obj_set_user_data(btn, (void *)(intptr_t)row_index);
        lv_obj_t *lbl = lv_obj_get_child(btn, 0);
        if (lbl) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL);
    }
    row_count++;
}

static void sd_browser_show_list(void) {
    browser_mode = SD_BROWSER_MODE_LIST;
    row_count = 0;

    if (browser_detail) {
        detail_view_destroy(browser_detail);
        browser_detail = NULL;
    }
    if (browser_options) {
        options_view_destroy(browser_options);
        browser_options = NULL;
    }

    char title[96];
    sd_browser_title(title, sizeof(title));
    browser_options = options_view_create(browser_root, title);
    if (!browser_options) return;

#ifdef CONFIG_USE_TOUCHSCREEN
    lv_obj_t *list = options_view_get_list(browser_options);
    if (list && lv_obj_is_valid(list)) {
        int container_height = LV_VER_RES - GUI_STATUS_BAR_H - gui_touch_bar_height();
        lv_obj_set_size(list, GUI_OPTIONS_LIST_WIDTH, container_height);
        lv_obj_align(list, LV_ALIGN_TOP_MID, 0, GUI_STATUS_BAR_H);
    }
#endif

    int loaded = sd_browser_load_page();
    if (loaded < 0) {
        sd_browser_add_row(LV_SYMBOL_SD_CARD " SD unavailable", SD_ROW_REFRESH, -1);
        sd_browser_add_row(LV_SYMBOL_REFRESH " Refresh", SD_ROW_REFRESH, -1);
        sd_browser_add_row(LV_SYMBOL_LEFT " Back", SD_ROW_BACK, -1);
        options_view_set_selected(browser_options, 1);
#ifdef CONFIG_USE_TOUCHSCREEN
        sd_update_scroll_buttons_visibility();
#endif
        return;
    }

    if (!sd_browser_is_root()) sd_browser_add_row(LV_SYMBOL_UP " Up", SD_ROW_UP, -1);
    if (page_offset > 0) sd_browser_add_row(LV_SYMBOL_LEFT " Prev", SD_ROW_PREV, -1);
    if (sd_browser_has_pending_op()) {
        char paste_label[SD_BROWSER_NAME_MAX + 28];
        snprintf(paste_label, sizeof(paste_label), LV_SYMBOL_PASTE " Paste %s: %.127s",
                 pending_op == SD_BROWSER_OP_MOVE ? "Move" : "Copy", pending_name);
        sd_browser_add_row(paste_label, SD_ROW_PASTE, -1);
        sd_browser_add_row(LV_SYMBOL_CLOSE " Cancel File Op", SD_ROW_CANCEL_OP, -1);
    }

    for (int i = 0; i < page_entry_count; i++) {
        char label[SD_BROWSER_NAME_MAX + 12];
        const char *prefix = page_entries[i].is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
        snprintf(label, sizeof(label), "%.4s %.127s", prefix, page_entries[i].name);
        sd_browser_add_row(label, SD_ROW_ENTRY, i);
    }

    if (page_entry_count == 0) sd_browser_add_row("No items found", SD_ROW_REFRESH, -1);
    if (page_has_next) sd_browser_add_row("Next " LV_SYMBOL_RIGHT, SD_ROW_NEXT, -1);
    sd_browser_add_row(LV_SYMBOL_LEFT " Back", SD_ROW_BACK, -1);
    options_view_set_selected(browser_options, 0);

#ifdef CONFIG_USE_TOUCHSCREEN
    sd_update_scroll_buttons_visibility();
#endif
}

void sd_browser_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    browser_root = gui_screen_create_root(NULL, NULL, lv_color_hex(theme_palette_get_background(theme)), LV_OPA_COVER);
    sd_browser_view.root = browser_root;
    if (!browser_root) return;

    /* Lazily allocate page state. All buffers survive until sd_browser_destroy
     * is called (the view is single-instance). */
    if (!current_dir) {
        current_dir = malloc(SD_BROWSER_PATH_MAX);
        if (current_dir) snprintf(current_dir, SD_BROWSER_PATH_MAX, "%s", SD_BROWSER_ROOT);
    }
    if (!page_entries) {
        page_entries = calloc(SD_BROWSER_PAGE_SIZE, sizeof(*page_entries));
    }
    if (!rows) {
        rows = calloc(SD_BROWSER_PAGE_SIZE + SD_BROWSER_EXTRA_ROWS, sizeof(*rows));
    }
    if (!selected_entry) {
        selected_entry = calloc(1, sizeof(*selected_entry));
    }
    if (!selected_path) {
        selected_path = malloc(SD_BROWSER_PATH_MAX);
    }
    if (!pending_path) {
        pending_path = malloc(SD_BROWSER_PATH_MAX);
        if (pending_path) pending_path[0] = '\0';
    }
    if (!pending_name) {
        pending_name = malloc(SD_BROWSER_NAME_MAX);
        if (pending_name) pending_name[0] = '\0';
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    s_sd_touch_tb = gui_touch_bar_create(browser_root);
    sd_scroll_up_btn = s_sd_touch_tb.up_btn;
    sd_back_btn = s_sd_touch_tb.back_btn;
    sd_scroll_down_btn = s_sd_touch_tb.down_btn;
    if (s_sd_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_sd_touch_tb,
                                    sd_scroll_up_cb, NULL,
                                    sd_back_btn_cb, NULL,
                                    sd_scroll_down_cb, NULL);
    }
#endif

    page_offset = 0;
    sd_touch_reset();
    sd_browser_show_list();
}

void sd_browser_destroy(void) {
    if (browser_detail) {
        detail_view_destroy(browser_detail);
        browser_detail = NULL;
    }
    if (browser_options) {
        options_view_destroy(browser_options);
        browser_options = NULL;
    }
#ifdef CONFIG_USE_TOUCHSCREEN
    gui_touch_bar_destroy(&s_sd_touch_tb);
    sd_scroll_up_btn = NULL;
    sd_scroll_down_btn = NULL;
    sd_back_btn = NULL;
#endif
    if (browser_root) {
        lvgl_obj_del_safe(&browser_root);
        sd_browser_view.root = NULL;
    }
    browser_mode = SD_BROWSER_MODE_LIST;
    row_count = 0;
    page_entry_count = 0;
    free(current_dir);
    current_dir = NULL;
    free(page_entries);
    page_entries = NULL;
    free(rows);
    rows = NULL;
    free(selected_entry);
    selected_entry = NULL;
    free(selected_path);
    selected_path = NULL;
    free(pending_path);
    pending_path = NULL;
    free(pending_name);
    pending_name = NULL;
    pending_op = SD_BROWSER_OP_NONE;
    sd_touch_reset();
}

static void sd_browser_select_current(void) {
    if (browser_mode == SD_BROWSER_MODE_DETAIL || browser_mode == SD_BROWSER_MODE_CONTENT) {
        lv_obj_t *obj = detail_view_get_selected_obj(browser_detail);
        if (obj && lv_obj_is_valid(obj)) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
        return;
    }

    int selected = options_view_get_selected(browser_options);
    sd_browser_handle_row(selected);
}

static void sd_browser_back(void) {
    if (browser_mode == SD_BROWSER_MODE_CONTENT) {
        sd_browser_show_selected_file_detail();
    } else if (browser_mode == SD_BROWSER_MODE_DETAIL) {
        sd_browser_show_list();
    } else if (!sd_browser_is_root()) {
        sd_browser_parent_dir();
        page_offset = 0;
        sd_browser_show_list();
    } else {
        display_manager_go_back();
    }
}

static void sd_browser_move(int delta) {
    if (browser_mode == SD_BROWSER_MODE_DETAIL || browser_mode == SD_BROWSER_MODE_CONTENT) {
        if (delta < 0) detail_view_step_up(browser_detail);
        else detail_view_step_down(browser_detail);
    } else {
        options_view_move_selection(browser_options, delta);
    }
}

static void sd_browser_handle_key(int key) {
    if (key == LV_KEY_UP || key == 'k' || key == ';') sd_browser_move(-1);
    else if (key == LV_KEY_DOWN || key == 'j' || key == '.') sd_browser_move(1);
    else if (key == LV_KEY_ENTER || key == 13) sd_browser_select_current();
    else if (key == LV_KEY_ESC || key == '`' || key == 29) sd_browser_back();
}

static void sd_browser_input_callback(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) sd_browser_move(-1);
        else if (button == 4) sd_browser_move(1);
        else if (button == 1) sd_browser_select_current();
        else if (button == 0) sd_browser_back();
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) sd_browser_select_current();
        else sd_browser_move(event->data.encoder.direction > 0 ? 1 : -1);
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        if (!event->is_repeat) {
            sd_browser_handle_key(event->data.key_value);
        }
    } else if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->state == LV_INDEV_STATE_PR) {
#ifdef CONFIG_USE_TOUCHSCREEN
            if (gui_touch_bar_hit(sd_scroll_up_btn, data->point.x, data->point.y)) {
                sd_scroll_up_cb(NULL);
                sd_touch_started = false;
                return;
            }
            if (gui_touch_bar_hit(sd_scroll_down_btn, data->point.x, data->point.y)) {
                sd_scroll_down_cb(NULL);
                sd_touch_started = false;
                return;
            }
            if (gui_touch_bar_hit(sd_back_btn, data->point.x, data->point.y)) {
                sd_back_btn_cb(NULL);
                sd_touch_started = false;
                return;
            }
#endif
            if (!sd_touch_started) {
                sd_touch_started = true;
                sd_touch_dragged = false;
                sd_touch_drag_axis = 0;
                sd_touch_start_x = data->point.x;
                sd_touch_start_y = data->point.y;
                sd_touch_last_x = data->point.x;
                sd_touch_last_y = data->point.y;
                sd_touch_scroll_target = NULL;
            } else {
                int dy = data->point.y - sd_touch_last_y;
                sd_touch_last_x = data->point.x;
                sd_touch_last_y = data->point.y;

                if (!sd_touch_dragged) {
                    sd_touch_drag_axis = sd_resolve_drag_axis(
                        data->point.x - sd_touch_start_x,
                        data->point.y - sd_touch_start_y);
                    sd_touch_dragged = sd_touch_drag_axis != 0;
                }

                if (sd_touch_dragged && sd_touch_drag_axis == 1) {
                    bool live = settings_get_touch_drag_scroll(&G_Settings);
                    lv_obj_t *scroll_target = sd_browser_scroll_target();
                    if (scroll_target && lv_obj_is_valid(scroll_target)) {
                        if (live) {
                            dy = sd_clamp_drag_delta(dy);
                            if (dy) display_manager_queue_scroll(scroll_target, dy);
                        } else {
                            sd_touch_scroll_target = scroll_target;
                        }
                    }
                }
            }
            return;
        }
        if (data->state == LV_INDEV_STATE_REL) {
            if (!sd_touch_started) return;
            bool was_dragged = sd_touch_dragged;
            int release_dy = data->point.y - sd_touch_start_y;
            lv_obj_t *release_target = sd_touch_scroll_target;
            sd_touch_reset();
            if (was_dragged) {
                if (release_target && lv_obj_is_valid(release_target) &&
                    !settings_get_touch_drag_scroll(&G_Settings) && release_dy) {
                    display_manager_queue_scroll(release_target, release_dy);
                }
                return;
            }

            int dx = data->point.x - sd_touch_start_x;
            int dy = data->point.y - sd_touch_start_y;
            int thr = LV_VER_RES / SD_SWIPE_THRESHOLD_RATIO;

            // Horizontal swipe right = back
            if (abs(dx) > thr && abs(dx) > abs(dy) && dx > 0) {
                sd_browser_back();
                return;
            }

            // Tap: find which item was tapped
            lv_obj_t *list = sd_browser_scroll_target();
            if (list && lv_obj_is_valid(list)) {
                uint32_t cnt = lv_obj_get_child_cnt(list);
                for (uint32_t i = 0; i < cnt; i++) {
                    lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
                    if (!child) continue;
                    lv_area_t a;
                    lv_obj_get_coords(child, &a);
                    if (data->point.x >= a.x1 && data->point.x <= a.x2 &&
                        data->point.y >= a.y1 && data->point.y <= a.y2) {
                        lv_event_send(child, LV_EVENT_CLICKED, NULL);
                        return;
                    }
                }
            }
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        sd_browser_back();
#endif
    }
}

void sd_browser_get_callback(void **callback) {
    *callback = sd_browser_input_callback;
}

View sd_browser_view = {
    .root = NULL,
    .create = sd_browser_create,
    .destroy = sd_browser_destroy,
    .name = "SD Browser",
    .get_hardwareinput_callback = sd_browser_get_callback,
    .input_callback = sd_browser_input_callback,
};
