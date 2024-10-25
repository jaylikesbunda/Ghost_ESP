#include "managers/views/nes_emulator_screen.h"
#include "core/serial_manager.h"
#include "managers/views/main_menu_screen.h"
#include <stdlib.h>
#include <string.h>
#include "nes_emulator_view.h"

lv_obj_t *nes_canvas = NULL;
lv_obj_t *status_bar = NULL;
lv_timer_t *emulator_loop_timer = NULL;
bool is_emulator_running = false;
uint8_t *nes_framebuffer = NULL;

void draw_pixel(int x, int y, lv_color_t color) {
    if (x >= 0 && x < LV_HOR_RES && y >= 0 && y < LV_VER_RES) {
        lv_canvas_set_px_color(nes_canvas, x, y, color);
    }
}

void draw_framebuffer_to_canvas() {
    if (!nes_framebuffer) return;
    for (int y = 0; y < LV_VER_RES; y++) {
        for (int x = 0; x < LV_HOR_RES; x++) {
            uint8_t pixel = nes_framebuffer[y * LV_HOR_RES + x];
            lv_color_t color = pixel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
            draw_pixel(x, y, color);
        }
    }
    lv_obj_invalidate(nes_canvas);
}

void nes_emulator_view_create(void) {
    if (nes_emulator_view.root != NULL) {
        return;
    }

    nes_emulator_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(nes_emulator_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(nes_emulator_view.root, lv_color_black(), 0);

   
    nes_canvas = lv_canvas_create(nes_emulator_view.root);
    lv_obj_set_size(nes_canvas, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(nes_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_canvas_set_buffer(nes_canvas, malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(LV_HOR_RES, LV_VER_RES)), LV_HOR_RES, LV_VER_RES, LV_IMG_CF_TRUE_COLOR);

    
    display_manager_add_status_bar("NES Emulator");


    is_emulator_running = true;


    
}

void nes_emulator_view_destroy(void) {
    if (nes_emulator_view.root != NULL) {
        is_emulator_running = false;
        if (emulator_loop_timer != NULL) {
            lv_timer_del(emulator_loop_timer);
            emulator_loop_timer = NULL;
        }

        
        lv_obj_del(nes_emulator_view.root);
        nes_emulator_view.root = NULL;
        nes_canvas = NULL;
        status_bar = NULL;

        
        if (nes_framebuffer) {
            free(nes_framebuffer);
            nes_framebuffer = NULL;
        }
    }
}

void nes_emulator_view_hardwareinput_callback(InputEvent *event) {
    if (!is_emulator_running) {
        return;
    }


    if (event->type == INPUT_TYPE_TOUCH) {
        int touch_x = event->data.touch_data.point.x;
        int touch_y = event->data.touch_data.point.y;


        if (touch_x < 50 && touch_y < 50) {
            display_manager_switch_view(&main_menu_view);
        }
    } else if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
    }
}

void nes_emulator_view_get_hardwareinput_callback(void **callback) {
    if (callback != NULL) {
        *callback = (void *)nes_emulator_view_hardwareinput_callback;
    }
}

void write_frame(const uint16_t x, const uint16_t y, const uint16_t width, const uint16_t height, const uint8_t *data[])
{
    if (!nes_canvas || !data) return;

    for (uint16_t row = 0; row < height; row++) {
        for (uint16_t col = 0; col < width; col++) {
            uint8_t pixel = data[row][col];

            lv_color_t color = pixel ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);

            
            draw_pixel(x + col, y + row, color);
        }
    }

    lv_obj_invalidate(nes_canvas);
}

View nes_emulator_view = {
    .root = NULL,
    .create = nes_emulator_view_create,
    .destroy = nes_emulator_view_destroy,
    .input_callback = nes_emulator_view_hardwareinput_callback,
    .name = "NESEmulatorView",
    .get_hardwareinput_callback = nes_emulator_view_get_hardwareinput_callback
};