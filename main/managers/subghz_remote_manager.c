#include "managers/subghz_remote_manager.h"
#include "managers/subghz_decoders.h"
#include "managers/ghostchi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_SUBGHZ

#include "core/esp_comm_manager.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "managers/sd_card_manager.h"
#include "rom/ets_sys.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI)
#include "lvgl_tft/disp_spi.h"
#include "lvgl_helpers.h"
#include "lvgl_spi_conf.h"
#include "managers/display_manager.h"
#endif

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
#define SUBGHZ_LOCAL_FRAME_GAP_US 30000U
#define SUBGHZ_RAW_CHUNK_MAX_DURATIONS 1024
#define SUBGHZ_RAW_PREBUF_SIZE 5U
#define SUBGHZ_RAW_NOISE_THRESHOLD_US 50U
#define SUBGHZ_RAW_SETTLE_US 0U
#define SUBGHZ_RCSWITCH_SEPARATION_US 4300U
#define SUBGHZ_RCSWITCH_REPEAT_TOLERANCE_US 200U
/* GDO0 glitch filter for the GPIO-ISR local capture path. Probe histograms
 * on this board show noise intervals of 2-9us (EMI/AGC chatter) while the
 * shortest real OOK pulse is ~95us; 20us sits cleanly between. The RMT
 * engine gets the same rejection in hardware via filter_ticks_thresh=3us. */
#define SUBGHZ_GDO0_GLITCH_US 20U
#define SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES 157U
#define SUBGHZ_LOCAL_MIN_EDGES_FOR_ACCUMULATE 8
#define SUBGHZ_LOCAL_SIGNAL_CHUNK_MIN_DURATIONS 80U
#define SUBGHZ_TEMBED_SPI_CLOCK_HZ 2000000

#if CONFIG_FREERTOS_UNICORE
#define SUBGHZ_DECODER_TASK_CORE tskNO_AFFINITY
#define SUBGHZ_DECODER_TASK_PRIORITY 13
#else
#define SUBGHZ_DECODER_TASK_CORE 1
#define SUBGHZ_DECODER_TASK_PRIORITY 15
#endif

static esp_err_t cc1101_strobe(uint8_t strobe_cmd);

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
#define CC1101_REG_ADDR     0x09
#define CC1101_REG_CHANNR   0x0A
#define CC1101_REG_FSCTRL1  0x0B
#define CC1101_REG_FSCTRL0  0x0C
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
#define CC1101_REG_FSTEST   0x29
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

#define CC1101_REG_TABLE_END 0xFF

/* Register values for the OOK async presets follow the widely used public
 * CC1101 community configurations (Flipper-style OOK270/OOK650 presets and
 * the T-Embed CC1101 board bring-up flow). Thanks to the respective authors
 * of those open-source reference implementations. */
static const cc1101_reg_entry_t s_preset_ook270[] = {
    {CC1101_REG_IOCFG0,   0x0D},
    {CC1101_REG_IOCFG2,   0x0D},
    {CC1101_REG_FIFOTHR,  0x07},
    {CC1101_REG_PKTLEN,   0x00},
    {CC1101_REG_PKTCTRL1, 0x04},
    {CC1101_REG_PKTCTRL0, 0x32},
    {CC1101_REG_ADDR,     0x00},
    {CC1101_REG_CHANNR,   0x00},
    {CC1101_REG_FSCTRL1,  0x06},
    {CC1101_REG_MDMCFG0,  0x00},
    {CC1101_REG_MDMCFG1,  0x00},
    {CC1101_REG_MDMCFG2,  0x30},
    {CC1101_REG_MDMCFG3,  0x32},
    {CC1101_REG_MDMCFG4,  0x67},
    {CC1101_REG_DEVIATN,  0x47},
    {CC1101_REG_MCSM0,    0x18},
    {CC1101_REG_FOCCFG,   0x18},
    {CC1101_REG_BSCFG,    0x1C},
    {CC1101_REG_AGCCTRL0, 0x40},
    {CC1101_REG_AGCCTRL1, 0x01},
    {CC1101_REG_AGCCTRL2, 0xC7},
    {CC1101_REG_FREND0,   0x11},
    {CC1101_REG_FREND1,   0xB6},
    {CC1101_REG_FSCAL3,   0xE9},
    {CC1101_REG_FSCAL2,   0x2A},
    {CC1101_REG_FSCAL1,   0x00},
    {CC1101_REG_FSCAL0,   0x1F},
    {CC1101_REG_FSTEST,   0x59},
    {CC1101_REG_TEST2,    0x81},
    {CC1101_REG_TEST1,    0x35},
    {CC1101_REG_TEST0,    0x09},
    {CC1101_REG_TABLE_END, 0},
};

static const cc1101_reg_entry_t s_preset_ook650[] = {
    {CC1101_REG_IOCFG0,   0x0D},
    {CC1101_REG_IOCFG2,   0x0D},
    {CC1101_REG_FIFOTHR,  0x07},
    {CC1101_REG_PKTLEN,   0x00},
    {CC1101_REG_PKTCTRL1, 0x04},
    {CC1101_REG_PKTCTRL0, 0x32},
    {CC1101_REG_ADDR,     0x00},
    {CC1101_REG_CHANNR,   0x00},
    {CC1101_REG_FSCTRL1,  0x06},
    {CC1101_REG_MDMCFG0,  0x00},
    {CC1101_REG_MDMCFG1,  0x00},
    {CC1101_REG_MDMCFG2,  0x30},
    {CC1101_REG_MDMCFG3,  0x32},
    {CC1101_REG_MDMCFG4,  0x17},
    {CC1101_REG_DEVIATN,  0x47},
    {CC1101_REG_MCSM0,    0x18},
    {CC1101_REG_FOCCFG,   0x18},
    {CC1101_REG_BSCFG,    0x1C},
    {CC1101_REG_AGCCTRL0, 0x40},
    {CC1101_REG_AGCCTRL1, 0x01},
    {CC1101_REG_AGCCTRL2, 0xC7},
    {CC1101_REG_FREND0,   0x11},
    {CC1101_REG_FREND1,   0xB6},
    {CC1101_REG_FSCAL3,   0xE9},
    {CC1101_REG_FSCAL2,   0x2A},
    {CC1101_REG_FSCAL1,   0x00},
    {CC1101_REG_FSCAL0,   0x1F},
    {CC1101_REG_FSTEST,   0x59},
    {CC1101_REG_TEST2,    0x81},
    {CC1101_REG_TEST1,    0x35},
    {CC1101_REG_TEST0,    0x09},
    {CC1101_REG_TABLE_END, 0},
};



static const uint8_t s_ook_patable_315[8] = {0x00, 0xC2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t s_ook_patable_433[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t s_ook_patable_868[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
static const uint8_t s_ook_patable_915[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t *subghz_patable_for_freq(uint32_t freq_hz) {
    if (freq_hz >= 300000000U && freq_hz <= 348000000U) return s_ook_patable_315;
    if (freq_hz >= 378000000U && freq_hz <= 464000000U) return s_ook_patable_433;
    if (freq_hz >= 779000000U && freq_hz <= 900000000U) return s_ook_patable_868;
    if (freq_hz >= 900000000U && freq_hz <= 928000000U) return s_ook_patable_915;
    return s_ook_patable_433;
}

#define CC1101_STATUS_PARTNUM 0x30
#define CC1101_STATUS_VERSION 0x31
#define CC1101_STATUS_RSSI    0x34
#define CC1101_STATUS_MARCSTATE 0x35

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

typedef enum {
    SUBGHZ_BUS_STANDALONE = 0,
    SUBGHZ_BUS_SHARED_DISPLAY,
    SUBGHZ_BUS_SHARED_SDCARD,
    SUBGHZ_BUS_SHARED_NRF24,
} subghz_bus_mode_t;

static spi_device_handle_t s_spi_dev = NULL;
static spi_host_device_t s_spi_host = SPI3_HOST;
static bool s_spi_manual_csn = false;
static bool s_spi_bus_initialized_by_us = false;
static subghz_bus_mode_t s_bus_mode = SUBGHZ_BUS_STANDALONE;
static bool s_display_spi_suspended = false;
static uint8_t s_display_spi_hold_depth = 0;
static bool s_cc1101_bus_acquired = false;
static bool s_gpio_isr_service_installed = false;
#ifdef CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_capture_pm_lock = NULL;
/* IDF PM locks are reference-counted; mirror the pairing here so the
 * session-wide hold (hw_start..hw_stop) and the capture-level hold nest. */
static int s_capture_pm_lock_depth = 0;
#endif

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
static volatile uint32_t s_raw_ignore_until_us = 0;
static volatile int32_t s_raw_workbufs[2][SUBGHZ_RAW_CHUNK_MAX_DURATIONS];
static volatile uint8_t s_raw_isr_buf_idx = 0;
static volatile size_t s_raw_worklen = 0;
static volatile size_t s_raw_completed_len = 0;
static volatile int s_raw_prev_level = 0;
static volatile int s_raw_capture_gpio = -1;
static volatile bool s_raw_timeout_poll_mode = false;
static volatile bool s_raw_waiting_first_edge = false;
static volatile bool s_tembed_local_capture = false;
static volatile uint32_t s_raw_prebuf[SUBGHZ_RAW_PREBUF_SIZE] = {0};
static volatile uint32_t s_raw_prebuf_sum = 0;
static volatile bool s_raw_noise_gate_open = false;
static bool s_raw_preview_logged = false;
static volatile int32_t *s_raw_stream_ptr = NULL;
static size_t s_raw_stream_count = 0;
static bool s_raw_capture_pending = false;
static bool s_raw_local_signal_seen = false;
static int32_t s_shared_buf[SUBGHZ_RAW_MAX_DURATIONS];

/* T-Embed capture engine: 1 = RMT hardware sampler, 0 = GPIO ISR + local
 * RCSwitch accumulation. RMT on this board loses ~75% of mid-frame symbols
 * (diag: GDO0 pin keeps toggling per the GPIO tap while the RMT reports long
 * flats -- threshold/copy path stalls under the WiFi+LVGL ISR environment).
 * The GPIO ISR path captured continuous streams reliably once the status-bar
 * EMI bursts were eliminated, so it is the default again. */
#define SUBGHZ_TEMBED_USE_RMT_RX 0

/* CONFIRMED A/B (2026-08-06 log): pausing LVGL status-bar updates during
 * capture made the receiver go deaf ~15ms after arm (pin flat, rc_changes
 * frozen). With the display active (0), capture receives continuously for
 * 10s+ incl. "arm first, press remote later". The diag probe and known-good
 * receiver builds both run display-active. Keep this 0: the display pause is
 * what killed reception.
 * The remaining frame-integrity problem (noise edges, false gap chops) is
 * handled by the glitch filter below, not by pausing the display. */
#define SUBGHZ_TEMBED_PAUSE_DISPLAY_DURING_CAPTURE 0

static rmt_channel_handle_t s_rmt_rx_chan = NULL;
static QueueHandle_t s_rmt_rx_queue = NULL;
static rmt_symbol_word_t *s_rmt_rx_buf = NULL;
#define SUBGHZ_RMT_RX_BUF_SYMBOLS 512U
static bool s_rmt_rx_active = false;
static volatile bool s_rmt_rx_queue_overflow = false;
static int64_t s_rmt_rx_last_event_us = 0;
/* Diagnostic: counts GDO0 edges seen by the GPIO module while the RMT owns
 * capture. The GPIO input path feeds both the matrix (RMT tap) and the GPIO
 * interrupt logic independently, so this tells us whether edges reach the
 * pin at all when the RMT reports silence. */
static volatile uint32_t s_rmt_diag_pin_edges = 0;
static uint32_t s_rmt_diag_last_edges = 0;
static int64_t s_capture_diag_last_us = 0;
/* Capture-path edge accounting for the 1Hz diag: every ISR invocation,
 * intervals accepted into the RCSwitch accumulator, and glitch excursions
 * absorbed by the SUBGHZ_GDO0_GLITCH_US filter. */
static volatile uint32_t s_capture_isr_edges = 0;
static volatile uint32_t s_capture_isr_accepted = 0;
static volatile uint32_t s_capture_isr_glitches = 0;
static uint32_t s_capture_diag_last_isr_edges = 0;
static uint32_t s_capture_diag_last_pin_edges = 0;
/* Glitch-rewind bookkeeping: time of the edge before the last accepted one,
 * so the interval recorded for a spike's leading edge can be popped and the
 * surrounding same-level segments merged when the trailing edge lands. */
static volatile uint32_t s_local_prev_accept_time_us = 0;
static volatile bool s_local_prev_accept_valid = false;
static volatile bool s_local_prev_accept_was_gap = false;
/* Runtime RX engine select for T-Embed local capture (compile-time default
 * below, overridable via `subghz rxmode`; applies on next capture arm). */
static bool s_tembed_rx_use_rmt = (SUBGHZ_TEMBED_USE_RMT_RX != 0);

static size_t s_rx_stream_expected = 0;
static size_t s_rx_stream_received = 0;
static uint32_t s_rx_stream_freq_hz = 0;
static subghz_preset_t s_rx_stream_preset = SUBGHZ_PRESET_OOK270_ASYNC;
static subghz_decoder_engine_t s_decoder_engine;
static volatile bool s_decode_result_ready = false;
static subghz_decoded_signal_t s_local_decode_result;
static volatile bool s_local_decode_result_ready = false;
static volatile int32_t s_local_rc_timings[SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES];
static volatile int32_t s_local_rc_pending_timings[SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES];
static int32_t s_local_rc_decode_work[SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES];
static volatile size_t s_local_rc_change_count = 0;
static volatile size_t s_local_rc_pending_count = 0;
static volatile uint8_t s_local_rc_repeat_count = 0;
static volatile bool s_local_rc_decode_pending = false;
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

static void subghz_capture_pm_lock_acquire(void) {
#ifdef CONFIG_PM_ENABLE
    if (!s_capture_pm_lock) {
        esp_err_t err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "subghz_rx", &s_capture_pm_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SubGHz PM lock create failed: %s", esp_err_to_name(err));
            return;
        }
    }
    esp_err_t err = esp_pm_lock_acquire(s_capture_pm_lock);
    if (err == ESP_OK) {
        s_capture_pm_lock_depth++;
    } else {
        ESP_LOGW(TAG, "SubGHz PM lock acquire failed: %s", esp_err_to_name(err));
    }
#endif
}

static void subghz_capture_pm_lock_release(void) {
#ifdef CONFIG_PM_ENABLE
    if (s_capture_pm_lock && s_capture_pm_lock_depth > 0) {
        esp_err_t err = esp_pm_lock_release(s_capture_pm_lock);
        if (err == ESP_OK) {
            s_capture_pm_lock_depth--;
        } else {
            ESP_LOGW(TAG, "SubGHz PM lock release failed: %s", esp_err_to_name(err));
        }
    }
#endif
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
static esp_err_t cc1101_read_reg(uint8_t reg, uint8_t *value);
static esp_err_t cc1101_read_status(uint8_t status_reg, uint8_t *value);
static esp_err_t cc1101_strobe(uint8_t strobe_cmd);
static esp_err_t cc1101_reset(void);
static esp_err_t cc1101_wait_for_state(uint8_t expected_state, uint32_t timeout_us, uint8_t *last_state);
static esp_err_t cc1101_write_patable(const uint8_t *data, size_t len);
static esp_err_t subghz_apply_preset(subghz_preset_t preset);
static esp_err_t subghz_retune_frequency(uint32_t freq_hz);
static int subghz_frequency_to_index(uint32_t frequency_hz);
static bool subghz_is_tembed_c1101(void);
static void subghz_prepare_tembed_board_pins(void);
static esp_err_t subghz_apply_tembed_freq_calibration(uint32_t frequency_hz);
static esp_err_t subghz_apply_tembed_local_rx_settings(void);
static esp_err_t subghz_verify_cc1101_register_readback(uint32_t frequency_hz, const char *phase);
static void subghz_apply_board_rf_switch(uint32_t frequency_hz);
static void subghz_display_spi_hold_begin(void);
static void subghz_display_spi_hold_end(void);
static void subghz_display_status_updates_set_enabled(bool enabled);
static void subghz_force_inactive_shared_spi_cs_high(void);
static esp_err_t cc1101_select_wait_ready(void);
static bool subghz_should_watch_gdo2_capture(void);
static esp_err_t subghz_configure_capture_gdo_outputs(void);
static uint32_t subghz_raw_timeout_interval_us(void);
static uint32_t subghz_min_valid_edge_us(void);
static void subghz_log_raw_preview(const int32_t *durations, size_t count, int gpio_pin);
static void subghz_reset_capture_buffers(void);
static void subghz_finalize_raw_capture(bool allow_short_capture);
static void subghz_reset_local_rcswitch_state(void);
static void subghz_poll_local_rcswitch_decode(void);
static void subghz_poll_raw_timeout(void);
static void subghz_prepare_raw_capture_for_stream(void);
static void subghz_try_local_batch_decode(void);
static void subghz_finish_capture_request(uint32_t request_id, bool ok, const char *reason);
static void subghz_process_capture_request(void);
static bool subghz_rmt_rx_start(void);
static void subghz_rmt_rx_stop(void);
static void subghz_rmt_rx_rearm(void);
static void subghz_rmt_poll_local_decode(void);

static bool IRAM_ATTR subghz_rmt_rx_cb(rmt_channel_handle_t chan,
                                        const rmt_rx_done_event_data_t *edata,
                                        void *user_ctx)
{
    BaseType_t hpw = pdFALSE;
    /* Queue EVERY done event, including 0-symbol idle-timeout completions on a
     * quiet line. Dropping those here meant the poll never rearmed and the
     * receiver chain died permanently the first time the line stayed flat for
     * 30ms -- which is exactly the "capture only works if the remote is held
     * during arm" failure. */
    if (s_rmt_rx_queue && edata) {
        if (xQueueSendFromISR(s_rmt_rx_queue, edata, &hpw) != pdTRUE) {
            s_rmt_rx_queue_overflow = true;
        }
    }
    return hpw == pdTRUE;
}

static bool subghz_rmt_rx_start(void)
{
    if (s_rmt_rx_active) {
        return true;
    }

    if (!s_rmt_rx_buf) {
        s_rmt_rx_buf = (rmt_symbol_word_t *)heap_caps_malloc(
            SUBGHZ_RMT_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
            MALLOC_CAP_8BIT);
        if (!s_rmt_rx_buf) {
            ESP_LOGE(TAG, "RMT RX: buffer alloc failed");
            return false;
        }
    }
    if (!s_rmt_rx_queue) {
        s_rmt_rx_queue = xQueueCreate(4, sizeof(rmt_rx_done_event_data_t));
        if (!s_rmt_rx_queue) {
            ESP_LOGE(TAG, "RMT RX: queue create failed");
            return false;
        }
    }

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = (gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = SUBGHZ_RMT_RESOLUTION_HZ,
        .intr_priority = 0,
        .flags = {.invert_in = false, .with_dma = false, .allow_pd = false},
    };

    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &s_rmt_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX channel create failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Tap GDO0 with the GPIO interrupt as a diagnostic edge counter. This
     * does not disturb the RMT matrix input -- both read the pin
     * independently. */
    s_rmt_diag_pin_edges = 0;
    s_rmt_diag_last_edges = 0;
    gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_ANYEDGE);
    gpio_intr_enable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = subghz_rmt_rx_cb,
    };
    err = rmt_rx_register_event_callbacks(s_rmt_rx_chan, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX callback register failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_rmt_rx_chan);
        s_rmt_rx_chan = NULL;
        return false;
    }

    err = rmt_enable(s_rmt_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX enable failed: %s", esp_err_to_name(err));
        rmt_del_channel(s_rmt_rx_chan);
        s_rmt_rx_chan = NULL;
        return false;
    }

    s_rmt_rx_active = true;
    subghz_rmt_rx_rearm();

    uint8_t marc = 0;
    if (cc1101_read_status(CC1101_STATUS_MARCSTATE, &marc) == ESP_OK) {
        ESP_LOGI(TAG, "RMT RX started (glitch filter 3us, idle timeout 30ms) MARCSTATE=0x%02X", marc);
    } else {
        ESP_LOGI(TAG, "RMT RX started (glitch filter 3us, idle timeout 30ms)");
    }
    return true;
}

static void subghz_rmt_rx_rearm(void)
{
    if (!s_rmt_rx_chan || !s_rmt_rx_active || !s_rmt_rx_buf) {
        return;
    }
    rmt_receive_config_t rcfg = {
        .signal_range_min_ns = 3000,
        .signal_range_max_ns = 30000000,
    };
    esp_err_t err = rmt_receive(s_rmt_rx_chan, s_rmt_rx_buf,
                                SUBGHZ_RMT_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t),
                                &rcfg);
    if (err == ESP_ERR_INVALID_STATE) {
        /* A receive is already active -- the watchdog probe found a healthy
         * channel, nothing to do. */
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RMT RX rearm failed: %s", esp_err_to_name(err));
    }
}

