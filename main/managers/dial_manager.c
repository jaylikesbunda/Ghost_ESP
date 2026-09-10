#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

#include "managers/dial_manager.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DIAL_HTTP_BUFFER_SIZE 512
#define DIAL_RESPONSE_BUFFER_SIZE 512
#define DIAL_MIN_FREE_HEAP_FOR_HTTPS 25000

static const char *TAG = "DIALManager";

// Default name for DIAL device in bind session
static char g_dial_device_name[64] = "Flipper_0";
// Set the device name used in DIAL bind session
void dial_manager_set_device_name(const char *name) {
  if (name && *name) {
    strncpy(g_dial_device_name, name, sizeof(g_dial_device_name) - 1);
    g_dial_device_name[sizeof(g_dial_device_name) - 1] = '\0';
  }
}

typedef struct {
  char *buffer;
  int buffer_len;
  int buffer_size;
} response_buffer_t;

typedef struct {
  char *sid;        // Store SID
  char *gsessionid; // Store gsessionid
  char *listId;     // Store listId
} BindSession_Params_t;

char *getC(const char *input) {
  const char *start = strstr(input, "c\",\"");

  if (start != NULL) {
    start += 4;

    const char *end = strchr(start, '"');

    if (end != NULL) {
      size_t len = end - start;
      char *result = (char *)malloc(len + 1);

      if (result != NULL) {
        strncpy(result, start, len);
        result[len] = '\0';
      }

      return result;
    }
  }

  return NULL;
}

char *getS(const char *input) {
  const char *start = strstr(input, "S\",\"");

  if (start != NULL) {
    start += 4;

    const char *end = strchr(start, '"');

    if (end != NULL) {
      size_t len = end - start;
      char *result = (char *)malloc(len + 1);

      if (result != NULL) {
        strncpy(result, start, len);
        result[len] = '\0';
      }

      return result;
    }
  }

  return NULL;
}

char *getlistId(const char *input) {
  const char *start = strstr(input, "\"playlistModified\",{\"listId\":\"");

  if (start != NULL) {
    start += 30;

    const char *end = strchr(start, '"');

    if (end != NULL) {
      size_t len = end - start;
      char *result = (char *)malloc(len + 1);

      if (result != NULL) {
        strncpy(result, start, len);
        result[len] = '\0';
      }

      return result;
    }
  }

  return NULL;
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  response_buffer_t *resp_buf = evt->user_data;
  switch (evt->event_id) {
  case HTTP_EVENT_ON_DATA:
    if (resp_buf->buffer_len + evt->data_len >= resp_buf->buffer_size) {
      resp_buf->buffer_size += evt->data_len;
      char *new_buffer = realloc(resp_buf->buffer, resp_buf->buffer_size);
      if (new_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for response buffer");
        return ESP_FAIL;
      }
      resp_buf->buffer = new_buffer;
    }
    memcpy(resp_buf->buffer + resp_buf->buffer_len, evt->data, evt->data_len);
    resp_buf->buffer_len += evt->data_len;
    break;
  default:
    break;
  }
  return ESP_OK;
}

#define SERVER_PORT 443
#define MAX_APP_URL_LENGTH 500

BindSession_Params_t parse_response(const char *response_body) {
  BindSession_Params_t result;

  result.gsessionid = getS(response_body);
  result.sid = getC(response_body);
  result.listId = getlistId(response_body);

  return result;
}

char *url_encode(const char *str) {
  const char *hex = "0123456789ABCDEF";
  char *encoded = malloc(strlen(str) * 3 + 1); // Worst case scenario
  char *pencoded = encoded;
  if (!encoded) {
    ESP_LOGE(TAG, "Failed to allocate memory for URL encoding");
    return NULL;
  }

  while (*str) {
    if (('a' <= *str && *str <= 'z') || ('A' <= *str && *str <= 'Z') ||
        ('0' <= *str && *str <= '9') ||
        (*str == '-' || *str == '_' || *str == '.' || *str == '~')) {
      *pencoded++ = *str;
    } else {
      *pencoded++ = '%';
      *pencoded++ = hex[(*str >> 4) & 0xF];
      *pencoded++ = hex[*str & 0xF];
    }
    str++;
  }
  *pencoded = '\0';
  return encoded;
}

void generate_uuid(char *uuid, size_t uuid_len) {
  snprintf(uuid, uuid_len, "%08x-%04x-%04x-%04x-%08x%04x",
           (unsigned int)esp_random(),            // 8 hex digits
           (unsigned int)(esp_random() & 0xFFFF), // 4 hex digits
           (unsigned int)(esp_random() & 0xFFFF), // 4 hex digits
           (unsigned int)(esp_random() & 0xFFFF), // 4 hex digits
           (unsigned int)(esp_random()), // First 8 hex digits of the last part
           (unsigned int)(esp_random() &
                          0xFFFF)); // Last 4 hex digits of the last part
}

char *generate_zx() {
  static const char alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  char *zx = malloc(17);
  if (!zx) {
    ESP_LOGE(TAG, "Failed to allocate memory for zx");
    return NULL;
  }
  for (int i = 0; i < 16; ++i) {
    zx[i] = alphanum[esp_random() % (sizeof(alphanum) - 1)];
  }
  zx[16] = '\0';
  return zx;
}

char *extract_path_from_url(const char *url) {
  // Find the first slash after the host and port (i.e., after "://host:port/")
  const char *path_start = strchr(url + 7, '/'); // +7 to skip over "http://"
  if (path_start) {
    return strdup(path_start); // Duplicate and return the path
  }
  return strdup("/"); // Default to "/" if no path is found
}

