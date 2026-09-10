 #include "gui/screen_layout.h"
#include "managers/views/infrared_view.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "managers/views/keyboard_screen.h"
#include "managers/settings_manager.h"
#include "gui/accessibility_fonts.h"
#include "managers/views/error_popup.h"
#include "gui/theme_palette_api.h"
#include "gui/options_view.h"
#include "gui/touch_bar.h"
#include "gui/ios_toggle.h"
#include "managers/status_display_manager.h"
#include "managers/ghostchi_manager.h"
#include "gui/design_tokens.h"
#include "gui/gui_router.h"

void update_learning_popup_selection(void);
void update_easy_learn_popup_selection(void);
void update_easy_learn_instruction_text(void);
void easy_learn_toggle_cb(lv_event_t *e);
void easy_learn_cancel_cb(lv_event_t *e);
void easy_learn_skip_cb(lv_event_t *e);
void create_easy_learn_popup(void);
void cleanup_easy_learn_popup(void *obj);
void easy_learn_signal_name_callback(void);
bool is_button_name_used(const char* remote_path, const char* button_name);
const char* get_next_available_button_name(const char* remote_path);
void generate_unique_remote_filename(const char* base_name, char* output_filename, size_t output_size);

// jit sd prototypes
static bool ir_sd_begin(bool *display_was_suspended);
static void ir_sd_end(bool display_was_suspended);

static const char *TAG = "infrared_view";

#ifdef CONFIG_HAS_INFRARED_RX
void cleanup_signal_preview_popup(void *obj);
void signal_preview_save_cb(lv_event_t *e);
void signal_preview_cancel_cb(lv_event_t *e);
void update_signal_preview_selection(void);
void learned_signal_name_callback(const char *name);
void learning_cancel_cb(lv_event_t *e);
static void save_learned_signal(const char *signal_name);
static void create_learning_popup(void);
static void start_ir_learning_task(void);

static void rename_remote_keyboard_callback(const char *name);
static void add_signal_keyboard_callback(const char *name);
#endif

void rename_remote_cb(lv_event_t *e);
void add_signal_cb(lv_event_t *e);
void delete_remote_cb(lv_event_t *e);


#ifndef JOYSTICK_LEFT
#define JOYSTICK_LEFT    (-1)
#endif
#ifndef JOYSTICK_RIGHT
#define JOYSTICK_RIGHT   (1)
#endif
#ifndef JOYSTICK_PRESS
#define JOYSTICK_PRESS   (0)
#endif
#ifndef ENCODER_LEFT
#define ENCODER_LEFT     (-1)
#endif
#ifndef ENCODER_RIGHT
#define ENCODER_RIGHT    (1)
#endif
#ifndef ENCODER_PRESS
#define ENCODER_PRESS    (0)
#endif

#ifdef CONFIG_HAS_INFRARED_RX
static lv_style_t popup_style;
static bool popup_style_initialized = false;
#endif

#include "managers/display_manager.h"
#include "managers/views/main_menu_screen.h"
#include "gui/popup.h"
#include "managers/views/keyboard_screen.h"
#include "gui/lvgl_safe.h"
#include <lvgl/lvgl.h>
#include <dirent.h>
#include <string.h>
#include "managers/infrared_manager.h"
#include "managers/infrared_decoder.h"
#include "core/universal_ir.h"
#include "managers/rgb_manager.h"
#include "sdkconfig.h"
#include "managers/sd_card_manager.h"
#include "esp_log.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#ifdef CONFIG_HAS_INFRARED_RX
#include <time.h>
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#endif

static options_view_t *g_ir_ov = NULL;
static void back_event_cb(lv_event_t *e);
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
static void ir_add_back_row(void);
#endif
static lv_obj_t *root = NULL;
static lv_obj_t *list = NULL;
static popup_confirm_t *ir_confirm_popup = NULL;
static int selected_ir_index = 0;
static int num_ir_items = 0;
static char **ir_file_paths = NULL;
static size_t ir_file_count = 0;
static size_t ir_file_capacity = 0;
static infrared_signal_t *signals = NULL;
static size_t signal_count = 0;
static bool showing_commands = false;
static char current_dir[256] = "/mnt/ghostesp";
static bool has_remotes_option = false;
static bool has_universals_option = false;
static bool in_universals_mode = false;
// Deep-link request (favorite launch): full path of the remote to open as
// soon as the view is created, or immediately if it is already running.
static char *s_pending_remote_open = NULL;
static char **uni_command_names = NULL;
static size_t uni_command_count = 0;
static char current_universal_file[256] = "";
static lv_obj_t *transmitting_popup = NULL;
static lv_timer_t *transmit_popup_delay_timer = NULL;
static TaskHandle_t universal_task_handle = NULL;

// Defer creating the transmitting popup until after the epilepsy warning popup
// finishes displaying and fading out, so its solid background color doesn't
// flash through the semi-transparent warning animation.
#define TRANSMIT_POPUP_DELAY_MS 2300
static lv_obj_t *dazzler_popup = NULL;
static lv_obj_t *dazzler_stop_btn = NULL;
// background sd io worker for infrared
typedef enum { IR_IO_SAVE = 1, IR_IO_APPEND = 2, IR_IO_RENAME = 3, IR_IO_DELETE = 4 } IrIoOp_t;
typedef struct {
    IrIoOp_t op;
    char path[256];
    char aux[256];
    // payload for save/append
    bool is_raw;
    uint32_t frequency;
    float duty_cycle;
    uint32_t *timings;
    size_t timings_size;
    bool has_decoded;
    char protocol[16];
    uint32_t address;
    uint32_t command;
} IrIoJob_t;
static QueueHandle_t ir_sd_queue = NULL;
static TaskHandle_t ir_sd_worker_handle = NULL;
static void ir_sd_worker_task(void *arg);

static bool s_resume_commands = false;

#ifdef CONFIG_HAS_INFRARED_RX

static lv_obj_t *learning_popup = NULL;
static lv_obj_t *learning_cancel_btn = NULL;

static TaskHandle_t ir_learning_task_handle = NULL;
#ifdef CONFIG_SPIRAM
static StaticTask_t *ir_learning_task_tcb = NULL;
static StackType_t *ir_learning_task_stack = NULL;
#endif
static bool ir_learning_cancel = false;
// static rmt_channel_handle_t rx_channel = NULL; // Removed, using infrared_manager
static infrared_signal_t learned_signal = {0};
static char learned_signal_name[64] = {0};
static bool add_signal_mode = false;
static bool preserve_learned_signal = false;

static bool is_easy_mode = false;
static int existing_remote_button_index = 0;

static const char *common_button_names[] = {
    "Power", "Vol_up", "Vol_dn", "Mute", "Ch_up",
    "Ch_dn", "Ok", "Up", "Down", "Left",
    "Right", "Menu", "Back", "Play", "Pause",
    "Stop", "Next", "Prev", "FF", "Rew",
    "Input", "Exit", "Eject", "Subtitle"
};
static const int num_common_buttons = sizeof(common_button_names) / sizeof(common_button_names[0]);

static lv_obj_t *easy_learn_popup = NULL;
static lv_obj_t *easy_learn_cancel_btn = NULL;
static lv_obj_t *easy_learn_skip_btn = NULL;
static lv_obj_t *easy_learn_instruction_label = NULL;
static int easy_learn_selected_option = 0;

static lv_obj_t *signal_preview_popup = NULL;
static lv_obj_t *protocol_label = NULL;
static lv_obj_t *address_label = NULL;
static lv_obj_t *command_label = NULL;
static lv_obj_t *save_btn = NULL;
static lv_obj_t *cancel_btn = NULL;
static int preview_selected_option = 0;
// Removed: signal_decoded, decoded_message, and decoder_context - GPIO-based implementation handles decoding internally
// queue carries rx_event_copy_t by value (no heap allocs in ISR)
// static QueueHandle_t ir_rx_queue = NULL; // Removed, using infrared_manager

/*
#define IR_RX_MAX_SYMBOLS 128
typedef struct {
    size_t num_symbols;
    rmt_symbol_word_t symbols[IR_RX_MAX_SYMBOLS];
} rx_event_copy_t;

static bool ir_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_ctx);
*/
#endif
static void command_event_cb(lv_event_t *e);
static volatile bool universal_transmit_cancel = false;

static char current_remote_path[256] = "";
static char current_remote_name[64] = "";

// override weak hook from infrared_manager to temporarily suspend RX during TX
void infrared_rx_pause_for_tx(bool pause) {
#ifdef CONFIG_HAS_INFRARED_RX
    if (!infrared_manager_rx_is_initialized()) {
        return;
    }
    if (pause) {
        infrared_manager_rx_suspend();
        ESP_LOGI(TAG, "paused RMT RX (suspend) for TX");
    } else {
        infrared_manager_rx_resume();
        ESP_LOGI(TAG, "resumed RMT RX after TX (resume)");
    }
#else
    (void)pause;
#endif
}

static void rename_remote_keyboard_callback(const char *name) {
    s_resume_commands = true;
    if (!name || strlen(name) == 0) {
        display_manager_switch_view(&infrared_view);
        return;
    }
    
    char new_path[256];
    char old_path[256];
    char dir_path[256];

    strncpy(dir_path, current_remote_path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';

    char *last_slash = strrchr(dir_path, '/');
    if (last_slash) {
        *last_slash = '\0';
    }

    size_t dir_len = strlen(dir_path);
    size_t max_name_len = sizeof(new_path) - dir_len - 16;
    
    if (max_name_len <= 0 || max_name_len > 200) {
        max_name_len = 200;
    }
    
    char truncated_name[max_name_len + 1];
    strncpy(truncated_name, name, max_name_len);
    truncated_name[max_name_len] = '\0';
    
    char safe_dir_path[128];
    char filename_part[64];
    
    strncpy(safe_dir_path, dir_path, sizeof(safe_dir_path) - 1);
    safe_dir_path[sizeof(safe_dir_path) - 1] = '\0';
    
    strncpy(filename_part, truncated_name, sizeof(filename_part) - 1);
    filename_part[sizeof(filename_part) - 1] = '\0';
    
    if (strlen(safe_dir_path) + strlen(filename_part) + 4 >= sizeof(new_path)) {
        ESP_LOGE(TAG, "Path would be too long");
        display_manager_switch_view(&infrared_view);
        return;
    }

    snprintf(new_path, sizeof(new_path), "%s/%s.ir", safe_dir_path, filename_part);
    strncpy(old_path, current_remote_path, sizeof(old_path) - 1);
    old_path[sizeof(old_path) - 1] = '\0';
    
    bool susp = false; bool did = ir_sd_begin(&susp);
    int rn = rename(old_path, new_path);
    /* resume display immediately; unmount is deferred by sd_card_manager */
    if (did) ir_sd_end(susp);
    if (rn == 0) {
        ESP_LOGI(TAG, "Renamed remote from %s to %s", old_path, new_path);
        strncpy(current_remote_path, new_path, sizeof(current_remote_path) - 1);
        current_remote_path[sizeof(current_remote_path) - 1] = '\0';
        strncpy(current_remote_name, name, sizeof(current_remote_name) - 1);
        current_remote_name[sizeof(current_remote_name) - 1] = '\0';
    } else {
        ESP_LOGE(TAG, "Failed to rename remote from %s to %s", old_path, new_path);
    }
    
    display_manager_switch_view(&infrared_view);
}

static void add_signal_keyboard_callback(const char *name) {
    s_resume_commands = true;
    if (!name || strlen(name) == 0) {
        display_manager_switch_view(&infrared_view);
        return;
    }
    
    ESP_LOGI(TAG, "Adding new signal with name: %s", name);
    display_manager_switch_view(&infrared_view);
}

#ifdef CONFIG_HAS_INFRARED_RX

static void ir_learning_task(void *arg);
static void add_signal_to_remote_callback(const char *name);
static void append_signal_to_remote(const char *signal_name);
#endif


#ifdef CONFIG_HAS_INFRARED_RX
static void add_signal_to_remote_callback(const char *name)
{
    s_resume_commands = true;
    if (name) {
        strncpy(learned_signal_name, name, sizeof(learned_signal_name) - 1);
        learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
        append_signal_to_remote(learned_signal_name);
    }
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    // Return to infrared view
    display_manager_switch_view(&infrared_view);
}

void learned_signal_name_callback(const char *name)
{
    bool was_adding_to_existing = add_signal_mode && strlen(current_remote_path) > 0;
    if (name) {
        strncpy(learned_signal_name, name, sizeof(learned_signal_name) - 1);
        learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
        
        status_display_show_status("IR Learning...");
        
        if (add_signal_mode) {
            // Adding signal to existing remote
            append_signal_to_remote(learned_signal_name);
            status_display_show_status("Signal Added");
        } else {
            // Learning new remote
            save_learned_signal(learned_signal_name);
            status_display_show_status("Remote Saved");
        }
        
        // Reset the add signal mode flag
        add_signal_mode = false;
        
        // If we were adding to an existing remote, reload the remote's signal list to stay on it
        if (was_adding_to_existing) {
            // Reload the current remote's signals to refresh the view
            if (signals) {
                infrared_manager_free_list(signals, signal_count);
                signals = NULL;
                signal_count = 0;
            }
            if (infrared_manager_read_list(current_remote_path, &signals, &signal_count)) {
                if (g_ir_ov) options_view_clear(g_ir_ov);
                selected_ir_index = 0;
                
                for (size_t i = 0; i < signal_count; i++) {
                    options_view_add_item(g_ir_ov, signals[i].name, command_event_cb, (void *)(intptr_t)i);
                }
                
                options_view_add_item(g_ir_ov, "Rename Remote", rename_remote_cb, NULL);
                options_view_add_item(g_ir_ov, "Add Signal", add_signal_cb, NULL);
                lv_obj_t *delete_btn = options_view_add_item(g_ir_ov, "Delete Remote", delete_remote_cb, NULL);
                if (delete_btn) lv_obj_set_style_bg_color(delete_btn, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
                
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
                ir_add_back_row();
#endif
                num_ir_items = options_view_get_item_count(g_ir_ov);
                options_view_set_selected(g_ir_ov, 0);
                
                ESP_LOGI(TAG, "Reloaded %zu signals for remote after adding new signal (regular mode)", signal_count);
                return;
            }
        }
    }
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    if (was_adding_to_existing) {
        s_resume_commands = true;
    }
    
    // Return to infrared view
    display_manager_switch_view(&infrared_view);
}
#endif

#ifdef CONFIG_HAS_INFRARED_RX
// Function to append a learned signal to an existing remote file
static void append_signal_to_remote(const char *signal_name) {
    ESP_LOGI(TAG, "append_signal_to_remote called with name: %s", signal_name ? signal_name : "NULL");
    ESP_LOGI(TAG, "Current remote path: %s", current_remote_path);
    ESP_LOGI(TAG, "Signal data: timings=%p, size=%d, is_raw=%d", 
             learned_signal.payload.raw.timings, 
             learned_signal.payload.raw.timings_size,
             learned_signal.is_raw);
    
    if (!signal_name || strlen(signal_name) == 0) {
        ESP_LOGE(TAG, "Invalid signal name provided");
        // Free timing data if it exists to prevent memory leaks (only for raw signals!)
        if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }

    if (strlen(current_remote_path) == 0) {
        ESP_LOGE(TAG, "No current remote file selected");
        // Free timing data if it exists to prevent memory leaks (only for raw signals!)
        if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }

    // Check if we have valid signal data (raw signals need timing data)
    if (learned_signal.is_raw) {
        if (!learned_signal.payload.raw.timings || learned_signal.payload.raw.timings_size == 0) {
            ESP_LOGE(TAG, "No timing data in raw signal");
            return;
        }
    }
    
    // do not attempt to validate pointer address range; trust allocation
    
    // Add a small delay to ensure any pending operations are complete
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "Appending signal to remote file: %s", current_remote_path);
    
    // hand off to background worker
    IrIoJob_t job = {0};
    job.op = IR_IO_APPEND;
    strncpy(job.path, current_remote_path, sizeof(job.path)-1);
    strncpy(job.aux, signal_name, sizeof(job.aux)-1);
    if (!learned_signal.is_raw) {
        // Signal was decoded
        job.has_decoded = true;
        strncpy(job.protocol, learned_signal.payload.message.protocol, sizeof(job.protocol)-1);
        job.address = learned_signal.payload.message.address;
        job.command = learned_signal.payload.message.command;
    } else {
        // Signal is raw
        job.is_raw = true;
        job.frequency = learned_signal.payload.raw.frequency;
        job.duty_cycle = learned_signal.payload.raw.duty_cycle;
        job.timings_size = learned_signal.payload.raw.timings_size;
        job.timings = malloc(job.timings_size * sizeof(uint32_t));
        if (job.timings) memcpy(job.timings, learned_signal.payload.raw.timings, job.timings_size * sizeof(uint32_t));
    }
    if (ir_sd_queue) xQueueSend(ir_sd_queue, &job, 0);
    
    ESP_LOGI(TAG, "Queued append of signal '%s' to remote file %s", signal_name, current_remote_path);

    // Free timing data to prevent memory leaks (only for raw signals!)
    if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
    }
}
#endif

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
static const char *IR_BACK_OPTION_MAGIC_STR = "__IR_BACK_OPTION__";
static void ir_add_back_row(void) {
    lv_obj_t *btn = options_view_add_back_row(g_ir_ov, back_event_cb, NULL);
    if (btn) lv_obj_set_user_data(btn, (void *)IR_BACK_OPTION_MAGIC_STR);
}
#endif

