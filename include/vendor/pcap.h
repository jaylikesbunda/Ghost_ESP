
#ifndef PCAP_HEADER
#define PCAP_HEADER

#include <stdio.h>
#include <stdint.h>
#include "esp_vfs_fat.h"

#define PCAP_GLOBAL_HEADER_SIZE 24
#define PCAP_PACKET_HEADER_SIZE 16

// PCAP global header structure
typedef struct {
    uint32_t magic_number;   // Magic number (0xa1b2c3d4)
    uint16_t version_major;  // Major version (usually 2)
    uint16_t version_minor;  // Minor version (usually 4)
    int32_t  thiszone;       // GMT to local correction (usually 0)
    uint32_t sigfigs;        // Accuracy of timestamps
    uint32_t snaplen;        // Max length of captured packets
    uint32_t network;        // Data link type (DLT_IEEE802_11 for Wi-Fi)
} pcap_global_header_t;

// PCAP packet header structure
typedef struct {
    uint32_t ts_sec;   // Timestamp seconds
    uint32_t ts_usec;  // Timestamp microseconds
    uint32_t incl_len; // Number of octets of packet saved in file
    uint32_t orig_len; // Actual length of packet (on the wire)
} pcap_packet_header_t;


#define MAX_FILE_NAME_LENGTH 528
#define BUFFER_SIZE 4096


static uint8_t pcap_buffer[BUFFER_SIZE];
static size_t buffer_offset = 0;
static FILE *pcap_file = NULL;



esp_err_t pcap_write_global_header(FILE* f);
esp_err_t pcap_file_open(const char* base_file_name);
esp_err_t pcap_write_packet_to_buffer(const void* packet, size_t length);
esp_err_t pcap_flush_buffer_to_file();
void pcap_file_close();



#endif