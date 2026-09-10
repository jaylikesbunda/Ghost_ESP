#include "managers/ap_manager.h"
#include "managers/ghost_esp_site_gz.h"
#define GHOST_SITE_PAYLOAD ghost_site_html_gz
#define GHOST_SITE_PAYLOAD_SIZE ghost_site_html_gz_size
#define GHOST_SITE_IS_GZ 1
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include "core/ouis.h"
#include "core/utils.h"
#include "sdkconfig.h"
#include <cJSON.h>
#include <core/serial_manager.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core/glog.h"
#include <math.h>
#include <mdns.h>
#include <nvs_flash.h>
#include <stdio.h>
#include "mbedtls/base64.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static const char *TAG = "ap_manager";
#include <unistd.h>
#include <lwip/sockets.h>
#include <lwip/ip4_addr.h>
#include <lwip/def.h>
#include <lwip/inet.h>
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "managers/status_display_manager.h"
#include "managers/auth_digest.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#ifdef CONFIG_HAS_CAMERA
#include "managers/camera_stream_manager.h"
#endif

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED(a)                                             \
    (((const uint8_t *)(a))[0] == 0 && ((const uint8_t *)(a))[1] == 0 &&    \
     ((const uint8_t *)(a))[2] == 0 && ((const uint8_t *)(a))[3] == 0 &&    \
     ((const uint8_t *)(a))[4] == 0 && ((const uint8_t *)(a))[5] == 0 &&    \
     ((const uint8_t *)(a))[6] == 0 && ((const uint8_t *)(a))[7] == 0 &&    \
     ((const uint8_t *)(a))[8] == 0 && ((const uint8_t *)(a))[9] == 0 &&    \
     ((const uint8_t *)(a))[10] == 0xFF && ((const uint8_t *)(a))[11] == 0xFF)
#endif

static esp_err_t respond_with_site(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
#if GHOST_SITE_IS_GZ
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
#endif
    return httpd_resp_send(req, (const char *)GHOST_SITE_PAYLOAD, GHOST_SITE_PAYLOAD_SIZE);
}

// Forward declarations
static esp_err_t http_get_handler(httpd_req_t *req);
static esp_err_t api_clear_logs_handler(httpd_req_t *req);
static esp_err_t api_settings_handler(httpd_req_t *req);
static esp_err_t api_command_handler(httpd_req_t *req);
static esp_err_t api_settings_get_handler(httpd_req_t *req);
static esp_err_t api_logs_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_status_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_control_handler(httpd_req_t *req);
static esp_err_t api_esp_comm_send_handler(httpd_req_t *req);

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data);

static esp_err_t load_server_config(void);
static esp_err_t start_http_server(void);
static esp_err_t stop_http_server(void);
static void reset_server_config(void);
static bool is_server_running(void);
static bool is_config_loaded(void);
static esp_err_t setup_mdns(void);
static esp_err_t teardown_mdns(void);

#define WEBUI_AP_SUBNET_BASE_ADDR PP_HTONL(LWIP_MAKEU32(192, 168, 4, 0))
#define WEBUI_AP_SUBNET_MASK_ADDR PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0))
#define MAX_FILE_SIZE (5 * 1024 * 1024) // 5 MB
#define AP_MANAGER_BUFFER_SIZE (1024)   // 1 KB buffer size for reading chunks
#define SD_DIRECTORY_PATH_MAX 512
#define SD_DIRECTORY_MAX_ENTRIES 64
#define MIN_(a, b) ((a) < (b) ? (a) : (b))
#define AP_SERIAL_BUFFER_SIZE 528       // Size of serial buffer
#define AUTH_MAX_HDR_LEN 512            // max size for Authorization header (increased for Digest)
#define AUTH_MAX_DECODE_LEN 256         // max decoded credential length

static bool is_safe_mnt_path(const char *path, bool allow_root) {
    if (!path || strncmp(path, "/mnt", 4) != 0) return false;
    if (path[4] != '\0' && path[4] != '/') return false;
    if (!allow_root && path[4] == '\0') return false;
    if (strstr(path, "..") != NULL) return false;
    return true;
}

static bool is_safe_upload_filename(const char *name) {
    if (!name || name[0] == '\0' || strlen(name) >= 128) return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
    return strchr(name, '/') == NULL && strchr(name, '\\') == NULL && strstr(name, "..") == NULL;
}

static bool is_ip_in_ap_subnet(uint32_t addr_net_order) {
    ip4_addr_t addr = { .addr = addr_net_order };
    const ip4_addr_t base = { .addr = WEBUI_AP_SUBNET_BASE_ADDR };
    const ip4_addr_t mask = { .addr = WEBUI_AP_SUBNET_MASK_ADDR };
    return ip4_addr_netcmp(&addr, &base, &mask);
}

static bool webui_origin_allowed(httpd_req_t *req) {
    if (!settings_get_webui_restrict_to_ap(&G_Settings)) {
        return true;
    }

    int sock = httpd_req_to_sockfd(req);
    if (sock < 0) {
        ESP_LOGW(TAG, "Unable to obtain socket descriptor for HTTP request");
        goto deny;
    }

    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    if (getpeername(sock, (struct sockaddr *)&peer_addr, &peer_len) != 0) {
        ESP_LOGW(TAG, "getpeername failed for HTTP request: errno %d", errno);
        goto deny;
    }

    char ip_buf[INET6_ADDRSTRLEN] = "unknown";

    if (peer_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&peer_addr;
        inet_ntop(AF_INET, &addr4->sin_addr, ip_buf, sizeof(ip_buf));
        if (is_ip_in_ap_subnet(addr4->sin_addr.s_addr)) {
            return true;
        }
    }
#if LWIP_IPV6
    else if (peer_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&peer_addr;
        inet_ntop(AF_INET6, &addr6->sin6_addr, ip_buf, sizeof(ip_buf));

        if (IN6_IS_ADDR_V4MAPPED(addr6->sin6_addr.s6_addr)) {
            uint32_t ipv4_addr_net_order = PP_HTONL(LWIP_MAKEU32(addr6->sin6_addr.s6_addr[12],
                addr6->sin6_addr.s6_addr[13],
                addr6->sin6_addr.s6_addr[14],
                addr6->sin6_addr.s6_addr[15]));
            if (is_ip_in_ap_subnet(ipv4_addr_net_order)) {
                return true;
            }
        }
    }
#endif

    ESP_LOGW(TAG, "Blocking WebUI request from %s (AP-only restriction enabled)", ip_buf);

deny:
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Unauthorized");
    return false;
}

#define WEBUI_GUARD_OR_RETURN(req) \
    do {                           \
        if (!ap_manager_webui_request_allowed(req)) return ESP_OK; \
    } while (0)

// simple global backoff state (very small memory footprint)
static uint8_t auth_fail_count = 0;
static uint32_t auth_last_fail_ts = 0; // seconds since epoch of last fail

static int constant_time_eq(const unsigned char *a, const unsigned char *b, size_t len) {
    unsigned char diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

static void str_lower_inplace(char *s) {
    for (; *s; ++s) *s = (char)tolower((unsigned char)*s);
}

// parse key from Digest header: looks for key=\"value\" or key=value, copies into out
static void parse_digest_param(const char *hdr, const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    if (!hdr || !key) return;
    const char *p = hdr;
    size_t keylen = strlen(key);

    while ((p = strstr(p, key)) != NULL) {
        // ensure key is a standalone token (preceded by start/comma/space) and followed by optional spaces then '='
        if (p != hdr) {
            char prev = *(p - 1);
            if (prev != '"' && prev != ',' && !isspace((unsigned char)prev)) {
                p = p + 1; // skip this match (it's inside another token)
                continue;
            }
        }
        const char *q = p + keylen;
        // skip spaces
        while (*q && isspace((unsigned char)*q)) q++;
        if (*q != '=') {
            p = p + 1; // not a key=value occurrence
            continue;
        }
        q++; // skip '='
        while (*q && isspace((unsigned char)*q)) q++;

        // value can be quoted or unquoted
        if (*q == '"') {
            q++;
            size_t i = 0;
            while (*q && *q != '"' && i + 1 < out_len) {
                out[i++] = *q++;
            }
            out[i] = '\0';
            return;
        } else {
            size_t i = 0;
            while (*q && *q != ',' && !isspace((unsigned char)*q) && i + 1 < out_len) {
                out[i++] = *q++;
            }
            out[i] = '\0';
            return;
        }
    }
}


static httpd_handle_t server = NULL;
static esp_netif_t *netif = NULL;
static bool mdns_freed = false;

static httpd_config_t server_config;
static httpd_uri_t uri_handlers[20];
static int handler_count = 0;
static bool config_loaded = false;

// Checks if the AP enabled key exists in NVS. Used to decide whether to apply a default override.
static bool settings_ap_enabled_key_exists(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &h);
    if (err != ESP_OK) return false;
    uint8_t val;
    err = nvs_get_u8(h, "ap_enabled", &val);
    nvs_close(h);
    return (err == ESP_OK);
}

