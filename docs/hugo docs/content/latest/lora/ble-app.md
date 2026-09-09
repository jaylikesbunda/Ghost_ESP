---
title: "LoRa BLE App Link"
description: "Pair the official Meshtastic app to GhostESP over BLE"
keywords: ["LoRa", "Meshtastic", "BLE", "phone app", "Ghost-XXXX", "MTU"]
weight: 30
---

GhostESP exposes the same BLE GATT service as stock Meshtastic nodes so the official app connects without mods.

## Advertising

- **Name:** `Ghost-XXXX` where `XXXX` is the last two bytes of the node ID (hex).
- **Service:** Meshtastic BLE service UUID; LongFast channel is the default (chash `0x08`).
- **Start:** `lora ble on` (or `lora start` if BLE bridge is set to auto). Check `lora ble status`.

## Pairing

1. Enable location/Bluetooth on your phone and open the official Meshtastic app.
2. Scan — select `Ghost-XXXX` when it appears.
3. Wait for configuration sync to complete. The app should then show the LongFast channel, node list, and chat.

## Text flow

- **App → mesh:** type in the app; GhostESP encrypts as AES-CTR with the channel PSK and transmits via SX1262.
- **Mesh → app:** received frames are forwarded once by packet ID. Reliable repeats are still acknowledged without creating duplicate messages.
- **Direct messages:** DMs use the recipient's NodeInfo public key. Public-channel access alone is not enough.
- Other nodes see GhostESP as a regular peer (NodeInfo, routing, ACKs all interop).

Messages received while the phone is disconnected are queued for the next connection. The newest eight mesh packets are retained.

## Drop and link semantics

- **Drop:** app disconnect, BLE timeout, or `lora ble off` tears down GATT and clears the session nonce. Reconnect triggers fresh nonce reads.
- **Link:** `lora ble status` reports `linked` vs `advertising`; `lora companion ble` keeps the bridge tied to BLE, while `lora companion wifi` keeps BLE free for scans.
- Only one phone link at a time. A second connect preempts the first.

## Quick troubleshooting

- **Key unavailable:** wait for NodeInfo, or run `lora nodeinfo <node-id>` and check `lora pkinfo <node-id>`.
- **Blank or old node name:** chat packets do not carry names. Request NodeInfo and let the app sync again.
- **Message remains pending:** reliable messages retry twice. LongFast retries are several seconds apart, so delivery is not immediate when an ACK is lost.
- **Connection has no data:** wait for configuration sync, then check `lora ble status` and `lora diag`.

See [Commands]({{< relref "commands.md" >}}) for transport switching.
