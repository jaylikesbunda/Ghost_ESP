// lora_phoneapi.c
// PhoneAPI facade: FIFO + want_config sequence + ToRadio dispatch.
// The BLE transport (lora_ble.c) only moves bytes; all framing lives here.

#include "managers/lora_phoneapi.h"
#include "managers/lora_ble.h"
#include "managers/lora_channels.h"
#include "managers/lora_manager.h"
#include "managers/lora_mesh.h"
#include "managers/lora_modem.h"
#include "managers/lora_pb.h"
#include "managers/lora_pki.h"
#include "managers/lora_store.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

// Bridge to manager-local caches (lora_manager.c; header owned elsewhere).
extern void lora_manager_note_ignored(uint32_t node, bool ignored);
extern uint32_t lora_manager_peer_last_heard(uint32_t node);

static const char *TAG = "LoRaApp";

static uint8_t s_fifo[LORA_PHONE_FIFO_DEPTH][LORA_PHONE_SLOT];
static uint16_t s_flen[LORA_PHONE_FIFO_DEPTH];
static uint8_t s_head = 0; // pop index
static uint8_t s_count = 0;
static uint32_t s_from_num = 0;
static bool s_linked = false;
static uint32_t s_pushed = 0, s_popped = 0, s_dropped = 0;
static uint32_t s_reboots = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

// App requests can arrive while the config walk owns the three-slot FIFO.
// Keep the baseline queue internal and expand it to native ESP32-S3 depth only
// when PSRAM is present.  This avoids taking another 5.8 KiB from Heltec V3.
#define LORA_PHONE_PENDING_BASE_DEPTH 8
#define LORA_PHONE_PENDING_PSRAM_DEPTH 32
#define SPECIAL_NONCE_ONLY_CONFIG 69420u
#define SPECIAL_NONCE_ONLY_NODES  69421u
static uint8_t s_pending_base[LORA_PHONE_PENDING_BASE_DEPTH][240];
static uint16_t s_pending_len_base[LORA_PHONE_PENDING_BASE_DEPTH];
static uint8_t (*s_pending)[240] = s_pending_base;
static uint16_t *s_pending_len = s_pending_len_base;
static uint8_t s_pending_depth = LORA_PHONE_PENDING_BASE_DEPTH;
static uint8_t s_pending_head = 0;
static uint8_t s_pending_count = 0;

// MeshService keeps QueueStatus in its own high-priority queue (depth two in
// mesh-pb-constants.h). The separate allocator has four objects, but the
// actual to-phone queue is deliberately two entries.
// It outlives the BLE PhoneAPI object and is drained before mesh packets.
#define LORA_PHONE_STATUS_DEPTH 2
static uint32_t s_status_id[LORA_PHONE_STATUS_DEPTH];
static uint8_t s_status_head = 0;
static uint8_t s_status_count = 0;

// Upstream PhoneAPI rejects retransmitted MeshPackets by packet ID, not by
// comparing raw BLE writes. Keep this across BLE reconnects for the lifetime
// of the boot, matching recentToRadioPacketIds.
#define LORA_RECENT_TORADIO_IDS 20
static uint32_t s_recent_toradio_ids[LORA_RECENT_TORADIO_IDS];
static uint8_t s_recent_toradio_pos = 0;

static bool toradio_id_seen(uint32_t id) {
    if (id == 0) return false;
    for (uint8_t i = 0; i < LORA_RECENT_TORADIO_IDS; i++) {
        if (s_recent_toradio_ids[i] == id) return true;
    }
    s_recent_toradio_ids[s_recent_toradio_pos] = id;
    s_recent_toradio_pos =
        (uint8_t)((s_recent_toradio_pos + 1) % LORA_RECENT_TORADIO_IDS);
    return false;
}

// Lazy config sequence: upstream generates config items on demand per poll
// (PhoneAPI state machine), so a depth-3 FIFO never overflows. Pushing all
// frames upfront would drop most of them and the app would never converge.
// Step order mirrors upstream getFromRadio EXACTLY (client apps assume it):
// my_info, uiconfig, own_nodeinfo, metadata, region presets, channels×8,
// configs×10, modules×17, peers, complete. Special client nonces split this
// into config-only and node-only stages; see begin_config().
typedef enum {
    STEP_IDLE = 0,
    STEP_MYINFO,
    STEP_UIDATA,
    STEP_SELF,
    STEP_META,
    STEP_REGION,
    STEP_CHANNEL,
    STEP_CONFIG,
    STEP_MODULE,
    STEP_PEERS,
    STEP_COMPLETE,
} cfg_step_t;
static cfg_step_t s_step = STEP_IDLE;
static uint8_t s_sub = 0; // sub-index for CHANNEL/CONFIG/MODULE steps
static uint32_t s_nonce = 0;
// Stable config-walk order without duplicating the complete RAM NodeDB.
// A peer removed while syncing is skipped; newly learned peers appear on the
// next node-only sync, matching snapshot semantics.
static uint32_t s_peer_ids[LORA_MESH_NODES_MAX];
static uint16_t s_peer_n = 0;
static uint16_t s_peer_idx = 0;

static void fifo_push_locked(const uint8_t *p, uint16_t n) {
    if (s_count >= LORA_PHONE_FIFO_DEPTH) {
        s_dropped++;
        return;
    }
    uint8_t idx = (uint8_t)((s_head + s_count) % LORA_PHONE_FIFO_DEPTH);
    memcpy(s_fifo[idx], p, n);
    s_flen[idx] = n;
    s_count++;
    s_from_num++;
    s_pushed++;
}

static bool pending_push_locked(const uint8_t *mesh, uint16_t n) {
    if (!mesh || n == 0 || n > sizeof(s_pending[0])) {
        s_dropped++;
        return false;
    }
    // Stock's toPhoneQueue evicts its oldest queued packet when a new text
    // arrives to a full queue. Most disconnected traffic admitted here is
    // chat; retaining the newest packet is safer than silently losing it.
    if (s_pending_count >= s_pending_depth) {
        s_pending_head =
            (uint8_t)((s_pending_head + 1) % s_pending_depth);
        s_pending_count--;
        s_dropped++;
    }
    uint8_t idx = (uint8_t)((s_pending_head + s_pending_count) % s_pending_depth);
    memcpy(s_pending[idx], mesh, n);
    s_pending_len[idx] = n;
    s_pending_count++;
    return true;
}

static bool pending_prepend_locked(const uint8_t *mesh, uint16_t n) {
    if (!mesh || n == 0 || n > sizeof(s_pending[0])) {
        s_dropped++;
        return false;
    }
    // Recovered FIFO entries are older than anything already deferred. If
    // capacity is exhausted, discard the newest deferred entry, not this
    // already-admitted unread packet.
    if (s_pending_count >= s_pending_depth) {
        s_pending_count--;
        s_dropped++;
    }
    s_pending_head = (uint8_t)((s_pending_head + s_pending_depth - 1) %
                               s_pending_depth);
    memcpy(s_pending[s_pending_head], mesh, n);
    s_pending_len[s_pending_head] = n;
    s_pending_count++;
    return true;
}

static void status_push_locked(uint32_t mesh_packet_id) {
    if (s_status_count >= LORA_PHONE_STATUS_DEPTH) {
        s_status_head = (uint8_t)((s_status_head + 1) % LORA_PHONE_STATUS_DEPTH);
        s_status_count--;
        s_dropped++;
    }
    uint8_t idx = (uint8_t)((s_status_head + s_status_count) % LORA_PHONE_STATUS_DEPTH);
    s_status_id[idx] = mesh_packet_id;
    s_status_count++;
}

static void status_prepend_locked(uint32_t mesh_packet_id) {
    if (s_status_count >= LORA_PHONE_STATUS_DEPTH) {
        s_status_count--; // discard newest deferred status
        s_dropped++;
    }
    s_status_head = (uint8_t)((s_status_head + LORA_PHONE_STATUS_DEPTH - 1) %
                              LORA_PHONE_STATUS_DEPTH);
    s_status_id[s_status_head] = mesh_packet_id;
    s_status_count++;
}

static void status_drop_heartbeats_locked(void) {
    uint32_t keep[LORA_PHONE_STATUS_DEPTH];
    uint8_t n = 0;
    for (uint8_t i = 0; i < s_status_count; i++) {
        uint32_t id = s_status_id[(s_status_head + i) % LORA_PHONE_STATUS_DEPTH];
        if (id != 0) keep[n++] = id;
    }
    for (uint8_t i = 0; i < n; i++) s_status_id[i] = keep[i];
    s_status_head = 0;
    s_status_count = n;
}

static void status_drain_locked(void) {
    while (s_step == STEP_IDLE && s_status_count > 0 &&
           s_count < LORA_PHONE_FIFO_DEPTH) {
        uint8_t fr[LORA_PHONE_SLOT];
        uint16_t fl = pb_build_fromradio_queue_id(fr, sizeof(fr), s_from_num + 1,
                                                   s_status_id[s_status_head]);
        if (fl) fifo_push_locked(fr, fl);
        else s_dropped++;
        s_status_head = (uint8_t)((s_status_head + 1) % LORA_PHONE_STATUS_DEPTH);
        s_status_count--;
    }
}

// A BLE disconnect/config restart may invalidate already wrapped FromRadio
// frames. Recover FromRadio.packet (field 2) and QueueStatus (field 11) into
// their service-level queues; config envelopes are session state.
static void fifo_requeue_service_locked(void) {
    uint8_t queued = s_count;
    // Walk newest-to-oldest and prepend each item. The final order is the
    // original FIFO followed by items that were already deferred.
    for (uint8_t off = queued; off > 0; off--) {
        uint8_t idx = (uint8_t)((s_head + off - 1) % LORA_PHONE_FIFO_DEPTH);
        pb_r_t r;
        pb_r_init(&r, s_fifo[idx], s_flen[idx]);
        uint8_t field, wire;
        uint32_t varint;
        const uint8_t *bytes;
        uint16_t blen;
        while (pb_r_next(&r, &field, &wire, &varint, &bytes, &blen)) {
            if (field == 2 && wire == 2) {
                (void)pending_prepend_locked(bytes, blen);
                break;
            } else if (field == 11 && wire == 2) {
                pb_r_t qs;
                pb_r_init(&qs, bytes, blen);
                uint8_t qf, qw;
                uint32_t qv;
                const uint8_t *qb;
                uint16_t ql;
                uint32_t mesh_packet_id = 0;
                while (pb_r_next(&qs, &qf, &qw, &qv, &qb, &ql)) {
                    if (qf == 4 && qw == 0) {
                        mesh_packet_id = qv;
                        break;
                    }
                }
                // id 0 is a session heartbeat response. Upstream clears its
                // heartbeat flag on close; only packet QueueStatus survives.
                if (mesh_packet_id != 0) status_prepend_locked(mesh_packet_id);
                break;
            }
        }
    }
}

static void pending_drain_locked(void) {
    // Upstream PhoneAPI serves QueueStatus before ordinary mesh packets.
    status_drain_locked();
    while (s_pending_count > 0 && s_count < LORA_PHONE_FIFO_DEPTH) {
        uint8_t fr[LORA_PHONE_SLOT];
        uint16_t fl = pb_build_fromradio_packet(fr, sizeof(fr), s_from_num + 1,
                                                s_pending[s_pending_head],
                                                s_pending_len[s_pending_head]);
        if (!fl) {
            s_dropped++;
            s_pending_head = (uint8_t)((s_pending_head + 1) % s_pending_depth);
            s_pending_count--;
            continue;
        }
        fifo_push_locked(fr, fl);
        s_pending_head = (uint8_t)((s_pending_head + 1) % s_pending_depth);
        s_pending_count--;
    }
}

static bool mesh_push_locked(const uint8_t *mesh, uint16_t ml) {
    // MeshService's upstream toPhoneQueue lives outside a PhoneAPI/BLE
    // session: packets received while no phone is attached remain queued
    // for the next client.  Keep our unwrapped MeshPacket in s_pending while
    // disconnected so a later config handshake can finish before delivery.
    if (!s_linked)
        return pending_push_locked(mesh, ml);
    // Preserve ordering once anything has been deferred. A full steady-state
    // FIFO also spills here rather than discarding an otherwise valid packet.
    if (s_step != STEP_IDLE || s_pending_count > 0 ||
        s_count >= LORA_PHONE_FIFO_DEPTH) {
        bool ok = pending_push_locked(mesh, ml);
        if (ok && s_step == STEP_IDLE) pending_drain_locked();
        return ok;
    }
    uint8_t fr[LORA_PHONE_SLOT];
    uint16_t fl = pb_build_fromradio_packet(fr, sizeof(fr), s_from_num + 1, mesh, ml);
    if (!fl) {
        s_dropped++;
        return false;
    }
    fifo_push_locked(fr, fl);
    return true;
}

static void short_name(char *out, size_t n) {
    lora_mesh_owner(NULL, 0, out, n);
}

static void long_name(char *out, size_t n) {
    lora_mesh_owner(out, n, NULL, 0);
}

static void node_id_str(char *out, size_t n, uint32_t node) {
    snprintf(out, n, "!%08x", (unsigned)node);
}

// Phone-pushed device timezone (set_config{device.tzdef}): persisted so the
// app stops pushing it every connect, and reported in our device section.
static char s_tzdef[40] = {0};

static void tzdef_load(void) {
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(s_tzdef);
        if (nvs_get_str(h, "tzdef", s_tzdef, &n) != ESP_OK) s_tzdef[0] = '\0';
        nvs_close(h);
    }
}

static void tzdef_save(const char *tz) {
    if (!tz) return;
    snprintf(s_tzdef, sizeof(s_tzdef), "%s", tz);
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "tzdef", s_tzdef);
        nvs_commit(h);
        nvs_close(h);
    }
}

// LoRaConfig fields the modem core doesn't apply (config.proto 12/13/15/
// 103..107): mirrored here so set_config values survive and the handshake
// re-reports them verbatim (lora_store drops section 6 by design, so the
// store alone can't round-trip them).
static pb_lora_extras_t s_lora_ex;

