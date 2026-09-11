#include "boot_banner_text.h"
#include "core/shell.h"
#include "core/commandline.h"
#include "core/callbacks.h"
#include "core/serial_manager.h"
#include "core/system_manager.h"
#include "core/ghostesp_version.h"
#include "core/memory_debug.h"
#include "managers/ap_manager.h"
#include "managers/display_manager.h"
#include "managers/ghostchi_manager.h"
#include "managers/ghostchi_mood.h"
#include "managers/haptic_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "managers/ota_manager.h"
#include "managers/peer_ota_manager.h"
#include "managers/peer_storage_manager.h"
#include "managers/self_ota_manager.h"
#include "managers/crash_reporter.h"
#include "managers/wifi_manager.h"
#include "gui/asset_pack.h"
#include "gui/toast.h"
#include "managers/plugin_manager.h"
#include "esp_wifi.h"
#include "core/esp_comm_manager.h"
#include "core/ghostlink_bench.h"
#include "managers/status_display_manager.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_hosted.h"
#include "managers/p4_slave_ota_manager.h"
#include "managers/ghost_raw_radio.h"
#endif
#include "vendor/drivers/aw9523.h"
#include "vendor/drivers/pcf8563.h"
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#include "managers/ble_manager.h"
#include "managers/ble_bridge_manager.h"
#endif
#include <esp_log.h>
#include "esp_random.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "managers/usb_keyboard_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/lora_manager.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif

#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_comm_handler.h"
#endif

#ifdef CONFIG_HAS_BADUSB
#include "managers/badusb_manager.h"
#endif
#ifdef CONFIG_HAS_BADBLE
#include "managers/badble_manager.h"
#endif

#ifdef CONFIG_HAS_CAMERA
#include "managers/motion_detector_manager.h"
#include "managers/camera_stream_manager.h"
#endif
#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER) || defined(CONFIG_HAS_CROWPANEL_NS4168)
#include "managers/audio_receiver_manager.h"
#endif

#ifdef CONFIG_WITH_SCREEN
#include "managers/views/splash_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/tdongle_status_screen.h"
#include "gui/popup.h"
#include "gui/screen_layout.h"
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
#include "managers/views/nrf24_analyzer_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/subghz_remote_manager.h"
#endif
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
#include "managers/status_display_manager.h"
#endif

#ifdef CONFIG_WITH_SCREEN
static bool use_tdongle_status_startup(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-S3") == 0 ||
           strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0;
#else
    return false;
#endif
}

static void boot_status_set_progress(float pct, const char *label) {
    if (use_tdongle_status_startup()) {
        (void)pct;
        tdongle_status_show_status(label ? label : "Booting");
        return;
    }
    splash_set_progress(pct, label);
}

static void boot_status_signal_completion(void) {
    if (use_tdongle_status_startup()) {
        tdongle_status_show_status("Ready");
        return;
    }
    splash_signal_completion();
}

static void apply_main_menu_background_cb(void *arg) {
    (void)arg;
    gui_screen_apply_background(main_menu_view.root);
}

#if GHOSTESP_OTA_SUPPORTED
static char s_self_ota_boot_error[160];

static void self_ota_boot_error_toast_timer_cb(lv_timer_t *timer) {
    lv_timer_del(timer);
    if (s_self_ota_boot_error[0] != '\0') {
        toast_show(s_self_ota_boot_error, TOAST_WARN);
        s_self_ota_boot_error[0] = '\0';
    }
}

static void schedule_self_ota_boot_error_toast_cb(void *arg) {
    (void)arg;
    if (s_self_ota_boot_error[0] == '\0') return;

    lv_timer_t *timer = lv_timer_create(self_ota_boot_error_toast_timer_cb, 1200, NULL);
    if (timer) {
        lv_timer_set_repeat_count(timer, 1);
    } else {
        toast_show(s_self_ota_boot_error, TOAST_WARN);
        s_self_ota_boot_error[0] = '\0';
    }
}

static void maybe_schedule_self_ota_boot_error_popup(void) {
    if (!self_ota_manager_is_supported()) return;

    SelfOtaStatus status = self_ota_manager_get_status();
    if (status.state != SELF_OTA_STATE_FAILED || status.error_msg[0] == '\0') return;

    snprintf(s_self_ota_boot_error, sizeof(s_self_ota_boot_error), "%s", status.error_msg);
    display_manager_run_on_lvgl(schedule_self_ota_boot_error_toast_cb, NULL);
}
#endif

static void splash_asset_pack_progress_cb(float pct, const char *stage, void *user) {
    (void)user;
    boot_status_set_progress(pct, stage);
}

static void splash_plugin_progress_cb(float pct, int files_scanned, int files_total, void *user) {
    (void)files_scanned;
    (void)files_total;
    (void)user;
    if (pct < 0.0f) {
        boot_status_set_progress(-1.0f, "Checking apps");
    } else {
        boot_status_set_progress(pct, "Checking apps");
    }
}
#endif

#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_visualizer.h"
#endif
#if defined(CONFIG_HAS_ES7210_MIC) || defined(CONFIG_HAS_AW88298_SPEAKER)
#include "managers/m5_audio_codec.h"
#endif

// Helper macro for measuring RAM usage (internal + PSRAM) around a feature init call
#define MEASURE_INIT_RAM(name, init_call) do { \
    size_t before_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
    size_t before_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); \
    ESP_LOGI(TAG, "[%s] before: internal_free=%d bytes, psram_free=%d bytes", name, (int)before_int, (int)before_psram); \
    init_call; \
    size_t after_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); \
    size_t after_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM); \
    ESP_LOGI(TAG, "[%s] after: internal_free=%d bytes (used=%d), psram_free=%d bytes (used=%d)", name, \
             (int)after_int, (int)(before_int - after_int), \
             (int)after_psram, (int)(before_psram - after_psram)); \
} while(0)

RGBManager_t rgb_manager;  // Global instance for entire project

int ieee80211_raw_frame_sanity_check(int32_t arg, int32_t arg2, int32_t arg3) { return 0; }
static const char *TAG = "Main.c";

/* timegm() is not available in ESP-IDF's newlib for ESP32-C5 (RISC-V).
 * Provide a minimal implementation that both main.c (RTC sync) and
 * minmea.c (GPS timestamp conversion) can link against. */
time_t timegm(struct tm *tm) {
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    int d = tm->tm_mday;
    // Days from 1970-01-01 to year y, month m, day d
    static const int days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    int64_t days = (int64_t)(y - 1970) * 365 + (y - 1969) / 4
                 - (y - 1901) / 100 + (y - 1601) / 400
                 + days_before_month[m - 1] + d - 1;
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) {
        days++;
    }
    return (time_t)(days * 86400 + tm->tm_hour * 3600
                  + tm->tm_min * 60 + tm->tm_sec);
}



