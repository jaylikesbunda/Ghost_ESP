// lora_unishox.c — decode-only Unishox2 DECOMPRESSOR (no compressor).
//
// Vendored decode path from siara-cc/Unishox2 unishox2.c (Apache-2.0;
// copyright Siara Logics, see LICENSES/unishox2-Apache-2.0.txt).
// Encode-only symbols (usx_code_94, init_coder, append_*, encodeCount,
// encodeUnicode, readUTF8, matchOccurance, matchLine, compress_*) are
// dropped. prev_lines (string-array) support is dropped: Mesh port-7
// frames never use it (upstream decompress_simple passes NULL).
// Bounds-checked variant throughout (olen honored on every output);
// all tables are static const, no malloc.
//
// Interop: default preset (USX_PSET_DFLT) + 1 magic bit — byte-identical
// to what Meshtastic firmware's unishox2_decompress_simple() decodes
// (src/mesh/compression/unishox2.*, same upstream source).

#include "managers/lora_unishox.h"

#include <string.h>
#include <stdint.h>

// ---- States ----
enum { USX_ALPHA = 0, USX_SYM, USX_NUM, USX_DICT, USX_DELTA };

// 1 magic bit (UNISHOX_MAGIC_BITS 0xFF, UNISHOX_MAGIC_BIT_LEN 1).
#define USX_MAGIC_BIT_LEN 1

// Default preset horizontal codes + lengths (USX_HCODES_DFLT / USX_HCODE_LENS_DFLT).
static const uint8_t usx_hcodes_dflt[] = {0x00, 0x40, 0x80, 0xC0, 0xE0};
static const uint8_t usx_hcode_lens_dflt[] = {2, 2, 2, 3, 3};

// Default frequent sequences (USX_FREQ_SEQ_DFLT) + templates (USX_TEMPLATES).
static const char *usx_freq_seq_dflt[] = {"\": \"", "\": ", "</", "=\"", "\":\"", "://"};
static const char *usx_templates_dflt[] = {"tfff-of-tfTtf:rf:rf.fffZ", "tfff-of-tf", "(fff) fff-ffff", "tf:rf:rf", 0};

// Character sets USX_ALPHA / USX_SYM / USX_NUM.
static const uint8_t usx_sets[][28] = {{  0, ' ', 'e', 't', 'a', 'o', 'i', 'n',
                        's', 'r', 'l', 'c', 'd', 'h', 'u', 'p', 'm', 'b',
                        'g', 'w', 'f', 'y', 'v', 'k', 'q', 'j', 'x', 'z'},
                       {'"', '{', '}', '_', '<', '>', ':', '\n',
                          0, '[', ']', '\\', ';', '\'', '\t', '@', '*', '&',
                        '?', '!', '^', '|', '\r', '~', '`', 0, 0, 0},
                       {  0, ',', '.', '0', '1', '9', '2', '5', '-',
                        '/', '3', '4', '6', '7', '8', '(', ')', ' ',
                        '=', '+', '$', '%', '#', 0, 0, 0, 0, 0}};

// Vertical-code section tables (memory-light vcode decoder).
#define USX_SECTION_COUNT 5
static const uint8_t usx_vsections[] = {0x7F, 0xBF, 0xDF, 0xEF, 0xFF};
static const uint8_t usx_vsection_pos[] = {0, 4, 8, 12, 20};
static const uint8_t usx_vsection_mask[] = {0x7F, 0x3F, 0x1F, 0x0F, 0x0F};
static const uint8_t usx_vsection_shift[] = {5, 4, 3, 1, 0};

// Vertical decoder lookup: 3 bits code len (one less), 5 bits vertical pos.
static const uint8_t usx_vcode_lookup[36] = {
  (1 << 5) + 0,  (1 << 5) + 0,  (2 << 5) + 1,  (2 << 5) + 2,
  (3 << 5) + 3,  (3 << 5) + 4,  (3 << 5) + 5,  (3 << 5) + 6,
  (3 << 5) + 7,  (3 << 5) + 7,  (4 << 5) + 8,  (4 << 5) + 9,
  (5 << 5) + 10, (5 << 5) + 10, (5 << 5) + 11, (5 << 5) + 11,
  (5 << 5) + 12, (5 << 5) + 12, (6 << 5) + 13, (6 << 5) + 14,
  (6 << 5) + 15, (6 << 5) + 15, (6 << 5) + 16, (6 << 5) + 16,
  (6 << 5) + 17, (6 << 5) + 17, (7 << 5) + 18, (7 << 5) + 19,
  (7 << 5) + 20, (7 << 5) + 21, (7 << 5) + 22, (7 << 5) + 23,
  (7 << 5) + 24, (7 << 5) + 25, (7 << 5) + 26, (7 << 5) + 27
};

