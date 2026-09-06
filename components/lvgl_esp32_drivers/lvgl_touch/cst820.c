#include <esp_log.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include <lvgl.h>
#else
#include <lvgl/lvgl.h>
#endif
#include "cst820.h"
#include "i2c_shared.h"

#define TAG "CST820"
#define I2C_MASTER_TIMEOUT_MS 50
#define I2C_MASTER_FREQ_HZ 400000

static i2c_master_bus_handle_t s_cst820_bus = NULL;
static i2c_master_dev_handle_t s_cst820_dev = NULL;
static bool s_cst820_bus_owned = false;

#if CONFIG_CROWPANEL_1P28_ROTARY
/* Elecrow's factory firmware uses Wire1 for the CST816D.  Keep the same
 * controller/bus identity here while using the shared I2C manager so the
 * touch reader remains serialized with the rest of GhostESP. */
#define TOUCH_I2C_PORT 1
#define TOUCH_INT_PIN 5
#define TOUCH_RST_PIN 13
#define TOUCH_SDA_PIN 6
#define TOUCH_SCL_PIN 7
#elif CONFIG_USE_TDISPLAY_S3
/* T-Display S3: touch is a CST816 on SDA=18/SCL=17, INT=16, RESET=21.
 * The CYD defaults (INT=21, RST=25) are wrong here: GPIO 25 does not exist
 * on the ESP32-S3, and GPIO 21 is the touch RESET line, not the interrupt. */
#define TOUCH_I2C_PORT 0
#define TOUCH_INT_PIN 16
#define TOUCH_RST_PIN 21
#if defined(CONFIG_I2C_MANAGER_0_ENABLED)
#define TOUCH_SDA_PIN CONFIG_I2C_MANAGER_0_SDA
#define TOUCH_SCL_PIN CONFIG_I2C_MANAGER_0_SCL
#else
#define TOUCH_SDA_PIN 18
#define TOUCH_SCL_PIN 17
#endif
#else
#define TOUCH_I2C_PORT 0
#define TOUCH_INT_PIN CYD28_TouchC_INT
#define TOUCH_RST_PIN CYD28_TouchC_RST
#define TOUCH_SDA_PIN CYD28_TouchC_SDA
#define TOUCH_SCL_PIN CYD28_TouchC_SCL
#endif

