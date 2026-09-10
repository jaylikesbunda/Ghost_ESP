#include "sdkconfig.h"

#ifdef CONFIG_WITH_ETHERNET

#include "managers/ethernet/eth_fingerprint.h"
#include "managers/ethernet/eth_http.h"

#include "core/glog.h"
#include "core/system_manager.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include <esp_timer.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MDNS_PORT 5353
#define MDNS_MULTICAST_ADDR "224.0.0.251"
#define NBNS_PORT 137
#define SSDP_PORT 1900
#define SSDP_MULTICAST_ADDR "239.255.255.250"
#define ETH_FINGERPRINT_TIMEOUT_MS 3000
#define ETH_FINGERPRINT_MAX_HOSTS 32

// --- Async scan state ---
static eth_fp_results_t   s_last_results;
static volatile bool      s_fp_running      = false;
static volatile bool      s_fp_done         = false;
static volatile bool      s_fp_cancel       = false;
static TaskHandle_t       s_fp_task_handle  = NULL;

static const char *DEVICE_KEYWORDS[] = {
    "Chromecast", "eero", "Roku", "Apple", "Samsung", "LG", "Sony", "Philips",
    "NVIDIA", "Amazon", "Google", "Microsoft", "Sonos", "Nest", "Ring", "TP-Link",
    "Netgear", "Asus", "Ubiquiti", "Synology", "QNAP", "Buffalo", "D-Link", "Linksys"
};
    
typedef struct {
    uint32_t ip;
    char name[64];
    char device_type[48];
    char protocol[16];
    char service_type[96];
    char os_info[64];
} eth_discovered_host_t;

static int eth_ascii_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

static int eth_ascii_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca == '\0' || cb == '\0') return (int)eth_ascii_tolower(ca) - (int)eth_ascii_tolower(cb);
        int da = eth_ascii_tolower(ca);
        int db = eth_ascii_tolower(cb);
        if (da != db) return da - db;
    }
    return 0;
}

static bool eth_ssdp_header_get(const char *resp, const char *key, char *out, size_t out_len) {
    if (!resp || !key || !out || out_len == 0) return false;
    out[0] = '\0';

    size_t key_len = strlen(key);
    const char *p = resp;
    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);

        if (line_len > key_len + 1 && eth_ascii_strncasecmp(p, key, key_len) == 0 && p[key_len] == ':') {
            size_t start = key_len + 1;
            while (start < line_len && p[start] == ' ') start++;

            size_t copy_len = line_len - start;
            if (copy_len >= out_len) copy_len = out_len - 1;
            memcpy(out, p + start, copy_len);
            out[copy_len] = '\0';
            return true;
        }

        if (!line_end) break;
        p = line_end + 2;
    }
    return false;
}

static inline bool eth_fingerprint_is_cancelled(void) {
    return s_fp_cancel;
}

static void eth_xml_get_tag_value(const char *xml, const char *tag, char *out, size_t out_len) {
    if (!xml || !tag || !out || out_len == 0) return;
    out[0] = '\0';

    char open_tag[64];
    char close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return;
    start += strlen(open_tag);

    const char *end = strstr(start, close_tag);
    if (!end) return;

    size_t len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
}

static void eth_fingerprint_mdns_scan(eth_discovered_host_t *hosts, int *count, int max_hosts) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        glog("mDNS: Failed to create socket\n");
        return;
    }

    uint8_t *buf = malloc(512);
    if (!buf) {
        close(sock);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        free(buf);
        close(sock);
        return;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_MULTICAST_ADDR);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    uint8_t query[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x09, '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's',
        0x07, '_', 'd', 'n', 's', '-', 's', 'd',
        0x04, '_', 'u', 'd', 'p',
        0x05, 'l', 'o', 'c', 'a', 'l',
        0x00, 0x00, 0x0c, 0x00, 0x01
    };

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = inet_addr(MDNS_MULTICAST_ADDR)
    };
    sendto(sock, query, sizeof(query), 0, (struct sockaddr *)&dest, sizeof(dest));

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int64_t start = esp_timer_get_time() / 1000;

    while (!eth_fingerprint_is_cancelled()
        && (esp_timer_get_time() / 1000 - start) < ETH_FINGERPRINT_TIMEOUT_MS
        && *count < max_hosts) {
        int len = recvfrom(sock, buf, 512, 0, (struct sockaddr *)&from, &fromlen);
        if (len > 12) {
            bool found = false;
            for (int i = 0; i < *count; i++) {
                if (hosts[i].ip == from.sin_addr.s_addr) { found = true; break; }
            }
            if (!found) {
                hosts[*count].ip = from.sin_addr.s_addr;
                snprintf(hosts[*count].name, sizeof(hosts[*count].name), "Unknown");
                snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "mDNS Responder");
                snprintf(hosts[*count].protocol, sizeof(hosts[*count].protocol), "mDNS");
                snprintf(hosts[*count].service_type, sizeof(hosts[*count].service_type), "mDNS Service");
                (*count)++;
            }
        }
    }

    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
    free(buf);
    close(sock);
}

