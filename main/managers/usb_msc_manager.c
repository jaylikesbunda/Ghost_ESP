#include "sdkconfig.h"

#ifdef CONFIG_HAS_USB_MSC_SD

// The TinyUSB MSC class and its buffer-size symbol must stay enabled together
// with this feature (HAS_USB_MSC_SD selects TINYUSB_MSC_ENABLED in
// main/Kconfig.projbuild). Fail here with a clear message instead of deep
// inside the TinyUSB headers if a board config ever drops it again.
#ifndef CONFIG_TINYUSB_MSC_ENABLED
#define CONFIG_TINYUSB_MSC_ENABLED 0
#endif
#if !CONFIG_TINYUSB_MSC_ENABLED
#error "CONFIG_HAS_USB_MSC_SD requires CONFIG_TINYUSB_MSC_ENABLED"
#endif

#include "managers/usb_msc_manager.h"
#include "managers/sd_card_manager.h"
#include "managers/usb_keyboard_manager.h"
#include "managers/badusb_manager.h"
#include "core/glog.h"
#include "core/serial_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/msc/msc_device.h"
#include "tinyusb_msc.h"
#include <string.h>

static const char *TAG = "usb_msc";

static bool s_active = false;
static tinyusb_msc_storage_handle_t s_storage_handle = NULL;

static const tusb_desc_device_t msc_device_descriptor = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x1209,
    .idProduct          = 0x0002,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN  0x81

// Full-speed bulk endpoints are limited to 64-byte wMaxPacketSize (the S3 has
// no high-speed PHY). CFG_TUD_MSC_EP_BUFSIZE is only the internal FIFO size
// and must NOT be used as the endpoint size here.
#define EPNUM_MSC_SIZE 64

#define MSC_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t msc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, MSC_CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, EPNUM_MSC_SIZE),
};

static const char *msc_string_descriptors[] = {
    [0] = "\x09\x04",  // English (US)
    [1] = "Ghost ESP",
    [2] = "Ghost ESP SD",
    [3] = "000001",
};

static void msc_storage_event_cb(tinyusb_msc_storage_handle_t handle,
                                 tinyusb_msc_event_t *event, void *arg) {
    (void)handle;
    (void)arg;
    switch (event->id) {
        case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
            if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
                glog("USB SD: host ejected the card (run 'usbsd off' to exit)\n");
            } else {
                glog("USB SD: host mounted the card\n");
            }
            break;
        case TINYUSB_MSC_EVENT_MOUNT_FAILED:
            glog("USB SD: storage mount failed\n");
            break;
        default:
            break;
    }
}

// GPIO19/20 are the S3 native USB D-/D+ lines; the SD card must not be wired
// to them or passthrough would fight the USB PHY. Only the pins of the build's
// active SD mode are checked — the other pin set keeps its struct defaults and
// is not wired at all.
static bool sd_pins_conflict_with_usb(void) {
#if defined(CONFIG_USING_SPI)
    const int pins[] = {
        sd_card_manager.spi_cs_pin, sd_card_manager.spi_clk_pin,
        sd_card_manager.spi_miso_pin, sd_card_manager.spi_mosi_pin,
    };
#elif defined(CONFIG_USING_MMC_1_BIT)
    const int pins[] = {
        CONFIG_SD_MMC_CLK, CONFIG_SD_MMC_CMD, CONFIG_SD_MMC_D0,
    };
#elif defined(CONFIG_USING_MMC)
    const int pins[] = {
        sd_card_manager.clkpin, sd_card_manager.cmdpin,
        sd_card_manager.d0pin, sd_card_manager.d1pin,
        sd_card_manager.d2pin, sd_card_manager.d3pin,
    };
#else
    const int pins[] = { -1 };
#endif
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] == 19 || pins[i] == 20) {
            return true;
        }
    }
    return false;
}

