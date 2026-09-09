#include "sdkconfig.h"
#include "managers/status_display_manager.h"

#ifdef CONFIG_WITH_STATUS_DISPLAY

#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "managers/settings_manager.h"
#include "managers/status_display_animations.h"
#ifdef CONFIG_HAS_LORA
#include "managers/lora_manager.h"
#include "managers/lora_mesh.h"
#include "managers/lora_phoneapi.h"
#endif

static esp_err_t status_display_send(uint8_t control, const uint8_t *data, size_t len);

#define STATUS_DISPLAY_I2C_PORT CONFIG_STATUS_DISPLAY_I2C_PORT
#define STATUS_DISPLAY_ADDR CONFIG_STATUS_DISPLAY_I2C_ADDRESS
#define STATUS_CMD 0x00
#define STATUS_DATA 0x40

#if CONFIG_STATUS_DISPLAY_ROTATE_180
#define STATUS_SEGMENT_REMAP_CMD 0xA1
#define STATUS_COM_SCAN_CMD 0xC8
#else
#define STATUS_SEGMENT_REMAP_CMD 0xA0
#define STATUS_COM_SCAN_CMD 0xC0
#endif

static const char *TAG = "StatusDisplay";

static SemaphoreHandle_t s_mutex;
static bool s_ready;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_display_dev;
static bool s_i2c_bus_owned;
static uint8_t *s_buffer;
static uint8_t s_dirty_pages; // pages pending flush (bit = page)
static uint8_t s_drawn_pages; // pages drawn in the last render pass
#define STATUS_BUFFER_SIZE (128 * 8)
#define STATUS_ANIM_TASK_STACK_BYTES 3072
static char s_line1[24];
static char s_line2[24];
static const int SCALE_Y = CONFIG_STATUS_DISPLAY_SCALE_Y; // 1=1:1 pixels, 2=double-height
#if defined(CONFIG_USE_IO_EXPANDER)
static TickType_t s_next_flush_allowed_tick;
static const TickType_t STATUS_DISPLAY_MIN_FLUSH_INTERVAL_TICKS = pdMS_TO_TICKS(200);
#endif
// idle animation settings
static TimerHandle_t s_idle_timer;
static TickType_t s_last_update_tick;
static const TickType_t ANIM_INTERVAL_TICKS = pdMS_TO_TICKS(150);
static const TickType_t STATUS_UPDATE_MIN_INTERVAL_TICKS = pdMS_TO_TICKS(500);
static TickType_t s_last_status_update_tick;
static int s_anim_frame;
static TaskHandle_t s_anim_task;
static StackType_t *s_anim_task_stack;
static StaticTask_t *s_anim_task_tcb;
static TickType_t s_next_anim_allowed_tick;
static TickType_t s_oom_backoff_until;
static bool s_oom_logged;
#ifdef CONFIG_HAS_LORA
static TickType_t s_last_lora_hud_tick;
#define LORA_MESSAGE_PREVIEW_TICKS pdMS_TO_TICKS(8000)
// 0 = the compact LoRa page, 1..9 = the normal selectable idle animations.
// The PRG button cycles this page while LoRa is running.
static uint8_t s_lora_page;
static bool s_lora_was_running;
static uint16_t s_lora_last_nodes;
static uint32_t s_lora_last_incoming_seq;
static TickType_t s_lora_preview_started_tick;
static bool s_lora_preview_visible;
static char s_lora_preview_who[24];
static char s_lora_preview_text[96];
#endif
// static int s_i2c_error_streak; // unused

static esp_err_t status_display_init_i2c(void) {
    esp_err_t err = i2c_master_get_bus_handle(STATUS_DISPLAY_I2C_PORT, &s_i2c_bus);
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
#if defined(CONFIG_USE_IO_EXPANDER)
        return err;
#else
        i2c_master_bus_config_t conf = {
            .i2c_port = STATUS_DISPLAY_I2C_PORT,
            .sda_io_num = CONFIG_STATUS_DISPLAY_SDA_PIN,
            .scl_io_num = CONFIG_STATUS_DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        err = i2c_new_master_bus(&conf, &s_i2c_bus);
        if (err == ESP_OK) {
            s_i2c_bus_owned = true;
        }
#endif
    }
    if (err != ESP_OK) {
        return err;
    }

    if (!s_display_dev) {
        i2c_device_config_t dev_conf = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = STATUS_DISPLAY_ADDR,
            .scl_speed_hz = 400000,
            .scl_wait_us = 0,
        };
        err = i2c_master_bus_add_device(s_i2c_bus, &dev_conf, &s_display_dev);
    }

    return err;
}

