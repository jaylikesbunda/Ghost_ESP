// lora_unishox.h — decode-only Unishox2 for TEXT_MESSAGE_COMPRESSED_APP (port 7).
// Upstream: https://github.com/siara-cc/Unishox2 (Apache-2.0, see
// LICENSES/unishox2-Apache-2.0.txt). Decode path only; default preset
// (USX_PSET_DFLT), which is what Meshtastic's unishox2_compress_simple /
// unishox2_decompress_simple use on both ends. Static tables only, no
// malloc — safe on the radio RX task without PSRAM.

#ifndef LORA_UNISHOX_H
#define LORA_UNISHOX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Port number for compressed text (meshtastic PortNum TEXT_MESSAGE_COMPRESSED_APP).
#define LORA_UNISHOX_PORTNUM 7
// Decompressed-text ceiling: matches LORA_MESH_TEXT_MAX (160B chat payloads).
#define LORA_UNISHOX_TEXT_MAX 160

// Decompress one Unishox2 frame (default preset, 1 magic bit).
// in/in_len: compressed bytes (Data.payload of a port-7 message).
// out/out_cap: caller buffer; out_cap must be >= 2 (1 byte reserved for NUL).
// Returns decoded byte count (> 0, out NUL-terminated) on success,
// <= 0 on any failure (bad input, corrupt stream, or output overflow).
// Failure means the caller must keep the opaque-forward behavior.
int lora_unishox_decompress(const uint8_t *in, int in_len, char *out, int out_cap);

#ifdef __cplusplus
}
#endif

#endif // LORA_UNISHOX_H
