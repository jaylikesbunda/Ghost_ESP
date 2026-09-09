#include "vendor/drivers/ST7262.h"

#ifdef CONFIG_USE_7_INCHER

#pragma message("Compiling 7 Incher")

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#ifdef CONFIG_CROWPANEL_ADVANCE_RGB_LCD
#include "src/draw/sw/lv_draw_sw_s3_simd.h"
#endif

static const char *TAG = "lcd_st7262";

// Panel handle
static esp_lcd_panel_handle_t rgb_panel_handle = NULL;

// LVGL display driver
static lv_disp_drv_t disp_drv;

// Semaphores for synchronization
static SemaphoreHandle_t sem_vsync_end = NULL;
static SemaphoreHandle_t sem_gui_ready = NULL;

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
#if LV_COLOR_DEPTH != 16 || LV_COLOR_16_SWAP
#error "CrowPanel RGB framebuffers require native (not byte-swapped) RGB565"
#endif
#if defined(CONFIG_CROWPANEL_ADVANCE_5_LCD) || defined(CONFIG_CROWPANEL_ADVANCE_7_LCD)
#define CROWPANEL_PCLK_HZ 16000000U
#else
#define CROWPANEL_PCLK_HZ 21000000U
#endif
#ifndef GHOST_CROWPANEL_RGB_FACTORY_DMA
#error "CrowPanel requires its board-local RGB DMA adaptation from CMake"
#endif
#ifndef CONFIG_LCD_RGB_RESTART_IN_VSYNC
#error "CrowPanel factory-style scanout requires CONFIG_LCD_RGB_RESTART_IN_VSYNC"
#endif
// Two immutable circular GDMA chains. LVGL renders directly into the inactive
// PSRAM framebuffer; VSYNC alone chooses which chain becomes scanout. This keeps
// the factory transport settings without writing into the active framebuffer.
static lv_color_t *crowpanel_framebuffers[2];
static SemaphoreHandle_t crowpanel_frame_done;
static lv_color_t *crowpanel_pending_fb;
static portMUX_TYPE crowpanel_frame_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_disp_t *crowpanel_display;
static crowpanel_rgb_stats_t crowpanel_stats = {
    .requested_pclk_hz = CROWPANEL_PCLK_HZ,
};
static int64_t crowpanel_last_vsync_us;
static uint32_t crowpanel_expected_frame_us = 19524; // 820 * 500 / 21 MHz
static bool crowpanel_flush_active;
static int64_t crowpanel_render_start_us;

extern bool crowpanel_rgb_scanout_uses_buffer(esp_lcd_panel_handle_t panel,
                                              const void *buffer);

