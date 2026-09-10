/**
 * @file arp_scan.c
 * @brief ARP network scanning implementation
 * 
 * This module handles ARP-based network scanning operations including:
 * - Scanning subnets for active hosts
 * - Resolving MAC addresses from IP addresses
 * - Managing ARP scan results
 */

#include "scans/wifi/arp_scan.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/scan_saver.h"
#include "core/system_manager.h"
#include "core/utils.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "managers/wifi_manager.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcpip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Module tag for logging
static const char *TAG = "ARPScan";

// Scan configuration
// Batches are kept at 8 because lwIP's default ARP_TABLE_SIZE is 10:
// a larger batch evicts entries before the harvest pass can read them.
#define BATCH_SIZE 8
#define ARP_REQUEST_DELAY_MS 5
#define ARP_RESPONSE_WAIT_MS 80
#define MAX_RETRIES 3
#define ARP_SCAN_MAX_PREFIX_LEN 20  // cap at /20 (4094 hosts) to keep scan time sane

// Multi-pass scan configuration (technique inspired by DecentLabs/officeAir).
// Multiple passes with inter-pass delays catch power-saving mobile clients
// that miss single-pass probes. Results are unioned across passes.
#define ARP_SCAN_PASSES_SMALL  3     // for /24 and smaller
#define ARP_SCAN_PASSES_LARGE  2     // for larger subnets
#define ARP_SMALL_THRESHOLD  256     // <= this many hosts = "small"
#define ARP_INTER_PASS_DELAY_MS 800

// Persistent result storage (heap-allocated)
static arp_host_t *g_arp_results = NULL;
static int g_arp_result_count = 0;
static volatile bool g_arp_scan_running = false;
static volatile bool g_arp_scan_done = false;

// Live progress tracking (read from UI poll timer)
static volatile int g_arp_progress_pass = 0;
static volatile int g_arp_progress_total_passes = 0;
static volatile int g_arp_progress_scanned = 0;
static volatile int g_arp_progress_total_hosts = 0;
static volatile int g_arp_progress_found = 0;

typedef struct {
    ip4_addr_t target;          /* by value: owned by the heap struct */
    uint8_t *mac;
    arp_scanner_ctx_t *scanner;
} arp_tcpip_call_t;

typedef struct {
    arp_scanner_ctx_t *scanner;
    volatile bool done;
} arp_harvest_req_t;

// ============================================================================
// Internal Helpers (Module-specific)
// ============================================================================

static const char *lookup_oui_vendor(const uint8_t *mac);

/**
 * @brief Format a host entry for display (single source of truth)
 */
static void format_host_entry(char *buffer, size_t size, size_t index, const char *ip, const uint8_t *mac) {
    char mac_str[18];
    format_mac_address(mac, mac_str, sizeof(mac_str), true);
    const char *vendor = lookup_oui_vendor(mac);
    if (vendor) {
        snprintf(buffer, size, "%2zu. %s [%s] %s", index, ip, mac_str, vendor);
    } else {
        snprintf(buffer, size, "%2zu. %s [%s]", index, ip, mac_str);
    }
}

/**
 * @brief Log and save a host entry
 */
static void log_host_entry(scan_file_t *sf, size_t index, const char *ip, const uint8_t *mac) {
    char entry[80];
    format_host_entry(entry, sizeof(entry), index, ip, mac);
    glog("%s\n", entry);
    if (scan_file_is_open(sf)) {
        scan_file_printf(sf, "%s\n", entry);
    }
}

// ============================================================================
// Context Management Functions
// ============================================================================

/**
 * @brief Initialize ARP scanner context
 */
arp_scanner_ctx_t *arp_scanner_init(void) {
    arp_scanner_ctx_t *ctx = malloc(sizeof(arp_scanner_ctx_t));
    if (!ctx) {
        return NULL;
    }

    // Allocate up to ARP_SCAN_MAX_RESULTS (the persistent storage cap)
    ctx->max_hosts = ARP_SCAN_MAX_RESULTS;
    ctx->hosts = malloc(sizeof(arp_host_t) * ctx->max_hosts);
    if (!ctx->hosts) {
        free(ctx);
        return NULL;
    }

    ctx->num_active_hosts = 0;
    ctx->scan_first = 0;
    ctx->scan_last = 0;
    ctx->total_hosts = 0;
    memset(ctx->subnet_prefix, 0, sizeof(ctx->subnet_prefix));
    return ctx;
}

/**
 * @brief Clean up ARP scanner context
 */
void arp_scanner_cleanup(arp_scanner_ctx_t *ctx) {
    if (ctx) {
        if (ctx->hosts) {
            free(ctx->hosts);
        }
        free(ctx);
    }
}

// ============================================================================
// ARP Request Functions
// ============================================================================

/**
 * @brief Send ARP request to target IP via the lwIP stack
 *
 * Uses the thread-safe etharp_request() call through the lwIP TCP/IP core
 * instead of crafting raw 802.11 frames. This is more reliable (no WiFi
 * buffer exhaustion) and lets the stack handle retransmits.
 * Technique inspired by DecentLabs/officeAir (MIT-licensed).
 */
static void arp_request_callback(void *ctx) {
    arp_tcpip_call_t *request = (arp_tcpip_call_t *)ctx;
    if (netif_default) {
        etharp_request(netif_default, &request->target);
    }
    free(request);
}

bool send_arp_request(const char *target_ip) {
    if (!target_ip) {
        ESP_LOGW(TAG, "send_arp_request: target_ip is NULL");
        return false;
    }

    ip4_addr_t target_addr;
    if (!ip4addr_aton(target_ip, &target_addr)) {
        ESP_LOGW(TAG, "send_arp_request: invalid IP '%s'", target_ip);
        return false;
    }

    arp_tcpip_call_t *call = malloc(sizeof(arp_tcpip_call_t));
    if (!call) {
        ESP_LOGW(TAG, "send_arp_request: out of memory");
        return false;
    }
    call->target = target_addr;

    if (tcpip_callback_with_block(arp_request_callback, call, 1) == ERR_MEM) {
        free(call);  /* not queued: the callback will never run */
        ESP_LOGW(TAG, "send_arp_request: callback queue full");
        return false;
    }
    return true;  /* queued (or already executed); the callback frees it */
}

