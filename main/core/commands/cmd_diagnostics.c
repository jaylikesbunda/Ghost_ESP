// cmd_diagnostics.c
// Stop, DIAL/cast, memory, TP-Link test, and status idle commands.

#include "core/callbacks.h"
#include "core/commands.h"
#include "core/shell.h"
#include "core/commandline.h"
#include "core/esp_comm_manager.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "core/system_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "attacks/ble/ble_spam.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "attacks/wifi/dhcp_starvation.h"
#include "core/dns_server.h"
#include "scans/wifi/name_sniff.h"
#include "managers/aerial_detector_manager.h"
#ifdef CONFIG_HAS_BADBLE
#include "managers/badble_manager.h"
#endif
#include "managers/ble_manager.h"
#include "managers/dial_manager.h"
#include "managers/display_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/gps_manager.h"
#include "managers/infrared_manager.h"
#include "managers/microphone/mic_visualizer.h"
#include "managers/rgb_manager.h"
#include "managers/settings_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "managers/wifi_manager.h"
#include "managers/zigbee_manager.h"
#include "scans/ble/device_detect_scan.h"
#include "scans/wifi/arp_scan.h"
#include "sdkconfig.h"
#include "vendor/GPS/gps_logger.h"
#include "vendor/pcap.h"
#include "lwip/sockets.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "cmd_diagnostics"

#ifndef DISCOVER_TASK_STACK
#if defined(CONFIG_USE_CARDPUTER) || defined(CONFIG_USE_CARDPUTER_ADV)
#define DISCOVER_TASK_STACK 4096
#else
#define DISCOVER_TASK_STACK 6144
#endif
#endif

// Globals owned by main commandline.c, shared with GPS info command.
extern TaskHandle_t gps_info_task_handle;
extern StackType_t *gps_task_stack;
extern StaticTask_t *gps_task_tcb;

static bool g_dial_cast_all = false;

void discover_task(void *pvParameter) {
    DIALClient client;
    DIALManager manager;

    if (dial_client_init(&client) == ESP_OK) {
        dial_manager_init(&manager, &client);
        explore_network(&manager, g_dial_cast_all);
        dial_client_deinit(&client);
    } else {
        glog("Failed to init DIAL client.\n");
        status_display_show_status("DIAL Failed");
    }

    {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
        glog("discover_task min stack free: %u words\n", (unsigned)hwm);
    }
    vTaskDeleteWithCaps(NULL);
}


void wifi_manager_cancel_connect(void);
void wifi_manager_stop_visualizer(void);


void handle_stop_flipper(int argc, char **argv) {
    bool stopped_any = false;

    if (shell_stop_watch()) {
        glog("Stopped CLI watch.\n");
        stopped_any = true;
    }

#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
    rgb_manager_set_mic_stream_suspended(true);
#endif

#ifdef CONFIG_HAS_MIC
    bool restart_mic_visualizer = mic_visualizer_is_running();
    if (restart_mic_visualizer) {
        glog("Stopped mic visualizer.\n");
        stopped_any = true;
    }
    mic_visualizer_stop();
#endif

    if (cmd_ir_stop_universal_send()) {
        glog("Stopped IR universal send.\n");
        stopped_any = true;
    }

#if defined(CONFIG_NFC_ST25R3916) || defined(CONFIG_NFC_PN532)
    if (nfc_cli_stop()) {
        glog("Stopped NFC scanner.\n");
        stopped_any = true;
    }
#endif

    if (wdstream_stop_and_wait("stop")) {
        stopped_any = true;
    }

    stop_wardriving();
    if (!esp_comm_manager_is_remote_command()) {
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("startwd", "-s --helper");
        }
        wardriving_set_peer_assist(false);
    }

    if (wifi_manager_is_channel_switch_attack_running()) {
        glog("Stopped channel switch attack.\n");
        stopped_any = true;
    }
    wifi_manager_stop_deauth();
    if (wifi_manager_stop_handshake_deauth()) {
        glog("Stopped handshake+deauth attack.\n");
        stopped_any = true;
    }
    wifi_manager_stop_channel_switch_attack();
    if (wifi_manager_is_probe_flood_running()) {
        glog("Stopped probe request flood.\n");
        stopped_any = true;
    }
    wifi_manager_stop_probe_flood();
    if (wifi_manager_is_bad_msg_running()) {
        glog("Stopped bad msg attack.\n");
        stopped_any = true;
    }
    wifi_manager_stop_bad_msg();
    if (wifi_manager_is_auth_flood_running()) {
        glog("Stopped auth flood attack.\n");
        stopped_any = true;
    }
    wifi_manager_stop_auth_flood();
    wifi_manager_cancel_connect();

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (ble_spam_is_running()) {
        glog("Stopped BLE spam.\n");
        stopped_any = true;
    }
    ble_spam_stop();
    ble_stop_gatt_scan();
