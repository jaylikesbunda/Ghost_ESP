// lora_sx1262.c — bare-metal SX1262 driver (no RadioLib).
// Sync 0x442B, region slot freq, and sensitivity patch match upstream.

#include "managers/lora_sx1262.h"
#include "managers/lora_modem.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"
#include <string.h>

static const char *TAG = "SX1262";

// Opcodes
#define OP_GET_STATUS        0xC0
#define OP_WRITE_REG         0x0D
#define OP_READ_REG          0x1D
#define OP_WRITE_BUF         0x0E
#define OP_READ_BUF          0x1E
#define OP_SET_SLEEP         0x84
#define OP_SET_STANDBY       0x80
#define OP_SET_FS            0xC1
#define OP_SET_TX            0x83
#define OP_SET_RX            0x82
#define OP_SET_CAD           0xC5
#define OP_SET_REGULATOR     0x96
#define OP_CALIBRATE         0x89
#define OP_CAL_IMAGE         0x98
#define OP_SET_PA            0x95
#define OP_SET_DIO_IRQ       0x08
#define OP_GET_IRQ           0x12
#define OP_CLR_IRQ           0x02
#define OP_SET_DIO2_RF       0x9D
#define OP_SET_DIO3_TCXO     0x97
#define OP_SET_RFFREQ        0x86
#define OP_SET_PKT_TYPE      0x8A
#define OP_SET_TX_PARAMS     0x8E
#define OP_SET_MOD_PARAMS    0x8B
#define OP_SET_PKT_PARAMS    0x8C
#define OP_SET_CAD_PARAMS    0x88
#define OP_SET_BUF_BASE      0x8F
#define OP_GET_RSSI_INST     0x15
#define OP_GET_RXBUF         0x13
#define OP_GET_PKT_STATUS    0x14
#define OP_GET_DEV_ERRORS    0x17
#define OP_CLR_DEV_ERRORS    0x07

#define IRQ_TX_DONE   0x0001
#define IRQ_RX_DONE   0x0002
#define IRQ_CRC_ERR   0x0040
#define IRQ_CAD_DONE  0x0080
#define IRQ_CAD_DET   0x0100
#define IRQ_TIMEOUT   0x0200
#define IRQ_ALL       0xFFFF

#define PKT_TYPE_LORA 0x01
#define STANDBY_RC    0x00
#define REGULATOR_DCDC 0x01

static spi_device_handle_t s_dev = NULL;
static spi_host_device_t s_host = SPI2_HOST;
static bool s_bus_ours = false;

static void lora_module_enable(bool enabled) {
#if CONFIG_LORA_ENABLE_PIN >= 0
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_LORA_ENABLE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) == ESP_OK) {
#if CONFIG_LORA_ENABLE_ACTIVE_LOW
        gpio_set_level((gpio_num_t)CONFIG_LORA_ENABLE_PIN, enabled ? 0 : 1);
#else
        gpio_set_level((gpio_num_t)CONFIG_LORA_ENABLE_PIN, enabled ? 1 : 0);
#endif
    }
#else
    (void)enabled;
#endif
}
static lora_hw_t s_hw;
static uint32_t s_freq;
static int s_sf, s_bw, s_tx, s_cr;
static volatile bool s_ready = false;
static volatile bool s_rx_on = false;
static TaskHandle_t s_task = NULL;
static lora_rx_cb_t s_cb = NULL;
static void *s_ctx = NULL;
static SemaphoreHandle_t s_mutex = NULL;
/* Keep all SPI transaction buffers internal and aligned for DMA.  Passing
 * automatic task-stack arrays to spi_master forces an allocated private DMA
 * copy for every packet, fragmenting the small internal heap on the Advance
 * 2.4/2.8 until the next transfer cannot allocate its TX buffer. */
static uint8_t s_tx_buf[256] __attribute__((aligned(4)));
static uint8_t s_rx_buf[256] __attribute__((aligned(4)));
static uint8_t s_spi_tx[256] __attribute__((aligned(4)));
static uint8_t s_spi_rx[256] __attribute__((aligned(4)));
static StaticSemaphore_t s_rx_exit_sem_buf;
static SemaphoreHandle_t s_rx_exit_sem;
// Regional TX cap (lora_modem_power_limit); init_ex clamps through it.
// is_licensed bypasses the region cap (HAM) but never the 22dBm ceiling.
static int s_tx_region = 1; // US915 default
static bool s_tx_licensed = false;

