// lora_channels.h
// 8 Meshtastic channels with PSK expansion + XOR hash.
// Reference: meshtastic/firmware src/mesh/Channels.cpp (xorHash, getKey
//   1-byte PSK expansion to default key, empty name -> preset display name,
//   "Default" name treated as empty, secondary with empty PSK inherits the
//   primary key for hash+crypt, single PRIMARY demotion) + channel.proto
//   roles. Hash 0x00 is a VALID air hash (e.g. empty name + no key);
//   lookup misses report -1 (int), never a hash byte.

#ifndef LORA_CHANNELS_H
#define LORA_CHANNELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_CH_MAX 8
#define LORA_CH_PSK_MAX 32
#define LORA_CH_NAME_MAX 32
#define LORA_CH_MOD_MAX 12 // ChannelSettings.module_settings opaque cap

typedef enum {
    LORA_CH_DISABLED = 0,
    LORA_CH_PRIMARY = 1,
    LORA_CH_SECONDARY = 2,
} lora_ch_role_t;

typedef struct {
    bool used;
    uint8_t role; // lora_ch_role_t
    uint8_t psk_len; // 0 (no crypto), 1 (shorthand), 16, 32
    uint8_t psk[LORA_CH_PSK_MAX];
    uint8_t hash; // air header hint
    char name[LORA_CH_NAME_MAX];
    bool uplink;
    bool downlink;
    uint32_t id; // app-assigned fixed32 id
} lora_channel_t;

void lora_channels_init(void);
// Default display name used when a channel name is empty (preset name).
void lora_channels_set_default_name(const char *name);
uint8_t lora_channels_count(void);
const lora_channel_t *lora_channel_get(uint8_t idx);
// psk may be NULL to keep; psk_len 0 clears to no-crypto (secondary only).
bool lora_channel_set(uint8_t idx, const char *name, const uint8_t *psk,
                      uint8_t psk_len, uint8_t role, bool uplink, bool downlink);
bool lora_channel_set_id(uint8_t idx, uint32_t id);
bool lora_channel_disable(uint8_t idx);
// Opaque ChannelSettings.module_settings=7 per channel (ModuleSettings:
// position_precision=1, is_muted=2). Persisted in NVS ("ch_mod" sidecar, so
// the "channels" blob stays layout-stable); position_precision is honored
// via lora_mesh_set_position_precision on set and on load. len==0 clears.
bool lora_channel_set_mod(uint8_t idx, const uint8_t *mod, uint8_t len);
uint16_t lora_channel_get_mod(uint8_t idx, uint8_t *out, uint16_t cap);
uint8_t lora_channel_primary(void); // index of PRIMARY, 0 fallback
uint8_t lora_channel_hash_of(uint8_t idx);
int lora_channel_lookup_hash(uint8_t hash); // idx or -1
// Expanded AES key for a channel (1-byte shorthand -> default key variant).
// Secondary with empty PSK inherits the primary key (upstream Channels).
// Returns key len 0 (no crypto), 16 or 32. out must hold 32B.
uint8_t lora_channel_key(uint8_t idx, uint8_t *out32);
// Hash input helper (XOR name + key), exposed for tests.
uint8_t lora_ch_hash_bytes(const char *name, const uint8_t *key, uint8_t key_len);
// Upstream RadioInterface::uses_default_frequency_slot / hasDefaultChannel
// equivalent: true when the primary channel is a default channel, i.e. its
// name is empty, "Default", or a known preset display name (LongFast,
// MediumFast, ...). Channel_num!=0 or a custom name means a hashed slot.
// Currently informational (callers log it); kept for the future freq path.
bool lora_channel_has_default(void);
bool lora_channel_uses_default_slot(uint8_t idx);

#ifdef __cplusplus
}
#endif

#endif // LORA_CHANNELS_H
