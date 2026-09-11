// cmd_subghz.c
// SubGHz command handler.

#include "core/commands.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "esp_wifi.h"
#include "managers/status_display_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/wifi_manager.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MIC
#include "managers/microphone/mic_driver.h"
#endif

#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE))
#include "managers/views/subghz_view.h"
#endif

void handle_subghz_cmd(int argc, char **argv) {
    if (argc < 2) {
        glog("Usage: subghz <start|waterfall_start|stop|waterfall_stop|pause|resume|status|capture|capture_on|capture_off|capture_begin|cycle_freq|save|load|list|replay|state|probe|rxmode|writereg|readreg|wifioff|wifion>\n");
        return;
    }

    const char *sub = argv[1];
    bool remote_request = esp_comm_manager_is_remote_command();

#if defined(CONFIG_WITH_SCREEN) && (defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE))
    if (strcmp(sub, "state") == 0) {
        if (argc >= 3) {
            char joined[64] = {0};
            for (int i = 2; i < argc && i < 6; i++) {
                if (i > 2) strlcat(joined, " ", sizeof(joined));
                strlcat(joined, argv[i], sizeof(joined));
            }
            subghz_view_update_remote_state(joined);
        }
        return;
    }
#endif

#ifdef CONFIG_HAS_SUBGHZ
    bool stream_to_peer = remote_request && esp_comm_manager_is_connected();

    if (strcmp(sub, "start") == 0 || strcmp(sub, "waterfall_start") == 0) {
        bool waterfall_mode = (strcmp(sub, "waterfall_start") == 0);
        bool ok = waterfall_mode ? subghz_remote_manager_start_waterfall(stream_to_peer) : subghz_remote_manager_start(stream_to_peer);
#ifdef CONFIG_HAS_MIC
        if (ok && waterfall_mode) {
            mic_pause();
        }
#endif
        if (ok) {
            glog(waterfall_mode ? "SubGHz waterfall scanner started\n" : "SubGHz scanner started\n");
            glog("SubGHz cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d GDO0=%d GDO2=%d\n",
                 CONFIG_SUBGHZ_SPI_HOST,
                 CONFIG_SUBGHZ_SPI_MOSI_PIN,
                 CONFIG_SUBGHZ_SPI_MISO_PIN,
                 CONFIG_SUBGHZ_SPI_SCK_PIN,
                 CONFIG_SUBGHZ_CSN_PIN,
                 CONFIG_SUBGHZ_GDO0_PIN,
                 CONFIG_SUBGHZ_GDO2_PIN);
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", waterfall_mode ? "state waterfall_started" : "state started");
            }
        } else {
            glog("SubGHz scanner failed to start: %s\n", subghz_remote_manager_get_last_error());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state error");
            }
        }
        return;
    }

    if (strcmp(sub, "stop") == 0 || strcmp(sub, "waterfall_stop") == 0) {
        if (!subghz_remote_manager_is_running()) {
#ifdef CONFIG_HAS_MIC
            mic_resume();
#endif
            glog("SubGHz scanner already stopped\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state stopped");
            }
            return;
        }
        subghz_remote_manager_stop();
#ifdef CONFIG_HAS_MIC
        mic_resume();
