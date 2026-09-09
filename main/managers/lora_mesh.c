// lora_mesh.c
// Stock-wire mesh with FloodingRouter/ReliableRouter/NextHopRouter parity.
// SINGLE default crypto context + per-channel PSKs + PKI DMs (channel byte
// 0). Foreign-key frames can't be read locally but are still flood-forwarded
// by default (header-only mutation, ciphertext untouched) per upstream
// ALL-mode behavior; role/rebroadcast-mode gates optionally restrict that.

#include "managers/lora_mesh.h"
#include "managers/lora_manager.h"
#include "managers/lora_channels.h"
#include "managers/lora_crypto.h"
#include "managers/lora_modem.h"
#include "managers/lora_pb.h"
#include "managers/lora_phoneapi.h"
#include "managers/lora_pki.h"
#include "managers/lora_sx1262.h"
#include "managers/fuel_gauge_manager.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS // TF-PSA hides decls without this
#include "mbedtls/private/ccm.h"
#include "mbedtls/private/sha256.h"

#define MESH_HDR_LEN 16
#define MESH_HIST 32
#define NODEINFO_SECS (3u * 3600u)
#define NODEINFO_INITIAL_DELAY_SECS 30u
#define NODEINFO_REPLY_SUPPRESS_SECS (12u * 3600u)
#define NODEINFO_REPLY_CACHE_BASE_N 32
#define NODEINFO_REPLY_CACHE_PSRAM_N LORA_MESH_NODES_MAX
#define NODEINFO_NORMAL_THROTTLE_MS (10u * 60u * 1000u)
#define NODEINFO_INTERACTIVE_THROTTLE_MS (60u * 1000u)
#define NODEDB_FULL_ADMIT_INTERVAL_MS 10000u
#define POS_BCAST_SECS (15u * 60u)   // periodic position originate interval
#define TLM_BCAST_SECS (1u * 3600u)  // hourly device-telemetry originate
// Air PortNums (portnums.proto). Verified: phone-side PKI exclusion list
// covers 3/4/5/70 (lora_phoneapi.c pki_ok), manager telemetry path uses 67.
#define MESH_PORT_POSITION 3    // POSITION_APP
#define MESH_PORT_TELEMETRY 67  // TELEMETRY_APP
#define MESH_PORT_TRACEROUTE 70 // TRACEROUTE_APP

static uint32_t s_node = 0;
static bool s_init = false;
static char s_owner_long[40];
static char s_owner_short[8];

void lora_mesh_owner(char *lo, size_t lc, char *sh, size_t sc) {
    if (!s_init) lora_mesh_init();
    if (lo && lc) snprintf(lo, lc, "%s", s_owner_long);
    if (sh && sc) snprintf(sh, sc, "%s", s_owner_short);
}

bool lora_mesh_set_owner(const char *lo, const char *sh) {
    if (!lo || !sh || !lo[0] || !sh[0] || strlen(lo) >= sizeof(s_owner_long) ||
        strlen(sh) >= sizeof(s_owner_short)) return false;
    nvs_handle_t h;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, "owner_long", lo) == ESP_OK &&
              nvs_set_str(h, "owner_short", sh) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (ok) {
        snprintf(s_owner_long, sizeof(s_owner_long), "%s", lo);
        snprintf(s_owner_short, sizeof(s_owner_short), "%s", sh);
    }
    return ok;
}

static uint32_t s_hist_from[MESH_HIST];
static uint32_t s_hist_id[MESH_HIST];
static uint8_t s_hist_pos = 0;
static uint32_t s_dups = 0;

/* The 200-entry NodeDB is the largest long-lived LoRa allocation (~17 KB).
 * Keep an internal fallback for Heltec/no-PSRAM boards, but use PSRAM on the
 * display profiles when available.  NVS writes copy each peer into a small
 * internal blob before commit, so the table itself never remains a flash
 * operation buffer. */
#if defined(CONFIG_SPIRAM)
static lora_mesh_node_t *s_nodes;
#else
static lora_mesh_node_t s_nodes_internal[LORA_MESH_NODES_MAX];
static lora_mesh_node_t *s_nodes = s_nodes_internal;
#endif
static uint16_t s_ncount = 0;
static uint8_t s_hop_limit = LORA_MESH_TTL;
static uint32_t s_next_nodeinfo_ms = 0;
static uint32_t s_nodeinfo_target = 0;
static uint32_t s_nodeinfo_target_due_ms = 0;
static uint8_t s_nodeinfo_target_channel = 0;
static bool s_nodeinfo_key_refresh_broadcast = false;
static bool s_nodeinfo_force = false;
/* Native NodeInfoModule suppresses repeated replies to the same requester for
 * 12 hours. Keep this bounded and uptime-based; it is intentionally not
 * persisted, so rebooting starts a fresh discovery generation. */
typedef struct {
    uint32_t node;
    uint32_t secs;
} nodeinfo_reply_t;
static nodeinfo_reply_t s_nodeinfo_reply_base[NODEINFO_REPLY_CACHE_BASE_N];
static nodeinfo_reply_t *s_nodeinfo_replies = s_nodeinfo_reply_base;
static uint16_t s_nodeinfo_reply_cap = NODEINFO_REPLY_CACHE_BASE_N;
static uint32_t s_last_nodeinfo_tx_ms = 0;
static uint32_t s_last_full_node_admit_ms = 0;
// Flood parity state (defaults preserve flood-all HIL behavior).
static int s_role = LORA_MESH_ROLE_CLIENT;
static int s_rebroadcast_mode = LORA_MESH_RB_ALL;
static int s_pos_precision_bits = 32; // 32 = full, originator-only mask
static bool s_neighborinfo_enabled = false; // default: drop NEIGHBORINFO
// Current modem for airtime accounting (set by manager on start/reconfig).
static int s_modem_sf = 11;
static int s_modem_bw_khz = 250;
static int s_modem_cr = 5;

void lora_mesh_set_modem(int sf, int bw_khz, int cr) {
    if (sf >= 5 && sf <= 12) s_modem_sf = sf;
    if (bw_khz == 125 || bw_khz == 250 || bw_khz == 500) s_modem_bw_khz = bw_khz;
    if (cr >= 5 && cr <= 8) s_modem_cr = cr;
}

static uint32_t region_freq_hz(int code); // defined with the table below
static uint32_t region_freq_hz_ex(int code, const lora_modem_cfg_t *mcfg);
// Defined with the Data parser below; needed early by lora_mesh_try_pki.
static uint8_t data_parse(const uint8_t *p, uint16_t len,
                          const uint8_t **payload, uint16_t *plen,
                          uint32_t *request_id);
static bool hdr_parse(const uint8_t *f, uint8_t len, uint32_t *to, uint32_t *from,
                      uint32_t *id, uint8_t *hop_limit, uint8_t *hop_start,
                      bool *want_ack, uint8_t *channel);

uint32_t lora_air_freq_hz(int pb_code) {
    return region_freq_hz(pb_code);
}

uint32_t lora_air_freq_hz_ex(int pb_code, const lora_modem_cfg_t *mcfg) {
    return region_freq_hz_ex(pb_code, mcfg);
}

// ---- Region table: upstream RDEF bands restricted to the STD profile +
// EU_868, all of which admit LongFast (SF11/BW250/CR4/5). EU_866/EU_N_868
// (LITE/NARROW presets) and licensed HAM bands are intentionally absent.
// pb codes match meshtastic_Config_LoRaConfig_RegionCode_*.
typedef struct {
    int code;
    const char *name; // CLI/Kconfig/UI token
    float start_mhz;
    float end_mhz;
    float duty_pct;
} region_def_t;

static const region_def_t REGIONS[] = {
    {1, "us915", 902.0f, 928.0f, 100},
    {3, "eu868", 869.4f, 869.65f, 10},
    {2, "eu433", 433.0f, 434.0f, 10},
    {4, "cn", 470.0f, 510.0f, 100},
    {5, "jp", 920.5f, 923.5f, 100},
    {6, "anz", 915.0f, 928.0f, 100},
    {7, "kr", 920.0f, 923.0f, 100},
    {8, "tw", 920.0f, 925.0f, 100},
    {9, "ru", 868.7f, 869.2f, 100},
    {10, "in", 865.0f, 867.0f, 100},
    {11, "nz865", 864.0f, 868.0f, 100},
    {12, "th", 920.0f, 925.0f, 10},
    {14, "ua433", 433.0f, 434.7f, 10},
    {16, "my433", 433.0f, 435.0f, 100},
    {17, "my919", 919.0f, 924.0f, 100},
    {18, "sg923", 917.0f, 925.0f, 100},
    {19, "ph433", 433.0f, 434.7f, 100},
    {20, "ph868", 868.0f, 869.4f, 100},
    {21, "ph915", 915.0f, 918.0f, 100},
    {22, "anz433", 433.05f, 434.79f, 100},
    {23, "kz433", 433.075f, 434.775f, 100},
    {24, "kz863", 863.0f, 868.0f, 100},
    {25, "np865", 865.0f, 868.0f, 100},
    {26, "br902", 902.0f, 907.5f, 100},
};
#define NREGIONS (sizeof(REGIONS) / sizeof(REGIONS[0]))

static uint32_t djb2(const char *s) {
    uint32_t h = 5381;
    while (*s) h = h * 33 + (uint8_t)*s++;
    return h;
}

static uint32_t region_freq_hz(int code) {
    const region_def_t *r = NULL;
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (REGIONS[i].code == code) {
            r = &REGIONS[i];
            break;
        }
    }
    if (!r) return 906875000u; // unknown -> US default (never TX-blind)
    // Upstream: width=bw/1000, slots=round(band/width),
    // slot=djb2(presetName)%slots, freq=start+bw/2000+slot*width (MHz).
    const float bw_mhz = 0.25f; // LongFast BW250
    float width = bw_mhz;
    int slots = (int)(((r->end_mhz - r->start_mhz) / width) + 0.5f);
    if (slots < 1) slots = 1;
    uint32_t slot = djb2(LORA_DEFAULT_PRESET_NAME) % (uint32_t)slots;
    float f = r->start_mhz + bw_mhz / 2 + (float)slot * width;
    return (uint32_t)(f * 1000000.0f + 0.5f);
}

// Upstream RadioInterface::applyModemConfig frequency selection:
// override_frequency wins verbatim; else channel_num (1-based) selects the
// slot directly; else hash(channelName)%numChannels where channelName is the
// primary channel name or the preset display name when empty.
static uint32_t region_freq_hz_ex(int code, const lora_modem_cfg_t *mcfg) {
    const region_def_t *r = NULL;
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (REGIONS[i].code == code) {
            r = &REGIONS[i];
            break;
        }
    }
    if (!r) return 906875000u;
    int sf = 11, bwk = 250, cr = 5;
    if (mcfg) {
        lora_modem_effective(mcfg, &sf, &bwk, &cr);
    }
    float bw_mhz = (float)bwk / 1000.0f;
    if (bw_mhz <= 0) bw_mhz = 0.25f;
    if (mcfg && mcfg->override_freq_mhz > 0.1f) {
        float f = mcfg->override_freq_mhz + mcfg->freq_offset_mhz;
        return (uint32_t)(f * 1000000.0f + 0.5f);
    }
    float span = r->end_mhz - r->start_mhz;
    int slots = (int)(span / bw_mhz + 0.5f);
    if (slots < 1) slots = 1;
    // Narrow region + wide preset: upstream falls back to LongFast.
    if (span < bw_mhz) {
        bw_mhz = 0.25f;
        slots = (int)(span / bw_mhz + 0.5f);
        if (slots < 1) slots = 1;
        uint32_t s = djb2("LongFast") % (uint32_t)slots;
        float f = r->start_mhz + bw_mhz / 2 + (float)s * bw_mhz;
        if (mcfg) f += mcfg->freq_offset_mhz;
        return (uint32_t)(f * 1000000.0f + 0.5f);
    }
    uint32_t slot = 0;
    if (mcfg && mcfg->channel_num > 0) {
        slot = (mcfg->channel_num - 1) % (uint32_t)slots;
    } else {
        const lora_channel_t *pc = NULL;
        const char *cname = NULL;
        char tmp[32] = {0};
        // Primary channel name, or preset display name when empty.
        extern const lora_channel_t *lora_channel_get(uint8_t idx);
        pc = lora_channel_get(lora_channel_primary());
        if (pc && pc->name[0]) {
            snprintf(tmp, sizeof(tmp), "%s", pc->name);
            cname = tmp;
        } else {
            cname = lora_preset_display_name(mcfg ? mcfg->preset : 0,
                                            mcfg ? mcfg->use_preset : true);
        }
        slot = djb2(cname ? cname : "LongFast") % (uint32_t)slots;
    }
    float f = r->start_mhz + bw_mhz / 2 + (float)slot * bw_mhz;
    if (mcfg) f += mcfg->freq_offset_mhz;
    return (uint32_t)(f * 1000000.0f + 0.5f);
}

int lora_region_count(void) { return (int)NREGIONS; }

int lora_region_code(int index) {
    if (index < 0 || index >= (int)NREGIONS) return 1;
    return REGIONS[index].code;
}

const char *lora_region_name(int code) {
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (REGIONS[i].code == code) return REGIONS[i].name;
    }
    return "?";
}

const char *lora_region_name_by_index(int index) {
    if (index < 0 || index >= (int)NREGIONS) return "?";
    return REGIONS[index].name;
}

int lora_region_by_name(const char *name) {
    if (!name) return -1;
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (strcasecmp(name, REGIONS[i].name) == 0) return REGIONS[i].code;
    }
    return -1;
}

int lora_region_next(int code) {
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (REGIONS[i].code == code) return REGIONS[(i + 1) % NREGIONS].code;
    }
    return 1;
}

float lora_region_duty(int code) {
    for (unsigned i = 0; i < NREGIONS; i++) {
        if (REGIONS[i].code == code) return REGIONS[i].duty_pct;
    }
    return 100;
}

// ---- Duty-cycle guard ----
#define DUTY_HIST 32
static uint32_t s_duty_ms[DUTY_HIST];
static uint32_t s_duty_dur[DUTY_HIST];
static uint8_t s_duty_pos = 0;
static uint32_t s_duty_drop_count = 0;
static uint32_t s_duty_region = 1;

void lora_duty_set_region(int pb_code) { s_duty_region = (uint32_t)pb_code; }

// Semtech airtime for the current modem (SF/BW/CR, pre16/explicit/CRC).
static uint32_t airtime_ms(uint8_t frame_len) {
    float bw_hz = (float)s_modem_bw_khz * 1000.0f;
    if (bw_hz <= 0) bw_hz = 250000.0f;
    float tsym = ((float)(1 << s_modem_sf) / bw_hz) * 1000.0f;
    int sf = s_modem_sf;
    int cr = s_modem_cr; // 5..8 -> (cr-4) extra bits per symbol group
    int den = sf;
    if (den <= 0) den = 11;
    int num = 8 * (int)frame_len - 4 * sf + 28 + 16 - 20;
    int sym = 0;
    if (num > 0) sym = ((num + 4 * den - 1) / (4 * den)) * (cr + 4);
    float total = 16 + 4.25f + 8 + (float)sym;
    // LDRO adds roughly 1 extra symbol per group at SF11/12-125; ignore here.
    return (uint32_t)(total * tsym + 0.5f);
}

static uint32_t duty_used_ms(uint32_t now) {
    uint32_t sum = 0;
    for (int i = 0; i < DUTY_HIST; i++) {
        if (s_duty_ms[i] != 0 && now - s_duty_ms[i] < 3600000u) sum += s_duty_dur[i];
    }
    return sum;
}

bool lora_duty_allow(uint8_t frame_len) {
    float duty = lora_region_duty((int)s_duty_region);
    if (duty >= 100) return true;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t est = airtime_ms(frame_len);
    if (duty_used_ms(now) + est > (uint32_t)(duty / 100.0f * 3600000.0f)) {
        s_duty_drop_count++;
        return false;
    }
    return true;
}

void lora_duty_record(uint8_t frame_len) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_duty_ms[s_duty_pos] = now;
    s_duty_dur[s_duty_pos] = airtime_ms(frame_len);
    s_duty_pos = (uint8_t)((s_duty_pos + 1) % DUTY_HIST);
}

uint32_t lora_duty_drops(void) { return s_duty_drop_count; }

// ---- LoRa mesh peer persistence (NodeDB parity, NVS namespace "lora") ----
// RAM table s_nodes is LRU-200; identity (nodenum/pki_priv) already persists.
// Peers persist as one small blob per node plus an index array so the table
// survives reboot (handshake peers=0 / DM key-mismatch otherwise).
// Per-peer key: "pr_<8 hex node>" e.g. "pr_e026f431" (11 chars < 15 limit).
// Index blobs: "peers" (primary) + "pr_idx" mirror (compat); array of u32
// node_nums, capped separately at LORA_MESH_PERSIST_MAX. The legacy-compatible
// blob layout stays unchanged so upgrades retain learned keys and names. The
// smaller flash cap protects the board's 0x7000 NVS partition; all 200 peers
// remain available in RAM until reboot. All NVS ops fail-open.
typedef struct {
    uint32_t node_num;
    char long_name[40];
    char short_name[8];
    uint8_t pubkey[32];
    uint8_t has_pubkey;
    uint8_t verified;
    uint8_t muted;
    uint8_t ignored;
    uint32_t hw_model;
    uint32_t role;
    uint8_t has_user;
    uint8_t channel;
    uint8_t favorite; // consumes former tail padding; blob sizeof unchanged
} mesh_peer_blob_t;
_Static_assert(sizeof(mesh_peer_blob_t) == 100,
               "peer blob layout changed; add an explicit NVS migration");

