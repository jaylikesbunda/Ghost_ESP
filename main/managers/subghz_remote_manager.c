#include "managers/subghz_remote_manager.h"
#include "managers/subghz_decoders.h"
#include "managers/ghostchi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_SUBGHZ

#include "core/esp_comm_manager.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "managers/sd_card_manager.h"
#include "rom/ets_sys.h"
#include "soc/soc_caps.h"

#include <dirent.h>
#include <errno.h>
#include "gui/toast.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define SUBGHZ_TASK_SLEEP_MS 45
#define SUBGHZ_WATERFALL_SLEEP_MS 2
#define SUBGHZ_SNAPSHOT_DIR "/mnt/ghostesp/subghz"
#define SUBGHZ_SNAPSHOT_EXT ".sub"
#define SUBGHZ_RAW_TIMEOUT_US 20000

#define SUBGHZ_FREQ_COUNT 5
static const uint32_t s_scan_freqs[SUBGHZ_FREQ_COUNT] = {
    315000000U,
    390000000U,
    433920000U,
    868350000U,
    915000000U,
};
static const char *s_scan_freq_labels[SUBGHZ_FREQ_COUNT] = {
    "315 MHz", "390 MHz", "433.92 MHz", "868.35 MHz", "915 MHz",
};

#define CC1101_REG_IOCFG2   0x00
#define CC1101_REG_IOCFG0   0x02
#define CC1101_REG_FIFOTHR  0x03
#define CC1101_REG_PKTLEN   0x06
#define CC1101_REG_PKTCTRL1 0x07
#define CC1101_REG_PKTCTRL0 0x08
#define CC1101_REG_FSCTRL1  0x0B
#define CC1101_REG_FREQ2    0x0D
#define CC1101_REG_FREQ1    0x0E
#define CC1101_REG_FREQ0    0x0F
#define CC1101_REG_MDMCFG4  0x10
#define CC1101_REG_MDMCFG3  0x11
#define CC1101_REG_MDMCFG2  0x12
#define CC1101_REG_MDMCFG1  0x13
#define CC1101_REG_MDMCFG0  0x14
#define CC1101_REG_DEVIATN  0x15
#define CC1101_REG_MCSM0    0x18
#define CC1101_REG_FOCCFG   0x19
#define CC1101_REG_BSCFG    0x1A
#define CC1101_REG_AGCCTRL2 0x1B
#define CC1101_REG_AGCCTRL1 0x1C
#define CC1101_REG_AGCCTRL0 0x1D
#define CC1101_REG_FREND1   0x21
#define CC1101_REG_FREND0   0x22
#define CC1101_REG_FSCAL3   0x23
#define CC1101_REG_FSCAL2   0x24
#define CC1101_REG_FSCAL1   0x25
#define CC1101_REG_FSCAL0   0x26
#define CC1101_REG_TEST2    0x2C
#define CC1101_REG_TEST1    0x2D
#define CC1101_REG_TEST0    0x2E
#define CC1101_REG_FSCAL3   0x23
#define CC1101_REG_FSCAL2   0x24
#define CC1101_REG_FSCAL1   0x25
#define CC1101_REG_FSCAL0   0x26
#define CC1101_REG_WORCTRL  0x20

typedef struct {
    uint8_t reg;
    uint8_t val;
} cc1101_reg_entry_t;

static const cc1101_reg_entry_t s_preset_ook270[] = {
    {CC1101_REG_IOCFG0,   0x0D},
    {CC1101_REG_FIFOTHR,  0x47},
    {CC1101_REG_PKTCTRL0, 0x32},
    {CC1101_REG_FSCTRL1,  0x06},
    {CC1101_REG_MDMCFG0,  0x00},
    {CC1101_REG_MDMCFG1,  0x00},
    {CC1101_REG_MDMCFG2,  0x30},
    {CC1101_REG_MDMCFG3,  0x32},
    {CC1101_REG_MDMCFG4,  0x67},
    {CC1101_REG_MCSM0,    0x18},
    {CC1101_REG_FOCCFG,   0x18},
    {CC1101_REG_AGCCTRL0, 0x40},
    {CC1101_REG_AGCCTRL1, 0x00},
    {CC1101_REG_AGCCTRL2, 0x03},
    {CC1101_REG_WORCTRL,  0xFB},
    {CC1101_REG_FREND0,   0x11},
    {CC1101_REG_FREND1,   0xB6},
    {0, 0},
};

static const cc1101_reg_entry_t s_preset_ook650[] = {
    {CC1101_REG_IOCFG0,   0x0D},
    {CC1101_REG_FIFOTHR,  0x07},
    {CC1101_REG_PKTCTRL0, 0x32},
    {CC1101_REG_FSCTRL1,  0x06},
    {CC1101_REG_MDMCFG0,  0x00},
    {CC1101_REG_MDMCFG1,  0x00},
    {CC1101_REG_MDMCFG2,  0x30},
    {CC1101_REG_MDMCFG3,  0x32},
    {CC1101_REG_MDMCFG4,  0x17},
    {CC1101_REG_MCSM0,    0x18},
    {CC1101_REG_FOCCFG,   0x18},
    {CC1101_REG_AGCCTRL0, 0x91},
    {CC1101_REG_AGCCTRL1, 0x00},
    {CC1101_REG_AGCCTRL2, 0x07},
    {CC1101_REG_WORCTRL,  0xFB},
    {CC1101_REG_FREND0,   0x11},
    {CC1101_REG_FREND1,   0xB6},
    {0, 0},
};

static const uint8_t s_ook_patable[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

#define CC1101_STATUS_PARTNUM 0x30
#define CC1101_STATUS_VERSION 0x31
#define CC1101_STATUS_RSSI    0x34

#define CC1101_STROBE_SRES  0x30
#define CC1101_STROBE_SCAL  0x33
#define CC1101_STROBE_SRX   0x34
#define CC1101_STROBE_STX   0x35
#define CC1101_STROBE_SIDLE 0x36
#define CC1101_STROBE_SFRX  0x3A
#define CC1101_STROBE_SFTX  0x3B

#define SUBGHZ_RMT_RESOLUTION_HZ 1000000U
#define SUBGHZ_RMT_MAX_DURATION_TICKS 32767U

static const char *TAG = "SubGHzRemoteMgr";

static TaskHandle_t s_subghz_task = NULL;
static volatile bool s_stop_requested = false;
static volatile bool s_paused = false;
static volatile bool s_stream_to_peer = false;
static volatile bool s_waterfall_stream_requested = false;

static spi_device_handle_t s_spi_dev = NULL;
static spi_host_device_t s_spi_host = SPI3_HOST;
static bool s_spi_bus_initialized_by_us = false;

static uint8_t s_levels[SUBGHZ_SCANNER_CHANNEL_COUNT];
static uint8_t s_next_channel = 0;
static uint8_t s_waterfall_line[SUBGHZ_SCANNER_CHANNEL_COUNT];
static uint8_t s_waterfall_ready_line[SUBGHZ_SCANNER_CHANNEL_COUNT];
static uint8_t s_waterfall_count = 0;
static uint8_t s_waterfall_ready_count = 0;
static uint8_t s_waterfall_freq_idx = 2;
static uint8_t s_waterfall_ready_freq_idx = 2;
static uint16_t s_waterfall_seq = 0;
static bool s_waterfall_ready = false;
static char s_last_error[96] = "none";
static SemaphoreHandle_t s_data_mutex = NULL;
static SemaphoreHandle_t s_radio_mutex = NULL;
static uint8_t s_snapshot_levels[SUBGHZ_SCANNER_CHANNEL_COUNT];
static uint8_t s_snapshot_cursor = 0;
static bool s_snapshot_valid = false;
static char s_active_snapshot_name[SUBGHZ_SNAPSHOT_NAME_MAX] = "none";
static esp_timer_handle_t s_raw_timeout_timer = NULL;
static volatile bool s_raw_active = false;
static volatile bool s_raw_ready = false;
static volatile bool s_raw_capture_enabled = false;
static volatile uint32_t s_raw_last_time_us = 0;
static volatile int32_t s_raw_workbufs[2][SUBGHZ_RAW_MAX_DURATIONS];
static volatile uint8_t s_raw_isr_buf_idx = 0;
static volatile size_t s_raw_worklen = 0;
static volatile size_t s_raw_completed_len = 0;
static volatile int s_raw_prev_level = 0;
static volatile int32_t *s_raw_stream_ptr = NULL;
static size_t s_raw_stream_count = 0;
static bool s_raw_capture_pending = false;
static int32_t s_shared_buf[SUBGHZ_RAW_MAX_DURATIONS];
static size_t s_rx_stream_expected = 0;
static size_t s_rx_stream_received = 0;
static uint32_t s_rx_stream_freq_hz = 0;
static subghz_preset_t s_rx_stream_preset = SUBGHZ_PRESET_OOK270_ASYNC;
static subghz_decoder_engine_t s_decoder_engine;
static volatile bool s_decode_result_ready = false;
static volatile uint32_t s_current_freq_hz = 433920000U;
static volatile uint8_t s_current_freq_idx = 2;
static volatile bool s_radio_ready = false;
static volatile bool s_capture_request_pending = false;
static volatile bool s_capture_request_raw = false;
static volatile uint32_t s_capture_request_freq_hz = 433920000U;
static volatile uint32_t s_capture_request_id = 0;
static volatile uint32_t s_capture_completed_id = 0;
static volatile bool s_capture_request_ok = false;
static volatile bool s_capture_raw_mode_active = false;
static char s_capture_request_error[96] = "none";

static inline void subghz_radio_lock(void) {
    if (s_radio_mutex) {
        xSemaphoreTake(s_radio_mutex, portMAX_DELAY);
    }
}

static inline void subghz_radio_unlock(void) {
    if (s_radio_mutex) {
        xSemaphoreGive(s_radio_mutex);
    }
}

typedef struct {
    bool level;
    uint32_t duration;
} subghz_edge_t;

#define SUBGHZ_EDGE_QUEUE_LEN 256
static QueueHandle_t s_edge_queue = NULL;
static TaskHandle_t s_decoder_task = NULL;
static volatile bool s_decoder_task_running = false;

static void subghz_hw_stop(void);
static void subghz_set_last_error(const char *msg);
static void subghz_gdo0_isr_handler(void *arg);
static void subghz_raw_timeout_cb(void *arg);
static void subghz_stream_raw_capture(void);
static void subghz_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static esp_err_t cc1101_write_reg(uint8_t reg, uint8_t value);
static esp_err_t cc1101_write_patable(const uint8_t *data, size_t len);
static esp_err_t subghz_apply_preset(subghz_preset_t preset);
static esp_err_t subghz_retune_frequency(uint32_t freq_hz);
static int subghz_frequency_to_index(uint32_t frequency_hz);
static void subghz_reset_capture_buffers(void);
static void subghz_finalize_raw_capture(bool allow_short_capture);
static void subghz_prepare_raw_capture_for_stream(void);
static void subghz_finish_capture_request(uint32_t request_id, bool ok, const char *reason);
static void subghz_process_capture_request(void);

static esp_err_t subghz_apply_preset(subghz_preset_t preset) {
    const cc1101_reg_entry_t *tbl = (preset == SUBGHZ_PRESET_OOK270_ASYNC) ? s_preset_ook270 : s_preset_ook650;
    esp_err_t err = ESP_OK;
    for (int i = 0; tbl[i].reg != 0 && err == ESP_OK; i++) {
        err = cc1101_write_reg(tbl[i].reg, tbl[i].val);
    }
    if (err == ESP_OK) {
        err = cc1101_write_patable(s_ook_patable, 8);
    }
    return err;
}

static void subghz_build_default_snapshot_name(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }

    unsigned tick = (unsigned)xTaskGetTickCount();
    snprintf(out, out_len, "snapshot_%08X", tick);
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

    out[pos] = '\0';
    if (pos == 0) {
        subghz_build_default_snapshot_name(out, out_len);
    }
}

