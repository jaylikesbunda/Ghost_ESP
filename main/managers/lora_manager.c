// lora_manager.c — stock-framing mesh + guided first-run + duty guard.
// See include/managers/lora_manager.h and docs/lora-meshtastic-app.md.

#include "managers/lora_manager.h"
#include "sdkconfig.h"

#ifndef CONFIG_HAS_LORA
void lora_manager_chat_result(uint32_t p, uint32_t i, bool s) { (void)p; (void)i; (void)s; }
void lora_manager_chat_read(uint32_t p) { (void)p; }
// Stubs so non-LoRa boards pay nothing.
void lora_manager_early_init(void) {}
bool lora_manager_early_init_off_main(void) { return false; }
bool lora_manager_start(void) { return false; }
void lora_manager_stop(void) {}
bool lora_manager_is_running(void) { return false; }
bool lora_manager_is_present(void) { return false; }
const char *lora_manager_last_error(void) { return "lora disabled"; }
bool lora_manager_set_region(lora_region_t r) { (void)r; return false; }
bool lora_manager_set_params(int s, int b, int t) { (void)s; (void)b; (void)t; return false; }
bool lora_manager_set_modem(int sf, int bw, int cr) { (void)sf; (void)bw; (void)cr; return false; }
bool lora_manager_set_preset(int p) { (void)p; return false; }
bool lora_manager_set_custom(int sf, int bw, int cr) { (void)sf; (void)bw; (void)cr; return false; }
bool lora_manager_set_freq_offset(float m) { (void)m; return false; }
bool lora_manager_set_override_freq(float m) { (void)m; return false; }
bool lora_manager_set_channel_num(uint32_t n) { (void)n; return false; }
bool lora_manager_set_tx_enabled(bool e) { (void)e; return false; }
bool lora_manager_set_role(int r) { (void)r; return false; }
int lora_manager_get_role(void) { return 0; }
bool lora_manager_get_modem_cfg(bool *up, int *pr, int *sf, int *bw, int *cr, float *fo, float *of, uint32_t *cn, bool *te) { (void)up; (void)pr; (void)sf; (void)bw; (void)cr; (void)fo; (void)of; (void)cn; (void)te; return false; }
bool lora_manager_apply_app_radio(lora_region_t r, int t) { (void)r; (void)t; return false; }
bool lora_manager_apply_lora_cfg(lora_region_t rg, bool up, int pr, int sf, int bw, int cr, float fo, float of, uint32_t cn, bool te, int tp, int hl) { (void)rg; (void)up; (void)pr; (void)sf; (void)bw; (void)cr; (void)fo; (void)of; (void)cn; (void)te; (void)tp; (void)hl; return false; }
bool lora_manager_set_channel(uint8_t i, const char *n, const uint8_t *p, uint8_t l, uint8_t r, bool u, bool d) { (void)i; (void)n; (void)p; (void)l; (void)r; (void)u; (void)d; return false; }
bool lora_manager_disable_channel(uint8_t i) { (void)i; return false; }
bool lora_manager_send_dm_text(const char *t, uint32_t to, uint32_t pid, bool wa, uint32_t *id) { (void)t; (void)to; (void)pid; (void)wa; (void)id; return false; }
bool lora_manager_send_dm_data(const uint8_t *d, uint16_t dl, uint32_t to, uint32_t pid, bool wa, uint32_t *id) { (void)d; (void)dl; (void)to; (void)pid; (void)wa; (void)id; return false; }
bool lora_manager_send_data_verbatim_ch(uint32_t to, const uint8_t *d, uint16_t dl, uint32_t pid, bool wa, uint8_t ch, uint32_t *id) { (void)to; (void)d; (void)dl; (void)pid; (void)wa; (void)ch; (void)id; return false; }
bool lora_manager_air_send(const uint8_t *f, uint8_t l) { (void)f; (void)l; return false; }
bool lora_manager_set_hop_limit(int h) { (void)h; return false; }
bool lora_manager_set_companion(lora_companion_t c) { (void)c; return false; }
bool lora_manager_send_text(const char *t) { (void)t; return false; }
bool lora_manager_send_app_text(const char *t, uint32_t to, uint32_t *id) {
    (void)t; (void)to; (void)id; return false;
}
bool lora_manager_send_app_text_ex(const char *t, uint32_t to, uint32_t packet_id,
                                   bool want_ack, uint32_t *id) {
    (void)t; (void)to; (void)packet_id; (void)want_ack; (void)id; return false;
}
uint16_t lora_manager_msg_count(void) { return 0; }
bool lora_manager_msg_at(uint16_t i, lora_msg_t *o) { (void)i; (void)o; return false; }
bool lora_manager_latest_message(lora_msg_t *o, uint32_t *s) { (void)o; (void)s; return false; }
uint16_t lora_manager_msg_since(uint32_t *s, lora_msg_t *o, uint16_t m) {
    (void)s; (void)o; (void)m; return 0;
}
bool lora_manager_latest_incoming(lora_msg_t *o, uint32_t *s) {
    (void)o; (void)s; return false;
}
uint16_t lora_manager_node_count(void) { return 0; }
void lora_manager_get_status(lora_status_t *o) { (void)o; }
bool lora_manager_get_hw(lora_hw_t *o) { (void)o; return false; }
#else
#include "core/glog.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include "managers/lora_mesh.h"
#include "managers/lora_channels.h"
#include "managers/lora_modem.h"
#include "managers/lora_store.h"
#include "managers/lora_pki.h"
#include "managers/lora_sx1262.h"
#include "managers/lora_phoneapi.h"
#include "managers/lora_pb.h"
#include "managers/lora_ble.h"
#include "managers/lora_crypto.h"
#include "managers/lora_unishox.h"

#define LORA_MSG_RING CONFIG_LORA_MSG_RING
#define LORA_NVS_NS "lora"
#define LORA_CHAT_MAGIC 0x4C434833U /* LCH3: compact variable-length records */
#define LORA_CHAT_MAGIC_LEGACY 0x4C434832U /* LCH2: full runtime structs */

static const char *TAG = "LORA";

static char s_last_error[96] = "none";
static bool s_running = false;
static bool s_present = false;
static lora_region_t s_region = LORA_REGION_US915;
static bool s_region_saved = false; // true only after an explicit `lora set region`
static uint32_t s_freq_hz = 906875000U; // slot; recomputed in start()
static int s_sf = 11;
static int s_bw_khz = 250; // LongFast default view
static int s_cr = 5;
static int s_tx_dbm = 17;
static int s_hop_limit = 3;
static lora_modem_cfg_t s_modem; // source of truth for preset/custom/offsets
static int s_role = 0; // DeviceConfig.Role CLIENT=0
static lora_companion_t s_companion = LORA_COMPANION_BLE;
static uint32_t s_seq = 0;
static uint32_t s_tx_ok = 0;
static int s_last_rssi = 0;
static float s_last_snr = 0;

#if defined(CONFIG_SPIRAM)
static lora_msg_t *s_ring;
#else
static lora_msg_t s_ring_storage[CONFIG_LORA_MSG_RING];
static lora_msg_t *s_ring = s_ring_storage;
#endif
static uint16_t s_ring_count = 0;
static uint16_t s_ring_head = 0; // next write index
static SemaphoreHandle_t s_lock = NULL;

typedef struct __attribute__((packed)) {
    uint32_t node_num;
    uint32_t packet_id;
    uint8_t flags;
    uint8_t who_len;
    uint8_t text_len;
} lora_chat_disk_rec_t;

#define CHAT_DISK_HEADER 4u
#define CHAT_DISK_MAX_REC (sizeof(lora_chat_disk_rec_t) + 24u + 160u)
#define CHAT_FLAG_OUTGOING 0x01u
#define CHAT_FLAG_DIRECT   0x02u
#define CHAT_FLAG_READ     0x04u
#define CHAT_FLAG_DELIVERY 0x18u
#define CHAT_FLAG_DELIVERY_SHIFT 3u

static void nvs_load_chat(void);
static void nvs_save_chat(void);

static void set_err(const char *m) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", m ? m : "unknown");
}

// First-flash default region from Kconfig (pb codes). NVS wins after that.
#if defined(CONFIG_LORA_REGION_EU868)
#define LORA_KCONFIG_REGION 3
#elif defined(CONFIG_LORA_REGION_EU433)
#define LORA_KCONFIG_REGION 2
#elif defined(CONFIG_LORA_REGION_CN)
#define LORA_KCONFIG_REGION 4
#elif defined(CONFIG_LORA_REGION_JP)
#define LORA_KCONFIG_REGION 5
#elif defined(CONFIG_LORA_REGION_ANZ)
#define LORA_KCONFIG_REGION 6
#elif defined(CONFIG_LORA_REGION_KR)
#define LORA_KCONFIG_REGION 7
#elif defined(CONFIG_LORA_REGION_TW)
#define LORA_KCONFIG_REGION 8
#elif defined(CONFIG_LORA_REGION_RU)
#define LORA_KCONFIG_REGION 9
#elif defined(CONFIG_LORA_REGION_IN)
#define LORA_KCONFIG_REGION 10
#elif defined(CONFIG_LORA_REGION_NZ865)
#define LORA_KCONFIG_REGION 11
#elif defined(CONFIG_LORA_REGION_TH)
#define LORA_KCONFIG_REGION 12
#elif defined(CONFIG_LORA_REGION_UA433)
#define LORA_KCONFIG_REGION 14
#elif defined(CONFIG_LORA_REGION_MY433)
#define LORA_KCONFIG_REGION 16
#elif defined(CONFIG_LORA_REGION_MY919)
#define LORA_KCONFIG_REGION 17
#elif defined(CONFIG_LORA_REGION_SG923)
#define LORA_KCONFIG_REGION 18
#elif defined(CONFIG_LORA_REGION_PH433)
#define LORA_KCONFIG_REGION 19
#elif defined(CONFIG_LORA_REGION_PH868)
#define LORA_KCONFIG_REGION 20
#elif defined(CONFIG_LORA_REGION_PH915)
#define LORA_KCONFIG_REGION 21
#elif defined(CONFIG_LORA_REGION_ANZ433)
#define LORA_KCONFIG_REGION 22
#elif defined(CONFIG_LORA_REGION_KZ433)
#define LORA_KCONFIG_REGION 23
#elif defined(CONFIG_LORA_REGION_KZ863)
#define LORA_KCONFIG_REGION 24
#elif defined(CONFIG_LORA_REGION_NP865)
#define LORA_KCONFIG_REGION 25
#elif defined(CONFIG_LORA_REGION_BR902)
#define LORA_KCONFIG_REGION 26
#else
#define LORA_KCONFIG_REGION 1
#endif