char *extract_application_url(const char *headers) {
  const char *app_url_header = strstr(headers, "Application-Url");
  if (!app_url_header) {
    return NULL;
  }

  const char *url_start = app_url_header + strlen("Application-Url");
  while (*url_start == ' ')
    url_start++; // Skip leading spaces

  const char *url_end = strchr(url_start, '\r');
  if (!url_end) {
    return NULL;
  }

  size_t url_len = url_end - url_start;
  char *application_url = malloc(url_len + 1);
  if (application_url) {
    strncpy(application_url, url_start, url_len);
    application_url[url_len] = '\0'; // Null-terminate the URL
  }

  return application_url;
}

esp_err_t send_command(const char *command, const char *video_id,
                       const Device *device) {
  if (!device || !command) {
    ESP_LOGE(TAG, "Invalid arguments: device or command is NULL.");
    return ESP_ERR_INVALID_ARG;
  }

  response_buffer_t resp_buf = {0};
  esp_http_client_handle_t client = NULL;
  char *url_params = NULL;
  char *body_params = NULL;
  char *full_url = NULL;
  esp_err_t result = ESP_OK;

  // URL-encode parameters
  char *encoded_loungeIdToken = url_encode(device->YoutubeToken);
  char *encoded_SID = url_encode(device->SID);
  char *encoded_gsession = url_encode(device->gsession);
  char *encoded_command = url_encode(command);
  char *encoded_video_id = video_id
                               ? url_encode(video_id)
                               : NULL; // For commands that don't need video_id

  if (!encoded_loungeIdToken || !encoded_SID || !encoded_gsession ||
      !encoded_command || (!encoded_video_id && video_id)) {
    ESP_LOGE(TAG, "URL encoding failed for one or more parameters.");
    result = ESP_FAIL;
    goto cleanup;
  }

  size_t url_params_len = snprintf(NULL, 0,
           "CVER=1&RID=1&SID=%s&VER=8&gsessionid=%s&loungeIdToken=%s",
           encoded_SID, encoded_gsession, encoded_loungeIdToken) + 1;
  url_params = malloc(url_params_len);
  if (!url_params) {
    ESP_LOGE(TAG, "Failed to allocate memory for URL parameters");
    result = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  snprintf(url_params, url_params_len,
           "CVER=1&RID=1&SID=%s&VER=8&gsessionid=%s&loungeIdToken=%s",
           encoded_SID, encoded_gsession, encoded_loungeIdToken);
  ESP_LOGI(TAG, "Query Parameters: %s", url_params);

  size_t body_params_len = 0;
  if (strcmp(command, "setVideo") == 0) {
    body_params_len = snprintf(NULL, 0,
             "count=1&req0__sc=%s&req0_videoId=%s&req0_currentTime=0&req0_currentIndex=0&req0_videoIds=%s",
             encoded_command, encoded_video_id, encoded_video_id) + 1;
    body_params = malloc(body_params_len);
    if (!body_params) {
      ESP_LOGE(TAG, "Failed to allocate memory for body parameters");
      result = ESP_ERR_NO_MEM;
      goto cleanup;
    }
    snprintf(body_params, body_params_len,
             "count=1&req0__sc=%s&req0_videoId=%s&req0_currentTime=0&req0_currentIndex=0&req0_videoIds=%s",
             encoded_command, encoded_video_id, encoded_video_id);
  } else if (strcmp(command, "addVideo") == 0) {
    body_params_len = snprintf(NULL, 0,
             "count=1&req0__sc=%s&req0_videoId=%s", encoded_command,
             encoded_video_id) + 1;
    body_params = malloc(body_params_len);
    if (!body_params) {
      ESP_LOGE(TAG, "Failed to allocate memory for body parameters");
      result = ESP_ERR_NO_MEM;
      goto cleanup;
    }
    snprintf(body_params, body_params_len,
             "count=1&req0__sc=%s&req0_videoId=%s", encoded_command,
             encoded_video_id);
  } else if (strcmp(command, "play") == 0 || strcmp(command, "pause") == 0) {
    body_params_len = snprintf(NULL, 0, "count=1&req0__sc=%s",
             encoded_command) + 1;
    body_params = malloc(body_params_len);
    if (!body_params) {
      ESP_LOGE(TAG, "Failed to allocate memory for body parameters");
      result = ESP_ERR_NO_MEM;
      goto cleanup;
    }
    snprintf(body_params, body_params_len, "count=1&req0__sc=%s",
             encoded_command);
  } else {
    ESP_LOGE(TAG, "Unsupported command: %s", command);
    result = ESP_ERR_INVALID_ARG;
    goto cleanup;
  }
  ESP_LOGI(TAG, "Body Parameters: %s", body_params);

  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (free_heap < DIAL_MIN_FREE_HEAP_FOR_HTTPS) {
    ESP_LOGE(TAG, "Insufficient heap for HTTPS: %u bytes free, need %d", free_heap, DIAL_MIN_FREE_HEAP_FOR_HTTPS);
    result = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  resp_buf.buffer = malloc(DIAL_RESPONSE_BUFFER_SIZE);
  resp_buf.buffer_len = 0;
  resp_buf.buffer_size = DIAL_RESPONSE_BUFFER_SIZE;
  if (!resp_buf.buffer) {
    ESP_LOGE(TAG, "Failed to allocate memory for response buffer.");
    result = ESP_FAIL;
    goto cleanup;
  }

  esp_http_client_config_t config = {
      .url = "https://www.youtube.com/api/lounge/bc/bind",
      .timeout_ms = 5000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .event_handler = _http_event_handler,
      .user_data = &resp_buf,
      .buffer_size = DIAL_HTTP_BUFFER_SIZE,
      .buffer_size_tx = DIAL_HTTP_BUFFER_SIZE,
  };
  client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    result = ESP_FAIL;
    goto cleanup;
  }

  // Set the request body
  esp_http_client_set_post_field(client, body_params, strlen(body_params));

  // Dynamically allocate buffer for full_url
  size_t full_url_len = strlen(config.url) + 1 + strlen(url_params) + 1; // base + '?' + params + '\0'
  full_url = malloc(full_url_len);
  if (full_url == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for full_url");
    result = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  snprintf(full_url, full_url_len, "%s?%s", config.url, url_params);
  esp_http_client_set_url(client, full_url);

  ESP_LOGI(TAG, "Full URL: %s", full_url);
  ESP_LOGI(TAG, "POST Body: %s", body_params);

  // Set HTTP method, headers, and post data
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type",
                             "application/x-www-form-urlencoded");
  esp_http_client_set_header(client, "Origin", "https://www.youtube.com");

  // Perform the HTTP request
  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    result = err;
    goto cleanup;
  }

  ESP_LOGI(TAG, "Response: %s", resp_buf.buffer);

cleanup:
  // Free allocated memory
  free(encoded_loungeIdToken);
  free(encoded_SID);
  free(encoded_gsession);
  free(encoded_command);
  if (encoded_video_id) {
    free(encoded_video_id);
  }
  if (resp_buf.buffer) {
    free(resp_buf.buffer);
  }
  free(url_params);
  free(body_params);
  esp_http_client_cleanup(client);
  free(full_url); // Free allocated memory

  return result;
}