// Emulated stock firmware version reported to the phone (config-walk
// metadata + get_device_metadata). This is the Meshtastic compat version
// the app gates features on — NOT the GhostESP build version.
#define MESHTASTIC_COMPAT_FW "2.8.1"

// Last ACKed set_config[lora] values. The manager applies radio changes on
// a deferred task (~1s), so live status/modem reads go stale right after
// our ACK; get_config[lora] and the config-walk lora section report this
// snapshot while fresh so the app reads back what it just set. It doubles
// as the single-slot retry queue when the manager defer slot is busy
// (latest wins, flushed opportunistically below).
static struct {
    bool valid;
    bool retry; // manager defer slot was busy; still needs applying
    int64_t stamp_us;
    lora_region_t region;
    bool use_preset;
    int preset;
    int sf, bw, cr;     // requested (custom path; retry args)
    int esf, ebw, ecr;  // effective air params at ACK time (read-back)
    float fo, of;
    uint32_t chn;
    bool te;
    int pwr, hop;
} s_lora_pend;
#define LORA_PEND_FRESH_US (15 * 1000000LL)

static bool lora_pend_fresh(void) {
    if (!s_lora_pend.valid) return false;
    return (esp_timer_get_time() - s_lora_pend.stamp_us) < LORA_PEND_FRESH_US;
}

// Best-effort flush of a busy-queued request. Clears the retry flag once the
// manager accepts it; the snapshot stays for read-back either way.
static void lora_pend_flush(void) {
    if (!s_lora_pend.valid || !s_lora_pend.retry) return;
    if (lora_manager_apply_lora_cfg(s_lora_pend.region, s_lora_pend.use_preset,
                                    s_lora_pend.preset, s_lora_pend.sf,
                                    s_lora_pend.bw, s_lora_pend.cr,
                                    s_lora_pend.fo, s_lora_pend.of,
                                    s_lora_pend.chn, s_lora_pend.te,
                                    s_lora_pend.pwr, s_lora_pend.hop)) {
        s_lora_pend.retry = false;
        lora_manager_set_hop_limit(s_lora_pend.hop);
    }
}

// Snapshot the ACKed values (effective air params resolved for read-back).
static void lora_pend_save(lora_region_t region, bool use_preset, int preset,
                           int sf, int bw_khz, int cr, float fo, float of,
                           uint32_t chn, bool te, int pwr, int hop, bool retry) {
    // Current custom-path params for the preset-path mapping below
    // (get_modem_cfg NULL-checks each out pointer).
    int cpr = 0, csf = 11, cbw = 250;
    lora_manager_get_modem_cfg(NULL, &cpr, &csf, &cbw, NULL, NULL, NULL, NULL, NULL);
    lora_modem_cfg_t m;
    memset(&m, 0, sizeof(m));
    m.use_preset = use_preset;
    m.preset = use_preset ? preset : cpr;
    m.sf = use_preset ? csf : sf;
    m.bw_khz = use_preset ? cbw : bw_khz;
    m.cr = (cr >= 5 && cr <= 8) ? cr : (use_preset ? lora_preset_cr(preset) : 5);
    m.freq_offset_mhz = fo;
    m.override_freq_mhz = of;
    m.channel_num = chn;
    m.tx_enabled = te;
    int esf = 0, ebw = 0, ecr = 0;
    if (!lora_modem_effective(&m, &esf, &ebw, &ecr)) return; // post-validated; keep old snapshot
    s_lora_pend.valid = true;
    s_lora_pend.retry = retry;
    s_lora_pend.stamp_us = esp_timer_get_time();
    s_lora_pend.region = region;
    s_lora_pend.use_preset = use_preset;
    s_lora_pend.preset = preset;
    s_lora_pend.sf = sf;
    s_lora_pend.bw = bw_khz;
    s_lora_pend.cr = cr;
    s_lora_pend.esf = esf;
    s_lora_pend.ebw = ebw;
    s_lora_pend.ecr = ecr;
    s_lora_pend.fo = fo;
    s_lora_pend.of = of;
    s_lora_pend.chn = chn;
    s_lora_pend.te = te;
    s_lora_pend.pwr = pwr;
    s_lora_pend.hop = hop;
}

// Mirrors lora_manager_apply_lora_cfg validation (minus the busy-slot check)
// so a rejected apply can be told apart from a busy defer slot: fully-valid
// input that still fails means busy -> queue instead of BAD_REQUEST.
static bool lora_cfg_locally_valid(lora_region_t region, bool use_preset, int preset,
                                   int sf, int bw_khz, int cr, float fo, float of,
                                   uint32_t chn, int pwr, int hop) {
    if (lora_region_name((int)region)[0] == '?') return false;
    if (hop < 1 || hop > 7) return false;
    if (use_preset && !lora_preset_is_valid(preset)) return false;
    int mcr = (cr >= 5 && cr <= 8) ? cr : (use_preset ? lora_preset_cr(preset) : 5);
    if (!use_preset &&
        !lora_modem_resolve(false, 0, sf, bw_khz, mcr, NULL, NULL, NULL))
        return false;
    lora_modem_cfg_t m;
    memset(&m, 0, sizeof(m));
    m.use_preset = use_preset;
    m.preset = preset;
    m.sf = sf;
    m.bw_khz = bw_khz;
    m.cr = mcr;
    m.freq_offset_mhz = fo;
    m.override_freq_mhz = of;
    m.channel_num = chn;
    if (!lora_modem_effective(&m, NULL, NULL, NULL)) return false;
    if (fo < -2.0f || fo > 2.0f) return false;
    if (of < 0 || (of > 0.1f && (of < 150.0f || of > 960.0f))) return false;
    if (chn > 512) return false;
    lora_hw_t hw;
    if (!lora_manager_get_hw(&hw)) return false;
    int tp = (pwr == 0) ? hw.max_tx_dbm : pwr;
    if (tp < 2 || tp > hw.max_tx_dbm) return false;
    return true;
}

// Per-channel ChannelSettings.module_settings=7 (ModuleSettings opaque:
// position_precision=1, is_muted=2), both directions. lora_channel_t has no
// slot for it, so it lives here keyed by channel index.
#define LORA_CH_MOD_MAX 12
static uint8_t s_ch_mod[8][LORA_CH_MOD_MAX];
static uint8_t s_ch_mod_len[8];

// Peer favorite/ignored flags (admin set_favorite_node=39,
// remove_favorite_node=40, set_ignored_node=47, remove_ignored_node=48),
// reported as NodeInfo.is_favorite=10 / is_ignored=11.
typedef struct {
    uint32_t node_num;
    bool favorite;
    bool ignored;
} peer_flag_t;
static peer_flag_t s_peer_flags[16];

static peer_flag_t *peer_flags(uint32_t node_num, bool create) {
    for (unsigned i = 0; i < 16; i++) {
        if (s_peer_flags[i].node_num == node_num && node_num != 0)
            return &s_peer_flags[i];
    }
    if (!create || node_num < 4) return NULL;
    for (unsigned i = 0; i < 16; i++) {
        if (s_peer_flags[i].node_num == 0) {
            s_peer_flags[i].node_num = node_num;
            s_peer_flags[i].favorite = false;
            s_peer_flags[i].ignored = false;
            return &s_peer_flags[i];
        }
    }
    return NULL; // table full: flag lost (ACK still sent)
}

static void peer_flag_get(uint32_t node_num, bool *fav, bool *ign) {
    if (fav) *fav = false;
    if (ign) *ign = false;
    for (unsigned i = 0; i < 16; i++) {
        if (s_peer_flags[i].node_num == node_num && node_num != 0) {
            if (fav) *fav = s_peer_flags[i].favorite;
            if (ign) *ign = s_peer_flags[i].ignored;
            return;
        }
    }
}

// Phone-side peer flags persistence (NVS namespace "lora", blob "peer_flags").
// The mesh agent persists the mesh peer table; favorite/ignored live only here,
// so they are mirrored to NVS on every mutation and restored at boot.
// Fail-open: NVS errors or missing/empty blob leave RAM behavior unchanged.
#define PEER_FLAGS_NVS_KEY "peer_flags"
typedef struct {
    uint32_t node;
    uint8_t fav;
    uint8_t ign;
    uint8_t _rsv[2];
} peer_flag_save_t;

static void peer_flags_save(void) {
    peer_flag_save_t out[16];
    for (unsigned i = 0; i < 16; i++) {
        out[i].node = s_peer_flags[i].node_num;
        out[i].fav = s_peer_flags[i].favorite ? 1 : 0;
        out[i].ign = s_peer_flags[i].ignored ? 1 : 0;
        out[i]._rsv[0] = 0;
        out[i]._rsv[1] = 0;
    }
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, PEER_FLAGS_NVS_KEY, out, sizeof(out)) == ESP_OK)
            nvs_commit(h);
        nvs_close(h);
    }
}

static void peer_flags_load(void) {
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    peer_flag_save_t in[16];
    size_t n = sizeof(in);
    esp_err_t e = nvs_get_blob(h, PEER_FLAGS_NVS_KEY, in, &n);
    nvs_close(h);
    if (e != ESP_OK) return; // empty: keep RAM defaults
    if (n != sizeof(in)) return; // size mismatch: fail-open
    memset(s_peer_flags, 0, sizeof(s_peer_flags));
    for (unsigned i = 0; i < 16; i++) {
        if (in[i].node < 4) continue;
        s_peer_flags[i].node_num = in[i].node;
        s_peer_flags[i].favorite = in[i].fav ? true : false;
        s_peer_flags[i].ignored = in[i].ign ? true : false;
        if (s_peer_flags[i].favorite)
            lora_mesh_peer_set_favorite(s_peer_flags[i].node_num, true);
        if (s_peer_flags[i].ignored) {
            lora_manager_note_ignored(s_peer_flags[i].node_num, true);
            lora_mesh_peer_set_ignored(s_peer_flags[i].node_num, true);
        }
    }
}

// Canned messages (set 36 / get 10) + ringtone (set 37 / get 14). Upstream
// persists these in flash; we mirror the last set_* payload in NVS ns "lora"
// (blobs "canned"/"ringtone") so get_* round-trips instead of empty.
// Cap 192B (~200B): keeps the 11/15 response inside admin_reply's 200B frame
// (field-1 string + AdminMessage tag overhead). Fail-open: NVS errors keep
// RAM behavior and the admin ACK still goes out.
#define LORA_CANNED_MAX 192
#define LORA_RINGTONE_MAX 192
static char s_canned[LORA_CANNED_MAX + 1];
static char s_ringtone[LORA_RINGTONE_MAX + 1];

static void msgblob_load(const char *key, char *slot, size_t cap) {
    if (!key || !slot || cap == 0) return;
    slot[0] = '\0';
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    // Two-step read: query size first so an oversized blob truncates instead
    // of failing (nvs_get_blob errors when the buffer is too small).
    size_t need = 0;
    if (nvs_get_blob(h, key, NULL, &need) == ESP_OK && need > 0) {
        size_t want = need < cap - 1 ? need : cap - 1;
        size_t got = want;
        if (nvs_get_blob(h, key, (uint8_t *)slot, &got) == ESP_OK)
            slot[got < cap - 1 ? got : cap - 1] = '\0';
        else
            slot[0] = '\0';
    }
    nvs_close(h);
}

static void msgblob_save(const char *key, char *slot, size_t cap,
                         const uint8_t *d, uint16_t len) {
    if (!key || !slot || cap == 0) return;
    uint16_t c = len < cap - 1 ? len : (uint16_t)(cap - 1);
    if (c && d) memcpy(slot, d, c);
    slot[c] = '\0';
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        if (c == 0) nvs_erase_key(h, key); // cleared: drop the key
        else nvs_set_blob(h, key, slot, c);
        nvs_commit(h); // fail-open: errors ignored, RAM copy still serves
        nvs_close(h);
    }
}

// Native-parity reboot-class actions. The Routing ACK is queued by the shared
// tail of lora_phoneapi_handle_admin AFTER these arm functions return, and
// the BLE worker (lora_ble.c worker_task) rings the FromNum doorbell on
// return — so each deferred task re-rings the doorbell, waits ~500ms for the
// phone to poll the ACK, and only then tears anything down. A direct
// esp_restart()/sleep inside the handler would kill BLE before the ACK goes
// out (same reason lora_manager_apply_lora_cfg defers radio reconfigs to
// apply_radio_task in lora_manager.c: vTaskDelay then act, xTaskCreate 4096).
static TaskHandle_t s_admin_defer_task = NULL;

static void admin_reboot_task(void *arg) {
    (void)arg;
    lora_ble_notify_from_num(); // flush the ACK notify first
    vTaskDelay(pdMS_TO_TICKS(500)); // let the phone poll the ACK
    ESP_LOGI(TAG, "admin reboot: restarting now (ACK flushed)");
    esp_restart();
    vTaskDelete(NULL); // unreachable
}

static void admin_arm_reboot(const char *why) {
    if (s_admin_defer_task) return; // one deferred admin action at a time
    if (xTaskCreate(admin_reboot_task, "lora_reboot", 2048, NULL, 5,
                    &s_admin_defer_task) != pdPASS) {
        s_admin_defer_task = NULL;
        ESP_LOGW(TAG, "admin %s: defer task spawn failed, staying up", why);
    } else {
        ESP_LOGI(TAG, "admin %s: ACK first, restart in ~500ms", why);
    }
}

