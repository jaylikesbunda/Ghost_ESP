/**
 * Wigle.net upload manager.
 * Uploads wardriving CSV files (sweeps, GPS logs) to Wigle via API.
 * API: https://api.wigle.net/api/v2/file/upload
 * Auth: Basic (APIName:APIToken from wigle.net/account)
 */

#include "managers/wigle_manager.h"
#include "managers/settings_manager.h"
#include "managers/sd_card_manager.h"
#include "core/glog.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "managers/http_proxy.h"
#include "mbedtls/base64.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_netif.h"

#define WIGLE_UPLOAD_URL "https://api.wigle.net/api/v2/file/upload"
#define WIGLE_BOUNDARY "------------------------GhostESP"
#define WIGLE_UPLOADED_FILE "/mnt/ghostesp/.wigle_uploaded"
#define WIGLE_QUEUE_FILE "/mnt/ghostesp/.wigle_queue"
/* Max queue entries processed per connect; kept small to limit RAM. */
#define WIGLE_QUEUE_BATCH_MAX 16
#define WIGLE_STREAM_CHUNK_SIZE 4096
#define AUTH_BUF_SIZE 256
#define WIGLE_RESP_BUF_SIZE 2048
#define WIGLE_TASK_STACK (12 * 1024)
/* JIT-mount upload limits */
#define WIGLE_JIT_MAX_ENTRIES   32             /* max paths per scan */

static volatile bool wigle_upload_in_progress = false;
static wigle_test_callback_t wigle_test_cb = NULL;
static wigle_test_callback_t wigle_manual_upload_cb = NULL;
static wigle_test_callback_t wigle_stats_cb = NULL;
static bool wigle_test_in_progress = false;
static bool wigle_manual_upload_in_progress = false;
static bool wigle_stats_in_progress = false;

void wigle_set_test_callback(wigle_test_callback_t callback) {
    wigle_test_cb = callback;
}

void wigle_set_manual_upload_callback(wigle_test_callback_t callback) {
    wigle_manual_upload_cb = callback;
}

void wigle_set_stats_callback(wigle_test_callback_t callback) {
    wigle_stats_cb = callback;
}

bool wigle_is_test_in_progress(void) {
    return wigle_test_in_progress;
}

bool wigle_is_manual_upload_in_progress(void) {
    return wigle_manual_upload_in_progress;
}

bool wigle_is_stats_in_progress(void) {
    return wigle_stats_in_progress;
}

typedef struct {
    char buf[WIGLE_RESP_BUF_SIZE];
    size_t len;
} wigle_resp_t;

