#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "lvgl.h"
#include "managers/joystick_manager.h"
#include <stdbool.h>
#include <stdint.h>

typedef void *QueueHandle_tt;
typedef void *SemaphoreHandle_tt; // Because Circular Includes are fun :)

// static lv_timer_t *rainbow_timer = NULL; // Removed: moved to implementation file
// static uint16_t rainbow_hue = 0; // Removed: moved to implementation file
void display_manager_set_rainbow_mode(bool enable);


typedef enum {
    INPUT_TYPE_TOUCH,
    INPUT_TYPE_JOYSTICK,
    INPUT_TYPE_KEYBOARD,
    INPUT_TYPE_ENCODER,         // --- new
    INPUT_TYPE_EXIT_BUTTON,     // --- new for IO6 exit button
    INPUT_TYPE_HOME_BUTTON      // Board-level Home action
} InputType;

typedef struct {
  InputType type;
  bool is_touch_move;
  bool is_repeat;                 // true for auto-repeated keyboard events
  union {
    struct {
      int joystick_index;
      bool joystick_pressed;
    };
    lv_indev_data_t touch_data; // Used for touchscreen inputs
    uint8_t key_value;          // Used for keyboard inputs
    struct { int8_t direction; bool button; } encoder; // Added for encoder input
    bool exit_pressed;          // Used for IO6 exit button
    bool home_pressed;          // Used for a board-level Home button
  } data;
} InputEvent;

#define INPUT_QUEUE_LENGTH 32
#define INPUT_ITEM_SIZE sizeof(InputEvent)
extern QueueHandle_tt input_queue;

#define MUTEX_TIMEOUT_MS 10

#define HARDWARE_INPUT_TASK_PRIORITY (14)
#define RENDERING_TASK_PRIORITY (15)

typedef struct View {
  lv_obj_t *root;
  void (*create)(void);
  void (*destroy)(void);
  const char *name;
  void (*get_hardwareinput_callback)(void **callback);
  void (*input_callback)(InputEvent *);
} View;

typedef struct {
  View *current_view;
  View *previous_view;
  SemaphoreHandle_tt mutex;
} DisplayManager;

extern View options_menu_view;
extern View terminal_view;
extern View number_pad_view;
extern View keyboard_view;
extern View compass_view;
extern View accelerometer_view;
extern View enviii_view;
extern View wardriving_view;
extern View ethernet_screen_view;
extern View lockscreen_view;

/* Function prototypes */

/**
 * @brief Initialize the Display Manager.
 */
void display_manager_init(void);

#ifdef CONFIG_USE_ENCODER
/* Apply persisted encoder-specific settings to the live decoder. */
void display_manager_apply_encoder_settings(void);
#endif

/**
 * @brief Register a new view.
 */
bool display_manager_register_view(View *view);

/**
 * @brief Switch to a new view.
 */
void display_manager_switch_view(View *view);

/** Render a view without changing route history. Only the GUI router calls this. */
void display_manager_render_view(View *view);

/**
 * @brief Switch to a new view on the LVGL task and wait for the first refresh.
 */
bool display_manager_switch_view_and_wait_for_refresh(View *view);

/**
 * @brief Switch back to whichever view was active before the current one,
 * falling back to the main menu if there is no usable previous view. Views
 * reachable from more than one place (Apps Gallery, a direct Main Menu
 * item, a CLI/serial command, or a user-configured hardware button
 * shortcut that can fire from any screen) should call this from their
 * back/exit handler instead of hardcoding a single destination.
 */
void display_manager_go_back(void);

/**
 * @brief Initialize slower display-adjacent peripherals after the first UI frame.
 */
void display_manager_init_deferred_peripherals(void);

/**
 * @brief Switch to the lockscreen and remember the current view for unlock.
 *
 * Runs registered freeze hooks before swapping the current view out for the
 * lockscreen.
 */
void display_manager_show_lockscreen(void);

/**
 * @brief Register a callback invoked at the start of display_manager_show_lockscreen.
 * The callback runs before the current view is destroyed, so it can capture or
 * tear down transient UI drawn outside the view root. Maximum of 4 callbacks
 * are supported at once.
 * @return token id (>0) to use with display_manager_unregister_freeze_pre_lock,
 *         or -1 if registration failed (table full or fn is NULL).
 */
int display_manager_register_freeze_pre_lock(void (*fn)(void));

/**
 * @brief Unregister a previously registered freeze-pre-lock callback.
 * @param id token returned from display_manager_register_freeze_pre_lock.
 */
void display_manager_unregister_freeze_pre_lock(int id);

/**
 * @brief True while the lockscreen is shown as a floating overlay (wake /
 * auto-lock) on top of a still-live view. Used to keep the underlying view's
 * capture running and to gate input to the lockscreen.
 */
bool display_manager_is_lockscreen_active(void);

/**
 * @brief Clear the lockscreen-overlay state after an overlay unlock. Called by
 * the lockscreen once it has torn its overlay down; does not touch the view
 * underneath, which was never destroyed.
 */
void display_manager_clear_lockscreen_overlay(void);

/**
 * @brief Return view captured when entering the lockscreen, or NULL.
 */
View *display_manager_get_lockscreen_return_view(void);

/**
 * @brief Clear the captured lockscreen return view.
 */
void display_manager_clear_lockscreen_return_view(void);

void apply_power_management_config(bool power_save_enabled);

void display_manager_update_status_bar_color(void);