static const uint8_t usx_len_masks[] = {0x80, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC, 0xFE, 0xFF};

// Count / unicode-delta level tables.
static const uint8_t usx_count_bit_lens[5] = {2, 4, 7, 11, 16};
static const int32_t usx_count_adder[5] = {4, 20, 148, 2196, 67732};
static const uint8_t usx_uni_bit_len[5] = {6, 12, 14, 16, 21};
static const int32_t usx_uni_adder[5] = {0, 64, 4160, 20544, 86080};

#define USX_NICE_LEN 5
#define USX_TERM_BYTE_PRESET_1 0
#define USX_TERM_BYTE_PRESET_1_LEN_LOWER 6
#define USX_TERM_BYTE_PRESET_1_LEN_UPPER 4
#define USX_SW_CODE_LEN 2

enum {USX_NIB_NUM = 0, USX_NIB_HEX_LOWER, USX_NIB_HEX_UPPER, USX_NIB_NOT};

static long usx_min_of(long c, long i) {
  return c > i ? i : c;
}

static int usx_read_bit(const char *in, int bit_no) {
   return in[bit_no >> 3] & (0x80 >> (bit_no % 8));
}

static int usx_read_8bit(const char *in, int len, int bit_no) {
  int bit_pos = bit_no & 0x07;
  int char_pos = bit_no >> 3;
  len >>= 3;
  uint8_t code = (((uint8_t)in[char_pos]) << bit_pos);
  char_pos++;
  if (char_pos < len) {
    code |= ((uint8_t)in[char_pos]) >> (8 - bit_pos);
  } else
    code |= (0xFF >> (8 - bit_pos));
  return code;
}

static int usx_read_vcode(const char *in, int len, int *bit_no_p) {
  if (*bit_no_p < len) {
    uint8_t code = (uint8_t)usx_read_8bit(in, len, *bit_no_p);
    int i = 0;
    do {
      if (code <= usx_vsections[i]) {
        uint8_t vcode = usx_vcode_lookup[usx_vsection_pos[i] + ((code & usx_vsection_mask[i]) >> usx_vsection_shift[i])];
        (*bit_no_p) += ((vcode >> 5) + 1);
        if (*bit_no_p > len)
          return 99;
        return vcode & 0x1F;
      }
    } while (++i < USX_SECTION_COUNT);
  }
  return 99;
}

static int usx_read_hcode(const char *in, int len, int *bit_no_p,
                          const uint8_t usx_hcodes[], const uint8_t usx_hcode_lens[]) {
  if (!usx_hcode_lens[USX_ALPHA])
    return USX_ALPHA;
  if (*bit_no_p < len) {
    uint8_t code = (uint8_t)usx_read_8bit(in, len, *bit_no_p);
    for (int code_pos = 0; code_pos < 5; code_pos++) {
      if (usx_hcode_lens[code_pos] && (code & usx_len_masks[usx_hcode_lens[code_pos] - 1]) == usx_hcodes[code_pos]) {
        *bit_no_p += usx_hcode_lens[code_pos];
        return code_pos;
      }
    }
  }
  return 99;
}

static int usx_step_code(const char *in, int len, int *bit_no_p, int limit) {
  int idx = 0;
  while (*bit_no_p < len && usx_read_bit(in, *bit_no_p)) {
    idx++;
    (*bit_no_p)++;
    if (idx == limit)
      return idx;
  }
  if (*bit_no_p >= len)
    return 99;
  (*bit_no_p)++;
  return idx;
}

static int32_t usx_num_bits(const char *in, int len, int bit_no, int count) {
   int32_t ret = 0;
   while (count-- && bit_no < len) {
     ret += (usx_read_bit(in, bit_no) ? 1 << count : 0);
     bit_no++;
   }
   return count < 0 ? ret : -1;
}

static int32_t usx_read_count(const char *in, int *bit_no_p, int len) {
  int idx = usx_step_code(in, len, bit_no_p, 4);
  if (idx == 99)
    return -1;
  if (*bit_no_p + usx_count_bit_lens[idx] - 1 >= len)
    return -1;
  int32_t count = usx_num_bits(in, len, *bit_no_p, usx_count_bit_lens[idx]) + (idx ? usx_count_adder[idx - 1] : 0);
  (*bit_no_p) += usx_count_bit_lens[idx];
  return count;
}