esp_err_t cst820_i2c_read(uint8_t reg_addr, uint8_t *data, size_t len) {
    if (s_cst820_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_cst820_dev, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t cst820_i2c_write(uint8_t reg_addr, uint8_t data) {
    if (s_cst820_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t payload[2] = { reg_addr, data };
    return i2c_master_transmit(s_cst820_dev, payload, sizeof(payload), I2C_MASTER_TIMEOUT_MS);
}

static void cst820_reset_pins(void) {
    /* CST816-style panels hold the IC in reset until RST is toggled low and
     * released high. Without this the IC never ACKs on I2C. */
    if (GPIO_IS_VALID_GPIO(TOUCH_INT_PIN)) {
        gpio_set_direction(TOUCH_INT_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_INT_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1));
        gpio_set_level(TOUCH_INT_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1));
#if CONFIG_USE_TDISPLAY_S3 || CONFIG_CROWPANEL_1P28_ROTARY
        /* The CST816S drives INT as an open-drain output. Hand the pin back
         * to the IC instead of leaving it forced high, or its interrupt
         * (and test) pulses get shorted. */
        gpio_set_direction(TOUCH_INT_PIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode(TOUCH_INT_PIN, GPIO_PULLUP_ONLY);
#endif
    }
    if (GPIO_IS_VALID_GPIO(TOUCH_RST_PIN)) {
        gpio_set_direction(TOUCH_RST_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(TOUCH_RST_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TOUCH_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void cst820_init(void) {
    ESP_ERROR_CHECK(i2c_shared_get_or_create_bus(TOUCH_I2C_PORT, TOUCH_SDA_PIN, TOUCH_SCL_PIN,
                                                 true, &s_cst820_bus, &s_cst820_bus_owned));

    cst820_reset_pins();

    if (s_cst820_dev == NULL) {
        /* Some panels (e.g. T-Display S3) carry an FT6x36-compatible IC at
         * 0x38 instead of the CST820 at 0x15. Probe both, they share the
         * same register layout. Retry a few times: the IC can take a moment
         * to come up after the reset pulse. */
        const uint16_t probe_addrs[] = {
#if CONFIG_CROWPANEL_1P28_ROTARY
            I2C_ADDR_CST820
#else
            I2C_ADDR_CST820, 0x38
#endif
        };
        for (int attempt = 0; attempt < 3 && s_cst820_dev == NULL; attempt++) {
            for (size_t i = 0; i < sizeof(probe_addrs) / sizeof(probe_addrs[0]); i++) {
                if (i2c_master_probe(s_cst820_bus, probe_addrs[i], 100) == ESP_OK) {
                    esp_err_t ret = i2c_shared_add_device(s_cst820_bus, probe_addrs[i],
                                                          I2C_MASTER_FREQ_HZ, &s_cst820_dev);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "Touch IC found at 0x%02X", probe_addrs[i]);
                        break;
                    }
                }
            }
            if (s_cst820_dev == NULL && attempt < 2) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
        if (s_cst820_dev == NULL) {
#if CONFIG_CROWPANEL_1P28_ROTARY
            ESP_LOGE(TAG, "No CST816D-compatible touch IC detected at 0x15 on I2C1");
#else
            ESP_LOGE(TAG, "No touch IC detected on I2C bus (probed 0x15, 0x38)");
#endif
        }
    }

    if (s_cst820_dev != NULL) {
        /* disSleep: a non-zero value keeps the IC out of its low-power
         * standby. While in standby it stops ACKing/returns garbage on I2C,
         * which a poller turns into phantom touches. */
        esp_err_t err = cst820_i2c_write(0xFE, 0xFF);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch IC disSleep write failed: %s", esp_err_to_name(err));
        }
        /* Exit gesture mode: swipes and double taps are reported by the IC
         * as fake finger-down events (with zero/stale coordinates) that the
         * UI would otherwise decode as phantom taps. */
        err = cst820_i2c_write(0xD0, 0x00);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Touch IC gesture disable write failed: %s", esp_err_to_name(err));
        }
    }
}
static void convert_raw_xy(int16_t raw_x, int16_t raw_y, int16_t *x, int16_t *y) {
#if CONFIG_USE_TDISPLAY_S3
    *x = raw_y;
    *y = 170 - raw_x;
    // rotate 90 degrees
    ESP_LOGV(TAG, "Raw: x=%d, y=%d, Converted: x=%d, y=%d", raw_x, raw_y, *x, *y);
#else
    *x = raw_x;
    *y = raw_y;
#endif
}

bool cst820_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    static int16_t pending_x = 0;
    static int16_t pending_y = 0;
    static uint8_t press_samples = 0;
    static uint8_t release_samples = 0;
    static bool pressed = false;

    uint8_t buf[8];
    bool valid = false;

    /* Atomic burst read: gesture, finger count and coordinates are snapshotted
     * by the IC in one transaction. Reading the count (0x02) and the data
     * (0x03) in separate transactions can interleave with the IC's scan cycle
     * and pair a fresh count with stale/zero coordinates. */
    if (cst820_i2c_read(0x01, buf, sizeof(buf)) == ESP_OK) {
        uint8_t finger = buf[1] & 0x0F;             // 0x02: touch points [3:0]
        uint8_t event_flag = (buf[2] & 0xC0) >> 6;  // 0x03 bits 7:6: 0=down, 1=up, 2=contact
        int16_t raw_x = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
        int16_t raw_y = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);

        /* An idle or sleeping IC can report 0xFF in the finger-count register.
         * Only exactly one finger, on a down/contact event, is a real touch;
         * anything else (including release reports and garbage) is invalid. */
        if (finger == 1 && event_flag != 1) {
            int16_t x, y;
            convert_raw_xy(raw_x, raw_y, &x, &y);
            if (x >= 0 && x < LV_HOR_RES && y >= 0 && y < LV_VER_RES) {
                pending_x = x;
                pending_y = y;
                valid = true;
            }
        }
    }

    if (valid) {
        release_samples = 0;
        /* Require two consecutive valid samples before claiming a press so a
         * one-off glitch can never become a phantom tap. Real touches last
         * far longer than the 10 ms poll period. */
        if (!pressed && ++press_samples >= 2) {
            pressed = true;
            press_samples = 0;
        }
    } else {
        press_samples = 0;
        /* Symmetric debounce on release so a single dropped sample during a
         * drag does not cut the touch short. */
        if (pressed && ++release_samples >= 2) {
            pressed = false;
            release_samples = 0;
        }
    }

    if (pressed) {
        last_x = pending_x;
        last_y = pending_y;
        data->point.x = pending_x;
        data->point.y = pending_y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->point.x = last_x;
        data->point.y = last_y;
        data->state = LV_INDEV_STATE_REL;
    }

    return false;
}
