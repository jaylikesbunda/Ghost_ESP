// lora_mesh.h
// Meshtastic-compatible air wire (primary + secondary channels, PKI DMs).
// Reference: meshtastic/firmware src/mesh/RadioInterface.h (PacketHeader,
//   16B: to,from,id LE u32 + flags + channel + next_hop + relay_node;
//   flags = hop_limit&7 | want_ack?8 | (hop_start&7)<<5) +
//   src/mesh/Channels.cpp (channel hash, default key) +
//   src/mesh/CryptoEngine.cpp (AES-CTR nonce [id u64][from u32]) +
//   src/mesh/FloodingRouter.cpp / ReliableRouter.cpp / NextHopRouter.cpp /
//   RoutingModule.cpp / TraceRouteModule.cpp (flood + reliable + ACK/NAK).
// Payload = AES-CTR(Data protobuf) on a channel PSK, or X25519+CCM PKI DM
// (channel byte 0). PortNums handled locally: TEXT(1), NODEINFO(4),
// ROUTING(5), TRACEROUTE(70); other module ports are flooded and forwarded
// to the phone opaquely (telemetry/position/admin/waypoint/...).

#ifndef LORA_MESH_H
#define LORA_MESH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "managers/lora_modem.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_MESH_TEXT_MAX 160
// Match the native Heltec V3 NodeDB RAM capacity without reserving copies of
// the table. Flash persistence remains bounded for the 0x7000 NVS partition.
#define LORA_MESH_NODES_MAX 200
// Keep the larger discovery history exclusive to PSRAM-class builds.  The
// records themselves live in NVS, but the wider index/config snapshots and
// reply cache consume runtime RAM while loading and syncing.
#if defined(CONFIG_SPIRAM)
#define LORA_MESH_PERSIST_MAX 96
#else
#define LORA_MESH_PERSIST_MAX 32
#endif
#define LORA_MESH_TTL 3
#define LORA_MESH_BROADCAST 0xFFFFFFFFu

typedef struct {
    uint32_t node_num;
    uint32_t last_seen_ms;
    uint8_t pubkey[32]; // Curve25519, valid when has_pubkey
    char long_name[25]; // compact cached prefix; wire User accepts 40 bytes
    char short_name[5]; // native on-air maximum: 4 UTF-8 bytes + NUL
    int16_t last_rssi;
    uint16_t hw_model; // HardwareModel values are protobuf enums
    float last_snr;
    uint8_t role; // DeviceConfig.Role enum (0 = CLIENT default)
    uint8_t channel; // decoded channel index used to reach this peer
    uint8_t hops_away; // hs-hl of last RX, valid when hops_valid
    uint8_t has_user : 1; // false until NODEINFO_APP / SharedContact
    uint8_t has_pubkey : 1;
    uint8_t key_verified : 1; // manual verification (DO_VERIFY)
    uint8_t muted : 1; // local silence; still relays
    uint8_t ignored : 1; // never relayed; default false
    uint8_t hops_valid : 1;
    uint8_t favorite : 1; // protects CLIENT_BASE relay paths
    uint8_t _reserved_flags : 1;
} lora_mesh_node_t;

// Keep the 200-entry table within the Heltec V3's internal-RAM budget.
#ifdef __cplusplus
static_assert(sizeof(lora_mesh_node_t) <= 84, "lora_mesh_node_t grew unexpectedly");
#else
_Static_assert(sizeof(lora_mesh_node_t) <= 84, "lora_mesh_node_t grew unexpectedly");
#endif

// Must be called once (NVS nodenum load-or-create inside).
void lora_mesh_init(void);
void lora_mesh_set_hop_limit(uint8_t hop_limit);
void lora_mesh_set_modem(int sf, int bw_khz, int cr);
void lora_mesh_owner(char *long_name, size_t long_cap, char *short_name, size_t short_cap);
bool lora_mesh_set_owner(const char *long_name, const char *short_name);
uint32_t lora_mesh_node_num(void);

// Air frequency for an upstream region code under LongFast/BW250.
// Verified against upstream slot math (djb2("LongFast")%slots):
// US(1)->906.875, EU_868(3)->869.525 MHz.
uint32_t lora_air_freq_hz(int pb_code);
uint32_t lora_air_freq_hz_ex(int pb_code, const lora_modem_cfg_t *mcfg);

