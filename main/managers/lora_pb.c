// lora_pb.c
// Minimal protobuf codec for the Meshtastic PhoneAPI subset. See lora_pb.h
// for the reference (files + field numbers). Unknown fields are skipped so
// newer apps stay compatible.

#include "managers/lora_pb.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_LORA

#include "esp_mac.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>

uint32_t lora_pb_local_hardware_model(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strncmp(CONFIG_BUILD_CONFIG_TEMPLATE, "crowpanel_advance",
                strlen("crowpanel_advance")) == 0)
        return MESHTASTIC_HW_CROWPANEL;
#endif
    return MESHTASTIC_HW_HELTEC_V3;
}

const char *lora_pb_local_pio_env(void) {
    return lora_pb_local_hardware_model() == MESHTASTIC_HW_CROWPANEL
               ? "crowpanel-advance"
               : "heltec-v3";
}

// ---------- writer ----------
void pb_w_init(pb_w_t *w, uint8_t *buf, uint16_t cap) {
    w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false;
}

static void pb_put(pb_w_t *w, uint8_t b) {
    if (w->len < w->cap) w->buf[w->len++] = b;
    else w->overflow = true;
}

static void pb_uvar(pb_w_t *w, uint32_t v);

static void pb_tag(pb_w_t *w, uint8_t field, uint8_t wire) {
    // Tags are varints: fields >= 16 need TWO bytes (e.g. field 17 =
    // 0x8A 0x01). Emitting one byte 0x8A makes the parser swallow the
    // length as a tag continuation and the whole frame dies. This broke
    // every FromRadio variant >= 16 (uiconfig=17, region=19).
    pb_uvar(w, ((uint32_t)field << 3) | wire);
}

static void pb_uvar(pb_w_t *w, uint32_t v) {
    do {
        uint8_t b = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        pb_put(w, b);
    } while (v);
}

void pb_w_varint(pb_w_t *w, uint8_t field, uint32_t v) {
    if (v == 0) return; // proto3: default values are omitted
    pb_tag(w, field, 0);
    pb_uvar(w, v);
}

static void pb_w_varint_force(pb_w_t *w, uint8_t field, uint32_t v) {
    pb_tag(w, field, 0);
    pb_uvar(w, v);
}

// fixed32 that emits even when v == 0 (explicitly-present fields: Data
// dest/source/reply_id/emoji use has_* flags from the parser).
static void pb_w_fixed32_force(pb_w_t *w, uint8_t field, uint32_t v) {
    pb_tag(w, field, 5);
    pb_put(w, (uint8_t)v);
    pb_put(w, (uint8_t)(v >> 8));
    pb_put(w, (uint8_t)(v >> 16));
    pb_put(w, (uint8_t)(v >> 24));
}

void pb_w_fixed32(pb_w_t *w, uint8_t field, uint32_t v) {
    if (v == 0) return;
    pb_tag(w, field, 5);
    pb_put(w, (uint8_t)v);
    pb_put(w, (uint8_t)(v >> 8));
    pb_put(w, (uint8_t)(v >> 16));
    pb_put(w, (uint8_t)(v >> 24));
}

void pb_w_float(pb_w_t *w, uint8_t field, float f) {
    if (f == 0.0f) return;
    uint32_t v;
    memcpy(&v, &f, 4);
    pb_tag(w, field, 5);
    pb_put(w, (uint8_t)v);
    pb_put(w, (uint8_t)(v >> 8));
    pb_put(w, (uint8_t)(v >> 16));
    pb_put(w, (uint8_t)(v >> 24));
}

void pb_w_bytes(pb_w_t *w, uint8_t field, const uint8_t *d, uint16_t n) {
    if (!d || n == 0) return;
    pb_tag(w, field, 2);
    pb_uvar(w, n);
    for (uint16_t i = 0; i < n; i++) pb_put(w, d[i]);
}

void pb_w_string(pb_w_t *w, uint8_t field, const char *s) {
    if (!s || !s[0]) return;
    pb_w_bytes(w, field, (const uint8_t *)s, (uint16_t)strlen(s));
}

void pb_w_msg(pb_w_t *w, uint8_t field, const uint8_t *d, uint16_t n) {
    // Empty submessage still encodes as tag+len0 (valid, occasionally needed).
    pb_tag(w, field, 2);
    pb_uvar(w, n);
    for (uint16_t i = 0; i < n; i++) pb_put(w, d[i]);
}

// ---------- reader ----------
void pb_r_init(pb_r_t *r, const uint8_t *p, uint16_t len) {
    r->p = p; r->len = len; r->pos = 0;
}