static void subghz_rmt_rx_stop(void)
{
    if (!s_rmt_rx_active) {
        return;
    }
    s_rmt_rx_active = false;
    gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_DISABLE);
    if (s_rmt_rx_chan) {
        rmt_disable(s_rmt_rx_chan);
        rmt_del_channel(s_rmt_rx_chan);
        s_rmt_rx_chan = NULL;
    }
    if (s_rmt_rx_queue) {
        rmt_rx_done_event_data_t dummy;
        while (xQueueReceive(s_rmt_rx_queue, &dummy, 0) == pdTRUE) {}
    }
    ESP_LOGI(TAG, "RMT RX stopped");
}

static void subghz_rmt_poll_local_decode(void)
{
    if (!s_rmt_rx_active || !s_rmt_rx_queue) {
        return;
    }
    if (s_local_decode_result_ready) {
        return;
    }

    bool got_event = false;
    rmt_rx_done_event_data_t evt;
    while (xQueueReceive(s_rmt_rx_queue, &evt, 0) == pdTRUE) {
        got_event = true;
        if (!evt.received_symbols || evt.num_symbols == 0) {
            subghz_rmt_rx_rearm();
            continue;
        }

        size_t nsym = evt.num_symbols;
        if (nsym > SUBGHZ_RMT_RX_BUF_SYMBOLS) {
            nsym = SUBGHZ_RMT_RX_BUF_SYMBOLS;
        }

        int32_t durations[SUBGHZ_RAW_MAX_DURATIONS];
        size_t count = 0;
        for (size_t i = 0; i < nsym && count < SUBGHZ_RAW_MAX_DURATIONS; i++) {
            const rmt_symbol_word_t *s = &evt.received_symbols[i];
            if (s->duration0 > 0 && count < SUBGHZ_RAW_MAX_DURATIONS) {
                durations[count++] = s->level0 ? (int32_t)s->duration0 : -(int32_t)s->duration0;
            }
            if (s->duration1 > 0 && count < SUBGHZ_RAW_MAX_DURATIONS) {
                durations[count++] = s->level1 ? (int32_t)s->duration1 : -(int32_t)s->duration1;
            }
        }

        subghz_rmt_rx_rearm();

        /* Dump every silicon-captured burst: this is the ground-truth pulse
         * train (3us hardware glitch filter, no ISR involvement), needed to
         * tell "demod fades mid-frame" from "transmission genuinely short". */
        if (count >= 2) {
            char dbuf[512];
            size_t dpos = 0;
            size_t head = count < 48 ? count : 48;
            dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos,
                             "RMT burst %lu timings (pin=%lu):",
                             (unsigned long)count, (unsigned long)s_rmt_diag_pin_edges);
            for (size_t i = 0; i < head && dpos + 12 < sizeof(dbuf); i++) {
                dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos, " %ld", (long)durations[i]);
            }
            if (count > head) dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos, " ...");
            ESP_LOGI(TAG, "%s", dbuf);
        }

        if (count < 6) {
            continue;
        }

        subghz_log_raw_preview(durations, count, CONFIG_SUBGHZ_GDO0_PIN);

        s_raw_capture_gpio = CONFIG_SUBGHZ_GDO0_PIN;
        s_raw_local_signal_seen = true;

        subghz_decoded_signal_t decoded;
        memset(&decoded, 0, sizeof(decoded));
        uint64_t keeloq_code = 0;
        int keeloq_bits = 0;

        if (subghz_decode_keeloq(durations, count,
                                 &keeloq_code, &keeloq_bits)) {
            decoded.decoded = true;
            decoded.code = keeloq_code;
            decoded.bits = keeloq_bits;
            decoded.te = 400;
            snprintf(decoded.protocol, sizeof(decoded.protocol), "KeeLoq");
            snprintf(decoded.info, sizeof(decoded.info),
                     "KeeLoq %dbit\nCode:0x%016llX",
                     keeloq_bits, (unsigned long long)keeloq_code);
        } else if (!subghz_decode_signal(durations, count, &decoded) || !decoded.decoded) {
            ESP_LOGI(TAG, "RMT RX: no decode from %lu durations", (unsigned long)count);

            if (count <= SUBGHZ_RAW_MAX_DURATIONS) {
                memcpy(s_shared_buf, durations, count * sizeof(int32_t));
                s_raw_stream_ptr = s_shared_buf;
                s_raw_stream_count = count;
                s_raw_capture_pending = true;
            }
            continue;
        }

        decoded.frequency_hz = (int)s_current_freq_hz;
        s_local_decode_result = decoded;
        s_local_decode_result_ready = true;
        ESP_LOGI(TAG, "RMT RX decode: %s %dbit from %lu durations",
                 s_local_decode_result.protocol,
                 s_local_decode_result.bits,
                 (unsigned long)count);

        if (count <= SUBGHZ_RAW_MAX_DURATIONS) {
            memcpy(s_shared_buf, durations, count * sizeof(int32_t));
            s_raw_stream_ptr = s_shared_buf;
            s_raw_stream_count = count;
            s_raw_capture_pending = true;
        }
    }

    /* Wedge detector: done events stopped for 250ms. If the pin is STILL
     * toggling (GPIO diag counter moved) or the queue overflowed, the receive
     * chain is broken -> force a rearm. If the pin is quiet we do nothing:
     * the pending receive is healthy and a blind rmt_receive() probe would
     * just spam the driver's "channel not in enable state" error. */
    int64_t now_us = esp_timer_get_time();
    if (got_event) {
        s_rmt_rx_last_event_us = now_us;
        s_rmt_diag_last_edges = s_rmt_diag_pin_edges;
    } else if ((now_us - s_rmt_rx_last_event_us) > 250000) {
        s_rmt_rx_last_event_us = now_us;
        bool overflow = s_rmt_rx_queue_overflow;
        s_rmt_rx_queue_overflow = false;
        uint32_t pin_edges = s_rmt_diag_pin_edges;
        bool pin_active = (pin_edges != s_rmt_diag_last_edges);
        s_rmt_diag_last_edges = pin_edges;
        if (overflow || pin_active) {
            ESP_LOGW(TAG,
                     "RMT RX: no done events 250ms but pin_edges=%lu (overflow=%d), forcing rearm",
                     (unsigned long)pin_edges, overflow ? 1 : 0);
            subghz_rmt_rx_rearm();
        }
    }
}

static esp_err_t subghz_apply_preset(subghz_preset_t preset) {
    const cc1101_reg_entry_t *tbl = NULL;
    tbl = (preset == SUBGHZ_PRESET_OOK270_ASYNC) ? s_preset_ook270 : s_preset_ook650;
    esp_err_t err = ESP_OK;
    for (int i = 0; tbl[i].reg != CC1101_REG_TABLE_END && err == ESP_OK; i++) {
        err = cc1101_write_reg(tbl[i].reg, tbl[i].val);
    }
    if (err == ESP_OK) {
        err = subghz_apply_tembed_freq_calibration(s_current_freq_hz);
    }
    if (err == ESP_OK) {
        err = cc1101_write_patable(subghz_patable_for_freq(s_current_freq_hz), 8);
    }
    return err;
}

static bool subghz_is_tembed_c1101(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0;
#else
    return false;
#endif
}

static void subghz_prepare_tembed_board_pins(void) {
    if (!subghz_is_tembed_c1101()) {
        return;
    }

    /* GPIO15 = PIN_POWER_ON: powers the CC1101 (board power-enable, driven
     * HIGH at boot and held). With CONFIG_PM_SLP_DISABLE_GPIO=y every GPIO output is
     * floated during light sleep, which would cut CC1101 power and wipe its
     * registers/state on every doze. gpio_sleep_sel_dis() keeps the pin driven
     * through light sleep; the session PM lock in subghz_hw_start() provides
     * the second layer of protection. */
    gpio_set_direction((gpio_num_t)15, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)15, 1);
    (void)gpio_sleep_sel_dis((gpio_num_t)15);

    /* After a cold power-up the CC1101 needs its regulator + POR + XOSC before
     * it answers SPI (CHIP_RDYn). The pin is never driven low again, so only
     * the first power application needs the settle delay. */
    static bool s_cc1101_power_settled = false;
    if (!s_cc1101_power_settled) {
        vTaskDelay(pdMS_TO_TICKS(10));
        s_cc1101_power_settled = true;
    }

    gpio_set_direction((gpio_num_t)44, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)44, 1);
    gpio_set_direction((gpio_num_t)43, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)43, 0);

    /* The shared SPI chip-select pins are initialized once at boot. Do not
     * reconfigure TFT/SD/NRF CS here: those pins may already be owned by
     * active ESP-IDF SPI devices, and touching TFT CS can stop panel flushes. */
    gpio_set_direction((gpio_num_t)12, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)12, 1);
    /* Keep CSN parked high through light sleep so the CC1101 is never
     * spuriously selected by shared-bus traffic around sleep transitions. */
    (void)gpio_sleep_sel_dis((gpio_num_t)12);
}

/* FSCTRL0 band interpolation: clamp, linear ratio, round. Band endpoints
 * follow the proven T-Embed CC1101 community calibration. */
static uint8_t subghz_interp_fsctrl0(uint32_t freq_hz, uint32_t min_hz, uint32_t max_hz,
                                     uint8_t min_val, uint8_t max_val) {
    if (freq_hz <= min_hz) return min_val;
    if (freq_hz >= max_hz) return max_val;
    const float ratio = (float)(freq_hz - min_hz) / (float)(max_hz - min_hz);
    return (uint8_t)((float)min_val + ratio * (float)(max_val - min_val) + 0.5f);
}

static esp_err_t subghz_apply_tembed_freq_calibration(uint32_t frequency_hz) {
    if (!subghz_is_tembed_c1101()) {
        return ESP_OK;
    }

    /* Matches the proven T-Embed CC1101 calibration sequence exactly
     * (endpoints per public CC1101 reference implementations for this board).
     * The previous tables used wrong endpoints for the 315/433 bands
     * (433.92MHz produced FSCTRL0=0x11 instead of the correct 0x23),
     * detuning the synthesizer and causing the fragmented/glitchy OOK edges
     * seen in captures. */
    uint8_t fsctrl0;
    bool high_vco;

    if (frequency_hz >= 280000000U && frequency_hz <= 348000000U) {
        fsctrl0 = subghz_interp_fsctrl0(frequency_hz, 280000000U, 348000000U, 24, 28);
        high_vco = frequency_hz >= 322880000U;
    } else if (frequency_hz >= 387000000U && frequency_hz <= 464000000U) {
        fsctrl0 = subghz_interp_fsctrl0(frequency_hz, 387000000U, 464000000U, 31, 38);
        high_vco = frequency_hz >= 430500000U;
    } else if (frequency_hz >= 779000000U && frequency_hz <= 899999999U) {
        fsctrl0 = subghz_interp_fsctrl0(frequency_hz, 779000000U, 899999999U, 65, 76);
        high_vco = frequency_hz >= 861000000U;
    } else if (frequency_hz >= 900000000U && frequency_hz <= 928000000U) {
        fsctrl0 = subghz_interp_fsctrl0(frequency_hz, 900000000U, 928000000U, 77, 79);
        high_vco = true;
    } else {
        return ESP_OK; /* out of band: leave defaults, like the reference flow */
    }

    const uint8_t test0 = high_vco ? 0x09 : 0x0B;

    esp_err_t err = cc1101_write_reg(CC1101_REG_FSCTRL0, fsctrl0);
    if (err == ESP_OK) {
        err = cc1101_write_reg(CC1101_REG_TEST0, test0);
    }
    /* SCAL recalibrates the synthesizer with the new FSCTRL0 and parks the
     * chip in IDLE; every caller either re-strobes SRX (retune) or runs this
     * before arming RX (init/capture/tx), matching the reference ordering. The
     * earlier removal of SCAL was based on MARCSTATE=0x00 reads that turned
     * out to be SPI read artifacts, not real SLEEP transitions. */
    if (err == ESP_OK) {
        err = cc1101_strobe(CC1101_STROBE_SCAL);
    }
    if (err == ESP_OK) {
        (void)cc1101_wait_for_state(0x01, 20000U, NULL); /* IDLE; 20ms per the reference flow */
    }
    if (err == ESP_OK && high_vco) {
        uint8_t fscal2 = 0;
        if (cc1101_read_reg(CC1101_REG_FSCAL2, &fscal2) == ESP_OK && fscal2 < 0x20) {
            (void)cc1101_write_reg(CC1101_REG_FSCAL2, (uint8_t)(fscal2 + 0x20));
            (void)cc1101_strobe(CC1101_STROBE_SCAL);
            (void)cc1101_wait_for_state(0x01, 20000U, NULL);
        }
    }
    if (err == ESP_OK && s_stream_to_peer) {
        err = cc1101_write_reg(CC1101_REG_AGCCTRL2, 0xC4);
    }
    if (err == ESP_OK && s_stream_to_peer) {
        err = cc1101_write_reg(CC1101_REG_AGCCTRL1, 0x09);
    }
    if (err == ESP_OK && s_stream_to_peer) {
        err = cc1101_write_reg(CC1101_REG_AGCCTRL0, 0x43);
    }
    return err;
}

