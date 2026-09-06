#include "managers/display_manager.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
#include "lvgl_tft/banshee_c5_parlio.h"
#endif
#include "managers/sd_card_manager.h"
#include "managers/plugin_api.h"
#include "managers/settings_manager.h"
#include "managers/ghostchi_manager.h"
#include "gui/theme_palette_api.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/gui_anim.h"
#include "gui/toast.h"
#include "managers/views/error_popup.h"
#include "managers/views/main_menu_screen.h"
#include "suppress_noisy_logs.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/clock_screen.h"
#include "managers/views/compass_screen.h"
#include "managers/views/enviii_screen.h"
#include "managers/views/accelerometer_screen.h"
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif
#include "managers/views/nfc_view.h"
#include "managers/views/badusb_view.h"
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#include "managers/views/app_gallery_screen.h"
#ifdef CONFIG_WITH_SCREEN
#include "managers/views/ghostchi_screen.h"
#endif
#include "managers/views/lockscreen.h"
#include "managers/views/splash_screen.h"
#include "managers/encoder_manager.h"
#include "managers/crash_reporter.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_pm.h"
#include "driver/ledc.h"
#include <limits.h> // for UINT32_MAX
#include "managers/ap_manager.h"
#include "managers/wifi_manager.h"
#include "core/serial_manager.h"
#include "managers/wifi_manager.h"
#include "managers/rgb_manager.h"
#include "driver/i2c_master.h"
#include "soc/soc_caps.h"
#include "io_manager/i2c_bus_lock.h"
#ifdef CONFIG_USE_IO_EXPANDER
#include "io_manager/io_manager.h"
#endif
#include "core/screen_mirror.h"
#include "gui/lvgl_safe.h"
#include "gui/gui_router.h"

uint32_t theme_palette_get_surface_alt(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);

#if defined(CONFIG_IDF_TARGET_ESP32C5)
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
#if defined(CONFIG_BUILD_CONFIG_TEMPLATE_SOMETHINGSOMETHING) || defined(CONFIG_BUILD_CONFIG_TEMPLATE_SOMETHINGSOMETHING2)
#define LVGL_TICK_TASK_STACK_SIZE 8192
#else
#define LVGL_TICK_TASK_STACK_SIZE 8192
#endif
#else
#define LVGL_TICK_TASK_STACK_SIZE 8192
#endif
#else
#define LVGL_TICK_TASK_STACK_SIZE 8192
#endif

#ifndef CONFIG_JC3248W535EN_LCD
static TaskHandle_t hardware_input_task_handle = NULL;
static StackType_t *hardware_input_task_stack = NULL;
static StaticTask_t *hardware_input_task_buffer = NULL;
#if defined(CONFIG_USE_TDISPLAY_S3) || defined(CONFIG_Waveshare_LCD)
static i2c_master_bus_handle_t s_touch_i2c_bus = NULL;
#endif
#endif

#ifdef CONFIG_USE_CARDPUTER
#include "vendor/keyboard_handler.h"
#endif
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_IS_ATOMS3R)
#include "vendor/m5/m5gfx_wrapper.h"
#endif

#ifdef CONFIG_USE_TDISPLAY_S3
#include "../vendor/i80_display.h"
#include "lvgl_touch/touch_driver.h"
#endif

#ifdef CONFIG_HAS_BATTERY_ADC
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <soc/adc_channel.h>
#include <soc/soc_caps.h>
#include <math.h>
#endif

#ifdef CONFIG_HAS_BATTERY
#include "vendor/drivers/axp2101.h"
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
#include "managers/fuel_gauge_manager.h"
#endif

QueueHandle_tt input_queue = NULL;
joystick_t joysticks[5];

static volatile bool g_low_i2c_mode = false;
static bool s_deferred_peripherals_initialized = false;
static void display_manager_set_backlight_raw(uint8_t percentage);

#ifdef CONFIG_HAS_FUEL_GAUGE
// Background polling logic moved to get_battery_info (lazy loading)
static volatile uint8_t g_cached_batt_percent = 0;
static volatile bool g_cached_batt_charging = false;
static volatile bool g_cached_batt_valid = false;
#endif

#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif

#ifdef CONFIG_USE_7_INCHER
#include "vendor/drivers/ST7262.h"
#endif

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD) || \
    defined(CONFIG_CROWPANEL_ADVANCE_24_LCD) || \
    defined(CONFIG_CROWPANEL_ADVANCE_28_LCD)
#include "lvgl_i2c/i2c_manager.h"
#endif

#ifdef CONFIG_Waveshare_LCD
#include "vendor/drivers/CH422G.h"
#endif

#ifdef CONFIG_JC3248W535EN_LCD
#include "axs15231b/esp_bsp.h"
#include "axs15231b/lv_port.h"
#include "vendor/drivers/axs15231b.h"
#endif

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
#include "gui/lv_draw_ppa_v8.h"
#include "managers/views/plugin_runner_view.h"
#include "lvgl_i2c/i2c_manager.h"
#include "lvgl_touch/touch_driver.h"
#include "vendor/drivers/crowpanel_p4_display.h"
#endif

#ifdef CONFIG_CROWPANEL_EPAPER_42
#include "vendor/drivers/crowpanel_epaper.h"
#endif

#ifdef CONFIG_USE_TDECK
#include "lvgl_i2c/i2c_manager.h"

#define LILYGO_KB_SLAVE_ADDRESS              0x55
#define LILYGO_KB_BRIGHTNESS_CMD             0x01
#define LILYGO_KB_ALT_B_BRIGHTNESS_CMD       0x02
#define LILYGO_KB_MODE_RAW_CMD              0x03
#define LILYGO_KB_MODE_KEY_CMD              0x04
#define TDECK_KEY_DEBOUNCE_MS                30
#define TDECK_EVENT_RATE_LIMIT_MS            10

// T-Deck keyboard layout from Arduino code
#define TDECK_COLS                           5
#define TDECK_ROWS                           7
// Shift key positions: Left Shift at (1,6), Right Shift at (2,3)
#define TDECK_LEFT_SHIFT_COL                 1
#define TDECK_LEFT_SHIFT_ROW                 6
#define TDECK_RIGHT_SHIFT_COL                2
#define TDECK_RIGHT_SHIFT_ROW                3
// Symbol key position
#define TDECK_SYMBOL_COL                     0
#define TDECK_SYMBOL_ROW                     2

#define TDECK_TRACKBALL_DEBOUNCE_MS          200
#define TDECK_TRACKBALL_POST_DELAY_MS        50

static volatile bool trackball_up_flag = false;
static volatile bool trackball_down_flag = false;
static volatile bool trackball_left_flag = false;
static volatile bool trackball_right_flag = false;
static volatile uint32_t trackball_last_event_ms = 0;

static void IRAM_ATTR trackball_isr_up(void *arg) {
    trackball_up_flag = true;
}
static void IRAM_ATTR trackball_isr_down(void *arg) {
    trackball_down_flag = true;
}
static void IRAM_ATTR trackball_isr_left(void *arg) {
    trackball_left_flag = true;
}
static void IRAM_ATTR trackball_isr_right(void *arg) {
    trackball_right_flag = true;
}

void set_keyboard_brightness(uint8_t brightness);

#endif


#ifndef CONFIG_TFT_WIDTH
#define CONFIG_TFT_WIDTH 240
#endif

#ifndef CONFIG_TFT_HEIGHT
#define CONFIG_TFT_HEIGHT 320
#endif

#define BACKLIGHT_TIMER LEDC_TIMER_0
#define RGB_TIMER       LEDC_TIMER_1

#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
#define LVGL_TASK_PERIOD_MS 5
#else
#define LVGL_TASK_PERIOD_MS 10
#endif
#define INTERMEDIATE_DIM_PERCENT 20
#define INTERMEDIATE_DIM_DURATION_MS 5000
static const char *TAG = "DisplayManager";
DisplayManager dm = {.current_view = NULL, .previous_view = NULL};

lv_obj_t *status_bar;
lv_obj_t *wifi_label = NULL;
lv_obj_t *bt_label = NULL;
lv_obj_t *sd_label = NULL;
lv_obj_t *battery_label = NULL;
lv_obj_t *level_label = NULL;
lv_obj_t *mainlabel = NULL;

static View *s_lockscreen_return_view = NULL;

/* True while the lockscreen is shown as a floating overlay (wake / auto-lock)
 * on top of a still-live view. In this mode dm.current_view keeps pointing at
 * the underlying view so its capture/timers keep running; input is routed to
 * the lockscreen instead, and the auto-lock/wake paths must not re-trigger. */
static volatile bool s_lockscreen_overlay_active = false;

/* Lets views clean transient UI before the lockscreen destroys/rebuilds them. */
typedef struct {
    void (*fn)(void);
    int id; /* monotonically increasing; <0 disables; 0 reserved */
} freeze_pre_lock_cb_t;

static freeze_pre_lock_cb_t s_freeze_cbs[4];
static int s_freeze_cb_next_id = 1;

int display_manager_register_freeze_pre_lock(void (*fn)(void)) {
    if (!fn) return -1;
    for (size_t i = 0; i < sizeof(s_freeze_cbs) / sizeof(s_freeze_cbs[0]); ++i) {
        if (s_freeze_cbs[i].fn == NULL) {
            int id = s_freeze_cb_next_id++;
            if (s_freeze_cb_next_id < 0) s_freeze_cb_next_id = 1;
            s_freeze_cbs[i].fn = fn;
            s_freeze_cbs[i].id = id;
            return id;
        }
    }
    return -1;
}

void display_manager_unregister_freeze_pre_lock(int id) {
    if (id <= 0) return;
    for (size_t i = 0; i < sizeof(s_freeze_cbs) / sizeof(s_freeze_cbs[0]); ++i) {
        if (s_freeze_cbs[i].id == id) {
            s_freeze_cbs[i].fn = NULL;
            s_freeze_cbs[i].id = 0;
            return;
        }
    }
}

static void display_manager_run_freeze_pre_lock(void) {
    for (size_t i = 0; i < sizeof(s_freeze_cbs) / sizeof(s_freeze_cbs[0]); ++i) {
        if (s_freeze_cbs[i].fn) s_freeze_cbs[i].fn();
    }
}

bool display_manager_init_success = false;
static bool status_timer_initialized = false;
static TaskHandle_t lvgl_task_handle = NULL;
static TaskHandle_t input_task_handle = NULL;
#ifdef CONFIG_CROWPANEL_EPAPER_42
/* The e-paper reader, ZIP/FATFS paths, and LVGL callbacks can be much deeper
 * than the normal display loop. Keep this stack in PSRAM so the LVGL task has
 * room without consuming the board's small internal-heap reserve. */
static StackType_t *epaper_lvgl_task_stack = NULL;
static StaticTask_t *epaper_lvgl_task_buffer = NULL;
#endif
/* Cooperative quiesce gate for the LVGL render task. On shared-SPI boards the
 * SD path must free the SPI bus, but it can only do that safely while the render
 * task is OUTSIDE lv_timer_handler() (i.e. not mid-flush). The suspender raises
 * s_lvgl_gate_closed and waits for the render task to acknowledge via
 * s_lvgl_gate_parked before tearing the bus down. Plain bool (not a counter) to
 * match the existing idempotent suspend/resume semantics. */
static volatile bool s_lvgl_gate_closed = false;
static volatile bool s_lvgl_gate_parked = false;
static volatile bool s_input_gate_closed = false;
static volatile bool s_input_gate_parked = false;
/* Serializes every cross-task entry into LVGL's internal timer list/heap
 * (lv_timer_handler() and lv_async_call()); see the creation site for why
 * this must be recursive. */
static SemaphoreHandle_t s_lvgl_call_mutex = NULL;
static lv_timer_t *status_update_timer = NULL;
static lv_timer_t *rainbow_timer = NULL;
static uint16_t rainbow_hue = 0;
/* Avoid redrawing a static status bar. Repeating the same update is expensive
 * on the CrowPanel's PSRAM scanout framebuffer. */
static bool status_snapshot_valid = false;
static bool status_last_wifi_enabled;
static bool status_last_bt_enabled;
static bool status_last_sd_mounted;
static int status_last_battery = -2;
static bool status_last_power_save;
static bool status_last_ap_active;
static bool status_last_charging;
static uint8_t status_last_theme;
static TickType_t last_dim_time = 0; // Initialize to 0
static TickType_t last_touch_time;
static bool is_backlight_dimmed = false;
static bool is_backlight_off = false;
static bool s_switch_backlight_configured = false;
static View *s_joystick_press_owner[5];

static bool dm_joystick_event_matches_view(const InputEvent *event, View *view) {
  if (!event || event->type != INPUT_TYPE_JOYSTICK) return true;
  int index = event->data.joystick_index;
  if (index < 0 || index >= (int)(sizeof(s_joystick_press_owner) /
                                  sizeof(s_joystick_press_owner[0]))) {
    return false;
  }
  if (event->data.joystick_pressed) {
    s_joystick_press_owner[index] = view;
    return true;
  }
  View *owner = s_joystick_press_owner[index];
  s_joystick_press_owner[index] = NULL;
  return owner == NULL || owner == view;
}

#ifdef CONFIG_USE_ENCODER
static encoder_t g_encoder;
static joystick_t enc_button;
static joystick_t exit_button;
static TaskHandle_t encoder_poll_task_handle = NULL;
static void encoder_poll_task(void *pvParameters);
#endif

#define FADE_DURATION_MS GUI_ANIM_TRANSITION
#define DEFAULT_DISPLAY_TIMEOUT_MS 30000

uint32_t display_timeout_ms = DEFAULT_DISPLAY_TIMEOUT_MS;

static inline uint32_t get_joystick_repeat_initial_delay(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 600;  // Slow
        case 1: return 350;  // Normal
        case 2: return 150;  // Fast
        default: return 350;
    }
}

static inline uint32_t get_joystick_repeat_interval(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 250;  // Slow
        case 1: return 120;  // Normal
        case 2: return 60;   // Fast
        default: return 120;
    }
}

static inline uint32_t get_tdeck_repeat_delay(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 800;  // Slow
        case 1: return 500;  // Normal
        case 2: return 200;  // Fast
        default: return 500;
    }
}

static void display_manager_flush_pending_scroll_if_due(void);
static void display_manager_cancel_pending_scroll(void);

#ifdef CONFIG_CROWPANEL_EPAPER_42
static int crowpanel_epaper_logical_joystick_index(int physical_index) {
  /* PRV/NEXT are the board's natural list navigation controls. The existing
   * LVGL views already use slots 2/4 for Up/Down, so preserve that contract
   * while reserving slot 2's physical button for Home below. */
  if (physical_index == 0) return 2;
  if (physical_index == 3) return 4;
  return physical_index;
}
#endif

static inline uint32_t get_tdeck_repeat_rate(void) {
    switch (settings_get_input_repeat_speed(&G_Settings)) {
        case 0: return 200;  // Slow
        case 1: return 100;  // Normal
        case 2: return 50;   // Fast
        default: return 100;
    }
}

static uint16_t original_beacon_interval __attribute__((unused)) = 100;

// Global keyboard key repeat: re-injects the last pressed key while held.
// Uses the same input_repeat_speed setting as joystick repeat.
static uint8_t s_kb_repeat_key = 0;
static esp_timer_handle_t s_kb_repeat_timer = NULL;

static void kb_repeat_fire_cb(void *arg) {
    (void)arg;
    if (!s_kb_repeat_key) return;
    InputEvent ev = {0};
    ev.type = INPUT_TYPE_KEYBOARD;
    ev.data.key_value = s_kb_repeat_key;
    ev.is_touch_move = false;
    ev.is_repeat = true;
    xQueueSend(input_queue, &ev, 0);
}

static void kb_repeat_start(uint8_t key) {
    s_kb_repeat_key = key;
    if (!s_kb_repeat_timer) return;
    if (esp_timer_is_active(s_kb_repeat_timer)) {
        esp_timer_stop(s_kb_repeat_timer);
    }
    // First repeat uses the initial delay, then switch to interval
    esp_timer_start_once(s_kb_repeat_timer, get_joystick_repeat_initial_delay() * 1000);
}

static void kb_repeat_stop(void) {
    s_kb_repeat_key = 0;
    if (s_kb_repeat_timer && esp_timer_is_active(s_kb_repeat_timer)) {
        esp_timer_stop(s_kb_repeat_timer);
    }
}

// Called after initial delay fires — switch to periodic repeat
static void kb_repeat_initial_cb(void *arg) {
    (void)arg;
    if (!s_kb_repeat_key) return;
    // Inject the first repeat immediately
    kb_repeat_fire_cb(NULL);
    // Start periodic repeat at the interval rate
    if (s_kb_repeat_timer && s_kb_repeat_key) {
        esp_timer_start_periodic(s_kb_repeat_timer, get_joystick_repeat_interval() * 1000);
    }
}

#define BACKLIGHT_SLEEP_POLL_MS 50   // Poll slower when dimmed

static inline uint32_t dm_now_ms(void) {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

#ifdef CONFIG_IS_S3TWATCH
#define WAKE_UP_PIN GPIO_NUM_16
static SemaphoreHandle_t wake_up_sem = NULL;
#endif

void set_display_timeout(uint32_t timeout_ms) {
  display_timeout_ms = timeout_ms;
}

#ifdef CONFIG_USE_CARDPUTER
Keyboard_t gkeyboard;

static bool caps_latch = false;
static int shift_count = 0;
static int shift_count_before_caps = 10;
static Point2D_t last_pressed_keys[16];
static size_t last_pressed_len = 0;
#endif

#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_IS_ATOMS3R)
void m5stack_lvgl_render_callback(lv_disp_drv_t *drv, const lv_area_t *area,
                                  lv_color_t *color_p) {
  int32_t x1 = area->x1;
  int32_t y1 = area->y1;
  int32_t x2 = area->x2;
  int32_t y2 = area->y2;

  m5gfx_write_pixels(x1, y1, x2, y2, (uint16_t *)color_p);

  lv_disp_flush_ready(drv);
}
#endif

#ifdef CONFIG_IS_S3TWATCH
static void gpio_isr_handler(void* arg) {
    if (xTaskGetTickCount() - last_dim_time < pdMS_TO_TICKS(1000)) {
        return;
    }
    xSemaphoreGiveFromISR(wake_up_sem, NULL);
}
#endif

static __attribute__((unused)) void invert_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                            lv_color_t *color_p) {
    if (settings_get_invert_colors(&G_Settings)) {
        int w = area->x2 - area->x1 + 1;
        int h = area->y2 - area->y1 + 1;
        int cnt = w * h;
        for (int i = 0; i < cnt; i++) {
            color_p[i].full = ~color_p[i].full;
        }
    }
    
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    // Direct-mode flushes carry the full framebuffer, including intermediate
    // redraws. Mirror only the completed refresh, as the P4 panel driver does.
    if (!drv->direct_mode || lv_disp_flush_is_last(drv)) {
        screen_mirror_send_area(area, color_p);
    }
#else
    screen_mirror_send_area(area, color_p);
#endif
    
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV) || defined(CONFIG_IS_ATOMS3R)
    m5stack_lvgl_render_callback(drv, area, color_p);
#elif defined(CONFIG_USE_TDISPLAY_S3)
    i80_display_flush_cb(drv, area, color_p);
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4)
    crowpanel_p4_display_flush_cb(drv, area, color_p);
#elif defined(CONFIG_CROWPANEL_EPAPER_42)
    crowpanel_epaper_flush_cb(drv, area, color_p);
#else
    disp_driver_flush(drv, area, color_p);
#endif
}

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
static void (*s_p4_buffer_copy)(lv_draw_ctx_t *, void *, lv_coord_t, const lv_area_t *,
                                void *, lv_coord_t, const lv_area_t *);

static void p4_buffer_copy(lv_draw_ctx_t *draw_ctx,
                           void *dest_buf, lv_coord_t dest_stride, const lv_area_t *dest_area,
                           void *src_buf, lv_coord_t src_stride, const lv_area_t *src_area)
{
    s_p4_buffer_copy(draw_ctx, dest_buf, dest_stride, dest_area,
                     src_buf, src_stride, src_area);
}
#endif

void set_backlight_brightness(uint8_t percentage); // forward declaration

#ifdef CONFIG_HAS_BATTERY_ADC

static int s_filtered_mv = -1;
static int s_charge_samples[5];
static int s_charge_sample_idx = 0;
static bool s_charge_samples_filled = false;
static int s_display_percent = -1;

#ifdef CONFIG_USE_CARDPUTER
#define _batAdcCh ADC_CHANNEL_9 //sar adc1 channel 9 - ADC1_GPIO10_CHANNEL;

#define CARDPUTER_SOC_TABLE_SIZE 11
typedef struct {
    int mv;
    uint8_t percent;
} cardputer_soc_point_t;

static const cardputer_soc_point_t s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE] = {
    {3200, 0},
    {3300, 3},
    {3400, 8},
    {3500, 15},
    {3600, 30},
    {3700, 45},
    {3800, 60},
    {3900, 75},
    {4000, 88},
    {4100, 96},
    {4200, 100},
};

static uint8_t cardputer_voltage_to_percent(int mv) {
    if (mv <= s_cardputer_soc_table[0].mv) {
        return s_cardputer_soc_table[0].percent;
    }
    if (mv >= s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].mv) {
        return s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].percent;
    }

    for (int i = 1; i < CARDPUTER_SOC_TABLE_SIZE; i++) {
        if (mv <= s_cardputer_soc_table[i].mv) {
            const cardputer_soc_point_t *low = &s_cardputer_soc_table[i - 1];
            const cardputer_soc_point_t *high = &s_cardputer_soc_table[i];
            int range_mv = high->mv - low->mv;
            if (range_mv <= 0) {
                return high->percent;
            }
            int range_percent = high->percent - low->percent;
            int offset_mv = mv - low->mv;
            return (uint8_t)(low->percent + (range_percent * offset_mv) / range_mv);
        }
    }
    return s_cardputer_soc_table[CARDPUTER_SOC_TABLE_SIZE - 1].percent;
}

#elif CONFIG_USE_TDECK
#define _batAdcCh ADC1_GPIO4_CHANNEL
#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool _isCharging = false;

// track previous battery millivolt for charging detection
static int last_mv = 0;

// T-Deck specific SOC table for more accurate battery percentage
#define TDECK_SOC_TABLE_SIZE 11
typedef struct {
    int mv;
    uint8_t percent;
} tdeck_soc_point_t;

static const tdeck_soc_point_t s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE] = {
    {3300, 0},
    {3400, 5},
    {3500, 12},
    {3600, 25},
    {3700, 40},
    {3800, 55},
    {3900, 70},
    {4000, 82},
    {4100, 92},
    {4200, 98},
    {4300, 100},
};

static uint8_t tdeck_voltage_to_percent(int mv) {
    if (mv <= s_tdeck_soc_table[0].mv) {
        return s_tdeck_soc_table[0].percent;
    }
    if (mv >= s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].mv) {
        return s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].percent;
    }

    for (int i = 1; i < TDECK_SOC_TABLE_SIZE; i++) {
        if (mv <= s_tdeck_soc_table[i].mv) {
            const tdeck_soc_point_t *low = &s_tdeck_soc_table[i - 1];
            const tdeck_soc_point_t *high = &s_tdeck_soc_table[i];
            int range_mv = high->mv - low->mv;
            if (range_mv <= 0) {
                return high->percent;
            }
            int range_percent = high->percent - low->percent;
            int offset_mv = mv - low->mv;
            return (uint8_t)(low->percent + (range_percent * offset_mv) / range_mv);
        }
    }
    return s_tdeck_soc_table[TDECK_SOC_TABLE_SIZE - 1].percent;
}

#elif CONFIG_USE_TDISPLAY_S3
#define _batAdcCh ADC1_GPIO4_CHANNEL
#endif

#ifndef CONFIG_USE_TDECK
#define _batAdcUnit ADC_UNIT_1
#define _batAdcAtten ADC_ATTEN_DB_12
bool _isCharging = false;
static int last_mv = 0;
#endif

// threshold to ignore ADC noise
#define CHARGE_THRESH_MV 30

int getBattery() {
    uint8_t percent;
    static bool init_done = false;
    static adc_oneshot_unit_handle_t handle = NULL;
    static adc_cali_handle_t cali_handle = NULL;

    if (!init_done) {
        const adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = _batAdcUnit,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &handle));
        const adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_DEFAULT,
            .atten = _batAdcAtten,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, _batAdcCh, &chan_cfg));

        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = _batAdcUnit,
            .atten    = _batAdcAtten,
            .bitwidth= ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
            ESP_LOGI(TAG, "ADC calibration scheme ready");
        } else {
            ESP_LOGW(TAG, "ADC calibration not supported, skipping");
        }
        init_done = true;
    }

    // raw ADC → calibrated millivolt
    int raw = 0;
    esp_err_t ret = adc_oneshot_read(handle, _batAdcCh, &raw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        return -1;
    }

    // Check for invalid ADC readings
    if (raw < 0 || raw > 4095) {
        ESP_LOGW(TAG, "Invalid ADC reading: %d", raw);
        return -1;
    }

    int mv = 0;
    if (cali_handle) {
        if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) != ESP_OK) {
            ESP_LOGE(TAG, "Calibration raw_to_voltage failed");
            mv = raw * 3300 / 4095;  // fallback to raw count
        }
    } else {
        // rough estimate if no calibration
        mv = raw * 3300 / 4095;
    }

#ifdef CONFIG_USE_CARDPUTER
    // Cardputer divides the battery voltage roughly in half with a resistor divider.
    // Scale the measured voltage back up to actual battery voltage.
    mv = (mv * 2);
