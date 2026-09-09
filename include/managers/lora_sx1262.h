// lora_sx1262.h
// Minimal SX1262 driver for GhostESP LoRa (bare-metal SPI, no RadioLib).
// Single static instance, no mallocs on RX/TX hot paths. Heltec V3 tuned
// (DIO2 RF-switch + board-selected DIO3 TCXO voltage) with board quirks in lora_hw_t.

#ifndef LORA_SX1262_H
#define LORA_SX1262_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/lora_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*lora_rx_cb_t)(const uint8_t *payload, uint8_t len,
                             int16_t rssi, float snr, void *ctx);

// Bring up SPI + radio with params from hw/region. Returns ESP_OK on ready.
int lora_radio_init(const lora_hw_t *hw, uint32_t freq_hz, int sf, int bw_khz, int tx_dbm);
int lora_radio_init_ex(const lora_hw_t *hw, uint32_t freq_hz, int sf, int bw_khz, int tx_dbm, int cr_denom);
void lora_radio_deinit(void);
bool lora_radio_is_ready(void);
// Last failed init stage ("none" when healthy). Valid after a failed init.
const char *lora_radio_step(void);

// RX-continuous with user callback (invoked from radio task, not ISR).
int lora_radio_start_rx(lora_rx_cb_t cb, void *ctx);
void lora_radio_stop(void);

// Blocking-ish TX (CAD + timeout inside). len 1..240.
int lora_radio_send(const uint8_t *data, uint8_t len);

// Regional TX-power enforcement (upstream RegionInfo powerLimit).
// set_region_tx_limit records region+licensed state; init_ex clamps through
// lora_modem_clamp_tx() so the RF path is capped even if the apply path
// passed a stale value. is_licensed bypasses the region cap (HAM) but never
// the SX1262 22dBm ceiling.
void lora_radio_set_region_tx_limit(int region_code, bool is_licensed);
int lora_radio_clamp_tx_dbm(int region_code, int tx_dbm, bool is_licensed);

// Channel activity detect: true = busy.
bool lora_radio_cad(void);
int lora_radio_rssi_inst(int16_t *out_rssi);
// Diagnostic register read, up to 8 consecutive bytes.
int lora_radio_read_register(uint16_t addr, uint8_t *out, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif // LORA_SX1262_H
