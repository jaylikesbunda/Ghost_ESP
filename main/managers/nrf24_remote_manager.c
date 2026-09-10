#include "managers/nrf24_remote_manager.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_NRF24

#include "core/esp_comm_manager.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "rom/ets_sys.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define NRF24_CHANNEL_COUNT 126
#define NRF24_STREAM_VERSION 1
#define NRF24_TASK_SLEEP_MS 40

#define NRF_CMD_R_REGISTER 0x00
#define NRF_CMD_W_REGISTER 0x20
#define NRF_CMD_FLUSH_TX   0xE1
#define NRF_CMD_FLUSH_RX   0xE2
#define NRF_CMD_NOP        0xFF

#define NRF_REG_CONFIG     0x00
#define NRF_REG_EN_AA      0x01
#define NRF_REG_EN_RXADDR  0x02
#define NRF_REG_SETUP_AW   0x03
#define NRF_REG_RF_CH      0x05
#define NRF_REG_RF_SETUP   0x06
#define NRF_REG_STATUS     0x07
#define NRF_REG_RX_ADDR_P0 0x0A
#define NRF_REG_RX_PW_P0   0x11
#define NRF_REG_DYNPD      0x1C
#define NRF_REG_FEATURE    0x1D
#define NRF_REG_RPD        0x09

static const char *TAG = "NRF24RemoteMgr";

static TaskHandle_t s_nrf24_task = NULL;
static volatile bool s_stop_requested = false;
static volatile bool s_paused = false;
static volatile bool s_stream_to_peer = false;

static spi_device_handle_t s_spi_dev = NULL;
static spi_host_device_t s_spi_host = SPI3_HOST;
static bool s_spi_bus_initialized_by_us = false;

static uint8_t s_levels[NRF24_CHANNEL_COUNT];
static uint8_t s_next_channel = 0;
static char s_last_error[96] = "none";

static void nrf24_hw_stop(void);

static void nrf24_set_last_error(const char *msg) {
    if (!msg || msg[0] == '\0') {
        msg = "unknown";
    }
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg);
}

static bool nrf24_validate_pin_config(void) {
    const int mosi = CONFIG_NRF24_SPI_MOSI_PIN;
    const int miso = CONFIG_NRF24_SPI_MISO_PIN;
    const int sck = CONFIG_NRF24_SPI_SCK_PIN;
    const int csn = CONFIG_NRF24_CSN_PIN;
    const int ce = CONFIG_NRF24_CE_PIN;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(mosi)) {
        nrf24_set_last_error("invalid MOSI pin");
        ESP_LOGE(TAG, "Invalid NRF24 MOSI pin: GPIO%d", mosi);
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(miso)) {
        nrf24_set_last_error("invalid MISO pin");
        ESP_LOGE(TAG, "Invalid NRF24 MISO pin: GPIO%d", miso);
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(sck)) {
        nrf24_set_last_error("invalid SCK pin");
        ESP_LOGE(TAG, "Invalid NRF24 SCK pin: GPIO%d", sck);
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(csn)) {
        nrf24_set_last_error("invalid CSN pin");
        ESP_LOGE(TAG, "Invalid NRF24 CSN pin: GPIO%d", csn);
        return false;
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO(ce)) {
        nrf24_set_last_error("invalid CE pin");
        ESP_LOGE(TAG, "Invalid NRF24 CE pin: GPIO%d", ce);
        return false;
    }

    if (mosi == miso || mosi == sck || mosi == csn || mosi == ce ||
        miso == sck || miso == csn || miso == ce ||
        sck == csn || sck == ce || csn == ce) {
        nrf24_set_last_error("pin conflict");
        ESP_LOGE(TAG, "NRF24 pin conflict detected (pins must be unique)");
        return false;
    }

    return true;
}

static inline spi_host_device_t nrf24_spi_host_from_config(void) {
#if defined(SPI3_HOST)
    if (CONFIG_NRF24_SPI_HOST == 2) {
        return SPI2_HOST;
    }
    return SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}

