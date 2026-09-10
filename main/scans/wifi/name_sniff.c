// name_sniff.c
// Passive local-name sniffer: mDNS (:5353), LLMNR (:5355), SSDP (:1900) and
// NetBIOS-NS (:137). All are multicast/broadcast, so any STA sees them with
// no spoofing and no disconnect. One active mDNS + SSDP probe is sent at
// start to solicit replies (same as a normal client joining, not an attack).

#include "scans/wifi/name_sniff.h"
#include "core/glog.h"
#include "core/scan_saver.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define NS_MDNS_PORT 5353
#define NS_LLMNR_PORT 5355
#define NS_SSDP_PORT 1900
#define NS_NBNS_PORT 137
#define NS_BUF_LEN 1024
#define NS_MAX_SOCKS 4

static TaskHandle_t s_task = NULL;
static volatile bool s_run = false;
static char s_filter[16] = {0};
static char s_self_ip[16] = {0};
static scan_file_t s_file = SCAN_FILE_INIT;
static bool s_saving = false;
static uint32_t s_shown = 0;

static bool filter_match(const char *src_ip) {
    if (s_filter[0] == '\0') return true;
    return strcmp(s_filter, src_ip) == 0;
}

static void emit(const char *src_ip, const char *proto, const char *info) {
    if (!filter_match(src_ip)) return;
    s_shown++;
    glog("%s %s %s\n", src_ip, proto, info);
    if (s_saving) {
        scan_file_printf(&s_file, "%lld,%s,%s,%s\n",
                         (long long)(esp_timer_get_time() / 1000),
                         src_ip, proto, info);
    }
}

// Decode one DNS name at *pos (handles compression pointers). On success
// writes the dotted name to out and sets *next to the offset right after the
// name bytes (i.e. past the pointer, not the jump target). Returns false on
// truncation.
static bool decode_name(const uint8_t *pkt, int len, int pos, char *out,
                        size_t out_len, int *next) {
    int npos = 0;
    int jumps = 0;
    int end = -1;
    bool done = false;
    while (pos < len && jumps < 8) {
        uint8_t lbl = pkt[pos];
        if (lbl == 0) {
            if (end < 0) end = pos + 1;
            done = true;
            break;
        }
        if ((lbl & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            if (end < 0) end = pos + 2;
            pos = ((lbl & 0x3F) << 8) | pkt[pos + 1];
            jumps++;
            continue;
        }
        if (lbl > 63 || pos + 1 + lbl > len) return false;
        if ((size_t)(npos + lbl + 1) >= out_len) return false;
        memcpy(out + npos, pkt + pos + 1, lbl);
        npos += lbl;
        out[npos++] = '.';
        pos += 1 + lbl;
    }
    if (!done || npos == 0) return false;
    out[npos - 1] = '\0';
    if (next) *next = (end >= 0) ? end : pos;
    return true;
}

static uint16_t rd_u16(const uint8_t *pkt, int len, int pos) {
    if (pos + 1 >= len) return 0;
    return (uint16_t)((pkt[pos] << 8) | pkt[pos + 1]);
}

// Walk questions + answers. Emits owner names plus the interesting bits of
// rdata (PTR/CNAME targets, SRV targets, A/AAAA addresses); caps output so
// one chatty packet can't flood the terminal.
static void emit_dns_names(const uint8_t *pkt, int len, const char *src_ip,
                           const char *proto) {
    if (len < 12) return;
    int qd = rd_u16(pkt, len, 4);
    int an = rd_u16(pkt, len, 6);
    int ns = rd_u16(pkt, len, 8);
    int ar = rd_u16(pkt, len, 10);
    int pos = 12;
    int names = 0;
    char name[96];

    for (int i = 0; i < qd && names < 8; i++) {
        int next = 0;
        if (!decode_name(pkt, len, pos, name, sizeof(name), &next)) return;
        pos = next;
        if (pos + 4 > len) return;
        pos += 4; // QTYPE + QCLASS
        emit(src_ip, proto, name);
        names++;
    }

    int rrs = an + ns + ar;
    for (int i = 0; i < rrs && names < 8; i++) {
        int next = 0;
        if (!decode_name(pkt, len, pos, name, sizeof(name), &next)) return;
        pos = next;
        if (pos + 10 > len) return;
        uint16_t type = rd_u16(pkt, len, pos);
        uint16_t rdlen = rd_u16(pkt, len, pos + 8);
        pos += 10;
        if (pos + rdlen > len) return;
        emit(src_ip, proto, name);
        names++;
        if ((type == 12 || type == 5 || type == 2) && names < 8) {
            // PTR / CNAME / NS: rdata is another name.
            char tgt[96];
            int dummy = 0;
            if (decode_name(pkt, len, pos, tgt, sizeof(tgt), &dummy)) {
                emit(src_ip, proto, tgt);
                names++;
            }
        } else if (type == 33 && rdlen > 6 && names < 8) {
            // SRV: priority/weight/port + target name.
            char tgt[96];
            int dummy = 0;
            if (decode_name(pkt, len, pos + 6, tgt, sizeof(tgt), &dummy)) {
                emit(src_ip, proto, tgt);
                names++;
            }
        } else if (type == 1 && rdlen == 4 && names < 8) {
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u",
                     pkt[pos], pkt[pos + 1], pkt[pos + 2], pkt[pos + 3]);
            emit(src_ip, proto, ip);
            names++;
        }
        pos += rdlen;
    }
}

