// lora_ble.c
// NimBLE GATT server speaking the Meshtastic phone protocol. No crypto on
// the BLE link itself (NO_PIN / Just Works); MeshPacket protos ride the link
// in the clear exactly like upstream. Air framing is stock Meshtastic RF
// (16B header + AES-CTR Data); see docs/lora-meshtastic-app.md.
//
// Transport parity with upstream meshtastic/firmware
// src/nimble/NimbleBluetooth.cpp:
//  - 512B ToRadio/FromRadio frames (ATT MTU 517 negotiated where available).
//    NOTE: the BLE transport here accepts/returns up to 512B, but the
//    PhoneAPI FIFO slot (LORA_PHONE_SLOT, owned by lora_phoneapi.h) is still
//    288B, so current FromRadio frames cap at ~288B until that slot grows.
//    ToRadio writes up to 512B are accepted and forwarded; the parser keeps
//    the leading fields (text/payload_head caps apply downstream).
//  - ToRadio queue depth 3 (matches TO_PHONE depth 3) + identical-consecutive
//    write dedup (lastToRadio).
//  - Battery service 0x180F / level 0x2A19 (read-only, fixed 100%) advertised
//    alongside the Meshtastic service so apps don't error on missing BAS.
//  - LogRadio NOTIFY|READ stub (empty reads) so log-streaming apps attach.
//  - High-throughput conn params during the config handshake, low-power
//    after; 2M PHY + 251B data-length attempts, all best-effort.

#include "managers/lora_ble.h"
#include "managers/lora_manager.h"
#include "managers/lora_mesh.h"
#include "managers/lora_pb.h"
#include "managers/lora_phoneapi.h"
#include "sdkconfig.h"

#if defined(CONFIG_HAS_LORA) && !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)

#include "managers/ble_manager.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#if __has_include("host/ble_att.h")
#include "host/ble_att.h"
#endif
#if __has_include("esp_gap_ble_api.h")
#include "esp_gap_ble_api.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include "os/os_mbuf.h"
#include "services/gatt/ble_svc_gatt.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "LoRaBLE";

// 512B frame support (NimbleBluetooth.cpp TORADIO_MAX). Error only above this.
#define TORADIO_MAX 512
// FromRadio read buffer: transport-side cap; phone FIFO slot may be smaller
// (see note above) — pop() just returns what fits.
#define FROMRADIO_BUF 512
// ToRadio queue depth matches TO_PHONE (LORA_PHONE_FIFO_DEPTH == 3).
#define TO_RADIO_DEPTH 3
#define BLE_PREFERRED_MTU 517
#define BLE_DLE_OCTETS 251
#define BLE_DLE_TIME_US 2120
#define FROMRADIO_READ_WAIT_MS 20000
#define LORA_APP_STACK_BYTES 7168
// Battery service/level (16-bit UUIDs, read-only fixed 100%).
#define BAT_SVC_UUID16 0x180F
#define BAT_LVL_UUID16 0x2A19
// LogRadio stub UUID. Prefix 5a3d6e49- matches upstream NimbleBluetooth.cpp;
// trailing groups below are a placeholder — replace with the exact upstream
// suffix if apps fail to bind the log stream. Kept as a full 128-bit string
// so the existing uuid128_le() parser accepts it unchanged.
#define MESHTASTIC_LOGRADIO_UUID "5a3d6e49-0000-4000-8000-000000000000"

static bool s_registered = false;   // GATT service installed
static bool s_advertising = false;
static bool s_linked = false;       // FromNum CCCD subscribed
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_fromradio_h = 0;
static uint16_t s_fromnum_h = 0;
static uint16_t s_bat_h = 0;
static uint16_t s_log_h = 0;
// UUID objects need file scope: adv_tick resolves handles post-sync.
static ble_uuid128_t s_svc_uuid, s_tor_uuid, s_frr_uuid, s_frn_uuid, s_log_uuid;
static ble_uuid16_t s_bat_svc_uuid, s_bat_chr_uuid;
static bool s_uuids_ready = false;
static esp_timer_handle_t s_adv_timer = NULL;
static QueueHandle_t s_to_radio_q = NULL;
static TaskHandle_t s_worker = NULL;
// lastToRadio dedup: drop identical consecutive ToRadio writes (app retries).
// Conn-param state: true while high-throughput params are in effect.
static bool s_fast_params = false;
static bool s_cfg_seen = false;

