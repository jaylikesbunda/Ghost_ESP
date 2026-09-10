#include "managers/views/terminal_screen.h"
#include "core/serial_manager.h"
#include "core/commandline.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/portmacro.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/keyboard_screen.h"
#include "managers/wifi_manager.h"
#include "managers/display_manager.h"
#include "scans/ble/device_detect_scan.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui/design_tokens.h"

extern View keyboard_view;
extern void keyboard_view_set_return_view(View *view);
extern void keyboard_view_set_start_caps(bool start_caps);


#include "lvgl.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "gui/theme_palette_api.h"

// Function declarations
static void submit_text();
static void add_char_to_buffer(char c);
static void update_input_label();
static void keyboard_input_callback(const char *text);

static const lv_font_t *terminal_font(void) {
    uint8_t size = settings_get_terminal_font_size(&G_Settings);
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    switch (size) {
        case 0: return &lv_font_montserrat_14;
        case 2: return &lv_font_montserrat_20;
        default: return &lv_font_montserrat_16;
    }
#else
    switch (size) {
        case 0: return &lv_font_montserrat_10;
        case 2: return &lv_font_montserrat_14;
        default: return &lv_font_montserrat_12;
    }
#endif
}

static const char *TAG = "Terminal";
static lv_obj_t *terminal_scroller = NULL;
static lv_obj_t *terminal_label = NULL;
static SemaphoreHandle_t terminal_mutex = NULL;
static bool retry_cleanup_flag = false;
static lv_timer_t *terminal_cleanup_retry_timer = NULL;
static bool terminal_active = false;
static bool is_stopping = false;
static bool terminal_initialized = false; // Flag to track if terminal has been fully initialized
static bool terminal_dualcomm_only = false;
#define MAX_TEXT_LENGTH 8192
#define PROCESSING_INTERVAL_MS 10
#define PROCESSING_INTERVAL_FAST_MS 5
#define MIN_SCREEN_SIZE 239
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
#define BUTTON_SIZE 48
#define BUTTON_PADDING 6
#else
#define BUTTON_SIZE 28
#define BUTTON_PADDING 3
#endif

static lv_obj_t *back_btn = NULL;
static lv_obj_t *input_label = NULL;
lv_timer_t *terminal_update_timer = NULL;
static unsigned long createdTimeInMs = 0;
#define ENCODER_DEBOUNCE_TIME_MS 500

static char input_buffer[128] = {0}; // keyboard input buffer
static int input_len = 0; // input length counter

/*
 * Joystick Keyboard Input Controls:
 * 
 * - Button 1: Open keyboard interface
 * - Button 0 (left): Exit terminal app
 * - Button 3 (right): Submit current text
 * - Button 2 (up): Scroll terminal up
 * - Button 4 (down): Scroll terminal down
 */

// Callback function for keyboard input
static void keyboard_input_callback(const char *text) {
    if (text) {
        // Clear current buffer and set new text
        memset(input_buffer, 0, sizeof(input_buffer));
        strncpy(input_buffer, text, sizeof(input_buffer) - 1);
        input_buffer[sizeof(input_buffer) - 1] = '\0';
        input_len = strlen(input_buffer);
        update_input_label();

        /* The keyboard submit callback runs while the keyboard is still the
         * active view.  Restore the terminal synchronously so its canvas and
         * update timer are live before command output starts arriving. */
        display_manager_switch_view_and_wait_for_refresh(&terminal_view);

        // Submit the text to the terminal
        submit_text();
    }
}


static void scroll_terminal_up(void);
static void scroll_terminal_down(void);
static void stop_all_operations(void);
static bool terminal_is_near_bottom(void);
static bool terminal_is_dualcomm_line(const char *text);
static const char *terminal_dualcomm_display_text(const char *text);

// Additional function predefs
static void recalc_layout_if_needed(void);
static void terminal_canvas_draw_event(lv_event_t *e);
static void terminal_canvas_size_event(lv_event_t *e);
static void terminal_push_incoming(const char *data, size_t len);
static void clear_lines(void);
static void recompute_partial_start(void);
static void trim_terminal_history(size_t capacity);

#define MAX_TERMINAL_LINES 400
#define MAX_TERMINAL_TEXT_BYTES (MAX_TEXT_LENGTH * 2)
#define TERMINAL_TEXT_ARENA_BYTES (MAX_TERMINAL_TEXT_BYTES + MAX_TERMINAL_LINES)

// Virtualized terminal canvas and line storage. The text arena is the sole
// owner of retained output; line entries only describe slices within it.
static lv_obj_t *terminal_canvas = NULL;

typedef struct {
  uint16_t offset;
  uint16_t len;
  uint16_t pxh; // cached pixel height at last layout width
  bool is_dualcomm; // precomputed at insert time, read in draw callback
} TermLine;

static char *term_text_arena = NULL;
static size_t term_text_capacity = 0;
static size_t term_text_used = 0;
static size_t term_partial_start = 0;
static TermLine *term_lines = NULL;
static uint16_t term_line_head = 0;   // index of oldest
static uint16_t term_line_count = 0;  // number of valid lines
static SemaphoreHandle_t term_store_mutex = NULL;
static portMUX_TYPE term_store_init_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t term_store_generation = 0;
static uint32_t term_seen_generation = 0;
static lv_coord_t cached_layout_width = -1;
static lv_coord_t cached_total_height = 0;

static void *terminal_alloc_buffer(size_t size) {
  uint32_t caps = MALLOC_CAP_8BIT;
#if CONFIG_SPIRAM
  caps |= MALLOC_CAP_SPIRAM;
#endif
  void *ptr = heap_caps_malloc(size, caps);
#if CONFIG_SPIRAM
  if (!ptr) {
    ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
  }
#endif
  return ptr;
}

