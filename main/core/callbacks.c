#include "core/callbacks.h"
#include "core/system_manager.h"
#include "esp_wifi.h"
#include "managers/gps_manager.h"
#include "managers/rgb_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "managers/status_display_manager.h"
#include "managers/ghostchi_manager.h"
#include "managers/ghostscript_runtime.h"
#include "core/utils.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/pcap.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "scans/wifi/wifi_channels.h"
#include "scans/wifi/hop_profile.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/wardrive_policy.h"
#include "scans/wifi/wardrive_scan.h"
#include "managers/settings_manager.h"
#include <ctype.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_rom_sys.h"  // Contains esp_rom_printf
#include <esp_timer.h>  // For esp_timer_get_time
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "gui/toast.h"
#ifdef CONFIG_HAS_RTC_CLOCK
#include "vendor/drivers/pcf8563.h"
#endif

// prototypes for static inline helpers
static inline bool is_packet_valid(const wifi_promiscuous_pkt_t *pkt, wifi_promiscuous_pkt_type_t type);
static inline bool is_on_target_channel(const wifi_promiscuous_pkt_t *pkt, uint8_t target_channel);

#define STORE_STR_ATTR
#define STORE_DATA_ATTR
#define WPS_OUI 0x0050f204
#define TAG "WIFI_MONITOR"

/* Heap-on-demand allocation for monitor tables: PSRAM first, internal
 * fallback (helps PSRAM-less targets by time-slicing instead of static). */
static void *mon_tbl_calloc(size_t n, size_t size) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}
#define WPS_CONF_METHODS_PBC 0x0080
#define WPS_CONF_METHODS_PIN_DISPLAY 0x0004
#define WPS_CONF_METHODS_PIN_KEYPAD 0x0008
#define WIFI_PKT_DEAUTH 0x0C     // Deauth subtype
#define WIFI_PKT_BEACON 0x08     // Beacon subtype
#define WIFI_PKT_PROBE_REQ 0x04  // Probe Request subtype
#define WIFI_PKT_PROBE_RESP 0x05 // Probe Response subtype
#define WIFI_PKT_EAPOL 0x80
#define ESP_WIFI_VENDOR_METADATA_LEN 8 // Channel(1) + RSSI(1) + Rate(1) + Timestamp(4) + Noise(1)
#define MIN_SSIDS_FOR_DETECTION 2 // Minimum SSIDs needed to flag as PineAP
#define MAX_PINEAP_NETWORKS 20
#define MAX_SSIDS_PER_BSSID 10
#if !defined(MAX_WIFI_CHANNEL)
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define MAX_WIFI_CHANNEL 165
#else
#define MAX_WIFI_CHANNEL 13
#endif
#endif
#define CHANNEL_HOP_INTERVAL_MS 100
#define WARDRIVE_STREAM_VERSION 2
#define WARDRIVE_STREAM_VERSION_LEGACY 1
#define WARDRIVE_STREAM_FLAG_GPS_PRESENT 0x01
#define WARDRIVE_STREAM_FLAG_GPS_FIX 0x02
#define WARDRIVE_STREAM_FLAG_GPS_DATE_VALID 0x04
#define WARDRIVE_STREAM_FLAG_GPS_TIME_VALID 0x08
#define GPS_STREAM_VERSION 1
#define GPS_STREAM_FLAG_PRESENT 0x01
#define GPS_STREAM_FLAG_FIX 0x02
#define GPS_STREAM_FLAG_DATE_VALID 0x04
#define GPS_STREAM_FLAG_TIME_VALID 0x08
#define WARDRIVE_CONTROL_MARKER 0xF0
#define WARDRIVE_CONTROL_VERSION 1
#define WARDRIVE_CONTROL_HELPER_READY 1
#define WARDRIVE_CONTROL_VERSION_PLAN 2
#define WARDRIVE_CONTROL_OBS_ACK 2
#define WARDRIVE_STREAM_VERSION_ACKED 3
#define WARDRIVE_HELPER_STATUS_TIMEOUT_MS 25000
#define WARDRIVE_HELPER_DEDUPE_SIZE 128
#define WARDRIVE_HELPER_REFRESH_MS 2000
#define WARDRIVE_OBS_QUEUE_PSRAM_LEN 64
#define WARDRIVE_OBS_QUEUE_INTERNAL_LEN 32
#define WARDRIVE_OBS_TASK_STACK_BYTES 8192
#define PEER_GPS_STREAM_INTERVAL_MS 1000
#define PEER_GPS_INIT_RETRY_MS 5000
#define RECENT_SSID_COUNT 5
#define LOG_DELAY_MS 5000
#define PROBE_DEDUPE_TIMEOUT_MS 1000
#define MIN_RSSI_THRESHOLD -90  // Drop packets weaker than -90 dBm
#define MIN_PACKET_LENGTH 24    // Minimum 802.11 header size
#define MAX_IE_LEN 255
static const uint8_t pineapple_ouis[][3] = {
    {0x00, 0x13, 0x37},
};
static const size_t pineapple_oui_count = sizeof(pineapple_ouis) / sizeof(pineapple_ouis[0]);
static pineap_network_t *pineap_networks = NULL;
static int pineap_network_count = 0;
static bool pineap_detection_active = false;
static uint8_t current_channel = 1;
static esp_timer_handle_t channel_hop_timer = NULL;
static bool wardriving_hopping_active = false;
static uint8_t wardrive_channel = 1;
static esp_timer_handle_t wardrive_hop_timer = NULL;
static esp_timer_handle_t wardrive_heartbeat_timer = NULL;
static int64_t wardrive_start_us = 0;
static uint32_t wardrive_wifi_frames_seen = 0;
static uint32_t wardrive_ble_advs_seen = 0;
static uint32_t wardrive_log_attempts = 0;
static uint32_t wardrive_log_ok = 0;
static uint32_t wardrive_gps_rejected = 0;
static uint32_t wardrive_helper_rx_observations = 0;
static uint32_t wardrive_helper_merged_ok = 0;
static uint32_t wardrive_helper_tx_new = 0;
static uint32_t wardrive_helper_tx_ssid_promo = 0;
static uint32_t wardrive_helper_tx_rssi = 0;
static uint32_t wardrive_helper_tx_refresh = 0;
static uint32_t wardrive_helper_tx_suppressed = 0;
static uint32_t wardrive_helper_stream_send_ok = 0;
static uint32_t wardrive_helper_stream_send_fail = 0;
static uint32_t peer_gps_stream_tx_ok = 0;
static uint32_t peer_gps_stream_tx_fail = 0;
static uint32_t peer_gps_stream_rx_packets = 0;
static uint32_t peer_gps_stream_rx_fix_packets = 0;
static TaskHandle_t peer_gps_stream_task_handle = NULL;
static StackType_t *peer_gps_stream_stack = NULL;
static StaticTask_t *peer_gps_stream_tcb = NULL;

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#define BLE_WD_SEEN_SIZE 64
static uint32_t ble_wd_seen_hashes[BLE_WD_SEEN_SIZE];
static uint16_t ble_wd_seen_idx = 0;
static uint32_t ble_wd_unique_count = 0;
static portMUX_TYPE ble_wd_mux = portMUX_INITIALIZER_UNLOCKED;

static uint32_t ble_wd_hash_mac(const uint8_t *addr) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; i++) {
        hash ^= addr[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

uint32_t ble_wardriving_get_unique_device_count(void) {
    uint32_t count;
    portENTER_CRITICAL(&ble_wd_mux);
    count = ble_wd_unique_count;
    portEXIT_CRITICAL(&ble_wd_mux);
    return count;
}

void ble_wardriving_reset_unique_device_count(void) {
    portENTER_CRITICAL(&ble_wd_mux);
    memset(ble_wd_seen_hashes, 0, sizeof(ble_wd_seen_hashes));
    ble_wd_seen_idx = 0;
    ble_wd_unique_count = 0;
    portEXIT_CRITICAL(&ble_wd_mux);
}
#endif

static void wardrive_heartbeat_cb(void *arg);
static void start_wardrive_heartbeat(void);
static void stop_wardrive_heartbeat(void);

static uint8_t wardrive_channels[WIFI_CHANNELS_MAX];
static uint8_t wardrive_channel_count = 0;
static uint8_t wardrive_channel_idx = 0;
static portMUX_TYPE wardrive_ch_mux = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    WARDRIVE_ROLE_PRIMARY = 0,
    WARDRIVE_ROLE_HELPER = 1,
} wardrive_role_t;

typedef enum {
    WD_AUTH_OPEN = 0,
    WD_AUTH_WEP = 1,
    WD_AUTH_WPA = 2,
    WD_AUTH_WPA2 = 3,
    WD_AUTH_WPA3 = 4,
    WD_AUTH_OWE = 5,
} wd_auth_t;

typedef struct {
    uint32_t hash;
    uint8_t bssid[6];
    int8_t best_rssi;
    uint32_t last_sent_ms;
    bool ssid_empty;
    bool used;
} wardrive_helper_dedupe_t;

typedef enum {
    WARDRIVE_OBS_LOCAL = 0,
    WARDRIVE_OBS_HELPER = 1,
    WARDRIVE_OBS_DRAIN_FENCE = 2,
    WARDRIVE_OBS_HELPER_TX = 3,
} wardrive_obs_source_t;

typedef struct {
    wardrive_obs_source_t source;
    uint32_t pending_token;
    bool has_gps_snapshot;
    bool using_peer_gps;
    gps_t gps_snapshot;
    wardriving_data_t data;
} wardrive_obs_item_t;

typedef struct {
    char bssid[18];
    uint32_t token;
    int rssi;
    bool used, queued, named, gps_valid;
} wardrive_pending_t;
// Reservations only cover queued observations, never successfully logged APs.
// A failed queue insertion or completed dequeue releases the reservation.
static wardrive_pending_t wardrive_pending[WARDRIVE_OBS_QUEUE_PSRAM_LEN];
static uint32_t wardrive_pending_token;
static uint32_t wardrive_pending_suppressed;
static StaticSemaphore_t wardrive_ack_storage;
static SemaphoreHandle_t wardrive_ack_sem;
static uint32_t wardrive_tx_session;
static uint16_t wardrive_tx_sequence;
static uint32_t wardrive_ack_session;
static uint16_t wardrive_ack_sequence;

static wardrive_role_t wardrive_role = WARDRIVE_ROLE_PRIMARY;
static volatile bool wardrive_peer_assist_active = false;
static bool wardrive_peer_assist_pending = false;
static uint32_t wardrive_peer_status_ms = 0;
static wardrive_helper_dedupe_t *wardrive_helper_dedupe = NULL;
static uint8_t wardrive_helper_dedupe_idx = 0;
static uint8_t wardrive_forced_helper_channels[WIFI_CHANNELS_MAX] = {0};
static uint8_t wardrive_forced_helper_channel_count = 0;
static uint8_t wardrive_primary_channels[WIFI_CHANNELS_MAX];
static uint8_t wardrive_primary_channel_count;
static uint8_t wardrive_peer_channels[WIFI_CHANNELS_MAX];
static uint8_t wardrive_peer_channel_count;
static bool wardrive_peer_plan_known;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32S3)
static bool wardrive_active_scan_enabled = true;
#else
static bool wardrive_active_scan_enabled = false;
#endif
static uint32_t wardrive_parser_rejected;
static uint32_t wardrive_channel_failures;
static uint32_t wardrive_probe_failures;
static uint32_t wardrive_worker_suppressed;
static uint16_t wardrive_helper_hop_override_ms = 0; // 0 = use local setting
static bool wardrive_weighted_5g_override = false;    // true if primary told us to use weighted
static QueueHandle_t wardrive_obs_queue = NULL;
static StaticQueue_t *wardrive_obs_queue_control = NULL;
static uint8_t *wardrive_obs_queue_storage = NULL;
static UBaseType_t wardrive_obs_queue_capacity = 0;
static bool wardrive_obs_queue_in_psram = false;
static TaskHandle_t wardrive_obs_task_handle = NULL;
static StaticTask_t *wardrive_obs_task_tcb = NULL;
static StackType_t *wardrive_obs_task_stack = NULL;
static SemaphoreHandle_t wardrive_obs_drain_sem = NULL;
static SemaphoreHandle_t wardrive_obs_lifecycle_mutex = NULL;
static bool wardrive_obs_accepting = false;
static uint32_t wardrive_obs_active_producers = 0;
static uint32_t wardrive_obs_enqueued = 0;
static uint32_t wardrive_obs_drop_local = 0;
static uint32_t wardrive_obs_drop_helper = 0;
static uint32_t wardrive_obs_drop_sink = 0;
static UBaseType_t wardrive_obs_high_water = 0;
static portMUX_TYPE wardrive_obs_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE wardrive_obs_init_mux = portMUX_INITIALIZER_UNLOCKED;
static bool wardrive_obs_init_in_progress = false;

static void wardrive_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static void gps_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static uint8_t wardrive_select_auth_code(const char *encryption_type);
static const char *wardrive_auth_code_to_string(uint8_t auth_code);
static uint32_t wardrive_hash_bssid(const uint8_t *bssid);
static bool wardrive_helper_should_send(const uint8_t *bssid, int8_t rssi, const char *ssid);
static bool wardrive_send_helper_observation(const uint8_t *bssid,
                                             uint8_t channel,
                                             int8_t rssi,
                                             uint8_t auth_code,
                                             const char *ssid);
static inline void wardrive_put_i32le(uint8_t *dst, int32_t value);
static inline void wardrive_put_i16le(uint8_t *dst, int16_t value);
static inline int32_t wardrive_get_i32le(const uint8_t *src);
static inline int16_t wardrive_get_i16le(const uint8_t *src);
static bool wardrive_is_valid_date(const gps_date_t *date);
static bool wardrive_is_valid_time(const gps_time_t *tim);
static inline uint32_t now_ms_u32(void);
static bool wardrive_send_peer_gps_stream(void);
static bool wardrive_send_helper_status(void);
static void peer_gps_stream_task(void *arg);
static uint32_t wardrive_get_hop_interval_ms(void);
static void wardrive_apply_hop_interval(void);
static uint8_t wardrive_build_full_channel_list(uint8_t *full_channels);
static bool wardrive_obs_queue_ensure(void);
static bool wardrive_obs_submit(const wardriving_data_t *data, wardrive_obs_source_t source);
static void wardrive_obs_session_stop_and_drain(void);
static void wardrive_obs_teardown_locked(void);
static void wardrive_process_helper_tx(const wardriving_data_t *data);
static void wardrive_active_result(const wifi_ap_record_t *record, void *ctx);

static void wardrive_obs_init_done(void) {
    portENTER_CRITICAL(&wardrive_obs_init_mux);
    wardrive_obs_init_in_progress = false;
    portEXIT_CRITICAL(&wardrive_obs_init_mux);
}