#elif CONFIG_USE_TDECK
    // T-Deck divides the battery voltage roughly in half with a resistor divider.
    // Scale the measured voltage back up to actual battery voltage.
    mv = (mv * 2);
#endif

    // -- charging detection using rolling window to filter noise --
#ifdef CONFIG_USE_CARDPUTER
    s_charge_samples[s_charge_sample_idx] = mv;
    s_charge_sample_idx = (s_charge_sample_idx + 1) % 5;
    if (s_charge_sample_idx == 0) s_charge_samples_filled = true;
    
    if (s_charge_samples_filled) {
        int oldest_mv = s_charge_samples[s_charge_sample_idx];
        int trend = mv - oldest_mv;
        
        if (trend > CHARGE_THRESH_MV) {
            _isCharging = true;
        } else if (trend < -CHARGE_THRESH_MV) {
            _isCharging = false;
        }
    }
#elif CONFIG_USE_TDECK
    // T-Deck: use rolling window like Cardputer for stable charging detection
    s_charge_samples[s_charge_sample_idx] = mv;
    s_charge_sample_idx = (s_charge_sample_idx + 1) % 5;
    if (s_charge_sample_idx == 0) s_charge_samples_filled = true;
    
    if (s_charge_samples_filled) {
        int oldest_mv = s_charge_samples[s_charge_sample_idx];
        int trend = mv - oldest_mv;
        
        // Use higher threshold for T-Deck to avoid false charging detection
        if (trend > (CHARGE_THRESH_MV * 2)) {
            _isCharging = true;
        } else if (trend < -(CHARGE_THRESH_MV * 2)) {
            _isCharging = false;
        }
    }
#else
    if (last_mv != 0) {
        int diff = mv - last_mv;
        if (diff >  CHARGE_THRESH_MV) _isCharging = true;
        if (diff < -CHARGE_THRESH_MV) _isCharging = false;
    }
    last_mv = mv;
#endif

    ESP_LOGD(TAG, "Battery ADC raw: %d, mV: %d", raw, mv);

    // Check for unrealistic voltage values
    // Lower threshold allows for very dead batteries (down to ~2.0V)
    if (mv < 2000 || mv > 5000) {
        ESP_LOGW(TAG, "Battery voltage out of range: %d mV", mv);
        return -1;
    }

    int mv_for_percent = mv;

#ifdef CONFIG_USE_CARDPUTER
    if (s_filtered_mv < 0) {
        s_filtered_mv = mv;
    } else {
        // Faster exponential smoothing to follow charging curve without sudden jumps
        s_filtered_mv = (s_filtered_mv * 7 + mv) / 8;
    }
    mv_for_percent = s_filtered_mv;
#elif CONFIG_USE_TDECK
    // T-Deck: apply filtering for stable readings like Cardputer
    if (s_filtered_mv < 0) {
        s_filtered_mv = mv;
    } else {
        // Moderate smoothing for T-Deck to balance responsiveness and stability
        s_filtered_mv = (s_filtered_mv * 6 + mv * 2) / 8;
    }
    mv_for_percent = s_filtered_mv;
#endif

    // Map measured voltage to a percentage
#ifdef CONFIG_USE_CARDPUTER
    percent = cardputer_voltage_to_percent(mv_for_percent);
#elif CONFIG_USE_TDECK
    percent = tdeck_voltage_to_percent(mv_for_percent);
#else
    const int min_mv = 3300;
    const int max_mv = 4200;
    if (mv_for_percent <= min_mv) {
        percent = 0;
    } else if (mv_for_percent >= max_mv) {
        percent = 100;
    } else {
        percent = (uint8_t)(((mv_for_percent - min_mv) * 100) / (max_mv - min_mv));
    }
#endif

#ifdef CONFIG_USE_CARDPUTER
    if (s_display_percent < 0) {
        s_display_percent = percent;
    } else {
        int diff = percent - s_display_percent;
        int threshold = _isCharging ? 4 : 3;
        if (diff >= threshold) {
            s_display_percent++;
        } else if (diff <= -threshold) {
            s_display_percent--;
        }
        if (s_display_percent < 0) s_display_percent = 0;
        if (s_display_percent > 100) s_display_percent = 100;
    }
    percent = (uint8_t)s_display_percent;
#elif CONFIG_USE_TDECK
    // T-Deck: apply gradual display changes to prevent sudden jumps
    if (s_display_percent < 0) {
        s_display_percent = percent;
    } else {
        int diff = percent - s_display_percent;
        int threshold = _isCharging ? 5 : 3;
        if (diff >= threshold) {
            s_display_percent++;
        } else if (diff <= -threshold) {
            s_display_percent--;
        }
        if (s_display_percent < 0) s_display_percent = 0;
        if (s_display_percent > 100) s_display_percent = 100;
    }
    percent = (uint8_t)s_display_percent;
#endif

    ESP_LOGD(TAG, "Battery percentage: %d%%", percent);
    return percent;
}
bool isCharging() { return _isCharging; }

#endif

/**
 * @brief Get battery information from available sources
 * @param percentage Pointer to store battery percentage
 * @param is_charging Pointer to store charging status
 * @return true if battery data is available, false otherwise
 */
static bool get_battery_info(uint8_t *percentage, bool *is_charging) {
    bool result = false;
    if (!percentage || !is_charging) {
        return result;
    }

    // Cache for non-fuel-gauge configurations to avoid I2C access in low mode
    static uint8_t last_pct_cache = 0;
    static bool last_chg_cache = false;
    static bool last_valid_cache = false;

    // When low-I2C mode is active, avoid any fresh I2C queries here.
    // For fuel-gauge configs, we already maintain cached values via the background task.
    if (g_low_i2c_mode) {
#ifdef CONFIG_HAS_FUEL_GAUGE
        if (g_cached_batt_valid) {
            *percentage = g_cached_batt_percent;
            *is_charging = g_cached_batt_charging;
            return true;
        }
#endif
        if (last_valid_cache) {
            *percentage = last_pct_cache;
            *is_charging = last_chg_cache;
            return true;
        }
        return false;
    }

#ifdef CONFIG_HAS_FUEL_GAUGE
    // Lazy poll on UI thread with rate limiting (every 5s)
    static int64_t last_poll_time = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (!g_cached_batt_valid || (now - last_poll_time > 5000)) {
        fuel_gauge_data_t fg;
        if (fuel_gauge_manager_get_data(&fg)) {
            g_cached_batt_percent = (uint8_t)fg.percentage;
            g_cached_batt_charging = fg.is_charging;
            g_cached_batt_valid = true;
            last_poll_time = now;
        }
    }

    if (g_cached_batt_valid) {
        *percentage = g_cached_batt_percent;
        *is_charging = g_cached_batt_charging;
        result = true;
    }
#elif defined(CONFIG_HAS_BATTERY)
    // Fallback to AXP2101
    axp2101_get_power_level(percentage);
    *is_charging = axp202_is_charging();
    result = true;
#elif defined(CONFIG_HAS_BATTERY_ADC)
    // Fallback to ADC
    int battery_percent = getBattery();
    if (battery_percent >= 0) {
        *percentage = (uint8_t)battery_percent;
        *is_charging = isCharging();
        result = true;
    } else {
        ESP_LOGW(TAG, "ADC battery read failed, using cached values if available");
        if (last_valid_cache) {
            *percentage = last_pct_cache;
            *is_charging = last_chg_cache;
            result = true;
        }
    }
#endif
    ESP_LOGD(TAG, "get_battery_info %d%%, Charging: %d", *percentage, *is_charging);

    // Update local cache for non-fuel-gauge builds
#if !defined(CONFIG_HAS_FUEL_GAUGE)
    if (result) {
        last_pct_cache = *percentage;
        last_chg_cache = *is_charging;
        last_valid_cache = true;
    }
#endif

    return result;
}

void display_manager_set_low_i2c_mode(bool on) {
    g_low_i2c_mode = on;
}

static void slide_set_x(void *var, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)var;
    lv_coord_t cur = lv_obj_get_x(o);
    if (cur == (lv_coord_t)v) return;
    lv_obj_set_x(o, (lv_coord_t)v);
}

void fade_out_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

void fade_in_cb(void *obj, int32_t v) {
  if (obj) {
    lv_obj_set_style_opa(obj, v, LV_PART_MAIN);
  }
}

static void rainbow_effect_cb(lv_timer_t *timer) {
}

void display_manager_set_rainbow_mode(bool enable) {
  if (enable) {
    if (rainbow_timer == NULL) {
      rainbow_timer = lv_timer_create(rainbow_effect_cb, 50, NULL);
      rainbow_hue = 0;
    }
  } else {
    if (rainbow_timer != NULL) {
      lv_timer_del(rainbow_timer);
      rainbow_timer = NULL;
      display_manager_update_status_bar_color();
    }
  }
}


static bool g_use_slide_transition = true;

void display_manager_fade_out(lv_obj_t *obj, lv_anim_ready_cb_t ready_cb,
                              View *view) {
  if (settings_get_reduced_motion(&G_Settings) || GUI_EPAPER) {
    // Skip animation - set final state immediately
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    if (ready_cb) {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_user_data(&anim, view);
      ready_cb(&anim);
    }
    return;
  }
  if (g_use_slide_transition && obj && lv_obj_is_valid(obj)) {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, 0, -(LV_HOR_RES / 3));
      lv_anim_set_time(&anim, GUI_ANIM_TRANSITION);
      lv_anim_set_path_cb(&anim, gui_anim_path_decel);
      lv_anim_set_exec_cb(&anim, slide_set_x);
      lv_anim_set_ready_cb(&anim, ready_cb);
      lv_anim_set_user_data(&anim, view);
      lv_anim_start(&anim);
  } else {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_OPA_COVER, LV_OPA_TRANSP);
      lv_anim_set_time(&anim, FADE_DURATION_MS);
      lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_out_cb);
      lv_anim_set_ready_cb(&anim, ready_cb);
      lv_anim_set_user_data(&anim, view);
      lv_anim_start(&anim);
  }
}

void display_manager_fade_in(lv_obj_t *obj) {
  if (!obj) return;
  if (settings_get_reduced_motion(&G_Settings) || GUI_EPAPER) {
    // Skip animation - set final state immediately
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    return;
  }
  if (g_use_slide_transition) {
      lv_obj_set_x(obj, LV_HOR_RES);
      lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);

      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_HOR_RES, 0);
      lv_anim_set_time(&anim, GUI_ANIM_TRANSITION);
      lv_anim_set_path_cb(&anim, gui_anim_path_decel);
      lv_anim_set_exec_cb(&anim, slide_set_x);
      lv_anim_start(&anim);
  } else {
      lv_anim_t anim;
      lv_anim_init(&anim);
      lv_anim_set_var(&anim, obj);
      lv_anim_set_values(&anim, LV_OPA_TRANSP, LV_OPA_COVER);
      lv_anim_set_time(&anim, FADE_DURATION_MS);
      lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)fade_in_cb);
      lv_anim_start(&anim);
  }
}

// recursively set radius for obj and its children (used to avoid mask draws during heavy transitions)
static void set_radius_recursive(lv_obj_t *obj, lv_coord_t r) {
  if (!obj) return;
  lv_obj_set_style_radius(obj, r, 0);
  uint32_t cnt = lv_obj_get_child_cnt(obj);
  for (uint32_t i = 0; i < cnt; i++) {
    lv_obj_t *c = lv_obj_get_child(obj, i);
    if (c) set_radius_recursive(c, r);
  }
}

void fade_out_ready_cb(lv_anim_t *anim) {
  display_manager_destroy_current_view();

  View *new_view = (View *)anim->user_data;
  if (new_view) {
    dm.previous_view = dm.current_view;
    dm.current_view = new_view;

    if (new_view->get_hardwareinput_callback) {
      new_view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
    }

    new_view->create();

    // Avoid running the per-tick fade animation for specific heavy views
    // because the opacity animation forces heavy draw work (masks/labels)
    // and can starve the LVGL tick/watchdog during the fade-in.
    if (new_view->name && strcmp(new_view->name, "Keyboard Screen") == 0) {
      if (new_view->root) {
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else if (new_view->name && strcmp(new_view->name, "Options Screen") == 0 && SelectedMenuType == OT_DualComm) {
      if (new_view->root) {
        lv_obj_set_style_opa(new_view->root, LV_OPA_COVER, 0);
        set_radius_recursive(new_view->root, 0);
      }
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else {
      display_manager_fade_in(new_view->root);
      if (status_bar) {
        lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
      }
    }
  }
}

lv_color_t hex_to_lv_color(const char *hex_str) {
  if (!hex_str) return lv_color_black();
  if (hex_str[0] == '#') {
    hex_str++;
  }

  if (strlen(hex_str) != 6) {
    ESP_LOGE(TAG, "Invalid hex color format. Expected 6 characters.\n");
    return lv_color_white();
  }

  // Parse the hex string into RGB values
  char r_str[3] = {hex_str[0], hex_str[1], '\0'};
  char g_str[3] = {hex_str[2], hex_str[3], '\0'};
  char b_str[3] = {hex_str[4], hex_str[5], '\0'};

  uint8_t r = (uint8_t)strtol(r_str, NULL, 16);
  uint8_t g = (uint8_t)strtol(g_str, NULL, 16);
  uint8_t b = (uint8_t)strtol(b_str, NULL, 16);

  return lv_color_make(r, g, b);
}

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted,
  int batteryPercentage, bool power_save_enabled, bool is_ap_active, bool is_charging) {
  uint8_t theme = settings_get_menu_theme(&G_Settings);
  bool changed = !status_snapshot_valid ||
                 status_last_wifi_enabled != wifi_enabled ||
                 status_last_bt_enabled != bt_enabled ||
                 status_last_sd_mounted != sd_card_mounted ||
                 status_last_battery != batteryPercentage ||
                 status_last_power_save != power_save_enabled ||
                 status_last_ap_active != is_ap_active ||
                 status_last_charging != is_charging ||
                 status_last_theme != theme;
  if (!changed) return;

  status_snapshot_valid = true;
  status_last_wifi_enabled = wifi_enabled;
  status_last_bt_enabled = bt_enabled;
  status_last_sd_mounted = sd_card_mounted;
  status_last_battery = batteryPercentage;
  status_last_power_save = power_save_enabled;
  status_last_ap_active = is_ap_active;
  status_last_charging = is_charging;
  status_last_theme = theme;

  // Update visibility of status icons
  if (sd_card_mounted) {
    lv_obj_clear_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (bt_enabled) {
    lv_obj_clear_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (wifi_enabled) {
    lv_obj_clear_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  }

  if (batteryPercentage < 0){
    lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
    // Update battery icon and percentage
    const char *battery_symbol;

    battery_symbol = (batteryPercentage > 75) ? LV_SYMBOL_BATTERY_FULL :
             (batteryPercentage > 50) ? LV_SYMBOL_BATTERY_3 :
             (batteryPercentage > 25) ? LV_SYMBOL_BATTERY_2 :
             (batteryPercentage > 10) ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;

    if (is_charging) {
      battery_symbol = LV_SYMBOL_CHARGE;
    }
    lv_label_set_text_fmt(battery_label, "%s %d%%", battery_symbol, batteryPercentage);
  }

#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  /* The round panel has only a compact top pill. Keep secondary status
   * badges out of it; Wi-Fi is the single persistent system indicator and
   * the active view owns feature-specific state such as USB HID readiness. */
  lv_obj_add_flag(level_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
#endif

  lv_obj_invalidate(status_bar);

  // set status bar icon colors based on power save mode and AP state
  lv_color_t default_color = lv_color_hex(theme_palette_get_text_muted(theme));
  
  // WiFi icon color logic
  if (wifi_label && lv_obj_is_valid(wifi_label)) {
      // Check if AP should be active (enabled in settings AND power saving disabled)
      bool ap_should_be_active = settings_get_ap_enabled(&G_Settings) && !power_save_enabled;
      
      if (!ap_should_be_active) {
          lv_obj_set_style_text_color(wifi_label, default_color, 0);
      } else if (wifi_manager_is_evil_portal_active()) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x60A5FA), 0);
      } else if (is_ap_active) {
          lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x22C55E), 0);
      } else {
          lv_obj_set_style_text_color(wifi_label, default_color, 0);
      }
  }
  
  if (power_save_enabled) {
    lv_color_t amber_color = lv_color_hex(0xF59E0B);
    if (battery_label && lv_obj_is_valid(battery_label)) {
      lv_obj_set_style_text_color(battery_label, amber_color, 0);
    }
  } else {
    if (bt_label && lv_obj_is_valid(bt_label)) {
      lv_obj_set_style_text_color(bt_label, default_color, 0);
    }
    if (sd_label && lv_obj_is_valid(sd_label)) {
      lv_obj_set_style_text_color(sd_label, default_color, 0);
    }
    if (battery_label && lv_obj_is_valid(battery_label)) {
      lv_color_t battery_color = default_color;

      if (is_charging) {
        battery_color = lv_color_hex(0x22C55E);
      } else if (batteryPercentage <= 20) {
        battery_color = lv_color_hex(0xEF4444);
      }
      lv_obj_set_style_text_color(battery_label, battery_color, 0);
    }
  }
}

static void status_update_cb(lv_timer_t *timer) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  if (is_backlight_off) return; // Skip updates when backlight is off

  bool HasBluetooth;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif
  bool server_running = false;
  ap_manager_get_status(&server_running, NULL, NULL); // Get AP server status

  // Get battery information from available sources
  uint8_t power_level = 0;
  bool is_charging = false;
  bool has_battery = get_battery_info(&power_level, &is_charging);

  int battery_percentage = has_battery ? power_level : -1;

  // Debug logging for battery status
  ESP_LOGD(TAG, "Status update - Battery: %d%%, Charging: %s, Has battery: %s",
           battery_percentage, is_charging ? "YES" : "NO", has_battery ? "YES" : "NO");

  // WiFi icon should always be visible - pass true for wifi_enabled
  // Color will be determined by AP state and power saving mode in update_status_bar
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    battery_percentage, settings_get_power_save_enabled(&G_Settings), server_running, is_charging);

  if (level_label && lv_obj_is_valid(level_label)) {
    ghostchi_snapshot_t snap;
    ghostchi_manager_get_snapshot(&snap);
    static const unsigned int lv_xp[] = {
        0, 10, 40, 90, 160, 250, 360, 490, 640, 810, 1000,
        1210, 1440, 1690, 1960, 2250, 2560, 2890, 3240, 3610, 4000,
        4410, 4840, 5290, 5760, 6250, 6760, 7290, 7840, 8410, 9000,
        9610, 10240, 10890, 11560, 12250, 12960, 13690, 14440, 15210, 16000,
        16810, 17640, 18490, 19360, 20250, 21160, 22090, 23040, 24010, 25000
    };
    unsigned int level = 1;
    unsigned int xp = snap.total_xp;
    for (size_t i = 1; i < sizeof(lv_xp) / sizeof(lv_xp[0]); ++i) {
      if (xp < lv_xp[i]) { level = (unsigned int)i; break; }
      if (i == sizeof(lv_xp) / sizeof(lv_xp[0]) - 1) level = (unsigned int)i;
    }
    char level_text[16];
    snprintf(level_text, sizeof(level_text), "Lv%u", level);
    if (strcmp(lv_label_get_text(level_label), level_text) != 0) {
      lv_label_set_text(level_label, level_text);
    }
#ifndef CONFIG_CROWPANEL_1P28_ROTARY
    lv_obj_clear_flag(level_label, LV_OBJ_FLAG_HIDDEN);
#endif
  }
}

void display_manager_update_status_bar_color(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) {
    return;
  }

  uint8_t theme = settings_get_menu_theme(&G_Settings);
  lv_color_t accent_color = lv_color_hex(theme_palette_get_accent(theme));
  lv_color_t status_bg_color = lv_color_hex(theme_palette_get_surface_alt(theme));
  lv_color_t text_color = lv_color_hex(theme_palette_get_text_muted(theme));
  lv_color_t primary_text = lv_color_hex(theme_palette_get_text(theme));
  
  lv_obj_set_style_bg_color(status_bar, status_bg_color, LV_PART_MAIN);
  lv_obj_set_style_border_color(status_bar, accent_color, LV_PART_MAIN);

  // Reset all status bar label colors when leaving rainbow mode
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    lv_obj_set_style_text_color(mainlabel, primary_text, 0);
  }
  if (wifi_label && lv_obj_is_valid(wifi_label)) {
    lv_obj_set_style_text_color(wifi_label, text_color, 0);
  }
  if (bt_label && lv_obj_is_valid(bt_label)) {
    lv_obj_set_style_text_color(bt_label, text_color, 0);
  }
  if (sd_label && lv_obj_is_valid(sd_label)) {
    lv_obj_set_style_text_color(sd_label, text_color, 0);
  }
  if (battery_label && lv_obj_is_valid(battery_label)) {
    lv_obj_set_style_text_color(battery_label, text_color, 0);
  }
  if (level_label && lv_obj_is_valid(level_label)) {
    lv_obj_set_style_text_color(level_label, text_color, 0);
  }

  status_update_cb(NULL);
}

static void level_label_click_cb(lv_event_t *e) {
    (void)e;
#ifdef CONFIG_WITH_SCREEN
    display_manager_switch_view(&ghostchi_view);
#endif
}

const char *display_manager_get_status_title(void) {
  if (status_bar && lv_obj_is_valid(status_bar) && mainlabel && lv_obj_is_valid(mainlabel)) {
    return lv_label_get_text(mainlabel);
  }
  return "";
}

void display_manager_add_status_bar(const char *CurrentMenuName) {
    const char *label_text = CurrentMenuName ? CurrentMenuName : "";
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t status_bg_color = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t status_text_color = lv_color_hex(theme_palette_get_text_muted(theme));

    if (status_bar && lv_obj_is_valid(status_bar)) {
        if (mainlabel && lv_obj_is_valid(mainlabel)) {
            lv_label_set_text(mainlabel, label_text);
            lv_obj_move_foreground(status_bar);
            return;
        }
        lv_obj_t *old_bar = status_bar;
        status_bar = NULL;
        status_snapshot_valid = false;
        mainlabel = NULL;
        wifi_label = NULL;
        bt_label = NULL;
        sd_label = NULL;
        battery_label = NULL;
        level_label = NULL;
        lvgl_obj_del_safe(&old_bar);
    }
    status_bar = lv_obj_create(lv_scr_act());
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  /* Keep the status strip inside the circular aperture rather than placing
   * text/icons at the square framebuffer corners. */
  lv_obj_set_size(status_bar, 112, 20);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_set_size(status_bar, LV_HOR_RES, GUI_STATUS_BAR_H);
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
#endif
  lv_obj_set_style_bg_color(status_bar, status_bg_color, LV_PART_MAIN);
  lv_obj_set_scrollbar_mode(status_bar, LV_SCROLLBAR_MODE_OFF);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_bar, 1, LV_PART_MAIN);
#else
  lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_border_width(status_bar, 2, LV_PART_MAIN);
