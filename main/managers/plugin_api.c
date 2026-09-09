#include "managers/plugin_api.h"
#include "managers/plugin_api_internal.h"
#include "managers/ghostscript_runtime.h"

#include "core/serial_manager.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"
#include "gui/screen_layout.h"
#include "managers/badusb_manager.h"
#include "managers/ble_manager.h"
#include "managers/display_manager.h"
#include "managers/espnow_manager.h"
#include "managers/infrared_manager.h"
#include "managers/ghostchi_manager.h"
#include "managers/plugin_manager.h"
#include "managers/rgb_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/plugin_runner_view.h"
#include "managers/wifi_manager.h"
#include "managers/ap_manager.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/device_detect_scan.h"
#include "scans/ble/gatt_scan.h"
#include "scans/wifi/ap_scan.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#if CONFIG_ENABLE_NATIVE_SD_APPS
#include "esp_elf.h"
#endif

static const char *TAG = "PluginAPI";

extern bool plugin_api_nfc_t2_scan_start(void);
extern bool plugin_api_nfc_t2_scan_stop(void);
extern bool plugin_api_nfc_t2_scan_active(void);
extern bool plugin_api_nfc_t2_read(ghostesp_nfc_t2_info_t *out_info, uint8_t *ndef_out,
                                   size_t max_ndef_bytes, size_t *ndef_bytes_out);
extern bool plugin_api_nfc_t2_write_ndef(const uint8_t *ndef, size_t ndef_len);

static void (*s_ui_set_title)(const char *title) = NULL;
static void (*s_ui_print)(const char *text) = NULL;
static void (*s_ui_clear)(void) = NULL;
static void (*s_ui_toast)(const char *message) = NULL;
static char s_app_id[PLUGIN_APP_ID_MAX];
static char *s_app_data_path;
static char *s_app_base_path;
static bool s_asset_session_active;
static bool s_asset_session_display_suspended;
static plugin_permission_t s_permissions = 0;
static bool s_allow_absolute_storage = false;
static size_t s_memory_limit = 0;
static size_t s_memory_used = 0;
static bool s_api_active = false;
static SemaphoreHandle_t s_api_mutex = NULL;
static portMUX_TYPE s_input_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_input_held;
static uint32_t s_input_pressed;
static uint32_t s_input_released;
static uint32_t s_input_last_change_ms;

/* Assets an app's materialize pass chose to leave inside its .gapp archive
   rather than extract to the SD cache (see plugin_installer.c's ".direct_index"
   writer). Loaded once when the app's base path is set, freed on unload. */
typedef struct {
    char name[96];
    uint32_t offset;
    uint32_t length;
} plugin_direct_asset_t;
static char *s_direct_gapp_path;
static plugin_direct_asset_t *s_direct_assets = NULL;
static int s_direct_asset_count = 0;

static void plugin_api_direct_index_reset(void) {
    if (s_direct_assets) {
        free(s_direct_assets);
        s_direct_assets = NULL;
    }
    s_direct_asset_count = 0;
    free(s_direct_gapp_path);
    s_direct_gapp_path = NULL;
}

static void plugin_api_direct_index_load(const char *base_path) {
    plugin_api_direct_index_reset();
    if (!base_path || base_path[0] == '\0') return;

    char index_path[PLUGIN_APP_PATH_MAX];
    if (snprintf(index_path, sizeof(index_path), "%s/.direct_index", base_path) <= 0) return;
    FILE *f = fopen(index_path, "rb");
    if (!f) return;

    uint8_t magic[4];
    uint16_t version = 0, gapp_len = 0;
    uint32_t count = 0;
    bool header_ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "DIDX", 4) == 0 &&
                     fread(&version, sizeof(version), 1, f) == 1 && version == 1 &&
                     fread(&gapp_len, sizeof(gapp_len), 1, f) == 1 &&
                     gapp_len > 0 && gapp_len < PLUGIN_APP_PATH_MAX;
    if (header_ok) {
        s_direct_gapp_path = malloc((size_t)gapp_len + 1);
        header_ok = s_direct_gapp_path && fread(s_direct_gapp_path, 1, gapp_len, f) == gapp_len;
    }
    if (header_ok) {
        s_direct_gapp_path[gapp_len] = '\0';
        header_ok = fread(&count, sizeof(count), 1, f) == 1 && count > 0 && count <= 256;
    }
    if (!header_ok) {
        fclose(f);
        free(s_direct_gapp_path);
        s_direct_gapp_path = NULL;
        return;
    }

    plugin_direct_asset_t *entries = heap_caps_malloc(sizeof(plugin_direct_asset_t) * count,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) entries = malloc(sizeof(plugin_direct_asset_t) * count);
    if (!entries) {
        fclose(f);
        free(s_direct_gapp_path);
        s_direct_gapp_path = NULL;
        return;
    }

    uint32_t loaded = 0;
    for (; loaded < count; ++loaded) {
        uint16_t name_len = 0;
        if (fread(&name_len, sizeof(name_len), 1, f) != 1 || name_len == 0 ||
            name_len >= sizeof(entries[loaded].name)) break;
        if (fread(entries[loaded].name, 1, name_len, f) != name_len) break;
        entries[loaded].name[name_len] = '\0';
        if (fread(&entries[loaded].offset, sizeof(uint32_t), 1, f) != 1) break;
        if (fread(&entries[loaded].length, sizeof(uint32_t), 1, f) != 1) break;
    }
    fclose(f);

    if (loaded != count) {
        free(entries);
        free(s_direct_gapp_path);
        s_direct_gapp_path = NULL;
        return;
    }
    s_direct_assets = entries;
    s_direct_asset_count = (int)count;
}

static const plugin_direct_asset_t *plugin_api_direct_index_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < s_direct_asset_count; ++i) {
        if (strcmp(s_direct_assets[i].name, name) == 0) return &s_direct_assets[i];
    }
    return NULL;
}

static uint16_t plugin_ap_count = 0;
static wifi_ap_record_t *plugin_scanned_aps = NULL;
static volatile bool s_plugin_live_scan_active = false;
static bool s_plugin_ble_started = false;
static bool s_plugin_espnow_started = false;
static char *s_subghz_loaded_path;

static void plugin_wifi_snapshot_scan_results(void) {
    extern uint16_t ap_count;
    extern wifi_ap_record_t *scanned_aps;
    if (plugin_scanned_aps) {
        free(plugin_scanned_aps);
        plugin_scanned_aps = NULL;
    }
    plugin_ap_count = 0;
    if (ap_count > 0 && scanned_aps) {
        plugin_scanned_aps = spiram_malloc(ap_count * sizeof(wifi_ap_record_t));
        if (plugin_scanned_aps) {
            memcpy(plugin_scanned_aps, scanned_aps, ap_count * sizeof(wifi_ap_record_t));
            plugin_ap_count = ap_count;
        }
    }
}

static void plugin_wifi_clear_scan_results(void) {
    if (plugin_scanned_aps) {
        free(plugin_scanned_aps);
        plugin_scanned_aps = NULL;
    }
    plugin_ap_count = 0;
}

static SemaphoreHandle_t s_scan_done_sem = NULL;

static void plugin_scan_task(void *arg) {
    wifi_manager_start_scan();
    if (s_scan_done_sem) xSemaphoreGive(s_scan_done_sem);
    vTaskDelete(NULL);
}

static SemaphoreHandle_t s_async_scan_sem = NULL;
static bool s_async_scan_result = false;
static volatile bool s_plugin_async_scan_active = false;
static int64_t s_plugin_async_scan_start_ms = 0;
#ifdef CONFIG_IDF_TARGET_ESP32C5
#define PLUGIN_SCRIPT_SCAN_MIN_MS 6000
#else
#define PLUGIN_SCRIPT_SCAN_MIN_MS 5000
#endif

static void plugin_async_scan_start_task(void *arg) {
    display_manager_suspend_lvgl_task();
    esp_err_t err = ap_scan_start_async();
    s_async_scan_result = (err == ESP_OK);
    vTaskDelay(pdMS_TO_TICKS(500));
    display_manager_resume_lvgl_task();
    if (s_async_scan_sem) xSemaphoreGive(s_async_scan_sem);
    vTaskDelete(NULL);
}

static SemaphoreHandle_t s_async_finish_sem = NULL;

static void plugin_async_scan_finish_task(void *arg) {
    display_manager_suspend_lvgl_task();
    ap_scan_finish_async();
    plugin_wifi_snapshot_scan_results();
    vTaskDelay(pdMS_TO_TICKS(500));
    display_manager_resume_lvgl_task();
    if (s_async_finish_sem) xSemaphoreGive(s_async_finish_sem);
    vTaskDelete(NULL);
}

static void plugin_wifi_run_scan(void) {
    if (!s_scan_done_sem) s_scan_done_sem = xSemaphoreCreateBinary();
    if (!s_scan_done_sem) {
        wifi_manager_start_scan();
        plugin_wifi_snapshot_scan_results();
        return;
    }
    BaseType_t ok = xTaskCreate(plugin_scan_task, "plugin_scan", 8192, NULL, 4, NULL);
    if (ok != pdPASS) {
        wifi_manager_start_scan();
        plugin_wifi_snapshot_scan_results();
        return;
    }
    xSemaphoreTake(s_scan_done_sem, portMAX_DELAY);
    plugin_wifi_snapshot_scan_results();
}

#define PLUGIN_ALLOC_MAGIC 0x47415050u

typedef struct {
    uint32_t magic;
    size_t size;
} plugin_alloc_header_t;

typedef struct {
    const char *title;
    const char *text;
} plugin_ui_popup_ctx_t;

static void plugin_api_lock(void) {
    if (!s_api_mutex) s_api_mutex = xSemaphoreCreateMutex();
    if (s_api_mutex) xSemaphoreTake(s_api_mutex, portMAX_DELAY);
}

static void plugin_api_unlock(void) {
    if (s_api_mutex) xSemaphoreGive(s_api_mutex);
}

static bool plugin_api_build_app_path(const char *path, char *out, size_t out_len);
static bool plugin_api_absolute_storage_allowed(const char *path);

static bool plugin_api_has_permission(plugin_permission_t permission) {
    return (s_permissions & permission) != 0;
}

static plugin_permission_t plugin_api_permission_from_string(const char *value) {
    if (!value) return 0;
    if (strcmp(value, "ui") == 0) return PLUGIN_PERMISSION_UI;
    if (strcmp(value, "storage") == 0) return PLUGIN_PERMISSION_STORAGE;
    if (strcmp(value, "commands") == 0 || strcmp(value, "command") == 0) return PLUGIN_PERMISSION_COMMANDS;
    if (strcmp(value, "tasks") == 0) return PLUGIN_PERMISSION_TASKS;
    if (strcmp(value, "wifi") == 0) return PLUGIN_PERMISSION_WIFI;
    if (strcmp(value, "ble") == 0) return PLUGIN_PERMISSION_BLE;
    if (strcmp(value, "nfc") == 0) return PLUGIN_PERMISSION_NFC;
    if (strcmp(value, "ir") == 0 || strcmp(value, "infrared") == 0) return PLUGIN_PERMISSION_IR;
    if (strcmp(value, "subghz") == 0) return PLUGIN_PERMISSION_SUBGHZ;
    if (strcmp(value, "badusb") == 0) return PLUGIN_PERMISSION_BADUSB;
    if (strcmp(value, "raw_gpio") == 0) return PLUGIN_PERMISSION_RAW_GPIO;
    if (strcmp(value, "lvgl") == 0) return PLUGIN_PERMISSION_LVGL;
    if (strcmp(value, "rgb") == 0 || strcmp(value, "led") == 0 || strcmp(value, "leds") == 0) return PLUGIN_PERMISSION_RGB;
    if (strcmp(value, "uart") == 0 || strcmp(value, "serial") == 0) return PLUGIN_PERMISSION_UART;
    if (strcmp(value, "i2c") == 0) return PLUGIN_PERMISSION_I2C;
    if (strcmp(value, "spi") == 0) return PLUGIN_PERMISSION_SPI;
    if (strcmp(value, "adc") == 0) return PLUGIN_PERMISSION_ADC;
    if (strcmp(value, "pwm") == 0 || strcmp(value, "ledc") == 0) return PLUGIN_PERMISSION_PWM;
    if (strcmp(value, "network") == 0 || strcmp(value, "http") == 0) return PLUGIN_PERMISSION_NETWORK;
    if (strcmp(value, "wifi_control") == 0) return PLUGIN_PERMISSION_WIFI_CONTROL;
    if (strcmp(value, "power") == 0 || strcmp(value, "battery") == 0) return PLUGIN_PERMISSION_POWER;
    if (strcmp(value, "input") == 0 || strcmp(value, "buttons") == 0) return PLUGIN_PERMISSION_INPUT;
    if (strcmp(value, "display") == 0 || strcmp(value, "backlight") == 0) return PLUGIN_PERMISSION_DISPLAY;
    if (strcmp(value, "time") == 0) return PLUGIN_PERMISSION_TIME;
    if (strcmp(value, "random") == 0) return PLUGIN_PERMISSION_RANDOM;
    if (strcmp(value, "system") == 0) return PLUGIN_PERMISSION_SYSTEM;
    if (strcmp(value, "camera") == 0) return PLUGIN_PERMISSION_CAMERA;
    if (strcmp(value, "usb") == 0) return PLUGIN_PERMISSION_USB;
    if (strcmp(value, "ethernet") == 0 || strcmp(value, "eth") == 0) return PLUGIN_PERMISSION_ETHERNET;
    if (strcmp(value, "audio") == 0 || strcmp(value, "mic") == 0 || strcmp(value, "microphone") == 0) return PLUGIN_PERMISSION_AUDIO;
    if (strcmp(value, "settings") == 0) return PLUGIN_PERMISSION_SETTINGS;
    if (strcmp(value, "zigbee") == 0 || strcmp(value, "ieee802154") == 0) return PLUGIN_PERMISSION_ZIGBEE;
    if (strcmp(value, "nrf24") == 0) return PLUGIN_PERMISSION_NRF24;
    if (strcmp(value, "espnow") == 0 || strcmp(value, "esp_now") == 0) return PLUGIN_PERMISSION_ESPNOW;
    return 0;
}

static bool plugin_api_has_permission_name(const char *permission) {
    plugin_permission_t bit = plugin_api_permission_from_string(permission);
    return bit != 0 && plugin_api_has_permission(bit);
}