static void nvs_load(void) {
    nvs_handle_t h = 0;
    if (nvs_open(LORA_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint32_t v32 = 0;
    // v2 migration: v1 stored 0=US/1=EU; v2 stores upstream pb codes.
    uint8_t v = 0;
    bool migrated = (nvs_get_u8(h, "region_v", &v) == ESP_OK && v == 2);
    if (!migrated) {
        uint8_t old = 0;
        bool had_legacy = (nvs_get_u8(h, "region", &old) == ESP_OK);
        if (had_legacy && old == 1) {
            s_region = LORA_REGION_EU868;
            s_region_saved = true;
        } else if (had_legacy) {
            s_region = LORA_REGION_US915;
            s_region_saved = true;
        } else {
            s_region = (lora_region_t)LORA_KCONFIG_REGION; // fresh flash
            s_region_saved = false;
        }
    } else if (nvs_get_u32(h, "region32", &v32) == ESP_OK) {
        s_region = (lora_region_t)v32;
        s_region_saved = true;
    }
    if (lora_region_name((int)s_region)[0] == '?') s_region = LORA_REGION_US915;
    int32_t i = 0;
    if (nvs_get_i32(h, "tx", &i) == ESP_OK && i >= 2 && i <= 22) s_tx_dbm = (int)i;
    if (nvs_get_i32(h, "hop", &i) == ESP_OK && i >= 1 && i <= 7) s_hop_limit = (int)i;
    if (nvs_get_u8(h, "comp", &v) == ESP_OK && v <= LORA_COMPANION_WIFI) {
        s_companion = (lora_companion_t)v;
    }
    if (nvs_get_i32(h, "role", &i) == ESP_OK && i >= 0 && i <= 12) s_role = (int)i;
    nvs_close(h);
    // Modem preset/custom is the source of truth for SF/BW/CR (same NVS ns).
    lora_modem_cfg_load(&s_modem);
    {
        int esf = 11, ebw = 250, ecr = 5;
        if (lora_modem_effective(&s_modem, &esf, &ebw, &ecr)) {
            s_sf = esf;
            s_bw_khz = ebw;
            s_cr = ecr;
        }
    }
    lora_channels_init();
    lora_channels_set_default_name(lora_preset_display_name(s_modem.preset, s_modem.use_preset));
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_load_chat();
}

static void nvs_save(void) {
    // NOTE: never writes region keys — only an explicit `lora set region`
    // marks the region saved (see nvs_save_region). Kconfig/flash defaults
    // must not satisfy the start gate.
    // Modem SF/BW/CR live in the modem cfg (same ns, keys sf/bw/cr).
    (void)lora_modem_cfg_save(&s_modem);
    nvs_handle_t h = 0;
    if (nvs_open(LORA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "tx", (int32_t)s_tx_dbm);
    nvs_set_i32(h, "hop", (int32_t)s_hop_limit);
    nvs_set_u8(h, "comp", (uint8_t)s_companion);
    nvs_set_i32(h, "role", (int32_t)s_role);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_region(void) {
    nvs_handle_t h = 0;
    if (nvs_open(LORA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "region_v", 2);
    nvs_set_u32(h, "region32", (uint32_t)s_region);
    nvs_commit(h);
    nvs_close(h);
    s_region_saved = true;
}

/* Chat history is deliberately kept in the shared manager, not in a board
 * view, so Heltec, CrowPanel and other LoRa targets have identical behavior.
 * The on-flash format stores only live bytes: short names/messages do not pay
 * for the full runtime arrays, and timestamps are reconstructed as "saved". */
static void nvs_load_chat(void) {
    nvs_handle_t h = 0;
    if (nvs_open(LORA_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    uint32_t magic = 0;
    uint16_t count = 0, head = 0;
    size_t size = 0;
    bool valid = nvs_get_u32(h, "chat_v", &magic) == ESP_OK &&
                 nvs_get_blob(h, "chat", NULL, &size) == ESP_OK;
    if (valid && magic == LORA_CHAT_MAGIC) {
        valid = size >= CHAT_DISK_HEADER &&
                size <= CHAT_DISK_HEADER + (size_t)LORA_MSG_RING * CHAT_DISK_MAX_REC;
    } else if (valid && magic == LORA_CHAT_MAGIC_LEGACY) {
        valid = size && (size % sizeof(lora_msg_t)) == 0 &&
                (size / sizeof(lora_msg_t)) <= LORA_MSG_RING &&
                nvs_get_u16(h, "chat_n", &count) == ESP_OK &&
                nvs_get_u16(h, "chat_h", &head) == ESP_OK;
    } else {
        valid = false;
    }
    uint8_t *blob = valid ? (uint8_t *)malloc(size) : NULL;
    if (valid) valid = blob && nvs_get_blob(h, "chat", blob, &size) == ESP_OK;
    nvs_close(h);
    if (!valid || !blob || !s_lock) {
        free(blob);
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_ring, 0, sizeof(*s_ring) * LORA_MSG_RING);
    if (magic == LORA_CHAT_MAGIC_LEGACY) {
        /* Migrate a previous full-struct circular blob into the new ring. */
        uint16_t old_slots = (uint16_t)(size / sizeof(lora_msg_t));
        uint16_t old_count = count < old_slots ? count : old_slots;
        for (uint16_t order = 0; order < old_count; ++order) {
            uint16_t old_index = (uint16_t)((head + old_slots - old_count + order) % old_slots);
            memcpy(&s_ring[order], blob + (size_t)old_index * sizeof(lora_msg_t),
                   sizeof(lora_msg_t));
        }
        s_ring_count = old_count;
        s_ring_head = old_count % LORA_MSG_RING;
    } else {
        uint16_t stored_count = (uint16_t)blob[0] | ((uint16_t)blob[1] << 8);
        size_t pos = CHAT_DISK_HEADER;
        uint16_t loaded = 0;
        while (loaded < stored_count && loaded < LORA_MSG_RING &&
               pos + sizeof(lora_chat_disk_rec_t) <= size) {
            lora_chat_disk_rec_t rec;
            memcpy(&rec, blob + pos, sizeof(rec));
            pos += sizeof(rec);
            if (rec.who_len > sizeof(s_ring[0].who) - 1 ||
                rec.text_len > sizeof(s_ring[0].text) - 1 ||
                pos + rec.who_len + rec.text_len > size) break;
            lora_msg_t *m = &s_ring[loaded++];
            m->node_num = rec.node_num;
            m->packet_id = rec.packet_id;
            m->outgoing = (rec.flags & CHAT_FLAG_OUTGOING) != 0;
            m->direct = (rec.flags & CHAT_FLAG_DIRECT) != 0;
            m->read = (rec.flags & CHAT_FLAG_READ) != 0;
            m->delivery = (uint8_t)((rec.flags & CHAT_FLAG_DELIVERY) >> CHAT_FLAG_DELIVERY_SHIFT);
            memcpy(m->who, blob + pos, rec.who_len);
            m->who[rec.who_len] = '\0';
            pos += rec.who_len;
            memcpy(m->text, blob + pos, rec.text_len);
            m->text[rec.text_len] = '\0';
            pos += rec.text_len;
        }
        s_ring_count = loaded;
        s_ring_head = loaded % LORA_MSG_RING;
    }
    s_seq = s_ring_count;
    /* Uptime timestamps cannot survive a restart. Pending ACKs cannot either;
     * show them as failed/unknown instead of leaving a permanent spinner. */
    for (uint16_t i = 0; i < s_ring_count; ++i) {
        s_ring[i].timestamp_ms = 0;
        s_ring[i].read = true;
        if (s_ring[i].outgoing && s_ring[i].delivery == 1) s_ring[i].delivery = 3;
    }
    xSemaphoreGive(s_lock);
    free(blob);
}

static void nvs_save_chat(void) {
    /* Serialize only live records. The temporary buffer is freed before
     * return, so increasing history does not add a second permanent ring. */
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t count = s_ring_count;
    size_t max_size = CHAT_DISK_HEADER + (size_t)count * CHAT_DISK_MAX_REC;
    uint8_t *blob = (uint8_t *)malloc(max_size);
    if (!blob) {
        xSemaphoreGive(s_lock);
        return;
    }

    size_t pos = CHAT_DISK_HEADER;
    blob[0] = (uint8_t)(count & 0xFF);
    blob[1] = (uint8_t)(count >> 8);
    blob[2] = blob[3] = 0;
    for (uint16_t order = 0; order < count; ++order) {
        uint16_t index = (uint16_t)((s_ring_head + LORA_MSG_RING - s_ring_count + order) % LORA_MSG_RING);
        const lora_msg_t *m = &s_ring[index];
        lora_chat_disk_rec_t rec = {
            .node_num = m->node_num,
            .packet_id = m->packet_id,
            .flags = (m->outgoing ? CHAT_FLAG_OUTGOING : 0) |
                     (m->direct ? CHAT_FLAG_DIRECT : 0) |
                     (m->read ? CHAT_FLAG_READ : 0) |
                     ((m->delivery & 0x03u) << CHAT_FLAG_DELIVERY_SHIFT),
            .who_len = (uint8_t)strnlen(m->who, sizeof(m->who)),
            .text_len = (uint8_t)strnlen(m->text, sizeof(m->text)),
        };
        memcpy(blob + pos, &rec, sizeof(rec));
        pos += sizeof(rec);
        memcpy(blob + pos, m->who, rec.who_len);
        pos += rec.who_len;
        memcpy(blob + pos, m->text, rec.text_len);
        pos += rec.text_len;
    }
    xSemaphoreGive(s_lock);

    nvs_handle_t h = 0;
    if (nvs_open(LORA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        free(blob);
        return;
    }
    esp_err_t err = nvs_set_blob(h, "chat", blob, pos);
    if (err == ESP_OK) err = nvs_set_u16(h, "chat_n", count);
    if (err == ESP_OK) err = nvs_set_u16(h, "chat_h", count % LORA_MSG_RING);
    if (err == ESP_OK) err = nvs_set_u32(h, "chat_v", LORA_CHAT_MAGIC);
    if (err == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
    free(blob);
}

static void ring_push(bool outgoing, const char *who, const char *text,
                      uint32_t node_num, bool direct, uint32_t packet_id, bool want_ack) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lora_msg_t *slot = &s_ring[s_ring_head];
    slot->outgoing = outgoing;
    slot->direct = direct;
    slot->read = outgoing;
    slot->delivery = outgoing && want_ack ? 1 : 0;
    slot->packet_id = packet_id;
    slot->node_num = node_num;
    slot->timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snprintf(slot->who, sizeof(slot->who), "%s", who ? who : "??");
    snprintf(slot->text, sizeof(slot->text), "%s", text ? text : "");
    s_ring_head = (uint16_t)((s_ring_head + 1) % LORA_MSG_RING);
    if (s_ring_count < LORA_MSG_RING) s_ring_count++;
    s_seq++;
    xSemaphoreGive(s_lock);
    nvs_save_chat();
}

void lora_manager_chat_result(uint32_t peer, uint32_t id, bool success) {
    if (!s_lock || !id) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < LORA_MSG_RING; ++i) {
        lora_msg_t *m = &s_ring[i];
        if (m->outgoing && m->node_num == peer && m->packet_id == id) {
            m->delivery = success ? 2 : 3;
            changed = true;
        }
    }
    xSemaphoreGive(s_lock);
    if (changed) nvs_save_chat();
}

void lora_manager_chat_read(uint32_t peer) {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (uint16_t i = 0; i < LORA_MSG_RING; ++i)
        if (peer ? (s_ring[i].direct && s_ring[i].node_num == peer) : !s_ring[i].direct)
            s_ring[i].read = true;
    xSemaphoreGive(s_lock);
}

bool lora_manager_get_hw(lora_hw_t *out) {
    if (!out) return false;
    out->spi_host = CONFIG_LORA_SPI_HOST;
    out->mosi_pin = CONFIG_LORA_SPI_MOSI_PIN;
    out->miso_pin = CONFIG_LORA_SPI_MISO_PIN;
    out->sck_pin = CONFIG_LORA_SPI_SCK_PIN;
    out->nss_pin = CONFIG_LORA_NSS_PIN;
    out->dio1_pin = CONFIG_LORA_DIO1_PIN;
    out->busy_pin = CONFIG_LORA_BUSY_PIN;
    out->rst_pin = CONFIG_LORA_RST_PIN;
#if CONFIG_LORA_MODULE_SX1276
    out->module = LORA_MODULE_SX1276;
#elif CONFIG_LORA_MODULE_LLCC68
    out->module = LORA_MODULE_LLCC68;
#else
    out->module = LORA_MODULE_SX1262;
#endif
#if CONFIG_LORA_TCXO_DISABLED
    out->tcxo_controlled = false;
    out->tcxo_voltage_code = 0;
#elif CONFIG_LORA_TCXO_3V3
    out->tcxo_controlled = true;
    out->tcxo_voltage_code = 0x07;
#else
    out->tcxo_controlled = true;
    out->tcxo_voltage_code = 0x02;
#endif
    out->dio2_rf_switch = true;
    out->max_tx_dbm = CONFIG_LORA_MAX_TX_DBM;
    // Basic pin sanity: no duplicates among wired pins.
    int pins[7] = {out->mosi_pin, out->miso_pin, out->sck_pin, out->nss_pin,
                   out->dio1_pin, out->busy_pin, out->rst_pin};
    for (int i = 0; i < 7; i++) {
        if (pins[i] < 0) continue;
        for (int j = i + 1; j < 7; j++) {
            if (pins[j] < 0) continue;
            if (pins[i] == pins[j]) {
                set_err("pin conflict");
                return false;
            }
        }
    }
    return true;
}

static uint32_t s_rx_ok = 0;
static uint32_t s_tx_fail = 0;
static uint32_t s_tx_relay = 0;
static uint32_t s_q_drops = 0;
#define LORA_PENDING_ACK_MAX 8
static uint32_t s_pending_ack_ids[LORA_PENDING_ACK_MAX];
static uint8_t s_pending_ack_pos = 0;
static portMUX_TYPE s_pending_ack_mux = portMUX_INITIALIZER_UNLOCKED;

static void pending_ack_track(uint32_t id) {
    if (!id) return;
    portENTER_CRITICAL(&s_pending_ack_mux);
    s_pending_ack_ids[s_pending_ack_pos] = id;
    s_pending_ack_pos = (uint8_t)((s_pending_ack_pos + 1) % LORA_PENDING_ACK_MAX);
    portEXIT_CRITICAL(&s_pending_ack_mux);
}

static bool pending_ack_take(uint32_t id) {
    bool found = false;
    if (!id) return false;
    portENTER_CRITICAL(&s_pending_ack_mux);
    for (uint8_t i = 0; i < LORA_PENDING_ACK_MAX; i++) {
        if (s_pending_ack_ids[i] == id) {
            s_pending_ack_ids[i] = 0;
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_pending_ack_mux);
    return found;
}

// Local Routing NAK to the phone (failed originations: duty-limit etc.).
// from=self, request_id=origin id so the app correlates the failure.
static void push_routing_nak_to_phone(uint32_t dest, uint32_t req_id, uint8_t err) {
    if (!req_id || !err) return;
    uint8_t routing[] = {0x18, err};
    uint8_t data[16];
    uint16_t dl = pb_build_data_msg(data, sizeof(data), 5,
                                    routing, sizeof(routing), req_id);
    if (!dl) return;
    uint32_t me = lora_mesh_node_num();
    uint32_t aid = 0;
    while (aid == 0) aid = esp_random();
    if (lora_phoneapi_push_mesh_data(me, dest ? dest : me, aid, data, dl))
        lora_ble_notify_from_num();
}

// Parse air header fields needed for duty-NAK decisions (no mesh internals).
static bool air_hdr_parse(const uint8_t *f, uint8_t len, uint32_t *to,
                          uint32_t *id, bool *want_ack) {
    if (!f || len < 16) return false;
    uint32_t t = (uint32_t)f[0] | ((uint32_t)f[1] << 8) |
                 ((uint32_t)f[2] << 16) | ((uint32_t)f[3] << 24);
    uint32_t i = (uint32_t)f[8] | ((uint32_t)f[9] << 8) |
                 ((uint32_t)f[10] << 16) | ((uint32_t)f[11] << 24);
    if (to) *to = t;
    if (id) *id = i;
    if (want_ack) *want_ack = (f[12] & 0x08) != 0;
    return true;
}

// ---- RX-to-phone parity helpers (manager-local) ----
// Per-peer last-heard: STEP_PEERS (lora_phoneapi.c, phoneapi-owned) stamps
// time(NULL) for every peer, so all entries render "just now". This table
// records the actual last-heard epoch per node on any decoded RX (channel
// or PKI DM) while the wall clock is valid, and backs
// lora_manager_peer_last_heard() used by the immediate NodeInfo pushes
// below. PHONEAPI ONE-LINER (owner applies, not here): in STEP_PEERS use
//   heard = lora_manager_peer_last_heard(nd.node_num);
// (with `extern uint32_t lora_manager_peer_last_heard(uint32_t node);`)
// instead of stamping now for all peers.
// Mute/ignore audit (grep 2026-09-07, mesh/phoneapi-owned, no new getters
// found): muted is live via lora_mesh_peer_meta() (admin toggle_muted_node
// writes the mesh table) and is enforced below; ignored lives only in
// phoneapi's static s_peer_flags (peer_flag_get, no mesh getter) and
// channel-mute only in phoneapi's opaque s_ch_mod[] (ModuleSettings
// is_muted, no parsed getter), so neither is enforceable here today.
// s_mgr_ignored[] is a bridge cache the phoneapi owner can mirror via
// lora_manager_note_ignored() (wire set/remove_ignored_node af 47/48,
// add_contact should_ignore, remove_by_nodenum clear); until then the
// ignored leg is a no-op. Flood/relay path (mesh-owned, below) untouched.
#define LORA_MGR_HEARD_MAX 16
#define LORA_MGR_IGNORED_MAX 16
static uint32_t s_heard_nodes[LORA_MGR_HEARD_MAX];
static uint32_t s_heard_epochs[LORA_MGR_HEARD_MAX];
static uint32_t s_mgr_ignored[LORA_MGR_IGNORED_MAX];

// Prototypes here (not lora_manager.h: header owned by another workstream).
// A compatible re-declaration there later is harmless.
uint32_t lora_manager_peer_last_heard(uint32_t node);
void lora_manager_note_ignored(uint32_t node, bool ignored);

static void mgr_heard_note(uint32_t node, uint32_t epoch) {
    if (!node || !epoch) return;
    if (node == lora_mesh_node_num()) return; // self needs no last-heard
    for (int i = 0; i < LORA_MGR_HEARD_MAX; i++) {
        if (s_heard_nodes[i] == node) { s_heard_epochs[i] = epoch; return; }
    }
    for (int i = 0; i < LORA_MGR_HEARD_MAX; i++) {
        if (s_heard_nodes[i] == 0) {
            s_heard_nodes[i] = node;
            s_heard_epochs[i] = epoch;
            return;
        }
    }
    int oldest = 0; // full: evict least-recently-heard
    for (int i = 1; i < LORA_MGR_HEARD_MAX; i++) {
        if (s_heard_epochs[i] < s_heard_epochs[oldest]) oldest = i;
    }
    s_heard_nodes[oldest] = node;
    s_heard_epochs[oldest] = epoch;
}

// Last-heard epoch for a peer, 0 = never heard / no valid clock yet (the
// pb builder omits last_heard then, fixing 1970 instead of "just now").
uint32_t lora_manager_peer_last_heard(uint32_t node) {
    if (!node) return 0;
    for (int i = 0; i < LORA_MGR_HEARD_MAX; i++) {
        if (s_heard_nodes[i] == node) return s_heard_epochs[i];
    }
    return 0;
}

void lora_manager_note_ignored(uint32_t node, bool ignored) {
    if (!node) return;
    for (int i = 0; i < LORA_MGR_IGNORED_MAX; i++) {
        if (s_mgr_ignored[i] == node) {
            if (!ignored) s_mgr_ignored[i] = 0;
            return;
        }
    }
    if (!ignored) return;
    for (int i = 0; i < LORA_MGR_IGNORED_MAX; i++) {
        if (s_mgr_ignored[i] == 0) { s_mgr_ignored[i] = node; return; }
    }
    // Full: drop (ACK still sent phone-side; mirrors phoneapi behavior).
}

static bool mgr_peer_ignored(uint32_t node) {
    if (!node) return false;
    for (int i = 0; i < LORA_MGR_IGNORED_MAX; i++) {
        if (s_mgr_ignored[i] == node) return true;
    }
    return false;
}

static bool mgr_peer_muted(uint32_t node) {
    bool muted = false;
    lora_mesh_peer_meta(node, NULL, &muted, NULL, NULL);
    return muted;
}

// Phone-push suppression: muted (live via mesh) or ignored (bridge cache).
// Relay/flood intentionally NOT gated here (mesh-owned path below).
static bool mgr_peer_suppressed(uint32_t node) {
    return mgr_peer_muted(node) || mgr_peer_ignored(node);
}

void lora_manager_phone_session_reset(void) {
    // s_heard_* and s_mgr_ignored[] intentionally persist: RF last-heard and
    // admin mute/ignore outlive a BLE session (phoneapi's own s_peer_flags
    // persist too; see lora_phoneapi_reset).
    portENTER_CRITICAL(&s_pending_ack_mux);
    memset(s_pending_ack_ids, 0, sizeof(s_pending_ack_ids));
    s_pending_ack_pos = 0;
    portEXIT_CRITICAL(&s_pending_ack_mux);
}

// Port 7 (TEXT_MESSAGE_COMPRESSED_APP) RX parity: Unishox2-decompress the
// Data.payload so compressed chat follows the identical port-1 chat path in
// lora_rx_cb (ring + phone push, same privacy gates). Returns true with
// out_who/out_text filled only on success. *out_dupe is set when an exact
// (from,id) repeat is suppressed. Any other failure
// returns false and the caller keeps today's opaque forward.
// Radio-task context only; file-static scratch keeps RX-task stack flat.
#define LORA_P7_SEEN_N 8
static struct { uint32_t from; uint32_t id; } s_p7_seen[LORA_P7_SEEN_N];
static uint8_t s_p7_seen_pos = 0;

// Delivery dedup is deliberately separate from the mesh routing history.
// A repeated reliable packet must still reach the ACK and flood logic, but
// must not become a second chat/module row in the companion app.
#define LORA_DELIVERY_SEEN_N 32
static struct { uint32_t from; uint32_t id; } s_delivery_seen[LORA_DELIVERY_SEEN_N];
static uint8_t s_delivery_seen_pos = 0;
static bool delivery_seen_before(uint32_t from, uint32_t id) {
    if (from == 0 || id == 0 || from == lora_mesh_node_num()) return false;
    for (int i = 0; i < LORA_DELIVERY_SEEN_N; i++) {
        if (s_delivery_seen[i].from == from && s_delivery_seen[i].id == id)
            return true;
    }
    s_delivery_seen[s_delivery_seen_pos].from = from;
    s_delivery_seen[s_delivery_seen_pos].id = id;
    s_delivery_seen_pos = (uint8_t)((s_delivery_seen_pos + 1) % LORA_DELIVERY_SEEN_N);
    return false;
}
static bool lora_p7_try_chat(uint32_t from, uint32_t to, uint32_t air_id,
                             const uint8_t *data, uint16_t data_len, bool is_pki,
                             char *out_who, size_t who_max,
                             char *out_text, size_t text_max, bool *out_dupe) {
    if (out_dupe) *out_dupe = false;
    if (!data || !data_len || !out_who || !who_max || !out_text || text_max < 2)
        return false;
    // Gates mirror lora_mesh_on_rx_ex: not-self, broadcast-or-self only.
    if (from == lora_mesh_node_num()) return false;
    if (to != LORA_MESH_BROADCAST && to != lora_mesh_node_num()) return false;
    if (from != 0 && air_id != 0) {
        for (int i = 0; i < LORA_P7_SEEN_N; i++) {
            if (s_p7_seen[i].from == from && s_p7_seen[i].id == air_id) {
                if (out_dupe) *out_dupe = true;
                return false;
            }
        }
    }
    // Data.payload is field 2, wire type 2.
    const uint8_t *cp = NULL;
    uint16_t cl = 0;
    {
        pb_r_t dr;
        pb_r_init(&dr, data, data_len);
        uint8_t df, dw;
        uint32_t dv;
        const uint8_t *db;
        uint16_t dl;
        while (pb_r_next(&dr, &df, &dw, &dv, &db, &dl)) {
            if (df == 2 && dw == 2) { cp = db; cl = dl; break; }
        }
    }
    if (!cp || !cl) return false;
    if (lora_unishox_decompress(cp, (int)cl, out_text, (int)text_max) <= 0)
        return false;
    // who mirrors on_rx_ex: learned short name, else hex; DM: prefix for PKI.
    {
        char base[8] = {0};
        bool named = lora_mesh_peer_short_name(from, base, sizeof(base));
        if (!named) snprintf(base, sizeof(base), "%06X", (unsigned)(from & 0xFFFFFF));
        if (is_pki) snprintf(out_who, who_max, "DM:%s", base);
        else snprintf(out_who, who_max, "%s", base);
    }
    if (from != 0 && air_id != 0) {
        s_p7_seen[s_p7_seen_pos].from = from;
        s_p7_seen[s_p7_seen_pos].id = air_id;
        s_p7_seen_pos = (uint8_t)((s_p7_seen_pos + 1) % LORA_P7_SEEN_N);
    }
    return true;
}

// Meshtastic RoutingModule::getHopLimitForResponse() for the received header.
// A direct request with a nonzero hop_start gets a small two-hop return path;
// an explicitly local (hop_start=0) request stays local.
static uint8_t response_hop_limit(uint32_t hop_start, uint32_t hop_limit) {
    uint8_t configured = s_hop_limit > 0 && s_hop_limit <= 7
                             ? (uint8_t)s_hop_limit
                             : 3;
    uint8_t used = hop_start >= hop_limit
                       ? (uint8_t)(hop_start - hop_limit)
                       : 0;
    if (used > configured) return used > 7 ? 7 : used;
    if (hop_start == 0) return 0;
    if ((uint8_t)(used + 2) < configured) return (uint8_t)(used + 2);
    return configured;
}

// Driver RX callback (radio task context): deliver locally AND flood —
// upstream does both for every reception (flood regardless of delivery).
// FIFO ops are lock-guarded inside phoneapi.
static void lora_rx_cb(const uint8_t *payload, uint8_t len,
                       int16_t rssi, float snr, void *ctx) {
    (void)ctx;
    char raw[25] = {0};
    uint8_t raw_n = len < 8 ? len : 8;
    for (uint8_t i = 0; i < raw_n; i++) snprintf(raw + i * 3, 4, "%02X ", payload[i]);
    ESP_LOGI(TAG, "radio RX payload=%uB rssi=%d snr=%.1f head=[%s]",
             (unsigned)len, (int)rssi, (double)snr, raw);
    // Native only admits a sender to NodeDB after successfully decoding the
    // packet. Opaque headers are unauthenticated and must not create blank or
    // spoofable contacts; decoded packets are touched by lora_mesh_on_rx_ex.
    char who[24] = {0};
    char text[LORA_MESH_TEXT_MAX + 1] = {0};
    uint32_t from = 0, air_id = 0, to = 0, hs = 0, hl = 0;
    uint8_t data[233];
    uint16_t data_len = 0;
    uint8_t port = 0;
    uint8_t ch_idx = 0;
    uint32_t request_id = 0;
    bool want_ack = false;
    bool decoded = false;
    bool is_pki = false;
    bool module_reply_sent = false;
    if (len > 16) {
        // Native authenticates eligible channel-0 directs with PKI before
        // trying the eight-bit channel hash.
        uint8_t pport = 0;
        bool pwa = false;
        uint16_t pl = lora_mesh_try_pki(payload, len, &from, &to, &air_id,
                                        data, sizeof(data), &pport, &pwa);
        if (pl) {
            decoded = true;
            is_pki = true;
            port = pport;
            data_len = pl;
            want_ack = pwa;
            request_id = 0;
            // Data.request_id, if present, correlates routing ACKs.
            {
                pb_r_t drr;
                pb_r_init(&drr, data, data_len);
                uint8_t df2, dw2;
                uint32_t dv2;
                const uint8_t *db2;
                uint16_t dl2;
                while (pb_r_next(&drr, &df2, &dw2, &dv2, &db2, &dl2)) {
                    if (df2 == 6 && dw2 == 5) { request_id = dv2; break; }
                }
            }
            ch_idx = lora_channel_primary();
            // PKI changes only the payload encoding; routing metadata remains
            // in the ordinary clear header and must survive for ACK/reply hops.
            hl = payload[12] & 0x07;
            hs = (payload[12] >> 5) & 0x07;
        }
    }
    if (!decoded) {
        decoded = lora_mesh_decode_data_ch(payload, len, &from, &to, &air_id,
                                            &want_ack, &port, &request_id, &ch_idx,
                                            &hs, &hl,
                                            data, sizeof(data), &data_len);
    }
    // Reception metadata for phone-bound packets (fixes 1970 last-heard:
    // rx_time lets the app stamp arrivals even before any clock sync of
    // its own, and matches upstream's per-packet rx_time).
    pb_rx_meta_t rxmeta;
    memset(&rxmeta, 0, sizeof(rxmeta));
    {
        time_t rnow = time(NULL);
        if (rnow >= 1577836800LL && (uint64_t)rnow <= UINT32_MAX)
            rxmeta.rx_time = (uint32_t)rnow;
    }
    rxmeta.snr = snr;
    if (hs >= hl) lora_mesh_note_hops(from, (uint8_t)(hs - hl));
    bool delivery_duplicate = decoded && delivery_seen_before(from, air_id);
    if (delivery_duplicate) {
        ESP_LOGD(TAG, "mesh duplicate delivery suppressed from=%08x id=%08x",
                 (unsigned)from, (unsigned)air_id);
    }
    // Per-peer last-heard for the phone NodeDB (STEP_PEERS + immediate
    // pushes read the table instead of stamping now for every peer).
    if (decoded && rxmeta.rx_time) mgr_heard_note(from, rxmeta.rx_time);
    if (decoded && port == 4 && from != lora_mesh_node_num()) {
        // NODEINFO_APP is promiscuous upstream: accept valid overheard
        // unicasts too. Preserve and queue the exact decrypted Data packet
        // instead of fabricating a separate NodeInfoLite phone record.
        const uint8_t *user = NULL;
        uint16_t user_len = 0;
        pb_r_t dr;
        pb_r_init(&dr, data, data_len);
        uint8_t df, dw;
        uint32_t dv;
        const uint8_t *db;
        uint16_t dl;
        bool want_response = false;
        while (pb_r_next(&dr, &df, &dw, &dv, &db, &dl)) {
            if (df == 2 && dw == 2) { user = db; user_len = dl; }
            else if (df == 3 && dw == 0) want_response = dv != 0;
        }
        uint32_t hw = 0, role = 0;
        const uint8_t *rx_pub = NULL;
        if (user) {
            pb_r_t ur;
            pb_r_init(&ur, user, user_len);
            while (pb_r_next(&ur, &df, &dw, &dv, &db, &dl)) {
                if (df == 5 && dw == 0) hw = dv;
                else if (df == 7 && dw == 0) role = dv;
                else if (df == 8 && dw == 2 && dl == 32) rx_pub = db;
            }
        }
        bool key_ok = user &&
                      lora_mesh_peer_nodeinfo_key_ok(from, rx_pub, rx_pub != NULL);
        if (key_ok) {
            if (rx_pub) lora_mesh_peer_set_pubkey(from, rx_pub);
            lora_mesh_peer_set_hw_role(from, hw, role);
            lora_mesh_peer_set_channel(from, ch_idx);
            ESP_LOGI(TAG, "mesh NodeInfo from %08x ch=%u reply=%u key=%s",
                     (unsigned)from, (unsigned)ch_idx,
                     (unsigned)want_response, rx_pub ? "yes" : "no");
            if (mgr_peer_suppressed(from)) {
                ESP_LOGI(TAG, "mesh NodeInfo from muted/ignored %08x suppressed (phone)",
                         (unsigned)from);
            } else if (!delivery_duplicate &&
                       lora_phoneapi_push_mesh_data_rx(from, to, air_id, ch_idx,
                                                       hl, hs, data, data_len,
                                                       &rxmeta)) {
                lora_ble_notify_from_num();
            }
        } else {
            ESP_LOGW(TAG, "mesh NodeInfo from %08x rejected", (unsigned)from);
        }
        // Promiscuous reception must not answer a request intended for some
        // other node. Native replies on the request packet's channel.
        // Meshtastic's NodeInfo module decides whether to answer independently
        // of whether NodeDB accepted the advertised key.  This is important
        // after a peer rekeys: its request may be rejected locally, but our
        // reply still lets that peer refresh our identity and resume PKI.
        if (user && want_response &&
            (to == LORA_MESH_BROADCAST || to == lora_mesh_node_num())) {
            if (lora_mesh_nodeinfo_reply_allowed(from)) {
                module_reply_sent = lora_mesh_send_nodeinfo_reply(
                    from, ch_idx, air_id, want_ack,
                    response_hop_limit(hs, hl));
            } else {
                ESP_LOGD(TAG, "NodeInfo reply suppressed for %08x", (unsigned)from);
            }
        }
    }
    // Port 7 (TEXT_MESSAGE_COMPRESSED_APP): Unishox2-decompress first so
    // compressed chat rides the identical port-1 path below (same ring +
    // phone push, same privacy gates). p7_chat=false on any failure keeps
    // the existing opaque forward; p7_dupe drops exact (from,id) repeats
    // like port-1 duplicates (no chat AND no opaque forward).
    bool p7_chat = false;
    bool p7_dupe = false;
    char p7_who[24] = {0};
    char p7_text[LORA_MESH_TEXT_MAX + 1] = {0};
    if (decoded && port == LORA_UNISHOX_PORTNUM) {
        p7_chat = lora_p7_try_chat(from, to, air_id, data, data_len, is_pki,
                                   p7_who, sizeof(p7_who),
                                   p7_text, sizeof(p7_text), &p7_dupe);
    }
    const char *chat_who = p7_chat ? p7_who : who;
    const char *chat_text = p7_chat ? p7_text : text;
    if (p7_chat ||
        lora_mesh_on_rx_ex(payload, len, rssi, snr, who, sizeof(who),
                           text, sizeof(text), &from, &air_id, &to, &hs, &hl)) {
        if (!delivery_duplicate) {
            s_last_rssi = rssi;
            s_last_snr = snr;
            s_rx_ok++;
            ring_push(false, chat_who, chat_text, is_pki ? from : 0, is_pki, 0, false);
            ESP_LOGI(TAG, "mesh RX chat from %s rssi=%d snr=%.1f%s: %.40s",
                     chat_who, rssi, (double)snr, is_pki ? " (DM)" : "", chat_text);
        }
        // Muted/ignored or duplicate: skip the phone push. Routing and ACK
        // handling below still see the repeated packet.
        if (delivery_duplicate) {
            // No delivery work.
        } else if (mgr_peer_suppressed(from)) {
            ESP_LOGI(TAG, "mesh chat from muted/ignored %08x suppressed (phone)",
                     (unsigned)from);
        } else if (is_pki) {
            uint8_t spub[32];
            if (lora_mesh_peer_pubkey(from, spub)) {
                uint8_t dd[200];
                const uint8_t *phone_data = data;
                uint16_t phone_len = data_len;
                uint16_t ddl = 0;
                // Port-7 compressed text is presented to the phone as normal
                // port-1 chat. Ordinary PKI text keeps the exact decrypted
                // Data so reply_id/emoji/thread metadata survives.
                if (p7_chat) {
                    ddl = pb_build_data_msg(dd, sizeof(dd), 1,
                                            (const uint8_t *)chat_text,
                                            strlen(chat_text), 0);
                    phone_data = dd;
                    phone_len = ddl;
                }
                // Preserve the air packet ID. The app uses it for message
                // identity/deduplication and Routing ACK correlation.
                if (phone_len && lora_phoneapi_push_mesh_data_pki_rx(
                                      from, to, air_id, ch_idx, hs, hl, spub,
                                      phone_data, phone_len, &rxmeta))
                    lora_ble_notify_from_num();
                memset(spub, 0, sizeof(spub));
                memset(dd, 0, sizeof(dd));
            } else {
                lora_phoneapi_push_mesh_text_rx(from, to, air_id, hl, hs, ch_idx,
                                                chat_text, &rxmeta);
                if (lora_phoneapi_has_data()) lora_ble_notify_from_num();
            }
        } else {
            lora_phoneapi_push_mesh_text_rx(from, to, air_id, hl, hs, ch_idx,
                                            chat_text, &rxmeta);
            if (lora_phoneapi_has_data()) lora_ble_notify_from_num();
        }
    } else if (!p7_dupe && !delivery_duplicate && decoded && lora_phoneapi_is_linked() &&
               from != lora_mesh_node_num() && port != 1 && port != 4 && port != 5 &&
               (to == LORA_MESH_BROADCAST || to == lora_mesh_node_num() ||
                // Promiscuous forward (stock-app parity: the app shows every
                // heard node): overheard unicast position(3)/telemetry(67)/
                // waypoint(8)/traceroute(70) to OTHER nodes is still pushed.
                // Chat privacy kept: TEXT(1) rides the chat path above and
                // PKI stays self/bcast-only. Port 7 (compressed text) that
                // decompresses rides the chat path too; undecodable port 7
                // stays opaque and self/bcast-only like TEXT.
                // NeighborInfo(71) gate unchanged.
                (!is_pki && (port == 3 || port == 67 || port == 8 || port == 70)))) {
        bool mgr_sup = mgr_peer_suppressed(from);
        // Module traffic (position/telemetry/traceroute/...): forward the
        // decrypted Data to the phone so module screens stay live.
        // NEIGHBORINFO (71) is dropped unless the module is explicitly
        // enabled (default drop; upstream NeighborInfoModule parity).
        if (port == 71 && !lora_mesh_neighborinfo_enabled()) {
            ESP_LOGI(TAG, "mesh NEIGHBORINFO dropped (module disabled)");
        } else {
        s_rx_ok++;
        // Muted/ignored: skip the phone push (air traceroute reply below kept).
        if (mgr_sup) {
            ESP_LOGI(TAG, "mesh port=%u from muted/ignored %08x suppressed (phone)",
                     (unsigned)port, (unsigned)from);
        }
        bool ok = false;
        if (mgr_sup) {
            ok = false;
        } else if (is_pki) {
            uint8_t spub[32];
            bool has = lora_mesh_peer_pubkey(from, spub);
            uint8_t mesh[256];
            uint16_t ml = pb_build_mesh_packet_pki_rx(mesh, sizeof(mesh), from, to, air_id,
                                                      ch_idx, hl, hs,
                                                      has ? spub : NULL, data, data_len,
                                                      &rxmeta);
            memset(spub, 0, sizeof(spub));
            if (ml) {
                extern bool lora_phoneapi_push_mesh_raw(const uint8_t *mesh, uint16_t ml);
                ok = lora_phoneapi_push_mesh_raw(mesh, ml);
            }
        } else {
            ok = lora_phoneapi_push_mesh_data_rx(from, to, air_id, ch_idx, hl, hs,
                                                 data, data_len, &rxmeta);
        }
        if (ok) lora_ble_notify_from_num();
        }
        // Traceroute destined to us (port 70): reply with route_back
        // (minimal viable: opaque echo + our ID appended).
        if (port == 70 && to == lora_mesh_node_num() &&
            from != lora_mesh_node_num()) {
            const uint8_t *tpay = NULL;
            uint16_t tpay_len = 0;
            {
                pb_r_t drr;
                pb_r_init(&drr, data, data_len);
                uint8_t df2, dw2;
                uint32_t dv2;
                const uint8_t *db2;
                uint16_t dl2;
                while (pb_r_next(&drr, &df2, &dw2, &dv2, &db2, &dl2)) {
                    if (df2 == 2 && dw2 == 2) { tpay = db2; tpay_len = dl2; break; }
                }
            }
            uint8_t trep[240];
            uint32_t trep_id = 0;
            uint8_t trep_len = lora_mesh_build_traceroute_reply(
                from, air_id ? air_id : request_id, tpay, tpay_len,
                trep, sizeof(trep), &trep_id);
            if (trep_len && lora_duty_allow(trep_len) && !lora_radio_cad() &&
                lora_radio_send(trep, trep_len) == 0) {
                lora_duty_record(trep_len);
                ESP_LOGI(TAG, "mesh traceroute reply to %08x", (unsigned)from);
                // Echo our reply to the phone so the app traceroute screen
                // shows our hop. TX-side plain push (no rx_time); gated on
                // mute/ignore like any phone push (air TX above is kept).
                if (!mgr_sup) {
                    uint32_t rfrom = 0, rto = 0, rid = 0, rreq = 0;
                    uint32_t rhs = 0, rhl = 0;
                    bool rwa = false;
                    uint8_t rport = 0, rch = 0;
                    uint8_t rdata[233];
                    uint16_t rdata_len = 0;
                    if (lora_mesh_decode_data_ch(trep, trep_len, &rfrom, &rto, &rid,
                                                 &rwa, &rport, &rreq, &rch, &rhs, &rhl,
                                                 rdata, sizeof(rdata), &rdata_len) &&
                        rdata_len) {
                        if (lora_phoneapi_push_mesh_data(
                                rfrom ? rfrom : lora_mesh_node_num(),
                                rto ? rto : from,
                                rid ? rid : trep_id, rdata, rdata_len))
                            lora_ble_notify_from_num();
                    }
                }
            }
        }
    }
    if (decoded && port == 5 && request_id != 0 &&
        to == lora_mesh_node_num() && from != lora_mesh_node_num()) {
        // Forward the real over-air Routing ACK to the app. Its Data.request_id
        // is the original app packet id, which drives the delivered state.
        // It also clears the matching reliable entry. Successful replies can
        // teach the immediate relay next hop; NAKs never do.
        bool fwd = false;
        if (!delivery_duplicate && is_pki) {
            uint8_t spub[32];
            if (lora_mesh_peer_pubkey(from, spub)) {
                fwd = lora_phoneapi_push_mesh_data_pki_rx(from, to, air_id, ch_idx,
                                                          hl, hs, spub, data, data_len,
                                                          &rxmeta);
                memset(spub, 0, sizeof(spub));
            }
        } else if (!delivery_duplicate) {
            fwd = lora_phoneapi_push_mesh_data_rx(from, to, air_id, ch_idx, hl, hs,
                                                  data, data_len, &rxmeta);
        }
        uint32_t rerr = 0;
        {
            pb_r_t dr; pb_r_init(&dr, data, data_len);
            uint8_t rf, rw; uint32_t rv; const uint8_t *rb; uint16_t rl;
            while (pb_r_next(&dr, &rf, &rw, &rv, &rb, &rl)) {
                if (rf == 2 && rw == 2) {
                    pb_r_t rr; pb_r_init(&rr, rb, rl);
                    uint8_t qf, qw; uint32_t qv; const uint8_t *qb; uint16_t ql;
                    while (pb_r_next(&rr, &qf, &qw, &qv, &qb, &ql))
                        if (qf == 3 && qw == 0) { rerr = qv; break; }
                    break;
                }
            }
            ESP_LOGI(TAG, "mesh routing %s from %08x for %08x (err=%u)",
                     rerr == 0 ? "ACK" : "NAK", (unsigned)from, (unsigned)request_id, (unsigned)rerr);
        }
        if (rerr == LORA_ROUTING_PKI_UNKNOWN_PUBKEY) {
            ESP_LOGI(TAG, "PKI peer %08x lacks our key; queueing direct NodeInfo",
                     (unsigned)from);
            lora_mesh_request_nodeinfo_to(from, ch_idx);
        }
        lora_manager_chat_result(from, request_id, rerr == 0);
        lora_mesh_reliable_on_air_ack(request_id, from,
                                      rerr == 0 && len >= 16 ? payload[15] : 0);
        if (fwd) {
            lora_ble_notify_from_num();
        }
    }
    if (decoded && port != 5 && request_id != 0 &&
        to == lora_mesh_node_num() && from != lora_mesh_node_num()) {
        // Native treats a correlated module reply like a routing ACK for
        // retransmission and next-hop purposes.
        lora_mesh_reliable_on_air_ack(request_id, from,
                                      len >= 16 ? payload[15] : 0);
    }
    if (decoded && port == 5 && request_id != 0 &&
        to != lora_mesh_node_num() && from != lora_mesh_node_num()) {
        // Intermediate next-hop relayers overhear the end-to-end ACK even
        // though it is addressed to the original sender.
        lora_mesh_reliable_on_air_ack(request_id, from,
                                      len >= 16 ? payload[15] : 0);
    }
    if (decoded && request_id != 0 && to != lora_mesh_node_num() &&
        to != LORA_MESH_BROADCAST) {
        // ACK/reply for somebody else's DM: native cancels our queued copy
        // of that already-completed original transmission.
        lora_mesh_cancel_rebroadcast(to, request_id);
    }
    if (decoded && from == lora_mesh_node_num() && port != 5 &&
        to == LORA_MESH_BROADCAST && len >= 16 && payload[15] != 0 &&
        pending_ack_take(air_id)) {
        // Hearing a relayed copy of our broadcast is Meshtastic's implicit
        // ACK. Convert it to the local Routing ACK the phone expects.
        lora_mesh_reliable_on_heard(from, air_id);
        static const uint8_t routing_ok[] = {0x18, 0x00};
        uint8_t ack_data[16];
        uint16_t ack_data_len = pb_build_data_msg(ack_data, sizeof(ack_data), 5,
                                                  routing_ok, sizeof(routing_ok), air_id);
        uint32_t ack_id = 0;
        while (ack_id == 0) ack_id = esp_random();
        if (ack_data_len && lora_phoneapi_push_mesh_data(from, from, ack_id,
                                                         ack_data, ack_data_len)) {
            ESP_LOGI(TAG, "mesh implicit ACK via relay %02x for %08x",
                     payload[15], (unsigned)air_id);
            lora_ble_notify_from_num();
        }
    }
    // Implicit ACK for originator and intermediate queues: a later relay
    // copy with the same global (from,id) stops its retry.
    if (decoded && len >= 16)
        lora_mesh_reliable_on_heard(from, air_id);
    if (!decoded && len >= 16) {
        // Undecryptable want_ack addressed to us: NAK over air so the sender
        // stops retrying. NO_CHANNEL when we hold no key for the hash;
        // PKI_UNKNOWN_PUBKEY for a PKI DM (channel 0) without the peer key.
        uint32_t hto = 0, hfrom = 0, hid = 0;
        uint8_t hhl = 0, hhs = 0, hch = 0;
        bool hwa = false;
        uint32_t t = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8) |
                     ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
        uint32_t fr = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
                      ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24);
        hid = (uint32_t)payload[8] | ((uint32_t)payload[9] << 8) |
              ((uint32_t)payload[10] << 16) | ((uint32_t)payload[11] << 24);
        hhl = payload[12] & 0x07; hhs = (payload[12] >> 5) & 0x07;
        hwa = (payload[12] & 0x08) != 0; hch = payload[13];
        hto = t; hfrom = fr;
        (void)hhl;
        if (hto == lora_mesh_node_num() && hfrom != lora_mesh_node_num() && hwa && hid) {
            uint8_t err = LORA_ROUTING_NO_CHANNEL;
            if (hch == 0) {
                uint8_t pk[32];
                if (!lora_mesh_peer_pubkey(hfrom, pk)) err = LORA_ROUTING_PKI_UNKNOWN_PUBKEY;
                else err = LORA_ROUTING_PKI_FAILED;
                memset(pk, 0, sizeof(pk));
            }
            uint8_t nak_hops = response_hop_limit(hhs, hhl);
            uint8_t nak[240];
            uint8_t nak_len = lora_mesh_build_routing_nak(hfrom, hid, err, nak_hops,
                                                          nak, sizeof(nak), NULL);
            if (nak_len && lora_duty_allow(nak_len) && !lora_radio_cad() &&
                lora_radio_send(nak, nak_len) == 0) {
                lora_duty_record(nak_len);
                ESP_LOGI(TAG, "mesh routing NAK err=%u to %08x for %08x",
                         (unsigned)err, (unsigned)hfrom, (unsigned)hid);
            }
        }
    }
    if (decoded && want_ack && port != 5 && !module_reply_sent &&
        to == lora_mesh_node_num() && from != lora_mesh_node_num()) {
        // Meshtastic reliable routing ACK: Routing{NONE} with
        // Data.request_id equal to the received MeshPacket.id.
        // Always channel-encrypted (upstream excludes ROUTING_APP from PKI).
        // Hop-aware return path from RoutingModule::getHopLimitForResponse().
        uint8_t ack_hops = response_hop_limit(hs, hl);
        // want_ack-on-ACK for TEXT DMs (reliable DM handshake).
        bool ack_wa = is_pki && port == 1 && want_ack;
        uint8_t ack_frame[240];
        uint8_t ack_len = 0;
        ack_len = lora_mesh_build_routing_ack_hops(from, air_id, ack_hops, ack_wa,
                                                   ack_frame, sizeof(ack_frame), NULL);
        if (ack_len && lora_duty_allow(ack_len) && !lora_radio_cad() &&
            lora_radio_send(ack_frame, ack_len) == 0) {
            lora_duty_record(ack_len);
            ESP_LOGI(TAG, "mesh routing ACK sent to %08x for %08x",
                     (unsigned)from, (unsigned)air_id);
        }
    }
    // Queue the relay before TX. This makes native duplicate cancellation
    // real: another node's copy can cancel ours during the SNR-weighted
    // contention window. The 5Hz mesh tick performs duty/CAD-gated TX.
    (void)lora_mesh_schedule_rebroadcast(payload, len, snr);
}

void lora_manager_early_init(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
#if defined(CONFIG_SPIRAM)
    if (!s_ring) {
        s_ring = heap_caps_calloc(LORA_MSG_RING, sizeof(*s_ring),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            s_ring = heap_caps_calloc(LORA_MSG_RING, sizeof(*s_ring),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            ESP_LOGW(TAG, "PSRAM chat-ring allocation failed; using internal RAM");
        }
    }
#endif
    if (!s_ring) {
        set_err("chat storage unavailable");
        ESP_LOGE(TAG, "LoRa chat-ring allocation failed");
        return;
    }
    nvs_load();
    lora_mesh_init();
    lora_mesh_set_role(s_role);
    lora_pki_init();
    lora_store_init();
    lora_phoneapi_boot();
    set_err("none");
    ESP_LOGI(TAG, "LoRa ready (region=%s %s sf=%d bw=%d cr=4/%d tx=%d comp=%s node=%06X) — radio off until `lora start`",
             lora_region_name((int)s_region),
             s_modem.use_preset ? lora_preset_display_name(s_modem.preset, true) : "custom",
             s_sf, s_bw_khz, s_cr, s_tx_dbm,
             s_companion == LORA_COMPANION_WIFI ? "wifi" : "ble",
             (unsigned)(lora_mesh_node_num() & 0xFFFFFF));
}

/* app_main has a deliberately small ESP-IDF startup stack.  LoRa startup
 * walks several NVS-backed stores and initializes PKI, so keep that nested
 * call chain off the main task.  The task deletes itself after signalling
 * completion, returning its stack allocation to the heap. */
#define LORA_EARLY_INIT_TASK_STACK 4096
static StaticSemaphore_t s_early_init_sem_buf;
static SemaphoreHandle_t s_early_init_sem;

static void lora_early_init_task(void *arg) {
    SemaphoreHandle_t done = (SemaphoreHandle_t)arg;
    lora_manager_early_init();
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

bool lora_manager_early_init_off_main(void) {
    s_early_init_sem = xSemaphoreCreateBinaryStatic(&s_early_init_sem_buf);
    if (!s_early_init_sem) {
        ESP_LOGW(TAG, "LoRa init semaphore unavailable; running on main task");
        lora_manager_early_init();
        return false;
    }
    BaseType_t rc = xTaskCreate(lora_early_init_task, "LoRaInit",
                                LORA_EARLY_INIT_TASK_STACK, s_early_init_sem,
                                tskIDLE_PRIORITY + 1, NULL);
    if (rc != pdPASS) {
        ESP_LOGW(TAG, "LoRa init task unavailable; running on main task");
        lora_manager_early_init();
        return false;
    }
    xSemaphoreTake(s_early_init_sem, portMAX_DELAY);
    return true;
}

bool lora_manager_start(void) {
    if (s_running) return true;
    lora_mesh_routing_reset();
    if (!s_region_saved) {
        // No silent TX on a default band plan: the CLI prints the guided
        // questionnaire (see lora_manager_setup_text) and refuses.
        set_err("region not set");
        ESP_LOGW(TAG, "LoRa start refused: region not explicitly saved");
        return false;
    }
    lora_hw_t hw;
    if (!lora_manager_get_hw(&hw)) return false;
    if (s_tx_dbm > hw.max_tx_dbm) {
        s_tx_dbm = hw.max_tx_dbm;
    }
    int esf = 11, ebw = 250, ecr = 5;
    if (!lora_modem_effective(&s_modem, &esf, &ebw, &ecr)) {
        set_err("bad modem");
        return false;
    }
    s_sf = esf;
    s_bw_khz = ebw;
    s_cr = ecr;
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    lora_duty_set_region((int)s_region);
    lora_mesh_set_modem(esf, ebw, ecr);
    lora_mesh_set_hop_limit((uint8_t)s_hop_limit);
    lora_channels_set_default_name(lora_preset_display_name(s_modem.preset, s_modem.use_preset));
    if (!s_modem.tx_enabled) {
        set_err("tx disabled");
        ESP_LOGW(TAG, "LoRa start refused: tx_enabled=false (app set_config)");
        return false;
    }
    if (s_modem.use_preset && (s_modem.preset != 0)) {
        ESP_LOGW(TAG, "preset %s SF%d/BW%d/CR4/%d: only same-preset nodes hear you",
                 lora_preset_display_name(s_modem.preset, true), esf, ebw, ecr);
    } else if (!s_modem.use_preset) {
        ESP_LOGW(TAG, "custom modem SF%d/BW%d/CR4/%d: stock nodes won't hear you",
                 esf, ebw, ecr);
    }
    if (!lora_crypto_init()) {
        set_err("crypto self-test failed");
        ESP_LOGE(TAG, "LoRa crypto self-test failed; refusing air interop");
        return false;
    }
    if (lora_radio_init_ex(&hw, s_freq_hz, esf, ebw, s_tx_dbm, ecr) != 0) {
        set_err(lora_radio_step());
        ESP_LOGW(TAG, "LoRa radio init failed at stage '%s'", lora_radio_step());
        return false;
    }
    if (lora_radio_start_rx(lora_rx_cb, NULL) != 0) {
        set_err(lora_radio_step());
        ESP_LOGW(TAG, "LoRa RX start failed at stage '%s'", lora_radio_step());
        lora_radio_deinit();
        return false;
    }
    s_present = true;
    s_running = true;
    set_err("none");
    ESP_LOGI(TAG, "LoRa started (%u Hz SF%d BW%d CR4/%d TX%d)", (unsigned)s_freq_hz, esf, ebw, ecr, s_tx_dbm);
    // Companion link follows the persisted mode (no-PSRAM builds allow
    // WiFi XOR BLE — companion picks which one rides along).
    if (s_companion == LORA_COMPANION_BLE) {
        if (!lora_ble_start()) {
            ESP_LOGW(TAG, "LoRa BLE app link refused (stop BLE scans first); radio keeps running");
        }
    }
    return true;
}

void lora_manager_stop(void) {
    lora_ble_stop();
    lora_radio_stop();
    lora_radio_deinit();
    lora_mesh_routing_reset();
    s_running = false;
}

bool lora_manager_is_running(void) { return s_running; }
static TaskHandle_t s_apply_task;
static lora_region_t s_apply_region;
static int s_apply_tx;
static lora_modem_cfg_t s_apply_modem;
static int s_apply_hop;

static void apply_radio_task(void *arg) {
    (void)arg;
    // Let the phone consume its routing response before changing the radio.
    vTaskDelay(pdMS_TO_TICKS(1000));
    bool restart = s_running;
    s_running = false;
    lora_radio_stop();
    lora_radio_deinit();
    lora_mesh_routing_reset();
    s_region = s_apply_region;
    s_tx_dbm = s_apply_tx;
    s_hop_limit = s_apply_hop;
    s_modem = s_apply_modem;
    lora_mesh_set_hop_limit((uint8_t)s_hop_limit);
    int esf = 11, ebw = 250, ecr = 5;
    if (lora_modem_effective(&s_modem, &esf, &ebw, &ecr)) {
        s_sf = esf;
        s_bw_khz = ebw;
        s_cr = ecr;
        lora_mesh_set_modem(esf, ebw, ecr);
    }
    lora_channels_set_default_name(lora_preset_display_name(s_modem.preset, s_modem.use_preset));
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save_region();
    nvs_save();
    if (restart && s_modem.tx_enabled) {
        lora_hw_t hw;
        if (lora_manager_get_hw(&hw) && lora_radio_init_ex(&hw, s_freq_hz, s_sf, s_bw_khz, s_tx_dbm, s_cr) == 0 &&
            lora_radio_start_rx(lora_rx_cb, NULL) == 0) {
            s_running = true;
            set_err("none");
            ESP_LOGI(TAG, "LoRa radio reconfigured (%u Hz SF%d BW%d CR4/%d TX%d)",
                     (unsigned)s_freq_hz, s_sf, s_bw_khz, s_cr, s_tx_dbm);
        } else {
            set_err(lora_radio_step());
            ESP_LOGW(TAG, "LoRa radio reconfigure failed at '%s'", lora_radio_step());
        }
    } else if (restart && !s_modem.tx_enabled) {
        set_err("tx disabled");
        ESP_LOGW(TAG, "LoRa left stopped: tx_enabled=false");
    }
    s_apply_task = NULL;
    vTaskDelete(NULL);
}

bool lora_manager_apply_app_radio(lora_region_t region, int tx_dbm) {
    return lora_manager_apply_lora_cfg(region, true, 0, 0, 0, 0,
                                       0.0f, 0.0f, 0, true, tx_dbm,
                                       s_hop_limit);
}

// Full LoRaConfig apply from the phone (admin set_config[lora]).
// Validates everything before queueing the deferred reconfigure; returns
// false on validation failure (caller replies BAD_REQUEST).
bool lora_manager_apply_lora_cfg(lora_region_t region, bool use_preset, int preset,
                                 int sf, int bw_khz, int cr,
                                 float freq_offset, float override_freq,
                                 uint32_t channel_num, bool tx_enabled,
                                 int tx_power, int hop_limit) {
    lora_hw_t hw;
    if (s_apply_task || !lora_manager_get_hw(&hw)) return false;
    if (lora_region_name((int)region)[0] == '?') return false;
    if (hop_limit < 1 || hop_limit > 7) return false;
    if (tx_power < 0 || tx_power > hw.max_tx_dbm) return false;
    if (tx_power == 0) tx_power = hw.max_tx_dbm;
    if (tx_power < 2) return false;
    if (use_preset && !lora_preset_is_valid(preset)) return false;
    lora_modem_cfg_t m;
    m.use_preset = use_preset;
    m.preset = use_preset ? preset : s_modem.preset;
    m.sf = use_preset ? s_modem.sf : sf;
    m.bw_khz = use_preset ? s_modem.bw_khz : bw_khz;
    m.cr = (cr >= 5 && cr <= 8) ? cr : (use_preset ? lora_preset_cr(preset) : 5);
    if (!use_preset && !lora_modem_resolve(false, 0, sf, bw_khz, m.cr, NULL, NULL, NULL))
        return false;
    if (freq_offset < -2.0f || freq_offset > 2.0f) return false;
    if (override_freq < 0 || (override_freq > 0.1f &&
        (override_freq < 150.0f || override_freq > 960.0f))) return false;
    if (channel_num > 512) return false;
    m.freq_offset_mhz = freq_offset;
    m.override_freq_mhz = override_freq;
    m.channel_num = channel_num;
    m.tx_enabled = tx_enabled;
    int esf = 0, ebw = 0, ecr = 0;
    if (!lora_modem_effective(&m, &esf, &ebw, &ecr)) return false;
    if (s_region == region && s_tx_dbm == tx_power && s_hop_limit == hop_limit &&
        s_modem.use_preset == m.use_preset && s_modem.preset == m.preset &&
        s_modem.sf == m.sf && s_modem.bw_khz == m.bw_khz && s_modem.cr == m.cr &&
        s_modem.freq_offset_mhz == m.freq_offset_mhz &&
        s_modem.override_freq_mhz == m.override_freq_mhz &&
        s_modem.channel_num == m.channel_num &&
        s_modem.tx_enabled == m.tx_enabled)
        return true;
    s_apply_region = region;
    s_apply_tx = tx_power;
    s_apply_hop = hop_limit;
    s_apply_modem = m;
    return xTaskCreate(apply_radio_task, "lora_config", 4096, NULL, 5, &s_apply_task) == pdPASS;
}

bool lora_manager_set_hop_limit(int hop_limit) {
    if (hop_limit < 1 || hop_limit > 7) { set_err("hop limit must be 1..7"); return false; }
    s_hop_limit = hop_limit;
    lora_mesh_set_hop_limit((uint8_t)hop_limit);
    nvs_save();
    return true;
}
bool lora_manager_is_present(void) { return s_present; }
const char *lora_manager_last_error(void) { return s_last_error; }

bool lora_manager_set_region(lora_region_t region) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (lora_region_name((int)region)[0] == '?') {
        set_err("unknown region");
        return false;
    }
    s_region = region;
    s_freq_hz = lora_air_freq_hz_ex((int)region, &s_modem);
    nvs_save_region();
    return true;
}

bool lora_manager_region_saved(void) { return s_region_saved; }
bool lora_manager_needs_setup(void) { return !s_running && !s_region_saved; }

// Guided first-run text. Serial-friendly: states the one blocking question
// (region: transmitting on the wrong band plan is a legal problem), the safe
// preset defaults that apply unless changed, and the exact fix commands.
bool lora_manager_setup_text(char *out, size_t n) {
    if (!out || n == 0) return false;
    lora_status_t st;
    memset(&st, 0, sizeof(st));
    lora_manager_get_status(&st);
    int w = snprintf(out, n,
        "LoRa setup: region is required before first TX.\n"
        "  Q1 region? (legal TX band; current default %s%s)\n"
        "     lora set region <name>   e.g. lora set region %s\n"
        "     names: us915 eu868 eu433 cn jp anz kr tw ru in nz865 th ua433\n"
        "            my433 my919 sg923 ph433 ph868 ph915 anz433 kz433 kz863 np865 br902\n"
        "  Defaults kept unless changed:\n"
        "     modem %s SF%d/BW%d/CR4/%d, TX %d dBm, hops %d, companion %s\n"
        "     lora set <preset|sf|bw|cr|tx|hop|offset|companion> <val>\n"
        "  Then: lora start\n",
        lora_region_name((int)st.region), s_region_saved ? "" : " (NOT SAVED)",
        lora_region_name((int)st.region),
        st.use_preset ? lora_preset_display_name(st.preset, true) : "custom",
        st.sf, st.bw_khz, st.cr, st.tx_dbm, st.hop_limit,
        st.companion == 1 ? "wifi" : "ble");
    return w > 0 && (size_t)w < n;
}

bool lora_manager_set_params(int sf, int bw_khz, int tx_dbm) {
    // Legacy CLI: maps to custom modem + tx.
    if (s_running) {
        set_err("stop first");
        return false;
    }
    lora_hw_t hw;
    if (!lora_manager_get_hw(&hw)) return false;
    if (sf < 5 || sf > 12) return false;
    if (bw_khz != 125 && bw_khz != 250 && bw_khz != 500) return false;
    if (tx_dbm < 2 || tx_dbm > hw.max_tx_dbm) return false;
    s_modem.use_preset = false;
    s_modem.sf = sf;
    s_modem.bw_khz = bw_khz;
    if (s_modem.cr < 5 || s_modem.cr > 8) s_modem.cr = 5;
    s_tx_dbm = tx_dbm;
    int esf = 0, ebw = 0, ecr = 0;
    if (lora_modem_effective(&s_modem, &esf, &ebw, &ecr)) {
        s_sf = esf;
        s_bw_khz = ebw;
        s_cr = ecr;
    }
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_modem(int sf, int bw_khz, int cr_denom) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (!lora_modem_resolve(false, 0, sf, bw_khz, cr_denom, NULL, NULL, NULL)) {
        set_err("bad modem");
        return false;
    }
    s_modem.use_preset = false;
    s_modem.sf = sf;
    s_modem.bw_khz = bw_khz;
    s_modem.cr = cr_denom;
    s_sf = sf;
    s_bw_khz = bw_khz;
    s_cr = cr_denom;
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_preset(int preset) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (!lora_preset_is_valid(preset)) {
        set_err("bad preset");
        return false;
    }
    s_modem.use_preset = true;
    s_modem.preset = preset;
    int esf = 0, ebw = 0, ecr = 0;
    if (lora_modem_effective(&s_modem, &esf, &ebw, &ecr)) {
        s_sf = esf;
        s_bw_khz = ebw;
        s_cr = ecr;
    }
    lora_channels_set_default_name(lora_preset_display_name(preset, true));
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_custom(int sf, int bw_khz, int cr_denom) {
    return lora_manager_set_modem(sf, bw_khz, cr_denom);
}

bool lora_manager_set_freq_offset(float mhz) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (mhz < -2.0f || mhz > 2.0f) return false;
    s_modem.freq_offset_mhz = mhz;
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_override_freq(float mhz) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (mhz < 0 || (mhz > 0.1f && (mhz < 150.0f || mhz > 960.0f))) return false;
    s_modem.override_freq_mhz = mhz;
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_channel_num(uint32_t num) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    if (num > 512) return false;
    s_modem.channel_num = num;
    s_freq_hz = lora_air_freq_hz_ex((int)s_region, &s_modem);
    nvs_save();
    return true;
}

bool lora_manager_set_tx_enabled(bool en) {
    if (s_running) {
        set_err("stop first");
        return false;
    }
    s_modem.tx_enabled = en;
    nvs_save();
    return true;
}

bool lora_manager_set_role(int role) {
    if (role < 0 || role > 12) return false;
    s_role = role;
    lora_mesh_set_role(role);
    nvs_save();
    return true;
}

int lora_manager_get_role(void) { return s_role; }

bool lora_manager_get_modem_cfg(bool *use_preset, int *preset, int *sf,
                                int *bw_khz, int *cr, float *freq_offset,
                                float *override_freq, uint32_t *channel_num,
                                bool *tx_enabled) {
    if (use_preset) *use_preset = s_modem.use_preset;
    if (preset) *preset = s_modem.preset;
    if (sf) *sf = s_sf;
    if (bw_khz) *bw_khz = s_bw_khz;
    if (cr) *cr = s_cr;
    if (freq_offset) *freq_offset = s_modem.freq_offset_mhz;
    if (override_freq) *override_freq = s_modem.override_freq_mhz;
    if (channel_num) *channel_num = s_modem.channel_num;
    if (tx_enabled) *tx_enabled = s_modem.tx_enabled;
    return true;
}

bool lora_manager_set_channel(uint8_t idx, const char *name, const uint8_t *psk,
                              uint8_t psk_len, uint8_t role, bool uplink, bool downlink) {
    if (idx >= 8) return false;
    if (!lora_channel_set(idx, name, psk, psk_len, role, uplink, downlink)) return false;
    return true;
}

bool lora_manager_disable_channel(uint8_t idx) {
    if (idx >= 8) return false;
    return lora_channel_disable(idx);
}

bool lora_manager_set_companion(lora_companion_t companion) {
    // Takes effect on next start (WiFi XOR BLE on no-PSRAM).
    s_companion = companion;
    nvs_save();
    return true;
}

bool lora_manager_send_text(const char *text) {
    return lora_manager_send_app_text(text, 0xFFFFFFFF, NULL);
}

bool lora_manager_send_app_text(const char *text, uint32_t to, uint32_t *out_id) {
    return lora_manager_send_app_text_ex(text, to, 0, false, out_id);
}

bool lora_manager_send_app_text_ex(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint32_t *out_id) {
    return lora_manager_send_app_text_ch(text, to, packet_id, want_ack,
                                         lora_channel_primary(), out_id);
}

bool lora_manager_send_app_text_ch(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint8_t channel_idx, uint32_t *out_id) {
    if (!s_running) {
        set_err("radio not running (lora start first)");
        ESP_LOGW(TAG, "send refused: radio not running");
        return false;
    }
    if (!s_modem.tx_enabled) {
        set_err("tx disabled");
        return false;
    }
    if (to == 0) {
        set_err("bad dest");
        return false;
    }
    if (to == lora_mesh_node_num()) {
        // sendLocal loopback: deliver straight to the phone, never airs.
        uint32_t lid = packet_id;
        while (lid == 0) lid = esp_random();
        char tmp[160];
        snprintf(tmp, sizeof(tmp), "%s", text ? text : "");
        ring_push(true, "me", tmp, to, false, 0, false);
        lora_phoneapi_push_mesh_text_ch(lora_mesh_node_num(), to, lid,
                                        0.0f, 3, 3, channel_idx, tmp);
        if (lora_phoneapi_has_data()) lora_ble_notify_from_num();
        s_tx_ok++;
        if (out_id) *out_id = lid;
        return true;
    }
    if (channel_idx >= 8) {
        set_err("bad channel");
        return false;
    }
    if (!text || !text[0]) {
        set_err("empty text");
        return false;
    }
    uint8_t frame[240];
    uint32_t air_id = 0;
    uint8_t flen = lora_mesh_build_text_to_id_ch(text, to, packet_id, want_ack,
                                                channel_idx,
                                                frame, sizeof(frame), &air_id);
    if (flen == 0) {
        s_q_drops++;
        set_err("frame build failed");
        ESP_LOGW(TAG, "send refused: frame build failed");
        return false;
    }
    if (lora_radio_is_ready()) {
        if (!lora_duty_allow(flen)) {
            s_tx_fail++;
            set_err("duty cycle");
            // Duty-drop of a want_ack packet: NAK to the phone so the app
            // shows DUTY_CYCLE_LIMIT instead of spinning to timeout.
            if (want_ack) push_routing_nak_to_phone(to, air_id, 9);
            return false;
        }
        if (lora_radio_cad()) {
            s_tx_fail++;
            set_err("channel busy");
            return false;
        }
        if (lora_radio_send(frame, flen) != 0) {
            s_tx_fail++;
            set_err("tx failed");
            return false;
        }
        lora_duty_record(flen);
    }
    if (want_ack && to == LORA_MESH_BROADCAST) pending_ack_track(air_id);
    if (want_ack && to != LORA_MESH_BROADCAST) lora_mesh_reliable_track(frame, flen);
    // Echo locally (dedup drops our own air echo on RX).
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s", text);
    ring_push(true, "me", tmp, to, false, 0, false);
    s_tx_ok++;
    if (out_id) *out_id = air_id;
    return true;
}

bool lora_manager_send_data_ch(uint32_t to, uint8_t portnum,
                               const uint8_t *payload, uint16_t plen,
                               uint32_t request_id, uint32_t packet_id,
                               bool want_ack, bool want_response,
                               uint8_t channel_idx, uint32_t *out_id) {
    if (!s_running || !s_modem.tx_enabled) return false;
    if (channel_idx >= 8 || portnum == 0) return false;
    if (plen > 233) return false;
    if (to == 0) return false; // sendLocal: to==0 is an error
    if (to == lora_mesh_node_num()) {
        // sendLocal loopback: deliver straight to the phone, never airs
        // with dest=self. Position/telemetry-class packets addressed to
        // self are additionally rebroadcast to BROADCAST below.
        if (portnum == 3 && payload && plen)
            lora_mesh_set_self_position(payload, plen);
        uint32_t lid = packet_id;
        while (lid == 0) lid = esp_random();
        uint8_t data[233];
        uint16_t dl = pb_build_data_msg(data, sizeof(data), portnum,
                                        payload, plen, request_id);
        if (dl) {
            uint32_t me = lora_mesh_node_num();
            if (lora_phoneapi_push_mesh_data(me, me, lid, data, dl))
                lora_ble_notify_from_num();
        }
        s_tx_ok++;
        if (out_id) *out_id = lid;
        if (portnum == 3 || portnum == 8 || portnum == 67 ||
            portnum == 70 || portnum == 71) {
            // Own position/module status: put BROADCAST (not self) on air.
            uint8_t frame[240];
            uint32_t air_id = 0;
            uint8_t flen = lora_mesh_build_data_ch(
                LORA_MESH_BROADCAST, portnum, payload, plen, request_id,
                packet_id, want_ack, want_response, channel_idx,
                frame, sizeof(frame), &air_id);
            if (flen) lora_manager_air_send(frame, flen);
        }
        return true;
    }
    uint8_t frame[240];
    uint32_t air_id = 0;
    uint8_t flen = lora_mesh_build_data_ch(to, portnum, payload, plen,
                                          request_id, packet_id,
                                          want_ack, want_response,
                                          channel_idx,
                                          frame, sizeof(frame), &air_id);
    if (flen == 0) {
        s_q_drops++;
        return false;
    }
    if (!lora_manager_air_send(frame, flen)) {
        // Duty NAK already emitted inside air_send when applicable.
        return false;
    }
    if (want_ack && to != LORA_MESH_BROADCAST) lora_mesh_reliable_track(frame, flen);
    s_tx_ok++;
    if (out_id) *out_id = air_id;
    return true;
}

bool lora_manager_air_send(const uint8_t *frame, uint8_t len) {
    if (!s_running || !s_modem.tx_enabled) return false;
    if (!frame || len == 0 || len > 240) return false;
    // Never put dest=self (or 0) on air. The phone's verbatim Data path
    // hands us position/telemetry addressed to self: store the position
    // for NodeInfo and rebroadcast the identical frame to BROADCAST.
    {
        uint32_t hto = 0;
        if (air_hdr_parse(frame, len, &hto, NULL, NULL) &&
            (hto == lora_mesh_node_num() || hto == 0)) {
            uint32_t dfrom = 0, dto = 0, did = 0, dreq = 0;
            bool dwa = false;
            uint8_t dport = 0, dch = 0;
            uint32_t dhs = 0, dhl = 0;
            uint8_t ddata[233];
            uint16_t ddata_len = 0;
            uint32_t me = lora_mesh_node_num();
            bool dec = lora_mesh_decode_data_ch(
                frame, len, &dfrom, &dto, &did, &dwa, &dport, &dreq,
                &dch, &dhs, &dhl, ddata, sizeof(ddata), &ddata_len);
            if (dec && (dport == 3 || dport == 8 || dport == 67 ||
                        dport == 70 || dport == 71)) {
                if (dport == 3 && ddata_len) {
                    pb_r_t dr;
                    pb_r_init(&dr, ddata, ddata_len);
                    uint8_t df, dw;
                    uint32_t dv;
                    const uint8_t *db;
                    uint16_t dl;
                    while (pb_r_next(&dr, &df, &dw, &dv, &db, &dl)) {
                        if (df == 2 && dw == 2 && db && dl) {
                            lora_mesh_set_self_position(db, dl);
                            break;
                        }
                    }
                }
                ESP_LOGI(TAG, "self position/module port=%u rebroadcast to air",
                         (unsigned)dport);
                uint8_t bcast[240];
                memcpy(bcast, frame, len);
                bcast[0] = bcast[1] = bcast[2] = bcast[3] = 0xFF;
                bcast[12] &= (uint8_t)~0x08; // broadcast: no WANT_ACK on air
                if (!lora_radio_is_ready()) return true;
                if (!lora_duty_allow(len)) {
                    s_tx_fail++;
                    set_err("duty cycle");
                    return false;
                }
                if (lora_radio_cad()) {
                    s_tx_fail++;
                    set_err("channel busy");
                    return false;
                }
                if (lora_radio_send(bcast, len) != 0) {
                    s_tx_fail++;
                    set_err("tx failed");
                    return false;
                }
                lora_duty_record(len);
                return true;
            }
            // Non-module to-self: loopback only, never air.
            if (dec && ddata_len) {
                uint32_t lid = did;
                while (lid == 0) lid = esp_random();
                if (lora_phoneapi_push_mesh_data(me, me, lid, ddata, ddata_len))
                    lora_ble_notify_from_num();
            }
            return true;
        }
    }
    if (!lora_radio_is_ready()) return true; // radio off in tests: count ok
    if (!lora_duty_allow(len)) {
        s_tx_fail++;
        set_err("duty cycle");
        uint32_t hto = 0, hid = 0;
        bool hwa = false;
        if (air_hdr_parse(frame, len, &hto, &hid, &hwa) && hwa)
            push_routing_nak_to_phone(hto, hid, 9);
        return false;
    }
    if (lora_radio_cad()) {
        s_tx_fail++;
        set_err("channel busy");
        return false;
    }
    if (lora_radio_send(frame, len) != 0) {
        s_tx_fail++;
        set_err("tx failed");
        return false;
    }
    lora_duty_record(len);
    return true;
}

bool lora_manager_send_data_verbatim_ch(uint32_t to,
                                        const uint8_t *data, uint16_t dlen,
                                        uint32_t packet_id, bool want_ack,
                                        uint8_t channel_idx,
                                        uint32_t *out_id) {
    if (!s_running || !s_modem.tx_enabled) return false;
    uint8_t frame[240];
    uint32_t air_id = 0;
    uint8_t flen = lora_mesh_build_data_verbatim_ch(
        to, data, dlen, packet_id, want_ack, channel_idx,
        frame, sizeof(frame), &air_id);
    if (!flen) {
        s_q_drops++;
        set_err("verbatim data build failed");
        return false;
    }
    if (!lora_manager_air_send(frame, flen)) return false;
    if (want_ack && to != LORA_MESH_BROADCAST)
        lora_mesh_reliable_track(frame, flen);
    s_tx_ok++;
    if (out_id) *out_id = air_id;
    return true;
}

bool lora_manager_send_dm_text(const char *text, uint32_t to,
                               uint32_t packet_id, bool want_ack,
                               uint32_t *out_id) {
    if (!s_running || !s_modem.tx_enabled) return false;
    if (to == LORA_MESH_BROADCAST || to == 0 || !text || !text[0]) {
        set_err("dm needs unicast + text");
        return false;
    }
    uint8_t peer[32];
    if (!lora_mesh_peer_pubkey(to, peer)) {
        set_err("no pubkey (need NodeInfo first)");
        ESP_LOGW(TAG, "DM refused: no public key for %08x", (unsigned)to);
        return false;
    }
    memset(peer, 0, sizeof(peer));
    uint8_t frame[252];
    uint32_t air_id = 0;
    uint8_t flen = lora_mesh_build_dm_text(text, to, packet_id, want_ack,
                                           frame, sizeof(frame), &air_id);
    if (flen == 0) {
        s_q_drops++;
        set_err("dm build failed");
        return false;
    }
    if (!lora_manager_air_send(frame, flen)) return false; // duty NAK inside air_send
    if (want_ack) lora_mesh_reliable_track(frame, flen);
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s", text);
    ring_push(true, "dm:me", tmp, to, true, air_id, want_ack);
    s_tx_ok++;
    if (out_id) *out_id = air_id;
    return true;
}

bool lora_manager_send_dm_data(const uint8_t *data, uint16_t dlen,
                               uint32_t to, uint32_t packet_id,
                               bool want_ack, uint32_t *out_id) {
    if (!s_running || !s_modem.tx_enabled) return false;
    if (to == LORA_MESH_BROADCAST || to == 0 || !data || dlen == 0) {
        set_err("dm needs unicast + data");
        return false;
    }
    uint8_t frame[240];
    uint32_t air_id = 0;
    uint8_t flen = lora_mesh_build_dm_data(to, data, dlen, packet_id, want_ack,
                                           frame, sizeof(frame), &air_id);
    if (flen == 0) {
        s_q_drops++;
        set_err("dm data build failed (key/size)");
        return false;
    }
    if (!lora_manager_air_send(frame, flen)) return false;
    if (want_ack) lora_mesh_reliable_track(frame, flen);
    s_tx_ok++;
    if (out_id) *out_id = air_id;
    return true;
}

uint16_t lora_manager_msg_count(void) {
    if (!s_lock) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t count = s_ring_count;
    xSemaphoreGive(s_lock);
    return count;
}

bool lora_manager_msg_at(uint16_t index, lora_msg_t *out) {
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = index < s_ring_count;
    if (found) {
        uint16_t oldest = (uint16_t)((s_ring_head + LORA_MSG_RING - s_ring_count) %
                                     LORA_MSG_RING);
        *out = s_ring[(oldest + index) % LORA_MSG_RING];
    }
    xSemaphoreGive(s_lock);
    return found;
}

bool lora_manager_latest_message(lora_msg_t *out, uint32_t *out_seq) {
    if (!out || !out_seq || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = s_ring_count != 0;
    if (found) {
        uint16_t index = (uint16_t)((s_ring_head + LORA_MSG_RING - 1u) %
                                    LORA_MSG_RING);
        *out = s_ring[index];
        *out_seq = s_seq;
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint16_t lora_manager_msg_since(uint32_t *io_seq, lora_msg_t *out, uint16_t max) {
    if (!io_seq || !out || max == 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t since = *io_seq;
    if (since == 0 && s_seq > s_ring_count) {
        since = s_seq - s_ring_count; // snap to oldest retained
    }
    uint16_t copied = 0;
    // Oldest retained seq:
    uint32_t oldest = (s_seq > s_ring_count) ? (s_seq - s_ring_count) : 0;
    for (uint32_t seq = (since > oldest ? since : oldest); seq < s_seq && copied < max; seq++) {
        // seq 0-based: ring index of seq = (head - count) + (seq - oldest)
        uint32_t order = seq - oldest;
        uint16_t idx = (uint16_t)((s_ring_head + LORA_MSG_RING - s_ring_count + order) % LORA_MSG_RING);
        out[copied++] = s_ring[idx];
    }
    *io_seq = s_seq;
    xSemaphoreGive(s_lock);
    return copied;
}

bool lora_manager_latest_incoming(lora_msg_t *out, uint32_t *out_seq) {
    if (!out || !out_seq || !s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool found = false;
    for (uint16_t age = 0; age < s_ring_count; age++) {
        uint16_t idx = (uint16_t)((s_ring_head + LORA_MSG_RING - 1u - age) % LORA_MSG_RING);
        if (!s_ring[idx].outgoing) {
            *out = s_ring[idx];
            *out_seq = s_seq - age;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

uint16_t lora_manager_node_count(void) { return lora_mesh_nodes(NULL, 0); }

void lora_manager_get_status(lora_status_t *out) {
    if (!out) return;
    out->running = s_running;
    out->radio_present = s_present;
    out->region = s_region;
    out->freq_hz = s_freq_hz;
    out->sf = s_sf;
    out->bw_khz = s_bw_khz;
    out->cr = s_cr;
    out->use_preset = s_modem.use_preset;
    out->preset = s_modem.preset;
    out->freq_offset_mhz = s_modem.freq_offset_mhz;
    out->override_freq_mhz = s_modem.override_freq_mhz;
    out->channel_num = s_modem.channel_num;
    out->tx_enabled = s_modem.tx_enabled;
    out->role = s_role;
    out->tx_dbm = s_tx_dbm;
    out->hop_limit = s_hop_limit;
    out->companion = s_companion;
    out->tx_ok = s_tx_ok;
    out->tx_fail = s_tx_fail + lora_mesh_relay_failed();
    out->tx_relay = s_tx_relay + lora_mesh_relay_sent();
    out->rx_ok = s_rx_ok;
    out->rx_crc_err = 0; // driver drops CRC frames pre-callback; counted in Phase 3 stats
    out->rx_dups = lora_mesh_dups();
    out->q_drops = s_q_drops;
    out->duty_drops = lora_duty_drops();
    out->last_rssi = s_last_rssi;
    out->last_snr = s_last_snr;
    out->node_count = lora_mesh_nodes(NULL, 0);
}

#endif // CONFIG_HAS_LORA