esp_err_t bind_session_id(Device *device) {
  if (!device) {
    ESP_LOGE(TAG, "Invalid device pointer.");
    return ESP_ERR_INVALID_ARG;
  }

  generate_uuid(device->UUID, sizeof(device->UUID));

  // Generate ZX parameter
  char *zx = generate_zx();
  if (!zx) {
    ESP_LOGE(TAG, "Failed to generate zx");
    return ESP_FAIL;
  }

  // URL-encode parameter values
  char *encoded_loungeIdToken = url_encode(device->YoutubeToken);
  char *encoded_UUID = url_encode(device->UUID);
  char *encoded_zx = url_encode(zx);
  char *encoded_name = url_encode(g_dial_device_name);

  if (!encoded_loungeIdToken || !encoded_UUID || !encoded_zx || !encoded_name) {
    ESP_LOGE(TAG, "Failed to URL-encode parameters.");
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    return ESP_FAIL;
  }

  size_t url_params_len = snprintf(NULL, 0,
      "device=REMOTE_CONTROL&mdx-version=3&ui=1&v=2&name=%s"
      "&app=youtube-desktop&loungeIdToken=%s&id=%s&VER=8&CVER=1&zx=%s&RID=%i",
      encoded_name, encoded_loungeIdToken, encoded_UUID, encoded_zx, 1) + 1;
  char *url_params = malloc(url_params_len);
  if (!url_params) {
    ESP_LOGE(TAG, "Failed to allocate memory for URL parameters");
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    return ESP_ERR_NO_MEM;
  }
  snprintf(url_params, url_params_len,
      "device=REMOTE_CONTROL&mdx-version=3&ui=1&v=2&name=%s"
      "&app=youtube-desktop&loungeIdToken=%s&id=%s&VER=8&CVER=1&zx=%s&RID=%i",
      encoded_name, encoded_loungeIdToken, encoded_UUID, encoded_zx, 1);
  ESP_LOGI(TAG, "Constructed URL: %s?%s", "https://www.youtube.com/api/lounge/bc/bind", url_params);

  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (free_heap < DIAL_MIN_FREE_HEAP_FOR_HTTPS) {
    ESP_LOGE(TAG, "Insufficient heap for HTTPS: %u bytes free, need %d", free_heap, DIAL_MIN_FREE_HEAP_FOR_HTTPS);
    printf("    [-] Insufficient memory for HTTPS (need %dKB, have %uKB)\n", 
           DIAL_MIN_FREE_HEAP_FOR_HTTPS/1024, free_heap/1024);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    free(url_params);
    return ESP_ERR_NO_MEM;
  }

  response_buffer_t resp_buf = {
      .buffer = malloc(DIAL_RESPONSE_BUFFER_SIZE), .buffer_len = 0, .buffer_size = DIAL_RESPONSE_BUFFER_SIZE};
  if (resp_buf.buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for response buffer");
    free(url_params);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    return ESP_FAIL;
  }

  esp_http_client_config_t config = {
      .url = "https://www.youtube.com/api/lounge/bc/bind",
      .timeout_ms = 5000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .event_handler = _http_event_handler,
      .user_data = &resp_buf,
      .buffer_size = DIAL_HTTP_BUFFER_SIZE,
      .buffer_size_tx = DIAL_HTTP_BUFFER_SIZE,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client.");
    free(url_params);
    free(resp_buf.buffer);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    return ESP_FAIL;
  }

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "Origin", "https://www.youtube.com");

  size_t full_url_len = strlen(config.url) + 1 + strlen(url_params) + 1;
  char *full_url = malloc(full_url_len);
  if (!full_url) {
    ESP_LOGE(TAG, "Failed to allocate memory for full URL");
    free(url_params);
    esp_http_client_cleanup(client);
    free(resp_buf.buffer);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    return ESP_ERR_NO_MEM;
  }
  snprintf(full_url, full_url_len, "%s?%s", config.url, url_params);
  esp_http_client_set_url(client, full_url);

  const char *json_data = "{\"count\": 0}";
  esp_http_client_set_post_field(client, json_data, strlen(json_data));

  ESP_LOGI(TAG, "Request Body: %s", json_data);

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    free(resp_buf.buffer);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    free(url_params);
    free(full_url);
    return err;
  }

  if (resp_buf.buffer_len < resp_buf.buffer_size) {
    resp_buf.buffer[resp_buf.buffer_len] = '\0';
  } else {
    char *new_buffer = realloc(resp_buf.buffer, resp_buf.buffer_size + 1);
    if (new_buffer == NULL) {
      ESP_LOGE(TAG, "Failed to allocate memory for null terminator");
      esp_http_client_cleanup(client);
      free(resp_buf.buffer);
      free(encoded_loungeIdToken);
      free(encoded_UUID);
      free(encoded_zx);
      free(encoded_name);
      free(zx);
      free(url_params);
      free(full_url);
      return ESP_FAIL;
    }
    resp_buf.buffer = new_buffer;
    resp_buf.buffer[resp_buf.buffer_len] = '\0';
  }

  printf("Response: %s", resp_buf.buffer);

  BindSession_Params_t result = parse_response(resp_buf.buffer);

  char *gsession_item = result.gsessionid;
  char *sid_item = result.sid;
  char *lid_item = result.listId;

  if (gsession_item && sid_item) {
    if (strnlen(gsession_item, sizeof(device->gsession)) >= sizeof(device->gsession) ||
        strnlen(sid_item,     sizeof(device->SID))     >= sizeof(device->SID)     ||
        (lid_item && strnlen(lid_item, sizeof(device->listID)) >= sizeof(device->listID))) {
      ESP_LOGE(TAG, "Bind response field exceeds buffer; aborting bind");
      esp_http_client_cleanup(client);
      free(resp_buf.buffer);
      free(encoded_loungeIdToken);
      free(encoded_UUID);
      free(encoded_zx);
      free(encoded_name);
      free(zx);
      free(url_params);
      free(full_url);
      free(gsession_item);
      free(sid_item);
      free(lid_item);
      return ESP_FAIL;
    }
    snprintf(device->gsession, sizeof(device->gsession), "%s", gsession_item);
    snprintf(device->SID,     sizeof(device->SID),     "%s", sid_item);
    if (lid_item) {
      snprintf(device->listID, sizeof(device->listID), "%s", lid_item);
    }
    ESP_LOGI(TAG, "Session bound successfully.");
  } else {
    ESP_LOGE(TAG, "Failed to bind session ID.");
    esp_http_client_cleanup(client);
    free(resp_buf.buffer);
    free(encoded_loungeIdToken);
    free(encoded_UUID);
    free(encoded_zx);
    free(encoded_name);
    free(zx);
    free(url_params);
    free(full_url);
    free(gsession_item);
    free(sid_item);
    free(lid_item);
    return ESP_FAIL;
  }

  // Clean up
  free(encoded_loungeIdToken);
  free(encoded_UUID);
  free(encoded_zx);
  free(encoded_name);
  free(zx);
  esp_http_client_cleanup(client);
  free(resp_buf.buffer);
  free(url_params);
  free(full_url);
  free(gsession_item);
  free(sid_item);
  free(lid_item);
  return ESP_OK;
}

