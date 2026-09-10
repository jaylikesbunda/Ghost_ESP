#include "managers/views/compass_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/design_tokens.h"
#include "gui/lvgl_safe.h"
#include "gui/view_input.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus_lock.h"
#include "managers/bmi270_driver.h"
#include <math.h>

#ifdef CONFIG_HAS_COMPASS

static const char *TAG = "CompassScreen";

static lv_obj_t *compass_container = NULL;
static lv_obj_t *ring = NULL;  // The rotating ring with cardinal labels
static lv_obj_t *needle_line_n = NULL;  // North half (red) - FIXED pointing up
static lv_obj_t *needle_line_s = NULL;  // South half (white) - FIXED pointing down
static lv_point_t pts_n[2];
static lv_point_t pts_s[2];
static lv_obj_t *heading_label = NULL;

#define PI 3.14159265f
#ifdef CONFIG_IS_ATOMS3R
#define RING_SIZE 76
#define CARDINAL_RADIUS 30
#define NEEDLE_LEN 24
#else
#define RING_SIZE 200
#define CARDINAL_RADIUS 85  // Distance from center to N/S/E/W labels
#define NEEDLE_LEN 70
#endif

static int compass_ring_size = RING_SIZE;
static int compass_cardinal_radius = CARDINAL_RADIUS;
static int compass_needle_len = NEEDLE_LEN;
static int compass_ring_center = RING_SIZE / 2;

static lv_obj_t *lbl_n = NULL;
static lv_obj_t *lbl_s = NULL;
static lv_obj_t *lbl_e = NULL;
static lv_obj_t *lbl_w = NULL;

static int cardinal_half_w = 0;
static int cardinal_half_h = 0;

static void update_cardinal_positions(float heading_deg) {
    float ring_rotation = -heading_deg * PI / 180.0f;
    
    // Calculate once, apply to all
    float sin_n = sinf(ring_rotation);
    float cos_n = cosf(ring_rotation);
    
    // Use lv_obj_set_pos instead of lv_obj_align (faster, no layout recalc)
    int cx = compass_ring_center;
    int cy = compass_ring_center;
    int w = cardinal_half_w;
    int h = cardinal_half_h;
    
    if (lbl_n) lv_obj_set_pos(lbl_n, cx + (int)(sin_n * compass_cardinal_radius) - w,
                                      cy - (int)(cos_n * compass_cardinal_radius) - h);
    if (lbl_s) lv_obj_set_pos(lbl_s, cx - (int)(sin_n * compass_cardinal_radius) - w,
                                      cy + (int)(cos_n * compass_cardinal_radius) - h);
    if (lbl_e) lv_obj_set_pos(lbl_e, cx + (int)(cos_n * compass_cardinal_radius) - w,
                                      cy + (int)(sin_n * compass_cardinal_radius) - h);
    if (lbl_w) lv_obj_set_pos(lbl_w, cx - (int)(cos_n * compass_cardinal_radius) - w,
                                      cy - (int)(sin_n * compass_cardinal_radius) - h);
}

static void init_fixed_needle(void) {
    // Needle always points straight up (north half) and down (south half)
    pts_n[0].x = compass_ring_center;
    pts_n[0].y = compass_ring_center;
    pts_n[1].x = compass_ring_center;
    pts_n[1].y = compass_ring_center - compass_needle_len;  // Up
    
    pts_s[0].x = compass_ring_center;
    pts_s[0].y = compass_ring_center;
    pts_s[1].x = compass_ring_center;
    pts_s[1].y = compass_ring_center + compass_needle_len;  // Down
    
    if (needle_line_n) lv_line_set_points(needle_line_n, pts_n, 2);
    if (needle_line_s) lv_line_set_points(needle_line_s, pts_s, 2);
}

static lv_timer_t *compass_timer = NULL;
static i2c_master_dev_handle_t s_compass_dev = NULL;

#ifdef CONFIG_COMPASS_USE_BMM150
#define COMPASS_I2C_ADDR CONFIG_BMM150_I2C_ADDR
#else
#define COMPASS_I2C_ADDR 0x7C
#endif

