/**
 * @file port_scan.c
 * @brief TCP/UDP Port scanning implementation
 * 
 * This module handles TCP and UDP port scanning operations including:
 * - Subnet-wide port scanning
 * - Individual host port scanning
 * - TCP port range scanning
 * - UDP port scanning
 * - SSH service detection
 */

#include "scans/wifi/port_scan.h"
#include "core/network_constants.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "core/utils.h"
#include "managers/ghostchi_manager.h"
#include "scans/wifi/arp_scan.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/icmp.h"
#include "lwip/ip4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/task.h"

// Constants
#define SCAN_TIMEOUT_MS 100
#define HOST_TIMEOUT_MS 100

// Module tag for logging
static const char *TAG = "PortScan";

// Cancellation flag for async scans
static volatile bool g_port_scan_cancel = false;

// Task handle for async scan
static TaskHandle_t g_port_scan_task_handle = NULL;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Calculate IP checksum
 * 
 * Calculates the IP header checksum for raw packet construction.
 */
uint16_t port_scan_calculate_checksum(uint16_t *addr, int len) {
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    while (nleft > 1) {
        sum += *w++;
        nleft -= 2;
    }

    if (nleft == 1) {
        *(unsigned char *)(&answer) = *(unsigned char *)w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    answer = ~sum;
    return answer;
}

// ============================================================================
// Context Management Functions
// ============================================================================

/**
 * @brief Initialize a port scanner context
 */
port_scanner_ctx_t *port_scanner_init(void) {
    port_scanner_ctx_t *ctx = malloc(sizeof(port_scanner_ctx_t));
    if (!ctx) {
        return NULL;
    }

    ctx->num_active_hosts = 0;
    memset(ctx->subnet_prefix, 0, sizeof(ctx->subnet_prefix));

    return ctx;
}

/**
 * @brief Clean up a port scanner context
 */
void port_scanner_cleanup(port_scanner_ctx_t *ctx) {
    if (ctx) {
        free(ctx);
    }
}

// ============================================================================
// Network Utility Functions
// ============================================================================

/**
 * @brief Get subnet prefix from current WiFi connection
 */
bool port_scan_get_subnet_prefix(port_scanner_ctx_t *ctx) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        glog("Failed to get WiFi interface\n");
        return false;
    }

    // Check if WiFi is connected
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        glog("WiFi is not connected\n");
        return false;
    }

    // Get IP info
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        glog("Failed to get IP info\n");
        return false;
    }

    uint32_t network = ip_info.ip.addr & ip_info.netmask.addr;
    struct in_addr network_addr;
    network_addr.s_addr = network;

    char *network_str = inet_ntoa(network_addr);
    char *last_dot = strrchr(network_str, '.');
    if (last_dot == NULL) {
        glog("Invalid network address format\n");
        return false;
    }

    size_t prefix_len = last_dot - network_str + 1;
    memcpy(ctx->subnet_prefix, network_str, prefix_len);
    ctx->subnet_prefix[prefix_len] = '\0';

    glog("Determined subnet prefix: %s\n", ctx->subnet_prefix);
    return true;
}

/**
 * @brief Check if a host is active (responds to ping)
 */
bool port_scan_is_host_active(const char *ip_addr) {
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        return false;
    }

    struct timeval tv;
    tv.tv_sec = HOST_TIMEOUT_MS / 1000;
    tv.tv_usec = (HOST_TIMEOUT_MS % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &dest_addr.sin_addr.s_addr);

    // Build ICMP echo request
    uint8_t packet[64];
    memset(packet, 0, sizeof(packet));
    struct icmp_echo_hdr *icmp = (struct icmp_echo_hdr *)packet;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->chksum = 0;
    icmp->id = htons((uint16_t)esp_random());
    icmp->seqno = htons(1);
    icmp->chksum = port_scan_calculate_checksum((uint16_t *)packet, sizeof(packet));

    sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    uint8_t buf[128];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);

    close(sock);
    return n > 0;
}