static esp_err_t subghz_apply_tembed_local_rx_settings(void) {
    if (!subghz_is_tembed_c1101() || s_stream_to_peer) {
        return ESP_OK;
    }

    esp_err_t err = cc1101_write_reg(CC1101_REG_MDMCFG3, 0x32);
    if (err == ESP_OK) {
        err = cc1101_write_reg(CC1101_REG_MDMCFG4, 0x67);
    }
    if (err == ESP_OK) {
        err = cc1101_write_reg(CC1101_REG_MDMCFG2, 0x30);
    }
    if (err == ESP_OK && s_capture_raw_mode_active) {
        uint8_t mdmcfg2 = 0;
        if (cc1101_read_reg(CC1101_REG_MDMCFG2, &mdmcfg2) == ESP_OK) {
            err = cc1101_write_reg(CC1101_REG_MDMCFG2, (uint8_t)(mdmcfg2 | 0x80));
        }
    }
    return err;
}

static esp_err_t subghz_verify_cc1101_register_readback(uint32_t frequency_hz, const char *phase) {
    uint8_t f2 = 0, f1 = 0, f0 = 0;
    uint8_t mdmcfg4 = 0, mdmcfg3 = 0, mdmcfg2 = 0;
    uint8_t agc2 = 0, agc1 = 0, agc0 = 0;
    esp_err_t err = cc1101_read_reg(CC1101_REG_FREQ2, &f2);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_FREQ1, &f1);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_FREQ0, &f0);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_MDMCFG4, &mdmcfg4);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_MDMCFG3, &mdmcfg3);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_MDMCFG2, &mdmcfg2);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_AGCCTRL2, &agc2);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_AGCCTRL1, &agc1);
    if (err == ESP_OK) err = cc1101_read_reg(CC1101_REG_AGCCTRL0, &agc0);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t expected_word = (uint32_t)((((uint64_t)frequency_hz) * 65536ULL) / 26000000ULL);
    uint32_t readback_word = ((uint32_t)f2 << 16) | ((uint32_t)f1 << 8) | f0;
    bool modem_zero = (mdmcfg4 == 0 && mdmcfg3 == 0 && mdmcfg2 == 0 && agc2 == 0 && agc1 == 0 && agc0 == 0);
    if (readback_word != expected_word || modem_zero) {
        ESP_LOGW(TAG,
                 "CC1101 register verify failed (%s): freq expected=0x%06lX readback=0x%06lX modem=0x%02X/0x%02X/0x%02X agc=0x%02X/0x%02X/0x%02X",
                 phase ? phase : "?",
                 (unsigned long)expected_word,
                 (unsigned long)readback_word,
                 mdmcfg4,
                 mdmcfg3,
                 mdmcfg2,
                 agc2,
                 agc1,
                 agc0);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
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

static void subghz_apply_board_rf_switch(uint32_t frequency_hz) {
    /* The T-Embed CC1101 routes its antenna through a GPIO-controlled band
     * switch on SW1=GPIO47, SW0=GPIO48 (board-level truth table):
     *   SW1=1 SW0=0 -> 315MHz band
     *   SW1=1 SW0=1 -> 433MHz band
     *   SW1=0 SW0=1 -> 868/915MHz band
     * Leaving these pins floating detunes/disconnects the antenna path and
     * starves the receiver, so the band must be asserted on every retune. */
    if (!subghz_is_tembed_c1101()) {
        return;
    }

    int band = -1;
    uint8_t sw1 = 0;
    uint8_t sw0 = 0;
    if (frequency_hz <= 350000000UL) {
        band = 0; sw1 = 1; sw0 = 0;
    } else if (frequency_hz > 350000000UL && frequency_hz < 468000000UL) {
        band = 1; sw1 = 1; sw0 = 1;
    } else if (frequency_hz > 778000000UL) {
        band = 2; sw1 = 0; sw0 = 1;
    }
    if (band < 0) {
        return; /* Outside supported bands: leave the switch as-is. */
    }

    static bool s_rf_switch_gpio_ready = false;
    if (!s_rf_switch_gpio_ready) {
        gpio_config_t sw_cfg = {
            .pin_bit_mask = (1ULL << 47) | (1ULL << 48),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&sw_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "TEmbed RF switch GPIO47/48 config failed");
            return;
        }
        /* Keep driving the antenna switch if the rest of the system sleeps. */
        (void)gpio_sleep_sel_dis((gpio_num_t)47);
        (void)gpio_sleep_sel_dis((gpio_num_t)48);
        s_rf_switch_gpio_ready = true;
    }

    static uint8_t s_rf_switch_band = 0xFF;
    bool changed = (s_rf_switch_band != (uint8_t)band);
    gpio_set_level((gpio_num_t)47, sw1);
    gpio_set_level((gpio_num_t)48, sw0);
    s_rf_switch_band = (uint8_t)band;
    if (changed) {
        /* Settle time for the antenna switch, matching the reference flow. */
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "TEmbed RF switch band=%d (SW1=%d SW0=%d)", band, sw1, sw0);
    }
}

static subghz_preset_t subghz_capture_preset_for_request(bool raw_mode) {
    if (subghz_is_tembed_c1101()) {
        (void)raw_mode;
        return SUBGHZ_PRESET_OOK270_ASYNC;
    }

    return raw_mode ? SUBGHZ_PRESET_OOK650_ASYNC : SUBGHZ_PRESET_OOK270_ASYNC;
}

static bool subghz_should_watch_gdo2_capture(void) {
    (void)0;
    return false;
}

static esp_err_t subghz_configure_capture_gdo_outputs(void) {
    esp_err_t err = cc1101_write_reg(CC1101_REG_IOCFG0, 0x0D);
    if (err != ESP_OK) {
        return err;
    }

    if (subghz_is_tembed_c1101() || subghz_should_watch_gdo2_capture()) {
        err = cc1101_write_reg(CC1101_REG_IOCFG2, 0x0D);
    }

    return err;
}

static uint32_t subghz_raw_timeout_interval_us(void) {
    if (subghz_is_tembed_c1101() && !s_stream_to_peer && !s_capture_raw_mode_active) {
        return SUBGHZ_LOCAL_FRAME_GAP_US;
    }
    return SUBGHZ_RAW_TIMEOUT_US;
}

static uint32_t subghz_min_valid_edge_us(void) {
    /* Preserve every CHANGE edge, matching the classic RCSwitch receiver.
     * Dropping one edge merges opposite-level intervals and destroys decoder
     * polarity. */
    return 0U;
}

static bool subghz_local_chunk_has_signal(const int32_t *durations, size_t count, bool normal_mode) {
    if (!durations || count == 0 || s_stream_to_peer) {
        return true;
    }
    if (normal_mode) {
        /* Classic RCSwitch-style receive keeps the timings it sees and lets the
         * protocol decoder decide whether they are meaningful. */
        return true;
    }
    if (count < SUBGHZ_LOCAL_MIN_EDGES_FOR_ACCUMULATE && !(normal_mode && s_raw_local_signal_seen)) {
        return false;
    }
    if (count >= SUBGHZ_LOCAL_SIGNAL_CHUNK_MIN_DURATIONS) {
        return true;
    }

    subghz_decoded_signal_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    return subghz_decode_signal(durations, count, &decoded) && decoded.decoded;
}

static inline uint32_t IRAM_ATTR subghz_u32_abs_diff(uint32_t a, uint32_t b) {
    return (a > b) ? (a - b) : (b - a);
}

static inline uint32_t IRAM_ATTR subghz_i32_abs(int32_t value) {
    return value >= 0 ? (uint32_t)value : (uint32_t)(-(int64_t)value);
}

static void subghz_reset_local_rcswitch_state(void) {
    s_local_rc_change_count = 0;
    s_local_rc_pending_count = 0;
    s_local_rc_repeat_count = 0;
    s_local_rc_decode_pending = false;
    memset((void *)s_local_rc_timings, 0, sizeof(s_local_rc_timings));
    memset((void *)s_local_rc_pending_timings, 0, sizeof(s_local_rc_pending_timings));
}

static void IRAM_ATTR subghz_local_rcswitch_handle_duration(uint32_t duration) {
    if (duration > SUBGHZ_RCSWITCH_SEPARATION_US) {
        /* Publish on the FIRST gap-terminated chunk that looks like a frame.
         * The old two-matching-gaps rule assumed continuous remotes whose
         * inter-frame gaps are crystal-stable; bursty remotes (one code word
         * every ~500ms, gaps jittering by ms) never matched and starved the
         * decoder (2026-08-06 log: rc_changes stuck at 8 for 4s of held
         * remote). The downstream protocol decoders are far stricter noise
         * gates than gap matching ever was. */
        if (s_local_rc_change_count >= SUBGHZ_LOCAL_MIN_EDGES_FOR_ACCUMULATE &&
            !s_local_rc_decode_pending) {
            size_t copy = s_local_rc_change_count;
            if (copy > SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES) {
                copy = SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES;
            }
            for (size_t i = 0; i < copy; i++) {
                s_local_rc_pending_timings[i] = s_local_rc_timings[i];
            }
            s_local_rc_pending_count = copy;
            s_local_rc_decode_pending = true;
        }
        s_local_rc_change_count = 0;
        s_local_rc_repeat_count = 0;
    }

    if (s_local_rc_change_count >= SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES) {
        s_local_rc_change_count = 0;
        s_local_rc_repeat_count = 0;
    }
    s_local_rc_timings[s_local_rc_change_count++] = (int32_t)duration;
}

static void subghz_publish_local_rcswitch_frame(const int32_t *timings, size_t count) {
    if (!timings || count == 0) {
        return;
    }
    if (count > SUBGHZ_RAW_MAX_DURATIONS) {
        count = SUBGHZ_RAW_MAX_DURATIONS;
    }

    for (size_t i = 0; i < count; i++) {
        int32_t duration = (int32_t)subghz_i32_abs(timings[i]);
        s_shared_buf[i] = (i & 1U) ? -duration : duration;
    }
    s_raw_stream_ptr = s_shared_buf;
    s_raw_stream_count = count;
    s_raw_capture_pending = true;
    s_raw_local_signal_seen = true;
    s_raw_capture_gpio = CONFIG_SUBGHZ_GDO0_PIN;
    subghz_log_raw_preview(s_shared_buf, count, s_raw_capture_gpio);
}

static void subghz_poll_local_rcswitch_decode(void) {
    if (s_stream_to_peer || s_capture_raw_mode_active || s_local_decode_result_ready ||
        !s_local_rc_decode_pending || s_local_rc_pending_count == 0) {
        return;
    }

    size_t count = s_local_rc_pending_count;
    if (count > SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES) {
        count = SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES;
    }
    for (size_t i = 0; i < count; i++) {
        s_local_rc_decode_work[i] = s_local_rc_pending_timings[i];
    }
    s_local_rc_pending_count = 0;
    s_local_rc_decode_pending = false;
    subghz_publish_local_rcswitch_frame(s_local_rc_decode_work, count);

    {
        size_t head = count < 20 ? count : 20;
        char dbuf[512];
        size_t dpos = 0;
        dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos, "rcswitch %lu timings:", (unsigned long)count);
        for (size_t i = 0; i < head && dpos < sizeof(dbuf) - 16; i++) {
            dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos, " %ld", (long)s_local_rc_decode_work[i]);
        }
        if (count > head) dpos += snprintf(dbuf + dpos, sizeof(dbuf) - dpos, " ...");
        ESP_LOGI(TAG, "%s", dbuf);
    }

    subghz_decoded_signal_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    uint64_t keeloq_code = 0;
    int keeloq_bits = 0;
    if (subghz_decode_keeloq(s_local_rc_decode_work,
                             count,
                             &keeloq_code,
                             &keeloq_bits)) {
        decoded.decoded = true;
        decoded.code = keeloq_code;
        decoded.bits = keeloq_bits;
        decoded.te = 400;
        snprintf(decoded.protocol, sizeof(decoded.protocol), "KeeLoq");
        snprintf(decoded.info,
                 sizeof(decoded.info),
                 "KeeLoq %dbit\nCode:0x%016llX",
                 keeloq_bits,
                 (unsigned long long)keeloq_code);
    } else if (!subghz_decode_signal(s_local_rc_decode_work, count, &decoded) || !decoded.decoded) {
        ESP_LOGI(TAG, "local rcswitch repeat no decode from %lu timings", (unsigned long)count);
        return;
    }

    decoded.frequency_hz = (int)s_current_freq_hz;
    s_local_decode_result = decoded;
    s_local_decode_result_ready = true;
    ESP_LOGI(TAG,
             "local rcswitch decode found: %s %dbit from %lu timings",
             s_local_decode_result.protocol,
             s_local_decode_result.bits,
             (unsigned long)count);
}

static void subghz_log_raw_preview(const int32_t *durations, size_t count, int gpio_pin) {
    if (s_raw_preview_logged || !durations || count == 0) {
        return;
    }

    size_t preview = count;
    if (preview > 48) {
        preview = 48;
    }

    char buf[768];
    size_t pos = 0;
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "raw preview pin=%d count=%lu:",
                    gpio_pin,
                    (unsigned long)count);
    for (size_t i = 0; i < preview && pos < sizeof(buf) - 16; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " %ld", (long)durations[i]);
    }
    if (preview < count && pos < sizeof(buf) - 8) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, " ...");
    }
    ESP_LOGI(TAG, "%s", buf);
    s_raw_preview_logged = true;
}