static esp_err_t scan_directory_non_recursive(const char *base_path, cJSON *json_array, bool *truncated) {
    DIR *dir = opendir(base_path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", base_path);
        return ESP_FAIL;
    }

    size_t entry_count = 0;
    if (truncated) *truncated = false;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        if (entry_count >= SD_DIRECTORY_MAX_ENTRIES) {
            if (truncated) *truncated = true;
            break;
        }

        char full_path[SD_DIRECTORY_PATH_MAX];
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", base_path, entry->d_name);
        if (path_len < 0 || path_len >= (int)sizeof(full_path)) {
            ESP_LOGW(TAG, "Skipping overlong SD path: %s/%s", base_path, entry->d_name);
            continue;
        }

        struct stat entry_stat;
        if (stat(full_path, &entry_stat) != 0) {
            ESP_LOGW(TAG, "Failed to stat file: %s", full_path);
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "name", entry->d_name);
        cJSON_AddStringToObject(item, "path", full_path);

        if (S_ISDIR(entry_stat.st_mode)) {
            cJSON_AddStringToObject(item, "type", "folder");
        } else {
            cJSON_AddStringToObject(item, "type", "file");
            cJSON_AddNumberToObject(item, "size", entry_stat.st_size);
        }

        cJSON_AddItemToArray(json_array, item);
        entry_count++;
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t api_sd_card_get_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);

    char query[512] = {0};
    char path_param[SD_DIRECTORY_PATH_MAX] = "/mnt";
    
    esp_err_t query_ret = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (query_ret == ESP_OK && strlen(query) > 0) {
        char encoded_value[512] = {0};
        char decoded_value[512] = {0};
        esp_err_t key_ret = httpd_query_key_value(query, "path", encoded_value, sizeof(encoded_value));
        if (key_ret == ESP_OK && strlen(encoded_value) > 0) {
            url_decode(decoded_value, encoded_value);
            ESP_LOGI(TAG, "SD card query path: %s (decoded from %s)", decoded_value, encoded_value);
            if (is_safe_mnt_path(decoded_value, true)) {
                strncpy(path_param, decoded_value, sizeof(path_param) - 1);
                path_param[sizeof(path_param) - 1] = '\0';
            } else {
                ESP_LOGW(TAG, "Invalid path prefix: %s", decoded_value);
            }
        }
    }

    ESP_LOGI(TAG, "Scanning SD path: %s", path_param);

    struct stat st;
    if (stat(path_param, &st) != 0) {
        ESP_LOGE(TAG, "Path not accessible: %s", path_param);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Path not found.\"}");
        return ESP_FAIL;
    }

    if (!S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "Path is not a directory: %s", path_param);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Path is not a directory.\"}");
        return ESP_FAIL;
    }

    cJSON *response_json = cJSON_CreateObject();
    if (!response_json) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to create JSON object.\"}");
        return ESP_FAIL;
    }

    cJSON_AddStringToObject(response_json, "path", path_param);

    uint64_t total_bytes = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info("/mnt", &total_bytes, &free_bytes);

    if (ret == ESP_OK) {
        cJSON *storage_info = cJSON_CreateObject();
        cJSON_AddNumberToObject(storage_info, "total", total_bytes);
        cJSON_AddNumberToObject(storage_info, "used", total_bytes - free_bytes);
        cJSON_AddNumberToObject(storage_info, "free", free_bytes);
        cJSON_AddItemToObject(response_json, "storage", storage_info);
    } else {
        ESP_LOGW(TAG, "Could not get FATFS info (%s)", esp_err_to_name(ret));
    }

    cJSON *files_array = cJSON_CreateArray();
    bool truncated = false;
    if (!files_array || scan_directory_non_recursive(path_param, files_array, &truncated) != ESP_OK) {
        if (files_array) cJSON_Delete(files_array);
        cJSON_Delete(response_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to scan directory.\"}");
        return ESP_FAIL;
    }
    cJSON_AddItemToObject(response_json, "files", files_array);
    cJSON_AddBoolToObject(response_json, "truncated", truncated);

    char *response_string = cJSON_PrintUnformatted(response_json);
    if (!response_string) {
        ESP_LOGE(TAG, "Failed to serialize JSON.");
        cJSON_Delete(response_json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to serialize SD card data.\"}");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(response_json);
    free(response_string);

    return ESP_OK;
}

static esp_err_t api_sd_card_post_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "Failed to receive request payload.");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload.\"}");
        return ESP_FAIL;
    }

    // Parse JSON payload
    buf[received] = '\0'; // Null-terminate the received string
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGE(TAG, "Failed to parse JSON payload.");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload.\"}");
        return ESP_FAIL;
    }

    cJSON *path_item = cJSON_GetObjectItem(json, "path");
    if (!cJSON_IsString(path_item) || !path_item->valuestring) {
        ESP_LOGE(TAG, "Missing or invalid 'path' in request payload.");
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"'path' is required and must be a string.\"}");
        return ESP_FAIL;
    }

    const char *file_path = path_item->valuestring;

    if (!is_safe_mnt_path(file_path, false)) {
        ESP_LOGE(TAG, "Path traversal rejected: %s", file_path);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\": \"Access denied: path must be under /mnt.\"}");
        return ESP_FAIL;
    }

    FILE *file = fopen(file_path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\": \"File not found.\"}");
        return ESP_FAIL;
    }

    // Set response headers for chunked transfer
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment");

    // Allocate a buffer for sending chunks
    char *chunk_buf = malloc(AP_MANAGER_BUFFER_SIZE);
    if (!chunk_buf) {
        ESP_LOGE(TAG, "Failed to allocate memory for chunk buffer.");
        fclose(file);
        cJSON_Delete(json);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed.\"}");
        return ESP_FAIL;
    }

    size_t bytes_read;
    esp_err_t ret = ESP_OK;
    while ((bytes_read = fread(chunk_buf, 1, AP_MANAGER_BUFFER_SIZE, file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk_buf, bytes_read) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send file chunk.");
            ret = ESP_FAIL;
            break;
        }
    }

    // Send final, zero-length chunk
    if (ret == ESP_OK) {
        if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send final chunk.");
            ret = ESP_FAIL;
        }
    }

    fclose(file);
    free(chunk_buf);
    cJSON_Delete(json);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "File sent successfully: %s", file_path);
    } else {
        ESP_LOGE(TAG, "File download failed for: %s", file_path);
    }

    return ret;
}

#define MAX_PATH_LENGTH 512

esp_err_t get_query_param(httpd_req_t *req, const char *key, char *value, size_t max_len) {
    size_t query_len = httpd_req_get_url_query_len(req) + 1;

    if (query_len > 512) {
        ESP_LOGE(TAG, "Query string too long.");
        return ESP_ERR_INVALID_SIZE;
    }

    if (query_len > 1) { // >1 because query string starts with '?'
        char *query = malloc(query_len);
        if (!query) {
            ESP_LOGE(TAG, "Failed to allocate memory for query string.");
            return ESP_ERR_NO_MEM;
        }

        if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
            char encoded_value[512];
            size_t ev_size = max_len < sizeof(encoded_value) ? max_len : sizeof(encoded_value);
            if (httpd_query_key_value(query, key, encoded_value, ev_size) == ESP_OK) {
                url_decode(value, encoded_value);
                free(query);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Key '%s' not found in query string.", key);
            }
        } else {
            ESP_LOGE(TAG, "Failed to get query string.");
        }

        free(query);
    } else {
        ESP_LOGE(TAG, "No query string found in the URL.");
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t api_sd_card_delete_file_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    char filepath[256 + 1];

    size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len > 1) {
        char query[query_len];
        httpd_req_get_url_query_str(req, query, query_len);

        char path[256];
        if (httpd_query_key_value(query, "path", path, sizeof(path)) == ESP_OK) {
            snprintf(filepath, sizeof(filepath), "%s", path);

            if (!is_safe_mnt_path(filepath, false)) {
                ESP_LOGE(TAG, "Path traversal rejected in delete: %s", filepath);
                httpd_resp_set_status(req, "403 Forbidden");
                httpd_resp_send(req, "Access denied: path must be under /mnt", HTTPD_RESP_USE_STRLEN);
                return ESP_FAIL;
            }

            ESP_LOGI(TAG, "Deleting file: %s", filepath);

            int res = unlink(filepath);
            if (res == 0) {
                ESP_LOGI(TAG, "File deleted successfully");
                httpd_resp_set_status(req, "200 OK");
                httpd_resp_send(req, "File deleted successfully", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            } else {
                ESP_LOGE(TAG, "Failed to delete file: %s, errno: %d", filepath, errno);
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_send(req, "Failed to delete the file", HTTPD_RESP_USE_STRLEN);
                return ESP_FAIL;
            }
        }
    }

    ESP_LOGE(TAG, "Invalid query parameters");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Missing or invalid 'path' parameter", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
}