// NetBIOS-NS RFC1002 half-ASCII decode of the question name.
static void emit_nbns_name(const uint8_t *pkt, int len, const char *src_ip) {
    if (len < 12 + 34 + 4) return;
    const uint8_t *enc = pkt + 12 + 1; // skip length byte (0x20)
    char name[17];
    int n = 0;
    for (int i = 0; i < 16 && n < 16; i++) {
        uint8_t hi = enc[i * 2] - 'A';
        uint8_t lo = enc[i * 2 + 1] - 'A';
        if (hi > 15 || lo > 15) return;
        char c = (char)((hi << 4) | lo);
        if (c == 0x20) break;
        name[n++] = (c >= 32 && c < 127) ? c : '?';
    }
    name[n] = '\0';
    if (n > 0) emit(src_ip, "NBNS", name);
}

static void emit_ssdp_headers(const char *buf, const char *src_ip) {
    char line[160];
    const char *p = buf;
    while (*p) {
        int i = 0;
        while (*p && *p != '\n' && i < (int)sizeof(line) - 1) {
            if (*p != '\r') line[i++] = *p;
            p++;
        }
        if (*p == '\n') p++;
        line[i] = '\0';
        if (strncasecmp(line, "ST:", 3) == 0 ||
            strncasecmp(line, "USN:", 4) == 0 ||
            strncasecmp(line, "SERVER:", 7) == 0 ||
            strncasecmp(line, "LOCATION:", 9) == 0) {
            emit(src_ip, "SSDP", line);
        }
    }
}