static void print_boot_banner(void) {
    if (!shell_get_banner_enabled()) return;
    static const char *const banners[] = {
        BOOT_BANNER_BLOCK,
        BOOT_BANNER_GHOSTS,
        BOOT_BANNER_PEOPLE,
        BOOT_BANNER_DEVILS,
        BOOT_BANNER_OGRE,
        BOOT_BANNER_RECTANGLES,
        BOOT_BANNER_SLANT,
        BOOT_BANNER_SOFT,
    };
    const size_t n = sizeof(banners) / sizeof(banners[0]);
    unsigned idx = (unsigned)(esp_random() % n);
    printf("%s\n", banners[idx]);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#define COREDUMP_ROOT_DIR "/mnt/ghostesp"
#define COREDUMP_LOGS_DIR "/mnt/ghostesp/logs"
#define COREDUMP_SD_DIR "/mnt/ghostesp/logs/coredumps"
#define COREDUMP_SIG_PATH "/mnt/ghostesp/logs/coredumps/.last_saved_sig"

#ifndef COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE
/* Set to 0 to keep coredump data in flash after autosave. */
#define COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE 1
#endif

static uint32_t coredump_fnv1a_update(uint32_t hash, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool coredump_read_saved_sig(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }

    FILE *f = fopen(COREDUMP_SIG_PATH, "rb");
    if (!f) {
        return false;
    }

    size_t n = fread(out, 1, out_len - 1, f);
    fclose(f);
    if (n == 0) {
        out[0] = '\0';
        return false;
    }

    out[n] = '\0';
    char *nl = strchr(out, '\n');
    if (nl) {
        *nl = '\0';
    }
    return true;
}

static void coredump_write_saved_sig(const char *sig) {
    FILE *f = fopen(COREDUMP_SIG_PATH, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to write coredump signature marker");
        return;
    }
    fwrite(sig, 1, strlen(sig), f);
    fwrite("\n", 1, 1, f);
    fclose(f);
}

static bool coredump_detect_present_and_sig(const esp_partition_t *part, int *elf_offset_out, uint32_t *sig_out) {
    uint8_t head[256];
    size_t head_len = part->size < sizeof(head) ? (size_t)part->size : sizeof(head);
    if (esp_partition_read(part, 0, head, head_len) != ESP_OK) {
        return false;
    }

    int empty = 1;
    int elf_offset = -1;
    for (size_t i = 0; i < head_len; i++) {
        if (head[i] != 0xff) {
            empty = 0;
        }
        if (elf_offset < 0 && i + 4 <= head_len &&
            head[i] == 0x7f && head[i + 1] == 'E' && head[i + 2] == 'L' && head[i + 3] == 'F') {
            elf_offset = (int)i;
        }
    }
    if (empty != 0) {
        return false;
    }

    uint32_t hash = 2166136261u;
    hash = coredump_fnv1a_update(hash, (const uint8_t *)&part->size, sizeof(part->size));
    hash = coredump_fnv1a_update(hash, head, head_len);

    if (elf_offset_out) {
        *elf_offset_out = elf_offset;
    }
    if (sig_out) {
        *sig_out = hash;
    }
    return true;
}

static esp_err_t coredump_save_partition_bin(const esp_partition_t *part, const char *path, size_t *written_out) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }

    uint8_t buf[512];
    size_t offset = 0;
    while (offset < part->size) {
        size_t chunk = (part->size - offset) > sizeof(buf) ? sizeof(buf) : (part->size - offset);
        esp_err_t err = esp_partition_read(part, offset, buf, chunk);
        if (err != ESP_OK) {
            fclose(f);
            return err;
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            fclose(f);
            return ESP_FAIL;
        }
        offset += chunk;
    }

    fclose(f);
    if (written_out) {
        *written_out = offset;
    }
    return ESP_OK;
}

static esp_err_t coredump_erase_partition(const esp_partition_t *part) {
    size_t erase_size = part->erase_size;
    if (erase_size == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t to_erase = (part->size / erase_size) * erase_size;
    if (to_erase == 0) {
        to_erase = erase_size;
    }
    return esp_partition_erase_range(part, 0, to_erase);
}

static void coredump_write_summary(const char *summary_path, const char *bin_path,
                                   const esp_partition_t *part, int elf_offset,
                                   const char *panic_reason) {
    FILE *f = fopen(summary_path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to create coredump summary: %s", summary_path);
        return;
    }

    fprintf(f, "coredump_file=%s\n", bin_path);
    fprintf(f, "partition_label=%s\n", part->label);
    fprintf(f, "partition_size=%u\n", (unsigned)part->size);
    fprintf(f, "format=%s\n", (elf_offset >= 0) ? "elf" : "binary");
    if (elf_offset > 0) {
        fprintf(f, "elf_offset=%d\n", elf_offset);
    }
    fprintf(f, "panic_reason=%s\n", panic_reason && panic_reason[0] ? panic_reason : "run idf.py coredump-info for decoded panic reason");
    fprintf(f, "decode_hint=idf.py coredump-info -c <file>\n");
    fclose(f);
}

/* Decode the panic reason recorded in the flash coredump, if any. Returns
 * true and fills out[] with a short human-readable reason. */
static bool coredump_get_panic_reason_text(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }
    out[0] = '\0';
    esp_err_t err = esp_core_dump_get_panic_reason(out, out_len);
    if (err == ESP_OK && out[0] != '\0') {
        return true;
    }
#if defined(__riscv)
    static const char *const riscv_causes[] = {
        "Instruction address misaligned", "Instruction access fault", "Illegal instruction",
        "Breakpoint", "Load address misaligned", "Load access fault",
        "Store address misaligned", "Store access fault"
    };
    esp_core_dump_summary_t summary;
    if (esp_core_dump_get_summary(&summary) == ESP_OK) {
        uint32_t cause = summary.ex_info.mcause;
        if (cause < sizeof(riscv_causes) / sizeof(riscv_causes[0])) {
            snprintf(out, out_len, "%s", riscv_causes[cause]);
        } else {
            snprintf(out, out_len, "RISC-V exception %lu", (unsigned long)cause);
        }
        return true;
    }
#endif
    return false;
}

/* Returns true if a coredump was found in flash, meaning the previous boot
 * ended in a crash. The coredump is saved to the SD card when possible. */