// Handler for uploading files to SD card
static esp_err_t api_sd_card_upload_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    ESP_LOGI(TAG, "Received file upload request.");

    // Retrieve 'path' query parameter
    char path_param[MAX_PATH_LENGTH] = {0};
    if (get_query_param(req, "path", path_param, sizeof(path_param)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid 'path' query parameter.\"}");
        return ESP_FAIL;
    }
    if (!is_safe_mnt_path(path_param, true)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Access denied: path must be under /mnt.\"}");
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > MAX_FILE_SIZE) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Upload too large or empty.\"}");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Upload path: %s", path_param);

    // Buffer for receiving data
    char *buf = malloc(AP_MANAGER_BUFFER_SIZE + 1);
    if (!buf) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed for buffer.\"}");
        return ESP_FAIL;
    }

    char *file_path = NULL;
    FILE *file = NULL;
    int received;
    int total_received = 0;
    bool headers_parsed = false;
    char *body_start = NULL;

    while ((received = httpd_req_recv(req, buf, AP_MANAGER_BUFFER_SIZE)) > 0) {
        buf[received] = '\0';
        total_received += received;

        if (!headers_parsed) {
            body_start = strstr(buf, "\r\n\r\n");
            if (body_start) {
                headers_parsed = true;
                body_start += 4; // Move past the separator

                // Extract filename from headers
                char *filename_start = strstr(buf, "filename=\"");
                if (filename_start) {
                    filename_start += strlen("filename=\"");
                    char *filename_end = strstr(filename_start, "\"");
                    if (filename_end) {
                        size_t filename_len = filename_end - filename_start;
                        if (filename_len >= 128) filename_len = 127;
                        char original_filename[128] = {0};
                        memcpy(original_filename, filename_start, filename_len);
                        if (!is_safe_upload_filename(original_filename)) {
                            free(buf);
                            httpd_resp_set_status(req, "400 Bad Request");
                            httpd_resp_sendstr(req, "{\"error\": \"Invalid filename.\"}");
                            return ESP_FAIL;
                        }

                        // Allocate memory for the full file path
                        size_t file_path_size = strlen(path_param) + strlen(original_filename) + 2;
                        file_path = malloc(file_path_size);
                        if (!file_path) {
                            free(buf);
                            httpd_resp_set_status(req, "500 Internal Server Error");
                            httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed for file path.\"}");
                            return ESP_FAIL;
                        }
                        snprintf(file_path, file_path_size, "%s/%s", path_param, original_filename);
                        
                        ESP_LOGI(TAG, "Writing to file: %s", file_path);
                        file = fopen(file_path, "wb");
                        if (!file) {
                            free(buf);
                            free(file_path);
                            httpd_resp_set_status(req, "500 Internal Server Error");
                            httpd_resp_sendstr(req, "{\"error\": \"Failed to open file for writing.\"}");
                            return ESP_FAIL;
                        }
                        
                        // Write the first part of the file data
                        size_t data_len = received - (body_start - buf);
                        if (data_len > 0) {
                            fwrite(body_start, 1, data_len, file);
                        }
                    }
                }
            }
        } else if (file) {
            // Write subsequent chunks of file data
            fwrite(buf, 1, received, file);
        }

        if (total_received > MAX_FILE_SIZE) {
            if (file) fclose(file);
            free(file_path);
            free(buf);
            httpd_resp_set_status(req, "413 Payload Too Large");
            httpd_resp_sendstr(req, "{\"error\": \"Upload too large.\"}");
            return ESP_FAIL;
        }
    }
    
    free(buf);
    if (file) {
        fclose(file);
        
        // Post-process the file to remove the boundary
        file = fopen(file_path, "r+b");
        if (file) {
            fseek(file, 0, SEEK_END);
            long file_size = ftell(file);
            fseek(file, 0, SEEK_SET);
            
            long tail_len = file_size < 512 ? file_size : 512;
            char tail[513] = {0};
            if (tail_len > 0 && fseek(file, file_size - tail_len, SEEK_SET) == 0) {
                size_t read_len = fread(tail, 1, tail_len, file);
                tail[read_len] = '\0';
                char *end_boundary = strstr(tail, "\r\n--");
                if (end_boundary) {
                    long new_size = (file_size - tail_len) + (end_boundary - tail);
                    ftruncate(fileno(file), new_size);
                }
            }
            fclose(file);
        }
    }
    free(file_path); // Free the allocated file_path

    if (received < 0) {
        ESP_LOGE(TAG, "Error receiving file data.");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to receive file data.\"}");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "File upload finished, total bytes: %d", total_received);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "File uploaded successfully.");

    return ESP_OK;
}

esp_err_t ap_manager_ensure_wifi_init(void) {
    wifi_mode_t mode;
    esp_err_t err = esp_wifi_get_mode(&mode);
    if (err == ESP_OK) {
        return ESP_OK; // driver already initialized
    }
    if (err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // Runtime radio config must never persist across reboots. Attacks switch
    // mode/protocols for raw TX; with the default WIFI_STORAGE_FLASH a reboot
    // mid-attack would reload that broken state from NVS and leave AP beacons
    // invisible. RAM-only storage keeps NVS out of the picture entirely.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to force RAM wifi storage: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

esp_err_t ap_manager_apply_attack_radio(void) {
    // Attack profile = STA-only radio. Switching out of AP mode drops any
    // WebUI/GhostNet client immediately AND removes the beacon, so nothing can
    // re-associate mid-attack and lock the radio to one channel (#368).
    // Raw injection still works from the STA interface while unassociated.
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        glog("WiFi mode set failed\n");
        return ret;
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    wifi_protocols_t p = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR,
#if defined(CONFIG_IDF_TARGET_ESP32C5) /* C6 is 2.4GHz-only; no 5G protocols */
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
#endif
    };
    ret = esp_wifi_set_protocols(WIFI_IF_STA, &p);
    if (ret != ESP_OK) {
        printf("Warning: Failed to set attack protocols: %s\n", esp_err_to_name(ret));
    }
#else
    (void)esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
#endif

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        glog("WiFi start failed\n");
    } else {
        printf("GhostNet AP off for attack (clients kicked)\n");
    }
    return ret;
}

wifi_interface_t ap_manager_get_tx_iface(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
        return WIFI_IF_STA;
    }
    return WIFI_IF_AP;
}

esp_err_t ap_manager_apply_normal_ap_profile(void) {
    // be conservative for client compatibility (2.4GHz only, HT20)
    (void)esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    // No LR anywhere in the normal profile; a persisted LR config makes AP
    // beacons invisible to regular clients.
    wifi_protocols_t p = {
        .ghz_2g = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N,
#if defined(CONFIG_IDF_TARGET_ESP32C5) /* C6 is 2.4GHz-only; no 5G protocols */
        .ghz_5g = WIFI_PROTOCOL_11A | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AC | WIFI_PROTOCOL_11AX,
#endif
    };
    return esp_wifi_set_protocols(WIFI_IF_AP, &p);
#else
    return esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
#endif
}

bool ap_manager_restore_after_attack(const char *who) {
    esp_err_t err = ap_manager_start_services();
    if (err == ESP_OK) {
        return true;
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        glog("%s: AP services not restored (disabled or power saving on)\n",
             who ? who : "restore");
    } else {
        glog("%s: failed to restore AP services: %s\n", who ? who : "restore",
             esp_err_to_name(err));
    }
    return false;
}

esp_err_t ap_manager_init(void) {
    esp_err_t ret;
    wifi_mode_t mode;

    // Default override: For ESP32-C5 with build template "somethingsomething",
    // default AP to OFF on first boot (when key not present in NVS)
#if defined(CONFIG_IDF_TARGET_ESP32C5) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (!settings_ap_enabled_key_exists()) {
            G_Settings.ap_enabled = false;
        }
    }
#endif

    // Default override: GhostLink P1 peer (ESP32-C3) also defaults AP to OFF
    // on first boot (when key not present in NVS)
