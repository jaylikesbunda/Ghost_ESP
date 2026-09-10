#include "managers/sd_card_manager.h"
#include "core/utils.h"
#include "driver/gpio.h"
#include "driver/sdmmc_defs.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_types.h"
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "vendor/drivers/CH422G.h"
#include "vendor/pcap.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "managers/status_display_manager.h"
#include "managers/display_manager.h"
#include "gui/toast.h"
#include "lvgl_tft/disp_spi.h"
#if defined(CONFIG_LV_TOUCH_DRIVER_PROTOCOL_SPI) && !defined(CONFIG_USE_BIT_BANG_TOUCH)
#include "lvgl_touch/tp_spi.h"
#endif

#define MAX_PORTALS 32
#define MAX_PORTAL_NAME 64

static const char *TAG = "SD_Card_Manager";
static const char *NVS_NAMESPACE = "sd_config";

#if defined(CONFIG_CROWPANEL_ADVANCED_P4) && defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
static esp_err_t sdmmc_host_init_dummy(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#endif
static bool s_sd_log_levels_tuned = false;
static SemaphoreHandle_t s_sd_jit_mutex = NULL;
static uint32_t s_sd_jit_mount_depth = 0;
static bool s_sd_jit_display_suspended = false;

// Track SPI bus ownership locally so cleanup only frees a bus SD initialized
// itself, while still clearing reused-host bookkeeping on unmount/failure.
static bool s_spi_bus_initialized = false;
static bool s_spi_bus_owned_by_sd = false;
static int s_spi_host_id = -1;
typedef enum { MOUNT_NONE = 0, MOUNT_VIRTUAL, MOUNT_SDMMC, MOUNT_SPI } sd_mount_type_t;
static sd_mount_type_t s_mount_type = MOUNT_NONE;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
static sd_pwr_ctrl_handle_t s_crowpanel_sd_power = NULL;
#endif
static TickType_t s_next_unmount_tick = 0;

// USB MSC passthrough state (see sd_card_suspend_for_usb_msc)
static bool s_msc_owns_sd = false;
static sdmmc_card_t *s_msc_card = NULL;
static BYTE s_msc_pdrv = 0xff;

static void sd_spi_bus_release_if_tracked(void);

#if defined(CONFIG_IDF_TARGET_ESP32C5)
static int sd_card_c5_max_freq_khz(void) {
#if defined(CONFIG_BUILD_CONFIG_TEMPLATE)
  if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
    return 20000;
  }
#endif
  return 1000;
}
#endif

static void sd_spi_release_cs_pin(void) {
#if defined(CONFIG_USING_SPI)
  int cs_pin = sd_card_manager.spi_cs_pin;
  if (cs_pin >= 0 && cs_pin < 64) esp_gpio_revoke(1ULL << cs_pin);
#endif
}

static void sd_spi_bus_track(int host_id, bool owned_by_sd) {
  s_spi_bus_initialized = owned_by_sd;
  s_spi_bus_owned_by_sd = owned_by_sd;
  s_spi_host_id = host_id;
}

static void sd_spi_bus_clear_tracking(void) {
  s_spi_bus_initialized = false;
  s_spi_bus_owned_by_sd = false;
  s_spi_host_id = -1;
}

/* time multiplex spi when display and sd share the spi bus */
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI) && !defined(CONFIG_USE_TDISPLAY_S3)
#include "lvgl_helpers.h"
#include "lvgl_tft/disp_spi.h"
#include "lvgl_spi_conf.h"
#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif
#include "managers/display_manager.h"
static bool s_display_spi_suspended_flag = false;
static bool s_touch_spi_detached = false;
/* Tracks whether the display panel's SPI device is currently attached to the bus.
 * Set true on boot after disp_spi_add_device() succeeds; cleared on suspend. Lets
 * the resume path skip a redundant lvgl_spi_driver_init() when the bus is still
 * initialized from a prior flush. */
static bool s_display_panel_attached = false;

static bool display_sd_spi_pins_match(void) {
#if defined(CONFIG_LV_DISP_SPI_MOSI) && defined(CONFIG_LV_DISP_SPI_CLK)
  bool mosi_match = (sd_card_manager.spi_mosi_pin == CONFIG_LV_DISP_SPI_MOSI);
  bool clk_match = (sd_card_manager.spi_clk_pin == CONFIG_LV_DISP_SPI_CLK);
#if defined(CONFIG_LV_DISP_SPI_MISO)
  bool miso_match = (sd_card_manager.spi_miso_pin == CONFIG_LV_DISP_SPI_MISO);
  return mosi_match && clk_match && miso_match;
#else
  return mosi_match && clk_match;
#endif
#else
  return false;
#endif
}

static bool is_shared_display_sd_spi(void) {
#if defined(CONFIG_USE_C5_PARLIO_DISPLAY)
  /* PARLIO is independent of the SPI2 host used by the SD card. */
  return false;
#elif (defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)) && defined(CONFIG_LV_TFT_DISPLAY_SPI2_HOST)
  /* These targets mount SD on SPI2_HOST, so a display on SPI2_HOST must be
   * time-multiplexed even when the display and SD use different pins. */
  return true;
#else
  return display_sd_spi_pins_match();
#endif
}

static bool display_spi_requires_rebind_for_sd(void) {
  return is_shared_display_sd_spi() && !display_sd_spi_pins_match();
}

static bool display_spi_resume_after_sd(void);

static bool display_spi_suspend_for_sd(void) {
  if (!is_shared_display_sd_spi()) {
    return false;
  }
  if (s_display_spi_suspended_flag) {
    return true;  /* already suspended — idempotent, matches resume_after_sd guard */
  }
  /* Stop LVGL from queuing new flushes. */
  display_manager_suspend_input_task();
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) {
    lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
    if (refr) lv_timer_pause(refr);
  }
  /* Drain in-flight SPI transactions BEFORE suspending the LVGL task.
   * Suspending the task mid-flush leaves the SPI peripheral holding a
   * half-finished DMA descriptor; on C5 this produces a visible tear
   * or "stuck" pixel row until the next resume cycle. */
  disp_wait_for_pending_transactions();
  display_manager_suspend_lvgl_task();
  if (display_sd_spi_pins_match()) {
    /* All devices can remain registered on a same-pin bus. Their owning tasks
     * are parked, so SPI master arbitration and chip-select handling remain in
     * the known-working boot configuration while SD temporarily owns traffic. */
#ifdef CONFIG_LV_DISP_SPI_CS
    gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
#endif
    s_display_panel_attached = true;
    s_display_spi_suspended_flag = true;
    return true;
  }
#if defined(CONFIG_LV_TOUCH_DRIVER_PROTOCOL_SPI) && !defined(CONFIG_USE_BIT_BANG_TOUCH)
  esp_err_t touch_ret = tp_spi_remove_device();
  if (touch_ret != ESP_OK) {
    ESP_LOGE(TAG, "Cannot release touch SPI device for SD: %s", esp_err_to_name(touch_ret));
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
      lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
      if (refr) lv_timer_resume(refr);
    }
    display_manager_resume_lvgl_task();
    display_manager_resume_input_task();
    return false;
  }
  s_touch_spi_detached = true;
#endif
  esp_err_t remove_ret = disp_spi_remove_device();
  if (remove_ret != ESP_OK) {
    ESP_LOGE(TAG, "Cannot release display SPI device for SD: %s", esp_err_to_name(remove_ret));
#if defined(CONFIG_LV_TOUCH_DRIVER_PROTOCOL_SPI) && !defined(CONFIG_USE_BIT_BANG_TOUCH)
    if (s_touch_spi_detached) {
      tp_spi_add_device(TOUCH_SPI_HOST);
      s_touch_spi_detached = false;
    }
#endif
    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
      lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
      if (refr) lv_timer_resume(refr);
    }
    display_manager_resume_lvgl_task();
    display_manager_resume_input_task();
    return false;
  }
  s_display_panel_attached = false;
  esp_err_t free_ret = spi_bus_free(TFT_SPI_HOST);
  if (free_ret != ESP_OK) {
    ESP_LOGE(TAG, "Cannot release display SPI bus for SD: %s", esp_err_to_name(free_ret));
    s_display_spi_suspended_flag = true;
    display_spi_resume_after_sd();
    return false;
  }
  /* assert CS high so that panel stays quiet */
  #ifdef CONFIG_LV_DISP_SPI_CS
  gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
  #endif
  s_display_spi_suspended_flag = true;
  return true;
}
static bool display_spi_resume_after_sd(void) {
  if (!is_shared_display_sd_spi()) {
    return true;
  }
  if (!s_display_spi_suspended_flag) {
    return true;
  }
  if (s_display_panel_attached) {
    /* Already attached: nothing to do (shouldn't normally hit this, but
     * guards against double-resume). */
    ESP_LOGD("sd_card", "display_spi_resume: panel already attached, skipping reinit");
  } else {
    esp_err_t ret = lvgl_spi_driver_init(TFT_SPI_HOST, DISP_SPI_MISO, DISP_SPI_MOSI, DISP_SPI_CLK,
                                SPI_BUS_MAX_TRANSFER_SZ, 1, DISP_SPI_IO2, DISP_SPI_IO3);
    if (ret == ESP_OK) {
      esp_err_t add_ret = disp_spi_add_device(TFT_SPI_HOST);
      if (add_ret != ESP_OK) {
        ESP_LOGE("sd_card", "display_spi_resume: add device failed: %s", esp_err_to_name(add_ret));
      } else {
        s_display_panel_attached = true;
      }
    } else if (ret == ESP_ERR_INVALID_STATE) {
      /* Bus is already initialized (by SD or by an earlier flush) — just
       * re-attach the panel device. */
      esp_err_t add_ret = disp_spi_add_device(TFT_SPI_HOST);
      if (add_ret != ESP_OK) {
        ESP_LOGE("sd_card", "display_spi_resume: add device failed: %s", esp_err_to_name(add_ret));
      } else {
        s_display_panel_attached = true;
      }
    } else {
      ESP_LOGE("sd_card", "display_spi_resume: bus init failed: %s", esp_err_to_name(ret));
    }
  }
  if (!s_display_panel_attached) {
    /* Keep LVGL parked: a live render task without a panel device can use a
     * stale SPI handle and turn one failed handoff into a watchdog reset. */
    ESP_LOGE(TAG, "Display SPI rebind failed; LVGL remains suspended");
    return false;
  }
#if defined(CONFIG_LV_TOUCH_DRIVER_PROTOCOL_SPI) && !defined(CONFIG_USE_BIT_BANG_TOUCH)
  if (s_touch_spi_detached) {
    tp_spi_add_device(TOUCH_SPI_HOST);
    s_touch_spi_detached = false;
  }
#endif
  /* Resume LVGL only after its panel device is attached to the display bus. */
  lv_disp_t *disp = lv_disp_get_default();
  if (disp) {
    lv_timer_t *refr = _lv_disp_get_refr_timer(disp);
    if (refr) lv_timer_resume(refr);
  }
  display_manager_resume_lvgl_task();
  display_manager_resume_input_task();
  s_display_spi_suspended_flag = false;
  return true;
}
#else
static bool display_sd_spi_pins_match(void) { return false; }
static bool is_shared_display_sd_spi(void) { return false; }
static bool display_spi_requires_rebind_for_sd(void) { return false; }
static bool display_spi_suspend_for_sd(void) { return false; }
static bool display_spi_resume_after_sd(void) { return true; }
#endif

