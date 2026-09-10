/*
 * 1. Both chips boot up and automatically start listening for peers
 * 2. Once connected, use 'commsend <command>' to send commands
 */

#include "core/esp_comm_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "core/serial_manager.h"
#include "core/uart_share.h"
#include "soc/uart_pins.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "managers/views/terminal_screen.h"
#include "managers/ap_manager.h"
#include "managers/peer_storage_manager.h"
#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_visualizer.h"
#endif

#define COMM_BUFFER_SIZE 256
#define UART_RX_BUFFER_SIZE (COMM_BUFFER_SIZE * 2)
#define COMM_PACKET_SIZE 64
#define DISCOVERY_INTERVAL_MS 3000
#define HANDSHAKE_TIMEOUT_MS 3000
#define COMMAND_TIMEOUT_MS 500
#define PING_INTERVAL_MS 1000
#define LINK_TIMEOUT_MS 4000
#define COMM_PROTOCOL_STACK_WORDS 4096

// protocol constants
#define PACKET_START_BYTE 0xAA
#define PACKET_HEADER_SIZE 3
#define PACKET_CHECKSUM_SIZE 1
#define PACKET_MAX_PAYLOAD (COMM_PACKET_SIZE - 4)
#define RESPONSE_LOG_PREFIX "RX: "
#define COMM_STREAM_COMMAND_MAX 250
// flags for PACKET_TYPE_RESPONSE framing
#define RESP_FLAG_LINE_START 0x01


// sizes for fields embedded in packets
#define CHIP_ID_LEN 6
#define CHIP_NAME_MAX 32           // includes null terminator
#define CHIP_NAME_COPY_LEN 31      // max characters copied for name (excludes terminator)
#define DISCOVERY_PAYLOAD_LEN (CHIP_ID_LEN + CHIP_NAME_MAX)
#define MAX_CMD_LEN 32             // includes null terminator when placed in packet

#if defined(CONFIG_IDF_TARGET_ESP32)
#define DEFAULT_TX_PIN GPIO_NUM_17
#define DEFAULT_RX_PIN GPIO_NUM_16
#else
#define DEFAULT_TX_PIN GPIO_NUM_6
#define DEFAULT_RX_PIN GPIO_NUM_7
#endif

static const char* TAG = "esp_comm_manager";

static bool comm_pins_conflict_with_rgb_display(gpio_num_t tx_pin, gpio_num_t rx_pin)
{
#if defined(CONFIG_CROWPANEL_P4_PANEL_RGB_800X480)
    // 5-inch P4: RGB data uses GPIO4..19, DE/PCLK use 2/3, HSYNC/VSYNC use 40/41.
    const int pins[] = { (int)tx_pin, (int)rx_pin };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if ((pins[i] >= 2 && pins[i] <= 19) || pins[i] == 40 || pins[i] == 41) {
            ESP_LOGW(TAG, "GhostLink TX:%d RX:%d conflicts with 5-inch RGB display; choose free pins",
                     (int)tx_pin, (int)rx_pin);
            return true;
        }
    }
#else
    (void)tx_pin;
    (void)rx_pin;
#endif
    return false;
}

typedef enum {
    PACKET_TYPE_DISCOVERY = 0x01,
    PACKET_TYPE_HANDSHAKE_REQ = 0x02,
    PACKET_TYPE_HANDSHAKE_ACK = 0x03,
    PACKET_TYPE_COMMAND = 0x04,
    PACKET_TYPE_RESPONSE = 0x05,
    PACKET_TYPE_PING = 0x06,
    PACKET_TYPE_PONG = 0x07,
    PACKET_TYPE_STREAM = 0x08,
    PACKET_TYPE_COMMAND_END = 0x09
} packet_type_t;

typedef enum {
    PARSE_STATE_IDLE = 0,
    PARSE_STATE_START_BYTE,
    PARSE_STATE_TYPE,
    PARSE_STATE_DATA,
    PARSE_STATE_CHECKSUM
} packet_parse_state_t;

typedef struct {
    uint8_t start_byte;
    uint8_t type;
    uint8_t length;
    uint8_t data[COMM_PACKET_SIZE - 4];
} __attribute__((packed)) comm_packet_t;

typedef struct {
    StackType_t *stack;
    StaticTask_t *tcb;
} psram_task_resources_t;

typedef struct {
    char command[33];
    char data[COMM_PACKET_SIZE];
    char* dynamic_data;
} comm_command_t;

#define COMM_TX_QUEUE_DEPTH 16
#define COMM_RX_QUEUE_DEPTH 64
#define COMM_COMMAND_QUEUE_DEPTH 4

EXT_RAM_BSS_ATTR static uint8_t s_tx_queue_storage[COMM_TX_QUEUE_DEPTH * sizeof(comm_packet_t)];
EXT_RAM_BSS_ATTR static uint8_t s_rx_queue_storage[COMM_RX_QUEUE_DEPTH * sizeof(comm_packet_t)];
EXT_RAM_BSS_ATTR static uint8_t s_command_queue_storage[COMM_COMMAND_QUEUE_DEPTH * sizeof(comm_command_t)];
static StaticQueue_t s_tx_queue_control;
static StaticQueue_t s_rx_queue_control;
static StaticQueue_t s_command_queue_control;

static QueueHandle_t create_tx_queue(void) {
    return xQueueCreateStatic(COMM_TX_QUEUE_DEPTH, sizeof(comm_packet_t),
                              s_tx_queue_storage, &s_tx_queue_control);
}

static QueueHandle_t create_rx_queue(void) {
    return xQueueCreateStatic(COMM_RX_QUEUE_DEPTH, sizeof(comm_packet_t),
                              s_rx_queue_storage, &s_rx_queue_control);
}

static QueueHandle_t create_command_queue(void) {
    return xQueueCreateStatic(COMM_COMMAND_QUEUE_DEPTH, sizeof(comm_command_t),
                              s_command_queue_storage, &s_command_queue_control);
}

typedef struct {
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    
    comm_state_t state;
    comm_role_t role;
    comm_peer_t peer;
    
    QueueHandle_t rx_packet_queue;
    QueueHandle_t tx_queue;
    QueueHandle_t command_queue;
    TaskHandle_t rx_task_handle;
    TaskHandle_t tx_task_handle;
    TaskHandle_t protocol_task_handle;
    TaskHandle_t command_executor_task_handle;
    TimerHandle_t discovery_timer;
    TimerHandle_t handshake_timer;
    TimerHandle_t ping_timer;
    
    comm_command_callback_t command_callback;
    void* callback_user_data;
    
    char chip_name[32];
    uint8_t chip_id[6];
    
    volatile bool initialized;
    bool is_executing_remote_cmd;
    bool remote_output_capture;
    bool uart_driver_installed;
    bool use_crc;


    uint32_t tx_dropped_packets;
    uint32_t rx_queue_dropped_packets;
    uint32_t rx_crc_error_count;
    uint32_t stream_ignored_packets;
    size_t rx_buffer_high_watermark;
    uint32_t rx_high_water_alerts;

    char response_assembly[512];
    size_t response_assembly_len;

    packet_parse_state_t parse_state;
    comm_packet_t partial_packet;
    uint8_t data_bytes_received;

    // simple sequencing for RESPONSE frames to detect gaps
    uint8_t tx_seq;
    uint8_t rx_expected_seq;
    bool rx_seq_initialized;
    bool rx_drop_until_newline;

    SemaphoreHandle_t state_mutex;

    psram_task_resources_t rx_task_res;
    psram_task_resources_t tx_task_res;
    psram_task_resources_t protocol_task_res;
    psram_task_resources_t command_task_res;
    TickType_t last_rx_tick;

    comm_stream_callback_t stream_handlers[COMM_MAX_STREAM_CHANNELS];
    void* stream_user_data[COMM_MAX_STREAM_CHANNELS];
    char* command_stream_buf;
    size_t command_stream_len;
    size_t command_stream_cap;
    bool command_stream_discarding;
} esp_comm_manager_t;

static esp_comm_manager_t* s_comm_manager = NULL;
static comm_command_callback_t s_pending_callback = NULL;
static void* s_pending_callback_user_data = NULL;
static comm_response_callback_t s_response_callback = NULL;
static void* s_response_callback_user_data = NULL;
static comm_data_callback_t s_data_callback = NULL;
static void* s_data_callback_user_data = NULL;
static comm_command_end_callback_t s_command_end_callback = NULL;
static void* s_command_end_callback_user_data = NULL;
static uart_port_t s_uart_num = UART_NUM_1; /* selected UART for dualcomm */

// Forward declarations for functions referenced before their definitions
static void tx_task(void* arg);
static void rx_task(void* arg);
static void protocol_task(void* arg);
static void command_executor_task(void* arg);
static void discovery_timer_callback(TimerHandle_t xTimer);
static void handshake_timer_callback(TimerHandle_t xTimer);
static void ping_timer_callback(TimerHandle_t xTimer);
static void send_discovery_packet(void);
static void send_handshake_request(const char* peer_name);
static void send_handshake_ack(void);
static void handle_received_packet(esp_comm_manager_t* comm, const comm_packet_t* packet);
static void handle_connection_loss(esp_comm_manager_t* comm, const char* reason);
static bool queue_received_command(esp_comm_manager_t* comm, const char* command, const char* data);
static void drain_command_queue(QueueHandle_t queue);
static void reset_command_stream(esp_comm_manager_t* comm);
static void release_command_stream(esp_comm_manager_t* comm);
static bool send_command_end_packet(uint8_t status);

static inline void lock_state(esp_comm_manager_t* comm) {
    if (comm && comm->state_mutex) {
        xSemaphoreTake(comm->state_mutex, portMAX_DELAY);
    }
}

static inline void unlock_state(esp_comm_manager_t* comm) {
    if (comm && comm->state_mutex) {
        xSemaphoreGive(comm->state_mutex);
    }
}

