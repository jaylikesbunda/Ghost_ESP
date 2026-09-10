#include "vendor/GPS/gps_logger.h"
#include "core/callbacks.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "core/glog.h"
#include "managers/gps_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/wigle_manager.h"
#include "gui/toast.h"
#include "managers/views/terminal_screen.h"
#include "sys/time.h"
#include "vendor/GPS/MicroNMEA.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <esp_heap_caps.h>
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "core/ghostesp_version.h"

static const char *GPS_TAG = "GPS";
static const char *CSV_TAG = "CSV";
static const char *CSV_HEADER = "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,RCOIs,MfgrId,Type\n";

static bool is_valid_date(const gps_date_t *date);

static void resolve_timestamp_for_file(gps_date_t *out_date, gps_time_t *out_time) {
    if (!out_date || !out_time) {
        return;
    }

    memset(out_date, 0, sizeof(*out_date));
    memset(out_time, 0, sizeof(*out_time));

    if (nmea_hdl != NULL) {
        void *hdl = nmea_hdl;
        esp_gps_t *esp_gps = (esp_gps_t *)hdl;
        if (esp_gps != NULL) {
            gps_t *gps = &esp_gps->parent;
            if (gps != NULL && is_valid_date(&gps->date) &&
                gps->tim.hour <= 23 && gps->tim.minute <= 59 && gps->tim.second <= 59) {
                *out_date = gps->date;
                *out_time = gps->tim;
                return;
            }
        }
    }

    if (has_valid_cached_date && is_valid_date(&cacheddate)) {
        *out_date = cacheddate;
    } else {
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        struct tm tm_now;
        gmtime_r(&tv_now.tv_sec, &tm_now);
        out_date->year = (uint16_t)(tm_now.tm_year + 1900 - 2000);
        out_date->month = (uint8_t)(tm_now.tm_mon + 1);
        out_date->day = (uint8_t)tm_now.tm_mday;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);
    struct tm tm_now;
    gmtime_r(&tv_now.tv_sec, &tm_now);
    out_time->hour = (uint8_t)tm_now.tm_hour;
    out_time->minute = (uint8_t)tm_now.tm_min;
    out_time->second = (uint8_t)tm_now.tm_sec;
    out_time->thousand = 0;
}

#define CSV_GPS_BUFFER_SIZE 512
#define CSV_PRE_HEADER_SIZE 256

static FILE *csv_file = NULL;
static char *csv_buffer = NULL;
static size_t buffer_offset = 0;
static char csv_file_path[GPS_MAX_FILE_NAME_LENGTH];
static char csv_base_name[32] = "wardriving";
static bool gps_connection_logged = false;
static SemaphoreHandle_t csv_mutex = NULL;
static TaskHandle_t csv_flush_task = NULL;
static volatile bool csv_flush_requested = false;
static volatile bool csv_closing = false;
static bool csv_header_pending_uart = false;
static bool csv_jit_sd_disabled = false;
static bool csv_jit_file_written = false;
static bool csv_jit_file_queued = false;
static bool csv_writing_final_chunk = false;
static char *csv_data_line = NULL;
static TickType_t csv_last_sync_tick = 0;
static portMUX_TYPE csv_state_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t csv_active_producers = 0;
static TaskHandle_t csv_close_waiter = NULL;

static void csv_request_flush(void) {
    csv_flush_requested = true;
    if (csv_flush_task) {
        xTaskNotifyGive(csv_flush_task);
    }
}

static char *csv_pre_header = NULL;
static size_t csv_pre_header_len = 0;

static esp_err_t csv_write_chunk_to_sink(const char *data, size_t len);

#define WD_DEDUPE_SIZE_INTERNAL 128
#define WD_DEDUPE_SIZE_PSRAM 1024
#define WD_PROBE_MAX 32

typedef struct {
    uint32_t hash;
    uint8_t mac[6];
    int8_t best_rssi;
    uint8_t flags;
} wd_dedupe_entry_t;

#define WD_FLAG_USED       0x01
#define WD_FLAG_NAME_EMPTY 0x02

static wd_dedupe_entry_t *wd_wifi_dedupe = NULL;
static wd_dedupe_entry_t *wd_ble_dedupe = NULL;
static size_t wd_dedupe_size = 0;
static bool wd_dedupe_in_psram = false;
static size_t wd_wifi_idx = 0;
static size_t wd_ble_idx = 0;
static uint32_t wd_wifi_unique_logged = 0;
static uint32_t wd_ble_unique_logged = 0;
static uint32_t wd_wifi_hidden_count = 0;
static bool wd_wifi_saturated_warned = false;
static bool wd_ble_saturated_warned = false;

static uint32_t wd_hash_mac(const char *mac) {
    uint32_t hash = 2166136261u;
    while (*mac) {
        char c = *mac++;
        if (c >= 'a' && c <= 'f') c -= 32;
        hash ^= (uint8_t)c;
        hash *= 16777619u;
    }
    return hash;
}

static bool wd_is_pow2(size_t v) {
    return v && ((v & (v - 1)) == 0);
}

static bool csv_producer_begin(void) {
    bool accepted = false;
    portENTER_CRITICAL(&csv_state_mux);
    if (!csv_closing && csv_buffer && csv_data_line && wd_wifi_dedupe && wd_ble_dedupe &&
        wd_is_pow2(wd_dedupe_size)) {
        csv_active_producers++;
        accepted = true;
    }
    portEXIT_CRITICAL(&csv_state_mux);
    return accepted;
}

static void csv_producer_end(void) {
    portENTER_CRITICAL(&csv_state_mux);
    if (csv_active_producers > 0) {
        csv_active_producers--;
    }
    portEXIT_CRITICAL(&csv_state_mux);
}

static size_t wd_probe_index(uint32_t hash, size_t step) {
    size_t mask = wd_dedupe_size - 1;
    return ((size_t)hash + step) & mask;
}

static void wd_free_dedupe_tables(void) {
    if (wd_wifi_dedupe) {
        heap_caps_free(wd_wifi_dedupe);
        wd_wifi_dedupe = NULL;
    }
    if (wd_ble_dedupe) {
        heap_caps_free(wd_ble_dedupe);
        wd_ble_dedupe = NULL;
    }
    wd_dedupe_size = 0;
    wd_dedupe_in_psram = false;
}