typedef struct {
    uint8_t data[TORADIO_MAX + 16];
    uint16_t len;
} toradio_item_t;

// "6ba1b218-..." -> 16B little-endian for NimBLE (full byte reversal of the
// canonical big-endian string; matches NimBLE-Arduino behavior).
static bool uuid128_le(const char *s, uint8_t out[16]) {
    uint8_t be[16];
    int ni = 0;
    for (int i = 0; s[i] && ni < 16; i++) {
        if (s[i] == '-') continue;
        int hi;
        char c = s[i];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        else return false;
        i++;
        char d = s[i];
        int lo;
        if (!d) return false;
        if (d >= '0' && d <= '9') lo = d - '0';
        else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
        else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
        else return false;
        be[ni++] = (uint8_t)((hi << 4) | lo);
    }
    if (ni != 16) return false;
    for (int i = 0; i < 16; i++) out[i] = be[15 - i];
    return true;
}

static void dev_name(char *out, size_t n) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) memset(mac, 0, sizeof(mac));
    snprintf(out, n, "Ghost-%02X%02X", mac[4], mac[5]);
}

// ---------- Link tuning (best-effort, non-fatal) ----------
// Request connection parameters via NimBLE (Bluedroid name guarded below).
static void req_conn_params(uint16_t conn, bool fast) {
#ifdef CONFIG_BT_NIMBLE_ENABLED
    struct ble_gap_upd_params p;
    memset(&p, 0, sizeof(p));
    if (fast) {
        // High throughput for the config handshake (7.5-15ms interval).
        p.itvl_min = 6;
        p.itvl_max = 12;
        p.latency = 0;
        p.supervision_timeout = 400; // 4s
        p.min_ce_len = 16;
        p.max_ce_len = 64;
    } else {
        // Low power for steady state (100-200ms interval).
        p.itvl_min = 80;
        p.itvl_max = 160;
        p.latency = 4;
        p.supervision_timeout = 600; // 6s
        p.min_ce_len = 8;
        p.max_ce_len = 32;
    }
    int rc = ble_gap_update_params(conn, &p);
    ESP_LOGI(TAG, "conn params %s request rc=%d", fast ? "fast" : "slow", rc);
#else
    (void)conn;
    (void)fast;
#endif
#if defined(ESP_BLUEDROID_ENABLED) && __has_include("esp_gap_ble_api.h")
    // Alternate stack conn-param name for reference (Bluedroid builds only);
    // NimBLE path above is the one used here. Best-effort, guarded.
    {
        esp_ble_conn_update_params_t bp;
        memset(&bp, 0, sizeof(bp));
        (void)bp;
    }
#endif
}

// MTU (517) + 2M PHY + 251B data-length attempts. Each step is rc-checked and
// compiled only where its feature macro/API exists in this IDF (non-fatal).
static void tune_link(uint16_t conn, bool fast) {
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
#ifdef CONFIG_BT_NIMBLE_ENABLED
    int rc = ble_att_set_preferred_mtu(BLE_PREFERRED_MTU);
    if (rc != 0) ESP_LOGW(TAG, "preferred MTU %u rc=%d", BLE_PREFERRED_MTU, rc);
#if defined(BLE_GAP_EVENT_PHY_UPDATE_COMPLETE)
    // 2M PHY: allow 1M|2M so 2M is used where supported, 1M fallback else.
    rc = ble_gap_set_prefered_le_phy(conn,
            (uint8_t)(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK),
            (uint8_t)(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK),
            BLE_GAP_LE_PHY_CODED_ANY);
    if (rc != 0) ESP_LOGW(TAG, "2M PHY request rc=%d", rc);
#endif
#if defined(BLE_GAP_EVENT_DATA_LEN_CHG)
    rc = ble_gap_set_data_len(conn, BLE_DLE_OCTETS, BLE_DLE_TIME_US);
    if (rc != 0) ESP_LOGW(TAG, "data-len %uB request rc=%d", BLE_DLE_OCTETS, rc);
#endif
#else
    (void)fast;
#endif
    req_conn_params(conn, fast);
    s_fast_params = fast;
}