/* CoreS3-SE shares GPIO35 between LCD D/C and SD MISO. The display drives it
 * while its CS is active; the SD card must be allowed to drive it as an input
 * during the shared-bus mount. */
static void shared_spi_set_display_dc_mode(bool sd_mode)
{
#if defined(CONFIG_LV_DISPLAY_USE_SPI_MISO) && defined(CONFIG_LV_DISP_PIN_DC) && \
    defined(CONFIG_SD_SPI_MISO_PIN) && (CONFIG_LV_DISP_PIN_DC == CONFIG_SD_SPI_MISO_PIN)
  esp_err_t ret = gpio_set_direction(CONFIG_LV_DISP_PIN_DC,
                                     sd_mode ? GPIO_MODE_INPUT : GPIO_MODE_OUTPUT);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to switch shared LCD D/C GPIO%d to %s: %s",
             CONFIG_LV_DISP_PIN_DC, sd_mode ? "SD input" : "LCD output",
             esp_err_to_name(ret));
  }
#else
  (void)sd_mode;
#endif
}

static inline void shared_spi_guard_resume_lvgl_if_needed(bool guard_active, bool sd_mounted) {
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI) && !defined(CONFIG_USE_TDECK)
  ESP_LOGI(TAG, "shared_spi_guard_resume_lvgl_if_needed(%d, mounted=%d)",
           guard_active, sd_mounted);
  if (guard_active) {
    shared_spi_set_display_dc_mode(sd_mounted);
    display_manager_resume_lvgl_task();
    display_manager_resume_input_task();
  }
#else
  (void)guard_active;
  (void)sd_mounted;
#endif
}

static const char *sd_spi_host_name(int host_id) {
  switch (host_id) {
    case SPI2_HOST:
      return "SPI2_HOST";
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
    case SPI3_HOST:
      return "SPI3_HOST";
#endif
    default:
      return "SPI_HOST?";
  }
}

static int sd_spi_host_id(void) {
#if defined(CONFIG_CROWPANEL_EPAPER_42) && defined(CONFIG_IDF_TARGET_ESP32S3)
  /* Elecrow's factory firmware uses SPIClass(HSPI), i.e. SPI2_HOST, for the
   * SD socket.  The e-paper adapter is factory-compatible GPIO bit-banged on
   * GPIO11/12, so SPI2 is free here and is the known-good SD host. */
  return SPI2_HOST;
#endif
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI) && defined(TFT_SPI_HOST)
  if (is_shared_display_sd_spi()) {
    return TFT_SPI_HOST;
  }
#endif
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
  return SPI3_HOST;
#else
  return SPI2_HOST;
#endif
}

static esp_err_t sd_card_prepare_shared_spi_card(void) {
#if defined(CONFIG_USING_SPI)
  if (!is_shared_display_sd_spi()) return ESP_OK;

  gpio_set_direction(sd_card_manager.spi_cs_pin, GPIO_MODE_OUTPUT);
  gpio_set_level(sd_card_manager.spi_cs_pin, 1);
#ifdef CONFIG_LV_DISP_SPI_CS
  gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
#endif
#if defined(CONFIG_LV_TOUCH_DRIVER_PROTOCOL_SPI) && !defined(CONFIG_USE_BIT_BANG_TOUCH)
  gpio_set_level(TP_SPI_CS, 1);
#endif

  spi_device_interface_config_t devcfg = {
      .clock_speed_hz = 400000,
      .mode = 0,
      .spics_io_num = -1,
      .queue_size = 1,
  };
  spi_device_handle_t clock_device = NULL;
  esp_err_t ret = spi_bus_add_device(sd_spi_host_id(), &devcfg, &clock_device);
  if (ret != ESP_OK) return ret;

  uint8_t idle_clocks[20];
  memset(idle_clocks, 0xFF, sizeof(idle_clocks));
  spi_transaction_t transaction = {
      .length = sizeof(idle_clocks) * 8,
      .tx_buffer = idle_clocks,
  };
  ret = spi_device_transmit(clock_device, &transaction);
  esp_err_t remove_ret = spi_bus_remove_device(clock_device);
  return ret != ESP_OK ? ret : remove_ret;
#else
  return ESP_OK;
#endif
}

static esp_err_t sd_card_mount_spi_with_retry(
    sdmmc_host_t *host,
    const sdspi_device_config_t *slot_config,
    const esp_vfs_fat_sdmmc_mount_config_t *mount_config) {
  esp_err_t ret = ESP_FAIL;
  // The IDF SD driver logs a scary multi-line error (with a reboot HINT)
  // for every attempt. Silence it during retries and emit ONE clear,
  // user-friendly summary at the end instead - the raw spam was generating
  // bogus bug reports for the normal "no card inserted" case.
  esp_log_level_t prev_vfs = esp_log_level_get("vfs_fat_sdmmc");
  esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_NONE);
  for (int attempt = 1; attempt <= 3; ++attempt) {
    esp_err_t prepare_ret = sd_card_prepare_shared_spi_card();
    if (prepare_ret != ESP_OK) {
      ESP_LOGW(TAG, "Shared SPI SD idle clocks failed: %s", esp_err_to_name(prepare_ret));
    }
    sd_card_manager.card = NULL;
    ret = esp_vfs_fat_sdspi_mount("/mnt", host, slot_config, mount_config,
                                  &sd_card_manager.card);
    if (ret == ESP_OK) break;
    ESP_LOGD(TAG, "SD mount attempt %d/3 failed: %s", attempt, esp_err_to_name(ret));
    if (attempt < 3) vTaskDelay(pdMS_TO_TICKS(100));
  }
  esp_log_level_set("vfs_fat_sdmmc", prev_vfs);
  if (ret != ESP_OK) {
    bool no_card = (ret == ESP_ERR_TIMEOUT);
    ESP_LOGW(TAG, "No SD card detected after 3 attempts (%s)%s",
             esp_err_to_name(ret),
             no_card ? " - insert a card and reboot to enable SD features" : "");
  }
  return ret;
}



sd_card_manager_t sd_card_manager = { // Change this based on board config
    .card = NULL,
    .is_initialized = false,
    .clkpin = 19,
    .cmdpin = 18,
    .d0pin = 20,
    .d1pin = 21,
    .d2pin = 22,
    .d3pin = 23,
#ifdef CONFIG_USING_SPI
    .spi_cs_pin = CONFIG_SD_SPI_CS_PIN,
    .spi_clk_pin = CONFIG_SD_SPI_CLK_PIN,
    .spi_miso_pin = CONFIG_SD_SPI_MISO_PIN,
    .spi_mosi_pin = CONFIG_SD_SPI_MOSI_PIN
#endif
};

#if defined(CONFIG_IDF_TARGET_ESP32S3)
static int choose_free_s3_sd_spi_host(const spi_bus_config_t *bus_config, int dma_channel) {
  /* When Ethernet or NRF24 is configured on SPI3, prefer SPI2 for SD to avoid
   * stealing the host that Ethernet needs, which would force it onto SPI2
   * where the display is already initialized (causing Ethernet to fail). */
#if defined(CONFIG_WITH_ETHERNET) || defined(CONFIG_HAS_NRF24)
  int preferred_hosts[] = { SPI2_HOST, SPI3_HOST };
#else
  int preferred_hosts[] = { SPI3_HOST, SPI2_HOST };
#endif

  for (size_t i = 0; i < sizeof(preferred_hosts) / sizeof(preferred_hosts[0]); ++i) {
    int host_id = preferred_hosts[i];
    esp_err_t probe_ret = spi_bus_initialize(host_id, bus_config, dma_channel);
    if (probe_ret == ESP_OK) {
      sd_spi_bus_track(host_id, true);
      ESP_LOGI(TAG, "Selected free SD SPI host: %s", sd_spi_host_name(host_id));
      return host_id;
    }
    if (probe_ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "SPI host probe failed for %s: %s",
               sd_spi_host_name(host_id),
               esp_err_to_name(probe_ret));
    }
  }

  return -1;
}
#endif