typedef struct {
    char path[256];
    char command[32];
} UniversalTransmitArgs_t;
static void ir_select_item(int index);

// touchscreen controls
#ifdef CONFIG_USE_TOUCHSCREEN
static gui_touch_bar_t s_ir_touch_tb = {0};
static lv_obj_t *ir_scroll_up_btn = NULL;
static lv_obj_t *ir_scroll_down_btn = NULL;
static lv_obj_t *ir_back_btn = NULL;
#endif

static touch_drag_t ir_touch_drag = {0};
#define IR_SWIPE_THRESHOLD_RATIO 10

#ifdef CONFIG_USE_TOUCHSCREEN
// scroll callbacks
static void file_scroll_up_cb(lv_event_t *e) { ir_select_item(selected_ir_index - 1); }
static void file_scroll_down_cb(lv_event_t *e) { ir_select_item(selected_ir_index + 1); }
#endif

// add job struct and queue/task for universals
typedef struct {
    char path[256];
    char command[32];
} UniversalJob_t;
static QueueHandle_t universals_queue = NULL;
static TaskHandle_t universals_task_handle = NULL;

// forward declarations
static void file_event_open(int idx);
static void command_event_execute(int idx);
static void ir_open_universal_path(const char *full_path);
static void ir_open_remote_path(const char *full_path, const char *fname);
static void infrared_view_apply_pending_open(void);
static void file_event_cb(lv_event_t *e);
static void command_event_cb(lv_event_t *e);
static void placeholder_event_cb(lv_event_t *e);
static void remotes_event_cb(lv_event_t *e);
static void universals_event_cb(lv_event_t *e);
#ifdef CONFIG_HAS_INFRARED_RX
static void learn_remote_event_cb(lv_event_t *e);
#endif

static void cleanup_transmit_popup(void *obj);
#ifdef CONFIG_HAS_INFRARED_RX
static void cleanup_learning_popup(void *obj);
#else
static void cleanup_learning_popup(void *obj) { (void)obj; }
#endif
static void universal_transmit_task(void *arg);
static void dazzler_event_cb(lv_event_t *e);
static void dazzler_stop_cb(lv_event_t *e);
static void cleanup_dazzler_popup(void *obj);

static void clear_ir_file_paths(void) {
    if (ir_file_paths) {
        for (size_t i = 0; i < ir_file_count; i++) {
            free(ir_file_paths[i]);
        }
        free(ir_file_paths);
    }
    ir_file_paths = NULL;
    ir_file_count = 0;
    ir_file_capacity = 0;
}

static bool load_ir_file_list_from_dir(const char *dir) {
    clear_ir_file_paths();

    bool susp = false;
    bool did = ir_sd_begin(&susp);

    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "Failed to open IR directory: %s", dir);
        if (did) {
            ir_sd_end(susp);
        }
        
        // Still add TURNHISTVOFF for universals directory even if SD is not available
        if (strcmp(dir, "/mnt/ghostesp/infrared/universals") == 0) {
            char *turnthistvoff_name = strdup("TURNHISTVOFF.ir");
            if (turnthistvoff_name) {
                if (ir_file_count >= ir_file_capacity) {
                    size_t new_cap = ir_file_capacity ? ir_file_capacity * 2 : 8;
                    char **new_paths = realloc(ir_file_paths, new_cap * sizeof(*ir_file_paths));
                    if (new_paths) {
                        ir_file_paths = new_paths;
                        ir_file_capacity = new_cap;
                    } else {
                        free(turnthistvoff_name);
                        turnthistvoff_name = NULL;
                    }
                }
                if (turnthistvoff_name) {
                    ir_file_paths[ir_file_count] = turnthistvoff_name;
                    ir_file_count++;
                }
            }
        }
        
        // Treat missing or inaccessible directories as empty so callers can show placeholders
        return true;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *name = entry->d_name;
        if (!name) {
            continue;
        }
        if ((name[0] == '.') && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }

        const char *ext = strrchr(name, '.');
        if (!ext) {
            continue;
        }
        if (!((ext[0] == '.') &&
              (ext[1] == 'i' || ext[1] == 'I') &&
              (ext[2] == 'r' || ext[2] == 'R') &&
              ext[3] == '\0')) {
            continue;
        }

        char *name_copy = strdup(name);
        if (!name_copy) {
            ESP_LOGE(TAG, "Failed to allocate IR filename copy");
            continue;
        }

        if (ir_file_count >= ir_file_capacity) {
            size_t new_cap = ir_file_capacity ? ir_file_capacity * 2 : 8;
            char **new_paths = realloc(ir_file_paths, new_cap * sizeof(*ir_file_paths));
            if (!new_paths) {
                ESP_LOGE(TAG, "Failed to grow IR file list");
                free(name_copy);
                continue;
            }
            ir_file_paths = new_paths;
            ir_file_capacity = new_cap;
        }

        ir_file_paths[ir_file_count] = name_copy;
        ir_file_count++;
    }

    closedir(d);
    if (did) {
        ir_sd_end(susp);
    }
    
    // Add TURNTHISTVOFF to universals directory
    if (strcmp(dir, "/mnt/ghostesp/infrared/universals") == 0) {
        char *turnthistvoff_name = strdup("TURNHISTVOFF.ir");
        if (turnthistvoff_name) {
            if (ir_file_count >= ir_file_capacity) {
                size_t new_cap = ir_file_capacity ? ir_file_capacity * 2 : 8;
                char **new_paths = realloc(ir_file_paths, new_cap * sizeof(*ir_file_paths));
                if (new_paths) {
                    ir_file_paths = new_paths;
                    ir_file_capacity = new_cap;
                } else {
                    free(turnthistvoff_name);
                    turnthistvoff_name = NULL;
                }
            }
            if (turnthistvoff_name) {
                ir_file_paths[ir_file_count] = turnthistvoff_name;
                ir_file_count++;
            }
        }
    }
    
    return true;
}

static void rebuild_ir_file_list_ui(void) {
    if (!g_ir_ov) return;

    ESP_LOGI(TAG, "mem[ir_ui_pre]: free=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    options_view_clear(g_ir_ov);
    selected_ir_index = 0;

    for (size_t i = 0; i < ir_file_count; i++) {
        options_view_add_item(g_ir_ov, ir_file_paths[i], file_event_cb, (void *)(intptr_t)i);
    }

    if (ir_file_count == 0) {
        options_view_add_item(g_ir_ov, "No .ir files", placeholder_event_cb, NULL);
    }

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
    ir_add_back_row();
#endif
    num_ir_items = options_view_get_item_count(g_ir_ov);
    options_view_set_selected(g_ir_ov, 0);
    ESP_LOGI(TAG, "mem[ir_ui_post]: free=%u largest=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void refresh_ir_file_list(const char *dir) {
    if (!list) {
        return;
    }

    bool loaded = load_ir_file_list_from_dir(dir);
    if (!loaded) {
        if (g_ir_ov) options_view_clear(g_ir_ov);
        num_ir_items = 0;
        selected_ir_index = 0;
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
        if (g_ir_ov) ir_add_back_row();
        num_ir_items = g_ir_ov ? options_view_get_item_count(g_ir_ov) : 0;
#endif
        return;
    }

    rebuild_ir_file_list_ui();
}

// jit sd helpers for somethingsomething template
static bool ir_sd_begin(bool *display_was_suspended)
{
    return sd_card_jit_begin(display_was_suspended, true);
}

static void ir_sd_end(bool display_was_suspended)
{
    sd_card_jit_end(display_was_suspended);
}

static void create_transmit_popup_now(void) {
    if (transmitting_popup && lv_obj_is_valid(transmitting_popup)) return;
    if (!in_universals_mode) return;
    if (universal_task_handle == NULL) return;

    transmitting_popup = popup_create_container(lv_scr_act(), 200, 60, false);
    lv_obj_clear_flag(transmitting_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(transmitting_popup);
    lv_label_set_text(label, "Transmitting...");
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(label);
}

static void create_transmit_popup_cb(lv_timer_t *timer) {
    (void)timer;
    transmit_popup_delay_timer = NULL;
    create_transmit_popup_now();
}

static void cleanup_transmit_popup(void *obj) {
    (void)obj;
    if (transmit_popup_delay_timer) {
        lv_timer_del(transmit_popup_delay_timer);
        transmit_popup_delay_timer = NULL;
    }
    lvgl_obj_del_safe(&transmitting_popup);
}

static void cleanup_dazzler_popup(void *obj) {
    (void)obj;
    lvgl_obj_del_safe(&dazzler_popup);
    dazzler_stop_btn = NULL;
}

static void dazzler_stop_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Dazzler stop button pressed");
    infrared_manager_dazzler_stop();
    cleanup_dazzler_popup(NULL);
}

static void dazzler_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "IR Dazzler button pressed");
    
    if (infrared_manager_dazzler_is_active()) {
        infrared_manager_dazzler_stop();
        return;
    }
    
    if (!infrared_manager_dazzler_start()) {
        ESP_LOGE(TAG, "Failed to start dazzler");
        return;
    }
    
    lv_coord_t scr_w = LV_HOR_RES;
    lv_coord_t scr_h = LV_VER_RES;
    lv_coord_t popup_w = scr_w - 40;
    if (popup_w > 280) popup_w = 280;
    if (popup_w < 160) popup_w = scr_w - 20;
    lv_coord_t popup_h = 120;
    if (scr_h < 200) popup_h = scr_h - 40;
    if (popup_h < 80) popup_h = 80;
    
    dazzler_popup = popup_create_container(lv_scr_act(), popup_w, popup_h, true);
    
    lv_obj_t *title = popup_create_title_label(dazzler_popup, "IR Dazzler Active", accessibility_get_font_body(), 15);
    (void)title;
    
    lv_obj_t *info = popup_create_body_label(dazzler_popup, "Emitting IR...", popup_w - 20, true, accessibility_get_font_small(), 45);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_TOP_MID, 0, 45);
    
    lv_coord_t btn_w = 100;
    if (btn_w > popup_w - 40) btn_w = popup_w - 40;
    dazzler_stop_btn = popup_add_styled_button(dazzler_popup, "Stop", btn_w, 30, 
                                                LV_ALIGN_BOTTOM_MID, 0, -10, NULL, 
                                                dazzler_stop_cb, NULL);
}

static void ir_sd_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        IrIoJob_t job;
        if (xQueueReceive(ir_sd_queue, &job, portMAX_DELAY) != pdTRUE) continue;

        bool susp = false;
        bool did = ir_sd_begin(&susp);
        if (!did) {
            ESP_LOGE(TAG, "IR SD worker: unable to mount SD card (op=%d, path=%s)", job.op, job.path);
            if ((job.op == IR_IO_SAVE || job.op == IR_IO_APPEND) && job.timings) {
                free(job.timings);
            }
            continue;
        }

        esp_err_t dir_err = sd_card_setup_directory_structure();
        if (dir_err != ESP_OK) {
            ESP_LOGW(TAG, "IR SD worker: directory setup failed: %s", esp_err_to_name(dir_err));
        }
        switch (job.op) {
        case IR_IO_SAVE: {
            char filename[256];
            // create unique filename under remotes using provided base name in aux
            generate_unique_remote_filename(job.aux, filename, sizeof(filename));
            FILE *f = fopen(filename, "w");
            if (f) {
                fprintf(f, "Filetype: IR signals file\n");
                fprintf(f, "Version: 1\n");
                fprintf(f, "#\n");
                fprintf(f, "# Generated by Ghost ESP\n");
                fprintf(f, "# Signal: %s\n", job.aux);
                fprintf(f, "#\n");
                fprintf(f, "name: %s\n", job.aux);
                if (job.has_decoded) {
                    fprintf(f, "type: parsed\n");
                    fprintf(f, "protocol: %s\n", job.protocol);
                    fprintf(f, "address: %02lX %02lX %02lX %02lX\n",
                            (unsigned long)(job.address >> 0) & 0xFF,
                            (unsigned long)(job.address >> 8) & 0xFF,
                            (unsigned long)(job.address >> 16) & 0xFF,
                            (unsigned long)(job.address >> 24) & 0xFF);
                    fprintf(f, "command: %02lX %02lX %02lX %02lX\n",
                            (unsigned long)(job.command >> 0) & 0xFF,
                            (unsigned long)(job.command >> 8) & 0xFF,
                            (unsigned long)(job.command >> 16) & 0xFF,
                            (unsigned long)(job.command >> 24) & 0xFF);
                } else {
                    fprintf(f, "type: raw\n");
                    fprintf(f, "frequency: %lu\n", (unsigned long)job.frequency);
                    fprintf(f, "duty_cycle: %.6f\n", job.duty_cycle);
                    fprintf(f, "data: ");
                    for (size_t i = 0; i < job.timings_size; i++) {
                        fprintf(f, "%lu ", (unsigned long)job.timings[i]);
                    }
                    fprintf(f, "\n");
                }
                fclose(f);
            } else {
                ESP_LOGE(TAG, "Failed to open IR file for writing: %s (errno=%d)", filename, errno);
            }
            break;
        }
        case IR_IO_APPEND: {
            FILE *f = fopen(job.path, "a");
            if (f) {
                fprintf(f, "#\n");
                fprintf(f, "name: %s\n", job.aux);
                if (job.has_decoded) {
                    fprintf(f, "type: parsed\n");
                    fprintf(f, "protocol: %s\n", job.protocol);
                    fprintf(f, "address: %02lX %02lX %02lX %02lX\n",
                            (unsigned long)(job.address >> 0) & 0xFF,
                            (unsigned long)(job.address >> 8) & 0xFF,
                            (unsigned long)(job.address >> 16) & 0xFF,
                            (unsigned long)(job.address >> 24) & 0xFF);
                    fprintf(f, "command: %02lX %02lX %02lX %02lX\n",
                            (unsigned long)(job.command >> 0) & 0xFF,
                            (unsigned long)(job.command >> 8) & 0xFF,
                            (unsigned long)(job.command >> 16) & 0xFF,
                            (unsigned long)(job.command >> 24) & 0xFF);
                } else {
                    fprintf(f, "type: raw\n");
                    fprintf(f, "frequency: %lu\n", (unsigned long)job.frequency);
                    fprintf(f, "duty_cycle: %.6f\n", job.duty_cycle);
                    fprintf(f, "data: ");
                    for (size_t i = 0; i < job.timings_size; i++) {
                        fprintf(f, "%lu ", (unsigned long)job.timings[i]);
                    }
                    fprintf(f, "\n");
                }
                fclose(f);
            }
            break;
        }
        case IR_IO_RENAME: {
            (void)rename(job.path, job.aux);
            break;
        }
        case IR_IO_DELETE: {
            (void)remove(job.path);
            break;
        }
        default: break;
        }
        if (did) ir_sd_end(susp);
        // free any transferred buffers
        if ((job.op == IR_IO_SAVE || job.op == IR_IO_APPEND) && job.timings) {
            free(job.timings);
        }
    }
}

#ifdef CONFIG_SPIRAM

#define UNIVERSAL_BATCH_SIZE 16

typedef struct {
    infrared_signal_t *signals;
    size_t count;
    bool eof_reached;
    bool target_found;
} universal_batch_t;

static void free_batch_signals(universal_batch_t *batch) {
    if (!batch || !batch->signals) return;
    for (size_t i = 0; i < batch->count; i++) {
        infrared_manager_free_signal(&batch->signals[i]);
    }
    free(batch->signals);
    batch->signals = NULL;
    batch->count = 0;
}