static void wardrive_obs_task(void *arg) {
    (void)arg;
    wardrive_obs_item_t item;
    uint8_t batch_count = 0;

    for (;;) {
        if (xQueueReceive(wardrive_obs_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (item.source == WARDRIVE_OBS_DRAIN_FENCE) {
            if (wardrive_obs_drain_sem) xSemaphoreGive(wardrive_obs_drain_sem);
            batch_count = 0;
            continue;
        }
        portENTER_CRITICAL(&wardrive_obs_mux);
        for (size_t i = 0; i < WARDRIVE_OBS_QUEUE_PSRAM_LEN; ++i) {
            if (wardrive_pending[i].used && wardrive_pending[i].token == item.pending_token) {
                wardrive_pending[i].used = false;
                break;
            }
        }
        portEXIT_CRITICAL(&wardrive_obs_mux);

        if (item.source == WARDRIVE_OBS_HELPER_TX) {
            wardrive_process_helper_tx(&item.data);
            continue;
        }
        // This may take the CSV mutex, so it belongs in the worker, never in
        // the Wi-Fi driver callback. Failed writes remain eligible for retry.
        if (!csv_wifi_ap_should_log_peek(item.data.bssid, item.data.rssi, item.data.ssid)) {
            wardrive_worker_suppressed++;
            continue;
        }

        wardrive_log_attempts++;
        esp_err_t err = ESP_ERR_INVALID_STATE;
        if (item.has_gps_snapshot) {
            for (uint8_t attempt = 0; attempt < 5; attempt++) {
                err = gps_manager_log_wardriving_data_with_snapshot(&item.data, &item.gps_snapshot,
                                                                    item.using_peer_gps);
                if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NO_MEM) break;
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (err == ESP_ERR_INVALID_STATE) {
            wardrive_gps_rejected++;
        } else if (err == ESP_OK) {
            wardrive_log_ok++;
            if (item.source == WARDRIVE_OBS_HELPER) wardrive_helper_merged_ok++;
        } else {
            portENTER_CRITICAL(&wardrive_obs_mux);
            wardrive_obs_drop_sink++;
            portEXIT_CRITICAL(&wardrive_obs_mux);
        }

        if (++batch_count >= 8) {
            batch_count = 0;
            vTaskDelay(1);
        }
    }
}

static bool wardrive_obs_queue_ensure(void) {
    for (;;) {
        portENTER_CRITICAL(&wardrive_obs_init_mux);
        if (wardrive_obs_queue && wardrive_obs_task_handle) {
            portEXIT_CRITICAL(&wardrive_obs_init_mux);
            return true;
        }
        if (!wardrive_obs_init_in_progress) {
            wardrive_obs_init_in_progress = true;
            portEXIT_CRITICAL(&wardrive_obs_init_mux);
            break;
        }
        portEXIT_CRITICAL(&wardrive_obs_init_mux);
        vTaskDelay(1);
    }

    UBaseType_t capacity = WARDRIVE_OBS_QUEUE_PSRAM_LEN;
    size_t storage_size = capacity * sizeof(wardrive_obs_item_t);
    uint8_t *storage = heap_caps_malloc(storage_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool in_psram = storage != NULL;
    if (!storage) {
        capacity = WARDRIVE_OBS_QUEUE_INTERNAL_LEN;
        storage_size = capacity * sizeof(wardrive_obs_item_t);
        storage = heap_caps_malloc(storage_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    StaticQueue_t *control = heap_caps_calloc(1, sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage || !control) {
        if (storage) heap_caps_free(storage);
        if (control) heap_caps_free(control);
        wardrive_obs_init_done();
        return false;
    }

    QueueHandle_t queue = xQueueCreateStatic(capacity, sizeof(wardrive_obs_item_t), storage, control);
    if (!queue) {
        heap_caps_free(storage);
        heap_caps_free(control);
        wardrive_obs_init_done();
        return false;
    }

    SemaphoreHandle_t drain_sem = xSemaphoreCreateBinary();
    SemaphoreHandle_t lifecycle_mutex = xSemaphoreCreateRecursiveMutex();
    if (!drain_sem || !lifecycle_mutex) {
        if (drain_sem) vSemaphoreDelete(drain_sem);
        if (lifecycle_mutex) vSemaphoreDelete(lifecycle_mutex);
        vQueueDelete(queue);
        heap_caps_free(storage);
        heap_caps_free(control);
        wardrive_obs_init_done();
        return false;
    }

    StackType_t *stack = NULL;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    stack = heap_caps_malloc(WARDRIVE_OBS_TASK_STACK_BYTES,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!stack) {
        stack = heap_caps_malloc(WARDRIVE_OBS_TASK_STACK_BYTES,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    StaticTask_t *tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stack || !tcb) {
        if (stack) heap_caps_free(stack);
        if (tcb) heap_caps_free(tcb);
        vSemaphoreDelete(drain_sem);
        vSemaphoreDelete(lifecycle_mutex);
        vQueueDelete(queue);
        heap_caps_free(storage);
        heap_caps_free(control);
        wardrive_obs_init_done();
        return false;
    }

    wardrive_obs_queue = queue;
    wardrive_obs_queue_control = control;
    wardrive_obs_queue_storage = storage;
    wardrive_obs_queue_capacity = capacity;
    wardrive_obs_queue_in_psram = in_psram;
    wardrive_obs_task_stack = stack;
    wardrive_obs_task_tcb = tcb;
    wardrive_obs_drain_sem = drain_sem;
    wardrive_obs_lifecycle_mutex = lifecycle_mutex;
    wardrive_obs_task_handle = xTaskCreateStatic(wardrive_obs_task, "wd_obs", WARDRIVE_OBS_TASK_STACK_BYTES,
                                                 NULL, 2, stack, tcb);
    if (!wardrive_obs_task_handle) {
        wardrive_obs_queue = NULL;
        vQueueDelete(queue);
        heap_caps_free(stack);
        heap_caps_free(tcb);
        heap_caps_free(storage);
        heap_caps_free(control);
        vSemaphoreDelete(drain_sem);
        vSemaphoreDelete(lifecycle_mutex);
        wardrive_obs_queue_control = NULL;
        wardrive_obs_queue_storage = NULL;
        wardrive_obs_task_stack = NULL;
        wardrive_obs_task_tcb = NULL;
        wardrive_obs_drain_sem = NULL;
        wardrive_obs_lifecycle_mutex = NULL;
        wardrive_obs_queue_capacity = 0;
        wardrive_obs_init_done();
        return false;
    }

    ESP_LOGI(TAG, "Wardrive observation queue: %u entries, %u bytes (%s), stack=%u bytes",
             (unsigned)capacity, (unsigned)storage_size, in_psram ? "PSRAM" : "internal RAM",
             (unsigned)WARDRIVE_OBS_TASK_STACK_BYTES);
    wardrive_obs_init_done();
    return true;
}

static bool wardrive_obs_submit(const wardriving_data_t *data, wardrive_obs_source_t source) {
    if (!data || source == WARDRIVE_OBS_DRAIN_FENCE) return false;

    portENTER_CRITICAL(&wardrive_obs_mux);
    bool accepted = wardrive_obs_accepting && wardrive_obs_queue != NULL;
    if (accepted) wardrive_obs_active_producers++;
    portEXIT_CRITICAL(&wardrive_obs_mux);
    if (!accepted) return false;

    wardrive_obs_item_t item = {.source = source, .data = *data};
    item.has_gps_snapshot = gps_manager_get_wardrive_snapshot(&item.gps_snapshot,
                                                                       &item.using_peer_gps);
    int slot = -1;
    portENTER_CRITICAL(&wardrive_obs_mux);
    for (size_t i = 0; i < WARDRIVE_OBS_QUEUE_PSRAM_LEN; ++i) {
        wardrive_pending_t *p = &wardrive_pending[i];
        if (!p->used) { if (slot < 0) slot = (int)i; continue; }
        if (strcmp(p->bssid, data->bssid) != 0) continue;
        if (p->queued && data->rssi <= p->rssi + 3 &&
            (p->named || !data->ssid[0]) && (p->gps_valid || !item.has_gps_snapshot)) {
            wardrive_pending_suppressed++;
            wardrive_obs_active_producers--;
            portEXIT_CRITICAL(&wardrive_obs_mux);
            return true;
        }
        slot = (int)i;
        break;
    }
    if (slot >= 0) {
        if (++wardrive_pending_token == 0) ++wardrive_pending_token;
        item.pending_token = wardrive_pending_token;
        wardrive_pending[slot] = (wardrive_pending_t){.used = true, .token = item.pending_token,
            .rssi = data->rssi, .named = data->ssid[0] != 0, .gps_valid = item.has_gps_snapshot};
        memcpy(wardrive_pending[slot].bssid, data->bssid, sizeof(data->bssid));
    }
    portEXIT_CRITICAL(&wardrive_obs_mux);
    // Driver scan records arrive in a burst. Allow their task to yield to the
    // logger; the Wi-Fi callback and GhostLink RX task remain nonblocking.
    TickType_t wait = wardrive_active_scan_enabled && source != WARDRIVE_OBS_HELPER ? pdMS_TO_TICKS(20) : 0;
    bool queued = xQueueSend(wardrive_obs_queue, &item, wait) == pdTRUE;
    UBaseType_t depth = queued ? uxQueueMessagesWaiting(wardrive_obs_queue) : 0;

    portENTER_CRITICAL(&wardrive_obs_mux);
    if (slot >= 0 && wardrive_pending[slot].used && wardrive_pending[slot].token == item.pending_token) {
        if (queued) wardrive_pending[slot].queued = true;
        else wardrive_pending[slot].used = false;
    }
    if (queued) {
        wardrive_obs_enqueued++;
        if (depth > wardrive_obs_high_water) wardrive_obs_high_water = depth;
    } else if (source == WARDRIVE_OBS_HELPER) {
        wardrive_obs_drop_helper++;
    } else {
        wardrive_obs_drop_local++;
    }
    wardrive_obs_active_producers--;
    portEXIT_CRITICAL(&wardrive_obs_mux);
    return queued;
}

static void wardrive_obs_session_stop_and_drain(void) {
    if (!wardrive_obs_lifecycle_mutex) return;
    xSemaphoreTakeRecursive(wardrive_obs_lifecycle_mutex, portMAX_DELAY);

    portENTER_CRITICAL(&wardrive_obs_mux);
    bool was_accepting = wardrive_obs_accepting;
    wardrive_obs_accepting = false;
    portEXIT_CRITICAL(&wardrive_obs_mux);

    if (!was_accepting) {
        xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
        return;
    }

    for (;;) {
        portENTER_CRITICAL(&wardrive_obs_mux);
        uint32_t active = wardrive_obs_active_producers;
        portEXIT_CRITICAL(&wardrive_obs_mux);
        if (active == 0) break;
        vTaskDelay(1);
    }

    if (wardrive_obs_queue && wardrive_obs_task_handle) {
        wardrive_obs_item_t fence = {.source = WARDRIVE_OBS_DRAIN_FENCE};
        (void)xSemaphoreTake(wardrive_obs_drain_sem, 0);
        if (xQueueSend(wardrive_obs_queue, &fence, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(wardrive_obs_drain_sem, portMAX_DELAY);
        }
    }

    glog("Wardrive queue: enqueued=%lu high=%u/%u dropped(local/helper/sink)=%lu/%lu/%lu storage=%s\n",
         (unsigned long)wardrive_obs_enqueued,
         (unsigned)wardrive_obs_high_water,
         (unsigned)wardrive_obs_queue_capacity,
         (unsigned long)wardrive_obs_drop_local,
         (unsigned long)wardrive_obs_drop_helper,
         (unsigned long)wardrive_obs_drop_sink,
         wardrive_obs_queue_in_psram ? "PSRAM" : "internal");
    wardrive_obs_teardown_locked();
    xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
}

// Free the observation queue + worker after a drained stop so ~8k stack +
// queue storage returns to the heap while wardriving sits idle. The next
// start_wardriving() recreates everything via wardrive_obs_queue_ensure().
// Caller must hold wardrive_obs_lifecycle_mutex (as stop_wardriving does);
// the mutex itself is kept for future sessions.
static void wardrive_obs_teardown_locked(void) {
    if (wardrive_obs_task_handle) {
        vTaskDelete(wardrive_obs_task_handle);
        wardrive_obs_task_handle = NULL;
    }
    if (wardrive_obs_queue) {
        vQueueDelete(wardrive_obs_queue);
        wardrive_obs_queue = NULL;
    }
    heap_caps_free(wardrive_obs_task_stack);
    heap_caps_free(wardrive_obs_task_tcb);
    heap_caps_free(wardrive_obs_queue_storage);
    heap_caps_free(wardrive_obs_queue_control);
    wardrive_obs_task_stack = NULL;
    wardrive_obs_task_tcb = NULL;
    wardrive_obs_queue_storage = NULL;
    wardrive_obs_queue_control = NULL;
    wardrive_obs_queue_capacity = 0;
    if (wardrive_obs_drain_sem) {
        vSemaphoreDelete(wardrive_obs_drain_sem);
        wardrive_obs_drain_sem = NULL;
    }
}

static uint8_t wardrive_select_auth_code(const char *encryption_type) {
    if (!encryption_type) {
        return WD_AUTH_OPEN;
    }
    if (strcmp(encryption_type, "WEP") == 0) {
        return WD_AUTH_WEP;
    }
    if (strcmp(encryption_type, "WPA") == 0) {
        return WD_AUTH_WPA;
    }
    if (strcmp(encryption_type, "WPA2") == 0) {
        return WD_AUTH_WPA2;
    }
    if (strcmp(encryption_type, "WPA3") == 0) {
        return WD_AUTH_WPA3;
    }
    if (strcmp(encryption_type, "OWE") == 0) {
        return WD_AUTH_OWE;
    }
    return WD_AUTH_OPEN;
}

static const char *wardrive_auth_code_to_string(uint8_t auth_code) {
    switch (auth_code) {
        case WD_AUTH_WEP:
            return "WEP";
        case WD_AUTH_WPA:
            return "WPA";
        case WD_AUTH_WPA2:
            return "WPA2";
        case WD_AUTH_WPA3:
            return "WPA3";
        case WD_AUTH_OWE:
            return "OWE";
        case WD_AUTH_OPEN:
        default:
            return "OPEN";
    }
}

static uint32_t wardrive_hash_bssid(const uint8_t *bssid) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 6; i++) {
        hash ^= bssid[i];
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

static inline void wardrive_put_i32le(uint8_t *dst, int32_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static inline void wardrive_put_i16le(uint8_t *dst, int16_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static inline int32_t wardrive_get_i32le(const uint8_t *src) {
    return (int32_t)((uint32_t)src[0] |
                     ((uint32_t)src[1] << 8) |
                     ((uint32_t)src[2] << 16) |
                     ((uint32_t)src[3] << 24));
}

static inline int16_t wardrive_get_i16le(const uint8_t *src) {
    return (int16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static bool wardrive_is_valid_date(const gps_date_t *date) {
    if (!date) {
        return false;
    }
    if (date->year > 99 || date->month < 1 || date->month > 12 || date->day < 1 || date->day > 31) {
        return false;
    }
    return true;
}

static bool wardrive_is_valid_time(const gps_time_t *tim) {
    if (!tim) {
        return false;
    }
    return tim->hour <= 23 && tim->minute <= 59 && tim->second <= 59;
}

static bool wardrive_helper_should_send(const uint8_t *bssid, int8_t rssi, const char *ssid) {
    if (!wardrive_helper_dedupe) {
        wardrive_helper_dedupe = mon_tbl_calloc(WARDRIVE_HELPER_DEDUPE_SIZE, sizeof(*wardrive_helper_dedupe));
        if (!wardrive_helper_dedupe) return true;
    }
    uint32_t hash = wardrive_hash_bssid(bssid);
    uint32_t now_ms = now_ms_u32();
    bool ssid_empty = (!ssid || ssid[0] == '\0');

    for (int i = 0; i < WARDRIVE_HELPER_DEDUPE_SIZE; i++) {
        wardrive_helper_dedupe_t *entry = &wardrive_helper_dedupe[i];
        if (entry->used && entry->hash == hash && memcmp(entry->bssid, bssid, 6) == 0) {
            if (entry->ssid_empty && !ssid_empty) {
                entry->ssid_empty = false;
                if (rssi > entry->best_rssi) {
                    entry->best_rssi = rssi;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_ssid_promo++;
                return true;
            }
            if (rssi > entry->best_rssi + 3) {
                entry->best_rssi = rssi;
                if (!ssid_empty) {
                    entry->ssid_empty = false;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_rssi++;
                return true;
            }
            if ((uint32_t)(now_ms - entry->last_sent_ms) >= WARDRIVE_HELPER_REFRESH_MS) {
                if (rssi > entry->best_rssi) {
                    entry->best_rssi = rssi;
                }
                if (!ssid_empty) {
                    entry->ssid_empty = false;
                }
                entry->last_sent_ms = now_ms;
                wardrive_helper_tx_refresh++;
                return true;
            }
            wardrive_helper_tx_suppressed++;
            return false;
        }
    }

    wardrive_helper_dedupe_t *slot = &wardrive_helper_dedupe[wardrive_helper_dedupe_idx];
    slot->used = true;
    slot->hash = hash;
    memcpy(slot->bssid, bssid, 6);
    slot->best_rssi = rssi;
    slot->last_sent_ms = now_ms;
    slot->ssid_empty = ssid_empty;
    wardrive_helper_dedupe_idx = (uint8_t)((wardrive_helper_dedupe_idx + 1) % WARDRIVE_HELPER_DEDUPE_SIZE);
    wardrive_helper_tx_new++;
    return true;
}

static bool wardrive_send_helper_observation(const uint8_t *bssid,
                                             uint8_t channel,
                                             int8_t rssi,
                                             uint8_t auth_code,
                                             const char *ssid) {
    if (!esp_comm_manager_is_connected()) {
        return false;
    }

    if (wardrive_primary_channel_count) {
        // New peers acknowledge queue acceptance. Every observation, including
        // a 32-byte SSID, fits one physical stream packet (max 49 bytes).
        uint8_t wire[49] = {WARDRIVE_STREAM_VERSION_ACKED, channel, (uint8_t)rssi, auth_code};
        memcpy(wire + 4, bssid, 6);
        size_t len = strnlen(ssid ? ssid : "", 32);
        wire[10] = (uint8_t)len;
        if (len) memcpy(wire + 11, ssid, len);
        uint16_t sequence = ++wardrive_tx_sequence;
        wardrive_put_i32le(wire + 11 + len, (int32_t)wardrive_tx_session);
        wardrive_put_i16le(wire + 15 + len, (int16_t)sequence);
        (void)xSemaphoreTake(wardrive_ack_sem, 0);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            if (!esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_WARDRIVE, wire, 17 + len, 5)) continue;
            if (xSemaphoreTake(wardrive_ack_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
                portENTER_CRITICAL(&wardrive_obs_mux);
                bool matched = wardrive_ack_session == wardrive_tx_session && wardrive_ack_sequence == sequence;
                portEXIT_CRITICAL(&wardrive_obs_mux);
                if (matched) return true;
            }
        }
        return false;
    }

    uint8_t ssid_len = 0;
    if (ssid) {
        size_t raw_len = strlen(ssid);
        if (raw_len > 32) {
            raw_len = 32;
        }
        ssid_len = (uint8_t)raw_len;
    }

    uint8_t payload[1 + 1 + 1 + 1 + 6 + 1 + 32 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 7] = {0};
    size_t pos = 0;
    payload[pos++] = WARDRIVE_STREAM_VERSION;
    payload[pos++] = channel;
    payload[pos++] = (uint8_t)rssi;
    payload[pos++] = auth_code;
    memcpy(payload + pos, bssid, 6);
    pos += 6;
    payload[pos++] = ssid_len;
    if (ssid_len > 0) {
        memcpy(payload + pos, ssid, ssid_len);
        pos += ssid_len;
    }

    uint8_t gps_flags = 0;
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    int16_t alt_dm = 0;
    uint8_t sats_in_use = 0;
    uint8_t sats_in_view = 0;
    uint8_t fix = (uint8_t)GPS_FIX_INVALID;
    uint8_t fix_mode = (uint8_t)GPS_MODE_INVALID;
    uint16_t hdop_x10 = 0;
    uint8_t gps_day = 0;
    uint8_t gps_month = 0;
    uint16_t gps_year = 0;
    uint8_t gps_hour = 0;
    uint8_t gps_minute = 0;
    uint8_t gps_second = 0;

    gps_t local_gps = {0};
    if (gps_manager_has_recent_update() && gps_manager_get_local_gps_snapshot(&local_gps)) {
        gps_flags |= WARDRIVE_STREAM_FLAG_GPS_PRESENT;
        sats_in_use = local_gps.sats_in_use;
        sats_in_view = local_gps.sats_in_view;
        fix = (uint8_t)local_gps.fix;
        fix_mode = (uint8_t)local_gps.fix_mode;

        if (local_gps.dop_h >= 0.0f && local_gps.dop_h <= 6553.5f) {
            hdop_x10 = (uint16_t)(local_gps.dop_h * 10.0f);
        }

        if (wardrive_is_valid_date(&local_gps.date)) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_DATE_VALID;
        }
        if (wardrive_is_valid_time(&local_gps.tim)) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_TIME_VALID;
        }
        gps_day = local_gps.date.day;
        gps_month = local_gps.date.month;
        gps_year = local_gps.date.year;
        gps_hour = local_gps.tim.hour;
        gps_minute = local_gps.tim.minute;
        gps_second = local_gps.tim.second;

        if (local_gps.valid && local_gps.fix >= GPS_FIX_GPS && local_gps.fix_mode >= GPS_MODE_2D &&
            local_gps.latitude >= -90.0f && local_gps.latitude <= 90.0f &&
            local_gps.longitude >= -180.0f && local_gps.longitude <= 180.0f) {
            gps_flags |= WARDRIVE_STREAM_FLAG_GPS_FIX;
            lat_e7 = (int32_t)(local_gps.latitude * 10000000.0f);
            lon_e7 = (int32_t)(local_gps.longitude * 10000000.0f);
            float alt = local_gps.altitude * 10.0f;
            if (alt > 32767.0f) {
                alt = 32767.0f;
            }
            if (alt < -32768.0f) {
                alt = -32768.0f;
            }
            alt_dm = (int16_t)alt;
        }
    }

    // A full SSID and embedded GPS do not fit one stream packet. Long SSIDs
    // use the existing independent GPS stream instead of being truncated or
    // fragmented into packets the receiver cannot reassemble.
    if (pos + 24 > 59) gps_flags = 0;
    payload[pos++] = gps_flags;
    if (gps_flags & WARDRIVE_STREAM_FLAG_GPS_PRESENT) {
        wardrive_put_i32le(payload + pos, lat_e7);
        pos += 4;
        wardrive_put_i32le(payload + pos, lon_e7);
        pos += 4;
        wardrive_put_i16le(payload + pos, alt_dm);
        pos += 2;
        payload[pos++] = sats_in_use;
        payload[pos++] = sats_in_view;
        payload[pos++] = fix;
        payload[pos++] = fix_mode;
        payload[pos++] = (uint8_t)(hdop_x10 & 0xFF);
        payload[pos++] = (uint8_t)((hdop_x10 >> 8) & 0xFF);
        payload[pos++] = gps_day;
        payload[pos++] = gps_month;
        wardrive_put_i16le(payload + pos, (int16_t)gps_year);
        pos += 2;
        payload[pos++] = gps_hour;
        payload[pos++] = gps_minute;
        payload[pos++] = gps_second;
    }

    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_WARDRIVE, payload, pos, 5)) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static bool wardrive_send_peer_gps_stream(void) {
    if (!esp_comm_manager_is_connected()) {
        return false;
    }
    if (!gps_manager_has_recent_update()) {
        return false;
    }

    gps_t gps_local = {0};
    if (!gps_manager_get_local_gps_snapshot(&gps_local)) {
        return false;
    }

    uint8_t payload[1 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 2 + 2 + 7] = {0};
    size_t pos = 0;
    payload[pos++] = GPS_STREAM_VERSION;

    uint8_t flags = GPS_STREAM_FLAG_PRESENT;
    bool has_fix = gps_local.valid &&
                   gps_local.fix >= GPS_FIX_GPS &&
                   gps_local.fix_mode >= GPS_MODE_2D &&
                   gps_local.latitude >= -90.0f && gps_local.latitude <= 90.0f &&
                   gps_local.longitude >= -180.0f && gps_local.longitude <= 180.0f;
    if (has_fix) {
        flags |= GPS_STREAM_FLAG_FIX;
    }
    if (wardrive_is_valid_date(&gps_local.date)) {
        flags |= GPS_STREAM_FLAG_DATE_VALID;
    }
    if (wardrive_is_valid_time(&gps_local.tim)) {
        flags |= GPS_STREAM_FLAG_TIME_VALID;
    }
    payload[pos++] = flags;

    int32_t lat_e7 = has_fix ? (int32_t)(gps_local.latitude * 10000000.0f) : 0;
    int32_t lon_e7 = has_fix ? (int32_t)(gps_local.longitude * 10000000.0f) : 0;
    float alt_dm_f = gps_local.altitude * 10.0f;
    if (alt_dm_f > 32767.0f) {
        alt_dm_f = 32767.0f;
    }
    if (alt_dm_f < -32768.0f) {
        alt_dm_f = -32768.0f;
    }
    int16_t alt_dm = (int16_t)alt_dm_f;

    uint16_t hdop_x10 = 0;
    if (isfinite(gps_local.dop_h) && gps_local.dop_h >= 0.0f && gps_local.dop_h <= 6553.5f) {
        hdop_x10 = (uint16_t)(gps_local.dop_h * 10.0f);
    }

    float speed_x100_f = gps_local.speed * 100.0f;
    if (!isfinite(speed_x100_f)) {
        speed_x100_f = 0.0f;
    }
    if (speed_x100_f > 32767.0f) {
        speed_x100_f = 32767.0f;
    }
    if (speed_x100_f < -32768.0f) {
        speed_x100_f = -32768.0f;
    }
    int16_t speed_x100 = (int16_t)speed_x100_f;

    float course_x100_f = gps_local.cog * 100.0f;
    if (!isfinite(course_x100_f)) {
        course_x100_f = 0.0f;
    }
    if (course_x100_f > 32767.0f) {
        course_x100_f = 32767.0f;
    }
    if (course_x100_f < -32768.0f) {
        course_x100_f = -32768.0f;
    }
    int16_t course_x100 = (int16_t)course_x100_f;

    wardrive_put_i32le(payload + pos, lat_e7);
    pos += 4;
    wardrive_put_i32le(payload + pos, lon_e7);
    pos += 4;
    wardrive_put_i16le(payload + pos, alt_dm);
    pos += 2;
    payload[pos++] = gps_local.sats_in_use;
    payload[pos++] = gps_local.sats_in_view;
    payload[pos++] = (uint8_t)gps_local.fix;
    payload[pos++] = (uint8_t)gps_local.fix_mode;
    wardrive_put_i16le(payload + pos, (int16_t)hdop_x10);
    pos += 2;
    wardrive_put_i16le(payload + pos, speed_x100);
    pos += 2;
    wardrive_put_i16le(payload + pos, course_x100);
    pos += 2;
    payload[pos++] = gps_local.date.day;
    payload[pos++] = gps_local.date.month;
    wardrive_put_i16le(payload + pos, (int16_t)gps_local.date.year);
    pos += 2;
    payload[pos++] = gps_local.tim.hour;
    payload[pos++] = gps_local.tim.minute;
    payload[pos++] = gps_local.tim.second;

    return esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_GPS, payload, pos);
}

static void peer_gps_stream_task(void *arg) {
    (void)arg;
    int64_t last_init_try_ms = 0;
    gps_t gps_local = {0};

    while (1) {
        if (esp_comm_manager_is_connected()) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
            if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0 &&
                !g_gpsManager.isinitilized) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                if ((now_ms - last_init_try_ms) >= PEER_GPS_INIT_RETRY_MS) {
                    gps_manager_init(&g_gpsManager);
                    last_init_try_ms = now_ms;
                }
            }
#endif

            if (gps_manager_has_recent_update() && gps_manager_get_local_gps_snapshot(&gps_local)) {
                if (wardrive_send_peer_gps_stream()) {
                    peer_gps_stream_tx_ok++;
                } else {
                    peer_gps_stream_tx_fail++;
                }
            }
            if (wardrive_role == WARDRIVE_ROLE_HELPER) {
                (void)wardrive_send_helper_status();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(PEER_GPS_STREAM_INTERVAL_MS));
    }
}

static uint32_t wardrive_get_hop_interval_ms(void) {
    uint32_t interval_ms;
    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        interval_ms = wardrive_helper_hop_override_ms > 0
            ? wardrive_helper_hop_override_ms
            : settings_get_wd_hop_helper_ms(&G_Settings);
    } else {
        interval_ms = settings_get_wd_hop_primary_ms(&G_Settings);
    }
    if (interval_ms < 40) {
        interval_ms = 40;
    }
    if (interval_ms > 1000) {
        interval_ms = 1000;
    }
    return interval_ms;
}

static bool wardrive_send_helper_status(void) {
    if (wardrive_role != WARDRIVE_ROLE_HELPER || !wardriving_hopping_active ||
        !esp_comm_manager_is_connected()) return false;
    gps_t gps = {0};
    bool gps_ready = gps_manager_has_recent_update() && gps_manager_get_local_gps_snapshot(&gps) &&
                     gps.valid && gps.fix >= GPS_FIX_GPS && gps.fix_mode >= GPS_MODE_2D && gps.sats_in_use >= 3;
    uint16_t hop_ms = (uint16_t)wardrive_get_hop_interval_ms();
    uint8_t payload[8 + WIFI_CHANNELS_MAX] = {
        WARDRIVE_CONTROL_MARKER,
        wardrive_primary_channel_count ? WARDRIVE_CONTROL_VERSION_PLAN : WARDRIVE_CONTROL_VERSION,
        WARDRIVE_CONTROL_HELPER_READY, 0,
        (uint8_t)hop_ms, (uint8_t)(hop_ms >> 8), gps_ready ? 1 : 0,
        wardrive_active_scan_enabled ? 1 : 0,
    };
    uint8_t count = 0;
    portENTER_CRITICAL(&wardrive_ch_mux);
    for (uint8_t i = 0; i < wardrive_channel_count; ++i)
        if (!wd_plan_contains(payload + 8, count, wardrive_channels[i]))
            payload[8 + count++] = wardrive_channels[i];
    portEXIT_CRITICAL(&wardrive_ch_mux);
    payload[3] = count;
    return esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_WARDRIVE, payload,
                                       wardrive_primary_channel_count ? 8 + count : 7);
}

static void wardrive_apply_hop_interval(void) {
    if (wardrive_active_scan_enabled) {
        if (wardriving_hopping_active)
            wardrive_scan_set_plan(wardrive_channels, wardrive_channel_count, wardrive_get_hop_interval_ms());
        return;
    }
    if (!wardriving_hopping_active || wardrive_hop_timer == NULL) {
        return;
    }

    uint32_t interval_ms = wardrive_get_hop_interval_ms();
    (void)esp_timer_stop(wardrive_hop_timer);
    esp_err_t err = esp_timer_start_periodic(wardrive_hop_timer, (uint64_t)interval_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wardrive: failed to apply hop interval (%s)", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Wardrive: hop interval now %lu ms", (unsigned long)interval_ms);
}

static uint8_t wardrive_build_full_channel_list(uint8_t *full_channels) {
    if (!full_channels) {
        return 0;
    }

    uint8_t full_count = wifi_channels_build_country_list(full_channels, WIFI_CHANNELS_MAX);
    if (full_count == 0) {
        uint8_t fallback[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,
#if defined(CONFIG_IDF_TARGET_ESP32C5)
            36,40,44,48,52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144,149,153,157,161,165};
#else
        };
#endif
        uint8_t count = sizeof(fallback);
        if (count > WIFI_CHANNELS_MAX) count = WIFI_CHANNELS_MAX;
        memcpy(full_channels, fallback, count);
        full_count = count;
    }

    return full_count;
}

static bool wardrive_is_common_5g_channel(uint8_t ch) {
    return (ch >= 36 && ch <= 48) || (ch >= 149 && ch <= 165);
}

static void wardrive_build_channel_list(void) {
    uint8_t full[WIFI_CHANNELS_MAX] = {0}, base[WIFI_CHANNELS_MAX] = {0};
    size_t profile_count = 0;
    hop_profile_resolve(full, sizeof(full), &profile_count);
    uint8_t full_count = profile_count ? (uint8_t)profile_count : wardrive_build_full_channel_list(full);
    uint8_t count = 0;
    if (wardrive_role == WARDRIVE_ROLE_HELPER && wardrive_primary_channel_count) {
        count = (uint8_t)wd_plan_helper(wardrive_primary_channels, wardrive_primary_channel_count,
                                       full, full_count, base);
    } else if (wardrive_role == WARDRIVE_ROLE_HELPER && wardrive_forced_helper_channel_count) {
        // A remote assignment takes precedence over local profile weighting,
        // but can never make this radio scan an unsupported/country-excluded channel.
        uint8_t supported[WIFI_CHANNELS_MAX];
        uint8_t supported_count = wardrive_build_full_channel_list(supported);
        for (uint8_t i = 0; i < wardrive_forced_helper_channel_count; ++i) {
            uint8_t ch = wardrive_forced_helper_channels[i];
            if (wd_plan_contains(supported, supported_count, ch)) base[count++] = ch;
        }
    } else if (wardrive_role == WARDRIVE_ROLE_PRIMARY && wardrive_peer_assist_active) {
        if (wardrive_peer_plan_known) {
            count = (uint8_t)wd_plan_remaining(full, full_count, wardrive_peer_channels, wardrive_peer_channel_count, base);
        } else {
            // Legacy helper only acknowledges a count, not its actual channels.
            // Keep complete local coverage instead of assuming a successful split.
            memcpy(base, full, full_count); count = full_count;
        }
    } else {
        memcpy(base, full, full_count); count = full_count;
    }
    // If there is no distinct work (e.g. one shared channel), overlap safely.
    if (!count) { memcpy(base, full, full_count); count = full_count; }
    uint8_t scheduled[WIFI_CHANNELS_MAX];
    uint8_t n = 0, bonus[8], bonus_count = 0, bonus_index = 0;
    bool weighted = wardrive_role == WARDRIVE_ROLE_HELPER ? wardrive_weighted_5g_override :
                    settings_get_wd_weighted_5g(&G_Settings);
    if (weighted && !profile_count) {
        for (uint8_t i = 0; i < count && bonus_count < sizeof(bonus) &&
             count + bonus_count < WIFI_CHANNELS_MAX; ++i) {
            if (base[i] == 1 || base[i] == 6 || base[i] == 11 || wardrive_is_common_5g_channel(base[i]))
                bonus[bonus_count++] = base[i];
        }
    }
    for (uint8_t i = 0; i < count; ++i) {
        scheduled[n++] = base[i];
        if ((i + 1) % 4 == 0 && bonus_index < bonus_count) scheduled[n++] = bonus[bonus_index++];
    }
    while (bonus_index < bonus_count) scheduled[n++] = bonus[bonus_index++];
    portENTER_CRITICAL(&wardrive_ch_mux);
    memcpy(wardrive_channels, scheduled, n);
    wardrive_channel_count = n;
    wardrive_channel_idx = 0;
    portEXIT_CRITICAL(&wardrive_ch_mux);
    if (wardrive_active_scan_enabled && wardriving_hopping_active)
        wardrive_scan_set_plan(scheduled, n, wardrive_get_hop_interval_ms());
    ESP_LOGI(TAG, "Wardrive plan: %s backend=%s distinct=%u visits=%u peer=%s",
             wardrive_role == WARDRIVE_ROLE_HELPER ? "helper" : "primary",
             wardrive_active_scan_enabled ? "active" : "monitor", count, n,
             wardrive_peer_assist_active ? "ready" : "off");
}

static uint8_t wardrive_parse_channel_csv(const char *csv, uint8_t *out, uint8_t out_max) {
    if (!csv || !out || out_max == 0) {
        return 0;
    }

    char buf[192];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint8_t count = 0;
    char *tok = strtok(buf, ",");
    while (tok && count < out_max) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) {
            end--;
        }
        *end = '\0';

        if (*tok != '\0') {
            char *num_end = NULL;
            long ch = strtol(tok, &num_end, 10);
            if (num_end != tok && *num_end == '\0' && ch >= 1 && ch <= MAX_WIFI_CHANNEL) {
                bool exists = false;
                for (uint8_t i = 0; i < count; i++) {
                    if (out[i] == (uint8_t)ch) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    out[count++] = (uint8_t)ch;
                }
            }
        }

        tok = strtok(NULL, ",");
    }

    return count;
}

static uint32_t hash_ssid(const char *ssid);
static bool ssid_hash_exists(pineap_network_t *network, uint32_t hash);
static int build_recent_ssids_string(const pineap_network_t *network, char *out, size_t out_size);
static void log_pineap_details(pineap_network_t *network,
                               const char *title,
                               const char *ssids_str,
                               int ssid_count);
static bool is_pineapple_oui(const uint8_t *bssid);
static void trim_trailing(char *str);
static bool compare_bssid(const uint8_t *bssid1, const uint8_t *bssid2);
static bool is_beacon_packet(const wifi_promiscuous_pkt_t *pkt);
static pineap_network_t *find_or_create_network(const uint8_t *bssid);
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#endif

// handshake pairing and limited beacon emission for eapol capture
typedef struct {
    uint8_t ap[6];
    uint8_t sta[6];
    uint64_t replay;
    uint8_t ap_msg;   // 0=unknown, 1..4=M1..M4
    uint8_t sta_msg;  // 0=unknown, 1..4=M1..M4
} hs_entry_t;

#define HS_TABLE_MAX 16
static hs_entry_t *hs_table = NULL;
static uint8_t hs_count_local = 0;
static uint8_t hs_insert_idx_local = 0;
static uint32_t hs_found_count = 0;
static portMUX_TYPE hs_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_pcap_enabled = true;

static inline bool mac_equal(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, 6) == 0;
}

uint32_t wifi_callbacks_get_handshake_count(void) {
    uint32_t count;
    portENTER_CRITICAL(&hs_mux);
    count = hs_found_count;
    portEXIT_CRITICAL(&hs_mux);
    return count;
}

void wifi_callbacks_reset_handshake_tracking(void) {
    portENTER_CRITICAL(&hs_mux);
    if (hs_table) {
        memset(hs_table, 0, HS_TABLE_MAX * sizeof(*hs_table));
    }
    hs_count_local = 0;
    hs_insert_idx_local = 0;
    hs_found_count = 0;
    portEXIT_CRITICAL(&hs_mux);
}

void wifi_callbacks_set_pcap_enabled(bool enabled) {
    s_pcap_enabled = enabled;
}

static const char *msg_name(uint8_t m) {
    switch (m) { case 1: return "M1"; case 2: return "M2"; case 3: return "M3"; case 4: return "M4"; default: return "M?"; }
}

static void process_eapol_candidate_pair(const uint8_t *ap,
                                         const uint8_t *sta,
                                         uint64_t replay,
                                         bool from_ap,
                                         uint8_t msg_type) {
    bool log_handshake = false;
    char log_ap_str[18];
    uint8_t log_ap_msg = 0;
    uint8_t log_sta_msg = 0;

    if (!hs_table) {
        hs_table = mon_tbl_calloc(HS_TABLE_MAX, sizeof(*hs_table));
        if (!hs_table) return;
    }
    portENTER_CRITICAL(&hs_mux);
    for (uint8_t i = 0; i < hs_count_local; i++) {
        hs_entry_t *e = &hs_table[i];
        if (mac_equal(e->ap, ap) && mac_equal(e->sta, sta) && e->replay == replay) {
            if (from_ap) e->ap_msg = msg_type; else e->sta_msg = msg_type;
            if (e->ap_msg && e->sta_msg) {
                hs_found_count++;
                snprintf(log_ap_str, sizeof(log_ap_str), "%02x:%02x:%02x:%02x:%02x:%02x",
                         e->ap[0], e->ap[1], e->ap[2], e->ap[3], e->ap[4], e->ap[5]);
                log_ap_msg = e->ap_msg;
                log_sta_msg = e->sta_msg;
                log_handshake = true;
                // reset to avoid duplicate notifications for same replay
                e->ap_msg = 0;
                e->sta_msg = 0;
            }
            portEXIT_CRITICAL(&hs_mux);
            if (log_handshake) {
                glog("Handshake found!\nAP=%s\nPair=%s/%s\n",
                     log_ap_str, msg_name(log_ap_msg), msg_name(log_sta_msg));
                char hs_payload[40];
                snprintf(hs_payload, sizeof(hs_payload), "%s|%s/%s",
                         log_ap_str, msg_name(log_ap_msg), msg_name(log_sta_msg));
                ghostscript_emit_event("handshake_captured", hs_payload);
            }
            return;
        }
    }
    uint8_t idx;
    if (hs_count_local < HS_TABLE_MAX) {
        idx = hs_count_local++;
    } else {
        idx = hs_insert_idx_local;
        hs_insert_idx_local = (hs_insert_idx_local + 1) % HS_TABLE_MAX;
    }
    hs_entry_t *ne = &hs_table[idx];
    memcpy(ne->ap, ap, 6);
    memcpy(ne->sta, sta, 6);
    ne->replay = replay;
    ne->ap_msg = from_ap ? msg_type : 0;
    ne->sta_msg = from_ap ? 0 : msg_type;
    portEXIT_CRITICAL(&hs_mux);
}

typedef struct {
    uint8_t bssid[6];
    uint8_t emitted;
    bool saw_nonempty_ssid;
} beacon_limiter_t;

#define BEACON_LIMIT_MAX 64
#define BEACON_MAX_PER_BSSID 3
static beacon_limiter_t *beacon_limits = NULL;
static uint8_t beacon_limit_count = 0;
static uint8_t beacon_limit_insert = 0;

// probe request dedupe to keep files small
#define PROBE_DEDUPE_MAX 64
typedef struct {
    uint8_t src[6];
    uint32_t ssid_hash;
    uint64_t last_ms;
} probe_dedupe_t;
static probe_dedupe_t *probe_dedupe_tbl = NULL;
static uint8_t probe_dedupe_count = 0;
static uint8_t probe_dedupe_insert = 0;

static bool probe_should_emit(const uint8_t *src, uint32_t ssid_hash, uint64_t now_ms) {
    if (!probe_dedupe_tbl) {
        probe_dedupe_tbl = mon_tbl_calloc(PROBE_DEDUPE_MAX, sizeof(*probe_dedupe_tbl));
        if (!probe_dedupe_tbl) return true;
    }
    for (uint8_t i = 0; i < probe_dedupe_count; i++) {
        probe_dedupe_t *e = &probe_dedupe_tbl[i];
        if (memcmp(e->src, src, 6) == 0 && e->ssid_hash == ssid_hash) {
            if (now_ms - e->last_ms < PROBE_DEDUPE_TIMEOUT_MS) {
                return false;
            }
            e->last_ms = now_ms;
            return true;
        }
    }
    uint8_t idx;
    if (probe_dedupe_count < PROBE_DEDUPE_MAX) {
        idx = probe_dedupe_count++;
    } else {
        idx = probe_dedupe_insert;
        probe_dedupe_insert = (probe_dedupe_insert + 1) % PROBE_DEDUPE_MAX;
    }
    probe_dedupe_t *ne = &probe_dedupe_tbl[idx];
    memcpy(ne->src, src, 6);
    ne->ssid_hash = ssid_hash;
    ne->last_ms = now_ms;
    return true;
}

static bool beacon_should_emit_limited(const uint8_t *bssid, bool ssid_has_text) {
    if (!beacon_limits) {
        beacon_limits = mon_tbl_calloc(BEACON_LIMIT_MAX, sizeof(*beacon_limits));
        if (!beacon_limits) return true;
    }
    for (uint8_t i = 0; i < beacon_limit_count; i++) {
        if (mac_equal(beacon_limits[i].bssid, bssid)) {
            if (beacon_limits[i].emitted >= BEACON_MAX_PER_BSSID) {
                if (!beacon_limits[i].saw_nonempty_ssid && ssid_has_text) {
                    beacon_limits[i].saw_nonempty_ssid = true;
                    return true;
                }
                return false;
            }
            beacon_limits[i].emitted++;
            if (ssid_has_text) beacon_limits[i].saw_nonempty_ssid = true;
            return true;
        }
    }
    uint8_t idx;
    if (beacon_limit_count < BEACON_LIMIT_MAX) {
        idx = beacon_limit_count++;
    } else {
        idx = beacon_limit_insert;
        beacon_limit_insert = (beacon_limit_insert + 1) % BEACON_LIMIT_MAX;
    }
    memcpy(beacon_limits[idx].bssid, bssid, 6);
    beacon_limits[idx].emitted = 1;
    beacon_limits[idx].saw_nonempty_ssid = ssid_has_text;
    return true;
}

// queued writer to avoid heavy work in promiscuous callback
typedef struct {
    uint16_t length;
    uint8_t data[768];
    bool in_use;
} pcap_pool_slot_t;

typedef struct {
    uint8_t slot_idx;
    pcap_capture_type_t cap_type;
} pcap_q_item_t;

#define EAPOL_Q_LEN 64
#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(GHOSTESP_NO_NATIVE_BLE)
#define PCAP_POOL_SLOTS_DEFAULT 10
#define PCAP_POOL_SLOTS_MIN 4
#else
#define PCAP_POOL_SLOTS_DEFAULT 16
#define PCAP_POOL_SLOTS_MIN 8
#endif
static QueueHandle_t s_pcap_q = NULL;
static TaskHandle_t s_pcap_writer_task = NULL;
static pcap_pool_slot_t *s_pcap_pool = NULL;
static size_t s_pcap_pool_slots = 0;
static portMUX_TYPE s_pcap_pool_lock = portMUX_INITIALIZER_UNLOCKED;

static bool pcap_pool_init(void) {
    if (s_pcap_pool != NULL && s_pcap_pool_slots > 0) {
        return true;
    }

    size_t slots = PCAP_POOL_SLOTS_DEFAULT;
    while (slots >= PCAP_POOL_SLOTS_MIN) {
        pcap_pool_slot_t *pool = (pcap_pool_slot_t *)heap_caps_calloc(slots, sizeof(pcap_pool_slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!pool) {
            pool = (pcap_pool_slot_t *)heap_caps_calloc(slots, sizeof(pcap_pool_slot_t), MALLOC_CAP_8BIT);
        }
        if (pool != NULL) {
            s_pcap_pool = pool;
            s_pcap_pool_slots = slots;
            ESP_LOGI(TAG, "PCAP pool allocated: %lu slots (%lu bytes)",
                     (unsigned long)s_pcap_pool_slots,
                     (unsigned long)(s_pcap_pool_slots * sizeof(pcap_pool_slot_t)));
            return true;
        }
        if (slots == PCAP_POOL_SLOTS_MIN) {
            break;
        }
        slots = (slots > 2) ? (slots - 2) : PCAP_POOL_SLOTS_MIN;
        if (slots < PCAP_POOL_SLOTS_MIN) {
            slots = PCAP_POOL_SLOTS_MIN;
        }
    }

    ESP_LOGE(TAG, "PCAP pool allocation failed");
    return false;
}

static int pcap_pool_acquire_slot(void) {
    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0) {
        return -1;
    }

    int slot = -1;
    taskENTER_CRITICAL(&s_pcap_pool_lock);
    for (size_t i = 0; i < s_pcap_pool_slots; i++) {
        if (!s_pcap_pool[i].in_use) {
            s_pcap_pool[i].in_use = true;
            slot = (int)i;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_pcap_pool_lock);
    return slot;
}

static void pcap_pool_release_slot(uint8_t slot_idx) {
    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0 || slot_idx >= s_pcap_pool_slots) {
        return;
    }

    taskENTER_CRITICAL(&s_pcap_pool_lock);
    s_pcap_pool[slot_idx].in_use = false;
    s_pcap_pool[slot_idx].length = 0;
    taskEXIT_CRITICAL(&s_pcap_pool_lock);
}

static void pcap_writer_task(void *arg) {
    (void)arg;
    pcap_q_item_t item;
    uint32_t processed = 0;
    for (;;) {
        if (xQueueReceive(s_pcap_q, &item, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (s_pcap_pool != NULL && item.slot_idx < s_pcap_pool_slots) {
                pcap_pool_slot_t *slot = &s_pcap_pool[item.slot_idx];
                if (slot->length > 0) {
                    pcap_write_packet_to_buffer(slot->data, slot->length, item.cap_type);
                }
                pcap_pool_release_slot(item.slot_idx);
            }
            processed++;
            if ((processed & 0xFF) == 0) { // log occasionally to avoid spam
                UBaseType_t hwm_words = uxTaskGetStackHighWaterMark(NULL);
                glog("PCAP writer HWM (bytes): %lu\n", (unsigned long)hwm_words);
            }
            if ((processed & 0x1F) == 0 && pcap_auto_flush_enabled()) {
                pcap_flush_buffer_to_file();
            }
        } else {
            // periodic flush even if idle
            if (pcap_auto_flush_enabled()) {
                pcap_flush_buffer_to_file();
            }
        }
    }
}

static inline void ensure_pcap_queue_started(void) {
    if (s_pcap_q != NULL) {
        return;
    }

    if (!pcap_pool_init()) {
        return;
    }

    s_pcap_q = xQueueCreate(EAPOL_Q_LEN, sizeof(pcap_q_item_t));
    if (s_pcap_q != NULL && s_pcap_writer_task == NULL) {
        xTaskCreate_psram(pcap_writer_task, "pcap_wr", 3072, NULL, 5, &s_pcap_writer_task);
    }
}

static inline void enqueue_pcap_write_typed(const uint8_t *payload, uint16_t len, pcap_capture_type_t cap_type) {
    if (!payload || len == 0) return;
    ensure_pcap_queue_started();
    if (!s_pcap_q) return;

    if (s_pcap_pool == NULL || s_pcap_pool_slots == 0) {
        return;
    }

    if (len > sizeof(s_pcap_pool[0].data)) {
        return;
    }

    int slot = pcap_pool_acquire_slot();
    if (slot < 0) {
        return;
    }

    pcap_pool_slot_t *pool_slot = &s_pcap_pool[slot];
    pool_slot->length = len;
    memcpy(pool_slot->data, payload, len);

    pcap_q_item_t item = {0};
    item.cap_type = cap_type;
    item.slot_idx = (uint8_t)slot;

    if (xQueueSend(s_pcap_q, &item, 0) != pdTRUE) {
        pcap_pool_release_slot((uint8_t)slot);
    }
}

static inline void enqueue_pcap_write(const uint8_t *payload, uint16_t len) {
    if (!s_pcap_enabled) return;
    enqueue_pcap_write_typed(payload, len, PCAP_CAPTURE_WIFI);
}

static wifi_raw_observer_t s_wifi_raw_observer = NULL;

void wifi_raw_set_observer(wifi_raw_observer_t observer) {
    s_wifi_raw_observer = observer;
}

// cleanup function to free pcap queue and task when not capturing
void cleanup_pcap_queue(void) {
    if (s_pcap_writer_task != NULL) {
        vTaskDeleteWithCaps(s_pcap_writer_task);
        s_pcap_writer_task = NULL;
    }
    if (s_pcap_q != NULL) {
        // drain any remaining items and release pool slots
        pcap_q_item_t item;
        while (xQueueReceive(s_pcap_q, &item, 0) == pdTRUE) {
            pcap_pool_release_slot(item.slot_idx);
        }
        vQueueDelete(s_pcap_q);
        s_pcap_q = NULL;
    }

    if (s_pcap_pool != NULL) {
        pcap_pool_slot_t *pool_to_free = NULL;
        taskENTER_CRITICAL(&s_pcap_pool_lock);
        pool_to_free = s_pcap_pool;
        s_pcap_pool = NULL;
        s_pcap_pool_slots = 0;
        taskEXIT_CRITICAL(&s_pcap_pool_lock);
        heap_caps_free(pool_to_free);
    }
}

static const char *suspicious_names[] STORE_DATA_ATTR = {
    "HC-03", "HC-05", "HC-06",  "HC-08",    "BT-HC05", "JDY-31",
    "AT-09", "HM-10", "CC41-A", "MLT-BT05", "SPP-CA",  "FFD0"};

static wps_network_t *detected_wps_networks = NULL;
int detected_network_count = 0;
static char *last_probe_log = NULL;
static uint64_t last_probe_log_time_ms = 0;

/* Heap-on-demand monitor tables (~5KB internal when idle). Backing arrays are
 * allocated at monitor/wardrive session start and released at stop; RX paths
 * NULL-check and degrade gracefully (emit untracked), so OOM can never
 * corrupt a capture. Freeing happens strictly after promiscuous delivery
 * stops; detach-then-free mirrors the s_pcap_pool pattern in this file. */
static portMUX_TYPE s_mon_tbl_lock = portMUX_INITIALIZER_UNLOCKED;

void wifi_callbacks_monitor_tables_ensure(void) {
    if (!hs_table) {
        hs_table = mon_tbl_calloc(HS_TABLE_MAX, sizeof(*hs_table));
        if (!hs_table) ESP_LOGW(TAG, "hs_table alloc failed; handshake tracking degraded");
    }
    if (!probe_dedupe_tbl) {
        probe_dedupe_tbl = mon_tbl_calloc(PROBE_DEDUPE_MAX, sizeof(*probe_dedupe_tbl));
        if (!probe_dedupe_tbl) ESP_LOGW(TAG, "probe_dedupe_tbl alloc failed; probe dedupe degraded");
    }
    if (!beacon_limits) {
        beacon_limits = mon_tbl_calloc(BEACON_LIMIT_MAX, sizeof(*beacon_limits));
        if (!beacon_limits) ESP_LOGW(TAG, "beacon_limits alloc failed; beacon limiting degraded");
    }
    if (!detected_wps_networks) {
        detected_wps_networks = mon_tbl_calloc(MAX_WPS_NETWORKS, sizeof(*detected_wps_networks));
        if (!detected_wps_networks) ESP_LOGW(TAG, "WPS table alloc failed; WPS detection degraded");
    }
    if (!wardrive_helper_dedupe) {
        wardrive_helper_dedupe = mon_tbl_calloc(WARDRIVE_HELPER_DEDUPE_SIZE, sizeof(*wardrive_helper_dedupe));
        if (!wardrive_helper_dedupe) ESP_LOGW(TAG, "wardrive dedupe alloc failed; helper suppression degraded");
    }
    if (!last_probe_log) {
        last_probe_log = mon_tbl_calloc(128, 1);
    }
}

void wifi_callbacks_monitor_tables_release(void) {
    hs_entry_t *hs;
    probe_dedupe_t *probe;
    beacon_limiter_t *beacon;
    wps_network_t *wps;
    wardrive_helper_dedupe_t *wd;
    char *lp;
    portENTER_CRITICAL(&hs_mux);
    hs = hs_table;
    hs_table = NULL;
    portEXIT_CRITICAL(&hs_mux);
    taskENTER_CRITICAL(&s_mon_tbl_lock);
    probe = probe_dedupe_tbl;
    probe_dedupe_tbl = NULL;
    beacon = beacon_limits;
    beacon_limits = NULL;
    wps = detected_wps_networks;
    detected_wps_networks = NULL;
    wd = wardrive_helper_dedupe;
    wardrive_helper_dedupe = NULL;
    lp = last_probe_log;
    last_probe_log = NULL;
    taskEXIT_CRITICAL(&s_mon_tbl_lock);
    heap_caps_free(hs);
    heap_caps_free(probe);
    heap_caps_free(beacon);
    heap_caps_free(wps);
    heap_caps_free(wd);
    heap_caps_free(lp);
}

static void wardrive_dedupe_release(void) {
    wardrive_helper_dedupe_t *wd;
    taskENTER_CRITICAL(&s_mon_tbl_lock);
    wd = wardrive_helper_dedupe;
    wardrive_helper_dedupe = NULL;
    taskEXIT_CRITICAL(&s_mon_tbl_lock);
    heap_caps_free(wd);
}
esp_timer_handle_t stop_timer;
int should_store_wps = 1;
gps_t *gps = NULL;
static bool gps_time_synced = false;

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static bool gps_build_utc_timeval(const gps_t *g, struct timeval *out) {
    if (!g || !out) {
        return false;
    }
    if (!gps_is_valid_year(g->date.year)) {
        return false;
    }
    if (g->date.month < 1 || g->date.month > 12 || g->date.day < 1 || g->date.day > 31) {
        return false;
    }
    if (g->tim.hour > 23 || g->tim.minute > 59 || g->tim.second > 59) {
        return false;
    }

    const int year = (int)gps_get_absolute_year(g->date.year);
    const int64_t days = days_from_civil(year, (unsigned)g->date.month, (unsigned)g->date.day);
    const int64_t sec = days * 86400LL + (int64_t)g->tim.hour * 3600LL + (int64_t)g->tim.minute * 60LL + (int64_t)g->tim.second;
    if (sec < 946684800LL) {
        return false;
    }

    out->tv_sec = (time_t)sec;
    out->tv_usec = 0;
    return true;
}

static void gps_try_sync_time_from_fix(const gps_t *g) {
    if (gps_time_synced || !g) {
        return;
    }

    struct timeval tv;
    if (!gps_build_utc_timeval(g, &tv)) {
        return;
    }

    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return;
    }

    if (now.tv_sec >= 1600000000) {
        gps_time_synced = true;
        return;
    }

    if (tv.tv_sec >= 1600000000) {
        settimeofday(&tv, NULL);
        gps_time_synced = true;

#ifdef CONFIG_HAS_RTC_CLOCK
        // Persist UTC time to the external RTC so it survives reboots, matching
        // what NTP sync and the `settime` command do. gmtime_r yields UTC fields
        // because tv was built directly from civil date/time without TZ.
        struct tm utc_tm;
        RTC_Date rtc_time;
        gmtime_r(&tv.tv_sec, &utc_tm);
        rtc_time.year = utc_tm.tm_year + 1900;
        rtc_time.month = utc_tm.tm_mon + 1;
        rtc_time.day = utc_tm.tm_mday;
        rtc_time.hour = utc_tm.tm_hour;
        rtc_time.minute = utc_tm.tm_min;
        rtc_time.second = utc_tm.tm_sec;
        if (rtc_set_datetime(&rtc_time) == ESP_OK) {
            ESP_LOGI("GPS", "UTC time saved to RTC from GPS fix");
        }
#endif
    }
}

typedef struct {
    uint8_t bssid[6];
    time_t detection_time;
    time_t last_update_time;
} blacklisted_ap_t;

static blacklisted_ap_t *blacklist = NULL;
static int blacklist_count = 0;

typedef struct {
    uint8_t network_index;
    uint32_t due_ms;
} pineap_log_event_t;

#define PINEAP_LOG_QUEUE_LEN 8
static QueueHandle_t s_pineap_log_queue = NULL;
static TaskHandle_t s_pineap_log_task = NULL;

static inline uint32_t now_ms_u32(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool allocate_pineap_tables(void) {
    if (pineap_networks != NULL && blacklist != NULL) {
        return true;
    }

    pineap_network_t *new_networks = calloc(MAX_PINEAP_NETWORKS, sizeof(*new_networks));
    blacklisted_ap_t *new_blacklist = calloc(MAX_PINEAP_NETWORKS, sizeof(*new_blacklist));
    if (new_networks == NULL || new_blacklist == NULL) {
        free(new_networks);
        free(new_blacklist);
        return false;
    }

    pineap_networks = new_networks;
    blacklist = new_blacklist;
    return true;
}

static void free_pineap_tables(void) {
    free(pineap_networks);
    pineap_networks = NULL;
    free(blacklist);
    blacklist = NULL;
    pineap_network_count = 0;
    blacklist_count = 0;
}

static bool is_blacklisted(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return false;
    }
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            return true;
        }
    }
    return false;
}

