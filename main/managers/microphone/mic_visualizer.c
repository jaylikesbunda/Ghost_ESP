/*
 * MIC Visualizer
 * 
 * RGB visualization modes inspired by Sensory Bridge
 * by Connor Nishijima (https://github.com/connornishijima/SensoryBridge)
 * 
 * Licensed under GPL-3.0 (same as Sensory Bridge)
 */

#include "managers/microphone/mic_visualizer.h"

// Only compile MIC visualizer when MIC hardware is enabled
#ifdef CONFIG_HAS_MIC

#include "managers/microphone/mic_driver.h"
#ifndef CONFIG_MIC_I2S_PORT
#define CONFIG_MIC_I2S_PORT 0
#endif
#ifndef CONFIG_MIC_I2S_MCLK_PIN
#define CONFIG_MIC_I2S_MCLK_PIN -1
#endif
#include "managers/microphone/mic_goertzel.h"
#include "core/esp_comm_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>

static const char *TAG = "MIC_Visualizer";

static TaskHandle_t mic_visualizer_task_handle = NULL;
static bool mic_visualizer_running = false;
static bool mic_initialized = false;

// Amplitude smoothing (asymmetric like sensory_bridge)
static float amplitude_follower = 0.0f;
#define AMP_ATTACK_RATE 0.25f
#define AMP_RELEASE_RATE 0.02f

// Wire payload: [bass, low_mid, high_mid, treble, amplitude]
#define MIC_WIRE_PAYLOAD_LEN 5