extern bool plugin_api_ui_screen_is_compact(void);
extern bool plugin_api_ui_has_touchscreen(void);

bool plugin_api_feature_supported(const char *feature) {
    if (!feature || feature[0] == '\0') return false;
    if (strcmp(feature, "touchscreen") == 0) return plugin_api_ui_has_touchscreen();
    if (strcmp(feature, "dpad") == 0 || strcmp(feature, "joystick") == 0) {
#ifdef CONFIG_USE_JOYSTICK
        return true;
#else
        return false;
#endif
    }
    if (strcmp(feature, "encoder") == 0) {
#ifdef CONFIG_USE_ENCODER
        return true;
#else
        return false;
#endif
    }
    if (strcmp(feature, "keyboard") == 0 || strcmp(feature, "physical_keyboard") == 0) {
#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV) || defined(CONFIG_USE_TDECK) || defined(CONFIG_USE_USB_KEYBOARD)
        return true;
#else
        return false;
#endif
    }
    if (strcmp(feature, "compact_screen") == 0) return plugin_api_ui_screen_is_compact();
    if (strcmp(feature, "banshee_c5") == 0) {
#ifdef CONFIG_USE_C5_PARLIO_DISPLAY
        return true;
#else
        return false;
#endif
    }
    if (strcmp(feature, "wifi") == 0) return true;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (strcmp(feature, "ble") == 0) return true;
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    if (strcmp(feature, "subghz") == 0) return subghz_remote_manager_is_ready();
#endif
#ifdef CONFIG_HAS_NRF24
    if (strcmp(feature, "nrf24") == 0) return true;
#endif
#ifdef CONFIG_HAS_CAMERA
    if (strcmp(feature, "camera") == 0) return true;
#endif
#ifdef CONFIG_HAS_BADUSB
    if (strcmp(feature, "usb") == 0 || strcmp(feature, "badusb") == 0) return true;
#endif
#ifdef CONFIG_HAS_INFRARED
    if (strcmp(feature, "ir") == 0 || strcmp(feature, "infrared") == 0) return true;
#endif
#ifdef CONFIG_HAS_ZIGBEE
    if (strcmp(feature, "zigbee") == 0) return true;
#endif
    return false;
}

static bool plugin_api_has_feature(const char *feature) {
    if (feature && strcmp(feature, "absolute_storage") == 0) return s_allow_absolute_storage;
    if (feature && strcmp(feature, "persistent_storage") == 0) {
        return sd_card_manager.is_initialized && !sd_card_needs_jit_mount();
    }
    return plugin_api_feature_supported(feature);
}

static bool plugin_api_has_ui_permission(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_UI) || plugin_api_has_permission(PLUGIN_PERMISSION_LVGL);
}

static void plugin_ui_sync_apply(void *arg) {
    plugin_ui_sync_call_t *call = (plugin_ui_sync_call_t *)arg;
    if (call && call->fn) call->fn(call->ctx);
    if (call && call->done) xSemaphoreGive(call->done);
}

static bool plugin_ui_run_sync(void (*fn)(void *ctx), void *ctx) {
    if (!fn) return false;
    if (display_manager_is_lvgl_task()) {
        fn(ctx);
        return true;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (!done) return false;
    plugin_ui_sync_call_t call = {
        .fn = fn,
        .ctx = ctx,
        .done = done,
    };
    display_manager_run_on_lvgl(plugin_ui_sync_apply, &call);
    /* The queued call borrows both call and ctx. They must remain alive until
       LVGL has completed the callback; there is no safe timeout without queue
       cancellation support from display_manager_run_on_lvgl(). */
    bool ok = xSemaphoreTake(done, portMAX_DELAY) == pdTRUE;
    vSemaphoreDelete(done);
    return ok;
}

static lv_obj_t *plugin_ui_parent_or_current(ghostesp_ui_obj_t parent) {
    if (parent && lv_obj_is_valid((lv_obj_t *)parent)) return (lv_obj_t *)parent;
    View *view = display_manager_get_current_view();
    if (view && view->root && lv_obj_is_valid(view->root)) return view->root;
    return lv_scr_act();
}

bool plugin_api_internal_has_ui_permission(void) {
    return plugin_api_has_ui_permission();
}

bool plugin_api_internal_has_permission(plugin_permission_t permission) {
    return plugin_api_has_permission(permission);
}

bool plugin_api_internal_build_app_path(const char *path, char *out, size_t out_len) {
    return plugin_api_build_app_path(path, out, out_len);
}

bool plugin_api_internal_absolute_storage_allowed(const char *path) {
    return plugin_api_absolute_storage_allowed(path);
}

const char *plugin_api_internal_app_id(void) {
    return s_app_id;
}

bool plugin_api_internal_run_sync(void (*fn)(void *ctx), void *ctx) {
    return plugin_ui_run_sync(fn, ctx);
}

bool plugin_api_internal_run_async(void (*fn)(void *ctx), void *ctx) {
    return display_manager_run_on_lvgl(fn, ctx);
}

bool plugin_api_internal_run_async_nowait(void (*fn)(void *ctx), void *ctx) {
    return display_manager_run_on_lvgl_nowait(fn, ctx);
}

lv_obj_t *plugin_api_internal_parent_or_current(ghostesp_ui_obj_t parent) {
    return plugin_ui_parent_or_current(parent);
}

static void plugin_ui_style_panel(lv_obj_t *obj) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_bg_color(obj, lv_color_hex(theme_palette_get_surface(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(theme_palette_get_border(theme)), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, GUI_RADIUS_MD, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, GUI_GRID * 3, LV_PART_MAIN);
}

static void plugin_ui_style_button(lv_obj_t *obj) {
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_bg_color(obj, lv_color_hex(theme_palette_get_surface_alt(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(obj, lv_color_hex(theme_palette_get_border(theme)), LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, GUI_RADIUS_MD, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(obj, GUI_GRID * 4, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(obj, GUI_GRID * 3, LV_PART_MAIN);
}

static void plugin_ui_button_event_cb(lv_event_t *event) {
    plugin_ui_button_ctx_t *ctx = (plugin_ui_button_ctx_t *)lv_event_get_user_data(event);
    if (!ctx) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        if (ctx->cb) ctx->cb(ctx->user);
    } else if (code == LV_EVENT_DELETE) {
        free(ctx);
    }
}

static void plugin_ui_delete_user_obj_event_cb(lv_event_t *event) {
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_user_data(event);
    if (obj && lv_obj_is_valid(obj)) lv_obj_del(obj);
}

static bool plugin_api_path_is_ghostesp_absolute(const char *path) {
    if (!path || path[0] == '\0') return false;
    if (strchr(path, '\\') || strstr(path, "..")) return false;
    return strcmp(path, "/mnt/ghostesp") == 0 || strncmp(path, "/mnt/ghostesp/", 14) == 0;
}

static bool plugin_api_absolute_storage_allowed(const char *path) {
    return s_allow_absolute_storage && plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) && plugin_api_path_is_ghostesp_absolute(path);
}

static bool plugin_api_build_app_path(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0 || !plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) ||
        !s_app_data_path || s_app_data_path[0] == '\0') return false;
    if (!path || path[0] == '\0') {
        int n = snprintf(out, out_len, "%s", s_app_data_path);
        return n > 0 && (size_t)n < out_len;
    }
    if (path[0] == '/' || path[0] == '\\' || strstr(path, "..")) return false;
    int n = snprintf(out, out_len, "%s/%s", s_app_data_path, path);
    return n > 0 && (size_t)n < out_len;
}

static void plugin_api_log(const char *message) {
    if (!message) return;
    ESP_LOGI(TAG, "%s", message);
    glog("[app] %s\n", message);
}

static void plugin_api_ui_set_title(const char *title) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_set_title) s_ui_set_title(title ? title : "App");
}

static void plugin_api_ui_print(const char *text) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_print) s_ui_print(text ? text : "");
}

static void plugin_api_ui_clear(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_clear) s_ui_clear();
}

static void plugin_api_toast(const char *message) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    if (s_ui_toast) s_ui_toast(message ? message : "");
    else if (message) glog("[app] %s\n", message);
}

static void plugin_api_ui_show_text(const char *title, const char *text) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_UI)) return;
    plugin_api_ui_set_title(title);
    plugin_api_ui_clear();
    plugin_api_ui_print(text);
}

static void plugin_api_ui_screen_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *root = plugin_ui_parent_or_current(NULL);
    if (!root) return;

    lv_obj_clean(root);
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_bg_color(root, lv_color_hex(theme_palette_get_background(theme)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    display_manager_add_status_bar(ctx->title && ctx->title[0] ? ctx->title : "SD App");

    lv_obj_t *content = gui_screen_create_content(root, GUI_STATUS_BAR_HEIGHT);
    if (!content) return;
    lv_obj_set_style_pad_all(content, GUI_GRID * 2, LV_PART_MAIN);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, GUI_GRID * 2, LV_PART_MAIN);
    ctx->result = content;
}

static ghostesp_ui_obj_t plugin_api_ui_screen_create(const char *title) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .title = title };
    return plugin_ui_run_sync(plugin_api_ui_screen_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_card_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *card = lv_obj_create(parent);
    if (!card) return;
    plugin_ui_style_panel(card);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, GUI_GRID * 2, LV_PART_MAIN);
    ctx->result = card;
}

static ghostesp_ui_obj_t plugin_api_ui_card_create(ghostesp_ui_obj_t parent) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent };
    return plugin_ui_run_sync(plugin_api_ui_card_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_label_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return;
    lv_label_set_text(label, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(100));
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_text_color(label, lv_color_hex(theme_palette_get_text(theme)), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, gui_font_body(), LV_PART_MAIN);
    ctx->result = label;
}

static ghostesp_ui_obj_t plugin_api_ui_label_create(ghostesp_ui_obj_t parent, const char *text) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent, .text = text };
    return plugin_ui_run_sync(plugin_api_ui_label_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_button_create_now(void *arg) {
    plugin_ui_create_ctx_t *ctx = (plugin_ui_create_ctx_t *)arg;
    lv_obj_t *parent = plugin_ui_parent_or_current(ctx->parent);
    lv_obj_t *button = lv_btn_create(parent);
    gui_apply_pressed_style(button);
    if (!button) return;
    plugin_ui_style_button(button);
    lv_obj_set_width(button, LV_PCT(100));

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_center(label);
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_text_color(label, lv_color_hex(theme_palette_get_text(theme)), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, gui_font_body(), LV_PART_MAIN);

    plugin_ui_button_ctx_t *button_ctx = calloc(1, sizeof(*button_ctx));
    if (button_ctx) {
        button_ctx->cb = ctx->cb;
        button_ctx->user = ctx->user;
        lv_obj_add_event_cb(button, plugin_ui_button_event_cb, LV_EVENT_ALL, button_ctx);
    }
    ctx->result = button;
}

static ghostesp_ui_obj_t plugin_api_ui_button_create(ghostesp_ui_obj_t parent, const char *text, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!plugin_api_has_ui_permission()) return NULL;
    plugin_ui_create_ctx_t ctx = { .parent = parent, .text = text, .cb = on_click, .user = user };
    return plugin_ui_run_sync(plugin_api_ui_button_create_now, &ctx) ? ctx.result : NULL;
}

static void plugin_api_ui_label_set_text_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *label = (lv_obj_t *)ctx->obj;
    if (label && lv_obj_is_valid(label)) lv_label_set_text(label, ctx->text ? ctx->text : "");
}

static void plugin_api_ui_label_set_text(ghostesp_ui_obj_t label, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = label, .text = text };
    plugin_ui_run_sync(plugin_api_ui_label_set_text_now, &ctx);
}

static void plugin_api_ui_button_set_text_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *button = (lv_obj_t *)ctx->obj;
    if (!button || !lv_obj_is_valid(button) || lv_obj_get_child_cnt(button) == 0) return;
    lv_obj_t *label = lv_obj_get_child(button, 0);
    if (label && lv_obj_is_valid(label)) lv_label_set_text(label, ctx->text ? ctx->text : "");
}

static void plugin_api_ui_button_set_text(ghostesp_ui_obj_t button, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = button, .text = text };
    plugin_ui_run_sync(plugin_api_ui_button_set_text_now, &ctx);
}

static void plugin_api_ui_obj_set_visible_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (!obj || !lv_obj_is_valid(obj)) return;
    if (ctx->visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void plugin_api_ui_obj_set_visible(ghostesp_ui_obj_t obj, bool visible) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj, .visible = visible };
    plugin_ui_run_sync(plugin_api_ui_obj_set_visible_now, &ctx);
}

static void plugin_api_ui_obj_delete_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    lv_obj_t *obj = (lv_obj_t *)ctx->obj;
    if (obj && lv_obj_is_valid(obj)) lv_obj_del(obj);
}

static void plugin_api_ui_obj_delete(ghostesp_ui_obj_t obj) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .obj = obj };
    plugin_ui_run_sync(plugin_api_ui_obj_delete_now, &ctx);
}

static void plugin_api_ui_set_status_now(void *arg) {
    plugin_ui_obj_ctx_t *ctx = (plugin_ui_obj_ctx_t *)arg;
    display_manager_add_status_bar(ctx->text ? ctx->text : "SD App");
}

static void plugin_api_ui_set_status(const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_obj_ctx_t ctx = { .text = text };
    plugin_ui_run_sync(plugin_api_ui_set_status_now, &ctx);
}

static void plugin_api_ui_show_popup_now(void *arg) {
    plugin_ui_popup_ctx_t *ctx = (plugin_ui_popup_ctx_t *)arg;
    lv_obj_t *overlay = lv_obj_create(plugin_ui_parent_or_current(NULL));
    if (!overlay) return;
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(overlay);
    plugin_ui_style_panel(box);
    lv_obj_set_width(box, LV_PCT(82));
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, GUI_GRID * 3, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, ctx->title ? ctx->title : "App");
    lv_obj_set_width(title, LV_PCT(100));
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_obj_set_style_text_color(title, lv_color_hex(theme_palette_get_text(theme)), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, gui_font_title(), LV_PART_MAIN);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body, ctx->text ? ctx->text : "");
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(theme_palette_get_text_muted(theme)), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, gui_font_body(), LV_PART_MAIN);

    lv_obj_t *close = lv_btn_create(box);
    gui_apply_pressed_style(close);
    plugin_ui_style_button(close);
    lv_obj_set_width(close, LV_PCT(100));
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, "Close");
    lv_obj_set_style_text_color(close_label, lv_color_hex(theme_palette_get_text(theme)), LV_PART_MAIN);
    lv_obj_center(close_label);
    lv_obj_add_event_cb(close, plugin_ui_delete_user_obj_event_cb, LV_EVENT_CLICKED, overlay);
}

