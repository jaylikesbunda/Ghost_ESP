#include "managers/views/audio_player_screen.h"
#include "managers/audio_stream_manager.h"
#include "managers/audio_i2s_output.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/display_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"
#include "gui/screen_layout.h"
#include "gui/touch_bar.h"
#include "gui/lvgl_safe.h"
#include "gui/toast.h"
#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "gui/design_tokens.h"
#ifdef CONFIG_HAS_TLV320DAC_I2C
#include "io_manager.h"
#include "tlv320dac3100.h"
#endif
#include "freertos/task.h"

#ifdef CONFIG_HAS_AUDIO_PLAYER

static const char *TAG = "AudioPlayer";

/* Audio Player is reachable both from Apps Gallery and directly from the
 * Main Menu's "Audio" item; back/ESC must return to whichever one actually
 * opened it, not a fixed destination. */
static View *s_return_view = NULL;

void audio_player_set_return_view(View *view) {
    s_return_view = view;
}

/* Bottom touch bar, same style as other views (badusb/settings/etc): a
 * full-width bar with a centered Back button. Height matches
 * SCROLL_BTN_SIZE + 2*SCROLL_BTN_PADDING used by those views. */
#define AUDIO_TOUCH_BAR_H 34
#if GUI_LARGE_TOUCH_UI
#define AUDIO_BOTTOM_SAFE_H GUI_HOME_SAFE_H
#else
#define AUDIO_BOTTOM_SAFE_H AUDIO_TOUCH_BAR_H
#endif

/* The view has two full-screen sub-screens: a Library (track list) and a
 * dedicated Now Playing screen. Tapping a track opens Now Playing; its back
 * control returns to the Library; the Library's back exits the whole view. */
typedef enum {
    SCREEN_LIBRARY = 0,
    SCREEN_NOWPLAYING,
} audio_screen_t;

static audio_screen_t s_screen = SCREEN_LIBRARY;

/* UI element handles */
static lv_obj_t *s_root = NULL;
static lv_obj_t *s_library_cont = NULL;
static lv_obj_t *s_np_cont = NULL;

/* Library */
static lv_obj_t *s_file_list = NULL;
static lv_obj_t *s_list_hint_label = NULL;
static lv_obj_t *s_lib_back_btn = NULL;
static gui_touch_bar_t s_lib_tb;

/* Now Playing */
static lv_obj_t *s_np_title = NULL;
static lv_obj_t *s_meta_label = NULL;
static lv_obj_t *s_progress_track = NULL;
static lv_obj_t *s_progress_fill = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_play_btn = NULL;
static lv_obj_t *s_pause_btn = NULL;
static lv_obj_t *s_prev_btn = NULL;
static lv_obj_t *s_next_btn = NULL;
static lv_obj_t *s_np_back_btn = NULL;
static gui_touch_bar_t s_np_tb;
static lv_obj_t *s_vol_track = NULL;
static lv_obj_t *s_vol_fill = NULL;
static lv_obj_t *s_vol_minus_btn = NULL;
static lv_obj_t *s_vol_plus_btn = NULL;
static lv_obj_t *s_volume_label = NULL;

/* State */
static int s_selected_index = 0;
static int s_visible_count = 0;
static uint8_t s_volume_percent = 85;
static lv_timer_t *s_update_timer = NULL;
static bool s_vol_dragging = false;

static touch_drag_t s_touch_drag = {0};

static lv_color_t s_bg_color;
static lv_color_t s_surface_color;
static lv_color_t s_surface_alt_color;
static lv_color_t s_text_color;
static lv_color_t s_dim_color;
static lv_color_t s_accent_color;
static int s_last_rendered_playing_index = -2;
static audio_stream_state_t s_last_rendered_state = AUDIO_STREAM_STATE_IDLE;

/* Forward declarations */
static void refresh_theme_colors(void);
static void create_file_list(void);
static void update_file_list_selection(void);
static void audio_player_update_status(void);
static void update_timer_cb(lv_timer_t *timer);
static void audio_player_go_back(void);
static void show_library(void);
static void show_nowplaying(void);
static void apply_volume(uint8_t pct);
static void update_volume_ui(void);
static void adjust_volume(int delta);
static bool play_track_with_toast(int index);
static bool change_track_with_toast(bool next);
static bool point_in_obj(lv_obj_t *obj, int x, int y);
static lv_obj_t *make_round_btn(lv_obj_t *parent, int w, int h, const char *sym, lv_color_t txt);

static void refresh_theme_colors(void)
{
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    s_bg_color = lv_color_hex(theme_palette_get_background(theme));
    s_surface_color = lv_color_hex(theme_palette_get_surface(theme));
    s_surface_alt_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    s_text_color = lv_color_hex(theme_palette_get_text(theme));
    s_dim_color = lv_color_hex(theme_palette_get_text_muted(theme));
    s_accent_color = lv_color_hex(theme_palette_get_accent(theme));
}