static bool pb_ruvar(pb_r_t *r, uint32_t *v) {
    uint32_t out = 0;
    int shift = 0;
    for (int i = 0; i < 5; i++) {
        if (r->pos >= r->len) return false;
        uint8_t b = r->p[r->pos++];
        out |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = out;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool pb_r_next(pb_r_t *r, uint8_t *field, uint8_t *wire,
               uint32_t *varint, const uint8_t **bytes, uint16_t *bytes_len) {
    if (r->pos >= r->len) return false;
    uint32_t tag;
    if (!pb_ruvar(r, &tag)) return false;
    uint8_t f = (uint8_t)(tag >> 3);
    uint8_t wt = (uint8_t)(tag & 0x07);
    if (f == 0) return false;
    if (field) *field = f;
    if (wire) *wire = wt;
    if (wt == 0) {
        uint32_t v;
        if (!pb_ruvar(r, &v)) return false;
        if (varint) *varint = v;
    } else if (wt == 5) {
        if (r->pos + 4 > r->len) return false;
        uint32_t v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) |
                     ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
        r->pos += 4;
        if (varint) *varint = v;
    } else if (wt == 2) {
        uint32_t n;
        if (!pb_ruvar(r, &n)) return false;
        if (n > (uint32_t)(r->len - r->pos)) return false;
        if (bytes) *bytes = &r->p[r->pos];
        if (bytes_len) *bytes_len = (uint16_t)n;
        r->pos += (uint16_t)n;
    } else if (wt == 1) {
        if (r->pos + 8 > r->len) return false;
        r->pos += 8;
    } else {
        return false;
    }
    return true;
}

// ---------- builders ----------
uint16_t pb_build_mesh_text(uint8_t *out, uint16_t cap,
                            uint32_t from, uint32_t to, uint32_t id,
                            float snr, uint32_t hop_limit, uint32_t hop_start,
                            const char *text) {
    uint8_t data[200];
    pb_w_t dw;
    pb_w_init(&dw, data, sizeof(data));
    pb_w_varint(&dw, 1, MESHTASTIC_PORT_TEXT);
    pb_w_string(&dw, 2, text);
    if (dw.overflow) return 0;
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_fixed32(&w, 1, from);
    pb_w_fixed32(&w, 2, to);
    // channel 0 omitted (default). decoded=4:
    pb_w_msg(&w, 4, data, dw.len);
    pb_w_fixed32(&w, 6, id);
    pb_w_float(&w, 8, snr);
    pb_w_varint(&w, 9, hop_limit);
    pb_w_varint(&w, 15, hop_start);
    if (w.overflow) return 0;
    return w.len;
}

static uint16_t fromradio_wrap(uint8_t *out, uint16_t cap, uint32_t from_num,
                               uint8_t variant_field, const uint8_t *payload, uint16_t plen) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_varint_force(&w, 1, from_num); // id: always present (app uses it as doorbell seq)
    pb_w_msg(&w, variant_field, payload, plen);
    if (w.overflow) return 0;
    return w.len;
}

uint16_t pb_build_fromradio_packet(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   const uint8_t *mesh_pkt, uint16_t mesh_len) {
    return fromradio_wrap(out, cap, from_num, 2, mesh_pkt, mesh_len);
}

// Data{portnum, payload?, request_id?} for device->phone replies.
uint16_t pb_build_data_msg(uint8_t *out, uint16_t cap, uint32_t portnum,
                           const uint8_t *payload, uint16_t plen, uint32_t request_id) {
    return pb_build_data_msg_full(out, cap, portnum, payload, plen, request_id,
                                  false, 0, false, 0, false, 0, false, 0,
                                  false, 0);
}

uint16_t pb_build_data_msg_full(uint8_t *out, uint16_t cap, uint32_t portnum,
                           const uint8_t *payload, uint16_t plen, uint32_t request_id,
                           bool has_dest, uint32_t dest,
                           bool has_source, uint32_t source,
                           bool has_reply_id, uint32_t reply_id,
                           bool has_emoji, uint32_t emoji,
                           bool has_bitfield, uint32_t bitfield) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_varint(&w, 1, portnum);
    pb_w_bytes(&w, 2, payload, plen);
    if (has_dest) pb_w_fixed32_force(&w, 4, dest);
    if (has_source) pb_w_fixed32_force(&w, 5, source);
    pb_w_fixed32(&w, 6, request_id); // Data.request_id; response linkage
    if (has_reply_id) pb_w_fixed32_force(&w, 7, reply_id);
    if (has_emoji) pb_w_fixed32_force(&w, 8, emoji);
    if (has_bitfield) pb_w_varint(&w, 9, bitfield);
    if (w.overflow) return 0;
    return w.len;
}

// MeshPacket for FromRadio.packet replies (ACKs, admin responses).
uint16_t pb_build_mesh_packet(uint8_t *out, uint16_t cap,
                              uint32_t from, uint32_t to, uint32_t id,
                              const uint8_t *data, uint16_t dlen) {
    return pb_build_mesh_packet_ch(out, cap, from, to, id, 0, data, dlen);
}

uint16_t pb_build_mesh_packet_ch(uint8_t *out, uint16_t cap,
                                 uint32_t from, uint32_t to, uint32_t id,
                                 uint32_t channel_idx,
                                 const uint8_t *data, uint16_t dlen) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_fixed32(&w, 1, from);
    pb_w_fixed32(&w, 2, to);
    pb_w_varint(&w, 3, channel_idx); // 0 omitted (primary)
    pb_w_msg(&w, 4, data, dlen);
    pb_w_fixed32(&w, 6, id);
    pb_w_varint(&w, 9, 3);  // hop_limit (local reply, never airs)
    pb_w_varint(&w, 15, 3); // hop_start mirrors limit
    if (w.overflow) return 0;
    return w.len;
}

uint16_t pb_build_mesh_packet_pki(uint8_t *out, uint16_t cap,
                                  uint32_t from, uint32_t to, uint32_t id,
                                  uint32_t channel_idx,
                                  const uint8_t *sender_pub32,
                                  const uint8_t *data, uint16_t dlen) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_fixed32(&w, 1, from);
    pb_w_fixed32(&w, 2, to);
    pb_w_varint(&w, 3, channel_idx);
    pb_w_msg(&w, 4, data, dlen);
    pb_w_fixed32(&w, 6, id);
    pb_w_varint(&w, 9, 3);
    pb_w_varint(&w, 15, 3);
    pb_w_bytes(&w, 16, sender_pub32, sender_pub32 ? 32 : 0);
    pb_w_varint(&w, 17, 1); // pki_encrypted
    if (w.overflow) return 0;
    return w.len;
}

