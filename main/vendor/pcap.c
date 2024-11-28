#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sys/time.h"
#include "vendor/pcap.h"
#include "driver/uart.h"
#include <errno.h>
#include "core/utils.h"
#include <sys/stat.h>
#include <arpa/inet.h>
#include "managers/sd_card_manager.h"

#define RADIOTAP_HEADER_LEN 8
static const char *PCAP_TAG = "PCAP";

esp_err_t pcap_write_global_header(FILE* f) {
    // Initialize PCAP file header with standard values
    pcap_global_header_t global_header;
    global_header.magic_number = 0xa1b2c3d4;    // Standard PCAP magic
    global_header.version_major = 2;
    global_header.version_minor = 4;
    global_header.thiszone = 0;                 // Use UTC timezone
    global_header.sigfigs = 0;
    global_header.snaplen = 65535;              // Max packet length
    global_header.network = 127;                // 802.11 radio format

    if (f == NULL) {
        // When no file is open, write to serial
        const char* mark_begin = "[BUF/BEGIN]";
        const size_t mark_begin_len = strlen(mark_begin);
        const char* mark_close = "[BUF/CLOSE]";
        const size_t mark_close_len = strlen(mark_close);

        uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);
        uart_write_bytes(UART_NUM_0, (const char*)&global_header, sizeof(global_header));
        uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);
        uart_write_bytes(UART_NUM_0, "\n", 1);
        return ESP_OK;
    } else {
        size_t written = fwrite(&global_header, 1, sizeof(global_header), f);
        return (written == sizeof(global_header)) ? ESP_OK : ESP_FAIL;
    }
}

void get_next_pcap_file_name(char *file_name_buffer, const char* base_name) {
    int next_index = get_next_pcap_file_index(base_name);
    snprintf(file_name_buffer, MAX_FILE_NAME_LENGTH, "/mnt/ghostesp/pcaps/%s_%d.pcap", base_name, next_index);
}

esp_err_t pcap_file_open(const char* base_file_name) {
    char file_name[MAX_FILE_NAME_LENGTH];
    
    if (sd_card_exists("/mnt/ghostesp/pcaps"))
    {
        get_next_pcap_file_name(file_name, base_file_name);
        pcap_file = fopen(file_name, "wb");
    }
    
    esp_err_t ret = pcap_write_global_header(pcap_file);
    if (ret != ESP_OK) {
        ESP_LOGE(PCAP_TAG, "Failed to write PCAP global header.");
        fclose(pcap_file);
        pcap_file = NULL;
        return ret;
    }

    ESP_LOGI(PCAP_TAG, "PCAP file %s opened and global header written.", file_name);
    return ESP_OK;
}

static bool is_valid_tag_length(uint8_t tag_num, uint8_t tag_len) {
    switch (tag_num) {
        case 9:   // Hopping Pattern Table
            return tag_len >= 4;
        case 32:  // Power Constraint
            return tag_len == 1;
        case 33:  // Power Capability
            return tag_len == 2;
        case 35:  // TPC Report
            return tag_len == 2;
        case 36:  // Channels
            return tag_len >= 3;
        case 37:  // Channel Switch Announcement
            return tag_len == 3;
        case 38:  // Measurement Request
            return tag_len >= 3;
        case 39:  // Measurement Report
            return tag_len >= 3;
        case 41:  // IBSS DFS
            return tag_len >= 7;
        case 45:  // HT Capabilities
            return tag_len == 26;
        case 47:  // HT Operation
            return tag_len >= 22;
        case 48:  // RSN
            return tag_len >= 2;
        case 51:  // AP Channel Report
            return tag_len >= 3;
        case 61:  // HT Operation
            return tag_len >= 22;
        case 74:  // Overlapping BSS Scan Parameters
            return tag_len == 14;
        case 107: // Interworking
            return tag_len >= 1;
        case 127: // Extended Capabilities
            return tag_len >= 1;
        case 142: // Page Slice
            return tag_len >= 3;
        case 191: // VHT Capabilities
            return tag_len == 12;
        case 192: // VHT Operation
            return tag_len >= 5;
        case 195: // VHT Transmit Power Envelope
            return tag_len >= 2;
        case 216: // Target Wake Time
            return tag_len >= 4;
        case 221: // Vendor Specific
            return tag_len >= 3;
        case 232: // DMG Operation
            return tag_len >= 5;
        case 235: // S1G Beacon Compatibility
            return tag_len >= 7;
        case 255: // Extended tag
            return tag_len >= 1;
        default:
            return true;  // All other tags can have any length
    }
}
static bool is_valid_beacon_fixed_params(const uint8_t* frame, size_t offset, size_t max_len) {
    if (offset + 12 > max_len) return false;
    
    // Skip timestamp (8 bytes) as it can be any value
    
    // Check beacon interval (2 bytes) - typically between 1-65535
    uint16_t beacon_interval = frame[offset + 8] | (frame[offset + 9] << 8);
    if (beacon_interval == 0) return false;
    
    // Check capability info (2 bytes) - must have some bits set
    uint16_t capability = frame[offset + 10] | (frame[offset + 11] << 8);
    if ((capability & 0x0001) == 0 && (capability & 0x0002) == 0) {
        // At least one of ESS or IBSS must be set
        return false;
    }
    
    return true;
}