static esp_err_t crowpanel_sync_rect(const lv_color_t *framebuffer,
                                     const lv_area_t *area) {
  const size_t width_bytes = (size_t)(area->x2 - area->x1 + 1) * sizeof(lv_color_t);
  if (area->x1 == 0 && area->x2 == 799) {
    return esp_cache_msync((void *)&framebuffer[(size_t)area->y1 * 800],
                           (size_t)(area->y2 - area->y1 + 1) * 800 * sizeof(lv_color_t),
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  }
  for (int y = area->y1; y <= area->y2; ++y) {
    esp_err_t ret = esp_cache_msync(
        (void *)&framebuffer[(size_t)y * 800 + area->x1], width_bytes,
        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (ret != ESP_OK) return ret;
  }
  return ESP_OK;
}

static void crowpanel_sync_copied_area(lv_disp_drv_t *drv, void *buffer,
                                       const lv_area_t *area) {
  (void)drv;
  // LVGL's direct-mode coherence copy is not part of the current invalidated
  // flush list. Publish it here so a later framebuffer handoff cannot expose
  // stale PSRAM outside the freshly rendered rectangles.
  ESP_ERROR_CHECK(crowpanel_sync_rect((const lv_color_t *)buffer, area));
}

static void crowpanel_render_start(lv_disp_drv_t *drv) {
  crowpanel_render_start_us = esp_timer_get_time();
}

static bool IRAM_ATTR crowpanel_vsync(esp_lcd_panel_handle_t panel,
                                     const esp_lcd_rgb_panel_event_data_t *edata,
                                     void *user_ctx) {
  // Called AFTER DMA restart. Never print, allocate, or read hardware here.
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&crowpanel_frame_lock);
  if (crowpanel_last_vsync_us) {
    const uint32_t interval = (uint32_t)(now - crowpanel_last_vsync_us);
    if (!crowpanel_stats.frame_min_us || interval < crowpanel_stats.frame_min_us)
      crowpanel_stats.frame_min_us = interval;
    if (interval > crowpanel_stats.frame_max_us) crowpanel_stats.frame_max_us = interval;
    if (interval > crowpanel_expected_frame_us + 500) ++crowpanel_stats.long_frames;
    if (interval + 500 < crowpanel_expected_frame_us) ++crowpanel_stats.short_frames;
  }
  crowpanel_last_vsync_us = now;
  if (crowpanel_flush_active) ++crowpanel_stats.vsync_during_flush;
  bool released = false;
  if (crowpanel_pending_fb &&
      crowpanel_rgb_scanout_uses_buffer(panel, crowpanel_pending_fb)) {
    crowpanel_pending_fb = NULL;
    ++crowpanel_stats.presented_frames;
    released = true;
  }
  ++crowpanel_stats.frames;
  portEXIT_CRITICAL_ISR(&crowpanel_frame_lock);
  BaseType_t task_woken = pdFALSE;
  if (released) xSemaphoreGiveFromISR(crowpanel_frame_done, &task_woken);
  return task_woken == pdTRUE;
}
#endif

// Data lines D0 to D15
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
// Elecrow CrowPanel Advance 4.3/5/7-inch HMI RGB panels share this bus map.
static const int lcd_data_gpio_nums[] = {
    GPIO_NUM_21, // D0 - B0
    GPIO_NUM_47, // D1 - B1
    GPIO_NUM_48, // D2 - B2
    GPIO_NUM_45, // D3 - B3
    GPIO_NUM_38, // D4 - B4
    GPIO_NUM_9,  // D5 - G0
    GPIO_NUM_10, // D6 - G1
    GPIO_NUM_11, // D7 - G2
    GPIO_NUM_12, // D8 - G3
    GPIO_NUM_13, // D9 - G4
    GPIO_NUM_14, // D10 - G5
    GPIO_NUM_7,  // D11 - R0
    GPIO_NUM_17, // D12 - R1
    GPIO_NUM_18, // D13 - R2
    GPIO_NUM_3,  // D14 - R3
    GPIO_NUM_46, // D15 - R4
};

#define LCD_HSYNC_GPIO_NUM GPIO_NUM_40
#define LCD_VSYNC_GPIO_NUM GPIO_NUM_41
#define LCD_DE_GPIO_NUM GPIO_NUM_42
#define LCD_PCLK_GPIO_NUM GPIO_NUM_39
#define LCD_DISP_GPIO_NUM GPIO_NUM_NC
#define LCD_BACKLIGHT_GPIO GPIO_NUM_NC
#define LCD_RESET_GPIO GPIO_NUM_NC

#elif defined(CONFIG_Crowtech_LCD)
// Crowtech display (formerly Sasquatch display)
static const int lcd_data_gpio_nums[] = {
    GPIO_NUM_15, // D0 - B0
    GPIO_NUM_7,  // D1 - B1
    GPIO_NUM_6,  // D2 - B2
    GPIO_NUM_5,  // D3 - B3
    GPIO_NUM_4,  // D4 - B4
    GPIO_NUM_9,  // D5 - G0
    GPIO_NUM_46, // D6 - G1
    GPIO_NUM_3,  // D7 - G2
    GPIO_NUM_8,  // D8 - G3
    GPIO_NUM_16, // D9 - G4
    GPIO_NUM_1,  // D10 - G5
    GPIO_NUM_14, // D11 - R0
    GPIO_NUM_21, // D12 - R1
    GPIO_NUM_47, // D13 - R2
    GPIO_NUM_48, // D14 - R3
    GPIO_NUM_45  // D15 - R4
};

// Control signals for Crowtech display
#define LCD_HSYNC_GPIO_NUM GPIO_NUM_39
#define LCD_VSYNC_GPIO_NUM GPIO_NUM_40
#define LCD_DE_GPIO_NUM GPIO_NUM_41
#define LCD_PCLK_GPIO_NUM GPIO_NUM_0
#define LCD_DISP_GPIO_NUM -1 // Not used
#define LCD_BACKLIGHT_GPIO GPIO_NUM_2
#define LCD_RESET_GPIO GPIO_NUM_4 // Corrected to GPIO4

#endif

#ifdef CONFIG_Waveshare_LCD
// Waveshare display
static const int lcd_data_gpio_nums[] = {
    GPIO_NUM_14, // D0 - B3
    GPIO_NUM_38, // D1 - B4
    GPIO_NUM_18, // D2 - B5
    GPIO_NUM_17, // D3 - B6
    GPIO_NUM_10, // D4 - B7
    GPIO_NUM_39, // D5 - G2
    GPIO_NUM_0,  // D6 - G3
    GPIO_NUM_45, // D7 - G4
    GPIO_NUM_48, // D8 - G5
    GPIO_NUM_47, // D9 - G6
    GPIO_NUM_21, // D10 - G7
    GPIO_NUM_1,  // D11 - R3
    GPIO_NUM_2,  // D12 - R4
    GPIO_NUM_42, // D13 - R5
    GPIO_NUM_41, // D14 - R6
    GPIO_NUM_40  // D15 - R7
};

// Control signals for Waveshare display
#define LCD_HSYNC_GPIO_NUM GPIO_NUM_46
#define LCD_VSYNC_GPIO_NUM GPIO_NUM_3
#define LCD_DE_GPIO_NUM GPIO_NUM_5
#define LCD_PCLK_GPIO_NUM GPIO_NUM_7
#define LCD_DISP_GPIO_NUM -1 // Not used
#define LCD_BACKLIGHT_GPIO -1 // Not used
#define LCD_RESET_GPIO GPIO_NUM_4 // Corrected to GPIO4

#endif

#ifdef CONFIG_Sunton_LCD

static const int lcd_data_gpio_nums[] = {
    GPIO_NUM_8,  // D0 - B0
    GPIO_NUM_3,  // D1 - B1
    GPIO_NUM_46, // D2 - B2
    GPIO_NUM_9,  // D3 - B3
    GPIO_NUM_1,  // D4 - B4
    GPIO_NUM_5,  // D5 - G0
    GPIO_NUM_6,  // D6 - G1
    GPIO_NUM_7,  // D7 - G2
    GPIO_NUM_15, // D8 - G3
    GPIO_NUM_16, // D9 - G4
    GPIO_NUM_4,  // D10 - G5
    GPIO_NUM_21, // D11 - R3
    GPIO_NUM_14, // D12 - R4
    GPIO_NUM_47, // D13 - R2
    GPIO_NUM_48, // D14 - R1
    GPIO_NUM_45  // D15 - R0
};

#define LCD_HSYNC_GPIO_NUM GPIO_NUM_39
#define LCD_VSYNC_GPIO_NUM GPIO_NUM_41
#define LCD_DE_GPIO_NUM GPIO_NUM_40
#define LCD_PCLK_GPIO_NUM GPIO_NUM_42
#define LCD_DISP_GPIO_NUM GPIO_NUM_NC // Not connected
#define LCD_BACKLIGHT_GPIO GPIO_NUM_2 // Backlight
#define LCD_RESET_GPIO GPIO_NUM_4 // Reset

#endif

// SPI pins for control interface (if used)
#define LCD_SPI_CS_GPIO_NUM GPIO_NUM_13 // Adjust as per your hardware
#define LCD_SPI_SCK_GPIO_NUM GPIO_NUM_12
#define LCD_SPI_MOSI_GPIO_NUM GPIO_NUM_11
#define LCD_SPI_CLK_FREQ_HZ (40 * 1000 * 1000) // 10MHz

static esp_lcd_panel_io_handle_t io_handle = NULL;

// LVGL flush callback
static void lcd_st7262_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                                     lv_color_t *color_map) {
  esp_err_t ret;
  esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;

#if LV_COLOR_16_SWAP
  size_t num_pixels = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);

  for (size_t i = 0; i < num_pixels; i++) {
    uint16_t pixel = color_map[i].full;
    color_map[i].full = (pixel >> 8) | (pixel << 8);
  }
