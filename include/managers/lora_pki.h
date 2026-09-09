// lora_pki.h
// Meshtastic PKI (direct messages): X25519 + SHA256 + AES-256-CCM.
// Reference: meshtastic/firmware src/mesh/CryptoEngine.cpp
//   encryptCurve25519/decryptCurve25519 (DH, SHA256(shared), AES-CCM with
//   13-byte nonce, 8-byte tag + 4-byte extraNonce = 12B overhead) +
//   src/mesh/Router.cpp perhapsEncode/perhapsDecode +
//   protobufs mesh.proto (MeshPacket.public_key=16, pki_encrypted=17,
//   User.public_key=8).
// Nonce layout is byte-exact with CryptoEngine::initNonce: [packetId u64 LE]
// at [0..7], [fromNode u32 LE] at [8..11], and the random extraNonce
// (u32 LE, TX-generated) overwrites [4..7] when nonzero. Meshtastic's
// aes-ccm implementation fixes L=2, so AES-CCM consumes nonce[0..12]
// (13 bytes); byte 12 remains zero (see build_nonce in lora_pki.c).
// X25519 itself is a vendored public-domain scalarmult (curve25519-donna);
// no hand-rolled curve math in tree.

#ifndef LORA_PKI_H
#define LORA_PKI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_PKI_KEYLEN 32
#define LORA_PKI_OVERHEAD 12
#define LORA_PKI_CCM_NONCE_LEN 13

// ---- PortNum constants (meshtastic/protobufs portnums.proto) ----
// NOTE: TRACEROUTE_APP is 70, not 6 or 7: ADMIN_APP=6,
// TEXT_MESSAGE_COMPRESSED_APP=7. The PKI port exclusion covers 3/4/5/70
// only; ADMIN stays PKI-eligible (remote-admin DMs are PKI-encrypted).
#define LORA_PORT_TEXT 1
#define LORA_PORT_POSITION 3
#define LORA_PORT_NODEINFO 4
#define LORA_PORT_ROUTING 5
#define LORA_PORT_ADMIN 6
#define LORA_PORT_TRACEROUTE 70

// Routing NAK code the phone path returns when a DM is refused for lack of
// a peer public key (lora_phoneapi.c admin_ack path; docs
// lora-meshtastic-app.md). Never send the DM channel-encrypted instead.
#define LORA_PKI_SEND_FAIL_PUBLIC_KEY 39

void lora_pki_init(void); // NVS load-or-generate keypair
bool lora_pki_has_key(void);
const uint8_t *lora_pki_public(void); // 32B, NULL if none
const uint8_t *lora_pki_private(void); // 32B, NULL if none
// Regenerate keypair (admin / CLI). Returns false on RNG failure.
bool lora_pki_regen(void);
// Install an explicit private key (admin SecurityConfig). Recomputes pubkey.
bool lora_pki_set_private(const uint8_t *priv32);
// Authorized admin keys (remote-admin allowlist, stored, up to 3).
uint8_t lora_pki_admin_count(void);
bool lora_pki_admin_get(uint8_t idx, uint8_t *out32);
bool lora_pki_admin_set(const uint8_t *keys, uint8_t count);

// Encrypt Data-plaintext for a DM to a peer.
// out must hold plen+12B. Returns ciphertext len (0 = fail: no key/peer key).
uint16_t lora_pki_encrypt(uint32_t to_node, uint32_t from_node,
                          const uint8_t *peer_pub32,
                          uint32_t packet_id,
                          const uint8_t *pt, uint16_t plen,
                          uint8_t *out, uint16_t out_cap);
// Decrypt a DM ciphertext from a peer. Returns plaintext len (0 = fail).
uint16_t lora_pki_decrypt(uint32_t from_node,
                          const uint8_t *peer_pub32,
                          uint32_t packet_id,
                          const uint8_t *ct, uint16_t ct_len,
                          uint8_t *out, uint16_t out_cap);

// ---- Decrypt-variant probe helpers (diagnostic, no behavior change) ----
// Raw X25519 shared secret (no SHA256); false on NULL/weak-point/no-key.
bool lora_pki_dh_raw(const uint8_t *peer_pub32, uint8_t out32[32]);
// In-place SHA256 of a 32B buffer (the key-derivation step).
void lora_pki_sha256_32(uint8_t io[32]);
// Standard CryptoEngine::initNonce bytes (wraps build_nonce in lora_pki.c).
void lora_pki_nonce(uint8_t n16[16], uint32_t from, uint64_t id,
                    uint32_t extra);

// Run fixed, non-secret crypto known-answer tests. Returns a failure mask:
// bit 0/1 = RFC 7748 X25519 vector 1/2, bit 2 = SHA-256(00..1f),
// bit 3 = AES-256-CCM-8 encrypt/decrypt with a 13-byte nonce.
// Zero means every test passed. Diagnostic only; does not touch live keys/NVS.
uint8_t lora_pki_selftest(void);

// ---- Router::perhapsEncode/perhapsDecode policy helpers (pure, no HW) ----
// Expected mesh-layer call order (for the lora_mesh.c RX/TX paths):
//   1. RX: if lora_pki_should_try_first(hdr_channel, to) holds (channel byte
//      0 + unicast shape, with to == self verified by the caller), attempt
//      PKI decrypt first with the sender's known public key.
//   2. RX: otherwise run the channel-key loop (each configured channel +
//      default-preset fallback).
//   3. RX: if a frame channel-decrypts but lora_pki_is_legacy_dm(port, to,
//      from) is true (unicast TEXT to self without PKI), return
//      DECODE_FAILURE ("Rejecting legacy DM") instead of delivering.
//   4. TX: emit PKI only if lora_pki_can_encode(port) AND the peer key is
//      known; unknown key -> refuse with LORA_PKI_SEND_FAIL_PUBLIC_KEY,
//      never fall back to a channel-encrypted (legacy) DM
//      (see lora_pki_must_refuse_no_key).
// Upstream reference: meshtastic/firmware src/mesh/Router.cpp
// perhapsEncode/perhapsDecode.

// PKI-eligibility by port (perhapsEncode): false for POSITION / NODEINFO /
// ROUTING / TRACEROUTE (always channel-encrypted); true otherwise,
// including TEXT and ADMIN (remote-admin DMs are PKI).
bool lora_pki_can_encode(uint32_t portnum);

// Wire-shape test for PKI-first RX (perhapsDecode): true iff channel_byte
// == 0 and `to` is a unicast address (not broadcast 0xFFFFFFFF / zero).
// The caller must additionally require to == self before attempting PKI
// decrypt (this helper deliberately takes no node DB dependency).
bool lora_pki_should_try_first(uint8_t channel_byte, uint32_t to);

// Legacy-DM test: true iff a channel-decrypted frame is a unicast TEXT
// message (to not broadcast/zero, from != to). Call only for frames
// addressed to self; true means return DECODE_FAILURE ("Rejecting legacy
// DM") — stock drops channel-encrypted DMs instead of delivering them.
bool lora_pki_is_legacy_dm(uint32_t port, uint32_t to, uint32_t from);

// TX refusal test: true when the peer public key is unknown
// (!have_peer_key). True means refuse the DM with
// LORA_PKI_SEND_FAIL_PUBLIC_KEY; never send it channel-encrypted.
bool lora_pki_must_refuse_no_key(bool have_peer_key);

#ifdef __cplusplus
}
#endif

#endif // LORA_PKI_H
