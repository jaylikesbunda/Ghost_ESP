/*
 * subghz_decoders.c
 *
 * SubGHz protocol decoders for raw transition timing data.
 * Data convention: positive values = HIGH periods, negative = LOW periods.
 *
 * Based on protocol decoder logic from the Flipper Zero firmware
 * (Flipper Unleashed / xMasterX fork).
 *
 * Original source: https://github.com/xMasterX/flipper-zero-unleashed-firmware
 * License: GNU General Public License v3.0 (GPL-3.0)
 *
 * Copyright (c) Flipper Devices Inc. / Unleashed contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Timing constants sourced from SubGhzBlockConst structures in the
 * unleashed firmware protocol implementations.
 */

#include "managers/subghz_decoders.h"
#include "esp_attr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define DURATION_DIFF(x, y) (((x) < (y)) ? ((y) - (x)) : ((x) - (y)))
#define DUR_ABS(x) ((x) >= 0 ? (x) : -(x))

typedef enum {
    ManEventShortLow = 0,
    ManEventShortHigh = 2,
    ManEventLongLow = 4,
    ManEventLongHigh = 6,
    ManEventReset = 8
} ManEvent;

typedef enum {
    ManStateStart1 = 0,
    ManStateMid1 = 1,
    ManStateMid0 = 2,
    ManStateStart0 = 3
} ManState;

static const uint8_t s_man_transitions[] = {0x01, 0x91, 0x9B, 0xFB};

static bool manchester_advance(ManState state, ManEvent event,
                                ManState *next_state, bool *data) {
    if (event == ManEventReset) {
        *next_state = ManStateMid1;
        return false;
    }
    ManState ns = (ManState)((s_man_transitions[state] >> event) & 0x03);
    if (ns == state) {
        *next_state = ManStateMid1;
        return false;
    }
    if (ns == ManStateMid0) {
        if (data) *data = false;
        *next_state = ns;
        return true;
    }
    if (ns == ManStateMid1) {
        if (data) *data = true;
        *next_state = ns;
        return true;
    }
    *next_state = ns;
    return false;
}

static ManEvent man_event_for(int32_t dur, int32_t te_short, int32_t te_long, int32_t te_delta) {
    int32_t v = DUR_ABS(dur);
    if (dur > 0) {
        if (DURATION_DIFF(v, te_short) < te_delta) return ManEventShortHigh;
        if (DURATION_DIFF(v, te_long) < te_delta) return ManEventLongHigh;
    } else if (dur < 0) {
        if (DURATION_DIFF(v, te_short) < te_delta) return ManEventShortLow;
        if (DURATION_DIFF(v, te_long) < te_delta) return ManEventLongLow;
    }
    return ManEventReset;
}

/*
 * Princeton: te_short=390, te_long=1170, te_delta=300, min_bits=24
 * Encoding: H_short+L_long=0, H_long+L_short=1
 * Preamble: long LOW ~ te_short*36, guard ~ te_long*2+
 * L_long tolerance uses te_delta*3 in reference
 */
bool subghz_decode_princeton(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 50) return false;
    const int32_t te_s = 390, te_l = 1170, te_d = 300;

    size_t start = 0;
    for (size_t i = 0; i < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 36) < te_d * 36) {
            start = i + 1;
            break;
        }
    }
    if (start == 0 || start + 48 > count) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 48) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d * 3) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d * 3 && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits >= 8) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * CAME: te_short=320, te_long=640, te_delta=150, min_bits=12
 * Encoding: L_short+H_long=0, L_long+H_short=1 (LOW first)
 * Header: long LOW ~ te_short*56, start bit HIGH ~ te_short
 * Valid: 12, 18, 24, 25, 42 bits
 */
bool subghz_decode_came(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 320, te_l = 640, te_d = 150;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 56) < te_d * 63) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s) < te_d) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 48) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_l) < te_d && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 12 || bits == 18 || bits == 24 || bits == 25 || bits == 42) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Nice FLO: te_short=700, te_long=1400, te_delta=200, min_bits=12
 * Encoding: L_short+H_long=0, L_long+H_short=1 (LOW first)
 * Header: long LOW ~ te_short*36, start bit HIGH ~ te_short
 * Valid: 12, 24 bits
 */
bool subghz_decode_nice_flo(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 700, te_l = 1400, te_d = 200;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 36) < te_d * 36) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s) < te_d) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 28) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_l) < te_d && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 12 || bits == 24) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * PT2260/PT2262: dynamic te, triple-level encoding
 * Each data bit is 2 half-bits: short+long=0/F, long+short=1
 * Preamble: gap ~ te*8..te*40
 */
bool subghz_decode_pt2260(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;

    int32_t te = 0;
    int te_samples = 0;
    for (size_t i = 0; i + 1 < count && te_samples < 20; i++) {
        int32_t v = DUR_ABS(dur[i]);
        if (v > 100 && v < 1500) {
            te += v;
            te_samples++;
        }
    }
    if (te_samples < 6) return false;
    te /= te_samples;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        int32_t v = DUR_ABS(dur[i]);
        if (v > te * 8 && v < te * 40) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    int32_t tol = te / 2;

    while (i + 3 < count && bits < 24) {
        int32_t s1 = DUR_ABS(dur[i]);
        int32_t s3 = DUR_ABS(dur[i + 2]);
        if (s1 < tol || s3 < tol) break;
        bool first_long = (s1 > te + tol);
        bool second_long = (s3 > te + tol);
        if (first_long && second_long) {
            code = (code << 2) | 0;
            bits += 2;
        } else if (!first_long && second_long) {
            code = (code << 2) | 1;
            bits += 2;
        } else if (first_long && !second_long) {
            code = (code << 2) | 2;
            bits += 2;
        } else {
            break;
        }
        i += 4;
    }
    if (bits >= 8 && bits % 2 == 0) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

bool subghz_decode_pt2262(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    return subghz_decode_pt2260(dur, count, out_code, out_bits);
}

/*
 * CAME Atomo: te_short=600, te_long=1200, te_delta=250, min_bits=62
 * Manchester encoding (inverted output: !data)
 * Header: long LOW ~ te_long*10 or te_long*60
 */
bool subghz_decode_came_atomo(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 90) return false;
    const int32_t te_s = 600, te_l = 1200, te_d = 250;

    size_t start = 0;
    for (size_t i = 0; i < count; i++) {
        if (dur[i] < 0) {
            int32_t v = -dur[i];
            if ((DURATION_DIFF(v, te_l * 10) < te_d * 20) ||
                (DURATION_DIFF(v, te_l * 60) < te_d * 40)) {
                start = i + 1;
                break;
            }
        }
    }
    if (start == 0 || count - start < 80) return false;

    uint64_t code = 0;
    int bits = 0;
    ManState mstate = ManStateMid1;
    manchester_advance(mstate, ManEventReset, &mstate, NULL);
    manchester_advance(mstate, ManEventShortLow, &mstate, NULL);

    for (size_t i = start; i < count && bits < 66; i++) {
        ManEvent ev = man_event_for(dur[i], te_s, te_l, te_d);
        if (ev == ManEventReset) break;
        if (dur[i] < 0 && DUR_ABS(dur[i]) >= (uint32_t)(te_l * 2 + te_d)) break;
        if (dur[i] > 0 && DUR_ABS(dur[i]) >= (uint32_t)(te_l * 2 + te_d)) break;
        bool data;
        bool ok = manchester_advance(mstate, ev, &mstate, &data);
        if (ok) {
            code = (code << 1) | (!data ? 1 : 0);
            bits++;
        }
    }
    if (bits == 62) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Nice Flor S: te_short=500, te_long=1000, te_delta=300, min_bits=52
 * Encoding: H_short+L_long=0, H_long+L_short=1
 * Header: long LOW ~ te_short*38, then H ~ te_short*3, then L ~ te_short*3
 * Stop bit: H ~ te_short*3
 */
bool subghz_decode_nice_flor_s(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 80) return false;
    const int32_t te_s = 500, te_l = 1000, te_d = 300;

    size_t start = 0;
    for (size_t i = 0; i + 2 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 38) < te_d * 38) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s * 3) < te_d * 3) {
                if (dur[i + 2] < 0 && DURATION_DIFF(DUR_ABS(dur[i + 2]), te_s * 3) < te_d * 3) {
                    start = i + 3;
                    break;
                }
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 80) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && DUR_ABS(h) >= te_s * 3 - te_d && DUR_ABS(h) <= te_s * 3 + te_d) {
            break;
        }
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 52 || bits == 76) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Chamberlain: te_short=1000, te_long=3000, te_delta=200, min_bits=10
 * 4-bit symbols: STOP=L(3t)+H(t)=0b0001, ONE=L(2t)+H(2t)=0b0011, ZERO=L(t)+H(3t)=0b0111
 * Header: long LOW ~ te_short*39, start bit HIGH ~ te_short
 */
bool subghz_decode_chamberlain(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 18) return false;
    const int32_t te_s = 1000, te_d = 200;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 39) < te_d * 20) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s) < te_d) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t symbols = 0;
    int sym_count = 0;
    size_t i = start;
    while (i + 1 < count && sym_count < 12) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (DUR_ABS(lv) > te_s * 5) break;
            if (DURATION_DIFF(la, te_s * 3) < te_d && DURATION_DIFF(ha, te_s) < te_d) {
                symbols = (symbols << 4) | 0x1;
                sym_count++; i += 2;
            } else if (DURATION_DIFF(la, te_s * 2) < te_d && DURATION_DIFF(ha, te_s * 2) < te_d) {
                symbols = (symbols << 4) | 0x3;
                sym_count++; i += 2;
            } else if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_s * 3) < te_d) {
                symbols = (symbols << 4) | 0x7;
                sym_count++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    if (sym_count >= 10 && sym_count <= 11) {
        uint64_t data = 0;
        int dbits = 0;
        for (int j = 0; j < sym_count; j++) {
            int nibble = (symbols >> ((sym_count - 1 - j) * 4)) & 0xF;
            if (nibble == 0x3) {
                data = (data << 1) | 1;
                dbits++;
            } else if (nibble == 0x7) {
                data = (data << 1) | 0;
                dbits++;
            } else if (nibble == 0x1) {
            }
        }
        if (dbits >= 7 && dbits <= 9) {
            *out_code = data;
            *out_bits = dbits;
            return true;
        }
    }
    return false;
}

/*
 * Linear: te_short=500, te_long=1500, te_delta=350, min_bits=10
 * Encoding: H_short+L_long=0, H_long+L_short=1
 * Header: long LOW ~ te_short*42
 */
bool subghz_decode_linear(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 20) return false;
    const int32_t te_s = 500, te_l = 1500, te_d = 350;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 42) < te_d * 15) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 12) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (lv >= te_s * 5) break;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 10) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Linear Delta3: te_short=500, te_long=2000, te_delta=150, min_bits=8
 * H_short + L(7*te) = bit 1
 * H_long  + L_long  = bit 0
 * Header: long LOW ~ te_short*70
 */