// Persistence priority when the deliberately small NVS cache fills. Keep
// manually verified keys first, then PKI/behavior flags, then name-only
// contacts. This prevents ordinary NodeInfo churn from displacing DM keys.
static uint8_t mesh_peer_blob_priority(const mesh_peer_blob_t *b) {
    if (!b) return 0;
    if (b->verified) return 3;
    if (b->has_pubkey || b->muted || b->ignored || b->favorite) return 2;
    if (b->has_user || b->long_name[0] || b->short_name[0]) return 1;
    return 0;
}

static void mesh_peer_key(char *out, size_t cap, uint32_t node_num) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "pr_%08x", (unsigned)node_num);
}

// Read the peer index ("peers" primary, "pr_idx" fallback) into out.
// Returns entry count (0 when missing/unavailable). Fail-open.
static size_t mesh_peers_idx_read(nvs_handle_t h, uint32_t *out, size_t cap) {
    if (!out || cap == 0) return 0;
    size_t n = cap * sizeof(out[0]);
    if (nvs_get_blob(h, "peers", out, &n) == ESP_OK) {
        size_t cnt = n / sizeof(out[0]);
        return cnt > cap ? cap : cnt;
    }
    n = cap * sizeof(out[0]);
    if (nvs_get_blob(h, "pr_idx", out, &n) == ESP_OK) {
        size_t cnt = n / sizeof(out[0]);
        return cnt > cap ? cap : cnt;
    }
    return 0;
}

// Write the peer index to both "peers" and "pr_idx" (compat mirror).
static bool mesh_peers_idx_write(nvs_handle_t h, const uint32_t *list, size_t cnt) {
    if (cnt == 0) {
        esp_err_t a = nvs_erase_key(h, "peers");
        esp_err_t b = nvs_erase_key(h, "pr_idx");
        return (a == ESP_OK || a == ESP_ERR_NVS_NOT_FOUND) &&
               (b == ESP_OK || b == ESP_ERR_NVS_NOT_FOUND);
    }
    return nvs_set_blob(h, "peers", list, cnt * sizeof(list[0])) == ESP_OK &&
           nvs_set_blob(h, "pr_idx", list, cnt * sizeof(list[0])) == ESP_OK;
}

// Upsert one RAM entry to NVS (blob "pr_<hex>" + index update + commit).
// Cap: LORA_MESH_PERSIST_MAX entries; evicts the oldest lowest-priority
// entry when full and never replaces a higher-priority entry with a lower.
// Fail-open on any NVS error. Skips reserved ids and our own node.
static void mesh_peers_store_one(const lora_mesh_node_t *src) {
    if (!src || src->node_num < 4 || src->node_num == LORA_MESH_BROADCAST) return;
    if (src->node_num == s_node) return;
    mesh_peer_blob_t b;
    memset(&b, 0, sizeof(b));
    b.node_num = src->node_num;
    snprintf(b.long_name, sizeof(b.long_name), "%s", src->long_name);
    snprintf(b.short_name, sizeof(b.short_name), "%s", src->short_name);
    memcpy(b.pubkey, src->pubkey, sizeof(b.pubkey));
    b.has_pubkey = src->has_pubkey ? 1 : 0;
    // Zero-key guard: never persist a "has_pubkey" with an all-zero key.
    if (b.has_pubkey) {
        bool allz = true;
        for (int i = 0; i < 32; i++) if (b.pubkey[i]) { allz = false; break; }
        if (allz) b.has_pubkey = 0;
    }
    b.verified = src->key_verified ? 1 : 0;
    b.muted = src->muted ? 1 : 0;
    b.ignored = src->ignored ? 1 : 0;
    b.hw_model = src->hw_model;
    b.role = src->role;
    b.has_user = src->has_user ? 1 : 0;
    b.channel = src->channel < LORA_CH_MAX ? src->channel : 0;
    b.favorite = src->favorite ? 1 : 0;
    char key[16];
    mesh_peer_key(key, sizeof(key), src->node_num);
    nvs_handle_t h = 0;
    esp_err_t open_rc = nvs_open("lora", NVS_READWRITE, &h);
    if (open_rc != ESP_OK) {
        ESP_LOGW("LoRaMesh", "peer %08x not persisted: nvs_open %s",
                 (unsigned)src->node_num, esp_err_to_name(open_rc));
        return;
    }
    uint32_t idx[LORA_MESH_PERSIST_MAX];
    size_t n = mesh_peers_idx_read(h, idx, LORA_MESH_PERSIST_MAX);
    bool present = false;
    for (size_t i = 0; i < n; i++) if (idx[i] == src->node_num) { present = true; break; }
    if (!present) {
        if (n >= LORA_MESH_PERSIST_MAX) {
            // Index full: choose the oldest least-important entry. Refuse to
            // evict a more important entry for a lower-priority newcomer.
            size_t victim = 0;
            uint8_t victim_prio = UINT8_MAX;
            for (size_t i = 0; i < n; i++) {
                char old_key[16];
                mesh_peer_key(old_key, sizeof(old_key), idx[i]);
                mesh_peer_blob_t old;
                size_t old_len = sizeof(old);
                memset(&old, 0, sizeof(old));
                uint8_t prio = 0;
                if (nvs_get_blob(h, old_key, &old, &old_len) == ESP_OK)
                    prio = mesh_peer_blob_priority(&old);
                if (prio < victim_prio) {
                    victim = i;
                    victim_prio = prio;
                }
            }
            if (victim_prio > mesh_peer_blob_priority(&b)) {
                nvs_close(h);
                return;
            }
            char vkey[16];
            mesh_peer_key(vkey, sizeof(vkey), idx[victim]);
            (void)nvs_erase_key(h, vkey);
            memmove(&idx[victim], &idx[victim + 1],
                    (n - victim - 1) * sizeof(idx[0]));
            n--;
        }
        idx[n++] = src->node_num;
    }
    esp_err_t blob_rc = nvs_set_blob(h, key, &b, sizeof(b));
    if (blob_rc != ESP_OK) {
        ESP_LOGW("LoRaMesh", "peer %08x not persisted: nvs_set_blob %s",
                 (unsigned)src->node_num, esp_err_to_name(blob_rc));
        nvs_close(h);
        return;
    }
    if (!mesh_peers_idx_write(h, idx, n)) {
        ESP_LOGW("LoRaMesh", "peer %08x index not persisted",
                 (unsigned)src->node_num);
        nvs_close(h);
        return;
    }
    esp_err_t commit_rc = nvs_commit(h);
    if (commit_rc != ESP_OK)
        ESP_LOGW("LoRaMesh", "peer %08x commit failed: %s",
                 (unsigned)src->node_num, esp_err_to_name(commit_rc));
    nvs_close(h);
}

// Delete one peer's NVS entry + remove it from the index. Fail-open.
static void mesh_peers_erase_one(uint32_t node_num) {
    if (node_num == 0 || node_num == LORA_MESH_BROADCAST) return;
    char key[16];
    mesh_peer_key(key, sizeof(key), node_num);
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_key(h, key);
    uint32_t idx[LORA_MESH_PERSIST_MAX];
    size_t n = mesh_peers_idx_read(h, idx, LORA_MESH_PERSIST_MAX);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (idx[i] != node_num) idx[w++] = idx[i];
    }
    if (w != n) mesh_peers_idx_write(h, idx, w);
    (void)nvs_commit(h);
    nvs_close(h);
}

// Load all persisted peers back into s_nodes. Called from lora_mesh_init
// after nodenum/owner load. Marks hops invalid + rssi 0 (ephemeral radio
// state is not persisted). Fail-open; loads the bounded persistent cache.
static void mesh_peers_load_all(void) {
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    uint32_t idx[LORA_MESH_PERSIST_MAX];
    size_t n = mesh_peers_idx_read(h, idx, LORA_MESH_PERSIST_MAX);
    for (size_t k = 0; k < n; k++) {
        uint32_t nn = idx[k];
        if (nn < 4 || nn == LORA_MESH_BROADCAST || nn == s_node) continue;
        bool dup = false;
        for (int i = 0; i < s_ncount; i++) {
            if (s_nodes[i].node_num == nn) { dup = true; break; }
        }
        if (dup) continue;
        if (s_ncount >= LORA_MESH_NODES_MAX) break;
        char key[16];
        mesh_peer_key(key, sizeof(key), nn);
        mesh_peer_blob_t b;
        size_t blen = sizeof(b);
        memset(&b, 0, sizeof(b));
        if (nvs_get_blob(h, key, &b, &blen) != ESP_OK) continue;
        const size_t old_blob_size = offsetof(mesh_peer_blob_t, has_user);
        if ((blen != old_blob_size && blen != sizeof(b)) || b.node_num != nn)
            continue;
        lora_mesh_node_t e;
        memset(&e, 0, sizeof(e));
        e.node_num = b.node_num;
        // Old blobs predate has_user/channel. Names, a key, hardware, or role
        // prove a real User/contact existed; a fallback short hex alone was
        // only the former provisional-peer behavior.
        e.has_user = blen > offsetof(mesh_peer_blob_t, has_user)
                         ? b.has_user != 0
                         : (b.long_name[0] != '\0' || b.has_pubkey != 0 ||
                            b.hw_model != 0 || b.role != 0);
        memcpy(e.long_name, b.long_name, sizeof(e.long_name));
        e.long_name[sizeof(e.long_name) - 1] = '\0';
        memcpy(e.short_name, b.short_name, sizeof(e.short_name));
        e.short_name[sizeof(e.short_name) - 1] = '\0';
        if (!e.has_user) {
            e.long_name[0] = '\0';
            e.short_name[0] = '\0';
        }
        memcpy(e.pubkey, b.pubkey, sizeof(e.pubkey));
        e.has_pubkey = b.has_pubkey ? true : false;
        if (e.has_pubkey) {
            bool allz = true;
            for (int i = 0; i < 32; i++) if (e.pubkey[i]) { allz = false; break; }
            if (allz) e.has_pubkey = false;
        }
        e.hw_model = b.hw_model <= UINT16_MAX ? (uint16_t)b.hw_model : 0;
        e.role = b.role <= UINT8_MAX ? (uint8_t)b.role : 0;
        e.channel = blen > offsetof(mesh_peer_blob_t, channel) &&
                            b.channel < LORA_CH_MAX
                        ? b.channel
                        : 0;
        e.key_verified = b.verified ? true : false;
        e.muted = b.muted ? true : false;
        e.ignored = b.ignored ? true : false;
        e.favorite = blen > offsetof(mesh_peer_blob_t, favorite)
                         ? b.favorite != 0 : false;
        e.last_rssi = 0;
        e.last_snr = 0.0f;
        e.last_seen_ms = 0;
        e.hops_valid = false;
        e.hops_away = 0;
        s_nodes[s_ncount++] = e;
    }
    nvs_close(h);
}

static int node_oldest_index(void) {
    if (s_ncount == 0) return -1;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t oldest_age = 0;
    int oldest = 0;
    for (int i = 0; i < s_ncount; i++) {
        uint32_t age = now - s_nodes[i].last_seen_ms;
        if (age > oldest_age) {
            oldest_age = age;
            oldest = i;
        }
    }
    return oldest;
}

static void node_touch_named(uint32_t from, const char *long_name,
                             const char *short_name,
                             bool has_user, uint8_t channel,
                             int16_t rssi, float snr) {
    int idx = -1;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == from) { idx = i; break; }
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (idx >= 0) {
        s_nodes[idx].last_rssi = rssi;
        s_nodes[idx].last_snr = snr;
        s_nodes[idx].last_seen_ms = now;
        // Persist only on name changes; RSSI-only touches skip NVS
        // (no commit per RSSI update) to avoid flash wear on every RX.
        bool identity_changed = false;
        if (has_user && !s_nodes[idx].has_user) {
            s_nodes[idx].has_user = true;
            identity_changed = true;
        }
        if (channel < LORA_CH_MAX && s_nodes[idx].channel != channel) {
            s_nodes[idx].channel = channel;
            identity_changed = true;
        }
        // A valid NodeInfo replaces the complete native User identity. Empty
        // names therefore clear stale/provisional values instead of leaving
        // an old name attached to a freshly reset node.
        if (has_user && long_name) {
            char compact[sizeof(s_nodes[idx].long_name)];
            snprintf(compact, sizeof(compact), "%s", long_name);
            if (strcmp(s_nodes[idx].long_name, compact) != 0) {
                memcpy(s_nodes[idx].long_name, compact, sizeof(compact));
                identity_changed = true;
            }
        }
        if (has_user && short_name) {
            char compact[sizeof(s_nodes[idx].short_name)];
            snprintf(compact, sizeof(compact), "%s", short_name);
            if (strcmp(s_nodes[idx].short_name, compact) != 0) {
                memcpy(s_nodes[idx].short_name, compact, sizeof(compact));
                identity_changed = true;
            }
        }
        if (identity_changed &&
            (s_nodes[idx].has_user || s_nodes[idx].has_pubkey ||
             s_nodes[idx].key_verified || s_nodes[idx].muted ||
             s_nodes[idx].ignored)) {
            mesh_peers_store_one(&s_nodes[idx]);
        }
        return;
    }
    lora_mesh_node_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = from;
    n.has_user = has_user;
    n.channel = channel < LORA_CH_MAX ? channel : 0;
    n.last_rssi = rssi;
    n.last_snr = snr;
    n.last_seen_ms = now;
    if (long_name && long_name[0]) snprintf(n.long_name, sizeof(n.long_name), "%s", long_name);
    if (short_name && short_name[0])
        snprintf(n.short_name, sizeof(n.short_name), "%s", short_name);
    // New peer: evict the RAM LRU at capacity. Ephemeral header-only peers
    // stay RAM-only; a real User/contact or key/flag mutation is persisted.
    uint32_t evicted = 0;
    bool had_evict = false;
    if (s_ncount >= LORA_MESH_NODES_MAX) {
        // Node numbers are unauthenticated. Match native NodeDB's full-table
        // admission throttle so a noisy/hostile mesh cannot churn real peers
        // out of the discovery list at packet rate.
        if (s_last_full_node_admit_ms != 0 &&
            (uint32_t)(now - s_last_full_node_admit_ms) <
                NODEDB_FULL_ADMIT_INTERVAL_MS)
            return;
        s_last_full_node_admit_ms = now;
        int victim = node_oldest_index();
        if (victim < 0) return;
        evicted = s_nodes[victim].node_num;
        memmove(&s_nodes[victim], &s_nodes[victim + 1],
                (size_t)(s_ncount - victim - 1) * sizeof(s_nodes[0]));
        s_ncount--;
        had_evict = true;
    }
    memmove(&s_nodes[1], &s_nodes[0], (size_t)s_ncount * sizeof(n));
    s_nodes[0] = n;
    s_ncount++;
    if (had_evict && evicted != from) mesh_peers_erase_one(evicted);
    if (s_nodes[0].has_user) mesh_peers_store_one(&s_nodes[0]);
}

static bool seen_before(uint32_t from, uint32_t id) {
    for (int i = 0; i < MESH_HIST; i++) {
        if (s_hist_from[i] == from && s_hist_id[i] == id) return true;
    }
    return false;
}
static void seen_mark(uint32_t from, uint32_t id) {
    s_hist_from[s_hist_pos] = from;
    s_hist_id[s_hist_pos] = id;
    s_hist_pos = (uint8_t)((s_hist_pos + 1) % MESH_HIST);
}

void lora_mesh_init(void) {
    if (s_init) return;
    s_init = true;
#if defined(CONFIG_SPIRAM)
    lora_mesh_node_t *psram_nodes = heap_caps_calloc(
        LORA_MESH_NODES_MAX, sizeof(*psram_nodes),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (psram_nodes) {
        s_nodes = psram_nodes;
    } else {
        s_nodes = heap_caps_calloc(LORA_MESH_NODES_MAX, sizeof(*s_nodes),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGW("LoRaMesh", "PSRAM NodeDB allocation failed; using internal RAM");
    }
    nodeinfo_reply_t *reply_cache = heap_caps_calloc(
        NODEINFO_REPLY_CACHE_PSRAM_N, sizeof(*reply_cache),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (reply_cache) {
        s_nodeinfo_replies = reply_cache;
        s_nodeinfo_reply_cap = NODEINFO_REPLY_CACHE_PSRAM_N;
    } else {
        ESP_LOGW("LoRaMesh", "PSRAM NodeInfo reply cache allocation failed; using %u entries",
                 (unsigned)s_nodeinfo_reply_cap);
    }
#endif
    if (!s_nodes) {
        ESP_LOGE("LoRaMesh", "NodeDB allocation failed");
        s_init = false;
        return;
    }
    lora_channels_init();
    uint32_t mac_n = 0;
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        mac_n = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                ((uint32_t)mac[4] << 8) | mac[5];
    }
    if (mac_n == 0 || mac_n < 4) mac_n = 0x0A000001; // 0..3 reserved upstream
    nvs_handle_t h = 0;
    uint32_t stored = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_get_u32(h, "nodenum", &stored) != ESP_OK || stored < 4) {
            stored = mac_n;
            nvs_set_u32(h, "nodenum", stored);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    s_node = stored ? stored : mac_n;
    uint32_t hop = 0;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u32(h, "hop", &hop) == ESP_OK && hop >= 1 && hop <= 7) s_hop_limit = (uint8_t)hop;
        nvs_close(h);
    }
    snprintf(s_owner_short, sizeof(s_owner_short), "G%02X%02X",
             (unsigned)((s_node >> 8) & 255), (unsigned)(s_node & 255));
    snprintf(s_owner_long, sizeof(s_owner_long), "Ghost-%s", s_owner_short);
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        size_t ln = sizeof(s_owner_long), sn = sizeof(s_owner_short);
        nvs_get_str(h, "owner_long", s_owner_long, &ln);
        nvs_get_str(h, "owner_short", s_owner_short, &sn);
        nvs_close(h);
    }
    // Restore persisted mesh peers (NodeDB parity): index blobs "peers" /
    // "pr_idx" list per-peer "pr_<hex>" entries. Hops marked invalid, rssi 0.
    mesh_peers_load_all();
    ESP_LOGI("LoRaMesh", "NodeDB ready: %u/%u peers, %uB/record, %uB RAM, persist=%u",
             (unsigned)s_ncount, (unsigned)LORA_MESH_NODES_MAX,
             (unsigned)sizeof(lora_mesh_node_t),
             (unsigned)(sizeof(*s_nodes) * LORA_MESH_NODES_MAX),
             (unsigned)LORA_MESH_PERSIST_MAX);
}