static const uint8_t font_5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5f,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7f,0x14,0x7f,0x14}, {0x24,0x2a,0x7f,0x2a,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1c,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1c,0x00}, {0x14,0x08,0x3e,0x08,0x14}, {0x08,0x08,0x3e,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14},
    {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3e},
    {0x7e,0x11,0x11,0x11,0x7e}, {0x7f,0x49,0x49,0x49,0x36}, {0x3e,0x41,0x41,0x41,0x22},
    {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41}, {0x7f,0x09,0x09,0x09,0x01},
    {0x3e,0x41,0x49,0x49,0x7a}, {0x7f,0x08,0x08,0x08,0x7f}, {0x00,0x41,0x7f,0x41,0x00},
    {0x20,0x40,0x41,0x3f,0x01}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40},
    {0x7f,0x02,0x0c,0x02,0x7f}, {0x7f,0x04,0x08,0x10,0x7f}, {0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06}, {0x3e,0x41,0x51,0x21,0x5e}, {0x7f,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7f,0x01,0x01}, {0x3f,0x40,0x40,0x40,0x3f},
    {0x1f,0x20,0x40,0x20,0x1f}, {0x3f,0x40,0x38,0x40,0x3f}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7f,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7f,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7f,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7f},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e},
    {0x7f,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78},
    {0x7c,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7c,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7c}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0c,0x50,0x50,0x50,0x3c},
    {0x44,0x64,0x54,0x4c,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7f,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x10,0x08,0x08,0x10,0x08}, {0x00,0x06,0x09,0x09,0x06}
};

static esp_err_t status_display_send(uint8_t control, const uint8_t *data, size_t len) {
    if (!data || !len) return ESP_OK;
    TickType_t now = xTaskGetTickCount();
    if (s_oom_backoff_until && now < s_oom_backoff_until) {
        return ESP_ERR_NO_MEM;
    }
    if (!s_display_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    s_oom_backoff_until = 0;
    s_oom_logged = false;
    bool locked = i2c_bus_lock(STATUS_DISPLAY_I2C_PORT, 120);
    if (!locked) {
        ESP_LOGD(TAG, "status display i2c busy, skipping ctrl=0x%02X", control);
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_transmit_multi_buffer_info_t buffers[] = {
        {.write_buffer = &control, .buffer_size = 1},
        {.write_buffer = data, .buffer_size = len},
    };
    esp_err_t err = i2c_master_multi_buffer_transmit(s_display_dev, buffers, 2, 100);
    i2c_bus_unlock(STATUS_DISPLAY_I2C_PORT);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "i2c write failed ctrl=0x%02X len=%u err=%s", control, (unsigned)len, esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "i2c write ok ctrl=0x%02X len=%u", control, (unsigned)len);
    }
    return err;
}

static esp_err_t status_display_write_command(uint8_t command) {
    return status_display_send(STATUS_CMD, &command, 1);
}

static void status_display_flush(void) {
    if (!s_buffer) return;
#if defined(CONFIG_USE_IO_EXPANDER)
    TickType_t now = xTaskGetTickCount();
    if (now < s_next_flush_allowed_tick) {
        return;
    }
    s_next_flush_allowed_tick = now + STATUS_DISPLAY_MIN_FLUSH_INTERVAL_TICKS;
#endif
    uint8_t dirty = s_dirty_pages;
    for (uint8_t page = 0; page < 8; ++page) {
        if (!(dirty & (uint8_t)(1u << page))) continue;
        uint8_t setup[] = { (uint8_t)(0xB0 | page), 0x00, 0x10 };
        if (status_display_send(STATUS_CMD, setup, sizeof(setup)) != ESP_OK) return;
        const uint8_t *chunk = &s_buffer[page * 128];
        if (status_display_send(STATUS_DATA, chunk, 128) != ESP_OK) return;
        s_dirty_pages &= (uint8_t)~(1u << page);
#if defined(CONFIG_USE_IO_EXPANDER)
        // brief pause lets other IO expander clients grab the bus between bursts;
        // skipped after the last page and for partial flushes
        if (s_dirty_pages) vTaskDelay(pdMS_TO_TICKS(5));
#endif
    }
}

static void status_display_clear_buffer(void) {
    if (s_buffer) memset(s_buffer, 0, STATUS_BUFFER_SIZE);
}