static void subghz_reset_capture_buffers(void) {
    s_raw_active = false;
    s_raw_ready = false;
    s_raw_ignore_until_us = 0;
    s_raw_worklen = 0;
    s_raw_completed_len = 0;
    s_raw_capture_gpio = -1;
    memset((void *)s_raw_prebuf, 0, sizeof(s_raw_prebuf));
    s_raw_prebuf_sum = 0;
    s_raw_noise_gate_open = false;
    s_raw_preview_logged = false;
    s_raw_stream_count = 0;
    s_raw_capture_pending = false;
    s_raw_local_signal_seen = false;
    s_raw_timeout_poll_mode = false;
    s_raw_waiting_first_edge = false;
    s_decode_result_ready = false;
    s_local_decode_result_ready = false;
    memset(&s_local_decode_result, 0, sizeof(s_local_decode_result));
    subghz_reset_local_rcswitch_state();
    if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
        esp_timer_stop(s_raw_timeout_timer);
    }
    if (s_edge_queue) {
        xQueueReset(s_edge_queue);
    }
    if (s_rmt_rx_queue) {
        rmt_rx_done_event_data_t dummy;
        while (xQueueReceive(s_rmt_rx_queue, &dummy, 0) == pdTRUE) {}
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
    uint32_t min_edge_us = subghz_min_valid_edge_us();
    if (delta > 0 && min_edge_us > 0 && delta <= min_edge_us) {
        if (!allow_short_capture && s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
            esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
            return;
        }
        delta = 0;
    }

    if (delta > 0 && s_raw_worklen < SUBGHZ_RAW_CHUNK_MAX_DURATIONS) {
        s_raw_workbufs[s_raw_isr_buf_idx][s_raw_worklen++] = s_raw_prev_level ? (int32_t)delta : -(int32_t)delta;
    }

    s_raw_completed_len = s_raw_worklen;
    s_raw_active = false;
    bool local_normal = subghz_is_tembed_c1101() && !s_stream_to_peer && !s_capture_raw_mode_active;
    s_raw_ready = (allow_short_capture || local_normal) ? (s_raw_completed_len > 0) : (s_raw_completed_len > 8);
    s_raw_isr_buf_idx = 1 - s_raw_isr_buf_idx;
    if (s_raw_ready) {
        char sub_payload[24];
        snprintf(sub_payload, sizeof(sub_payload), "%u", (unsigned)s_raw_completed_len);
        ghostscript_emit_event("subghz_captured", sub_payload);
    }
}

static void subghz_try_local_batch_decode(void) {
    if (s_stream_to_peer || s_capture_raw_mode_active || s_local_decode_result_ready ||
        !s_raw_stream_ptr || s_raw_stream_count < 8) {
        return;
    }

    subghz_decoded_signal_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    if (!subghz_decode_signal((const int32_t *)s_raw_stream_ptr, s_raw_stream_count, &decoded) ||
        !decoded.decoded) {
        return;
    }

    decoded.frequency_hz = (int)s_current_freq_hz;
    s_local_decode_result = decoded;
    s_local_decode_result_ready = true;
    ESP_LOGI(TAG,
             "local batch decode found: %s %dbit from %lu timings",
             s_local_decode_result.protocol,
             s_local_decode_result.bits,
             (unsigned long)s_raw_stream_count);
}

static void subghz_poll_raw_timeout(void) {
    if (!s_raw_capture_enabled || !s_raw_timeout_poll_mode || !s_raw_active || s_raw_ready) {
        return;
    }

    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t delta = (now_us >= s_raw_last_time_us) ? (now_us - s_raw_last_time_us)
                                                    : (UINT32_MAX - s_raw_last_time_us + now_us + 1U);
    if (delta < subghz_raw_timeout_interval_us()) {
        return;
    }

    subghz_finalize_raw_capture(false);
    ESP_LOGD(TAG,
             "raw poll timeout: pin=%d transitions=%lu ready=%d",
             s_raw_capture_gpio,
             (unsigned long)s_raw_completed_len,
             s_raw_ready ? 1 : 0);
}

static void subghz_prepare_raw_capture_for_stream(void) {
    if (!s_raw_ready) {
        return;
    }

    uint8_t completed_idx = 1 - s_raw_isr_buf_idx;
    size_t len = s_raw_completed_len;
    if (len > SUBGHZ_RAW_CHUNK_MAX_DURATIONS) {
        len = SUBGHZ_RAW_CHUNK_MAX_DURATIONS;
    }

    volatile int32_t *completed_ptr = (volatile int32_t *)s_raw_workbufs[completed_idx];
    bool stream_online = s_stream_to_peer && esp_comm_manager_is_connected();
    if (!stream_online && len > 0) {
        if (len == 0) {
            s_raw_completed_len = 0;
            s_raw_ready = false;
            return;
        }
        bool gate_local_idle = subghz_is_tembed_c1101();
        bool normal_mode = !s_capture_raw_mode_active;
        bool chunk_signal = !gate_local_idle ||
                            subghz_local_chunk_has_signal((const int32_t *)completed_ptr, len, normal_mode);
        if (gate_local_idle && chunk_signal) {
            s_raw_local_signal_seen = true;
        } else if (gate_local_idle) {
            ESP_LOGD(TAG,
                     "local %s ignore idle chunk=%lu total=%lu",
                     normal_mode ? "normal" : "raw",
                     (unsigned long)len,
                     (unsigned long)s_raw_stream_count);
            s_raw_completed_len = 0;
            s_raw_ready = false;
            return;
        }
        size_t existing = (s_raw_capture_pending && s_raw_stream_count > 0) ? s_raw_stream_count : 0;
        if (existing > SUBGHZ_RAW_MAX_DURATIONS) {
            existing = SUBGHZ_RAW_MAX_DURATIONS;
        }
        if (existing > 0 && s_raw_stream_ptr != s_shared_buf) {
            memcpy(s_shared_buf, (const void *)s_raw_stream_ptr, existing * sizeof(int32_t));
        }
        size_t copy = len;
        if (existing + copy > SUBGHZ_RAW_MAX_DURATIONS) {
            copy = SUBGHZ_RAW_MAX_DURATIONS - existing;
        }
        if (copy > 0) {
            memcpy(s_shared_buf + existing, (const void *)completed_ptr, copy * sizeof(int32_t));
            existing += copy;
        }
        s_raw_stream_ptr = s_shared_buf;
        s_raw_stream_count = existing;
        s_raw_capture_pending = gate_local_idle ? s_raw_local_signal_seen : (existing > 0);
        if (normal_mode) {
            ESP_LOGI(TAG,
                     "local normal append chunk=%lu total=%lu signal=%d",
                     (unsigned long)len,
                     (unsigned long)s_raw_stream_count,
                     s_raw_local_signal_seen ? 1 : 0);
        }
    } else {
        s_raw_stream_ptr = completed_ptr;
        s_raw_stream_count = len;
        s_raw_capture_pending = (len > 0);
    }
    subghz_log_raw_preview((const int32_t *)s_raw_stream_ptr, s_raw_stream_count, s_raw_capture_gpio);
    subghz_try_local_batch_decode();
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

    subghz_preset_t capture_preset = subghz_capture_preset_for_request(raw_mode);
    esp_err_t err = ESP_OK;
    subghz_display_spi_hold_begin();
    (void)cc1101_reset();
    vTaskDelay(pdMS_TO_TICKS(1));
    err = subghz_apply_preset(capture_preset);
    if (err != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_finish_capture_request(request_id, false, "capture preset apply failed");
        return;
    }
    err = subghz_apply_tembed_local_rx_settings();
    if (err != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_finish_capture_request(request_id, false, "capture local rx config failed");
        return;
    }
    ESP_LOGI(TAG,
             "capture preset=%s",
             (capture_preset == SUBGHZ_PRESET_OOK270_ASYNC) ? "OOK270" :
             (capture_preset == SUBGHZ_PRESET_OOK650_ASYNC) ? "OOK650" : "other");

    err = subghz_configure_capture_gdo_outputs();
    if (err != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_finish_capture_request(request_id, false, "capture gdo config failed");
        return;
    }

    if (subghz_is_tembed_c1101() && !s_stream_to_peer) {
        uint8_t r_iocfg0 = 0, r_iocfg2 = 0, r_pktctrl0 = 0, r_mdmcfg2 = 0, r_mdmcfg3 = 0, r_mdmcfg4 = 0;
        uint8_t r_freq2 = 0, r_freq1 = 0, r_freq0 = 0, r_fsctrl0 = 0, r_frend0 = 0;
        cc1101_read_reg(CC1101_REG_IOCFG0, &r_iocfg0);
        cc1101_read_reg(CC1101_REG_IOCFG2, &r_iocfg2);
        cc1101_read_reg(CC1101_REG_PKTCTRL0, &r_pktctrl0);
        cc1101_read_reg(CC1101_REG_MDMCFG2, &r_mdmcfg2);
        cc1101_read_reg(CC1101_REG_MDMCFG3, &r_mdmcfg3);
        cc1101_read_reg(CC1101_REG_MDMCFG4, &r_mdmcfg4);
        cc1101_read_reg(CC1101_REG_FREQ2, &r_freq2);
        cc1101_read_reg(CC1101_REG_FREQ1, &r_freq1);
        cc1101_read_reg(CC1101_REG_FREQ0, &r_freq0);
        cc1101_read_reg(CC1101_REG_FSCTRL0, &r_fsctrl0);
        cc1101_read_reg(CC1101_REG_FREND0, &r_frend0);
        ESP_LOGI(TAG,
                 "pre-SRX regs: IOCFG0=0x%02X IOCFG2=0x%02X PKTCTRL0=0x%02X "
                 "MDMCFG2=0x%02X MDMCFG3=0x%02X MDMCFG4=0x%02X "
                 "FREQ=0x%02X%02X%02X FSCTRL0=0x%02X FREND0=0x%02X",
                 r_iocfg0, r_iocfg2, r_pktctrl0,
                 r_mdmcfg2, r_mdmcfg3, r_mdmcfg4,
                 r_freq2, r_freq1, r_freq0, r_fsctrl0, r_frend0);
    }

    err = subghz_retune_frequency(frequency_hz);
    if (s_capture_request_id != request_id) {
        subghz_display_spi_hold_end();
        return;
    }
    if (err != ESP_OK) {
        subghz_display_spi_hold_end();
        s_current_freq_idx = (uint8_t)freq_idx;
        s_current_freq_hz = frequency_hz;
        subghz_finish_capture_request(request_id, false, subghz_remote_manager_get_last_error());
        return;
    }

    s_current_freq_idx = (uint8_t)freq_idx;
    subghz_display_spi_hold_end();
    subghz_remote_manager_set_raw_capture_enabled(true);
    s_capture_raw_mode_active = raw_mode;
    if (s_capture_request_id != request_id) {
        subghz_remote_manager_set_raw_capture_enabled(false);
        return;
    }

    subghz_finish_capture_request(request_id, true, NULL);
}

static void IRAM_ATTR subghz_gdo0_isr_handler(void *arg) {
    if (!s_raw_capture_enabled) return;
    s_capture_isr_edges++;

    /* RMT owns capture: just count pin edges for the wedge diagnostic and
     * get out -- no queue work, no duration recording. */
    if (s_rmt_rx_active) {
        s_rmt_diag_pin_edges++;
        return;
    }

    int pin = (int)(intptr_t)arg;
    if (pin < 0) {
        pin = CONFIG_SUBGHZ_GDO0_PIN;
    }
    if (s_raw_capture_gpio >= 0 && pin != s_raw_capture_gpio) {
        return;
    }

    uint32_t now_us = (uint32_t)esp_timer_get_time();
    if (s_tembed_local_capture && !s_capture_raw_mode_active) {
        /* Glitch-filtered edge intake. The 2026-08-06 capture log showed the
         * unfiltered path drowning in 2-9us noise excursions: each spike
         * during a long (e.g. 10.8ms PT2262 sync-low) interval chopped it at
         * a random point, so the repeat-gap matcher almost never fired and
         * frames died as 5-19 timing fragments. Rules:
         *  - level unchanged since last accepted edge: coalesced edge pair
         *    (ISR latency) or spurious retrigger -- nothing real happened.
         *  - interval < SUBGHZ_GDO0_GLITCH_US: trailing edge of a spike.
         *    Rewind the interval its leading edge recorded so the two
         *    surrounding same-level segments merge into one clean interval.
         *  - otherwise accept and feed the accumulator. */
        int pin_level = gpio_get_level((gpio_num_t)pin);
        if (pin_level == s_raw_prev_level) {
            return;
        }
        uint32_t duration = now_us - s_raw_last_time_us;
        if (duration < SUBGHZ_GDO0_GLITCH_US) {
            s_capture_isr_glitches++;
            if (s_local_prev_accept_valid) {
                if (s_local_rc_change_count > 0) {
                    s_local_rc_change_count--;
                }
                if (s_local_prev_accept_was_gap && s_local_rc_repeat_count > 0) {
                    s_local_rc_repeat_count--;
                }
                s_raw_last_time_us = s_local_prev_accept_time_us;
                s_local_prev_accept_valid = false;
            }
            s_raw_prev_level = pin_level;
            return;
        }
        s_local_prev_accept_time_us = s_raw_last_time_us;
        s_local_prev_accept_was_gap = (duration > SUBGHZ_RCSWITCH_SEPARATION_US);
        s_local_prev_accept_valid = true;
        s_raw_last_time_us = now_us;
        s_raw_prev_level = pin_level;
        s_raw_capture_gpio = pin;
        s_capture_isr_accepted++;
        subghz_local_rcswitch_handle_duration(duration);
        return;
    }

    int current_level = gpio_get_level((gpio_num_t)pin);
    if (!s_stream_to_peer && s_raw_ignore_until_us != 0 && (int32_t)(now_us - s_raw_ignore_until_us) < 0) {
        return;
    }

    if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
        esp_timer_stop(s_raw_timeout_timer);
    }

    if (!s_raw_active) {
        s_raw_capture_gpio = pin;
        s_raw_active = true;
        s_raw_waiting_first_edge = false;
        s_raw_worklen = 0;
        memset((void *)s_raw_prebuf, 0, sizeof(s_raw_prebuf));
        s_raw_prebuf_sum = 0;
        s_raw_noise_gate_open = false;
        /* The interval ending at the next edge has the level observed now. */
        s_raw_prev_level = current_level;
        s_raw_last_time_us = now_us;
        if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
            esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
        }
        return;
    }

    uint32_t delta = (now_us >= s_raw_last_time_us) ? (now_us - s_raw_last_time_us)
                                                    : (UINT32_MAX - s_raw_last_time_us + now_us + 1U);
    uint32_t min_edge_us = 0U;
    if (delta > 0 && min_edge_us > 0 && delta <= min_edge_us) {
        /* Drop the short pulse without losing the physical GPIO phase. */
        s_raw_prev_level = current_level;
        s_raw_last_time_us = now_us;
        if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
            esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
        }
        return;
    }

    int32_t signed_delta = s_raw_prev_level ? (int32_t)delta : -(int32_t)delta;

    if (!s_stream_to_peer && s_capture_raw_mode_active) {
        uint32_t oldest = s_raw_prebuf[0];
        s_raw_prebuf_sum = s_raw_prebuf_sum + delta - oldest;
        for (size_t i = 0; i + 1 < SUBGHZ_RAW_PREBUF_SIZE; i++) {
            s_raw_prebuf[i] = s_raw_prebuf[i + 1];
        }
        s_raw_prebuf[SUBGHZ_RAW_PREBUF_SIZE - 1] = delta;

        if (!s_raw_noise_gate_open) {
            if (s_raw_prebuf_sum < (SUBGHZ_RAW_PREBUF_SIZE * SUBGHZ_RAW_NOISE_THRESHOLD_US)) {
                s_raw_prev_level = current_level;
                s_raw_last_time_us = now_us;
                if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
                    esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
                }
                return;
            }
            s_raw_noise_gate_open = true;
        }

        if (delta <= SUBGHZ_RAW_NOISE_THRESHOLD_US) {
            s_raw_prev_level = current_level;
            s_raw_last_time_us = now_us;
            if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
                esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
            }
            return;
        }
    }

    if (delta > 0 && s_raw_worklen < SUBGHZ_RAW_CHUNK_MAX_DURATIONS) {
        s_raw_workbufs[s_raw_isr_buf_idx][s_raw_worklen++] = signed_delta;
    }

    if (s_edge_queue && !s_capture_raw_mode_active && !s_decode_result_ready && delta > 0) {
        subghz_edge_t edge = { .level = (bool)s_raw_prev_level, .duration = delta };
        xQueueSendFromISR(s_edge_queue, &edge, NULL);
    }

    s_raw_prev_level = current_level;
    s_raw_last_time_us = now_us;

    if (s_raw_timeout_timer && !s_raw_timeout_poll_mode) {
        esp_timer_start_once(s_raw_timeout_timer, subghz_raw_timeout_interval_us());
    }
}

static void subghz_raw_timeout_cb(void *arg) {
    (void)arg;
    if (!s_raw_active) {
        return;
    }

    subghz_finalize_raw_capture(false);
    if (!s_stream_to_peer) {
        ESP_LOGD(TAG,
                 "raw timeout: pin=%d transitions=%lu ready=%d",
                 s_raw_capture_gpio,
                 (unsigned long)s_raw_completed_len,
                 s_raw_ready);
    } else {
        ESP_LOGI(TAG,
                 "raw timeout: pin=%d transitions=%lu ready=%d",
                 s_raw_capture_gpio,
                 (unsigned long)s_raw_completed_len,
                 s_raw_ready);
    }
}