static void admin_shutdown_task(void *arg) {
    (void)arg;
    // Wake sources: GPIO0 (Heltec V3 PRG button, active-low) + 24h timer
    // failsafe. After esp_deep_sleep_start() only RTC wake (ext0/ext1/timer)
    // + the RESET button work; USB/UART/BLE stay down until wake. A PRG press
    // pulls GPIO0 low -> wake + reboot. EXT0 is used where the target has it
    // (S3/C3 classic RTC IO); ESP32-P4 has EXT1 only, so select at compile
    // time. The timer bounds sleep even if GPIO0 floats.
    // (No light-sleep fallback here: radio+BLE are already stopped below, so
    // a deep-sleep refusal still idles safely — see tail of this task.)
    lora_ble_notify_from_num(); // flush the ACK notify first
    vTaskDelay(pdMS_TO_TICKS(500)); // let the phone poll the ACK
    ESP_LOGI(TAG, "admin shutdown: stopping radio+BLE, deep-sleep "
                  "(wake: GPIO0 low / timer 24h)");
    lora_manager_stop(); // stops the LoRa radio + disconnects BLE
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
#if SOC_PM_SUPPORT_EXT0_WAKEUP
    if (esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0) != ESP_OK) {
        ESP_LOGW(TAG, "admin shutdown: ext0 GPIO0 unsupported, trying ext1");
        (void)esp_sleep_enable_ext1_wakeup_io(1ULL << GPIO_NUM_0,
                                              ESP_EXT1_WAKEUP_ANY_LOW);
    }
#elif SOC_PM_SUPPORT_EXT1_WAKEUP
    // ESP32-P4 has no EXT0/RTC-IO wakeup; GPIO0 is an LP/RTC pin, so use EXT1.
    if (esp_sleep_enable_ext1_wakeup_io(1ULL << GPIO_NUM_0,
                                        ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        ESP_LOGW(TAG, "admin shutdown: ext1 GPIO0 wake unsupported, timer only");
    }
#else
    ESP_LOGW(TAG, "admin shutdown: GPIO wake unsupported, timer wake only");
#endif
    (void)esp_sleep_enable_timer_wakeup(24ULL * 3600ULL * 1000000ULL);
    ESP_LOGI(TAG, "admin shutdown: entering deep sleep now");
    vTaskDelay(pdMS_TO_TICKS(100)); // let the log flush
    esp_deep_sleep_start();
    // Should never return; if it does, idle with radio+BLE already stopped.
    ESP_LOGW(TAG, "admin shutdown: deep-sleep refused, idling (radio+BLE off)");
    vTaskDelete(NULL);
}

static void admin_arm_shutdown(void) {
    if (s_admin_defer_task) return;
    if (xTaskCreate(admin_shutdown_task, "lora_shutdown", 4096, NULL, 5,
                    &s_admin_defer_task) != pdPASS) {
        s_admin_defer_task = NULL;
        ESP_LOGW(TAG, "admin shutdown: defer task spawn failed, staying up");
    } else {
        ESP_LOGI(TAG, "admin shutdown requested: ACK first, sleep in ~500ms");
    }
}

static void admin_factory_erase(void) {
    // Genuine factory: erase ALL keys in NVS namespace "lora" — owner,
    // nodenum, PKI identity, peers/pr_idx index + pr_* blobs, peer_flags,
    // channels, modem/region/tx/hop/role, tzdef, canned/ringtone, reboot
    // counter. Next boot re-derives nodenum from MAC + stock defaults.
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        esp_err_t e = nvs_erase_all(h);
        if (e == ESP_OK) e = nvs_commit(h);
        nvs_close(h);
        if (e == ESP_OK) ESP_LOGI(TAG, "admin factory_reset: NVS 'lora' erased");
        else ESP_LOGW(TAG, "admin factory_reset: NVS erase/commit FAILED");
    } else {
        ESP_LOGW(TAG, "admin factory_reset: NVS open failed");
    }
}

static void admin_nodedb_reset(void) {
    // Peer entries only, NO restart: clear the RAM table in one operation,
    // clear the phone-side favorite/ignored mirror, then erase indexed peer
    // blobs + peer_flags in one NVS transaction. The next
    // want_config handshake / NodeInfo exchange reflects the empty NodeDB.
    uint16_t n = lora_mesh_nodes(NULL, 0);
    lora_mesh_node_t nd;
    for (uint16_t i = 0; i < n; i++) {
        if (lora_mesh_node_at(i, &nd) && nd.node_num >= 4)
            lora_manager_note_ignored(nd.node_num, false);
    }
    uint16_t removed = lora_mesh_clear_nodes();
    memset(s_peer_flags, 0, sizeof(s_peer_flags));
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        // Erase per-peer blobs listed in either index (catches entries
        // evicted from RAM but still in flash), then the indexes themselves.
        for (int pass = 0; pass < 2; pass++) {
            const char *idxkey = pass ? "pr_idx" : "peers";
            uint32_t idx[LORA_MESH_PERSIST_MAX];
            size_t blen = sizeof(idx);
            if (nvs_get_blob(h, idxkey, idx, &blen) == ESP_OK) {
                size_t cnt = blen / sizeof(idx[0]);
                if (cnt > LORA_MESH_PERSIST_MAX) cnt = LORA_MESH_PERSIST_MAX;
                for (size_t k = 0; k < cnt; k++) {
                    if (idx[k] < 4) continue;
                    char key[16];
                    snprintf(key, sizeof(key), "pr_%08x", (unsigned)idx[k]);
                    (void)nvs_erase_key(h, key);
                }
            }
        }
        (void)nvs_erase_key(h, "peers");
        (void)nvs_erase_key(h, "pr_idx");
        (void)nvs_erase_key(h, PEER_FLAGS_NVS_KEY);
        (void)nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "admin nodedb_reset: removed %u/%u peers (RAM+NVS), no restart",
             (unsigned)removed, (unsigned)n);
}

// Own fixed position (admin set_fixed_position=41) as raw Position bytes,
// reported as NodeInfo.position=3 so the app map shows us.
static uint8_t s_self_pos[48];
static uint8_t s_self_pos_len = 0;

// First top-level LEN submessage with the given field number (nested admin /
// config parses). Zero-copy into the source buffer.
static bool find_submsg(const uint8_t *p, uint16_t len, uint8_t field,
                        const uint8_t **out, uint16_t *out_len) {
    if (!p || !out || !out_len) return false;
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t f, w;
    uint32_t v;
    const uint8_t *b;
    uint16_t bl;
    while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
        if (f == field && w == 2) {
            *out = b;
            *out_len = bl;
            return true;
        }
    }
    return false;
}