// Region table (STD-profile + EU_868 LongFast-legal regions).
int lora_region_count(void);
int lora_region_code(int index);          // upstream pb code
const char *lora_region_name(int code);   // "US915", "?" if unknown
const char *lora_region_name_by_index(int index);
int lora_region_by_name(const char *name); // pb code or -1
int lora_region_next(int code);           // cycle for UI pickers
float lora_region_duty(int code);         // legal duty-cycle percent

// Duty-cycle airtime guard (rolling 1h window). allow() estimates the frame
// airtime (Semtech formula, SF11/BW250/CR4/5/pre16) and denies when the
// region allowance would be exceeded; record() logs completed TX airtime.
bool lora_duty_allow(uint8_t frame_len);
void lora_duty_record(uint8_t frame_len);
void lora_duty_set_region(int pb_code);
uint32_t lora_duty_drops(void);

// Build a stock TX frame carrying TEXT. id = random nonzero u32.
// Returns frame len (0 = error); out_id gets the air id when non-NULL.
uint8_t lora_mesh_build_text(const char *text, uint8_t *out_frame, uint8_t out_max);
uint8_t lora_mesh_build_text_to(const char *text, uint32_t to,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id);
uint8_t lora_mesh_build_text_to_id(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint8_t *out_frame, uint8_t out_max,
                                   uint32_t *out_id);
// Channel-aware variants (channel_idx 0..7). Returns 0 on disabled channel.
uint8_t lora_mesh_build_text_to_id_ch(const char *text, uint32_t to,
                                      uint32_t packet_id, bool want_ack,
                                      uint8_t channel_idx,
                                      uint8_t *out_frame, uint8_t out_max,
                                      uint32_t *out_id);
uint8_t lora_mesh_build_data_ch(uint32_t to, uint8_t portnum,
                                const uint8_t *payload, uint16_t plen,
                                uint32_t request_id, uint32_t packet_id,
                                bool want_ack, bool want_response,
                                uint8_t channel_idx,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id);
// Channel-encrypt an already serialized Meshtastic Data submessage.
uint8_t lora_mesh_build_data_verbatim_ch(uint32_t to,
                                         const uint8_t *data, uint16_t dlen,
                                         uint32_t packet_id, bool want_ack,
                                         uint8_t channel_idx,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id);
// PKI direct-message frame: Data encrypted to the peer's public key
// (X25519+CCM, +12B). Header channel byte is 0 (upstream perhapsEncode:
// receiver tries PKI first only when channel == 0). Relays flood it like
// any frame when rebroadcast mode allows. Returns 0 when peer key unknown.
uint8_t lora_mesh_build_dm(uint32_t to, uint8_t portnum,
                           const uint8_t *payload, uint16_t plen,
                           uint32_t request_id, uint32_t packet_id,
                           bool want_ack, bool want_response,
                           uint8_t *out_frame, uint8_t out_max,
                           uint32_t *out_id);
// PKI-encrypt an already serialized Meshtastic Data submessage.  This is the
// lossless PhoneAPI path for reply_id/emoji/dest/source/bitfield fields.
uint8_t lora_mesh_build_dm_data(uint32_t to,
                                const uint8_t *data, uint16_t dlen,
                                uint32_t packet_id, bool want_ack,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id);
uint8_t lora_mesh_build_dm_text(const char *text, uint32_t to,
                                uint32_t packet_id, bool want_ack,
                                uint8_t *out_frame, uint8_t out_max,
                                uint32_t *out_id);
// Try PKI decrypt of one air frame addressed to us. Returns Data len
// (0 = not PKI / unknown key / auth fail); out_port gets the Data portnum.
uint16_t lora_mesh_try_pki(const uint8_t *frame, uint8_t len,
                           uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                           uint8_t *out_data, uint16_t data_cap,
                           uint8_t *out_port, bool *out_want_ack);
// PKI decrypt-variant probe (diagnostic only, no RX/TX behavior change):
// parse header, load the sender's stored peer key, and try stock-framing
// variants (key derivation / nonce / tail layout), printing one ESP_LOGI
// line per variant (OK with port + payload hex/ASCII, or fail with ccm rc).
void lora_mesh_pktry(const uint8_t *frame, uint8_t len);
uint8_t lora_mesh_build_routing_ack(uint32_t to, uint32_t request_id,
                                    uint8_t *out_frame, uint8_t out_max,
                                    uint32_t *out_id);