static void subghz_build_snapshot_path(const char *name, char *out_path, size_t out_path_len) {
    if (!out_path || out_path_len == 0) {
        return;
    }

    const char *safe = (name && name[0] != '\0') ? name : "snapshot";
    snprintf(out_path, out_path_len, "%s/%s%s", SUBGHZ_SNAPSHOT_DIR, safe, SUBGHZ_SNAPSHOT_EXT);
}

static bool subghz_ensure_snapshot_dir(void) {
    if (!sd_card_exists("/mnt/ghostesp")) {
        subghz_set_last_error("sd card not mounted");
        return false;
    }

    if (mkdir(SUBGHZ_SNAPSHOT_DIR, 0777) == 0) {
        return true;
    }

    if (errno == EEXIST) {
        return true;
    }

    subghz_set_last_error("snapshot dir create failed");
    return false;
}

static bool subghz_sd_begin(bool *display_was_suspended) {
    if (display_was_suspended) {
        *display_was_suspended = false;
    }

    if (sd_card_needs_jit_mount()) {
        esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
        if (mount_err != ESP_OK) {
            subghz_set_last_error("sd mount failed");
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

static void subghz_set_last_error(const char *msg) {
    if (!msg || msg[0] == '\0') {
        msg = "unknown";
    }
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

static int subghz_frequency_to_index(uint32_t frequency_hz) {
    for (int i = 0; i < SUBGHZ_FREQ_COUNT; i++) {
        if (s_scan_freqs[i] == frequency_hz) {
            return i;
        }
    }
    return -1;
}

static void subghz_reset_capture_buffers(void) {
    s_raw_active = false;
    s_raw_ready = false;
    s_raw_worklen = 0;
    s_raw_completed_len = 0;
    s_raw_stream_count = 0;
    s_raw_capture_pending = false;
    s_decode_result_ready = false;
    if (s_raw_timeout_timer) {
        esp_timer_stop(s_raw_timeout_timer);
    }
    if (s_edge_queue) {
        xQueueReset(s_edge_queue);
    }
    subghz_engine_reset(&s_decoder_engine);
}

static void subghz_finalize_raw_capture(bool allow_short_capture) {
    if (!s_raw_active) {
        return;
    }

    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t delta = (now_us >= s_raw_last_time_us) ? (now_us - s_raw_last_time_us)
                                                    : (UINT32_MAX - s_raw_last_time_us + now_us + 1U);
    if (delta > 0 && s_raw_worklen < SUBGHZ_RAW_MAX_DURATIONS) {
        s_raw_workbufs[s_raw_isr_buf_idx][s_raw_worklen++] = s_raw_prev_level ? (int32_t)delta : -(int32_t)delta;
    }

    s_raw_completed_len = s_raw_worklen;
    s_raw_active = false;
    s_raw_ready = allow_short_capture ? (s_raw_completed_len > 0) : (s_raw_completed_len > 8);
    s_raw_isr_buf_idx = 1 - s_raw_isr_buf_idx;
    if (s_raw_ready) {
        char sub_payload[24];
        snprintf(sub_payload, sizeof(sub_payload), "%u", (unsigned)s_raw_completed_len);
        ghostscript_emit_event("subghz_captured", sub_payload);
    }
}

static void subghz_prepare_raw_capture_for_stream(void) {
    if (!s_raw_ready) {
        return;
    }

    uint8_t completed_idx = 1 - s_raw_isr_buf_idx;
    size_t len = s_raw_completed_len;
    if (len > SUBGHZ_RAW_MAX_DURATIONS) {
        len = SUBGHZ_RAW_MAX_DURATIONS;
    }

    s_raw_stream_ptr = (volatile int32_t *)s_raw_workbufs[completed_idx];
    s_raw_stream_count = len;
    s_raw_capture_pending = (len > 0);
    s_raw_completed_len = 0;
    s_raw_ready = false;
}

static void subghz_finish_capture_request(uint32_t request_id, bool ok, const char *reason) {
    if (s_capture_request_id != request_id) {
        return;
    }

    s_capture_request_ok = ok;
    if (ok) {
        snprintf(s_capture_request_error, sizeof(s_capture_request_error), "none");
        subghz_set_last_error("none");
    } else {
        const char *msg = (reason && reason[0] != '\0') ? reason : "capture arm failed";
        snprintf(s_capture_request_error, sizeof(s_capture_request_error), "%s", msg);
        subghz_set_last_error(s_capture_request_error);
    }
    s_capture_request_pending = false;
    s_capture_completed_id = request_id;
}

static void subghz_process_capture_request(void) {
    if (!s_capture_request_pending) {
        return;
    }

    uint32_t request_id = s_capture_request_id;
    uint32_t frequency_hz = s_capture_request_freq_hz;
    bool raw_mode = s_capture_request_raw;
    int freq_idx = subghz_frequency_to_index(frequency_hz);
    if (freq_idx < 0) {
        subghz_finish_capture_request(request_id, false, "unsupported frequency");
        return;
    }

    if (!s_spi_dev || !s_radio_ready) {
        subghz_finish_capture_request(request_id, false, "radio not ready");
        return;
    }

    ESP_LOGI(TAG, "arming %s capture @ %lu Hz", raw_mode ? "raw" : "normal", (unsigned long)frequency_hz);

    s_paused = false;
    if (s_raw_capture_enabled) {
        subghz_remote_manager_set_raw_capture_enabled(false);
    }
    subghz_reset_capture_buffers();
    s_capture_raw_mode_active = raw_mode;

    esp_err_t err = subghz_retune_frequency(frequency_hz);
    if (s_capture_request_id != request_id) {
        return;
    }
    if (err != ESP_OK) {
        subghz_finish_capture_request(request_id, false, subghz_remote_manager_get_last_error());
        return;
    }

    s_current_freq_idx = (uint8_t)freq_idx;
    subghz_remote_manager_set_raw_capture_enabled(true);
    s_capture_raw_mode_active = raw_mode;
    if (s_capture_request_id != request_id) {
        subghz_remote_manager_set_raw_capture_enabled(false);
        return;
    }

    subghz_finish_capture_request(request_id, true, NULL);
}

static void IRAM_ATTR subghz_gdo0_isr_handler(void *arg) {
    (void)arg;
    if (!s_raw_capture_enabled) return;
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    int level = gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);

    if (s_raw_timeout_timer) {
        esp_timer_stop(s_raw_timeout_timer);
    }

    if (!s_raw_active) {
        s_raw_active = true;
        s_raw_worklen = 0;
        s_raw_prev_level = level;
        s_raw_last_time_us = now_us;
        if (s_raw_timeout_timer) {
            esp_timer_start_once(s_raw_timeout_timer, SUBGHZ_RAW_TIMEOUT_US);
        }
        return;
    }

    uint32_t delta = (now_us >= s_raw_last_time_us) ? (now_us - s_raw_last_time_us)
                                                    : (UINT32_MAX - s_raw_last_time_us + now_us + 1U);
    if (delta > 0 && s_raw_worklen < SUBGHZ_RAW_MAX_DURATIONS) {
        s_raw_workbufs[s_raw_isr_buf_idx][s_raw_worklen++] = s_raw_prev_level ? (int32_t)delta : -(int32_t)delta;
    }

    if (s_edge_queue && !s_capture_raw_mode_active && !s_decode_result_ready && delta > 0) {
        subghz_edge_t edge = { .level = (bool)s_raw_prev_level, .duration = delta };
        xQueueSendFromISR(s_edge_queue, &edge, NULL);
    }

    s_raw_prev_level = level;
    s_raw_last_time_us = now_us;

    if (s_raw_timeout_timer) {
        esp_timer_start_once(s_raw_timeout_timer, SUBGHZ_RAW_TIMEOUT_US);
    }
}

static void subghz_raw_timeout_cb(void *arg) {
    (void)arg;
    if (!s_raw_active) {
        return;
    }

    subghz_finalize_raw_capture(false);
    ESP_LOGI(TAG, "raw timeout: %lu transitions captured, ready=%d", (unsigned long)s_raw_completed_len, s_raw_ready);
}

static void subghz_stream_raw_capture(void) {
    if (!s_stream_to_peer || !esp_comm_manager_is_connected() || !s_raw_capture_pending || s_raw_stream_count == 0 ||
        ((!s_capture_raw_mode_active) && s_decode_result_ready) || s_paused) {
        ESP_LOGI(TAG, "stream_raw: skip pending=%d count=%lu online=%d", s_raw_capture_pending, (unsigned long)s_raw_stream_count, esp_comm_manager_is_connected());
        return;
    }

    ESP_LOGI(TAG, "streaming raw capture: %lu durations", (unsigned long)s_raw_stream_count);

    uint8_t start_pkt[4] = { SUBGHZ_STREAM_VERSION, 1, (uint8_t)(s_raw_stream_count & 0xFF),
                             (uint8_t)((s_raw_stream_count >> 8) & 0xFF) };
    if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, start_pkt, sizeof(start_pkt))) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    size_t offset = 0;
    while (offset < s_raw_stream_count) {
        size_t chunk = s_raw_stream_count - offset;
        if (chunk > 13) {
            chunk = 13;
        }

        uint8_t pkt[5 + 13 * 4] = {0};
        pkt[0] = SUBGHZ_STREAM_VERSION;
        pkt[1] = 2;
        pkt[2] = (uint8_t)(offset & 0xFF);
        pkt[3] = (uint8_t)((offset >> 8) & 0xFF);
        pkt[4] = (uint8_t)chunk;
        for (size_t i = 0; i < chunk; i++) {
            int32_t v = s_raw_stream_ptr[offset + i];
            size_t base = 5 + i * 4;
            pkt[base + 0] = (uint8_t)(v & 0xFF);
            pkt[base + 1] = (uint8_t)((v >> 8) & 0xFF);
            pkt[base + 2] = (uint8_t)((v >> 16) & 0xFF);
            pkt[base + 3] = (uint8_t)((v >> 24) & 0xFF);
        }
        if (!esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, pkt, 5 + chunk * 4)) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        offset += chunk;
    }

    uint8_t end_pkt[2] = { SUBGHZ_STREAM_VERSION, 3 };
    (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, end_pkt, sizeof(end_pkt));
}

