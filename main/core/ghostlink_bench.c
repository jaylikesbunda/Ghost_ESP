// ghostlink_bench.c
// GhostLink throughput benchmark (glbench).
//
// Measures real UART link throughput in both directions over a dedicated stream
// channel (COMM_STREAM_CHANNEL_BENCH). Diagnostic only: it changes no transport
// behavior and touches no other channel.
//
// Safety notes (see configs for the numbers these protect):
//  - The producer runs on a low-priority task (prio 4): above the CLI SerialTask
//    (2) so it can run, below tx_task (11) and comm_rx_task so it never preempts
//    the transport it is measuring.
//  - It yields explicitly every GLBENCH_YIELD_CHUNKS chunks. Combined with the
//    blocking TX-queue waits, the C5 idle task keeps running (task WDT) and no
//    interrupt-off window ever approaches the 300 ms INT WDT.
//  - Every run is bounded by GLBENCH_MAX_MS and is abortable via `glbench stop`.
//  - The measurement window performs no logging at all: glog() forwards to the
//    peer, which would consume the very link being measured.
//  - Producer tasks come from xTaskCreate_psram(), so they must exit with
//    vTaskDeleteWithCaps() (never vTaskDelete()).

#include "core/ghostlink_bench.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLBENCH_TASK_STACK      4096
#define GLBENCH_TASK_PRIO       4
#define GLBENCH_CHUNK_BYTES     4096   // must be a multiple of 256 (pattern period)
#define GLBENCH_SEND_WAIT_MS    250    // per-fragment TX queue wait
#define GLBENCH_YIELD_CHUNKS    16     // chunks between explicit vTaskDelay(1)
#define GLBENCH_MAX_FAIL_STREAK 64     // consecutive send failures before abort
#define GLBENCH_RX_IDLE_MS      1500   // recv: idle gap that means "producer done"
#define GLBENCH_DEFAULT_KB      128u
#define GLBENCH_MAX_KB          65536u
#define GLBENCH_MAX_MS          60000u
#define GLBENCH_ARM_SETTLE_MS   250    // let the peer process "arm" before stream data

typedef enum {
    GLBENCH_MODE_SEND = 0,
    GLBENCH_MODE_RECV,
    GLBENCH_MODE_BOTH
} glbench_mode_t;

// ---- receiver-side sink state (registered on every board) ----
// Written by comm_rx_task, read by the benchmark task (prio 4).
// The 64-bit esp_timer timestamps are not atomic on a 32-bit core, so all
// access goes through s_sink_mux. Critical sections are a few field copies.
static portMUX_TYPE       s_sink_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_rx_bytes;
static volatile uint32_t s_rx_first_bad;   // byte offset of first divergence, GLBENCH_NO_BAD if intact
static volatile int64_t  s_rx_first_us;
static volatile int64_t  s_rx_last_us;
static volatile bool     s_rx_started;
static volatile bool     s_rx_armed;
static volatile uint32_t s_rx_expect;      // expected byte count for this run (0 = unknown)

#define GLBENCH_NO_BAD 0xFFFFFFFFu

typedef struct {
    uint32_t bytes;
    uint32_t first_bad;
    uint32_t expect;
    int64_t  first_us;
    int64_t  last_us;
    bool     started;
} glbench_sink_snapshot_t;

static void glbench_sink_snapshot(glbench_sink_snapshot_t *out) {
    portENTER_CRITICAL(&s_sink_mux);
    out->bytes = s_rx_bytes;
    out->first_bad = s_rx_first_bad;
    out->expect = s_rx_expect;
    out->first_us = s_rx_first_us;
    out->last_us = s_rx_last_us;
    out->started = s_rx_started;
    portEXIT_CRITICAL(&s_sink_mux);
}

/* Byte loss shifts every subsequent byte, so a raw mismatch *count* wildly
 * overstates the problem (a single dropped packet made 127 KB look corrupt
 * when only 3 KB was actually lost). The only meaningful pattern signal is
 * where the stream first diverges; after that point the offset is unusable. */
static void glbench_format_divergence(const glbench_sink_snapshot_t *snap, char *out, size_t out_len) {
    if (snap->first_bad == GLBENCH_NO_BAD) {
        snprintf(out, out_len, "pattern intact");
    } else {
        snprintf(out, out_len, "divergence at byte %u", (unsigned)snap->first_bad);
    }
}