#ifdef CONFIG_HAS_BADBLE
    if (badble_manager_is_running()) {
        glog("Stopped BadBLE keyboard.\n");
        stopped_any = true;
        (void)badble_manager_stop();
    }
#endif
    if (ble_device_detect_is_active() || ble_device_detect_is_tracking()) {
        glog("Stopped BLE device detect.\n");
        stopped_any = true;
    }
    ble_device_detect_stop();
    ble_stop();
    ble_unregister_handler(ble_wardriving_callback);
    ble_set_suspend_allowed(true);
#endif

    if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
        glog("Flushed pending CSV data.\n");
        stopped_any = true;
        csv_flush_buffer_to_file();
    }
    csv_file_close();                  // Close any open CSV files
    gps_manager_deinit(&g_gpsManager); // Clean up GPS if active

    // stop aerial operations
    if (aerial_detector_is_scanning() || aerial_detector_is_emulating()) {
        glog("Stopped aerial detector.\n");
        stopped_any = true;
        if (aerial_detector_is_scanning()) {
            aerial_detector_stop_scan();
        }
        if (aerial_detector_is_emulating()) {
            aerial_detector_stop_emulation();
        }
    }
    aerial_detector_untrack_device();

    // stop flock detector if running
    if (flock_detector_is_running()) {
        glog("Stopped flock detector.\n");
        stopped_any = true;
        flock_detector_stop();
    }

    // also stop any in-progress IR RX (ir rx / ir learn) and dazzler
    if (infrared_manager_dazzler_is_active()) {
        glog("Stopped IR dazzler.\n");
        stopped_any = true;
    }
    infrared_manager_rx_cancel();
    infrared_manager_dazzler_stop();

    // also stop the gps info display task if it is running
    if (gps_info_task_handle != NULL) {
        glog("Stopped GPS info display.\n");
        stopped_any = true;
        vTaskDelete(gps_info_task_handle);
        gps_info_task_handle = NULL;

        // Free the manually allocated stack and TCB
        if (gps_task_stack) {
            heap_caps_free(gps_task_stack);
            gps_task_stack = NULL;
        }
        if (gps_task_tcb) {
            heap_caps_free(gps_task_tcb);
            gps_task_tcb = NULL;
        }
    }

    wifi_manager_stop_monitor_mode();  // Stop any active monitoring

    // Stop network discovery scans
    ssh_scan_cancel();
    netbios_scan_cancel();
    http_banner_scan_cancel();
    snmp_scan_cancel();
    arp_scan_stop_passive();
    port_scan_cancel();
    name_sniff_stop();
    glog("Stopped network scans.\n");
    stopped_any = true;

    if (wifi_manager_stop_deauth_station()) {
        glog("Stopped station deauth.\n");
        stopped_any = true;
    }
    wifi_manager_stop_deauth();
    dhcp_starvation_stop();
    wifi_manager_stop_eapollogoff_attack();
    wifi_manager_stop_sae_flood();
    if (wifi_manager_karma_is_running()) {
        glog("Stopped Karma attack.\n");
        stopped_any = true;
    }
    wifi_manager_stop_karma();            // stop karma attack (sets flag; task self-exits)
    if (wifi_manager_is_evil_portal_active()) {
        glog("Stopped evil portal.\n");
        stopped_any = true;
        wifi_manager_stop_evil_portal();  // stop evil portal and flush credentials
    } else {
        wifi_manager_clear_html_buffer();
    }

    {
        dns_server_handle_t h = dns_handle_take();
        if (h) {
            glog("Stopped DNS sinkhole.\n");
            stopped_any = true;
            stop_dns_server(h);
        }
    }

    wifi_manager_stop_tracking();  // stop ap/sta rssi tracking
    wifi_manager_stop_beacon();  // stop beacon spam

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    // ensure zigbee capture is stopped when using generic stop
    zigbee_manager_stop_capture();