#if defined(CONFIG_IDF_TARGET_ESP32C3) && defined(CONFIG_BUILD_CONFIG_TEMPLATE)
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "ghostlink_p1_peer") == 0) {
        if (!settings_ap_enabled_key_exists()) {
            G_Settings.ap_enabled = false;
        }
    }
#endif

    // --- Memory check before AP init ---
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (free_heap < (45 * 1024)) {
        ESP_LOGW(TAG, "WARNING: Less than 45KB of free RAM available (%d bytes). AP may fail to initialize or operate reliably!", (int)free_heap);
        //TERMINAL_VIEW_ADD_TEXT("WARNING: <45KB RAM free (%d bytes). AP may not initialize or operate reliably!\n", (int)free_heap);
    }

    // Check if AP is disabled in settings
    if (!settings_get_ap_enabled(&G_Settings)) {
        glog("Access point: off\n");
        return ESP_OK;
    }

    ret = esp_wifi_get_mode(&mode);
    if (ret == ESP_ERR_WIFI_NOT_INIT) {
        glog("Wi-Fi not initialized, initializing as Access Point...\n");

        ret = ap_manager_ensure_wifi_init();
        if (ret != ESP_OK) {
            glog("esp_wifi_init failed: %s\n", esp_err_to_name(ret));
            return ret;
        }

        netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!netif) {
            netif = esp_netif_create_default_wifi_ap();
            if (netif == NULL) {
                glog("Failed to create default Wi-Fi AP\n");
                return ESP_FAIL;
            }
        }
    } else if (ret == ESP_OK) {
        glog("Wi-Fi already initialized, skipping Wi-Fi init.\n");
        // Ensure our static AP netif handle is set (and exists)
        if (!netif) {
            netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        }
        if (!netif) {
            netif = esp_netif_create_default_wifi_ap();
            if (!netif) {
                glog("Failed to create default Wi-Fi AP when Wi-Fi already initialized\n");
                return ESP_FAIL;
            }
        }
    } else {
        glog("esp_wifi_get_mode failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        glog("esp_wifi_set_mode failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    // Re-assert the normal radio profile every boot. This heals devices that
    // carry an attack-time config (plain AP mode, LR/11AX protocols) persisted
    // by older builds, which made beacons invisible after reboot.
    ret = ap_manager_apply_normal_ap_profile();
    if (ret != ESP_OK) {
        glog("Failed to apply normal AP profile: %s\n", esp_err_to_name(ret));
    }

    const char *ssid = strlen(settings_get_ap_ssid(&G_Settings)) > 0
                           ? settings_get_ap_ssid(&G_Settings)
                           : "GhostNet";

    const char *password = strlen(settings_get_ap_password(&G_Settings)) >= 8
                               ? settings_get_ap_password(&G_Settings)
                               : "GhostNet";

    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = 6,
                .max_connection = 4,
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
                .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
#else
                .authmode = WIFI_AUTH_WPA2_PSK,
#endif
                .beacon_interval = 100,
            },
    };

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid[sizeof(wifi_config.ap.ssid) - 1] = '\0';

    wifi_config.ap.ssid_len = strlen(ssid);

    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.password[sizeof(wifi_config.ap.password) - 1] = '\0';

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        glog("esp_wifi_set_config failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif == NULL) {
        glog("Failed to get the AP network interface\n");
    } else {
        // Stop DHCP server before configuring
        esp_netif_dhcps_stop(ap_netif);

        // Configure IP address
        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);        // IP address (192.168.4.1)
        ip_info.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);        // Gateway (usually same as IP)
        ip_info.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0); // Subnet mask
        esp_netif_set_ip_info(ap_netif, &ip_info);

        esp_netif_dhcps_start(ap_netif);
        glog("DHCP server configured successfully.\n");
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        glog("esp_wifi_start failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    glog("Wi-Fi Access Point started with SSID: %s\n", ssid);

    // Register event handlers for Wi-Fi events if not registered already
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler, NULL));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    // Initialize mDNS
    ret = setup_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup mDNS");
        return ret;
    }

    // Start HTTP server
    ret = load_server_config();
    if (ret != ESP_OK) {
        glog("Error loading server config\n");
        return ret;
    }

    log_heap_status(TAG, "ap_init_pre_httpd");
    ret = start_http_server();
    if (ret != ESP_OK) {
        glog("Error starting HTTP server\n");
        log_heap_status(TAG, "ap_httpd_start_fail");
        return ret;
    }
    log_heap_status(TAG, "ap_init_post_httpd");

    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
        glog("ESP32 AP IP Address: \n" IPSTR, IP2STR(&ip_info.ip));
    } else {
        glog("Failed to get IP address\n");
    }

    return ESP_OK;
}

// Deinitialize and stop the servers
void ap_manager_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing AP Manager");

    stop_http_server();
    reset_server_config();

    // Quiesce mDNS BEFORE the WiFi driver goes down: a pending responder
    // announce/probe fired after esp_wifi_stop() crashes in the WiFi ROM
    // (LoadProhibited in ieee80211_search_node on tcpip_thread).
    teardown_mdns();

    {
        esp_err_t err_reg = esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister WIFI_EVENT handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_AP_STAIPASSIGNED handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_STA_GOT_IP handler: %s", esp_err_to_name(err_reg));
        }
    }
    
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (netif) {
        esp_netif_destroy(netif);
        netif = NULL;
    }

    ESP_LOGI(TAG, "AP Manager deinitialized successfully");
}

void ap_manager_add_log(const char *log_message) {
    terminal_view_add_text(log_message);
}

esp_err_t ap_manager_start_services() {
    esp_err_t ret;

    // if ap is disabled or power saving is on, do not start ap services.
    if (!settings_get_ap_enabled(&G_Settings) || settings_get_power_save_enabled(&G_Settings)) {
        glog("ap services skipped: ap disabled or power saving mode is on\n");
        status_display_show_status("AP Disabled");
        // make sure services are stopped if they somehow started and conditions changed
        ap_manager_stop_services();
        // Report the skip so callers don't mistake it for a successful restore.
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Set Wi-Fi mode to AP
    ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        glog("WiFi mode set failed\n");
        return ret;
    }

    // Undo any attack-time radio config (LR protocols etc.) so beacons are
    // visible to regular clients again, then start Wi-Fi.
    ret = ap_manager_apply_normal_ap_profile();
    if (ret != ESP_OK) {
        glog("Failed to apply normal AP profile: %s\n", esp_err_to_name(ret));
    }

    // Start Wi-Fi
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        glog("WiFi start failed\n");
        return ret;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    if (server != NULL) {
        ESP_LOGI(TAG, "HTTP server already running; skipping restart");
        status_display_show_status("AP Services On");
        return ESP_OK;
    }

    if (mdns_freed) {
        ret = setup_mdns();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // Start HTTPD server
    if (config_loaded) {
        reset_server_config();
    }
    ret = load_server_config();
    if (ret != ESP_OK) {
        glog("Error loading server config\n");
        return ret;
    }

    ret = start_http_server();
    if (ret != ESP_OK) {
        glog("Error starting HTTP server\n");
        status_display_show_status("AP HTTP Fail");
        return ret;
    }

    status_display_show_status("AP Services On");
    return ESP_OK;
}

void ap_manager_stop_services() {
    log_heap_status(TAG, "ap_stop_pre");
    wifi_mode_t wifi_mode;
    esp_err_t err = esp_wifi_get_mode(&wifi_mode);

    ap_manager_stop_services_keep_wifi();

    if (err == ESP_OK) {
        if (wifi_mode == WIFI_MODE_AP || wifi_mode == WIFI_MODE_STA ||
            wifi_mode == WIFI_MODE_APSTA) {
            glog("Stopping Wi-Fi...\n");
            esp_err_t stop_err = esp_wifi_stop();
            if (stop_err != ESP_OK &&
                stop_err != ESP_ERR_WIFI_NOT_INIT &&
                stop_err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGE(TAG, "esp_wifi_stop failed: %s", esp_err_to_name(stop_err));
            }
        }
    } else {
        glog("Failed to get Wi-Fi mode, error: %d\n", err);
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    log_heap_status(TAG, "ap_stop_post");
    status_display_show_status("AP Services Off");
}

void ap_manager_stop_services_keep_wifi(void) {
    log_heap_status(TAG, "ap_stop_pre");

    esp_err_t http_ret = stop_http_server();
    if (http_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(http_ret));
    }
    reset_server_config();

    {
        esp_err_t err_reg = esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister WIFI_EVENT handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_AP_STAIPASSIGNED handler: %s", esp_err_to_name(err_reg));
        }
    }
    {
        esp_err_t err_reg = esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler);
        if (err_reg != ESP_OK && err_reg != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to unregister IP_EVENT_STA_GOT_IP handler: %s", esp_err_to_name(err_reg));
        }
    }

    teardown_mdns();
}