static void log_response_line(const char* line, size_t line_len, char* log_buffer, size_t buffer_size) {
    const char* prefix = RESPONSE_LOG_PREFIX;
    size_t prefix_len = strlen(prefix);

    if (buffer_size <= prefix_len + 2) {
        terminal_view_add_text(prefix);
        return;
    }

    size_t max_copy = buffer_size - prefix_len - 2; // reserve for optional ellipsis + newline + NUL
    size_t copy_len = (line_len < max_copy) ? line_len : max_copy;
    size_t pos = 0;

    memcpy(log_buffer, prefix, prefix_len);
    pos += prefix_len;

    if (copy_len > 0) {
        memcpy(log_buffer + pos, line, copy_len);
        pos += copy_len;
    }

    if (line_len > copy_len && pos + 4 < buffer_size) {
        memcpy(log_buffer + pos, "...", 3);
        pos += 3;
    }

    log_buffer[pos++] = '\n';
    log_buffer[pos] = '\0';

    terminal_view_add_text(log_buffer);
}

static StackType_t* alloc_task_stack(size_t stack_bytes);
static StaticTask_t* alloc_task_tcb(void);
static void free_task_resources(psram_task_resources_t* res);

static TaskHandle_t create_task_static(psram_task_resources_t* res,
                                       TaskFunction_t task_fn,
                                       const char* name,
                                       uint32_t stack_bytes,
                                       void* arg,
                                       UBaseType_t priority) {
    if (!res) {
        return NULL;
    }

    res->stack = alloc_task_stack(stack_bytes);
    res->tcb = alloc_task_tcb();

    if (!res->stack || !res->tcb) {
        free_task_resources(res);
        return NULL;
    }

    return xTaskCreateStatic(task_fn, name, stack_bytes, arg, priority, res->stack, res->tcb);
}

static StackType_t* alloc_task_stack(size_t stack_bytes) {
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    StackType_t* stack = (StackType_t*)heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stack) {
        return stack;
    }
#endif
    return (StackType_t*)heap_caps_malloc(stack_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static StaticTask_t* alloc_task_tcb(void) {
    return (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void free_task_resources(psram_task_resources_t* res) {
    if (!res) {
        return;
    }
    if (res->stack) {
        heap_caps_free(res->stack);
        res->stack = NULL;
    }
    if (res->tcb) {
        heap_caps_free(res->tcb);
        res->tcb = NULL;
    }
}

static bool ensure_command_executor(esp_comm_manager_t* comm) {
    if (!comm || !comm->command_callback) {
        return false;
    }
    if (!comm->command_queue) {
        comm->command_queue = create_command_queue();
    }
    if (comm->command_queue && !comm->command_executor_task_handle) {
        TaskHandle_t t = create_task_static(&comm->command_task_res, command_executor_task,
                                            "comm_cmd_exec_task", 3072, comm, 5);
        if (!t) {
            printf("E: failed to create command executor task\n");
            free_task_resources(&comm->command_task_res);
        }
        comm->command_executor_task_handle = t;
    }
    return comm->command_queue != NULL;
}

static void free_command_payload(comm_command_t* cmd) {
    if (cmd && cmd->dynamic_data) {
        free(cmd->dynamic_data);
        cmd->dynamic_data = NULL;
    }
}

static void drain_command_queue(QueueHandle_t queue) {
    if (!queue) {
        return;
    }
    comm_command_t pending;
    while (xQueueReceive(queue, &pending, 0) == pdPASS) {
        free_command_payload(&pending);
    }
}

static bool queue_received_command(esp_comm_manager_t* comm, const char* command, const char* data) {
    if (!comm || !command || !ensure_command_executor(comm)) {
        return false;
    }

    comm_command_t cmd_to_queue;
    memset(&cmd_to_queue, 0, sizeof(comm_command_t));
    strncpy(cmd_to_queue.command, command, MAX_CMD_LEN);
    cmd_to_queue.command[MAX_CMD_LEN] = '\0';

    if (data && data[0] != '\0') {
        size_t data_len = strlen(data);
        if (data_len < sizeof(cmd_to_queue.data)) {
            memcpy(cmd_to_queue.data, data, data_len + 1);
        } else {
            cmd_to_queue.dynamic_data = (char*)malloc(data_len + 1);
            if (!cmd_to_queue.dynamic_data) {
                printf("Command data allocation failed for: %s\n", command);
                return false;
            }
            memcpy(cmd_to_queue.dynamic_data, data, data_len + 1);
        }
    }

    if (xQueueSend(comm->command_queue, &cmd_to_queue, pdMS_TO_TICKS(10)) != pdPASS) {
        printf("Command queue full, dropped command: %s\n", cmd_to_queue.command);
        free_command_payload(&cmd_to_queue);
        return false;
    }
    return true;
}

static void reset_command_stream(esp_comm_manager_t* comm) {
    if (!comm) {
        return;
    }
    comm->command_stream_len = 0;
    comm->command_stream_discarding = false;
}

static void release_command_stream(esp_comm_manager_t* comm) {
    if (!comm) return;
    free(comm->command_stream_buf);
    comm->command_stream_buf = NULL;
    comm->command_stream_cap = 0;
    reset_command_stream(comm);
}

static bool ensure_command_stream_capacity(esp_comm_manager_t* comm, size_t needed) {
    if (!comm || needed > COMM_STREAM_COMMAND_MAX + 1) {
        return false;
    }
    if (needed <= comm->command_stream_cap) {
        return true;
    }

    free(comm->command_stream_buf);
    char* new_buf = (char*)malloc(COMM_STREAM_COMMAND_MAX + 1);
    if (!new_buf) {
        comm->command_stream_buf = NULL;
        comm->command_stream_cap = 0;
        return false;
    }
    comm->command_stream_buf = new_buf;
    comm->command_stream_cap = COMM_STREAM_COMMAND_MAX + 1;
    return true;
}

static bool queue_command_line(esp_comm_manager_t* comm, char* line) {
    if (!comm || !line) {
        return false;
    }

    while (*line && isspace((unsigned char)*line)) {
        line++;
    }
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return false;
    }

    size_t cmd_len = 0;
    while (line[cmd_len] && !isspace((unsigned char)line[cmd_len])) {
        cmd_len++;
    }
    if (cmd_len == 0 || cmd_len > MAX_CMD_LEN) {
        printf("Stream command name too long\n");
        return false;
    }

    char command[MAX_CMD_LEN + 1];
    memcpy(command, line, cmd_len);
    command[cmd_len] = '\0';

    char* data = line + cmd_len;
    while (*data && isspace((unsigned char)*data)) {
        data++;
    }
    return queue_received_command(comm, command, data[0] ? data : NULL);
}

static void handle_command_stream_data(esp_comm_manager_t* comm, const uint8_t* payload, size_t payload_len) {
    if (!comm || !payload || payload_len == 0) {
        return;
    }

    for (size_t i = 0; i < payload_len; ++i) {
        uint8_t b = payload[i];
        if (b == '\0' || b == '\n') {
            if (comm->command_stream_len > 0 && comm->command_stream_buf) {
                comm->command_stream_buf[comm->command_stream_len] = '\0';
                comm->remote_output_capture = true;
                if (!queue_command_line(comm, comm->command_stream_buf)) {
                    printf("Stream command dropped\n");
                    (void)send_command_end_packet(COMM_COMMAND_END_STATUS_DISPATCH_FAILED);
                }
            }
            reset_command_stream(comm);
            continue;
        }
        if (b == '\r') {
            continue;
        }
        if (comm->command_stream_discarding) {
            continue;
        }
        if (comm->command_stream_len >= COMM_STREAM_COMMAND_MAX ||
            !ensure_command_stream_capacity(comm, comm->command_stream_len + 2)) {
            printf("Stream command too long\n");
            reset_command_stream(comm);
            comm->command_stream_discarding = true;
            (void)send_command_end_packet(COMM_COMMAND_END_STATUS_DISPATCH_FAILED);
            continue;
        }
        comm->command_stream_buf[comm->command_stream_len++] = (char)b;
    }
}

static uint8_t crc8_step(uint8_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        if (crc & 0x80) {
            crc = (uint8_t)((crc << 1) ^ 0x07);
        } else {
            crc <<= 1;
        }
    }
    return crc;
}

static uint8_t calculate_crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = crc8_step(crc, data[i]);
    }
    return crc;
}

static uint8_t calculate_legacy_checksum(const uint8_t* data, size_t len) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; ++i) {
        checksum ^= data[i];
    }
    return checksum;
}

static uint8_t compute_packet_checksum(const comm_packet_t* packet, bool use_crc) {
    size_t frame_len = PACKET_HEADER_SIZE + packet->length;
    const uint8_t* bytes = (const uint8_t*)packet;
    return use_crc ? calculate_crc8(bytes, frame_len)
                   : calculate_legacy_checksum(bytes, frame_len);
}