static void eth_fingerprint_nbns_scan(eth_discovered_host_t *hosts, int *count, int max_hosts) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        glog("NBNS: Failed to create socket\n");
        return;
    }

    uint8_t *buf = malloc(512);
    if (!buf) {
        close(sock);
        return;
    }

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(NBNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        free(buf);
        close(sock);
        return;
    }

    uint8_t query[] = {
        0x80, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x20, 0x43, 0x4b, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
        0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
        0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x00, 0x00, 0x21,
        0x00, 0x01
    };

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(NBNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST)
    };
    sendto(sock, query, sizeof(query), 0, (struct sockaddr *)&dest, sizeof(dest));

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int64_t start = esp_timer_get_time() / 1000;

    while (!eth_fingerprint_is_cancelled()
        && (esp_timer_get_time() / 1000 - start) < ETH_FINGERPRINT_TIMEOUT_MS
        && *count < max_hosts) {
        int len = recvfrom(sock, buf, 512, 0, (struct sockaddr *)&from, &fromlen);
        if (len > 56) {
            bool found = false;
            for (int i = 0; i < *count; i++) {
                if (hosts[i].ip == from.sin_addr.s_addr) { found = true; break; }
            }
            if (!found) {
                hosts[*count].ip = from.sin_addr.s_addr;
                int name_count = buf[56];
                if (name_count > 0 && len > 57 + 18) {
                    memset(hosts[*count].name, 0, sizeof(hosts[*count].name));
                    for (int j = 0; j < 15 && buf[57 + j] != ' ' && buf[57 + j] != 0; j++) {
                        hosts[*count].name[j] = buf[57 + j];
                    }
                } else {
                    snprintf(hosts[*count].name, sizeof(hosts[*count].name), "Unknown");
                }
                snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "Windows/Samba");
                snprintf(hosts[*count].protocol, sizeof(hosts[*count].protocol), "NetBIOS");
                snprintf(hosts[*count].service_type, sizeof(hosts[*count].service_type), "File Sharing");
                (*count)++;
            }
        }
    }
    free(buf);
    close(sock);
}