static esp_err_t wigle_http_event(esp_http_client_event_t *evt) {
    wigle_resp_t *r = (wigle_resp_t *)evt->user_data;
    if (!r || evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    size_t copy = evt->data_len;
    if (r->len + copy >= WIGLE_RESP_BUF_SIZE) copy = WIGLE_RESP_BUF_SIZE - r->len - 1;
    if (copy > 0) {
        memcpy(r->buf + r->len, evt->data, copy);
        r->len += copy;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

void wigle_set_api_key(const char *key) {
    if (!key) return;
    FSettings *s = &G_Settings;
    strncpy(s->wigle_api_key, key, sizeof(s->wigle_api_key) - 1);
    s->wigle_api_key[sizeof(s->wigle_api_key) - 1] = '\0';
    settings_persist_setting(SETTING_WIGLE_API_KEY);
}

const char *wigle_get_api_key(void) {
    return G_Settings.wigle_api_key;
}

/**
 * Upload a single file to Wigle.
 * Builds multipart body: file + donate=on.
 * Only uploads files with WigleWifi pre-header (skips sweep CSV).
 */
static bool wigle_file_is_valid_format(FILE *f) {
    char line[80];
    if (!fgets(line, sizeof(line), f)) return false;
    return (strstr(line, "WigleWifi") != NULL);
}

/* Check STA has IP; return true if ready for upload */
static bool wigle_sta_has_ip(void) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return false;
    esp_netif_ip_info_t ip = {0};
    return (esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0);
}

static bool wigle_require_jit_mount(void) {
    return sd_card_needs_jit_mount();
}

static bool wigle_is_safe_csv_name(const char *name) {
    if (!name || !name[0]) return false;
    if (strlen(name) >= MAX_PORTAL_NAME) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    size_t len = strlen(name);
    if (len <= 4 || strcasecmp(name + len - 4, ".csv") != 0) return false;
    return true;
}

static esp_err_t wigle_mount_if_needed(bool *did_mount, bool *display_was_suspended) {
    if (did_mount) *did_mount = false;
    if (display_was_suspended) *display_was_suspended = false;
    if (sd_card_manager.is_initialized) return ESP_OK;
    esp_err_t err = sd_card_mount_for_flush(display_was_suspended);
    if (err != ESP_OK) return err;
    if (did_mount) *did_mount = true;
    return ESP_OK;
}

static void wigle_unmount_if_needed(bool did_mount, bool display_was_suspended) {
    if (did_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
}

/* In-RAM cache of the uploaded-history file so wigle_uploaded_check() does
 * not have to linearly re-walk the SD-resident log on every call (was O(N)
 * per file, which stalled the upload loop on long wardriving sessions).
 * Real basenames (e.g. "biscuit_wardrive_20260423_151052.csv") are <40 chars;
 * ",<size up to 10 digits>\n" pads to <55, so 64 B/row is enough. 32 rows
 * covers a typical multi-session boot with plenty of headroom.
 *
 * The cache is heap-allocated on first use (PSRAM preferred, internal RAM
 * fallback) so the file contributes zero BSS. Only a single pointer to the
 * cache struct lives in BSS. If allocation fails we silently fall back to
 * a direct SD scan on every check. */
#define WIGLE_UPLOADED_CACHE_ROWS 32
#define WIGLE_UPLOADED_CACHE_ROW  64

typedef struct {
    int  n;
    bool loaded;
    char rows[WIGLE_UPLOADED_CACHE_ROWS][WIGLE_UPLOADED_CACHE_ROW];
} wigle_uploaded_cache_t;

static wigle_uploaded_cache_t *s_wigle_cache = NULL;

static wigle_uploaded_cache_t *wigle_uploaded_cache_get(void) {
    if (s_wigle_cache) return s_wigle_cache;
    s_wigle_cache = heap_caps_calloc(1, sizeof(wigle_uploaded_cache_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wigle_cache) {
        s_wigle_cache = heap_caps_calloc(1, sizeof(wigle_uploaded_cache_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return s_wigle_cache;
}

static void wigle_uploaded_cache_load(void) {
    wigle_uploaded_cache_t *c = wigle_uploaded_cache_get();
    if (!c) return;
    c->n = 0;
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "r");
    if (!f) {
        c->loaded = true;
        return;
    }
    char line[WIGLE_UPLOADED_CACHE_ROW];
    while (c->n < WIGLE_UPLOADED_CACHE_ROWS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        strncpy(c->rows[c->n], line, sizeof(c->rows[0]) - 1);
        c->rows[c->n][sizeof(c->rows[0]) - 1] = '\0';
        c->n++;
    }
    fclose(f);
    c->loaded = true;
}

/* Check if file (basename,size) was already uploaded. File format: basename,size\n */
static bool wigle_uploaded_check(const char *basename, long size) {
    char expect[160];
    snprintf(expect, sizeof(expect), "%s,%ld", basename, size);
    wigle_uploaded_cache_t *c = wigle_uploaded_cache_get();
    if (c && !c->loaded) {
        wigle_uploaded_cache_load();
    }
    if (c) {
        for (int i = 0; i < c->n; i++) {
            if (strcmp(c->rows[i], expect) == 0) {
                return true;
            }
        }
    }
    /* Cache miss (or no cache): fall back to a one-shot SD scan in case the
     * on-disk log has more entries than the in-RAM cache, and learn the new
     * entry if found and a cache is available. */
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "r");
    if (!f) return false;
    char line[WIGLE_UPLOADED_CACHE_ROW];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, expect) == 0) {
            found = true;
            if (c && c->n < WIGLE_UPLOADED_CACHE_ROWS) {
                strncpy(c->rows[c->n], line, sizeof(c->rows[0]) - 1);
                c->rows[c->n][sizeof(c->rows[0]) - 1] = '\0';
                c->n++;
            }
            break;
        }
    }
    fclose(f);
    return found;
}

/* Append basename,size to uploaded list after successful upload */
static void wigle_uploaded_add(const char *basename, long size) {
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "a");
    if (!f) {
        glog("Wigle: cannot append to %s\n", WIGLE_UPLOADED_FILE);
        return;
    }
    fprintf(f, "%s,%ld\n", basename, size);
    fclose(f);
    wigle_uploaded_cache_t *c = wigle_uploaded_cache_get();
    if (c && c->n < WIGLE_UPLOADED_CACHE_ROWS) {
        snprintf(c->rows[c->n], sizeof(c->rows[0]), "%s,%ld", basename, size);
        c->n++;
    }
}

/* Wigle rejects files with only pre-header + header (no data rows) */
static bool wigle_file_has_data_rows(FILE *f) {
    long pos = ftell(f);
    fseek(f, 0, SEEK_SET);
    int newlines = 0;
    int c;
    while ((c = fgetc(f)) != EOF && newlines < 3) {
        if (c == '\n') newlines++;
    }
    fseek(f, pos, SEEK_SET);
    return newlines >= 2;  /* pre-header, header, and at least one data row */
}

/**
 * Upload a single CSV file to WiGLE API.
 *
 * Multipart structure:
 *   --------------------------GhostESP\r\n
 *   Content-Disposition: form-data; name="file"; filename="test.csv"\r\n
 *   Content-Type: text/csv\r\n\r\n
 *   [file content]
 *   \r\n--------------------------GhostESP\r\n
 *   Content-Disposition: form-data; name="donate"\r\n\r\n
 *   true\r\n
 *   --------------------------GhostESP--\r\n
 *
 * Content-Type header: multipart/form-data; boundary=------------------------GhostESP
 * Note: boundary in header has NO dashes, body boundaries have "--" prefix
 */
static esp_err_t wigle_upload_file(const char *filepath, const char *api_key) {
    /* Resolve basename and file size via stat() BEFORE opening the file.
     * wigle_process_queue holds q (FD1) + q_out (FD2) open; opening f here
     * (FD3) then calling wigle_uploaded_check() (needs another FD) would
     * exceed the JIT mount's max_files=3 limit.  Using stat() avoids the
     * extra descriptor entirely. */
    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = filepath;
    else basename++;

    struct stat st;
    if (stat(filepath, &st) != 0) {
        glog("Wigle: cannot stat %s\n", filepath);
        return ESP_FAIL;
    }
    long fsize = (long)st.st_size;
    if (fsize <= 0) {
        glog("Wigle: file %s size %ld invalid\n", filepath, fsize);
        return ESP_ERR_INVALID_SIZE;
    }

    if (wigle_uploaded_check(basename, fsize)) {
        glog("Wigle: skip %s (already uploaded)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        glog("Wigle: cannot open %s\n", filepath);
        return ESP_FAIL;
    }

    if (!wigle_file_is_valid_format(f)) {
        fclose(f);
        glog("Wigle: skip %s (not Wigle CSV format)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }
    fseek(f, 0, SEEK_SET);
    if (!wigle_file_has_data_rows(f)) {
        fclose(f);
        glog("Wigle: skip %s (no data rows - need GPS fix + wardriving)\n", filepath);
        return ESP_ERR_NOT_SUPPORTED;
    }
    fseek(f, 0, SEEK_SET);

    size_t file_len = (size_t)fsize;

    /* Multipart body size:
     * boundary + headers + file + boundary + donate + closing
     */
    size_t part1_len = strlen("--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\"\r\n"
        "Content-Type: text/csv\r\n\r\n");
    size_t part1_fname = strlen(basename);
    part1_len += part1_fname;

    const char *donate_val = G_Settings.wigle_donate ? "true" : "false";
    size_t part2_len = strlen("\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n") +
        strlen(donate_val) + strlen("\r\n--" WIGLE_BOUNDARY "--\r\n");

    size_t body_len = part1_len + file_len + part2_len;

    /* Build Authorization: Basic base64(APIName:APIToken)
     * If api_key has no colon, it's already EncodedForUseToken (pre-encoded) */
    char auth_b64[AUTH_BUF_SIZE];
    if (strchr(api_key, ':') == NULL) {
        // Already encoded token - use directly
        strncpy(auth_b64, api_key, AUTH_BUF_SIZE - 1);
        auth_b64[AUTH_BUF_SIZE - 1] = '\0';
    } else {
        // Need to encode APIName:APIToken
        size_t enc_len = AUTH_BUF_SIZE - 1;
        int r = mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                      (const unsigned char *)api_key, strlen(api_key));
        if (r != 0) {
            fclose(f);
            glog("Wigle: base64 encode failed\n");
            return ESP_FAIL;
        }
        auth_b64[enc_len] = '\0';
    }

    char auth_val[6 + AUTH_BUF_SIZE + 1];
    snprintf(auth_val, sizeof(auth_val), "Basic %s", auth_b64);

    char content_type_hdr[128];
    snprintf(content_type_hdr, sizeof(content_type_hdr),
             "multipart/form-data; boundary=%s", WIGLE_BOUNDARY);

    wigle_resp_t resp = {0};

    esp_http_client_config_t config = {
        .url = WIGLE_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,
        .event_handler = wigle_http_event,
        .user_data = &resp,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        fclose(f);
        return proxy_err;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(f);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);
    esp_http_client_set_header(client, "Content-Type", content_type_hdr);

    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) {
        fclose(f);
        esp_http_client_cleanup(client);
        return err;
    }

    /* Write part1 (boundary + headers) */
    char part1[256];
    int p1_len = snprintf(part1, sizeof(part1), "--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", basename);
    int written = esp_http_client_write(client, part1, p1_len);
    if (written != p1_len) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Stream file content (heap buffer to keep task stack small) */
    char *chunk = (char *)malloc(WIGLE_STREAM_CHUNK_SIZE);
    if (!chunk) {
        fclose(f);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t remaining = file_len;
    while (remaining > 0) {
        size_t to_read = (remaining > WIGLE_STREAM_CHUNK_SIZE) ? WIGLE_STREAM_CHUNK_SIZE : remaining;
        size_t read_len = fread(chunk, 1, to_read, f);
        if (read_len == 0) break;
        written = esp_http_client_write(client, chunk, (int)read_len);
        if (written != (int)read_len) {
            free(chunk);
            fclose(f);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        remaining -= read_len;
    }
    free(chunk);
    fclose(f);

    /* Write part2 (boundary + donate + closing) */
    char part2[160];
    int p2_len = snprintf(part2, sizeof(part2), "\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n"
        "%s\r\n"
        "--" WIGLE_BOUNDARY "--\r\n", donate_val);
    written = esp_http_client_write(client, part2, p2_len);
    if (written != p2_len) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    /* After writing body manually, fetch response headers */
    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return (esp_err_t)content_len;
    }

    /* Drain response body to complete the request */
    char resp_drain[128];
    while (esp_http_client_read(client, resp_drain, sizeof(resp_drain)) > 0) {
        /* Response captured by event handler */
    }

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        glog("Wigle: upload %s failed HTTP %d\n", filepath, status);
        return ESP_FAIL;
    }
    glog("Wigle: uploaded %s\n", filepath);
    wigle_uploaded_add(basename, fsize);
    return ESP_OK;
}

/**
 * Upload a file to WiGLE using JIT-mount streaming.
 *
 * Key property: DNS/TCP/TLS setup happens with SD unmounted.  SD is mounted
 * only for the actual file-streaming window (fast disk I/O, typically < 1s).
 * No heap allocation for file content — uses a small on-stack chunk buffer.
 *
 * fsize and basename must be pre-validated by the caller.
 */
static esp_err_t wigle_upload_file_jit(const char *filepath, long fsize,
                                        const char *basename, const char *api_key) {
    const char *donate_val = G_Settings.wigle_donate ? "true" : "false";

    size_t part1_len = strlen("--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\"\r\n"
        "Content-Type: text/csv\r\n\r\n") + strlen(basename);
    size_t part2_len = strlen("\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n") +
        strlen(donate_val) + strlen("\r\n--" WIGLE_BOUNDARY "--\r\n");
    size_t body_len = part1_len + (size_t)fsize + part2_len;

    /* Build Authorization: Basic base64(APIName:APIToken)
     * If api_key has no colon, it's already EncodedForUseToken (pre-encoded) */
    char auth_b64[AUTH_BUF_SIZE];
    if (strchr(api_key, ':') == NULL) {
        // Already encoded token - use directly
        strncpy(auth_b64, api_key, AUTH_BUF_SIZE - 1);
        auth_b64[AUTH_BUF_SIZE - 1] = '\0';
    } else {
        // Need to encode APIName:APIToken
        size_t enc_len = AUTH_BUF_SIZE - 1;
        if (mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                  (const unsigned char *)api_key, strlen(api_key)) != 0) {
            glog("Wigle JIT: base64 encode failed\n");
            return ESP_FAIL;
        }
        auth_b64[enc_len] = '\0';
    }

    char auth_val[6 + AUTH_BUF_SIZE + 1];
    snprintf(auth_val, sizeof(auth_val), "Basic %s", auth_b64);

    char content_type_hdr[128];
    snprintf(content_type_hdr, sizeof(content_type_hdr),
             "multipart/form-data; boundary=%s", WIGLE_BOUNDARY);

    wigle_resp_t resp = {0};
    esp_http_client_config_t cfg = {
        .url               = WIGLE_UPLOAD_URL,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = 90000,
        .event_handler     = wigle_http_event,
        .user_data         = &resp,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&cfg, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) return proxy_err;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept",        "application/json");
    esp_http_client_set_header(client, "User-Agent",    "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);
    esp_http_client_set_header(client, "Content-Type",  content_type_hdr);

    /* Open connection — DNS/TCP/TLS happen here, SD is NOT mounted */
    esp_err_t err = esp_http_client_open(client, (int)body_len);
    if (err != ESP_OK) { esp_http_client_cleanup(client); return err; }

    /* Write part1 — no SD needed */
    char part1[256];
    int p1_len = snprintf(part1, sizeof(part1),
        "--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", basename);
    if (esp_http_client_write(client, part1, p1_len) != p1_len) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Brief JIT mount: open file, stream to HTTP in small chunks, close, unmount.
     * Display SPI is suspended only for this fast disk-I/O window. */
    bool dws = false;
    if (sd_card_mount_for_flush(&dws) != ESP_OK) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        sd_card_unmount_after_flush(dws);
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    esp_err_t stream_err = ESP_OK;
    char chunk[512];   /* on-stack; no heap allocation for file content */
    size_t remaining = (size_t)fsize;
    while (remaining > 0) {
        size_t to_read = (remaining > sizeof(chunk)) ? sizeof(chunk) : remaining;
        size_t got = fread(chunk, 1, to_read, f);
        if (got == 0) { stream_err = ESP_FAIL; break; }
        if (esp_http_client_write(client, chunk, (int)got) != (int)got) {
            stream_err = ESP_FAIL; break;
        }
        remaining -= got;
    }
    fclose(f);
    sd_card_unmount_after_flush(dws);   /* SD unmounted right after file read */

    if (stream_err != ESP_OK) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    /* Write part2 — no SD needed */
    char part2[160];
    int p2_len = snprintf(part2, sizeof(part2),
        "\r\n--" WIGLE_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"donate\"\r\n\r\n"
        "%s\r\n--" WIGLE_BOUNDARY "--\r\n", donate_val);
    if (esp_http_client_write(client, part2, p2_len) != p2_len) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        esp_http_client_close(client); esp_http_client_cleanup(client);
        return (esp_err_t)content_len;
    }
    char drain[128];
    while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}

    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        glog("Wigle JIT: upload %s failed HTTP %d\n", basename, status);
        return ESP_FAIL;
    }
    glog("Wigle JIT: uploaded %s\n", basename);
    return ESP_OK;
}