static bool should_update_blacklisted(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return false;
    }
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            time_t current_time = time(NULL);
            // Allow updates every 30 seconds
            if (current_time - blacklist[i].last_update_time >= 30) {
                blacklist[i].last_update_time = current_time;
                return true;
            }
            return false;
        }
    }
    return false;
}

static void add_to_blacklist(const uint8_t *bssid) {
    if (blacklist == NULL) {
        return;
    }
    time_t current_time = time(NULL);

    // First check if BSSID exists
    for (int i = 0; i < blacklist_count; i++) {
        if (memcmp(blacklist[i].bssid, bssid, 6) == 0) {
            blacklist[i].last_update_time = current_time;
            return;
        }
    }

    // If not found and we have space, add new entry
    if (blacklist_count < MAX_PINEAP_NETWORKS) {
        memcpy(blacklist[blacklist_count].bssid, bssid, 6);
        blacklist[blacklist_count].detection_time = current_time;
        blacklist[blacklist_count].last_update_time = current_time;
        blacklist_count++;
    }
}

static void channel_hop_timer_callback(void *arg) {
    if (!pineap_detection_active)
        return;

    portENTER_CRITICAL(&wardrive_ch_mux);
    wardrive_channel_idx = (wardrive_channel_idx + 1) % wardrive_channel_count;
    current_channel = wardrive_channels[wardrive_channel_idx];
    portEXIT_CRITICAL(&wardrive_ch_mux);
    esp_wifi_set_channel(current_channel, WIFI_SECOND_CHAN_NONE);
}

