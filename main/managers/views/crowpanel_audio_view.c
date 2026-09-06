#include "sdkconfig.h"

#if defined(CONFIG_CROWPANEL_1P28_ROTARY) && defined(CONFIG_HAS_BADUSB)

#include "managers/views/crowpanel_audio_view.h"

#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/lvgl_safe.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/badusb_manager.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>

static lv_obj_t *s_root;
static lv_obj_t *s_arc;
static lv_obj_t *s_value_label;
static lv_obj_t *s_state_label;
static lv_obj_t *s_back_button;
static lv_obj_t *s_back_label;
static lv_timer_t *s_timer;
static lv_timer_t *s_wheel_timer;
static bool s_started_usb;
static bool s_muted;
static uint32_t s_feedback_until;
static int32_t s_wheel_target = 270;
static int32_t s_wheel_draw = 270;
static TaskHandle_t s_hid_task;
static volatile bool s_hid_stop;
static portMUX_TYPE s_hid_lock = portMUX_INITIALIZER_UNLOCKED;
static int32_t s_pending_volume_steps;
static uint8_t s_pending_mute_presses;
static int8_t s_last_volume_direction;

static lv_color_t s_background;
static lv_color_t s_surface;
static lv_color_t s_surface_alt;
static lv_color_t s_text;
static lv_color_t s_muted_text;
static lv_color_t s_accent;

static void crowpanel_audio_create(void);
static void crowpanel_audio_destroy(void);

static void audio_wheel_timer_cb(lv_timer_t *timer) {
    (void)timer;
    int32_t delta = s_wheel_target - s_wheel_draw;
    if (delta == 0 || !s_arc) return;

    /* Stay visibly attached to the knob while retaining sub-frame easing. */
    int32_t step = delta / 2;
    if (step == 0) step = delta > 0 ? 1 : -1;
    s_wheel_draw += step;
    int32_t angle = s_wheel_draw % 360;
    if (angle < 0) angle += 360;
    lv_arc_set_rotation(s_arc, (uint16_t)angle);
}

static void audio_animate_wheel(int direction) {
    if (!s_arc || direction == 0) return;
    s_wheel_target += direction > 0 ? 12 : -12;
}

static void audio_hid_task(void *arg) {
    (void)arg;
    while (!s_hid_stop) {
        uint8_t control = 0;

        portENTER_CRITICAL(&s_hid_lock);
        /* Mute is latency-sensitive, so let a press jump ahead of a large
         * volume burst. Opposing fast turns collapse to their net movement
         * instead of replaying a stale backlog after the wheel has reversed. */
        if (s_pending_mute_presses > 0) {
            s_pending_mute_presses--;
            control = BADUSB_CONSUMER_MUTE;
        } else if (s_pending_volume_steps > 0) {
            s_pending_volume_steps--;
            control = BADUSB_CONSUMER_VOLUME_UP;
        } else if (s_pending_volume_steps < 0) {
            s_pending_volume_steps++;
            control = BADUSB_CONSUMER_VOLUME_DOWN;
        }
        portEXIT_CRITICAL(&s_hid_lock);

        if (control != 0) {
            (void)badusb_manager_send_consumer(control);
        } else {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(40));
        }
    }
    s_hid_task = NULL;
    vTaskDelete(NULL);
}

static void audio_send_consumer(uint8_t control) {
    if (!badusb_manager_is_consumer_mode()) return;
    if (!s_hid_task) {
        (void)badusb_manager_send_consumer(control);
        return;
    }

    portENTER_CRITICAL(&s_hid_lock);
    if (control == BADUSB_CONSUMER_VOLUME_UP) {
        if (s_pending_volume_steps < 1024) s_pending_volume_steps++;
    } else if (control == BADUSB_CONSUMER_VOLUME_DOWN) {
        if (s_pending_volume_steps > -1024) s_pending_volume_steps--;
    } else if (control == BADUSB_CONSUMER_MUTE) {
        if (s_pending_mute_presses < UINT8_MAX) s_pending_mute_presses++;
    }
    portEXIT_CRITICAL(&s_hid_lock);
    xTaskNotifyGive(s_hid_task);
}