/**
 * JIT-mount variant of wigle_process_queue for builds where SD and display
 * share an SPI bus ("somethingsomething").  SD is held mounted only during
 * brief file-I/O windows; HTTP is done with SD unmounted so the display SPI
 * is never suspended for more than a few milliseconds at a time.
 *
 * Zero heap allocations.  The queue file on SD is the backing store;
 * we seek to line i on each iteration rather than loading all paths to RAM.
 * Failures are tracked with a uint32_t bitmask (1 bit per entry, 4 bytes).
 *
 * Phase 1 – one mount: ensure queue file exists (build from GPS dir if needed)
 * Phase 2 – per file: mount → read path i + validate → unmount
 *            → HTTP (DNS/TLS with SD unmounted; file streamed in brief window)
 *            → mount → record success → unmount
 * Phase 3 – one mount: rewrite queue keeping only failed entries
 *
 * FD budget per window: never exceeds 2 simultaneous descriptors,
 * well within the JIT max_files=3 limit.
 */
static esp_err_t wigle_process_queue_jit(const char *api_key) {
    char path[320];          /* stack only — no heap for file paths */
    uint32_t fail_mask = 0;  /* bit i set = entry i failed upload */

    /* ── Phase 1: brief mount – ensure queue file exists ───────── */
    {
        bool dws = false;
        if (sd_card_mount_for_flush(&dws) != ESP_OK) {
            glog("Wigle JIT: SD mount failed (phase 1)\n");
            return ESP_FAIL;
        }

        FILE *q = fopen(WIGLE_QUEUE_FILE, "r");  /* FD1 */
        if (q) {
            fclose(q);
        } else {
            /* Build queue from GPS directory */
            DIR *d = opendir("/mnt/ghostesp/gps");  /* FD1 */
            if (d) {
                FILE *qw = fopen(WIGLE_QUEUE_FILE, "w");  /* FD2 */
                struct dirent *e;
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    size_t len = strlen(e->d_name);
                    if (len <= 4 || strcasecmp(e->d_name + len - 4, ".csv") != 0) continue;
                    if (qw) fprintf(qw, "/mnt/ghostesp/gps/%s\n", e->d_name);
                }
                if (qw) fclose(qw);
                closedir(d);
            }
        }

        sd_card_unmount_after_flush(dws);
    }

    /* ── Phase 2: process entries one at a time ─────────────────── */
    int uploaded = 0, failed = 0, skipped = 0;

    for (int i = 0; i < WIGLE_JIT_MAX_ENTRIES; i++) {
        /* Brief mount: read line i from queue, then validate the file. */
        bool dws2 = false;
        if (sd_card_mount_for_flush(&dws2) != ESP_OK) {
            fail_mask |= (1u << i); failed++;
            continue;
        }

        /* Seek to line i in the queue file */
        path[0] = '\0';
        FILE *q = fopen(WIGLE_QUEUE_FILE, "r");  /* FD1 */
        if (q) {
            for (int j = 0; j <= i; j++) {
                if (!fgets(path, sizeof(path), q)) { path[0] = '\0'; break; }
                path[strcspn(path, "\r\n")] = '\0';
            }
            fclose(q);
        }

        if (!path[0]) {
            sd_card_unmount_after_flush(dws2);
            break;  /* queue exhausted */
        }

        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;

        /* stat + uploaded check + format validation — max 1 FD at any time */
        long fsize = 0;
        bool should_upload = false;
        struct stat st;
        if (stat(path, &st) == 0 && st.st_size > 0) {
            fsize = (long)st.st_size;
            if (!wigle_uploaded_check(basename, fsize)) {   /* FD1, brief */
                FILE *f = fopen(path, "rb");                /* FD1 */
                if (f) {
                    bool valid = wigle_file_is_valid_format(f);
                    if (valid) { fseek(f, 0, SEEK_SET); valid = wigle_file_has_data_rows(f); }
                    fclose(f);
                    should_upload = valid;
                }
            }
        }
        sd_card_unmount_after_flush(dws2);

        if (!should_upload) { skipped++; continue; }

        /* HTTP upload: DNS/TLS outside mount; file streamed in brief window */
        esp_err_t ret = wigle_upload_file_jit(path, fsize, basename, api_key);

        if (ret == ESP_OK) {
            uploaded++;
            bool dws3 = false;
            if (sd_card_mount_for_flush(&dws3) == ESP_OK) {
                wigle_uploaded_add(basename, fsize);   /* FD1 – brief */
                sd_card_unmount_after_flush(dws3);
            }
        } else {
            fail_mask |= (1u << i); failed++;
        }
    }

    /* ── Phase 3: rewrite queue keeping only failed entries ─────── */
    {
        bool dws4 = false;
        if (sd_card_mount_for_flush(&dws4) == ESP_OK) {
            if (failed > 0) {
                FILE *qin  = fopen(WIGLE_QUEUE_FILE, "r");         /* FD1 */
                FILE *qtmp = fopen(WIGLE_QUEUE_FILE ".tmp", "w");  /* FD2 */
                if (qin && qtmp) {
                    int idx = 0;
                    /* reuse path[] as line buffer; fgets preserves '\n' */
                    while (fgets(path, sizeof(path), qin) &&
                           idx < WIGLE_JIT_MAX_ENTRIES) {
                        if (fail_mask & (1u << idx))
                            fputs(path, qtmp);
                        idx++;
                    }
                }
                if (qin)  fclose(qin);
                if (qtmp) fclose(qtmp);
                remove(WIGLE_QUEUE_FILE);
                rename(WIGLE_QUEUE_FILE ".tmp", WIGLE_QUEUE_FILE);
            } else {
                remove(WIGLE_QUEUE_FILE);
            }
            sd_card_unmount_after_flush(dws4);
        }
    }

    if (uploaded > 0 && failed == 0) {
        glog("Wigle JIT: all files uploaded (%d)\n", uploaded);
        return ESP_OK;
    } else if (uploaded > 0) {
        glog("Wigle JIT: uploaded=%d failed=%d\n", uploaded, failed);
        return ESP_FAIL;
    } else if (failed > 0) {
        glog("Wigle JIT: upload failed (%d files)\n", failed);
        return ESP_FAIL;
    }
    glog("Wigle JIT: no new files to upload\n");
    return ESP_ERR_NOT_FOUND;
}