static int32_t usx_read_unicode(const char *in, int *bit_no_p, int len) {
  int idx = usx_step_code(in, len, bit_no_p, 5);
  if (idx == 99)
    return 0x7FFFFF00 + 99;
  if (idx == 5) {
    idx = usx_step_code(in, len, bit_no_p, 4);
    return 0x7FFFFF00 + idx;
  }
  if (idx >= 0) {
    int sign = (*bit_no_p < len ? usx_read_bit(in, *bit_no_p) : 0);
    (*bit_no_p)++;
    if (*bit_no_p + usx_uni_bit_len[idx] - 1 >= len)
      return 0x7FFFFF00 + 99;
    int32_t count = usx_num_bits(in, len, *bit_no_p, usx_uni_bit_len[idx]);
    count += usx_uni_adder[idx];
    (*bit_no_p) += usx_uni_bit_len[idx];
    return sign ? -count : count;
  }
  return 0;
}

#define USX_DEC_CHAR(out, olen, ol, c) do { \
  char *const obuf = (out); \
  const int oidx = (ol); \
  const int limit = (olen); \
  if (limit <= oidx) return limit + 1; \
  else if (oidx < 0) return 0; \
  else obuf[oidx] = (c); \
} while (0)

#define USX_DEC_CHARS(olen, exp) do { \
  const int newidx = (exp); \
  const int limit = (olen); \
  if (newidx > limit) return limit + 1; \
} while (0)

static int usx_write_utf8(char *out, int olen, int ol, int uni) {
  if (uni < (1 << 11)) {
    USX_DEC_CHAR(out, olen, ol++, (char)(0xC0 + (uni >> 6)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + (uni & 0x3F)));
  } else
  if (uni < (1 << 16)) {
    USX_DEC_CHAR(out, olen, ol++, (char)(0xE0 + (uni >> 12)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + ((uni >> 6) & 0x3F)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + (uni & 0x3F)));
  } else {
    USX_DEC_CHAR(out, olen, ol++, (char)(0xF0 + (uni >> 18)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + ((uni >> 12) & 0x3F)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + ((uni >> 6) & 0x3F)));
    USX_DEC_CHAR(out, olen, ol++, (char)(0x80 + (uni & 0x3F)));
  }
  return ol;
}

// Self-repeat only (upstream decodeRepeat with prev_lines == NULL).
static int usx_decode_repeat(const char *in, int len, char *out, int olen, int ol, int *bit_no) {
  int32_t dict_len = usx_read_count(in, bit_no, len) + USX_NICE_LEN;
  if (dict_len < USX_NICE_LEN)
    return -1;
  int32_t dist = usx_read_count(in, bit_no, len) + USX_NICE_LEN - 1;
  if (dist < USX_NICE_LEN - 1)
    return -1;
  const int32_t left = (int32_t)olen - (int32_t)ol;
  if (left <= 0) return olen + 1;
  if ((int32_t)ol - dist < 0)
    return -1;
  memmove(out + ol, out + ol - dist, (size_t)usx_min_of(left, dict_len));
  if (left < dict_len) return olen + 1;
  ol += dict_len;
  return ol;
}

static char usx_hexchar(int32_t nibble, int hex_type) {
  if (nibble >= 0 && nibble <= 9)
    return (char)('0' + nibble);
  else if (hex_type < USX_NIB_HEX_UPPER)
    return (char)('a' + nibble - 10);
  return (char)('A' + nibble - 10);
}