static bool webui_session_cookie_valid(httpd_req_t *req, const char *password) {
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0 || cookie_len >= 512) return false;

    char cookie_buf[512];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_buf, sizeof(cookie_buf)) != ESP_OK) return false;

    char *sess_pos = strstr(cookie_buf, "session=");
    if (!sess_pos) return false;
    sess_pos += strlen("session=");

    char session_token[256] = {0};
    size_t si = 0;
    while (*sess_pos && *sess_pos != ';' && si + 1 < sizeof(session_token)) {
        session_token[si++] = *sess_pos++;
    }

    return si > 0 && validate_stateless_nonce(password, strlen(password), session_token, 300) == 0;
}

bool ap_manager_webui_request_allowed(httpd_req_t *req) {
    if (!webui_origin_allowed(req)) return false;
    if (!settings_get_web_auth_enabled(&G_Settings)) return true;

    const char *password = settings_get_ap_password(&G_Settings);
    if (!password || strlen(password) < 8) password = "GhostNet";
    if (webui_session_cookie_valid(req, password)) return true;

    char nonce[128] = {0};
    if (generate_stateless_nonce(password, strlen(password), nonce, sizeof(nonce)) != 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "Authentication error");
        return false;
    }

    char www[256];
    snprintf(www, sizeof(www), "Digest realm=\"Protected Area\", qop=\"auth\", nonce=\"%s\", algorithm=MD5", nonce);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", www);
    httpd_resp_sendstr(req, "Authentication required");
    return false;
}

// Handler for GET requests (serves the HTML page)
static esp_err_t http_get_handler(httpd_req_t *req) {
    if (!webui_origin_allowed(req)) return ESP_OK;
    printf("Received HTTP GET request: %s\n", req->uri);

    if (!settings_get_web_auth_enabled(&G_Settings)) {
        return respond_with_site(req);
    }

    char auth_buffer[AUTH_MAX_HDR_LEN] = {0};

    // global backoff check
    if (auth_fail_count > 0) {
        uint32_t now = (uint32_t)time(NULL);
        uint32_t elapsed = now - auth_last_fail_ts;
        uint8_t capped = auth_fail_count > 6 ? 6 : auth_fail_count;
        uint32_t wait_s = (1u << capped); // exponential backoff in seconds
        if (elapsed < wait_s) {
            httpd_resp_set_status(req, "429 Too Many Requests");
            httpd_resp_sendstr(req, "Too many authentication attempts, try later");
            return ESP_OK;
        }
    }

    // attempt session cookie first
    const FSettings *settings_local = &G_Settings;
    const char *expected_password_local = settings_get_ap_password(settings_local);
    if (!expected_password_local || strlen(expected_password_local) < 8) expected_password_local = "GhostNet";

    if (webui_session_cookie_valid(req, expected_password_local)) {
        return respond_with_site(req);
    }

    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");

    if (auth_len == 0 || auth_len >= AUTH_MAX_HDR_LEN) {
        // send Digest challenge
        const FSettings *settings = &G_Settings;
        const char *pwd = settings_get_ap_password(settings);
        if (!pwd || strlen(pwd) < 8) pwd = "GhostNet";
        char nonce[128] = {0};
        if (generate_stateless_nonce(pwd, strlen(pwd), nonce, sizeof(nonce)) != 0) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "Authentication error");
            return ESP_OK;
        }
        char www[256];
        snprintf(www, sizeof(www), "Digest realm=\"Protected Area\", qop=\"auth\", nonce=\"%s\", algorithm=MD5", nonce);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", www);
        httpd_resp_sendstr(req, "Authentication required");
        return ESP_OK;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_buffer, sizeof(auth_buffer)) == ESP_OK) {
        ESP_LOGD(TAG, "Authorization header: %s", auth_buffer);
        if (strncmp(auth_buffer, "Digest ", 7) == 0) {
            char *fields = auth_buffer + 7;
            char username[64] = {0};
            char realm[64] = {0};
            char nonce_hdr[128] = {0};
            char uri_hdr[128] = {0};
            char response[64] = {0};
            char cnonce[64] = {0};
            char nc[16] = {0};
            char qop[16] = {0};

            parse_digest_param(fields, "username", username, sizeof(username));
            parse_digest_param(fields, "realm", realm, sizeof(realm));
            parse_digest_param(fields, "nonce", nonce_hdr, sizeof(nonce_hdr));
            parse_digest_param(fields, "uri", uri_hdr, sizeof(uri_hdr));
            parse_digest_param(fields, "response", response, sizeof(response));
            parse_digest_param(fields, "cnonce", cnonce, sizeof(cnonce));
            parse_digest_param(fields, "nc", nc, sizeof(nc));
            parse_digest_param(fields, "qop", qop, sizeof(qop));

            ESP_LOGI(TAG, "Digest parsed: user='%s' realm='%s' nonce(len)=%d uri='%s' response(len)=%d qop='%s' nc='%s' cnonce='%s'",
                    username, realm, (int)strlen(nonce_hdr), uri_hdr, (int)strlen(response), qop, nc, cnonce);

            const FSettings *settings = &G_Settings;
            const char *expected_username = settings_get_ap_ssid(settings);
            const char *expected_password = settings_get_ap_password(settings);
            if (expected_username == NULL || strlen(expected_username) == 0) expected_username = "GhostNet";
            if (expected_password == NULL || strlen(expected_password) < 8) expected_password = "GhostNet";

            // quick username check
            if (strcmp(username, expected_username) != 0) {
                ESP_LOGW(TAG, "Digest username mismatch: got '%s' expected '%s'", username, expected_username);
                goto digest_fail;
            }

            // validate nonce
            int nv = validate_stateless_nonce(expected_password, strlen(expected_password), nonce_hdr, 300);
            if (nv != 0) {
                // stale nonce -> ask client to retry with new nonce
                char newnonce[128] = {0};
                generate_stateless_nonce(expected_password, strlen(expected_password), newnonce, sizeof(newnonce));
                char www[256];
                snprintf(www, sizeof(www), "Digest realm=\"Protected Area\", qop=\"auth\", nonce=\"%s\", algorithm=MD5, stale=\"true\"", newnonce);
                httpd_resp_set_status(req, "401 Unauthorized");
                httpd_resp_set_hdr(req, "WWW-Authenticate", www);
                httpd_resp_sendstr(req, "Stale nonce");
                return ESP_OK;
            }

            char expected_resp[33] = {0};
            if (compute_digest_response(expected_username, realm, expected_password, "GET", uri_hdr, nonce_hdr, cnonce, nc, qop, expected_resp, sizeof(expected_resp)) != 0) {
                ESP_LOGE(TAG, "Failed to compute expected digest response");
                goto digest_fail;
            }

            // normalize to lowercase before constant-time compare
            str_lower_inplace(response);
            // expected_resp is already lowercase
            if (strlen(response) == strlen(expected_resp) && constant_time_eq((const unsigned char *)response, (const unsigned char *)expected_resp, strlen(expected_resp))) {
                // success
                auth_fail_count = 0;
                auth_last_fail_ts = 0;
                memset(auth_buffer, 0, sizeof(auth_buffer));
                // issue short-lived stateless session cookie (Max-Age=300s)
                char session_token[128] = {0};
                if (generate_stateless_nonce(expected_password, strlen(expected_password), session_token, sizeof(session_token)) == 0) {
                    char cookie[256];
                    snprintf(cookie, sizeof(cookie), "session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=300", session_token);
                    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
                }
                return respond_with_site(req);
            }
        }
    }

digest_fail:
    auth_fail_count = auth_fail_count < 255 ? auth_fail_count + 1 : 255;
    auth_last_fail_ts = (uint32_t)time(NULL);
    memset(auth_buffer, 0, sizeof(auth_buffer));
    // issue new Digest challenge
    const FSettings *settings = &G_Settings;
    const char *pwd = settings_get_ap_password(settings);
    if (!pwd || strlen(pwd) < 8) pwd = "GhostNet";
    char nonce2[128] = {0};
    generate_stateless_nonce(pwd, strlen(pwd), nonce2, sizeof(nonce2));
    char www2[256];
    snprintf(www2, sizeof(www2), "Digest realm=\"Protected Area\", qop=\"auth\", nonce=\"%s\", algorithm=MD5", nonce2);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", www2);
    httpd_resp_sendstr(req, "Invalid credentials");
    return ESP_OK;
}