#endif
        glog("SubGHz scanner stopping\n");
        return;
    }

    if (strcmp(sub, "pause") == 0) {
        if (!subghz_remote_manager_is_running()) {
            glog("SubGHz scanner is not running\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state error");
            }
            return;
        }
        subghz_remote_manager_set_paused(true);
        glog("SubGHz scanner paused\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state paused");
        }
        return;
    }

    if (strcmp(sub, "resume") == 0) {
        if (!subghz_remote_manager_is_running()) {
            glog("SubGHz scanner is not running\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state error");
            }
            return;
        }
        subghz_remote_manager_set_paused(false);
        glog("SubGHz scanner resumed\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state resumed");
        }
        return;
    }

    if (strcmp(sub, "probe") == 0) {
        glog("SubGHz diag probe running -- watch the serial log. Hold a known remote when Phase D starts.\n");
        subghz_remote_manager_diag_probe();
        return;
    }

    if (strcmp(sub, "writereg") == 0) {
        if (argc < 4) {
            glog("Usage: subghz writereg <reg> <val>  (hex ok, e.g. subghz writereg 0x1D 0xB2)\n");
            return;
        }
        uint32_t reg = strtoul(argv[2], NULL, 0);
        uint32_t val = strtoul(argv[3], NULL, 0);
        if (reg > 0x2E || val > 0xFF) {
            glog("SubGHz writereg: reg must be <= 0x2E, val <= 0xFF\n");
            return;
        }
        uint8_t rb = 0;
        if (!subghz_remote_manager_debug_reg_write((uint8_t)reg, (uint8_t)val, &rb)) {
            glog("SubGHz writereg failed: %s\n", subghz_remote_manager_get_last_error());
            return;
        }
        glog("SubGHz reg 0x%02lX <- 0x%02lX (readback 0x%02X)\n",
             (unsigned long)reg, (unsigned long)val, rb);
        return;
    }

    if (strcmp(sub, "readreg") == 0) {
        if (argc < 3) {
            glog("Usage: subghz readreg <reg>  (hex ok)\n");
            return;
        }
        uint32_t reg = strtoul(argv[2], NULL, 0);
        if (reg > 0x2E) {
            glog("SubGHz readreg: reg must be <= 0x2E\n");
            return;
        }
        uint8_t val = 0;
        if (!subghz_remote_manager_debug_reg_read((uint8_t)reg, &val)) {
            glog("SubGHz readreg failed: %s\n", subghz_remote_manager_get_last_error());
            return;
        }
        glog("SubGHz reg 0x%02lX = 0x%02X\n", (unsigned long)reg, val);
        return;
    }

    /* RF-isolation experiment: the T-Embed CC1101 receiver catches only ~2%
     * of a KeeLoq transmission with the S3 WiFi modem connected (2.4GHz PA
     * activity next to a 433MHz LNA). Known-good receiver firmware for this
     * board runs its RF modes WiFi-free. Stop the modem entirely and
     * compare RMT burst dumps. */
    if (strcmp(sub, "wifioff") == 0) {
        wifi_manager_set_manual_disconnect(true);
        (void)esp_wifi_disconnect();
        esp_err_t err = esp_wifi_stop();
        glog("WiFi modem stopped for SubGHz RX isolation (%s)\n", esp_err_to_name(err));
        return;
    }

    if (strcmp(sub, "wifion") == 0) {
        esp_err_t err = esp_wifi_start();
        glog("WiFi modem started (%s) -- reconnect via UI or reboot\n", esp_err_to_name(err));
        return;
    }

    if (strcmp(sub, "rxmode") == 0) {
        if (argc >= 3) {
            if (strcmp(argv[2], "rmt") == 0) {
                subghz_remote_manager_set_tembed_rx_use_rmt(true);
            } else if (strcmp(argv[2], "isr") == 0) {
                subghz_remote_manager_set_tembed_rx_use_rmt(false);
            } else {
                glog("Usage: subghz rxmode <isr|rmt>\n");
                return;
            }
            glog("SubGHz RX mode set to %s (applies on next capture arm)\n",
                 subghz_remote_manager_get_tembed_rx_use_rmt() ? "rmt" : "isr");
        } else {
            glog("SubGHz RX mode: %s\n",
                 subghz_remote_manager_get_tembed_rx_use_rmt() ? "rmt" : "isr");
        }
        return;
    }

    if (strcmp(sub, "status") == 0) {
        glog("SubGHz running: %s\n", subghz_remote_manager_is_running() ? "yes" : "no");
        glog("SubGHz paused: %s\n", subghz_remote_manager_is_paused() ? "yes" : "no");
        glog("SubGHz last error: %s\n", subghz_remote_manager_get_last_error());
        glog("SubGHz active snapshot: %s\n", subghz_remote_manager_get_active_snapshot_name());
        glog("SubGHz cfg: SPI%d MOSI=%d MISO=%d SCK=%d CSN=%d GDO0=%d GDO2=%d\n",
             CONFIG_SUBGHZ_SPI_HOST,
             CONFIG_SUBGHZ_SPI_MOSI_PIN,
             CONFIG_SUBGHZ_SPI_MISO_PIN,
             CONFIG_SUBGHZ_SPI_SCK_PIN,
             CONFIG_SUBGHZ_CSN_PIN,
             CONFIG_SUBGHZ_GDO0_PIN,
             CONFIG_SUBGHZ_GDO2_PIN);
        return;
    }

    if (strcmp(sub, "capture_on") == 0) {
        subghz_remote_manager_set_raw_capture_enabled(true);
        glog("SubGHz raw capture enabled\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state capture_on");
        }
        return;
    }

    if (strcmp(sub, "capture_off") == 0) {
        subghz_remote_manager_set_raw_capture_enabled(false);
        glog("SubGHz raw capture disabled\n");
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state capture_off");
        }
        return;
    }

    if (strcmp(sub, "capture_begin") == 0) {
        if (argc < 4) {
            glog("Usage: subghz capture_begin <normal|raw> <frequency_hz>\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state capture_begin_error invalid arguments");
            }
            return;
        }

        const char *mode = argv[2];
        bool raw_mode = (strcmp(mode, "raw") == 0);
        bool valid_mode = (strcmp(mode, "normal") == 0 || raw_mode);
        uint32_t frequency_hz = (uint32_t)strtoul(argv[3], NULL, 10);
        if (!valid_mode || frequency_hz == 0) {
            glog("SubGHz capture begin failed: invalid arguments\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state capture_begin_error invalid arguments");
            }
            return;
        }

        if (!subghz_remote_manager_begin_capture(raw_mode, frequency_hz, stream_to_peer, 2000)) {
            glog("SubGHz capture begin failed: %s\n", subghz_remote_manager_get_last_error());
            if (stream_to_peer) {
                char state_cmd[128];
                snprintf(state_cmd,
                         sizeof(state_cmd),
                         "state capture_begin_error %s",
                         subghz_remote_manager_get_last_error());
                esp_comm_manager_send_command("subghz", state_cmd);
            }
            return;
        }

        glog("SubGHz capture armed: %s @ %s\n", mode, subghz_remote_manager_get_frequency_label());
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state capture_begin_ok");
        }
        return;
    }

    if (strcmp(sub, "cycle_freq") == 0) {
        subghz_remote_manager_cycle_frequency();
        glog("SubGHz freq: %s\n", subghz_remote_manager_get_frequency_label());
        if (stream_to_peer) {
            char state_cmd[64];
            snprintf(state_cmd, sizeof(state_cmd), "state freq_%s", subghz_remote_manager_get_frequency_label());
            esp_comm_manager_send_command("subghz", state_cmd);
        }
        return;
    }

    if (strcmp(sub, "capture") == 0) {
        const char *name_hint = (argc >= 3) ? argv[2] : NULL;
        if (subghz_remote_manager_capture_snapshot(name_hint)) {
            glog("SubGHz snapshot captured: %s\n", subghz_remote_manager_get_active_snapshot_name());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state capture_ok");
            }
        } else {
            glog("SubGHz capture failed: %s\n", subghz_remote_manager_get_last_error());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state capture_error");
            }
        }
        return;
    }

    if (strcmp(sub, "save") == 0) {
        const char *name_hint = (argc >= 3) ? argv[2] : NULL;
        char saved_path[192] = {0};
        if (subghz_remote_manager_save_snapshot(name_hint, saved_path, sizeof(saved_path))) {
            glog("SubGHz snapshot saved: %s\n", saved_path);
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state save_ok");
            }
        } else {
            glog("SubGHz save failed: %s\n", subghz_remote_manager_get_last_error());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state save_error");
            }
        }
        return;
    }

    if (strcmp(sub, "load") == 0 || strcmp(sub, "replay") == 0) {
        const char *target = (argc >= 3) ? argv[2] : "last";
        if (subghz_remote_manager_load_snapshot(target)) {
            glog("SubGHz snapshot loaded: %s\n", subghz_remote_manager_get_active_snapshot_name());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state load_ok");
            }
        } else {
            glog("SubGHz load failed: %s\n", subghz_remote_manager_get_last_error());
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state load_error");
            }
        }
        return;
    }

    if (strcmp(sub, "list") == 0) {
        char names[12][SUBGHZ_SNAPSHOT_NAME_MAX];
        int n = subghz_remote_manager_list_snapshots(names, 12);
        if (n <= 0) {
            glog("No SubGHz snapshots found\n");
            if (stream_to_peer) {
                esp_comm_manager_send_command("subghz", "state list_empty");
            }
            return;
        }
        int shown = (n < 12) ? n : 12;
        glog("SubGHz snapshots (%d total, showing %d):\n", n, shown);
        for (int i = 0; i < shown; i++) {
            glog("  %s\n", names[i]);
        }
        if (stream_to_peer) {
            esp_comm_manager_send_command("subghz", "state list_ok");
        }
        return;
    }

    glog("Unknown subghz subcommand: %s\n", sub);
#else
#ifdef CONFIG_HAS_SUBGHZ_REMOTE
    glog("SubGHz local scanner not enabled on this build (remote/display role only)\n");
#else
    glog("SubGHz not enabled on this build\n");
#endif
    if (remote_request && esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("subghz", "state error");
    }
#endif
}