static bool wd_allocate_dedupe_tables(void) {
    if (wd_wifi_dedupe && wd_ble_dedupe && wd_is_pow2(wd_dedupe_size)) {
        return true;
    }

    wd_free_dedupe_tables();

    size_t target_size = WD_DEDUPE_SIZE_INTERNAL;
    uint32_t caps = MALLOC_CAP_8BIT;

#if CONFIG_SPIRAM
    target_size = WD_DEDUPE_SIZE_PSRAM;
    caps |= MALLOC_CAP_SPIRAM;
#endif

    wd_wifi_dedupe = heap_caps_calloc(target_size, sizeof(wd_dedupe_entry_t), caps);
    wd_ble_dedupe = heap_caps_calloc(target_size, sizeof(wd_dedupe_entry_t), caps);

    if (!wd_wifi_dedupe || !wd_ble_dedupe) {
        if (wd_wifi_dedupe) {
            heap_caps_free(wd_wifi_dedupe);
            wd_wifi_dedupe = NULL;
        }
        if (wd_ble_dedupe) {
            heap_caps_free(wd_ble_dedupe);
            wd_ble_dedupe = NULL;
        }

        target_size = WD_DEDUPE_SIZE_INTERNAL;
        wd_wifi_dedupe = calloc(target_size, sizeof(wd_dedupe_entry_t));
        wd_ble_dedupe = calloc(target_size, sizeof(wd_dedupe_entry_t));
        wd_dedupe_in_psram = false;
    } else {
        wd_dedupe_in_psram = (target_size == WD_DEDUPE_SIZE_PSRAM);
    }

    if (!wd_wifi_dedupe || !wd_ble_dedupe) {
        wd_free_dedupe_tables();
        return false;
    }

    wd_dedupe_size = target_size;
    return true;
}

static void wd_mac_bytes(const char *text, uint8_t mac[6]) {
    // All producers use the canonical six-octet colon-separated format.
    unsigned values[6] = {0};
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) != 6) memset(values, 0, sizeof(values));
    for (unsigned i = 0; i < 6; ++i) mac[i] = (uint8_t)values[i];
}

static wd_dedupe_entry_t *wd_lookup_entry(wd_dedupe_entry_t *table, uint32_t hash, const char *text) {
    if (!table || !wd_is_pow2(wd_dedupe_size)) {
        return NULL;
    }

    uint8_t mac[6];
    wd_mac_bytes(text, mac);
    for (size_t step = 0; step < WD_PROBE_MAX; step++) {
        size_t idx = wd_probe_index(hash, step);
        wd_dedupe_entry_t *entry = &table[idx];
        if (!(entry->flags & WD_FLAG_USED)) {
            return NULL;
        }
        if (entry->hash == hash && memcmp(entry->mac, mac, 6) == 0) {
            return entry;
        }
    }

    return NULL;
}

static wd_dedupe_entry_t *wd_insert_entry(wd_dedupe_entry_t *table,
                                          uint32_t hash,
                                          size_t *ring_idx,
                                          bool *replaced) {
    if (!table || !ring_idx || !wd_is_pow2(wd_dedupe_size)) {
        return NULL;
    }

    if (replaced) {
        *replaced = false;
    }

    for (size_t step = 0; step < WD_PROBE_MAX; step++) {
        size_t idx = wd_probe_index(hash, step);
        wd_dedupe_entry_t *entry = &table[idx];
        if (!(entry->flags & WD_FLAG_USED)) {
            return entry;
        }
    }

    size_t victim_step = (*ring_idx) % WD_PROBE_MAX;
    *ring_idx = (*ring_idx + 1);
    size_t victim_idx = wd_probe_index(hash, victim_step);
    wd_dedupe_entry_t *entry = &table[victim_idx];
    if (replaced) {
        *replaced = ((entry->flags & WD_FLAG_USED) != 0);
    }
    return entry;
}

static void csv_escape_field(char *out, size_t out_len, const char *in) {
    if (out_len == 0) {
        return;
    }
    if (in == NULL) {
        out[0] = '\0';
        return;
    }

    bool need_quotes = false;
    for (const char *p = in; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quotes = true;
            break;
        }
    }

    if (!need_quotes) {
        snprintf(out, out_len, "%s", in);
        return;
    }

    size_t o = 0;
    if (o + 1 < out_len) {
        out[o++] = '"';
    }
    for (const char *p = in; *p && o + 1 < out_len; p++) {
        if (*p == '"') {
            if (o + 2 < out_len) {
                out[o++] = '"';
                out[o++] = '"';
            } else {
                break;
            }
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 < out_len) {
        out[o++] = '"';
    }
    out[o] = '\0';
}

static int csv_escape_append(char *buf, int off, int buf_size, const char *in) {
    if (!in || buf_size <= 0 || off >= buf_size) {
        if (off < buf_size && buf) buf[off] = '\0';
        return off;
    }

    bool need_quotes = false;
    for (const char *p = in; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            need_quotes = true;
            break;
        }
    }

    int o = off;
    int rem = buf_size - off;

    if (!need_quotes) {
        while (*in && o + 1 < buf_size) buf[o++] = *in++;
        buf[o] = '\0';
        return o;
    }

    if (o + 1 < buf_size) buf[o++] = '"';
    for (const char *p = in; *p; p++) {
        if (*p == '"') {
            if (o + 2 < buf_size) { buf[o++] = '"'; buf[o++] = '"'; }
            else break;
        } else {
            if (o + 1 < buf_size) buf[o++] = *p;
            else break;
        }
    }
    if (o + 1 < buf_size) buf[o++] = '"';
    buf[o] = '\0';
    return o;
}

static void csv_build_pre_header(void) {
    char f0[64], f1[64], f2[64], f3[64], f4[64], f5[64], f6[64], f7[64], f8[64], f9[64], f10[64];

    char app_release[64];
    char release[64];
    char device[64];

    const char *model_str = "ESP32";
    const char *board_str = "ESP32";
#if defined(CONFIG_IDF_TARGET_ESP32C5)
    model_str = "ESP32-C5";
    board_str = "ESP32-C5";
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    model_str = "ESP32-C6";
    board_str = "ESP32-C6";
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    model_str = "ESP32-S3";
    board_str = "ESP32-S3";
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    model_str = "ESP32-S2";
    board_str = "ESP32-S2";
#elif defined(CONFIG_IDF_TARGET_ESP32)
    model_str = "ESP32";
    board_str = "ESP32";
#endif

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (CONFIG_BUILD_CONFIG_TEMPLATE[0] != '\0' && strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "unknown_board") != 0) {
        model_str = CONFIG_BUILD_CONFIG_TEMPLATE;
        board_str = CONFIG_BUILD_CONFIG_TEMPLATE;
    }