// ============================================================================
// Port Scanning Functions
// ============================================================================

// lwIP is configured for 16 active TCP sockets; keep headroom for the rest
// of the system while still getting a large speedup over sequential connects.
#define TCP_PIPELINE_SLOTS 8
#define TCP_LAUNCH_PACING_MS 2

typedef struct {
    const uint16_t *ports;
    uint32_t count;
    uint32_t next;
    uint16_t range_first;
    uint16_t range_last;
} tcp_port_source_t;

static bool tcp_port_source_init_list(tcp_port_source_t *src) {
    src->ports = COMMON_PORTS;
    src->count = (uint32_t)NUM_PORTS;
    src->next = 0;
    src->range_first = 0;
    src->range_last = 0;
    return src->count > 0;
}

static bool tcp_port_source_init_range(tcp_port_source_t *src, uint16_t first, uint16_t last) {
    src->ports = NULL;
    src->count = 0;
    src->next = 0;
    src->range_first = first;
    src->range_last = last;
    return first <= last;
}

static bool tcp_port_source_next(tcp_port_source_t *src, uint16_t *out) {
    if (src->ports) {
        if (src->next >= src->count) return false;
        *out = src->ports[src->next++];
        return true;
    }
    uint32_t idx = src->next++;
    if (src->range_first + idx > src->range_last) return false;
    *out = (uint16_t)(src->range_first + idx);
    return true;
}

static void tcp_scan_pipelined(const char *target_ip, tcp_port_source_t *src,
                               port_scan_result_t *result) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &addr.sin_addr);

    int socks[TCP_PIPELINE_SLOTS];
    uint16_t ports[TCP_PIPELINE_SLOTS];
    int active = 0;
    bool more = true;

    while (more && !g_port_scan_cancel && result->num_open_ports < PORT_SCAN_MAX_OPEN_PORTS) {
        while (active < TCP_PIPELINE_SLOTS && result->num_open_ports < PORT_SCAN_MAX_OPEN_PORTS) {
            uint16_t port;
            if (!tcp_port_source_next(src, &port)) {
                more = false;
                break;
            }

            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }

            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);

            addr.sin_port = htons(port);
            int r = connect(sock, (struct sockaddr *)&addr, sizeof(addr));

            if (r == 0) {
                result->open_ports[result->num_open_ports++] = port;
                glog("  Port %d: OPEN\n", port);
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(TCP_LAUNCH_PACING_MS));
                continue;
            }
            if (r < 0 && errno != EINPROGRESS) {
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(TCP_LAUNCH_PACING_MS));
                continue;
            }

            socks[active] = sock;
            ports[active] = port;
            active++;
            vTaskDelay(pdMS_TO_TICKS(TCP_LAUNCH_PACING_MS));
        }

        if (active == 0) break;

        fd_set wfds;
        FD_ZERO(&wfds);
        int maxfd = -1;
        for (int i = 0; i < active; i++) {
            FD_SET(socks[i], &wfds);
            if (socks[i] > maxfd) maxfd = socks[i];
        }

        struct timeval tv = {
            .tv_sec = SCAN_TIMEOUT_MS / 1000,
            .tv_usec = (SCAN_TIMEOUT_MS % 1000) * 1000,
        };
        int rv = select(maxfd + 1, NULL, &wfds, NULL, &tv);
        if (rv < 0) break;

        int remaining = 0;
        for (int i = 0; i < active; i++) {
            bool done = (rv == 0);
            bool open = false;
            if (!done && FD_ISSET(socks[i], &wfds)) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    open = true;
                }
                done = true;
            }
            if (done) {
                if (open && result->num_open_ports < PORT_SCAN_MAX_OPEN_PORTS) {
                    result->open_ports[result->num_open_ports++] = ports[i];
                    glog("  Port %d: OPEN\n", ports[i]);
                }
                close(socks[i]);
            } else {
                socks[remaining] = socks[i];
                ports[remaining] = ports[i];
                remaining++;
            }
        }
        active = remaining;
    }

    for (int i = 0; i < active; i++) {
        close(socks[i]);
    }
}