static bool terminal_store_lock(void) {
  if (!term_store_mutex) {
    SemaphoreHandle_t new_mutex = xSemaphoreCreateRecursiveMutex();
    portENTER_CRITICAL(&term_store_init_mux);
    if (!term_store_mutex) {
      term_store_mutex = new_mutex;
      new_mutex = NULL;
    }
    portEXIT_CRITICAL(&term_store_init_mux);
    if (new_mutex) vSemaphoreDelete(new_mutex);
  }
  return term_store_mutex &&
         xSemaphoreTakeRecursive(term_store_mutex, portMAX_DELAY) == pdTRUE;
}

static void terminal_store_unlock(void) {
  if (term_store_mutex) xSemaphoreGiveRecursive(term_store_mutex);
}

static bool ensure_terminal_text_arena(size_t capacity) {
  if (term_text_arena && term_text_capacity >= capacity) return true;

  char *new_arena = terminal_alloc_buffer(capacity);
  if (!new_arena) {
    ESP_LOGE(TAG, "Failed to allocate terminal text arena");
    return false;
  }
  if (term_text_arena && term_text_used) {
    memcpy(new_arena, term_text_arena, term_text_used);
  }
  free(term_text_arena);
  term_text_arena = new_arena;
  term_text_capacity = capacity;
  return true;
}

static bool ensure_terminal_lines(void) {
  if (term_lines) return true;
  term_lines = terminal_alloc_buffer(sizeof(TermLine) * MAX_TERMINAL_LINES);
  if (!term_lines) {
    ESP_LOGE(TAG, "Failed to allocate terminal line metadata");
    return false;
  }
  memset(term_lines, 0, sizeof(TermLine) * MAX_TERMINAL_LINES);
  return true;
}

static void rebuild_terminal_lines(void) {
  clear_lines();
  size_t start = 0;
  for (size_t pos = 0; pos < term_text_used; pos++) {
    if (term_text_arena[pos] != '\0') continue;

    if (term_line_count >= MAX_TERMINAL_LINES) {
      term_line_head = (term_line_head + 1) % MAX_TERMINAL_LINES;
      term_line_count--;
    }
    uint16_t idx = (term_line_head + term_line_count) % MAX_TERMINAL_LINES;
    term_lines[idx].offset = (uint16_t)start;
    term_lines[idx].len = (uint16_t)(pos - start);
    term_lines[idx].pxh = 0;
    term_lines[idx].is_dualcomm = terminal_is_dualcomm_line(term_text_arena + start);
    term_line_count++;
    start = pos + 1;
  }
  term_partial_start = start;
}

static void submit_text() {
    if (input_len > 0) {
      char prompt_buf[sizeof(input_buffer) + 4]; // +4 for "> " and null terminator
      snprintf(prompt_buf, sizeof(prompt_buf), "> %s", input_buffer); // format the prompt
      terminal_view_add_text(prompt_buf); // add prompt before the command when printing to screen
      terminal_view_add_text("\n"); // ensure prompt appears immediately as its own line
      simulateCommand(input_buffer); // execute the command
      memset(input_buffer, 0, sizeof(input_buffer)); // clear the input buffer
      input_len = 0; // reset input length
      update_input_label(); // update the input label to show empty state
    }
}

static void add_char_to_buffer(char c) {
  if (input_len < sizeof(input_buffer) - 1) {
    input_buffer[input_len++] = c;
    input_buffer[input_len] = '\0';
    update_input_label();
  }
}

static void remove_char_from_buffer() {
  if (input_len > 0) {
    input_buffer[--input_len] = '\0';
    update_input_label();
  }
}
static void update_input_label() {
    if (input_label) {
        if (input_len > 0) {
            lv_label_set_text(input_label, input_buffer);
        } else {
            lv_label_set_text(input_label, "Type Command...");
        }
    }
}
static void release_terminal_view_storage(void) {
  if (!terminal_store_lock()) return;
  trim_terminal_history(MAX_TEXT_LENGTH);
  clear_lines();
  free(term_lines);
  term_lines = NULL;
  if (term_text_capacity > MAX_TEXT_LENGTH) {
    char *small_arena = terminal_alloc_buffer(MAX_TEXT_LENGTH);
    if (small_arena) {
      free(term_text_arena);
      term_text_arena = small_arena;
      term_text_capacity = MAX_TEXT_LENGTH;
    }
  }
  recompute_partial_start();
  term_store_generation++;
  terminal_store_unlock();
  cached_layout_width = -1;
  cached_total_height = 0;
  if (terminal_canvas && lv_obj_is_valid(terminal_canvas)) {
    lv_obj_invalidate(terminal_canvas);
  }
}

static void clear_pre_init_message_queue(void) {
  // Output is retained directly in the canonical terminal text arena.
}

static void update_terminal_label(const char *text) { (void)text; (void)terminal_label; }

static bool terminal_is_near_bottom(void);

static void scroll_to_bottom_if_needed(bool was_near_bottom) {
  if (!terminal_scroller || !lv_obj_is_valid(terminal_scroller)) return;
  if (was_near_bottom) {
    lv_obj_scroll_to_y(terminal_scroller, LV_COORD_MAX, LV_ANIM_OFF);
  }
}

// ========== Virtualized terminal helpers ==========

static void clear_lines(void) {
  if (term_lines) {
    memset(term_lines, 0, sizeof(TermLine) * MAX_TERMINAL_LINES);
  }
  term_line_head = 0;
  term_line_count = 0;
}

