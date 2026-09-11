#ifndef ESP_COMM_MANAGER_H
#define ESP_COMM_MANAGER_H

#include "driver/gpio.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DEFAULT_BAUD_RATE 115200
#define COMM_COMMAND_END_STATUS_OK 0
#define COMM_COMMAND_END_STATUS_DISPATCH_FAILED 1

typedef enum {
    COMM_STATE_IDLE,
    COMM_STATE_SCANNING,
    COMM_STATE_HANDSHAKE,
    COMM_STATE_CONNECTED,
    COMM_STATE_ERROR
} comm_state_t;

typedef enum {
    COMM_ROLE_MASTER,
    COMM_ROLE_SLAVE
} comm_role_t;

typedef struct {
    uint8_t chip_id[6];
    char chip_name[32];
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    comm_state_t state;
    comm_role_t role;
} comm_peer_t;

typedef void (*comm_command_callback_t)(const char* command, const char* data, void* user_data);
typedef void (*comm_response_callback_t)(const uint8_t* data, size_t length, void* user_data);
typedef void (*comm_data_callback_t)(const uint8_t* data, size_t length, void* user_data);
/* Signals that the remote command callback returned. This is a dispatch
 * boundary, not completion of asynchronous work started by the command. */
typedef void (*comm_command_end_callback_t)(uint8_t status, void* user_data);


#define COMM_MAX_STREAM_CHANNELS 13
#define COMM_STREAM_CHANNEL_COMMAND 0
#define COMM_STREAM_CHANNEL_KEYBOARD 1
#define COMM_STREAM_CHANNEL_BADUSB  2
#define COMM_STREAM_CHANNEL_WARDRIVE 3
#define COMM_STREAM_CHANNEL_GPS 4
#define COMM_STREAM_CHANNEL_NRF24 5
#define COMM_STREAM_CHANNEL_ETHERNET      6
#define COMM_STREAM_CHANNEL_MIC_AMPLITUDE 7  // MIC audio data for RGB visualizer
#define COMM_STREAM_CHANNEL_SUBGHZ 8
#define COMM_STREAM_CHANNEL_AUDIO 9          // MP3 audio stream for DAC playback
#define COMM_STREAM_CHANNEL_OTA 10           // Firmware image bytes for GhostLink peer flashing
#define COMM_STREAM_CHANNEL_STORAGE 11       // Peer-backed file IO RPC for SD-less boards
#define COMM_STREAM_CHANNEL_BENCH 12         // Throughput benchmark payload (glbench)

typedef void (*comm_stream_callback_t)(uint8_t channel, const uint8_t* data, size_t length, void* user_data);

/* Runtime link telemetry. Read-only snapshot of the counters the comm manager
 * already tracks internally plus live queue depth, for diagnostics such as the
 * glbench throughput benchmark. Values are a point-in-time sample. */
typedef struct {
    uint32_t tx_dropped_packets;
    uint32_t rx_queue_dropped_packets;
    uint32_t rx_crc_error_count;
    uint32_t stream_ignored_packets;
    size_t   rx_buffer_high_watermark;
    uint32_t rx_high_water_alerts;
    unsigned tx_queue_waiting;
    unsigned rx_queue_free;
    uint32_t baud;
} esp_comm_manager_stats_t;

void esp_comm_manager_init_with_defaults(void);
void esp_comm_manager_init(gpio_num_t tx_pin, gpio_num_t rx_pin, uint32_t baud_rate);
bool esp_comm_manager_set_pins(gpio_num_t tx_pin, gpio_num_t rx_pin);
bool esp_comm_manager_start_discovery(void);
bool esp_comm_manager_connect_to_peer(const char* peer_name);
bool esp_comm_manager_send_command(const char* command, const char* data);
bool esp_comm_manager_send_command_line(const char* command_line);
bool esp_comm_manager_is_connected(void);
comm_state_t esp_comm_manager_get_state(void);
void esp_comm_manager_set_command_callback(comm_command_callback_t callback, void* user_data);
void esp_comm_manager_set_response_callback(comm_response_callback_t callback, void* user_data);
void esp_comm_manager_set_data_callback(comm_data_callback_t callback, void* user_data);
void esp_comm_manager_set_command_end_callback(comm_command_end_callback_t callback, void* user_data);
void esp_comm_manager_disconnect(void);
void esp_comm_manager_deinit(void);
bool esp_comm_manager_send_response(const uint8_t* data, size_t length);
void esp_comm_manager_set_remote_command_flag(bool is_remote);
bool esp_comm_manager_is_remote_command(void);
bool esp_comm_manager_should_forward_output(void);

bool esp_comm_manager_send_stream(uint8_t channel, const uint8_t* data, size_t length);
bool esp_comm_manager_send_stream_wait(uint8_t channel, const uint8_t* data, size_t length, uint32_t wait_ms);
bool esp_comm_manager_register_stream_handler(uint8_t channel, comm_stream_callback_t callback, void* user_data);
bool esp_comm_manager_get_peer_name(char* out, size_t out_len);
bool esp_comm_manager_get_pins(gpio_num_t* tx_pin, gpio_num_t* rx_pin);

/* Snapshot the link telemetry counters and live queue depth. Returns false if
 * the comm manager is not initialised. Purely additive; changes no behaviour. */
bool esp_comm_manager_get_stats(esp_comm_manager_stats_t* out);

/* Effective (resolved) GhostLink UART baud rate, or 0 if not initialised. */
uint32_t esp_comm_manager_get_baud(void);

#endif // ESP_COMM_MANAGER_H
