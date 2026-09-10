#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_scan_async.h"
#include "managers/ethernet_manager.h"
#include "core/glog.h"
#include "core/system_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

// esp_netif_get_netif_impl is an internal ESP-IDF function not in public headers.
// Forward-declare it the same way eth_arp_poison.c does at its line 43.
void *esp_netif_get_netif_impl(esp_netif_t *esp_netif);

static const char *TAG = "EthScanAsync";

// Cap ARP sweep range derived from the real netmask (protects against very
// large subnets, e.g. a /16, turning into an hours-long scan).
#define ETH_ARP_SCAN_MAX_HOSTS 512

static eth_scan_results_t s_results;
static volatile bool      s_running      = false;
static volatile bool      s_done         = false;
static volatile bool      s_cancelled    = false;
static TaskHandle_t       s_task_handle  = NULL;

// --- Helper: map port number to service name ---
static const char *port_to_service(uint16_t port) {
    switch (port) {
        case 21:   return "FTP";
        case 22:   return "SSH";
        case 23:   return "Telnet";
        case 25:   return "SMTP";
        case 53:   return "DNS";
        case 80:   return "HTTP";
        case 110:  return "POP3";
        case 111:  return "RPC";
        case 135:  return "MSRPC";
        case 139:  return "NetBIOS";
        case 143:  return "IMAP";
        case 443:  return "HTTPS";
        case 445:  return "SMB";
        case 993:  return "IMAPS";
        case 995:  return "POP3S";
        case 1723: return "PPTP";
        case 3306: return "MySQL";
        case 3389: return "RDP";
        case 5900: return "VNC";
        case 8080: return "HTTP-Alt";
        case 8443: return "HTTPS-Alt";
        default:   return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// ARP scan task
// Mirrors handle_eth_arp_cmd: sends ARP requests in batches across the
// actual subnet derived from the interface IP & real netmask (not a
// hardcoded /24 -- smaller VLAN subnets need the true range), then reads
// the lwIP ARP table and stores hits in s_results.arp_hosts[]. Batch size
// and wait times are tuned to match scans/wifi/arp_scan.c.
// ---------------------------------------------------------------------------
static void arp_scan_task(void *arg) {
    ESP_LOGI(TAG, "ARP scan task started");

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting ARP scan");
        goto done;
    }

    {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No valid IP, aborting ARP scan");
            goto done;
        }

        esp_netif_t *eth_netif = ethernet_manager_get_netif();
        if (eth_netif == NULL) {
            ESP_LOGW(TAG, "No netif, aborting ARP scan");
            goto done;
        }

        struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(eth_netif);
        if (lwip_netif == NULL) {
            ESP_LOGW(TAG, "No lwip netif, aborting ARP scan");
            goto done;
        }

        const uint32_t ip_host      = ntohl(ip_info.ip.addr);
        const uint32_t netmask_host = ntohl(ip_info.netmask.addr);
        const uint32_t network_host = ip_host & netmask_host;
        const uint32_t bcast_host   = network_host | (~netmask_host);

        uint32_t start_host = network_host + 1;
        uint32_t end_host   = (bcast_host > network_host) ? bcast_host - 1 : network_host;
        if (end_host < start_host) {
            start_host = network_host;
            end_host   = bcast_host;
        }
        if (end_host - start_host + 1 > ETH_ARP_SCAN_MAX_HOSTS) {
            end_host = start_host + ETH_ARP_SCAN_MAX_HOSTS - 1;
        }

        char subnet_prefix[16];
        ip4_addr_t network_addr;
        network_addr.addr = htonl(network_host);
        ip4addr_ntoa_r(&network_addr, subnet_prefix, sizeof(subnet_prefix));

        ESP_LOGI(TAG, "ARP scan: %s (%u hosts)", subnet_prefix,
                 (unsigned)(end_host - start_host + 1));

        const uint32_t batch_size = 20;

        s_results.progress_total   = (int)(end_host - start_host + 1);
        s_results.progress_current = 0;

        for (uint32_t batch_start = start_host;
             batch_start <= end_host && !s_cancelled;
             batch_start += batch_size) {

            uint32_t batch_end = batch_start + batch_size - 1;
            if (batch_end > end_host) batch_end = end_host;

            // Send ARP requests for this batch
            for (uint32_t host = batch_start; host <= batch_end && !s_cancelled; host++) {
                if (host == ip_host) continue;
                ip4_addr_t target_addr;
                target_addr.addr = htonl(host);
                etharp_request(lwip_netif, &target_addr);
                vTaskDelay(pdMS_TO_TICKS(5));
            }

            if (s_cancelled) break;

            // Wait for ARP replies
            vTaskDelay(pdMS_TO_TICKS(80));

            // Check ARP table for each host in the batch
            for (uint32_t host = batch_start; host <= batch_end && !s_cancelled; host++) {
                if (host == ip_host) continue;
                ip4_addr_t target_addr;
                target_addr.addr = htonl(host);

                struct eth_addr    *eth_ret = NULL;
                const ip4_addr_t   *ip_ret  = NULL;
                s8_t arp_idx = etharp_find_addr(lwip_netif, &target_addr, &eth_ret, &ip_ret);

                if (arp_idx >= 0 && eth_ret) {
                    if (s_results.arp_count < 64) {
                        eth_arp_result_t *r = &s_results.arp_hosts[s_results.arp_count];
                        ip4addr_ntoa_r(&target_addr, r->ip_str, sizeof(r->ip_str));
                        memcpy(r->mac, eth_ret->addr, 6);
                        r->hostname[0] = '\0'; // hostname resolution not attempted
                        s_results.arp_count++;
                    }
                }

                s_results.progress_current = (int)(host - start_host + 1);
            }
        }
    }

done:
    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "ARP scan done: %d hosts found", s_results.arp_count);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------------------