void lora_mesh_set_hop_limit(uint8_t hop_limit) {
    if (hop_limit >= 1 && hop_limit <= 7) s_hop_limit = hop_limit;
}

void lora_mesh_set_role(int role) {
    if (role == LORA_MESH_ROLE_CLIENT || role == LORA_MESH_ROLE_CLIENT_MUTE ||
        role == LORA_MESH_ROLE_ROUTER) s_role = role;
    else if (role >= 0 && role <= 12) s_role = role; // passthrough DeviceConfig.Role
}
int lora_mesh_get_role(void) { return s_role; }
void lora_mesh_set_rebroadcast_mode(int mode) {
    if (mode >= LORA_MESH_RB_ALL && mode <= LORA_MESH_RB_CORE_ONLY)
        s_rebroadcast_mode = mode;
}
int lora_mesh_get_rebroadcast_mode(void) { return s_rebroadcast_mode; }

bool lora_mesh_should_decrement_hop(uint8_t hops_away) {
    (void)hops_away;
    // Without a relay_node identity there is no safe favorite-router
    // exception. In particular, native always decrements the first hop.
    return true;
}

static bool route_should_decrement_hop(uint8_t hops_away, uint8_t relay_node) {
    if (hops_away == 0) return true;
    bool local_router = s_role == LORA_MESH_ROLE_ROUTER ||
                        s_role == LORA_MESH_ROLE_ROUTER_LATE ||
                        s_role == LORA_MESH_ROLE_CLIENT_BASE;
    if (!local_router) return true;
    for (uint16_t i = 0; i < s_ncount; i++) {
        bool peer_router = s_nodes[i].role == LORA_MESH_ROLE_ROUTER ||
                           s_nodes[i].role == LORA_MESH_ROLE_ROUTER_LATE ||
                           s_nodes[i].role == LORA_MESH_ROLE_CLIENT_BASE;
        if (s_nodes[i].favorite && s_nodes[i].has_user && peer_router &&
            (uint8_t)(s_nodes[i].node_num & 0xFF) == relay_node)
            return false;
    }
    return true;
}

uint8_t lora_mesh_channel_for_dest(uint32_t to, uint8_t requested) {
    if (requested >= LORA_CH_MAX) requested = lora_channel_primary();
    // Router::sendLocal resolves an unset/default channel (wire value zero)
    // through the destination's NodeDB entry for unicasts. This matters for
    // contacts learned on a secondary channel; broadcasts stay on primary.
    if (requested == 0 && to != 0 && to != LORA_MESH_BROADCAST) {
        for (uint16_t i = 0; i < s_ncount; i++) {
            if (s_nodes[i].node_num != to) continue;
            uint8_t peer_channel = s_nodes[i].channel;
            const lora_channel_t *channel = peer_channel < LORA_CH_MAX
                                                ? lora_channel_get(peer_channel)
                                                : NULL;
            if (channel && channel->used &&
                channel->role != LORA_CH_DISABLED)
                return peer_channel;
            break;
        }
    }
    return requested;
}

void lora_mesh_set_position_precision(int bits) {
    if (bits == 32 || bits == 24 || bits == 16) s_pos_precision_bits = bits;
}
static uint32_t pos_mask_with(uint32_t v, int bits) {
    // Originator-only bitmask before encode (config Precision). 32-bit full
    // is a no-op; lower precisions zero low bits (upstream behavior).
    if (bits >= 32) return v;
    int drop = 32 - bits;
    if (drop <= 0 || drop >= 32) return v;
    return (v >> drop) << drop;
}
static uint32_t pos_mask_apply(uint32_t v) {
    return pos_mask_with(v, s_pos_precision_bits);
}
// Effective precision for one originator TX: per-channel
// ChannelSettings.module_settings (ModuleSettings.position_precision=1,
// varint bits) wins when present and valid (16/24/32); otherwise the global
// s_pos_precision_bits (MQTT map_report / last-applied mod) applies.
// Default 32 = full = no-op. Read via lora_channel_get_mod() so this file
// never touches channel internals directly.
static int pos_effective_bits(uint8_t channel_idx) {
    if (channel_idx < LORA_CH_MAX) {
        uint8_t mod[LORA_CH_MOD_MAX];
        uint16_t mlen = lora_channel_get_mod(channel_idx, mod, sizeof(mod));
        uint16_t i = 0;
        while (i < mlen) {
            uint32_t tag = 0;
            uint8_t shift = 0;
            bool tag_ok = false;
            while (i < mlen) {
                uint8_t b = mod[i++];
                tag |= (uint32_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80) || shift >= 28) { tag_ok = true; break; }
            }
            if (!tag_ok) break;
            uint8_t field = (uint8_t)(tag >> 3);
            uint8_t wire = (uint8_t)(tag & 7);
            if (wire == 0) {
                uint32_t v = 0;
                shift = 0;
                bool v_ok = false;
                while (i < mlen) {
                    uint8_t b = mod[i++];
                    v |= (uint32_t)(b & 0x7F) << shift;
                    shift += 7;
                    if (!(b & 0x80) || shift >= 28) { v_ok = true; break; }
                }
                if (!v_ok) break;
                if (field == 1 && (v == 32 || v == 24 || v == 16)) return (int)v;
            } else if (wire == 5) {
                if (i + 4 > mlen) break;
                i += 4;
            } else if (wire == 1) {
                if (i + 8 > mlen) break;
                i += 8;
            } else if (wire == 2) {
                uint32_t n = 0;
                shift = 0;
                bool n_ok = false;
                while (i < mlen) {
                    uint8_t b = mod[i++];
                    n |= (uint32_t)(b & 0x7F) << shift;
                    shift += 7;
                    if (!(b & 0x80) || shift >= 28) { n_ok = true; break; }
                }
                if (!n_ok || i + n > mlen) break;
                i += (uint16_t)n;
            } else {
                break; // groups / reserved: stop, don't misparse
            }
        }
    }
    return s_pos_precision_bits;
}
// In-place mask of Position lat_i/lon_i inside a raw Position submessage.
// Field-encoding assumption (documented): upstream mesh.proto Position
// latitude_i=1 / longitude_i=2 are sfixed32 (wire 5, tags 0x0D/0x15),
// wire-identical to fixed32 LE u32/i32. Tree has no local Position builder
// (grep 2026-09-07: Position bytes are opaque via s_self_pos / Data field 2
// in lora_mesh.c, lora_manager.c, lora_phoneapi.c; pb_w_fixed32/pb_r_next
// wire-5 handling in lora_pb.c is the only fixed32 path), so wire-5 LE
// masking is the consistent decode. Clearing low bits of the u32 pattern
// preserves sign for negative coords. Other fields (altitude varint,
// time fixed32, etc.) are skipped generically. Returns true when any byte
// changed. 32-bit (or invalid) bits is a no-op returning false.
static bool pos_payload_mask(uint8_t *p, uint16_t len, int bits) {
    if (!p || len == 0 || bits >= 32) return false;
    int drop_chk = 32 - bits;
    if (drop_chk <= 0 || drop_chk >= 32) return false;
    bool changed = false;
    uint16_t pos = 0;
    while (pos < len) {
        uint32_t tag = 0;
        uint8_t shift = 0;
        uint16_t tpos = pos;
        bool tag_ok = false;
        for (int k = 0; k < 5; k++) {
            if (tpos >= len) break;
            uint8_t b = p[tpos++];
            tag |= (uint32_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) { tag_ok = true; break; }
            if (shift >= 32) break;
        }
        if (!tag_ok) break;
        pos = tpos;
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 7);
        if ((field == 1 || field == 2) && wire == 5) {
            if (pos + 4 > len) break;
            uint32_t v = (uint32_t)p[pos] | ((uint32_t)p[pos + 1] << 8) |
                         ((uint32_t)p[pos + 2] << 16) | ((uint32_t)p[pos + 3] << 24);
            // Route through the public mask fns when the effective precision
            // equals the global so grep shows TX callers for
            // lora_mesh_position_mask_lat/lon; per-channel overrides use the
            // shared param helper directly (same bit-clear semantics).
            uint32_t m;
            if (bits == s_pos_precision_bits) {
                m = (field == 1) ? lora_mesh_position_mask_lat(v)
                                 : lora_mesh_position_mask_lon(v);
            } else {
                m = pos_mask_with(v, bits);
            }
            if (m != v) {
                p[pos] = (uint8_t)m;
                p[pos + 1] = (uint8_t)(m >> 8);
                p[pos + 2] = (uint8_t)(m >> 16);
                p[pos + 3] = (uint8_t)(m >> 24);
                changed = true;
            }
            pos += 4;
        } else if (wire == 0) {
            while (pos < len && (p[pos] & 0x80)) pos++;
            if (pos < len) pos++;
            else break;
        } else if (wire == 5) {
            if (pos + 4 > len) break;
            pos += 4;
        } else if (wire == 1) {
            if (pos + 8 > len) break;
            pos += 8;
        } else if (wire == 2) {
            uint32_t n = 0;
            shift = 0;
            bool n_ok = false;
            while (pos < len) {
                uint8_t b = p[pos++];
                n |= (uint32_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80)) { n_ok = true; break; }
                if (shift >= 28) break;
            }
            if (!n_ok || pos + n > len) break;
            pos += (uint16_t)n;
        } else {
            break;
        }
    }
    return changed;
}
uint32_t lora_mesh_position_mask_lat(uint32_t lat_i) { return pos_mask_apply(lat_i); }
uint32_t lora_mesh_position_mask_lon(uint32_t lon_i) { return pos_mask_apply(lon_i); }

void lora_mesh_set_neighborinfo_enabled(bool en) { s_neighborinfo_enabled = en; }
bool lora_mesh_neighborinfo_enabled(void) { return s_neighborinfo_enabled; }

// Own position as last sent on air (raw Position submessage bytes, capped).
// Stored when the phone addresses position to self (sendLocal) so NodeInfo
// can advertise it and the packet can be rebroadcast to BROADCAST instead
// of putting dest=self on air.
static uint8_t s_self_pos[64];
static uint8_t s_self_pos_len = 0;
void lora_mesh_set_self_position(const uint8_t *raw, uint16_t len) {
    if (!raw || len == 0) {
        // NULL/0 clears the store (phone remove_fixed_position path).
        s_self_pos_len = 0;
        memset(s_self_pos, 0, sizeof(s_self_pos));
        return;
    }
    uint16_t c = len < sizeof(s_self_pos) ? len : sizeof(s_self_pos);
    memcpy(s_self_pos, raw, c);
    s_self_pos_len = (uint8_t)c;
}
uint16_t lora_mesh_get_self_position(uint8_t *out, uint16_t cap) {
    if (!out || cap == 0 || s_self_pos_len == 0) return 0;
    uint16_t c = s_self_pos_len < cap ? s_self_pos_len : cap;
    memcpy(out, s_self_pos, c);
    return c;
}

float lora_duty_used_pct(void) {
    float duty = lora_region_duty((int)s_duty_region);
    if (duty >= 100) return 0.0f;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t allow = (uint32_t)(duty / 100.0f * 3600000.0f);
    if (!allow) return 100.0f;
    return (float)duty_used_ms(now) * 100.0f / (float)allow;
}

// ---- Native-style radio scheduling ----
// v2.7.26 uses CWmin=3/CWmax=8 and a slot of 2.5 symbols + 7.6ms on
// sub-GHz radios. Keep the math integer-only with the same truncation as
// RadioInterface::computeSlotTimeMsec().
#define ROUTE_CW_MIN 3u
#define ROUTE_CW_MAX 8u
#define ROUTE_PROCESSING_MS 4500u
static uint32_t route_slot_ms(void) {
    uint32_t bw = s_modem_bw_khz > 0 ? (uint32_t)s_modem_bw_khz : 250u;
    uint32_t sf = (s_modem_sf >= 5 && s_modem_sf <= 12) ? (uint32_t)s_modem_sf : 11u;
    uint64_t scaled = 5ull * (1ull << sf) * 1000ull + 15200ull * bw;
    return (uint32_t)(scaled / (2000ull * bw));
}
static uint8_t route_cw_for_snr(float snr) {
    if (snr <= -20.0f) return ROUTE_CW_MIN;
    if (snr >= 10.0f) return ROUTE_CW_MAX;
    return (uint8_t)(ROUTE_CW_MIN + (uint8_t)(((snr + 20.0f) * 5.0f) / 30.0f));
}
static uint32_t route_flood_delay_ms(float snr) {
    uint32_t slot = route_slot_ms();
    uint8_t cw = route_cw_for_snr(snr);
    uint32_t count;
    uint32_t offset = 0;
    if (s_role == LORA_MESH_ROLE_ROUTER) {
        count = 2u * cw;
    } else {
        offset = 2u * ROUTE_CW_MAX * slot;
        count = 1u << cw;
    }
    if (count == 0) count = 1;
    return offset + (esp_random() % count) * slot;
}
static uint32_t route_retry_delay_ms(uint8_t frame_len) {
    // Native maps live channel utilization into the contention window. We
    // do not yet maintain that separate metric, so use CWmin (idle channel),
    // while preserving the exact airtime/contention/processing formula.
    uint32_t slot = route_slot_ms();
    uint32_t contention = (1u << ROUTE_CW_MIN) + 2u * ROUTE_CW_MAX +
                          (1u << ((ROUTE_CW_MAX + ROUTE_CW_MIN) / 2u));
    return 2u * airtime_ms(frame_len) + contention * slot + ROUTE_PROCESSING_MS;
}

// ReliableRouter keeps multiple pending packets. Eight bounded entries cost
// about 2KiB and avoid the old behavior where each new DM overwrote the one
// retry slot. NUM_RELIABLE_RETX=3 means initial TX plus two retries.
#define RELIABLE_N 8
#define RELIABLE_RETRIES 2u
typedef struct {
    bool active;
    uint8_t retries_left;
    uint8_t frame[252];
    uint8_t flen;
    uint32_t from;
    uint32_t to;
    uint32_t id;
    uint32_t deadline_ms;
} reliable_entry_t;
static reliable_entry_t s_rel[RELIABLE_N];
static portMUX_TYPE s_rel_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_route_relay_sent = 0;
static uint32_t s_route_failed = 0;

// Compact runtime next-hop cache. Official firmware stores this on NodeDB
// records; keeping it separate preserves the 84-byte/200-node RAM target.
#define NEXT_HOP_N 16
#define NEXT_HOP_MAX_AGE_MS (24u * 3600u * 1000u)
typedef struct {
    uint32_t dest;
    uint32_t learned_ms;
    uint8_t hop;
} next_hop_entry_t;
static next_hop_entry_t s_next_hops[NEXT_HOP_N];
static uint8_t route_next_hop(uint32_t dest, uint8_t avoid) {
    uint8_t hop = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < NEXT_HOP_N; i++) {
        if (s_next_hops[i].dest == dest && s_next_hops[i].hop != avoid &&
            now - s_next_hops[i].learned_ms < NEXT_HOP_MAX_AGE_MS) {
            hop = s_next_hops[i].hop;
            break;
        }
    }
    portEXIT_CRITICAL(&s_rel_mux);
    return hop;
}
static void route_learn(uint32_t dest, uint8_t hop) {
    if (dest < 4 || hop == 0 || hop == (uint8_t)(s_node & 0xFF)) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int slot = -1;
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < NEXT_HOP_N; i++) {
        if (s_next_hops[i].dest == dest) { slot = i; break; }
        if (slot < 0 && s_next_hops[i].dest == 0) slot = i;
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < NEXT_HOP_N; i++)
            if ((int32_t)(s_next_hops[i].learned_ms - s_next_hops[slot].learned_ms) < 0)
                slot = i;
    }
    s_next_hops[slot].dest = dest;
    s_next_hops[slot].hop = hop;
    s_next_hops[slot].learned_ms = now;
    portEXIT_CRITICAL(&s_rel_mux);
    ESP_LOGI("LoRaMesh", "next-hop learned dest=%08x via=%02x",
             (unsigned)dest, (unsigned)hop);
}
static void route_clear(uint32_t dest) {
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < NEXT_HOP_N; i++)
        if (s_next_hops[i].dest == dest) memset(&s_next_hops[i], 0, sizeof(s_next_hops[i]));
    portEXIT_CRITICAL(&s_rel_mux);
}

