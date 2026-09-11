// cmd_shell.c
// Small headless-shell conveniences shared by the serial and WebUI consoles.

#include "core/commandline.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "core/shell.h"
#include "core/ghostesp_version.h"
#include "core/utils.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/ip4_addr.h"
#include "lwip/sockets.h"
#include "managers/sd_card_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "managers/ghost_raw_radio.h"
#endif
#include "esp_idf_version.h"
#ifdef CONFIG_WITH_ETHERNET
#include "managers/ethernet_manager.h"
#endif
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "nvs.h"

#ifndef GIT_COMMIT_HASH
#define GIT_COMMIT_HASH "unknown"
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH "unknown"
#endif

#define SHELL_NVS_NAMESPACE "cli"
#define CLI_ALIAS_COUNT 8
#define CLI_ALIAS_NAME_LEN 32
#define CLI_ALIAS_VALUE_LEN 256
#define CLI_ENV_COUNT 8
#define CLI_ENV_NAME_LEN 24
#define CLI_ENV_VALUE_LEN 96

typedef struct {
    char hostname[CLI_ALIAS_NAME_LEN];
    char color[8];
    bool banner;
    char alias_names[CLI_ALIAS_COUNT][CLI_ALIAS_NAME_LEN];
    char alias_values[CLI_ALIAS_COUNT][CLI_ALIAS_VALUE_LEN];
    char env_names[CLI_ENV_COUNT][CLI_ENV_NAME_LEN];
    char env_values[CLI_ENV_COUNT][CLI_ENV_VALUE_LEN];
} shell_state_t;

static shell_state_t *s_shell;
static bool s_shell_loaded;
#define s_hostname     (s_shell->hostname)
#define s_color        (s_shell->color)
#define s_banner       (s_shell->banner)
#define s_alias_names  (s_shell->alias_names)
#define s_alias_values (s_shell->alias_values)
#define s_env_names    (s_shell->env_names)
#define s_env_values   (s_shell->env_values)
static TaskHandle_t s_watch_task;
static volatile bool s_watch_stop;
static int s_watch_interval;

static bool shell_load(void) {
    if (s_shell_loaded) return true;
    s_shell = calloc(1, sizeof(*s_shell));
    if (!s_shell) return false;
    strcpy(s_hostname, "ghost");
    strcpy(s_color, "36");
    s_banner = true;
    s_shell_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return true;

    size_t len = sizeof(s_hostname);
    (void)nvs_get_str(nvs, "hostname", s_hostname, &len);
    len = sizeof(s_color);
    (void)nvs_get_str(nvs, "color", s_color, &len);
    uint8_t banner = 1;
    if (nvs_get_u8(nvs, "banner", &banner) == ESP_OK) s_banner = banner != 0;

    for (int i = 0; i < CLI_ALIAS_COUNT; ++i) {
        char key[8];
        char value[CLI_ALIAS_VALUE_LEN];
        snprintf(key, sizeof(key), "alias%d", i);
        len = sizeof(value);
        if (nvs_get_str(nvs, key, value, &len) != ESP_OK) continue;
        char *separator = strchr(value, '=');
        if (!separator) continue;
        *separator++ = '\0';
        strncpy(s_alias_names[i], value, sizeof(s_alias_names[i]) - 1);
        strncpy(s_alias_values[i], separator, sizeof(s_alias_values[i]) - 1);
    }

    for (int i = 0; i < CLI_ENV_COUNT; ++i) {
        char key[8];
        char value[CLI_ENV_NAME_LEN + CLI_ENV_VALUE_LEN];
        snprintf(key, sizeof(key), "env%d", i);
        len = sizeof(value);
        if (nvs_get_str(nvs, key, value, &len) != ESP_OK) continue;
        char *separator = strchr(value, '=');
        if (!separator) continue;
        *separator++ = '\0';
        strncpy(s_env_names[i], value, sizeof(s_env_names[i]) - 1);
        strncpy(s_env_values[i], separator, sizeof(s_env_values[i]) - 1);
    }
    nvs_close(nvs);
    return true;
}