static void audio_set_idle_ui(void) {
    if (!s_arc || !s_value_label) return;
    lv_label_set_text(s_value_label, "VOLUME");
    lv_obj_set_style_text_color(s_value_label, s_text, 0);
    s_feedback_until = 0;
    s_last_volume_direction = 0;
}

static void audio_show_action(const char *text) {
    if (!s_arc || !s_value_label) return;
    lv_label_set_text(s_value_label, text);
    lv_obj_set_style_text_color(s_value_label, s_accent, 0);
    s_feedback_until = lv_tick_get() + 450;
}

static void audio_update_state_ui(void) {
    if (!s_state_label) return;

    if (!badusb_manager_is_active()) {
        lv_label_set_text(s_state_label, s_started_usb ? "CONNECT USB" : "USB OFFLINE");
        return;
    }
    if (!badusb_manager_is_consumer_mode()) {
        lv_label_set_text(s_state_label, "USB BUSY");
    } else if (!badusb_manager_is_usb_mounted()) {
        lv_label_set_text(s_state_label, "CONNECT USB");
    } else {
        lv_label_set_text(s_state_label, s_muted ? "UNMUTE" : "MUTE");
    }
}

static void audio_timer_cb(lv_timer_t *timer) {
    (void)timer;
    audio_update_state_ui();
    if (s_feedback_until != 0 &&
        (int32_t)(lv_tick_get() - s_feedback_until) >= 0) {
        audio_set_idle_ui();
    }
}

static void audio_mute_cb(lv_event_t *event) {
    (void)event;
    audio_send_consumer(BADUSB_CONSUMER_MUTE);
    s_muted = !s_muted;
    s_last_volume_direction = 0;
    audio_show_action(s_muted ? "MUTED" : "LIVE");
    audio_update_state_ui();
}

static void audio_back_cb(lv_event_t *event) {
    (void)event;
    display_manager_go_back();
}

static void audio_adjust(int direction) {
    if (direction == 0) return;
    audio_animate_wheel(direction);
    audio_send_consumer(direction > 0
        ? BADUSB_CONSUMER_VOLUME_UP
        : BADUSB_CONSUMER_VOLUME_DOWN);
    int8_t direction_sign = direction > 0 ? 1 : -1;
    if (s_last_volume_direction != direction_sign) {
        audio_show_action(direction_sign > 0 ? "VOL +" : "VOL -");
        s_last_volume_direction = direction_sign;
    } else {
        /* Extending the feedback deadline does not invalidate/re-layout text. */
        s_feedback_until = lv_tick_get() + 450;
    }
}

static void crowpanel_audio_input(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) audio_mute_cb(NULL);
        else audio_adjust(event->data.encoder.direction);
        return;
    }

    if (event->type == INPUT_TYPE_TOUCH &&
        event->data.touch_data.state == LV_INDEV_STATE_REL) {
        const lv_point_t point = event->data.touch_data.point;
        if (point.x >= 82 && point.x <= 158 &&
            point.y >= 154 && point.y <= 200) {
            display_manager_go_back();
        }
        return;
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON ||
        event->type == INPUT_TYPE_JOYSTICK) {
        display_manager_go_back();
    }
}

static void crowpanel_audio_get_callback(void **callback) {
    if (callback) *callback = crowpanel_audio_input;
}

View crowpanel_audio_view = {
    .root = NULL,
    .create = crowpanel_audio_create,
    .destroy = crowpanel_audio_destroy,
    .input_callback = crowpanel_audio_input,
    .name = "USB Audio",
    .get_hardwareinput_callback = crowpanel_audio_get_callback,
};

