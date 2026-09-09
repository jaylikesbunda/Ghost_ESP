// lora_manager.h
// Multi-board LoRa (SX1262/SX1276/LLCC68) manager: Meshtastic-compatible
// lite node (stock RF framing + default-key crypto + flood + BLE PhoneAPI).
// All hot-path RAM is statically pooled, safe on no-PSRAM single-core
// boards (Heltec V3). See docs/lora-meshtastic-app.md for the reference.

#ifndef LORA_MANAGER_H
#define LORA_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LORA_MODULE_SX1262 = 0,
    LORA_MODULE_SX1276,
    LORA_MODULE_LLCC68,
} lora_module_t;

// Region = upstream pb code (meshtastic_Config_LoRaConfig_RegionCode_*).
// Only LongFast-legal STD regions + EU_868 are populated (see lora_mesh.c).
typedef enum {
    LORA_REGION_US915 = 1,
    LORA_REGION_EU868 = 3,
    LORA_REGION_EU433 = 2,
    LORA_REGION_CN = 4,
    LORA_REGION_JP = 5,
    LORA_REGION_ANZ = 6,
    LORA_REGION_KR = 7,
    LORA_REGION_TW = 8,
    LORA_REGION_RU = 9,
    LORA_REGION_IN = 10,
    LORA_REGION_NZ865 = 11,
    LORA_REGION_TH = 12,
    LORA_REGION_UA433 = 14,
    LORA_REGION_MY433 = 16,
    LORA_REGION_MY919 = 17,
    LORA_REGION_SG923 = 18,
    LORA_REGION_PH433 = 19,
    LORA_REGION_PH868 = 20,
    LORA_REGION_PH915 = 21,
    LORA_REGION_ANZ433 = 22,
    LORA_REGION_KZ433 = 23,
    LORA_REGION_KZ863 = 24,
    LORA_REGION_NP865 = 25,
    LORA_REGION_BR902 = 26,
} lora_region_t;

typedef enum {
    LORA_COMPANION_BLE = 0,
    LORA_COMPANION_WIFI,
} lora_companion_t;

// Board descriptor — new boards only add Kconfig pins + this struct,
// never fork the driver.
typedef struct {
    int spi_host;          // 2 = SPI2_HOST, 3 = SPI3_HOST
    int mosi_pin;
    int miso_pin;
    int sck_pin;
    int nss_pin;
    int dio1_pin;
    int busy_pin;
    int rst_pin;           // -1 if not wired
    lora_module_t module;
    bool tcxo_controlled;  // Heltec V3: DIO3 TCXO via driver
    uint8_t tcxo_voltage_code; // SX126x SetDIO3AsTCXOCtrl voltage selector
    bool dio2_rf_switch;   // Heltec V3: DIO2 RF switch via driver
    int max_tx_dbm;        // board/regulatory cap
} lora_hw_t;

typedef struct {
    bool running;
    bool radio_present;
    lora_region_t region;
    uint32_t freq_hz;
    int sf;                // 5..12
    int bw_khz;            // 125/250/500
    int cr;                // 5..8 (CR 4/5..4/8)
    bool use_preset;
    int preset;            // 0..16
    float freq_offset_mhz;
    float override_freq_mhz;
    uint32_t channel_num;
    bool tx_enabled;
    int role;              // DeviceConfig.Role
    int tx_dbm;
    int hop_limit;
    lora_companion_t companion;
    uint32_t tx_ok;
    uint32_t tx_fail;
    uint32_t tx_relay; // flood forwards (stock interop, not local chat)
    uint32_t rx_ok;
    uint32_t rx_crc_err;
    uint32_t rx_dups;
    uint32_t q_drops;
    uint32_t duty_drops; // refused by the region duty-cycle guard
    int last_rssi;
    float last_snr;
    int node_count;
} lora_status_t;

typedef struct {
    char who[24];
    char text[160];
    uint32_t node_num;      // peer node for direct messages, if known
    uint32_t timestamp_ms;  // local uptime when the line entered the ring
    bool outgoing;
    bool direct;
    bool read;
    uint8_t delivery; // 0: sent/received, 1: awaiting ACK, 2: delivered, 3: failed
    uint32_t packet_id;
} lora_msg_t;

void lora_manager_chat_result(uint32_t peer, uint32_t id, bool success);
void lora_manager_chat_read(uint32_t peer); // zero selects public chat

// Lifecycle
void lora_manager_early_init(void);   // NVS load only, no SPI
// Run startup initialization from a dedicated task so the ESP-IDF main task
// does not carry the nested LoRa/NVS/PKI initialization stack.
bool lora_manager_early_init_off_main(void);
bool lora_manager_start(void);        // exclusive-mode + probe + RX
void lora_manager_stop(void);
bool lora_manager_is_running(void);
bool lora_manager_is_present(void);   // probe result
const char *lora_manager_last_error(void);