static void reliable_track_with(const uint8_t *frame, uint8_t len,
                                uint8_t retries, bool origin_only) {
    if (!frame || len < MESH_HDR_LEN) return;
    uint32_t to = 0, from = 0, id = 0;
    uint8_t hl = 0, hs = 0, ch = 0;
    bool wa = false;
    if (!hdr_parse(frame, len, &to, &from, &id, &hl, &hs, &wa, &ch)) return;
    if (to == LORA_MESH_BROADCAST || to == 0) return;
    if (origin_only) {
        if (!wa || from != s_node) return;
    } else if (from == s_node || frame[14] == 0 || (hl == 0 && !wa)) {
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int slot = -1;
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < RELIABLE_N; i++) {
        if (s_rel[i].active && s_rel[i].id == id && s_rel[i].from == from) {
            slot = i;
            break;
        }
        if (slot < 0 && !s_rel[i].active) slot = i;
    }
    if (slot >= 0) {
        reliable_entry_t *r = &s_rel[slot];
        r->active = true;
        r->retries_left = retries;
        r->from = from;
        r->to = to;
        r->id = id;
        r->flen = len;
        memcpy(r->frame, frame, len);
        r->deadline_ms = now + route_retry_delay_ms(len);
    }
    portEXIT_CRITICAL(&s_rel_mux);
    if (slot < 0) {
        s_route_failed++;
        ESP_LOGW("LoRaMesh", "reliable queue full; no retries for to=%08x id=%08x",
                 (unsigned)to, (unsigned)id);
    }
}
void lora_mesh_reliable_track(const uint8_t *frame, uint8_t len) {
    reliable_track_with(frame, len, RELIABLE_RETRIES, true);
}
void lora_mesh_reliable_on_air_ack(uint32_t request_id, uint32_t from,
                                   uint8_t relay_node) {
    uint32_t dest = 0;
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < RELIABLE_N; i++) {
        if (s_rel[i].active && request_id == s_rel[i].id &&
            s_rel[i].to == from) {
            dest = from;
            s_rel[i].active = false;
        }
    }
    portEXIT_CRITICAL(&s_rel_mux);
    if (dest && relay_node) route_learn(dest, relay_node);
}
void lora_mesh_reliable_on_heard(uint32_t from, uint32_t id) {
    // Implicit ACK: hearing a later relay copy of the same origin/id stops
    // either an originator or intermediate next-hop retransmission.
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < RELIABLE_N; i++)
        if (s_rel[i].active && from == s_rel[i].from && id == s_rel[i].id)
            s_rel[i].active = false;
    portEXIT_CRITICAL(&s_rel_mux);
}

// ---- Delayed flood queue (dupe cancel + hop upgrade) ----
#define FLOODQ_N 8
static struct {
    bool active;
    uint8_t cad_deferrals;
    uint32_t from;
    uint32_t id;
    uint8_t rx_hl; // source copy hop_limit, before our decrement
    uint32_t due_ms;
    uint8_t frame[252];
    uint8_t flen;
} s_floodq[FLOODQ_N];
static bool floodq_put(uint32_t from, uint32_t id, uint8_t hl, float snr,
                       const uint8_t *f, uint8_t len) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int slot = -1;
    for (int i = 0; i < FLOODQ_N; i++) if (!s_floodq[i].active) { slot = i; break; }
    if (slot < 0) {
        s_route_failed++;
        ESP_LOGW("LoRaMesh", "flood queue full; drop from=%08x id=%08x",
                 (unsigned)from, (unsigned)id);
        return false;
    }
    s_floodq[slot].active = true;
    s_floodq[slot].cad_deferrals = 0;
    s_floodq[slot].from = from;
    s_floodq[slot].id = id;
    s_floodq[slot].rx_hl = hl;
    s_floodq[slot].due_ms = now + route_flood_delay_ms(snr);
    s_floodq[slot].flen = len > sizeof(s_floodq[slot].frame) ? sizeof(s_floodq[slot].frame) : len;
    memcpy(s_floodq[slot].frame, f, s_floodq[slot].flen);
    return true;
}
// Returns true when the duplicate was absorbed (caller must not forward).
// A greater hop_limit is the better copy; v2.7.26 replaces the queued copy
// before applying ordinary duplicate cancellation.
static bool floodq_on_duplicate(uint32_t from, uint32_t id, uint8_t hl,
                                float snr, const uint8_t *frame, uint8_t len) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (int i = 0; i < FLOODQ_N; i++) {
        if (!s_floodq[i].active) continue;
        if (s_floodq[i].from != from || s_floodq[i].id != id) continue;
        if (hl > s_floodq[i].rx_hl) {
            uint8_t n = len > sizeof(s_floodq[i].frame) ? sizeof(s_floodq[i].frame) : len;
            memcpy(s_floodq[i].frame, frame, n);
            uint8_t hs = (frame[12] >> 5) & 0x07;
            uint8_t new_hl = hl;
            uint8_t hops_away = hs >= hl ? (uint8_t)(hs - hl) : 0;
            if (route_should_decrement_hop(hops_away, frame[15]) && new_hl > 0)
                new_hl--;
            s_floodq[i].frame[12] = (uint8_t)((new_hl & 0x07) |
                                              (s_floodq[i].frame[12] & 0xF8));
            uint32_t dest = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
                            ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24);
            s_floodq[i].frame[14] = route_next_hop(dest, frame[15]);
            s_floodq[i].frame[15] = (uint8_t)(s_node & 0xFF);
            s_floodq[i].flen = n;
            s_floodq[i].rx_hl = hl;
            s_floodq[i].due_ms = now + route_flood_delay_ms(snr);
            s_floodq[i].cad_deferrals = 0;
            ESP_LOGD("LoRaMesh", "flood upgraded from=%08x id=%08x hl=%u",
                     (unsigned)from, (unsigned)id, (unsigned)hl);
            return true;
        }
        bool can_cancel = s_role != LORA_MESH_ROLE_ROUTER &&
                          s_role != LORA_MESH_ROLE_ROUTER_LATE;
        if (can_cancel && s_role == LORA_MESH_ROLE_CLIENT_BASE) {
            for (uint16_t n = 0; n < s_ncount; n++) {
                if ((s_nodes[n].node_num == from ||
                     s_nodes[n].node_num == ((uint32_t)frame[0] |
                       ((uint32_t)frame[1] << 8) | ((uint32_t)frame[2] << 16) |
                       ((uint32_t)frame[3] << 24))) && s_nodes[n].favorite) {
                    can_cancel = false;
                    break;
                }
            }
        }
        if (can_cancel) s_floodq[i].active = false;
        return true;
    }
    return false;
}

void lora_mesh_cancel_rebroadcast(uint32_t from, uint32_t id) {
    if (from == 0 || id == 0) return;
    for (int i = 0; i < FLOODQ_N; i++) {
        if (s_floodq[i].active && s_floodq[i].from == from &&
            s_floodq[i].id == id) {
            s_floodq[i].active = false;
            ESP_LOGD("LoRaMesh", "pending relay canceled by reply from=%08x id=%08x",
                     (unsigned)from, (unsigned)id);
        }
    }
}

uint32_t lora_mesh_relay_sent(void) { return s_route_relay_sent; }
uint32_t lora_mesh_relay_failed(void) { return s_route_failed; }
void lora_mesh_routing_reset(void) {
    portENTER_CRITICAL(&s_rel_mux);
    memset(s_rel, 0, sizeof(s_rel));
    portEXIT_CRITICAL(&s_rel_mux);
    // Called only while the radio task is stopped or before it starts.
    memset(s_floodq, 0, sizeof(s_floodq));
}

uint32_t lora_mesh_node_num(void) {
    if (!s_init) lora_mesh_init();
    return s_node;
}

// Header: to,from,id LE u32 + flags + channel + next_hop + relay_node.
static void hdr_write(uint8_t *f, uint32_t to, uint32_t from, uint32_t id,
                      uint8_t hop_limit, uint8_t hop_start, bool want_ack,
                      uint8_t channel, uint8_t next_hop, uint8_t relay) {
    f[0] = (uint8_t)to; f[1] = (uint8_t)(to >> 8);
    f[2] = (uint8_t)(to >> 16); f[3] = (uint8_t)(to >> 24);
    f[4] = (uint8_t)from; f[5] = (uint8_t)(from >> 8);
    f[6] = (uint8_t)(from >> 16); f[7] = (uint8_t)(from >> 24);
    f[8] = (uint8_t)id; f[9] = (uint8_t)(id >> 8);
    f[10] = (uint8_t)(id >> 16); f[11] = (uint8_t)(id >> 24);
    f[12] = (uint8_t)((hop_limit & 0x07) | (want_ack ? 0x08 : 0) |
                      ((hop_start & 0x07) << 5));
    f[13] = channel;
    f[14] = next_hop;
    f[15] = relay;
}

static bool hdr_parse(const uint8_t *f, uint8_t len, uint32_t *to, uint32_t *from,
                      uint32_t *id, uint8_t *hop_limit, uint8_t *hop_start,
                      bool *want_ack, uint8_t *channel) {
    if (!f || len < MESH_HDR_LEN) return false;
    uint32_t t = (uint32_t)f[0] | ((uint32_t)f[1] << 8) |
                 ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
    uint32_t fr = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                  ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
    if (fr == 0) return false; // upstream drops senderless (spoof/admin risk)
    if (to) *to = t;
    if (from) *from = fr;
    if (id) *id = (uint32_t)f[8] | ((uint32_t)f[9] << 8) |
                  ((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 24);
    if (hop_limit) *hop_limit = f[12] & 0x07;
    if (hop_start) *hop_start = (f[12] >> 5) & 0x07;
    if (want_ack) *want_ack = (f[12] & 0x08) != 0;
    if (channel) *channel = f[13];
    return true;
}

// Data protobuf for TX: {portnum, payload}.
static uint16_t data_build(uint8_t portnum, const uint8_t *payload, uint16_t plen,
                           uint32_t request_id, bool want_response,
                           uint8_t *out, uint16_t cap) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_varint(&w, 1, portnum);
    pb_w_bytes(&w, 2, payload, plen);
    pb_w_varint(&w, 3, want_response);
    pb_w_fixed32(&w, 6, request_id);
    if (w.overflow) return 0;
    return w.len;
}

// Full stock TX: header + AES-CTR(Data). Returns total len / 0.
static uint8_t stock_tx_ch(uint32_t to, uint8_t portnum,
                           const uint8_t *payload, uint16_t plen, uint32_t request_id,
                           uint32_t packet_id, bool want_ack, bool want_response,
                           uint8_t channel_idx, int hop_limit_override,
                           uint8_t *out_frame, uint8_t out_max, uint32_t *out_id) {
    if (!s_init) lora_mesh_init();
    if (to == 0) return 0; // sendLocal: to==0 is an error (never air)
    if (plen > 233 || (uint16_t)16 + plen > out_max) return 0;
    channel_idx = lora_mesh_channel_for_dest(to, channel_idx);
    if (channel_idx >= LORA_CH_MAX) return 0;
    // Position precision (originator-only bitmask before encode, privacy
    // parity): port-3 Position payloads are masked here using the effective
    // precision (per-TX-channel module_settings position_precision when
    // present, else s_pos_precision_bits; 32 = full = no-op). Relay path
    // verification (grep 2026-09-08): lora_mesh_schedule_rebroadcast() only
    // memcpy()s the frame + mutates header hl/relay_node with ciphertext
    // untouched and never calls stock_tx_ch (callers of stock_tx_ch are
    // stock_tx/build_text_to_id_ch/build_data_ch only), so masking here is
    // originator-only by construction — relays never re-mask.
    const lora_channel_t *ch = lora_channel_get(channel_idx);
    if (!ch || !ch->used || ch->role == LORA_CH_DISABLED) return 0;
    const uint8_t *eff_payload = payload;
    uint16_t eff_plen = plen;
    uint8_t pos_masked[233];
    if (portnum == MESH_PORT_POSITION && payload && plen &&
        plen <= sizeof(pos_masked)) {
        int eff_bits = pos_effective_bits(channel_idx);
        if (eff_bits < 32) {
            memcpy(pos_masked, payload, plen);
            bool touched = pos_payload_mask(pos_masked, plen, eff_bits);
            eff_payload = pos_masked;
            ESP_LOGD("LoRaMesh", "position precision %d masked on ch %u (touched=%u)",
                     eff_bits, (unsigned)channel_idx, (unsigned)touched);
        }
    }
    uint8_t data[233];
    uint16_t dlen = data_build(portnum, eff_payload, eff_plen, request_id, want_response,
                               data, sizeof(data));
    if (!dlen || (uint16_t)MESH_HDR_LEN + dlen > out_max) return 0;
    uint32_t id = packet_id;
    while (id == 0) id = esp_random(); // CLI/device packets use random ids
    uint8_t *f = out_frame;
    // Official ReliableRouter tracks broadcast reliability locally, then
    // clears WANT_ACK before putting a broadcast on air. Rebroadcast copies
    // therefore arrive with WANT_ACK clear and serve as the implicit ACK.
    bool air_want_ack = want_ack && to != LORA_MESH_BROADCAST;
    // sendLocal: hop_limit==0 + want_ack implies the phone left hops unset;
    // default to 3 (upstream default) so reliable unicasts still route.
    bool explicit_hops = hop_limit_override >= 0 && hop_limit_override <= 7;
    uint8_t eff_hl = explicit_hops ? (uint8_t)hop_limit_override : s_hop_limit;
    if (!explicit_hops && eff_hl == 0 && air_want_ack) eff_hl = 3;
    uint8_t next_hop = to == LORA_MESH_BROADCAST ? 0 : route_next_hop(to, 0);
    hdr_write(f, to, s_node, id, eff_hl, eff_hl,
              air_want_ack, ch->hash, next_hop, (uint8_t)(s_node & 0xFF));
    memcpy(&f[16], data, dlen);
    uint8_t key[32];
    uint8_t klen = lora_channel_key(channel_idx, key);
    if (klen) {
        lora_crypto_crypt_key(key, klen, s_node, id, &f[16], dlen);
        memset(key, 0, sizeof(key));
    }
    seen_mark(s_node, id);
    if (out_id) *out_id = id;
    return (uint8_t)(16 + dlen);
}

