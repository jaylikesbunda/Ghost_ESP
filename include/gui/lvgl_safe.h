#pragma once

#include "lvgl.h"

void lvgl_obj_del_safe(lv_obj_t **obj);
void lvgl_timer_del_safe(lv_timer_t **timer);

/* Standard view-destroy pattern: delete LVGL object/timer and NULL the
 * caller's pointer. Hoist timer deletes OUTSIDE any `if (root)` guard so a
 * partially-constructed view cannot leak when create() aborts early. */
#define VIEW_DESTROY_OBJ(ptr) lvgl_obj_del_safe(&(ptr))
#define VIEW_DESTROY_TIMER(tmr) lvgl_timer_del_safe(&(tmr))
#define VIEW_CLEAR_PTR(ptr) do { (ptr) = NULL; } while (0)