static bool coredump_autosave_on_boot(char *panic_reason, size_t panic_reason_len) {
    if (panic_reason && panic_reason_len) {
        panic_reason[0] = '\0';
    }
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_COREDUMP,
        NULL);
    if (!part) {
        return false;
    }

    int elf_offset = -1;
    uint32_t sig = 0;
    if (!coredump_detect_present_and_sig(part, &elf_offset, &sig)) {
        return false;
    }

    char decoded_reason[256];
    if (!coredump_get_panic_reason_text(decoded_reason, sizeof(decoded_reason))) {
        decoded_reason[0] = '\0';
    }
    if (panic_reason && panic_reason_len) {
        snprintf(panic_reason, panic_reason_len, "%s", decoded_reason);
    }

    bool display_was_suspended = false;
    bool did_jit_mount = false;
    if (!sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) != ESP_OK) {
            ESP_LOGW(TAG, "Coredump present but SD unavailable for autosave");
            return true;
        }
        did_jit_mount = true;
    }

    if (!sd_card_exists(COREDUMP_ROOT_DIR) && sd_card_create_directory(COREDUMP_ROOT_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure root dir for coredump autosave");
        goto cleanup;
    }
    if (!sd_card_exists(COREDUMP_LOGS_DIR) && sd_card_create_directory(COREDUMP_LOGS_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure logs dir for coredump autosave");
        goto cleanup;
    }
    if (!sd_card_exists(COREDUMP_SD_DIR) && sd_card_create_directory(COREDUMP_SD_DIR) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to ensure coredump dir for autosave");
        goto cleanup;
    }

    char sig_id[40];
    snprintf(sig_id, sizeof(sig_id), "%08lx_%u", (unsigned long)sig, (unsigned)part->size);

    char previous_sig[40];
    if (coredump_read_saved_sig(previous_sig, sizeof(previous_sig)) && strcmp(previous_sig, sig_id) == 0) {
        ESP_LOGI(TAG, "Coredump already saved: %s", sig_id);
        goto cleanup;
    }

    char bin_path[192];
    char summary_path[192];
    snprintf(bin_path, sizeof(bin_path), COREDUMP_SD_DIR "/coredump_%s.bin", sig_id);
    snprintf(summary_path, sizeof(summary_path), COREDUMP_SD_DIR "/coredump_%s.summary.txt", sig_id);

    size_t bytes_written = 0;
    esp_err_t save_err = coredump_save_partition_bin(part, bin_path, &bytes_written);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "Coredump autosave failed: %s", esp_err_to_name(save_err));
        goto cleanup;
    }

    coredump_write_summary(summary_path, bin_path, part, elf_offset, decoded_reason);
    coredump_write_saved_sig(sig_id);
    ESP_LOGI(TAG, "Coredump autosaved (%u bytes): %s", (unsigned)bytes_written, bin_path);

#if COREDUMP_AUTOSAVE_ERASE_AFTER_SAVE
    esp_err_t erase_err = coredump_erase_partition(part);
    if (erase_err == ESP_OK) {
        ESP_LOGI(TAG, "Erased coredump partition after autosave");
    } else {
        ESP_LOGW(TAG, "Failed to erase coredump after autosave: %s", esp_err_to_name(erase_err));
    }
#endif

cleanup:
    if (did_jit_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
    return true;
}
#endif

#ifdef CONFIG_WITH_SCREEN
// Boot-time completion coordination. The SD/asset-pack step and the
// (fat-stack) plugin-discovery step run on separate tasks; the splash only
// fades out once both have finished. The mask is updated under a spinlock
// so the two tasks can race to be the last finisher.
static portMUX_TYPE s_boot_done_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_boot_done_mask = 0;
#define BOOT_DONE_SD_ASSET  (1u << 0)
#define BOOT_DONE_PLUGIN    (1u << 1)
#define BOOT_DONE_ALL       (BOOT_DONE_SD_ASSET | BOOT_DONE_PLUGIN)

static void splash_boot_step_done(uint32_t step) {
    uint32_t now;
    portENTER_CRITICAL(&s_boot_done_mux);
    s_boot_done_mask |= step;
    now = s_boot_done_mask;
    portEXIT_CRITICAL(&s_boot_done_mux);
    if (now == BOOT_DONE_ALL) {
        boot_status_set_progress(100.0f, "Ready");
        boot_status_signal_completion();
    }
}
#endif

#define BOOT_APP_SCAN_STACK_BYTES 32768

static void boot_app_discovery_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "Boot app discovery started");
#ifdef CONFIG_WITH_SCREEN
    plugin_manager_set_progress_cb(splash_plugin_progress_cb, NULL);
    boot_status_set_progress(-1.0f, "Checking apps...");
#endif
    if (plugin_manager_reload() < 0) {
        ESP_LOGW(TAG, "Boot plugin reload failed: %s", plugin_manager_last_error());
    }
    ESP_LOGI(TAG, "Boot app discovery finished (%d apps)", plugin_manager_count());
#ifdef CONFIG_WITH_SCREEN
    plugin_manager_set_progress_cb(NULL, NULL);
    splash_boot_step_done(BOOT_DONE_PLUGIN);
#endif
    vTaskDeleteWithCaps(NULL);
}

static bool start_boot_app_discovery_task(void) {
    BaseType_t ok = xTaskCreateWithCaps(boot_app_discovery_task, "BootApps",
                                        BOOT_APP_SCAN_STACK_BYTES, NULL, 5,
                                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "Boot app discovery task create failed; skipping");
        return false;
    }
    return true;
}

#if GHOSTESP_OTA_SUPPORTED
// Both checks are low-priority, check-only passes -- they must never
// download or flash. Shared by one task since neither blocks for long:
// each background_check() call is a no-op unless its own connectivity
// gate (GhostLink session / board has Wi-Fi) is already satisfied.
static void ota_background_check_task(void *arg) {
    (void)arg;
    // GhostLink connects independently of Wi-Fi, so the peer check can go
    // first; give the boot sequence a moment to settle either way.
    vTaskDelay(pdMS_TO_TICKS(10000));
    peer_ota_manager_background_check();

    // Extra time for Wi-Fi to connect (if configured) and DNS to be ready --
    // Cardputer ADV and similar boards need it.
    vTaskDelay(pdMS_TO_TICKS(10000));
    ota_manager_background_check();
    vTaskDelete(NULL);
}
#endif

static void deferred_sd_init_task(void *arg) {
    // Short initial delay: the splash holds the screen during boot work, so we
    // only need enough time for splash_create to render the progress bar.
    vTaskDelay(pdMS_TO_TICKS(200));
#if defined(CONFIG_HAS_LORA) && (defined(CONFIG_CROWPANEL_ADVANCE_RGB_LCD) || \
                                defined(CONFIG_CROWPANEL_ADVANCE_SMALL_SPI_LCD))
    // CrowPanel Advance wireless-module mode owns a SPI host/function-mux
    // path. The 4.3 board shares GPIO4/5/6 with TF; the 2.4/2.8 boards keep
    // the display on SPI2 and reserve SPI3 for the radio. In either case SD
    // probing would claim or reconfigure the radio path, so leave SD off.
    ESP_LOGI(TAG, "SD init skipped: CrowPanel wireless-module mode owns shared pins");
#ifdef CONFIG_WITH_SCREEN
    boot_status_set_progress(100.0f, "LoRa wireless module mode");
    boot_status_signal_completion();