static uint8_t stock_tx(uint32_t to, uint8_t portnum,
                         const uint8_t *payload, uint16_t plen, uint32_t request_id,
                         uint32_t packet_id, bool want_ack, bool want_response,
                         uint8_t *out_frame, uint8_t out_max, uint32_t *out_id) {
    return stock_tx_ch(to, portnum, payload, plen, request_id, packet_id,
                       want_ack, want_response, lora_channel_primary(), -1,
                       out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_text(const char *text, uint8_t *out_frame, uint8_t out_max) {
    return lora_mesh_build_text_to(text, LORA_MESH_BROADCAST, out_frame, out_max, NULL);
}

uint8_t lora_mesh_build_text_to(const char *text, uint32_t to,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id) {
    return lora_mesh_build_text_to_id(text, to, 0, false,
                                      out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_text_to_id(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint8_t *out_frame, uint8_t out_max,
                                   uint32_t *out_id) {
    return lora_mesh_build_text_to_id_ch(text, to, packet_id, want_ack,
                                         lora_channel_primary(),
                                         out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_text_to_id_ch(const char *text, uint32_t to,
                                      uint32_t packet_id, bool want_ack,
                                      uint8_t channel_idx,
                                      uint8_t *out_frame, uint8_t out_max,
                                      uint32_t *out_id) {
    if (!text || !out_frame) return 0;
    size_t tlen = strlen(text);
    if (tlen > LORA_MESH_TEXT_MAX) tlen = LORA_MESH_TEXT_MAX;
    return stock_tx_ch(to, 1 /*TEXT_MESSAGE_APP*/, (const uint8_t *)text,
                       (uint16_t)tlen, 0, packet_id, want_ack,
                       false, channel_idx, -1,
                       out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_data_ch(uint32_t to, uint8_t portnum,
                                const uint8_t *payload, uint16_t plen,
                                uint32_t request_id, uint32_t packet_id,
                                bool want_ack, bool want_response,
                                uint8_t channel_idx,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id) {
    if (!payload && plen) return 0;
    if (plen > 233 || !out_frame) return 0;
    return stock_tx_ch(to, portnum, payload, plen, request_id, packet_id,
                       want_ack, want_response, channel_idx, -1,
                       out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_data_verbatim_ch(uint32_t to,
                                         const uint8_t *data, uint16_t dlen,
                                         uint32_t packet_id, bool want_ack,
                                         uint8_t channel_idx,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id) {
    if (!s_init) lora_mesh_init();
    if (to == 0 || !data || dlen == 0 || dlen > 233 || !out_frame ||
        (uint16_t)MESH_HDR_LEN + dlen > out_max) return 0;
    channel_idx = lora_mesh_channel_for_dest(to, channel_idx);
    if (channel_idx >= LORA_CH_MAX) return 0;
    const lora_channel_t *ch = lora_channel_get(channel_idx);
    if (!ch || !ch->used || ch->role == LORA_CH_DISABLED) return 0;
    uint32_t id = packet_id;
    while (id == 0) id = esp_random();
    bool air_want_ack = want_ack && to != LORA_MESH_BROADCAST;
    uint8_t eff_hl = s_hop_limit;
    if (eff_hl == 0 && air_want_ack) eff_hl = 3;
    uint8_t next_hop = to == LORA_MESH_BROADCAST ? 0 : route_next_hop(to, 0);
    hdr_write(out_frame, to, s_node, id, eff_hl, eff_hl,
              air_want_ack, ch->hash, next_hop, (uint8_t)(s_node & 0xFF));
    memcpy(&out_frame[MESH_HDR_LEN], data, dlen);
    uint8_t key[32];
    uint8_t klen = lora_channel_key(channel_idx, key);
    if (klen) {
        lora_crypto_crypt_key(key, klen, s_node, id,
                             &out_frame[MESH_HDR_LEN], dlen);
        memset(key, 0, sizeof(key));
    }
    seen_mark(s_node, id);
    if (out_id) *out_id = id;
    return (uint8_t)(MESH_HDR_LEN + dlen);
}

uint8_t lora_mesh_build_dm_data(uint32_t to,
                                const uint8_t *data, uint16_t dlen,
                                uint32_t packet_id, bool want_ack,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id) {
    if (!s_init) lora_mesh_init();
    if (to == LORA_MESH_BROADCAST || to == 0 || !data || dlen == 0 ||
        !out_frame) return 0;
    // PKI adds a 4-byte extra nonce and 8-byte authentication tag.  The
    // caller's output cap is the authoritative radio-frame limit.
    if (dlen > 233 || (uint16_t)MESH_HDR_LEN + dlen + LORA_PKI_OVERHEAD > out_max)
        return 0;
    uint8_t peer[32];
    if (!lora_mesh_peer_pubkey(to, peer)) return 0;
    uint32_t id = packet_id;
    while (id == 0) id = esp_random();
    uint8_t ct[233 + 12];
    uint16_t ct_len = lora_pki_encrypt(to, s_node, peer, id, data, dlen,
                                       ct, sizeof(ct));
    memset(peer, 0, sizeof(peer));
    if (!ct_len || (uint16_t)MESH_HDR_LEN + ct_len > out_max) {
        memset(ct, 0, sizeof(ct));
        return 0;
    }
    bool air_want_ack = want_ack; // DMs are unicast: keep WANT_ACK on air
    // Upstream perhapsEncode: PKI frames carry channel byte 0 (the receiver
    // tries PKI first only when channel == 0). Never the primary hash.
    uint8_t dm_hl = s_hop_limit ? s_hop_limit : 3;
    uint8_t next_hop = route_next_hop(to, 0);
    hdr_write(out_frame, to, s_node, id, dm_hl, dm_hl,
              air_want_ack, 0, next_hop, (uint8_t)(s_node & 0xFF));
    memcpy(&out_frame[16], ct, ct_len);
    memset(ct, 0, sizeof(ct));
    seen_mark(s_node, id);
    if (out_id) *out_id = id;
    return (uint8_t)(16 + ct_len);
}

uint8_t lora_mesh_build_dm(uint32_t to, uint8_t portnum,
                           const uint8_t *payload, uint16_t plen,
                           uint32_t request_id, uint32_t packet_id,
                           bool want_ack, bool want_response,
                           uint8_t *out_frame, uint8_t out_max,
                           uint32_t *out_id) {
    if (!payload && plen) return 0;
    // The serialized Data plus PKI overhead must fit the caller's frame.
    if (plen > 200) return 0;
    uint8_t data[233];
    uint16_t dlen = data_build(portnum, payload, plen, request_id, want_response,
                               data, sizeof(data));
    if (!dlen) return 0;
    uint8_t n = lora_mesh_build_dm_data(to, data, dlen, packet_id, want_ack,
                                        out_frame, out_max, out_id);
    memset(data, 0, sizeof(data));
    return n;
}

uint8_t lora_mesh_build_dm_text(const char *text, uint32_t to,
                                uint32_t packet_id, bool want_ack,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id) {
    if (!text || !out_frame) return 0;
    size_t tlen = strlen(text);
    if (tlen > 160) tlen = 160;
    // PKI overhead caps DM text below the 233B Data limit.
    if (tlen > 200) tlen = 200;
    return lora_mesh_build_dm(to, 1, (const uint8_t *)text, (uint16_t)tlen,
                              0, packet_id, want_ack, false,
                              out_frame, out_max, out_id);
}

// Bounded hex encoder for PKI RX diagnostics (logging only).
// Writes up to (cap-1)/2 bytes as uppercase hex, always NUL-terminated.
static void pki_frame_hex(const uint8_t *p, uint16_t n, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!p) return;
    size_t pos = 0;
    for (uint16_t i = 0; i < n && pos + 2 < cap; i++) {
        int w = snprintf(out + pos, cap - pos, "%02X", p[i]);
        if (w != 2) break;
        pos += (size_t)w;
    }
}

uint16_t lora_mesh_try_pki(const uint8_t *frame, uint8_t len,
                           uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                           uint8_t *out_data, uint16_t data_cap,
                           uint8_t *out_port, bool *out_want_ack) {
    uint32_t to = 0, from = 0, id = 0;
    bool want_ack = false;
    uint8_t ch = 0;
    if (!hdr_parse(frame, len, &to, &from, &id, NULL, NULL, &want_ack, &ch))
        return 0;
    // Inbound-PKI diagnostics below fire only for frames addressed to self
    // (unicast DM shape); flood/overheard traffic stays silent (rate-safe).
    // Logging only: every return value is unchanged.
    if (ch != 0 || to != lora_mesh_node_num() ||
        from == lora_mesh_node_num()) return 0;
    uint16_t ct_len = (uint16_t)(len - MESH_HDR_LEN);
    if (ct_len <= LORA_PKI_OVERHEAD || ct_len > 233 + 12) {
        ESP_LOGW("LoRaMesh", "PKI DM short from=%08x id=%08x ch=%02x len=%u",
                 (unsigned)from, (unsigned)id, (unsigned)ch, (unsigned)len);
        return 0;
    }
    uint8_t peer[32];
    if (!lora_mesh_peer_pubkey(from, peer)) {
        ESP_LOGW("LoRaMesh", "PKI DM no-peer-key from=%08x ch=%02x id=%08x",
                 (unsigned)from, (unsigned)ch, (unsigned)id);
        return 0;
    }
    static uint8_t pt[245];
    uint16_t pt_len = lora_pki_decrypt(from, peer, id, &frame[MESH_HDR_LEN],
                                       ct_len, pt, sizeof(pt));
    memset(peer, 0, sizeof(peer));
    if (!pt_len) {
        // Inbound-CCM mystery diagnostic (logging only, no behavior change):
        // dump the FULL RX frame (header+ct, <=255B) as hex in 2 bounded
        // chunks plus id/from/extra-nonce so stock's exact bytes can be
        // verified offline (nonce = [pktId u64 LE][from u32 LE] with
        // extra-nonce overwriting bytes [4..7]; CCM uses first 13B; the
        // extra-nonce u32 is the LE tail 4B of the ciphertext).
        uint32_t extra = 0;
        if (ct_len >= 4) {
            const uint8_t *tail = &frame[MESH_HDR_LEN + ct_len - 4];
            extra = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8) |
                    ((uint32_t)tail[2] << 16) | ((uint32_t)tail[3] << 24);
        }
        ESP_LOGW("LoRaMesh", "PKI DM decrypt-fail from=%08x id=%08x ch=%02x ct=%u extra=%08x len=%u",
                 (unsigned)from, (unsigned)id, (unsigned)ch, (unsigned)ct_len,
                 (unsigned)extra, (unsigned)len);
        {
            char hx1[2 * 128 + 1];
            char hx2[2 * 127 + 1];
            uint16_t n1 = len > 128 ? 128 : len;
            uint16_t n2 = len > n1 ? (uint16_t)(len - n1) : 0;
            pki_frame_hex(frame, n1, hx1, sizeof(hx1));
            ESP_LOGW("LoRaMesh", "PKI DM rxframe0 len=%u %s", (unsigned)len, hx1);
            if (n2) {
                pki_frame_hex(frame + n1, n2, hx2, sizeof(hx2));
                ESP_LOGW("LoRaMesh", "PKI DM rxframe1 %s", hx2);
            }
        }
        return 0;
    }
    const uint8_t *pl = NULL;
    uint16_t pllen = 0;
    uint8_t port = data_parse(pt, pt_len, &pl, &pllen, NULL);
    if (!port || pt_len > data_cap || !out_data) {
        ESP_LOGW("LoRaMesh", "PKI DM data-decode-fail from=%08x id=%08x ch=%02x pt=%u",
                 (unsigned)from, (unsigned)id, (unsigned)ch, (unsigned)pt_len);
        memset(pt, 0, sizeof(pt));
        return 0;
    }
    memcpy(out_data, pt, pt_len);
    memset(pt, 0, sizeof(pt));
    if (out_from) *out_from = from;
    if (out_to) *out_to = to;
    if (out_id) *out_id = id;
    if (out_port) *out_port = port;
    if (out_want_ack) *out_want_ack = want_ack;
    ESP_LOGI("LoRaMesh", "PKI DM ok from=%08x id=%08x port=%u len=%u",
             (unsigned)from, (unsigned)id, (unsigned)port, (unsigned)pt_len);
    return pt_len;
}

// ---- PKI decrypt-variant probe (diagnostic, no RX/TX behavior change) ----
// Runs one captured frame through stock-framing variants using the stored
// peer key + our priv. Prints one ESP_LOGI line per variant: OK (port +
// payload first 32B as hex + printable ASCII) or fail (ccm rc). A variant
// "succeeds" iff CCM rc==0 AND data_parse gives port!=0. Output capped 245B.
static void pktry_one(const char *name, const uint8_t *key32,
                      const uint8_t nonce16[16],
                      const uint8_t *ct, uint16_t plen,
                      const uint8_t *tag) {
    uint8_t out[245];
    int rc = -1;
    uint8_t port = 0;
    if (key32 && nonce16 && (plen == 0 || ct) && tag && plen <= sizeof(out)) {
        mbedtls_ccm_context ctx;
        mbedtls_ccm_init(&ctx);
        rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key32, 256);
        if (rc == 0) {
            rc = mbedtls_ccm_auth_decrypt(&ctx, plen, nonce16,
                                          LORA_PKI_CCM_NONCE_LEN,
                                          NULL, 0, ct, out, tag, 8);
        }
        mbedtls_ccm_free(&ctx);
        if (rc == 0) {
            const uint8_t *pl = NULL;
            uint16_t pll = 0;
            port = data_parse(out, plen, &pl, &pll, NULL);
            if (port != 0) {
                const uint8_t *show = (pl && pll) ? pl : out;
                uint16_t show_len = (pl && pll) ? pll : plen;
                uint16_t sn = show_len < 32 ? show_len : 32;
                char hx[65] = {0};
                char asc[33] = {0};
                for (uint16_t i = 0; i < sn; i++) {
                    snprintf(hx + i * 2, 3, "%02X", show[i]);
                    asc[i] = (show[i] >= 32 && show[i] < 127)
                                 ? (char)show[i] : '.';
                }
                ESP_LOGI("LoRaMesh", "pktry %s: OK port=%u plen=%u hex=%s ascii=%s",
                         name, (unsigned)port, (unsigned)plen, hx, asc);
                memset(out, 0, sizeof(out));
                return;
            }
        }
    }
    ESP_LOGI("LoRaMesh", "pktry %s: fail ccm=%d port=%u",
             name, rc, (unsigned)port);
    memset(out, 0, sizeof(out));
}

void lora_mesh_pktry(const uint8_t *frame, uint8_t len) {
    uint32_t to = 0, from = 0, id = 0;
    uint8_t hl = 0, hs = 0, ch = 0;
    bool wa = false;
    if (!frame || !hdr_parse(frame, len, &to, &from, &id, &hl, &hs, &wa, &ch)) {
        ESP_LOGI("LoRaMesh", "pktry: bad header len=%u",
                 (unsigned)(frame ? len : 0));
        return;
    }
    (void)hl;
    (void)hs;
    (void)wa;
    ESP_LOGI("LoRaMesh", "pktry from=%08x to=%08x id=%08x ch=%02x len=%u",
             (unsigned)from, (unsigned)to, (unsigned)id,
             (unsigned)ch, (unsigned)len);
    if (len <= MESH_HDR_LEN) {
        ESP_LOGI("LoRaMesh", "pktry: short frame (no payload)");
        return;
    }
    uint8_t peer[32];
    if (!lora_mesh_peer_pubkey(from, peer)) {
        ESP_LOGI("LoRaMesh", "pktry: no peer key from=%08x", (unsigned)from);
        return;
    }
    uint8_t raw[32], std[32], dbl[32];
    if (!lora_pki_dh_raw(peer, raw)) {
        memset(peer, 0, sizeof(peer));
        ESP_LOGI("LoRaMesh", "pktry: dh fail from=%08x", (unsigned)from);
        return;
    }
    memset(peer, 0, sizeof(peer));
    memcpy(std, raw, 32);
    lora_pki_sha256_32(std);
    memcpy(dbl, std, 32);
    lora_pki_sha256_32(dbl);
    const uint8_t *p = &frame[MESH_HDR_LEN];
    uint16_t total = (uint16_t)(len - MESH_HDR_LEN);
    if (total > LORA_PKI_OVERHEAD) {
        uint16_t plen = (uint16_t)(total - LORA_PKI_OVERHEAD);
        const uint8_t *ct = p;
        const uint8_t *tag = p + plen; // [ct][tag8][extra4]
        uint32_t extra = (uint32_t)tag[8] | ((uint32_t)tag[9] << 8) |
                         ((uint32_t)tag[10] << 16) | ((uint32_t)tag[11] << 24);
        uint32_t extra_sw = (uint32_t)p[plen] | ((uint32_t)p[plen + 1] << 8) |
                            ((uint32_t)p[plen + 2] << 16) |
                            ((uint32_t)p[plen + 3] << 24);
        const uint8_t *tag_sw = p + plen + 4; // swapped tail: last 8B are tag
        uint8_t n_std[16], n_zero[16], n_be[16], n_sw[16];
        lora_pki_nonce(n_std, from, id, extra);
        lora_pki_nonce(n_zero, from, id, 0);
        memset(n_be, 0, sizeof(n_be));
        n_be[0] = (uint8_t)(id >> 24);
        n_be[1] = (uint8_t)(id >> 16);
        n_be[2] = (uint8_t)(id >> 8);
        n_be[3] = (uint8_t)id;
        n_be[4] = (uint8_t)extra;
        n_be[5] = (uint8_t)(extra >> 8);
        n_be[6] = (uint8_t)(extra >> 16);
        n_be[7] = (uint8_t)(extra >> 24);
        n_be[8] = (uint8_t)from;
        n_be[9] = (uint8_t)(from >> 8);
        n_be[10] = (uint8_t)(from >> 16);
        n_be[11] = (uint8_t)(from >> 24);
        lora_pki_nonce(n_sw, from, id, extra_sw);
        pktry_one("V0 std", std, n_std, ct, plen, tag);
        pktry_one("V1 extra0", std, n_zero, ct, plen, tag);
        pktry_one("V2 rawdh", raw, n_std, ct, plen, tag);
        pktry_one("V3 rawdh+extra0", raw, n_zero, ct, plen, tag);
        pktry_one("V4 be-id", std, n_be, ct, plen, tag);
        pktry_one("V6 dblsha", dbl, n_std, ct, plen, tag);
        pktry_one("V7 swapped", std, n_sw, ct, plen, tag_sw);
        memset(n_std, 0, sizeof(n_std));
        memset(n_zero, 0, sizeof(n_zero));
        memset(n_be, 0, sizeof(n_be));
        memset(n_sw, 0, sizeof(n_sw));
    } else {
        ESP_LOGI("LoRaMesh", "pktry V0 std: fail short");
        ESP_LOGI("LoRaMesh", "pktry V1 extra0: fail short");
        ESP_LOGI("LoRaMesh", "pktry V2 rawdh: fail short");
        ESP_LOGI("LoRaMesh", "pktry V3 rawdh+extra0: fail short");
        ESP_LOGI("LoRaMesh", "pktry V4 be-id: fail short");
        ESP_LOGI("LoRaMesh", "pktry V6 dblsha: fail short");
        ESP_LOGI("LoRaMesh", "pktry V7 swapped: fail short");
    }
    if (total > 8) {
        uint16_t plen = (uint16_t)(total - 8); // [ct][tag8], extra=0
        uint8_t n_zero[16];
        lora_pki_nonce(n_zero, from, id, 0);
        pktry_one("V8 noextra", std, n_zero, p, plen, p + plen);
        memset(n_zero, 0, sizeof(n_zero));
    } else {
        ESP_LOGI("LoRaMesh", "pktry V8 noextra: fail short");
    }
    memset(raw, 0, sizeof(raw));
    memset(std, 0, sizeof(std));
    memset(dbl, 0, sizeof(dbl));
}

uint8_t lora_mesh_build_routing_ack(uint32_t to, uint32_t request_id,
                                    uint8_t *out_frame, uint8_t out_max,
                                    uint32_t *out_id) {
    return lora_mesh_build_routing_ack_hops(to, request_id, s_hop_limit,
                                            false, out_frame, out_max, out_id);
}

uint8_t lora_mesh_build_routing_ack_hops(uint32_t to, uint32_t request_id,
                                         uint8_t ack_hops,
                                         bool want_ack_on_ack,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id) {
    // Routing{error_reason=NONE}. It is a oneof, so its zero enum value must
    // still be present on the wire (field 3, value 0).
    static const uint8_t routing_ok[] = {0x18, 0x00};
    if (!request_id || !out_frame) return 0;
    if (ack_hops > 7) ack_hops = 7;
    uint8_t saved = s_hop_limit;
    s_hop_limit = ack_hops;
    uint8_t n = stock_tx(to, 5 /*ROUTING_APP*/, routing_ok, sizeof(routing_ok),
                         request_id, 0, want_ack_on_ack, false,
                         out_frame, out_max, out_id);
    s_hop_limit = saved;
    return n;
}

uint8_t lora_mesh_build_routing_nak(uint32_t to, uint32_t request_id,
                                    uint8_t err_reason,
                                    uint8_t ack_hops,
                                    uint8_t *out_frame, uint8_t out_max,
                                    uint32_t *out_id) {
    uint8_t routing_nak[] = {0x18, err_reason};
    if (!request_id || !out_frame || err_reason == 0) return 0;
    if (ack_hops > 7) ack_hops = 7;
    uint8_t saved = s_hop_limit;
    s_hop_limit = ack_hops;
    uint8_t n = stock_tx(to, 5 /*ROUTING_APP*/, routing_nak, sizeof(routing_nak),
                         request_id, 0, false, false,
                         out_frame, out_max, out_id);
    s_hop_limit = saved;
    return n;
}

// Minimal traceroute reply: echo the inbound route payload opaquely with our
// node id appended (4B LE) as route_back, on TRACEROUTE port. Upstream
// RouteDiscovery carries route[]/route_back[]/snr arrays; we keep the
// inbound bytes verbatim and append, which stock firmware parses as extra
// route entries while remaining forward-compatible.
uint8_t lora_mesh_build_traceroute_reply(uint32_t to, uint32_t request_id,
                                         const uint8_t *rx_payload,
                                         uint16_t rx_plen,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id) {
    if (to == 0 || to == LORA_MESH_BROADCAST || !out_frame) return 0;
    uint8_t payload[233];
    uint16_t n = rx_plen > sizeof(payload) - 4 ? sizeof(payload) - 4 : rx_plen;
    if (rx_payload && n) memcpy(payload, rx_payload, n);
    uint32_t me = s_node;
    payload[n++] = (uint8_t)me; payload[n++] = (uint8_t)(me >> 8);
    payload[n++] = (uint8_t)(me >> 16); payload[n++] = (uint8_t)(me >> 24);
    return stock_tx(to, 70 /*TRACEROUTE_APP*/, payload, n, request_id,
                    0, false, false, out_frame, out_max, out_id);
}

// Traceroute request: empty RouteDiscovery payload, unicast, want_ack +
// want_response (mirrors the reply builder above; stock answers with a
// route_back reply on the same port).
uint8_t lora_mesh_build_traceroute_request(uint32_t to,
                                           uint8_t *out_frame, uint8_t out_max,
                                           uint32_t *out_id) {
    if (to == 0 || to == LORA_MESH_BROADCAST || !out_frame) return 0;
    return stock_tx(to, MESH_PORT_TRACEROUTE, NULL, 0, 0,
                    0, true, true, out_frame, out_max, out_id);
}

// User protobuf for our NodeInfo broadcasts.
static uint16_t user_build(uint8_t *out, uint16_t cap) {
    char idstr[16], lo[40], sh[8];
    snprintf(idstr, sizeof(idstr), "!%08x", (unsigned)s_node);
    snprintf(sh, sizeof(sh), "G%02X%02X",
             (unsigned)((s_node >> 8) & 0xFF), (unsigned)(s_node & 0xFF));
    // Derive short MAC suffix like the phone layer does for consistency.
    lora_mesh_owner(lo, sizeof(lo), sh, sizeof(sh));
    if (!lo[0]) snprintf(lo, sizeof(lo), "Ghost-%s", sh[0] ? sh : "????");
    if (!sh[0]) snprintf(sh, sizeof(sh), "G%02X%02X",
             (unsigned)((s_node >> 8) & 0xFF), (unsigned)(s_node & 0xFF));
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_string(&w, 1, idstr);
    // v2.7.26's wire User.long_name cap is 40 UTF-8 bytes. Our owner buffer is
    // 40 bytes including NUL, so emit at most 39 bytes and keep characters whole.
    {
        size_t n = 0, i = 0, sl = strlen(lo);
        while (i < sl && n < 39) {
            uint8_t c = (uint8_t)lo[i];
            size_t cl = 1;
            if ((c & 0x80) == 0) cl = 1;
            else if ((c & 0xE0) == 0xC0) cl = 2;
            else if ((c & 0xF0) == 0xE0) cl = 3;
            else if ((c & 0xF8) == 0xF0) cl = 4;
            if (n + cl > 39 || i + cl > sl) break;
            n += cl;
            i += cl;
        }
        pb_w_bytes(&w, 2, (const uint8_t *)lo, (uint16_t)n);
    }
    pb_w_string(&w, 3, sh);
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        pb_w_bytes(&w, 4, mac, sizeof(mac));
    pb_w_varint(&w, 5, lora_pb_local_hardware_model());
    if (s_role != LORA_MESH_ROLE_CLIENT)
        pb_w_varint(&w, 7, (uint32_t)s_role);
    const uint8_t *pub = lora_pki_public();
    pb_w_bytes(&w, 8, pub, pub ? 32 : 0); // User.public_key (DMs)
    // NOTE: our fixed position goes out via periodic port-3 broadcasts (see tick),
    // not here: air NODEINFO payload is a User message where field 3 is short_name.
    if (w.overflow) return 0;
    return w.len;
}

static bool nodeinfo_send_ch_rate(uint32_t to, bool request_replies,
                                  uint8_t channel_idx, bool interactive,
                                  uint32_t request_id, bool want_ack,
                                  int hop_limit_override) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000ULL);
    uint32_t throttle = interactive ? NODEINFO_INTERACTIVE_THROTTLE_MS
                                    : NODEINFO_NORMAL_THROTTLE_MS;
    // Match native congestion scaling above 40 recently heard nodes. Router,
    // tracker and sensor roles keep the base interval.
    if (!interactive && s_role != LORA_MESH_ROLE_ROUTER &&
        s_role != LORA_MESH_ROLE_ROUTER_LATE &&
        s_role != LORA_MESH_ROLE_TRACKER && s_role != LORA_MESH_ROLE_SENSOR &&
        s_role != LORA_MESH_ROLE_TAK_TRACKER) {
        uint16_t online = 1;
        for (uint16_t i = 0; i < s_ncount; i++) {
            if ((uint32_t)(now - s_nodes[i].last_seen_ms) <= 2u * 3600u * 1000u)
                online++;
        }
        if (online > 40) {
            float factor = (float)(1u << s_modem_sf) /
                           ((float)s_modem_bw_khz * 100.0f);
            double scaled = (double)throttle *
                            (1.0 + (double)(online - 40) * factor);
            throttle = scaled >= 2147483647.0 ? 2147483647u
                                               : (uint32_t)scaled;
        }
    }
    if (s_last_nodeinfo_tx_ms != 0 &&
        (uint32_t)(now - s_last_nodeinfo_tx_ms) < throttle) {
        ESP_LOGD("LoRaMesh", "NodeInfo throttled (%s)",
                 interactive ? "interactive" : "normal");
        return false;
    }
    const lora_channel_t *channel = lora_channel_get(channel_idx);
    if (!channel || !channel->used || channel->role == LORA_CH_DISABLED)
        return false;
    uint8_t user[144];
    uint16_t ulen = user_build(user, sizeof(user));
    if (!ulen) return false;
    uint8_t frame[255];
    uint8_t flen = stock_tx_ch(to, 4 /*NODEINFO_APP*/,
                               user, ulen, request_id, 0, want_ack, request_replies,
                               channel_idx, hop_limit_override,
                               frame, sizeof(frame), NULL);
    if (!flen || !lora_duty_allow(flen) || lora_radio_cad()) return false;
    if (lora_radio_send(frame, flen) != 0) return false;
    lora_duty_record(flen);
    if (want_ack && to != LORA_MESH_BROADCAST)
        lora_mesh_reliable_track(frame, flen);
    s_last_nodeinfo_tx_ms = now;
    // key=yes/no mirrors the User.public_key stock learns from this frame;
    // ch is the primary-channel air hash byte stock must share to hear us.
    ESP_LOGI("LoRaMesh", "NodeInfo sent to %08x (%uB, replies=%u, key=%s, ch=%02x)",
             (unsigned)to, (unsigned)flen, (unsigned)request_replies,
             lora_pki_public() ? "yes" : "no",
             (unsigned)lora_channel_hash_of(channel_idx));
    return true;
}

bool lora_mesh_send_nodeinfo_ch(uint32_t to, bool request_replies,
                                uint8_t channel_idx) {
    return nodeinfo_send_ch_rate(to, request_replies, channel_idx,
                                  request_replies, 0, false, -1);
}

bool lora_mesh_send_nodeinfo_reply(uint32_t to, uint8_t channel_idx,
                                   uint32_t request_id, bool want_ack,
                                   uint8_t hop_limit) {
    if (!request_id || hop_limit > 7) return false;
    return nodeinfo_send_ch_rate(to, false, channel_idx, false,
                                 request_id, want_ack, hop_limit);
}

bool lora_mesh_send_nodeinfo(uint32_t to, bool request_replies) {
    return lora_mesh_send_nodeinfo_ch(to, request_replies,
                                      lora_channel_primary());
}

void lora_mesh_request_nodeinfo(void) {
    // The radio task owns the actual TX. Resetting the deadline avoids doing a
    // blocking SPI/TX operation from the BLE GATT callback.
    s_next_nodeinfo_ms = 0;
    s_nodeinfo_force = true;
}

void lora_mesh_request_nodeinfo_to(uint32_t to, uint8_t channel_idx) {
    if (to < 4 || to == s_node || to == LORA_MESH_BROADCAST ||
        channel_idx >= LORA_CH_MAX) return;
    s_nodeinfo_target = to;
    s_nodeinfo_target_channel = channel_idx;
    s_nodeinfo_target_due_ms = 0;
    /* Native first sends a direct NodeInfo after PKI_UNKNOWN_PUBKEY.  A
     * broadcast fallback is useful with stock nodes that retain a stale
     * unsigned unicast NodeInfo entry after a key change; it is public key
     * material and matches the normal periodic NodeInfo announcement. */
    s_nodeinfo_key_refresh_broadcast = true;
}

static bool nodeinfo_send(bool request_replies) {
    bool interactive = request_replies;
    if (s_role == LORA_MESH_ROLE_TRACKER ||
        s_role == LORA_MESH_ROLE_SENSOR)
        request_replies = false;
    return nodeinfo_send_ch_rate(LORA_MESH_BROADCAST, request_replies,
                                  lora_channel_primary(), interactive,
                                  0, false, -1);
}

bool lora_mesh_nodeinfo_reply_allowed(uint32_t from) {
    if (from < 4 || from == s_node || from == LORA_MESH_BROADCAST) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int free_slot = -1;
    int oldest = 0;
    for (uint16_t i = 0; i < s_nodeinfo_reply_cap; i++) {
        if (s_nodeinfo_replies[i].node == from) {
            if ((uint32_t)(now - s_nodeinfo_replies[i].secs) < NODEINFO_REPLY_SUPPRESS_SECS)
                return false;
            s_nodeinfo_replies[i].secs = now;
            return true;
        }
        if (free_slot < 0 && s_nodeinfo_replies[i].node == 0) free_slot = i;
        if (s_nodeinfo_replies[i].secs < s_nodeinfo_replies[oldest].secs) oldest = i;
    }
    int slot = free_slot >= 0 ? free_slot : oldest;
    s_nodeinfo_replies[slot].node = from;
    s_nodeinfo_replies[slot].secs = now;
    return true;
}

// Generic periodic broadcast originate (position / telemetry): primary
// channel, duty + CAD gated like NodeInfo. Returns true when aired.
static bool periodic_broadcast(uint8_t portnum,
                               const uint8_t *payload, uint16_t plen) {
    if (payload == NULL && plen != 0) return false;
    uint8_t frame[255];
    uint8_t flen = stock_tx(LORA_MESH_BROADCAST, portnum, payload, plen,
                            0, 0, false, false,
                            frame, sizeof(frame), NULL);
    if (!flen || !lora_duty_allow(flen) || lora_radio_cad()) return false;
    if (lora_radio_send(frame, flen) != 0) return false;
    lora_duty_record(flen);
    return true;
}

// Minimal DeviceMetrics originate (telemetry.proto): Telemetry{
// device_metrics=3{ battery_level=1?, voltage=2?, channel_utilization=3?,
// uptime_seconds=5 } }. Battery fields only when the fuel gauge reads;
// without them the frame still carries uptime + our TX duty share so stock
// nodes list us as telemetry-capable. Hand-rolled varints via pb_w_*.
static uint16_t devmetrics_build(uint8_t *out, uint16_t cap) {
    uint8_t dm[32];
    pb_w_t w;
    pb_w_init(&w, dm, sizeof(dm));
    int pct = fuel_gauge_manager_get_percentage(); // -1 when unavailable
    if (pct >= 0 && pct <= 100) pb_w_varint(&w, 1, (uint32_t)pct);
    uint16_t mv = fuel_gauge_manager_get_voltage_mv(); // 0 when unavailable
    if (mv > 0) pb_w_float(&w, 2, (float)mv / 1000.0f);
    float duty = lora_duty_used_pct(); // our TX duty share, 0 when uncapped
    if (duty > 0.0f) pb_w_float(&w, 3, duty);
    pb_w_varint(&w, 5, (uint32_t)(esp_timer_get_time() / 1000000LL));
    if (w.overflow) return 0;
    pb_w_t t;
    pb_w_init(&t, out, cap);
    pb_w_bytes(&t, 3, dm, w.len);
    if (t.overflow) return 0;
    return t.len;
}

static uint32_t s_next_pos_ms = 0;
static uint32_t s_next_tlm_ms = 0;

static bool floodq_tick(uint32_t now) {
    int slot = -1;
    for (int i = 0; i < FLOODQ_N; i++) {
        if (s_floodq[i].active && (int32_t)(now - s_floodq[i].due_ms) >= 0) {
            if (slot < 0 || (int32_t)(s_floodq[i].due_ms - s_floodq[slot].due_ms) < 0)
                slot = i;
        }
    }
    if (slot < 0) return false;
    if (!lora_duty_allow(s_floodq[slot].flen)) {
        ESP_LOGW("LoRaMesh", "delayed flood dropped by duty guard id=%08x",
                 (unsigned)s_floodq[slot].id);
        s_floodq[slot].active = false;
        s_route_failed++;
        return false;
    }
    if (lora_radio_cad()) {
        if (++s_floodq[slot].cad_deferrals >= 8) {
            ESP_LOGW("LoRaMesh", "delayed flood abandoned on busy channel id=%08x",
                     (unsigned)s_floodq[slot].id);
            s_floodq[slot].active = false;
            s_route_failed++;
        } else {
            s_floodq[slot].due_ms = now +
                (1u + esp_random() % 4u) * route_slot_ms();
        }
        return false;
    }
    uint32_t from = s_floodq[slot].from;
    uint32_t id = s_floodq[slot].id;
    uint8_t flen = s_floodq[slot].flen;
    int rc = lora_radio_send(s_floodq[slot].frame, flen);
    s_floodq[slot].active = false;
    if (rc == 0) {
        lora_duty_record(flen);
        // A chosen-next-hop relay gets one intermediate retry (native
        // NUM_INTERMEDIATE_RETX=2: initial send plus one fallback retry).
        reliable_track_with(s_floodq[slot].frame, flen, 1u, false);
        s_route_relay_sent++;
        ESP_LOGD("LoRaMesh", "delayed flood sent from=%08x id=%08x",
                 (unsigned)from, (unsigned)id);
        return true;
    }
    s_route_failed++;
    ESP_LOGW("LoRaMesh", "delayed flood radio failure from=%08x id=%08x",
             (unsigned)from, (unsigned)id);
    return false;
}

static void reliable_max_retransmit(uint32_t to, uint32_t id) {
    lora_manager_chat_result(to, id, false);
    uint8_t routing[] = {0x18, LORA_ROUTING_MAX_RETRANSMIT};
    uint8_t data[16];
    uint16_t dl = pb_build_data_msg(data, sizeof(data), 5,
                                    routing, sizeof(routing), id);
    uint32_t aid = 0;
    while (aid == 0) aid = esp_random();
    if (dl) (void)lora_phoneapi_push_mesh_data(s_node, to, aid, data, dl);
    ESP_LOGW("LoRaMesh", "reliable MAX_RETRANSMIT to %08x id %08x",
             (unsigned)to, (unsigned)id);
}

static bool reliable_tick(uint32_t now) {
    int slot = -1;
    reliable_entry_t work;
    memset(&work, 0, sizeof(work));
    portENTER_CRITICAL(&s_rel_mux);
    for (int i = 0; i < RELIABLE_N; i++) {
        if (s_rel[i].active && (int32_t)(now - s_rel[i].deadline_ms) >= 0) {
            slot = i;
            work = s_rel[i];
            break; // at most one retry per 5Hz tick
        }
    }
    if (slot >= 0 && work.retries_left == 0) s_rel[slot].active = false;
    portEXIT_CRITICAL(&s_rel_mux);
    if (slot < 0) return false;
    if (work.retries_left == 0) {
        s_route_failed++;
        reliable_max_retransmit(work.to, work.id);
        return false;
    }
    if (!lora_duty_allow(work.flen)) {
        portENTER_CRITICAL(&s_rel_mux);
        if (s_rel[slot].active && s_rel[slot].id == work.id)
            s_rel[slot].deadline_ms = now + route_retry_delay_ms(work.flen);
        portEXIT_CRITICAL(&s_rel_mux);
        ESP_LOGW("LoRaMesh", "reliable retry deferred by duty guard id=%08x",
                 (unsigned)work.id);
        return false;
    }
    if (lora_radio_cad()) {
        portENTER_CRITICAL(&s_rel_mux);
        if (s_rel[slot].active && s_rel[slot].id == work.id)
            s_rel[slot].deadline_ms = now +
                (1u + esp_random() % 4u) * route_slot_ms();
        portEXIT_CRITICAL(&s_rel_mux);
        return false;
    }
    if (work.retries_left == 1 && work.frame[14] != 0) {
        // Native's final originator retry clears a failed next-hop choice
        // and falls back to ordinary flooding.
        work.frame[14] = 0;
        route_clear(work.to);
        ESP_LOGI("LoRaMesh", "next-hop fallback dest=%08x id=%08x",
                 (unsigned)work.to, (unsigned)work.id);
    }
    int rc = lora_radio_send(work.frame, work.flen);
    portENTER_CRITICAL(&s_rel_mux);
    if (s_rel[slot].active && s_rel[slot].id == work.id) {
        s_rel[slot].retries_left--;
        s_rel[slot].deadline_ms = now + route_retry_delay_ms(work.flen);
    }
    portEXIT_CRITICAL(&s_rel_mux);
    if (rc == 0) {
        lora_duty_record(work.flen);
        ESP_LOGI("LoRaMesh", "reliable retry to %08x id %08x (%u left)",
                 (unsigned)work.to, (unsigned)work.id,
                 (unsigned)(work.retries_left - 1u));
        return true;
    }
    s_route_failed++;
    ESP_LOGW("LoRaMesh", "reliable retry radio failure id=%08x",
             (unsigned)work.id);
    return false;
}

void lora_mesh_tick(void) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    // Floods and reliable retries share one radio. Native's queue serializes
    // them; at 5Hz we likewise permit at most one queued TX per tick.
    bool radio_used = floodq_tick(now);
    if (!radio_used) radio_used = reliable_tick(now);
    if (radio_used) return;
    /* Native ReliableRouter responds to PKI_UNKNOWN_PUBKEY by sending our
     * NodeInfo directly to that peer. Defer it out of the RX callback; retry
     * gently if CAD/duty gating prevents the first attempt. */
    if (s_nodeinfo_target &&
        (s_nodeinfo_target_due_ms == 0 ||
         (int32_t)(now - s_nodeinfo_target_due_ms) >= 0)) {
        if (nodeinfo_send_ch_rate(s_nodeinfo_target, false,
                                  s_nodeinfo_target_channel, true,
                                  0, false, -1)) {
            s_nodeinfo_target = 0;
            s_nodeinfo_target_due_ms = s_nodeinfo_key_refresh_broadcast
                                           ? now + 1000u : 0;
        } else {
            s_nodeinfo_target_due_ms = now + 1000;
        }
        return;
    }
    if (s_nodeinfo_key_refresh_broadcast && s_nodeinfo_target == 0 &&
        (s_nodeinfo_target_due_ms == 0 ||
         (int32_t)(now - s_nodeinfo_target_due_ms) >= 0)) {
        if (nodeinfo_send_ch_rate(LORA_MESH_BROADCAST, false,
                                  s_nodeinfo_target_channel, true,
                                  0, false, -1)) {
            s_nodeinfo_key_refresh_broadcast = false;
            s_nodeinfo_target_due_ms = 0;
        } else {
            s_nodeinfo_target_due_ms = now + 1000u;
        }
        return;
    }
    if (s_nodeinfo_force) {
        if (nodeinfo_send(true)) {
            s_nodeinfo_force = false;
            s_next_nodeinfo_ms = now + (NODEINFO_SECS * 1000u);
        } else {
            s_next_nodeinfo_ms = now + 1000u;
        }
        return;
    }
    if (s_next_nodeinfo_ms == 0) {
        // Native NodeInfoModule waits 30 seconds after radio startup so the
        // network can settle before its first periodic announcement.
        s_next_nodeinfo_ms = now + (NODEINFO_INITIAL_DELAY_SECS * 1000u);
    }
    if ((int32_t)(now - s_next_nodeinfo_ms) >= 0) {
        if (s_role == LORA_MESH_ROLE_CLIENT_HIDDEN) {
            s_next_nodeinfo_ms = now + (NODEINFO_SECS * 1000u);
        } else if (nodeinfo_send(false))
            s_next_nodeinfo_ms = now + (NODEINFO_SECS * 1000u);
        else
            s_next_nodeinfo_ms = now + 1000u;
    }
    // Periodic position originate (default 15min): only while a fixed
    // position is known; duty-gated inside periodic_broadcast (a denied
    // slot simply retries next tick window, timer still advances).
    if (s_self_pos_len > 0) {
        if (s_next_pos_ms == 0) {
            s_next_pos_ms = now + (POS_BCAST_SECS * 1000u);
        } else if ((int32_t)(now - s_next_pos_ms) >= 0) {
            s_next_pos_ms = now + (POS_BCAST_SECS * 1000u);
            uint8_t pos[64];
            uint16_t pl = lora_mesh_get_self_position(pos, sizeof(pos));
            if (pl && periodic_broadcast(MESH_PORT_POSITION, pos, pl)) {
                ESP_LOGI("LoRaMesh", "position bcast (%uB)", (unsigned)pl);
            }
        }
    } else {
        s_next_pos_ms = 0;
    }
    // Hourly power-telemetry originate (always on; payload degrades to
    // uptime+duty when no battery gauge is readable).
    if (s_next_tlm_ms == 0) {
        s_next_tlm_ms = now + (TLM_BCAST_SECS * 1000u);
    } else if ((int32_t)(now - s_next_tlm_ms) >= 0) {
        s_next_tlm_ms = now + (TLM_BCAST_SECS * 1000u);
        uint8_t tm[48];
        uint16_t tl = devmetrics_build(tm, sizeof(tm));
        if (tl && periodic_broadcast(MESH_PORT_TELEMETRY, tm, tl)) {
            ESP_LOGI("LoRaMesh", "telemetry bcast (%uB)", (unsigned)tl);
        }
    }
}

// Driver background hook (weak in lora_sx1262.c): runs ~5Hz in the radio
// task while RX is armed, so NodeInfo goes out without a new task.
void lora_bg_tick(void) {
    lora_mesh_tick();
}

// Parse a decrypted Data payload. Returns portnum (0 = invalid).
// Writes through payload/plen directly (no statics: caller may nest).
static uint8_t data_parse(const uint8_t *p, uint16_t len,
                          const uint8_t **payload, uint16_t *plen,
                          uint32_t *request_id) {
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t field, wire, port = 0;
    uint32_t varint;
    const uint8_t *bytes;
    uint16_t blen;
    const uint8_t *found = NULL;
    uint16_t found_len = 0;
    uint32_t req = 0;
    while (r.pos < r.len) {
        if (!pb_r_next(&r, &field, &wire, &varint, &bytes, &blen))
            return 0;
        if (field == 1 && wire == 0) {
            if (varint > UINT8_MAX) return 0;
            port = (uint8_t)varint;
        }
        else if (field == 2 && wire == 2) {
            found = bytes;
            found_len = blen;
        }
        else if (field == 6 && wire == 5) req = varint;
    }
    if (port == 0) return 0;
    if (payload) *payload = found;
    if (plen) *plen = found_len;
    if (request_id) *request_id = req;
    return port;
}

// Try to decrypt one frame. Returns channel idx 0..7, or -2 for
// default-preset fallback (foreign preset name, default PSK), or -1.
// out_pt gets the decrypted Data on success.
static int decrypt_for_hash(uint8_t hash, uint32_t from, uint32_t id,
                            const uint8_t *ct, uint16_t ct_len,
                            uint8_t *out_pt) {
    // The hash is only eight bits and is not a channel identifier. Native
    // tries every matching local channel and accepts the first plaintext that
    // decodes as a non-UNKNOWN Data protobuf. Do not let a hash collision hide
    // a later valid channel.
    for (uint8_t idx = 0; idx < LORA_CH_MAX; idx++) {
        const lora_channel_t *channel = lora_channel_get(idx);
        if (!channel || !channel->used || channel->role == LORA_CH_DISABLED ||
            lora_channel_hash_of(idx) != hash)
            continue;
        uint8_t key[32];
        uint8_t klen = lora_channel_key(idx, key);
        memcpy(out_pt, ct, ct_len);
        if (klen == 0) {
            if (data_parse(out_pt, ct_len, NULL, NULL, NULL) != 0)
                return idx;
            continue;
        }
        lora_crypto_crypt_key(key, klen, from, id, out_pt, ct_len);
        memset(key, 0, sizeof(key));
        if (data_parse(out_pt, ct_len, NULL, NULL, NULL) != 0)
            return idx;
    }
    // Default-preset fallback (upstream setDefaultPresetCryptoForHash):
    // foreign mesh on default PSK but different preset name -> hash of
    // preset display name + default key. Try all known preset names.
    for (int p = 0; p < LORA_PRESET_COUNT; p++) {
        const char *nm = lora_preset_display_name(p, true);
        if (!nm || nm[0] == '\0' || strcmp(nm, "Invalid") == 0) continue;
        uint8_t h = lora_ch_hash_bytes(nm, LORA_DEFAULT_KEY, 16);
        if (h == hash) {
            memcpy(out_pt, ct, ct_len);
            lora_crypto_crypt(from, id, out_pt, ct_len);
            if (data_parse(out_pt, ct_len, NULL, NULL, NULL) != 0)
                return -2;
        }
    }
    return -1;
}

bool lora_mesh_decode_data(const uint8_t *frame, uint8_t len,
                           uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                           bool *out_want_ack, uint8_t *out_port,
                           uint32_t *out_request_id,
                           uint8_t *out_data, uint16_t data_cap, uint16_t *out_data_len) {
    return lora_mesh_decode_data_ch(frame, len, out_from, out_to, out_id,
                                    out_want_ack, out_port, out_request_id,
                                    NULL, NULL, NULL,
                                    out_data, data_cap, out_data_len);
}

bool lora_mesh_decode_data_ch(const uint8_t *frame, uint8_t len,
                              uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                              bool *out_want_ack, uint8_t *out_port,
                              uint32_t *out_request_id, uint8_t *out_ch_idx,
                              uint32_t *out_hop_start, uint32_t *out_hop_limit,
                              uint8_t *out_data, uint16_t data_cap, uint16_t *out_data_len) {
    uint32_t to = 0, from = 0, id = 0;
    bool want_ack = false;
    uint8_t channel = 0;
    uint8_t hl = 0, hs = 0;
    if (!hdr_parse(frame, len, &to, &from, &id, &hl, &hs, &want_ack, &channel)) return false;
    uint16_t dlen = (uint16_t)(len - MESH_HDR_LEN);
    if (!out_data || dlen == 0 || dlen > 245 || dlen > data_cap) return false;
    static uint8_t pt_tmp[245];
    if (dlen > sizeof(pt_tmp)) return false;
    int ch_idx = decrypt_for_hash(channel, from, id, &frame[MESH_HDR_LEN], dlen, pt_tmp);
    if (ch_idx < 0 && ch_idx != -2) {
        return false;
    }
    // pt_tmp now holds the decrypted Data bytes.
    memcpy(out_data, pt_tmp, dlen);
    const uint8_t *payload = NULL;
    uint16_t payload_len = 0;
    uint32_t request_id = 0;
    uint8_t port = data_parse(out_data, dlen, &payload, &payload_len, &request_id);
    (void)payload;
    (void)payload_len;
    if (!port) {
        char hex[25] = {0};
        uint16_t n = dlen < 8 ? dlen : 8;
        for (uint16_t i = 0; i < n; i++) snprintf(hex + i * 3, 4, "%02X ", out_data[i]);
        ESP_LOGW("LoRaMesh", "Data decode failed from=%08x to=%08x id=%08x ch=%02x len=%u plain=[%s]",
                 (unsigned)from, (unsigned)to, (unsigned)id, (unsigned)channel,
                 (unsigned)dlen, hex);
        return false;
    }
    if (out_from) *out_from = from;
    if (out_to) *out_to = to;
    if (out_id) *out_id = id;
    if (out_want_ack) *out_want_ack = want_ack;
    if (out_port) *out_port = port;
    if (out_request_id) *out_request_id = request_id;
    if (out_ch_idx) *out_ch_idx = (ch_idx >= 0) ? (uint8_t)ch_idx : lora_channel_primary();
    if (out_hop_start) *out_hop_start = hs;
    if (out_hop_limit) *out_hop_limit = hl;
    if (out_data_len) *out_data_len = dlen;
    return true;
}

// Parse a User protobuf (NODEINFO payload) for display names + public key.
static void user_parse_ex(const uint8_t *p, uint16_t len,
                          char *long_name, size_t ln_max,
                          char *short_name, size_t sn_max,
                          uint8_t *out_pub32) {
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t field, wire;
    uint32_t varint;
    const uint8_t *bytes;
    uint16_t blen;
    if (long_name && ln_max) long_name[0] = '\0';
    if (short_name && sn_max) short_name[0] = '\0';
    while (pb_r_next(&r, &field, &wire, &varint, &bytes, &blen)) {
        if (field == 2 && wire == 2 && long_name && ln_max && blen < ln_max) {
            memcpy(long_name, bytes, blen);
            long_name[blen] = '\0';
        } else if (field == 3 && wire == 2 && short_name && sn_max && blen < sn_max) {
            memcpy(short_name, bytes, blen);
            short_name[blen] = '\0';
        } else if (field == 8 && wire == 2 && out_pub32 && blen == 32) {
            memcpy(out_pub32, bytes, 32);
        }
    }
}

bool lora_mesh_on_rx(const uint8_t *frame, uint8_t len, int16_t rssi, float snr,
                     char *out_who, size_t who_max, char *out_text, size_t text_max) {
    return lora_mesh_on_rx_ex(frame, len, rssi, snr, out_who, who_max,
                              out_text, text_max, NULL, NULL, NULL, NULL, NULL);
}

bool lora_mesh_on_rx_ex(const uint8_t *frame, uint8_t len, int16_t rssi, float snr,
                        char *out_who, size_t who_max, char *out_text, size_t text_max,
                        uint32_t *out_from, uint32_t *out_id, uint32_t *out_to,
                        uint32_t *out_hop_start, uint32_t *out_hop_limit) {
    uint32_t to, from, id;
    uint8_t hl = 0, hs = 0, ch = 0;
    bool ack = false;
    if (!hdr_parse(frame, len, &to, &from, &id, &hl, &hs, &ack, &ch)) {
        return false;
    }
    (void)ack;
    if (from == lora_mesh_node_num()) return false;
    // NOTE: no seen-marking here — the flood scheduler
    // owns dedup accounting; delivery parses without claiming.
    if (seen_before(from, id)) return false;
    uint16_t plen = (uint16_t)(len - MESH_HDR_LEN);
    if (plen == 0 || plen > 245) return false; // 233 channel + 12 PKI overhead
    // Decrypt a copy (caller buffer may be shared/static). Native tries an
    // eligible authenticated PKI direct before channel-hash decryption.
    // 245B cap covers channel frames (233) plus PKI overhead (+12).
    static uint8_t pt[245];
    if (plen > sizeof(pt)) return false;
    const uint8_t *pl = NULL;
    uint16_t pllen = 0;
    uint8_t port = 0;
    bool is_pki = false;
    int decoded_ch_idx = -1;
    if (ch == 0 && to == lora_mesh_node_num()) {
        uint8_t peer[32];
        if (lora_mesh_peer_pubkey(from, peer)) {
            static uint8_t ptd[245];
            uint16_t ptd_len = lora_pki_decrypt(from, peer, id,
                                               &frame[MESH_HDR_LEN], plen,
                                               ptd, sizeof(ptd));
            memset(peer, 0, sizeof(peer));
            if (ptd_len) {
                uint8_t p2 = data_parse(ptd, ptd_len, NULL, NULL, NULL);
                if (p2) {
                    memcpy(pt, ptd, ptd_len);
                    plen = ptd_len;
                    // Re-parse from the retained plaintext buffer.  Parsing
                    // ptd directly leaves pl pointing into ptd, which is
                    // cleared below before the caller copies the message.
                    // That made valid PKI DMs appear as empty text on the
                    // OLED/status preview even though decrypt and ACK worked.
                    port = data_parse(pt, plen, &pl, &pllen, NULL);
                    is_pki = true;
                }
                memset(ptd, 0, sizeof(ptd));
            }
        }
    }
    if (port == 0 && plen <= 233) {
        decoded_ch_idx = decrypt_for_hash(ch, from, id,
                                          &frame[MESH_HDR_LEN], plen, pt);
        if (decoded_ch_idx >= 0 || decoded_ch_idx == -2)
            port = data_parse(pt, plen, &pl, &pllen, NULL);
    }
    // Legacy-DM diagnostic (logging only, still delivered below): stock sends
    // a DM channel-encrypted instead of PKI exactly when it lacks our public
    // key, so a channel-decrypted unicast TEXT to self proves "stock sent,
    // we heard" for that case. Self-only (rate-safe); flood traffic silent.
    // NOTE: lora_pki_is_legacy_dm had no call sites before this line — there
    // is no reject path in tree (we deliver); this is its first use, as a
    // pure observation predicate with no behavior change.
    if (port != 0 && !is_pki && to == lora_mesh_node_num() &&
        lora_pki_is_legacy_dm(port, to, from)) {
        ESP_LOGW("LoRaMesh", "legacy DM from=%08x id=%08x ch=%02x (channel-encrypted TEXT to self)",
                 (unsigned)from, (unsigned)id, (unsigned)ch);
    }
    if (port == 0) return false; // foreign key or corrupt: flood-only
    if (port == 4) { // NODEINFO_APP: learn names + public key silently
        char ln[40] = {0};
        char sn[8] = {0};
        uint8_t pk[32] = {0};
        uint32_t rhw = 0, rrole = 0;
        bool advertised_pub = false;
        // Parse all identity fields first. Native validates the public key
        // before mutating any part of the stored User record.
        user_parse_ex(pl, pllen, ln, sizeof(ln), sn, sizeof(sn), pk);
        {
            pb_r_t hr;
            pb_r_init(&hr, pl, pllen);
            uint8_t hf, hw2;
            uint32_t hv;
            const uint8_t *hb;
            uint16_t hl2;
            while (pb_r_next(&hr, &hf, &hw2, &hv, &hb, &hl2)) {
                if (hf == 5 && hw2 == 0) rhw = hv;
                else if (hf == 7 && hw2 == 0) rrole = hv;
                else if (hf == 8 && hw2 == 2 && hl2 == 32)
                    advertised_pub = true;
            }
        }
        if (!lora_mesh_peer_nodeinfo_key_ok(from, pk, advertised_pub)) {
            memset(pk, 0, sizeof(pk));
            return false;
        }
        node_touch_named(from, ln, sn,
                         true,
                         decoded_ch_idx >= 0 ? (uint8_t)decoded_ch_idx : 0xFF,
                         rssi, snr);
        lora_mesh_peer_set_hw_role(from, rhw, rrole);
        if (advertised_pub)
            lora_mesh_peer_set_pubkey(from, pk);
        memset(pk, 0, sizeof(pk));
        if (hs >= hl) lora_mesh_note_hops(from, (uint8_t)(hs - hl));
        return false;
    }
    node_touch_named(from, NULL, NULL, false,
                     (!is_pki && decoded_ch_idx >= 0)
                         ? (uint8_t)decoded_ch_idx
                         : 0xFF,
                     rssi, snr);
    if (hs >= hl) lora_mesh_note_hops(from, (uint8_t)(hs - hl));
    if (port != 1) return false; // flood-only for other portnums
    if (to != LORA_MESH_BROADCAST && to != lora_mesh_node_num()) return false;
    if (out_from) *out_from = from;
    if (out_id) *out_id = id;
    if (out_to) *out_to = to;
    if (out_hop_start) *out_hop_start = hs;
    if (out_hop_limit) *out_hop_limit = hl;
    if (out_who && who_max) {
        // Prefer learned short name, else hex. PKI directs get a DM: prefix.
        const char *nm = NULL;
        for (int i = 0; i < s_ncount; i++) {
            if (s_nodes[i].node_num == from && s_nodes[i].has_user) {
                nm = s_nodes[i].short_name;
                break;
            }
        }
        char base[8] = {0};
        if (nm && nm[0]) snprintf(base, sizeof(base), "%s", nm);
        else snprintf(base, sizeof(base), "%06X", (unsigned)(from & 0xFFFFFF));
        if (is_pki) snprintf(out_who, who_max, "DM:%s", base);
        else snprintf(out_who, who_max, "%s", base);
    }
    if (out_text && text_max) {
        size_t c = pllen < text_max - 1 ? pllen : text_max - 1;
        memcpy(out_text, pl, c);
        out_text[c] = '\0';
    }
    return true;
}

bool lora_mesh_schedule_rebroadcast(const uint8_t *frame, uint8_t len,
                                    float rx_snr) {
    uint32_t to, from, id;
    uint8_t hl = 0, hs = 0, ch = 0;
    bool ack = false;
    if (!hdr_parse(frame, len, &to, &from, &id, &hl, &hs, &ack, &ch)) {
        return false;
    }
    (void)ack;
    uint32_t self = lora_mesh_node_num();
    if (from == self || to == self) return false;
    if (hl == 0) return false;
    if (len > 252) return false;
    // Sole owner of dedup accounting: every reception funnels here, so each
    // duplicate is counted exactly once (delivery above never marks).
    if (seen_before(from, id)) {
        s_dups++;
        if (floodq_on_duplicate(from, id, hl, rx_snr, frame, len)) return false;
        // A reliable originator repeats with hop_start==hop_limit when its
        // ACK was lost. Native requeues it if our earlier relay is no longer
        // pending so it can receive another implicit ACK.
        if (!(hs > 0 && hs == hl)) return false;
    }
    seen_mark(from, id);
    // --- Relay eligibility (role / rebroadcast-mode gate) ---
    // Default CLIENT/ALL floods everything (HIL-safe). Modes below only
    // restrict; they never expand.
    if (s_role == LORA_MESH_ROLE_CLIENT_MUTE) return false; // never flood
    if (s_rebroadcast_mode == LORA_MESH_RB_NONE) return false;
    // NextHopRouter only accepts a preferred-hop packet if it names us.
    if (frame[14] != 0 && frame[14] != (uint8_t)(self & 0xFF)) return false;
    // Ignored peers are consumed (dup-counted) but never relayed. Muted
    // peers still relay (mute only silences local delivery/display).
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == from && s_nodes[i].ignored) {
            return false;
        }
    }
    {
        uint16_t ctl = (uint16_t)(len - MESH_HDR_LEN);
        static uint8_t pt_probe[245];
        int di = -1;
        if (ctl <= sizeof(pt_probe))
            di = decrypt_for_hash(ch, from, id, &frame[MESH_HDR_LEN], ctl, pt_probe);
        bool decryptable = (di >= 0 || di == -2);
        bool pki_known = false;
        if (!decryptable && ch == 0) {
            uint8_t pk[32];
            if (lora_mesh_peer_pubkey(from, pk)) pki_known = true;
            memset(pk, 0, sizeof(pk));
            // PKI DM with known key counts as local for gating purposes.
            if (pki_known) decryptable = true;
        }
        bool foreign = !decryptable;
        if (s_rebroadcast_mode == LORA_MESH_RB_LOCAL_ONLY && foreign) return false;
        if (s_rebroadcast_mode == LORA_MESH_RB_KNOWN_ONLY) {
            bool known_peer = false;
            for (int i = 0; i < s_ncount; i++) {
                if (s_nodes[i].node_num == from) { known_peer = true; break; }
            }
            bool known_ch = decryptable;
            if (!known_peer && !known_ch) return false;
        }
        if (s_rebroadcast_mode == LORA_MESH_RB_CORE_ONLY && foreign) {
            // Foreign + undecryptable: no port to classify; allow opaque
            // flood (dropping it would blackhole other meshes). When
            // decryptable, filter below by port.
        }
        if (s_rebroadcast_mode == LORA_MESH_RB_CORE_ONLY && decryptable &&
            to != self) {
            const uint8_t *pl = NULL;
            uint16_t pll = 0;
            uint8_t port = data_parse(pt_probe, ctl, &pl, &pll, NULL);
            bool core = (port == 1 || port == 3 || port == 4 || port == 7 ||
                         port == 5 || port == 6 || port == 70);
            if (port != 0 && !core) return false; // drop non-core module ports
        }
    }
    uint8_t out_frame[252];
    memcpy(out_frame, frame, len);
    // Hop accounting (shouldDecrementHopLimit): normally hl-1; preserved
    // only for the router-to-favorite-router shortcut (direct + ROUTER).
    {
        uint8_t hops_away = (hs >= hl) ? (uint8_t)(hs - hl) : 0;
        uint8_t new_hl = hl;
        if (route_should_decrement_hop(hops_away, frame[15]) && hl > 0)
            new_hl = (uint8_t)(hl - 1);
        uint8_t flags = (uint8_t)((new_hl & 0x07) | (out_frame[12] & 0xF8));
        out_frame[12] = flags;
    }
    // A packet which selected us gets our validated next hop, avoiding the
    // neighbor which just relayed it. No cache entry means flood fallback.
    out_frame[14] = route_next_hop(to, frame[15]);
    out_frame[15] = (uint8_t)(self & 0xFF);
    // Traceroute relay (port 70, minimal viable): ciphertext stays opaque
    // for stock interop; our ID/SNR contribution is the relay_node stamp
    // above plus the NodeDB SNR record. A full route[] append would require
    // re-encrypting with the channel key and is intentionally avoided here
    // (see build_traceroute_reply for the self-destined reply path).
    return floodq_put(from, id, hl, rx_snr, out_frame, len);
}

uint16_t lora_mesh_nodes(lora_mesh_node_t *out, uint16_t max) {
    if (!out || max == 0) return s_ncount;
    uint16_t n = s_ncount < max ? s_ncount : max;
    memcpy(out, s_nodes, (size_t)n * sizeof(*out));
    return n;
}

bool lora_mesh_node_get(uint32_t node_num, lora_mesh_node_t *out) {
    if (!out) return false;
    for (uint16_t i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            *out = s_nodes[i];
            return true;
        }
    }
    return false;
}

bool lora_mesh_node_at(uint16_t index, lora_mesh_node_t *out) {
    if (!out || index >= s_ncount) return false;
    *out = s_nodes[index];
    return true;
}

uint16_t lora_mesh_node_ids(uint32_t *out, uint16_t max) {
    if (!out || max == 0) return s_ncount;
    uint16_t n = s_ncount < max ? s_ncount : max;
    for (uint16_t i = 0; i < n; i++) out[i] = s_nodes[i].node_num;
    return n;
}

bool lora_mesh_peer_short_name(uint32_t node_num, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    for (uint16_t i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num && s_nodes[i].has_user &&
            s_nodes[i].short_name[0]) {
            snprintf(out, cap, "%s", s_nodes[i].short_name);
            return true;
        }
    }
    return false;
}