/* "125/128 KB" when the expected size is known, else just "125 KB". */
static void glbench_format_bytes(const glbench_sink_snapshot_t *snap, char *out, size_t out_len) {
    if (snap->expect) {
        snprintf(out, out_len, "%u/%u KB", (unsigned)(snap->bytes / 1024u),
                 (unsigned)(snap->expect / 1024u));
    } else {
        snprintf(out, out_len, "%u KB", (unsigned)(snap->bytes / 1024u));
    }
}

// ---- producer / run state ----
static TaskHandle_t      s_task;
static volatile bool     s_stop;
static volatile bool     s_running;
static glbench_mode_t    s_mode;
static uint32_t          s_target_kb;
static bool              s_report_peer;

// The pattern repeats every 256 bytes and the chunk size is a multiple of 256,
// so one fill is valid for every chunk AND the receiver can verify using the
// absolute byte offset across fragment boundaries.
static inline uint8_t glbench_pattern(uint32_t index) {
    return (uint8_t)((index * 31u + 7u) & 0xFFu);
}

static void glbench_reset_sink(bool arm) {
    portENTER_CRITICAL(&s_sink_mux);
    s_rx_bytes = 0;
    s_rx_first_bad = GLBENCH_NO_BAD;
    s_rx_first_us = 0;
    s_rx_last_us = 0;
    s_rx_started = false;
    s_rx_expect = 0;
    portEXIT_CRITICAL(&s_sink_mux);
    s_rx_armed = arm;
}

static uint32_t glbench_kbps(uint32_t bytes, int64_t elapsed_us) {
    if (elapsed_us <= 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)bytes * 1000000ull) / (1024ull * (uint64_t)elapsed_us));
}

static void glbench_stream_rx(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!s_rx_armed || !data || length == 0) {
        return;
    }

    int64_t now = esp_timer_get_time();
    uint32_t base;

    portENTER_CRITICAL(&s_sink_mux);
    if (!s_rx_started) {
        s_rx_started = true;
        s_rx_first_us = now;
    }
    s_rx_last_us = now;
    base = s_rx_bytes;
    portEXIT_CRITICAL(&s_sink_mux);

    // Pattern check runs outside the critical section (up to 59 iterations).
    // Only the first divergence is recorded: once bytes are lost, every later
    // offset is shifted and further "mismatches" carry no information.
    uint32_t bad_at = GLBENCH_NO_BAD;
    for (size_t i = 0; i < length; ++i) {
        if (data[i] != glbench_pattern(base + (uint32_t)i)) {
            bad_at = base + (uint32_t)i;
            break;
        }
    }

    portENTER_CRITICAL(&s_sink_mux);
    s_rx_bytes = base + (uint32_t)length;
    if (bad_at != GLBENCH_NO_BAD && s_rx_first_bad == GLBENCH_NO_BAD) {
        s_rx_first_bad = bad_at;
    }
    portEXIT_CRITICAL(&s_sink_mux);
}

void ghostlink_bench_init(void) {
    // Safe no-op when the comm manager is not initialized on this board.
    esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_BENCH,
                                             glbench_stream_rx, NULL);
}

bool ghostlink_bench_is_running(void) {
    return s_running;
}

// Streams s_target_kb worth of patterned payload. Returns bytes accepted by the
// transport. Yields to lower-priority tasks periodically.
static uint32_t glbench_run_send(void) {
    uint32_t total = s_target_kb * 1024u;
    size_t chunk = GLBENCH_CHUNK_BYTES;
    if (chunk > total) {
        chunk = total;
    }
    if (chunk == 0) {
        return 0;
    }

    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        glog("glbench: out of memory for %u byte buffer\n", (unsigned)chunk);
        return 0;
    }
    for (size_t i = 0; i < chunk; ++i) {
        buf[i] = glbench_pattern((uint32_t)i);
    }

    int64_t start = esp_timer_get_time();
    uint32_t sent = 0;
    uint32_t chunks = 0;
    uint32_t fail_streak = 0;

    while (!s_stop && sent < total) {
        size_t want = (size_t)(total - sent);
        size_t this_len = (want < chunk) ? want : chunk;

        if (!esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_BENCH, buf, this_len,
                                              GLBENCH_SEND_WAIT_MS)) {
            if (++fail_streak >= GLBENCH_MAX_FAIL_STREAK) {
                glog("glbench: aborting send after %u consecutive failures\n",
                     (unsigned)fail_streak);
                break;
            }
            if (!esp_comm_manager_is_connected()) {
                break;
            }
            vTaskDelay(1);
            continue;
        }
        fail_streak = 0;
        sent += (uint32_t)this_len;

        if ((++chunks % GLBENCH_YIELD_CHUNKS) == 0) {
            vTaskDelay(1);
        }
        if ((esp_timer_get_time() - start) > (int64_t)GLBENCH_MAX_MS * 1000) {
            break;
        }
    }

    int64_t elapsed = esp_timer_get_time() - start;
    glog("glbench TX: %u KB in %lld ms = %u KB/s\n",
         (unsigned)(sent / 1024u), (long long)(elapsed / 1000), (unsigned)glbench_kbps(sent, elapsed));

    free(buf);
    return sent;
}

