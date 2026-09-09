// lora_crypto.c
// See lora_crypto.h for the reference. AES via IDF mbedTLS; CTR keystream
// built manually from ECB so behavior is identical across mbedTLS 2.x/3.x:
// keystream block = AES(key, nonce[12] || counter_BE[4]), counter from 0.

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS // TF-PSA hides AES decls without this
#include "managers/lora_crypto.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_log.h"
#include "mbedtls/private/aes.h"
#include <string.h>

static const char *TAG = "LoRaCrypto";

const uint8_t LORA_DEFAULT_KEY[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static mbedtls_aes_context s_aes;
static bool s_ready = false;

uint8_t lora_channel_hash(const char *name, const uint8_t *key, size_t key_len) {
    uint8_t h = 0;
    if (name) {
        for (const char *p = name; *p; p++) h ^= (uint8_t)*p;
    }
    if (key) {
        for (size_t i = 0; i < key_len; i++) h ^= key[i];
    }
    return h;
}

void lora_crypto_crypt(uint32_t from, uint64_t packet_id,
                       uint8_t *data, size_t len) {
    lora_crypto_crypt_key(LORA_DEFAULT_KEY, 16, from, packet_id, data, len);
}

void lora_crypto_crypt_key(const uint8_t *key, uint8_t key_len,
                           uint32_t from, uint64_t packet_id,
                           uint8_t *data, size_t len) {
    if (!s_ready || !data || len == 0) return;
    if (!key || (key_len != 16 && key_len != 32)) {
        if (key_len == 0) return; // unencrypted channel
        return;
    }
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int rc = mbedtls_aes_setkey_enc(&aes, key, key_len == 32 ? 256 : 128);
    if (rc != 0) {
        mbedtls_aes_free(&aes);
        return;
    }
    // initNonce byte-exact (CryptoEngine.cpp): [packetId u64 LE][from u32 LE]
    // at [0..7]/[8..11]; bytes [12..15] are the BE block counter from 0.
    // (The PKI extraNonce-overwrites-[4..7] rule lives in lora_pki.c
    // build_nonce; channel frames have no extraNonce.)
    uint8_t nonce[16] = {0};
    nonce[0] = (uint8_t)packet_id; // u64 LE, explicit (all our targets are LE)
    nonce[1] = (uint8_t)(packet_id >> 8);
    nonce[2] = (uint8_t)(packet_id >> 16);
    nonce[3] = (uint8_t)(packet_id >> 24);
    nonce[4] = (uint8_t)(packet_id >> 32);
    nonce[5] = (uint8_t)(packet_id >> 40);
    nonce[6] = (uint8_t)(packet_id >> 48);
    nonce[7] = (uint8_t)(packet_id >> 56);
    nonce[8] = (uint8_t)from;
    nonce[9] = (uint8_t)(from >> 8);
    nonce[10] = (uint8_t)(from >> 16);
    nonce[11] = (uint8_t)(from >> 24);
    // nonce[12..15] = BE counter, starts at 0.
    uint8_t keystream[16];
    uint8_t ctr[16];
    memcpy(ctr, nonce, 16);
    size_t off = 0;
    uint32_t counter = 0;
    while (off < len) {
        ctr[12] = (uint8_t)(counter >> 24);
        ctr[13] = (uint8_t)(counter >> 16);
        ctr[14] = (uint8_t)(counter >> 8);
        ctr[15] = (uint8_t)counter;
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, ctr, keystream);
        size_t n = len - off;
        if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++) data[off + i] ^= keystream[i];
        off += n;
        counter++;
    }
    mbedtls_aes_free(&aes);
}

bool lora_crypto_init(void) {
    mbedtls_aes_init(&s_aes);
    if (mbedtls_aes_setkey_enc(&s_aes, LORA_DEFAULT_KEY, 128) != 0) {
        return false;
    }
    s_ready = true;
    // Loopback self-test: encrypt twice must match, decrypt must restore.
    // (Catches port bugs; stock interop is proven by HIL, not this.)
    uint8_t a[64], b[64];
    for (int i = 0; i < (int)sizeof(a); i++) a[i] = (uint8_t)(i * 7 + 3);
    memcpy(b, a, sizeof(a));
    lora_crypto_crypt(0x12345678, 0x1122334455667788ULL, a, sizeof(a));
    uint8_t c[64];
    memcpy(c, a, sizeof(a));
    lora_crypto_crypt(0x12345678, 0x1122334455667788ULL, a, sizeof(a));
    if (memcmp(a, b, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "self-test round-trip FAILED");
        s_ready = false;
        return false;
    }
    // Ciphertext must differ from plaintext (key actually applied).
    if (memcmp(c, b, sizeof(c)) == 0) {
        ESP_LOGE(TAG, "self-test keystream FAILED");
        s_ready = false;
        return false;
    }
    ESP_LOGI(TAG, "AES-CTR default-key ready (chash=0x%02x)",
             lora_channel_hash(LORA_DEFAULT_PRESET_NAME, LORA_DEFAULT_KEY, 16));
    return true;
}

#else
typedef int lora_crypto_stub_guard;
#endif
