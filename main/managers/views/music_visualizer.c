#include "managers/views/music_visualizer.h"

#include "gui/lvgl_safe.h"
#include "gui/view_input.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/wifi_manager.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bars[NUM_BARS];
    char track_name[33];
    char artist_name[33];
} AmplitudeFrame;

static const uint32_t STREAM_STALE_MS = 2500;
static const uint32_t ANIMATION_INTERVAL_MS = 16;

static MusicVisualizerView view;
static lv_obj_t *root = NULL;
static lv_obj_t *visual_canvas = NULL;
static lv_obj_t *debug_label = NULL;
static lv_timer_t *animation_timer = NULL;
static QueueHandle_t amplitude_queue = NULL;

static int current_amplitudes[NUM_BARS] = {0};
static int target_amplitudes[NUM_BARS] = {0};
static int peak_amplitudes[NUM_BARS] = {0};
static int display_amplitudes[NUM_BARS] = {0};
static int display_amplitudes_fp[NUM_BARS] = {0};
static uint32_t last_stream_tick = 0;
static uint32_t frame_counter = 0;
static uint32_t last_debug_label_tick = 0;
static char current_track[33] = "VISUALIZER";
static char current_artist[33] = "Waiting for signal";
static lv_color_t theme_bg = {0};
static lv_color_t theme_accent = {0};
static lv_color_t theme_text = {0};
static lv_color_t theme_muted = {0};