bool lora_mesh_remove_node(uint32_t node_num) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            memmove(&s_nodes[i], &s_nodes[i + 1],
                    (size_t)(s_ncount - i - 1) * sizeof(s_nodes[0]));
            s_ncount--;
            memset(&s_nodes[s_ncount], 0, sizeof(s_nodes[0]));
            // Delete the NVS entry + update the "peers"/"pr_idx" index.
            mesh_peers_erase_one(node_num);
            return true;
        }
    }
    return false;
}

uint16_t lora_mesh_clear_nodes(void) {
    uint16_t removed = s_ncount;
    memset(s_nodes, 0, sizeof(*s_nodes) * LORA_MESH_NODES_MAX);
    s_ncount = 0;
    return removed;
}

static lora_mesh_node_t *node_find_or_add(uint32_t node_num, int16_t rssi, float snr) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) return &s_nodes[i];
    }
    lora_mesh_node_t n;
    memset(&n, 0, sizeof(n));
    n.node_num = node_num;
    n.last_rssi = rssi;
    n.last_snr = snr;
    n.last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
    lora_mesh_node_t *slot;
    uint32_t evicted = 0;
    bool had_evict = false;
    if (s_ncount >= LORA_MESH_NODES_MAX) {
        int victim = node_oldest_index();
        if (victim < 0) return NULL;
        evicted = s_nodes[victim].node_num;
        memmove(&s_nodes[victim], &s_nodes[victim + 1],
                (size_t)(s_ncount - victim - 1) * sizeof(s_nodes[0]));
        s_ncount--;
        had_evict = true;
    }
    memmove(&s_nodes[1], &s_nodes[0], (size_t)s_ncount * sizeof(n));
    s_nodes[0] = n;
    s_ncount++;
    slot = &s_nodes[0];
    // Evict a persisted copy only if the RAM LRU happened to have one. The
    // caller persists after applying its key/contact/flag mutation, avoiding
    // an extra blank-peer NVS commit.
    if (had_evict && evicted != node_num) mesh_peers_erase_one(evicted);
    return slot;
}

