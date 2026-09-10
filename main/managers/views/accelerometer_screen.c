#include "managers/views/accelerometer_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/display_manager.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "gui/view_input.h"
#include "gui/theme_palette_api.h"
#include "gui/design_tokens.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "lvgl.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_bus_lock.h"
#include <math.h>
#include "managers/gps_manager.h"
#include "managers/bmi270_driver.h"

#ifdef CONFIG_HAS_ACCELEROMETER

static const char *TAG = "AccelScreen";

#ifndef CONFIG_ACCEL_I2C_PORT
#define CONFIG_ACCEL_I2C_PORT 0
#endif
#define ACCEL_I2C_PORT CONFIG_ACCEL_I2C_PORT

#ifndef CONFIG_ACCEL_I2C_ADDR
#define CONFIG_ACCEL_I2C_ADDR 0x53
#endif
#define ACCEL_I2C_ADDR CONFIG_ACCEL_I2C_ADDR

#define ADXL345_REG_DEVID        0x00
#define ADXL345_REG_BW_RATE      0x2C
#define ADXL345_REG_POWER_CTL    0x2D
#define ADXL345_REG_DATA_FORMAT  0x31
#define ADXL345_REG_OFSX         0x1E
#define ADXL345_REG_OFSY         0x1F
#define ADXL345_REG_OFSZ         0x20
#define ADXL345_REG_DATAX0       0x32

#define ADXL345_DEVID            0xE5
#define ADXL345_MEASURE_MODE     0x08
#define ADXL345_FULL_RES_16G     0x0B
#define ADXL345_BW_100HZ         0x0A

#define MG_PER_LSB 0.0039f
#ifdef CONFIG_ACCEL_USE_BMI270
/* BMI270 is configured for its default +/-2 G range: 16384 LSB per G. */
#define BMI270_G_PER_LSB (1.0f / 16384.0f)
#endif
#define GAUGE_MAX_G 4.0f
#define ALPHA 0.3f
#define DT_SEC 0.033f
#define GRAV_LP 0.15f
#define LIN_DEAD_ZONE 0.20f
#define ZUPT_JERK_THRESH 0.05f
#define ZUPT_WINDOW 4
#define VEL_FRICTION 0.93f
#define MAX_VELOCITY_MS 2.0f
#define MS_TO_KMH 3.6f
#define MS_TO_MPH 2.23694f
#define G_TO_MS2 9.80665f
#define FREEFALL_THRESH 0.3f
#define GPS_SPEED_MIN 0.5f
#define CAL_SAMPLES 16
#define OFS_SCALE_LSB_PER_G 256.0f
#define OFS_REG_SCALE 4.0f
#define SHAKE_ALPHA 0.2f
#define PI_F 3.14159265f
#define RAD_TO_DEG (180.0f / PI_F)

static lv_obj_t *accel_container = NULL;
static lv_obj_t *gauge_arc_bg = NULL;
static lv_obj_t *gauge_arc = NULL;
static lv_obj_t *g_label = NULL;
static lv_obj_t *peak_label = NULL;
static lv_obj_t *xyz_label = NULL;
static lv_obj_t *speed_label = NULL;
static lv_obj_t *tilt_label = NULL;
static lv_obj_t *orient_label = NULL;
static lv_obj_t *needle_line = NULL;
static lv_point_t needle_pts[2];
static lv_timer_t *accel_timer = NULL;
static bool accel_initialized = false;

static float filtered_x = 0, filtered_y = 0, filtered_z = 0;
static float grav_x = 0, grav_y = 0, grav_z = 1.0f;
static float peak_g = 0;
static float velocity_ms = 0;
static float shake_intensity = 0;
static float prev_total_g = 1.0f;
static int zupt_still_count = 0;
static int i2c_error_count = 0;
static bool first_sample = true;
static bool freefall_active = false;
static bool gps_started_by_accel = false;
static i2c_master_dev_handle_t s_accel_dev = NULL;

static int gauge_cx, gauge_cy, gauge_r;

