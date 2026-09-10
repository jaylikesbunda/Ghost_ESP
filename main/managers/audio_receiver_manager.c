#include "managers/audio_receiver_manager.h"

#if defined(CONFIG_HAS_TLV320DAC_I2S) || defined(CONFIG_HAS_AW88298_SPEAKER) || defined(CONFIG_HAS_CROWPANEL_NS4168)

#include "managers/audio_i2s_output.h"
#include "core/esp_comm_manager.h"
#include "managers/audio_stream_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* esp_audio_codec headers */
#include "esp_audio_dec.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_mp3_dec.h"

static const char *TAG = "AudioRecv";

#define AUDIO_RX_RINGBUF_SIZE  (96 * 1024)   /* 96KB ring buffer — absorbs C5 SPI gaps */
#define AUDIO_DEC_TASK_STACK   (8192)
#define AUDIO_DEC_TASK_PRIO    (23)
#define AUDIO_PCM_BUF_SIZE     (8192)
#define AUDIO_DEC_IN_BUF_SIZE  (8192)
#define AUDIO_DEC_MIN_INPUT    (1024)
#define AUDIO_DEC_START_THRESHOLD (48 * 1024)  /* Wait until buffer is 50% full before decoding */
#define AUDIO_STATUS_INTERVAL_MS 250

typedef struct {
    uint8_t *buf;
    volatile uint32_t head;
    volatile uint32_t tail;
} ringbuf_t;

static portMUX_TYPE s_rx_ringbuf_mux = portMUX_INITIALIZER_UNLOCKED;

static struct {
    bool initialized;
    bool active;
    bool flush_requested;
    bool waiting_for_buffer;
    bool stop_requested;
    SemaphoreHandle_t decode_done_sem;
    esp_audio_simple_dec_handle_t simple_dec;
    TaskHandle_t decode_task;
    ringbuf_t rx_ringbuf;
    uint32_t detected_sample_rate;
    bool first_stream_packet_logged;
    bool first_pcm_logged;
    uint32_t rx_bytes_total;
    TickType_t last_status_tick;
    uint32_t played_ms;
    uint64_t played_pcm_bytes;
    uint32_t output_channels;
} s_recv = {0};

static StackType_t *s_decode_task_stack = NULL;
static StaticTask_t *s_decode_task_tcb = NULL;

static inline size_t ringbuf_count(ringbuf_t *rb)
{
    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    size_t ret;
    if (head >= tail) ret = head - tail;
    else ret = AUDIO_RX_RINGBUF_SIZE - (tail - head);
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    return ret;
}

static size_t ringbuf_write(ringbuf_t *rb, const uint8_t *data, size_t len)
{
    size_t count = ringbuf_count(rb);
    /* Keep one byte empty so head == tail always means empty, never full. */
    size_t free = (count < AUDIO_RX_RINGBUF_SIZE - 1) ? (AUDIO_RX_RINGBUF_SIZE - count - 1) : 0;
    size_t to_write = (len < free) ? len : free;
    if (to_write == 0 || !rb->buf) return 0;

    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    uint32_t head = rb->head;
    for (size_t i = 0; i < to_write; i++) {
        rb->buf[head] = data[i];
        head = (head + 1) % AUDIO_RX_RINGBUF_SIZE;
    }
    rb->head = head;
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    return to_write;
}

static size_t ringbuf_read(ringbuf_t *rb, uint8_t *data, size_t len)
{
    size_t count = ringbuf_count(rb);
    size_t to_read = (len < count) ? len : count;
    if (to_read == 0 || !rb->buf) return 0;

    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    uint32_t tail = rb->tail;
    for (size_t i = 0; i < to_read; i++) {
        data[i] = rb->buf[tail];
        tail = (tail + 1) % AUDIO_RX_RINGBUF_SIZE;
    }
    rb->tail = tail;
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    return to_read;
}

static size_t ringbuf_peek(ringbuf_t *rb, uint8_t *data, size_t len)
{
    size_t count = ringbuf_count(rb);
    size_t to_peek = (len < count) ? len : count;
    if (to_peek == 0 || !rb->buf) return 0;

    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    uint32_t tail = rb->tail;
    for (size_t i = 0; i < to_peek; i++) {
        data[i] = rb->buf[tail];
        tail = (tail + 1) % AUDIO_RX_RINGBUF_SIZE;
    }
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    return to_peek;
}