#endif
  lv_obj_set_style_border_color(status_bar, lv_color_hex(theme_palette_get_accent(theme)), LV_PART_MAIN);
  lv_obj_set_style_border_opa(status_bar, LV_OPA_40, LV_PART_MAIN);
  lv_obj_set_style_pad_all(status_bar, 0, LV_PART_MAIN);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_style_radius(status_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
#else
  lv_obj_set_style_radius(status_bar, 0, LV_PART_MAIN);
#endif

  lv_obj_t *left_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(left_container);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_size(left_container, 72, 20);
#else
  lv_obj_set_size(left_container, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
#endif
  lv_obj_set_flex_flow(left_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(left_container, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(left_container, LV_ALIGN_CENTER,
#ifdef CONFIG_IS_ATOMS3R
               0,
#else
               0,
#endif
               0);
  mainlabel = lv_label_create(left_container);
  lv_label_set_text(mainlabel, label_text);
  lv_obj_set_style_text_color(mainlabel, lv_color_hex(theme_palette_get_text(theme)), 0);
  lv_label_set_long_mode(mainlabel, LV_LABEL_LONG_DOT);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_width(mainlabel, 72);
  lv_obj_set_style_text_align(mainlabel, LV_TEXT_ALIGN_CENTER, 0);
#else
  lv_obj_set_width(mainlabel, LV_HOR_RES / 2 - GUI_SAFEAREA_HOR);
#endif
  lv_obj_set_style_text_font(mainlabel, accessibility_get_font_small(), 0);

  lv_obj_t *right_container = lv_obj_create(status_bar);
  lv_obj_remove_style_all(right_container);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_size(right_container, 28, 20);
#else
  lv_obj_set_size(right_container, lv_pct(50), GUI_STATUS_BAR_H);
#endif
  lv_obj_set_flex_flow(right_container, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(right_container, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_set_style_pad_column(right_container, 0, 0);
#else
  lv_obj_set_style_pad_column(right_container, GUI_GRID, 0);
#endif
  lv_obj_align(right_container, LV_ALIGN_RIGHT_MID,
#ifdef CONFIG_IS_ATOMS3R
               -1, /* tighter inset: push icons further right */
#elif defined(CONFIG_CROWPANEL_1P28_ROTARY)
               -4,
#else
               -GUI_GRID,
#endif
               0);
  level_label = lv_label_create(right_container);
  lv_label_set_text(level_label, "");
  lv_obj_set_style_text_color(level_label, status_text_color, 0);
  lv_obj_set_style_text_font(level_label, accessibility_get_font_small(), 0);
  lv_obj_add_flag(level_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(level_label, level_label_click_cb, LV_EVENT_CLICKED, NULL);
  sd_label = lv_label_create(right_container);
  lv_label_set_text(sd_label, LV_SYMBOL_SD_CARD);
  lv_obj_set_style_text_color(sd_label, status_text_color, 0);
  lv_obj_set_style_text_font(sd_label, accessibility_get_font_icon(), 0);
  lv_obj_add_flag(sd_label, LV_OBJ_FLAG_HIDDEN);
  bt_label = lv_label_create(right_container);
  lv_label_set_text(bt_label, LV_SYMBOL_BLUETOOTH);
  lv_obj_set_style_text_color(bt_label, status_text_color, 0);
  lv_obj_set_style_text_font(bt_label, accessibility_get_font_icon(), 0);
  lv_obj_add_flag(bt_label, LV_OBJ_FLAG_HIDDEN);
  wifi_label = lv_label_create(right_container);
  lv_label_set_text(wifi_label, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(wifi_label, status_text_color, 0);
  lv_obj_set_style_text_font(wifi_label, accessibility_get_font_icon(), 0);
  lv_obj_add_flag(wifi_label, LV_OBJ_FLAG_HIDDEN);
  battery_label = lv_label_create(right_container);
  lv_label_set_text(battery_label, "");
  lv_obj_set_style_text_color(battery_label, status_text_color, 0);
  lv_obj_set_style_text_font(battery_label, accessibility_get_font_icon(), 0);
  lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);

  bool HasBluetooth;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
  HasBluetooth = true;
#else
  HasBluetooth = false;
#endif

  bool server_running = false;
  ap_manager_get_status(&server_running, NULL, NULL); // Get AP server status

  // Get battery information from available sources
  uint8_t power_level = 0;
  bool is_charging = false;
  bool has_battery = get_battery_info(&power_level, &is_charging);

  int battery_percentage = has_battery ? power_level : -1;
  
  // WiFi icon should always be visible - pass true for wifi_enabled
  // Color will be determined by AP state and power saving mode in update_status_bar
  update_status_bar(true, HasBluetooth, sd_card_manager.is_initialized,
                    battery_percentage, settings_get_power_save_enabled(&G_Settings), server_running, is_charging);
  if (!status_timer_initialized) {
    status_update_timer = lv_timer_create(status_update_cb, 500, NULL);
    status_timer_initialized = true;
  }
}

void display_manager_set_status_bar_hidden(bool hidden) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  if (hidden) {
    lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(status_bar);
  }
}

void display_manager_raise_status_bar(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  lv_obj_set_parent(status_bar, lv_layer_top());
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
#endif
  lv_obj_move_foreground(status_bar);
}

void display_manager_restore_status_bar(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  lv_obj_set_parent(status_bar, lv_scr_act());
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
#endif
  lv_obj_move_foreground(status_bar);
}

void apply_power_management_config(bool power_save_enabled) {
  esp_pm_config_t pm_cfg = {
      .max_freq_mhz = power_save_enabled ? 160 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
      .min_freq_mhz = power_save_enabled ? 80 : CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
#ifdef CONFIG_USE_TDECK
      .light_sleep_enable = false, // Disable light sleep for T-Deck to prevent trackball issues
#else
      .light_sleep_enable = power_save_enabled, // Keep light sleep for other configs
#endif
  };
  rgb_manager_power_transition_begin();
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }
  rgb_manager_power_transition_end();

#if defined(CONFIG_LV_DISP_BACKLIGHT_PWM) && !defined(CONFIG_IS_ATOMS3R)
  // Reconfigure LEDC timer after power management changes to maintain stable PWM
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = BACKLIGHT_TIMER,
#ifdef CONFIG_USE_ATOMS3R_BUTTON
       .freq_hz = 500, // AtomS3R LP5562 backlight recommendation
#else
       .freq_hz = 5000, // 5 kHz
#endif
      .clk_cfg = LEDC_USE_RC_FAST_CLK, // Auto-select best clock for current power mode
  };
  esp_err_t timer_err = ledc_timer_config(&ledc_timer);
  uint32_t current_duty = ledc_get_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  ESP_LOGI(TAG, "LEDC timer reconfigured for power save mode: %s, timer_err=%s, current_duty=%lu",
           power_save_enabled ? "enabled" : "disabled", esp_err_to_name(timer_err), (unsigned long)current_duty);
#endif

  // control ap based on power save mode and AP enabled setting
  if (!wifi_manager_is_evil_portal_active()) {
    if (power_save_enabled) {
      ap_manager_stop_services();
    } else if (settings_get_ap_enabled(&G_Settings)) {
      (void)ap_manager_restore_after_attack("power save off");
    }
  }
}

#ifdef CONFIG_Waveshare_LCD
static void waveshare_ch422g_init(void)
{
  esp_io_expander_ch422g_t *ch422g_dev = NULL;
  esp_err_t err = ch422g_new_device(I2C_NUM_0, 0x24, &ch422g_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CH422G init failed: %s", esp_err_to_name(err));
    return;
  }

  uint32_t direction = 0, output_value = 0;
  ch422g_read_direction_reg(ch422g_dev, &direction);
  ch422g_read_output_reg(ch422g_dev, &output_value);

  // EXIO1 = Touch Reset (set as output, HIGH to release touch from reset)
  direction &= ~(1 << 1);
  output_value |= (1 << 1);

  // EXIO2 = Backlight Enable (set as output, HIGH to enable backlight)
  direction &= ~(1 << 2);
  output_value |= (1 << 2);

  // EXIO4 = SD Card CS (set as output, HIGH to disable SD card)
  direction &= ~(1 << 4);
  output_value |= (1 << 4);

  ch422g_write_direction_reg(ch422g_dev, direction);
  ch422g_write_output_reg(ch422g_dev, output_value);

  cleanup_resources(ch422g_dev, I2C_NUM_0);
  ESP_LOGI(TAG, "CH422G initialized: EXIO1=1 (touch rst), EXIO2=1 (backlight), EXIO4=1 (SD CS)");
}
#endif

void display_manager_init(void) {
  ESP_LOGI(TAG, "display_manager_init: starting, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  static bool lvgl_lock_registered = false;
  if (!lvgl_lock_registered) {
    lvgl_i2c_locking(i2c_bus_get_lock_handle());
    lvgl_lock_registered = true;
  }

#ifdef CONFIG_Waveshare_LCD
  static bool ch422g_initialized = false;
  if (!ch422g_initialized) {
    waveshare_ch422g_init();
    ch422g_initialized = true;
  }
#endif

  ESP_LOGI(TAG, "display_manager: configuring power management...");
  esp_pm_config_t pm_cfg = {
    .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
    .min_freq_mhz = 80,
    .light_sleep_enable = true,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_cfg);
  if (pm_err != ESP_OK) {
    ESP_LOGW(TAG, "pm configure failed: %s", esp_err_to_name(pm_err));
  }
  ESP_LOGI(TAG, "display_manager: power management configured, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  apply_power_management_config(settings_get_power_save_enabled(&G_Settings));

  // Configure LEDC timer for backlight (only for PWM boards or TDisplay S3)
#if defined(CONFIG_USE_TDISPLAY_S3) || (defined(CONFIG_LV_DISP_BACKLIGHT_PWM) && !defined(CONFIG_IS_ATOMS3R))
  ledc_timer_config_t ledc_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = BACKLIGHT_TIMER,
#ifdef CONFIG_USE_ATOMS3R_BUTTON
       .freq_hz = 500, // AtomS3R LP5562 backlight recommendation
#else
       .freq_hz = 5000, // 5 kHz
#endif
      .clk_cfg = LEDC_USE_RC_FAST_CLK, // Use stable APB clock for reliable PWM
  };
  ledc_timer_config(&ledc_timer);

  // Configure LEDC channel for backlight
  ledc_channel_config_t ledc_channel = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = BACKLIGHT_TIMER,
      .intr_type = LEDC_INTR_DISABLE,
#ifdef CONFIG_USE_TDISPLAY_S3
      .gpio_num = I80_BUS_BL_GPIO, // Use I80 backlight pin for TDisplay S3
#else
      .gpio_num = CONFIG_LV_DISP_PIN_BCKL,
#endif
      .duty = 0, // Set initial duty to 0
      .hpoint = 0,
      .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
  };
  #ifdef CONFIG_USE_TDISPLAY_S3
  ledc_channel_config(&ledc_channel);
  #else
  if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
    ledc_channel_config(&ledc_channel);
  } else {
    ESP_LOGI(TAG, "Backlight GPIO not configured; skipping LEDC channel init");
  }
  #endif
  ESP_LOGI(TAG, "display_manager: LEDC configured, free internal RAM: %d bytes",
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif // CONFIG_USE_TDISPLAY_S3 || CONFIG_LV_DISP_BACKLIGHT_PWM

#ifdef CONFIG_USE_TDECK
set_keyboard_brightness(0xFF); // Set to 100% brightness

gpio_install_isr_service(0);

gpio_config_t trackball_io_conf = {
    .pin_bit_mask = (1ULL << CONFIG_U_BTN) | (1ULL << CONFIG_D_BTN) | 
                    (1ULL << CONFIG_L_BTN) | (1ULL << CONFIG_R_BTN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE,
};
gpio_config(&trackball_io_conf);

gpio_isr_handler_add(CONFIG_U_BTN, trackball_isr_up, NULL);
gpio_isr_handler_add(CONFIG_D_BTN, trackball_isr_down, NULL);
gpio_isr_handler_add(CONFIG_L_BTN, trackball_isr_left, NULL);
gpio_isr_handler_add(CONFIG_R_BTN, trackball_isr_right, NULL);
ESP_LOGI(TAG, "T-Deck trackball ISRs registered");
#endif
#ifndef CONFIG_JC3248W535EN_LCD
  // Initialize I2C driver for touch functionality
#ifdef CONFIG_USE_TDISPLAY_S3
  ESP_LOGI(TAG, "Initializing I2C for touch functionality");
  esp_err_t i2c_ret = i2c_master_get_bus_handle(I2C_NUM_0, &s_touch_i2c_bus);
  if (i2c_ret == ESP_ERR_NOT_FOUND || i2c_ret == ESP_ERR_INVALID_STATE) {
    i2c_master_bus_config_t i2c_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = 18,
      .scl_io_num = 17,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags.enable_internal_pullup = true,
    };
    i2c_ret = i2c_new_master_bus(&i2c_config, &s_touch_i2c_bus);
  }
  if (i2c_ret == ESP_OK) {
    ESP_LOGI(TAG, "I2C bus ready for touch functionality");
  } else {
    ESP_LOGE(TAG, "Failed to initialize touch I2C bus: %s", esp_err_to_name(i2c_ret));
  }
#endif
#ifdef CONFIG_Waveshare_LCD
  ESP_LOGI(TAG, "Pre-initializing I2C bus for Waveshare 7-inch touch");
  esp_err_t ws_i2c_ret = i2c_master_get_bus_handle(I2C_NUM_0, &s_touch_i2c_bus);
  if (ws_i2c_ret == ESP_ERR_NOT_FOUND || ws_i2c_ret == ESP_ERR_INVALID_STATE) {
    i2c_master_bus_config_t ws_i2c_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = 8,
      .scl_io_num = 9,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags.enable_internal_pullup = true,
    };
    ws_i2c_ret = i2c_new_master_bus(&ws_i2c_config, &s_touch_i2c_bus);
  }
  if (ws_i2c_ret == ESP_OK) {
    ESP_LOGI(TAG, "Waveshare I2C bus ready for touch (SDA=8, SCL=9)");
  } else {
    ESP_LOGE(TAG, "Failed to initialize Waveshare I2C bus: %s", esp_err_to_name(ws_i2c_ret));
  }
#endif
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
  esp_err_t crowpanel_i2c_ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
  if (crowpanel_i2c_ret != ESP_OK && crowpanel_i2c_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to initialize CrowPanel touch I2C bus: %s",
             esp_err_to_name(crowpanel_i2c_ret));
  }
#endif
#if defined(CONFIG_CROWPANEL_ADVANCE_24_LCD) || defined(CONFIG_CROWPANEL_ADVANCE_28_LCD)
  /* The 2.4/2.8 GT911 shares the I2C bus with the RTC. Initialize it before
   * the LVGL driver so touch probing cannot race another early I2C client. */
  ESP_LOGI(TAG, "Pre-initializing CrowPanel 2.4/2.8 touch I2C bus (SDA=15, SCL=16)");
  esp_err_t crowpanel_small_i2c_ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
  if (crowpanel_small_i2c_ret != ESP_OK && crowpanel_small_i2c_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to initialize CrowPanel 2.4/2.8 touch I2C bus: %s",
             esp_err_to_name(crowpanel_small_i2c_ret));
  }
#endif
#ifdef CONFIG_CROWPANEL_ADVANCE_RGB_LCD
  esp_err_t crowpanel_7_i2c_ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
  if (crowpanel_7_i2c_ret != ESP_OK && crowpanel_7_i2c_ret != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to initialize CrowPanel Advance 7 touch I2C bus: %s",
             esp_err_to_name(crowpanel_7_i2c_ret));
  }
#endif
  ESP_LOGI(TAG, "display_manager: initializing LVGL...");
  lv_init();
  ESP_LOGI(TAG, "display_manager: LVGL core init done, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  /* The factory firmware enables both board power rails before touching the
   * display. GPIO40 is the active-low power indicator. Do this before the
   * generic SPI/LVGL driver so the panel and touch controller are powered
   * during their reset/probe sequences. */
  gpio_config_t crowpanel_power = {
    .pin_bit_mask = (1ULL << 1) | (1ULL << 2) | (1ULL << 40),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&crowpanel_power);
  gpio_set_level(1, 1);
  gpio_set_level(2, 1);
  gpio_set_level(40, 0);
  ESP_LOGI(TAG, "CrowPanel rotary power rails enabled (GPIO1/GPIO2), indicator active");
#endif
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV) || defined(CONFIG_IS_ATOMS3R)
  init_m5gfx_display();
#elif defined(CONFIG_USE_TDISPLAY_S3)
  esp_err_t ret = i80_display_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "I80 display initialization failed: %s", esp_err_to_name(ret));
    return;
  }
  /* The I80 path skips lvgl_driver_init(), so the touch driver (CST820) must
   * be initialized here or its device handle stays NULL and every read fails
   * with "i2c handle not initialized". */
  touch_driver_init();
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4)
  esp_err_t ret = crowpanel_p4_display_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "CrowPanel Advanced P4 display initialization failed: %s", esp_err_to_name(ret));
    return;
  }
  ret = lvgl_i2c_init(CONFIG_LV_I2C_TOUCH_PORT);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "CrowPanel touch I2C initialization failed: %s", esp_err_to_name(ret));
    return;
  }
  ret = crowpanel_p4_display_touch_reset();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "CrowPanel touch reset failed: %s", esp_err_to_name(ret));
    return;
  }
  touch_driver_init();
#elif defined(CONFIG_CROWPANEL_EPAPER_42)
  esp_err_t ret = crowpanel_epaper_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "CrowPanel 4.2 e-paper initialization failed: %s", esp_err_to_name(ret));
    return;
  }
#elif defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  /* The Advance 7 uses the dedicated RGB driver below. Keep the legacy LVGL
   * display helper out of this branch; only initialize GT911 touch here. */
  touch_driver_init();
#else
  lvgl_driver_init();
#endif
  ESP_LOGI(TAG, "display_manager: display driver init done, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#if defined(CONFIG_CROWPANEL_ADVANCE_35_LCD) || defined(CONFIG_CROWPANEL_ADVANCE_24_LCD) || defined(CONFIG_CROWPANEL_ADVANCE_28_LCD)
  // Factory HMI3-5 drives backlight via GPIO38 HIGH (ST7789/ILI9488 3.5" etc)
  // Ghost generic ILI9488 path leaves it LOW → black screen.
  gpio_config_t bl38 = {
      .pin_bit_mask = 1ULL << 38,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&bl38);
  gpio_set_level(38, 1);
  ESP_LOGI(TAG, "CrowPanel 3.5/2.4/2.8 backlight GPIO38 HIGH");
#endif
#endif // CONFIG_JC3248W535EN_LCD

#if !defined(CONFIG_USE_7_INCHER) && !defined(CONFIG_JC3248W535EN_LCD)
/* For cardputer (no PSRAM) use a single smaller buffer to save internal RAM.
   Single buffer increases flush frequency but greatly reduces RAM usage. */
  static lv_color_t *buf1 = NULL;
  static lv_color_t *buf2 = NULL;
  size_t buf1_pixels;
  size_t buf2_pixels = 0;

  /* Determine display resolution before sizing the buffers so the allocation
     and LVGL registration always use the same pixel count. */
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV) || defined(CONFIG_IS_ATOMS3R)
  int width = get_m5gfx_width();
  int height = get_m5gfx_height();
#elif defined(CONFIG_USE_TDISPLAY_S3)
  int width = I80_LCD_H_RES;
  int height = I80_LCD_V_RES;
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4)
  int width = CONFIG_TFT_WIDTH;
  int height = CONFIG_TFT_HEIGHT;
#elif defined(CONFIG_CROWPANEL_EPAPER_42)
  int width = CROWPANEL_EPAPER_WIDTH;
  int height = CROWPANEL_EPAPER_HEIGHT;
#else
  int width = CONFIG_TFT_WIDTH;
  int height = CONFIG_TFT_HEIGHT;
#endif

#if defined(CONFIG_IS_ATOMS3R)
  /* Single full-frame buffer. full_refresh=1 needs a screen-sized buffer, but a
     single one is enough and, more importantly, one 32KB internal DMA block
     allocates reliably where two (double-buffered, 64KB) do not once WiFi/LVGL
     have fragmented internal RAM -- the double alloc was failing and bailing out
     before dm.mutex was created, leaving render_view to assert on a NULL queue.
     The full-canvas present path (setAddrWindow 0,0,128,128) is preserved. */
  buf1_pixels = (size_t)width * (size_t)height;
  buf2_pixels = 0;
#elif defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
  buf1_pixels = (size_t)width * 2;
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4)
  buf1_pixels = (size_t)width * (size_t)height;
  buf2_pixels = buf1_pixels;
  esp_err_t fb_err = crowpanel_p4_display_get_frame_buffers((void **)&buf1, (void **)&buf2);
  if (fb_err != ESP_OK) {
    ESP_LOGE(TAG, "display_manager: failed to get CrowPanel frame buffers: %s",
             esp_err_to_name(fb_err));
    return;
  }
#elif defined(CONFIG_CROWPANEL_EPAPER_42)
  /* The factory panel is a full-frame device. Keep one complete RGB565 LVGL
     canvas so the SSD1683 adapter can threshold the rendered UI into its
     15,000-byte MSB-first 1-bit framebuffer. */
  buf1_pixels = (size_t)width * (size_t)height;
  buf2_pixels = 0;
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
   /* Keep the C5 display buffers in DMA-capable internal RAM. PSRAM draw
     buffers force the SPI driver to allocate internal bounce buffers at flush
     time, which is fragile once WiFi/LVGL have fragmented internal RAM.
     Only somethingsomething gets a second buffer: LVGL renders the next
     chunk while the SPI DMA flushes the previous one, hiding render time
     behind the transfer. Other C5 boards stay single-buffered to save
     internal RAM. */
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
  buf1_pixels = (size_t)width * 8;
#else
  buf1_pixels = (size_t)width * 5;
#endif
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
  if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
    buf2_pixels = (size_t)width * 8;
#else
    buf2_pixels = (size_t)width * 5;
#endif
  }
#endif
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
  buf1_pixels = (size_t)width * 5;
#elif defined(CONFIG_IDF_TARGET_ESP32)
  buf1_pixels = (size_t)width * 10;
#else
  buf1_pixels = (size_t)width * 10;
  buf2_pixels = (size_t)width * 10;
#endif
  size_t buf1_bytes = buf1_pixels * sizeof(*buf1);
  size_t buf2_bytes = buf2_pixels * sizeof(*buf2);

  if (!buf1) {
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    return;
#elif defined(CONFIG_IS_ATOMS3R)
    buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf1) ESP_LOGI(TAG, "display_manager: AtomS3R buf1 allocated in internal DMA RAM (%d bytes)", (int)buf1_bytes);
    if (!buf1) {
      /* Once the WiFi AP is up, internal DMA RAM is too fragmented to hand back
         a contiguous full-frame block. ESP32-S3 SPI DMA can source the flush
         straight from PSRAM, so fall back there instead of crash-looping. */
      buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
      if (buf1) ESP_LOGI(TAG, "display_manager: AtomS3R buf1 allocated in PSRAM (%d bytes)", (int)buf1_bytes);
    }
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#if defined(CONFIG_USE_C5_PARLIO_DISPLAY)
    buf1 = banshee_c5_parlio_alloc_draw_buffer(buf1_bytes);
#else
    buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
#endif
    if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in internal DMA RAM (%d bytes)", (int)buf1_bytes);
#elif defined(CONFIG_SPIRAM)
    buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in PSRAM (%d bytes)", (int)buf1_bytes);
#endif
    if (!buf1) {
      buf1 = heap_caps_malloc(buf1_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
      if (buf1) ESP_LOGI(TAG, "display_manager: buf1 allocated in internal DMA RAM (%d bytes)", (int)buf1_bytes);
    }
  }
  if (buf2_pixels > 0 && !buf2) {
#if defined(CONFIG_IS_ATOMS3R)
    buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (buf2) ESP_LOGI(TAG, "display_manager: AtomS3R buf2 allocated in internal DMA RAM (%d bytes)", (int)buf2_bytes);
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
#if defined(CONFIG_USE_C5_PARLIO_DISPLAY)
    buf2 = banshee_c5_parlio_alloc_draw_buffer(buf2_bytes);
#else
    buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
#endif
    if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in internal DMA RAM (%d bytes)", (int)buf2_bytes);
#elif defined(CONFIG_SPIRAM)
    buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in PSRAM (%d bytes)", (int)buf2_bytes);
#endif
    if (!buf2) {
      buf2 = heap_caps_malloc(buf2_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
      if (buf2) ESP_LOGI(TAG, "display_manager: buf2 allocated in internal DMA RAM (%d bytes)", (int)buf2_bytes);
    }
  }

#if defined(CONFIG_IDF_TARGET_ESP32C5)
  if (!buf1) {
    ESP_LOGE(TAG, "display_manager: failed to allocate LVGL draw buffers");
    free(buf2);
    buf2 = NULL;
    return;
  }
  if (!buf2) {
    ESP_LOGW(TAG, "display_manager: buf2 allocation failed, falling back to single buffer");
    buf2_pixels = 0;
  }
#else
  if (!buf1 || (buf2_pixels > 0 && !buf2)) {
    ESP_LOGE(TAG, "display_manager: failed to allocate LVGL draw buffers");
    free(buf1);
    free(buf2);
    buf1 = NULL;
    buf2 = NULL;
    return;
  }
#endif
  ESP_LOGI(TAG, "display_manager: draw buffers allocated, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  static lv_disp_draw_buf_t disp_buf;
  lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf1_pixels);

  /* Initialize the display */
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = width;
  disp_drv.ver_res = height;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
  disp_drv.direct_mode = 1;
#endif
#ifdef CONFIG_CROWPANEL_EPAPER_42
  /* LVGL must render the complete canvas before a single e-paper update. */
  disp_drv.full_refresh = 1;
#endif
#ifdef CONFIG_IS_ATOMS3R
  // Match the reference full-canvas present path and avoid stale GC9107 edge
  // pixels left behind by partial LVGL flush windows.
  disp_drv.full_refresh = 1;
#endif

  disp_drv.flush_cb = invert_flush_cb;
  disp_drv.draw_buf = &disp_buf;
#if CONFIG_GHOSTESP_P4_PPA_RENDER
  if (!lv_draw_ppa_v8_install(&disp_drv)) {
    ESP_LOGW(TAG, "display_manager: PPA backend unavailable; using LVGL software rendering");
  }
#endif
  lv_disp_t *registered_disp = lv_disp_drv_register(&disp_drv);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
  if (registered_disp && registered_disp->driver && registered_disp->driver->draw_ctx &&
      registered_disp->driver->draw_ctx->buffer_copy) {
    s_p4_buffer_copy = registered_disp->driver->draw_ctx->buffer_copy;
    registered_disp->driver->draw_ctx->buffer_copy = p4_buffer_copy;
  }
#endif
  ESP_LOGI(TAG, "display_manager: display driver registered, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#elif defined(CONFIG_JC3248W535EN_LCD)
  esp_err_t ret = lcd_axs15231b_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD initialization failed");
    return;
  }
#else

  esp_err_t ret = lcd_st7262_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LCD initialization failed");
    return;
  }

  ret = lcd_st7262_lvgl_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "LVGL initialization failed");
#ifdef CONFIG_CROWPANEL_ADVANCE_RGB_LCD
    lcd_st7262_deinit();
#endif
    return;
  }