static void style_track_row(lv_obj_t *obj, bool selected, bool playing)
{
    lv_obj_set_style_bg_color(obj, (selected || playing) ? s_surface_alt_color : s_surface_color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, playing ? s_accent_color : (selected ? s_dim_color : s_surface_alt_color), 0);
    lv_obj_set_style_border_width(obj, playing ? 2 : 1, 0);
    lv_obj_set_style_border_side(obj, (selected || playing) ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_NONE, 0);
    lv_obj_set_style_radius(obj, 9, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void update_list_hint(void)
{
    if (!s_list_hint_label) return;

    int total = audio_stream_manager_get_file_count();
    if (total <= 0) {
        lv_label_set_text(s_list_hint_label, "0 tracks");
        return;
    }

    char text[32];
    snprintf(text, sizeof(text), "%d/%d", s_selected_index + 1, total);
    lv_label_set_text(s_list_hint_label, text);
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

static bool point_in_obj(lv_obj_t *obj, int x, int y)
{
    return gui_touch_bar_hit(obj, x, y);
}

static void check_sd_and_show_toast(void)
{
    if (audio_stream_manager_get_file_count() > 0) {
        return;
    }
    if (!audio_stream_manager_sd_available()) {
        toast_show("No SD card - insert card with MP3s in /audio", TOAST_WARN);
    } else {
        toast_show("No MP3 files found in /audio", TOAST_INFO);
    }
}

static void format_time(char *buf, size_t buf_len, size_t seconds)
{
    if (seconds >= 3600) {
        snprintf(buf, buf_len, "%lu:%02lu:%02lu",
                 (unsigned long)(seconds / 3600),
                 (unsigned long)((seconds % 3600) / 60),
                 (unsigned long)(seconds % 60));
    } else {
        snprintf(buf, buf_len, "%lu:%02lu",
                 (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    }
}

static uint32_t get_estimated_playback_ms(size_t pos, size_t total_size, uint32_t duration_ms)
{
    /* Once the local receiver or GhostLink peer has reported a playback
     * position at all, trust it completely - including 0 while the
     * prebuffer fills at the start of a track. The byte-offset fallbacks
     * below track the sender, which runs far ahead of the audible position
     * during the initial prebuffer and would make the bar jump around. */
    if (audio_stream_manager_has_receiver_feedback()) {
        return audio_stream_manager_get_playback_ms();
    }

    /* No feedback ever (e.g. a peer that never reports): estimate from the
     * sender-side offsets. */
    uint16_t bitrate = audio_stream_manager_get_bitrate();
    if (bitrate > 0) {
        uint32_t bytes_per_sec = (uint32_t)bitrate * 125;
        if (bytes_per_sec > 0) return (uint32_t)(((uint64_t)pos * 1000) / bytes_per_sec);
    }

    if (total_size > 0 && duration_ms > 0) {
        return (uint32_t)(((uint64_t)pos * duration_ms) / total_size);
    }

    return 0;
}

/* ---------------- Volume ---------------- */

static void update_volume_ui(void)
{
    if (s_vol_track && s_vol_fill && lv_obj_is_valid(s_vol_track)) {
        lv_coord_t w = lv_obj_get_width(s_vol_track);
        lv_coord_t h = lv_obj_get_height(s_vol_track);
        lv_obj_set_size(s_vol_fill, (w * s_volume_percent) / 100, h);
    }
    if (s_volume_label) {
        char vbuf[16];
        snprintf(vbuf, sizeof(vbuf), "Vol %u%%", (unsigned)s_volume_percent);
        lv_label_set_text(s_volume_label, vbuf);
    }
}

static void apply_volume(uint8_t pct)
{
    if (pct > 100) pct = 100;
#ifdef CONFIG_HAS_TLV320DAC_I2C
    esp_err_t ret = tlv320dac3100_set_volume(pct);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Volume set failed: %s", esp_err_to_name(ret));
    }
#else
    /* Routed by the I2S output: the AW88298 has its own attenuator and uses
     * it, other codecs fall back to software PCM scaling. */
    audio_i2s_output_set_volume(pct);
#endif
    s_volume_percent = pct;
    update_volume_ui();
}

static void adjust_volume(int delta)
{
    int volume = (int)s_volume_percent + delta;
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (volume == (int)s_volume_percent) return;
    apply_volume((uint8_t)volume);
    ESP_LOGI(TAG, "Audio volume: %u%%", (unsigned)s_volume_percent);
}

/* Map an absolute touch x within the volume track to a 0-100 percentage. */
static void set_volume_from_x(int x)
{
    if (!s_vol_track || !lv_obj_is_valid(s_vol_track)) return;
    lv_area_t a;
    lv_obj_get_coords(s_vol_track, &a);
    int w = a.x2 - a.x1;
    if (w <= 0) return;
    int rel = x - a.x1;
    if (rel < 0) rel = 0;
    if (rel > w) rel = w;
    apply_volume((uint8_t)((rel * 100) / w));
}

/* ---------------- Library list ---------------- */

static void create_file_list(void)
{
    int file_count = audio_stream_manager_get_file_count();
    s_visible_count = file_count;
    s_last_rendered_playing_index = audio_stream_manager_get_current_index();
    s_last_rendered_state = audio_stream_manager_get_state();

    if (!s_file_list) return;

    lv_obj_clean(s_file_list);

    if (file_count == 0) {
        lv_obj_t *lbl = lv_label_create(s_file_list);
        lv_label_set_text(lbl, "No MP3 files found");
        lv_obj_set_style_text_color(lbl, s_dim_color, 0);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_body(), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    audio_stream_state_t state = audio_stream_manager_get_state();
    int playing_index = audio_stream_manager_get_current_index();
    bool has_playing = (state == AUDIO_STREAM_STATE_PLAYING || state == AUDIO_STREAM_STATE_PAUSED) &&
                       playing_index >= 0 && playing_index < file_count;

    for (int i = 0; i < file_count; i++) {
        const char *fname = audio_stream_manager_get_filename(i);
        if (!fname) continue;

        bool selected = i == s_selected_index;
        bool playing = has_playing && i == playing_index;
        lv_obj_t *btn = lv_btn_create(s_file_list);
        gui_apply_pressed_style(btn);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                          LV_VER_RES >= 400 ? 56 : 44
#else
                          LV_VER_RES <= 160 ? 30 : 34
#endif
        );
        style_track_row(btn, selected, playing);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        char idx_text[8];
        snprintf(idx_text, sizeof(idx_text), playing ? ">" : "%02d", i + 1);
        lv_obj_t *idx_lbl = lv_label_create(btn);
        lv_label_set_text(idx_lbl, idx_text);
        lv_obj_set_style_text_color(idx_lbl, playing ? s_accent_color : s_dim_color, 0);
        lv_obj_set_style_text_font(idx_lbl, accessibility_get_font_small(), 0);
        lv_obj_align(idx_lbl, LV_ALIGN_LEFT_MID, 7, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, fname);
        lv_obj_set_style_text_color(lbl, playing ? s_accent_color : (selected ? s_text_color : s_dim_color), 0);
        lv_obj_set_style_text_font(lbl,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                                    accessibility_get_font_body(),
#else
                                    accessibility_get_font_small(),
#endif
                                    0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(lbl,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                         ((LV_HOR_RES * 43) / 100) - 112
#else
                         LV_HOR_RES - 92
#endif
        );
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                     46,
#else
                     34,
#endif
                     0);

        lv_obj_t *badge = lv_label_create(btn);
        lv_label_set_text(badge, playing ? (state == AUDIO_STREAM_STATE_PAUSED ? "PAUSE" : "PLAY") : (selected ? "SEL" : ""));
        lv_obj_set_style_text_color(badge, playing ? s_accent_color : s_dim_color, 0);
        lv_obj_set_style_text_font(badge, accessibility_get_font_small(), 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -8, 0);
    }

    update_list_hint();
}

static void update_file_list_selection(void)
{
    if (!s_file_list) return;
    if (s_visible_count <= 0) {
        update_list_hint();
        return;
    }

    uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
    audio_stream_state_t state = audio_stream_manager_get_state();
    int playing_index = audio_stream_manager_get_current_index();
    bool has_playing = (state == AUDIO_STREAM_STATE_PLAYING || state == AUDIO_STREAM_STATE_PAUSED) &&
                       playing_index >= 0 && playing_index < s_visible_count;

    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, i);
        if (!btn) continue;
        bool selected = ((int)i == s_selected_index);
        bool playing = has_playing && ((int)i == playing_index);
        style_track_row(btn, selected, playing);

        lv_obj_t *idx_lbl = lv_obj_get_child(btn, 0);
        if (idx_lbl) {
            char idx_text[8];
            snprintf(idx_text, sizeof(idx_text), playing ? ">" : "%02lu", (unsigned long)i + 1);
            lv_label_set_text(idx_lbl, idx_text);
            lv_obj_set_style_text_color(idx_lbl, playing ? s_accent_color : s_dim_color, 0);
        }

        lv_obj_t *lbl = lv_obj_get_child(btn, 1);
        if (lbl) {
            lv_obj_set_style_text_color(lbl, playing ? s_accent_color : (selected ? s_text_color : s_dim_color), 0);
        }

        lv_obj_t *badge = lv_obj_get_child(btn, 2);
        if (badge) {
            lv_label_set_text(badge, playing ? (state == AUDIO_STREAM_STATE_PAUSED ? "PAUSE" : "PLAY") : (selected ? "SEL" : ""));
            lv_obj_set_style_text_color(badge, playing ? s_accent_color : s_dim_color, 0);
        }
    }

    if (s_selected_index >= 0 && s_selected_index < (int)child_cnt) {
        lv_obj_t *btn = lv_obj_get_child(s_file_list, s_selected_index);
        if (btn) lv_obj_scroll_to_view(btn, LV_ANIM_OFF);
    }
    update_list_hint();
}

/* ---------------- Status / Now Playing refresh ---------------- */

static void audio_player_update_status(void)
{
    audio_stream_state_t state = audio_stream_manager_get_state();
    int current = audio_stream_manager_get_current_index();
    int total = audio_stream_manager_get_file_count();

    /* The precheck runs on a worker task, so a refusal (e.g. bitrate too
     * high for the GhostLink link) surfaces asynchronously here. */
    esp_err_t play_err = audio_stream_manager_consume_last_error();
    if (play_err == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    }

    if (current != s_last_rendered_playing_index || state != s_last_rendered_state) {
        s_last_rendered_playing_index = current;
        s_last_rendered_state = state;
        update_file_list_selection();
    }

    bool is_playing = (state == AUDIO_STREAM_STATE_PLAYING);
    if (s_play_btn) {
        if (is_playing) lv_obj_add_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_pause_btn) {
        if (is_playing) lv_obj_clear_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }

    const char *state_str = "Stopped";
    switch (state) {
        case AUDIO_STREAM_STATE_PLAYING: state_str = "Playing"; break;
        case AUDIO_STREAM_STATE_PAUSED:  state_str = "Paused";  break;
        case AUDIO_STREAM_STATE_STOPPED: state_str = "Stopped"; break;
        default: break;
    }

    /* Title: prefer the actually-playing track, else the current selection. */
    int show_index = (total > 0 && current >= 0 && current < total) ? current : s_selected_index;
    if (s_np_title) {
        const char *fname = (show_index >= 0 && show_index < total)
                                ? audio_stream_manager_get_filename(show_index) : NULL;
        lv_label_set_text(s_np_title, fname ? fname : (total > 0 ? "Select a track" : "No MP3 files"));
    }

    /* Progress + time */
    size_t pos = audio_stream_manager_get_position();
    size_t total_size = audio_stream_manager_get_total_size();
    uint32_t duration_ms = audio_stream_manager_get_duration_ms();
    uint32_t played_ms = get_estimated_playback_ms(pos, total_size, duration_ms);
    if (duration_ms > 0 && played_ms > duration_ms) played_ms = duration_ms;
    if (s_progress_fill) {
        lv_coord_t track_w = s_progress_track ? lv_obj_get_width(s_progress_track) : (LV_HOR_RES - 24);
        lv_coord_t track_h = s_progress_track ? lv_obj_get_height(s_progress_track) : 8;
        int32_t pct = 0;
        if (duration_ms > 0) pct = (int32_t)(((uint64_t)played_ms * 100) / duration_ms);
        else if (total_size > 0) pct = (int32_t)((pos * 100) / total_size);
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        lv_obj_set_size(s_progress_fill, (track_w * pct) / 100, track_h);
    }

    if (s_time_label) {
        char tbuf[32];
        char tbuf2[32];
        format_time(tbuf, sizeof(tbuf), played_ms / 1000);
        if (duration_ms > 0) {
            format_time(tbuf2, sizeof(tbuf2), duration_ms / 1000);
        } else {
            /* Unknown length: show it instead of a misleading 0:00. */
            snprintf(tbuf2, sizeof(tbuf2), "--:--");
        }
        char tline[80];
        snprintf(tline, sizeof(tline), "%s / %s", tbuf, tbuf2);
        lv_label_set_text(s_time_label, tline);
    }

    if (s_meta_label) {
        uint16_t bitrate = audio_stream_manager_get_bitrate();
        char mbuf[64];
        if (total > 0) {
            if (bitrate > 0) snprintf(mbuf, sizeof(mbuf), "%s  -  %d/%d  -  %uk", state_str, show_index + 1, total, (unsigned)bitrate);
            else snprintf(mbuf, sizeof(mbuf), "%s  -  %d/%d", state_str, show_index + 1, total);
        } else {
            snprintf(mbuf, sizeof(mbuf), "%s", state_str);
        }
        lv_label_set_text(s_meta_label, mbuf);
    }
}

/* ---------------- Playback helpers ---------------- */

static bool play_track_with_toast(int index)
{
    esp_err_t ret = audio_stream_manager_play(index);
    if (ret == ESP_OK) {
        return true;
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    } else {
        toast_show_duration("Could not play MP3", TOAST_WARN, 1600);
    }
    ESP_LOGW(TAG, "Audio play failed for index %d: %s", index, esp_err_to_name(ret));
    return false;
}

static bool change_track_with_toast(bool next)
{
    esp_err_t ret = next ? audio_stream_manager_next() : audio_stream_manager_prev();
    if (ret == ESP_OK) {
        return true;
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        toast_show_duration("MP3 too high for GhostLink (max 264kbps)", TOAST_WARN, 2400);
    } else {
        toast_show_duration("Could not play MP3", TOAST_WARN, 1600);
    }
    ESP_LOGW(TAG, "Audio %s failed: %s", next ? "next" : "previous", esp_err_to_name(ret));
    return false;
}

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    audio_player_update_status();
}

static void audio_player_go_back(void)
{
    s_return_view = NULL;
    display_manager_go_back();
}

/* ---------------- Screen switching ---------------- */

static void show_library(void)
{
    s_screen = SCREEN_LIBRARY;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    /* The P4 keeps both panes alive. This call only changes keyboard/joystick
     * focus; the library remains available while a track is playing. */
    update_file_list_selection();
    return;
#else
    if (s_np_cont) lv_obj_add_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_library_cont) lv_obj_clear_flag(s_library_cont, LV_OBJ_FLAG_HIDDEN);
    update_file_list_selection();
#endif
}