static void status_display_plot_pixel(int x, int y, bool on) {
    if (!s_buffer || x < 0 || x >= 128 || y < 0 || y >= 64) return;
    int index = x + (y / 8) * 128;
    uint8_t bit = 1u << (y & 7);
    if (on) s_buffer[index] |= bit; else s_buffer[index] &= (uint8_t)~bit;
    uint8_t page_bit = (uint8_t)(1u << (y / 8));
    s_drawn_pages |= page_bit;
    s_dirty_pages |= page_bit;
}

static void status_display_draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font_5x7[(int)c - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t column = glyph[col];
        for (int row = 0; row < 7; ++row) {
            bool on = (column >> row) & 0x01;
            // scale vertically by drawing multiple rows per bit
            for (int sy = 0; sy < SCALE_Y; ++sy) {
                status_display_plot_pixel(x + col, y + row * SCALE_Y + sy, on);
            }
        }
    }
}

static void status_display_draw_char_rot90_right(int x, int y, char c)
{
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *glyph = font_5x7[(int)c - 32];
    const int w = 5;
    const int h = 7;
    for (int col = 0; col < w; ++col) {
        uint8_t column = glyph[col];
        for (int row = 0; row < h; ++row) {
            bool on = (column >> row) & 0x01;
            if (!on) continue;
            int X = x + (h - 1 - row);
            int Y = y + col * SCALE_Y;
            for (int sy = 0; sy < SCALE_Y; ++sy) {
                status_display_plot_pixel(X, Y + sy, true);
            }
        }
    }
}

static void status_display_draw_text(int x, int y, const char *text) {
    int cursor = x;
    while (*text && cursor < 128 - 6) {
        status_display_draw_char(cursor, y, *text);
        cursor += 6;
        ++text;
    }
}

#ifdef CONFIG_HAS_LORA
static void status_display_lora_frame_locked(void) {
    s_dirty_pages |= s_drawn_pages;
    s_drawn_pages = 0;
    status_display_clear_buffer();

    // A simple frame keeps this page distinct while leaving room for readable text.
    for (int x = 0; x < 128; x++) {
        status_display_plot_pixel(x, 0, true);
        status_display_plot_pixel(x, 63, true);
    }
    for (int y = 0; y < 64; y++) {
        status_display_plot_pixel(0, y, true);
        status_display_plot_pixel(127, y, true);
    }
    for (int x = 1; x < 127; x++) status_display_plot_pixel(x, 11, true);
}

static void status_display_lora_copy_preview(char *out, size_t out_len,
                                             const char *in) {
    if (!out || out_len == 0) return;
    size_t used = 0;
    bool last_space = true;
    while (in && *in && used + 1 < out_len) {
        unsigned char c = (unsigned char)*in++;
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c < 32 || c > 126) c = '?';
        if (c == ' ' && last_space) continue;
        out[used++] = (char)c;
        last_space = (c == ' ');
    }
    while (used > 0 && out[used - 1] == ' ') used--;
    out[used] = '\0';
}

static const char *status_display_lora_wrap_line(const char *src, char out[21]) {
    while (*src == ' ') src++;
    size_t left = strlen(src);
    size_t take = left > 20 ? 20 : left;
    if (left > 20) {
        size_t split = take;
        while (split > 0 && src[split] != ' ') split--;
        if (split > 0) take = split;
    }
    memcpy(out, src, take);
    out[take] = '\0';
    src += take;
    while (*src == ' ') src++;
    return src;
}

static bool status_display_lora_preview_update(TickType_t now) {
    lora_msg_t latest;
    uint32_t seq = 0;
    if (lora_manager_latest_incoming(&latest, &seq) &&
        seq != 0 && seq != s_lora_last_incoming_seq) {
        s_lora_last_incoming_seq = seq;
        status_display_lora_copy_preview(s_lora_preview_who,
                                         sizeof(s_lora_preview_who), latest.who);
        status_display_lora_copy_preview(s_lora_preview_text,
                                         sizeof(s_lora_preview_text), latest.text);
        if (s_lora_preview_who[0] == '\0') snprintf(s_lora_preview_who, sizeof(s_lora_preview_who), "UNKNOWN");
        if (s_lora_preview_text[0] == '\0') snprintf(s_lora_preview_text, sizeof(s_lora_preview_text), "(empty message)");
        s_lora_preview_started_tick = now;
        s_lora_preview_visible = true;
        s_last_lora_hud_tick = 0;
        ESP_LOGI(TAG, "LoRa message preview: %.20s: %.40s",
                 s_lora_preview_who, s_lora_preview_text);
    }
    if (s_lora_preview_visible &&
        now - s_lora_preview_started_tick >= LORA_MESSAGE_PREVIEW_TICKS) {
        s_lora_preview_visible = false;
        s_last_lora_hud_tick = 0;
    }
    return s_lora_preview_visible;
}