// void rainbow_effect_cb(lv_timer_t *timer); // Removed: internal static function

/**
 * @brief Destroy the current view.
 */
void display_manager_destroy_current_view(void);

/**
 * @brief Get the current active view.
 */
View *display_manager_get_current_view(void);

bool display_manager_is_available(void);

void lvgl_tick_task(void *arg);

void hardware_input_task(void *pvParameters);

void display_manager_fill_screen(lv_color_t color);

/**
 * @brief Notify the display manager that a user input occurred (external driver/task).
 * If the display was dimmed/off this will restore backlight and return true to indicate
 * the input was consumed for wake purposes and should not be forwarded as a UI event.
 * Returns true if the input woke the display (and should be swallowed), false otherwise.
 */
bool display_manager_notify_user_input(void);

lv_color_t hex_to_lv_color(const char *hex_str);

// Status Bar Functions

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted, int batteryPercentage, bool power_save_enabled, bool is_ap_active, bool is_charging);

void display_manager_add_status_bar(const char *CurrentMenuName);
void display_manager_set_status_bar_hidden(bool hidden);

/* Current status-bar title text ("" if none). Valid until the next
 * add_status_bar call - copy if you need to keep it. */
const char *display_manager_get_status_title(void);
void display_manager_raise_status_bar(void);
void display_manager_restore_status_bar(void);

// Reduce I2C activity (e.g., pause battery polling/logging) while other subsystems
// such as PN532 scanning/bruteforcing are active to avoid bus contention.
void display_manager_set_low_i2c_mode(bool on);
bool display_manager_is_low_i2c_mode(void);

void display_manager_suspend_lvgl_task(void);
void display_manager_resume_lvgl_task(void);
void display_manager_suspend_input_task(void);
void display_manager_resume_input_task(void);

bool display_manager_run_on_lvgl(void (*fn)(void *), void *arg);
/* Non-blocking variant for high-rate producers that can drop stale frames. */
bool display_manager_run_on_lvgl_nowait(void (*fn)(void *), void *arg);
bool display_manager_is_lvgl_task(void);

/* Thread-safe replacement for lv_async_call(). LVGL's timer list and internal
 * heap are not thread-safe, and the render task calls lv_timer_handler() on a
 * dedicated tick task with no locking of its own; calling lv_async_call()
 * directly from any other task races that task and can corrupt a timer node,
 * producing a garbage callback pointer (Guru Meditation in lv_async_timer_cb).
 * Every call site outside display_manager.c must go through this instead of
 * calling lv_async_call() directly. */
lv_res_t display_manager_lvgl_async_call(lv_async_cb_t cb, void *user_data);
lv_res_t display_manager_lvgl_async_call_nowait(lv_async_cb_t cb, void *user_data);

/* Coalesce scroll deltas: queue a scroll_by_bounded into a small accumulator
 * instead of running it on every touch sample. The accumulator is flushed
 * once per LVGL tick (10ms) inside processEvent, which collapses up to
 * ~100 touch samples/sec into a handful of scroll+repaint operations. */
void display_manager_queue_scroll(lv_obj_t *target, int32_t dy);

/* Apply any pending scroll immediately. Safe to call repeatedly. */
void display_manager_flush_pending_scroll(void);

/* Touch drag state machine for live-drag scrolling. Each view maintains
 * a `touch_drag_t` (typically file-static), supplies the scroll target on
 * each call, and reads `was_dragged` from the release return to suppress
 * its own tap handling. The global `touch_drag_scroll` setting gates
 * behavior: ON = per-sample live updates, OFF = single scroll on release
 * using the total drag distance (release-on-release). */
typedef struct {
    bool started;
    bool dragged;
    int drag_axis;
    int start_x, start_y;
    int last_x, last_y;
    lv_obj_t *release_target;
} touch_drag_t;

void touch_drag_reset(touch_drag_t *d);
void touch_drag_begin(touch_drag_t *d, const lv_indev_data_t *data);
/* On touch move. Resolves drag axis, applies live drag (or remembers the
 * target for release). Returns the scroll target that was used, or NULL. */
lv_obj_t *touch_drag_update(touch_drag_t *d, const lv_indev_data_t *data, lv_obj_t *scroll_target);
/* On touch release. Applies release-on-release if appropriate. Returns
 * true if a drag was in progress (caller should suppress its tap/click
 * handling) OR if a release-on-release scroll was applied. */
bool touch_drag_release(touch_drag_t *d, const lv_indev_data_t *data);

LV_IMG_DECLARE(Ghost_ESP);
LV_IMG_DECLARE(Map);
LV_IMG_DECLARE(bluetooth);
LV_IMG_DECLARE(wifi);
LV_IMG_DECLARE(rave);
LV_IMG_DECLARE(ghost);
LV_IMG_DECLARE(GESPAppGallery);
LV_IMG_DECLARE(camera_icon);
LV_IMG_DECLARE(clock_icon);
LV_IMG_DECLARE(settings_icon);
LV_IMG_DECLARE(infrared);
LV_IMG_DECLARE(terminal_icon);
LV_IMG_DECLARE(nfc_icon);
LV_IMG_DECLARE(compass);
LV_IMG_DECLARE(usb);

extern joystick_t joysticks[5];
#ifdef CONFIG_USE_ENCODER
#endif

void set_backlight_brightness(uint8_t value);

#endif /* DISPLAY_MANAGER_H */
