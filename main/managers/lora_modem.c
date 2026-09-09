// lora_modem.c
// See lora_modem.h for reference.

#include "managers/lora_modem.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <string.h>

static const char *MODEM_TAG = "LoRaModem";

bool lora_modem_is_wide_bw(int bw_khz); // defined below; used by slot_hz

// Air params match upstream RadioInterface::modemPresetToParams, which has
// NO case for ids 2,10-16 -- those fall through to the LongFast default
// (SF11/BW250/CR4/5). Display names below still match upstream
// DisplayFormatters for all 17 ids (channel-hash compat); only the air
// columns for 2,10-16 are the LongFast fallback (marked unmapped).
static const lora_preset_info_t PRESETS[] = {
    {0, "LongFast", "LongF", 11, 250, 5},
    {1, "LongSlow", "LongS", 12, 125, 8},
    {2, "VeryLongSlow", "VLongS", 11, 250, 5}, // unmapped upstream -> LongFast air
    {3, "MediumSlow", "MedS", 10, 250, 5},
    {4, "MediumFast", "MedF", 9, 250, 5},
    {5, "ShortSlow", "ShortS", 8, 250, 5},
    {6, "ShortFast", "ShortF", 7, 250, 5},
    {7, "LongMod", "LongM", 11, 125, 8},
    {8, "ShortTurbo", "ShortT", 7, 500, 5},
    {9, "LongTurbo", "LongT", 11, 500, 8},
    {10, "LiteFast", "LiteF", 11, 250, 5}, // unmapped upstream -> LongFast air
    {11, "LiteSlow", "LiteS", 11, 250, 5}, // unmapped upstream -> LongFast air
    {12, "NarrowFast", "NarrF", 11, 250, 5}, // unmapped upstream -> LongFast air
    {13, "NarrowSlow", "NarrS", 11, 250, 5}, // unmapped upstream -> LongFast air
    {14, "TinyFast", "TinyF", 11, 250, 5}, // unmapped upstream -> LongFast air
    {15, "TinySlow", "TinyS", 11, 250, 5}, // unmapped upstream -> LongFast air
    {16, "MediumTurbo", "MedT", 11, 250, 5}, // unmapped upstream -> LongFast air
};

const lora_preset_info_t *lora_preset_info(int preset) {
    for (unsigned i = 0; i < sizeof(PRESETS) / sizeof(PRESETS[0]); i++) {
        if (PRESETS[i].preset == preset) return &PRESETS[i];
    }
    return NULL;
}

bool lora_preset_is_valid(int preset) {
    return lora_preset_info(preset) != NULL;
}

int lora_preset_sf(int preset) {
    const lora_preset_info_t *p = lora_preset_info(preset);
    return p ? p->sf : 11;
}

int lora_preset_bw_khz(int preset) {
    const lora_preset_info_t *p = lora_preset_info(preset);
    return p ? p->bw_khz : 250;
}

int lora_preset_cr(int preset) {
    const lora_preset_info_t *p = lora_preset_info(preset);
    return p ? p->cr : 5;
}

const char *lora_preset_display_name(int preset, bool use_preset) {
    if (!use_preset) return "Custom";
    const lora_preset_info_t *p = lora_preset_info(preset);
    if (!p) return "Invalid";
    // Display names match upstream DisplayFormatters for all 17 ids
    // (channel-hash compat); only air params for 2,10-16 fall back to
    // LongFast per modemPresetToParams (see PRESETS table).
    return p->name;
}

static bool bw_ok(int bw) {
    return bw == 125 || bw == 250 || bw == 500;
}

bool lora_modem_resolve(bool use_preset, int preset, int sf, int bw_khz, int cr,
                        int *out_sf, int *out_bw_khz, int *out_cr) {
    int rsf = 11, rbw = 250, rcr = 5;
    if (use_preset) {
        const lora_preset_info_t *p = lora_preset_info(preset);
        if (!p) return false;
        rsf = p->sf;
        rbw = p->bw_khz;
        rcr = p->cr;
        // Upstream honors an explicit coding_rate override even with presets.
        if (cr >= 5 && cr <= 8) rcr = cr;
    } else {
        if (sf < 5 || sf > 12) return false;
        if (!bw_ok(bw_khz)) return false;
        if (cr < 5 || cr > 8) return false;
        // SX126x constraint: SF6 requires implicit header (we run explicit).
        if (sf == 6) return false;
        if (bw_khz == 500 && sf < 7) return false;
        rsf = sf;
        rbw = bw_khz;
        rcr = cr;
    }
    if (out_sf) *out_sf = rsf;
    if (out_bw_khz) *out_bw_khz = rbw;
    if (out_cr) *out_cr = rcr;
    return true;
}

