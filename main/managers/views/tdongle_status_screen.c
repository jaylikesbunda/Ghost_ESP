#include "managers/views/tdongle_status_screen.h"

#include "gui/screen_layout.h"
#include "lvgl.h"
#include "managers/display_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TDONGLE_STATUS_LINE_LEN 32
#define TDONGLE_STATUS_IDLE_MS 5000

static lv_obj_t *s_root;
static lv_obj_t *s_logo_img;
static lv_obj_t *s_text_panel;
static lv_obj_t *s_line1_label;
static lv_obj_t *s_line2_label;
static lv_timer_t *s_idle_timer;
static char s_line1[TDONGLE_STATUS_LINE_LEN] = "GhostESP";
static char s_line2[TDONGLE_STATUS_LINE_LEN] = "Ready";

extern const lv_img_dsc_t ghostesplogo;

typedef struct {
    char line1[TDONGLE_STATUS_LINE_LEN];
    char line2[TDONGLE_STATUS_LINE_LEN];
} tdongle_status_msg_t;

static void tdongle_status_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) src = "";
    size_t pos = 0;
    while (src[pos] && pos + 1 < dst_len) {
        dst[pos] = src[pos];
        pos++;
    }
    dst[pos] = '\0';
}

static void tdongle_status_apply_cached(void)
{
    if (!s_line1_label || !s_line2_label) return;
    lv_label_set_text(s_line1_label, s_line1);
    lv_label_set_text(s_line2_label, s_line2);
}

static void tdongle_status_show_idle(void)
{
    if (s_logo_img) lv_obj_clear_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
    if (s_text_panel) lv_obj_add_flag(s_text_panel, LV_OBJ_FLAG_HIDDEN);
}

static void tdongle_status_idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    tdongle_status_show_idle();
}

static void tdongle_status_show_text(void)
{
    if (s_logo_img) lv_obj_add_flag(s_logo_img, LV_OBJ_FLAG_HIDDEN);
    if (s_text_panel) lv_obj_clear_flag(s_text_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_idle_timer) lv_timer_reset(s_idle_timer);
}

static void tdongle_status_apply_async(void *arg)
{
    tdongle_status_msg_t *msg = (tdongle_status_msg_t *)arg;
    if (!msg) return;
    tdongle_status_copy(s_line1, sizeof(s_line1), msg->line1);
    tdongle_status_copy(s_line2, sizeof(s_line2), msg->line2);
    tdongle_status_apply_cached();
    tdongle_status_show_text();
    free(msg);
}

bool tdongle_status_is_ready(void)
{
    return s_root != NULL;
}

void tdongle_status_set_lines(const char *line_one, const char *line_two)
{
    tdongle_status_msg_t *msg = malloc(sizeof(*msg));
    if (!msg) return;
    tdongle_status_copy(msg->line1, sizeof(msg->line1), line_one);
    tdongle_status_copy(msg->line2, sizeof(msg->line2), line_two);
    if (!display_manager_run_on_lvgl(tdongle_status_apply_async, msg)) {
        free(msg);
    }
}

void tdongle_status_show_status(const char *status_line)
{
    tdongle_status_set_lines("Status", status_line ? status_line : "");
}

void tdongle_status_show_attack(const char *attack_name, const char *target)
{
    char line1[TDONGLE_STATUS_LINE_LEN];
    snprintf(line1, sizeof(line1), "Attack: %s", attack_name ? attack_name : "?");
    tdongle_status_set_lines(line1, target ? target : "");
}

void tdongle_status_clear(void)
{
    tdongle_status_set_lines("", "");
}

static void tdongle_status_create(void)
{
    display_manager_fill_screen(lv_color_black());

    s_root = gui_screen_create_root_no_bg(NULL, NULL, lv_color_black(), LV_OPA_COVER);
    tdongle_status_view.root = s_root;

    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_logo_img = lv_img_create(s_root);
    lv_img_set_src(s_logo_img, &ghostesplogo);
    lv_img_set_size_mode(s_logo_img, LV_IMG_SIZE_MODE_REAL);
    lv_img_set_zoom(s_logo_img, 192);
    lv_obj_center(s_logo_img);

    s_text_panel = lv_obj_create(s_root);
    lv_obj_set_size(s_text_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_text_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_text_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_text_panel, 0, 0);
    lv_obj_set_style_pad_all(s_text_panel, 0, 0);
    lv_obj_clear_flag(s_text_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_line1_label = lv_label_create(s_text_panel);
    lv_obj_set_width(s_line1_label, lv_pct(96));
    lv_obj_set_style_text_color(s_line1_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_line1_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_line1_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_line1_label, LV_ALIGN_CENTER, 0, -10);

    s_line2_label = lv_label_create(s_text_panel);
    lv_obj_set_width(s_line2_label, lv_pct(96));
    lv_obj_set_style_text_color(s_line2_label, lv_color_hex(0xB8C0CC), 0);
    lv_obj_set_style_text_align(s_line2_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_line2_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_line2_label, LV_ALIGN_CENTER, 0, 12);

    tdongle_status_apply_cached();
    s_idle_timer = lv_timer_create(tdongle_status_idle_timer_cb, TDONGLE_STATUS_IDLE_MS, NULL);
    tdongle_status_show_idle();
}

static void tdongle_status_destroy(void)
{
    if (s_idle_timer) {
        lv_timer_del(s_idle_timer);
        s_idle_timer = NULL;
    }
    if (s_root) {
        lv_obj_del(s_root);
    }
    s_root = NULL;
    s_logo_img = NULL;
    s_text_panel = NULL;
    s_line1_label = NULL;
    s_line2_label = NULL;
    tdongle_status_view.root = NULL;
}

View tdongle_status_view = {
    .root = NULL,
    .create = tdongle_status_create,
    .destroy = tdongle_status_destroy,
    .name = "T-Dongle Status",
    .get_hardwareinput_callback = NULL,
    .input_callback = NULL,
};