// Waits for the receiver sink to go idle (peer finished producing), then reports.
static void glbench_run_recv(void) {
    glbench_sink_snapshot_t snap;
    int64_t deadline = esp_timer_get_time() + (int64_t)GLBENCH_MAX_MS * 1000;

    while (!s_stop && esp_timer_get_time() < deadline) {
        glbench_sink_snapshot(&snap);
        if (snap.started &&
            (esp_timer_get_time() - snap.last_us) > (int64_t)GLBENCH_RX_IDLE_MS * 1000) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    glbench_sink_snapshot(&snap);
    if (!snap.started) {
        glog("glbench RX: no data received\n");
        return;
    }

    int64_t span = snap.last_us - snap.first_us;
    char bytes[32];
    char div[48];
    glbench_format_bytes(&snap, bytes, sizeof(bytes));
    glbench_format_divergence(&snap, div, sizeof(div));
    glog("glbench RX: %s in %lld ms = %u KB/s (%s)\n",
         bytes, (long long)(span / 1000), (unsigned)glbench_kbps(snap.bytes, span), div);
}

static void glbench_task(void *arg) {
    (void)arg;
    glbench_mode_t mode = s_mode;
    bool report_peer = s_report_peer;

    if (mode == GLBENCH_MODE_SEND || mode == GLBENCH_MODE_BOTH) {
        if (report_peer) {
            // Reset the peer's sink so its report covers this run only, and tell
            // it how much to expect so it can report loss directly. The arm
            // command is queued, while stream packets are handled inline on the
            // peer's rx_task, so settle briefly or the reset would land
            // mid-stream and lose bytes.
            char arm_buf[24];
            snprintf(arm_buf, sizeof(arm_buf), "arm %u", (unsigned)s_target_kb);
            esp_comm_manager_send_command("bench", arm_buf);
            vTaskDelay(pdMS_TO_TICKS(GLBENCH_ARM_SETTLE_MS));
        }
        glbench_run_send();
        if (report_peer) {
            // Ask the peer to report what it actually received.
            esp_comm_manager_send_command("bench", "report");
        }
    }

    if (!s_stop && (mode == GLBENCH_MODE_RECV || mode == GLBENCH_MODE_BOTH)) {
        glbench_reset_sink(true);
        // The peer will send this much; lets the local report show loss.
        s_rx_expect = s_target_kb * 1024u;
        if (report_peer) {
            char arg_buf[24];
            snprintf(arg_buf, sizeof(arg_buf), "tx %u", (unsigned)s_target_kb);
            if (!esp_comm_manager_send_command("bench", arg_buf)) {
                glog("glbench: failed to start peer transmit\n");
            } else {
                glbench_run_recv();
            }
        } else {
            glbench_run_recv();
        }
    }

    s_running = false;
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

static void glbench_start(glbench_mode_t mode, uint32_t kb, bool report_peer) {
    if (s_running) {
        glog("glbench: already running (use 'glbench stop')\n");
        return;
    }
    if (kb == 0 || kb > GLBENCH_MAX_KB) {
        glog("glbench: size must be 1-%u KB\n", (unsigned)GLBENCH_MAX_KB);
        return;
    }
    if (!esp_comm_manager_is_connected()) {
        glog("glbench: not connected to a peer\n");
        return;
    }

    s_mode = mode;
    s_target_kb = kb;
    s_report_peer = report_peer;
    s_stop = false;
    s_running = true;

    if (xTaskCreate_psram(glbench_task, "glbench", GLBENCH_TASK_STACK, NULL,
                          GLBENCH_TASK_PRIO, &s_task) != pdPASS) {
        s_running = false;
        s_task = NULL;
        glog("glbench: failed to create task\n");
    }
}

// ---- peer-side command handling ----

bool ghostlink_bench_handle_command(const char *command, const char *data) {
    if (!command || strcmp(command, "bench") != 0) {
        return false;
    }
    if (!data || data[0] == '\0') {
        return false;
    }

    if (strncmp(data, "arm", 3) == 0) {
        glbench_reset_sink(true);
        // Optional expected size ("arm <kb>") so the report can show loss.
        const char *p = data + 3;
        while (*p == ' ') {
            ++p;
        }
        if (*p != '\0') {
            s_rx_expect = (uint32_t)strtoul(p, NULL, 10) * 1024u;
        }
        return true;
    }

    if (strncmp(data, "tx", 2) == 0) {
        const char *p = data + 2;
        while (*p == ' ') {
            ++p;
        }
        uint32_t kb = (uint32_t)strtoul(p, NULL, 10);
        glbench_start(GLBENCH_MODE_SEND, kb ? kb : GLBENCH_DEFAULT_KB, false);
        return true;
    }

    if (strncmp(data, "report", 6) == 0) {
        glbench_sink_snapshot_t snap;
        glbench_sink_snapshot(&snap);
        int64_t span = snap.last_us - snap.first_us;
        char bytes[32];
        char div[48];
        char line[160];
        glbench_format_bytes(&snap, bytes, sizeof(bytes));
        glbench_format_divergence(&snap, div, sizeof(div));
        int n = snprintf(line, sizeof(line),
                         "GLBENCH RX peer: %s in %lld ms = %u KB/s (%s)\n",
                         bytes, (long long)(span / 1000),
                         (unsigned)glbench_kbps(snap.bytes, span), div);
        if (n > 0) {
            esp_comm_manager_send_response((const uint8_t *)line, (size_t)n);
        }
        return true;
    }

    return false;
}

// ---- CLI ----

static void glbench_print_status(void) {
    esp_comm_manager_stats_t st;
    if (!esp_comm_manager_get_stats(&st)) {
        glog("glbench: comm manager unavailable\n");
        return;
    }
    glog("GhostLink: %u baud, %s\n", (unsigned)st.baud,
         esp_comm_manager_is_connected() ? "connected" : "disconnected");
    glog("  counters: tx_dropped=%u rx_queue_dropped=%u crc_err=%u high_water=%u\n",
         (unsigned)st.tx_dropped_packets, (unsigned)st.rx_queue_dropped_packets,
         (unsigned)st.rx_crc_error_count, (unsigned)st.rx_high_water_alerts);
    glog("  queues: tx_waiting=%u, rx_free=%u, rx_peak=%u bytes\n",
         st.tx_queue_waiting, st.rx_queue_free,
         (unsigned)st.rx_buffer_high_watermark);
    if (s_running) {
        glog("  run: active\n");
    }
}

void handle_glbench_cmd(int argc, char **argv) {
    const char *sub = (argc > 1) ? argv[1] : "both";

    if (strcmp(sub, "stop") == 0) {
        if (!s_running) {
            glog("glbench: not running\n");
            return;
        }
        s_stop = true;
        glog("glbench: stopping...\n");
        return;
    }

    if (strcmp(sub, "status") == 0) {
        glbench_print_status();
        return;
    }

    glbench_mode_t mode;
    if (strcmp(sub, "send") == 0) {
        mode = GLBENCH_MODE_SEND;
    } else if (strcmp(sub, "recv") == 0) {
        mode = GLBENCH_MODE_RECV;
    } else if (strcmp(sub, "both") == 0) {
        mode = GLBENCH_MODE_BOTH;
    } else {
        glog("Usage: glbench [send|recv|both] [kb]\n");
        glog("       glbench stop | glbench status\n");
        glog("Example: glbench send 512\n");
        return;
    }

    uint32_t kb = GLBENCH_DEFAULT_KB;
    if (argc > 2) {
        kb = (uint32_t)strtoul(argv[2], NULL, 10);
    }

    // Arm the local sink so a recv phase counts from a clean state.
    glbench_reset_sink(true);
    glbench_start(mode, kb, true);
}