static void subghz_stream_decoded_result(void) {
    const subghz_stream_decoder_t *res = subghz_engine_get_result(&s_decoder_engine);
    if (!res) return;

    char info[SUBGHZ_DECODED_INFO_MAX] = {0};
    subghz_stream_decoder_format_result(res, info, sizeof(info));

    uint8_t name_len = 0;
    while (res->name[name_len] && name_len < 31) name_len++;

    uint8_t pkt[2 + 1 + 8 + 1 + name_len + 1 + 4];
    size_t pos = 0;
    pkt[pos++] = SUBGHZ_STREAM_VERSION;
    pkt[pos++] = 9;
    pkt[pos++] = name_len;
    memcpy(pkt + pos, res->name, name_len); pos += name_len;
    uint64_t code = res->code;
    for (int i = 0; i < 8; i++) pkt[pos++] = (uint8_t)(code >> (i * 8));
    pkt[pos++] = (uint8_t)res->bits;
    uint32_t freq = s_current_freq_hz;
    pkt[pos++] = (uint8_t)(freq & 0xFF);
    pkt[pos++] = (uint8_t)((freq >> 8) & 0xFF);
    pkt[pos++] = (uint8_t)((freq >> 16) & 0xFF);
    pkt[pos++] = (uint8_t)((freq >> 24) & 0xFF);

    (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, pkt, pos);
    ESP_LOGI(TAG, "streamed decoded: %s %dbit code=0x%llX freq=%u",
             res->name, res->bits, (unsigned long long)res->code, (unsigned)freq);
}

static void subghz_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!data || length < 2) return;
    uint8_t ver = data[0];
    if (ver != SUBGHZ_STREAM_VERSION && ver != 1) return;

    uint8_t packet_type = data[1];

    /* v2 REPLAY_BEGIN (0x10) or legacy start (4) */
    if (packet_type == 0x10 || packet_type == 4) {
        if (packet_type == 0x10 && length >= 8) {
            s_rx_stream_expected = (size_t)data[2] | ((size_t)data[3] << 8) |
                                    ((size_t)data[4] << 16) | ((size_t)data[5] << 24);
            s_rx_stream_freq_hz = (uint32_t)data[6] |
                                  ((uint32_t)data[7] << 8) |
                                  ((uint32_t)data[8] << 16) |
                                  ((uint32_t)data[9] << 0x18);
            if (length >= 11) {
                uint8_t pb = data[10];
                if (pb == 1) s_rx_stream_preset = SUBGHZ_PRESET_OOK650_ASYNC;
                else if (pb == 2) s_rx_stream_preset = SUBGHZ_PRESET_2FSK_DEV238_ASYNC;
                else if (pb == 3) s_rx_stream_preset = SUBGHZ_PRESET_2FSK_DEV476_ASYNC;
                else if (pb == 4) s_rx_stream_preset = SUBGHZ_PRESET_CUSTOM;
                else s_rx_stream_preset = SUBGHZ_PRESET_OOK270_ASYNC;
            }
        } else if (length >= 4) {
            s_rx_stream_expected = (size_t)data[2] | ((size_t)data[3] << 8);
            s_rx_stream_freq_hz = 0;
            s_rx_stream_preset = SUBGHZ_PRESET_OOK270_ASYNC;
            if (length >= 8) {
                s_rx_stream_freq_hz = (uint32_t)data[4] |
                                      ((uint32_t)data[5] << 8) |
                                      ((uint32_t)data[6] << 16) |
                                      ((uint32_t)data[7] << 0x18);
            }
            if (length >= 9) {
                s_rx_stream_preset = (data[8] == 1) ? SUBGHZ_PRESET_OOK650_ASYNC : SUBGHZ_PRESET_OOK270_ASYNC;
            }
        } else {
            return;
        }
        if (s_rx_stream_expected > SUBGHZ_RAW_MAX_DURATIONS) s_rx_stream_expected = SUBGHZ_RAW_MAX_DURATIONS;
        s_rx_stream_received = 0;
        if (s_rx_stream_freq_hz == 0) s_rx_stream_freq_hz = 433920000;
        return;
    }
    if (packet_type == 0x11 || packet_type == 5) {
        if (length < 5) return;
        size_t offset = (size_t)data[2] | ((size_t)data[3] << 8);
        size_t count = (size_t)data[4];
        if (length < 5 + count * 4 || offset + count > SUBGHZ_RAW_MAX_DURATIONS) {
            ESP_LOGE(TAG, "Stream chunk DROPPED: bounds check fail");
            return;
        }
        for (size_t i = 0; i < count; i++) {
            size_t base = 5 + i * 4;
            s_shared_buf[offset + i] = (int32_t)((uint32_t)data[base] |
                                                    ((uint32_t)data[base + 1] << 8) |
                                                    ((uint32_t)data[base + 2] << 16) |
                                                    ((uint32_t)data[base + 3] << 24));
        }
        if (offset + count > s_rx_stream_received) s_rx_stream_received = offset + count;
        if (offset == 0 && count > 0) {
            ESP_LOGD(TAG, "Stream chunk[0]: count=%u first4=%ld %ld %ld %ld",
                     (unsigned)count,
                     (long)s_shared_buf[0], (long)s_shared_buf[1],
                     (long)s_shared_buf[2], (long)s_shared_buf[3]);
        }
        return;
    }
    if (packet_type == 0x12 || packet_type == 6) {
        if (s_rx_stream_received > 0) {
            ESP_LOGD(TAG, "Stream TX trigger: %lu durations, first4=%ld %ld %ld %ld",
                     (unsigned long)s_rx_stream_received,
                     (long)s_shared_buf[0], (long)s_shared_buf[1],
                     (long)s_shared_buf[2], (long)s_shared_buf[3]);
            (void)subghz_remote_manager_transmit_raw(s_shared_buf, s_rx_stream_received, s_rx_stream_freq_hz, s_rx_stream_preset);
        }
    }
}

static bool subghz_validate_pin_config(void) {
    const int mosi = CONFIG_SUBGHZ_SPI_MOSI_PIN;
    const int miso = CONFIG_SUBGHZ_SPI_MISO_PIN;
    const int sck = CONFIG_SUBGHZ_SPI_SCK_PIN;
    const int csn = CONFIG_SUBGHZ_CSN_PIN;
    const int gdo0 = CONFIG_SUBGHZ_GDO0_PIN;
    const int gdo2 = CONFIG_SUBGHZ_GDO2_PIN;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(mosi)) {
        subghz_set_last_error("invalid MOSI pin");
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(miso)) {
        subghz_set_last_error("invalid MISO pin");
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(sck)) {
        subghz_set_last_error("invalid SCK pin");
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(csn)) {
        subghz_set_last_error("invalid CSN pin");
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(gdo0)) {
        subghz_set_last_error("invalid GDO0 pin");
        return false;
    }
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
    if (!GPIO_IS_VALID_GPIO(gdo2)) {
        subghz_set_last_error("invalid GDO2 pin");
        return false;
    }
#endif

    int pins[6] = { mosi, miso, sck, csn, gdo0, gdo2 };
    for (int i = 0; i < 6; i++) {
        if (pins[i] < 0) {
            continue;
        }
        for (int j = i + 1; j < 6; j++) {
            if (pins[j] < 0) {
                continue;
            }
            if (pins[i] == pins[j]) {
                subghz_set_last_error("pin conflict");
                return false;
            }
        }
    }

    return true;
}

static inline spi_host_device_t subghz_spi_host_from_config(void) {
#if defined(SPI3_HOST)
    if (CONFIG_SUBGHZ_SPI_HOST == 2) {
        return SPI2_HOST;
    }
    return SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}

static esp_err_t subghz_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_spi_dev || !tx || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (uint32_t)(len * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    return spi_device_polling_transmit(s_spi_dev, &t);
}