static esp_err_t api_command_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    int ret;

    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > 4096) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Empty or oversized payload", strlen("Empty or oversized payload"));
        return ESP_FAIL;
    }

    char *content = malloc(content_len + 1);
    if (!content) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "Out of memory", strlen("Out of memory"));
        return ESP_FAIL;
    }

    size_t received_total = 0;
    while (received_total < content_len) {
        ret = httpd_req_recv(req, content + received_total, content_len - received_total);
        if (ret <= 0) {
            free(content);
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
            }
            return ESP_FAIL;
        }
        received_total += ret;
    }
    content[received_total] = '\0';

    cJSON *json = cJSON_Parse(content);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Invalid JSON", strlen("Invalid JSON"));
        free(content);
        return ESP_FAIL;
    }

    cJSON *command_json = cJSON_GetObjectItem(json, "command");
    if (command_json == NULL || !cJSON_IsString(command_json)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, "Missing or invalid 'command' field",
                        strlen("Missing or invalid 'command' field"));
        cJSON_Delete(json);
        free(content);
        return ESP_FAIL;
    }

    const char *command = command_json->valuestring;

    // Add command to log buffer
    char cmd_log[1024];
    snprintf(cmd_log, sizeof(cmd_log), "> %s\n", command);
    ap_manager_add_log(cmd_log);

    simulateCommand(command);

    httpd_resp_send(req, "Command executed", strlen("Command executed"));

    cJSON_Delete(json);
    free(content);
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;
    esp_err_t err;
} api_log_stream_t;

static bool api_logs_send_chunk(const char *data, size_t len, void *ctx) {
    api_log_stream_t *stream = ctx;
    stream->err = httpd_resp_send_chunk(stream->req, data, len);
    return stream->err == ESP_OK;
}

// Handler for the shared terminal/WebUI history.
static esp_err_t api_logs_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    httpd_resp_set_type(req, "text/plain");
    api_log_stream_t stream = {.req = req, .err = ESP_OK};
    if (!terminal_view_visit_history(api_logs_send_chunk, &stream)) {
        ESP_LOGE(TAG, "Failed to stream logs: %s", esp_err_to_name(stream.err));
        return stream.err == ESP_OK ? ESP_FAIL : stream.err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// Handler for /api/clear_logs (clears the shared terminal/WebUI history)
static esp_err_t api_clear_logs_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    terminal_view_clear_history();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"logs_cleared\"}");
}

// Handler for /api/settings (updates settings based on JSON payload)
static esp_err_t api_settings_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Payload too large or empty.\"}");
        return ESP_FAIL;
    }
    int cur_len = 0;
    int received = 0;
    char *buf = malloc(total_len + 1);
    if (!buf) {
        glog("Failed to allocate memory for JSON payload\n");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Memory allocation failed.\"}");
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            free(buf);
            glog("Failed to receive JSON payload\n");
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_sendstr(req, "{\"error\": \"Failed to receive payload.\"}");
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        glog("Failed to parse JSON\n");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON.\"}");
        return ESP_FAIL;
    }

    // Update settings
    FSettings *settings = &G_Settings;

    // Core settings
    cJSON *broadcast_speed = cJSON_GetObjectItem(root, "broadcast_speed");
    if (broadcast_speed) {
        settings_set_broadcast_speed(settings, broadcast_speed->valueint);
    }

    cJSON *ap_ssid = cJSON_GetObjectItem(root, "ap_ssid");
    if (cJSON_IsString(ap_ssid) && ap_ssid->valuestring) {
        settings_set_ap_ssid(settings, ap_ssid->valuestring);
    }

    cJSON *ap_password = cJSON_GetObjectItem(root, "ap_password");
    if (cJSON_IsString(ap_password) && ap_password->valuestring) {
        size_t ap_password_len = strlen(ap_password->valuestring);
        if (ap_password_len > 0 && (ap_password_len < 8 || ap_password_len > 63)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\": \"AP password must be empty or 8-63 characters.\"}");
            return ESP_FAIL;
        }
        if (ap_password_len > 0) {
            settings_set_ap_password(settings, ap_password->valuestring);
        }
    }

    cJSON *rgb_mode = cJSON_GetObjectItem(root, "rainbow_mode");
    if (cJSON_IsBool(rgb_mode)) {
        bool rgb_mode_value = cJSON_IsTrue(rgb_mode);
        printf("Debug: Passed rgb_mode_value = %d to settings_set_rgb_mode()\n", rgb_mode_value);
        settings_set_rgb_mode(settings, (RGBMode)rgb_mode_value);
    } else {
        glog("Error: 'rgb_mode' is not a boolean.\n");
    }

    cJSON *rgb_speed = cJSON_GetObjectItem(root, "rgb_speed");
    if (rgb_speed) {
        settings_set_rgb_speed(settings, rgb_speed->valueint);
    }

    cJSON *neopixel_brightness = cJSON_GetObjectItem(root, "neopixel_brightness");
    if (neopixel_brightness) {
        settings_set_neopixel_max_brightness(settings, (uint8_t)neopixel_brightness->valueint);
    }

    cJSON *channel_delay = cJSON_GetObjectItem(root, "channel_delay");
    if (channel_delay) {
        settings_set_channel_delay(settings, (float)channel_delay->valuedouble);
    }

    // Evil Portal settings
    cJSON *portal_url = cJSON_GetObjectItem(root, "portal_url");
    if (cJSON_IsString(portal_url) && portal_url->valuestring) {
        settings_set_portal_url(settings, portal_url->valuestring);
    }

    cJSON *portal_ssid = cJSON_GetObjectItem(root, "portal_ssid");
    if (cJSON_IsString(portal_ssid) && portal_ssid->valuestring) {
        settings_set_portal_ssid(settings, portal_ssid->valuestring);
    }

    cJSON *portal_password = cJSON_GetObjectItem(root, "portal_password");
    if (cJSON_IsString(portal_password) && portal_password->valuestring) {
        if (portal_password->valuestring[0] != '\0') {
            settings_set_portal_password(settings, portal_password->valuestring);
        }
    }

    cJSON *portal_ap_ssid = cJSON_GetObjectItem(root, "portal_ap_ssid");
    if (cJSON_IsString(portal_ap_ssid) && portal_ap_ssid->valuestring) {
        settings_set_portal_ap_ssid(settings, portal_ap_ssid->valuestring);
    }

    cJSON *portal_domain = cJSON_GetObjectItem(root, "portal_domain");
    if (cJSON_IsString(portal_domain) && portal_domain->valuestring) {
        settings_set_portal_domain(settings, portal_domain->valuestring);
    }

    cJSON *portal_offline_mode = cJSON_GetObjectItem(root, "portal_offline_mode");
    if (portal_offline_mode) {
        settings_set_portal_offline_mode(settings, portal_offline_mode->valueint != 0);
    }

    // Power Printer settings
    cJSON *printer_ip = cJSON_GetObjectItem(root, "printer_ip");
    if (cJSON_IsString(printer_ip) && printer_ip->valuestring) {
        settings_set_printer_ip(settings, printer_ip->valuestring);
    }

    cJSON *printer_text = cJSON_GetObjectItem(root, "printer_text");
    if (cJSON_IsString(printer_text) && printer_text->valuestring) {
        settings_set_printer_text(settings, printer_text->valuestring);
    }

    cJSON *printer_font_size = cJSON_GetObjectItem(root, "printer_font_size");
    if (printer_font_size) {
        printf("PRINTER FONT SIZE %i", printer_font_size->valueint);
        settings_set_printer_font_size(settings, printer_font_size->valueint);
    }

    cJSON *printer_alignment = cJSON_GetObjectItem(root, "printer_alignment");
    if (printer_alignment) {
        printf("printer_alignment %i", printer_alignment->valueint);
        settings_set_printer_alignment(settings, (PrinterAlignment)printer_alignment->valueint);
    }

    cJSON *flappy_ghost_name = cJSON_GetObjectItem(root, "flappy_ghost_name");
    if (cJSON_IsString(flappy_ghost_name) && flappy_ghost_name->valuestring) {
        settings_set_flappy_ghost_name(settings, flappy_ghost_name->valuestring);
    }

    cJSON *time_zone_str_name = cJSON_GetObjectItem(root, "timezone_str");
    if (cJSON_IsString(time_zone_str_name) && time_zone_str_name->valuestring) {
        settings_set_timezone_str(settings, time_zone_str_name->valuestring);
    }

    cJSON *hex_accent_color_str = cJSON_GetObjectItem(root, "hex_accent_color");
    if (cJSON_IsString(hex_accent_color_str) && hex_accent_color_str->valuestring) {
        settings_set_accent_color_str(settings, hex_accent_color_str->valuestring);
    }

    cJSON *rts_enabled_bool = cJSON_GetObjectItem(root, "rts_enabled");
    if (rts_enabled_bool) {
        settings_set_rts_enabled(settings, rts_enabled_bool->valueint != 0);
    }

    cJSON *web_auth_enabled_bool = cJSON_GetObjectItem(root, "web_auth_enabled");
    if (web_auth_enabled_bool) {
        settings_set_web_auth_enabled(settings, web_auth_enabled_bool->valueint != 0);
    }

    cJSON *ap_enabled_bool = cJSON_GetObjectItem(root, "ap_enabled");
    if (ap_enabled_bool) {
        settings_set_ap_enabled(settings, ap_enabled_bool->valueint != 0);
    }

    cJSON *gps_rx_pin = cJSON_GetObjectItem(root, "gps_rx_pin");
    if (gps_rx_pin) {
        settings_set_gps_rx_pin(settings, gps_rx_pin->valueint);
    }

    cJSON *gps_baud_rate = cJSON_GetObjectItem(root, "gps_baud_rate");
    if (gps_baud_rate) {
        settings_set_gps_baud_rate(settings, (uint32_t)gps_baud_rate->valueint);
    }

    // Handle display timeout
    cJSON *display_timeout = cJSON_GetObjectItem(root, "display_timeout");
    if (display_timeout) {
        settings_set_display_timeout(settings, display_timeout->valueint);
        ESP_LOGI(TAG, "Setting display timeout to: %d ms", display_timeout->valueint);
    }

    // GhostLink (ESP comm) TX/RX pins. Persist them so they survive reboots.
    // -1 is the "not used / disabled" sentinel (boards whose shared pins are
    // otherwise claimed); a live re-pin while connected/scanning is disruptive,
    // so these are staged and applied on the next comm manager init.
    cJSON *esp_comm_tx_pin = cJSON_GetObjectItem(root, "esp_comm_tx_pin");
    cJSON *esp_comm_rx_pin = cJSON_GetObjectItem(root, "esp_comm_rx_pin");
    if (esp_comm_tx_pin && esp_comm_rx_pin) {
        int32_t comm_tx = esp_comm_tx_pin->valueint;
        int32_t comm_rx = esp_comm_rx_pin->valueint;
        if ((comm_tx < -1 || comm_tx > 48) || (comm_rx < -1 || comm_rx > 48)) {
            cJSON_Delete(root);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\": \"esp_comm pins must be -1 or 0-48.\"}");
            return ESP_FAIL;
        }
        settings_set_esp_comm_pins(settings, comm_tx, comm_rx);
    }
    glog("About to Save Settings\n");

    esp_err_t save_err = settings_save(settings);
    if (save_err != ESP_OK) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to save settings.\"}");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"settings_updated\"}");

    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t api_settings_get_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    FSettings *settings = &G_Settings;

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        glog("Failed to create JSON object\n");
        return ESP_FAIL;
    }

    cJSON_AddNumberToObject(root, "broadcast_speed", settings_get_broadcast_speed(settings));
    cJSON_AddStringToObject(root, "ap_ssid", settings_get_ap_ssid(settings));
    cJSON_AddStringToObject(root, "ap_password", "");
    cJSON_AddNumberToObject(root, "rgb_mode", settings_get_rgb_mode(settings));
    cJSON_AddNumberToObject(root, "rgb_speed", settings_get_rgb_speed(settings));
    cJSON_AddNumberToObject(root, "channel_delay", settings_get_channel_delay(settings));

    cJSON_AddStringToObject(root, "portal_url", settings_get_portal_url(settings));
    cJSON_AddStringToObject(root, "portal_ssid", settings_get_portal_ssid(settings));
    cJSON_AddStringToObject(root, "portal_password", "");
    cJSON_AddStringToObject(root, "portal_ap_ssid", settings_get_portal_ap_ssid(settings));
    cJSON_AddStringToObject(root, "portal_domain", settings_get_portal_domain(settings));
    cJSON_AddBoolToObject(root, "portal_offline_mode", settings_get_portal_offline_mode(settings));

    cJSON_AddStringToObject(root, "printer_ip", settings_get_printer_ip(settings));
    cJSON_AddStringToObject(root, "printer_text", settings_get_printer_text(settings));
    cJSON_AddNumberToObject(root, "printer_font_size", settings_get_printer_font_size(settings));
    cJSON_AddNumberToObject(root, "printer_alignment", settings_get_printer_alignment(settings));
    cJSON_AddStringToObject(root, "hex_accent_color", settings_get_accent_color_str(settings));
    cJSON_AddStringToObject(root, "timezone_str", settings_get_timezone_str(settings));
    cJSON_AddNumberToObject(root, "gps_rx_pin", settings_get_gps_rx_pin(settings));
    cJSON_AddNumberToObject(root, "gps_baud_rate", settings_get_gps_baud_rate(settings));
    cJSON_AddNumberToObject(root, "display_timeout", settings_get_display_timeout(settings));
    cJSON_AddNumberToObject(root, "rts_enabled_bool", settings_get_rts_enabled(settings));
    cJSON_AddBoolToObject(root, "web_auth_enabled", settings_get_web_auth_enabled(settings));
    cJSON_AddBoolToObject(root, "ap_enabled", settings_get_ap_enabled(settings));

    // Add ESP communication pin settings
    int32_t tx_pin, rx_pin;
    settings_get_esp_comm_pins(settings, &tx_pin, &rx_pin);
    cJSON_AddNumberToObject(root, "esp_comm_tx_pin", tx_pin);
    cJSON_AddNumberToObject(root, "esp_comm_rx_pin", rx_pin);

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        if (ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(root, "station_ip", ip_str);
        }
    }

    const char *json_response = cJSON_Print(root);
    if (!json_response) {
        cJSON_Delete(root);
        glog("Failed to print JSON object\n");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_response);

    cJSON_Delete(root);
    free((void *)json_response);

    return ESP_OK;
}