static void plugin_api_ui_show_popup(const char *title, const char *text) {
    if (!plugin_api_has_ui_permission()) return;
    plugin_ui_popup_ctx_t ctx = { .title = title, .text = text };
    plugin_ui_run_sync(plugin_api_ui_show_popup_now, &ctx);
}

static bool plugin_api_command_exec(const char *command) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_COMMANDS)) return false;
    if (!command || command[0] == '\0') return false;
    if (!s_api_active) return false;
    simulateCommand(command);
    return true;
}

static bool plugin_api_storage_exists(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return sd_card_exists(path);
}

static int plugin_api_storage_read(const char *path, void *buffer, size_t buffer_len) {
    if (!plugin_api_absolute_storage_allowed(path) || !buffer || buffer_len == 0) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return (int)n;
}

static bool plugin_api_build_asset_path(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0 || !plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) ||
        !s_app_base_path || s_app_base_path[0] == '\0' || !path || path[0] == '\0') return false;
    if (path[0] == '/' || path[0] == '\\' || strstr(path, "..")) return false;
    int n = snprintf(out, out_len, "%s/assets/%s", s_app_base_path, path);
    return n > 0 && (size_t)n < out_len;
}

/* Callers doing many small offset-reads against the same file within one
   asset_session_begin()/end() bracket (e.g. a game streaming WAD lumps
   during gameplay) used to pay a full fopen/fclose per read here, which on
   some SD paths dominated frame time far more than the actual data
   transfer. Kept open across reads of the same path while a session is
   active; closed on session end, on a path change, or with no active
   session (matching the original always-close-immediately behavior). */
static FILE *s_read_cache_file = NULL;
static char *s_read_cache_path;

static void plugin_api_read_cache_close(void) {
    if (s_read_cache_file) {
        fclose(s_read_cache_file);
        s_read_cache_file = NULL;
    }
    free(s_read_cache_path);
    s_read_cache_path = NULL;
}

static int plugin_api_read_at_path(const char *path, uint32_t offset,
                                   void *buffer, size_t buffer_len) {
    if (!path || !buffer || buffer_len == 0 || buffer_len > INT_MAX || offset > LONG_MAX) return -1;

    bool display_was_suspended = false;
    if (!sd_card_jit_begin(&display_was_suspended, false)) return -1;

    int result = -1;
    FILE *f;
    if (s_asset_session_active && s_read_cache_file && s_read_cache_path &&
        strcmp(s_read_cache_path, path) == 0) {
        f = s_read_cache_file;
    } else {
        plugin_api_read_cache_close();
        f = fopen(path, "rb");
        if (f && s_asset_session_active) {
            s_read_cache_path = strdup(path);
            if (s_read_cache_path) s_read_cache_file = f;
        }
    }
    if (f) {
        if (fseek(f, (long)offset, SEEK_SET) == 0) {
            result = (int)fread(buffer, 1, buffer_len, f);
        }
        if (!s_asset_session_active || f != s_read_cache_file) {
            if (f == s_read_cache_file) plugin_api_read_cache_close();
            else fclose(f);
        }
    }

    sd_card_jit_end(display_was_suspended);
    return result;
}

static int plugin_api_storage_read_at(const char *path, uint32_t offset,
                                      void *buffer, size_t buffer_len) {
    if (!plugin_api_absolute_storage_allowed(path)) return -1;
    return plugin_api_read_at_path(path, offset, buffer, buffer_len);
}

static bool plugin_api_storage_write(const char *path, const void *data, size_t len) {
    if (!plugin_api_absolute_storage_allowed(path) || (!data && len > 0)) return false;
    return sd_card_write_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_append(const char *path, const void *data, size_t len) {
    if (!plugin_api_absolute_storage_allowed(path) || (!data && len > 0)) return false;
    return sd_card_append_file(path, data, len) == ESP_OK;
}

static bool plugin_api_storage_delete(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return unlink(path) == 0;
}

static bool plugin_api_storage_mkdir(const char *path) {
    if (!plugin_api_absolute_storage_allowed(path)) return false;
    return sd_card_create_directory(path) == ESP_OK;
}

static int plugin_api_storage_list(const char *path, ghostesp_storage_entry_t *out, int max_entries) {
    if (!plugin_api_absolute_storage_allowed(path) || !out || max_entries <= 0) return -1;
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        strncpy(out[count].name, entry->d_name, GHOSTESP_STORAGE_NAME_MAX - 1);
        out[count].name[GHOSTESP_STORAGE_NAME_MAX - 1] = '\0';
        struct stat st;
        char full_path[256];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
#pragma GCC diagnostic pop
        out[count].is_directory = (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(dir);
    return count;
}

static const char *plugin_api_app_id(void) {
    return s_app_id;
}

static const char *plugin_api_app_data_path(void) {
    return s_app_data_path ? s_app_data_path : "";
}

static bool plugin_api_app_storage_exists(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return sd_card_exists(full_path);
}

static int plugin_api_app_storage_read(const char *path, void *buffer, size_t buffer_len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || !buffer || buffer_len == 0) return -1;
    FILE *f = fopen(full_path, "rb");
    if (!f) return -1;
    size_t n = fread(buffer, 1, buffer_len, f);
    fclose(f);
    return (int)n;
}

static int plugin_api_app_storage_read_at(const char *path, uint32_t offset,
                                           void *buffer, size_t buffer_len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return -1;
    return plugin_api_read_at_path(full_path, offset, buffer, buffer_len);
}

static int plugin_api_asset_storage_read_at(const char *path, uint32_t offset,
                                             void *buffer, size_t buffer_len) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) || !path || path[0] == '\0') return -1;
    const plugin_direct_asset_t *direct = plugin_api_direct_index_find(path);
    if (direct) {
        /* Served straight out of the installed .gapp at the offset recorded
           by materialize; clamp so a caller can never read past this asset's
           own bytes into whatever entry follows it in the archive. */
        if (offset >= direct->length) return 0;
        size_t avail = direct->length - offset;
        size_t want = buffer_len < avail ? buffer_len : avail;
        return plugin_api_read_at_path(s_direct_gapp_path, direct->offset + offset, buffer, want);
    }
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_asset_path(path, full_path, sizeof(full_path))) return -1;
    return plugin_api_read_at_path(full_path, offset, buffer, buffer_len);
}

static int64_t plugin_api_asset_storage_size(const char *path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE) || !path || path[0] == '\0') return -1;
    const plugin_direct_asset_t *direct = plugin_api_direct_index_find(path);
    if (direct) return (int64_t)direct->length;
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_asset_path(path, full_path, sizeof(full_path))) return -1;
    struct stat st;
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) return -1;
    return (int64_t)st.st_size;
}

static bool plugin_api_asset_path(const char *path, char *out, size_t out_len) {
    return plugin_api_build_asset_path(path, out, out_len);
}

static bool plugin_api_asset_session_begin(void) {
    if (s_asset_session_active || !plugin_api_has_permission(PLUGIN_PERMISSION_STORAGE)) return false;
    bool display_was_suspended = false;
    if (!sd_card_jit_begin(&display_was_suspended, false)) return false;
    s_asset_session_display_suspended = display_was_suspended;
    s_asset_session_active = true;
    return true;
}

static void plugin_api_asset_session_end(void) {
    if (!s_asset_session_active) return;
    bool display_was_suspended = s_asset_session_display_suspended;
    s_asset_session_active = false;
    s_asset_session_display_suspended = false;
    plugin_api_read_cache_close();
    sd_card_jit_end(display_was_suspended);
}

static void plugin_api_request_exit(void) {
    plugin_runner_request_exit();
}

void plugin_api_record_joystick_state(unsigned int index, bool pressed) {
    if (index >= 5 || !s_api_active) return;
    uint32_t bit = 1u << index;
    portENTER_CRITICAL(&s_input_snapshot_mux);
    if (pressed) {
        if (!(s_input_held & bit)) {
            s_input_pressed |= bit;
            s_input_last_change_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        }
        s_input_held |= bit;
    } else {
        if (s_input_held & bit) {
            s_input_released |= bit;
            s_input_last_change_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        }
        s_input_held &= ~bit;
    }
    portEXIT_CRITICAL(&s_input_snapshot_mux);
}

static bool plugin_api_input_snapshot(ghostesp_input_snapshot_t *out) {
    if (!out || !s_api_active || !plugin_api_has_permission(PLUGIN_PERMISSION_INPUT)) return false;
    portENTER_CRITICAL(&s_input_snapshot_mux);
    out->held = s_input_held;
    out->pressed = s_input_pressed;
    out->released = s_input_released;
    out->last_change_ms = s_input_last_change_ms;
    s_input_pressed = 0;
    s_input_released = 0;
    portEXIT_CRITICAL(&s_input_snapshot_mux);
    return true;
}

static bool plugin_api_app_storage_write(const char *path, const void *data, size_t len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || (!data && len > 0)) return false;
    return sd_card_write_file(full_path, data, len) == ESP_OK;
}

static bool plugin_api_app_storage_append(const char *path, const void *data, size_t len) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || (!data && len > 0)) return false;
    return sd_card_append_file(full_path, data, len) == ESP_OK;
}

static bool plugin_api_app_storage_delete(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return unlink(full_path) == 0;
}

static bool plugin_api_app_storage_mkdir(const char *path) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path))) return false;
    return sd_card_create_directory(full_path) == ESP_OK;
}

static int plugin_api_app_storage_list(const char *path, ghostesp_storage_entry_t *out, int max_entries) {
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(path, full_path, sizeof(full_path)) || !out || max_entries <= 0) return -1;
    DIR *dir = opendir(full_path);
    if (!dir) return -1;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        strncpy(out[count].name, entry->d_name, GHOSTESP_STORAGE_NAME_MAX - 1);
        out[count].name[GHOSTESP_STORAGE_NAME_MAX - 1] = '\0';
        struct stat st;
        char child_path[PLUGIN_APP_PATH_MAX];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
        snprintf(child_path, sizeof(child_path), "%s/%s", full_path, entry->d_name);
#pragma GCC diagnostic pop
        out[count].is_directory = (stat(child_path, &st) == 0 && S_ISDIR(st.st_mode));
        count++;
    }
    closedir(dir);
    return count;
}

static bool plugin_api_badusb_run_script(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BADUSB)) return false;
#ifdef CONFIG_HAS_BADUSB
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    return badusb_manager_execute_file(full_path) == ESP_OK;
#else
    return false;
#endif
}

static bool plugin_api_badusb_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BADUSB)) return false;
#ifdef CONFIG_HAS_BADUSB
    return badusb_manager_stop() == ESP_OK;
#else
    return false;
#endif
}

static bool plugin_api_ir_send_file(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    char full_path[PLUGIN_APP_PATH_MAX];
    infrared_signal_t signal = {0};
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    if (!infrared_manager_read_file(full_path, &signal)) return false;
    ghostchi_manager_add_xp(4);
    bool ok = infrared_manager_transmit(&signal);
    infrared_manager_free_signal(&signal);
    return ok;
#else
    return false;
#endif
}

static bool plugin_api_ir_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_IR)) return false;
#ifdef CONFIG_HAS_INFRARED
    infrared_manager_dazzler_stop();
    return true;
#else
    return false;
#endif
}

static bool plugin_api_nfc_is_available(void) {
#if defined(CONFIG_NFC_PN532) || defined(CONFIG_NFC_ST25R3916)
    return true;
#else
    return false;
#endif
}

static bool plugin_api_nfc_read_start(void) {
    return plugin_api_nfc_t2_scan_start();
}

static bool plugin_api_nfc_stop(void) {
    return plugin_api_nfc_t2_scan_stop();
}

static bool plugin_api_ble_start_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    ghostscript_runtime_reset_ble_seen();
    s_plugin_ble_started = gatt_scan_start();
    return s_plugin_ble_started;
#else
    return false;
#endif
}

static bool plugin_api_ble_stop_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    s_plugin_ble_started = false;
    ble_stop();
    return true;
#else
    return false;
#endif
}

static int plugin_api_ble_device_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return 0;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    return ble_get_gatt_device_count();
#else
    return 0;
#endif
}

static bool plugin_api_ble_get_device(int index, ghostesp_ble_device_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE) || !out) return false;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    memset(out, 0, sizeof(*out));
    return ble_get_gatt_device_data(index, out->mac, &out->rssi, out->name, sizeof(out->name)) == 0;
#else
    return false;
#endif
}

static bool plugin_api_subghz_is_available(void) {
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    return subghz_remote_manager_is_ready();
#else
    return false;
#endif
}

static bool plugin_subghz_key_match(const char *line, const char *key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < klen; i++) {
        char a = line[i], b = key[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

static const char *plugin_subghz_skip_key_colon(const char *line, size_t key_len) {
    const char *p = line + key_len;
    while (*p == ' ' || *p == ':') p++;
    return p;
}

static void plugin_subghz_trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ')) {
        s[--len] = '\0';
    }
}

static subghz_preset_t plugin_subghz_parse_preset(const char *v) {
    if (strstr(v, "Ook650") || strstr(v, "OOK650") || strstr(v, "ook650")) return SUBGHZ_PRESET_OOK650_ASYNC;
    if (strstr(v, "2FSK") && strstr(v, "Dev238")) return SUBGHZ_PRESET_2FSK_DEV238_ASYNC;
    if (strstr(v, "2FSK") && strstr(v, "Dev476")) return SUBGHZ_PRESET_2FSK_DEV476_ASYNC;
    if (strstr(v, "Custom") || strstr(v, "custom")) return SUBGHZ_PRESET_CUSTOM;
    return SUBGHZ_PRESET_OOK270_ASYNC;
}

static size_t plugin_subghz_sanitize_pulses(int32_t *durations, size_t count) {
    if (!durations || count == 0) return 0;
    size_t write = 0;
    for (size_t i = 0; i < count; i++) {
        if (durations[i] == 0) continue;
        durations[write++] = durations[i];
    }
    return write;
}