static void status_display_render_lora_preview_locked(void) {
    if (!s_buffer) return;
    status_display_lora_frame_locked();

    bool dm = strncmp(s_lora_preview_who, "DM:", 3) == 0;
    const char *who = dm ? s_lora_preview_who + 3 : s_lora_preview_who;
    char line[24];
    snprintf(line, sizeof(line), "%s  %s", dm ? "NEW DM" : "NEW CHAT",
             lora_phoneapi_is_linked() ? "APP:LINK" : "APP:WAIT");
    status_display_draw_text(4, 2, line);
    snprintf(line, sizeof(line), "FROM: %.14s", who);
    status_display_draw_text(4, 15, line);

    const char *next = s_lora_preview_text;
    for (int row = 0; row < 3; row++) {
        char wrapped[21];
        next = status_display_lora_wrap_line(next, wrapped);
        status_display_draw_text(4, 27 + row * 12, wrapped);
        if (*next == '\0') break;
    }
    status_display_flush();
}

static void status_display_render_lora_hud_locked(const lora_status_t *st,
                                                   TickType_t now) {
    if (!st || !s_buffer) return;
    status_display_lora_frame_locked();

    char line[24];
    const char *headline = (st->node_count > s_lora_last_nodes) ? "NEW NODE" : "LORA";
    snprintf(line, sizeof(line), "%s  %s", headline,
             lora_phoneapi_is_linked() ? "APP:LINK" : "APP:WAIT");
    status_display_draw_text(4, 2, line);

    uint32_t mhz = st->freq_hz / 1000000u;
    uint32_t khz = (st->freq_hz % 1000000u) / 1000u;
    snprintf(line, sizeof(line), "%s %u.%03u SF%d",
             lora_region_name((int)st->region), (unsigned)mhz, (unsigned)khz, st->sf);
    status_display_draw_text(4, 15, line);
    snprintf(line, sizeof(line), "N:%d M:%u H:%d", st->node_count,
             (unsigned)lora_manager_msg_count(), st->hop_limit);
    status_display_draw_text(4, 27, line);
    snprintf(line, sizeof(line), "TX:%lu RX:%lu R:%lu",
             (unsigned long)st->tx_ok, (unsigned long)st->rx_ok,
             (unsigned long)st->tx_relay);
    status_display_draw_text(4, 39, line);
    uint32_t errors = st->tx_fail + st->rx_crc_err;
    uint32_t drops = st->q_drops + st->duty_drops;
    bool show_health = (errors || drops) &&
                       (((now / pdMS_TO_TICKS(3000)) & 1u) != 0);
    if (show_health) {
        snprintf(line, sizeof(line), "ERR:%lu Q:%lu DUP:%lu",
                 (unsigned long)errors, (unsigned long)drops,
                 (unsigned long)st->rx_dups);
    } else if (st->last_rssi != 0) {
        snprintf(line, sizeof(line), "RSSI:%d SNR:%.1f", st->last_rssi,
                 (double)st->last_snr);
    } else {
        snprintf(line, sizeof(line), "READY ERR:%lu Q:%lu",
                 (unsigned long)errors, (unsigned long)drops);
    }
    status_display_draw_text(4, 51, line);
    status_display_flush();
    s_lora_last_nodes = (uint16_t)st->node_count;
}
#endif

static void status_display_render_locked(const char *line_one, const char *line_two) {
    // pages from the previous pass still hold content: re-flush them to erase
    s_dirty_pages |= s_drawn_pages;
    s_drawn_pages = 0;
    status_display_clear_buffer();
    int len1 = (int)strlen(line_one);
    int len2 = (int)strlen(line_two);
    int w1 = len1 * 6;
    int w2 = len2 * 6;
    if (w1 > 128) w1 = 128;
    if (w2 > 128) w2 = 128;
    int x1 = (128 - w1) / 2;
    int x2 = (128 - w2) / 2;
    if (x1 < 0) x1 = 0;
    if (x2 < 0) x2 = 0;
    int line_height = 7 * SCALE_Y;
    int gap = 6;
    int total_h = line_height * 2 + gap;
    int y_base = (64 - total_h) / 2;
    if (y_base < 0) y_base = 0;
    status_display_draw_text(x1, y_base, line_one);
    status_display_draw_text(x2, y_base + line_height + gap, line_two);
    status_display_flush();
}

