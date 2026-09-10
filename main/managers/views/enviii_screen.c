#include "managers/views/enviii_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "i2c_shared.h"
#include <math.h>
#include <string.h>
#include "gui/design_tokens.h"
#include "gui/touch_bar.h"
#include "managers/gps_manager.h"
#include "vendor/drivers/aw9523.h"

#ifdef CONFIG_HAS_ENVIII

static const char *TAG = "ENVIIIScreen";

static lv_obj_t *enviii_container = NULL;
static lv_obj_t *enviii_content = NULL;
static lv_obj_t *temp_label = NULL;
static lv_obj_t *comfort_label = NULL;
static lv_obj_t *feels_like_label = NULL;
static lv_obj_t *weather_hint_label = NULL;
static lv_obj_t *hum_label = NULL;
static lv_obj_t *dew_label = NULL;
static lv_obj_t *press_hpa_label = NULL;
static lv_obj_t *press_inhg_label = NULL;
static lv_obj_t *alt_label = NULL;
static lv_obj_t *touch_bar = NULL;
static lv_obj_t *enviii_btn_up = NULL;
static lv_obj_t *enviii_btn_back = NULL;
static lv_obj_t *enviii_btn_down = NULL;
static gui_touch_bar_t s_touch_tb;
static lv_timer_t *enviii_timer = NULL;

#ifdef CONFIG_USE_TOUCHSCREEN
static touch_drag_t enviii_touch_drag;
#endif

static i2c_master_dev_handle_t s_sht30_dev = NULL;
static i2c_master_dev_handle_t s_qmp6988_dev = NULL;

static float sea_level_pressure = 1013.25f;
static bool sht_data_valid = false;
static bool qmp_data_valid = false;
static bool gps_stopped_by_enviii = false;
static bool enviii_released_i2c_bus = false;
static int enviii_pin_candidate = -1; /* -1 = board profile pins */

static uint32_t accent_color = 0x00FFFF;
static uint32_t bg_color = 0x0A0A0A;
static uint32_t card_color = 0x1A1A1A;
static uint32_t text_color = 0xFFFFFF;
static uint32_t dim_color = 0x888888;

static void enviii_scroll_content(int dir);

#ifdef CONFIG_USE_TOUCHSCREEN
/* Geometry aliases: canonical values now live in gui/touch_bar.h
 * (GUI_TOUCH_BAR_BTN_SIZE / GUI_TOUCH_BAR_PADDING). Height math uses
 * gui_touch_bar_height() directly. */
#define ENVIII_SCROLL_BTN_SIZE    28
#define ENVIII_SCROLL_BTN_PADDING 3
#endif

#ifndef CONFIG_ENVIII_I2C_PORT
#define CONFIG_ENVIII_I2C_PORT 0
#endif
#define ENVIII_I2C_PORT CONFIG_ENVIII_I2C_PORT

static bool enviii_has_gps_i2c_conflict(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "m5cores3se") == 0 &&
           ENVIII_I2C_PORT == 1;
#else
    return false;
#endif
}

#ifndef CONFIG_I2C_MANAGER_0_SDA
#define CONFIG_I2C_MANAGER_0_SDA (-1)
#endif
#ifndef CONFIG_I2C_MANAGER_0_SCL
#define CONFIG_I2C_MANAGER_0_SCL (-1)
#endif
#ifndef CONFIG_I2C_MANAGER_0_PULLUPS
#define CONFIG_I2C_MANAGER_0_PULLUPS 0
#endif
#ifndef CONFIG_I2C_MANAGER_1_SDA
#ifdef CONFIG_IS_ATOMS3R
#define CONFIG_I2C_MANAGER_1_SDA 2
#else
#define CONFIG_I2C_MANAGER_1_SDA (-1)
#endif
#endif
#ifndef CONFIG_I2C_MANAGER_1_SCL
#ifdef CONFIG_IS_ATOMS3R
#define CONFIG_I2C_MANAGER_1_SCL 1
#else
#define CONFIG_I2C_MANAGER_1_SCL (-1)
#endif
#endif
#ifndef CONFIG_I2C_MANAGER_1_PULLUPS
#define CONFIG_I2C_MANAGER_1_PULLUPS 0
#endif

/* CoreS3-SE DIN-base {SDA, SCL} candidates. The board profile covers PORT.A
 * (yellow=G2 SDA, white=G1 SCL, confirmed on hardware); units on the other
 * ports or reversed cables are probed in order below until the sensors
 * acknowledge. */
static int enviii_candidate_count(void) {
    return enviii_has_gps_i2c_conflict() ? 5 : 0;
}

static void enviii_candidate_pins(int idx, gpio_num_t *sda, gpio_num_t *scl) {
    static const gpio_num_t pins[][2] = {
        { GPIO_NUM_1, GPIO_NUM_2 },   /* PORT.A swapped */
        { GPIO_NUM_8, GPIO_NUM_9 },   /* PORT.B (white=G8, yellow=G9) */
        { GPIO_NUM_9, GPIO_NUM_8 },   /* PORT.B swapped */
        { GPIO_NUM_18, GPIO_NUM_17 }, /* PORT.C (white=G18, yellow=G17) */
        { GPIO_NUM_17, GPIO_NUM_18 }, /* PORT.C swapped */
    };
    *sda = pins[idx][0];
    *scl = pins[idx][1];
}