// Core decoder: upstream unishox2_decompress_lines() with the bounded-olen
// variant, default preset tables, and prev_lines == NULL.
static int usx_decompress_core(const char *in, int len, char *out, int olen,
                               const uint8_t usx_hcodes[], const uint8_t usx_hcode_lens[],
                               const char *usx_freq_seq[], const char *usx_templates[]) {
  int dstate;
  int bit_no;
  int h, v;
  uint8_t is_all_upper;

  int ol = 0;
  bit_no = USX_MAGIC_BIT_LEN; // ignore the magic bit
  dstate = h = USX_ALPHA;
  is_all_upper = 0;

  int prev_uni = 0;

  len <<= 3;
  while (bit_no < len) {
    int orig_bit_no = bit_no;
    if (dstate == USX_DELTA || h == USX_DELTA) {
      if (dstate != USX_DELTA)
        h = dstate;
      int32_t delta = usx_read_unicode(in, &bit_no, len);
      if ((delta >> 8) == 0x7FFFFF) {
        int spl_code_idx = (int)(delta & 0x000000FF);
        if (spl_code_idx == 99)
          break;
        switch (spl_code_idx) {
          case 0:
            USX_DEC_CHAR(out, olen, ol++, ' ');
            continue;
          case 1:
            h = usx_read_hcode(in, len, &bit_no, usx_hcodes, usx_hcode_lens);
            if (h == 99) {
              bit_no = len;
              continue;
            }
            if (h == USX_DELTA || h == USX_ALPHA) {
              dstate = h;
              continue;
            }
            if (h == USX_DICT) {
              int rpt_ret = usx_decode_repeat(in, len, out, olen, ol, &bit_no);
              if (rpt_ret < 0)
                return ol;
              USX_DEC_CHARS(olen, ol = rpt_ret);
              h = dstate;
              continue;
            }
            break;
          case 2:
            USX_DEC_CHAR(out, olen, ol++, ',');
            continue;
          case 3:
            USX_DEC_CHAR(out, olen, ol++, '.');
            continue;
          case 4:
            USX_DEC_CHAR(out, olen, ol++, 10);
            continue;
        }
      } else {
        prev_uni += delta;
        USX_DEC_CHARS(olen, ol = usx_write_utf8(out, olen, ol, prev_uni));
      }
      if (dstate == USX_DELTA && h == USX_DELTA)
        continue;
    } else
      h = dstate;
    char c = 0;
    uint8_t is_upper = is_all_upper;
    v = usx_read_vcode(in, len, &bit_no);
    if (v == 99 || h == 99) {
      bit_no = orig_bit_no;
      break;
    }
    if (v == 0 && h != USX_SYM) {
      if (bit_no >= len)
        break;
      if (h != USX_NUM || dstate != USX_DELTA) {
        h = usx_read_hcode(in, len, &bit_no, usx_hcodes, usx_hcode_lens);
        if (h == 99 || bit_no >= len) {
          bit_no = orig_bit_no;
          break;
        }
      }
      if (h == USX_ALPHA) {
         if (dstate == USX_ALPHA) {
           if (!usx_hcode_lens[USX_ALPHA] && USX_TERM_BYTE_PRESET_1 == (usx_read_8bit(in, len, bit_no - USX_SW_CODE_LEN) & (0xFF << (8 - (is_all_upper ? USX_TERM_BYTE_PRESET_1_LEN_UPPER : USX_TERM_BYTE_PRESET_1_LEN_LOWER)))))
             break; // Terminator for preset 1
           if (is_all_upper) {
             is_upper = is_all_upper = 0;
             continue;
           }
           v = usx_read_vcode(in, len, &bit_no);
           if (v == 99) {
             bit_no = orig_bit_no;
             break;
           }
           if (v == 0) {
              h = usx_read_hcode(in, len, &bit_no, usx_hcodes, usx_hcode_lens);
              if (h == 99) {
                bit_no = orig_bit_no;
                break;
              }
              if (h == USX_ALPHA) {
                 is_all_upper = 1;
                 continue;
              }
           }
           is_upper = 1;
         } else {
            dstate = USX_ALPHA;
            continue;
         }
      } else
      if (h == USX_DICT) {
        int rpt_ret = usx_decode_repeat(in, len, out, olen, ol, &bit_no);
        if (rpt_ret < 0)
          break;
        USX_DEC_CHARS(olen, ol = rpt_ret);
        continue;
      } else
      if (h == USX_DELTA) {
        continue;
      } else {
        if (h != USX_NUM || dstate != USX_DELTA)
          v = usx_read_vcode(in, len, &bit_no);
        if (v == 99) {
          bit_no = orig_bit_no;
          break;
        }
        if (h == USX_NUM && v == 0) {
          int idx = usx_step_code(in, len, &bit_no, 5);
          if (idx == 99)
            break;
          if (idx == 0) {
            idx = usx_step_code(in, len, &bit_no, 4);
            if (idx >= 5)
              break;
            int32_t rem = usx_read_count(in, &bit_no, len);
            if (rem < 0)
              break;
            if (usx_templates[idx] == NULL)
              break;
            size_t tlen = strlen(usx_templates[idx]);
            if (rem > (int32_t)tlen)
              break;
            rem = (int32_t)tlen - rem;
            int eof = 0;
            for (int j = 0; j < rem; j++) {
              char c_t = usx_templates[idx][j];
              if (c_t == 'f' || c_t == 'r' || c_t == 't' || c_t == 'o' || c_t == 'F') {
                  char nibble_len = (c_t == 'f' || c_t == 'F' ? 4 : (c_t == 'r' ? 3 : (c_t == 't' ? 2 : 1)));
                  const int32_t raw_char = usx_num_bits(in, len, bit_no, nibble_len);
                  if (raw_char < 0) {
                      eof = 1;
                      break;
                  }
                  USX_DEC_CHAR(out, olen, ol++, usx_hexchar((char)raw_char,
                      c_t == 'f' ? USX_NIB_HEX_LOWER : USX_NIB_HEX_UPPER));
                  bit_no += nibble_len;
              } else
                USX_DEC_CHAR(out, olen, ol++, c_t);
            }
            if (eof) break; // reach input eof
          } else
          if (idx == 5) {
            int32_t bin_count = usx_read_count(in, &bit_no, len);
            if (bin_count < 0)
              break;
            if (bin_count == 0) // invalid encoding
              break;
            do {
              const int32_t raw_char = usx_num_bits(in, len, bit_no, 8);
              if (raw_char < 0)
                  break;
              USX_DEC_CHAR(out, olen, ol++, (char)raw_char);
              bit_no += 8;
            } while (--bin_count);
            if (bin_count > 0) break; // reach input eof
          } else {
            int32_t nibble_count = 0;
            if (idx == 2 || idx == 4)
              nibble_count = 32;
            else {
              nibble_count = usx_read_count(in, &bit_no, len);
              if (nibble_count < 0)
                break;
              if (nibble_count == 0) // invalid encoding
                break;
            }
            do {
              int32_t nibble = usx_num_bits(in, len, bit_no, 4);
              if (nibble < 0)
                  break;
              USX_DEC_CHAR(out, olen, ol++, usx_hexchar(nibble, idx < 3 ? USX_NIB_HEX_LOWER : USX_NIB_HEX_UPPER));
              if ((idx == 2 || idx == 4) && (nibble_count == 25 || nibble_count == 21 || nibble_count == 17 || nibble_count == 13))
                USX_DEC_CHAR(out, olen, ol++, '-');
              bit_no += 4;
            } while (--nibble_count);
            if (nibble_count > 0) break; // reach input eof
          }
          if (dstate == USX_DELTA)
            h = USX_DELTA;
          continue;
        }
      }
    }
    if (is_upper && v == 1) {
      h = dstate = USX_DELTA; // continuous delta coding
      continue;
    }
    if (h < 3 && v < 28)
      c = (char)usx_sets[h][v];
    if (c >= 'a' && c <= 'z') {
      dstate = USX_ALPHA;
      if (is_upper)
        c -= 32;
    } else {
      if (c >= '0' && c <= '9') {
        dstate = USX_NUM;
      } else if (c == 0) {
        if (v == 8) {
          USX_DEC_CHAR(out, olen, ol++, '\r');
          USX_DEC_CHAR(out, olen, ol++, '\n');
        } else if (h == USX_NUM && v == 26) {
          int32_t count = usx_read_count(in, &bit_no, len);
          if (count < 0)
            break;
          count += 4;
          if (ol <= 0)
            return 0; // invalid encoding
          char rpt_c = out[ol - 1];
          while (count--)
            USX_DEC_CHAR(out, olen, ol++, rpt_c);
        } else if (h == USX_SYM && v > 24) {
          v -= 25;
          const int freqlen = (int)strlen(usx_freq_seq[v]);
          const int left = olen - ol;
          if (left <= 0) return olen + 1;
          memcpy(out + ol, usx_freq_seq[v], (size_t)usx_min_of(left, freqlen));
          if (left < freqlen) return olen + 1;
          ol += freqlen;
        } else if (h == USX_NUM && v > 22 && v < 26) {
          v -= (23 - 3);
          const int freqlen = (int)strlen(usx_freq_seq[v]);
          const int left = olen - ol;
          if (left <= 0) return olen + 1;
          memcpy(out + ol, usx_freq_seq[v], (size_t)usx_min_of(left, freqlen));
          if (left < freqlen) return olen + 1;
          ol += freqlen;
        } else
          break; // Terminator
        if (dstate == USX_DELTA)
          h = USX_DELTA;
        continue;
      }
    }
    if (dstate == USX_DELTA)
      h = USX_DELTA;
    USX_DEC_CHAR(out, olen, ol++, c);
  }

  return ol;
}

int lora_unishox_decompress(const uint8_t *in, int in_len, char *out, int out_cap) {
  if (!in || !out || in_len <= 0 || out_cap < 2)
    return -1;
  int olen = out_cap - 1; // reserve one byte for NUL
  int n = usx_decompress_core((const char *)in, in_len, out, olen,
                              usx_hcodes_dflt, usx_hcode_lens_dflt,
                              usx_freq_seq_dflt, usx_templates_dflt);
  if (n <= 0 || n > olen) {
    out[0] = '\0';
    return -1;
  }
  out[n] = '\0';
  return n;
}