/**
 * @brief Scan common TCP ports on a host
 */
void port_scan_scan_tcp_ports(const char *target_ip, port_scan_result_t *result) {
    snprintf(result->ip, sizeof(result->ip), "%s", target_ip);
    result->num_open_ports = 0;

    glog("Scanning TCP ports on %s...\n", target_ip);

    tcp_port_source_t src;
    if (!tcp_port_source_init_list(&src)) {
        return;
    }
    tcp_scan_pipelined(target_ip, &src, result);
}

/**
 * @brief Build a UDP probe packet for specific ports
 */
static size_t build_udp_probe(uint16_t port, uint8_t *buf, size_t bufsize) {
    if (port == 53 && bufsize >= 64) {
        // DNS query
        uint8_t *p = buf;
        uint16_t id = (uint16_t)esp_random();
        *(uint16_t *)(p + 0) = htons(id);
        *(uint16_t *)(p + 2) = htons(0x0100);
        *(uint16_t *)(p + 4) = htons(1);
        *(uint16_t *)(p + 6) = 0;
        *(uint16_t *)(p + 8) = 0;
        *(uint16_t *)(p + 10) = 0;
        p += 12;
        const char *name = "example.com";
        const char *dot = name;
        while (*dot) {
            const char *start = dot;
            while (*dot && *dot != '.') dot++;
            size_t len = (size_t)(dot - start);
            *p++ = (uint8_t)len;
            memcpy(p, start, len);
            p += len;
            if (*dot == '.') dot++;
        }
        *p++ = 0;
        *(uint16_t *)p = htons(1);
        p += 2;
        *(uint16_t *)p = htons(1);
        p += 2;
        return (size_t)(p - buf);
    }
    if (port == 123 && bufsize >= 48) {
        // NTP request
        memset(buf, 0, 48);
        buf[0] = 0x1b;
        return 48;
    }
    if (port == 69 && bufsize >= 64) {
        // TFTP request
        uint8_t *p = buf;
        *(uint16_t *)p = htons(1);
        p += 2;
        const char *fname = "test";
        memcpy(p, fname, strlen(fname));
        p += strlen(fname);
        *p++ = 0;
        const char *mode = "octet";
        memcpy(p, mode, strlen(mode));
        p += strlen(mode);
        *p++ = 0;
        return (size_t)(p - buf);
    }
    if (port == 1900 && bufsize >= 256) {
        // SSDP M-SEARCH
        const char *msearch = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
        size_t len = strlen(msearch);
        memcpy(buf, msearch, len);
        return len;
    }
    if (bufsize >= 1) {
        buf[0] = 0x00;
        return 1;
    }
    return 0;
}

/**
 * @brief Check if a UDP port is open
 */
static bool udp_port_is_open(const char *target_ip, uint16_t port, uint32_t wait_ms) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr.s_addr);

    struct timeval tv;
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t probe[256];
    size_t probe_len = build_udp_probe(port, probe, sizeof(probe));
    if (probe_len == 0) {
        close(sock);
        return false;
    }
    sendto(sock, probe, probe_len, 0, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[512];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
    int err = (n < 0) ? errno : 0;
    close(sock);
    if (n > 0) {
        return true;
    }
    (void)err;
    return false;
}

/**
 * @brief Scan common UDP ports on a host
 */