// Handler for ESP communication status
static esp_err_t api_esp_comm_status_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to create JSON object\"}");
        return ESP_FAIL;
    }

    comm_state_t state = esp_comm_manager_get_state();
    const char *state_str = "unknown";
    switch (state) {
        case COMM_STATE_IDLE: state_str = "idle"; break;
        case COMM_STATE_SCANNING: state_str = "scanning"; break;
        case COMM_STATE_HANDSHAKE: state_str = "handshake"; break;
        case COMM_STATE_CONNECTED: state_str = "connected"; break;
        case COMM_STATE_ERROR: state_str = "error"; break;
        default: state_str = "unknown"; break;
    }

    cJSON_AddStringToObject(root, "state", state_str);
    cJSON_AddBoolToObject(root, "connected", esp_comm_manager_is_connected());
    cJSON_AddBoolToObject(root, "is_remote_command", esp_comm_manager_is_remote_command());

    char *response_string = cJSON_Print(root);
    if (!response_string) {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\": \"Failed to serialize JSON\"}");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(root);
    free(response_string);
    return ESP_OK;
}

// Handler for ESP communication control (start discovery, connect, disconnect)
static esp_err_t api_esp_comm_control_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    char content[512];
    int ret = httpd_req_recv(req, content, MIN_(req->content_len, sizeof(content) - 1));
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload\"}");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload\"}");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid action\"}");
        return ESP_FAIL;
    }

    const char *action_str = action->valuestring;
    bool success = false;
    char response_msg[256] = {0};

    if (strcmp(action_str, "start_discovery") == 0) {
        success = esp_comm_manager_start_discovery();
        snprintf(response_msg, sizeof(response_msg), "Discovery %s", success ? "started" : "failed");
    } else if (strcmp(action_str, "connect") == 0) {
        cJSON *peer_name = cJSON_GetObjectItem(json, "peer_name");
        if (peer_name && cJSON_IsString(peer_name)) {
            success = esp_comm_manager_connect_to_peer(peer_name->valuestring);
            snprintf(response_msg, sizeof(response_msg), "Connection to %s %s", 
                     peer_name->valuestring, success ? "initiated" : "failed");
        } else {
            snprintf(response_msg, sizeof(response_msg), "Missing peer name");
        }
    } else if (strcmp(action_str, "disconnect") == 0) {
        esp_comm_manager_disconnect();
        success = true;
        snprintf(response_msg, sizeof(response_msg), "Disconnected");
    } else {
        snprintf(response_msg, sizeof(response_msg), "Unknown action: %s", action_str);
    }

    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    cJSON_AddStringToObject(response, "message", response_msg);

    char *response_string = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(json);
    cJSON_Delete(response);
    free(response_string);
    return ESP_OK;
}