static bool read_universal_batch(FILE *f, const char *command, universal_batch_t *batch) {
    if (!f || !command || !batch) return false;
    
    batch->signals = heap_caps_malloc(UNIVERSAL_BATCH_SIZE * sizeof(infrared_signal_t), 
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!batch->signals) return false;
    
    batch->count = 0;
    batch->eof_reached = false;
    batch->target_found = false;
    
    char buf[256];
    infrared_signal_t sig;
    bool in_block = false, block_valid = false;
    
    while (batch->count < UNIVERSAL_BATCH_SIZE && fgets(buf, sizeof(buf), f)) {
        char *s = buf; while (*s && isspace((unsigned char)*s)) s++;
        if (*s=='#'||*s=='\0') continue;
        
        if (strncmp(s, "name:", 5)==0) {
            if (in_block && block_valid) {
                memcpy(&batch->signals[batch->count], &sig, sizeof(sig));
                batch->count++;
                if (batch->count >= UNIVERSAL_BATCH_SIZE) {
                    batch->target_found = true;
                    return true;
                }
            }
            char *v = s+5; while (*v && isspace((unsigned char)*v)) v++;
            char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
            if (strcmp(v, command)==0) {
                in_block=true; block_valid=false; memset(&sig,0,sizeof(sig));
                strncpy(sig.name, v, sizeof(sig.name)-1);
                batch->target_found = true;
            } else {
                in_block=false;
            }
        } else if (in_block) {
            if (strncmp(s, "type:",5)==0) {
                char *v=s+5; while(*v&&isspace((unsigned char)*v))v++;
                char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
                sig.is_raw = (strncmp(v,"raw",3)==0);
                block_valid = true;
            } else if (sig.is_raw) {
                if (strncmp(s, "frequency:",10)==0) sig.payload.raw.frequency = strtoul(s+10,NULL,10);
                else if (strncmp(s, "duty_cycle:",11)==0) sig.payload.raw.duty_cycle = strtof(s+11,NULL);
                else if (strncmp(s, "data:",5)==0) {
                    char *p=s+5; size_t cnt=0; char *t=p;
                    while(*t){while(*t&&isspace((unsigned char)*t))t++;if(!*t)break;cnt++;while(*t&&!isspace((unsigned char)*t))t++;}
                    uint32_t *arr=heap_caps_malloc(cnt*sizeof(uint32_t), MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM);
                    if (!arr) { sig.payload.raw.timings=NULL; sig.payload.raw.timings_size=0; }
                    else { size_t ii=0; char *endp;
                    while(*p){while(*p&&isspace((unsigned char)*p))p++;if(!*p)break;arr[ii++]=strtoul(p,&endp,10);p=endp;}
                    sig.payload.raw.timings=arr; sig.payload.raw.timings_size=cnt; }
                }
            } else {
                if (strncmp(s, "protocol:",9)==0) {
                    char *v=s+9; 
                    while(*v && isspace((unsigned char)*v)) v++;
                    char *e = v + strlen(v) - 1;
                    while(e > v && isspace((unsigned char)*e)) *e-- = '\0';
                    strncpy(sig.payload.message.protocol, v, sizeof(sig.payload.message.protocol)-1);
                    sig.payload.message.protocol[sizeof(sig.payload.message.protocol)-1] = '\0';
                    block_valid=true;
                } else if (strncmp(s, "address:",8)==0) {
                    char* p = s + 8;
                    uint32_t addr = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        addr |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.address = addr;
                } else if (strncmp(s, "command:",8)==0) {
                    char* p = s + 8;
                    uint32_t cmd = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        cmd |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.command = cmd;
                }
            }
        }
    }
    
    if (in_block && block_valid) {
        memcpy(&batch->signals[batch->count], &sig, sizeof(sig));
        batch->count++;
    }
    
    batch->eof_reached = feof(f) != 0;
    return batch->count > 0 || batch->eof_reached;
}