void lora_modem_cfg_load(lora_modem_cfg_t *out) {
    if (!out) return;
    out->use_preset = true;
    out->preset = 0;
    out->sf = 11;
    out->bw_khz = 250;
    out->cr = 5;
    out->freq_offset_mhz = 0;
    out->override_freq_mhz = 0;
    out->channel_num = 0;
    out->tx_enabled = true;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8 = 0;
    int32_t i = 0;
    uint32_t u32 = 0;
    if (nvs_get_u8(h, "use_preset", &u8) == ESP_OK) out->use_preset = u8 != 0;
    if (nvs_get_i32(h, "preset", &i) == ESP_OK && i >= 0 && i < LORA_PRESET_COUNT)
        out->preset = (int)i;
    if (nvs_get_i32(h, "sf", &i) == ESP_OK && i >= 5 && i <= 12) out->sf = (int)i;
    if (nvs_get_i32(h, "bw", &i) == ESP_OK && bw_ok((int)i)) out->bw_khz = (int)i;
    if (nvs_get_i32(h, "cr", &i) == ESP_OK && i >= 5 && i <= 8) out->cr = (int)i;
    if (nvs_get_i32(h, "freqoff_k", &i) == ESP_OK && i >= -2000 && i <= 2000)
        out->freq_offset_mhz = (float)i / 1000.0f;
    if (nvs_get_u32(h, "ovrfreq_k", &u32) == ESP_OK && u32 >= 150000 && u32 <= 960000)
        out->override_freq_mhz = (float)u32 / 1000.0f;
    if (nvs_get_u32(h, "chnum", &u32) == ESP_OK && u32 <= 512) out->channel_num = u32;
    if (nvs_get_u8(h, "txen", &u8) == ESP_OK) out->tx_enabled = u8 != 0;
    nvs_close(h);
}