static int open_udp(uint16_t port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(port);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void join_group(int sock, const char *multi) {
    struct ip_mreq mreq = {0};
    mreq.imr_multiaddr.s_addr = inet_addr(multi);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
}

static void send_probe(int sock, const char *multi, uint16_t port,
                       const uint8_t *pkt, size_t pkt_len) {
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = inet_addr(multi);
    sendto(sock, pkt, pkt_len, 0, (struct sockaddr *)&dest, sizeof(dest));
}

static void name_sniff_task(void *arg) {
    (void)arg;
    s_self_ip[0] = '\0';
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            ip4addr_ntoa_r(&ip_info.ip, s_self_ip, sizeof(s_self_ip));
        }
    }
    int socks[NS_MAX_SOCKS] = {-1, -1, -1, -1};
    int n = 0;

    int mdns = open_udp(NS_MDNS_PORT);
    if (mdns >= 0) {
        join_group(mdns, "224.0.0.251");
        socks[n++] = mdns;
    }
    int llmnr = open_udp(NS_LLMNR_PORT);
    if (llmnr >= 0) {
        join_group(llmnr, "224.0.0.252");
        socks[n++] = llmnr;
    }
    int ssdp = open_udp(NS_SSDP_PORT);
    if (ssdp >= 0) {
        join_group(ssdp, "239.255.255.250");
        socks[n++] = ssdp;
    }
    int nbns = open_udp(NS_NBNS_PORT);
    if (nbns >= 0) socks[n++] = nbns;

    if (n == 0) {
        glog("Name sniff: no sockets available.\n");
        s_run = false;
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Solicit replies like any joining client (not an attack).
    static const uint8_t mdns_probe[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x09, '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's',
        0x07, '_', 'd', 'n', 's', '-', 's', 'd',
        0x04, '_', 'u', 'd', 'p',
        0x05, 'l', 'o', 'c', 'a', 'l',
        0x00, 0x00, 0x0c, 0x00, 0x01
    };
    static const char ssdp_probe[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
        "MAN: \"ns=01; ns=01\"\r\nMX: 2\r\nST: upnp:rootdevice\r\n\r\n";
    if (mdns >= 0) send_probe(mdns, "224.0.0.251", NS_MDNS_PORT, mdns_probe, sizeof(mdns_probe));
    if (ssdp >= 0) send_probe(ssdp, "239.255.255.250", NS_SSDP_PORT,
                              (const uint8_t *)ssdp_probe, strlen(ssdp_probe));

    uint8_t buf[NS_BUF_LEN];
    while (s_run) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < n; i++) {
            FD_SET(socks[i], &rfds);
            if (socks[i] > maxfd) maxfd = socks[i];
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 500000};
        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel <= 0) continue;
        for (int i = 0; i < n && s_run; i++) {
            if (!FD_ISSET(socks[i], &rfds)) continue;
            struct sockaddr_in from = {0};
            socklen_t fromlen = sizeof(from);
            int len = recvfrom(socks[i], buf, sizeof(buf) - 1, 0,
                               (struct sockaddr *)&from, &fromlen);
            if (len <= 0) continue;
            char src[16] = {0};
            inet_ntoa_r(from.sin_addr, src, sizeof(src));
            src[sizeof(src) - 1] = '\0';
            if (src[0] == '\0') continue;
            if (s_self_ip[0] && strcmp(src, s_self_ip) == 0) continue; // our own probe echo
            if (socks[i] == ssdp) {
                buf[len] = '\0';
                emit_ssdp_headers((const char *)buf, src);
            } else if (socks[i] == nbns) {
                emit_nbns_name(buf, len, src);
            } else if (socks[i] == mdns) {
                if (len > 12) emit_dns_names(buf, len, src, "mDNS");
            } else {
                if (len > 12) emit_dns_names(buf, len, src, "LLMNR");
            }
        }
    }

    for (int i = 0; i < n; i++) close(socks[i]);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t name_sniff_start(const char *ip_filter) {
    if (!ip_filter || !ip_filter[0]) return ESP_ERR_INVALID_ARG;
    name_sniff_stop();

    if (strcmp(ip_filter, "all") == 0) {
        s_filter[0] = '\0';
    } else {
        strlcpy(s_filter, ip_filter, sizeof(s_filter));
    }

    s_shown = 0;
    s_saving = false;
    memset(&s_file, 0, sizeof(s_file));

    char prefix[32] = "names_all";
    if (s_filter[0]) {
        char safe[16];
        strlcpy(safe, s_filter, sizeof(safe));
        for (char *p = safe; *p; p++) {
            if (*p == '.') *p = '_';
        }
        snprintf(prefix, sizeof(prefix), "names_%s", safe);
    }
    if (scan_file_open(&s_file, prefix, "csv") == ESP_OK) {
        s_saving = true;
        scan_file_printf(&s_file, "timestamp_ms,src_ip,proto,info\n");
    }

    s_run = true;
    if (xTaskCreate(name_sniff_task, "name_sniff", 4096, NULL, 5, &s_task) != pdPASS) {
        s_run = false;
        s_task = NULL;
        if (s_saving) {
            scan_file_close(&s_file);
            s_saving = false;
        }
        return ESP_ERR_NO_MEM;
    }

    if (s_filter[0]) {
        glog("Sniffing local names from %s (mDNS/LLMNR/SSDP/NBNS)... (`stop` to end)\n", s_filter);
    } else {
        glog("Sniffing local names from all hosts (mDNS/LLMNR/SSDP/NBNS)... (`stop` to end)\n");
    }
    if (s_saving) {
        glog("Saving to %s\n", s_file.path);
    } else {
        glog("Auto-save off (Settings > Capture & Location > Saving): listening only.\n");
    }
    return ESP_OK;
}

void name_sniff_stop(void) {
    if (!s_run && s_task == NULL) {
        if (s_saving) {
            scan_file_close(&s_file);
            s_saving = false;
        }
        return;
    }
    s_run = false;
    for (int i = 0; i < 40 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_task = NULL;
    if (s_saving) {
        glog("Name sniff stopped (%lu shown, saved).\n", (unsigned long)s_shown);
        scan_file_close(&s_file);
        s_saving = false;
    } else if (s_shown > 0) {
        glog("Name sniff stopped (%lu shown).\n", (unsigned long)s_shown);
    }
    s_shown = 0;
}

bool name_sniff_is_running(void) {
    return s_run;
}

const char *name_sniff_get_filter(void) {
    return s_filter;
}

const char *name_sniff_get_path(void) {
    return s_saving ? s_file.path : "";
}