static void subghz_stream_raw_capture(void) {
    if (!s_raw_capture_pending || s_raw_stream_count == 0) {
        return;
    }
    if (!s_stream_to_peer || !esp_comm_manager_is_connected()) {
        return;
    }
    if (((!s_capture_raw_mode_active) && s_decode_result_ready) || s_paused) {
        ESP_LOGD(TAG, "stream_raw: skip pending=%d count=%lu online=%d", s_raw_capture_pending, (unsigned long)s_raw_stream_count, esp_comm_manager_is_connected());
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
#if SOC_SPI_PERIPH_NUM > 2
    if (CONFIG_SUBGHZ_SPI_HOST == 2) {
        return SPI2_HOST;
    }
    return SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}

static bool subghz_shares_display_spi_bus(void) {
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI)
    bool host_match = false;
#if SOC_SPI_PERIPH_NUM > 2
#if defined(CONFIG_LV_TFT_DISPLAY_SPI2_HOST)
    if (CONFIG_SUBGHZ_SPI_HOST == 2) {
        host_match = true;
    }
#endif
#if defined(CONFIG_LV_TFT_DISPLAY_SPI3_HOST)
    if (CONFIG_SUBGHZ_SPI_HOST == 3) {
        host_match = true;
    }
#endif
#else
    host_match = true;
#endif

    return host_match &&
           CONFIG_SUBGHZ_SPI_MOSI_PIN == CONFIG_LV_DISP_SPI_MOSI &&
           CONFIG_SUBGHZ_SPI_MISO_PIN == CONFIG_LV_DISP_SPI_MISO &&
           CONFIG_SUBGHZ_SPI_SCK_PIN == CONFIG_LV_DISP_SPI_CLK;
#else
    return false;
#endif
}

static bool subghz_shares_sdcard_spi_bus(void) {
#if defined(CONFIG_USING_SPI)
    return CONFIG_SUBGHZ_SPI_MOSI_PIN == sd_card_manager.spi_mosi_pin &&
           CONFIG_SUBGHZ_SPI_MISO_PIN == sd_card_manager.spi_miso_pin &&
           CONFIG_SUBGHZ_SPI_SCK_PIN == sd_card_manager.spi_clk_pin;
#else
    return false;
#endif
}

static bool subghz_shares_nrf24_spi_bus(void) {
#if defined(CONFIG_HAS_NRF24)
    return CONFIG_SUBGHZ_SPI_MOSI_PIN == CONFIG_NRF24_SPI_MOSI_PIN &&
           CONFIG_SUBGHZ_SPI_MISO_PIN == CONFIG_NRF24_SPI_MISO_PIN &&
           CONFIG_SUBGHZ_SPI_SCK_PIN == CONFIG_NRF24_SPI_SCK_PIN &&
           !subghz_shares_sdcard_spi_bus();
#else
    return false;
#endif
}

static subghz_bus_mode_t subghz_detect_bus_mode(void) {
    /* T-Embed CC1101: force STANDALONE even though the pins match the TFT bus.
     * Known-good firmware for this board runs the CC1101 as a plain extra
     * device on the shared wires with zero arbitration and works; GhostESP's
     * SHARED_DISPLAY suspend/hold/resume machinery was the prime suspect in
     * the dead-RX bug. STANDALONE lets the ESP-IDF SPI driver serialize
     * devices (CS12 vs CS41) while LVGL keeps running -- the same physical
     * sharing model, without the churn. */
    if (subghz_is_tembed_c1101()) {
        ESP_LOGI(TAG, "Bus mode: STANDALONE (forced for TEmbedC1101)");
        return SUBGHZ_BUS_STANDALONE;
    }
    if (subghz_shares_display_spi_bus()) {
        ESP_LOGI(TAG, "Bus mode: SHARED_DISPLAY (MOSI=%d matches TFT)",
                 CONFIG_SUBGHZ_SPI_MOSI_PIN);
        return SUBGHZ_BUS_SHARED_DISPLAY;
    }
    if (subghz_shares_sdcard_spi_bus()) {
        ESP_LOGI(TAG, "Bus mode: SHARED_SDCARD (MOSI=%d matches SD)",
                 CONFIG_SUBGHZ_SPI_MOSI_PIN);
        return SUBGHZ_BUS_SHARED_SDCARD;
    }
    if (subghz_shares_nrf24_spi_bus()) {
        ESP_LOGI(TAG, "Bus mode: SHARED_NRF24 (MOSI=%d matches NRF24)",
                 CONFIG_SUBGHZ_SPI_MOSI_PIN);
        return SUBGHZ_BUS_SHARED_NRF24;
    }
    ESP_LOGI(TAG, "Bus mode: STANDALONE (own SPI bus)");
    return SUBGHZ_BUS_STANDALONE;
}

#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI)
static void subghz_display_spi_suspend(void) {
    if (s_bus_mode != SUBGHZ_BUS_SHARED_DISPLAY) {
        return;
    }
    if (s_display_spi_suspended) {
        return;
    }
    display_manager_suspend_lvgl_task();
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
        if (refr) lv_timer_pause(refr);
    }
    disp_wait_for_pending_transactions();
#ifdef CONFIG_LV_DISP_SPI_CS
    gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
#endif
    s_display_spi_suspended = true;
    ESP_LOGD(TAG, "Display SPI suspended for CC1101");
}

static void subghz_display_spi_restore(void) {
    if (!s_display_spi_suspended) {
        return;
    }
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
        if (refr) lv_timer_resume(refr);
    }
    display_manager_resume_lvgl_task();
    s_display_spi_suspended = false;
    ESP_LOGD(TAG, "Display SPI restored after CC1101");
}
#else
static void subghz_display_spi_suspend(void) {}
static void subghz_display_spi_restore(void) {}
#endif

/* Keep the screen fully static while the CC1101 listens: every LVGL
 * invalidation triggers a display SPI flush on the shared wires, and each
 * flush couples an EMI burst into GDO0 / the RF front-end (observed as
 * 3-11 garbage edges every 500ms, matching the status-bar refresh). The
 * 500ms status-bar timer is the only periodic invalidator left once the
 * capture popup stops updating labels, so gating it silences the bus. */
static void subghz_display_status_updates_set_enabled(bool enabled) {
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI)
    display_manager_set_status_updates_enabled(enabled);
#else
    (void)enabled;
#endif
}

static void subghz_display_spi_hold_begin(void) {
    if (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) {
        if (s_display_spi_hold_depth++ == 0) {
            subghz_display_spi_suspend();
        }
        subghz_force_inactive_shared_spi_cs_high();
    }

    if (s_spi_dev && !s_cc1101_bus_acquired) {
        if (spi_device_acquire_bus(s_spi_dev, portMAX_DELAY) == ESP_OK) {
            s_cc1101_bus_acquired = true;
        }
    }
}

static void subghz_display_spi_hold_end(void) {
    if (s_cc1101_bus_acquired) {
        spi_device_release_bus(s_spi_dev);
        s_cc1101_bus_acquired = false;
    }

    if (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY && s_display_spi_hold_depth > 0) {
        s_display_spi_hold_depth--;
        if (s_display_spi_hold_depth == 0) {
            subghz_display_spi_restore();
        }
    }
}

static void subghz_force_inactive_shared_spi_cs_high(void) {
    if (!subghz_is_tembed_c1101()) {
        return;
    }

    /* Only babysit foreign CS pins when we actually share their bus. Under the
     * forced-STANDALONE TEmbed mode the other drivers own their CS lines and
     * the IDF bus lock serializes transactions; touching them per-op is both
     * useless and racy (the reference flow does none of this). Boot-time isolation of the
     * NRF/SD CS pins still happens once in subghz_prepare_tembed_board_pins(). */
    if (s_bus_mode == SUBGHZ_BUS_STANDALONE) {
        return;
    }

#ifdef CONFIG_LV_DISP_SPI_CS
    gpio_set_level((gpio_num_t)CONFIG_LV_DISP_SPI_CS, 1);
#endif
#ifdef CONFIG_SD_SPI_CS_PIN
    gpio_set_level((gpio_num_t)CONFIG_SD_SPI_CS_PIN, 1);
#endif
    gpio_set_level((gpio_num_t)44, 1);
    gpio_set_level((gpio_num_t)43, 0);
}

static esp_err_t cc1101_select_wait_ready(void) {
    gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 0);
    /* 5ms: after a cold power-up (GPIO15) or SRES the crystal needs up to a
     * few ms before CHIP_RDYn (SO low) asserts; a powered, running chip
     * answers in microseconds. */
    int64_t deadline = esp_timer_get_time() + 5000;
    while (gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_SPI_MISO_PIN) != 0) {
        if (esp_timer_get_time() >= deadline) {
            gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
            return ESP_ERR_TIMEOUT;
        }
        ets_delay_us(1);
    }
    return ESP_OK;
}

static esp_err_t subghz_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_spi_dev || !tx || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    bool local_hold = false;
    if (!s_cc1101_bus_acquired) {
        subghz_display_spi_hold_begin();
        local_hold = true;
    }

    esp_err_t err = ESP_OK;
    if (s_spi_manual_csn) {
        subghz_force_inactive_shared_spi_cs_high();
        gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
        ets_delay_us(2);
        err = cc1101_select_wait_ready();
        if (err != ESP_OK) {
            if (local_hold) {
                subghz_display_spi_hold_end();
            }
            return err;
        }
        ets_delay_us(2);
    }

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (uint32_t)(len * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    err = spi_device_transmit(s_spi_dev, &t);

    if (s_spi_manual_csn) {
        ets_delay_us(1);
        gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
        ets_delay_us(2);
    }

    if (local_hold) {
        subghz_display_spi_hold_end();
    }

    return err;
}

static esp_err_t cc1101_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = { reg, value };
    uint8_t rx[2] = {0};
    return subghz_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t cc1101_read_reg(uint8_t reg, uint8_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[2] = { (uint8_t)(reg | 0x80), 0x00 };
    uint8_t rx[2] = {0};
    esp_err_t err = subghz_spi_transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
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

static esp_err_t cc1101_reset(void) {
    if (s_spi_manual_csn) {
        esp_err_t err = ESP_OK;
        bool local_hold = false;
        if (!s_cc1101_bus_acquired) {
            subghz_display_spi_hold_begin();
            local_hold = true;
        }

        uint8_t tx[1] = { CC1101_STROBE_SRES };
        uint8_t rx[1] = {0};
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        t.length = 8;
        t.tx_buffer = tx;
        t.rx_buffer = rx;

        gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
        ets_delay_us(1000);
        err = cc1101_select_wait_ready();
        if (err != ESP_OK) {
            if (local_hold) {
                subghz_display_spi_hold_end();
            }
            return err;
        }
        ets_delay_us(1000);
        gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
        ets_delay_us(1000);
        err = cc1101_select_wait_ready();
        if (err != ESP_OK) {
            if (local_hold) {
                subghz_display_spi_hold_end();
            }
            return err;
        }
        ets_delay_us(2);
        err = spi_device_transmit(s_spi_dev, &t);
        ets_delay_us(1);
        gpio_set_level((gpio_num_t)CONFIG_SUBGHZ_CSN_PIN, 1);
        ets_delay_us(2);
        if (local_hold) {
            subghz_display_spi_hold_end();
        }
        ets_delay_us(1000);
        return err;
    }

    return cc1101_strobe(CC1101_STROBE_SRES);
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

static esp_err_t cc1101_wait_for_state(uint8_t expected_state, uint32_t timeout_us, uint8_t *last_state) {
    int64_t deadline = esp_timer_get_time() + timeout_us;
    uint8_t state = 0xFF;
    do {
        esp_err_t err = cc1101_read_status(CC1101_STATUS_MARCSTATE, &state);
        if (err != ESP_OK) {
            if (last_state) {
                *last_state = state;
            }
            return err;
        }
        state &= 0x1F;
        if (state == expected_state) {
            if (last_state) {
                *last_state = state;
            }
            return ESP_OK;
        }
        ets_delay_us(100);
    } while (esp_timer_get_time() < deadline);

    if (last_state) {
        *last_state = state;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t cc1101_write_patable(const uint8_t *data, size_t len) {
    if (!data || len == 0 || !s_spi_dev) return ESP_ERR_INVALID_ARG;
    size_t total = 1 + len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;
    buf[0] = 0x7E;
    memcpy(&buf[1], data, len);
    esp_err_t err = subghz_spi_transfer(buf, NULL, total);
    free(buf);
    return err;
}

static esp_err_t subghz_hw_start(void) {
    /* Keep the system out of light sleep for the entire SubGHz session. With
     * CONFIG_PM_ENABLE + CONFIG_PM_SLP_DISABLE_GPIO + tickless idle, every
     * idle moment floated GPIO15 (CC1101 power) and stopped the RMT/APB
     * clocks, wiping the radio's registers at random times (MARCSTATE reads
     * of 0x00 were a dead chip, not SLEEP). */
    subghz_capture_pm_lock_acquire();

    s_spi_host = subghz_spi_host_from_config();
    s_bus_mode = subghz_detect_bus_mode();
    subghz_prepare_tembed_board_pins();

    gpio_config_t gdo_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SUBGHZ_GDO0_PIN),
        .mode = GPIO_MODE_INPUT,
        /* Pull-up keeps the line from floating (and picking up display-SPI
         * EMI) whenever the CC1101 leaves GDO0 high-impedance, e.g. in SLEEP.
         * A driven push-pull output overrides the weak pull, so normal
         * reception is unaffected. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
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

    /* Preserve the active GDO input routing if the rest of the system sleeps. */
    (void)gpio_sleep_sel_dis((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
    (void)gpio_sleep_sel_dis((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
#endif

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

    if (!s_gpio_isr_service_installed) {
        /* LEVEL3: at the Arduino-compatible default priority the GDO0 ISR
         * latency under WiFi+LVGL regularly exceeded short OOK pulse widths,
         * coalescing edge pairs in the GPIO status bit (2026-08-06 diag:
         * ~40% of ISR invocations found the pin level unchanged). The local
         * handler is short and IRAM-safe, so a higher level is safe. Only
         * applies when this call installs the service. */
        esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
            subghz_set_last_error("gpio isr service failed");
            return isr_ret;
        }
        if (isr_ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "GPIO ISR service was already installed; using its existing interrupt flags");
        } else {
            ESP_LOGI(TAG, "GPIO ISR service installed at LEVEL3 priority");
        }
        s_gpio_isr_service_installed = true;
    }
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    err = gpio_isr_handler_add((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN,
                               subghz_gdo0_isr_handler,
                               (void *)(intptr_t)CONFIG_SUBGHZ_GDO0_PIN);
    if (err != ESP_OK) {
        subghz_set_last_error("gdo0 isr add failed");
        return err;
    }
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
    if (subghz_should_watch_gdo2_capture()) {
        gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
        err = gpio_isr_handler_add((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN,
                                   subghz_gdo0_isr_handler,
                                   (void *)(intptr_t)CONFIG_SUBGHZ_GDO2_PIN);
        if (err != ESP_OK) {
            subghz_set_last_error("gdo2 isr add failed");
            return err;
        }
    }
#endif

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_SUBGHZ_SPI_MOSI_PIN,
        .miso_io_num = CONFIG_SUBGHZ_SPI_MISO_PIN,
        .sclk_io_num = CONFIG_SUBGHZ_SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = subghz_is_tembed_c1101() ? SUBGHZ_TEMBED_SPI_CLOCK_HZ : CONFIG_SUBGHZ_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = subghz_is_tembed_c1101() ? -1 : CONFIG_SUBGHZ_CSN_PIN,
        .queue_size = 1,
    };
    s_spi_manual_csn = subghz_is_tembed_c1101();

    if (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) {
        s_spi_bus_initialized_by_us = false;
        display_manager_suspend_lvgl_task();
        disp_wait_for_pending_transactions();
#ifdef CONFIG_LV_DISP_SPI_CS
        gpio_set_level((gpio_num_t)CONFIG_LV_DISP_SPI_CS, 1);
#endif
        err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
        if (err != ESP_OK) {
            subghz_set_last_error("shared display spi add device failed");
            display_manager_resume_lvgl_task();
            return err;
        }
        display_manager_resume_lvgl_task();
        ESP_LOGI(TAG, "Using shared display SPI bus (host=%d)", (int)s_spi_host);
    } else if (s_bus_mode == SUBGHZ_BUS_SHARED_SDCARD) {
        s_spi_bus_initialized_by_us = false;
        err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_spi_bus_initialized_by_us = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized_by_us = false;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            subghz_set_last_error("shared sdcard spi bus init failed");
            return err;
        }
        ESP_LOGI(TAG, "Using shared SD card SPI bus (host=%d)", (int)s_spi_host);
    } else if (s_bus_mode == SUBGHZ_BUS_SHARED_NRF24) {
        s_spi_bus_initialized_by_us = false;
        err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_spi_bus_initialized_by_us = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized_by_us = false;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            subghz_set_last_error("shared nrf24 spi bus init failed");
            return err;
        }
        ESP_LOGI(TAG, "Using shared NRF24 SPI bus (host=%d)", (int)s_spi_host);
    } else {
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
        ESP_LOGI(TAG, "Using standalone SPI bus (host=%d)", (int)s_spi_host);
    }

    if (s_bus_mode != SUBGHZ_BUS_SHARED_DISPLAY) {
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
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    err = cc1101_reset();
    if (err != ESP_OK) {
        subghz_set_last_error("radio reset failed");
        subghz_hw_stop();
        return err;
    }

    ets_delay_us(1000);
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    (void)cc1101_strobe(CC1101_STROBE_SFRX);
    (void)cc1101_strobe(CC1101_STROBE_SFTX);

    /* Detect the CC1101 while it is in IDLE, BEFORE any RX / async-serial config.
     * On the TEmbed C1101 the status registers read back 0x00 once the chip is
     * strobed into RX (diag probe: VERSION=0x14 in IDLE, 0x00 after SRX), so the
     * old post-SRX detection was a false negative. The reference flow likewise
     * detects the chip before entering RX. */
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

    s_current_freq_hz = (uint32_t)((uint64_t)CONFIG_SUBGHZ_BASE_FREQ_MHZ * 10000ULL);
    subghz_apply_board_rf_switch(s_current_freq_hz);

    if (err == ESP_OK) err = subghz_apply_preset(SUBGHZ_PRESET_OOK650_ASYNC);
    if (err == ESP_OK) err = subghz_apply_tembed_local_rx_settings();
    if (err == ESP_OK) err = subghz_configure_capture_gdo_outputs();
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_CHANNR, 0x00);

    uint32_t freq_word = (uint32_t)((((uint64_t)s_current_freq_hz) * 65536ULL) / 26000000ULL);
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));
    if (err == ESP_OK) err = subghz_apply_tembed_freq_calibration(s_current_freq_hz);

    if (err == ESP_OK) {
        esp_err_t verify_err = subghz_verify_cc1101_register_readback(s_current_freq_hz, "init");
        if (verify_err != ESP_OK) {
            ESP_LOGW(TAG, "CC1101 init readback failed, retrying reset/config sequence");
            err = cc1101_reset();
            if (err == ESP_OK) {
                ets_delay_us(1000);
                (void)cc1101_strobe(CC1101_STROBE_SIDLE);
                (void)cc1101_strobe(CC1101_STROBE_SFRX);
                (void)cc1101_strobe(CC1101_STROBE_SFTX);
                subghz_apply_board_rf_switch(s_current_freq_hz);
                err = subghz_apply_preset(SUBGHZ_PRESET_OOK650_ASYNC);
            }
            if (err == ESP_OK) err = subghz_apply_tembed_local_rx_settings();
            if (err == ESP_OK) err = subghz_configure_capture_gdo_outputs();
            if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_CHANNR, 0x00);
            if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
            if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
            if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));
            if (err == ESP_OK) err = subghz_apply_tembed_freq_calibration(s_current_freq_hz);
            if (err == ESP_OK) err = subghz_verify_cc1101_register_readback(s_current_freq_hz, "init-retry");
        }
    }

    if (err == ESP_OK) {
        err = cc1101_strobe(CC1101_STROBE_SIDLE);
        if (err == ESP_OK) err = cc1101_strobe(CC1101_STROBE_SRX);
    }

    if (err != ESP_OK) {
        subghz_set_last_error("radio init sequence failed");
        subghz_hw_stop();
        return err;
    }

    /* Detection already ran in IDLE above (before SRX); version/partnum hold
     * those values for the log line below. */

    const char *bus_label =
        (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) ? "SHARED-DISPLAY" :
        (s_bus_mode == SUBGHZ_BUS_SHARED_SDCARD)  ? "SHARED-SDCARD" :
        (s_bus_mode == SUBGHZ_BUS_SHARED_NRF24)   ? "SHARED-NRF24" :
                                                     "STANDALONE";

    ESP_LOGI(TAG,
             "CC1101 init OK host=%d bus=%s MOSI=%d MISO=%d SCK=%d CSN=%d GDO0=%d GDO2=%d ver=0x%02X part=0x%02X %s",
             (int)CONFIG_SUBGHZ_SPI_HOST,
             bus_label,
             CONFIG_SUBGHZ_SPI_MOSI_PIN,
             CONFIG_SUBGHZ_SPI_MISO_PIN,
             CONFIG_SUBGHZ_SPI_SCK_PIN,
             CONFIG_SUBGHZ_CSN_PIN,
             CONFIG_SUBGHZ_GDO0_PIN,
             CONFIG_SUBGHZ_GDO2_PIN,
             version,
             partnum,
             subghz_is_tembed_c1101() ? "ESP-IDF-SPI-MANUAL-CS" : "ESP-IDF-SPI");

    s_radio_ready = true;

    return ESP_OK;
}