static void status_display_render(const char *line_one, const char *line_two) {
    if (!s_ready) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    status_display_render_locked(line_one, line_two);
    xSemaphoreGive(s_mutex);
}

static void anim_clear(void *user)
{
    (void)user;
    status_display_clear_buffer();
}

static void anim_flush(void *user)
{
    (void)user;
    status_display_flush();
}

static void anim_plot_pixel(void *user, int x, int y, bool on)
{
    (void)user;
    status_display_plot_pixel(x, y, on);
}

static void anim_draw_text(void *user, int x, int y, const char *text)
{
    (void)user;
    status_display_draw_text(x, y, text);
}

static void anim_draw_char_rot90_right(void *user, int x, int y, char c)
{
    (void)user;
    status_display_draw_char_rot90_right(x, y, c);
}

static bool status_idle_delay_elapsed(TickType_t now)
{
    uint32_t timeout_ms = settings_get_status_idle_timeout_ms(&G_Settings);
    if (timeout_ms == 0 || timeout_ms == UINT32_MAX) {
        return false;
    }
    TickType_t required = pdMS_TO_TICKS(timeout_ms);
    return (now - s_last_update_tick) >= required;
}

static void status_display_idle_timer_cb(TimerHandle_t t) {
    (void)t;
    // timer runs in FreeRTOS timer task context; keep it light and just notify
    if (s_anim_task) {
        xTaskNotifyGive(s_anim_task);
    }
}

static void status_display_anim_task(void *arg) {
    (void)arg;
#if CONFIG_STATUS_DISPLAY_BUTTON_PIN >= 0
    int btn_last = 1;
    TickType_t btn_lockout = 0;
#endif
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        TickType_t now = xTaskGetTickCount();
#ifdef CONFIG_HAS_LORA
        lora_status_t lora_status;
        memset(&lora_status, 0, sizeof(lora_status));
        lora_manager_get_status(&lora_status);
        if (lora_status.running && !s_lora_was_running) {
            s_lora_page = 0;
            s_last_lora_hud_tick = 0;
        }
        if (!lora_status.running) {
            s_lora_page = 0;
            s_lora_preview_visible = false;
        }
        s_lora_was_running = lora_status.running;
        bool lora_preview_active = lora_status.running &&
                                   status_display_lora_preview_update(now);
#endif
#if CONFIG_STATUS_DISPLAY_BUTTON_PIN >= 0
        // PRG cycles LoRa compact status + every normal idle animation.
        {
            int lvl = gpio_get_level(CONFIG_STATUS_DISPLAY_BUTTON_PIN);
            if (btn_last == 1 && lvl == 0 &&
                (btn_lockout == 0 || (now - btn_lockout) > pdMS_TO_TICKS(300))) {
#ifdef CONFIG_HAS_LORA
                if (lora_status.running) {
                    if (lora_preview_active) {
                        s_lora_preview_visible = false;
                        lora_preview_active = false;
                        s_last_lora_hud_tick = 0;
                        ESP_LOGI(TAG, "button: dismissed LoRa message preview");
                    } else {
                        s_lora_page = (uint8_t)((s_lora_page + 1) % 10);
                        if (s_lora_page > 0) {
                            settings_set_status_idle_animation(&G_Settings,
                                (IdleAnimation)(s_lora_page - 1));
                            settings_persist_setting(SETTING_IDLE_ANIMATION);
                        }
                        s_last_update_tick = 0;
                        ESP_LOGI(TAG, "button: LoRa page %u/9 (%s)",
                                 (unsigned)s_lora_page,
                                 s_lora_page == 0 ? "HUD" : "animation");
                    }
                } else
#endif
                {
                    IdleAnimation cur = settings_get_status_idle_animation(&G_Settings);
                    IdleAnimation next = (IdleAnimation)(((int)cur + 1) % 9);
                    settings_set_status_idle_animation(&G_Settings, next);
                    settings_persist_setting(SETTING_IDLE_ANIMATION);
                }
                status_display_animations_reset();
                btn_lockout = now;
            }
            btn_last = lvl;
        }
#endif
#ifdef CONFIG_HAS_LORA
        if (lora_preview_active) {
            if (s_last_lora_hud_tick == 0 ||
                now - s_last_lora_hud_tick >= pdMS_TO_TICKS(1000)) {
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    status_display_render_lora_preview_locked();
                    xSemaphoreGive(s_mutex);
                    s_last_lora_hud_tick = now;
                }
            }
            continue;
        }
        if (lora_status.running && s_lora_page == 0) {
            if (s_last_lora_hud_tick == 0 ||
                now - s_last_lora_hud_tick >= pdMS_TO_TICKS(1000)) {
                if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    status_display_render_lora_hud_locked(&lora_status, now);
                    xSemaphoreGive(s_mutex);
                    s_last_lora_hud_tick = now;
                }
            }
            continue;
        }
        s_last_lora_hud_tick = 0;
