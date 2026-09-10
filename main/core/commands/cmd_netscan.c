// cmd_netscan.c
// Network reconnaissance commands: port, ARP, SSH, NetBIOS, HTTP banner, SNMP,
// congestion, and probe listening.

#include "core/commands.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "scans/wifi/port_scan.h"
#include "scans/wifi/ssh_scan.h"
#include "scans/wifi/netbios_scan.h"
#include "scans/wifi/http_banner_scan.h"
#include "scans/wifi/snmp_scan.h"
#include "scans/wifi/enum4linux_scan.h"
#include "scans/wifi/arp_scan.h"
#include "scans/wifi/name_sniff.h"
#include "vendor/pcap.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/network_constants.h"

static bool normalize_subnet_prefix_arg(const char *arg, char *out, size_t out_len) {
    if (arg == NULL || out == NULL || out_len < 16) {
        return false;
    }

    char tmp[32];
    strlcpy(tmp, arg, sizeof(tmp));
    char *slash = strchr(tmp, '/');
    if (slash != NULL) {
        *slash = '\0';
    }

    unsigned int a = 0, b = 0, c = 0, d = 0;
    int consumed = 0;
    if (sscanf(tmp, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) == 4 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    consumed = 0;
    if (sscanf(tmp, "%u.%u.%u.%n", &a, &b, &c, &consumed) == 3 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    consumed = 0;
    if (sscanf(tmp, "%u.%u.%u%n", &a, &b, &c, &consumed) == 3 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    return false;
}

void handle_scan_ports(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage:\n");
        glog("  scanports local\n");
        glog("  scanports <IP> [all | start-end]\n");
        status_display_show_status("Ports Usage");
        return;
    }

    // Handle local subnet scan
    if (strcmp(argv[1], "local") == 0) {
        if (argc > 2) {
            glog("Info: 'local' scan does not take arguments.\n");
            status_display_show_status("Ports Local");
        }
        glog("Starting local subnet scan...\n");
        port_scan_subnet_async();  // Use async version to keep CLI responsive
        status_display_show_status("Ports Local");
        return;
    }

    // Handle remote IP scan
    const char *target_ip = argv[1];
    int start_port = 0, end_port = 0;

    // Default to common ports if no range is specified
    if (argc < 3) {
        host_result_t result;
        glog("Scanning common tcp ports on %s...\n", target_ip);
        scan_ports_on_host(target_ip, &result);

        if (result.num_open_ports > 0) {
            glog("Found %d open ports on %s:\n", result.num_open_ports, target_ip);
            for (int i = 0; i < result.num_open_ports; i++) {
                glog("  Port %d\n", result.open_ports[i]);
            }
        } else {
            glog("No common open ports found.\n");
        }

        host_result_t udp_result;
        glog("Scanning common udp ports on %s...\n", target_ip);
        scan_udp_ports_on_host(target_ip, &udp_result);
        if (udp_result.num_open_ports > 0) {
            glog("Found %d udp ports responding on %s:\n", udp_result.num_open_ports, target_ip);
            for (int i = 0; i < udp_result.num_open_ports; i++) {
                glog("  UDP %d\n", udp_result.open_ports[i]);
            }
        } else {
            glog("No common udp responses found.\n");
        }
        status_display_show_status("Ports Common");
        return;
    }

    // Parse port range argument
    const char *port_arg = argv[2];
    if (strcmp(port_arg, "all") == 0) {
        start_port = 1;
        end_port = 65535;
    } else if (sscanf(port_arg, "%d-%d", &start_port, &end_port) != 2 || start_port < 1 ||
               end_port > 65535 || start_port > end_port) {
        glog("Error: Invalid port range. Use 'all' or 'start-end'.\n");
        status_display_show_status("Range Invalid");
        return;
    }

    glog("Scanning %s tcp ports %d-%d...\n", target_ip, start_port, end_port);
    scan_ip_port_range(target_ip, start_port, end_port);

    glog("Scanning %s udp ports %d-%d...\n", target_ip, start_port, end_port);
    scan_ip_udp_port_range(target_ip, start_port, end_port);
    status_display_show_status("Ports Custom");
}

void handle_scan_arp(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "monitor") == 0 ||
                      strcmp(argv[1], "passive") == 0)) {
        int duration = 0; // 0 = run until stopped
        if (argc >= 3) {
            duration = atoi(argv[2]);
        }
        arp_scan_start_passive(duration);
        status_display_show_status("Packet Monitor");
        return;
    }

    glog("Starting ARP scan on local network...\n");
    wifi_manager_arp_scan_subnet();
    status_display_show_status("ARP Scan");
}