static esp_err_t start_channel_hopping(void) {
    esp_timer_create_args_t timer_args = {.callback = channel_hop_timer_callback,
                                          .name = "channel_hop"};

    if (channel_hop_timer == NULL) {
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &channel_hop_timer));
    }

    return esp_timer_start_periodic(channel_hop_timer, CHANNEL_HOP_INTERVAL_MS * 1000);
}

static void stop_channel_hopping(void) {
    if (channel_hop_timer) {
        esp_timer_stop(channel_hop_timer);
        esp_timer_delete(channel_hop_timer);
        channel_hop_timer = NULL;
    }
}

static pineap_network_t *find_or_create_network(const uint8_t *bssid) {
    if (pineap_networks == NULL) {
        return NULL;
    }

    for (int i = 0; i < pineap_network_count; i++) {
        if (compare_bssid(pineap_networks[i].bssid, bssid)) {
            return &pineap_networks[i];
        }
    }

    if (pineap_network_count < MAX_PINEAP_NETWORKS) {
        pineap_network_t *network = &pineap_networks[pineap_network_count++];
        memcpy(network->bssid, bssid, 6);
        network->ssid_count = 0;
        network->is_pineap = false;
        network->has_pineapple_oui = is_pineapple_oui(bssid);
        network->oui_logged = false;
        network->first_seen = time(NULL);
        network->log_due_ms = 0;
        network->log_pending = false;
        return network;
    }

    return NULL;
}