#endif
#ifdef CONFIG_WITH_ETHERNET
    eth_cmd_set_scan_cancel(true);
    if (eth_arp_poison_is_running()) {
        glog("Stopped Ethernet ARP poison.\n");
        stopped_any = true;
        eth_arp_poison_stop();
    }
#endif
    // ensure pcap is properly flushed and closed
    pcap_file_close();

    // reconnect to saved WiFi if credentials exist
    wifi_manager_configure_sta_from_settings();

    // kill any feature tasks we spawned that may still be around
    if (VisualizerHandle != NULL) {
        glog("Stopped WiFi visualizer.\n");
        stopped_any = true;
        wifi_manager_stop_visualizer();
    }
    settings_restart_rgb_effect();

    if (stopped_any) {
        glog("All activities stopped.\n");
        status_display_show_status("All Stopped");
    } else {
        glog("Nothing was running.\n");
        status_display_show_status("Nothing Running");
    }

#ifdef CONFIG_HAS_MIC
    if (restart_mic_visualizer) {
        mic_visualizer_start();
    }
#endif

#ifdef CONFIG_ENABLE_MIC_RGB_VISUALIZER
    rgb_manager_set_mic_stream_suspended(false);
#endif
}

void handle_dial_command(int argc, char **argv) {
    g_dial_cast_all = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "all") == 0 || strcmp(argv[i], "-a") == 0) {
            g_dial_cast_all = true;
        } else {
            dial_manager_set_device_name(argv[i]);
        }
    }
    BaseType_t rc = xTaskCreate_psram(&discover_task, "discover_task", DISCOVER_TASK_STACK, NULL, 5, NULL);
    if (rc != pdPASS) {
        glog("Failed to start DIAL discovery task (err=%ld).\n", (long)rc);
    }
}

static void dump_task_stacks(void) {
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY)
    UBaseType_t num = uxTaskGetNumberOfTasks();
    TaskStatus_t *list = (TaskStatus_t *)pvPortMalloc(num * sizeof(TaskStatus_t));
    if (!list) return;
    UBaseType_t out = uxTaskGetSystemState(list, num, NULL);
    for (UBaseType_t i = 0; i < out; i++) {
        printf("task=%s state=%u prio=%u min_free_stack=%u bytes\n",
               list[i].pcTaskName,
               (unsigned)list[i].eCurrentState,
               (unsigned)list[i].uxCurrentPriority,
               (unsigned)list[i].usStackHighWaterMark);
    }
    vPortFree(list);
#else
    glog("task stack snapshot unavailable: enable CONFIG_FREERTOS_USE_TRACE_FACILITY in sdkconfig\n");
#endif
}

static void print_mem_usage(void) {
    glog("usage: mem [heaps|tasks|regions|check|dump|trace]\n");
    glog("  mem              heap summary + task stack high-water marks\n");
    glog("  mem heaps        heap summary only\n");
    glog("  mem tasks        task stack high-water marks only\n");
    glog("  mem regions      ESP-IDF heap region breakdown\n");
    glog("  mem check        heap integrity check\n");
    glog("  mem dump         verbose internal/PSRAM heap block dump\n");
    glog("  mem trace <start|leaks|all|stop|dump>\n");
}

