#include "gui/menu_catalog.h"
#include "managers/settings_manager.h"
#include "core/esp_comm_manager.h"
#include "gui/toast.h"
#include "managers/haptic_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/app_gallery_screen.h"
#include "managers/views/lockscreen.h"
#include "managers/views/ethernet_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/sd_browser_screen.h"
#if CONFIG_CROWPANEL_EPAPER_42
#include "managers/views/book_reader_view.h"
#endif
#ifdef CONFIG_WITH_SCREEN
#include "managers/views/ghostchi_screen.h"
#endif
#include "managers/views/clock_screen.h"
#include "managers/views/cloud_store_screen.h"
#include "managers/views/plugin_runner_view.h"
#if CONFIG_ENABLE_GHOSTSCRIPT
#include "managers/views/ghostscript_browser_view.h"
#endif
#if CONFIG_HAS_INFRARED
#include "managers/views/infrared_view.h"
#endif
#ifdef CONFIG_HAS_NFC
#include "managers/views/nfc_view.h"
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
#include "managers/views/subghz_view.h"
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
#include "managers/views/badusb_view.h"
#endif
#ifdef CONFIG_HAS_BADBLE
#include "managers/views/badble_view.h"
#endif
#if defined(CONFIG_CROWPANEL_1P28_ROTARY) && defined(CONFIG_HAS_BADUSB)
#include "managers/views/crowpanel_audio_view.h"
#endif
#ifdef CONFIG_HAS_AUDIO_PLAYER
#include "managers/views/audio_player_screen.h"
#endif
#ifdef CONFIG_HAS_COMPASS
#include "managers/views/compass_screen.h"
#endif
#ifdef CONFIG_HAS_ENVIII
#include "managers/views/enviii_screen.h"
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
#include "managers/views/accelerometer_screen.h"
#endif
#ifdef CONFIG_CROWPANEL_P4_CAMERA
#include "managers/views/crowpanel_p4_camera_view.h"
#endif
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