static bool plugin_subghz_parse_raw_file(const char *path, int32_t *out, size_t max_count, size_t *out_count, uint32_t *out_frequency_hz, subghz_preset_t *out_preset) {
    if (!path || !out || max_count == 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    size_t count = 0;
    uint32_t freq = 0;
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    bool in_raw_data = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        plugin_subghz_trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') {
            in_raw_data = false;
            continue;
        }

        if (plugin_subghz_key_match(line, "Frequency:")) {
            in_raw_data = false;
            freq = (uint32_t)atoi(plugin_subghz_skip_key_colon(line, 10));
        } else if (plugin_subghz_key_match(line, "Preset:")) {
            in_raw_data = false;
            preset = plugin_subghz_parse_preset(plugin_subghz_skip_key_colon(line, 7));
        } else if (plugin_subghz_key_match(line, "RAW_Data:")) {
            in_raw_data = true;
            char *saveptr = NULL;
            char *tok = strtok_r((char *)plugin_subghz_skip_key_colon(line, 9), " ,", &saveptr);
            while (tok && count < max_count) {
                out[count++] = (int32_t)strtol(tok, NULL, 10);
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else if (in_raw_data) {
            char *saveptr = NULL;
            char *tok = strtok_r(line, " ,", &saveptr);
            while (tok && count < max_count) {
                if (*tok != '\0') out[count++] = (int32_t)strtol(tok, NULL, 10);
                tok = strtok_r(NULL, " ,", &saveptr);
            }
        } else {
            in_raw_data = false;
        }
    }
    fclose(f);

    if (out_count) *out_count = count;
    if (out_frequency_hz) *out_frequency_hz = freq;
    if (out_preset) *out_preset = preset;
    return count > 0;
}

static bool plugin_subghz_parse_decoded_file(const char *path, int32_t *out, size_t max_count, size_t *out_count, uint32_t *out_frequency_hz, subghz_preset_t *out_preset) {
    if (!path || !out || max_count == 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char protocol[SUBGHZ_DECODED_PROTO_MAX] = {0};
    int bits = 0;
    int frequency_hz = 0;
    uint64_t code = 0;
    int key_bytes = 0;
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    char line[384];
    while (fgets(line, sizeof(line), f)) {
        plugin_subghz_trim_newline(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        if (plugin_subghz_key_match(line, "Frequency:")) {
            frequency_hz = atoi(plugin_subghz_skip_key_colon(line, 10));
        } else if (plugin_subghz_key_match(line, "Protocol:")) {
            snprintf(protocol, sizeof(protocol), "%.*s", (int)sizeof(protocol) - 1, plugin_subghz_skip_key_colon(line, 9));
        } else if (plugin_subghz_key_match(line, "Preset:")) {
            preset = plugin_subghz_parse_preset(plugin_subghz_skip_key_colon(line, 7));
        } else if (plugin_subghz_key_match(line, "Bit:")) {
            bits = atoi(plugin_subghz_skip_key_colon(line, 4));
        } else if (plugin_subghz_key_match(line, "Key:")) {
            char *saveptr = NULL;
            char *tok = strtok_r((char *)plugin_subghz_skip_key_colon(line, 4), " ", &saveptr);
            while (tok) {
                unsigned long b = strtoul(tok, NULL, 16);
                code = (code << 8) | (uint64_t)(b & 0xFFUL);
                key_bytes++;
                tok = strtok_r(NULL, " ", &saveptr);
            }
        }
    }
    fclose(f);

    if (protocol[0] == '\0' || strcmp(protocol, "RAW") == 0 || key_bytes <= 0) return false;
    subghz_decoded_signal_t decoded = {0};
    decoded.decoded = true;
    decoded.code = code;
    decoded.bits = subghz_normalize_decoded_bits(protocol, (bits > 0) ? bits : (key_bytes * 8));
    decoded.frequency_hz = (frequency_hz > 0) ? frequency_hz : 433920000;
    decoded.te = (int)subghz_protocol_te(protocol);
    snprintf(decoded.protocol, sizeof(decoded.protocol), "%s", protocol);
    if (!subghz_build_raw_from_decoded(decoded.protocol, decoded.code, decoded.bits, out, max_count, out_count)) return false;
    if (out_frequency_hz) *out_frequency_hz = (uint32_t)decoded.frequency_hz;
    if (out_preset) *out_preset = preset;
    return out_count && *out_count > 0;
}

static bool plugin_api_subghz_load_snapshot(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    free(s_subghz_loaded_path);
    s_subghz_loaded_path = NULL;
    bool loaded_snapshot = subghz_remote_manager_load_snapshot(full_path);
    if (loaded_snapshot || sd_card_exists(full_path)) {
        s_subghz_loaded_path = strdup(full_path);
        return s_subghz_loaded_path != NULL;
    }
    return false;
#else
    return false;
#endif
}

static bool plugin_subghz_transmit_path(const char *full_path);

static bool plugin_api_subghz_transmit_loaded(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    return s_subghz_loaded_path && plugin_subghz_transmit_path(s_subghz_loaded_path);
#else
    return false;
#endif
}

static bool plugin_subghz_transmit_path(const char *full_path) {
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    if (!full_path || full_path[0] == '\0') return false;
    int32_t *durations = malloc(SUBGHZ_RAW_MAX_DURATIONS * sizeof(*durations));
    if (!durations) return false;
    size_t count = 0;
    uint32_t frequency_hz = 0;
    subghz_preset_t preset = SUBGHZ_PRESET_OOK270_ASYNC;
    if (!plugin_subghz_parse_raw_file(full_path, durations, SUBGHZ_RAW_MAX_DURATIONS, &count, &frequency_hz, &preset) &&
        !plugin_subghz_parse_decoded_file(full_path, durations, SUBGHZ_RAW_MAX_DURATIONS, &count, &frequency_hz, &preset)) {
        free(durations);
        return false;
    }
    count = plugin_subghz_sanitize_pulses(durations, count);
    if (count == 0) {
        free(durations);
        return false;
    }
    if (frequency_hz == 0) frequency_hz = 433920000;
    bool ok = subghz_remote_manager_transmit_raw(durations, count, frequency_hz, preset);
    free(durations);
    return ok;
#else
    (void)full_path;
    return false;
#endif
}

static bool plugin_api_subghz_transmit_file(const char *app_relative_path) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
    char full_path[PLUGIN_APP_PATH_MAX];
    if (!plugin_api_build_app_path(app_relative_path, full_path, sizeof(full_path))) return false;
    return plugin_subghz_transmit_path(full_path);
}

static bool plugin_api_subghz_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_SUBGHZ)) return false;
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    subghz_remote_manager_stop();
    return true;
#else
    return false;
#endif
}

static void plugin_api_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void *plugin_api_app_malloc(size_t size) {
    if (size == 0) return NULL;
    if (s_memory_limit > 0 && s_memory_used + size > s_memory_limit) return NULL;
    plugin_alloc_header_t *header = heap_caps_malloc(sizeof(*header) + size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!header) {
        header = malloc(sizeof(*header) + size);
    }
    if (!header) return NULL;
    header->magic = PLUGIN_ALLOC_MAGIC;
    header->size = size;
    s_memory_used += size;
    return header + 1;
}

static void *plugin_api_psram_malloc(size_t size) {
    if (size == 0) return NULL;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void plugin_api_psram_free(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

static void *plugin_api_app_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) return NULL;
    if (count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *ptr = plugin_api_app_malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

static void plugin_api_app_free(void *ptr) {
    if (!ptr) return;
    plugin_alloc_header_t *header = ((plugin_alloc_header_t *)ptr) - 1;
    if (header->magic != PLUGIN_ALLOC_MAGIC) return;
    if (s_memory_used >= header->size) s_memory_used -= header->size;
    else s_memory_used = 0;
    header->magic = 0;
    free(header);
}

static size_t plugin_api_app_memory_used(void) {
    return s_memory_used;
}

static size_t plugin_api_app_memory_limit(void) {
    return s_memory_limit;
}

static size_t plugin_api_system_free_heap(void) {
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

static size_t plugin_api_system_free_internal_heap(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static uint32_t plugin_api_system_uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static const char *plugin_api_system_firmware_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

static const char *plugin_api_system_target(void) {
    return CONFIG_IDF_TARGET;
}

static bool plugin_api_wifi_start_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    plugin_wifi_run_scan();
    return true;
}

static bool plugin_api_wifi_stop_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    s_plugin_async_scan_active = false;
    // wifi_manager_start_scan() is blocking and already collects results.
    // Calling wifi_manager_stop_scan() here would attempt a second stop,
    // find 0 APs (scan already consumed), and clobber ap_count to 0,
    // destroying the snapshot taken after start_scan.
    return true;
}

static uint16_t plugin_api_wifi_ap_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return 0;
    if (s_plugin_live_scan_active) {
        plugin_wifi_snapshot_scan_results();
    }
    return plugin_ap_count;
}

static bool plugin_api_wifi_scan_get_ap(uint16_t index, ghostesp_wifi_ap_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    if (!out || index >= plugin_ap_count || !plugin_scanned_aps) return false;
    wifi_ap_record_t *ap = &plugin_scanned_aps[index];
    memcpy(out->bssid, ap->bssid, 6);
    memcpy(out->ssid, ap->ssid, 32);
    out->ssid[32] = '\0';
    out->channel = ap->primary;
    out->rssi = ap->rssi;
    out->auth_mode = (uint8_t)ap->authmode;
    return true;
}

static bool plugin_api_wifi_start_scan_async(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    s_plugin_async_scan_active = false;
    plugin_wifi_clear_scan_results();
    if (!s_async_scan_sem) s_async_scan_sem = xSemaphoreCreateBinary();
    if (!s_async_scan_sem) return false;
    xSemaphoreTake(s_async_scan_sem, 0);
    BaseType_t ok = xTaskCreate(plugin_async_scan_start_task, "async_scan", 8192, NULL, 5, NULL);
    if (ok != pdPASS) return false;
    xSemaphoreTake(s_async_scan_sem, portMAX_DELAY);
    if (s_async_scan_result) {
        s_plugin_async_scan_active = true;
        s_plugin_async_scan_start_ms = esp_timer_get_time() / 1000;
    }
    return s_async_scan_result;
}

static bool plugin_api_wifi_scan_check_done(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return true;
    if (!s_plugin_async_scan_active) return true;
    int64_t elapsed_ms = (esp_timer_get_time() / 1000) - s_plugin_async_scan_start_ms;
    if (elapsed_ms < PLUGIN_SCRIPT_SCAN_MIN_MS) return false;
    return ap_scan_check_done();
}

static void plugin_api_wifi_finish_scan(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return;
    s_plugin_async_scan_active = false;
    if (!s_async_finish_sem) s_async_finish_sem = xSemaphoreCreateBinary();
    if (!s_async_finish_sem) {
        ap_scan_finish_async();
        plugin_wifi_snapshot_scan_results();
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%u", (unsigned)plugin_ap_count);
        ghostscript_runtime_dispatch_event(ghostscript_runtime_get_active(), "wifi_scan_done", count_str);
        return;
    }
    xSemaphoreTake(s_async_finish_sem, 0);
    BaseType_t ok = xTaskCreate(plugin_async_scan_finish_task, "async_finish", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ap_scan_finish_async();
        plugin_wifi_snapshot_scan_results();
        char count_str[16];
        snprintf(count_str, sizeof(count_str), "%u", (unsigned)plugin_ap_count);
        ghostscript_runtime_dispatch_event(ghostscript_runtime_get_active(), "wifi_scan_done", count_str);
        return;
    }
    xSemaphoreTake(s_async_finish_sem, portMAX_DELAY);
    char count_str[16];
    snprintf(count_str, sizeof(count_str), "%u", (unsigned)plugin_ap_count);
    ghostscript_runtime_dispatch_event(ghostscript_runtime_get_active(), "wifi_scan_done", count_str);
}

static uint16_t plugin_api_wifi_scan_quick(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return 0;
    plugin_wifi_run_scan();
    return plugin_ap_count;
}

static bool plugin_api_wifi_live_scan_start(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_WIFI)) return false;
    if (s_plugin_live_scan_active) return true;
    plugin_wifi_clear_scan_results();
    wifi_manager_start_live_ap_scan();
    s_plugin_live_scan_active = true;
    return true;
}

static void plugin_api_wifi_live_scan_stop(void) {
    if (!s_plugin_live_scan_active) return;
    s_plugin_live_scan_active = false;
    wifi_manager_stop_monitor_mode();
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "wifi stop after live scan: %s", esp_err_to_name(stop_err));
    }
    (void)ap_manager_restore_after_attack("plugin live scan");
    plugin_wifi_snapshot_scan_results();
}

static bool plugin_api_wifi_live_scan_active(void) {
    return s_plugin_live_scan_active;
}

static bool plugin_api_espnow_start(uint8_t channel) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW)) return false;
    s_plugin_espnow_started = espnow_manager_start(channel);
    return s_plugin_espnow_started;
}

static void plugin_api_espnow_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW)) return;
    espnow_manager_stop();
    s_plugin_espnow_started = false;
}

static bool plugin_api_espnow_is_active(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) && espnow_manager_is_active();
}

static uint8_t plugin_api_espnow_channel(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) ? espnow_manager_channel() : 0;
}

static const char *plugin_api_espnow_name(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) ? espnow_manager_name() : "";
}

static const char *plugin_api_espnow_last_error(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) ? espnow_manager_last_error() : "ESP-NOW permission required";
}

static bool plugin_api_espnow_announce(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) && espnow_manager_announce();
}

static int plugin_api_espnow_peer_count(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) ? espnow_manager_peer_count() : 0;
}

static bool plugin_api_espnow_get_peer(int index, ghostesp_espnow_peer_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) || !out) return false;
    espnow_manager_peer_t peer;
    if (!espnow_manager_get_peer(index, &peer)) return false;
    memcpy(out->mac, peer.mac, sizeof(out->mac));
    out->rssi = peer.rssi;
    out->last_seen_ms = peer.last_seen_ms;
    snprintf(out->name, sizeof(out->name), "%s", peer.name);
    return true;
}

static bool plugin_api_espnow_send(const uint8_t mac[6], const char *text) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) && espnow_manager_send(mac, text);
}