/* Add file path to upload queue (only if not already uploaded) */
void wigle_queue_add(const char *filepath) {
    const char *basename = strrchr(filepath, '/');
    if (!basename) basename = filepath;
    else basename++;
    
    struct stat st;
    if (stat(filepath, &st) != 0) return;
    
    /* Skip if already uploaded */
    if (wigle_uploaded_check(basename, st.st_size)) return;
    
    FILE *q = fopen(WIGLE_QUEUE_FILE, "a");
    if (!q) return;
    fprintf(q, "%s\n", filepath);
    fclose(q);
}

int wigle_list_csv_files_paged(int offset, int max_count,
                               char (*out_names)[MAX_PORTAL_NAME],
                               bool *out_has_more) {
    if (!out_names || max_count <= 0 || offset < 0) {
        if (out_has_more) *out_has_more = false;
        return -1;
    }

    bool did_mount = false;
    bool dws = false;
    esp_err_t err = wigle_mount_if_needed(&did_mount, &dws);
    if (err != ESP_OK) {
        if (out_has_more) *out_has_more = false;
        return -1;
    }

    int count = sd_card_list_dir_paged("/mnt/ghostesp/gps", ".csv",
                                       offset, max_count,
                                       out_names, out_has_more);

    wigle_unmount_if_needed(did_mount, dws);
    return count;
}