/**
 * @brief Get ARP table entry for IP address (thread-safe)
 */
static void arp_lookup_callback(void *ctx) {
    arp_tcpip_call_t *lookup = (arp_tcpip_call_t *)ctx;
    struct eth_addr *eth = NULL;
    const ip4_addr_t *found_ip = NULL;
    if (etharp_find_addr(NULL, &lookup->target, &eth, &found_ip) >= 0 && eth) {
        memcpy(lookup->mac, eth->addr, 6);
        lookup->scanner = (arp_scanner_ctx_t *)(uintptr_t)1; // success flag
    }
    free(lookup);
}

bool get_arp_table_entry(const char *ip, uint8_t *mac) {
    if (!ip || !mac) {
        return false;
    }

    // Parse target IP
    ip4_addr_t target_addr;
    if (!ip4addr_aton(ip, &target_addr)) {
        return false;
    }

    arp_tcpip_call_t *call = malloc(sizeof(arp_tcpip_call_t));
    if (!call) {
        return false;
    }
    call->target = target_addr;
    call->mac = mac;
    call->scanner = NULL;

    if (tcpip_callback_with_block(arp_lookup_callback, call, 1) == ERR_MEM) {
        free(call);  /* not queued: the callback will never run */
        return false;
    }
    return true;  /* queued; the callback writes the MAC and frees the struct */
}

// ============================================================================
// Subnet Helper Functions
// ============================================================================

/**
 * @brief Build an IP string from a uint32_t address
 */
static void ip_to_string(uint32_t addr, char *buf, size_t size) {
    struct in_addr a;
    // addr is in host byte order; inet_ntoa expects network byte order
    a.s_addr = htonl(addr);
    strncpy(buf, inet_ntoa(a), size - 1);
    buf[size - 1] = '\0';
}

/**
 * @brief Determine the scan range from the actual netmask
 *
 * Fills ctx->scan_first, ctx->scan_last, ctx->total_hosts and builds
 * a human-readable subnet_prefix.  Caps at /20 to keep scan time sane.
 */
static bool arp_get_subnet_range(arp_scanner_ctx_t *ctx) {
    esp_netif_t *netif = get_wifi_sta_netif();
    if (!netif) {
        glog("Failed to get WiFi interface\n");
        return false;
    }

    if (!is_wifi_sta_connected()) {
        glog("WiFi is not connected\n");
        return false;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        glog("Failed to get IP info\n");
        return false;
    }

    uint32_t netmask = ntohl(ip_info.netmask.addr);
    uint32_t my_ip   = ntohl(ip_info.ip.addr);

    // Count host bits: trailing ones in ~netmask
    int host_bits = 0;
    uint32_t host_mask = ~netmask;
    for (uint32_t bit = 1; bit && (host_mask & bit); bit <<= 1) {
        host_bits++;
    }

    // Cap at /20 (12 host bits = 4094 hosts) to keep scan time sane.
    // When capping, shrink the effective host_mask so the scan range
    // stays within the first /20 block that contains our IP.
    if (host_bits > (32 - ARP_SCAN_MAX_PREFIX_LEN)) {
        host_bits = 32 - ARP_SCAN_MAX_PREFIX_LEN;
        host_mask = (1u << host_bits) - 1;
    }

    uint32_t network  = my_ip & ~host_mask;
    uint32_t broadcast = network | host_mask;
    uint32_t first_ip = network + 1;
    uint32_t last_ip  = broadcast - 1;

    if (first_ip >= last_ip) {
        glog("Subnet too small to scan\n");
        return false;
    }

    ctx->scan_first  = htonl(first_ip);
    ctx->scan_last   = htonl(last_ip);
    ctx->total_hosts = (int)(last_ip - first_ip + 1);

    // Build a display prefix (e.g. "192.168.1." for /24)
    // For larger subnets this is just the network address
    ip_to_string(network, ctx->subnet_prefix, sizeof(ctx->subnet_prefix));

    int cidr = 32 - host_bits;
    char first_str[16], last_str[16];
    ip_to_string(first_ip, first_str, sizeof(first_str));
    ip_to_string(last_ip, last_str, sizeof(last_str));
    ESP_LOGI(TAG, "Subnet: %s/%d (%d hosts, scan range %s-%s)",
             ctx->subnet_prefix, cidr, ctx->total_hosts, first_str, last_str);
    return true;
}

// ============================================================================
// Host Management Functions
// ============================================================================

/**
 * @brief Add a discovered host to the context
 */
static bool add_discovered_host(arp_scanner_ctx_t *ctx, const char *ip, const uint8_t *mac) {
    if (ctx->num_active_hosts >= ctx->max_hosts) {
        return false;
    }
    
    arp_host_t *host = &ctx->hosts[ctx->num_active_hosts];
    strncpy(host->ip, ip, sizeof(host->ip) - 1);
    host->ip[sizeof(host->ip) - 1] = '\0';
    memcpy(host->mac, mac, 6);
    host->is_active = true;
    ctx->num_active_hosts++;
    return true;
}

/**
 * @brief Check if a host IP is already in the discovered list
 */
static bool is_host_known(const arp_scanner_ctx_t *ctx, const char *ip) {
    for (size_t i = 0; i < ctx->num_active_hosts; i++) {
        if (strcmp(ctx->hosts[i].ip, ip) == 0) return true;
    }
    return false;
}

/**
 * @brief Harvest all entries currently in the lwIP ARP table
 *
 * Called between sub-batches and between passes to pick up early responses.
 * Thread-safe: runs on the TCP/IP core thread while iterating the table.
 */