#endif
    vTaskDelete(NULL);
    return;
#endif
    ESP_LOGI(TAG, "Deferred SD Card init starting");

#ifdef CONFIG_WITH_SCREEN
    boot_status_set_progress(-1.0f, "Mounting SD card...");
#endif
    esp_err_t sd_init_ret = ESP_OK;
    MEASURE_INIT_RAM("SD Card init", sd_init_ret = sd_card_init());
    if (sd_init_ret != ESP_OK) {
        ESP_LOGW(TAG, "Deferred SD Card init failed, skipping coredump autosave");
#ifdef CONFIG_WITH_SCREEN
        boot_status_set_progress(100.0f, "SD unavailable");
        boot_status_signal_completion();
#endif
        vTaskDelete(NULL);
        return;
    }

    // Load persisted Ghostchi XP before the first status bar is shown after
    // splash, otherwise the badge briefly starts from Lv1 until Ghostchi opens.
    (void)ghostchi_manager_probe_storage();

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#ifdef CONFIG_WITH_SCREEN
    boot_status_set_progress(-1.0f, "Saving core dump...");
#endif
    char panic_reason[256];
    bool had_crash = coredump_autosave_on_boot(panic_reason, sizeof(panic_reason));
    if (had_crash) {
        crash_reporter_set_boot_crash(panic_reason);
    }
#endif

#ifdef CONFIG_WITH_SCREEN
    splash_require_completion();
    asset_pack_set_progress_cb(splash_asset_pack_progress_cb, NULL);
    boot_status_set_progress(0.0f, "Loading asset pack...");
    esp_err_t asset_err = asset_pack_load_active();
    asset_pack_set_progress_cb(NULL, NULL);
    if (asset_err != ESP_OK && asset_err != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "Active asset pack load failed: %s", esp_err_to_name(asset_err));
    }
    if (asset_err == ESP_OK) {
        display_manager_run_on_lvgl(apply_main_menu_background_cb, NULL);
    }

    // Hand off app discovery to its own PSRAM-backed static task so the GAPP
    // inflate can use the same fat stack the Apps menu allocates. The SD Init
    // task stays at 6K; the splash holds until both boot steps are signalled.
#if CONFIG_ENABLE_NATIVE_SD_APPS && !defined(CONFIG_IS_ATOMS3R)
    if (!start_boot_app_discovery_task()) {
        splash_boot_step_done(BOOT_DONE_PLUGIN);
    }
#else
    splash_boot_step_done(BOOT_DONE_PLUGIN);
#endif
    splash_boot_step_done(BOOT_DONE_SD_ASSET);
#endif
    vTaskDelete(NULL);
}

void app_main(void) {
    memory_debug_init();
    memory_debug_start_boot_trace();
    MEASURE_INIT_RAM("Ghostchi Mood init", ghostchi_mood_init());
    ghostchi_mood_record_event(GHOSTCHI_MOOD_EVENT_BOOT, 3);

#ifdef CONFIG_CROWPANEL_EPAPER_42
    /* Factory firmware enables both board rails before touching the panel or
     * SD socket. GPIO7 is the display rail and GPIO42 is the SD rail; GPIO7
     * must not later be reused as the generic comm-manager RX pin. */
    gpio_config_t crowpanel_power_config = {
        .pin_bit_mask = (1ULL << 7) | (1ULL << 42),
        /* Keep input enabled so board-driver diagnostics can verify the
         * physical rail-enable levels, not just the output latch. */
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&crowpanel_power_config);
    gpio_set_level(7, 1);
    gpio_set_level(42, 1);
    ESP_LOGI(TAG, "CrowPanel power rails enabled: display GPIO7, SD GPIO42");
#endif

#if defined(CONFIG_USING_SPI) && defined(CONFIG_SD_SPI_CS_PIN) && \
    !defined(CONFIG_CROWPANEL_EPAPER_42)
    /* Keep the card deselected before any shared-bus display/touch traffic. */
    gpio_reset_pin(CONFIG_SD_SPI_CS_PIN);
    gpio_set_direction(CONFIG_SD_SPI_CS_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(CONFIG_SD_SPI_CS_PIN, 1);
    ESP_LOGI(TAG, "SD Card CS pin %d set HIGH", CONFIG_SD_SPI_CS_PIN);
#endif

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    /* CoreS3-SE: the HY2.0 grove ports get their 5V from a boost converter
     * gated by the AW9523 expander, which powers up disabled. Without this,
     * units plugged into PORT.A/B/C (ENV-III, GPS, NFC) are never powered and
     * never ACK on I2C. */
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "m5cores3se") == 0) {
        esp_err_t bus5v_err = aw9523_set_port_5v(true);
        if (bus5v_err != ESP_OK) {
            ESP_LOGW(TAG, "Grove port 5V enable failed: %s",
                     esp_err_to_name(bus5v_err));
        }
    }
#endif

    // Reduce NimBLE log verbosity (keep warnings/errors only)
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // Pull SPI CS pins HIGH to prevent bus conflicts for the TEmbed C1101
#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        ESP_LOGI(TAG, "Initializing SPI CS pins for TEmbed C1101");

        gpio_reset_pin(CONFIG_LV_DISP_SPI_CS);
        gpio_set_direction(CONFIG_LV_DISP_SPI_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_LV_DISP_SPI_CS, 1);
        ESP_LOGI(TAG, "TFT CS pin %d set HIGH", CONFIG_LV_DISP_SPI_CS);

        // CC1101 SS pin
        gpio_reset_pin(12);
        gpio_set_direction(12, GPIO_MODE_OUTPUT);
        gpio_set_level(12, 1);
        ESP_LOGI(TAG, "CC1101 SS pin 12 set HIGH");

        // SD Card CS pin
        gpio_reset_pin(CONFIG_SD_SPI_CS_PIN);
        gpio_set_direction(CONFIG_SD_SPI_CS_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(CONFIG_SD_SPI_CS_PIN, 1);
        ESP_LOGI(TAG, "SD Card CS pin %d set HIGH", CONFIG_SD_SPI_CS_PIN);

        gpio_reset_pin(44);
        gpio_set_direction(44, GPIO_MODE_OUTPUT);
        gpio_set_level(44, 1);
        gpio_reset_pin(43);
        gpio_set_direction(43, GPIO_MODE_OUTPUT);
        gpio_set_level(43, 0);
        ESP_LOGI(TAG, "Shared NRF/W5500 isolated: CS44 HIGH, CE43 LOW");
    }