#endif

    snprintf(app_release, sizeof(app_release), "appRelease=%s %s", GHOSTESP_NAME, GHOSTESP_VERSION);
    snprintf(release, sizeof(release), "release=%s", GHOSTESP_VERSION);
    snprintf(device, sizeof(device), "device=%s", GHOSTESP_NAME);

    csv_escape_field(f0, sizeof(f0), "WigleWifi-1.6");
    csv_escape_field(f1, sizeof(f1), app_release);
    {
        char model[64];
        snprintf(model, sizeof(model), "model=%s", model_str);
        csv_escape_field(f2, sizeof(f2), model);
    }
    csv_escape_field(f3, sizeof(f3), release);
    csv_escape_field(f4, sizeof(f4), device);
    csv_escape_field(f5, sizeof(f5), "display=NONE");
    {
        char board[64];
        snprintf(board, sizeof(board), "board=%s", board_str);
        csv_escape_field(f6, sizeof(f6), board);
    }
    csv_escape_field(f7, sizeof(f7), "brand=GhostESP");
    csv_escape_field(f8, sizeof(f8), "star=Sol");
    csv_escape_field(f9, sizeof(f9), "body=3");
    csv_escape_field(f10, sizeof(f10), "subBody=0");

    int n = snprintf(csv_pre_header,
                     CSV_PRE_HEADER_SIZE,
                     "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                     f0,
                     f1,
                     f2,
                     f3,
                     f4,
                     f5,
                     f6,
                     f7,
                     f8,
                     f9,
                     f10);
    if (n < 0) {
        csv_pre_header[0] = '\0';
        csv_pre_header_len = 0;
        return;
    }
    if ((size_t)n >= CSV_PRE_HEADER_SIZE) {
        csv_pre_header[CSV_PRE_HEADER_SIZE - 2] = '\n';
        csv_pre_header[CSV_PRE_HEADER_SIZE - 1] = '\0';
        csv_pre_header_len = strlen(csv_pre_header);
        return;
    }
    csv_pre_header_len = (size_t)n;
}

static wd_dedupe_entry_t *csv_find_wifi_dedupe_entry(uint32_t hash, const char *bssid) {
    return wd_lookup_entry(wd_wifi_dedupe, hash, bssid);
}

static bool csv_wifi_dedupe_should_log(const wd_dedupe_entry_t *entry, int rssi, bool ssid_empty) {
    if (entry == NULL) {
        return true;
    }
    if ((entry->flags & WD_FLAG_NAME_EMPTY) && !ssid_empty) {
        return true;
    }
    if (abs(rssi - entry->best_rssi) > 3) {
        return true;
    }
    return false;
}

bool csv_wifi_ap_should_log_peek(const char *bssid, int rssi, const char *ssid) {
    if (!bssid || csv_closing) return false;

    uint32_t hash = wd_hash_mac(bssid);
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    if (csv_mutex) xSemaphoreTake(csv_mutex, portMAX_DELAY);
    wd_dedupe_entry_t *entry = csv_find_wifi_dedupe_entry(hash, bssid);
    bool should_log = csv_wifi_dedupe_should_log(entry, rssi, ssid_empty);
    if (csv_mutex) xSemaphoreGive(csv_mutex);

    return should_log;
}

static void csv_wifi_ap_log_commit_unlocked(const char *bssid, int rssi, const char *ssid) {
    uint32_t hash = wd_hash_mac(bssid);
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    wd_dedupe_entry_t *entry = csv_find_wifi_dedupe_entry(hash, bssid);
    if (entry == NULL) {
        bool replaced = false;
        entry = wd_insert_entry(wd_wifi_dedupe, hash, &wd_wifi_idx, &replaced);
        if (!entry) {
            return;
        }

        if (replaced && (entry->flags & WD_FLAG_NAME_EMPTY) && wd_wifi_hidden_count > 0) {
            wd_wifi_hidden_count--;
        }

        entry->hash = hash;
        wd_mac_bytes(bssid, entry->mac);
        entry->flags = WD_FLAG_USED | (ssid_empty ? WD_FLAG_NAME_EMPTY : 0);
        entry->best_rssi = (int8_t)rssi;

        wd_wifi_unique_logged++;
        if (replaced && !wd_wifi_saturated_warned) {
            wd_wifi_saturated_warned = true;
            glog("WiFi dedupe saturated (%u entries); unique AP counter may include re-seen APs.\n",
                 (unsigned)wd_dedupe_size);
        }
        if (ssid_empty) {
            wd_wifi_hidden_count++;
        }
    } else {
        if ((entry->flags & WD_FLAG_NAME_EMPTY) && !ssid_empty) {
            entry->flags &= ~WD_FLAG_NAME_EMPTY;
            if (wd_wifi_hidden_count > 0) {
                wd_wifi_hidden_count--;
            }
        }
        entry->best_rssi = (int8_t)rssi;
    }
}

void csv_wifi_ap_log_commit(const char *bssid, int rssi, const char *ssid) {
    if (!bssid || csv_closing || !csv_mutex) return;

    xSemaphoreTake(csv_mutex, portMAX_DELAY);
    if (!csv_closing && wd_wifi_dedupe) {
        csv_wifi_ap_log_commit_unlocked(bssid, rssi, ssid);
    }
    xSemaphoreGive(csv_mutex);
}

bool csv_should_log_wifi_ap(const char *bssid, int rssi, const char *ssid) {
    bool should_log = csv_wifi_ap_should_log_peek(bssid, rssi, ssid);
    if (should_log) {
        csv_wifi_ap_log_commit(bssid, rssi, ssid);
    }
    return should_log;
}

static const char *wigle_wifi_capabilities(const char *enc) {
    if (enc == NULL || enc[0] == '\0') {
        return "[ESS]";
    }
    if (strcmp(enc, "OPEN") == 0) {
        return "[ESS]";
    }
    if (strcmp(enc, "WEP") == 0) {
        return "[WEP][ESS]";
    }
    if (strcmp(enc, "WPA") == 0) {
        return "[WPA-PSK][ESS]";
    }
    if (strcmp(enc, "WPA2") == 0) {
        return "[WPA2-PSK][ESS]";
    }
    if (strcmp(enc, "WPA3") == 0) {
        return "[WPA3-SAE][ESS]";
    }
    if (strcmp(enc, "OWE") == 0) {
        return "[OWE][ESS]";
    }
    return "[ESS]";
}