static esp_err_t cc1101_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = { reg, value };
    uint8_t rx[2] = {0};
    return subghz_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t cc1101_read_status(uint8_t status_reg, uint8_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[2] = { (uint8_t)(status_reg | 0xC0), 0x00 };
    uint8_t rx[2] = {0};
    esp_err_t err = subghz_spi_transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

static esp_err_t cc1101_strobe(uint8_t strobe_cmd) {
    uint8_t tx[1] = { strobe_cmd };
    uint8_t rx[1] = {0};
    return subghz_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t cc1101_get_state(uint8_t *state) {
    if (!state) return ESP_ERR_INVALID_ARG;
    uint8_t tx[1] = { 0x3D };
    uint8_t rx[1] = {0};
    esp_err_t err = subghz_spi_transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *state = (rx[0] >> 4) & 0x07;
    }
    return err;
}

static esp_err_t cc1101_write_patable(const uint8_t *data, size_t len) {
    if (!data || len == 0 || !s_spi_dev) return ESP_ERR_INVALID_ARG;
    size_t total = 1 + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0x7E;
    memcpy(&buf[1], data, len);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (uint32_t)(total * 8);
    t.tx_buffer = buf;
    t.rx_buffer = NULL;
    esp_err_t err = spi_device_polling_transmit(s_spi_dev, &t);
    free(buf);
    return err;
}

static esp_err_t subghz_hw_start(void) {
    s_spi_host = subghz_spi_host_from_config();

    gpio_config_t gdo_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SUBGHZ_GDO0_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
    {
        gdo_cfg.pin_bit_mask |= (1ULL << CONFIG_SUBGHZ_GDO2_PIN);
    }
#endif

    esp_err_t err = gpio_config(&gdo_cfg);
    if (err != ESP_OK) {
        subghz_set_last_error("gdo config failed");
        return err;
    }

    if (!s_raw_timeout_timer) {
        esp_timer_create_args_t timer_args = {
            .callback = subghz_raw_timeout_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "subghz_raw_to",
        };
        err = esp_timer_create(&timer_args, &s_raw_timeout_timer);
        if (err != ESP_OK) {
            subghz_set_last_error("raw timer create failed");
            return err;
        }
    }

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        subghz_set_last_error("gpio isr service failed");
        return isr_ret;
    }
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    err = gpio_isr_handler_add((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, subghz_gdo0_isr_handler, NULL);
    if (err != ESP_OK) {
        subghz_set_last_error("gdo0 isr add failed");
        return err;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SUBGHZ_SPI_MOSI_PIN,
        .miso_io_num = CONFIG_SUBGHZ_SPI_MISO_PIN,
        .sclk_io_num = CONFIG_SUBGHZ_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_spi_bus_initialized_by_us = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_spi_bus_initialized_by_us = false;
        err = ESP_OK;
    }

    if (err != ESP_OK) {
        subghz_set_last_error("spi bus init failed");
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_SUBGHZ_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_SUBGHZ_CSN_PIN,
        .queue_size = 1,
    };

    err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        subghz_set_last_error("add spi device failed");
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(s_spi_host);
            s_spi_bus_initialized_by_us = false;
        }
        s_spi_dev = NULL;
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    err = cc1101_strobe(CC1101_STROBE_SRES);
    if (err != ESP_OK) {
        subghz_set_last_error("radio reset failed");
        subghz_hw_stop();
        return err;
    }

    ets_delay_us(1000);
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    (void)cc1101_strobe(CC1101_STROBE_SFRX);
    (void)cc1101_strobe(CC1101_STROBE_SFTX);

    s_current_freq_hz = (uint32_t)((uint64_t)CONFIG_SUBGHZ_BASE_FREQ_MHZ * 10000ULL);
    uint32_t freq_word = (uint32_t)((((uint64_t)s_current_freq_hz) * 65536ULL) / 26000000ULL);

    err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));

    if (err == ESP_OK) err = subghz_apply_preset(SUBGHZ_PRESET_OOK650_ASYNC);

    if (err == ESP_OK) err = cc1101_write_reg(0x0A, 0x00);
    if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SCAL);
    if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SRX);

    if (err != ESP_OK) {
        subghz_set_last_error("radio init sequence failed");
        subghz_hw_stop();
        return err;
    }

    uint8_t version = 0;
    uint8_t partnum = 0;
    if (cc1101_read_status(CC1101_STATUS_VERSION, &version) != ESP_OK ||
        cc1101_read_status(CC1101_STATUS_PARTNUM, &partnum) != ESP_OK) {
        subghz_set_last_error("radio version read failed");
        subghz_hw_stop();
        return ESP_FAIL;
    }

    if ((version == 0x00 || version == 0xFF) && (partnum == 0x00 || partnum == 0xFF)) {
        subghz_set_last_error("cc1101 not detected");
        subghz_hw_stop();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "CC1101 init OK SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d GDO0=%d GDO2=%d ver=0x%02X part=0x%02X",
             (int)s_spi_host,
             CONFIG_SUBGHZ_SPI_MOSI_PIN,
             CONFIG_SUBGHZ_SPI_MISO_PIN,
             CONFIG_SUBGHZ_SPI_SCK_PIN,
             CONFIG_SUBGHZ_CSN_PIN,
             CONFIG_SUBGHZ_GDO0_PIN,
             CONFIG_SUBGHZ_GDO2_PIN,
             version,
             partnum);

    s_radio_ready = true;

    return ESP_OK;
}

static esp_err_t subghz_retune_frequency(uint32_t freq_hz) {
    if (!s_spi_dev || !s_radio_ready) {
        subghz_set_last_error("radio not ready");
        return ESP_ERR_INVALID_STATE;
    }

    subghz_radio_lock();

    uint32_t freq_word = (uint32_t)((((uint64_t)freq_hz) * 65536ULL) / 26000000ULL);
    const char *step = "SIDLE";
    esp_err_t err = cc1101_strobe(CC1101_STROBE_SIDLE);
    if (err == ESP_OK) {
        step = "FREQ2";
        err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
    }
    if (err == ESP_OK) {
        step = "FREQ1";
        err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
    }
    if (err == ESP_OK) {
        step = "FREQ0";
        err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));
    }
    if (err == ESP_OK) {
        step = "SCAL";
        err = cc1101_strobe(CC1101_STROBE_SCAL);
    }
    if (err == ESP_OK) {
        step = "SRX";
        err = cc1101_strobe(CC1101_STROBE_SRX);
    }

    subghz_radio_unlock();

    if (err == ESP_OK) {
        s_current_freq_hz = freq_hz;
        subghz_set_last_error("none");
    } else {
        char reason[96];
        snprintf(reason, sizeof(reason), "retune %s failed", step);
        subghz_set_last_error(reason);
        ESP_LOGW(TAG,
                 "retune to %lu Hz failed at %s: %s",
                 (unsigned long)freq_hz,
                 step,
                 esp_err_to_name(err));
    }
    return err;
}

static void subghz_hw_stop(void) {
    s_radio_ready = false;
    s_raw_capture_enabled = false;
    s_capture_raw_mode_active = false;
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    if (s_raw_timeout_timer) {
        esp_timer_stop(s_raw_timeout_timer);
    }
    if (s_spi_dev) {
        (void)cc1101_strobe(CC1101_STROBE_SIDLE);
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }

    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
    }
}

static uint8_t subghz_sample_channel(uint8_t ch, int settle_us) {
    subghz_radio_lock();

    if (cc1101_write_reg(0x0A, ch) != ESP_OK) {
        subghz_radio_unlock();
        return 0;
    }
    if (cc1101_strobe(CC1101_STROBE_SRX) != ESP_OK) {
        subghz_radio_unlock();
        return 0;
    }

    ets_delay_us((uint32_t)settle_us);

    uint8_t raw = 0;
    if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
        subghz_radio_unlock();
        return 0;
    }

    subghz_radio_unlock();

    int8_t raw_signed = (int8_t)raw;
    int rssi_dbm = (raw_signed / 2) - 74;
    int level = ((rssi_dbm + 110) * 100) / 70;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    return (uint8_t)level;
}

static uint8_t s_rssi_smooth[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};

static bool subghz_sample_rssi_waterfall_line(uint8_t *out_levels, uint8_t count) {
    if (!out_levels || count == 0) {
        return false;
    }
    if (count > SUBGHZ_SCANNER_CHANNEL_COUNT) {
        count = SUBGHZ_SCANNER_CHANNEL_COUNT;
    }

    subghz_radio_lock();
    bool ok = true;
    for (uint8_t ch = 0; ch < count && ok; ch++) {
        if (ch == 32) {
            subghz_radio_unlock();
            vTaskDelay(pdMS_TO_TICKS(1));
            subghz_radio_lock();
        }
        if (cc1101_write_reg(0x0A, ch) != ESP_OK) {
            ok = false;
            break;
        }
        if (cc1101_strobe(CC1101_STROBE_SRX) != ESP_OK) {
            ok = false;
            break;
        }
        ets_delay_us(200);
        uint8_t raw = 0;
        if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
            ok = false;
            break;
        }
        int8_t raw_signed = (int8_t)raw;
        int rssi_dbm = (raw_signed / 2) - 74;
        int level = ((rssi_dbm + 110) * 100) / 70;
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        // light temporal IIR per channel: 75% previous + 25% new
        // smooths noise without creating cross-frame ghosts like old demod FFT
        int smooth = ((int)s_rssi_smooth[ch] * 3 + level) / 4;
        if (smooth < 0) smooth = 0;
        if (smooth > 100) smooth = 100;
        s_rssi_smooth[ch] = (uint8_t)smooth;
        out_levels[ch] = (uint8_t)smooth;
    }
    subghz_radio_unlock();
    return ok;
}

static uint8_t s_wf_band_idx = 0;

static void subghz_stream_chunk(uint8_t cursor, uint8_t start_ch, uint8_t count) {
    if (!s_stream_to_peer || !esp_comm_manager_is_connected() || count == 0) {
        return;
    }

    if (count > 32) {
        count = 32;
    }

    uint8_t pkt[7 + 32] = {0};
    pkt[0] = SUBGHZ_STREAM_VERSION;
    pkt[1] = 0;
    pkt[2] = cursor;
    pkt[3] = start_ch;
    pkt[4] = count;
    pkt[5] = s_current_freq_idx;
    pkt[6] = (uint8_t)(s_current_freq_hz & 0xFF);

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    for (uint8_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)((start_ch + i) % SUBGHZ_SCANNER_CHANNEL_COUNT);
        pkt[7 + i] = s_levels[ch];
    }
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }

    (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, pkt, (size_t)(7 + count));
}