void handle_mem_cmd(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "?") == 0)) {
        print_mem_usage();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "heaps") == 0) {
        memory_debug_print_heap_summary();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "tasks") == 0) {
        dump_task_stacks();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "regions") == 0) {
        memory_debug_print_heap_regions();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "check") == 0) {
        bool ok = memory_debug_check_heap_integrity();
        glog("heap integrity: %s\n", ok ? "ok" : "FAILED");
        return;
    }

    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        memory_debug_print_heap_summary();
        ESP_LOGI(TAG, "heap internal dump");
        heap_caps_dump(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) > 0) {
            ESP_LOGI(TAG, "heap psram dump");
            heap_caps_dump(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        return;
    }

    if (argc > 1 && strcmp(argv[1], "trace") == 0) {
        if (argc > 2 && (strcmp(argv[2], "start") == 0 || strcmp(argv[2], "leaks") == 0)) {
            esp_err_t e = memory_debug_trace_start(true);
            glog("heap leak trace start: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
            return;
        }
        if (argc > 2 && strcmp(argv[2], "all") == 0) {
            esp_err_t e = memory_debug_trace_start(false);
            glog("heap all trace start: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
            return;
        }
        if (argc > 2 && strcmp(argv[2], "stop") == 0) {
            esp_err_t e = memory_debug_trace_stop();
            glog("heap trace stop: %s\n", e == ESP_OK ? "ok" : esp_err_to_name(e));
            return;
        }
        if (argc > 2 && strcmp(argv[2], "dump") == 0) {
            memory_debug_trace_dump();
            return;
        }
        glog("usage: mem trace <start|leaks|all|stop|dump>\n");
        return;
    }

    if (argc > 1) {
        print_mem_usage();
        return;
    }

    memory_debug_print_heap_summary();
    dump_task_stacks();
}


bool ip_str_to_bytes(const char *ip_str, uint8_t *ip_bytes) {
    int ip[4];
    if (sscanf(ip_str, "%d.%d.%d.%d", &ip[0], &ip[1], &ip[2], &ip[3]) == 4) {
        for (int i = 0; i < 4; i++) {
            if (ip[i] < 0 || ip[i] > 255)
                return false;
            ip_bytes[i] = (uint8_t)ip[i];
        }
        return true;
    }
    return false;
}

bool mac_str_to_bytes(const char *mac_str, uint8_t *mac_bytes) {
    int mac[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4],
               &mac[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            if (mac[i] < 0 || mac[i] > 255)
                return false;
            mac_bytes[i] = (uint8_t)mac[i];
        }
        return true;
    }
    return false;
}

void encrypt_tp_link_command(const char *input, uint8_t *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = output[i];
    }
}

void decrypt_tp_link_response(const uint8_t *input, char *output, size_t len) {
    uint8_t key = 171;
    for (size_t i = 0; i < len; i++) {
        output[i] = input[i] ^ key;
        key = input[i];
    }
}