static gpio_num_t enviii_i2c_sda(void) {
    if (enviii_pin_candidate >= 0 && ENVIII_I2C_PORT == 1) {
        gpio_num_t sda, scl;
        enviii_candidate_pins(enviii_pin_candidate, &sda, &scl);
        return sda;
    }
    return (gpio_num_t)CONFIG_I2C_MANAGER_1_SDA;
}

static gpio_num_t enviii_i2c_scl(void) {
    if (enviii_pin_candidate >= 0 && ENVIII_I2C_PORT == 1) {
        gpio_num_t sda, scl;
        enviii_candidate_pins(enviii_pin_candidate, &sda, &scl);
        return scl;
    }
    return (gpio_num_t)CONFIG_I2C_MANAGER_1_SCL;
}

#ifndef CONFIG_ENVIII_SHT30_I2C_ADDR
#define CONFIG_ENVIII_SHT30_I2C_ADDR 0x44
#endif
#define SHT30_I2C_ADDR CONFIG_ENVIII_SHT30_I2C_ADDR

#ifndef CONFIG_ENVIII_QMP6988_I2C_ADDR
#define CONFIG_ENVIII_QMP6988_I2C_ADDR 0x70
#endif
#define QMP6988_I2C_ADDR CONFIG_ENVIII_QMP6988_I2C_ADDR

static void enviii_scan_and_log_bus(i2c_master_bus_handle_t bus) {
    char found[128];
    int pos = 0;
    found[0] = '\0';
    for (uint16_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 15) == ESP_OK) {
            pos += snprintf(found + pos, sizeof(found) - pos, "%s0x%02X",
                            pos ? " " : "", addr);
            if (pos >= (int)sizeof(found) - 6) break;
        }
    }
    ESP_LOGI(TAG, "I2C bus scan result: %s", pos ? found : "no devices found");
}