char *extract_token_from_json(const char *json_response) {
  cJSON *json = cJSON_Parse(json_response);
  if (!json) {
    ESP_LOGE(TAG, "Failed to parse JSON.");
    return NULL;
  }

  cJSON *screens = cJSON_GetObjectItem(json, "screens");
  if (!screens || !cJSON_IsArray(screens)) {
    ESP_LOGE(TAG, "No screens array in response.");
    cJSON_Delete(json);
    return NULL;
  }

  cJSON *first_screen = cJSON_GetArrayItem(screens, 0);
  if (!first_screen) {
    ESP_LOGE(TAG, "No screen element found.");
    cJSON_Delete(json);
    return NULL;
  }

  cJSON *lounge_token = cJSON_GetObjectItem(first_screen, "loungeToken");
  if (!lounge_token || !cJSON_IsString(lounge_token)) {
    ESP_LOGE(TAG, "No loungeToken found.");
    cJSON_Delete(json);
    return NULL;
  }

  char *token = strdup(lounge_token->valuestring);
  cJSON_Delete(json);
  if (!token) {
    ESP_LOGE(TAG, "Failed to allocate memory for token");
  }
  return token;
}

char *get_youtube_token(const char *screen_id) {
  size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  if (free_heap < DIAL_MIN_FREE_HEAP_FOR_HTTPS) {
    ESP_LOGE(TAG, "Insufficient heap for HTTPS: %u bytes free, need %d", free_heap, DIAL_MIN_FREE_HEAP_FOR_HTTPS);
    return NULL;
  }

  response_buffer_t resp_buf = {
      .buffer = malloc(DIAL_RESPONSE_BUFFER_SIZE), .buffer_len = 0, .buffer_size = DIAL_RESPONSE_BUFFER_SIZE};
  if (resp_buf.buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for response buffer");
    return NULL;
  }

  esp_http_client_config_t config = {
      .url =
          "https://www.youtube.com/api/lounge/pairing/get_lounge_token_batch",
      .crt_bundle_attach = esp_crt_bundle_attach,
      .timeout_ms = 10000,
      .method = HTTP_METHOD_POST,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .event_handler = _http_event_handler,
      .user_data = &resp_buf,
      .buffer_size = DIAL_HTTP_BUFFER_SIZE,
      .buffer_size_tx = DIAL_HTTP_BUFFER_SIZE,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client.");
    free(resp_buf.buffer);
    return NULL;
  }

  // Prepare POST data
  char post_data[128];
  snprintf(post_data, sizeof(post_data), "screen_ids=%s", screen_id);

  // Set the POST data
  esp_http_client_set_post_field(client, post_data, strlen(post_data));

  // Set headers
  esp_http_client_set_header(client, "Content-Type",
                             "application/x-www-form-urlencoded");

  // Perform the HTTP request
  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    free(resp_buf.buffer);
    return NULL;
  }

  // Get the HTTP status code
  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP Response Code: %d", status_code);
  if (status_code != 200) {
    ESP_LOGE(TAG, "Failed to retrieve token. HTTP Response Code: %d",
             status_code);
    esp_http_client_cleanup(client);
    free(resp_buf.buffer);
    return NULL;
  }

  if (resp_buf.buffer_len < resp_buf.buffer_size) {
    resp_buf.buffer[resp_buf.buffer_len] = '\0';
  } else {
    // Reallocate buffer to add null terminator
    char *new_buffer = realloc(resp_buf.buffer, resp_buf.buffer_size + 1);
    if (new_buffer == NULL) {
      ESP_LOGE(TAG, "Failed to allocate memory for null terminator");
      esp_http_client_cleanup(client);
      free(resp_buf.buffer);
      return NULL;
    }
    resp_buf.buffer = new_buffer;
    resp_buf.buffer[resp_buf.buffer_len] = '\0';
  }

  // Log the raw API response
  ESP_LOGI(TAG, "YouTube API raw response: %s", resp_buf.buffer);

  // Extract the lounge token from the JSON response
  char *lounge_token = extract_token_from_json(resp_buf.buffer);
  if (lounge_token) {
    ESP_LOGI(TAG, "Successfully retrieved loungeToken: %s", lounge_token);
  } else {
    ESP_LOGE(TAG, "Failed to parse JSON or retrieve loungeToken.");
  }

  // Free the allocated memory
  free(resp_buf.buffer);

  // Clean up the HTTP client
  esp_http_client_cleanup(client);

  // Return the lounge token (or NULL if extraction failed)
  return lounge_token;
}