static bool csv_wifi_channel_is_valid(int channel) {
    if (channel >= 1 && channel <= 14) return true;
    if ((channel >= 36 && channel <= 64 && (channel % 4) == 0) ||
        (channel >= 100 && channel <= 144 && (channel % 4) == 0) ||
        (channel >= 149 && channel <= 165 && ((channel - 149) % 4) == 0)) {
        return true;
    }
    return false;
}

bool csv_buffer_has_pending_data(void) {
    return buffer_offset > 0;
}

uint32_t csv_get_unique_wifi_ap_count(void) {
    if (csv_closing || !csv_mutex) return 0;
    uint32_t count = 0;
    xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = (wd_wifi_unique_logged > wd_wifi_hidden_count)
                ? (wd_wifi_unique_logged - wd_wifi_hidden_count)
                : 0;
    xSemaphoreGive(csv_mutex);
    return count;
}

uint32_t csv_get_unique_wifi_ap_count_including_hidden(void) {
    if (csv_closing || !csv_mutex) return 0;
    uint32_t count = 0;
    xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = wd_wifi_unique_logged;
    xSemaphoreGive(csv_mutex);
    return count;
}

uint32_t csv_get_unique_ble_device_count(void) {
    if (csv_closing || !csv_mutex) return 0;
    uint32_t count = 0;
    xSemaphoreTake(csv_mutex, portMAX_DELAY);
    count = wd_ble_unique_logged;
    xSemaphoreGive(csv_mutex);
    return count;
}

size_t csv_get_pending_bytes(void) {
    if (csv_closing || !csv_mutex) return 0;
    size_t pending = 0;
    
    xSemaphoreTake(csv_mutex, portMAX_DELAY);
    pending = buffer_offset;
    xSemaphoreGive(csv_mutex);
    return pending;
}