void handle_scan_ssh(int argc, char **argv) {
    if (argc < 2) {
        glog("Starting SSH scan on local subnet...\n");
        ssh_scan_subnet();
        status_display_show_status("SSH Scan Done");
        return;
    }

    const char *target_ip = argv[1];
    glog("Starting SSH scan on %s...\n", target_ip);
    ssh_scan_host(target_ip);
    status_display_show_status("SSH Scan Done");
}

void handle_netbios_scan(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "subnet") == 0) {
        char subnet_prefix[16];
        if (!normalize_subnet_prefix_arg(argv[2], subnet_prefix, sizeof(subnet_prefix))) {
            glog("Usage: netbiosscan subnet <a.b.c[.0|.]>\n");
            return;
        }
        glog("Starting NetBIOS scan on subnet %s*...\n", subnet_prefix);
        netbios_scan_subnet_prefix(subnet_prefix);
        status_display_show_status("NetBIOS Done");
        return;
    }

    if (argc < 2 || strcmp(argv[1], "subnet") == 0) {
        glog("Starting NetBIOS scan on local subnet...\n");
        netbios_scan_subnet();
        status_display_show_status("NetBIOS Done");
        return;
    }

    glog("Starting NetBIOS scan on %s...\n", argv[1]);
    netbios_scan_host(argv[1]);
    status_display_show_status("NetBIOS Done");
}

void handle_http_banner_scan(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "subnet") == 0) {
        char subnet_prefix[16];
        if (!normalize_subnet_prefix_arg(argv[2], subnet_prefix, sizeof(subnet_prefix))) {
            glog("Usage: httpbannerscan subnet <a.b.c[.0|.]>\n");
            return;
        }
        glog("Starting HTTP banner scan on subnet %s*...\n", subnet_prefix);
        http_banner_scan_subnet_prefix(subnet_prefix);
        status_display_show_status("HTTP Banner Done");
        return;
    }

    if (argc < 2 || strcmp(argv[1], "subnet") == 0) {
        glog("Starting HTTP banner scan on local subnet...\n");
        http_banner_scan_subnet();
        status_display_show_status("HTTP Banner Done");
        return;
    }

    glog("Starting HTTP banner scan on %s...\n", argv[1]);
    http_banner_scan_host(argv[1]);
    status_display_show_status("HTTP Banner Done");
}