static esp_err_t subghz_retune_frequency(uint32_t freq_hz) {
    if (!s_spi_dev || !s_radio_ready) {
        subghz_set_last_error("radio not ready");
        return ESP_ERR_INVALID_STATE;
    }

    subghz_radio_lock();
    subghz_display_spi_hold_begin();

    uint32_t freq_word = (uint32_t)((((uint64_t)freq_hz) * 65536ULL) / 26000000ULL);
    subghz_apply_board_rf_switch(freq_hz);
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
        step = "CALCFG";
        err = subghz_apply_tembed_freq_calibration(freq_hz);
    }
    if (err == ESP_OK) {
        step = "VERIFY";
        err = subghz_verify_cc1101_register_readback(freq_hz, "retune-pre-rx");
    }
    if (err == ESP_OK) {
        step = "SRX";
        err = cc1101_strobe(CC1101_STROBE_SRX);
    }
    if (err == ESP_OK) {
        /* NOTE: MARCSTATE reads during RX are unreliable on this board (they
         * return 0x00 even when the chip is verifiably in RX -- the diag
         * probe sees 0x00 for seconds before a real 0x0D read lands). The old
         * retry treated 0x00 as "not in RX" and strobed SFRX + SRX ~10ms
         * after entering RX. SFRX is only spec'd for IDLE/RXFIFO_OVERFLOW;
         * firing it into an actively-demodulating receiver killed the RX
         * chain within ~15ms of every capture arm (probe, which never does
         * this, receives continuously). Log the wait result only. */
        uint8_t state = 0;
        if (cc1101_wait_for_state(0x0D, 10000U, &state) != ESP_OK) {
            ESP_LOGW(TAG, "retune RX wait: state=0x%02X (0x00 reads are a known artifact, RX likely fine)", state);
        }
    }

    subghz_display_spi_hold_end();
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
    s_raw_timeout_poll_mode = false;
    s_raw_waiting_first_edge = false;
    /* hw_stop bypasses set_raw_capture_enabled(false); don't leave the status
     * bar frozen if the view was destroyed mid-capture. */
    if (s_tembed_local_capture) {
        subghz_display_status_updates_set_enabled(true);
    }
    s_tembed_local_capture = false;
    s_local_decode_result_ready = false;
    subghz_reset_local_rcswitch_state();
    subghz_rmt_rx_stop();
    gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
    if (subghz_should_watch_gdo2_capture()) {
        gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
        gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
    }
#endif
    if (s_raw_timeout_timer) {
        esp_timer_stop(s_raw_timeout_timer);
    }
    if (s_spi_dev) {
        (void)cc1101_strobe(CC1101_STROBE_SIDLE);
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }
    s_spi_manual_csn = false;

    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
    }

    s_display_spi_hold_depth = 0;
    subghz_display_spi_restore();
    s_bus_mode = SUBGHZ_BUS_STANDALONE;

    /* Drop every outstanding PM hold: the session-wide hold from
     * subghz_hw_start() plus any nested capture-level hold still armed when
     * the view closed (hw_stop bypasses set_raw_capture_enabled(false)). */
#ifdef CONFIG_PM_ENABLE
    while (s_capture_pm_lock_depth > 0) {
        subghz_capture_pm_lock_release();
    }
#else
    subghz_capture_pm_lock_release();
#endif
}