static bool send_packet_internal(const comm_packet_t* packet, TickType_t wait) {
    if (!packet || !s_comm_manager) {
        return false;
    }

    esp_comm_manager_t *comm = s_comm_manager;
    bool lock_acquired = false;
    if (comm->state_mutex) {
        TaskHandle_t holder = xSemaphoreGetMutexHolder(comm->state_mutex);
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        if (holder != self && xSemaphoreTake(comm->state_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            lock_acquired = true;
        } else if (holder != self) {
            return false;
        }
    }

    if (!comm->initialized) {
        if (lock_acquired) {
            unlock_state(comm);
        }
        return false;
    }

    QueueHandle_t tx_queue = comm->tx_queue;
    bool use_crc = comm->use_crc;

    if (tx_queue) {
        if (xQueueSend(tx_queue, packet, wait) != pdPASS) {
            comm->tx_dropped_packets++;
            if ((comm->tx_dropped_packets & 0x0F) == 1) {
                printf("TX queue full, dropped packet type 0x%02x (drops=%lu)\n",
                       packet->type, (unsigned long)comm->tx_dropped_packets);
            }
            if (lock_acquired) {
                unlock_state(comm);
            }
            return false;
        }
        if (lock_acquired) {
            unlock_state(comm);
        }
        return true;
    }

    if (lock_acquired) {
        unlock_state(comm);
    }

    // Direct transmit path used during scanning/handshake before TX task exists
    uart_write_bytes(s_uart_num, (const char *)packet, PACKET_HEADER_SIZE + packet->length);
    uint8_t checksum = compute_packet_checksum(packet, use_crc);
    uart_write_bytes(s_uart_num, (const char *)&checksum, 1);
    if (packet->type == PACKET_TYPE_RESPONSE) {
        bool ends_with_newline = false;
        if (packet->length > 2) {
            uint8_t last = packet->data[packet->length - 1];
            ends_with_newline = (last == '\n');
        }
        vTaskDelay(pdMS_TO_TICKS(5));
        if (ends_with_newline) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return true;
}

static bool send_packet(const comm_packet_t* packet) {
    TickType_t wait = (packet && packet->type == PACKET_TYPE_RESPONSE)
        ? pdMS_TO_TICKS(100)
        : pdMS_TO_TICKS(5);
    return send_packet_internal(packet, wait);
}

static bool send_command_end_packet(uint8_t status) {
    comm_packet_t packet = {0};
    packet.start_byte = PACKET_START_BYTE;
    packet.type = PACKET_TYPE_COMMAND_END;
    packet.length = 1;
    packet.data[0] = status;
    if (!send_packet(&packet)) {
        printf("Command dispatch end packet dropped (status=%u)\n", (unsigned)status);
        return false;
    }
    return true;
}

static void tx_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_packet_t packet;

    while (comm->initialized) {
        if (xQueueReceive(comm->tx_queue, &packet, pdMS_TO_TICKS(100)) == pdPASS) {
            uart_write_bytes(s_uart_num, (uint8_t*)&packet, PACKET_HEADER_SIZE + packet.length);
            uint8_t checksum = compute_packet_checksum(&packet, comm->use_crc);
            uart_write_bytes(s_uart_num, &checksum, 1);
            if (packet.type == PACKET_TYPE_STREAM) {
                /* Stream pacing needs physical UART backpressure, not just TX
                 * queue acceptance. Without this the TX task can build a UART
                 * backlog and the receiver sees large bursts despite sender
                 * media-clock pacing. */
                uart_wait_tx_done(s_uart_num, pdMS_TO_TICKS(20));
            }
            if (packet.type == PACKET_TYPE_RESPONSE) {
                bool ends_with_newline = false;
                if (packet.length > 2) {
                    uint8_t last = packet.data[packet.length - 1];
                    ends_with_newline = (last == '\n');
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                if (ends_with_newline) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                }
            }
        }
    }

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void rx_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    uint8_t rx_buffer[COMM_BUFFER_SIZE];

    while (comm->initialized) {
        int len = uart_read_bytes(s_uart_num, rx_buffer, COMM_BUFFER_SIZE, pdMS_TO_TICKS(10));
        if (len <= 0) {
            continue;
        }

        TickType_t now = xTaskGetTickCount();

        while (len > 0) {
            size_t buffered_len = 0;
            if (uart_get_buffered_data_len(s_uart_num, &buffered_len) == ESP_OK) {
                if (buffered_len > comm->rx_buffer_high_watermark) {
                    comm->rx_buffer_high_watermark = buffered_len;
                }
                size_t alert_threshold = UART_RX_BUFFER_SIZE * 3 / 4;
                if (buffered_len > alert_threshold) {
                    comm->rx_high_water_alerts++;
                    if ((comm->rx_high_water_alerts & 0x0F) == 1) {
                        printf("UART RX buffered %u bytes (alerts=%lu)\n",
                               (unsigned)buffered_len, (unsigned long)comm->rx_high_water_alerts);
                    }
                }
            }

            for (int i = 0; i < len; ++i) {
                uint8_t byte = rx_buffer[i];

                switch (comm->parse_state) {
                    case PARSE_STATE_IDLE:
                        if (byte == PACKET_START_BYTE) {
                            comm->parse_state = PARSE_STATE_START_BYTE;
                            comm->partial_packet.start_byte = byte;
                        }
                        break;

                    case PARSE_STATE_START_BYTE:
                        comm->partial_packet.type = byte;
                        comm->parse_state = PARSE_STATE_TYPE;
                        break;

                    case PARSE_STATE_TYPE:
                        comm->partial_packet.length = byte;
                        if (byte > COMM_PACKET_SIZE - 4) {
                            comm->parse_state = PARSE_STATE_IDLE;
                            break;
                        }
                        comm->data_bytes_received = 0;
                        comm->parse_state = (byte == 0) ? PARSE_STATE_CHECKSUM : PARSE_STATE_DATA;
                        break;

                    case PARSE_STATE_DATA:
                        comm->partial_packet.data[comm->data_bytes_received++] = byte;
                        if (comm->data_bytes_received >= comm->partial_packet.length) {
                            comm->parse_state = PARSE_STATE_CHECKSUM;
                        }
                        break;

                    case PARSE_STATE_CHECKSUM:
                        {
                            size_t frame_len = PACKET_HEADER_SIZE + comm->partial_packet.length;
                            const uint8_t* frame_bytes = (const uint8_t*)&comm->partial_packet;
                            uint8_t crc = calculate_crc8(frame_bytes, frame_len);
                            bool valid = (crc == byte);
                            if (valid) {
                                if (!comm->use_crc) {
                                    comm->use_crc = true;
                                    printf("Peer supports CRC-8, upgrading TX checksum\n");
                                }
                            } else {
                                uint8_t legacy = calculate_legacy_checksum(frame_bytes, frame_len);
                                if (legacy == byte) {
                                    if (comm->use_crc) {
                                        comm->use_crc = false;
                                        printf("Peer using legacy checksum, downgrading to XOR\n");
                                    }
                                    valid = true;
                                }
                            }

                            if (valid) {
                                comm->last_rx_tick = now;
                                if (comm->partial_packet.type == PACKET_TYPE_STREAM) {
                                    handle_received_packet(comm, &comm->partial_packet);
                                } else if (comm->rx_packet_queue) {
                                    if (xQueueSend(comm->rx_packet_queue, &comm->partial_packet, pdMS_TO_TICKS(2)) != pdPASS) {
                                        comm->rx_queue_dropped_packets++;
                                        if ((comm->rx_queue_dropped_packets & 0x0F) == 1) {
                                            printf("RX packet queue full, dropped type 0x%02x (drops=%lu)\n",
                                                   comm->partial_packet.type,
                                                   (unsigned long)comm->rx_queue_dropped_packets);
                                        }
                                    }
                                } else {
                                    // Minimal inline handling during scanning/handshake (no protocol task yet)
                                    handle_received_packet(comm, &comm->partial_packet);
                                }
                            } else {
                                comm->rx_crc_error_count++;
                                if (comm->partial_packet.type == PACKET_TYPE_RESPONSE) {
                                    // Signal protocol task to drop until newline to avoid merging partial lines
                                    comm->rx_drop_until_newline = true;
                                }
                                if ((comm->rx_crc_error_count & 0x0F) == 1) {
                                    size_t fifo_bytes = 0;
                                    if (uart_get_buffered_data_len(s_uart_num, &fifo_bytes) != ESP_OK) {
                                        fifo_bytes = 0;
                                    }
                                    UBaseType_t queue_free = comm->rx_packet_queue
                                        ? uxQueueSpacesAvailable(comm->rx_packet_queue)
                                        : 0;
                                    printf("CRC error on packet type 0x%02x (errors=%lu, fifo=%u, rx_queue_free=%u)\n",
                                           comm->partial_packet.type,
                                           (unsigned long)comm->rx_crc_error_count,
                                           (unsigned)fifo_bytes,
                                           (unsigned)queue_free);
                                }
                            }
                            comm->parse_state = PARSE_STATE_IDLE;
                        }
                        break;
                }
            }

            int drained = uart_read_bytes(s_uart_num, rx_buffer, COMM_BUFFER_SIZE, 0);
            if (drained > 0) {
                len = drained;
            } else {
                len = 0;
            }
        }
    }

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void send_discovery_packet(void) {
    if (!s_comm_manager) return;

    comm_packet_t packet = {0};
    packet.start_byte = PACKET_START_BYTE;
    packet.type = PACKET_TYPE_DISCOVERY;
    packet.length = DISCOVERY_PAYLOAD_LEN;

    memcpy(packet.data, s_comm_manager->chip_id, CHIP_ID_LEN);
    strncpy((char*)packet.data + CHIP_ID_LEN, s_comm_manager->chip_name, CHIP_NAME_COPY_LEN);
    ((char*)packet.data)[CHIP_ID_LEN + CHIP_NAME_COPY_LEN] = '\0';

    send_packet(&packet);
}

static void send_handshake_request(const char* peer_name) {
    if (!s_comm_manager) return;

    comm_packet_t packet = {0};
    packet.start_byte = PACKET_START_BYTE;
    packet.type = PACKET_TYPE_HANDSHAKE_REQ;
    packet.length = MAX_CMD_LEN;

    strncpy((char*)packet.data, peer_name, MAX_CMD_LEN);

    send_packet(&packet);
}

static void send_handshake_ack(void) {
    if (!s_comm_manager) return;

    comm_packet_t packet = {0};
    packet.start_byte = PACKET_START_BYTE;
    packet.type = PACKET_TYPE_HANDSHAKE_ACK;
    packet.length = 0;

    send_packet(&packet);
}

static void handle_received_packet(esp_comm_manager_t* comm, const comm_packet_t* packet) {
    if (!comm || !packet) return;
    static char log_buffer[128];

    switch(packet->type) {
        case PACKET_TYPE_DISCOVERY:
            if (comm->state == COMM_STATE_SCANNING) {
                if (memcmp(comm->chip_id, packet->data, CHIP_ID_LEN) != 0) {
                    memcpy(comm->peer.chip_id, packet->data, CHIP_ID_LEN);
                    strncpy(comm->peer.chip_name, (char*)packet->data + CHIP_ID_LEN, CHIP_NAME_MAX);
                    comm->peer.chip_name[CHIP_NAME_MAX - 1] = '\0';
                    printf("Discovered peer: %s\n", comm->peer.chip_name);
                    snprintf(log_buffer, sizeof(log_buffer), "I: Discovered peer: %s\n", comm->peer.chip_name);
                    terminal_view_add_text(log_buffer);

                    if (strcmp(comm->chip_name, comm->peer.chip_name) > 0) {
                        printf("Peer has smaller name, I will initiate connection.\n");
                        terminal_view_add_text("I: Peer has smaller name, I will initiate connection.\n");
                        esp_comm_manager_connect_to_peer(comm->peer.chip_name);
                    }
                }
            }
            break;

        case PACKET_TYPE_HANDSHAKE_REQ:
            if (comm->state == COMM_STATE_SCANNING || comm->state == COMM_STATE_IDLE) {
                char requested_name[CHIP_NAME_MAX];
                strncpy(requested_name, (char*)packet->data, CHIP_NAME_MAX);
                requested_name[CHIP_NAME_MAX - 1] = '\0';
                if (strcmp(requested_name, comm->chip_name) == 0) {
                    lock_state(comm);
                    comm->state = COMM_STATE_HANDSHAKE;
                    comm->role = COMM_ROLE_SLAVE;
                    if (comm->handshake_timer) {
                        xTimerStart(comm->handshake_timer, 0);
                    }
                    if (comm->command_callback && !comm->command_queue) {
                        comm->command_queue = create_command_queue();
                        if (comm->command_queue && !comm->command_executor_task_handle) {
                            TaskHandle_t t = create_task_static(&comm->command_task_res, command_executor_task,
                                                               "comm_cmd_exec_task", 3072, comm, 5);
                            if (!t) {
                                printf("E: failed to create command executor task\n");
                                free_task_resources(&comm->command_task_res);
                            }
                            comm->command_executor_task_handle = t;
                        }
                    }
                    send_handshake_ack();
                    comm->state = COMM_STATE_CONNECTED;
                    // reset seq tracking on new connection
                    comm->tx_seq = 0;
                    comm->rx_seq_initialized = false;
                    comm->rx_expected_seq = 0;
                    comm->rx_drop_until_newline = false;
                    comm->last_rx_tick = xTaskGetTickCount();
                    comm->remote_output_capture = false;
                    if (comm->handshake_timer) {
                        xTimerStop(comm->handshake_timer, 0);
                    }
                    if (comm->discovery_timer) {
                        xTimerStop(comm->discovery_timer, 0);
                        xTimerDelete(comm->discovery_timer, 0);
                        comm->discovery_timer = NULL;
                    }
                    if (!comm->ping_timer) {
                        comm->ping_timer = xTimerCreate("ping_timer", pdMS_TO_TICKS(PING_INTERVAL_MS), pdTRUE, NULL, ping_timer_callback);
                    }
                    if (comm->ping_timer) {
                        xTimerStart(comm->ping_timer, 0);
                    }
                    if (!comm->tx_queue) {
                        comm->tx_queue = create_tx_queue();
                    }
                    if (!comm->tx_task_handle && comm->tx_queue) {
                        TaskHandle_t t = create_task_static(&comm->tx_task_res, tx_task,
                                                            "comm_tx_task", 3072, comm, 11);
                        if (!t) {
                            printf("E: failed to create tx task\n");
                            free_task_resources(&comm->tx_task_res);
                        }
                        comm->tx_task_handle = t;
                    }
                    if (!comm->rx_packet_queue) {
                        comm->rx_packet_queue = create_rx_queue();
                    }
                    if (!comm->protocol_task_handle && comm->rx_packet_queue) {
                        TaskHandle_t t = create_task_static(&comm->protocol_task_res, protocol_task,
                                                            "comm_protocol_t", 4096, comm, 10);
                        if (!t) {
                            printf("E: failed to create protocol task\n");
                            free_task_resources(&comm->protocol_task_res);
                        }
                        comm->protocol_task_handle = t;
                    }
                    unlock_state(comm);
                    printf("Handshake completed!\n");
                    terminal_view_add_text("Handshake completed!\n");
#ifdef CONFIG_HAS_MIC
                    // Headless boards skip boot-time mic capture; start the
                    // GhostLink amplitude stream now that a peer is listening.
                    (void)mic_visualizer_start();
#endif
                }
            }
            break;

        case PACKET_TYPE_HANDSHAKE_ACK:
            if (comm->state == COMM_STATE_HANDSHAKE && comm->role == COMM_ROLE_MASTER) {
                lock_state(comm);
                comm->state = COMM_STATE_CONNECTED;
                // reset seq tracking on new connection
                comm->tx_seq = 0;
                comm->rx_seq_initialized = false;
                comm->rx_expected_seq = 0;
                comm->rx_drop_until_newline = false;
                comm->last_rx_tick = xTaskGetTickCount();
                comm->remote_output_capture = false;
                if (comm->handshake_timer) {
                    xTimerStop(comm->handshake_timer, 0);
                }
                if (comm->command_callback && !comm->command_queue) {
                    comm->command_queue = create_command_queue();
                    if (comm->command_queue && !comm->command_executor_task_handle) {
                        TaskHandle_t t = create_task_static(&comm->command_task_res, command_executor_task,
                                                           "comm_cmd_exec_task", 3072, comm, 5);
                        if (!t) {
                            printf("E: failed to create command executor task\n");
                            free_task_resources(&comm->command_task_res);
                        }
                        comm->command_executor_task_handle = t;
                    }
                }
                if (comm->discovery_timer) {
                    xTimerStop(comm->discovery_timer, 0);
                    xTimerDelete(comm->discovery_timer, 0);
                    comm->discovery_timer = NULL;
                }
                if (!comm->ping_timer) {
                    comm->ping_timer = xTimerCreate("ping_timer", pdMS_TO_TICKS(PING_INTERVAL_MS), pdTRUE, NULL, ping_timer_callback);
                }
                if (comm->ping_timer) {
                    xTimerStart(comm->ping_timer, 0);
                }
                if (!comm->tx_queue) {
                    comm->tx_queue = create_tx_queue();
                }
                if (!comm->tx_task_handle && comm->tx_queue) {
                    TaskHandle_t t = create_task_static(&comm->tx_task_res, tx_task,
                                                        "comm_tx_task", 3072, comm, 11);
                    if (!t) {
                        printf("E: failed to create tx task\n");
                        free_task_resources(&comm->tx_task_res);
                    }
                    comm->tx_task_handle = t;
                }
                if (!comm->rx_packet_queue) {
                    comm->rx_packet_queue = create_rx_queue();
                }
                if (!comm->protocol_task_handle && comm->rx_packet_queue) {
                    TaskHandle_t t = create_task_static(&comm->protocol_task_res, protocol_task,
                                                        "comm_protocol_t", 4096, comm, 10);
                    if (!t) {
                        printf("E: failed to create protocol task\n");
                        free_task_resources(&comm->protocol_task_res);
                    }
                    comm->protocol_task_handle = t;
                }
                unlock_state(comm);
                printf("Handshake completed!\n");
                terminal_view_add_text("Handshake completed!\n");
#ifdef CONFIG_HAS_MIC
                // Headless boards skip boot-time mic capture; start the
                // GhostLink amplitude stream now that a peer is listening.
                (void)mic_visualizer_start();
#endif
            }
            break;

        case PACKET_TYPE_COMMAND:
            if (comm->state == COMM_STATE_CONNECTED && comm->command_callback) {
                comm->remote_output_capture = true;
                char command[MAX_CMD_LEN + 1];
                strncpy(command, (char*)packet->data, MAX_CMD_LEN);
                command[MAX_CMD_LEN] = '\0';
                size_t cmd_len = strlen(command);
                size_t data_start = cmd_len + 1;
                char data[COMM_PACKET_SIZE];
                data[0] = '\0';
                if (packet->length > data_start) {
                    size_t data_len = packet->length - data_start;
                    if (data_len > sizeof(data) - 1) {
                        data_len = sizeof(data) - 1;
                    }
                    memcpy(data, (char*)packet->data + data_start, data_len);
                    data[data_len] = '\0';
                }

                if (!queue_received_command(comm, command, data[0] ? data : NULL)) {
                    (void)send_command_end_packet(COMM_COMMAND_END_STATUS_DISPATCH_FAILED);
                }
            }
            break;

        case PACKET_TYPE_STREAM:
            if (comm->state != COMM_STATE_CONNECTED) {
                comm->stream_ignored_packets++;
                if ((comm->stream_ignored_packets & 0x0F) == 1) {
                    printf("STREAM packet ignored: not connected (ignored=%lu)\n",
                           (unsigned long)comm->stream_ignored_packets);
                }
                break;
            }
            if (packet->length < 1) {
                comm->stream_ignored_packets++;
                if ((comm->stream_ignored_packets & 0x0F) == 1) {
                    printf("STREAM packet ignored: empty payload (ignored=%lu)\n",
                           (unsigned long)comm->stream_ignored_packets);
                }
                break;
            }
            {
                uint8_t channel = packet->data[0];
                if (channel >= COMM_MAX_STREAM_CHANNELS) {
                    comm->stream_ignored_packets++;
                    if ((comm->stream_ignored_packets & 0x0F) == 1) {
                        printf("STREAM packet ignored: invalid channel %d (ignored=%lu)\n",
                               channel, (unsigned long)comm->stream_ignored_packets);
                    }
                    break;
                }
                comm_stream_callback_t cb = comm->stream_handlers[channel];
                const uint8_t* payload = packet->data + 1;
                size_t payload_len = packet->length - 1;
                if (channel == COMM_STREAM_CHANNEL_COMMAND) {
                    handle_command_stream_data(comm, payload, payload_len);
                    break;
                }
                if (s_data_callback && payload_len > 0) {
                    s_data_callback(payload, payload_len, s_data_callback_user_data);
                }
                if (cb) {
                    cb(channel, payload, payload_len, comm->stream_user_data[channel]);
                }
            }
            break;

        case PACKET_TYPE_PING:
            if (comm->state == COMM_STATE_CONNECTED) {
                comm_packet_t pong = {0};
                pong.start_byte = PACKET_START_BYTE;
                pong.type = PACKET_TYPE_PONG;
                pong.length = 0;
                send_packet(&pong);
            }
            break;

        case PACKET_TYPE_PONG:
            // nothing to do; last_rx_tick is already bumped at byte-parse stage
            break;

        case PACKET_TYPE_RESPONSE:
            if (comm->state == COMM_STATE_CONNECTED) {
                // Backward compatibility: header [seq|flags] is optional.
                // Accept as header ONLY if flags contains no unknown bits.
                bool has_header = (packet->length >= 2) && ((packet->data[1] & ~RESP_FLAG_LINE_START) == 0);
                uint8_t seq = 0;
                uint8_t flags = 0;
                const uint8_t* p = packet->data;
                size_t rem = packet->length;
                if (has_header) {
                    seq = packet->data[0];
                    flags = packet->data[1];
                    p = packet->data + 2;
                    rem = packet->length - 2;

                    // detect sequence gaps (e.g., dropped/CRC-failed frame)
                    bool gap = false;
                    if (!comm->rx_seq_initialized) {
                        comm->rx_seq_initialized = true;
                        comm->rx_expected_seq = (uint8_t)(seq + 1);
                    } else {
                        if (seq != comm->rx_expected_seq) {
                            gap = true;
                        }
                        comm->rx_expected_seq = (uint8_t)(seq + 1);
                    }
                    if (gap) {
                        // drop any partial line to avoid merging across a gap
                        comm->response_assembly_len = 0;
                        comm->rx_drop_until_newline = true;
                    }
                    if ((flags & RESP_FLAG_LINE_START) && comm->response_assembly_len > 0) {
                        // start a new line; discard stray partial tail
                        comm->response_assembly_len = 0;
                    }
                    if (flags & RESP_FLAG_LINE_START) {
                        // safe to resume assembling at a clean line start
                        comm->rx_drop_until_newline = false;
                    }
                }
                while (rem > 0) {
                    // If a previous gap was detected, drop bytes until newline boundary
                    if (comm->rx_drop_until_newline) {
                        size_t drop = 0;
                        bool eol_found = false;
                        for (; drop < rem; ++drop) {
                            if (p[drop] == '\n') { // include the delimiter in this chunk
                                eol_found = true;
                                drop++;
                                break;
                            }
                            if (p[drop] == '\r') {
                                if (drop + 1 < rem && p[drop + 1] == '\n') {
                                    drop += 2;
                                } else {
                                    drop += 1;
                                }
                                eol_found = true;
                                break;
                            }
                        }
                        p += drop;
                        rem -= drop;
                        if (!eol_found) {
                            // entire chunk dropped; continue to next packet
                            continue;
                        }
                        // found EOL; resume normal assembly for remaining bytes
                        comm->rx_drop_until_newline = false;
                        if (rem == 0) {
                            break;
                        }
                    }
                    size_t cap = sizeof(comm->response_assembly) - comm->response_assembly_len;
                    if (cap == 0) {
                        // force flush oldest buffered data if no newline present
                        size_t line_len = comm->response_assembly_len;
                        if (line_len > 0) {
                            if (line_len > 255) line_len = 255;
                            char line[256];
                            memcpy(line, comm->response_assembly, line_len);
                            line[line_len] = '\0';
                            printf("ESP Comm Response: %s\n", line);
                            log_response_line(line, line_len, log_buffer, sizeof(log_buffer));
                        }
                        comm->response_assembly_len = 0;
                        cap = sizeof(comm->response_assembly);
                    }
                    size_t to_copy = (rem < cap) ? rem : cap;
                    if (s_response_callback && to_copy > 0) {
                        s_response_callback(p, to_copy, s_response_callback_user_data);
                    }
                    memcpy(comm->response_assembly + comm->response_assembly_len, p, to_copy);
                    comm->response_assembly_len += to_copy;
                    p += to_copy;
                    rem -= to_copy;

                    // flush complete lines (support both '\n' and '\r', coalescing CRLF)
                    size_t start = 0;
                    size_t i = 0;
                    while (i < comm->response_assembly_len) {
                        char c = comm->response_assembly[i];
                        if (c == '\n' || c == '\r') {
                            size_t line_len = i - start;
                            // trim preceding '\r' if the delimiter is '\n'
                            if (c == '\n' && line_len > 0 && comm->response_assembly[i - 1] == '\r') {
                                line_len -= 1;
                            }
                            if (line_len > 255) line_len = 255;
                            char line[256];
                            memcpy(line, comm->response_assembly + start, line_len);
                            line[line_len] = '\0';
                            printf("ESP Comm Response: %s\n", line);
                            log_response_line(line, line_len, log_buffer, sizeof(log_buffer));
                            // advance start; skip optional '\n' after '\r'
                            start = i + 1;
                            if (c == '\r' && start < comm->response_assembly_len && comm->response_assembly[start] == '\n') {
                                start++;
                                i = start;
                                continue;
                            }
                        }
                        i++;
                    }
                    if (start > 0) {
                        size_t tail = comm->response_assembly_len - start;
                        memmove(comm->response_assembly, comm->response_assembly + start, tail);
                        comm->response_assembly_len = tail;
                    }
                }
            }
            break;

        case PACKET_TYPE_COMMAND_END:
            if (comm->state == COMM_STATE_CONNECTED && packet->length >= 1) {
                comm_command_end_callback_t callback = s_command_end_callback;
                void* user_data = s_command_end_callback_user_data;
                if (callback) {
                    callback(packet->data[0], user_data);
                }
            }
            break;

        default:
            printf("Unknown packet type: 0x%02x\n", packet->type);
            break;
    }
}

static void command_executor_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_command_t received_cmd;

    while (comm->initialized) {
        if (xQueueReceive(comm->command_queue, &received_cmd, pdMS_TO_TICKS(100)) == pdPASS) {
            if (comm->command_callback) {
                const char* data = received_cmd.dynamic_data ? received_cmd.dynamic_data : received_cmd.data;
                // Temporarily set the remote command flag to indicate this is a remote command
                bool was_remote = esp_comm_manager_is_remote_command();
                esp_comm_manager_set_remote_command_flag(true);
                comm->command_callback(received_cmd.command, data, comm->callback_user_data);
                /* This only marks command callback return. Commands may have
                 * started asynchronous work that is still running. */
                (void)send_command_end_packet(COMM_COMMAND_END_STATUS_OK);
                // Restore the previous remote command flag state
                esp_comm_manager_set_remote_command_flag(was_remote);
            }
            free_command_payload(&received_cmd);
        }
    }

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void protocol_task(void* arg) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*)arg;
    comm_packet_t packet;

    while(comm->initialized) {
        if (xQueueReceive(comm->rx_packet_queue, &packet, pdMS_TO_TICKS(10)) == pdPASS) {
            handle_received_packet(comm, &packet);
        }
    }

    for (;;) {
        vTaskSuspend(NULL);
    }
}

