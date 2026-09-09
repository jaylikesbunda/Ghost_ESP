#include "gui/large_builtin_icons.h"
#include "sdkconfig.h"
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "vendor/images/p4/p4_material_icons.h"
#endif

#include "esp_heap_caps.h"

#include <string.h>

#define LARGE_ICON_SOURCE_SIZE 50
#define LARGE_ICON_SIZE 128
#define LARGE_ICON_CACHE_SIZE 32
#define P4_ICON_WEAK __attribute__((weak))

#if defined(CONFIG_IDF_TARGET_ESP32P4)
extern const lv_img_dsc_t accelerometer_icon P4_ICON_WEAK;
extern const lv_img_dsc_t speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48 P4_ICON_WEAK;
extern const lv_img_dsc_t bluetooth P4_ICON_WEAK;
extern const lv_img_dsc_t camera_icon P4_ICON_WEAK;
extern const lv_img_dsc_t clock_icon P4_ICON_WEAK;
extern const lv_img_dsc_t compass P4_ICON_WEAK;
extern const lv_img_dsc_t description P4_ICON_WEAK;
extern const lv_img_dsc_t dualcomm P4_ICON_WEAK;
extern const lv_img_dsc_t enviii P4_ICON_WEAK;
extern const lv_img_dsc_t lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48 P4_ICON_WEAK;
extern const lv_img_dsc_t folder P4_ICON_WEAK;
extern const lv_img_dsc_t GESPAppGallery P4_ICON_WEAK;
extern const lv_img_dsc_t infrared P4_ICON_WEAK;
extern const lv_img_dsc_t lock P4_ICON_WEAK;
extern const lv_img_dsc_t lora P4_ICON_WEAK;
extern const lv_img_dsc_t Map P4_ICON_WEAK;
extern const lv_img_dsc_t nfc_icon P4_ICON_WEAK;
extern const lv_img_dsc_t nrf24 P4_ICON_WEAK;
extern const lv_img_dsc_t rave P4_ICON_WEAK;
extern const lv_img_dsc_t settings_icon P4_ICON_WEAK;
extern const lv_img_dsc_t storefront P4_ICON_WEAK;
extern const lv_img_dsc_t subghz P4_ICON_WEAK;
extern const lv_img_dsc_t terminal_icon P4_ICON_WEAK;
extern const lv_img_dsc_t usb P4_ICON_WEAK;
extern const lv_img_dsc_t wifi P4_ICON_WEAK;
#endif

typedef struct {
    const lv_img_dsc_t *source;
    lv_img_dsc_t large;
    uint8_t *data;
} large_icon_cache_entry_t;

static large_icon_cache_entry_t s_cache[LARGE_ICON_CACHE_SIZE];

#if defined(CONFIG_IDF_TARGET_ESP32P4)
static const lv_img_dsc_t *p4_native_icon(const lv_img_dsc_t *source) {
    if (source == &accelerometer_icon) return &p4_accelerometer;
    if (source == &speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48) return &p4_audio_icon;
    if (source == &bluetooth) return &p4_bluetooth;
    if (source == &camera_icon) return &p4_camera_icon;
    if (source == &clock_icon) return &p4_clock_icon;
    if (source == &compass) return &p4_compass;
    if (source == &description) return &p4_description;
    if (source == &dualcomm) return &p4_dualcomm;
    if (source == &enviii) return &p4_enviii;
    if (source == &lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48) return &p4_ethernet;
    if (source == &folder) return &p4_folder;
    if (source == &GESPAppGallery) return &p4_GESPAppGallery;
    if (source == &infrared) return &p4_infrared;
    if (source == &lock) return &p4_lock;
    if (source == &lora) return &p4_lora;
    if (source == &Map) return &p4_Map;
    if (source == &nfc_icon) return &p4_nfc;
    if (source == &nrf24) return &p4_nrf24;
    if (source == &rave) return &p4_rave;
    if (source == &settings_icon) return &p4_settings;
    if (source == &storefront) return &p4_storefront;
    if (source == &subghz) return &p4_subghz;
    if (source == &terminal_icon) return &p4_terminal_icon;
    if (source == &usb) return &p4_usb;
    if (source == &wifi) return &p4_wifi;
    return source;
}
#endif

