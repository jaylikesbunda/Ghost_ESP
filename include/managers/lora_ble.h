// lora_ble.h
// Meshtastic-compatible BLE transport for the official app.
// Reference: meshtastic/firmware src/BluetoothCommon.h (UUIDs) +
//   src/nimble/NimbleBluetooth.cpp (FromRadio READ, ToRadio WRITE,
//   FromNum NOTIFY doorbell, want_config sequence, single connection).
// Transport parity notes (NimbleBluetooth.cpp):
//  - 512B ToRadio writes accepted (error only above 512B); FromRadio reads
//    served from a 512B-capable buffer. NOTE: the PhoneAPI FIFO slot
//    (LORA_PHONE_SLOT, owned by lora_phoneapi.h) is still 288B, so live
//    FromRadio frames cap there until that slot grows.
//  - ToRadio queue depth 3 (matches TO_PHONE) + identical-consecutive-write
//    dedup; both queues flushed on disconnect/unsubscribe/stop.
//  - ATT MTU 517 + 2M PHY + 251B data-length attempted best-effort (guarded,
//    non-fatal); MTU changes logged.
//  - Battery service 0x180F / level 0x2A19 (read-only, fixed 100%) is hosted
//    and advertised; LogRadio NOTIFY|READ stub returns empty reads.
//  - Fast conn params during the config handshake, low-power after.
// Framing/sequencing lives in lora_phoneapi.c; this file only moves bytes.

#ifndef LORA_BLE_H
#define LORA_BLE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start advertising the Meshtastic service. Refuses (false) when the
// controller is busy with scans/advertising owned by someone else —
// stop those first (`stopscan`, `blespam -s`). Safe to call twice.
bool lora_ble_start(void);
void lora_ble_stop(void); // stop adv, keep host up for other users
bool lora_ble_is_advertising(void);
bool lora_ble_is_connected(void); // raw link (app may not be subscribed yet)
bool lora_ble_is_linked(void);    // subscribed: the UI/session truth

// Called by phoneapi paths to ring the FromNum doorbell after fifo_push.
void lora_ble_notify_from_num(void);

#ifdef __cplusplus
}
#endif

#endif // LORA_BLE_H