void handle_tp_link_test(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: tp_link_test <on|off|loop>\n");
        status_display_show_status("TP Link Usage");
        return;
    }

    bool isloop = false;

    if (strcmp(argv[1], "loop") == 0) {
        isloop = true;
    } else if (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0) {
        glog("Invalid argument. Use 'on', 'off', or 'loop'.\n");
        status_display_show_status("TP Arg Invalid");
        return;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_addr.s_addr = inet_addr("255.255.255.255");
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(9999);

    int iterations = isloop ? 10 : 1;

    for (int i = 0; i < iterations; i++) {
        const char *command;
        if (isloop) {
            command = (i % 2 == 0) ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}" : // "on"
                          "{\"system\":{\"set_relay_state\":{\"state\":0}}}";             // "off"
        } else {

            command = (strcmp(argv[1], "on") == 0)
                          ? "{\"system\":{\"set_relay_state\":{\"state\":1}}}"
                          : "{\"system\":{\"set_relay_state\":{\"state\":0}}}";
        }

        uint8_t encrypted_command[128];
        memset(encrypted_command, 0, sizeof(encrypted_command));

        size_t command_len = strlen(command);
        if (command_len >= sizeof(encrypted_command)) {
            glog("Command too large to encrypt\n");
            status_display_show_status("TP Cmd Too Big");
            return;
        }

        encrypt_tp_link_command(command, encrypted_command, command_len);

        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            glog("Failed to create socket: errno %d\n", errno);
            status_display_show_status("TP Sock Error");
            return;
        }

        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        int err = sendto(sock, encrypted_command, command_len, 0, (struct sockaddr *)&dest_addr,
                         sizeof(dest_addr));
        if (err < 0) {
            glog("Error occurred during sending: errno %d\n", errno);
            close(sock);
            status_display_show_status("TP Send Error");
            return;
        }

        glog("Broadcast message sent: %s\n", command);
        status_display_show_status("TP Packet Sent");

        struct timeval timeout = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        uint8_t recv_buf[128];
        socklen_t addr_len = sizeof(dest_addr);
        int len = recvfrom(sock, recv_buf, sizeof(recv_buf) - 1, 0, (struct sockaddr *)&dest_addr,
                           &addr_len);
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                glog("No response from any device\n");
                status_display_show_status("No TP Reply");
            } else {
                glog("Error receiving response: errno %d\n", errno);
                status_display_show_status("TP Recv Error");
            }
        } else {
            recv_buf[len] = 0;
            char decrypted_response[128];
            decrypt_tp_link_response(recv_buf, decrypted_response, len);
            decrypted_response[len] = 0;
            glog("Response: %s\n", decrypted_response);
            status_display_show_status("TP Reply Recv");
        }

        close(sock);

        if (isloop && i < 9) {
            vTaskDelay(pdMS_TO_TICKS(700));
        }
    }
}


#ifdef CONFIG_WITH_STATUS_DISPLAY
static const char *idle_anim_to_name(IdleAnimation anim) {
    switch (anim) {
        case IDLE_ANIM_GAME_OF_LIFE: return "life";
        case IDLE_ANIM_GHOST: return "ghost";
        case IDLE_ANIM_STARFIELD: return "starfield";
        case IDLE_ANIM_HUD: return "hud";
        case IDLE_ANIM_MATRIX: return "matrix";
        case IDLE_ANIM_FLYING_GHOSTS: return "ghosts";
        case IDLE_ANIM_SPIRAL: return "spiral";
        case IDLE_ANIM_FALLING_LEAVES: return "leaves";
        case IDLE_ANIM_BOUNCING_TEXT: return "bouncing";
        default: return "unknown";
    }
}