static esp_err_t usb_msc_install(void) {
    const tinyusb_msc_driver_config_t driver_cfg = {
        .user_flags.auto_mount_off = 1,  // firmware controls remounting, not host connect/disconnect
        .callback = msc_storage_event_cb,
        .callback_arg = NULL,
    };
    esp_err_t ret = tinyusb_msc_install_driver(&driver_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_install_driver failed: %s", esp_err_to_name(ret));
        return ret;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.card = sd_card_manager.card,
        .fat_fs.base_path = NULL,
        .fat_fs.config = {
            .format_if_mount_failed = false,
            .disk_status_check_enable = false,
            .max_files = 3,
            .allocation_unit_size = 16 * 1024,
        },
        .fat_fs.do_not_format = true,
        .fat_fs.format_flags = 0,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ret = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_msc_new_storage_sdmmc failed: %s", esp_err_to_name(ret));
        tinyusb_msc_uninstall_driver();
        return ret;
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &msc_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = msc_configuration_descriptor;
    tusb_cfg.descriptor.string = msc_string_descriptors;
    tusb_cfg.descriptor.string_count = sizeof(msc_string_descriptors) / sizeof(msc_string_descriptors[0]);

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(ret));
        tinyusb_msc_delete_storage(s_storage_handle);
        s_storage_handle = NULL;
        tinyusb_msc_uninstall_driver();
        return ret;
    }
    return ESP_OK;
}

static void usb_msc_teardown(void) {
    if (s_storage_handle) {
        tinyusb_msc_delete_storage(s_storage_handle);
        s_storage_handle = NULL;
    }
    tinyusb_msc_uninstall_driver();
    tinyusb_driver_uninstall();
    vTaskDelay(pdMS_TO_TICKS(50));
    serial_manager_restore_console();
}

esp_err_t usb_msc_start(void) {
    if (s_active) {
        glog("USB SD passthrough already active\n");
        return ESP_ERR_INVALID_STATE;
    }

#if defined(CONFIG_HAS_BADUSB)
    if (badusb_manager_is_active()) {
        glog("USB SD: stop BadUSB first\n");
        return ESP_ERR_INVALID_STATE;
    }
#endif
#ifdef CONFIG_USE_USB_KEYBOARD
    if (usb_keyboard_manager_is_host_mode()) {
        glog("USB SD: disable USB host keyboard mode first\n");
        return ESP_ERR_INVALID_STATE;
    }
#endif
    if (sd_pins_conflict_with_usb()) {
        glog("USB SD: board wires the SD card to GPIO19/20 (USB PHY), unsupported\n");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!sd_card_manager.is_initialized || sd_card_manager.card == NULL) {
        glog("USB SD: no SD card mounted\n");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = sd_card_suspend_for_usb_msc(NULL);
    if (ret != ESP_OK) {
        glog("USB SD: failed to claim SD card: %s\n", esp_err_to_name(ret));
        return ret;
    }

    ret = usb_msc_install();
    if (ret != ESP_OK) {
        glog("USB SD: TinyUSB install failed: %s\n", esp_err_to_name(ret));
        sd_card_resume_from_usb_msc();
        return ret;
    }

    s_active = true;

    int timeout = 500;  // 5 s
    while (!tud_mounted() && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!tud_mounted()) {
        glog("USB SD: waiting for USB host to enumerate the card...\n");
    } else {
        glog("USB SD: passthrough active — card is now a USB drive\n");
    }
    return ESP_OK;
}

esp_err_t usb_msc_stop(void) {
    if (!s_active) {
        glog("USB SD passthrough is not active\n");
        return ESP_ERR_INVALID_STATE;
    }

    usb_msc_teardown();
    s_active = false;

    esp_err_t ret = sd_card_resume_from_usb_msc();
    if (ret != ESP_OK) {
        glog("USB SD: card remount failed: %s (power cycle will restore it)\n", esp_err_to_name(ret));
        return ret;
    }
    glog("USB SD: passthrough stopped, card remounted\n");
    return ESP_OK;
}

static void usb_msc_async_task(void *arg) {
    (void)arg;
    usb_msc_start();
    vTaskDelete(NULL);
}

esp_err_t usb_msc_start_async(void) {
    if (s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(usb_msc_async_task, "usb_msc_start", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool usb_msc_is_active(void) {
    return s_active;
}

#endif // CONFIG_HAS_USB_MSC_SD