static bool shell_save_string(const char *key, const char *value) {
    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static bool shell_save_u8(const char *key, uint8_t value) {
    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

void shell_get_prompt(char *output, size_t output_len) {
    if (!output || output_len == 0) return;
    if (!shell_load()) {
        snprintf(output, output_len, "ghost> ");
        return;
    }
    if (strcmp(s_color, "off") == 0) {
        snprintf(output, output_len, "%s> ", s_hostname);
    } else {
        snprintf(output, output_len, "\033[38;5;%sm%s\033[0m> ", s_color, s_hostname);
    }
}

void shell_set_hostname(const char *hostname) {
    if (!shell_load()) return;
    strncpy(s_hostname, hostname, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';
    shell_save_string("hostname", s_hostname);
}

bool shell_get_banner_enabled(void) {
    return shell_load() && s_banner;
}

void shell_set_banner_enabled(bool enabled) {
    if (!shell_load()) return;
    s_banner = enabled;
    shell_save_u8("banner", enabled ? 1 : 0);
}

void shell_set_color(const char *color) {
    if (!shell_load()) return;
    strncpy(s_color, color, sizeof(s_color) - 1);
    s_color[sizeof(s_color) - 1] = '\0';
    shell_save_string("color", s_color);
}

const char *shell_get_color(void) {
    return shell_load() ? s_color : "36";
}

bool shell_stop_watch(void) {
    if (!s_watch_task) return false;
    s_watch_stop = true;
    return true;
}

static int alias_index(const char *name) {
    if (!shell_load()) return -1;
    for (int i = 0; i < CLI_ALIAS_COUNT; ++i) {
        if (s_alias_names[i][0] && strcasecmp(s_alias_names[i], name) == 0) return i;
    }
    return -1;
}

static bool valid_name(const char *name, size_t max_len) {
    if (!name || !name[0] || strlen(name) >= max_len) return false;
    for (const char *p = name; *p; ++p) {
        if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
    }
    return true;
}

static bool alias_save(int index) {
    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    char key[8];
    snprintf(key, sizeof(key), "alias%d", index);
    char value[CLI_ALIAS_NAME_LEN + CLI_ALIAS_VALUE_LEN + 2];
    snprintf(value, sizeof(value), "%s=%s", s_alias_names[index], s_alias_values[index]);
    esp_err_t err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

static void alias_remove(int index) {
    if (index < 0 || index >= CLI_ALIAS_COUNT || !shell_load()) return;
    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        char key[8];
        snprintf(key, sizeof(key), "alias%d", index);
        nvs_erase_key(nvs, key);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    s_alias_names[index][0] = '\0';
    s_alias_values[index][0] = '\0';
}

static const char *env_value(const char *name) {
    if (!shell_load()) return NULL;
    if (strcasecmp(name, "HOSTNAME") == 0) return s_hostname;
    if (strcasecmp(name, "CLI_COLOR") == 0) return s_color;
    if (strcasecmp(name, "CLI_BANNER") == 0) return s_banner ? "on" : "off";
    for (int i = 0; i < CLI_ENV_COUNT; ++i) {
        if (s_env_names[i][0] && strcasecmp(s_env_names[i], name) == 0) return s_env_values[i];
    }
    return NULL;
}

bool shell_expand_command(const char *input, char *output, size_t output_len) {
    if (!input || !output || output_len == 0) return false;
    if (!shell_load()) return false;
    char expanded[512];
    const char *start = input;
    while (isspace((unsigned char)*start)) start++;
    const char *end = start;
    while (*end && !isspace((unsigned char)*end)) end++;
    size_t first_len = (size_t)(end - start);
    int index = -1;
    if (first_len > 0 && first_len < CLI_ALIAS_NAME_LEN) {
        char first[CLI_ALIAS_NAME_LEN];
        memcpy(first, start, first_len);
        first[first_len] = '\0';
        index = alias_index(first);
    }

    if (index >= 0) {
        snprintf(expanded, sizeof(expanded), "%s%s%s", s_alias_values[index],
                 (*end ? " " : ""), end);
    } else {
        strncpy(expanded, input, sizeof(expanded) - 1);
        expanded[sizeof(expanded) - 1] = '\0';
    }

    /* Expand only simple $NAME variables. This deliberately does not interpret shell syntax. */
    size_t out = 0;
    for (size_t i = 0; expanded[i] && out + 1 < output_len; ++i) {
        if (expanded[i] != '$') {
            output[out++] = expanded[i];
            continue;
        }
        size_t name_start = i + 1;
        size_t name_end = name_start;
        while (isalnum((unsigned char)expanded[name_end]) || expanded[name_end] == '_') name_end++;
        if (name_end == name_start) {
            output[out++] = expanded[i];
            continue;
        }
        char name[CLI_ENV_NAME_LEN];
        size_t name_len = name_end - name_start;
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, expanded + name_start, name_len);
        name[name_len] = '\0';
        const char *value = env_value(name);
        if (!value) value = "";
        while (*value && out + 1 < output_len) output[out++] = *value++;
        i = name_end - 1;
    }
    output[out] = '\0';
    return index >= 0;
}

static int edit_distance(const char *a, const char *b) {
    int previous[64] = {0};
    int current[64] = {0};
    size_t blen = strlen(b);
    if (blen >= 64) blen = 63;
    for (size_t j = 0; j <= blen; ++j) previous[j] = (int)j;
    for (size_t i = 1; a[i - 1] && i < 64; ++i) {
        current[0] = (int)i;
        for (size_t j = 1; j <= blen; ++j) {
            int cost = tolower((unsigned char)a[i - 1]) != tolower((unsigned char)b[j - 1]);
            int insert = current[j - 1] + 1;
            int remove = previous[j] + 1;
            int replace = previous[j - 1] + cost;
            current[j] = insert < remove ? insert : remove;
            if (replace < current[j]) current[j] = replace;
        }
        memcpy(previous, current, sizeof(previous));
    }
    return previous[blen];
}

void shell_suggest_command(const char *command) {
    if (!command || !command[0]) return;
    int best_distance = 99;
    const char *best = NULL;
    for (size_t i = 0;; ++i) {
        const char *name = command_name_at(i);
        if (!name) break;
        int distance = edit_distance(command, name);
        if (distance < best_distance) {
            best_distance = distance;
            best = name;
        }
    }
    if (best && best_distance <= 3) glog("Did you mean '%s'?\n", best);
}

void shell_print_command_help(const char *command) {
    static const struct {
        const char *name;
        const char *usage;
    } help[] = {
        {"echo", "echo <text>"}, {"ifconfig", "ifconfig"}, {"ping", "ping <host> [count]"},
        {"version", "version"}, {"uuid", "uuid"}, {"macaddr", "macaddr [all|sta|ap]"},
        {"uptime", "uptime"}, {"date", "date"}, {"whoami", "whoami"}, {"status", "status"},
        {"clear", "clear"}, {"hostname", "hostname [name]"}, {"color", "color [name|0-255|off]"},
        {"banner", "banner [on|off|status]"}, {"alias", "alias <name> <command>"},
        {"unalias", "unalias <name|all>"}, {"history", "history [-c]"}, {"ps", "ps"},
        {"top", "top"}, {"df", "df"}, {"tail", "tail <file> [lines]"},
        {"grep", "grep <pattern> <file>"}, {"source", "source <file>"},
        {"tee", "tee <file> <text>"}, {"env", "env"}, {"export", "export NAME=value"},
        {"watch", "watch <seconds> <command> | watch stop"},
        {"hop", "hop [auto|all|basic|custom <channels>|<channels>]"},
        {"glbench", "glbench [send|recv|both] [kb] | glbench stop | glbench status"}
    };
    for (size_t i = 0; i < sizeof(help) / sizeof(help[0]); ++i) {
        if (strcasecmp(command, help[i].name) == 0) {
            glog("Usage: %s\n", help[i].usage);
            return;
        }
    }
    glog("Use 'help <category>' for command details. Try 'help shell' for headless commands.\n");
}

static void print_ip_info(const char *label, esp_netif_t *netif, bool active) {
    esp_netif_ip_info_t info;
    uint8_t mac[6] = {0};
    char ip[16] = "0.0.0.0";
    char mask[16] = "0.0.0.0";
    char gw[16] = "0.0.0.0";
    bool has_info = netif && esp_netif_get_ip_info(netif, &info) == ESP_OK;
    if (has_info) {
        ip4addr_ntoa_r(&info.ip, ip, sizeof(ip));
        ip4addr_ntoa_r(&info.netmask, mask, sizeof(mask));
        ip4addr_ntoa_r(&info.gw, gw, sizeof(gw));
    }
    bool has_mac = netif && esp_netif_get_mac(netif, mac) == ESP_OK;
    glog("%s: %s\n", label, active ? "UP" : "DOWN");
    glog("  inet %s  netmask %s  gateway %s\n", ip, mask, gw);
    if (has_mac) glog("  ether %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void handle_echo_cmd(int argc, char **argv) {
    char text[512];
    size_t out = 0;
    for (int i = 1; i < argc && out + 1 < sizeof(text); ++i) {
        if (i > 1 && out + 1 < sizeof(text)) text[out++] = ' ';
        for (const char *p = argv[i]; *p && out + 1 < sizeof(text); ++p) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') text[out++] = '\n';
                else if (*p == 't') text[out++] = '\t';
                else if (*p == 'r') text[out++] = '\r';
                else if (*p == '\\') text[out++] = '\\';
                else { text[out++] = '\\'; if (out + 1 < sizeof(text)) text[out++] = *p; }
            } else {
                text[out++] = *p;
            }
        }
    }
    text[out] = '\0';
    glog("%s\n", text);
}

void handle_ifconfig_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    print_ip_info("STA", esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), is_wifi_sta_connected());
#if CONFIG_IDF_TARGET_ESP32P4
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dhcp_status_t dhcp_status;
    if (sta && esp_netif_dhcpc_get_status(sta, &dhcp_status) == ESP_OK) {
        glog("  netif %s  DHCP %s\n", esp_netif_is_netif_up(sta) ? "UP" : "DOWN",
             dhcp_status == ESP_NETIF_DHCP_STARTED ? "running" :
             dhcp_status == ESP_NETIF_DHCP_STOPPED ? "stopped" : "not started");
        esp_netif_ip_info_t ip;
        if (esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK && !ip.ip.addr) {
            glog("  No IPv4 lease yet; Wi-Fi association alone does not mean network access.\n");
        }
    }
    uint8_t radio_mac[6] = {0}, netif_mac[6] = {0};
    esp_err_t mac_err = esp_wifi_get_mac(WIFI_IF_STA, radio_mac);
    if (mac_err == ESP_OK) {
        const char *comparison = "cannot compare with";
        if (sta && esp_netif_get_mac(sta, netif_mac) == ESP_OK) {
            comparison = memcmp(radio_mac, netif_mac, sizeof(radio_mac)) == 0 ?
                         "matches" : "DIFFERS FROM";
        }
        glog("  C6 radio MAC %02x:%02x:%02x:%02x:%02x:%02x (%s host netif)\n",
             radio_mac[0], radio_mac[1], radio_mac[2], radio_mac[3], radio_mac[4], radio_mac[5],
             comparison);
    } else {
        glog("  C6 radio MAC query: %s\n", esp_err_to_name(mac_err));
    }
    esp_err_t diag_err = ghost_wifi_request_sta_diag();
    if (diag_err == ESP_OK) glog("  C6 STA counters requested; asynchronous P4_STA reply follows.\n");
    else glog("  C6 STA diagnostic request failed: %s\n", esp_err_to_name(diag_err));
#endif
    print_ip_info("AP", esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                  mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
#ifdef CONFIG_WITH_ETHERNET
    print_ip_info("ETH", ethernet_manager_get_netif(), ethernet_manager_is_connected());
#endif
}

static uint16_t icmp_checksum(const void *data, size_t len) {
    const uint16_t *words = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *words++; len -= 2; }
    if (len) sum += *(const uint8_t *)words;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

void handle_ping_cmd(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        glog("Usage: ping <host> [count]\n");
        return;
    }
    int count = argc == 3 ? atoi(argv[2]) : 4;
    if (count < 1 || count > 20) { glog("ping: count must be 1-20\n"); return; }

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    struct addrinfo *result = NULL;
    if (getaddrinfo(argv[1], NULL, &hints, &result) != 0 || !result) {
        glog("ping: cannot resolve %s\n", argv[1]);
        return;
    }
    struct sockaddr_in target = *(struct sockaddr_in *)result->ai_addr;
    freeaddrinfo(result);

    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) { glog("ping: raw socket unavailable (%d)\n", errno); return; }
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char ip[16];
    inet_ntoa_r(target.sin_addr, ip, sizeof(ip));
    glog("PING %s (%s):\n", argv[1], ip);

    int received = 0;
    for (int seq = 0; seq < count; ++seq) {
        struct {
            uint8_t type;
            uint8_t code;
            uint16_t checksum;
            uint16_t id;
            uint16_t sequence;
        } packet = {8, 0, 0, htons(0x4745), htons((uint16_t)seq)};
        packet.checksum = icmp_checksum(&packet, sizeof(packet));
        int64_t started = esp_timer_get_time();
        if (sendto(sock, &packet, sizeof(packet), 0, (struct sockaddr *)&target, sizeof(target)) < 0) {
            glog("ping: send failed (%d)\n", errno);
            break;
        }
        uint8_t reply[256];
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        int length = recvfrom(sock, reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_len);
        int64_t elapsed = (esp_timer_get_time() - started) / 1000;
        size_t header_len = length > 0 ? (size_t)((reply[0] & 0x0f) * 4) : 0;
        bool valid = length >= (int)(header_len + 8) && reply[header_len] == 0 &&
                     memcmp(&from.sin_addr, &target.sin_addr, sizeof(target.sin_addr)) == 0;
        if (valid) {
            received++;
            glog("%d bytes from %s: icmp_seq=%d time=%lld ms\n", length - (int)header_len,
                 ip, seq, (long long)elapsed);
        } else {
            glog("Request timeout for icmp_seq=%d\n", seq);
        }
        if (seq + 1 < count) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    close(sock);
    glog("--- %s ping statistics ---\n", argv[1]);
    glog("%d packets transmitted, %d received, %d%% packet loss\n", count, received,
         count ? ((count - received) * 100) / count : 0);
}