esp_err_t wigle_get_csv_info(const char *filename, int *out_wifi_rows, int *out_total_rows) {
    if (!wigle_is_safe_csv_name(filename)) {
        return ESP_ERR_INVALID_ARG;
    }

    bool did_mount = false;
    bool dws = false;
    esp_err_t err = wigle_mount_if_needed(&did_mount, &dws);
    if (err != ESP_OK) {
        return err;
    }

    char path[320];
    snprintf(path, sizeof(path), "/mnt/ghostesp/gps/%s", filename);

    FILE *f = fopen(path, "rb");
    if (!f) {
        wigle_unmount_if_needed(did_mount, dws);
        return ESP_ERR_NOT_FOUND;
    }

    if (!wigle_file_is_valid_format(f)) {
        fclose(f);
        wigle_unmount_if_needed(did_mount, dws);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int wifi_rows = 0;
    int total_rows = 0;
    char line[512];
    int line_num = 0;
    fseek(f, 0, SEEK_SET);
    while (fgets(line, sizeof(line), f)) {
        line_num++;
        if (line_num <= 2) continue; /* pre-header + csv header */
        if (line[0] == '\r' || line[0] == '\n' || line[0] == '\0') continue;
        total_rows++;
        if (strstr(line, ",WIFI") != NULL) {
            wifi_rows++;
        }
    }

    fclose(f);
    wigle_unmount_if_needed(did_mount, dws);

    if (out_wifi_rows) *out_wifi_rows = wifi_rows;
    if (out_total_rows) *out_total_rows = total_rows;
    return ESP_OK;
}

/* Process queue file: upload queued files, remove successful ones from queue.
 * Reads queue line-by-line to avoid large buffers. Only files that fail to
 * upload are written back to the queue for retry. */
static esp_err_t wigle_process_queue(const char *api_key) {
    FILE *q = fopen(WIGLE_QUEUE_FILE, "r");
    if (!q) {
        /* No queue yet: scan GPS directory and enqueue valid Wigle CSVs. */
        char *scan_path = malloc(320);
        if (!scan_path) {
            return ESP_ERR_NO_MEM;
        }
        DIR *d = opendir("/mnt/ghostesp/gps");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                size_t len = strlen(e->d_name);
                if (len <= 4 || strcasecmp(e->d_name + len - 4, ".csv") != 0) continue;
                (void)snprintf(scan_path, 320, "/mnt/ghostesp/gps/%s", e->d_name);
                FILE *f = fopen(scan_path, "rb");
                if (f) {
                    if (wigle_file_is_valid_format(f)) {
                        wigle_queue_add(scan_path);
                    }
                    fclose(f);
                }
            }
            closedir(d);
        }
        free(scan_path);
        q = fopen(WIGLE_QUEUE_FILE, "r");
        if (!q) {
            glog("Wigle: files already uploaded, no new files to upload\n");
            return ESP_ERR_NOT_FOUND;
        }
    }

    FILE *q_out = fopen(WIGLE_QUEUE_FILE ".tmp", "w");
    if (!q_out) {
        fclose(q);
        return ESP_FAIL;
    }

    char *line = malloc(320);
    if (!line) {
        fclose(q);
        fclose(q_out);
        return ESP_ERR_NO_MEM;
    }

    int uploaded = 0, failed = 0, skipped = 0;

    while (fgets(line, 320, q)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        const char *path = line;
        const char *basename = strrchr(path, '/');
        if (!basename) basename = path;
        else basename++;

        struct stat st;
        if (stat(path, &st) != 0) {
            /* File doesn't exist, drop from queue */
            continue;
        }

        /* Skip if already uploaded (remove from queue) */
        if (wigle_uploaded_check(basename, st.st_size)) {
            skipped++;
            continue;
        }

        esp_err_t ret = wigle_upload_file(path, api_key);
        if (ret == ESP_OK) {
            uploaded++;
            /* Successfully uploaded, don't add back to queue */
        } else if (ret == ESP_ERR_NOT_SUPPORTED) {
            /* Invalid/empty Wigle CSV, drop from queue */
            continue;
        } else {
            failed++;
            /* Failed upload, keep in queue for retry */
            fprintf(q_out, "%s\n", path);
        }
    }

    free(line);
    fclose(q);
    fclose(q_out);

    if (failed > 0) {
        /* Keep only failures in the queue. */
        remove(WIGLE_QUEUE_FILE);
        if (rename(WIGLE_QUEUE_FILE ".tmp", WIGLE_QUEUE_FILE) != 0) {
            /* If rename fails, leave tmp as-is for inspection. */
        }
    } else {
        /* No failures: no queue needed. */
        remove(WIGLE_QUEUE_FILE);
        remove(WIGLE_QUEUE_FILE ".tmp");
    }

    if (uploaded > 0 && failed == 0) {
        glog("Wigle: all files uploaded successfully (%d)\n", uploaded);
        return ESP_OK;
    } else if (uploaded > 0 && failed > 0) {
        glog("Wigle: uploaded=%d failed=%d\n", uploaded, failed);
        return ESP_FAIL;
    } else if (failed > 0) {
        glog("Wigle: upload failed (%d files)\n", failed);
        return ESP_FAIL;
    } else {
        glog("Wigle: files already uploaded, no new files to upload\n");
        return ESP_ERR_NOT_FOUND;
    }
}