static uint8_t subghz_sample_channel(uint8_t ch, int settle_us) {
    subghz_radio_lock();
    subghz_display_spi_hold_begin();

    if (cc1101_write_reg(0x0A, ch) != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_radio_unlock();
        return 0;
    }
    if (cc1101_strobe(CC1101_STROBE_SRX) != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_radio_unlock();
        return 0;
    }

    ets_delay_us((uint32_t)settle_us);

    uint8_t raw = 0;
    if (cc1101_read_status(CC1101_STATUS_RSSI, &raw) != ESP_OK) {
        subghz_display_spi_hold_end();
        subghz_radio_unlock();
        return 0;
    }

    subghz_display_spi_hold_end();
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
    int edge_count = 0;
    while (s_decoder_task_running) {
        if (xQueueReceive(s_edge_queue, &edge, pdMS_TO_TICKS(20))) {
            if (!s_decode_result_ready) {
                subghz_engine_feed(&s_decoder_engine, edge.level, edge.duration);
                edge_count++;
                if (s_decoder_engine.found) {
                    const subghz_stream_decoder_t *res = subghz_engine_get_result(&s_decoder_engine);
                    ESP_LOGI(TAG, "%s decode found: %s %dbit after %d edges",
                             s_stream_to_peer ? "stream" : "local",
                             res ? res->name : "?", res ? (int)res->bits : 0, edge_count);
                    s_decode_result_ready = true;
                    if (s_stream_to_peer) {
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
        } else if (edge_count > 0 && s_raw_capture_enabled) {
            ESP_LOGD(TAG, "%s decoder queue empty after %d edges, no match",
                     s_stream_to_peer ? "stream" : "local", edge_count);
            edge_count = 0;
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
    xTaskCreatePinnedToCore(subghz_decoder_task,
                            "subghz_dec",
                            5120,
                            NULL,
                            SUBGHZ_DECODER_TASK_PRIORITY,
                            &s_decoder_task,
                            SUBGHZ_DECODER_TASK_CORE);

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
    bool local_tembed_analyzer_skip_logged = false;

    while (!s_stop_requested) {
        subghz_poll_raw_timeout();
        subghz_poll_local_rcswitch_decode();
        subghz_rmt_poll_local_decode();

        /* 1Hz RX-state diagnostic during T-Embed local capture: same 1Hz
         * MARCSTATE reads the diag probe performs safely. Tells us whether
         * the chip stays in RX (0x0D) when the pin goes flat. The edge
         * counters make capture directly comparable to the probe's Phase D:
         * isr = all GDO0 ISR invocations, acc = intervals accepted into the
         * RCSwitch accumulator, gl = glitch excursions absorbed, pin = the
         * GPIO tap counter used when the RMT owns capture. */
        if (s_raw_capture_enabled && s_tembed_local_capture) {
            int64_t now_diag = esp_timer_get_time();
            if (now_diag - s_capture_diag_last_us >= 1000000) {
                s_capture_diag_last_us = now_diag;
                uint8_t marc = 0, rssi = 0;
                (void)cc1101_read_status(CC1101_STATUS_MARCSTATE, &marc);
                (void)cc1101_read_status(CC1101_STATUS_RSSI, &rssi);
                uint32_t isr_now = s_capture_isr_edges;
                uint32_t pin_now = s_rmt_diag_pin_edges;
                ESP_LOGI(TAG,
                         "capture diag: MARC=0x%02X RSSI=0x%02X GDO0=%d rc_changes=%lu "
                         "isr=%lu(+%lu) acc=%lu gl=%lu pin=%lu(+%lu)",
                         marc, rssi,
                         gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN),
                         (unsigned long)s_local_rc_change_count,
                         (unsigned long)isr_now,
                         (unsigned long)(isr_now - s_capture_diag_last_isr_edges),
                         (unsigned long)s_capture_isr_accepted,
                         (unsigned long)s_capture_isr_glitches,
                         (unsigned long)pin_now,
                         (unsigned long)(pin_now - s_capture_diag_last_pin_edges));
                s_capture_diag_last_isr_edges = isr_now;
                s_capture_diag_last_pin_edges = pin_now;
            }
        }

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

        if (subghz_is_tembed_c1101() && !s_stream_to_peer) {
            if (!local_tembed_analyzer_skip_logged) {
                ESP_LOGI(TAG, "TEmbed local mode: background analyzer disabled to keep shared TFT/CC1101 SPI stable");
                local_tembed_analyzer_skip_logged = true;
            }
            vTaskDelay(pdMS_TO_TICKS(60));
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

static esp_err_t subghz_probe_read_status_raw(uint8_t status_reg, uint8_t *hdr, uint8_t *val) {
    uint8_t tx[2] = { (uint8_t)(status_reg | 0xC0), 0x00 };
    uint8_t rx[2] = {0};
    esp_err_t err = subghz_spi_transfer(tx, rx, sizeof(tx));
    if (hdr) *hdr = rx[0];
    if (val) *val = rx[1];
    return err;
}

/* ---- Phase D: GDO0 edge probe -------------------------------------------
 * The one measurement the SPI probe never made: does GDO0 actually toggle
 * when the radio is armed in RX and a known remote is held? Independent of
 * the capture pipeline (own ISR, own counters) so it discriminates between
 * "RF never reaches the pin", "interrupt delivery broken" and "edges fine,
 * bug is downstream (thresholds / jitter)". */

#define SUBGHZ_PROBE_D_HIST_BUCKETS 9
#define SUBGHZ_PROBE_D_FIRST_MAX 16
#define SUBGHZ_PROBE_D_WINDOW_S 4
#define SUBGHZ_PROBE_D_POLL_US 30000

static volatile uint32_t s_probe_d_edges = 0;
static volatile uint32_t s_probe_d_last_us = 0;
static volatile uint32_t s_probe_d_hist[SUBGHZ_PROBE_D_HIST_BUCKETS];
static volatile uint32_t s_probe_d_min_us = 0;
static volatile uint32_t s_probe_d_max_us = 0;
static volatile uint32_t s_probe_d_first[SUBGHZ_PROBE_D_FIRST_MAX];
static volatile uint32_t s_probe_d_first_count = 0;

static const char *const s_probe_d_hist_labels[SUBGHZ_PROBE_D_HIST_BUCKETS] = {
    "<50us", "50-100", "100-200", "200-400", "400-800",
    "800-1600", "1.6-5ms", "5-30ms", ">=30ms"
};

static void subghz_probe_d_reset(void) {
    s_probe_d_edges = 0;
    s_probe_d_last_us = (uint32_t)esp_timer_get_time();
    s_probe_d_min_us = 0;
    s_probe_d_max_us = 0;
    s_probe_d_first_count = 0;
    for (int i = 0; i < SUBGHZ_PROBE_D_HIST_BUCKETS; i++) {
        s_probe_d_hist[i] = 0;
    }
}

static void IRAM_ATTR subghz_probe_d_isr(void *arg) {
    (void)arg;
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint32_t delta = now_us - s_probe_d_last_us;
    s_probe_d_last_us = now_us;
    s_probe_d_edges++;
    if (s_probe_d_edges == 1) {
        return; /* first edge has no interval */
    }

    if (s_probe_d_first_count < SUBGHZ_PROBE_D_FIRST_MAX) {
        s_probe_d_first[s_probe_d_first_count++] = delta;
    }
    if (s_probe_d_min_us == 0 || delta < s_probe_d_min_us) {
        s_probe_d_min_us = delta;
    }
    if (delta > s_probe_d_max_us) {
        s_probe_d_max_us = delta;
    }

    /* buckets: <50, 50-100, 100-200, 200-400, 400-800, 800-1600,
     * 1.6ms-5ms, 5-30ms, >=30ms */
    int bucket;
    if (delta < 50U) bucket = 0;
    else if (delta < 100U) bucket = 1;
    else if (delta < 200U) bucket = 2;
    else if (delta < 400U) bucket = 3;
    else if (delta < 800U) bucket = 4;
    else if (delta < 1600U) bucket = 5;
    else if (delta < 5000U) bucket = 6;
    else if (delta < 30000U) bucket = 7;
    else bucket = 8;
    s_probe_d_hist[bucket]++;
}

static void subghz_probe_d_run(void) {
    ESP_LOGI(TAG, "---- Phase D: GDO0 edge capture (RX armed %ds -- HOLD A KNOWN REMOTE NOW) ----",
             SUBGHZ_PROBE_D_WINDOW_S);

    gpio_config_t gdo0_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_SUBGHZ_GDO0_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&gdo0_cfg);

    if (!s_gpio_isr_service_installed) {
        /* Same LEVEL3 flags as the capture path: whoever installs the
         * service first (probe or capture) sets the session-wide level. */
        esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL3);
        if (isr_ret == ESP_OK || isr_ret == ESP_ERR_INVALID_STATE) {
            s_gpio_isr_service_installed = true;
        } else {
            ESP_LOGE(TAG, "  D abort: gpio isr service install failed: %s", esp_err_to_name(isr_ret));
            return;
        }
    }

    /* Radio is in SIDLE at the end of Phase C: flush and re-arm RX. */
    (void)cc1101_strobe(CC1101_STROBE_SFRX);
    (void)cc1101_strobe(CC1101_STROBE_SRX);

    uint8_t marc = 0xAA, rssi = 0xAA;
    (void)cc1101_read_status(CC1101_STATUS_MARCSTATE, &marc);
    (void)cc1101_read_status(CC1101_STATUS_RSSI, &rssi);
    ESP_LOGI(TAG, "  D armed: MARCSTATE=0x%02X (expect 0x0D=RX) RSSIraw=0x%02X GDO0_level=%d",
             marc, rssi, gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN));

    subghz_probe_d_reset();
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    esp_err_t intr_err = gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_ANYEDGE);
    if (intr_err == ESP_OK) {
        intr_err = gpio_isr_handler_add((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN,
                                        subghz_probe_d_isr,
                                        (void *)(intptr_t)CONFIG_SUBGHZ_GDO0_PIN);
    }
    if (intr_err == ESP_OK) {
        intr_err = gpio_intr_enable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    }
    if (intr_err != ESP_OK) {
        ESP_LOGE(TAG, "  D abort: GDO0 interrupt setup failed: %s", esp_err_to_name(intr_err));
        return;
    }

    for (int s = 1; s <= SUBGHZ_PROBE_D_WINDOW_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uint8_t m = 0, r = 0;
        (void)cc1101_read_status(CC1101_STATUS_MARCSTATE, &m);
        (void)cc1101_read_status(CC1101_STATUS_RSSI, &r);
        ESP_LOGI(TAG, "  D t=%ds: edges=%lu level=%d MARC=0x%02X RSSI=0x%02X",
                 s, (unsigned long)s_probe_d_edges,
                 gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN), m, r);
    }

    gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);

    /* Poll cross-check (KEEP HOLDING THE REMOTE): if the ISR saw zero edges but
     * a tight software poll sees the pin toggle, interrupt delivery is broken,
     * not the RF path. */
    uint32_t poll_edges = 0;
    int last_lvl = gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
    int64_t poll_end = esp_timer_get_time() + SUBGHZ_PROBE_D_POLL_US;
    while (esp_timer_get_time() < poll_end) {
        int lvl = gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
        if (lvl != last_lvl) {
            poll_edges++;
            last_lvl = lvl;
        }
        ets_delay_us(2);
    }

    (void)cc1101_strobe(CC1101_STROBE_SIDLE);

    uint32_t edges = s_probe_d_edges;
    ESP_LOGI(TAG, "  D result: isr_edges=%lu poll_edges(30ms)=%lu min=%luus max=%luus level_now=%d",
             (unsigned long)edges, (unsigned long)poll_edges,
             (unsigned long)s_probe_d_min_us, (unsigned long)s_probe_d_max_us,
             gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN));

    if (edges > 1) {
        char hbuf[256];
        size_t hpos = 0;
        hpos += snprintf(hbuf + hpos, sizeof(hbuf) - hpos, "  D histogram:");
        for (int i = 0; i < SUBGHZ_PROBE_D_HIST_BUCKETS && hpos < sizeof(hbuf) - 24; i++) {
            hpos += snprintf(hbuf + hpos, sizeof(hbuf) - hpos, " %s=%lu",
                             s_probe_d_hist_labels[i], (unsigned long)s_probe_d_hist[i]);
        }
        ESP_LOGI(TAG, "%s", hbuf);

        char fbuf[160];
        size_t fpos = 0;
        fpos += snprintf(fbuf + fpos, sizeof(fbuf) - fpos, "  D first %lu intervals (us):",
                         (unsigned long)s_probe_d_first_count);
        for (uint32_t i = 0; i < s_probe_d_first_count && fpos < sizeof(fbuf) - 12; i++) {
            fpos += snprintf(fbuf + fpos, sizeof(fbuf) - fpos, " %lu", (unsigned long)s_probe_d_first[i]);
        }
        ESP_LOGI(TAG, "%s", fbuf);
    }

    uint32_t intervals = (edges > 0) ? (edges - 1) : 0;
    uint32_t ook_like = s_probe_d_hist[2] + s_probe_d_hist[3] +
                        s_probe_d_hist[4] + s_probe_d_hist[5]; /* 100..1600us */
    bool noise_only = intervals > 0 &&
                      (s_probe_d_hist[0] + s_probe_d_hist[1]) >= (intervals * 9U / 10U);

    if (edges == 0 && poll_edges == 0) {
        ESP_LOGI(TAG, "  PHASE D VERDICT: GDO0 ELECTRICALLY SILENT (ISR and poll agree). RF never "
                      "reaches the pin -> suspect chain BEFORE decode: IOCFG0=0x0D write, FREQ word, "
                      "RX state (MARCSTATE above), antenna/band path. Decode thresholds are innocent.");
    } else if (edges == 0 && poll_edges > 0) {
        ESP_LOGI(TAG, "  PHASE D VERDICT: GDO0 TOGGLES (%lu poll edges) BUT ISR NEVER FIRED -> GPIO "
                      "interrupt delivery is broken (isr service flags / intr type / pin mux / sleep "
                      "config). RX is dead no matter what the decoder does.", (unsigned long)poll_edges);
    } else if (ook_like > 0 && !noise_only) {
        ESP_LOGI(TAG, "  PHASE D VERDICT: GDO0 ALIVE: %lu edges, %lu in OOK-like 100..1600us bands. "
                      "Datapath to the pin is GOOD -> bug is DOWNSTREAM: noise gate / accumulation "
                      "thresholds (#3) or ISR jitter under display DMA (#4, fix = RMT capture).",
                 (unsigned long)edges, (unsigned long)ook_like);
    } else {
        ESP_LOGI(TAG, "  PHASE D VERDICT: edges present (%lu) but noise-dominated (no OOK-like "
                      "widths). Wrong band for the remote, remote not held, or severe RF noise.",
                 (unsigned long)edges);
    }
}

/* Live CC1101 register access for RF experiments (AGC/bandwidth tweaks)
 * from the serial CLI without rebuilds. Requires the radio started (SubGHz
 * view open); safe to call mid-capture -- takes the same locks as retune. */
bool subghz_remote_manager_debug_reg_write(uint8_t reg, uint8_t val, uint8_t *out_readback) {
    if (!s_spi_dev || !s_radio_ready) {
        subghz_set_last_error("radio not ready");
        return false;
    }
    subghz_radio_lock();
    subghz_display_spi_hold_begin();
    esp_err_t err = cc1101_write_reg(reg, val);
    uint8_t rb = 0;
    if (err == ESP_OK) {
        (void)cc1101_read_reg(reg, &rb);
    }
    subghz_display_spi_hold_end();
    subghz_radio_unlock();
    if (err != ESP_OK) {
        subghz_set_last_error("debug reg write failed");
        return false;
    }
    if (out_readback) {
        *out_readback = rb;
    }
    return true;
}

bool subghz_remote_manager_debug_reg_read(uint8_t reg, uint8_t *out_val) {
    if (!s_spi_dev || !s_radio_ready || !out_val) {
        subghz_set_last_error("radio not ready");
        return false;
    }
    subghz_radio_lock();
    subghz_display_spi_hold_begin();
    esp_err_t err = cc1101_read_reg(reg, out_val);
    subghz_display_spi_hold_end();
    subghz_radio_unlock();
    if (err != ESP_OK) {
        subghz_set_last_error("debug reg read failed");
        return false;
    }
    return true;
}

void subghz_remote_manager_diag_probe(void) {
    if (s_subghz_task != NULL) {
        ESP_LOGW(TAG, "PROBE: capture task running; exit the SubGHz view first, then retry");
        return;
    }

    ESP_LOGI(TAG, "==== CC1101 DIAG PROBE start (board=%s) ====",
             subghz_is_tembed_c1101() ? "TEmbedC1101" : "generic");

    subghz_radio_lock();

    s_spi_host = subghz_spi_host_from_config();
    s_bus_mode = subghz_detect_bus_mode();
    s_stream_to_peer = false;
    subghz_prepare_tembed_board_pins();

    const char *bus_label =
        (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) ? "SHARED_DISPLAY" :
        (s_bus_mode == SUBGHZ_BUS_SHARED_SDCARD)  ? "SHARED_SDCARD" :
        (s_bus_mode == SUBGHZ_BUS_SHARED_NRF24)   ? "SHARED_NRF24" : "STANDALONE";
    int clk = subghz_is_tembed_c1101() ? SUBGHZ_TEMBED_SPI_CLOCK_HZ : CONFIG_SUBGHZ_SPI_CLOCK_HZ;
    ESP_LOGI(TAG, "PROBE: host=%d bus=%s MOSI=%d MISO=%d SCK=%d CSN=%d clk=%dHz manualCS=%d",
             (int)s_spi_host, bus_label,
             CONFIG_SUBGHZ_SPI_MOSI_PIN, CONFIG_SUBGHZ_SPI_MISO_PIN, CONFIG_SUBGHZ_SPI_SCK_PIN,
             CONFIG_SUBGHZ_CSN_PIN, clk, (int)subghz_is_tembed_c1101());

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = clk,
        .mode = 0,
        .spics_io_num = subghz_is_tembed_c1101() ? -1 : CONFIG_SUBGHZ_CSN_PIN,
        .queue_size = 1,
    };
    s_spi_manual_csn = subghz_is_tembed_c1101();
    s_spi_bus_initialized_by_us = false;

    esp_err_t err;
    if (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) {
        display_manager_suspend_lvgl_task();
        disp_wait_for_pending_transactions();
#ifdef CONFIG_LV_DISP_SPI_CS
        gpio_set_level((gpio_num_t)CONFIG_LV_DISP_SPI_CS, 1);
#endif
        err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
        display_manager_resume_lvgl_task();
    } else {
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
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
        }
    }
    if (err != ESP_OK || !s_spi_dev) {
        ESP_LOGE(TAG, "PROBE: spi add device failed: %s", esp_err_to_name(err));
        s_spi_dev = NULL;
        s_spi_manual_csn = false;
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(s_spi_host);
            s_spi_bus_initialized_by_us = false;
        }
        s_bus_mode = SUBGHZ_BUS_STANDALONE;
        subghz_radio_unlock();
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    err = cc1101_reset();
    ESP_LOGI(TAG, "PROBE: cc1101_reset -> %s", esp_err_to_name(err));

    const int N = 16;

    /* ---- Phase A: bus acquired ONCE, LVGL suspended for the whole window.
     * This is the electrically-clean condition: no display transactions can
     * interleave. If reads are garbage here, the fault is power/pin/reset,
     * not bus contention. ---- */
    ESP_LOGI(TAG, "---- Phase A: EXCLUSIVE (bus held once, LVGL %s) ----",
             (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) ? "suspended" : "running (STANDALONE)");
    subghz_display_spi_hold_begin();
    (void)cc1101_reset();
    int a_ok = 0;
    uint8_t a_first_ver = 0;
    bool a_have_first = false;
    bool a_stable = true;
    for (int i = 0; i < N; i++) {
        uint8_t vh = 0, vv = 0, ph = 0, pv = 0;
        (void)subghz_probe_read_status_raw(CC1101_STATUS_VERSION, &vh, &vv);
        (void)subghz_probe_read_status_raw(CC1101_STATUS_PARTNUM, &ph, &pv);
        bool ok = !((vv == 0x00 || vv == 0xFF) && (pv == 0x00 || pv == 0xFF));
        if (ok) a_ok++;
        if (!a_have_first) { a_first_ver = vv; a_have_first = true; }
        else if (vv != a_first_ver) { a_stable = false; }
        ESP_LOGI(TAG, "  A[%02d] VER hdr=0x%02X val=0x%02X | PART hdr=0x%02X val=0x%02X %s",
                 i, vh, vv, ph, pv, ok ? "" : "<-- BAD");
    }
    uint8_t wf2 = 0x10, wf1 = 0xA7, wf0 = 0x62;
    uint8_t rf2 = 0xAA, rf1 = 0xAA, rf0 = 0xAA;
    (void)cc1101_write_reg(CC1101_REG_FREQ2, wf2);
    (void)cc1101_write_reg(CC1101_REG_FREQ1, wf1);
    (void)cc1101_write_reg(CC1101_REG_FREQ0, wf0);
    (void)cc1101_read_reg(CC1101_REG_FREQ2, &rf2);
    (void)cc1101_read_reg(CC1101_REG_FREQ1, &rf1);
    (void)cc1101_read_reg(CC1101_REG_FREQ0, &rf0);
    bool wb_ok = (rf2 == wf2 && rf1 == wf1 && rf0 == wf0);
    ESP_LOGI(TAG, "  A write/readback FREQ wrote %02X%02X%02X read %02X%02X%02X %s",
             wf2, wf1, wf0, rf2, rf1, rf0, wb_ok ? "OK" : "<-- MISMATCH");
    subghz_display_spi_hold_end();
    ESP_LOGI(TAG, "  Phase A summary: %d/%d good, version_stable=%d, write_readback=%d",
             a_ok, N, (int)a_stable, (int)wb_ok);

    /* ---- Phase B: the REAL per-op transfer path with LVGL running. Each read
     * acquires+releases the bus and suspend/resumes LVGL on its own (exactly
     * what capture does). The delay lets the display flusher interleave between
     * ops. If A is solid but B is flaky, the bug is in the per-op churn. ---- */
    ESP_LOGI(TAG, "---- Phase B: PER-OP (LVGL running, real transfer path) ----");
    int b_ok = 0;
    for (int i = 0; i < N; i++) {
        uint8_t v = 0, p = 0;
        esp_err_t e1 = cc1101_read_status(CC1101_STATUS_VERSION, &v);
        esp_err_t e2 = cc1101_read_status(CC1101_STATUS_PARTNUM, &p);
        bool ok = (e1 == ESP_OK && e2 == ESP_OK) &&
                  !((v == 0x00 || v == 0xFF) && (p == 0x00 || p == 0xFF));
        if (ok) b_ok++;
        ESP_LOGI(TAG, "  B[%02d] VER=0x%02X PART=0x%02X e=%s/%s %s",
                 i, v, p, esp_err_to_name(e1), esp_err_to_name(e2), ok ? "" : "<-- BAD");
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    ESP_LOGI(TAG, "  Phase B summary: %d/%d good", b_ok, N);

    /* ---- Phase C: replicate the ARM config sequence and read VERSION after
     * each step to pinpoint where status reads go bad. Expected on TEmbed:
     * VERSION stays 0x14 through config, then goes 0x00 after SRX -- exactly
     * where subghz_hw_start historically ran its detection check. ---- */
    ESP_LOGI(TAG, "---- Phase C: replicate ARM sequence, watch VERSION per step ----");
    s_current_freq_hz = 433920000U;
    uint8_t cver = 0xAA;
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    (void)cc1101_strobe(CC1101_STROBE_SFRX);
    (void)cc1101_strobe(CC1101_STROBE_SFTX);
    subghz_apply_board_rf_switch(s_current_freq_hz);
    (void)subghz_apply_preset(SUBGHZ_PRESET_OOK650_ASYNC);
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &cver);
    ESP_LOGI(TAG, "  C after preset(OOK650):  VER=0x%02X", cver);
    (void)subghz_apply_tembed_local_rx_settings();
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &cver);
    ESP_LOGI(TAG, "  C after rx_settings:     VER=0x%02X", cver);
    (void)subghz_configure_capture_gdo_outputs();
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &cver);
    ESP_LOGI(TAG, "  C after gdo_cfg:         VER=0x%02X", cver);
    /* Program the carrier exactly like subghz_retune_frequency(): without this
     * the probe listened on the POR/test FREQ and Phase D was invalid. */
    uint32_t c_freq_word = (uint32_t)((((uint64_t)s_current_freq_hz) * 65536ULL) / 26000000ULL);
    (void)cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((c_freq_word >> 16) & 0xFF));
    (void)cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((c_freq_word >> 8) & 0xFF));
    (void)cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(c_freq_word & 0xFF));
    (void)subghz_apply_tembed_freq_calibration(s_current_freq_hz);
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &cver);
    ESP_LOGI(TAG, "  C after freq_cal:        VER=0x%02X", cver);

    /* Full RX register dump (reads are reliable here, pre-SRX) for diffing
     * against the known-good working state: IOCFG0/2=0x0D PKTCTRL0=0x32 PKTCTRL1=0x04
     * FREQ=0x10B071 FSCTRL0=0x23 FSCTRL1=0x06 MDMCFG0/1=0x00 MDMCFG2=0x30
     * MDMCFG3=0x32 MDMCFG4=0x17(OOK650) DEVIATN=0x47 MCSM0=0x18 FOCCFG=0x18
     * BSCFG=0x1C AGC=0x40/0x01/0xC7 FREND0=0x11 FREND1=0xB6 FSCAL=0xE9/0x2A/
     * 0x00/0x1F FSTEST=0x59 TEST2=0x81 TEST1=0x35 TEST0=0x09 FIFOTHR=0x07 */
    {
        uint8_t r[24];
        const uint8_t regs[24] = {
            CC1101_REG_IOCFG0, CC1101_REG_IOCFG2, CC1101_REG_PKTCTRL0, CC1101_REG_PKTCTRL1,
            CC1101_REG_FREQ2, CC1101_REG_FREQ1, CC1101_REG_FREQ0,
            CC1101_REG_FSCTRL0, CC1101_REG_FSCTRL1,
            CC1101_REG_MDMCFG0, CC1101_REG_MDMCFG1, CC1101_REG_MDMCFG2,
            CC1101_REG_MDMCFG3, CC1101_REG_MDMCFG4,
            CC1101_REG_DEVIATN, CC1101_REG_MCSM0, CC1101_REG_FOCCFG, CC1101_REG_BSCFG,
            CC1101_REG_AGCCTRL0, CC1101_REG_AGCCTRL1, CC1101_REG_AGCCTRL2,
            CC1101_REG_FREND0, CC1101_REG_FREND1, CC1101_REG_FIFOTHR
        };
        for (int k = 0; k < 24; k++) {
            r[k] = 0xEE;
            (void)cc1101_read_reg(regs[k], &r[k]);
        }
        ESP_LOGI(TAG, "  C dump1: IOCFG0=%02X IOCFG2=%02X PKT0=%02X PKT1=%02X FREQ=%02X%02X%02X FS0=%02X FS1=%02X",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8]);
        ESP_LOGI(TAG, "  C dump2: MDM0=%02X MDM1=%02X MDM2=%02X MDM3=%02X MDM4=%02X DEV=%02X MCSM0=%02X FOC=%02X BSC=%02X",
                 r[9], r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]);
        ESP_LOGI(TAG, "  C dump3: AGC0=%02X AGC1=%02X AGC2=%02X FREND0=%02X FREND1=%02X FIFOTHR=%02X",
                 r[18], r[19], r[20], r[21], r[22], r[23]);
    }

    (void)cc1101_strobe(CC1101_STROBE_SIDLE);
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &cver);
    ESP_LOGI(TAG, "  C after SIDLE:           VER=0x%02X", cver);
    (void)cc1101_strobe(CC1101_STROBE_SRX);
    uint8_t c_srx_ver = 0xAA, c_srx_part = 0xAA;
    (void)cc1101_read_status(CC1101_STATUS_VERSION, &c_srx_ver);
    (void)cc1101_read_status(CC1101_STATUS_PARTNUM, &c_srx_part);
    ESP_LOGI(TAG, "  C after SRX:             VER=0x%02X PART=0x%02X  <-- arm path detected HERE",
             c_srx_ver, c_srx_part);
    (void)cc1101_strobe(CC1101_STROBE_SIDLE);

    /* ---- Phase D: the missing measurement. Radio is configured exactly as
     * the capture path leaves it; count GDO0 edges + histogram pulse widths
     * while the user holds a known remote. ---- */
    subghz_probe_d_run();

    ESP_LOGI(TAG, "==== PROBE VERDICT ====");
    bool srx_read_bad = (c_srx_ver == 0x00 || c_srx_ver == 0xFF);
    if (a_ok == N && wb_ok && b_ok == N && srx_read_bad) {
        ESP_LOGI(TAG, "CONFIRMED: chip+SPI perfect (VER=0x14 in IDLE) but status reads return "
                      "0x%02X after SRX. The arm-path detection ran AFTER SRX -> false 'cc1101 not "
                      "detected'. Fix = detect in IDLE before SRX (now applied in subghz_hw_start).",
                 c_srx_ver);
    } else if (a_ok == N && wb_ok && b_ok == N) {
        ESP_LOGI(TAG, "SPI link SOLID all phases incl. post-SRX (VER=0x%02X). Detection ordering is "
                      "NOT the cause -> chase the RX capture datapath (GDO0 edges / decode).", c_srx_ver);
    } else if (a_ok == N && wb_ok && b_ok < N) {
        ESP_LOGI(TAG, "EXCLUSIVE solid but PER-OP flaky (%d/%d) -> per-op bus/CS churn while the "
                      "display bus is live. Hold the bus across the whole arm+capture window.", b_ok, N);
    } else {
        ESP_LOGI(TAG, "EXCLUSIVE reads unstable (A=%d/%d wb=%d) -> electrical: power sequencing, "
                      "floating MISO during wait-ready, or reset timing.", a_ok, N, (int)wb_ok);
    }

    if (s_spi_dev) {
        (void)cc1101_strobe(CC1101_STROBE_SIDLE);
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }
    s_spi_manual_csn = false;
    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
    }
    s_display_spi_hold_depth = 0;
    subghz_display_spi_restore();
    s_bus_mode = SUBGHZ_BUS_STANDALONE;
    subghz_radio_unlock();
    ESP_LOGI(TAG, "==== CC1101 DIAG PROBE done ====");
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
    if (s_stream_to_peer && esp_comm_manager_is_connected()) {
        s_raw_capture_pending = false;
    }
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
    if (s_local_decode_result_ready) {
        *out_result = s_local_decode_result;
        s_local_decode_result_ready = false;
        memset(&s_local_decode_result, 0, sizeof(s_local_decode_result));
        return true;
    }
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
    s_bus_mode = subghz_detect_bus_mode();
    subghz_prepare_tembed_board_pins();

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

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = subghz_is_tembed_c1101() ? SUBGHZ_TEMBED_SPI_CLOCK_HZ : CONFIG_SUBGHZ_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = subghz_is_tembed_c1101() ? -1 : CONFIG_SUBGHZ_CSN_PIN,
        .queue_size = 1,
    };
    s_spi_manual_csn = subghz_is_tembed_c1101();

    if (s_bus_mode == SUBGHZ_BUS_SHARED_DISPLAY) {
        s_spi_bus_initialized_by_us = false;
        subghz_display_spi_suspend();
        err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
        if (err != ESP_OK) {
            subghz_set_last_error("TX shared display spi add device failed");
            subghz_display_spi_restore();
            return false;
        }
        ESP_LOGI(TAG, "TX using shared display SPI bus (host=%d)", (int)s_spi_host);
    } else if (s_bus_mode == SUBGHZ_BUS_SHARED_SDCARD) {
        s_spi_bus_initialized_by_us = false;
        err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_spi_bus_initialized_by_us = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized_by_us = false;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            subghz_set_last_error("TX shared sdcard spi bus init failed");
            return false;
        }
        ESP_LOGI(TAG, "TX using shared SD card SPI bus (host=%d)", (int)s_spi_host);
    } else if (s_bus_mode == SUBGHZ_BUS_SHARED_NRF24) {
        s_spi_bus_initialized_by_us = false;
        err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_spi_bus_initialized_by_us = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized_by_us = false;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            subghz_set_last_error("TX shared nrf24 spi bus init failed");
            return false;
        }
        ESP_LOGI(TAG, "TX using shared NRF24 SPI bus (host=%d)", (int)s_spi_host);
    } else {
        err = spi_bus_initialize(s_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err == ESP_OK) {
            s_spi_bus_initialized_by_us = true;
        } else if (err == ESP_ERR_INVALID_STATE) {
            s_spi_bus_initialized_by_us = false;
            err = ESP_OK;
        }
        if (err != ESP_OK) {
            subghz_set_last_error("TX spi bus init failed");
            return false;
        }
    }

    if (s_bus_mode != SUBGHZ_BUS_SHARED_DISPLAY) {
        err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
        if (err != ESP_OK) {
            subghz_set_last_error("TX add spi device failed");
            if (s_spi_bus_initialized_by_us) {
                spi_bus_free(s_spi_host);
                s_spi_bus_initialized_by_us = false;
            }
            s_spi_dev = NULL;
            return false;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(2));
    err = cc1101_reset();
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

    if (frequency_hz == 0) {
        frequency_hz = (uint32_t)((uint64_t)CONFIG_SUBGHZ_BASE_FREQ_MHZ * 10000ULL);
    }
    s_current_freq_hz = frequency_hz;
    err = subghz_apply_preset(preset);

    subghz_apply_board_rf_switch(frequency_hz);
    if (err == ESP_OK) err = subghz_apply_tembed_freq_calibration(frequency_hz);
    uint64_t f_hz = (uint64_t)frequency_hz;
    uint32_t freq_word = (uint32_t)((f_hz * 65536ULL) / 26000000ULL);
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ2, (uint8_t)((freq_word >> 16) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ1, (uint8_t)((freq_word >> 8) & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(CC1101_REG_FREQ0, (uint8_t)(freq_word & 0xFF));
    if (err == ESP_OK) err = cc1101_write_reg(0x0A, 0x00);

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
        subghz_capture_pm_lock_acquire();
        s_raw_capture_enabled = true;
        s_raw_active = false;
        s_raw_ready = false;
        s_raw_ignore_until_us = !s_stream_to_peer ? ((uint32_t)esp_timer_get_time() + SUBGHZ_RAW_SETTLE_US) : 0;
        s_raw_worklen = 0;
        s_raw_capture_gpio = -1;
        s_raw_capture_pending = false;
        s_raw_local_signal_seen = false;
        s_tembed_local_capture = subghz_is_tembed_c1101() && !s_stream_to_peer;
        if (s_tembed_local_capture && SUBGHZ_TEMBED_PAUSE_DISPLAY_DURING_CAPTURE) {
            subghz_display_status_updates_set_enabled(false);
        }
        s_raw_timeout_poll_mode = s_tembed_local_capture && s_capture_raw_mode_active;
        s_raw_waiting_first_edge = false;
        s_raw_last_time_us = (uint32_t)esp_timer_get_time();
        subghz_reset_local_rcswitch_state();
        s_capture_isr_edges = 0;
        s_capture_isr_accepted = 0;
        s_capture_isr_glitches = 0;
        s_capture_diag_last_isr_edges = 0;
        s_capture_diag_last_pin_edges = 0;
        s_local_prev_accept_valid = false;
        s_local_prev_accept_was_gap = false;
        s_raw_prev_level = gpio_get_level((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);

        bool use_rmt = s_tembed_rx_use_rmt &&
                       (s_tembed_local_capture && !s_capture_raw_mode_active);
        if (use_rmt) {
            if (!subghz_rmt_rx_start()) {
                ESP_LOGE(TAG, "RMT RX start failed, falling back to GPIO ISR");
                use_rmt = false;
            }
        }
        if (!use_rmt) {
            esp_err_t intr_err = gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_ANYEDGE);
            if (intr_err == ESP_OK) {
                intr_err = gpio_intr_enable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
            }
            if (intr_err != ESP_OK) {
                ESP_LOGE(TAG, "GDO0 interrupt enable failed: %s", esp_err_to_name(intr_err));
            }
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
            if (subghz_should_watch_gdo2_capture()) {
                intr_err = gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN, GPIO_INTR_ANYEDGE);
                if (intr_err == ESP_OK) {
                    intr_err = gpio_intr_enable((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
                }
                if (intr_err != ESP_OK) {
                    ESP_LOGE(TAG, "GDO2 interrupt enable failed: %s", esp_err_to_name(intr_err));
                }
                ESP_LOGI(TAG, "raw capture enabled (watching GDO0+GDO2 ANYEDGE)");
            } else {
                ESP_LOGI(TAG, "raw capture enabled (GDO0 CHANGE)");
            }
#else
            ESP_LOGI(TAG, "raw capture enabled (GDO0 CHANGE)");
#endif
        }
    } else if (!enabled && s_raw_capture_enabled) {
        if (s_rmt_rx_active) {
            subghz_rmt_rx_stop();
        } else {
            gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN);
            gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO0_PIN, GPIO_INTR_DISABLE);
#if CONFIG_SUBGHZ_GDO2_PIN >= 0
            if (subghz_should_watch_gdo2_capture()) {
                gpio_intr_disable((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN);
                gpio_set_intr_type((gpio_num_t)CONFIG_SUBGHZ_GDO2_PIN, GPIO_INTR_DISABLE);
            }
#endif
        }
        if (s_raw_timeout_timer) {
            esp_timer_stop(s_raw_timeout_timer);
        }

        bool local_rcswitch_normal = s_tembed_local_capture && !s_capture_raw_mode_active;
        if (local_rcswitch_normal && !s_raw_capture_pending && s_local_rc_change_count > 0) {
            size_t count = s_local_rc_change_count;
            if (count > SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES) {
                count = SUBGHZ_LOCAL_RCSWITCH_MAX_CHANGES;
            }
            for (size_t i = 0; i < count; i++) {
                s_local_rc_decode_work[i] = s_local_rc_timings[i];
            }
            subghz_publish_local_rcswitch_frame(s_local_rc_decode_work, count);
        } else if (s_raw_active) {
            subghz_finalize_raw_capture(true);
            ESP_LOGI(TAG,
                     "raw capture flush on disable: pin=%d transitions=%lu ready=%d",
                     s_raw_capture_gpio,
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
        s_raw_ignore_until_us = 0;
        s_raw_worklen = 0;
        s_raw_capture_gpio = -1;
        s_raw_timeout_poll_mode = false;
        s_raw_waiting_first_edge = false;
        if (s_tembed_local_capture) {
            subghz_display_status_updates_set_enabled(true);
        }
        s_tembed_local_capture = false;
        subghz_capture_pm_lock_release();
        ESP_LOGI(TAG, "raw capture disabled (GDO intr DISABLE)");
    }
}

bool subghz_remote_manager_is_tembed_local_capture(void) {
    return s_tembed_local_capture;
}

void subghz_remote_manager_set_tembed_rx_use_rmt(bool use_rmt) {
    s_tembed_rx_use_rmt = use_rmt;
}

bool subghz_remote_manager_get_tembed_rx_use_rmt(void) {
    return s_tembed_rx_use_rmt;
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
void subghz_remote_manager_diag_probe(void) {}
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
bool subghz_remote_manager_is_tembed_local_capture(void) { return false; }
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