static bool parse_idle_anim_arg(const char *arg, IdleAnimation *out) {
    if (!arg || !out) return false;
    if (strcmp(arg, "0") == 0 || strcmp(arg, "life") == 0 || strcmp(arg, "gameoflife") == 0) {
        *out = IDLE_ANIM_GAME_OF_LIFE;
        return true;
    }
    if (strcmp(arg, "1") == 0 || strcmp(arg, "ghost") == 0) {
        *out = IDLE_ANIM_GHOST;
        return true;
    }
    if (strcmp(arg, "2") == 0 || strcmp(arg, "starfield") == 0 || strcmp(arg, "startfield") == 0) {
        *out = IDLE_ANIM_STARFIELD;
        return true;
    }
    if (strcmp(arg, "3") == 0 || strcmp(arg, "hud") == 0 || strcmp(arg, "stats") == 0) {
        *out = IDLE_ANIM_HUD;
        return true;
    }
    if (strcmp(arg, "4") == 0 || strcmp(arg, "matrix") == 0 || strcmp(arg, "rain") == 0 || strcmp(arg, "code") == 0) {
        *out = IDLE_ANIM_MATRIX;
        return true;
    }
    if (strcmp(arg, "5") == 0 || strcmp(arg, "ghosts") == 0 || strcmp(arg, "flyingghosts") == 0 || strcmp(arg, "flying_ghosts") == 0 || strcmp(arg, "ghoster") == 0 || strcmp(arg, "flyingghoster") == 0 || strcmp(arg, "flying_ghoster") == 0 || strcmp(arg, "toaster") == 0 || strcmp(arg, "flyingtoaster") == 0 || strcmp(arg, "flying_toaster") == 0) {
        *out = IDLE_ANIM_FLYING_GHOSTS;
        return true;
    }
    if (strcmp(arg, "6") == 0 || strcmp(arg, "spiral") == 0 || strcmp(arg, "hypnotic") == 0 || strcmp(arg, "hypnoticspiral") == 0 || strcmp(arg, "hypnotic_spiral") == 0) {
        *out = IDLE_ANIM_SPIRAL;
        return true;
    }
    if (strcmp(arg, "7") == 0 || strcmp(arg, "leaves") == 0 || strcmp(arg, "fallingleaves") == 0 || strcmp(arg, "falling_leaves") == 0) {
        *out = IDLE_ANIM_FALLING_LEAVES;
        return true;
    }
    if (strcmp(arg, "8") == 0 || strcmp(arg, "bouncing") == 0 || strcmp(arg, "bouncingtext") == 0 || strcmp(arg, "bouncing_text") == 0 || strcmp(arg, "dvd") == 0 || strcmp(arg, "dvdplayer") == 0) {
        *out = IDLE_ANIM_BOUNCING_TEXT;
        return true;
    }
    return false;
}

void handle_status_idle_cmd(int argc, char **argv) {
    if (!status_display_is_ready()) {
        glog("Status display not ready; check wiring and CONFIG_WITH_STATUS_DISPLAY.\n");
        return;
    }

    if (argc < 2) {
        IdleAnimation current = settings_get_status_idle_animation(&G_Settings);
        uint32_t timeout = settings_get_status_idle_timeout_ms(&G_Settings);
        const char *name = idle_anim_to_name(current);
        const char *timeout_desc = (timeout == 0 || timeout == UINT32_MAX) ? "never" : "delayed";
        glog("Current idle animation: %s (%d)\n", name, (int)current);
        glog("Idle timeout: %lu ms (%s)\n", (unsigned long)timeout, timeout_desc);
        status_display_show_status("Idle Anim Info");
        return;
    }

    if (strcmp(argv[1], "list") == 0) {
        glog("Available idle animations:\n");
        glog("  0 - life      (Game of Life)\n");
        glog("  1 - ghost      (Ghost sprite)\n");
        glog("  2 - starfield  (Starfield effect)\n");
        glog("  3 - hud        (System HUD)\n");
        glog("  4 - matrix     (Matrix code rain)\n");
        glog("  5 - ghosts     (Flying Ghosts)\n");
        glog("  6 - spiral    (Hypnotic Spiral)\n");
        glog("  7 - leaves    (Falling Leaves)\n");
        glog("  8 - bouncing  (Bouncing Text)\n");
        status_display_show_status("Idle Anim List");
        return;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            glog("Usage: statusidle set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>\n");
            return;
        }
        IdleAnimation anim;
        if (!parse_idle_anim_arg(argv[2], &anim)) {
            glog("Unknown idle animation: %s\n", argv[2]);
            glog("Use 'statusidle list' to see options.\n");
            return;
        }
        settings_set_status_idle_animation(&G_Settings, anim);
        settings_save(&G_Settings);
        glog("Idle animation set to %s (%d)\n", idle_anim_to_name(anim), (int)anim);
        status_display_show_status("Idle Anim Set");
        return;
    }

    glog("Usage: statusidle [list|set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>]\n");
}

#endif

void handle_unknown_command(const char *cmd) {
    glog("Unsupported command: %s\n", cmd);
    shell_suggest_command(cmd);
}
