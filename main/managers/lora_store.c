// lora_store.c
// See lora_store.h.

#include "managers/lora_store.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static bool s_init = false;

static void key_cfg(char *out, size_t n, uint8_t section) {
    snprintf(out, n, "cfg%u", (unsigned)section);
}

static void key_mod(char *out, size_t n, uint8_t module) {
    snprintf(out, n, "mod%u", (unsigned)module);
}

void lora_store_init(void) {
    s_init = true;
}

uint16_t lora_store_cfg_get(uint8_t section, uint8_t *out, uint16_t cap) {
    if (section < 1 || section > LORA_CFG_SECTIONS || !out || cap == 0) return 0;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return 0;
    char k[8];
    key_cfg(k, sizeof(k), section);
    size_t n = cap;
    bool ok = (nvs_get_blob(h, k, out, &n) == ESP_OK);
    nvs_close(h);
    return ok ? (uint16_t)n : 0;
}

bool lora_store_cfg_set(uint8_t section, const uint8_t *data, uint16_t len) {
    if (section < 1 || section > LORA_CFG_SECTIONS) return false;
    if (len > LORA_STORE_BLOB_MAX) return false;
    // Section 6 (lora) is owned by the modem manager; don't shadow it here.
    if (section == 6) return true;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return false;
    char k[8];
    key_cfg(k, sizeof(k), section);
    bool ok;
    if (!data || len == 0) {
        ok = (nvs_erase_key(h, k) == ESP_OK || true);
    } else {
        ok = (nvs_set_blob(h, k, data, len) == ESP_OK);
    }
    nvs_commit(h);
    nvs_close(h);
    return ok;
}

uint16_t lora_store_mod_get(uint8_t module, uint8_t *out, uint16_t cap) {
    if (module < 1 || module > LORA_MOD_SLOTS || !out || cap == 0) return 0;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READONLY, &h) != ESP_OK) return 0;
    char k[8];
    key_mod(k, sizeof(k), module);
    size_t n = cap;
    bool ok = (nvs_get_blob(h, k, out, &n) == ESP_OK);
    nvs_close(h);
    return ok ? (uint16_t)n : 0;
}

bool lora_store_mod_set(uint8_t module, const uint8_t *data, uint16_t len) {
    if (module < 1 || module > LORA_MOD_SLOTS) return false;
    if (len > LORA_STORE_BLOB_MAX) return false;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return false;
    char k[8];
    key_mod(k, sizeof(k), module);
    bool ok;
    if (!data || len == 0) {
        ok = true;
        nvs_erase_key(h, k);
    } else {
        ok = (nvs_set_blob(h, k, data, len) == ESP_OK);
    }
    nvs_commit(h);
    nvs_close(h);
    return ok;
}

#else
typedef int lora_store_stub_guard;
#endif