bool lora_mesh_peer_pubkey(uint32_t node_num, uint8_t *out32) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num && s_nodes[i].has_pubkey) {
            if (out32) memcpy(out32, s_nodes[i].pubkey, 32);
            return true;
        }
    }
    return false;
}

bool lora_mesh_peer_nodeinfo_key_ok(uint32_t node_num,
                                    const uint8_t *pub32, bool has_pubkey) {
    if (node_num < 4 || node_num == s_node || node_num == LORA_MESH_BROADCAST)
        return false;
    // Native rejects a remote node advertising our own public key.
    const uint8_t *ours = lora_pki_public();
    if (has_pubkey && pub32 && ours && memcmp(pub32, ours, 32) == 0) {
        ESP_LOGW("LoRaMesh", "NodeInfo %08x advertises our public key -> dropped",
                 (unsigned)node_num);
        return false;
    }
    // Native NodeDB::updateUser treats an established 32-byte key as
    // immutable. A missing or different key rejects the whole NodeInfo.
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num != node_num || !s_nodes[i].has_pubkey) continue;
        if (!has_pubkey || !pub32 || memcmp(s_nodes[i].pubkey, pub32, 32) != 0) {
            ESP_LOGW("LoRaMesh", "Public Key mismatch, dropping NodeInfo from %08x",
                     (unsigned)node_num);
            return false;
        }
        break;
    }
    return true;
}