// Shared air->phone MeshPacket writer with RX metadata + real hop counts.
static uint16_t mesh_packet_rx_write(uint8_t *out, uint16_t cap,
                                     uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     const uint8_t *sender_pub32, bool pki,
                                     const uint8_t *data, uint16_t dlen,
                                     const pb_rx_meta_t *meta) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_fixed32(&w, 1, from);
    pb_w_fixed32(&w, 2, to);
    pb_w_varint(&w, 3, channel_idx);
    pb_w_msg(&w, 4, data, dlen);
    pb_w_fixed32(&w, 6, id);
    if (meta && meta->rx_time >= 1577836800U) pb_w_fixed32(&w, 7, meta->rx_time);
    pb_w_float(&w, 8, meta ? meta->snr : 0.0f);
    pb_w_varint(&w, 9, hop_limit);
    // rx_rssi (12, optional int32) deliberately omitted: negative RSSI
    // encodes as a 10-byte varint and no phone needs it (snr carries
    // link quality). See docs/lora-meshtastic-app.md.
    pb_w_varint(&w, 15, hop_start);
    if (pki) {
        pb_w_bytes(&w, 16, sender_pub32, sender_pub32 ? 32 : 0);
        pb_w_varint(&w, 17, 1);
    }
    if (w.overflow) return 0;
    return w.len;
}

uint16_t pb_build_mesh_text_rx(uint8_t *out, uint16_t cap,
                               uint32_t from, uint32_t to, uint32_t id,
                               uint32_t hop_limit, uint32_t hop_start,
                               uint32_t channel_idx, const char *text,
                               const pb_rx_meta_t *meta) {
    uint8_t data[200];
    pb_w_t dw;
    pb_w_init(&dw, data, sizeof(data));
    pb_w_varint(&dw, 1, MESHTASTIC_PORT_TEXT);
    pb_w_string(&dw, 2, text);
    if (dw.overflow) return 0;
    return mesh_packet_rx_write(out, cap, from, to, id, channel_idx,
                                hop_limit, hop_start, NULL, false,
                                data, dw.len, meta);
}

uint16_t pb_build_mesh_packet_rx(uint8_t *out, uint16_t cap,
                                 uint32_t from, uint32_t to, uint32_t id,
                                 uint32_t channel_idx,
                                 uint32_t hop_limit, uint32_t hop_start,
                                 const uint8_t *data, uint16_t dlen,
                                 const pb_rx_meta_t *meta) {
    return mesh_packet_rx_write(out, cap, from, to, id, channel_idx,
                                hop_limit, hop_start, NULL, false,
                                data, dlen, meta);
}

uint16_t pb_build_mesh_packet_pki_rx(uint8_t *out, uint16_t cap,
                                     uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     const uint8_t *sender_pub32,
                                     const uint8_t *data, uint16_t dlen,
                                     const pb_rx_meta_t *meta) {
    return mesh_packet_rx_write(out, cap, from, to, id, channel_idx,
                                hop_limit, hop_start, sender_pub32, true,
                                data, dlen, meta);
}

uint16_t pb_build_mesh_text_ch(uint8_t *out, uint16_t cap,
                               uint32_t from, uint32_t to, uint32_t id,
                               float snr, uint32_t hop_limit, uint32_t hop_start,
                               uint32_t channel_idx, const char *text) {
    uint8_t data[200];
    pb_w_t dw;
    pb_w_init(&dw, data, sizeof(data));
    pb_w_varint(&dw, 1, MESHTASTIC_PORT_TEXT);
    pb_w_string(&dw, 2, text);
    if (dw.overflow) return 0;
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_fixed32(&w, 1, from);
    pb_w_fixed32(&w, 2, to);
    pb_w_varint(&w, 3, channel_idx);
    pb_w_msg(&w, 4, data, dw.len);
    pb_w_fixed32(&w, 6, id);
    pb_w_float(&w, 8, snr);
    pb_w_varint(&w, 9, hop_limit);
    pb_w_varint(&w, 15, hop_start);
    if (w.overflow) return 0;
    return w.len;
}

// Config{device{tzdef?}} — matches the generic empty section when no zone is
// stored, so the handshake is unchanged until the app pushes one.
uint16_t pb_build_fromradio_config_device(uint8_t *out, uint16_t cap, uint32_t from_num,
                                          const char *tzdef) {
    return pb_build_fromradio_config_device_full(out, cap, from_num, 0, tzdef);
}

uint16_t pb_build_fromradio_config_device_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                               int role, const char *tzdef) {
    uint8_t dev[48];
    pb_w_t d;
    pb_w_init(&d, dev, sizeof(dev));
    pb_w_varint(&d, 1, (uint32_t)role); // DeviceConfig.role CLIENT=0 omitted
    pb_w_string(&d, 11, tzdef); // DeviceConfig.tzdef; NULL/empty omitted
    if (d.overflow) return 0;
    uint8_t cfg[48];
    pb_w_t w;
    pb_w_init(&w, cfg, sizeof(cfg));
    pb_w_msg(&w, 1, dev, d.len);
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 5, cfg, w.len);
}

uint16_t pb_build_fromradio_myinfo(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   uint32_t node_num, uint32_t reboot_count,
                                   uint32_t nodedb_count) {
    uint8_t mi[64];
    pb_w_t w;
    pb_w_init(&w, mi, sizeof(mi));
    pb_w_varint_force(&w, 1, node_num);
    pb_w_varint(&w, 8, reboot_count);
    pb_w_varint(&w, 11, MESHTASTIC_MIN_APP_VERSION);
    // device_id 8 bytes (short MAC) helps app correlate; pio_env omitted (defaults)
    {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        uint8_t did[8];
        memcpy(did, mac, 6);
        did[6] = 0xBE; did[7] = 0xEF;
        pb_w_bytes(&w, 12, did, 8);
    }
    // MyNodeInfo.pio_env=13: upstream-style build environment name.
    // firmware_edition=14 is VANILLA=0 here -> proto3 default, omitted.
    pb_w_string(&w, 13, lora_pb_local_pio_env());
    pb_w_varint(&w, 15, nodedb_count);
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 3, mi, w.len);
}