#endif

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  const int64_t flush_start = esp_timer_get_time();
  const uint32_t render_us = (uint32_t)(flush_start - crowpanel_render_start_us);
  portENTER_CRITICAL(&crowpanel_frame_lock);
  crowpanel_flush_active = true;
  if (lv_disp_flush_is_last(drv)) {
    crowpanel_stats.render_total_us += render_us;
    if (render_us > crowpanel_stats.render_max_us) crowpanel_stats.render_max_us = render_us;
  }
  portEXIT_CRITICAL(&crowpanel_frame_lock);
  if (!lv_disp_flush_is_last(drv)) {
    // Direct mode keeps one full framebuffer for the whole refresh. Publish
    // dirty cache lines now, but do not arm its DMA chain until the final area.
    ret = crowpanel_sync_rect(color_map, area);
  } else {
    while (xSemaphoreTake(crowpanel_frame_done, 0) == pdTRUE) {}
    portENTER_CRITICAL(&crowpanel_frame_lock);
    crowpanel_pending_fb = color_map;
    portEXIT_CRITICAL(&crowpanel_frame_lock);
    // The buffer is driver-owned, so this performs no pixel copy. It syncs the
    // final dirty area and selects the pending framebuffer index; the immutable
    // descriptor chain itself is changed only by the following VSYNC ISR.
    ret = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1,
                                    area->y2 + 1, color_map);
    if (ret == ESP_OK) {
      while (xSemaphoreTake(crowpanel_frame_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        portENTER_CRITICAL(&crowpanel_frame_lock);
        ++crowpanel_stats.present_wait_timeouts;
        portEXIT_CRITICAL(&crowpanel_frame_lock);
        ESP_LOGW(TAG, "Waiting for VSYNC framebuffer handoff; active buffer remains protected");
      }
    } else {
      portENTER_CRITICAL(&crowpanel_frame_lock);
      crowpanel_pending_fb = NULL;
      portEXIT_CRITICAL(&crowpanel_frame_lock);
    }
  }