static int plugin_api_espnow_message_count(void) {
    return plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) ? espnow_manager_message_count() : 0;
}

static bool plugin_api_espnow_receive(ghostesp_espnow_message_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_ESPNOW) || !out) return false;
    espnow_manager_message_t message;
    if (!espnow_manager_receive(&message)) return false;
    memcpy(out->sender_mac, message.sender_mac, sizeof(out->sender_mac));
    out->received_at_ms = message.received_at_ms;
    snprintf(out->sender_name, sizeof(out->sender_name), "%s", message.sender_name);
    snprintf(out->text, sizeof(out->text), "%s", message.text);
    return true;
}

static void plugin_api_app_exit(void) {
    if (!s_api_active) return;
    plugin_runner_request_exit();
}

static void plugin_api_ble_detect_start(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return;
    ble_device_detect_start();
}

static void plugin_api_ble_detect_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return;
    ble_device_detect_stop();
}

static bool plugin_api_ble_detect_is_active(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return ble_device_detect_is_active();
}

static int plugin_api_ble_detect_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return 0;
    return ble_device_detect_get_count();
}

static bool plugin_api_ble_detect_get_device(int index, ghostesp_ble_detect_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE) || !out) return false;
    BLEDetectDeviceInfo info;
    if (ble_device_detect_get_device(index, &info) != 0) return false;
    memset(out, 0, sizeof(*out));
    out->type = (uint8_t)info.type;
    memcpy(out->mac, info.mac, sizeof(out->mac));
    out->rssi = info.rssi;
    strncpy(out->name, info.name, sizeof(out->name) - 1);
    strncpy(out->subtype, info.subtype, sizeof(out->subtype) - 1);
    out->tracking = info.tracking;
    return true;
}

static const char *plugin_api_ble_detect_type_name(uint8_t type) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return "Unknown";
    return ble_device_detect_type_to_string((BLEDetectDeviceType)type);
}

static bool plugin_api_ble_detect_start_tracking(int index) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return ble_device_detect_start_tracking(index);
}

static bool plugin_api_ble_detect_start_airtag_spoof(int index) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return ble_device_detect_start_airtag_spoof(index);
}

static void plugin_api_ble_adv_scan_start(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return;
    advertiser_scan_start();
}

static void plugin_api_ble_adv_scan_stop(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return;
    advertiser_scan_stop();
}

static bool plugin_api_ble_adv_scan_active(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return advertiser_scan_is_active();
}

static int plugin_api_ble_adv_scan_count(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return 0;
    return advertiser_scan_get_count();
}

static bool plugin_api_ble_adv_scan_get(int index, ghostesp_ble_adv_info_t *out) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE) || out == NULL) return false;

    AdvertiserDeviceInfo info;
    if (advertiser_scan_get_device(index, &info) != 0) return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->mac, info.mac, sizeof(out->mac));
    out->rssi = info.rssi;
    out->event_type = info.event_type;
    out->seen_count = info.seen_count;
    out->has_flags = info.has_flags;
    out->flags = info.flags;
    out->has_tx_power = info.has_tx_power;
    out->tx_power = info.tx_power;
    out->has_manufacturer_id = info.has_manufacturer_id;
    out->manufacturer_id = info.manufacturer_id;
    out->is_ibeacon = info.is_ibeacon;
    out->ibeacon_major = info.ibeacon_major;
    out->ibeacon_minor = info.ibeacon_minor;
    out->ibeacon_measured_power = info.ibeacon_measured_power;
    strncpy(out->name, info.name, sizeof(out->name) - 1);
    strncpy(out->ibeacon_uuid, info.ibeacon_uuid, sizeof(out->ibeacon_uuid) - 1);
    strncpy(out->adv_type, info.adv_type, sizeof(out->adv_type) - 1);
    strncpy(out->manufacturer, info.manufacturer, sizeof(out->manufacturer) - 1);
    strncpy(out->oui_vendor, info.oui_vendor, sizeof(out->oui_vendor) - 1);
    strncpy(out->services, info.services, sizeof(out->services) - 1);
    strncpy(out->service_data, info.service_data, sizeof(out->service_data) - 1);
    out->has_appearance = info.has_appearance;
    out->appearance = info.appearance;
    return true;
}

static bool plugin_api_ble_adv_scan_track(int index) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return advertiser_scan_start_tracking(index);
}

static void plugin_api_ble_adv_scan_stop_tracking(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return;
    advertiser_scan_stop_tracking();
}

static bool plugin_api_ble_adv_scan_save_to_sd(int index) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_BLE)) return false;
    return advertiser_scan_save_to_sd(index);
}

static bool plugin_api_rgb_set_all(uint8_t red, uint8_t green, uint8_t blue) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_RGB)) return false;
    return rgb_manager_set_color(&rgb_manager, -1, red, green, blue, false) == ESP_OK;
}

const char *plugin_api_current_target(void) {
    return CONFIG_IDF_TARGET;
}

static void *plugin_unsafe_lv_scr_act(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_LVGL)) return NULL;
    return lv_scr_act();
}

static void *plugin_unsafe_display_get_current_view(void) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_LVGL)) return NULL;
    return display_manager_get_current_view();
}

static void *plugin_unsafe_raw_symbol(const char *name) {
    if (!plugin_api_has_permission(PLUGIN_PERMISSION_LVGL)) return NULL;
    if (!name) return NULL;
#if CONFIG_ENABLE_NATIVE_SD_APPS
    return (void *)esp_elf_find_symbol(name);
#else
    return NULL;
#endif
}

extern bool plugin_api_gps_is_available(void);
extern bool plugin_api_gps_has_fix(void);
extern double plugin_api_gps_get_latitude(void);
extern double plugin_api_gps_get_longitude(void);
extern double plugin_api_gps_get_altitude(void);
extern int plugin_api_gps_get_satellites(void);
extern float plugin_api_gps_get_speed(void);
extern float plugin_api_gps_get_heading(void);
extern uint8_t plugin_api_settings_get_theme(void);
extern const char *plugin_api_settings_get_device_name(void);
extern void plugin_api_ui_input_dialog(const char *title, const char *default_text,
                                       ghostesp_input_submit_cb_t on_submit, void *user);