#endif
#ifdef CONFIG_HAS_LORA
        bool lora_alt_page = lora_status.running && s_lora_page != 0;
#else
        bool lora_alt_page = false;
#endif
        if (!lora_alt_page && !status_idle_delay_elapsed(now)) {
            status_display_animations_reset();
            continue;
        }
        if (now < s_next_anim_allowed_tick) continue;
        s_next_anim_allowed_tick = now + ANIM_INTERVAL_TICKS;

        s_anim_frame++;

        IdleAnimation anim = settings_get_status_idle_animation(&G_Settings);
#ifdef CONFIG_HAS_LORA
        if (lora_alt_page) anim = (IdleAnimation)(s_lora_page - 1);
#endif

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            // pages from the previous pass still hold content: re-flush them to erase
            s_dirty_pages |= s_drawn_pages;
            s_drawn_pages = 0;
            StatusAnimGfx gfx = {
                .clear = anim_clear,
                .flush = anim_flush,
                .plot_pixel = anim_plot_pixel,
                .draw_text = anim_draw_text,
                .draw_char_rot90_right = anim_draw_char_rot90_right,
                .width = 128,
                .height = 64,
                .scale_y = SCALE_Y,
                .font_char_width = 5,
                .font_char_height = 7,
                .user = NULL,
            };

            status_display_clear_buffer();
            status_display_animations_step(anim, now, s_anim_frame, &gfx);
            status_display_flush();
            xSemaphoreGive(s_mutex);
        }
    }
}

static void status_display_sanitize(char *dst, size_t dst_len, const char *src) {
    if (!dst_len) return;
    size_t pos = 0;
    while (src && *src && pos < dst_len - 1) {
        char c = *src++;
        if (!isprint((unsigned char)c)) c = ' ';
        dst[pos++] = c;
    }
    dst[pos] = '\0';
}

static bool status_display_ensure_anim_task(void) {
    if (s_anim_task != NULL) {
        return true;
    }
    // LoRa builds keep the worker because it also owns the live radio HUD.
#ifndef CONFIG_HAS_LORA
    // Idle animation disabled -> no worker needed, ever. Saves 3k stack + TCB.
    uint32_t timeout_ms = settings_get_status_idle_timeout_ms(&G_Settings);
    if (timeout_ms == 0 || timeout_ms == UINT32_MAX) {
        return false;
    }
#endif
    s_next_anim_allowed_tick = 0;
    s_anim_task_stack = heap_caps_malloc(STATUS_ANIM_TASK_STACK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_anim_task_tcb = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_anim_task_stack && s_anim_task_tcb) {
        s_anim_task = xTaskCreateStatic(status_display_anim_task, "status_anim",
                                        STATUS_ANIM_TASK_STACK_BYTES, NULL,
                                        tskIDLE_PRIORITY + 1,
                                        s_anim_task_stack, s_anim_task_tcb);
        if (s_anim_task) {
            ESP_LOGI(TAG, "status animation task stack allocated from PSRAM: %d bytes", STATUS_ANIM_TASK_STACK_BYTES);
            return true;
        }
    }
    free(s_anim_task_stack);
    free(s_anim_task_tcb);
    s_anim_task_stack = NULL;
    s_anim_task_tcb = NULL;
    if (xTaskCreate(status_display_anim_task, "status_anim", STATUS_ANIM_TASK_STACK_BYTES,
                    NULL, tskIDLE_PRIORITY + 1, &s_anim_task) == pdPASS) {
        return true;
    }
    s_anim_task = NULL;
    return false;
}