static uint32_t get_tick_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void copy_text_safely(char *dst, size_t dst_size, const char *src, const char *fallback) {
    const char *text = (src && src[0]) ? src : fallback;
    if (!text) text = "";
    strncpy(dst, text, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static lv_color_t mix_color(lv_color_t a, lv_color_t b, uint8_t amount) {
    lv_color32_t ca;
    lv_color32_t cb;
    ca.full = lv_color_to32(a);
    cb.full = lv_color_to32(b);
    uint16_t inv = (uint16_t)(255 - amount);
    return lv_color_make(
        (uint8_t)((ca.ch.red * inv + cb.ch.red * amount) / 255),
        (uint8_t)((ca.ch.green * inv + cb.ch.green * amount) / 255),
        (uint8_t)((ca.ch.blue * inv + cb.ch.blue * amount) / 255));
}

static void refresh_theme_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    theme_bg = lv_color_hex(theme_palette_get_background(theme));
    theme_accent = lv_color_hex(theme_palette_get_accent(theme));
    theme_text = lv_color_hex(theme_palette_get_text(theme));
    theme_muted = lv_color_hex(theme_palette_get_text_muted(theme));
}

static lv_color_t bar_color_for_index(int index) {
    int slot = (index * (THEME_PALETTE_SLOT_COUNT - 1)) / (NUM_BARS - 1);
    lv_color_t ramp = lv_color_hex(theme_palette_get(settings_get_menu_theme(&G_Settings), slot));
    uint8_t mix = (uint8_t)((index * 180) / (NUM_BARS - 1));
    return mix_color(ramp, theme_accent, mix);
}

static void update_status_text(void) {
    if (!view.status_label || !view.endpoint_label) return;

    uint32_t now = get_tick_ms();
    uint32_t age = last_stream_tick ? (now - last_stream_tick) : UINT32_MAX;
    if (age < STREAM_STALE_MS) {
        lv_label_set_text(view.status_label, current_track);
        lv_label_set_text(view.endpoint_label, current_artist);
    } else {
        lv_label_set_text(view.status_label, "VISUALIZER");
        lv_label_set_text(view.endpoint_label, "USB serial / UDP ready");
    }

    lv_obj_set_style_text_color(view.status_label, theme_text, 0);
    lv_obj_set_style_text_color(view.endpoint_label, theme_muted, 0);

    if (debug_label) {
        if (last_debug_label_tick == 0 || (now - last_debug_label_tick) >= 100) {
            char debug[96];
            snprintf(debug, sizeof(debug), "frames %lu  %s",
                     (unsigned long)frame_counter,
                     age < STREAM_STALE_MS ? "streaming" : "idle");
            lv_label_set_text(debug_label, debug);
            lv_obj_set_style_text_color(debug_label, theme_muted, 0);
            last_debug_label_tick = now;
        }
    }
}

static void visual_canvas_draw_event(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    if (!obj || obj != visual_canvas) return;

    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !draw_ctx->clip_area) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);

    int width = coords.x2 - coords.x1 + 1;
    int height = coords.y2 - coords.y1 + 1;
    int center_x = coords.x1 + (width / 2);
    int center_y = coords.y1 + (height / 2);

    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_opa = LV_OPA_COVER;
    bg_dsc.bg_color = theme_bg;
    bg_dsc.border_width = 0;
    bg_dsc.radius = 0;
    lv_draw_rect(draw_ctx, &bg_dsc, &coords);

    int strongest = 0;
    for (int i = 0; i < NUM_BARS; i++) {
        if (display_amplitudes[i] > strongest) strongest = display_amplitudes[i];
    }
    if (strongest < 24) {
        strongest = 24 + (int)((get_tick_ms() / 36U) % 64U);
    }

    int glow_size = (height > 128 ? 26 : 16) + strongest / 10;
    lv_area_t center_glow = {
        .x1 = center_x - (glow_size / 2),
        .y1 = center_y - (glow_size / 2),
        .x2 = center_x + (glow_size / 2),
        .y2 = center_y + (glow_size / 2),
    };
    bg_dsc.radius = LV_RADIUS_CIRCLE;
    bg_dsc.bg_opa = (lv_opa_t)(8 + strongest / 14);
    bg_dsc.bg_color = mix_color(theme_accent, lv_color_white(), 80);
    lv_draw_rect(draw_ctx, &bg_dsc, &center_glow);

    int title_y = (height > 128 ? 10 : 4);
    int subtitle_y = title_y + (height > 128 ? 22 : 14);
    int gap = (width > 180) ? 4 : 2;
    int spectrum_top = coords.y1 + subtitle_y + (height > 128 ? 16 : 10);
    int half_height = center_y - spectrum_top;
    if (half_height < 16) half_height = 16;

    int left_pad = width > 180 ? 12 : 6;
    int usable_w = width - (left_pad * 2);
    int bar_width = (usable_w - ((NUM_BARS - 1) * gap)) / NUM_BARS;
    if (bar_width < 3) bar_width = 3;
    int total_w = (bar_width * NUM_BARS) + (gap * (NUM_BARS - 1));
    int start_x = coords.x1 + ((width - total_w) / 2);
    if (start_x < (coords.x1 + 2)) start_x = coords.x1 + 2;

    lv_draw_rect_dsc_t bar_dsc;
    lv_draw_rect_dsc_init(&bar_dsc);
    bar_dsc.border_width = 0;

    lv_draw_rect_dsc_t peak_dsc;
    lv_draw_rect_dsc_init(&peak_dsc);
    peak_dsc.border_width = 0;
    peak_dsc.bg_opa = LV_OPA_90;

    lv_draw_rect_dsc_t glow_dsc;
    lv_draw_rect_dsc_init(&glow_dsc);
    glow_dsc.border_width = 0;
    glow_dsc.radius = LV_RADIUS_CIRCLE;
    glow_dsc.bg_opa = (lv_opa_t)26;

    for (int i = 0; i < NUM_BARS; i++) {
        int amp = display_amplitudes[i];
        int peak = peak_amplitudes[i];

        int capped_amp = (amp * 190) / 255;
        int capped_peak = (peak * 190) / 255;
        int amp_h = (capped_amp * half_height) / 255;
        int peak_h = (capped_peak * half_height) / 255;
        if (amp_h < 6) amp_h = 6;
        if (peak_h < amp_h) peak_h = amp_h;

        int x = start_x + (i * (bar_width + gap));
        int bar_top = center_y - amp_h;
        int peak_y = center_y - peak_h;
        lv_color_t color = bar_color_for_index(i);

        bar_dsc.bg_color = color;
        bar_dsc.bg_opa = (lv_opa_t)(180 + ((i * 64) / NUM_BARS));
        bar_dsc.radius = bar_width > 6 ? 2 : 1;
        lv_area_t bar_area = {
            .x1 = x,
            .y1 = bar_top,
            .x2 = x + bar_width - 1,
            .y2 = bar_top + (amp_h * 2) - 1,
        };
        lv_draw_rect(draw_ctx, &bar_dsc, &bar_area);

        peak_dsc.bg_color = mix_color(theme_text, color, 40);
        lv_area_t peak_area = {
            .x1 = x,
            .y1 = peak_y,
            .x2 = x + bar_width - 1,
            .y2 = peak_y + 1,
        };
        lv_draw_rect(draw_ctx, &peak_dsc, &peak_area);

        glow_dsc.bg_color = color;
        lv_area_t glow_area = {
            .x1 = x - 1,
            .y1 = center_y - amp_h - (amp_h / 4),
            .x2 = x + bar_width,
            .y2 = center_y + amp_h + (amp_h / 4),
        };
        lv_draw_rect(draw_ctx, &glow_dsc, &glow_area);
    }
}