void lora_radio_set_region_tx_limit(int region_code, bool is_licensed) {
    s_tx_region = region_code;
    s_tx_licensed = is_licensed;
    ESP_LOGI(TAG, "region %d TX limit %ddBm%s", region_code,
             lora_modem_power_limit(region_code),
             is_licensed ? " (licensed bypass)" : "");
}

int lora_radio_clamp_tx_dbm(int region_code, int tx_dbm, bool is_licensed) {
    return lora_modem_clamp_tx(region_code, tx_dbm, is_licensed);
}

static void lock(void) { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
static void unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

// Last failed bring-up stage (see RCHECK below). Surfaced via
// lora_radio_step() so `lora start` failures name themselves.
static const char *s_fail_step = "none";
#define RCHECK(call, name) do { s_fail_step = (name); if ((call) != 0) goto fail; } while (0)

const char *lora_radio_step(void) { return s_fail_step; }

static bool wait_busy(int timeout_ms) {
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        if (gpio_get_level((gpio_num_t)s_hw.busy_pin) == 0) return true;
        esp_rom_delay_us(100);
    }
    return false;
}

static int xfer(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_dev || !tx || len == 0 || len > sizeof(s_tx_buf)) return -1;
    if (!wait_busy(100)) return -1;
    size_t wire_len = (len + 3u) & ~3u;
    if (wire_len > sizeof(s_spi_tx)) return -1;
    memcpy(s_spi_tx, tx, len);
    if (wire_len > len) memset(s_spi_tx + len, 0, wire_len - len);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    /* GDMA requires an aligned transfer length.  Padding here keeps the
     * driver from allocating a temporary private TX/RX buffer per command;
     * SX126x ignores trailing command clocks, and WriteBuffer's packet length
     * is set separately. */
    t.length = (uint32_t)(wire_len * 8);
    t.tx_buffer = s_spi_tx;
    t.rx_buffer = s_spi_rx;
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) return -1;
    if (rx) memcpy(rx, s_spi_rx, len);
    if (!wait_busy(100)) return -1;
    return 0;
}

/* WriteRegister has a true variable-length payload: trailing clocks would
 * write adjacent registers, so keep those three bring-up writes exact. */
static int xfer_exact(const uint8_t *tx, uint8_t *rx, size_t len) {
    if (!s_dev || !tx || len == 0 || len > sizeof(s_tx_buf)) return -1;
    if (!wait_busy(100)) return -1;
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = (uint32_t)(len * 8);
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    if (spi_device_polling_transmit(s_dev, &t) != ESP_OK) return -1;
    return wait_busy(100) ? 0 : -1;
}

static int cmd1(uint8_t op) {
    uint8_t tx[1] = {op}, rx[1] = {0};
    return xfer(tx, rx, 1);
}
static int cmdN(uint8_t op, const uint8_t *p, uint8_t n) {
    // NOTE: longest stock command is SetPacketParams at 9 payload bytes;
    // keep headroom for future commands (DIO_IRQ uses 8).
    uint8_t tx[1 + 16]; uint8_t rx[1 + 16];
    if (n > 16) return -1;
    tx[0] = op; memcpy(&tx[1], p, n);
    return xfer(tx, rx, (size_t)(1 + n));
}
static int cmdN_resp(uint8_t op, const uint8_t *p, uint8_t n, uint8_t *out, uint8_t out_n) {
    // SX126x read commands are command + params + NOP/status + response.
    // The status byte is clocked during the NOP and is not response data.
    uint8_t tx[2 + 8 + 8]; uint8_t rx[2 + 8 + 8];
    if (n > 8 || out_n > 8) return -1;
    tx[0] = op;
    if (n > 0) {
        if (!p) return -1;
        memcpy(&tx[1], p, n);
    }
    memset(&tx[1 + n], 0, (size_t)(1 + out_n));
    if (xfer(tx, rx, (size_t)(2 + n + out_n)) != 0) return -1;
    memcpy(out, &rx[2 + n], out_n);
    return 0;
}

// Raw register write (WriteRegister 0x0D: addr16 BE + values). Caller holds lock.
static int wr_reg(uint16_t addr, const uint8_t *vals, uint8_t n) {
    uint8_t tx[3 + 8]; uint8_t rx[3 + 8];
    if (n > 8 || !vals) return -1;
    tx[0] = OP_WRITE_REG;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    memcpy(&tx[3], vals, n);
    return xfer_exact(tx, rx, (size_t)(3 + n));
}