/* Ordinary CYD boards keep their display on SPI2 while SD owns a separate
 * SPI3 bus. The CrowPanel e-paper is the exception: its display is GPIO-SPI
 * and the factory assigns SD to SPI2/HSPI. On classic ESP32 boards, tearing
 * down the separate SPI3 bus on a no-card mount can disturb the live display,
 * so that topology keeps the bus initialized for a later retry. */
static bool sd_keep_spi_bus_for_board(void) {
#if defined(CONFIG_IDF_TARGET_ESP32) && defined(CONFIG_WITH_SCREEN)
  return !is_shared_display_sd_spi();
#else
  return false;
#endif
}

static void sd_spi_bus_release_if_tracked(void) {
  ESP_LOGD(TAG, "sd_spi_bus_release_if_tracked: initialized=%d owned=%d host=%d",
            s_spi_bus_initialized, s_spi_bus_owned_by_sd, s_spi_host_id);
  if (s_spi_host_id >= 0) {
    if (!s_spi_bus_owned_by_sd || sd_keep_spi_bus_for_board()) {
      ESP_LOGD(TAG, "Skipping spi_bus_free for reused SPI host %d", s_spi_host_id);
      sd_spi_bus_clear_tracking();
      return;
    }

    ESP_LOGD(TAG, "Freeing SPI bus host %d", s_spi_host_id);
    spi_bus_free(s_spi_host_id);
    sd_spi_bus_clear_tracking();
  }
}

static sd_card_cached_stats_t s_cached_stats = { .valid = false, .used_pct = 0 };

static SemaphoreHandle_t sd_card_get_jit_mutex(void) {
    if (s_sd_jit_mutex == NULL) {
        /* Recursive: callers like options_screen / commandline can hold
         * jit_mount across a nested flush (e.g. flush triggered by another
         * flush). A plain mutex would deadlock; a recursive one just bumps
         * the lock count. */
        s_sd_jit_mutex = xSemaphoreCreateRecursiveMutex();
    }
    return s_sd_jit_mutex;
}

static void sd_card_update_cached_stats(void) {
    if (!sd_card_manager.is_initialized) {
        s_cached_stats.valid = false;
        return;
    }
    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info("/mnt", &total_bytes, &free_bytes);
    if (ret == ESP_OK && total_bytes > 0) {
        uint64_t used_bytes = total_bytes - free_bytes;
        s_cached_stats.used_pct = (int)((used_bytes * 100) / total_bytes);
        if (s_cached_stats.used_pct < 0) s_cached_stats.used_pct = 0;
        if (s_cached_stats.used_pct > 100) s_cached_stats.used_pct = 100;
        s_cached_stats.valid = true;
    }
}

void sd_card_get_cached_stats(sd_card_cached_stats_t *out) {
    if (out) {
        *out = s_cached_stats;
    }
}

#if defined(CONFIG_IS_S3TWATCH) || defined(CONFIG_IS_ATOMS3R)
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_virtual_storage_mounted = false;

static esp_err_t mount_virtual_storage(void) {
    if (s_virtual_storage_mounted) {
        ESP_LOGI(TAG, "Virtual storage already mounted");
        return ESP_OK;
    }

    const esp_partition_t* storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!storage_partition) {
        ESP_LOGE(TAG, "Storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Found storage partition at offset 0x%lx with size %lu KB", 
             (unsigned long)storage_partition->address, (unsigned long)(storage_partition->size / 1024));
    
    if (storage_partition->size < 64 * 1024) {
        ESP_LOGE(TAG, "Storage partition too small: %lu bytes", (unsigned long)storage_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 3,
        .allocation_unit_size = 4 * 1024
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl("/mnt", "storage", &mount_config, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount virtual storage: %s", esp_err_to_name(ret));
        toast_show("Virtual storage mount failed", TOAST_ERROR);
        return ret;
    }

    s_virtual_storage_mounted = true;
    ESP_LOGI(TAG, "Virtual storage mounted successfully at /mnt");
    s_mount_type = MOUNT_VIRTUAL;
    status_display_show_status("Virtual SD OK");
    toast_show("Virtual storage mounted", TOAST_SUCCESS);
    return ESP_OK;
}

static void unmount_virtual_storage(void) {
    if (!s_virtual_storage_mounted) {
        return;
    }

    esp_vfs_fat_spiflash_unmount_rw_wl("/mnt", s_wl_handle);
    s_virtual_storage_mounted = false;
    s_wl_handle = WL_INVALID_HANDLE;
    ESP_LOGI(TAG, "Virtual storage unmounted");
    s_mount_type = MOUNT_NONE;
    status_display_show_status("Virtual SD Off");
}
#endif

void list_files_recursive(const char *dirname, int level) {
  DIR *dir = opendir(dirname);
  if (!dir) {
    printf("Failed to open directory: %s\n", dirname);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char path[512];
    int written = snprintf(path, sizeof(path), "%s/%s", dirname, entry->d_name);

    if (written < 0 || written >= sizeof(path)) {
      printf("Path was truncated: %s/%s\n", dirname, entry->d_name);
      continue;
    }

    struct stat statbuf;
    if (stat(path, &statbuf) == 0) {
      for (int i = 0; i < level; i++) {
        printf("  ");
      }

      if (S_ISDIR(statbuf.st_mode)) {
        printf("[Dir] %s/\n", entry->d_name);
        list_files_recursive(path, level + 1);
      } else {
        printf("[File] %s\n", entry->d_name);
      }
    }
  }
  closedir(dir);
}

static void sdmmc_card_print_info(const sdmmc_card_t *card) {
  if (card == NULL) {
    printf("SD card info unavailable\n");
    return;
  }

  printf("SD card ready: %s, %lluMB (%s)\n",
         (card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC",
         ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024),
         (card->csd.tr_speed > 25000000) ? "high speed" : "default speed");

  ESP_LOGD(TAG, "SD details: %s, sector size %dB, CSD v%d, mfg %02x, serial %08x",
           card->cid.name, card->csd.sector_size, card->csd.csd_ver,
           card->cid.mfg_id, card->cid.serial);
}

esp_err_t sd_card_init(void) {
  esp_err_t ret = ESP_FAIL;

  ESP_LOGI(TAG, "sd_card_init: starting, free internal RAM: %d bytes", 
           (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  if (!s_sd_log_levels_tuned) {
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);
    s_sd_log_levels_tuned = true;
  }

  if (sd_card_manager.is_initialized) {
    ESP_LOGI(TAG, "sd_card_init: already initialized");
    return ESP_OK;
  }

  if (s_msc_owns_sd) {
    ESP_LOGW(TAG, "sd_card_init: card is in USB passthrough mode");
    return ESP_ERR_INVALID_STATE;
  }

  /* Clean up stale tracked SPI state before a fresh init attempt. */
  if (s_spi_host_id >= 0) {
    sd_spi_bus_release_if_tracked();
  }
  sd_card_manager.card = NULL;


#if defined(CONFIG_IS_S3TWATCH) || defined(CONFIG_IS_ATOMS3R)
  ESP_LOGI(TAG, "Board without SD card detected - attempting virtual storage mount");
  
  vTaskDelay(pdMS_TO_TICKS(100));
  
  ret = mount_virtual_storage();
  if (ret == ESP_OK) {
    sd_card_manager.is_initialized = true;
    ESP_LOGI(TAG, "Virtual storage initialized successfully");
    sd_card_setup_directory_structure();
    return ESP_OK;
  } else {
    ESP_LOGW(TAG, "Virtual storage mount failed (%s), falling back to physical SD card", esp_err_to_name(ret));
  }
#endif

  // Load configuration from NVS first
  sd_card_load_config();
  sd_card_print_config(); // Print loaded/default config

  // Backup current config in case init fails
  sd_card_manager_t backup_config = sd_card_manager;

#ifdef CONFIG_USING_MMC_1_BIT
  printf("Mounting SD card (SDMMC 1-bit)...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
  if (!s_crowpanel_sd_power) {
    const sd_pwr_ctrl_ldo_config_t ldo_config = {.ldo_chan_id = 4};
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_crowpanel_sd_power);
    if (ret != ESP_OK) {
      /* LDO4 may already be held by another driver (e.g. an older display
       * init path).  On CrowPanel boards the SD card rail is powered by
       * on-chip LDO4 at 3.3 V; if the channel is already enabled at the
       * correct voltage the card will work even without our own handle. */
      ESP_LOGW(TAG, "CrowPanel SD LDO4 acquire returned %s — rail may already be on",
               esp_err_to_name(ret));
      s_crowpanel_sd_power = NULL;
    }
  }
  if (s_crowpanel_sd_power) {
    host.pwr_ctrl_handle = s_crowpanel_sd_power;
  }
#endif
#if defined(CONFIG_CROWPANEL_ADVANCED_P4) && defined(CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE)
  // ESP32-P4 has a single SDMMC host controller (SDMMC_LL_HOST_CTLR_NUMS=1).
  // ESP-Hosted already claimed it for hosted SDIO on slot 1; use dummy
  // init/deinit so the SD mount reuses that controller and only adds slot 0.
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  host.init = sdmmc_host_init_dummy;
  host.deinit = sdmmc_host_deinit_dummy;
#elif defined(CONFIG_CROWPANEL_ADVANCED_P4)
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#endif

  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;

  slot_config.clk = CONFIG_SD_MMC_CLK;
  slot_config.cmd = CONFIG_SD_MMC_CMD;
  slot_config.d0 = CONFIG_SD_MMC_D0;

  gpio_set_pull_mode(CONFIG_SD_MMC_D0, GPIO_PULLUP_ONLY);  // CLK
  gpio_set_pull_mode(CONFIG_SD_MMC_CLK, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(CONFIG_SD_MMC_CMD, GPIO_PULLUP_ONLY); // D0

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
    if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    sd_card_manager.card = NULL;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    if (s_crowpanel_sd_power) {
      sd_pwr_ctrl_del_on_chip_ldo(s_crowpanel_sd_power);
      s_crowpanel_sd_power = NULL;
    }
#endif
    toast_show("SD mount failed", TOAST_ERROR);
    return ret;
  }

  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SDMMC;
  sdmmc_card_print_info(sd_card_manager.card);

  sd_card_setup_directory_structure();

  return ESP_OK;

#elif defined(CONFIG_USING_MMC)

  printf("Mounting SD card (SDMMC 4-bit)...\n");

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

  slot_config.clk = sd_card_manager.clkpin;
  slot_config.cmd = sd_card_manager.cmdpin; // SDMMC_CMD -> GPIO 16
  slot_config.d0 = sd_card_manager.d0pin;   // SDMMC_D0  -> GPIO 14
  slot_config.d1 = sd_card_manager.d1pin;   // SDMMC_D1  -> GPIO 17
  slot_config.d2 = sd_card_manager.d2pin;   // SDMMC_D2  -> GPIO 21
  slot_config.d3 = sd_card_manager.d3pin;   // SDMMC_D3  -> GPIO 18

  host.flags = SDMMC_HOST_FLAG_4BIT;

  gpio_set_pull_mode(sd_card_manager.clkpin, GPIO_PULLUP_ONLY); // CLK
  gpio_set_pull_mode(sd_card_manager.cmdpin, GPIO_PULLUP_ONLY); // CMD
  gpio_set_pull_mode(sd_card_manager.d0pin, GPIO_PULLUP_ONLY);  // D0
  gpio_set_pull_mode(sd_card_manager.d1pin, GPIO_PULLUP_ONLY);  // D1
  gpio_set_pull_mode(sd_card_manager.d2pin, GPIO_PULLUP_ONLY);  // D2
  gpio_set_pull_mode(sd_card_manager.d3pin, GPIO_PULLUP_ONLY);  // D3

  slot_config.gpio_cd = GPIO_NUM_NC; // Disable Card Detect pin
  slot_config.gpio_wp = GPIO_NUM_NC; // Disable Write Protect pin

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
      .allocation_unit_size = 16 * 1024};

  ret = esp_vfs_fat_sdmmc_mount("/mnt", &host, &slot_config, &mount_config,
                                &sd_card_manager.card);
  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      printf("Failed to mount filesystem. If you want the card to be "
             "formatted, set format_if_mount_failed = true.\n");
    } else {
      printf("Failed to initialize the card (%s). Make sure SD card lines have "
             "pull-up resistors in place.\n",
             esp_err_to_name(ret));
    }
    sd_card_manager.card = NULL;
    toast_show("SD mount failed", TOAST_ERROR);
    return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);

  sd_card_setup_directory_structure();

  return ESP_OK;
#elif CONFIG_USING_SPI

  printf("Mounting SD card (SPI)...\n");

  bool shared_spi_guard_active = false;
  bool display_rebind_required = false;
  bool concurrent_shared_spi = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
  concurrent_shared_spi = strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "m5cores3se") == 0;
