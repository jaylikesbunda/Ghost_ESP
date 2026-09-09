// lora_pb.h
// Hand-rolled protobuf codec for the Meshtastic PhoneAPI subset GhostESP
// speaks. Field numbers verified against meshtastic/protobufs master:
//   mesh.proto (MeshPacket/Data/MyNodeInfo/NodeInfo/User/FromRadio/ToRadio),
//   channel.proto, config.proto (LoRaConfig), portnums.proto.
// No nanopb dependency: static buffers only, ~no-PSRAM safe.

#ifndef LORA_PB_H
#define LORA_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Reference constants (upstream) ----
#define MESHTASTIC_PORT_TEXT 1       // PortNum.TEXT_MESSAGE_APP
#define MESHTASTIC_HW_HELTEC_V3 43   // HardwareModel.HELTEC_V3
#define MESHTASTIC_HW_CROWPANEL 97   // HardwareModel.CROWPANEL
#define MESHTASTIC_REGION_US 1       // Config.LoRaConfig.RegionCode.US
#define MESHTASTIC_REGION_EU868 3    // Config.LoRaConfig.RegionCode.EU_868
#define MESHTASTIC_CH_ROLE_PRIMARY 1 // Channel.Role.PRIMARY
#define MESHTASTIC_MIN_APP_VERSION 20000 // low: never nag to update the app

// Build-specific identity reported both over the air and to official apps.
uint32_t lora_pb_local_hardware_model(void);
const char *lora_pb_local_pio_env(void);

// BLE transport UUIDs (upstream src/BluetoothCommon.h)
#define MESHTASTIC_SVC_UUID   "6ba1b218-15a8-461f-9fa8-5dcae273eafd"
#define MESHTASTIC_TORADIO_UUID  "f75c76d2-129e-4dad-a1dd-7866124401e7"
#define MESHTASTIC_FROMRADIO_UUID "2c55e69e-4993-11ed-b878-0242ac120002"
#define MESHTASTIC_FROMNUM_UUID   "ed9da18c-a800-4f66-a670-aa7547e34453"

// ---- Writer ----
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;
    bool overflow;
} pb_w_t;

void pb_w_init(pb_w_t *w, uint8_t *buf, uint16_t cap);
void pb_w_varint(pb_w_t *w, uint8_t field, uint32_t v); // uint32/enum/bool
void pb_w_fixed32(pb_w_t *w, uint8_t field, uint32_t v);
void pb_w_float(pb_w_t *w, uint8_t field, float f);
void pb_w_bytes(pb_w_t *w, uint8_t field, const uint8_t *d, uint16_t n);
void pb_w_string(pb_w_t *w, uint8_t field, const char *s);
void pb_w_msg(pb_w_t *w, uint8_t field, const uint8_t *d, uint16_t n);

// ---- Reader (ToRadio path only) ----
typedef struct {
    const uint8_t *p;
    uint16_t len;
    uint16_t pos;
} pb_r_t;

void pb_r_init(pb_r_t *r, const uint8_t *p, uint16_t len);
// Returns false at end or on malformed input. For LEN fields, *bytes points
// into the input buffer (zero-copy).
bool pb_r_next(pb_r_t *r, uint8_t *field, uint8_t *wire,
               uint32_t *varint, const uint8_t **bytes, uint16_t *bytes_len);

// ---- RX metadata for air->phone MeshPackets (rx_time/snr/rssi) ----
// Upstream stamps every phone-bound packet with reception metadata; the app
// uses rx_time for last-heard display (missing rx_time renders as 1970).
typedef struct {
    uint32_t rx_time; // epoch secs, 0 = omit (no trustworthy clock yet)
    float snr;
    bool has_rssi;
    int32_t rssi;
} pb_rx_meta_t;

