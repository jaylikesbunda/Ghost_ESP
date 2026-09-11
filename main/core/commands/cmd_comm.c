// cmd_comm.c
// ESP peer communication commands.

#include "core/commands.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "core/ghostlink_bench.h"
#include "managers/ghostscript_runtime.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "sdkconfig.h"
#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet/eth_comm_handler.h"
#endif
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/audio_stream_manager.h"
#endif
#include "driver/gpio.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void simulateCommand(const char *commandString);

void handle_comm_discovery(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    
    if (state == COMM_STATE_SCANNING) {
        glog("Already in discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Scanning");
        return;
    }
    
    if (esp_comm_manager_start_discovery()) {
        glog("Started discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Discover");
    } else {
        glog("Failed to start discovery. Check if already connected.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_connect(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: commconnect <peer_name>\n");
        glog("Example: commconnect ESP_A1B2C3\n");
        status_display_show_status("CommConn Use");
        return;
    }
    
    if (esp_comm_manager_connect_to_peer(argv[1])) {
        glog("Attempting to connect to peer: %s\n", argv[1]);
        status_display_show_status("Comm Connect");
    } else {
        glog("Failed to connect. Make sure you're in discovery mode first.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_send(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: commsend <command> [data]\n");
        glog("Example: commsend hello world\n");
        glog("Example: commsend scanap\n");
        status_display_show_status("CommSend Use");
        return;
    }
    
    if (!esp_comm_manager_is_connected()) {
        glog("Not connected to any peer. Use 'commdiscovery' and 'commconnect' first.\n");
        status_display_show_status("Comm NotConn");
        return;
    }
    
    char data_buffer[256] = {0};
    if (argc > 2) {
        int offset = 0;
        for (int i = 2; i < argc; i++) {
            int remaining = sizeof(data_buffer) - offset;
            int written = snprintf(data_buffer + offset, remaining, "%s ", argv[i]);
            if (written >= remaining) {
                glog("W: Command data truncated.\n");
                break;
            }
            offset += written;
        }
        if (offset > 0) {
            data_buffer[offset - 1] = '\0'; // Remove trailing space
        }
    }

    const char* command = argv[1];
    const char* data = (argc > 2) ? data_buffer : NULL;

    if (esp_comm_manager_send_command(command, data)) {
        if (data && data[0] != '\0') {
            glog("Command sent: %s %s\n", command, data);
        } else {
            glog("Command sent: %s\n", command);
        }
        status_display_show_status("Comm Sent");
    } else {
        glog("Failed to send command.\n");
        status_display_show_status("Comm Fail");
    }
}

void handle_comm_status(int argc, char **argv) {
    comm_state_t state = esp_comm_manager_get_state();
    const char* state_str;
    
    switch(state) {
        case COMM_STATE_IDLE: state_str = "IDLE"; break;
        case COMM_STATE_SCANNING: state_str = "SCANNING"; break;
        case COMM_STATE_HANDSHAKE: state_str = "HANDSHAKE"; break;
        case COMM_STATE_CONNECTED: state_str = "CONNECTED"; break;
        case COMM_STATE_ERROR: state_str = "ERROR"; break;
        default: state_str = "UNKNOWN"; break;
    }
    
    glog("Communication Status: %s\n", state_str);
    
    if (state == COMM_STATE_SCANNING) {
        glog("Already in discovery mode. Listening for peers...\n");
        status_display_show_status("Comm Scanning");
        return;
    }
    
    if (state == COMM_STATE_CONNECTED) {
        glog("Connected to peer. Ready to send commands.\n");
        status_display_show_status("Comm Connected");
        return;
    }
    
    glog("Not connected. Use 'commdiscovery' to find peers.\n");
    status_display_show_status("Comm Idle");
}

void handle_comm_disconnect(int argc, char **argv) {
    esp_comm_manager_disconnect();
    glog("Disconnected from peer.\n");
    status_display_show_status("Comm Closed");
}

void handle_comm_setpins(int argc, char **argv) {
    if (argc != 3) {
        glog("Usage: commsetpins <tx_pin> <rx_pin>\n");
        glog("Example: commsetpins 4 5\n");
        status_display_show_status("Pins Usage");
        return;
    }
    
    int tx_pin = atoi(argv[1]);
    int rx_pin = atoi(argv[2]);
    
    if (tx_pin < 0 || tx_pin > 48 || rx_pin < 0 || rx_pin > 48) {
        glog("Invalid pin numbers. Must be between 0-48.\n");
        status_display_show_status("Pins Invalid");
        return;
    }
    
    if (esp_comm_manager_set_pins((gpio_num_t)tx_pin, (gpio_num_t)rx_pin)) {
        settings_set_esp_comm_pins(&G_Settings, tx_pin, rx_pin);
        settings_save(&G_Settings);
        
        glog("Communication pins changed to TX:%d RX:%d and saved to NVS\n", tx_pin, rx_pin);
        status_display_show_status("Pins Updated");
    } else {
        glog("Failed to change pins. Make sure not connected or scanning.\n");
        status_display_show_status("Pins Failed");
    }
}

static void comm_command_callback(const char* command, const char* data, void* user_data) {
    char comm_payload[160];
    snprintf(comm_payload, sizeof(comm_payload), "%s|%s",
             command ? command : "", data ? data : "");
    ghostscript_emit_event_escaped("comm_command", comm_payload);

    if (ghostlink_bench_handle_command(command, data)) {
        return;
    }

#ifdef CONFIG_WITH_ETHERNET
    if (eth_comm_handler_handle_command(command, data)) {
        return;
    }

    if (strcmp(command, "stop") == 0) {
        eth_cmd_set_scan_cancel(true);
    }
#endif
    
#ifdef CONFIG_HAS_AUDIO_PLAYER
    if (strcmp(command, "audio") == 0 && data && strncmp(data, "state ", 6) == 0) {
        char *end = NULL;
        unsigned long fill = strtoul(data + 6, &end, 10);
        if (end && *end == ' ') {
            char *end2 = NULL;
            unsigned long capacity = strtoul(end + 1, &end2, 10);
            unsigned long played_ms = 0;
            if (end2 && *end2 == ' ') {
                played_ms = strtoul(end2 + 1, NULL, 10);
            }
            audio_stream_manager_update_receiver_status((size_t)fill, (size_t)capacity, (uint32_t)played_ms);
        }
        return;
    }
#endif

    char stack_command[128];
    char *full_command = stack_command;
    size_t data_len = (data && strlen(data) > 0) ? strlen(data) : 0;
    size_t needed = strlen("peer:") + strlen(command) + (data_len ? 1 + data_len : 0) + 1;
    if (needed > sizeof(stack_command)) {
        full_command = (char *)malloc(needed);
        if (!full_command) {
            glog("Failed to allocate remote command buffer\n");
            return;
        }
    }

    if (data && strlen(data) > 0) {
        snprintf(full_command, needed, "peer:%s %s", command, data);
    } else {
        snprintf(full_command, needed, "peer:%s", command);
    }
    
    simulateCommand(full_command);
    if (full_command != stack_command) {
        free(full_command);
    }
}

void cmd_comm_register_callback(void) {
    esp_comm_manager_set_command_callback(comm_command_callback, NULL);
}