void handle_snmp_probe(int argc, char **argv) {
    // snmpprobe communities <c1,c2,...|file>
    if (argc >= 3 && strcmp(argv[1], "communities") == 0) {
        if (snmp_scan_load_communities_file(argv[2])) {
            glog("Loaded SNMP communities from %s\n", argv[2]);
        } else {
            snmp_scan_set_communities(argv[2]);
        }
        return;
    }

    // SNMP Walk mode: snmpprobe walk [host|subnet] [OID]
    if (argc >= 2 && strcmp(argv[1], "walk") == 0) {
        if (argc >= 4 && strcmp(argv[2], "subnet") == 0) {
            char subnet_prefix[16];
            if (!normalize_subnet_prefix_arg(argv[3], subnet_prefix, sizeof(subnet_prefix))) {
                glog("Usage: snmpprobe walk subnet <a.b.c[.0|.]> [OID]\n");
                return;
            }
            const char *oid = (argc >= 5) ? argv[4] : NULL;
            glog("Starting SNMP walk on subnet %s*...\n", subnet_prefix);
            snmp_walk_subnet_prefix(subnet_prefix, oid);
            status_display_show_status("SNMP Walk Done");
            return;
        }

        if (argc >= 3 && strcmp(argv[2], "subnet") != 0) {
            // Walk specific host
            const char *oid = (argc >= 4) ? argv[3] : NULL;
            glog("Starting SNMP walk on %s...\n", argv[2]);
            snmp_walk_host(argv[2], oid);
            status_display_show_status("SNMP Walk Done");
            return;
        }

        // Walk local subnet
        const char *oid = (argc >= 3) ? argv[2] : NULL;
        glog("Starting SNMP walk on local subnet...\n");
        snmp_walk_subnet(oid);
        status_display_show_status("SNMP Walk Done");
        return;
    }

    if (argc >= 3 && strcmp(argv[1], "subnet") == 0) {
        char subnet_prefix[16];
        if (!normalize_subnet_prefix_arg(argv[2], subnet_prefix, sizeof(subnet_prefix))) {
            glog("Usage: snmpprobe subnet <a.b.c[.0|.]>\n");
            return;
        }
        glog("Starting SNMP probe on subnet %s*...\n", subnet_prefix);
        snmp_scan_subnet_prefix(subnet_prefix);
        status_display_show_status("SNMP Done");
        return;
    }

    if (argc < 2 || strcmp(argv[1], "subnet") == 0) {
        glog("Starting SNMP probe on local subnet...\n");
        snmp_scan_subnet();
        status_display_show_status("SNMP Done");
        return;
    }

    glog("Starting SNMP probe on %s...\n", argv[1]);
    snmp_scan_host(argv[1]);
    status_display_show_status("SNMP Done");
}

void handle_enum_scan(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "subnet") == 0) {
        char subnet_prefix[16];
        if (!normalize_subnet_prefix_arg(argv[2], subnet_prefix, sizeof(subnet_prefix))) {
            glog("Usage: enumscan subnet <a.b.c[.0|.]>\n");
            return;
        }
        glog("Starting Enum scan on subnet %s*...\n", subnet_prefix);
        enum_scan_subnet_prefix(subnet_prefix);
        status_display_show_status("Enum Done");
        return;
    }

    if (argc < 2 || strcmp(argv[1], "subnet") == 0) {
        glog("Starting Enum scan on local subnet...\n");
        enum_scan_subnet();
        status_display_show_status("Enum Done");
        return;
    }

    glog("Starting Enum scan on %s...\n", argv[1]);
    enum_scan_host(argv[1]);
    status_display_show_status("Enum Done");
}

void handle_mdns_sniff(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "stop") == 0 || strcmp(argv[1], "-s") == 0 ||
                      strcmp(argv[1], "-stop") == 0)) {
        if (!name_sniff_is_running()) {
            glog("Name sniff is not running.\n");
            return;
        }
        name_sniff_stop();
        status_display_show_status("Names Stop");
        return;
    }

    if (argc < 2) {
        glog("Usage:\n");
        glog("  mdnssniff <IP|all>\n");
        glog("  mdnssniff stop\n");
        if (name_sniff_is_running()) {
            const char *f = name_sniff_get_filter();
            glog("Sniffing now: %s\n", f[0] ? f : "all");
        }
        status_display_show_status("Names Usage");
        return;
    }

    const char *target = argv[1];
    if (strcmp(target, "all") != 0) {
        unsigned int a = 0, b = 0, c = 0, d = 0;
        char extra = '\0';
        if (sscanf(target, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
            a > 255 || b > 255 || c > 255 || d > 255) {
            glog("Error: Invalid IP. Use <IP|all>.\n");
            status_display_show_status("Names Bad IP");
            return;
        }
    }

    if (name_sniff_start(target) != ESP_OK) {
        glog("Error: Could not start name sniff.\n");
        status_display_show_status("Names Failed");
        return;
    }
    status_display_show_status("Names Listen");
}