#endif
#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_LV_TFT_DISPLAY_PROTOCOL_SPI) && !defined(CONFIG_USE_TDECK)
  ESP_LOGI(TAG, "Checking shared SPI: is_shared_display_sd_spi()=%d, display_sd_spi_pins_match()=%d",
           is_shared_display_sd_spi(), display_sd_spi_pins_match());
  display_rebind_required = display_spi_requires_rebind_for_sd();
  ESP_LOGI(TAG, "display_rebind_required=%d", display_rebind_required);
  if (is_shared_display_sd_spi() && !display_rebind_required) {
    if (concurrent_shared_spi) {
      /* CoreS3-SE has a real shared SPI bus. Keep LVGL alive and let the SPI
       * host arbitrate display and SD transactions normally. */
      shared_spi_set_display_dc_mode(true);
    } else {
      /* Pins match the display, so the display's SPI device is still attached
       * to the shared host. Pause LVGL and defer panel detach to the
       * gating/rebind block below before SD claims the bus. */
      shared_spi_guard_active = true;
      ESP_LOGI(TAG, "Suspending LVGL task for shared SPI access");
      display_manager_suspend_input_task();
      display_manager_suspend_lvgl_task();
      disp_wait_for_pending_transactions();
#ifdef CONFIG_LV_DISP_SPI_CS
      gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
#endif
      shared_spi_set_display_dc_mode(true);
    }
  }
#else
  ESP_LOGI(TAG, "Shared SPI code path not compiled in");
#endif

  bool gating_template = false;
  /* On classic-ESP32 boards whose SD owns a separate SPI3 bus (every CYD
   * variant), SD genuinely *owns* that bus (bus_init_success == true).
   * See sd_keep_spi_bus_for_board() for why freeing it freezes the display;
   * keep the bus alive on mount failure (no card) here. */
  bool keep_bus_on_failure = sd_keep_spi_bus_for_board();
  gating_template = sd_card_needs_jit_mount();
  bool display_was_suspended = false;
  /* Only boards that explicitly JIT-gate SD or need pin rebinding should detach
   * the panel. Same-pin shared SPI boards like TEmbedC1101 keep the old path. */
  if (gating_template || display_rebind_required) {
    display_was_suspended = display_spi_suspend_for_sd();
    if (display_was_suspended) {
      /* Full suspend removed the panel device. Do not resume the LVGL task via
       * the lightweight guard; display_spi_resume_after_sd() must re-add the
       * panel first and then resume LVGL. */
      shared_spi_guard_active = false;
    }
  }