// ---- Message builders (all write into caller buffer, return len/0) ----
// MeshPacket{from,to,channel=0,decoded{portnum=TEXT,payload},id,snr,hop} for BLE.
// hop_start mirrors the air header (origin=3) so the app's hop math works.
uint16_t pb_build_mesh_text(uint8_t *out, uint16_t cap,
                            uint32_t from, uint32_t to, uint32_t id,
                            float snr, uint32_t hop_limit, uint32_t hop_start,
                            const char *text);
uint16_t pb_build_fromradio_packet(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   const uint8_t *mesh_pkt, uint16_t mesh_len);
uint16_t pb_build_fromradio_myinfo(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   uint32_t node_num, uint32_t reboot_count,
                                   uint32_t nodedb_count);
uint16_t pb_build_fromradio_nodeinfo(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t num, const char *id_str,
                                     const char *long_name, const char *short_name,
                                     float snr);
uint16_t pb_build_fromradio_nodeinfo_pki(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t num, const char *id_str,
                                         const char *long_name, const char *short_name,
                                         float snr, const uint8_t *pub32);
// Full NodeInfo: pubkey + verified/muted/hops/last_heard for the app list.
uint16_t pb_build_fromradio_nodeinfo_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                          uint32_t num, const char *id_str,
                                          const char *long_name, const char *short_name,
                                          float snr, const uint8_t *pub32,
                                          bool verified, bool muted,
                                          bool hops_valid, uint8_t hops_away,
                                          uint32_t last_heard);
// Extended NodeInfo (mesh.proto): position=3 / device_metrics=6 as opaque
// submessage bytes (stored/forwarded verbatim so the app map works),
// channel=7 (0 omitted = primary/default), via_mqtt=8,
// is_favorite=10, is_ignored=11. NULL/0-len blobs omitted.
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
                                          uint32_t hw_model, uint32_t role);
uint16_t pb_build_fromradio_config_lora(uint8_t *out, uint16_t cap, uint32_t from_num,
                                        uint8_t region_us_eu, uint32_t hop_limit,
                                        int32_t tx_power);
uint16_t pb_build_fromradio_channel(uint8_t *out, uint16_t cap, uint32_t from_num);
uint16_t pb_build_fromradio_channel_idx(uint8_t *out, uint16_t cap, uint32_t from_num,
                                        uint32_t index, uint32_t role, bool with_settings);
uint16_t pb_build_fromradio_channel_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t index, uint32_t role,
                                         const char *name,
                                         const uint8_t *psk, uint16_t psk_len,
                                         bool uplink, bool downlink);
// Extended ChannelSettings (channel.proto): module_settings=7 as opaque
// ModuleSettings bytes (position_precision=1, is_muted=2) both directions.
// NULL/0-len omitted.
uint16_t pb_build_fromradio_channel_full_ex(uint8_t *out, uint16_t cap, uint32_t from_num,
                                         uint32_t index, uint32_t role,
                                         const char *name,
                                         const uint8_t *psk, uint16_t psk_len,
                                         bool uplink, bool downlink,
                                         const uint8_t *mod_settings, uint16_t mod_len);
uint16_t pb_build_fromradio_metadata(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     const char *fw_version);
// DeviceMetadata (mesh.proto) with role=7 (CLIENT=0 omitted) + hasPKC=11=1.
uint16_t pb_build_fromradio_metadata_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     const char *fw_version, int role);
uint16_t pb_build_fromradio_uiconfig(uint8_t *out, uint16_t cap, uint32_t from_num);
uint16_t pb_build_fromradio_region(uint8_t *out, uint16_t cap, uint32_t from_num);
uint16_t pb_build_fromradio_config_sec(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t section);
uint16_t pb_build_fromradio_config_raw(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t section, const uint8_t *raw, uint16_t raw_len);
uint16_t pb_build_fromradio_module(uint8_t *out, uint16_t cap, uint32_t from_num,
                                   uint8_t module);
uint16_t pb_build_fromradio_module_raw(uint8_t *out, uint16_t cap, uint32_t from_num,
                                       uint8_t module, const uint8_t *raw, uint16_t raw_len);
