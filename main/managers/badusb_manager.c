#include "sdkconfig.h"

#ifdef CONFIG_HAS_BADUSB

#include "managers/badusb_manager.h"
#include "managers/badusb_builtin_script.h"
#include "managers/hid_script_parser.h"
#include "managers/ghostchi_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/settings_manager.h"
#include "core/glog.h"
#include "core/esp_comm_manager.h"
#include "core/serial_manager.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"
#include "driver/gpio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dirent.h>

static const char *TAG = "badusb";

static const uint8_t hid_keyboard_report_desc[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Minimum (224)
    0x29, 0xE7,       //   Usage Maximum (231)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute) -- Modifier byte
    0x95, 0x01,       //   Report Count (1)
    0x75, 0x08,       //   Report Size (8)
    0x81, 0x01,       //   Input (Constant) -- Reserved byte
    0x95, 0x06,       //   Report Count (6)
    0x75, 0x08,       //   Report Size (8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (101)
    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (101)
    0x81, 0x00,       //   Input (Data, Array) -- Key arrays (6 keys)
    0xC0              // End Collection
};

static const uint8_t hid_mouse_report_desc[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Buttons)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x03,       //     Usage Maximum (3)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x75, 0x01,       //     Report Size (1)
    0x95, 0x03,       //     Report Count (3)
    0x81, 0x02,       //     Input (Data, Variable, Absolute) -- 3 buttons
    0x75, 0x05,       //     Report Size (5)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x01,       //     Input (Constant) -- 5-bit padding
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x03,       //     Report Count (3)
    0x81, 0x06,       //     Input (Data, Variable, Relative) -- X, Y, Wheel
    0xC0,             //   End Collection (Physical)
    0xC0              // End Collection (Application)
};

static const uint8_t hid_consumer_report_desc[] = {
    0x05, 0x0C,       // Usage Page (Consumer)
    0x09, 0x01,       // Usage (Consumer Control)
    0xA1, 0x01,       // Collection (Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x03,       //   Report Count (3)
    0x0A, 0xE9, 0x00, //   Usage (Volume Increment)
    0x0A, 0xEA, 0x00, //   Usage (Volume Decrement)
    0x0A, 0xE2, 0x00, //   Usage (Mute)
    0x81, 0x02,       //   Input (Data, Variable, Absolute)
    0x75, 0x05,       //   Report Size (5)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Constant)
    0xC0              // End Collection
};

static bool s_initialized = false;
static bool s_driver_installed = false;
static bool s_active = false;
static volatile bool s_stop_requested = false;

#define MIN_KEY_DELAY_MS 10
#define CONSUMER_HID_INTERVAL_MS 2

static tusb_desc_device_t device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1234,
    .idProduct          = 0x5678,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

enum {
    ITF_NUM_HID_KEYBOARD,
    ITF_NUM_HID_MOUSE,
    ITF_NUM_HID_CONSUMER,
    ITF_NUM_TOTAL
};

enum {
    HID_INSTANCE_KEYBOARD,
    HID_INSTANCE_MOUSE,
    HID_INSTANCE_CONSUMER,
};

#define EPNUM_HID_KEYBOARD  0x81
#define EPNUM_HID_MOUSE     0x82
#define EPNUM_HID_CONSUMER  0x83

// Each TUD_HID_DESC_LEN is 9+7+7 = 23 bytes
#define BADUSB_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, BADUSB_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYBOARD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(hid_keyboard_report_desc), EPNUM_HID_KEYBOARD, CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_MOUSE, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(hid_mouse_report_desc), EPNUM_HID_MOUSE, CFG_TUD_HID_EP_BUFSIZE, 10),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_CONSUMER, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_consumer_report_desc), EPNUM_HID_CONSUMER,
                       CFG_TUD_HID_EP_BUFSIZE, CONSUMER_HID_INTERVAL_MS),
};

static char mfr_string[33] = "Ghost ESP";
static char prod_string[33] = "BadUSB HID";