void status_display_init(void) {
    if (s_ready) return;

    ESP_LOGI(TAG, "initializing status display on I2C port %d addr 0x%02X", STATUS_DISPLAY_I2C_PORT, STATUS_DISPLAY_ADDR);

    // Configure power control pin (Vext) if specified
#if CONFIG_STATUS_DISPLAY_POWER_PIN >= 0
    gpio_reset_pin(CONFIG_STATUS_DISPLAY_POWER_PIN);
    gpio_set_direction(CONFIG_STATUS_DISPLAY_POWER_PIN, GPIO_MODE_OUTPUT);
    // Set LOW to power on display (Heltec V3 Vext pin behavior)
    gpio_set_level(CONFIG_STATUS_DISPLAY_POWER_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for power to stabilize
    ESP_LOGI(TAG, "power control pin %d set LOW (display ON)", CONFIG_STATUS_DISPLAY_POWER_PIN);
#endif

    // Configure reset pin if specified
#if CONFIG_STATUS_DISPLAY_RESET_PIN >= 0
    gpio_reset_pin(CONFIG_STATUS_DISPLAY_RESET_PIN);
    gpio_set_direction(CONFIG_STATUS_DISPLAY_RESET_PIN, GPIO_MODE_OUTPUT);
    // Reset sequence: LOW -> delay -> HIGH
    gpio_set_level(CONFIG_STATUS_DISPLAY_RESET_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CONFIG_STATUS_DISPLAY_RESET_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "reset pin %d sequence completed", CONFIG_STATUS_DISPLAY_RESET_PIN);
#endif

    // Animation-cycle button (e.g. Heltec V3 PRG): input with pullup,
    // polled by the anim task. -1 disables.
#if CONFIG_STATUS_DISPLAY_BUTTON_PIN >= 0
    gpio_reset_pin(CONFIG_STATUS_DISPLAY_BUTTON_PIN);
    gpio_set_direction(CONFIG_STATUS_DISPLAY_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CONFIG_STATUS_DISPLAY_BUTTON_PIN, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "animation button on pin %d", CONFIG_STATUS_DISPLAY_BUTTON_PIN);
#endif

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            ESP_LOGE(TAG, "failed to create mutex");
            return;
        }
    }

    if (!s_buffer) {
#if CONFIG_SPIRAM
        s_buffer = heap_caps_malloc(STATUS_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_buffer) {
            ESP_LOGI(TAG, "buffer allocated in PSRAM");
        } else
#endif
        {
            s_buffer = heap_caps_malloc(STATUS_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_buffer) {
            ESP_LOGE(TAG, "failed to allocate display buffer");
            return;
        }
    }

    esp_err_t err = status_display_init_i2c();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status display I2C init failed: %s", esp_err_to_name(err));
        return;
    }

    // quick probe: display off
    if (status_display_write_command(0xAE) != ESP_OK) {
        ESP_LOGE(TAG, "probe failed (driver missing or device NACK)");
        return;
    }

    uint8_t init_cmds[] = {
        // addressing mode: PAGE addressing (matches per-page flush)
        0x20, 0x02, 0xB0, STATUS_COM_SCAN_CMD, 0x00, 0x10, 0x40,
        0x81, 0x8F, STATUS_SEGMENT_REMAP_CMD, 0xA6, 0xA8, 0x3F, 0xA4, 0xD3,
        0x00, 0xD5, 0x80, 0xD9, 0xF1, 0xDA, 0x12, 0xDB,
        0x40, 0x8D, 0x14, 0xAF
    };

    for (size_t i = 0; i < sizeof(init_cmds); ++i) {
        ESP_LOGD(TAG, "send init cmd[%u]=0x%02X", (unsigned)i, init_cmds[i]);
        if (status_display_write_command(init_cmds[i]) != ESP_OK) {
            ESP_LOGE(TAG, "command 0x%02X failed", init_cmds[i]);
            return;
        }
    }

    // clear any garbage before first text
    status_display_clear_buffer();
    s_dirty_pages = 0xFF;
    status_display_flush();

    s_ready = true;
    status_display_sanitize(s_line1, sizeof(s_line1), "GhostESP: Revival");
    status_display_sanitize(s_line2, sizeof(s_line2), "made with <3");
    status_display_render(s_line1, s_line2);
    // setup idle animation timer
    s_last_update_tick = xTaskGetTickCount();
    s_last_status_update_tick = 0;
    s_anim_frame = 0;
    s_idle_timer = xTimerCreate("status_idle", ANIM_INTERVAL_TICKS, pdTRUE, NULL, status_display_idle_timer_cb);
    if (s_idle_timer) {
        xTimerStart(s_idle_timer, 0);
    }
    // create animation worker task (lazy: skipped when idle timeout disabled)
    (void)status_display_ensure_anim_task();
    ESP_LOGI(TAG, "status display ready");
}

