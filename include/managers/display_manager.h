#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "lvgl.h"
#include <stdbool.h>
#include "managers/joystick_manager.h"


typedef void* QueueHandle_tt;
typedef void* SemaphoreHandle_tt; // Because Circular Includes are fun :)



#define INPUT_QUEUE_LENGTH    10
#define INPUT_ITEM_SIZE       sizeof(int)
QueueHandle_tt input_queue;

#define MUTEX_TIMEOUT_MS 100


#define HARDWARE_INPUT_TASK_PRIORITY    (4)
#define INPUT_PROCESSING_TASK_PRIORITY  (3)
#define RENDERING_TASK_PRIORITY         (5)

typedef struct {
    lv_obj_t *root;
    void (*create)(void);  
    void (*destroy)(void);
    void (*hardwareinput_callback)(int);
    const char* name;
    void (*get_hardwareinput_callback)(void **callback);
} View;


typedef struct {
    View *current_view; 
    View *previous_view; 
    SemaphoreHandle_tt mutex;
} DisplayManager;

/* Function prototypes */

/**
 * @brief Initialize the Display Manager.
 */
void display_manager_init(void);

/**
 * @brief Register a new view.
 */
bool display_manager_register_view(View *view);

/**
 * @brief Switch to a new view.
 */
void display_manager_switch_view(View *view);

/**
 * @brief Destroy the current view.
 */
void display_manager_destroy_current_view(void);

/**
 * @brief Get the current active view.
 */
View *display_manager_get_current_view(void);


void lvgl_tick_task(void *arg);

void hardware_input_task(void *pvParameters);

void input_processing_task(void *pvParameters);

void display_manager_fill_screen(lv_color_t color);

// Status Bar Functions

void update_status_bar(bool wifi_enabled, bool bt_enabled, bool sd_card_mounted, int batteryPercentage);

void display_manager_add_status_bar(const char* CurrentMenuName);


LV_IMG_DECLARE(Ghost_ESP);
LV_IMG_DECLARE(Map);
LV_IMG_DECLARE(bluetooth);
LV_IMG_DECLARE(Settings);
LV_IMG_DECLARE(wifi);

joystick_t joysticks[5];


#endif /* DISPLAY_MANAGER_H */