static const char *string_descriptors[] = {
    [0] = "\x09\x04",  // English (US)
    [1] = mfr_string,
    [2] = prod_string,
    [3] = "000001",
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    if (instance == ITF_NUM_HID_MOUSE) return hid_mouse_report_desc;
    if (instance == ITF_NUM_HID_CONSUMER) return hid_consumer_report_desc;
    return hid_keyboard_report_desc;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

static bool badusb_send_key(uint8_t modifiers, uint8_t keycode, void *ctx) {
    (void)ctx;
    if (s_stop_requested) return false;

    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};

    int timeout = 100;
    while (!tud_hid_n_ready(ITF_NUM_HID_KEYBOARD) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(ITF_NUM_HID_KEYBOARD)) return false;

    tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, modifiers, keycodes);
    vTaskDelay(pdMS_TO_TICKS(MIN_KEY_DELAY_MS));
    return true;
}

static bool badusb_send_string(const char *text, size_t len, void *ctx) {
    (void)ctx;
    if (s_stop_requested) return false;

    for (size_t i = 0; i < len; i++) {
        if (s_stop_requested) return false;

        uint8_t keycode, modifier;
        if (!hid_ascii_to_keycode(text[i], &keycode, &modifier)) {
            continue;  // Skip unmappable characters
        }

        if (!badusb_send_key(modifier, keycode, ctx)) {
            return false;
        }
        tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, 0, NULL);
        vTaskDelay(pdMS_TO_TICKS(MIN_KEY_DELAY_MS));
    }
    return true;
}

static void badusb_delay(uint32_t ms, void *ctx) {
    (void)ctx;
    while (ms > 0 && !s_stop_requested) {
        uint32_t chunk = (ms > 100) ? 100 : ms;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
}

static bool badusb_release_keys(void *ctx) {
    (void)ctx;
    if (!tud_hid_n_ready(ITF_NUM_HID_KEYBOARD)) return false;
    tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(MIN_KEY_DELAY_MS));
    return true;
}

static bool badusb_transport_cancelled(void *ctx) {
    (void)ctx;
    return s_stop_requested;
}

static const hid_transport_t usb_transport = {
    .send_key     = badusb_send_key,
    .send_string  = badusb_send_string,
    .delay        = badusb_delay,
    .release_keys = badusb_release_keys,
    .is_cancelled = badusb_transport_cancelled,
    .ctx          = NULL,
};

// --- Forward declarations ---
static esp_err_t badusb_install_driver(void);
static void badusb_uninstall_driver(void);
static esp_err_t badusb_wait_for_mount(void);
static void keyboard_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data);
static volatile bool s_keyboard_mode = false;

static esp_err_t badusb_wait_for_vbus(const char *status_after_connect) {
    if (badusb_has_vsense() && !badusb_vsense_connected()) {
        ESP_LOGI(TAG, "Waiting for VBUS...");
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status waiting");
        }
        while (!badusb_vsense_connected() && !s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_stop_requested) return ESP_ERR_INVALID_STATE;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (status_after_connect && esp_comm_manager_is_connected()) {
        char status[32];
        snprintf(status, sizeof(status), "status %s", status_after_connect);
        esp_comm_manager_send_command("badusb", status);
    }
    return ESP_OK;
}

// --- Mouse HID ---

bool badusb_hid_mouse_send(int8_t dx, int8_t dy, uint8_t buttons) {
    if (!s_active) return false;
    int timeout = 100;
    while (!tud_hid_n_ready(HID_INSTANCE_MOUSE) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(HID_INSTANCE_MOUSE)) return false;

    // Descriptor is boot mouse: buttons, X, Y, wheel. Send exactly 4 bytes.
    uint8_t report[4] = {buttons, (uint8_t)dx, (uint8_t)dy, 0};
    return tud_hid_n_report(HID_INSTANCE_MOUSE, 0, report, sizeof(report));
}