esp_err_t audio_receiver_manager_start(void)
{
    if (!s_recv.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_recv.active = true;
    s_recv.flush_requested = true;
    s_recv.waiting_for_buffer = true;
    s_recv.first_stream_packet_logged = false;
    s_recv.first_pcm_logged = false;
    s_recv.detected_sample_rate = 0;
    s_recv.rx_bytes_total = 0;
    s_recv.last_status_tick = 0;
    s_recv.played_ms = 0;
    s_recv.played_pcm_bytes = 0;
    s_recv.output_channels = 2;
    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    s_recv.rx_ringbuf.head = 0;
    s_recv.rx_ringbuf.tail = 0;
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    ESP_LOGI(TAG, "Audio receiver started");
    return ESP_OK;
}

void audio_receiver_manager_stop(void)
{
    if (!s_recv.initialized) return;
    s_recv.active = false;
    s_recv.waiting_for_buffer = false;
    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    s_recv.rx_ringbuf.head = 0;
    s_recv.rx_ringbuf.tail = 0;
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);
    ESP_LOGI(TAG, "Audio receiver stopped");
}

void audio_receiver_manager_pause(void)
{
    if (!s_recv.initialized) return;
    /* Stop the decode task, but DO NOT clear the ring buffer or the decoder
     * state: the sender freezes its stream offset at the exact byte count it
     * wrote into this buffer, so everything still buffered here is what a
     * resume must continue from. Clearing it was causing resume to skip ahead
     * by up to a prebuffer's worth of audio. */
    s_recv.active = false;
    /* Silence the I2S DMA tail so no audio leaks after pause. */
    audio_i2s_output_flush();
    ESP_LOGI(TAG, "Audio receiver paused");
}

void audio_receiver_manager_resume(void)
{
    if (!s_recv.initialized) return;
    /* Continue decoding exactly where pause left off: keep the ring buffer
     * contents and decoder state, and skip the prebuffer wait because the
     * decoder either still has data or is about to be refilled by the sender
     * continuing from its preserved stream offset. */
    s_recv.active = true;
    s_recv.flush_requested = false;
    s_recv.waiting_for_buffer = false;
    ESP_LOGI(TAG, "Audio receiver resumed");
}

void audio_receiver_manager_flush(void)
{
    if (!s_recv.initialized) return;
    s_recv.flush_requested = true;
    s_recv.rx_ringbuf.head = 0;
    s_recv.rx_ringbuf.tail = 0;
    s_recv.waiting_for_buffer = true;
    ESP_LOGI(TAG, "Audio receiver flush requested");
}