static void universal_transmit_task(void *arg) {
    UniversalTransmitArgs_t *args = (UniversalTransmitArgs_t *)arg;
    char path[256];
    char command[32];
    strncpy(path, args->path, sizeof(path) -1);
    path[sizeof(path) - 1] = '\0';
    strncpy(command, args->command, sizeof(command) -1);
    command[sizeof(command) - 1] = '\0';
    free(args);

    printf("universal_transmit_task: start %s -> %s\n", path, command);

    ghostchi_manager_add_xp(1);

    if (strcmp(path, "TURNHISTVOFF") == 0) {
        printf("Using universal IR system for TURNHISTVOFF transmission\n");
        size_t signal_count = universal_ir_get_signal_count();
        universal_transmit_cancel = false;

        for (size_t i = 0; i < signal_count; i++) {
            if (universal_transmit_cancel) break;
            infrared_signal_t signal;
            if (universal_ir_get_signal(i, &signal)) {
                printf("Transmitting TURNHISTVOFF signal %zu: %s\n", i, signal.name);
                status_display_show_status("Universal IR TX");
                infrared_manager_transmit(&signal);
                infrared_manager_free_signal(&signal);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
        display_manager_lvgl_async_call(cleanup_transmit_popup, NULL);
        universal_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    universal_transmit_cancel = false;
    long file_pos = 0;
    bool done = false;
    
    while (!done && !universal_transmit_cancel) {
        bool susp = false;
        bool did = ir_sd_begin(&susp);
        
        FILE *f = fopen(path, "r");
        if (!f) {
            printf("universal_transmit_task: fopen failed for %s\n", path);
            if (did) ir_sd_end(susp);
            break;
        }
        
        if (file_pos > 0) {
            fseek(f, file_pos, SEEK_SET);
        }
        
        universal_batch_t batch = {0};
        bool read_ok = read_universal_batch(f, command, &batch);
        file_pos = ftell(f);
        
        fclose(f);
        if (did) ir_sd_end(susp);
        
        if (!read_ok || batch.count == 0) {
            done = true;
            break;
        }
        
        for (size_t i = 0; i < batch.count && !universal_transmit_cancel; i++) {
            infrared_manager_transmit(&batch.signals[i]);
            infrared_manager_free_signal(&batch.signals[i]);
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        
        free(batch.signals);
        batch.signals = NULL;
        
        if (batch.eof_reached) {
            done = true;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    printf("universal_transmit_task: finished processing %s\n", path);
    display_manager_lvgl_async_call(cleanup_transmit_popup, NULL);
    universal_task_handle = NULL;
    vTaskDelete(NULL);
}

#else

static void universal_transmit_task(void *arg) {
    UniversalTransmitArgs_t *args = (UniversalTransmitArgs_t *)arg;
    char path[256];
    char command[32];
    strncpy(path, args->path, sizeof(path) -1);
    path[sizeof(path) - 1] = '\0';
    strncpy(command, args->command, sizeof(command) -1);
    command[sizeof(command) - 1] = '\0';
    free(args);

    printf("universal_transmit_task: start %s -> %s\n", path, command);

    ghostchi_manager_add_xp(1);

    if (strcmp(path, "TURNHISTVOFF") == 0) {
        printf("Using universal IR system for TURNHISTVOFF transmission\n");
        size_t signal_count = universal_ir_get_signal_count();
        universal_transmit_cancel = false;
        
        for (size_t i = 0; i < signal_count; i++) {
            if (universal_transmit_cancel) break;
            infrared_signal_t signal;
            if (universal_ir_get_signal(i, &signal)) {
                printf("Transmitting TURNHISTVOFF signal %zu: %s\n", i, signal.name);
                status_display_show_status("Universal IR TX");
                infrared_manager_transmit(&signal);
                infrared_manager_free_signal(&signal);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }
        display_manager_lvgl_async_call(cleanup_transmit_popup, NULL);
        universal_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    bool susp = false; bool did = ir_sd_begin(&susp);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("universal_transmit_task: fopen failed for %s\n", path);
        if (did) ir_sd_end(susp);
        display_manager_lvgl_async_call(cleanup_transmit_popup, NULL);
        universal_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    char buf[256];
    infrared_signal_t sig;
    bool in_block = false, block_valid = false;
    universal_transmit_cancel = false;

    while (fgets(buf, sizeof(buf), f)) {
        if (universal_transmit_cancel) break;
        char *s = buf; while (*s && isspace((unsigned char)*s)) s++;
        if (*s=='#'||*s=='\0') continue;
        if (strncmp(s, "name:", 5)==0) {
            if (in_block&&block_valid) {
                infrared_manager_transmit(&sig);
                infrared_manager_free_signal(&sig);
                vTaskDelay(pdMS_TO_TICKS(150));
            }
            char *v = s+5; while (*v && isspace((unsigned char)*v)) v++;
            char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
            if (strcmp(v, command)==0) {
                in_block=true; block_valid=false; memset(&sig,0,sizeof(sig));
                strncpy(sig.name, v, sizeof(sig.name)-1);
            } else {
                in_block=false;
            }
        } else if (in_block) {
            if (strncmp(s, "type:",5)==0) {
                char *v=s+5; while(*v&&isspace((unsigned char)*v))v++;
                char *e=v+strlen(v)-1; while (e>v&&isspace((unsigned char)*e))*e--='\0';
                sig.is_raw = (strncmp(v,"raw",3)==0);
                block_valid = true;
            } else if (sig.is_raw) {
                if (strncmp(s, "frequency:",10)==0) sig.payload.raw.frequency = strtoul(s+10,NULL,10);
                else if (strncmp(s, "duty_cycle:",11)==0) sig.payload.raw.duty_cycle = strtof(s+11,NULL);
                else if (strncmp(s, "data:",5)==0) {
                    char *p=s+5; size_t cnt=0; char *t=p;
                    while(*t){while(*t&&isspace((unsigned char)*t))t++;if(!*t)break;cnt++;while(*t&&!isspace((unsigned char)*t))t++;}
                    uint32_t *arr=malloc(cnt*sizeof(uint32_t));
                    if (!arr) { sig.payload.raw.timings=NULL; sig.payload.raw.timings_size=0; }
                    else { size_t ii=0; char *endp;
                    while(*p){while(*p&&isspace((unsigned char)*p))p++;if(!*p)break;arr[ii++]=strtoul(p,&endp,10);p=endp;}
                    sig.payload.raw.timings=arr; sig.payload.raw.timings_size=cnt; }
                }
            } else {
                if (strncmp(s, "protocol:",9)==0) {
                    char *v=s+9; 
                    while(*v && isspace((unsigned char)*v)) v++;
                    char *e = v + strlen(v) - 1;
                    while(e > v && isspace((unsigned char)*e)) *e-- = '\0';
                    strncpy(sig.payload.message.protocol, v, sizeof(sig.payload.message.protocol)-1);
                    sig.payload.message.protocol[sizeof(sig.payload.message.protocol)-1] = '\0';
                    block_valid=true;
                } else if (strncmp(s, "address:",8)==0) {
                    char* p = s + 8;
                    uint32_t addr = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        addr |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.address = addr;
                } else if (strncmp(s, "command:",8)==0) {
                    char* p = s + 8;
                    uint32_t cmd = 0;
                    uint8_t shift = 0;
                    while (*p && shift < 32) {
                        while (*p && isspace((unsigned char)*p)) p++;
                        if (!*p) break;
                        char* endp;
                        unsigned long val = strtoul(p, &endp, 16);
                        if (p == endp) break;
                        cmd |= (uint32_t)(val & 0xFF) << shift;
                        shift += 8;
                        p = endp;
                    }
                    sig.payload.message.command = cmd;
                }
            }
        }
    }
    if (!universal_transmit_cancel && in_block&&block_valid) {
        infrared_manager_transmit(&sig);
        infrared_manager_free_signal(&sig);
    }
    fclose(f);
    if (did) ir_sd_end(susp);
    printf("universal_transmit_task: finished processing %s\n", path);
    display_manager_lvgl_async_call(cleanup_transmit_popup, NULL);
    universal_task_handle = NULL;
    vTaskDelete(NULL);
}

#endif

static void back_event_cb(lv_event_t *e) {
    if (showing_commands) {
        // if currently showing commands, return to file list
        showing_commands = false;

        // free command-related resources
        if (!in_universals_mode && signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if (in_universals_mode && uni_command_names) {
            for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
            free(uni_command_names);
            uni_command_names = NULL;
            uni_command_count = 0;
        }

        // rebuild file list
        refresh_ir_file_list(current_dir);
        return;
    }

    // if we are in a file list (remotes or universals) but not at top-level, go back to top-level menu
    if (!has_remotes_option || !has_universals_option) {
        // cancel any ongoing universal transmission
        if (universal_task_handle) {
            universal_transmit_cancel = true;
        }

        // free resources from file list level
        if (signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        clear_ir_file_paths();
        if (uni_command_names) {
            for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
            free(uni_command_names);
            uni_command_names = NULL;
            uni_command_count = 0;
        }

        in_universals_mode = false;
        has_remotes_option = true;
        has_universals_option = true;
        strcpy(current_dir, "/mnt/ghostesp");

        if (g_ir_ov) options_view_clear(g_ir_ov);
        options_view_add_item(g_ir_ov, "Remotes", remotes_event_cb, NULL);
        options_view_add_item(g_ir_ov, "Universals", universals_event_cb, NULL);
#ifdef CONFIG_HAS_INFRARED_RX
        is_easy_mode = settings_get_infrared_easy_mode(&G_Settings);
        options_view_add_item(g_ir_ov, "Learn Remote", learn_remote_event_cb, NULL);
        {
            lv_obj_t *easy_learn_btn = options_view_add_item(g_ir_ov, "Easy Learn", easy_learn_toggle_cb, NULL);
            if (easy_learn_btn) {
                lv_obj_set_flex_flow(easy_learn_btn, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(easy_learn_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_t *easy_learn_label = lv_obj_get_child(easy_learn_btn, 0);
                if (easy_learn_label) {
                    lv_obj_set_flex_grow(easy_learn_label, 1);
                    lv_obj_set_width(easy_learn_label, LV_SIZE_CONTENT);
                }
                lv_obj_t *easy_learn_toggle = ios_toggle_create(easy_learn_btn);
                lv_obj_update_layout(easy_learn_btn);
                ios_toggle_set_value(easy_learn_toggle, is_easy_mode, false);
            }
        }
#endif
        options_view_add_item(g_ir_ov, "IR Dazzler", dazzler_event_cb, NULL);
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
        ir_add_back_row();
#endif
        num_ir_items = options_view_get_item_count(g_ir_ov);
        selected_ir_index = 0;
        if (num_ir_items > 0) options_view_set_selected(g_ir_ov, 0);
        return;
    }

    // default: leave view
    display_manager_go_back();
}

void infrared_view_create(void) {
#ifdef CONFIG_HAS_INFRARED_RX
    is_easy_mode = settings_get_infrared_easy_mode(&G_Settings);
#endif
    
    root = gui_screen_create_root_default(NULL, "Infrared");
    lv_obj_set_style_pad_all(root, 0, 0);
    infrared_view.root = root;
    if (!ir_sd_queue) {
        ir_sd_queue = xQueueCreate(4, sizeof(IrIoJob_t));
    }
    if (ir_sd_queue && !ir_sd_worker_handle) {
        xTaskCreate(ir_sd_worker_task, "ir_io", 4096, NULL, tskIDLE_PRIORITY + 1, &ir_sd_worker_handle);
    }

    g_ir_ov = options_view_create(root, "Infrared");
    list = options_view_get_list(g_ir_ov);

#ifdef CONFIG_USE_TOUCHSCREEN
    const int STATUS_BAR_HEIGHT = GUI_STATUS_BAR_HEIGHT;
    int list_h = LV_VER_RES - STATUS_BAR_HEIGHT - gui_touch_bar_height();
    lv_obj_set_size(list, GUI_OPTIONS_LIST_WIDTH, list_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, STATUS_BAR_HEIGHT);
#endif

    has_remotes_option = true;
    has_universals_option = true;
    options_view_add_item(g_ir_ov, "Remotes", remotes_event_cb, NULL);
    options_view_add_item(g_ir_ov, "Universals", universals_event_cb, NULL);

#ifdef CONFIG_HAS_INFRARED_RX
    ESP_LOGI(TAG, "Initializing RMT RX channel for infrared learning");
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << infrared_get_rx_pin()),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);
    if (infrared_manager_rx_init()) {
        ESP_LOGI(TAG, "RMT RX channel initialized successfully via manager");
    } else {
        ESP_LOGE(TAG, "Failed to initialize RMT RX channel via manager");
    }
    options_view_add_item(g_ir_ov, "Learn Remote", learn_remote_event_cb, NULL);
    {
        lv_obj_t *easy_learn_btn = options_view_add_item(g_ir_ov, "Easy Learn", easy_learn_toggle_cb, NULL);
        if (easy_learn_btn) {
            lv_obj_set_flex_flow(easy_learn_btn, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(easy_learn_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_t *easy_learn_label = lv_obj_get_child(easy_learn_btn, 0);
            if (easy_learn_label) {
                lv_obj_set_flex_grow(easy_learn_label, 1);
                lv_obj_set_width(easy_learn_label, LV_SIZE_CONTENT);
            }
            lv_obj_t *easy_learn_toggle = ios_toggle_create(easy_learn_btn);
            lv_obj_update_layout(easy_learn_btn);
            ios_toggle_set_value(easy_learn_toggle, is_easy_mode, false);
        }
    }
#endif

    options_view_add_item(g_ir_ov, "IR Dazzler", dazzler_event_cb, NULL);

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
    ir_add_back_row();
#endif
    num_ir_items = options_view_get_item_count(g_ir_ov);
    /* Root-menu highlight only resets on a genuine fresh entry from the
     * Main Menu; returning here restores the previously highlighted row. */
    if (gui_router_previous_view() == &main_menu_view) {
        selected_ir_index = 0;
    } else if (selected_ir_index < 0 || selected_ir_index >= num_ir_items) {
        selected_ir_index = 0;
    }
    if (num_ir_items > 0) options_view_set_selected(g_ir_ov, selected_ir_index);

    /* Returning from the keyboard or learn flows after an in-remote edit:
     * rebuild the command list of the remote being edited instead of
     * showing the root menu. */
    if (s_resume_commands) {
        s_resume_commands = false;
        if (current_remote_path[0] != '\0' && !in_universals_mode) {
            if (signals) {
                infrared_manager_free_list(signals, signal_count);
                signals = NULL;
                signal_count = 0;
            }
            if (infrared_manager_read_list(current_remote_path, &signals, &signal_count)) {
                options_view_clear(g_ir_ov);
                selected_ir_index = 0;

                for (size_t i = 0; i < signal_count; i++) {
                    options_view_add_item(g_ir_ov, signals[i].name, command_event_cb, (void *)(intptr_t)i);
                }

                options_view_add_item(g_ir_ov, "Rename Remote", rename_remote_cb, NULL);
                options_view_add_item(g_ir_ov, "Add New Signal", add_signal_cb, NULL);
                lv_obj_t *delete_btn = options_view_add_item(g_ir_ov, "Delete Remote", delete_remote_cb, NULL);
                if (delete_btn) lv_obj_set_style_bg_color(delete_btn, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);

                num_ir_items = options_view_get_item_count(g_ir_ov);

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
                ir_add_back_row();
                num_ir_items = options_view_get_item_count(g_ir_ov);
#endif
                options_view_set_selected(g_ir_ov, 0);
            }
        }
    }

#ifdef CONFIG_USE_TOUCHSCREEN
    s_ir_touch_tb = gui_touch_bar_create(root);
    ir_scroll_up_btn = s_ir_touch_tb.up_btn;
    ir_back_btn = s_ir_touch_tb.back_btn;
    ir_scroll_down_btn = s_ir_touch_tb.down_btn;
    if (s_ir_touch_tb.bar != NULL) {
        gui_touch_bar_set_callbacks(&s_ir_touch_tb,
                                    file_scroll_up_cb, NULL,
                                    back_event_cb, NULL,
                                    file_scroll_down_cb, NULL);
    }

    if (list && lv_obj_is_valid(list)) {
        gui_touch_bar_update_visibility(&s_ir_touch_tb, list);
    }
#endif

    // Consume any pending favorite deep-link (infrared_view_open_remote).
    infrared_view_apply_pending_open();
}

void infrared_view_destroy(void) {
    popup_confirm_close(&ir_confirm_popup);

    if (universal_task_handle) {
        universal_transmit_cancel = true;
        vTaskDelete(universal_task_handle);
        universal_task_handle = NULL;
    }
    cleanup_transmit_popup(NULL);
    
    if (infrared_manager_dazzler_is_active()) {
        infrared_manager_dazzler_stop();
    }
    cleanup_dazzler_popup(NULL);
    
#ifdef CONFIG_HAS_INFRARED_RX
    // Cleanup IR learning resources
    if (ir_learning_task_handle) {
        ir_learning_cancel = true;
        // Wait briefly for task to exit
        // vTaskDelay(pdMS_TO_TICKS(100)); // Optional: give it a moment
        // If task handle is still valid (it might have self-deleted), we can't force delete freely if static
        // But for static tasks, we must free memory. The task should ideally have exited.
        // Assuming ir_learning_cancel triggers exit loop -> vTaskDelete(NULL)
        // We will free memory in next cycle or ensure it's freed if handle becomes NULL
    }
    // Free static task memory if allocated
#ifdef CONFIG_SPIRAM
    if (ir_learning_task_stack) {
        free(ir_learning_task_stack);
        ir_learning_task_stack = NULL;
    }
    if (ir_learning_task_tcb) {
        free(ir_learning_task_tcb);
        ir_learning_task_tcb = NULL;
    }
#endif
    ir_learning_task_handle = NULL;

    cleanup_learning_popup(NULL);
    cleanup_signal_preview_popup(NULL);

    // Only free learned signal data if it's not being preserved for the callback
    // If preserve_learned_signal is true, it means we're switching to keyboard view
    // and the timing data should be preserved for the callback
    if (!preserve_learned_signal && learned_signal.is_raw && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
    }
    
    // Clean up RMT RX resources via manager
    infrared_manager_rx_deinit();
#endif
    if(root) {
        if(signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        clear_ir_file_paths();
        showing_commands = false;
        if (g_ir_ov) { options_view_destroy(g_ir_ov); g_ir_ov = NULL; }
        lvgl_obj_del_safe(&root);
        list = NULL;
        infrared_view.root = NULL;
        /* selected_ir_index deliberately not reset here -- it needs to
         * survive the destroy() -> create() cycle so create() can restore
         * it when returning rather than always resetting to the top. */
        num_ir_items = 0;
    }
#ifdef CONFIG_USE_TOUCHSCREEN
    gui_touch_bar_destroy(&s_ir_touch_tb);
    ir_scroll_up_btn = NULL;
    ir_scroll_down_btn = NULL;
    ir_back_btn = NULL;
#endif
}

static void ir_select_item(int index) {
    if (num_ir_items == 0 || !g_ir_ov) return;
    if (index < 0) index = num_ir_items - 1;
    if (index >= num_ir_items) index = 0;

    selected_ir_index = index;
    options_view_set_selected(g_ir_ov, index);

    if (showing_commands && !in_universals_mode) {
        int del_idx = (int)signal_count + 2;
        lv_obj_t *del = lv_obj_get_child(list, del_idx);
        if (del) {
            if (index == del_idx) {
                lv_obj_set_style_bg_color(del, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN);
                lv_obj_t *lbl = lv_obj_get_child(del, 0);
                if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            } else {
                lv_obj_set_style_bg_color(del, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN);
                lv_obj_t *lbl = lv_obj_get_child(del, 0);
                if (lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
            }
        }
    }
}

static void ir_delete_remote_confirm_cb(void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "Deleting remote: %s", current_remote_path);

    bool susp = false; bool did = ir_sd_begin(&susp);
    int rm = remove(current_remote_path);
    if (did) ir_sd_end(susp);
    if (rm == 0) {
        ESP_LOGI(TAG, "Successfully deleted remote: %s", current_remote_path);
        display_manager_switch_view(&infrared_view);
    } else {
        ESP_LOGE(TAG, "Failed to delete remote: %s", current_remote_path);
    }
}

static bool ir_confirm_handle_input(InputEvent *event) {
    if (!popup_confirm_is_open(ir_confirm_popup)) return false;
    if (!event) return true;

    if (event->type == INPUT_TYPE_TOUCH) return popup_confirm_handle_touch(&ir_confirm_popup, &event->data.touch_data);
    if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        popup_confirm_cancel(&ir_confirm_popup);
        return true;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        if (button == 1) popup_confirm_select(&ir_confirm_popup);
        else if (button == 0) popup_confirm_set_selected(ir_confirm_popup, 0);
        else if (button == 3) popup_confirm_set_selected(ir_confirm_popup, 1);
        else if (button == 2 || button == 4) popup_confirm_move(ir_confirm_popup, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t key = event->data.key_value;
        if (key == 13 || key == 10 || key == LV_KEY_ENTER) popup_confirm_select(&ir_confirm_popup);
        else if (key == 27 || key == 29 || key == LV_KEY_ESC || key == '`') popup_confirm_cancel(&ir_confirm_popup);
        else if (key == 'h' || key == 'l' || key == 'k' || key == 'j' || key == ',' || key == '.' || key == ';' || key == '/' ||
                 key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN) popup_confirm_move(ir_confirm_popup, 1);
        return true;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) popup_confirm_select(&ir_confirm_popup);
        else if (event->data.encoder.direction != 0) popup_confirm_move(ir_confirm_popup, event->data.encoder.direction);
        return true;
    }

    return true;
}

void infrared_view_input_cb(InputEvent *event) {
    if (ir_confirm_handle_input(event)) {
        return;
    }

    // allow enter/esc to cancel universals transmit popup immediately
    if (transmitting_popup && lv_obj_is_valid(transmitting_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            if (event->data.touch_data.state == LV_INDEV_STATE_PR) {
                universal_transmit_cancel = true;
                cleanup_transmit_popup(NULL);
                return;
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            uint8_t key = event->data.key_value;
            if (key == 13 || key == 10 || key == 27 || key == 'c' || key == 'C') {
                universal_transmit_cancel = true;
                cleanup_transmit_popup(NULL);
                return;
            }
        }
    }
    
    // Handle dazzler popup input - any button press stops the dazzler
    if (dazzler_popup && lv_obj_is_valid(dazzler_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                if (dazzler_stop_btn && lv_obj_is_valid(dazzler_stop_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(dazzler_stop_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        dazzler_stop_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER ||
                   event->type == INPUT_TYPE_KEYBOARD) {
            dazzler_stop_cb(NULL);
            return;
        }
        return;
    }
    
#ifdef CONFIG_HAS_INFRARED_RX
    // Handle learn remote popup input
    if (learning_popup && lv_obj_is_valid(learning_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on cancel button
                if (learning_cancel_btn && lv_obj_is_valid(learning_cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(learning_cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        learning_cancel_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            if ((event->type == INPUT_TYPE_JOYSTICK && (event->data.joystick_index == 0 || event->data.joystick_index == 1)) ||
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                learning_cancel_cb(NULL);
                return;
            } else if (
                (event->type == INPUT_TYPE_JOYSTICK && (event->data.joystick_index == 3)) ||
                (event->type == INPUT_TYPE_ENCODER && (event->data.encoder.direction == ENCODER_LEFT || event->data.encoder.direction == ENCODER_RIGHT)) ||
                (event->type == INPUT_TYPE_KEYBOARD && event->data.key_value == 9))
            {
                preview_selected_option = 1;
                update_learning_popup_selection();
            } else if (event->type == INPUT_TYPE_KEYBOARD) {
                // Handle Cardputer keyboard input for cancel
                if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                    event->data.key_value == 27 || // ESC key
                    event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                    learning_cancel_cb(NULL);
                    return;
                }
            }
        }
        return; // Don't process other input when learning popup is active
    }
    
    // Handle Easy Learn popup input
    if (easy_learn_popup && lv_obj_is_valid(easy_learn_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on cancel button
                if (easy_learn_cancel_btn && lv_obj_is_valid(easy_learn_cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(easy_learn_cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        easy_learn_cancel_cb(NULL);
                        return;
                    }
                }
                // Handle touch on skip button
                if (easy_learn_skip_btn && lv_obj_is_valid(easy_learn_skip_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(easy_learn_skip_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        easy_learn_skip_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            // Handle joystick/encoder navigation
            if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 0) || 
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_LEFT)) {
                easy_learn_selected_option = 0; // Cancel
                update_easy_learn_popup_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 3) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_RIGHT)) {
                easy_learn_selected_option = 1; // Skip
                update_easy_learn_popup_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 1) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                if (easy_learn_selected_option == 0) {
                    easy_learn_cancel_cb(NULL);
                } else {
                    easy_learn_skip_cb(NULL);
                }
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            // Handle Cardputer keyboard input
            if (event->data.key_value == 's' || event->data.key_value == 'S') {
                easy_learn_skip_cb(NULL);
            } else if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                      event->data.key_value == 27) { // ESC key
                easy_learn_cancel_cb(NULL);
            } else if (event->data.key_value == 9) { // Tab key
                easy_learn_selected_option = (easy_learn_selected_option + 1) % 2;
                update_easy_learn_popup_selection();
            } else if (event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                if (easy_learn_selected_option == 0) {
                    easy_learn_cancel_cb(NULL);
                } else {
                    easy_learn_skip_cb(NULL);
                }
            }
        }
        return; // Don't process other input when easy learn popup is active
    }
    
    // Handle signal preview popup input
    if (signal_preview_popup && lv_obj_is_valid(signal_preview_popup)) {
        if (event->type == INPUT_TYPE_TOUCH) {
            lv_indev_data_t *data = &event->data.touch_data;
            if (data->state == LV_INDEV_STATE_PR) {
                // Handle touch on save/cancel buttons
                if (save_btn && lv_obj_is_valid(save_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(save_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        signal_preview_save_cb(NULL);
                        return;
                    }
                }
                if (cancel_btn && lv_obj_is_valid(cancel_btn)) {
                    lv_area_t area;
                    lv_obj_get_coords(cancel_btn, &area);
                    if (data->point.x >= area.x1 && data->point.x <= area.x2 &&
                        data->point.y >= area.y1 && data->point.y <= area.y2) {
                        signal_preview_cancel_cb(NULL);
                        return;
                    }
                }
            }
        } else if (event->type == INPUT_TYPE_JOYSTICK || event->type == INPUT_TYPE_ENCODER) {
            // Handle joystick/encoder navigation
            if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 0) || 
                (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_LEFT)) {
                preview_selected_option = 0; // Save
                update_signal_preview_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 3) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_RIGHT)) {
                preview_selected_option = 1; // Cancel
                update_signal_preview_selection();
            } else if ((event->type == INPUT_TYPE_JOYSTICK && event->data.joystick_index == 1) ||
                      (event->type == INPUT_TYPE_ENCODER && event->data.encoder.direction == ENCODER_PRESS)) {
                if (preview_selected_option == 0) {
                    signal_preview_save_cb(NULL);
                } else {
                    signal_preview_cancel_cb(NULL);
                }
            }
        } else if (event->type == INPUT_TYPE_KEYBOARD) {
            // Handle Cardputer keyboard input
            if (event->data.key_value == 's' || event->data.key_value == 'S') {
                signal_preview_save_cb(NULL);
            } else if (event->data.key_value == 'c' || event->data.key_value == 'C' ||
                      event->data.key_value == 27) { // ESC key
                signal_preview_cancel_cb(NULL);
            } else if (event->data.key_value == 9) { // Tab key
                preview_selected_option = (preview_selected_option + 1) % 2;
                update_signal_preview_selection();
            } else if (event->data.key_value == 13 || event->data.key_value == 10) { // Enter
                if (preview_selected_option == 0) {
                    signal_preview_save_cb(NULL);
                } else {
                    signal_preview_cancel_cb(NULL);
                }
            }
        }
        return; // Don't process other input when preview is active
    }
#endif
    
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *data = &event->data.touch_data;
        
        if (data->state == LV_INDEV_STATE_PR) {
#ifdef CONFIG_USE_TOUCHSCREEN
            if (gui_touch_bar_hit(ir_scroll_up_btn, data->point.x, data->point.y)) {
                ir_select_item(selected_ir_index - 1);
                touch_drag_reset(&ir_touch_drag);
                return;
            }

            if (gui_touch_bar_hit(ir_scroll_down_btn, data->point.x, data->point.y)) {
                ir_select_item(selected_ir_index + 1);
                touch_drag_reset(&ir_touch_drag);
                return;
            }

            if (gui_touch_bar_hit(ir_back_btn, data->point.x, data->point.y)) {
                back_event_cb(NULL);
                touch_drag_reset(&ir_touch_drag);
                return;
            }
#endif

            if (!ir_touch_drag.started) {
                touch_drag_begin(&ir_touch_drag, data);
            } else {
                // Move event - apply live drag or remember target for release
                lv_area_t list_area;
                lv_obj_get_coords(list, &list_area);
                bool started_in_list = (ir_touch_drag.start_x >= list_area.x1 && ir_touch_drag.start_x <= list_area.x2 &&
                                         ir_touch_drag.start_y >= list_area.y1 && ir_touch_drag.start_y <= list_area.y2);
                if (started_in_list) {
                    touch_drag_update(&ir_touch_drag, data, list);
                }
            }
            return;
        }

        if (data->state == LV_INDEV_STATE_REL) {
            if (!ir_touch_drag.started) return;

            int start_x = ir_touch_drag.start_x;
            int start_y = ir_touch_drag.start_y;
            int dx = (int)data->point.x - start_x;
            int dy = (int)data->point.y - start_y;

            // Let the shared touch_drag helper handle release-on-release
            // (it applies a single scroll when the live setting is off) and
            // tell us if a drag was in progress so we can skip tap handling.
            bool was_dragged = touch_drag_release(&ir_touch_drag, data);
            if (was_dragged) return;

            int thr_y = LV_VER_RES / IR_SWIPE_THRESHOLD_RATIO;
            int thr_x = LV_HOR_RES / IR_SWIPE_THRESHOLD_RATIO;

            lv_area_t list_area;
            lv_obj_get_coords(list, &list_area);
            bool started_in_list = (start_x >= list_area.x1 && start_x <= list_area.x2 &&
                                     start_y >= list_area.y1 && start_y <= list_area.y2);

            if (started_in_list) {
                if (abs(dx) > thr_x) return;

                // thirds-control special behavior
                if (settings_get_thirds_control_enabled(&G_Settings)) {
                    int list_h = (int)(list_area.y2 - list_area.y1);
                    if (list_h > 0) {
                        int y_rel = (int)data->point.y - (int)list_area.y1;
                        if (y_rel < list_h / 3) {
                            ir_select_item(selected_ir_index - 1);
                            return;
                        } else if (y_rel > (list_h * 2) / 3) {
                            ir_select_item(selected_ir_index + 1);
                            return;
                        }
                    }
                }
            } else {
                // if it didn't start in the list, we still allow tap check for scroll buttons or other edge cases
                // but we skip swipe/thirds logic
                if (abs(dy) > thr_y || abs(dx) > thr_x) return;
            }

            // treat as tap inside the list
            uint32_t child_cnt = lv_obj_get_child_cnt(list);
            for (uint32_t i = 0; i < child_cnt; i++) {
                lv_obj_t *btn = lv_obj_get_child(list, i);
                if (btn) {
                    lv_area_t btn_area;
                    lv_obj_get_coords(btn, &btn_area);
                    if (data->point.x >= btn_area.x1 && data->point.x <= btn_area.x2 &&
                        data->point.y >= btn_area.y1 && data->point.y <= btn_area.y2) {
                        
                        ir_select_item((int)i);

                        // Magic "Back" button for encoder mode
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
                        if (lv_obj_get_user_data(btn) == IR_BACK_OPTION_MAGIC_STR) {
                            back_event_cb(NULL);
                            return;
                        }
#endif

                        bool top_level = (has_remotes_option || has_universals_option);
                        if (!showing_commands) {
                            if (top_level) {
                                if (has_remotes_option && i == 0) {
                                    remotes_event_cb(NULL);
                                } else if (has_universals_option && i == (has_remotes_option ? 1 : 0)) {
                                    universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                                } else if (i == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                                    learn_remote_event_cb(NULL);
                                } else if (i == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                                    easy_learn_toggle_cb(NULL);
                                } else if (i == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 2)) {
                                    dazzler_event_cb(NULL);
#else
                                } else if (i == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                                    dazzler_event_cb(NULL);
#endif
                                } else {
                                    int top_count = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0);
#ifdef CONFIG_HAS_INFRARED_RX
                                    top_count += 2;
#endif
                                    top_count += 1;  // Dazzler
                                    int file_idx = (int)i - top_count;
                                    file_event_open(file_idx);
                                }
                            } else {
                                // inside a file list: direct open
                                file_event_open((int)i);
                            }
                        } else {
                            if (!in_universals_mode) {
                                if ((size_t)i == signal_count) {
                                    lv_event_t e = {0};
                                    rename_remote_cb(&e);
                                } else if ((size_t)i == signal_count + 1) {
                                    lv_event_t e = {0};
                                    add_signal_cb(&e);
                                } else if ((size_t)i == signal_count + 2) {
                                    lv_event_t e = {0};
                                    delete_remote_cb(&e);
                                } else {
                                    command_event_execute((int)i);
                                }
                            } else {
                                command_event_execute((int)i);
                            }
                        }
                        return;
                    }
                }
            }
            return;
        }
    } else if(event->type == INPUT_TYPE_JOYSTICK) {
        uint8_t idx = event->data.joystick_index;
        if(idx == 0) {
            ESP_LOGI(TAG, "joystick left pressed, going back");
            back_event_cb(NULL);
        } else if(idx == 2) {
            ESP_LOGI(TAG, "joystick up pressed, selecting previous item");
            ir_select_item(selected_ir_index - 1);
        } else if(idx == 4) {
            ESP_LOGI(TAG, "joystick down pressed, selecting next item");
            ir_select_item(selected_ir_index + 1);
        } else if(idx == 1) {
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
            // Check if the selected item is the Back option (ensure it's the last child)
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            lv_obj_t *last_child = lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1);
            if (last_child && lv_obj_get_user_data(last_child) == IR_BACK_OPTION_MAGIC_STR && selected_obj == last_child) {
                ESP_LOGI(TAG, "Joystick Enter pressed on Back option");
                back_event_cb(NULL);
                return;
            }
#endif
            bool top_level = (has_remotes_option || has_universals_option);
            if (!showing_commands) {
                if (top_level && has_remotes_option && selected_ir_index == 0) {
                    ESP_LOGI(TAG, "joystick enter pressed on Remotes, opening remotes directory");
                    remotes_event_cb(NULL);
                } else if (top_level && has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                    ESP_LOGI(TAG, "joystick enter pressed on Universals, opening universals directory");
                    universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                    ESP_LOGI(TAG, "joystick enter pressed on Learn Remote, starting IR learning");
                    learn_remote_event_cb(NULL);
                } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                    ESP_LOGI(TAG, "joystick enter pressed on Easy Learn toggle");
                    easy_learn_toggle_cb(NULL);
                } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 2)) {
                    ESP_LOGI(TAG, "joystick enter pressed on IR Dazzler");
                    dazzler_event_cb(NULL);
#else
                } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                    ESP_LOGI(TAG, "joystick enter pressed on IR Dazzler");
                    dazzler_event_cb(NULL);
#endif
                } else {
                    int top_count = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                        + 2  // Account for both Learn Remote and Easy Learn buttons
#endif
                        + 1  // Account for Dazzler button
                    ;
                    int file_idx = top_level ? (selected_ir_index - top_count) : selected_ir_index;
                    ESP_LOGI(TAG, "joystick enter pressed, opening selected file at index %d", file_idx);
                    file_event_open(file_idx);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                ESP_LOGI(TAG, "joystick enter pressed, executing selected command at index %d", selected_ir_index);
                command_event_execute(selected_ir_index);
            }
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        uint8_t keyValue = event->data.key_value;

        if ((keyValue == 44 || keyValue == ',') || (keyValue == 59 || keyValue == ';')) {
            ESP_LOGI(TAG, "Keyboard Left/Up button pressed\n");
            ir_select_item(selected_ir_index - 1);
        } else if ((keyValue == 47 || keyValue == '/') || (keyValue == 46 || keyValue == '.')) {
            ESP_LOGI(TAG, "Keyboard Right/Down button pressed\n");
            ir_select_item(selected_ir_index + 1);
        } else if (keyValue == 13) {
            ESP_LOGI(TAG, "Keyboard Enter button pressed\n");
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
            // Check if the selected item is the Back option
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            if (selected_obj && lv_obj_get_user_data(selected_obj) == IR_BACK_OPTION_MAGIC_STR) {
                ESP_LOGI(TAG, "Keyboard Enter pressed on Back option");
                back_event_cb(NULL);
                return;
            }
#endif
            bool top_level = (has_remotes_option || has_universals_option);
            if (!showing_commands) {
                // Check if we're in the top-level menu or in a file list
                if (top_level) {
                    // Top-level menu logic
                    if (has_remotes_option && selected_ir_index == 0) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Remotes, opening remotes directory");
                        remotes_event_cb(NULL);
                    } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Universals, opening universals directory");
                        universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                    } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Learn Remote");
                        learn_remote_event_cb(NULL);
                    } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on Easy Learn");
                        easy_learn_toggle_cb(NULL);
                    } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 2)) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on IR Dazzler");
                        dazzler_event_cb(NULL);
#else
                    } else if (top_level && selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        ESP_LOGI(TAG, "Keyboard Enter pressed on IR Dazzler");
                        dazzler_event_cb(NULL);
#endif
                    } else {
                        int top_count = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                            + 2  // Account for both Learn Remote and Easy Learn buttons
#endif
                            + 1  // Account for Dazzler button
                        ;
                        int file_idx = top_level ? (selected_ir_index - top_count) : selected_ir_index;
                        ESP_LOGI(TAG, "Keyboard Enter pressed, opening selected file at index %d", file_idx);
                        file_event_open(file_idx);
                    }
                } else {
                    // File list mode - direct file selection
                    ESP_LOGI(TAG, "Keyboard Enter pressed, opening selected file at index %d", selected_ir_index);
                    file_event_open(selected_ir_index);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                ESP_LOGI(TAG, "Keyboard Enter pressed, executing selected command at index %d", selected_ir_index);
                command_event_execute(selected_ir_index);
            }
        } else if (keyValue == 29 || keyValue == '`') {
            ESP_LOGI(TAG, "Keyboard Esc button pressed\n");
            back_event_cb(NULL);
        }
#ifdef CONFIG_USE_ENCODER
    } else if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) {
            // Check if the selected item is the Back option (ensure it's the last child)
            lv_obj_t *selected_obj = lv_obj_get_child(list, selected_ir_index);
            lv_obj_t *last_child = lv_obj_get_child(list, lv_obj_get_child_cnt(list) - 1);
            if (last_child && lv_obj_get_user_data(last_child) == IR_BACK_OPTION_MAGIC_STR && selected_obj == last_child) {
                ESP_LOGI(TAG, "Encoder button pressed on Back option");
                back_event_cb(NULL);
                return;
            }
            if (!showing_commands) {
                // Check if we're in the top-level menu or in a file list
                if (has_remotes_option || has_universals_option) {
                    // Top-level menu logic
                    if (has_remotes_option && selected_ir_index == 0) {
                        remotes_event_cb(NULL);
                    } else if (has_universals_option && selected_ir_index == (has_remotes_option ? 1 : 0)) {
                        universals_event_cb(NULL);
#ifdef CONFIG_HAS_INFRARED_RX
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        learn_remote_event_cb(NULL);
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1)) {
                        easy_learn_toggle_cb(NULL);
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 2)) {
                        dazzler_event_cb(NULL);
#else
                    } else if (selected_ir_index == ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0))) {
                        dazzler_event_cb(NULL);
#endif
                    } else {
                        int file_idx = selected_ir_index - ((has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0)
#ifdef CONFIG_HAS_INFRARED_RX
                            + 2  // Account for both Learn Remote and Easy Learn buttons
#endif
                            + 1  // Account for Dazzler button
                        );
                        file_event_open(file_idx);
                    }
                } else {
                    // File list mode - direct file selection
                    file_event_open(selected_ir_index);
                }
            } else {
                // Check if this is one of our remote management options
                // These are added after all the IR commands, so they have indices:
                // signal_count = Rename Remote
                // signal_count + 1 = Add New Signal
                // signal_count + 2 = Delete Remote
                
                if (showing_commands && !in_universals_mode) {
                    if (selected_ir_index == signal_count) {
                        // Rename Remote option
                        lv_event_t fake_event = {0};
                        rename_remote_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 1) {
                        // Add New Signal option
                        lv_event_t fake_event = {0};
                        add_signal_cb(&fake_event);
                        return;
                    } else if (selected_ir_index == signal_count + 2) {
                        // Delete Remote option
                        lv_event_t fake_event = {0};
                        delete_remote_cb(&fake_event);
                        return;
                    }
                }
                
                // Otherwise, it's a regular command
                command_event_execute(selected_ir_index);
            }
        } else {
            if (event->data.encoder.direction > 0) {
                ir_select_item(selected_ir_index + 1);
            } else {
                ir_select_item(selected_ir_index - 1);
            }
        }
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "Exit button pressed, returning to previous IR level");
        back_event_cb(NULL);