static void subghz_stream_waterfall_line(uint8_t freq_idx, const uint8_t *line, uint8_t count, uint16_t seq) {
    if (!s_stream_to_peer || !esp_comm_manager_is_connected() || !line || count == 0) {
        return;
    }
    if (count > SUBGHZ_SCANNER_CHANNEL_COUNT) {
        count = SUBGHZ_SCANNER_CHANNEL_COUNT;
    }

    if (seq == 1 || (seq % 32U) == 0U) {
        uint8_t peak = 0;
        for (uint8_t i = 0; i < count; i++) {
            if (line[i] > peak) {
                peak = line[i];
            }
        }
        ESP_LOGI(TAG, "waterfall tx seq=%u freq_idx=%u bins=%u peak=%u", (unsigned)seq, (unsigned)freq_idx, (unsigned)count, (unsigned)peak);
    }

    uint8_t offset = 0;
    while (offset < count) {
        uint8_t chunk = (uint8_t)(count - offset);
        if (chunk > 32) {
            chunk = 32;
        }
        uint8_t pkt[8 + 32] = {0};
        pkt[0] = SUBGHZ_STREAM_VERSION;
        pkt[1] = SUBGHZ_STREAM_WATERFALL_CHUNK;
        pkt[2] = count;
        pkt[3] = freq_idx;
        pkt[4] = (uint8_t)(seq & 0xFF);
        pkt[5] = (uint8_t)((seq >> 8) & 0xFF);
        pkt[6] = offset;
        pkt[7] = chunk;
        memcpy(pkt + 8, line + offset, chunk);
        (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_SUBGHZ, pkt, (size_t)(8 + chunk));
        offset = (uint8_t)(offset + chunk);
    }
}

static void subghz_decoder_task(void *arg) {
    (void)arg;
    subghz_edge_t edge;
    while (s_decoder_task_running) {
        if (xQueueReceive(s_edge_queue, &edge, pdMS_TO_TICKS(20))) {
            if (!s_decode_result_ready) {
                subghz_engine_feed(&s_decoder_engine, edge.level, edge.duration);
                if (s_decoder_engine.found) {
                    s_decode_result_ready = true;
                    s_raw_active = false;
                    s_raw_ready = false;
                    s_raw_worklen = 0;
                    s_raw_stream_count = 0;
                    s_raw_capture_pending = false;
                    if (s_raw_timeout_timer) {
                        esp_timer_stop(s_raw_timeout_timer);
                    }
                }
            }
        }
    }
    vTaskDelete(NULL);
}