#ifdef CONFIG_Waveshare_LCD
#define I2C_NUM I2C_NUM_0
#define I2C_ADDRESS 0x24
#define EXIO4_BIT (1 << 4)
#define EXIO1_BIT (1 << 1)

  esp_io_expander_ch422g_t *ch422g_dev = NULL;
  esp_err_t err;

  err = ch422g_new_device(I2C_NUM, I2C_ADDRESS, &ch422g_dev);
  if (err != ESP_OK) {
    printf("Failed to initialize CH422G: %s\n", esp_err_to_name(err));
    return err;
  }

  uint32_t direction, output_value;

  err = ch422g_read_direction_reg(ch422g_dev, &direction);
  if (err != ESP_OK) {
    printf("Failed to read direction register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial direction register: 0x%03lX\n", direction);

  err = ch422g_read_output_reg(ch422g_dev, &output_value);
  if (err != ESP_OK) {
    printf("Failed to read output register: %s\n", esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  printf("Initial output register: 0x%03lX\n", output_value);

  direction &= ~EXIO1_BIT;
  output_value |= EXIO1_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO1: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  direction &= ~EXIO4_BIT;
  output_value &= ~EXIO4_BIT;

  err = ch422g_write_direction_reg(ch422g_dev, direction);
  if (err != ESP_OK) {
    printf("Failed to write direction register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }
  err = ch422g_write_output_reg(ch422g_dev, output_value);
  if (err != ESP_OK) {
    printf("Failed to write output register for EXIO4: %s\n",
           esp_err_to_name(err));
    cleanup_resources(ch422g_dev, I2C_NUM);
    return err;
  }

  printf("Final direction register: 0x%03lX\n", direction);
  printf("Final output register: 0x%03lX\n", output_value);

  cleanup_resources(ch422g_dev, I2C_NUM);
#endif

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(CONFIG_ENCODER_INA)
  host.max_freq_khz = 4000;       /* 4 MHz for first probe – increase later if needed */
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  host.max_freq_khz = sd_card_c5_max_freq_khz();
#elif defined(CONFIG_SHARED_TFT_SD_SPI)
  host.max_freq_khz = 4000;       /* more reliable init on shared SPI bus boards */
#endif
  /* select spi host slot for target */
  host.slot = sd_spi_host_id();
  ESP_LOGI(TAG, "SD SPI max clock: %d kHz", host.max_freq_khz);

  spi_bus_config_t bus_config = {
    .mosi_io_num = sd_card_manager.spi_mosi_pin,
    .miso_io_num = sd_card_manager.spi_miso_pin,
    .sclk_io_num = sd_card_manager.spi_clk_pin,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    /* Bulk transfers (app-package materialize, large asset writes) were
       chopped into 512-byte SPI transactions, each paying fixed CS-toggle/
       setup overhead regardless of clock speed. Raised to 2048 (not higher)
       since this board's internal RAM is extremely tight at the exact point
       large writes happen (as little as ~6KB free during boot materialize
       per observed logs) and the sdspi driver's per-transaction DMA buffer
       scales with this cap; left the SPI clock itself untouched since it was
       deliberately capped low after past timeout issues on this hardware. */
    .max_transfer_sz = 2048,
  };
  /* The SD SPI device configures CS when it attaches. Preconfiguring it here
   * causes a harmless but noisy GPIO matrix reassignment on JIT mounts. */

#ifdef CONFIG_IDF_TARGET_ESP32
  int dmabus = SPI_DMA_CH_AUTO;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  int dmabus = SPI_DMA_CH_AUTO;
#else
  int dmabus = SPI_DMA_CH_AUTO;
#endif

  bool bus_init_success = false;
  int sd_host_id = sd_spi_host_id();

  ESP_LOGI(TAG, "SD SPI host selected: %s (shared=%d, pins_match=%d)",
           sd_spi_host_name(sd_host_id),
           is_shared_display_sd_spi(), display_sd_spi_pins_match());

#if defined(CONFIG_IDF_TARGET_ESP32C5)
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      sd_spi_bus_track(SPI2_HOST, true);
    } else if (bus_ret == ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "SPI bus %d already initialized. Reusing existing bus.", SPI2_HOST);
      sd_spi_bus_track(SPI2_HOST, false);
    } else {
      shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }

#elif !defined(CONFIG_USE_TDECK)
#if defined(CONFIG_IDF_TARGET_ESP32)
  {
    ESP_LOGI(TAG, "ESP32: Attempting spi_bus_initialize on host %d (%s)", sd_host_id, sd_spi_host_name(sd_host_id));
    esp_err_t bus_ret = spi_bus_initialize(sd_host_id, &bus_config, dmabus);
    ESP_LOGI(TAG, "ESP32: spi_bus_initialize returned %s", esp_err_to_name(bus_ret));
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      sd_spi_bus_track(sd_host_id, true);
    } else if (bus_ret == ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "SPI bus %d already initialized. Reusing existing bus.", sd_host_id);
      sd_spi_bus_track(sd_host_id, false);
    } else {
      ESP_LOGE(TAG, "ESP32: spi_bus_initialize failed with %s", esp_err_to_name(bus_ret));
      shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  {
#if !defined(CONFIG_CROWPANEL_EPAPER_42)
    int host_id = sd_host_id;
#endif
#if defined(CONFIG_CROWPANEL_EPAPER_42)
    /* Unlike the generic S3 path, this board has a factory-proven fixed
     * topology: SD is on HSPI/SPI2. Do not run the free-host probe here,
     * because it intentionally prefers SPI3 on ordinary S3 boards. */
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      sd_spi_bus_track(SPI2_HOST, true);
    } else if (bus_ret == ESP_ERR_INVALID_STATE) {
      sd_spi_bus_track(SPI2_HOST, false);
    } else {
      shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
      printf("Failed to initialize factory SD SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
    sd_host_id = SPI2_HOST;
    host.slot = SPI2_HOST;
#elif defined(CONFIG_WITH_ETHERNET) || defined(CONFIG_HAS_NRF24)
    /* SPI3 is reserved for Ethernet/NRF24 on this config. Use SPI2 directly
     * (old pre-refactor behaviour) so we don't steal SPI3 from those peripherals.
     * INVALID_STATE means the bus is already up — reuse it. */
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      sd_spi_bus_track(SPI2_HOST, true);
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
    sd_host_id = SPI2_HOST;
    host_id = SPI2_HOST;
    host.slot = SPI2_HOST;
#else
    if (!is_shared_display_sd_spi()) {
      host_id = choose_free_s3_sd_spi_host(&bus_config, dmabus);
      if (host_id < 0) {
        shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
        printf("Failed to find a free SPI host for SD on ESP32-S3\n");
        return ESP_ERR_INVALID_STATE;
      }
      bus_init_success = true;
      sd_host_id = host_id;
    } else {
      esp_err_t bus_ret = spi_bus_initialize(host_id, &bus_config, dmabus);
      if (bus_ret == ESP_OK) {
        bus_init_success = true;
        sd_spi_bus_track(host_id, true);
      } else if (bus_ret == ESP_ERR_INVALID_STATE) {
        sd_spi_bus_track(host_id, false);
      } else if (bus_ret != ESP_ERR_INVALID_STATE) {
        shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
        printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
        return bus_ret;
      }
    }

    if (host_id >= 0 && !s_spi_bus_initialized && !bus_init_success) {
      /* host already initialized elsewhere; reuse it */
      sd_host_id = host_id;
    }
#endif
  }
#else
  {
    esp_err_t bus_ret = spi_bus_initialize(SPI2_HOST, &bus_config, dmabus);
    if (bus_ret == ESP_OK) {
      bus_init_success = true;
      sd_spi_bus_track(SPI2_HOST, true);
    } else if (bus_ret != ESP_ERR_INVALID_STATE) {
      shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, false);
      printf("Failed to initialize SPI bus: %s\n", esp_err_to_name(bus_ret));
      return bus_ret;
    }
  }
#endif
#endif

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
      .allocation_unit_size = 4 * 1024};

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_card_manager.spi_cs_pin;
#if defined(CONFIG_IDF_TARGET_ESP32)
  slot_config.host_id = sd_host_id;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  slot_config.host_id = sd_host_id;
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  slot_config.host_id = SPI2_HOST;
#else
  slot_config.host_id = SPI2_HOST;
#endif

  ret = sd_card_mount_spi_with_retry(&host, &slot_config, &mount_config);
  ESP_LOGD(TAG, "SD mount result: %s, shared_spi_guard_active=%d, display_was_suspended=%d",
           esp_err_to_name(ret), shared_spi_guard_active, display_was_suspended);
  shared_spi_guard_resume_lvgl_if_needed(shared_spi_guard_active, ret == ESP_OK);
  if (ret != ESP_OK) {
    ESP_LOGD(TAG, "Mount failed, bus_init_success=%d", bus_init_success);
    printf("Failed to mount filesystem: %s\n", esp_err_to_name(ret));
    (void)bus_init_success;
    (void)keep_bus_on_failure;
    sd_spi_bus_release_if_tracked();
    if (display_was_suspended) {
      ESP_LOGD(TAG, "Calling display_spi_resume_after_sd()");
      display_spi_resume_after_sd();
    }
    sd_card_manager.card = NULL;
    toast_show("SD mount failed", TOAST_ERROR);
    return ret;
  }

  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SPI;
  sdmmc_card_print_info(sd_card_manager.card);

  sd_card_setup_directory_structure();

  if (gating_template) {
    sd_card_update_cached_stats();
    sd_card_unmount_with_context(SD_UNMOUNT_CONTEXT_JIT);
    if (display_was_suspended) {
      ESP_LOGD(TAG, "Calling display_spi_resume_after_sd()");
      display_spi_resume_after_sd();
    }
    return ESP_OK;
  }

  return ESP_OK;
#endif

  // Common failure handling
  if (ret != ESP_OK) {
      // Restore backup config if init failed with loaded pins
      sd_card_manager = backup_config;
      printf("SD Card init failed with loaded pins. Check configuration.\n");
      toast_show("SD mount failed", TOAST_ERROR);
      // Optionally: attempt init with known defaults here as a fallback?
      return ret;
  }

  sd_card_manager.is_initialized = true;
  sdmmc_card_print_info(sd_card_manager.card);

  sd_card_setup_directory_structure();

  return ESP_OK;
}

// mount sd just-in-time for short io, then unmount after
esp_err_t sd_card_mount_for_flush(bool *display_was_suspended) {
  SemaphoreHandle_t jit_mutex = sd_card_get_jit_mutex();

  if (!s_sd_log_levels_tuned) {
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);
    s_sd_log_levels_tuned = true;
  }

  if (display_was_suspended) *display_was_suspended = false;
  if (jit_mutex == NULL) {
    return ESP_ERR_NO_MEM;
  }
  if (s_msc_owns_sd) {
    ESP_LOGW(TAG, "sd_card_mount_for_flush: card is in USB passthrough mode");
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTakeRecursive(jit_mutex, portMAX_DELAY) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  // If already mounted, nothing to do
  if (sd_card_manager.is_initialized) {
    if (s_sd_jit_mount_depth > 0) {
      ++s_sd_jit_mount_depth;
    }
    xSemaphoreGiveRecursive(jit_mutex);
    return ESP_OK;
  }

#if defined(CONFIG_USING_SPI)
  /* Only time-multiplex display SPI when the active display really shares the
   * SD bus. A PARLIO display leaves SPI2 available for a permanent SD mount. */
  if (sd_card_uses_shared_display_spi()) {
    bool display_suspended = display_spi_suspend_for_sd();
    if (display_was_suspended) *display_was_suspended = display_suspended;
  }
  // Minimal SPI mount path for flush: reuse sd_card_init SPI branch logic
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = sd_spi_host_id();
#if defined(CONFIG_IDF_TARGET_ESP32C5)
  host.max_freq_khz = sd_card_c5_max_freq_khz();
#endif

  spi_bus_config_t bus_config = {
    .mosi_io_num = sd_card_manager.spi_mosi_pin,
    .miso_io_num = sd_card_manager.spi_miso_pin,
    .sclk_io_num = sd_card_manager.spi_clk_pin,
    .max_transfer_sz = 2048,  /* see rationale in sd_card_init's bus_config above */
  };

  /* The SD SPI device configures CS when it attaches. Preconfiguring it here
   * causes a harmless but noisy GPIO matrix reassignment on JIT mounts. */

#if defined(CONFIG_IDF_TARGET_ESP32)
  int dmabus = SPI_DMA_CH_AUTO;
#else
  int dmabus = SPI_DMA_CH_AUTO;
#endif

  if (!s_spi_bus_initialized) {
    int host_id = sd_spi_host_id();
    esp_err_t bus_ret = spi_bus_initialize(host_id, &bus_config, dmabus);
    if (bus_ret != ESP_OK && bus_ret != ESP_ERR_INVALID_STATE) {
      if (display_was_suspended && *display_was_suspended) display_spi_resume_after_sd();
      xSemaphoreGiveRecursive(jit_mutex);
      return bus_ret;
    }
    if (bus_ret == ESP_OK) {
      sd_spi_bus_track(host_id, true);
    } else {
      sd_spi_bus_track(host_id, false);
    }
  }

  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
      .allocation_unit_size = 4 * 1024};

  sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot_config.gpio_cs = sd_card_manager.spi_cs_pin;
  slot_config.host_id = sd_spi_host_id();

  esp_err_t ret = sd_card_mount_spi_with_retry(&host, &slot_config, &mount_config);
  if (ret != ESP_OK) {
    if (!sd_keep_spi_bus_for_board()) {
      sd_spi_bus_release_if_tracked();
    }
    if (display_was_suspended && *display_was_suspended) display_spi_resume_after_sd();
    xSemaphoreGiveRecursive(jit_mutex);
    return ret;
  }
  sd_card_manager.is_initialized = true;
  s_mount_type = MOUNT_SPI;
  s_sd_jit_mount_depth = 1;
  s_sd_jit_display_suspended = display_was_suspended && *display_was_suspended;
  sd_card_update_cached_stats();
  s_next_unmount_tick = xTaskGetTickCount() + pdMS_TO_TICKS(300);
  status_display_show_status("SD Active");
  xSemaphoreGiveRecursive(jit_mutex);
  return ESP_OK;
#else
  // For SDMMC, if not mounted try normal init path quickly
  xSemaphoreGiveRecursive(jit_mutex);
  return sd_card_init();
#endif
}