static void show_nowplaying(void)
{
    s_screen = SCREEN_NOWPLAYING;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    /* Split view: the player pane is always present, so selecting a track
     * never causes the library to disappear. */
    if (s_library_cont) lv_obj_clear_flag(s_library_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_np_cont) lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
#else
    if (s_library_cont) lv_obj_add_flag(s_library_cont, LV_OBJ_FLAG_HIDDEN);
    if (s_np_cont) lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
#endif
    update_volume_ui();
    audio_player_update_status();
}

/* ---------------- Construction ---------------- */

static lv_obj_t *make_round_btn(lv_obj_t *parent, int w, int h, const char *sym, lv_color_t txt)
{
    lv_obj_t *btn = lv_btn_create(parent);
    gui_apply_pressed_style(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, s_surface_alt_color, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = create_label(btn, sym, accessibility_get_font_body(), txt);
    lv_obj_center(lbl);
    return btn;
}

/* Bottom touch bar with a centered Back button, styled like the bars on
 * other views. Back-only variant: arrows are permanently hidden.
 * Button dispatch stays manual (point_in_obj in the tap handlers), so no
 * LVGL CLICKED callbacks are attached — same as before.
 * Returns the back button (or NULL when the bar is not shown, in which case
 * hardware back input still works as usual). */
static lv_obj_t *create_touch_bar(lv_obj_t *parent, gui_touch_bar_t *tb)
{
    if (!tb) return NULL;
    *tb = gui_touch_bar_create(parent);
    if (!tb->bar) return NULL;
    gui_touch_bar_hide_arrows(tb);
    return tb->back_btn;
}

static void build_library(int status_bar_h)
{
    int screen_h = LV_VER_RES - status_bar_h;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    const lv_coord_t panel_x = 24;
    const lv_coord_t panel_w = (LV_HOR_RES * 43) / 100;
#endif
    int header_h =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                    64;
#else
                    30;
#endif
    int list_h = screen_h - header_h - AUDIO_BOTTOM_SAFE_H - 6;

    s_library_cont = lv_obj_create(s_root);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_size(s_library_cont, panel_w, screen_h);
    lv_obj_set_pos(s_library_cont, panel_x, status_bar_h);
    lv_obj_set_style_bg_color(s_library_cont, s_surface_color, 0);
    lv_obj_set_style_bg_opa(s_library_cont, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_library_cont, GUI_RADIUS_MD, 0);
#else
    lv_obj_set_size(s_library_cont, LV_PCT(100), screen_h);
    lv_obj_align(s_library_cont, LV_ALIGN_TOP_MID, 0, status_bar_h);
#endif
    lv_obj_set_style_bg_opa(s_library_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_library_cont, 0, 0);
    lv_obj_set_style_pad_all(s_library_cont, 0, 0);
    lv_obj_clear_flag(s_library_cont, LV_OBJ_FLAG_SCROLLABLE);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_style_bg_color(s_library_cont, s_surface_color, 0);
    lv_obj_set_style_bg_opa(s_library_cont, LV_OPA_70, 0);
#endif

    /* Header: count + title */

    s_list_hint_label = lv_label_create(s_library_cont);
    lv_obj_set_style_text_color(s_list_hint_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_list_hint_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_list_hint_label, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_label_set_text(s_list_hint_label, "0 tracks");

    lv_obj_t *title = lv_label_create(s_library_cont);
    lv_label_set_text(title, "Library");
    lv_obj_set_style_text_color(title, s_text_color, 0);
    lv_obj_set_style_text_font(title,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                               accessibility_get_font_title(),
#else
                               accessibility_get_font_small(),
#endif
                               0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                 78,
#else
                 0,
#endif
                 8);

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_t *library_hint = create_label(s_library_cont, "Tap a track to play", accessibility_get_font_small(), s_dim_color);
    lv_obj_align(library_hint, LV_ALIGN_TOP_LEFT, 78, 36);

    /* Keep a visible, thumb-friendly escape hatch in addition to the P4 edge
     * back gesture. This is especially useful when the library has focus and
     * makes the split view feel like a real two-pane app. */
    s_lib_back_btn = make_round_btn(s_library_cont, 48, 36, LV_SYMBOL_LEFT, s_dim_color);
    lv_obj_set_pos(s_lib_back_btn, 16, 8);
#endif

    /* File list */
    s_file_list = lv_obj_create(s_library_cont);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_size(s_file_list, panel_w - 16, list_h);
#else
    lv_obj_set_size(s_file_list, LV_HOR_RES - 8, list_h);
#endif
    lv_obj_align(s_file_list, LV_ALIGN_TOP_MID, 0, header_h);
    lv_obj_set_style_bg_opa(s_file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_file_list, 0, 0);
    lv_obj_set_style_pad_all(s_file_list,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                             8,
#else
                             4,
#endif
                             0);
    lv_obj_set_flex_flow(s_file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_file_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_file_list, LV_DIR_VER);
    lv_obj_set_style_pad_row(s_file_list, 2, 0);
    lv_obj_clear_flag(s_file_list, LV_OBJ_FLAG_CLICKABLE);

    create_file_list();
    update_list_hint();

    /* Bottom touch bar: Back exits the whole view on legacy displays. Large
     * touch panels use the header escape button plus the system edge gesture.
     * Gating is handled inside gui_touch_bar_create(): when it shows no bar,
     * keep any button created above (P4 header escape). */
    {
        lv_obj_t *bar_back = create_touch_bar(s_library_cont, &s_lib_tb);
        if (bar_back) s_lib_back_btn = bar_back;
    }
}

#ifndef CONFIG_CROWPANEL_ADVANCED_P4
static void build_nowplaying_standard(int status_bar_h)
{
    int screen_h = LV_VER_RES - status_bar_h;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    const lv_coord_t player_x = 24 + ((LV_HOR_RES * 43) / 100) + 16;
    const lv_coord_t player_w = LV_HOR_RES - player_x - 24;
#endif

    s_np_cont = lv_obj_create(s_root);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_size(s_np_cont, player_w, screen_h);
    lv_obj_set_pos(s_np_cont, player_x, status_bar_h);
    lv_obj_set_style_bg_color(s_np_cont, s_surface_color, 0);
    lv_obj_set_style_bg_opa(s_np_cont, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_np_cont, GUI_RADIUS_MD, 0);
#else
    lv_obj_set_size(s_np_cont, LV_PCT(100), screen_h);
    lv_obj_align(s_np_cont, LV_ALIGN_TOP_MID, 0, status_bar_h);
#endif
    lv_obj_set_style_bg_opa(s_np_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_np_cont, 0, 0);
    lv_obj_set_style_pad_all(s_np_cont, 0, 0);
    lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_SCROLLABLE);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_set_style_bg_color(s_np_cont, s_surface_color, 0);
    lv_obj_set_style_bg_opa(s_np_cont, LV_OPA_70, 0);
#endif
#ifndef CONFIG_CROWPANEL_ADVANCED_P4
    lv_obj_add_flag(s_np_cont, LV_OBJ_FLAG_HIDDEN);
#endif

    /* Track title */
    s_np_title = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_np_title, s_text_color, 0);
    lv_obj_set_style_text_font(s_np_title, accessibility_get_font_title(), 0);
    lv_obj_set_width(s_np_title,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                     player_w - 48
#else
                     LV_HOR_RES - 100
#endif
    );
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_np_title, LV_ALIGN_TOP_MID, 0, 4);
    lv_label_set_text(s_np_title, "Ready");

    /* Meta line */
    s_meta_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_meta_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_meta_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_meta_label,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                     player_w - 32