#ifndef CONFIG_COMPASS_I2C_PORT
#define CONFIG_COMPASS_I2C_PORT 0
#endif
#define COMPASS_I2C_PORT CONFIG_COMPASS_I2C_PORT

static esp_err_t compass_get_device(void) {
    if (s_compass_dev) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(COMPASS_I2C_PORT, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", COMPASS_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = COMPASS_I2C_ADDR,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_compass_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add compass device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmc6309_write_reg(uint8_t reg, uint8_t val) {
    esp_err_t ret = compass_get_device();
    if (ret != ESP_OK) {
        return ret;
    }
    
    bool locked = i2c_bus_lock(COMPASS_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;
    
    uint8_t payload[2] = {reg, val};
    ret = i2c_master_transmit(s_compass_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(COMPASS_I2C_PORT);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C Write Error: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t qmc6309_read_bytes(uint8_t reg, uint8_t *data, size_t len) {
    esp_err_t ret = compass_get_device();
    if (ret != ESP_OK) {
        return ret;
    }
    
    bool locked = i2c_bus_lock(COMPASS_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;
    
    ret = i2c_master_transmit_receive(s_compass_dev, &reg, sizeof(reg), data, len, 50);
    i2c_bus_unlock(COMPASS_I2C_PORT);
    
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C Read Error: %s", esp_err_to_name(ret));
    }
    return ret;
}

// QMC6309 Register Mapping
#define QMC6309_REG_CHIP_ID     0x00
#define QMC6309_REG_DATA_START  0x01
#define QMC6309_REG_STATUS      0x09
#define QMC6309_REG_CTRL1       0x0A
#define QMC6309_REG_CTRL2       0x0B
#define QMC6309_REG_RESET       0x0B

#define QMC6309_CHIP_ID_VAL     0x90
#define QMC6309_STATUS_DRDY     0x01
#define QMC6309_STATUS_OVL      0x02

#define QMC6309_MODE_SUSPEND    0x00
#define QMC6309_MODE_NORMAL     0x01
#define QMC6309_MODE_SINGLE     0x02
#define QMC6309_MODE_CONTINUOUS 0x03

#define QMC6309_OSR1_8          0x00
#define QMC6309_OSR1_4          0x01
#define QMC6309_OSR1_2          0x02
#define QMC6309_OSR1_1          0x03

#define QMC6309_OSR2_1          0x00
#define QMC6309_OSR2_2          0x01
#define QMC6309_OSR2_4          0x02
#define QMC6309_OSR2_8          0x03
#define QMC6309_OSR2_16         0x04

#define QMC6309_SOFT_RST        0x80

#define QMC6309_ODR_10HZ        0x01
#define QMC6309_ODR_50HZ        0x02
#define QMC6309_ODR_100HZ       0x03
#define QMC6309_ODR_200HZ       0x04

#define QMC6309_RNG_32G         0x00
#define QMC6309_RNG_16G         0x01
#define QMC6309_RNG_8G          0x02

#define QMC6309_SET_RESET_ON    0x00
#define QMC6309_SET_ONLY        0x01
#define QMC6309_SET_RESET_OFF   0x02

#ifdef CONFIG_COMPASS_USE_BMM150
#define BMM150_REG_CHIP_ID      0x40
#define BMM150_REG_DATA_START   0x42
#define BMM150_REG_POWER        0x4B
#define BMM150_REG_OPMODE       0x4C
#define BMM150_REG_REPXY        0x51
#define BMM150_REG_REPZ         0x52
#define BMM150_CHIP_ID_VAL      0x32
#endif

// Calibration data
static int x_offset = 0;
static int y_offset = 0;
static float x_scale = 1.0f;
static float y_scale = 1.0f;
static int x_min = 32767, x_max = -32768;
static int y_min = 32767, y_max = -32768;
static int z_min = 32767, z_max = -32768;
static bool cal_valid = false;

// Smoothing data
static float filtered_x = 0;
static float filtered_y = 0;
static float filtered_z = 0;
static float last_heading = -1;
static int16_t last_raw_x = 0, last_raw_y = 0;
static bool first_sample = true;
static int sample_count = 0;

#define MIN_CAL_SAMPLES 50
#define ALPHA_RAW 0.25f
#define ALPHA_UI  0.15f

static bool compass_initialized = false;

static void reset_calibration(void) {
    x_min = 32767; x_max = -32768;
    y_min = 32767; y_max = -32768;
    z_min = 32767; z_max = -32768;
    cal_valid = false;
    filtered_x = 0;
    filtered_y = 0;
    filtered_z = 0;
    x_offset = 0;
    y_offset = 0;
    x_scale = 1.0f;
    y_scale = 1.0f;
    last_heading = -1;
    first_sample = true;
    sample_count = 0;
}

static void compass_init_hw(void) {
    if (compass_initialized) return;

#ifdef CONFIG_COMPASS_USE_BMM150
    if (bmi270_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BMI270/BMM150 sensor hub");
        return;
    }

    compass_initialized = true;
    ESP_LOGI(TAG, "BMI270/BMM150 sensor hub initialized");
    return;
#else
    
    uint8_t chip_id = 0;
    if (qmc6309_read_bytes(QMC6309_REG_CHIP_ID, &chip_id, 1) != ESP_OK || chip_id != QMC6309_CHIP_ID_VAL) {
        ESP_LOGE(TAG, "Failed to find QMC6309 (Chip ID: 0x%02X)", chip_id);
        return;
    }
    ESP_LOGI(TAG, "Found QMC6309 ID: 0x%02X", chip_id);

    if (qmc6309_write_reg(QMC6309_REG_CTRL2, QMC6309_SOFT_RST) != ESP_OK) ESP_LOGE(TAG, "Reset failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    if (qmc6309_write_reg(QMC6309_REG_CTRL2, 0x00) != ESP_OK) ESP_LOGE(TAG, "Reset clear failed");
    vTaskDelay(pdMS_TO_TICKS(20));
    
    uint8_t ctrl1_val = (QMC6309_OSR2_16 << 5) | (QMC6309_OSR1_8 << 3) | QMC6309_MODE_CONTINUOUS;
    uint8_t ctrl2_val = (QMC6309_ODR_200HZ << 4) | (QMC6309_RNG_8G << 2) | QMC6309_SET_RESET_ON;

    ESP_LOGI(TAG, "Configuring Compass: CTRL1=0x%02X CTRL2=0x%02X", ctrl1_val, ctrl2_val);

    qmc6309_write_reg(QMC6309_REG_CTRL1, ctrl1_val);
    qmc6309_write_reg(QMC6309_REG_CTRL2, ctrl2_val);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    compass_initialized = true;
    ESP_LOGI(TAG, "Compass HW initialized");
#endif
}

static void compass_timer_cb(lv_timer_t *timer) {
    if (!compass_initialized) return;
    
#ifdef CONFIG_COMPASS_USE_BMM150
    int16_t raw_x, raw_y, raw_z;
    if (bmi270_read_mag(&raw_x, &raw_y, &raw_z) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read BMM150 through BMI270");
        return;
    }

    /* Feed the common calibration/filter/rendering path below. */
    static uint32_t last_log_time = 0;
    uint32_t now = esp_timer_get_time() / 1000;
    if (first_sample) {
        x_min = x_max = raw_x;
        y_min = y_max = raw_y;
        z_min = z_max = raw_z;
        last_raw_x = raw_x;
        last_raw_y = raw_y;
        first_sample = false;
    }
    int movement = abs(raw_x - last_raw_x) + abs(raw_y - last_raw_y);
    last_raw_x = raw_x;
    last_raw_y = raw_y;
    if (!cal_valid) {
        if (raw_x < x_min) x_min = raw_x;
        if (raw_x > x_max) x_max = raw_x;
        if (raw_y < y_min) y_min = raw_y;
        if (raw_y > y_max) y_max = raw_y;
        sample_count++;
        int x_range = x_max - x_min;
        int y_range = y_max - y_min;
        float ratio = (y_range > 10) ? ((float)x_range / y_range) : 0.0f;
        if (x_range > 100 && y_range > 100 && ratio > 0.6f && ratio < 1.7f &&
            sample_count > MIN_CAL_SAMPLES) {
            x_offset = (x_max + x_min) / 2;
            y_offset = (y_max + y_min) / 2;
            float avg_range = (x_range + y_range) / 2.0f;
            x_scale = x_range > 0 ? avg_range / x_range : 1.0f;
            y_scale = y_range > 0 ? avg_range / y_range : 1.0f;
            cal_valid = true;
        }
    } else if (movement > 10) {
        bool updated = false;
        if (raw_x < x_min - 5) { x_min = raw_x; updated = true; }
        if (raw_x > x_max + 5) { x_max = raw_x; updated = true; }
        if (raw_y < y_min - 5) { y_min = raw_y; updated = true; }
        if (raw_y > y_max + 5) { y_max = raw_y; updated = true; }
        if (updated) {
            x_offset = (x_max + x_min) / 2;
            y_offset = (y_max + y_min) / 2;
        }
    }
    if (!cal_valid) {
        if (heading_label) lv_label_set_text(heading_label, "Rotate Device...");
        return;
    }
    float cur_x_cal = (float)(raw_x - x_offset) * x_scale;
    float cur_y_cal = (float)(raw_y - y_offset) * y_scale;
    filtered_x = filtered_x * (1.0f - ALPHA_RAW) + cur_x_cal * ALPHA_RAW;
    filtered_y = filtered_y * (1.0f - ALPHA_RAW) + cur_y_cal * ALPHA_RAW;
    float heading = atan2f(filtered_x, filtered_y) * 180.0f / PI;
    if (heading < 0) heading += 360.0f;
    if (heading >= 360.0f) heading -= 360.0f;
    if (last_heading < 0) last_heading = heading;
    else {
        float diff = heading - last_heading;
        if (diff > 180.0f) diff -= 360.0f;
        else if (diff < -180.0f) diff += 360.0f;
        last_heading += diff * ALPHA_UI;
        if (last_heading < 0) last_heading += 360.0f;
        if (last_heading >= 360.0f) last_heading -= 360.0f;
    }
    if (heading_label) {
        static int last_displayed_heading = -1;
        int current_heading = (int)last_heading;
        if (current_heading != last_displayed_heading) {
            lv_label_set_text_fmt(heading_label, "%d°", current_heading);
            last_displayed_heading = current_heading;
        }
    }
    static float last_render_heading = -999;
    if (fabsf(last_heading - last_render_heading) >= 0.2f) {
        update_cardinal_positions(last_heading);
        last_render_heading = last_heading;
    }
#ifdef DEBUG_COMPASS
    if (now - last_log_time > 2000) {
        ESP_LOGI(TAG, "BMM150 heading: %.1f", last_heading);
        last_log_time = now;
    }
#endif
    return;
#else
    static int drdy_fail_count = 0;
    uint8_t status = 0;
    esp_err_t ret = qmc6309_read_bytes(QMC6309_REG_STATUS, &status, 1);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read status register: %s", esp_err_to_name(ret));
        return;
    }

    if (!(status & QMC6309_STATUS_DRDY)) {
        drdy_fail_count++;
        if (drdy_fail_count % 50 == 0) {
            uint8_t c1=0, c2=0;
            qmc6309_read_bytes(QMC6309_REG_CTRL1, &c1, 1);
            qmc6309_read_bytes(QMC6309_REG_CTRL2, &c2, 1);
            ESP_LOGW(TAG, "Waiting for DRDY (St:0x%02X C1:0x%02X C2:0x%02X)", status, c1, c2);
        }
        return;
    }
    drdy_fail_count = 0;

    static uint32_t last_log_time = 0;
    uint8_t data[6];
    if (qmc6309_read_bytes(QMC6309_REG_DATA_START, data, 6) == ESP_OK) {
        int16_t raw_x = (int16_t)((data[1] << 8) | data[0]);
        int16_t raw_y = (int16_t)((data[3] << 8) | data[2]);
        int16_t raw_z = (int16_t)((data[5] << 8) | data[4]);
        
        if (first_sample) {
            x_min = x_max = raw_x;
            y_min = y_max = raw_y;
            z_min = z_max = raw_z;
            last_raw_x = raw_x;
            last_raw_y = raw_y;
            first_sample = false;
        }

        int movement = abs(raw_x - last_raw_x) + abs(raw_y - last_raw_y);
        last_raw_x = raw_x;
        last_raw_y = raw_y;
        
        uint32_t now = esp_timer_get_time() / 1000;
        
        if (!cal_valid) {
            if (raw_x < x_min) x_min = raw_x;
            if (raw_x > x_max) x_max = raw_x;
            if (raw_y < y_min) y_min = raw_y;
            if (raw_y > y_max) y_max = raw_y;
            sample_count++;
            
            int x_range = x_max - x_min;
            int y_range = y_max - y_min;
            float ratio = (y_range > 10) ? ((float)x_range / y_range) : 0.0f;
            
            static uint32_t last_cal_log = 0;
            if (now - last_cal_log > 1000) {
                ESP_LOGI(TAG, "Cal: X[%d,%d]=%d Y[%d,%d]=%d ratio=%.2f samples=%d", 
                         x_min, x_max, x_range, y_min, y_max, y_range, ratio, sample_count);
                last_cal_log = now;
            }
            
            if (x_range > 800 && y_range > 800 && 
                ratio > 0.6f && ratio < 1.7f && 
                sample_count > MIN_CAL_SAMPLES) {
                
                x_offset = (x_max + x_min) / 2;
                y_offset = (y_max + y_min) / 2;
                
                float avg_range = (x_range + y_range) / 2.0f;
                x_scale = (x_range > 0) ? (avg_range / x_range) : 1.0f;
                y_scale = (y_range > 0) ? (avg_range / y_range) : 1.0f;
                
                cal_valid = true;
                
                filtered_x = (float)(raw_x - x_offset) * x_scale;
                filtered_y = (float)(raw_y - y_offset) * y_scale;
                
                float init_heading = atan2f(filtered_x, filtered_y) * 180.0f / PI;
                if (init_heading < 0) init_heading += 360.0f;
                last_heading = init_heading;
                
                ESP_LOGI(TAG, "Calibration locked! Off:%d,%d Scale:%.2f,%.2f", 
                         x_offset, y_offset, x_scale, y_scale);
            }
        } else {
            if (movement > 40) {
                bool updated = false;
                if (raw_x < x_min - 20) { x_min = raw_x; updated = true; }
                if (raw_x > x_max + 20) { x_max = raw_x; updated = true; }
                if (raw_y < y_min - 20) { y_min = raw_y; updated = true; }
                if (raw_y > y_max + 20) { y_max = raw_y; updated = true; }
                
                if (updated) {
                    x_offset = (x_max + x_min) / 2;
                    y_offset = (y_max + y_min) / 2;
                    int x_range = x_max - x_min;
                    int y_range = y_max - y_min;
                    float avg_range = (x_range + y_range) / 2.0f;
                    
                    if (x_range > 0) x_scale = avg_range / x_range;
                    if (y_range > 0) y_scale = avg_range / y_range;
                    
                    ESP_LOGI(TAG, "Cal refined: Off:%d,%d Range:%d,%d", x_offset, y_offset, x_range, y_range);
                }
            }
        }

        if (!cal_valid) {
            if (heading_label) lv_label_set_text(heading_label, "Rotate Device...");
            return;
        }

        float cur_x_cal = (float)(raw_x - x_offset) * x_scale;
        float cur_y_cal = (float)(raw_y - y_offset) * y_scale;
        filtered_x = (filtered_x * (1.0f - ALPHA_RAW)) + (cur_x_cal * ALPHA_RAW);
        filtered_y = (filtered_y * (1.0f - ALPHA_RAW)) + (cur_y_cal * ALPHA_RAW);
        
        float heading = atan2f(filtered_x, filtered_y) * 180.0f / PI;
        if (heading < 0) heading += 360.0f;
        if (heading >= 360.0f) heading -= 360.0f;
        
        if (last_heading < 0) {
            last_heading = heading;
        } else {
            float diff = heading - last_heading;
            if (diff > 180.0f) diff -= 360.0f;
            else if (diff < -180.0f) diff += 360.0f;
            last_heading += diff * ALPHA_UI;
            if (last_heading < 0) last_heading += 360.0f;
            if (last_heading >= 360.0f) last_heading -= 360.0f;
        }

        if (heading_label) {
            static int last_displayed_heading = -1;
            int current_heading = (int)last_heading;
            if (current_heading != last_displayed_heading) {
                lv_label_set_text_fmt(heading_label, "%d°", current_heading);
                last_displayed_heading = current_heading;
            }
        }
        
#ifdef DEBUG_COMPASS
        if (now - last_log_time > 2000) {  // Every 2 seconds instead of 1
            ESP_LOGI(TAG, "Heading: %.1f", last_heading);
            last_log_time = now;
        }
#endif
        
        // Update the rotating cardinal labels instead of the needle
        static float last_render_heading = -999;
        if (fabsf(last_heading - last_render_heading) >= 0.2f) {
            update_cardinal_positions(last_heading);
            last_render_heading = last_heading;
        }
    } else {
        ESP_LOGE(TAG, "Failed to read data registers");
    }
#endif
}

static void compass_event_handler(InputEvent *event) {
    if (view_input_wants_back(event)) {
        display_manager_go_back();
        return;
    }
#if defined(CONFIG_USE_ENCODER)
    if (event->type == INPUT_TYPE_ENCODER && event->data.encoder.button) {
        display_manager_go_back();
        return;
    }
#endif
    if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        lv_indev_data_t *data = &event->data.touch_data;
        if (data->point.x <= 56 && data->point.y >= GUI_STATUS_BAR_HEIGHT &&
            data->point.y <= GUI_STATUS_BAR_HEIGHT + 56) {
            display_manager_go_back();
            return;
        }
        reset_calibration();
        if (heading_label) lv_label_set_text(heading_label, "Resetting...");
    } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
    }
}

void compass_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t background = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t muted = lv_color_hex(theme_palette_get_text_muted(theme));
    lv_color_t danger = lv_color_hex(theme_palette_get_danger(theme));
    display_manager_fill_screen(background);
    compass_container = gui_screen_create_root(NULL, "Compass", background, LV_OPA_COVER);
    compass_view.root = compass_container;
    
    lv_obj_t *content = gui_screen_create_content(compass_container, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_color(content, text, 0);

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    int available = LV_MIN(LV_HOR_RES, LV_VER_RES - GUI_STATUS_BAR_H);
    compass_ring_size = (available * 55) / 100;
    if (compass_ring_size < 120) compass_ring_size = 120;
    if (compass_ring_size > 400) compass_ring_size = 400;
    compass_cardinal_radius = (compass_ring_size * 42) / 100;
    compass_needle_len = (compass_ring_size * 35) / 100;
    compass_ring_center = compass_ring_size / 2;
#else
    compass_ring_size = RING_SIZE;
    compass_cardinal_radius = CARDINAL_RADIUS;
    compass_needle_len = NEEDLE_LEN;
    compass_ring_center = RING_SIZE / 2;
#endif
    
    // Ring container (for the rotating cardinal labels)
    ring = lv_obj_create(content);
    lv_obj_set_size(ring, compass_ring_size, compass_ring_size);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(theme_palette_get_border(theme)), 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ring, 0, 0);
    
    // Cardinal direction labels (will be repositioned by update_cardinal_positions)
    lbl_n = lv_label_create(ring);
    lv_label_set_text(lbl_n, "N");
    lv_obj_set_style_text_color(lbl_n, danger, 0);
    lv_obj_set_style_text_font(lbl_n, accessibility_get_font_title(), 0);
    
    lbl_s = lv_label_create(ring);
    lv_label_set_text(lbl_s, "S");
    lv_obj_set_style_text_color(lbl_s, text, 0);
    lv_obj_set_style_text_font(lbl_s, accessibility_get_font_title(), 0);
    
    lbl_e = lv_label_create(ring);
    lv_label_set_text(lbl_e, "E");
    lv_obj_set_style_text_color(lbl_e, text, 0);
    lv_obj_set_style_text_font(lbl_e, accessibility_get_font_title(), 0);
    
    lbl_w = lv_label_create(ring);
    lv_label_set_text(lbl_w, "W");
    lv_obj_set_style_text_color(lbl_w, text, 0);
    lv_obj_set_style_text_font(lbl_w, accessibility_get_font_title(), 0);

    // Apply animation times for smoother interpolation
    lv_obj_set_style_anim_time(lbl_n, 50, 0);
    lv_obj_set_style_anim_time(lbl_s, 50, 0);
    lv_obj_set_style_anim_time(lbl_e, 50, 0);
    lv_obj_set_style_anim_time(lbl_w, 50, 0);
    
    // South needle (white) - FIXED pointing down
    needle_line_s = lv_line_create(ring);
    lv_obj_set_style_line_width(needle_line_s, 6, 0);
    lv_obj_set_style_line_color(needle_line_s, muted, 0);
    lv_obj_set_style_line_rounded(needle_line_s, true, 0);
    
    // North needle (red) - FIXED pointing up
    needle_line_n = lv_line_create(ring);
    lv_obj_set_style_line_width(needle_line_n, 6, 0);
    lv_obj_set_style_line_color(needle_line_n, danger, 0);
    lv_obj_set_style_line_rounded(needle_line_n, true, 0);
    
    // Center pivot
    lv_obj_t *pivot = lv_obj_create(ring);
    lv_obj_set_size(pivot, 12, 12);
    lv_obj_set_style_bg_color(pivot, muted, 0);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pivot, 2, 0);
    lv_obj_set_style_border_color(pivot, text, 0);
    lv_obj_align(pivot, LV_ALIGN_CENTER, 0, 0);
    
    // Calculate label dimensions for centering
    if (lbl_n) {
        lv_obj_update_layout(lbl_n);
        cardinal_half_w = lv_obj_get_width(lbl_n) / 2;
        cardinal_half_h = lv_obj_get_height(lbl_n) / 2;
    }

    // Initialize fixed needle and cardinal positions
    init_fixed_needle();
    update_cardinal_positions(0);  // Start with N at top
    
    heading_label = lv_label_create(content);
    lv_label_set_text(heading_label, "Initialize...");
    lv_obj_set_style_text_color(heading_label, text, 0);
    lv_obj_set_style_text_font(heading_label,
#ifdef CONFIG_IS_ATOMS3R
                               accessibility_get_font_small(),
#else
                               accessibility_get_font_title(),
#endif
                               0);
    lv_obj_align(heading_label, LV_ALIGN_BOTTOM_MID, 0,
#ifdef CONFIG_IS_ATOMS3R
                 -1
#else
                 -(GUI_HOME_SAFE_H + 10)
#endif
    );
    
    reset_calibration();
    compass_timer = lv_timer_create(compass_timer_cb, 33, NULL);
    compass_init_hw();
}

void compass_destroy(void) {
    lvgl_timer_del_safe(&compass_timer);
    if (s_compass_dev) {
        i2c_master_bus_rm_device(s_compass_dev);
        s_compass_dev = NULL;
    }
    if (compass_container) { lv_obj_del(compass_container); compass_container = NULL; compass_view.root = NULL; }
    ring = NULL;
    needle_line_n = NULL;
    needle_line_s = NULL;
    lbl_n = lbl_s = lbl_e = lbl_w = NULL;
}

void get_compass_callback(void **callback) { if (callback) *callback = (void *)compass_event_handler; }

View compass_view = {
    .root = NULL,
    .create = compass_create,
    .destroy = compass_destroy,
    .input_callback = compass_event_handler,
    .name = "Compass",
    .get_hardwareinput_callback = get_compass_callback
};

#endif