uint16_t pb_build_fromradio_config_lora_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                             bool use_preset, uint32_t preset,
                                             uint32_t bw, uint32_t sf, uint32_t cr,
                                             float freq_offset, uint8_t region,
                                             uint32_t hop_limit, bool tx_enabled,
                                             int32_t tx_power, uint32_t channel_num,
                                             float override_freq);
// LoRaConfig extras the modem core doesn't apply (config.proto): preserved
// verbatim so the app handshake converges instead of re-pushing every link:
// override_duty_cycle=12, sx126x_rx_boosted_gain=13, pa_fan_disabled=15,
// ignore_incoming=103 (repeated), ignore_mqtt=104, config_ok_to_mqtt=105,
// fem_lna_mode=106, serial_hal_only=107.
typedef struct {
    bool override_duty_cycle;
    bool sx126x_rx_boosted_gain;
    bool pa_fan_disabled;
    bool ignore_mqtt;
    bool config_ok_to_mqtt;
    bool serial_hal_only;
    bool has_fem_lna_mode;
    uint32_t fem_lna_mode;
    uint32_t ignore_incoming[8];
    uint8_t n_ignore_incoming;
} pb_lora_extras_t;

void pb_parse_lora_extras(const uint8_t *p, uint16_t len, pb_lora_extras_t *out);
void pb_w_lora_extras(pb_w_t *w, const pb_lora_extras_t *ex);
// LoRaConfig builder with preserved extras appended (ex may be NULL).
uint16_t pb_build_fromradio_config_lora_full_ex(uint8_t *out, uint16_t cap, uint32_t from_num,
                                             bool use_preset, uint32_t preset,
                                             uint32_t bw, uint32_t sf, uint32_t cr,
                                             float freq_offset, uint8_t region,
                                             uint32_t hop_limit, bool tx_enabled,
                                             int32_t tx_power, uint32_t channel_num,
                                             float override_freq,
                                             const pb_lora_extras_t *ex);
uint16_t pb_build_fromradio_complete(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t nonce);
uint16_t pb_build_fromradio_queue(uint8_t *out, uint16_t cap, uint32_t from_num);
uint16_t pb_build_fromradio_queue_id(uint8_t *out, uint16_t cap, uint32_t from_num,
                                     uint32_t mesh_packet_id);
// Data{portnum, payload?, request_id?} — NULL/empty payload omitted (Routing
// ACKs and empty admin replies encode exactly like upstream: port + req id).
uint16_t pb_build_data_msg(uint8_t *out, uint16_t cap, uint32_t portnum,
                           const uint8_t *payload, uint16_t plen, uint32_t request_id);
// Full Data round-trip (mesh.proto): dest=4/source=5/reply_id=7/emoji=8 are
// fixed32, bitfield=9 is varint. has_*=false omits the field.
uint16_t pb_build_data_msg_full(uint8_t *out, uint16_t cap, uint32_t portnum,
                           const uint8_t *payload, uint16_t plen, uint32_t request_id,
                           bool has_dest, uint32_t dest,
                           bool has_source, uint32_t source,
                           bool has_reply_id, uint32_t reply_id,
                           bool has_emoji, uint32_t emoji,
                           bool has_bitfield, uint32_t bitfield);
// MeshPacket{from,to,channel=0,decoded{portnum=TEXT,payload},id,snr,hop...} for FromRadio.packet replies.
uint16_t pb_build_mesh_packet(uint8_t *out, uint16_t cap,
                              uint32_t from, uint32_t to, uint32_t id,
                              const uint8_t *data, uint16_t dlen);
uint16_t pb_build_mesh_packet_ch(uint8_t *out, uint16_t cap,
                                 uint32_t from, uint32_t to, uint32_t id,
                                 uint32_t channel_idx,
                                 const uint8_t *data, uint16_t dlen);
uint16_t pb_build_mesh_packet_pki(uint8_t *out, uint16_t cap,
                                  uint32_t from, uint32_t to, uint32_t id,
                                  uint32_t channel_idx,
                                  const uint8_t *sender_pub32,
                                  const uint8_t *data, uint16_t dlen);