#else
                     LV_HOR_RES - 24
#endif
    );
    lv_obj_set_style_text_align(s_meta_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_align(s_meta_label, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_text(s_meta_label, "Stopped");

    /* Progress bar */
    s_progress_track = lv_obj_create(s_np_cont);
    lv_obj_set_size(s_progress_track,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                    player_w - 32,
#else
                    LV_HOR_RES - 24,
#endif
                    8);
    lv_obj_align(s_progress_track, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(s_progress_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_progress_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_track, 4, 0);
    lv_obj_set_style_border_width(s_progress_track, 0, 0);
    lv_obj_set_style_pad_all(s_progress_track, 0, 0);
    lv_obj_clear_flag(s_progress_track, LV_OBJ_FLAG_SCROLLABLE);

    s_progress_fill = lv_obj_create(s_progress_track);
    lv_obj_set_size(s_progress_fill, 0, 8);
    lv_obj_align(s_progress_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_fill, 4, 0);
    lv_obj_set_style_border_width(s_progress_fill, 0, 0);
    lv_obj_set_style_pad_all(s_progress_fill, 0, 0);
    lv_obj_clear_flag(s_progress_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_time_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_time_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_time_label,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                     player_w - 32
#else
                     LV_HOR_RES - 24
#endif
    );
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, 66);
    lv_label_set_text(s_time_label, "0:00 / 0:00");

    /* Transport row */
    int transport_y =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                      (screen_h >= 400) ? 112 : 96;
#else
                      88;
#endif
    int btn_h =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                  (screen_h >= 400) ? 56 : 46;
#else
                  (screen_h > 200) ? 42 : 34;
#endif
    int transport_gap =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                       (player_w >= 420) ? 118 : 92;
#else
                       84;
#endif
    int side_btn_w =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                   (screen_h >= 400) ? 72 : 60;
#else
                   56;
#endif
    int play_btn_w =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                   (screen_h >= 400) ? 82 : 68;
#else
                   64;
#endif
    s_prev_btn = make_round_btn(s_np_cont, side_btn_w, btn_h, LV_SYMBOL_PREV, s_accent_color);
    lv_obj_align(s_prev_btn, LV_ALIGN_TOP_MID, -transport_gap, transport_y);

    s_play_btn = make_round_btn(s_np_cont, play_btn_w, btn_h, LV_SYMBOL_PLAY, s_accent_color);
    lv_obj_align(s_play_btn, LV_ALIGN_TOP_MID, 0, transport_y);

    s_pause_btn = make_round_btn(s_np_cont, play_btn_w, btn_h, LV_SYMBOL_PAUSE, s_accent_color);
    lv_obj_align(s_pause_btn, LV_ALIGN_TOP_MID, 0, transport_y);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);

    s_next_btn = make_round_btn(s_np_cont, side_btn_w, btn_h, LV_SYMBOL_NEXT, s_accent_color);
    lv_obj_align(s_next_btn, LV_ALIGN_TOP_MID, transport_gap, transport_y);

    /* Volume row: [-]  [====slider====]  [+] with label above.
     * Anchored above the bottom touch bar. */
    s_volume_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_volume_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_volume_label, accessibility_get_font_small(), 0);
    lv_obj_align(s_volume_label, LV_ALIGN_BOTTOM_MID, 0, -(AUDIO_BOTTOM_SAFE_H + 40));
    lv_label_set_text(s_volume_label, "Vol 85%");

    int volume_btn_w =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                      (player_w >= 420) ? 56 : 44;
