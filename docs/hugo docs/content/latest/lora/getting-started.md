---
title: "LoRa Getting Started"
description: "First-run checklist for GhostESP LoRa Meshtastic"
keywords: ["LoRa", "Meshtastic", "SX1262", "getting started", "Heltec"]
weight: 10
---

Bring up the mesh in five steps. Assumes a Heltec V3/V3.2 or a correctly wired SX1262.

## 1. Enable the build flag

Set `CONFIG_HAS_LORA=y` (or select Heltec V3 / V3.2 board config which enables it). Rebuild and flash; LoRa is not available on non-SX1262 builds.

```text
idf.py menuconfig  # Component config -> GhostESP Features -> HAS_LORA
idf.py build flash monitor
```

## 2. Set your region

Pick the LongFast-legal preset for your country before transmitting:

```text
lora set region anz
```

Replace `anz` with `us915`, `eu868`, `eu433`, `jp`, etc. Region selects frequency, bandwidth, SF, coding rate, and power.

## 3. Start the radio

```text
lora start
```

Watch the console:

- `SX1262 ready` — SPI and DIO/RESET/BUSY pins responded.
- `chash 0x08` — LongFast default channel hash; confirms default PSK (`AQ==`). If you see a different chash, re-check `lora set region` and channel key.
- If you see `SX1262 not detected`, see [Hardware]({{< relref "hardware.md" >}}).

## 4. Wait for NodeInfo

GhostESP broadcasts its NodeInfo after start. Within 30–60 seconds you should see `NodeInfo` RX/TX logs and `lora nodes` start to populate. Stock nodes rebroadcast every ~3 hours.

```text
lora nodes
```

## 5. Test public chat

- **Ghost to Ghost:** on two GhostESP nodes on the same region/channel, run `lora chat hello` on each and confirm RX on the peer.
- **App to mesh:** pair the official app per [BLE App Link]({{< relref "ble-app.md" >}}), send a text, and verify it arrives via `lora chat` or the peer's console.

## Direct messages

DMs need the other node's current public key. After either node is wiped or factory-reset, exchange NodeInfo again:

```text
lora nodeinfo !e026f431
lora pkinfo !e026f431
lora dm !e026f431 hello
```

Replace the example ID with the destination shown by `lora nodes`. `pkinfo` must show a peer key before a DM can be sent. In the phone app, wait for the node to appear with its name and key before opening the direct-message conversation.

A public chat packet does not contain a node name or public key. If a node is blank, stale, or reports **Key unavailable**, send `lora nodeinfo <node>` and wait for the reply. Do not run `lora pki-regen` unless you intentionally want a new GhostESP identity key.

Next: [Commands]({{< relref "commands.md" >}}) for tuning SF/BW/TX and diagnostics.