void handle_version_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    const esp_app_desc_t *app = esp_app_get_description();
    glog("%s %s (%s)\n", GHOSTESP_NAME, GHOSTESP_VERSION, GHOSTESP_FLAVOR);
    glog("Build: %s %s\n", __DATE__, __TIME__);
    glog("Git: %s @ %s\n", GIT_BRANCH, GIT_COMMIT_HASH);
    if (app) glog("App: %s\n", app->version);
    glog("IDF: %s\n", esp_get_idf_version());
}

static void print_mac_for_type(esp_mac_type_t type, const char *label) {
    uint8_t mac[6];
    if (esp_read_mac(mac, type) != ESP_OK) { glog("%s: unavailable\n", label); return; }
    glog("%s: %02x:%02x:%02x:%02x:%02x:%02x\n", label, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void handle_macaddr_cmd(int argc, char **argv) {
    const char *type = argc > 1 ? argv[1] : "all";
    if (strcmp(type, "all") == 0) {
        print_mac_for_type(ESP_MAC_WIFI_STA, "STA");
        print_mac_for_type(ESP_MAC_WIFI_SOFTAP, "AP");
        return;
    }
    if (strcmp(type, "sta") == 0) print_mac_for_type(ESP_MAC_WIFI_STA, "STA");
    else if (strcmp(type, "ap") == 0) print_mac_for_type(ESP_MAC_WIFI_SOFTAP, "AP");
    else glog("Usage: macaddr [all|sta|ap]\n");
}

void handle_uuid_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) { glog("UUID unavailable\n"); return; }
    glog("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[0], mac[1],
         mac[2], mac[3], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void handle_uptime_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t seconds = (uint64_t)(esp_timer_get_time() / 1000000ULL);
    glog("up %llud %02lluh %02llum %02llus\n", (unsigned long long)(seconds / 86400),
         (unsigned long long)((seconds / 3600) % 24), (unsigned long long)((seconds / 60) % 60),
         (unsigned long long)(seconds % 60));
}

void handle_whoami_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!shell_load()) return;
    glog("hostname: %s\n", s_hostname);
    handle_uuid_cmd(0, NULL);
    handle_version_cmd(0, NULL);
    handle_uptime_cmd(0, NULL);
    handle_ifconfig_cmd(0, NULL);
}

