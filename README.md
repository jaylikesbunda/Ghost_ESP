<img width="800" alt="ghostesp_logo_white_transparent_2x_shine" src="https://github.com/user-attachments/assets/36005ffd-9cfc-433e-a306-1606feb18107" />

> **The ESP-IDF-native wireless security platform for ESP32.**
> Deep Wi-Fi and BLE assessment, research-grade capture and export, and a real app ecosystem. Built directly on Espressif's ESP-IDF rather than through the Arduino core, so new silicon and radio features land first and there is no abstraction layer between GhostESP and the hardware.

[![Version](https://img.shields.io/badge/version-2.1.2-7c5cff?style=flat-square)](https://github.com/GhostESP-Revival/GhostESP)
[![License](https://img.shields.io/badge/license-GPL--3.0-blue?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-orange?style=flat-square)](https://docs.espressif.com/projects/esp-idf/)
[![Discord](https://img.shields.io/discord/5cyNmUMgwh?style=flat-square&label=Discord&color=5865F2)](https://discord.gg/5cyNmUMgwh)
[![Boards](https://img.shields.io/badge/board%20targets-61-2ea043?style=flat-square)](#supported-boards)

**⭐️ Enjoying GhostESP? Please give the repo a star. It helps a lot.**

---

## What's New

v2.0 rebuilt the UI, added a native app ecosystem, and expanded the radio workflows. v2.1 (Revival) adds on-device OTA with rollback protection, a second NFC backend (ST25R3916), a Cloud Store for apps and scripts and asset packs, and GhostScript, a sandboxed Lua runtime.

<img width="320" height="170" alt="app-gallery2" src="https://github.com/user-attachments/assets/f7bb96ed-db0c-4777-a721-ded2d397b167" /> <img width="320" height="170" alt="airspace-monitor" src="https://github.com/user-attachments/assets/e049dfc8-3888-42ec-9fd1-6be62fcec114" />

Full history in [`CHANGELOG.md`](CHANGELOG.md).

---

## Get Started

| **Flash your device** | **Community and support** | **Learn more** |
| --- | --- | --- |
| [ghostesp.net/flasher](https://ghostesp.net/flasher) | [Discord](https://discord.gg/5cyNmUMgwh) | [Documentation](https://docs.ghostesp.net) · [Website](https://ghostesp.net) |

---

## Why GhostESP

GhostESP is a platform, not a bag of tools. Five things set it apart:

- **Built on ESP-IDF, not Arduino.** Unlike Marauder and Bruce, GhostESP works against Espressif's SDK directly. It gets new chips and radio features first (802.15.4, Zigbee, 5 GHz on the C5) and lower-level control of the radios, memory, and partitions. Arduino-based firmware can reach the same APIs, just not as directly.
- **Live capture and on-device analysis.** Stream traffic into Wireshark in real time over USB, and browse, inspect, and convert captures (PCAP, hc22000, WiGLE, 802.15.4) on the device itself.
- **GhostLink.** Pair two ESP32s so one runs the other's radio, keyboard, and display, with BLE bridging to the Android app, split-channel wardriving, and peer OTA.
- **Native apps.** SD apps with their own permissions and storage, built with `gbt` and a plugin SDK, installed from the Cloud Store, plus a Lua runtime for scripts. Examples: Device Inspector, ESP32Finder, a Doom port, a QR generator.
- **One command set, many front ends.** Drive GhostESP from the on-device UI, serial CLI, WebUI, Flipper Zero app, Android app, or a GhostLink-connected device, all against the same commands.

---

## Features

<details>
<summary><strong>WiFi Features</strong></summary>

- Evil Portal (with custom HTML from buffer via serial)
- Deauth / disassoc attacks
- Channel switch attack
- GTK abuse / client isolation testing
- EAPOL logoff attack
- Karma (with custom SSID lists and custom portal chaining)
- Beacon spam (single/list/random/Rickroll)
- AP scan / STA scan / scanall
- Multi-select APs and stations
- Probe request listening (with auto-spawned Evil Twin)
- Handshake + PMKID capture
- WiFi capture to SD (PCAP) with on-device PCAP browser + hc22000 export
- USB dongle mode for Wireshark (extcap stream)
- DHCP starvation
- ARP / port / SSH / local IP scanners
- mDNS discovery / NetBIOS scan / HTTP banner scan / SNMP probe (with per-host and per-subnet variants)
- WiFi OUI vendor lookup
- WPA3/SAE attacks (flood + compliance checker)
- Wardriving exports (WiFi/BLE/GPS) + sweep CSV (WiFi/BLE/GPS/802.15.4)
- Split-channel wardriving helper via GhostLink
- RSSI tracking (AP/station) with live RSSI meter view
- Drone detection / spoofing
- PineAP detection
- Flock / surveillance detector
- WPS detection
- Pwnagotchi-style automated capture mode (Capture PWN)
- Channel congestion analysis
- WiFi Airspace Monitor (real-time packet/threat insights, fast channel hopping, suspect device cards, packets/sec sparkline)
- DNS Sinkhole (blocklist-based NXDOMAIN blocking with built-in blocklist downloads)
- Web UI + filesystem + remote command relay
</details>

<details>
<summary><strong>BLE Features</strong></summary>

- BLE scan modes (general, AirTag, Flipper, raw)
- BLE advertisement scan with OUI prefix/vendor filtering + RGB match pulses
- BLE spam modes (Apple, Microsoft, Samsung, Google, Random)
- AirTag scan / spoof / select
- BLE packet capture
- BLE stream to Wireshark
- Flipper finder + RSSI
- GATT/service scan + per-device enumeration + device tracking (live RSSI meter)
- BLE wardriving
- BLE skimmer detection
- Drone / OpenDroneID scan / list / track / spoof
- Aerial (drone) detector with threat classification
- GhostLink BLE bridge to the Android companion app
</details>

<details>
<summary><strong>USB Features</strong></summary>

- USB keyboard host mode (ESP32-S3 builds)
- Remote keyboard control over GhostLink
- BadUSB script runner
- BadUSB identity options (VID/PID/manufacturer/product/layout/randomize)
- BadUSB trackpad (touchpad-style cursor control)
- BadUSB mouse jiggler
- BadUSB `type_char` CLI for typing individual ASCII characters
- USB HID keyboard output mode (forward on-device keystrokes over USB)
- Dedicated WebUI BadUSB page
</details>

<details>
<summary><strong>IR Features</strong></summary>

- IR TX/RX on supported boards
- IR learn mode
- IR easy learn mode
- Flipper `.ir` file support
- Universal library transmit
- IR CLI tools
- IR dazzler (38 kHz high duty)
</details>

<details>
<summary><strong>NFC Features</strong></summary>

- PN532 NTAG/MIFARE Classic support
- ST25R3916 NFC backend (EMV payment-card reads, DESFire application and file trees, PicoPass and iCLASS)
- Flipper `.nfc` import/export
- MIFARE Classic dictionary attack (default + user dictionary + session key reuse / sector sweep)
- Full embedded MIFARE Classic dictionary
- MIFARE Classic hardnested recovery
- Flipper NFC parser set (transit, parking, access, amusement, loyalty): BIP, Clipper, CharlieCard, Troika, Plantain, Zolotaya Korona, Ventra, WashCity, Social Moscow, Sonicare, Saflok, Gallagher, Disney Infinity, Skylanders, Aime, Hi, HWorld, Two Cities, Umarsh, Microel, MIZIP, MetroMoney, Kazan, SmartRider, TRT, and more
- MIFARE Desfire detection
- Chameleon Ultra support (CLI + UI + BLE control)
- Chameleon Ultra HF/LF RFID scan + reader controls
</details>

<details>
<summary><strong>SubGHz Features</strong></summary>

- Signal scanning across 64 channels
- Frequency analyzer with waterfall display
- Signal capture and decoding
- 20+ protocol decoders based on Flipper Unleashed/xMasterX
- Signal transmission and replay
- Saved signals as `.sub` files
- Flipper SubGhz Key File format compatibility
- CC1101 hardware support
- Frequency bands: 315, 390, 433.92, 868.35, 915 MHz
- Full CLI support
- SubGHz remote radio support via GhostLink
</details>

<details>
<summary><strong>Audio Features</strong></summary>

- I2S DAC audio player (MP3 playback with headphone detection and volume control)
- Audio receiver (TLV320DAC I2S)
- Music visualizer (RGB LED audio visualization)
- Microphone spectrum / MIC visualizer (microphone-driven RGB LED effects with multiple modes, color modes, sensitivity, smoothing, and contrast)
</details>

<details>
<summary><strong>Display & Sensor Features</strong></summary>

- Full LVGL graphical UI with carousel, grid, and list layouts
- Custom asset packs loaded from SD (icons, colors, backgrounds, themes)
- 21 color themes
- On-screen splash/boot animation with progress bar
- Toast notification system
- Persistent status bar with level badge
- Touch drag scroll + tap-to-wake
- Configurable screen timeout, brightness, and orientation
- Idle animations (Game of Life, Ghost, Starfield, HUD, Matrix, Flying Ghosts, Spiral, Falling Leaves, Bouncing Text)
- On-screen Clock
- Compass screen (magnetometer)
- Accelerometer screen (G-force, tilt, orientation, shake, speed)
- ENV-III sensor screen (temperature, humidity, pressure, dew point, altitude)
- PIN Lock screen with auto-lock (overlay mode keeps captures running while locked)
- Trackpad / cursor control
- Setup wizard with Home WiFi configuration
- Accessibility settings (font size, high contrast, reduced motion, input repeat speed, epilepsy-safe mode)
- Terminal font size control
- Rave mode (display builds)
- DRV2605 haptic feedback (S3TWatch)
</details>

<details>
<summary><strong>Apps & Extensibility</strong></summary>

- Apps Gallery (central launcher for native SD apps, with categorical submenus)
- Native SD app system (load, list, inspect, launch, stop, reset apps with permissions and scoped storage)
- Ghost Build Tool (`gbt`) for scaffolding, building, and packaging apps and firmware
- Plugin/app SDK and example apps (Device Inspector, ESP32Finder)
- Cloud Store (browse and install apps, scripts, and asset packs on-device)
- GhostScript (sandboxed Lua 5.4 scripting runtime for scripts from the SD card)
- Ghostchi virtual pet companion (50-level XP system, 27 XP sources, passive/aggressive modes, companion lockscreen, global mood, level-up toasts, status-bar badge)
- SD Browser (file/folder browsing, rename, delete, copy/move, text file preview)
- On-device PCAP browser with hc22000 export
- On-device Info screen (device, runtime, build, credits)
- Reusable confirmation popups for dangerous UI actions
</details>

<details>
<summary><strong>Additional Features</strong></summary>

- GhostLink (dual-device command and display interface) with remote radio support and keyboard relay
- Setup wizard (display builds)
- Wired + web screen mirroring
- Ethernet mode (W5500) + full toolset: fingerprint scan, port scan, ping sweep, ARP scan, ARP poisoning, MITM, HTTP, DNS, NTP, trace route, MAC tools, statistics
- TLS SNI / HTTP / FTP credential capture over Ethernet
- DIAL / Chromecast V2 support
- GPS integration (`gpsinfo`) with WiGLE manual upload, runtime baud config, and `wdstream` companion streaming
- Network printer output (`powerprinter`, PJL)
- RGB LED modes (Normal, Rainbow, Stealth, Knight Rider, MIC Visualizer, custom colors) with neopixel brightness control
- Timezone configuration (`timezone`) + NTP time set
- Camera motion detection with SD card snapshot capture and Discord webhook alerts (XIAO S3 Sense)
- Live MJPEG camera stream (`/camera`)
- NRF24 spectrum analyzer with passive 2.4 GHz jamming detection
- Zigbee / 802.15.4 packet capture + sweep CSV (ESP32-C5/C6)
- 802.15.4 / Zigbee channel capture
- Battery monitoring / fuel gauge support
- Sensor / RTC hardware support (PCF8563)
- M5 Cardputer / Cardputer ADV keyboard support
- Android companion app
- On-device CH422G / ST7262 / AXS15231B / APX2102 display driver support
- Light-sleep idle + frequency scaling + Wi-Fi power saving
- Reduced-motion animations
- SD config backup / restore
- On-device OTA and SD firmware update with verification and rollback protection
</details>

---

## Supported ESP32 Variants

- ESP32-Wroom · ESP32-S2 · ESP32-C3 · ESP32-S3 · ESP32-C5 · ESP32-C6 · ESP32-P4

> **Note:** Feature availability varies by chip. S2 lacks Bluetooth hardware. C5 has 5 GHz and 802.15.4 or Zigbee support. P4 uses an external ESP32-C6 for Wi-Fi and Bluetooth via ESP-Hosted.

---

## Supported Boards

61 board targets build in CI ([`.github/workflows/compile_all.yml`](.github/workflows/compile_all.yml)) from 60 configs in [`configs/`](configs/). Awok V5 shares the generic ESP32-S2 config.

<details>
<summary><strong>Board feature matrix (click to expand)</strong></summary>

| Board | Bluetooth | NFC (PN532) | NFC (Chameleon) | IR TX | IR RX | GPS Default | Keyboard | Display | SD | OTA | Native SD Apps |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ESP32-Wroom DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-S2 DevKitC | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-S3 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C3 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C5 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| ESP32-C6 DevKitC | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Awok V5 | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| GhostBoard | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| Marauder v4 | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✓ | ✗ |
| Marauder v6 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✗ | ✗ | ✗ |
| AWOK Mini | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✗ | ✗ | ✓ |
| Cardputer | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | Full | ✓ | ✓ | ✗ |
| Heltec V3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Status | ✓ | ✗ | ✗ |
| CYD2 USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 Micro USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 Dual USB | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 USB 2.4" | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD2 USB 2.4" (C variant) | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| CYD 2432S028R | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✗ |
| Waveshare 7" Touch | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| Crowtech 7" | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✓ |
| CrowPanel Advance 7" (S3, TFCard mode) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel 4.2" E-paper (400×300) | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | E-paper | ✓ | ✗ | ✗ |
| CrowPanel 5.79" E-paper (792×272) | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | E-paper | ✓ | ✗ | ✗ |
| CrowPanel Advance 2.4" (S3, 320×240) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel Advance 2.8" (S3, 320×240) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel Advance 3.5" (S3, 480×320) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel Advance 4.3" (S3, 800×480) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel Advance 5" (S3, 800×480) | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| CrowPanel Advanced P4 7/9/10.1" (v1.2+) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✓ |
| CrowPanel Advanced P4 7/9/10.1" (v1.1) | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✓ |
| CrowPanel Advanced P4 5" RGB | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✓ |
| Sunton 7" | ✓ | ✗ | ✓* | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| JC3248W535EN | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| Flipper JCMK GPS | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| T-Deck | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✓ | Full | ✓ | ✗ | ✓ |
| T-Embed CC1101 | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| GhostLink P1 Core | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✗ | ✓ |
| GhostLink P1 Peer | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| T-Dongle-S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✗ |
| T-Dongle-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| S3TWatch | ✓ | ✗ | ✓ | ✓ | ✗ | ✗ | ✗ | Full | has 4MB vfs partition | ✓ | ✗ |
| T-Display S3 Touch | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✗ |
| JCMK Devboard Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| Minion | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Lolin S3 Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Cardputer ADV | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✓ | Full | ✓ | ✓ | ✗ |
| Poltergeist | ✓ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | Status | ✓ | ✗ | ✗ |
| Banshee (C5 display MCU) | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✗ | Full + Status | ✓ | ✓ | ✓ |
| Banshee (S3 main) | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✗ | ✓ | ✗ |
| Febris Pro | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✗ | ✗ | ✗ |
| ACE C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | — | ✓ | ✗ | ✗ |
| NM-CYD-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | Full | ✓ | ✓ | ✓ |
| ACE S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Seeed XIAO ESP32-S3 Sense | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✓ | ✗ |
| Seeed XIAO ESP32-S3 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✓ | ✗ |
| Seeed XIAO ESP32-C5 | ✓ | ✗ | ✓ | ✗ | ✗ | ✗ | ✗ | — | ✓ | ✗ | ✗ |
| Marauder v8 | ✓ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✓ |
| Pancake C5 | ✓ | ✗ | ✗ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✓ |
| M5Stack CoreS3-SE | ✓ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | Full | ✓ | ✗ | ✓ |
| M5Stack AtomS3R | ✓ | ✗ | ✓ | ✓ | ✗ | ✓ | ✗ | Full | has 1MB vfs partition | ✗ | ✗ |

`*` — the checked-in config for this board predates a Kconfig option (`NFC_CHAMELEON`) that defaults on for BLE-capable boards; no board-specific override is present, so this reflects the Kconfig default rather than an explicit setting in the file. Most unstarred BLE-capable boards set the symbol explicitly, but some generic configs may also rely on the Kconfig default.

**M5Stack Grove ports:** the M5Stack CoreS3-SE and AtomS3R configs expose their HY2.0-4P Grove connectors on I2C port 1 (`PORT.A`: SDA=G2, SCL=G1; the CoreS3-SE also has `PORT.B` G8/G9 and `PORT.C` G17/G18). Plug an ST25R3916 NFC module (I2C, 0x50) and/or an M5Stack ENV III unit (SHT30 0x44 + QMP6988 0x70) into Grove `PORT.A` and open the NFC or ENV III app — both devices share the same bus.

**Display:** `Full` = LVGL graphical UI. `Status` = secondary small status display only (shares the IO-expander I2C bus), no full UI. `—` = headless, no display.

**SD:** most boards use SPI-mode SD. JC3248W535EN and T-Dongle-S3 use SDMMC 1-bit mode instead; the SDMMC bus option exists in Kconfig for other boards.

**NFC (Chameleon):** Chameleon Ultra support rides over BLE, so it's on by default for any BLE-capable board and off where BLE is unavailable (ESP32-S2 boards) or explicitly disabled (Marauder v8, Pancake C5).

**Native SD Apps:** at compile time the feature depends only on `CONFIG_SPIRAM` (`main/Kconfig.projbuild:1410`). At runtime the app gallery checks `MALLOC_CAP_SPIRAM` and renders into the full LVGL screen, so a display is required for the UI to be usable. That leaves it enabled on: AWOK Mini, Waveshare/Crowtech/Sunton 7″, CrowPanel Advance 2.4/2.8/3.5/4.3/5″, CrowPanel Advanced P4 5″/7/9/10.1″ (v1.1/v1.2+), JC3248W535EN, T-Deck, T-Embed CC1101, GhostLink P1 Core, T-Dongle-C5, NM-CYD-C5, M5Stack CoreS3-SE, Banshee (C5), and Marauder v8/Pancake C5. Boards with a screen but no PSRAM (Cardputer, Cardputer ADV, the CYD2 family, S3TWatch, T-Dongle-S3, M5Stack AtomS3R, etc.) don't get it.

**Banshee** ships as two configs: the S3 main board (headless) and the C5 module that drives its display and status LED, paired over GhostLink.

</details>

---

## ESP32 Firmware Comparison

Marauder is the focused, hardware-first Wi-Fi and BLE toolset that much of this space grew out of, and GhostESP credits it directly (see [Credits](#credits)). GhostESP covers the same Wi-Fi and BLE ground and adds the capture-to-analysis pipeline, multi-device control through GhostLink, and the native app ecosystem described above, on an ESP-IDF base rather than Arduino.

The table below compares GhostESP against other broad-scope firmware. It is based on GhostESP's feature set and the publicly available source for each listed project. It is not a complete feature list for every firmware. HaleHound and nyanBOX are compared against the latest public source available to us. If newer releases are closed source, this table cannot be independently verified against those builds.

<details>
<summary><strong>View comparison table</strong></summary>

| Feature | GhostESP | Bruce | HaleHound | nyanBOX |
| --- | --- | --- | --- | --- |
| Current source available for audit | [x] | [x] | Limited / older public source | Limited / older public source |
| ESP-IDF-native architecture | [x] |  |  |  |
| Arduino / PlatformIO architecture |  | [x] | [x] | [x] |
| Supported board targets | 61 CI targets | 42+ | 4 | 1 |
| Full LVGL graphical UI | [x] |  |  |  |
| Web dashboard / REST control | [x] | [x] |  |  |
| Captive portal web server | [x] | [x] | [x] | [x] |
| AP / station WiFi scanning | [x] | [x] | [x] | [x] |
| Deauth / disassoc testing | [x] | [x] | [x] | [x] |
| Beacon spam | [x] | [x] | [x] | [x] |
| Karma / probe response attack | [x] | [x] | [x] |  |
| Handshake / EAPOL capture | [x] | [x] | [x] |  |
| On-device PCAP browser / hc22000 export | [x] |  |  |  |
| PMKID capture / export | [x] |  | [x] |  |
| Live Wireshark USB streaming | [x] |  |  |  |
| SAE flood / WPA3-specific testing | [x] |  |  |  |
| WPA3 compliance checker | [x] |  |  |  |
| EAPOL logoff attack | [x] |  |  |  |
| Channel switch attack | [x] |  |  |  |
| GTK abuse / client isolation testing | [x] |  |  |  |
| DHCP starvation | [x] | [x] |  |  |
| ARP / port / SSH scanners | [x] | [x] |  |  |
| mDNS discovery | [x] |  |  |  |
| NetBIOS scanner | [x] |  |  |  |
| HTTP banner scanner | [x] |  |  |  |
| SNMP probe | [x] |  |  |  |
| WiFi OUI vendor lookup | [x] | [x] | [x] |  |
| PineAP / Evil Twin detection | [x] |  |  | [x] |
| WPS detection / reporting | [x] | [x] |  |  |
| Pwnagotchi-style automated capture mode | [x] | [x] |  |  |
| Pwnagotchi detector / spam |  | [x] |  | [x] |
| Channel congestion analysis | [x] |  |  | [x] |
| Live WiFi packet monitor / visualizer | [x] |  | [x] | [x] |
| WiFi Airspace Monitor | [x] |  |  |  |
| DNS sinkhole / blocklist NXDOMAIN | [x] |  |  |  |
| GPS WiFi wardriving | [x] | [x] | [x] |  |
| BLE wardriving | [x] | [x] | [x] |  |
| WiGLE upload integration | [x] | [x] |  |  |
| 802.15.4 capture and PCAP export | [x] |  |  |  |
| GhostLink dual-ESP control | [x] |  |  |  |
| GhostLink BLE bridge to Android | [x] |  |  |  |
| Split-channel wardriving helper | [x] |  |  |  |
| GhostLink remote radio support | [x] |  |  |  |
| Drone / OpenDroneID detect | [x] |  |  | [x] |
| Drone / OpenDroneID spoof | [x] |  |  | [x] |
| BLE scanning | [x] | [x] | [x] | [x] |
| Raw BLE scanner | [x] |  |  |  |
| BLE spam modes | [x] | [x] | [x] | [x] |
| AirTag scan / spoof | [x] | [x] | [x] | [x] |
| BLE tracker detection tools | [x] |  | [x] | [x] |
| Flipper Zero finder | [x] |  |  | [x] |
| GATT / service enumeration | [x] |  | [x] |  |
| BLE device tracking by RSSI | [x] |  |  |  |
| BLE stream to Wireshark | [x] |  |  |  |
| BLE skimmer detection | [x] |  |  | [x] |
| FastPair / pairing exploit research |  | [x] | [x] | [x] |
| BLE HID injection / DuckyScript over BLE |  | [x] |  |  |
| BLE keyboard mode |  | [x] |  |  |
| BLE GATT honeypot / cloned peripheral |  |  | [x] | [x] |
| BLE vulnerability profiling |  | [x] |  |  |
| Flock / surveillance detector | [x] |  | [x] | [x] |
| PN532 NFC support | [x] | [x] | [x] |  |
| ST25R3916 NFC support | [x] | [x] |  |  |
| Chameleon Ultra support | [x] | [x] |  |  |
| Chameleon Ultra BLE control | [x] | [x] |  |  |
| Flipper `.nfc` import/export | [x] |  |  |  |
| Flipper NFC parser collection | [x] |  |  |  |
| MIFARE Classic default-key attack | [x] | [x] | [x] |  |
| MIFARE Classic embedded dictionary | [x] |  |  |  |
| MIFARE Classic user dictionary file | [x] | [x] |  |  |
| MIFARE Classic session key reuse / sector sweep | [x] |  |  |  |
| MIFARE Classic hardnested recovery | [x] |  |  |  |
| PicoPass / iCLASS reading | [x] |  |  |  |
| MIFARE DESFire application / file tree reads | [x] |  |  |  |
| EMV / payment card reader | [x] | [x] |  |  |
| BadUSB / DuckyScript | [x] | [x] |  |  |
| USB keyboard host mode | [x] |  |  |  |
| USB HID keyboard output mode | [x] | [x] |  |  |
| Remote keyboard over dual-device link | [x] |  |  |  |
| BadUSB VID/PID identity options | [x] | [x] |  |  |
| BadUSB mouse jiggler / trackpad | [x] |  |  |  |
| IR learn / capture / replay | [x] | [x] |  |  |
| Flipper `.ir` file support | [x] | [x] |  |  |
| Universal IR library transmit | [x] | [x] |  |  |
| CC1101 SubGHz scan / replay | [x] | [x] | [x] |  |
| CC1101 waterfall spectrum analyzer | [x] | [x] | [x] |  |
| Flipper `.sub` read/write support | [x] | [x] | [x] | [x] |
| SubGHz protocol decoders | [x] | [x] | [x] |  |
| NRF24 spectrum analyzer | [x] | [x] | [x] | [x] |
| NRF24 MouseJack |  | [x] | [x] |  |
| Passive jamming detection | [x] |  | [x] |  |
| Active RF jamming shipped | Not shipped | [x] | [x] | [x] |
| Zigbee / 802.15.4 packet capture | [x] |  |  |  |
| Ethernet W5500 support | [x] | [x] |  |  |
| Ethernet ARP poisoning / MITM tools | [x] | [x] |  |  |
| Ethernet fingerprint / port / ping tools | [x] |  |  |  |
| Ethernet DNS / NTP / HTTP / trace tools | [x] |  |  |  |
| TLS SNI / HTTP / FTP credential capture over Ethernet | [x] |  |  |  |
| Camera streaming / motion detection | [x] |  |  |  |
| Motion alerts with webhook support | [x] |  |  |  |
| Network printer / PJL output | [x] |  |  |  |
| DIAL / Chromecast testing | [x] |  |  |  |
| On-device setup wizard | [x] |  |  |  |
| PIN / password lock | [x] |  | [x] | [x] |
| On-device OTA / SD firmware update | [x] |  | [x] |  |
| Firmware verification / rollback protection | [x] |  |  |  |
| GhostLink peer firmware update | [x] |  |  |  |
| Wired screen mirroring | [x] |  |  | [x] |
| Web screen mirroring | [x] | [x] |  |  |
| SD config backup / restore | [x] |  |  |  |
| SD file manager / browser | [x] | [x] | [x] |  |
| Native SD app/plugin system | [x] |  |  |  |
| Native app SDK / build tooling | [x] |  |  |  |
| Sandboxed on-device scripting runtime | Lua 5.4 | JavaScript |  |  |
| Cloud app / script / asset store | [x] |  |  |  |
| Apps gallery / launcher | [x] |  |  |  |
| Ghostchi / virtual pet | [x] | [x] |  |  |
| Audio player | [x] | [x] |  |  |
| Microphone spectrum / visualizer | [x] | [x] |  |  |
| RGB LED visualizer modes | [x] | [x] |  |  |
| Clock / RTC screen | [x] | [x] |  |  |
| Compass screen | [x] |  |  |  |
| Accelerometer screen | [x] |  |  |  |
| ENV-III temperature / humidity / pressure | [x] |  |  |  |
| Battery monitoring / fuel gauge support | [x] | [x] | [x] |  |
| Sensor / RTC hardware support | [x] | [x] |  |  |
| M5 Cardputer keyboard support | [x] | [x] |  |  |
| Android companion app | [x] |  |  |  |
| Accessibility modes / reduced motion | [x] |  | [x] |  |
| Custom theme / UI palette system | [x] | [x] | [x] |  |
| Custom SD asset packs | [x] |  |  |  |
| LoRa support |  | [x] |  |  |
| FM radio support |  | [x] |  |  |

> GhostESP does not ship active jamming features. Distribution, promotion, sale, and use of jamming devices or firmware is illegal in many jurisdictions.

</details>

---

## Credits

Special thanks to:

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/justcallmekoko">
        <img src="https://github.com/justcallmekoko.png" width="80" height="80" style="border-radius: 50%;" alt="JustCallMeKoKo"/><br/>
        <b>JustCallMeKoKo</b>
      </a><br/>
      <sub>ESP32Marauder foundational development</sub>
    </td>
    <td align="center">
      <a href="https://github.com/thibauts">
        <img src="https://github.com/thibauts.png" width="80" height="80" style="border-radius: 50%;" alt="thibauts"/><br/>
        <b>thibauts</b>
      </a><br/>
      <sub>CastV2 protocol insights</sub>
    </td>
    <td align="center">
      <a href="https://github.com/MarcoLucidi01">
        <img src="https://github.com/MarcoLucidi01.png" width="80" height="80" style="border-radius: 50%;" alt="MarcoLucidi01"/><br/>
        <b>MarcoLucidi01</b>
      </a><br/>
      <sub>DIAL protocol integration</sub>
    </td>
    <td align="center">
      <a href="https://github.com/SpacehuhnTech">
        <img src="https://github.com/SpacehuhnTech.png" width="80" height="80" style="border-radius: 50%;" alt="SpacehuhnTech"/><br/>
        <b>SpacehuhnTech</b>
      </a><br/>
      <sub>Reference deauthentication code</sub>
    </td>
    <td align="center">
      <a href="https://github.com/Spooks4576">
        <img src="https://github.com/Spooks4576.png" width="80" height="80" style="border-radius: 50%;" alt="Spooks4576"/><br/>
        <b>Spooks4576</b>
      </a><br/>
      <sub>Original GhostESP Developer</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/tototo31">
        <img src="https://github.com/tototo31.png" width="80" height="80" style="border-radius: 50%;" alt="Tototo31"/><br/>
        <b>Tototo31</b>
      </a><br/>
      <sub>Large contributions to the project</sub>
    </td>
    <td align="center">
      <a href="https://github.com/WillyJL">
        <img src="https://github.com/WillyJL.png" width="80" height="80" style="border-radius: 50%;" alt="WillyJL"/><br/>
        <b>WillyJL</b>
      </a><br/>
      <sub>Core Flipper Firmware functionality and BLE Spam code</sub>
    </td>
    <td align="center">
      <a href="https://github.com/flipperdevices/flipperzero-firmware">
        <img src="https://github.com/flipperdevices.png" width="80" height="80" style="border-radius: 50%;" alt="flipperdevices"/><br/>
        <b>Flipper Zero firmware</b>
      </a><br/>
      <sub>Core IR &amp; NFC implementation (flipperdevices/flipperzero-firmware &amp; contributors)</sub>
    </td>
    <td align="center">
      <a href="https://github.com/Garag">
        <img src="https://github.com/Garag.png" width="80" height="80" style="border-radius: 50%;" alt="Garag"/><br/>
        <b>Garag</b>
      </a><br/>
      <sub>Core NFC library</sub>
    </td>
    <td align="center">
      <a href="https://github.com/connornishijima">
        <img src="https://github.com/connornishijima.png" width="80" height="80" style="border-radius: 50%;" alt="connornishijima"/><br/>
        <b>connornishijima</b>
      </a><br/>
      <sub><a href="https://github.com/connornishijima/SensoryBridge">SensoryBridge</a> - MIC RGB visualizer algorithms &amp; inspiration</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/DarkFlippers">
        <img src="https://github.com/DarkFlippers.png" width="80" height="80" style="border-radius: 50%;" alt="DarkFlippers"/><br/>
        <b>DarkFlippers</b>
      </a><br/>
      <sub>Flipper Zero Unleashed firmware (SubGHz protocol decoders)</sub>
    </td>
    <td align="center">
      <a href="https://github.com/xMasterX">
        <img src="https://github.com/xMasterX.png" width="80" height="80" style="border-radius: 50%;" alt="xMasterX"/><br/>
        <b>xMasterX</b>
      </a><br/>
      <sub>Flipper Zero Unleashed SubGHz improvements</sub>
    </td>
    <td align="center">
      <a href="https://github.com/DecentLabs">
        <img src="https://github.com/DecentLabs.png" width="80" height="80" style="border-radius: 50%;" alt="DecentLabs"/><br/>
        <b>DecentLabs</b>
      </a><br/>
      <sub><a href="https://github.com/DecentLabs/officeAir">officeAir</a> - multi-pass ARP scanning &amp; lwIP thread-safety techniques</sub>
    </td>
    <td align="center">
      <a href="https://github.com/jaylikesbunda">
        <img src="https://github.com/jaylikesbunda.png" width="80" height="80" style="border-radius: 50%;" alt="jaylikesbunda"/><br/>
        <b>jaylikesbunda</b>
      </a><br/>
      <sub>Project maintainer</sub>
    </td>
    <td align="center">
      <a href="https://github.com/Play2BReal">
        <img src="https://github.com/Play2BReal.png" width="80" height="80" style="border-radius: 50%;" alt="Play2BReal"/><br/>
        <b>Play2BReal</b>
      </a><br/>
      <sub>WiGLE upload &amp; IO expander support</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/the1anonlypr3">
        <img src="https://github.com/the1anonlypr3.png" width="80" height="80" style="border-radius: 50%;" alt="the1anonlypr3"/><br/>
        <b>the1anonlypr3</b>
      </a><br/>
      <sub>Art and assets</sub>
    </td>
    <td align="center">
      <a href="https://github.com/Billi-Green">
        <img src="https://github.com/Billi-Green.png" width="80" height="80" style="border-radius: 50%;" alt="Billi-Green"/><br/>
        <b>Billi-Green</b>
      </a><br/>
      <sub>Audio &amp; ENV-III sensor support</sub>
    </td>
    <td align="center">
      <a href="https://github.com/Next-Flip">
        <img src="https://github.com/Next-Flip.png" width="80" height="80" style="border-radius: 50%;" alt="Next-Flip"/><br/>
        <b>Next-Flip</b>
      </a><br/>
      <sub>Momentum-Firmware - NFC parser base (EMV, DESFire, hardnested, transit parsers; Gallagher by Nick Mooney)</sub>
    </td>
    <td align="center">
      <a href="https://github.com/noproto">
        <img src="https://github.com/noproto.png" width="80" height="80" style="border-radius: 50%;" alt="noproto"/><br/>
        <b>noproto</b>
      </a><br/>
      <sub>MIFARE Classic hardnested / nested recovery</sub>
    </td>
    <td align="center">
      <a href="https://github.com/leptopt1los">
        <img src="https://github.com/leptopt1los.png" width="80" height="80" style="border-radius: 50%;" alt="leptopt1los"/><br/>
        <b>Leptopt1los</b>
      </a><br/>
      <sub>EMV payment-card parser</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/bettse">
        <img src="https://github.com/bettse.png" width="80" height="80" style="border-radius: 50%;" alt="bettse"/><br/>
        <b>bettse</b>
      </a><br/>
      <sub><a href="https://github.com/bettse/picopass">picopass</a> - PicoPass / iCLASS support</sub>
    </td>
    <td align="center">
      <a href="https://github.com/micolous">
        <img src="https://github.com/micolous.png" width="80" height="80" style="border-radius: 50%;" alt="micolous"/><br/>
        <b>micolous</b>
      </a><br/>
      <sub>Opal transit card parser</sub>
    </td>
    <td align="center">
      <a href="https://github.com/emilytrau">
        <img src="https://github.com/emilytrau.png" width="80" height="80" style="border-radius: 50%;" alt="emilytrau"/><br/>
        <b>emilytrau</b>
      </a><br/>
      <sub>myki transit card parser</sub>
    </td>
    <td align="center">
      <a href="https://github.com/holiman">
        <img src="https://github.com/holiman.png" width="80" height="80" style="border-radius: 50%;" alt="holiman"/><br/>
        <b>holiman</b>
      </a><br/>
      <sub>loclass - MIFARE key recovery algorithms</sub>
    </td>
    <td align="center">
      <a href="https://github.com/RfidResearchGroup">
        <img src="https://github.com/RfidResearchGroup.png" width="80" height="80" style="border-radius: 50%;" alt="RfidResearchGroup"/><br/>
        <b>RfidResearchGroup</b>
      </a><br/>
      <sub>proxmark3 - RFID research tooling</sub>
    </td>
    <td align="center">
      <a href="https://meshtastic.org">
        <img src="https://raw.githubusercontent.com/meshtastic/design/master/Meshtastic%20Powered%20Logo/M-POWERED_minified.svg" width="80" height="80" alt="M-Powered by Meshtastic"/><br/>
        <b>Meshtastic®</b>
      </a><br/>
      <sub>Mesh framing, crypto, and PhoneAPI reference</sub>
    </td>
  </tr>
  <tr>
    <td align="center">
      <a href="https://github.com/jgromes/RadioLib">
        <img src="https://github.com/jgromes.png" width="80" height="80" style="border-radius: 50%;" alt="jgromes"/><br/>
        <b>RadioLib</b>
      </a><br/>
      <sub>SX126x driver reference</sub>
    </td>
    <td align="center">
      <a href="https://github.com/agl/curve25519-donna">
        <img src="https://github.com/agl.png" width="80" height="80" style="border-radius: 50%;" alt="agl"/><br/>
        <b>curve25519-donna</b>
      </a><br/>
      <sub>X25519 for Meshtastic PKI directs (BSD, vendored verbatim)</sub>
    </td>
    <td align="center">
      <a href="https://github.com/HighCodeh/TentacleOS">
        <img src="https://github.com/HighCodeh.png" width="80" height="80" style="border-radius: 50%;" alt="HighCodeh"/><br/>
        <b>TentacleOS</b>
      </a><br/>
      <sub>LoRa modem preset table cross-check (GPL-3.0, no code used)</sub>
    </td>
  </tr>
</table>

### Attributions

LoRa region slot math, header layout, AES-CTR nonce, PKI wire format, and BLE PhoneAPI sequencing are modeled on Meshtastic® firmware and protobufs (GPL-3.0). SX126x bring-up references RadioLib documentation. X25519 is vendored verbatim from curve25519-donna (Google Inc., BSD, see [LICENSES/curve25519-donna-BSD.txt](LICENSES/curve25519-donna-BSD.txt)); all other GhostESP LoRa code is original under this project's GPL-3.0.

> Meshtastic® is a registered trademark of Meshtastic LLC. Meshtastic software components are released under various licenses, see GitHub for details. No warranty is provided - use at your own risk.

GhostESP is not affiliated with or endorsed by Meshtastic.

---

## Contributing

GhostESP welcomes contributions — from a one-line board config to a new feature.

- **Adding a board**: https://docs.ghostesp.net/v2.1/development/custom-board-configs/
- **Fixing a bug or adding a feature**: check [open issues](https://github.com/GhostESP-Revival/GhostESP/issues) - anything tagged `good first issue` is a solid place to start.
- **Building a native app**: see the `gbt` (Ghost Build Tool) [docs](https://docs.ghostesp.net/v2.1/development/gbt/) and example apps (Device Inspector, ESP32Finder) for the SDK pattern.
- Questions before you start? Ask in [Discord](https://discord.gg/5cyNmUMgwh) - the team is responsive.

---

## Disclaimers

Ghost ESP is intended solely for educational and ethical security research. Unauthorized or malicious use is illegal. Familiarize yourself with your local laws, and always obtain proper permission before conducting any network tests.

> **Note:** this is a detached fork of [Spooky's GhostESP](https://github.com/Spooks4576/Ghost_ESP), which has been archived and is no longer in development.

For guidelines on using the GhostESP name and logo, see [BRAND GUIDELINES](BRAND_GUIDELINES.md).

Interested in becoming an official partner? Email `partners@ghostesp.net`.

---

## Open Source Contributions

This project is open source and welcomes your contributions. If you've added new features or enhanced device support, please submit your changes!