#endif


    MEASURE_INIT_RAM("Serial Manager", serial_manager_init());
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    /* Start ESP-Hosted from app_main instead of a C constructor.  IDF 6.1
     * performs P4 sleep-retention startup after C constructors; initializing
     * Hosted from the constructor races that initialization and can trip the
     * sleep-retention module assertion before app_main starts. */
    ESP_LOGI(TAG, "Starting ESP-Hosted C6 transport...");
    esp_err_t hosted_init_err = esp_hosted_init();
    bool hosted_ready = hosted_init_err == ESP_OK;
    if (!hosted_ready) {
        ESP_LOGE(TAG, "ESP-Hosted initialization failed: %s", esp_err_to_name(hosted_init_err));
    }

    /* The C6 handshake must be completed before esp_wifi_remote is used. */
    esp_err_t hosted_err = hosted_ready ? esp_hosted_connect_to_slave() : hosted_init_err;
    hosted_ready = hosted_err == ESP_OK;
    if (!hosted_ready) {
        ESP_LOGE(TAG, "ESP-Hosted C6 connection failed: %s", esp_err_to_name(hosted_err));
    } else {
        /* ESP32-P4 has no local Bluetooth controller. The onboard C6
         * supplies it through ESP-Hosted's NimBLE VHCI bridge. */
        esp_err_t hosted_bt_err = esp_hosted_bt_controller_init();
        if (hosted_bt_err == ESP_OK) {
            hosted_bt_err = esp_hosted_bt_controller_enable();
        }
        if (hosted_bt_err != ESP_OK) {
            ESP_LOGW(TAG, "ESP-Hosted C6 Bluetooth unavailable: %s",
                     esp_err_to_name(hosted_bt_err));
        } else {
            ESP_LOGI(TAG, "ESP-Hosted C6 Bluetooth controller enabled");
        }
        p4_slave_ota_result_t ota_result = p4_slave_ota_update(false);
        if (ota_result == P4_SLAVE_OTA_UPDATED) {
            ESP_LOGI(TAG, "C6 update complete; rebooting P4");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
        esp_err_t raw_init = ghost_raw_radio_init();
        if (raw_init != ESP_OK) {
            ESP_LOGW(TAG, "Ghost raw radio init deferred: %s", esp_err_to_name(raw_init));
        }
#endif
    }
#else
    bool hosted_ready = true;
#endif
    if (hosted_ready) {
        MEASURE_INIT_RAM("Wifi Manager", wifi_manager_init());
    } else {
        ESP_LOGW(TAG, "Skipping Wi-Fi initialization because ESP-Hosted is offline");
    }
#ifdef CONFIG_WITH_ETHERNET
    {
        esp_err_t eth_ret;
        MEASURE_INIT_RAM("Ethernet Manager", eth_ret = ethernet_manager_init());
        if (eth_ret != ESP_OK) {
            ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(eth_ret));
        }
    }
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32S2)
    // MEASURE_INIT_RAM("BLE Manager", ble_init());
#endif
#ifdef CONFIG_HAS_LORA
    MEASURE_INIT_RAM("LoRa Manager", lora_manager_early_init_off_main());
#endif
#ifdef CONFIG_HAS_BADUSB
    MEASURE_INIT_RAM("BadUSB Manager", badusb_manager_init());
#endif

#ifdef CONFIG_USE_TDECK
    ESP_LOGI(TAG, "TDECK: Delay for c3 boot");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "TDECK: END DELAY for c3 boot");

    // SET all SPI CS pins high to get the devices to shut it
    gpio_set_direction(39, GPIO_MODE_OUTPUT);
    gpio_set_level(39, 1);
    gpio_set_direction(12, GPIO_MODE_OUTPUT);
    gpio_set_level(12, 1);
    gpio_set_direction(9, GPIO_MODE_OUTPUT);
    gpio_set_level(9, 1);

    gpio_set_direction(10, GPIO_MODE_OUTPUT);
    gpio_set_level(10, 1); // set tdeck POWER_ON pin high to enable peripherals
#endif

#ifdef USB_MODULE
    wifi_manager_auto_deauth();
    return;
#endif

#if defined(CONFIG_USE_ENCODER) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo TEmbedC1101") == 0) {
        gpio_reset_pin(15);
        gpio_set_direction(15, GPIO_MODE_OUTPUT);
        
        // Check if we woke up from deep sleep
        uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
        esp_sleep_wakeup_cause_t wakeup_reason = ESP_SLEEP_WAKEUP_UNDEFINED;
        for (unsigned i = 0; i < 32; i++) {
            if (wakeup_causes & (1U << i)) {
                wakeup_reason = (esp_sleep_wakeup_cause_t)i;
                break;
            }
        }

        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_UNDEFINED:
                ESP_LOGI("Main", "Normal startup (not from deep sleep), IO15 set high");
                break;
            case ESP_SLEEP_WAKEUP_EXT0:
                ESP_LOGI("DeepSleep", "Woke up from deep sleep via EXT0 (IO6), pulling IO15 high");
                gpio_set_level(15, 1);
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI("DeepSleep", "Woke up from deep sleep via EXT1 (IO6), pulling IO15 high");
                gpio_set_level(15, 1);
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI("Main", "Woke up from deep sleep via timer, IO15 set high");
                break;
            case ESP_SLEEP_WAKEUP_TOUCHPAD:
                ESP_LOGI("Main", "Woke up from deep sleep via touchpad, IO15 set high");
                break;
            case ESP_SLEEP_WAKEUP_ULP:
                ESP_LOGI("Main", "Woke up from deep sleep via ULP, IO15 set high");
                break;
            default:
                ESP_LOGI("Main", "Woke up from deep sleep via unknown cause (%d), IO15 set high", wakeup_reason);
                break;
        }
        
        // Always set IO15 high on startup
        gpio_set_level(15, 1);
    }
#endif

    ESP_LOGI(TAG, "Initializing Commands");
    MEASURE_INIT_RAM("Commands init", command_init());

    ESP_LOGI(TAG, "Registering Commands");
    MEASURE_INIT_RAM("Commands registration", register_commands());

    ESP_LOGI(TAG, "Initializing Settings");
    MEASURE_INIT_RAM("Settings init", settings_init(&G_Settings));
#ifdef CONFIG_HAS_BADBLE
    MEASURE_INIT_RAM("BadBLE Manager", badble_manager_init());
#endif
    esp_log_level_set("*", (esp_log_level_t)settings_get_log_level(&G_Settings));

#if GHOSTESP_OTA_SUPPORTED
    MEASURE_INIT_RAM("OTA manager init", ota_manager_init());
#ifndef CONFIG_WITH_SCREEN
    // Screen-less boards (e.g. somethingsomething2 / Banshee S3) never reach
    // the display-init confirm-boot-ok hook further down (it's compiled out
    // entirely for them) -- settings having loaded successfully is as good a
    // "did boot succeed" checkpoint as this board gets.
    ota_manager_confirm_boot_ok();
#endif
#endif