void handle_status_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    glog("GhostESP status\n");
    glog("  uptime: ");
    handle_uptime_cmd(0, NULL);
    glog("  heap: %u bytes free\n", (unsigned)esp_get_free_heap_size());
    glog("  wifi: %s\n", is_wifi_sta_connected() ? "connected" : "disconnected");
    glog("  sd: %s\n", sd_card_manager.is_initialized ? "mounted" : "not mounted");
#ifdef CONFIG_WITH_ETHERNET
    glog("  ethernet: %s\n", ethernet_manager_is_connected() ? "connected" : "disconnected");
#endif
    glog("  portal: %s\n", wifi_manager_is_evil_portal_active() ? "active" : "inactive");
}

void handle_clear_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    const char clear[] = "\033[2J\033[H";
    serial_manager_write_bytes(clear, sizeof(clear) - 1);
    terminal_view_clear_history();
}

void handle_hostname_cmd(int argc, char **argv) {
    if (!shell_load()) { glog("hostname: unavailable\n"); return; }
    if (argc == 1) { glog("%s\n", s_hostname); return; }
    if (argc != 2 || !valid_name(argv[1], sizeof(s_hostname))) {
        glog("Usage: hostname [name]\n");
        return;
    }
    shell_set_hostname(argv[1]);
    glog("hostname: %s\n", argv[1]);
}