#endif
    }
}

// open selected IR file and list commands
// Open a universal-remote file and list its unique command names.
static void ir_open_universal_path(const char *full_path) {
    // clear previous unique names
    for (size_t i = 0; i < uni_command_count; i++) free(uni_command_names[i]);
    free(uni_command_names);
    uni_command_names = NULL;
    uni_command_count = 0;
    const char *fname = strrchr(full_path, '/');
    fname = fname ? fname + 1 : full_path;
    // Check if this is TURNHISTVOFF.ir - use universal IR system
    if (strcmp(fname, "TURNHISTVOFF.ir") == 0) {
        // Set up for universal IR transmission
        strcpy(current_universal_file, "TURNHISTVOFF");
        printf("Using universal IR system for TURNHISTVOFF\n");

        // Create a single command entry for universal transmission
        uni_command_names = malloc(sizeof(*uni_command_names));
        if (uni_command_names) {
            uni_command_names[0] = strdup("Power Off");
            uni_command_count = 1;
        }
    } else {
        // remember full path for transmit
        strncpy(current_universal_file, full_path, sizeof(current_universal_file) - 1);
        current_universal_file[sizeof(current_universal_file) - 1] = '\0';
        printf("scanning universal file: %s\n", current_universal_file);
        // scan file for unique command names
        bool susp = false; bool did = ir_sd_begin(&susp);
        FILE *f = fopen(full_path, "r"); if (!f) { if (did) ir_sd_end(susp); return; }
        char buf[256], last[256] = "";
        while (fgets(buf, sizeof(buf), f)) {
            char *s = buf;
            while (*s && isspace((unsigned char)*s)) s++;
            if (*s == '#' || *s == '\0') continue;
            if (strncmp(s, "name:", 5) == 0) {
                char *v = s + 5; while (*v && isspace((unsigned char)*v)) v++;
                char *e = v + strlen(v) - 1; while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
                if (strcmp(v, last) != 0) {
                    bool dup = false;
                    for (size_t j = 0; j < uni_command_count; j++) {
                        if (strcmp(uni_command_names[j], v) == 0) { dup = true; break; }
                    }
                    if (!dup) {
                        char **tmp = realloc(uni_command_names, (uni_command_count + 1) * sizeof(*uni_command_names));
                        if (!tmp) break;
                        uni_command_names = tmp;
                        char *name = strdup(v);
                        if (!name) break;
                        uni_command_names[uni_command_count] = name;
                        uni_command_count++;
                    }
                    strcpy(last, v);
                }
            }
        }
        fclose(f);
        if (did) ir_sd_end(susp);
        printf("found %zu unique commands\n", uni_command_count);
    }
    if (g_ir_ov) options_view_clear(g_ir_ov);
    showing_commands = true;
    selected_ir_index = 0;
    for (size_t i = 0; i < uni_command_count; i++) {
        options_view_add_item(g_ir_ov, uni_command_names[i], command_event_cb, (void*)(intptr_t)i);
    }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
    ir_add_back_row();
#endif
    num_ir_items = options_view_get_item_count(g_ir_ov);
    options_view_set_selected(g_ir_ov, 0);
}