static void drop_oldest_line(void) {
  if (!term_lines || term_line_count == 0) return;
  uint16_t idx = term_line_head;
  size_t remove_end = (size_t)term_lines[idx].offset + term_lines[idx].len + 1;
  if (remove_end > term_text_used) remove_end = term_text_used;

  if (remove_end > 0) {
    size_t remaining = term_text_used - remove_end;
    if (remaining > 0) {
      memmove(term_text_arena, term_text_arena + remove_end, remaining);
    }
    term_text_used = remaining;
    term_partial_start = term_partial_start >= remove_end ?
                         term_partial_start - remove_end : 0;
  }

  term_line_head = (term_line_head + 1) % MAX_TERMINAL_LINES;
  term_line_count--;
  for (uint16_t i = 0; i < term_line_count; i++) {
    uint16_t line_idx = (term_line_head + i) % MAX_TERMINAL_LINES;
    term_lines[line_idx].offset -= remove_end;
  }
}

static void recompute_partial_start(void) {
  term_partial_start = 0;
  for (size_t i = 0; i < term_text_used; i++) {
    if (term_text_arena[i] == '\0') term_partial_start = i + 1;
  }
}

static void trim_terminal_history(size_t capacity) {
  if (!term_text_arena || term_text_used <= capacity) return;

  size_t remove = term_text_used - capacity;
  while (remove < term_text_used && term_text_arena[remove - 1] != '\0') {
    remove++;
  }
  if (remove >= term_text_used) {
    // A single unbroken line is larger than the retained history.
    remove = term_text_used - capacity;
  }

  memmove(term_text_arena, term_text_arena + remove, term_text_used - remove);
  term_text_used -= remove;
  recompute_partial_start();
  if (term_lines) rebuild_terminal_lines();
}

static void append_line(size_t len) {
  size_t offset = term_partial_start;
  if (!term_lines || offset > UINT16_MAX || len > UINT16_MAX) return;
  if (term_line_count >= MAX_TERMINAL_LINES) {
    drop_oldest_line();
    offset = term_partial_start;
  }
  uint16_t idx = (term_line_head + term_line_count) % MAX_TERMINAL_LINES;
  term_lines[idx].offset = (uint16_t)offset;
  term_lines[idx].len = (uint16_t)len;
  term_lines[idx].pxh = 0; // recalc lazily
  term_lines[idx].is_dualcomm = terminal_is_dualcomm_line(term_text_arena + offset);
  term_line_count++;
}

static void ensure_text_space(size_t needed) {
  if (term_text_used + needed > term_text_capacity && term_lines &&
      term_text_capacity < TERMINAL_TEXT_ARENA_BYTES) {
    size_t expanded_capacity = term_text_capacity + 4096;
    if (expanded_capacity < term_text_used + needed) {
      expanded_capacity = term_text_used + needed;
    }
    if (expanded_capacity > TERMINAL_TEXT_ARENA_BYTES) {
      expanded_capacity = TERMINAL_TEXT_ARENA_BYTES;
    }
    (void)ensure_terminal_text_arena(expanded_capacity);
  }

  while (term_text_used + needed > term_text_capacity && term_text_used > 0) {
    if (term_lines && term_line_count > 0) {
      drop_oldest_line();
    } else {
      size_t remove = term_text_used + needed - term_text_capacity;
      while (remove < term_text_used && term_text_arena[remove - 1] != '\0') {
        remove++;
      }
      if (remove >= term_text_used) remove = term_text_used + needed - term_text_capacity;
      memmove(term_text_arena, term_text_arena + remove, term_text_used - remove);
      term_text_used -= remove;
      recompute_partial_start();
    }
  }
  if (term_text_used + needed <= term_text_capacity) return;

  // A single unbroken line exceeded the bounded history. Keep its tail rather
  // than allowing the old partial-line builder to grow without bound.
  size_t drop = term_text_used + needed - term_text_capacity;
  if (drop > term_text_used) drop = term_text_used;
  if (drop > 0) {
    memmove(term_text_arena, term_text_arena + drop, term_text_used - drop);
    term_text_used -= drop;
    term_partial_start = term_partial_start >= drop ? term_partial_start - drop : 0;
  }
}

static void terminal_push_incoming(const char *data, size_t len) {
  if (!data || len == 0) return;
  if (!terminal_store_lock()) return;
  if (!ensure_terminal_text_arena(MAX_TEXT_LENGTH)) {
    terminal_store_unlock();
    return;
  }

  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\n') {
      if (term_text_used > term_partial_start &&
          term_text_arena[term_text_used - 1] == '\r') {
        term_text_arena[term_text_used - 1] = '\0';
      } else {
        ensure_text_space(1);
        term_text_arena[term_text_used++] = '\0';
      }
      if (term_lines) {
        append_line(term_text_used - term_partial_start - 1);
      }
      term_partial_start = term_text_used;
      continue;
    }

    ensure_text_space(1);
    term_text_arena[term_text_used++] = data[i];
  }
  term_store_generation++;
  terminal_store_unlock();
}



static bool terminal_should_use_split_view(void) {
    return terminal_dualcomm_only && settings_get_ghostlink_split_view(&G_Settings);
}

