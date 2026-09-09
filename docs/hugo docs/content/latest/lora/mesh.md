---
title: "LoRa Mesh Details"
description: "Meshtastic wire format, channels, and flood routing on GhostESP"
keywords: ["LoRa", "Meshtastic", "wire format", "AES-CTR", "LongFast", "NodeInfo", "flood"]
weight: 40
---

GhostESP speaks the stock Meshtastic wire format so it interoperates with unmodified firmware.

## Wire format

Each LoRa frame is:

```text
[16 B header] [AES-CTR ciphertext: protobuf Data]
```

- **Header (16 B):** `from`, `to`, packet ID, flags, channel hash, next hop, and relay node.
- **Ciphertext:** AES-CTR with the channel PSK (LongFast default PSK `AQ==` → chash `0x08`) covering the serialized `Data` protobuf (portnum, payload, routing fields).
- **De-dup:** `from` + packet ID identifies a message. Repeats may still be acknowledged, but they are not shown twice in chat.

Direct messages use X25519 and authenticated AES-CCM instead of the channel key. Both nodes must first learn each other's public key from NodeInfo.

## Region and channel

- **Region slots:** each of the 24 region presets selects a Meshtastic frequency slot, bandwidth, spreading factor, coding rate, and power ceiling (e.g., US LongFast at 906.875 MHz / SF11 / BW250). GhostESP and stock nodes must agree; mismatch = no RX.
- **Sync word:** `0x442B` — Meshtastic private LoRa sync word (not `0x12`/`0x34` public). GhostESP sets this on SX1262 at `lora start`; stock nodes ignore frames with other sync words.
- **Default-key channel:** `LongFast` on chash `0x08`. Custom PSKs derive a different chash; both ends must match.

## Routing

- **Flood:** relays wait for a short SNR-weighted window. Hearing another copy can cancel an unnecessary transmission.
- **Destination:** a packet addressed to this node is delivered and acknowledged, not rebroadcast.
- **Reliable messages:** the sender makes up to three attempts. The delay is calculated from modem airtime; on LongFast it is normally several seconds.
- **Next hop:** successful ACKs can teach a preferred relay. If that route fails, the final retry falls back to normal flooding.
- **Duty guard:** regional guard throttles forwarded TX when the band duty budget is exhausted.

## NodeInfo

- **Interval:** every 3 hours per node (plus on boot and on explicit request). GhostESP sends its long/short name, HW model, and role after `lora start`.
- **Discovery:** first 30–60 seconds after `lora start` populates `lora nodes` as NodeInfos arrive. Nodes expire from the list only on long silence.
- **Diagnostics:** `lora nodes` and `lora diag` expose last NodeInfo timestamps, SNR/RSSI, and hop counts.
- **Capacity:** Heltec V3 keeps up to 200 nodes in RAM. Up to 32 important peer records are retained across reboot, prioritizing public keys and user flags.

See [Commands]({{< relref "commands.md" >}}) for tuning SF/BW and [Getting Started]({{< relref "getting-started.md" >}}) for first contact.