esp_err_t wigle_upload_all(void) {
    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        glog("Wigle: no API key set. Use 'wigle API <encoded>' or 'wigle API <name>:<token>'\n");
        return ESP_ERR_INVALID_STATE;
    }

    // Accept both formats: APIName:APIToken or EncodedForUseToken (no colon)
    // EncodedForUseToken is already base64 encoded by Wigle

    if (!wigle_sta_has_ip()) {
        glog("Wigle: STA not connected - connect to WiFi first (Menu > WiFi > Connect)\n");
        return ESP_ERR_INVALID_STATE;
    }

    bool require_jit = sd_card_needs_jit_mount();

    if (require_jit) {
        /* JIT path: mount only in brief file-I/O windows, never during HTTP. */
        return wigle_process_queue_jit(api_key);
    }

    /* Non-JIT path: SD does not share the display SPI bus. */
    bool display_was_suspended = false;
    bool did_mount = false;
    if (!sd_card_manager.is_initialized) {
        if (sd_card_mount_for_flush(&display_was_suspended) != ESP_OK) {
            glog("Wigle: SD mount failed, cannot read queue\n");
            return ESP_FAIL;
        }
        did_mount = true;
    }

    esp_err_t ret = wigle_process_queue(api_key);

    if (did_mount) {
        sd_card_unmount_after_flush(display_was_suspended);
    }
    return ret;
}

void wigle_uploaded_list(void) {
    FILE *f = fopen(WIGLE_UPLOADED_FILE, "r");
    if (!f) {
        glog("Wigle: no upload history (%s missing)\n", WIGLE_UPLOADED_FILE);
        return;
    }
    glog("Wigle: uploaded CSV memory (%s):\n", WIGLE_UPLOADED_FILE);
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0]) {
            glog("  %d: %s\n", ++n, line);
        }
    }
    fclose(f);
    if (n == 0) glog("  (empty)\n");
}

static void wigle_upload_all_task(void *arg) {
    (void)arg;
    /* Give the DNS resolver time to stabilize after DHCP.
     * Without this, getaddrinfo() returns EAI_AGAIN (~500ms after IP event). */
    vTaskDelay(pdMS_TO_TICKS(2000));
    (void)wigle_upload_all();
    wigle_upload_in_progress = false;
    vTaskDeleteWithCaps(NULL);
}

void wigle_upload_all_async(void) {
    if (wigle_upload_in_progress) {
        glog("Wigle: upload already in progress\n");
        return;
    }
    const char *key = wigle_get_api_key();
    if (!key || key[0] == '\0') return;
    wigle_upload_in_progress = true;
    if (xTaskCreateWithCaps(wigle_upload_all_task, "wigle_up", WIGLE_TASK_STACK, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        glog("Wigle: auto-upload started\n");
    } else {
        glog("Wigle: failed to start upload task\n");
        wigle_upload_in_progress = false;
    }
}

#define WIGLE_PROFILE_URL "https://api.wigle.net/api/v2/profile/user"
#define WIGLE_STATS_URL   "https://api.wigle.net/api/v2/stats/user"

typedef struct {
    char filename[MAX_PORTAL_NAME];
} wigle_single_upload_arg_t;

typedef struct {
    char message[256];
    bool success;
} wigle_result_t;

static esp_err_t wigle_build_auth_header(const char *api_key, char *auth_val, size_t auth_val_len) {
    if (!api_key || !auth_val || auth_val_len == 0) return ESP_ERR_INVALID_ARG;

    char auth_b64[AUTH_BUF_SIZE];
    
    /* If api_key has no colon, it's already EncodedForUseToken (pre-encoded) */
    if (strchr(api_key, ':') == NULL) {
        // Already encoded token - use directly
        strncpy(auth_b64, api_key, AUTH_BUF_SIZE - 1);
        auth_b64[AUTH_BUF_SIZE - 1] = '\0';
    } else {
        // Need to encode APIName:APIToken
        size_t enc_len = AUTH_BUF_SIZE - 1;
        int r = mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                      (const unsigned char *)api_key, strlen(api_key));
        if (r != 0) {
            return ESP_FAIL;
        }
        auth_b64[enc_len] = '\0';
    }
    
    snprintf(auth_val, auth_val_len, "Basic %s", auth_b64);
    return ESP_OK;
}

static esp_err_t wigle_validate_single_csv_mounted(const char *filename,
                                                   char *path_out, size_t path_out_len,
                                                   long *fsize_out,
                                                   char *reason, size_t reason_len) {
    if (reason && reason_len > 0) reason[0] = '\0';

    if (!wigle_is_safe_csv_name(filename)) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Invalid CSV filename");
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(path_out, path_out_len, "/mnt/ghostesp/gps/%s", filename);

    struct stat st;
    if (stat(path_out, &st) != 0) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "CSV not found: %s", filename);
        return ESP_ERR_NOT_FOUND;
    }

    long fsize = (long)st.st_size;
    if (fsize <= 0) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "CSV is empty: %s", filename);
        return ESP_ERR_INVALID_SIZE;
    }

    if (wigle_uploaded_check(filename, fsize)) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Already uploaded: %s", filename);
        return ESP_ERR_NOT_SUPPORTED;
    }

    FILE *f = fopen(path_out, "rb");
    if (!f) {
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Cannot open CSV: %s", filename);
        return ESP_FAIL;
    }

    if (!wigle_file_is_valid_format(f)) {
        fclose(f);
        if (reason && reason_len > 0) snprintf(reason, reason_len, "Not a WiGLE CSV: %s", filename);
        return ESP_ERR_INVALID_RESPONSE;
    }

    fseek(f, 0, SEEK_SET);
    if (!wigle_file_has_data_rows(f)) {
        fclose(f);
        if (reason && reason_len > 0) snprintf(reason, reason_len, "CSV has no data rows");
        return ESP_ERR_NOT_SUPPORTED;
    }

    fclose(f);
    if (fsize_out) *fsize_out = fsize;
    return ESP_OK;
}

