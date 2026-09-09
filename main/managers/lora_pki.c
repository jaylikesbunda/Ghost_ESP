// lora_pki.c
// Meshtastic PKI direct messages. See lora_pki.h for the reference.
// X25519 via vendored curve25519-donna (Google/BSD, DJB-derived);
// SHA256 + AES-256-CCM (8B tag) via IDF mbedTLS.

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS // TF-PSA hides decls without this
#include "managers/lora_pki.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/private/ccm.h"
#include "mbedtls/private/sha256.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <string.h>

// Vendored scalarmult (lora_x25519_donna.c, BSD/Google).
int curve25519_donna(unsigned char *mypublic, const unsigned char *secret,
                     const unsigned char *basepoint);

static const char *TAG = "LoRaPKI";

static uint8_t s_priv[32];
static uint8_t s_pub[32];
static bool s_have = false;
#define LORA_PKI_ADMINS_MAX 3
static uint8_t s_admin[LORA_PKI_ADMINS_MAX][32];
static uint8_t s_admin_n = 0;

static bool all_zero(const uint8_t *p, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

static void admin_load(void) {
    nvs_handle_t h = 0;
    size_t n = sizeof(s_admin);
    s_admin_n = 0;
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "pki_admin", s_admin, &n) == ESP_OK)
            s_admin_n = (uint8_t)(n / 32);
        nvs_close(h);
    }
}

uint8_t lora_pki_admin_count(void) { return s_admin_n; }

bool lora_pki_admin_get(uint8_t idx, uint8_t *out32) {
    if (idx >= s_admin_n || !out32) return false;
    memcpy(out32, s_admin[idx], 32);
    return true;
}