uint16_t pb_build_fromradio_nodeinfo(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t num, const char *id_str,
                                     const char *long_name, const char *short_name,
                                     float snr) {
    return pb_build_fromradio_nodeinfo_pki(out, cap, from_num, num, id_str,
                                           long_name, short_name, snr, NULL);
}

// The v2.7.26 wire User accepts 40 UTF-8 bytes (newer slim NodeDBs still keep
// the wider wire type for compatibility). Emit whole characters up to that cap.
static void pb_w_long_name(pb_w_t *w, const char *s) {
    if (!s || !s[0]) return;
    // Walk forward emitting whole UTF-8 chars that fit in 40 bytes.
    size_t n = 0, i = 0, sl = strlen(s);
    while (i < sl && n < 40) {
        uint8_t c = (uint8_t)s[i];
        size_t cl = 1;
        if ((c & 0x80) == 0) cl = 1;
        else if ((c & 0xE0) == 0xC0) cl = 2;
        else if ((c & 0xF0) == 0xE0) cl = 3;
        else if ((c & 0xF8) == 0xF0) cl = 4;
        if (n + cl > 40 || i + cl > sl) break;
        n += cl;
        i += cl;
    }
    pb_w_bytes(w, 2, (const uint8_t *)s, (uint16_t)n);
}

uint16_t pb_build_fromradio_nodeinfo_pki(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t num, const char *id_str,
                                         const char *long_name, const char *short_name,
                                         float snr, const uint8_t *pub32) {
    uint8_t user[144];
    pb_w_t u;
    pb_w_init(&u, user, sizeof(user));
    pb_w_string(&u, 1, id_str);
    pb_w_long_name(&u, long_name);
    pb_w_string(&u, 3, short_name);
    // This helper describes an arbitrary peer and has no hardware/role input;
    // leave both unknown instead of incorrectly labelling every peer Heltec.
    pb_w_bytes(&u, 8, pub32, pub32 ? 32 : 0); // User.public_key
    if (u.overflow) return 0;
    uint8_t ni[192];
    pb_w_t w;
    pb_w_init(&w, ni, sizeof(ni));
    pb_w_varint_force(&w, 1, num);
    pb_w_msg(&w, 2, user, u.len);
    pb_w_float(&w, 4, snr);
    // NodeInfo.last_heard is Unix epoch seconds. Uptime here made apps render
    // dates near 1920/1970. Omit it until the clock has actually been set.
    time_t now = time(NULL);
    if (now >= 1577836800LL && (uint64_t)now <= UINT32_MAX)
        pb_w_fixed32(&w, 5, (uint32_t)now);
    pb_w_varint(&w, 15, 1); // heard_on_current_lora=true
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 4, ni, w.len);
}

uint16_t pb_build_fromradio_nodeinfo_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                          uint32_t num, const char *id_str,
                                          const char *long_name, const char *short_name,
                                          float snr, const uint8_t *pub32,
                                          bool verified, bool muted,
                                          bool hops_valid, uint8_t hops_away,
                                          uint32_t last_heard) {
    return pb_build_fromradio_nodeinfo_full_ex(out, cap, from_num, num, id_str,
                                               true,
                                               long_name, short_name, snr, pub32,
                                               verified, muted, hops_valid, hops_away,
                                               last_heard, NULL, 0, NULL, 0,
                                               0, false, false, false, 0, 0);
}

uint16_t pb_build_fromradio_nodeinfo_full_ex(uint8_t *out, uint16_t cap, uint32_t from_num,
                                          uint32_t num, const char *id_str,
                                          bool has_user,
                                          const char *long_name, const char *short_name,
                                          float snr, const uint8_t *pub32,
                                          bool verified, bool muted,
                                          bool hops_valid, uint8_t hops_away,
                                          uint32_t last_heard,
                                          const uint8_t *position, uint16_t position_len,
                                          const uint8_t *devmetrics, uint16_t devmetrics_len,
                                          uint32_t channel, bool via_mqtt,
                                          bool favorite, bool ignored,
                                          uint32_t hw_model, uint32_t role) {
    uint8_t user[144];
    pb_w_t u;
    pb_w_init(&u, user, sizeof(user));
    if (has_user) {
        pb_w_string(&u, 1, id_str);
        pb_w_long_name(&u, long_name);
        pb_w_string(&u, 3, short_name);
        pb_w_varint(&u, 5, hw_model);
        if (role) pb_w_varint(&u, 7, role);
        pb_w_bytes(&u, 8, pub32, pub32 ? 32 : 0);
    }
    if (u.overflow) return 0;
    uint8_t ni[256];
    pb_w_t w;
    pb_w_init(&w, ni, sizeof(ni));
    pb_w_varint_force(&w, 1, num);
    if (has_user) pb_w_msg(&w, 2, user, u.len);
    if (position && position_len) pb_w_msg(&w, 3, position, position_len);
    pb_w_float(&w, 4, snr);
    if (last_heard >= 1577836800U) pb_w_fixed32(&w, 5, last_heard);
    if (devmetrics && devmetrics_len) pb_w_msg(&w, 6, devmetrics, devmetrics_len);
    pb_w_varint(&w, 7, channel); // 0 omitted (primary/default)
    if (via_mqtt) pb_w_varint(&w, 8, 1);
    if (hops_valid) pb_w_varint(&w, 9, hops_away);
    if (favorite) pb_w_varint(&w, 10, 1); // is_favorite
    if (ignored) pb_w_varint(&w, 11, 1); // is_ignored
    if (verified) pb_w_varint(&w, 12, 1); // is_key_manually_verified
    if (muted) pb_w_varint(&w, 13, 1); // is_muted
    pb_w_varint(&w, 15, 1); // heard_on_current_lora=true
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 4, ni, w.len);
}