// Hop-aware ACK: hop_limit 0 when the inbound leg was direct
// (hop_start==hop_limit, 0 hops used), else hopsUsed+2 clamped to 0..7.
// want_ack_on_ack mirrors upstream for TEXT DMs (ACK itself requests ACK).
uint8_t lora_mesh_build_routing_ack_hops(uint32_t to, uint32_t request_id,
                                         uint8_t ack_hops,
                                         bool want_ack_on_ack,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id);
// Routing NAK (Routing{error_reason=err}, Data.request_id=request_id).
uint8_t lora_mesh_build_routing_nak(uint32_t to, uint32_t request_id,
                                    uint8_t err_reason,
                                    uint8_t ack_hops,
                                    uint8_t *out_frame, uint8_t out_max,
                                    uint32_t *out_id);
// Routing error codes (mesh.proto Routing.Error subset used on air/phone).
#define LORA_ROUTING_NONE 0
#define LORA_ROUTING_NO_CHANNEL 6
#define LORA_ROUTING_MAX_RETRANSMIT 5
#define LORA_ROUTING_DUTY_CYCLE_LIMIT 9
#define LORA_ROUTING_PKI_FAILED 34 // upstream Routing.Error PKI_FAILED
#define LORA_ROUTING_PKI_UNKNOWN_PUBKEY 35 // upstream Routing.Error PKI_UNKNOWN_PUBKEY (was 10, undefined upstream)

// ---- Flood parity (FloodingRouter / ReliableRouter / NextHopRouter) ----
// DeviceConfig.Role values used by the routing scheduler.
#define LORA_MESH_ROLE_CLIENT 0
#define LORA_MESH_ROLE_CLIENT_MUTE 1
#define LORA_MESH_ROLE_ROUTER 2
#define LORA_MESH_ROLE_TRACKER 5
#define LORA_MESH_ROLE_SENSOR 6
#define LORA_MESH_ROLE_CLIENT_HIDDEN 8
#define LORA_MESH_ROLE_TAK_TRACKER 10
#define LORA_MESH_ROLE_ROUTER_LATE 11
#define LORA_MESH_ROLE_CLIENT_BASE 12
// DeviceConfig.RebroadcastMode values from config.proto. Keep these wire
// values exact: they are sent by official apps and may later be persisted.
#define LORA_MESH_RB_ALL 0
#define LORA_MESH_RB_ALL_SKIP_DECODING 1
#define LORA_MESH_RB_LOCAL_ONLY 2   // never flood foreign (undecryptable)
#define LORA_MESH_RB_KNOWN_ONLY 3   // flood only known peers/channels
#define LORA_MESH_RB_NONE 4
#define LORA_MESH_RB_CORE_ONLY 5    // foreign: drop non-core module ports
void lora_mesh_set_role(int role);
int lora_mesh_get_role(void);
void lora_mesh_set_rebroadcast_mode(int mode);
int lora_mesh_get_rebroadcast_mode(void);
// Basic hop-accounting query. The relay scheduler additionally checks the
// previous relay's favorite/router identity before preserving a later hop.
bool lora_mesh_should_decrement_hop(uint8_t hops_away);
// Per-destination channel select stub (currently primary; future: per-dest).
uint8_t lora_mesh_channel_for_dest(uint32_t to, uint8_t requested);
// Position precision hook (originator-only bitmask before encode; config
// Precision, default 32-bit full = no-op). Bits 32/24/16 supported.
void lora_mesh_set_position_precision(int bits);
uint32_t lora_mesh_position_mask_lat(uint32_t lat_i);
uint32_t lora_mesh_position_mask_lon(uint32_t lon_i);
// Reliable unicast: bounded multi-packet queue. Native originators transmit
// up to three times total (initial + two retries), with airtime/slot-derived
// deadlines. Cleared by an air ACK or implicit ACK.
void lora_mesh_reliable_track(const uint8_t *frame, uint8_t len);
void lora_mesh_reliable_on_air_ack(uint32_t request_id, uint32_t from,
                                   uint8_t relay_node);