bool badusb_hid_mouse_wheel_send(int8_t wheel, uint8_t buttons) {
    if (!s_active) return false;
    int timeout = 100;
    while (!tud_hid_n_ready(HID_INSTANCE_MOUSE) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(HID_INSTANCE_MOUSE)) return false;

    // Wheel-only report keeps dx/dy at zero so the cursor doesn't drift.
    uint8_t report[4] = {buttons, 0, 0, (uint8_t)wheel};
    return tud_hid_n_report(HID_INSTANCE_MOUSE, 0, report, sizeof(report));
}

bool badusb_manager_send_consumer(uint8_t control_mask) {
    if (!s_active || control_mask == 0) return false;

    int timeout = 100;
    while (!tud_hid_n_ready(HID_INSTANCE_CONSUMER) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(HID_INSTANCE_CONSUMER)) return false;

    uint8_t report = control_mask;
    uint8_t release = 0;
    if (!tud_hid_n_report(HID_INSTANCE_CONSUMER, 0, &report, 1)) return false;

    /* Wait for the host to consume the press before submitting its release.
     * The consumer endpoint polls every 2 ms, so this is both faster and more
     * reliable than a fixed keyboard-oriented 10 ms delay. */
    timeout = 100;
    while (!tud_hid_n_ready(HID_INSTANCE_CONSUMER) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(HID_INSTANCE_CONSUMER)) return false;
    return tud_hid_n_report(HID_INSTANCE_CONSUMER, 0, &release, 1);
}

// --- Mouse Jiggler ---

static TaskHandle_t s_jiggler_task = NULL;
static TaskHandle_t s_mode_start_task = NULL;
static volatile bool s_jiggler_stop = false;
static volatile bool s_trackpad_active = false;
static volatile uint8_t s_trackpad_buttons = 0;