void port_scan_scan_udp_ports(const char *target_ip, port_scan_result_t *result) {
    snprintf(result->ip, sizeof(result->ip), "%s", target_ip);
    result->num_open_ports = 0;

    glog("Scanning UDP ports on %s...\n", target_ip);

    for (size_t i = 0; i < NUM_UDP_PORTS; i++) {
        if (result->num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS) break;
        uint16_t port = UDP_COMMON_PORTS[i];
        if (udp_port_is_open(target_ip, port, 40)) {
            result->open_ports[result->num_open_ports++] = port;
            glog("  UDP %d: OPEN\n", port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/**
 * @brief Scan for SSH service on a host
 */
void port_scan_ssh(const char *target_ip, port_scan_result_t *result) {
    struct sockaddr_in server_addr;
    int sock;
    int scan_result;
    struct timeval timeout;
    fd_set fdset;
    int flags;
    char banner[256];
    ssize_t bytes_read;
    
    ESP_LOGI(TAG, "Starting SSH scan on host: %s", target_ip);
    
    snprintf(result->ip, sizeof(result->ip), "%s", target_ip);
    result->num_open_ports = 0;
    
    server_addr.sin_family = AF_INET;
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr.s_addr);
    
    glog("SSH scanning host: %s\n", target_ip);
    
    uint16_t ssh_ports[] = {22, 2222, 2022};
    size_t num_ssh_ports = sizeof(ssh_ports) / sizeof(ssh_ports[0]);
    
    for (size_t i = 0; i < num_ssh_ports; i++) {
        if (result->num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS)
            break;
            
        uint16_t port = ssh_ports[i];
        ESP_LOGI(TAG, "Testing port %d on %s", port, target_ip);
        glog("  Testing SSH port %d...", port);
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create socket for port %d: errno=%d", port, errno);
            glog(" FAILED\n");
            continue;
        }
            
        flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        server_addr.sin_port = htons(port);
        ESP_LOGD(TAG, "Attempting connection to %s:%d", target_ip, port);
        scan_result = connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
        
        if (scan_result == 0 || (scan_result < 0 && errno == EINPROGRESS)) {
            timeout.tv_sec = 3;
            timeout.tv_usec = 0;
            
            FD_ZERO(&fdset);
            FD_SET(sock, &fdset);
            
            scan_result = select(sock + 1, NULL, &fdset, NULL, &timeout);
            
            if (scan_result > 0) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 && error == 0) {
                    ESP_LOGI(TAG, "Port %d is OPEN on %s", port, target_ip);
                    result->open_ports[result->num_open_ports++] = port;
                    
                    fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
                    
                    timeout.tv_sec = 2;
                    timeout.tv_usec = 0;
                    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    
                    memset(banner, 0, sizeof(banner));
                    bytes_read = recv(sock, banner, sizeof(banner) - 1, 0);
                    ESP_LOGD(TAG, "Received %d bytes from %s:%d", (int)bytes_read, target_ip, port);
                    
                    if (bytes_read > 0) {
                        banner[bytes_read] = '\0';
                        char *newline = strchr(banner, '\r');
                        if (newline) *newline = '\0';
                        newline = strchr(banner, '\n');
                        if (newline) *newline = '\0';
                        
                        ESP_LOGI(TAG, "SSH banner from %s:%d: %s", target_ip, port, banner);
                        glog(" OPEN: %s\n", banner);
                    } else {
                        glog(" OPEN (no banner)\n");
                    }
                } else {
                    ESP_LOGD(TAG, "Port %d connection failed on %s (getsockopt error)", port, target_ip);
                    glog(" CLOSED\n");
                }
            } else {
                ESP_LOGD(TAG, "Port %d timeout on %s (select result: %d)", port, target_ip, scan_result);
                glog(" TIMEOUT\n");
            }
        } else {
            ESP_LOGD(TAG, "Port %d immediate connection failure on %s (errno: %d)", port, target_ip, errno);
            glog(" CLOSED\n");
        }
        
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    glog("SSH scan completed on %s - found %d open ports\n", target_ip, result->num_open_ports);
}

/**
 * @brief Scan a specific IP address for open TCP ports (range)
 */
bool port_scan_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    if (start_port > end_port) {
        glog("Invalid port range: %d-%d\n", start_port, end_port);
        return false;
    }

    port_scan_result_t result;
    snprintf(result.ip, sizeof(result.ip), "%s", target_ip);
    result.num_open_ports = 0;

    glog("Scanning %s TCP ports %d-%d\n", target_ip, start_port, end_port);

    tcp_port_source_t src;
    if (!tcp_port_source_init_range(&src, start_port, end_port)) {
        return false;
    }
    tcp_scan_pipelined(target_ip, &src, &result);

    if (result.num_open_ports > 0) {
        glog("Host %s has %d open ports\n", result.ip, result.num_open_ports);
    }

    return true;
}