#endif

  dm.mutex = xSemaphoreCreateMutex();
  if (dm.mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create mutex\n");
    return;
  }

  /* LVGL (lv_async_call/lv_timer_create/lv_mem_alloc) is not thread-safe: the
   * tick task walks/mutates the timer list and internal heap in lv_timer_handler()
   * while background tasks (NFC scan progress, IR/WiFi/BLE progress, etc.) call
   * lv_async_call() concurrently with no synchronization, corrupting timer nodes
   * and producing garbage callback pointers (Guru Meditation in lv_async_timer_cb).
   * A single recursive mutex serializes every entry point into LVGL's internal
   * state; recursive so an LVGL event callback that itself calls
   * display_manager_lvgl_async_call() while already inside lv_timer_handler()
   * (same task) can re-enter without deadlocking. */
  s_lvgl_call_mutex = xSemaphoreCreateRecursiveMutex();
  if (s_lvgl_call_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create LVGL call mutex\n");
    return;
  }

  input_queue = xQueueCreate(32, sizeof(InputEvent));
  if (input_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create input queue\n");
    return;
  }
  ESP_LOGI(TAG, "display_manager: mutex and queue created, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

#ifdef CONFIG_USE_CARDPUTER
  keyboard_init(&gkeyboard);
  keyboard_begin(&gkeyboard);
#endif

#ifdef CONFIG_HAS_BATTERY
  axp2101_init();
#endif

#ifdef CONFIG_USE_ENCODER
#ifdef CONFIG_USE_IO_EXPANDER
    // Encoder on IO expander - use virtual pin numbers (P05=5, P06=6, P07=7)
    // These are IO expander pins, not ESP32 GPIOs
    encoder_init(&g_encoder,
                 5,  // P05 = encoder A on IO expander
                 6,  // P06 = encoder B on IO expander  
                 false,  // pullups handled by TCA9535
                 ENCODER_LATCH_FOUR3);
    joystick_init(&enc_button, 7, 500 /*hold ms*/, false); // P07 = encoder button
#else
    // Direct GPIO encoder (TEmbed C1101)
    encoder_init(&g_encoder,
                 CONFIG_ENCODER_INA,
                 CONFIG_ENCODER_INB,
                 true,                    /* pull-ups */
                 ENCODER_LATCH_FOUR3);    /* detented knobs */
    joystick_init(&enc_button, CONFIG_ENCODER_KEY,
                  500 /*hold ms*/, true);

    // Run encoder sampling at 1 kHz for cleaner quadrature decoding. The
    // CrowPanel sampler outranks the touch/input task so an I2C transaction
    // cannot hide quadrature phases during a quick spin.
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
    const UBaseType_t encoder_poll_priority = HARDWARE_INPUT_TASK_PRIORITY + 1;
#else
    const UBaseType_t encoder_poll_priority = HARDWARE_INPUT_TASK_PRIORITY;
#endif
    if (xTaskCreate(encoder_poll_task, "EncPoll", 2048, NULL,
                    encoder_poll_priority, &encoder_poll_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create encoder poll task");
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    // GPIO 6 exit button is TEmbed C1101 only
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        joystick_init(&exit_button, 6, 500 /*hold ms*/, true);
    }
    /* The Banshee's TSC2007 touch is read by hardware_input_task (see
     * enable_touch_polling below), which pushes InputEvents through the
     * same manual queue + view-callback tap/drag pipeline every other
     * touchscreen board uses. This board used to *also* register a real
     * lv_indev_drv_t pointing at the same touch_driver_read() -- LVGL's
     * own indev polling then independently drove native press/click
     * events on lv_obj widgets (e.g. LV_EVENT_CLICKED on grid cards) from
     * the same physical touch, completely uncoordinated with the manual
     * path. A single tap could be decided twice by two systems that don't
     * know about each other, most visibly right at a view transition.
     * Removed; this board now goes through the same single path as
     * everything else. */
#endif
#endif
#endif
// initialize wake button interrupt
#ifdef CONFIG_IS_S3TWATCH
  wake_up_sem = xSemaphoreCreateBinary();
  if (wake_up_sem != NULL) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL<<WAKE_UP_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,  // no GPIO ISR here
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(WAKE_UP_PIN, gpio_isr_handler, (void*)WAKE_UP_PIN);
    // Wake from light-sleep on *low-level*, not edge
    gpio_wakeup_enable(WAKE_UP_PIN, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();  // light-sleep only
  } else {
    ESP_LOGE(TAG, "Failed to create wake_up_sem");
  }
#endif

  display_manager_init_success = true;
  last_touch_time = xTaskGetTickCount();

  // Create global keyboard repeat timer (used by all views for held-key repeat)
  if (!s_kb_repeat_timer) {
      esp_timer_create_args_t args = {
          .callback = kb_repeat_initial_cb,
          .name = "kb_repeat",
      };
      esp_timer_create(&args, &s_kb_repeat_timer);
  }
  is_backlight_dimmed = false;

  // Keep the panel dark until the first real view has been drawn.
  display_manager_set_backlight_raw(0);


#ifndef CONFIG_JC3248W535EN_LCD
    // LVGL refresh must stay on an internal stack on C5; PSRAM stack can starve the idle WDT.
#if (defined(CONFIG_CROWPANEL_ADVANCED_P4) || defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    xTaskCreatePinnedToCore(lvgl_tick_task, "LVGL Tick Task", LVGL_TICK_TASK_STACK_SIZE, NULL,
                            RENDERING_TASK_PRIORITY, &lvgl_task_handle, 1);
#else
#ifdef CONFIG_CROWPANEL_EPAPER_42
    epaper_lvgl_task_stack = heap_caps_malloc(12288 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    epaper_lvgl_task_buffer = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (epaper_lvgl_task_stack && epaper_lvgl_task_buffer) {
      lvgl_task_handle = xTaskCreateStatic(lvgl_tick_task, "LVGL Tick Task", 12288, NULL,
                                           RENDERING_TASK_PRIORITY, epaper_lvgl_task_stack,
                                           epaper_lvgl_task_buffer);
      ESP_LOGI(TAG, "LVGL tick task stack allocated from PSRAM: %d bytes",
               (int)(12288 * sizeof(StackType_t)));
    } else {
      if (epaper_lvgl_task_stack) heap_caps_free(epaper_lvgl_task_stack);
      if (epaper_lvgl_task_buffer) heap_caps_free(epaper_lvgl_task_buffer);
      epaper_lvgl_task_stack = NULL;
      epaper_lvgl_task_buffer = NULL;
      xTaskCreate(lvgl_tick_task, "LVGL Tick Task", LVGL_TICK_TASK_STACK_SIZE, NULL,
                  RENDERING_TASK_PRIORITY, &lvgl_task_handle);
      ESP_LOGW(TAG, "PSRAM LVGL stack allocation failed; using default stack");
    }
#else
    xTaskCreate(lvgl_tick_task, "LVGL Tick Task", LVGL_TICK_TASK_STACK_SIZE, NULL,
                RENDERING_TASK_PRIORITY, &lvgl_task_handle);
#endif
#endif
    ESP_LOGI(TAG, "LVGL tick task stack allocated from internal RAM");
    ESP_LOGI(TAG, "After LVGL task creation, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // Allocate hardware input task stack from PSRAM
    hardware_input_task_stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
    hardware_input_task_buffer = malloc(sizeof(StaticTask_t));
    if (hardware_input_task_stack && hardware_input_task_buffer) {
        hardware_input_task_handle = xTaskCreateStatic(
            hardware_input_task, "RawInput", 4096,
            NULL, HARDWARE_INPUT_TASK_PRIORITY,
            hardware_input_task_stack, hardware_input_task_buffer);
        ESP_LOGI(TAG, "Hardware input task stack allocated from PSRAM: %d bytes", (int)(4096 * sizeof(StackType_t)));
        input_task_handle = hardware_input_task_handle;
    } else {
        ESP_LOGE(TAG, "Failed to allocate hardware input task stack from PSRAM, falling back to internal");
        if (hardware_input_task_stack) free(hardware_input_task_stack);
        if (hardware_input_task_buffer) free(hardware_input_task_buffer);
        hardware_input_task_stack = NULL;
        hardware_input_task_buffer = NULL;
        if (xTaskCreate(hardware_input_task, "RawInput", 4096, NULL,
                        HARDWARE_INPUT_TASK_PRIORITY, &input_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create RawInput task\n");
        }
    }
    ESP_LOGI(TAG, "After RawInput task creation, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif

ESP_LOGI(TAG, "display_manager_init: COMPLETE, free internal RAM: %d bytes", 
         (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

bool display_manager_register_view(View *view) {
  if (view == NULL || view->create == NULL || view->destroy == NULL) {
    return false;
  }
  return true;
}

static lv_obj_t *dm_pressed_obj = NULL;

#if GUI_LARGE_TOUCH_UI
static lv_obj_t *s_p4_home_indicator;
static lv_obj_t *s_p4_back_indicator;
static bool s_p4_home_gesture;
static bool s_p4_back_gesture;
static lv_point_t s_p4_home_start;
static lv_point_t s_p4_back_start;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
static bool s_p4_skip_next_view_animation;
#endif
static bool s_p4_control_center_gesture;
static bool s_p4_control_center_active;
static bool s_p4_control_center_closing;
static lv_point_t s_p4_control_center_start;
static lv_obj_t *s_p4_control_center;
static lv_obj_t *s_p4_control_center_scrim;
static lv_obj_t *s_p4_control_center_brightness;
static lv_obj_t *s_p4_control_center_brightness_fill;
static lv_obj_t *s_p4_control_center_brightness_knob;
static lv_obj_t *s_p4_control_center_wifi;
static lv_obj_t *s_p4_control_center_timeout;
static lv_obj_t *s_p4_control_center_home;
static lv_obj_t *s_p4_control_center_lock;
static uint8_t s_p4_control_center_brightness_value = 100;
static bool s_p4_control_center_brightness_drag;
static lv_point_t s_p4_control_center_touch_start;
static lv_timer_t *s_p4_home_exit_timer;
static View *s_p4_home_exit_view;
static uint8_t s_p4_home_exit_phase;

static bool dm_system_overlay_reduced_motion(void) {
    return settings_get_reduced_motion(&G_Settings) || !GUI_SYSTEM_OVERLAY_ANIMATIONS;
}

static bool dm_p4_point_in_obj(lv_obj_t *obj, lv_point_t point) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return point.x >= area.x1 && point.x <= area.x2 &&
           point.y >= area.y1 && point.y <= area.y2;
}

static void dm_p4_control_center_delete(void) {
    if (s_p4_control_center && lv_obj_is_valid(s_p4_control_center)) {
        lv_obj_del(s_p4_control_center);
    }
    if (s_p4_control_center_scrim && lv_obj_is_valid(s_p4_control_center_scrim)) {
        lv_obj_del(s_p4_control_center_scrim);
    }
    s_p4_control_center = NULL;
    s_p4_control_center_scrim = NULL;
    s_p4_control_center_brightness = NULL;
    s_p4_control_center_brightness_fill = NULL;
    s_p4_control_center_brightness_knob = NULL;
    s_p4_control_center_brightness_drag = false;
    s_p4_control_center_wifi = NULL;
    s_p4_control_center_timeout = NULL;
    s_p4_control_center_home = NULL;
    s_p4_control_center_lock = NULL;
    s_p4_control_center_active = false;
    s_p4_control_center_closing = false;
}

static void dm_p4_control_center_set_y(void *obj, int32_t value) {
    if (obj && lv_obj_is_valid((lv_obj_t *)obj)) {
        lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)value);
    }
}

static void dm_p4_control_center_set_scrim_opa(void *obj, int32_t value) {
    if (obj && lv_obj_is_valid((lv_obj_t *)obj)) {
        lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
    }
}

static void dm_p4_control_center_close_ready(lv_anim_t *anim) {
    (void)anim;
    dm_p4_control_center_delete();
}

static void dm_p4_control_center_close(lv_event_t *event) {
    (void)event;
    if (!s_p4_control_center_active || s_p4_control_center_closing) return;
    if (!s_p4_control_center || !lv_obj_is_valid(s_p4_control_center) ||
        dm_system_overlay_reduced_motion()) {
        dm_p4_control_center_delete();
        return;
    }

    s_p4_control_center_closing = true;
    lv_obj_clear_flag(s_p4_control_center, LV_OBJ_FLAG_CLICKABLE);
    if (s_p4_control_center_scrim && lv_obj_is_valid(s_p4_control_center_scrim)) {
        lv_obj_clear_flag(s_p4_control_center_scrim, LV_OBJ_FLAG_CLICKABLE);
        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, s_p4_control_center_scrim);
        lv_anim_set_values(&fade, lv_obj_get_style_bg_opa(s_p4_control_center_scrim, 0),
                           LV_OPA_TRANSP);
        lv_anim_set_time(&fade, 180);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&fade, dm_p4_control_center_set_scrim_opa);
        lv_anim_start(&fade);
    }

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, s_p4_control_center);
    lv_anim_set_values(&slide, lv_obj_get_y(s_p4_control_center),
                       -lv_obj_get_height(s_p4_control_center) - 12);
    lv_anim_set_time(&slide, 240);
    lv_anim_set_path_cb(&slide, lv_anim_path_ease_in);
    lv_anim_set_exec_cb(&slide, dm_p4_control_center_set_y);
    lv_anim_set_ready_cb(&slide, dm_p4_control_center_close_ready);
    lv_anim_start(&slide);
}

static void dm_p4_control_center_set_brightness_visual(int32_t value) {
    if (!s_p4_control_center_brightness ||
        !lv_obj_is_valid(s_p4_control_center_brightness)) return;
    if (value < 1) value = 1;
    if (value > 100) value = 100;

    lv_coord_t track_w = lv_obj_get_width(s_p4_control_center_brightness);
    lv_coord_t knob_w = s_p4_control_center_brightness_knob &&
                        lv_obj_is_valid(s_p4_control_center_brightness_knob)
                            ? lv_obj_get_width(s_p4_control_center_brightness_knob) : 18;
    lv_coord_t travel = LV_MAX(0, track_w - knob_w);
    lv_coord_t knob_x = (lv_coord_t)(((int32_t)travel * value) / 100);
    if (s_p4_control_center_brightness_fill &&
        lv_obj_is_valid(s_p4_control_center_brightness_fill)) {
        lv_obj_set_width(s_p4_control_center_brightness_fill, knob_x + knob_w / 2);
    }
    if (s_p4_control_center_brightness_knob &&
        lv_obj_is_valid(s_p4_control_center_brightness_knob)) {
        lv_obj_set_x(s_p4_control_center_brightness_knob, knob_x);
    }
}

static void dm_p4_control_center_apply_brightness(int32_t value) {
    /* Keep one real PWM step at the bottom: zero is reserved by the display
     * manager for its sleep/backlight-off path. */
    if (value < 1) value = 1;
    if (value > 100) value = 100;
    dm_p4_control_center_set_brightness_visual(value);
    s_p4_control_center_brightness_value = (uint8_t)value;
    settings_set_max_screen_brightness(&G_Settings, (uint8_t)value);
#if defined(CONFIG_CROWPANEL_P4_PANEL_MIPI_1024X600) && defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
    /* The P4 MIPI panel's backlight is a plain LEDC input.  Do not route this
     * quick-control value through set_backlight_brightness(): that function
     * intentionally applies the global maximum again for timeout/wake paths,
     * which makes the slider's duty value ambiguous. */
    uint32_t duty = ((uint32_t)value * ((1U << LEDC_TIMER_10_BIT) - 1U)) / 100U;
#if !defined(CONFIG_LV_BACKLIGHT_ACTIVE_LVL)
    duty = ((1U << LEDC_TIMER_10_BIT) - 1U) - duty;
#endif
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    is_backlight_dimmed = false;
    is_backlight_off = false;
#else
    set_backlight_brightness(100);
#endif
}

static int32_t dm_p4_control_center_brightness_at(lv_point_t point) {
    if (!s_p4_control_center_brightness ||
        !lv_obj_is_valid(s_p4_control_center_brightness)) return 100;
    lv_area_t area;
    lv_obj_get_coords(s_p4_control_center_brightness, &area);
    int32_t width = area.x2 - area.x1;
    if (width <= 0) return 100;
    int32_t x = point.x - area.x1;
    if (x < 0) x = 0;
    if (x > width) x = width;
    return 1 + (x * 99) / width;
}

static void dm_p4_control_center_update_wifi(void) {
    if (!s_p4_control_center_wifi || !lv_obj_is_valid(s_p4_control_center_wifi)) return;
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    bool enabled = settings_get_ap_enabled(&G_Settings);
    lv_obj_t *title = lv_obj_get_child(s_p4_control_center_wifi, 0);
    lv_obj_t *subtitle = lv_obj_get_child(s_p4_control_center_wifi, 1);
    lv_obj_set_style_bg_color(
        s_p4_control_center_wifi,
        lv_color_hex(enabled ? theme_palette_get_accent(theme) :
                               theme_palette_get_surface_alt(theme)), 0);
    if (title) {
        lv_obj_set_style_text_color(
            title, lv_color_hex(enabled ? theme_palette_get_on_accent(theme) :
                                          theme_palette_get_text(theme)), 0);
    }
    if (subtitle) {
        lv_label_set_text(subtitle, enabled ? "GhostNet AP: On" : "GhostNet AP: Off");
        lv_obj_set_style_text_color(
            subtitle, lv_color_hex(enabled ? theme_palette_get_on_accent(theme) :
                                             theme_palette_get_text_muted(theme)), 0);
        lv_obj_set_style_text_opa(subtitle, enabled ? LV_OPA_80 : LV_OPA_COVER, 0);
    }
}

static void dm_p4_control_center_wifi_cb(lv_event_t *event) {
    (void)event;
    bool enabled = !settings_get_ap_enabled(&G_Settings);
    settings_set_ap_enabled(&G_Settings, enabled);
    settings_save(&G_Settings);
    if (enabled) {
        (void)ap_manager_restore_after_attack("control center AP enable");
    } else {
        ap_manager_stop_services();
    }
    dm_p4_control_center_update_wifi();
}

static const uint32_t s_p4_control_center_timeout_values[] = {
    15000, 30000, 60000, 120000, 300000, 0
};

static void dm_p4_control_center_update_timeout(void) {
    if (!s_p4_control_center_timeout ||
        !lv_obj_is_valid(s_p4_control_center_timeout)) return;

    uint32_t timeout = settings_get_display_timeout(&G_Settings);
    const char *text = "Never";
    if (timeout != UINT32_MAX) {
        if (timeout < 60000) {
            static char seconds[16];
            snprintf(seconds, sizeof(seconds), "%lus", (unsigned long)(timeout / 1000));
            text = seconds;
        } else {
            static char minutes[16];
            unsigned long mins = (unsigned long)(timeout / 60000);
            snprintf(minutes, sizeof(minutes), "%lum", mins);
            text = minutes;
        }
    }
    lv_obj_t *subtitle = lv_obj_get_child(s_p4_control_center_timeout, 1);
    if (subtitle) lv_label_set_text(subtitle, text);
}

static void dm_p4_control_center_timeout_cb(lv_event_t *event) {
    (void)event;
    uint32_t current = settings_get_display_timeout(&G_Settings);
    size_t next = 0;
    for (size_t i = 0; i < sizeof(s_p4_control_center_timeout_values) /
                         sizeof(s_p4_control_center_timeout_values[0]); i++) {
        uint32_t stored = s_p4_control_center_timeout_values[i] == 0
                              ? UINT32_MAX : s_p4_control_center_timeout_values[i];
        if (stored == current) {
            next = (i + 1) % (sizeof(s_p4_control_center_timeout_values) /
                              sizeof(s_p4_control_center_timeout_values[0]));
            break;
        }
    }
    settings_set_display_timeout(&G_Settings, s_p4_control_center_timeout_values[next]);
    settings_save(&G_Settings);
    dm_p4_control_center_update_timeout();
}

static lv_obj_t *dm_p4_control_center_button(lv_obj_t *parent, const char *title,
                                              const char *subtitle, lv_coord_t x,
                                              lv_coord_t y, lv_coord_t w,
                                              lv_event_cb_t callback) {
    lv_obj_t *button = lv_btn_create(parent);
    if (!button) return NULL;
    gui_apply_pressed_style(button);
    lv_obj_set_size(button, w, 74);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(theme_palette_get_surface_alt(
                                  settings_get_menu_theme(&G_Settings))), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(button, GUI_RADIUS_MD, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_pad_left(button, 0, 0);
    lv_obj_set_style_pad_row(button, 5, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    if (callback) lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, title);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, gui_font_body(), 0);
    lv_obj_set_style_text_color(label,
                                lv_color_hex(theme_palette_get_text(
                                    settings_get_menu_theme(&G_Settings))), 0);
    if (subtitle) {
        lv_obj_t *sub = lv_label_create(button);
        lv_label_set_text(sub, subtitle);
        lv_obj_set_width(sub, LV_PCT(100));
        lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(sub, gui_font_micro(), 0);
        lv_obj_set_style_text_color(sub,
                                    lv_color_hex(theme_palette_get_text_muted(
                                        settings_get_menu_theme(&G_Settings))), 0);
    }
    return button;
}

static void dm_p4_go_home_through_view(void);

static void dm_p4_control_center_home_cb(lv_event_t *event) {
    (void)event;
    dm_p4_control_center_close(NULL);
    dm_p4_go_home_through_view();
}

static void dm_p4_finish_home_route(void) {
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    s_p4_skip_next_view_animation = true;
#endif
    gui_route_t home = {.id = GUI_ROUTE_VIEW, .view = &main_menu_view};
    gui_router_reset(&home);
}

static void dm_p4_home_exit_timer_cb(lv_timer_t *timer) {
    if (!timer || timer != s_p4_home_exit_timer) return;

    /* The view completed its normal asynchronous back transition. Finish the
     * Home gesture only after that cleanup path has run. */
    if (dm.current_view != s_p4_home_exit_view) {
        ESP_LOGI(TAG, "System Home: normal exit completed; finishing Home route");
        lv_timer_del(timer);
        s_p4_home_exit_timer = NULL;
        s_p4_home_exit_view = NULL;
        dm_p4_finish_home_route();
        return;
    }

    /* Native apps can still be finishing a tick or engine bring-up. Forcing
     * destruction after 280ms would join that worker on the UI task again. */
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (s_p4_home_exit_view == &plugin_runner_view && plugin_runner_home_exit_pending()) return;
#endif

    if (s_p4_home_exit_phase == 0) {
        /* A few legacy views do not implement INPUT_TYPE_EXIT_BUTTON on this
         * board. Give them their keyboard back event as a delayed fallback. */
        void (*input_callback)(InputEvent *) = s_p4_home_exit_view->input_callback;
        if (input_callback) {
            ESP_LOGW(TAG, "System Home: exit event did not navigate; trying Escape fallback");
            InputEvent back = {.type = INPUT_TYPE_KEYBOARD};
            back.data.key_value = LV_KEY_ESC;
            input_callback(&back);
        }
        s_p4_home_exit_phase = 1;
        lv_timer_set_period(timer, 140);
        return;
    }

    /* A view with no back implementation must not trap the system gesture. */
    lv_timer_del(timer);
    s_p4_home_exit_timer = NULL;
    s_p4_home_exit_view = NULL;
    ESP_LOGW(TAG, "System Home: view did not handle exit; forcing Home route");
    dm_p4_finish_home_route();
}

static void dm_p4_go_home_through_view(void) {
    View *leaving = dm.current_view;
    if (!leaving || leaving == &splash_view || leaving == &main_menu_view) return;

#if defined(CONFIG_IDF_TARGET_ESP32P4)
    if (leaving == &plugin_runner_view) {
        ESP_LOGI(TAG, "P4 Home: waiting for native app tick before normal exit");
        if (!plugin_runner_request_home_exit()) return;
        if (s_p4_home_exit_timer) lv_timer_del(s_p4_home_exit_timer);
        s_p4_home_exit_view = leaving;
        s_p4_home_exit_phase = 0;
        s_p4_home_exit_timer = lv_timer_create(dm_p4_home_exit_timer_cb, 140, NULL);
        return;
    }
#endif

    /* Use the same event as the physical exit button. Scan/attack views stop
     * workers and restore their radio profile before navigating away. */
    void (*input_callback)(InputEvent *) = leaving->input_callback;
    if (!input_callback && leaving->get_hardwareinput_callback) {
        leaving->get_hardwareinput_callback((void **)&input_callback);
    }
    if (input_callback) {
        ESP_LOGI(TAG, "System Home: dispatching physical exit event to %s",
                 leaving->name ? leaving->name : "unnamed view");
        InputEvent back = {.type = INPUT_TYPE_EXIT_BUTTON};
        back.data.exit_pressed = true;
        input_callback(&back);
        if (s_p4_home_exit_timer) lv_timer_del(s_p4_home_exit_timer);
        s_p4_home_exit_view = leaving;
        s_p4_home_exit_phase = 0;
        s_p4_home_exit_timer = lv_timer_create(dm_p4_home_exit_timer_cb, 140, NULL);
    } else {
        dm_p4_finish_home_route();
    }
}

static void dm_p4_control_center_lock_cb(lv_event_t *event) {
    (void)event;
    dm_p4_control_center_close(NULL);
    display_manager_show_lockscreen();
}

static void dm_p4_control_center_create(void) {
    if (s_p4_control_center_active || LV_HOR_RES < 600 || LV_VER_RES < 400) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t bg = lv_color_hex(theme_palette_get_background(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    s_p4_control_center_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_p4_control_center_scrim);
    lv_obj_set_size(s_p4_control_center_scrim, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(s_p4_control_center_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_p4_control_center_scrim, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_p4_control_center_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_p4_control_center_scrim, dm_p4_control_center_close,
                        LV_EVENT_CLICKED, NULL);

    lv_coord_t panel_w = LV_HOR_RES - 32;
    if (panel_w > 760) panel_w = 760;
    lv_coord_t panel_h = 344;
    if (panel_h > LV_VER_RES - 24) panel_h = LV_VER_RES - 24;
    s_p4_control_center = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_p4_control_center);
    lv_obj_set_size(s_p4_control_center, panel_w, panel_h);
    lv_obj_align(s_p4_control_center, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_color(s_p4_control_center, bg, 0);
    lv_obj_set_style_bg_opa(s_p4_control_center, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_p4_control_center, GUI_RADIUS_LG, 0);
    lv_obj_set_style_border_color(s_p4_control_center, accent, 0);
    lv_obj_set_style_border_opa(s_p4_control_center, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_p4_control_center, 1, 0);
    lv_obj_set_style_pad_all(s_p4_control_center, 0, 0);
    lv_obj_clear_flag(s_p4_control_center, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *handle = lv_obj_create(s_p4_control_center);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 64, 5);
    lv_obj_align(handle, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(handle,
                              lv_color_hex(theme_palette_get_text_muted(theme)), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_70, 0);
    lv_obj_set_style_radius(handle, LV_RADIUS_CIRCLE, 0);

    lv_coord_t card_gap = 10;
    lv_coord_t card_w = (panel_w - 48 - card_gap * 3) / 4;
    s_p4_control_center_wifi = dm_p4_control_center_button(
        s_p4_control_center, "Wi-Fi", "GhostNet AP", 24, 28, card_w,
        dm_p4_control_center_wifi_cb);
    dm_p4_control_center_update_wifi();
    dm_p4_control_center_button(s_p4_control_center, "BLE",
#if defined(CONFIG_IDF_TARGET_ESP32P4)
                                "C6 hosted",
#else
                                "ESP32-S3",
#endif
                                24 + card_w + card_gap, 28, card_w, NULL);
    dm_p4_control_center_button(s_p4_control_center, "Storage", "SD card",
                                24 + (card_w + card_gap) * 2, 28, card_w, NULL);
    s_p4_control_center_timeout = dm_p4_control_center_button(
        s_p4_control_center, "Timeout", "Screen", 24 + (card_w + card_gap) * 3,
        28, card_w, dm_p4_control_center_timeout_cb);
    dm_p4_control_center_update_timeout();

    lv_obj_t *brightness_label = lv_label_create(s_p4_control_center);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_font(brightness_label, gui_font_body(), 0);
    lv_obj_set_style_text_color(brightness_label, text, 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 24, 142);

    s_p4_control_center_brightness = lv_obj_create(s_p4_control_center);
    lv_obj_remove_style_all(s_p4_control_center_brightness);
    lv_obj_set_width(s_p4_control_center_brightness, panel_w - 208);
    lv_obj_set_height(s_p4_control_center_brightness, 18);
    lv_obj_align(s_p4_control_center_brightness, LV_ALIGN_TOP_LEFT, 160, 146);
    lv_obj_set_style_bg_color(s_p4_control_center_brightness,
                              lv_color_hex(theme_palette_get_surface_alt(theme)), 0);
    lv_obj_set_style_bg_opa(s_p4_control_center_brightness, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_p4_control_center_brightness, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_p4_control_center_brightness,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_p4_control_center_brightness_fill = lv_obj_create(s_p4_control_center_brightness);
    lv_obj_remove_style_all(s_p4_control_center_brightness_fill);
    lv_obj_set_height(s_p4_control_center_brightness_fill, 18);
    lv_obj_set_style_bg_color(s_p4_control_center_brightness_fill, accent, 0);
    lv_obj_set_style_bg_opa(s_p4_control_center_brightness_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_p4_control_center_brightness_fill, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(s_p4_control_center_brightness_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(s_p4_control_center_brightness_fill,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_p4_control_center_brightness_knob = lv_obj_create(s_p4_control_center_brightness);
    lv_obj_remove_style_all(s_p4_control_center_brightness_knob);
    lv_obj_set_size(s_p4_control_center_brightness_knob, 18, 18);
    lv_obj_set_style_bg_color(s_p4_control_center_brightness_knob, accent, 0);
    lv_obj_set_style_bg_opa(s_p4_control_center_brightness_knob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_p4_control_center_brightness_knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_p4_control_center_brightness_knob,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_p4_control_center_brightness_value = settings_get_max_screen_brightness(&G_Settings);
    dm_p4_control_center_set_brightness_visual(s_p4_control_center_brightness_value);

    lv_coord_t action_w = (panel_w - 60) / 2;
    s_p4_control_center_home = dm_p4_control_center_button(
        s_p4_control_center, "Home", "Return to apps", 24, 216,
        action_w, dm_p4_control_center_home_cb);
    s_p4_control_center_lock = dm_p4_control_center_button(
        s_p4_control_center, "Lock", "Lock screen", 36 + action_w, 216,
        action_w, dm_p4_control_center_lock_cb);
    s_p4_control_center_active = true;
    s_p4_control_center_closing = false;

    if (dm_system_overlay_reduced_motion()) {
        lv_obj_set_style_bg_opa(s_p4_control_center_scrim, LV_OPA_40, 0);
    } else {
        lv_coord_t resting_y = 8;
        lv_obj_set_y(s_p4_control_center, -panel_h - 12);

        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, s_p4_control_center);
        lv_anim_set_values(&slide, -panel_h - 12, resting_y);
        lv_anim_set_time(&slide, 300);
        lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&slide, dm_p4_control_center_set_y);
        lv_anim_start(&slide);

        lv_anim_t fade;
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, s_p4_control_center_scrim);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_40);
        lv_anim_set_time(&fade, 220);
        lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&fade, dm_p4_control_center_set_scrim_opa);
        lv_anim_start(&fade);
    }
}

static void dm_p4_control_center_open_cb(void *arg) {
    (void)arg;
    dm_p4_control_center_create();
}

static bool dm_p4_control_center_handle_input(const InputEvent *event) {
    if (!event || event->type != INPUT_TYPE_TOUCH) return false;
    if (s_p4_control_center_active) {
        if (s_p4_control_center_closing) return true;
        lv_point_t point = event->data.touch_data.point;
        if (event->data.touch_data.state == LV_INDEV_STATE_PR && !event->is_touch_move) {
            s_p4_control_center_touch_start = point;
        }
        if (event->data.touch_data.state == LV_INDEV_STATE_PR &&
            (s_p4_control_center_brightness_drag ||
             dm_p4_point_in_obj(s_p4_control_center_brightness, point))) {
            int value = dm_p4_control_center_brightness_at(point);
            s_p4_control_center_brightness_drag = true;
            dm_p4_control_center_apply_brightness(value);
        } else if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            bool swipe_closed = (s_p4_control_center_touch_start.y - point.y) >= 70 &&
                                abs(point.x - s_p4_control_center_touch_start.x) <= 300;
            if (s_p4_control_center_brightness_drag) {
                int value = dm_p4_control_center_brightness_at(point);
                dm_p4_control_center_apply_brightness(value);
                s_p4_control_center_brightness_drag = false;
            } else if (swipe_closed) {
                dm_p4_control_center_close(NULL);
            } else if (dm_p4_point_in_obj(s_p4_control_center_wifi, point)) {
                dm_p4_control_center_wifi_cb(NULL);
            } else if (dm_p4_point_in_obj(s_p4_control_center_timeout, point)) {
                dm_p4_control_center_timeout_cb(NULL);
            } else
            if (dm_p4_point_in_obj(s_p4_control_center_home, point)) {
                dm_p4_control_center_home_cb(NULL);
            } else if (dm_p4_point_in_obj(s_p4_control_center_lock, point)) {
                dm_p4_control_center_lock_cb(NULL);
            } else if (dm_p4_point_in_obj(s_p4_control_center_brightness, point)) {
                int value = dm_p4_control_center_brightness_at(point);
                dm_p4_control_center_apply_brightness(value);
            } else if (!dm_p4_point_in_obj(s_p4_control_center, point)) {
                dm_p4_control_center_close(NULL);
            }
        }
        return true;
    }
    if (LV_HOR_RES < 600 || LV_VER_RES < 400) return false;

    /* A raw top-edge pull has already captured this touch.  Some views also
     * forward held samples; consume them without replacing the original
     * top-edge start coordinate. */
    if (event->is_touch_move && s_p4_control_center_gesture) return true;

    if (event->data.touch_data.state == LV_INDEV_STATE_PRESSED) {
        s_p4_control_center_start = event->data.touch_data.point;
        /* Only a true top-edge pull opens Control Center. Content and option
         * rows directly below the status bar must receive their own touch. */
        s_p4_control_center_gesture = s_p4_control_center_start.y <= 24;
        if (s_p4_control_center_gesture) {
            ESP_LOGD(TAG, "Control Center gesture start: x=%d y=%d",
                     (int)s_p4_control_center_start.x,
                     (int)s_p4_control_center_start.y);
        }
        return s_p4_control_center_gesture;
    }
    if (s_p4_control_center_gesture) {
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            lv_point_t end = event->data.touch_data.point;
            bool opened = (end.y - s_p4_control_center_start.y) >= 80 &&
                          abs(end.x - s_p4_control_center_start.x) <= 260;
            s_p4_control_center_gesture = false;
            if (opened && !s_lockscreen_overlay_active) {
                ESP_LOGI(TAG, "Opening Control Center");
                dm_p4_control_center_create();
            }
        }
        return true;
    }
    return false;
}

#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
static void dm_p4_animate_view_in(lv_obj_t *root) {
    if (!root || settings_get_reduced_motion(&G_Settings)) return;
    if (s_p4_skip_next_view_animation) {
        s_p4_skip_next_view_animation = false;
        return;
    }
    lv_obj_set_x(root, 36);
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, root);
    lv_anim_set_values(&anim, 36, 0);
    lv_anim_set_time(&anim, 180);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&anim, slide_set_x);
    lv_anim_start(&anim);
}
#endif