// Raw register read (ReadRegister 0x1D: addr16 BE + NOP/status + values).
// Caller holds lock.
static int rd_reg(uint16_t addr, uint8_t *vals, uint8_t n) {
    uint8_t tx[4 + 8] = {0};
    uint8_t rx[4 + 8] = {0};
    if (n == 0 || n > 8 || !vals) return -1;
    tx[0] = OP_READ_REG;
    tx[1] = (uint8_t)(addr >> 8);
    tx[2] = (uint8_t)addr;
    // tx[3] clocks the status byte; register data starts at rx[4].
    if (xfer(tx, rx, (size_t)(4 + n)) != 0) return -1;
    memcpy(vals, &rx[4], n);
    return 0;
}

static uint8_t bw_reg(int bw_khz) {
    // SX126x SetModulationParams BW byte (datasheet + RadioLib:
    // 125kHz=0x04, 250=0x05, 500=0x06). The old 0x38/0x48/0x58 values
    // belong to no SX126x table — the modem never actually ran BW250,
    // so stock nodes could neither hear us nor be heard.
    if (bw_khz >= 500) return 0x06;
    if (bw_khz >= 250) return 0x05;
    return 0x04; // 125
}

static void dio1_isr(void *arg) {
    (void)arg;
    if (s_task) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s_task, &hp);
        if (hp) portYIELD_FROM_ISR();
    }
}

static void clear_irq(uint16_t mask) {
    uint8_t p[2] = {(uint8_t)(mask >> 8), (uint8_t)(mask & 0xFF)};
    cmdN(OP_CLR_IRQ, p, 2);
}

// LoRa packet parameters are six bytes on SX126x.  The payload length must be
// changed to the exact frame length before TX, then restored to 255 for
// explicit-header RX.  RadioLib follows the same sequence.
static int set_lora_packet_params(uint8_t payload_len) {
    uint8_t p[6] = {
        0x00, 0x10, // 16-symbol preamble
        0x00,       // explicit header
        payload_len,
        0x01,       // CRC on
        0x00,       // standard IQ
    };
    return cmdN(OP_SET_PKT_PARAMS, p, sizeof(p));
}

// Background hook for mesh housekeeping (NodeInfo timer). Weak: mesh.c
// provides the real one; driver stays layer-clean without the include.
__attribute__((weak)) void lora_bg_tick(void) {}

static int set_rx_continuous(void) {
    if (set_lora_packet_params(0xFF) != 0) return -1;
    // SetRx with timeout 0xFFFFFF = continuous RX.
    uint8_t p[3] = {0xFF, 0xFF, 0xFF};
    return cmdN(OP_SET_RX, p, 3);
}

