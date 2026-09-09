// lora_channels.c
// See lora_channels.h for reference.

#include "managers/lora_channels.h"
#include "managers/lora_crypto.h"
#include "managers/lora_mesh.h"
#include "managers/lora_modem.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "LoRaCh";

static lora_channel_t s_ch[LORA_CH_MAX];
static bool s_init = false;
static char s_defname[32] = "LongFast";

// Per-channel ChannelSettings.module_settings=7 opaque bytes
// (ModuleSettings: position_precision=1, is_muted=2). Kept beside the
// channel table (not inside it, so the "channels" NVS blob stays
// layout-stable) and persisted in a separate "ch_mod" blob.
static uint8_t s_mod[LORA_CH_MAX][LORA_CH_MOD_MAX];
static uint8_t s_mod_len[LORA_CH_MAX];

// Best-effort ModuleSettings.position_precision (field 1, varint bits):
// live-apply to the mesh mask (setter ignores other values).
static void apply_mod_precision(const uint8_t *mod, uint8_t len) {
    if (!mod || len == 0) return;
    uint8_t i = 0;
    while (i < len) {
        uint8_t tag = mod[i++];
        uint8_t field = (uint8_t)(tag >> 3);
        uint8_t wire = (uint8_t)(tag & 7);
        if (wire == 0) {
            uint32_t v = 0;
            uint8_t shift = 0;
            while (i < len) {
                uint8_t b = mod[i++];
                v |= (uint32_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80) || shift >= 28) break;
            }
            if (field == 1 && v != 0) lora_mesh_set_position_precision((int)v);
        } else if (wire == 5) {
            if (i + 4 > len) break;
            i += 4;
        } else if (wire == 2) {
            uint32_t n = 0;
            uint8_t shift = 0;
            while (i < len) {
                uint8_t b = mod[i++];
                n |= (uint32_t)(b & 0x7F) << shift;
                shift += 7;
                if (!(b & 0x80) || shift >= 28) break;
            }
            if (i + n > len) break;
            i += (uint8_t)n;
        } else {
            break; // groups / reserved: stop, don't misparse
        }
    }
}

static uint8_t xor_hash(const uint8_t *p, size_t n) {
    uint8_t h = 0;
    if (p) {
        for (size_t i = 0; i < n; i++) h ^= p[i];
    }
    return h;
}

uint8_t lora_ch_hash_bytes(const char *name, const uint8_t *key, uint8_t key_len) {
    uint8_t h = 0;
    if (name) h ^= xor_hash((const uint8_t *)name, strlen(name));
    h ^= xor_hash(key, key_len);
    return h;
}

// Expand 1-byte shorthand: 0 = no crypto, 1..N = default key with last byte
// bumped (upstream Channels::getKey). Longer short keys are zero-padded to 16.
static uint8_t expand_key(const uint8_t *psk, uint8_t len, uint8_t *out32) {
    if (!out32) return 0;
    if (len == 0 || !psk) return 0;
    if (len == 1) {
        if (psk[0] == 0) return 0;
        memcpy(out32, LORA_DEFAULT_KEY, 16);
        out32[15] = (uint8_t)(out32[15] + psk[0] - 1);
        return 16;
    }
    if (len <= 16) {
        memcpy(out32, psk, len);
        if (len < 16) memset(out32 + len, 0, 16 - len);
        return 16;
    }
    if (len <= 32) {
        memcpy(out32, psk, len);
        if (len < 32) memset(out32 + len, 0, 32 - len);
        return 32;
    }
    return 0;
}

static void recompute(uint8_t idx);

// "Default" is the phone-app placeholder for "no name" (upstream fixup):
// treat it as empty so hash/slot fall back to the preset display name.
static bool is_default_name(const char *n) {
    return !n || n[0] == '\0' || strcmp(n, "Default") == 0;
}

static const char *canonical_name(const lora_channel_t *ch) {
    if (!ch || is_default_name(ch->name)) return s_defname;
    return ch->name;
}

// Primary channel index, or -1 when none is set.
static int primary_idx(void) {
    for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
        if (s_ch[i].used && s_ch[i].role == LORA_CH_PRIMARY) return (int)i;
    }
    return -1;
}

// Expanded key for hashing/crypt, with the upstream secondary fallback:
// a SECONDARY with empty PSK inherits the PRIMARY key.
static uint8_t effective_key(uint8_t idx, uint8_t *out32) {
    if (idx >= LORA_CH_MAX || !out32) return 0;
    uint8_t klen = expand_key(s_ch[idx].psk, s_ch[idx].psk_len, out32);
    if (klen == 0 && s_ch[idx].role == LORA_CH_SECONDARY) {
        int pi = primary_idx();
        if (pi >= 0 && pi != (int)idx) {
            klen = expand_key(s_ch[pi].psk, s_ch[pi].psk_len, out32);
        }
    }
    return klen;
}