static void subghz_scan_task(void *arg) {
    (void)arg;

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    memset(s_levels, 0, sizeof(s_levels));
    memset(s_waterfall_line, 0, sizeof(s_waterfall_line));
    memset(s_waterfall_ready_line, 0, sizeof(s_waterfall_ready_line));
    memset(s_rssi_smooth, 0, sizeof(s_rssi_smooth));
    s_wf_band_idx = 0;
    s_next_channel = 0;
    s_waterfall_count = 0;
    s_waterfall_ready_count = 0;
    s_waterfall_freq_idx = s_current_freq_idx;
    s_waterfall_ready_freq_idx = s_current_freq_idx;
    s_waterfall_ready = false;
    s_raw_stream_count = 0;
    s_raw_stream_ptr = NULL;
    s_raw_capture_pending = false;
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }
    s_raw_worklen = 0;
    s_raw_ready = false;
    s_raw_active = false;
    s_decode_result_ready = false;
    s_capture_raw_mode_active = false;
    s_capture_request_pending = false;
    s_capture_request_ok = false;
    s_capture_completed_id = 0;
    snprintf(s_capture_request_error, sizeof(s_capture_request_error), "none");
    subghz_engine_init(&s_decoder_engine);

    s_edge_queue = xQueueCreate(SUBGHZ_EDGE_QUEUE_LEN, sizeof(subghz_edge_t));
    s_decoder_task_running = true;
    xTaskCreatePinnedToCore(subghz_decoder_task, "subghz_dec", 5120, NULL, 15, &s_decoder_task, 1);

    if (subghz_hw_start() != ESP_OK) {
        if (s_stream_to_peer && esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("subghz", "state error");
        }
        s_decoder_task_running = false;
        if (s_decoder_task) { vTaskDelete(s_decoder_task); s_decoder_task = NULL; }
        if (s_edge_queue) { vQueueDelete(s_edge_queue); s_edge_queue = NULL; }
        s_subghz_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    bool skip_initial_freq_cycle = true;

    while (!s_stop_requested) {
        if (!s_capture_raw_mode_active && s_decode_result_ready && s_stream_to_peer && esp_comm_manager_is_connected()) {
            subghz_stream_decoded_result();
            s_decode_result_ready = false;
            subghz_engine_reset(&s_decoder_engine);
        }

        if (s_raw_ready) {
            subghz_prepare_raw_capture_for_stream();
            subghz_stream_raw_capture();
        }

        if (s_capture_request_pending) {
            subghz_process_capture_request();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (s_paused || s_raw_capture_enabled) {
            vTaskDelay(pdMS_TO_TICKS(s_raw_capture_enabled ? 10 : 60));
            continue;
        }

        if (s_waterfall_stream_requested) {
            uint8_t line[SUBGHZ_SCANNER_CHANNEL_COUNT];
            uint8_t line_freq_idx = s_wf_band_idx;
            bool line_ready = false;
            uint16_t line_seq = 0;

            if (subghz_retune_frequency(s_scan_freqs[line_freq_idx]) == ESP_OK) {
                line_ready = subghz_sample_rssi_waterfall_line(line, SUBGHZ_SCANNER_CHANNEL_COUNT);
            }

            if (line_ready) {
                if (s_data_mutex) {
                    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
                }
                memcpy(s_waterfall_ready_line, line, sizeof(s_waterfall_ready_line));
                memcpy(s_waterfall_line, line, sizeof(s_waterfall_line));
                memcpy(s_levels, line, sizeof(s_levels));
                s_waterfall_ready_count = SUBGHZ_SCANNER_CHANNEL_COUNT;
                s_waterfall_ready_freq_idx = line_freq_idx;
                s_waterfall_freq_idx = line_freq_idx;
                s_waterfall_seq++;
                line_seq = s_waterfall_seq;
                s_waterfall_ready = true;
                s_snapshot_cursor = 0;
                memcpy(s_snapshot_levels, s_levels, sizeof(s_snapshot_levels));
                s_snapshot_valid = true;
                if (s_data_mutex) {
                    xSemaphoreGive(s_data_mutex);
                }
                subghz_stream_waterfall_line(line_freq_idx, line, SUBGHZ_SCANNER_CHANNEL_COUNT, line_seq);
                vTaskDelay(pdMS_TO_TICKS(1));
            }

            s_wf_band_idx = (uint8_t)((s_wf_band_idx + 1) % SUBGHZ_FREQ_COUNT);

            vTaskDelay(pdMS_TO_TICKS(SUBGHZ_WATERFALL_SLEEP_MS));
            continue;
        }

        int channels_per_tick = CONFIG_SUBGHZ_ANALYZER_CHANNELS_PER_TICK;
        int settle_us = CONFIG_SUBGHZ_ANALYZER_SETTLE_US;
        if (channels_per_tick < 1) channels_per_tick = 1;
        if (channels_per_tick > 32) channels_per_tick = 32;
        if (settle_us < 100) settle_us = 100;

        if (s_next_channel == 0 && s_waterfall_count == 0) {
            if (skip_initial_freq_cycle) {
                skip_initial_freq_cycle = false;
            } else {
                uint8_t next_idx = (s_current_freq_idx + 1) % SUBGHZ_FREQ_COUNT;
                if (subghz_retune_frequency(s_scan_freqs[next_idx]) == ESP_OK) {
                    s_current_freq_idx = next_idx;
                    ESP_LOGD(TAG, "scan freq: %s", s_scan_freq_labels[next_idx]);
                }
            }
        }

        uint8_t start_ch = s_next_channel;
        uint8_t line_copy[SUBGHZ_SCANNER_CHANNEL_COUNT];
        uint8_t line_count = 0;
        uint8_t line_freq_idx = 0;
        uint16_t line_seq = 0;
        bool line_ready = false;

        if (s_data_mutex) {
            xSemaphoreTake(s_data_mutex, portMAX_DELAY);
        }
        for (int i = 0; i < channels_per_tick; i++) {
            uint8_t ch = s_next_channel;
            s_next_channel = (uint8_t)((s_next_channel + 1) % SUBGHZ_SCANNER_CHANNEL_COUNT);

            uint8_t sample = subghz_sample_channel(ch, settle_us);
            s_levels[ch] = (uint8_t)((s_levels[ch] * 3 + sample) / 4);
            s_waterfall_line[ch] = sample;
            if (ch + 1 > s_waterfall_count) {
                s_waterfall_count = (uint8_t)(ch + 1);
            }
            if (s_next_channel == 0 && s_waterfall_count >= SUBGHZ_SCANNER_CHANNEL_COUNT) {
                s_waterfall_count = SUBGHZ_SCANNER_CHANNEL_COUNT;
                s_waterfall_freq_idx = s_current_freq_idx;
                s_waterfall_seq++;
                s_waterfall_ready = true;
                memcpy(s_waterfall_ready_line, s_waterfall_line, sizeof(s_waterfall_ready_line));
                s_waterfall_ready_count = s_waterfall_count;
                s_waterfall_ready_freq_idx = s_waterfall_freq_idx;
                memcpy(line_copy, s_waterfall_line, sizeof(line_copy));
                line_count = s_waterfall_count;
                line_freq_idx = s_waterfall_freq_idx;
                line_seq = s_waterfall_seq;
                line_ready = true;
                s_waterfall_count = 0;
            }
        }
        uint8_t cursor = s_next_channel;
        if (s_data_mutex) {
            xSemaphoreGive(s_data_mutex);
        }

        subghz_stream_chunk(cursor, start_ch, (uint8_t)channels_per_tick);
        (void)line_ready;
        (void)line_copy;
        (void)line_count;
        (void)line_freq_idx;
        (void)line_seq;
        vTaskDelay(pdMS_TO_TICKS(SUBGHZ_TASK_SLEEP_MS));
    }

    s_decoder_task_running = false;
    if (s_decoder_task) {
        vTaskDelete(s_decoder_task);
        s_decoder_task = NULL;
    }
    if (s_edge_queue) {
        vQueueDelete(s_edge_queue);
        s_edge_queue = NULL;
    }

    subghz_hw_stop();
    if (s_stream_to_peer && esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("subghz", "state stopped");
    }

    s_subghz_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

bool subghz_remote_manager_start(bool stream_to_peer) {
    s_stream_to_peer = stream_to_peer;
    s_waterfall_stream_requested = false;
    s_stop_requested = false;
    s_paused = false;

    if (!s_data_mutex) {
        s_data_mutex = xSemaphoreCreateMutex();
    }
    if (!s_radio_mutex) {
        s_radio_mutex = xSemaphoreCreateMutex();
    }

    if (s_subghz_task) {
        subghz_set_last_error("none");
        return true;
    }

    if (!subghz_validate_pin_config()) {
        return false;
    }

    const uint32_t stack_size = 4096;
    BaseType_t ok = xTaskCreateWithCaps(subghz_scan_task, "subghz_scan", stack_size, NULL, 5, &s_subghz_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(subghz_scan_task, "subghz_scan", stack_size, NULL, 5, &s_subghz_task,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        subghz_set_last_error("task create failed");
        return false;
    }

    subghz_set_last_error("none");
    return true;
}

bool subghz_remote_manager_start_waterfall(bool stream_to_peer) {
    bool ok = subghz_remote_manager_start(stream_to_peer);
    if (ok) {
        s_waterfall_stream_requested = true;
    }
    return ok;
}

void subghz_remote_manager_stop(void) {
    s_waterfall_stream_requested = false;
    s_stop_requested = true;
}

void subghz_remote_manager_set_paused(bool paused) {
    s_paused = paused;
}

bool subghz_remote_manager_is_running(void) {
    return s_subghz_task != NULL;
}

bool subghz_remote_manager_is_paused(void) {
    return s_paused;
}

bool subghz_remote_manager_is_ready(void) {
    return s_subghz_task != NULL && s_radio_ready;
}

bool subghz_remote_manager_begin_capture(bool raw_mode, uint32_t frequency_hz, bool stream_to_peer, uint32_t timeout_ms) {
    if (subghz_frequency_to_index(frequency_hz) < 0) {
        subghz_set_last_error("unsupported frequency");
        return false;
    }

    if (!s_subghz_task) {
        if (!subghz_remote_manager_start(stream_to_peer)) {
            return false;
        }
    } else {
        s_stream_to_peer = stream_to_peer;
    }

    int64_t ready_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (!subghz_remote_manager_is_ready()) {
        if (!subghz_remote_manager_is_running()) {
            if (strcmp(subghz_remote_manager_get_last_error(), "none") == 0) {
                subghz_set_last_error("radio init timeout");
            }
            return false;
        }
        if (esp_timer_get_time() >= ready_deadline_us) {
            subghz_set_last_error("radio init timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    uint32_t request_id = s_capture_request_id + 1;
    if (request_id == 0) {
        request_id = 1;
    }
    s_capture_request_raw = raw_mode;
    s_capture_request_freq_hz = frequency_hz;
    s_capture_request_ok = false;
    snprintf(s_capture_request_error, sizeof(s_capture_request_error), "none");
    s_capture_request_id = request_id;
    s_capture_request_pending = true;

    int64_t arm_deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000LL);
    while (s_capture_completed_id != request_id) {
        if (!subghz_remote_manager_is_running()) {
            if (strcmp(subghz_remote_manager_get_last_error(), "none") == 0) {
                subghz_set_last_error("capture arm interrupted");
            }
            return false;
        }
        if (esp_timer_get_time() >= arm_deadline_us) {
            s_capture_request_pending = false;
            if (s_capture_request_id == request_id) {
                s_capture_request_id = request_id + 1;
            }
            subghz_set_last_error("capture arm timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!s_capture_request_ok) {
        if (strcmp(s_capture_request_error, "none") != 0) {
            subghz_set_last_error(s_capture_request_error);
        }
        return false;
    }

    subghz_set_last_error("none");
    return true;
}

const char *subghz_remote_manager_get_last_error(void) {
    return s_last_error;
}

bool subghz_remote_manager_get_levels(uint8_t *out_levels, size_t max_levels, uint8_t *out_cursor) {
    if (!out_levels || max_levels == 0) {
        return false;
    }

    size_t copy_len = max_levels;
    if (copy_len > SUBGHZ_SCANNER_CHANNEL_COUNT) {
        copy_len = SUBGHZ_SCANNER_CHANNEL_COUNT;
    }

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    memcpy(out_levels, s_levels, copy_len);
    if (out_cursor) {
        *out_cursor = s_next_channel;
    }
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }

    return true;
}

bool subghz_remote_manager_take_waterfall_line(uint8_t *out_levels, size_t max_levels, uint8_t *out_count, uint8_t *out_freq_idx, uint16_t *out_seq) {
    if (!out_levels || max_levels == 0) {
        return false;
    }

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }

    bool ready = s_waterfall_ready && s_waterfall_ready_count > 0;
    if (ready) {
        size_t copy_len = s_waterfall_ready_count;
        if (copy_len > max_levels) {
            copy_len = max_levels;
        }
        memcpy(out_levels, s_waterfall_ready_line, copy_len);
        if (out_count) {
            *out_count = (uint8_t)copy_len;
        }
        if (out_freq_idx) {
            *out_freq_idx = s_waterfall_ready_freq_idx;
        }
        if (out_seq) {
            *out_seq = s_waterfall_seq;
        }
        s_waterfall_ready = false;
    }

    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }

    return ready;
}

bool subghz_remote_manager_take_raw_capture(int32_t *out_durations, size_t max_durations, size_t *out_count) {
    if (!out_durations || max_durations == 0) {
        return false;
    }

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    if (!s_raw_capture_pending || s_raw_stream_count == 0) {
        if (out_count) {
            *out_count = 0;
        }
        if (s_data_mutex) {
            xSemaphoreGive(s_data_mutex);
        }
        return false;
    }

    size_t copy = s_raw_stream_count;
    if (copy > max_durations) {
        copy = max_durations;
    }
    memcpy(out_durations, (const void *)s_raw_stream_ptr, copy * sizeof(int32_t));
    s_raw_capture_pending = false;
    if (out_count) {
        *out_count = copy;
    }
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }
    return true;
}

bool subghz_remote_manager_take_decode_result(subghz_decoded_signal_t *out_result) {
    if (!out_result) return false;
    if (!s_decode_result_ready) return false;

    const subghz_stream_decoder_t *res = subghz_engine_get_result(&s_decoder_engine);
    if (!res) {
        s_decode_result_ready = false;
        return false;
    }

    memset(out_result, 0, sizeof(*out_result));
    snprintf(out_result->protocol, sizeof(out_result->protocol), "%s", res->name);
    out_result->code = res->code;
    out_result->bits = res->bits;
    out_result->frequency_hz = (int)s_current_freq_hz;
    out_result->te = (int)res->te_short;
    out_result->decoded = true;

    subghz_stream_decoder_format_result(res, out_result->info, sizeof(out_result->info));

    s_decode_result_ready = false;
    subghz_engine_reset(&s_decoder_engine);
    return true;
}

bool subghz_remote_manager_transmit_raw(const int32_t *durations, size_t count, uint32_t frequency_hz, subghz_preset_t preset) {
    if (!durations || count == 0) {
        subghz_set_last_error("no raw durations");
        return false;
    }
    if (s_subghz_task) {
        subghz_set_last_error("stop scanner before replay");
        return false;
    }
    if (!subghz_validate_pin_config()) {
        return false;
    }

    rmt_channel_handle_t tx_chan = NULL;
    rmt_encoder_handle_t copy_encoder = NULL;
    rmt_symbol_word_t *symbols = NULL;

    s_spi_host = subghz_spi_host_from_config();

    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, 0);
    gpio_config_t gdo_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SUBGHZ_GDO0_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&gdo_cfg);
    if (err != ESP_OK) {
        subghz_set_last_error("gdo0 output config failed");
        return false;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SUBGHZ_SPI_MOSI_PIN,
        .miso_io_num = CONFIG_SUBGHZ_SPI_MISO_PIN,
        .sclk_io_num = CONFIG_SUBGHZ_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        s_spi_bus_initialized_by_us = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        s_spi_bus_initialized_by_us = false;
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        subghz_set_last_error("spi bus init failed");
        return false;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_SUBGHZ_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_SUBGHZ_CSN_PIN,
        .queue_size = 1,
    };
    err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        subghz_set_last_error("add spi device failed");
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(s_spi_host);
            s_spi_bus_initialized_by_us = false;
        }
        s_spi_dev = NULL;
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    err = cc1101_strobe(CC1101_STROBE_SIDLE);
    if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SRES);
    if (err != ESP_OK) {
        subghz_set_last_error("radio reset failed");
        subghz_hw_stop();
        return false;
    }
    ets_delay_us(1000);
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    (void)cc1101_strobe(CC1101_STROBE_SFRX);
    (void)cc1101_strobe(CC1101_STROBE_SFTX);

    err = cc1101_write_reg(CC1101_REG_IOCFG0, 0x2E);
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_IOCFG2, 0x2E);
    if (err != ESP_OK) {
        subghz_set_last_error("tx reset sequence failed");
        subghz_hw_stop();
        return false;
    }

    err = subghz_apply_preset(preset);

    if (frequency_hz == 0) {
        frequency_hz = (uint32_t)((uint64_t)CONFIG_SUBGHZ_BASE_FREQ_MHZ * 10000ULL);
    }
    uint64_t f_hz = (uint64_t)frequency_hz;
    uint32_t freq_word = (uint32_t)((f_hz * 65536ULL) / 26000000ULL);
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(0x0A, 0x00);

    if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SCAL);
    if (err == ESP_OK) {
        for (int i = 0; i < 200; i++) {
            uint8_t state = 0;
            if (cc1101_get_state(&state) == ESP_OK && state == 0x00) {
                break;
            }
            ets_delay_us(100);
        }
    }
    if (err != ESP_OK) {
        subghz_set_last_error("tx register setup failed");
        subghz_hw_stop();
        return false;
    }

    size_t chunk_count = 0;
    for (size_t i = 0; i < count; i++) {
        uint32_t dur = (uint32_t)llabs((long long)durations[i]);
        if (dur == 0) continue;
        chunk_count += (dur + SUBGHZ_RMT_MAX_DURATION_TICKS - 1U) / SUBGHZ_RMT_MAX_DURATION_TICKS;
    }
    if (chunk_count == 0) {
        subghz_set_last_error("no valid durations");
        subghz_hw_stop();
        return false;
    }

    size_t symbol_capacity = (chunk_count + 1U) / 2U;
    symbols = heap_caps_malloc(symbol_capacity * sizeof(rmt_symbol_word_t), MALLOC_CAP_DMA);
    if (!symbols) {
        subghz_set_last_error("rmt symbol alloc failed");
        subghz_hw_stop();
        return false;
    }
    memset(symbols, 0, symbol_capacity * sizeof(rmt_symbol_word_t));

    size_t symbol_count = 0;
    bool fill_first = true;
    for (size_t i = 0; i < count; i++) {
        bool level = durations[i] > 0;
        uint32_t remaining = (uint32_t)llabs((long long)durations[i]);
        while (remaining > 0) {
            uint32_t chunk = remaining;
            if (chunk > SUBGHZ_RMT_MAX_DURATION_TICKS) chunk = SUBGHZ_RMT_MAX_DURATION_TICKS;
            if (fill_first) {
                symbols[symbol_count].level0 = level;
                symbols[symbol_count].duration0 = chunk;
                fill_first = false;
            } else {
                symbols[symbol_count].level1 = level;
                symbols[symbol_count].duration1 = chunk;
                symbol_count++;
                fill_first = true;
            }
            remaining -= chunk;
        }
    }
    if (!fill_first) {
        symbols[symbol_count].level1 = symbols[symbol_count].level0;
        symbols[symbol_count].duration1 = 0;
        symbol_count++;
    }

    size_t hw_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
    if ((hw_symbols % 2U) != 0U) hw_symbols++;

    rmt_tx_channel_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = (gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN,
        .mem_block_symbols = hw_symbols,
        .resolution_hz = SUBGHZ_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
        .flags = {.with_dma = true, .invert_out = false},
    };
    err = rmt_new_tx_channel(&rmt_cfg, &tx_chan);
    if (err == ESP_OK) err = rmt_enable(tx_chan);
    if (err == ESP_OK) err = rmt_new_copy_encoder(&(rmt_copy_encoder_config_t){}, &copy_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT setup failed: %s", esp_err_to_name(err));
        subghz_set_last_error("rmt setup failed");
        if (copy_encoder) rmt_del_encoder(copy_encoder);
        if (tx_chan) { rmt_disable(tx_chan); rmt_del_channel(tx_chan); }
        heap_caps_free(symbols);
        subghz_hw_stop();
        return false;
    }

    ESP_LOGI(TAG, "TX: freq=%u Hz, preset=%s, %lu durations",
             (unsigned)frequency_hz,
             (preset == SUBGHZ_PRESET_OOK270_ASYNC) ? "OOK270" : "OOK650",
             (unsigned long)count);

    err = cc1101_strobe(CC1101_STROBE_STX);
    if (err != ESP_OK) {
        subghz_set_last_error("stx failed");
        goto tx_cleanup;
    }

    {
        bool tx_ready = false;
        for (int retry = 0; retry < 200; retry++) {
            uint8_t state = 0;
            if (cc1101_get_state(&state) == ESP_OK && state == 0x02) {
                tx_ready = true;
                break;
            }
            ets_delay_us(100);
        }
        if (!tx_ready) {
            uint8_t state = 0;
            cc1101_get_state(&state);
            ESP_LOGE(TAG, "TX failed: STATE=0x%02X (expected 0x02 TX)", state);
            subghz_set_last_error("cc1101 failed to enter TX");
            goto tx_cleanup;
        }
    }

    err = rmt_transmit(
        tx_chan, copy_encoder, symbols,
        symbol_count * sizeof(rmt_symbol_word_t),
        &(rmt_transmit_config_t){.loop_count = 0});
    if (err == ESP_OK) err = rmt_tx_wait_all_done(tx_chan, -1);