static void radio_task(void *arg) {
    (void)arg;
    uint8_t irq[2];
    while (s_rx_on) {
        // Wake on DIO1 or 200ms poll (covers missed edges on shared GPIO).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        if (!s_rx_on) break;
        // Background tick (NodeInfo schedule, etc.): runs unlocked in task
        // context; implementors must use the radio API (which locks).
        lora_bg_tick();
        if (!s_rx_on) break;
        lock();
        if (cmdN_resp(OP_GET_IRQ, NULL, 0, irq, 2) != 0) {
            unlock();
            continue;
        }
        uint16_t flags = (uint16_t)((irq[0] << 8) | irq[1]);
        if (flags == 0) {
            unlock();
            continue;
        }
        // Keep the raw SX1262 event visible while validating interoperability.
        // A valid RX_DONE proves the RF/IRQ path works even if the higher-level
        // Meshtastic frame is rejected by the channel/key/parser.
        ESP_LOGI(TAG, "IRQ flags=0x%04X%s%s%s%s",
                 (unsigned)flags,
                 (flags & IRQ_RX_DONE) ? " RX_DONE" : "",
                 (flags & IRQ_TX_DONE) ? " TX_DONE" : "",
                 (flags & IRQ_CRC_ERR) ? " CRC_ERR" : "",
                 (flags & IRQ_TIMEOUT) ? " TIMEOUT" : "");
        if (flags & IRQ_RX_DONE) {
            uint8_t st[2] = {0};
            uint8_t ps[3] = {0};
            uint8_t payload_len = 0;
            int16_t rssi = 0;
            float snr = 0;
            bool ok = false;
            if (cmdN_resp(OP_GET_RXBUF, NULL, 0, st, 2) == 0) {
                payload_len = st[0];
                uint8_t off = st[1];
                if (payload_len > 0 && payload_len <= 240) {
                    s_spi_tx[0] = OP_READ_BUF;
                    s_spi_tx[1] = off;
                    s_spi_tx[2] = 0x00;
                    // Direct transfer (already hold mutex; inline, no busy re-lock issue:
                    // xfer takes no mutex itself).
                    if (s_dev && wait_busy(100)) {
                        spi_transaction_t t;
                        memset(&t, 0, sizeof(t));
                        size_t wire_len = ((size_t)3 + payload_len + 3u) & ~3u;
                        memset(s_spi_tx + 3 + payload_len, 0,
                               wire_len - ((size_t)3 + payload_len));
                        t.length = (uint32_t)(wire_len * 8);
                        t.tx_buffer = s_spi_tx;
                        t.rx_buffer = s_spi_rx;
                        if (spi_device_polling_transmit(s_dev, &t) == ESP_OK && wait_busy(100)) {
                            memcpy(s_rx_buf, &s_spi_rx[3], payload_len);
                            ok = true;
                        }
                    }
                }
            }
            if (cmdN_resp(OP_GET_PKT_STATUS, NULL, 0, ps, 3) == 0) {
                // RadioLib unsigned convention: RSSI = -raw/2 dBm.
                rssi = -(int16_t)ps[0] / 2;
                snr = ((int8_t)ps[1]) / 4.0f;
            }
            bool crc = (flags & IRQ_CRC_ERR) != 0;
            ESP_LOGI(TAG, "RX status raw=%02X%02X len=%u off=%u rssi=%d snr=%.1f crc=%u read=%u",
                     st[0], st[1], (unsigned)payload_len, (unsigned)(st[1]), (int)rssi,
                     (double)snr, (unsigned)crc, (unsigned)ok);
            clear_irq(IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_TIMEOUT);
            lora_rx_cb_t cb = s_cb;
            void *ctx = s_ctx;
            unlock();
            if (ok && !crc && cb && payload_len > 0) {
                cb(s_rx_buf, payload_len, rssi, snr, ctx);
            }
            lock();
            set_rx_continuous();
            unlock();
            continue;
        }
        if (flags & (IRQ_TIMEOUT | IRQ_CRC_ERR)) {
            clear_irq(IRQ_TIMEOUT | IRQ_CRC_ERR);
            set_rx_continuous();
        }
        unlock();
    }
    s_task = NULL;
    if (s_rx_exit_sem) xSemaphoreGive(s_rx_exit_sem);
    vTaskDelete(NULL);
}

int lora_radio_init(const lora_hw_t *hw, uint32_t freq_hz, int sf, int bw_khz, int tx_dbm) {
    return lora_radio_init_ex(hw, freq_hz, sf, bw_khz, tx_dbm, 5);
}

