// lora_crypto.h
// Meshtastic channel crypto (AES-CTR per channel; PKI direct messages live
// in lora_pki.h).
// Reference: meshtastic/firmware src/mesh/CryptoEngine.cpp +
//   src/mesh/Channels.cpp + meshtastic/channel.proto.
//   - Channel PSK -> AES-128/256 CTR, nonce = [packetId u64 LE][from u32 LE],
//     4-byte BE counter starting at 0 (counter_size=4). Byte-exact with
//     CryptoEngine::initNonce (packetId LE64 at [0..7], fromNode LE32 at
//     [8..11]); bytes [12..15] carry the BE block counter instead of the
//     PKI extraNonce (channel frames have no extraNonce).
//   - Default channel: 1-byte PSK 0x01 expands to the 16-byte default key.
//   - Channel hash byte = xor(name) ^ xor(key bytes) (generateHash).

#ifndef LORA_CRYPTO_H
#define LORA_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 16-byte default channel key (channel.proto: {0xd4..0x01}).
extern const uint8_t LORA_DEFAULT_KEY[16];
// Display name of the default preset, used for the channel hash.
#define LORA_DEFAULT_PRESET_NAME "LongFast"

// Init default-key context. Runs a loopback self-test; returns false on
// crypto failure (caller must refuse air interop and say so).
bool lora_crypto_init(void);

// In-place AES-CTR crypt with the default key. CTR is symmetric.
void lora_crypto_crypt(uint32_t from, uint64_t packet_id,
                       uint8_t *data, size_t len);

// In-place AES-CTR with an explicit key (per-channel). key_len 16 (AES-128)
// or 32 (AES-256); key_len 0 = no crypto (memcpy noop, returns silently).
void lora_crypto_crypt_key(const uint8_t *key, uint8_t key_len,
                           uint32_t from, uint64_t packet_id,
                           uint8_t *data, size_t len);

// Channel-hash byte for TX headers (xorHash(name) ^ xorHash(key)).
uint8_t lora_channel_hash(const char *name, const uint8_t *key, size_t key_len);

#ifdef __cplusplus
}
#endif

#endif // LORA_CRYPTO_H