#if GHOSTESP_OTA_SUPPORTED
    // Gated the same as the ota_manager_init() call above -- peer_ota_manager.c
    // and self_ota_manager.c are only ever meaningfully used on 8MB/16MB boards
    // (somethingsomething/somethingsomething2 today), but without this guard
    // these calls would reference their symbols unconditionally on every board,
    // pulling both files' static buffers into every 4MB build's BSS for nothing.
    if (peer_ota_manager_is_supported()) {
        peer_ota_manager_init();
    }
    if (self_ota_manager_is_supported()) {
        MEASURE_INIT_RAM("Self-OTA manager init", self_ota_manager_init());
    }
#endif

    // Apply timezone from settings
    const char *tz = settings_get_timezone_str(&G_Settings);
    if (tz && strlen(tz) > 0) {
        setenv("TZ", tz, 1);
        tzset();
        ESP_LOGI(TAG, "Timezone applied: %s", tz);
    }

    // Apply WiFi country from settings
    uint8_t country_index = settings_get_wifi_country(&G_Settings);
    const char *country_codes[] = {"US", "GB", "JP", "AU", "CN", "01"};
    if (country_index < sizeof(country_codes) / sizeof(country_codes[0])) {
        esp_err_t err = esp_wifi_set_country_code(country_codes[country_index], true);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set WiFi country: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "WiFi country applied: %s", country_codes[country_index]);
        }
    }

    ESP_LOGI(TAG, "Configuring WiFi STA from settings");
    MEASURE_INIT_RAM("WiFi STA Config", wifi_manager_configure_sta_from_settings());

    ESP_LOGI(TAG, "Initializing Comm Manager");
    {
        int32_t comm_tx = G_Settings.esp_comm_tx_pin;
        int32_t comm_rx = G_Settings.esp_comm_rx_pin;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0 &&
            comm_tx == 6 && comm_rx == 7) {
            comm_tx = 11;
            comm_rx = 12;
        } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Pancake") == 0 ||
                   strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "MarauderV8") == 0) {
            comm_tx = UART_PIN_NO_CHANGE;
            comm_rx = UART_PIN_NO_CHANGE;
        }
#endif
#ifdef CONFIG_CROWPANEL_EPAPER_42
        /* GPIO7 is the factory display-power enable, not a UART input. This
         * board has no wired peer-comm header, so leave the optional manager
         * off rather than stealing the display rail. */
        comm_tx = UART_PIN_NO_CHANGE;
        comm_rx = UART_PIN_NO_CHANGE;
#endif
        if (comm_tx != UART_PIN_NO_CHANGE || comm_rx != UART_PIN_NO_CHANGE) {
            MEASURE_INIT_RAM("Comm Manager", esp_comm_manager_init((gpio_num_t)comm_tx, (gpio_num_t)comm_rx, DEFAULT_BAUD_RATE));
        } else {
            ESP_LOGI(TAG, "Comm Manager disabled for this build");
        }
    }
#if !defined(CONFIG_IDF_TARGET_ESP32S2)
    MEASURE_INIT_RAM("BLE Bridge restore", ble_bridge_apply_saved_enabled());
#endif
    wardriving_register_stream_handler();
    usb_keyboard_manager_register_stream_handler();
    ghostlink_bench_init();
#ifdef CONFIG_HAS_BADUSB
    badusb_manager_register_stream_handler();
#endif
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        peer_storage_manager_init_peer();
    }
#endif
#ifdef CONFIG_WITH_ETHERNET
    MEASURE_INIT_RAM("Ethernet Comm Handler init", eth_comm_handler_init());
#endif
#ifdef CONFIG_HAS_MIC
#if defined(CONFIG_HAS_ES7210_MIC) || defined(CONFIG_HAS_AW88298_SPEAKER)
    // M5 audio codec is only present on boards with the ES7210 mic and/or
    // AW88298 speaker (e.g. M5Stack CoreS3-SE). Keep in sync with the
    // include guard at the top of this file.
    MEASURE_INIT_RAM("M5 audio codec init", m5_audio_codec_init());
#endif
#if !defined(CONFIG_WITH_SCREEN)
    // Headless boards (e.g. somethingsomething2) don't run mic capture at boot;
    // the visualizer self-initializes on first mic_visualizer_start call.
#else
    // Initialize MIC visualizer (will start sending amplitude over GhostLink when connected)
    MEASURE_INIT_RAM("Mic Visualizer init", mic_visualizer_init());
    mic_visualizer_start();
#endif
#endif
#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER) || defined(CONFIG_HAS_CROWPANEL_NS4168)
#if !defined(CONFIG_WITH_SCREEN)
    // Headless boards only need the receiver when `audio start` runs; it
    // self-initializes there. Skip the boot-time I2S + decode task + 96KB
    // ringbuf registration to preserve internal RAM.
#else
    ESP_LOGI(TAG, "Initializing audio receiver");
    MEASURE_INIT_RAM("Audio Receiver", audio_receiver_manager_init());
#endif
#endif
#ifdef CONFIG_HAS_CAMERA
    MEASURE_INIT_RAM("Motion Detector init", motion_detector_init());
    MEASURE_INIT_RAM("Camera Stream init", camera_stream_init());
#endif
#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE))
    nrf24_analyzer_register_stream_handler();
#endif
#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE))
    subghz_view_register_stream_handler();
#elif defined(CONFIG_HAS_SUBGHZ)
    subghz_remote_manager_register_stream_handler();
#endif

    ESP_LOGI(TAG, "Initializing AP Manager");
    MEASURE_INIT_RAM("AP Manager", ap_manager_init());


#ifdef CONFIG_WITH_SCREEN

#ifdef CONFIG_USE_ATOMS3R_BUTTON
    gpio_config_t atoms3r_button_config = {
        .pin_bit_mask = 1ULL << CONFIG_ATOMS3R_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&atoms3r_button_config);
    joystick_init(&joysticks[0], CONFIG_ATOMS3R_BUTTON_PIN, 2500, true);
#endif

#ifdef CONFIG_USE_JOYSTICK
#ifdef CONFIG_USE_IO_EXPANDER
    esp_err_t io_ret;
    MEASURE_INIT_RAM("Joystick IO Expander init", io_ret = joystick_io_expander_init());
    if (io_ret == ESP_OK) {
        printf("Joystick: IO expander\n");
        // Map to display manager expectations: [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
        joystick_init(&joysticks[0], 3, HOLD_LIMIT, true);  // Left button (P03) -> joysticks[0]
        joystick_init(&joysticks[1], 2, HOLD_LIMIT, true);  // Select button (P02) -> joysticks[1]
        joystick_init(&joysticks[2], 0, HOLD_LIMIT, true);  // Up button (P00) -> joysticks[2]
        joystick_init(&joysticks[3], 4, HOLD_LIMIT, true);  // Right button (P04) -> joysticks[3]
        joystick_init(&joysticks[4], 1, HOLD_LIMIT, true);  // Down button (P01) -> joysticks[4]
    } else {
        printf("IO Expander initialization failed, falling back to GPIO mode\n");
        // Fallback to GPIO mode - map to display manager expectations: [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
        joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);  // Left
        joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);  // Select
        joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);  // Up
        joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);  // Right
        joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);  // Down
    }