// Open a saved remote file and list its signals plus management actions.
static void ir_open_remote_path(const char *full_path, const char *fname) {
    ESP_LOGI(TAG, "opening IR file: %s", full_path);

    // Save current remote info for management functions
    strncpy(current_remote_path, full_path, sizeof(current_remote_path) - 1);
    current_remote_path[sizeof(current_remote_path) - 1] = '\0';

    // Extract remote name (without .ir extension)
    strncpy(current_remote_name, fname, sizeof(current_remote_name) - 1);
    current_remote_name[sizeof(current_remote_name) - 1] = '\0';
    char *dot = strrchr(current_remote_name, '.');
    if (dot) *dot = '\0';

    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    // Free the previous screen's rows BEFORE parsing so LVGL returns its
    // widgets to the internal heap first -- on PSRAM-less boards (Cardputer)
    // this is often exactly the room the timings arrays need.
    if (g_ir_ov) options_view_clear(g_ir_ov);
    bool susp2 = false; bool did2 = ir_sd_begin(&susp2);
    if (!infrared_manager_read_list(full_path, &signals, &signal_count)) {
        if (did2) ir_sd_end(susp2);
        ESP_LOGE(TAG, "failed to read IR list from file: %s", full_path);
        return;
    }
    if (did2) ir_sd_end(susp2);
    showing_commands = true;
    selected_ir_index = 0;

    ESP_LOGI(TAG, "listing %zu commands for %s", signal_count, fname);

    for (size_t i = 0; i < signal_count; i++) {
        options_view_add_item(g_ir_ov, signals[i].name, command_event_cb, (void*)(intptr_t)i);
    }

    options_view_add_item(g_ir_ov, "Rename Remote", rename_remote_cb, NULL);
    options_view_add_item(g_ir_ov, "Add New Signal", add_signal_cb, NULL);
    lv_obj_t *delete_btn = options_view_add_item(g_ir_ov, "Delete Remote", delete_remote_cb, NULL);
    if (delete_btn) lv_obj_set_style_bg_color(delete_btn, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);

    num_ir_items = options_view_get_item_count(g_ir_ov);

#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
    ir_add_back_row();
    num_ir_items = options_view_get_item_count(g_ir_ov);
#endif
    options_view_set_selected(g_ir_ov, 0);
}

static void file_event_open(int idx) {
    if (in_universals_mode) {
        char path[256];
        size_t base_len = strlen(current_dir);
        if (base_len >= sizeof(path) - 1) base_len = sizeof(path) - 1;
        memcpy(path, current_dir, base_len);
        path[base_len] = '\0';
        snprintf(path + base_len, sizeof(path) - base_len, "/%s", ir_file_paths[idx]);
        ir_open_universal_path(path);
        return;
    }
    if (idx < 0 || idx >= ir_file_count) return;
    char path[256];
    size_t base_len = strlen(current_dir);
    if (base_len >= sizeof(path) - 1) base_len = sizeof(path) - 1;
    memcpy(path, current_dir, base_len);
    path[base_len] = '\0';
    if (base_len + 1 < sizeof(path)) {
        strncat(path, "/", sizeof(path) - strlen(path) - 1);
    }
    strncat(path, ir_file_paths[idx], sizeof(path) - strlen(path) - 1);
    ir_open_remote_path(path, ir_file_paths[idx]);
}

// Deep-link support for favorites: open a specific remote file. Safe to call
// before the view exists - the request is consumed by infrared_view_create().
void infrared_view_open_remote(const char *path) {
    if (!path || !path[0]) return;
    free(s_pending_remote_open);
    s_pending_remote_open = strdup(path);
    // View already live (e.g. lockscreen overlay on top of it): apply now.
    if (g_ir_ov && lv_obj_is_valid(g_ir_ov)) {
        infrared_view_apply_pending_open();
    }
}