static void dm_p4_ensure_home_indicator(void) {
    if (!s_p4_home_indicator || !lv_obj_is_valid(s_p4_home_indicator)) {
        s_p4_home_indicator = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_p4_home_indicator);
        lv_obj_set_size(s_p4_home_indicator, 112, 5);
        lv_obj_set_style_radius(s_p4_home_indicator, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_p4_home_indicator,
                                  lv_color_hex(theme_palette_get_text_muted(
                                      settings_get_menu_theme(&G_Settings))), 0);
        lv_obj_set_style_bg_opa(s_p4_home_indicator, LV_OPA_70, 0);
        lv_obj_clear_flag(s_p4_home_indicator, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_align(s_p4_home_indicator, LV_ALIGN_BOTTOM_MID, 0, -7);
    lv_obj_move_foreground(s_p4_home_indicator);
}

static void dm_p4_back_indicator_set_width(void *obj, int32_t value) {
    if (obj && lv_obj_is_valid((lv_obj_t *)obj)) {
        lv_obj_set_width((lv_obj_t *)obj, value);
    }
}

static void dm_p4_back_indicator_set_opa(void *obj, int32_t value) {
    if (obj && lv_obj_is_valid((lv_obj_t *)obj)) {
        lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
    }
}

static void dm_p4_back_indicator_delete(lv_anim_t *anim) {
    lv_obj_t *indicator = anim ? (lv_obj_t *)anim->var : NULL;
    if (indicator && lv_obj_is_valid(indicator)) {
        lv_obj_del(indicator);
    }
    if (s_p4_back_indicator == indicator) {
        s_p4_back_indicator = NULL;
    }
}

static void dm_p4_back_indicator_cancel(void) {
    if (s_p4_back_indicator && lv_obj_is_valid(s_p4_back_indicator)) {
        lv_anim_del(s_p4_back_indicator, NULL);
        lv_obj_del(s_p4_back_indicator);
    }
    s_p4_back_indicator = NULL;
}

static void dm_p4_back_indicator_begin(lv_point_t start) {
    dm_p4_back_indicator_cancel();
    s_p4_back_indicator = lv_obj_create(lv_layer_top());
    if (!s_p4_back_indicator) return;
    lv_obj_remove_style_all(s_p4_back_indicator);
    lv_obj_set_size(s_p4_back_indicator, 8, 72);
    lv_obj_set_pos(s_p4_back_indicator, 0, start.y - 36);
    lv_obj_set_style_radius(s_p4_back_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_p4_back_indicator,
                              lv_color_hex(theme_palette_get_accent(
                                  settings_get_menu_theme(&G_Settings))), 0);
    lv_obj_set_style_bg_opa(s_p4_back_indicator, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_p4_back_indicator,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_p4_back_indicator);

    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, s_p4_back_indicator);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_70);
    lv_anim_set_time(&fade, 120);
    lv_anim_set_exec_cb(&fade, dm_p4_back_indicator_set_opa);
    lv_anim_start(&fade);
}

static void dm_p4_back_indicator_commit(void) {
    if (!s_p4_back_indicator || !lv_obj_is_valid(s_p4_back_indicator)) return;
    lv_anim_del(s_p4_back_indicator, NULL);

    lv_anim_t width;
    lv_anim_init(&width);
    lv_anim_set_var(&width, s_p4_back_indicator);
    lv_anim_set_values(&width, lv_obj_get_width(s_p4_back_indicator), 144);
    lv_anim_set_time(&width, 160);
    lv_anim_set_path_cb(&width, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&width, dm_p4_back_indicator_set_width);
    lv_anim_set_ready_cb(&width, dm_p4_back_indicator_delete);
    lv_anim_start(&width);
}
#endif

#if !GUI_LARGE_TOUCH_UI
static bool dm_p4_control_center_handle_input(const InputEvent *event) {
    (void)event;
    return false;
}
#endif

static void dm_clear_pressed_state(lv_obj_t *obj) {
    (void)obj;
    if (dm_pressed_obj) {
        if (lv_obj_is_valid(dm_pressed_obj)) {
            lv_obj_clear_state(dm_pressed_obj, LV_STATE_PRESSED);
        }
        dm_pressed_obj = NULL;
    }
}

void display_manager_render_view(View *view) {
  if (view == NULL) return;
  // A failed LCD/LVGL allocation must not let startup navigate into a NULL
  // mutex and assert inside xQueueSemaphoreTake().
  if (!display_manager_init_success || dm.mutex == NULL) {
    ESP_LOGE(TAG, "Cannot render view: display manager is not initialized");
    return;
  }
#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_lock(0);
#endif
  if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
    ESP_LOGD(TAG, "Switching view from %s to %s", dm.current_view ? dm.current_view->name : "NULL", view->name);
    if (view == &lockscreen_view) {
      display_manager_run_freeze_pre_lock();
    }
    if (dm.current_view && dm.current_view->root) {
      dm_clear_pressed_state(dm.current_view->root);
    }
    display_manager_cancel_pending_scroll();
    if (dm.current_view && dm.current_view->root) {
      if (dm.current_view->destroy) {
        dm.current_view->destroy();
      }
      dm.current_view = view;
      if (view->get_hardwareinput_callback) {
        view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
      }
      view->create();
      lv_obj_set_style_opa(view->root, LV_OPA_COVER, 0);
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    } else {
      dm.current_view = view;
      if (view->get_hardwareinput_callback) {
        view->get_hardwareinput_callback((void **)&dm.current_view->input_callback);
      }
      view->create();
      lv_obj_set_style_opa(view->root, LV_OPA_COVER, 0);
      if (status_bar) lv_obj_set_style_opa(status_bar, LV_OPA_COVER, 0);
    }
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    dm_p4_animate_view_in(view->root);
#endif
#if GUI_LARGE_TOUCH_UI
    dm_p4_ensure_home_indicator();
#endif
    xSemaphoreGive(dm.mutex);
  } else {
    ESP_LOGE(TAG, "Failed to acquire mutex for switching view\n");
  }
#ifdef CONFIG_JC3248W535EN_LCD
  bsp_display_unlock();
#endif
}

static void dm_switch_async_cb(void *param) {
  gui_router_legacy_switch_immediate((View *)param);
}

typedef struct {
  View *view;
  SemaphoreHandle_t done;
} dm_switch_wait_t;

static void dm_switch_wait_async_cb(void *param) {
  dm_switch_wait_t *call = (dm_switch_wait_t *)param;
  if (!call) return;

  gui_route_t route = {.id = GUI_ROUTE_VIEW, .view = call->view};
  gui_router_navigate_immediate(&route);
  lv_timer_handler();
  lv_refr_now(NULL);
  xSemaphoreGive(call->done);
  free(call);
}

static lv_res_t display_manager_lvgl_async_call_with_timeout(lv_async_cb_t cb,
                                                              void *user_data,
                                                              TickType_t timeout) {
    if (!cb) return LV_RES_INV;
    if (!s_lvgl_call_mutex) {
        /* Called before display_manager_init() finished creating the mutex; there
     * is no concurrent lv_timer_handler() running yet, so this is safe. */
    return lv_async_call(cb, user_data);
  }
  /* lv_timer_handler() can hold this mutex through an entire frame flush, which
   * may run well past MUTEX_TIMEOUT_MS (tuned for the unrelated, short dm-state
   * critical sections elsewhere in this file), so give enqueueing callers more
   * room before dropping the call. */
    if (xSemaphoreTakeRecursive(s_lvgl_call_mutex, timeout) != pdTRUE) {
        if (timeout != 0) {
            ESP_LOGW(TAG, "display_manager_lvgl_async_call: timed out waiting for LVGL call mutex");
        }
        return LV_RES_INV;
    }
  lv_res_t res = lv_async_call(cb, user_data);
    xSemaphoreGiveRecursive(s_lvgl_call_mutex);
    return res;
}

lv_res_t display_manager_lvgl_async_call(lv_async_cb_t cb, void *user_data) {
    return display_manager_lvgl_async_call_with_timeout(cb, user_data, pdMS_TO_TICKS(100));
}

lv_res_t display_manager_lvgl_async_call_nowait(lv_async_cb_t cb, void *user_data) {
    return display_manager_lvgl_async_call_with_timeout(cb, user_data, 0);
}

typedef struct {
  void (*fn)(void *);
  void *arg;
} dm_lvgl_call_t;

static void dm_run_on_lvgl_async_cb(void *param) {
  dm_lvgl_call_t *call = (dm_lvgl_call_t *)param;
  if (!call) return;
  if (call->fn) call->fn(call->arg);
  free(call);
}

bool display_manager_is_lvgl_task(void) {
  return !lvgl_task_handle || xTaskGetCurrentTaskHandle() == lvgl_task_handle;
}

static bool display_manager_run_on_lvgl_common(void (*fn)(void *), void *arg,
                                                bool wait_for_lvgl) {
    if (!fn) return false;
    if (!display_manager_is_lvgl_task()) {
        dm_lvgl_call_t *call = malloc(sizeof(*call));
        if (!call) return false;
        call->fn = fn;
        call->arg = arg;
        lv_res_t result = wait_for_lvgl
                              ? display_manager_lvgl_async_call(dm_run_on_lvgl_async_cb, call)
                              : display_manager_lvgl_async_call_nowait(dm_run_on_lvgl_async_cb, call);
        if (result != LV_RES_OK) {
            free(call);
            return false;
        }
    return true;
  }
    fn(arg);
    return true;
}

bool display_manager_run_on_lvgl(void (*fn)(void *), void *arg) {
    return display_manager_run_on_lvgl_common(fn, arg, true);
}

bool display_manager_run_on_lvgl_nowait(void (*fn)(void *), void *arg) {
    return display_manager_run_on_lvgl_common(fn, arg, false);
}

void display_manager_switch_view(View *view) {
  if (view == NULL) return;
  dm_lvgl_call_t *call = malloc(sizeof(*call));
  if (!call) return;
  call->fn = dm_switch_async_cb;
  call->arg = view;
  display_manager_lvgl_async_call(dm_run_on_lvgl_async_cb, call);
}

bool display_manager_switch_view_and_wait_for_refresh(View *view) {
  if (view == NULL) return false;

  if (!lvgl_task_handle || xTaskGetCurrentTaskHandle() == lvgl_task_handle) {
    gui_route_t route = {.id = GUI_ROUTE_VIEW, .view = view};
    gui_router_navigate_immediate(&route);
    lv_timer_handler();
    lv_refr_now(NULL);
    return true;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (!done) {
    display_manager_switch_view(view);
    return false;
  }

  dm_switch_wait_t *call = malloc(sizeof(*call));
  if (!call) {
    vSemaphoreDelete(done);
    display_manager_switch_view(view);
    return false;
  }
  call->view = view;
  call->done = done;

  display_manager_lvgl_async_call(dm_switch_wait_async_cb, call);
  if (xSemaphoreTake(done, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGW(TAG, "Timed out waiting for first view refresh");
    return false;
  }
  vSemaphoreDelete(done);
  return true;
}

void display_manager_go_back(void) {
  gui_router_back();
}

void display_manager_init_deferred_peripherals(void) {
  if (s_deferred_peripherals_initialized) return;
  s_deferred_peripherals_initialized = true;

#ifdef CONFIG_HAS_RTC_CLOCK
  rtc_chip_type_t chip_type = (rtc_chip_type_t)CONFIG_RTC_CHIP_TYPE;
  const char* chip_names[] = {"PCF8563", "DS1307", "DS3231"};
  esp_err_t rtc_ret = ghost_rtc_init(CONFIG_RTC_I2C_PORT, CONFIG_RTC_I2C_ADDRESS, chip_type);
  if (rtc_ret == ESP_OK) {
    ESP_LOGI(TAG, "RTC initialized: %s on I2C port %d at address 0x%02X (SDA: %d, SCL: %d)",
             chip_names[CONFIG_RTC_CHIP_TYPE], CONFIG_RTC_I2C_PORT, CONFIG_RTC_I2C_ADDRESS,
             CONFIG_RTC_I2C_SDA_PIN, CONFIG_RTC_I2C_SCL_PIN);
  } else {
    ESP_LOGW(TAG, "RTC init failed on I2C port %d at address 0x%02X: %s",
             CONFIG_RTC_I2C_PORT, CONFIG_RTC_I2C_ADDRESS, esp_err_to_name(rtc_ret));
  }
#endif

#ifdef CONFIG_HAS_FUEL_GAUGE
  if (fuel_gauge_manager_init()) {
    ESP_LOGI(TAG, "Fuel gauge manager initialized successfully");
  } else {
    ESP_LOGW(TAG, "Failed to initialize fuel gauge manager");
  }
#endif
}

/* While an overlay lock is up, the shared status bar (normally on lv_scr_act)
 * would be hidden under the opaque top-layer overlay. Lift it onto the top
 * layer above the overlay so battery/clock/icons stay live and the lock looks
 * like a real screen; restore it verbatim on unlock. */
static char s_pre_lock_status_title[48];

static void dm_raise_status_bar_for_overlay(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    const char *t = lv_label_get_text(mainlabel);
    strncpy(s_pre_lock_status_title, t ? t : "", sizeof(s_pre_lock_status_title) - 1);
    s_pre_lock_status_title[sizeof(s_pre_lock_status_title) - 1] = '\0';
    lv_label_set_text(mainlabel, "Locked");
  }
  lv_obj_set_parent(status_bar, lv_layer_top());
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
#endif
  lv_obj_move_foreground(status_bar);
}

static void dm_restore_status_bar_after_overlay(void) {
  if (!status_bar || !lv_obj_is_valid(status_bar)) return;
  lv_obj_set_parent(status_bar, lv_scr_act());
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 16);
#else
  lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
#endif
  lv_obj_move_foreground(status_bar);
  if (mainlabel && lv_obj_is_valid(mainlabel)) {
    lv_label_set_text(mainlabel, s_pre_lock_status_title);
  }
}

static void dm_lockscreen_overlay_create_cb(void *arg) {
  (void)arg;
  /* Runs on the LVGL task: build the lockscreen on the top layer without
   * touching dm.current_view, so the view underneath keeps running. */
  lockscreen_create();
  dm_raise_status_bar_for_overlay();
}

void display_manager_show_lockscreen(void) {
  /* Already locked (overlay up) — don't stack a second one. */
  if (s_lockscreen_overlay_active) return;

  /* The wake / auto-lock flow locks on top of whatever is running. Keep the
   * current view alive and float the lockscreen over it as an overlay so an
   * active capture (wardriving, sniffing, ...) is never torn down by locking. */
  if (dm.current_view && dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
    s_lockscreen_return_view = dm.current_view;
    lockscreen_reset_input();
    lockscreen_set_overlay_mode(true);
    s_lockscreen_overlay_active = true;
    display_manager_run_on_lvgl(dm_lockscreen_overlay_create_cb, NULL);
    return;
  }

  /* No live view to preserve (e.g. boot) — fall back to a plain view switch. */
  s_lockscreen_return_view = NULL;
  lockscreen_reset_input();
  display_manager_switch_view(&lockscreen_view);
}

bool display_manager_is_lockscreen_active(void) {
  return s_lockscreen_overlay_active;
}

void display_manager_clear_lockscreen_overlay(void) {
  /* Called on the LVGL task from the lockscreen once it has torn its overlay
   * down. Put the status bar back where the live view expects it. */
  dm_restore_status_bar_after_overlay();
  s_lockscreen_overlay_active = false;
  s_lockscreen_return_view = NULL;
}

View *display_manager_get_lockscreen_return_view(void) {
  return s_lockscreen_return_view;
}

void display_manager_clear_lockscreen_return_view(void) {
  s_lockscreen_return_view = NULL;
}

void display_manager_destroy_current_view(void) {
  display_manager_cancel_pending_scroll();
  if (dm.current_view) {
    if (dm.current_view->destroy) {
      dm.current_view->destroy();
    }

    dm.current_view = NULL;
  }
}

View *display_manager_get_current_view(void) { return dm.current_view; }

