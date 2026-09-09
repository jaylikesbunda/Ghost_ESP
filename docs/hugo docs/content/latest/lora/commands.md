---
title: "LoRa Commands"
description: "CLI reference for GhostESP LoRa Meshtastic"
keywords: ["LoRa", "Meshtastic", "CLI", "commands", "SX1262"]
weight: 20
---

All LoRa commands are under the `lora` prefix. Run `lora help` or `help all` for the live list.

## Lifecycle

- **`lora status`** — show radio state, region, frequency, modem settings, TX power, companion mode, and last error.
- **`lora start`** — power Vext (Heltec), init SX1262, join LongFast channel, begin RX/TX.
- **`lora stop`** — stop RX, deinit radio, release Vext if held.

## Configuration

- **`lora set region <code>`** — set the legal region, such as `anz`, `us915`, `eu868`, `eu433`, or `jp`. Persists to NVS.
- **`lora set sf <5..12>`** — spreading factor. Higher SF = longer range, lower rate.
- **`lora set bw <kHz>`** — bandwidth (`125`, `250`, or `500`). Must match peers.
- **`lora set tx <dBm>`** — TX power, clamped to the region limit.
- **`lora set owner <long> <short>`** — set the advertised node names. Keep the long name to 24 bytes and the short name to 4 bytes, then send NodeInfo again.
- **`lora set companion <wifi|ble>`** — transport for the GhostESP companion app bridge.

Changes take effect on next `lora start` (or immediately if already running, where supported).

## Messaging and nodes

- **`lora chat <text>`** — broadcast a text message on the LongFast channel (default-key encrypted).
- **`lora dm <node> <text>`** — send an authenticated PKI direct message. The peer key must already be known.
- **`lora nodes`** — list known nodes: ID, short/long name, last heard, SNR/RSSI, hops.
- **`lora nodeinfo [node]`** — advertise our name/key and request a NodeInfo reply. Use this after either node is wiped.
- **`lora pkinfo <node>`** — show the stored peer key and our key. The command is `pkinfo`, without a space.
- **`lora diag`** — IRQ counters, TX queue, app FIFO, and last RX metadata.
- **`lora cad`** — run five channel-activity trials with instantaneous RSSI.
- **`lora reg <hex-address> [count]`** — read one to eight consecutive SX1262 registers after a successful start; for example, `lora reg 0740 2` should report the SX1262-encoded Meshtastic sync word `24 B4`.

`lora pubkey`, `lora pkselftest`, `lora pkitest`, `lora pktry`, and `lora pki-regen` are diagnostic commands. Avoid `pki-regen` during normal troubleshooting because it replaces the identity key that peers have stored.

## BLE bridge

- **`lora ble on`** — start the Meshtastic BLE GATT service (`Ghost-XXXX` adv).
- **`lora ble off`** — stop the BLE service and advertising.
- **`lora ble status`** — show BLE link state, MTU, connected phone, and nonce sync.

## Companion and setup

- **`lora app`** — print companion app pairing hint (advertised name + channel hash).
- **`lora companion wifi`** — bridge mesh to companion app over Wi-Fi (keeps BLE free for scans).
- **`lora companion ble`** — bridge over BLE (shares radio; prefer `wifi` when scanning).
- **`lora setup`** — interactive wizard: region → channel → companion transport → start.

See [Getting Started]({{< relref "getting-started.md" >}}) for first-run order and [BLE App Link]({{< relref "ble-app.md" >}}) for phone pairing.