static void eth_fingerprint_ssdp_scan(eth_discovered_host_t *hosts, int *count, int max_hosts) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    uint8_t *buf = malloc(1024);
    char *http_resp = malloc(1024);
    if (!buf || !http_resp) {
        free(buf);
        free(http_resp);
        close(sock);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in local_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };
    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        free(buf);
        free(http_resp);
        close(sock);
        return;
    }

    const char *ssdp_query =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(SSDP_PORT),
        .sin_addr.s_addr = inet_addr(SSDP_MULTICAST_ADDR)
    };
    sendto(sock, ssdp_query, strlen(ssdp_query), 0, (struct sockaddr *)&dest, sizeof(dest));

    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int64_t start = esp_timer_get_time() / 1000;

    while (!eth_fingerprint_is_cancelled()
        && (esp_timer_get_time() / 1000 - start) < ETH_FINGERPRINT_TIMEOUT_MS
        && *count < max_hosts) {
        int len = recvfrom(sock, buf, 1023, 0, (struct sockaddr *)&from, &fromlen);
        if (len > 0) {
            buf[len] = '\0';

            bool found = false;
            for (int i = 0; i < *count; i++) {
                if (hosts[i].ip == from.sin_addr.s_addr) { found = true; break; }
            }
            if (!found) {
                hosts[*count].ip = from.sin_addr.s_addr;
                snprintf(hosts[*count].protocol, sizeof(hosts[*count].protocol), "SSDP");
                snprintf(hosts[*count].name, sizeof(hosts[*count].name), "Unknown");
                snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "UPnP Device");
                snprintf(hosts[*count].service_type, sizeof(hosts[*count].service_type), "Unknown");
                hosts[*count].os_info[0] = '\0';

                char st[96];
                char location[128];
                char server[64];
                st[0] = '\0';
                location[0] = '\0';
                server[0] = '\0';

                eth_ssdp_header_get((char *)buf, "ST", st, sizeof(st));
                eth_ssdp_header_get((char *)buf, "LOCATION", location, sizeof(location));
                eth_ssdp_header_get((char *)buf, "SERVER", server, sizeof(server));

                char manufacturer[48];
                char model[48];
                manufacturer[0] = '\0';
                model[0] = '\0';

                if (location[0] != '\0' && strncmp(location, "http://", 7) == 0) {
                    int http_len = eth_http_get_simple(location, http_resp, 1024, 300);
                    if (http_len > 0) {
                        const char *body = strstr(http_resp, "\r\n\r\n");
                        if (body) body += 4;
                        else body = http_resp;

                        char friendly[64];
                        friendly[0] = '\0';
                        eth_xml_get_tag_value(body, "friendlyName", friendly, sizeof(friendly));
                        eth_xml_get_tag_value(body, "manufacturer", manufacturer, sizeof(manufacturer));
                        eth_xml_get_tag_value(body, "modelName", model, sizeof(model));

                        if (friendly[0] != '\0') {
                            snprintf(hosts[*count].name, sizeof(hosts[*count].name), "%s", friendly);
                        }
                    }
                }

                if (strcmp(hosts[*count].name, "Unknown") == 0) {
                    char usn[128];
                    usn[0] = '\0';
                    eth_ssdp_header_get((char *)buf, "USN", usn, sizeof(usn));
                    if (usn[0] != '\0') {
                        const char *uuid_start = strstr(usn, "uuid:");
                        if (uuid_start) {
                            uuid_start += 5;
                            const char *uuid_end = strchr(uuid_start, ':');
                            if (uuid_end) {
                                size_t uuid_len = (size_t)(uuid_end - uuid_start);
                                if (uuid_len > 8) {
                                    snprintf(hosts[*count].name, sizeof(hosts[*count].name), "%.*s", (int)(uuid_len > 16 ? 16 : uuid_len), uuid_start);
                                }
                            }
                        }
                    }
                }

                if (model[0] != '\0') {
                    snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "%s", model);
                } else if (manufacturer[0] != '\0') {
                    snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "%s", manufacturer);
                } else if (st[0] != '\0') {
                    const char *last = strrchr(st, ':');
                    if (last && *(last + 1) != '\0') {
                        snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "%s", last + 1);
                    } else {
                        snprintf(hosts[*count].device_type, sizeof(hosts[*count].device_type), "%.*s", (int)sizeof(hosts[*count].device_type) - 1, st);
                    }
                }

                if (st[0] != '\0') {
                    snprintf(hosts[*count].service_type, sizeof(hosts[*count].service_type), "%s", st);
                }

                if (server[0] != '\0') {
                    snprintf(hosts[*count].os_info, sizeof(hosts[*count].os_info), "%s", server);
                    if (strcmp(hosts[*count].name, "Unknown") == 0) {
                        for (size_t k = 0; k < sizeof(DEVICE_KEYWORDS) / sizeof(DEVICE_KEYWORDS[0]); k++) {
                            if (strstr(server, DEVICE_KEYWORDS[k])) {
                                snprintf(hosts[*count].name, sizeof(hosts[*count].name), "%s", DEVICE_KEYWORDS[k]);
                                break;
                            }
                        }
                    }
                }

                (*count)++;
            }
        }
    }
    free(buf);
    free(http_resp);
    close(sock);
}