void sd_card_unmount_after_flush(bool display_was_suspended) {
  SemaphoreHandle_t jit_mutex = sd_card_get_jit_mutex();
  bool resume_display = false;

  (void)display_was_suspended;
  if (jit_mutex == NULL) {
    return;
  }
  if (xSemaphoreTakeRecursive(jit_mutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

  if (s_sd_jit_mount_depth == 0) {
    xSemaphoreGiveRecursive(jit_mutex);
    return;
  }

  --s_sd_jit_mount_depth;
  if (s_sd_jit_mount_depth == 0) {
    resume_display = s_sd_jit_display_suspended;
    s_sd_jit_display_suspended = false;
    if (sd_card_manager.is_initialized) {
      sd_card_unmount_with_context(SD_UNMOUNT_CONTEXT_JIT);
    }
  }

  xSemaphoreGiveRecursive(jit_mutex);

  if (resume_display) {
    display_spi_resume_after_sd();
  }
}

bool sd_card_needs_jit_mount(void) {
#if defined(CONFIG_USE_C5_PARLIO_DISPLAY)
    return false;
#endif
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    /* Boards where the SD card shares SPI pins/host with the LVGL display
     * cannot keep both attached simultaneously on ESP32-C5 (single SPI host).
     * Force JIT mount/unmount so the display is restored between SD use. */
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0;
#else
    return false;
#endif
}

bool sd_card_uses_shared_display_spi(void) {
    return is_shared_display_sd_spi();
}

bool sd_card_jit_begin(bool *display_was_suspended, bool ensure_dirs) {
    if (display_was_suspended) *display_was_suspended = false;

    if (!sd_card_needs_jit_mount()) {
        return true;
    }

    esp_err_t mount_err = sd_card_mount_for_flush(display_was_suspended);
    if (mount_err != ESP_OK) {
        ESP_LOGE(TAG, "sd_card_jit_begin: mount failed: %s", esp_err_to_name(mount_err));
        return false;
    }

    if (ensure_dirs) {
        esp_err_t dir_err = sd_card_setup_directory_structure();
        if (dir_err != ESP_OK) {
            ESP_LOGW(TAG, "sd_card_jit_begin: setup_directory_structure failed: %s",
                     esp_err_to_name(dir_err));
        }
    }

    return true;
}

void sd_card_jit_end(bool display_was_suspended) {
    if (!sd_card_needs_jit_mount()) {
        return;
    }
    sd_card_unmount_after_flush(display_was_suspended);
}

void sd_card_unmount_with_context(sd_unmount_context_t context) {
#if defined(CONFIG_IS_S3TWATCH) || defined(CONFIG_IS_ATOMS3R)
  if (s_virtual_storage_mounted) {
    unmount_virtual_storage();
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    if (s_crowpanel_sd_power) {
      sd_pwr_ctrl_del_on_chip_ldo(s_crowpanel_sd_power);
      s_crowpanel_sd_power = NULL;
    }
#endif
    
    // Show appropriate status based on context
    switch (context) {
      case SD_UNMOUNT_CONTEXT_JIT:
        status_display_show_status("Virtual SD Idle");
        break;
      case SD_UNMOUNT_CONTEXT_USER:
        status_display_show_status("Virtual SD Off");
        break;
      case SD_UNMOUNT_CONTEXT_ERROR:
        status_display_show_status("Virtual SD Err");
        break;
      case SD_UNMOUNT_CONTEXT_SHUTDOWN:
        // Don't show status during shutdown
        break;
      default:
        status_display_show_status("Virtual SD Off");
        break;
    }
    return;
  }
#endif

#if SOC_SDMMC_HOST_SUPPORTED && SOC_SDMMC_USE_GPIO_MATRIX
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdcard_unmount("/mnt", sd_card_manager.card);
    sd_spi_release_cs_pin();
    if (s_mount_type == MOUNT_SPI && !sd_keep_spi_bus_for_board()) {
      sd_spi_bus_release_if_tracked();
    }
    if (context != SD_UNMOUNT_CONTEXT_JIT) {
      printf("SD card unmounted\n");
    }
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    if (s_crowpanel_sd_power) {
      sd_pwr_ctrl_del_on_chip_ldo(s_crowpanel_sd_power);
      s_crowpanel_sd_power = NULL;
    }
#endif
    s_sd_jit_mount_depth = 0;
    s_sd_jit_display_suspended = false;
    
    // Show appropriate status based on context
    switch (context) {
      case SD_UNMOUNT_CONTEXT_JIT:
        break;
      case SD_UNMOUNT_CONTEXT_USER:
        status_display_show_status("SD Unmounted");
        toast_show("SD card unmounted", TOAST_INFO);
        break;
      case SD_UNMOUNT_CONTEXT_ERROR:
        status_display_show_status("SD Error");
        toast_show("SD unmount error", TOAST_WARN);
        break;
      case SD_UNMOUNT_CONTEXT_SHUTDOWN:
        break;
      default:
        status_display_show_status("SD Unmounted");
        break;
    }
  }
#else
  if (sd_card_manager.is_initialized) {
    esp_vfs_fat_sdcard_unmount("/mnt", sd_card_manager.card);
    sd_spi_release_cs_pin();
    if (!sd_keep_spi_bus_for_board()) {
      sd_spi_bus_release_if_tracked();
    }
    if (context != SD_UNMOUNT_CONTEXT_JIT) {
      printf("SD card unmounted\n");
    }
    sd_card_manager.is_initialized = false;
    sd_card_manager.card = NULL;
    s_mount_type = MOUNT_NONE;
    s_sd_jit_mount_depth = 0;
    s_sd_jit_display_suspended = false;
    
    // Show appropriate status based on context
    switch (context) {
      case SD_UNMOUNT_CONTEXT_JIT:
        status_display_show_status("SD Idle");
        break;
      case SD_UNMOUNT_CONTEXT_USER:
        status_display_show_status("SD Unmounted");
        toast_show("SD card unmounted", TOAST_INFO);
        break;
      case SD_UNMOUNT_CONTEXT_ERROR:
        status_display_show_status("SD Error");
        toast_show("SD unmount error", TOAST_WARN);
        break;
      case SD_UNMOUNT_CONTEXT_SHUTDOWN:
        // Don't show status during shutdown
        break;
      default:
        status_display_show_status("SD Unmounted");
        break;
    }
  } else {
    status_display_show_status("SD Not Mounted");
  }
#endif
}

void sd_card_unmount(void) {
  sd_card_unmount_with_context(SD_UNMOUNT_CONTEXT_USER);
}

bool sd_card_usb_msc_active(void) {
  return s_msc_owns_sd;
}

esp_err_t sd_card_suspend_for_usb_msc(sdmmc_card_t **out_card) {
  if (s_msc_owns_sd) {
    return ESP_ERR_INVALID_STATE;
  }
  if (!sd_card_manager.is_initialized || sd_card_manager.card == NULL) {
    ESP_LOGE(TAG, "USB MSC suspend: card not mounted");
    return ESP_ERR_INVALID_STATE;
  }
  if (sd_card_is_virtual_storage()) {
    ESP_LOGE(TAG, "USB MSC suspend: virtual storage is not supported");
    return ESP_ERR_NOT_SUPPORTED;
  }

  BYTE pdrv = ff_diskio_get_pdrv_card(sd_card_manager.card);
  if (pdrv == 0xff) {
    ESP_LOGE(TAG, "USB MSC suspend: card has no FatFS drive registered");
    return ESP_ERR_INVALID_STATE;
  }

  /* Release the VFS + diskio registration while keeping the card host
   * controller and card handle alive so the MSC class can do raw sector I/O.
   * esp_vfs_fat_sdcard_unmount() must NOT be used here: it deinits the host
   * and frees the card. */
  char drv[3] = {(char)('0' + pdrv), ':', 0};
  f_mount(NULL, drv, 0);
  ff_diskio_unregister(pdrv);
  esp_err_t err = esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_NOT_FOUND) {
    ESP_LOGE(TAG, "USB MSC suspend: failed to unregister VFS: %s", esp_err_to_name(err));
    return err;
  }

  s_msc_card = sd_card_manager.card;
  s_msc_pdrv = pdrv;
  s_msc_owns_sd = true;
  sd_card_manager.is_initialized = false;
  ESP_LOGI(TAG, "SD handed to USB MSC (pdrv=%u)", pdrv);

  if (out_card) {
    *out_card = s_msc_card;
  }
  return ESP_OK;
}