uint16_t pb_build_fromradio_config_lora(uint8_t *out, uint16_t cap, uint32_t from_num,
                                        uint8_t region_us_eu, uint32_t hop_limit,
                                        int32_t tx_power) {
    uint8_t lora[32];
    pb_w_t l;
    pb_w_init(&l, lora, sizeof(lora));
    pb_w_varint(&l, 1, 1); // use_preset=true; modem_preset LONG_FAST=0 omitted
    pb_w_varint(&l, 7, region_us_eu);
    pb_w_varint(&l, 8, hop_limit);
    pb_w_varint(&l, 9, 1); // tx_enabled
    if (tx_power > 0) pb_w_varint(&l, 10, (uint32_t)tx_power);
    if (l.overflow) return 0;
    uint8_t cfg[40];
    pb_w_t w;
    pb_w_init(&w, cfg, sizeof(cfg));
    pb_w_msg(&w, 6, lora, l.len); // Config.lora=6
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 5, cfg, w.len);
}

uint16_t pb_build_fromradio_channel(uint8_t *out, uint16_t cap, uint32_t from_num) {
    return pb_build_fromradio_channel_idx(out, cap, from_num, 0,
                                          MESHTASTIC_CH_ROLE_PRIMARY, true);
}

uint16_t pb_build_fromradio_channel_idx(uint8_t *out, uint16_t cap, uint32_t from_num,
                                        uint32_t index, uint32_t role, bool with_settings) {
    if (!with_settings) {
        uint8_t ch[8];
        pb_w_t w;
        pb_w_init(&w, ch, sizeof(ch));
        if (index != 0) pb_w_varint(&w, 1, index);
        pb_w_varint(&w, 3, role);
        if (w.overflow) return 0;
        return fromradio_wrap(out, cap, from_num, 10, ch, w.len);
    }
    const uint8_t psk1 = 0x01;
    return pb_build_fromradio_channel_full(out, cap, from_num, index, role,
                                           "", &psk1, 1, true, true);
}

uint16_t pb_build_fromradio_channel_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t index, uint32_t role,
                                         const char *name,
                                         const uint8_t *psk, uint16_t psk_len,
                                         bool uplink, bool downlink) {
    return pb_build_fromradio_channel_full_ex(out, cap, from_num, index, role,
                                              name, psk, psk_len, uplink, downlink,
                                              NULL, 0);
}

uint16_t pb_build_fromradio_channel_full_ex(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t index, uint32_t role,
                                         const char *name,
                                         const uint8_t *psk, uint16_t psk_len,
                                         bool uplink, bool downlink,
                                         const uint8_t *mod_settings, uint16_t mod_len) {
    uint8_t ch[96];
    pb_w_t w;
    pb_w_init(&w, ch, sizeof(ch));
    if (index != 0) pb_w_varint(&w, 1, index); // index 0 omitted (default)
    if (role != 0) {
        uint8_t settings[64];
        pb_w_t s;
        pb_w_init(&s, settings, sizeof(settings));
        // ChannelSettings.psk=2: 1-byte 0x01 = default shorthand (upstream).
        // Longer keys verbatim (16/32B). len 0 = omit (no crypto).
        if (psk && psk_len) pb_w_bytes(&s, 2, psk, psk_len);
        pb_w_string(&s, 3, name); // empty omitted (preset display default)
        if (uplink) pb_w_varint(&s, 5, 1);
        if (downlink) pb_w_varint(&s, 6, 1);
        // ChannelSettings.module_settings=7 (ModuleSettings{position_precision=1,
        // is_muted=2}): opaque passthrough both directions.
        if (mod_settings && mod_len) pb_w_bytes(&s, 7, mod_settings, mod_len);
        if (s.overflow) return 0;
        pb_w_msg(&w, 2, settings, s.len);
    }
    pb_w_varint(&w, 3, role); // DISABLED=0 omitted automatically
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 10, ch, w.len);
}

uint16_t pb_build_fromradio_metadata(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     const char *fw_version) {
    return pb_build_fromradio_metadata_full(out, cap, from_num, fw_version, 0);
}

uint16_t pb_build_fromradio_metadata_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     const char *fw_version, int role) {
    uint8_t md[64];
    pb_w_t w;
    pb_w_init(&w, md, sizeof(md));
    pb_w_string(&w, 1, fw_version);
    pb_w_varint(&w, 2, 25); // current Meshtastic device-state schema
    pb_w_varint(&w, 4, 1); // hasWifi
    pb_w_varint(&w, 5, 1); // hasBluetooth
    if (role > 0) pb_w_varint(&w, 7, (uint32_t)role); // DeviceMetadata.role
    pb_w_varint(&w, 9, lora_pb_local_hardware_model());
    pb_w_varint(&w, 11, 1); // hasPKC (X25519 DMs)
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 13, md, w.len);
}

uint16_t pb_build_fromradio_complete(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t nonce) {
    pb_w_t w;
    pb_w_init(&w, out, cap);
    pb_w_varint_force(&w, 1, from_num);
    pb_w_varint(&w, 7, nonce); // config_complete_id echoes want_config_id
    if (w.overflow) return 0;
    return w.len;
}

// QueueStatus reply to app heartbeats (field 11). Upstream answers every
// heartbeat with router queue depth; answering 0B makes some apps assume a
// dead link and re-ask want_config with a fresh nonce in a loop.
uint16_t pb_build_fromradio_queue(uint8_t *out, uint16_t cap, uint32_t from_num) {
    return pb_build_fromradio_queue_id(out, cap, from_num, 0);
}