static const char *color_code(const char *name) {
    if (strcasecmp(name, "black") == 0) return "0";
    if (strcasecmp(name, "red") == 0) return "196";
    if (strcasecmp(name, "green") == 0) return "46";
    if (strcasecmp(name, "yellow") == 0) return "226";
    if (strcasecmp(name, "blue") == 0) return "27";
    if (strcasecmp(name, "magenta") == 0) return "201";
    if (strcasecmp(name, "cyan") == 0) return "51";
    if (strcasecmp(name, "white") == 0) return "15";
    return NULL;
}

void handle_color_cmd(int argc, char **argv) {
    if (argc == 1) { glog("cli color: %s\n", shell_get_color()); return; }
    if (argc != 2) { glog("Usage: color [off|black|red|green|yellow|blue|magenta|cyan|white|0-255]\n"); return; }
    const char *color = strcmp(argv[1], "off") == 0 ? "off" : color_code(argv[1]);
    if (!color) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);
        if (*argv[1] == '\0' || !end || *end || value < 0 || value > 255) {
            glog("Invalid color. Use a name, 0-255, or off.\n");
            return;
        }
        static char numeric[8];
        snprintf(numeric, sizeof(numeric), "%ld", value);
        color = numeric;
    }
    shell_set_color(color);
    glog("cli color: %s\n", color);
}

