// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#undef false
#undef true
#undef bool

#include <math.h>
#include <string.h>
#include <noftypes.h>
#include <bitmap.h>
#include <nofconfig.h>
#include <event.h>
#include <gui.h>
#include <log.h>
#include <nes.h>
#include <nes_pal.h>
#include <nesinput.h>
#include <osd.h>
#include <stdint.h>
#include "driver/i2s.h"
#include "sdkconfig.h"
#include <spi_lcd.h>
#include "lvgl.h"
#include "managers/views/nes_emulator_view.h"

#include <psxcontroller.h>

#define DEFAULT_WIDTH        LV_HOR_RES
#define DEFAULT_HEIGHT       LV_VER_RES

TimerHandle_t timer;

// Timer function for synchronizing NES frames
int osd_installtimer(int frequency, void *func, int funcsize, void *counter, int countersize)
{
    printf("Timer install, freq=%d\n", frequency);
    timer = xTimerCreate("nes", configTICK_RATE_HZ / frequency, pdTRUE, NULL, func);
    xTimerStart(timer, 0);
    return 0;
}

/*
** Video
*/

static int init(int width, int height);
static void shutdown(void);
static int set_mode(int width, int height);
static void set_palette(rgb_t *pal);
static void clear(uint8 color);
static bitmap_t *lock_write(void);
static void free_write(int num_dirties, rect_t *dirty_rects);
static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects);

QueueHandle_t vidQueue;

viddriver_t sdlDriver =
{
   "Simple DirectMedia Layer",         /* name */
   init,          /* init */
   shutdown,      /* shutdown */
   set_mode,      /* set_mode */
   set_palette,   /* set_palette */
   clear,         /* clear */
   lock_write,    /* lock_write */
   free_write,    /* free_write */
   custom_blit,   /* custom_blit */
   false          /* invalidate flag */
};

bitmap_t *myBitmap;

// Video initialization
static int init(int width, int height)
{
    return 0;
}

static void shutdown(void)
{
}

// Set the video mode (resolution)
static int set_mode(int width, int height)
{
    return 0;
}

uint16 myPalette[256];

// Copy NES palette over to hardware
static void set_palette(rgb_t *pal)
{
    uint16 c;
    for (int i = 0; i < 256; i++)
    {
        c = (pal[i].b >> 3) + ((pal[i].g >> 2) << 5) + ((pal[i].r >> 3) << 11);
        myPalette[i] = c;
    }
}

// Clear all frames to a particular color
static void clear(uint8 color)
{
    // Not needed for now, can be used to reset the frame if necessary.
}

// Acquire the direct buffer for writing
static bitmap_t *lock_write(void)
{
    myBitmap = bmp_createhw((uint8 *)fb, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_WIDTH * 2);
    return myBitmap;
}

// Release the resource after writing
static void free_write(int num_dirties, rect_t *dirty_rects)
{
    bmp_destroy(&myBitmap);
}

// Custom blit function to send the frame to the display
static void custom_blit(bitmap_t *bmp, int num_dirties, rect_t *dirty_rects)
{
    xQueueSend(vidQueue, &bmp, 0);
}

// Task for rendering the video frames
static void videoTask(void *arg)
{
    int x = (320 - DEFAULT_WIDTH) / 2;
    int y = ((240 - DEFAULT_HEIGHT) / 2);
    bitmap_t *bmp = NULL;

    while (1)
    {
        xQueueReceive(vidQueue, &bmp, portMAX_DELAY);
        write_frame(x, y, DEFAULT_WIDTH, DEFAULT_HEIGHT, (const uint8_t **)bmp->line);
    }
}

/*
** Input
*/

static void osd_initinput()
{
    psxcontrollerInit();
}

void osd_getinput(void)
{
    const int ev[16] = {
        event_joypad1_select, 0, 0, event_joypad1_start, event_joypad1_up, event_joypad1_right,
        event_joypad1_down, event_joypad1_left, 0, 0, 0, 0, event_soft_reset, event_joypad1_a,
        event_joypad1_b, event_hard_reset
    };

    static int oldb = 0xffff;
    int b = psxReadInput();
    int chg = b ^ oldb;
    oldb = b;
    event_t evh;

    for (int x = 0; x < 16; x++)
    {
        if (chg & 1)
        {
            evh = event_get(ev[x]);
            if (evh)
                evh((b & 1) ? INP_STATE_BREAK : INP_STATE_MAKE);
        }
        chg >>= 1;
        b >>= 1;
    }
}

static void osd_freeinput(void)
{
}

void osd_getmouse(int *x, int *y, int *button)
{
    // No mouse input needed for NES.
}

/*
** Shutdown
*/

// Shutdown and cleanup resources
void osd_shutdown()
{
    osd_freeinput();
}

static int logprint(const char *string)
{
    return printf("%s", string);
}

/*
** Startup
*/

int osd_init()
{
    log_chain_logfunc(logprint);

	
    vidQueue = xQueueCreate(1, sizeof(bitmap_t *));
    xTaskCreatePinnedToCore(&videoTask, "videoTask", 2048, NULL, 5, NULL, 1);
    //osd_initinput();
    return 0;
}