static bool find_varint(const uint8_t *p, uint16_t len, uint8_t field, uint32_t *out) {
    if (!p || !out) return false;
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t f, w;
    uint32_t v;
    const uint8_t *b;
    uint16_t bl;
    while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
        if (f == field && w == 0) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool find_fixed32(const uint8_t *p, uint16_t len, uint8_t field, uint32_t *out) {
    if (!p || !out) return false;
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t f, w;
    uint32_t v;
    const uint8_t *b;
    uint16_t bl;
    while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
        if (f == field && w == 5) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool find_float(const uint8_t *p, uint16_t len, uint8_t field, float *out) __attribute__((unused));
static bool find_float(const uint8_t *p, uint16_t len, uint8_t field, float *out) {
    uint32_t v = 0;
    if (!find_fixed32(p, len, field, &v)) return false;
    memcpy(out, &v, 4);
    return true;
}

// AdminMessage lives inside Data field 2 (wire 2): Data{1=port, 2=payload}.
// pb_parse_toradio leaves payload_head at the full Data submessage, so admin
// fields (32..67) must be searched inside field 2 bytes, not at Data level.
static bool admin_msg(const toradio_t *t, const uint8_t **out, uint16_t *out_len) {
    if (!t || !out || !out_len) return false;
    if (t->payload_len &&
        find_submsg(t->payload_head, t->payload_len, 2, out, out_len))
        return true;
    *out = t->payload_head;
    *out_len = t->payload_len;
    return true;
}

// Routing ACK + AdminMessage reply helpers.
static bool admin_ack(uint32_t me, uint32_t dst, uint32_t msg_id, uint8_t err) {
    uint8_t data[16];
    uint8_t routing[] = {0x18, err};
    uint16_t dl = pb_build_data_msg(data, sizeof(data), 5, routing, sizeof(routing), msg_id);
    uint32_t aid = 0;
    while (aid == 0) aid = esp_random();
    return dl && lora_phoneapi_push_mesh_data(me, dst, aid, data, dl);
}

static bool admin_reply(uint32_t me, uint32_t dst, uint32_t msg_id,
                        uint8_t resp_field, const uint8_t *inner, uint16_t inner_len) {
    uint8_t admin[200];
    pb_w_t w;
    pb_w_init(&w, admin, sizeof(admin));
    pb_w_msg(&w, resp_field, inner, inner_len);
    if (w.overflow) return false;
    uint8_t data[233];
    uint16_t dl = pb_build_data_msg(data, sizeof(data), 6, admin, w.len, msg_id);
    uint32_t rid = 0;
    while (rid == 0) rid = esp_random();
    return dl && lora_phoneapi_push_mesh_data(me, dst, rid, data, dl);
}

// Bandwidth tolerant parse: app may send Hz, kHz, or small codes.
static int bw_to_khz(uint32_t v) {
    if (v >= 62500) return (int)(v / 1000); // Hz
    if (v == 125 || v == 250 || v == 500 || v == 31) return (int)v;
    if (v == 0) return 0;
    return -1;
}

// Full AdminMessage dispatch (port 6). Native parity: all gets answered, sets
// applied for owner/channels/config/modules/time/canned/ringtone; reboot/
// shutdown/factory/nodedb-reset applied (reboot-class actions ACK first,
// then act from a deferred task so the phone sees the ACK before BLE dies).
// HAM mode (18) stays NAK (no-ham by design).
bool lora_phoneapi_handle_admin(const toradio_t *t, uint32_t me, uint32_t dst);

static uint8_t lora_region_pb(void) {
    lora_status_t st;
    memset(&st, 0, sizeof(st));
    lora_manager_get_status(&st);
    // Region is already stored as the upstream pb code; fall back to US.
    if (lora_region_name((int)st.region)[0] == '?') return MESHTASTIC_REGION_US;
    return (uint8_t)st.region;
}

void lora_phoneapi_reset(void) {
    portENTER_CRITICAL(&s_mux);
    fifo_requeue_service_locked();
    status_drop_heartbeats_locked();
    s_head = 0;
    s_count = 0;
    s_step = STEP_IDLE;
    portEXIT_CRITICAL(&s_mux);
    // from_num keeps counting (doorbell).  s_pending and s_status_* are
    // service-level queues, not session state, so they survive BLE disconnects
    // and fresh config walks like upstream MeshService queues.
}

bool lora_phoneapi_is_linked(void) { return s_linked; }
void lora_phoneapi_set_linked(bool linked) {
    if (linked && !s_linked) {
        lora_phoneapi_reset(); // fresh session on subscribe (fifo + steps)
        lora_manager_phone_session_reset();
    } else if (!linked && s_linked) {
        // A CCCD unsubscribe or ToRadio.disconnect closes PhoneAPI even when
        // GAP remains connected. Preserve unread service events, but discard
        // the transport/config walk exactly as a physical disconnect does.
        lora_phoneapi_reset();
    }
    s_linked = linked;
}

void lora_phoneapi_push_mesh_text(uint32_t from, uint32_t to, uint32_t id,
                                  float snr, uint32_t hop_limit, uint32_t hop_start,
                                  const char *text) {
    lora_phoneapi_push_mesh_text_ch(from, to, id, snr, hop_limit, hop_start, 0, text);
}

void lora_phoneapi_push_mesh_text_ch(uint32_t from, uint32_t to, uint32_t id,
                                     float snr, uint32_t hop_limit, uint32_t hop_start,
                                     uint32_t channel_idx, const char *text) {
    // Build + push atomically: FromRadio.id must equal the doorbell seq
    // assigned at push time (the app uses it for gap detection).
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_text_ch(mesh, sizeof(mesh), from, to, id, snr,
                                        hop_limit, hop_start, channel_idx, text);
    if (ml) {
        (void)mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
}

// Generic device->phone MeshPacket (Routing ACKs, admin replies): Data is
// prebuilt by the caller (port + optional payload + request_id echo).
// Returns true if a frame was queued (caller notifies the doorbell).
bool lora_phoneapi_push_mesh_data(uint32_t from, uint32_t to, uint32_t id,
                                  const uint8_t *data, uint16_t dlen) {
    if (!data || dlen == 0) return false;
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_packet(mesh, sizeof(mesh), from, to, id, data, dlen);
    bool ok = false;
    if (ml) {
        ok = mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool lora_phoneapi_push_mesh_raw(const uint8_t *mesh, uint16_t ml) {
    if (!mesh || ml == 0 || ml > LORA_PHONE_SLOT) return false;
    portENTER_CRITICAL(&s_mux);
    bool ok = mesh_push_locked(mesh, ml);
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool lora_phoneapi_push_mesh_data_pki(uint32_t from, uint32_t to, uint32_t id,
                                      uint32_t channel_idx,
                                      const uint8_t *sender_pub32,
                                      const uint8_t *data, uint16_t dlen) {
    if (!data || dlen == 0) return false;
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_packet_pki(mesh, sizeof(mesh), from, to, id,
                                           channel_idx, sender_pub32, data, dlen);
    bool ok = false;
    if (ml) {
        ok = mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

void lora_phoneapi_push_mesh_text_rx(uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     uint32_t channel_idx, const char *text,
                                     const pb_rx_meta_t *meta) {
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_text_rx(mesh, sizeof(mesh), from, to, id,
                                        hop_limit, hop_start, channel_idx,
                                        text, meta);
    if (ml) {
        (void)mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
}

bool lora_phoneapi_push_mesh_data_rx(uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     const uint8_t *data, uint16_t dlen,
                                     const pb_rx_meta_t *meta) {
    if (!data || dlen == 0) return false;
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_packet_rx(mesh, sizeof(mesh), from, to, id,
                                          channel_idx, hop_limit, hop_start,
                                          data, dlen, meta);
    bool ok = false;
    if (ml) {
        ok = mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool lora_phoneapi_push_mesh_data_pki_rx(uint32_t from, uint32_t to, uint32_t id,
                                         uint32_t channel_idx,
                                         uint32_t hop_limit, uint32_t hop_start,
                                         const uint8_t *sender_pub32,
                                         const uint8_t *data, uint16_t dlen,
                                         const pb_rx_meta_t *meta) {
    if (!data || dlen == 0) return false;
    portENTER_CRITICAL(&s_mux);
    uint8_t mesh[240];
    uint16_t ml = pb_build_mesh_packet_pki_rx(mesh, sizeof(mesh), from, to, id,
                                              channel_idx, hop_limit, hop_start,
                                              sender_pub32, data, dlen, meta);
    bool ok = false;
    if (ml) {
        ok = mesh_push_locked(mesh, ml);
    } else {
        s_dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool lora_phoneapi_push_mesh_node(uint32_t num, const char *id_str,
                                  const char *long_name, const char *short_name,
                                  float snr) {
    return lora_phoneapi_push_mesh_node_pki(num, id_str, long_name, short_name,
                                            snr, NULL);
}

bool lora_phoneapi_push_mesh_node_pki(uint32_t num, const char *id_str,
                                      const char *long_name, const char *short_name,
                                      float snr, const uint8_t *pub32) {
    portENTER_CRITICAL(&s_mux);
    uint8_t fr[LORA_PHONE_SLOT];
    uint16_t fl = pb_build_fromradio_nodeinfo_pki(fr, sizeof(fr), s_from_num + 1,
                                                  num, id_str, long_name,
                                                  short_name, snr, pub32);
    bool ok = false;
    if (fl && s_count < LORA_PHONE_FIFO_DEPTH) {
        fifo_push_locked(fr, fl);
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool lora_phoneapi_push_mesh_node_full(uint32_t num, const char *id_str,
                                       const char *long_name, const char *short_name,
                                       float snr, const uint8_t *pub32,
                                       bool verified, bool muted,
                                       bool hops_valid, uint8_t hops_away,
                                       uint32_t last_heard,
                                       uint32_t hw_model, uint32_t role) {
    portENTER_CRITICAL(&s_mux);
    uint8_t fr[LORA_PHONE_SLOT];
    bool fav = false, ign = false;
    peer_flag_get(num, &fav, &ign);
    uint16_t fl = pb_build_fromradio_nodeinfo_full_ex(fr, sizeof(fr), s_from_num + 1,
                                                   num, id_str, true, long_name,
                                                   short_name, snr, pub32,
                                                   verified, muted,
                                                    hops_valid, hops_away,
                                                    last_heard,
                                                    NULL, 0, NULL, 0, 0, false,
                                                    fav, ign, hw_model, role);
    bool ok = false;
    if (fl && s_count < LORA_PHONE_FIFO_DEPTH) {
        fifo_push_locked(fr, fl);
        ok = true;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

static bool push_queue_status(uint32_t mesh_packet_id) {
    portENTER_CRITICAL(&s_mux);
    status_push_locked(mesh_packet_id);
    status_drain_locked();
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool lora_phoneapi_has_data(void) { return s_count > 0; }

bool lora_phoneapi_config_active(void) {
    bool active;
    portENTER_CRITICAL(&s_mux);
    active = s_step != STEP_IDLE;
    portEXIT_CRITICAL(&s_mux);
    return active;
}

uint16_t lora_phoneapi_pop(uint8_t *out, uint16_t cap) {
    if (!out || cap == 0) return 0;
    portENTER_CRITICAL(&s_mux);
    if (s_count == 0) {
        portEXIT_CRITICAL(&s_mux);
        return 0;
    }
    uint16_t n = s_flen[s_head];
    if (n > cap) {
        portEXIT_CRITICAL(&s_mux);
        return 0; // caller buffer too small; keep queued
    }
    memcpy(out, s_fifo[s_head], n);
    s_head = (uint8_t)((s_head + 1) % LORA_PHONE_FIFO_DEPTH);
    s_count--;
    s_popped++;
    // Refill from the pending config sequence so slow readers still converge.
    if (s_step != STEP_IDLE) {
        portEXIT_CRITICAL(&s_mux);
        lora_phoneapi_pump();
        return n;
    }
    pending_drain_locked();
    portEXIT_CRITICAL(&s_mux);
    return n;
}

uint32_t lora_phoneapi_from_num(void) { return s_from_num; }

// Snapshot of our own identity for the config sequence (filled in
// begin_config outside the lock; pump only reads).
static uint32_t s_cfg_me = 0;
static char s_cfg_sh[8] = {0};
static char s_cfg_lo[24] = {0};
static char s_cfg_idstr[16] = {0};
static uint8_t s_cfg_region = 1;
static int s_cfg_tx = 17;
static int s_cfg_hop = 3;
static int s_cfg_role = 0;
static lora_modem_cfg_t s_cfg_modem;
static lora_channel_t s_cfg_ch[8];
static uint16_t s_cfg_nnodes = 0;

// Emit the next pending config step while the FIFO has room. Build + push
// happen under one critical section so FromRadio.id always matches the
// doorbell seq (gap detection on the app side depends on it).
void lora_phoneapi_pump(void) {
    uint8_t buf[LORA_PHONE_SLOT];
    lora_pend_flush(); // deliver any busy-queued radio request (no-op unless queued)
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        if (s_count >= LORA_PHONE_FIFO_DEPTH || s_step == STEP_IDLE) {
            if (s_step == STEP_IDLE) pending_drain_locked();
            portEXIT_CRITICAL(&s_mux);
            return;
        }
        uint32_t fn = s_from_num + 1;
        uint16_t n = 0;
        cfg_step_t step = s_step;
        if (step == STEP_MYINFO) {
            n = pb_build_fromradio_myinfo(buf, sizeof(buf), fn, s_cfg_me,
                                          s_reboots, (uint32_t)(s_cfg_nnodes + 1));
        } else if (step == STEP_SELF) {
            bool fav = false, ign = false;
            peer_flag_get(s_cfg_me, &fav, &ign);
            n = pb_build_fromradio_nodeinfo_full_ex(buf, sizeof(buf), fn, s_cfg_me,
                                                 s_cfg_idstr, true, s_cfg_lo, s_cfg_sh,
                                                 0.0f, lora_pki_public(),
                                                 false, false, false, 0, 0,
                                                  s_self_pos_len ? s_self_pos : NULL,
                                                  s_self_pos_len,
                                                   NULL, 0, 0, false, fav, ign,
                                                   lora_pb_local_hardware_model(),
                                                   (uint32_t)s_cfg_role);
        } else if (step == STEP_UIDATA) {
            n = pb_build_fromradio_uiconfig(buf, sizeof(buf), fn);
        } else if (step == STEP_CONFIG) {
            // Full 10-section walk like upstream (device..device_ui).
            // Sections report persisted values (store) so the app converges;
            // lora(6) reflects the live modem (or last ACKed values while the
            // deferred radio task converges).
            uint8_t sec = s_sub;
            if (sec < 1 || sec > 10) sec = 1;
            if (sec == 1) {
                n = pb_build_fromradio_config_device_full(buf, sizeof(buf), fn,
                                                          s_cfg_role, s_tzdef);
            } else if (sec == 6) {
                // Prefer the last ACKed values while the deferred radio task
                // is still converging (same snapshot as get_config[lora]).
                lora_modem_cfg_t pm = s_cfg_modem;
                int ptx = s_cfg_tx, phop = s_cfg_hop;
                uint8_t preg = s_cfg_region;
                if (lora_pend_fresh()) {
                    pm.use_preset = s_lora_pend.use_preset;
                    pm.preset = s_lora_pend.preset;
                    pm.sf = s_lora_pend.sf;
                    pm.bw_khz = s_lora_pend.bw;
                    pm.cr = s_lora_pend.cr;
                    pm.freq_offset_mhz = s_lora_pend.fo;
                    pm.override_freq_mhz = s_lora_pend.of;
                    pm.channel_num = s_lora_pend.chn;
                    pm.tx_enabled = s_lora_pend.te;
                    preg = (uint8_t)s_lora_pend.region;
                    phop = s_lora_pend.hop;
                    ptx = s_lora_pend.pwr;
                }
                int esf = 11, ebw = 250, ecr = 5;
                lora_modem_effective(&pm, &esf, &ebw, &ecr);
                // Upstream bandwidth field is a code; report kHz directly —
                // our tolerant parser accepts kHz/Hz/codes on the way back.
                n = pb_build_fromradio_config_lora_full_ex(buf, sizeof(buf), fn,
                    pm.use_preset, (uint32_t)pm.preset,
                    pm.use_preset ? 0 : (uint32_t)ebw,
                    pm.use_preset ? 0 : (uint32_t)esf,
                    (uint32_t)ecr,
                    pm.freq_offset_mhz, preg,
                    (uint32_t)phop, pm.tx_enabled,
                    ptx, pm.channel_num,
                    pm.override_freq_mhz, &s_lora_ex);
            } else {
                uint8_t raw[160];
                uint16_t rl = lora_store_cfg_get(sec, raw, sizeof(raw));
                n = pb_build_fromradio_config_raw(buf, sizeof(buf), fn, sec,
                                                 rl ? raw : NULL, rl);
            }
        } else if (step == STEP_MODULE) {
            // 17-module walk; persisted module blobs are reported verbatim.
            uint8_t mod = s_sub;
            if (mod < 1 || mod > 17) mod = 1;
            uint8_t raw[160];
            uint16_t rl = lora_store_mod_get(mod, raw, sizeof(raw));
            n = pb_build_fromradio_module_raw(buf, sizeof(buf), fn, mod,
                                              rl ? raw : NULL, rl);
        } else if (step == STEP_CHANNEL) {
            // All 8 slots with live PSK/name/role.
            uint8_t idx = s_sub;
            if (idx > 7) idx = 0;
            const lora_channel_t *ch = &s_cfg_ch[idx];
            if (!ch->used || ch->role == 0) {
                n = pb_build_fromradio_channel_idx(buf, sizeof(buf), fn,
                                                   idx, 0, false);
            } else {
                const uint8_t *mod = s_ch_mod_len[idx] ? s_ch_mod[idx] : NULL;
                n = pb_build_fromradio_channel_full_ex(buf, sizeof(buf), fn,
                    idx, ch->role, ch->name,
                    ch->psk_len ? ch->psk : NULL, ch->psk_len,
                    ch->uplink, ch->downlink, mod, s_ch_mod_len[idx]);
            }
        } else if (step == STEP_PEERS) {
            if (s_peer_idx >= s_peer_n) {
                s_step = STEP_COMPLETE;
                portEXIT_CRITICAL(&s_mux);
                continue;
            }
            lora_mesh_node_t nd;
            if (!lora_mesh_node_get(s_peer_ids[s_peer_idx], &nd)) {
                s_peer_idx++;
                portEXIT_CRITICAL(&s_mux);
                continue;
            }
            char pid[16], ps[8];
            node_id_str(pid, sizeof(pid), nd.node_num);
            snprintf(ps, sizeof(ps), "%s", nd.short_name);
            // Unknown nodes have no User submessage. Do not invent a User
            // short name; that caused blank/phantom contacts in native apps.
            if (!ps[0] && !nd.has_user)
                snprintf(ps, sizeof(ps), "%06X", (unsigned)(nd.node_num & 0xFFFFFF));
            uint32_t heard = lora_manager_peer_last_heard(nd.node_num);
            if (!heard) {
                time_t pnow = time(NULL);
                if (pnow >= 1577836800LL && (uint64_t)pnow <= UINT32_MAX) heard = (uint32_t)pnow;
            }
            bool fav = false, ign = false;
            peer_flag_get(nd.node_num, &fav, &ign);
            n = pb_build_fromradio_nodeinfo_full_ex(buf, sizeof(buf), fn,
                                                 nd.node_num, pid, nd.has_user,
                                                 nd.long_name, ps,
                                                 nd.last_snr,
                                                 nd.has_pubkey ? nd.pubkey : NULL,
                                                 nd.key_verified, nd.muted,
                                                  nd.hops_valid, nd.hops_away,
                                                  heard,
                                                  NULL, 0, NULL, 0, nd.channel, false,
                                                  fav, ign, nd.hw_model, nd.role);
            if (n) {
                fifo_push_locked(buf, n);
                s_peer_idx++;
            }
            portEXIT_CRITICAL(&s_mux);
            if (!n) return; // odd build failure; retry on next poll
            continue;
        } else if (step == STEP_META) {
            n = pb_build_fromradio_metadata_full(buf, sizeof(buf), fn, MESHTASTIC_COMPAT_FW,
                                                 s_cfg_role);
        } else if (step == STEP_REGION) {
            n = pb_build_fromradio_region(buf, sizeof(buf), fn);
        } else if (step == STEP_COMPLETE) {
            n = pb_build_fromradio_complete(buf, sizeof(buf), fn, s_nonce);
        }
        if (n == 0 && step != STEP_PEERS) {
            portEXIT_CRITICAL(&s_mux);
            return;
        }
        if (step != STEP_PEERS) {
            fifo_push_locked(buf, n);
            if (s_step == step) {
                // Sub-indexed steps walk their table before advancing.
                if (step == STEP_CHANNEL) {
                    if (s_sub >= 7) {
                        s_step = STEP_CONFIG;
                        s_sub = 1;
                    } else {
                        s_sub++;
                    }
                } else if (step == STEP_CONFIG) {
                    if (s_sub >= 10) {
                        s_step = STEP_MODULE;
                        s_sub = 1;
                    } else {
                        s_sub++;
                    }
                } else if (step == STEP_MODULE) {
                    if (s_sub >= 17) {
                        s_step = (s_nonce == SPECIAL_NONCE_ONLY_CONFIG)
                                     ? STEP_COMPLETE
                                     : STEP_PEERS;
                        s_sub = 0;
                    } else {
                        s_sub++;
                    }
                } else if (step == STEP_META) {
                    s_step = STEP_REGION;
                } else if (step == STEP_REGION) {
                    s_step = STEP_CHANNEL;
                    s_sub = 0;
                } else if (step == STEP_SELF &&
                           s_nonce == SPECIAL_NONCE_ONLY_NODES) {
                    s_step = STEP_PEERS;
                } else if (step == STEP_COMPLETE) {
                    s_step = STEP_IDLE;
                } else {
                    s_step = (cfg_step_t)(step + 1);
                }
            }
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

void lora_phoneapi_begin_config(uint32_t nonce) {
    // Snapshot everything the sequence needs (no locks held here).
    s_cfg_me = lora_mesh_node_num();
    short_name(s_cfg_sh, sizeof(s_cfg_sh));
    long_name(s_cfg_lo, sizeof(s_cfg_lo));
    node_id_str(s_cfg_idstr, sizeof(s_cfg_idstr), s_cfg_me);
    s_cfg_region = lora_region_pb();
    {
        lora_status_t st;
        memset(&st, 0, sizeof(st));
        lora_manager_get_status(&st);
        s_cfg_tx = st.tx_dbm;
        s_cfg_hop = st.hop_limit;
        s_cfg_role = st.role;
    }
    lora_manager_get_modem_cfg(&s_cfg_modem.use_preset, &s_cfg_modem.preset,
                               &s_cfg_modem.sf, &s_cfg_modem.bw_khz, &s_cfg_modem.cr,
                               &s_cfg_modem.freq_offset_mhz, &s_cfg_modem.override_freq_mhz,
                               &s_cfg_modem.channel_num, &s_cfg_modem.tx_enabled);
    for (uint8_t i = 0; i < 8; i++) {
        const lora_channel_t *c = lora_channel_get(i);
        if (c) memcpy(&s_cfg_ch[i], c, sizeof(s_cfg_ch[i]));
        else memset(&s_cfg_ch[i], 0, sizeof(s_cfg_ch[i]));
    }
    s_cfg_nnodes = lora_manager_node_count();
    s_peer_n = lora_mesh_node_ids(s_peer_ids, LORA_MESH_NODES_MAX);
    s_nonce = nonce;
    // Reference two-stage flow used by current clients:
    // 69420: my_info, uiconfig, own node, metadata, region presets,
    //        channels, configs, modules, complete (no peer DB).
    // 69421: own node, peer DB, complete.
    // Other non-zero nonces retain the legacy full walk.
    // State swap is locked; pump() emits lazily per FIFO space.
    portENTER_CRITICAL(&s_mux);
    fifo_requeue_service_locked();
    s_head = 0;
    s_count = 0;
    s_peer_idx = 0;
    s_sub = 0;
    s_step = (nonce == SPECIAL_NONCE_ONLY_NODES) ? STEP_SELF : STEP_MYINFO;
    portEXIT_CRITICAL(&s_mux);
    lora_phoneapi_pump();
    // A stock node only announces on boot and then on a long interval. Make a
    // fresh phone session perform the same discovery request immediately so a
    // node powered on after Ghost still appears in the app.
    lora_mesh_request_nodeinfo();
    ESP_LOGI(TAG, "config sequence started (nonce=%u, peers=%u)", (unsigned)nonce, (unsigned)s_peer_n);
}

bool lora_phoneapi_handle_admin(const toradio_t *t, uint32_t me, uint32_t dst) {
    if (!t) return false;
    bool queued = false;
    uint8_t err = 0;
    uint8_t af = (uint8_t)t->admin_field;
    // Unwrap AdminMessage bytes (Data field 2); fall back to payload_head.
    const uint8_t *adm = t->payload_head;
    uint16_t adml = t->payload_len;
    (void)admin_msg(t, &adm, &adml);
    // --- GETs ---
    if (af == 1) { // get_channel_request: uint32 index+1
        uint32_t v = 0;
        find_varint(adm, adml, 1, &v);
        uint8_t idx = v > 0 ? (uint8_t)(v - 1) : 0;
        if (idx >= 8) err = 32;
        else {
            const lora_channel_t *ch = lora_channel_get(idx);
            uint8_t inner[112];
            pb_w_t w;
            pb_w_init(&w, inner, sizeof(inner));
            if (idx != 0) pb_w_varint(&w, 1, idx);
            if (ch && ch->used && ch->role != 0) {
                uint8_t st[80];
                pb_w_t s;
                pb_w_init(&s, st, sizeof(st));
                if (ch->psk_len) pb_w_bytes(&s, 2, ch->psk, ch->psk_len);
                pb_w_string(&s, 3, ch->name);
                if (ch->id) pb_w_fixed32(&s, 4, ch->id);
                if (ch->uplink) pb_w_varint(&s, 5, 1);
                if (ch->downlink) pb_w_varint(&s, 6, 1);
                if (s_ch_mod_len[idx]) pb_w_bytes(&s, 7, s_ch_mod[idx], s_ch_mod_len[idx]);
                if (!s.overflow) pb_w_msg(&w, 2, st, s.len);
            }
            pb_w_varint(&w, 3, ch ? ch->role : 0);
            if (!w.overflow && admin_reply(me, dst, t->msg_id, 2, inner, w.len))
                queued = true;
            else err = 32;
        }
    } else if (af == 3) { // get_owner_request
        char lo[40], sh[8], idstr[16];
        lora_mesh_owner(lo, sizeof(lo), sh, sizeof(sh));
        snprintf(idstr, sizeof(idstr), "!%08x", (unsigned)lora_mesh_node_num());
        uint8_t user[144];
        pb_w_t u;
        pb_w_init(&u, user, sizeof(user));
        pb_w_string(&u, 1, idstr);
        pb_w_string(&u, 2, lo);
        pb_w_string(&u, 3, sh);
        pb_w_varint(&u, 5, lora_pb_local_hardware_model());
        {
            // User.role=7 from the live device role (CLIENT=0 omitted).
            lora_status_t ost;
            memset(&ost, 0, sizeof(ost));
            lora_manager_get_status(&ost);
            if (ost.role > 0) pb_w_varint(&u, 7, (uint32_t)ost.role);
        }
        const uint8_t *pub = lora_pki_public();
        pb_w_bytes(&u, 8, pub, pub ? 32 : 0);
        if (!u.overflow && admin_reply(me, dst, t->msg_id, 4, user, u.len))
            queued = true;
        else err = 32;
    } else if (af == 5) { // get_config_request: ConfigType enum 0..9
        // (admin.proto). Config oneof fields are 1..10, so section = enum+1.
        // Out-of-range enums get ACK-only (no response payload).
        uint32_t e = 0;
        find_varint(adm, adml, 5, &e);
        if (e > 9) {
            // ACK-only below.
        } else {
        uint32_t sec = e + 1;
        if (sec == 6) {
            lora_status_t cst;
            memset(&cst, 0, sizeof(cst));
            lora_manager_get_status(&cst);
            uint8_t lora[96];
            pb_w_t l;
            pb_w_init(&l, lora, sizeof(lora));
            bool up = false;
            int pr = 0, sf = 0, bw = 0, cr = 0;
            float fo = 0, of = 0;
            uint32_t cn = 0;
            bool te = true;
            lora_manager_get_modem_cfg(&up, &pr, &sf, &bw, &cr, &fo, &of, &cn, &te);
            // Report the last ACKed values while the deferred radio task is
            // still converging so an immediate read-back isn't stale.
            lora_pend_flush();
            if (lora_pend_fresh()) {
                up = s_lora_pend.use_preset;
                pr = s_lora_pend.preset;
                sf = s_lora_pend.esf;
                bw = s_lora_pend.ebw;
                cr = s_lora_pend.ecr;
                fo = s_lora_pend.fo;
                of = s_lora_pend.of;
                cn = s_lora_pend.chn;
                te = s_lora_pend.te;
                cst.region = s_lora_pend.region;
                cst.hop_limit = s_lora_pend.hop;
                cst.tx_dbm = s_lora_pend.pwr;
            }
            if (up) pb_w_varint(&l, 1, 1);
            if (up) pb_w_varint(&l, 2, (uint32_t)pr);
            else {
                if (bw) pb_w_varint(&l, 3, (uint32_t)bw);
                if (sf) pb_w_varint(&l, 4, (uint32_t)sf);
                if (cr) pb_w_varint(&l, 5, (uint32_t)cr);
            }
            if (up && cr >= 5 && cr <= 8) pb_w_varint(&l, 5, (uint32_t)cr);
            if (fo != 0.0f) pb_w_float(&l, 6, fo);
            pb_w_varint(&l, 7, (uint32_t)cst.region);
            pb_w_varint(&l, 8, (uint32_t)cst.hop_limit);
            if (te) pb_w_varint(&l, 9, 1);
            if (cst.tx_dbm > 0) pb_w_varint(&l, 10, (uint32_t)cst.tx_dbm);
            if (cn) pb_w_varint(&l, 11, cn);
            if (of != 0.0f) pb_w_float(&l, 14, of);
            pb_w_lora_extras(&l, &s_lora_ex); // preserved 12/13/15/103..107
            uint8_t cfg[128];
            pb_w_t w;
            pb_w_init(&w, cfg, sizeof(cfg));
            pb_w_msg(&w, 6, lora, l.len);
            if (!w.overflow && !l.overflow &&
                admin_reply(me, dst, t->msg_id, 6, cfg, w.len))
                queued = true;
            else err = 32;
        } else if (sec == 1) {
            // Device section: stored blob is the base, but role/tzdef live
            // in the manager/phone layer — overlay the live values so a
            // role change (or tzdef push) isn't shadowed by a stale store
            // on read-back. Same live source as the config-walk builder.
            uint8_t raw[160];
            uint16_t rl = lora_store_cfg_get((uint8_t)sec, raw, sizeof(raw));
            lora_status_t cst;
            memset(&cst, 0, sizeof(cst));
            lora_manager_get_status(&cst);
            uint8_t dev[208];
            pb_w_t d;
            pb_w_init(&d, dev, sizeof(dev));
            {
                pb_r_t r;
                pb_r_init(&r, raw, rl);
                uint8_t f, wr;
                uint32_t v;
                const uint8_t *b;
                uint16_t bl;
                while (pb_r_next(&r, &f, &wr, &v, &b, &bl)) {
                    if (f == 1 || f == 11) continue; // role/tzdef: live below
                    if (wr == 0) pb_w_varint(&d, f, v);
                    else if (wr == 5) pb_w_fixed32(&d, f, v);
                    else if (wr == 2) pb_w_bytes(&d, f, b, bl);
                }
            }
            pb_w_varint(&d, 1, (uint32_t)cst.role);
            pb_w_string(&d, 11, s_tzdef);
            uint8_t cfg[216];
            pb_w_t w;
            pb_w_init(&w, cfg, sizeof(cfg));
            pb_w_msg(&w, 1, dev, d.len);
            if (!w.overflow && !d.overflow &&
                admin_reply(me, dst, t->msg_id, 6, cfg, w.len))
                queued = true;
            else err = 32;
        } else {
            uint8_t raw[160];
            uint16_t rl = lora_store_cfg_get((uint8_t)sec, raw, sizeof(raw));
            uint8_t cfg[170];
            pb_w_t w;
            pb_w_init(&w, cfg, sizeof(cfg));
            if (sec == 8) {
                // SecurityConfig: live public key + stored admin keys/policy.
                uint8_t se[160];
                pb_w_t s;
                pb_w_init(&s, se, sizeof(se));
                const uint8_t *pub = lora_pki_public();
                pb_w_bytes(&s, 1, pub, pub ? 32 : 0);
                // Carry stored admin_key/is_managed/etc through.
                pb_r_t r;
                pb_r_init(&r, raw, rl);
                uint8_t f, wr;
                uint32_t v;
                const uint8_t *b;
                uint16_t bl;
                while (pb_r_next(&r, &f, &wr, &v, &b, &bl)) {
                    if (f == 1 || f == 2) continue; // keys live above
                    if (wr == 0) pb_w_varint(&s, f, v);
                    else if (wr == 5) pb_w_fixed32(&s, f, v);
                    else if (wr == 2) pb_w_bytes(&s, f, b, bl);
                }
                if (!s.overflow) pb_w_msg(&w, (uint8_t)sec, se, s.len);
                else pb_w_msg(&w, (uint8_t)sec, raw, rl);
            } else {
                pb_w_msg(&w, (uint8_t)sec, raw, rl);
            }
            if (!w.overflow && admin_reply(me, dst, t->msg_id, 6, cfg, w.len))
                queued = true;
            else err = 32;
        }
        } // else (e <= 9): out-of-range falls through to ACK-only
    } else if (af == 7) { // get_module_config_request: ModuleConfigType 0..16
        // ModuleConfig oneof fields are 1..17, so module = enum+1.
        // Out-of-range enums get ACK-only (no response payload).
        uint32_t e = 0;
        find_varint(adm, adml, 7, &e);
        if (e <= 16) {
        uint32_t m = e + 1;
        uint8_t raw[160];
        uint16_t rl = lora_store_mod_get((uint8_t)m, raw, sizeof(raw));
        uint8_t mc[170];
        pb_w_t w;
        pb_w_init(&w, mc, sizeof(mc));
        pb_w_msg(&w, (uint8_t)m, raw, rl);
        if (!w.overflow && admin_reply(me, dst, t->msg_id, 8, mc, w.len))
            queued = true;
        else err = 32;
        } // else (e > 16): ACK-only
    } else if (af == 12) { // get_device_metadata_request -> DeviceMetadata
        lora_status_t mst;
        memset(&mst, 0, sizeof(mst));
        lora_manager_get_status(&mst);
        uint8_t md[80];
        pb_w_t w;
        pb_w_init(&w, md, sizeof(md));
        pb_w_string(&w, 1, MESHTASTIC_COMPAT_FW);
        pb_w_varint(&w, 2, 25);
        pb_w_varint(&w, 4, 1); // hasWifi
        pb_w_varint(&w, 5, 1); // hasBluetooth
        if (mst.role > 0) pb_w_varint(&w, 7, (uint32_t)mst.role);
        pb_w_varint(&w, 9, lora_pb_local_hardware_model());
        pb_w_varint(&w, 11, 1); // hasPKC
        if (!w.overflow && admin_reply(me, dst, t->msg_id, 13, md, w.len))
            queued = true;
        else err = 32;
    } else if (af == 16) { // get_device_connection_status_request
        // Minimal DeviceConnectionStatus (connection_status.proto): BLE link
        // to the phone is up, WiFi is not. No battery/WiFi rssi on this hw.
        uint8_t wifi_net[8];
        pb_w_t wn;
        pb_w_init(&wn, wifi_net, sizeof(wifi_net));
        pb_w_varint(&wn, 2, 0); // is_connected=false (omitted anyway)
        uint8_t wifi[16];
        pb_w_t ww;
        pb_w_init(&ww, wifi, sizeof(wifi));
        pb_w_msg(&ww, 1, wifi_net, wn.len);
        uint8_t bt[8];
        pb_w_t wb;
        pb_w_init(&wb, bt, sizeof(bt));
        pb_w_varint(&wb, 3, 1); // bluetooth is_connected (this phone link)
        uint8_t cs[40];
        pb_w_t w;
        pb_w_init(&w, cs, sizeof(cs));
        pb_w_msg(&w, 1, wifi, ww.len);
        pb_w_msg(&w, 3, bt, wb.len);
        if (!w.overflow && !ww.overflow && !wb.overflow &&
            admin_reply(me, dst, t->msg_id, 17, cs, w.len))
            queued = true;
        else err = 32;
    } else if (af == 39 || af == 40) { // set/remove_favorite_node
        uint32_t nn = 0;
        if (!find_varint(adm, adml, af, &nn) || nn < 4) err = 32;
        else {
            peer_flag_t *pf = peer_flags(nn, true);
            if (!pf) err = 32;
            else {
                pf->favorite = (af == 39);
                lora_mesh_peer_set_favorite(nn, af == 39);
                peer_flags_save();
            }
        }
    } else if (af == 47 || af == 48) { // set/remove_ignored_node
        uint32_t nn = 0;
        if (!find_varint(adm, adml, af, &nn) || nn < 4) err = 32;
        else {
            peer_flag_t *pf = peer_flags(nn, true);
            if (!pf) err = 32;
            else {
                pf->ignored = (af == 47);
                lora_manager_note_ignored(nn, af == 47);
                lora_mesh_peer_set_ignored(nn, af == 47);
                peer_flags_save();
            }
        }
    } else if (af == 10 || af == 14) { // get canned(10)/ringtone(14)
        // GetCannedMessageModuleMessagesResponse(11){string messages=1} /
        // GetRingtoneResponse(15){string ringtone=1}: report the last
        // set_canned(36)/set_ringtone(37) value; empty store -> empty
        // response (same wire shape as before).
        const char *src = (af == 10) ? s_canned : s_ringtone;
        uint8_t inner[200];
        pb_w_t cw;
        pb_w_init(&cw, inner, sizeof(inner));
        if (src[0]) pb_w_string(&cw, 1, src);
        if (!cw.overflow &&
            admin_reply(me, dst, t->msg_id, af == 10 ? 11 : 15,
                        cw.len ? inner : NULL, cw.len))
            queued = true;
        else if (cw.overflow) err = 32;
    // --- SETs ---
    } else if (af == 32) { // set_owner
        const uint8_t *user, *value;
        uint16_t user_len, value_len;
        char lo[40], sh[8];
        lora_mesh_owner(lo, sizeof(lo), sh, sizeof(sh));
        if (!find_submsg(adm, adml, 32, &user, &user_len)) err = 32;
        else {
            if (find_submsg(user, user_len, 2, &value, &value_len)) {
                if (value_len >= sizeof(lo)) err = 32;
                else { memcpy(lo, value, value_len); lo[value_len] = 0; }
            }
            if (!err && find_submsg(user, user_len, 3, &value, &value_len)) {
                if (value_len >= sizeof(sh)) err = 32;
                else { memcpy(sh, value, value_len); sh[value_len] = 0; }
            }
            if (!err && !lora_mesh_set_owner(lo, sh)) err = 32;
            // Announce immediately: periodic NodeInfo is 3h, so without
            // this the app + stock nodes show the old name until then.
            if (!err) lora_mesh_request_nodeinfo();
        }
    } else if (af == 33) { // set_channel
        const uint8_t *chb;
        uint16_t chl;
        if (!find_submsg(adm, adml, 33, &chb, &chl)) err = 32;
        else {
            pb_r_t r;
            pb_r_init(&r, chb, chl);
            uint8_t f, w;
            uint32_t v;
            const uint8_t *b;
            uint16_t bl;
            uint32_t idx = 0, role = 1;
            const uint8_t *st = NULL;
            uint16_t stl = 0;
            while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
                if (f == 1 && w == 0) idx = v;
                else if (f == 2 && w == 2) { st = b; stl = bl; }
                else if (f == 3 && w == 0) role = v;
            }
            if (idx >= 8 || role > 2) err = 32;
            else if (!st || stl == 0) {
                if (role == 0) {
                    if (!lora_manager_disable_channel((uint8_t)idx)) err = 32;
                    else s_ch_mod_len[idx] = 0;
                } else err = 32;
            } else {
                pb_r_t s;
                pb_r_init(&s, st, stl);
                const uint8_t *psk = NULL, *nm = NULL, *mod = NULL;
                uint16_t pskl_in = 0, nml = 0, modl = 0;
                uint32_t id = 0;
                bool up = false, dn = false;
                while (pb_r_next(&s, &f, &w, &v, &b, &bl)) {
                    if (f == 2 && w == 2) { psk = b; pskl_in = bl; }
                    else if (f == 3 && w == 2) { nm = b; nml = bl; }
                    else if (f == 4 && w == 5) id = v;
                    else if (f == 5 && w == 0) up = v != 0;
                    else if (f == 6 && w == 0) dn = v != 0;
                    else if (f == 7 && w == 2) { mod = b; modl = bl; }
                }
                char name[32] = {0};
                if (nm && nml && nml < sizeof(name)) { memcpy(name, nm, nml); name[nml] = 0; }
                uint8_t pskb[32];
                uint8_t pskl = 0;
                if (psk && pskl_in) {
                    if (pskl_in > 32) err = 32;
                    else { memcpy(pskb, psk, pskl_in); pskl = (uint8_t)pskl_in; }
                }
                if (!err) {
                    if (!lora_manager_set_channel((uint8_t)idx, nm ? name : NULL,
                                                 psk ? pskb : NULL, pskl,
                                                 (uint8_t)role, up, dn))
                        err = 32;
                    else if (id) lora_channel_set_id((uint8_t)idx, id);
                    // ChannelSettings.module_settings=7 opaque passthrough.
                    if (!err) {
                        if (mod && modl && modl <= LORA_CH_MOD_MAX) {
                            memcpy(s_ch_mod[idx], mod, modl);
                            s_ch_mod_len[idx] = (uint8_t)modl;
                        } else if (!mod || !modl) {
                            s_ch_mod_len[idx] = 0;
                        } else err = 32;
                    }
                }
            }
        }
    } else if (af == 34) { // set_config
        const uint8_t *cfg;
        uint16_t cfgl;
        if (!find_submsg(adm, adml, 34, &cfg, &cfgl)) err = 32;
        else {
            // Walk Config sections present in this message.
            pb_r_t r;
            pb_r_init(&r, cfg, cfgl);
            uint8_t f, w;
            uint32_t v;
            const uint8_t *b;
            uint16_t bl;
            bool saw_lora = false, saw_device = false, saw_sec = false, role_changed = false;
            const uint8_t *lora_b = NULL;
            uint16_t lora_l = 0;
            const uint8_t *dev_b = NULL;
            uint16_t dev_l = 0;
            const uint8_t *sec_b = NULL;
            uint16_t sec_l = 0;
            // Collect raw sections for store.
            struct { uint8_t sec; const uint8_t *b; uint16_t l; } raws[10];
            uint8_t nraw = 0;
            while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
                if (w != 2 || f < 1 || f > 10) continue;
                if (f == 6) { saw_lora = true; lora_b = b; lora_l = bl; }
                else if (f == 1) { saw_device = true; dev_b = b; dev_l = bl; }
                else if (f == 8) { saw_sec = true; sec_b = b; sec_l = bl; }
                else if (nraw < 10) { raws[nraw].sec = f; raws[nraw].b = b; raws[nraw].l = bl; nraw++; }
            }
            if (saw_device && dev_b) {
                uint32_t role = 0;
                bool has_role = find_varint(dev_b, dev_l, 1, &role);
                const uint8_t *tz = NULL;
                uint16_t tzl = 0;
                if (find_submsg(dev_b, dev_l, 11, &tz, &tzl) && tzl > 0 && tzl < sizeof(s_tzdef)) {
                    char tmp[40];
                    memcpy(tmp, tz, tzl);
                    tmp[tzl] = '\0';
                    tzdef_save(tmp);
                }
                if (has_role) {
                    if (role > 12) err = 32;
                    else if (lora_manager_set_role((int)role)) role_changed = true;
                    else err = 32;
                }
                if (!err) lora_store_cfg_set(1, dev_b, dev_l);
            }
            for (uint8_t i = 0; i < nraw && !err; i++) {
                if (!lora_store_cfg_set(raws[i].sec, raws[i].b, raws[i].l)) err = 32;
            }
            if (!err && saw_sec && sec_b) {
                // SecurityConfig: private_key installs our identity,
                // admin_key allowlist is stored; rest persisted verbatim.
                pb_r_t sr;
                pb_r_init(&sr, sec_b, sec_l);
                uint8_t sf2, sw2;
                uint32_t sv2;
                const uint8_t *sb2;
                uint16_t sl2;
                uint8_t admins[3][32];
                uint8_t nadmin = 0;
                while (pb_r_next(&sr, &sf2, &sw2, &sv2, &sb2, &sl2)) {
                    if (sf2 == 2 && sw2 == 2 && sl2 == 32) {
                        if (!lora_pki_set_private(sb2)) err = 32;
                    } else if (sf2 == 3 && sw2 == 2 && sl2 == 32 && nadmin < 3) {
                        memcpy(admins[nadmin++], sb2, 32);
                    }
                }
                if (!err && !lora_store_cfg_set(8, sec_b, sec_l)) err = 32;
                if (!err && nadmin && !lora_pki_admin_set(&admins[0][0], nadmin))
                    err = 32;
            }
            if (!err && saw_lora && lora_b) {
                pb_r_t lr;
                pb_r_init(&lr, lora_b, lora_l);
                bool use_preset = true, has_use_preset = false;
                bool has_preset = false, has_sf = false, has_bw = false, has_cr = false;
                bool has_fo = false, has_of = false, has_chn = false, has_te = false, has_hop = false, has_pwr = false, has_region = false;
                int preset = 0, sf = 0, bw = 0, cr = 0, hop = 3, pwr = 17, region = 1;
                float fo = 0, of = 0;
                uint32_t chn = 0;
                bool te = true;
                while (pb_r_next(&lr, &f, &w, &v, &b, &bl)) {
                    if (f == 1 && w == 0) { use_preset = v != 0; has_use_preset = true; }
                    else if (f == 2 && w == 0) { preset = (int)v; has_preset = true; }
                    else if (f == 3 && w == 0) { int k = bw_to_khz(v); if (k < 0) err = 32; else { bw = k; has_bw = true; } }
                    else if (f == 4 && w == 0) { sf = (int)v; has_sf = true; }
                    else if (f == 5 && w == 0) { cr = (int)v; has_cr = true; }
                    else if (f == 6 && w == 5) { memcpy(&fo, &v, 4); has_fo = true; }
                    else if (f == 7 && w == 0) { region = (int)v; has_region = true; }
                    else if (f == 8 && w == 0) { hop = (int)v; has_hop = true; }
                    else if (f == 9 && w == 0) { te = v != 0; has_te = true; }
                    else if (f == 10 && w == 0) { pwr = (int)v; has_pwr = true; }
                    else if (f == 11 && w == 0) { chn = v; has_chn = true; }
                    else if (f == 14 && w == 5) { memcpy(&of, &v, 4); has_of = true; }
                }
                if (!err) {
                    // Fill unset fields from current state (app sends deltas).
                    lora_status_t cur;
                    memset(&cur, 0, sizeof(cur));
                    lora_manager_get_status(&cur);
                    bool cup;
                    int cpr, csf, cbw, ccr;
                    float cfo, cof;
                    uint32_t ccn;
                    bool cte;
                    lora_manager_get_modem_cfg(&cup, &cpr, &csf, &cbw, &ccr, &cfo, &cof, &ccn, &cte);
                    if (!has_use_preset) use_preset = cup;
                    if (!has_preset) preset = cpr;
                    if (!has_sf) sf = csf;
                    if (!has_bw) bw = cbw;
                    if (!has_cr) cr = ccr;
                    if (!has_fo) fo = cfo;
                    if (!has_of) of = cof;
                    if (!has_chn) chn = ccn;
                    if (!has_te) te = cte;
                    if (!has_hop) hop = cur.hop_limit;
                    if (!has_pwr) pwr = cur.tx_dbm;
                    if (!has_region) region = cur.region;
                    if (region <= 0 || lora_region_name(region)[0] == '?') err = 32;
                    else {
                        // Clamp TX to the regional/board ceiling instead of
                        // BAD_REQUEST on over-max (upstream clamps via
                        // RegionInfo powerLimit); 0 = board max.
                        {
                            int pwr_in = pwr;
                            pwr = lora_modem_clamp_tx(region, pwr, false);
                            lora_hw_t hw;
                            if (lora_manager_get_hw(&hw)) {
                                if (pwr == 0) pwr = hw.max_tx_dbm;
                                if (pwr > hw.max_tx_dbm) pwr = hw.max_tx_dbm;
                            }
                            if (pwr != pwr_in)
                                ESP_LOGI(TAG, "set_config[lora]: tx_power %d clamped to %d",
                                         pwr_in, pwr);
                        }
                        // Deferred apply keeps the radio up; a fully-valid
                        // request refused here only means the manager defer
                        // slot is busy -> queue latest (single-slot replace)
                        // and ACK OK instead of a spurious BAD_REQUEST.
                        bool need_queue = false;
                        if (!lora_manager_apply_lora_cfg((lora_region_t)region, use_preset, preset,
                                                         sf, bw, cr, fo, of, chn, te, pwr, hop)) {
                            if (lora_cfg_locally_valid((lora_region_t)region, use_preset, preset,
                                                       sf, bw, cr, fo, of, chn, pwr, hop)) {
                                need_queue = true;
                                ESP_LOGI(TAG, "set_config[lora]: manager busy, queued latest");
                            } else {
                                err = 32;
                            }
                        }
                        if (!err) {
                            // Snapshot for read-back (live state lags the
                            // deferred task ~1s); retry=true marks the
                            // busy-queued slot for opportunistic flush.
                            lora_pend_save((lora_region_t)region, use_preset, preset,
                                           sf, bw, cr, fo, of, chn, te, pwr, hop,
                                           need_queue);
                            if (!lora_manager_set_hop_limit(hop)) err = 32;
                        }
                        if (!err) {
                            // Preserve modem-unknown LoRaConfig fields (12/13/15/
                            // 103..107) with presence merge: the app sends deltas,
                            // so only fields actually on the wire update the mirror.
                            pb_lora_extras_t tmp;
                            uint32_t pv = 0;
                            pb_parse_lora_extras(lora_b, lora_l, &tmp);
                            if (find_varint(lora_b, lora_l, 12, &pv))
                                s_lora_ex.override_duty_cycle = tmp.override_duty_cycle;
                            if (find_varint(lora_b, lora_l, 13, &pv))
                                s_lora_ex.sx126x_rx_boosted_gain = tmp.sx126x_rx_boosted_gain;
                            if (find_varint(lora_b, lora_l, 15, &pv))
                                s_lora_ex.pa_fan_disabled = tmp.pa_fan_disabled;
                            if (find_varint(lora_b, lora_l, 103, &pv)) {
                                s_lora_ex.n_ignore_incoming = tmp.n_ignore_incoming;
                                for (uint8_t k = 0; k < tmp.n_ignore_incoming; k++)
                                    s_lora_ex.ignore_incoming[k] = tmp.ignore_incoming[k];
                            }
                            if (find_varint(lora_b, lora_l, 104, &pv))
                                s_lora_ex.ignore_mqtt = tmp.ignore_mqtt;
                            if (find_varint(lora_b, lora_l, 105, &pv))
                                s_lora_ex.config_ok_to_mqtt = tmp.config_ok_to_mqtt;
                            if (find_varint(lora_b, lora_l, 106, &pv)) {
                                s_lora_ex.has_fem_lna_mode = true;
                                s_lora_ex.fem_lna_mode = tmp.fem_lna_mode;
                            }
                            if (find_varint(lora_b, lora_l, 107, &pv))
                                s_lora_ex.serial_hal_only = tmp.serial_hal_only;
                        } // if (!err): extras merge for applied + queued
                    } // else (region ok)
                }
            }
            if (!err && !saw_lora && !saw_device && !saw_sec && nraw == 0) err = 32;
            // A role change alters our air NodeInfo: announce immediately so
            // peers converge (same as set_owner).
            if (!err && role_changed) lora_mesh_request_nodeinfo();
        }
    } else if (af == 35) { // set_module_config
        const uint8_t *mc;
        uint16_t mcl;
        if (!find_submsg(adm, adml, 35, &mc, &mcl)) err = 32;
        else {
            pb_r_t r;
            pb_r_init(&r, mc, mcl);
            uint8_t f, w;
            uint32_t v;
            const uint8_t *b;
            uint16_t bl;
            bool saw = false;
            while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
                if (w == 2 && f >= 1 && f <= 17) {
                    if (!lora_store_mod_set(f, b, bl)) err = 32;
                    else if (f == 1 && b && bl) {
                        // MQTTConfig.map_report_settings=11.position_precision=2
                        // (bits, 32 = full): live-apply so originated
                        // positions mask immediately. Stored regardless;
                        // parse is best-effort (setter ignores other values).
                        const uint8_t *mr = NULL;
                        uint16_t mrl = 0;
                        uint32_t prec = 0;
                        if (find_submsg(b, bl, 11, &mr, &mrl) &&
                            find_varint(mr, mrl, 2, &prec) && prec != 0) {
                            lora_mesh_set_position_precision((int)prec);
                            ESP_LOGI(TAG, "map_report position_precision=%u applied",
                                     (unsigned)prec);
                        }
                    }
                    saw = true;
                }
            }
            if (!saw) err = 32;
        }
    } else if (af == 42) { // remove_fixed_position
        // Clear our advertised fixed position everywhere (phone copy + air
        // NodeInfo source) and ask for an immediate rebroadcast so peers
        // converge instead of holding the stale map pin.
        memset(s_self_pos, 0, sizeof(s_self_pos));
        s_self_pos_len = 0;
        lora_mesh_set_self_position(NULL, 0);
        lora_mesh_request_nodeinfo();
        ESP_LOGI(TAG, "fixed position cleared");
    } else if (af == 43 || af == 41) { // set_time_only / set_fixed_position
        if (af == 41) {
            // Keep the raw Position submessage so our NodeInfo.position=3
            // lets the app map show us (capped; time sync handled below).
            const uint8_t *pos = NULL;
            uint16_t posl = 0;
            if (find_submsg(adm, adml, 41, &pos, &posl) &&
                pos && posl) {
                uint16_t c = posl < sizeof(s_self_pos) ? posl : sizeof(s_self_pos);
                memcpy(s_self_pos, pos, c);
                s_self_pos_len = (uint8_t)c;
                // Advertise on air too: our NodeInfo.position source reads
                // the mesh copy (mesh caps length internally).
                lora_mesh_set_self_position(pos, posl);
                // Position may carry fixed32 time fields; scan them.
                pb_r_t ir;
                pb_r_init(&ir, pos, posl);
                uint8_t f2, w2;
                uint32_t v2;
                const uint8_t *b2;
                uint16_t bl2;
                while (pb_r_next(&ir, &f2, &w2, &v2, &b2, &bl2)) {
                    if (w2 == 5 && v2 >= 1577836800U) {
                        struct timeval now = {.tv_sec = v2, .tv_usec = 0};
                        settimeofday(&now, NULL);
                        ESP_LOGI(TAG, "time sync %u (NodeInfo last_heard now live)", (unsigned)v2);
                    }
                }
            }
        }
        if (af == 43) {
            // AdminMessage field 43 is fixed32 wire 5 (tag DD 02).
            uint32_t tv = 0;
            if (find_fixed32(adm, adml, 43, &tv)) {
                if (tv >= 1577836800U) {
                    struct timeval now = {.tv_sec = tv, .tv_usec = 0};
                    settimeofday(&now, NULL);
                    ESP_LOGI(TAG, "time sync %u (NodeInfo last_heard now live)", (unsigned)tv);
                }
            } else {
                // Fallback: LEN-wrapped time inside submessage 43.
                const uint8_t *inner = NULL;
                uint16_t inl = 0;
                if (find_submsg(adm, adml, 43, &inner, &inl) && inner) {
                    pb_r_t ir;
                    pb_r_init(&ir, inner, inl);
                    uint8_t f2, w2;
                    uint32_t v2;
                    const uint8_t *b2;
                    uint16_t bl2;
                    while (pb_r_next(&ir, &f2, &w2, &v2, &b2, &bl2)) {
                        if (w2 == 5 && v2 >= 1577836800U) {
                            struct timeval now = {.tv_sec = v2, .tv_usec = 0};
                            settimeofday(&now, NULL);
                            ESP_LOGI(TAG, "time sync %u (NodeInfo last_heard now live)", (unsigned)v2);
                        }
                    }
                }
            }
        }
    } else if (af == 66) { // add_contact: SharedContact
        const uint8_t *sc;
        uint16_t scl;
        if (!find_submsg(adm, adml, 66, &sc, &scl)) err = 32;
        else {
            pb_r_t r;
            pb_r_init(&r, sc, scl);
            uint8_t f, w;
            uint32_t v;
            const uint8_t *b;
            uint16_t bl;
            uint32_t nn = 0;
            const uint8_t *user = NULL;
            uint16_t user_len = 0;
            bool verified = false;
            bool should_ignore = false;
            while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
                if (f == 1 && w == 0) nn = v;
                else if (f == 2 && w == 2) { user = b; user_len = bl; }
                else if (f == 3 && w == 0) should_ignore = v != 0;
                else if (f == 4 && w == 0) verified = v != 0;
            }
            if (nn < 4 || !user) err = 32;
            else {
                pb_r_t ur;
                pb_r_init(&ur, user, user_len);
                const uint8_t *pk = NULL;
                char long_name[40] = {0};
                char short_name[8] = {0};
                uint32_t hw_model = 0, role = 0;
                while (pb_r_next(&ur, &f, &w, &v, &b, &bl)) {
                    char *dst = NULL;
                    size_t cap = 0;
                    if (f == 2 && w == 2) {
                        dst = long_name;
                        cap = sizeof(long_name);
                    } else if (f == 3 && w == 2) {
                        dst = short_name;
                        cap = sizeof(short_name);
                    } else if (f == 5 && w == 0) {
                        hw_model = v;
                    } else if (f == 7 && w == 0) {
                        role = v;
                    } else if (f == 8 && w == 2 && bl == 32) {
                        pk = b;
                    }
                    if (dst && cap) {
                        size_t copy = bl < cap - 1 ? bl : cap - 1;
                        memcpy(dst, b, copy);
                        dst[copy] = '\0';
                    }
                }
                bool applied = lora_mesh_peer_apply_contact(
                    nn, long_name, short_name, hw_model, role,
                    pk, pk != NULL, verified, should_ignore);
                if (applied) {
                    peer_flag_t *pf = peer_flags(nn, true);
                    if (pf) {
                        if (should_ignore) {
                            pf->ignored = true;
                            pf->favorite = false;
                        } else {
                            // Native CLIENT nodes auto-favorite contacts sent
                            // by the app before a DM so they are not evicted.
                            pf->favorite = true;
                        }
                        lora_mesh_peer_set_favorite(nn, pf->favorite);
                        peer_flags_save();
                    }
                    if (should_ignore) {
                        lora_manager_note_ignored(nn, true);
                        lora_mesh_peer_set_ignored(nn, true);
                    }
                }
                ESP_LOGI(TAG, "add_contact nn=%08x applied=%u verified=%u key=%s",
                         (unsigned)nn, (unsigned)applied,
                         (unsigned)verified, pk ? "yes" : "no");
            }
        }
    } else if (af == 67) { // key_verification: KeyVerificationAdmin
        const uint8_t *kv;
        uint16_t kvl;
        if (!find_submsg(adm, adml, 67, &kv, &kvl)) err = 32;
        else {
            pb_r_t r;
            pb_r_init(&r, kv, kvl);
            uint8_t f, w;
            uint32_t v;
            const uint8_t *b;
            uint16_t bl;
            uint32_t mtype = 0, remote = 0, secno = 0;
            uint64_t nonce = 0;
            while (pb_r_next(&r, &f, &w, &v, &b, &bl)) {
                if (f == 1 && w == 0) mtype = v;
                else if (f == 2 && w == 0) remote = v;
                else if (f == 3 && (w == 0 || w == 1)) {
                    // nonce is uint64 varint-or-fixed: assemble LE.
                    if (w == 0) nonce = v;
                    else if (bl == 8) {
                        nonce = (uint64_t)b[0] | ((uint64_t)b[1] << 8) |
                                ((uint64_t)b[2] << 16) | ((uint64_t)b[3] << 24) |
                                ((uint64_t)b[4] << 32) | ((uint64_t)b[5] << 40) |
                                ((uint64_t)b[6] << 48) | ((uint64_t)b[7] << 56);
                    }
                } else if (f == 4 && w == 0) secno = v;
            }
            ESP_LOGI(TAG, "keyverify type=%u remote=%08x nonce=%llu sec=%u",
                     (unsigned)mtype, (unsigned)remote,
                     (unsigned long long)nonce, (unsigned)secno);
            if (mtype == 2) { // DO_VERIFY
                if (remote >= 4) lora_mesh_peer_set_verified(remote, true);
            } else if (mtype == 3) { // DO_NOT_VERIFY
                if (remote >= 4) lora_mesh_peer_set_verified(remote, false);
            } else if (mtype > 3) {
                err = 32;
            }
            // INITIATE/PROVIDE need no device action: the app drives the
            // mesh KeyVerification exchange (port 12) itself.
        }
    } else if (af == 38) { // remove_by_nodenum
        uint32_t nn = 0;
        if (!find_varint(adm, adml, 38, &nn) || nn < 4) err = 32;
        else {
            lora_mesh_remove_node(nn);
            lora_manager_note_ignored(nn, false);
            peer_flag_t *pf = peer_flags(nn, false);
            if (pf) {
                memset(pf, 0, sizeof(*pf));
                peer_flags_save();
            }
        }
    } else if (af == 49) { // toggle_muted_node
        uint32_t nn = 0;
        if (!find_varint(adm, adml, 49, &nn) || nn < 4) err = 32;
        else {
            bool cur = lora_mesh_peer_get_muted(nn);
            if (!lora_mesh_peer_set_muted(nn, !cur)) err = 32;
        }
    } else if (af == 64 || af == 65) { // begin/commit edit: no-op ack
    } else if (af == 36 || af == 37) { // set_canned/set_ringtone: persist
        const uint8_t *val = NULL;
        uint16_t vallen = 0;
        // set_canned_message_module_messages(36)/set_ringtone_message(37)
        // are LEN/string fields; the raw bytes are the message payload.
        if (!find_submsg(adm, adml, af, &val, &vallen)) err = 32;
        else {
            if (af == 36) msgblob_save("canned", s_canned, sizeof(s_canned), val, vallen);
            else msgblob_save("ringtone", s_ringtone, sizeof(s_ringtone), val, vallen);
            uint16_t kept = vallen;
            if (af == 36 && kept > LORA_CANNED_MAX) kept = LORA_CANNED_MAX;
            if (af == 37 && kept > LORA_RINGTONE_MAX) kept = LORA_RINGTONE_MAX;
            ESP_LOGI(TAG, "admin set_%s %uB persisted%s", af == 36 ? "canned" : "ringtone",
                     (unsigned)vallen, vallen > kept ? " (truncated)" : "");
        }
    } else if (af == 18) { // ham mode: NAK (no-ham by design)
        // Intentionally unsupported: NAK BAD_REQUEST so the app shows the
        // real state instead of assuming HAM mode applied from an ACK.
        err = 32;
    } else if (af == 97) { // reboot
        admin_arm_reboot("reboot"); // ACKs below, restarts ~500ms later
    } else if (af == 98) { // shutdown
        admin_arm_shutdown(); // ACKs below, radio+BLE stop + deep-sleep later
    } else if (af == 94 || af == 99) { // factory_reset (both IDs)
        admin_factory_erase(); // wipe NVS 'lora' now...
        admin_arm_reboot("factory_reset"); // ...ACK below, restart ~500ms later
    } else if (af == 100) { // nodedb_reset: peers only, no restart
        admin_nodedb_reset();
    } else if (af == 2 || af == 4 || af == 6 || af == 8 || af == 11 || af == 13 ||
               af == 15 || af == 17 || af == 20 || af == 45) {
        // App polling a response field as a request (or unknown): ack only.
    } else {
        // Unknown admin: ack OK (upstream ignores unknown fields).
    }
    if (!admin_ack(me, dst, t->msg_id, err)) return queued;
    return true;
}

// Verbatim phone->air Data TX: encrypts the app's complete Data submessage
// (portnum+payload+dest/source/reply_id/emoji/bitfield) as-is. Mirrors
// stock_tx_ch (lora_mesh.c) header/flags/crypto; the (portnum,payload,
// request_id) mesh builders can't carry Data fields 4/5/7/8/9, so routing
// through them silently drops reply threads and emoji reactions.
static bool air_send_data_verbatim(uint32_t to, const uint8_t *data, uint16_t dlen,
                                   uint32_t packet_id, bool want_ack, uint8_t ch_idx,
                                   uint32_t *out_air_id) {
    return lora_manager_send_data_verbatim_ch(to, data, dlen, packet_id,
                                               want_ack, ch_idx, out_air_id);
}

bool lora_phoneapi_on_toradio(const uint8_t *p, uint16_t len) {
    toradio_t t;
    if (!pb_parse_toradio(p, len, &t)) return false;
    if ((t.kind == TORADIO_PACKET_TEXT || t.kind == TORADIO_PACKET_OTHER) &&
        toradio_id_seen(t.msg_id)) {
        ESP_LOGI(TAG, "app packet id=%08x duplicate -> ignored",
                 (unsigned)t.msg_id);
        return false;
    }
    if (t.kind == TORADIO_WANT_CONFIG) {
        // Upstream handleStartConfig() re-opens PhoneAPI if it was closed.
        // This matters when a client sends ToRadio.disconnect but keeps the
        // BLE link/CCCD alive and immediately starts a fresh config session.
        if (!lora_phoneapi_is_linked()) lora_phoneapi_set_linked(true);
        lora_phoneapi_begin_config(t.want_config_id);
        return false;
    }
    if (t.kind == TORADIO_DISCONNECT) {
        lora_phoneapi_set_linked(false);
        return false;
    }
    if (t.kind == TORADIO_PACKET_TEXT) {
        // App text -> mesh air TX on the packet's channel.  The app already
        // owns its outgoing message; stock returns QueueStatus only and does
        // not loop the submitted MeshPacket back through FromRadio.
        // Unicast to a peer with a known public key goes out PKI-encrypted.
        uint8_t ch_idx = t.channel < 8 ? (uint8_t)t.channel : 0;
        const lora_channel_t *ch = lora_channel_get(ch_idx);
        if (!ch || !ch->used || ch->role == 0) ch_idx = lora_channel_primary();
        uint32_t air_id = 0;
        bool pki = false;
        uint8_t peer_pub[32];
        bool is_dm = (t.to != 0xFFFFFFFFu && t.to != 0);
        if (is_dm && lora_mesh_peer_pubkey(t.to, peer_pub)) {
            memset(peer_pub, 0, sizeof(peer_pub));
            pki = true;
        }
        if (is_dm && !pki) {
            // Upstream refuses legacy (channel-encrypted) DMs: stock drops
            // them ("Rejecting legacy DM"). NAK so the app shows the real
            // cause instead of a timeout. Exchange NodeInfo first.
            ESP_LOGW(TAG, "app DM to %08x refused: no public key", (unsigned)t.to);
            if (admin_ack(lora_mesh_node_num(), t.from ? t.from : t.to,
                          t.msg_id, 39)) // PKI_SEND_FAIL_PUBLIC_KEY
                return true;
            return false;
        }
        // A complete Data with reply/thread/reaction fields must stay intact.
        // Upstream PKI encrypts the serialized Data object, not a rebuilt
        // (port,payload) subset.
        bool has_data_extras = t.data_complete &&
            (t.has_reply_id || t.has_emoji || t.has_dest || t.has_source ||
             t.has_bitfield);
        if (!pki && has_data_extras) {
            uint32_t air_id = 0;
            if (air_send_data_verbatim(t.to, t.payload_head, t.payload_len,
                                       t.msg_id, t.want_ack, ch_idx, &air_id)) {
                (void)push_queue_status(air_id);
                ESP_LOGI(TAG, "app text+extra TX id=%08x to=%08x ch=%u",
                         (unsigned)air_id, (unsigned)t.to, (unsigned)ch_idx);
                return true;
            }
            return false;
        }
        bool sent;
        if (pki && has_data_extras) {
            sent = lora_manager_send_dm_data(t.payload_head, t.payload_len,
                                             t.to, t.msg_id, t.want_ack,
                                             &air_id);
        } else if (pki) {
            sent = lora_manager_send_dm_text(t.text, t.to, t.msg_id,
                                             t.want_ack, &air_id);
        } else {
            sent = lora_manager_send_app_text_ch(t.text, t.to, t.msg_id,
                                                  t.want_ack, ch_idx, &air_id);
        }
        if (sent) {
            // Confirm that the packet entered and left our (synchronous) radio
            // queue using the same id supplied by the app.
            (void)push_queue_status(air_id);
            ESP_LOGI(TAG, "app text TX id=%08x to=%08x ch=%u pki=%u want_ack=%u",
                     (unsigned)air_id, (unsigned)t.to, (unsigned)ch_idx,
                     (unsigned)pki, (unsigned)t.want_ack);
            return true;
        }
        return false;
    }
    if (t.kind == TORADIO_PACKET_OTHER) {
        char hex[37] = {0};
        uint16_t hlen = t.payload_len < 12 ? t.payload_len : 12;
        for (int i = 0; i < hlen; i++) snprintf(hex + i * 3, 4, "%02X ", t.payload_head[i]);
        ESP_LOGI(TAG, "app pkt port=%u to=0x%08x ch=%u len=%u admin=%u [%s]", (unsigned)t.port,
                 (unsigned)t.to, (unsigned)t.channel, (unsigned)t.payload_len, (unsigned)t.admin_field, hex);
        uint32_t me = lora_mesh_node_num();
        uint32_t dst = t.from ? t.from : t.to;
        bool queued = false;
        if (t.port == 6) {
            queued = lora_phoneapi_handle_admin(&t, me, dst);
            return queued;
        }
        // Non-admin Data (position/telemetry/traceroute/...): forward to air
        // on the packet's channel so the app's module screens actually TX.
        uint8_t ch_idx = t.channel < 8 ? (uint8_t)t.channel : 0;
        const lora_channel_t *ch = lora_channel_get(ch_idx);
        if (!ch || !ch->used || ch->role == 0) ch_idx = lora_channel_primary();
        // Parse Data submessage for port/payload/request to rebuild air Data.
        const uint8_t *dp = NULL;
        uint16_t dpl = 0;
        uint32_t dport = 0, dreq = 0;
        bool dwant_resp = false;
        {
            pb_r_t d;
            pb_r_init(&d, t.payload_head, t.payload_len);
            uint8_t df, dw;
            uint32_t dv;
            const uint8_t *db;
            uint16_t dl;
            while (pb_r_next(&d, &df, &dw, &dv, &db, &dl)) {
                if (df == 1 && dw == 0) dport = dv;
                else if (df == 2 && dw == 2) { dp = db; dpl = dl; }
                else if (df == 3 && dw == 0) dwant_resp = dv != 0;
                else if (df == 6 && dw == 5) dreq = dv;
            }
        }
        if (dport == 0) dport = t.port;
        uint32_t air_id = 0;
        uint8_t peer_pub[32];
        // Upstream perhapsEncode: PKI only for originated unicasts with a
        // known key, never for position/nodeinfo/routing/traceroute.
        bool pki_ok = (dport != 3 && dport != 4 && dport != 5 && dport != 70 &&
                       t.to != 0xFFFFFFFFu && t.to != 0 &&
                       lora_mesh_peer_pubkey(t.to, peer_pub));
        bool use_pki = pki_ok;
        if (use_pki) {
            memset(peer_pub, 0, sizeof(peer_pub));
            ESP_LOGI(TAG, "app pkt PKI-encrypted to %08x port=%u", (unsigned)t.to, (unsigned)dport);
            bool sent = t.data_complete
                ? lora_manager_send_dm_data(t.payload_head, t.payload_len,
                                            t.to, t.msg_id, t.want_ack, &air_id)
                : false;
            if (sent) {
                (void)push_queue_status(t.msg_id ? t.msg_id : air_id);
                queued = true;
            }
        } else if (t.data_complete) {
            // Verbatim: the app's Data (incl. dest/source/reply_id/emoji/
            // bitfield) goes on air untouched instead of a lossy rebuild.
            if (air_send_data_verbatim(t.to, t.payload_head, t.payload_len,
                                       t.msg_id, t.want_ack, ch_idx, &air_id)) {
                ESP_LOGI(TAG, "app pkt verbatim to=%08x port=%u id=%08x",
                         (unsigned)t.to, (unsigned)dport, (unsigned)air_id);
                (void)push_queue_status(t.msg_id ? t.msg_id : air_id);
                queued = true;
            }
        } else {
            bool sent = lora_manager_send_data_ch(t.to, (uint8_t)dport, dp, dpl,
                                                  dreq, t.msg_id, t.want_ack,
                                                  dwant_resp, ch_idx, &air_id);
            if (sent) {
                (void)push_queue_status(t.msg_id ? t.msg_id : air_id);
                queued = true;
            }
        }
        // Do not synthesize a successful Routing ACK here. Stock returns a
        // QueueStatus for local admission; delivery ACK/NAK must come from
        // the remote node and correlate through Data.request_id.
        return queued;
    }
    if (t.kind == TORADIO_IGNORED) {
        if (t.heartbeat_nonce == 1) {
            // Exact 2.7.26 lifecycle: nonce 1 forces a fresh NodeInfo ping so
            // peers can re-learn our name/public key after a reset.
            lora_mesh_request_nodeinfo();
            return false;
        }
        // Queue independently while a config walk is active. It will be the
        // first steady-state item, matching MeshService's priority queue.
        return push_queue_status(0);
    }
    return false; // unknown / ignored
}

void lora_phoneapi_stats(uint32_t *pushed, uint32_t *popped, uint32_t *dropped,
                         bool *linked) {
    if (pushed) *pushed = s_pushed;
    if (popped) *popped = s_popped;
    if (dropped) *dropped = s_dropped;
    if (linked) *linked = s_linked;
}

// Reboot counter init (call once from manager early_init).
void lora_phoneapi_boot(void) {
#if defined(CONFIG_SPIRAM)
    // Allocate the larger service history once, before radio RX can enqueue.
    // Keep the internal eight-packet arrays as a safe fallback.
    if (s_pending == s_pending_base) {
        uint8_t (*pending)[240] = heap_caps_calloc(
            LORA_PHONE_PENDING_PSRAM_DEPTH, sizeof(*pending),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        uint16_t *lengths = heap_caps_calloc(
            LORA_PHONE_PENDING_PSRAM_DEPTH, sizeof(*lengths),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pending && lengths) {
            s_pending = pending;
            s_pending_len = lengths;
            s_pending_depth = LORA_PHONE_PENDING_PSRAM_DEPTH;
            ESP_LOGI(TAG, "phone backlog in PSRAM: %u packets",
                     (unsigned)s_pending_depth);
        } else {
            if (pending) heap_caps_free(pending);
            if (lengths) heap_caps_free(lengths);
            ESP_LOGW(TAG, "PSRAM phone backlog allocation failed; using %u packets",
                     (unsigned)s_pending_depth);
        }
    }
#endif
    tzdef_load();
    peer_flags_load();
    msgblob_load("canned", s_canned, sizeof(s_canned));
    msgblob_load("ringtone", s_ringtone, sizeof(s_ringtone));
    lora_store_init();
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        uint32_t r = 0;
        nvs_get_u32(h, "reboot", &r);
        r++;
        nvs_set_u32(h, "reboot", r);
        nvs_commit(h);
        nvs_close(h);
        s_reboots = r;
    }
}

#else
typedef int lora_phoneapi_stub_guard;
#endif