bool lora_modem_cfg_save(const lora_modem_cfg_t *cfg) {
    if (!cfg) return false;
    int rsf = 0, rbw = 0, rcr = 0;
    if (!lora_modem_resolve(cfg->use_preset, cfg->preset, cfg->sf,
                            cfg->bw_khz, cfg->cr, &rsf, &rbw, &rcr))
        return false;
    if (cfg->preset < 0 || cfg->preset >= LORA_PRESET_COUNT) return false;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return false;
    nvs_set_u8(h, "use_preset", cfg->use_preset ? 1 : 0);
    nvs_set_i32(h, "preset", (int32_t)cfg->preset);
    nvs_set_i32(h, "sf", (int32_t)cfg->sf);
    nvs_set_i32(h, "bw", (int32_t)cfg->bw_khz);
    nvs_set_i32(h, "cr", (int32_t)cfg->cr);
    nvs_set_i32(h, "freqoff_k", (int32_t)(cfg->freq_offset_mhz * 1000.0f));
    nvs_set_u32(h, "ovrfreq_k", (uint32_t)(cfg->override_freq_mhz * 1000.0f));
    nvs_set_u32(h, "chnum", cfg->channel_num);
    nvs_set_u8(h, "txen", cfg->tx_enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    return true;
}

bool lora_modem_effective(const lora_modem_cfg_t *cfg,
                          int *out_sf, int *out_bw_khz, int *out_cr) {
    if (!cfg) return false;
    return lora_modem_resolve(cfg->use_preset, cfg->preset, cfg->sf,
                              cfg->bw_khz, cfg->cr, out_sf, out_bw_khz, out_cr);
}

// Upstream RegionInfo regions[] powerLimit (dBm). Confirmed entries
// (EU433 10, JP 13, KR 23) per task; remainder best-effort from firmware
// regions[] -- exact upstream values to be re-verified against
// RadioInterface regions[] on next sync. Unknown codes -> default 30
// (callers cap to the SX1262 22dBm / SX1276-class 17dBm ceilings).
static const struct { int code; int limit_dbm; } POWER_LIMITS[] = {
    {1, 30},   // US915
    {2, 10},   // EU433
    {3, 16},   // EU868 (EN300-220 25mW sub-bands)
    {4, 22},   // CN
    {5, 13},   // JP
    {6, 30},   // ANZ
    {7, 23},   // KR
    {8, 23},   // TW
    {9, 20},   // RU
    {10, 30},  // IN
    {11, 30},  // NZ865
    {12, 30},  // TH
    {13, 10},  // LORA24 (2.4GHz, EIRP-ish cap; SX126x unused)
    {14, 10},  // UA433
    {15, 20},  // UA868
    {16, 10},  // MY433
    {17, 30},  // MY919
    {18, 30},  // SG923
    {19, 10},  // PH433
    {20, 10},  // PH868
    {21, 30},  // PH915
    {22, 10},  // ANZ433
    {23, 10},  // KZ433
    {24, 20},  // KZ863
    {25, 30},  // NP865
    {26, 30},  // BR902
    // HAM 27-37: licensed bands, no regulatory cap enforced here --
    // is_licensed bypass applies (see lora_modem_clamp_tx). Listed as 30
    // so the bypass path is explicit, not a table miss.
    {27, 30}, {28, 30}, {29, 30}, {30, 30}, {31, 30},
    {32, 30}, {33, 30}, {34, 30}, {35, 30}, {36, 30}, {37, 30},
    {38, 16}, // EU866 sub-band
    {39, 16}, // EU874 sub-band
    {40, 16}, // EU917 sub-band (supplemental codes; confirm upstream)
};

int lora_modem_power_limit(int region_code) {
    for (unsigned i = 0; i < sizeof(POWER_LIMITS) / sizeof(POWER_LIMITS[0]); i++) {
        if (POWER_LIMITS[i].code == region_code) return POWER_LIMITS[i].limit_dbm;
    }
    return 30; // upstream default; SX1262 path caps to 22 below
}

int lora_modem_clamp_tx(int region_code, int tx_dbm, bool is_licensed) {
    int pwr = tx_dbm;
    if (!is_licensed) {
        int lim = lora_modem_power_limit(region_code);
        if (lim > 22) lim = 22; // SX1262 ceiling (SX1276-class paths: 17)
        if (pwr > lim) {
            ESP_LOGW(MODEM_TAG, "TX %ddBm exceeds region %d limit %ddBm, clamping",
                     pwr, region_code, lim);
            pwr = lim;
        }
    } else {
        ESP_LOGI(MODEM_TAG, "licensed TX bypass for region %d", region_code);
    }
    if (pwr > 22) pwr = 22;
    if (pwr < -9) pwr = -9;
    return pwr;
}

int lora_modem_num_slots(float start_mhz, float end_mhz, int bw_khz) {
    float bw_mhz = (float)bw_khz / 1000.0f;
    if (!(bw_mhz > 0)) bw_mhz = 0.25f;
    float span = end_mhz - start_mhz;
    // Upstream applyModemConfig uses floor() when spacing>0, not round().
    int slots = (int)floorf(span / bw_mhz);
    if (slots < 1) slots = 1;
    return slots;
}

uint32_t lora_modem_slot_hz(float start_mhz, float end_mhz, int bw_khz,
                            uint32_t slot, float offset_mhz) {
    (void)end_mhz;
    int bwk = bw_khz;
    if (lora_modem_is_wide_bw(bwk)) {
        // 2.4GHz wideLora BW requested on a sub-GHz SX126x path.
        ESP_LOGW(MODEM_TAG, "wideLora BW%d requested, using LongFast params", bwk);
        bwk = 250;
    }
    float bw_mhz = (float)bwk / 1000.0f;
    if (!(bw_mhz > 0)) bw_mhz = 0.25f;
    float f = start_mhz + bw_mhz / 2.0f + (float)slot * bw_mhz + offset_mhz;
    return (uint32_t)(f * 1000000.0f + 0.5f);
}

bool lora_modem_is_wide_bw(int bw_khz) {
    return bw_khz > 500; // SX126x tops at 500; above = 2.4GHz wideLora BWs
}

// Supplemental spans for codes absent from lora_mesh.c REGIONS[].
// UNSET(0) has no band (caller uses the 906.875MHz US LongFast default).
// UA_868 added per task; HAM 27-37 get 70cm placeholder spans (licensed-only,
// callers must log "HAM not supported, using LongFast params" and use
// LongFast air); EU_866/874/917 sub-bands + LORA24 complete the table so
// freq calc never falls through to the wrong default.
static const struct { int code; const char *name; float start; float end; } EXTRA_SPANS[] = {
    {13, "lora24", 2400.0f, 2483.5f},
    {15, "ua868", 868.0f, 868.6f},
    {27, "ham27", 430.0f, 440.0f},
    {28, "ham28", 430.0f, 440.0f},
    {29, "ham29", 430.0f, 440.0f},
    {30, "ham30", 430.0f, 440.0f},
    {31, "ham31", 430.0f, 440.0f},
    {32, "ham32", 430.0f, 440.0f},
    {33, "ham33", 430.0f, 440.0f},
    {34, "ham34", 430.0f, 440.0f},
    {35, "ham35", 430.0f, 440.0f},
    {36, "ham36", 430.0f, 440.0f},
    {37, "ham37", 430.0f, 440.0f},
    {38, "eu866", 865.0f, 867.0f},
    {39, "eu874", 873.0f, 875.0f},
    {40, "eu917", 916.0f, 918.0f},
};

bool lora_modem_region_span(int code, float *out_start_mhz,
                            float *out_end_mhz, const char **out_name) {
    if (code == LORA_REGION_UNSET) return false; // caller uses 906.875MHz default
    for (unsigned i = 0; i < sizeof(EXTRA_SPANS) / sizeof(EXTRA_SPANS[0]); i++) {
        if (EXTRA_SPANS[i].code == code) {
            if (out_start_mhz) *out_start_mhz = EXTRA_SPANS[i].start;
            if (out_end_mhz) *out_end_mhz = EXTRA_SPANS[i].end;
            if (out_name) *out_name = EXTRA_SPANS[i].name;
            if (code >= LORA_REGION_HAM_FIRST && code <= LORA_REGION_HAM_LAST) {
                ESP_LOGW(MODEM_TAG, "HAM not supported, using LongFast params");
            }
            return true;
        }
    }
    return false;
}

#else
typedef int lora_modem_stub_guard;
#endif