#else
                      40;
#endif
    int volume_btn_h =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                      (LV_VER_RES >= 400) ? 42 : 34;
#else
                      30;
#endif
    s_vol_minus_btn = make_round_btn(s_np_cont, volume_btn_w, volume_btn_h, LV_SYMBOL_MINUS, s_text_color);
    lv_obj_align(s_vol_minus_btn, LV_ALIGN_BOTTOM_LEFT, 10, -(AUDIO_BOTTOM_SAFE_H + 6));

    s_vol_plus_btn = make_round_btn(s_np_cont, volume_btn_w, volume_btn_h, LV_SYMBOL_PLUS, s_text_color);
    lv_obj_align(s_vol_plus_btn, LV_ALIGN_BOTTOM_RIGHT, -10, -(AUDIO_BOTTOM_SAFE_H + 6));

    s_vol_track = lv_obj_create(s_np_cont);
    lv_coord_t volume_track_w =
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                                player_w - 2 * (volume_btn_w + 28);
#else
                                LV_HOR_RES - 2 * (volume_btn_w + 28);
#endif
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    if (volume_track_w > 520) volume_track_w = 520;
#endif
    if (volume_track_w < 80) volume_track_w = 80;
    lv_obj_set_size(s_vol_track, volume_track_w,
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
                    16
#else
                    12
#endif
    );
    lv_obj_align(s_vol_track, LV_ALIGN_BOTTOM_MID, 0, -(AUDIO_BOTTOM_SAFE_H + 15));
    lv_obj_set_style_bg_color(s_vol_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_vol_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_track, 6, 0);
    lv_obj_set_style_border_width(s_vol_track, 0, 0);
    lv_obj_set_style_pad_all(s_vol_track, 0, 0);
    lv_obj_clear_flag(s_vol_track, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_fill = lv_obj_create(s_vol_track);
    lv_obj_set_size(s_vol_fill, 0, lv_obj_get_height(s_vol_track));
    lv_obj_align(s_vol_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_vol_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_vol_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_fill, 6, 0);
    lv_obj_set_style_border_width(s_vol_fill, 0, 0);
    lv_obj_set_style_pad_all(s_vol_fill, 0, 0);
    lv_obj_clear_flag(s_vol_fill, LV_OBJ_FLAG_SCROLLABLE);

    /* Bottom touch bar: Back returns to the library on small devices. The P4
     * uses the system edge gesture/home indicator instead. */
    s_np_back_btn = create_touch_bar(s_np_cont, &s_np_tb);
}

static void build_nowplaying(int status_bar_h)
{
    build_nowplaying_standard(status_bar_h);
}

#else /* CONFIG_CROWPANEL_ADVANCED_P4 */

LV_IMG_DECLARE(speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);

/* The 1024x600 P4 has enough room for a real player composition. Keep the
 * visual hierarchy horizontal so the controls never collapse into a narrow
 * vertical stack as the library remains visible beside it. */