bool subghz_decode_linear_delta3(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 16) return false;
    const int32_t te_s = 500, te_l = 2000, te_d = 150;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 70) < te_d * 24) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 12) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (lv >= te_s * 10) {
                if (DURATION_DIFF(hv, te_s) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                } else if (DURATION_DIFF(hv, te_l) < te_d) {
                    code = (code << 1) | 0;
                    bits++;
                }
                break;
            }
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_s * 7) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 8) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * UNILARM: dynamic te, 25-bit, same structure as PT2260 family
 * Kept as-is since no unleashed reference exists
 */
bool subghz_decode_unilarm(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;

    int32_t te = 0;
    int te_samples = 0;
    for (size_t i = 0; i + 1 < count && te_samples < 20; i++) {
        int32_t v = DUR_ABS(dur[i]);
        if (v > 50 && v < 2000) {
            te += v;
            te_samples++;
        }
    }
    if (te_samples < 6) return false;
    te /= te_samples;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        int32_t v = DUR_ABS(dur[i]);
        if (v > te * 10 && v < te * 50) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;

    while (i + 1 < count && bits < 25) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (hv > te * 2 && lv < te * 2) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (hv < te * 2 && lv > te * 2) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 25) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * GangQi: te_short=500, te_long=1200, te_delta=200, min_bits=34
 * Encoding: H_short+L_long=0, H_long+L_short=1
 * GAP: long LOW ~ te_long*2
 */
bool subghz_decode_gangqi(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 500, te_l = 1200, te_d = 200;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_l * 2) < te_d * 3) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 36) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else if (lv >= te_l * 2 - te_d * 3) {
                if (DURATION_DIFF(hv, te_s) < te_d) {
                    code = (code << 1) | 0;
                    bits++;
                } else if (DURATION_DIFF(hv, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                }
                break;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 34) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Holtek: te_short=430, te_long=870, te_delta=100, min_bits=40
 * Encoding: L_short+H_long=0, L_long+H_short=1 (LOW first)
 * Preamble: ~te_short*36 then te_short start bit
 */
bool subghz_decode_holtek(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 50) return false;
    const int32_t te_s = 430, te_l = 870, te_d = 100;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        int32_t v = DUR_ABS(dur[i]);
        if (dur[i] < 0 && v >= te_s * 36 - te_d * 36 && v <= te_s * 36 + te_d * 36) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s) < te_d) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 40) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_l) < te_d * 2 && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits >= 40) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Holtek HT12x: te_short=320, te_long=640, te_delta=200, min_bits=12
 * Encoding: L_long+H_short=1, L_short+H_long=0 (LOW first)
 * Header: long LOW ~ te_short*28, start bit HIGH ~ te_short
 */
bool subghz_decode_holtek_ht12x(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 320, te_l = 640, te_d = 200;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 28) < te_d * 20) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s) < te_d) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 16) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (la >= te_s * 10 + te_d) break;
            if (DURATION_DIFF(la, te_l) < te_d * 2 && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 12) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Doitrand: te_short=400, te_long=1100, te_delta=150, min_bits=37
 * Encoding: L_short+H_long=0, L_long+H_short=1 (LOW first)
 * Long tolerance: te_delta*3
 * Header: long LOW ~ te_short*62, start bit HIGH ~ te_short*2
 */
bool subghz_decode_doitrand(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 400, te_l = 1100, te_d = 150;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 62) < te_d * 30) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_s * 2) < te_d * 3) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 40) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (la >= te_s * 10 + te_d) break;
            if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d * 3) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_l) < te_d * 3 && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 37) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Gate TX: te_short=350, te_long=700, te_delta=100, min_bits=24
 * Encoding: L_short+H_long=0, L_long+H_short=1 (LOW first)
 * Long tolerance: te_delta*3
 * Header: long LOW ~ te_short*47, start bit HIGH ~ te_long
 */
bool subghz_decode_gate_tx(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 20) return false;
    const int32_t te_s = 350, te_l = 700, te_d = 100;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_s * 47) < te_d * 47) {
            if (dur[i + 1] > 0 && DURATION_DIFF(dur[i + 1], te_l) < te_d * 3) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 28) {
        int32_t lv = dur[i], hv = dur[i + 1];
        if (lv < 0 && hv > 0) {
            int32_t la = -lv, ha = hv;
            if (la >= te_s * 10 + te_d) break;
            if (DURATION_DIFF(la, te_s) < te_d && DURATION_DIFF(ha, te_l) < te_d * 3) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(la, te_l) < te_d * 3 && DURATION_DIFF(ha, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 24) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

bool subghz_decode_keeloq_rcswitch23(const int32_t *dur,
                                     size_t count,
                                     uint64_t *out_code,
                                     int *out_bits) {
    if (!dur || !out_code || !out_bits || count <= 26) {
        return false;
    }

    const int32_t delay = 400;
    const int32_t tolerance = delay * 60 / 100;
    uint64_t code = 0;
    int bits = 0;

    for (size_t i = 25; i + 1 < count && bits < 64; i += 2) {
        int32_t first = DUR_ABS(dur[i]);
        int32_t second = DUR_ABS(dur[i + 1]);
        code <<= 1;
        if (DURATION_DIFF(first, delay * 2) < tolerance &&
            DURATION_DIFF(second, delay) < tolerance) {
            /* zero */
        } else if (DURATION_DIFF(first, delay) < tolerance &&
                   DURATION_DIFF(second, delay * 2) < tolerance) {
            code |= 1;
        } else {
            return false;
        }
        bits++;
    }

    if (count > 7 && bits > 0) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * KeeLoq: te_short=400, te_long=800, bits=64.
 * The classic RCSwitch protocol 23 decodes timing magnitudes and does not depend on
 * which GDO edge polarity starts the capture, so keep this decoder polarity-tolerant.
 *
 * RMT captures on the T-Embed CC1101 pick up short spurious edges (3-180us)
 * that split real 400/800us KeeLoq pulses; strict pair decoders then fail on
 * the shifted pairs. Two-stage robust decode:
 *   1. Deglitch: fold every segment shorter than 200us (half a KeeLoq TE, so
 *      it can never be a legit KeeLoq pulse) into its same-level neighbours.
 *   2. Sync scan: every long LOW (>=3ms: the 4ms header or an 11-25ms
 *      inter-word gap) is a candidate; decode 64 PWM pairs after it with
 *      protocol-23 polarity: (TE,2TE)=1, (2TE,TE)=0, tolerance 60%.
 */
#define SUBGHZ_KEELOQ_DEGLITCH_CAP 384U

static size_t subghz_deglitch_durations(const int32_t *in, size_t count,
                                        int32_t *out, size_t cap, int32_t min_us) {
    size_t n = (count < cap) ? count : cap;
    memcpy(out, in, n * sizeof(int32_t));

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            int32_t a = DUR_ABS(out[i]);
            if (a >= min_us) continue;
            if (i > 0 && i + 1 < n &&
                (out[i - 1] < 0) == (out[i + 1] < 0)) {
                /* spurious edge: neighbours share the level, fold all three */
                int32_t m = DUR_ABS(out[i - 1]) + a + DUR_ABS(out[i + 1]);
                out[i - 1] = (out[i - 1] < 0) ? -m : m;
                memmove(&out[i], &out[i + 2], (n - i - 2) * sizeof(int32_t));
                n -= 2;
            } else {
                /* boundary partial pulse: drop it */
                memmove(&out[i], &out[i + 1], (n - i - 1) * sizeof(int32_t));
                n -= 1;
            }
            changed = true;
            break;
        }
    }
    return n;
}

bool subghz_decode_keeloq(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (!dur || !out_code || !out_bits || count < 40) {
        return false;
    }

    int32_t clean[SUBGHZ_KEELOQ_DEGLITCH_CAP];
    size_t n = subghz_deglitch_durations(dur, count, clean, SUBGHZ_KEELOQ_DEGLITCH_CAP, 200);

    const int32_t te = 400;
    const int32_t tol = te * 60 / 100;

    for (size_t s = 0; s < n; s++) {
        if (clean[s] > -3000) continue; /* sync candidates are long LOWs */
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < n && bits < 64) {
            int32_t first = DUR_ABS(clean[i]);
            int32_t second = DUR_ABS(clean[i + 1]);
            code <<= 1;
            if (DURATION_DIFF(first, te) < tol && DURATION_DIFF(second, te * 2) < tol) {
                code |= 1;
            } else if (DURATION_DIFF(first, te * 2) < tol && DURATION_DIFF(second, te) < tol) {
                /* zero */
            } else {
                break;
            }
            bits++;
            i += 2;
        }
        if (bits == 64) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

/*
 * FAAC SLH: te_short=255, te_long=595, te_delta=100, min_bits=64
 * Encoding: H_short+L_long=0, H_long+L_short=1
 * Preamble: H ~ te_long*2 then L ~ te_long*2
 */
bool subghz_decode_faac_slh(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 80) return false;
    const int32_t te_s = 255, te_l = 595, te_d = 100;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        if (dur[i] > 0 && DURATION_DIFF(dur[i], te_l * 2) < te_d * 3) {
            if (dur[i + 1] < 0 && DURATION_DIFF(DUR_ABS(dur[i + 1]), te_l * 2) < te_d * 3) {
                start = i + 2;
                break;
            }
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 66) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (hv >= te_s * 3 + te_d) break;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 64) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Alutech AT-4N: te_short=400, te_long=800, te_delta=140, min_bits=72
 * Encoding: H_short+L_long=1, H_long+L_short=0 (INVERTED)
 * Long tolerance: te_delta*2
 * Preamble: >9 alternating H_short+L_short pairs, then L ~ te_short*10
 */
bool subghz_decode_alutech_at_4n(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 80) return false;
    const int32_t te_s = 400, te_l = 800, te_d = 140;

    size_t start = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        int header_pairs = 0;
        size_t j = i;
        while (j + 1 < count &&
               dur[j] > 0 && DURATION_DIFF(dur[j], te_s) < te_d &&
               dur[j + 1] < 0 && DURATION_DIFF(DUR_ABS(dur[j + 1]), te_s) < te_d) {
            header_pairs++;
            j += 2;
        }

        if (header_pairs > 9 && j + 1 < count &&
            dur[j] > 0 && DURATION_DIFF(dur[j], te_s) < te_d &&
            dur[j + 1] < 0 &&
            DURATION_DIFF(DUR_ABS(dur[j + 1]), te_s * 10) < te_d * 10) {
            start = j + 2;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 0;
    int bits = 0;
    size_t i = start;
    while (i + 1 < count && bits < 74) {
        int32_t h = dur[i], l = dur[i + 1];
        if (h > 0 && l < 0) {
            int32_t hv = h, lv = -l;
            if (lv >= te_s * 2 + te_d) {
                if (DURATION_DIFF(hv, te_s) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                } else if (DURATION_DIFF(hv, te_l) < te_d * 2) {
                    code = (code << 1) | 0;
                    bits++;
                }
                break;
            }
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d * 2) {
                code = (code << 1) | 1;
                bits++; i += 2;
            } else if (DURATION_DIFF(hv, te_l) < te_d * 2 && DURATION_DIFF(lv, te_s) < te_d) {
                code = (code << 1) | 0;
                bits++; i += 2;
            } else {
                break;
            }
        } else {
            break;
        }
    }
    if (bits == 72) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

/*
 * Marantec: te_short=1000, te_long=2000, te_delta=200, min_bits=49
 * Manchester encoding (direct: data output as-is)
 * Header: long LOW ~ te_long*5, first bit always 1
 */
bool subghz_decode_marantec(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 60) return false;
    const int32_t te_s = 1000, te_l = 2000, te_d = 200;

    size_t start = 0;
    for (size_t i = 0; i < count; i++) {
        if (dur[i] < 0 && DURATION_DIFF(DUR_ABS(dur[i]), te_l * 5) < te_d * 8) {
            start = i + 1;
            break;
        }
    }
    if (start == 0) return false;

    uint64_t code = 1;
    int bits = 1;
    ManState mstate = ManStateMid1;
    manchester_advance(mstate, ManEventReset, &mstate, NULL);

    for (size_t i = start; i < count && bits < 52; i++) {
        ManEvent ev = man_event_for(dur[i], te_s, te_l, te_d);
        if (ev == ManEventReset) break;
        if (dur[i] < 0 && DUR_ABS(dur[i]) >= (uint32_t)(te_l * 2 + te_d)) break;
        if (dur[i] > 0 && DUR_ABS(dur[i]) >= (uint32_t)(te_l * 2 + te_d)) break;
        bool data;
        bool ok = manchester_advance(mstate, ev, &mstate, &data);
        if (ok) {
            code = (code << 1) | (data ? 1 : 0);
            bits++;
        }
    }
    if (bits == 49) {
        *out_code = code;
        *out_bits = bits;
        return true;
    }
    return false;
}