#else
    // Standard GPIO joystick mode - map to display manager expectations.
#ifdef CONFIG_CROWPANEL_EPAPER_42
    // The factory firmware labels these five GPIOs HOME, EXIT, PRV, NEXT, OK.
    // Keep the five slots available to the existing input stack; the e-paper
    // adapter translates HOME/EXIT and the prev/next actions at dispatch time.
    joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);  // PRV
    joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);  // OK
    joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);  // HOME
    joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);  // NEXT
    joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);  // EXIT
#else
    // [0]=Left, [1]=Select, [2]=Up, [3]=Right, [4]=Down
    joystick_init(&joysticks[0], CONFIG_L_BTN, HOLD_LIMIT, true);  // Left
    joystick_init(&joysticks[1], CONFIG_C_BTN, HOLD_LIMIT, true);  // Select
    joystick_init(&joysticks[2], CONFIG_U_BTN, HOLD_LIMIT, true);  // Up
    joystick_init(&joysticks[3], CONFIG_R_BTN, HOLD_LIMIT, true);  // Right
    joystick_init(&joysticks[4], CONFIG_D_BTN, HOLD_LIMIT, true);  // Down
#endif
    printf("Joystick: GPIO buttons\n");
#if defined(CONFIG_JOYSTICK_COM_PIN) && CONFIG_JOYSTICK_COM_PIN >= 0
    {
        gpio_config_t com_conf = {
            .pin_bit_mask = (1ULL << CONFIG_JOYSTICK_COM_PIN),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE};
        gpio_config(&com_conf);
        gpio_set_level(CONFIG_JOYSTICK_COM_PIN, 0);
        printf("Joystick COM pin %d driven LOW\n", CONFIG_JOYSTICK_COM_PIN);
    }
#endif
#endif
#endif
    ESP_LOGI(TAG, "Initializing display manager");
    MEASURE_INIT_RAM("Display Manager", display_manager_init() );
    ESP_LOGI(TAG, "Presenting startup screen");
    bool startup_ready = false;
    View *startup_view = &splash_view;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-S3") == 0 ||
        strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0) {
        startup_view = &tdongle_status_view;
    }
#endif
    MEASURE_INIT_RAM("Switch to startup view", startup_ready = display_manager_switch_view_and_wait_for_refresh(startup_view));
    if (startup_ready) {
        set_backlight_brightness(100);
    } else {
        ESP_LOGW(TAG, "Startup view first refresh did not complete; leaving backlight off");
    }
    MEASURE_INIT_RAM("Deferred display peripherals", display_manager_init_deferred_peripherals());
#if GHOSTESP_OTA_SUPPORTED
    // Boot got this far without crashing -- confirm the image and cancel any
    // pending bootloader rollback (no-op if this wasn't a post-OTA boot).
    ota_manager_confirm_boot_ok();
    maybe_schedule_self_ota_boot_error_popup();
#endif
    // If the previous boot crashed, show a short popup once the boot
    // screens are gone (Flipper Zero style crash notice).
    crash_reporter_init();
#ifdef CONFIG_HAS_DRV2605_HAPTICS
    esp_err_t haptic_err;
    MEASURE_INIT_RAM("Haptic Manager", haptic_err = haptic_manager_init());
    if (haptic_err == ESP_OK) {
        haptic_manager_play(HAPTIC_EFFECT_SUCCESS);
    } else {
        ESP_LOGW(TAG, "Haptic manager failed to initialize: %s", esp_err_to_name(haptic_err));
    }
#endif
    if (settings_get_rgb_mode(&G_Settings) == RGB_MODE_RAINBOW) {
        display_manager_set_rainbow_mode(true);
    }
#endif
#ifdef CONFIG_WITH_STATUS_DISPLAY
    MEASURE_INIT_RAM("Status display init", status_display_init());
    if (!status_display_is_ready()) {
        ESP_LOGW(TAG, "Status display failed to initialize");
    }
#endif

    // Deferred SD card init: run in a background task so the shared-SPI
    // suspend/resume does not freeze the splash. The splash persists with a
    // progress bar until splash_signal_completion() fires (or a hard timeout
    // in splash_screen.c kicks in). The fat stack needed by GAPP inflate is
    // handled by the separate boot_app_discovery_task spawned from inside
    // deferred_sd_init_task.
    {
        // 8K: sd_card_init's SDMMC/SPI locals + coredump autosave + asset pack
        // load are all sequential in this task; 6K overflowed on Cardputer ADV.
        BaseType_t sd_task_rc = xTaskCreate(deferred_sd_init_task, "SD Init", 8192, NULL,
                                            tskIDLE_PRIORITY + 1, NULL);
        if (sd_task_rc != pdPASS) {
            ESP_LOGE(TAG, "Failed to create SD Init task");
        }
    }

#if GHOSTESP_OTA_SUPPORTED
    {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
        // Networked boards are checked by wifi_manager after GOT_IP. Keep this
        // worker only for the Wi-Fi-less Banshee S3 GhostLink peer.
        if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
            BaseType_t ota_task_rc = xTaskCreate(ota_background_check_task, "OTA Check", 6144,
                                                 NULL, tskIDLE_PRIORITY + 1, NULL);
            if (ota_task_rc != pdPASS) {
                ESP_LOGE(TAG, "Failed to create OTA background check task");
            }
        }
#endif
    }
#endif

    // Initialize RGB Manager based on persisted settings or compile-time defaults
    {
        bool initialized = false;
        int32_t data_pin = settings_get_rgb_data_pin(&G_Settings);
        int rgb_led_count = settings_get_rgb_led_count(&G_Settings);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
        /* CrowPanel P4 has no user RGB LED on the generic LEDC pins.  The
         * generic fallback defaults can otherwise resolve to GPIO0 and claim
         * LEDC channel 0, which is also the MIPI panel backlight channel. */
        data_pin = GPIO_NUM_NC;
        rgb_led_count = 0;
#endif
        if (rgb_led_count <= 0) {
#ifndef CONFIG_CROWPANEL_ADVANCED_P4
            if (rgb_manager.num_leds > 0) {
                rgb_led_count = rgb_manager.num_leds;
            } else if (CONFIG_NUM_LEDS > 0) {
                rgb_led_count = CONFIG_NUM_LEDS;
            } else {
                rgb_led_count = 1;
            }
#endif
        }
        int32_t red_pin, green_pin, blue_pin;
        settings_get_rgb_separate_pins(&G_Settings, &red_pin, &green_pin, &blue_pin);
        if (data_pin != GPIO_NUM_NC) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (data pin) init", rgb_err = rgb_manager_init(&rgb_manager, data_pin, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC));
            initialized = (rgb_err == ESP_OK);
        } else if (red_pin != GPIO_NUM_NC && green_pin != GPIO_NUM_NC && blue_pin != GPIO_NUM_NC) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (separate pins) init", rgb_err = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, red_pin, green_pin, blue_pin));
            initialized = (rgb_err == ESP_OK);
        }
            if (!initialized && rgb_led_count > 0) {
#ifdef CONFIG_LED_DATA_PIN
            if (CONFIG_LED_DATA_PIN >= 0) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (fallback) init", rgb_err = rgb_manager_init(&rgb_manager, CONFIG_LED_DATA_PIN, rgb_led_count, LED_ORDER,
                                                 LED_MODEL_WS2812, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC));
            initialized = (rgb_err == ESP_OK);
            }