static void recalc_layout_if_needed(void) {
  if (!terminal_canvas || !lv_obj_is_valid(terminal_canvas)) return;
  if (!terminal_store_lock()) return;
  lv_coord_t full_w = lv_obj_get_width(terminal_canvas);
  if (full_w <= 0 || !term_lines || !term_text_arena) {
    terminal_store_unlock();
    return;
  }

  bool split = terminal_should_use_split_view() && (full_w > 0);
  lv_coord_t col_w = split ? (full_w / 2) : full_w;
  if (col_w <= 0) col_w = full_w;

  if (col_w != cached_layout_width) {
    // width changed, invalidate cached heights
    for (uint16_t i = 0; i < term_line_count; i++) {
      uint16_t idx = (term_line_head + i) % MAX_TERMINAL_LINES;
      term_lines[idx].pxh = 0;
    }
    cached_layout_width = col_w;
  }

  const lv_font_t *font = lv_obj_get_style_text_font(terminal_canvas, 0);
  lv_coord_t letter_space = lv_obj_get_style_text_letter_space(terminal_canvas, 0);
  lv_coord_t line_space = lv_obj_get_style_text_line_space(terminal_canvas, 0);

  lv_coord_t total = 0;
  for (uint16_t i = 0; i < term_line_count; i++) {
    uint16_t idx = (term_line_head + i) % MAX_TERMINAL_LINES;
    TermLine *L = &term_lines[idx];
    if (L->pxh == 0) {
      lv_point_t sz;
      const char *txt = L->len ? term_text_arena + L->offset : " ";
      lv_txt_get_size(&sz, txt, font, letter_space, line_space, col_w, LV_TEXT_FLAG_NONE);
      if (sz.y <= 0) sz.y = lv_font_get_line_height(font);
      L->pxh = (uint16_t)sz.y;
    }
    total += L->pxh;
  }
  if (total <= 0) total = 1;
  cached_total_height = total;
  lv_obj_set_height(terminal_canvas, total);
  terminal_store_unlock();
}

static void terminal_canvas_size_event(lv_event_t *e) {
  LV_UNUSED(e);
  cached_layout_width = -1; // force recompute
  recalc_layout_if_needed();
  if (terminal_canvas) lv_obj_invalidate(terminal_canvas);
}

static void terminal_canvas_draw_event(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  if (!obj || obj != terminal_canvas) return;
  if (!terminal_store_lock()) return;
  recalc_layout_if_needed();

  lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
  if (!draw_ctx) {
    terminal_store_unlock();
    return;
  }

  lv_area_t obj_coords;
  lv_obj_get_coords(obj, &obj_coords);
  const lv_area_t *clip = draw_ctx->clip_area;
  if (!clip) {
    terminal_store_unlock();
    return;
  }

  lv_draw_label_dsc_t dsc;
  lv_draw_label_dsc_init(&dsc);
  dsc.font = lv_obj_get_style_text_font(obj, 0);
  dsc.color = lv_obj_get_style_text_color(obj, 0);
  dsc.letter_space = lv_obj_get_style_text_letter_space(obj, 0);
  dsc.line_space = lv_obj_get_style_text_line_space(obj, 0);
  dsc.flag = LV_TEXT_FLAG_NONE;

  lv_coord_t w = lv_obj_get_width(obj);
  bool split = terminal_should_use_split_view() && (w > 60);
  lv_coord_t col_w = split ? (w / 2) : w;
  lv_coord_t local_top = clip->y1 - obj_coords.y1;
  lv_coord_t local_bottom = clip->y2 - obj_coords.y1;
  if (!term_lines || !term_text_arena) {
    terminal_store_unlock();
    return;
  }

  if (!split) {
    // Single-column mode: draw all lines sequentially as before
    lv_coord_t y = 0;
    for (uint16_t i = 0; i < term_line_count; i++) {
      uint16_t idx = (term_line_head + i) % MAX_TERMINAL_LINES;
      TermLine *L = &term_lines[idx];
      lv_coord_t h = L->pxh;
      if ((y + h) < local_top) { y += h; continue; }
      if (y > local_bottom) break;
      const char *txt = L->len ? term_text_arena + L->offset : " ";
      lv_area_t a;
      a.x1 = obj_coords.x1;
      a.y1 = obj_coords.y1 + y;
      a.x2 = a.x1 + col_w - 1;
      a.y2 = a.y1 + h - 1;
      lv_draw_label(draw_ctx, &dsc, &a, txt, NULL);
      y += h;
    }
  } else {
    // Split view: compact each column independently
    // Left column: non-Dual-Comm logs
    lv_coord_t y_left = 0;
    for (uint16_t i = 0; i < term_line_count; i++) {
      uint16_t idx = (term_line_head + i) % MAX_TERMINAL_LINES;
      TermLine *L = &term_lines[idx];
      lv_coord_t h = L->pxh;
      if (L->is_dualcomm) {
        continue; // skip Dual Comm lines in left column
      }
      if ((y_left + h) < local_top) { y_left += h; continue; }
      if (y_left > local_bottom) break;
      const char *txt = L->len ? term_text_arena + L->offset : " ";
      lv_area_t a;
      a.x1 = obj_coords.x1;
      a.y1 = obj_coords.y1 + y_left;
      a.x2 = a.x1 + col_w - 1;
      a.y2 = a.y1 + h - 1;
      lv_draw_label(draw_ctx, &dsc, &a, txt, NULL);
      y_left += h;
    }

    // Right column: Dual-Comm-only view
    lv_coord_t y_right = 0;
    for (uint16_t i = 0; i < term_line_count; i++) {
      uint16_t idx = (term_line_head + i) % MAX_TERMINAL_LINES;
      TermLine *L = &term_lines[idx];
      lv_coord_t h = L->pxh;
      if (!L->is_dualcomm) {
        continue; // skip non-Dual lines in right column
      }
      if ((y_right + h) < local_top) { y_right += h; continue; }
      if (y_right > local_bottom) break;
      const char *txt = L->len ? term_text_arena + L->offset : " ";
      const char *s = terminal_dualcomm_display_text(txt);
      lv_area_t b;
      b.x1 = obj_coords.x1 + col_w;
      b.y1 = obj_coords.y1 + y_right;
      b.x2 = obj_coords.x1 + w - 1;
      b.y2 = b.y1 + h - 1;
      lv_draw_label(draw_ctx, &dsc, &b, s, NULL);
      y_right += h;
    }
  }
  terminal_store_unlock();
}