// RX-forward variants with reception metadata + actual hop counts.
uint16_t pb_build_mesh_text_rx(uint8_t *out, uint16_t cap,
                               uint32_t from, uint32_t to, uint32_t id,
                               uint32_t hop_limit, uint32_t hop_start,
                               uint32_t channel_idx, const char *text,
                               const pb_rx_meta_t *meta);
uint16_t pb_build_mesh_packet_rx(uint8_t *out, uint16_t cap,
                                 uint32_t from, uint32_t to, uint32_t id,
                                 uint32_t channel_idx,
                                 uint32_t hop_limit, uint32_t hop_start,
                                 const uint8_t *data, uint16_t dlen,
                                 const pb_rx_meta_t *meta);
uint16_t pb_build_mesh_packet_pki_rx(uint8_t *out, uint16_t cap,
                                     uint32_t from, uint32_t to, uint32_t id,
                                     uint32_t channel_idx,
                                     uint32_t hop_limit, uint32_t hop_start,
                                     const uint8_t *sender_pub32,
                                     const uint8_t *data, uint16_t dlen,
                                     const pb_rx_meta_t *meta);
uint16_t pb_build_mesh_text_ch(uint8_t *out, uint16_t cap,
                               uint32_t from, uint32_t to, uint32_t id,
                               float snr, uint32_t hop_limit, uint32_t hop_start,
                               uint32_t channel_idx, const char *text);
// Config{device{tzdef?}} — tzdef NULL/empty emits the same empty section as
// the generic builder; a stored zone is reported so the app stops pushing it.
uint16_t pb_build_fromradio_config_device(uint8_t *out, uint16_t cap, uint32_t from_num,
                                          const char *tzdef);
uint16_t pb_build_fromradio_config_device_full(uint8_t *out, uint16_t cap, uint32_t from_num,
                                               int role, const char *tzdef);

// ---- ToRadio parse (phone -> device) ----
typedef enum {
    TORADIO_NONE = 0,
    TORADIO_PACKET_TEXT, // MeshPacket decoded TEXT; text/to filled
    TORADIO_PACKET_OTHER, // MeshPacket decoded non-TEXT; port/payload_len filled
    TORADIO_WANT_CONFIG,
    TORADIO_DISCONNECT,
    TORADIO_IGNORED, // heartbeat / unknown: ack by ignoring
} toradio_kind_t;

typedef struct {
    toradio_kind_t kind;
    uint32_t want_config_id;
    uint32_t heartbeat_nonce;
    uint32_t from;          // MeshPacket.from (app fills with our nodenum)
    uint32_t to;            // MeshPacket.to (broadcast or node)
    uint32_t msg_id;        // MeshPacket.id (echo as request_id in replies)
    bool want_ack;          // MeshPacket.want_ack
    uint32_t channel;       // MeshPacket.channel index (0..7)
    uint32_t port;          // Data.portnum for PACKET_OTHER (1 for TEXT w/ Data)
    uint32_t admin_field;   // AdminMessage field no. for admin port (0 if none)
    // Data routing/reply extras (mesh.proto): dest=4/source=5/reply_id=7/
    // emoji=8 fixed32, bitfield=9 varint. has_* false = absent on the wire.
    uint32_t data_dest;
    uint32_t data_source;
    uint32_t data_reply_id;
    uint32_t data_emoji;
    uint32_t data_bitfield;
    bool has_dest;
    bool has_source;
    bool has_reply_id;
    bool has_emoji;
    bool has_bitfield;
    bool data_complete;   // payload_head holds the entire Data submessage
    uint16_t payload_len;   // Data.payload length for PACKET_OTHER
    uint8_t payload_head[233]; // complete Meshtastic Data payload, including admin settings
    char text[161];
} toradio_t;

bool pb_parse_toradio(const uint8_t *p, uint16_t len, toradio_t *out);

#ifdef __cplusplus
}
#endif

#endif // LORA_PB_H