void handle_banner_cmd(int argc, char **argv) {
    if (argc == 1 || strcmp(argv[1], "status") == 0) {
        glog("banner: %s\n", shell_get_banner_enabled() ? "on" : "off");
        return;
    }
    if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
        glog("Usage: banner [on|off|status]\n");
        return;
    }
    shell_set_banner_enabled(strcmp(argv[1], "on") == 0);
    glog("banner: %s\n", argv[1]);
}

void handle_alias_cmd(int argc, char **argv) {
    if (!shell_load()) { glog("alias: unavailable\n"); return; }
    if (argc == 1) {
        for (int i = 0; i < CLI_ALIAS_COUNT; ++i) if (s_alias_names[i][0]) glog("alias %s='%s'\n", s_alias_names[i], s_alias_values[i]);
        return;
    }
    char name[CLI_ALIAS_NAME_LEN] = {0};
    char value[CLI_ALIAS_VALUE_LEN] = {0};
    char *equals = strchr(argv[1], '=');
    if (equals) {
        size_t name_len = (size_t)(equals - argv[1]);
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, argv[1], name_len);
        strncpy(value, equals + 1, sizeof(value) - 1);
    } else {
        strncpy(name, argv[1], sizeof(name) - 1);
        for (int i = 2; i < argc && strlen(value) + 2 < sizeof(value); ++i) {
            if (i > 2) strcat(value, " ");
            strncat(value, argv[i], sizeof(value) - strlen(value) - 1);
        }
    }
    if (!valid_name(name, sizeof(name)) || !value[0]) {
        glog("Usage: alias <name> <command>\n");
        return;
    }
    int index = alias_index(name);
    if (index < 0) {
        for (int i = 0; i < CLI_ALIAS_COUNT; ++i) if (!s_alias_names[i][0]) { index = i; break; }
    }
    if (index < 0) { glog("alias: alias table full (%d entries)\n", CLI_ALIAS_COUNT); return; }
    strncpy(s_alias_names[index], name, sizeof(s_alias_names[index]) - 1);
    strncpy(s_alias_values[index], value, sizeof(s_alias_values[index]) - 1);
    if (!alias_save(index)) glog("alias: failed to save\n");
    else glog("alias %s='%s'\n", name, value);
}

void handle_unalias_cmd(int argc, char **argv) {
    if (argc != 2) { glog("Usage: unalias <name|all>\n"); return; }
    if (!shell_load()) { glog("unalias: unavailable\n"); return; }
    if (strcmp(argv[1], "all") == 0) {
        for (int i = 0; i < CLI_ALIAS_COUNT; ++i) alias_remove(i);
        glog("aliases cleared\n");
        return;
    }
    int index = alias_index(argv[1]);
    if (index < 0) { glog("unalias: %s not found\n", argv[1]); return; }
    alias_remove(index);
    glog("unalias %s\n", argv[1]);
}