bool lora_pki_admin_set(const uint8_t *keys, uint8_t count) {
    if (count > LORA_PKI_ADMINS_MAX) return false;
    if (count && !keys) return false;
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok;
    if (count == 0) {
        ok = (nvs_erase_key(h, "pki_admin") == ESP_OK);
    } else {
        memcpy(s_admin, keys, (size_t)count * 32);
        ok = (nvs_set_blob(h, "pki_admin", s_admin, (size_t)count * 32) == ESP_OK);
    }
    if (ok) ok = (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (ok) s_admin_n = count;
    return ok;
}

static bool priv_install(const uint8_t *priv) {
    if (!priv || all_zero(priv, 32)) return false;
    static const uint8_t basepoint[32] = {9};
    uint8_t pub[32];
    curve25519_donna(pub, priv, basepoint);
    if (all_zero(pub, 32)) {
        memset(pub, 0, sizeof(pub));
        return false;
    }
    nvs_handle_t h = 0;
    if (nvs_open("lora", NVS_READWRITE, &h) != ESP_OK) {
        memset(pub, 0, sizeof(pub));
        return false;
    }
    bool ok = (nvs_set_blob(h, "pki_priv", priv, 32) == ESP_OK) &&
              (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    if (!ok) {
        memset(pub, 0, sizeof(pub));
        return false;
    }
    memcpy(s_priv, priv, 32);
    memcpy(s_pub, pub, 32);
    memset(pub, 0, sizeof(pub));
    s_have = true;
    return true;
}

bool lora_pki_set_private(const uint8_t *priv32) {
    return priv_install(priv32);
}

static void sha256_32(uint8_t *io) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    // Empty-input arg selects SHA-256 (not 224) in mbedTLS 2.x/3.x API.
    if (mbedtls_sha256_starts(&ctx, 0) == 0) {
        mbedtls_sha256_update(&ctx, io, 32);
        uint8_t out[32];
        if (mbedtls_sha256_finish(&ctx, out) == 0) memcpy(io, out, 32);
        memset(out, 0, sizeof(out));
    }
    mbedtls_sha256_free(&ctx);
}

// Upstream CryptoEngine::initNonce byte-exact: [packetId u64 LE][from u32 LE]
// at [0..7]/[8..11], then extraNonce overwrites bytes [4..7] when nonzero.
// Meshtastic's aes-ccm.cpp fixes L=2, therefore CCM uses the first 13 bytes:
// [packetId-lo u32][extraNonce u32][fromNode u32][zero].  The `8` passed
// to upstream aes_ccm_ae/ad is M (tag length), not the nonce length.
static void build_nonce(uint8_t *nonce16, uint32_t from_node, uint64_t packet_id,
                        uint32_t extra_nonce) {
    memset(nonce16, 0, 16);
    nonce16[0] = (uint8_t)packet_id;
    nonce16[1] = (uint8_t)(packet_id >> 8);
    nonce16[2] = (uint8_t)(packet_id >> 16);
    nonce16[3] = (uint8_t)(packet_id >> 24);
    nonce16[4] = (uint8_t)(packet_id >> 32);
    nonce16[5] = (uint8_t)(packet_id >> 40);
    nonce16[6] = (uint8_t)(packet_id >> 48);
    nonce16[7] = (uint8_t)(packet_id >> 56);
    nonce16[8] = (uint8_t)from_node;
    nonce16[9] = (uint8_t)(from_node >> 8);
    nonce16[10] = (uint8_t)(from_node >> 16);
    nonce16[11] = (uint8_t)(from_node >> 24);
    if (extra_nonce) {
        nonce16[4] = (uint8_t)extra_nonce;
        nonce16[5] = (uint8_t)(extra_nonce >> 8);
        nonce16[6] = (uint8_t)(extra_nonce >> 16);
        nonce16[7] = (uint8_t)(extra_nonce >> 24);
    }
}

static bool dh_raw(const uint8_t *peer_pub32, uint8_t *out32) {
    if (!peer_pub32 || all_zero(peer_pub32, 32) || !s_have) return false;
    uint8_t shared[32];
    curve25519_donna(shared, s_priv, peer_pub32);
    if (all_zero(shared, 32)) {
        memset(shared, 0, sizeof(shared));
        return false; // weak peer point (upstream dh2 check)
    }
    memcpy(out32, shared, 32);
    memset(shared, 0, sizeof(shared));
    return true;
}

static bool dh_key(const uint8_t *peer_pub32, uint8_t *out_aes256) {
    if (!dh_raw(peer_pub32, out_aes256)) return false;
    sha256_32(out_aes256);
    return true;
}

// Probe helpers (diagnostic only): raw DH, in-place SHA256, standard nonce.
// dh_key() above is dh_raw + sha256_32, behavior unchanged.
void lora_pki_nonce(uint8_t n16[16], uint32_t from, uint64_t id,
                    uint32_t extra) {
    if (!n16) return;
    build_nonce(n16, from, id, extra);
}

bool lora_pki_dh_raw(const uint8_t *peer_pub32, uint8_t out32[32]) {
    if (!peer_pub32 || !out32) return false;
    return dh_raw(peer_pub32, out32);
}

void lora_pki_sha256_32(uint8_t io[32]) {
    if (!io) return;
    sha256_32(io);
}

uint8_t lora_pki_selftest(void) {
    // RFC 7748 section 5.2. Vector 2 intentionally has its input u-coordinate
    // high bit set; conforming X25519 implementations ignore that bit.
    static const uint8_t scalar[2][32] = {
        {0xA5,0x46,0xE3,0x6B,0xF0,0x52,0x7C,0x9D,
         0x3B,0x16,0x15,0x4B,0x82,0x46,0x5E,0xDD,
         0x62,0x14,0x4C,0x0A,0xC1,0xFC,0x5A,0x18,
         0x50,0x6A,0x22,0x44,0xBA,0x44,0x9A,0xC4},
        {0x4B,0x66,0xE9,0xD4,0xD1,0xB4,0x67,0x3C,
         0x5A,0xD2,0x26,0x91,0x95,0x7D,0x6A,0xF5,
         0xC1,0x1B,0x64,0x21,0xE0,0xEA,0x01,0xD4,
         0x2C,0xA4,0x16,0x9E,0x79,0x18,0xBA,0x0D}
    };
    static const uint8_t u[2][32] = {
        {0xE6,0xDB,0x68,0x67,0x58,0x30,0x30,0xDB,
         0x35,0x94,0xC1,0xA4,0x24,0xB1,0x5F,0x7C,
         0x72,0x66,0x24,0xEC,0x26,0xB3,0x35,0x3B,
         0x10,0xA9,0x03,0xA6,0xD0,0xAB,0x1C,0x4C},
        {0xE5,0x21,0x0F,0x12,0x78,0x68,0x11,0xD3,
         0xF4,0xB7,0x95,0x9D,0x05,0x38,0xAE,0x2C,
         0x31,0xDB,0xE7,0x10,0x6F,0xC0,0x3C,0x3E,
         0xFC,0x4C,0xD5,0x49,0xC7,0x15,0xA4,0x93}
    };
    static const uint8_t expected[2][32] = {
        {0xC3,0xDA,0x55,0x37,0x9D,0xE9,0xC6,0x90,
         0x8E,0x94,0xEA,0x4D,0xF2,0x8D,0x08,0x4F,
         0x32,0xEC,0xCF,0x03,0x49,0x1C,0x71,0xF7,
         0x54,0xB4,0x07,0x55,0x77,0xA2,0x85,0x52},
        {0x95,0xCB,0xDE,0x94,0x76,0xE8,0x90,0x7D,
         0x7A,0xAD,0xE4,0x5C,0xB4,0xB8,0x73,0xF8,
         0x8B,0x59,0x5A,0x68,0x79,0x9F,0xA1,0x52,
         0xE6,0xF8,0xF7,0x64,0x7A,0xAC,0x79,0x57}
    };
    static const uint8_t sha_expected[32] = {
        0x63,0x0D,0xCD,0x29,0x66,0xC4,0x33,0x66,
        0x91,0x12,0x54,0x48,0xBB,0xB2,0x5B,0x4F,
        0xF4,0x12,0xA4,0x9C,0x73,0x2D,0xB2,0xC8,
        0xAB,0xC1,0xB8,0x58,0x1B,0xD7,0x10,0xDD
    };
    // Independently generated AES-256-CCM known answer: key=00..1f,
    // nonce=00..0c, plaintext=00..0f, no AAD, 8-byte authentication tag.
    static const uint8_t ccm_ct_expected[16] = {
        0x1C,0x06,0xA9,0x65,0x08,0xAD,0x7C,0x8F,
        0xC9,0xD9,0xDC,0x3B,0x16,0xD7,0x1B,0xE7
    };
    static const uint8_t ccm_tag_expected[8] = {
        0x2B,0xB2,0x01,0x30,0x91,0x31,0x56,0x2D
    };

    uint8_t failed = 0;
    uint8_t out[32];
    for (uint8_t i = 0; i < 2; i++) {
        curve25519_donna(out, scalar[i], u[i]);
        if (memcmp(out, expected[i], sizeof(out)) != 0)
            failed |= (uint8_t)(1u << i);
    }
    for (uint8_t i = 0; i < sizeof(out); i++) out[i] = i;
    sha256_32(out);
    if (memcmp(out, sha_expected, sizeof(out)) != 0) failed |= (1u << 2);

    uint8_t nonce[LORA_PKI_CCM_NONCE_LEN], pt[16], ct[16], tag[8], back[16];
    for (uint8_t i = 0; i < sizeof(out); i++) out[i] = i; // AES key
    for (uint8_t i = 0; i < sizeof(nonce); i++) nonce[i] = i;
    for (uint8_t i = 0; i < sizeof(pt); i++) pt[i] = i;
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, out, 256);
    if (rc == 0) {
        rc = mbedtls_ccm_encrypt_and_tag(&ctx, sizeof(pt), nonce, sizeof(nonce),
                                         NULL, 0, pt, ct, tag, sizeof(tag));
    }
    if (rc != 0 || memcmp(ct, ccm_ct_expected, sizeof(ct)) != 0 ||
        memcmp(tag, ccm_tag_expected, sizeof(tag)) != 0) {
        failed |= (1u << 3);
    } else {
        rc = mbedtls_ccm_auth_decrypt(&ctx, sizeof(ct), nonce, sizeof(nonce),
                                      NULL, 0, ccm_ct_expected, back,
                                      ccm_tag_expected, sizeof(ccm_tag_expected));
        if (rc != 0 || memcmp(back, pt, sizeof(back)) != 0)
            failed |= (1u << 3);
    }
    mbedtls_ccm_free(&ctx);
    memset(out, 0, sizeof(out));
    memset(nonce, 0, sizeof(nonce));
    memset(pt, 0, sizeof(pt));
    memset(ct, 0, sizeof(ct));
    memset(tag, 0, sizeof(tag));
    memset(back, 0, sizeof(back));
    return failed;
}