extern uint32_t plugin_api_ui_theme_get_background(void);
extern uint32_t plugin_api_ui_theme_get_surface(void);
extern uint32_t plugin_api_ui_theme_get_surface_alt(void);
extern uint32_t plugin_api_ui_theme_get_text(void);
extern uint32_t plugin_api_ui_theme_get_text_muted(void);
extern uint32_t plugin_api_ui_theme_get_accent(void);
extern bool plugin_api_ui_theme_is_bright(void);
extern void plugin_api_ui_obj_set_bg_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_text_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_border_color(ghostesp_ui_obj_t obj, uint32_t hex_color);
extern void plugin_api_ui_obj_set_border_width(ghostesp_ui_obj_t obj, int32_t width);
extern void plugin_api_ui_obj_set_radius(ghostesp_ui_obj_t obj, int32_t radius);
extern void plugin_api_ui_obj_set_pad(ghostesp_ui_obj_t obj, int32_t left, int32_t right, int32_t top, int32_t bottom);
extern void plugin_api_ui_obj_set_font(ghostesp_ui_obj_t obj, ghostesp_font_size_t size);
extern void plugin_api_ui_obj_set_opa(ghostesp_ui_obj_t obj, uint8_t opa);
extern void plugin_api_ui_obj_set_pos(ghostesp_ui_obj_t obj, int32_t x, int32_t y);
extern void plugin_api_ui_obj_set_size(ghostesp_ui_obj_t obj, int32_t w, int32_t h);
extern void plugin_api_ui_obj_set_width(ghostesp_ui_obj_t obj, int32_t w);
extern void plugin_api_ui_obj_set_height(ghostesp_ui_obj_t obj, int32_t h);
extern void plugin_api_ui_obj_align(ghostesp_ui_obj_t obj, ghostesp_align_t align, int32_t x_ofs, int32_t y_ofs);
extern int32_t plugin_api_ui_obj_get_width(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_height(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_x(ghostesp_ui_obj_t obj);
extern int32_t plugin_api_ui_obj_get_y(ghostesp_ui_obj_t obj);
extern void plugin_api_ui_obj_set_flex_flow(ghostesp_ui_obj_t obj, ghostesp_flex_flow_t flow);
extern void plugin_api_ui_obj_set_flex_align(ghostesp_ui_obj_t obj, ghostesp_flex_align_t main, ghostesp_flex_align_t cross, ghostesp_flex_align_t track);
extern void plugin_api_ui_obj_set_flex_grow(ghostesp_ui_obj_t obj, uint8_t grow);
extern void plugin_api_ui_obj_set_pad_row(ghostesp_ui_obj_t obj, int32_t pad);
extern void plugin_api_ui_obj_set_pad_column(ghostesp_ui_obj_t obj, int32_t pad);
extern int32_t plugin_api_ui_screen_get_width(void);
extern int32_t plugin_api_ui_screen_get_height(void);
extern ghostesp_ui_obj_t plugin_api_ui_touch_bar_create(ghostesp_ui_obj_t parent);
extern ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_back(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
extern ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_up(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
extern ghostesp_ui_obj_t plugin_api_ui_touch_bar_add_down(ghostesp_ui_obj_t bar, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_obj_set_scrollable(ghostesp_ui_obj_t obj, bool scrollable);
extern void plugin_api_ui_obj_scroll_by(ghostesp_ui_obj_t obj, int32_t dx, int32_t dy, bool animated);
extern void plugin_api_ui_obj_set_scrollbar(ghostesp_ui_obj_t obj, bool enabled);
extern void plugin_api_ui_button_set_selected(ghostesp_ui_obj_t button, bool selected);
extern int32_t plugin_api_ui_screen_get_content_width(void);
extern int32_t plugin_api_ui_screen_get_content_height(void);
extern bool plugin_api_ui_screen_is_compact(void);
extern bool plugin_api_ui_has_touchscreen(void);

extern ghostesp_options_t plugin_api_ui_options_create(const char *title);
extern ghostesp_ui_obj_t plugin_api_ui_options_add_item(ghostesp_options_t opts, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern ghostesp_ui_obj_t plugin_api_ui_options_add_back(ghostesp_options_t opts, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_options_set_selected(ghostesp_options_t opts, int index);
extern void plugin_api_ui_options_move_selection(ghostesp_options_t opts, int delta);
extern int plugin_api_ui_options_get_selected(ghostesp_options_t opts);
extern void plugin_api_ui_options_clear(ghostesp_options_t opts);
extern void plugin_api_ui_options_destroy(ghostesp_options_t opts);

extern ghostesp_detail_t plugin_api_ui_detail_create(const char *title);
extern void plugin_api_ui_detail_add_info(ghostesp_detail_t dv, const char *label, const char *value);
extern void plugin_api_ui_detail_add_action(ghostesp_detail_t dv, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_detail_add_header(ghostesp_detail_t dv, const char *text);
extern void plugin_api_ui_detail_add_divider(ghostesp_detail_t dv);
extern ghostesp_ui_obj_t plugin_api_ui_detail_add_back(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_click, void *user);
extern ghostesp_ui_obj_t plugin_api_ui_detail_finish(ghostesp_detail_t dv, ghostesp_ui_button_cb_t on_back, void *user);
extern void plugin_api_ui_detail_set_selected(ghostesp_detail_t dv, int index);
extern void plugin_api_ui_detail_move_selection(ghostesp_detail_t dv, int delta);
extern int plugin_api_ui_detail_get_selected(ghostesp_detail_t dv);
extern int plugin_api_ui_detail_get_count(ghostesp_detail_t dv);
extern bool plugin_api_ui_detail_step_up(ghostesp_detail_t dv);
extern bool plugin_api_ui_detail_step_down(ghostesp_detail_t dv);
extern void plugin_api_ui_detail_activate_selected(ghostesp_detail_t dv);
extern void plugin_api_ui_detail_clear(ghostesp_detail_t dv);
extern void plugin_api_ui_detail_destroy(ghostesp_detail_t dv);

extern ghostesp_popup_t plugin_api_ui_popup_create(int32_t width, int32_t height);
extern void plugin_api_ui_popup_set_title(ghostesp_popup_t p, const char *title);
extern void plugin_api_ui_popup_set_body(ghostesp_popup_t p, const char *body);
extern ghostesp_ui_obj_t plugin_api_ui_popup_add_button(ghostesp_popup_t p, const char *label, ghostesp_ui_button_cb_t on_click, void *user);
extern void plugin_api_ui_popup_show(ghostesp_popup_t p);
extern void plugin_api_ui_popup_hide(ghostesp_popup_t p);
extern void plugin_api_ui_popup_destroy(ghostesp_popup_t p);

extern ghostesp_scan_t plugin_api_ui_scan_status_create(const char *message);
extern void plugin_api_ui_scan_status_update(ghostesp_scan_t ss, const char *message);
extern void plugin_api_ui_scan_status_set_progress(ghostesp_scan_t ss, int current, int total);
extern void plugin_api_ui_scan_status_close(ghostesp_scan_t ss);

extern ghostesp_ui_obj_t plugin_api_ui_canvas_create(ghostesp_ui_obj_t parent, int32_t width, int32_t height);
extern void plugin_api_ui_canvas_draw_rect(ghostesp_ui_obj_t canvas, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t hex_color);
extern void plugin_api_ui_canvas_fill(ghostesp_ui_obj_t canvas, uint32_t hex_color);
extern void plugin_api_ui_canvas_draw_line(ghostesp_ui_obj_t canvas, const ghostesp_point_t *points, int count, uint32_t hex_color, int32_t width);
extern void plugin_api_ui_canvas_draw_arc(ghostesp_ui_obj_t canvas, int32_t cx, int32_t cy, int32_t r, int32_t start_angle, int32_t end_angle, uint32_t hex_color, int32_t width);
extern bool plugin_api_ui_canvas_blit_rgb565(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                             int32_t src_width, int32_t src_height, int32_t src_stride,
                                             int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height);
extern bool plugin_api_ui_canvas_is_rgb565_native_byte_order(void);
extern bool plugin_api_ui_canvas_blit_rgb565_async(ghostesp_ui_obj_t canvas, const uint16_t *pixels,
                                                   int32_t src_width, int32_t src_height, int32_t src_stride,
                                                   int32_t dst_x, int32_t dst_y, int32_t dst_width, int32_t dst_height);
extern bool plugin_api_ui_canvas_blit_async_wait(uint32_t timeout_ms);

extern void plugin_api_ui_anim_slide_in(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms);
extern void plugin_api_ui_anim_slide_out(ghostesp_ui_obj_t obj, int direction, uint32_t duration_ms, ghostesp_anim_done_cb_t on_done, void *user);
extern void plugin_api_ui_anim_pop_in(ghostesp_ui_obj_t obj);
extern void plugin_api_ui_anim_press_pulse(ghostesp_ui_obj_t obj);

extern ghostesp_ui_obj_t plugin_api_ui_arc_create(ghostesp_ui_obj_t parent);
extern void plugin_api_ui_arc_set_value(ghostesp_ui_obj_t arc, int32_t value);
extern void plugin_api_ui_arc_set_range(ghostesp_ui_obj_t arc, int32_t min, int32_t max);
extern void plugin_api_ui_arc_set_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
extern void plugin_api_ui_arc_set_bg_angles(ghostesp_ui_obj_t arc, int32_t start, int32_t end);
extern void plugin_api_ui_arc_set_bg_color(ghostesp_ui_obj_t arc, uint32_t hex_color);
extern void plugin_api_ui_arc_set_indicator_color(ghostesp_ui_obj_t arc, uint32_t hex_color);

extern ghostesp_ui_obj_t plugin_api_ui_line_create(ghostesp_ui_obj_t parent);
extern void plugin_api_ui_line_set_points(ghostesp_ui_obj_t line, const ghostesp_point_t *points, int count);
extern void plugin_api_ui_line_set_color(ghostesp_ui_obj_t line, uint32_t hex_color);
extern void plugin_api_ui_line_set_width(ghostesp_ui_obj_t line, int32_t width);

extern ghostesp_ui_obj_t plugin_api_ui_image_create(ghostesp_ui_obj_t parent);
extern bool plugin_api_ui_image_set_src(ghostesp_ui_obj_t img, const char *app_relative_path);
extern bool plugin_api_ui_image_set_builtin(ghostesp_ui_obj_t img, const char *image_name);

extern ghostesp_ui_timer_t plugin_api_ui_timer_create(ghostesp_ui_timer_cb_t cb, uint32_t interval_ms, void *user);
extern void plugin_api_ui_timer_delete(ghostesp_ui_timer_t timer);
extern void plugin_api_ui_timer_set_interval(ghostesp_ui_timer_t timer, uint32_t interval_ms);

extern ghostesp_paged_menu_t plugin_api_ui_paged_menu_create(int page_size, ghostesp_paged_menu_load_fn load_fn, void *user);
extern void plugin_api_ui_paged_menu_set_callbacks(ghostesp_paged_menu_t pm, ghostesp_paged_menu_select_fn select_fn, ghostesp_paged_menu_nav_fn prev_fn, ghostesp_paged_menu_nav_fn next_fn, void *user);
extern void plugin_api_ui_paged_menu_reset(ghostesp_paged_menu_t pm);
extern void plugin_api_ui_paged_menu_destroy(ghostesp_paged_menu_t pm);
extern bool plugin_api_ui_paged_menu_has_prev(ghostesp_paged_menu_t pm);
extern bool plugin_api_ui_paged_menu_has_next(ghostesp_paged_menu_t pm);

extern bool plugin_api_gpio_set_mode(int pin, uint32_t mode);
extern bool plugin_api_gpio_write(int pin, int level);
extern int plugin_api_gpio_read(int pin);
extern bool plugin_api_gpio_set_pull(int pin, bool pullup, bool pulldown);
extern bool plugin_api_gpio_set_drive_strength(int pin, int strength);
extern bool plugin_api_gpio_set_intr(int pin, int edge, ghostesp_gpio_intr_cb_t cb, void *user);
extern bool plugin_api_gpio_clear_intr(int pin);
extern bool plugin_api_uart_open(int uart_num, int tx_pin, int rx_pin, uint32_t baud);
extern int plugin_api_uart_write(int uart_num, const void *data, size_t len);
extern int plugin_api_uart_read(int uart_num, void *buffer, size_t len, uint32_t timeout_ms);
extern bool plugin_api_uart_close(int uart_num);
extern bool plugin_api_i2c_probe(uint8_t addr, uint32_t timeout_ms);
extern bool plugin_api_i2c_write(uint8_t addr, const void *data, size_t len, uint32_t timeout_ms);
extern int plugin_api_i2c_read(uint8_t addr, void *buffer, size_t len, uint32_t timeout_ms);
extern bool plugin_api_i2c_write_read(uint8_t addr, const void *tx, size_t tx_len, void *rx, size_t rx_len, uint32_t timeout_ms);
extern int plugin_api_spi_open(int host, int sclk, int miso, int mosi, int cs, uint32_t hz, int mode);
extern int plugin_api_spi_transfer(int handle, const void *tx, void *rx, size_t len);
extern bool plugin_api_spi_close(int handle);
extern int plugin_api_adc_read_raw(int channel);
extern int plugin_api_adc_read_mv(int channel);
extern bool plugin_api_pwm_attach(int pin, uint32_t freq_hz, uint8_t resolution_bits);
extern bool plugin_api_pwm_write(int pin, uint32_t duty);
extern bool plugin_api_pwm_detach(int pin);
extern uint64_t plugin_api_system_uptime_us(void);
extern void plugin_api_delay_us(uint32_t us);
extern uint32_t plugin_api_random_u32(void);
extern bool plugin_api_random_bytes(void *buffer, size_t len);
extern bool plugin_api_storage_stat(const char *path, ghostesp_storage_stat_t *out);
extern int64_t plugin_api_storage_size(const char *path);
extern bool plugin_api_storage_rename(const char *from, const char *to);
extern bool plugin_api_storage_mkdir_recursive(const char *path);
extern bool plugin_api_app_storage_stat(const char *path, ghostesp_storage_stat_t *out);
extern int64_t plugin_api_app_storage_size(const char *path);
extern bool plugin_api_app_storage_rename(const char *from, const char *to);
extern bool plugin_api_app_storage_mkdir_recursive(const char *path);
extern int plugin_api_battery_percent(void);
extern int plugin_api_battery_voltage_mv(void);
extern bool plugin_api_battery_is_charging(void);
extern uint8_t plugin_api_display_get_brightness(void);
extern bool plugin_api_display_set_brightness(uint8_t percent);
extern uint32_t plugin_api_input_buttons_state(void);
extern bool plugin_api_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);
extern bool plugin_api_wifi_disconnect(void);
extern bool plugin_api_wifi_is_connected(void);
extern int plugin_api_wifi_rssi(void);
extern bool plugin_api_wifi_ip(char *out, size_t out_len);
extern int plugin_api_http_get(const char *url, void *buffer, size_t buffer_len, uint32_t timeout_ms);
extern int plugin_api_http_post(const char *url, const void *body, size_t body_len, void *buffer, size_t buffer_len, uint32_t timeout_ms);
extern ghostesp_task_t plugin_api_task_create(const char *name, ghostesp_task_fn_t fn, void *user, uint32_t stack_size, int priority);
extern bool plugin_api_task_delete(ghostesp_task_t task);
extern void plugin_api_task_yield(void);
extern int plugin_api_tcp_connect(const char *host, uint16_t port, uint32_t timeout_ms);
extern int plugin_api_socket_send(int sock, const void *data, size_t len);
extern int plugin_api_socket_recv(int sock, void *buffer, size_t len, uint32_t timeout_ms);
extern bool plugin_api_socket_close(int sock);
extern int plugin_api_udp_open(uint16_t local_port);
extern int plugin_api_udp_send_to(int sock, const char *host, uint16_t port, const void *data, size_t len);
extern int plugin_api_udp_recv_from(int sock, void *buffer, size_t len, char *host_out, size_t host_out_len, uint16_t *port_out, uint32_t timeout_ms);
extern int64_t plugin_api_time_unix(void);
extern bool plugin_api_time_set_unix(int64_t unix_time);
extern void plugin_api_system_reboot(void);
extern bool plugin_api_wifi_set_channel(uint8_t channel);
extern uint8_t plugin_api_wifi_get_channel(void);
extern bool plugin_api_wifi_monitor_start(ghostesp_wifi_packet_cb_t cb, void *user);
extern bool plugin_api_wifi_monitor_stop(void);
extern bool plugin_api_wifi_raw_tx(const void *data, size_t len);
extern bool plugin_api_nfc_get_last_uid(uint8_t *uid, size_t *uid_len);
extern bool plugin_api_nfc_write_file(const char *app_relative_path);
extern bool plugin_api_ir_send_raw(uint32_t carrier_hz, const uint16_t *durations, size_t count);
extern bool plugin_api_ir_receive_start(void);
extern bool plugin_api_ir_receive_stop(void);
extern int plugin_api_ir_receive_read(uint16_t *durations, size_t max_count);
extern bool plugin_api_subghz_transmit_raw(uint32_t frequency_hz, const uint16_t *durations, size_t count);
extern bool plugin_api_ble_adv_start(const uint8_t *data, size_t len);
extern bool plugin_api_ble_adv_stop(void);
extern bool plugin_api_ble_gatt_connect(const uint8_t mac[6]);
extern bool plugin_api_ble_gatt_disconnect(void);
extern int plugin_api_ble_gatt_read(uint16_t service_uuid, uint16_t char_uuid, void *buffer, size_t buffer_len);
extern bool plugin_api_ble_gatt_write(uint16_t service_uuid, uint16_t char_uuid, const void *data, size_t len);
extern bool plugin_api_ble_gatt_server_start(const char *name);
extern bool plugin_api_ble_gatt_server_stop(void);
extern bool plugin_api_nrf24_start(bool stream_to_peer);
extern void plugin_api_nrf24_stop(void);
extern bool plugin_api_nrf24_is_running(void);
extern bool plugin_api_nrf24_is_paused(void);
extern void plugin_api_nrf24_set_paused(bool paused);
extern bool plugin_api_wifi_deauth(const uint8_t bssid[6], const uint8_t sta[6], uint8_t reason);
extern bool plugin_api_wifi_send_beacon(const char *ssid, const uint8_t bssid[6], uint8_t channel);
extern bool plugin_api_wifi_pcap_start(const char *app_relative_path);
extern bool plugin_api_wifi_pcap_stop(void);
extern bool plugin_api_ethernet_is_connected(void);
extern bool plugin_api_ethernet_ip(char *out, size_t out_len);
extern int plugin_api_camera_capture_jpeg(void *buffer, size_t buffer_len);
extern bool plugin_api_camera_capture_jpeg_file(const char *app_relative_path);
extern bool plugin_api_usb_hid_keyboard_send(const char *text);
extern bool plugin_api_usb_hid_mouse_move(int dx, int dy, uint8_t buttons);
extern bool plugin_api_audio_mic_is_available(void);
extern int plugin_api_audio_mic_read(int32_t *samples, size_t max_samples, uint32_t timeout_ms);
extern float plugin_api_audio_mic_rms(const int32_t *samples, size_t count);
extern bool plugin_api_zigbee_capture_start(uint8_t channel);
extern bool plugin_api_zigbee_capture_stop(void);
extern bool plugin_api_zigbee_is_capturing(void);
extern int plugin_api_zigbee_device_count(void);
extern bool plugin_api_settings_get_u8(const char *key, uint8_t *out);
extern bool plugin_api_settings_set_u8(const char *key, uint8_t value);
extern bool plugin_api_settings_get_string(const char *key, char *out, size_t out_len);
extern bool plugin_api_settings_set_string(const char *key, const char *value);
extern bool plugin_api_settings_save(void);
extern bool plugin_api_nvs_get_u32(const char *key, uint32_t *out);
extern bool plugin_api_nvs_set_u32(const char *key, uint32_t value);
extern int plugin_api_nvs_get_blob(const char *key, void *buffer, size_t buffer_len);
extern bool plugin_api_nvs_set_blob(const char *key, const void *data, size_t len);
extern bool plugin_api_nvs_delete(const char *key);
extern ghostesp_event_sub_t plugin_api_event_subscribe(const char *topic, ghostesp_event_cb_t cb, void *user);
extern bool plugin_api_event_unsubscribe(ghostesp_event_sub_t sub);
extern bool plugin_api_event_publish(const char *topic, const void *data, size_t len);
extern bool plugin_api_parser_nfc_summary(const char *app_relative_path, char *out, size_t out_len);
extern bool plugin_api_parser_ir_summary(const char *app_relative_path, char *out, size_t out_len);
extern bool plugin_api_parser_subghz_summary(const char *app_relative_path, char *out, size_t out_len);
extern void plugin_api_lowlevel_release(void);

static void plugin_api_dead_log(const char *msg) {
    (void)msg;
    ESP_LOGW(TAG, "plugin API call after release - app was not unloaded cleanly");
}

static const ghostesp_api_t s_dead_api = {
    .api_version = 0,
    .struct_size = 0,
    .log = plugin_api_dead_log,
};

static ghostesp_api_t s_api_live_backup;
static bool s_api_swapped = false;

static ghostesp_api_t s_api = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = GHOSTESP_API_STRUCT_SIZE_V1,
    .target = CONFIG_IDF_TARGET,
    .log = plugin_api_log,
    .ui_set_title = plugin_api_ui_set_title,
    .ui_print = plugin_api_ui_print,
    .ui_clear = plugin_api_ui_clear,
    .toast = plugin_api_toast,
    .ui_show_text = plugin_api_ui_show_text,
    .command_exec = plugin_api_command_exec,
    .app_exit = plugin_api_app_exit,
    .storage_exists = plugin_api_storage_exists,
    .storage_read = plugin_api_storage_read,
    .storage_write = plugin_api_storage_write,
    .storage_append = plugin_api_storage_append,
    .storage_delete = plugin_api_storage_delete,
    .storage_mkdir = plugin_api_storage_mkdir,
    .storage_list = plugin_api_storage_list,
    .malloc = plugin_api_app_malloc,
    .free = plugin_api_app_free,
    .app_malloc = plugin_api_app_malloc,
    .app_calloc = plugin_api_app_calloc,
    .app_free = plugin_api_app_free,
    .app_memory_used = plugin_api_app_memory_used,
    .app_memory_limit = plugin_api_app_memory_limit,
    .delay_ms = plugin_api_delay_ms,
    .system_free_heap = plugin_api_system_free_heap,
    .system_free_internal_heap = plugin_api_system_free_internal_heap,
    .system_uptime_ms = plugin_api_system_uptime_ms,
    .system_firmware_version = plugin_api_system_firmware_version,
    .system_target = plugin_api_system_target,
    .wifi_start_scan = plugin_api_wifi_start_scan,
    .wifi_stop_scan = plugin_api_wifi_stop_scan,
    .wifi_ap_count = plugin_api_wifi_ap_count,
    .wifi_scan_get_ap = plugin_api_wifi_scan_get_ap,
    .wifi_start_scan_async = plugin_api_wifi_start_scan_async,
    .wifi_scan_check_done = plugin_api_wifi_scan_check_done,
    .wifi_finish_scan = plugin_api_wifi_finish_scan,
    .rgb_set_all = plugin_api_rgb_set_all,
    .lv_scr_act = plugin_unsafe_lv_scr_act,
    .display_get_current_view = plugin_unsafe_display_get_current_view,
    .raw_symbol = plugin_unsafe_raw_symbol,
    .app_id = plugin_api_app_id,
    .app_data_path = plugin_api_app_data_path,
    .app_storage_exists = plugin_api_app_storage_exists,
    .app_storage_read = plugin_api_app_storage_read,
    .app_storage_write = plugin_api_app_storage_write,
    .app_storage_append = plugin_api_app_storage_append,
    .app_storage_delete = plugin_api_app_storage_delete,
    .app_storage_mkdir = plugin_api_app_storage_mkdir,
    .app_storage_list = plugin_api_app_storage_list,
    .badusb_run_script = plugin_api_badusb_run_script,
    .badusb_stop = plugin_api_badusb_stop,
    .ir_send_file = plugin_api_ir_send_file,
    .ir_stop = plugin_api_ir_stop,
    .nfc_is_available = plugin_api_nfc_is_available,
    .nfc_read_start = plugin_api_nfc_read_start,
    .nfc_stop = plugin_api_nfc_stop,
    .ble_start_scan = plugin_api_ble_start_scan,
    .ble_stop_scan = plugin_api_ble_stop_scan,
    .ble_device_count = plugin_api_ble_device_count,
    .ble_get_device = plugin_api_ble_get_device,
    .ble_detect_start = plugin_api_ble_detect_start,
    .ble_detect_stop = plugin_api_ble_detect_stop,
    .ble_detect_is_active = plugin_api_ble_detect_is_active,
    .ble_detect_count = plugin_api_ble_detect_count,
    .ble_detect_get_device = plugin_api_ble_detect_get_device,
    .ble_detect_type_name = plugin_api_ble_detect_type_name,
    .ble_detect_start_tracking = plugin_api_ble_detect_start_tracking,
    .ble_detect_start_airtag_spoof = plugin_api_ble_detect_start_airtag_spoof,
    .subghz_is_available = plugin_api_subghz_is_available,
    .subghz_load_snapshot = plugin_api_subghz_load_snapshot,
    .subghz_transmit_loaded = plugin_api_subghz_transmit_loaded,
    .subghz_stop = plugin_api_subghz_stop,
    .ui_screen_create = plugin_api_ui_screen_create,
    .ui_card_create = plugin_api_ui_card_create,
    .ui_label_create = plugin_api_ui_label_create,
    .ui_button_create = plugin_api_ui_button_create,
    .ui_label_set_text = plugin_api_ui_label_set_text,
    .ui_button_set_text = plugin_api_ui_button_set_text,
    .ui_obj_set_visible = plugin_api_ui_obj_set_visible,
    .ui_obj_delete = plugin_api_ui_obj_delete,
    .ui_set_status = plugin_api_ui_set_status,
    .ui_show_popup = plugin_api_ui_show_popup,
    .ui_theme_get_background = plugin_api_ui_theme_get_background,
    .ui_theme_get_surface = plugin_api_ui_theme_get_surface,
    .ui_theme_get_surface_alt = plugin_api_ui_theme_get_surface_alt,
    .ui_theme_get_text = plugin_api_ui_theme_get_text,
    .ui_theme_get_text_muted = plugin_api_ui_theme_get_text_muted,
    .ui_theme_get_accent = plugin_api_ui_theme_get_accent,
    .ui_theme_is_bright = plugin_api_ui_theme_is_bright,
    .ui_obj_set_bg_color = plugin_api_ui_obj_set_bg_color,
    .ui_obj_set_text_color = plugin_api_ui_obj_set_text_color,
    .ui_obj_set_border_color = plugin_api_ui_obj_set_border_color,
    .ui_obj_set_border_width = plugin_api_ui_obj_set_border_width,
    .ui_obj_set_radius = plugin_api_ui_obj_set_radius,
    .ui_obj_set_pad = plugin_api_ui_obj_set_pad,
    .ui_obj_set_font = plugin_api_ui_obj_set_font,
    .ui_obj_set_opa = plugin_api_ui_obj_set_opa,
    .ui_obj_set_pos = plugin_api_ui_obj_set_pos,
    .ui_obj_set_size = plugin_api_ui_obj_set_size,
    .ui_obj_set_width = plugin_api_ui_obj_set_width,
    .ui_obj_set_height = plugin_api_ui_obj_set_height,
    .ui_obj_align = plugin_api_ui_obj_align,
    .ui_obj_get_width = plugin_api_ui_obj_get_width,
    .ui_obj_get_height = plugin_api_ui_obj_get_height,
    .ui_obj_get_x = plugin_api_ui_obj_get_x,
    .ui_obj_get_y = plugin_api_ui_obj_get_y,
    .ui_obj_set_flex_flow = plugin_api_ui_obj_set_flex_flow,
    .ui_obj_set_flex_align = plugin_api_ui_obj_set_flex_align,
    .ui_obj_set_flex_grow = plugin_api_ui_obj_set_flex_grow,
    .ui_obj_set_pad_row = plugin_api_ui_obj_set_pad_row,
    .ui_obj_set_pad_column = plugin_api_ui_obj_set_pad_column,
    .ui_timer_create = plugin_api_ui_timer_create,
    .ui_timer_delete = plugin_api_ui_timer_delete,
    .ui_timer_set_interval = plugin_api_ui_timer_set_interval,
    .ui_options_create = plugin_api_ui_options_create,
    .ui_options_add_item = plugin_api_ui_options_add_item,
    .ui_options_add_back = plugin_api_ui_options_add_back,
    .ui_options_set_selected = plugin_api_ui_options_set_selected,
    .ui_options_move_selection = plugin_api_ui_options_move_selection,
    .ui_options_get_selected = plugin_api_ui_options_get_selected,
    .ui_options_clear = plugin_api_ui_options_clear,
    .ui_options_destroy = plugin_api_ui_options_destroy,
    .ui_detail_create = plugin_api_ui_detail_create,
    .ui_detail_add_info = plugin_api_ui_detail_add_info,
    .ui_detail_add_action = plugin_api_ui_detail_add_action,
    .ui_detail_add_header = plugin_api_ui_detail_add_header,
    .ui_detail_add_divider = plugin_api_ui_detail_add_divider,
    .ui_detail_add_back = plugin_api_ui_detail_add_back,
    .ui_detail_set_selected = plugin_api_ui_detail_set_selected,
    .ui_detail_move_selection = plugin_api_ui_detail_move_selection,
    .ui_detail_get_selected = plugin_api_ui_detail_get_selected,
    .ui_detail_get_count = plugin_api_ui_detail_get_count,
    .ui_detail_step_up = plugin_api_ui_detail_step_up,
    .ui_detail_step_down = plugin_api_ui_detail_step_down,
    .ui_detail_activate_selected = plugin_api_ui_detail_activate_selected,
    .ui_detail_clear = plugin_api_ui_detail_clear,
    .ui_detail_destroy = plugin_api_ui_detail_destroy,
    .ui_popup_create = plugin_api_ui_popup_create,
    .ui_popup_set_title = plugin_api_ui_popup_set_title,
    .ui_popup_set_body = plugin_api_ui_popup_set_body,
    .ui_popup_add_button = plugin_api_ui_popup_add_button,
    .ui_popup_show = plugin_api_ui_popup_show,
    .ui_popup_hide = plugin_api_ui_popup_hide,
    .ui_popup_destroy = plugin_api_ui_popup_destroy,
    .ui_scan_status_create = plugin_api_ui_scan_status_create,
    .ui_scan_status_update = plugin_api_ui_scan_status_update,
    .ui_scan_status_set_progress = plugin_api_ui_scan_status_set_progress,
    .ui_scan_status_close = plugin_api_ui_scan_status_close,
    .ui_canvas_create = plugin_api_ui_canvas_create,
    .ui_canvas_draw_rect = plugin_api_ui_canvas_draw_rect,
    .ui_canvas_fill = plugin_api_ui_canvas_fill,
    .ui_canvas_draw_line = plugin_api_ui_canvas_draw_line,
    .ui_canvas_draw_arc = plugin_api_ui_canvas_draw_arc,
    .ui_anim_slide_in = plugin_api_ui_anim_slide_in,
    .ui_anim_slide_out = plugin_api_ui_anim_slide_out,
    .ui_anim_pop_in = plugin_api_ui_anim_pop_in,
    .ui_anim_press_pulse = plugin_api_ui_anim_press_pulse,
    .ui_arc_create = plugin_api_ui_arc_create,
    .ui_arc_set_value = plugin_api_ui_arc_set_value,
    .ui_arc_set_range = plugin_api_ui_arc_set_range,
    .ui_arc_set_angles = plugin_api_ui_arc_set_angles,
    .ui_arc_set_bg_angles = plugin_api_ui_arc_set_bg_angles,
    .ui_arc_set_bg_color = plugin_api_ui_arc_set_bg_color,
    .ui_arc_set_indicator_color = plugin_api_ui_arc_set_indicator_color,
    .ui_line_create = plugin_api_ui_line_create,
    .ui_line_set_points = plugin_api_ui_line_set_points,
    .ui_line_set_color = plugin_api_ui_line_set_color,
    .ui_line_set_width = plugin_api_ui_line_set_width,
    .ui_image_create = plugin_api_ui_image_create,
    .ui_image_set_src = plugin_api_ui_image_set_src,
    .ui_paged_menu_create = plugin_api_ui_paged_menu_create,
    .ui_paged_menu_set_callbacks = plugin_api_ui_paged_menu_set_callbacks,
    .ui_paged_menu_reset = plugin_api_ui_paged_menu_reset,
    .ui_paged_menu_destroy = plugin_api_ui_paged_menu_destroy,
    .ui_paged_menu_has_prev = plugin_api_ui_paged_menu_has_prev,
    .ui_paged_menu_has_next = plugin_api_ui_paged_menu_has_next,
    .gps_is_available = plugin_api_gps_is_available,
    .gps_has_fix = plugin_api_gps_has_fix,
    .gps_get_latitude = plugin_api_gps_get_latitude,
    .gps_get_longitude = plugin_api_gps_get_longitude,
    .gps_get_altitude = plugin_api_gps_get_altitude,
    .gps_get_satellites = plugin_api_gps_get_satellites,
    .gps_get_speed = plugin_api_gps_get_speed,
    .gps_get_heading = plugin_api_gps_get_heading,
    .settings_get_theme = plugin_api_settings_get_theme,
    .settings_get_device_name = plugin_api_settings_get_device_name,
    .ui_input_dialog = plugin_api_ui_input_dialog,
    .ui_screen_get_width = plugin_api_ui_screen_get_width,
    .ui_screen_get_height = plugin_api_ui_screen_get_height,
    .gpio_set_mode = plugin_api_gpio_set_mode,
    .gpio_write = plugin_api_gpio_write,
    .gpio_read = plugin_api_gpio_read,
    .gpio_set_pull = plugin_api_gpio_set_pull,
    .gpio_set_drive_strength = plugin_api_gpio_set_drive_strength,
    .gpio_set_intr = plugin_api_gpio_set_intr,
    .gpio_clear_intr = plugin_api_gpio_clear_intr,
    .uart_open = plugin_api_uart_open,
    .uart_write = plugin_api_uart_write,
    .uart_read = plugin_api_uart_read,
    .uart_close = plugin_api_uart_close,
    .i2c_probe = plugin_api_i2c_probe,
    .i2c_write = plugin_api_i2c_write,
    .i2c_read = plugin_api_i2c_read,
    .i2c_write_read = plugin_api_i2c_write_read,
    .spi_open = plugin_api_spi_open,
    .spi_transfer = plugin_api_spi_transfer,
    .spi_close = plugin_api_spi_close,
    .adc_read_raw = plugin_api_adc_read_raw,
    .adc_read_mv = plugin_api_adc_read_mv,
    .pwm_attach = plugin_api_pwm_attach,
    .pwm_write = plugin_api_pwm_write,
    .pwm_detach = plugin_api_pwm_detach,
    .system_uptime_us = plugin_api_system_uptime_us,
    .delay_us = plugin_api_delay_us,
    .random_u32 = plugin_api_random_u32,
    .random_bytes = plugin_api_random_bytes,
    .storage_stat = plugin_api_storage_stat,
    .storage_size = plugin_api_storage_size,
    .storage_rename = plugin_api_storage_rename,
    .storage_mkdir_recursive = plugin_api_storage_mkdir_recursive,
    .app_storage_stat = plugin_api_app_storage_stat,
    .app_storage_size = plugin_api_app_storage_size,
    .app_storage_rename = plugin_api_app_storage_rename,
    .app_storage_mkdir_recursive = plugin_api_app_storage_mkdir_recursive,
    .battery_percent = plugin_api_battery_percent,
    .battery_voltage_mv = plugin_api_battery_voltage_mv,
    .battery_is_charging = plugin_api_battery_is_charging,
    .display_get_brightness = plugin_api_display_get_brightness,
    .display_set_brightness = plugin_api_display_set_brightness,
    .input_buttons_state = plugin_api_input_buttons_state,
    .wifi_connect = plugin_api_wifi_connect,
    .wifi_disconnect = plugin_api_wifi_disconnect,
    .wifi_is_connected = plugin_api_wifi_is_connected,
    .wifi_rssi = plugin_api_wifi_rssi,
    .wifi_ip = plugin_api_wifi_ip,
    .http_get = plugin_api_http_get,
    .http_post = plugin_api_http_post,
    .task_create = plugin_api_task_create,
    .task_delete = plugin_api_task_delete,
    .task_yield = plugin_api_task_yield,
    .tcp_connect = plugin_api_tcp_connect,
    .socket_send = plugin_api_socket_send,
    .socket_recv = plugin_api_socket_recv,
    .socket_close = plugin_api_socket_close,
    .udp_open = plugin_api_udp_open,
    .udp_send_to = plugin_api_udp_send_to,
    .udp_recv_from = plugin_api_udp_recv_from,
    .time_unix = plugin_api_time_unix,
    .time_set_unix = plugin_api_time_set_unix,
    .system_reboot = plugin_api_system_reboot,
    .wifi_set_channel = plugin_api_wifi_set_channel,
    .wifi_get_channel = plugin_api_wifi_get_channel,
    .wifi_monitor_start = plugin_api_wifi_monitor_start,
    .wifi_monitor_stop = plugin_api_wifi_monitor_stop,
    .wifi_raw_tx = plugin_api_wifi_raw_tx,
    .nfc_get_last_uid = plugin_api_nfc_get_last_uid,
    .nfc_write_file = plugin_api_nfc_write_file,
    .ir_send_raw = plugin_api_ir_send_raw,
    .ir_receive_start = plugin_api_ir_receive_start,
    .ir_receive_stop = plugin_api_ir_receive_stop,
    .ir_receive_read = plugin_api_ir_receive_read,
    .subghz_transmit_raw = plugin_api_subghz_transmit_raw,
    .ble_adv_start = plugin_api_ble_adv_start,
    .ble_adv_stop = plugin_api_ble_adv_stop,
    .ble_gatt_connect = plugin_api_ble_gatt_connect,
    .ble_gatt_disconnect = plugin_api_ble_gatt_disconnect,
    .ble_gatt_read = plugin_api_ble_gatt_read,
    .ble_gatt_write = plugin_api_ble_gatt_write,
    .ble_gatt_server_start = plugin_api_ble_gatt_server_start,
    .ble_gatt_server_stop = plugin_api_ble_gatt_server_stop,
    .nrf24_start = plugin_api_nrf24_start,
    .nrf24_stop = plugin_api_nrf24_stop,
    .nrf24_is_running = plugin_api_nrf24_is_running,
    .nrf24_is_paused = plugin_api_nrf24_is_paused,
    .nrf24_set_paused = plugin_api_nrf24_set_paused,
    .wifi_deauth = plugin_api_wifi_deauth,
    .wifi_send_beacon = plugin_api_wifi_send_beacon,
    .wifi_pcap_start = plugin_api_wifi_pcap_start,
    .wifi_pcap_stop = plugin_api_wifi_pcap_stop,
    .ethernet_is_connected = plugin_api_ethernet_is_connected,
    .ethernet_ip = plugin_api_ethernet_ip,
    .camera_capture_jpeg = plugin_api_camera_capture_jpeg,
    .camera_capture_jpeg_file = plugin_api_camera_capture_jpeg_file,
    .usb_hid_keyboard_send = plugin_api_usb_hid_keyboard_send,
    .usb_hid_mouse_move = plugin_api_usb_hid_mouse_move,
    .audio_mic_is_available = plugin_api_audio_mic_is_available,
    .audio_mic_read = plugin_api_audio_mic_read,
    .audio_mic_rms = plugin_api_audio_mic_rms,
    .zigbee_capture_start = plugin_api_zigbee_capture_start,
    .zigbee_capture_stop = plugin_api_zigbee_capture_stop,
    .zigbee_is_capturing = plugin_api_zigbee_is_capturing,
    .zigbee_device_count = plugin_api_zigbee_device_count,
    .settings_get_u8 = plugin_api_settings_get_u8,
    .settings_set_u8 = plugin_api_settings_set_u8,
    .settings_get_string = plugin_api_settings_get_string,
    .settings_set_string = plugin_api_settings_set_string,
    .settings_save = plugin_api_settings_save,
    .nvs_get_u32 = plugin_api_nvs_get_u32,
    .nvs_set_u32 = plugin_api_nvs_set_u32,
    .nvs_get_blob = plugin_api_nvs_get_blob,
    .nvs_set_blob = plugin_api_nvs_set_blob,
    .nvs_delete = plugin_api_nvs_delete,
    .event_subscribe = plugin_api_event_subscribe,
    .event_unsubscribe = plugin_api_event_unsubscribe,
    .event_publish = plugin_api_event_publish,
    .parser_nfc_summary = plugin_api_parser_nfc_summary,
    .parser_ir_summary = plugin_api_parser_ir_summary,
    .parser_subghz_summary = plugin_api_parser_subghz_summary,
    .ui_detail_finish = plugin_api_ui_detail_finish,
    .ui_touch_bar_create = plugin_api_ui_touch_bar_create,
    .ui_touch_bar_add_back = plugin_api_ui_touch_bar_add_back,
    .ui_touch_bar_add_up = plugin_api_ui_touch_bar_add_up,
    .ui_touch_bar_add_down = plugin_api_ui_touch_bar_add_down,
    .ui_obj_set_scrollable = plugin_api_ui_obj_set_scrollable,
    .ui_obj_scroll_by = plugin_api_ui_obj_scroll_by,
    .ui_obj_set_scrollbar = plugin_api_ui_obj_set_scrollbar,
    .ui_button_set_selected = plugin_api_ui_button_set_selected,
    .ui_screen_get_content_width = plugin_api_ui_screen_get_content_width,
    .ui_screen_get_content_height = plugin_api_ui_screen_get_content_height,
    .ui_screen_is_compact = plugin_api_ui_screen_is_compact,
    .ui_has_touchscreen = plugin_api_ui_has_touchscreen,
    .wifi_live_scan_start = plugin_api_wifi_live_scan_start,
    .wifi_live_scan_stop = plugin_api_wifi_live_scan_stop,
    .wifi_live_scan_active = plugin_api_wifi_live_scan_active,
    .has_permission = plugin_api_has_permission_name,
    .has_feature = plugin_api_has_feature,
    .subghz_transmit_file = plugin_api_subghz_transmit_file,
    .ble_adv_scan_start = plugin_api_ble_adv_scan_start,
    .ble_adv_scan_stop = plugin_api_ble_adv_scan_stop,
    .ble_adv_scan_active = plugin_api_ble_adv_scan_active,
    .ble_adv_scan_count = plugin_api_ble_adv_scan_count,
    .ble_adv_scan_get = plugin_api_ble_adv_scan_get,
    .ble_adv_scan_track = plugin_api_ble_adv_scan_track,
    .ble_adv_scan_stop_tracking = plugin_api_ble_adv_scan_stop_tracking,
    .ble_adv_scan_save_to_sd = plugin_api_ble_adv_scan_save_to_sd,
    .nfc_t2_scan_start = plugin_api_nfc_t2_scan_start,
    .nfc_t2_scan_stop = plugin_api_nfc_t2_scan_stop,
    .nfc_t2_scan_active = plugin_api_nfc_t2_scan_active,
    .nfc_t2_read = plugin_api_nfc_t2_read,
    .nfc_t2_write_ndef = plugin_api_nfc_t2_write_ndef,
    .ui_image_set_builtin = plugin_api_ui_image_set_builtin,
    .ui_canvas_blit_rgb565 = plugin_api_ui_canvas_blit_rgb565,
    .ui_canvas_is_rgb565_native_byte_order = plugin_api_ui_canvas_is_rgb565_native_byte_order,
    .storage_read_at = plugin_api_storage_read_at,
    .app_storage_read_at = plugin_api_app_storage_read_at,
    .asset_storage_read_at = plugin_api_asset_storage_read_at,
    .asset_path = plugin_api_asset_path,
    .asset_session_begin = plugin_api_asset_session_begin,
    .asset_session_end = plugin_api_asset_session_end,
    .request_exit = plugin_api_request_exit,
    .input_snapshot = plugin_api_input_snapshot,
    .asset_storage_size = plugin_api_asset_storage_size,
    .ui_canvas_blit_rgb565_async = plugin_api_ui_canvas_blit_rgb565_async,
    .ui_canvas_blit_async_wait = plugin_api_ui_canvas_blit_async_wait,
    .espnow_start = plugin_api_espnow_start,
    .espnow_stop = plugin_api_espnow_stop,
    .espnow_is_active = plugin_api_espnow_is_active,
    .espnow_channel = plugin_api_espnow_channel,
    .espnow_name = plugin_api_espnow_name,
    .espnow_last_error = plugin_api_espnow_last_error,
    .espnow_announce = plugin_api_espnow_announce,
    .espnow_peer_count = plugin_api_espnow_peer_count,
    .espnow_get_peer = plugin_api_espnow_get_peer,
    .espnow_send = plugin_api_espnow_send,
    .espnow_message_count = plugin_api_espnow_message_count,
    .espnow_receive = plugin_api_espnow_receive,
    .psram_malloc = plugin_api_psram_malloc,
    .psram_free = plugin_api_psram_free,
};

const ghostesp_api_t *plugin_api_get(const char *app_id,
                                     const char *base_path,
                                     uint64_t permissions,
                                     size_t memory_limit,
                                     bool allow_absolute_storage) {
    plugin_api_lock();
    if (s_api_active) {
        plugin_api_unlock();
        ESP_LOGE(TAG, "plugin_api_get called while another app is active");
        return NULL;
    }
    s_permissions = permissions;
    s_allow_absolute_storage = allow_absolute_storage;
    s_memory_limit = memory_limit;
    s_memory_used = 0;
    s_app_id[0] = '\0';
    free(s_app_data_path);
    free(s_app_base_path);
    s_app_data_path = NULL;
    s_app_base_path = NULL;
    s_asset_session_active = false;
    s_asset_session_display_suspended = false;
    s_plugin_ble_started = false;
    s_plugin_espnow_started = false;
    portENTER_CRITICAL(&s_input_snapshot_mux);
    s_input_held = 0;
    s_input_pressed = 0;
    s_input_released = 0;
    s_input_last_change_ms = 0;
    portEXIT_CRITICAL(&s_input_snapshot_mux);
    if (app_id) {
        for (const char *p = app_id; *p; ++p) {
            if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
                plugin_api_unlock();
                ESP_LOGE(TAG, "plugin_api_get: invalid app_id");
                return NULL;
            }
        }
        strncpy(s_app_id, app_id, sizeof(s_app_id) - 1);
        s_app_id[sizeof(s_app_id) - 1] = '\0';
        char app_data_path[PLUGIN_APP_PATH_MAX];
        snprintf(app_data_path, sizeof(app_data_path), "/mnt/ghostesp/appdata/%s", s_app_id);
        s_app_data_path = strdup(app_data_path);
        s_app_base_path = strdup(base_path ? base_path : "");
        if (!s_app_data_path || !s_app_base_path) {
            free(s_app_data_path);
            free(s_app_base_path);
            s_app_data_path = NULL;
            s_app_base_path = NULL;
            s_app_id[0] = '\0';
            plugin_api_unlock();
            ESP_LOGE(TAG, "plugin_api_get: out of memory for app paths");
            return NULL;
        }
        sd_card_create_directory("/mnt/ghostesp/appdata");
        sd_card_create_directory(s_app_data_path);
        plugin_api_direct_index_load(s_app_base_path);
    }

    if (s_api_swapped) {
        memcpy(&s_api, &s_api_live_backup, sizeof(s_api));
        s_api_swapped = false;
    }
    s_api.flags = GHOSTESP_APP_FLAG_PERMISSIONS_ENFORCED;
    if (allow_absolute_storage) s_api.flags |= GHOSTESP_APP_FLAG_ABSOLUTE_STORAGE_ALLOWED;
    s_api_active = true;
    plugin_api_unlock();
    return &s_api;
}

void plugin_api_set_ui_hooks(void (*set_title)(const char *title),
                             void (*print)(const char *text),
                             void (*clear)(void),
                             void (*toast)(const char *message)) {
    s_ui_set_title = set_title;
    s_ui_print = print;
    s_ui_clear = clear;
    s_ui_toast = toast;
}

bool plugin_api_is_active(void) {
    return s_api_active;
}

void plugin_api_release(void) {
    plugin_api_lock();

    plugin_api_asset_session_end();
    /* App storage may leave the shared-SPI quiesce gate closed while an exit
     * callback is already running on LVGL. Clear it before the next UI loop. */
    display_manager_resume_lvgl_task();

    /* Safety: stop hardware that a misbehaving script may have left running. */
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (s_plugin_ble_started && ble_is_initialized()) {
        ble_stop();
    }
    s_plugin_ble_started = false;
#endif
    if (s_plugin_live_scan_active) {
        s_plugin_live_scan_active = false;
        wifi_manager_stop_monitor_mode();
        esp_wifi_stop();
        (void)ap_manager_restore_after_attack("plugin cleanup");
    }
    if (s_plugin_espnow_started) {
        espnow_manager_stop();
        s_plugin_espnow_started = false;
    }
    s_plugin_async_scan_active = false;

    /* Async blits own a packed snapshot of the app pixels, so teardown must
       not block the LVGL task waiting for a callback queued on that task. */
    plugin_api_lowlevel_release();
    s_api_active = false;
    s_permissions = 0;
    s_allow_absolute_storage = false;
    s_memory_limit = 0;
    s_memory_used = 0;
    s_app_id[0] = '\0';
    free(s_app_data_path);
    free(s_app_base_path);
    free(s_subghz_loaded_path);
    s_app_data_path = NULL;
    s_app_base_path = NULL;
    s_subghz_loaded_path = NULL;
    plugin_api_direct_index_reset();
    /* Backstop: normally closed by asset_session_end(), but an app that
       unloads mid-session (crash, forced exit) shouldn't leave an open
       file handle behind for the next app that loads. */
    plugin_api_read_cache_close();
    portENTER_CRITICAL(&s_input_snapshot_mux);
    s_input_held = 0;
    s_input_pressed = 0;
    s_input_released = 0;
    s_input_last_change_ms = 0;
    portEXIT_CRITICAL(&s_input_snapshot_mux);

    if (!s_api_swapped) {
        memcpy(&s_api_live_backup, &s_api, sizeof(s_api));
        memcpy(&s_api, &s_dead_api, sizeof(s_dead_api));
        s_api_swapped = true;
    }

    plugin_api_unlock();
    plugin_api_canvas_cleanup_timers();
}