/**
 * @brief Scan a specific IP address for open UDP ports (range)
 */
bool port_scan_udp_ip_range(const char *target_ip, uint16_t start_port, uint16_t end_port) {
    if (start_port > end_port) {
        glog("Invalid port range: %d-%d\n", start_port, end_port);
        return false;
    }
    // Use local result - no need for context allocation
    port_scan_result_t result;
    snprintf(result.ip, sizeof(result.ip), "%s", target_ip);
    result.num_open_ports = 0;

    glog("Scanning %s UDP ports %d-%d\n", target_ip, start_port, end_port);

    uint32_t ports_scanned = 0;
    uint32_t total_ports = (uint32_t)end_port - start_port + 1;

    for (uint32_t p = (uint32_t)start_port; p <= (uint32_t)end_port; p++) {
        uint16_t port = (uint16_t)p;
        if (result.num_open_ports >= PORT_SCAN_MAX_OPEN_PORTS) break;
        ports_scanned++;
        if (ports_scanned % 200 == 0) {
            glog("Progress: %lu/%lu ports (%.1f%%)\n",
                 (unsigned long)ports_scanned, (unsigned long)total_ports,
                 (float)ports_scanned / total_ports * 100);
        }
        if (udp_port_is_open(target_ip, port, 40)) {
            result.open_ports[result.num_open_ports++] = port;
            glog("  UDP %d: OPEN\n", port);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    if (result.num_open_ports > 0) {
        glog("Host %s has %d UDP ports responding\n", result.ip, result.num_open_ports);
    }

    return true;
}

// ============================================================================
// Main Subnet Scanning Function
// ============================================================================

/**
 * @brief Print service analysis for a scan result
 * 
 * Analyzes open ports and prints possible services and device types.
 * Uses centralized port service definitions from network_constants.
 * 
 * @param result Scan result to analyze
 */
static void print_service_analysis(const port_scan_result_t *result) {
    if (result->num_open_ports == 0) {
        return;
    }

    glog("Host %s has %d open ports:\n", result->ip, result->num_open_ports);
    glog("Possible services/devices:\n");

    for (uint8_t j = 0; j < result->num_open_ports; j++) {
        uint16_t port = result->open_ports[j];
        glog("  - Port %d: ", port);

        const char *description = get_port_service_description(port);
        if (description) {
            glog("%s\n", description);
        } else {
            glog("Unknown Service\n");
        }
    }

    bool has_web = false;
    bool has_db = false;
    bool has_file_sharing = false;

    for (uint8_t j = 0; j < result->num_open_ports; j++) {
        uint16_t port = result->open_ports[j];
        if (is_web_port(port))
            has_web = true;
        if (is_database_port(port))
            has_db = true;
        if (is_file_sharing_port(port))
            has_file_sharing = true;
    }

    glog("\nPossible device type:\n");

    if (has_web && has_db) {
        glog("- Web Application Server\n");
    }
    if (has_file_sharing) {
        glog("- Windows Server\n");
    }
    glog("\n");
}

static void port_scan_report_host(const char *ip, const uint8_t *mac, size_t index) {
    const char *vendor = mac ? arp_scan_get_vendor(mac) : NULL;
    if (vendor) {
        glog("\n[Host %zu] Found active host: %s (%s)\n", index, ip, vendor);
    } else {
        glog("\n[Host %zu] Found active host: %s\n", index, ip);
    }
}

static void port_scan_target(const char *ip) {
    port_scan_result_t tcp_result;
    port_scan_result_t udp_result;

    port_scan_scan_tcp_ports(ip, &tcp_result);
    port_scan_scan_udp_ports(ip, &udp_result);

    print_service_analysis(&tcp_result);

    if (udp_result.num_open_ports > 0) {
        glog("UDP ports on %s:\n", ip);
        for (uint8_t k = 0; k < udp_result.num_open_ports; k++) {
            glog("  UDP %d: OPEN\n", udp_result.open_ports[k]);
        }
    }
}

/**
 * @brief Scan the local subnet for open ports
 */
bool port_scan_subnet(void) {
    port_scanner_ctx_t *ctx = port_scanner_init();
    if (!ctx) {
        glog("Failed to initialize scanner context\n");
        return false;
    }

    char current_ip[16];
    ctx->num_active_hosts = 0;
    g_port_scan_cancel = false;  // Reset cancellation flag

    int arp_count = arp_scan_get_count();
    if (arp_count > 0) {
        glog("Starting port scan on %d ARP-discovered host(s)...\n", arp_count);

        for (int i = 0; i < arp_count && !g_port_scan_cancel; i++) {
            const arp_host_t *host = arp_scan_get_host(i);
            if (!host) continue;

            ctx->num_active_hosts++;
            port_scan_report_host(host->ip, host->mac, ctx->num_active_hosts);
            port_scan_target(host->ip);
        }
    } else {
        uint32_t first, last;
        if (!get_wifi_subnet_range(ctx->subnet_prefix, sizeof(ctx->subnet_prefix), &first, &last)) {
            glog("Failed to get network information. Make sure WiFi is connected.\n");
            port_scanner_cleanup(ctx);
            return false;
        }

        uint32_t total_hosts = last - first + 1;
        glog("Starting subnet scan on %s (%u hosts)\n", ctx->subnet_prefix,
             (unsigned)total_hosts);

        uint32_t scanned = 0;
        for (uint32_t ip = first; ip <= last && !g_port_scan_cancel; ip++, scanned++) {
            if (scanned % 25 == 0) {
                glog("Progress: %u/%u hosts scanned\n", (unsigned)scanned, (unsigned)total_hosts);
            }

            ip_u32_to_str(ip, current_ip, sizeof(current_ip));

            if (port_scan_is_host_active(current_ip)) {
                ctx->num_active_hosts++;
                port_scan_report_host(current_ip, NULL, ctx->num_active_hosts);
                port_scan_target(current_ip);
            }
        }
    }

    glog("\n========================================\n");
    if (g_port_scan_cancel) {
        glog("Scan cancelled. Found %zu active hosts.\n", ctx->num_active_hosts);
    } else {
        glog("Scan completed. Found %zu active hosts.\n", ctx->num_active_hosts);
    }
    glog("========================================\n");

    port_scanner_cleanup(ctx);
    g_port_scan_task_handle = NULL;
    return !g_port_scan_cancel;
}

// ============================================================================
// Async Scan Functions
// ============================================================================

/**
 * @brief Task wrapper for async subnet scanning
 */
static void port_scan_subnet_task(void *pvParameters) {
    port_scan_subnet();
    vTaskDeleteWithCaps(NULL);
}

/**
 * @brief Start an async subnet scan in a separate task
 * 
 * This keeps the CLI responsive during the scan.
 */
void port_scan_subnet_async(void) {
    if (g_port_scan_task_handle != NULL) {
        glog("Port scan already in progress\n");
        return;
    }
    ghostchi_manager_add_xp(3);
    
    g_port_scan_cancel = false;
    xTaskCreate_psram(port_scan_subnet_task, "port_scan", 8192, NULL, 5, &g_port_scan_task_handle);
}

/**
 * @brief Cancel an ongoing port scan
 */
void port_scan_cancel(void) {
    g_port_scan_cancel = true;
}

/**
 * @brief Check if a port scan is in progress
 */
bool port_scan_is_running(void) {
    return g_port_scan_task_handle != NULL;
}