static void recompute(uint8_t idx) {
    uint8_t key[32];
    uint8_t klen = effective_key(idx, key);
    // Unencrypted primary still hashes name-only (upstream behavior).
    // NOTE: hash 0x00 is valid (empty name + no key); only lookup miss is -1.
    s_ch[idx].hash = lora_ch_hash_bytes(canonical_name(&s_ch[idx]), key, klen);
    memset(key, 0, sizeof(key));
}

static void persist(void) {
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "channels", s_ch, sizeof(s_ch));
    // mod_settings sidecar: packed lens + bytes (missing blob = all empty).
    {
        uint8_t blob[LORA_CH_MAX * (1 + LORA_CH_MOD_MAX)];
        uint8_t *p = blob;
        for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
            *p++ = s_mod_len[i];
            memcpy(p, s_mod[i], LORA_CH_MOD_MAX);
            p += LORA_CH_MOD_MAX;
        }
        nvs_set_blob(h, "ch_mod", blob, sizeof(blob));
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_mod(void) {
    memset(s_mod, 0, sizeof(s_mod));
    memset(s_mod_len, 0, sizeof(s_mod_len));
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t blob[LORA_CH_MAX * (1 + LORA_CH_MOD_MAX)];
    size_t n = sizeof(blob);
    if (nvs_get_blob(h, "ch_mod", blob, &n) == ESP_OK && n == sizeof(blob)) {
        const uint8_t *p = blob;
        for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
            uint8_t l = *p++;
            if (l > LORA_CH_MOD_MAX) l = LORA_CH_MOD_MAX;
            s_mod_len[i] = l;
            memcpy(s_mod[i], p, LORA_CH_MOD_MAX);
            p += LORA_CH_MOD_MAX;
        }
    }
    nvs_close(h);
}

void lora_channels_set_default_name(const char *name) {
    if (name && name[0] && strcmp(name, "Default") != 0) {
        snprintf(s_defname, sizeof(s_defname), "%s", name);
        if (s_init) {
            for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
                if (s_ch[i].used && is_default_name(s_ch[i].name)) recompute(i);
            }
        }
    }
}

void lora_channels_init(void) {
    if (s_init) return;
    s_init = true;
    memset(s_ch, 0, sizeof(s_ch));
    nvs_handle_t h = 0;
    size_t n = sizeof(s_ch);
    bool ok = false;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        ok = (nvs_get_blob(h, "channels", s_ch, &n) == ESP_OK && n == sizeof(s_ch));
        nvs_close(h);
    }
    if (!ok) {
        memset(s_ch, 0, sizeof(s_ch));
        s_ch[0].used = true;
        s_ch[0].role = LORA_CH_PRIMARY;
        s_ch[0].psk_len = 1;
        s_ch[0].psk[0] = 0x01;
        s_ch[0].name[0] = '\0'; // default -> preset display name
        s_ch[0].uplink = true;
        s_ch[0].downlink = true;
        recompute(0);
        persist();
        ESP_LOGI(TAG, "installed default PRIMARY (hash=0x%02x)", s_ch[0].hash);
    } else {
        for (uint8_t i = 0; i < LORA_CH_MAX; i++) recompute(i);
    }
    load_mod();
    // Honor stored position_precision on boot (last used channel wins when
    // several carry it; the mask is global).
    for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
        if (s_mod_len[i]) apply_mod_precision(s_mod[i], s_mod_len[i]);
    }
}

uint8_t lora_channels_count(void) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
        if (s_ch[i].used && s_ch[i].role != LORA_CH_DISABLED) n++;
    }
    return n;
}

const lora_channel_t *lora_channel_get(uint8_t idx) {
    if (idx >= LORA_CH_MAX) return NULL;
    return &s_ch[idx];
}