static void discovery_timer_callback(TimerHandle_t xTimer) {
    if (s_comm_manager && s_comm_manager->state == COMM_STATE_SCANNING) {
        send_discovery_packet();
    }
}

static void ping_timer_callback(TimerHandle_t xTimer) {
    esp_comm_manager_t* comm = s_comm_manager;
    if (!comm) return;
    if (comm->state != COMM_STATE_CONNECTED) return;

    TickType_t now = xTaskGetTickCount();
    if ((now - comm->last_rx_tick) > pdMS_TO_TICKS(LINK_TIMEOUT_MS)) {
        handle_connection_loss(comm, "link timeout");
        return;
    }

    comm_packet_t ping = {0};
    ping.start_byte = PACKET_START_BYTE;
    ping.type = PACKET_TYPE_PING;
    ping.length = 0;
    send_packet(&ping);
}

void esp_comm_manager_init_with_defaults(void) {
    esp_comm_manager_init(DEFAULT_TX_PIN, DEFAULT_RX_PIN, DEFAULT_BAUD_RATE);
}

void esp_comm_manager_init(gpio_num_t tx_pin, gpio_num_t rx_pin, uint32_t baud_rate) {
#if defined(CONFIG_CROWPANEL_ADVANCE_S3_LCD)
    // This profile selects the SD slot, not the mutually exclusive wireless
    // socket. Also block direct CLI initialization with the generic 6/7 pins.
    printf("ESP Comm Manager disabled on CrowPanel Advance S3 (SD/display profile)\n");
    return;
#endif
    ESP_LOGI(TAG, "esp_comm_manager_init: starting, free internal RAM: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (s_comm_manager) {
        printf("Already initialized\n");
        return;
    }

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Pancake") == 0 ||
        strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "MarauderV8") == 0) {
        printf("ESP Comm Manager disabled on %s\n", CONFIG_BUILD_CONFIG_TEMPLATE);
        return;
    }