static void infrared_view_apply_pending_open(void) {
    if (!s_pending_remote_open) return;
    char path[256];
    strncpy(path, s_pending_remote_open, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    free(s_pending_remote_open);
    s_pending_remote_open = NULL;
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;
    bool is_universals = strstr(path, "/universals/") != NULL;
    // Reuse the normal navigation path so flags (has_remotes_option etc.)
    // stay consistent for the back button. This avoids the state mismatch
    // that left has_remotes_option=true after a direct ir_open_* call and
    // later made the second back press fall through to display_manager_go_back
    // or try to rebuild a stale list.
    if (is_universals) {
        universals_event_cb(NULL);
    } else {
        remotes_event_cb(NULL);
    }
    // Find the requested file in the freshly populated list and open it
    for (size_t i = 0; i < ir_file_count; i++) {
        if (strcmp(ir_file_paths[i], fname) == 0) {
            file_event_open((int)i);
            return;
        }
    }
    // Fallback: path already compared via fname; if still not found, keep
    // the file list visible so the user can pick manually.
}

// execute selected IR command
static void command_event_execute(int idx) {
    if (in_universals_mode) {
        if (universal_task_handle) {
            printf("Universal transmission cancel requested.\n");
            universal_transmit_cancel = true;
            cleanup_transmit_popup(NULL);
            return;
        }
        if (idx < 0 || idx >= uni_command_count) return;

        if (settings_get_epilepsy_warning_enabled(&G_Settings)) {
            error_popup_create("EPILEPSY WARNING\nRGB LED will flash\nduring IR transmission");
        }

        UniversalTransmitArgs_t *args = heap_caps_malloc(sizeof(UniversalTransmitArgs_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!args) {
            args = heap_caps_malloc(sizeof(UniversalTransmitArgs_t), MALLOC_CAP_DEFAULT);
        }
        if (!args) {
            printf("Failed to allocate args for universal transmit task\n");
            cleanup_transmit_popup(NULL);
            return;
        }
        strncpy(args->path, current_universal_file, sizeof(args->path)-1);
        args->path[sizeof(args->path)-1] = '\0';
        strncpy(args->command, uni_command_names[idx], sizeof(args->command)-1);
        args->command[sizeof(args->command)-1] = '\0';
#if CONFIG_SPIRAM
        static StaticTask_t s_uni_tx_tcb;
        static StackType_t *s_uni_tx_stack = NULL;
        if (!s_uni_tx_stack) {
            /* The task stack MUST live in internal RAM, not PSRAM: this task
               does fopen/fread on the IR file and drives RMT, both of which can
               momentarily disable the flash cache that maps PSRAM. With the
               stack in PSRAM the CPU loses access to its own stack during those
               windows -> double exception with a garbage SP. */
            s_uni_tx_stack = heap_caps_malloc(4096 * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (s_uni_tx_stack) {
            universal_task_handle = xTaskCreateStatic(universal_transmit_task, "uni_tx_task", 4096, args, tskIDLE_PRIORITY + 1, s_uni_tx_stack, &s_uni_tx_tcb);
        }
        if (!universal_task_handle)
#endif
        {
            if (xTaskCreate(universal_transmit_task, "uni_tx_task", 4096, args, tskIDLE_PRIORITY + 1, &universal_task_handle) != pdPASS) {
                printf("universals job task create failed\n");
                cleanup_transmit_popup(NULL);
                heap_caps_free(args);
                universal_task_handle = NULL;
                return;
            }
        }
        printf("universals job task created\n");

        if (settings_get_epilepsy_warning_enabled(&G_Settings)) {
            if (transmit_popup_delay_timer) {
                lv_timer_del(transmit_popup_delay_timer);
                transmit_popup_delay_timer = NULL;
            }
            transmit_popup_delay_timer = lv_timer_create(create_transmit_popup_cb, TRANSMIT_POPUP_DELAY_MS, NULL);
            lv_timer_set_repeat_count(transmit_popup_delay_timer, 1);
        } else {
            create_transmit_popup_now();
        }
        return;
    }
    if (idx < 0 || idx >= signal_count) return;
    ESP_LOGI(TAG, "transmitting command: %s", signals[idx].name);
    status_display_show_status("IR Transmitting...");
    ghostchi_manager_add_xp(4);
    infrared_manager_transmit(&signals[idx]);
    status_display_show_status("IR Sent");
}

// LVGL event wrappers
static void file_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    file_event_open(idx);
}

static void placeholder_event_cb(lv_event_t *e) {
    // No-op for placeholder row
}

static void command_event_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    
    // Check if this is one of our remote management options
    // These are added after all the IR commands, so they have indices:
    // signal_count = Rename Remote
    // signal_count + 1 = Add New Signal
    // signal_count + 2 = Delete Remote
    
    if (showing_commands && !in_universals_mode) {
        if (idx == signal_count) {
            // Rename Remote option
            rename_remote_cb(e);
            return;
        } else if (idx == signal_count + 1) {
            // Add New Signal option
            add_signal_cb(e);
            return;
        } else if (idx == signal_count + 2) {
            // Delete Remote option
            delete_remote_cb(e);
            return;
        }
    }
    
    // Otherwise, it's a regular command
    command_event_execute(idx);
}

static void remotes_event_cb(lv_event_t *e) {
    // exit any universals mode
    in_universals_mode = false;
    has_remotes_option = false;
    has_universals_option = false;
    strcpy(current_dir, "/mnt/ghostesp/infrared/remotes");
    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    clear_ir_file_paths();
    showing_commands = false;
    refresh_ir_file_list(current_dir);
}

// Remote management callback functions
void rename_remote_cb(lv_event_t *e) {
    // Switch to keyboard view to get new name
    keyboard_view_set_submit_callback(rename_remote_keyboard_callback);

    // Use current remote name as placeholder
    if (strlen(current_remote_name) > 0) {
        keyboard_view_set_placeholder(current_remote_name);
    } else {
        keyboard_view_set_placeholder("Enter remote name");
    }
    display_manager_switch_view(&keyboard_view);
}

#ifdef CONFIG_HAS_INFRARED_RX
typedef enum {
    LEARNING_POPUP_STANDARD,
    LEARNING_POPUP_EASY_LEARN
} learning_popup_type_t;

typedef struct {
    const char *title;
    const char *instruction;
    int width;
    int height;
    bool has_skip_button;
    lv_event_cb_t cancel_cb;
    lv_event_cb_t skip_cb;
} learning_popup_config_t;

static void create_unified_learning_popup(learning_popup_type_t type, learning_popup_config_t *config) {
    lv_obj_t *popup;
    lv_obj_t *cancel_btn;
    lv_obj_t *skip_btn = NULL;
    lv_obj_t *instruction_label;

    if (type == LEARNING_POPUP_STANDARD) {
        learning_popup = popup_create_container(lv_scr_act(), config->width, config->height, true);
        popup = learning_popup;
    } else {
        easy_learn_popup = popup_create_container(lv_scr_act(), config->width, config->height, true);
        popup = easy_learn_popup;
    }

    // compute responsive button sizes/positions to avoid overlap on small screens
    lv_coord_t pw = lv_obj_get_width(popup);
    lv_coord_t edge = 8;
    lv_coord_t spacing = 24;

    if (config->has_skip_button) {
        lv_coord_t pair_w = pw - (2 * edge) - spacing;
        if (pair_w < 0) pair_w = 0;
        lv_coord_t btn_w = pair_w / 2;
        if (btn_w < 60) btn_w = 60;
        lv_coord_t left_x = edge + (btn_w / 2);
        lv_coord_t right_x = -(edge + (btn_w / 2));

        cancel_btn = popup_add_styled_button(popup, "Cancel", btn_w, 30, LV_ALIGN_BOTTOM_LEFT, left_x, -10, NULL, config->cancel_cb, NULL);
        skip_btn = popup_add_styled_button(popup, "Skip", btn_w, 30, LV_ALIGN_BOTTOM_RIGHT, right_x, -10, NULL, config->skip_cb, NULL);
        if (type == LEARNING_POPUP_EASY_LEARN) {
            easy_learn_cancel_btn = cancel_btn;
            easy_learn_skip_btn = skip_btn;
        }
    } else {
        lv_coord_t btn_w = pw - (2 * edge);
        if (btn_w < 80) btn_w = 80;
        if (btn_w > 160) btn_w = 160;
        cancel_btn = popup_add_styled_button(popup, "Cancel", btn_w, 30, LV_ALIGN_BOTTOM_MID, 0, -10, NULL, config->cancel_cb, NULL);
        if (type == LEARNING_POPUP_STANDARD) {
            learning_cancel_btn = cancel_btn;
        }
    }

    lv_obj_t *title_label = popup_create_title_label(popup, config->title, accessibility_get_font_body(), 20);
    (void)title_label;

    instruction_label = popup_create_body_label(popup, config->instruction, config->width - 20, true, accessibility_get_font_small(), 60);
    // center instruction text horizontally for both modes
    lv_obj_set_style_text_align(instruction_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(instruction_label, LV_ALIGN_TOP_MID, 0, 60);
    if (type == LEARNING_POPUP_EASY_LEARN) {
        easy_learn_instruction_label = instruction_label;
        lv_obj_align(instruction_label, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_text_color(instruction_label, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_text_align(instruction_label, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void create_learning_popup(void) {
    // compute responsive geometry from actual screen size; ensure it never exceeds viewport
    lv_coord_t scr_w = LV_HOR_RES;
    lv_coord_t scr_h = LV_VER_RES;
    lv_coord_t margin = 10;
    lv_coord_t popup_w;
    if (scr_w <= 240) {
        // 240x320 devices: match decoded popup convention (screen - 20)
        popup_w = scr_w - 20; // 220 on 240-wide
    } else {
        // wider devices (e.g., 320x170): match decoded popup width logic
        lv_coord_t base_w = scr_w - 20;
        if (base_w > 340) base_w = 340;
        if (base_w < 180) base_w = scr_w - 10;
        popup_w = base_w; // 300 on 320-wide
    }
    // match decoded popup height behavior on short displays (e.g., 320x170)
    lv_coord_t popup_h;
    lv_coord_t max_h = scr_h - (2 * margin);
    if (scr_h <= 200) {
        // same logic as create_signal_preview_popup: base 160, clamp to scr_h - 30 on very short screens
        popup_h = 160;
        if (scr_h < 200) popup_h = scr_h - 30;
        if (popup_h < 120) popup_h = 120;
    } else {
        // keep original for 240x320
        popup_h = 150;
        if (popup_h > max_h) popup_h = max_h;
        if (popup_h < 110) popup_h = 110;
    }

    learning_popup_config_t config = {
        .title = "Learning IR Signal",
        .instruction = "Press a button on your remote...",
        .width = popup_w,
        .height = popup_h,
        .has_skip_button = false,
        .cancel_cb = learning_cancel_cb,
        .skip_cb = NULL
    };

    create_unified_learning_popup(LEARNING_POPUP_STANDARD, &config);
}

// Function to start the IR learning task
static void start_ir_learning_task(void) {
    // Start IR learning task
    status_display_show_status("IR Ready");
    ir_learning_cancel = false;
    xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
}

void add_signal_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Add Signal button pressed");
    
    // Check if we have a remote selected
    if (current_remote_path[0] == '\0') {
        ESP_LOGW(TAG, "No remote selected for adding signal");
        // Show error popup
        lv_obj_t *error_popup = lv_msgbox_create(NULL, "Error", "Please select a remote first", NULL, true);
        lv_obj_center(error_popup);
        return;
    }
    
    // Set flag to indicate we're adding to existing remote
    add_signal_mode = true;
    
    // Reset button index for easy learn mode
    existing_remote_button_index = 0;
    
    // Create appropriate popup based on easy learn mode
    if (is_easy_mode) {
        // Create easy learn popup
        create_easy_learn_popup();
    } else {
        // Create learning popup
        create_learning_popup();
        
        // Start IR learning task
        start_ir_learning_task();
    }
}
#else
void add_signal_cb(lv_event_t *e) {
    ESP_LOGW(TAG, "IR RX not configured - cannot add signals");
}
#endif

void delete_remote_cb(lv_event_t *e) {
    (void)e;
    popup_confirm_show(&ir_confirm_popup, lv_layer_top(), "Delete Remote?",
                       "This remote will be permanently deleted.",
                       "Delete", "Cancel", ir_delete_remote_confirm_cb, NULL);
}

// implement universals option callback
static void universals_event_cb(lv_event_t *e) {
    has_remotes_option = false;
    has_universals_option = false;
    in_universals_mode = true;
    strcpy(current_dir, "/mnt/ghostesp/infrared/universals");
    ESP_LOGI(TAG, "entering universals mode, dir=%s", current_dir);
    if (signals) {
        infrared_manager_free_list(signals, signal_count);
        signals = NULL;
        signal_count = 0;
    }
    clear_ir_file_paths();
    showing_commands = false;
    refresh_ir_file_list(current_dir);
}


#ifdef CONFIG_HAS_INFRARED_RX
// IR learning functionality
static void cleanup_unified_learning_popup(learning_popup_type_t type)
{
    if (type == LEARNING_POPUP_STANDARD) {
        if (learning_popup) {
            lvgl_obj_del_safe(&learning_popup);
        }
    } else {
        if (easy_learn_popup) {
            lvgl_obj_del_safe(&easy_learn_popup);
            easy_learn_instruction_label = NULL;
        }
    }
}

static void cleanup_learning_popup(void *obj)
{
    cleanup_unified_learning_popup(LEARNING_POPUP_STANDARD);
}

void cleanup_easy_learn_popup(void *obj)
{
    cleanup_unified_learning_popup(LEARNING_POPUP_EASY_LEARN);
}

// Signal preview UI functions
void cleanup_signal_preview_popup(void *obj)
{
    (void)obj;
    lvgl_obj_del_safe(&signal_preview_popup);
    protocol_label = NULL;
    address_label = NULL;
    command_label = NULL;
    save_btn = NULL;
    cancel_btn = NULL;
}



void signal_preview_save_cb(lv_event_t *e)
{
    // Transition to keyboard view for naming
    status_display_show_status("IR Saving...");
    display_manager_lvgl_async_call(cleanup_signal_preview_popup, NULL);
    keyboard_view_set_placeholder("Enter signal name");
    
    // Use different callbacks based on whether we're adding to existing remote, easy learn mode, or creating new
    if (is_easy_mode) {
        // Easy learn mode - use automatic naming (takes priority over add_signal_mode)
        easy_learn_signal_name_callback();
        return; // Don't switch to keyboard view
    } else if (add_signal_mode) {
        // Adding signal to existing remote (non-easy mode)
        keyboard_view_set_submit_callback(add_signal_to_remote_callback);
    } else {
        // Learning new remote (original behavior)
        keyboard_view_set_submit_callback(learned_signal_name_callback);
    }
    
    display_manager_switch_view(&keyboard_view);
}

void easy_learn_signal_name_callback(void)
{
    // Get the signal name based on current button index (which gets updated by skip)
    const char *signal_name = "Unknown";
    if (existing_remote_button_index < num_common_buttons) {
        // Always use the current button index - this reflects what the user sees in the UI
        signal_name = common_button_names[existing_remote_button_index];
    }
    
    strncpy(learned_signal_name, signal_name, sizeof(learned_signal_name) - 1);
    learned_signal_name[sizeof(learned_signal_name) - 1] = '\0';
    
    // Save the learned signal based on mode
    if (add_signal_mode) {
        // Adding signal to existing remote
        append_signal_to_remote(learned_signal_name);
    } else {
        // Learning new remote
        save_learned_signal(learned_signal_name);
    }
    
    // Store whether we were in add signal mode before resetting the flag
    bool was_adding_to_existing = add_signal_mode && strlen(current_remote_path) > 0;
    
    // Reset the add signal mode flag
    add_signal_mode = false;
    
    // Move to the next button index for the next signal
    existing_remote_button_index++;
    
    // If we were adding to an existing remote, reload the remote's signal list to stay on it
    if (was_adding_to_existing) {
        // Reload the current remote's signals to refresh the view
        if (signals) {
            infrared_manager_free_list(signals, signal_count);
            signals = NULL;
            signal_count = 0;
        }
        if (infrared_manager_read_list(current_remote_path, &signals, &signal_count)) {
            if (g_ir_ov) options_view_clear(g_ir_ov);
            selected_ir_index = 0;
            
            for (size_t i = 0; i < signal_count; i++) {
                options_view_add_item(g_ir_ov, signals[i].name, command_event_cb, (void *)(intptr_t)i);
            }
            
            options_view_add_item(g_ir_ov, "Rename Remote", rename_remote_cb, NULL);
            options_view_add_item(g_ir_ov, "Add Signal", add_signal_cb, NULL);
            lv_obj_t *delete_btn = options_view_add_item(g_ir_ov, "Delete Remote", delete_remote_cb, NULL);
            if (delete_btn) lv_obj_set_style_bg_color(delete_btn, lv_color_hex(theme_palette_get_danger(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
            
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_USE_JOYSTICK) || defined(CONFIG_USE_ATOMS3R_BUTTON)
            ir_add_back_row();
#endif
            num_ir_items = options_view_get_item_count(g_ir_ov);
            options_view_set_selected(g_ir_ov, 0);
            
            ESP_LOGI(TAG, "Reloaded %zu signals for remote after adding new signal", signal_count);
        }
    }
    
    // We're already in the infrared view and have reloaded the signal list
    // Just close the popup - no need to switch views
    if (easy_learn_popup) {
        cleanup_easy_learn_popup(NULL);
    }
}

void signal_preview_cancel_cb(lv_event_t *e)
{
    // Clean up learned signal data (only for raw signals!)
    bool was_adding_cancel = add_signal_mode;
    status_display_show_status("IR Discarded");
    if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
        learned_signal.payload.raw.timings_size = 0;
    }
    learned_signal.is_raw = false;
    add_signal_mode = false;
    
    if (was_adding_cancel) {
        s_resume_commands = true;
    }
    
    // Clean up popup immediately (not async) to prevent UI corruption
    cleanup_signal_preview_popup(NULL);
    
    // Return to infrared view after cleanup is complete
    display_manager_switch_view(&infrared_view);
}

void easy_learn_cancel_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Easy Learn cancel button pressed");
    ir_learning_cancel = true;
    
    // Reset the add signal mode flag when cancelling
    add_signal_mode = false;
    
    if (ir_learning_task_handle) {
        // Task will clean up and close popup
    } else {
        cleanup_easy_learn_popup(NULL);
    }
}

void easy_learn_skip_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "Easy Learn skip button pressed");
    
    // Skip to next available button name (auto-skip already used ones)
    if (add_signal_mode && strlen(current_remote_path) > 0) {
        // When adding to existing remote, find next unused button
        int original_index = existing_remote_button_index;
        do {
            existing_remote_button_index++;
            if (existing_remote_button_index >= num_common_buttons) {
                existing_remote_button_index = 0; // Wrap around
            }
            
            // Prevent infinite loop by checking if we've wrapped around to the original index
            if (existing_remote_button_index == original_index) {
                break;
            }
            
            const char *button_name = common_button_names[existing_remote_button_index];
            if (!is_button_name_used(current_remote_path, button_name)) {
                break; // Found next unused button
            }
        } while (true);
    } else {
        // For new remotes, just increment normally
        existing_remote_button_index++;
        if (existing_remote_button_index >= num_common_buttons) {
            existing_remote_button_index = 0; // Wrap around
        }
    }
    
    // Update the instruction text to show the next button to learn
    update_easy_learn_instruction_text();
}

void update_signal_preview_selection(void)
{
    if (!save_btn || !cancel_btn) return;
    
    // Update button styles based on selection
    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    if (preview_selected_option == 0) {
        // Save selected - theme accent background
        lv_obj_set_style_bg_color(save_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(save_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *save_label = lv_obj_get_child(save_btn, 0);
        if (save_label) {
            if (theme_palette_is_bright(theme)) lv_obj_set_style_text_color(save_label, lv_color_hex(0x000000), 0);
            else lv_obj_set_style_text_color(save_label, lv_color_hex(0xFFFFFF), 0);
        }
        
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(theme_palette_get_surface_alt(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cancel_btn, lv_color_hex(theme_palette_get_border(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    } else {
        // Cancel selected - theme accent background
        lv_obj_set_style_bg_color(cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(cancel_btn, 0);
        if (cancel_label) {
            if (theme_palette_is_bright(theme)) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
            else lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
        }
        
        // Save unselected - dark background, white text
        lv_obj_set_style_bg_color(save_btn, lv_color_hex(theme_palette_get_surface_alt(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(save_btn, lv_color_hex(theme_palette_get_border(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *save_label = lv_obj_get_child(save_btn, 0);
        if (save_label) lv_obj_set_style_text_color(save_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- New: Update learning popup cancel button highlight ---
void update_learning_popup_selection(void)
{
    if (!learning_cancel_btn) return;
    if (preview_selected_option == 1) {
        // Cancel selected - theme accent background
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        lv_obj_set_style_bg_color(learning_cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(learning_cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(learning_cancel_btn, 0);
        if (cancel_label) {
            if (theme_palette_is_bright(theme)) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
            else lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(learning_cancel_btn, lv_color_hex(theme_palette_get_surface_alt(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(learning_cancel_btn, lv_color_hex(theme_palette_get_border(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(learning_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- Update Easy Learn popup button highlighting ---
void update_easy_learn_popup_selection(void)
{
    if (!easy_learn_cancel_btn || !easy_learn_skip_btn) return;
    
    // Update Cancel button
    if (easy_learn_selected_option == 0) {
        // Cancel selected - theme accent background
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        lv_obj_set_style_bg_color(easy_learn_cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_cancel_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(easy_learn_cancel_btn, 0);
        if (cancel_label) {
            if (theme_palette_is_bright(theme)) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0x000000), 0);
            else lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        // Cancel unselected - dark background, white text
        lv_obj_set_style_bg_color(easy_learn_cancel_btn, lv_color_hex(theme_palette_get_surface_alt(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_cancel_btn, lv_color_hex(theme_palette_get_border(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *cancel_label = lv_obj_get_child(easy_learn_cancel_btn, 0);
        if (cancel_label) lv_obj_set_style_text_color(cancel_label, lv_color_hex(0xFFFFFF), 0);
    }
    
    // Update Skip button
    if (easy_learn_selected_option == 1) {
        // Skip selected - theme accent background
        uint8_t theme = settings_get_menu_theme(&G_Settings);
        lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));
        lv_obj_set_style_bg_color(easy_learn_skip_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_skip_btn, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *skip_label = lv_obj_get_child(easy_learn_skip_btn, 0);
        if (skip_label) {
            if (theme_palette_is_bright(theme)) lv_obj_set_style_text_color(skip_label, lv_color_hex(0x000000), 0);
            else lv_obj_set_style_text_color(skip_label, lv_color_hex(0xFFFFFF), 0);
        }
    } else {
        // Skip unselected - dark background, white text
        lv_obj_set_style_bg_color(easy_learn_skip_btn, lv_color_hex(theme_palette_get_surface_alt(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(easy_learn_skip_btn, lv_color_hex(theme_palette_get_border(settings_get_menu_theme(&G_Settings))), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_t *skip_label = lv_obj_get_child(easy_learn_skip_btn, 0);
        if (skip_label) lv_obj_set_style_text_color(skip_label, lv_color_hex(0xFFFFFF), 0);
    }
}

// --- Update Easy Learn instruction text with current suggested button name ---
void update_easy_learn_instruction_text(void)
{
    if (!easy_learn_instruction_label) return;
    
    const char *button_name = "Unknown";
    
    // Always use the current button index to show what button we're currently suggesting
    if (existing_remote_button_index < num_common_buttons) {
        button_name = common_button_names[existing_remote_button_index];
    }
    
    lv_label_set_text_fmt(easy_learn_instruction_label, "Press '%s' button...", button_name);
}

// --- Check if a button name is already used in a remote file ---
bool is_button_name_used(const char* remote_path, const char* button_name)
{
    if (!remote_path || strlen(remote_path) == 0 || !button_name) {
        return false;
    }
    
    bool susp = false; bool did = ir_sd_begin(&susp);
    FILE *f = fopen(remote_path, "r");
    if (!f) {
        if (did) ir_sd_end(susp);
        return false;
    }
    
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        char *s = buf;
        // Skip whitespace
        while (*s && isspace((unsigned char)*s)) s++;
        // Skip comments and empty lines
        if (*s == '#' || *s == '\0') continue;
        
        // Look for "name:" lines
        if (strncmp(s, "name:", 5) == 0) {
            char *v = s + 5;
            // Skip whitespace after "name:"
            while (*v && isspace((unsigned char)*v)) v++;
            // Remove trailing whitespace
            char *e = v + strlen(v) - 1;
            while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
            
            // Check if this name matches the button name we're looking for
            if (strcmp(v, button_name) == 0) {
                fclose(f);
                if (did) ir_sd_end(susp);
                return true;
            }
        }
    }
    fclose(f);
    if (did) ir_sd_end(susp);
    return false;
}

// --- Get next available common button name by reading existing signals ---
const char* get_next_available_button_name(const char* remote_path)
{
    if (!remote_path || strlen(remote_path) == 0) {
        // No remote path, return first common button
        return common_button_names[0];
    }
    
    // Read existing signal names from the remote file
    bool susp = false; bool did = ir_sd_begin(&susp);
    FILE *f = fopen(remote_path, "r");
    if (!f) {
        // File doesn't exist or can't be opened, return first common button
        if (did) ir_sd_end(susp);
        return common_button_names[0];
    }
    
    // Track which common button names are already used
    bool used_buttons[num_common_buttons];
    memset(used_buttons, false, sizeof(used_buttons));
    
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        char *s = buf;
        // Skip whitespace
        while (*s && isspace((unsigned char)*s)) s++;
        // Skip comments and empty lines
        if (*s == '#' || *s == '\0') continue;
        
        // Look for "name:" lines
        if (strncmp(s, "name:", 5) == 0) {
            char *v = s + 5;
            // Skip whitespace after "name:"
            while (*v && isspace((unsigned char)*v)) v++;
            // Remove trailing whitespace
            char *e = v + strlen(v) - 1;
            while (e > v && isspace((unsigned char)*e)) *e-- = '\0';
            
            // Check if this name matches any common button name
            for (int i = 0; i < num_common_buttons; i++) {
                if (strcmp(v, common_button_names[i]) == 0) {
                    used_buttons[i] = true;
                    break;
                }
            }
        }
    }
    fclose(f);
    if (did) ir_sd_end(susp);
    
    // Find the first unused common button name
    for (int i = 0; i < num_common_buttons; i++) {
        if (!used_buttons[i]) {
            return common_button_names[i];
        }
    }
    
    // All common buttons are used, return the first one (user can override)
    return common_button_names[0];
}

// end rx-only block to expose filename generator for tx-only builds
#endif

// --- Generate unique remote filename by auto-incrementing if file exists ---
void generate_unique_remote_filename(const char* base_name, char* output_filename, size_t output_size)
{
    if (!base_name || !output_filename || output_size == 0) {
        return;
    }
    
    // Start with the base filename
    snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s.ir", base_name);
    
    // Check if file exists
    bool need_mount = !sd_card_manager.is_initialized;
    bool susp = false; bool did = false;
    if (need_mount) {
        did = ir_sd_begin(&susp);
        if (!did) {
            ESP_LOGE(TAG, "Failed to mount SD card while generating filename for %s", base_name);
            return;
        }
    }
    FILE *test_file = fopen(output_filename, "r");
    if (!test_file) {
        // File doesn't exist, we can use the base name
        if (need_mount && did) ir_sd_end(susp);
        return;
    }
    fclose(test_file);
    
    // File exists, try incrementing numbers
    int counter = 2;
    while (counter <= 999) { // Reasonable limit to prevent infinite loop
        snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s_%d.ir", base_name, counter);
        
        test_file = fopen(output_filename, "r");
        if (!test_file) {
            // This filename is available
            if (need_mount && did) ir_sd_end(susp);
            return;
        }
        fclose(test_file);
        counter++;
    }
    if (need_mount && did) ir_sd_end(susp);
    
    // If we get here, we couldn't find a unique name (very unlikely)
    // Fall back to using timestamp
    time_t now = time(NULL);
    snprintf(output_filename, output_size, "/mnt/ghostesp/infrared/remotes/learned_%s_%ld.ir", base_name, (long)now);
}

// resume rx-only block
#ifdef CONFIG_HAS_INFRARED_RX

void create_easy_learn_popup(void)
{
    learning_popup_config_t config = {
        .title = "Easy Learn Mode",
        .instruction = "", // Will be set by update_easy_learn_instruction_text()
        .width = LV_HOR_RES - 30,
        .height = 160,
        .has_skip_button = true,
        .cancel_cb = easy_learn_cancel_cb,
        .skip_cb = easy_learn_skip_cb
    };
    
    create_unified_learning_popup(LEARNING_POPUP_EASY_LEARN, &config);
    
    // Initialize button index to first unused button when adding to existing remote
    if (add_signal_mode && strlen(current_remote_path) > 0) {
        // Find first unused button name
        existing_remote_button_index = 0;
        for (int i = 0; i < num_common_buttons; i++) {
            if (!is_button_name_used(current_remote_path, common_button_names[i])) {
                existing_remote_button_index = i;
                break;
            }
        }
    }
    
    // Update instruction text with current suggested button name
    update_easy_learn_instruction_text();
    
    // Initialize selection to Cancel button and update highlighting
    easy_learn_selected_option = 0;
    update_easy_learn_popup_selection();
    
    // Start IR learning task
    ir_learning_cancel = false;
#ifdef CONFIG_SPIRAM
    if (ir_learning_task_stack) free(ir_learning_task_stack);
    if (ir_learning_task_tcb) free(ir_learning_task_tcb);
    const uint32_t ir_learning_stack_bytes = 8192;
    const uint32_t ir_learning_stack_words = (ir_learning_stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);
    /* Internal RAM, not PSRAM: ir_learning_task drives RMT RX and touches the
       filesystem, which can disable the flash cache that maps PSRAM and leave
       the task unable to reach a PSRAM-resident stack (double exception). */
    ir_learning_task_stack = (StackType_t *)heap_caps_malloc(ir_learning_stack_words * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ir_learning_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ir_learning_task_stack && ir_learning_task_tcb) {
        ir_learning_task_handle = xTaskCreateStatic(ir_learning_task, "ir_learning", ir_learning_stack_words, NULL, 5, ir_learning_task_stack, ir_learning_task_tcb);
    } else {
        if (ir_learning_task_stack) free(ir_learning_task_stack);
        if (ir_learning_task_tcb) free(ir_learning_task_tcb);
        ir_learning_task_stack = NULL; ir_learning_task_tcb = NULL;
        xTaskCreate(ir_learning_task, "ir_learning", 8192, NULL, 5, &ir_learning_task_handle);
    }
#else
    xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
#endif
}

void create_signal_preview_popup(void)
{
    // Create popup container responsively (works for both landscape and vertical)
    lv_coord_t scr_w = LV_HOR_RES;
    lv_coord_t scr_h = LV_VER_RES;
    lv_coord_t base_w = scr_w - 20; // small horizontal margin
    if (base_w > 340) base_w = 340; // cap to avoid overly wide on large screens
    if (base_w < 180) base_w = scr_w - 10; // very narrow screens
    lv_coord_t base_h = 160;
    if (scr_h < 200) base_h = scr_h - 30; // keep small margin on very short displays
    if (base_h < 120) base_h = 120;
    
    // Show status when signal is decoded and ready for preview
    if (learned_signal.is_raw) {
        status_display_show_status("IR Raw Signal");
    } else {
        status_display_show_status("IR Decoded");
    }
    
    signal_preview_popup = popup_create_container(lv_scr_act(), base_w, base_h, true);
    
    // Remove scrollbars and ensure content fits
    lv_obj_set_scrollbar_mode(signal_preview_popup, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(signal_preview_popup, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(signal_preview_popup, 8, 0);
    
    // Create buttons first to ensure proper z-order (styled helper)
    // compute responsive widths/offsets to prevent overlap and increase inner gap
    lv_coord_t pw_btn = lv_obj_get_width(signal_preview_popup);
    lv_coord_t edge_btn = 8;
    lv_coord_t spacing_btn = 24;
    lv_coord_t pair_w_btn = pw_btn - (2 * edge_btn) - spacing_btn;
    if (pair_w_btn < 0) pair_w_btn = 0;
    lv_coord_t btn_w = pair_w_btn / 2;
    if (btn_w < 60) btn_w = 60;
    lv_coord_t left_x = edge_btn + (btn_w / 2);
    lv_coord_t right_x = -(edge_btn + (btn_w / 2));

    save_btn = popup_add_styled_button(signal_preview_popup, "Save", btn_w, 30, LV_ALIGN_BOTTOM_LEFT, left_x, -5, NULL, signal_preview_save_cb, NULL);
    cancel_btn = popup_add_styled_button(signal_preview_popup, "Cancel", btn_w, 30, LV_ALIGN_BOTTOM_RIGHT, right_x, -5, NULL, signal_preview_cancel_cb, NULL);
    
    // Title
    popup_create_title_label(signal_preview_popup, "IR Signal Decoded", accessibility_get_font_body(), 10);
    
    // Protocol info (use popup helpers for consistent layout)
    lv_coord_t popup_w = lv_obj_get_width(signal_preview_popup);
    protocol_label = popup_create_body_label(signal_preview_popup, "", popup_w - 20, false, accessibility_get_font_small(), 32);
    address_label = popup_create_body_label(signal_preview_popup, "", popup_w - 20, false, accessibility_get_font_small(), 48);
    command_label = popup_create_body_label(signal_preview_popup, "", popup_w - 20, false, accessibility_get_font_small(), 64);

    // Set concise text
    if (!learned_signal.is_raw) {
        // Signal was successfully decoded
        lv_label_set_text_fmt(protocol_label, "%s", learned_signal.payload.message.protocol);
        lv_label_set_text_fmt(address_label, "Addr: 0x%lX", (unsigned long)learned_signal.payload.message.address);
        lv_label_set_text_fmt(command_label, "Cmd: 0x%lX", (unsigned long)learned_signal.payload.message.command);
    } else {
        lv_label_set_text(protocol_label, "Raw Signal");
        lv_label_set_text(address_label, "Unknown Protocol");
        lv_label_set_text(command_label, "");
    }

    // Style labels (popup helper already set some defaults)
    lv_obj_set_style_text_color(protocol_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(address_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_color(command_label, lv_color_hex(0xFFFFFF), 0);
    // center-align body labels to look better on narrow displays
    lv_obj_set_style_text_align(protocol_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(address_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_align(command_label, LV_TEXT_ALIGN_CENTER, 0);
    
    // Raw signal info (use popup helper for consistent layout)
    lv_coord_t popup_w2 = lv_obj_get_width(signal_preview_popup);
    lv_coord_t raw_y = !learned_signal.is_raw ? 80 : 64; // if decoded, place below cmd (80), else at cmd position (64)
    lv_obj_t *raw_info = popup_create_body_label(signal_preview_popup, "", popup_w2 - 20, false, accessibility_get_font_small(), raw_y);
    if (learned_signal.is_raw) {
        lv_label_set_text_fmt(raw_info, "%d timings", learned_signal.payload.raw.timings_size);
    } else {
        lv_label_set_text(raw_info, "");  // Don't show timing info for decoded signals
    }
    lv_obj_set_style_text_color(raw_info, lv_color_hex(0xCCCCCC), 0);
    
    // Set initial selection
    preview_selected_option = 0;
    update_signal_preview_selection();
}

static void save_learned_signal(const char *signal_name) {
    ESP_LOGI(TAG, "save_learned_signal called with name: %s", signal_name ? signal_name : "NULL");
    ESP_LOGI(TAG, "Signal data: timings=%p, size=%d, is_raw=%d", 
             learned_signal.payload.raw.timings, 
             learned_signal.payload.raw.timings_size,
             learned_signal.is_raw);
    
    if (!signal_name || strlen(signal_name) == 0) {
        ESP_LOGE(TAG, "Invalid signal name provided");
        // Free timing data if it exists to prevent memory leaks (only for raw signals!)
        if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }

    // Check if we have valid signal data (raw signals need timing data)
    if (learned_signal.is_raw) {
        if (!learned_signal.payload.raw.timings || learned_signal.payload.raw.timings_size == 0) {
            ESP_LOGE(TAG, "No timing data in raw signal");
            return;
        }
    }
    
    // do not attempt to validate pointer address range; trust allocation
    
    // Add a small delay to ensure any pending operations are complete
    vTaskDelay(pdMS_TO_TICKS(10));
    
    char filename[256];
    // Ensure the signal name doesn't cause buffer overflow
    // Use a conservative approach to avoid compiler warnings
    size_t max_signal_name_len = sizeof(filename) - 64; // Reserve 64 chars for path prefix and safety margin
    
    // Ensure max_signal_name_len is reasonable
    if (max_signal_name_len <= 0 || max_signal_name_len > 190) {
        max_signal_name_len = 190; // Set a reasonable limit
    }
    
    // Truncate signal name if too long
    char truncated_signal_name[max_signal_name_len + 1];
    strncpy(truncated_signal_name, signal_name, max_signal_name_len);
    truncated_signal_name[max_signal_name_len] = '\0';
    
    // Generate unique filename to prevent overwriting existing remotes
    generate_unique_remote_filename(truncated_signal_name, filename, sizeof(filename));
    
    // Final safety check for filename length
    if (strlen(filename) >= sizeof(filename) - 1) {
        ESP_LOGE(TAG, "Generated filename would be too long");
        // Free timing data if it exists to prevent memory leaks (only for raw signals!)
        if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
        }
        return;
    }
    
    ESP_LOGI(TAG, "Creating IR file: %s", filename);
    
    ESP_LOGI(TAG, "Opening file for writing...");

    // Offload to worker
    IrIoJob_t job = {0};
    job.op = IR_IO_SAVE;
    // aux carries the base name (signal name)
    strncpy(job.aux, signal_name, sizeof(job.aux)-1);
    if (!learned_signal.is_raw) {
        // Signal was decoded
        job.has_decoded = true;
        strncpy(job.protocol, learned_signal.payload.message.protocol, sizeof(job.protocol)-1);
        job.address = learned_signal.payload.message.address;
        job.command = learned_signal.payload.message.command;
    } else {
        // Signal is raw
        job.is_raw = true;
        job.frequency = learned_signal.payload.raw.frequency;
        job.duty_cycle = learned_signal.payload.raw.duty_cycle;
        job.timings_size = learned_signal.payload.raw.timings_size;
        job.timings = malloc(job.timings_size * sizeof(uint32_t));
        if (job.timings) memcpy(job.timings, learned_signal.payload.raw.timings, job.timings_size * sizeof(uint32_t));
    }
    if (ir_sd_queue) xQueueSend(ir_sd_queue, &job, 0);
    
    ESP_LOGI(TAG, "Queued IR signal save to %s", filename);

    // Free timing data to prevent memory leaks (only for raw signals!)
    if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
        free(learned_signal.payload.raw.timings);
        learned_signal.payload.raw.timings = NULL;
    }
}



// RMT RX callback function - Removed, using infrared_manager's callback

static void ir_learning_task(void *arg) {
    ESP_LOGI(TAG, "IR learning task started");
    
    // Reset learning cancel flag
    ir_learning_cancel = false;
    
    // Check if IR RX is initialized (GPIO-based implementation)
    // Ensure manager is initialized
    if (!infrared_manager_rx_init()) {
        ESP_LOGE(TAG, "Failed to init infrared manager RX");
        display_manager_lvgl_async_call(cleanup_learning_popup, NULL);
        ir_learning_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (!infrared_manager_rx_is_initialized()) {
        ESP_LOGE(TAG, "IR RX not initialized");
        display_manager_lvgl_async_call(cleanup_learning_popup, NULL);
        ir_learning_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Use GPIO-based IR RX - much simpler!
    ESP_LOGI(TAG, "Waiting for IR signal...");

    while (!ir_learning_cancel) {
        // infrared_manager_rx_receive handles all the decoding internally
        // It returns a fully decoded signal (or raw if decoding fails)
        memset(&learned_signal, 0, sizeof(learned_signal));

        if (infrared_manager_rx_receive(&learned_signal, 1000)) {  // 1 second timeout per attempt
            ESP_LOGI(TAG, "IR signal received successfully");

            // Set flag to preserve timing data during view transition
            preserve_learned_signal = true;

            // Signal received successfully, show preview popup
            if (is_easy_mode) {
                display_manager_lvgl_async_call(cleanup_easy_learn_popup, NULL);
            } else {
                display_manager_lvgl_async_call(cleanup_learning_popup, NULL);
            }

            // Create and show signal preview popup
            display_manager_lvgl_async_call((lv_async_cb_t)create_signal_preview_popup, NULL);

            ir_learning_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }

        // Check for cancel flag after timeout
        if (ir_learning_cancel) {
            break;
        }

        // No signal received in this iteration, continue listening
        ESP_LOGD(TAG, "No IR signal received, continuing to listen...");
    }

    // Cleanup - only reached if learning was cancelled
    if (ir_learning_cancel) {
        // Clean up any allocated memory in learned_signal
        if (learned_signal.is_raw && learned_signal.payload.raw.timings) {
            free(learned_signal.payload.raw.timings);
            learned_signal.payload.raw.timings = NULL;
            learned_signal.payload.raw.timings_size = 0;
        }

        display_manager_lvgl_async_call(cleanup_learning_popup, NULL);
    }

    ir_learning_task_handle = NULL;
    vTaskDelete(NULL);
}

#endif

void learning_cancel_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Learn Remote cancel button pressed");
#ifdef CONFIG_HAS_INFRARED_RX
    ir_learning_cancel = true;
    
    // Reset the add signal mode flag when cancelling
    add_signal_mode = false;
    
    if (ir_learning_task_handle) {
        // Task will clean up and close popup
    } else {
        cleanup_learning_popup(NULL);
    }
#else
    cleanup_learning_popup(NULL);
#endif
}

void easy_learn_toggle_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Easy Learn toggle pressed");
    
#ifdef CONFIG_HAS_INFRARED_RX
    // Toggle the easy mode state
    is_easy_mode = !is_easy_mode;
    
    // Update the settings
    settings_set_infrared_easy_mode(&G_Settings, is_easy_mode);
    settings_save(&G_Settings);
    
    // Keep the iOS toggle widget in sync with `is_easy_mode`. Touch-driven
    // LVGL events deliver `e` so we can grab the track directly; encoder/
    // keyboard callbacks arrive with `e == NULL`, so we locate the toggle
    // child the same way the settings rows do.
    lv_obj_t *btn = NULL;
    if (e != NULL) {
        btn = lv_event_get_target(e);
    } else {
        // Easy Learn button is at index: (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1
        int easy_learn_index = (has_remotes_option ? 1 : 0) + (has_universals_option ? 1 : 0) + 1;
        btn = lv_obj_get_child(list, easy_learn_index);
    }
    
    if (btn && lv_obj_is_valid(btn)) {
        uint32_t child_cnt = lv_obj_get_child_cnt(btn);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(btn, i);
            if (child && lv_obj_get_user_data(child) == IOS_TOGGLE_USER_DATA) {
                ios_toggle_set_value(child, is_easy_mode, e != NULL);
                break;
            }
        }
    }
    
    ESP_LOGI(TAG, "Easy Learn mode %s", is_easy_mode ? "enabled" : "disabled");
#else
    // Do nothing if IR RX is not configured
    ESP_LOGI(TAG, "Easy Learn mode not available (IR RX not configured)");
#endif
}

#ifdef CONFIG_HAS_INFRARED_RX

static void learn_remote_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Learn Remote button pressed");
    
    // Reset flag to indicate we're learning a new remote (not adding to existing)
    add_signal_mode = false;
    
    // Reset button index for easy learn mode
    existing_remote_button_index = 0;
    
    // Create appropriate popup based on easy learn mode
    if (is_easy_mode) {
        // Create easy learn popup
        create_easy_learn_popup();
    } else {
        // Use unified responsive popup for standard flow
        create_learning_popup();
        // Start IR learning task
        ir_learning_cancel = false;
#ifdef CONFIG_SPIRAM
        if (ir_learning_task_stack) free(ir_learning_task_stack);
        if (ir_learning_task_tcb) free(ir_learning_task_tcb);
        const uint32_t ir_learning_stack_bytes = 8192;
        const uint32_t ir_learning_stack_words = (ir_learning_stack_bytes + sizeof(StackType_t) - 1) / sizeof(StackType_t);
        /* Internal RAM, not PSRAM: ir_learning_task drives RMT RX and touches the
       filesystem, which can disable the flash cache that maps PSRAM and leave
       the task unable to reach a PSRAM-resident stack (double exception). */
    ir_learning_task_stack = (StackType_t *)heap_caps_malloc(ir_learning_stack_words * sizeof(StackType_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ir_learning_task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (ir_learning_task_stack && ir_learning_task_tcb) {
            ir_learning_task_handle = xTaskCreateStatic(ir_learning_task, "ir_learning", ir_learning_stack_words, NULL, 5, ir_learning_task_stack, ir_learning_task_tcb);
        } else {
            if (ir_learning_task_stack) free(ir_learning_task_stack);
            if (ir_learning_task_tcb) free(ir_learning_task_tcb);
            ir_learning_task_stack = NULL; ir_learning_task_tcb = NULL;
            xTaskCreate(ir_learning_task, "ir_learning", 8192, NULL, 5, &ir_learning_task_handle);
        }
#else
        xTaskCreate(ir_learning_task, "ir_learning", 4096, NULL, 5, &ir_learning_task_handle);
#endif
    }
}
#endif

View infrared_view = {
    .root = NULL,
    .create = infrared_view_create,
    .destroy = infrared_view_destroy,
    .name = "Infrared View",
    .get_hardwareinput_callback = NULL,
    .input_callback = infrared_view_input_cb
};
