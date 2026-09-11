#ifndef SUBGHZ_DECODERS_H
#define SUBGHZ_DECODERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SUBGHZ_DECODED_PROTO_MAX 32
#define SUBGHZ_DECODED_INFO_MAX 128
#define SUBGHZ_PRESET_NAME_MAX 48
#define SUBGHZ_CUSTOM_PRESET_BLOB_MAX 64
#define SUBGHZ_PRESERVED_EXTRA_MAX 512

typedef struct {
    char protocol[SUBGHZ_DECODED_PROTO_MAX];
    char info[SUBGHZ_DECODED_INFO_MAX];
    uint64_t code;
    int bits;
    int frequency_hz;
    int te;
    bool decoded;
    char preset_name[SUBGHZ_PRESET_NAME_MAX];
    char custom_preset_blob[SUBGHZ_CUSTOM_PRESET_BLOB_MAX];
    char *preserved_extra;
    bool raw_truncated;
    uint32_t raw_truncated_count;
} subghz_decoded_signal_t;

int32_t subghz_protocol_te(const char *protocol);
int subghz_normalize_decoded_bits(const char *protocol, int bits);

bool subghz_decode_princeton(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_came(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_nice_flo(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_pt2260(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_pt2262(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_came_atomo(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_nice_flor_s(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_chamberlain(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_linear(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_linear_delta3(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_unilarm(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_gangqi(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_holtek(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_holtek_ht12x(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_doitrand(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_gate_tx(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_keeloq(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_keeloq_rcswitch23(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_faac_slh(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_alutech_at_4n(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_marantec(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_ansonic(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_bett(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_clemsa(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_dickert_mahs(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_dooya(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_elplast(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_marantec24(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_hollarm(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_hay21(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_feron(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_roger(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_treadmill37(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_keyfinder(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);
bool subghz_decode_nord_ice(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits);

bool subghz_decode_signal(const int32_t *dur, size_t count, subghz_decoded_signal_t *result);
bool subghz_build_raw_from_decoded(const char *protocol,
                                   uint64_t code,
                                   int bits,
                                   int32_t *out,
                                   size_t max_count,
                                   size_t *out_count);

#define SUBGHZ_STREAM_DECODER_MAX_PROTOCOLS 32

typedef struct subghz_stream_decoder subghz_stream_decoder_t;

struct subghz_stream_decoder {
    const char *name;
    int state;
    uint64_t code;
    int bits;
    bool decoded;
    uint32_t prev_duration;
    void (*format_fn)(uint64_t code, int bits, char *out, size_t out_len);
    bool (*feed_fn)(subghz_stream_decoder_t *dec, bool level, uint32_t duration);
    int32_t te_short;
    int32_t te_long;
    int32_t te_delta;
    int32_t te_delta_long;
    int32_t sync_min;
    int32_t sync_max;
    int32_t start_bit;
    int max_bits;
    const int *valid_bits;
    int valid_bits_count;
    bool high_first;
    bool invert;
    int man_state;
    int pre_count;
};

typedef struct {
    subghz_stream_decoder_t decoders[SUBGHZ_STREAM_DECODER_MAX_PROTOCOLS];
    size_t count;
    volatile bool found;
    size_t found_idx;
} subghz_decoder_engine_t;

void subghz_engine_init(subghz_decoder_engine_t *engine);
void subghz_engine_feed(subghz_decoder_engine_t *engine, bool level, uint32_t duration);
void subghz_engine_reset(subghz_decoder_engine_t *engine);
const subghz_stream_decoder_t *subghz_engine_get_result(const subghz_decoder_engine_t *engine);
void subghz_stream_decoder_format_result(const subghz_stream_decoder_t *dec, char *out, size_t out_len);

#endif