static uint32_t hash_ssid(const char *ssid) {
    uint32_t hash = 5381;
    int c;
    while ((c = *ssid++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static bool ssid_hash_exists(pineap_network_t *network, uint32_t hash) {
    for (int i = 0; i < network->ssid_count; i++) {
        if (network->ssid_hashes[i] == hash) {
            return true;
        }
    }
    return false;
}

static void wardrive_send_probe_request(void) {
    // Broadcast probe request frame
    uint8_t probe_req[] = {
        0x40, 0x00,                         // Frame Control: Probe Request
        0x00, 0x00,                         // Duration
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Destination: broadcast
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source: filled below
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // BSSID: broadcast
        0x00, 0x00,                         // Sequence Control
        // SSID IE (wildcard - empty means "any SSID")
        0x00, 0x00,
        // Supported Rates IE
        0x01, 0x08, 0x02, 0x04, 0x0b, 0x16, 0x0c, 0x12, 0x18, 0x24,
        // Extended Supported Rates IE
        0x32, 0x04, 0x30, 0x48, 0x60, 0x6c,
        // DS Parameter Set (current channel)
        0x03, 0x01, 0x01  // Channel placeholder
    };
    
    // Get our MAC address
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(&probe_req[10], mac, 6);
    
    // Set current channel in DS Parameter Set
    probe_req[sizeof(probe_req) - 1] = wardrive_channel;
    
    if (esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof(probe_req), false) != ESP_OK)
        wardrive_probe_failures++;
}

static int hop_count = 0;

static void wardrive_hop_timer_callback(void *arg) {
    if (!wardriving_hopping_active)
        return;

    uint32_t now_ms = now_ms_u32();
    bool helper_status_stale = (wardrive_peer_assist_active || wardrive_peer_assist_pending) &&
                               wardrive_peer_status_ms != 0 &&
                               (uint32_t)(now_ms - wardrive_peer_status_ms) > WARDRIVE_HELPER_STATUS_TIMEOUT_MS;
    if (wardrive_role == WARDRIVE_ROLE_PRIMARY &&
        (wardrive_peer_assist_active || wardrive_peer_assist_pending) &&
        (!esp_comm_manager_is_connected() || helper_status_stale)) {
        wardrive_peer_assist_active = false;
        wardrive_peer_assist_pending = false;
        wardrive_peer_status_ms = 0;
        gps_manager_set_peer_gps_preferred(false);
        gps_manager_clear_peer_fix();
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        glog("Wardrive: peer helper unavailable, continuing local scan only\n");
        wardrive_apply_hop_interval();
    }

    if (wardrive_channel_count == 0) return;
    wardrive_channel_idx = (wardrive_channel_idx + 1) % wardrive_channel_count;
    wardrive_channel = wardrive_channels[wardrive_channel_idx];
    if (esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        wardrive_channel_failures++;
        return;
    }
    
    // Send probe request to trigger AP responses
    wardrive_send_probe_request();
    
    hop_count++;
    if (hop_count % 200 == 0) {
        ESP_LOGI(TAG, "Wardrive hopped to channel %d (hop #%d)", wardrive_channel, hop_count);
    }
}

static esp_err_t start_wardrive_channel_hopping(void) {
    if (wardrive_active_scan_enabled) {
        wardrive_build_channel_list();
        esp_err_t err = wardrive_scan_start(wardrive_channels, wardrive_channel_count,
                                           wardrive_get_hop_interval_ms(), wardrive_active_result, NULL);
        wardriving_hopping_active = err == ESP_OK;
        if (err != ESP_OK) ESP_LOGE(TAG, "Active wardrive start failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_timer_create_args_t timer_args = {.callback = wardrive_hop_timer_callback,
                                          .name = "wardrive_hop"};

    if (wardrive_hop_timer == NULL) {
        esp_err_t err = esp_timer_create(&timer_args, &wardrive_hop_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create wardrive hop timer: %s", esp_err_to_name(err));
            return err;
        }
    }

    wardrive_build_channel_list();
    wardrive_channel_idx = 0;
    wardrive_channel = wardrive_channels[0];
    wardriving_hopping_active = true;
    hop_count = 0;
    
    esp_err_t err = esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "Wardrive starting on channel %d (set_channel: %s)", wardrive_channel, esp_err_to_name(err));
    
    uint32_t interval_ms = wardrive_get_hop_interval_ms();
    err = esp_timer_start_periodic(wardrive_hop_timer, (uint64_t)interval_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wardrive hop timer: %s", esp_err_to_name(err));
        wardriving_hopping_active = false;
        esp_timer_delete(wardrive_hop_timer);
        wardrive_hop_timer = NULL;
        toast_show("Wardrive hop failed", TOAST_ERROR);
        return err;
    }
    ESP_LOGI(TAG,
             "Wardrive channel hopping started (%d channels, %lums interval, role=%s)",
             wardrive_channel_count,
             (unsigned long)interval_ms,
             wardrive_role == WARDRIVE_ROLE_PRIMARY ? "primary" : "helper");
    return ESP_OK;
}

static void stop_wardrive_channel_hopping(void) {
    wardriving_hopping_active = false;
    wardrive_scan_stop();
    if (wardrive_hop_timer) {
        esp_timer_stop(wardrive_hop_timer);
        esp_timer_delete(wardrive_hop_timer);
        wardrive_hop_timer = NULL;
    }
}

#define WARDRIVE_HEARTBEAT_INTERVAL_MS 10000

static void wardrive_heartbeat_cb(void *arg) {
    (void)arg;

    if (!wardriving_hopping_active) {
        return;
    }

    if (wardrive_role == WARDRIVE_ROLE_PRIMARY &&
        (wardrive_peer_assist_active || wardrive_peer_assist_pending) &&
        (!esp_comm_manager_is_connected() ||
         (wardrive_peer_status_ms && (uint32_t)(now_ms_u32() - wardrive_peer_status_ms) > WARDRIVE_HELPER_STATUS_TIMEOUT_MS))) {
        wardrive_peer_assist_pending = false;
        wardrive_peer_plan_known = false;
        wardrive_peer_channel_count = 0;
        wardrive_peer_status_ms = 0;
        wardriving_set_peer_assist(false);
        gps_manager_clear_peer_fix();
        glog("Wardrive: peer unavailable; restored all locally supported channels\n");
    }

    wardrive_scan_stats_t scan = {0};
    wardrive_scan_get_stats(&scan);
    glog("Wardrive discovery: backend=%s scans=%lu results=%lu scanfail=%lu drainfail=%lu duration=%lu/%lums channel=%u parsefail=%lu tune/probefail=%lu/%lu suppressed=%lu pending_coalesced=%lu\n",
         wardrive_active_scan_enabled ? "active" : "monitor", (unsigned long)scan.completed,
         (unsigned long)scan.results, (unsigned long)scan.failures, (unsigned long)scan.drain_errors,
         (unsigned long)scan.last_ms, (unsigned long)scan.max_ms, scan.channel,
         (unsigned long)wardrive_parser_rejected, (unsigned long)wardrive_channel_failures,
         (unsigned long)wardrive_probe_failures, (unsigned long)wardrive_worker_suppressed,
         (unsigned long)wardrive_pending_suppressed);

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        (void)wardrive_send_helper_status();
    }

    gps_t gps_snapshot = {0};
    bool using_peer_gps = false;
    bool have_active_gps = gps_manager_get_wardrive_snapshot(&gps_snapshot, &using_peer_gps);
    bool peer_preferred = gps_manager_is_peer_gps_preferred();
    gps_t *gps_local = NULL;
    const char *fix_status = "No GPS";
    char fix_status_buf[24] = {0};
    uint8_t sats = 0;

    if (have_active_gps) {
        gps_local = &gps_snapshot;
    } else if (!peer_preferred) {
        static gps_t local_snapshot = {0};
        if (gps_manager_get_local_gps_snapshot(&local_snapshot)) {
            gps_local = &local_snapshot;
        }
    }

    if (gps_local != NULL) {
        sats = gps_local->sats_in_use;
        if (!gps_local->valid || gps_local->fix < GPS_FIX_GPS || gps_local->fix_mode < GPS_MODE_2D) {
            fix_status = using_peer_gps ? "Peer No Fix" : "No Fix";
        } else if (gps_local->fix_mode == GPS_MODE_2D) {
            fix_status = using_peer_gps ? "Peer 2D" : "2D";
        } else if (gps_local->fix_mode == GPS_MODE_3D) {
            fix_status = using_peer_gps ? "Peer 3D" : "3D";
        } else {
            fix_status = using_peer_gps ? "Peer Fix" : "Fix";
        }

        if (!wardrive_is_valid_date(&gps_local->date)) {
            snprintf(fix_status_buf, sizeof(fix_status_buf), "%s/NoDate", fix_status);
            fix_status = fix_status_buf;
        }
    } else if (peer_preferred) {
        fix_status = "Peer Stale";
    }

    uint32_t up_s = 0;
    if (wardrive_start_us != 0) {
        up_s = (uint32_t)((esp_timer_get_time() - wardrive_start_us) / 1000000LL);
    }

    uint32_t up_m = up_s / 60;
    uint32_t up_rem_s = up_s % 60;

    size_t pending = csv_get_pending_bytes();
    size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    UBaseType_t queue_depth = wardrive_obs_queue ? uxQueueMessagesWaiting(wardrive_obs_queue) : 0;
    portENTER_CRITICAL(&wardrive_obs_mux);
    UBaseType_t queue_high_water = wardrive_obs_high_water;
    uint32_t queue_drop_local = wardrive_obs_drop_local;
    uint32_t queue_drop_helper = wardrive_obs_drop_helper;
    uint32_t queue_drop_sink = wardrive_obs_drop_sink;
    portEXIT_CRITICAL(&wardrive_obs_mux);

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        glog("Wardrive: ap=%lu logged=%lu/%lu gpsrej=%lu helper=%lu/%lu tx(n/p/r/t/s)=%lu/%lu/%lu/%lu/%lu send(ok/fail)=%lu/%lu peergps(rx/fix tx_ok/fail)=%lu/%lu %lu/%lu ch=%u up=%lum%02lus gps=%s/%u q=%u/%u hi=%u drop=%lu/%lu/%lu pending=%uB heap=%u/%uB\n",
             (unsigned long)wardrive_wifi_frames_seen,
             (unsigned long)wardrive_log_ok,
             (unsigned long)wardrive_log_attempts,
             (unsigned long)wardrive_gps_rejected,
             (unsigned long)wardrive_helper_merged_ok,
             (unsigned long)wardrive_helper_rx_observations,
             (unsigned long)wardrive_helper_tx_new,
             (unsigned long)wardrive_helper_tx_ssid_promo,
             (unsigned long)wardrive_helper_tx_rssi,
             (unsigned long)wardrive_helper_tx_refresh,
             (unsigned long)wardrive_helper_tx_suppressed,
             (unsigned long)wardrive_helper_stream_send_ok,
             (unsigned long)wardrive_helper_stream_send_fail,
             (unsigned long)peer_gps_stream_rx_packets,
             (unsigned long)peer_gps_stream_rx_fix_packets,
             (unsigned long)peer_gps_stream_tx_ok,
             (unsigned long)peer_gps_stream_tx_fail,
             (unsigned)wardrive_channel,
             (unsigned long)up_m,
             (unsigned long)up_rem_s,
             fix_status,
             (unsigned)sats,
             (unsigned)queue_depth,
             (unsigned)wardrive_obs_queue_capacity,
             (unsigned)queue_high_water,
             (unsigned long)queue_drop_local,
             (unsigned long)queue_drop_helper,
             (unsigned long)queue_drop_sink,
             (unsigned)pending,
             (unsigned)heap_free,
             (unsigned)heap_largest);
    } else {
        glog("Wardrive: ap=%lu logged=%lu/%lu gpsrej=%lu helper=%lu/%lu peergps(rx/fix tx_ok/fail)=%lu/%lu %lu/%lu ch=%u up=%lum%02lus gps=%s/%u q=%u/%u hi=%u drop=%lu/%lu/%lu pending=%uB heap=%u/%uB\n",
             (unsigned long)wardrive_wifi_frames_seen,
             (unsigned long)wardrive_log_ok,
             (unsigned long)wardrive_log_attempts,
             (unsigned long)wardrive_gps_rejected,
             (unsigned long)wardrive_helper_merged_ok,
             (unsigned long)wardrive_helper_rx_observations,
             (unsigned long)peer_gps_stream_rx_packets,
             (unsigned long)peer_gps_stream_rx_fix_packets,
             (unsigned long)peer_gps_stream_tx_ok,
             (unsigned long)peer_gps_stream_tx_fail,
             (unsigned)wardrive_channel,
             (unsigned long)up_m,
             (unsigned long)up_rem_s,
             fix_status,
             (unsigned)sats,
             (unsigned)queue_depth,
             (unsigned)wardrive_obs_queue_capacity,
             (unsigned)queue_high_water,
             (unsigned long)queue_drop_local,
             (unsigned long)queue_drop_helper,
             (unsigned long)queue_drop_sink,
             (unsigned)pending,
             (unsigned)heap_free,
             (unsigned)heap_largest);
    }
}

static void start_wardrive_heartbeat(void) {
    wardrive_parser_rejected = 0;
    wardrive_channel_failures = 0;
    wardrive_probe_failures = 0;
    wardrive_worker_suppressed = 0;
    wardrive_start_us = esp_timer_get_time();
    wardrive_wifi_frames_seen = 0;
    wardrive_ble_advs_seen = 0;
    wardrive_log_attempts = 0;
    wardrive_log_ok = 0;
    wardrive_gps_rejected = 0;
    wardrive_helper_rx_observations = 0;
    wardrive_helper_merged_ok = 0;
    wardrive_helper_tx_new = 0;
    wardrive_helper_tx_ssid_promo = 0;
    wardrive_helper_tx_rssi = 0;
    wardrive_helper_tx_refresh = 0;
    wardrive_helper_tx_suppressed = 0;
    wardrive_helper_stream_send_ok = 0;
    wardrive_helper_stream_send_fail = 0;
    peer_gps_stream_tx_ok = 0;
    peer_gps_stream_tx_fail = 0;
    peer_gps_stream_rx_packets = 0;
    peer_gps_stream_rx_fix_packets = 0;
    if (!wardrive_helper_dedupe) {
        wardrive_helper_dedupe = mon_tbl_calloc(WARDRIVE_HELPER_DEDUPE_SIZE, sizeof(*wardrive_helper_dedupe));
        if (!wardrive_helper_dedupe) ESP_LOGW(TAG, "wardrive dedupe alloc failed; helper suppression degraded");
    }
    if (wardrive_helper_dedupe) {
        memset(wardrive_helper_dedupe, 0, WARDRIVE_HELPER_DEDUPE_SIZE * sizeof(*wardrive_helper_dedupe));
    }
    wardrive_helper_dedupe_idx = 0;
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    ble_wardriving_reset_unique_device_count();
#endif

    if (!wardrive_heartbeat_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &wardrive_heartbeat_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "wardrive_hb"
        };
        esp_err_t err = esp_timer_create(&timer_args, &wardrive_heartbeat_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create wardrive heartbeat timer: %s", esp_err_to_name(err));
            return;
        }
    }

    (void)esp_timer_stop(wardrive_heartbeat_timer);
    esp_err_t start_err = esp_timer_start_periodic(wardrive_heartbeat_timer,
                                                    (uint64_t)WARDRIVE_HEARTBEAT_INTERVAL_MS * 1000ULL);
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wardrive heartbeat timer: %s", esp_err_to_name(start_err));
    }
}

static void stop_wardrive_heartbeat(void) {
    if (wardrive_heartbeat_timer) {
        esp_timer_stop(wardrive_heartbeat_timer);
        esp_timer_delete(wardrive_heartbeat_timer);
        wardrive_heartbeat_timer = NULL;
    }
}