// Handler for sending ESP communication commands
static esp_err_t api_esp_comm_send_handler(httpd_req_t *req) {
    WEBUI_GUARD_OR_RETURN(req);
    char content[512];
    int ret = httpd_req_recv(req, content, MIN_(req->content_len, sizeof(content) - 1));
    if (ret <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid request payload\"}");
        return ESP_FAIL;
    }
    
    content[ret] = '\0';
    
    cJSON *json = cJSON_Parse(content);
    if (!json) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Invalid JSON payload\"}");
        return ESP_FAIL;
    }

    cJSON *command = cJSON_GetObjectItem(json, "command");
    if (!command || !cJSON_IsString(command)) {
        cJSON_Delete(json);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\": \"Missing or invalid command\"}");
        return ESP_FAIL;
    }

    // Send the full command string as-is (no separate data field needed)
    bool success = esp_comm_manager_send_command(command->valuestring, NULL);
    
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", success);
    cJSON_AddStringToObject(response, "message", success ? "Command sent successfully" : "Failed to send command");

    char *response_string = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, response_string);

    cJSON_Delete(json);
    cJSON_Delete(response);
    free(response_string);
    return ESP_OK;
}

// Event handler for Wi-Fi events
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                          void *event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_START:
            glog("AP_manager: AP started\n");
            break;
        case WIFI_EVENT_AP_STOP:
            glog("AP_manager: AP stopped\n");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            glog("AP_manager: Device connected to AP\n");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            glog("AP_manager: Device disconnected from AP\n");

            break;
        case WIFI_EVENT_STA_START: {
            // No auto-connect here - handled by wifi_manager's wifi_event_handler
            glog("AP_manager: STA interface started\n");
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *) event_data;
            glog("Disconnected\nReason: %d\n", disconn->reason);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
            
            // Get SSID from active connection
            wifi_ap_record_t ap_info;
            if(esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                glog("\nConnected!\nSSID: %.*s\nIP: " IPSTR "\n",
                     18, ap_info.ssid,
                     IP2STR(&event->ip_info.ip));
            } else {
                glog("\nConnected!\nIP: " IPSTR "\n",
                     IP2STR(&event->ip_info.ip));
            }
            break;
        }
        case IP_EVENT_AP_STAIPASSIGNED:
            glog("Assigned STA IP\n");
            break;
        default:
            break;
        }
    }
}

static esp_err_t load_server_config(void) {
    if (config_loaded) {
        ESP_LOGW(TAG, "Server config already loaded");
        return ESP_OK;
    }

    if (server != NULL) {
        ESP_LOGE(TAG, "Cannot load config while server is running");
        return ESP_FAIL;
    }

    server_config = (httpd_config_t)HTTPD_DEFAULT_CONFIG();
    server_config.server_port = 80;
    server_config.ctrl_port = 32768;
    server_config.max_uri_handlers = 60;
    server_config.stack_size = 6144;
    server_config.recv_wait_timeout = 10;
    server_config.send_wait_timeout = 10;

    handler_count = 0;

#define ADD_URI_HANDLER(uri_path, method_type, handler_func) do { \
    if (handler_count >= sizeof(uri_handlers)/sizeof(uri_handlers[0])) { \
        ESP_LOGE(TAG, "Too many URI handlers, cannot add: %s", uri_path); \
        return ESP_FAIL; \
    } \
    uri_handlers[handler_count++] = (httpd_uri_t){ \
        .uri = uri_path, \
        .method = method_type, \
        .handler = handler_func, \
        .user_ctx = NULL \
    }; \
} while(0)

    ADD_URI_HANDLER("/", HTTP_GET, http_get_handler);
    ADD_URI_HANDLER("/api/settings", HTTP_POST, api_settings_handler);
    ADD_URI_HANDLER("/api/settings", HTTP_GET, api_settings_get_handler);
    ADD_URI_HANDLER("/api/sdcard", HTTP_GET, api_sd_card_get_handler);
    ADD_URI_HANDLER("/api/sdcard/download", HTTP_POST, api_sd_card_post_handler);
    ADD_URI_HANDLER("/api/sdcard/upload", HTTP_POST, api_sd_card_upload_handler);
    ADD_URI_HANDLER("/api/sdcard", HTTP_DELETE, api_sd_card_delete_file_handler);
    ADD_URI_HANDLER("/api/command", HTTP_POST, api_command_handler);
    ADD_URI_HANDLER("/api/logs", HTTP_GET, api_logs_handler);
    ADD_URI_HANDLER("/api/clear_logs", HTTP_POST, api_clear_logs_handler);
    ADD_URI_HANDLER("/api/esp_comm/status", HTTP_GET, api_esp_comm_status_handler);
    ADD_URI_HANDLER("/api/esp_comm/control", HTTP_POST, api_esp_comm_control_handler);
    ADD_URI_HANDLER("/api/esp_comm/send", HTTP_POST, api_esp_comm_send_handler);
#ifdef CONFIG_HAS_CAMERA
    ADD_URI_HANDLER("/camera", HTTP_GET, camera_stream_page_handler);
    ADD_URI_HANDLER("/camera/stream", HTTP_GET, camera_stream_http_handler);
    ADD_URI_HANDLER("/camera/api", HTTP_POST, camera_stream_api_handler);
#endif

#undef ADD_URI_HANDLER

    config_loaded = true;
    ESP_LOGI(TAG, "Server configuration loaded successfully with %d handlers", handler_count);
    return ESP_OK;
}

static esp_err_t start_http_server(void) {
    if (!config_loaded) {
        ESP_LOGE(TAG, "Server config not loaded");
        return ESP_FAIL;
    }

    if (server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ret = httpd_start(&server, &server_config);
        if (ret == ESP_ERR_HTTPD_TASK && server_config.stack_size > 4096) {
            server_config.stack_size = 4096;
            ret = httpd_start(&server, &server_config);
        }
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGE(TAG, "Failed to start HTTP server (attempt %d): %s", attempt + 1, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ret != ESP_OK) {
        return ret;
    }

    for (int i = 0; i < handler_count; i++) {
        ret = httpd_register_uri_handler(server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler %d: %s", i, esp_err_to_name(ret));
            httpd_stop(server);
            server = NULL;
            return ret;
        }
    }

    ESP_LOGI(TAG, "HTTP server started successfully on port %d", server_config.server_port);
    return ESP_OK;
}

static esp_err_t stop_http_server(void) {
    if (server) {
        esp_err_t ret = httpd_stop(server);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(ret));
            return ret;
        }
        server = NULL;
        wifi_manager_release_stream_buffer();
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

static void reset_server_config(void) {
    config_loaded = false;
    handler_count = 0;
    memset(&server_config, 0, sizeof(server_config));
    memset(uri_handlers, 0, sizeof(uri_handlers));
    ESP_LOGI(TAG, "Server configuration reset");
}

static bool is_server_running(void) {
    return server != NULL;
}

static bool is_config_loaded(void) {
    return config_loaded;
}

esp_err_t ap_manager_reload_config(void) {
    ESP_LOGI(TAG, "Reloading server configuration and mDNS");
    
    esp_err_t ret = stop_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop server before reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    teardown_mdns();
    reset_server_config();
    
    ret = load_server_config();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reload config: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = setup_mdns();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to setup mDNS after reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart server after reload: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "Server configuration and mDNS reloaded successfully");
    return ESP_OK;
}

void ap_manager_get_status(bool *server_running, bool *config_loaded_status, int *handler_count_status) {
    if (server_running) *server_running = is_server_running();
    if (config_loaded_status) *config_loaded_status = is_config_loaded();
    if (handler_count_status) *handler_count_status = handler_count;
}

static esp_err_t setup_mdns(void) {
    esp_err_t ret = mdns_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    mdns_freed = false;

    ret = mdns_hostname_set("ghostesp");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_instance_name_set("GhostESP Web Interface");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(ret));
    }

    mdns_txt_item_t serviceTxtData[] = {{"ip", "192.168.4.1"}, {"ipv4", "192.168.4.1"}};

    ret = mdns_service_add("GhostESP", "_http", "_tcp", 80, serviceTxtData, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = mdns_service_txt_set("_http", "_tcp", serviceTxtData, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mdns_service_txt_set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    

    ESP_LOGI(TAG, "mDNS setup completed successfully");
    return ESP_OK;
}

static esp_err_t teardown_mdns(void) {
    if (!mdns_freed) {
        mdns_free();
        mdns_freed = true;
        ESP_LOGI(TAG, "mDNS teardown completed");
    }
    return ESP_OK;
}