static esp_err_t nrf24_spi_transfer(const uint8_t *tx, uint8_t *rx, size_t len) {
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

static esp_err_t nrf24_write_reg(uint8_t reg, uint8_t value) {
    uint8_t tx[2] = { (uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F)), value };
    uint8_t rx[2] = {0};
    return nrf24_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t nrf24_read_reg(uint8_t reg, uint8_t *value) {
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t tx[2] = { (uint8_t)(NRF_CMD_R_REGISTER | (reg & 0x1F)), NRF_CMD_NOP };
    uint8_t rx[2] = {0};
    esp_err_t err = nrf24_spi_transfer(tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *value = rx[1];
    }
    return err;
}

static esp_err_t nrf24_write_reg_buf(uint8_t reg, const uint8_t *buf, size_t len) {
    if (!buf || len == 0 || len > 5) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[6] = {0};
    uint8_t rx[6] = {0};
    tx[0] = (uint8_t)(NRF_CMD_W_REGISTER | (reg & 0x1F));
    memcpy(&tx[1], buf, len);
    return nrf24_spi_transfer(tx, rx, len + 1);
}

static esp_err_t nrf24_cmd(uint8_t cmd) {
    uint8_t tx[1] = { cmd };
    uint8_t rx[1] = {0};
    return nrf24_spi_transfer(tx, rx, sizeof(tx));
}

static esp_err_t nrf24_set_channel(uint8_t channel) {
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);
    esp_err_t err = nrf24_write_reg(NRF_REG_RF_CH, (uint8_t)(channel & 0x7F));
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 1);
    return err;
}

static esp_err_t nrf24_hw_start(void) {
    s_spi_host = nrf24_spi_host_from_config();

    ESP_LOGI(TAG,
             "NRF24 init SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d CE=%d",
             (int)s_spi_host,
             CONFIG_NRF24_SPI_MOSI_PIN,
             CONFIG_NRF24_SPI_MISO_PIN,
             CONFIG_NRF24_SPI_SCK_PIN,
             CONFIG_NRF24_CSN_PIN,
             CONFIG_NRF24_CE_PIN);

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_NRF24_CE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CE gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_NRF24_SPI_MOSI_PIN,
        .miso_io_num = CONFIG_NRF24_SPI_MISO_PIN,
        .sclk_io_num = CONFIG_NRF24_SPI_SCK_PIN,
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
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = CONFIG_NRF24_SPI_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = CONFIG_NRF24_CSN_PIN,
        .queue_size = 1,
    };

    err = spi_bus_add_device(s_spi_host, &dev_cfg, &s_spi_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        if (s_spi_bus_initialized_by_us) {
            spi_bus_free(s_spi_host);
            s_spi_bus_initialized_by_us = false;
        }
        s_spi_dev = NULL;
        return err;
    }

    uint8_t rx_addr[5] = { 0xE7, 0xE7, 0xE7, 0xE7, 0xE7 };

    err = nrf24_write_reg(NRF_REG_EN_AA, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_EN_RXADDR, 0x01);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_SETUP_AW, 0x03);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_RF_SETUP, 0x07);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_RX_PW_P0, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_DYNPD, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_FEATURE, 0x00);
    if (err == ESP_OK) err = nrf24_write_reg_buf(NRF_REG_RX_ADDR_P0, rx_addr, sizeof(rx_addr));
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_STATUS, 0x70);
    if (err == ESP_OK) err = nrf24_cmd(NRF_CMD_FLUSH_RX);
    if (err == ESP_OK) err = nrf24_cmd(NRF_CMD_FLUSH_TX);
    if (err == ESP_OK) err = nrf24_write_reg(NRF_REG_CONFIG, 0x03);
    if (err == ESP_OK) err = nrf24_set_channel(0);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NRF24 init sequence failed: %s", esp_err_to_name(err));
        nrf24_hw_stop();
        return err;
    }

    ets_delay_us(2000);
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 1);
    return ESP_OK;
}

static void nrf24_hw_stop(void) {
    gpio_set_level((gpio_num_t)CONFIG_NRF24_CE_PIN, 0);

    if (s_spi_dev) {
        spi_bus_remove_device(s_spi_dev);
        s_spi_dev = NULL;
    }

    if (s_spi_bus_initialized_by_us) {
        spi_bus_free(s_spi_host);
        s_spi_bus_initialized_by_us = false;
    }
}

static void nrf24_stream_chunk(uint8_t cursor, uint8_t start_ch, uint8_t count) {
    if (!s_stream_to_peer || !esp_comm_manager_is_connected() || count == 0) {
        return;
    }

    uint8_t pkt[4 + 32] = {0};
    if (count > 32) count = 32;
    pkt[0] = NRF24_STREAM_VERSION;
    pkt[1] = cursor;
    pkt[2] = start_ch;
    pkt[3] = count;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t ch = (uint8_t)((start_ch + i) % NRF24_CHANNEL_COUNT);
        pkt[4 + i] = s_levels[ch];
    }

    (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_NRF24, pkt, (size_t)(4 + count));
}

