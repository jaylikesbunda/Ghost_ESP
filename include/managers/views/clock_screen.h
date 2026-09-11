#ifndef CLOCK_SCREEN_H
#define CLOCK_SCREEN_H

#include "lvgl.h"
#include "managers/display_manager.h"

void clock_create(void);
void clock_destroy(void);
void get_clock_callback(void **callback);

extern View clock_view;
extern lv_timer_t *clock_timer;

#endif /* CLOCK_SCREEN_H */ 