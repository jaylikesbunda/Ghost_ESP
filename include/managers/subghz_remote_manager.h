#ifndef SUBGHZ_REMOTE_MANAGER_H
#define SUBGHZ_REMOTE_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "managers/subghz_decoders.h"

#define SUBGHZ_SCANNER_CHANNEL_COUNT 64
#define SUBGHZ_STREAM_VERSION 2
#define SUBGHZ_STREAM_WATERFALL_LINE 0x20
#define SUBGHZ_STREAM_WATERFALL_CHUNK 0x21
#define SUBGHZ_SNAPSHOT_NAME_MAX 64
#define SUBGHZ_RAW_MAX_DURATIONS 4096
#define SUBGHZ_RAW_INITIAL_CAP 1024
#define SUBGHZ_RAW_MAX_CAP 16384

typedef enum {
    SUBGHZ_PRESET_OOK270_ASYNC = 0,
    SUBGHZ_PRESET_OOK650_ASYNC = 1,
    SUBGHZ_PRESET_2FSK_DEV238_ASYNC = 2,
    SUBGHZ_PRESET_2FSK_DEV476_ASYNC = 3,
    SUBGHZ_PRESET_CUSTOM = 4,
    SUBGHZ_PRESET_UNKNOWN = 5,
} subghz_preset_t;

bool subghz_remote_manager_start(bool stream_to_peer);
bool subghz_remote_manager_start_waterfall(bool stream_to_peer);
void subghz_remote_manager_stop(void);
void subghz_remote_manager_set_paused(bool paused);
bool subghz_remote_manager_is_running(void);
bool subghz_remote_manager_is_paused(void);
bool subghz_remote_manager_is_ready(void);
bool subghz_remote_manager_begin_capture(bool raw_mode, uint32_t frequency_hz, bool stream_to_peer, uint32_t timeout_ms);
const char *subghz_remote_manager_get_last_error(void);
bool subghz_remote_manager_get_levels(uint8_t *out_levels, size_t max_levels, uint8_t *out_cursor);
bool subghz_remote_manager_take_waterfall_line(uint8_t *out_levels, size_t max_levels, uint8_t *out_count, uint8_t *out_freq_idx, uint16_t *out_seq);
bool subghz_remote_manager_take_raw_capture(int32_t *out_durations, size_t max_durations, size_t *out_count);
bool subghz_remote_manager_take_decode_result(subghz_decoded_signal_t *out_result);
bool subghz_remote_manager_transmit_raw(const int32_t *durations, size_t count, uint32_t frequency_hz, subghz_preset_t preset);
void subghz_remote_manager_register_stream_handler(void);
void subghz_remote_manager_set_raw_capture_enabled(bool enabled);
/* True while a T-Embed CC1101 local (non-streamed) capture is armed. Lets the
 * view keep the screen static to avoid display-SPI EMI bursts on GDO0. */
bool subghz_remote_manager_is_tembed_local_capture(void);
/* T-Embed CC1101 RX engine select for local capture: false = GPIO ISR +
 * local RCSwitch accumulation (default), true = RMT hardware sampler
 * (hardware 3us glitch filter + 30ms idle detection in silicon). Applies on
 * the next capture arm; runtime-switchable via `subghz rxmode <isr|rmt>`. */
void subghz_remote_manager_set_tembed_rx_use_rmt(bool use_rmt);
bool subghz_remote_manager_get_tembed_rx_use_rmt(void);
void subghz_remote_manager_cycle_frequency(void);
bool subghz_remote_manager_set_frequency_hz(uint32_t frequency_hz);
const char *subghz_remote_manager_get_frequency_label(void);
uint32_t subghz_remote_manager_get_frequency_hz(void);
bool subghz_remote_manager_capture_snapshot(const char *name_hint);
bool subghz_remote_manager_save_snapshot(const char *name_hint, char *out_path, size_t out_path_len);
bool subghz_remote_manager_load_snapshot(const char *name_or_path);
int subghz_remote_manager_list_snapshots(char names[][SUBGHZ_SNAPSHOT_NAME_MAX], int max_names);
const char *subghz_remote_manager_get_active_snapshot_name(void);

/* Diagnostic: one-shot CC1101 SPI link probe. Logs raw VERSION/PARTNUM reads
 * under two conditions (bus held exclusively w/ LVGL suspended, vs. the real
 * per-op transfer path w/ LVGL running) to isolate SPI-transport corruption
 * from hardware/power faults and from the RX capture datapath. Serial-only. */
void subghz_remote_manager_diag_probe(void);
/* Live CC1101 config-register access for RF experiments (e.g. AGC tweaks
 * mid-capture). reg <= 0x2E. Returns false when the radio is not started. */
bool subghz_remote_manager_debug_reg_write(uint8_t reg, uint8_t val, uint8_t *out_readback);
bool subghz_remote_manager_debug_reg_read(uint8_t reg, uint8_t *out_val);

#endif