static void harvest_arp_table_callback(void *ctx) {
    arp_harvest_req_t *req = (arp_harvest_req_t *)ctx;
    arp_scanner_ctx_t *scanner = req->scanner;
    for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *entry_netif = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &entry_netif, &eth) || !ip || !eth) continue;

        char ip_str[16];
        ip4addr_ntoa_r(ip, ip_str, sizeof(ip_str));

        if (!is_host_known(scanner, ip_str)) {
            add_discovered_host(scanner, ip_str, eth->addr);
        }
    }
    __sync_synchronize();
    req->done = true;
}

static void harvest_arp_table(arp_scanner_ctx_t *ctx) {
    arp_harvest_req_t *req = malloc(sizeof(arp_harvest_req_t));
    if (!req) {
        ESP_LOGW(TAG, "Failed to allocate ARP table harvest request");
        return;
    }
    req->scanner = ctx;
    req->done = false;

    if (tcpip_callback_with_block(harvest_arp_table_callback, req, 1) == ERR_MEM) {
        free(req);  /* not queued: the callback will never run */
        ESP_LOGW(TAG, "Failed to read ARP table on TCP/IP thread");
        return;
    }
    /* The tcpip thread runs at higher priority, so this bounded spin ends
     * as soon as the callback completes. */
    while (!req->done) {
        taskYIELD();
    }
    __sync_synchronize();
    free(req);
}

/**
 * @brief Process a batch of hosts - send ARP requests and collect responses
 */
static void process_batch(arp_scanner_ctx_t *ctx, uint32_t batch_start, uint32_t batch_end) {
    char current_ip[16];
    
    // Send batch of ARP requests using lwIP
    for (uint32_t ip = batch_start; ip <= batch_end; ip++) {
        ip_to_string(ip, current_ip, sizeof(current_ip));
        if (!is_host_known(ctx, current_ip)) {
            send_arp_request(current_ip);
        }
        vTaskDelay(pdMS_TO_TICKS(ARP_REQUEST_DELAY_MS));
    }
    
    // Wait for responses to arrive
    vTaskDelay(pdMS_TO_TICKS(ARP_RESPONSE_WAIT_MS));
    
    // Harvest the ARP table (picks up responses from this batch and any
    // late arrivals from previous batches)
    harvest_arp_table(ctx);
}

// ============================================================================
// Main Scan Functions
// ============================================================================

/**
 * @brief Scan subnet for active hosts using multi-pass ARP sweeps
 *
 * Respects the actual netmask — scans the full network range (up to /20).
 * Runs multiple passes, unioning results across passes. Inter-pass delays
 * give power-saving mobile clients a chance to wake up and respond.
 * Technique inspired by DecentLabs/officeAir (MIT-licensed).
 */
bool arp_scan_subnet(void) {
    arp_scanner_ctx_t *ctx = arp_scanner_init();
    if (!ctx) {
        glog("Failed to initialize ARP scanner context\n");
        return false;
    }

    // Get actual subnet range from netmask
    if (!arp_get_subnet_range(ctx)) {
        glog("Failed to get network information. Make sure WiFi is connected.\n");
        arp_scanner_cleanup(ctx);
        return false;
    }

    // Scale passes based on network size
    int num_passes = (ctx->total_hosts <= ARP_SMALL_THRESHOLD)
                     ? ARP_SCAN_PASSES_SMALL : ARP_SCAN_PASSES_LARGE;
    // Derive CIDR from the host bit count (before capping)
    esp_netif_ip_info_t _ip_info;
    esp_netif_get_ip_info(get_wifi_sta_netif(), &_ip_info);
    uint32_t _hm = ~ntohl(_ip_info.netmask.addr);
    int host_bits = 0;
    for (uint32_t bit = 1; bit && (_hm & bit); bit <<= 1) host_bits++;
    int cidr = 32 - host_bits;

    glog("Starting %d-pass ARP scan on %s/%d (%d hosts)\n",
         num_passes, ctx->subnet_prefix, cidr, ctx->total_hosts);
    ESP_LOGI(TAG, "Starting %d-pass ARP scan, %d hosts in range",
             num_passes, ctx->total_hosts);
    
    ctx->num_active_hosts = 0;
    // Work in host byte order for the loop — comparing network-byte-order
    // uint32_t values on little-endian gives wrong IP ordering.
    uint32_t first = ntohl(ctx->scan_first);
    uint32_t last  = ntohl(ctx->scan_last);

    g_arp_progress_total_passes = num_passes;
    g_arp_progress_total_hosts  = ctx->total_hosts;

    for (int pass = 0; pass < num_passes; pass++) {
        if (!g_arp_scan_running) {
            glog("ARP scan cancelled\n");
            arp_scanner_cleanup(ctx);
            return false;
        }

        g_arp_progress_pass = pass + 1;
        g_arp_progress_scanned = 0;

        glog("Pass %d/%d: scanning %s/%d...\n", pass + 1, num_passes, ctx->subnet_prefix, cidr);
        ESP_LOGI(TAG, "Pass %d/%d: subnet sweep", pass + 1, num_passes);

        int scanned_count = 0;
        for (uint32_t batch_start = first; batch_start <= last; batch_start += BATCH_SIZE) {
            if (!g_arp_scan_running) {
                glog("ARP scan cancelled\n");
                arp_scanner_cleanup(ctx);
                return false;
            }

            uint32_t batch_end = batch_start + BATCH_SIZE - 1;
            if (batch_end > last) batch_end = last;
            
            process_batch(ctx, batch_start, batch_end);
            
            scanned_count += (int)(batch_end - batch_start + 1);
            g_arp_progress_scanned = scanned_count;
            g_arp_progress_found = (int)ctx->num_active_hosts;
            if (scanned_count % 500 == 0 || batch_end == last) {
                glog("Pass %d/%d: %d/%d scanned, %d hosts found\n",
                     pass + 1, num_passes, scanned_count, ctx->total_hosts, (int)ctx->num_active_hosts);
                ESP_LOGI(TAG, "Pass %d/%d: %d/%d scanned, %zu hosts",
                         pass + 1, num_passes, scanned_count, ctx->total_hosts, ctx->num_active_hosts);
            }
        }

        // Inter-pass delay: lets power-saving mobile clients wake up
        if (pass < num_passes - 1) {
            glog("Pass %d/%d done (%zu hosts). Waiting %d ms for mobile clients...\n",
                 pass + 1, num_passes, ctx->num_active_hosts, ARP_INTER_PASS_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(ARP_INTER_PASS_DELAY_MS));
        }
    }

    // Final harvest after last pass
    harvest_arp_table(ctx);

    glog("%d-pass scan complete. %zu unique hosts found.\n",
         num_passes, ctx->num_active_hosts);

    // Transfer the scanner's bounded result allocation to the public result
    // owner. This works for both synchronous CLI and asynchronous UI scans.
    int limit = (int)ctx->num_active_hosts;
    if (limit > ARP_SCAN_MAX_RESULTS) limit = ARP_SCAN_MAX_RESULTS;
    free(g_arp_results);
    g_arp_results = ctx->hosts;
    ctx->hosts = NULL;
    g_arp_result_count = limit;

    // Open scan file for saving results
    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "arp_scan", "txt") == ESP_OK);

    // Final summary
    glog("\n=== ARP Scan Results ===\n");
    glog("Found %zu active hosts on %s/%d (%d passes):\n",
         ctx->num_active_hosts, ctx->subnet_prefix, cidr, num_passes);
    
    if (saving) {
        scan_file_printf(&sf, "--- ARP Scan Results (%zu hosts, %d passes) ---\n",
                         ctx->num_active_hosts, num_passes);
        scan_file_printf(&sf, "Subnet: %s/%d\n\n", ctx->subnet_prefix, cidr);
    }
    
    if (ctx->num_active_hosts > 0) {
        glog("\nActive hosts:\n");
        
        for (size_t i = 0; i < ctx->num_active_hosts; i++) {
            log_host_entry(&sf, i + 1, g_arp_results[i].ip, g_arp_results[i].mac);
        }
    } else {
        glog("No active hosts found.\n");
    }
    
    glog("\nARP scan completed.\n");
    ESP_LOGI(TAG, "ARP scan completed. Found %zu active hosts", ctx->num_active_hosts);

    if (saving) {
        scan_file_close(&sf);
    }

    arp_scanner_cleanup(ctx);
    return true;
}