static void animation_timer_callback(lv_timer_t *timer) {
    (void)timer;

    bool got_frame = false;
    AmplitudeFrame frame;
    while (amplitude_queue && xQueueReceive(amplitude_queue, &frame, 0) == pdTRUE) {
        for (int i = 0; i < NUM_BARS; i++) {
            target_amplitudes[i] = frame.bars[i];
            if (frame.bars[i] > peak_amplitudes[i]) {
                peak_amplitudes[i] = frame.bars[i];
            }
        }
        copy_text_safely(current_track, sizeof(current_track), frame.track_name, "LIVE INPUT");
        copy_text_safely(current_artist, sizeof(current_artist), frame.artist_name, "Desktop Audio");
        last_stream_tick = get_tick_ms();
        got_frame = true;
    }

    for (int i = 0; i < NUM_BARS; i++) {
        if (current_amplitudes[i] < target_amplitudes[i]) {
            int delta = target_amplitudes[i] - current_amplitudes[i];
            current_amplitudes[i] += (delta * 3) / 4;
        } else {
            int delta = current_amplitudes[i] - target_amplitudes[i];
            current_amplitudes[i] -= (delta * 2) / 5;
        }

        if (peak_amplitudes[i] > current_amplitudes[i]) {
            peak_amplitudes[i] -= 8;
            if (peak_amplitudes[i] < current_amplitudes[i]) peak_amplitudes[i] = current_amplitudes[i];
        }

        if (!got_frame && target_amplitudes[i] > 0) {
            target_amplitudes[i] -= 6;
            if (target_amplitudes[i] < 0) target_amplitudes[i] = 0;
        }

        int target_fp = current_amplitudes[i] * 256;
        int delta_fp = target_fp - display_amplitudes_fp[i];
        if (delta_fp > 0) {
            int step_fp = (delta_fp * 5) / 8;
            if (step_fp < 48) step_fp = 48;
            display_amplitudes_fp[i] += step_fp;
            if (display_amplitudes_fp[i] > target_fp) display_amplitudes_fp[i] = target_fp;
        } else if (delta_fp < 0) {
            int step_fp = ((-delta_fp) * 3) / 10;
            if (step_fp < 24) step_fp = 24;
            display_amplitudes_fp[i] -= step_fp;
            if (display_amplitudes_fp[i] < target_fp) display_amplitudes_fp[i] = target_fp;
        }

        display_amplitudes[i] = display_amplitudes_fp[i] / 256;
    }

    update_status_text();
    if (visual_canvas) lv_obj_invalidate(visual_canvas);
}

static void return_to_apps(void) {
    /* Reachable from Apps Gallery as well as CLI/serial "startvisualizer"
     * commands, so back must return to whichever actually opened this. */
    display_manager_go_back();
}

static void handle_hardware_input_music_callback(InputEvent *event) {
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            return_to_apps();
        }
        return;
    }

    if (view_input_wants_back(event)) {
        return_to_apps();
        return;
    }
    if (event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 0) {
        return_to_apps();
        return;
    }
#if defined(CONFIG_USE_ENCODER)
    if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
        return_to_apps();
        return;
    }
#endif
    /* Ignore all other input (joystick motion, rotation, keys) so the
     * visualizer cannot be dismissed accidentally. */
}

static void get_music_visualizer_callback(void **callback) {
    *callback = music_visualizer_view.input_callback;
}

View music_visualizer_view = {
    .root = NULL,
    .create = music_visualizer_view_create,
    .destroy = music_visualizer_destroy,
    .input_callback = handle_hardware_input_music_callback,
    .name = "Visualizer",
    .get_hardwareinput_callback = get_music_visualizer_callback,
};