static void nrf24_scan_task(void *arg) {
    (void)arg;

    memset(s_levels, 0, sizeof(s_levels));
    s_next_channel = 0;

    if (nrf24_hw_start() != ESP_OK) {
        nrf24_set_last_error("hw init failed");
        if (s_stream_to_peer && esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("nrf24", "state error");
        }
        s_nrf24_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    while (!s_stop_requested) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int samples_per_channel = CONFIG_NRF24_ANALYZER_SAMPLES_PER_CHANNEL;
        int channels_per_tick = CONFIG_NRF24_ANALYZER_CHANNELS_PER_TICK;
        int settle_us = CONFIG_NRF24_ANALYZER_SETTLE_US;

        if (samples_per_channel < 1) samples_per_channel = 1;
        if (channels_per_tick < 1) channels_per_tick = 1;
        if (channels_per_tick > 32) channels_per_tick = 32;
        if (settle_us < 130) settle_us = 130;

        uint8_t start_ch = s_next_channel;

        for (int i = 0; i < channels_per_tick; ++i) {
            uint8_t ch = s_next_channel;
            s_next_channel = (uint8_t)((s_next_channel + 1) % NRF24_CHANNEL_COUNT);

            int hits = 0;
            for (int s = 0; s < samples_per_channel; ++s) {
                if (nrf24_set_channel(ch) != ESP_OK) {
                    continue;
                }

                ets_delay_us((uint32_t)settle_us);

                uint8_t rpd = 0;
                if (nrf24_read_reg(NRF_REG_RPD, &rpd) == ESP_OK && (rpd & 0x01)) {
                    hits++;
                }
            }

            uint8_t raw_level = (uint8_t)((hits * 100) / samples_per_channel);
            /* Heavier smoothing than a plain 3:1 EMA — RPD is a 1-bit carrier
             * detect, not analog RSSI, so raw_level is coarsely quantized and a
             * single noisy sample can otherwise swing the streamed value by 25%
             * in one chunk. The receive side applies no further smoothing, so
             * jitter here goes straight to the peer's graph unfiltered. */
            s_levels[ch] = (uint8_t)((s_levels[ch] * 5 + raw_level) / 6);
        }

        nrf24_stream_chunk(s_next_channel, start_ch, (uint8_t)channels_per_tick);
        vTaskDelay(pdMS_TO_TICKS(NRF24_TASK_SLEEP_MS));
    }

    nrf24_hw_stop();

    if (s_stream_to_peer && esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("nrf24", "state stopped");
    }

    s_nrf24_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

bool nrf24_remote_manager_start(bool stream_to_peer) {
    s_stream_to_peer = stream_to_peer;
    s_stop_requested = false;
    s_paused = false;

    if (s_nrf24_task) {
        nrf24_set_last_error("none");
        return true;
    }

    if (!nrf24_validate_pin_config()) {
        return false;
    }

    const uint32_t stack_size = 4096;
    BaseType_t ok = xTaskCreateWithCaps(nrf24_scan_task, "nrf24_scan", stack_size, NULL, 5, &s_nrf24_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(nrf24_scan_task, "nrf24_scan", stack_size, NULL, 5, &s_nrf24_task,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        nrf24_set_last_error("task create failed");
        return false;
    }

    nrf24_set_last_error("none");
    return true;
}

void nrf24_remote_manager_stop(void) {
    s_stop_requested = true;
}

void nrf24_remote_manager_set_paused(bool paused) {
    s_paused = paused;
}

bool nrf24_remote_manager_is_running(void) {
    return s_nrf24_task != NULL;
}

bool nrf24_remote_manager_is_paused(void) {
    return s_paused;
}

const char *nrf24_remote_manager_get_last_error(void) {
    return s_last_error;
}

#else

bool nrf24_remote_manager_start(bool stream_to_peer) {
    (void)stream_to_peer;
    return false;
}

void nrf24_remote_manager_stop(void) {}
void nrf24_remote_manager_set_paused(bool paused) { (void)paused; }
bool nrf24_remote_manager_is_running(void) { return false; }
bool nrf24_remote_manager_is_paused(void) { return false; }
const char *nrf24_remote_manager_get_last_error(void) { return "built without CONFIG_HAS_NRF24"; }

#endif