typedef struct {
    const char *name;
    bool (*decode)(const int32_t *, size_t, uint64_t *, int *);
    void (*format)(uint64_t code, int bits, char *out, size_t out_len);
} subghz_decoder_entry_t;

static void format_princeton(uint64_t code, int bits, char *out, size_t out_len) {
    uint32_t btn = (uint32_t)((code >> (bits - 4)) & 0xF);
    if (bits == 24) {
        uint32_t addr = (uint32_t)((code >> 4) & 0xFF);
        uint32_t data = (uint32_t)(code & 0xF);
        snprintf(out, out_len, "Princeton %dbit\nBtn:0x%X Addr:0x%02X Dat:0x%X",
                 bits, (unsigned)btn, (unsigned)addr, (unsigned)data);
    } else {
        snprintf(out, out_len, "Princeton %dbit\nCode:0x%08llX", bits, (unsigned long long)(uint32_t)code);
    }
}

static void format_came(uint64_t code, int bits, char *out, size_t out_len) {
    if (bits == 12) {
        uint32_t serial = (uint32_t)((code >> 4) & 0xFF);
        uint32_t key = (uint32_t)(code & 0xF);
        snprintf(out, out_len, "CAME %dbit\nSerial:0x%02X Key:0x%X",
                 bits, (unsigned)serial, (unsigned)key);
    } else {
        snprintf(out, out_len, "CAME %dbit\nCode:0x%06llX", bits, (unsigned long long)(uint32_t)code);
    }
}

static void format_nice_flo(uint64_t code, int bits, char *out, size_t out_len) {
    uint32_t serial = (uint32_t)((code >> 4) & 0xFF);
    uint32_t key = (uint32_t)(code & 0xF);
    snprintf(out, out_len, "Nice FLO %dbit\nSerial:0x%02X Key:0x%X",
             bits, (unsigned)serial, (unsigned)key);
}

static void format_pt2260(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "PT2260 %dbit\nCode:0x%08llX", bits, (unsigned long long)(uint32_t)code);
}

static void format_generic(const char *proto, uint64_t code, int bits, char *out, size_t out_len) {
    if (bits > 32) {
        snprintf(out, out_len, "%s %dbit\nCode:0x%016llX", proto, bits, (unsigned long long)code);
    } else {
        uint32_t c = (uint32_t)code;
        int len = 0;
        len += snprintf(out + len, out_len - len, "%s %dbit\nCode:0x", proto, bits);
        for (int i = bits - 4; i >= 0; i -= 4) {
            if (len < (int)out_len - 1)
                len += snprintf(out + len, out_len - len, "%X", (unsigned)((c >> i) & 0xF));
        }
    }
}

static void format_chamberlain(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Chamberlain %dbit\nCode:0x%04llX", bits, (unsigned long long)(uint32_t)code);
}

static void format_linear(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Linear %dbit\nCode:0x%04llX", bits, (unsigned long long)(uint32_t)code);
}

static void format_keeloq(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "KeeLoq %dbit\nCode:0x%016llX", bits, (unsigned long long)code);
}

static void format_marantec(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Marantec %dbit\nCode:0x%016llX", bits, (unsigned long long)code);
}

static void format_ansonic(uint64_t code, int bits, char *out, size_t out_len) {
    int len = snprintf(out, out_len, "Ansonic %dbit\nDIP:", bits);
    for (int i = 0; i < bits && len < (int)out_len - 2; i++) {
        len += snprintf(out + len, out_len - len, "%d", (int)((code >> (bits - 1 - i)) & 1));
    }
}

static void format_bett(uint64_t code, int bits, char *out, size_t out_len) {
    int len = snprintf(out, out_len, "Bett %dbit\nDIP:", bits);
    for (int i = 0; i < bits && len < (int)out_len - 2; i++) {
        len += snprintf(out + len, out_len - len, "%d", (int)((code >> (bits - 1 - i)) & 1));
    }
}

static void format_clemsa(uint64_t code, int bits, char *out, size_t out_len) {
    int len = snprintf(out, out_len, "Clemsa %dbit\nDIP:", bits);
    for (int i = 0; i < bits && len < (int)out_len - 2; i++) {
        len += snprintf(out + len, out_len - len, "%d", (int)((code >> (bits - 1 - i)) & 1));
    }
}

static void format_dickert_mahs(uint64_t code, int bits, char *out, size_t out_len) {
    if (bits >= 36) {
        uint32_t factory = (uint32_t)((code >> 12) & 0xFFFFFF);
        uint32_t user = (uint32_t)(code & 0xFFF);
        snprintf(out, out_len, "Dickert MAHS %dbit\nFac:0x%06lX User:0x%03lX", bits, factory, user);
    } else {
        snprintf(out, out_len, "Dickert MAHS %dbit\nCode:0x%016llX", bits, (unsigned long long)code);
    }
}

static void format_dooya(uint64_t code, int bits, char *out, size_t out_len) {
    if (bits >= 40) {
        uint32_t serial = (uint32_t)((code >> 12) & 0xFFFFF);
        uint8_t channel = (uint8_t)((code >> 8) & 0xF);
        uint8_t button = (uint8_t)(code & 0xFF);
        snprintf(out, out_len, "Dooya %dbit\nS/N:0x%05lX Ch:%d Btn:0x%02X", bits, serial, channel, button);
    } else {
        snprintf(out, out_len, "Dooya %dbit\nCode:0x%016llX", bits, (unsigned long long)code);
    }
}

static void format_elplast(uint64_t code, int bits, char *out, size_t out_len) {
    int len = snprintf(out, out_len, "Elplast %dbit\nDIP:", bits);
    for (int i = 0; i < bits && len < (int)out_len - 2; i++) {
        len += snprintf(out + len, out_len - len, "%d", (int)((code >> (bits - 1 - i)) & 1));
    }
}

static void format_marantec24(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Marantec24 %dbit\nKey:0x%06llX\nSerial:0x%05llX Btn:%01llX",
             bits,
             (unsigned long long)(code & 0xFFFFFFULL),
             (unsigned long long)((code >> 4) & 0xFFFFFULL),
             (unsigned long long)(code & 0xFULL));
}

static void format_hollarm(uint64_t code, int bits, char *out, size_t out_len) {
    uint8_t sum = (uint8_t)(((code >> 32) & 0xFF) + ((code >> 24) & 0xFF) +
                            ((code >> 16) & 0xFF) + ((code >> 8) & 0xFF));
    snprintf(out, out_len, "Hollarm %dbit\nKey:0x%010llX\nSerial:0x%07llX Btn:%01llX Sum:%02X",
             bits,
             (unsigned long long)(code & 0x3FFFFFFFFFFULL),
             (unsigned long long)((code >> 16) & 0x0FFFFFFFULL),
             (unsigned long long)((code >> 8) & 0xFULL),
             sum);
}

static void format_hay21(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Hay21 %dbit\nKey:0x%06llX\nSerial:0x%02llX Btn:0x%02llX Cnt:%01llX",
             bits,
             (unsigned long long)(code & 0x1FFFFFULL),
             (unsigned long long)((code >> 5) & 0xFFULL),
             (unsigned long long)((code >> 13) & 0xFFULL),
             (unsigned long long)((code >> 1) & 0xFULL));
}

static void format_feron(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Feron %dbit\nKey:0x%08llX\nSerial:0x%04llX Cmd:0x%04llX",
             bits,
             (unsigned long long)(code & 0xFFFFFFFFULL),
             (unsigned long long)((code >> 16) & 0xFFFFULL),
             (unsigned long long)(code & 0xFFFFULL));
}

static void format_roger(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Roger %dbit\nKey:0x%07llX\nSerial:0x%04llX Btn:%01llX End:%02llX",
             bits,
             (unsigned long long)(code & 0xFFFFFFFULL),
             (unsigned long long)((code >> 12) & 0xFFFFULL),
             (unsigned long long)((code >> 8) & 0xFULL),
             (unsigned long long)(code & 0xFFULL));
}

static void format_treadmill37(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "Treadmill37 %dbit\nKey:0x%010llX\nSerial:0x%06llX Btn:%04llX",
             bits,
             (unsigned long long)(code & 0x1FFFFFFFFFULL),
             (unsigned long long)((code >> 17) & 0x1FFFFFULL),
             (unsigned long long)((code >> 1) & 0xFFFFULL));
}

static void format_keyfinder(uint64_t code, int bits, char *out, size_t out_len) {
    snprintf(out, out_len, "KeyFinder %dbit\nKey:0x%06llX\nSerial:0x%05llX ID:%01llX",
             bits,
             (unsigned long long)(code & 0xFFFFFFULL),
             (unsigned long long)((code >> 4) & 0xFFFFFULL),
             (unsigned long long)(code & 0xFULL));
}

static void format_nord_ice(uint64_t code, int bits, char *out, size_t out_len) {
    uint32_t serial = (uint32_t)((((code >> 15) << 9) | (code & 0x1FFULL)) & 0x3FFFFFFULL);
    uint32_t btn = (uint32_t)((code >> 9) & 0x3FULL);
    snprintf(out, out_len, "Nord ICE %dbit\nKey:0x%09llX\nSerial:0x%07lX Btn:%02lX",
             bits,
             (unsigned long long)(code & 0x1FFFFFFFFULL),
             serial,
             btn);
}