bool status_display_is_ready(void) {
    return s_ready;
}

void status_display_set_lines(const char *line_one, const char *line_two) {
    if (!s_ready) {
        ESP_LOGW(TAG, "set_lines called while display not ready");
        return;
    }
    char tmp1[sizeof(s_line1)];
    char tmp2[sizeof(s_line2)];
    status_display_sanitize(tmp1, sizeof(tmp1), line_one ? line_one : "");
    status_display_sanitize(tmp2, sizeof(tmp2), line_two ? line_two : "");
    if (strcmp(tmp1, s_line1) == 0 && strcmp(tmp2, s_line2) == 0) return;
    strcpy(s_line1, tmp1);
    strcpy(s_line2, tmp2);
    status_display_render(s_line1, s_line2);
    // reset idle timer
    s_last_update_tick = xTaskGetTickCount();
    status_display_animations_reset();
    // late-create anim worker if idle animation was enabled after boot
    (void)status_display_ensure_anim_task();
}

void status_display_show_attack(const char *attack_name, const char *target) {
    if (!s_ready) {
        ESP_LOGW(TAG, "show_attack ignored (display not ready)");
        return;
    }
    char line_one[sizeof(s_line1)];
    snprintf(line_one, sizeof(line_one), "Attack: %s", attack_name ? attack_name : "?");
    const char *line_two_src = target ? target : "";
    status_display_set_lines(line_one, line_two_src);
}

void status_display_show_status(const char *status_line) {
    if (!s_ready) {
        ESP_LOGW(TAG, "show_status ignored (display not ready)");
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (now - s_last_status_update_tick < STATUS_UPDATE_MIN_INTERVAL_TICKS) {
        return;
    }
    s_last_status_update_tick = now;
    status_display_set_lines("Status:", status_line ? status_line : "");
}

void status_display_clear(void) {
    if (!s_ready) return;
    status_display_set_lines("", "");
}

void status_display_deinit(void) {
    if (!s_ready) return;
    ESP_LOGI(TAG, "deinitializing status display");
    status_display_clear_buffer();
    s_dirty_pages = 0xFF;
    status_display_flush();
    s_ready = false;
    if (s_idle_timer) {
        xTimerStop(s_idle_timer, 0);
        xTimerDelete(s_idle_timer, 0);
        s_idle_timer = NULL;
    }
    if (s_anim_task) {
        vTaskDelete(s_anim_task);
        s_anim_task = NULL;
    }
    heap_caps_free(s_anim_task_stack);
    heap_caps_free(s_anim_task_tcb);
    s_anim_task_stack = NULL;
    s_anim_task_tcb = NULL;
    if (s_display_dev) {
        i2c_master_bus_rm_device(s_display_dev);
        s_display_dev = NULL;
    }
    if (s_i2c_bus_owned && s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
        s_i2c_bus = NULL;
        s_i2c_bus_owned = false;
    }
#if CONFIG_STATUS_DISPLAY_POWER_PIN >= 0
    // Turn off display by setting power pin HIGH
    gpio_set_level(CONFIG_STATUS_DISPLAY_POWER_PIN, 1);
    ESP_LOGI(TAG, "power control pin %d set HIGH (display OFF)", CONFIG_STATUS_DISPLAY_POWER_PIN);
#endif
}

#else

#include <string.h>
#include "managers/views/tdongle_status_screen.h"

static bool status_display_use_tdongle_lcd(void)
{
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-S3") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0;
#else
    return false;
#endif
}

void status_display_init(void) {}

bool status_display_is_ready(void)
{
    return status_display_use_tdongle_lcd() && tdongle_status_is_ready();
}

void status_display_set_lines(const char *a, const char *b)
{
    if (status_display_use_tdongle_lcd()) tdongle_status_set_lines(a, b);
}

void status_display_show_attack(const char *a, const char *b)
{
    if (status_display_use_tdongle_lcd()) tdongle_status_show_attack(a, b);
}

void status_display_show_status(const char *s)
{
    if (status_display_use_tdongle_lcd()) tdongle_status_show_status(s);
}

void status_display_clear(void)
{
    if (status_display_use_tdongle_lcd()) tdongle_status_clear();
}

void status_display_deinit(void) {}

#endif
