#pragma once

#include "lvgl.h"
#include "gui/design_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GUI_STATUS_BAR_HEIGHT
#define GUI_STATUS_BAR_HEIGHT GUI_STATUS_BAR_H
#endif

/*
 * GUI_DEFAULT_BG_COLOR is the fallback background colour used by root views
 * that need a solid (non-asset-pack) base. It is intentionally NOT derived
 * from the active theme palette: those callers explicitly want a flat black
 * background (e.g. trackpad, setup wizard) instead of the surface ramp.
 */
#ifndef GUI_DEFAULT_BG_COLOR
#define GUI_DEFAULT_BG_COLOR 0x121212u
#endif

void gui_screen_apply_background(lv_obj_t *root);
/* Re-apply the active palette's background treatment (flat fill + optional
 * pattern) to a root whose fill came from the theme background. */
void gui_screen_apply_theme_background(lv_obj_t *root);
/* Same, for an explicit theme id (used by previews). Removes any previous
 * pattern widget first. */
void gui_screen_apply_theme_background_for(lv_obj_t *root, uint8_t theme);
lv_obj_t *gui_screen_create_root(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa);

/*
 * Same as gui_screen_create_root but uses GUI_DEFAULT_BG_COLOR as the
 * fill colour at LV_OPA_COVER (with the asset-pack background image
 * drawn on top, matching what the legacy callers did with
 * `lv_color_hex(0x121212)` + LV_OPA_COVER). Use for full-screen input
 * views (trackpad, keyboards) that need a flat black canvas regardless
 * of the menu background image.
 */
lv_obj_t *gui_screen_create_root_default(lv_obj_t *parent, const char *title);

/*
 * Same as gui_screen_create_root but skips the asset-pack background
 * image (solid colour only). Useful for transparent overlays or popups.
 */
lv_obj_t *gui_screen_create_root_no_bg(lv_obj_t *parent, const char *title, lv_color_t bg_color, lv_opa_t bg_opa);

lv_obj_t *gui_screen_create_content(lv_obj_t *root, lv_coord_t status_bar_h);

/* Same as gui_screen_create_content but reserves bottom space for the
 * standard touch bar / footer / home indicator.
 * content_h = LV_VER_RES - status_bar_h - bottom_reserved (clamped >= 0).
 * Prefer this over hand-rolled `LV_VER_RES - GUI_STATUS_BAR_H` math so a
 * touch-bar height change touches one file. */
lv_obj_t *gui_screen_create_content_with_bottom_reserved(lv_obj_t *root,
                                                         lv_coord_t status_bar_h,
                                                         lv_coord_t bottom_reserved);

/* Content height helper for list sizing without creating a container:
 * returns LV_VER_RES - status_bar_h - bottom_reserved, clamped to >= 40
 * (matches the 40px floor used by options-style views). */
lv_coord_t gui_screen_content_height(lv_coord_t status_bar_h, lv_coord_t bottom_reserved);

/* Drop the cached bg-widget state. Call after lv_obj_clean on a root. */
void gui_screen_invalidate_bg_cache(void);

#ifdef __cplusplus
}
#endif