static void process_queued_messages(void) {
  if (!terminal_active || !terminal_scroller || !terminal_canvas || is_stopping) return;
  if (!lv_obj_is_valid(terminal_scroller) || !lv_obj_is_valid(terminal_canvas)) {
    ESP_LOGW(TAG, "terminal scroller invalid, skipping queued message processing");
    terminal_scroller = NULL;
    terminal_canvas = NULL;
    return;
  }

  if (!terminal_store_lock()) return;
  bool has_new_data = term_seen_generation != term_store_generation;
  if (has_new_data) term_seen_generation = term_store_generation;
  terminal_store_unlock();
  if (!has_new_data) return;

  bool was_near_bottom = terminal_is_near_bottom();
  recalc_layout_if_needed();
  if (terminal_canvas) lv_obj_invalidate(terminal_canvas);
  scroll_to_bottom_if_needed(was_near_bottom);
}

static void process_queued_messages_callback(lv_timer_t * timer) {
    // This now runs within the LVGL task context - no race conditions!
    process_queued_messages();
}


static void scroll_terminal_up(void) {
  if (!terminal_scroller) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_scroller) / 2;
  lv_obj_scroll_by(terminal_scroller, 0, scroll_pixels, LV_ANIM_OFF);
}

static void scroll_terminal_down(void) {
  if (!terminal_scroller) return;
  lv_coord_t scroll_pixels = lv_obj_get_height(terminal_scroller) / 2;
  lv_obj_scroll_by(terminal_scroller, 0, -scroll_pixels, LV_ANIM_OFF);
}

static void stop_all_operations(void) {
    ESP_LOGI(TAG, "Stop all operations triggered");
    terminal_active = false;
    is_stopping = true;
    
    if (terminal_dualcomm_only) {
        simulateCommand("commsend stop");
    }
    terminal_dualcomm_only = false;

    // call stop handler directly instead of queuing it
    // but skip BLE teardown if we're returning from BLE detect tracking
    bool ble_tracking_active = ble_device_detect_is_tracking();
    if (!ble_tracking_active) {
        handle_stop_flipper(0, NULL);
    }

    display_manager_go_back();
}
#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
void text_box_click_cb(lv_event_t *e){
  keyboard_view_set_return_view(&terminal_view);
  keyboard_view_set_submit_callback(keyboard_input_callback);
  keyboard_view_set_placeholder("Enter command...");
  keyboard_view_set_initial_text(input_buffer);
  keyboard_view_set_start_caps(false);
  display_manager_switch_view(&keyboard_view);

  // If using a hardware keyboard, we can ignore this click
}
#endif

static void back_btn_event_cb(lv_event_t *e) {
    stop_all_operations();
}

