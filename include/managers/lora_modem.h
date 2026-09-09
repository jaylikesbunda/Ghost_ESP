// lora_modem.h
// Meshtastic modem presets + custom modem path.
// Reference: meshtastic/firmware RadioInterface::applyModemConfig +
//   RadioInterface::modemPresetToParams + DisplayFormatters +
//   protobufs config.proto LoRaConfig.ModemPreset + RegionCode +
//   RegionInfo regions[] (powerLimit) + RadioInterface::applyModemConfig
//   frequency selection (floor slot math, override wins, wideLora fallback).
// Preset display names match upstream for all 17 ids (channel-hash compat);
// air params (SF/BW/CR) only exist upstream for 0,1,3-9 -- ids 2,10-16
// fall back to LongFast air (11/250/5) per modemPresetToParams.

#ifndef LORA_MODEM_H
#define LORA_MODEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Upstream ModemPreset ids (config.proto). VERY_LONG_SLOW/LONG_SLOW are
// deprecated upstream but still accepted for interop.
typedef enum {
    LORA_PRESET_LONG_FAST = 0,
    LORA_PRESET_LONG_SLOW = 1,
    LORA_PRESET_VERY_LONG_SLOW = 2,
    LORA_PRESET_MEDIUM_SLOW = 3,
    LORA_PRESET_MEDIUM_FAST = 4,
    LORA_PRESET_SHORT_SLOW = 5,
    LORA_PRESET_SHORT_FAST = 6,
    LORA_PRESET_LONG_MODERATE = 7,
    LORA_PRESET_SHORT_TURBO = 8,
    LORA_PRESET_LONG_TURBO = 9,
    LORA_PRESET_LITE_FAST = 10,
    LORA_PRESET_LITE_SLOW = 11,
    LORA_PRESET_NARROW_FAST = 12,
    LORA_PRESET_NARROW_SLOW = 13,
    LORA_PRESET_TINY_FAST = 14,
    LORA_PRESET_TINY_SLOW = 15,
    LORA_PRESET_MEDIUM_TURBO = 16,
} lora_preset_t;

#define LORA_PRESET_COUNT 17
#define LORA_PRESET_INVALID -1

// Upstream RegionCode ids relevant to modem/freq helpers (config.proto).
// UNSET(0) has no band; helpers return the LongFast US default (906.875MHz).
#define LORA_REGION_UNSET 0
#define LORA_REGION_LORA24 13 // 2.4GHz band (wideLora BWs; SX126x falls back)
#define LORA_REGION_UA868 15 // Ukraine 868MHz
// Licensed HAM bands (is_licensed bypass only): 27..37 inclusive.
#define LORA_REGION_HAM_FIRST 27
#define LORA_REGION_HAM_LAST 37

typedef struct {
    int preset; // lora_preset_t, 0..16
    const char *name; // display name used for channel-hash default
    const char *short_name;
    int sf; // 7..12
    int bw_khz; // 125/250/500 (clamped view of the preset)
    int cr; // denominator 5..8 (CR 4/5..4/8)
} lora_preset_info_t;

const lora_preset_info_t *lora_preset_info(int preset);
int lora_preset_sf(int preset);
int lora_preset_bw_khz(int preset);
int lora_preset_cr(int preset);
// Display name for channel-hash default. Falls back to "LongFast".
const char *lora_preset_display_name(int preset, bool use_preset);
bool lora_preset_is_valid(int preset);

// Resolve effective modem params. When use_preset=true, preset table wins
// (custom cr override still honored when 5..8, matching upstream).
// When use_preset=false, sf/bw/cr args are used directly (validated).
// Returns false when neither path yields a legal combo.
bool lora_modem_resolve(bool use_preset, int preset, int sf, int bw_khz, int cr,
                        int *out_sf, int *out_bw_khz, int *out_cr);

// Persisted radio-modem config (NVS "lora" namespace).
typedef struct {
    bool use_preset;
    int preset; // 0..16
    int sf; // custom path only
    int bw_khz; // custom path only (125/250/500)
    int cr; // custom path only, 5..8
    float freq_offset_mhz; // added to computed slot freq
    float override_freq_mhz; // >0: verbatim freq, skips slot math
    uint32_t channel_num; // 0 = hash-derived (upstream default)
    bool tx_enabled;
} lora_modem_cfg_t;

void lora_modem_cfg_load(lora_modem_cfg_t *out);
bool lora_modem_cfg_save(const lora_modem_cfg_t *cfg);

// Effective air params after resolve (SF/BW/CR + whether BW was clamped).
bool lora_modem_effective(const lora_modem_cfg_t *cfg,
                          int *out_sf, int *out_bw_khz, int *out_cr);

// Region power limits (upstream RegionInfo regions[] powerLimit, dBm).
// Returns the regulatory cap for a RegionCode; unknown codes return the
// upstream default (30, SX1262 path caps to 22 / SX1276-class paths to 17).
int lora_modem_power_limit(int region_code);
// Clamp a requested TX power to [sane range + region limit]. is_licensed
// bypasses the region limit (HAM bands) but never the SX1262 22dBm ceiling.
// The lora_manager apply path should call this before persisting tx_power.
int lora_modem_clamp_tx(int region_code, int tx_dbm, bool is_licensed);

// Slot math matching RadioInterface::applyModemConfig: floor(), not round(),
// when spacing>0. slots = floor(span/bw); slot freq = start+bw/2+slot*bw.
int lora_modem_num_slots(float start_mhz, float end_mhz, int bw_khz);
uint32_t lora_modem_slot_hz(float start_mhz, float end_mhz, int bw_khz,
                            uint32_t slot, float offset_mhz);
// True when bw_khz is a 2.4GHz wideLora BW (>500): SX126x has no such BW,
// callers must fall back to LongFast air + log.
bool lora_modem_is_wide_bw(int bw_khz);
// Supplemental region spans NOT in lora_mesh.c (UA_868, HAM 27-37,
// EU_866/EU_874/EU_917 sub-bands, LORA24, UNSET guard). Returns false when
// the code is unknown here too (caller keeps its previous default).
// HAM spans are placeholders (70cm) -- air use additionally requires
// is_licensed; callers log "HAM not supported, using LongFast params".
bool lora_modem_region_span(int code, float *out_start_mhz,
                            float *out_end_mhz, const char **out_name);

#ifdef __cplusplus
}
#endif

#endif // LORA_MODEM_H