/* ============================================================
 * Streaming decoder engine (Flipper-style per-edge feed)
 * ============================================================ */

#define SD_STATE_IDLE       0
#define SD_STATE_SYNC       1
#define SD_STATE_FIRST      2
#define SD_STATE_SECOND     3

static bool sd_check_valid_bits(const subghz_stream_decoder_t *d) {
    for (int i = 0; i < d->valid_bits_count; i++) {
        if (d->bits == d->valid_bits[i]) return true;
    }
    return false;
}

static bool IRAM_ATTR pair_feed(subghz_stream_decoder_t *d, bool level, uint32_t duration) {
    if (d->decoded) return false;
    int32_t dur = (int32_t)duration;
    int32_t te_s = d->te_short, te_l = d->te_long;
    int32_t te_d = d->te_delta, te_dl = d->te_delta_long > 0 ? d->te_delta_long : d->te_delta;

    switch (d->state) {
    case SD_STATE_IDLE:
        if (!level && dur >= d->sync_min && dur <= d->sync_max) {
            d->state = SD_STATE_SYNC;
        }
        return false;

    case SD_STATE_SYNC: {
        bool start_match = (d->start_bit > 0)
            ? (DURATION_DIFF(dur, d->start_bit) < te_dl)
            : (DURATION_DIFF(dur, te_s) < te_d);
        bool start_level = d->high_first;
        if (level == start_level && start_match) {
            d->state = SD_STATE_FIRST;
            d->code = 0;
            d->bits = 0;
        } else {
            d->state = SD_STATE_IDLE;
        }
        return false;
    }

    case SD_STATE_FIRST: {
        bool expect_level = d->high_first;
        if (level == expect_level) {
            d->prev_duration = duration;
            d->state = SD_STATE_SECOND;
        } else if (level == !expect_level && dur >= d->sync_min && dur <= d->sync_max) {
            if (d->bits > 0 && sd_check_valid_bits(d)) {
                d->decoded = true;
                return true;
            }
            d->state = SD_STATE_SYNC;
        } else if (d->bits > 0 && sd_check_valid_bits(d)) {
            d->decoded = true;
            return true;
        } else {
            d->state = SD_STATE_IDLE;
        }
        return false;
    }

    case SD_STATE_SECOND: {
        bool expect_level = !d->high_first;
        if (level == expect_level) {
            int32_t first = (int32_t)d->prev_duration;
            int32_t second = dur;
            int32_t a, b;
            if (d->high_first) { a = first; b = second; }
            else { a = -first; b = second; if (a < 0) a = -a; }

            bool bit_val;
            if (DURATION_DIFF(a, te_s) < te_d && DURATION_DIFF(b, te_l) < te_dl) {
                bit_val = d->invert ? 1 : 0;
            } else if (DURATION_DIFF(a, te_l) < te_dl && DURATION_DIFF(b, te_s) < te_d) {
                bit_val = d->invert ? 0 : 1;
            } else {
                if (d->bits > 0 && sd_check_valid_bits(d)) {
                    d->decoded = true;
                    return true;
                }
                d->state = SD_STATE_IDLE;
                return false;
            }
            d->code = (d->code << 1) | bit_val;
            d->bits++;
            if (d->bits >= d->max_bits) {
                if (sd_check_valid_bits(d)) {
                    d->decoded = true;
                    return true;
                }
                d->state = SD_STATE_IDLE;
            } else {
                d->state = SD_STATE_FIRST;
            }
        } else if (level == expect_level) {
            // already handled above
        } else if (!level && dur >= d->sync_min && dur <= d->sync_max) {
            if (d->bits > 0 && sd_check_valid_bits(d)) {
                d->decoded = true;
                return true;
            }
            d->state = SD_STATE_SYNC;
        } else {
            if (d->bits > 0 && sd_check_valid_bits(d)) {
                d->decoded = true;
                return true;
            }
            d->state = SD_STATE_IDLE;
        }
        return false;
    }
    }
    return false;
}

static bool IRAM_ATTR keeloq_feed(subghz_stream_decoder_t *d, bool level, uint32_t duration) {
    if (d->decoded) return false;
    int32_t dur = (int32_t)duration;
    int32_t te_s = d->te_short, te_l = d->te_long, te_d = d->te_delta;

    switch (d->state) {
    case SD_STATE_IDLE:
        if (!level && dur >= te_s * 8) {
            d->pre_count = 0;
            d->state = 10;
        }
        return false;
    case 10:
        if (level && DURATION_DIFF(dur, te_s) < te_d) {
            d->pre_count++;
            d->state = 11;
        } else {
            d->pre_count = 0;
            d->state = SD_STATE_IDLE;
        }
        return false;
    case 11:
        if (!level) {
            if (dur >= te_s * 5 && d->pre_count >= 4) {
                d->state = SD_STATE_FIRST;
                d->code = 0;
                d->bits = 0;
            } else if (DURATION_DIFF(dur, te_s) < te_d) {
                d->state = 10;
            } else {
                d->pre_count = 0;
                d->state = SD_STATE_IDLE;
            }
        } else {
            d->pre_count = 0;
            d->state = SD_STATE_IDLE;
        }
        return false;
    case SD_STATE_FIRST:
        if (level) {
            d->prev_duration = duration;
            d->state = SD_STATE_SECOND;
        } else {
            if (d->bits == 64) { d->decoded = true; return true; }
            d->state = SD_STATE_IDLE;
        }
        return false;
    case SD_STATE_SECOND:
        if (!level) {
            int32_t hv = (int32_t)d->prev_duration, lv = dur;
            if (DURATION_DIFF(hv, te_s) < te_d && DURATION_DIFF(lv, te_l) < te_d) {
                d->code = (d->code << 1) | 1; d->bits++;
                if (d->bits == 64) { d->decoded = true; return true; }
                d->state = SD_STATE_FIRST;
            } else if (DURATION_DIFF(hv, te_l) < te_d && DURATION_DIFF(lv, te_s) < te_d) {
                d->code = (d->code << 1) | 0; d->bits++;
                if (d->bits == 64) { d->decoded = true; return true; }
                d->state = SD_STATE_FIRST;
            } else {
                if (d->bits == 64) { d->decoded = true; return true; }
                d->state = SD_STATE_IDLE;
            }
        } else {
            if (d->bits == 64) { d->decoded = true; return true; }
            d->state = SD_STATE_IDLE;
        }
        return false;
    }
    return false;
}

static bool IRAM_ATTR manchester_feed_wrapper(subghz_stream_decoder_t *d, bool level, uint32_t duration) {
    if (d->decoded) return false;
    int32_t dur_signed = level ? (int32_t)duration : -(int32_t)duration;
    int32_t te_s = d->te_short, te_l = d->te_long, te_d = d->te_delta;
    ManEvent ev = man_event_for(dur_signed, te_s, te_l, te_d);
    if (ev == ManEventReset) {
        if (d->state == SD_STATE_FIRST && d->bits > 0 && sd_check_valid_bits(d)) {
            d->decoded = true;
            return true;
        }
        d->state = SD_STATE_IDLE;
        d->man_state = ManStateMid1;
        manchester_advance(ManStateMid1, ManEventReset, (ManState *)&d->man_state, NULL);
        return false;
    }
    if (DUR_ABS(dur_signed) >= (uint32_t)(te_l * 2 + te_d)) {
        if (d->state == SD_STATE_FIRST && d->bits > 0 && sd_check_valid_bits(d)) {
            d->decoded = true;
            return true;
        }
        d->state = SD_STATE_IDLE;
        return false;
    }
    if (d->state == SD_STATE_IDLE) {
        if (!level && (DURATION_DIFF(duration, te_l * 10) < te_d * 20 ||
                       DURATION_DIFF(duration, te_l * 60) < te_d * 40 ||
                       DURATION_DIFF(duration, te_l * 5) < te_d * 8)) {
            d->state = SD_STATE_SYNC;
            d->man_state = ManStateMid1;
            manchester_advance(d->man_state, ManEventReset, (ManState *)&d->man_state, NULL);
            if (d->name && strcmp(d->name, "Marantec") == 0) {
                manchester_advance(d->man_state, ManEventShortLow, (ManState *)&d->man_state, NULL);
            } else {
                manchester_advance(d->man_state, ManEventShortLow, (ManState *)&d->man_state, NULL);
            }
        }
        return false;
    }
    if (d->state == SD_STATE_SYNC) {
        d->state = SD_STATE_FIRST;
        d->code = 0;
        d->bits = 0;
        if (d->name && strcmp(d->name, "Marantec") == 0) {
            d->code = 1;
            d->bits = 1;
        }
    }
    bool data;
    bool ok = manchester_advance((ManState)d->man_state, ev, (ManState *)&d->man_state, &data);
    if (ok) {
        bool bit_val;
        if (d->name && strcmp(d->name, "CAME Atomo") == 0) {
            bit_val = !data;
        } else {
            bit_val = data;
        }
        d->code = (d->code << 1) | (bit_val ? 1 : 0);
        d->bits++;
        if (d->bits >= d->max_bits) {
            if (sd_check_valid_bits(d)) {
                d->decoded = true;
                return true;
            }
            d->state = SD_STATE_IDLE;
        }
    }
    return false;
}

static void format_generic_holtek(uint64_t code, int bits, char *out, size_t out_len) {
    format_generic("Holtek", code, bits, out, out_len);
}
static void format_generic_ht12x(uint64_t code, int bits, char *out, size_t out_len) {
    format_generic("Holtek HT12x", code, bits, out, out_len);
}

static const int s_vb_came[] = {12, 18, 24, 25, 42};
static const int s_vb_nice_flo[] = {12, 24};
static const int s_vb_chamberlain[] = {7, 8, 9};
static const int s_vb_linear[] = {10};
static const int s_vb_linear_d3[] = {8};
static const int s_vb_gate_tx[] = {24};
static const int s_vb_holtek[] = {40};
static const int s_vb_holtek_ht12x[] = {12};
static const int s_vb_doitrand[] = {37};
static const int s_vb_gangqi[] = {34};
static const int s_vb_keeloq[] = {64};
static const int s_vb_faac_slh[] = {64};
static const int s_vb_alutech[] = {72};
static const int s_vb_nice_flor_s[] = {52, 76};
static const int s_vb_atomo[] = {62};
static const int s_vb_marantec[] = {49};
static const int s_vb_ansonic[] = {12, 24};
static const int s_vb_bett[] = {18};
static const int s_vb_clemsa[] = {18};
static const int s_vb_dickert_mahs[] = {36};
static const int s_vb_dooya[] = {40};
static const int s_vb_elplast[] = {18};
static const int s_vb_hollarm[] = {42};
static const int s_vb_hay21[] = {21};
static const int s_vb_feron[] = {32};
static const int s_vb_roger[] = {28};
static const int s_vb_treadmill37[] = {37};
static const int s_vb_nord_ice[] = {33};