LV_IMG_DECLARE(dualcomm);
LV_IMG_DECLARE(lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
LV_IMG_DECLARE(nrf24);
LV_IMG_DECLARE(subghz);
LV_IMG_DECLARE(lock);
LV_IMG_DECLARE(rave);
LV_IMG_DECLARE(speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48);
LV_IMG_DECLARE(folder);
LV_IMG_DECLARE(description);
LV_IMG_DECLARE(storefront);
LV_IMG_DECLARE(enviii);
LV_IMG_DECLARE(accelerometer_icon);

#define ITEM(key, label, asset, image, target, type, place) \
    {.id = key, .name = label, .asset_key = asset, .icon = &image, \
     .view = &target, .options_type = type, .default_placement = place}

/* Keep historical defaults in order; adding a new item never renumbers IDs. */
static const menu_catalog_item_t builtin_items[] = {
#if CONFIG_CROWPANEL_EPAPER_42
    /* The e-paper board is primarily a reading device. Keep Reader as the
     * first main-menu entry so it is reachable immediately after boot. */
    ITEM("book_reader", "Reader", "description", description, book_reader_view, 0, MENU_PLACE_MAIN),
#endif
    ITEM("wifi", "WiFi", "wifi", wifi, options_menu_view, OT_Wifi, MENU_PLACE_MAIN),
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    ITEM("ble", "BLE", "bluetooth", bluetooth, options_menu_view, OT_Bluetooth, MENU_PLACE_MAIN),
#endif
    ITEM("gps", "GPS", "Map", Map, options_menu_view, OT_GPS, MENU_PLACE_MAIN),
#if CONFIG_HAS_INFRARED
    ITEM("infrared", "Infrared", "infrared", infrared, infrared_view, 0, MENU_PLACE_MAIN),
#endif
#ifdef CONFIG_HAS_NFC
    ITEM("nfc", "NFC", "nfc_icon", nfc_icon, nfc_view, 0, MENU_PLACE_MAIN),
#endif
#if defined(CONFIG_HAS_NRF24) || defined(CONFIG_HAS_NRF24_REMOTE)
    ITEM("nrf24", "NRF24", "nrf24", nrf24, options_menu_view, OT_NRF24, MENU_PLACE_MAIN),
#endif
#if defined(CONFIG_HAS_SUBGHZ) || defined(CONFIG_HAS_SUBGHZ_REMOTE)
    ITEM("subghz", "SubGHz", "subghz", subghz, subghz_view, 0, MENU_PLACE_MAIN),
#endif
#if defined(CONFIG_HAS_BADUSB) || defined(CONFIG_HAS_BADUSB_REMOTE)
    ITEM("badusb", "BadUSB", "usb", usb, badusb_view, 0, MENU_PLACE_MAIN),
#endif
#ifdef CONFIG_HAS_BADBLE
    ITEM("badble", "BadBLE", "bluetooth", bluetooth, badble_view, 0, MENU_PLACE_MAIN),
#endif
#if defined(CONFIG_CROWPANEL_1P28_ROTARY) && defined(CONFIG_HAS_BADUSB)
    /* The rotary board's primary job is audio control; keep it on the
     * circular home gallery instead of hiding it behind Apps. */
    ITEM("audio_master", "USB Audio", "speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48", speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48, crowpanel_audio_view, 0, MENU_PLACE_MAIN),
#endif
    ITEM("ghostlink", "GhostLink", "dualcomm", dualcomm, options_menu_view, OT_DualComm, MENU_PLACE_MAIN),
    ITEM("ethernet", "Ethernet", "lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48", lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48, ethernet_screen_view, 0, MENU_PLACE_MAIN),
    ITEM("apps", "Apps", "GESPAppGallery", GESPAppGallery, apps_menu_view, 0, MENU_PLACE_MAIN),
    ITEM("lock", "Lock", "lock", lock, lockscreen_view, 0, MENU_PLACE_MAIN),
    ITEM("settings", "Settings", "settings_icon", settings_icon, options_menu_view, OT_Settings, MENU_PLACE_MAIN),
    ITEM("visualizer", "Visualizer", "rave", rave, music_visualizer_view, 0, MENU_PLACE_APPS),
#ifdef CONFIG_HAS_AUDIO_PLAYER
    ITEM("audio", "Audio", "speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48", speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48, audio_player_view, 0, MENU_PLACE_APPS),
#endif
#ifdef CONFIG_CROWPANEL_P4_CAMERA
    ITEM("camera", "Camera", "camera_icon", camera_icon, crowpanel_p4_camera_view, 0, MENU_PLACE_APPS),
#endif
    ITEM("terminal", "Terminal", "terminal_icon", terminal_icon, terminal_view, 0, MENU_PLACE_APPS),
    ITEM("sd_browser", "SD Browser", "folder", folder, sd_browser_view, 0, MENU_PLACE_APPS),
#if CONFIG_ENABLE_GHOSTSCRIPT
    ITEM("ghostscript", "GhostScript", "description", description, ghostscript_browser_view, 0, MENU_PLACE_APPS),
#endif
    ITEM("store", "Store", "storefront", storefront, cloud_store_view, 0, MENU_PLACE_APPS),
#ifdef CONFIG_WITH_SCREEN
    ITEM("ghostchi", "Ghostchi", "ghost", ghost, ghostchi_view, 0, MENU_PLACE_APPS),
#endif
    ITEM("clock", "Clock", "clock_icon", clock_icon, clock_view, 0, MENU_PLACE_APPS),
#ifdef CONFIG_HAS_COMPASS
    ITEM("compass", "Compass", "compass", compass, compass_view, 0, MENU_PLACE_APPS),
#endif
#ifdef CONFIG_HAS_ENVIII
    ITEM("enviii", "ENV-III", "enviii", enviii, enviii_view, 0, MENU_PLACE_APPS),
#endif
#ifdef CONFIG_HAS_ACCELEROMETER
    ITEM("accelerometer", "Accelerometer", "accelerometer_icon", accelerometer_icon, accelerometer_view, 0, MENU_PLACE_APPS),
#endif
};
#undef ITEM
#define BUILTIN_COUNT ((int)(sizeof(builtin_items) / sizeof(builtin_items[0])))

int menu_catalog_count(void) {
    /* Match the gallery's native-app RAM requirement. No SD scan on menu entry. */
    return BUILTIN_COUNT + (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? plugin_manager_count() : 0);
}

bool menu_catalog_get(int index, menu_catalog_item_t *item) {
    if (!item || index < 0) return false;
    if (index < BUILTIN_COUNT) {
        *item = builtin_items[index];
        return true;
    }
    const plugin_app_manifest_t *app = plugin_manager_get(index - BUILTIN_COUNT);
    if (!app) return false;
    memset(item, 0, sizeof(*item));
    snprintf(item->id, sizeof(item->id), "plugin:%s", app->id);
    snprintf(item->name, sizeof(item->name), "%s", app->name);
    item->icon = &GESPAppGallery;
    item->view = &plugin_runner_view;
    item->default_placement = MENU_PLACE_APPS;
    return true;
}

bool menu_catalog_available(const menu_catalog_item_t *item, bool connected) {
    if (!item) return false;
    if (strcmp(item->id, "ghostlink") == 0) return connected;
    if (strcmp(item->id, "lock") == 0) return settings_get_lockscreen_enabled(&G_Settings);
    if (strcmp(item->id, "ethernet") == 0) {
#ifdef CONFIG_WITH_ETHERNET
        return true;
#elif defined(CONFIG_BUILD_CONFIG_TEMPLATE)
        return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
        return false;
#endif
    }
    return true;
}

static int item_order(const menu_catalog_item_t *item, uint8_t menu) {
#if CONFIG_CROWPANEL_EPAPER_42
    if (menu == MENU_PLACE_MAIN && item && strcmp(item->id, "book_reader") == 0) return -1;
#endif
    const menu_config_entry_t *entry = menu_config_find(&G_Settings.menu_config, item->id);
    return entry ? entry->order[menu == MENU_PLACE_APPS] : MENU_ORDER_DEFAULT;
}

bool menu_catalog_is_grouped_plugin(const menu_catalog_item_t *item) {
    if (strncmp(item->id, "plugin:", 7) != 0 || menu_config_find(&G_Settings.menu_config, item->id)) return false;
    const plugin_app_manifest_t *app = plugin_manager_find(item->id + 7);
    return app && app->category[0];
}

menu_catalog_item_t *menu_catalog_collect(uint8_t menu, bool available_only, int *count) {
    *count = 0;
    int capacity = menu_catalog_count();
    bool connected = esp_comm_manager_is_connected();
    int required = 0;
    for (int i = 0; i < capacity; ++i) {
        menu_catalog_item_t item;
        if (!menu_catalog_get(i, &item)) continue;
        if (menu && !(menu_config_placement(&G_Settings.menu_config, item.id, item.default_placement) & menu)) continue;
        if (available_only && !menu_catalog_available(&item, connected)) continue;
        ++required;
    }
    menu_catalog_item_t *items = calloc(required > 0 ? required : 1, sizeof(*items));
    if (!items) return NULL;
    for (int i = 0; i < capacity; ++i) {
        menu_catalog_item_t item;
        if (!menu_catalog_get(i, &item)) continue;
        if (menu && !(menu_config_placement(&G_Settings.menu_config, item.id, item.default_placement) & menu)) continue;
        if (available_only && !menu_catalog_available(&item, connected)) continue;
        if (*count >= required) break;
        /* Stable insertion keeps untouched defaults and new apps in original order. */
        int pos = *count;
        if (menu) {
            while (pos > 0 && item_order(&items[pos - 1], menu) > item_order(&item, menu)) {
                items[pos] = items[pos - 1];
                --pos;
            }
        }
        items[pos] = item;
        ++*count;
    }
    return items;
}

void menu_catalog_launch(const char *id, View *return_view) {
    menu_catalog_item_t item;
    bool found = false;
    for (int i = 0; i < menu_catalog_count(); ++i) {
        if (menu_catalog_get(i, &item) && strcmp(item.id, id) == 0) { found = true; break; }
    }
    if (!found || !menu_catalog_available(&item, esp_comm_manager_is_connected())) {
        toast_show("Item unavailable", TOAST_WARN);
        return;
    }
    if (strncmp(item.id, "plugin:", 7) == 0) {
        const plugin_app_manifest_t *app = plugin_manager_find(item.id + 7);
        char missing[24] = {0};
        if (!app || !plugin_manager_required_features_supported(app, missing, sizeof(missing))) {
            char message[80];
            snprintf(message, sizeof(message), "Requires %s", missing[0] ? missing : "unsupported hardware");
            toast_show(message, TOAST_WARN);
            return;
        }
        plugin_runner_set_app(item.id + 7);
    }
    if (item.view == &options_menu_view) SelectedMenuType = item.options_type;
    if (item.view == &terminal_view) {
        terminal_set_return_view(return_view);
        terminal_set_dualcomm_filter(false);
    }
    if (item.view == &ethernet_screen_view) ethernet_screen_set_return_view(return_view);
#ifdef CONFIG_HAS_AUDIO_PLAYER
    if (item.view == &audio_player_view) audio_player_set_return_view(return_view);
#endif
    if (item.view == &lockscreen_view) lockscreen_reset_input();
    status_display_show_status(item.name);
    haptic_manager_play(HAPTIC_EFFECT_SELECTION);
    display_manager_switch_view(item.view);
}