static esp_err_t enviii_get_bus(i2c_master_bus_handle_t *bus) {
    if (!bus) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = i2c_master_get_bus_handle(ENVIII_I2C_PORT, bus);
    if (ret == ESP_OK) return ESP_OK;

    gpio_num_t sda = GPIO_NUM_NC;
    gpio_num_t scl = GPIO_NUM_NC;
    bool pullups = false;

    if (ENVIII_I2C_PORT == 0) {
        sda = (gpio_num_t)CONFIG_I2C_MANAGER_0_SDA;
        scl = (gpio_num_t)CONFIG_I2C_MANAGER_0_SCL;
        pullups = CONFIG_I2C_MANAGER_0_PULLUPS;
    } else if (ENVIII_I2C_PORT == 1) {
        sda = enviii_i2c_sda();
        scl = enviii_i2c_scl();
        pullups = CONFIG_I2C_MANAGER_1_PULLUPS;
    }

    if (sda == GPIO_NUM_NC || scl == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "I2C bus %d unavailable and pins are not configured", ENVIII_I2C_PORT);
        return ret;
    }

    bool created = false;
    ret = i2c_shared_get_or_create_bus((i2c_port_num_t)ENVIII_I2C_PORT,
                                       sda,
                                       scl,
                                       pullups,
                                       bus,
                                       &created);
    if (ret == ESP_OK && created) {
        ESP_LOGI(TAG, "Created ENV-III I2C bus %d (SDA=%d, SCL=%d)", ENVIII_I2C_PORT, sda, scl);
        enviii_scan_and_log_bus(*bus);
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/* SHT30                                                                      */
/* -------------------------------------------------------------------------- */
#define SHT30_CMD_MEASURE_HIGH 0x2C06

static uint8_t sht30_crc8(uint8_t *data, uint8_t len) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht30_get_device(void) {
    if (s_sht30_dev) return ESP_OK;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = enviii_get_bus(&bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", ENVIII_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_shared_get_device(bus, SHT30_I2C_ADDR, 100000, &s_sht30_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add SHT30 device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sht30_write_cmd(uint16_t cmd) {
    esp_err_t ret = sht30_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    uint8_t payload[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    ret = i2c_master_transmit(s_sht30_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 write error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t sht30_read_data(uint8_t *data, size_t len) {
    esp_err_t ret = sht30_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    ret = i2c_master_receive(s_sht30_dev, data, len, 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SHT30 read error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static bool sht30_initialized = false;

static void sht30_init_hw(void) {
    if (sht30_initialized) return;
    esp_err_t ret = sht30_write_cmd(0x30A2);  /* soft reset */
    vTaskDelay(pdMS_TO_TICKS(5));
    sht30_initialized = ret == ESP_OK;
    if (sht30_initialized) {
        ESP_LOGI(TAG, "SHT30 initialized");
    }
}

static bool sht30_read(float *out_temp, float *out_hum) {
    if (sht30_write_cmd(SHT30_CMD_MEASURE_HIGH) != ESP_OK) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6];
    if (sht30_read_data(data, sizeof(data)) != ESP_OK) {
        return false;
    }

    if (sht30_crc8(&data[0], 2) != data[2]) {
        ESP_LOGW(TAG, "SHT30 temp CRC mismatch");
        return false;
    }
    if (sht30_crc8(&data[3], 2) != data[5]) {
        ESP_LOGW(TAG, "SHT30 hum CRC mismatch");
        return false;
    }

    uint16_t raw_temp = (data[0] << 8) | data[1];
    uint16_t raw_hum  = (data[3] << 8) | data[4];

    *out_temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *out_hum  = 100.0f * ((float)raw_hum / 65535.0f);
    return true;
}

/* -------------------------------------------------------------------------- */
/* QMP6988  (based on M5Stack M5Unit-ENV driver)                            */
/* -------------------------------------------------------------------------- */
#define QMP6988_REG_CHIP_ID         0xD1
#define QMP6988_REG_RESET           0xE0
#define QMP6988_REG_CTRL_MEAS       0xF4
#define QMP6988_REG_CONFIG          0xF1
#define QMP6988_REG_PRESS_MSB       0xF7
#define QMP6988_REG_CALIBRATION     0xA0
#define QMP6988_CALIB_LEN           25
#define QMP6988_CHIP_ID_VAL         0x5C
#define QMP6988_SOFT_RESET_VAL      0xE6
#define QMP6988_SUBTRACTOR          8388608

typedef struct {
    int32_t  COE_a0;   /* 20Q4 */
    int16_t  COE_a1;
    int16_t  COE_a2;
    int32_t  COE_b00;  /* 20Q4 */
    int16_t  COE_bt1;
    int16_t  COE_bt2;
    int16_t  COE_bp1;
    int16_t  COE_b11;
    int16_t  COE_bp2;
    int16_t  COE_b12;
    int16_t  COE_b21;
    int16_t  COE_bp3;
} qmp6988_cali_data_t;

typedef struct {
    int32_t a0, b00;
    int32_t a1, a2;
    int64_t bt1, bt2, bp1, b11, bp2, b12, b21, bp3;
} qmp6988_ik_data_t;

static qmp6988_cali_data_t qmp_cali = {0};
static qmp6988_ik_data_t qmp_ik = {0};
static bool qmp6988_initialized = false;

static esp_err_t qmp6988_get_device(void) {
    if (s_qmp6988_dev) return ESP_OK;

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = enviii_get_bus(&bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", ENVIII_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_shared_get_device(bus, QMP6988_I2C_ADDR, 100000, &s_qmp6988_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add QMP6988 device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmp6988_write_reg(uint8_t reg, uint8_t val) {
    esp_err_t ret = qmp6988_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    uint8_t payload[2] = {reg, val};
    ret = i2c_master_transmit(s_qmp6988_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMP6988 write error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmp6988_read_bytes(uint8_t reg, uint8_t *data, size_t len) {
    esp_err_t ret = qmp6988_get_device();
    if (ret != ESP_OK) return ret;

    bool locked = i2c_bus_lock(ENVIII_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    ret = i2c_master_transmit_receive(s_qmp6988_dev, &reg, sizeof(reg), data, len, 50);
    i2c_bus_unlock(ENVIII_I2C_PORT);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMP6988 read error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void qmp6988_read_calibration(void) {
    uint8_t buf[32] = {0};
    if (qmp6988_read_bytes(QMP6988_REG_CALIBRATION, buf, QMP6988_CALIB_LEN) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read QMP6988 calibration");
        return;
    }

    /* M5Stack QMP6988 coefficient extraction (scattered across 25 bytes) */
    qmp_cali.COE_a0 = (int32_t)(((buf[18] << 12) | (buf[19] << 4) | (buf[24] & 0x0f)) << 12);
    qmp_cali.COE_a0 = qmp_cali.COE_a0 >> 12;

    qmp_cali.COE_a1  = (int16_t)((buf[20] << 8) | buf[21]);
    qmp_cali.COE_a2  = (int16_t)((buf[22] << 8) | buf[23]);

    qmp_cali.COE_b00 = (int32_t)(((buf[0] << 12) | (buf[1] << 4) | ((buf[24] & 0xf0) >> 4)) << 12);
    qmp_cali.COE_b00 = qmp_cali.COE_b00 >> 12;

    qmp_cali.COE_bt1 = (int16_t)((buf[2] << 8) | buf[3]);
    qmp_cali.COE_bt2 = (int16_t)((buf[4] << 8) | buf[5]);
    qmp_cali.COE_bp1 = (int16_t)((buf[6] << 8) | buf[7]);
    qmp_cali.COE_b11 = (int16_t)((buf[8] << 8) | buf[9]);
    qmp_cali.COE_bp2 = (int16_t)((buf[10] << 8) | buf[11]);
    qmp_cali.COE_b12 = (int16_t)((buf[12] << 8) | buf[13]);
    qmp_cali.COE_b21 = (int16_t)((buf[14] << 8) | buf[15]);
    qmp_cali.COE_bp3 = (int16_t)((buf[16] << 8) | buf[17]);

    ESP_LOGI(TAG, "QMP6988 cal: a0=%ld a1=%d a2=%d b00=%ld",
             qmp_cali.COE_a0, qmp_cali.COE_a1, qmp_cali.COE_a2, qmp_cali.COE_b00);
    ESP_LOGI(TAG, "QMP6988 cal: bt1=%d bt2=%d bp1=%d b11=%d bp2=%d b12=%d b21=%d bp3=%d",
             qmp_cali.COE_bt1, qmp_cali.COE_bt2, qmp_cali.COE_bp1, qmp_cali.COE_b11,
             qmp_cali.COE_bp2, qmp_cali.COE_b12, qmp_cali.COE_b21, qmp_cali.COE_bp3);

    /* Integer coefficient transforms (M5Stack driver) */
    qmp_ik.a0  = qmp_cali.COE_a0;   /* 20Q4 */
    qmp_ik.b00 = qmp_cali.COE_b00;  /* 20Q4 */

    qmp_ik.a1  = 3608L * (int32_t)qmp_cali.COE_a1 - 1731677965L;   /* 31Q23 */
    qmp_ik.a2  = 16889L * (int32_t)qmp_cali.COE_a2 - 87619360L;     /* 30Q47 */
    qmp_ik.bt1 = 2982L * (int64_t)qmp_cali.COE_bt1 + 107370906L;    /* 28Q15 */
    qmp_ik.bt2 = 329854L * (int64_t)qmp_cali.COE_bt2 + 108083093L;  /* 34Q38 */
    qmp_ik.bp1 = 19923L * (int64_t)qmp_cali.COE_bp1 + 1133836764L;  /* 31Q20 */
    qmp_ik.b11 = 2406L * (int64_t)qmp_cali.COE_b11 + 118215883L;    /* 28Q34 */
    qmp_ik.bp2 = 3079L * (int64_t)qmp_cali.COE_bp2 - 181579595L;    /* 29Q43 */
    qmp_ik.b12 = 6846L * (int64_t)qmp_cali.COE_b12 + 85590281L;     /* 29Q53 */
    qmp_ik.b21 = 13836L * (int64_t)qmp_cali.COE_b21 + 79333336L;     /* 29Q60 */
    qmp_ik.bp3 = 2915L * (int64_t)qmp_cali.COE_bp3 + 157155561L;    /* 28Q65 */
}

/* Temperature compensation: returns 8.8 fixed-point (divide by 256 for °C) */
static int16_t qmp6988_convTx02e(qmp6988_ik_data_t *ik, int32_t dt) {
    int64_t wk1, wk2;

    wk1 = ((int64_t)ik->a1 * (int64_t)dt);           /* 31Q23 + 24-1 = 54Q23 */
    wk2 = ((int64_t)ik->a2 * (int64_t)dt) >> 14;     /* 30Q47 + 24-1 = 53 -> 39Q33 */
    wk2 = (wk2 * (int64_t)dt) >> 10;                 /* 39Q33 + 24-1 = 62 -> 52Q23 */
    wk2 = ((wk1 + wk2) / 32767) >> 19;               /* 55Q23 -> 20Q04 */
    return (int16_t)((ik->a0 + wk2) >> 4);            /* 21Q4 -> 17Q0 (8.8) */
}

/* Pressure compensation: returns 20.4 fixed-point (divide by 16 for Pa) */
static int32_t qmp6988_getPressure02e(qmp6988_ik_data_t *ik, int32_t dp, int16_t tx) {
    int64_t wk1, wk2, wk3;

    wk1 = ((int64_t)ik->bt1 * (int64_t)tx);           /* 28Q15 + 16-1 = 43Q15 */
    wk2 = ((int64_t)ik->bp1 * (int64_t)dp) >> 5;      /* 31Q20 + 24-1 = 54 -> 49Q15 */
    wk1 += wk2;
    wk2 = ((int64_t)ik->bt2 * (int64_t)tx) >> 1;      /* 34Q38 + 16-1 = 49 -> 48Q37 */
    wk2 = (wk2 * (int64_t)tx) >> 8;                   /* 48Q37 + 16-1 = 63 -> 55Q29 */
    wk3 = wk2;
    wk2 = ((int64_t)ik->b11 * (int64_t)tx) >> 4;      /* 28Q34 + 16-1 = 43 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q30 + 24-1 = 62 -> 61Q29 */
    wk3 += wk2;
    wk2 = ((int64_t)ik->bp2 * (int64_t)dp) >> 13;     /* 29Q43 + 24-1 = 52 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q30 + 24-1 = 62 -> 61Q29 */
    wk3 += wk2;
    wk1 += wk3 >> 14;                                 /* Q29 >> 14 = Q15 */
    wk2 = ((int64_t)ik->b12 * (int64_t)tx);           /* 29Q53 + 16-1 = 45Q53 */
    wk2 = (wk2 * (int64_t)tx) >> 22;                  /* 45Q53 + 16-1 = 61 -> 39Q31 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q31 + 24-1 = 62 -> 61Q30 */
    wk3 = wk2;
    wk2 = ((int64_t)ik->b21 * (int64_t)tx) >> 6;      /* 29Q60 + 16-1 = 45 -> 39Q54 */
    wk2 = (wk2 * (int64_t)dp) >> 23;                  /* 39Q54 + 24-1 = 62 -> 39Q31 */
    wk2 = (wk2 * (int64_t)dp) >> 1;                   /* 39Q31 + 24-1 = 62 -> 61Q30 */
    wk3 += wk2;
    wk2 = ((int64_t)ik->bp3 * (int64_t)dp) >> 12;     /* 28Q65 + 24-1 = 51 -> 39Q53 */
    wk2 = (wk2 * (int64_t)dp) >> 23;                  /* 39Q53 + 24-1 = 62 -> 39Q30 */
    wk2 = (wk2 * (int64_t)dp);                        /* 39Q30 + 24-1 = 62Q30 */
    wk3 += wk2;
    wk1 += wk3 >> 15;                                 /* Q30 >> 15 = Q15 */
    wk1 /= 32767L;
    wk1 >>= 11;                                       /* Q15 >> 11 = Q4 */
    wk1 += ik->b00;                                   /* Q4 + 20Q4 */
    return (int32_t)wk1;
}

static void qmp6988_init_hw(void) {
    if (qmp6988_initialized) return;

    uint8_t chip_id = 0;
    if (qmp6988_read_bytes(QMP6988_REG_CHIP_ID, &chip_id, 1) != ESP_OK || chip_id != QMP6988_CHIP_ID_VAL) {
        ESP_LOGE(TAG, "QMP6988 not found (ID: 0x%02X)", chip_id);
        return;
    }
    ESP_LOGI(TAG, "Found QMP6988 ID: 0x%02X", chip_id);

    /* Soft reset */
    qmp6988_write_reg(QMP6988_REG_RESET, QMP6988_SOFT_RESET_VAL);
    vTaskDelay(pdMS_TO_TICKS(20));

    qmp6988_read_calibration();

    /* Normal mode, P oversampling 8x, T oversampling 1x, filter coeff 4 */
    qmp6988_write_reg(QMP6988_REG_CTRL_MEAS, 0x33);
    qmp6988_write_reg(QMP6988_REG_CONFIG, 0x02);
    vTaskDelay(pdMS_TO_TICKS(20));

    qmp6988_initialized = true;
    ESP_LOGI(TAG, "QMP6988 initialized");
}

static bool qmp6988_read(float *out_press_hpa, float *out_temp_c) {
    uint8_t data[6] = {0};
    if (qmp6988_read_bytes(QMP6988_REG_PRESS_MSB, data, 6) != ESP_OK) {
        return false;
    }

    uint32_t P_read = ((uint32_t)data[0] << 16) | ((uint32_t)data[1] << 8) | data[2];
    uint32_t T_read = ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 8) | data[5];

    int32_t P_raw = (int32_t)(P_read - QMP6988_SUBTRACTOR);
    int32_t T_raw = (int32_t)(T_read - QMP6988_SUBTRACTOR);

    int16_t T_int = qmp6988_convTx02e(&qmp_ik, T_raw);
    int32_t P_int = qmp6988_getPressure02e(&qmp_ik, P_raw, T_int);

    *out_temp_c = (float)T_int / 256.0f;
    *out_press_hpa = (float)P_int / 16.0f / 100.0f;  /* Pa -> hPa */
    return true;
}

/* -------------------------------------------------------------------------- */
/* UI Update                                                                  */
/* -------------------------------------------------------------------------- */
static float last_temp = 0.0f;
static float last_hum = 0.0f;
static float last_press = 0.0f;

static float apply_filter(float prev, float curr, float alpha) {
    return (prev * (1.0f - alpha)) + (curr * alpha);
}

static float calculate_dew_point(float temp_c, float humidity_pct) {
    if (humidity_pct <= 0.0f) return NAN;

    const float a = 17.62f;
    const float b = 243.12f;
    float gamma = (a * temp_c / (b + temp_c)) + logf(humidity_pct / 100.0f);
    return (b * gamma) / (a - gamma);
}

static float calculate_feels_like(float temp_c, float humidity_pct) {
    if (temp_c < 26.7f || humidity_pct < 40.0f) return temp_c;

    float temp_f = (temp_c * 9.0f / 5.0f) + 32.0f;
    float hi_f = -42.379f + 2.04901523f * temp_f + 10.14333127f * humidity_pct
               - 0.22475541f * temp_f * humidity_pct
               - 0.00683783f * temp_f * temp_f
               - 0.05481717f * humidity_pct * humidity_pct
               + 0.00122874f * temp_f * temp_f * humidity_pct
               + 0.00085282f * temp_f * humidity_pct * humidity_pct
               - 0.00000199f * temp_f * temp_f * humidity_pct * humidity_pct;
    return (hi_f - 32.0f) * 5.0f / 9.0f;
}

static const char *get_comfort_text(float temp_c, float humidity_pct) {
    if (temp_c < 12.0f) return "Cold";
    if (temp_c > 30.0f) return humidity_pct >= 60.0f ? "Hot + Muggy" : "Hot";
    if (humidity_pct < 30.0f) return "Dry";
    if (humidity_pct > 70.0f) return "Humid";
    if (temp_c >= 18.0f && temp_c <= 26.0f && humidity_pct >= 35.0f && humidity_pct <= 60.0f) return "Comfortable";
    return "Okay";
}

static const char *get_weather_hint_text(bool has_sht, bool has_qmp, float temp_c, float humidity_pct, float press_hpa) {
    if (has_qmp) {
        if (press_hpa < 1000.0f) return "Low pressure";
        if (press_hpa > 1025.0f) return "High pressure";
    }
    if (has_sht) {
        float dew = calculate_dew_point(temp_c, humidity_pct);
        if (isfinite(dew) && dew >= 20.0f) return "Muggy air";
        if (humidity_pct < 30.0f) return "Dry air";
        if (humidity_pct > 70.0f) return "Humid air";
    }
    if (has_qmp) return "Stable pressure";
    return "Waiting...";
}

static void enviii_timer_cb(lv_timer_t *timer) {
    (void)timer;

    float temp = 0.0f, hum = 0.0f, press = 0.0f, qmp_temp = 0.0f;
    bool sht_ok = false;
    bool qmp_ok = false;

    if (sht30_initialized) {
        sht_ok = sht30_read(&temp, &hum);
    }

    if (qmp6988_initialized) {
        qmp_ok = qmp6988_read(&press, &qmp_temp);
    }

    static uint32_t last_log = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    bool do_log = (now - last_log > 3000);
    if (do_log) last_log = now;

    if (sht_ok) {
        sht_data_valid = true;
        last_temp = apply_filter(last_temp, temp, 0.3f);
        last_hum  = apply_filter(last_hum, hum, 0.3f);
        if (do_log) {
            ESP_LOGI(TAG, "SHT30 raw: T=%.2f C  H=%.2f %%", (double)temp, (double)hum);
        }
    } else if (do_log) {
        ESP_LOGW(TAG, "SHT30 read failed");
    }

    if (qmp_ok) {
        /* Sanity-check: real atmospheric pressure is 300-1100 hPa */
        if (press >= 300.0f && press <= 1100.0f) {
            qmp_data_valid = true;
            last_press = apply_filter(last_press, press, 0.3f);
            if (do_log) {
                ESP_LOGI(TAG, "QMP6988 raw: P=%.2f hPa  T=%.2f C", (double)press, (double)qmp_temp);
            }
        } else {
            ESP_LOGW(TAG, "QMP6988 pressure out of range: %.2f hPa (raw_press/T from regs)", (double)press);
        }
    } else if (do_log) {
        ESP_LOGW(TAG, "QMP6988 read failed");
    }

    char buf[64];

    if (temp_label) {
        if (sht_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f C / %.1f F",
                     (double)last_temp, (double)(last_temp * 9.0f / 5.0f + 32.0f));
            lv_label_set_text(temp_label, buf);
        } else {
            lv_label_set_text(temp_label, "--.- C / --.- F");
        }
    }
    if (hum_label) {
        if (sht_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f %%", (double)last_hum);
            lv_label_set_text(hum_label, buf);
        } else {
            lv_label_set_text(hum_label, "--.- %");
        }
    }
    if (comfort_label) {
        lv_label_set_text(comfort_label, sht_data_valid ? get_comfort_text(last_temp, last_hum) : "--");
    }
    if (feels_like_label) {
        if (sht_data_valid) {
            float feels = calculate_feels_like(last_temp, last_hum);
            snprintf(buf, sizeof(buf), "%.1f C / %.1f F",
                     (double)feels, (double)(feels * 9.0f / 5.0f + 32.0f));
            lv_label_set_text(feels_like_label, buf);
        } else {
            lv_label_set_text(feels_like_label, "--.- C / --.- F");
        }
    }
    if (weather_hint_label) {
        lv_label_set_text(weather_hint_label,
                          get_weather_hint_text(sht_data_valid, qmp_data_valid, last_temp, last_hum, last_press));
    }
    if (dew_label) {
        if (sht_data_valid) {
            float dew = calculate_dew_point(last_temp, last_hum);
            if (isfinite(dew)) {
                snprintf(buf, sizeof(buf), "%.1f C / %.1f F",
                         (double)dew, (double)(dew * 9.0f / 5.0f + 32.0f));
                lv_label_set_text(dew_label, buf);
            } else {
                lv_label_set_text(dew_label, "--.- C / --.- F");
            }
        } else {
            lv_label_set_text(dew_label, "--.- C / --.- F");
        }
    }
    if (press_hpa_label) {
        if (qmp_data_valid) {
            snprintf(buf, sizeof(buf), "%.1f hPa", (double)last_press);
            lv_label_set_text(press_hpa_label, buf);
        } else {
            lv_label_set_text(press_hpa_label, "---.- hPa");
        }
    }
    if (press_inhg_label) {
        if (qmp_data_valid) {
            snprintf(buf, sizeof(buf), "%.2f inHg", (double)(last_press * 0.02953f));
            lv_label_set_text(press_inhg_label, buf);
        } else {
            lv_label_set_text(press_inhg_label, "--.-- inHg");
        }
    }
    if (alt_label) {
        if (qmp_data_valid) {
            float altitude = 44330.0f * (1.0f - powf(last_press / sea_level_pressure, 0.1903f));
            snprintf(buf, sizeof(buf), "%.0f m / %.0f ft",
                     (double)altitude, (double)(altitude * 3.28084f));
            lv_label_set_text(alt_label, buf);
        } else {
            lv_label_set_text(alt_label, "--- m / --- ft");
        }
    }

}

/* -------------------------------------------------------------------------- */
/* Input Handling                                                             */
/* -------------------------------------------------------------------------- */
#ifdef CONFIG_USE_TOUCHSCREEN
static bool enviii_point_in_obj(const lv_point_t *p, lv_obj_t *obj) {
    if (!p) return false;
    return gui_touch_bar_hit(obj, p->x, p->y);
}
#endif

static void enviii_event_handler(InputEvent *event) {
    if (!event) return;
#ifdef CONFIG_USE_TOUCHSCREEN
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;

        if (data->state == LV_INDEV_STATE_PR) {
            if (enviii_point_in_obj(&data->point, enviii_btn_up)) {
                enviii_scroll_content(-1);
                touch_drag_reset(&enviii_touch_drag);
                return;
            }
            if (enviii_point_in_obj(&data->point, enviii_btn_down)) {
                enviii_scroll_content(1);
                touch_drag_reset(&enviii_touch_drag);
                return;
            }
            if (enviii_point_in_obj(&data->point, enviii_btn_back)) {
                display_manager_go_back();
                touch_drag_reset(&enviii_touch_drag);
                return;
            }

            if (!enviii_touch_drag.started) {
                touch_drag_begin(&enviii_touch_drag, data);
            } else if (enviii_content) {
                touch_drag_update(&enviii_touch_drag, data, enviii_content);
            }
        } else if (data->state == LV_INDEV_STATE_REL) {
            if (!enviii_touch_drag.started) return;
            touch_drag_release(&enviii_touch_drag, data);
        }
        return;
    }
#endif
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == LV_KEY_UP || key == ';' || key == 'k') {
            enviii_scroll_content(-1);
        } else if (key == LV_KEY_DOWN || key == '.' || key == 'j') {
            enviii_scroll_content(1);
        } else if (key == LV_KEY_ESC || key == 27 || key == 29 || key == '`') {
            display_manager_go_back();
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 2) enviii_scroll_content(-1);
        else if (button == 4) enviii_scroll_content(1);
        else display_manager_go_back();
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
    }
}

static void enviii_scroll_content(int dir) {
    if (!enviii_content) return;
    lv_coord_t step = lv_obj_get_height(enviii_content) / 2;
    if (step < 24) step = 24;
    lv_obj_scroll_by_bounded(enviii_content, 0, dir > 0 ? -step : step, LV_ANIM_OFF);
}

static void get_enviii_callback(void **callback) {
    if (callback) *callback = (void *)enviii_event_handler;
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
static void refresh_theme_colors(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    accent_color = theme_palette_get_accent(theme);
    bg_color = theme_palette_get_background(theme);
    card_color = theme_palette_get_surface(theme);
    text_color = theme_palette_get_text(theme);
    dim_color = theme_palette_get_text_muted(theme);
}

static lv_obj_t *make_card(lv_obj_t *parent, int width_pct) {
    lv_obj_t *card = lv_obj_create(parent);
    int padding = LV_VER_RES <= 100 ? 3 : (LV_VER_RES <= 160 ? 5 : 8);
    lv_obj_set_size(card, LV_PCT(width_pct), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent_color), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_pad_all(card, padding, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(text_color), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 2, 0);
    return card;
}

static lv_obj_t *make_metric_card(lv_obj_t *parent, int width_pct, const char *title, const char *initial, bool prominent) {
    lv_obj_t *card = make_card(parent, width_pct);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(dim_color), 0);
    lv_obj_set_style_text_font(title_label, accessibility_get_font_small(), 0);

    lv_obj_t *value_label = lv_label_create(card);
    lv_label_set_text(value_label, initial);
    lv_obj_set_width(value_label, LV_PCT(100));
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(value_label, prominent ? lv_color_hex(accent_color) : lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(value_label, prominent ? accessibility_get_font_title() : accessibility_get_font_body(), 0);
    return value_label;
}

#ifdef CONFIG_USE_TOUCHSCREEN
static void create_touch_control_bar(lv_obj_t *root) {
    if (!root) return;

    s_touch_tb = gui_touch_bar_create(root);
    touch_bar = s_touch_tb.bar;
    enviii_btn_up = s_touch_tb.up_btn;
    enviii_btn_back = s_touch_tb.back_btn;
    enviii_btn_down = s_touch_tb.down_btn;
    /* This view keeps the scroll arrows always visible (no overflow gating);
     * the helper starts them hidden, so un-hide to preserve the original look.
     * Button dispatch stays manual (enviii_event_handler PR stage). */
    if (enviii_btn_up && lv_obj_is_valid(enviii_btn_up))
        lv_obj_clear_flag(enviii_btn_up, LV_OBJ_FLAG_HIDDEN);
    if (enviii_btn_down && lv_obj_is_valid(enviii_btn_down))
        lv_obj_clear_flag(enviii_btn_down, LV_OBJ_FLAG_HIDDEN);
}
#endif

/* -------------------------------------------------------------------------- */
/* Create / Destroy                                                           */
/* -------------------------------------------------------------------------- */
void enviii_create(void) {
#ifdef CONFIG_USE_TOUCHSCREEN
    touch_drag_reset(&enviii_touch_drag);
#endif
    /* CoreS3-SE routes the default GPS RX pin (GPIO2) over ENV-III's I2C1
     * SDA line. Stop GPS before taking that bus so UART pin-matrix setup cannot
     * corrupt sensor transactions. */
    gps_stopped_by_enviii = false;
    enviii_released_i2c_bus = false;
    if (enviii_has_gps_i2c_conflict() && g_gpsManager.isinitilized) {
        gps_manager_deinit(&g_gpsManager);
        gps_stopped_by_enviii = true;
        esp_err_t bus_ret = i2c_shared_release_bus((i2c_port_num_t)ENVIII_I2C_PORT);
        enviii_released_i2c_bus = bus_ret == ESP_OK || bus_ret == ESP_ERR_NOT_FOUND;
    }
    refresh_theme_colors();
    display_manager_fill_screen(lv_color_hex(bg_color));
    enviii_container = gui_screen_create_root(NULL, "ENV-III", lv_color_hex(bg_color), LV_OPA_COVER);
    enviii_view.root = enviii_container;

    lv_obj_t *content = gui_screen_create_content(enviii_container, GUI_STATUS_BAR_HEIGHT);
    enviii_content = content;
#ifdef CONFIG_USE_TOUCHSCREEN
    const int touch_bar_h = gui_touch_bar_height();
    lv_obj_set_size(content, LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_HEIGHT - touch_bar_h);
#endif
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_text_color(content, lv_color_hex(text_color), 0);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, LV_VER_RES <= 100 ? 2 : 4, 0);

    temp_label = make_metric_card(content, 100, "Temperature", "--.- C / --.- F", true);

    lv_obj_t *summary_row = lv_obj_create(content);
    lv_obj_set_size(summary_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(summary_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(summary_row, 0, 0);
    lv_obj_set_style_pad_all(summary_row, 0, 0);
    lv_obj_set_flex_flow(summary_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summary_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(summary_row, LV_VER_RES <= 100 ? 2 : 4, 0);
    lv_obj_clear_flag(summary_row, LV_OBJ_FLAG_SCROLLABLE);

    comfort_label = make_metric_card(summary_row, 48, "Comfort", "--", false);
    weather_hint_label = make_metric_card(summary_row, 48, "Weather", "Waiting...", false);

    lv_obj_t *weather_row = lv_obj_create(content);
    lv_obj_set_size(weather_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(weather_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_row, 0, 0);
    lv_obj_set_style_pad_all(weather_row, 0, 0);
    lv_obj_set_flex_flow(weather_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(weather_row, LV_VER_RES <= 100 ? 2 : 4, 0);
    lv_obj_clear_flag(weather_row, LV_OBJ_FLAG_SCROLLABLE);

    feels_like_label = make_metric_card(weather_row, 48, "Feels Like", "--.- C / --.- F", false);
    hum_label = make_metric_card(weather_row, 48, "Humidity", "--.- %", false);

    lv_obj_t *dew_alt_row = lv_obj_create(content);
    lv_obj_set_size(dew_alt_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(dew_alt_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dew_alt_row, 0, 0);
    lv_obj_set_style_pad_all(dew_alt_row, 0, 0);
    lv_obj_set_flex_flow(dew_alt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dew_alt_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(dew_alt_row, LV_VER_RES <= 100 ? 2 : 4, 0);
    lv_obj_clear_flag(dew_alt_row, LV_OBJ_FLAG_SCROLLABLE);

    dew_label = make_metric_card(dew_alt_row, 48, "Dew Point", "--.- C / --.- F", false);
    alt_label = make_metric_card(dew_alt_row, 48, "Altitude", "--- m / --- ft", false);

    lv_obj_t *pressure_row = lv_obj_create(content);
    lv_obj_set_size(pressure_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pressure_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pressure_row, 0, 0);
    lv_obj_set_style_pad_all(pressure_row, 0, 0);
    lv_obj_set_flex_flow(pressure_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pressure_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(pressure_row, LV_VER_RES <= 100 ? 2 : 4, 0);
    lv_obj_clear_flag(pressure_row, LV_OBJ_FLAG_SCROLLABLE);

    press_hpa_label = make_metric_card(pressure_row, 48, "Pressure", "---.- hPa", false);
    press_inhg_label = make_metric_card(pressure_row, 48, "Pressure", "--.-- inHg", false);

    display_manager_add_status_bar("ENV-III");
#ifdef CONFIG_USE_TOUCHSCREEN
    create_touch_control_bar(enviii_container);
#endif

    sht_data_valid = false;
    qmp_data_valid = false;
    sea_level_pressure = 1013.25f;
    last_temp = 0.0f;
    last_hum = 0.0f;
    last_press = 0.0f;

    sht30_initialized = false;
    qmp6988_initialized = false;

    /* On the CoreS3-SE the grove ports' 5V comes from a boost converter gated
     * by the AW9523 expander (enabled at boot). Re-assert it here so a unit
     * hot-plugged after boot is powered, and let it settle before probing. */
    if (aw9523_set_port_5v(true) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    bool found = false;
    int attempts = enviii_candidate_count() + 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        if (attempt > 0) {
            s_sht30_dev = NULL;
            s_qmp6988_dev = NULL;
            i2c_shared_release_bus((i2c_port_num_t)ENVIII_I2C_PORT);
            enviii_released_i2c_bus = true;
            enviii_pin_candidate = attempt - 1;
            ESP_LOGW(TAG, "No ENV-III response; probing SDA=%d, SCL=%d",
                     (int)enviii_i2c_sda(), (int)enviii_i2c_scl());
        }
        sht30_init_hw();
        qmp6988_init_hw();
        if (sht30_initialized || qmp6988_initialized) {
            found = true;
            break;
        }
    }

    if (found) {
        enviii_timer = lv_timer_create(enviii_timer_cb, 500, NULL);
        /* immediate first sample */
        enviii_timer_cb(enviii_timer);
    } else {
        ESP_LOGE(TAG, "ENV-III not found on any CoreS3-SE DIN port (G1/G2, G8/G9, G17/G18)");
    }
}

void enviii_destroy(void) {
    if (enviii_timer) {
        lv_timer_del(enviii_timer);
        enviii_timer = NULL;
    }
    if (enviii_released_i2c_bus) {
        i2c_shared_release_bus((i2c_port_num_t)ENVIII_I2C_PORT);
        enviii_released_i2c_bus = false;
    }
    enviii_pin_candidate = -1;
    if (gps_stopped_by_enviii) {
        gps_manager_init(&g_gpsManager);
        gps_stopped_by_enviii = false;
    }
    /* Shared I2C device handles remain cached for other Grove apps. */
    s_sht30_dev = NULL;
    s_qmp6988_dev = NULL;
    if (enviii_container) {
        lv_obj_del(enviii_container);
        enviii_container = NULL;
        enviii_view.root = NULL;
    }
    enviii_content = NULL;
    temp_label = NULL;
    comfort_label = NULL;
    feels_like_label = NULL;
    weather_hint_label = NULL;
    hum_label = NULL;
    dew_label = NULL;
    press_hpa_label = NULL;
    press_inhg_label = NULL;
    alt_label = NULL;
    gui_touch_bar_destroy(&s_touch_tb);
    touch_bar = NULL;
    enviii_btn_up = NULL;
    enviii_btn_back = NULL;
    enviii_btn_down = NULL;
    sht30_initialized = false;
    qmp6988_initialized = false;
    sht_data_valid = false;
    qmp_data_valid = false;
}

View enviii_view = {
    .root = NULL,
    .create = enviii_create,
    .destroy = enviii_destroy,
    .input_callback = enviii_event_handler,
    .name = "ENV-III",
    .get_hardwareinput_callback = get_enviii_callback
};

#endif /* CONFIG_HAS_ENVIII */