uint16_t pb_build_fromradio_queue_id(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t mesh_packet_id) {
    uint8_t qs[16];
    pb_w_t q;
    pb_w_init(&q, qs, sizeof(qs));
    pb_w_varint(&q, 2, 10); // free
    pb_w_varint(&q, 3, 10); // maxlen (res=0 OK omitted)
    pb_w_varint(&q, 4, mesh_packet_id);
    if (q.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 11, qs, q.len);
}

// Empty DeviceUIConfig (field 17): version + minimal UI prefs so the app
// doesn't consider the config incomplete and re-ask forever.
uint16_t pb_build_fromradio_uiconfig(uint8_t *out, uint16_t cap, uint32_t from_num) {
    uint8_t ui[8];
    pb_w_t w;
    pb_w_init(&w, ui, sizeof(ui));
    pb_w_varint(&w, 1, 1); // version
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 17, ui, w.len);
}

// Region-preset map (field 19): every supported region × all modem presets.
// The app gates its preset picker on this table; the old single-entry map
// (LONG_FAST/ANZ) made every other preset unselectable.
uint16_t pb_build_fromradio_region(uint8_t *out, uint16_t cap, uint32_t from_num) {
    // LoRaPresetGroup{presets=[0..16], default_preset=0}. Preset 0 must be
    // forced on the wire (proto3 omits zero scalars otherwise).
    uint8_t grp[40] = {0};
    pb_w_t g;
    pb_w_init(&g, grp, sizeof(grp));
    pb_w_varint_force(&g, 1, 0); // presets[0]=LONG_FAST
    for (uint8_t p = 1; p <= 16; p++) pb_w_varint(&g, 1, p);
    if (g.overflow) return 0;
    static const uint8_t regions[] = {
        1, 3, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26,
    };
    uint8_t map[208];
    pb_w_t m;
    pb_w_init(&m, map, sizeof(map));
    pb_w_msg(&m, 1, grp, g.len);
    for (unsigned i = 0; i < sizeof(regions); i++) {
        uint8_t rg[8];
        pb_w_t r;
        pb_w_init(&r, rg, sizeof(rg));
        pb_w_varint(&r, 1, regions[i]);
        if (r.overflow) return 0;
        pb_w_msg(&m, 2, rg, r.len);
    }
    if (m.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 19, map, m.len);
}

// One Config section (field 5). Only lora(6) carries real content; the rest
// are empty (proto3 defaults = upstream defaults for our CLIENT role).
// section: Config oneof field 1..10 (device..device_ui).
uint16_t pb_build_fromradio_config_sec(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t section) {
    return pb_build_fromradio_config_raw(out, cap, from_num, section, NULL, 0);
}

uint16_t pb_build_fromradio_config_raw(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t section, const uint8_t *raw, uint16_t raw_len) {
    if (section < 1 || section > 10) return 0;
    if (section == 6) return 0; // lora has its own builder; never empty here
    uint8_t cfg[170];
    pb_w_t w;
    pb_w_init(&w, cfg, sizeof(cfg));
    if (raw && raw_len) {
        pb_w_msg(&w, section, raw, raw_len);
    } else if (section == 7) {
        // BluetoothConfig{enabled=true, mode=NO_PIN}.  The live GATT service
        // uses Just Works/no encrypted characteristics, so reporting the
        // proto default RANDOM_PIN here makes the app wait for pairing that
        // can never occur and then reconnect forever.
        uint8_t bt[4];
        pb_w_t b;
        pb_w_init(&b, bt, sizeof(bt));
        pb_w_varint(&b, 1, 1);
        pb_w_varint(&b, 2, 2);
        if (b.overflow) return 0;
        pb_w_msg(&w, section, bt, b.len);
    } else {
        pb_w_msg(&w, section, NULL, 0);
    }
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 5, cfg, w.len);
}

uint16_t pb_build_fromradio_config_lora_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                             bool use_preset, uint32_t preset,
                                             uint32_t bw, uint32_t sf, uint32_t cr,
                                             float freq_offset, uint8_t region,
                                             uint32_t hop_limit, bool tx_enabled,
                                             int32_t tx_power, uint32_t channel_num,
                                             float override_freq) {
    return pb_build_fromradio_config_lora_full_ex(out, cap, from_num,
                                                  use_preset, preset, bw, sf, cr,
                                                  freq_offset, region, hop_limit,
                                                  tx_enabled, tx_power, channel_num,
                                                  override_freq, NULL);
}

void pb_parse_lora_extras(const uint8_t *p, uint16_t len, pb_lora_extras_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!p) return;
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t f, wr;
    uint32_t v;
    const uint8_t *b;
    uint16_t bl;
    while (pb_r_next(&r, &f, &wr, &v, &b, &bl)) {
        if (wr != 0) continue;
        if (f == 12) out->override_duty_cycle = v != 0;
        else if (f == 13) out->sx126x_rx_boosted_gain = v != 0;
        else if (f == 15) out->pa_fan_disabled = v != 0;
        else if (f == 103 && out->n_ignore_incoming < 8)
            out->ignore_incoming[out->n_ignore_incoming++] = v;
        else if (f == 104) out->ignore_mqtt = v != 0;
        else if (f == 105) out->config_ok_to_mqtt = v != 0;
        else if (f == 106) { out->has_fem_lna_mode = true; out->fem_lna_mode = v; }
        else if (f == 107) out->serial_hal_only = v != 0;
    }
}