char *extract_screen_id(const char *xml_data) {
  const char *start_tag = "<screenId>";
  const char *end_tag = "</screenId>";

  char *start = strstr(xml_data, start_tag);
  if (!start) {
    ESP_LOGE("DIALManager", "Start tag <screenId> not found.");
    return NULL;
  }
  start += strlen(start_tag);

  char *end = strstr(start, end_tag);
  if (!end) {
    ESP_LOGE("DIALManager", "End tag </screenId> not found.");
    return NULL;
  }

  size_t screen_id_len = end - start;
  if (screen_id_len == 0) {
    ESP_LOGE("DIALManager", "Extracted screenId is empty.");
    return NULL;
  }

  char *screen_id = malloc(screen_id_len + 1);
  if (screen_id) {
    strncpy(screen_id, start, screen_id_len);
    screen_id[screen_id_len] = '\0';
    ESP_LOGI("DIALManager", "Extracted screenId: %s", screen_id);
  }
  return screen_id;
}

// Initialize DIAL Manager
esp_err_t dial_manager_init(DIALManager *manager, DIALClient *client) {
  if (!manager || !client) {
    return ESP_ERR_INVALID_ARG;
  }
  manager->client = client;
  return ESP_OK;
}

bool fetch_screen_id_with_retries(const char *applicationUrl, Device *device,
                                  DIALManager *manager) {
  const int max_retries = 5;       // Max retries (approx. 15 seconds total)
  const int retry_delay_ms = 3000; // 3 seconds delay between retries

  for (int i = 0; i < max_retries; i++) {

    if (check_app_status(manager, APP_YOUTUBE, applicationUrl, device) ==
            ESP_OK &&
        strlen(device->screenID) > 0) {
      ESP_LOGI(TAG, "Fetched Screen ID: %s", device->screenID);

      char *youtube_token = get_youtube_token(device->screenID);
      if (youtube_token) {
        strncpy(device->YoutubeToken, youtube_token,
                sizeof(device->YoutubeToken) - 1);
        device->YoutubeToken[sizeof(device->YoutubeToken) - 1] = '\0';
        free(youtube_token);
        ESP_LOGI(TAG, "Fetched YouTube Token: %s", device->YoutubeToken);
      } else {
        ESP_LOGE(TAG, "Failed to fetch YouTube token.");
        return false;
      }

      if (bind_session_id(device) == ESP_OK) {
        ESP_LOGI(TAG, "Session successfully bound.");
        return true;
      } else {
        ESP_LOGE(TAG, "Failed to bind session ID.");
        return false;
      }
    } else {
      ESP_LOGW(TAG, "Screen ID is empty. Retrying... (%d/%d)", i + 1,
               max_retries);
      vTaskDelay(retry_delay_ms / portTICK_PERIOD_MS);
    }
  }

  ESP_LOGE(TAG, "Failed to fetch Screen ID after max retries.");
  return false;
}

char *g_app_url = NULL;
char *g_friendly_name = NULL;

static char *extract_friendly_name(const char *xml) {
  const char *start = strstr(xml, "<friendlyName>");
  if (!start) return NULL;
  start += 14;
  const char *end = strstr(start, "</friendlyName>");
  if (!end) return NULL;
  size_t len = end - start;
  char *name = malloc(len + 1);
  if (name) {
    memcpy(name, start, len);
    name[len] = '\0';
  }
  return name;
}

esp_err_t _http_event_header_handler(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "Header: %s: %s", evt->header_key, evt->header_value);
    if (strcasecmp(evt->header_key, "Application-Url") == 0) {
      if (evt->header_value != NULL) {
        free(g_app_url);
        g_app_url = strdup(evt->header_value);
      }
    }
    break;
  default:
    break;
  }
  return ESP_OK;
}