static bool touch_move_events_enabled_for_view_name(const char *view_name) {
  return view_name &&
         (strcmp(view_name, "Options Screen") == 0 ||
          strcmp(view_name, "NFC") == 0 ||
          strcmp(view_name, "Infrared View") == 0 ||
          strcmp(view_name, "SubGHz") == 0 ||
          strcmp(view_name, "Ethernet") == 0 ||
          strcmp(view_name, "AirspaceMonitorView") == 0 ||
          strcmp(view_name, "Audio Player") == 0 ||
          strcmp(view_name, "Camera") == 0 ||
          strcmp(view_name, "Main Menu") == 0 ||
          strcmp(view_name, "Apps Menu") == 0 ||
          strcmp(view_name, "SD Browser") == 0 ||
           strcmp(view_name, "GhostScript Runner") == 0 ||
           strcmp(view_name, "SD App") == 0 ||
           strcmp(view_name, "BadUSB") == 0 ||
          strcmp(view_name, "WardrivingView") == 0 ||
          strcmp(view_name, "Trackpad") == 0 ||
          strcmp(view_name, "Cloud Store") == 0 ||
          strcmp(view_name, "ENV-III") == 0 ||
          strcmp(view_name, "Favorites Manager") == 0 ||
          strcmp(view_name, "Menu Editor") == 0 ||
          strcmp(view_name, "Lockscreen") == 0);
}

static bool touch_move_events_enabled_for_current_view(void) {
  if (!dm.mutex) return false;

  bool enabled = false;
  if (xSemaphoreTake(dm.mutex, 0) == pdTRUE) {
    View *current = dm.current_view;
    enabled = current && touch_move_events_enabled_for_view_name(current->name);
    xSemaphoreGive(dm.mutex);
  }
  return enabled;
}

bool display_manager_is_available(void) { return display_manager_init_success; }

void display_manager_fill_screen(lv_color_t color) {
  lv_obj_t *scr = lv_scr_act();
  if (!scr) return;
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(scr, color, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
}

void display_manager_suspend_lvgl_task(void) {
  if (!lvgl_task_handle || s_lvgl_gate_closed) return;
  if (xTaskGetCurrentTaskHandle() == lvgl_task_handle) return;
  /* Ask the render task to park itself outside lv_timer_handler(), then wait
   * for the acknowledgment before fully suspending. If it is currently mid-flush
   * the ack is delayed until that flush completes — exactly the guarantee the
   * shared-SPI teardown needs. A timeout falls back to the old hard-suspend
   * behavior so a wedged render task can never deadlock the caller. */
  s_lvgl_gate_closed = true;
  TickType_t start = xTaskGetTickCount();
  while (!s_lvgl_gate_parked) {
    if (xTaskGetTickCount() - start > pdMS_TO_TICKS(500)) {
      ESP_LOGW(TAG, "lvgl quiesce timed out; forcing suspend");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  vTaskSuspend(lvgl_task_handle);
}

void display_manager_resume_lvgl_task(void) {
  if (!lvgl_task_handle) return;
  /* Clear the gate first so the task leaves its park loop on resume, then undo
   * the suspend. vTaskResume on a non-suspended task is a harmless no-op, so
   * unbalanced resume calls (e.g. the lightweight shared-SPI guard path) are
   * safe. */
  s_lvgl_gate_closed = false;
  if (xTaskGetCurrentTaskHandle() != lvgl_task_handle) {
    vTaskResume(lvgl_task_handle);
  }
}

void display_manager_suspend_input_task(void) {
  if (!input_task_handle || s_input_gate_closed) return;
  if (xTaskGetCurrentTaskHandle() == input_task_handle) return;
  s_input_gate_closed = true;
  TickType_t start = xTaskGetTickCount();
  while (!s_input_gate_parked) {
    if (xTaskGetTickCount() - start > pdMS_TO_TICKS(500)) {
      ESP_LOGW(TAG, "input quiesce timed out; forcing suspend");
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  vTaskSuspend(input_task_handle);
}

void display_manager_resume_input_task(void) {
  if (!input_task_handle || !s_input_gate_closed) return;
  s_input_gate_closed = false;
  if (xTaskGetCurrentTaskHandle() != input_task_handle) {
    vTaskResume(input_task_handle);
  }
}

static void display_manager_set_backlight_raw(uint8_t percentage) {
    // Clamp to user setting
    uint8_t max_brightness = settings_get_max_screen_brightness(&G_Settings);

    // if (percentage > max_brightness) percentage = max_brightness;

    //scale percent by max_brightness
    percentage = (percentage * max_brightness) / 100;
    if (percentage > 100) percentage = 100;

#ifdef CONFIG_USE_TDISPLAY_S3
    // TDisplay S3 backlight now uses LEDC for PWM control
    uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    ESP_LOGI(TAG, "TDisplay S3 backlight: %d%% (LEDC PWM)", percentage);
#elif defined(CONFIG_IS_ATOMS3R)
    m5gfx_set_brightness(percentage);
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4) && defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    esp_err_t err = crowpanel_p4_display_set_backlight(percentage);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CrowPanel 5-inch backlight update failed: %s", esp_err_to_name(err));
    }
#elif defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
    /* V1.2+ CrowPanel Advance 7-inch boards route backlight control through
     * the onboard STC8H1K28 at 0x30. Its scale is inverted: 0=max and
     * 245=off. The factory firmware uses the same raw one-byte command. */
    uint8_t stc8_backlight = (uint8_t)(((100u - percentage) * 245u + 50u) / 100u);
    esp_err_t err = lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, 0x30, I2C_NO_REG,
                                   &stc8_backlight, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CrowPanel Advance 7-inch STC8 backlight update failed: %s",
                 esp_err_to_name(err));
    }
#elif defined(CONFIG_LV_DISP_BACKLIGHT_PWM)
    if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
        uint32_t duty = (percentage * ((1 << LEDC_TIMER_10_BIT) - 1)) / 100;
        ESP_LOGD(TAG, "BL PWM: scaled_pct=%d, raw_duty=%lu", percentage, (unsigned long)duty);
#if !defined(CONFIG_LV_BACKLIGHT_ACTIVE_LVL)
        duty = ((1 << LEDC_TIMER_10_BIT) - 1) - duty;
        if (duty == 0) duty = 1;
        ESP_LOGD(TAG, "BL PWM: active-low inverted, final_duty=%lu", (unsigned long)duty);
#endif
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
        esp_err_t err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        ESP_LOGD(TAG, "BL PWM: ledc_update_duty returned %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "Backlight GPIO not configured; skipping PWM backlight");
    }
#elif defined(CONFIG_LV_DISP_BACKLIGHT_SWITCH)
    // ----- switch mode -----
    // make sure the pin is configured as a GPIO output

#ifdef CONFIG_Waveshare_LCD
    // Waveshare 7-inch backlight is controlled by CH422G EXIO2, not GPIO
    // It's already set HIGH in waveshare_ch422g_init()
    ESP_LOGD(TAG, "set_backlight_brightness: %d%% (CH422G EXIO2, already on)", percentage);
#else
    if (CONFIG_LV_DISP_PIN_BCKL >= 0) {
        if (!s_switch_backlight_configured) {
            gpio_set_direction(CONFIG_LV_DISP_PIN_BCKL, GPIO_MODE_OUTPUT);
            gpio_set_drive_capability(CONFIG_LV_DISP_PIN_BCKL, GPIO_DRIVE_CAP_3);
            gpio_sleep_sel_dis(CONFIG_LV_DISP_PIN_BCKL);
            s_switch_backlight_configured = true;
        }
        gpio_hold_dis(CONFIG_LV_DISP_PIN_BCKL);
        /* Factory T-Dongle-C5 firmware enables the LCD backlight by driving
         * GPIO0 LOW (active-low BL), matching the rest of the codebase where
         * CONFIG_LV_BACKLIGHT_ACTIVE_LVL unset == "on when low".  The switch
         * path below honors that polarity; switch-mode boards with a
         * high-active backlight (e.g. CYD) set CONFIG_LV_BACKLIGHT_ACTIVE_LVL
         * and get the normal HIGH=on behavior. */
        bool on = percentage > 0;
#if defined(CONFIG_LV_BACKLIGHT_ACTIVE_LVL)
        gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, on ? 1 : 0);
#else
        gpio_set_level(CONFIG_LV_DISP_PIN_BCKL, on ? 0 : 1);
#endif
        gpio_hold_en(CONFIG_LV_DISP_PIN_BCKL);
        ESP_LOGI(TAG, "Backlight GPIO%d latched %s (active%s, requested=%u%%, scaled=%u%%)",
                 CONFIG_LV_DISP_PIN_BCKL, on ? "ON" : "OFF",
#if defined(CONFIG_LV_BACKLIGHT_ACTIVE_LVL)
                 "-high",
#else
                 "-low",
#endif
                 (unsigned)((max_brightness > 0) ? (percentage * 100) / max_brightness : 0),
                 (unsigned)percentage);
    } else {
        ESP_LOGD(TAG, "Backlight GPIO not configured; skipping switch backlight");
    }
#endif
#elif defined(CONFIG_LV_DISP_BACKLIGHT_OFF)
    ESP_LOGD(TAG, "Backlight hardware-fixed ON; ignoring %u%% request", (unsigned)percentage);
    (void)percentage;
#else
# error "Either CONFIG_LV_DISP_BACKLIGHT_PWM or CONFIG_LV_DISP_BACKLIGHT_SWITCH must be set"
#endif

    ESP_LOGD(TAG, "set_backlight_brightness: %d%% (max allowed: %d%%)", percentage, max_brightness);

#ifdef CONFIG_USE_TDECK
    // Synchronize keyboard backlight with screen backlight
    // ...
    set_keyboard_brightness(percentage == max_brightness ? 0xFF : 0x00);
#endif
}

void set_backlight_brightness(uint8_t percentage) {
    uint8_t max_brightness = settings_get_max_screen_brightness(&G_Settings);
    ESP_LOGD(TAG, "set_backlight_brightness: input=%d%%, max_brightness_setting=%d%%", percentage, max_brightness);
    display_manager_set_backlight_raw(percentage);

    percentage = (percentage * max_brightness) / 100;
    if (percentage > 100) percentage = 100;

    /*
     * The rest of your pause/resume logic stays exactly the same,
     * so when you call set_backlight_brightness(0) everything
     * (timers, Wi-Fi PS, tasks) still pauses as before.
     */
    if (percentage == 0) {
        // 1) Disable every wake-source (we'll re-enable only the one we actually want)
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        esp_sleep_disable_wifi_wakeup();
        esp_sleep_disable_wifi_beacon_wakeup();

#ifdef CONFIG_IS_S3TWATCH
        // 2a) On S3T-Watch: only GPIO button can wake us
        gpio_wakeup_enable(WAKE_UP_PIN, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();

        // 3a) Pause all UI and drop into light-sleep until that button is pressed
        if (status_update_timer)   lv_timer_pause(status_update_timer);
        if (status_update_timer)   lv_timer_set_period(status_update_timer, 5000);
#ifndef CONFIG_USE_CARDPUTER
        if (lvgl_task_handle)      vTaskSuspend(lvgl_task_handle);
#endif
        if (rainbow_timer)         lv_timer_pause(rainbow_timer);
        if (terminal_update_timer) lv_timer_pause(terminal_update_timer);
        if (clock_timer)           lv_timer_pause(clock_timer);
        {
            if (!wifi_manager_is_evil_portal_active()) {
                wifi_config_t cfg;
                if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
                    original_beacon_interval = cfg.ap.beacon_interval;
                    cfg.ap.beacon_interval = 1000;
                    esp_wifi_set_config(WIFI_IF_AP, &cfg);
                }
                esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
            }
        }
        esp_light_sleep_start();

#else
        // 2b) On Cardputer (or other devices without real GPIO wake):
        //     we do *not* enter light-sleep at all, we just turn the backlight off
        //     and rely on our existing polling (touch or key scans) to call
        //     set_backlight_brightness(1) when user input arrives.
#endif

        return;
    } else {
        bool was_off = is_backlight_off;
        is_backlight_dimmed = false;      // <— also clear whenever we restore brightness
        is_backlight_off = false;

        // Lockscreen on wake
        if (was_off && settings_get_lockscreen_enabled(&G_Settings) &&
            settings_get_lockscreen_wake_lock(&G_Settings) &&
            !s_lockscreen_overlay_active &&
            dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
            display_manager_show_lockscreen();
        }

        if (status_update_timer)   lv_timer_resume(status_update_timer);
        if (status_update_timer)   lv_timer_set_period(status_update_timer, 1000);
        if (lvgl_task_handle)      vTaskResume(lvgl_task_handle);
        if (rainbow_timer)         lv_timer_resume(rainbow_timer);
        if (terminal_update_timer) lv_timer_resume(terminal_update_timer);
        if (clock_timer)           lv_timer_resume(clock_timer);
#ifdef CONFIG_IS_S3TWATCH
        {
            if (!wifi_manager_is_evil_portal_active()) {
                esp_wifi_set_ps(WIFI_PS_NONE);
                wifi_config_t cfg;
                if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
                    cfg.ap.beacon_interval = original_beacon_interval;
                    esp_wifi_set_config(WIFI_IF_AP, &cfg);
                }
            }
        }
#endif
    }
}

bool display_manager_notify_user_input(void) {
    // If backlight is dimmed/off, restore it and indicate the input was used to wake
    if (is_backlight_dimmed || is_backlight_off) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
        last_touch_time = xTaskGetTickCount();
        return true;
    }
    // otherwise update last_touch_time and allow normal processing
    last_touch_time = xTaskGetTickCount();
    return false;
}

#ifdef CONFIG_USE_TDECK
// Convert T-Deck raw key position to character with shift and symbol support
static char tdeck_raw_to_char(int col, int row, bool shift, bool symbol) {
  // Handle special keys first
  if (col == 3 && row == 3) return '\r';  // Enter
  if (col == 4 && row == 3) return '\b';  // Backspace
  if (col == 0 && row == 5) return ' ';   // Space
  
  // Skip non-character keys (shift, alt, etc.)
  if ((col == 1 && row == 6) || (col == 2 && row == 3)) return 0; // Shift keys
  if (col == 0 && row == 2) return 0;  // Symbol key
  if (col == 0 && row == 4) return 0;  // ALT key
  if (col == 0 && row == 6) return 0;  // Mic key
  
  // T-Deck keyboard layout from Arduino code (5 cols x 7 rows)
  // Format: keyboard[col][row] where col=0-4, row=0-6
  static const char keyboard[TDECK_COLS][TDECK_ROWS] = {
    {'q', 'w', 0, 'a', 0, ' ', 0},      // col 0 (row 2=symbol, row 4=ALT, row 6=Mic)
    {'e', 's', 'd', 'p', 'x', 'z', 0},       // col 1 (row 6=Left Shift)
    {'r', 'g', 't', 0, 'v', 'c', 'f'},       // col 2 (row 3=Right Shift)
    {'u', 'h', 'y', 0, 'b', 'n', 'j'},       // col 3 (row 3=Enter)
    {'o', 'l', 'i', 0, '$', 'm', 'k'}        // col 4 (row 3=Backspace)
  };
  
  // Symbol characters from Arduino code (5 cols x 7 rows)
  // Format: keyboard_symbol[col][row] where col=0-4, row=0-6
  static const char keyboard_symbol[TDECK_COLS][TDECK_ROWS] = {
    {'#', '1', 0, '*', 0, 0, '0'},      // col 0
    {'2', '4', '5', '@', '8', '7', 0},       // col 1
    {'3', '/', '(', 0, '?', '9', '6'},       // col 2
    {'_', ':', ')', 0, '!', ',', ';'},        // col 3
    {'+', '"', '-', 0, 0, '.', '\''}       // col 4
  };
  
  // Get base character
  char c = 0;
  if (col < TDECK_COLS && row < TDECK_ROWS) {
    if (symbol) {
      c = keyboard_symbol[col][row];
    } else {
      c = keyboard[col][row];
    }
  }
  
  if (c == 0) return 0;
  
  // Apply shift if needed (for symbol mode, shift might add different characters)
  if (shift) {
    // For normal characters, convert to uppercase
    if (!symbol && c >= 'a' && c <= 'z') {
      c = c - ('a' - 'A');
    }
    // Additional shift combinations could be added here
  }
  
  return c;
}

// Emit a keyboard release event so the global key-repeat timer (kb_repeat_*)
// gets cancelled when the held key is let go. Without this the T-Deck matrix
// path only ever sends presses, so the repeat spams forever after release —
// exactly like the Cardputer matrix bug fixed earlier. The release value is
// ignored by kb_repeat_stop(); we just need one is_touch_move=true event.
static void tdeck_emit_key_release(uint8_t key) {
  if (key == 0) return;
  InputEvent rel_event = {0};
  rel_event.type = INPUT_TYPE_KEYBOARD;
  rel_event.data.key_value = key;
  rel_event.is_touch_move = true; // release
  if (xQueueSend(input_queue, &rel_event, 0) != pdTRUE) {
    ESP_LOGD(TAG, "Failed to queue T-Deck keyboard release event\n");
  }
}
#endif



#ifdef CONFIG_USE_ENCODER
static void encoder_poll_task(void *pvParameters)
{
    (void)pvParameters;
    const TickType_t sample_interval = pdMS_TO_TICKS(1);
    while (1) {
        encoder_tick(&g_encoder);
        vTaskDelay(sample_interval);
    }
    vTaskDelete(NULL);
}
#endif

void hardware_input_task(void *pvParameters) {
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
  const TickType_t tick_interval = pdMS_TO_TICKS(5);
#else
  const TickType_t tick_interval = pdMS_TO_TICKS(10);
#endif
  const int touch_move_min_delta = 8;

  lv_indev_drv_t touch_driver;
  lv_indev_drv_init(&touch_driver);
  touch_driver.disp = lv_disp_get_default();
  lv_indev_data_t touch_data = {
    .point = {0, 0},
    .state = LV_INDEV_STATE_REL,
  };
  bool touch_active = false;
  bool skip_next_release = false;
#if GUI_LARGE_TOUCH_UI
  bool system_edge_touch = false;
#endif
  int last_touch_x = 0;
  int last_touch_y = 0;
#ifdef CONFIG_IS_S3TWATCH
  bool was_woken_by_interrupt = false; // New flag for S3T-Watch
#endif
#ifdef CONFIG_USE_TDECK
  static uint8_t last_tdeck_key = 0;
  static uint32_t last_tdeck_key_ms = 0;
  static uint32_t last_tdeck_event_ms = 0;
  static uint8_t last_tdeck_sent = 0;
  static bool tdeck_shift_pressed = false;
  static bool tdeck_symbol_pressed = false;
  static uint8_t tdeck_last_raw_state[TDECK_COLS] = {0};
  static uint32_t tdeck_repeat_start_ms = 0;
  static bool tdeck_repeat_active = false;
  static char tdeck_repeat_char = 0;

  gpio_set_direction(46, GPIO_MODE_INPUT);
  
  // Initialize keyboard in raw mode for shift key support
  uint8_t raw_cmd = LILYGO_KB_MODE_RAW_CMD;
  lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, &raw_cmd, 1);
  ESP_LOGI(TAG, "T-Deck keyboard initialized in raw mode");
#endif
  while (1) {
    if (s_input_gate_closed) {
      s_input_gate_parked = true;
      while (s_input_gate_closed) {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
      s_input_gate_parked = false;
    }
#ifdef CONFIG_USE_TDECK
    // Read raw keyboard state for shift key support
    uint8_t raw_data[TDECK_COLS] = {0};
    bool key_changed = false;
    
    if (gpio_get_level(46)) {
      if (lvgl_i2c_read(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, &raw_data, TDECK_COLS) == ESP_OK) {
        // Check if any key state changed
        for (int i = 0; i < TDECK_COLS; i++) {
          if (raw_data[i] != tdeck_last_raw_state[i]) {
            key_changed = true;
            break;
          }
        }
        
        // Always update shift and symbol states
        bool left_shift = (raw_data[TDECK_LEFT_SHIFT_COL] & (1 << TDECK_LEFT_SHIFT_ROW)) != 0;
        bool right_shift = (raw_data[TDECK_RIGHT_SHIFT_COL] & (1 << TDECK_RIGHT_SHIFT_ROW)) != 0;
        tdeck_shift_pressed = left_shift || right_shift;
        tdeck_symbol_pressed = (raw_data[TDECK_SYMBOL_COL] & (1 << TDECK_SYMBOL_ROW)) != 0;
        
        // Find currently pressed keys
        char current_char = 0;
        bool key_pressed = false;
        
        for (int col = 0; col < TDECK_COLS; col++) {
          for (int row = 0; row < TDECK_ROWS; row++) {
            if (raw_data[col] & (1 << row)) {
              char c = tdeck_raw_to_char(col, row, tdeck_shift_pressed, tdeck_symbol_pressed);
              if (c != 0) {
                current_char = c;
                key_pressed = true;
                break; // Take first character found
              }
            }
          }
          if (key_pressed) break;
        }
        
        uint32_t now_ms = dm_now_ms();
        
        if (key_changed) {
          // Update last state
          memcpy(tdeck_last_raw_state, raw_data, TDECK_COLS);
          
          if (key_pressed) {
            if (current_char != tdeck_repeat_char) {
              // New key pressed - send immediately and start repeat timer
              tdeck_repeat_char = current_char;
              tdeck_repeat_start_ms = now_ms;
              tdeck_repeat_active = false;
              
              ESP_LOGD(TAG, "T-Deck key pressed: '%c'", current_char);
              
              touch_active = true;
              if (is_backlight_dimmed || is_backlight_off) {
                set_backlight_brightness(100);
                is_backlight_dimmed = false;
                is_backlight_off = false;
                vTaskDelay(pdMS_TO_TICKS(20));
              }
              
              InputEvent event;
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = current_char;
              if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send T-Deck key input to queue\n");
              }
            }
          } else {
            // No keys pressed - emit release so the global key-repeat timer
            // stops, then reset repeat state.
            tdeck_emit_key_release((uint8_t)tdeck_repeat_char);
            tdeck_repeat_char = 0;
            tdeck_repeat_active = false;
          }
        }
        
        // Handle key repeat - this runs every loop iteration, not just on state change
        if (key_pressed && tdeck_repeat_char != 0) {
          if (!tdeck_repeat_active) {
            // Check if initial delay has passed
            if (now_ms - tdeck_repeat_start_ms >= get_tdeck_repeat_delay()) {
              tdeck_repeat_active = true;
              tdeck_repeat_start_ms = now_ms; // Reset for repeat rate
              
              ESP_LOGD(TAG, "T-Deck repeat started for: '%c'", current_char);
            }
          } else {
            // Check if repeat rate has passed
            if (now_ms - tdeck_repeat_start_ms >= get_tdeck_repeat_rate()) {
              tdeck_repeat_start_ms = now_ms; // Reset for next repeat
              
              ESP_LOGD(TAG, "T-Deck key repeat: '%c'", current_char);
              
              InputEvent event;
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = current_char;
              if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGE(TAG, "Failed to send T-Deck repeat key to queue\n");
              }
            }
          }
        } else if (!key_pressed) {
          // No keys pressed - emit release so the global key-repeat timer
          // stops, then reset repeat state.
          tdeck_emit_key_release((uint8_t)tdeck_repeat_char);
          tdeck_repeat_char = 0;
          tdeck_repeat_active = false;
        }
      }
    } else {
      // Reset state when pin is low - emit release so the global key-repeat
      // timer stops (the interrupt line can drop before a zero read arrives).
      tdeck_emit_key_release((uint8_t)tdeck_repeat_char);
      memset(tdeck_last_raw_state, 0, TDECK_COLS);
      tdeck_shift_pressed = false;
      tdeck_symbol_pressed = false;
      tdeck_repeat_char = 0;
      tdeck_repeat_active = false;
    }
#endif


// Check for wake interrupt when dimmed
#ifdef CONFIG_IS_S3TWATCH
    if (is_backlight_dimmed && xSemaphoreTake(wake_up_sem, 0) == pdTRUE) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        last_touch_time = xTaskGetTickCount(); // Reset inactivity timer
        was_woken_by_interrupt = true; // Set flag
    }
#endif

#ifdef CONFIG_USE_ENCODER
#ifdef CONFIG_USE_IO_EXPANDER
    /* IO expander encoder: poll at hardware_input_task rate (100 Hz) */
    encoder_tick(&g_encoder);
#else
    /* Direct GPIO encoder: sampled at 1 kHz in encoder_poll_task; just drain events here */
#endif

    /* direction events */
    if (encoder_peek_direction(&g_encoder) != ENCODER_DIR_NONE) {
        // treat an encoder turn as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
        } else {
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
          /* Fast spins can include accelerated detents. Drain the decoder in
           * one burst so they reach the audio view on this hardware tick. */
          const int max_encoder_events_per_tick = 12;
#else
          const int max_encoder_events_per_tick = 4;
#endif
          for (int i = 0; i < max_encoder_events_per_tick; i++) {
              encoder_direction_t raw_dir = encoder_peek_direction(&g_encoder);
              if (raw_dir == ENCODER_DIR_NONE) break;

              int8_t dir = (int8_t)raw_dir;
              bool invert_encoder = settings_get_encoder_invert_direction(&G_Settings);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
              /* The factory firmware treats the A-rising/B-low phase as
               * its increment path. The shared four-state decoder reports
               * that physical cycle as CCW, so normalize the board once;
               * the user setting still provides the final invert toggle. */
              invert_encoder = !invert_encoder;
#endif
              if (invert_encoder) {
                  dir = (int8_t)-dir;
              }

              InputEvent ev = {
                  .type = INPUT_TYPE_ENCODER,
                  .data.encoder = { .direction = dir, .button = false }
              };
              if (xQueueSend(input_queue, &ev, 0) == pdTRUE) {
                  encoder_consume_direction(&g_encoder, raw_dir);
              } else {
                  break;
              }
          }
        }
    }

    /* push-switch -> treat like "button". The factory uses a 20 ms debounce;
     * retain that edge filter for the CrowPanel while leaving other encoder
     * boards on their established input path. */
    bool encoder_button_pressed = joystick_just_pressed(&enc_button);
