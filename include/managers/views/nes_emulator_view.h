#ifndef NES_EMULATOR_SCREEN_H
#define NES_EMULATOR_SCREEN_H


#include "display_manager.h"

// View structure for managing the NES emulator screen
extern View nes_emulator_view;

// Function declarations for creating and destroying the view
void nes_emulator_view_create(void);
void nes_emulator_view_destroy(void);

// Input callback function for handling hardware inputs
void nes_emulator_view_hardwareinput_callback(InputEvent *event);
void nes_emulator_view_get_hardwareinput_callback(void **callback);

void write_frame(const uint16_t x, const uint16_t y, const uint16_t width, const uint16_t height, const uint8_t *data[]);

// Main game loop for the NES emulator
void nes_emulator_loop(lv_timer_t *timer);

// Function to draw a pixel on the canvas
void draw_pixel(int x, int y, lv_color_t color);

// Function to draw the current framebuffer to the canvas
void draw_framebuffer_to_canvas(void);

#endif // NES_EMULATOR_SCREEN_H