char *get_dial_application_url(const char *location_url) {
  char ip[64];
  uint16_t port = 0;

  if (extract_ip_and_port(location_url, ip, &port) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to extract IP and port from URL");
    return NULL;
  }

  char *path = extract_path_from_url(location_url);
  if (!path) {
    ESP_LOGE(TAG, "Failed to extract path from URL");
    return NULL;
  }

  ESP_LOGI(TAG, "Connecting to IP: %s, Port: %u, Path: %s", ip, port, path);

  esp_http_client_config_t config = {
      .host = ip,
      .port = port,
      .path = path,
      .timeout_ms = 5000,
      .event_handler = _http_event_header_handler, // Set the event handler
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    free(path);
    return NULL;
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    free(path);
    return NULL;
  }

  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP Status Code: %d", status_code);
  if (status_code != 200) {
    ESP_LOGE(TAG, "Failed to fetch device description. HTTP Status Code: %d",
             status_code);
    esp_http_client_cleanup(client);
    free(path);
    return NULL;
  }

  int content_len = esp_http_client_get_content_length(client);
  if (content_len > 0 && content_len < 4096) {
    char *body = malloc(content_len + 1);
    if (body) {
      int read_len = esp_http_client_read(client, body, content_len);
      if (read_len > 0) {
        body[read_len] = '\0';
        if (g_friendly_name) free(g_friendly_name);
        g_friendly_name = extract_friendly_name(body);
      }
      free(body);
    }
  }

  if (g_app_url != NULL) {
    ESP_LOGI(TAG, "Application-Url: %s", g_app_url);
    char *app_url_copy = strdup(g_app_url);
    free(g_app_url);
    g_app_url = NULL;
    esp_http_client_cleanup(client);
    free(path);
    if (!app_url_copy) {
      ESP_LOGE(TAG, "Failed to allocate memory for app URL copy");
      return NULL;
    }
    return app_url_copy;
  } else {
    ESP_LOGE(TAG, "Couldn't find 'Application-Url' in the headers.");
  }

  esp_http_client_cleanup(client);
  free(path);
  return NULL;
}

// Helper to extract IP and port from URL
esp_err_t extract_ip_and_port(const char *url, char *ip_out,
                              uint16_t *port_out) {
  const char *ip_start = strstr(url, "http://");
  if (!ip_start) {
    return ESP_ERR_INVALID_ARG;
  }
  ip_start += strlen("http://");

  const char *port_start = strchr(ip_start, ':');
  const char *path_start = strchr(ip_start, '/');
  if (!port_start || !path_start || port_start > path_start) {
    return ESP_ERR_INVALID_ARG;
  }

  // Extract the IP and port
  size_t ip_len = port_start - ip_start;
  strncpy(ip_out, ip_start, ip_len);
  ip_out[ip_len] = '\0';

  *port_out = atoi(port_start + 1);

  return ESP_OK;
}

// Helper to get the correct path for the app
const char *get_app_path(DIALAppType app) {
  switch (app) {
  case APP_YOUTUBE:
    return "/YouTube";
  case APP_NETFLIX:
    return "/Netflix";
  default:
    return "/";
  }
}

char *remove_ip_and_port(const char *url) {
  const char *path_start = strchr(url, '/');
  if (path_start) {
    if (strncmp(path_start, "//", 2) == 0) {
      path_start = strchr(path_start + 2, '/');
    }
  }

  if (path_start) {
    return strdup(path_start);
  }

  return NULL;
}

// Check the app status by communicating with the device
esp_err_t check_app_status(DIALManager *manager, DIALAppType app,
                           const char *appUrl, Device *device) {
  if (!manager || !appUrl || !device) {
    return ESP_ERR_INVALID_ARG;
  }

  char ip[64];
  uint16_t port = 0;
  if (extract_ip_and_port(appUrl, ip, &port) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to extract IP and port from URL");
    return ESP_ERR_INVALID_ARG;
  }

  const char *app_path = get_app_path(app);
  char full_path[256];
  snprintf(full_path, sizeof(full_path), "%s%s", appUrl, app_path);

  ESP_LOGI(TAG, "Connecting to IP: %s, Port: %u, Path: %s", ip, port, app_path);

  char *path = remove_ip_and_port(full_path);

  esp_http_client_config_t config = {
      .host = ip, .port = port, .path = path, .timeout_ms = 5000};
  esp_http_client_handle_t http_client = esp_http_client_init(&config);
  if (http_client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    free(path);
    return ESP_ERR_NO_MEM;
  }

  esp_http_client_set_header(http_client, "Origin", "https://www.youtube.com");
  esp_http_client_set_header(
      http_client, "User-Agent",
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/96.0.4664.45 Safari/537.36");
  esp_http_client_set_header(http_client, "Content-Type",
                             "application/x-www-form-urlencoded");

  // Open the connection manually
  esp_err_t err =
      esp_http_client_open(http_client, 0); // 0 means no request body
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    esp_http_client_cleanup(http_client);
    free(path);
    return err;
  }

  // Check if we got any headers back
  int status_code = esp_http_client_fetch_headers(http_client);
  if (status_code < 0) {
    ESP_LOGE(TAG, "Failed to fetch HTTP headers");
    esp_http_client_cleanup(http_client);
    free(path);
    return ESP_FAIL;
  }

  // Log HTTP status code
  status_code = esp_http_client_get_status_code(http_client);
  ESP_LOGI(TAG, "HTTP status code: %d", status_code);

  if (status_code == 200) {
    char *response_body = malloc(DIAL_RESPONSE_BUFFER_SIZE);
    if(!response_body){
      ESP_LOGE(TAG, "malloc failed");
      esp_http_client_cleanup(http_client);
      free(path);
      return ESP_ERR_NO_MEM;
    }
    int content_len = esp_http_client_read(http_client, response_body, DIAL_RESPONSE_BUFFER_SIZE - 1);
    if (content_len >= 0) {
      response_body[content_len] = '\0'; // Null-terminate the response body

      ESP_LOGI(TAG, "Response Body:\n%s", response_body);

      // Check if app is running
      if (strstr(response_body, "<state>running</state>")) {
        ESP_LOGI("DIALManager", "%s app is running",
                 (app == APP_YOUTUBE) ? "YouTube" : "Netflix");

        // Extract screenId from the response
        char *screen_id = extract_screen_id(response_body);
        if (screen_id) {
          strncpy(device->screenID, screen_id,
                  sizeof(device->screenID) - 1); // Store in device
          free(screen_id);
          free(response_body);
          esp_http_client_cleanup(http_client);
          free(path);
          return ESP_OK;
        }
        free(response_body);
        esp_http_client_cleanup(http_client);
        free(path);
        return ESP_FAIL;
      } else {
        ESP_LOGW("DIALManager", "%s app is not running",
                 (app == APP_YOUTUBE) ? "YouTube" : "Netflix");
        free(response_body);
        esp_http_client_cleanup(http_client);
        free(path);
        return ESP_ERR_NOT_FOUND;
      }
    } else {
      ESP_LOGE(TAG, "Failed to read HTTP response body");
      free(response_body);
      esp_http_client_cleanup(http_client);
      free(path);
      return ESP_FAIL;
    }
  } else {
    ESP_LOGE("DIALManager", "Unexpected HTTP status code: %d", status_code);
    esp_http_client_cleanup(http_client);
    free(path);
    return ESP_FAIL;
  }
}