static void mouse_jiggler_task(void *arg) {
    (void)arg;
    s_jiggler_stop = false;
    int8_t dx = 8;

    glog("BadUSB: Mouse jiggler started\n");
    if (esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("badusb", "status jiggling");
    }

    while (!s_jiggler_stop) {
        badusb_hid_mouse_send(dx, 0, 0);
        dx = -dx;  // Alternate direction each tick
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    glog("BadUSB: Mouse jiggler stopped\n");
    if (esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("badusb", "status done");
    }
    s_jiggler_task = NULL;
    vTaskDelete(NULL);
}

typedef enum {
    BADUSB_START_JIGGLER = 1,
    BADUSB_START_KEYBOARD = 2,
    BADUSB_START_TRACKPAD = 3,
    BADUSB_START_CONSUMER = 4,
} badusb_start_mode_t;

static volatile bool s_consumer_mode = false;

static void badusb_mode_start_task(void *arg) {
    badusb_start_mode_t mode = (badusb_start_mode_t)(uintptr_t)arg;

    s_stop_requested = false;
    esp_err_t ret = badusb_wait_for_vbus(NULL);
    if (ret == ESP_OK) {
        badusb_manager_apply_settings();
        ret = badusb_install_driver();
    }
    if (ret == ESP_OK) {
        s_active = true;
        /* The HID endpoints can be used while the host is still enumerating;
         * expose the mode immediately so the UI does not discard early knob
         * presses during the mount wait. */
        if (mode == BADUSB_START_CONSUMER) {
            s_consumer_mode = true;
        }
        ret = badusb_wait_for_mount();
    }

    if (ret == ESP_OK && mode == BADUSB_START_JIGGLER) {
        if (xTaskCreate(mouse_jiggler_task, "mouse_jiggle", 4096, NULL, 5, &s_jiggler_task) != pdPASS) {
            ret = ESP_FAIL;
        }
    } else if (ret == ESP_OK && mode == BADUSB_START_KEYBOARD) {
        s_keyboard_mode = true;
        esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_KEYBOARD, keyboard_stream_rx_cb, NULL);
        glog("BadUSB: Keyboard mode started\n");
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status keyboard");
        }
    } else if (ret == ESP_OK && mode == BADUSB_START_TRACKPAD) {
        s_trackpad_buttons = 0;
        s_trackpad_active = true;
        glog("BadUSB: Trackpad mode started\n");
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status trackpad");
        }
    } else if (ret == ESP_OK && mode == BADUSB_START_CONSUMER) {
        glog("BadUSB: Consumer Control mode started\n");
    }

    if (ret != ESP_OK) {
        glog("BadUSB: Failed to start mode: %s\n", esp_err_to_name(ret));
        badusb_uninstall_driver();
        s_active = false;
        s_keyboard_mode = false;
        s_trackpad_active = false;
        s_trackpad_buttons = 0;
        s_consumer_mode = false;
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status done");
        }
    }

    s_mode_start_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t badusb_manager_mouse_jiggle_start(void) {
    if (s_mode_start_task) return ESP_ERR_INVALID_STATE;
    if (s_jiggler_task) return ESP_ERR_INVALID_STATE;
    if (s_active) return ESP_ERR_INVALID_STATE;

    if (xTaskCreate(badusb_mode_start_task, "badusb_mode", 6144,
                    (void *)(uintptr_t)BADUSB_START_JIGGLER, 5, &s_mode_start_task) != pdPASS) {
        s_mode_start_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t badusb_manager_mouse_jiggle_stop(void) {
    s_stop_requested = true;
    s_jiggler_stop = true;
    for (int i = 0; i < 100 && s_mode_start_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    // Give the task time to exit
    for (int i = 0; i < 50 && s_jiggler_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_driver_installed) {
        badusb_uninstall_driver();
    }
    s_active = false;
    return ESP_OK;
}

bool badusb_manager_is_jiggling(void) {
    return s_jiggler_task != NULL;
}

// --- Trackpad Mode (remote/local mouse HID control) ---

esp_err_t badusb_manager_trackpad_start(void) {
    if (s_mode_start_task) return ESP_ERR_INVALID_STATE;
    if (s_trackpad_active) return ESP_OK;

    if (xTaskCreate(badusb_mode_start_task, "badusb_mode", 6144,
                    (void *)(uintptr_t)BADUSB_START_TRACKPAD, 5, &s_mode_start_task) != pdPASS) {
        s_mode_start_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t badusb_manager_trackpad_stop(void) {
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_mode_start_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_trackpad_active = false;
    s_trackpad_buttons = 0;
    if (!s_keyboard_mode && !s_jiggler_task && s_driver_installed) {
        badusb_uninstall_driver();
        s_active = false;
    }
    glog("BadUSB: Trackpad mode stopped\n");
    if (esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("badusb", "status done");
    }
    return ESP_OK;
}

esp_err_t badusb_manager_consumer_start(void) {
    if (s_mode_start_task || s_active) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(badusb_mode_start_task, "badusb_mode", 6144,
                    (void *)(uintptr_t)BADUSB_START_CONSUMER, 5, &s_mode_start_task) != pdPASS) {
        s_mode_start_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t badusb_manager_consumer_stop(void) {
    s_stop_requested = true;
    s_consumer_mode = false;
    for (int i = 0; i < 100 && s_mode_start_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_driver_installed) badusb_uninstall_driver();
    s_active = false;
    return ESP_OK;
}

bool badusb_manager_is_consumer_mode(void) {
    return s_consumer_mode;
}

bool badusb_manager_is_trackpad(void) {
    return s_trackpad_active;
}

void badusb_manager_trackpad_move(int dx, int dy) {
    if (!s_trackpad_active) return;
    // The HID boot mouse report is 1 byte per axis, so saturate each axis to
    // int8. Excess magnitude is dropped - the caller is expected to chunk
    // large drags into successive reports if needed.
    if (dx > 127) dx = 127;
    if (dx < -128) dx = -128;
    if (dy > 127) dy = 127;
    if (dy < -128) dy = -128;
    badusb_hid_mouse_send((int8_t)dx, (int8_t)dy, s_trackpad_buttons);
}

void badusb_manager_trackpad_button(uint8_t buttons) {
    if (!s_trackpad_active) return;
    s_trackpad_buttons = buttons & 0x07;  // Boot mouse: 3 buttons
    // Emit a zero-delta report with the new button state so the host sees
    // the press/release immediately, even if the cursor hasn't moved.
    badusb_hid_mouse_send(0, 0, s_trackpad_buttons);
}

void badusb_manager_trackpad_wheel(int delta) {
    if (!s_trackpad_active) return;
    // Boot-mouse wheel byte is 8-bit signed (range -128..+127).
    if (delta > 127) delta = 127;
    if (delta < -128) delta = -128;
    badusb_hid_mouse_wheel_send((int8_t)delta, s_trackpad_buttons);
}

// --- Keyboard Mode (real-time key forwarding) ---

static void keyboard_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;
    if (!s_keyboard_mode || !data || length < 3) return;

    // Payload: [flags] [modifier] [keycode]
    // flags bit 0 = release event
    bool is_release = (data[0] & 0x01) != 0;
    uint8_t modifier = data[1];
    uint8_t keycode = data[2];

    if (is_release) {
        tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, 0, NULL);
    } else {
        uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
        tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, modifier, keycodes);
    }
}

esp_err_t badusb_manager_keyboard_mode_start(void) {
    if (s_mode_start_task) return ESP_ERR_INVALID_STATE;
    if (s_active || s_keyboard_mode) return ESP_ERR_INVALID_STATE;

    if (xTaskCreate(badusb_mode_start_task, "badusb_mode", 6144,
                    (void *)(uintptr_t)BADUSB_START_KEYBOARD, 5, &s_mode_start_task) != pdPASS) {
        s_mode_start_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t badusb_manager_keyboard_mode_stop(void) {
    s_stop_requested = true;
    for (int i = 0; i < 100 && s_mode_start_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_keyboard_mode = false;
    if (s_driver_installed) {
        badusb_uninstall_driver();
    }
    s_active = false;
    glog("BadUSB: Keyboard mode stopped\n");
    if (esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("badusb", "status done");
    }
    return ESP_OK;
}

bool badusb_manager_is_keyboard_mode(void) {
    return s_keyboard_mode;
}

bool badusb_manager_send_keypress(uint8_t modifier, uint8_t keycode) {
    if (!s_active) return false;
    int timeout = 100;
    while (!tud_hid_n_ready(ITF_NUM_HID_KEYBOARD) && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!tud_hid_n_ready(ITF_NUM_HID_KEYBOARD)) return false;

    uint8_t keycodes[6] = {keycode, 0, 0, 0, 0, 0};
    tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, modifier, keycodes);
    vTaskDelay(pdMS_TO_TICKS(MIN_KEY_DELAY_MS));
    tud_hid_n_keyboard_report(ITF_NUM_HID_KEYBOARD, 0, 0, NULL);
    return true;
}

bool badusb_manager_send_text(const char *text) {
    if (!text) return false;
    for (int i = 0; i < 300 && !s_active; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_active) return false;
    size_t len = strlen(text);
    if (len == 0) return true;
    bool ok = badusb_send_string(text, len, NULL);
    badusb_release_keys(NULL);
    return ok;
}

esp_err_t badusb_manager_init(void) {
    if (s_initialized) return ESP_OK;
    s_initialized = true;

#if defined(CONFIG_BADUSB_VSENSE_PIN) && CONFIG_BADUSB_VSENSE_PIN >= 0
    gpio_config_t vsense_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_BADUSB_VSENSE_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vsense_cfg);
    ESP_LOGI(TAG, "VSENSE pin configured on GPIO%d (level=%d)",
             CONFIG_BADUSB_VSENSE_PIN,
             gpio_get_level(CONFIG_BADUSB_VSENSE_PIN));
#endif

    ESP_LOGI(TAG, "BadUSB manager initialized");
    return ESP_OK;
}

bool badusb_has_vsense(void) {
#if defined(CONFIG_BADUSB_VSENSE_PIN) && CONFIG_BADUSB_VSENSE_PIN >= 0
    return true;
#else
    return false;
#endif
}

bool badusb_vsense_connected(void) {
#if defined(CONFIG_BADUSB_VSENSE_PIN) && CONFIG_BADUSB_VSENSE_PIN >= 0
    return gpio_get_level(CONFIG_BADUSB_VSENSE_PIN) != 0;
#else
    return true;
#endif
}

static void badusb_randomize_details(uint16_t *vid, uint16_t *pid, char *mfr, size_t mfr_len, char *prod, size_t prod_len) {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    if (vid) *vid = (uint16_t)(0x1000 + (r1 % 0xEFFF));
    if (pid) *pid = (uint16_t)(0x0001 + (r2 % 0xFFFE));
    if (mfr && mfr_len > 0 && vid) snprintf(mfr, mfr_len, "USB-%04X", *vid);
    if (prod && prod_len > 0 && pid) snprintf(prod, prod_len, "HID-%04X", *pid);
}

void badusb_manager_apply_settings(void) {
    uint16_t vid = settings_get_badusb_vid(&G_Settings);
    uint16_t pid = settings_get_badusb_pid(&G_Settings);
    bool randomize = settings_get_badusb_randomize(&G_Settings);

    if (randomize) {
        badusb_randomize_details(&vid, &pid, mfr_string, sizeof(mfr_string), prod_string, sizeof(prod_string));
    } else {
        strncpy(mfr_string, settings_get_badusb_manufacturer(&G_Settings), sizeof(mfr_string) - 1);
        mfr_string[sizeof(mfr_string) - 1] = '\0';
        strncpy(prod_string, settings_get_badusb_product(&G_Settings), sizeof(prod_string) - 1);
        prod_string[sizeof(prod_string) - 1] = '\0';
    }

    device_descriptor.idVendor = vid;
    device_descriptor.idProduct = pid;
    hid_set_keyboard_layout(settings_get_badusb_kb_layout(&G_Settings));
    ESP_LOGI(TAG, "Applied settings: VID=0x%04X PID=0x%04X Mfr=%s Prod=%s Layout=%u",
             device_descriptor.idVendor, device_descriptor.idProduct, mfr_string, prod_string,
             settings_get_badusb_kb_layout(&G_Settings));
}

// Install TinyUSB driver (does not wait for mount).
// Caller must call badusb_manager_apply_settings() first.
static esp_err_t badusb_install_driver(void) {
    if (s_driver_installed) {
        tinyusb_driver_uninstall();
        s_driver_installed = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &device_descriptor;
    tusb_cfg.descriptor.full_speed_config = configuration_descriptor;
    tusb_cfg.descriptor.string = string_descriptors;
    tusb_cfg.descriptor.string_count = sizeof(string_descriptors) / sizeof(string_descriptors[0]);

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TinyUSB driver: %s", esp_err_to_name(ret));
        return ret;
    }
    s_driver_installed = true;
    return ESP_OK;
}

// Stop TinyUSB and hand the USB peripheral back to the native
// USB-Serial-JTAG console driver (S3/C3/C5/C6). The restore is a no-op on
// targets without the native peripheral or when it is already installed.
static void badusb_uninstall_driver(void) {
    if (!s_driver_installed) {
        return;
    }
    tinyusb_driver_uninstall();
    s_driver_installed = false;
    // Give the USB host time to finish its disconnect handshake before the
    // native console driver takes over the bus.
    vTaskDelay(pdMS_TO_TICKS(50));
    serial_manager_restore_console();
}

// Wait for USB host to mount the device
static esp_err_t badusb_wait_for_mount(void) {
    int timeout = 500;  // 5 seconds
    while (!tud_mounted() && timeout-- > 0 && !s_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_stop_requested) return ESP_ERR_INVALID_STATE;

    if (!tud_mounted()) {
        ESP_LOGW(TAG, "USB not mounted after timeout, continuing anyway");
    } else {
        ESP_LOGI(TAG, "USB device mounted");
    }
    return ESP_OK;
}

esp_err_t badusb_manager_start(void) {
    if (s_active) return ESP_OK;

    badusb_manager_apply_settings();
    esp_err_t ret = badusb_install_driver();
    if (ret != ESP_OK) return ret;

    s_active = true;
    s_stop_requested = false;

    return badusb_wait_for_mount();
}

esp_err_t badusb_manager_stop(void) {
    s_stop_requested = true;
    s_keyboard_mode = false;
    s_jiggler_stop = true;
    s_trackpad_active = false;
    s_trackpad_buttons = 0;
    s_consumer_mode = false;
    s_active = false;
    if (s_driver_installed) badusb_uninstall_driver();
    ESP_LOGI(TAG, "BadUSB stopped");
    return ESP_OK;
}

static TaskHandle_t s_exec_task_handle = NULL;

typedef struct {
    char *buf;
    char path[256];
    bool from_file;
    size_t buf_len;
} exec_task_params_t;

static void badusb_exec_task(void *arg) {
    exec_task_params_t *params = (exec_task_params_t *)arg;

    // If VSENSE is available, wait for USB cable to be plugged in BEFORE
    // installing TinyUSB.  The ESP32-S3 internal PHY needs VBUS present for
    // the device stack to enumerate correctly.
    if (badusb_has_vsense() && !badusb_vsense_connected()) {
        ESP_LOGI(TAG, "Waiting for VBUS...");
        // Notify peer (C5) that we're waiting for USB
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status waiting");
        }
        while (!badusb_vsense_connected() && !s_stop_requested) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (s_stop_requested) {
            glog("BadUSB: Cancelled while waiting for USB\n");
            if (!params->from_file && params->buf) free(params->buf);
            free(params);
            s_exec_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "VBUS detected, letting connection settle...");
        // Let VBUS and data lines stabilise before touching the USB stack
        vTaskDelay(pdMS_TO_TICKS(500));
        // Notify peer (C5) that USB is connected
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status running");
        }
    }

    // Now install TinyUSB and wait for host enumeration
    esp_err_t ret = badusb_manager_start();
    if (ret != ESP_OK) {
        glog("BadUSB: Failed to start: %s\n", esp_err_to_name(ret));
        if (esp_comm_manager_is_connected()) {
            esp_comm_manager_send_command("badusb", "status done");
        }
        if (!params->from_file && params->buf) free(params->buf);
        free(params);
        s_exec_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    glog("BadUSB: USB ready, mounted=%d\n", tud_mounted() ? 1 : 0);

    int lines = 0;

    if (params->from_file) {
        FILE *f = fopen(params->path, "r");
        if (!f) {
            glog("BadUSB: Failed to open script: %s\n", params->path);
        } else {
            glog("BadUSB: Executing %s\n", params->path);
            lines = hid_script_execute_file(f, &usb_transport);
            fclose(f);
        }
    } else {
        params->buf[params->buf_len] = '\0';
        glog("BadUSB: Executing remote script (%zu bytes)\n", params->buf_len);
        lines = hid_script_execute(params->buf, &usb_transport);
        free(params->buf);
    }

    if (s_stop_requested) {
        glog("BadUSB: Execution stopped by user\n");
    } else {
        glog("BadUSB: Done (%d lines)\n", lines);
    }

    // Notify peer (C5) that execution is done
    if (esp_comm_manager_is_connected()) {
        esp_comm_manager_send_command("badusb", "status done");
    }

    badusb_uninstall_driver();

    s_active = false;
    free(params);
    s_exec_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t badusb_manager_execute_file(const char *path) {
    if (!path) return ESP_ERR_INVALID_ARG;
    if (s_exec_task_handle) {
        glog("BadUSB: Already executing a script\n");
        return ESP_ERR_INVALID_STATE;
    }

    exec_task_params_t *params = calloc(1, sizeof(exec_task_params_t));
    if (!params) return ESP_ERR_NO_MEM;
    params->from_file = true;
    strncpy(params->path, path, sizeof(params->path) - 1);

    s_stop_requested = false;

    if (xTaskCreate(badusb_exec_task, "badusb_exec", 8192, params, 5, &s_exec_task_handle) != pdPASS) {
        free(params);
        return ESP_FAIL;
    }
    ghostchi_manager_add_xp(8);

    return ESP_OK;
}

bool badusb_manager_is_active(void) {
    return s_active;
}

bool badusb_manager_is_usb_mounted(void) {
    return s_active && tud_mounted();
}

int badusb_manager_list_scripts(char scripts[][64], int max_scripts) {
    int count = 0;

    if (count < max_scripts) {
        strncpy(scripts[count], BADUSB_BUILTIN_SCRIPT_NAME, 63);
        scripts[count][63] = '\0';
        count++;
    }

    const char *dir_path = "/mnt/ghostesp/badusb";
    DIR *dir = opendir(dir_path);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL && count < max_scripts) {
            size_t len = strlen(entry->d_name);
            if (len > 4 && strcmp(entry->d_name + len - 4, ".txt") == 0) {
                strncpy(scripts[count], entry->d_name, 63);
                scripts[count][63] = '\0';
                count++;
            }
        }
        closedir(dir);
    }
    return count;
}

static char *s_script_buf = NULL;
static size_t s_script_size = 0;
static size_t s_script_offset = 0;

esp_err_t badusb_manager_prepare_receive(size_t size) {
    bool remote_request = esp_comm_manager_is_remote_command();

    if (s_script_buf) {
        free(s_script_buf);
        s_script_buf = NULL;
    }

    if (size == 0 || size > 65536) {
        if (!remote_request) glog("BadUSB: Invalid script size: %zu\n", size);
        return ESP_ERR_INVALID_ARG;
    }

    s_script_buf = malloc(size + 1);
    if (!s_script_buf) {
        if (!remote_request) glog("BadUSB: Failed to allocate %zu bytes for script\n", size + 1);
        return ESP_ERR_NO_MEM;
    }

    s_script_size = size;
    s_script_offset = 0;
    if (!remote_request) glog("BadUSB: Ready to receive %zu byte script\n", size);
    return ESP_OK;
}

esp_err_t badusb_manager_execute_buffer(char *buf, size_t len) {
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;
    if (s_exec_task_handle) {
        glog("BadUSB: Already executing a script\n");
        free(buf);
        return ESP_ERR_INVALID_STATE;
    }

    exec_task_params_t *params = calloc(1, sizeof(exec_task_params_t));
    if (!params) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    params->from_file = false;
    params->buf = buf;
    params->buf_len = len;

    s_stop_requested = false;

    if (xTaskCreate(badusb_exec_task, "badusb_exec", 8192, params, 5, &s_exec_task_handle) != pdPASS) {
        free(buf);
        free(params);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void badusb_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *user_data) {
    (void)channel;
    (void)user_data;

    if (!s_script_buf || !data || length == 0) return;

    size_t remaining = s_script_size - s_script_offset;
    size_t to_copy = (length < remaining) ? length : remaining;

    memcpy(s_script_buf + s_script_offset, data, to_copy);
    s_script_offset += to_copy;

    if (s_script_offset >= s_script_size) {
        char *buf = s_script_buf;
        size_t size = s_script_size;
        s_script_buf = NULL;
        s_script_size = 0;
        s_script_offset = 0;
        badusb_manager_execute_buffer(buf, size);
    }
}

void badusb_manager_register_stream_handler(void) {
    bool ok = esp_comm_manager_register_stream_handler(
        COMM_STREAM_CHANNEL_BADUSB, badusb_stream_rx_cb, NULL);
    ESP_LOGI(TAG, "BadUSB stream handler: %s", ok ? "OK" : "FAIL");
}

#endif // CONFIG_HAS_BADUSB