// Flush per-connection transport state. PhoneAPI retains its unwrapped mesh
// backlog so packets received while disconnected reach the next phone session.
static void flush_link_queues(void) {
    if (s_to_radio_q) xQueueReset(s_to_radio_q);
    lora_phoneapi_reset();
    s_fast_params = false;
    s_cfg_seen = false;
}

// ---------- GATT access (NimBLE host task context: copy fast, never block) --
static int gatt_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (attr == s_fromradio_h) {
            // Upstream SEND_PACKETS behavior: drain one FIFO entry per read,
            // waiting for data when caught up.  Returning an immediate
            // zero-length value here makes current clients treat the BLE
            // transport as broken and reconnect, especially when a queued
            // read races the config_complete_id frame.
            static uint8_t buf[FROMRADIO_BUF];
            uint16_t n = lora_phoneapi_pop(buf, sizeof(buf));
            uint32_t waited_ms = 0;
            while (n == 0 && waited_ms < FROMRADIO_READ_WAIT_MS &&
                   s_conn != BLE_HS_CONN_HANDLE_NONE &&
                   (waited_ms < 100 || lora_phoneapi_config_active())) {
                // The PhoneAPI worker and mesh manager run outside the
                // NimBLE host task and can populate the FIFO while this ATT
                // read waits, matching Meshtastic's NimBLE transport.
                vTaskDelay(pdMS_TO_TICKS(5));
                waited_ms += 5;
                n = lora_phoneapi_pop(buf, sizeof(buf));
            }
            ESP_LOGI(TAG, "app READ fromradio -> %uB%s", (unsigned)n,
                     waited_ms ? " (waited)" : "");
            if (n > 0) {
                int rc = os_mbuf_append(ctxt->om, buf, n);
                // Do not notify FromNum from inside a FromRadio read.  The
                // client already continues draining the configuration stream,
                // and an extra doorbell queued after config_complete_id makes
                // it issue one read too many and eventually drop the link.
                return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            return 0; // empty read = caught up
        }
        if (attr == s_fromnum_h) {
            uint32_t fn = lora_phoneapi_from_num();
            uint8_t le[4] = {(uint8_t)fn, (uint8_t)(fn >> 8), (uint8_t)(fn >> 16), (uint8_t)(fn >> 24)};
            int rc = os_mbuf_append(ctxt->om, le, 4);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr == s_bat_h) {
            // Battery level: read-only, fixed 100% (no fuel gauge on board).
            uint8_t lvl = 100;
            int rc = os_mbuf_append(ctxt->om, &lvl, 1);
            return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (attr == s_log_h) {
            // LogRadio stub: empty reads so log-streaming apps attach cleanly.
            return 0;
        }
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        // 512B frame support: error only above TORADIO_MAX.
        if (len == 0 || len > TORADIO_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        toradio_item_t it;
        it.len = len;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, it.data, sizeof(it.data), NULL);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        if (len <= 64) {
            char hex[195] = {0};
            for (int i = 0; i < len && i < 64; i++) snprintf(hex + i * 3, 4, "%02X ", it.data[i]);
            ESP_LOGI(TAG, "app WRITE toradio %uB [%s]", (unsigned)len, hex);
        } else {
            char hex[195] = {0};
            for (int i = 0; i < 64; i++) snprintf(hex + i * 3, 4, "%02X ", it.data[i]);
            ESP_LOGI(TAG, "app WRITE toradio %uB [%s...]", (unsigned)len, hex);
        }
        // Packet retry deduplication belongs in PhoneAPI and is keyed by
        // MeshPacket.id. Raw-write dedup can suppress legitimate repeated
        // want_config/heartbeat requests, whose bytes may be identical.
        // Hand to the worker (PhoneAPI -> mesh TX can block on CAD/airtime).
        if (s_to_radio_q && xQueueSend(s_to_radio_q, &it, 0) != pdTRUE) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return 0;
    }
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

static void worker_task(void *arg) {
    (void)arg;
    toradio_item_t it;
    while (1) {
        if (xQueueReceive(s_to_radio_q, &it, portMAX_DELAY) == pdTRUE) {
            lora_phoneapi_on_toradio(it.data, it.len);
            // Echoes/config replies land in the FIFO; ring the doorbell.
            if (lora_phoneapi_has_data()) lora_ble_notify_from_num();
        }
    }
}

// ---------- GAP ----------
static int gap_cb(struct ble_gap_event *ev, void *arg) {
    (void)arg;
    if (ev->type == BLE_GAP_EVENT_CONNECT) {
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "app connected (handle %u)", (unsigned)s_conn);
            // High-throughput params + MTU/PHY/DLE attempts (best-effort).
            tune_link(s_conn, true);
        } else {
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            s_advertising = false;
            // Failed attempt (e.g. supervision timeout during pairing):
            // re-arm so the next app open still finds us.
            if (s_registered && s_adv_timer) {
                esp_timer_start_once(s_adv_timer, 500 * 1000);
            }
        }
    } else if (ev->type == BLE_GAP_EVENT_DISCONNECT) {
        ESP_LOGI(TAG, "app disconnected");
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_linked = false;
        lora_phoneapi_set_linked(false);
        flush_link_queues(); // drop stale transport/config frames, keep mesh backlog
        s_advertising = false;
        // Never re-enter GAP APIs here: this runs in the NimBLE host task and
        // restarting advertising inline can wedge the host (same reason
        // upstream defers it). Just arm the timer; adv_tick (timer task)
        // does the real work.
        if (s_registered && s_adv_timer) {
            esp_timer_start_once(s_adv_timer, 500 * 1000);
        }
    } else if (ev->type == BLE_GAP_EVENT_SUBSCRIBE) {
        if (ev->subscribe.attr_handle == s_fromnum_h) {
            bool sub = ev->subscribe.cur_notify != 0;
            s_linked = sub;
            lora_phoneapi_set_linked(sub);
            ESP_LOGI(TAG, "app %s", sub ? "linked (subscribed)" : "unsubscribed");
            if (sub) {
                // Handshake is throughput-sensitive: go fast; the notify path
                // drops back to low-power once the config sequence drains.
                if (s_conn != BLE_HS_CONN_HANDLE_NONE) tune_link(s_conn, true);
            } else {
                // set_linked(false) closed the PhoneAPI session; also discard
                // writes that were queued by that client.
                if (s_to_radio_q) xQueueReset(s_to_radio_q);
            }
        } else if (ev->subscribe.attr_handle == s_log_h) {
            ESP_LOGI(TAG, "app %ssubscribed logradio (stub)",
                     ev->subscribe.cur_notify ? "" : "un");
        }
    } else if (ev->type == BLE_GAP_EVENT_MTU) {
        ESP_LOGI(TAG, "app MTU updated: %u", (unsigned)ev->mtu.value);
#ifdef BLE_GAP_EVENT_PHY_UPDATE_COMPLETE
    } else if (ev->type == BLE_GAP_EVENT_PHY_UPDATE_COMPLETE) {
        ESP_LOGI(TAG, "app PHY updated: status=%u tx=%u rx=%u",
                 (unsigned)ev->phy_updated.status,
                 (unsigned)ev->phy_updated.tx_phy,
                 (unsigned)ev->phy_updated.rx_phy);
#endif
#ifdef BLE_GAP_EVENT_DATA_LEN_CHG
    } else if (ev->type == BLE_GAP_EVENT_DATA_LEN_CHG) {
        ESP_LOGI(TAG, "app data-len changed: tx=%uB rx=%uB",
                 (unsigned)ev->data_len_chg.max_tx_octets,
                 (unsigned)ev->data_len_chg.max_rx_octets);
#endif
    } else if (ev->type == BLE_GAP_EVENT_ADV_COMPLETE) {
        s_advertising = false;
    }
    return 0;
}