static size_t calculate_wifi_frame_length(const uint8_t* frame, size_t max_len) {
    if (frame == NULL || max_len < 2) return 0;
    
    uint16_t frame_control = frame[0] | (frame[1] << 8);
    uint8_t type = (frame_control >> 2) & 0x3;
    uint8_t subtype = (frame_control >> 4) & 0xF;
    uint8_t to_ds = (frame_control >> 8) & 0x1;
    uint8_t from_ds = (frame_control >> 9) & 0x1;
    
    size_t length = 24;  // Basic MAC header length
    
    switch (type) {
        case 0x0:  // Management frames
            if (max_len < length) return max_len;
            
            // Handle fixed parameters
            switch (subtype) {
                case 0x8:  // Beacon
                case 0x5:  // Probe Response
                    if (max_len < length + 12) return length;
                    if (subtype == 0x8 && !is_valid_beacon_fixed_params(frame, length, max_len)) {
                        return length;
                    }
                    length += 12;
                    break;
                    
                case 0x0:  // Association Request
                    if (max_len < length + 4) return length;
                    length += 4;
                    break;
                    
                case 0xb:  // Authentication
                    if (max_len < length + 6) return length;
                    length += 6;
                    break;
                    
                case 0xd:  // Action
                    if (max_len < length + 1) return length;
                    length += 1;
                    break;
            }

            // Process tagged parameters
            if (max_len > length) {
                size_t pos = length;
                size_t remaining = max_len - pos;
                
                while (pos + 2 <= max_len) {
                    uint8_t tag_num = frame[pos];
                    uint8_t tag_len = frame[pos + 1];

                    if (pos + 2 + tag_len > max_len) {
                        length = pos;
                        break;
                    }

                    if (!is_valid_tag_length(tag_num, tag_len)) {
                        length = pos;
                        break;
                    }

                    pos += 2 + tag_len;
                    length = pos;
                }
            }
            break;
            
        case 0x1:  // Control frames
            switch (subtype) {
                case 0xB:  // RTS
                    length = 16;
                    break;
                case 0xC:  // CTS
                case 0xD:  // ACK
                    length = 10;
                    break;
                default:
                    length = 16;
            }
            break;
            
        case 0x2:  // Data frames
            if (to_ds && from_ds) {
                if (max_len < 30) return max_len;
                length = 30;
            }
            
            if ((subtype & 0x8) != 0) {  // QoS data
                if (max_len < length + 2) return length;
                length += 2;
            }
            
            if (max_len > length) {
                size_t data_len = max_len - length;
                if (data_len >= 8) {  // Minimum LLC/SNAP header
                    length = max_len;
                }
            }
            break;
    }
    
    return (length <= max_len) ? length : max_len;
}