int lora_radio_init_ex(const lora_hw_t *hw, uint32_t freq_hz, int sf, int bw_khz, int tx_dbm, int cr_denom) {
    if (!hw || freq_hz < 150000000U || freq_hz > 960000000U) return -1;
    if (sf < 5 || sf > 12) return -1;
    if (cr_denom < 5) cr_denom = 5;
    if (cr_denom > 8) cr_denom = 8;
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    lock();
    s_hw = *hw;
    s_freq = freq_hz; s_sf = sf; s_bw = bw_khz; s_tx = tx_dbm; s_cr = cr_denom;
    s_fail_step = "params";

    // Elecrow Advance 2.4/2.8 boards multiplex GPIO45 between the
    // microphone rail and the plug-in wireless module. Select the module
    // before touching its SPI pins; integrated radios leave this disabled.
    lora_module_enable(true);

#if CONFIG_LORA_VEXT_PIN >= 0
    // Power the radio rail first (e.g. Heltec Vext): probing an unpowered
    // chip fails every BUSY wait below with no other symptom.
    {
        s_fail_step = "vext";
        gpio_config_t vext = {
            .pin_bit_mask = (1ULL << CONFIG_LORA_VEXT_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&vext) != ESP_OK) {
            lora_module_enable(false);
            unlock();
            return -1;
        }
#if CONFIG_LORA_VEXT_ACTIVE_LOW
        gpio_set_level((gpio_num_t)CONFIG_LORA_VEXT_PIN, 0);
#else
        gpio_set_level((gpio_num_t)CONFIG_LORA_VEXT_PIN, 1);
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
    }
#endif

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << hw->nss_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)hw->nss_pin, 1);
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << hw->dio1_pin) | (1ULL << hw->busy_pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&in) != ESP_OK) {
        s_fail_step = "gpio-in";
        lora_module_enable(false);
        unlock();
        return -1;
    }
    if (hw->rst_pin >= 0) {
        gpio_config_t ro = {
            .pin_bit_mask = (1ULL << hw->rst_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&ro);
        gpio_set_level((gpio_num_t)hw->rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level((gpio_num_t)hw->rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_host = (hw->spi_host == 3) ? SPI3_HOST : SPI2_HOST;
    spi_bus_config_t bus = {
        .mosi_io_num = hw->mosi_pin,
        .miso_io_num = hw->miso_pin,
        .sclk_io_num = hw->sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* SX126x frames are capped at 240 bytes; command + address/status
         * overhead stays below 256.  Keeping this tight reduces the DMA
         * descriptor/private-buffer footprint on small-panel builds. */
        .max_transfer_sz = 256,
    };
    esp_err_t e = spi_bus_initialize(s_host, &bus, SPI_DMA_CH_AUTO);
    if (e == ESP_OK) s_bus_ours = true;
    else if (e != ESP_ERR_INVALID_STATE) {
        s_fail_step = "spi-bus";
        lora_module_enable(false);
        unlock();
        return -1;
    }
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 4000000,
        .mode = 0,
        .spics_io_num = hw->nss_pin,
        .queue_size = 1,
    };
    if (spi_bus_add_device(s_host, &dev, &s_dev) != ESP_OK) {
        s_fail_step = "spi-device";
        if (s_bus_ours) { spi_bus_free(s_host); s_bus_ours = false; }
        lora_module_enable(false);
        unlock();
        return -1;
    }

    // Bring-up per datasheet: standby → regulator → calibrate → DIO2/TCXO.
    uint8_t p1[1];
    p1[0] = STANDBY_RC;
    RCHECK(cmdN(OP_SET_STANDBY, p1, 1), "standby");
    p1[0] = REGULATOR_DCDC;
    RCHECK(cmdN(OP_SET_REGULATOR, p1, 1), "regulator");
    {
        uint8_t cal[1] = {0x7F};
        RCHECK(cmdN(OP_CALIBRATE, cal, 1), "calibrate");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (hw->dio2_rf_switch) {
        uint8_t d2[1] = {0x01};
        cmdN(OP_SET_DIO2_RF, d2, 1); // non-fatal on modules without switch
    }
    if (hw->tcxo_controlled) {
        // DIO3 TCXO, board-selected voltage, 5ms timeout (0x140 x 15.625us).
        uint8_t tcxo[4] = {hw->tcxo_voltage_code, 0x00, 0x01, 0x40};
        cmdN(OP_SET_DIO3_TCXO, tcxo, 4);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    {
        uint8_t pt[1] = {PKT_TYPE_LORA};
        RCHECK(cmdN(OP_SET_PKT_TYPE, pt, 1), "pkt-type");
    }
    {
        // RF freq: FRF = freq * 2^25 / 32MHz
        uint32_t frf = (uint32_t)(((uint64_t)freq_hz << 25) / 32000000ULL);
        uint8_t f[4] = {(uint8_t)(frf >> 24), (uint8_t)(frf >> 16), (uint8_t)(frf >> 8), (uint8_t)frf};
        RCHECK(cmdN(OP_SET_RFFREQ, f, 4), "freq");
    }
    {
        // PA: SX1262 high-power (+22 max): paDuty 0x04, hpMax 0x07.
        uint8_t pa[4] = {0x04, 0x07, 0x00, 0x01};
        RCHECK(cmdN(OP_SET_PA, pa, 4), "pa");
    }
    {
        // TX params: region-clamped power, ramp 40us (0x04).
        // lora_modem_clamp_tx enforces the RegionInfo powerLimit for the
        // sticky region set via lora_radio_set_region_tx_limit(); the
        // is_licensed bypass applies only when explicitly set (HAM).
        int pwr = lora_modem_clamp_tx(s_tx_region, tx_dbm, s_tx_licensed);
        if (pwr != tx_dbm) {
            ESP_LOGW(TAG, "TX %ddBm clamped to %ddBm (region %d%s)", tx_dbm, pwr,
                     s_tx_region, s_tx_licensed ? ", licensed" : "");
        }
        s_tx = pwr; // record effective power for the ready log below
        uint8_t tp[2] = {(uint8_t)pwr, 0x04};
        RCHECK(cmdN(OP_SET_TX_PARAMS, tp, 2), "tx-params");
    }
    {
        uint8_t mp[4];
        mp[0] = (uint8_t)sf;
        mp[1] = bw_reg(bw_khz);
        mp[2] = (uint8_t)(cr_denom - 4); // SX126x CR: 0x01=4/5 .. 0x04=4/8
        mp[3] = 0x00; // LDRO off (auto below for SF11/12@125)
        if (sf >= 11 && bw_khz <= 125) mp[3] = 0x01;
        RCHECK(cmdN(OP_SET_MOD_PARAMS, mp, sizeof(mp)), "modem");
    }
    {
        RCHECK(set_lora_packet_params(0xFF), "pkt-params");
    }
    {
        // Meshtastic calls RadioLib setSyncWord(0x2B).  SX126x stores the
        // nibbles interleaved with RadioLib's 0x44 control bits, yielding
        // 0x24, 0xB4 at 0x0740/0x0741.
        const uint8_t sw[2] = {0x24, 0xB4};
        RCHECK(wr_reg(0x0740, sw, 2), "syncword");
        uint8_t got[2] = {0};
        RCHECK(rd_reg(0x0740, got, 2), "syncword-read");
        if (memcmp(got, sw, sizeof(sw)) != 0) {
            ESP_LOGE(TAG, "sync-word readback failed: wrote %02X%02X read %02X%02X",
                     sw[0], sw[1], got[0], got[1]);
            s_fail_step = "syncword-readback";
            goto fail;
        }
    }
    {
        // RadioLib fixInvertedIQ(false): standard-IQ mode requires bit 2 set
        // in the SX1262 IQ workaround register.
        uint8_t iq[1] = {0};
        if (rd_reg(0x0736, iq, 1) == 0) {
            iq[0] = (uint8_t)(iq[0] | 0x04);
            wr_reg(0x0736, iq, 1);
        }
    }
    {
        // Undocumented SX1262 RX-sensitivity patch (Semtech via upstream
        // reinitChip): set bit 0 of 0x08B5. Best-effort, non-fatal.
        uint8_t cur[1] = {0};
        if (rd_reg(0x08B5, cur, 1) == 0) {
            cur[0] = (uint8_t)(cur[0] | 0x01);
            wr_reg(0x08B5, cur, 1);
        }
    }
    {
        uint8_t bb[2] = {0x00, 0x00};
        RCHECK(cmdN(OP_SET_BUF_BASE, bb, 2), "buf-base");
    }
    {
        // Image rejection cal for our band (RadioLib calibrateImage in
        // begin(); 902-928 MHz preset). Skipping it leaves image response
        // up and sensitivity/CAD behavior off-spec. Best-effort.
        uint32_t mhz = s_freq / 1000000u;
        const uint8_t *img = NULL;
        static const uint8_t img_902[] = {0xE1, 0xE9};
        static const uint8_t img_863[] = {0xD7, 0xDB};
        static const uint8_t img_779[] = {0xC1, 0xC5};
        static const uint8_t img_470[] = {0x75, 0x81};
        static const uint8_t img_430[] = {0x6B, 0x6F};
        if (mhz >= 902 && mhz <= 928) img = img_902;
        else if (mhz >= 863 && mhz <= 870) img = img_863;
        else if (mhz >= 779 && mhz <= 787) img = img_779;
        else if (mhz >= 470 && mhz <= 510) img = img_470;
        else if (mhz >= 430 && mhz <= 440) img = img_430;
        if (img) cmdN(OP_CAL_IMAGE, img, 2);
    }
    {
        // DIO1: RxDone + TxDone + Timeout + CrcErr.
        uint8_t irq[8] = {0x02, 0x43, 0x02, 0x43, 0x00, 0x00, 0x00, 0x00};
        RCHECK(cmdN(OP_SET_DIO_IRQ, irq, 8), "dio-irq");
    }
    {
        uint8_t errs[2] = {0};
        cmdN_resp(OP_GET_DEV_ERRORS, NULL, 0, errs, 2);
        cmdN(OP_CLR_DEV_ERRORS, (uint8_t[]){0x00, 0x00}, 2);
    }
    s_ready = true;
    s_fail_step = "none";
    unlock();
    ESP_LOGI(TAG, "SX1262 ready %u Hz SF%d BW%d CR4/%d TX%d pins=%d/%d/%d/%d irq=%d busy=%d rst=%d tcxo=0x%02x",
             (unsigned)freq_hz, sf, bw_khz, cr_denom, s_tx,
             hw->mosi_pin, hw->miso_pin, hw->sck_pin, hw->nss_pin,
             hw->dio1_pin, hw->busy_pin, hw->rst_pin,
             hw->tcxo_controlled ? hw->tcxo_voltage_code : 0);
    return 0;
fail:
    spi_bus_remove_device(s_dev);
    s_dev = NULL;
    if (s_bus_ours) { spi_bus_free(s_host); s_bus_ours = false; }
    lora_module_enable(false);
    unlock();
    return -1;
}

void lora_radio_deinit(void) {
    lora_radio_stop();
    lock();
    if (s_dev) {
        cmd1(OP_SET_SLEEP);
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus_ours) { spi_bus_free(s_host); s_bus_ours = false; }
    s_ready = false;
    unlock();
    lora_module_enable(false);
}

bool lora_radio_is_ready(void) { return s_ready && s_dev != NULL; }

int lora_radio_start_rx(lora_rx_cb_t cb, void *ctx) {
    if (!lora_radio_is_ready() || s_rx_on) {
        s_fail_step = "rx-not-ready";
        return -1;
    }
    s_cb = cb; s_ctx = ctx;
    if (!s_rx_exit_sem) s_rx_exit_sem = xSemaphoreCreateBinaryStatic(&s_rx_exit_sem_buf);
    if (s_rx_exit_sem) (void)xSemaphoreTake(s_rx_exit_sem, 0);
    esp_err_t isr_rc = gpio_install_isr_service(0);
    if (isr_rc != ESP_OK && isr_rc != ESP_ERR_INVALID_STATE) {
        s_fail_step = "dio1-isr-service";
        return -1;
    }
    gpio_isr_handler_remove((gpio_num_t)s_hw.dio1_pin);
    gpio_set_intr_type((gpio_num_t)s_hw.dio1_pin, GPIO_INTR_POSEDGE);
    if (gpio_isr_handler_add((gpio_num_t)s_hw.dio1_pin, dio1_isr, NULL) != ESP_OK) {
        s_fail_step = "dio1-isr";
        return -1;
    }
    s_rx_on = true;
    lock();
    clear_irq(IRQ_ALL);
    int r = set_rx_continuous();
    unlock();
    if (r != 0) { s_fail_step = "rx-continuous"; s_rx_on = false; return -1; }
    // 8192B: the RX callback (mesh decode + phone pushes + PKI + logging)
    // runs in this task; 4096 overflowed once NodeInfo/PKI forwarding grew.
    if (xTaskCreate(radio_task, "lora_rx", 8192, NULL, 10, &s_task) != pdPASS) {
        s_fail_step = "rx-task";
        s_rx_on = false;
        return -1;
    }
    s_fail_step = "none";
    return 0;
}

void lora_radio_stop(void) {
    s_rx_on = false;
    if (s_task) {
        TaskHandle_t task = s_task;
        xTaskNotifyGive(task);
        /* Wait for the callback/decoder task to really exit.  A fixed 50 ms
         * delay allowed a busy packet callback to survive into the next
         * `lora start`, leaking a task stack and a second radio allocation. */
        if (s_rx_exit_sem) (void)xSemaphoreTake(s_rx_exit_sem, portMAX_DELAY);
        s_task = NULL;
    }
    gpio_isr_handler_remove((gpio_num_t)s_hw.dio1_pin);
    s_cb = NULL;
}

int lora_radio_send(const uint8_t *data, uint8_t len) {
    if (!lora_radio_is_ready() || !data || len == 0 || len > 240) return -1;
    lock();
    // Configuration writes and buffer setup are performed from standby in
    // the reference driver.
    {
        uint8_t sb[1] = {STANDBY_RC};
        if (cmdN(OP_SET_STANDBY, sb, 1) != 0 ||
            set_lora_packet_params(len) != 0) {
            set_rx_continuous();
            unlock();
            return -1;
        }
    }
    clear_irq(IRQ_ALL);
    uint8_t hdr[2] = {OP_WRITE_BUF, 0x00};
    s_spi_tx[0] = hdr[0]; s_spi_tx[1] = hdr[1];
    memcpy(&s_spi_tx[2], data, len);
    int r = -1;
    if (s_dev && wait_busy(200)) {
        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        size_t wire_len = ((size_t)2 + len + 3u) & ~3u;
        memset(s_spi_tx + 2 + len, 0, wire_len - ((size_t)2 + len));
        t.length = (uint32_t)(wire_len * 8);
        t.tx_buffer = s_spi_tx;
        t.rx_buffer = s_spi_rx;
        if (spi_device_polling_transmit(s_dev, &t) == ESP_OK && wait_busy(200)) {
            // 8 seconds in SX126x 15.625us units (24-bit big endian).
            uint8_t to[3] = {0x07, 0xD0, 0x00};
            r = cmdN(OP_SET_TX, to, 3);
        }
    }
    unlock();
    if (r != 0) {
        lock();
        clear_irq(IRQ_ALL);
        set_rx_continuous();
        unlock();
        return -1;
    }
    // Poll TxDone (sender task is NOT the radio task, so task-notify
    // would park us for the full timeout on every TX — poll instead).
    for (int i = 0; i < 90; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        lock();
        uint8_t irq[2] = {0};
        int q = cmdN_resp(OP_GET_IRQ, NULL, 0, irq, 2);
        uint16_t f = (q == 0) ? (uint16_t)((irq[0] << 8) | irq[1]) : 0;
        if (q == 0 && (f & (IRQ_TX_DONE | IRQ_TIMEOUT))) {
            clear_irq(IRQ_ALL);
            set_rx_continuous();
            unlock();
            return (f & IRQ_TX_DONE) ? 0 : -1;
        }
        unlock();
    }
    lock();
    clear_irq(IRQ_ALL);
    set_rx_continuous();
    unlock();
    return -1;
}

bool lora_radio_cad(void) {
    if (!lora_radio_is_ready()) return false;
    lock();
    // Mirror RadioLib startChannelScan: standby first, then clear stale IRQ
    // flags so this measurement starts clean (stale CAD_DET read as "busy"
    // forever was the original always-busy lockup).
    {
        uint8_t sb[1] = {STANDBY_RC};
        cmdN(OP_SET_STANDBY, sb, 1);
    }
    clear_irq(IRQ_ALL);
    // cadSymbolNum enum: 0x01 = 2 symbols (upstream NUM_SYM_CAD=2 per
    // AN1200.48; 0x02 would be 4). detPeak=22/detMin=10 (RadioLib defaults):
    // the old peak=16/min=0 fired on pure noise, bricking every TX.
    // Exit=CAD_ONLY, no extra timeout (upstream timeout=0).
    uint8_t p[7] = {0x01, 0x16, 0x0A, 0x00, 0x00, 0x00, 0x00};
    bool busy = false;
    if (cmdN(OP_SET_CAD_PARAMS, p, 7) == 0 && cmd1(OP_SET_CAD) == 0) {
        // CAD at SF11/BW250 needs ~25ms; SF12/125 ~100ms. The old fixed
        // 15ms read raced the measurement and sampled stale flags. Poll
        // for CAD_DONE, then trust CAD_DET only.
        bool done = false;
        for (int i = 0; i < 30; i++) {
            vTaskDelay(pdMS_TO_TICKS(5));
            uint8_t irq[2] = {0};
            if (cmdN_resp(OP_GET_IRQ, NULL, 0, irq, 2) == 0) {
                uint16_t f = (uint16_t)((irq[0] << 8) | irq[1]);
                if (f & IRQ_CAD_DONE) {
                    busy = (f & IRQ_CAD_DET) != 0;
                    done = true;
                    break;
                }
            }
        }
        if (!done) busy = false; // CAD never finished: assume free, don't brick TX
        clear_irq(IRQ_ALL);
        set_rx_continuous();
    }
    unlock();
    return busy;
}

int lora_radio_rssi_inst(int16_t *out_rssi) {
    if (!out_rssi || !lora_radio_is_ready()) return -1;
    lock();
    uint8_t r[1] = {0};
    int rc = cmdN_resp(OP_GET_RSSI_INST, NULL, 0, r, 1);
    unlock();
    if (rc != 0) return -1;
    *out_rssi = -(int16_t)r[0] / 2; // unsigned raw, RadioLib convention
    return 0;
}

int lora_radio_read_register(uint16_t addr, uint8_t *out, uint8_t len) {
    if (!out || len == 0 || len > 8 || !lora_radio_is_ready()) return -1;
    lock();
    int rc = rd_reg(addr, out, len);
    unlock();
    return rc;
}

#else
// No-LoRa stub translation unit (globbed always).
typedef int lora_sx1262_stub_guard;
#endif