#endif

    uart_port_t desired_uart = UART_NUM_1;
    gpio_num_t resolved_tx = tx_pin;
    gpio_num_t resolved_rx = rx_pin;
    uint32_t resolved_baud = baud_rate;

#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            resolved_tx = GPIO_NUM_13;
            resolved_rx = GPIO_NUM_14;
        }
        resolved_baud = 460800;
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething2") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            resolved_tx = GPIO_NUM_9;
            resolved_rx = GPIO_NUM_10;
        }
        resolved_baud = 460800;
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Ace_S3") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            resolved_tx = GPIO_NUM_5;
            resolved_rx = GPIO_NUM_4;
        }
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "Ace_C5") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            resolved_tx = GPIO_NUM_6;
            resolved_rx = GPIO_NUM_7;
        }
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "NM-CYD-C5") == 0) {
        desired_uart = UART_NUM_1;
        if (((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) ||
            ((int)tx_pin == 6 && (int)rx_pin == 7)) {
            resolved_tx = GPIO_NUM_11;
            resolved_rx = GPIO_NUM_12;
        }
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0) {
        desired_uart = UART_NUM_1;
        if (((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) ||
            ((int)tx_pin == 6 && (int)rx_pin == 7)) {
            resolved_tx = GPIO_NUM_11;
            resolved_rx = GPIO_NUM_12;
        }
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "ghostlink_p1_core") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            // Straight-through wiring: core TX5 -> peer RX5, peer TX6 -> core RX6.
            resolved_tx = GPIO_NUM_5;
            resolved_rx = GPIO_NUM_6;
        }
    } else if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "ghostlink_p1_peer") == 0) {
        desired_uart = UART_NUM_1;
        if ((int)tx_pin == (int)DEFAULT_TX_PIN && (int)rx_pin == (int)DEFAULT_RX_PIN) {
            resolved_tx = GPIO_NUM_6;
            resolved_rx = GPIO_NUM_5;
        }
    } else {
        desired_uart = UART_NUM_1;
    }