esp_err_t wigle_upload_single_csv(const char *filename, char *message, size_t message_len) {
    if (!message || message_len == 0) return ESP_ERR_INVALID_ARG;
    message[0] = '\0';

    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        snprintf(message, message_len, "No API key set");
        return ESP_ERR_INVALID_STATE;
    }
    // Accept both APIName:APIToken or EncodedForUseToken
    if (!wigle_sta_has_ip()) {
        snprintf(message, message_len, "Connect to WiFi first");
        return ESP_ERR_INVALID_STATE;
    }

    char path[320];
    char reason[128];
    long fsize = 0;
    esp_err_t ret;

    if (wigle_require_jit_mount()) {
        bool dws = false;
        if (sd_card_mount_for_flush(&dws) != ESP_OK) {
            snprintf(message, message_len, "SD mount failed");
            return ESP_FAIL;
        }

        ret = wigle_validate_single_csv_mounted(filename, path, sizeof(path), &fsize, reason, sizeof(reason));
        sd_card_unmount_after_flush(dws);
        if (ret != ESP_OK) {
            snprintf(message, message_len, "%s", reason[0] ? reason : "CSV validation failed");
            return ret;
        }

        ret = wigle_upload_file_jit(path, fsize, filename, api_key);
        if (ret == ESP_OK) {
            bool dws2 = false;
            if (sd_card_mount_for_flush(&dws2) == ESP_OK) {
                wigle_uploaded_add(filename, fsize);
                sd_card_unmount_after_flush(dws2);
            }
            snprintf(message, message_len, "Uploaded %s", filename);
            return ESP_OK;
        }

        snprintf(message, message_len, "Upload failed: %s", filename);
        return ret;
    }

    bool did_mount = false;
    bool dws = false;
    ret = wigle_mount_if_needed(&did_mount, &dws);
    if (ret != ESP_OK) {
        snprintf(message, message_len, "SD mount failed");
        return ret;
    }

    ret = wigle_validate_single_csv_mounted(filename, path, sizeof(path), &fsize, reason, sizeof(reason));
    if (ret != ESP_OK) {
        wigle_unmount_if_needed(did_mount, dws);
        snprintf(message, message_len, "%s", reason[0] ? reason : "CSV validation failed");
        return ret;
    }

    ret = wigle_upload_file(path, api_key);
    wigle_unmount_if_needed(did_mount, dws);

    if (ret == ESP_OK) {
        snprintf(message, message_len, "Uploaded %s", filename);
    } else {
        snprintf(message, message_len, "Upload failed: %s", filename);
    }
    return ret;
}