static void pineap_log_worker_task(void *arg) {
    (void)arg;
    pineap_log_event_t ev;

    for (;;) {
        if (xQueueReceive(s_pineap_log_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint32_t now = now_ms_u32();
        if (ev.due_ms > now) {
            vTaskDelay(pdMS_TO_TICKS(ev.due_ms - now));
        }

        if (!pineap_detection_active || pineap_networks == NULL) {
            continue;
        }

        if (ev.network_index >= (uint8_t)pineap_network_count) {
            continue;
        }

        pineap_network_t *network = &pineap_networks[ev.network_index];
        if (!network->log_pending || network->log_due_ms != ev.due_ms) {
            continue;
        }

        char mac_str[18];
        format_mac_address(network->bssid, mac_str, sizeof(mac_str), false);

        char ssids_str[256] = {0};
        int valid_ssid_count = build_recent_ssids_string(network, ssids_str, sizeof(ssids_str));

        if (valid_ssid_count >= MIN_SSIDS_FOR_DETECTION) {
            pulse_once(&rgb_manager, 255, 0, 255);

            for (int i = 0; i < pineap_network_count; i++) {
                if (i != (network - pineap_networks) &&
                    strcasecmp(network->recent_ssids[0], pineap_networks[i].recent_ssids[0]) == 0) {
                    char other_mac_str[18];
                    format_mac_address(pineap_networks[i].bssid, other_mac_str, sizeof(other_mac_str), false);
                    glog("Evil Twin Detected:\nSame SSID '%.100s'\nfrom BSSID %s and\n%s\n",
                         network->recent_ssids[0], mac_str, other_mac_str);
                }
            }

            log_pineap_details(network, "Pineapple detected!", ssids_str, valid_ssid_count);
        }

        network->log_pending = false;
        network->log_due_ms = 0;
    }
}

static void start_pineap_log_worker(void) {
    if (s_pineap_log_queue == NULL) {
        s_pineap_log_queue = xQueueCreate(PINEAP_LOG_QUEUE_LEN, sizeof(pineap_log_event_t));
    }
    if (s_pineap_log_queue != NULL && s_pineap_log_task == NULL) {
        if (xTaskCreate(pineap_log_worker_task, "pineap_logw", 1792, NULL, 1, &s_pineap_log_task) != pdPASS) {
            s_pineap_log_task = NULL;
        }
    }
}

static void stop_pineap_log_worker(void) {
    if (s_pineap_log_task != NULL) {
        vTaskDelete(s_pineap_log_task);
        s_pineap_log_task = NULL;
    }
    if (s_pineap_log_queue != NULL) {
        vQueueDelete(s_pineap_log_queue);
        s_pineap_log_queue = NULL;
    }
}

void start_pineap_detection(void) {
    if (!allocate_pineap_tables()) {
        glog("PineAP: failed to allocate detection tables\n");
        return;
    }

    pineap_detection_active = true;
    pineap_network_count = 0;
    blacklist_count = 0;
    memset(pineap_networks, 0, MAX_PINEAP_NETWORKS * sizeof(*pineap_networks));
    memset(blacklist, 0, MAX_PINEAP_NETWORKS * sizeof(*blacklist));
    start_pineap_log_worker();
    wardrive_build_channel_list();
    wardrive_channel_idx = 0;
    current_channel = wardrive_channels[0];
    start_channel_hopping();
}

void stop_pineap_detection(void) {
    pineap_detection_active = false;
    stop_channel_hopping();
    stop_pineap_log_worker();
    free_pineap_tables();
}

static void wardrive_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data) {
        return;
    }
    if (length == 9 && data[0] == WARDRIVE_CONTROL_MARKER && data[1] == WARDRIVE_CONTROL_VERSION_PLAN &&
        data[2] == WARDRIVE_CONTROL_OBS_ACK && wardrive_ack_sem && wardrive_role == WARDRIVE_ROLE_HELPER) {
        portENTER_CRITICAL(&wardrive_obs_mux);
        wardrive_ack_session = (uint32_t)wardrive_get_i32le(data + 3);
        wardrive_ack_sequence = (uint16_t)wardrive_get_i16le(data + 7);
        portEXIT_CRITICAL(&wardrive_obs_mux);
        xSemaphoreGive(wardrive_ack_sem);
        return;
    }
    if (length >= 17 && data[0] == WARDRIVE_STREAM_VERSION_ACKED) {
        if (!wardriving_hopping_active || wardrive_role != WARDRIVE_ROLE_PRIMARY ||
            !wardrive_peer_plan_known || !wd_channel_valid(data[1]) || (data[4] & 1) ||
            data[10] > 32 || length != (size_t)17 + data[10]) return;
        static const uint8_t zero[6] = {0};
        if (memcmp(data + 4, zero, 6) == 0) return;
        wardriving_data_t observation = {0};
        observation.channel = data[1]; observation.rssi = (int8_t)data[2];
        memcpy(observation.ssid, data + 11, data[10]);
        snprintf(observation.bssid, sizeof(observation.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 data[4], data[5], data[6], data[7], data[8], data[9]);
        strncpy(observation.encryption_type, wardrive_auth_code_to_string(data[3]), sizeof(observation.encryption_type) - 1);
        wardrive_helper_rx_observations++;
        if (wardrive_obs_submit(&observation, WARDRIVE_OBS_HELPER)) {
            uint8_t ack[9] = {WARDRIVE_CONTROL_MARKER, WARDRIVE_CONTROL_VERSION_PLAN, WARDRIVE_CONTROL_OBS_ACK};
            memcpy(ack + 3, data + 11 + data[10], 6);
            (void)esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_WARDRIVE, ack, sizeof(ack));
        }
        return;
    }
    if (length >= 7 && data[0] == WARDRIVE_CONTROL_MARKER) {
        bool modern = data[1] == WARDRIVE_CONTROL_VERSION_PLAN;
        if ((!modern && data[1] != WARDRIVE_CONTROL_VERSION) ||
            data[2] != WARDRIVE_CONTROL_HELPER_READY || !wardriving_hopping_active ||
            wardrive_role != WARDRIVE_ROLE_PRIMARY ||
            (!wardrive_peer_assist_pending && !wardrive_peer_assist_active)) return;
        if (modern) {
            uint8_t count = data[3];
            if (count > WIFI_CHANNELS_MAX || length != (size_t)8 + count) return;
            for (uint8_t i = 0; i < count; ++i) if (!wd_channel_valid(data[8 + i])) return;
            bool changed = !wardrive_peer_plan_known || count != wardrive_peer_channel_count ||
                           memcmp(wardrive_peer_channels, data + 8, count) != 0;
            memcpy(wardrive_peer_channels, data + 8, count);
            wardrive_peer_channel_count = count;
            wardrive_peer_plan_known = true;
            if (changed && wardrive_peer_assist_active) wardrive_build_channel_list();
        }
        wardrive_peer_status_ms = now_ms_u32();
        if (data[3] > 0 && wardrive_peer_assist_pending) {
            wardrive_peer_assist_pending = false;
            wardriving_set_peer_assist(true);
            glog("Wardrive helper ready: channels=%u plan=%s GPS=%s (independent of scanning)\n",
                 data[3], modern ? "acknowledged" : "legacy", data[6] ? "ready" : "unavailable");
        } else if (!data[3]) {
            wardriving_set_peer_assist(false);
            wardrive_peer_assist_pending = true;
        }
        return;
    }
    if (length < (1 + 1 + 1 + 1 + 6 + 1)) {
        return;
    }
    if (!wardriving_hopping_active || wardrive_role != WARDRIVE_ROLE_PRIMARY) {
        return;
    }

    size_t pos = 0;
    uint8_t version = data[pos++];
    if (version != WARDRIVE_STREAM_VERSION && version != WARDRIVE_STREAM_VERSION_LEGACY) {
        return;
    }

    uint8_t channel_num = data[pos++];
    int8_t rssi = (int8_t)data[pos++];
    uint8_t auth_code = data[pos++];
    const uint8_t *bssid = data + pos;
    pos += 6;
    uint8_t ssid_len = data[pos++];
    if (ssid_len > 32 || (pos + ssid_len) > length) {
        return;
    }

    char ssid[33] = {0};
    if (ssid_len > 0) {
        memcpy(ssid, data + pos, ssid_len);
        ssid[ssid_len] = '\0';
        for (uint8_t i = 0; i < ssid_len; i++) {
            if (ssid[i] == '\0' || (uint8_t)ssid[i] < 0x20 || (uint8_t)ssid[i] == 0x7f) {
                ssid[i] = '?';
            }
        }
    }
    pos += ssid_len;

    gps_peer_fix_t peer_fix = {0};
    bool peer_fix_present = false;
    bool peer_fix_has_coords = false;
    if (version >= WARDRIVE_STREAM_VERSION) {
        if (pos >= length) {
            return;
        }

        uint8_t gps_flags = data[pos++];
        if (gps_flags & WARDRIVE_STREAM_FLAG_GPS_PRESENT) {
            if ((length - pos) < (4 + 4 + 2 + 1 + 1 + 1 + 1 + 2)) {
                return;
            }

            int32_t lat_e7 = wardrive_get_i32le(data + pos);
            pos += 4;
            int32_t lon_e7 = wardrive_get_i32le(data + pos);
            pos += 4;
            int16_t alt_dm = wardrive_get_i16le(data + pos);
            pos += 2;
            uint8_t sats_in_use = data[pos++];
            uint8_t sats_in_view = data[pos++];
            uint8_t fix = data[pos++];
            uint8_t fix_mode = data[pos++];
            uint16_t hdop_x10 = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
            pos += 2;
            uint8_t day = 0;
            uint8_t month = 0;
            uint16_t year = 0;
            uint8_t hour = 0;
            uint8_t minute = 0;
            uint8_t second = 0;
            if ((length - pos) >= 7) {
                day = data[pos++];
                month = data[pos++];
                year = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
                pos += 2;
                hour = data[pos++];
                minute = data[pos++];
                second = data[pos++];
            }

            peer_fix_present = true;
            peer_fix.valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_FIX) != 0;
            peer_fix.fix = (gps_fix_t)fix;
            peer_fix.fix_mode = (gps_fix_mode_t)fix_mode;
            peer_fix.date_valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_DATE_VALID) != 0;
            peer_fix.time_valid = (gps_flags & WARDRIVE_STREAM_FLAG_GPS_TIME_VALID) != 0;
            peer_fix.date.day = day;
            peer_fix.date.month = month;
            peer_fix.date.year = year;
            peer_fix.tim.hour = hour;
            peer_fix.tim.minute = minute;
            peer_fix.tim.second = second;
            peer_fix.tim.thousand = 0;
            peer_fix.sats_in_use = sats_in_use;
            peer_fix.sats_in_view = sats_in_view;
            peer_fix.latitude = (float)lat_e7 / 10000000.0f;
            peer_fix.longitude = (float)lon_e7 / 10000000.0f;
            peer_fix.altitude = (float)alt_dm / 10.0f;
            peer_fix.speed = 0.0f;
            peer_fix.course = 0.0f;
            peer_fix.hdop = (float)hdop_x10 / 10.0f;

            peer_fix_has_coords =
                peer_fix.valid &&
                peer_fix.latitude >= -90.0f && peer_fix.latitude <= 90.0f &&
                peer_fix.longitude >= -180.0f && peer_fix.longitude <= 180.0f;
        }
    }

    wardrive_helper_rx_observations++;

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = false;
    if (ssid_len > 0) {
        strncpy(wardriving_data.ssid, ssid, sizeof(wardriving_data.ssid) - 1);
    }
    snprintf(wardriving_data.bssid,
             sizeof(wardriving_data.bssid),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             bssid[0],
             bssid[1],
             bssid[2],
             bssid[3],
             bssid[4],
             bssid[5]);
    wardriving_data.rssi = rssi;
    wardriving_data.channel = channel_num;
    strncpy(wardriving_data.encryption_type,
            wardrive_auth_code_to_string(auth_code),
            sizeof(wardriving_data.encryption_type) - 1);
    if (peer_fix_present && peer_fix_has_coords) {
        wardriving_data.latitude = peer_fix.latitude;
        wardriving_data.longitude = peer_fix.longitude;
        wardriving_data.altitude = peer_fix.altitude;
        wardriving_data.accuracy = peer_fix.hdop * 5.0f;
    }

    (void)wardrive_obs_submit(&wardriving_data, WARDRIVE_OBS_HELPER);
}

static void gps_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!data || length < (1 + 1 + 4 + 4 + 2 + 1 + 1 + 1 + 1 + 2 + 2 + 2)) {
        return;
    }

    size_t pos = 0;
    uint8_t version = data[pos++];
    if (version != GPS_STREAM_VERSION) {
        return;
    }

    uint8_t flags = data[pos++];
    int32_t lat_e7 = wardrive_get_i32le(data + pos);
    pos += 4;
    int32_t lon_e7 = wardrive_get_i32le(data + pos);
    pos += 4;
    int16_t alt_dm = wardrive_get_i16le(data + pos);
    pos += 2;
    uint8_t sats_in_use = data[pos++];
    uint8_t sats_in_view = data[pos++];
    uint8_t fix = data[pos++];
    uint8_t fix_mode = data[pos++];
    int16_t hdop_x10 = wardrive_get_i16le(data + pos);
    pos += 2;
    int16_t speed_x100 = wardrive_get_i16le(data + pos);
    pos += 2;
    int16_t course_x100 = wardrive_get_i16le(data + pos);
    pos += 2;
    uint8_t day = 0;
    uint8_t month = 0;
    uint16_t year = 0;
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    if ((length - pos) >= 7) {
        day = data[pos++];
        month = data[pos++];
        year = (uint16_t)data[pos] | ((uint16_t)data[pos + 1] << 8);
        pos += 2;
        hour = data[pos++];
        minute = data[pos++];
        second = data[pos++];
    }

    gps_peer_fix_t peer_fix = {0};
    peer_fix.valid = (flags & GPS_STREAM_FLAG_FIX) != 0;
    peer_fix.fix = (gps_fix_t)fix;
    peer_fix.fix_mode = (gps_fix_mode_t)fix_mode;
    peer_fix.date_valid = (flags & GPS_STREAM_FLAG_DATE_VALID) != 0;
    peer_fix.time_valid = (flags & GPS_STREAM_FLAG_TIME_VALID) != 0;
    peer_fix.date.day = day;
    peer_fix.date.month = month;
    peer_fix.date.year = year;
    peer_fix.tim.hour = hour;
    peer_fix.tim.minute = minute;
    peer_fix.tim.second = second;
    peer_fix.tim.thousand = 0;
    peer_fix.sats_in_use = sats_in_use;
    peer_fix.sats_in_view = sats_in_view;
    peer_fix.latitude = (float)lat_e7 / 10000000.0f;
    peer_fix.longitude = (float)lon_e7 / 10000000.0f;
    peer_fix.altitude = (float)alt_dm / 10.0f;
    peer_fix.hdop = (float)hdop_x10 / 10.0f;
    peer_fix.speed = (float)speed_x100 / 100.0f;
    peer_fix.course = (float)course_x100 / 100.0f;

    gps_manager_update_peer_fix(&peer_fix);
    peer_gps_stream_rx_packets++;
    if (peer_fix.valid) {
        peer_gps_stream_rx_fix_packets++;
    }
}

void wardriving_register_stream_handler(void) {
    bool ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_WARDRIVE,
                                                       wardrive_stream_rx_cb,
                                                       NULL);
    bool gps_ok = esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_GPS,
                                                           gps_stream_rx_cb,
                                                           NULL);
    ESP_LOGI(TAG, "Wardrive stream handler: %s", ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "Peer GPS stream handler: %s", gps_ok ? "OK" : "FAIL");

    if (peer_gps_stream_task_handle == NULL) {
        peer_gps_stream_stack = heap_caps_malloc(3072 * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        peer_gps_stream_tcb = malloc(sizeof(StaticTask_t));
        if (peer_gps_stream_stack && peer_gps_stream_tcb) {
            peer_gps_stream_task_handle = xTaskCreateStatic(
                peer_gps_stream_task,
                "peer_gps_stream",
                3072,
                NULL,
                3,
                peer_gps_stream_stack,
                peer_gps_stream_tcb);
            ESP_LOGI(TAG, "Peer GPS stream task stack allocated from PSRAM: %d bytes", 
                     (int)(3072 * sizeof(StackType_t)));
        }
        if (!peer_gps_stream_task_handle) {
            if (peer_gps_stream_stack) {
                free(peer_gps_stream_stack);
                peer_gps_stream_stack = NULL;
            }
            if (peer_gps_stream_tcb) {
                free(peer_gps_stream_tcb);
                peer_gps_stream_tcb = NULL;
            }
            ESP_LOGW(TAG, "Peer GPS stream task create failed");
        }
    }
}

bool wardriving_get_helper_channel_plan_csv(char *out, size_t out_len) {
    if (!out || out_len == 0) {
        return false;
    }

    uint8_t full_channels[WIFI_CHANNELS_MAX] = {0};
    uint8_t channels_24[WIFI_CHANNELS_MAX] = {0};
    uint8_t full_count = wardrive_build_full_channel_list(full_channels);
    uint8_t channels_24_count = 0;

    for (uint8_t i = 0; i < full_count; i++) {
        if (full_channels[i] <= 14 && channels_24_count < WIFI_CHANNELS_MAX) {
            channels_24[channels_24_count++] = full_channels[i];
        }
    }

    uint8_t plan[WIFI_CHANNELS_MAX] = {0};
    uint8_t plan_count = 0;
    if (channels_24_count > 0) {
        memcpy(plan, channels_24, channels_24_count);
        plan_count = channels_24_count;
    } else {
        for (uint8_t i = 1; i < full_count && plan_count < WIFI_CHANNELS_MAX; i += 2) {
            plan[plan_count++] = full_channels[i];
        }
    }

    if (plan_count == 0) {
        out[0] = '\0';
        return false;
    }

    size_t pos = 0;
    for (uint8_t i = 0; i < plan_count; i++) {
        int wrote = snprintf(out + pos, out_len - pos, (i == 0) ? "%u" : ",%u", (unsigned)plan[i]);
        if (wrote <= 0 || (size_t)wrote >= (out_len - pos)) {
            out[0] = '\0';
            return false;
        }
        pos += (size_t)wrote;
    }

    return true;
}

bool wardriving_set_primary_channels_from_csv(const char *csv) {
    if (wardriving_hopping_active) return false;
    wardrive_primary_channel_count = 0;
    if (!csv || !*csv) return true;
    // Parse peer capabilities without filtering them through this chip's bands.
    uint8_t count = wifi_channels_parse_list(csv, wardrive_primary_channels, WIFI_CHANNELS_MAX);
    for (uint8_t i = 0; i < count; ++i) if (!wd_channel_valid(wardrive_primary_channels[i])) return false;
    wardrive_primary_channel_count = count;
    return count > 0;
}

bool wardriving_set_active_scan(bool enabled) {
    if (wardriving_hopping_active) return false;
    wardrive_active_scan_enabled = enabled;
    return true;
}

bool wardriving_is_running(void) {
    return wardriving_hopping_active;
}

bool wardriving_start_peer_helper(void) {
    if (!esp_comm_manager_is_connected()) return false;
    uint8_t channels[WIFI_CHANNELS_MAX];
    size_t count = 0;
    hop_profile_resolve(channels, sizeof(channels), &count);
    if (!count) count = wardrive_build_full_channel_list(channels);
    char primary[192] = {0}, fallback[192] = {0}, command[251];
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        int n = snprintf(primary + pos, sizeof(primary) - pos, i ? ",%u" : "%u", channels[i]);
        if (n < 0 || (size_t)n >= sizeof(primary) - pos) return false;
        pos += n;
    }
    if (!wardriving_get_helper_channel_plan_csv(fallback, sizeof(fallback))) return false;
    int n = snprintf(command, sizeof(command),
                     "startwd --helper --channels %s --primary-channels %s --hop %u%s%s",
                     fallback, primary, settings_get_wd_hop_helper_ms(&G_Settings),
                     settings_get_wd_weighted_5g(&G_Settings) ? " --weighted" : "",
                     wardrive_active_scan_enabled ? " --active" : " --monitor");
    if (n < 0 || (size_t)n >= sizeof(command)) return false;
    wardriving_expect_peer_assist(true);
    bool sent = esp_comm_manager_send_command_line(command);
    if (!sent) wardriving_expect_peer_assist(false);
    return sent;
}

bool wardriving_set_helper_channels_from_csv(const char *csv) {
    if (!csv || csv[0] == '\0') {
        wardrive_forced_helper_channel_count = 0;
        return true;
    }

    uint8_t parsed[WIFI_CHANNELS_MAX] = {0};
    uint8_t count = wardrive_parse_channel_csv(csv, parsed, WIFI_CHANNELS_MAX);
    if (count == 0) {
        wardrive_forced_helper_channel_count = 0;
        return false;
    }

    memcpy(wardrive_forced_helper_channels, parsed, count);
    wardrive_forced_helper_channel_count = count;

    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        wardrive_apply_hop_interval();
    }

    return true;
}

void wardriving_set_helper_hop_ms(uint16_t ms) {
    wardrive_helper_hop_override_ms = ms;
    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        wardrive_apply_hop_interval();
    }
}

void wardriving_set_helper_weighted_5g(bool enabled) {
    wardrive_weighted_5g_override = enabled;
    if (wardrive_role == WARDRIVE_ROLE_HELPER) {
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
    }
}

static bool start_wardriving_session(bool helper) {
    if (ap_scan_is_running()) {
        ESP_LOGW(TAG, "Stop the general AP scan before starting wardriving");
        return false;
    }
    if (!wardrive_obs_queue_ensure()) {
        ESP_LOGE(TAG, "Failed to create wardrive observation queue");
        return false;
    }
    xSemaphoreTakeRecursive(wardrive_obs_lifecycle_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&wardrive_obs_mux);
    bool already_accepting = wardrive_obs_accepting;
    portEXIT_CRITICAL(&wardrive_obs_mux);
    if (already_accepting) {
        xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
        ESP_LOGW(TAG, "Wardrive observation queue already active");
        return false;
    }

    if (!wardrive_ack_sem) wardrive_ack_sem = xSemaphoreCreateBinaryStatic(&wardrive_ack_storage);
    wardrive_tx_session = (uint32_t)esp_timer_get_time();
    wardrive_tx_sequence = 0;
    xQueueReset(wardrive_obs_queue);
    portENTER_CRITICAL(&wardrive_obs_mux);
    wardrive_obs_enqueued = 0;
    wardrive_obs_drop_local = 0;
    wardrive_obs_drop_helper = 0;
    wardrive_obs_drop_sink = 0;
    wardrive_obs_high_water = 0;
    wardrive_obs_active_producers = 0;
    memset(wardrive_pending, 0, sizeof(wardrive_pending));
    wardrive_pending_suppressed = 0;
    wardrive_pending_token = 0;
    wardrive_obs_accepting = true;
    portEXIT_CRITICAL(&wardrive_obs_mux);
    wardrive_role = helper ? WARDRIVE_ROLE_HELPER : WARDRIVE_ROLE_PRIMARY;
    wardrive_peer_assist_active = false;
    wardrive_peer_assist_pending = false;
    wardrive_peer_status_ms = 0;
    wardrive_peer_plan_known = false;
    wardrive_peer_channel_count = 0;
    if (!helper) {
        wardrive_forced_helper_channel_count = 0;
        wardrive_primary_channel_count = 0;
    }
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
    start_wardrive_heartbeat();
    if (start_wardrive_channel_hopping() != ESP_OK) {
        stop_wardrive_heartbeat();
        wardrive_obs_session_stop_and_drain();
        xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
        return false;
    }
    xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
    return true;
}

bool start_wardriving(void) {
    return start_wardriving_session(false);
}

bool start_wardriving_helper(void) {
    if (!start_wardriving_session(true)) return false;
    (void)wardrive_send_helper_status();
    return true;
}

void stop_wardriving(void) {
    if (wardrive_obs_lifecycle_mutex) {
        xSemaphoreTakeRecursive(wardrive_obs_lifecycle_mutex, portMAX_DELAY);
    }
    stop_wardrive_heartbeat();
    stop_wardrive_channel_hopping();
    wardrive_obs_session_stop_and_drain();
    wardrive_role = WARDRIVE_ROLE_PRIMARY;
    wardrive_peer_assist_active = false;
    wardrive_peer_assist_pending = false;
    wardrive_peer_status_ms = 0;
    wardrive_forced_helper_channel_count = 0;
    wardrive_primary_channel_count = 0;
    wardrive_peer_channel_count = 0;
    wardrive_peer_plan_known = false;
    wardrive_helper_hop_override_ms = 0;
    wardrive_weighted_5g_override = false;
    gps_manager_set_peer_gps_preferred(false);
    gps_manager_clear_peer_fix();
    if (wardrive_obs_lifecycle_mutex) {
        xSemaphoreGiveRecursive(wardrive_obs_lifecycle_mutex);
    }
    wardrive_dedupe_release();
}