static void adv_tick(void *arg) {
    (void)arg;
    if (s_advertising || !s_registered) return;
    if (!ble_hs_synced()) {
        esp_timer_start_once(s_adv_timer, 500 * 1000);
        return;
    }
    // Runs in timer task (not host task), so GAP state checks are safe here.
    // If something else owns the controller, back off and retry.
    if (ble_gap_disc_active() || ble_gap_adv_active()) {
        esp_timer_start_once(s_adv_timer, 1000 * 1000);
        return;
    }
    // Handles are assigned at ble_gatts_start() (sync), not at add_svcs().
    // Resolve (or verify the val_handle write) before anyone can connect.
    if (!s_uuids_ready) return;
    if (s_fromradio_h == 0) {
        if (ble_gatts_find_chr(&s_svc_uuid.u, &s_frr_uuid.u, NULL, &s_fromradio_h) != 0 ||
            s_fromradio_h == 0) {
            ESP_LOGW(TAG, "adv: FromRadio handle not ready, retry");
            esp_timer_start_once(s_adv_timer, 500 * 1000);
            return;
        }
    }
    if (s_fromnum_h == 0) {
        if (ble_gatts_find_chr(&s_svc_uuid.u, &s_frn_uuid.u, NULL, &s_fromnum_h) != 0 ||
            s_fromnum_h == 0) {
            ESP_LOGW(TAG, "adv: FromNum handle not ready, retry");
            esp_timer_start_once(s_adv_timer, 500 * 1000);
            return;
        }
    }
    struct ble_hs_adv_fields adv = {0};
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // uuids128 takes ble_uuid128_t STRUCTS (type+value), not raw bytes —
    // passing raw bytes makes the stack read a garbage type and drop the
    // service UUID from the packet (app then never sees us).
    adv.uuids128 = &s_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    // Battery service rides along as a non-complete 16-bit list so phones
    // and the Meshtastic app see BAS without a second discovery.
    adv.uuids16 = &s_bat_svc_uuid;
    adv.num_uuids16 = 1;
    adv.uuids16_is_complete = 0;
    if (ble_gap_adv_set_fields(&adv) != 0) return;
    char name[16];
    dev_name(name, sizeof(name));
    struct ble_hs_adv_fields rsp = {0};
    rsp.name = (uint8_t *)name;
    rsp.name_len = (uint8_t)strlen(name);
    rsp.name_is_complete = 1;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0) return;
    struct ble_gap_adv_params p = {0};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          &p, gap_cb, NULL) == 0) {
        s_advertising = true;
        ESP_LOGI(TAG, "advertising as %s", name);
    }
}