void subghz_engine_init(subghz_decoder_engine_t *engine) {
    memset(engine, 0, sizeof(*engine));
    subghz_stream_decoder_t *d = engine->decoders;

    #define PAIR_DEC(idx, nm, tes, tel, ted, tedl, smin, smax, sbit, hf, inv, mx, vb, vb_cnt, fmt) \
        d[idx].name = nm; d[idx].feed_fn = pair_feed; d[idx].format_fn = fmt; \
        d[idx].te_short = tes; d[idx].te_long = tel; d[idx].te_delta = ted; \
        d[idx].te_delta_long = tedl; d[idx].sync_min = smin; d[idx].sync_max = smax; \
        d[idx].start_bit = sbit; d[idx].high_first = hf; d[idx].invert = inv; \
        d[idx].max_bits = mx; d[idx].valid_bits = vb; d[idx].valid_bits_count = vb_cnt;

    PAIR_DEC(0,  "CAME",         320,  640,  150, 150,  320*40, 320*72,  320, false, false, 48, s_vb_came, 5, format_came);
    PAIR_DEC(1,  "Nice FLO",     700, 1400,  200, 200,  700*20, 700*52,  700, false, false, 28, s_vb_nice_flo, 2, format_nice_flo);
    PAIR_DEC(2,  "Holtek",       430,  870,  100, 200,  430*20, 430*52,  430, false, false, 42, s_vb_holtek, 1, format_generic_holtek);
    PAIR_DEC(3,  "Holtek HT12x", 320,  640,  200, 400,  320*10, 320*46,  320, false, false, 16, s_vb_holtek_ht12x, 1, format_generic_ht12x);
    PAIR_DEC(4,  "Gate TX",      350,  700,  100, 300,  350*30, 350*64,  700, false, false, 28, s_vb_gate_tx, 1, NULL);
    PAIR_DEC(5,  "Doitrand",     400, 1100,  150, 450,  400*40, 400*84,  800, false, false, 40, s_vb_doitrand, 1, NULL);
    PAIR_DEC(6,  "Linear",       500, 1500,  350, 350,  500*30, 500*54,    0, true,  false, 12, s_vb_linear, 1, format_linear);
    PAIR_DEC(7,  "Linear D3",    500, 2000,  150, 150,  500*50, 500*90,    0, true,  false, 12, s_vb_linear_d3, 1, format_linear);
    PAIR_DEC(8,  "GangQi",       500, 1200,  200, 200,  1200*1, 1200*4,    0, true,  false, 36, s_vb_gangqi, 1, NULL);
    PAIR_DEC(9,  "Nice Flor S",  500, 1000,  300, 300,  500*20, 500*56,    0, true,  false, 80, s_vb_nice_flor_s, 2, NULL);
    PAIR_DEC(10, "FAAC SLH",     255,  595,  100, 100,    0,     0,         0, true,  false, 66, s_vb_faac_slh, 1, NULL);
    PAIR_DEC(11, "Alutech",      400,  800,  140, 280,    0,     0,         0, true,  true,  74, s_vb_alutech, 1, NULL);
    PAIR_DEC(12, "Chamberlain",  1000, 3000, 200, 200, 1000*25, 1000*53, 1000, false, false, 24, s_vb_chamberlain, 3, format_chamberlain);

    int idx = 13;

    d[idx].name = "KeeLoq";
    d[idx].feed_fn = keeloq_feed;
    d[idx].format_fn = format_keeloq;
    d[idx].te_short = 400; d[idx].te_long = 800; d[idx].te_delta = 180;
    d[idx].max_bits = 64;
    d[idx].valid_bits = s_vb_keeloq;
    d[idx].valid_bits_count = 1;
    idx++;

    d[idx].name = "CAME Atomo";
    d[idx].feed_fn = manchester_feed_wrapper;
    d[idx].format_fn = NULL;
    d[idx].te_short = 600; d[idx].te_long = 1200; d[idx].te_delta = 250;
    d[idx].max_bits = 66;
    d[idx].valid_bits = s_vb_atomo;
    d[idx].valid_bits_count = 1;
    d[idx].man_state = ManStateMid1;
    idx++;

    d[idx].name = "Marantec";
    d[idx].feed_fn = manchester_feed_wrapper;
    d[idx].format_fn = format_marantec;
    d[idx].te_short = 1000; d[idx].te_long = 2000; d[idx].te_delta = 200;
    d[idx].max_bits = 52;
    d[idx].valid_bits = s_vb_marantec;
    d[idx].valid_bits_count = 1;
    d[idx].man_state = ManStateMid1;
    idx++;

    PAIR_DEC(idx, "Ansonic",      500, 1000, 150, 300, 1000*10, 1000*16,    0, true, false, 24, s_vb_ansonic, 2, format_ansonic);
    idx++;
    PAIR_DEC(idx, "Bett",         800, 1600, 200, 400, 1600*4,  1600*8,      0, true, false, 18, s_vb_bett, 1, format_bett);
    idx++;
    PAIR_DEC(idx, "Clemsa",       375, 1125, 120, 360, 375*30,  375*42,      0, true, false, 18, s_vb_clemsa, 1, format_clemsa);
    idx++;
    PAIR_DEC(idx, "Dickert MAHS", 286,  572,  80, 160, 286*30,  286*42,      0, true, false, 36, s_vb_dickert_mahs, 1, format_dickert_mahs);
    idx++;
    PAIR_DEC(idx, "Dooya",        366,  733, 120, 240, 733*10,  733*16,      0, true, false, 40, s_vb_dooya, 1, format_dooya);
    idx++;
    PAIR_DEC(idx, "Elplast",      230, 1550, 160, 320, 1550*6,  1550*10,     0, true, false, 18, s_vb_elplast, 1, format_elplast);
    idx++;
    PAIR_DEC(idx, "Hollarm",      200, 1000, 200, 200, 200*8,   200*16,      0, true, false, 42, s_vb_hollarm, 1, format_hollarm);
    idx++;
    PAIR_DEC(idx, "Hay21",        300,  700, 150, 150, 700*4,   700*8,       0, true, false, 21, s_vb_hay21, 1, format_hay21);
    idx++;
    PAIR_DEC(idx, "Feron",        350,  750, 150, 150, 750*4,   750*8,       0, true, false, 32, s_vb_feron, 1, format_feron);
    idx++;
    PAIR_DEC(idx, "Roger",        500, 1000, 270, 270, 500*12,  500*24,      0, true, false, 28, s_vb_roger, 1, format_roger);
    idx++;
    PAIR_DEC(idx, "Treadmill37",  300,  900, 150, 150, 300*12,  300*28,      0, true, false, 37, s_vb_treadmill37, 1, format_treadmill37);
    idx++;
    PAIR_DEC(idx, "Nord ICE",     300,  800, 150, 150, 300*18,  300*30,      0, true, false, 33, s_vb_nord_ice, 1, format_nord_ice);
    idx++;

    #undef PAIR_DEC

    engine->count = (size_t)idx;
}

void IRAM_ATTR subghz_engine_feed(subghz_decoder_engine_t *engine, bool level, uint32_t duration) {
    if (engine->found) return;
    for (size_t i = 0; i < engine->count; i++) {
        if (engine->decoders[i].feed_fn(&engine->decoders[i], level, duration)) {
            engine->found = true;
            engine->found_idx = i;
            return;
        }
    }
}

void subghz_engine_reset(subghz_decoder_engine_t *engine) {
    for (size_t i = 0; i < engine->count; i++) {
        engine->decoders[i].state = SD_STATE_IDLE;
        engine->decoders[i].code = 0;
        engine->decoders[i].bits = 0;
        engine->decoders[i].decoded = false;
        engine->decoders[i].prev_duration = 0;
        engine->decoders[i].pre_count = 0;
        engine->decoders[i].man_state = ManStateMid1;
    }
    engine->found = false;
    engine->found_idx = 0;
}

const subghz_stream_decoder_t *subghz_engine_get_result(const subghz_decoder_engine_t *engine) {
    if (!engine->found || engine->found_idx >= engine->count) return NULL;
    return &engine->decoders[engine->found_idx];
}

void subghz_stream_decoder_format_result(const subghz_stream_decoder_t *dec, char *out, size_t out_len) {
    if (!dec || !out || out_len == 0) return;
    if (dec->format_fn) {
        dec->format_fn(dec->code, dec->bits, out, out_len);
    } else {
        format_generic(dec->name, dec->code, dec->bits, out, out_len);
    }
}

bool subghz_decode_ansonic(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 500, te_l = 1000, te_d = 150;
    const int min_bits = 12;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_l * 12) > te_d * 12) continue;

        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d * 2 && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_bett(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 800, te_l = 1600, te_d = 200;
    const int min_bits = 18;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        if (DUR_ABS(dur[s]) < te_l * 4) continue;

        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d * 2 && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_clemsa(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 375, te_l = 1125, te_d = 120;
    const int min_bits = 18;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        int32_t asyn = DUR_ABS(dur[s]);
        if (DURATION_DIFF(asyn, te_s * 36) > te_d * 10) continue;

        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d * 3) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d * 3 && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_dickert_mahs(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 286, te_l = 572, te_d = 80;
    const int min_bits = 36;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        int32_t asyn = DUR_ABS(dur[s]);
        if (DURATION_DIFF(asyn, te_s * 36) > te_d * 10) continue;

        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d * 2 && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_dooya(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 366, te_l = 733, te_d = 120;
    const int min_bits = 40;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        int32_t asyn = DUR_ABS(dur[s]);
        if (DURATION_DIFF(asyn, te_l * 12) > te_d * 20) continue;

        size_t i = s + 1;
        if (i >= count || dur[i] <= 0) continue;
        int32_t start_hi = DUR_ABS(dur[i]);
        if (DURATION_DIFF(start_hi, te_s * 13) > te_d * 5) continue;

        i++;
        if (i >= count || dur[i] >= 0) continue;
        int32_t start_lo = DUR_ABS(dur[i]);
        if (DURATION_DIFF(start_lo, te_l * 2) > te_d * 3) continue;

        i++;
        uint64_t code = 0;
        int bits = 0;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d * 2) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d * 2 && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_elplast(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 230, te_l = 1550, te_d = 160;
    const int min_bits = 18;

    for (size_t s = 0; s < count; s++) {
        if (dur[s] >= 0) continue;
        int32_t asyn = DUR_ABS(dur[s]);
        if (DURATION_DIFF(asyn, te_l * 8) > te_d * 13) continue;

        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1) | 0;
                bits++;
            } else if (DURATION_DIFF(alo, te_l * 8) < te_d * 13) {
                if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                } else if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1) | 0;
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits >= min_bits) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_marantec24(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 20) return false;
    const int32_t te_s = 800, te_l = 1600, te_d = 200;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_l * 9 + te_s) > te_d * 5 &&
            DURATION_DIFF(DUR_ABS(dur[s]), te_l * 9) > te_d * 5) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s * 3) < te_d * 2) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l * 2) < te_d * 2) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(alo, te_l * 9 + te_s) < te_d * 5 || DURATION_DIFF(alo, te_l * 9) < te_d * 5) {
                if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1);
                    bits++;
                } else if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 24) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_hollarm(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 200, te_l = 1000, te_d = 200;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_s * 12) > te_d * 2) continue;
        uint64_t raw = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                raw = (raw << 1);
                bits++;
            } else if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_s * 8) < te_d) {
                raw = (raw << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(alo, te_s * 12) < te_d) {
                raw = (raw << 1);
                bits++;
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 42) {
            uint64_t code = raw >> 2;
            uint8_t sum = (uint8_t)(((code >> 32) & 0xFF) + ((code >> 24) & 0xFF) +
                                    ((code >> 16) & 0xFF) + ((code >> 8) & 0xFF));
            if (sum == (uint8_t)(code & 0xFF)) {
                *out_code = code;
                *out_bits = bits;
                return true;
            }
        }
    }
    return false;
}