#elif defined(CONFIG_RED_RGB_PIN) && defined(CONFIG_GREEN_RGB_PIN) && defined(CONFIG_BLUE_RGB_PIN)
            if (CONFIG_RED_RGB_PIN >= 0 && CONFIG_GREEN_RGB_PIN >= 0 && CONFIG_BLUE_RGB_PIN >= 0) {
            esp_err_t rgb_err;
            MEASURE_INIT_RAM("RGB Manager (fallback separate pins) init", rgb_err = rgb_manager_init(&rgb_manager, GPIO_NUM_NC, rgb_led_count, LED_PIXEL_FORMAT_GRB,
                                                 LED_MODEL_WS2812, CONFIG_RED_RGB_PIN, CONFIG_GREEN_RGB_PIN, CONFIG_BLUE_RGB_PIN));
            initialized = (rgb_err == ESP_OK);
            }
#endif
        }
        RGBMode boot_mode = settings_get_rgb_mode(&G_Settings);
        if (initialized && boot_mode == RGB_MODE_RAINBOW) {
            xTaskCreatePinnedToCore(rainbow_task, "Rainbow Task", 3072,
                                    &rgb_manager, RGB_EFFECT_TASK_PRIORITY,
                                    &rgb_effect_task_handle,
                                    RGB_EFFECT_TASK_CORE);
        } else if (initialized && boot_mode == RGB_MODE_KNIGHT_RIDER) {
            xTaskCreate(knightrider_task, "Knight Rider Task", 3072, &rgb_manager,
                        RGB_EFFECT_TASK_PRIORITY, &rgb_effect_task_handle);
        } else if (initialized && boot_mode >= RGB_MODE_RED && boot_mode <= RGB_MODE_PINK) {
            // Restore saved static color at boot
            rgb_manager_apply_static_from_settings();
        }
        
#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
        // Register MIC amplitude stream handler for RGB visualizer
        if (initialized) {
            rgb_manager_register_mic_stream_handler();
        }
#endif
    }

    printf("\n");
    ESP_LOGI(TAG, "Build config used: %s", CONFIG_BUILD_CONFIG_TEMPLATE);
    printf("Build Name: %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);
    
    ESP_LOGI(TAG, "Git branch: %s, commit: %s", GIT_BRANCH, GIT_COMMIT_HASH);
    printf("Git branch: %s, commit: %s\n", GIT_BRANCH, GIT_COMMIT_HASH);

    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_8BIT);
    float percent_free = (total_heap > 0) ? (100.0f * free_heap / total_heap) : 0.0f;
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    float percent_internal_free = (total_internal > 0) ? (100.0f * free_internal / total_internal) : 0.0f;
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    float percent_psram_free = (total_psram > 0) ? (100.0f * free_psram / total_psram) : 0.0f;

    ESP_LOGI(TAG, "Free heap after init: %d / %d bytes (%.1f%% free)", (int)free_heap, (int)total_heap, percent_free);
    ESP_LOGI(TAG, "Free INTERNAL RAM after init: %d / %d bytes (%.1f%% free)", (int)free_internal, (int)total_internal, percent_internal_free);
    if (total_psram > 0) {
        ESP_LOGI(TAG, "Free PSRAM after init: %d / %d bytes (%.1f%% free)", (int)free_psram, (int)total_psram, percent_psram_free);
    } else {
        ESP_LOGI(TAG, "PSRAM not present on this build");
    }
    printf("Free heap after init: %d / %d bytes (%.1f%% free)\n", (int)free_heap, (int)total_heap, percent_free);
    printf("Free INTERNAL RAM after init: %d / %d bytes (%.1f%% free)\n", (int)free_internal, (int)total_internal, percent_internal_free);
    if (total_psram > 0) {
        printf("Free PSRAM after init: %d / %d bytes (%.1f%% free)\n", (int)free_psram, (int)total_psram, percent_psram_free);
    }

#ifdef CONFIG_HAS_RTC_CLOCK
    // Sync system time from RTC on boot. The RTC stores UTC, so the struct tm
    // must be interpreted as UTC. mktime() would honor the TZ env var and treat
    // the fields as local time, shifting the restored clock by the timezone
    // offset; timegm() interprets the fields as UTC instead.
    RTC_Date rtc_time;
    if (rtc_get_datetime(&rtc_time) == ESP_OK) {
        struct timeval tv = {0};
        struct tm tm = {0};
        
        tm.tm_year = rtc_time.year - 1900;
        tm.tm_mon = rtc_time.month - 1;
        tm.tm_mday = rtc_time.day;
        tm.tm_hour = rtc_time.hour;
        tm.tm_min = rtc_time.minute;
        tm.tm_sec = rtc_time.second;
        tm.tm_isdst = 0;
        
        tv.tv_sec = timegm(&tm);
        tv.tv_usec = 0;
        
        if (tv.tv_sec > 1600000000) { // Valid time (after Sept 2020)
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "System time synchronized from RTC: %04d-%02d-%02d %02d:%02d:%02d", 
                     rtc_time.year, rtc_time.month, rtc_time.day, 
                     rtc_time.hour, rtc_time.minute, rtc_time.second);
            printf("System time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d\n", 
                   rtc_time.year, rtc_time.month, rtc_time.day, 
                   rtc_time.hour, rtc_time.minute, rtc_time.second);
        } else {
            ESP_LOGW(TAG, "RTC time invalid, keeping default time");
        }
    } else {
        ESP_LOGW(TAG, "Failed to read time from RTC");
    }
#endif

    ESP_LOGI(TAG, "Ghost ESP INIT complete.");
    memory_debug_log_snapshot("app_main complete");
    esp_err_t mem_monitor_err = memory_debug_start_periodic_monitor();
    if (mem_monitor_err != ESP_OK) {
        ESP_LOGW(TAG, "Periodic RAM monitor failed to start: %s", esp_err_to_name(mem_monitor_err));
    }
    printf("\n");
    print_boot_banner();
    printf("\n");
    printf("Type 'help' for available commands\n");
}