// Params (persisted to NVS, applied on start)
bool lora_manager_set_region(lora_region_t region);
bool lora_manager_set_params(int sf, int bw_khz, int tx_dbm);
bool lora_manager_set_modem(int sf, int bw_khz, int cr_denom);
bool lora_manager_set_preset(int preset);
bool lora_manager_set_custom(int sf, int bw_khz, int cr_denom);
bool lora_manager_set_freq_offset(float mhz);
bool lora_manager_set_override_freq(float mhz);
bool lora_manager_set_channel_num(uint32_t num);
bool lora_manager_set_tx_enabled(bool en);
bool lora_manager_set_role(int role);
int lora_manager_get_role(void);
bool lora_manager_get_modem_cfg(bool *use_preset, int *preset, int *sf,
                                int *bw_khz, int *cr, float *freq_offset,
                                float *override_freq, uint32_t *channel_num,
                                bool *tx_enabled);
// Apply the supported stock LongFast settings from the phone without dropping BLE.
bool lora_manager_apply_app_radio(lora_region_t region, int tx_dbm);
// Full LoRaConfig apply from admin set_config (deferred reconfigure).
bool lora_manager_apply_lora_cfg(lora_region_t region, bool use_preset, int preset,
                                 int sf, int bw_khz, int cr,
                                 float freq_offset, float override_freq,
                                 uint32_t channel_num, bool tx_enabled,
                                 int tx_power, int hop_limit);
bool lora_manager_set_hop_limit(int hop_limit);
bool lora_manager_set_companion(lora_companion_t companion);
// Channel store (8 slots, persisted, applied immediately to air crypto).
bool lora_manager_set_channel(uint8_t idx, const char *name, const uint8_t *psk,
                              uint8_t psk_len, uint8_t role, bool uplink, bool downlink);
bool lora_manager_disable_channel(uint8_t idx);

// Chat: CLI/serial entry points. lora_manager_send_text broadcasts;
// lora_manager_send_app_text targets an explicit node (BLE app path).
bool lora_manager_send_text(const char *text);
// App/BLE path: text to an explicit destination; out_id gets the air id.
bool lora_manager_send_app_text(const char *text, uint32_t to, uint32_t *out_id);
bool lora_manager_send_app_text_ex(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint32_t *out_id);
bool lora_manager_send_app_text_ch(const char *text, uint32_t to,
                                   uint32_t packet_id, bool want_ack,
                                   uint8_t channel_idx, uint32_t *out_id);
// PKI direct message (encrypted to the peer's public key). Returns false
// when the peer key is unknown (exchange NodeInfo first).
bool lora_manager_send_dm_text(const char *text, uint32_t to,
                               uint32_t packet_id, bool want_ack,
                               uint32_t *out_id);
// Lossless app PKI path: Data is already serialized and is encrypted without
// discarding reply/thread/reaction fields.
bool lora_manager_send_dm_data(const uint8_t *data, uint16_t dlen,
                               uint32_t to, uint32_t packet_id,
                               bool want_ack, uint32_t *out_id);
// Generic Data TX on a channel (phone/admin path, any portnum).
bool lora_manager_send_data_ch(uint32_t to, uint8_t portnum,
                               const uint8_t *payload, uint16_t plen,
                               uint32_t request_id, uint32_t packet_id,
                               bool want_ack, bool want_response,
                               uint8_t channel_idx, uint32_t *out_id);
bool lora_manager_send_data_verbatim_ch(uint32_t to,
                                        const uint8_t *data, uint16_t dlen,
                                        uint32_t packet_id, bool want_ack,
                                        uint8_t channel_idx,
                                        uint32_t *out_id);
// Raw air frame TX with duty/CAD (non-admin phone path already built a frame).
bool lora_manager_air_send(const uint8_t *frame, uint8_t len);
// Clear per-phone peer announcements when a new BLE PhoneAPI session starts.
void lora_manager_phone_session_reset(void);
// First-run gate: true when no explicit region choice is saved yet.
bool lora_manager_needs_setup(void);
bool lora_manager_region_saved(void);
// Guided questionnaire text for `lora start` refusal and `lora setup`.
bool lora_manager_setup_text(char *out, size_t n);
uint16_t lora_manager_msg_count(void);
// Read one retained message in oldest-to-newest order without copying the ring.
bool lora_manager_msg_at(uint16_t index, lora_msg_t *out);
// Snapshot the newest retained message and its monotonic change token.
bool lora_manager_latest_message(lora_msg_t *out, uint32_t *out_seq);
// Cursor drain: *io_seq==0 snaps to oldest retained; returns copied count.
uint16_t lora_manager_msg_since(uint32_t *io_seq, lora_msg_t *out, uint16_t max);
// Snapshot the newest received chat line. out_seq is a monotonic change token;
// callers can poll without draining the shared message ring.
bool lora_manager_latest_incoming(lora_msg_t *out, uint32_t *out_seq);
uint16_t lora_manager_node_count(void);
void lora_manager_get_status(lora_status_t *out);

// Board plumbing for Bring-up: fills hw from Kconfig, validates pins.
bool lora_manager_get_hw(lora_hw_t *out);

#ifdef __cplusplus
}
#endif

#endif // LORA_MANAGER_H