bool subghz_decode_hay21(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 24) return false;
    const int32_t te_s = 300, te_l = 700, te_d = 150;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_l * 6) > te_d * 3) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(alo, te_l * 6) < te_d * 2) {
                if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                } else if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1);
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 21) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_feron(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 34) return false;
    const int32_t te_s = 350, te_l = 750, te_d = 150;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_l * 6) > te_d * 4) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(alo, te_s + 150) < te_d) {
                if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1);
                    bits++;
                } else if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 32) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_roger(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 30) return false;
    const int32_t te_s = 500, te_l = 1000, te_d = 270;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_s * 19) > te_d * 5) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(alo, te_s * 19) < te_d * 5) {
                if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                } else if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1);
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 28) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_treadmill37(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 300, te_l = 900, te_d = 150;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_s * 20) > te_d * 4) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(alo, te_s * 20) < te_d * 4) {
                if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1);
                    bits++;
                } else if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 37) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

bool subghz_decode_keyfinder(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 40) return false;
    const int32_t te_s = 400, te_l = 1200, te_d = 150;
    for (size_t s = 0; s + 10 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_s * 10) > te_d * 5) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 24) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1);
                bits++;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 24 && i + 7 < count) {
            bool suffix_ok = true;
            for (int k = 0; k < 3; k++) {
                int32_t hi = dur[i + k * 2], lo = dur[i + k * 2 + 1];
                if (hi <= 0 || lo >= 0 || DURATION_DIFF(DUR_ABS(hi), te_s) > te_d || DURATION_DIFF(DUR_ABS(lo), te_s) > te_d) {
                    suffix_ok = false;
                    break;
                }
            }
            if (suffix_ok) {
                int32_t hi = dur[i + 6], lo = dur[i + 7];
                if (hi > 0 && lo < 0 && DURATION_DIFF(DUR_ABS(hi), te_s) < te_d && DURATION_DIFF(DUR_ABS(lo), te_s * 10) < te_d * 5) {
                    *out_code = code;
                    *out_bits = bits;
                    return true;
                }
            }
        }
    }
    return false;
}

bool subghz_decode_nord_ice(const int32_t *dur, size_t count, uint64_t *out_code, int *out_bits) {
    if (count < 36) return false;
    const int32_t te_s = 300, te_l = 800, te_d = 150;
    for (size_t s = 0; s + 2 < count; s++) {
        if (dur[s] >= 0) continue;
        if (DURATION_DIFF(DUR_ABS(dur[s]), te_s * 25) > te_d * 11) continue;
        uint64_t code = 0;
        int bits = 0;
        size_t i = s + 1;
        while (i + 1 < count && bits < 64) {
            int32_t hi = dur[i], lo = dur[i + 1];
            if (hi <= 0 || lo >= 0) break;
            int32_t ahi = DUR_ABS(hi), alo = DUR_ABS(lo);
            if (DURATION_DIFF(ahi, te_s) < te_d && DURATION_DIFF(alo, te_l) < te_d) {
                code = (code << 1);
                bits++;
            } else if (DURATION_DIFF(ahi, te_l) < te_d && DURATION_DIFF(alo, te_s) < te_d) {
                code = (code << 1) | 1;
                bits++;
            } else if (DURATION_DIFF(alo, te_s * 25) < te_d * 11) {
                if (DURATION_DIFF(ahi, te_s) < te_d) {
                    code = (code << 1);
                    bits++;
                } else if (DURATION_DIFF(ahi, te_l) < te_d) {
                    code = (code << 1) | 1;
                    bits++;
                }
                break;
            } else {
                break;
            }
            i += 2;
        }
        if (bits == 33) {
            *out_code = code;
            *out_bits = bits;
            return true;
        }
    }
    return false;
}

static const subghz_decoder_entry_t s_decoders[] = {
    {"Princeton",    subghz_decode_princeton,      format_princeton},
    {"CAME",         subghz_decode_came,            format_came},
    {"Nice FLO",     subghz_decode_nice_flo,        format_nice_flo},
    {"PT2260",       subghz_decode_pt2260,          format_pt2260},
    {"PT2262",       subghz_decode_pt2262,          format_pt2260},
    {"CAME Atomo",   subghz_decode_came_atomo,      NULL},
    {"Nice FLO-R-S", subghz_decode_nice_flor_s,     NULL},
    {"Chamberlain",  subghz_decode_chamberlain,     format_chamberlain},
    {"Linear",       subghz_decode_linear,          format_linear},
    {"Linear D3",    subghz_decode_linear_delta3,   format_linear},
    {"UNILARM",      subghz_decode_unilarm,         NULL},
    {"GangQi",       subghz_decode_gangqi,          NULL},
    {"Holtek",       subghz_decode_holtek,          NULL},
    {"Holtek HT12x", subghz_decode_holtek_ht12x,    NULL},
    {"Doitrand",     subghz_decode_doitrand,        NULL},
    {"Gate TX",      subghz_decode_gate_tx,         NULL},
    {"KeeLoq",       subghz_decode_keeloq,          format_keeloq},
    {"FAAC SLH",     subghz_decode_faac_slh,        NULL},
    {"Alutech",      subghz_decode_alutech_at_4n,   NULL},
    {"Marantec",     subghz_decode_marantec,        format_marantec},
    {"Ansonic",      subghz_decode_ansonic,          format_ansonic},
    {"Bett",         subghz_decode_bett,             format_bett},
    {"Clemsa",       subghz_decode_clemsa,           format_clemsa},
    {"Dickert MAHS", subghz_decode_dickert_mahs,      format_dickert_mahs},
    {"Dooya",        subghz_decode_dooya,            format_dooya},
    {"Elplast",      subghz_decode_elplast,          format_elplast},
    {"Marantec24",   subghz_decode_marantec24,       format_marantec24},
    {"Hollarm",      subghz_decode_hollarm,          format_hollarm},
    {"Hay21",        subghz_decode_hay21,            format_hay21},
    {"Feron",        subghz_decode_feron,            format_feron},
    {"Roger",        subghz_decode_roger,            format_roger},
    {"Treadmill37",  subghz_decode_treadmill37,      format_treadmill37},
    {"KeyFinder",    subghz_decode_keyfinder,        format_keyfinder},
    {"Nord ICE",     subghz_decode_nord_ice,         format_nord_ice},
};

#define NUM_DECODERS (sizeof(s_decoders) / sizeof(s_decoders[0]))

int32_t subghz_protocol_te(const char *protocol) {
    if (!protocol) return 0;
    if (strcmp(protocol, "Princeton") == 0) return 390;
    if (strcmp(protocol, "CAME") == 0) return 320;
    if (strcmp(protocol, "Nice FLO") == 0) return 700;
    if (strcmp(protocol, "PT2260") == 0) return 350;
    if (strcmp(protocol, "PT2262") == 0) return 350;
    if (strcmp(protocol, "CAME Atomo") == 0) return 600;
    if (strcmp(protocol, "Nice FLO-R-S") == 0) return 500;
    if (strcmp(protocol, "Chamberlain") == 0) return 1000;
    if (strcmp(protocol, "Linear") == 0) return 500;
    if (strcmp(protocol, "Linear D3") == 0) return 500;
    if (strcmp(protocol, "UNILARM") == 0) return 320;
    if (strcmp(protocol, "GangQi") == 0) return 500;
    if (strcmp(protocol, "Holtek") == 0) return 430;
    if (strcmp(protocol, "Holtek HT12x") == 0) return 320;
    if (strcmp(protocol, "Doitrand") == 0) return 400;
    if (strcmp(protocol, "Gate TX") == 0) return 350;
    if (strcmp(protocol, "KeeLoq") == 0) return 400;
    if (strcmp(protocol, "FAAC SLH") == 0) return 255;
    if (strcmp(protocol, "Alutech") == 0) return 400;
    if (strcmp(protocol, "Marantec") == 0) return 1000;
    if (strcmp(protocol, "Ansonic") == 0) return 500;
    if (strcmp(protocol, "Bett") == 0) return 800;
    if (strcmp(protocol, "Clemsa") == 0) return 375;
    if (strcmp(protocol, "Dickert MAHS") == 0) return 286;
    if (strcmp(protocol, "Dooya") == 0) return 366;
    if (strcmp(protocol, "Elplast") == 0) return 230;
    if (strcmp(protocol, "Marantec24") == 0) return 800;
    if (strcmp(protocol, "Hollarm") == 0) return 200;
    if (strcmp(protocol, "Hay21") == 0) return 300;
    if (strcmp(protocol, "Feron") == 0) return 350;
    if (strcmp(protocol, "Roger") == 0) return 500;
    if (strcmp(protocol, "Treadmill37") == 0) return 300;
    if (strcmp(protocol, "KeyFinder") == 0) return 400;
    if (strcmp(protocol, "Nord ICE") == 0) return 300;
    return 0;
}

int subghz_normalize_decoded_bits(const char *protocol, int bits) {
    if (protocol && strcmp(protocol, "KeeLoq") == 0 && bits >= 64) {
        return 64;
    }
    return bits;
}

bool subghz_decode_signal(const int32_t *dur, size_t count, subghz_decoded_signal_t *result) {
    if (!dur || count < 4 || !result) return false;

    memset(result, 0, sizeof(*result));

    for (size_t d = 0; d < NUM_DECODERS; d++) {
        uint64_t code = 0;
        int bits = 0;
        if (s_decoders[d].decode(dur, count, &code, &bits)) {
            result->decoded = true;
            result->code = code;
            result->bits = bits;
            snprintf(result->protocol, sizeof(result->protocol), "%s", s_decoders[d].name);
            result->te = (int)subghz_protocol_te(s_decoders[d].name);

            if (s_decoders[d].format) {
                s_decoders[d].format(code, bits, result->info, sizeof(result->info));
            } else {
                format_generic(s_decoders[d].name, code, bits, result->info, sizeof(result->info));
            }
            return true;
        }
    }

    return false;
}

static bool sd_push(int32_t *out, size_t max_count, size_t *count, int32_t v) {
    if (!out || !count || *count >= max_count) {
        return false;
    }
    out[*count] = v;
    (*count)++;
    return true;
}

static bool sd_push_pair(int32_t *out, size_t max_count, size_t *count, int32_t high_us, int32_t low_us) {
    return sd_push(out, max_count, count, high_us) && sd_push(out, max_count, count, -low_us);
}