#ifdef CONFIG_CROWPANEL_1P28_ROTARY
    static TickType_t crowpanel_last_button_tick = 0;
    if (encoder_button_pressed) {
      TickType_t now = xTaskGetTickCount();
      if (crowpanel_last_button_tick != 0 &&
          (now - crowpanel_last_button_tick) < pdMS_TO_TICKS(20)) {
        encoder_button_pressed = false;
      } else {
        crowpanel_last_button_tick = now;
      }
    }
#endif
    if (encoder_button_pressed) {
        // treat an encoder click as "touch"
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
        } else {
          // Only send input event if display was already active
          InputEvent ev = {
              .type = INPUT_TYPE_ENCODER,
              .data.encoder = { .direction = 0, .button = true }
          };
          xQueueSend(input_queue, &ev, 0);
        }
    }
#endif

#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        // check IO6 exit button (TEmbed C1101 only)
        if (joystick_just_pressed(&exit_button)) {
            last_touch_time = xTaskGetTickCount();
            if (is_backlight_dimmed || is_backlight_off) {
              set_backlight_brightness(100);
              is_backlight_dimmed = false;
              is_backlight_off = false;
            } else {
              InputEvent ev = {
                  .type = INPUT_TYPE_EXIT_BUTTON,
                  .data.exit_pressed = true
              };
              xQueueSend(input_queue, &ev, 0);
            }
        }

        // Check for 7-second hold to enter deep sleep
        if (joystick_get_button_state(&exit_button) && exit_button.pressed) {
        uint32_t elapsed = (esp_timer_get_time() / 1000) - exit_button.hold_init;
        if (elapsed >= 4000 && !exit_button.deep_sleep_triggered) { // 4 seconds
            ESP_LOGI("DeepSleep", "IO6 held for 4 seconds, preparing for deep sleep");
            exit_button.deep_sleep_triggered = true;

            // Pull IO15 low before sleep (TEmbed C1101 power control)
            gpio_set_level(15, 0);
            ESP_LOGI("DeepSleep", "IO15 pulled low");

            ESP_LOGI("DeepSleep", "Configuring wake-up source");

            // Disable all wake-up sources first
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

            // Temporarily disable the GPIO to avoid immediate wake-up
            gpio_config_t io_conf = {
                .pin_bit_mask = (1ULL << 6),
                .mode = GPIO_MODE_DISABLE,  // Temporarily disable the GPIO
                .pull_up_en = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE
            };
            gpio_config(&io_conf);

            error_popup_create_persistent("SHUTTING DOWN");
            // Wait a couple of seconds to ignore any button releases
            ESP_LOGI("DeepSleep", "Waiting 4 seconds before sleep to ignore button release...");
            vTaskDelay(pdMS_TO_TICKS(4000)); // 4 second delay

            // Re-enable the GPIO with proper configuration for wake-up
            io_conf.mode = GPIO_MODE_INPUT;
            io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
            gpio_config(&io_conf);

#if SOC_PM_SUPPORT_EXT0_WAKEUP
            esp_err_t ret = esp_sleep_enable_ext0_wakeup(GPIO_NUM_6, 0);
#elif SOC_PM_SUPPORT_EXT1_WAKEUP
            esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(1ULL << GPIO_NUM_6, ESP_EXT1_WAKEUP_ANY_LOW);
#else
            esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
#endif
            if (ret != ESP_OK) {
                ESP_LOGE("DeepSleep", "Failed to configure wake-up source: %s", esp_err_to_name(ret));
                exit_button.deep_sleep_triggered = false;
                gpio_set_level(15, 1); // Restore IO15 high
                return;
            }
            ESP_LOGI("DeepSleep", "Wake-up source configured for new button press");

            ESP_LOGI("DeepSleep", "Entering deep sleep now...");
            vTaskDelay(pdMS_TO_TICKS(200)); // Give time for log to print

            // Final check of GPIO state before sleep
            ESP_LOGI("DeepSleep", "Final GPIO6 state: %d", gpio_get_level(6));
            ESP_LOGI("DeepSleep", "Final GPIO15 state: %d", gpio_get_level(15));

            // Enter deep sleep
            esp_deep_sleep_start();
        }
        } else {
            // Reset deep sleep trigger when button is released
            exit_button.deep_sleep_triggered = false;
        }
    }
#endif

#ifdef CONFIG_USE_CARDPUTER
    keyboard_update_key_list(&gkeyboard);
    keyboard_update_keys_state(&gkeyboard);

    // force caps-lock off for normal cardputer so letters aren't stuck uppercase
    gkeyboard.is_caps_locked = false;

      if (!keyboard_is_key_pressed(&gkeyboard,129) && caps_latch){ // caps lock latch so it doesnt continuously flip on and off
        caps_latch = false;
        shift_count = 0;
      }

    if (gkeyboard.key_list_buffer_len > 0) {

      for (size_t i = 0; i < gkeyboard.key_list_buffer_len; ++i) {
        Point2D_t key_pos = gkeyboard.key_list_buffer[i];
        // emit only on new press (edge-triggered)
        bool is_new_press = true;
        for (size_t k = 0; k < last_pressed_len; ++k) {
          if (last_pressed_keys[k].x == key_pos.x && last_pressed_keys[k].y == key_pos.y) {
            is_new_press = false;
            break;
          }
        }
        if (!is_new_press) {
          continue;
        }
        uint8_t key_value = keyboard_get_key(&gkeyboard, key_pos);
        keyboard_update_keys_state(&gkeyboard);
        if (key_value != 0) {
          bool skip_event = false;
          last_touch_time = xTaskGetTickCount();
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
            skip_event = true;
            vTaskDelay(pdMS_TO_TICKS(20));
          }

          if (!skip_event) {
            InputEvent event = {0};
            event.is_touch_move = false; // press event (release is emitted separately)
            // event.type will be set inside the switch for specific keys

            if (shift_count > shift_count_before_caps && !caps_latch){ // toggle caps if weve been holding shift long enough without intteruption
              gkeyboard.is_caps_locked = !gkeyboard.is_caps_locked;
              caps_latch = true;
              ESP_LOGW(TAG, "Capslock toggled %s\n", gkeyboard.is_caps_locked ? "on" : "off");
              shift_count = 0;
            }


            ESP_LOGI(TAG, "Input key value: %d\n", key_value);

            switch (key_value) {
            case 129: //shift - keyboard library already handled caps for letters
              if(!keyboard_is_change(&gkeyboard)){ // if shift is held down increment the counter for capslocks
                shift_count += 1;
              }
              continue;
            case 255: //fn - fn key actions
              continue;
            case 128: //ctrl - ctrl key actions
              continue;
            case 130: // alt - alt key actions
              continue;
            default:
              ESP_LOGI(TAG, "Keyboard key: %c (value: %d) (CAPS: %s)\n", key_value, key_value, gkeyboard.is_caps_locked ? "on" : "off" );
              shift_count = 0; // restart caps timer wheen a key gets pressed
              event.type = INPUT_TYPE_KEYBOARD;
              event.data.key_value = key_value;
              break;
            }
            if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to send button input to queue\n");
            }
          }

        }
      }
      }
      // Emit release events for keys that were down last poll but are now up,
      // so the global key-repeat timer gets cancelled. Without this the matrix
      // path only ever sends presses and the repeat spams forever (the ADV/TCA8418
      // path emits releases explicitly, which is why it doesn't have this bug).
      for (size_t k = 0; k < last_pressed_len; ++k) {
        bool still_down = false;
        for (size_t i = 0; i < gkeyboard.key_list_buffer_len; ++i) {
          if (gkeyboard.key_list_buffer[i].x == last_pressed_keys[k].x &&
              gkeyboard.key_list_buffer[i].y == last_pressed_keys[k].y) {
            still_down = true;
            break;
          }
        }
        if (still_down) continue;
        uint8_t rel_value = keyboard_get_key(&gkeyboard, last_pressed_keys[k]);
        if (rel_value == 0) continue;
        InputEvent rel_event = {0};
        rel_event.type = INPUT_TYPE_KEYBOARD;
        rel_event.data.key_value = rel_value;
        rel_event.is_touch_move = true; // release
        if (xQueueSend(input_queue, &rel_event, 0) != pdTRUE) {
          ESP_LOGD(TAG, "Failed to queue keyboard release event\n");
        }
      }
      // update last pressed cache (cap to 16)
      last_pressed_len = gkeyboard.key_list_buffer_len;
      if (last_pressed_len > 16) last_pressed_len = 16;
      for (size_t m = 0; m < last_pressed_len; ++m) {
        last_pressed_keys[m] = gkeyboard.key_list_buffer[m];
      }
#endif

#ifdef CONFIG_USE_ATOMS3R_BUTTON
    /* AtomS3R single active-low button. Actions are decided on release by how
       long the button was held, plus a short multi-tap counter:
         1 tap          -> Down   (index 4)       navigate next
         2 taps         -> Up     (index 2)       navigate previous
         3 taps         -> Back   (EXIT_BUTTON)
         hold >= 0.5s   -> Select (index 1)
         hold >= 1.0s   -> Back   (EXIT_BUTTON)
       Back has TWO triggers -- triple tap and long hold -- so going back is
       always reliable even when triple-tap timing is slightly off (this is the
       universal back affordance for menus that have no on-screen Back item). */
    {
      static bool button_was_pressed = false;
      static uint8_t tap_count = 0;
      static uint32_t button_pressed_at = 0;
      static uint32_t last_release_at = 0;
      const uint32_t MULTI_TAP_WINDOW_MS = 300; /* forgiving enough for triple */
      const uint32_t HOLD_SELECT_MS = 500;
      const uint32_t HOLD_BACK_MS = 2000;
      bool pressed = joystick_get_button_state(&joysticks[0]);
      uint32_t now_ms = dm_now_ms();

      /* Commit counted quick taps: instantly at 3 (max), otherwise once the
         window since the last release elapses with the button still up. */
      if (!pressed && tap_count > 0 &&
          (tap_count >= 3 || (now_ms - last_release_at) >= MULTI_TAP_WINDOW_MS)) {
        if (tap_count >= 3) {
          // Triple tap = Back. Views map INPUT_TYPE_EXIT_BUTTON to go-back;
          // joystick index 0 is "Left" on the carousel, not back.
          InputEvent event = {
            .type = INPUT_TYPE_EXIT_BUTTON,
            .data.exit_pressed = true,
          };
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
        } else {
          int index = (tap_count == 1) ? 4 : 2; /* 1 -> Down, 2 -> Up */
          InputEvent event = {
            .type = INPUT_TYPE_JOYSTICK,
            .data.joystick_index = index,
            .data.joystick_pressed = true,
          };
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
          event.data.joystick_pressed = false;
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
        }
        tap_count = 0;
      }

      if (pressed && !button_was_pressed) {
        button_pressed_at = now_ms;
        button_was_pressed = true;
        last_touch_time = xTaskGetTickCount();
        if (is_backlight_dimmed || is_backlight_off) {
          set_backlight_brightness(100);
          is_backlight_dimmed = false;
          is_backlight_off = false;
        }
      } else if (!pressed && button_was_pressed) {
        uint32_t duration = now_ms - button_pressed_at;
        if (duration >= HOLD_BACK_MS) {
          // Long hold = Back. Reliable, timing-independent alternative to the
          // triple tap so back always works in any menu.
          tap_count = 0;
          InputEvent event = {
            .type = INPUT_TYPE_EXIT_BUTTON,
            .data.exit_pressed = true,
          };
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
        } else if (duration >= HOLD_SELECT_MS) {
          // Medium hold = Select, delivered as a press+release pair.
          tap_count = 0;
          InputEvent event = {
            .type = INPUT_TYPE_JOYSTICK,
            .data.joystick_index = 1,
            .data.joystick_pressed = true,
          };
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
          event.data.joystick_pressed = false;
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
        } else {
          // Quick tap: count toward Down/Up/triple-Back (committed above).
          if (tap_count < 3) tap_count++;
          last_release_at = now_ms;
        }
        button_was_pressed = false;
      }
    }
#elif defined(CONFIG_USE_JOYSTICK)
#ifdef CONFIG_USE_TDECK
    {
      uint32_t now_ms = dm_now_ms();
      if ((now_ms - trackball_last_event_ms) >= TDECK_TRACKBALL_DEBOUNCE_MS) {
        int direction = -1;
        
        if (trackball_up_flag || trackball_left_flag) {
          direction = trackball_up_flag ? 2 : 0;  // Up=2, Left=0
        } else if (trackball_down_flag || trackball_right_flag) {
          direction = trackball_down_flag ? 4 : 3;  // Down=4, Right=3
        }
        
        trackball_up_flag = false;
        trackball_down_flag = false;
        trackball_left_flag = false;
        trackball_right_flag = false;
        
        if (direction >= 0) {
          trackball_last_event_ms = now_ms;
          last_touch_time = xTaskGetTickCount();
          
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
          } else {
            InputEvent event;
            event.type = INPUT_TYPE_JOYSTICK;
            event.data.joystick_index = direction;
            event.data.joystick_pressed = true;
            xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
          }
          
          vTaskDelay(pdMS_TO_TICKS(TDECK_TRACKBALL_POST_DELAY_MS));
        }
      } else {
        trackball_up_flag = false;
        trackball_down_flag = false;
        trackball_left_flag = false;
        trackball_right_flag = false;
      }
      
      if (joystick_just_released(&joysticks[1])) {
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = 1;
        event.data.joystick_pressed = false;
        xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
      } else if (joystick_just_pressed(&joysticks[1])) {
        last_touch_time = xTaskGetTickCount();
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index = 1;
        event.data.joystick_pressed = true;
        xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
      }
    }
#else
    static uint32_t joystick_repeat_next_ms[5] = {0};
    static bool joystick_reported_pressed[5] = {false};
    for (int i = 0; i < 5; i++) {
      if (joysticks[i].pin < 0) continue;

      bool pressed_now = joystick_get_button_state(&joysticks[i]);
      plugin_api_record_joystick_state((unsigned int)i, pressed_now);
      if (pressed_now != joystick_reported_pressed[i]) {
        joystick_reported_pressed[i] = pressed_now;
        ESP_LOGI(TAG, "Joystick %d %s", i, pressed_now ? "pressed" : "released");
#ifdef CONFIG_CROWPANEL_EPAPER_42
        /* HOME is a board-level route action, not an Up key. It is handled
         * before the active view callback so every screen gets the same
         * reliable escape-to-root behavior. */
        if (i == 2) {
          joystick_repeat_next_ms[i] = 0;
          if (pressed_now) {
            last_touch_time = xTaskGetTickCount();
            if (!(is_backlight_dimmed || is_backlight_off)) {
              InputEvent home_event = {0};
              home_event.type = INPUT_TYPE_HOME_BUTTON;
              home_event.data.home_pressed = true;
              xQueueSend(input_queue, &home_event, pdMS_TO_TICKS(10));
            }
          }
          continue;
        }
        /* The factory firmware calls GPIO1 EXIT_KEY. It occupies the fifth
         * joystick slot in main.c only so the existing GPIO setup can be
         * reused; expose it to every view as the shared Back event instead of
         * pretending it is a Down key. */
        if (i == 4) {
          joystick_repeat_next_ms[i] = 0;
          if (pressed_now) {
            last_touch_time = xTaskGetTickCount();
            if (is_backlight_dimmed || is_backlight_off) {
              set_backlight_brightness(100);
              is_backlight_dimmed = false;
              is_backlight_off = false;
            } else {
              InputEvent back_event = {0};
              back_event.type = INPUT_TYPE_EXIT_BUTTON;
              back_event.data.exit_pressed = true;
              xQueueSend(input_queue, &back_event, pdMS_TO_TICKS(10));
            }
          }
          continue;
        }
#endif
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index =
#ifdef CONFIG_CROWPANEL_EPAPER_42
            crowpanel_epaper_logical_joystick_index(i);
#else
            i;
#endif
        event.data.joystick_pressed = pressed_now;
        if (pressed_now) {
          last_touch_time = xTaskGetTickCount();
          if (is_backlight_dimmed || is_backlight_off) {
            set_backlight_brightness(100);
            is_backlight_dimmed = false;
            is_backlight_off = false;
          } else {
            if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
              ESP_LOGE(TAG, "Failed to send joystick input to queue\n");
            }
          }
          if (i != 1) {
            joystick_repeat_next_ms[i] = dm_now_ms() + get_joystick_repeat_initial_delay();
          }
        } else {
          joystick_repeat_next_ms[i] = 0;
          xQueueSend(input_queue, &event, pdMS_TO_TICKS(10));
        }
        continue;
      }

      if (i != 0 && i != 2 && i != 3 && i != 4) continue;

      if (!pressed_now) {
        joystick_repeat_next_ms[i] = 0;
        continue;
      }

      if (joystick_repeat_next_ms[i] == 0) continue;

      uint32_t now_ms = dm_now_ms();
      if ((int32_t)(now_ms - joystick_repeat_next_ms[i]) >= 0) {
        last_touch_time = xTaskGetTickCount();
        InputEvent event;
        event.type = INPUT_TYPE_JOYSTICK;
        event.data.joystick_index =
#ifdef CONFIG_CROWPANEL_EPAPER_42
            crowpanel_epaper_logical_joystick_index(i);
#else
            i;
#endif
        event.data.joystick_pressed = true;

        if (xQueueSend(input_queue, &event, 0) == pdTRUE) {
          joystick_repeat_next_ms[i] = now_ms + get_joystick_repeat_interval();
        }
      }
    }
#ifdef CONFIG_USE_IO_EXPANDER
    /* P10, P11, P12 (B1, B2, B3): if a command is set, run it; else send as joystick 5/6/7 */
    {
        static bool prev_b1 = false, prev_b2 = false, prev_b3 = false;
        btn_event_t states = {0};
        if (io_manager_get_cached_button_states(&states) == ESP_OK) {
            if (states.b1 && !prev_b1) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p10_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
#if CONFIG_HAS_INFRARED
                            display_manager_switch_view(&infrared_view);
#endif
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 5 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            if (states.b2 && !prev_b2) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p11_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
#if CONFIG_HAS_INFRARED
                            display_manager_switch_view(&infrared_view);
#endif
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 6 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            if (states.b3 && !prev_b3) {
                last_touch_time = xTaskGetTickCount();
                if (is_backlight_dimmed || is_backlight_off) { set_backlight_brightness(100); is_backlight_dimmed = false; is_backlight_off = false; }
                const char *cmd = settings_get_io_btn_p12_cmd(&G_Settings);
                if (cmd && cmd[0] != '\0') {
                    if (strncmp(cmd, "view:", 5) == 0) {
                        if (strcmp(cmd, "view:wifi") == 0) {
                            SelectedMenuType = OT_Wifi;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ble") == 0) {
                            SelectedMenuType = OT_Bluetooth;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:nfc") == 0) {
                            display_manager_switch_view(&nfc_view);
                        } else if (strcmp(cmd, "view:ir") == 0) {
#if CONFIG_HAS_INFRARED
                            display_manager_switch_view(&infrared_view);
#endif
                        } else if (strcmp(cmd, "view:badusb") == 0) {
                            display_manager_switch_view(&badusb_view);
                        } else if (strcmp(cmd, "view:gps") == 0) {
                            SelectedMenuType = OT_GPS;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:compass") == 0) {
                            display_manager_switch_view(&compass_view);
                        } else if (strcmp(cmd, "view:enviii") == 0) {
                            display_manager_switch_view(&enviii_view);
                        } else if (strcmp(cmd, "view:accel") == 0) {
                            display_manager_switch_view(&accelerometer_view);
                        } else if (strcmp(cmd, "view:clock") == 0) {
                            display_manager_switch_view(&clock_view);
                        } else if (strcmp(cmd, "view:apps") == 0) {
                            display_manager_switch_view(&apps_menu_view);
                        } else if (strcmp(cmd, "view:nrf24") == 0) {
                            SelectedMenuType = OT_NRF24;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:subghz") == 0) {
                            display_manager_switch_view(&subghz_view);
                        } else if (strcmp(cmd, "view:settings") == 0) {
                            SelectedMenuType = OT_Settings;
                            display_manager_switch_view(&options_menu_view);
                        } else if (strcmp(cmd, "view:ghostlink") == 0) {
                            SelectedMenuType = OT_DualComm;
                            display_manager_switch_view(&options_menu_view);
                        }
                    } else {
                        display_manager_switch_view(&terminal_view);
                        simulateCommand(cmd);
                    }
                } else {
                    InputEvent ev = { .type = INPUT_TYPE_JOYSTICK, .data.joystick_index = 7 };
                    xQueueSend(input_queue, &ev, pdMS_TO_TICKS(10));
                }
            }
            prev_b1 = states.b1;
            prev_b2 = states.b2;
            prev_b3 = states.b3;
        }
    }
#endif
#endif
 #endif

    bool enable_touch_polling = false;
#ifdef CONFIG_JC3248W535EN_LCD
    /* The BSP pointer indev is the sole touch owner on this board. */
    enable_touch_polling = false;
#elif defined(CONFIG_USE_TOUCHSCREEN)
    enable_touch_polling = true;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        enable_touch_polling = true;
    }
#endif
#endif

    if (enable_touch_polling) {

#ifdef CONFIG_JC3248W535EN_LCD
    touch_driver_read_axs15231b(&touch_driver, &touch_data);
#else
    touch_driver_read(&touch_driver, &touch_data);
#endif

    if (touch_data.state == LV_INDEV_STATE_PR && !touch_active) {
      bool skip_event = false;
      last_touch_time = xTaskGetTickCount();
      last_touch_x = touch_data.point.x;
      last_touch_y = touch_data.point.y;
#if GUI_LARGE_TOUCH_UI
      system_edge_touch = touch_data.point.x <= 48 ||
                          touch_data.point.y >= (LV_VER_RES - 48);
      /* Large touch panels use the manual touch queue rather than a registered
       * LVGL indev. Claim top-edge pulls here, before per-view move filtering
       * can discard the trajectory. */
      if (!s_p4_control_center_active && !s_lockscreen_overlay_active &&
          LV_HOR_RES >= 600 && LV_VER_RES >= 400 &&
          touch_data.point.y <= 24) {
        s_p4_control_center_start = touch_data.point;
        s_p4_control_center_gesture = true;
        system_edge_touch = false;
        touch_active = true;
        skip_event = true;
        ESP_LOGD(TAG, "Control Center raw gesture start: x=%d y=%d",
                 (int)touch_data.point.x, (int)touch_data.point.y);
      }
#endif
#ifdef CONFIG_IS_S3TWATCH
      if (was_woken_by_interrupt) {
        was_woken_by_interrupt = false; // Consume the flag
        skip_event = true;
        vTaskDelay(pdMS_TO_TICKS(30)); // Debounce period
      } else
#endif
      if (is_backlight_dimmed || is_backlight_off) {
// Disable tap-to-wake, use button interrupt instead.
#ifndef CONFIG_IS_S3TWATCH
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
        skip_event = true;
        touch_active = true;       // claim this touch so re-polls while held don't re-fire
        skip_next_release = true;  // eat the paired release too
        vTaskDelay(pdMS_TO_TICKS(20));
#endif
      }
      if (!skip_event) {
        touch_active = true;
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.is_touch_move = false;
        event.data.touch_data.point.x = touch_data.point.x;
        event.data.touch_data.point.y = touch_data.point.y;
        event.data.touch_data.state = touch_data.state;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send touch input to queue\n");
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_PR && touch_active && !skip_next_release) {
      if (abs(touch_data.point.x - last_touch_x) >= touch_move_min_delta ||
          abs(touch_data.point.y - last_touch_y) >= touch_move_min_delta) {
        last_touch_time = xTaskGetTickCount();
        if (touch_move_events_enabled_for_current_view()
#if GUI_LARGE_TOUCH_UI
            || s_p4_control_center_active || system_edge_touch
#endif
            ) {
          InputEvent event;
          event.type = INPUT_TYPE_TOUCH;
          event.is_touch_move = true;
          event.data.touch_data = touch_data;
          if (xQueueSend(input_queue, &event, 0) == pdTRUE) {
            last_touch_x = touch_data.point.x;
            last_touch_y = touch_data.point.y;
          }
        } else {
          last_touch_x = touch_data.point.x;
          last_touch_y = touch_data.point.y;
        }
      }
    } else if (touch_data.state == LV_INDEV_STATE_REL && touch_active) {
      last_touch_time = xTaskGetTickCount();
      touch_active = false;
#if GUI_LARGE_TOUCH_UI
      if (s_p4_control_center_gesture) {
        bool opened = (last_touch_y - s_p4_control_center_start.y) >= 80 &&
                      abs(last_touch_x - s_p4_control_center_start.x) <= 360;
        s_p4_control_center_gesture = false;
        if (opened && !s_lockscreen_overlay_active) {
          ESP_LOGI(TAG, "Opening Control Center (raw touch)");
          display_manager_run_on_lvgl(dm_p4_control_center_open_cb, NULL);
        }
        system_edge_touch = false;
        skip_next_release = false;
        continue;
      }
      system_edge_touch = false;
#endif
      if (skip_next_release) {
        skip_next_release = false; // eat the release that paired with the swallowed wake press
      } else {
        InputEvent event;
        event.type = INPUT_TYPE_TOUCH;
        event.is_touch_move = false;
        event.data.touch_data = touch_data;
        if (xQueueSend(input_queue, &event, pdMS_TO_TICKS(10)) != pdTRUE) {
          ESP_LOGE(TAG, "Failed to send touch input to queue\n");
        }
      }
    }

    } // enable_touch_polling

    // backlight dim logic
    uint32_t current_timeout = G_Settings.display_timeout_ms;

    if (current_timeout != UINT32_MAX) { // Only apply dimming logic if timeout is not 'Never'
      TickType_t now = xTaskGetTickCount();

      // Stage 1: Dim after timeout
      if (!is_backlight_dimmed && !is_backlight_off &&
          (now - last_touch_time > pdMS_TO_TICKS(current_timeout))) {
        ESP_LOGD(TAG, "Display timeout reached, dimming backlight (intermediate)");
        set_backlight_brightness(INTERMEDIATE_DIM_PERCENT);
        is_backlight_dimmed = true;
        last_dim_time = now;
      }
      // Stage 2: Turn off after dim duration
      else if (is_backlight_dimmed && !is_backlight_off &&
               (now - last_dim_time > pdMS_TO_TICKS(INTERMEDIATE_DIM_DURATION_MS))) {
        ESP_LOGD(TAG, "Intermediate dim duration elapsed, turning backlight off");
        /* Build the wake-lock overlay now, while the screen is still dark, so
         * it is already the top frame when the backlight comes back — avoids a
         * visible flash of the underlying view on wake. */
        if (settings_get_lockscreen_enabled(&G_Settings) &&
            settings_get_lockscreen_wake_lock(&G_Settings) &&
            !s_lockscreen_overlay_active &&
            dm.current_view && dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
          display_manager_show_lockscreen();
        }
        set_backlight_brightness(0);
        is_backlight_off = true;
      }
      // Wake up on input
      else if ((is_backlight_dimmed || is_backlight_off) &&
               (now - last_touch_time < pdMS_TO_TICKS(current_timeout))) {
        ESP_LOGD(TAG, "Input detected, restoring backlight");
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
      }
    } else if (is_backlight_dimmed || is_backlight_off) { // If timeout is 'Never' and backlight is dimmed/off, set to full brightness
        ESP_LOGD(TAG, "Display timeout set to Never, waking backlight from dimmed/off state.");
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
    }
    //end backlight dim logic
    // When backlight is off (dimmed), poll less frequently to save power
    TickType_t delay = (is_backlight_dimmed ? pdMS_TO_TICKS(BACKLIGHT_SLEEP_POLL_MS) : tick_interval);
    vTaskDelay(delay);
  }

  vTaskDelete(NULL);
}