esp_err_t sd_card_resume_from_usb_msc(void) {
  if (!s_msc_owns_sd || s_msc_card == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  sdmmc_card_t *card = s_msc_card;
  BYTE pdrv = s_msc_pdrv;
  s_msc_card = NULL;
  s_msc_pdrv = 0xff;
  s_msc_owns_sd = false;

  /* Re-register the diskio + VFS on the same drive number the original mount
   * used. esp_vfs_fat_sdcard_unmount() freed its context during suspend, so
   * this recreates the mount from scratch without re-probing the card. */
  ff_diskio_register_sdmmc(pdrv, card);
  char drv[3] = {(char)('0' + pdrv), ':', 0};
  esp_vfs_fat_conf_t conf = {
      .base_path = SD_MOUNT_POINT,
      .fat_drive = drv,
      .max_files = 3,
  };
  FATFS *fs = NULL;
  esp_err_t err = esp_vfs_fat_register(&conf, &fs);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "USB MSC resume: VFS register failed: %s", esp_err_to_name(err));
    ff_diskio_unregister(pdrv);
    sd_card_manager.card = NULL;
    return err;
  }

  FRESULT res = f_mount(fs, drv, 1);
  if (res != FR_OK) {
    ESP_LOGE(TAG, "USB MSC resume: f_mount failed (%d)", res);
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path(SD_MOUNT_POINT);
    ff_diskio_unregister(pdrv);
    sd_card_manager.card = NULL;
    return ESP_FAIL;
  }

  sd_card_manager.is_initialized = true;
  sd_card_manager.card = card;
  ESP_LOGI(TAG, "SD remounted after USB MSC (pdrv=%u)", pdrv);
  return ESP_OK;
}

esp_err_t sd_card_append_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot append to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "ab");
  if (f == NULL) {
    printf("Failed to open file for appending\n");
    return ESP_FAIL;
  }
  size_t written = fwrite(data, 1, size, f);
  int write_failed = ferror(f);
  fclose(f);
  if (write_failed || written != size) {
    printf("Failed to append full data to file: %s (%zu/%zu bytes)\n", path, written,
           size);
    return ESP_FAIL;
  }
  printf("Data appended to file: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_write_file(const char *path, const void *data, size_t size) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot write to file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "wb");
  if (f == NULL) {
    printf("Failed to open file for writing\n");
    return ESP_FAIL;
  }
  size_t written = fwrite(data, 1, size, f);
  int write_failed = ferror(f);
  fclose(f);
  if (write_failed || written != size) {
    printf("Failed to write full file: %s (%zu/%zu bytes)\n", path, written, size);
    return ESP_FAIL;
  }
  printf("File written: %s\n", path);
  return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot read from file.\n");
    return ESP_FAIL;
  }

  FILE *f = fopen(path, "r");
  if (f == NULL) {
    printf("Failed to open file for reading\n");
    return ESP_FAIL;
  }
  char line[64];
  while (fgets(line, sizeof(line), f) != NULL) {
    printf("%s", line);
  }
  fclose(f);
  printf("File read: %s\n", path);
  return ESP_OK;
}

static bool has_full_permissions(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if ((st.st_mode & 0777) == 0777) {
      return true;
    }
  }
  return false;
}

esp_err_t sd_card_create_directory(const char *path) {
  if (!sd_card_manager.is_initialized) {
    printf("Storage is not initialized. Cannot create directory.\n");
    return ESP_FAIL;
  }

  if (sd_card_exists(path)) {
    if (!has_full_permissions(path)) {
      printf("Directory %s does not have full permissions. Deleting and "
             "recreating.\n",
             path);

      if (rmdir(path) != 0) {
        printf("Failed to remove directory: %s\n", path);
        return ESP_FAIL;
      }

      int res = mkdir(path, 0777);
      if (res != 0) {
        printf("Failed to create directory: %s\n", path);
        return ESP_FAIL;
      }

    } else {
      return ESP_OK;
    }
    return ESP_OK;
  }

  int res = mkdir(path, 0777);
  if (res != 0) {
    printf("Failed to create directory: %s\n", path);
    return ESP_FAIL;
  }

  return ESP_OK;
}

bool sd_card_exists(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    return true;
  } else {
    return false;
  }
}

static esp_err_t ensure_sd_dir_exists(const char *path) {
  if (!sd_card_exists(path)) {
    esp_err_t ret = sd_card_create_directory(path);
    if (ret != ESP_OK) {
      printf("Failed to create directory %s: %s\n", path, esp_err_to_name(ret));
      return ret;
    }
  }
  return ESP_OK;
}