// Port scan task
// Mirrors handle_eth_ports_cmd:
//   PORT_LOCAL: scans the 20 common ports against the target IP (or gateway).
//   PORT_ALL  : scans ports 1-65535 against the target IP.
// Uses non-blocking TCP connect + select() with a 500 ms timeout, exactly
// as commandline.c does.
// ---------------------------------------------------------------------------
static void port_scan_task(void *arg) {
    ESP_LOGI(TAG, "Port scan task started for %s (type=%d)",
             s_results.target_ip, s_results.type);

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting port scan");
        goto done;
    }

    {
        // If no target IP was supplied, fall back to the gateway
        if (s_results.target_ip[0] == '\0') {
            esp_netif_ip_info_t ip_info;
            if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.gw.addr == 0) {
                ESP_LOGW(TAG, "No target IP and no gateway, aborting port scan");
                goto done;
            }
            ip4addr_ntoa_r(&ip_info.gw, s_results.target_ip, sizeof(s_results.target_ip));
        }

        struct sockaddr_in server_addr;
        memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, s_results.target_ip, &server_addr.sin_addr) != 1) {
            ESP_LOGW(TAG, "Invalid target IP: %s", s_results.target_ip);
            goto done;
        }

        bool scan_all = (s_results.type == ETH_SCAN_TYPE_PORT_ALL);

        if (scan_all) {
            // ---- Scan all ports 1-65535 ----
            const uint32_t start_port = 1;
            const uint32_t end_port   = 65535;
            s_results.progress_total   = (int)(end_port - start_port + 1);
            s_results.progress_current = 0;

            for (uint32_t port = start_port;
                 port <= end_port && !s_cancelled;
                 port++) {

                s_results.progress_current = (int)(port - start_port + 1);

                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (sock < 0) {
                    continue;
                }

                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                server_addr.sin_port = htons((uint16_t)port);
                int scan_result = connect(sock, (struct sockaddr *)&server_addr,
                                          sizeof(server_addr));

                bool connected = false;
                if (scan_result == 0) {
                    connected = true;
                } else if (scan_result < 0 && errno == EINPROGRESS) {
                    uint32_t elapsed = 0;
                    const uint32_t interval = 50;
                    while (elapsed < 500 && !s_cancelled) {
                        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(interval * 1000) };
                        fd_set fdset;
                        FD_ZERO(&fdset);
                        FD_SET(sock, &fdset);
                        if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                            int error = 0;
                            socklen_t len = sizeof(error);
                            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 &&
                                error == 0) {
                                connected = true;
                            }
                            break;
                        }
                        elapsed += interval;
                    }
                }

                if (connected && s_results.port_count < 256) {
                    eth_port_result_t *r = &s_results.port_results[s_results.port_count];
                    r->port = (uint16_t)port;
                    strlcpy(r->service, port_to_service((uint16_t)port), sizeof(r->service));
                    s_results.port_count++;
                }

                close(sock);
            }
        } else {
            // ---- Scan the 20 common ports (PORT_LOCAL) ----
            static const uint16_t COMMON_PORTS[] = {
                21, 22, 23, 25, 53, 80, 110, 111, 135, 139,
                143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080
            };
            const size_t NUM_COMMON_PORTS = sizeof(COMMON_PORTS) / sizeof(COMMON_PORTS[0]);

            s_results.progress_total   = (int)NUM_COMMON_PORTS;
            s_results.progress_current = 0;

            for (size_t i = 0; i < NUM_COMMON_PORTS && !s_cancelled; i++) {
                uint16_t port = COMMON_PORTS[i];
                s_results.progress_current = (int)(i + 1);

                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (sock < 0) {
                    continue;
                }

                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);

                server_addr.sin_port = htons(port);
                int scan_result = connect(sock, (struct sockaddr *)&server_addr,
                                          sizeof(server_addr));

                bool connected = false;
                if (scan_result == 0) {
                    connected = true;
                } else if (scan_result < 0 && errno == EINPROGRESS) {
                    uint32_t elapsed = 0;
                    const uint32_t interval = 50;
                    while (elapsed < 500 && !s_cancelled) {
                        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(interval * 1000) };
                        fd_set fdset;
                        FD_ZERO(&fdset);
                        FD_SET(sock, &fdset);
                        if (select(sock + 1, NULL, &fdset, NULL, &tv) > 0) {
                            int error = 0;
                            socklen_t len = sizeof(error);
                            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) >= 0 &&
                                error == 0) {
                                connected = true;
                            }
                            break;
                        }
                        elapsed += interval;
                    }
                }

                if (connected && s_results.port_count < 256) {
                    eth_port_result_t *r = &s_results.port_results[s_results.port_count];
                    r->port = port;
                    strlcpy(r->service, port_to_service(port), sizeof(r->service));
                    s_results.port_count++;
                }

                close(sock);
            }
        }
    }