static void build_nowplaying_p4(int status_bar_h)
{
    const lv_coord_t screen_h = LV_VER_RES - status_bar_h;
    const lv_coord_t panel_x = 24 + ((LV_HOR_RES * 43) / 100) + 16;
    const lv_coord_t panel_w = LV_HOR_RES - panel_x - 24;
    const lv_coord_t pad = 24;
    const lv_coord_t content_w = panel_w - (pad * 2);
    const lv_coord_t art_size = content_w >= 430 ? 208 : (content_w >= 360 ? 176 : 148);
    const lv_coord_t gap = 20;
    const lv_coord_t info_x = pad + art_size + gap;
    const lv_coord_t info_w = content_w - art_size - gap;

    s_np_cont = lv_obj_create(s_root);
    lv_obj_set_size(s_np_cont, panel_w, screen_h);
    lv_obj_set_pos(s_np_cont, panel_x, status_bar_h);
    lv_obj_set_style_bg_color(s_np_cont, s_surface_color, 0);
    lv_obj_set_style_bg_opa(s_np_cont, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_np_cont, GUI_RADIUS_MD, 0);
    lv_obj_set_style_border_color(s_np_cont, s_surface_alt_color, 0);
    lv_obj_set_style_border_width(s_np_cont, 1, 0);
    lv_obj_set_style_pad_all(s_np_cont, 0, 0);
    lv_obj_clear_flag(s_np_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = create_label(s_np_cont, "NOW PLAYING", accessibility_get_font_small(), s_accent_color);
    lv_obj_set_pos(eyebrow, pad, 18);

    char queue_text[24];
    snprintf(queue_text, sizeof(queue_text), "%d tracks", audio_stream_manager_get_file_count());
    lv_obj_t *queue = create_label(s_np_cont, queue_text, accessibility_get_font_small(), s_dim_color);
    lv_obj_align(queue, LV_ALIGN_TOP_RIGHT, -pad, 18);

    /* A proper artwork surface gives the selected track a visual anchor even
     * when the MP3 has no embedded album art available to the firmware. */
    lv_obj_t *art = lv_obj_create(s_np_cont);
    lv_obj_set_size(art, art_size, art_size);
    lv_obj_set_pos(art, pad, 58);
    lv_obj_set_style_bg_color(art, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(art, s_accent_color, 0);
    lv_obj_set_style_border_width(art, 2, 0);
    lv_obj_set_style_radius(art, GUI_RADIUS_LG, 0);
    lv_obj_set_style_shadow_width(art, 18, 0);
    lv_obj_set_style_shadow_opa(art, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(art, s_accent_color, 0);
    lv_obj_clear_flag(art, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *art_glow = lv_obj_create(art);
    lv_obj_set_size(art_glow, art_size - 28, art_size - 28);
    lv_obj_center(art_glow);
    lv_obj_set_style_bg_color(art_glow, s_accent_color, 0);
    lv_obj_set_style_bg_opa(art_glow, LV_OPA_20, 0);
    lv_obj_set_style_radius(art_glow, GUI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(art_glow, 0, 0);
    lv_obj_clear_flag(art_glow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *art_icon = lv_img_create(art);
    lv_img_set_src(art_icon, &speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
    lv_obj_set_style_img_recolor(art_icon, s_accent_color, 0);
    lv_obj_set_style_img_recolor_opa(art_icon, LV_OPA_COVER, 0);
    lv_img_set_zoom(art_icon, art_size >= 190 ? 640 : 512);
    lv_obj_center(art_icon);

    lv_obj_t *art_caption = create_label(art, "AUDIO", accessibility_get_font_small(), s_dim_color);
    lv_obj_align(art_caption, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* Track identity and status live beside the artwork instead of competing
     * with the transport controls for the same vertical pixels. */
    s_np_title = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_np_title, s_text_color, 0);
    lv_obj_set_style_text_font(s_np_title, accessibility_get_font_title(), 0);
    lv_obj_set_width(s_np_title, info_w);
    lv_obj_set_style_text_align(s_np_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_np_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_np_title, info_x, 62);
    lv_label_set_text(s_np_title, "Ready");

    s_meta_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_meta_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_meta_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_meta_label, info_w);
    lv_obj_set_style_text_align(s_meta_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_meta_label, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_meta_label, info_x, 98);
    lv_label_set_text(s_meta_label, "Stopped");

    lv_obj_t *state_line = lv_obj_create(s_np_cont);
    lv_obj_set_size(state_line, info_w, 2);
    lv_obj_set_pos(state_line, info_x, 128);
    lv_obj_set_style_bg_color(state_line, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(state_line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(state_line, 0, 0);

    s_progress_track = lv_obj_create(s_np_cont);
    lv_obj_set_size(s_progress_track, info_w, 10);
    lv_obj_set_pos(s_progress_track, info_x, 158);
    lv_obj_set_style_bg_color(s_progress_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_progress_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_track, 5, 0);
    lv_obj_set_style_border_width(s_progress_track, 0, 0);
    lv_obj_set_style_pad_all(s_progress_track, 0, 0);
    lv_obj_clear_flag(s_progress_track, LV_OBJ_FLAG_SCROLLABLE);

    s_progress_fill = lv_obj_create(s_progress_track);
    lv_obj_set_size(s_progress_fill, 0, 10);
    lv_obj_align(s_progress_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_progress_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_progress_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_progress_fill, 5, 0);
    lv_obj_set_style_border_width(s_progress_fill, 0, 0);
    lv_obj_set_style_pad_all(s_progress_fill, 0, 0);
    lv_obj_clear_flag(s_progress_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_time_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_time_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_time_label, accessibility_get_font_small(), 0);
    lv_obj_set_width(s_time_label, info_w);
    lv_obj_set_style_text_align(s_time_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_pos(s_time_label, info_x, 178);
    lv_label_set_text(s_time_label, "0:00 / 0:00");

    const lv_coord_t transport_y = 222;
    const lv_coord_t transport_h = 58;
    const lv_coord_t side_w = info_w >= 220 ? 56 : 48;
    const lv_coord_t play_w = info_w >= 220 ? 70 : 60;
    const lv_coord_t transport_gap = info_w >= 220 ? 14 : 8;
    const lv_coord_t transport_w = (side_w * 2) + play_w + (transport_gap * 2);
    const lv_coord_t transport_x = info_x + ((info_w - transport_w) / 2);

    s_prev_btn = make_round_btn(s_np_cont, side_w, transport_h, LV_SYMBOL_PREV, s_accent_color);
    lv_obj_set_pos(s_prev_btn, transport_x, transport_y);

    s_play_btn = make_round_btn(s_np_cont, play_w, transport_h, LV_SYMBOL_PLAY, s_accent_color);
    lv_obj_set_pos(s_play_btn, transport_x + side_w + transport_gap, transport_y);

    s_pause_btn = make_round_btn(s_np_cont, play_w, transport_h, LV_SYMBOL_PAUSE, s_accent_color);
    lv_obj_set_pos(s_pause_btn, transport_x + side_w + transport_gap, transport_y);
    lv_obj_add_flag(s_pause_btn, LV_OBJ_FLAG_HIDDEN);

    s_next_btn = make_round_btn(s_np_cont, side_w, transport_h, LV_SYMBOL_NEXT, s_accent_color);
    lv_obj_set_pos(s_next_btn, transport_x + side_w + transport_gap + play_w + transport_gap, transport_y);

    /* Volume is a full-width, thumb-friendly control in the footer. The
     * bottom safe area is reserved for the P4 home indicator/edge gesture. */
    const lv_coord_t footer_y = screen_h - AUDIO_BOTTOM_SAFE_H - 72;
    s_volume_label = lv_label_create(s_np_cont);
    lv_obj_set_style_text_color(s_volume_label, s_dim_color, 0);
    lv_obj_set_style_text_font(s_volume_label, accessibility_get_font_small(), 0);
    lv_obj_set_pos(s_volume_label, pad, footer_y);
    lv_label_set_text(s_volume_label, "Vol 85%");

    const lv_coord_t volume_btn_w = 50;
    const lv_coord_t volume_btn_h = 44;
    s_vol_minus_btn = make_round_btn(s_np_cont, volume_btn_w, volume_btn_h, LV_SYMBOL_MINUS, s_text_color);
    lv_obj_set_pos(s_vol_minus_btn, pad, footer_y + 24);

    s_vol_plus_btn = make_round_btn(s_np_cont, volume_btn_w, volume_btn_h, LV_SYMBOL_PLUS, s_text_color);
    lv_obj_set_pos(s_vol_plus_btn, panel_w - pad - volume_btn_w, footer_y + 24);

    const lv_coord_t volume_track_w = content_w - (volume_btn_w * 2) - 24;
    s_vol_track = lv_obj_create(s_np_cont);
    lv_obj_set_size(s_vol_track, volume_track_w, 16);
    lv_obj_set_pos(s_vol_track, pad + volume_btn_w + 12, footer_y + 38);
    lv_obj_set_style_bg_color(s_vol_track, s_surface_alt_color, 0);
    lv_obj_set_style_bg_opa(s_vol_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_track, 8, 0);
    lv_obj_set_style_border_width(s_vol_track, 0, 0);
    lv_obj_set_style_pad_all(s_vol_track, 0, 0);
    lv_obj_clear_flag(s_vol_track, LV_OBJ_FLAG_SCROLLABLE);

    s_vol_fill = lv_obj_create(s_vol_track);
    lv_obj_set_size(s_vol_fill, 0, 16);
    lv_obj_align(s_vol_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_vol_fill, s_accent_color, 0);
    lv_obj_set_style_bg_opa(s_vol_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_vol_fill, 8, 0);
    lv_obj_set_style_border_width(s_vol_fill, 0, 0);
    lv_obj_set_style_pad_all(s_vol_fill, 0, 0);
    lv_obj_clear_flag(s_vol_fill, LV_OBJ_FLAG_SCROLLABLE);

    s_np_back_btn = NULL;
}

static void build_nowplaying(int status_bar_h)
{
    build_nowplaying_p4(status_bar_h);
}

#endif /* CONFIG_CROWPANEL_ADVANCED_P4 */

void audio_player_create(void)
{
    refresh_theme_colors();
    s_screen = SCREEN_LIBRARY;

#ifdef CONFIG_HAS_TLV320DAC_I2C
    ESP_LOGI(TAG, "Resetting TLV320DAC3100 DAC");
    if (tlv320dac3100_is_initialized()) {
        tlv320dac3100_deinit();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_err_t reset_ret = io_manager_dac_reset_pulse();
    if (reset_ret != ESP_OK) {
        ESP_LOGW(TAG, "DAC reset pulse failed: %s", esp_err_to_name(reset_ret));
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    tlv320dac3100_config_t dac_cfg = TLV320DAC3100_DEFAULT_CONFIG();
    esp_err_t dac_ret = tlv320dac3100_init(&dac_cfg);
    if (dac_ret != ESP_OK) {
        ESP_LOGW(TAG, "TLV320DAC init failed: %s", esp_err_to_name(dac_ret));
    } else {
        s_volume_percent = 85;
    }
#endif

    esp_err_t ret = audio_stream_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Audio stream manager init failed: %s", esp_err_to_name(ret));
    }

    s_root = gui_screen_create_root(NULL, "Audio Player", s_bg_color, LV_OPA_COVER);
    audio_player_view.root = s_root;
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    int status_bar_h = GUI_STATUS_BAR_H;
    display_manager_add_status_bar("Audio Player");

    build_library(status_bar_h);
    build_nowplaying(status_bar_h);

    /* Apply the starting volume to the active backend + slider. */
    apply_volume(s_volume_percent);

    s_update_timer = lv_timer_create(update_timer_cb, 200, NULL);

    check_sd_and_show_toast();
    audio_player_update_status();

    ESP_LOGI(TAG, "Audio player view created (%d files)", s_visible_count);
}

void audio_player_destroy(void)
{
    if (s_update_timer) {
        lvgl_timer_del_safe(&s_update_timer);
    }

    audio_stream_manager_stop();
    audio_stream_manager_deinit();

    gui_touch_bar_destroy(&s_lib_tb);
    gui_touch_bar_destroy(&s_np_tb);
    lvgl_obj_del_safe(&s_root);
    audio_player_view.root = NULL;

    s_library_cont = NULL;
    s_np_cont = NULL;
    s_file_list = NULL;
    s_list_hint_label = NULL;
    s_lib_back_btn = NULL;
    s_np_title = NULL;
    s_meta_label = NULL;
    s_progress_track = NULL;
    s_progress_fill = NULL;
    s_time_label = NULL;
    s_play_btn = NULL;
    s_pause_btn = NULL;
    s_prev_btn = NULL;
    s_next_btn = NULL;
    s_np_back_btn = NULL;
    s_vol_track = NULL;
    s_vol_fill = NULL;
    s_vol_minus_btn = NULL;
    s_vol_plus_btn = NULL;
    s_volume_label = NULL;
    s_selected_index = 0;
    s_visible_count = 0;
    s_screen = SCREEN_LIBRARY;
    s_vol_dragging = false;
    touch_drag_reset(&s_touch_drag);
}

/* ---------------- Input ---------------- */

static void toggle_play_pause(void)
{
    audio_stream_state_t st = audio_stream_manager_get_state();
    if (st == AUDIO_STREAM_STATE_PLAYING) {
        audio_stream_manager_pause();
    } else if (st == AUDIO_STREAM_STATE_PAUSED) {
        audio_stream_manager_resume();
    } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
        play_track_with_toast(s_selected_index);
    }
    audio_player_update_status();
}

static void handle_library_tap(lv_indev_data_t *d, int dx, int dy)
{
    if (point_in_obj(s_lib_back_btn, d->point.x, d->point.y) && abs(dx) < 15 && abs(dy) < 15) {
        audio_player_go_back();
        return;
    }
    if (abs(dx) < 10 && abs(dy) < 10 && s_file_list) {
        uint32_t child_cnt = lv_obj_get_child_cnt(s_file_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(s_file_list, i);
            if (!child) continue;
            lv_area_t a;
            lv_obj_get_coords(child, &a);
            if (d->point.x >= a.x1 && d->point.x <= a.x2 &&
                d->point.y >= a.y1 && d->point.y <= a.y2) {
                s_selected_index = (int)i;
                update_file_list_selection();
                if (play_track_with_toast((int)i)) {
                    show_nowplaying();
                }
                audio_player_update_status();
                break;
            }
        }
    }
}

static void handle_nowplaying_tap(lv_indev_data_t *d, int dx, int dy)
{
    if (abs(dx) >= 15 || abs(dy) >= 15) return;
    int px = (int)d->point.x, py = (int)d->point.y;

    if (point_in_obj(s_np_back_btn, px, py)) { show_library(); return; }
    if (point_in_obj(s_vol_minus_btn, px, py)) { adjust_volume(-5); return; }
    if (point_in_obj(s_vol_plus_btn, px, py)) { adjust_volume(+5); return; }
    if (point_in_obj(s_prev_btn, px, py)) {
        change_track_with_toast(false);
        s_selected_index = audio_stream_manager_get_current_index();
        audio_player_update_status();
        return;
    }
    if (point_in_obj(s_next_btn, px, py)) {
        change_track_with_toast(true);
        s_selected_index = audio_stream_manager_get_current_index();
        audio_player_update_status();
        return;
    }
    if (point_in_obj(s_play_btn, px, py) || point_in_obj(s_pause_btn, px, py)) {
        toggle_play_pause();
        return;
    }
    if (point_in_obj(s_vol_track, px, py)) {
        set_volume_from_x(px);
        return;
    }
}

static void audio_player_input_handler(InputEvent *event)
{
    if (!event) return;

    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;

        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_drag.started) {
                touch_drag_begin(&s_touch_drag, d);
                /* Start a volume drag if the press landed on the slider. */
                if (s_screen == SCREEN_NOWPLAYING && point_in_obj(s_vol_track, d->point.x, d->point.y)) {
                    s_vol_dragging = true;
                    set_volume_from_x((int)d->point.x);
                }
                return;
            }
            if (s_vol_dragging) {
                set_volume_from_x((int)d->point.x);
                return;
            }
            /* Live list drag. On P4 this is selected by the original touch
             * region because both library and player remain visible. */
            if (s_file_list && lv_obj_is_valid(s_file_list)) {
                lv_area_t list_area;
                lv_obj_get_coords(s_file_list, &list_area);
                bool started_in_list = (s_touch_drag.start_x >= list_area.x1 && s_touch_drag.start_x <= list_area.x2 &&
                                        s_touch_drag.start_y >= list_area.y1 && s_touch_drag.start_y <= list_area.y2);
                if (started_in_list) {
                    touch_drag_update(&s_touch_drag, d, s_file_list);
                }
            }
            return;
        }

        if (d->state == LV_INDEV_STATE_REL) {
            if (s_vol_dragging) {
                set_volume_from_x((int)d->point.x);
                s_vol_dragging = false;
                touch_drag_reset(&s_touch_drag);
                return;
            }
            if (!s_touch_drag.started) return;
            int dx = (int)d->point.x - s_touch_drag.start_x;
            int dy = (int)d->point.y - s_touch_drag.start_y;
            int start_x = s_touch_drag.start_x;
            int start_y = s_touch_drag.start_y;

            bool was_dragged = touch_drag_release(&s_touch_drag, d);
            if (was_dragged) return;

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
            /* Split-view input is routed by pane, not by the last selected
             * sub-screen. This keeps the library usable while playing. */
            bool in_library = point_in_obj(s_file_list, d->point.x, d->point.y) ||
                              point_in_obj(s_lib_back_btn, d->point.x, d->point.y);
            bool started_in_library = point_in_obj(s_file_list, start_x, start_y) ||
                                      point_in_obj(s_lib_back_btn, start_x, start_y);
            if (in_library || started_in_library) {
                handle_library_tap(d, dx, dy);
            } else {
                handle_nowplaying_tap(d, dx, dy);
            }
#else
            if (s_screen == SCREEN_NOWPLAYING) {
                handle_nowplaying_tap(d, dx, dy);
            } else {
                handle_library_tap(d, dx, dy);
            }
#endif
        }
        return;
    }

    if (event->type == INPUT_TYPE_JOYSTICK) {
        int btn = event->data.joystick_index;
        if (btn == 0) { /* Left -> back (Now Playing -> Library -> exit) */
            if (s_screen == SCREEN_NOWPLAYING) show_library();
            else audio_player_go_back();
        } else if (btn == 2) { /* Up */
            if (s_screen == SCREEN_NOWPLAYING) {
                adjust_volume(+5);
            } else if (s_selected_index > 0) {
                s_selected_index--;
                update_file_list_selection();
            }
        } else if (btn == 4) { /* Down */
            if (s_screen == SCREEN_NOWPLAYING) {
                adjust_volume(-5);
            } else if (s_selected_index < s_visible_count - 1) {
                s_selected_index++;
                update_file_list_selection();
            }
        } else if (btn == 1) { /* Select */
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        } else if (btn == 3) { /* Right -> next track */
            change_track_with_toast(true);
            s_selected_index = audio_stream_manager_get_current_index();
            update_file_list_selection();
            audio_player_update_status();
        }
        return;
    }

    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        } else {
            adjust_volume(event->data.encoder.direction > 0 ? 5 : -5);
        }
        return;
    }

    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        if (s_screen == SCREEN_NOWPLAYING) show_library();
        else audio_player_go_back();
        return;
    }

    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == '`') {
            if (s_screen == SCREEN_NOWPLAYING) show_library();
            else audio_player_go_back();
        } else if (key == LV_KEY_UP || key == 'k') {
            if (s_screen == SCREEN_NOWPLAYING) adjust_volume(+5);
            else if (s_selected_index > 0) { s_selected_index--; update_file_list_selection(); }
        } else if (key == LV_KEY_DOWN || key == 'j') {
            if (s_screen == SCREEN_NOWPLAYING) adjust_volume(-5);
            else if (s_selected_index < s_visible_count - 1) { s_selected_index++; update_file_list_selection(); }
        } else if (key == LV_KEY_ENTER || key == 13) {
            if (s_screen == SCREEN_NOWPLAYING) {
                toggle_play_pause();
            } else if (s_selected_index >= 0 && s_selected_index < s_visible_count) {
                if (play_track_with_toast(s_selected_index)) show_nowplaying();
                audio_player_update_status();
            }
        }
        return;
    }
}

static void get_audio_player_callback(void **callback)
{
    *callback = audio_player_input_handler;
}

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_input_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#else /* !CONFIG_HAS_AUDIO_PLAYER */

void audio_player_create(void) {}
void audio_player_destroy(void) {}

static void audio_player_dummy_handler(InputEvent *event) { (void)event; }
static void get_audio_player_callback(void **callback) { *callback = audio_player_dummy_handler; }

View audio_player_view = {
    .root = NULL,
    .create = audio_player_create,
    .destroy = audio_player_destroy,
    .input_callback = audio_player_dummy_handler,
    .name = "Audio Player",
    .get_hardwareinput_callback = get_audio_player_callback,
};

#endif /* CONFIG_HAS_AUDIO_PLAYER */