void terminal_view_create(void) {
    is_stopping = false;
    if (terminal_view.root != NULL) {
        return;
    }

    bool terminal_storage_ready = false;
    if (terminal_store_lock()) {
        if (ensure_terminal_text_arena(MAX_TEXT_LENGTH) && ensure_terminal_lines()) {
            rebuild_terminal_lines();
            term_store_generation++;
            terminal_storage_ready = true;
        }
        terminal_store_unlock();
    }
    if (!terminal_storage_ready) {
        // The view switcher requires a root even under severe heap pressure.
        // Keep the CLI usable and allow logging to resume if memory recovers.
        ESP_LOGE(TAG, "Terminal history unavailable: insufficient heap");
    }

    if (!terminal_mutex) {
        terminal_mutex = xSemaphoreCreateMutex();
        if (!terminal_mutex) {
            ESP_LOGE(TAG, "Failed to create terminal mutex");
            return;
        }
    }

    terminal_active = true;

    terminal_view.root = gui_screen_create_root(NULL, "Terminal", lv_color_black(), LV_OPA_COVER);

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t control_bg = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t control_text = lv_color_hex(theme_palette_get_text(theme));

    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_HEIGHT;
    const int padding = 3;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    const int textbox_height = GUI_CONTROL_H - 4;
#else
    const int textbox_height = 28;
#endif

    int back_button_height = 0;
    bool show_back_btn = false;
    bool show_input_bar = false;

#if defined(CONFIG_USE_TOUCHSCREEN) && GUI_LEGACY_TOUCH_BAR
    if (GUI_LEGACY_TOUCH_BAR &&
        ((LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) ||
         (LV_HOR_RES == 320 && LV_VER_RES == 170))) {
        show_back_btn = true;
        back_button_height = BUTTON_SIZE + BUTTON_PADDING * 2;
    }
#endif

    show_input_bar = false;
#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    show_input_bar = true;
#endif

    // Calculate the height for the input area (input box + padding)
    int input_area_height = 0;
    if (show_back_btn && show_input_bar) {
        input_area_height = (back_button_height > (textbox_height + padding) ? back_button_height : (textbox_height + padding));
    } else if (show_back_btn) {
        input_area_height = back_button_height;
    } else if (show_input_bar) {
        input_area_height = textbox_height + padding;
    } else {
        input_area_height = 0;
    }

    // Calculate the height for the terminal readout area
    int textarea_height = LV_VER_RES - STATUS_BAR_HEIGHT - GUI_HOME_SAFE_H - input_area_height;

    // Create the terminal_page to fill all space above the input box and back button
    terminal_scroller = lv_obj_create(terminal_view.root);
    lv_obj_set_pos(terminal_scroller, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_scroller, LV_HOR_RES, textarea_height);
    lv_obj_set_style_bg_color(terminal_scroller, lv_color_black(), 0);
    lv_obj_set_style_pad_all(terminal_scroller, 0, 0);
    lv_obj_set_style_radius(terminal_scroller, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_scroller, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(terminal_scroller, 0, 0);
    lv_obj_set_style_clip_corner(terminal_scroller, false, 0);
    /* Scrollbar OFF is intentional here (matches main_menu list): the
     * virtualized canvas repaints visible lines only, and a scrollbar strip
     * would force extra redraws on low-refresh displays. Overflow is still
     * reachable via touch-drag + encoder + autoscroll. See view_input.h
     * view_input_make_log_scrollable() for the standard AUTO log setup used
     * by ghostscript_runner. */
    lv_obj_set_scroll_dir(terminal_scroller, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(terminal_scroller, LV_SCROLL_SNAP_NONE);
    lv_obj_clear_flag(terminal_scroller, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // Virtualized canvas child that paints only visible lines
    terminal_canvas = lv_obj_create(terminal_scroller);
    lv_obj_remove_style_all(terminal_canvas);
    lv_obj_set_width(terminal_canvas, LV_PCT(100));
    lv_obj_set_height(terminal_canvas, 0);
    lv_obj_set_style_bg_opa(terminal_canvas, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(terminal_canvas, 0, 0);
    // Match previous label style
    lv_obj_set_style_text_color(terminal_canvas, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
    lv_obj_set_style_text_font(terminal_canvas, terminal_font(), 0);
    lv_obj_add_event_cb(terminal_canvas, terminal_canvas_draw_event, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(terminal_canvas, terminal_canvas_size_event, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_add_event_cb(terminal_scroller, terminal_canvas_size_event, LV_EVENT_SIZE_CHANGED, NULL);
    update_terminal_label("");

#ifdef CONFIG_USE_TOUCHSCREEN
    if (show_back_btn) {
        back_btn = lv_btn_create(terminal_view.root);
        gui_apply_pressed_style(back_btn);
        lv_obj_set_size(back_btn, BUTTON_SIZE, BUTTON_SIZE);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, BUTTON_PADDING, -BUTTON_PADDING);
        lv_obj_set_style_bg_color(back_btn, control_bg, LV_PART_MAIN);
        lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_t *back_label = lv_label_create(back_btn);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(back_label, control_text, 0);
        lv_obj_center(back_label);

        lv_obj_update_layout(terminal_view.root);
        ESP_LOGW(TAG, "Back pos: x=%d, y=%d, w=%d, h=%d",
                 lv_obj_get_x(back_btn), lv_obj_get_y(back_btn),
                 lv_obj_get_width(back_btn), lv_obj_get_height(back_btn));
    }
#endif

#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (show_input_bar) {
        int textbox_width = LV_HOR_RES - 2 * padding;
    #ifdef CONFIG_USE_TOUCHSCREEN
        if (show_back_btn) {
            textbox_width -= BUTTON_SIZE + 2 * BUTTON_PADDING;
        }
    #endif
        if (textbox_width < 40) textbox_width = 40;

        input_label = lv_label_create(terminal_view.root);
        lv_obj_set_size(input_label, textbox_width, textbox_height);
        lv_obj_set_style_bg_color(input_label, control_bg, 0);
        lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(input_label, control_text, 0);
        lv_obj_set_style_pad_all(input_label, padding, 0);
        lv_obj_set_style_radius(input_label, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_align(input_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_border_width(input_label, 0, 0);
        lv_obj_set_style_shadow_width(input_label, 0, 0);
        lv_obj_align(input_label, LV_ALIGN_BOTTOM_RIGHT, -padding,
                     -(GUI_HOME_SAFE_H + padding));
        lv_obj_add_event_cb(input_label, text_box_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(input_label, "Type Command...");
        // Center vertically by adjusting vertical padding
        const lv_font_t *current_font = lv_obj_get_style_text_font(input_label, 0);
        int font_height = lv_font_get_line_height(current_font);
        int vertical_pad = (textbox_height - font_height) / 2;
        if (vertical_pad < 0) vertical_pad = 0; // Prevent negative padding
        lv_obj_set_style_pad_top(input_label, vertical_pad, 0);
        lv_obj_set_style_pad_bottom(input_label, vertical_pad, 0);
    }
#endif

    display_manager_add_status_bar("Terminal");

    if (!terminal_update_timer) {
        terminal_update_timer = lv_timer_create(process_queued_messages_callback, 30, NULL);
        if (!terminal_update_timer) {
            ESP_LOGE(TAG, "Failed to create terminal update timer");
        }
    }

    // core UI objects and timer exist; allow producers to enqueue immediately
    terminal_initialized = true;
    
    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}
static void terminal_retry_cleanup_cb(lv_timer_t *timer) {
    if (!retry_cleanup_flag) {
        lvgl_timer_del_safe(&terminal_cleanup_retry_timer);
        return;
    }
    ESP_LOGI(TAG, "Retrying terminal cleanup...");
    // Try to destroy again
    retry_cleanup_flag = false;
    terminal_view_destroy();
    // If cleanup succeeds, the flag will stay false and timer will be deleted
    // If not, the flag will be set again and timer will keep running
}

void terminal_view_destroy(void) {
    // Signal all callbacks/timers to stop
    terminal_active = false;
    is_stopping = true;
    terminal_initialized = false; // Reset initialization flag

    input_len = 0;
    input_buffer[0] = '\0';

    // Delete timer first to prevent callbacks after objects are freed
    lvgl_timer_del_safe(&terminal_update_timer);
    release_terminal_view_storage();

    // Safely delete LVGL objects
    if (terminal_mutex) {
        if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Delete LVGL objects if they exist
            lvgl_obj_del_safe(&terminal_view.root);
            // Set all pointers to NULL to avoid dangling references
            terminal_scroller = NULL;
            terminal_canvas = NULL;
            back_btn = NULL;
            input_label = NULL;

            vSemaphoreDelete(terminal_mutex); // Delete mutex directly after acquiring
            terminal_mutex = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to acquire terminal mutex during destroy. A leak may occur.");
            retry_cleanup_flag = true; // Set flag to retry cleanup later
            if (!terminal_cleanup_retry_timer) {
                terminal_cleanup_retry_timer = lv_timer_create(terminal_retry_cleanup_cb, 250, NULL);
            }
        }
    } else {
        // If mutex is already NULL, still clear pointers
        terminal_view.root = NULL;
        terminal_scroller = NULL;
        back_btn = NULL;
        input_label = NULL;
    }

    // Final state reset
    is_stopping = false;
    lvgl_timer_del_safe(&terminal_cleanup_retry_timer);
}

static bool terminal_is_dualcomm_line(const char *text) {
  if (!text) return false;
  if (strstr(text, "RX: ") != NULL) return true;
  if (strstr(text, "I: Discovered peer:") != NULL) return true;
  if (strstr(text, "Peer has smaller name") != NULL) return true;
  if (strstr(text, "I: Connecting to peer:") != NULL) return true;
  if (strstr(text, "I: Sent command:") != NULL) return true;
  if (strstr(text, "Handshake completed!") != NULL) return true;
  if (strstr(text, "W: Handshake timeout") != NULL) return true;
  if (strstr(text, "W: Connection lost, restarting discovery") != NULL) return true;
  if (strstr(text, "ESP Comm Response: ") != NULL) return true;
  return false;
}

static const char *terminal_dualcomm_display_text(const char *text) {
  if (!text) return "";
  // trim leading spaces
  while (*text == ' ' || *text == '\t') text++;
  if (strncmp(text, "RX: ", 4) == 0) return text + 4;
  if (strncmp(text, "I: ", 3) == 0) return text + 3;
  if (strncmp(text, "W: ", 3) == 0) return text + 3;
  if (strncmp(text, "ESP Comm Response: ", 20) == 0) return text + 20;
  return text;
}

void terminal_view_add_text(const char *text) {
  if (!text || text[0] == '\0') {
      return;
  }
  terminal_push_incoming(text, strlen(text));
}

bool terminal_view_visit_history(terminal_history_visitor_t visitor, void *ctx) {
  if (!visitor || !terminal_store_lock()) return false;

  bool complete = true;
  size_t start = 0;
  for (size_t i = 0; i < term_text_used; i++) {
    if (term_text_arena[i] != '\0') continue;
    term_text_arena[i] = '\n';
    complete = visitor(term_text_arena + start, i - start + 1, ctx);
    term_text_arena[i] = '\0';
    if (!complete) break;
    start = i + 1;
  }
  if (complete && start < term_text_used) {
    complete = visitor(term_text_arena + start, term_text_used - start, ctx);
  }

  terminal_store_unlock();
  return complete;
}

void terminal_view_clear_history(void) {
  if (!terminal_store_lock()) return;
  term_text_used = 0;
  term_partial_start = 0;
  clear_lines();
  term_store_generation++;
  terminal_store_unlock();

  cached_layout_width = -1;
  cached_total_height = 0;
  if (terminal_canvas && lv_obj_is_valid(terminal_canvas)) {
    lv_obj_set_height(terminal_canvas, 0);
    lv_obj_invalidate(terminal_canvas);
  }
}

size_t terminal_view_log_count(void) {
  return term_line_count;
}

bool terminal_view_log_get(size_t index, char *out, size_t out_len) {
  if (!out || out_len == 0 || !terminal_store_lock()) return false;
  if (!term_lines || !term_text_arena || index >= term_line_count) {
    terminal_store_unlock();
    return false;
  }
  uint16_t idx = (term_line_head + (uint16_t)index) % MAX_TERMINAL_LINES;
  const TermLine *line = &term_lines[idx];
  size_t len = line->len;
  if (line->offset >= term_text_used || len > term_text_used - line->offset) {
    terminal_store_unlock();
    return false;
  }
  if (len >= out_len) len = out_len - 1;
  memcpy(out, term_text_arena + line->offset, len);
  out[len] = '\0';
  terminal_store_unlock();
  return true;
}

static bool terminal_is_near_bottom(void) {
  if (!terminal_scroller) return true;
  const lv_coord_t threshold = 20;
  lv_coord_t content_h = lv_obj_get_content_height(terminal_scroller);
  lv_coord_t view_h = lv_obj_get_height(terminal_scroller);
  lv_coord_t scroll_y = lv_obj_get_scroll_y(terminal_scroller);
  if (content_h <= view_h) return true;
  return (scroll_y + view_h + threshold) >= content_h;
}

void terminal_view_hardwareinput_callback(InputEvent *event) {
  if (event->type == INPUT_TYPE_TOUCH) {
    if (event->data.touch_data.state != LV_INDEV_STATE_PR) {
      return;
    }
    int touch_x = event->data.touch_data.point.x;
    int touch_y = event->data.touch_data.point.y;

    if (input_label){
      // Check if the touch is within the input label area
      int input_x_min = lv_obj_get_x(input_label);
      int input_x_max = input_x_min + lv_obj_get_width(input_label);
      int input_y_min = lv_obj_get_y(input_label);
      int input_y_max = input_y_min + lv_obj_get_height(input_label);

      if (touch_x >= input_x_min && touch_x <= input_x_max &&
          touch_y >= input_y_min && touch_y <= input_y_max) {
        lv_event_send(input_label, LV_EVENT_CLICKED, NULL);
        return;
      }
    }

    // Handle back button touch on larger screens and T-Display S3
    if (GUI_LEGACY_TOUCH_BAR &&
        ((LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) ||
         (LV_HOR_RES == 320 && LV_VER_RES == 170))) {
      int button_y_min = LV_VER_RES - (BUTTON_SIZE + BUTTON_PADDING * 2);
      int button_y_max = LV_VER_RES - BUTTON_PADDING;
      

      if (touch_y >= button_y_min && touch_y <= button_y_max) {
        int back_x_min = BUTTON_PADDING;
        int back_x_max = BUTTON_PADDING + BUTTON_SIZE + 25;
        if (touch_x >= back_x_min && touch_x <= back_x_max) {
          lv_event_send(back_btn, LV_EVENT_CLICKED, NULL);
          return;
        }
      }
      

      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        scroll_terminal_up();
      } else if (touch_y < button_y_min) {
        scroll_terminal_down();
      }
    } else {
      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        scroll_terminal_up();
      } else {
        scroll_terminal_down();
      }
    }
  } else if (event->type == INPUT_TYPE_JOYSTICK) {
    int button = event->data.joystick_index;
    
    if (button == 1 || button == 3) {
      if (input_len > 0) {
        submit_text();
      } else if (input_label) {
        keyboard_view_set_return_view(&terminal_view);
        keyboard_view_set_submit_callback(keyboard_input_callback);
        keyboard_view_set_placeholder("Enter command...");
        keyboard_view_set_start_caps(false);
        display_manager_switch_view(&keyboard_view);
      }
    } else if (button == 2) {
      scroll_terminal_up();
    } else if (button == 4) {
      scroll_terminal_down();
    } else if (button == 0) {
      stop_all_operations();
    }
  } else if (event->type == INPUT_TYPE_KEYBOARD) {
    uint8_t key = event->data.key_value;
    if (key == LV_KEY_ESC || key == 27 || key == 29 || key == '`') {
      stop_all_operations();
    } else if (key == 59 || key == ';') {// up arrow
      scroll_terminal_up();
    } else if (key == 46 || key == '.') {      //down arrow
      scroll_terminal_down();
    } else if (key == 13) {
      if (input_len > 0) {
        submit_text();
      } else if (input_label) {
        keyboard_view_set_return_view(&terminal_view);
        keyboard_view_set_submit_callback(keyboard_input_callback);
        keyboard_view_set_placeholder("Enter command...");
        keyboard_view_set_start_caps(false);
        display_manager_switch_view(&keyboard_view);
      }
    } else if (key == 8 || key == 127) { // backspace
      remove_char_from_buffer();
    } else if (key == 32) { // space
      add_char_to_buffer(' ');
    } else if (key >= 32 && key <= 126) { // printable ASCII characters
      add_char_to_buffer((char)key);
    } else if (key == 0) {
    }
    else {
      // Optionally handle other keys or log them
      char key_str[2];
      key_str[0] = (char)key;
      key_str[1] = '\0';
      terminal_view_add_text(key_str); // Add unhandled keys to terminal
    }
  } else if (event->type == INPUT_TYPE_ENCODER) {
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    if (event->data.encoder.button) {
      if (now_ms - createdTimeInMs <= ENCODER_DEBOUNCE_TIME_MS) {
        ESP_LOGD(TAG, "Encoder button press debounced");
        return;
      }
      createdTimeInMs = now_ms;
      if (input_len > 0) {
        submit_text();
#if defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_TOUCHSCREEN)
      } else if (input_label) {
        keyboard_view_set_return_view(&terminal_view);
        keyboard_view_set_submit_callback(keyboard_input_callback);
        keyboard_view_set_placeholder("Enter command...");
        keyboard_view_set_start_caps(false);
        display_manager_switch_view(&keyboard_view);
#else
      } else {
        stop_all_operations();
#endif
      }
    } else {
      if (event->data.encoder.direction > 0) {
        scroll_terminal_down();
      } else {
        scroll_terminal_up();
      }
    }
  } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
    stop_all_operations();
  }
}



void terminal_view_get_hardwareinput_callback(void **callback) {
  if (callback != NULL) {
    *callback = (void *)terminal_view_hardwareinput_callback;
  }
}



View terminal_view = {
  .root = NULL,
  .create = terminal_view_create,
  .destroy = terminal_view_destroy,
  .input_callback = terminal_view_hardwareinput_callback,
  .name = "TerminalView",
  .get_hardwareinput_callback = terminal_view_get_hardwareinput_callback
};

void terminal_set_return_view(View *view) {
    /* Compatibility no-op. The preceding route is now the return target. */
    (void)view;
}

void terminal_set_dualcomm_filter(bool enable) {
    terminal_dualcomm_only = enable;
}
