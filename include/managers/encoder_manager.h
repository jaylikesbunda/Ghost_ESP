#pragma once
#include <stdint.h>
#include <stdbool.h>
#define ENCODER_RPM_SMOOTHING_SIZE 4
#define ENCODER_MAX_PENDING_STEPS 16  ///< cap queued detents to avoid event flooding

#ifdef __cplusplus
extern "C" {
#endif

/** Latch positions (see original Arduino lib) */
typedef enum {
    ENCODER_LATCH_FOUR3,   ///< 4 detents, latch on state 3      (default)
    ENCODER_LATCH_FOUR0,   ///< 4 detents, latch on state 0
    ENCODER_LATCH_TWO03    ///< 2 detents, latch on states 0 & 3
} encoder_latch_mode_t;

/** Direction reported by get_direction() */
typedef enum {
    ENCODER_DIR_NONE = 0,
    ENCODER_DIR_CW   = 1,
    ENCODER_DIR_CCW  = -1
} encoder_direction_t;

/** User-visible handle (mirrors joystick_t style) */
typedef struct {
    int pin_a;
    int pin_b;
    bool pullup;
    bool use_io_expander;        ///< true if pins are on IO expander, not ESP32 GPIOs

    /* internal state */
    int8_t  old_state;
    int32_t position;            ///< raw internal counter
    int32_t position_base_ext;   ///< latched position without acceleration
    int32_t position_ext;        ///< latched "user" position
    int32_t position_ext_prev;   ///< last position returned
    int32_t pending_steps;
    uint32_t pos_time_ms;        ///< time of last latch (ms)
    uint32_t pos_time_prev_ms;   ///< previous latch time (ms)
    uint64_t pos_time_us;
    uint64_t pos_time_prev_us;

    uint32_t rpm_time_diffs_us[ENCODER_RPM_SMOOTHING_SIZE];
    uint8_t rpm_time_index;
    uint8_t rpm_time_count;
    encoder_latch_mode_t mode;
} encoder_t;

/* -------- public API ---------- */
void  encoder_init(encoder_t *enc,
                   int pin_a,
                   int pin_b,
                   bool pullup,
                   encoder_latch_mode_t mode);
void  encoder_set_latch_mode(encoder_t *enc, encoder_latch_mode_t mode);

void  encoder_tick(encoder_t *enc);                    ///< Call as fast as you like (e.g. from a 1 kHz timer ISR/task)
int32_t encoder_get_position(const encoder_t *enc);    ///< latched count
encoder_direction_t encoder_get_direction(encoder_t *enc); ///< consumes delta
encoder_direction_t encoder_peek_direction(const encoder_t *enc);
void encoder_consume_direction(encoder_t *enc, encoder_direction_t dir);
uint32_t encoder_get_millis_between_rotations(const encoder_t *enc);
uint32_t encoder_get_rpm(const encoder_t *enc);

#ifdef __cplusplus
}
#endif
