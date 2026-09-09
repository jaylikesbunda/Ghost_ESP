// lora_phoneapi.h
// Meshtastic PhoneAPI facade for the official app over BLE.
// Reference: meshtastic/firmware src/mesh/PhoneAPI.h (state machine) +
//   src/nimble/NimbleBluetooth.cpp (transport: FromRadio READ, ToRadio WRITE,
//   FromNum NOTIFY doorbell, want_config_id -> config sequence).
// RAM: 3 x 288B transport FIFO.  The service backlog remains 8 packets on
// no-PSRAM boards and expands to native ESP32-S3 depth (32) in PSRAM.

#ifndef LORA_PHONEAPI_H
#define LORA_PHONEAPI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/lora_pb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_PHONE_FIFO_DEPTH 3
#define LORA_PHONE_SLOT 288

// Lifecycle (called by lora_ble / lora_manager, single-threaded side)
// Reset transport/config session state while preserving received MeshPackets
// queued for the next phone connection (upstream toPhoneQueue semantics).
void lora_phoneapi_reset(void);
bool lora_phoneapi_is_linked(void);   // subscribed, not merely connected
void lora_phoneapi_set_linked(bool linked);

// Outbound (radio -> phone). Queued; caller learns via has_data().
void lora_phoneapi_push_mesh_text(uint32_t from, uint32_t to, uint32_t id,
                                  float snr, uint32_t hop_limit, uint32_t hop_start,
                                  const char *text);
void lora_phoneapi_push_mesh_text_ch(uint32_t from, uint32_t to, uint32_t id,
                                     float snr, uint32_t hop_limit, uint32_t hop_start,
                                     uint32_t channel_idx, const char *text);
// RX-forward variants with reception metadata + actual hop counts.
void lora_phoneapi_push_mesh_text_rx(uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     uint32_t channel_idx, const char *text,
                                     const pb_rx_meta_t *meta);
bool lora_phoneapi_push_mesh_data_rx(uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     const uint8_t *data, uint16_t dlen,
                                     const pb_rx_meta_t *meta);
bool lora_phoneapi_push_mesh_data_pki_rx(uint32_t from, uint32_t to, uint32_t id,
                                         uint32_t channel_idx,
                                         uint32_t hop_limit, uint32_t hop_start,
                                         const uint8_t *sender_pub32,
                                         const uint8_t *data, uint16_t dlen,
                                         const pb_rx_meta_t *meta);
// Generic Data push (ACKs, admin replies). Returns true if queued.
bool lora_phoneapi_push_mesh_data(uint32_t from, uint32_t to, uint32_t id,
                                   const uint8_t *data, uint16_t dlen);
// PKI variant: MeshPacket carries public_key + pki_encrypted for DMs.
bool lora_phoneapi_push_mesh_data_pki(uint32_t from, uint32_t to, uint32_t id,
                                      uint32_t channel_idx,
                                      const uint8_t *sender_pub32,
                                      const uint8_t *data, uint16_t dlen);
bool lora_phoneapi_push_mesh_node(uint32_t num, const char *id_str,
                                  const char *long_name, const char *short_name,
                                  float snr);
bool lora_phoneapi_push_mesh_node_pki(uint32_t num, const char *id_str,
                                      const char *long_name, const char *short_name,
                                      float snr, const uint8_t *pub32);
bool lora_phoneapi_push_mesh_node_full(uint32_t num, const char *id_str,
                                       const char *long_name, const char *short_name,
                                       float snr, const uint8_t *pub32,
                                       bool verified, bool muted,
                                       bool hops_valid, uint8_t hops_away,
                                       uint32_t last_heard,
                                       uint32_t hw_model, uint32_t role);
bool lora_phoneapi_has_data(void);
bool lora_phoneapi_push_mesh_raw(const uint8_t *mesh, uint16_t ml);
bool lora_phoneapi_config_active(void);
// Pops oldest into out (cap 288+). Returns len, 0 = empty.
uint16_t lora_phoneapi_pop(uint8_t *out, uint16_t cap);
uint32_t lora_phoneapi_from_num(void); // doorbell counter for FromNum notify

// Inbound (phone -> radio). Returns true if it produced mesh TX.
bool lora_phoneapi_on_toradio(const uint8_t *p, uint16_t len);

// Config-sequence trigger (ToRadio want_config_id).
void lora_phoneapi_begin_config(uint32_t nonce);

// Pump pending config steps (called after pops / new sessions).
void lora_phoneapi_pump(void);

// Reboot counter init (call once from manager early_init).
void lora_phoneapi_boot(void);

// Admin dispatch (port 6). Returns true if any frame was queued.
bool lora_phoneapi_handle_admin(const toradio_t *t, uint32_t me, uint32_t dst);

// Stats for `lora app status`.
void lora_phoneapi_stats(uint32_t *pushed, uint32_t *popped, uint32_t *dropped,
                         bool *linked);

#ifdef __cplusplus
}
#endif

#endif // LORA_PHONEAPI_H