static void csv_flush_task_fn(void *arg) {
    (void)arg;
    char write_chunk[CSV_GPS_BUFFER_SIZE];
    size_t pending_len = 0;
    for (;;) {
        if (pending_len == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            csv_flush_requested = false;

            xSemaphoreTake(csv_mutex, portMAX_DELAY);
            pending_len = buffer_offset;
            if (pending_len > sizeof(write_chunk)) pending_len = sizeof(write_chunk);
            if (pending_len > 0) {
                memcpy(write_chunk, csv_buffer, pending_len);
                memmove(csv_buffer, csv_buffer + pending_len, buffer_offset - pending_len);
                buffer_offset -= pending_len;
                csv_writing_final_chunk = csv_closing && buffer_offset == 0;
            }
            xSemaphoreGive(csv_mutex);
        }

        if (pending_len > 0) {
            // The shared buffer is unlocked before storage or UART I/O, so radio
            // callbacks remain fast while this task drains a bounded chunk.
            if (csv_write_chunk_to_sink(write_chunk, pending_len) == ESP_OK) {
                pending_len = 0;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (csv_closing) {
            TaskHandle_t waiter;
            portENTER_CRITICAL(&csv_state_mux);
            waiter = csv_close_waiter;
            csv_flush_task = NULL;
            portEXIT_CRITICAL(&csv_state_mux);
            if (waiter) {
                xTaskNotifyGive(waiter);
            }
            vTaskDelete(NULL);
        }
    }
}

esp_err_t csv_write_header(FILE *f) {
    if (f == NULL) {
        csv_header_pending_uart = true;
        return ESP_OK;
    } else {
        if (csv_pre_header_len == 0) {
            csv_build_pre_header();
        }
        size_t pre_len = csv_pre_header_len;
        size_t hdr_len = strlen(CSV_HEADER);
        size_t written = fwrite(csv_pre_header, 1, pre_len, f);
        if (written != pre_len) {
            return ESP_FAIL;
        }
        written = fwrite(CSV_HEADER, 1, hdr_len, f);
        if (written != hdr_len) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }
}

void get_next_csv_file_name(char *file_name_buffer, const char *base_name) {
    int next_index = get_next_csv_file_index(base_name);
    snprintf(file_name_buffer, GPS_MAX_FILE_NAME_LENGTH, SD_DIR_GPS "/%s_%d.csv", base_name,
             next_index);
}

bool csv_file_is_open(void) {
    return !csv_closing && csv_buffer != NULL;
}

esp_err_t csv_file_open(const char *base_file_name) {
    if (csv_buffer || csv_flush_task || csv_file) {
        return ESP_ERR_INVALID_STATE;
    }
    csv_closing = false;
    char file_name[GPS_MAX_FILE_NAME_LENGTH];

    if (!csv_buffer) {
        csv_buffer = (char *)heap_caps_calloc(1, GPS_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!csv_buffer) csv_buffer = (char *)calloc(1, GPS_BUFFER_SIZE);
        if (!csv_buffer) return ESP_ERR_NO_MEM;
    }
    if (!csv_pre_header) {
        csv_pre_header = (char *)heap_caps_calloc(1, CSV_PRE_HEADER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!csv_pre_header) csv_pre_header = (char *)calloc(1, CSV_PRE_HEADER_SIZE);
        if (!csv_pre_header) { free(csv_buffer); csv_buffer = NULL; return ESP_ERR_NO_MEM; }
    }
    buffer_offset = 0;
    csv_last_sync_tick = 0;
    csv_header_pending_uart = false;
    csv_jit_sd_disabled = false;
    csv_jit_file_written = false;
    csv_jit_file_queued = false;
    csv_writing_final_chunk = false;

    csv_build_pre_header();

    // remember base name for later just-in-time open on somethingsomething
    if (base_file_name && *base_file_name) {
        strncpy(csv_base_name, base_file_name, sizeof(csv_base_name) - 1);
        csv_base_name[sizeof(csv_base_name) - 1] = '\0';
    }
    csv_file_path[0] = '\0';

    bool gating_template = sd_card_needs_jit_mount();

    if (sd_card_exists(SD_DIR_GPS)) {
        get_next_csv_file_name(file_name, base_file_name);
        strncpy(csv_file_path, file_name, GPS_MAX_FILE_NAME_LENGTH);
        csv_file = fopen(file_name, "w");
    } else {
        // on somethingsomething, we will mount just-in-time during flush
        if (gating_template && !csv_jit_sd_disabled) {
            csv_file = NULL;
            csv_file_path[0] = '\0';
        } else {
            csv_file = NULL;
        }
    }

    if (csv_mutex == NULL) {
        csv_mutex = xSemaphoreCreateMutex();
        if (!csv_mutex) {
            if (csv_file) {
                fclose(csv_file);
                csv_file = NULL;
            }
            free(csv_buffer);
            free(csv_pre_header);
            csv_buffer = NULL;
            csv_pre_header = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    if (!wd_allocate_dedupe_tables()) {
        glog("Failed to allocate wardrive dedupe tables\n");
        if (csv_file) {
            fclose(csv_file);
            csv_file = NULL;
        }
        free(csv_buffer);
        free(csv_pre_header);
        csv_buffer = NULL;
        csv_pre_header = NULL;
        csv_pre_header_len = 0;
        return ESP_ERR_NO_MEM;
    }

    wd_wifi_idx = 0;
    wd_ble_idx = 0;
    wd_wifi_unique_logged = 0;
    wd_ble_unique_logged = 0;
    wd_wifi_hidden_count = 0;
    wd_wifi_saturated_warned = false;
    wd_ble_saturated_warned = false;
    memset(wd_wifi_dedupe, 0, wd_dedupe_size * sizeof(wd_dedupe_entry_t));
    memset(wd_ble_dedupe, 0, wd_dedupe_size * sizeof(wd_dedupe_entry_t));

    glog("Wardrive dedupe table: %u entries (%s)\n",
         (unsigned)wd_dedupe_size,
         wd_dedupe_in_psram ? "PSRAM" : "internal RAM");

    esp_err_t ret = csv_write_header(csv_file);
    if (ret != ESP_OK) {
        glog("Failed to write CSV header.");
        fclose(csv_file);
        csv_file = NULL;
        wd_free_dedupe_tables();
        free(csv_buffer);
        free(csv_pre_header);
        csv_buffer = NULL;
        csv_pre_header = NULL;
        csv_pre_header_len = 0;
        return ret;
    }

    if (csv_file == NULL) {
        // UART output must carry a complete WiGLE document just like SD output.
        csv_header_pending_uart = true;
    }

    if (!csv_data_line) {
        csv_data_line = (char *)malloc(CSV_GPS_BUFFER_SIZE);
        if (!csv_data_line) {
            if (csv_file) {
                fclose(csv_file);
                csv_file = NULL;
            }
            wd_free_dedupe_tables();
            free(csv_buffer);
            free(csv_pre_header);
            csv_buffer = NULL;
            csv_pre_header = NULL;
            csv_pre_header_len = 0;
            return ESP_ERR_NO_MEM;
        }
    }

    if (csv_flush_task == NULL) {
        if (xTaskCreate(csv_flush_task_fn, "csv_flush", 3072, NULL, 1, &csv_flush_task) != pdPASS) {
            if (csv_file) {
                fclose(csv_file);
                csv_file = NULL;
            }
            free(csv_data_line);
            free(csv_buffer);
            free(csv_pre_header);
            csv_data_line = NULL;
            csv_buffer = NULL;
            csv_pre_header = NULL;
            csv_pre_header_len = 0;
            wd_free_dedupe_tables();
            return ESP_ERR_NO_MEM;
        }
    }

    if (csv_file) {
        glog("Streaming CSV buffer to SD card\n");
    } else {
        if (gating_template) {
            glog("CSV buffer will flush to SD via JIT mount (fallback UART)\n");
        } else {
            glog("Streaming CSV buffer over UART\n");
        }
        // Header will be emitted with the first non-empty flush via csv_flush_buffer_to_file()
    }
    return ESP_OK;
}

esp_err_t csv_write_data_to_buffer(wardriving_data_t *data) {
    if (!data) return ESP_ERR_INVALID_ARG;

    char timestamp[24];
    gps_date_t date_to_use = data->gps_date;
    if (!data->gps_date_valid || !is_valid_date(&date_to_use)) {
        if (!has_valid_cached_date) return ESP_ERR_INVALID_STATE;
        date_to_use = cacheddate;
    }
    gps_time_t time_to_use = data->gps_time;
    if (!data->gps_time_valid || time_to_use.hour > 23 || time_to_use.minute > 59 ||
        time_to_use.second > 59) return ESP_ERR_INVALID_STATE;
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             gps_get_absolute_year(date_to_use.year), date_to_use.month, date_to_use.day,
             time_to_use.hour, time_to_use.minute, time_to_use.second);

    if (!csv_producer_begin()) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(csv_mutex, 0) != pdTRUE) {
        csv_producer_end();
        return ESP_ERR_TIMEOUT;
    }

    int len;
    char *data_line = csv_data_line;
    wd_dedupe_entry_t *dedupe_entry = NULL;
    bool dedupe_new = false;
    bool dedupe_replaced = false;
    bool name_empty = false;
    bool should_log = true;

    if (data->ble_data.is_ble_device) {
        uint32_t hash = wd_hash_mac(data->ble_data.ble_mac);
        dedupe_entry = wd_lookup_entry(wd_ble_dedupe, hash, data->ble_data.ble_mac);
        name_empty = (data->ble_data.ble_name[0] == '\0');
        if (dedupe_entry == NULL) {
            dedupe_entry = wd_insert_entry(wd_ble_dedupe, hash, &wd_ble_idx, &dedupe_replaced);
            dedupe_new = true;
            should_log = dedupe_entry != NULL;
        } else {
            should_log = ((dedupe_entry->flags & WD_FLAG_NAME_EMPTY) && !name_empty) ||
                         (abs(data->ble_data.ble_rssi - dedupe_entry->best_rssi) > 5);
        }
        if (!should_log) {
            xSemaphoreGive(csv_mutex);
            csv_producer_end();
            return ESP_OK;
        }

        char mfgr_str[12] = {0};
        if (data->ble_data.ble_has_mfgr_id) {
            snprintf(mfgr_str, sizeof(mfgr_str), "%u", (unsigned)data->ble_data.ble_mfgr_id);
        }
        int altitude_val = (int)lround(data->altitude);
        if (altitude_val > 1000000 || altitude_val < -1000000) altitude_val = 0;
        int o = snprintf(data_line, CSV_GPS_BUFFER_SIZE, "%s,", data->ble_data.ble_mac);
        o = csv_escape_append(data_line, o, CSV_GPS_BUFFER_SIZE, data->ble_data.ble_name);
        o += snprintf(data_line + o, CSV_GPS_BUFFER_SIZE - o, ",");
        o = csv_escape_append(data_line, o, CSV_GPS_BUFFER_SIZE, "Misc [LE]");
        o += snprintf(data_line + o, CSV_GPS_BUFFER_SIZE - o,
                      ",%s,0,%u,%d,%.6f,%.6f,%d,%.1f,,%s,BLE\n", timestamp,
                      (unsigned)data->ble_data.ble_appearance, data->ble_data.ble_rssi,
                      data->latitude, data->longitude, altitude_val, data->accuracy, mfgr_str);
        len = o;
    } else {
        uint32_t hash = wd_hash_mac(data->bssid);
        dedupe_entry = csv_find_wifi_dedupe_entry(hash, data->bssid);
        name_empty = (data->ssid[0] == '\0');
        if (!csv_wifi_channel_is_valid(data->channel)) {
            xSemaphoreGive(csv_mutex);
            csv_producer_end();
            return ESP_ERR_INVALID_ARG;
        }
        should_log = csv_wifi_dedupe_should_log(dedupe_entry, data->rssi, name_empty);
        if (!should_log) {
            xSemaphoreGive(csv_mutex);
            csv_producer_end();
            return ESP_OK;
        }
        int frequency = (data->channel == 14) ? 2484 :
                        (data->channel > 14) ? 5000 + (data->channel * 5) :
                                               2407 + (data->channel * 5);
        const char *ssid_for_csv = name_empty ? "<hidden>" : data->ssid;
        int o = snprintf(data_line, CSV_GPS_BUFFER_SIZE, "%s,", data->bssid);
        o = csv_escape_append(data_line, o, CSV_GPS_BUFFER_SIZE, ssid_for_csv);
        o += snprintf(data_line + o, CSV_GPS_BUFFER_SIZE - o, ",");
        o = csv_escape_append(data_line, o, CSV_GPS_BUFFER_SIZE,
                              wigle_wifi_capabilities(data->encryption_type));
        o += snprintf(data_line + o, CSV_GPS_BUFFER_SIZE - o,
                      ",%s,%d,%d,%d,%.6f,%.6f,%d,%.1f,,,WIFI\n", timestamp,
                      data->channel, frequency, data->rssi, data->latitude, data->longitude,
                      (int)lround(data->altitude), data->accuracy);
        len = o;
    }

    if (len < 1 || len >= CSV_GPS_BUFFER_SIZE || data_line[len - 1] != '\n') {
        ESP_LOGE(CSV_TAG, "CSV line truncated or malformed (len=%d)", len);
        xSemaphoreGive(csv_mutex);
        csv_producer_end();
        return ESP_ERR_NO_MEM;
    }
    if (buffer_offset + len >= GPS_BUFFER_SIZE) {
        csv_request_flush();
        xSemaphoreGive(csv_mutex);
        csv_producer_end();
        return ESP_ERR_NO_MEM;
    }

    memcpy(csv_buffer + buffer_offset, data_line, len);
    buffer_offset += len;
    if (data->ble_data.is_ble_device) {
        if (dedupe_new) {
            dedupe_entry->hash = wd_hash_mac(data->ble_data.ble_mac);
            wd_mac_bytes(data->ble_data.ble_mac, dedupe_entry->mac);
            dedupe_entry->flags = WD_FLAG_USED | (name_empty ? WD_FLAG_NAME_EMPTY : 0);
            dedupe_entry->best_rssi = (int8_t)data->ble_data.ble_rssi;
            wd_ble_unique_logged++;
            if (dedupe_replaced && !wd_ble_saturated_warned) {
                wd_ble_saturated_warned = true;
                glog("BLE dedupe saturated (%u entries); unique device counter may include re-seen devices.\n",
                     (unsigned)wd_dedupe_size);
            }
        } else {
            if ((dedupe_entry->flags & WD_FLAG_NAME_EMPTY) && !name_empty) {
                dedupe_entry->flags &= ~WD_FLAG_NAME_EMPTY;
            }
            dedupe_entry->best_rssi = (int8_t)data->ble_data.ble_rssi;
        }
    } else {
        csv_wifi_ap_log_commit_unlocked(data->bssid, data->rssi, data->ssid);
    }
    if (buffer_offset >= (GPS_BUFFER_SIZE - CSV_GPS_BUFFER_SIZE)) csv_request_flush();
    xSemaphoreGive(csv_mutex);
    csv_producer_end();
    return ESP_OK;
}

esp_err_t csv_flush_buffer_to_file() {
    if (csv_closing || !csv_flush_task) return ESP_ERR_INVALID_STATE;
    csv_request_flush();
    return ESP_OK;
}

static esp_err_t csv_write_chunk_to_sink(const char *data, size_t len) {
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;
    if (csv_file == NULL) {
        bool gating_template = sd_card_needs_jit_mount();

        if (gating_template && !csv_jit_sd_disabled) {
            bool display_was_suspended = false;
            esp_err_t mret = sd_card_mount_for_flush(&display_was_suspended);
            if (mret == ESP_OK) {
                // lazily choose file name on first flush if not set
                if (csv_file_path[0] == '\0') {
                    get_next_csv_file_name(csv_file_path, csv_base_name);
                }
                FILE *f = fopen(csv_file_path, "ab+");
                if (f) {
                    // The producer buffer never contains headers; each new JIT file does.
                    fseek(f, 0, SEEK_END);
                    long sz = ftell(f);
                    if (sz == 0 && csv_write_header(f) != ESP_OK) {
                        fclose(f);
                        sd_card_unmount_after_flush(display_was_suspended);
                        csv_jit_sd_disabled = true;
                        return ESP_FAIL;
                    }
                    size_t written = fwrite(data, 1, len, f);
                    int close_result = fclose(f);
                    if (written != len || close_result != 0) {
                        glog("Failed to write buffer to file.\n");
                        csv_jit_sd_disabled = true;
                    } else {
                        glog("Flushed %zu bytes to CSV file.\n", len);
                        csv_jit_file_written = true;
                        if (csv_writing_final_chunk && !csv_jit_file_queued) {
                            wigle_queue_add(csv_file_path);
                            csv_jit_file_queued = true;
                        }
                        sd_card_unmount_after_flush(display_was_suspended);
                        return ESP_OK;
                    }
                }
                sd_card_unmount_after_flush(display_was_suspended);
            }
        }

        glog_set_defer(1);
        const char *mark_begin = "[BUF/BEGIN]";
        const char *mark_close = "[BUF/CLOSE]";
        size_t mark_begin_len = strlen(mark_begin);
        size_t mark_close_len = strlen(mark_close);
        size_t header_len = csv_header_pending_uart ? (csv_pre_header_len + strlen(CSV_HEADER)) : 0;
        size_t out_len = mark_begin_len + header_len + len + mark_close_len + 1;
        uint8_t *out = (uint8_t *)malloc(out_len);
        if (out) {
            size_t off = 0;
            memcpy(out + off, mark_begin, mark_begin_len); off += mark_begin_len;
            if (csv_header_pending_uart) {
                size_t pre_len = csv_pre_header_len;
                size_t hdr_len = strlen(CSV_HEADER);
                memcpy(out + off, csv_pre_header, pre_len); off += pre_len;
                memcpy(out + off, CSV_HEADER, hdr_len); off += hdr_len;
                csv_header_pending_uart = false;
            }
            memcpy(out + off, data, len); off += len;
            memcpy(out + off, mark_close, mark_close_len); off += mark_close_len;
            out[off++] = '\n';
            uart_write_bytes(UART_NUM_0, (const char *)out, off);
            free(out);
        } else {
            uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
            if (csv_header_pending_uart) {
                uart_write_bytes(UART_NUM_0, csv_pre_header, csv_pre_header_len);
                uart_write_bytes(UART_NUM_0, CSV_HEADER, strlen(CSV_HEADER));
                csv_header_pending_uart = false;
            }
            uart_write_bytes(UART_NUM_0, data, len);
            uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
            uart_write_bytes(UART_NUM_0, "\n", 1);
        }
        glog_set_defer(0);
        glog_flush_deferred();

        return ESP_OK;
    }

    size_t written = fwrite(data, 1, len, csv_file);
    TickType_t now = xTaskGetTickCount();
    bool sync_due = csv_closing || csv_last_sync_tick == 0 ||
                    (now - csv_last_sync_tick) >= pdMS_TO_TICKS(2000);
    if (written != len || (sync_due && fflush(csv_file) != 0)) {
        glog("Failed to write buffer to file.\n");
        fclose(csv_file);
        csv_file = NULL;
        csv_header_pending_uart = true;
        return ESP_FAIL;
    }
    if (sync_due) csv_last_sync_tick = now;

    glog("Flushed %zu bytes to CSV file.\n", len);

    return ESP_OK;
}

void csv_file_close() {
    bool already_closing;
    portENTER_CRITICAL(&csv_state_mux);
    already_closing = csv_closing;
    if (!already_closing) {
        csv_closing = true;
        csv_close_waiter = xTaskGetCurrentTaskHandle();
    }
    portEXIT_CRITICAL(&csv_state_mux);
    if (already_closing) return;

    for (;;) {
        uint32_t active;
        portENTER_CRITICAL(&csv_state_mux);
        active = csv_active_producers;
        portEXIT_CRITICAL(&csv_state_mux);
        if (active == 0) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (csv_flush_task != NULL) {
        csv_request_flush();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    if (csv_file != NULL) {
        fclose(csv_file);
        csv_file = NULL;
    }
    if (csv_file_path[0] != '\0') {
        bool saved = sd_card_needs_jit_mount() && csv_jit_file_written;
        if (!saved) {
            gps_date_t file_date = {0};
            gps_time_t file_time = {0};
            resolve_timestamp_for_file(&file_date, &file_time);
            const char *mount = "/mnt";
            const char *rel_path = csv_file_path + strlen(mount);
            if (*rel_path == '/') rel_path++;
            FILINFO finfo;
            if (f_stat(rel_path, &finfo) == FR_OK) {
                uint16_t year = gps_get_absolute_year(file_date.year);
                finfo.fdate = ((year - 1980) << 9) | (file_date.month << 5) | file_date.day;
                finfo.ftime =
                    (file_time.hour << 11) | (file_time.minute << 5) | (file_time.second / 2);
                f_utime(rel_path, &finfo);
                saved = true;
            }
        }
        if (saved) {
            if (!sd_card_needs_jit_mount() || !csv_jit_file_queued) {
                wigle_queue_add(csv_file_path);
            }
            toast_show("GPS log saved", TOAST_SUCCESS);
        }
    }
    buffer_offset = 0;
    csv_header_pending_uart = false;
    if (csv_buffer) { free(csv_buffer); csv_buffer = NULL; }
    if (csv_pre_header) { free(csv_pre_header); csv_pre_header = NULL; }
    if (csv_data_line) { free(csv_data_line); csv_data_line = NULL; }
    csv_pre_header_len = 0;
    wd_free_dedupe_tables();
    csv_close_waiter = NULL;
    glog("CSV file closed.\n");
}

static bool is_valid_date(const gps_date_t *date) {
    if (!date)
        return false;

    // Check year (0-99 represents 2000-2099)
    if (!gps_is_valid_year(date->year))
        return false;

    // Check month (1-12)
    if (date->month < 1 || date->month > 12)
        return false;

    // Check day (1-31 depending on month)
    uint8_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    // Adjust February for leap years
    uint16_t absolute_year = gps_get_absolute_year(date->year);
    if ((absolute_year % 4 == 0 && absolute_year % 100 != 0) || (absolute_year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    if (date->day < 1 || date->day > days_in_month[date->month - 1])
        return false;

    return true;
}

void populate_gps_quality_data(wardriving_data_t *data, const gps_t *gps) {
    if (!data || !gps)
        return;

    data->gps_quality.satellites_used = gps->sats_in_use;
    data->gps_quality.hdop = gps->dop_h;
    data->gps_quality.speed = gps->speed;
    data->gps_quality.course = gps->cog;
    data->gps_quality.fix_quality = gps->fix;
    data->gps_quality.magnetic_var = gps->variation;
    data->gps_quality.has_valid_fix = gps->valid;

    // Calculate accuracy (existing method)
    data->accuracy = gps->dop_h * 5.0;

    // Only overwrite coordinates if caller hasn't set them
    if (data->latitude == 0.0 && data->longitude == 0.0) {
        data->latitude = gps->latitude;
        data->longitude = gps->longitude;
    }
    data->altitude = gps->altitude;
}

const char *get_gps_quality_string(const wardriving_data_t *data) {
    if (!data->gps_quality.has_valid_fix) {
        return "No Fix";
    }

    if (data->gps_quality.hdop <= 1.0) {
        return "Excellent";
    } else if (data->gps_quality.hdop <= 2.0) {
        return "Good";
    } else if (data->gps_quality.hdop <= 5.0) {
        return "Moderate";
    } else if (data->gps_quality.hdop <= 10.0) {
        return "Fair";
    } else {
        return "Poor";
    }
}

static const char *get_cardinal_direction(float course) {
    const char *directions[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    int index = (int)((course + 11.25f) / 22.5f) % 16;
    return directions[index];
}

static const char *get_fix_type_str(uint8_t fix) {
    switch (fix) {
    case GPS_FIX_INVALID:
        return "No Fix";
    case GPS_FIX_GPS:
        return "GPS";
    case GPS_FIX_DGPS:
        return "DGPS";
    default:
        return "Unknown";
    }
}

static void format_coordinates(double lat, double lon, char *lat_str, char *lon_str) {
    int lat_deg = (int)fabs(lat);
    double lat_min = (fabs(lat) - lat_deg) * 60;
    int lon_deg = (int)fabs(lon);
    double lon_min = (fabs(lon) - lon_deg) * 60;

    snprintf(lat_str, 20, "%ddeg %.4f'%c", lat_deg, lat_min, lat >= 0 ? 'N' : 'S');
    snprintf(lon_str, 20, "%ddeg %.4f'%c", lon_deg, lon_min, lon >= 0 ? 'E' : 'W');
}

float get_accuracy_percentage(float hdop) {
    // HDOP ranges from 1 (best) to 20+ (worst)
    // Let's consider HDOP of 1 as 100% and HDOP of 20 as 0%

    if (hdop <= 1.0f)
        return 100.0f;
    if (hdop >= 20.0f)
        return 0.0f;

    // Linear interpolation between 1 and 20
    return (20.0f - hdop) * (100.0f / 19.0f);
}

void gps_info_display_task(void *pvParameters) {
    const TickType_t delay = pdMS_TO_TICKS(5000);
    char lat_str[20] = {0}, lon_str[20] = {0};
    static wardriving_data_t gps_data = {0};
    static int8_t last_sats_warn_state = -1;
    static uint8_t gps_debug_count = 0;
    while (1) {
        bool peer_preferred = gps_manager_is_peer_gps_preferred();
        bool using_peer = false;
        gps_t gps_snapshot = {0};
        bool have_active_gps = gps_manager_get_active_gps_snapshot(&gps_snapshot, &using_peer);

        if (!have_active_gps) {
            if (gps_connection_logged) {
                glog("GPS Module Disconnected\n");
                gps_connection_logged = false;
            }
            if (peer_preferred) {
                glog("\nAwaiting peer GPS stream...\n");
            }
            vTaskDelay(delay);
            continue;
        }

        gps_t *gps = &gps_snapshot;
        const char *source = using_peer ? "Peer" : "Local";
        bool date_valid = gps->date.year <= 99 && gps->date.month >= 1 && gps->date.month <= 12 &&
                          gps->date.day >= 1 && gps->date.day <= 31;
        char date_str[24] = {0};
        if (date_valid) {
            snprintf(date_str,
                     sizeof(date_str),
                     "%04u-%02u-%02u",
                     (unsigned)(2000 + gps->date.year),
                     (unsigned)gps->date.month,
                     (unsigned)gps->date.day);
        } else {
            snprintf(date_str, sizeof(date_str), "Invalid");
        }

        if (!gps->valid || gps->fix < GPS_FIX_GPS || gps->fix_mode < GPS_MODE_2D ||
            gps->sats_in_use < 3) {
            // Debug: log when we have coords but no valid fix (weird state)
            static bool logged_coords_no_fix = false;
            if (!logged_coords_no_fix && gps->latitude != 0.0 && gps->longitude != 0.0) {
                logged_coords_no_fix = true;
                if (gps_debug_count < 3) {
                    gps_debug_count++;
                    glog("GPS Debug: coords but no fix! valid=%d fix=%d sats_in_use=%d dop_h=%.1f lat=%.6f lon=%.6f\n",
                         gps->valid, gps->fix, gps->sats_in_use, gps->dop_h, gps->latitude, gps->longitude);
                }
            } else if (gps->latitude == 0.0 && gps->longitude == 0.0) {
                logged_coords_no_fix = false;
            }
            if (!gps_is_timeout_detected()) {
                const char *fix_str = gps->fix_mode == GPS_MODE_3D ? "3D" 
                                     : gps->fix_mode == GPS_MODE_2D ? "2D" 
                                     : gps->fix == GPS_FIX_GPS ? "GPS" : "No Fix";
                glog("\nAcquiring GPS...\nSource: %s\nFix: %s\nDate: %s\nSats: %d/%d in view",
                     source,
                     fix_str,
                     date_str,
                     gps->sats_in_use,
                     gps->sats_in_view > 0 ? gps->sats_in_view : 0);
            }
        } else {
            // Only populate GPS data if we have a valid fix
            int8_t sats_warn = (gps->sats_in_use < 3) ? 1 : 0;
            if (sats_warn != last_sats_warn_state) {
                last_sats_warn_state = sats_warn;
                if (gps_debug_count < 3) {
                    gps_debug_count++;
                    glog("GPS Debug: sats_in_use=%d sats_in_view=%d dop_h=%.1f valid=%d fix=%d\n",
                         gps->sats_in_use, gps->sats_in_view, gps->dop_h, gps->valid, gps->fix);
                }
            }
            populate_gps_quality_data(&gps_data, gps);
            format_coordinates(gps_data.latitude, gps_data.longitude, lat_str, lon_str);
            const char *direction = get_cardinal_direction(gps_data.gps_quality.course);

            glog("\nGPS Info\nSource: %s\nFix: %s\nDate: %s\nSats: %d/%d\nLat: %s\nLong: %s\nAlt: %.1fm\nSpeed: %.1f km/h\nDirection: %d° %s\nHDOP: %.1f",
                 source,
                 gps->fix_mode == GPS_MODE_3D ? "3D" : "2D",
                 date_str,
                 gps_data.gps_quality.satellites_used,
                 gps->sats_in_view, lat_str, lon_str, gps->altitude,
                 gps->speed * 3.6,
                 (int)gps_data.gps_quality.course, direction ? direction : "Unknown", gps->dop_h);
        }

        vTaskDelay(delay);
    }
}

void csv_file_close_fast() {
    // Dropping buffered rows makes shutdown timing-dependent and can race a scan
    // callback. Retain the API but use the same draining lifecycle as normal close.
    csv_file_close();
}