bool launch_app(DIALManager *manager, DIALAppType app, const char *appUrl) {
  if (!manager || !appUrl) {
    return false;
  }

  char ip[64];
  uint16_t port = 0;
  if (extract_ip_and_port(appUrl, ip, &port) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to extract IP and port from URL");
    return false;
  }

  const char *app_path = get_app_path(app);
  char full_path[256];
  snprintf(full_path, sizeof(full_path), "%s%s", appUrl, app_path);

  ESP_LOGI(TAG, "Launching app: %s at IP: %s, Port: %u, Path: %s",
           (app == APP_YOUTUBE) ? "YouTube" : "Netflix", ip, port, full_path);

  char *path = remove_ip_and_port(full_path);

  esp_http_client_config_t config = {
      .host = ip,
      .port = port,
      .path = path,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t http_client = esp_http_client_init(&config);
  if (http_client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    free(path);
    return false;
  }

  esp_http_client_set_header(http_client, "Origin", "https://www.youtube.com");

  esp_err_t err = esp_http_client_perform(http_client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(http_client);
    if (status_code == 201 || status_code == 200) {
      ESP_LOGI(TAG, "Successfully launched the app: %s",
               (app == APP_YOUTUBE) ? "YouTube" : "Netflix");
      esp_http_client_cleanup(http_client);
      free(path);
      return true;
    } else {
      ESP_LOGE(TAG, "Failed to launch the app. HTTP Response Code: %d",
               status_code);
    }
  } else {
    ESP_LOGE(TAG, "Failed to send the launch request. Error: %s",
             esp_err_to_name(err));
  }

  esp_http_client_cleanup(http_client);
  free(path);
  return false;
}

static bool test_tcp_connect(const char *ip, uint16_t port) {
  struct sockaddr_in dest_addr;
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(port);
  inet_pton(AF_INET, ip, &dest_addr.sin_addr);

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock < 0) {
    printf("    [-] Socket create failed\n");
    return false;
  }

  struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  close(sock);

  if (err != 0) {
    printf("    [-] TCP connect to %s:%u failed: %d\n", ip, port, errno);
    return false;
  }
  printf("    [+] TCP connect to %s:%u OK\n", ip, port);
  return true;
}

bool launch_youtube_with_video(DIALManager *manager, const char *appUrl, const char *video_id) {
  if (!manager || !appUrl || !video_id) {
    return false;
  }

  char ip[64];
  uint16_t port = 0;
  if (extract_ip_and_port(appUrl, ip, &port) != ESP_OK) {
    printf("    [-] Failed to parse URL\n");
    return false;
  }

  if (!test_tcp_connect(ip, port)) {
    return false;
  }

  char url[256];
  snprintf(url, sizeof(url), "%s/YouTube", appUrl);

  char post_body[128];
  snprintf(post_body, sizeof(post_body), "v=%s", video_id);

  printf("    [*] POST %s\n", url);

  esp_http_client_config_t config = {
      .url = url,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 10000,
      .transport_type = HTTP_TRANSPORT_OVER_TCP,
  };
  esp_http_client_handle_t http_client = esp_http_client_init(&config);
  if (!http_client) {
    ESP_LOGE(TAG, "Failed to init HTTP client");
    return false;
  }

  esp_http_client_set_header(http_client, "Content-Type", "text/plain; charset=utf-8");
  esp_http_client_set_header(http_client, "Origin", "https://www.youtube.com");
  esp_http_client_set_post_field(http_client, post_body, strlen(post_body));

  esp_err_t err = esp_http_client_perform(http_client);
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(http_client);
    printf("    [*] Response: %d\n", status_code);
    if (status_code == 201 || status_code == 200) {
      ESP_LOGI(TAG, "Successfully launched YouTube with video: %s", video_id);
      esp_http_client_cleanup(http_client);
      return true;
    } else {
      ESP_LOGE(TAG, "Failed to launch YouTube. HTTP Response Code: %d", status_code);
    }
  } else {
    ESP_LOGE(TAG, "Failed to send launch request. Error: %s", esp_err_to_name(err));
  }

  esp_http_client_cleanup(http_client);
  return false;
}

const char *pick_random_yt_video() {
  const char *yt_urls[] = {
      "dQw4w9WgXcQ", // Rick Astley - Never Gonna Give You Up
      "oHg5SJYRHA0", // RickRoll'D
      "xvFZjo5PgG0", // Stick Bug
      "djV11Xbc914", // A-ha - Take On Me
      "fC7oUOUEEi4", // Toto - Africa
      "y6120QOlsfU", // Darude - Sandstorm
      "kJQP7kiw5Fk", // Luis Fonsi - Despacito
      "9bZkp7q19f0", // PSY - Gangnam Style
      "3JZ_D3ELwOQ", // Michael Jackson - Smooth Criminal
      "QH2-TGUlwu4", // Nyan Cat
      "wZZ7oFKsKzY", // He-Man HEYYEYAAEYAAAEYAEYAA
      "L_jWHffIx5E", // Smash Mouth - All Star
      "hTWKbfoikeg", // Nirvana - Smells Like Teen Spirit
      "btPJPFnesV4", // Eye of the Tiger
      "fJ9rUzIMcZQ", // Queen - Bohemian Rhapsody
      "YQHsXMglC9A", // Adele - Hello
      "CevxZvSJLk8", // Katy Perry - Roar
      "JGwWNGJdvx8", // Ed Sheeran - Shape of You
      "RgKAFK5djSk", // Wiz Khalifa - See You Again
      "09R8_2nJtjg", // Maroon 5 - Sugar
      "lp-EO5I60KA", // Eminem - Lose Yourself
      "hT_nvWreIhg", // OneRepublic - Counting Stars
      "OPf0YbXqDm0", // Mark Ronson - Uptown Funk
      "pRpeEdMmmQ0", // Shakira - Waka Waka
      "2Vv-BfVoq4g", // Perfect - Ed Sheeran
      "60ItHLz5WEA", // Alan Walker - Faded
      "Zi_XLOBDo_Y", // Michael Jackson - Billie Jean
  };

  int num_videos = sizeof(yt_urls) / sizeof(yt_urls[0]);
  return yt_urls[esp_random() % num_videos];
}