#else
  ret = esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1,
                                  area->y2 + 1, color_map);
#endif
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  const uint32_t flush_us = (uint32_t)(esp_timer_get_time() - flush_start);
  portENTER_CRITICAL(&crowpanel_frame_lock);
  crowpanel_flush_active = false;
  ++crowpanel_stats.flushes;
  if (ret != ESP_OK) ++crowpanel_stats.flush_errors;
  crowpanel_stats.flush_total_us += flush_us;
  crowpanel_stats.pixels += (uint32_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
  if (flush_us > crowpanel_stats.flush_max_us) crowpanel_stats.flush_max_us = flush_us;
  crowpanel_stats.last_x1 = area->x1;
  crowpanel_stats.last_y1 = area->y1;
  crowpanel_stats.last_x2 = area->x2;
  crowpanel_stats.last_y2 = area->y2;
  portEXIT_CRITICAL(&crowpanel_frame_lock);
#endif
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to draw bitmap to panel");
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
    // Direct double buffering cannot safely recycle a buffer after a failed
    // cache publication/submission; stop instead of exposing the active frame.
    ESP_ERROR_CHECK(ret);
#endif
  }

  // Inform LVGL that flushing is done
  lv_disp_flush_ready(drv);
}

static esp_err_t lcd_st7262_init_panel(void) {
  esp_err_t ret = ESP_OK;

  ESP_LOGI(TAG, "Initializing ST7262 LCD panel");

  // Initialize reset GPIO
  if (LCD_RESET_GPIO >= 0) {
    gpio_config_t reset_conf = {
        // Keep the shift well-defined for boards that report GPIO_NUM_NC.
        // This initializer is compiled even though the surrounding runtime
        // check skips reset configuration for those boards.
        .pin_bit_mask = 1ULL << ((LCD_RESET_GPIO < 0) ? 0 : LCD_RESET_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_conf));

    // Perform reset
    gpio_set_level(LCD_RESET_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(LCD_RESET_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
  }

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  // Current 5/7-inch factory sources use 16 MHz. The TCA9534-based 4.3-inch
  // factory board uses 21 MHz.
  int ClockFrequency = CROWPANEL_PCLK_HZ / 1000000U;
#elif defined(CONFIG_Crowtech_LCD)
  int ClockFrequency = 15;
#elif CONFIG_Waveshare_LCD
  int ClockFrequency = 25;
#elif CONFIG_Sunton_LCD
  int ClockFrequency = 18;
#else
  int ClockFrequency = 10;
#endif

#ifdef CONFIG_USE_7_INCHER

  // Prepare RGB panel configuration with accurate timings
  esp_lcd_rgb_panel_config_t panel_config = {
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
      .clk_src = LCD_CLK_SRC_PLL240M,
#else
      .clk_src = LCD_CLK_SRC_PLL160M,
#endif
      .timings =
          {
              .pclk_hz = ClockFrequency * 1000 *
                         1000, // Pixel clock frequency based on the typical 25
                               // MHz from datasheet
              .h_res = 800,
              .v_res = 480,
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
              .hsync_back_porch = 8,
              .hsync_front_porch = 8,
              .hsync_pulse_width = 4,
              .vsync_back_porch = 8,
              .vsync_front_porch = 8,
              .vsync_pulse_width = 4,
#else
              .hsync_back_porch = 4,
              .hsync_front_porch = 4,
              .hsync_pulse_width = 2,
              .vsync_back_porch = 4,
              .vsync_front_porch = 4,
              .vsync_pulse_width = 2,
#endif
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
              // LovyanGFX maps its polarity=0 to idle_low=true.
              .flags.hsync_idle_low = true,
              .flags.vsync_idle_low = true,
              .flags.pclk_active_neg = true,
              // Shared factory Bus_RGB.cpp sets lcd_ck_idle_edge=false.
              // Its misleading pclk_idle_high option controls the OUTPUT
              // edge (pclk_active_neg above), not this idle level.
              .flags.pclk_idle_high = false,
#else
              .flags.pclk_active_neg =
                  true, // Use as per your display’s requirements
#endif
          },
      .data_width = 16,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)
      .psram_trans_align = 64,
#endif
      .hsync_gpio_num = LCD_HSYNC_GPIO_NUM,
      .vsync_gpio_num = LCD_VSYNC_GPIO_NUM,
      .de_gpio_num = LCD_DE_GPIO_NUM,
      .pclk_gpio_num = LCD_PCLK_GPIO_NUM,
      .disp_gpio_num = LCD_DISP_GPIO_NUM,
      .data_gpio_nums =
          {
              [0] = lcd_data_gpio_nums[0],
              [1] = lcd_data_gpio_nums[1],
              [2] = lcd_data_gpio_nums[2],
              [3] = lcd_data_gpio_nums[3],
              [4] = lcd_data_gpio_nums[4],
              [5] = lcd_data_gpio_nums[5],
              [6] = lcd_data_gpio_nums[6],
              [7] = lcd_data_gpio_nums[7],
              [8] = lcd_data_gpio_nums[8],
              [9] = lcd_data_gpio_nums[9],
              [10] = lcd_data_gpio_nums[10],
              [11] = lcd_data_gpio_nums[11],
              [12] = lcd_data_gpio_nums[12],
              [13] = lcd_data_gpio_nums[13],
              [14] = lcd_data_gpio_nums[14],
              [15] = lcd_data_gpio_nums[15],
          },
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
      // Two direct PSRAM framebuffers, each with its own immutable factory-style
      // circular chain. VSYNC chooses the chain; no bounce-refill deadlines.
      .flags.fb_in_psram = true,
      .num_fbs = 2,
      .bounce_buffer_size_px = 0,
      .dma_burst_size = 64,
#else
      .flags.fb_in_psram = true,
      .num_fbs = 2, // Use double buffering
      .bounce_buffer_size_px = 20 * 480,
#endif
  };

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  crowpanel_frame_done = xSemaphoreCreateBinary();
  ESP_RETURN_ON_FALSE(crowpanel_frame_done, ESP_ERR_NO_MEM, TAG,
                      "Cannot allocate CrowPanel VSYNC handoff semaphore");
  crowpanel_pending_fb = NULL;
  crowpanel_flush_active = false;
  crowpanel_stats = (crowpanel_rgb_stats_t){.requested_pclk_hz = CROWPANEL_PCLK_HZ};
  lcd_st7262_reset_rgb_stats();
#endif

  // Create RGB panel
  ret = esp_lcd_new_rgb_panel(&panel_config, &rgb_panel_handle);
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to create RGB panel");

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(rgb_panel_handle, 2,
      (void **)&crowpanel_framebuffers[0], (void **)&crowpanel_framebuffers[1]),
      TAG, "Cannot obtain CrowPanel framebuffers");
  const esp_lcd_rgb_panel_event_callbacks_t callbacks = {
      .on_vsync = crowpanel_vsync,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(
      rgb_panel_handle, &callbacks, NULL), TAG, "Cannot register VSYNC callback");
  ESP_LOGI(TAG, "CrowPanel RGB v6: core %d, %d MHz PLL240M, VSYNC-switched direct double buffer",
           xPortGetCoreID(), ClockFrequency);
  ESP_LOGI(TAG, "CrowPanel v6: two immutable 4032-byte chains; 64B burst; FIFO-preserving VSYNC handoff");
  ESP_LOGI(TAG, "CrowPanel heap after scanout allocation: PSRAM free=%u, largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#endif

  // Initialize the panel
  ret = esp_lcd_panel_reset(rgb_panel_handle);
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to reset panel");

  ret = esp_lcd_panel_init(rgb_panel_handle);
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to initialize panel");

  // Turn on the display
  ret = esp_lcd_panel_disp_on_off(rgb_panel_handle, true);

  if (LCD_BACKLIGHT_GPIO != -1) {
    esp_rom_gpio_pad_select_gpio(LCD_BACKLIGHT_GPIO);
    gpio_set_direction(LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT);

    gpio_set_level(LCD_BACKLIGHT_GPIO, 1);
  }
  ESP_LOGI(TAG, "ST7262 LCD panel initialized successfully");
#endif
  return ESP_OK;
}

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD) && CONFIG_FREERTOS_NUMBER_OF_CORES > 1
typedef struct {
  SemaphoreHandle_t done;
  esp_err_t result;
} crowpanel_init_context_t;