#else
    desired_uart = UART_NUM_1;
#endif

    if (comm_pins_conflict_with_rgb_display(resolved_tx, resolved_rx)) {
        return;
    }

    /* If another subsystem (e.g. GPS NMEA parser) already owns this UART, do not init DualComm. */
    // prefer PSRAM if available, fall back to internal RAM (e.g. ESP32-C6 lacks SPIRAM)
    s_comm_manager = heap_caps_calloc(1, sizeof(esp_comm_manager_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_comm_manager) {
        s_comm_manager = heap_caps_calloc(1, sizeof(esp_comm_manager_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_comm_manager) {
        printf("E: Failed to allocate memory\n");
        return;
    }
    ESP_LOGI(TAG, "esp_comm_manager: struct allocated, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    s_comm_manager->tx_pin = resolved_tx;
    s_comm_manager->rx_pin = resolved_rx;
    s_comm_manager->baud_rate = resolved_baud;

    s_comm_manager->state = COMM_STATE_IDLE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    s_comm_manager->is_executing_remote_cmd = false;
    s_comm_manager->remote_output_capture = false;
    s_comm_manager->uart_driver_installed = false;
    s_comm_manager->use_crc = true;
    s_comm_manager->parse_state = PARSE_STATE_IDLE;
    s_comm_manager->data_bytes_received = 0;
    s_comm_manager->response_assembly_len = 0;
    s_comm_manager->tx_seq = 0;
    s_comm_manager->rx_seq_initialized = false;
    s_comm_manager->rx_expected_seq = 0;
    s_comm_manager->rx_drop_until_newline = false;
    s_comm_manager->last_rx_tick = xTaskGetTickCount();
    s_comm_manager->state_mutex = xSemaphoreCreateMutex();

    s_comm_manager->command_callback = s_pending_callback;
    s_comm_manager->callback_user_data = s_pending_callback_user_data;

    esp_read_mac(s_comm_manager->chip_id, ESP_MAC_WIFI_STA);
    snprintf(s_comm_manager->chip_name, sizeof(s_comm_manager->chip_name), 
             "ESP_%02X%02X%02X", 
             s_comm_manager->chip_id[3], 
             s_comm_manager->chip_id[4], 
             s_comm_manager->chip_id[5]);

    uart_config_t uart_config = {
        .baud_rate = resolved_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_uart_num = desired_uart;

#ifdef CONFIG_USE_TDECK
    printf("ESP Comm Manager disabled on TDECK to avoid UART conflicts\n");
    if (s_comm_manager->state_mutex) {
        vSemaphoreDelete(s_comm_manager->state_mutex);
    }
    free(s_comm_manager);
    s_comm_manager = NULL;
    return;
#endif

    bool keep_serial_manager = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    keep_serial_manager = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0);
#endif

    // Don't deinitialize serial manager on boards whose CLI runs over USB-JTAG.
#ifndef CONFIG_USE_TDECK
    if (!keep_serial_manager) {
        if (serial_manager_get_uart_num() == (int)UART_NUM_1) {
            serial_manager_deinit();
        } else if (serial_manager_get_uart_num() == (int)UART_NUM_0) {
            if ((int)resolved_tx == U0TXD_GPIO_NUM || (int)resolved_rx == U0RXD_GPIO_NUM) {
                serial_manager_deinit();
            }
        }
    }
#endif

    if (uart_share_ensure_installed(s_uart_num, COMM_BUFFER_SIZE * 2, 0, 0) == ESP_OK) {
        if (uart_share_acquire(s_uart_num, UART_SHARE_OWNER_DUALCOMM, pdMS_TO_TICKS(2000)) == ESP_OK) {
            s_comm_manager->uart_driver_installed = true;
        }
    }
    ESP_LOGI(TAG, "esp_comm_manager: UART driver installed, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!s_comm_manager->uart_driver_installed) {
        if (s_comm_manager->state_mutex) {
            vSemaphoreDelete(s_comm_manager->state_mutex);
        }
        free(s_comm_manager);
        s_comm_manager = NULL;
        printf("E: DualComm UART busy\n");
        return;
    }

    uart_flush_input(s_uart_num);

    uart_param_config(s_uart_num, &uart_config);
    uart_set_pin(s_uart_num, resolved_tx, resolved_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_comm_manager->tx_queue = NULL;
    s_comm_manager->rx_packet_queue = NULL;
    s_comm_manager->command_queue = NULL;
    s_comm_manager->command_executor_task_handle = NULL;
    s_comm_manager->tx_task_handle = NULL;
    s_comm_manager->protocol_task_handle = NULL;

    s_comm_manager->initialized = true;

    const uint32_t rx_stack_bytes = 4096;
    s_comm_manager->rx_task_res.stack = alloc_task_stack(rx_stack_bytes);
    s_comm_manager->rx_task_res.tcb = alloc_task_tcb();
    if (s_comm_manager->rx_task_res.stack && s_comm_manager->rx_task_res.tcb) {
        s_comm_manager->rx_task_handle = xTaskCreateStatic(rx_task, "comm_rx_task", rx_stack_bytes,
                                                           s_comm_manager, 12,
                                                           s_comm_manager->rx_task_res.stack,
                                                           s_comm_manager->rx_task_res.tcb);
    }
    if (!s_comm_manager->rx_task_handle) {
        printf("E: failed to create rx task\n");
        free_task_resources(&s_comm_manager->rx_task_res);
    }

    s_comm_manager->discovery_timer = xTimerCreate("discovery_timer", 
                                                   pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                   pdTRUE, NULL, discovery_timer_callback);
    s_comm_manager->handshake_timer = xTimerCreate("handshake_timer",
                                                   pdMS_TO_TICKS(HANDSHAKE_TIMEOUT_MS),
                                                   pdFALSE, s_comm_manager, handshake_timer_callback);

    s_comm_manager->state = COMM_STATE_SCANNING;
    if (s_comm_manager->discovery_timer) {
        xTimerStart(s_comm_manager->discovery_timer, 0);
    }

    ESP_LOGI(TAG, "esp_comm_manager_init: COMPLETE, free internal RAM: %d bytes", 
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    printf("ESP Comm Manager initialized as '%s' on TX:%d RX:%d at %lu baud - Auto-listening for peers\n", 
           s_comm_manager->chip_name, resolved_tx, resolved_rx, (unsigned long)resolved_baud);
}

bool esp_comm_manager_send_stream_wait(uint8_t channel, const uint8_t* data, size_t length, uint32_t wait_ms) {
    if (!s_comm_manager || !s_comm_manager->initialized || !data || length == 0) {
        return false;
    }
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        return false;
    }
    if (channel >= COMM_MAX_STREAM_CHANNELS) {
        return false;
    }

    const uint8_t* p = data;
    size_t remaining = length;
    bool ok = true;

    size_t payload_cap = (PACKET_MAX_PAYLOAD > 1) ? (PACKET_MAX_PAYLOAD - 1) : 0;
    if (payload_cap == 0) {
        return false;
    }

    while (remaining > 0) {
        size_t chunk = remaining < payload_cap ? remaining : payload_cap;

        comm_packet_t packet = {0};
        packet.start_byte = PACKET_START_BYTE;
        packet.type = PACKET_TYPE_STREAM;
        packet.data[0] = channel;
        memcpy(packet.data + 1, p, chunk);
        packet.length = (uint8_t)(chunk + 1);

        if (!send_packet_internal(&packet, pdMS_TO_TICKS(wait_ms))) {
            ok = false;
            break;
        }

        p += chunk;
        remaining -= chunk;
    }

    return ok;
}

bool esp_comm_manager_send_stream(uint8_t channel, const uint8_t* data, size_t length) {
    return esp_comm_manager_send_stream_wait(channel, data, length, 0);
}

bool esp_comm_manager_send_command_line(const char* command_line) {
    if (!command_line || command_line[0] == '\0') {
        return false;
    }
    size_t len = strlen(command_line);
    if (len > COMM_STREAM_COMMAND_MAX) {
        printf("Command line too long for stream transport\n");
        return false;
    }
    return esp_comm_manager_send_stream_wait(COMM_STREAM_CHANNEL_COMMAND,
                                            (const uint8_t*)command_line,
                                            len + 1,
                                            10);
}

bool esp_comm_manager_register_stream_handler(uint8_t channel, comm_stream_callback_t callback, void* user_data) {
    if (!s_comm_manager || channel >= COMM_MAX_STREAM_CHANNELS) {
        return false;
    }

    s_comm_manager->stream_handlers[channel] = callback;
    s_comm_manager->stream_user_data[channel] = user_data;
    return true;
}

bool esp_comm_manager_set_pins(gpio_num_t tx_pin, gpio_num_t rx_pin) {
    if (comm_pins_conflict_with_rgb_display(tx_pin, rx_pin)) {
        return false;
    }
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }

    // allow changing pins in IDLE; if SCANNING, temporarily pause discovery and resume after
    if (s_comm_manager->state != COMM_STATE_IDLE && s_comm_manager->state != COMM_STATE_SCANNING) {
        printf("Cannot change pins during handshake or while connected\n");
        return false;
    }

    bool paused_scanning = false;
    if (s_comm_manager->state == COMM_STATE_SCANNING) {
        if (s_comm_manager->discovery_timer) {
            xTimerStop(s_comm_manager->discovery_timer, 0);
        }
        paused_scanning = true;
    }

    s_comm_manager->tx_pin = tx_pin;
    s_comm_manager->rx_pin = rx_pin;

    bool keep_serial_manager = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    keep_serial_manager = (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "LilyGo T-Dongle-C5") == 0);
#endif

    // Don't deinitialize serial manager on boards whose CLI runs over USB-JTAG.
#ifndef CONFIG_USE_TDECK
    if (!keep_serial_manager) {
        if (serial_manager_get_uart_num() == (int)UART_NUM_1) {
            serial_manager_deinit();
        } else if (serial_manager_get_uart_num() == (int)UART_NUM_0) {
            if ((int)tx_pin == U0TXD_GPIO_NUM || (int)rx_pin == U0RXD_GPIO_NUM) {
                serial_manager_deinit();
            }
        }
    }
#endif

    uart_set_pin(s_uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    printf("Changed pins to TX:%d RX:%d\n", tx_pin, rx_pin);

    if (paused_scanning) {
        if (s_comm_manager->discovery_timer) {
            xTimerStart(s_comm_manager->discovery_timer, 0);
        }
    }
    return true;
}

bool esp_comm_manager_start_discovery(void) {
    if (!s_comm_manager || !s_comm_manager->initialized) {
        printf("E: Not initialized\n");
        return false;
    }

    if (s_comm_manager->state != COMM_STATE_IDLE) {
        printf("Already in discovery or connected\n");
        return false;
    }

    // release heavy resources during discovery
    lock_state(s_comm_manager);
    s_comm_manager->use_crc = true;
    // stop ping timer if running
    if (s_comm_manager->ping_timer) {
        xTimerStop(s_comm_manager->ping_timer, 0);
        xTimerDelete(s_comm_manager->ping_timer, 0);
        s_comm_manager->ping_timer = NULL;
    }
    if (s_comm_manager->protocol_task_handle) {
        vTaskDelete(s_comm_manager->protocol_task_handle);
        s_comm_manager->protocol_task_handle = NULL;
        free_task_resources(&s_comm_manager->protocol_task_res);
    }
    if (s_comm_manager->command_executor_task_handle) {
        vTaskDelete(s_comm_manager->command_executor_task_handle);
        s_comm_manager->command_executor_task_handle = NULL;
        free_task_resources(&s_comm_manager->command_task_res);
    }
    if (s_comm_manager->rx_packet_queue) {
        vQueueDelete(s_comm_manager->rx_packet_queue);
        s_comm_manager->rx_packet_queue = NULL;
    }
    if (s_comm_manager->command_queue) {
        drain_command_queue(s_comm_manager->command_queue);
        vQueueDelete(s_comm_manager->command_queue);
        s_comm_manager->command_queue = NULL;
    }
    release_command_stream(s_comm_manager);
    if (s_comm_manager->tx_task_handle) {
        vTaskDelete(s_comm_manager->tx_task_handle);
        s_comm_manager->tx_task_handle = NULL;
        free_task_resources(&s_comm_manager->tx_task_res);
    }
    if (s_comm_manager->tx_queue) {
        vQueueDelete(s_comm_manager->tx_queue);
        s_comm_manager->tx_queue = NULL;
    }
    unlock_state(s_comm_manager);

    s_comm_manager->state = COMM_STATE_SCANNING;
    if (!s_comm_manager->discovery_timer) {
        s_comm_manager->discovery_timer = xTimerCreate("discovery_timer", 
                                                       pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS),
                                                       pdTRUE, NULL, discovery_timer_callback);
    }
    if (s_comm_manager->discovery_timer) {
        xTimerStart(s_comm_manager->discovery_timer, 0);
    }

    printf("Started discovery as '%s'\n", s_comm_manager->chip_name);
    return true;
}

bool esp_comm_manager_connect_to_peer(const char* peer_name) {
    if (!s_comm_manager || !s_comm_manager->initialized || !peer_name) {
        printf("E: Invalid parameters\n");
        return false;
    }

    if (s_comm_manager->state != COMM_STATE_SCANNING) {
        printf("Not in scanning state\n");
        return false;
    }

    if (s_comm_manager->discovery_timer) {
        xTimerStop(s_comm_manager->discovery_timer, 0);
    }
    lock_state(s_comm_manager);
    s_comm_manager->state = COMM_STATE_HANDSHAKE;
    s_comm_manager->role = COMM_ROLE_MASTER;
    if (s_comm_manager->handshake_timer) {
        xTimerStart(s_comm_manager->handshake_timer, 0);
    }

    send_handshake_request(peer_name);

    printf("Connecting to peer: %s\n", peer_name);
    char log_msg[64];
    snprintf(log_msg, sizeof(log_msg), "I: Connecting to peer: %s\n", peer_name);
    terminal_view_add_text(log_msg);

    unlock_state(s_comm_manager);
    return true;
}

bool esp_comm_manager_send_command(const char* command, const char* data) {
    if (!s_comm_manager || !s_comm_manager->initialized || !command) {
        printf("E: Invalid parameters\n");
        return false;
    }

    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("Not connected\n");
        return false;
    }

    comm_packet_t packet = {0};
    packet.start_byte = PACKET_START_BYTE;
    packet.type = PACKET_TYPE_COMMAND;

    size_t cmd_len = strlen(command);
    if (cmd_len > MAX_CMD_LEN) {
        cmd_len = MAX_CMD_LEN;
    }

    strncpy((char*)packet.data, command, cmd_len);
    ((char*)packet.data)[cmd_len] = '\0';
    packet.length = cmd_len + 1;

    if (data) {
        size_t data_len = strlen(data);
        size_t max_data_len = COMM_PACKET_SIZE - packet.length - 4;
        if (data_len > max_data_len) {
            printf("Command data too long (%u > %u); use command stream transport\n",
                   (unsigned)data_len, (unsigned)max_data_len);
            return false;
        }
        strncpy((char*)packet.data + packet.length, data, data_len);
        packet.length += data_len;
    }

    bool result = send_packet(&packet);
    bool quiet_audio_status = (strcmp(command, "audio") == 0 && data && strncmp(data, "state ", 6) == 0);
    bool quiet_badusb_status = (strcmp(command, "badusb") == 0 && data && strncmp(data, "status ", 7) == 0);
    if (result && !quiet_audio_status && !quiet_badusb_status) {
        printf("Sent command: %s %s\n", command, data ? data : "");
        char log_msg[64];
        snprintf(log_msg, sizeof(log_msg), "I: Sent command: %s %s\n", command, data ? data : "");
        terminal_view_add_text(log_msg);
    }
    return result;
}

bool esp_comm_manager_send_response(const uint8_t* data, size_t length) {
    if (!s_comm_manager || !s_comm_manager->initialized || !data) {
        printf("E: Invalid parameters for send_response\n");
        return false;
    }
    if (s_comm_manager->state != COMM_STATE_CONNECTED) {
        printf("Not connected, can't send response\n");
        return false;
    }
    const uint8_t* p = data;
    size_t remaining = length;
    bool ok = true;
    bool at_line_start = true; // assume start of provided buffer begins a new line
    while (remaining > 0) {
        comm_packet_t packet = {0};
        packet.start_byte = PACKET_START_BYTE;
        packet.type = PACKET_TYPE_RESPONSE;

        // Leave room for [seq|flags] at start of payload
        size_t payload_cap = (PACKET_MAX_PAYLOAD >= 2) ? (PACKET_MAX_PAYLOAD - 2) : 0;
        if (payload_cap == 0) {
            return false;
        }

        // Prefer to end chunks at a newline boundary when possible to avoid
        // splitting human-readable lines across packets.
        size_t search_len = remaining < payload_cap ? remaining : payload_cap;
        size_t chunk = search_len;
        for (size_t i = 0; i < search_len; ++i) {
            if (p[i] == '\n') { // include the delimiter in this chunk
                chunk = i + 1;
            }
        }

        uint8_t flags = at_line_start ? RESP_FLAG_LINE_START : 0;
        uint8_t seq = s_comm_manager->tx_seq++;

        packet.data[0] = seq;
        packet.data[1] = flags;
        memcpy((char*)packet.data + 2, p, chunk);
        packet.length = (uint8_t)(chunk + 2);
        if (!send_packet(&packet)) {
            ok = false;
            break;
        }
        at_line_start = (p[chunk - 1] == '\n');
        p += chunk;
        remaining -= chunk;
    }
    return ok;
}

bool esp_comm_manager_is_connected(void) {
    return s_comm_manager && s_comm_manager->state == COMM_STATE_CONNECTED;
}

comm_state_t esp_comm_manager_get_state(void) {
    return s_comm_manager ? s_comm_manager->state : COMM_STATE_ERROR;
}

void esp_comm_manager_set_command_callback(comm_command_callback_t callback, void* user_data) {
    if (s_comm_manager) {
        s_comm_manager->command_callback = callback;
        s_comm_manager->callback_user_data = user_data;
    } else {
        s_pending_callback = callback;
        s_pending_callback_user_data = user_data;
    }
}

void esp_comm_manager_set_response_callback(comm_response_callback_t callback, void* user_data) {
    s_response_callback = callback;
    s_response_callback_user_data = user_data;
}

void esp_comm_manager_set_data_callback(comm_data_callback_t callback, void* user_data) {
    s_data_callback = callback;
    s_data_callback_user_data = user_data;
}

void esp_comm_manager_set_command_end_callback(comm_command_end_callback_t callback, void* user_data) {
    s_command_end_callback = callback;
    s_command_end_callback_user_data = user_data;
}

void esp_comm_manager_set_remote_command_flag(bool is_remote) {
    if (s_comm_manager) {
        s_comm_manager->is_executing_remote_cmd = is_remote;
    }
}

bool esp_comm_manager_is_remote_command(void) {
    return s_comm_manager && s_comm_manager->is_executing_remote_cmd;
}

bool esp_comm_manager_should_forward_output(void) {
    return s_comm_manager && (s_comm_manager->is_executing_remote_cmd || s_comm_manager->remote_output_capture);
}

bool esp_comm_manager_get_peer_name(char* out, size_t out_len) {
    if (!s_comm_manager || !out || out_len == 0 || s_comm_manager->peer.chip_name[0] == '\0') {
        return false;
    }
    strncpy(out, s_comm_manager->peer.chip_name, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

bool esp_comm_manager_get_pins(gpio_num_t* tx_pin, gpio_num_t* rx_pin) {
    if (!s_comm_manager) {
        return false;
    }
    if (tx_pin) {
        *tx_pin = s_comm_manager->tx_pin;
    }
    if (rx_pin) {
        *rx_pin = s_comm_manager->rx_pin;
    }
    return true;
}

void esp_comm_manager_disconnect(void) {
    if (s_comm_manager) {
        comm_state_t previous_state = s_comm_manager->state;
        if (s_comm_manager->discovery_timer) {
            xTimerStop(s_comm_manager->discovery_timer, 0);
        }
        lock_state(s_comm_manager);
        s_comm_manager->state = COMM_STATE_IDLE;
        s_comm_manager->remote_output_capture = false;
        if (s_comm_manager->handshake_timer) {
            xTimerStop(s_comm_manager->handshake_timer, 0);
        }
        if (s_comm_manager->ping_timer) {
            xTimerStop(s_comm_manager->ping_timer, 0);
            xTimerDelete(s_comm_manager->ping_timer, 0);
            s_comm_manager->ping_timer = NULL;
        }
        unlock_state(s_comm_manager);
        if (previous_state == COMM_STATE_SCANNING) {
            printf("Stopped discovery\n");
        } else if (previous_state == COMM_STATE_CONNECTED || previous_state == COMM_STATE_HANDSHAKE) {
            printf("Disconnected\n");
        } else {
            printf("Idle\n");
        }
    }
}

void esp_comm_manager_deinit(void) {
    if (!s_comm_manager) return;

    peer_storage_manager_reset();

    // Signal shutdown first - tasks check this flag
    s_comm_manager->initialized = false;

    // Stop timers with blocking wait to ensure callbacks complete
    if (s_comm_manager->discovery_timer) {
        xTimerStop(s_comm_manager->discovery_timer, portMAX_DELAY);
    }
    if (s_comm_manager->handshake_timer) {
        xTimerStop(s_comm_manager->handshake_timer, portMAX_DELAY);
    }
    if (s_comm_manager->ping_timer) {
        xTimerStop(s_comm_manager->ping_timer, portMAX_DELAY);
    }

    // Brief delay to let any in-flight timer callbacks or tasks notice initialized=false
    vTaskDelay(pdMS_TO_TICKS(20));

    // Now safe to update state - use short timeout to avoid deadlock
    if (s_comm_manager->state_mutex && 
        xSemaphoreTake(s_comm_manager->state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_comm_manager->state = COMM_STATE_IDLE;
        xSemaphoreGive(s_comm_manager->state_mutex);
    } else {
        s_comm_manager->state = COMM_STATE_IDLE;
    }

    if (s_comm_manager->rx_task_handle) {
        vTaskDelete(s_comm_manager->rx_task_handle);
        s_comm_manager->rx_task_handle = NULL;
    }
    if (s_comm_manager->tx_task_handle) {
        vTaskDelete(s_comm_manager->tx_task_handle);
        s_comm_manager->tx_task_handle = NULL;
    }
    if (s_comm_manager->protocol_task_handle) {
        vTaskDelete(s_comm_manager->protocol_task_handle);
        s_comm_manager->protocol_task_handle = NULL;
    }
    if (s_comm_manager->command_executor_task_handle) {
        vTaskDelete(s_comm_manager->command_executor_task_handle);
        s_comm_manager->command_executor_task_handle = NULL;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    if (s_comm_manager->discovery_timer) {
        xTimerDelete(s_comm_manager->discovery_timer, 0);
    }
    if (s_comm_manager->handshake_timer) {
        xTimerDelete(s_comm_manager->handshake_timer, 0);
    }
    if (s_comm_manager->ping_timer) {
        xTimerDelete(s_comm_manager->ping_timer, 0);
        s_comm_manager->ping_timer = NULL;
    }

    free_task_resources(&s_comm_manager->rx_task_res);
    free_task_resources(&s_comm_manager->tx_task_res);
    free_task_resources(&s_comm_manager->protocol_task_res);
    free_task_resources(&s_comm_manager->command_task_res);

    if (s_comm_manager->rx_packet_queue) {
        vQueueDelete(s_comm_manager->rx_packet_queue);
    }
    if (s_comm_manager->tx_queue) {
        vQueueDelete(s_comm_manager->tx_queue);
    }
    if (s_comm_manager->command_queue) {
        drain_command_queue(s_comm_manager->command_queue);
        vQueueDelete(s_comm_manager->command_queue);
    }
    release_command_stream(s_comm_manager);
    if (s_comm_manager->state_mutex) {
        vSemaphoreDelete(s_comm_manager->state_mutex);
    }

    // delete UART driver last to avoid tasks accessing a torn-down driver
    if (s_comm_manager->uart_driver_installed) {
        (void)uart_wait_tx_done(s_uart_num, pdMS_TO_TICKS(50));
        (void)uart_flush_input(s_uart_num);
        (void)uart_share_release(s_uart_num, UART_SHARE_OWNER_DUALCOMM);
        s_comm_manager->uart_driver_installed = false;
    }

    free(s_comm_manager);
    s_comm_manager = NULL;
    printf("ESP Comm Manager de-initialized\n");
}

static void handshake_timer_callback(TimerHandle_t xTimer) {
    esp_comm_manager_t* comm = (esp_comm_manager_t*) pvTimerGetTimerID(xTimer);
    if (!comm || !comm->initialized) return;
    lock_state(comm);
    if (comm->state == COMM_STATE_HANDSHAKE) {
        printf("Handshake timeout\n");
        terminal_view_add_text("W: Handshake timeout\n");
        comm->state = COMM_STATE_SCANNING;
        if (comm->discovery_timer) {
            xTimerStart(comm->discovery_timer, 0);
        }
    }
    unlock_state(comm);
}

static void handle_connection_loss(esp_comm_manager_t* comm, const char* reason) {
    if (!comm || !comm->initialized) return;
    lock_state(comm);
    if (comm->state != COMM_STATE_CONNECTED) {
        unlock_state(comm);
        return;
    }
    printf("Connection lost (%s)\n", reason ? reason : "unknown");
    terminal_view_add_text("W: Connection lost, restarting discovery\n");
#ifdef CONFIG_HAS_MIC
    bool mic_was_streaming = mic_visualizer_is_running();
    unlock_state(comm);
    if (mic_was_streaming) {
        (void)mic_visualizer_stop();
    }
    lock_state(comm);
    if (comm->state != COMM_STATE_CONNECTED) {
        unlock_state(comm);
        return;
    }
#endif

    comm->state = COMM_STATE_SCANNING;
    comm->remote_output_capture = false;
    peer_storage_manager_reset();

    // Stop ping timer; keep it allocated and restart on next connection.
    if (comm->ping_timer) {
        xTimerStop(comm->ping_timer, 0);
    }

    // Keep protocol/queue resources alive to avoid racey teardown under load.
    // Just clear pending work so reconnect starts cleanly.
    if (comm->rx_packet_queue) {
        xQueueReset(comm->rx_packet_queue);
    }
    if (comm->command_queue) {
        drain_command_queue(comm->command_queue);
        xQueueReset(comm->command_queue);
    }
    reset_command_stream(comm);
    if (comm->tx_queue) {
        xQueueReset(comm->tx_queue);
    }

    // restart discovery timer
    if (!comm->discovery_timer) {
        comm->discovery_timer = xTimerCreate("discovery_timer", pdMS_TO_TICKS(DISCOVERY_INTERVAL_MS), pdTRUE, NULL, discovery_timer_callback);
    }
    if (comm->discovery_timer) {
        xTimerStart(comm->discovery_timer, 0);
    }
    unlock_state(comm);
}