void handle_history_cmd(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-c") == 0) { command_history_init(); glog("history cleared\n"); return; }
    if (argc > 1) { glog("Usage: history [-c]\n"); return; }
    command_history_print();
}

void handle_didyoumean_cmd(int argc, char **argv) {
    if (argc != 2) { glog("Usage: didyoumean <command>\n"); return; }
    shell_suggest_command(argv[1]);
}

void handle_ps_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    glog("Tasks: %u, free heap: %u bytes\n", (unsigned)uxTaskGetNumberOfTasks(), (unsigned)esp_get_free_heap_size());
#if configUSE_TRACE_FACILITY && configUSE_STATS_FORMATTING_FUNCTIONS
    char tasks[1024] = {0};
    vTaskList(tasks);
    glog("Name            State  Prio Stack\n%s", tasks);
#else
    glog("Task details are disabled in this build.\n");
#endif
}

void handle_df_cmd(int argc, char **argv) {
    (void)argc; (void)argv;
    struct statvfs stats;
    if (statvfs("/mnt", &stats) != 0) { glog("df: /mnt unavailable\n"); return; }
    unsigned long long total = (unsigned long long)stats.f_blocks * stats.f_frsize;
    unsigned long long free_bytes = (unsigned long long)stats.f_bavail * stats.f_frsize;
    glog("Filesystem      Total       Used       Free\n");
    glog("/mnt       %llu KiB  %llu KiB  %llu KiB\n", total / 1024,
         total >= free_bytes ? (total - free_bytes) / 1024 : 0, free_bytes / 1024);
}

static void shell_file_path(const char *path, char *out, size_t out_len) {
    if (path[0] == '/') strncpy(out, path, out_len - 1);
    else snprintf(out, out_len, "/mnt/%s", path);
    out[out_len - 1] = '\0';
}

void handle_tail_cmd(int argc, char **argv) {
    if (argc < 2 || argc > 3) { glog("Usage: tail <file> [lines]\n"); return; }
    int requested = argc == 3 ? atoi(argv[2]) : 10;
    if (requested < 1 || requested > 32) { glog("tail: lines must be 1-32\n"); return; }
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) { glog("tail: SD unavailable\n"); return; }
    char path[256]; shell_file_path(argv[1], path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) { glog("tail: %s: %s\n", path, strerror(errno)); sd_card_jit_end(suspended); return; }
    char lines[32][256]; int count = 0; char line[256];
    while (fgets(line, sizeof(line), file)) {
        strncpy(lines[count % requested], line, sizeof(lines[0]) - 1);
        lines[count % requested][sizeof(lines[0]) - 1] = '\0';
        count++;
    }
    int start = count > requested ? count - requested : 0;
    for (int i = start; i < count; ++i) glog("%s", lines[i % requested]);
    fclose(file); sd_card_jit_end(suspended);
}

void handle_grep_cmd(int argc, char **argv) {
    if (argc != 3) { glog("Usage: grep <pattern> <file>\n"); return; }
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) { glog("grep: SD unavailable\n"); return; }
    char path[256]; shell_file_path(argv[2], path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) { glog("grep: %s: %s\n", path, strerror(errno)); sd_card_jit_end(suspended); return; }
    char line[256];
    while (fgets(line, sizeof(line), file)) if (strstr(line, argv[1])) glog("%s", line);
    fclose(file); sd_card_jit_end(suspended);
}

void handle_source_cmd(int argc, char **argv) {
    if (argc != 2) { glog("Usage: source <file>\n"); return; }
    static int source_depth = 0;
    if (source_depth >= 4) { glog("source: max nesting depth reached\n"); return; }
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) { glog("source: SD unavailable\n"); return; }
    char path[256]; shell_file_path(argv[1], path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) { glog("source: %s: %s\n", path, strerror(errno)); sd_card_jit_end(suspended); return; }
    char line[512];
    source_depth++;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *p = line; while (isspace((unsigned char)*p)) p++;
        if (*p && *p != '#') handle_serial_command(p);
    }
    source_depth--;
    fclose(file); sd_card_jit_end(suspended);
}

void handle_tee_cmd(int argc, char **argv) {
    if (argc < 3) { glog("Usage: tee <file> <text>\n"); return; }
    char path[256]; shell_file_path(argv[1], path, sizeof(path));
    bool suspended = false;
    if (!sd_card_jit_begin(&suspended, false)) { glog("tee: SD unavailable\n"); return; }
    FILE *file = fopen(path, "a");
    if (!file) { glog("tee: %s: %s\n", path, strerror(errno)); sd_card_jit_end(suspended); return; }
    for (int i = 2; i < argc; ++i) fprintf(file, "%s%s", i == 2 ? "" : " ", argv[i]);
    fputc('\n', file); fclose(file); sd_card_jit_end(suspended);
    for (int i = 2; i < argc; ++i) glog("%s%s", i == 2 ? "" : " ", argv[i]);
    glog("\n");
}