static void wigle_single_upload_task(void *arg) {
    wigle_single_upload_arg_t *task = (wigle_single_upload_arg_t *)arg;
    wigle_result_t result = {0};

    esp_err_t ret = wigle_upload_single_csv(task->filename, result.message, sizeof(result.message));
    result.success = (ret == ESP_OK);

    if (wigle_manual_upload_cb) {
        wigle_manual_upload_cb(result.success, result.message[0] ? result.message :
            (result.success ? "Upload completed" : "Upload failed"));
    }

    wigle_manual_upload_in_progress = false;
    free(task);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t wigle_upload_single_csv_async(const char *filename) {
    if (wigle_manual_upload_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!wigle_is_safe_csv_name(filename)) {
        return ESP_ERR_INVALID_ARG;
    }

    wigle_single_upload_arg_t *task = calloc(1, sizeof(*task));
    if (!task) return ESP_ERR_NO_MEM;
    strncpy(task->filename, filename, sizeof(task->filename) - 1);
    task->filename[sizeof(task->filename) - 1] = '\0';

    wigle_manual_upload_in_progress = true;
    if (xTaskCreateWithCaps(wigle_single_upload_task, "wigle_up1", WIGLE_TASK_STACK, task, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        wigle_manual_upload_in_progress = false;
        free(task);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wigle_get_stats(char *message, size_t message_len) {
    if (!message || message_len == 0) return ESP_ERR_INVALID_ARG;
    message[0] = '\0';

    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        snprintf(message, message_len, "No API key set");
        return ESP_ERR_INVALID_STATE;
    }
    // Accept both APIName:APIToken or EncodedForUseToken
    if (!wigle_sta_has_ip()) {
        snprintf(message, message_len, "Connect to WiFi first");
        return ESP_ERR_INVALID_STATE;
    }

    char auth_val[6 + AUTH_BUF_SIZE + 1];
    if (wigle_build_auth_header(api_key, auth_val, sizeof(auth_val)) != ESP_OK) {
        snprintf(message, message_len, "API key encoding failed");
        return ESP_FAIL;
    }

    wigle_resp_t resp = {0};
    esp_http_client_config_t config = {
        .url = WIGLE_STATS_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = wigle_http_event,
        .user_data = &resp,
        .buffer_size = 1024,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        snprintf(message, message_len, "Proxy URL failed: %s", esp_err_to_name(proxy_err));
        return proxy_err;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        snprintf(message, message_len, "HTTP client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        snprintf(message, message_len, "Network error: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        snprintf(message, message_len, "HTTP error: %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp.buf);
    if (!root) {
        snprintf(message, message_len, "Failed to parse stats response");
        return ESP_FAIL;
    }

    const cJSON *success_json = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (cJSON_IsBool(success_json) && !cJSON_IsTrue(success_json)) {
        const cJSON *msg_json = cJSON_GetObjectItemCaseSensitive(root, "message");
        snprintf(message, message_len, "%s",
                 cJSON_IsString(msg_json) ? msg_json->valuestring : "WiGLE stats request failed");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *stats = cJSON_GetObjectItemCaseSensitive(root, "statistics");
    if (!cJSON_IsObject(stats)) stats = root;

    const cJSON *user_json = cJSON_GetObjectItemCaseSensitive(root, "user");
    const cJSON *user_name_json = cJSON_GetObjectItemCaseSensitive(stats, "userName");
    const char *user = cJSON_IsString(user_json) ? user_json->valuestring :
                      (cJSON_IsString(user_name_json) ? user_name_json->valuestring : "(unknown)");

    const cJSON *rank_json = cJSON_GetObjectItemCaseSensitive(root, "rank");
    const cJSON *month_rank_json = cJSON_GetObjectItemCaseSensitive(root, "monthRank");
    long long rank = cJSON_IsNumber(rank_json) ? (long long)rank_json->valuedouble :
                     (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(stats, "rank")) ?
                      (long long)cJSON_GetObjectItemCaseSensitive(stats, "rank")->valuedouble : 0);
    long long month_rank = cJSON_IsNumber(month_rank_json) ? (long long)month_rank_json->valuedouble :
                           (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(stats, "monthRank")) ?
                            (long long)cJSON_GetObjectItemCaseSensitive(stats, "monthRank")->valuedouble : 0);

    const cJSON *wifi_json = cJSON_GetObjectItemCaseSensitive(stats, "discoveredWiFi");
    const cJSON *wifi_gps_json = cJSON_GetObjectItemCaseSensitive(stats, "discoveredWiFiGPS");
    const cJSON *cell_json = cJSON_GetObjectItemCaseSensitive(stats, "discoveredCell");
    const cJSON *bt_json = cJSON_GetObjectItemCaseSensitive(stats, "discoveredBt");
    const cJSON *total_loc_json = cJSON_GetObjectItemCaseSensitive(stats, "totalWiFiLocations");

    long long wifi = cJSON_IsNumber(wifi_json) ? (long long)wifi_json->valuedouble : 0;
    long long wifi_gps = cJSON_IsNumber(wifi_gps_json) ? (long long)wifi_gps_json->valuedouble : 0;
    long long cell = cJSON_IsNumber(cell_json) ? (long long)cell_json->valuedouble : 0;
    long long bt = cJSON_IsNumber(bt_json) ? (long long)bt_json->valuedouble : 0;
    long long total_locs = cJSON_IsNumber(total_loc_json) ? (long long)total_loc_json->valuedouble : 0;

    snprintf(message, message_len,
             "User: %s\n"
             "Global Rank: %lld\n"
             "Monthly Rank: %lld\n"
             "\n"
             "Discoveries\n"
             "WiFi Networks: %lld\n"
             "WiFi with GPS: %lld\n"
             "Cell Towers: %lld\n"
             "Bluetooth: %lld\n"
             "Total WiFi Locations: %lld",
             user, rank, month_rank, wifi, wifi_gps, cell, bt, total_locs);

    cJSON_Delete(root);
    return ESP_OK;
}

static void wigle_stats_task(void *arg) {
    (void)arg;
    wigle_result_t result = {0};
    esp_err_t ret = wigle_get_stats(result.message, sizeof(result.message));
    result.success = (ret == ESP_OK);

    if (wigle_stats_cb) {
        wigle_stats_cb(result.success, result.message[0] ? result.message :
            (result.success ? "Stats loaded" : "Failed to load stats"));
    }

    wigle_stats_in_progress = false;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t wigle_get_stats_async(void) {
    if (wigle_stats_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    wigle_stats_in_progress = true;
    if (xTaskCreateWithCaps(wigle_stats_task, "wigle_stats", WIGLE_TASK_STACK, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        wigle_stats_in_progress = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

typedef struct {
    char message[128];
    bool success;
} wigle_test_result_t;

static void wigle_test_api_task(void *arg);

esp_err_t wigle_test_api_key(void) {
    if (wigle_test_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    
    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    // Accept both formats: APIName:APIToken or EncodedForUseToken
    
    wigle_test_result_t *result = calloc(1, sizeof(wigle_test_result_t));
    if (!result) {
        return ESP_ERR_NO_MEM;
    }
    
    wigle_test_in_progress = true;
    
    if (xTaskCreateWithCaps(wigle_test_api_task, "wigle_test", WIGLE_TASK_STACK, result, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(result);
        wigle_test_in_progress = false;
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

static void wigle_test_api_task(void *arg) {
    wigle_test_result_t *result = (wigle_test_result_t *)arg;
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    if (!wigle_sta_has_ip()) {
        strncpy(result->message, "Not connected to WiFi", sizeof(result->message) - 1);
        result->success = false;
        goto done;
    }
    
    const char *api_key = wigle_get_api_key();
    if (!api_key || api_key[0] == '\0') {
        strncpy(result->message, "No API key set", sizeof(result->message) - 1);
        result->success = false;
        goto done;
    }
    
    /* Build Authorization: Basic base64(APIName:APIToken)
     * If api_key has no colon, it's already EncodedForUseToken (pre-encoded) */
    char auth_b64[AUTH_BUF_SIZE];
    if (strchr(api_key, ':') == NULL) {
        // Already encoded token - use directly
        strncpy(auth_b64, api_key, AUTH_BUF_SIZE - 1);
        auth_b64[AUTH_BUF_SIZE - 1] = '\0';
    } else {
        // Need to encode APIName:APIToken
        size_t enc_len = AUTH_BUF_SIZE - 1;
        int r = mbedtls_base64_encode((unsigned char *)auth_b64, AUTH_BUF_SIZE, &enc_len,
                                      (const unsigned char *)api_key, strlen(api_key));
        if (r != 0) {
            strncpy(result->message, "API key encoding failed", sizeof(result->message) - 1);
            result->success = false;
            goto done;
        }
        auth_b64[enc_len] = '\0';
    }
    
    char auth_val[6 + AUTH_BUF_SIZE + 1];
    snprintf(auth_val, sizeof(auth_val), "Basic %s", auth_b64);
    
    wigle_resp_t resp = {0};
    
    esp_http_client_config_t config = {
        .url = WIGLE_PROFILE_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .event_handler = wigle_http_event,
        .user_data = &resp,
        .buffer_size = 1024,
    };
    char proxy_url_buf[HTTP_PROXY_URL_MAX];
    esp_err_t proxy_err = proxy_apply(&config, proxy_url_buf, sizeof(proxy_url_buf));
    if (proxy_err != ESP_OK) {
        strncpy(result->message, "Proxy URL failed", sizeof(result->message) - 1);
        result->success = false;
        goto done;
    }
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        strncpy(result->message, "HTTP client init failed", sizeof(result->message) - 1);
        result->success = false;
        goto done;
    }
    
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "GhostESP/1.0");
    esp_http_client_set_header(client, "Authorization", auth_val);
    
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    
    if (err != ESP_OK) {
        snprintf(result->message, sizeof(result->message), "Network error: %s", esp_err_to_name(err));
        result->success = false;
    } else if (status == 200) {
        strncpy(result->message, "API key is valid!", sizeof(result->message) - 1);
        result->success = true;
    } else if (status == 401) {
        strncpy(result->message, "Invalid API key", sizeof(result->message) - 1);
        result->success = false;
    } else {
        snprintf(result->message, sizeof(result->message), "HTTP error: %d", status);
        result->success = false;
    }
    
done:
    glog("WiGLE: %s\n", result->message);
    
    if (wigle_test_cb) {
        wigle_test_cb(result->success, result->message);
    }
    
    wigle_test_in_progress = false;
    free(result);
    vTaskDeleteWithCaps(NULL);
}

