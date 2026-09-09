// lora_store.h
// Generic persistence for Config sections + ModuleConfig blobs.
// The phone is the source of truth for fields we don't otherwise apply
// (position, power, network, display, ... + all 17 module configs): we
// store the raw Config/ModuleConfig submessage bytes and report them
// verbatim in the handshake, so the app converges and survives reboots.

#ifndef LORA_STORE_H
#define LORA_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_CFG_SECTIONS 10
#define LORA_MOD_SLOTS 17
#define LORA_STORE_BLOB_MAX 160

void lora_store_init(void);
// section 1..10 (Config oneof). Returns len (0 = empty/default).
uint16_t lora_store_cfg_get(uint8_t section, uint8_t *out, uint16_t cap);
bool lora_store_cfg_set(uint8_t section, const uint8_t *data, uint16_t len);
// module 1..17 (ModuleConfig oneof). Returns len (0 = empty/disabled).
uint16_t lora_store_mod_get(uint8_t module, uint8_t *out, uint16_t cap);
bool lora_store_mod_set(uint8_t module, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif // LORA_STORE_H