static bool env_save(int index) {
    nvs_handle_t nvs;
    if (nvs_open(SHELL_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return false;
    char key[8], value[CLI_ENV_NAME_LEN + CLI_ENV_VALUE_LEN];
    snprintf(key, sizeof(key), "env%d", index);
    snprintf(value, sizeof(value), "%s=%s", s_env_names[index], s_env_values[index]);
    esp_err_t err = nvs_set_str(nvs, key, value);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err == ESP_OK;
}

void handle_env_cmd(int argc, char **argv) {
    (void)argv;
    if (argc != 1) { glog("Usage: env\n"); return; }
    if (!shell_load()) { glog("env: unavailable\n"); return; }
    glog("HOSTNAME=%s\nCLI_COLOR=%s\nCLI_BANNER=%s\n", s_hostname, s_color, s_banner ? "on" : "off");
    for (int i = 0; i < CLI_ENV_COUNT; ++i) if (s_env_names[i][0]) glog("%s=%s\n", s_env_names[i], s_env_values[i]);
}

void handle_export_cmd(int argc, char **argv) {
    if (argc != 2) { glog("Usage: export NAME=value\n"); return; }
    char *equals = strchr(argv[1], '=');
    if (!equals) { glog("export: use NAME=value\n"); return; }
    *equals = '\0';
    if (!valid_name(argv[1], CLI_ENV_NAME_LEN)) { glog("export: invalid variable name\n"); return; }
    if (strcasecmp(argv[1], "HOSTNAME") == 0) { handle_hostname_cmd(2, (char *[]){"hostname", equals + 1}); return; }
    if (strcasecmp(argv[1], "CLI_COLOR") == 0) { handle_color_cmd(2, (char *[]){"color", equals + 1}); return; }
    if (!shell_load()) { glog("export: unavailable\n"); return; }
    int index = -1;
    for (int i = 0; i < CLI_ENV_COUNT; ++i) if (strcasecmp(s_env_names[i], argv[1]) == 0) { index = i; break; }
    if (index < 0) for (int i = 0; i < CLI_ENV_COUNT; ++i) if (!s_env_names[i][0]) { index = i; break; }
    if (index < 0) { glog("export: environment table full\n"); return; }
    strncpy(s_env_names[index], argv[1], sizeof(s_env_names[index]) - 1);
    strncpy(s_env_values[index], equals + 1, sizeof(s_env_values[index]) - 1);
    if (!env_save(index)) glog("export: failed to save\n");
}

static void shell_watch_task(void *argument) {
    char *command = (char *)argument;
    while (!s_watch_stop) {
        handle_serial_command(command);
        for (int i = 0; i < s_watch_interval && !s_watch_stop; ++i) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    free(command);
    s_watch_task = NULL;
    glog("watch stopped\n");
    vTaskDelete(NULL);
}

void handle_watch_cmd(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        if (!shell_stop_watch()) glog("watch: no active watch\n");
        else glog("watch: stopping\n");
        return;
    }
    if (argc < 3) {
        glog("Usage: watch <seconds> <command> | watch stop\n");
        return;
    }
    if (s_watch_task) { glog("watch: already running; use 'watch stop'\n"); return; }
    int seconds = atoi(argv[1]);
    if (seconds < 1 || seconds > 3600) { glog("watch: interval must be 1-3600 seconds\n"); return; }
    char *command = calloc(1, 512);
    if (!command) { glog("watch: out of memory\n"); return; }
    for (int i = 2; i < argc && strlen(command) + strlen(argv[i]) + 2 < 512; ++i) {
        if (i > 2) strcat(command, " ");
        strcat(command, argv[i]);
    }
    s_watch_stop = false;
    s_watch_interval = seconds;
    if (xTaskCreate(shell_watch_task, "CliWatch", 12288, command, 1, &s_watch_task) != pdPASS) {
        free(command);
        s_watch_task = NULL;
        glog("watch: failed to start task\n");
        return;
    }
    glog("watch: running '%s' every %d seconds\n", command, seconds);
}