void music_visualizer_view_create(void) {
    wifi_manager_start_visualizer(true);
    refresh_theme_colors();

    root = gui_screen_create_root(NULL, NULL, theme_bg, LV_OPA_COVER);
    music_visualizer_view.root = root;
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_bg_color(root, theme_bg, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    visual_canvas = lv_obj_create(root);
    lv_obj_set_size(visual_canvas, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(visual_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(visual_canvas, 0, 0);
    lv_obj_set_style_pad_all(visual_canvas, 0, 0);
    lv_obj_set_style_radius(visual_canvas, 0, 0);
    lv_obj_clear_flag(visual_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(visual_canvas, visual_canvas_draw_event, LV_EVENT_DRAW_MAIN, NULL);

    view.status_label = lv_label_create(root);
    lv_obj_set_style_text_font(view.status_label,
                               (LV_VER_RES > 180) ? accessibility_get_font_title() : accessibility_get_font_body(),
                               0);
    lv_obj_set_style_text_color(view.status_label, theme_text, 0);
    lv_obj_align(view.status_label, LV_ALIGN_TOP_MID, 0, LV_VER_RES > 180 ? 12 : 6);
    lv_label_set_text(view.status_label, current_track);

    view.endpoint_label = lv_label_create(root);
    lv_obj_set_style_text_font(view.endpoint_label,
                               (LV_VER_RES > 180) ? accessibility_get_font_body() : accessibility_get_font_small(),
                               0);
    lv_obj_set_style_text_color(view.endpoint_label, theme_muted, 0);
    lv_obj_align(view.endpoint_label, LV_ALIGN_TOP_MID, 0, LV_VER_RES > 180 ? 40 : 24);
    lv_label_set_text(view.endpoint_label, current_artist);

    debug_label = lv_label_create(root);
    lv_obj_set_style_text_font(debug_label, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(debug_label, theme_muted, 0);
    lv_obj_align(debug_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 4));
    lv_label_set_text(debug_label, "frames 0  idle");

    if (!amplitude_queue) {
        amplitude_queue = xQueueCreate(1, sizeof(AmplitudeFrame));
    }

    update_status_text();
    lv_obj_invalidate(visual_canvas);

    if (!animation_timer) {
        animation_timer = lv_timer_create(animation_timer_callback, ANIMATION_INTERVAL_MS, NULL);
    }
}

void music_visualizer_view_update(const uint8_t *amplitudes,
                                  const char *track_name,
                                  const char *artist_name) {
    if (!amplitudes || !amplitude_queue) return;

    AmplitudeFrame frame;
    memset(&frame, 0, sizeof(frame));
    for (int i = 0; i < NUM_BARS; i++) {
        frame.bars[i] = amplitudes[i];
    }

    copy_text_safely(frame.track_name, sizeof(frame.track_name), track_name, "LIVE INPUT");
    copy_text_safely(frame.artist_name, sizeof(frame.artist_name), artist_name, "Desktop Audio");
    frame_counter++;
    xQueueOverwrite(amplitude_queue, &frame);
}

void music_visualizer_destroy(void) {
    wifi_manager_stop_visualizer();
    lvgl_timer_del_safe(&animation_timer);
    animation_timer = NULL;

    if (amplitude_queue) {
        vQueueDelete(amplitude_queue);
        amplitude_queue = NULL;
    }

    lvgl_obj_del_safe(&root);
    root = NULL;
    visual_canvas = NULL;
    debug_label = NULL;
    memset(&view, 0, sizeof(view));
    memset(current_amplitudes, 0, sizeof(current_amplitudes));
    memset(target_amplitudes, 0, sizeof(target_amplitudes));
    memset(peak_amplitudes, 0, sizeof(peak_amplitudes));
    memset(display_amplitudes, 0, sizeof(display_amplitudes));
    memset(display_amplitudes_fp, 0, sizeof(display_amplitudes_fp));
    last_stream_tick = 0;
    frame_counter = 0;
    last_debug_label_tick = 0;
    copy_text_safely(current_track, sizeof(current_track), NULL, "VISUALIZER");
    copy_text_safely(current_artist, sizeof(current_artist), NULL, "Waiting for signal");
    music_visualizer_view.root = NULL;
}