static esp_err_t accel_get_device(void) {
    if (s_accel_dev) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t ret = i2c_master_get_bus_handle(ACCEL_I2C_PORT, &bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus %d unavailable: %s", ACCEL_I2C_PORT, esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ACCEL_I2C_ADDR,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
    };

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_accel_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add accelerometer device: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t adxl345_write_reg(uint8_t reg, uint8_t val) {
    esp_err_t ret = accel_get_device();
    if (ret != ESP_OK) {
        return ret;
    }

    bool locked = i2c_bus_lock(ACCEL_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    uint8_t payload[2] = {reg, val};
    ret = i2c_master_transmit(s_accel_dev, payload, sizeof(payload), 50);
    i2c_bus_unlock(ACCEL_I2C_PORT);
    return ret;
}

static esp_err_t adxl345_read_bytes(uint8_t reg, uint8_t *data, size_t len) {
    esp_err_t ret = accel_get_device();
    if (ret != ESP_OK) {
        return ret;
    }

    bool locked = i2c_bus_lock(ACCEL_I2C_PORT, 100);
    if (!locked) return ESP_ERR_TIMEOUT;

    ret = i2c_master_transmit_receive(s_accel_dev, &reg, sizeof(reg), data, len, 50);
    i2c_bus_unlock(ACCEL_I2C_PORT);
    return ret;
}

static void accel_init_hw(void) {
    if (accel_initialized) return;

#ifdef CONFIG_ACCEL_USE_BMI270
    if (bmi270_init() == ESP_OK) {
        accel_initialized = true;
        // BMI270 self-calibrates internally; there is no host-side calibration
        // pass here (unlike the ADXL345 path below), so don't imply one.
        ESP_LOGI(TAG, "BMI270 initialized");
    }
    return;
#else

    uint8_t devid = 0;
    if (adxl345_read_bytes(ADXL345_REG_DEVID, &devid, 1) != ESP_OK || devid != ADXL345_DEVID) {
        ESP_LOGE(TAG, "ADXL345 not found (ID: 0x%02X)", devid);
        return;
    }
    ESP_LOGI(TAG, "Found ADXL345 ID: 0x%02X", devid);

    adxl345_write_reg(ADXL345_REG_POWER_CTL, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    adxl345_write_reg(ADXL345_REG_DATA_FORMAT, ADXL345_FULL_RES_16G);
    adxl345_write_reg(ADXL345_REG_BW_RATE, ADXL345_BW_100HZ);
    adxl345_write_reg(ADXL345_REG_POWER_CTL, ADXL345_MEASURE_MODE);
    vTaskDelay(pdMS_TO_TICKS(20));

    accel_initialized = true;
    ESP_LOGI(TAG, "ADXL345 initialized, calibrating...");

    adxl345_write_reg(ADXL345_REG_OFSX, 0);
    adxl345_write_reg(ADXL345_REG_OFSY, 0);
    adxl345_write_reg(ADXL345_REG_OFSZ, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    int32_t sum_x = 0, sum_y = 0, sum_z = 0;
    int valid = 0;
    for (int i = 0; i < CAL_SAMPLES; i++) {
        vTaskDelay(pdMS_TO_TICKS(12));
        uint8_t d[6];
        if (adxl345_read_bytes(ADXL345_REG_DATAX0, d, 6) == ESP_OK) {
            sum_x += (int16_t)((d[1] << 8) | d[0]);
            sum_y += (int16_t)((d[3] << 8) | d[2]);
            sum_z += (int16_t)((d[5] << 8) | d[4]);
            valid++;
        }
    }
    if (valid > 0) {
        int16_t avg_x = sum_x / valid;
        int16_t avg_y = sum_y / valid;
        int16_t avg_z = sum_z / valid - (int16_t)OFS_SCALE_LSB_PER_G;
        int8_t ofs_x = (int8_t)(-avg_x / (int16_t)OFS_REG_SCALE);
        int8_t ofs_y = (int8_t)(-avg_y / (int16_t)OFS_REG_SCALE);
        int8_t ofs_z = (int8_t)(-avg_z / (int16_t)OFS_REG_SCALE);
        adxl345_write_reg(ADXL345_REG_OFSX, (uint8_t)ofs_x);
        adxl345_write_reg(ADXL345_REG_OFSY, (uint8_t)ofs_y);
        adxl345_write_reg(ADXL345_REG_OFSZ, (uint8_t)ofs_z);
        ESP_LOGI(TAG, "Calibration done: ofs X=%d Y=%d Z=%d", ofs_x, ofs_y, ofs_z);
    }
#endif
}

static lv_color_t g_to_color(float g) {
    float ratio = g / GAUGE_MAX_G;
    if (ratio > 1.0f) ratio = 1.0f;
    if (ratio < 0.5f) {
        uint8_t r = (uint8_t)(ratio * 2.0f * 255);
        return lv_color_make(r, 255, 0);
    }
    uint8_t gr = (uint8_t)((1.0f - (ratio - 0.5f) * 2.0f) * 255);
    return lv_color_make(255, gr, 0);
}

static void update_needle(float g_val) {
    float ratio = g_val / GAUGE_MAX_G;
    if (ratio > 1.0f) ratio = 1.0f;
    float angle_deg = 225.0f - ratio * 270.0f;
    float angle_rad = angle_deg * 3.14159265f / 180.0f;

    int needle_len = gauge_r - 10;
    int cy_offset = gauge_cy;
    needle_pts[0].x = gauge_cx;
    needle_pts[0].y = cy_offset;
    needle_pts[1].x = gauge_cx + (int)(cosf(angle_rad) * needle_len);
    needle_pts[1].y = cy_offset - (int)(sinf(angle_rad) * needle_len);

    if (needle_line) lv_line_set_points(needle_line, needle_pts, 2);
}

static void accel_timer_cb(lv_timer_t *timer) {
    if (!accel_initialized) return;

#ifdef CONFIG_ACCEL_USE_BMI270
    int16_t raw_x, raw_y, raw_z;
    if (bmi270_read_accel(&raw_x, &raw_y, &raw_z) != ESP_OK) return;
#else
    uint8_t data[6];
    if (adxl345_read_bytes(ADXL345_REG_DATAX0, data, 6) != ESP_OK) {
        if (++i2c_error_count >= 10) {
            ESP_LOGW(TAG, "Too many I2C errors, reinitializing");
            accel_initialized = false;
            i2c_error_count = 0;
            accel_init_hw();
        }
        return;
    }
    i2c_error_count = 0;

    int16_t raw_x = (int16_t)((data[1] << 8) | data[0]);
    int16_t raw_y = (int16_t)((data[3] << 8) | data[2]);
    int16_t raw_z = (int16_t)((data[5] << 8) | data[4]);
#endif

#ifdef CONFIG_ACCEL_USE_BMI270
    float ax = raw_x * BMI270_G_PER_LSB;
    float ay = raw_y * BMI270_G_PER_LSB;
    float az = raw_z * BMI270_G_PER_LSB;
#else
    float ax = raw_x * MG_PER_LSB;
    float ay = raw_y * MG_PER_LSB;
    float az = raw_z * MG_PER_LSB;
#endif

    if (first_sample) {
        filtered_x = ax;
        filtered_y = ay;
        filtered_z = az;
        first_sample = false;
    } else {
        filtered_x = filtered_x * (1.0f - ALPHA) + ax * ALPHA;
        filtered_y = filtered_y * (1.0f - ALPHA) + ay * ALPHA;
        filtered_z = filtered_z * (1.0f - ALPHA) + az * ALPHA;
    }

    float total_g = sqrtf(filtered_x * filtered_x + filtered_y * filtered_y + filtered_z * filtered_z);
    if (total_g > peak_g) peak_g = total_g;

    static int last_arc_val = -1;
    int arc_val = (int)((total_g / GAUGE_MAX_G) * 100);
    if (arc_val > 100) arc_val = 100;

    if (arc_val != last_arc_val) {
        if (gauge_arc) {
            lv_arc_set_value(gauge_arc, arc_val);
            lv_obj_set_style_arc_color(gauge_arc, g_to_color(total_g), LV_PART_INDICATOR);
        }
        update_needle(total_g);
        last_arc_val = arc_val;
    }

    if (g_label) {
        static int last_g_100 = -1;
        int g_100 = (int)(total_g * 100);
        if (g_100 != last_g_100) {
            lv_label_set_text_fmt(g_label, "%d.%02d", g_100 / 100, g_100 % 100);
            last_g_100 = g_100;
        }
    }

    if (peak_label) {
        static int last_peak_100 = -1;
        int p100 = (int)(peak_g * 100);
        if (p100 != last_peak_100) {
            lv_label_set_text_fmt(peak_label, "Peak: %d.%02d G", p100 / 100, p100 % 100);
            last_peak_100 = p100;
        }
    }

    float pitch = atan2f(filtered_x, sqrtf(filtered_y * filtered_y + filtered_z * filtered_z)) * RAD_TO_DEG;
    float roll  = atan2f(filtered_y, sqrtf(filtered_x * filtered_x + filtered_z * filtered_z)) * RAD_TO_DEG;

    if (tilt_label) {
        static int lp = -9999, lr = -9999;
        int ip = (int)(pitch * 10), ir = (int)(roll * 10);
        if (ip != lp || ir != lr) {
            const char *ps = (ip < 0) ? "-" : "";
            const char *rs = (ir < 0) ? "-" : "";
            int ap = abs(ip), ar = abs(ir);
            lv_label_set_text_fmt(tilt_label, "P:%s%d.%ddeg  R:%s%d.%ddeg",
                ps, ap / 10, ap % 10, rs, ar / 10, ar % 10);
            lp = ip; lr = ir;
        }
    }

    freefall_active = (total_g < FREEFALL_THRESH);

    float jerk = fabsf(total_g - prev_total_g);
    shake_intensity = shake_intensity * (1.0f - SHAKE_ALPHA) + jerk * SHAKE_ALPHA;
    prev_total_g = total_g;

    const char *orientation;
    if (freefall_active) {
        orientation = "!! FREE FALL !!";
    } else if (fabsf(filtered_z) > 0.8f && fabsf(filtered_x) < 0.4f && fabsf(filtered_y) < 0.4f) {
        orientation = (filtered_z > 0) ? "Flat (Face Up)" : "Flat (Face Down)";
    } else if (fabsf(filtered_x) > fabsf(filtered_y) && fabsf(filtered_x) > fabsf(filtered_z)) {
        orientation = (filtered_x > 0) ? "Tilted Right" : "Tilted Left";
    } else if (fabsf(filtered_y) > fabsf(filtered_x) && fabsf(filtered_y) > fabsf(filtered_z)) {
        orientation = (filtered_y > 0) ? "Portrait Up" : "Portrait Down";
    } else {
        orientation = "Tilted";
    }

    if (orient_label) {
        static const char *last_orient = NULL;
        static int last_shake10 = -1;
        int s10 = (int)(shake_intensity * 100);
        if (last_orient != orientation || s10 != last_shake10) {
            if (freefall_active) {
                lv_obj_set_style_text_color(orient_label, lv_color_hex(0xFF0000), 0);
            } else if (shake_intensity > 0.15f) {
                lv_obj_set_style_text_color(orient_label, lv_color_hex(0xFFAA00), 0);
            } else {
                lv_obj_set_style_text_color(orient_label, lv_color_hex(0x44DD44), 0);
            }
            lv_label_set_text_fmt(orient_label, "%s  Shake:%d.%d", orientation, s10 / 10, s10 % 10);
            last_orient = orientation;
            last_shake10 = s10;
        }
    }

    grav_x += GRAV_LP * (filtered_x - grav_x);
    grav_y += GRAV_LP * (filtered_y - grav_y);
    grav_z += GRAV_LP * (filtered_z - grav_z);

    float lin_x = filtered_x - grav_x;
    float lin_y = filtered_y - grav_y;
    float lin_z = filtered_z - grav_z;
    float lin_mag = sqrtf(lin_x * lin_x + lin_y * lin_y + lin_z * lin_z);

    if (lin_mag < LIN_DEAD_ZONE) lin_mag = 0;

    if (shake_intensity < ZUPT_JERK_THRESH && lin_mag < LIN_DEAD_ZONE) {
        zupt_still_count++;
    } else {
        zupt_still_count = 0;
    }

    velocity_ms *= VEL_FRICTION;
    if (zupt_still_count >= ZUPT_WINDOW) {
        velocity_ms = 0;
    } else if (lin_mag > 0) {
        velocity_ms += lin_mag * G_TO_MS2 * DT_SEC;
    }
    if (velocity_ms > MAX_VELOCITY_MS) velocity_ms = MAX_VELOCITY_MS;
    if (velocity_ms < 0.01f) velocity_ms = 0;

    if (speed_label) {
        bool gps_active = false;
        float display_speed_ms = velocity_ms;
        if (g_gpsManager.isinitilized && nmea_hdl) {
            gps_t *gps = &((esp_gps_t *)nmea_hdl)->parent;
            if (gps && gps->valid && gps->fix >= GPS_FIX_GPS) {
                gps_active = true;
                display_speed_ms = (gps->speed >= GPS_SPEED_MIN) ? gps->speed : 0.0f;
            }
        }
        float kmh = display_speed_ms * MS_TO_KMH;
        float mph = display_speed_ms * MS_TO_MPH;
        static int last_kmh10 = -1, last_mph10 = -1;
        static bool last_gps_state = false;
        int kmh10 = (int)(kmh * 10);
        int mph10 = (int)(mph * 10);
        if (kmh10 != last_kmh10 || mph10 != last_mph10 || gps_active != last_gps_state) {
            const char *prefix = gps_active ? "GPS" : "NO GPS";
            lv_label_set_text_fmt(speed_label, "%s: %d.%d km/h | %d.%d mph",
                prefix, kmh10 / 10, kmh10 % 10, mph10 / 10, mph10 % 10);
            lv_obj_set_style_text_color(speed_label,
                lv_color_hex(gps_active ? 0x00FF88 : 0x00CCFF), 0);
            last_kmh10 = kmh10; last_mph10 = mph10;
            last_gps_state = gps_active;
        }
    }
}

static void accel_event_handler(InputEvent *event) {
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
        peak_g = 0;
        velocity_ms = 0;
        if (peak_label) lv_label_set_text(peak_label, "Peak: 0.00 G");
        if (speed_label) lv_label_set_text(speed_label, "0.0 km/h  |  0.0 mph");
    } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_EXIT_BUTTON) {
        display_manager_go_back();
    }
}

void accelerometer_create(void) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t background = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t surface = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t surface_alt = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t muted = lv_color_hex(theme_palette_get_text_muted(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
    lv_color_t danger = lv_color_hex(theme_palette_get_danger(theme));
    display_manager_fill_screen(background);
    accel_container = gui_screen_create_root(NULL, "Accel", background, LV_OPA_COVER);
    accelerometer_view.root = accel_container;

    lv_obj_t *content = gui_screen_create_content(accel_container, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_text_color(content, text, 0);

    int sw = LV_HOR_RES;
    int sh = LV_VER_RES - GUI_STATUS_BAR_HEIGHT;
    int arc_size = LV_MIN(sw, sh) - 20;
    int gauge_shift = -15;
#ifdef CONFIG_IS_ATOMS3R
    // Keep the compact gauge inside the content area on the 128px display.
    arc_size = 48;
    gauge_shift = -12;
#else
    if (arc_size < 80) arc_size = 80;
#endif
    if (arc_size > 220) arc_size = 220;

    gauge_cx = sw / 2;
    gauge_cy = sh / 2 + gauge_shift;
    gauge_r = arc_size / 2;

    gauge_arc_bg = lv_arc_create(content);
    lv_obj_set_size(gauge_arc_bg, arc_size, arc_size);
    lv_obj_align(gauge_arc_bg, LV_ALIGN_CENTER, 0,
                 gauge_shift
    );
    lv_arc_set_rotation(gauge_arc_bg, 135);
    lv_arc_set_bg_angles(gauge_arc_bg, 0, 270);
    lv_arc_set_range(gauge_arc_bg, 0, 100);
    lv_arc_set_value(gauge_arc_bg, 100);
    lv_obj_set_style_arc_width(gauge_arc_bg,
#ifdef CONFIG_IS_ATOMS3R
                               8,
#else
                               12,
#endif
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gauge_arc_bg, surface_alt, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(gauge_arc_bg,
#ifdef CONFIG_IS_ATOMS3R
                               8,
#else
                               12,
#endif
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(gauge_arc_bg, surface, LV_PART_MAIN);
    lv_obj_remove_style(gauge_arc_bg, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(gauge_arc_bg, LV_OBJ_FLAG_CLICKABLE);

    gauge_arc = lv_arc_create(content);
    lv_obj_set_size(gauge_arc, arc_size, arc_size);
    lv_obj_align(gauge_arc, LV_ALIGN_CENTER, 0,
                 gauge_shift
    );
    lv_arc_set_rotation(gauge_arc, 135);
    lv_arc_set_bg_angles(gauge_arc, 0, 270);
    lv_arc_set_range(gauge_arc, 0, 100);
    lv_arc_set_value(gauge_arc, 0);
    lv_obj_set_style_arc_width(gauge_arc,
#ifdef CONFIG_IS_ATOMS3R
                               8,
#else
                               12,
#endif
                               LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(gauge_arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(gauge_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_style(gauge_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(gauge_arc, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i <= 4; i++) {
        float ratio = (float)i / 4.0f;
        float angle_deg = 225.0f - ratio * 270.0f;
        float angle_rad = angle_deg * 3.14159265f / 180.0f;
        int tick_outer = gauge_r - 2;
        int tick_inner = gauge_r - 18;

        lv_obj_t *tick = lv_line_create(content);
        static lv_point_t tick_pts[5][2];
        tick_pts[i][0].x = gauge_cx + (int)(cosf(angle_rad) * tick_inner);
         tick_pts[i][0].y = gauge_cy - (int)(sinf(angle_rad) * tick_inner);
        tick_pts[i][1].x = gauge_cx + (int)(cosf(angle_rad) * tick_outer);
         tick_pts[i][1].y = gauge_cy - (int)(sinf(angle_rad) * tick_outer);
        lv_line_set_points(tick, tick_pts[i], 2);
        lv_obj_set_style_line_color(tick, muted, 0);
        lv_obj_set_style_line_width(tick, 2, 0);

        lv_obj_t *tick_lbl = lv_label_create(content);
        lv_label_set_text_fmt(tick_lbl, "%d", i);
        lv_obj_set_style_text_color(tick_lbl, muted, 0);
        lv_obj_set_style_text_font(tick_lbl, accessibility_get_font_small(), 0);
        int lbl_r = gauge_r - 24;
        int lbl_x = gauge_cx + (int)(cosf(angle_rad) * lbl_r);
         int lbl_y = gauge_cy - (int)(sinf(angle_rad) * lbl_r);
        lv_obj_set_pos(tick_lbl, lbl_x - 4, lbl_y - 6);
    }

    needle_line = lv_line_create(content);
    lv_obj_set_style_line_width(needle_line, 3, 0);
    lv_obj_set_style_line_color(needle_line, danger, 0);
    lv_obj_set_style_line_rounded(needle_line, true, 0);

    needle_pts[0].x = gauge_cx;
    needle_pts[0].y = gauge_cy;
    needle_pts[1].x = gauge_cx;
    needle_pts[1].y = gauge_cy;
    lv_line_set_points(needle_line, needle_pts, 2);

    lv_obj_t *pivot = lv_obj_create(content);
    lv_obj_set_size(pivot, 10, 10);
    lv_obj_set_style_bg_color(pivot, danger, 0);
    lv_obj_set_style_radius(pivot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pivot, 1, 0);
    lv_obj_set_style_border_color(pivot, text, 0);
    lv_obj_align(pivot, LV_ALIGN_CENTER, 0,
                 gauge_shift
    );

    g_label = lv_label_create(content);
    lv_label_set_text(g_label, "0.00");
    lv_obj_set_style_text_color(g_label, text, 0);
#ifdef CONFIG_IS_ATOMS3R
    // Display font (18-24px) is oversized inside the 48px gauge on 128px.
    lv_obj_set_style_text_font(g_label, &lv_font_montserrat_16, 0);
#else
    lv_obj_set_style_text_font(g_label, accessibility_get_font_display(), 0);
#endif
    lv_obj_align(g_label, LV_ALIGN_CENTER, 0,
#ifdef CONFIG_IS_ATOMS3R
                 0
#else
                 15
#endif
    );

    lv_obj_t *unit_label = lv_label_create(content);
    lv_label_set_text(unit_label, "G");
    lv_obj_set_style_text_color(unit_label, muted, 0);
    lv_obj_set_style_text_font(unit_label, accessibility_get_font_body(), 0);
    lv_obj_align(unit_label, LV_ALIGN_CENTER, 0,
#ifdef CONFIG_IS_ATOMS3R
                 15
#else
                 38
#endif
    );

    orient_label = lv_label_create(content);
    lv_label_set_text(orient_label, "Flat (Face Up)  Shake:0.0");
    lv_obj_set_style_text_color(orient_label, lv_color_hex(theme_palette_get_success(theme)), 0);
    lv_obj_set_style_text_font(orient_label, accessibility_get_font_small(), 0);
    lv_obj_align(orient_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 55));

    peak_label = lv_label_create(content);
    lv_label_set_text(peak_label, "Peak: 0.00 G");
    lv_obj_set_style_text_color(peak_label, lv_color_hex(theme_palette_get_warning(theme)), 0);
    lv_obj_set_style_text_font(peak_label, accessibility_get_font_small(), 0);
    lv_obj_align(peak_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 42));

    speed_label = lv_label_create(content);
    lv_label_set_text(speed_label, "NO GPS: 0.0 km/h | 0.0 mph");
    lv_obj_set_style_text_color(speed_label, accent, 0);
    lv_obj_set_style_text_font(speed_label, accessibility_get_font_small(), 0);
    lv_obj_align(speed_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 29));

    tilt_label = lv_label_create(content);
    lv_label_set_text(tilt_label, "Pitch:0.0deg  Roll:0.0deg");
    lv_obj_set_style_text_color(tilt_label, muted, 0);
    lv_obj_set_style_text_font(tilt_label, accessibility_get_font_small(), 0);
    lv_obj_align(tilt_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 16));

    xyz_label = lv_label_create(content);
    lv_label_set_text(xyz_label, "X:0.00  Y:0.00  Z:0.00");
    lv_obj_set_style_text_color(xyz_label, muted, 0);
    lv_obj_set_style_text_font(xyz_label, accessibility_get_font_small(), 0);
    lv_obj_align(xyz_label, LV_ALIGN_BOTTOM_MID, 0, -(GUI_HOME_SAFE_H + 3));

#ifdef CONFIG_IS_ATOMS3R
    // 128px has no room for the full readout stack. Keep only orientation/shake
    // and peak, pinned to the very bottom so they don't collide with the gauge
    // (the default -55/-42 offsets land on top of it on this screen).
    lv_obj_add_flag(speed_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tilt_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(xyz_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(peak_label, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_align(orient_label, LV_ALIGN_BOTTOM_MID, 0, -14);
#endif

    first_sample = true;
    peak_g = 0;
    velocity_ms = 0;
    shake_intensity = 0;
    prev_total_g = 1.0f;
    zupt_still_count = 0;
    freefall_active = false;
    filtered_x = filtered_y = filtered_z = 0;
    grav_x = 0; grav_y = 0; grav_z = 1.0f;

    gps_started_by_accel = false;
    if (!g_gpsManager.isinitilized) {
        gps_manager_init(&g_gpsManager);
        if (g_gpsManager.isinitilized) {
            gps_started_by_accel = true;
        }
    }

    accel_timer = lv_timer_create(accel_timer_cb, 33, NULL);
    accel_init_hw();
}

void accelerometer_destroy(void) {
    lvgl_timer_del_safe(&accel_timer);
    if (gps_started_by_accel && g_gpsManager.isinitilized) {
        gps_manager_deinit(&g_gpsManager);
        gps_started_by_accel = false;
    }
    if (s_accel_dev) {
        i2c_master_bus_rm_device(s_accel_dev);
        s_accel_dev = NULL;
    }
    if (accel_container) { lv_obj_del(accel_container); accel_container = NULL; accelerometer_view.root = NULL; }
    gauge_arc_bg = NULL;
    gauge_arc = NULL;
    g_label = NULL;
    peak_label = NULL;
    xyz_label = NULL;
    speed_label = NULL;
    tilt_label = NULL;
    orient_label = NULL;
    needle_line = NULL;
}

static void get_accel_callback(void **callback) { if (callback) *callback = (void *)accel_event_handler; }

View accelerometer_view = {
    .root = NULL,
    .create = accelerometer_create,
    .destroy = accelerometer_destroy,
    .input_callback = accel_event_handler,
    .name = "Accel",
    .get_hardwareinput_callback = get_accel_callback
};

#endif