void pb_w_lora_extras(pb_w_t *w, const pb_lora_extras_t *ex) {
    if (!w || !ex) return;
    if (ex->override_duty_cycle) pb_w_varint(w, 12, 1);
    if (ex->sx126x_rx_boosted_gain) pb_w_varint(w, 13, 1);
    if (ex->pa_fan_disabled) pb_w_varint(w, 15, 1);
    for (uint8_t i = 0; i < ex->n_ignore_incoming; i++)
        pb_w_varint(w, 103, ex->ignore_incoming[i]);
    if (ex->ignore_mqtt) pb_w_varint(w, 104, 1);
    if (ex->config_ok_to_mqtt) pb_w_varint(w, 105, 1);
    if (ex->has_fem_lna_mode) pb_w_varint(w, 106, ex->fem_lna_mode);
    if (ex->serial_hal_only) pb_w_varint(w, 107, 1);
}

uint16_t pb_build_fromradio_config_lora_full_ex(uint8_t *out, uint16_t cap, uint32_t from_num,
                                             bool use_preset, uint32_t preset,
                                             uint32_t bw, uint32_t sf, uint32_t cr,
                                             float freq_offset, uint8_t region,
                                             uint32_t hop_limit, bool tx_enabled,
                                             int32_t tx_power, uint32_t channel_num,
                                             float override_freq,
                                             const pb_lora_extras_t *ex) {
    uint8_t lora[96];
    pb_w_t l;
    pb_w_init(&l, lora, sizeof(lora));
    if (use_preset) pb_w_varint(&l, 1, 1);
    if (use_preset) pb_w_varint(&l, 2, preset);
    else {
        // Custom path: bandwidth code + sf/cr numbers. Upstream bwCodeToKHz
        // inverts here (125->0? we report raw kHz/1000 codes used by app).
        // The app sends bandwidth as index; report what we run.
        if (bw) pb_w_varint(&l, 3, bw);
        if (sf) pb_w_varint(&l, 4, sf);
        if (cr) pb_w_varint(&l, 5, cr);
    }
    // Custom CR override on preset path (upstream honors coding_rate 5..8).
    if (use_preset && cr >= 5 && cr <= 8) {
        // Re-emit as field 5 (preset table default otherwise applies on air).
        pb_w_varint(&l, 5, cr);
    }
    if (freq_offset != 0.0f) {
        pb_w_float(&l, 6, freq_offset);
    }
    pb_w_varint(&l, 7, region);
    pb_w_varint(&l, 8, hop_limit);
    if (tx_enabled) pb_w_varint(&l, 9, 1);
    if (tx_power > 0) pb_w_varint(&l, 10, (uint32_t)tx_power);
    if (channel_num) pb_w_varint(&l, 11, channel_num);
    if (override_freq != 0.0f) {
        pb_w_float(&l, 14, override_freq);
    }
    pb_w_lora_extras(&l, ex); // preserved modem-unknown fields (12/13/15/103..107)
    if (l.overflow) return 0;
    uint8_t cfg[112];
    pb_w_t w;
    pb_w_init(&w, cfg, sizeof(cfg));
    pb_w_msg(&w, 6, lora, l.len); // Config.lora=6
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 5, cfg, w.len);
}

// One ModuleConfig entry (field 9), empty payload (all modules disabled).
// module: ModuleConfig oneof field 1..17 (mqtt..mesh_beacon).
uint16_t pb_build_fromradio_module(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   uint8_t module) {
    return pb_build_fromradio_module_raw(out, cap, from_num, module, NULL, 0);
}

uint16_t pb_build_fromradio_module_raw(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t module, const uint8_t *raw, uint16_t raw_len) {
    if (module < 1 || module > 17) return 0;
    uint8_t mc[170];
    pb_w_t w;
    pb_w_init(&w, mc, sizeof(mc));
    pb_w_msg(&w, module, raw, raw_len);
    if (w.overflow) return 0;
    return fromradio_wrap(out, cap, from_num, 9, mc, w.len);
}