tx_cleanup:
    gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, 0);
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    gpio_set_direction((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_MODE_INPUT);

    if (copy_encoder) rmt_del_encoder(copy_encoder);
    if (tx_chan) { rmt_disable(tx_chan); rmt_del_channel(tx_chan); }
    heap_caps_free(symbols);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX failed: %s", esp_err_to_name(err));
        subghz_set_last_error("tx failed");
        subghz_hw_stop();
        toast_show("SubGHz TX failed", TOAST_ERROR);
        return false;
    }
    subghz_hw_stop();
    subghz_set_last_error("none");
    ESP_LOGI(TAG, "TX complete");
    toast_show("SubGHz TX complete", TOAST_SUCCESS);
    ghostchi_manager_add_xp(5);
    return true;
}

void subghz_remote_manager_register_stream_handler(void) {
    (void)esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_SUBGHZ, subghz_stream_rx_cb, NULL);
}

void subghz_remote_manager_set_raw_capture_enabled(bool enabled) {
    if (enabled && !s_raw_capture_enabled) {
        s_raw_capture_enabled = true;
        s_raw_active = false;
        s_raw_ready = false;
        s_raw_worklen = 0;
        s_raw_capture_pending = false;
        gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_ANYEDGE);
        ESP_LOGI(TAG, "raw capture enabled (GDO0 intr ANYEDGE)");
    } else if (!enabled && s_raw_capture_enabled) {
        gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_DISABLE);
        if (s_raw_timeout_timer) {
            esp_timer_stop(s_raw_timeout_timer);
        }

        if (s_raw_active) {
            subghz_finalize_raw_capture(true);
            ESP_LOGI(TAG,
                     "raw capture flush on disable: %lu transitions ready=%d",
                     (unsigned long)s_raw_completed_len,
                     s_raw_ready ? 1 : 0);
        }
        if (s_raw_ready) {
            subghz_prepare_raw_capture_for_stream();
            subghz_stream_raw_capture();
        }

        s_raw_capture_enabled = false;
        s_capture_raw_mode_active = false;
        s_raw_active = false;
        s_raw_ready = false;
        s_raw_worklen = 0;
        ESP_LOGI(TAG, "raw capture disabled (GDO0 intr DISABLE)");
    }
}

void subghz_remote_manager_cycle_frequency(void) {
    uint8_t next_idx = (s_current_freq_idx + 1) % SUBGHZ_FREQ_COUNT;
    if (subghz_retune_frequency(s_scan_freqs[next_idx]) == ESP_OK) {
        s_current_freq_idx = next_idx;
        ESP_LOGI(TAG, "cycle freq: %s", s_scan_freq_labels[next_idx]);
    }
}

bool subghz_remote_manager_set_frequency_hz(uint32_t frequency_hz) {
    int freq_idx = subghz_frequency_to_index(frequency_hz);
    if (freq_idx < 0) {
        subghz_set_last_error("unsupported frequency");
        return false;
    }

    if (subghz_retune_frequency(frequency_hz) != ESP_OK) {
        return false;
    }
    s_current_freq_idx = (uint8_t)freq_idx;
    ESP_LOGI(TAG, "set freq: %s", s_scan_freq_labels[freq_idx]);
    return true;
}

const char *subghz_remote_manager_get_frequency_label(void) {
    return s_scan_freq_labels[s_current_freq_idx];
}

uint32_t subghz_remote_manager_get_frequency_hz(void) {
    return s_current_freq_hz;
}

bool subghz_remote_manager_capture_snapshot(const char *name_hint) {
    char safe_name[SUBGHZ_SNAPSHOT_NAME_MAX];
    subghz_sanitize_snapshot_name(name_hint, safe_name, sizeof(safe_name));

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    memcpy(s_snapshot_levels, s_levels, sizeof(s_snapshot_levels));
    s_snapshot_cursor = s_next_channel;
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }

    s_snapshot_valid = true;
    snprintf(s_active_snapshot_name, sizeof(s_active_snapshot_name), "%s", safe_name);
    subghz_set_last_error("none");
    return true;
}

bool subghz_remote_manager_save_snapshot(const char *name_hint, char *out_path, size_t out_path_len) {
    if (!s_snapshot_valid) {
        if (!subghz_remote_manager_capture_snapshot(name_hint)) {
            return false;
        }
    }

    bool display_was_suspended = false;
    bool did_mount = subghz_sd_begin(&display_was_suspended);
    if (!did_mount) {
        return false;
    }

    if (!subghz_ensure_snapshot_dir()) {
        subghz_sd_end(display_was_suspended);
        return false;
    }

    char safe_name[SUBGHZ_SNAPSHOT_NAME_MAX];
    if (name_hint && name_hint[0] != '\0') {
        subghz_sanitize_snapshot_name(name_hint, safe_name, sizeof(safe_name));
    } else {
        subghz_sanitize_snapshot_name(s_active_snapshot_name, safe_name, sizeof(safe_name));
    }

    char file_path[192];
    subghz_build_snapshot_path(safe_name, file_path, sizeof(file_path));

    FILE *f = fopen(file_path, "w");
    if (!f) {
        subghz_set_last_error("snapshot write open failed");
        subghz_sd_end(display_was_suspended);
        return false;
    }

    fprintf(f, "ghostesp_subghz_snapshot=1\n");
    fprintf(f, "name=%s\n", safe_name);
    fprintf(f, "base_mhz=%d\n", CONFIG_SUBGHZ_BASE_FREQ_MHZ);
    fprintf(f, "step_khz=%d\n", CONFIG_SUBGHZ_CHANNEL_STEP_KHZ);
    fprintf(f, "cursor=%u\n", (unsigned)s_snapshot_cursor);
    fputs("levels=", f);
    for (int i = 0; i < SUBGHZ_SCANNER_CHANNEL_COUNT; i++) {
        fprintf(f, "%u", (unsigned)s_snapshot_levels[i]);
        if (i + 1 < SUBGHZ_SCANNER_CHANNEL_COUNT) {
            fputc(',', f);
        }
    }
    fputc('\n', f);
    fclose(f);
    subghz_sd_end(display_was_suspended);

    snprintf(s_active_snapshot_name, sizeof(s_active_snapshot_name), "%s", safe_name);
    subghz_set_last_error("none");

    if (out_path && out_path_len > 0) {
        snprintf(out_path, out_path_len, "%s", file_path);
    }
    return true;
}