esp_err_t sd_card_setup_directory_structure() {
  const char *root_dir = SD_GHOSTESP_ROOT;
  const char *logs_dir = SD_DIR_LOGS;
  const char *coredumps_dir = SD_DIR_COREDUMPS;
  const char *debug_dir = SD_DIR_DEBUG;
  const char *pcaps_dir = SD_DIR_PCAPS;
  const char *captures_dir = SD_DIR_CAPTURES;
  const char *scans_dir = SD_DIR_SCANS;
  const char *sweeps_dir = SD_DIR_SWEEPS;
  const char *gps_dir = SD_DIR_GPS;
  const char *ghostchi_dir = SD_DIR_GHOSTCHI;
  const char *ghostchi_pcaps_dir = SD_DIR_GHOSTCHI_PCAPS;
  const char *ghostchi_sessions_dir = SD_DIR_GHOSTCHI_SESSIONS;
  const char *games_dir = SD_GHOSTESP_ROOT "/games";
  const char *apps_dir = SD_DIR_APPS;
  const char *app_cache_dir = SD_DIR_APP_CACHE;
  const char *appdata_dir = SD_DIR_APPDATA;
  const char *scripts_dir = SD_DIR_SCRIPTS;
  const char *scriptdata_dir = SD_DIR_SCRIPTDATA;
  const char *downloads_dir = SD_DIR_DOWNLOADS;
  const char *comics_dir = SD_DIR_COMICS;
  const char *themes_dir = SD_DIR_THEMES;
  const char *active_theme_dir = SD_DIR_THEMES "/active";
  const char *evil_portal_dir = SD_GHOSTESP_ROOT "/evil_portal";
  const char *evil_portal_portals_dir = SD_GHOSTESP_ROOT "/evil_portal/portals";
  const char *universals_dir = SD_GHOSTESP_ROOT "/infrared/universals";
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
  const char *nfc_dir = "/mnt/ghostesp/nfc";
#endif

  esp_err_t ret = ensure_sd_dir_exists(root_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(games_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(apps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(app_cache_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(appdata_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(scripts_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(scriptdata_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(downloads_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(comics_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(themes_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(active_theme_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(gps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(logs_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(coredumps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(debug_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(pcaps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(captures_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(scans_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(sweeps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(ghostchi_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(ghostchi_pcaps_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(ghostchi_sessions_dir);
  if (ret != ESP_OK) return ret;

  // Create evil_portal directory
  ret = ensure_sd_dir_exists(evil_portal_dir);
  if (ret != ESP_OK) return ret;

  // Create evil_portal/portals directory
  ret = ensure_sd_dir_exists(evil_portal_portals_dir);
  if (ret != ESP_OK) return ret;

  const char *dns_sinkhole_dir = "/mnt/ghostesp/dns_sinkhole";
  ret = ensure_sd_dir_exists(dns_sinkhole_dir);
  if (ret != ESP_OK) return ret;

  const char *infrared_dir = "/mnt/ghostesp/infrared";
  ret = ensure_sd_dir_exists(infrared_dir);
  if (ret != ESP_OK) return ret;

  const char *remotes_dir = "/mnt/ghostesp/infrared/remotes";
  ret = ensure_sd_dir_exists(remotes_dir);
  if (ret != ESP_OK) return ret;

  ret = ensure_sd_dir_exists(universals_dir);
  if (ret != ESP_OK) return ret;

#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_CHAMELEON)
  ret = ensure_sd_dir_exists(nfc_dir);
  if (ret != ESP_OK) return ret;
#endif

#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
  const char *badusb_dir = "/mnt/ghostesp/badusb";
  ret = ensure_sd_dir_exists(badusb_dir);
  if (ret != ESP_OK) return ret;
#endif

#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
  const char *subghz_dir = "/mnt/ghostesp/subghz";
  ret = ensure_sd_dir_exists(subghz_dir);
  if (ret != ESP_OK) return ret;
#endif

#ifdef CONFIG_HAS_AUDIO_PLAYER
  const char *audio_dir = "/mnt/ghostesp/audio";
  ret = ensure_sd_dir_exists(audio_dir);
  if (ret != ESP_OK) return ret;
#endif

  return ESP_OK;
}

// New SD card pin configuration functions

esp_err_t sd_card_set_mmc_pins(int clk, int cmd, int d0, int d1, int d2, int d3) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.clkpin = clk;
  sd_card_manager.cmdpin = cmd;
  sd_card_manager.d0pin = d0;
  sd_card_manager.d1pin = d1;
  sd_card_manager.d2pin = d2;
  sd_card_manager.d3pin = d3;
  
  printf("SD card MMC pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_set_spi_pins(int cs, int clk, int miso, int mosi) {
  if (sd_card_manager.is_initialized) {
    printf("Cannot change pins while SD card is initialized. Unmount first.\n");
    return ESP_FAIL;
  }
  
  sd_card_manager.spi_cs_pin = cs;
  sd_card_manager.spi_clk_pin = clk;
  sd_card_manager.spi_miso_pin = miso;
  sd_card_manager.spi_mosi_pin = mosi;
  
  printf("SD card SPI pins updated. Restart or reinitialize to apply changes.\n");
  return ESP_OK;
}

esp_err_t sd_card_save_config() {
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    return err;
  }

  // Write MMC pins
  err = nvs_set_i32(nvs_handle, "mmc_clk", sd_card_manager.clkpin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_cmd", sd_card_manager.cmdpin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d0", sd_card_manager.d0pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d1", sd_card_manager.d1pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d2", sd_card_manager.d2pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "mmc_d3", sd_card_manager.d3pin);
  if (err != ESP_OK) goto nvs_write_error;

  // Write SPI pins
  err = nvs_set_i32(nvs_handle, "spi_cs", sd_card_manager.spi_cs_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_clk", sd_card_manager.spi_clk_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_miso", sd_card_manager.spi_miso_pin);
  if (err != ESP_OK) goto nvs_write_error;
  err = nvs_set_i32(nvs_handle, "spi_mosi", sd_card_manager.spi_mosi_pin);
  if (err != ESP_OK) goto nvs_write_error;

  // Commit changes
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    printf("Error (%s) committing NVS changes!\n", esp_err_to_name(err));
  } else {
    printf("SD card pin configuration saved to NVS.\n");
  }

  // Close NVS handle and return
  nvs_close(nvs_handle);
  return err; // Return the result of nvs_commit or nvs_open

  // error handling label for write failures (kept inside function scope)
nvs_write_error:
  printf("Error (%s) writing NVS key!\n", esp_err_to_name(err));
  nvs_close(nvs_handle);
  return err;
}

esp_err_t sd_card_load_config() {
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
  // CrowPanel wiring is fixed on the PCB. Do not let generic saved pin values
  // obscure or override the board profile selected at build time.
  sd_card_manager.clkpin = CONFIG_SD_MMC_CLK;
  sd_card_manager.cmdpin = CONFIG_SD_MMC_CMD;
  sd_card_manager.d0pin = CONFIG_SD_MMC_D0;
  printf("CrowPanel SD wiring loaded from board profile.\n");
  return ESP_OK;
#endif
  nvs_handle_t nvs_handle;
  esp_err_t err;

  // Open NVS namespace
  err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        printf("No saved SD pin config - using defaults.\n");
        // Keep default pins already set in sd_card_manager struct definition
        return ESP_OK; // Not an error if first boot
    } else {
        printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
        return err;
    }
  }

  int32_t temp_val;

  // Read MMC pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "mmc_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.clkpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_cmd", &temp_val);
  if (err == ESP_OK) sd_card_manager.cmdpin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d0", &temp_val);
  if (err == ESP_OK) sd_card_manager.d0pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d1", &temp_val);
  if (err == ESP_OK) sd_card_manager.d1pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d2", &temp_val);
  if (err == ESP_OK) sd_card_manager.d2pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "mmc_d3", &temp_val);
  if (err == ESP_OK) sd_card_manager.d3pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Read SPI pins (default to current value if not found in NVS)
  err = nvs_get_i32(nvs_handle, "spi_cs", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_cs_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_clk", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_clk_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_miso", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_miso_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  err = nvs_get_i32(nvs_handle, "spi_mosi", &temp_val);
  if (err == ESP_OK) sd_card_manager.spi_mosi_pin = temp_val;
  else if (err != ESP_ERR_NVS_NOT_FOUND) goto read_error;

  // Success path
  printf("SD card pin configuration loaded from NVS.\n");
  nvs_close(nvs_handle);
  return ESP_OK;

read_error:
  printf("Error (%s) reading NVS key! Using default SD pins.\n", esp_err_to_name(err));
  nvs_close(nvs_handle);
  // Keep default pins already set in sd_card_manager struct definition
  return err; // Return the actual read error
}

void sd_card_print_config() {
#if defined(CONFIG_IS_S3TWATCH) || defined(CONFIG_IS_ATOMS3R)
  if (s_virtual_storage_mounted) {
    printf("Storage Configuration: Virtual Flash Storage\n");
    printf("Mount Point: /mnt\n");
    printf("Storage Type: Internal Flash Partition\n");
    
    const esp_partition_t* storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (storage_partition) {
      printf("Partition Size: %lu KB\n", (unsigned long)(storage_partition->size / 1024));
      printf("Partition Offset: 0x%lx\n", (unsigned long)storage_partition->address);
    }
    return;
  }
#endif

#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
  printf("SD pins: SDMMC slot 0, 1-bit (CLK %d, CMD %d, D0 %d), max 10 MHz\n",
         CONFIG_SD_MMC_CLK, CONFIG_SD_MMC_CMD, CONFIG_SD_MMC_D0);
  return;
#endif

  printf("SD pins: MMC(CLK %d, CMD %d, D0 %d, D1 %d, D2 %d, D3 %d) "
         "SPI(CS %d, CLK %d, MISO %d, MOSI %d)\n",
         sd_card_manager.clkpin, sd_card_manager.cmdpin,
         sd_card_manager.d0pin, sd_card_manager.d1pin,
         sd_card_manager.d2pin, sd_card_manager.d3pin,
         sd_card_manager.spi_cs_pin, sd_card_manager.spi_clk_pin,
         sd_card_manager.spi_miso_pin, sd_card_manager.spi_mosi_pin);
}

bool sd_card_is_virtual_storage() {
#if defined(CONFIG_IS_S3TWATCH) || defined(CONFIG_IS_ATOMS3R)
  return s_virtual_storage_mounted;
#else
  return false;
#endif
}

int get_evil_portal_list(char portal_names[MAX_PORTALS][MAX_PORTAL_NAME]) {
    const char *portal_dir = "/mnt/ghostesp/evil_portal/portals";
    DIR *dir = opendir(portal_dir);
    if (!dir){
        ESP_LOGW(TAG, "Failed to open directory: %s\n", portal_dir);
        return -1; // Return -1 if directory cannot be opened
    }
    ESP_LOGI(TAG, "Listing portals in directory: %s\n", portal_dir);
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) && count < MAX_PORTALS) {
        bool is_reg = false;
        if (entry->d_type == DT_REG) {
            is_reg = true;
        } else if (entry->d_type == DT_UNKNOWN) {
            // fallback to stat when d_type is unknown
            char fullpath[256];
            int written = snprintf(fullpath, sizeof(fullpath), "%s/%s", portal_dir, entry->d_name);
            if (written > 0 && written < (int)sizeof(fullpath)) {
                struct stat st;
                if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
                    is_reg = true;
                }
            }
        }

        if (is_reg) {
            const char *dot = strrchr(entry->d_name, '.');
            if (dot && strcmp(dot, ".html") == 0) {
                strncpy(portal_names[count], entry->d_name, MAX_PORTAL_NAME - 1);
                portal_names[count][MAX_PORTAL_NAME - 1] = '\0';
                count++;
            }
        }
    }
    closedir(dir);
    return count;
}

int sd_card_list_dir_paged(const char *dir_path, const char *ext,
                            int offset, int max_count,
                            char (*out_names)[MAX_PORTAL_NAME],
                            bool *out_has_more) {
    if (out_has_more) *out_has_more = false;
    if (!dir_path || !out_names || max_count <= 0) return -1;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGW(TAG, "sd_card_list_dir_paged: failed to open '%s'", dir_path);
        return -1;
    }

    struct dirent *entry;
    int skipped   = 0;
    int collected = 0;

    while ((entry = readdir(dir)) != NULL) {
        /* ---- regular-file check (FAT may return DT_UNKNOWN) ---- */
        bool is_reg = (entry->d_type == DT_REG);
        if (!is_reg && entry->d_type == DT_UNKNOWN) {
            char fullpath[256];
            int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_path, entry->d_name);
            if (n > 0 && n < (int)sizeof(fullpath)) {
                struct stat st;
                if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) is_reg = true;
            }
        }
        if (!is_reg) continue;

        /* ---- optional extension filter ---- */
        if (ext) {
            const char *dot = strrchr(entry->d_name, '.');
            if (!dot || strcmp(dot, ext) != 0) continue;
        }

        /* ---- pagination ---- */
        if (skipped < offset) { skipped++; continue; }

        if (collected < max_count) {
            strncpy(out_names[collected], entry->d_name, MAX_PORTAL_NAME - 1);
            out_names[collected][MAX_PORTAL_NAME - 1] = '\0';
            collected++;
        } else {
            /* one extra entry confirms a following page exists */
            if (out_has_more) *out_has_more = true;
            break;
        }
    }

    closedir(dir);
    return collected;
}