typedef struct {
  char ip[64];
  uint16_t port;
  char *app_url;
  char *friendly_name;
} dial_target_t;

void explore_network(DIALManager *manager, bool cast_all) {
  printf("\n[*] Starting DIAL discovery%s...\n", cast_all ? " (all devices)" : "");

  Device *devices = (Device *)malloc(sizeof(Device) * 10);
  if (!devices) {
    printf("    [-] Memory allocation failed\n");
    return;
  }
  size_t device_count = 0;

  printf("    Discovering DIAL-enabled devices...\n");
  if (dial_client_discover_devices(manager->client, devices, 10,
                                   &device_count) != ESP_OK ||
      device_count == 0) {
    printf("    [-] No devices found\n");
  }

  if (device_count == 0) {
    printf("\n[-] No DIAL devices found\n\n");
    free(devices);
    return;
  }

  printf("    [+] Found %zu device(s)!\n", device_count);

  dial_target_t *targets = calloc(device_count, sizeof(dial_target_t));
  if (!targets) {
    printf("    [-] Memory allocation failed\n");
    free(devices);
    return;
  }

  esp_netif_ip_info_t sta_ip_info;
  esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  bool has_sta_ip = (sta_netif && esp_netif_get_ip_info(sta_netif, &sta_ip_info) == ESP_OK &&
                     sta_ip_info.ip.addr != 0);
  
  char sta_subnet[16] = {0};
  if (has_sta_ip) {
    uint32_t subnet = sta_ip_info.ip.addr & sta_ip_info.netmask.addr;
    snprintf(sta_subnet, sizeof(sta_subnet), "%lu.%lu.%lu.", 
             (subnet) & 0xFF, (subnet >> 8) & 0xFF, (subnet >> 16) & 0xFF);
    printf("[*] STA subnet: %s0/24, switching to STA-only for routing\n", sta_subnet);
    esp_wifi_set_mode(WIFI_MODE_STA);
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }

  size_t valid_targets = 0;

  printf("[*] Fetching Application-URLs...\n");
  for (size_t i = 0; i < device_count; ++i) {
    Device *device = &devices[i];

    if (extract_ip_and_port(device->location, targets[valid_targets].ip, 
                            &targets[valid_targets].port) != ESP_OK) {
      continue;
    }

    targets[valid_targets].app_url = get_dial_application_url(device->location);
    targets[valid_targets].friendly_name = g_friendly_name ? strdup(g_friendly_name) : NULL;
    if (g_friendly_name) { free(g_friendly_name); g_friendly_name = NULL; }

    if (targets[valid_targets].app_url) {
      size_t len = strlen(targets[valid_targets].app_url);
      if (len > 0 && targets[valid_targets].app_url[len-1] == '/') {
        targets[valid_targets].app_url[len-1] = '\0';
      }
    } else {
      targets[valid_targets].app_url = malloc(128);
      if (!targets[valid_targets].app_url) {
        free(targets[valid_targets].friendly_name);
        continue;
      }
      snprintf(targets[valid_targets].app_url, 128, "http://%s:%u/apps", 
               targets[valid_targets].ip, targets[valid_targets].port);
    }

    bool duplicate = false;
    for (size_t j = 0; j < valid_targets; ++j) {
      if (strcmp(targets[j].app_url, targets[valid_targets].app_url) == 0) {
        duplicate = true;
        free(targets[valid_targets].app_url);
        free(targets[valid_targets].friendly_name);
        targets[valid_targets].app_url = NULL;
        targets[valid_targets].friendly_name = NULL;
        break;
      }
    }
    if (duplicate) continue;

    const char *name = targets[valid_targets].friendly_name ? targets[valid_targets].friendly_name : "Unknown";
    printf("    [+] %s (%s)\n", name, targets[valid_targets].app_url);
    valid_targets++;
  }

  free(devices);

  if (valid_targets == 0) {
    printf("\n[-] No valid DIAL targets\n\n");
    free(targets);
    return;
  }

  size_t success_count = 0;
  for (size_t i = 0; i < valid_targets; ++i) {
    const char *video_id = pick_random_yt_video();
    const char *name = targets[i].friendly_name ? targets[i].friendly_name : targets[i].ip;
    printf("    [*] Launching on %s (video: %s)\n", name, video_id);

    if (launch_youtube_with_video(manager, targets[i].app_url, video_id)) {
      printf("    [+] Success!\n");
      success_count++;
      if (!cast_all) break;
    } else {
      printf("    [-] Failed\n");
    }
  }

  for (size_t i = 0; i < valid_targets; ++i) {
    free(targets[i].app_url);
    free(targets[i].friendly_name);
  }
  free(targets);

  if (success_count > 0) {
    printf("\n[+] DIAL cast complete! (%zu/%zu devices)\n\n", success_count, valid_targets);
  } else {
    printf("\n[-] DIAL cast failed\n\n");
  }
}

#pragma GCC diagnostic pop