void wardriving_set_peer_assist(bool enabled) {
    bool changed = (wardrive_peer_assist_active != enabled);
    wardrive_peer_assist_active = enabled;
    if (changed && wardrive_role == WARDRIVE_ROLE_PRIMARY) {
        if (wardriving_hopping_active && wardrive_hop_timer != NULL) {
            (void)esp_timer_stop(wardrive_hop_timer);
        }
        wardrive_build_channel_list();
        wardrive_channel_idx = 0;
        if (!wardrive_active_scan_enabled && wardrive_channel_count > 0) {
            wardrive_channel = wardrive_channels[0];
            (void)esp_wifi_set_channel(wardrive_channel, WIFI_SECOND_CHAN_NONE);
        }
        wardrive_apply_hop_interval();
    }
}

void wardriving_expect_peer_assist(bool enabled) {
    wardrive_peer_assist_pending = enabled;
    wardrive_peer_status_ms = enabled ? now_ms_u32() : 0;
    if (enabled && wardrive_peer_status_ms == 0) wardrive_peer_status_ms = 1;
}

bool wardriving_is_helper_mode(void) {
    return wardrive_role == WARDRIVE_ROLE_HELPER;
}

bool wardriving_is_peer_assist_active(void) {
    return wardrive_peer_assist_active;
}

bool wardriving_has_peer_helper(void) {
    return wardrive_peer_assist_active || wardrive_peer_assist_pending;
}

uint32_t wardriving_get_ap_count(void) {
    return wardrive_wifi_frames_seen;
}

#define IRAM_PRINTF(fmt, ...) do { \
    static const char flash_fmt[] STORE_STR_ATTR = fmt; \
    esp_rom_printf(flash_fmt, ##__VA_ARGS__); \
} while(0)

// Helper function to check if SSID is valid and unique
static bool is_valid_unique_ssid(const char *new_ssid, pineap_network_t *network) {
    // Check if SSID is empty or just whitespace
    if (strlen(new_ssid) == 0)
        return false;

    bool all_whitespace = true;
    for (const char *p = new_ssid; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            all_whitespace = false;
            break;
        }
    }
    if (all_whitespace)
        return false;

    // Check if this SSID is already in our recent list
    for (int i = 0; i < network->ssid_count && i < RECENT_SSID_COUNT; i++) {
        if (strcasecmp(network->recent_ssids[i], new_ssid) == 0) {
            return false; // SSID already exists (case insensitive)
        }
    }

    return true;
}

static int build_recent_ssids_string(const pineap_network_t *network, char *out, size_t out_size) {
    if (!network || !out || out_size == 0) {
        return 0;
    }

    out[0] = '\0';
    size_t len = 0;
    int count = 0;

    for (int i = 0; i < network->ssid_count && i < RECENT_SSID_COUNT; i++) {
        const char *ssid = network->recent_ssids[i];
        if (!ssid || ssid[0] == '\0')
            continue;

        if (len < out_size - 1 && count > 0) {
            if (len <= out_size - 3) {
                out[len++] = ',';
                out[len++] = ' ';
            } else {
                break;
            }
        }

        size_t ssid_len = strnlen(ssid, 32);
        size_t avail = out_size - len - 1;
        if (avail == 0)
            break;
        size_t to_copy = ssid_len < avail ? ssid_len : avail;
        memcpy(out + len, ssid, to_copy);
        len += to_copy;
        out[len] = '\0';

        count++;
    }

    return count;
}

static void log_pineap_details(pineap_network_t *network,
                               const char *title,
                               const char *ssids_str,
                               int ssid_count) {
    if (!network)
        return;

    char mac_str[18];
    format_mac_address(network->bssid, mac_str, sizeof(mac_str), false);
    const char *heading = title && title[0] ? title : "Pineapple detected!";

    IRAM_PRINTF("\n%s\nBSSID: %s\n", heading, mac_str);
    IRAM_PRINTF("Channel: %d\n", network->last_channel);
    IRAM_PRINTF("RSSI: %d\n", network->last_rssi);
    IRAM_PRINTF("SSIDs (%d): %s\n", ssid_count, ssids_str ? ssids_str : "");

    glog("\n%s\n", heading);
    glog("BSSID: %s\n", mac_str);
    glog("Channel: %d\n", network->last_channel);
    glog("RSSI: %d\n", network->last_rssi);
    glog("SSIDs (%d): %s\n", ssid_count, ssids_str ? ssids_str : "");

}

static void log_oui_match_notice(pineap_network_t *network) {
    if (!network || !network->has_pineapple_oui || network->oui_logged)
        return;

    char ssids_str[256] = {0};
    int valid_ssids = build_recent_ssids_string(network, ssids_str, sizeof(ssids_str));
    log_pineap_details(network, "Pineapple OUI match!", ssids_str, valid_ssids);
    network->oui_logged = true;
}

void wifi_pineap_detector_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!pineap_detection_active || type != WIFI_PKT_MGMT)
        return;

    const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)ppkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));  // Copy to avoid unaligned pointer
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    // Only process beacon frames
    if (!is_beacon_packet(ppkt))
        return;

    // Early filtering
    if (!is_packet_valid(ppkt, type)) {
        return;
    }

    // Channel filtering
    if (!is_on_target_channel(ppkt, current_channel)) {
        return;
    }

    // Find or create network
    pineap_network_t *network = find_or_create_network(hdr->addr3);
    if (!network)
        return;

    // Update channel and RSSI
    network->last_channel = ppkt->rx_ctrl.channel;
    network->last_rssi = ppkt->rx_ctrl.rssi;

    log_oui_match_notice(network);

    // Extract SSID from beacon
    const uint8_t *payload = ppkt->payload;
    int len = ppkt->rx_ctrl.sig_len;

    // Skip fixed parameters (24 bytes header + 12 bytes fixed params)
    int index = 36;
    if (index + 2 > len)
        return;

    // Look specifically for SSID element (ID = 0)
    if (payload[index] != 0)
        return;

    uint8_t ie_len = payload[index + 1];
    if (ie_len > 32 || index + 2 + ie_len > len)
        return;

    // Get SSID
    char ssid[33] = {0};
    memcpy(ssid, &payload[index + 2], ie_len);
    ssid[ie_len] = '\0';
    trim_trailing(ssid);

    // Only proceed if this is a valid and unique SSID
    if (!is_valid_unique_ssid(ssid, network))
        return;

    uint32_t ssid_hash = hash_ssid(ssid);

    // If this is a new SSID hash for this BSSID, add it
    if (!ssid_hash_exists(network, ssid_hash) && network->ssid_count < MAX_SSIDS_PER_BSSID) {
        network->ssid_hashes[network->ssid_count++] = ssid_hash;

        // Add to recent SSIDs circular buffer
        strncpy(network->recent_ssids[network->recent_ssid_index], ssid, 32);
        network->recent_ssids[network->recent_ssid_index][32] = '\0';
        network->recent_ssid_index = (network->recent_ssid_index + 1) % RECENT_SSID_COUNT;

        // If we detect multiple SSIDs from same BSSID, mark as potential Pineap
        if (network->ssid_count >= MIN_SSIDS_FOR_DETECTION &&
            (!is_blacklisted(hdr->addr3) || should_update_blacklisted(hdr->addr3))) {

            network->is_pineap = true;
            add_to_blacklist(hdr->addr3);

            if (!network->log_pending && s_pineap_log_queue != NULL) {
                pineap_log_event_t ev = {
                    .network_index = (uint8_t)(network - pineap_networks),
                    .due_ms = now_ms_u32() + 5000
                };
                network->log_due_ms = ev.due_ms;
                network->log_pending = true;
                if (xQueueSend(s_pineap_log_queue, &ev, 0) != pdTRUE) {
                    network->log_pending = false;
                    network->log_due_ms = 0;
                }
            }

            // Write to PCAP if capture is active
            if (pcap_is_capturing()) {
                enqueue_pcap_write(ppkt->payload, ppkt->rx_ctrl.sig_len);
            }
        }
    }
}

static void trim_trailing(char *str) {
    int i = strlen(str) - 1;
    while (i >= 0 && (str[i] == ' ' || str[i] == '\t' || str[i] == '\n' || str[i] == '\r')) {
        str[i] = '\0';
        i--;
    }
}

void gps_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
    static bool gps_fix_xp_awarded = false;
    switch (event_id) {
    case GPS_UPDATE:
        gps = (gps_t *)event_data;
        gps_manager_update_local_snapshot(gps);
        gps_manager_note_update();
        gps_try_sync_time_from_fix(gps);
        
        // Add status display messages for GPS fix status
        if (gps->valid && gps->fix >= GPS_FIX_GPS && gps->fix_mode >= GPS_MODE_2D && gps->sats_in_use > 0) {
            if (!gps_fix_xp_awarded) {
                ghostchi_manager_add_xp(15);
                gps_fix_xp_awarded = true;
            }
            if (gps->fix_mode == GPS_MODE_3D) {
                status_display_show_status("GPS 3D Lock");
            } else {
                status_display_show_status("GPS 2D Lock");
            }
        } else if (gps->valid && gps->fix == GPS_FIX_INVALID) {
            gps_fix_xp_awarded = false;
            status_display_show_status("GPS No Fix");
        }
        break;
    default:
        break;
    }
}

static bool compare_bssid(const uint8_t *bssid1, const uint8_t *bssid2) {
    for (int i = 0; i < 6; i++) {
        if (bssid1[i] != bssid2[i]) {
            return false;
        }
    }
    return true;
}

static bool is_pineapple_oui(const uint8_t *bssid) {
    if (!bssid)
        return false;

    if (bssid[1] == 0x13 && bssid[2] == 0x37)
        return true;

    for (size_t i = 0; i < pineapple_oui_count; i++) {
        if (memcmp(bssid, pineapple_ouis[i], 3) == 0) {
            return true;
        }
    }
    return false;
}

bool is_network_duplicate(const char *ssid, const uint8_t *bssid) {
    if (!detected_wps_networks) {
        detected_wps_networks = mon_tbl_calloc(MAX_WPS_NETWORKS, sizeof(*detected_wps_networks));
        if (!detected_wps_networks) return false;
    }
    for (int i = 0; i < detected_network_count; i++) {
        if (strcmp(detected_wps_networks[i].ssid, ssid) == 0 &&
            compare_bssid(detected_wps_networks[i].bssid, bssid)) {
            return true;
        }
    }
    return false;
}

void get_frame_type_and_subtype(const wifi_promiscuous_pkt_t *pkt, uint8_t *frame_type,
                                uint8_t *frame_subtype) {
    if (pkt->rx_ctrl.sig_len < 24) {
        *frame_type = 0xFF;
        *frame_subtype = 0xFF;
        return;
    }

    const uint8_t *frame_ctrl = pkt->payload;

    *frame_type = (frame_ctrl[0] & 0x0C) >> 2;
    *frame_subtype = (frame_ctrl[0] & 0xF0) >> 4;
}

bool is_beacon_packet(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_BEACON);
}

bool is_deauth_packet(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_DEAUTH);
}

bool is_probe_request(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_PROBE_REQ);
}

bool is_probe_response(const wifi_promiscuous_pkt_t *pkt) {
    uint8_t frame_type, frame_subtype;
    get_frame_type_and_subtype(pkt, &frame_type, &frame_subtype);
    return (frame_type == WIFI_PKT_MGMT && frame_subtype == WIFI_PKT_PROBE_RESP);
}

bool is_eapol_response(const wifi_promiscuous_pkt_t *pkt) {
    if (pkt->rx_ctrl.sig_len < 34) {
        return false;
    }

    const uint8_t *frame = pkt->payload;

    if ((frame[30] == 0x88 && frame[31] == 0x8E) || (frame[32] == 0x88 && frame[33] == 0x8E)) {
        return true;
    }

    return false;
}

bool is_pwn_response(const wifi_promiscuous_pkt_t *pkt) {
    const uint8_t *frame = pkt->payload;

    if (frame[0] == 0x80) {
        return true;
    }

    return false;
}

void wifi_raw_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Early filtering - raw captures everything but still filter junk
    if (type == WIFI_PKT_MISC || pkt->rx_ctrl.sig_len < MIN_PACKET_LENGTH) {
        return;
    }

    wifi_raw_observer_t observer = s_wifi_raw_observer;
    if (observer) observer(pkt, type);
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

static void wardrive_process_helper_tx(const wardriving_data_t *data) {
    uint8_t bssid[6];
    unsigned b[6];
    if (sscanf(data->bssid, "%02x:%02x:%02x:%02x:%02x:%02x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return;
    for (int i = 0; i < 6; ++i) bssid[i] = (uint8_t)b[i];
    const char *ssid = data->ssid;
    int rssi = data->rssi, channel = data->channel;
    const char *encryption_type = data->encryption_type;
    const char *tx_ssid = (ssid[0] == '\0') ? "" : ssid;
    uint32_t hash = wardrive_hash_bssid(bssid);
    if (!wardrive_helper_dedupe) {
        wardrive_helper_dedupe = mon_tbl_calloc(WARDRIVE_HELPER_DEDUPE_SIZE, sizeof(*wardrive_helper_dedupe));
    }
    uint8_t changed_idx = wardrive_helper_dedupe_idx;
    wardrive_helper_dedupe_t previous = {0};
    if (wardrive_helper_dedupe) {
        for (uint8_t i = 0; i < WARDRIVE_HELPER_DEDUPE_SIZE; i++) {
            if (wardrive_helper_dedupe[i].used && wardrive_helper_dedupe[i].hash == hash && memcmp(wardrive_helper_dedupe[i].bssid, bssid, 6) == 0) {
                changed_idx = i;
                break;
            }
        }
        previous = wardrive_helper_dedupe[changed_idx];
    }
    uint8_t previous_next_idx = wardrive_helper_dedupe_idx;
    uint32_t previous_new = wardrive_helper_tx_new;
    uint32_t previous_promo = wardrive_helper_tx_ssid_promo;
    uint32_t previous_rssi = wardrive_helper_tx_rssi;
    uint32_t previous_refresh = wardrive_helper_tx_refresh;
    if (wardrive_helper_should_send(bssid, (int8_t)rssi, tx_ssid)) {
        bool send_ok = wardrive_send_helper_observation(bssid,
                                                        (uint8_t)channel,
                                                        (int8_t)rssi,
                                                        wardrive_select_auth_code(encryption_type),
                                                        tx_ssid);
        if (send_ok) {
            wardrive_helper_stream_send_ok++;
        } else {
            if (wardrive_helper_dedupe) {
                wardrive_helper_dedupe[changed_idx] = previous;
            }
            wardrive_helper_dedupe_idx = previous_next_idx;
            wardrive_helper_tx_new = previous_new;
            wardrive_helper_tx_ssid_promo = previous_promo;
            wardrive_helper_tx_rssi = previous_rssi;
            wardrive_helper_tx_refresh = previous_refresh;
            wardrive_helper_stream_send_fail++;
        }
    }

}

static void wardrive_submit_ap(const wd_ap_t *ap) {
    wardriving_data_t data = {0};
    memcpy(data.ssid, ap->ssid, sizeof(data.ssid));
    snprintf(data.bssid, sizeof(data.bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
             ap->bssid[0], ap->bssid[1], ap->bssid[2], ap->bssid[3], ap->bssid[4], ap->bssid[5]);
    data.rssi = ap->rssi;
    data.channel = ap->channel;
    strncpy(data.encryption_type, wardrive_auth_code_to_string(ap->auth), sizeof(data.encryption_type) - 1);
    (void)wardrive_obs_submit(&data, wardrive_role == WARDRIVE_ROLE_HELPER ? WARDRIVE_OBS_HELPER_TX : WARDRIVE_OBS_LOCAL);
}

static void wardrive_active_result(const wifi_ap_record_t *record, void *ctx) {
    (void)ctx;
    wd_ap_t ap = {.channel = record->primary, .rssi = record->rssi};
    if (!wd_channel_valid(ap.channel) || (record->bssid[0] & 1)) return;
    memcpy(ap.bssid, record->bssid, 6);
    memcpy(ap.ssid, record->ssid, 32);
    for (size_t i = 0; i < 32 && ap.ssid[i]; ++i)
        if ((uint8_t)ap.ssid[i] < 0x20 || (uint8_t)ap.ssid[i] == 0x7f) ap.ssid[i] = '?';
    switch (record->authmode) {
        case WIFI_AUTH_OPEN: ap.auth = WD_AUTH_OPEN; break;
        case WIFI_AUTH_WEP: ap.auth = WD_AUTH_WEP; break;
        case WIFI_AUTH_WPA_PSK: ap.auth = WD_AUTH_WPA; break;
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK: ap.auth = WD_AUTH_WPA3; break;
        case WIFI_AUTH_OWE: ap.auth = WD_AUTH_OWE; break;
        default: ap.auth = WD_AUTH_WPA2; break;
    }
    wardrive_submit_ap(&ap);
}

void wardriving_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf || type != WIFI_PKT_MGMT || wardrive_active_scan_enabled) return;
    const wifi_promiscuous_pkt_t *pkt = buf;
    if (pkt->rx_ctrl.rx_state != 0) return;
    size_t len;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    // HE targets supply the captured MPDU length without FCS.
    len = pkt->rx_ctrl.dump_len;
    if (len > pkt->rx_ctrl.sig_len) return;
#else
    if (pkt->rx_ctrl.sig_len < 4) return;
    len = pkt->rx_ctrl.sig_len - 4;
#endif
    if (len < 36 || (pkt->payload[0] != 0x80 && pkt->payload[0] != 0x50)) return;
    wardrive_wifi_frames_seen++;
    wd_ap_t ap;
    if (!wd_parse_ap(pkt->payload, len, pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, &ap)) {
        wardrive_parser_rejected++;
        return;
    }
    wardrive_submit_ap(&ap);
}

void wifi_probe_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_beacon_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Beacon-specific filtering - only capture beacon frames
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_BEACON) return;
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_deauth_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;
    
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Deauth-specific filtering - only capture deauth/disassoc frames
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_DEAUTH && frame_subtype != 0x0A) return; // 0x0A = disassoc
    
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_pwn_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT)
        return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (!is_packet_valid(pkt, type)) return;
    if (pkt->rx_ctrl.sig_len > 0) {
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
    }
}