void lora_pki_init(void) {
    if (s_have) return;
    nvs_handle_t h = 0;
    size_t n = 32;
    uint8_t priv[32];
    bool loaded = false;
    bool can_generate = true;
    bool persisted = false;
    esp_err_t open_rc = nvs_open("lora", NVS_READONLY, &h);
    if (open_rc != ESP_OK) {
        if (open_rc == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "PKI key not present; generating first identity");
        } else {
            can_generate = false;
            ESP_LOGE(TAG, "PKI key load unavailable rc=%d (%s); refusing identity rotation",
                     (int)open_rc, esp_err_to_name(open_rc));
        }
    } else {
        esp_err_t get_rc = nvs_get_blob(h, "pki_priv", priv, &n);
        nvs_close(h);
        if (get_rc == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "PKI key not present; generating first identity");
        } else if (get_rc != ESP_OK) {
            can_generate = false;
            ESP_LOGE(TAG, "PKI key read failed rc=%d (%s); refusing identity rotation",
                     (int)get_rc, esp_err_to_name(get_rc));
        } else if (n != 32) {
            ESP_LOGW(TAG, "PKI key load: stored pki_priv invalid length %u (want 32); will generate new key",
                     (unsigned)n);
        } else if (all_zero(priv, 32)) {
            ESP_LOGW(TAG, "PKI key load: stored pki_priv present but all-zero; will generate new key");
        } else {
            loaded = true;
            persisted = true;
        }
    }
    if (!loaded) {
        if (!can_generate) {
            memset(priv, 0, sizeof(priv));
            admin_load();
            return;
        }
        esp_fill_random(priv, sizeof(priv));
        if (all_zero(priv, sizeof(priv))) priv[0] = 0x77;
        esp_err_t wopen_rc = nvs_open("lora", NVS_READWRITE, &h);
        if (wopen_rc != ESP_OK) {
            ESP_LOGW(TAG, "PKI key store: nvs_open RW failed rc=%d (%s); identity will stay disabled",
                     (int)wopen_rc, esp_err_to_name(wopen_rc));
        } else {
            esp_err_t set_rc = nvs_set_blob(h, "pki_priv", priv, 32);
            esp_err_t commit_rc = ESP_OK;
            if (set_rc != ESP_OK) {
                ESP_LOGW(TAG, "PKI key store: nvs_set_blob pki_priv failed rc=%d (%s); identity will stay disabled",
                         (int)set_rc, esp_err_to_name(set_rc));
            } else {
                commit_rc = nvs_commit(h);
                if (commit_rc != ESP_OK) {
                    ESP_LOGW(TAG, "PKI key store: nvs_commit failed rc=%d (%s); identity will stay disabled",
                             (int)commit_rc, esp_err_to_name(commit_rc));
                }
            }
            nvs_close(h);
            persisted = set_rc == ESP_OK && commit_rc == ESP_OK;
        }
        if (!persisted) {
            ESP_LOGE(TAG, "PKI identity disabled: generated identity was not persisted");
            memset(priv, 0, sizeof(priv));
            admin_load();
            return;
        }
    }
    static const uint8_t basepoint[32] = {9};
    uint8_t pub[32];
    curve25519_donna(pub, priv, basepoint);
    if (all_zero(pub, 32)) {
        ESP_LOGW(TAG, "PKI key weak point after %s; regenerating once",
                 loaded ? "load" : "generate");
        ESP_LOGE(TAG, "keygen produced weak point; retrying once");
        esp_fill_random(priv, sizeof(priv));
        curve25519_donna(pub, priv, basepoint);
        persisted = false;
        esp_err_t ropen_rc = nvs_open("lora", NVS_READWRITE, &h);
        if (ropen_rc != ESP_OK) {
            ESP_LOGW(TAG, "PKI key retry store: nvs_open RW failed rc=%d (%s); identity will stay disabled",
                     (int)ropen_rc, esp_err_to_name(ropen_rc));
        } else {
            esp_err_t set_rc = nvs_set_blob(h, "pki_priv", priv, 32);
            if (set_rc != ESP_OK) {
                ESP_LOGW(TAG, "PKI key retry store: nvs_set_blob failed rc=%d (%s); identity will stay disabled",
                         (int)set_rc, esp_err_to_name(set_rc));
            } else {
                esp_err_t commit_rc = nvs_commit(h);
                if (commit_rc != ESP_OK) {
                    ESP_LOGW(TAG, "PKI key retry store: nvs_commit failed rc=%d (%s); identity will stay disabled",
                             (int)commit_rc, esp_err_to_name(commit_rc));
                } else persisted = true;
            }
            nvs_close(h);
        }
        if (!persisted || all_zero(pub, 32)) {
            ESP_LOGE(TAG, "PKI identity disabled: retry key invalid or not persisted");
            memset(priv, 0, sizeof(priv));
            memset(pub, 0, sizeof(pub));
            admin_load();
            return;
        }
    }
    memcpy(s_priv, priv, 32);
    memcpy(s_pub, pub, 32);
    memset(priv, 0, sizeof(priv));
    memset(pub, 0, sizeof(pub));
    s_have = true;
    admin_load();
    // Full 32B pubkey on one INFO line so HIL can fingerprint-compare
    // across boots and spot key rotation. Logging only.
    char pub_hex[65];
    for (int i = 0; i < 32; i++) snprintf(pub_hex + i * 2, 3, "%02X", s_pub[i]);
    pub_hex[64] = '\0';
    ESP_LOGI(TAG, "PKI ready pub=%s (%s)", pub_hex, loaded ? "loaded" : "generated NEW");
}

