// cmd_lora.c
// LoRa command handler — mirrors cmd_subghz/cmd_nrf24 structure.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/lora_channels.h"
#include "managers/lora_manager.h"
#include "managers/lora_mesh.h"
#include "managers/lora_modem.h"
#include "managers/lora_pki.h"
#include "managers/lora_sx1262.h"
#include "managers/lora_ble.h"
#include "managers/lora_phoneapi.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

void handle_lora_cmd(int argc, char **argv) {
#ifndef CONFIG_HAS_LORA
    (void)argc; (void)argv;
    glog("LoRa not enabled on this board (enable HAS_LORA in Kconfig)\n");
    return;
#else
    if (argc >= 2 && strcmp(argv[1], "help") == 0) {
        glog("lora                 Show status\n"
             "lora on / off        Start / stop\n"
             "lora send <text>     Send a message\n"
             "lora dm <node> <tx>  PKI direct message\n"
             "lora messages        Read messages\n"
             "lora nodes           Show nearby nodes\n"
             "lora pubkey          Show our Curve25519 public key\n"
             "lora nodeinfo [node] Exchange NodeInfo now (broadcast by default)\n"
             "lora pki-regen       Regenerate PKI keypair\n"
             "lora pkinfo <node>   Show peer + our pubkeys\n"
             "lora pkitest <node>  PKI encrypt/decrypt loopback (no TX)\n"
             "lora pkselftest      X25519/SHA-256/AES-CCM known-answer tests\n"
             "lora pktry <hex>     PKI decrypt-variant probe on a captured frame\n"
             "lora channels        Show 8 channel slots\n"
             "lora region <name>   Set region while stopped (e.g. anz)\n"
             "lora set <preset|sf|bw|cr|tx|hop|offset|ovrfreq|chnum|txen|role|owner|companion> <value>\n"
             "Advanced: diag, cad, reg, ble, app, setup\n");
        return;
    }
    const char *sub = argc < 2 ? "status" : argv[1];
    if (strcmp(sub, "on") == 0) sub = "start";
    if (strcmp(sub, "off") == 0) sub = "stop";
    if (strcmp(sub, "send") == 0 || strcmp(sub, "messages") == 0) sub = "chat";
    if (strcmp(sub, "region") == 0 && argc == 3) {
        char *expanded[] = {argv[0], "set", "region", argv[2]};
        handle_lora_cmd(4, expanded);
        return;
    }
    if (strcmp(sub, "setup") == 0) {
        char q[640];
        if (lora_manager_setup_text(q, sizeof(q))) glog("%s", q);
        else glog("LoRa setup unavailable\n");
        return;
    }
    if (strcmp(sub, "status") == 0) {
        lora_status_t st;
        lora_manager_get_status(&st);
        glog("LoRa: %s radio:%s region:%s freq:%u %s sf:%d bw:%d cr:4/%d tx:%d%s comp:%s msgs:%u err:%s\n",
             st.running ? "ON" : "OFF",
             st.radio_present ? "ok" : "--",
             lora_region_name((int)st.region),
             (unsigned)st.freq_hz,
             st.use_preset ? lora_preset_display_name(st.preset, true) : "custom",
             st.sf, st.bw_khz, st.cr, st.tx_dbm,
             st.tx_enabled ? "" : " TXOFF",
             st.companion == 1 ? "wifi" : "ble",
             (unsigned)lora_manager_msg_count(),
             lora_manager_last_error());
        return;
    }
    if (strcmp(sub, "start") == 0) {
        if (lora_manager_needs_setup()) {
            char q[640];
            if (lora_manager_setup_text(q, sizeof(q))) glog("%s", q);
            return;
        }
        if (lora_manager_start()) {
            glog("LoRa started\n");
        } else {
            glog("LoRa start failed: %s\n", lora_manager_last_error());
        }
        return;
    }
    if (strcmp(sub, "stop") == 0) {
        lora_manager_stop();
        glog("LoRa stopped\n");
        return;
    }
    if (strcmp(sub, "set") == 0) {
        if (argc < 4) {
            glog("Usage: lora set region <name> | lora set <preset|sf|bw|cr|tx|hop|offset|ovrfreq|chnum|txen|role|companion> <val>\n");
            return;
        }
        if (strcmp(argv[2], "region") == 0) {
            int code = lora_region_by_name(argv[3]);
            if (code < 0) {
                glog("Unknown region '%s' (try us915 eu868 eu433 cn jp anz kr tw ru in nz865 th ua433 my433 my919 sg923 ph433 ph868 ph915 anz433 kz433 kz863 np865 br902)\n",
                     argv[3]);
                return;
            }
            bool ok = lora_manager_set_region((lora_region_t)code);
            glog(ok ? "LoRa region saved (takes effect on start)\n" : "set region failed: %s\n",
                 lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "preset") == 0) {
            bool ok = lora_manager_set_preset(atoi(argv[3]));
            glog(ok ? "LoRa preset saved\n" : "set preset failed: %s (0..16)\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "sf") == 0) {
            lora_status_t st; lora_manager_get_status(&st);
            bool ok = lora_manager_set_modem(atoi(argv[3]), st.bw_khz, st.cr);
            glog(ok ? "LoRa SF saved (custom modem)\n" : "set sf failed: %s\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "bw") == 0) {
            lora_status_t st; lora_manager_get_status(&st);
            bool ok = lora_manager_set_modem(st.sf, atoi(argv[3]), st.cr);
            glog(ok ? "LoRa BW saved (custom modem)\n" : "set bw failed: %s\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "cr") == 0) {
            lora_status_t st; lora_manager_get_status(&st);
            bool ok = lora_manager_set_modem(st.sf, st.bw_khz, atoi(argv[3]));
            glog(ok ? "LoRa CR saved\n" : "set cr failed: %s (5..8)\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "tx") == 0) {
            lora_status_t st; lora_manager_get_status(&st);
            bool ok = lora_manager_set_params(st.sf, st.bw_khz, atoi(argv[3]));
            glog(ok ? "LoRa TX saved\n" : "set tx failed: %s\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "offset") == 0) {
            bool ok = lora_manager_set_freq_offset((float)atof(argv[3]));
            glog(ok ? "LoRa freq offset saved\n" : "set offset failed (MHz, +/-2.0)\n");
            return;
        }
        if (strcmp(argv[2], "ovrfreq") == 0) {
            bool ok = lora_manager_set_override_freq((float)atof(argv[3]));
            glog(ok ? "LoRa override freq saved\n" : "set ovrfreq failed (MHz, 0=off)\n");
            return;
        }
        if (strcmp(argv[2], "chnum") == 0) {
            bool ok = lora_manager_set_channel_num((uint32_t)atoi(argv[3]));
            glog(ok ? "LoRa channel num saved\n" : "set chnum failed (0=hash, 1..512)\n");
            return;
        }
        if (strcmp(argv[2], "txen") == 0) {
            bool ok = lora_manager_set_tx_enabled(atoi(argv[3]) != 0);
            glog(ok ? "LoRa tx_enabled saved\n" : "set txen failed\n");
            return;
        }
        if (strcmp(argv[2], "role") == 0) {
            bool ok = lora_manager_set_role(atoi(argv[3]));
            glog(ok ? "LoRa role saved\n" : "set role failed (0..12)\n");
            return;
        }
        if (strcmp(argv[2], "owner") == 0) {
            if (argc < 5) {
                char lo[40], sh[8];
                lora_mesh_owner(lo, sizeof(lo), sh, sizeof(sh));
                glog("LoRa owner: long='%s' short='%s'\nUsage: lora set owner <long> <short>\n", lo, sh);
                return;
            }
            bool ok = lora_mesh_set_owner(argv[3], argv[4]);
            glog(ok ? "LoRa owner saved (announced on next NodeInfo)\n"
                    : "set owner failed (long<40, short<8 chars)\n");
            return;
        }
        if (strcmp(argv[2], "hop") == 0) {
            bool ok = lora_manager_set_hop_limit(atoi(argv[3]));
            glog(ok ? "LoRa hop limit saved\n" : "set hop failed: %s\n", lora_manager_last_error());
            return;
        }
        if (strcmp(argv[2], "companion") == 0) {
            bool ok = lora_manager_set_companion(
                (strcmp(argv[3], "wifi") == 0) ? 1 : 0);
            glog(ok ? "LoRa companion saved (reboot/start to apply; no-PSRAM = wifi XOR ble)\n"
                    : "set companion failed\n");
            return;
        }
        glog("Unknown lora set key\n");
        return;
    }
    if (strcmp(sub, "channels") == 0) {        for (uint8_t i = 0; i < 8; i++) {
            const lora_channel_t *ch = lora_channel_get(i);
            if (!ch || !ch->used || ch->role == 0) {
                glog("  ch%u: disabled\n", (unsigned)i);
            } else {
                glog("  ch%u: %s role=%u hash=%02x psk_len=%u up=%u dn=%u name='%s'\n",
                     (unsigned)i, i == lora_channel_primary() ? "PRIMARY" : "secondary",
                     (unsigned)ch->role, ch->hash, (unsigned)ch->psk_len,
                     (unsigned)ch->uplink, (unsigned)ch->downlink, ch->name);
            }
        }
        return;
    }
    if (strcmp(sub, "chat") == 0) {
        if (argc < 3) {
            // Drain mode: print retained messages.
            uint32_t seq = 0;
            lora_msg_t msgs[8];
            uint16_t n = lora_manager_msg_since(&seq, msgs, 8);
            if (n == 0) glog("(no messages — `lora chat <text>` to send)\n");
            for (int i = 0; i < n; i++) {
                glog("[%s] %s\n", msgs[i].who, msgs[i].text);
            }
            return;
        }
        char tmp[160] = {0};
        for (int i = 2; i < argc; i++) {
            if (i > 2) strlcat(tmp, " ", sizeof(tmp));
            strlcat(tmp, argv[i], sizeof(tmp));
        }
        glog(lora_manager_send_text(tmp) ? "LoRa queued\n" : "LoRa send failed: %s\n",
             lora_manager_last_error());
        return;
    }
    if (strcmp(sub, "nodes") == 0) {
        uint16_t n = lora_mesh_nodes(NULL, 0);
        if (n == 0) {
            glog("LoRa nodes: none yet (need air traffic or app link)\n");
            return;
        }
        for (uint16_t i = 0; i < n; i++) {
            lora_mesh_node_t node;
            if (!lora_mesh_node_at(i, &node)) break;
            char shown[8] = {0};
            if (node.has_user && node.short_name[0])
                snprintf(shown, sizeof(shown), "%s", node.short_name);
            else
                snprintf(shown, sizeof(shown), "%06X",
                         (unsigned)(node.node_num & 0xFFFFFF));
            glog("  !%08x %-8s rssi:%d snr:%.1f%s%s\n",
                 (unsigned)node.node_num, shown,
                 node.last_rssi, (double)node.last_snr,
                 node.has_pubkey ? " pki" : "",
                 node.key_verified ? "+" : "");
        }
        return;
    }
    if (strcmp(sub, "dm") == 0) {
        if (argc < 4) {
            glog("Usage: lora dm <nodehex|!xxxxxxxx> <text> (needs peer public key)\n");
            return;
        }
        char *end = NULL;
        uint32_t nn = (uint32_t)strtoul(argv[2][0] == '!' ? argv[2] + 1 : argv[2], &end, 16);
        if (!end || *end != '\0' || nn < 4) {
            glog("Bad node id '%s'\n", argv[2]);
            return;
        }
        char tmp[160] = {0};
        for (int i = 3; i < argc; i++) {
            if (i > 3) strlcat(tmp, " ", sizeof(tmp));
            strlcat(tmp, argv[i], sizeof(tmp));
        }
        glog(lora_manager_send_dm_text(tmp, nn, 0, true, NULL) ? "LoRa DM queued (PKI)\n"
                                                               : "LoRa DM failed: %s\n",
             lora_manager_last_error());
        return;
    }
    if (strcmp(sub, "pubkey") == 0) {
        const uint8_t *pub = lora_pki_public();
        if (!pub) {
            glog("LoRa PKI not ready\n");
            return;
        }
        glog("LoRa pubkey: ");
        for (int i = 0; i < 32; i++) glog("%02X", pub[i]);
        glog("\n");
        return;
    }
    if (strcmp(sub, "nodeinfo") == 0) {
        uint32_t to = LORA_MESH_BROADCAST;
        if (argc >= 3) {
            char *end = NULL;
            to = (uint32_t)strtoul(argv[2][0] == '!' ? argv[2] + 1 : argv[2],
                                   &end, 16);
            if (!end || *end != '\0' || to < 4 || to == LORA_MESH_BROADCAST) {
                glog("Usage: lora nodeinfo [nodehex|!xxxxxxxx]\n");
                return;
            }
        }
        // Include want_response so a peer reset/factory reset repairs both
        // halves of the key exchange: it learns our key and returns its own.
        bool ok = lora_mesh_send_nodeinfo(to, true);
        if (ok) glog("LoRa NodeInfo sent to %08x\n", (unsigned)to);
        else {
            // Direct sends can lose their one CAD attempt to a relay already
            // on air. Queue the normal broadcast exchange instead of making
            // the user race the channel manually. This also survives issuing
            // the command just before `lora start`.
            lora_mesh_request_nodeinfo();
            glog("LoRa NodeInfo busy/stopped; broadcast exchange queued\n");
        }
        return;
    }
    if (strcmp(sub, "pki-regen") == 0) {
        glog(lora_pki_regen() ? "LoRa keypair regenerated (re-share NodeInfo)\n"
                              : "LoRa key regen failed\n");
        return;
    }
    if (strcmp(sub, "pkinfo") == 0) {
        if (argc < 3) {
            glog("Usage: lora pkinfo <nodehex|!xxxxxxxx>\n");
            return;
        }
        char *end = NULL;
        uint32_t nn = (uint32_t)strtoul(argv[2][0] == '!' ? argv[2] + 1 : argv[2], &end, 16);
        if (!end || *end != '\0' || nn < 4) {
            glog("Bad node id '%s'\n", argv[2]);
            return;
        }
        uint8_t peer[32];
        if (lora_mesh_peer_pubkey(nn, peer)) {
            glog("peer !%08x: ", (unsigned)nn);
            for (int i = 0; i < 32; i++) glog("%02X", peer[i]);
            glog("\n");
            memset(peer, 0, sizeof(peer));
        } else {
            glog("peer !%08x: no key\n", (unsigned)nn);
        }
        const uint8_t *pub = lora_pki_public();
        if (!pub) {
            glog("self: no key\n");
        } else {
            glog("self !%08x: ", (unsigned)lora_mesh_node_num());
            for (int i = 0; i < 32; i++) glog("%02X", pub[i]);
            glog("\n");
        }
        return;
    }
    if (strcmp(sub, "pkitest") == 0) {
        // Loopback self-test (no air TX): encrypt a small payload to the
        // peer's stored key, decrypt it back, compare. Uses only
        // lora_mesh_peer_pubkey + lora_pki_encrypt/decrypt. Both sides use
        // from=self so the CCM nonce matches (same from+id+extra).
        if (argc < 3) {
            glog("Usage: lora pkitest <nodehex|!xxxxxxxx>\n");
            return;
        }
        char *end = NULL;
        uint32_t nn = (uint32_t)strtoul(argv[2][0] == '!' ? argv[2] + 1 : argv[2], &end, 16);
        if (!end || *end != '\0' || nn < 4) {
            glog("Bad node id '%s'\n", argv[2]);
            return;
        }
        uint8_t peer[32];
        if (!lora_mesh_peer_pubkey(nn, peer)) {
            glog("pkitest !%08x: FAIL no peer key\n", (unsigned)nn);
            return;
        }
        static const char tpt[] = "GhostPKItest12";
        uint16_t tplen = (uint16_t)(sizeof(tpt) - 1);
        uint32_t pid = 0;
        while (pid == 0) pid = esp_random();
        uint8_t ct[64];
        uint8_t back[64];
        uint32_t me = lora_mesh_node_num();
        uint16_t ct_len = lora_pki_encrypt(nn, me, peer, pid,
                                          (const uint8_t *)tpt, tplen,
                                          ct, sizeof(ct));
        if (!ct_len) {
            memset(peer, 0, sizeof(peer));
            glog("pkitest !%08x: FAIL encrypt pt=%u\n", (unsigned)nn, (unsigned)tplen);
            return;
        }
        uint16_t bk_len = lora_pki_decrypt(me, peer, pid, ct, ct_len, back, sizeof(back));
        memset(peer, 0, sizeof(peer));
        memset(ct, 0, sizeof(ct));
        bool ok = (bk_len == tplen && memcmp(back, tpt, tplen) == 0);
        memset(back, 0, sizeof(back));
        glog("pkitest !%08x: %s pt=%u ct=%u back=%u id=%08x\n",
             (unsigned)nn, ok ? "OK" : "FAIL",
             (unsigned)tplen, (unsigned)ct_len, (unsigned)bk_len, (unsigned)pid);
        return;
    }
    if (strcmp(sub, "pkselftest") == 0) {
        uint8_t failed = lora_pki_selftest();
        glog("PKI selftest: %s x25519-v1=%s x25519-v2=%s sha256=%s ccm=%s mask=%02x\n",
             failed ? "FAIL" : "PASS",
             (failed & 0x01) ? "FAIL" : "OK",
             (failed & 0x02) ? "FAIL" : "OK",
             (failed & 0x04) ? "FAIL" : "OK",
             (failed & 0x08) ? "FAIL" : "OK",
             (unsigned)failed);
        return;
    }
    if (strcmp(sub, "pktry") == 0) {
        // Decrypt-variant probe (no TX): parse a full captured frame
        // (16B header + PKI payload) as hex and try each stock-framing
        // variant against the sender's stored peer key. One ESP_LOGI line
        // per variant; see lora_mesh_pktry.
        if (argc < 3) {
            glog("Usage: lora pktry <framehex> (16B header + PKI payload, max 255B)\n");
            return;
        }
        const char *hx = argv[2];
        size_t hlen = strlen(hx);
        if (hlen == 0 || (hlen & 1) || hlen > 510) {
            glog("Bad frame hex (even hex chars, max 510 = 255B)\n");
            return;
        }
        uint8_t frame[255];
        size_t blen = hlen / 2;
        for (size_t i = 0; i < blen; i++) {
            char c1 = hx[2 * i], c2 = hx[2 * i + 1];
            int v1 = (c1 >= '0' && c1 <= '9') ? c1 - '0' :
                     (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 :
                     (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
            int v2 = (c2 >= '0' && c2 <= '9') ? c2 - '0' :
                     (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 :
                     (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
            if (v1 < 0 || v2 < 0) {
                glog("Bad hex at byte %u\n", (unsigned)i);
                memset(frame, 0, sizeof(frame));
                return;
            }
            frame[i] = (uint8_t)((v1 << 4) | v2);
        }
        glog("pktry %uB: trying variants (see log)\n", (unsigned)blen);
        lora_mesh_pktry(frame, (uint8_t)blen);
        memset(frame, 0, sizeof(frame));
        return;
    }
    if (strcmp(sub, "diag") == 0) {
        lora_status_t st; lora_manager_get_status(&st);
        glog("LoRa diag tx_ok:%u tx_fail:%u relay:%u rx_ok:%u crc:%u dups:%u qdrop:%u duty:%u rssi:%d snr:%.1f err:%s\n",
             (unsigned)st.tx_ok, (unsigned)st.tx_fail, (unsigned)st.tx_relay,
             (unsigned)st.rx_ok, (unsigned)st.rx_crc_err, (unsigned)st.rx_dups,
             (unsigned)st.q_drops, (unsigned)st.duty_drops, st.last_rssi,
             (double)st.last_snr, lora_manager_last_error());
        uint32_t pushed, popped, dropped;
        bool linked;
        lora_phoneapi_stats(&pushed, &popped, &dropped, &linked);
        glog("LoRa app fifo push:%u pop:%u drop:%u linked:%s adv:%s conn:%s\n",
             (unsigned)pushed, (unsigned)popped, (unsigned)dropped,
             linked ? "yes" : "no",
             lora_ble_is_advertising() ? "yes" : "no",
             lora_ble_is_connected() ? "yes" : "no");
        return;
    }
    if (strcmp(sub, "cad") == 0) {
        // Channel-activity probe: 5 CAD trials with instant RSSI. DET=1 with
        // RSSI near the floor (-110..-120) = false detection (thresholds);
        // DET=1 with hot RSSI = real energy on our slot (peer/interferer).
        for (int i = 0; i < 5; i++) {
            int16_t rssi = 0;
            (void)lora_radio_rssi_inst(&rssi);
            bool busy = lora_radio_cad();
            glog("LoRa cad %d: busy=%d rssi=%d\n", i, busy ? 1 : 0, rssi);
        }
        return;
    }
    if (strcmp(sub, "reg") == 0) {
        if (argc < 3) {
            glog("Usage: lora reg <hex-address> [count 1..8]\n");
            return;
        }
        char *end = NULL;
        unsigned long addr = strtoul(argv[2], &end, 16);
        int count = (argc >= 4) ? atoi(argv[3]) : 1;
        if (!end || *end != '\0' || addr > 0xFFFF || count < 1 || count > 8) {
            glog("Invalid register address/count (example: lora reg 0740 2)\n");
            return;
        }
        uint8_t vals[8] = {0};
        if (lora_radio_read_register((uint16_t)addr, vals, (uint8_t)count) != 0) {
            glog("LoRa register read failed (radio must be started)\n");
            return;
        }
        glog("LoRa reg 0x%04X:", (unsigned)addr);
        for (int i = 0; i < count; i++) glog(" %02X", vals[i]);
        glog("\n");
        return;
    }
    if (strcmp(sub, "ble") == 0) {
        const char *a = (argc >= 3) ? argv[2] : "status";
        if (strcmp(a, "on") == 0) {
            glog(lora_ble_start() ? "LoRa BLE advertising\n"
                                  : "LoRa BLE refused (stop BLE scans/advs first)\n");
        } else if (strcmp(a, "off") == 0) {
            lora_ble_stop();
            glog("LoRa BLE stopped\n");
        } else {
            glog("LoRa BLE adv:%s conn:%s linked:%s\n",
                 lora_ble_is_advertising() ? "yes" : "no",
                 lora_ble_is_connected() ? "yes" : "no",
                 lora_ble_is_linked() ? "yes" : "no");
        }
        return;
    }
    if (strcmp(sub, "app") == 0) {
        uint32_t pushed, popped, dropped;
        bool linked;
        lora_phoneapi_stats(&pushed, &popped, &dropped, &linked);
        glog("LoRa app linked:%s fifo:%u/%u/%u (push/pop/drop) pending:%s\n",
             linked ? "yes" : "no",
             (unsigned)pushed, (unsigned)popped, (unsigned)dropped,
             lora_phoneapi_has_data() ? "yes" : "no");
        return;
    }
    if (strcmp(sub, "companion") == 0) {
        if (argc < 3) {
            glog("Usage: lora companion <ble|wifi> (applies on next start; no-PSRAM = XOR)\n");
            return;
        }
        bool ok = lora_manager_set_companion(
            (strcmp(argv[2], "wifi") == 0) ? 1 : 0);
        glog(ok ? "LoRa companion saved\n" : "set companion failed\n");
        return;
    }
    glog("Unknown lora subcommand\n");
#endif
}
