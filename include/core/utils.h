// utils.h

#ifndef UTILS_H
#define UTILS_H

#include <esp_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_netif.h"

// ============================================================================
// String/Message Utilities
// ============================================================================

const char *wrap_message(const char *message, const char *file, int line);

void url_decode(char *decoded, const char *encoded);

int get_query_param_value(const char *query, const char *key, char *value, size_t value_size);

bool str_copy_upper(char *dst, size_t dst_size, const char *src);

// ============================================================================
// RGB/Brightness Utilities
// ============================================================================

void scale_grb_by_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float brightness);

void scale_grb_by_neopixel_brightness(uint8_t *g, uint8_t *r, uint8_t *b, float base_brightness, uint8_t max_brightness_percent);

// ============================================================================
// Task/Context Utilities
// ============================================================================

bool is_in_task_context(void);

// ============================================================================
// File Utilities
// ============================================================================

int get_next_pcap_file_index(const char *base_name);

int get_next_csv_file_index(const char *base_name);

int get_next_file_index(const char *dir_path, const char *base_name, const char *extension);

// ============================================================================
// Logging Utilities
// ============================================================================

void log_heap_status(const char *tag, const char *event);

// ============================================================================
// Network/MAC Utilities
// ============================================================================

/**
 * @brief Format a MAC address as a string
 * 
 * @param mac Pointer to 6-byte MAC address
 * @param buffer Output buffer for formatted string
 * @param buffer_len Size of output buffer (minimum 18 bytes)
 * @param uppercase If true, use uppercase hex digits
 */
void format_mac_address(const uint8_t *mac, char *buffer, size_t buffer_len, bool uppercase);

/**
 * @brief Build an IP address string from subnet prefix and host number
 * 
 * @param buffer Output buffer for IP string
 * @param size Size of output buffer
 * @param prefix Subnet prefix (e.g., "192.168.1.")
 * @param host Host number (e.g., 1-254)
 */
void build_ip_string(char *buffer, size_t size, const char *prefix, int host);

// ============================================================================
// WiFi Network Utilities
// ============================================================================

/**
 * @brief Get the WiFi STA network interface
 * 
 * @return esp_netif_t* Pointer to STA interface, or NULL if not available
 */
esp_netif_t *get_wifi_sta_netif(void);

/**
 * @brief Check if WiFi is connected in STA mode
 * 
 * @return true if connected to an AP, false otherwise
 */
bool is_wifi_sta_connected(void);

/**
 * @brief Get the device's own IP and MAC address
 * 
 * @param netif Network interface (use get_wifi_sta_netif())
 * @param ip_info Output buffer for IP information
 * @param mac Output buffer for MAC address (6 bytes)
 * @return true on success, false on failure
 */
bool get_own_ip_and_mac(esp_netif_t *netif, esp_netif_ip_info_t *ip_info, uint8_t *mac);

// ============================================================================
// Byte/Buffer Utilities
// ============================================================================

/**
 * @brief Read a 16-bit little-endian value from a byte array
 * 
 * @param data Pointer to at least 2 bytes
 * @return uint16_t The 16-bit value
 */
uint16_t read_u16_le(const uint8_t *data);

/**
 * @brief Read a 32-bit little-endian value from a byte array
 * 
 * @param data Pointer to at least 4 bytes
 * @return uint32_t The 32-bit value
 */
uint32_t read_u32_le(const uint8_t *data);

/**
 * @brief Parse device name from BLE advertisement data
 * 
 * @param data Raw advertisement data
 * @param len Length of advertisement data
 * @param name_buf Output buffer for device name
 * @param name_buf_len Size of output buffer
 */
void parse_ble_device_name(const uint8_t *data, size_t len, char *name_buf, size_t name_buf_len);

/**
 * @brief Format a byte array as a hex string
 * 
 * @param data Byte array to format
 * @param len Length of byte array
 * @param buf Output buffer for hex string
 * @param buf_size Size of output buffer
 * @param sep Separator character between bytes (e.g., ' ' or ':')
 * @return size_t Number of characters written (excluding null terminator)
 */