bool lora_pki_has_key(void) { return s_have; }

const uint8_t *lora_pki_public(void) { return s_have ? s_pub : NULL; }

const uint8_t *lora_pki_private(void) { return s_have ? s_priv : NULL; }

bool lora_pki_regen(void) {
    uint8_t priv[32];
    esp_fill_random(priv, sizeof(priv));
    bool ok = priv_install(priv);
    memset(priv, 0, sizeof(priv));
    return ok;
}

uint16_t lora_pki_encrypt(uint32_t to_node, uint32_t from_node,
                          const uint8_t *peer_pub32,
                          uint32_t packet_id,
                          const uint8_t *pt, uint16_t plen,
                          uint8_t *out, uint16_t out_cap) {
    (void)to_node;
    if (!pt || !out || plen == 0 || plen + LORA_PKI_OVERHEAD > out_cap) return 0;
    if (!s_have || !peer_pub32) return 0;
    uint8_t key[32];
    if (!dh_key(peer_pub32, key)) return 0;
    uint32_t extra = esp_random();
    uint8_t nonce16[16];
    build_nonce(nonce16, from_node, packet_id, extra);
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    uint8_t tag[8];
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_ccm_encrypt_and_tag(&ctx, plen, nonce16,
                                         LORA_PKI_CCM_NONCE_LEN,
                                         NULL, 0, pt, out, tag, sizeof(tag));
    }
    mbedtls_ccm_free(&ctx);
    memset(key, 0, sizeof(key));
    memset(nonce16, 0, sizeof(nonce16));
    if (rc != 0) {
        memset(tag, 0, sizeof(tag));
        return 0;
    }
    memcpy(out + plen, tag, 8);
    out[plen + 8] = (uint8_t)extra;
    out[plen + 9] = (uint8_t)(extra >> 8);
    out[plen + 10] = (uint8_t)(extra >> 16);
    out[plen + 11] = (uint8_t)(extra >> 24);
    memset(tag, 0, sizeof(tag));
    return (uint16_t)(plen + LORA_PKI_OVERHEAD);
}