static uint8_t alpha4_at(const lv_img_dsc_t *source, int x, int y) {
    const uint8_t *pixels = source->data;
    uint8_t packed = pixels[y * (LARGE_ICON_SOURCE_SIZE / 2) + x / 2];
    return (x & 1) ? (packed & 0x0F) : (packed >> 4);
}

static uint8_t bilinear_alpha4(const lv_img_dsc_t *source, int x, int y) {
    /* Map the destination pixel center into the complete source icon. The
     * 8-bit fractions avoid floating point and work for 128/50 as well as
     * future generated Material icon sizes. */
    int x_fp = x * (LARGE_ICON_SOURCE_SIZE - 1) * 256 / (LARGE_ICON_SIZE - 1);
    int y_fp = y * (LARGE_ICON_SOURCE_SIZE - 1) * 256 / (LARGE_ICON_SIZE - 1);
    int sx = x_fp >> 8;
    int sy = y_fp >> 8;
    int sx1 = sx < LARGE_ICON_SOURCE_SIZE - 1 ? sx + 1 : sx;
    int sy1 = sy < LARGE_ICON_SOURCE_SIZE - 1 ? sy + 1 : sy;
    int fx = x_fp & 0xff;
    int fy = y_fp & 0xff;
    int a00 = alpha4_at(source, sx, sy);
    int a10 = alpha4_at(source, sx1, sy);
    int a01 = alpha4_at(source, sx, sy1);
    int a11 = alpha4_at(source, sx1, sy1);
    int top = a00 * (256 - fx) + a10 * fx;
    int bottom = a01 * (256 - fx) + a11 * fx;
    return (uint8_t)((top * (256 - fy) + bottom * fy + 32768) / 65536);
}

const lv_img_dsc_t *gui_large_builtin_icon(const lv_img_dsc_t *source) {
#if defined(CONFIG_IDF_TARGET_ESP32P4)
    const lv_img_dsc_t *native = p4_native_icon(source);
    if (native != source) return native;
#if defined(CONFIG_CROWPANEL_ADVANCED_P4)
    if (!source || LV_MIN(LV_HOR_RES, LV_VER_RES) < 480 ||
        source->header.w != LARGE_ICON_SOURCE_SIZE ||
        source->header.h != LARGE_ICON_SOURCE_SIZE ||
        source->header.cf != LV_IMG_CF_ALPHA_4BIT) {
        return source;
    }

    for (int i = 0; i < LARGE_ICON_CACHE_SIZE; ++i) {
        if (s_cache[i].source == source) return &s_cache[i].large;
    }

    int slot = -1;
    for (int i = 0; i < LARGE_ICON_CACHE_SIZE; ++i) {
        if (!s_cache[i].source) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return source;

    uint8_t *data = heap_caps_malloc((LARGE_ICON_SIZE * LARGE_ICON_SIZE) / 2,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) data = heap_caps_malloc((LARGE_ICON_SIZE * LARGE_ICON_SIZE) / 2,
                                       MALLOC_CAP_8BIT);
    if (!data) return source;
    memset(data, 0, (LARGE_ICON_SIZE * LARGE_ICON_SIZE) / 2);

    for (int y = 0; y < LARGE_ICON_SIZE; ++y) {
        for (int x = 0; x < LARGE_ICON_SIZE; ++x) {
            uint8_t alpha = bilinear_alpha4(source, x, y);
            uint8_t *packed = &data[y * (LARGE_ICON_SIZE / 2) + x / 2];
            if (x & 1) *packed |= alpha;
            else *packed |= (uint8_t)(alpha << 4);
        }
    }

    s_cache[slot].source = source;
    s_cache[slot].data = data;
    s_cache[slot].large.header.cf = LV_IMG_CF_ALPHA_4BIT;
    s_cache[slot].large.header.always_zero = 0;
    s_cache[slot].large.header.reserved = 0;
    s_cache[slot].large.header.w = LARGE_ICON_SIZE;
    s_cache[slot].large.header.h = LARGE_ICON_SIZE;
    s_cache[slot].large.data_size = (LARGE_ICON_SIZE * LARGE_ICON_SIZE) / 2;
    s_cache[slot].large.data = data;
    return &s_cache[slot].large;
#endif
#else
    return source;
#endif
}
