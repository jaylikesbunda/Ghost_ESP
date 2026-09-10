#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rssi_meter: a lightweight live signal-strength view used by the Track AP /
 * Track Station options. It draws a pulsating ring whose colour follows the
 * RSSI, with the dBm value centred inside the ring and the tracked target name
 * shown as subtext at the bottom. Displayed readings are EMA-smoothed and a
 * trend arrow appears beside the value once enough movement accumulates
 * (warmer/colder); flat or stale reads show no arrow. It mirrors the overlay flow used by the scan
 * spinner (gui/scan_status) and detail_view: a single heap allocation, parented
 * onto lv_scr_act() so the shared options touch bar stays visible underneath.
 *
 * All state lives in the heap struct; the only caller-side state needed is the
 * opaque handle returned here.
 */
typedef struct rssi_meter_t rssi_meter_t;

/*
 * Sampler invoked on the meter's own timer. Writes the current RSSI to
 * *out_rssi and returns true when the reading is "live" (a recent packet), or
 * false when the signal is stale so the meter can dim the ring.
 */
typedef bool (*rssi_meter_sample_cb)(void *user, int8_t *out_rssi);

/*
 * Create the meter as a child of `parent` (defaults to lv_scr_act() when NULL).
 * `status_title` is shown in the status bar, `target_label` as the bottom
 * subtext (e.g. the SSID or station MAC). The meter polls `sample_cb` until it
 * is destroyed. Returns NULL on allocation failure.
 */
rssi_meter_t *rssi_meter_create(lv_obj_t *parent, const char *status_title,
                                const char *target_label,
                                rssi_meter_sample_cb sample_cb, void *user);

/* Reserve `reserved_h` px at the bottom (for the shared touch bar) and re-layout. */
void rssi_meter_set_bottom_reserved(rssi_meter_t *m, lv_coord_t reserved_h);

/* Update the bottom subtext (tracked target name). */
void rssi_meter_set_target(rssi_meter_t *m, const char *target_label);

void rssi_meter_destroy(rssi_meter_t *m);

bool rssi_meter_is_active(const rssi_meter_t *m);

#ifdef __cplusplus
}
#endif
