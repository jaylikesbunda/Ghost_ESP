---
title: "LoRa Meshtastic"
description: "LoRa Meshtastic chat on GhostESP with BLE app link and stock radio interop"
keywords: ["LoRa", "Meshtastic", "SX1262", "Heltec V3", "LongFast", "mesh"]
weight: 130
aliases:
  - "/lora/"
---

GhostESP implements a Meshtastic-compatible LoRa mesh on SX1262 radios. Chat with stock Meshtastic nodes, bridge the mesh to the official Meshtastic phone app over BLE, and use GhostESP as a field node or sniffer.

Stock radio interop is first-class: same sync word, channels, and wire encryption as Meshtastic firmware, so GhostESP joins existing meshes without flashing stock firmware.

## Hardware

- **Heltec V3 / V3.2** — SX1262 + ESP32-S3 with built-in OLED, pre-wired. Use `sdkconfig.heltecv3` (or the matching release build). Primary supported boards.
- **Elecrow CrowPanel Advance 2.4** — SX1262 wireless module. Use `sdkconfig.crowpanel_advance24`; the display stays on SPI2 and the radio uses SPI3.
- **Elecrow CrowPanel Advance 2.8** — SX1262 wireless module. Use `sdkconfig.crowpanel_advance28`; the display stays on SPI2 and the radio uses SPI3.
- **Elecrow CrowPanel Advance 4.3** — SX1262 Meshtastic wireless module. Use `sdkconfig.crowpanel_advance43` and set the rear switch to `01 (WM)`; SD/TF is unavailable in wireless-module mode.
- **Generic SX1262** — any ESP32 with an SX1262 via SPI through custom board wiring; configure pins in Kconfig. See [Hardware Support]({{< relref "../getting-started/supported-hardware.md" >}}) for build matrix.

See [Hardware]({{< relref "hardware.md" >}}) for Vext power sequencing and SPI/SD conflicts.

## Regions and channel

- **24 regions** — LongFast-legal presets such as `anz`, `us915`, `eu868`, `eu433`, `jp`, `kr`, and `in`. Each maps to the region's frequency slot and radio settings.
- **Default-key channel** — LongFast primary channel with the well-known default PSK (`AQ==` base64, hash `0x08`). GhostESP derives the same `chash` so stock nodes decrypt without rekeying.
- **Duty guard** — regional duty-cycle limiter throttles TX when airtime exceeds the legal window; `lora status` shows remaining budget.

## What you can do

- Chat on the mesh from the CLI (`lora chat`) or the official Meshtastic app over BLE.
- Send encrypted direct messages after the two nodes exchange NodeInfo public keys.
- On Heltec boards, the OLED shows live link, node, traffic, relay, signal, and error status. New messages briefly replace any selected animation with the sender and a three-line preview; press **PRG** to dismiss it.
- Track nodes, routing, and link health from NodeInfo and traceroute equivalents.
- Bridge to the companion app over Wi-Fi (`companion wifi`) or BLE (`companion ble`) when BLE radio sharing matters.

## Chatting on a screen

Open **LoRa → Messages** for public chat and direct conversations. Choose **New direct message** to pick a node. Select **Write** inside a conversation to open the keyboard.

- **Touch:** tap to open, drag to read, and select the Write or Back rows.
- **Encoder:** turn to move through messages and Back/Write; press to select.
- **Joystick:** up/down to move, centre to select, left to go back.
- **Keyboard:** arrows or Tab to move, Enter to select, Esc to go back; `n` opens Write inside a conversation.

DM status changes from Pending to Delivered or Failed when a routing result arrives. Sent means sent locally, not confirmed delivered. Recent messages use a compact 32-entry NVS-backed history: old entries expire as it fills, and normal reboot/reflash keeps them (an erase-flash/factory reset clears them). The built-in OLED uses its compact preview instead of this full chat screen.

## Quick links

- [Getting Started]({{< relref "getting-started.md" >}}) — first-run checklist from build flag to verified chat
- [Commands]({{< relref "commands.md" >}}) — full `lora` CLI reference
- [BLE App Link]({{< relref "ble-app.md" >}}) — pair the official Meshtastic app over Ghost-XXXX
- [Mesh Details]({{< relref "mesh.md" >}}) — wire format, sync word, flood, and NodeInfo
- [Hardware]({{< relref "hardware.md" >}}) — Vext power, SPI, and custom board Kconfig