size_t format_hex_bytes(const uint8_t *data, size_t len, char *buf, size_t buf_size, char sep);

// ============================================================================
// Signal Strength Utilities
// ============================================================================

/**
 * @brief Get proximity description from RSSI value
 * 
 * @param rssi RSSI value in dBm
 * @return const char* Proximity description string
 */
const char *rssi_to_proximity(int8_t rssi);

/* Small fixed-capacity median filter for smoothing RSSI at the source
 * (per-packet callbacks). No heap, no IRAM requirements — safe to use from
 * WiFi promiscuous and NimBLE GAP callbacks. Median rejects single-packet
 * multipath outliers better than an average; display-level EMA then only
 * has to polish. */
#define RSSI_MEDIAN_CAP 8

typedef struct {
    int8_t v[RSSI_MEDIAN_CAP];
    uint8_t n;
} rssi_median_t;

void rssi_median_reset(rssi_median_t *m);
void rssi_median_push(rssi_median_t *m, int8_t rssi);
int8_t rssi_median_get(const rssi_median_t *m);

// ============================================================================
// Network Scanning Utilities
// ============================================================================

/**
 * @brief Get subnet prefix from current WiFi connection
 * 
 * Extracts the subnet prefix (e.g., "192.168.1.") from the device's IP address.
 * 
 * @param prefix Buffer to store subnet prefix (minimum 16 bytes)
 * @param prefix_size Size of prefix buffer
 * @return true on success, false on failure (not connected, etc.)
 */
bool get_wifi_subnet_prefix(char *prefix, size_t prefix_size);

/**
 * @brief Get the full host scan range from the actual netmask
 *
 * Computes first/last usable host IPs (host byte order) from the STA
 * netmask, capped at /20 (4094 hosts) to keep scan times sane.
 *
 * @param prefix Buffer for the network address string (minimum 16 bytes)
 * @param prefix_size Size of prefix buffer
 * @param first_host First usable host IP (host byte order), may be NULL
 * @param last_host Last usable host IP (host byte order), may be NULL
 * @return true on success, false on failure (not connected, etc.)
 */
bool get_wifi_subnet_range(char *prefix, size_t prefix_size,
                           uint32_t *first_host, uint32_t *last_host);

/**
 * @brief Format a host-byte-order IPv4 address as a dotted string
 */
void ip_u32_to_str(uint32_t ip_host_order, char *out, size_t out_size);

/**
 * @brief Connect to a TCP port with timeout
 * 
 * Creates a non-blocking socket, connects with timeout, and returns the socket.
 * 
 * @param target_ip IP address to connect to
 * @param port Port number to connect to
 * @param timeout_sec Connection timeout in seconds
 * @return Socket descriptor on success, -1 on failure
 */
int tcp_connect_with_timeout(const char *target_ip, uint16_t port, int timeout_sec);
int tcp_connect_with_timeout_cancel(const char *target_ip, uint16_t port, int timeout_ms,
                                    volatile bool *cancel_flag);

/**
 * @brief Receive data from a socket with timeout
 * 
 * @param sock Socket descriptor
 * @param buffer Buffer to store received data
 * @param buffer_size Size of buffer
 * @param timeout_sec Receive timeout in seconds
 * @return Number of bytes received, -1 on error, 0 on timeout
 */
int tcp_recv_with_timeout(int sock, char *buffer, size_t buffer_size, int timeout_sec);
int tcp_recv_with_timeout_cancel(int sock, char *buffer, size_t buffer_size, int timeout_ms,
                                 volatile bool *cancel_flag);

/**
 * @brief Close socket safely (handles invalid socket)
 * 
 * @param sock Socket descriptor (set to -1 after close)
 */
void tcp_close_socket(int *sock);

// ============================================================================
// Macros
// ============================================================================

#define WRAP_MESSAGE(msg) wrap_message(msg, __FILE__, __LINE__)

#endif // UTILS_H