uint16_t lora_pki_decrypt(uint32_t from_node,
                          const uint8_t *peer_pub32,
                          uint32_t packet_id,
                          const uint8_t *ct, uint16_t ct_len,
                          uint8_t *out, uint16_t out_cap) {
    if (!ct || !out || ct_len <= LORA_PKI_OVERHEAD) return 0;
    if (!s_have || !peer_pub32) return 0;
    uint16_t plen = (uint16_t)(ct_len - LORA_PKI_OVERHEAD);
    if (plen > out_cap) return 0;
    uint32_t extra = (uint32_t)ct[ct_len - 4] | ((uint32_t)ct[ct_len - 3] << 8) |
                     ((uint32_t)ct[ct_len - 2] << 16) | ((uint32_t)ct[ct_len - 1] << 24);
    uint8_t key[32];
    if (!dh_key(peer_pub32, key)) return 0;
    uint8_t nonce16[16];
    build_nonce(nonce16, from_node, packet_id, extra);
    mbedtls_ccm_context ctx;
    mbedtls_ccm_init(&ctx);
    int rc = mbedtls_ccm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    if (rc == 0) {
        rc = mbedtls_ccm_auth_decrypt(&ctx, plen, nonce16,
                                      LORA_PKI_CCM_NONCE_LEN,
                                      NULL, 0, ct, out, ct + plen, 8);
    }
    mbedtls_ccm_free(&ctx);
    memset(key, 0, sizeof(key));
    memset(nonce16, 0, sizeof(nonce16));
    if (rc != 0) {
        memset(out, 0, plen);
        return 0;
    }
    return plen;
}

