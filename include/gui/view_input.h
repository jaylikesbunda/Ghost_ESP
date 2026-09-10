#pragma once

#include "managers/display_manager.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standardised view input helpers.
 *
 * Back-key handling was copy-pasted ~40x with drift (LV_KEY_ESC vs 27 vs 29
 * vs '`' vs q/Q vs \b vs h vs LEFT). Use these so a future keymap change
 * touches one file.
 *
 * Canonical Back keys: LV_KEY_ESC, 27 (== LV_KEY_ESC, kept for callers that
 * compare raw values), 29, '`'. Extended variants (q/Q/h/LEFT/\b) are
 * view-specific and NOT included; pass them explicitly if needed.
 */

/* True for the canonical Back key values. */
bool view_input_is_back_key(uint32_t key);

/* True when event is a keyboard Back press. */
bool view_input_is_back_event(const InputEvent *event);

/* True when event is INPUT_TYPE_EXIT_BUTTON pressed (IO6 exit). */
bool view_input_is_exit_event(const InputEvent *event);

/* True when Back or Exit was pressed. Most views want this. */
bool view_input_wants_back(const InputEvent *event);

/* Standard touch-swipe threshold: HOR_RES / ratio (matches badble/nfc/etc).
 * ratio<=0 defaults to 10. */
int view_input_swipe_threshold_x(int ratio);

/* Standard vertical list scroll: half a page. up=true scrolls up. */
void view_input_scroll_page(lv_obj_t *scrollable, bool up);

/* Standard list scrollbar setup. Call once after creating a scrollable list:
 * DIR_VER + AUTO (vertical lists) so overflow affordance is consistent.
 * Locked roots/content keep OFF via screen_layout (do not call here). */
void view_input_make_list_scrollable(lv_obj_t *list);

/* Standard log scrollbar setup (terminal / ghostscript runner):
 * DIR_VER + AUTO so overflow is visible. Replaces ad-hoc OFF. */
void view_input_make_log_scrollable(lv_obj_t *log);

#ifdef __cplusplus
}
#endif