static esp_err_t pre_host_init(void *arg) {
    (void)arg;
    // Stale bonds from an older NimBLE struct layout break restore with
    // "NVS data size mismatch" noise (and stale re-pair attempts). Same cure
    // as upstream: wipe the bond namespace once when a record's size differs
    // from this build's structs. Runs before the host first reads the store.
    {
        nvs_handle_t h = 0;
        bool mismatch = false;
        if (nvs_open("nimble_bond", NVS_READONLY, &h) == ESP_OK) {
            size_t sz = 0;
            mismatch =
                (nvs_get_blob(h, "our_sec_1", NULL, &sz) == ESP_OK && sz != sizeof(struct ble_store_value_sec)) ||
                (nvs_get_blob(h, "peer_sec_1", NULL, &sz) == ESP_OK && sz != sizeof(struct ble_store_value_sec)) ||
                (nvs_get_blob(h, "cccd_sec_1", NULL, &sz) == ESP_OK && sz != sizeof(struct ble_store_value_cccd));
            nvs_close(h);
        }
        if (mismatch) {
            ESP_LOGW(TAG, "wiping incompatible BLE bonds (struct size changed)");
            if (nvs_open("nimble_bond", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
        }
    }
    static uint8_t svc_le[16], tor_le[16], frr_le[16], frn_le[16], log_le[16];
    if (!uuid128_le(MESHTASTIC_SVC_UUID, svc_le) ||
        !uuid128_le(MESHTASTIC_TORADIO_UUID, tor_le) ||
        !uuid128_le(MESHTASTIC_FROMRADIO_UUID, frr_le) ||
        !uuid128_le(MESHTASTIC_FROMNUM_UUID, frn_le) ||
        !uuid128_le(MESHTASTIC_LOGRADIO_UUID, log_le)) {
        ESP_LOGE(TAG, "pre_host: UUID parse failed");
        return ESP_FAIL;
    }
    // Stable storage: the stack keeps these pointers. Objects live at file
    // scope so adv_tick can resolve handles post-sync (see below).
    s_svc_uuid.u.type = BLE_UUID_TYPE_128;
    s_tor_uuid.u.type = BLE_UUID_TYPE_128;
    s_frr_uuid.u.type = BLE_UUID_TYPE_128;
    s_frn_uuid.u.type = BLE_UUID_TYPE_128;
    s_log_uuid.u.type = BLE_UUID_TYPE_128;
    memcpy(s_svc_uuid.value, svc_le, 16);
    memcpy(s_tor_uuid.value, tor_le, 16);
    memcpy(s_frr_uuid.value, frr_le, 16);
    memcpy(s_frn_uuid.value, frn_le, 16);
    memcpy(s_log_uuid.value, log_le, 16);
    s_bat_svc_uuid.u.type = BLE_UUID_TYPE_16;
    s_bat_svc_uuid.value = BAT_SVC_UUID16;
    s_bat_chr_uuid.u.type = BLE_UUID_TYPE_16;
    s_bat_chr_uuid.value = BAT_LVL_UUID16;
    s_uuids_ready = true;
    static struct ble_gatt_chr_def chrs[] = {
        {
            .uuid = &s_tor_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_WRITE,
        },
        {
            // FromRadio stays READ-only (upstream SEND_PACKETS semantics).
            .uuid = &s_frr_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_READ,
            .val_handle = &s_fromradio_h,
        },
        {
            // FromNum stays NOTIFY|READ (doorbell + poll fallback).
            .uuid = &s_frn_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_fromnum_h,
        },
        {
            // LogRadio stub: NOTIFY|READ, empty reads (apps attach cleanly).
            .uuid = &s_log_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_log_h,
        },
        {0},
    };
    static struct ble_gatt_chr_def bat_chrs[] = {
        {
            // Battery level: read-only, fixed 100%.
            .uuid = &s_bat_chr_uuid.u,
            .access_cb = gatt_access,
            .flags = BLE_GATT_CHR_F_READ,
            .val_handle = &s_bat_h,
        },
        {0},
    };
    static struct ble_gatt_svc_def defs[] = {
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &s_svc_uuid.u,
            .characteristics = chrs,
        },
        {
            .type = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid = &s_bat_svc_uuid.u,
            .characteristics = bat_chrs,
        },
        {0},
    };
    // Size the host's static GATT tables for our service BEFORE add/start.
    // Without this, ble_hs_max_services stays 0 and registration dies at
    // sync with ENOMEM (rc=6 -> host assert). Safe to repeat after a deinit
    // cycle (stop() zeroes the counters); paired with one init per host life.
    ble_gatts_count_cfg(defs);
    // NOTE: add_svcs only STAGES the defs; attribute handles are assigned at
    // ble_gatts_start() (host sync), so val_handles are still 0 here by
    // design. adv_tick() resolves them post-sync before advertising.
    int rc = ble_gatts_add_svcs(defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "pre_host: add_svcs rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "pre_host: service staged");
    // NOTE: no ble_svc_gap_device_name_set — the GAP service is owned
    // elsewhere in this tree (ble_bridge) and our adv name rides in the
    // scan response we build ourselves in adv_tick().
    // Preferred ATT MTU 517 for 512B frames (best-effort, rc-checked).
#ifdef CONFIG_BT_NIMBLE_ENABLED
    {
        int mtu_rc = ble_att_set_preferred_mtu(BLE_PREFERRED_MTU);
        if (mtu_rc != 0) ESP_LOGW(TAG, "pre_host: preferred MTU rc=%d", mtu_rc);
    }
#endif
    if (!s_to_radio_q) {
#if defined(CONFIG_SPIRAM)
        /* Queue payloads are plain byte copies and never touched by an ISR;
         * keep this ~1.6KB allocation out of scarce contiguous internal RAM. */
        s_to_radio_q = xQueueCreateWithCaps(TO_RADIO_DEPTH, sizeof(toradio_item_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!s_to_radio_q)
            s_to_radio_q = xQueueCreate(TO_RADIO_DEPTH, sizeof(toradio_item_t));
    }
    if (!s_to_radio_q) {
        ESP_LOGE(TAG, "pre_host: queue create failed");
        return ESP_FAIL;
    }
    if (!s_worker) {
        // ToRadio dispatch runs protobuf builders (toradio_t
        // ~440B on-stack), the full admin handler (~1KB across nested
        // config/channel/pki frames), NVS store access, ESP_LOG formatting
        // and the notify path. 3072 overflowed; 7168 preserves that margin
        // while fitting the Advance 4.3's observed 7680B internal block.
        // Keep the stack internal because this task performs NVS flash writes.
        if (xTaskCreate(worker_task, "lora_app", LORA_APP_STACK_BYTES,
                        NULL, 8, &s_worker) != pdPASS) {
            ESP_LOGE(TAG, "pre_host: worker task create failed");
            s_worker = NULL;
            return ESP_FAIL;
        }
    }
    if (!s_adv_timer) {
        esp_timer_create_args_t a = {.callback = adv_tick, .name = "lora_adv"};
        if (esp_timer_create(&a, &s_adv_timer) != ESP_OK) {
            ESP_LOGE(TAG, "pre_host: timer create failed");
            return ESP_FAIL;
        }
    }
    s_registered = true;
    return ESP_OK;
}

static void pre_host_cleanup(void *arg) {
    (void)arg;
    s_registered = false;
    s_advertising = false;
    s_linked = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    // Handles die with the host (ATT db is rebuilt on next start); force
    // re-resolution so a stale value can never dispatch reads wrongly.
    s_fromradio_h = 0;
    s_fromnum_h = 0;
    s_bat_h = 0;
    s_log_h = 0;
    s_fast_params = false;
    s_cfg_seen = false;
}

bool lora_ble_start(void) {
    if (s_advertising) return true;
    if (ble_is_initialized()) {
        if (!s_registered) {
            // Host already up (scans used it): GATT registration needs a host
            // restart in this tree, so refuse rather than half-attach.
            ESP_LOGW(TAG, "BLE host already up without LoRa service — reboot to switch modes");
            return false;
        }
        // Re-entry (e.g. after `lora ble off` or a disconnect): service is
        // installed, just re-arm advertising below.
    } else if (!ble_init_with_pre_host(pre_host_init, pre_host_cleanup, NULL)) {
        ESP_LOGW(TAG, "BLE host init failed");
        return false;
    }
    // Host exists now: GAP queries are safe (they fault on a dead stack).
    // Exclusive radio: refuse while scans/advertising owned by others run.
    if (ble_gap_disc_active() || ble_gap_adv_active()) {
        ESP_LOGW(TAG, "BLE busy (scan/adv) — stop it first");
        return false;
    }
    esp_timer_start_once(s_adv_timer, 100 * 1000);
    return true;
}

void lora_ble_stop(void) {
    s_advertising = false;
    s_linked = false;
    lora_phoneapi_set_linked(false);
    flush_link_queues(); // set_linked(false) path: drop stale frames
    // Full teardown like every other BLE flow: releases the host and lets
    // ble_resume_networking() bring WiFi/AP back (on no-PSRAM the AP was
    // fully deinitialized on the way in). Without this the radio keeps
    // running but WebUI/AP stays down with no obvious recovery.
    if (ble_is_initialized()) ble_deinit();
}

bool lora_ble_is_advertising(void) { return s_advertising; }
bool lora_ble_is_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool lora_ble_is_linked(void) { return s_linked; }

void lora_ble_notify_from_num(void) {
    if (!s_linked || s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    // Conn-param schedule: fast during the config handshake, low-power after
    // it drains (transitions only, best-effort).
    bool active = lora_phoneapi_config_active();
    if (active) {
        s_cfg_seen = true;
        if (!s_fast_params) {
            req_conn_params(s_conn, true);
            s_fast_params = true;
        }
    } else if (s_cfg_seen && s_fast_params) {
        req_conn_params(s_conn, false);
        s_fast_params = false;
        s_cfg_seen = false;
    }
    // Prefer notify; the app falls back to polling FromNum anyway.
    ble_gatts_notify(s_conn, s_fromnum_h);
}

#else
// Non-LoRa or no-native-BLE targets: inert stubs (globbed always).
#include <stdbool.h>
bool lora_ble_start(void) { return false; }
void lora_ble_stop(void) {}
bool lora_ble_is_advertising(void) { return false; }
bool lora_ble_is_connected(void) { return false; }
bool lora_ble_is_linked(void) { return false; }
void lora_ble_notify_from_num(void) {}
#endif