static bool sd_get_code_bit(uint64_t code, int bit_index) {
    if (bit_index < 0 || bit_index >= 64) {
        return false;
    }
    return ((code >> bit_index) & 1ULL) != 0;
}

static bool sd_emit_pair_low_first(int32_t *out,
                                   size_t max_count,
                                   size_t *count,
                                   uint64_t code,
                                   int bits,
                                   int32_t te_short,
                                   int32_t te_long,
                                   int32_t sync_low,
                                   int32_t start_high,
                                   int repeats) {
    if (bits <= 0 || bits > 64 || repeats <= 0) {
        return false;
    }

    for (int r = 0; r < repeats; r++) {
        if (!sd_push(out, max_count, count, -sync_low)) return false;
        if (!sd_push(out, max_count, count, start_high)) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t low = bit ? te_long : te_short;
            int32_t high = bit ? te_short : te_long;
            if (!sd_push(out, max_count, count, -low)) return false;
            if (!sd_push(out, max_count, count, high)) return false;
        }
        if (!sd_push(out, max_count, count, -(sync_low / 2))) return false;
    }

    return true;
}

static bool sd_emit_pair_high_first(int32_t *out,
                                    size_t max_count,
                                    size_t *count,
                                    uint64_t code,
                                    int bits,
                                    int32_t te_short,
                                    int32_t te_long,
                                    int32_t sync_low,
                                    int32_t start_high,
                                    int repeats) {
    if (bits <= 0 || bits > 64 || repeats <= 0) {
        return false;
    }

    for (int r = 0; r < repeats; r++) {
        if (sync_low > 0) {
            if (!sd_push(out, max_count, count, -sync_low)) return false;
        }
        if (start_high > 0) {
            if (!sd_push(out, max_count, count, start_high)) return false;
        }
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_long : te_short;
            int32_t low = bit ? te_short : te_long;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(sync_low > 0 ? sync_low / 2 : te_long * 8))) return false;
    }

    return true;
}

static bool sd_emit_princeton(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 390;
    const int32_t te_l = 1170;
    const int32_t sync_l = te_s * 36;
    return sd_emit_pair_high_first(out, max_count, count, code, bits, te_s, te_l, sync_l, 0, 3);
}

static bool sd_emit_came(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 320;
    const int32_t te_l = 640;
    const int32_t sync_l = te_s * 56;
    return sd_emit_pair_low_first(out, max_count, count, code, bits, te_s, te_l, sync_l, te_s, 4);
}

static bool sd_emit_nice_flo(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 700;
    const int32_t te_l = 1400;
    const int32_t sync_l = te_s * 36;
    return sd_emit_pair_low_first(out, max_count, count, code, bits, te_s, te_l, sync_l, te_s, 4);
}

static bool sd_emit_keeloq(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 400;
    const int32_t te_l = 800;
    const int32_t gap = te_s * 40;
    if (bits != 64) {
        return false;
    }

    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 11; i++) {
            if (!sd_push_pair(out, max_count, count, te_s, te_s)) return false;
        }
        if (!sd_push_pair(out, max_count, count, te_s, te_s * 10)) return false;

        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (bit) {
                if (!sd_push_pair(out, max_count, count, te_s, te_l)) return false;
            } else {
                if (!sd_push_pair(out, max_count, count, te_l, te_s)) return false;
            }
        }

        if (!sd_push_pair(out, max_count, count, te_s, te_l)) return false;
        if (!sd_push(out, max_count, count, te_s)) return false;
        if (!sd_push(out, max_count, count, -gap)) return false;
    }

    return true;
}

static bool sd_emit_pt226x(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te = 350;
    if (bits < 2 || (bits % 2) != 0 || bits > 64) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te * 20))) return false;

    for (int b = bits - 2; b >= 0; b -= 2) {
        uint8_t sym = (uint8_t)((code >> b) & 0x3ULL);
        int32_t first = (sym == 1) ? te : (te * 3);
        int32_t second = (sym == 2) ? te : (te * 3);
        if (!sd_push_pair(out, max_count, count, first, te)) return false;
        if (!sd_push_pair(out, max_count, count, second, te)) return false;
    }
    if (!sd_push(out, max_count, count, -(te * 12))) return false;
    return true;
}

static bool sd_emit_nice_flor_s(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500;
    const int32_t te_l = 1000;
    if (bits <= 0 || bits > 80) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_s * 38))) return false;
    if (!sd_push(out, max_count, count, te_s * 3)) return false;
    if (!sd_push(out, max_count, count, -(te_s * 3))) return false;

    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        int32_t high = bit ? te_l : te_s;
        int32_t low = bit ? te_s : te_l;
        if (!sd_push(out, max_count, count, high)) return false;
        if (!sd_push(out, max_count, count, -low)) return false;
    }
    if (!sd_push(out, max_count, count, te_s * 3)) return false;
    if (!sd_push(out, max_count, count, -(te_s * 38))) return false;
    return true;
}

static bool sd_emit_chamberlain(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te = 1000;
    if (bits <= 0 || bits > 12) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te * 39))) return false;
    if (!sd_push(out, max_count, count, te)) return false;

    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        if (bit) {
            if (!sd_push(out, max_count, count, -(te * 2))) return false;
            if (!sd_push(out, max_count, count, te * 2)) return false;
        } else {
            if (!sd_push(out, max_count, count, -te)) return false;
            if (!sd_push(out, max_count, count, te * 3)) return false;
        }
    }
    if (!sd_push(out, max_count, count, -(te * 3))) return false;
    if (!sd_push(out, max_count, count, te)) return false;
    if (!sd_push(out, max_count, count, -(te * 3))) return false;
    if (!sd_push(out, max_count, count, te)) return false;
    return true;
}

static bool sd_emit_linear(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500;
    const int32_t te_l = 1500;
    if (bits <= 0 || bits > 12) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_s * 42))) return false;
    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        int32_t high = bit ? te_l : te_s;
        int32_t low = bit ? te_s : te_l;
        if (!sd_push(out, max_count, count, high)) return false;
        if (!sd_push(out, max_count, count, -low)) return false;
    }
    if (!sd_push(out, max_count, count, -(te_s * 12))) return false;
    return true;
}

static bool sd_emit_linear_d3(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500;
    const int32_t te_l = 2000;
    if (bits <= 0 || bits > 12) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_s * 70))) return false;
    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        if (bit) {
            if (!sd_push(out, max_count, count, te_s)) return false;
            if (!sd_push(out, max_count, count, -(te_s * 7))) return false;
        } else {
            if (!sd_push(out, max_count, count, te_l)) return false;
            if (!sd_push(out, max_count, count, -te_l)) return false;
        }
    }
    if (!sd_push(out, max_count, count, -(te_s * 12))) return false;
    return true;
}

static bool sd_emit_unilarm(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 100;
    const int32_t te_l = 700;
    if (bits <= 0 || bits > 32) {
        return false;
    }
    for (int i = 0; i < 12; i++) {
        if (!sd_push_pair(out, max_count, count, te_s, te_s)) return false;
    }
    if (!sd_push(out, max_count, count, -(te_s * 30))) return false;
    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        if (bit) {
            if (!sd_push_pair(out, max_count, count, te_s, te_l)) return false;
        } else {
            if (!sd_push_pair(out, max_count, count, te_l, te_s)) return false;
        }
    }
    if (!sd_push(out, max_count, count, -(te_s * 30))) return false;
    return true;
}

static bool sd_emit_gangqi(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500;
    const int32_t te_l = 1200;
    if (bits <= 0 || bits > 36) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_l * 2))) return false;
    for (int b = bits - 1; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        int32_t high = bit ? te_l : te_s;
        int32_t low = bit ? te_s : te_l;
        if (!sd_push(out, max_count, count, high)) return false;
        if (!sd_push(out, max_count, count, -low)) return false;
    }
    if (!sd_push(out, max_count, count, -(te_l * 2))) return false;
    return true;
}

static bool sd_emit_holtek(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    return sd_emit_pair_low_first(out, max_count, count, code, bits, 430, 870, 430 * 36, 430, 3);
}

static bool sd_emit_holtek_ht12x(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    return sd_emit_pair_low_first(out, max_count, count, code, bits, 320, 640, 320 * 28, 320, 3);
}

static bool sd_emit_doitrand(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    return sd_emit_pair_low_first(out, max_count, count, code, bits, 400, 1100, 400 * 62, 800, 3);
}

static bool sd_emit_gate_tx(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    return sd_emit_pair_low_first(out, max_count, count, code, bits, 350, 700, 350 * 47, 700, 3);
}

static bool sd_emit_faac_slh(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 255;
    const int32_t te_l = 595;
    if (bits <= 0 || bits > 66) {
        return false;
    }
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, te_l * 2)) return false;
        if (!sd_push(out, max_count, count, -(te_l * 2))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_l * 8))) return false;
    }
    return true;
}

static bool sd_emit_ansonic(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500, te_l = 1000;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_l * 12 + te_s))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_l * 4))) return false;
    }
    return true;
}

static bool sd_emit_bett(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 800, te_l = 1600;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_l * 4))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_l * 4))) return false;
    }
    return true;
}

static bool sd_emit_clemsa(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 375, te_l = 1125;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_s * 36))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_s * 36))) return false;
    }
    return true;
}

static bool sd_emit_dickert_mahs(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 286, te_l = 572;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_s * 36))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_s * 36))) return false;
    }
    return true;
}

static bool sd_emit_dooya(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 366, te_l = 733;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_l * 12 + te_l))) return false;
        if (!sd_push(out, max_count, count, te_s * 13)) return false;
        if (!sd_push(out, max_count, count, -(te_l * 2))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            int32_t high = bit ? te_l : te_s;
            int32_t low = bit ? te_s : te_l;
            if (!sd_push(out, max_count, count, high)) return false;
            if (!sd_push(out, max_count, count, -low)) return false;
        }
        if (!sd_push(out, max_count, count, -(te_l * 12))) return false;
    }
    return true;
}

static bool sd_emit_elplast(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 230, te_l = 1550;
    if (bits <= 0 || bits > 64) return false;
    for (int r = 0; r < 3; r++) {
        if (!sd_push(out, max_count, count, -(te_l * 8))) return false;
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (bit) {
                if (!sd_push(out, max_count, count, te_l)) return false;
                if (b == 0) {
                    if (!sd_push(out, max_count, count, -(te_l * 8))) return false;
                } else {
                    if (!sd_push(out, max_count, count, -te_s)) return false;
                }
            } else {
                if (!sd_push(out, max_count, count, te_s)) return false;
                if (b == 0) {
                    if (!sd_push(out, max_count, count, -(te_l * 8))) return false;
                } else {
                    if (!sd_push(out, max_count, count, -te_l)) return false;
                }
            }
        }
    }
    return true;
}

static bool sd_emit_marantec24(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 800, te_l = 1600;
    if (bits != 24) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (bit) {
                if (!sd_push(out, max_count, count, te_s)) return false;
                if (!sd_push(out, max_count, count, -(b == 0 ? (te_l * 9 + te_s) : (te_l * 2)))) return false;
            } else {
                if (!sd_push(out, max_count, count, te_l)) return false;
                if (!sd_push(out, max_count, count, -(b == 0 ? (te_l * 9 + te_s) : (te_s * 3)))) return false;
            }
        }
    }
    return true;
}