esp_err_t pcap_write_packet_to_buffer(const void* packet, size_t length) {
    if (packet == NULL || length < 2) {
        ESP_LOGE(PCAP_TAG, "Invalid packet data");
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t* frame = (const uint8_t*)packet;
    size_t valid_length = calculate_wifi_frame_length(frame, length);
    
    if (valid_length == 0) {
        ESP_LOGE(PCAP_TAG, "Invalid frame length calculated");
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    pcap_packet_header_t packet_header;

    // Add radiotap header length to packet length
    size_t total_length = valid_length + RADIOTAP_HEADER_LEN;
    packet_header.ts_sec = tv.tv_sec;
    packet_header.ts_usec = tv.tv_usec;
    packet_header.incl_len = total_length;
    packet_header.orig_len = total_length;

    size_t total_packet_size = sizeof(packet_header) + total_length;
    
    if (total_packet_size > BUFFER_SIZE) {
        ESP_LOGE(PCAP_TAG, 
            "Packet too large: %zu", 
            total_packet_size);
        return ESP_ERR_NO_MEM;
    }

    if (buffer_offset + total_packet_size > BUFFER_SIZE) {
        ESP_LOGI(PCAP_TAG, 
            "Buffer full, flushing...");
        esp_err_t ret = pcap_flush_buffer_to_file();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    // Write packet header
    memcpy(pcap_buffer + buffer_offset, &packet_header, sizeof(packet_header));
    buffer_offset += sizeof(packet_header);

    // Write radiotap header
    uint8_t radiotap_header[RADIOTAP_HEADER_LEN] = {
        0x00, 0x00,  // Version 0
        0x08, 0x00,  // Header length
        0x00, 0x00, 0x00, 0x00  // Present flags
    };
    memcpy(pcap_buffer + buffer_offset, radiotap_header, RADIOTAP_HEADER_LEN);
    buffer_offset += RADIOTAP_HEADER_LEN;

    // Write actual packet data
    memcpy(pcap_buffer + buffer_offset, packet, valid_length);
    buffer_offset += valid_length;

    ESP_LOGD(PCAP_TAG, "Added packet: size=%zu, buffer at: %zu", valid_length, buffer_offset);
    
    return ESP_OK;
}

esp_err_t pcap_flush_buffer_to_file() {
    if (pcap_file == NULL) {
        ESP_LOGE(PCAP_TAG, 
            "No PCAP file open. Using Serial...");
        
        // Write buffer markers and content to serial
        const char* mark_begin = "[BUF/BEGIN]";
        const size_t mark_begin_len = strlen(mark_begin);
        const char* mark_close = "[BUF/CLOSE]";
        const size_t mark_close_len = strlen(mark_close);

        uart_write_bytes(UART_NUM_0, mark_begin, mark_begin_len);

        
        uart_write_bytes(UART_NUM_0, (const char*)pcap_buffer, buffer_offset);

        
        uart_write_bytes(UART_NUM_0, mark_close, mark_close_len);

        
        const char* newline = "\n";
        uart_write_bytes(UART_NUM_0, newline, 1);

        
        buffer_offset = 0;
        return ESP_OK;
    }

    
    size_t written = fwrite(pcap_buffer, 1, buffer_offset, pcap_file);
    if (written != buffer_offset) {
        ESP_LOGE(PCAP_TAG, "Failed to write buffer to file.");
        return ESP_FAIL;
    }

    ESP_LOGI(PCAP_TAG, "Flushed %zu bytes to PCAP file.", buffer_offset);

    
    buffer_offset = 0;

    return ESP_OK;
}

void pcap_file_close() {
    if (pcap_file != NULL) {
        if (buffer_offset > 0) {
            ESP_LOGI(PCAP_TAG, "Flushing remaining buffer before closing file.");
            pcap_flush_buffer_to_file();
        }

        // Close the file
        fclose(pcap_file);
        pcap_file = NULL;
        ESP_LOGI(PCAP_TAG, "PCAP file closed.");
    }
}