static void crowpanel_audio_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    s_background = lv_color_hex(theme_palette_get_background(theme));
    s_surface = lv_color_hex(theme_palette_get_surface(theme));
    s_surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    s_text = lv_color_hex(theme_palette_get_text(theme));
    s_muted_text = lv_color_hex(theme_palette_get_text_muted(theme));
    s_accent = lv_color_hex(theme_palette_get_accent(theme));

    s_root = gui_screen_create_root(NULL, NULL, s_background, LV_OPA_COVER);
    crowpanel_audio_view.root = s_root;
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    display_manager_set_status_bar_hidden(true);

    s_arc = lv_arc_create(s_root);
    /* The bezel is the natural affordance on this board: a near-edge ring
     * makes the 240px circular panel read as one physical control surface. */
    /* Keep the active arc inside the upper-left Back target. The larger
     * near-bezel ring looked attractive, but its upper-left sweep competed
     * with the touch affordance on the circular panel. */
    lv_obj_set_size(s_arc, 176, 176);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, 8);
    s_wheel_target = 270;
    s_wheel_draw = 270;
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 100);
    /* A fixed bright segment behaves like a physical jog-wheel marker. It
     * rotates with the encoder but does not claim to represent host volume. */
    lv_arc_set_value(s_arc, 24);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, s_surface_alt, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, s_text, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *center_disc = lv_obj_create(s_root);
    lv_obj_set_size(center_disc, 136, 136);
    lv_obj_align(center_disc, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_color(center_disc, s_surface, 0);
    lv_obj_set_style_bg_opa(center_disc, LV_OPA_90, 0);
    lv_obj_set_style_border_width(center_disc, 1, 0);
    lv_obj_set_style_border_color(center_disc, s_surface_alt, 0);
    lv_obj_set_style_border_opa(center_disc, LV_OPA_70, 0);
    lv_obj_set_style_radius(center_disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(center_disc, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_value_label = lv_label_create(s_root);
    lv_label_set_text(s_value_label, "VOLUME");
    lv_obj_set_style_text_font(s_value_label, accessibility_get_font_title(), 0);
    lv_obj_set_style_text_color(s_value_label, s_text, 0);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -10);

    s_back_button = lv_btn_create(s_root);
    lv_obj_set_size(s_back_button, 60, 28);
    lv_obj_align(s_back_button, LV_ALIGN_CENTER, 0, 58);
    lv_obj_set_style_bg_color(s_back_button, s_surface, 0);
    lv_obj_set_style_bg_opa(s_back_button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(s_back_button, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(s_back_button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_back_button, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_back_button, 1, 0);
    lv_obj_set_style_border_color(s_back_button, s_surface_alt, 0);
    lv_obj_set_style_radius(s_back_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(s_back_button, audio_back_cb, LV_EVENT_CLICKED, NULL);
    s_back_label = lv_label_create(s_back_button);
    lv_label_set_text(s_back_label, LV_SYMBOL_LEFT " BACK");
    lv_obj_set_style_text_font(s_back_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_back_label, s_text, 0);
    lv_obj_center(s_back_label);

    s_state_label = lv_label_create(s_root);
    lv_obj_set_style_text_font(s_state_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(s_state_label, s_muted_text, 0);
    lv_obj_align(s_state_label, LV_ALIGN_CENTER, 0, 22);

    s_muted = false;

    s_hid_stop = false;
    s_pending_volume_steps = 0;
    s_pending_mute_presses = 0;
    s_last_volume_direction = 0;
    if (xTaskCreate(audio_hid_task, "usb_audio_hid", 3072, NULL, 5,
                    &s_hid_task) != pdPASS) {
        s_hid_task = NULL;
    }

    if (!badusb_manager_is_active() && !badusb_manager_is_consumer_mode()) {
        esp_err_t usb_result = badusb_manager_consumer_start();
        s_started_usb = (usb_result == ESP_OK);
        if (usb_result != ESP_OK) {
            lv_label_set_text(s_state_label, "USB ERROR");
        }
    }
    audio_set_idle_ui();
    audio_update_state_ui();
    s_timer = lv_timer_create(audio_timer_cb, 500, NULL);
    s_wheel_timer = lv_timer_create(audio_wheel_timer_cb, 8, NULL);
}

static void crowpanel_audio_destroy(void) {
    lvgl_timer_del_safe(&s_timer);
    lvgl_timer_del_safe(&s_wheel_timer);
    s_hid_stop = true;
    for (int i = 0; i < 30 && s_hid_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_hid_task) {
        vTaskDelete(s_hid_task);
        s_hid_task = NULL;
    }
    s_pending_volume_steps = 0;
    s_pending_mute_presses = 0;
    if (s_started_usb) {
        (void)badusb_manager_consumer_stop();
        s_started_usb = false;
    }
    display_manager_set_status_bar_hidden(false);
    lvgl_obj_del_safe(&s_root);
    s_arc = NULL;
    s_value_label = NULL;
    s_state_label = NULL;
    s_back_button = NULL;
    s_back_label = NULL;
    crowpanel_audio_view.root = NULL;
}

#endif