static void crowpanel_init_task(void *arg) {
  crowpanel_init_context_t *ctx = arg;
  // ESP-IDF allocates LCD/GDMA interrupts on the calling core. Keep these
  // on core 1 with LVGL, matching the factory Arduino application's core.
  ctx->result = lcd_st7262_init_panel();
  xSemaphoreGive(ctx->done);
  vTaskDelete(NULL);
}
#endif

esp_err_t lcd_st7262_init(void) {
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  ESP_RETURN_ON_FALSE(rgb_panel_handle == NULL, ESP_ERR_INVALID_STATE, TAG,
                     "CrowPanel already initialized");
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
  crowpanel_init_context_t ctx = {.done = xSemaphoreCreateBinary(),
                                 .result = ESP_FAIL};
  ESP_RETURN_ON_FALSE(ctx.done, ESP_ERR_NO_MEM, TAG, "Cannot create init semaphore");
  if (xTaskCreatePinnedToCore(crowpanel_init_task, "RGB Init", 4096, &ctx,
                            5, NULL, 1) != pdPASS) {
    vSemaphoreDelete(ctx.done);
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreTake(ctx.done, portMAX_DELAY);
  vSemaphoreDelete(ctx.done);
  esp_err_t ret = ctx.result;
#else
  esp_err_t ret = lcd_st7262_init_panel();
#endif
  if (ret != ESP_OK) lcd_st7262_deinit();
  return ret;
#else
  return lcd_st7262_init_panel();
#endif
}

esp_err_t lcd_st7262_deinit(void) {
  esp_err_t ret = ESP_OK;

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  // Stop LVGL before releasing either its draw buffers or the scanout panel.
  if (crowpanel_display) {
    lv_disp_remove(crowpanel_display);
    crowpanel_display = NULL;
  }
#endif

  if (rgb_panel_handle) {
    ret = esp_lcd_panel_del(rgb_panel_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to delete panel");
    rgb_panel_handle = NULL;
  }

  if (io_handle) {
    ret = esp_lcd_panel_io_del(io_handle);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to delete panel IO");
    io_handle = NULL;
  }

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  for (size_t i = 0; i < 2; ++i) {
    // esp_lcd_panel_del already freed these driver-owned framebuffers.
    crowpanel_framebuffers[i] = NULL;
  }
  crowpanel_pending_fb = NULL;
  if (crowpanel_frame_done) {
    vSemaphoreDelete(crowpanel_frame_done);
    crowpanel_frame_done = NULL;
  }
#else
  ret = spi_bus_free(SPI2_HOST);
  ESP_RETURN_ON_ERROR(ret, TAG, "Failed to free SPI bus");
#endif

  // Delete semaphores
  if (sem_vsync_end) {
    vSemaphoreDelete(sem_vsync_end);
    sem_vsync_end = NULL;
  }
  if (sem_gui_ready) {
    vSemaphoreDelete(sem_gui_ready);
    sem_gui_ready = NULL;
  }

  ESP_LOGI(TAG, "ST7262 LCD panel deinitialized successfully");
  return ESP_OK;
}

esp_lcd_panel_handle_t lcd_st7262_get_panel_handle(void) {
  return rgb_panel_handle;
}

#ifdef CONFIG_CROWPANEL_ADVANCE_RGB_LCD
esp_err_t lcd_st7262_set_pclk_mhz(uint32_t mhz) {
  if (mhz < 8 || mhz > 21) return ESP_ERR_INVALID_ARG;
  if (!rgb_panel_handle || !crowpanel_display) return ESP_ERR_INVALID_STATE;
  esp_err_t ret = esp_lcd_rgb_panel_set_pclk(rgb_panel_handle, mhz * 1000000U);
  if (ret != ESP_OK) return ret;
  // Recover any existing displacement at the same VSYNC as the clock change.
  ret = esp_lcd_rgb_panel_restart(rgb_panel_handle);
  if (ret != ESP_OK) return ret;
  portENTER_CRITICAL(&crowpanel_frame_lock);
  crowpanel_stats = (crowpanel_rgb_stats_t){.requested_pclk_hz = mhz * 1000000U,
                                          .started_us = esp_timer_get_time()};
  crowpanel_last_vsync_us = 0;
  crowpanel_expected_frame_us = 410000U / mhz;
  portEXIT_CRITICAL(&crowpanel_frame_lock);
  return ESP_OK;
}

void lcd_st7262_get_rgb_stats(crowpanel_rgb_stats_t *stats) {
  if (!stats) return;
  portENTER_CRITICAL(&crowpanel_frame_lock);
  *stats = crowpanel_stats;
  portEXIT_CRITICAL(&crowpanel_frame_lock);
}

void lcd_st7262_reset_rgb_stats(void) {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL(&crowpanel_frame_lock);
  uint32_t pclk = crowpanel_stats.requested_pclk_hz;
  crowpanel_stats = (crowpanel_rgb_stats_t){.requested_pclk_hz = pclk, .started_us = now};
  crowpanel_last_vsync_us = 0;
  portEXIT_CRITICAL(&crowpanel_frame_lock);
}

// Implemented in the board-local IDF adaptation, which owns the GDMA handle.
extern esp_err_t crowpanel_rgb_read_registers(esp_lcd_panel_handle_t panel, uint32_t regs[16]);
extern esp_err_t crowpanel_rgb_read_bounce_stats(esp_lcd_panel_handle_t panel, uint32_t stats[7]);
esp_err_t lcd_st7262_get_bounce_stats(uint32_t stats[7]) {
  if (!stats) return ESP_ERR_INVALID_ARG;
  if (!rgb_panel_handle) return ESP_ERR_INVALID_STATE;
  return crowpanel_rgb_read_bounce_stats(rgb_panel_handle, stats);
}

esp_err_t lcd_st7262_get_hw_stats(uint32_t regs[16]) {
  if (!regs) return ESP_ERR_INVALID_ARG;
  if (!rgb_panel_handle) return ESP_ERR_INVALID_STATE;
  return crowpanel_rgb_read_registers(rgb_panel_handle, regs);
}
#endif

esp_err_t lcd_st7262_lvgl_init(void) {
  if (rgb_panel_handle == NULL) {
    ESP_LOGE(TAG, "Panel not initialized. Call lcd_st7262_init() first.");
    return ESP_ERR_INVALID_STATE;
  }

#if !defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  // display_manager already initializes LVGL for CrowPanel.
  lv_init();

  // Create semaphores for synchronization
  sem_vsync_end = xSemaphoreCreateBinary();
  if (sem_vsync_end == NULL) {
    ESP_LOGE(TAG, "Failed to create sem_vsync_end");
    return ESP_ERR_NO_MEM;
  }

  sem_gui_ready = xSemaphoreCreateBinary();
  if (sem_gui_ready == NULL) {
    ESP_LOGE(TAG, "Failed to create sem_gui_ready");
    return ESP_ERR_NO_MEM;
  }
#endif

  // Allocate LVGL display buffer
  static lv_color_t *lvgl_disp_buf1 = NULL;
  static lv_color_t *lvgl_disp_buf2 = NULL;

#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  // IDF starts scanout on framebuffer 0. Render the first frame into 1, then
  // alternate only after the VSYNC callback confirms the chain handoff.
  lvgl_disp_buf1 = crowpanel_framebuffers[1];
  lvgl_disp_buf2 = crowpanel_framebuffers[0];
  if(lv_draw_sw_s3_simd_init()) {
    ESP_LOGI(TAG, "CrowPanel SIMD v1: self-test PASS; RGB565 fill/copy enabled (lcdsimd on|off)");
  } else {
    ESP_LOGE(TAG, "CrowPanel SIMD v1: self-test FAIL; using original LVGL renderer");
  }
#else
  // Legacy path: separate LVGL draw buffers in SPIRAM.
  lvgl_disp_buf1 =
      heap_caps_malloc(800 * 480 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
  lvgl_disp_buf2 =
      heap_caps_malloc(800 * 480 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);

  if (lvgl_disp_buf1 == NULL || lvgl_disp_buf2 == NULL) {
    ESP_LOGE(TAG, "Failed to allocate frame buffers");
    return ESP_ERR_NO_MEM;
  }
#endif

  static lv_disp_draw_buf_t draw_buf;
  lv_disp_draw_buf_init(&draw_buf, lvgl_disp_buf1, lvgl_disp_buf2, 800 * 480);

  // Initialize LVGL display driver
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 800; // Adjust based on your display resolution
  disp_drv.ver_res = 480;
  disp_drv.flush_cb = lcd_st7262_lvgl_flush_cb;
  disp_drv.draw_buf = &draw_buf;
  disp_drv.user_data = (void *)rgb_panel_handle;
  disp_drv.full_refresh = false;
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  disp_drv.direct_mode = true;
  disp_drv.render_start_cb = crowpanel_render_start;
  disp_drv.sync_area_cb = crowpanel_sync_copied_area;
  ESP_LOGI(TAG, "CrowPanel LVGL v6: direct dirty rendering with confirmed VSYNC buffer ownership");
  ESP_LOGI(TAG, "RGB diagnostics v5: bounce remains disabled; presented frames are confirmed handoffs");
#endif

  // Register the display driver with LVGL
#if defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD)
  crowpanel_display = lv_disp_drv_register(&disp_drv);
  ESP_RETURN_ON_FALSE(crowpanel_display, ESP_ERR_NO_MEM, TAG,
                     "Failed to register LVGL display");
#else
  lv_disp_drv_register(&disp_drv);
#endif

  ESP_LOGI(TAG, "LVGL initialized successfully");
  return ESP_OK;
}

#endif