void lora_mesh_reliable_on_heard(uint32_t from, uint32_t id);
void lora_mesh_cancel_rebroadcast(uint32_t from, uint32_t id);
// Duty utilization percent (0..100+) for the channel-util guard.
float lora_duty_used_pct(void);
// NeighborInfo module gate (default drop unless enabled).
void lora_mesh_set_neighborinfo_enabled(bool en);
bool lora_mesh_neighborinfo_enabled(void);
// Own position store (raw Position submessage, capped): set when the phone
// sends position to self; readable for NodeInfo advertisement. The air
// rebroadcast of such packets goes to BROADCAST, never dest=self.
void lora_mesh_set_self_position(const uint8_t *raw, uint16_t len);
uint16_t lora_mesh_get_self_position(uint8_t *out, uint16_t cap);
// Traceroute (port 70): append own ID when relaying (opaque forward), and
// build a route_back reply when destination is self. Returns 0 on error.
uint8_t lora_mesh_build_traceroute_reply(uint32_t to, uint32_t request_id,
                                         const uint8_t *rx_payload,
                                         uint16_t rx_plen,
                                         uint8_t *out_frame, uint8_t out_max,
                                         uint32_t *out_id);
// Traceroute request (port 70): empty RouteDiscovery, unicast with
// want_ack + want_response. Returns 0 on error.
uint8_t lora_mesh_build_traceroute_request(uint32_t to,
                                           uint8_t *out_frame, uint8_t out_max,
                                           uint32_t *out_id);
bool lora_mesh_send_nodeinfo(uint32_t to, bool request_replies);
// Send NodeInfo on the channel where a request was received. Native replies
// on the request packet's channel instead of always forcing the primary.
bool lora_mesh_send_nodeinfo_ch(uint32_t to, bool request_replies,
                                 uint8_t channel_idx);
// Native MeshModule reply semantics for an inbound NodeInfo request: preserve
// its packet id/channel/reliability and choose the return-path hop limit.
bool lora_mesh_send_nodeinfo_reply(uint32_t to, uint8_t channel_idx,
                                   uint32_t request_id, bool want_ack,
                                   uint8_t hop_limit);
// Ask the radio task to send an immediate broadcast NodeInfo request. This is
// used when a phone connects after the normal three-hour announcement window.
void lora_mesh_request_nodeinfo(void);
// Native NodeInfoModule-style 12-hour suppression for repeated replies.
bool lora_mesh_nodeinfo_reply_allowed(uint32_t from);
// Queue our NodeInfo directly to a peer. Used after the native
// PKI_UNKNOWN_PUBKEY handshake NAK so the peer can learn our public key.
void lora_mesh_request_nodeinfo_to(uint32_t to, uint8_t channel_idx);

// Decode the encrypted Data protobuf for routing/ack handling. out_data gets
// the complete decrypted Data message suitable for forwarding to PhoneAPI.
bool lora_mesh_decode_data(const uint8_t *frame, uint8_t len,
                           uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                           bool *out_want_ack, uint8_t *out_port,
                           uint32_t *out_request_id,
                           uint8_t *out_data, uint16_t data_cap, uint16_t *out_data_len);
// Extended decode that also resolves the air channel hash to a channel index
// (0..7, primary fallback) for the phone's MeshPacket.channel field.
bool lora_mesh_decode_data_ch(const uint8_t *frame, uint8_t len,
                              uint32_t *out_from, uint32_t *out_to, uint32_t *out_id,
                              bool *out_want_ack, uint8_t *out_port,
                              uint32_t *out_request_id, uint8_t *out_ch_idx,
                              uint32_t *out_hop_start, uint32_t *out_hop_limit,
                              uint8_t *out_data, uint16_t data_cap, uint16_t *out_data_len);

// Feed one RX frame from the driver. TRUE chat delivery only for TEXT
// addressed to us/broadcast. NODEINFO updates the DB silently (returns
// false). out_* valid only on true.
bool lora_mesh_on_rx(const uint8_t *frame, uint8_t len,
                     int16_t rssi, float snr,
                     char *out_who, size_t who_max,
                     char *out_text, size_t text_max);