bool subghz_remote_manager_load_snapshot(const char *name_or_path) {
    if (!name_or_path || name_or_path[0] == '\0') {
        subghz_set_last_error("snapshot name required");
        return false;
    }

    char file_path[192];
    if (strcmp(name_or_path, "last") == 0) {
        if (strcmp(s_active_snapshot_name, "none") == 0) {
            subghz_set_last_error("no active snapshot");
            return false;
        }
        subghz_build_snapshot_path(s_active_snapshot_name, file_path, sizeof(file_path));
    } else if (strchr(name_or_path, '/') != NULL || strchr(name_or_path, '\\') != NULL) {
        snprintf(file_path, sizeof(file_path), "%s", name_or_path);
    } else {
        char safe_name[SUBGHZ_SNAPSHOT_NAME_MAX];
        subghz_sanitize_snapshot_name(name_or_path, safe_name, sizeof(safe_name));
        subghz_build_snapshot_path(safe_name, file_path, sizeof(file_path));
    }

    bool display_was_suspended = false;
    bool did_mount = subghz_sd_begin(&display_was_suspended);
    if (!did_mount) {
        return false;
    }

    FILE *f = fopen(file_path, "r");
    if (!f) {
        subghz_set_last_error("snapshot open failed");
        subghz_sd_end(display_was_suspended);
        return false;
    }

    char line[384];
    uint8_t loaded_levels[SUBGHZ_SCANNER_CHANNEL_COUNT] = {0};
    uint8_t loaded_cursor = 0;
    char loaded_name[SUBGHZ_SNAPSHOT_NAME_MAX] = {0};
    bool got_levels = false;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cursor=", 7) == 0) {
            int c = atoi(line + 7);
            if (c < 0) c = 0;
            if (c >= SUBGHZ_SCANNER_CHANNEL_COUNT) c = SUBGHZ_SCANNER_CHANNEL_COUNT - 1;
            loaded_cursor = (uint8_t)c;
        } else if (strncmp(line, "name=", 5) == 0) {
            char *name = line + 5;
            char *nl = strchr(name, '\n');
            if (nl) *nl = '\0';
            subghz_sanitize_snapshot_name(name, loaded_name, sizeof(loaded_name));
        } else if (strncmp(line, "levels=", 7) == 0) {
            char *csv = line + 7;
            char *nl = strchr(csv, '\n');
            if (nl) *nl = '\0';

            int idx = 0;
            char *saveptr = NULL;
            char *tok = strtok_r(csv, ",", &saveptr);
            while (tok && idx < SUBGHZ_SCANNER_CHANNEL_COUNT) {
                int v = atoi(tok);
                if (v < 0) v = 0;
                if (v > 100) v = 100;
                loaded_levels[idx++] = (uint8_t)v;
                tok = strtok_r(NULL, ",", &saveptr);
            }

            if (idx == SUBGHZ_SCANNER_CHANNEL_COUNT) {
                got_levels = true;
            }
        }
    }

    fclose(f);

    if (!got_levels) {
        subghz_set_last_error("snapshot parse failed");
        subghz_sd_end(display_was_suspended);
        return false;
    }

    if (s_data_mutex) {
        xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    }
    memcpy(s_levels, loaded_levels, sizeof(s_levels));
    s_next_channel = loaded_cursor;
    if (s_data_mutex) {
        xSemaphoreGive(s_data_mutex);
    }

    memcpy(s_snapshot_levels, loaded_levels, sizeof(s_snapshot_levels));
    s_snapshot_cursor = loaded_cursor;
    s_snapshot_valid = true;

    if (loaded_name[0] == '\0') {
        subghz_sanitize_snapshot_name(name_or_path, loaded_name, sizeof(loaded_name));
    }
    snprintf(s_active_snapshot_name, sizeof(s_active_snapshot_name), "%s", loaded_name);

    subghz_set_last_error("none");
    subghz_sd_end(display_was_suspended);
    return true;
}

int subghz_remote_manager_list_snapshots(char names[][SUBGHZ_SNAPSHOT_NAME_MAX], int max_names) {
    if (!names || max_names <= 0) {
        return 0;
    }

    bool display_was_suspended = false;
    bool did_mount = subghz_sd_begin(&display_was_suspended);
    if (!did_mount) {
        return 0;
    }

    DIR *dir = opendir(SUBGHZ_SNAPSHOT_DIR);
    if (!dir) {
        subghz_sd_end(display_was_suspended);
        return 0;
    }

    int count = 0;
    struct dirent *ent = NULL;
    while ((ent = readdir(dir)) != NULL) {
        const char *n = ent->d_name;
        if (!n || n[0] == '.') {
            continue;
        }

        size_t len = strlen(n);
        size_t ext_len = strlen(SUBGHZ_SNAPSHOT_EXT);
        if (len <= ext_len) {
            continue;
        }
        if (strcmp(n + (len - ext_len), SUBGHZ_SNAPSHOT_EXT) != 0) {
            continue;
        }

        if (count < max_names) {
            size_t copy_len = len - ext_len;
            if (copy_len >= SUBGHZ_SNAPSHOT_NAME_MAX) {
                copy_len = SUBGHZ_SNAPSHOT_NAME_MAX - 1;
            }
            memcpy(names[count], n, copy_len);
            names[count][copy_len] = '\0';
        }

        count++;
    }

    closedir(dir);
    subghz_sd_end(display_was_suspended);
    return count;
}

const char *subghz_remote_manager_get_active_snapshot_name(void) {
    return s_active_snapshot_name;
}

#else

bool subghz_remote_manager_start(bool stream_to_peer) {
    (void)stream_to_peer;
    return false;
}
bool subghz_remote_manager_start_waterfall(bool stream_to_peer) {
    (void)stream_to_peer;
    return false;
}

void subghz_remote_manager_stop(void) {}
void subghz_remote_manager_set_paused(bool paused) { (void)paused; }
bool subghz_remote_manager_is_running(void) { return false; }
bool subghz_remote_manager_is_paused(void) { return false; }
bool subghz_remote_manager_is_ready(void) { return false; }
bool subghz_remote_manager_begin_capture(bool raw_mode, uint32_t frequency_hz, bool stream_to_peer, uint32_t timeout_ms) {
    (void)raw_mode;
    (void)frequency_hz;
    (void)stream_to_peer;
    (void)timeout_ms;
    return false;
}
const char *subghz_remote_manager_get_last_error(void) { return "built without CONFIG_HAS_SUBGHZ"; }
bool subghz_remote_manager_get_levels(uint8_t *out_levels, size_t max_levels, uint8_t *out_cursor) {
    (void)out_levels;
    (void)max_levels;
    if (out_cursor) {
        *out_cursor = 0;
    }
    return false;
}
bool subghz_remote_manager_take_waterfall_line(uint8_t *out_levels, size_t max_levels, uint8_t *out_count, uint8_t *out_freq_idx, uint16_t *out_seq) {
    (void)out_levels;
    (void)max_levels;
    if (out_count) {
        *out_count = 0;
    }
    if (out_freq_idx) {
        *out_freq_idx = 0;
    }
    if (out_seq) {
        *out_seq = 0;
    }
    return false;
}
bool subghz_remote_manager_take_raw_capture(int32_t *out_durations, size_t max_durations, size_t *out_count) {
    (void)out_durations;
    (void)max_durations;
    if (out_count) {
        *out_count = 0;
    }
    return false;
}
bool subghz_remote_manager_take_decode_result(subghz_decoded_signal_t *out_result) {
    (void)out_result;
    return false;
}
bool subghz_remote_manager_transmit_raw(const int32_t *durations, size_t count, uint32_t frequency_hz, subghz_preset_t preset) {
    (void)durations;
    (void)count;
    (void)frequency_hz;
    (void)preset;
    return false;
}
void subghz_remote_manager_register_stream_handler(void) {}
void subghz_remote_manager_set_raw_capture_enabled(bool enabled) { (void)enabled; }
void subghz_remote_manager_cycle_frequency(void) {}
bool subghz_remote_manager_set_frequency_hz(uint32_t frequency_hz) {
    (void)frequency_hz;
    return false;
}
const char *subghz_remote_manager_get_frequency_label(void) { return "N/A"; }
uint32_t subghz_remote_manager_get_frequency_hz(void) { return 0; }
bool subghz_remote_manager_capture_snapshot(const char *name_hint) {
    (void)name_hint;
    return false;
}
bool subghz_remote_manager_save_snapshot(const char *name_hint, char *out_path, size_t out_path_len) {
    (void)name_hint;
    if (out_path && out_path_len > 0) {
        out_path[0] = '\0';
    }
    return false;
}
bool subghz_remote_manager_load_snapshot(const char *name_or_path) {
    (void)name_or_path;
    return false;
}
int subghz_remote_manager_list_snapshots(char names[][SUBGHZ_SNAPSHOT_NAME_MAX], int max_names) {
    (void)names;
    (void)max_names;
    return 0;
}
const char *subghz_remote_manager_get_active_snapshot_name(void) {
    return "none";
}

#endif