void wifi_eapol_scan_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

    if (type == WIFI_PKT_MISC) return;
    if (pkt->rx_ctrl.sig_len < 24) return;

    if (type == WIFI_PKT_DATA) {
        const uint8_t *frame = pkt->payload;
        int len = pkt->rx_ctrl.sig_len;

        uint16_t fc = frame[0] | (frame[1] << 8);
        uint8_t dsub = (fc >> 4) & 0xF;
        bool qos = (dsub & 0x8) != 0;
        bool to_ds = (fc >> 8) & 0x1;
        bool from_ds = (fc >> 9) & 0x1;

        size_t hdr_len = 24;
        if (to_ds && from_ds) hdr_len = 30;
        if (qos) hdr_len += 2;

        // always write data frames to pcap first, then check for EAPOL
        enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);

        // check if this is an EAPOL frame for handshake tracking
        if (len < (int)(hdr_len + 8)) return;
        const uint8_t *llc = frame + hdr_len;
        if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) return;
        uint16_t ethertype = (llc[6] << 8) | llc[7];
        if (ethertype != 0x888E) return;

        const uint8_t *eapol = llc + 8;
        if (len < (int)(hdr_len + 8 + 17)) return;

        uint8_t key_desc_type = eapol[4];
        if (key_desc_type != 2) return;

        uint16_t key_info = (eapol[5] << 8) | eapol[6];
        bool has_mic = (key_info & 0x0100) != 0;
        bool is_pairwise = (key_info & 0x0008) != 0;
        bool is_install = (key_info & 0x0040) != 0;
        bool is_ack = (key_info & 0x0080) != 0;
        bool is_secure = (key_info & 0x0200) != 0;

        const uint8_t *addr1 = frame + 4;
        const uint8_t *addr2 = frame + 10;
        const uint8_t *ap_mac = is_ack ? addr2 : addr1;
        const uint8_t *sta_mac = is_ack ? addr1 : addr2;

        uint64_t replay = 0;
        for (int i = 0; i < 8; i++) replay = (replay << 8) | eapol[9 + i];

        if (is_pairwise) {
            uint8_t msg = 0;
            if (!has_mic && is_ack && !is_install) {
                msg = 1;  // M1: AP->STA, no MIC, no Install
            } else if (has_mic) {
                if (is_ack && is_install) msg = 3;             // M3
                else if (!is_ack && !is_install && !is_secure) msg = 2; // M2
                else if (!is_ack && !is_install && is_secure) msg = 4;  // M4
            }
            if (msg > 0) {
                process_eapol_candidate_pair(ap_mac, sta_mac, replay, is_ack, msg);
            }
        }
        return;
    }

    if (type == WIFI_PKT_MGMT) {
        const uint8_t *frame = pkt->payload;
        if (pkt->rx_ctrl.sig_len < 24) return;
        uint8_t subtype = (frame[0] & 0xF0) >> 4;

        // assoc/reassoc frames
        if (subtype == 0x0 || subtype == 0x1 || subtype == 0x2 || subtype == 0x3) {
            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            return;
        }

        // authentication frames (useful for context/sae)
        if (subtype == 0x0B) {
            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            return;
        }

        // probe request frames (capture undirected and directed) with de-duplication
        if (subtype == WIFI_PKT_PROBE_REQ) {
            const uint8_t *src = frame + 10; // addr2
            // parse SSID element
            char ssid[33] = {0};
            bool ssid_found = false;
            int index = 24;
            if (pkt->rx_ctrl.sig_len > index) {
                const uint8_t *body = frame + index;
                int body_len = pkt->rx_ctrl.sig_len - index;
                for (int i = 0; i < body_len - 1; i += 2 + body[i+1]) {
                    uint8_t tag_num = body[i];
                    uint8_t tag_len = body[i+1];
                    if (tag_num == 0 && tag_len < sizeof(ssid) && i + 2 + tag_len <= body_len) {
                        memcpy(ssid, &body[i+2], tag_len);
                        ssid[tag_len] = '\0';
                        if (tag_len == 0) strcpy(ssid, "Broadcast");
                        ssid_found = true;
                        break;
                    }
                }
                if (!ssid_found) strcpy(ssid, "Broadcast");
            }
            uint32_t h = hash_ssid(ssid);
            uint64_t now_ms = esp_timer_get_time() / 1000ULL;
            if (probe_should_emit(src, h, now_ms)) {
                enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
            }
            return;
        }

        // limited beacons and probe responses
        if (subtype == WIFI_PKT_BEACON || subtype == WIFI_PKT_PROBE_RESP) {
            if (pkt->rx_ctrl.sig_len >= 38) {
                const uint8_t *bssid = frame + 16;
                uint8_t ssid_len = frame[37];
                bool ssid_nonempty = ssid_len > 0;
                if (beacon_should_emit_limited(bssid, ssid_nonempty)) {
                    enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
                }
            }
            return;
        }
    }
}

void wifi_wps_detection_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) {
        return;
    }
    if (!detected_wps_networks) {
        detected_wps_networks = mon_tbl_calloc(MAX_WPS_NETWORKS, sizeof(*detected_wps_networks));
        if (!detected_wps_networks) return;
    }

    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;

    const uint8_t *payload = pkt->payload;
    int len = pkt->rx_ctrl.sig_len;

    uint8_t frame_type = hdr->frame_ctrl & 0xFC;
    if (frame_type != 0x80 && frame_type != 0x50) {
        return;
    }

    int index = 36;
    char ssid[33] = {0};
    uint8_t bssid[6];
    memcpy(bssid, hdr->addr3, 6);

    while (index + 1 < len) {
        uint8_t id = payload[index];
        uint8_t ie_len = payload[index + 1];

        /* sanity checks: ensure IE length fits within bounds */
        if (index + 2 + ie_len > len) {
            break;
        }

        if (id == 0 && ie_len <= 32) {
            memcpy(ssid, &payload[index + 2], ie_len);
            ssid[ie_len] = '\0';
            trim_trailing(ssid);
        }

        if (is_network_duplicate(ssid, bssid)) {
            return;
        }

        if (id == 221 && ie_len >= 4) {
            uint32_t oui =
                (payload[index + 2] << 16) | (payload[index + 3] << 8) | payload[index + 4];
            uint8_t oui_type = payload[index + 5];

            if (oui == 0x0050f2 && oui_type == 0x04) {
                int attr_index = index + 6;
                int wps_ie_end = index + 2 + ie_len;

                while (attr_index + 4 <= wps_ie_end) {
                    uint16_t attr_id = (payload[attr_index] << 8) | payload[attr_index + 1];
                    uint16_t attr_len = (payload[attr_index + 2] << 8) | payload[attr_index + 3];

                    /* sanity: attr_len must be reasonable and fit inside the WPS IE */
                    if (attr_len > MAX_IE_LEN || attr_len > (wps_ie_end - (attr_index + 4))) {
                        break;
                    }

                    if (attr_id == 0x1008 && attr_len == 2) {
                        uint16_t config_methods =
                            (payload[attr_index + 4] << 8) | payload[attr_index + 5];

                        IRAM_PRINTF("Configuration Methods found: 0x%04x\n", config_methods);

                        if (config_methods & WPS_CONF_METHODS_PBC) {
                            glog("WPS Push Button detected:\n%s\n", ssid);
                        } else if (config_methods &
                                   (WPS_CONF_METHODS_PIN_DISPLAY | WPS_CONF_METHODS_PIN_KEYPAD)) {
                            glog("WPS PIN detected:\n%s\n", ssid);
                        }

                        if (should_store_wps == 1) {
                            if (detected_network_count < MAX_WPS_NETWORKS) {
                                wps_network_t new_network;
                                strncpy(new_network.ssid, ssid, sizeof(new_network.ssid) - 1);
                                new_network.ssid[sizeof(new_network.ssid) - 1] = '\0';
                                memcpy(new_network.bssid, bssid, sizeof(new_network.bssid));
                                new_network.wps_enabled = true;
                                new_network.wps_mode = config_methods & (WPS_CONF_METHODS_PIN_DISPLAY |
                                                                         WPS_CONF_METHODS_PIN_KEYPAD)
                                                           ? WPS_MODE_PIN
                                                           : WPS_MODE_PBC;
                                detected_wps_networks[detected_network_count++] = new_network;
                            }
                        } else {
                            enqueue_pcap_write(pkt->payload, pkt->rx_ctrl.sig_len);
                        }

                        if (detected_network_count >= MAX_WPS_NETWORKS) {
                            glog("Maximum number of WPS networks detected\nStopping "
                                 "monitor mode.\n");
                            wifi_manager_stop_monitor_mode();
                        }
                    }

                    attr_index += (4 + attr_len);
                }
            }
        }

        index += (2 + ie_len);
    }
}

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#ifndef BLE_HS_ADV_TYPE_APPEARANCE
#define BLE_HS_ADV_TYPE_APPEARANCE 0x19
#endif

struct ble_hs_adv_field;
static int ble_hs_adv_parse_fields_cb(const struct ble_hs_adv_field *field, void *arg);

static const char SKIMMER_TAG[] STORE_STR_ATTR = "SKIMMER_DETECT";

struct ble_adv_parse_arg {
    wardriving_data_t *wd;
};

void ble_wardriving_callback(struct ble_gap_event *event, void *arg) {
    if (!event || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    wardrive_ble_advs_seen++;

    uint32_t mac_hash = ble_wd_hash_mac(event->disc.addr.val);
    bool already_seen = false;
    portENTER_CRITICAL(&ble_wd_mux);
    for (int i = 0; i < BLE_WD_SEEN_SIZE; i++) {
        if (ble_wd_seen_hashes[i] == mac_hash) { already_seen = true; break; }
    }
    if (!already_seen) {
        ble_wd_seen_hashes[ble_wd_seen_idx] = mac_hash;
        ble_wd_seen_idx = (ble_wd_seen_idx + 1) % BLE_WD_SEEN_SIZE;
        ble_wd_unique_count++;
    }
    portEXIT_CRITICAL(&ble_wd_mux);

    wardriving_data_t wardriving_data = {0};
    wardriving_data.ble_data.is_ble_device = true;

    snprintf(wardriving_data.ble_data.ble_mac, sizeof(wardriving_data.ble_data.ble_mac),
             "%02x:%02x:%02x:%02x:%02x:%02x", event->disc.addr.val[0], event->disc.addr.val[1],
             event->disc.addr.val[2], event->disc.addr.val[3], event->disc.addr.val[4],
             event->disc.addr.val[5]);

    wardriving_data.ble_data.ble_rssi = event->disc.rssi;

    if (event->disc.length_data > 0) {
        parse_ble_device_name(event->disc.data, event->disc.length_data,
                              wardriving_data.ble_data.ble_name,
                              sizeof(wardriving_data.ble_data.ble_name));
        struct ble_adv_parse_arg parse_arg = {.wd = &wardriving_data};
        ble_hs_adv_parse(event->disc.data, event->disc.length_data, ble_hs_adv_parse_fields_cb,
                         &parse_arg);
    }

    // Get GPS data from the global handle, if available
    gps_t gps_local = {0};
    if (gps_manager_get_local_gps_snapshot(&gps_local) && gps_local.valid) {
        wardriving_data.gps_quality.satellites_used = gps_local.sats_in_use;
        wardriving_data.gps_quality.hdop = gps_local.dop_h;
        wardriving_data.gps_quality.speed = gps_local.speed;
        wardriving_data.gps_quality.course = gps_local.cog;
        wardriving_data.gps_quality.fix_quality = gps_local.fix;
        wardriving_data.gps_quality.has_valid_fix = (gps_local.fix >= GPS_FIX_GPS);
    }
    

    // Use GPS manager to log data
    wardrive_log_attempts++;
    esp_err_t err = gps_manager_log_wardriving_data(&wardriving_data);
    if (err == ESP_ERR_INVALID_STATE) {
        wardrive_gps_rejected++;
    } else if (err == ESP_OK) {
        wardrive_log_ok++;
    }
}

// Move the callback implementation inside the ESP32S2 guard
static int ble_hs_adv_parse_fields_cb(const struct ble_hs_adv_field *field, void *arg) {
    struct ble_adv_parse_arg *p = (struct ble_adv_parse_arg *)arg;
    wardriving_data_t *data = p ? p->wd : NULL;
    if (data == NULL || field == NULL) {
        return 0;
    }

    if (field->type == BLE_HS_ADV_TYPE_COMP_NAME) {
        size_t name_len = MIN(field->length, sizeof(data->ble_data.ble_name) - 1);
        memcpy(data->ble_data.ble_name, field->value, name_len);
        data->ble_data.ble_name[name_len] = '\0';
    } else if (field->type == BLE_HS_ADV_TYPE_INCOMP_NAME && data->ble_data.ble_name[0] == '\0') {
        size_t name_len = MIN(field->length, sizeof(data->ble_data.ble_name) - 1);
        memcpy(data->ble_data.ble_name, field->value, name_len);
        data->ble_data.ble_name[name_len] = '\0';
    }

    if (field->type == BLE_HS_ADV_TYPE_MFG_DATA && field->length >= 2) {
        const uint8_t *v = (const uint8_t *)field->value;
        data->ble_data.ble_mfgr_id = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
        data->ble_data.ble_has_mfgr_id = true;
    }

    if (field->type == BLE_HS_ADV_TYPE_APPEARANCE && field->length >= 2) {
        const uint8_t *v = (const uint8_t *)field->value;
        data->ble_data.ble_appearance = (uint16_t)v[0] | ((uint16_t)v[1] << 8);
    }

    return 0;
}
#endif

// wrap for esp32s2
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)

static const int suspicious_names_count = sizeof(suspicious_names) / sizeof(suspicious_names[0]);
void ble_skimmer_scan_callback(struct ble_gap_event *event, void *arg) {
    if (!event || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }

    struct ble_hs_adv_fields fields;
    int rc = ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

    if (rc != 0) {
        ESP_LOGD(SKIMMER_TAG, "Failed to parse advertisement data");
        return;
    }

    // Check device name
    if (fields.name != NULL && fields.name_len > 0) {
        char device_name[32] = {0};
        size_t name_len = MIN(fields.name_len, sizeof(device_name) - 1);
        memcpy(device_name, fields.name, name_len);

        // Check against suspicious names
        for (int i = 0; i < suspicious_names_count; i++) {
            if (strcasecmp(device_name, suspicious_names[i]) == 0) {
                char mac_addr[18];
                snprintf(mac_addr, sizeof(mac_addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                         event->disc.addr.val[0], event->disc.addr.val[1], event->disc.addr.val[2],
                         event->disc.addr.val[3], event->disc.addr.val[4], event->disc.addr.val[5]);

                glog("\nPOTENTIAL SKIMMER DETECTED!\n");
                
                glog("Device Name: %s\n", device_name);

                glog("MAC Address: %s\n", mac_addr);

                glog("RSSI: %d dBm\n", event->disc.rssi);

                glog("Reason:\nMatched known skimmer pattern: %s\n", suspicious_names[i]);

                glog("Please verify before taking action.\n\n");

                // pulse rgb red once when skimmer is detected
                pulse_once(&rgb_manager, 255, 0, 0);

                // Create enhanced PCAP packet with metadata
                if (pcap_is_capturing()) {
                    // Format: [Timestamp][MAC][RSSI][Name][Raw Data]
                    uint8_t enhanced_packet[256] = {0};
                    size_t packet_len = 0;

                    // Add MAC address
                    memcpy(enhanced_packet + packet_len, event->disc.addr.val, 6);
                    packet_len += 6;

                    // Add RSSI
                    enhanced_packet[packet_len++] = (uint8_t)event->disc.rssi;

                    // Add device name length and name
                    if (packet_len + 1 + name_len > sizeof(enhanced_packet)) break;
                    enhanced_packet[packet_len++] = (uint8_t)name_len;
                    memcpy(enhanced_packet + packet_len, device_name, name_len);
                    packet_len += name_len;

                    // Add reason for flagging
                    const char *reason = suspicious_names[i];
                    uint8_t reason_len = strlen(reason);
                    if (packet_len + 1 + reason_len > sizeof(enhanced_packet)) break;
                    enhanced_packet[packet_len++] = reason_len;
                    memcpy(enhanced_packet + packet_len, reason, reason_len);
                    packet_len += reason_len;

                    // Add raw advertisement data
                    if (packet_len + event->disc.length_data > sizeof(enhanced_packet)) break;
                    memcpy(enhanced_packet + packet_len, event->disc.data, event->disc.length_data);
                    packet_len += event->disc.length_data;

                    // Write to PCAP with proper BLE packet format
                    pcap_write_packet_to_buffer(enhanced_packet, packet_len,
                                                PCAP_CAPTURE_BLUETOOTH);

                    // Force flush to ensure suspicious device is captured
                    pcap_flush_buffer_to_file();
                }
                break;
            }
        }
    }
}
#endif

// Packet statistics for monitoring filter effectiveness
static uint32_t total_packets_received = 0;
static uint32_t packets_filtered_out = 0;
static uint32_t packets_processed = 0;

// Early filtering helper - checks basic packet validity
static inline bool is_packet_valid(const wifi_promiscuous_pkt_t *pkt, wifi_promiscuous_pkt_type_t type) {
    total_packets_received++;
    
    // Drop MISC packets immediately
    if (type == WIFI_PKT_MISC) {
        packets_filtered_out++;
        return false;
    }
    
    // Check minimum length
    if (pkt->rx_ctrl.sig_len < MIN_PACKET_LENGTH) {
        packets_filtered_out++;
        return false;
    }
    
    // RSSI threshold filtering
    if (pkt->rx_ctrl.rssi < MIN_RSSI_THRESHOLD) {
        packets_filtered_out++;
        return false;
    }
    
    packets_processed++;
    
    // Log stats less frequently to reduce spam (every ~20000 packets)
    if (total_packets_received % 20000 == 0) {
        char stats_msg[128];
        snprintf(stats_msg, sizeof(stats_msg), "Filter stats: %lu total, %lu filtered, %lu processed (%.1f%% filtered)", 
                (unsigned long)total_packets_received, 
                (unsigned long)packets_filtered_out,
                (unsigned long)packets_processed,
                (float)packets_filtered_out * 100.0f / total_packets_received);
        
        glog("%s\n", stats_msg);
    }
    
    return true;
}

// Channel filtering helper
static inline bool is_on_target_channel(const wifi_promiscuous_pkt_t *pkt, uint8_t target_channel) {
    return (target_channel == 0) || (pkt->rx_ctrl.channel == target_channel);
}

// Flag indicating whether to save probe PCAP data to SD (disable UART fallback if false)
bool g_listen_probes_save_to_sd = false;

// (last_probe_log / last_probe_log_time_ms are heap-on-demand, declared above)

void wifi_listen_probes_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    // Early filtering for management frames only
    if (type != WIFI_PKT_MGMT) return;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    
    // Additional early filtering
    if (!is_packet_valid(pkt, type)) return;
    
    // Quick probe request check using frame subtype
    uint8_t frame_subtype = (pkt->payload[0] & 0xF0) >> 4;
    if (frame_subtype != WIFI_PKT_PROBE_REQ) return;

    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));  // Copy to avoid unaligned pointer
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    const uint8_t *payload = pkt->payload;

    // Extract source and dest MAC and SSID as before...
    char src_mac_str[18];
    format_mac_address(hdr->addr2, src_mac_str, sizeof(src_mac_str), false);
    char dest_mac_str[18];
    format_mac_address(hdr->addr1, dest_mac_str, sizeof(dest_mac_str), false);
    int index = 24;
    char ssid[33] = {0};
    bool ssid_found = false;
    if (pkt->rx_ctrl.sig_len > index) {
        const uint8_t *body = payload + index;
        int body_len = pkt->rx_ctrl.sig_len - index;
        for (int i = 0; i < body_len - 1; i += 2 + body[i+1]) {
            uint8_t tag_num = body[i];
            uint8_t tag_len = body[i+1];
            if (tag_num == 0 && tag_len < sizeof(ssid) && i + 2 + tag_len <= body_len) {
                memcpy(ssid, &body[i+2], tag_len);
                ssid[tag_len] = '\0';
                if (tag_len == 0) strcpy(ssid, "Broadcast");
                ssid_found = true;
                break;
            }
        }
        if (!ssid_found) strcpy(ssid, "Broadcast");
    }

    // Build log message
    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "Probe Req: %s -> %s for %s", src_mac_str, dest_mac_str, ssid);

    // Deduplicate: skip if same message within timeout
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    if (!last_probe_log) {
        last_probe_log = mon_tbl_calloc(128, 1);
    }
    if (last_probe_log && strcmp(log_msg, last_probe_log) == 0 && (now_ms - last_probe_log_time_ms) < PROBE_DEDUPE_TIMEOUT_MS) {
        return;
    }
    if (last_probe_log) {
        strcpy(last_probe_log, log_msg);
        last_probe_log_time_ms = now_ms;
    }

    // Optionally save packet to SD if enabled
    if (g_listen_probes_save_to_sd && pkt->rx_ctrl.sig_len > 0) {
        esp_err_t ret = pcap_write_packet_to_buffer(payload, pkt->rx_ctrl.sig_len, PCAP_CAPTURE_WIFI);
        if (ret != ESP_OK) {
            ESP_LOGE("PROBE_LISTEN", "Failed to write packet to buffer");
        }
    }

    // Print to console and display
    glog("%s\n", log_msg);
}