static bool sd_emit_hollarm(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 200, te_l = 1000;
    uint64_t raw = code << 2;
    if (bits != 42) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(raw, b);
            if (!sd_push(out, max_count, count, te_s)) return false;
            if (!sd_push(out, max_count, count, -(b == 0 ? (te_s * 12) : (bit ? (te_s * 8) : te_l)))) return false;
        }
    }
    return true;
}

static bool sd_emit_hay21(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 300, te_l = 700;
    if (bits != 21) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_l : te_s)) return false;
            if (!sd_push(out, max_count, count, -(b == 0 ? (te_l * 6) : (bit ? te_s : te_l)))) return false;
        }
    }
    return true;
}

static bool sd_emit_feron(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 350, te_l = 750;
    if (bits != 32) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_l : te_s)) return false;
            if (b == 0) {
                if (!sd_push(out, max_count, count, -(te_s + 150))) return false;
                if (!sd_push(out, max_count, count, te_s + 150)) return false;
                if (!sd_push(out, max_count, count, -(te_l * 6))) return false;
            } else {
                if (!sd_push(out, max_count, count, -(bit ? te_s : te_l))) return false;
            }
        }
    }
    return true;
}

static bool sd_emit_roger(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 500, te_l = 1000;
    if (bits != 28) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_l : te_s)) return false;
            if (!sd_push(out, max_count, count, -(b == 0 ? (te_s * 19) : (bit ? te_s : te_l)))) return false;
        }
    }
    return true;
}

static bool sd_emit_treadmill37(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 300, te_l = 900;
    if (bits != 37) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_l : te_s)) return false;
            if (!sd_push(out, max_count, count, -(b == 0 ? (te_s * 20) : (bit ? te_s : te_l)))) return false;
        }
    }
    return true;
}

static bool sd_emit_keyfinder(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 400, te_l = 1200;
    if (bits != 24) return false;
    for (int r = 0; r < 5; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_s : te_l)) return false;
            if (!sd_push(out, max_count, count, -(bit ? te_l : te_s))) return false;
        }
        for (int i = 0; i < 3; i++) {
            if (!sd_push(out, max_count, count, te_s)) return false;
            if (!sd_push(out, max_count, count, -te_s)) return false;
        }
        if (!sd_push(out, max_count, count, te_s)) return false;
        if (!sd_push(out, max_count, count, -(te_s * 10))) return false;
    }
    return true;
}

static bool sd_emit_nord_ice(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 300, te_l = 800;
    if (bits != 33) return false;
    for (int r = 0; r < 3; r++) {
        for (int b = bits - 1; b >= 0; b--) {
            bool bit = sd_get_code_bit(code, b);
            if (!sd_push(out, max_count, count, bit ? te_l : te_s)) return false;
            if (!sd_push(out, max_count, count, -(b == 0 ? (te_s * 25) : (bit ? te_s : te_l)))) return false;
        }
    }
    return true;
}

static bool sd_emit_alutech(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 400;
    const int32_t te_l = 800;
    if (bits <= 1 || bits > 74) {
        return false;
    }
    for (int i = 0; i < 10; i++) {
        if (!sd_push_pair(out, max_count, count, te_s, te_s)) return false;
    }
    if (!sd_push_pair(out, max_count, count, te_s, te_s * 10)) return false;

    for (int b = bits - 1; b >= 1; b--) {
        bool bit = sd_get_code_bit(code, b);
        if (bit) {
            if (!sd_push_pair(out, max_count, count, te_s, te_l)) return false;
        } else {
            if (!sd_push_pair(out, max_count, count, te_l, te_s)) return false;
        }
    }

    bool last_bit = sd_get_code_bit(code, 0);
    if (!sd_push(out, max_count, count, last_bit ? te_s : te_l)) return false;
    if (!sd_push(out, max_count, count, -(te_s * 20))) return false;
    return true;
}

static bool sd_push_manchester_event(int32_t *out,
                                     size_t max_count,
                                     size_t *count,
                                     ManEvent ev,
                                     int32_t te_short,
                                     int32_t te_long) {
    switch (ev) {
    case ManEventShortLow:
        return sd_push(out, max_count, count, -te_short);
    case ManEventShortHigh:
        return sd_push(out, max_count, count, te_short);
    case ManEventLongLow:
        return sd_push(out, max_count, count, -te_long);
    case ManEventLongHigh:
        return sd_push(out, max_count, count, te_long);
    default:
        return false;
    }
}

static bool sd_find_manchester_bit_sequence(ManState start_state,
                                            bool target_data,
                                            ManEvent *out_events,
                                            int *out_event_count,
                                            ManState *out_next_state) {
    static const ManEvent candidates[] = {
        ManEventShortHigh,
        ManEventShortLow,
        ManEventLongHigh,
        ManEventLongLow,
    };

    typedef struct {
        ManState state;
        int len;
        ManEvent seq[6];
    } node_t;

    node_t queue[32];
    int head = 0;
    int tail = 0;
    queue[tail++] = (node_t){ .state = start_state, .len = 0 };

    while (head < tail) {
        node_t cur = queue[head++];
        if (cur.len >= 5) {
            continue;
        }

        for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
            ManState next_state = cur.state;
            bool data = false;
            bool ok = manchester_advance(cur.state, candidates[i], &next_state, &data);

            node_t next = cur;
            next.seq[next.len++] = candidates[i];
            next.state = next_state;

            if (ok) {
                if (data == target_data) {
                    for (int j = 0; j < next.len; j++) {
                        out_events[j] = next.seq[j];
                    }
                    *out_event_count = next.len;
                    *out_next_state = next_state;
                    return true;
                }
                continue;
            }

            if (tail < (int)(sizeof(queue) / sizeof(queue[0]))) {
                queue[tail++] = next;
            }
        }
    }

    return false;
}

static bool sd_emit_manchester_payload(int32_t *out,
                                       size_t max_count,
                                       size_t *count,
                                       uint64_t code,
                                       int bits,
                                       int32_t te_short,
                                       int32_t te_long,
                                       bool invert_data,
                                       int skip_msb_bits) {
    ManState state = ManStateMid1;
    manchester_advance(state, ManEventReset, &state, NULL);

    int payload_bits = bits - skip_msb_bits;
    if (payload_bits <= 0 || payload_bits > 64) {
        return false;
    }

    for (int b = bits - 1 - skip_msb_bits; b >= 0; b--) {
        bool bit = sd_get_code_bit(code, b);
        bool target_data = invert_data ? !bit : bit;
        ManEvent seq[6];
        int seq_len = 0;
        ManState next_state = state;
        if (!sd_find_manchester_bit_sequence(state, target_data, seq, &seq_len, &next_state)) {
            return false;
        }
        for (int i = 0; i < seq_len; i++) {
            if (!sd_push_manchester_event(out, max_count, count, seq[i], te_short, te_long)) {
                return false;
            }
        }
        state = next_state;
    }

    return true;
}

static bool sd_emit_came_atomo(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 600;
    const int32_t te_l = 1200;
    if (bits <= 0 || bits > 66) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_l * 10))) return false;
    if (!sd_emit_manchester_payload(out, max_count, count, code, bits, te_s, te_l, true, 0)) return false;
    if (!sd_push(out, max_count, count, -(te_l * 5))) return false;
    return true;
}

static bool sd_emit_marantec(int32_t *out, size_t max_count, size_t *count, uint64_t code, int bits) {
    const int32_t te_s = 1000;
    const int32_t te_l = 2000;
    if (bits <= 1 || bits > 52) {
        return false;
    }
    if (!sd_push(out, max_count, count, -(te_l * 5))) return false;
    if (!sd_emit_manchester_payload(out, max_count, count, code, bits, te_s, te_l, false, 1)) return false;
    if (!sd_push(out, max_count, count, -(te_l * 2))) return false;
    return true;
}

bool subghz_build_raw_from_decoded(const char *protocol,
                                   uint64_t code,
                                   int bits,
                                   int32_t *out,
                                   size_t max_count,
                                   size_t *out_count) {
    if (!protocol || !out || max_count == 0) {
        return false;
    }

    size_t count = 0;
    bool ok = false;

    if (strcmp(protocol, "KeeLoq") == 0) {
        ok = sd_emit_keeloq(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "FAAC SLH") == 0) {
        ok = sd_emit_faac_slh(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Alutech") == 0) {
        ok = sd_emit_alutech(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "CAME") == 0) {
        ok = sd_emit_came(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "CAME Atomo") == 0) {
        ok = sd_emit_came_atomo(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Nice FLO") == 0) {
        ok = sd_emit_nice_flo(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Nice FLO-R-S") == 0 || strcmp(protocol, "Nice Flor S") == 0) {
        ok = sd_emit_nice_flor_s(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Chamberlain") == 0) {
        ok = sd_emit_chamberlain(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Linear") == 0) {
        ok = sd_emit_linear(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Linear D3") == 0) {
        ok = sd_emit_linear_d3(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "UNILARM") == 0) {
        ok = sd_emit_unilarm(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "GangQi") == 0) {
        ok = sd_emit_gangqi(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Holtek") == 0) {
        ok = sd_emit_holtek(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Holtek HT12x") == 0) {
        ok = sd_emit_holtek_ht12x(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Doitrand") == 0) {
        ok = sd_emit_doitrand(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Gate TX") == 0) {
        ok = sd_emit_gate_tx(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Princeton") == 0 || strcmp(protocol, "PT2260") == 0 || strcmp(protocol, "PT2262") == 0) {
        if (strcmp(protocol, "Princeton") == 0) ok = sd_emit_princeton(out, max_count, &count, code, bits);
        else ok = sd_emit_pt226x(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Marantec") == 0) {
        ok = sd_emit_marantec(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Ansonic") == 0) {
        ok = sd_emit_ansonic(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Bett") == 0) {
        ok = sd_emit_bett(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Clemsa") == 0) {
        ok = sd_emit_clemsa(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Dickert MAHS") == 0) {
        ok = sd_emit_dickert_mahs(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Dooya") == 0) {
        ok = sd_emit_dooya(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Elplast") == 0) {
        ok = sd_emit_elplast(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Marantec24") == 0) {
        ok = sd_emit_marantec24(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Hollarm") == 0) {
        ok = sd_emit_hollarm(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Hay21") == 0) {
        ok = sd_emit_hay21(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Feron") == 0) {
        ok = sd_emit_feron(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Roger") == 0) {
        ok = sd_emit_roger(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Treadmill37") == 0) {
        ok = sd_emit_treadmill37(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "KeyFinder") == 0) {
        ok = sd_emit_keyfinder(out, max_count, &count, code, bits);
    } else if (strcmp(protocol, "Nord ICE") == 0) {
        ok = sd_emit_nord_ice(out, max_count, &count, code, bits);
    }

    if (ok && out_count) {
        *out_count = count;
    }
    return ok;
}