#else
typedef int lora_pki_stub_guard;
#endif

// ---- perhapsEncode/perhapsDecode policy helpers ----
// Pure predicates (no NVS/HW): intentionally built even without
// CONFIG_HAS_LORA so the mesh layer can call them unconditionally.
// Broadcast literal matches LORA_MESH_BROADCAST (lora_mesh.h); the literal
// is used here so this file takes no mesh-layer dependency.
bool lora_pki_can_encode(uint32_t portnum) {
    return portnum != (uint32_t)LORA_PORT_POSITION &&
           portnum != (uint32_t)LORA_PORT_NODEINFO &&
           portnum != (uint32_t)LORA_PORT_ROUTING &&
           portnum != (uint32_t)LORA_PORT_TRACEROUTE;
}

bool lora_pki_should_try_first(uint8_t channel_byte, uint32_t to) {
    if (channel_byte != 0) return false;
    // Unicast shape only; the caller must additionally require to == self.
    return to != 0xFFFFFFFFu && to != 0u;
}

bool lora_pki_is_legacy_dm(uint32_t port, uint32_t to, uint32_t from) {
    if (port != (uint32_t)LORA_PORT_TEXT) return false;
    if (to == 0xFFFFFFFFu || to == 0u) return false;
    return from != to;
}

bool lora_pki_must_refuse_no_key(bool have_peer_key) {
    return !have_peer_key;
}
