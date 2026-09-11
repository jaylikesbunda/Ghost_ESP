# Ghost ESP Changelog

## Attribution
Untagged entries are by ([@jaylikesbunda](https://github.com/jaylikesbunda)). A trailing `@handle` credits a guest contributor for that specific line. "Ported from / adapted from" credits the upstream source a feature was based on, not GhostESP authorship.

## v2.2

### Added
- Added Meshtastic-compatible LoRa support for SX1262-family radios including BLE app support with the official Meshtastic App
- Added LoRa support to:
  - Heltec V3
  - Elecrow CrowPanel Advance 2.4-inch
  - Elecrow CrowPanel Advance 2.8-inch
  - Elecrow CrowPanel Advance 4.3-inch
- Added USB SD card passthrough on ESP32-S3 boards with an SD card
- Added custom channel hopping setting that applies to deauth, beacon spam, AP and station scans, airspace monitor, and packet visualizer/capture hopping.
- Added a Country selector to Settings > Wi-Fi for display UI
- Added a Row Height setting for the options menus
- Added support for setting IR TX/RX pins at runtime through the CLI or display UI settings menu
- Added `dualwd` BLE + WiFi coexistence wardriving (exclusive to PSRAM devices), available from the CLI and GPS menu
- Added Elecrow CrowPanel 1.28-inch rotary display support with USB Audio volume/mute control
- Added `mdnssniff` passive local-name sniffer (mDNS/LLMNR/SSDP/NetBIOS) per ARP-discovered host
- Added a `bledetect` CLI command for the device detect scan with list, track and AirTag spoof actions
- Added a `glbench` CLI command and Settings > Tools row to benchmark GhostLink throughput and view link statistics
- Added more devices to the BLE detect devices scan
  - Tile trackers. Ported from `gatt_scan.c`, adapted from [Flock-You-Android](https://github.com/MaxwellDPS/Flock-You-Android/blob/main/docs/detections/BLE_TRACKER_DETECTION.md) and [Silabs](https://docs.silabs.com/bluetooth/2.13/bluetooth-code-examples-applications/bluetooth-device-tracking-with-tile)
  - Samsung SmartTag. Adapted from [arxiv 2210.14702](https://arxiv.org/pdf/2210.14702) and [adwatch](https://github.com/bensmith83/adwatch/blob/main/docs/protocols/smarttag.md)
  - Chipolo. Ported from `gatt_scan.c`
  - Apple AirPods with per-model variants. Adapted from [Theengs](https://github.com/theengs/decoder/blob/development/src/devices/APPLEAIRPODS_json.h) and [Bruce ble_spam](https://github.com/BruceDevices/firmware/blob/main/src/modules/ble/ble_spam.cpp)
  - Apple Watch. Adapted from [Theengs](https://github.com/theengs/decoder/blob/development/src/devices/APPLEWATCH_json.h)
  - Generic FindMy clones. Ported from `gatt_scan.c`
  - Fast Pair / Find Hub accessories. Adapted from the [Google spec](https://developers.google.com/nearby/fast-pair/specifications/extensions/fmdn)
  - Hearing aids. Adapted from [Android docs](https://source.android.com/docs/core/connect/bluetooth/asha)
  - Exposure beacons. Adapted from [Theengs](https://github.com/theengs/decoder/blob/development/src/devices/GAEN_json.h)
  - Chameleon Ultra. Adapted from `chameleon_manager.c` and [Nordic docs](https://docs.nordicsemi.com/bundle/ncs-latest/page/nrf/libraries/bluetooth_services/services/nus.html)

### Changed
- Defaulted the terminal to white small text and improved readability with padding, spacing and colour-coded output
- Reorganized Settings menu categories for easier navigation
- Smoothed and improved stability and reliability of the Track RSSI readout and added a warmer/colder trend arrow
- Standardized bottom touch bar via shared helper
- Unified back-key handling with shared helper
- Standardized list/log scrolling with shared helper
- Standardized view layout and cleanup with shared helpers
- Mapped IR buttons to theme palette
- Reduced GhostScript RAM use on across all boards
- Matched Ethernet dashboard status card to the rounded action rows
- Cleaned up boot and GhostLink console output
- Restyled the on-screen keyboard with a standard iOS-style layout
- Wi-Fi and Bluetooth scan result menus now scroll as a single list instead of paging through `< Prev` / `Next >` rows
- Raised the Banshee GhostLink UART to 921600 baud and enlarged the UART receive buffer to 2048 bytes so the higher rate does not increase receive loss

### Fixed
- Fixed GhostScript crash when an event listener received an event
- Fixed BadBLE touch-drag scrolling
- Fixed CrowPanel display startup configs
- Fixed ESP32-C5 merged firmware failing to boot when flashed at `0x0` by placing the bootloader at the required `0x2000` offset - @yanxke (#395)
- Fixed BadBLE poll timer leak on exit
- Fixed lockscreen/ghostscript touch bar gaps on large screens
- Fixed detail titles always showing 'Details'
- Fixed PSRAM task stack leaks across Wi-Fi attacks, scans, Ethernet, nRF24, SubGHz and MIC visualizer
- Fixed DIAL URL/session buffer leaks and uninitialized-pointer cleanup
- Fixed leaked LVGL async payloads when the display queue is busy
- Fixed PCAP and wardriving CSV file handles leaking on restart
- Fixed semaphore leaks in Chameleon and audio receiver init failures
- Fixed Cloud Store install progress showing "0 KB downloaded" during the install phase
- Fixed detail view info labels being cut off instead of scrolling sideways when too wide


## Revival v2.1.2

### Added
- Added support for more devices including ESP32-P4 boards with ESP32-C6 ESP-Hosted Wi-Fi and Bluetooth  (huge thank you to M5Stack and Elecrow for providing hardware to work on)
  - M5Stack AtomS3R
  - M5CoreS3SE
  - Elecrow CrowPanel 4.2-inch E-paper
  - Elecrow CrowPanel 5.79-inch E-paper
  - Elecrow CrowPanel Advance 2.4-inch
  - Elecrow CrowPanel Advance 2.8-inch
  - Elecrow CrowPanel Advance 3.5-inch
  - Elecrow CrowPanel Advance 4.3-inch
  - Elecrow CrowPanel Advance 5-inch
  - Elecrow CrowPanel Advance 7-inch
  - Elecrow CrowPanel Advanced P4 5-inch
  - Elecrow CrowPanel Advanced P4 7/9/10.1-inch (v1.2+)
  - Elecrow CrowPanel Advanced P4 7/9/10.1-inch (v1.1)

- P4 and large S3 improvements including but not limited to:
  - Added left hand side swipe gestures for back and iOS-style home bar on the bottom of the screen for swipe up home 
  - Added new native camera app
  - Tweaked menu and view spacing and layouts
  - Added a control center for quick controls when swiping down from the status bar

- E-Paper support exclusive:
  - NEW Native 'Reader' app placed at the start of the main menu which can read .epub, .cbz, .jpg and more

- Added favorites menu where you can pin menu items, IR remotes, NFC tags, SubGHz captures and apps
- Replaced the accent-only menu themes with 21 full color palettes
- Added BadBLE native Bluetooth HID keyboard that runs DuckyScript payloads over BLE with on-device UI and CLI support
- Added the Hero main menu layout

### Changed
- Migrated to ESP-IDF v6.1 from v6.0.2
- Reduced screen mirroring overhead
- Standardized main menu and App Gallery icons
- Main Menu and App Gallery contents can be customized and reordered independently on all device targets
- BLE Detect Devices "Track" action now uses the live RSSI ring overlay (matching Track AP/STA/Adv/GATT) instead of switching to the terminal
- Added runtime global log-level control through the `loglevel` command and system settings
- SD card not inserted now logs a single clear line instead of repeating driver errors
- LVGL frame pacing is now a fixed 16 ms instead of rendering as fast as possible
- Carousel cards no longer draw a software shadow, cutting frame render cost while swiping
- Carousel side previews now swap their icon in place instead of destroying and recreating the widget on every nav step
- Neighbour icons are pre-fetched into the asset cache after each carousel/Hero transition
- Demoted hot-path navigation and input logs to verbose so UART output no longer stalls input handling
- Raised the persistent shared-SPI SD clock to 20 MHz on the Banshee C5 (was 10 MHz) so SD reads finish faster and free the display bus sooner
- Massively improved Banshee C5 display responsiveness by driving the screen over the PARLIO peripheral, freeing SPI for a permanent 20MHz SD mount - @Billi-Green
- Audio player shows exact track length parsed from Xing/VBRI headers (CBR fallback excludes ID3 tags)
- Audio progress now follows the audible playback clock: no jump at track start, and pause/resume continues from the same point
- 320 kbps MP3s now play on the local speaker; the bitrate cap only applies to GhostLink streaming
- Both audio player screens gained a touch Back bar matching other views

### Fixed
- Fixed the AP never coming back after scans or deauth attacks - resolves #382
- Fixed deauth attacks logging "Failed to set channel" while a client was connected to the GhostNet AP - resolves #368, #327
- Fixed GhostLink-relayed Ethernet fingerprint/port/ping scans and ARP poison silently failing; display now flags a peer that doesn't acknowledge
- Fixed the on-screen terminal keyboard's Done button not submitting commands
- Fixed crash on CYD devices by bumping LV_MEM_POOL to 24KB
- Fixed T-Dongle-C5 display colors (pink toast/noise) by enabling RGB565 byte-swap and dropping the SPI clock to 20 MHz to match the factory ST7735 driver
- Fixed T-Dongle-C5 backlight polarity so the display turns on at boot (active-low GPIO0)
- Fixed WebUI GhostLink pin changes not persisting across reboots
- Extended `setcountry` to all targets via the ESP-IDF country-code API so it works on 2.4 GHz-only boards and the C5 alike

### Removed
- Disabled self-OTA on the Banshee C5 (the updater partition and embedded updater image were bad UX and repeatedly pushed the build over flash. OTA is still available over GhostLink to the S3)


## Revival v2.1.1

### Added
- Added GhostLink P1 Core and Peer support
- Added new Pong, 'Lightcycle', Breakout, Snake and Tetris games to the app marketplace for C5 and S3 targets
- Added Wake-on-LAN gadget
- Added Govee LAN light discovery and control
- Added Probe Request Flood attack (directed SSID probes with randomized MACs, supports multi-selected APs)
- Improved C5 station scans with AP-aware 5 GHz channel hopping
- Expanded `wpa3check` with cipher and Enterprise/EAP assessment limits, plus PMF deauth warnings
- Added per-observation sweep timestamps and PCAP capture statistics
- Added Bad Msg attack (forged EAPOL key-install frames that drop stations from WPA networks)
- Added Auth Flood attack (jittered 802.11 auth storm with randomized MACs and rate limiting)
- Added Compact, an icon-free single-screen label layout inspired by [@MatthewKuKanich](https://github.com/MatthewKuKanich)'s Compact menu style in [Next-Flip/Momentum-Firmware](https://github.com/Next-Flip/Momentum-Firmware)
- ARP host details now offer per-host Scan Open Ports, SSH Banner, NetBIOS, HTTP Banner, SNMP Probe/Walk and SMB Enum actions

### Fixed
- Fixed Back navigation
- Fixed merged binary creation in GitHub workflow
- Fixed TDisplayS3-Touch touch input not working since ESP-IDF v6.0 i2c migration
- Fixed recurring "Wrong I2C status" errors on shared I2C buses: the shared I2C layer now caches device handles instead of adding/removing them per transaction
- Fixed `sd read --base64` downloads sending truncated data
- Fixed OTA and Cloud Store manifest fetches failing when CDN requests were redirected from HTTP to HTTPS
- Fixed Cloud Store fetches failing TLS verification via the HTTPS proxy by enabling cross-signed certificate bundle verification on all boards
- Fixed the GhostLink BLE bridge making both paired boards advertise as "GhostESP Bridge" (duplicate devices in the companion scan) and dropping command responses
- Fixed WiFi auto-reconnect attempts during intentional WiFi shutdowns
- Fixed WiFi reconnect interfering with AP/STA scans
- Fixed the Channel Switch attack broadcasting beacons to the AP's own MAC instead of broadcast and transmitting them on the wrong channel
- Fixed EAPOL Logoff never starting in the STA-only boot configuration
- Fixed a 2-byte over-length beacon frame in Beacon Spam
- Fixed a stack buffer overflow and dropped packets in the BLE Samsung Buds spam payload
- Fixed stack overflows in the SNMP walk when formatting long OIDs
- Fixed the port scan looping forever and dividing by zero on a full 1-65535 port range, and missing ports that connect instantly
- Fixed the AP scan dropping valid results after a blocking scan
- Fixed use-after-free of ARP scan callback contexts on the lwIP thread and the packet monitor's unbalanced core-lock call
- Fixed the ARP poisoning ICMP sweep never running
- Fixed the SSH host scan ignoring a pending scan cancellation
- Fixed a stack overflow when running SMB enum on a single host from the UI
- Fixed ARP sweep hosts being missed when table entries evicted before harvest, and SMB parsing breaking on fragmented responses
- Fixed a stack overflow in the Probe Request Flood attack task
- Fixed inverted joystick/keyboard scroll direction in the Airspace Monitor
- Fixed a use-after-free panic in the display manager when a cached touch-pressed object was destroyed in-place before the release event
- Touch now doesn't affect row styling behind a settings popup when open
- Fixed Back navigation not returning to the true parent view
- Fixed Wi-Fi scan result Back navigation bouncing into stale result lists

### Changed
- Condensed the SD card boot log into a single card summary line
- BLE manager now only deinits WiFi before init for non-PSRAM configs
- LAN scans now reuse ARP sweep results for host discovery with MAC vendor labels
- Port scan TCP connects now run 8 sockets in parallel for much faster subnet scans
- SMB enum now negotiates SMB2 and reports dialect + signing status on modern hosts
- SNMP probe now retries and validates responses; custom communities via `snmpprobe communities`
- LAN scans now use the real netmask (up to /20) instead of assuming /24
- Time now syncs via SNTP automatically on every Wi-Fi connection, not just manual `wifi connect` commands
- Random Beacon Spam now broadcasts multiple random SSIDs per channel hop
- Improved Airspace Monitor accuracy
- Card backgrounds are now off by default
- Various small optimisations to the ALPHA_8BIT LVGL rendering path
- The Banshee display is now double buffered
- Clarified Wi-Fi menu labels and standardised actions without the "Start" prefix
- Changed the pressed row styling to something a little.. cleaner
- Replaced the old splash logo on the TDeck
- Moved the Timezone settings option to it's own 'Date & Time' category
- Carousel labels now use a 60% opaque black background when an asset pack is enabled
- Moved the LVGL heap to PSRAM
- Moved UI and NFC buffers to PSRAM
- Moved GhostLink queues to PSRAM
- Removed unused NFC SPI buffers
- Removed a redundant OTA task
- Increased idle internal RAM from ~37 KB to ~75 KB

## Revival v2.1.0 - 2026-08-03

### TL;DR

**NFC:**
- GhostESP now supports a second NFC chip, the ST25R3916.
- GhostESP can now read more card types, including EMV payment cards, full DESFire application and file trees, PicoPass/iCLASS cards, and transit cards such as Opal, myki, ITSO, and Gallagher.
- MIFARE Classic keys can now be recovered with nested attacks, and you can create NDEF tags and read NTAG metadata.

**Updates and apps:**
- You can now update the firmware from GhostESP itself. You can update over Wi-Fi, from the SD card, or through a paired GhostLink peer.
- A new Cloud Store App lets you browse and install apps, scripts, and asset packs directly on the board through a connected wifi connection.
- GhostScript adds a sandboxed Lua runtime to run scripts from the SD card. 
- Native SD apps can now send ESP-NOW messages. This release adds three new apps: a Doom port, HackChat (an ESP-NOW messaging app), and a QR Generator.

**Wi-Fi:**
- GhostESP can now run a combined handshake and deauth attack. 
- In 2.1 we also added SMB and SNMP enumeration and a live packet visualizer.
- The Airspace Monitor can now detect more attack types. Before, it could only show that traffic was suspicious.
- Wi-Fi can now reconnect to your saved network automatically (Settings > Connectivity), and a new Channel Congestion chart shows how busy each channel is.

**Ethernet:**
- Ethernet scans are about three times faster and now export their results to the SD card automatically.

**Command line:**
- The terminal gained OS-style commands (echo, ifconfig, ping, version, uptime), persistent aliases, command scripts, environment variables, and typo suggestions.

**Reliability:**
- Wardriving now handles heavy load better.
- Before, Ethernet scans always used a /24 subnet size. Now they use the real subnet size.
- The Banshee C5 display and SD card no longer block each other. Before, they had to share the same SPI bus one at a time.
- Boards without an SD slot (like the Banshee S3) can now save files to their paired GhostLink peer's SD card.
- A long list of stability bugs was fixed: the external RTC keeps correct time across reboots, station scans no longer silently capture nothing, and several under-sized task stacks (OTA updates, BLE bridge, CLI watch) were fixed.

**UI:** This release fixes an old timing bug. The LVGL tick task used a fixed 10ms step, not the real frame time. This made animations play too slowly when a frame took a long time to draw. The tick now uses the actual time that has passed. This fix makes menu navigation and scrolling smoother. The main menu and Apps layouts were also reworked (Carousel, Grid, and List views), scrolling is animated and smoother throughout, and more screens remember where you were when you go back.


### NFC
- Added ST25R3916/ST25R3916B NFC support over SPI or I2C, plus an `auto` / `pn532` / `st25r` backend selector in UI and CLI
- Added more NFC tag tools: MIFARE Classic nested recovery, PicoPass/iCLASS reads, NDEF creation, NTAG metadata, and DESFire summaries, with credits to [flipperzero-firmware](https://github.com/flipperdevices/flipperzero-firmware), [Momentum-Firmware](https://github.com/Next-Flip/Momentum-Firmware), [@noproto](https://github.com/noproto), [bettse/picopass](https://github.com/bettse/picopass), and loclass ([proxmark3](https://github.com/RfidResearchGroup/proxmark3))
- Added EMV payment-card reading (PPSE/AID selection, GPO, and record parsing for PAN, expiry, and issuer country/currency with ISO name lookup), ported from the [Momentum-Firmware](https://github.com/Next-Flip/Momentum-Firmware) EMV poller and payment-card parser by [@leptopt1los](https://github.com/leptopt1los)
- Expanded DESFire support from version summaries to full application/file tree reads (lists applications, file settings, and plaintext file data over ISO7816) with Flipper-compatible `.nfc` export, adapted from the [Momentum-Firmware](https://github.com/Next-Flip/Momentum-Firmware) MIFARE DESFire poller
- Added supported-card parsers for Opal (Sydney), myki (Melbourne), ITSO (UK), and Gallagher access control, ported from [Momentum-Firmware](https://github.com/Next-Flip/Momentum-Firmware) with credit to [@micolous](https://github.com/micolous) (Opal), [@emilytrau](https://github.com/emilytrau) (myki), and Nick Mooney (Gallagher)
- Added Momentum-compatible nested logs at `/mnt/ghostesp/nfc/.nested.log` and the `nfc hardnested` CLI command
- Cleaned up the NFC menu, scan popup, progress labels, and credits page
- Fixed MIFARE Classic summaries skipping block reads after default-key auth

### Firmware Updates (OTA)
- Added Wi-Fi firmware updates from **Settings > Firmware Update**, with verification and rollback protection on supported boards
- Added GhostLink peer updates, so a primary device can download firmware and safely flash its paired peer
- Added offline SD card installs from `/ghostesp/firmware_update.bin`, with optional `.sha256` verification
- Fixed a boot-time stack overflow in the peer OTA background check on GhostLink primary boards; background checks now run every boot (gated on connectivity instead of a 24h timer) and share a single task

### Cloud Store
- Added Cloud Store in the Apps gallery for browsing and installing apps, scripts and asset packs from the GitHub catalogs
- Added a progress bar view showing download status while installing from Cloud Store
- Bounded catalog response buffering to protect heap availability during refreshes

### Native SD Apps & SDK
- Added RGB565 canvas blits and per-app tick intervals for high-frame-rate native apps
- Added HackChat for nearby ESP-NOW messaging with deterministic Ghostchi identities
- Added permission-gated ESP-NOW discovery and messaging APIs for native apps
- Added ESP32-C5 GOT relocation support and build-time relocation validation for larger native apps
- Added offset-based native app data and packaged-asset reads for streaming large files on JIT-mounted SD boards
- Added the joystick-required Doom Port native app with a bundled, directly streamed Freedoom IWAD
- Added the QR Generator native SD app with a compact menu, responsive full-screen QR preview, and touch, keyboard, encoder, and D-pad controls
- Fixed native app keyboard dialogs preserving the loaded app across the keyboard view, preventing callbacks into unloaded apps after submission
- Added live bounded touch scrolling to native apps without full-screen redraws
- Normalized physical keyboard arrow keys for native SD app navigation
- Added manifest input requirements that keep incompatible apps visible and prevent unsupported launches with a toast
- Added stable `ui_image_set_builtin` SDK access to bundled Ghostchi images
- Added interleaved true-color-alpha app icons and fixed GAPP manifest checksums on Windows

### GhostScript
- Added the GhostScript sandboxed Lua 5.4 runtime for running precompiled `.gsb` scripts from the SD card, including devices without PSRAM
- Added the GhostScript browser and `script list`, `script run <index>`, `script status`, and `script stop` CLI commands
- Added manifest permissions, scoped script storage, cooperative long-running scripts, failure-state recording, and a PSRAM-preferred runtime event queue
- Added JIT SD handling for display-sharing boards so GhostScript can access scripts without leaving the display SPI bus unavailable

### Wi-Fi
- Added combined Handshake+Deauth attack (`attack -hsd`) that sends short deauth bursts to force client reconnection while capturing EAPOL handshakes to a PCAP file, accessible from the Attacks menu, AP/Station detail views, and GhostLink remote
- Added a WiFi auto-reconnect toggle (Settings > Connectivity, `autoreconnect <on|off>` CLI, and `AutoReconnect=` in `config.cfg`) persisted to NVS
- Fixed standalone station scans to run the AP scan spinner first when no APs are cached
- Added SMB/NetBIOS enumeration scanner (`enumscan`) with native UI: discovers OS, domain, shares, and users via null session over port 445
- Added SNMP MIB walk (`snmpprobe walk`) using GetNextRequest to traverse OID subtrees with support for custom root OIDs
- Added a compact live Wi-Fi packet monitor (`scanarp monitor`) using the Wireshark raw-capture path
- Improved ARP scan with multi-pass scanning (4 passes with inter-pass delays), thread-safe lwIP access via TCP/IP core locking, lwIP `etharp_request` replacing raw 802.11 TX, and netmask-aware subnet scanning (respects /20-/31 instead of hardcoded /24) — techniques adapted from [DecentLabs/officeAir](https://github.com/DecentLabs/officeAir) (MIT-licensed)
- Added a graphical Packet Visualizer with smooth color-filled per-channel activity, channel hopping, and custom channel selection
- Added a responsive Channel Congestion chart with active-channel labels and scan summaries
- Airspace Monitor now detects more attacks: deauth spoof/tool fingerprinting (reason code + sequence analysis), evil-twin APs, Karma/Mana, auth floods, and adaptive beacon-flood detection that self-tunes to the local RF density
- Normalized WiFi TX buffer allocation and lwIP TCP window/mbox sizing across board configs, trimming redundant heap usage with no change to scan/deauth/sniffer behavior

### Wardriving
- Refactored CSV logging to drain bounded batches asynchronously, keeping Wi-Fi, BLE, and GhostLink capture paths responsive during storage writes
- Added helper readiness, GPS freshness, link-loss fallback, and retry-safe GhostLink forwarding for split wardriving
- Added a PSRAM-backed observation queue with GPS-at-capture snapshots, graceful draining, backpressure retries, and drop/high-water telemetry
- Changed the default primary/helper channel-hop interval to 125 ms and kept weighted common-channel 5 GHz coverage enabled by default
- Reduced wardriving callback load with management-frame hardware filtering
- Fixed WiGLE headers for UART/JIT SD output, reliable JIT SD finalization, and hidden/32-byte/UTF-8 SSID handling
- Fixed AP entries being silently dropped when a malformed beacon's DS Parameter Set IE reported a garbage channel number, now falls back to the radio's own channel instead
- Removed console log spam on every UART-streamed CSV chunk during active wardriving
- Added the actual error reason to the "Failed to write wardriving data to CSV buffer" log for easier diagnosis

### Infrared
- Added transmit support for the NEC42, NEC42ext, and RC5X protocols, so all of Flipper's IR protocols can now be sent as well as learned

### Ethernet
- Fixed `ethping`/ping sweep and `etharp`/ARP scan (CLI and GhostLink peer-relayed UI) always scanning a hardcoded /24 instead of the real DHCP netmask, missing hosts on smaller VLAN subnets
- Sped up ARP scan to match the Wi-Fi ARP scan's batching/timing, cutting a full-subnet scan from ~9s to ~2.5s
- Fixed peer-relayed Fingerprint Scan backing out immediately if a previous ARP/port/ping scan had run that session, from stale shared scan-done state
- Added automatic SD export for all Ethernet scans (ARP, fingerprint, port, ping, ARP poison) as JSONL files in `/mnt/ghostesp/scans/`, written via a new GhostLink peer-storage channel on boards without a local SD slot

### SD Storage
- Centralized all standard directory paths in `sd_card_manager.h` as `SD_DIR_*` macros so features share one source of truth
- Boot now creates `sweeps/`, `ghostchi/pcaps/`, `ghostchi/sessions/`, `app_cache/`, `appdata/`, `scripts/`, `scriptdata/`, and `downloads/` at mount time, matching the directories active features already use
- **Eliminated the long-standing Banshee C5 display/SD contention:** persistent shared SPI now allows SD card IO while the display continues rendering, without disruptive JIT mount handoffs

### GhostLink
- Added `COMM_STREAM_CHANNEL_STORAGE` for peer-backed file IO, so an SD-less board (e.g. Banshee S3) can write files to its paired peer's SD card over GhostLink
- Peer storage handler uses `sd_card_jit_begin/end` on the display peer so shared SPI bus arbitration with LVGL is respected automatically

### Headless CLI
- Added OS-style CLI commands for `echo`, `ifconfig`, `ping`, `version`, `uuid`, `macaddr`, `uptime`, `status`, and filesystem helpers
- Added persistent aliases, hostname/prompt color, banner control, history, `watch`, command scripts, environment variables, and typo suggestions
- Fixed the `subghz` command missing its CLI registration since the commandline.c refactor, causing all subghz commands (local and GhostLink peer-relayed) to fail with "Unsupported command"

### Main Menu, Apps & Navigation
- Restored submenu, selection, and scroll state when backing out of options and returning from tool views
- Fixed Apps menu grid scrolling so the selection stays visible when scrolling down on Cardputer
- Fixed Cardputer ADV keyboard spamming repeated select/input events when opening Apps menu
- Fixed pressed-state visual feedback (darken + scale) never actually being applied to any button; it's now wired into every button across the UI
- Smooth scroll on selection changes in main menu grid/list
- Lockscreen ghost companion bob uses a sine wave instead of a triangle wave
- Toast notifications decelerate as they exit instead of accelerating off screen
- Main menu list selection border now uses theme accent color instead of hardcoded white
- Reworked Main Menu and Apps layouts with responsive Carousel, Grid, and List views across compact and large displays
- Added paginated Grid navigation with page dots, swipe/controller support, top-left page alignment, and subtle selected tiles
- Fixed Grid view joystick/encoder/D-pad navigation dropping to the next row instead of the next page when pressing right/left from the edge column
- Improved Carousel navigation with previous/next previews, consistent directions, and faster transitions
- Fixed the LVGL tick task feeding a fixed 10ms increment to the animation clock regardless of actual frame time, which made every animation on the device play in slow motion whenever a render ran long; it now advances by real elapsed time
- Animated paged and row-selection scrolling in detail views (AP/STA/BLE/ARP/mDNS/sweep results) instead of snapping instantly
- Animated main menu List layout selection scroll
- Sped up and smoothed Grid page-swap transitions with a shorter, eased animation
- Removed a redundant full-grid relayout that ran on every Grid navigation press instead of only on page changes
- Fixed Banshee touch registering double taps: LVGL's own indev polling and the manual touch-input pipeline were both independently reading the same touch controller and deciding taps on their own, most visibly right at view transitions
- Fixed Audio Player's back button always returning to Apps Gallery instead of wherever it was actually opened from (it's reachable both from Apps Gallery and a direct Main Menu item)
- Added a generic `display_manager_go_back()` so views reachable from more than one place (Apps Gallery, a direct menu item, a CLI command, or a hardware-button shortcut that can fire from any screen) return to wherever they were actually opened from, instead of a single hardcoded destination — fixes back navigation on NFC, Infrared, BadUSB, SubGHz, Compass, ENV-III, Accelerometer, Clock, Ghostchi, the plugin/GhostScript runners, and the music visualizer
- Fixed Apps Gallery, NFC, Infrared, BadUSB, and SubGHz always resetting to the first item/root menu on re-entry instead of restoring the previous selection: their `destroy()` handlers were clearing state that `create()` needed to restore it

### Fixed
- Fixed stack buffer overflow in infrared universal file path construction when SD card filenames exceeded available space
- Fixed memory leak in DIAL manager session binding when HTTPS response buffer allocation fails
- Fixed memory leak in DESFire application tree read when realloc fails mid-enumeration
- Fixed memory leak in ESP command stream buffer reallocation (old buffer was not freed before replacement)
- Consolidated `MAX_WIFI_CHANNEL` into `network_constants.h` and removed nine duplicate definitions across the codebase
- Fixed `MAX_WIFI_CHANNEL` incorrectly set to 165 for ESP32-C6 (which does not support 5 GHz)
- Fixed `SERIAL_BUFFER_SIZE` conflicting macro definitions (512 vs 528) between serial manager and AP manager
- Added `static` to `const` arrays in four headers (`ghost_esp_site_gz.h`, `m5_keyboard_def.h`, `default_portal.h`, `keyboard_handler.h`) to prevent multiple-definition linker errors and save flash
- Fixed `rgb_effect_task_handle` declared without `extern` in header, causing tentative definitions on every include
- Fixed `strncpy` not null-terminating when SSID is exactly 32 bytes in PineAP detection
- Fixed include guard mismatches: `commandline.h` used `COMMAND_H`, `gps_logger.h` used `WARDRIVING_CSV_H`
- Fixed Kconfig typo: "Device Detials" to "Device Details"
- Replaced hardcoded GPIO pin 24 with `CONFIG_INFRARED_LED_PIN` in infrared manager (poltergeist template)
- Fixed status bar view titles and the Ghostchi level text clipping: the title now sits further left in a smaller font, and the level label can no longer be squeezed out by the status icons
- Fixed RC5/RC5X transmit sending no data: `send_rmt` hardcoded mark/space levels, discarding the encoder's Manchester-encoded level sequence; added per-timing level pass-through so RC5, RC5X, and RC6 signals transmit and decode correctly
- Fixed OTA download task stack 4× under-sized (3 KB effective instead of 12 KB) on PSRAM boards: ESP-IDF FreeRTOS treats `xTaskCreateStatic` depth as bytes, not words
- Fixed peer OTA rx worker stack 4× under-sized (1.5 KB effective instead of 6 KB), same root cause
- Fixed BLE bridge task stack 4× under-sized from the same word/byte confusion
- Fixed `arp_scan` NULL-pointer dereference when printing results after transferring `ctx->hosts` to `g_arp_results`
- Fixed `ndef_builder_vcard` stack buffer overflow when combined name+phone+email exceeds 384 bytes (snprintf size underflow on unsigned subtraction)
- Fixed standalone station scan silently capturing nothing: `esp_wifi_stop()` was called before `esp_wifi_set_promiscuous(true)` without a matching `esp_wifi_start()`
- Fixed SNMP walk infinite loop against a responder that repeats its OID: added 10,000-entry iteration cap
- Fixed CLI `watch` task deterministic stack overflow (4 KB task calling `handle_tail_cmd` which needs 8 KB+); increased to 12 KB
- Fixed unbounded `source` script recursion crashing the console task (~4 nested levels overflow the serial stack); added depth cap of 4
- Fixed unbounded `peer:` command prefix recursion in `handle_serial_command` (~7 nested prefixes overflow the stack); added depth cap of 4
- Fixed EMV Track 2 separator detection missing byte 0xD0 exactly and all even-nibble PAN cases; now handles both odd-nibble (0xD in high nibble) and even-nibble (0xD in low nibble) separator alignments
- Added JSON string escaping for all attacker-controlled fields (hostname, fingerprint name/device_type/protocol/service/os, poison domain/cookie/cred) in Ethernet JSONL scan exports
- Moved scan, attack, cloud, and pcap-writer task stacks to PSRAM-preferred allocation via `xTaskCreate_psram()` helper (PSRAM first, internal fallback), freeing ~120 KB of internal RAM on PSRAM boards
- Potentially fixed intermittent Banshee C5 white-screen or reboot-loop failures during shared display/SD SPI handoff
- Fixed display resume crashes after shared SPI SD mounts on C5 boards
- Fixed asset pack icons showing as blank/corrupted on no-PSRAM boards when a screen displayed more distinct icons than the icon cache could hold
- Fixed asset pack switch crashing the Cardputer with a stack overflow in the `pack_switch` task
- Fixed external RTC time persistence on boards with `CONFIG_HAS_RTC_CLOCK`: PCF8563 month/year were written to the wrong registers (corrupting stored dates), boot restore treated UTC time as local (shifting the clock by the timezone offset), and GPS fixes weren't saved to the RTC at all
- Fixed CYD display freezes after a missing SD card probe by retaining the SD SPI3 bus on classic ESP32 boards
- Fixed wardriving screen GPS speed flickering to 0 when using peer GPS: the wardrive stream handler was clobbering the peer fix snapshot with speed=0 on every WiFi observation, racing with the GPS stream that carried the real speed
- Reduced status bar icon sizes and increased spacing between icons for a cleaner look

### Other Changes
- Added a "Sun Mode" toggle in Settings > Display for outdoor visibility: switches to a white background with black text and forces max brightness, restoring your previous brightness when turned back off
- Smoothed NRF24 frequency analyzer channel levels (local and GhostLink peer scans) to reduce graph jitter from the RPD carrier-detect readings
- Reduced heap fragmentation in packet monitoring, Cardputer keyboard input, BLE GATT reads, mDNS, and SD directory browsing
- Shared terminal and WebUI history to remove the duplicate AP log buffer
- Freed the wardriving CSV line buffer when logging stops
- Freed PCAP staging resources when capture stops
- Freed the HTTP streaming buffer when the web server stops
- Show the native SD-app PSRAM warning only once per boot
- Coalesce duplicate toast notifications and their haptic feedback
- Asset pack icon cache now dedupes by image content instead of file path, so packs reusing the same artwork across icons use a single cache slot
- Converted eight built-in menu icons to compact A4 masks and added scaled A4 rendering support
- Shortened "Native SD apps require PSRAM" toast duration so it dismisses faster
- Moved large scan and UI buffers to PSRAM to free internal RAM on PSRAM boards
- Reduced Terminal memory use on no-PSRAM boards by sharing CLI history with the rendered line cache
- Reduced SD card SPI DMA and VFS memory footprint for no-PSRAM boards
- Hardened Evil Portal request handling against malformed and high-rate client traffic
- Added per-client rate limiting for Evil Portal DNS and HTTP requests to prevent floods from exhausting heap or socket descriptors
- Shortened Evil Portal socket timeouts and downgraded verbose portal logs to debug level
- Reworked Evil Portal input capture to record full field values from inputs, textareas, selects, and browser autofill via debounced `sendBeacon` instead of per-keystroke XHR

### Docs
- Restructured getting-started docs with dedicated pages for installation, first scan, manual flashing, Flipper flashing, and control methods
- Added new WiFi docs pages for connecting, LAN discovery, port scanning, and environment sweep
- Moved GhostScript docs to its own top-level section and updated GBT and native SD app docs

## Revival v2.1-pre4
- Made Cloud Store catalogs grow on demand with paged browsing, supporting up to 32 apps, asset packs, and scripts per type on PSRAM boards
- Preserved script permissions and memory limits in Cloud Store-installed GhostScript manifests
- Stopped allocating the native app registry on no-PSRAM boards
- Reduced standalone `.gsb` launch peak by up to 8 KiB and freed 384-byte task arguments before execution
- Removed about 0.3 KiB of typical Lua allocator overhead and fixed allocator alignment/accounting
- Fixed input and event-wait bookkeeping that could consume the full 16-24 KiB Lua quota over time
- Fixed the GhostScript browser selecting a different script after returning from a run
- Freed about 1 KiB of Apps gallery storage before launching apps and fixed accumulating options-view style allocations

## Revival v2.0.0 - 2026-06-29

### Added

#### Security & Lock Screen
- Added PIN lock screen with lock on wake and auto-lock settings
- Added Ghostchi companion lockscreen mode with no-PIN support and a global mood system

#### Native SD Apps & App Gallery
- Added native SD apps loaded from the SD card with permissions, scoped storage, and launch-failure quarantine
- Added App Gallery for discovering and launching SD apps with custom icons and accent colors
- Added `apps` CLI to list, reload, inspect, launch, stop, and reset native SD apps
- Added categorical submenus to the app gallery, grouping native SD apps by their manifest `category` field
- Added native app SDK/docs and example apps for Device Inspector and ESP32 Finder
- Added a separate `nrf24` native SD app permission
- Added small native SD app helpers for capability checks and SubGHz replay
- Added Ghost Build Tool (`gbt`) for scaffolding, building, and packaging apps and firmware

#### SD Browser & File Management
- Added SD Browser app for paginated file/folder browsing, rename, delete, text preview, and file copy/move operations
- Added Paste Here / Cancel File Op rows so SD browser file operations work across touch, keyboard, joystick, and encoder controls

#### WiFi & Networking
- Added WiFi Airspace Monitor with realtime packet/threat insights, fast channel hopping, suspect device cards, adaptive channel dwell, EWMA baseline, and packets/sec sparkline
- Added WPA3 compliance checker to WiFi > Scan & Select menu and CLI (`wpa3check`)
- Added SSH Scan, NetBIOS Scan, HTTP Banner Scan, and SNMP Probe to WiFi > Network menu and CLI
- Added per-host keyboard-input variants ("Scan SSH Host...", etc.) for targeted scanning
- Added `capture -channel <n>` to lock WiFi capture modes and 802.15.4 captures to a fixed channel
- Added `wdstream` CLI streaming for companion-app wardriving without device GPS, SD, CSV, or PCAP capture requirements
- Added spinner/detail view flow for ARP Scan Network, mDNS Discovery, and Environment Sweep with async scanning, paginated result lists, and per-item detail views
- Improved mDNS scan: deduplicates devices across services, null-safe when run from CLI, removed unused 2KB stack allocation

#### Bluetooth & BLE
- Added BLE advertisement scan option in the Bluetooth menu
- Added OUI prefix/vendor filtered BLE device scanning with RGB match pulses
- Added scanning spinner and details view for `GATT Scan` in the Bluetooth menu
- Added RSSI meter view for real-time signal strength tracking with a pulsing ring indicator
- Added support for assigning a GhostLink connected chip to act as a blebridge between a main chip and the android companion app

#### GPS & Wardriving
- Added GPS baud auto-detect and always-visible runtime GPS settings for boards without compile-time GPS enabled
- Added runtime-configurable GPS baud rate (settings, `gpsbaud` CLI, WebUI, and a new GPS menu under Scans & Data)
- Added Wardriving settings for per-chip hop intervals and weighted 5GHz channel hopping, synced over GhostLink

#### Hardware Support
- Added support for the LilyGo T-Dongle-S3 with WebUI BadUSB support
- Added support for the LilyGo T-Dongle-C5
- Added Marauder V8 hardware build - @H4W9
- Added Pancake C5 hardware build - @H4W9
- Added DRV2605 haptic feedback for the S3TWatch
- Added ENV-III support to the Cardputer + Cardputer ADV

#### BadUSB & USB
- Added a trackpad option to BadUSB
- Added proper touch support to the BadUSB view
- Added USB Keyboard Mode for forwarding on-device keystrokes over USB HID
- Added mouse jiggler to BadUSB
- Added `badusb type_char` CLI command for typing single ASCII characters

#### Custom Asset Packs & Theming
- Added custom asset packs loaded from SD with custom icons, colors, and backgrounds
- Added a "Card Background" setting in Appearance to hide the card surface/shadow/border on main menu and apps gallery items
- Added an "Invert Carousel" setting in Appearance that flips the slide direction
- Added a "Terminal Font" setting in Display to change the font size of the terminal view (Small / Normal / Large)

#### UI & Accessibility
- Added toast notification system
- Added epilepsy warning toggle (disables flashing LED effect popups)
- Added accessibility settings: Font size (Small/Normal/Large), High contrast mode, Reduced motion, Input repeat speed (Slow/Normal/Fast)
- Added on-device AP/STA SSID and password editing under Settings → Wi-Fi
- Added Timezone quick-edit under Settings → Wi-Fi
- Added subcategories to the Settings view and re-organised the options
- Added reusable LVGL confirmation popups for dangerous UI actions
- Added reusable select overlay for option picker rows
- Added fullscreen option to `popup_create_container` so popups can fill the screen under the status bar
- Added a small progress bar to the boot screen so SD mount, asset pack load, and app scan no longer happen while using the device
- Added Display Timeout options: 15s, 2m, and 5m
- Added on-device PCAP browser with hc22000 markers under WiFi > Capture
- Added PCAP to hc22000 export on displays

#### Ghostchi Companion
- Added 5 new Ghostchi images. "Banshee" by @pr3
- Added Ghostchi activity counters to state file
- Added level-up toast notification
- Added persistent level badge in the status bar visible on every screen
- Added Settings > Info with a read-only device, runtime, build, and credits page sourced from `chipinfo`
- Expanded Ghostchi levels from 10 to 50 with a smooth quadratic curve
- Expanded Ghostchi XP system from 3 sources to 27 across WiFi, BLE, GPS, IR, NFC, SubGHz, BadUSB, attacks, scans, games, plugins, and settings

### Changed

#### UI & Display
- Increased LVGL display refresh target from 30 FPS to 60 FPS
- Polished status bar with cleaner accent border, brighter title with truncation, softer semantic status colors
- Polished detail view to match main menu styling
- Polished number pad screen with theme-aware lockscreen-style numpad grid
- Polished keyboard screen with theme-aware key styling and accent highlights
- Changed startup logo to new logo and removed "GhostESP: Revival" text from splash screen
- Removed border from popups
- Replaced DEL label with the backspace symbol
- Skipping the setup wizard now defaults the main menu to List layout
- Replaced on/off settings rows with iOS style toggles
- Reduced status bar title font size to body font for a cleaner look
- Apps Gallery now renders the asset pack background image
- Larger lockscreen numpad on taller displays with solid keys and proper contrast
- Lockscreen shows the asset pack background instead of a solid color
- Made command lookup case-insensitive (`SCANAP` now works)
- Reduced the brief white screen before splash by keeping the backlight off until the first splash frame is drawn
- Removed "SD card mounted" toast during boot
- Removed "IR sent" toast notification
- Removed 2 redundant toast notifications when opening native SD apps
- Slimmed and rounded the terminal's bottom touch controls to match the rest of the UI
- Stopped logging unknown commands to command history so typos no longer pollute arrow-up
- Themed the terminal's input bar and back button via the palette instead of hardcoded colors
- Reduced `sd tree` walker memory from ~16 KB to ~600 B and made it work on devices without PSRAM
- Plugin icons now recolor to their manifest accent color, unless overridden by an asset pack

#### Performance & Optimization
- Optimized WiFi, BLE, mbedTLS, and LWIP buffer sizes for all non-PSRAM configs
- Compressed OUI list to save free up flash space
- Cached the DualComm line check per row to keep the split-view terminal smooth
- ESP32-C5 native SD apps now execute their code from flash (XIP) instead of internal RAM, lifting the ~25 KB executable-size ceiling on app code for C5 boards with more than 4 MB flash

#### Architecture & Code Quality
- Split commandline god file into separate organised files
- Wake/auto-lock now floats the lockscreen as an overlay instead of switching views, so active captures (wardriving, sniffing) keep running while locked
- Made native SD app launch failures diagnostic-only instead of quarantining apps
- Tightened native SD app storage scope and path checks
- Extracted the repeated tiered popup sizing math into `popup_calc_size` helpers
- Moved the per-view "mount SD on demand" boilerplate into shared `sd_card_jit_begin` / `sd_card_jit_end` helpers
- Added `gui_screen_create_root_default` and a `GUI_DEFAULT_BG_COLOR` constant for views that want a flat non-theme background
- Easy Learn in the Infrared menu now uses an iOS-style toggle row like the settings ones
- Completely redesigned the WebUI and added a dedicated BadUSB page
- Sorry about making you wait 7 seconds to shut down your TEmbed, that's now down to 4
- Improved rotary encoder: raised debounce to 3 ms, added quadrature transition validation, capped pending step accumulation, and moved direct-GPIO encoder sampling to a dedicated 1 kHz task
- Renamed "Default" named theme to OG since it wasn't actually the default theme
- Renamed "Scan LAN Devices" to "mDNS Discovery" in the WiFi > Network menu for accuracy
- Removed "Select LAN" from the WiFi > Network menu (it was a duplicate of Select AP)
- Made BadUSB keyboard startup async so GhostLink doesn't block
- Changed BadUSB popup to wait for actual VSENSE state before showing "Waiting for USB"
- Made `generate_uuid` take a caller-owned buffer so concurrent DIAL binds can no longer race on its static storage
- Hardened `serial_task` against OOM by checking the UART buffer allocation
- Made `glog` copy the formatted line to a heap buffer before unlocking and fixed the deferred-queue leak when defer mode is turned off
- Replaced the `VLA` in `get_query_param` with a fixed 512-byte buffer
- Replaced a few `sprintf`/`strcpy`/`strcat` sites in GPS coordinate formatting, aerial detector init, and the AP query-param helper with bounded variants
- Replaced the placeholder ghost sprite on small screens with the real GhostESP logo

#### Hardened WebUI
- API and camera endpoints now enforce Digest auth when enabled
- Passwords no longer leak from settings GET
- AP password validation accepts exactly 8 characters
- SD upload path/filename/size checks are unified
- Settings_save surfaces NVS write errors

### Fixed
- Fixed number pad touchscreen input
- Fixed apps gallery not respecting the "Item Borders" setting
- Fixed apps gallery list view icon misalignment and tiling artifact
- Fixed Cardputer (non-ADV) keyboard spamming input by emitting key-release events so the global key-repeat timer stops when keys are let go
- Fixed touch taps on the GATT device detail view leaking through to the underlying scan list
- Fixed GPS failing to initialize on T-Deck: GPS now temporarily takes over the shared UART1 from the serial command interface while running and hands it back on stop
- Wardriving/GPS view is now touch-scrollable with a bottom control bar matching the other views
- Fixed timed Wi-Fi scans returning 0 results when the auto-reconnect timer reconfigured STA mid-scan
- Fixed a null-pointer crash in `lv_async_call` when invoked from non-LVGL tasks by initializing the callback before the timer goes live
- Fixed NM-CYD-C5 internal RAM exhaustion and SD app loading (disabled memory protection so the ELF loader can allocate executable memory)
- Fixed a crash after a successful NFC scan caused by `free()`-ing a static NDEF pool slot instead of returning it to the pool
- Fixed the CYD 2.4" display freezing when no SD card is inserted by no longer tearing down SD's SPI3 bus on mount failure or unmount
- Fixed serial console staying dead after stopping BadUSB on the S3. The native USB-Serial-JTAG driver is now re-installed once TinyUSB releases the bus
- Fixed asset pack auto-selecting an installed pack on boot when the user hadn't picked one, so a pack dropped on the SD is no longer made active without manual selection
- Fixed "Scan SSH" menu item previously failing due to missing required argument
- Fixed asset pack background image not properly filling the screen on some devices
- Fixed GhostNet WebUI staying down after `scan -t`, `scanap`, and `scansta` failures or early stops
- Fixed `stopscan` always bringing the AP back, even when the Wi-Fi driver restart errored
- Fixed `station_scan_stop` to restore GhostNet so the WebUI returns when stopping a station scan
- Fixed Banshee (Wired Hatters) 100% screen brightness appearing dimmer than 90% caused by LEDC PWM producing a flat DC signal instead of a waveform at duty=0
- Fixed inconsistent vertical alignment of asset pack icons in the main menu and apps gallery grid cards by anchoring icons to a fixed top padding instead of centering their bounding boxes
- Fixed double-free in DIAL `send_command` where `url_params`/`body_params` were freed before `goto cleanup` and then freed again at the cleanup label
- Fixed leak of `full_url` and all preceding allocations on the OOM path in DIAL `send_command` by routing through the cleanup label
- Fixed unchecked `esp_http_client_init` in DIAL `send_command` that could crash if the client handle came back NULL
- Fixed leaks of `g_app_url` when the DIAL `Application-Url` header arrived more than once
- Fixed realloc-to-same-pointer in M5Stack keyboard, MIFARE Classic universal command loading, and Chameleon/MIFARE cache init paths so an OOM no longer leaks the previous allocation and then dereferences NULL
- Freed `filepath` on every exit path of `sinkhole_download_task` so each blocklist download no longer leaks it
- Made `sd_cli_cleanup` free the `strdup`'d path table so repeated `sd ls` calls stop leaking
- Cleared partial PRF output on allocation failure so a future caller of `wpa_derive_ptk` never sees stale data on `false`
- Fixed panic in Infrared when reopening the Remotes list. `clear_ir_file_paths` now always resets `ir_file_capacity`, and the duplicate manual frees in `back_event_cb` and `infrared_view_destroy` route through it so `load_ir_file_list_from_dir` can no longer write through a stale-but-freed pointer
- Fixed `chameleon` (no args) double-printing its help on the on-screen terminal
- Fixed `help` (no args) printing the category list twice
- Fixed `select -a 1,2,3` corrupting later `select` calls by switching to a re-entrant, non-mutating tokenizer
- Fixed audio decode task leaking decoder buffers on deinit by adding a self-exit semaphore so the task can free its own state
- Fixed audio receiver ring buffer producer/consumer race by serializing `head`/`tail` updates with a `portMUX`
- Fixed BLE stack restart racing NimBLE stop/deinit
- Fixed DIAL bind overflowing `gsession`/`SID`/`listID` with server-controlled bytes
- Fixed DNS sinkhole silently clobbering forward ring slot 0 under load
- Fixed duplicate `powerprinter` entry in `help printer`
- Fixed Invert Colors setting not refreshing the screen until something else triggered a redraw
- Fixed Waveshare 7-inch (ESP32-S3-Touch-LCD-7) backlight and touch
- Fixed Wigle "already uploaded" check stalling uploads on long histories by caching the on-disk log in RAM
- Fixed integer overflow in mic visualizer reactive color calculation causing incorrect colors at high volumes
- Fixed mic visualizer bloom and peak meter decay being inverted (higher smoothing = faster decay)
- Fixed mic visualizer waveform trail rendering backwards (oldest samples were brightest)
- Fixed mic visualizer spectrum distance fade underflow on strips longer than ~60 LEDs
- Wired up mic smoothing setting to band release rate so it affects all visualizer modes
- Fixed mic visualizer mode state not resetting on first frame when starting in default mode
- Moved NRF24, SubGHz, and MIC visualizer task stacks to PSRAM to fix internal RAM exhaustion
- Moved Goertzel ring buffers and MIC sample buffer to PSRAM to free up internal RAM
- Fixed LoadProhibited crash on deferred SD card init failure
- WiFi now auto-reconnects after BLE/Chameleon suspend, with bounded retry on involuntary disconnects
- Adjusted lockscreen layout for cardputer screens
- Fixed SD browser keyboard input causing rapid scrolling on cardputer key hold
- Fixed inconsistent icon sizing and alignment in app gallery and main menu grid layouts across different displays
- Fixed inability to exit SD app runner when app fails to load
- Fixed gbt generating planar RGB565A8 icons instead of interleaved format expected by LVGL
- Fixed random boot crashes on JIT mount boards caused by the SD card taking over the shared SPI bus while the screen was still mid-draw
- Fixed `nmea_parser` stack overflow crashes by raising the GPS parser task stack
- Fixed runtime GPS parsing by enabling NMEA decoding without `CONFIG_HAS_GPS` and improving UART drain/update handling
- Fixed S3TWatch RTC init on ESP-IDF 6.0 by avoiding an `rtc_init` symbol collision that could hang WiFi scans
- Fixed app gallery grid keyboard navigation to move by row like the main menu
- Fixed inconsistent character spacing and centering in the encoder-only keyboard view and swapped backspace/enter for LVGL symbols
- Fixed flashing solid grey behind the IR transmit epilepsy warning by deferring the transmitting popup until after the warning fades
- Fixed universal remote button sends over-awarding Ghostchi XP (1 XP per button press instead of 4 XP per signal)
- Fixed SHUTTING DOWN popup being wider than its text on T-Embed by auto-sizing popups to fit their content
- Fixed detail view wrap-around and selection behaving erratically with joystick/encoder input
- Fixed lockscreen rejecting correct PIN when Wi-Fi is off (BLE scan stops STA); key now uses stable STA MAC with legacy fallback
- Fixed wrong PIN after lock-on-wake; wake input is consumed and queued input is briefly ignored after lock
- Fixed detail view being orphaned over the rebuilt main menu after dim+lock; rebuilt after unlock
- Fixed `scan_status` spinner lingering on `lv_layer_top()` across lockscreen entry
- Fixed main menu touch scrolling feeling less responsive than settings
- Fixed fullscreen LVGL popups not filling the runtime display area below the status bar on some configs
- Fixed saved RGB pins being ignored at boot on boards with `CONFIG_NUM_LEDS=0` when the LED count was never set, causing `setrgbpins` to appear to not persist across reboots
- Fixed BLE scans over-awarding Ghostchi XP
- Fixed BLE scan RGB getting stuck instead of pulsing
- Fixed GitHub Actions merged binary using wrong bootloader offset for ESP32-S3/C3/C5/C6 targets

## Revival v2.0-pre9
- ESP32-C5 native SD apps now execute their code from flash (XIP) instead of internal RAM, lifting the ~25 KB executable-size ceiling on app code for C5 boards with more than 4 MB flash
- Fixes for Marauder V8 and Pancake configs - @H4W9
- Removed 2 redundant toast notifications when opening native SD apps
- Hardened WebUI: API and camera endpoints now enforce Digest auth when enabled, passwords no longer leak from settings GET, AP password validation accepts exactly 8 characters, SD upload path/filename/size checks are unified, and settings_save surfaces NVS write errors
- Fixed GitHub Actions merged binary using wrong bootloader offset for ESP32-S3/C3/C5/C6 targets
- Added spinner/detail view flow for ARP Scan Network, mDNS Discovery, and Environment Sweep (replaces terminal dump with scan progress spinner, paginated result list, and per-item detail view)
- Improved mDNS scan: deduplicates devices across services, null-safe when run from CLI, removed unused 2KB stack allocation

## Revival v2.0-pre8

### Changed
 - BLE advertiser and GATT device tracking now use the same live RSSI meter view as Wi-Fi AP tracking instead of the terminal
 - Airspace Monitor is smarter: adaptive channel dwell on kick frames for accurate rates, a learned EWMA baseline so detection adapts to the local environment, a single unified threat/insight engine, and an on-screen packets/sec sparkline

### Fixed
 - Fixed Cardputer (non-ADV) keyboard spamming input by emitting key-release events so the global key-repeat timer stops when keys are let go
 - Fixed touch taps on the GATT device detail view leaking through to the underlying scan list
 - Fixed GPS failing to initialize on T-Deck: GPS now temporarily takes over the shared UART1 from the serial command interface while running and hands it back on stop
 - Wardriving/GPS view is now touch-scrollable with a bottom control bar matching the other views
 - Fixed timed Wi-Fi scans returning 0 results when the auto-reconnect timer reconfigured STA mid-scan
 - Fixed a null-pointer crash in `lv_async_call` when invoked from non-LVGL tasks by initializing the callback before the timer goes live
 - Fixed NM-CYD-C5 internal RAM exhaustio and SD app loading (disabled memory protection so the ELF loader can allocate executable memory)
 - Fixed a crash after a successful NFC scan caused by `free()`-ing a static NDEF pool slot instead of returning it to the pool
 - Fixed the CYD 2.4" display freezing when no SD card is inserted by no longer tearing down SD's SPI3 bus on mount failure or unmount


## Revival v2.0-pre7

### Added
 - Added categorical submenus to the app gallery, grouping native SD apps by their manifest `category` field.
 - Added DRV2605 haptic feedback for the S3TWatch
 - Added GPS baud auto-detect and always-visible runtime GPS settings for boards without compile-time GPS enabled
 - Added ENV-III support to the Cardputer + Cardputer ADV
 - Added on-device AP/STA SSID and password editing under Settings → Wi-Fi (was WebUI/CLI/setup-wizard only)
 - Added Timezone quick-edit under Settings → Wi-Fi (was setup-wizard only)

### Changed
 - Optimized WiFi, BLE, mbedTLS, and LWIP buffer sizes for all non-PSRAM configs
 - Split commandline god file into separate organised files
 - Added a 250ms guard to prevent closing the wardriving view when opening it
 - Wake/auto-lock now floats the lockscreen as an overlay instead of switching views, so active captures (wardriving, sniffing) keep running while locked
 - Set default Cardputer + Cardputer ADV GPS pin to GPIO1
 - Allow scrolling the wardriving view with keyboard arrows
 - Changed App Gallery Icon

### Fixed
 - Adjusted lockscreen layout for cardputer screens
 - Fixed SD browser keyboard input causing rapid scrolling on cardputer key hold
 - Fixed inconsistent icon sizing and alignment in app gallery and main menu grid layouts across different displays
 - Fixed inability to exit SD app runner when app fails to load
 - Fixed gbt generating planar RGB565A8 icons instead of interleaved format expected by LVGL
 - Fixed random boot crashes on JIT mount boards caused by the SD card taking over the shared SPI bus while the screen was still mid-draw
 - Fixed `nmea_parser` stack overflow crashes by raising the GPS parser task stack 
 - Fixed runtime GPS parsing by enabling NMEA decoding without `CONFIG_HAS_GPS` and improving UART drain/update handling
 - Fixed S3TWatch RTC init on ESP-IDF 6.0 by avoiding an `rtc_init` symbol collision that could hang WiFi scans
 - Fixed app gallery grid keyboard navigation to move by row like the main menu
 - Fixed inconsistent character spacing and centering in the encoder-only keyboard view and swapped backspace/enter for LVGL symbols
 - Fixed flashing solid grey behind the IR transmit epilepsy warning by deferring the transmitting popup until after the warning fades
 - Fixed universal remote button sends over-awarding Ghostchi XP (1 XP per button press instead of 4 XP per signal)
 - Fixed SHUTTING DOWN popup being wider than its text on T-Embed by auto-sizing popups to fit their content

## Revival v2.0-pre6

### Added
 - Add Marauder V8 hardware build - @H4W9
 - Add Pancake C5 hardware build - @H4W9
 - Added a BLE advertisement scan option in the Bluetooth menu
 - Added OUI prefix/vendor filtered BLE device scanning with RGB match pulses
 - Added scanning spinner and details view for `GATT Scan` in the Bluetooth menu (matches the Detect Devices and Advertiser Scan flows)
 - Added Ghostchi companion lockscreen mode with no-PIN support and a global mood system
 - Added SD browser support for viewing text file previews and staging file copy/move operations
 - Added Paste Here / Cancel File Op rows so SD browser file operations work across touch, keyboard, joystick, and encoder controls
- Added reusable LVGL confirmation popups for dangerous UI actions
 - Added a runtime-configurable GPS baud rate (settings, `gpsbaud` CLI, WebUI, and a new GPS menu under Scans & Data)
 - Added `wdstream` CLI streaming for companion-app wardriving without device GPS, SD, CSV, or PCAP capture requirements
 - Added RSSI meter view for real-time signal strength tracking with a pulsing ring indicator

### Changed
 - Renamed "Default" named theme to OG since it wasn't actually the default theme

### Fixed
 - Fixed BLE scans over-awarding Ghostchi XP
 - Fixed BLE scan RGB getting stuck instead of pulsing
 - Fixed detail view wrap-around and selection behaving erratically with joystick/encoder input
 - Fixed lockscreen rejecting correct PIN when Wi-Fi is off (BLE scan stops STA); key now uses stable STA MAC with legacy fallback
 - Fixed wrong PIN after lock-on-wake; wake input is consumed and queued input is briefly ignored after lock
 - Fixed detail view being orphaned over the rebuilt main menu after dim+lock; rebuilt after unlock
 - Fixed `scan_status` spinner lingering on `lv_layer_top()` across lockscreen entry
 - Fixed main menu touch scrolling feeling less responsive than settings
 - Fixed fullscreen LVGL popups not filling the runtime display area below the status bar on some configs
 - Fixed saved RGB pins being ignored at boot on boards with `CONFIG_NUM_LEDS=0` when the LED count was never set, causing `setrgbpins` to appear to not persist across reboots

## Revival v2.0-pre5

### Added
 - Added support for assigning a GhostLink connected chip to act as a blebridge between a main chip and the android companion app
 - Added support for the LilyGo T-Dongle-S3 with WebUI BadUSB support
 - Added support for the LilyGo T-Dongle-C5
 - Added WPA3 compliance checker to WiFi > Scan & Select menu and CLI (`wpa3check`)
 - Added SSH Scan, NetBIOS Scan, HTTP Banner Scan, and SNMP Probe to WiFi > Network menu and CLI
 - Added per-host keyboard-input variants ("Scan SSH Host...", etc.) for targeted scanning
 - Added a trackpad option to BadUSB
 - Added proper touch support to the BadUSB view
 - Added USB Keyboard Mode for forwarding on-device keystrokes over USB HID
 - Added mouse jiggler to BadUSB
 - Added `badusb type_char` CLI command for typing single ASCII characters
 - Added a separate `nrf24` native SD app permission
 - Added small native SD app helpers for capability checks and SubGHz replay
 - Added spinlock protection to the handshake tracking table, BLE wardrive dedupe counters, and wardrive channel-hop state
 - Added NULL-check + reset paths to the six `calloc`s in `mfc_cache_begin` and `cu_mfc_cache_begin`
 - Added a "Card Background" setting in Appearance to hide the card surface/shadow/border on main menu and apps gallery items
 - Added an "Invert Carousel" setting in Appearance that flips the slide direction
 - Added a "Terminal Font" setting in Display to change the font size of the terminal view (Small / Normal / Large)
 - Added a reusable select overlay for option picker rows
 - Added fullscreen option to `popup_create_container` so popups can fill the screen under the status bar; opted NFC, SubGHz, Infrared, BadUSB, and WiGLE popups in
 - Added subcategories to the Settings view and re-organised the options
 - Added `capture -channel <n>` to lock WiFi capture modes and 802.15.4 captures to a fixed channel

### Changed

 - Completely redesigned the WebUI and added a dedicated BadUSB page
 - Sorry about making you wait 7 seconds to shut down your TEmbed, that's now down to 4
 - Improved rotary encoder: raised debounce to 3 ms, added quadrature transition validation, capped pending step accumulation, and moved direct-GPIO encoder sampling to a dedicated 1 kHz task
 - Renamed "Scan LAN Devices" to "mDNS Discovery" in the WiFi > Network menu for accuracy
 - Removed "Select LAN" from the WiFi > Network menu (it was a duplicate of Select AP)
 - BadUSB view now uses the standard touch bar styling
 - Made BadUSB keyboard startup async so GhostLink doesn't block
 - Changed BadUSB popup to wait for actual VSENSE state before showing "Waiting for USB"
 - Made native SD app launch failures diagnostic-only instead of quarantining apps
 - Tightened native SD app storage scope and path checks
 - Made `generate_uuid` take a caller-owned buffer so concurrent DIAL binds can no longer race on its static storage
 - Hardened `serial_task` against OOM by checking the UART buffer allocation
 - Made `glog` copy the formatted line to a heap buffer before unlocking and fixed the deferred-queue leak when defer mode is turned off
 - Replaced the `VLA` in `get_query_param` with a fixed 512-byte buffer
 - Replaced a few `sprintf`/`strcpy`/`strcat` sites in GPS coordinate formatting, aerial detector init, and the AP query-param helper with bounded variants
 - Replaced the placeholder ghost sprite on small screens with the real GhostESP logo
 - Extracted the repeated tiered popup sizing math into `popup_calc_size` helpers
 - Moved the per-view "mount SD on demand" boilerplate into shared `sd_card_jit_begin` / `sd_card_jit_end` helpers used by BadUSB, Infrared, NFC, and SD app views
 - Added `gui_screen_create_root_default` and a `GUI_DEFAULT_BG_COLOR` constant for views that want a flat non-theme background
 - Easy Learn in the Infrared menu now uses an iOS-style toggle row like the settings ones
 - Removed "IR sent" toast notification
 - Compressed OUI list to save free up flash space

### Fixed

 - Fixed serial console staying dead after stopping BadUSB on the S3. The native USB-Serial-JTAG driver is now re-installed once TinyUSB releases the bus
 - Fixed asset pack auto-selecting an installed pack on boot when the user hadn't picked one, so a pack dropped on the SD is no longer made active without manual selection
 - Fixed "Scan SSH" menu item previously failing due to missing required argument
 - Fixed asset pack background image not properly filling the screen on some devices
 - Fixed GhostNet WebUI staying down after `scan -t`, `scanap`, and `scansta` failures or early stops
 - Fixed `stopscan` always bringing the AP back, even when the Wi-Fi driver restart errored
 - Fixed `station_scan_stop` to restore GhostNet so the WebUI returns when stopping a station scan
 - Fixed Banshee (Wired Hatters) 100% screen brightness appearing dimmer than 90% caused by LEDC PWM producing a flat DC signal instead of a waveform at duty=0
 - Fixed inconsistent vertical alignment of asset pack icons in the main menu and apps gallery grid cards by anchoring icons to a fixed top padding instead of centering their bounding boxes
 - Fixed double-free in DIAL `send_command` where `url_params`/`body_params` were freed before `goto cleanup` and then freed again at the cleanup label
 - Fixed leak of `full_url` and all preceding allocations on the OOM path in DIAL `send_command` by routing through the cleanup label
 - Fixed unchecked `esp_http_client_init` in DIAL `send_command` that could crash if the client handle came back NULL
 - Fixed leaks of `g_app_url` when the DIAL `Application-Url` header arrived more than once
 - Fixed realloc-to-same-pointer in M5Stack keyboard, MIFARE Classic universal command loading, and Chameleon/MIFARE cache init paths so an OOM no longer leaks the previous allocation and then dereferences NULL
 - Freed `filepath` on every exit path of `sinkhole_download_task` so each blocklist download no longer leaks it
 - Made `sd_cli_cleanup` free the `strdup`'d path table so repeated `sd ls` calls stop leaking
 - Cleared partial PRF output on allocation failure so a future caller of `wpa_derive_ptk` never sees stale data on `false`
 - Fixed panic in Infrared when reopening the Remotes list. `clear_ir_file_paths` now always resets `ir_file_capacity`, and the duplicate manual frees in `back_event_cb` and `infrared_view_destroy` route through it so `load_ir_file_list_from_dir` can no longer write through a stale-but-freed pointer.



## Revival v2.0-pre4 - 2026-06-06

### Added
 - Added 5 new Ghostchi images. "Banshee" by @pr3
 - Added a small progress bar to the boot screen so SD mount, asset pack load, and app scan no longer happen while using the device
 - Added an SD Browser app for paginated file/folder browsing, rename, and delete actions
 - Added Ghostchi activity counters to state file
 - Added level-up toast notification
 - Added more Display Timeout options: 15s, 2m, and 5m
 - Added on-device PCAP browser with hc22000 markers under WiFi > Capture
 - Added PCAP to hc22000 export on displays
 - Added persistent level badge in the status bar visible on every screen
 - Added Settings > Info with a read-only device, runtime, build, and credits page sourced from `chipinfo`
 - Added Wardriving settings for per-chip hop intervals and weighted 5GHz channel hopping, synced over GhostLink

### Changed
 - Apps Gallery now renders the asset pack background image
 - Cached the DualComm line check per row to keep the split-view terminal smooth
 - Expanded Ghostchi levels from 10 to 50 with a smooth quadratic curve
 - Expanded Ghostchi XP system from 3 sources to 27 across WiFi, BLE, GPS, IR, NFC, SubGHz, BadUSB, attacks, scans, games, plugins, and settings
 - Larger lockscreen numpad on taller displays
 - Lockscreen numpad keys are now solid with proper contrast on focused buttons
 - Lockscreen prompt and PIN dots have a dark backdrop for readability
 - Lockscreen shows the asset pack background instead of a solid color
 - Made command lookup case-insensitive (`SCANAP` now works)
 - Made Ghostchi passive by default, added a passive/aggressive mode toggle on pages 1 & 2 of the Ghostchi view
 - Moved Clock, Compass, ENV-III, and Accelerometer from the main menu to the Apps Gallery
 - Plugin icons now recolor to their manifest accent color, unless overridden by an asset pack
 - Reduced `sd tree` walker memory from ~16 KB to ~600 B and made it work on devices without PSRAM
 - Reduced the brief white screen before splash by keeping the backlight off until the first splash frame is drawn
 - Removed "SD card mounted" toast during boot
 - Replaced on/off settings rows with iOS style toggles for a slightly nicer feel
 - Slimmed and rounded the terminal's bottom touch controls to match the rest of the UI
 - Stopped logging unknown commands to command history so typos no longer pollute arrow-up
 - Themed the terminal's input bar and back button via the palette instead of hardcoded colors

### Fixed
 - Fixed `chameleon` (no args) double-printing its help on the on-screen terminal
 - Fixed `help` (no args) printing the category list twice
 - Fixed `select -a 1,2,3` corrupting later `select` calls by switching to a re-entrant, non-mutating tokenizer
 - Fixed audio decode task leaking decoder buffers on deinit by adding a self-exit semaphore so the task can free its own state
 - Fixed audio receiver ring buffer producer/consumer race by serializing `head`/`tail` updates with a `portMUX`
 - Fixed BLE stack restart racing NimBLE stop/deinit
 - Fixed DIAL bind overflowing `gsession`/`SID`/`listID` with server-controlled bytes
 - Fixed DNS sinkhole silently clobbering forward ring slot 0 under load
 - Fixed duplicate `powerprinter` entry in `help printer`
 - Fixed Invert Colors setting not refreshing the screen until something else triggered a redraw
 - Fixed Waveshare 7-inch (ESP32-S3-Touch-LCD-7) backlight and touch
 - Fixed Wigle "already uploaded" check stalling uploads on long histories by caching the on-disk log in RAM

## Revival v2.0-pre3 - 2026-06-03

 - Added custom asset packs loaded from SD with custom icons, colors, and backgrounds.
 - Refactored main menu layout sizing and made portrait grid screens more compact
 - Added live drag scrolling (gated by the new "Touch Drag Scroll" setting. falls back to release on release when off)
 - Added "Touch Drag Scroll" toggle in Settings > Appearance (default ON, when off scrolling still works but updates only on release)
 - Reorganized docs sidebar into Wireless/Sensors/Apps & I/O/Developer categories and polished sidebar UI
 - Fixed integer overflow in mic visualizer reactive color calculation causing incorrect colors at high volumes
 - Fixed mic visualizer bloom and peak meter decay being inverted (higher smoothing = faster decay)
 - Fixed mic visualizer waveform trail rendering backwards (oldest samples were brightest)
 - Fixed mic visualizer spectrum distance fade underflow on strips longer than ~60 LEDs
 - Wired up mic smoothing setting to band release rate so it affects all visualizer modes
 - Fixed mic visualizer mode state not resetting on first frame when starting in default mode
 - Moved NRF24, SubGHz, and MIC visualizer task stacks to PSRAM to fix internal RAM exhaustion
 - Moved Goertzel ring buffers and MIC sample buffer to PSRAM to free up internal RAM
 - Bumped GhostBT to v0.2.2

## Revival v2.0-pre2 - 2026-06-01

 - Added WiFi Airspace Monitor with realtime packet/threat insights, fast channel hopping, and suspect device cards
 - Added native SD apps loaded from the SD card with permissions, scoped storage, and launch-failure quarantine
 - Added App Gallery for discovering and launching SD apps with custom icons and accent colors
 - Added `apps` CLI to list, reload, inspect, launch, stop, and reset native SD apps
 - Added Ghost Build Tool (`gbt`) for scaffolding, building, and packaging apps and firmware
 - Added native app SDK/docs and example apps for Device Inspector and ESP32 Finder
 - Lockscreen unlock now returns to the view that was active before auto-lock or wake-lock
 - Reduced status bar title font size to body font for a cleaner look
 - Fixed apps gallery list view icon misalignment and tiling artifact
 - Polished setup wizard styling and removed default button/card shadows
 - Added Home WiFi credential setup and clarified Device AP vs Home WiFi prompts
 - Fixed LoadProhibited crash on deferred SD card init failure
 - WiFi now auto-reconnects after BLE/Chameleon suspend, with bounded retry on involuntary disconnects
 - Added ENV-III sensor support (temperature, humidity, pressure) with on-screen UI - @Billi-Green
 - Added TLV320DAC3100 audio driver and Audio Player app with headphone detection and volume control - @Billi-Green
 - Moved Ethernet to a standalone main menu item - @Billi-Green
 - Added touch handling to ethernet view

## Revival v2.0-pre1 - 2026-05-21

 - Added PIN lock screen with lock on wake and auto-lock settings
 - Added toast notification system
 - Polished status bar with cleaner accent border, brighter title with truncation, softer semantic status colors
 - Changed startup logo to new logo and removed "GhostESP: Revival" text from splash screen
 - Removed border from popups
 - Polished detail view to match main menu styling
 - Polished number pad screen with theme-aware lockscreen-style numpad grid
 - Fixed number pad touchscreen input
 - Replaced DEL label with the backspace symbol
 - Polished keyboard screen with theme-aware key styling and accent highlights
 - Fixed apps gallery not respecting the "Item Borders" setting
 - Skipping the setup wizard now defaults the main menu to List layout
 - Increased LVGL display refresh target from 30 FPS to 60 FPS
 - Added accessibility settings:
   - Font size (Small/Normal/Large)
   - High contrast mode
   - Reduced motion
   - Input repeat speed (Slow/Normal/Fast)
 - Added epilepsy warning toggle (disables flashing LED effect popups)

## Revival v1.9.10 - 2026-05-18

- Fixed Settings submenus on Cardputer ADV showing shifted content from Network onward when Status Display is not compiled in
- Fixed Cardputer grid card navigation not scrolling down when keyboard/encoder selection moves below the visible rows
- Fixed the darkest menu background shade appearing green on RGB565 displays by using true black for that shade
- Fixed NULL pointer crash in LVGL display refresh during WiFi scan by disabling pop-in zoom animation
- Fixed potential boot hang on splash screen with no SD card inserted on shared-SPI boards
- Removed compile-time GPS menu gate. GPS menu is now always visible since the RX pin can be set at runtime via `gpspin` command

## Revival v1.9.9 - 2026-05-12

### Added
- Added DNS sinkhole with blocklist-based NXDOMAIN blocking, parent-domain matching, CNAME inspection, iOS/DoH bypass canaries, query logging, and PSRAM/no-PSRAM lookup paths
- Added CC1101 SubGHz (The Wired Hatter's Banshee only atm) support with frequency analyzer, capture/replay and multi-band scanning (315/390/433.92/868.35/915 MHz)
- Added passive jamming detection engine to the NRF24 frequency analyzer that identifies known 2.4GHz threat signatures in real time during normal spectrum scanning
- Added support for Seeed Studio XIAO ESP32-S3 Sense with motion detection
- Added support for Seeed Studio XIAO ESP32-C5
- Added support for Seeed Studio XIAO ESP32-S3
- Added live MJPEG camera stream viewable at http://ghostesp.local/camera
- Added SD card backup functionality for settings - @tototo31
- Added camera motion detector CLI (`motion start/stop/status/threshold/interval/percent/snap/discord/webhook/cooldown`) with configurable sensitivity, SD card JPEG snapshots, and Discord webhook integration
- Added GUI design token system for consistent spacing, radii, fonts, safe areas and animation timing across all screens
- Added slide transitions for screen navigation replacing fade transitions
- Added pop-in animations for popups and scan status card
- Added spinning arc spinner for scan status overlay replacing animated dots
- Added Flock Safety camera detector based on bennjordan/flock-you
- Added "Item Borders" setting to toggle borders on main menu items and defaulted it to off

### Changed
- Refactored main menu grid to flex rows with responsive column count and accent-colored selection highlight
- Replaced carousel text arrows with LVGL symbol arrows and made icon size scale with button size
- Solid-color themes now use a single consistent accent for all menu item borders instead of a tonal ramp
- Rewrote app gallery carousel to reuse a single card with slide animation instead of creating/destroying objects per swipe
- Nav button highlight now uses theme accent color instead of hardcoded yellow
- Status bar uses design token fonts and safe-area-aware padding
- Tweaked theme palette surface colors across all background shade levels
- Cleaned up terminal screen build config template conditionals
- Removed default LVGL shadow from popup buttons
- Improved fuel gague handling on the MAX17048 (Banshee), we now check SOC reported % against actual battery voltage

### Fixed
- Potentially fixed task stack overflow crashes in `sae_displ` and `eapol_logoff` tasks by making the glog format buffer static
- Fixed SAE flood not being accessible from the display UI attacks menu (C5/C6 only)
- Fixed potential division by zero crash in wardrive channel hopping timer when channel list is empty
- Fixed stack buffer overflow in BLE skimmer PCAP construction when processing oversized advertisement data from malicious BLE devices
- Fixed TOCTOU race condition in glog and uart_share lazy mutex initialization that could leak mutexes and break mutual exclusion under concurrent startup
- Fixed silent crypto failure in WPA PRF function where malloc errors produced garbage PTK output without signaling failure to callers
- Fixed NULL pointer crash in evil portal HTTP server when heap is exhausted during Host header extraction
- Fixed NULL pointer crash in WebUI settings API when JSON fields contain non-string types (e.g. numbers, null) - all cJSON valuestring accesses now guarded with cJSON_IsString()
- Fixed path traversal vulnerabilities in WebUI file read, download, and delete handlers allowing `../` bypass of /mnt sandbox
- Fixed NULL pointer crash in hex_to_lv_color when called with NULL input
- Fixed out-of-bounds read in SAE flood monitor callback when receiving truncated authentication frames without length validation
- Fixed race condition on static crypto buffers in SAE flood where monitor callback and flood task could corrupt each other's bignum state
- Fixed stack overflow in SAE flood monitor callback by deferring heavy mbedTLS operations to the flood task context
- Fixed use-after-free on global scanned_aps pointer in auto-deauth task - pointer now NULLed after free to prevent dangling access
- Fixed auto-deauth task blocking the caller permanently by spawning it as a FreeRTOS task instead of calling it directly, with duplicate-spawn guard and proper stop cleanup
- Fixed use-after-free in beacon spam where raw SSID pointer from command buffer was passed to task without copying - now uses strdup
- Fixed NULL pointer crash in options_view realloc failure where unchecked return led to guaranteed dereference on OOM
- Fixed silent out-of-bounds write in detail_view when realloc fails - ensure_capacity now returns bool and callers bail out safely
- Fixed ESP32-C5 not discovering 5GHz channels above UNII-1 (e.g. 149-165) during WiFi scans by using correct country code API at boot and re-applying it after WiFi driver reinit during AP scans
- Fixed RGB LED not turning off when stopping BLE device detection scan
- Fixed GPS info task stack corruption
- Fixed Cardputer ADV `*` key being treated as backspace in text entry fields
- Fixed Poltergeist status display failing to initialize due to I2C port returning ESP_ERR_INVALID_STATE instead of ESP_ERR_NOT_FOUND (#308)
- Fixed T-Deck ST7789 intermittent boot corruption by replacing init sequence with official LilyGo values and ensuring 120ms post-SWRESET delay
- Removed premature backlight activation in disp_driver_init to prevent garbage frame visibility on cold boot
- Fixed detail views reserving bottom space for touch controls when no touch control bar is rendered
- Fixed DIAL device discovery blocking up to 20 seconds by reducing retry count from 10 to 5 and delay from 2s to 1s
- Fixed memory leak in m5gfx_wrapper where Panel_ST7789 was allocated with new but never deleted on re-init
- Fixed potential memory leaks in NFC view where ndef_details_result_t was not always freed when display was unavailable
- Fixed malloc variable declaration issue in wpa_crypto PRF loop (size_t r_len moved inside loop)
- Fixed O(n²) realloc pattern in infrared file list by implementing exponential growth with capacity tracking
- Fixed WiFi connection retry having no user feedback by adding terminal status message before 3s delay
- Fixed NFC touchscreen controls double-firing menu actions
- Fixed CSV mutex use after free in wardriving close where flush task
  referenced a deleted semaphore
- Fixed wardriving scan callback blocking WiFi task forever when flush
  could not keep up, now capped at 200ms with graceful fallback
- Fixed dedupe tables leaked when closing without SD card, close path
  now always frees task, mutex, and dedupe tables
- Fixed dedupe race where scan callback accessed freed tables during
  stop, added csv_closing flag to reject new writes during teardown
- Fixed GPS quality data overwriting coordinates already set by caller
- Fixed TOCTOU race on nmea_hdl during CSV close by snapshotting handle
  before dereference
- Fixed hop counter retaining stale state across start/stop cycles
- Fixed WiFi raw capture (and other capture modes) always sniffing channel 1 instead of the selected AP's channel
- Fixed CSV line truncation going undetected by validating line ends
  with newline after incremental build
- Fixed count functions racing with close by guarding against csv_closing
  and NULL mutex
- Reduced wardriving stack usage by ~462 bytes by replacing escape
  buffers with direct incremental line build and replacing 150B gps_t
  snapshot with 60B lightweight copy
- Fixed inverted touch scroll direction on grid cards main menu layout

## Revival v1.9.8 - 2026-04-14

### Added
- Added New 'Ghostchi' App - assets by pr3!
- Added Wi-Fi multi-select flows for APs and stations in the Scan & Select menu so multiple targets can be selected from the paged detail lists before running attacks/actions
- Added GTK abuse testing flow for checking client isolation bypass behavior after joining a target Wi-Fi network
- Added beacon_spam_broadcast_karma function that uses real AP MAC so BSSID matches probe responses
- Added missing encoder controls to the detail view
- Added option to change background shade of options
- Added option to enable rounded menu items and set as default on
- Added a random ascii art boot banner to the serial log - @tototo31

### Changed
- Migrated project to ESP-IDF v6.0
- The Wired Hatter's Banshee C5 internal memory optimisations
- Improved The Wired Hatter's fuel gauge handling
- Karma now skips channel hopping when AP has connected clients
- Moved lvgl tick task back out of psram to resolve wd triggering on setup wizard
- Optimised LVGL memory footprint across all configs by disabling unused components
- Increased CYD display buffer for significantly smoother rendering
- Rename 'Normal' main menu layout to 'Carousel'
- Restyled touch control bar to be more compact and clean
- Wi-Fi capture commands now lock to the selected AP's channel when one AP is selected, or hop only across the selected AP channels when multiple APs are selected
- Removed unnecessary channel list rebuild every 100ms in PineAP detection hop timer
- Improved code readability by replacing comma operator with separate statement in reset_setting_value
- Replaced unsafe strcpy calls with snprintf in portal and AP credential commands
- Removed dead _WIN32 code path in file upload handler (never compiled on ESP32)
- Downgraded Digest auth header logging from INFO to DEBUG to avoid leaking credentials in logs
- Cleaned up duplicate includes in ap_manager.c and wifi_manager.c

### Fixed
- Miscellaneous stability fixes and code cleanup across the infrared, terminal, number pad, popup, options, badusb, and clock views
- Fixed crash on The Wired Hatter's Banshee S3 chip when enabling USB Keyboard caused by running out of input interrupts
- Potentially fixed issue where the C5 on the Banshee would run out of DMA
- Fixed channel_enabled flag not being reset when RMT operations fail
- Fixed crash when opening WebUI File Manager and improved styling
- Fixed airtag tracking not working
- Fixed TEmbedCC1101 fuel gague init
- Fixed WPS detection buffer overflow when exceeding MAX_WPS_NETWORKS limit
- Fixed out-of-bounds read in EAPOL detection on short packets
- Fixed EAPOL handshake M4 frames being misclassified as M2 by checking the Secure bit
- Fixed wardrive heartbeat timer using wrong interval (5s instead of 10s)
- Misc fixes: added packet validation to PWN scan callback, fixed symbol visibility on compare_bssid, added bounds checks to channel split loops
- Fixed capture command silently ignoring invalid capture types
- Fixed AP credentials command not validating SSID length (could overflow buffer)
- Fixed channel congestion command not checking malloc return values (potential crash on OOM)
- Fixed settime command rejecting valid Unix timestamp of 0 (Unix epoch)
- Fixed path traversal vulnerability in WebUI SD card file download and delete handlers (now enforces /mnt prefix)
- Fixed unbounded malloc in WebUI settings API handler (now capped at 4KB to prevent OOM crashes)
- Fixed missing HTTP error responses in settings API handler (previously left client hanging on parse errors)
- Fixed HCI buffer overflow in BLE PCAP callback when advertisement data exceeds 243 bytes
- Fixed snprintf size mismatch in WebUI file upload handler that could write past allocation
- Reset BLE spam detector state (company_id + counter) consistently on stop

## Revival v1.9.7 - 2026-03-23

### Added
- MIC RGB visualizer adapted from SensoryBridge by Connor Nishijima (https://github.com/connornishijima/SensoryBridge)
- Ethernet ARP poisoning attack with bidirectional spoofing, ICMP ping sweep, passive host discovery, DNS interception using network's actual DNS server, and IP packet forwarding

### Changed
- Reduced WiFi RX/TX buffer counts and LWIP pool sizes across all configs to lower memory usage
- Standardised FATFS sector size to 512, disabled per-file cache, and enabled dynamic buffers across all configs for better SD stability
- Disabled mDNS for Ethernet interface to prevent crashes
- Disabled SD SPI on somethingsomething2
- Upped GhostLink baudrate between The Wired Hatter's Banshee chips to 460800
- GhostLink ethernet uses new detail view instead of terminal
- Moved Flipper, Airtag and Skimmer display options to New 'Detect Devices' menu using detail view like 'Scan APs'

### Fixed
- Fixed beacon spam not broadcasting any SSIDs due to race condition where task flag was set after task creation
- Added back missing RGB pulse for flipper and airtag detection
- Fixed boot crash loop on devices without RTC hardware by replacing ESP_ERROR_CHECK with graceful error handling in RTC driver

## Revival v1.9.6 - 2026-03-10

### Added
- Added Channel Switch attack

### Changed
- Optimised CYD WiFi config values to save memory
- Enabled dynamic fatfs buffers for all CYD configs to save memory
- Optimised the terminal view to use half the memory with same functionality
- Offloaded misc memory allocations to PSRAM if enabled to lower internal pressure
- Universal remote transmit will cache signal batches into PSRAM if available to avoid suspending input processing on JIT mount configs

### Fixed
- Fixed FreeRTOS xTaskCreateStatic stack size bug, saving significant memory
- Fixed SD/SPI regression potentially causing some devices to not function properly

## Revival v1.9.5 - 2026-03-09

### Added
- Added auto saving of coredumps and cli commands for debugging - @tototo31
- Add NRF24 native + ghostlink support for The Wired Hatter's Banshee with a frequency analyzer
- Add Wigle EncodedForUseToken support and SD config loader - @Hamspiced

### Changed
- Wardriving dedupe now includes APs when RSSI differs lower or higher for better trilateration support
- Wardriving screen now shows `GPS Stale` when GPS data stops refreshing
- The Wired Hatter's Banshee GPS routing now uses S3 UART GPS on the GhostLink peer streamed over GhostLink to the C5 primary instead of relying on C5 soft GPS RX which has reliability issues
- Refactored WiFi options for better UX
  - Removed individual select and track options
  - Added new details view for listing APs, Stations and both combined
- Station scan now parses 802.11 frame control (type/subtype/DS bits) for better validation
- Station scan now captures data frames in addition to management frames for better detection
- Improved new Wi-Fi details view on Cardputer-sized screens
- Rewrote 'Visualiser' and desktop streamer (formerly known as Rave)

### Fixed
- Fixed new soft GPS parser losing first bytes of sentences by implementing double-buffering to eliminate re-arm gap
- Fixed soft GPS receive getting stuck after an RMT re-arm failure by adding retry recovery and re-arm telemetry counters
- Fixed wardriving writing stale last-known coordinates when GPS fix flags remained set but no fresh `GPS_UPDATE` events were arriving
- Fixed watchdog timeout during CSV UART streaming by releasing mutex before slow writes
- Potentially fixed watchdog timeout wardriving crash when writing to SD by making CSV buffer flush asynchronous
- Potentially fixed wardriving crash caused by O(n) linear probing in dedupe table
- Fixed BLE stop/exit races by stopping spam, spoofing, and scan modules before NimBLE deinit and waiting for the BLE spam task to exit cleanly
- Fixed repeated saved-WiFi reconnect failures after BLE use by restarting the Wi-Fi driver when needed and cancelling in-progress retries when `stop` is used
- Fixed multiple potential crashes when re-connecting to saved WiFi
- Fixed potential crash on device start-up
- Fixed Wi-Fi AP/station detail view overlaying behind recreated main menu content when backing out of scan flows
- Fixed a peer-helper wardriving crash risk caused by reading the live GPS parser handle while it could be deinitialized during helper/local GPS handoff
- Fixed a wardriving packet parsing crash risk by validating short management frames before copying the 802.11 header
- Removed incorrect blescan help log

## Revival v1.9.4 - 2026-03-02

### Added
- Added `wifistatus` CLI command to show connection status and saved network info
- Added new wardriving and GPS info display view
- Added GhostLink split-channel wardriving helper mode (`startwd --helper`) with helper-to-primary observation streaming
- Added optional software NMEA RX backend (`minmea_soft`) for template-specific GPS routing constraints
- Added Factory Reset option to wipe NVS and reboot
- Added auto upload to WiGLE - @Play2BReal
- Added WiGLE manual upload browser in display settings with paged CSV list and per-file upload actions
- Added WiGLE stats popup in display settings with scroll and close controls
- Added WiGLE CLI commands: `wigle files [page]`, `wigle upload <filename>`, and `wigle stats` - @Play2BReal, @jaylikesbunda
- Added control app updates - @tototo31
- Added Flipper Zero Companion App documentation - @tototo31
- Added SD JIT mounting for custom evil portal menu option
- Added hold to invert letter case on joystick select in keyboard view
- Added option to select custom portal for karma attack
- Add IO expander programmable button commands - @tototo31
- Added 'Cherry Blossom' and 'Soft Sand' themes

### Changed
- 'chipinfo' command now shows firmware version and enabled build features (Display, NFC, BadUSB, IR, GPS, etc.)
- Use country-appropriate channel list in main deauth task
- Improved GPS Info display with fix mode, satellites in view, and cleaner logging
- Moved multiple attacks and scans to separate files for maintainability
- Significantly optimised port scan memory usage
- Slightly increased IR Learn task size to prevent crash
- Improved BLE Spam
- Deauth: fixed 5GHz HT40 tuning, added burst loops, and removed rate limiting
- Reorganized settings menu into more categories
- Directly iterate to channels when deauthing multiple APs
- Optimised wardriving dwell times, added active probing and improved validation
- Wardriving now builds role-aware channel plans for split capture (primary 5 GHz, helper 2.4 GHz when both are available)
- Wardriving heartbeat now reports helper merge stats (`helper=merged/received`) for link visibility
- Reworked wardriving Wi-Fi dedupe into peek/commit flow to avoid consuming dedupe state before a successful CSV write
- Shortened delays for misc display menu building for more responsive feel
- Improved clock view responsiveness
- Increased BadUSB VSense delay to improve reliability of USB enumeration
- Improved CLI `scan` validation and status messaging for invalid durations and failed timed scans
- Improved CLI `sd` read/write/append reliability checks to report short writes and stream errors
- Improved task startup error handling for DIAL, Karma, Deauth, Beacon, EAPOL, DHCP Starvation, and SAE Flood
- Improved BLE capture startup flow to fail fast when handler registration or scan start fails
- Optimized PineAP detection memory model by lazily allocating detection tables at start and freeing them on stop
- Reworked PineAP detection logging to use a single queued worker task instead of per-detection task creation
- Reworked PCAP writer buffering to use a fixed static packet slot pool instead of per-packet heap allocations
- Reduced splash screen hold time from 2000ms to 900ms
- Refactored surface colors to be consistent across the UI
- Changed default screen timeout to 30s
- Miscellaneous fixes, improvements and refactors
- Fixed feberis pro spelling

### Fixed
- Fixed station deauth channel lookup
- Fixed potential NULL dereference in command registration when `strdup` fails under low memory
- Fixed silent serial startup failures by validating queue and task creation in `serial_manager_init`
- Fixed race-prone stop behavior in SAE flood by waiting for task exit before freeing crypto context
- Fixed race-prone restart behavior in Karma and Beacon by waiting/cleaning lingering tasks on stop
- Fixed DHCP starvation reporting success while socket/send operations were failing
- Fixed `select` CLI parse errors that omitted the invalid token and improved index list boundary checks
- Fixed GPS latitude parsing for GLL sentences (was using 3-digit degree width instead of 2)
- Fixed BLE not initializing when selecting a flipper
- Fixed crash when deinitializing BLE
- Fixed BLE stop hangs by draining active scan callbacks before shutdown and reducing heavy callback work
- Potentially fixed "Connect to saved WiFi" resets on repeated use
- Fixed GPS satellites logic
- Fixed misc wardriving issues
- Fixed wardriving AP loss where entries seen before a valid GPS fix could be skipped later by premature dedupe mutation
- Fixed 5GHz deauthing
- Fixed crash when stopping deauth
- Fixed joystick repeat only working vertically
- Fixed evil portal JIT mounting
- Fixed crash on the Setup Wizard screen
- Fixed touch handler for the WiGLE help popup
- Fixed saving of WiGLE API credentials to mirror other setting saves
- Fixed listing large amounts of evil portals on displays
- Fixed crash starting karma attack
- Fixed 'stop' not stopping the karma attack
- Fixed joystick and touch input not checking if display is dimmed
- Fixed wardrive exiting when waking the display with a touch press
- Fixed NFC saved tag popup having vertically aligned buttons instead of horizontal
- Fixed Marauder v4 SD Card mounting

## Revival v1.9.3 - 2026-02-11

### Added
- Added support for the Febris Pro board
- Added support for a new upcoming board
- Added GPIO interrupt-based IR RX approach for improved reliability
- Added Knight Rider and Static RGB modes
- Added 12-bit color precision pipeline for RGB
- Added GhostLink display commands to enable/disable the on-device AP and change credentials
- Added touch support for The Wired Hatter's Banshee
- Added proper touch support to the Infrared view
- Added MAX17048 fuel gauge support for The Wired Hatter's Banshee
- Added external RTC support for saving time sync on The Wired Hatter's Banshee
- Added new compass app for The Wired Hatter's Banshee
- Added ADXL345 accelerometer app to The Wired Hatter's Banshee
- Added touch control bar to the BadUSB view
- Added BadUSB display and CLI support with built-in test script for:
  - Cardputer
  - Cardputer ADV
  - TEmbedC1101
  - The Wired Hatter's Banshee
- Added more status display logs
- Added git commit hash retrieval and logging at build time - @tototo31
- Added helper for saving scan data to SD card with incremental file numbering
- Added auto saving of scans for:
  - AP scan
  - Station scan
  - Flipper scan
  - Airtag scan
  - BLE GATT scan

### Changed
- Set IR universal send RGB pulse brightness to 20% (reduced from 100%)
- Only save changed setting to NVS when changing in settings menu to prevent hangs/crashes
- Reduced the wait time when switching RGB modes
- Refactored and optimised Rainbow and Knight Rider RGB modes
- Show highlight border on all displays regardless of touch support
- Updated main menu item order
- Remove dependencies.lock file - @tototo31
- Adjust battery voltage threshold to allow for very dead batteries - @tototo31
- Replace "Ghost ESP Ready ;)" startup message with GHOST ESP ASCII and ghostcli help prompt
- Refactored NFC and IR menus to use shared options_view helpers for consistent styling
- Downgrade GPS errors to ESP_LOGW to prevent printing in terminal

### Fixed
- Issue where RGBs would stay lit after stopping a deauth attack
- Potential crash when stopping wardriving - Thanks to @10Evansr for reporting with a fix
- Issue where AP disable wouldn't work
- Minor RGB issues on The Wired Hatter's Banshee
- IR RX issues on The Wired Hatter's Banshee
- Display hanging when going to save NFC tags on The Wired Hatter's Banshee
- Crash on the TEmbedC1101 when processing large IR signals
- Clock icon not recoloring based on theme
- Country and timezone not properly persisting
- Airtag rgb pulsing in silent rgb mode
- Fixed incorrect speed conversion in NMEA GPS parser
- Bumped NMEA queue size from 16→32 to fix the UART pattern queue overflow
- Fixed issues with saving most settings to NVS
- Fixed deauth reverse-direction frames using station MAC as BSSID instead of AP BSSID

## Revival v1.9.2 - 2026-01-05

- Added Wireshark dongle mode for real-time PCAP streaming over USB/UART
- Added "No portal files found" placeholder for evil portal when SD folder is empty
- Optimized evil portal listing memory usage
- Added T-Deck keyboard shift, symbol key support with key repeat functionality
- Rewrote DIAL functionality to remove the need for HTTPS, decrease ram usage and increase reliability
- Fixed RGB LED error spam on devices without LEDs configured
- Fix EAPOL capture channel lock by stopping ALL hopping timers before capture
- Improved reliability of PCAP capture to SD card
- Fixed regression when using C5 with RGB + IR 
- Added ADC battery reading for the LilyGo T-Deck
- Fixed inverted touch scrolling in main menu list layout
- Fixed not being able to scroll up in options menus on some configs
- Fixed apps menu always opening top app instead of tapped app
- Disable light sleep in power saving mode on the T-Deck
- Added the back button to the terminal view on the T-Display S3 Touch
- Updated NimBLE config options to mirror the TEmbedC1101 for improved BLE reliability during certain tasks like AirTag detection
- Misc small fixes

## Revival v1.9.1 - 2025-12-25

- Fixed WebUI AP-only restriction to correctly allow AP clients (including IPv6-mapped IPv4 addresses)
- 'setcountry' command is now case-insensitive
- Fixed T-Deck trackball spamming inputs
- Removed limit of 50 for 'scanap' to prevent getting rid of early entries
- Changed "Unknown command" to "Unsupported command" in CLI error messages for better UX.
- Improved Cardputer charging detection
- Fixed dedicated GhostLink webui terminal not showing responses

## Revival v1.9 - 2025-12-23

### Added

#### Display & UI
- Added GhostLink display menu when connected to a peer with split view terminal showing normal/peer response logs
- Added 'Invert Encoder' setting to display UI for configs with encoders
- Added 9 new animations for the status display - @jaylikesbunda, @tototo31
- Added support for wired screen mirroring
- Added a first time boot setup wizard for display enabled configs

#### NFC
- Added Flipper Zero NFC parser compatibility layer with support for:
  - Aime
  - CSC Service Works
  - WashCity (Verified working)
  - Metromoney
  - Bip
  - CharlieCard
  - Disney Infinity (Verified working)
  - HI!
  - HID PACS (Verified working)
  - H World
  - Kazan
  - Microel
  - MiZIP
  - Plantain
  - Saflok (Verified working)
  - Skylanders (Verified working)
  - SmartRider (Verified working)
  - Social Moscow
  - Troika
  - Two Cities
  - Umarsh
  - Zolotaya Korona
  - Zolotaya Korona Online
- Added basic Mifare Desfire detection

#### BLE
- Added AirTag RSSI update logging so existing tags report RSSI changes every few seconds
- Added GATT scanning, service scanning and RSSI tracking

#### Wi-Fi & Networking
- Added Ethernet support and docs - @tototo31
- Added Ethernet Fingerprint scanning
- Added Unique AP counter to wardriving summary
- Added a Sweep scan to capture WiFi, BLE, GPS and 802154 data in a csv file on SD
- Added RSSI tracking for selected APs and stations
- Added OUI vendor lookup for access points and stations
- Added drone detection and spoofing

#### IR
- Added IR CLI support
- Added IR Dazzler functionality to pulse IR at 38kHz 95% duty load

#### Core & CLI
- Added CLI commands for changing the status display animations
- Added command to set amount of RGB LEDs
- Added SD Card CLI for control via WebSerial File Browser
- Added a shared string for the firmware name and version number
- Added JTAG support for ESP32C5
- Added USB HID keyboard host support on ESP32-S3 devices for controlling the UI and inputting text
- Added 'gpspin' command to set the GPS pin for recieving data

#### Hardware
- Added support for the RabbitLabs Poltergeist board
- Added basic support for the Heltec v3 (NO LORA/MESH) - @tototo31

#### Build & Docs
- Add Docker support for HTML header generation with build script - @tototo31

### Changed

#### Display & UI
- IR and NFC display views and popups now properly use active set UI theme
- Standardized LVGL screen root creation across display views and added status-bar content offset GUI helpers
- Grid menu now scrolls up and down instead of left and right
- BLE wardriving now uses dedicated wardriving screen with GPS stats and reliable device name parsing
- Reorganised the settings menu and adjusted styling
- Minor keyboard view logic and styling refactor
- Terminal enter/select now submits text if typed, otherwise opens keyboard view
- Enable clock menu for all boards by using built-in ESP32 RTC and changed icon
- Reorganised and renamed wifi display sections
- Small improvements to encoder handling
- Joystick Up/Down hold in options menus now auto-repeat

#### NFC
- Refactors to NFC logic to make more maintainable
- After scanning, NFC popup title now specifies the tag type
- Avoid redundant PN532 Mifare Classic reads for a minor speed up

#### Wi-Fi & Networking
- Changed WiGLE CSV header brand/model to report GhostESP and build template name
- Added wardriving deduplication, WiGLE CSV v1.6 pre-header escaping, and improved C5 channel hopping
- Free pcap queue and task when not capturing
- Optimized multi-AP deauth by grouping targets by channel and reducing inter-frame delays
- Added Pineapple OUI detection to existing Pineapple detection logic

#### Core & CLI
- Serial console UX improvements - @tototo31
- Use Kconfig baud rate for UART instead of hardcoded 115200
- Removed unused buffer to save 8KB RAM
- Added shared MAC formatting helper for refactors
- Added LVGL-safe helpers for NULL-safe object/timer deletion and scheduling
- Miscellaneous small code refactors and improvements
- Sync RTC time when a valid GPS fix is received
- Renamed Dual Comm UI and documentation branding to GhostLink
- Station scan now lists entries like AP scan results

#### Hardware
- Increase CPU clock speed on certain configs
- Changed default CPU clock speed to 240MHz instead of 160MHz for:
  - sdkconfig.awokimini
  - sdkconfig.CYD2432S028R
  - sdkconfig.CYD2USB
  - sdkconfig.CYD2USB2.4Inch
  - sdkconfig.CYD2USB2.4Inch_C_Varient
  - sdkconfig.CYDDualUSB
  - sdkconfig.CYDMicroUSB
  - sdkconfig.default.esp32
  - sdkconfig.default.esp2c5
  - sdkconfig.default.esp2s2
  - sdkconfig.flipper.jcmk_gps
  - sdkconfig.JCMK_DevBoardPro
  - sdkconfig.marauderv6
  - sdkconfig.minion
  - sdkconfig.poltergeist

#### Build & Docs
- Add vendor board support and images to documentation - @tototo31
- Removed Flappy Ghost app and related build/docs references

### Fixed

#### Display & UI
- Apps menu now follows main menu theme, controls and layout
- Main menu app colors are now consistent across devices
- Centralized UI theme palette definitions into a shared helper to reduce duplicate display code
- Fixed a crash when entering SYM menu on keyboard view - @dagnazty
- Fixed issues causing glitches with rainbow modes on certain devices and flicker when the RGB rainbow effect runs with power saving disabled
- Fixed status bar not resetting from rainbow styling when switching RGB mode back to normal
- Fixed apps menu not using the correct directions for joystick control
- Fixed an issue with layout of more than 6 apps on the grid menu layout

#### NFC
- Fixed an issue that would cause MFC dictionary attack to not try all possible keys
- Fixed an issue that would cause Chameleon Ultra to recover less keys than a PN532
- Adjusted NTAG model detection to infer 213/215/216 from read length even when CC size byte is incorrect
- Decoded NDEF URI fields so symbols like “@” display correctly

#### BLE
- Switched AirTag scanner to active BLE scanning for more reliable AirTag detection
- Fixed BLE scanning not being reliable
- Fixed crash after BLE deinit and during WiFi init
- Fixed RAW BLE Capture not working

#### Wi-Fi & Networking
- Fixed wardriving WiGLE v1.6 CSV output formatting
- Route evil portal HTML requests through the UART HTML buffer when active instead of the SD-backed file handler

#### IR
- Fixed IR send failing with long raw signals
- Fixed IR learn remote popup Cancel button not responding

#### Core & Hardware
- Fixed issues with GPS and GhostLink UART being shared
- Fixed gpsinfo display not logging anything when the GPS info task fails to start
- Fixed an issue where setting RGB pins would fail on configs with no LEDs set
- Debounce T-Deck trackball/keyboard I2C input
- Wrapped power-management transitions with RGB pause/resume to prevent a crash
- Fixed TEmbed C1101-specific hardware initialization running on all encoder configs
- Raise sys event task size to prevent intermittent crashes

## Revival v1.8.1 - 2025-11-03

### Added

- Added included TURNHISTVOFF universal IR file with popular TV Power buttons

### Changed

- Suspend Wi-Fi services during BLE commands to guarantee enough free memory for NimBLE to initialize successfully
- Updated MFC dictionary with new additions
- Only show touchscreen scroll buttons when the options list is scrollable
- Refactored the standard mainmenu to reduce memory usage and improve performance
- Changed default UI theme to 'Bright'
- Default to WebUI authentication disabled
- Replaced Basic HTTP auth with HTTP Digest (RFC2617) using HMAC-signed stateless nonces to avoid sending plaintext credentials
- Default AP authmode changed to WPA2/WPA3 mixed for ESP32‑C5 and ESP32‑C6
- Refactor NFC to use static pools instead of heap allocations for less fragmentation and better performance
- WebUI is now served as a gzipped file to reduce loading times
- IR remotes and universals menus now show “No .ir files” placeholder when no IR files are found

### Fixed

- Prevent accidental mainmenu nav button activation during swipes
- Fixed main menu color theming to match actually enabled items
- Fixed potential status bar display issues during screen transitions
- Fixed potential issue with menu navigation after clearing lists
- Correct ADC battery percentage scaling math to prevent incorrect readings
- Fixed BQ27220 reset/reseal flow to more accurately reflect battery state

## Revival v1.8 - 2025-10-27

### TL;DR

- PN532 and Chameleon Ultra support for NTAG and MIFARE Classic (read/write, NDEF, Flipper exports)
- Cardputer ADV, optional secondary status display and IO expander support
- WebUI redesign, 2 new main menu layouts
- Karma attack and 802.15.4 packet capture on C5/C6
- Heartbeat-based auto-reconnect for dual communication, stability fixes, and QoL improvements
- Miscellaneous fixes across core, display, Wi‑Fi/BLE, DNS, IR, and wardriving

### Added

#### NFC

##### PN532

- NTAG (Type 2) support: read/write NTAG213/215/216 with NDEF parsing and save to Flipper .nfc format
- MIFARE Classic support (Mini/1K/4K): Flipper dictionary attack, magic backdoor detection, and NDEF TLV parsing
- File management: 'Saved' menu for .nfc files and 'User Keys' view for `/mnt/ghostesp/nfc/mfc_user_dict.nfc`

##### Chameleon Ultra

- CLI support: connect/disconnect, status/battery, reader/emulator toggles - @tototo31
- UI support: PN532 parity with cached details, More/Save flows and dictionary attack
- NTAG and Mifare Classic NDEF parsing, Flipper `.nfc` exports from `chameleon savehf/savedump/saventag` - @tototo31, @jaylikesbunda

#### Hardware

- Added support for Cardputer ADV
- Added Kconfig support for a secondary status display
- Added Kconfig support for IO Expander - @Play2BReal
- Added heartbeat-based auto-reconnect for dual communication

#### UI

- Added 2 alternate main menu layouts (Grid and List)
- Ghost (asset by @the1anonlypr3) and Game of Life idle animations for status display
- Added command history with up/down navigation and full in-line cursor editing to the serial console - @tototo31
- Added joystick support for keyboard input in terminal view - @tototo31
- Added 'set/getneopixelbrightness' commands and ability to set settings via CLI - @tototo31

#### Attacks

- Added 802.15.4 packet capture (only on C5, C6)
- Added karma attack - @tototo31 in #108

#### Misc

- Added glog - a lightweight logging helper

### Changed

#### UI

- Use a fixed-size active-key buffer for keyboard
- Refactor popups to use reusable popup helpers
- Refactor options menu to use reusable options view helpers
- Refactor touch keyboard view to significantly reduce memory usage
- Enabled software back buttons made for encoder controls on joystick too
- Size popup buttons based on what's in them
- WebUI redesign (Part 2)
- Organise BLE menu into hierarchical sub-menus - @tototo31
- Lowered LV_MEM_SIZE from 32KB to 16KB on most display configs

#### Attacks

- Flush PCAP and CSV data to SD Card on a timer
- EAPOL capture now captures extra packet types for cracking and detects when a crackable handshake is found
- Added a summary log when starting a packet capture and reduce filter stats frequency

#### Misc

- Lowered pineap task size
- Changed the C5 to use a single display buffer to save memory
- Reduce VFS allocation unit size to 4KB
- Cap displayed WiFi APs to 50 for 'scanap' output
- Refactor comm manager to centralize packet handling, add state mutex and handshake timeout, and guard UART driver install
- If dualcomm is set to pins used by the serial UART, disable the serial UART
- Update main menu icons to RGB565A8
- Refactored dualcomm logic to be more robust
- lower all CYD LVGL memory buffers to 16KB and swap to single buffer for display

### Bug Fixes

#### Core

- Fixed intermittent IR learning errors by properly owning and copying received RMT symbol data before passing from ISR to task.
- Fixed memory leak, race conditions and add buffer error handling in pcap.c
- Track SPI host/mount state and only free initialized SPI host on unmount
- Added NMEA handle null-checks
- Flush PCAP header on open and close PCAP on generic stop command
- Miscellaneous fixes and improvements
- Small miscellaneous memory saves
- Fixed RMT channel allocation on C5 to prevent conflicts with IR TX
- Disable duplicate filtering in general BLE scanning
- Removed heap alloc per command
- Added deletions for VisualizerHandle on disconnect/stop and rgb_effect_task_handle on rgb off/stop to prevent lingering tasks 
- Removed second mdns init call
- Preallocate handlers array, remove reallocs; replace last_company_id malloc with value+flag in BLE manager
- Free all LED strip resources on deinit
- Ignore self when discovering peers for dual comm
- Prevent crash and spam in EAPOL Logoff attack
- Fixed minor issues with the dns server
- Fixed BLE capture stopping itself after recieving an event
- Added sanity checks to IE parsing to prevent OOB reads
- Accepted HCI packet types now include CMD, ACL, SCO, and ISO
- Reduce heap churn by reusing a single 4KB transfer buffer in wifi manager streaming
- Significantly improve reliability of capturing wifi frames
- Remove arbitrary limitation on the lines of text in the webUI dual comm terminal
- Fixed an issue causing potential corruption of pcaps saved to the Flipper Zero
- Fixed wardriving encryption detection
- Wardriving now properly hops channels for AP scanning

#### Display

- Possible fix for random rotation of ST7789 displays upon flashing
- Joystick builds now use touch keyboard layout with selection highlighting and navigation
- Fix keyboard not using SHIFT correctly and the keyboard view forcing lowercase
- Remove artificial delay in cardputer keyboard task to make more responsive
- Improve and refactor terminal message handling
- Remove key highlight on touch only devices for the keyboard view
- Fixed duplicate back button and wrong red styling in universals IR view

## Revival v1.7.2 - 2025-09-06

### Added

- Added navigation arrows to the main menu - @tototo31
- Add support for Lolin S3 pro - @tototo31
- Echo backspace, newline, and characters directly to UART and JTAG when supported - @tototo31

### Changed

- WebUI Redesign (Part 1)
- Flush PCAP and CSV data to SD Card on a timer
- Switch to single display buffer on cardputer for extra memory

### Bug Fixes

- Fix not saving or using saved dual comm pins correctly
- Shift main menu down to account for status bar
- Prevent UART conflicts on TDECK by conditionally disabling serial manager and UART driver installation in esp_comm_manager.c - @tototo31

## Revival v1.7.1 - 2025-08-21

- Fix for RGB not properly being handled on devices with no LEDs
- Possible fix for captive portal not being effective on some devices
- Apply existing wroom display memory optimizations to c5
- Fix incorrect usage of mDNS
- Update setcountry command on the C5 to use the official esp_wifi_set_country_code function


## Revival v1.7 - 2025-08-17

### Major Updates

- **Dual ESP32 Communication**
  - Connect two GhostESP devices together.
  - Dedicated WebUI section for managing linked devices.

- **Power Saving**
  - Up to 5x the battery life on compatible boards using the new Power Saving Mode (when compared with v1.6.1).

- **New Board Support**
  - LilyGo TEmbed C1101
  - LilyGo TDeck - @tototo31
  - LilyGo TDisplay S3 Touch
  - AITRIP CYD / ESP2432S028R - @tototo31
  - JCMK DevBoard Pro
  - Rabbit Labs Minion

- **Infrared RX** (only enabled on TEmbed C1101)
  - IR receive and decode support for all protocols supported by Flipper Zero firmware.
  - Ability to Rename, Delete, Add remotes 
  - Easy Learn Mode: **Name buttons automatically**
  
### Added

- Attacks
  - Support for setting an Evil Portal HTML via the Flipper Zero App with a max size of 2048 bytes (as of app v1.4)
  - Added option to select Custom Evil Portal html file from the SD Card - @tototo31

- Display
  - Added 'Never' display timeout setting.
  - Added 'Power Saving' setting which turns off the AP and lowers the CPU frequency on Cardputer and S3TWatch.
  - Display backlight percentage setting for PWM enabled devices - @tototo31
  - Placeholder text for keyboard view - @tototo31
  - Encoder friendly version of the keyboard view
  - Fuel Gauge support with manager and kconf setting (only BQ27220 support initially)
  - Add Vim keybindings for keyboard interactions in various screens - @tototo31
  - Add zebra menu styling and improve vertical alignment - @tototo31
  - Smooth mainmenu animations - @tototo31
  - Keyboard enhancements - @tototo31
  
- Commands
  - Help command reorganised into categories - @tototo31
  - 'chipinfo' command to display chip information
  - 'apenable' command to enable/disable the Access Point
  - 'disconnect' command to disconnect from the current network
  - 'setrgbmode' command to change the RGB mode
  - 'scanarp' command to initiate an ARP scan on the local network
  - 'scanssh [IP]' command to initiate an SSH scan on the target IP
  - '-live' arg for 'scanap' for a non blocking scan that lists APs as they're found

- Misc
  - Add build name config variable for debugging and auto-flash support - @tototo31
  - Try to connect to saved WiFi on boot if available
  - Add 'Stealth' mode for silencing RGB - @tototo31
  - Terminal App to use commands with the keyboard - @tototo31
  - Add memory checks for debugging before initializing AP, BLE, and WiFi managers - @tototo31
  
### Changed

- Display
  - Reuse options screen view for settings screen. Resolves #66 and #65
  - PWM backlight control using ledc on supported devices
  - Moved 'Terminal Color' and 'Third Control' to the Display section in settings
  - Color status bar icons based on their activity
  - S3TWatch: Disable tap-to-wake, use touch interrupt instead.
  - Exiting a view now returns to the previous view instead of the main menu - @tototo31
  - Add CAPSLOCK shift toggle to keyboard view - @tototo31
  - Restyle terminal view - @tototo31

- WebUI
  - Minor style tweaks
  - Update Help tab
  
- Attacks
  - Refactor packet capture
  - Refactor 'scanports' command to be more intuitive and user-friendly

- General
  - Cap displayed WiFi APs to 50 for 'scanap' output
  - Organise BLE menu into hierarchical sub-menus - @tototo31
  - If dualcomm is set to pins used by the serial UART, disable the serial UART

### Bug Fixes

- Display
  - Dynamically size error popup to content and center on screen
  - Reduce FatFS memory usage on S3TWatch and Cardputer
  - Improve battery reading accuracy on Cardputer
  - Fix keyboard view touch detection logic - @tototo31
  - Save settings when exiting the settings view
  - Update LEDs and Status bar when changing from rainbow mode
  - Refresh current item highlight when changing theme

- Commands
  - Add terminal_view_add_text logs to commands missing them
  - Skip pcap flush if mutex is null
  - Fix stop command not stopping GPS task
  - Fix serial going unresponsive by using 'scanap -stop'
  - Small fixes to the process of connecting to a WiFi network
  - Refactor SAE Flood Attack - now requires a password to be set as an argument
  - Handle backspace and DEL properly in serial input
  - Airtag spoofing fixes
  
- General
  - Disable and re-enable ESP comm manager UART around GPS usage to avoid driver conflicts
  - Flush every packet to UART (Flipper) immediately when there's no sd card
  - Miscellaneous refactoring for memory usage
  - Add wifi_manager_stop_beacon function
  - Check if an RMT channel already exists and clean it up before making a new one
  - Randomise BLE Spam MAC addr and add more devices
  - Better EP credential handling
  - Keep one led strip rmt instance
  - Remove legacy led strip rmt driver
  - Tweaks to evil portal captive portal handling

## Revival 1.6.1 - 2025-07-02

- Hotfix for 'BLE stack not ready' on CYD devices.


## Revival v1.6 - 2025-06-30

### TLDR

Support for FlipperZero IR files, Better power consumption, BLE Spam, WPA3 SAE Flood, Deauth multiple APs at once, and more!

### Added

- IR Support (enabled on LilyGo S3TWatch and Cardputer)
  - Uses FlipperZero formatted IR files stored in sdcard: /ghostesp/infrared/remotes or /ghostesp/infrared/universals
  - Universal Library IR Transmit
  - Signals File IR Transmit
  - IR Protocol Encoders:
    - NEC
    - Kaseikyo
    - Pioneer
    - RCA
    - Samsung
    - SIRC
    - RC5
    - RC6

- BLE Spam (not supported on ESP32S2)
  - Apple
  - Microsoft
  - Samsung
  - Google

- Display
  - Added keyboard view
  - Connect to WiFi command with keyboard view
  - Add S3TWatch virtual storage (4MB) acessable through webUI

- Attacks
  - EAPOL Logoff Attack
  - SAE Flood Attack (WPA3 only)
  - Probe request listen

- Commands
  - 'webauth on/off' command to enable/disable webui authentication

- Cardputer
  - Add keyboard event handling functionality - @tototo31
  - Enable Cardputer's LED in config - @tototo31
  - Get cardputer keys working - @tototo31

### Changed

- Display
  - Removed touch controls from settings menu on non-touch devices - @tototo31
  - Refactor wifi menu into hierarchical sub-menus
  - Enable ESPIDF Power Management freq scaling on Cardputer, S3TWatch 2.4Inch CYD, and Phantom
  - First item is no longer highlighted on menu lists for touch devices - @tototo31

- Cardputer
  - Use backtick key to return to main menu

- Attacks
  - Deauth Attack now supports targeting multiple APs

- Commands
  - Allow selection of multiple APs (eg. select -a 2,3,4)
  - AP list now includes wifi channel
  
- WebUI
  - Refactor file explorer to be more user friendly

- Power
  - Set min PM freq to 20MHz instead of 80MHz

- General
  - replaced several large static allocations with dynamic heap allocations

### Bug Fixes

- Display
  - Fix blank bootup screen on cardputer and show flappy ghost icon out of necessity - @tototo31
  - Fix status bar icons - @tototo31
  - Added auto-cleanup of old terminal messages when text length exceeds threshold

- WiFi
  - preserve STA mode in ap_manager init and start_services

- Cardputer
  - Get cardputer battery status working - @tototo31
  - Ignore the key press that wakes the display from sleep

- General
  - Fix SD Card init on CYD devices - @tototo31
  - Capped stored wifi scan results at 100 and auto-truncate lists to prevent memory bloat and crashes

- WebUI
  - Fix file explorer not opening folders, erroring on upload.


## Revival v1.5.1 - 2025-05-30

### Added

- add default sd pins for default configs
- 'setcountry' command to set country code for esp32c5

### Changed

- update pineap detection to use dualband channels with ESP32C5
- backlight dimming fix for cardputer - @tototo31
- handle button presses correctly on cardputer - @tototo31
- fix inputs not waking screen on cardputer - @tototo31
- bump M5GFX to 0.2.9
- wrap menu items once you hit the top or bottom of the screen - @tototo31


### Bug Fixes

- added warning that webui will disconnect you from the web interface when running wifi commands
- disable menu items in main menu if the device does not support them - @tototo31
- hide touch interface on non-touch devices - @tototo31
- fix cardputer settings menu crash
- fix rabbit labs' phantom n cyd build boot issues

## Revival v1.5 - 2025-05-25

### Added

- Support for ESP32C5 (some channels may not work as expected for now)
- FlipperZero Devboard w/JCMK GPS module config file - #11 - @tototo31

- Attacks
  - Deauthentication & DoS

    - Added support for direct station deauthentication
    - Added DHCP-Starve attack

  - Spoofing & Tracking

    - Added support for AirTag selection and spoofing
    - Added support for selecting and tracking Flipper Zero rssi

  - Beacon Management

    - Custom beacon SSID list management and spam

- Commands
  - Added station selection capability to existing select command
  - Added a timezone command to set the timezone with a POSIX TZ string
  - Enable passing custom DIAL device name via CLI argument

- Display
  - Add back button to options screen bottom center to return to main menu
  - Added swipe handling for the main menu and app gallery views
  - Add vertical swipe navigation for scrolling of menu items (requires a capacitive touch screen)
  - Added station scanning and the new station options to the wifi options screen
  - Added simple digital clock view
  - Settings menu (with old screen controls as an option)
  - Configurable main menu themes (15 different ones to choose from)
  - Added "Connect to saved WiFi" command
  - Configurable terminal text color
  - Added "List APs" command
  - Added "Invert Colors" option to settings menu

### Changed

- Attacks
  - If station data is available, directly deauth known stations of the AP selected for deauth
  - Deauth task now deauths on each AP's primary channel
  - Station scan now uses discovered AP channels for scanning
  - ESP32C5 shows band in AP scan results
  - ESP32C6 and ESP32C5 show Security and if PMF is required in AP scan results
  - If company is unknown, it won't be shown in AP scan results
  
- Display

  - Performance Optimizations

    - Refactored options screen to use lv_list instead of a custom flex container to improve performance
    - Replaced single lv_textarea in terminal view with scrollable lv_page and per-line lv_label children to improve performance
    - Optimize terminal screen by batching text additions

  - UI & UX Adjustments

    - Offset terminal page vertically by status bar height and adjust its height accordingly.
    - Remove index reset in main_menu_create to maintain selection across view switches
    - Default display timeout is now 30 seconds instead of 10
    - Status bar now updates every second instead of when views change
    - Removed rounding on the status bar
    - Changed bootup icon
    - Removed default shadow/border from back buttons
    - Changed option menu item color to be black and white
    - Added text to the splash screen and removed animation

- Commands
  - List stations with sanitized ascii and numeric index
  - Label APs with blank SSID fields as "Hidden"
  - Make congestion command ASCII-only for compatibility
  - Change display EP option to start default EP with a default SSID "FreeWiFi"
  - Update congestion to work with dualband channels
  - Make GPS formatting renderable on devices w/ a limited font - #13 - @tototo31 

- Power
  - Suspend LVGL, status bar update timer, and misc tasks when backlight is off
  - Use wifi power saving mode if no client is connected
  - Poll touch 5x slower when backlight off
  - Enabled light-sleep idle and frequency scaling

- RGB
  - Refactored rgb_manager_set_color to use is_separate_pins flag instead of compile-time directives

- WebUI
  - Changed color theme to black and white
  - Improve loading 

### Bug Fixes

- General
  - Fixed NVS persistence issues for AP credentials by ensuring a single shared NVS handle and settings instance.
  - Addressed unaligned memory access warning in ICMP ping logic by using an aligned buffer for checksum calculation.
  - Restart mDNS service with AP
  
- Display
  - Fixed an issue where an option would be duplicated and freeze the device.
  - Skip first touch event while backlight is dimmed so tap only wakes the screen without registering input
  - Fixed an issue where the numpad would register 2 inputs for a single tap.
  - Fixed screen timeout only resetting on the first wake-up tap
  - Add tap to wake functionality to non battery config models
  - Keep app gallery back button on top of icons

- Power
  - Fixed an issue where the device was reporting that it was not charging when it was.

- RGB
  - Persist RGB pin settings to NVS and auto-init from saved config, closes [jaylikesbunda/Ghost_ESP#5](https://github.com/jaylikesbunda/Ghost_ESP/issues/5)

- GPS
  - Initialize GPS quality data and zero-init wardriving entries to prevent crash in wardriving mode
  - Don't check for csv file before flushing buffer over UART
  - Actually open a CSV file for wardriving when an SD card is present
  - Fix CSV file timestamp to reflect GPS date/time on SD card close
  - Reset GPS timeout flag on initialization
  - Assign gps RX pin based on CONFIG if not explicitly set by the user - #12 - @tototo31

## Revival v1.4.9 - 2025-04-27

### ❤️ New Stuff

- Basic changeable SD Card pin out through webUI and serial command line (requires existing sd support to be enabled in your board's build)
- Added default evil portal html directly in the firmware (credit to @breaching and @bigbrodude6119 for the tiny but great html file)
- Basic congestion command to quickly see channel usage
- Added scanall command to scan aps and stations together

### 🤏 Tweaks and Improvements

- Simplified the evil-portal command line arguments.
  - eg. ```startportal <google.html> (or <default>) <EVILAP> <PSK>```
- Save credentials in flash when using connect command
- Captive portal now supports Android devices
- Simplified the evil-portal command line arguments.
- set LWIP_MAX_SOCKETS to 16 instead of 10
- Save captured evil-portal credentials to SD card if available
- Added support for scanning aps for a specific amount of time eg. ```scanap 10```
- Connect command now uses saved credentials from flash when no arguments are provided
- Added channel hopping to station scan
- Include BSSID in scanap output

### 🐛 Bug Fixes

- Use "GhostNet" as fallback default webUI credentials if G_Settings fields are not set or invalid
- Fix webUI not using evilportal command line arguments
- Fix evil‑portal local file serving 
- Correctly parse station/AP MACs and ignore broadcast/multicast in Station Scan
- Fix station scanning using wrong frame bit fields and offsetting the mac addresses

----------------------

OK, we back. - 22 April 2025

-----------------------

Rest in Peace, GhostESP - 22 April 2025

______________________

## 1.4.7 - 2025-03-09

### ❤️ New Stuff

General:

- Added WebUI "Terminal" for sending commands and receiving logs - @jaylikesbunda

Attacks:

- Added packet rate logging to deauth attacks with 5s intervals - @jaylikesbunda

Lighting:

- Added 'rgbmode' command to control the RGB LEDs directly with support for color and mode args- @jaylikesbunda
- Added new 'strobe' effect for RGB LEDs - @jaylikesbunda
- Added 'setrgbpins' command accessible through serial and webUI to set the RGB LED pins - @jaylikesbunda


### 🐛 Bug Fixes

- Immediate reconfiguration in apcred to bypass NVS dependency issues - @jaylikesbunda
- Disabled wifi_iram_opt for wroom models - @jaylikesbunda
- Fix station scanning not listing anything - @jaylikesbunda
- Connect command now supports SSID and PSK with spaces and special characters - @jaylikesbunda

### 🤏 Tweaks and Improvements

- General:
  - Added extra NVS recovery attempts - @jaylikesbunda
  - Cleaned up callbacks.c to reduce DIRAM usage - @jaylikesbunda
  - Removed some redundant checks to cleanup compiler warnings - @jaylikesbunda
  - Removed a bunch of dupe logs and reworded some - @jaylikesbunda
  - Updated police siren effect to use sine-based easing. - @jaylikesbunda
  - Improved WiFi connection output and connection state management - @jaylikesbunda
  - Optimised the WebUI to be smaller and faster to load - @jaylikesbunda

- Display Specific:
  - Update sdkconfig.CYD2USB2.4Inch_C_Varient config - @Spooks4576
  - Removed main menu icon shadow - @jaylikesbunda
  - Removed both options screen borders - @jaylikesbunda
  - Improved status bar containers - @jaylikesbunda
  - Tweaked terminal scrolling logic to be slightly more efficient - @jaylikesbunda
  - Added Reset AP Credentials as a display option - @jaylikesbunda


## 1.4.6 - 2025-01-06

### ❤️ New Features

- Added Local Network Port Scanning - @Spooks4576
- Added support for New CYD Model (2432S024C) - @Spooks4576
- Added WiFi Pineapple/Evil Twin detection - @jaylikesbunda
- Added 'apcred' command to change or reset GhostNet AP credentials - @jaylikesbunda

### 🐛 Bug Fixes

- Fixed BLE Crash on some devices! - @Spooks4576
- Remove Incorrect PCAP log spam message - @jaylikesbunda
- retry deauth channel switch + vtaskdelays - @jaylikesbunda
- Resolve issues with JC3248W535EN devices #116 - @i-am-shodan, @jaylikesbunda

### 🤏 Tweaks and Improvements

- Overall Log Cleanup - @jaylikesbunda
- Added a IFDEF for Larger Display Buffers On Non ESP32 Devices - @Spooks4576
- Revised 'gpsinfo' logs to be more helpful and consistent - @jaylikesbunda
- Added logs to tell if GPS module is connected correctly- @jaylikesbunda
- Added RGB Pulse for AirTag and Card Skimmer detection - @jaylikesbunda
- Miscellaneous fixes and improvements - @Spooks4576, @jaylikesbunda
- Clang-Format main and include folders for better code readability - @jaylikesbunda

## 1.4.5 - 2024-12-20

### 🛠️ Core Improvements

- Added starting logs to capture commands - @jaylikesbunda
- Improved WiFi connection logic - @jaylikesbunda
- Added support for variable display timeout on TWatch S3 - @jaylikesbunda
- Revise stop command callbacks to be more consistent - @jaylikesbunda, @Spooks4576

### 🌐 Network Features

- Enhanced Deauth Attack with bidirectional frames, proper 802.11 sequencing, and rate limiting (thank you @SpacehuhnTech for amazing reference code) - @jaylikesbunda  
- Added BLE Packet Capture support - @jaylikesbunda  
- Added BLE Wardriving - @jaylikesbunda  
- Added support for detecting and capturing packets from card skimmers - @jaylikesbunda  
- Added "gpsinfo" command to retrieve and display GPS information - @jaylikesbunda

### 🖥️ Interface & UI

- Added more terminal view logs - @jaylikesbunda, @Spooks4576  
- Better access for shared lvgl thread for panels where other work needs to be performed - @i-am-shodan
- Revised the WebUI styling to be more consistent with GhostESP.net - @jaylikesbunda
- Terminal View scrolling improvements - @jaylikesbunda
- Terminal_View_Add_Text queue system for adding text to the terminal view - @jaylikesbunda
- Revise options screen styling - @jaylikesbunda

### 🐛 Bug Fixes

- Fix GhostNet not coming back after stopping beacon - @Spooks4576
- Fixed GPS buffer overflow issue that could cause logging to stop - @jaylikesbunda
- Improved UART buffer handling to prevent task crashes in terminal view - @jaylikesbunda
- Terminal View trunication and cleanup to prevent overflow - @jaylikesbunda
- Fix and revise station scan command - @Spooks4576

### 🔧 Other Improvements

- Pulse LEDs Orange when Flipper is detected - @jaylikesbunda
- Refine DNS handling to more consistently handle redirects - @jaylikesbunda
- Removed Wi-Fi warnings and color codes for cleaner logs - @jaylikesbunda
- Miscellaneous fixes and improvements - @jaylikesbunda, @Spooks4576  
- WebUI fixes for better functionality - @Spooks4576

### 📦 External Updates

- New <https://ghostesp.net> website! - @jaylikesbunda
- Ghost ESP Flipper App v1.1.8 - @jaylikesbunda
- Cleanup README.md - @jaylikesbunda