bool lora_mesh_on_rx_ex(const uint8_t *frame, uint8_t len,
                        int16_t rssi, float snr,
                        char *out_who, size_t who_max,
                        char *out_text, size_t text_max,
                           uint32_t *out_from, uint32_t *out_id, uint32_t *out_to,
                           uint32_t *out_hop_start, uint32_t *out_hop_limit);

// Queue a native-style delayed flood. Works on any well-formed header (even
// undecryptable foreign-key frames in ALL mode). The radio tick performs the
// actual TX so a duplicate heard during the contention window can cancel it.
bool lora_mesh_schedule_rebroadcast(const uint8_t *frame, uint8_t len,
                                    float rx_snr);

// Routing TX counters generated inside the tick-driven queues.
uint32_t lora_mesh_relay_sent(void);
uint32_t lora_mesh_relay_failed(void);
void lora_mesh_routing_reset(void);

// Background tick (call ~5Hz from the radio loop): NodeInfo broadcast at
// boot + every 3h (upstream default_node_info_broadcast_secs).
void lora_mesh_tick(void);

// Nodes (RAM LRU, native Heltec V3 capacity). Bulk copy is retained for
// compatibility; PhoneAPI, CLI, and RX paths use the one-record APIs.
uint16_t lora_mesh_nodes(lora_mesh_node_t *out, uint16_t max);
bool lora_mesh_node_get(uint32_t node_num, lora_mesh_node_t *out);
bool lora_mesh_node_at(uint16_t index, lora_mesh_node_t *out);
uint16_t lora_mesh_node_ids(uint32_t *out, uint16_t max);
bool lora_mesh_peer_short_name(uint32_t node_num, char *out, size_t cap);
bool lora_mesh_remove_node(uint32_t node_num);
// Clear only the in-RAM table; admin NodeDB reset erases its NVS index/blobs
// in one transaction instead of committing once per peer.
uint16_t lora_mesh_clear_nodes(void);
// Peer public-key store (learned from NodeInfo User.public_key).
bool lora_mesh_peer_pubkey(uint32_t node_num, uint8_t *out32);
bool lora_mesh_peer_set_pubkey(uint32_t node_num, const uint8_t *pub32);
// Apply a phone SharedContact using native NodeDB::addFromContact semantics:
// replace the complete User, but protect an already manually verified key.
bool lora_mesh_peer_apply_contact(uint32_t node_num,
                                  const char *long_name,
                                  const char *short_name,
                                  uint32_t hw_model, uint32_t role,
                                  const uint8_t *pub32, bool has_pubkey,
                                  bool manually_verified, bool should_ignore);
// Native air-NodeInfo trust rule: once a 32-byte key is known, reject the
// entire NodeInfo if its key is absent/different; reject our own key on peers.
bool lora_mesh_peer_nodeinfo_key_ok(uint32_t node_num,
                                    const uint8_t *pub32, bool has_pubkey);
bool lora_mesh_peer_set_verified(uint32_t node_num, bool verified);
bool lora_mesh_peer_set_muted(uint32_t node_num, bool muted);
bool lora_mesh_peer_set_favorite(uint32_t node_num, bool favorite);
// Ignored set (never relayed; creates an entry for unknown nodes so a
// pre-learned ignore sticks). Muted still relays.
bool lora_mesh_peer_set_ignored(uint32_t node_num, bool ignored);
// Flag mirrors for the manager (both default false for unknown nodes).
bool lora_mesh_peer_get_muted(uint32_t node_num);
bool lora_mesh_peer_get_ignored(uint32_t node_num);
void lora_mesh_peer_set_hw_role(uint32_t node_num, uint32_t hw_model, uint32_t role);
void lora_mesh_peer_set_channel(uint32_t node_num, uint8_t channel);
// Lightweight flag read without copying the table (RX hot path).
void lora_mesh_peer_meta(uint32_t node_num, bool *verified, bool *muted,
                         bool *hops_valid, uint8_t *hops_away);
// Record observed hop distance (hs-hl) for the NodeInfo hops_away field.
void lora_mesh_note_hops(uint32_t node_num, uint8_t hops_away);
uint32_t lora_mesh_dups(void);

#ifdef __cplusplus
}
#endif

#endif // LORA_MESH_H