void eth_fingerprint_run_scan(void) {
    glog("\n--- Network Fingerprint Scan ---\n");
    glog("Scanning for mDNS, NetBIOS, and SSDP hosts (3s)...\n");

    bool used_caps_alloc = false;
    eth_discovered_host_t *hosts = NULL;
#if defined(CONFIG_SPIRAM)
    hosts = (eth_discovered_host_t *)heap_caps_calloc(ETH_FINGERPRINT_MAX_HOSTS, sizeof(eth_discovered_host_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (hosts) used_caps_alloc = true;
#endif
    if (!hosts) {
        hosts = calloc(ETH_FINGERPRINT_MAX_HOSTS, sizeof(eth_discovered_host_t));
    }
    if (!hosts) {
        glog("Failed to allocate memory for scan\n");
        return;
    }

    int count = 0;
    eth_fingerprint_mdns_scan(hosts, &count, ETH_FINGERPRINT_MAX_HOSTS);
    if (!eth_fingerprint_is_cancelled()) {
        eth_fingerprint_nbns_scan(hosts, &count, ETH_FINGERPRINT_MAX_HOSTS);
    }
    if (!eth_fingerprint_is_cancelled()) {
        eth_fingerprint_ssdp_scan(hosts, &count, ETH_FINGERPRINT_MAX_HOSTS);
    }

    if (count == 0) {
        glog("No hosts discovered\n");
    } else {
        glog("Discovered %d host(s):\n\n", count);
        for (int i = 0; i < count; i++) {
            struct in_addr addr = { .s_addr = hosts[i].ip };
            const char *title = (strcmp(hosts[i].name, "Unknown") != 0) ? hosts[i].name : hosts[i].device_type;
            glog("- %s\n", title);
            glog("    IP: %s\n", inet_ntoa(addr));
            glog("    Type: %s\n", hosts[i].device_type);
            if (hosts[i].service_type[0] != '\0' && strcmp(hosts[i].service_type, "Unknown") != 0) {
                glog("    Service: %s\n", hosts[i].service_type);
            }
            if (hosts[i].os_info[0] != '\0') {
                glog("    OS: %s\n", hosts[i].os_info);
            }
            glog("    Protocol: %s\n", hosts[i].protocol);
            if (i < count - 1) {
                glog("\n");
            }

            // Copy host into async result store
            if (s_last_results.count < 32) {
                eth_fp_host_t *h = &s_last_results.hosts[s_last_results.count];
                struct in_addr _addr = { .s_addr = hosts[i].ip };
                strlcpy(h->ip_str,      inet_ntoa(_addr),        sizeof(h->ip_str));
                strlcpy(h->name,        hosts[i].name,           sizeof(h->name));
                strlcpy(h->device_type, hosts[i].device_type,    sizeof(h->device_type));
                strlcpy(h->protocol,    hosts[i].protocol,       sizeof(h->protocol));
                strlcpy(h->service_type, hosts[i].service_type,  sizeof(h->service_type));
                strlcpy(h->os_info,     hosts[i].os_info,        sizeof(h->os_info));
                s_last_results.count++;
            }
        }
    }

    if (used_caps_alloc) {
        heap_caps_free(hosts);
    } else {
        free(hosts);
    }
}

// --- Async API implementation ---

static void eth_fp_task(void *arg) {
    s_fp_running = true;
    s_fp_done    = false;
    s_fp_cancel  = false;
    memset(&s_last_results, 0, sizeof(s_last_results));
    eth_fingerprint_run_scan();
    s_fp_running     = false;
    s_fp_done        = true;
    s_fp_task_handle = NULL;
    vTaskDeleteWithCaps(NULL);
}

void eth_fingerprint_start_async(void) {
    if (s_fp_running) return;
    s_fp_done = false;
    xTaskCreate_psram(eth_fp_task, "eth_fp_scan", 6144, NULL, 5, &s_fp_task_handle);
}

bool eth_fingerprint_scan_is_running(void) { return s_fp_running; }
bool eth_fingerprint_scan_is_done(void)    { return s_fp_done;    }
void eth_fingerprint_scan_cancel(void)     { s_fp_cancel = true;  }

int eth_fingerprint_get_results(eth_fp_results_t *out) {
    if (!out) return 0;
    memcpy(out, &s_last_results, sizeof(*out));
    return out->count;
}

const eth_fp_results_t *eth_fingerprint_get_last_results(void) {
    return &s_last_results;
}

#else

#include "managers/ethernet/eth_fingerprint.h"

void eth_fingerprint_run_scan(void) {
}

#endif