// ---------- ToRadio ----------
bool pb_parse_toradio(const uint8_t *p, uint16_t len, toradio_t *out) {
    if (!p || len == 0 || !out) return false;
    memset(out, 0, sizeof(*out));
    pb_r_t r;
    pb_r_init(&r, p, len);
    uint8_t field, wire;
    uint32_t varint;
    const uint8_t *bytes;
    uint16_t bytes_len;
    while (pb_r_next(&r, &field, &wire, &varint, &bytes, &bytes_len)) {
        if (field == 1 && wire == 2) {
            // MeshPacket: extract to + decoded TEXT payload.
            pb_r_t m;
            pb_r_init(&m, bytes, bytes_len);
            uint8_t mf, mw;
            uint32_t mv;
            const uint8_t *mb;
            uint16_t ml;
            uint32_t to = 0xFFFFFFFF;
            uint32_t from = 0;
            uint32_t msg_id = 0;
            uint32_t channel = 0;
            bool want_ack = false;
            bool got_text = false;
            bool got_data = false;
            uint32_t data_port = 0;
            const uint8_t *data_payload = NULL;
            uint16_t data_payload_len = 0;
            const uint8_t *data_msg = NULL;
            uint16_t data_msg_len = 0;
            uint32_t data_dest = 0, data_source = 0, data_reply_id = 0;
            uint32_t data_emoji = 0, data_bitfield = 0;
            bool has_dest = false, has_source = false, has_reply_id = false;
            bool has_emoji = false, has_bitfield = false;
            char text[161] = {0};
            while (pb_r_next(&m, &mf, &mw, &mv, &mb, &ml)) {
                if (mf == 1 && mw == 5) from = mv; // fixed32
                else if (mf == 2 && mw == 5) to = mv; // fixed32
                else if (mf == 3 && mw == 0) channel = mv; // channel index
                else if (mf == 6 && mw == 5) msg_id = mv; // fixed32
                else if (mf == 10 && mw == 0) want_ack = mv != 0;
                else if (mf == 4 && mw == 2) {
                    data_msg = mb;
                    data_msg_len = ml;
                    pb_r_t d;
                    pb_r_init(&d, mb, ml);
                    uint8_t df, dw2;
                    uint32_t dv;
                    const uint8_t *db;
                    uint16_t dl;
                    uint32_t port = 0;
                    while (pb_r_next(&d, &df, &dw2, &dv, &db, &dl)) {
                        if (df == 1 && dw2 == 0) port = dv;
                        else if (df == 4 && (dw2 == 5 || dw2 == 0)) { data_dest = dv; has_dest = true; }
                        else if (df == 5 && (dw2 == 5 || dw2 == 0)) { data_source = dv; has_source = true; }
                        else if (df == 7 && (dw2 == 5 || dw2 == 0)) { data_reply_id = dv; has_reply_id = true; }
                        else if (df == 8 && (dw2 == 5 || dw2 == 0)) { data_emoji = dv; has_emoji = true; }
                        else if (df == 9 && dw2 == 0) { data_bitfield = dv; has_bitfield = true; }
                        else if (df == 2 && dw2 == 2 && dl > 0) {
                            // Truncate to our GL text cap (160); longer app
                            // texts still send instead of silently dropping.
                            uint16_t c = dl < (uint16_t)(sizeof(text) - 1) ? dl : (uint16_t)(sizeof(text) - 1);
                            memcpy(text, db, c);
                            text[c] = '\0';
                            got_text = true;
                            got_data = true;
                            data_payload = db;
                            data_payload_len = dl;
                        }
                    }
                    if (port != MESHTASTIC_PORT_TEXT) got_text = false;
                    data_port = port;
                }
            }
            if (got_text) {
                out->kind = TORADIO_PACKET_TEXT;
                out->from = from;
                out->to = to;
                out->msg_id = msg_id;
                out->want_ack = want_ack;
                out->channel = channel > 7 ? 0 : channel;
                out->port = data_port;
                out->data_dest = data_dest;
                out->data_source = data_source;
                out->data_reply_id = data_reply_id;
                out->data_emoji = data_emoji;
                out->data_bitfield = data_bitfield;
                out->has_dest = has_dest;
                out->has_source = has_source;
                out->has_reply_id = has_reply_id;
                out->has_emoji = has_emoji;
                out->has_bitfield = has_bitfield;
                // Keep the full Data for verbatim air TX (emoji/reply threads).
                out->data_complete = (data_msg && data_msg_len <= sizeof(out->payload_head));
                if (out->data_complete) {
                    out->payload_len = data_msg_len;
                    memcpy(out->payload_head, data_msg, data_msg_len);
                } else {
                    out->payload_len = 0;
                }
                memcpy(out->text, text, sizeof(out->text));
                return true;
            }
            if (got_data || data_port != 0) {
                // Non-TEXT MeshPacket (admin/position/telemetry...): report
                // it so the transport can ack/reply; handling lives in
                // lora_phoneapi_on_toradio.
                out->kind = TORADIO_PACKET_OTHER;
                out->from = from;
                out->to = to;
                out->msg_id = msg_id;
                out->want_ack = want_ack;
                out->channel = channel > 7 ? 0 : channel;
                out->port = data_port;
                out->data_dest = data_dest;
                out->data_source = data_source;
                out->data_reply_id = data_reply_id;
                out->data_emoji = data_emoji;
                out->data_bitfield = data_bitfield;
                out->has_dest = has_dest;
                out->has_source = has_source;
                out->has_reply_id = has_reply_id;
                out->has_emoji = has_emoji;
                out->has_bitfield = has_bitfield;
                // Ship the full Data submessage (port+payload+ids) so the
                // phone path can re-encrypt it verbatim for air TX.
                out->data_complete = (data_msg && data_msg_len <= sizeof(out->payload_head));
                if (data_msg && data_msg_len) {
                    out->payload_len = data_msg_len > sizeof(out->payload_head) ?
                        sizeof(out->payload_head) : data_msg_len;
                    memcpy(out->payload_head, data_msg, out->payload_len);
                } else {
                    out->payload_len = data_payload_len;
                    uint16_t c = data_payload_len < sizeof(out->payload_head) ? data_payload_len : sizeof(out->payload_head);
                    if (data_payload && c) memcpy(out->payload_head, data_payload, c);
                }
                // First payload tag = AdminMessage field (port 6), so the
                // dispatcher can switch replies without a full admin parse.
                out->admin_field = 0;
                if (data_port == 6 && data_payload && data_payload_len) {
                    pb_r_t a;
                    pb_r_init(&a, data_payload, data_payload_len);
                    uint8_t af, aw;
                    uint32_t av;
                    const uint8_t *ab;
                    uint16_t al;
                    if (pb_r_next(&a, &af, &aw, &av, &ab, &al)) out->admin_field = af;
                }
                return true;
            }
            return false;
        } else if (field == 3 && wire == 0) {
            out->kind = TORADIO_WANT_CONFIG;
            out->want_config_id = varint;
            return true;
        } else if (field == 4) {
            out->kind = TORADIO_DISCONNECT;
            return true;
        } else if (field == 7 && wire == 2) {
            // Heartbeat{nonce=1} is a NodeInfo refresh request in current
            // clients; nonce 0 is the ordinary queue-status keepalive.
            pb_r_t hb;
            pb_r_init(&hb, bytes, bytes_len);
            uint8_t hf, hw;
            uint32_t hv;
            const uint8_t *hbytes;
            uint16_t hlen;
            while (pb_r_next(&hb, &hf, &hw, &hv, &hbytes, &hlen)) {
                if (hf == 1 && hw == 0) {
                    out->heartbeat_nonce = hv;
                    break;
                }
            }
            out->kind = TORADIO_IGNORED; // heartbeat
            return true;
        }
        // skip anything else
    }
    return false;
}

#else
typedef int lora_pb_stub_guard;
#endif