static void mic_visualizer_task(void *arg) {
    int32_t *samples = NULL;

    size_t buffer_size = (CONFIG_MIC_BUFFER_SAMPLES * 2) * sizeof(int32_t);
    // Allocate sample buffer from PSRAM to save internal RAM (retry, never internal)
    samples = (int32_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!samples) {
        ESP_LOGW(TAG, "PSRAM sample buffer alloc failed, retrying");
        samples = (int32_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (samples == NULL) {
        ESP_LOGE(TAG, "Failed to allocate sample buffer");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    uint32_t loop_counter = 0;
    bool was_connected = false;
    
    ESP_LOGI(TAG, "MIC visualizer task started");
    
    while (mic_visualizer_running) {
        if (mic_is_paused()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        size_t bytes_read = 0;
        
        esp_err_t ret = mic_read_samples(samples, CONFIG_MIC_BUFFER_SAMPLES, &bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read samples: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        size_t samples_read = bytes_read / sizeof(int32_t);
        if (samples_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        
        // Run Goertzel frequency analysis on the samples
        goertzel_process(samples, samples_read);
        
        // Get overall amplitude from driver
        float amplitude = mic_calculate_rms(samples, samples_read);
        
        // Asymmetric smoothing on the output (fast attack, slow release)
        if (amplitude > amplitude_follower) {
            float delta = amplitude - amplitude_follower;
            amplitude_follower += delta * AMP_ATTACK_RATE;
        } else {
            float delta = amplitude_follower - amplitude;
            amplitude_follower -= delta * AMP_RELEASE_RATE;
        }
        
        // Build wire payload: [bass, low_mid, high_mid, treble, amplitude]
        uint8_t payload[MIC_WIRE_PAYLOAD_LEN];
        payload[0] = goertzel_get_band(GOERTZEL_BAND_BASS);
        payload[1] = goertzel_get_band(GOERTZEL_BAND_LOW_MID);
        payload[2] = goertzel_get_band(GOERTZEL_BAND_HIGH_MID);
        payload[3] = goertzel_get_band(GOERTZEL_BAND_TREBLE);
        payload[4] = (uint8_t)(amplitude_follower * 255.0f);
        
        // Log periodically
        if (loop_counter % 500 == 0) {
            bool cal = !goertzel_is_calibrated();
            if (cal) {
                ESP_LOGI(TAG, "Calibrating... B=%d L=%d H=%d T=%d",
                         payload[0], payload[1], payload[2], payload[3]);
            } else {
                ESP_LOGD(TAG, "B=%d L=%d H=%d T=%d amp=%d",
                         payload[0], payload[1], payload[2], payload[3], payload[4]);
            }
        }
        
        // Send via GhostLink if connected
        bool is_connected = esp_comm_manager_is_connected();
        if (is_connected) {
            bool send_ok = esp_comm_manager_send_stream(COMM_STREAM_CHANNEL_MIC_AMPLITUDE, 
                                         payload, MIC_WIRE_PAYLOAD_LEN);
            if (!send_ok) {
                ESP_LOGW(TAG, "Failed to send MIC data (UART/busy?)");
            } else if (!was_connected) {
                ESP_LOGI(TAG, "GhostLink connected - sending MIC data");
            }
            
            if (loop_counter % 1000 == 0) {
                ESP_LOGD(TAG, "Sending B=%d L=%d H=%d T=%d A=%d (ok=%d)",
                         payload[0], payload[1], payload[2], payload[3], payload[4], send_ok);
            }
        } else if (was_connected) {
            ESP_LOGW(TAG, "GhostLink disconnected - MIC data not sending");
        }
        was_connected = is_connected;
        loop_counter++;
        
        // Target ~33 FPS (30ms delay)
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    
    ESP_LOGI(TAG, "MIC visualizer task stopped");
    
    if (samples) {
        free(samples);
    }
    
    mic_visualizer_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t mic_visualizer_init(void) {
    if (mic_initialized) {
        ESP_LOGW(TAG, "MIC visualizer already initialized");
        return ESP_OK;
    }
    
    // Initialize microphone
    mic_config_t cfg = {
        .bclk_pin = (gpio_num_t)CONFIG_MIC_I2S_BCLK_PIN,
        .ws_pin = (gpio_num_t)CONFIG_MIC_I2S_WS_PIN,
        .din_pin = (gpio_num_t)CONFIG_MIC_I2S_DIN_PIN,
        .mclk_pin = (gpio_num_t)CONFIG_MIC_I2S_MCLK_PIN,
        .i2s_port = CONFIG_MIC_I2S_PORT,
        .sample_rate = CONFIG_MIC_SAMPLE_RATE,
        .buffer_samples = CONFIG_MIC_BUFFER_SAMPLES
    };
    
    esp_err_t ret = mic_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize microphone: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Initialize Goertzel frequency analyzer
    ret = goertzel_init(CONFIG_MIC_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Goertzel: %s", esp_err_to_name(ret));
        return ret;
    }
    
    mic_initialized = true;
    amplitude_follower = 0.0f;
    ESP_LOGI(TAG, "MIC visualizer initialized (with 4-band Goertzel)");
    
    return ESP_OK;
}

esp_err_t mic_visualizer_start(void) {
    if (!mic_initialized) {
        esp_err_t init_ret = mic_visualizer_init();
        if (init_ret != ESP_OK) {
            ESP_LOGE(TAG, "MIC visualizer lazy init failed");
            return init_ret;
        }
    }
    
    if (mic_visualizer_running) {
        ESP_LOGW(TAG, "MIC visualizer already running");
        return ESP_OK;
    }
    
    mic_visualizer_running = true;

    const uint32_t stack_size = 8192;
    BaseType_t ret = xTaskCreateWithCaps(mic_visualizer_task, "mic_visualizer", stack_size, NULL, 5, &mic_visualizer_task_handle,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ret = xTaskCreateWithCaps(mic_visualizer_task, "mic_visualizer", stack_size, NULL, 5, &mic_visualizer_task_handle,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ret != pdPASS) {
        mic_visualizer_running = false;
        ESP_LOGE(TAG, "Failed to create MIC visualizer task");
        return ESP_ERR_NO_MEM;
    }
    
    ESP_LOGI(TAG, "MIC visualizer started");
    return ESP_OK;
}

esp_err_t mic_visualizer_stop(void) {
    if (!mic_visualizer_running) {
        return ESP_OK;
    }
    
    mic_visualizer_running = false;
    
    int timeout = 50;
    while (mic_visualizer_task_handle != NULL && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
        timeout--;
    }
    
    if (mic_visualizer_task_handle != NULL) {
        vTaskDeleteWithCaps(mic_visualizer_task_handle);
        mic_visualizer_task_handle = NULL;
    }
    
    ESP_LOGI(TAG, "MIC visualizer stopped");
    return ESP_OK;
}

bool mic_visualizer_is_running(void) {
    return mic_visualizer_running;
}

esp_err_t mic_visualizer_deinit(void) {
    mic_visualizer_stop();
    
    if (mic_initialized) {
        mic_deinit();
        mic_initialized = false;
    }
    
    ESP_LOGI(TAG, "MIC visualizer deinitialized");
    return ESP_OK;
}

#endif // CONFIG_HAS_MIC

// Stub implementations when MIC is not enabled
#ifndef CONFIG_HAS_MIC

esp_err_t mic_visualizer_init(void) {
    return ESP_OK;
}

esp_err_t mic_visualizer_start(void) {
    return ESP_OK;
}

esp_err_t mic_visualizer_stop(void) {
    return ESP_OK;
}

bool mic_visualizer_is_running(void) {
    return false;
}

esp_err_t mic_visualizer_deinit(void) {
    return ESP_OK;
}

#endif // !CONFIG_HAS_MIC