/**
 * @brief FreeRTOS task wrapper for async ARP scan
 */
static void arp_scan_task(void *pvParameters) {
    (void)pvParameters;
    arp_scan_subnet();
    g_arp_scan_done = true;
    vTaskDeleteWithCaps(NULL);
}

esp_err_t arp_scan_start_async(void) {
    if (g_arp_scan_running) {
        return ESP_ERR_INVALID_STATE;
    }
    arp_scan_clear_results();
    g_arp_scan_running = true;
    g_arp_scan_done = false;
    g_arp_progress_pass = 0;
    g_arp_progress_total_passes = 0;
    g_arp_progress_scanned = 0;
    g_arp_progress_total_hosts = 0;
    g_arp_progress_found = 0;
    BaseType_t ret = xTaskCreate_psram(arp_scan_task, "arp_scan", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        g_arp_scan_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool arp_scan_check_done(void) {
    return g_arp_scan_done;
}

void arp_scan_finish_async(void) {
    g_arp_scan_running = false;
}

bool arp_scan_is_running(void) {
    return g_arp_scan_running && !g_arp_scan_done;
}

void arp_scan_cancel(void) {
    g_arp_scan_running = false;
}

void arp_scan_get_progress(int *pass, int *total_passes, int *scanned, int *total_hosts, int *found) {
    if (pass)        *pass        = g_arp_progress_pass;
    if (total_passes)*total_passes= g_arp_progress_total_passes;
    if (scanned)     *scanned     = g_arp_progress_scanned;
    if (total_hosts) *total_hosts = g_arp_progress_total_hosts;
    if (found)       *found       = g_arp_progress_found;
}

int arp_scan_get_count(void) {
    return g_arp_result_count;
}

const arp_host_t* arp_scan_get_host(int index) {
    if (index < 0 || index >= g_arp_result_count) {
        return NULL;
    }
    return &g_arp_results[index];
}

void arp_scan_clear_results(void) {
    g_arp_result_count = 0;
    if (g_arp_results) {
        free(g_arp_results);
        g_arp_results = NULL;
    }
}

/**
 * @brief Print ARP scan results (placeholder for future use)
 */
void arp_scan_print_results(void) {
    // Results are printed during scan in the current implementation
    // This function is provided for API consistency
}

// ============================================================================
// Compact Wi-Fi Packet Monitor
// ============================================================================

static volatile bool g_passive_running = false;
static volatile int g_passive_count = 0;
static volatile uint32_t g_passive_data_frames = 0;
static volatile uint32_t g_passive_protected_frames = 0;
static volatile uint32_t g_packet_feed_emitted = 0;

#define PASSIVE_ARP_MAX_HOSTS 64
typedef struct {
    uint32_t ip_addr;
    uint8_t mac[6];
} passive_arp_host_t;

typedef struct {
    char line[48];
} passive_arp_line_t;

static passive_arp_host_t *g_passive_hosts = NULL;
static passive_arp_line_t *g_passive_new_lines = NULL;
static int g_passive_host_count = 0;
static volatile bool g_passive_poll_pending = false;
static volatile bool g_passive_poll_ready = false;
static volatile int g_passive_poll_count = 0;

// Simple OUI vendor lookup table (top vendors)
typedef struct {
    uint8_t prefix[3];
    const char *vendor;
} oui_entry_t;

static const oui_entry_t oui_table[] = {
    {{0x00, 0x50, 0x56}, "VMware"},
    {{0x00, 0x0C, 0x29}, "VMware"},
    {{0x08, 0x00, 0x27}, "VirtualBox"},
    {{0x0A, 0x00, 0x27}, "VirtualBox"},
    {{0xB8, 0x27, 0xEB}, "Raspberry Pi"},
    {{0xDC, 0xA6, 0x32}, "Raspberry Pi"},
    {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
    {{0x28, 0xCD, 0xC1}, "Raspberry Pi"},
    {{0x00, 0x1A, 0x79}, "Dell"},
    {{0x00, 0x14, 0x22}, "Dell"},
    {{0xF8, 0xBC, 0x12}, "Dell"},
    {{0x00, 0x25, 0xB5}, "Dell"},
    {{0x00, 0x1E, 0x67}, "Intel"},
    {{0x00, 0x1B, 0x21}, "Intel"},
    {{0x68, 0x05, 0xCA}, "Intel"},
    {{0xA4, 0x34, 0xD9}, "Intel"},
    {{0x3C, 0x22, 0xFB}, "Apple"},
    {{0x00, 0x1C, 0xB3}, "Apple"},
    {{0xF0, 0x18, 0x98}, "Apple"},
    {{0xAC, 0xDE, 0x48}, "Apple"},
    {{0x00, 0x26, 0xBB}, "Apple"},
    {{0x78, 0x7B, 0x8A}, "Apple"},
    {{0x00, 0x03, 0x93}, "Apple"},
    {{0x94, 0xE9, 0x79}, "Apple"},
    {{0x00, 0x17, 0x88}, "Philips Hue"},
    {{0x00, 0x12, 0x47}, "TP-Link"},
    {{0x50, 0xC7, 0xBF}, "TP-Link"},
    {{0xE8, 0xDE, 0x27}, "TP-Link"},
    {{0x30, 0xB5, 0xC2}, "TP-Link"},
    {{0x00, 0x0E, 0x8F}, "Netgear"},
    {{0x20, 0xE5, 0x2A}, "Netgear"},
    {{0x44, 0x94, 0xFC}, "Netgear"},
    {{0x00, 0x1F, 0x33}, "Netgear"},
    {{0x00, 0x24, 0xD4}, "ASUS"},
    {{0xF4, 0x6D, 0x04}, "ASUS"},
    {{0x2C, 0x56, 0xDC}, "ASUS"},
    {{0x00, 0x13, 0x02}, "Samsung"},
    {{0x00, 0x1A, 0x8A}, "Samsung"},
    {{0x5C, 0x3C, 0x27}, "Samsung"},
    {{0x00, 0x21, 0xD1}, "Samsung"},
    {{0x00, 0x16, 0x32}, "Samsung"},
    {{0x00, 0x0D, 0xB9}, "Samsung"},
    {{0x00, 0x07, 0xAB}, "Samsung"},
    {{0x00, 0x02, 0x78}, "Samsung"},
    {{0x34, 0x23, 0xBA}, "Samsung"},
    {{0xF0, 0x5B, 0x7B}, "Samsung"},
    {{0xFC, 0xF1, 0x36}, "Samsung"},
    {{0x00, 0x0E, 0x6D}, "D-Link"},
    {{0x00, 0x1B, 0x11}, "D-Link"},
    {{0x1C, 0x7E, 0xE5}, "D-Link"},
    {{0x00, 0x15, 0xE9}, "D-Link"},
    {{0x00, 0x1C, 0xF0}, "D-Link"},
    {{0x30, 0x23, 0x03}, "Belkin"},
    {{0x00, 0x11, 0x50}, "Belkin"},
    {{0x00, 0x1D, 0xD8}, "HP"},
    {{0x00, 0x0D, 0x9D}, "HP"},
    {{0x00, 0x08, 0x02}, "HP"},
    {{0x00, 0x1A, 0x4B}, "HP"},
    {{0x00, 0x0B, 0xCD}, "HP"},
    {{0x00, 0x13, 0x21}, "HP"},
    {{0x00, 0x08, 0x22}, "Xiaomi"},
    {{0x28, 0x6C, 0x07}, "Xiaomi"},
    {{0x7C, 0x1D, 0xD9}, "Xiaomi"},
    {{0x64, 0x09, 0x80}, "Xiaomi"},
    {{0x78, 0x11, 0xDC}, "Xiaomi"},
    {{0xF8, 0xA4, 0x5F}, "Xiaomi"},
    {{0x00, 0x0E, 0x58}, "Sony"},
    {{0x00, 0x04, 0x1B}, "Sony"},
    {{0x00, 0x1A, 0x80}, "Sony"},
    {{0x00, 0x1E, 0x45}, "Sony"},
    {{0x00, 0x0A, 0x95}, "Cisco"},
    {{0x00, 0x01, 0x42}, "Cisco"},
    {{0x00, 0x01, 0x63}, "Cisco"},
    {{0x00, 0x01, 0x96}, "Cisco"},
    {{0x00, 0x01, 0xC7}, "Cisco"},
    {{0x00, 0x02, 0x16}, "Cisco"},
    {{0x00, 0x02, 0x4A}, "Cisco"},
    {{0x00, 0x02, 0xB9}, "Cisco"},
    {{0x00, 0x03, 0x6B}, "Cisco"},
    {{0x00, 0x03, 0xE3}, "Cisco"},
    {{0x00, 0x04, 0x9D}, "Cisco"},
    {{0x00, 0x04, 0xC1}, "Cisco"},
    {{0x00, 0x05, 0x9B}, "Cisco"},
    {{0x00, 0x06, 0x28}, "Cisco"},
    {{0x00, 0x07, 0x0D}, "Cisco"},
    {{0x00, 0x07, 0x85}, "Cisco"},
    {{0x00, 0x08, 0x20}, "Cisco"},
    {{0x00, 0x08, 0x7C}, "Cisco"},
    {{0x00, 0x09, 0x43}, "Cisco"},
    {{0x00, 0x09, 0x44}, "Cisco"},
    {{0x00, 0x09, 0x7C}, "Cisco"},
    {{0x00, 0x09, 0xB6}, "Cisco"},
    {{0x00, 0x0A, 0x41}, "Cisco"},
    {{0x00, 0x0A, 0x8A}, "Cisco"},
    {{0x00, 0x0A, 0xB8}, "Cisco"},
    {{0x00, 0x0A, 0xB6}, "Cisco"},
    {{0x00, 0x0B, 0x46}, "Cisco"},
    {{0x00, 0x0B, 0x5F}, "Cisco"},
    {{0x00, 0x0B, 0x85}, "Cisco"},
    {{0x00, 0x0B, 0xBE}, "Cisco"},
    {{0x00, 0x0B, 0xFC}, "Cisco"},
    {{0x00, 0x0C, 0x30}, "Cisco"},
    {{0x00, 0x0C, 0x85}, "Cisco"},
    {{0x00, 0x0C, 0x86}, "Cisco"},
    {{0x00, 0x0C, 0xDB}, "Cisco"},
    {{0x00, 0x0D, 0x29}, "Cisco"},
    {{0x00, 0x0D, 0x65}, "Cisco"},
    {{0x00, 0x0D, 0xBC}, "Cisco"},
    {{0x00, 0x0D, 0xEC}, "Cisco"},
    {{0x00, 0x0E, 0x38}, "Cisco"},
    {{0x00, 0x0E, 0xD7}, "Cisco"},
    {{0x00, 0x0F, 0x24}, "Cisco"},
    {{0x00, 0x0F, 0x34}, "Cisco"},
    {{0x00, 0x0F, 0x8F}, "Cisco"},
    {{0x00, 0x12, 0x00}, "Cisco"},
    {{0x00, 0x12, 0x43}, "Cisco"},
    {{0x00, 0x12, 0x7F}, "Cisco"},
    {{0x00, 0x12, 0xD9}, "Cisco"},
    {{0x00, 0x13, 0x19}, "Cisco"},
    {{0x00, 0x13, 0x7F}, "Cisco"},
    {{0x00, 0x13, 0x80}, "Cisco"},
    {{0x00, 0x14, 0x6A}, "Cisco"},
    {{0x00, 0x14, 0xA8}, "Cisco"},
    {{0x00, 0x16, 0x46}, "Cisco"},
    {{0x00, 0x16, 0x47}, "Cisco"},
    {{0x00, 0x17, 0x0E}, "Cisco"},
    {{0x00, 0x17, 0x3B}, "Cisco"},
    {{0x00, 0x17, 0x94}, "Cisco"},
    {{0x00, 0x17, 0x95}, "Cisco"},
    {{0x00, 0x18, 0x73}, "Cisco"},
    {{0x00, 0x19, 0x2F}, "Cisco"},
    {{0x00, 0x19, 0xAA}, "Cisco"},
    {{0x00, 0x19, 0xE7}, "Cisco"},
    {{0x00, 0x1A, 0x2F}, "Cisco"},
    {{0x00, 0x1A, 0x6D}, "Cisco"},
    {{0x00, 0x1A, 0xA1}, "Cisco"},
    {{0x00, 0x1A, 0xA2}, "Cisco"},
    {{0x00, 0x1B, 0x0C}, "Cisco"},
    {{0x00, 0x1B, 0x53}, "Cisco"},
    {{0x00, 0x1B, 0x54}, "Cisco"},
    {{0x00, 0x1B, 0xD4}, "Cisco"},
    {{0x00, 0x1C, 0x0E}, "Cisco"},
    {{0x00, 0x1C, 0x0F}, "Cisco"},
    {{0x00, 0x1C, 0x10}, "Cisco"},
    {{0x00, 0x1C, 0x11}, "Cisco"},
    {{0x00, 0x1E, 0x13}, "Cisco"},
    {{0x00, 0x1E, 0x14}, "Cisco"},
    {{0x00, 0x1E, 0x49}, "Cisco"},
    {{0x00, 0x1E, 0x4A}, "Cisco"},
    {{0x00, 0x1E, 0xBE}, "Cisco"},
    {{0x00, 0x1E, 0xF7}, "Cisco"},
    {{0x00, 0x20, 0x3F}, "Cisco"},
    {{0x00, 0x21, 0x55}, "Cisco"},
    {{0x00, 0x21, 0x56}, "Cisco"},
    {{0x00, 0x22, 0x55}, "Cisco"},
    {{0x00, 0x22, 0x56}, "Cisco"},
    {{0x00, 0x23, 0x04}, "Cisco"},
    {{0x00, 0x23, 0x33}, "Cisco"},
    {{0x00, 0x23, 0x5D}, "Cisco"},
    {{0x00, 0x24, 0x13}, "Cisco"},
    {{0x00, 0x24, 0x14}, "Cisco"},
    {{0x00, 0x24, 0x50}, "Cisco"},
    {{0x00, 0x24, 0xC3}, "Cisco"},
    {{0x00, 0x25, 0x45}, "Cisco"},
    {{0x00, 0x25, 0x83}, "Cisco"},
    {{0x00, 0x25, 0x84}, "Cisco"},
    {{0x00, 0x26, 0x0A}, "Cisco"},
    {{0x00, 0x26, 0x51}, "Cisco"},
    {{0x00, 0x26, 0x98}, "Cisco"},
    {{0x00, 0x26, 0x99}, "Cisco"},
    {{0x00, 0x26, 0x9A}, "Cisco"},
    {{0x00, 0x26, 0xB0}, "Cisco"},
    {{0x00, 0x26, 0xB1}, "Cisco"},
    {{0x00, 0x26, 0xF2}, "Cisco"},
    {{0x00, 0x27, 0x0C}, "Cisco"},
    {{0x00, 0x50, 0x56}, "VMware"},
    {{0x00, 0x50, 0x57}, "VMware"},
    {{0x00, 0x50, 0x58}, "VMware"},
    {{0x00, 0x0C, 0x29}, "VMware"},
    {{0x00, 0x05, 0x69}, "VMware"},
};

static const int oui_table_size = sizeof(oui_table) / sizeof(oui_table[0]);

static const char *lookup_oui_vendor(const uint8_t *mac) {
    for (int i = 0; i < oui_table_size; i++) {
        if (mac[0] == oui_table[i].prefix[0] &&
            mac[1] == oui_table[i].prefix[1] &&
            mac[2] == oui_table[i].prefix[2]) {
            return oui_table[i].vendor;
        }
    }
    return NULL;
}

const char *arp_scan_get_vendor(const uint8_t *mac) {
    if (!mac) {
        return NULL;
    }
    return lookup_oui_vendor(mac);
}

static void format_short_mac(const uint8_t *mac, char out[7]) {
    snprintf(out, 7, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static const char *packet_type_name(uint8_t type, uint8_t subtype) {
    if (type == 0) {
        switch (subtype) {
            case 0: return "ASQ";
            case 1: return "ASR";
            case 4: return "PRQ";
            case 5: return "PRS";
            case 8: return "BCN";
            case 10: return "DIS";
            case 11: return "AUT";
            case 12: return "DEA";
            default: return "MGT";
        }
    }
    if (type == 1) {
        switch (subtype) {
            case 11: return "RTS";
            case 12: return "CTS";
            case 13: return "ACK";
            default: return "CTL";
        }
    }
    if (type == 2) return (subtype & 0x08) ? "QOS" : "DAT";
    return "UNK";
}

static void format_compact_packet(const wifi_promiscuous_pkt_t *pkt,
                                  wifi_promiscuous_pkt_type_t packet_type,
                                  char *out, size_t out_size) {
    const uint8_t *frame = pkt->payload;
    size_t len = pkt->rx_ctrl.sig_len;
    uint16_t fc = (uint16_t)frame[0] | ((uint16_t)frame[1] << 8);
    uint8_t type = (uint8_t)((fc >> 2) & 0x03);
    uint8_t subtype = (uint8_t)((fc >> 4) & 0x0F);
    char flags[3] = {0};
    int flag_pos = 0;
    if (fc & 0x4000) flags[flag_pos++] = 'P';
    if (fc & 0x0800) flags[flag_pos++] = 'R';

    char dst[7] = "------";
    char src[7] = "------";
    if (len >= 10) format_short_mac(&frame[4], dst);
    if (len >= 16 && !(type == 1 && (subtype == 12 || subtype == 13))) {
        format_short_mac(&frame[10], src);
    }

    const char *name = packet_type_name(type, subtype);
    if (packet_type == WIFI_PKT_DATA && len >= 24) {
        bool to_ds = (fc & 0x0100) != 0;
        bool from_ds = (fc & 0x0200) != 0;
        size_t header_len = (to_ds && from_ds) ? 30 : 24;
        if (subtype & 0x08) {
            header_len += 2;
            if (fc & 0x8000) header_len += 4;
        }
        if (len >= header_len + 8) {
            const uint8_t *llc = frame + header_len;
            if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03) {
                uint16_t ether_type = ((uint16_t)llc[6] << 8) | llc[7];
                if (ether_type == 0x0806 && len >= header_len + 8 + 28) {
                    const uint8_t *arp = llc + 8;
                    snprintf(out, out_size, "%02u %4d ARP %u.%u>%u.%u %s",
                             (unsigned int)pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                             (unsigned int)arp[16], (unsigned int)arp[17],
                             (unsigned int)arp[26], (unsigned int)arp[27], src);
                    return;
                }
                if (ether_type == 0x888E) name = "EAP";
                else if (ether_type == 0x0800) name = "IP4";
                else if (ether_type == 0x86DD) name = "IP6";
            }
        }
    }

    snprintf(out, out_size, "%02u %4d %s %s>%s %u%s",
             (unsigned int)pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi, name, src, dst,
             (unsigned int)len, flags);
}

static bool passive_neighbor_is_new(uint32_t ip_addr, const uint8_t *mac) {
    if (!g_passive_hosts) return false;
    for (int i = 0; i < g_passive_host_count; i++) {
        if (g_passive_hosts[i].ip_addr != ip_addr) continue;
        if (memcmp(g_passive_hosts[i].mac, mac, 6) == 0) return false;
        memcpy(g_passive_hosts[i].mac, mac, 6);
        return true;
    }

    if (g_passive_host_count >= PASSIVE_ARP_MAX_HOSTS) return false;
    g_passive_hosts[g_passive_host_count].ip_addr = ip_addr;
    memcpy(g_passive_hosts[g_passive_host_count].mac, mac, 6);
    g_passive_host_count++;
    return true;
}

/* Runs on the tcpip thread: the ARP table read is inherently serialized. */
static void passive_arp_table_poll_callback(void *ctx) {
    (void)ctx;
    int new_count = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE && new_count < PASSIVE_ARP_MAX_HOSTS; i++) {
        ip4_addr_t *ip = NULL;
        struct netif *entry_netif = NULL;
        struct eth_addr *eth = NULL;
        if (!etharp_get_entry(i, &ip, &entry_netif, &eth) || !ip || !eth) continue;
        if (!passive_neighbor_is_new(ip->addr, eth->addr)) continue;

        const uint8_t *ip_bytes = (const uint8_t *)&ip->addr;
        char short_mac[7];
        format_short_mac(eth->addr, short_mac);
        const char *vendor = lookup_oui_vendor(eth->addr);
        if (vendor) {
            snprintf(g_passive_new_lines[new_count].line, sizeof(g_passive_new_lines[0].line),
                     "NBR %u.%u %s %.16s",
                     (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3],
                     short_mac, vendor);
        } else {
            snprintf(g_passive_new_lines[new_count].line, sizeof(g_passive_new_lines[0].line),
                     "NBR %u.%u %s",
                     (unsigned int)ip_bytes[2], (unsigned int)ip_bytes[3], short_mac);
        }
        new_count++;
    }
    __sync_synchronize();
    g_passive_poll_count = new_count;
    g_passive_poll_ready = true;
    g_passive_poll_pending = false;
}

static void poll_passive_arp_table(void) {
    if (!g_passive_new_lines) return;

    if (g_passive_poll_ready) {  /* drain results of the previous poll */
        int count = g_passive_poll_count;
        __sync_synchronize();
        for (int i = 0; i < count; i++) {
            glog("%s\n", g_passive_new_lines[i].line);
        }
        g_passive_poll_ready = false;
        return;
    }
    if (g_passive_poll_pending) return;  /* one request in flight */

    g_passive_poll_pending = true;
    if (tcpip_callback(passive_arp_table_poll_callback, NULL) != ERR_OK) {
        g_passive_poll_pending = false;
    }
}

// Lock-free ring buffer for packet lines (observer writes, main loop reads)
#define MONITOR_RING_SIZE 64
#define MONITOR_LINE_MAX 48
static char (*g_monitor_ring)[MONITOR_LINE_MAX] = NULL;
static volatile uint32_t g_monitor_ring_head = 0;
static volatile uint32_t g_monitor_ring_tail = 0;

static void monitor_ring_push(const char *line) {
    if (!g_monitor_ring) return;
    uint32_t next = (g_monitor_ring_head + 1) % MONITOR_RING_SIZE;
    if (next == g_monitor_ring_tail) return; // full, drop
    memcpy(g_monitor_ring[g_monitor_ring_head], line, MONITOR_LINE_MAX);
    g_monitor_ring[g_monitor_ring_head][MONITOR_LINE_MAX - 1] = '\0';
    __sync_synchronize();
    g_monitor_ring_head = next;
}

/**
 * @brief Lightweight observer attached to the Wireshark raw capture callback.
 *
 * Only pushes formatted lines into a lock-free ring buffer. No FreeRTOS
 * or glog calls from this context.
 */
static void packet_feed_observer(const wifi_promiscuous_pkt_t *pkt,
                                 wifi_promiscuous_pkt_type_t type) {
    if (!g_passive_running || !pkt || pkt->rx_ctrl.sig_len < 24) return;

    g_passive_data_frames++;
    uint16_t fc = (uint16_t)pkt->payload[0] | ((uint16_t)pkt->payload[1] << 8);
    if (fc & 0x4000) g_passive_protected_frames++;

    // Rate limit: emit every 4th packet
    if ((g_passive_data_frames & 3) != 0) return;

    char line[MONITOR_LINE_MAX];
    format_compact_packet(pkt, type, line, sizeof(line));
    if (strstr(line, " ARP ")) g_passive_count++;
    monitor_ring_push(line);
    g_packet_feed_emitted++;
}

/**
 * @brief Start a compact local Wi-Fi packet feed.
 *
 * Uses the same raw callback as Wireshark streaming, but emits bounded text
 * summaries to the local terminal instead of binary PCAP records.
 */
void arp_scan_start_passive(int duration_sec) {
    if (g_passive_running) {
        glog("Packet Monitor: Already running\n");
        return;
    }

    glog("Packet Monitor: CH RSSI TYP SRC>DST LEN FLAGS\n");
    glog("Packet Monitor: Press Back to stop\n\n");

    g_passive_hosts = calloc(PASSIVE_ARP_MAX_HOSTS, sizeof(*g_passive_hosts));
    g_passive_new_lines = calloc(PASSIVE_ARP_MAX_HOSTS, sizeof(*g_passive_new_lines));
    g_monitor_ring = calloc(MONITOR_RING_SIZE, sizeof(*g_monitor_ring));
    if (!g_passive_hosts || !g_passive_new_lines || !g_monitor_ring) {
        free(g_passive_hosts);
        free(g_passive_new_lines);
        free(g_monitor_ring);
        g_passive_hosts = NULL;
        g_passive_new_lines = NULL;
        g_monitor_ring = NULL;
        glog("Packet Monitor: insufficient memory\n");
        return;
    }

    g_passive_running = true;
    g_passive_count = 0;
    g_passive_data_frames = 0;
    g_passive_protected_frames = 0;
    g_packet_feed_emitted = 0;
    g_passive_host_count = 0;
    g_monitor_ring_head = 0;
    g_monitor_ring_tail = 0;

    /* The raw callback normally also feeds the binary PCAP writer. This local
     * text monitor only needs its observer tap. */
    wifi_callbacks_set_pcap_enabled(false);
    wifi_raw_set_observer(packet_feed_observer);
    wifi_manager_start_monitor_mode(wifi_raw_scan_callback);
    wifi_manager_start_wireshark_channel_hop();

    int elapsed_ticks = 0;
    while (g_passive_running &&
           (duration_sec <= 0 || elapsed_ticks < duration_sec * 20)) {
        // Drain ring buffer (safe: main loop is the only reader)
        while (g_monitor_ring_tail != g_monitor_ring_head) {
            __sync_synchronize();
            glog("%s\n", g_monitor_ring[g_monitor_ring_tail]);
            g_monitor_ring_tail = (g_monitor_ring_tail + 1) % MONITOR_RING_SIZE;
        }
        elapsed_ticks++;
        if (elapsed_ticks % 10 == 0) poll_passive_arp_table();
        if (elapsed_ticks % 200 == 0) {
            glog("MON %lup %da %lue %dn\n",
                 (unsigned long)g_passive_data_frames,
                 g_passive_count,
                 (unsigned long)g_packet_feed_emitted,
                 g_passive_host_count);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    wifi_raw_set_observer(NULL);
    wifi_manager_stop_wireshark_channel_hop();
    wifi_manager_stop_monitor_mode();
    wifi_callbacks_set_pcap_enabled(true);
    g_passive_running = false;

    /* Drain any in-flight ARP table poll before freeing the buffers: the
     * callback runs on the tcpip thread and touches g_passive_new_lines. */
    while (g_passive_poll_pending) taskYIELD();

    free(g_passive_hosts);
    free(g_passive_new_lines);
    free(g_monitor_ring);
    g_passive_hosts = NULL;
    g_passive_new_lines = NULL;
    g_monitor_ring = NULL;
    g_passive_poll_ready = false;
    g_passive_poll_count = 0;

    glog("\nPacket Monitor: Stopped (%lup %da %luP)\n",
         (unsigned long)g_passive_data_frames,
         g_passive_count,
         (unsigned long)g_passive_protected_frames);
}

/**
 * @brief Stop the compact Wi-Fi packet monitor
 */
void arp_scan_stop_passive(void) {
    g_passive_running = false;
}