bool lora_mesh_peer_set_pubkey(uint32_t node_num, const uint8_t *pub32) {
    if (!pub32) return false;
    uint8_t z[32] = {0};
    if (memcmp(pub32, z, 32) == 0) return false;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].has_pubkey && memcmp(s_nodes[i].pubkey, pub32, 32) == 0)
                return true;
            memcpy(s_nodes[i].pubkey, pub32, 32);
            s_nodes[i].has_pubkey = true;
            mesh_peers_store_one(&s_nodes[i]);
            return true;
        }
    }
    lora_mesh_node_t *slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return false;
    memcpy(slot->pubkey, pub32, 32);
    slot->has_pubkey = true;
    mesh_peers_store_one(slot);
    return true;
}

bool lora_mesh_peer_apply_contact(uint32_t node_num,
                                  const char *long_name,
                                  const char *short_name,
                                  uint32_t hw_model, uint32_t role,
                                  const uint8_t *pub32, bool has_pubkey,
                                  bool manually_verified, bool should_ignore) {
    if (node_num < 4 || node_num == s_node ||
        node_num == LORA_MESH_BROADCAST) return false;
    lora_mesh_node_t *slot = NULL;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            slot = &s_nodes[i];
            break;
        }
    }
    // Native refuses a client-side contact replacement when the saved key is
    // manually verified but this contact is not verified and differs/misses.
    bool contact_key_matches = slot && slot->has_pubkey == has_pubkey &&
                               (!has_pubkey ||
                                (pub32 && memcmp(slot->pubkey, pub32, 32) == 0));
    if (slot && slot->key_verified && !manually_verified &&
        !contact_key_matches) {
        ESP_LOGW("LoRaMesh", "verified contact %08x key replacement rejected",
                 (unsigned)node_num);
        return false;
    }
    if (!slot) slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return false;

    slot->has_user = true;
    snprintf(slot->long_name, sizeof(slot->long_name), "%s",
             long_name ? long_name : "");
    snprintf(slot->short_name, sizeof(slot->short_name), "%s",
             short_name ? short_name : "");
    slot->hw_model = hw_model <= UINT16_MAX ? (uint16_t)hw_model : 0;
    slot->role = role <= UINT8_MAX ? (uint8_t)role : 0;
    if (should_ignore) {
        memset(slot->pubkey, 0, sizeof(slot->pubkey));
        slot->has_pubkey = false;
        slot->ignored = true;
    } else if (has_pubkey && pub32) {
        uint8_t zero[32] = {0};
        if (memcmp(pub32, zero, sizeof(zero)) != 0) {
            memcpy(slot->pubkey, pub32, sizeof(slot->pubkey));
            slot->has_pubkey = true;
        } else {
            memset(slot->pubkey, 0, sizeof(slot->pubkey));
            slot->has_pubkey = false;
        }
    } else {
        memset(slot->pubkey, 0, sizeof(slot->pubkey));
        slot->has_pubkey = false;
    }
    if (manually_verified) slot->key_verified = true;
    mesh_peers_store_one(slot);
    return true;
}

bool lora_mesh_peer_set_verified(uint32_t node_num, bool verified) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].key_verified == verified) return true;
            s_nodes[i].key_verified = verified;
            mesh_peers_store_one(&s_nodes[i]);
            return true;
        }
    }
    return false;
}

bool lora_mesh_peer_set_muted(uint32_t node_num, bool muted) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].muted == muted) return true;
            s_nodes[i].muted = muted;
            mesh_peers_store_one(&s_nodes[i]);
            return true;
        }
    }
    return false;
}

bool lora_mesh_peer_set_favorite(uint32_t node_num, bool favorite) {
    if (node_num < 4) return false;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].favorite == favorite) return true;
            s_nodes[i].favorite = favorite;
            mesh_peers_store_one(&s_nodes[i]);
            return true;
        }
    }
    if (!favorite) return true;
    lora_mesh_node_t *slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return false;
    slot->favorite = true;
    mesh_peers_store_one(slot);
    return true;
}

// Ignored peers: never relayed (see schedule_rebroadcast). Unknown nodes get
// a table entry so a pre-learned ignore applies to future receptions;
// defaults permissive (absent entry relays).
bool lora_mesh_peer_set_ignored(uint32_t node_num, bool ignored) {
    if (node_num < 4) return false;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].ignored == ignored) return true;
            s_nodes[i].ignored = ignored;
            mesh_peers_store_one(&s_nodes[i]);
            return true;
        }
    }
    if (!ignored) return true; // nothing to record when clearing unknown
    lora_mesh_node_t *slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return false;
    slot->ignored = true;
    mesh_peers_store_one(slot);
    return true;
}

// Lightweight flag reads so the manager can mirror mesh-side state.
bool lora_mesh_peer_get_muted(uint32_t node_num) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) return s_nodes[i].muted;
    }
    return false;
}

bool lora_mesh_peer_get_ignored(uint32_t node_num) {
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) return s_nodes[i].ignored;
    }
    return false;
}

void lora_mesh_peer_set_hw_role(uint32_t node_num, uint32_t hw_model, uint32_t role) {
    uint16_t compact_hw = hw_model <= UINT16_MAX ? (uint16_t)hw_model : 0;
    uint8_t compact_role = role <= UINT8_MAX ? (uint8_t)role : 0;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            bool changed = false;
            if (s_nodes[i].hw_model != compact_hw) {
                s_nodes[i].hw_model = compact_hw;
                changed = true;
            }
            if (s_nodes[i].role != compact_role) {
                s_nodes[i].role = compact_role;
                changed = true;
            }
            if (changed) mesh_peers_store_one(&s_nodes[i]);
            return;
        }
    }
    lora_mesh_node_t *slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return;
    slot->hw_model = compact_hw;
    slot->role = compact_role;
    mesh_peers_store_one(slot);
}

void lora_mesh_peer_set_channel(uint32_t node_num, uint8_t channel) {
    if (channel >= LORA_CH_MAX || node_num < 4 ||
        node_num == s_node || node_num == LORA_MESH_BROADCAST) return;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (s_nodes[i].channel != channel) {
                s_nodes[i].channel = channel;
                mesh_peers_store_one(&s_nodes[i]);
            }
            return;
        }
    }
    lora_mesh_node_t *slot = node_find_or_add(node_num, 0, 0.0f);
    if (!slot) return;
    slot->channel = channel;
    mesh_peers_store_one(slot);
}

void lora_mesh_note_hops(uint32_t node_num, uint8_t hops_away) {
    // Hops are ephemeral radio state: excluded from the NVS peer blob and
    // re-marked invalid on boot, so this hot-path update never commits to
    // NVS (batching rule — same as RSSI-only touches).
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            s_nodes[i].hops_away = hops_away > 7 ? 7 : hops_away;
            s_nodes[i].hops_valid = true;
            return;
        }
    }
}

void lora_mesh_peer_meta(uint32_t node_num, bool *verified, bool *muted,
                         bool *hops_valid, uint8_t *hops_away) {
    if (verified) *verified = false;
    if (muted) *muted = false;
    if (hops_valid) *hops_valid = false;
    if (hops_away) *hops_away = 0;
    for (int i = 0; i < s_ncount; i++) {
        if (s_nodes[i].node_num == node_num) {
            if (verified) *verified = s_nodes[i].key_verified;
            if (muted) *muted = s_nodes[i].muted;
            if (hops_valid) *hops_valid = s_nodes[i].hops_valid;
            if (hops_away) *hops_away = s_nodes[i].hops_away;
            return;
        }
    }
}

uint32_t lora_mesh_dups(void) { return s_dups; }

#else
typedef int lora_mesh_stub_guard;
#endif
