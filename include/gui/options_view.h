#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct options_view_t options_view_t;

// Create an options view list styled like options_screen.c and positioned under the status bar.
// - parent: LVGL parent (lv_scr_act() if NULL)
// - title:  Text shown in the status bar via display_manager_add_status_bar()
options_view_t *options_view_create(lv_obj_t *parent, const char *title);

// Create an opaque options list that does not load or display an asset-pack background.
options_view_t *options_view_create_no_bg(lv_obj_t *parent, const char *title);

// Destroy the options view and its internal objects.
void options_view_destroy(options_view_t *ov);

// Add one item to the list. Returns the created lv_obj_t* button for additional customization.
// The provided callback will be attached for LV_EVENT_CLICKED with user_data.
lv_obj_t *options_view_add_item(options_view_t *ov, const char *label, lv_event_cb_t on_click, void *user_data);

// Add a NULL-terminated array of labels, attaching the same callback/user_data to each item.
void options_view_add_items(options_view_t *ov, const char **labels, lv_event_cb_t on_click, void *user_data);

// Add a standard "< Back" row at the end with the provided callback (optional user_data).
lv_obj_t *options_view_add_back_row(options_view_t *ov, lv_event_cb_t on_click, void *user_data);

// Override the row height for this view (default follows the shared menu policy).
// Call before adding items. Existing rows and their label font are updated too,
// so a row re-heighted by the caller afterwards still wins.
void options_view_set_item_height(options_view_t *ov, int height);

// Selection helpers (wrap-around). Index is 0-based across added items.
void options_view_set_selected(options_view_t *ov, int index);
void options_view_move_selection(options_view_t *ov, int delta);
int  options_view_get_selected(const options_view_t *ov);

// Update an existing item's label text by index.
void options_view_update_item_text(options_view_t *ov, int index, const char *new_text);

// Remove all items.
void options_view_clear(options_view_t *ov);

// Return the number of items currently in the view.
int options_view_get_item_count(const options_view_t *ov);

// Access underlying list container (lv_list).
lv_obj_t *options_view_get_list(options_view_t *ov);

// Update status bar title after creation.
void options_view_set_title(options_view_t *ov, const char *title);

// Re-apply zebra striping and selection highlight using current settings/theme.
void options_view_refresh_styles(options_view_t *ov);

// Recenter a single item's label after changing its height externally.
void options_view_relayout_item(options_view_t *ov, lv_obj_t *item);

// Force re-apply selected style to the currently selected item (useful after modifying its children).
void options_view_refresh_selected_item(options_view_t *ov);

void options_view_trigger_wipe(options_view_t *ov);

#ifdef __cplusplus
}
#endif