bool lora_channel_set(uint8_t idx, const char *name, const uint8_t *psk,
                      uint8_t psk_len, uint8_t role, bool uplink, bool downlink) {
    if (idx >= LORA_CH_MAX) return false;
    if (role > LORA_CH_SECONDARY) return false;
    if (psk_len > LORA_CH_PSK_MAX) return false;
    if (role == LORA_CH_DISABLED) return lora_channel_disable(idx);
    s_ch[idx].used = true;
    s_ch[idx].role = role;
    if (psk) {
        s_ch[idx].psk_len = psk_len;
        if (psk_len) memcpy(s_ch[idx].psk, psk, psk_len);
    }
    if (name) {
        // Phone apps send "Default" for an unnamed channel: store empty so
        // hash/slot use the preset display name (upstream fixup).
        if (strcmp(name, "Default") == 0) s_ch[idx].name[0] = '\0';
        else snprintf(s_ch[idx].name, sizeof(s_ch[idx].name), "%s", name);
    }
    s_ch[idx].uplink = uplink;
    s_ch[idx].downlink = downlink;
    if (role == LORA_CH_PRIMARY) {
        for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
            if (i != idx && s_ch[i].role == LORA_CH_PRIMARY)
                s_ch[i].role = LORA_CH_SECONDARY;
        }
    }
    recompute(idx);
    persist();
    return true;
}

bool lora_channel_set_id(uint8_t idx, uint32_t id) {
    if (idx >= LORA_CH_MAX) return false;
    s_ch[idx].id = id;
    persist();
    return true;
}

bool lora_channel_disable(uint8_t idx) {
    if (idx >= LORA_CH_MAX) return false;
    s_ch[idx].used = false;
    s_ch[idx].role = LORA_CH_DISABLED;
    s_ch[idx].hash = 0;
    s_mod_len[idx] = 0; // stale module_settings must not re-apply later
    memset(s_mod[idx], 0, sizeof(s_mod[idx]));
    persist();
    return true;
}

// Opaque ChannelSettings.module_settings=7 store (phone path calls this
// alongside lora_channel_set, which has no mod slot). Persists + applies
// position_precision immediately. len==0 clears (mod NULL allowed then).
bool lora_channel_set_mod(uint8_t idx, const uint8_t *mod, uint8_t len) {
    if (idx >= LORA_CH_MAX || len > LORA_CH_MOD_MAX) return false;
    if (len && !mod) return false;
    if (len) memcpy(s_mod[idx], mod, len);
    else memset(s_mod[idx], 0, sizeof(s_mod[idx]));
    s_mod_len[idx] = len;
    apply_mod_precision(s_mod[idx], len);
    persist();
    return true;
}

uint16_t lora_channel_get_mod(uint8_t idx, uint8_t *out, uint16_t cap) {
    if (idx >= LORA_CH_MAX || !out || cap == 0) return 0;
    uint16_t c = s_mod_len[idx] < cap ? s_mod_len[idx] : cap;
    memcpy(out, s_mod[idx], c);
    return c;
}

uint8_t lora_channel_primary(void) {
    for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
        if (s_ch[i].used && s_ch[i].role == LORA_CH_PRIMARY) return i;
    }
    return 0;
}

uint8_t lora_channel_hash_of(uint8_t idx) {
    if (idx >= LORA_CH_MAX) return 0;
    return s_ch[idx].hash;
}

int lora_channel_lookup_hash(uint8_t hash) {
    if (!s_init) lora_channels_init();
    // hash 0x00 is a valid air hash (uint8); miss is reported as int -1.
    for (uint8_t i = 0; i < LORA_CH_MAX; i++) {
        if (s_ch[i].used && s_ch[i].role != LORA_CH_DISABLED &&
            s_ch[i].hash == hash)
            return (int)i;
    }
    return -1;
}

uint8_t lora_channel_key(uint8_t idx, uint8_t *out32) {
    if (idx >= LORA_CH_MAX || !out32) return 0;
    // Secondary with empty PSK falls back to the primary key for crypt
    // (mirrors the hash path in recompute()).
    return effective_key(idx, out32);
}

bool lora_channel_has_default(void) {
    if (!s_init) lora_channels_init();
    int pi = primary_idx();
    const char *nm = (pi >= 0) ? s_ch[pi].name : "";
    if (is_default_name(nm)) return true;
    // Named-but-stock preset channels (LongFast, MediumFast, ...) also use
    // the default frequency slot upstream.
    for (int p = 0; p < LORA_PRESET_COUNT; p++) {
        const char *dn = lora_preset_display_name(p, true);
        if (dn && strcmp(nm, dn) == 0) return true;
    }
    return false;
}

bool lora_channel_uses_default_slot(uint8_t idx) {
    (void)idx; // slot selection is a primary-channel property upstream
    bool dflt = lora_channel_has_default();
    ESP_LOGI(TAG, "uses_default_frequency_slot=%u", (unsigned)dflt);
    return dflt;
}

#else
typedef int lora_channels_stub_guard;
#endif