done:
    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "Port scan done: %d open ports on %s",
             s_results.port_count, s_results.target_ip);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------------------
// Ping sweep task
// Mirrors handle_eth_ping_cmd: raw ICMP echo to each host in the actual
// DHCP-assigned subnet (derived from the real netmask, not a hardcoded /24 --
// smaller VLAN subnets, e.g. /25-/28, need the real range or hosts outside
// the true subnet get pinged for nothing while real neighbors get skipped).
// Range is capped at ETH_PING_SWEEP_MAX_HOSTS for very large subnets.
// ---------------------------------------------------------------------------
#define ETH_PING_SWEEP_MAX_HOSTS 512

static void ping_scan_task(void *arg) {
    ESP_LOGI(TAG, "Ping sweep task started");

    if (!ethernet_manager_is_connected()) {
        ESP_LOGW(TAG, "Ethernet not connected, aborting ping sweep");
        goto done;
    }

    {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) != ESP_OK || ip_info.ip.addr == 0) {
            ESP_LOGW(TAG, "No valid IP, aborting ping sweep");
            goto done;
        }

        const uint32_t ip_host       = ntohl(ip_info.ip.addr);
        const uint32_t netmask_host  = ntohl(ip_info.netmask.addr);
        const uint32_t network_host  = ip_host & netmask_host;
        const uint32_t bcast_host    = network_host | (~netmask_host);

        uint32_t first_host = network_host + 1;
        uint32_t last_host  = (bcast_host > network_host) ? bcast_host - 1 : network_host;
        if (last_host < first_host) {
            // Degenerate mask (/31, /32): fall back to the two addresses we have.
            first_host = network_host;
            last_host  = bcast_host;
        }
        if (last_host - first_host + 1 > ETH_PING_SWEEP_MAX_HOSTS) {
            last_host = first_host + ETH_PING_SWEEP_MAX_HOSTS - 1;
        }

        int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (sock < 0) {
            ESP_LOGE(TAG, "Failed to create raw ICMP socket: %d", errno);
            goto done;
        }

        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        typedef struct {
            uint8_t  type;
            uint8_t  code;
            uint16_t checksum;
            uint16_t id;
            uint16_t seq;
        } icmp_hdr_t;

        s_results.ping_total   = 0;
        s_results.ping_alive   = 0;
        s_results.progress_total   = (int)(last_host - first_host + 1);
        s_results.progress_current = 0;

        uint16_t seq = 0;
        for (uint32_t target_host = first_host; target_host <= last_host && !s_cancelled; target_host++) {
            seq++;
            s_results.progress_current = (int)seq;

            if (target_host == ip_host) {
                continue; // skip own IP
            }

            s_results.ping_total++;

            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = htonl(target_host);

            // Build ICMP echo request (type 8) with checksum
            icmp_hdr_t icmp = {0};
            icmp.type     = 8;
            icmp.code     = 0;
            icmp.id       = 0xBEEF;
            icmp.seq      = htons(seq);

            uint16_t *buf16 = (uint16_t *)&icmp;
            uint32_t sum = 0;
            for (int i = 0; i < (int)(sizeof(icmp) / 2); i++) {
                sum += buf16[i];
            }
            sum = (sum >> 16) + (sum & 0xFFFF);
            sum += (sum >> 16);
            icmp.checksum = (uint16_t)(~sum);

            sendto(sock, &icmp, sizeof(icmp), 0,
                   (struct sockaddr *)&addr, sizeof(addr));

            uint8_t recv_buf[256];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int r = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr *)&from, &fromlen);
            if (r > 0 && from.sin_addr.s_addr == addr.sin_addr.s_addr) {
                s_results.ping_alive++;
                if (s_results.arp_count < 64) {
                    eth_arp_result_t *hr = &s_results.arp_hosts[s_results.arp_count];
                    ip4_addr_t resp_addr;
                    resp_addr.addr = from.sin_addr.s_addr;
                    ip4addr_ntoa_r(&resp_addr, hr->ip_str, sizeof(hr->ip_str));
                    memset(hr->mac, 0, 6);
                    hr->hostname[0] = '\0';
                    s_results.arp_count++;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        close(sock);
    }

done:
    s_results.done = true;
    s_done         = true;
    s_running      = false;
    s_results.cancelled = s_cancelled;
    ESP_LOGI(TAG, "Ping sweep done: %d/%d alive",
             s_results.ping_alive, s_results.ping_total);
    vTaskDeleteWithCaps(NULL);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void eth_scan_reset(void) {
    memset(&s_results, 0, sizeof(s_results));
    s_running   = false;
    s_done      = false;
    s_cancelled = false;
}

bool eth_scan_is_running(void) {
    return s_running;
}

bool eth_scan_is_done(void) {
    return s_done;
}

eth_scan_type_t eth_scan_get_type(void) {
    return s_results.type;
}

const eth_scan_results_t *eth_scan_get_results(void) {
    return &s_results;
}

void eth_scan_cancel(void) {
    s_cancelled = true;
    // Give the task up to 2 s to exit cleanly
    for (int i = 0; i < 20 && s_running; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void eth_scan_start_arp(void) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = ETH_SCAN_TYPE_ARP;
    s_running = true;
    xTaskCreate_psram(arp_scan_task, "eth_arp_scan", 8192, NULL, 5, &s_task_handle);
}

void eth_scan_start_port(const char *target_ip, bool scan_all) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = scan_all ? ETH_SCAN_TYPE_PORT_ALL : ETH_SCAN_TYPE_PORT_LOCAL;
    if (target_ip) {
        strlcpy(s_results.target_ip, target_ip, sizeof(s_results.target_ip));
    }
    s_running = true;
    xTaskCreate_psram(port_scan_task, "eth_port_scan", 8192, NULL, 5, &s_task_handle);
}

void eth_scan_start_ping(void) {
    if (s_running) return;
    eth_scan_reset();
    s_results.type = ETH_SCAN_TYPE_PING;
    s_running = true;
    xTaskCreate_psram(ping_scan_task, "eth_ping_scan", 8192, NULL, 5, &s_task_handle);
}

#endif // CONFIG_WITH_ETHERNET