static void stream_handler(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static void audio_decode_task(void *arg);

static uint32_t audio_receiver_get_played_ms(void)
{
    uint32_t sample_rate = s_recv.detected_sample_rate ? s_recv.detected_sample_rate : 44100;
    uint32_t channels = s_recv.output_channels ? s_recv.output_channels : 2;
    uint32_t bytes_per_sec = sample_rate * channels * sizeof(int16_t);
    if (bytes_per_sec == 0) return 0;
    return (uint32_t)((s_recv.played_pcm_bytes * 1000) / bytes_per_sec);
}

static void audio_receiver_send_status(bool force)
{
    if (!esp_comm_manager_is_connected()) return;

    TickType_t now = xTaskGetTickCount();
    if (!force && s_recv.last_status_tick != 0 &&
        now - s_recv.last_status_tick < pdMS_TO_TICKS(AUDIO_STATUS_INTERVAL_MS)) {
        return;
    }

    char status[48];
    snprintf(status, sizeof(status), "state %lu %lu %lu",
             (unsigned long)ringbuf_count(&s_recv.rx_ringbuf),
             (unsigned long)AUDIO_RX_RINGBUF_SIZE,
             (unsigned long)audio_receiver_get_played_ms());
    if (esp_comm_manager_send_command("audio", status)) {
        s_recv.last_status_tick = now;
    }
}

size_t audio_receiver_manager_write_stream(const uint8_t *data, size_t len)
{
    if (!s_recv.initialized || !s_recv.active || !data || len == 0) {
        return 0;
    }

    size_t written = ringbuf_write(&s_recv.rx_ringbuf, data, len);
    if (written > 0) {
        /* Mirror the same feedback the peer status command would send, so
         * local playback progress/buffer UI keeps working without GhostLink. */
        audio_stream_manager_update_receiver_status(ringbuf_count(&s_recv.rx_ringbuf),
                                                    AUDIO_RX_RINGBUF_SIZE,
                                                    audio_receiver_get_played_ms());
    }
    return written;
}

esp_err_t audio_receiver_manager_init(void)
{
    if (s_recv.initialized) {
        return ESP_OK;
    }

    memset(&s_recv, 0, sizeof(s_recv));
    taskENTER_CRITICAL(&s_rx_ringbuf_mux);
    s_recv.rx_ringbuf.head = 0;
    s_recv.rx_ringbuf.tail = 0;
    taskEXIT_CRITICAL(&s_rx_ringbuf_mux);

    /* Decoder task uses this binary semaphore to signal a clean self-exit. */
    if (s_recv.decode_done_sem == NULL) {
        s_recv.decode_done_sem = xSemaphoreCreateBinary();
    }

    /* Allocate ring buffer from PSRAM */
    s_recv.rx_ringbuf.buf = (uint8_t *)heap_caps_malloc(AUDIO_RX_RINGBUF_SIZE,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_recv.rx_ringbuf.buf) {
        ESP_LOGE(TAG, "Failed to allocate %lu byte ring buffer", (unsigned long)AUDIO_RX_RINGBUF_SIZE);
        if (s_recv.decode_done_sem) {
            vSemaphoreDelete(s_recv.decode_done_sem);
            s_recv.decode_done_sem = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    /* Initialize I2S output */
    esp_err_t ret = audio_i2s_output_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S output init failed: %s", esp_err_to_name(ret));
    }

    /* Register GhostLink stream handler */
    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_AUDIO,
                                              stream_handler, NULL);

    /* Start decoder task; keep the large task stack in PSRAM when available. */
    if (!s_decode_task_stack) {
        s_decode_task_stack = (StackType_t *)heap_caps_malloc(AUDIO_DEC_TASK_STACK * sizeof(StackType_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_decode_task_tcb) {
        s_decode_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (s_decode_task_stack && s_decode_task_tcb) {
        s_recv.decode_task = xTaskCreateStatic(audio_decode_task, "audio_dec",
                                              AUDIO_DEC_TASK_STACK, NULL,
                                              AUDIO_DEC_TASK_PRIO,
                                              s_decode_task_stack,
                                              s_decode_task_tcb);
        if (s_recv.decode_task) {
            ESP_LOGI(TAG, "Audio decode task stack allocated from PSRAM: %d bytes",
                     (int)(AUDIO_DEC_TASK_STACK * sizeof(StackType_t)));
        }
    }

    if (!s_recv.decode_task &&
        xTaskCreate(audio_decode_task, "audio_dec", AUDIO_DEC_TASK_STACK, NULL,
                    AUDIO_DEC_TASK_PRIO, &s_recv.decode_task) != pdPASS) {
        free(s_recv.rx_ringbuf.buf);
        s_recv.rx_ringbuf.buf = NULL;
        if (s_recv.decode_done_sem) {
            vSemaphoreDelete(s_recv.decode_done_sem);
            s_recv.decode_done_sem = NULL;
        }
        esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_AUDIO, NULL, NULL);
        return ESP_ERR_NO_MEM;
    } else if (!s_decode_task_stack || !s_decode_task_tcb) {
        ESP_LOGW(TAG, "Audio decode task using internal stack fallback");
    }

    s_recv.initialized = true;
    ESP_LOGI(TAG, "Audio receiver initialized (96KB ringbuf @ PSRAM)");
    return ESP_OK;
}

void audio_receiver_manager_deinit(void)
{
    if (!s_recv.initialized) return;

    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_AUDIO, NULL, NULL);

    if (s_recv.decode_task) {
        s_recv.stop_requested = true;
        if (s_recv.decode_done_sem &&
            xSemaphoreTake(s_recv.decode_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "Decode task did not exit in time; forcing delete");
            vTaskDelete(s_recv.decode_task);
        }
        s_recv.decode_task = NULL;
        if (s_recv.decode_done_sem) {
            vSemaphoreDelete(s_recv.decode_done_sem);
            s_recv.decode_done_sem = NULL;
        }
    }

    if (s_recv.rx_ringbuf.buf) {
        free(s_recv.rx_ringbuf.buf);
        s_recv.rx_ringbuf.buf = NULL;
    }

    audio_i2s_output_deinit();

    memset(&s_recv, 0, sizeof(s_recv));
    ESP_LOGI(TAG, "Audio receiver deinitialized");
}

bool audio_receiver_manager_is_initialized(void)
{
    return s_recv.initialized;
}

esp_err_t audio_receiver_manager_set_sample_rate(uint32_t sample_rate)
{
    s_recv.detected_sample_rate = sample_rate;
    ESP_LOGI(TAG, "Detected sample rate: %lu Hz", (unsigned long)sample_rate);
    return audio_i2s_output_set_sample_rate(sample_rate);
}

static void stream_handler(uint8_t channel, const uint8_t *data, size_t length, void *user_data)
{
    (void)user_data;

    if (!s_recv.initialized || !s_recv.active) return;

    if (!s_recv.first_stream_packet_logged) {
        ESP_LOGI(TAG, "Audio stream packets received on channel %d", channel);
        s_recv.first_stream_packet_logged = true;
    }

    size_t written = ringbuf_write(&s_recv.rx_ringbuf, data, length);
    if (written < length) {
        ESP_LOGW(TAG, "Ring buffer overflow, dropped %d bytes", (int)(length - written));
        audio_receiver_send_status(true);
    } else {
        audio_receiver_send_status(false);
    }
}

static void audio_decode_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Decode task started");

    esp_audio_err_t reg_err = esp_mp3_dec_register();
    if (reg_err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "MP3 decoder register returned: %d", (int)reg_err);
    }

    /* Allocate decoder handle and input buffer from PSRAM */
    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
    };

    /* Open simple decoder */
    esp_audio_err_t aerr = esp_audio_simple_dec_open(&dec_cfg, &s_recv.simple_dec);
    if (aerr != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Failed to open simple decoder: %d", (int)aerr);
        s_recv.simple_dec = NULL;
        /* Continue anyway - might use raw pass-through */
    } else {
        ESP_LOGI(TAG, "MP3 simple decoder opened");
    }

    /* Input buffer for decoder (PSRAM only; 8KB internal defeats the lazy-init savings) */
    uint8_t *dec_in_buf = (uint8_t *)heap_caps_malloc(AUDIO_DEC_IN_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dec_in_buf) {
        dec_in_buf = (uint8_t *)heap_caps_malloc(AUDIO_DEC_IN_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    /* PCM output buffer (PSRAM only; see above) */
    size_t pcm_buf_size = AUDIO_PCM_BUF_SIZE;
    int16_t *pcm_buf = (int16_t *)heap_caps_malloc(pcm_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm_buf) {
        pcm_buf = (int16_t *)heap_caps_malloc(pcm_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (!dec_in_buf || !pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate decoder buffers");
        goto cleanup;
    }

    size_t buffered = 0;
    uint32_t last_stats_log = 0;
    uint32_t last_wait_log = 0;
    while (!s_recv.stop_requested) {
        /* If not active, just wait for data to arrive */
        if (!s_recv.active) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* Check if flush is requested - reset decoder state */
        if (s_recv.flush_requested) {
            ESP_LOGI(TAG, "Flushing decoder and I2S buffers");
            buffered = 0;
            s_recv.played_ms = 0;
            s_recv.played_pcm_bytes = 0;
            audio_i2s_output_flush();
            if (s_recv.simple_dec) {
                esp_audio_simple_dec_close(s_recv.simple_dec);
                esp_audio_simple_dec_cfg_t dec_cfg = {
                    .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
                };
                esp_audio_err_t aerr = esp_audio_simple_dec_open(&dec_cfg, &s_recv.simple_dec);
                if (aerr != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "Failed to reopen decoder: %d", (int)aerr);
                    s_recv.simple_dec = NULL;
                } else {
                    ESP_LOGI(TAG, "Decoder reopened");
                }
            }
            s_recv.flush_requested = false;
        }

        /* Wait for buffer to fill to threshold before starting decode */
        if (s_recv.waiting_for_buffer) {
            size_t buf_count = ringbuf_count(&s_recv.rx_ringbuf);
            if (buf_count < AUDIO_DEC_START_THRESHOLD) {
                uint32_t now = xTaskGetTickCount();
                if (now - last_wait_log >= pdMS_TO_TICKS(500)) {
                    ESP_LOGI(TAG, "Waiting for buffer to fill: %lu / %lu bytes",
                             (unsigned long)buf_count,
                             (unsigned long)AUDIO_DEC_START_THRESHOLD);
                    last_wait_log = now;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGI(TAG, "Buffer threshold reached, starting decode");
            s_recv.waiting_for_buffer = false;
        }

        /* Always try to fill the input buffer as much as possible */
        if (buffered < AUDIO_DEC_IN_BUF_SIZE) {
            size_t avail = ringbuf_count(&s_recv.rx_ringbuf);
            if (avail > 0) {
                size_t to_read = AUDIO_DEC_IN_BUF_SIZE - buffered;
                if (to_read > avail) to_read = avail;
                size_t n = ringbuf_read(&s_recv.rx_ringbuf, dec_in_buf + buffered, to_read);
                buffered += n;
                s_recv.rx_bytes_total += n;
            }
        }

        /* Log ringbuf fill level every 5 seconds */
        uint32_t now = xTaskGetTickCount();
        if (now - last_stats_log >= pdMS_TO_TICKS(5000)) {
            ESP_LOGI(TAG, "Ringbuf fill: %lu / %lu bytes, total_rx=%lu",
                     (unsigned long)ringbuf_count(&s_recv.rx_ringbuf),
                     (unsigned long)AUDIO_RX_RINGBUF_SIZE,
                     (unsigned long)s_recv.rx_bytes_total);
            last_stats_log = now;
        }
        audio_receiver_send_status(false);

        if (buffered < AUDIO_DEC_MIN_INPUT) {
            /* Not enough for decode; just keep feeding */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (s_recv.simple_dec) {
            esp_audio_simple_dec_raw_t raw = {
                .buffer = dec_in_buf,
                .len = (uint32_t)buffered,
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            esp_audio_simple_dec_out_t out = {
                .buffer = (uint8_t *)pcm_buf,
                .len = (uint32_t)pcm_buf_size,
            };

            aerr = esp_audio_simple_dec_process(s_recv.simple_dec, &raw, &out);
            if (aerr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out.needed_size > pcm_buf_size) {
                void *new_buf = heap_caps_realloc(pcm_buf, out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!new_buf) {
                    new_buf = heap_caps_realloc(pcm_buf, out.needed_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                }
                if (!new_buf) {
                    ESP_LOGE(TAG, "Failed to grow PCM buffer to %lu bytes", (unsigned long)out.needed_size);
                } else {
                    pcm_buf = (int16_t *)new_buf;
                    pcm_buf_size = out.needed_size;
                    ESP_LOGI(TAG, "Grew PCM decode buffer to %lu bytes", (unsigned long)pcm_buf_size);
                    out.buffer = (uint8_t *)pcm_buf;
                    out.len = (uint32_t)pcm_buf_size;
                    out.needed_size = 0;
                    out.decoded_size = 0;
                    raw.consumed = 0;
                    aerr = esp_audio_simple_dec_process(s_recv.simple_dec, &raw, &out);
                }
            }
            if (aerr == ESP_AUDIO_ERR_OK && out.decoded_size > 0) {
                /* Query the decoder format before the first PCM write. The
                 * output driver retains the previous track's source rate
                 * between sessions; writing one block before updating it can
                 * make a 48 kHz track start at the wrong pitch (and can leave
                 * it wrong if decoder info is only reported once). */
                if (s_recv.detected_sample_rate == 0) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (esp_audio_simple_dec_get_info(s_recv.simple_dec, &info) == ESP_AUDIO_ERR_OK) {
                        if (info.sample_rate > 0) {
                            audio_receiver_manager_set_sample_rate(info.sample_rate);
                        }
                        if (info.channel > 0) {
                            s_recv.output_channels = info.channel;
                        }
                    }
                }

#ifndef CONFIG_HAS_AW88298_SPEAKER
                /* Light PCM attenuation (-6 dB) applied in-place in the
                 * heap-allocated decode buffer. This avoids clipping on a
                 * tiny speaker/headphone while keeping the I2S write path
                 * allocation-free.
                 *
                 * Skipped on the AW88298, where the amp mixes (L+R)/2 in
                 * hardware - that halving already supplies the headroom, so
                 * doing it here as well just discarded a bit of resolution
                 * and 6 dB of level that a 1 W speaker cannot spare. */
                size_t pcm_samples = out.decoded_size / sizeof(int16_t);
                for (size_t i = 0; i < pcm_samples; ++i) {
                    pcm_buf[i] = (int16_t)(pcm_buf[i] >> 1);
                }
#endif

                /* Write decoded PCM to I2S */
                esp_err_t write_ret = audio_i2s_output_write(pcm_buf, out.decoded_size);
                if (write_ret != ESP_OK) {
                    ESP_LOGW(TAG, "PCM write failed: %s", esp_err_to_name(write_ret));
                } else {
                    s_recv.played_pcm_bytes += out.decoded_size;
                    s_recv.played_ms = audio_receiver_get_played_ms();
                    if (!s_recv.first_pcm_logged) {
                        ESP_LOGI(TAG, "First decoded PCM frame written: %lu bytes", (unsigned long)out.decoded_size);
                        s_recv.first_pcm_logged = true;
                    }
                }

            } else if (aerr != ESP_AUDIO_ERR_OK && aerr != ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGW(TAG, "Decode error: %d", (int)aerr);
            }

            size_t consumed = raw.consumed;
            if (consumed > buffered) {
                consumed = buffered;
            }
            if (consumed > 0) {
                buffered -= consumed;
                if (buffered > 0) {
                    memmove(dec_in_buf, dec_in_buf + consumed, buffered);
                }
            } else if (aerr != ESP_AUDIO_ERR_OK && buffered == AUDIO_DEC_IN_BUF_SIZE) {
                /* Avoid getting stuck forever on an undecodable byte sequence. */
                memmove(dec_in_buf, dec_in_buf + 1, buffered - 1);
                buffered--;
            }

            if (consumed == 0 && out.decoded_size == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        } else {
            /* No decoder available - just pass raw bytes (won't sound right but keeps pipeline alive) */
            ESP_LOGD(TAG, "No decoder, passing %d bytes raw", (int)buffered);
            buffered = 0;
        }
    }

cleanup:
    if (dec_in_buf) {
        heap_caps_free(dec_in_buf);
    }
    if (pcm_buf) {
        heap_caps_free(pcm_buf);
    }
    if (s_recv.simple_dec) {
        esp_audio_simple_dec_close(s_recv.simple_dec);
        s_recv.simple_dec = NULL;
    }

    if (s_recv.decode_done_sem) {
        xSemaphoreGive(s_recv.decode_done_sem);
    }
    ESP_LOGI(TAG, "Decode task exiting");
    vTaskDelete(NULL);
}

#else /* !CONFIG_HAS_TLV320DAC_I2S */

esp_err_t audio_receiver_manager_init(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_receiver_manager_deinit(void) {}
bool audio_receiver_manager_is_initialized(void) { return false; }
esp_err_t audio_receiver_manager_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void audio_receiver_manager_stop(void) {}
void audio_receiver_manager_pause(void) {}
void audio_receiver_manager_resume(void) {}
void audio_receiver_manager_flush(void) {}
size_t audio_receiver_manager_write_stream(const uint8_t *data, size_t len) { (void)data; (void)len; return 0; }
esp_err_t audio_receiver_manager_set_sample_rate(uint32_t sample_rate) { (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }

#endif /* CONFIG_HAS_TLV320DAC_I2S */