static bool dm_update_manual_touch_pressed_state(InputEvent *ev) {
    if (ev->type != INPUT_TYPE_TOUCH) return false;
#if GUI_LARGE_TOUCH_UI
    if (ev->is_touch_move) {
        if (s_p4_home_gesture && s_p4_home_indicator) {
            int32_t rise = s_p4_home_start.y - ev->data.touch_data.point.y;
            lv_obj_set_width(s_p4_home_indicator, rise > 24 ? 144 : 112);
        }
        if (s_p4_back_gesture) {
            int32_t dx = ev->data.touch_data.point.x - s_p4_back_start.x;
            int32_t dy = ev->data.touch_data.point.y - s_p4_back_start.y;
            /* Commit the gesture only once the drag is clearly horizontal;
             * vertical movement cancels it so left-edge controls and scroll
             * views do not unexpectedly navigate back. */
            if (abs(dy) > abs(dx) + 24 || dx < -12) {
                s_p4_back_gesture = false;
                dm_p4_back_indicator_cancel();
            } else if (s_p4_back_indicator && dx >= 0) {
                int32_t indicator_width = 8 + dx / 2;
                int32_t indicator_opa = LV_OPA_30 + dx;
                if (indicator_width > 72) indicator_width = 72;
                if (indicator_opa > LV_OPA_90) indicator_opa = LV_OPA_90;
                lv_obj_set_width(s_p4_back_indicator, indicator_width);
                lv_obj_set_style_bg_opa(s_p4_back_indicator,
                                        (lv_opa_t)indicator_opa, 0);
            }
        }
        return false;
    }
#else
    if (ev->is_touch_move) return false;
#endif
    bool consumed = false;
    if (ev->data.touch_data.state == LV_INDEV_STATE_PRESSED) {
#if GUI_LARGE_TOUCH_UI
        s_p4_home_start = ev->data.touch_data.point;
        s_p4_home_gesture = s_p4_home_start.y >= (LV_VER_RES - 48);
        s_p4_back_start = ev->data.touch_data.point;
        /* A generous but still edge-bound activation strip makes the gesture
         * discoverable on a large panel while avoiding ordinary content taps. */
        s_p4_back_gesture = s_p4_back_start.x <= 48;
        if (s_p4_back_gesture) {
            dm_p4_back_indicator_begin(s_p4_back_start);
        }
        if (s_p4_home_gesture && s_p4_home_indicator) {
            lv_obj_set_style_bg_opa(s_p4_home_indicator, LV_OPA_COVER, 0);
        }
#endif
        if (dm_pressed_obj) {
            if (lv_obj_is_valid(dm_pressed_obj)) {
                lv_obj_clear_state(dm_pressed_obj, LV_STATE_PRESSED);
            }
            dm_pressed_obj = NULL;
        }
        lv_point_t pt = ev->data.touch_data.point;
        /* Search overlay layers first so popups on lv_layer_top()/lv_layer_sys()
         * consume the press instead of rows behind them getting styled. */
        lv_obj_t *found = lv_indev_search_obj(lv_layer_top(), &pt);
        if (!found) {
            lv_obj_t *sys = lv_layer_sys();
            if (sys && lv_obj_is_valid(sys)) {
                found = lv_indev_search_obj(sys, &pt);
            }
        }
        if (!found) {
            View *cur = dm.current_view;
            lv_obj_t *root = (cur && cur->root) ? cur->root : lv_scr_act();
            found = lv_indev_search_obj(root, &pt);
        }
        if (found) {
            lv_obj_add_state(found, LV_STATE_PRESSED);
            dm_pressed_obj = found;
        }
    } else {
#if GUI_LARGE_TOUCH_UI
        if (s_p4_home_gesture) {
            lv_point_t end = ev->data.touch_data.point;
            bool swipe_home = (s_p4_home_start.y - end.y) >= 90 &&
                              abs(s_p4_home_start.x - end.x) <= 240;
            s_p4_home_gesture = false;
            if (s_p4_home_indicator) {
                lv_obj_set_width(s_p4_home_indicator, 112);
                lv_obj_set_style_bg_opa(s_p4_home_indicator, LV_OPA_70, 0);
            }
            if (swipe_home && !s_lockscreen_overlay_active &&
                dm.current_view != &splash_view && dm.current_view != &main_menu_view) {
                dm_p4_go_home_through_view();
            }
        }
        if (s_p4_back_gesture) {
            lv_point_t end = ev->data.touch_data.point;
            bool swipe_back = (end.x - s_p4_back_start.x) >= 96 &&
                              abs(end.y - s_p4_back_start.y) <= 96;
            s_p4_back_gesture = false;
            if (swipe_back && !s_lockscreen_overlay_active &&
                dm.current_view != &splash_view && dm.current_view != &main_menu_view) {
                dm_p4_back_indicator_commit();
                /* Deliver the exact same event as physical joystick-left.
                 * The active view decides what left means. */
                InputEvent left = {
                    .type = INPUT_TYPE_JOYSTICK,
                    .data.joystick_index = 0,
                    .data.joystick_pressed = true,
                };
                xQueueSend(input_queue, &left, pdMS_TO_TICKS(10));
                consumed = true;
            } else {
                dm_p4_back_indicator_cancel();
            }
        }
#endif
        if (dm_pressed_obj) {
            if (lv_obj_is_valid(dm_pressed_obj)) {
                lv_obj_clear_state(dm_pressed_obj, LV_STATE_PRESSED);
            }
            dm_pressed_obj = NULL;
        }
    }
    return consumed;
}

#ifdef CONFIG_CROWPANEL_EPAPER_42
static bool dm_handle_crowpanel_home_event(const InputEvent *event) {
    if (!event || event->type != INPUT_TYPE_HOME_BUTTON ||
        !event->data.home_pressed || s_lockscreen_overlay_active) {
        return false;
    }

    View *leaving = NULL;
    void (*input_callback)(InputEvent *) = NULL;
    if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        leaving = dm.current_view;
        if (leaving) input_callback = leaving->input_callback;
        xSemaphoreGive(dm.mutex);
    }

    if (!leaving || leaving == &splash_view || leaving == &lockscreen_view ||
        leaving == &main_menu_view) {
        return true;
    }

    /* Give the active screen its normal Back event first so its existing
     * worker/radio cleanup still runs. Then reset the route, making HOME a
     * true root action rather than one level of back navigation. */
    if (input_callback) {
        InputEvent back = {0};
        back.type = INPUT_TYPE_EXIT_BUTTON;
        back.data.exit_pressed = true;
        input_callback(&back);
    }

    gui_route_t home = {.id = GUI_ROUTE_VIEW, .view = &main_menu_view};
    gui_router_reset(&home);
    return true;
}
#else
static bool dm_handle_crowpanel_home_event(const InputEvent *event) {
    (void)event;
    return false;
}
#endif

void processEvent() {  // do not process events until the display manager is up
  if (!display_manager_init_success) {
    return;
  }

#ifdef CONFIG_CROWPANEL_ADVANCED_P4
  const int max_events = 32;
#else
  const int max_events = 16;
#endif
  int processed = 0;
  InputEvent event;

  while (processed < max_events && xQueueReceive(input_queue, &event, 0) == pdTRUE) {
    if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move) {
      InputEvent next_event;
      while (xQueuePeek(input_queue, &next_event, 0) == pdTRUE &&
             next_event.type == INPUT_TYPE_TOUCH && next_event.is_touch_move) {
        xQueueReceive(input_queue, &event, 0);
      }
    }
    last_touch_time = xTaskGetTickCount();
    if (is_backlight_dimmed || is_backlight_off) {
      set_backlight_brightness(100);
      is_backlight_dimmed = false;
      is_backlight_off = false;
      processed++;
      continue;
    }
    if (dm_p4_control_center_handle_input(&event)) {
      processed++;
      continue;
    }
    if (dm_update_manual_touch_pressed_state(&event)) {
      processed++;
      continue;
    }
    if (crash_reporter_handle_input(&event)) {
      processed++;
      continue;
    }
    if (dm_handle_crowpanel_home_event(&event)) {
      processed++;
      continue;
    }
    if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
      View *current = dm.current_view;
      void (*input_callback)(InputEvent *) = NULL;
      const char *view_name = "NULL";

      if (current) {
        view_name = current->name;
        input_callback = current->input_callback;
      } else {
        ESP_LOGW(TAG, "Current view is NULL in input_processing_task\n");
      }

      /* While the lockscreen overlay is up, the underlying view is still the
       * "current" view (so its capture keeps running) but all input belongs to
       * the lockscreen. */
      if (s_lockscreen_overlay_active) {
        current = &lockscreen_view;
        input_callback = lockscreen_view.input_callback;
        view_name = lockscreen_view.name;
      }

      xSemaphoreGive(dm.mutex);

      ESP_LOGD(TAG, "Input event type: %d, Current view: %s\n", event.type, view_name);
      bool accepts_touch_move = touch_move_events_enabled_for_view_name(view_name);
      if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move && !accepts_touch_move) {
        processed++;
        continue;
      }
      if (!dm_joystick_event_matches_view(&event, current)) {
        processed++;
        continue;
      }
      // Keyboard, trackpad, and native apps need release events for held controls.
      // Other views only check joystick_index and would double-fire on release.
      if (event.type == INPUT_TYPE_JOYSTICK && !event.data.joystick_pressed &&
          strcmp(view_name, "Keyboard Screen") != 0 &&
          strcmp(view_name, "Trackpad") != 0 &&
          strcmp(view_name, "SD App") != 0) {
        processed++;
        continue;
      }
      // Global keyboard repeat: start on press, stop on release.
      // Release events are consumed here for all views except Trackpad
      // (which needs them for tap/hold click detection).
      if (event.type == INPUT_TYPE_KEYBOARD) {
          if (event.is_touch_move) {
              kb_repeat_stop();
              if (strcmp(view_name, "Trackpad") != 0) {
                  processed++;
                  continue;
              }
          } else {
              kb_repeat_start(event.data.key_value);
          }
      }
      if (input_callback) input_callback(&event);
    }
    processed++;
  }

  if (processed == 0) {
    if (xQueueReceive(input_queue, &event, pdMS_TO_TICKS(1)) == pdTRUE) {
      if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move) {
        InputEvent next_event;
        while (xQueuePeek(input_queue, &next_event, 0) == pdTRUE &&
               next_event.type == INPUT_TYPE_TOUCH && next_event.is_touch_move) {
          xQueueReceive(input_queue, &event, 0);
        }
      }
      last_touch_time = xTaskGetTickCount();
      if (is_backlight_dimmed || is_backlight_off) {
        set_backlight_brightness(100);
        is_backlight_dimmed = false;
        is_backlight_off = false;
        return;
      }
      if (dm_p4_control_center_handle_input(&event)) {
        return;
      }
      if (dm_update_manual_touch_pressed_state(&event)) {
        return;
      }
      if (crash_reporter_handle_input(&event)) {
        return;
      }
      if (dm_handle_crowpanel_home_event(&event)) {
        return;
      }
      if (xSemaphoreTake(dm.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE) {
        View *current = dm.current_view;
        void (*input_callback)(InputEvent *) = NULL;
        const char *view_name = "NULL";

        if (current) {
          view_name = current->name;
          input_callback = current->input_callback;
        } else {
          ESP_LOGW(TAG, "Current view is NULL in input_processing_task\n");
        }

        /* See note above: lockscreen overlay owns input while it's up. */
        if (s_lockscreen_overlay_active) {
          current = &lockscreen_view;
          input_callback = lockscreen_view.input_callback;
          view_name = lockscreen_view.name;
        }

        xSemaphoreGive(dm.mutex);

        ESP_LOGD(TAG, "Input event type: %d, Current view: %s\n", event.type, view_name);
        bool accepts_touch_move = touch_move_events_enabled_for_view_name(view_name);
        if (event.type == INPUT_TYPE_TOUCH && event.is_touch_move && !accepts_touch_move) {
          // drop live move samples for views that still treat pressed touches as clicks
        } else if (!dm_joystick_event_matches_view(&event, current)) {
          // A release belongs to the view where its press began.
        } else if (event.type == INPUT_TYPE_JOYSTICK && !event.data.joystick_pressed &&
            strcmp(view_name, "Keyboard Screen") != 0 &&
            strcmp(view_name, "Trackpad") != 0 &&
            strcmp(view_name, "SD App") != 0) {
          // drop release event for non-keyboard/non-trackpad views
        } else if (event.type == INPUT_TYPE_KEYBOARD && event.is_touch_move) {
          // keyboard release: stop global repeat, forward only to Trackpad
          kb_repeat_stop();
          if (strcmp(view_name, "Trackpad") != 0) {
              // drop for all other views
          } else if (input_callback) {
              input_callback(&event);
          }
        } else {
          if (event.type == INPUT_TYPE_KEYBOARD && !event.is_touch_move) {
              kb_repeat_start(event.data.key_value);
          }
          if (input_callback) input_callback(&event);
        }
      }
    }
  }

  /* Apply live drag scroll at a steady frame budget instead of every input tick. */
  display_manager_flush_pending_scroll_if_due();
}

/* ---- scroll coalescing ------------------------------------------------- */

#define SCROLL_COALESCE_MAX_STEP 64
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
#define SCROLL_FLUSH_INTERVAL_MS 5
#else
#define SCROLL_FLUSH_INTERVAL_MS 16
#endif

typedef struct {
  lv_obj_t *target;
  int32_t dy;
} pending_scroll_t;

static pending_scroll_t s_pending_scroll = { NULL, 0 };
static TickType_t s_last_scroll_flush_tick = 0;

static void display_manager_cancel_pending_scroll(void) {
  s_pending_scroll.target = NULL;
  s_pending_scroll.dy = 0;
}

void display_manager_queue_scroll(lv_obj_t *target, int32_t dy) {
  if (!target || dy == 0) return;
  if (s_pending_scroll.target && s_pending_scroll.target != target) {
    /* Different target - flush the previous one so we never lose it. */
    display_manager_flush_pending_scroll();
  }
  s_pending_scroll.target = target;
  int32_t new_dy = s_pending_scroll.dy + dy;
  if (new_dy >  SCROLL_COALESCE_MAX_STEP) new_dy =  SCROLL_COALESCE_MAX_STEP;
  if (new_dy < -SCROLL_COALESCE_MAX_STEP) new_dy = -SCROLL_COALESCE_MAX_STEP;
  s_pending_scroll.dy = new_dy;
}

void display_manager_flush_pending_scroll(void) {
  lv_obj_t *target = s_pending_scroll.target;
  int32_t dy = s_pending_scroll.dy;
  display_manager_cancel_pending_scroll();

  if (target && dy != 0 && lv_obj_is_valid(target)) {
    lv_obj_scroll_by_bounded(target, 0, dy, LV_ANIM_OFF);
    s_last_scroll_flush_tick = xTaskGetTickCount();
  }
}

static void display_manager_flush_pending_scroll_if_due(void) {
  if (!s_pending_scroll.target || s_pending_scroll.dy == 0) return;

  TickType_t now = xTaskGetTickCount();
  if (s_last_scroll_flush_tick == 0 ||
      now - s_last_scroll_flush_tick >= pdMS_TO_TICKS(SCROLL_FLUSH_INTERVAL_MS)) {
    display_manager_flush_pending_scroll();
  }
}

/* ---- touch drag state machine ---------------------------------------- */

#define TOUCH_DRAG_AXIS_THRESHOLD 10
#define TOUCH_DRAG_AXIS_BIAS 4
#define TOUCH_DRAG_DEADZONE 1
#define TOUCH_DRAG_MAX_STEP 64

void touch_drag_reset(touch_drag_t *d) {
    d->started = false;
    d->dragged = false;
    d->drag_axis = 0;
    d->start_x = d->start_y = 0;
    d->last_x = d->last_y = 0;
    d->release_target = NULL;
}

void touch_drag_begin(touch_drag_t *d, const lv_indev_data_t *data) {
    d->started = true;
    d->dragged = false;
    d->drag_axis = 0;
    d->start_x = data->point.x;
    d->start_y = data->point.y;
    d->last_x = data->point.x;
    d->last_y = data->point.y;
    d->release_target = NULL;
}

static int32_t _touch_drag_clamp(int32_t dy) {
    if (abs(dy) <= TOUCH_DRAG_DEADZONE) return 0;
    if (dy >  TOUCH_DRAG_MAX_STEP) return  TOUCH_DRAG_MAX_STEP;
    if (dy < -TOUCH_DRAG_MAX_STEP) return -TOUCH_DRAG_MAX_STEP;
    return dy;
}

static int _touch_drag_resolve_axis(int total_dx, int total_dy) {
    int abs_dx = abs(total_dx);
    int abs_dy = abs(total_dy);
    if (abs_dx < TOUCH_DRAG_AXIS_THRESHOLD && abs_dy < TOUCH_DRAG_AXIS_THRESHOLD) return 0;
    if (abs_dy >= abs_dx + TOUCH_DRAG_AXIS_BIAS) return 1;
    if (abs_dx >= abs_dy + TOUCH_DRAG_AXIS_BIAS) return 2;
    return 0;
}

lv_obj_t *touch_drag_update(touch_drag_t *d, const lv_indev_data_t *data, lv_obj_t *scroll_target) {
    if (!d->started) return NULL;

    int32_t dy = data->point.y - d->last_y;
    d->last_x = data->point.x;
    d->last_y = data->point.y;

    if (!d->dragged) {
        d->drag_axis = _touch_drag_resolve_axis(data->point.x - d->start_x,
                                                data->point.y - d->start_y);
        d->dragged = d->drag_axis != 0;
    }

    if (d->dragged && d->drag_axis == 1 && scroll_target) {
        bool live = settings_get_touch_drag_scroll(&G_Settings);
        if (live) {
            int32_t clamped = _touch_drag_clamp(dy);
            if (clamped) display_manager_queue_scroll(scroll_target, clamped);
        } else {
            // Remember the target; the scroll is applied on release
            // using the total drag distance.
            d->release_target = scroll_target;
        }
    }
    return d->dragged ? scroll_target : NULL;
}

bool touch_drag_release(touch_drag_t *d, const lv_indev_data_t *data) {
    if (!d->started) return false;
    bool was_dragged = d->dragged;
    int32_t total_dy = data->point.y - d->start_y;
    lv_obj_t *target = d->release_target;
    bool live = settings_get_touch_drag_scroll(&G_Settings);
    touch_drag_reset(d);
    if (was_dragged && target && !live && total_dy) {
        display_manager_queue_scroll(target, total_dy);
        return true;
    }
    return was_dragged;
}

void lvgl_tick_task(void *arg) {
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
  const uint32_t max_idle_delay_ms = 5;
#elif defined(CONFIG_USE_C5_PARLIO_DISPLAY)
  const uint32_t max_idle_delay_ms = 5;
#else
  const uint32_t max_idle_delay_ms = 10;
#endif
  TickType_t last_mon = 0;
#if !LV_TICK_CUSTOM
  TickType_t last_tick_time = xTaskGetTickCount();
#endif
  while (1) {
      /* Cooperative quiesce point: if a shared-SPI consumer (SD) has asked us
       * to park, acknowledge here — outside any flush — and spin without
       * touching the panel until the bus is handed back. This guarantees the
       * suspender never frees the SPI bus mid-transaction. */
      if (s_lvgl_gate_closed) {
          s_lvgl_gate_parked = true;
          while (s_lvgl_gate_closed) {
              vTaskDelay(pdMS_TO_TICKS(5));
          }
          s_lvgl_gate_parked = false;
      }

#if !LV_TICK_CUSTOM
      TickType_t pass_start = xTaskGetTickCount();
      uint32_t elapsed_ms = (uint32_t)(pass_start - last_tick_time) * portTICK_PERIOD_MS;
      /* A long shared-SPI park must not slingshot animations to their end. */
      if (elapsed_ms > 100) elapsed_ms = 100;
      if (elapsed_ms > 0) lv_tick_inc(elapsed_ms);
      last_tick_time = pass_start;
#endif

      processEvent();
      /* Hold the same recursive mutex background tasks take in
       * display_manager_lvgl_async_call() for the whole timer/render pass, so a
       * concurrent lv_async_call() from another task can never mutate LVGL's
       * timer list or internal heap while this task is walking/allocating in it.
       * portMAX_DELAY is safe here: the only other holders are brief enqueue
       * calls that always release promptly. */
      if (s_lvgl_call_mutex) xSemaphoreTakeRecursive(s_lvgl_call_mutex, portMAX_DELAY);
      uint32_t next_timer_ms = lv_timer_handler();
      if (s_lvgl_call_mutex) xSemaphoreGiveRecursive(s_lvgl_call_mutex);
      TickType_t now = xTaskGetTickCount();
      // Monitor input queue backlog periodically
      if (now - last_mon >= pdMS_TO_TICKS(500)) {
          UBaseType_t pending = uxQueueMessagesWaiting((QueueHandle_t)input_queue);
          if (pending > 0) {
              ESP_LOGW(TAG, "lvgl_tick: input_queue backlog=%u", (unsigned)pending);
          }
          last_mon = now;
      }

      // LVGL pool pressure snapshot every 30s: max_used tells us how far the
      // pool can be shrunk without any functional loss.
      {
          static TickType_t last_lv_mem_log;
          if (now - last_lv_mem_log >= pdMS_TO_TICKS(30000)) {
              last_lv_mem_log = now;
              lv_mem_monitor_t mmon;
              lv_mem_monitor(&mmon);
              ESP_LOGI(TAG, "lvgl pool: total=%u free=%u peak=%u frag=%u%%",
                       (unsigned)mmon.total_size, (unsigned)mmon.free_size,
                       (unsigned)mmon.max_used, (unsigned)mmon.frag_pct);
          }
      }

      // Auto-lock timeout check
      if (settings_get_lockscreen_enabled(&G_Settings)) {
          uint16_t auto_lock_sec = settings_get_lockscreen_timeout_sec(&G_Settings);
          if (auto_lock_sec > 0) {
              if (now - last_touch_time > pdMS_TO_TICKS(auto_lock_sec * 1000u)) {
                  if (!s_lockscreen_overlay_active &&
                      dm.current_view && dm.current_view != &lockscreen_view && dm.current_view != &splash_view) {
                      display_manager_show_lockscreen();
                  }
              }
          }
      }

#if LV_TICK_CUSTOM
      // The independent clock advances during rendering/waiting, so LVGL's
      // returned delay already accounts for work. Do not subtract it twice.
      uint32_t sleep_ms = next_timer_ms;
#else
      uint32_t work_ms = (uint32_t)(xTaskGetTickCount() - pass_start) * portTICK_PERIOD_MS;
      uint32_t sleep_ms = next_timer_ms > work_ms ? next_timer_ms - work_ms : 0;
#endif
      if (sleep_ms > max_idle_delay_ms) sleep_ms = max_idle_delay_ms;
      if (sleep_ms > 0) {
          vTaskDelay(pdMS_TO_TICKS(sleep_ms));
      } else {
          vTaskDelay(1);
      }
  }
  vTaskDelete(NULL);
}


#ifdef CONFIG_USE_TDECK
void set_keyboard_brightness(uint8_t brightness) {
    uint8_t kb_brightness[2] = {LILYGO_KB_BRIGHTNESS_CMD, brightness};
    ESP_LOGI(TAG, "Setting keyboard brightness to %d", brightness);
    // Write the brightness command to the keyboard
    lvgl_i2c_write(CONFIG_LV_I2C_TOUCH_PORT, LILYGO_KB_SLAVE_ADDRESS, 0x00, kb_brightness, 2);
}
#endif