void handle_congestion_cmd(int argc, char **argv) {
    wifi_manager_start_scan();
    status_display_show_status("Congest Scan");

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_manager_get_scan_results_data(&ap_count, &ap_records);

    if (ap_count == 0 || ap_records == NULL) {
        glog("No APs found during scan.\n");
        status_display_show_status("No AP Found");
        return;
    }

    int unique_count = 0;
    int *channels = spiram_malloc((size_t)ap_count * sizeof(int));
    int *counts = spiram_malloc((size_t)ap_count * sizeof(int));
    if (!channels || !counts) {
        free(channels);
        free(counts);
        glog("Error: Failed to allocate memory for channel counts.\n");
        status_display_show_status("Congest OOM");
        return;
    }
    int max_count = 0;
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_records[i].primary;
        if (ch <= 0) continue;
        int idx = -1;
        for (int j = 0; j < unique_count; j++) {
            if (channels[j] == ch) { idx = j; break; }
        }
        if (idx >= 0) {
            counts[idx]++;
        } else {
            channels[unique_count] = ch;
            counts[unique_count] = 1;
            idx = unique_count++;
        }
        if (counts[idx] > max_count) {
            max_count = counts[idx];
        }
    }
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = i + 1; j < unique_count; j++) {
            if (channels[i] > channels[j]) {
                int tmp_ch = channels[i]; channels[i] = channels[j]; channels[j] = tmp_ch;
                int tmp_cnt = counts[i]; counts[i] = counts[j]; counts[j] = tmp_cnt;
            }
        }
    }

    glog("\nChannel Congestion:\n\n");
    const char* header = "+----+-------+------------+\n";
    const char* separator = "+----+-------+------------+\n";
    const char* row_format = "| %2d | %5d | %s |\n";
    const char* footer = "+----+-------+------------+\n";

    glog("%s", header);
    glog("| CH | Count | Bar        |\n");
    glog("%s", separator);

    const int max_bar_length = 10;
    char display_bar[max_bar_length * 4]; // Generous buffer: 3 bytes/block + 1 space/pad + null

    for (int i = 0; i < unique_count; i++) {
        int ch = channels[i];
        int cnt = counts[i];
        int bar_length = 0;
        if (max_count > 0) {
            bar_length = (int)(((float)cnt / max_count) * max_bar_length);
            if (bar_length == 0 && cnt > 0) bar_length = 1;
        }
        char *ptr = display_bar;
        for (int j = 0; j < bar_length; ++j) {
            *ptr++ = '#';
        }
        int spaces_needed = max_bar_length - bar_length;
        for (int j = 0; j < spaces_needed; ++j) {
            *ptr++ = ' ';
        }
        *ptr = '\0';
        glog(row_format, ch, cnt, display_bar);
    }
    free(channels);
    free(counts);
    glog("%s", footer);
}

void handle_listen_probes_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        g_listen_probes_save_to_sd = false;
        glog("Probe request listening stopped.\n");
        status_display_show_status("Probes Stop");
        return;
    }

    uint8_t channel = 0;
    bool channel_hopping = true;

    if (argc > 1) {
        char *endptr;
        long ch = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && ch >= 1 && ch <= MAX_WIFI_CHANNEL) {
            channel = (uint8_t)ch;
            channel_hopping = false;
            glog("Starting to listen for probe requests on channel %d...\n", channel);
            char status_msg[18];
            snprintf(status_msg, sizeof(status_msg), "Probes Ch %02d", channel);
            status_display_show_status(status_msg);
        } else {
            glog("Invalid channel: %s. Valid range: 1-%d\n", argv[1], MAX_WIFI_CHANNEL);
            status_display_show_status("Channel Bad");
            return;
        }
    } else {
        glog("Starting to listen for probe requests (channel hopping)...\n");
        status_display_show_status("Probes Hop");
    }

    bool sd_available = sd_card_exists("/mnt/ghostesp/pcaps");
    g_listen_probes_save_to_sd = sd_available;
    if (sd_available) {
        int err = pcap_file_open("probelisten", PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            glog("Warning: PCAP file open failed; probes will not be saved to SD card.\n");
            g_listen_probes_save_to_sd = false;
            status_display_show_status("PCAP Warn");
        }
    } else {
        glog("SD card not available; probe PCAP disabled.\n");
        status_display_show_status("SD Missing");
    }

    if (channel_hopping) {
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    } else {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    }
}
