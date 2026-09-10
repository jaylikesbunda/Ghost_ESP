---
title: "CLI Reference"
description: "Common GhostESP CLI commands grouped by category."
weight: 80
toc: true
---

## Connecting to the CLI interface

- Use a [serial console](https://ghostesp.net/serial) (115200 baud is recommended) with a USB data cable or the built-in Terminal app on touch-enabled boards.
- From the web UI, open the Terminal panel for remote access. When you launch a Wi-Fi or BLE command, the device suspends the GhostNet AP until the radio work finishes; once you run `stop` (or the command completes), BLE deinitializes and Wi-Fi returns automatically.
- Send `help` to confirm connectivity; output appears prefixed with `>` in the console.

## Core

- **`help [category|all]`** — List commands by category (`wifi`, `ble`, `chameleon`, `comm`, `sd`, `led`, `gps`, `misc`, `portal`, `printer`, `cast`, `capture`, `beacon`, `attack`, `ir`, `ethernet`, `camera`, `shell`, `gadgets`). Use `help all` to list every command.
- **`chipinfo`** — Print SoC model, cores, features, and IDF version. When core dumps are enabled to flash, it also shows coredump partition status and (when available) the panic reason from the last crash.
- (for developers) **`mem [dump|trace <start|stop|dump>]`** — Print heap stats, dump allocation state, or control heap tracing.
- **`reboot`** — Soft restart the device.
- **`timezone <TZ>`** — Set timezone, e.g., `timezone EST5EDT,M3.2.0,M11.1.0`.
- **`stop`** — Stops all active attacks, scans, and background tasks. Also restarts Wi-Fi if it was suspended by BLE.
- **`stopscan`** — Alias for `scanap -stop`; stops an active AP scan.
- **`congestion`** — Display Wi-Fi channel congestion chart showing activity across all channels.

### Core dumps (flash builds only)

These commands are only present on builds that enable ESP-IDF core dumps **to flash** (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`).

- **`coredump`** — Print a quick summary (partition size and whether a coredump is present).
- **`coredump dump`** — Stream the coredump partition as base64. Save the output body to `coredump.b64` (omit the start/end marker lines), then decode on your host with `idf.py coredump-info -c coredump.b64`.
- **`coredump erase`** — Erase the coredump partition (clears the saved crash).

## WiFi

### Scanning

- **`scanap [seconds|-live|-stop]`** — Run an AP scan, optionally for a set duration, live channel hop, or stop (`-stop`).
- **`scansta`** — Hop channels and log associated stations.
- **`scanall [seconds]`** — Combined AP and STA scan with summary.
- **`sweep [-w wifi_sec] [-b ble_sec]`** — Full environment sweep: scans WiFi APs, stations, and BLE devices, then saves a CSV report to SD (`/mnt/ghostesp/sweeps/sweep_N.csv`).
- **`list [-a|-s|-airtags]`** — Show AP scan results, associated stations, or AirTags.
- **`listenprobes [channel|stop]`** — Monitor probe requests and log to PCAP if SD is present.
- **`wpa3check`** — Run a WPA3 compliance check on the selected AP (`select -a <idx>` first). If no AP is selected, scans all APs and prints a summary per AP showing WPA3 presence, transition mode, PMF posture, and a short finding. Available from **WiFi → Scan & Select** on-device.

### Targeting

- **`select [-a|-s|-airtag] <idx[,idx]>`** — Queue APs, a station, or an AirTag by index for later actions.
- **`connect <ssid> [pass]`** — Join an infrastructure network (saves credentials); wrap SSID/password in quotes when they contain spaces, e.g., `connect "My SSID" "My Password"`.
- **`disconnect`** — Leave the current STA connection.
- **`apcred <ssid> <pass>`** or **`apcred -r`** — Change or reset GhostNet AP credentials.
- **`apenable on|off`** — Toggle AP persistence across reboots.
- **`trackap`** — Track selected AP signal strength (RSSI) in real-time.
- **`tracksta`** — Track selected station signal strength (RSSI) in real-time.
- **`wifistatus`** — Show current WiFi connection status, including SSID, signal strength, IP address, and saved network info.

### Offense

- **`attack -d|-c|-e|-s|-g|-hsd|-p|-b|-a <args>`** — Trigger various Wi-Fi attacks.
  - `-d` — Deauthentication attack on selected AP(s).
  - `-c` — Channel Switch Announcement (CSA) attack. Sends forged 802.11 beacons with the AP's real SSID/BSSID and a Channel Switch Element (IE 37) directing clients to a different channel, causing disconnection.
  - `-e` — EAPOL logoff attack.
  - `-s <password>` — SAE flood attack (ESP32-C5/C6 only, requires target PSK).
  - `-g <ssid> <password>` — GTK abuse attack on the selected AP.
  - `-hsd` — Combined handshake capture + deauth. Simultaneously sends deauth bursts and captures EAPOL frames for WPA handshake extraction.
  - `-p` — Probe request flood. Broadcasts spoofed probe requests.
  - `-b` — Bad message attack (EAPOL key install).
  - `-a` — Authentication flood. Sends mass authentication frames to the AP.
- **`stop`** — Stops all active attacks, scans, and background tasks.
- **`stopdeauth`** / **`stopspam`** — Halt active attacks or beacon floods.
- **`beaconspam [mode]`** — Broadcast spoof SSIDs (`-r`, `-rr`, `-l`, or custom text).
- **`beaconadd <ssid>`** — Add an SSID to the beacon spam list.
- **`beaconremove <ssid>`** — Remove an SSID from the beacon spam list.
- **`beaconclear`** — Clear all SSIDs from the beacon spam list.
- **`beaconshow`** — Show current beacon spam list.
- **`beaconspamlist`** — Show the beacon spam list with details.
- **`karma start [ssid...]`** / **`karma stop`** — Respond to client probes with saved or provided SSIDs.
- **`pineap [-s]`** — Monitor Pineapple-style beacons; `-s` stops detection.
- **`saeflood <password>`** / **`stopsaeflood`** / **`saefloodhelp`** — Start, stop, or show help for SAE flood attacks.

### Network

- **`scanports <local|ip> [all|start-end]`**, **`scanarp`**, **`scanlocal`**, **`scanssh <ip>`** — Scan the subnet, a target host, or run mDNS/SSH discovery utilities.
- **`netbiosscan [<ip>|subnet <a.b.c[.0|.]>]`** — Discover Windows hosts via NetBIOS Name Service (NBNS) queries on UDP port 137. Without arguments, scans the current subnet. With an IP, scans a specific host. With `subnet <prefix>`, scans a `/24` prefix.
- **`httpbannerscan [<ip>|subnet <a.b.c[.0|.]>]`** — Probe common HTTP/HTTPS ports (80, 8080, 8000, 443, 8443) and grab `Server` banners to identify web servers and applications.
- **`snmpprobe [<ip>|subnet <a.b.c[.0|.]>|walk <ip> [OID]|communities <list|file>]`** — Probe SNMP v1/v2c on UDP port 161 with the built-in community list (`public`, `private`, and more) and retrieve `sysDescr` to identify network devices (routers, switches, printers). `walk` dumps a MIB subtree (default `system`). `communities` overrides the list for the session, either as `c1,c2,...` or a path to a file; `/mnt/ghostesp/snmp_communities.txt` (one community per line) is loaded automatically when present.
- **`enumscan [subnet [a.b.c.]]|<ip>`** — SMB enumeration: negotiates SMB1/SMB2, reports OS, hostname, domain, dialect, and whether SMB signing is required, then lists shares and users via null-session RAP where the server allows it.
- **`dhcpstarve <start [threads]|stop|display>`** — Flood a DHCP server or show collected leases.
- **`mdnssniff <IP|all>`** / **`mdnssniff stop`** — Passively sniff local names (mDNS/LLMNR/SSDP/NetBIOS) per host; no spoofing, auto-saves when Auto Save Scans is on.
- **`capture <-probe|-deauth|-beacon|-raw|-eapol|-wps|-pwn|-list|-export|-wireshark|-wiresharkble|-ble|-skimmer|-stop>`** — Start packet captures for the specified frame type to SD. ESP32-C5/C6 also supports `-802154` for 802.15.4 capture.

### Output

- **`powerprinter [ip text font alignment]`** — Send formatted PCL text jobs to LAN printers; pull saved defaults when arguments are omitted.
- **`dialconnect`** — Pair with a DIAL-capable device (e.g., Chromecast/YouTube).
- **`wol <MAC|IP> [broadcast IP]`** — Send a Wake-on-LAN magic packet; an IP is resolved while its host is awake, and the broadcast address defaults to `255.255.255.255`.
- **`govee scan`** / **`govee <IP> on|off|brightness N|color RRGGBB`** — Discover and control LAN-enabled Govee lights.

## BLE
*(ESP32-S2 excluded)*

### Discovery

- **`blescan [-f|-ds|-a|-r|-adv|-g|-s]`** — Scan for BLE devices, Flippers, spam detectors, raw advertising, or GATT services; `-s` stops.
- **`blewardriving [-s]`** — Log BLE beacons with GPS metadata.

### Spoofing

- **`blespam [mode|-s]`** — Emit spoofed BLE advertisements (Apple, Microsoft, Samsung, Google, random).
- **`spoofairtag`** / **`stopspoof`** — Launch or stop AirTag spoofing.

### Devices

- **`listflippers`** — Scan for nearby Flipper Zero devices.
- **`selectflipper <idx>`** — Choose a Flipper from the discovered list for interactions.
- **`listairtags`** — Discover nearby AirTags.
- **`selectairtag <idx>`** — Choose an AirTag for follow-up actions.

### GATT

- **`blescan -g`** — Scan for connectable BLE devices for GATT enumeration.
- **`listgatt`** — List discovered GATT devices with tracker type detection.
- **`selectgatt <idx>`** — Select a device by index for enumeration or tracking.
- **`enumgatt`** — Connect to the selected device and enumerate its GATT services.
- **`trackgatt`** — Track the selected device using real-time RSSI signal strength.

### BadBLE Keyboard

Bluetooth HID keyboard mode (DuckyScript over BLE). BadBLE owns the BLE radio while active — stop it before using BLE scans or the bridge.

- **`badble status`** — Show BadBLE state (advertising / connected / script), advertised name, and host status.
- **`badble list`** — List DuckyScripts in `/mnt/ghostesp/badble/`.
- **`badble run <file>`** — Wait for a paired host and run a DuckyScript. `badble run "Ghost Art (Built-in)"` runs the built-in payload without an SD card.
- **`badble stop`** — Stop BadBLE; WiFi/AP are restored.
- **`badble keyboard_start`** / **`keyboard_stop`** — Enable / disable live typing mode.
- **`badble type <text>`** — Type text on the connected host.
- **`badble keysend <modifier> <keycode>`** — Send a single HID keypress (numeric modifier and keycode values).
- **`badble name`** — Show the advertised device name.
- **`badble set_name <text>`** — Set the advertised name (max 31 characters, persisted in NVS).

See [BadBLE]({{< relref "../ble/badble.md" >}}) for pairing and usage details.

### Aerial Detection

- **`aerialscan [seconds]`** — Scan for aerial devices (drones, UAVs, RC controllers) using WiFi and BLE in sequential phases. Default: 30 seconds. Phase 1: WiFi scan (OpenDroneID WiFi, DJI WiFi, drone networks). Phase 2: BLE scan (OpenDroneID BLE, DJI BLE) — **WiFi automatically suspended during BLE phase and restored after**.
- **`aeriallist`** — Display all detected aerial devices with full details including device ID, type, MAC address, vendor, signal strength (RSSI), GPS coordinates, altitude, speed, direction, operator location, and flight status.
- **`aerialtrack <idx|mac>`** — Track a specific aerial device by index or MAC address (e.g., `aerialtrack 0` or `aerialtrack 12:34:56:78:9a:bc`).
- **`aerialstop`** — Stop aerial device scanning and tracking.
- **`aerialspoof [device_id lat lon alt]`** — Broadcast fake drone RemoteID for testing via BLE. Without arguments, uses default test drone (GHOST-TEST at San Francisco, 100m altitude). With arguments: device ID, latitude, longitude, altitude in meters. Example: `aerialspoof DRONE-1234 40.7128 -74.0060 100`. Complies with ASTM F3411 OpenDroneID standard. **Note: WiFi automatically suspended during broadcast, restored on stop**.
- **`aerialspoofstop`** — Stop broadcasting fake drone RemoteID and restore WiFi.

## Portal

- **`startportal <path|default> <AP_SSID> [PSK]`** — Serve an Evil Portal bundle from SD or flash (`default` uses the built-in portal).
- **`stopportal`** — Shut down the active portal.
- **`listportals`** — List bundles on SD card or flash.
- **`evilportal -c <sethtmlstr|clear>`** — Manage the Evil Portal HTML buffer (`-c sethtmlstr` to capture inbound HTML, `-c clear` to revert to defaults).
- **`webauth [on|off|toggle|status]`** — Require or disable web UI login. `toggle` flips the current state; `status` shows the current setting.

### DNS Sinkhole

- **`sinkhole start [upstream_dns] [log]`** — Start the DNS sinkhole server. Default upstream DNS: `1.1.1.1`. Use `log` to enable query logging.
- **`sinkhole stop`** — Stop the DNS sinkhole server.
- **`sinkhole status`** — Show sinkhole status and statistics.
- **`sinkhole download [n]`** — Download blocklist. `n` selects the source: `1`=Peter Lowe, `2`=OISD Basic, `3`=StevenBlack. Defaults to OISD Basic.
- **`sinkhole load <filename>`** — Load a custom blocklist from SD.
- **`sinkhole add <domain>`** — Add a domain to the blocklist.
- **`sinkhole remove <domain>`** — Remove a domain from the blocklist.
- **`sinkhole reload`** — Reload the blocklist from storage.
- **`sinkhole log <on|off>`** — Toggle query logging.

## GhostLink (Dual Communication)

- **`commdiscovery`** — Start discovery mode to find other GhostESP devices.
- **`commconnect <peer_name>`** — Connect to a discovered peer (after `commdiscovery`).
- **`commsetpins <tx> <rx>`** — Save preferred pins.
- **`commsend <command> [data...]`** — Issue commands to the connected peer.
- **`commstatus`** — Inspect current link state.
- **`commdisconnect`** — Close the peer link.

## Storage

### File Operations

- **`sd status`** — Show SD card mount status, type (physical/virtual), capacity, and usage percentage.
- **`sd list [path]`** — List files and directories with indices for quick reference. Default path: `/mnt/ghostesp`.
- **`sd info <index|path>`** — Display file or directory details (type, size, path).
- **`sd size <index|path>`** — Get file size in bytes (for pre-download checks).
- **`sd read <index|path> [offset] [length]`** — Read file with optional offset and length for chunked downloads. No size limit.
- **`sd write <path> <base64data>`** — Create/overwrite file with base64-decoded data.
- **`sd append <path> <base64data>`** — Append base64-decoded data to file.
- **`sd mkdir <path>`** — Create a new directory.
- **`sd rm <index|path>`** — Delete a file or empty directory.
- **`sd tree [path] [depth]`** — Recursive directory listing (default depth: 2, max: 10).

All `sd` commands return machine-parsable output with prefixes like `SD:OK:`, `SD:ERR:`, `SD:FILE:[n] name size`, `SD:DIR:[n] name`, `SD:READ:BEGIN:`, `SD:READ:END:`, `SD:WRITE:`.

### Pin Configuration

- **`sd_config`** — Display SD mode, pins, and status.
- **`sd_pins_spi <cs> <clk> <miso> <mosi>`** — Configure SPI wiring.
- **`sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>`** — Configure SDIO wiring.
- **`sd_save_config`** — Persist SD settings to storage.

## NRF24 Analyzer

Available on boards with `CONFIG_HAS_NRF24` or `CONFIG_HAS_NRF24_REMOTE`.

- **`nrf24 start`** — Start NRF24 frequency analysis and jamming detection.
- **`nrf24 pause`** — Pause analysis without stopping.
- **`nrf24 resume`** — Resume paused analysis.
- **`nrf24 status`** — Show current frequency, channel, detected signals, and jamming status.
- **`nrf24 stop`** — Stop NRF24 analysis.

## LoRa

Available on boards with `CONFIG_HAS_LORA` (SX1262-family radio).

- **`lora setup`** — Print the first-run questionnaire (region required before TX).
- **`lora set region <name>`** — Persist the LoRa band plan (e.g., `anz`, `us915`, `eu868`). See [LoRa]({{< relref "../lora/_index.md" >}}) for the full name list and frequencies.
- **`lora set tx <dbm>`** / **`lora set sf <5-12>`** / **`lora set bw <125|250|500>`** — Radio parameters (presets recommended; `set` takes effect on next `lora start`).
- **`lora set companion <ble|wifi>`** — Whether the BLE PhoneAPI app link rides alongside LoRa (no-PSRAM: WiFi XOR BLE).
- **`lora start`** / **`lora stop`** — Start or stop the radio and BLE link.
- **`lora chat [text]`** — Without text, lists recent messages; with text, broadcasts `TEXT_MESSAGE_APP` on the mesh.
- **`lora nodes`** — Show discovered peers with RSSI/SNR and short names.
- **`lora ble [on|off|status]`** — Manage BLE advertising for the official Meshtastic app.
- **`lora app`** / **`lora diag`** — Link stats and `tx_ok/fail/relay rx_ok dups duty_drops`.
- **`lora cad`** — Run five channel-activity trials with RSSI.
- **`lora reg <hex-address> [count]`** — Read up to eight consecutive SX1262 registers; `lora reg 0740 2` verifies the SX1262-encoded Meshtastic sync word `24 B4`.

On-device: **Menu → LoRa** mirrors these with selectable rows. See the [LoRa guides]({{< relref "../lora/_index.md" >}}) for wiring, BLE pairing, and mesh framing details.

## SubGHz

Available on boards with `CONFIG_HAS_SUBGHZ` (CC1101 hardware).

- **`subghz start`** — Start scanning the current frequency band (alias: `subghz waterfall_start` for waterfall mode).
- **`subghz stop`** — Stop scanning (alias: `subghz waterfall_stop`).
- **`subghz pause`** / **`subghz resume`** — Pause or resume scanning.
- **`subghz status`** — Show scanner state, active snapshot, and CC1101 pin configuration.
- **`subghz cycle_freq`** — Cycle to the next frequency band.
- **`subghz capture_begin <normal|raw> <frequency_hz>`** — Arm the CC1101 to capture a signal at a frequency (e.g., `subghz capture_begin normal 433920000`).
- **`subghz capture_on`** / **`subghz capture_off`** — Toggle raw capture mode.
- **`subghz capture [name_hint]`** — Capture the current signal as a snapshot (optionally hinting a name).
- **`subghz save [name_hint]`** — Save the active snapshot to `/mnt/ghostesp/subghz/<name>.sub`.
- **`subghz list`** — List snapshots on SD.
- **`subghz load <name>`** — Load a snapshot for transmission (defaults to `last`).
- **`subghz replay`** — Transmit the loaded snapshot.

For protocol documentation, see the [SubGHz Protocols]({{< relref "../subghz/protocols.md" >}}) guide.

## Audio

Available on boards with `CONFIG_HAS_AUDIO_PLAYER` or `CONFIG_HAS_MIC`.

- **`audio start`** — Start the audio receiver (boards with `CONFIG_HAS_TLV320DAC_I2S`).
- **`audio stop`** — Stop the audio receiver.
- **`audio pause`** — Pause the audio receiver.
- **`audio flush`** — Flush the audio buffers.
- **`audio state <width> <height> [played_ms]`** — Report receiver state (used by the companion app over GhostLink).
- **`mic_cal`** — Restart microphone calibration (boards with `CONFIG_HAS_MIC`). Resets the MIC RGB visualizer baseline.

## Visualizer (Rave Mode)

- **`rave on`** — Enable Rave Mode (display-based LED visualizer synced to music via microphone or line-in).
- **`rave off`** — Disable Rave Mode.
- **`raveport`** — Show the Rave UDP receiver port (default: 6677). Note: port-setting is currently a stub.

Rave Mode streams visualization data over UDP. Use the `rave_helper.bat` or `rave_tray.exe` app on your PC to receive and display the visualizer. See the [Visualizer app]({{< relref "../apps/visualizer.md" >}}) guide.

## Screen Mirroring

- **`mirror on`** — Start screen mirroring server (wired USB).
- **`mirror off`** — Stop screen mirroring.
- **`mirror refresh`** — Refresh the mirror connection.
- **`mirror status`** — Show mirror server status.

For wired mirroring, use `python ghost_mirror.py` on your PC with `--baud 460800` for CYD devices or `--list` to see available ports. For web-based mirroring, visit [ghostesp.net/serial](https://ghostesp.net/serial) and use the Screen Mirror tab.

## Input & Identity

- **`input`** — Show current button/encoder input state.
- **`identify`** — Display board identification info (model, MAC address, build config).
- **`time`** — Show current system time.
- **`settime <unix_timestamp>`** — Set system time manually (seconds since the Unix epoch, e.g., `settime 1704067200`).

## Flock Detection

- **`flockscan`** — Start scanning for Flock Safety camera wireless signals.
- **`flocklist`** — List detected Flock cameras with MAC prefix, signal strength, and confidence level.
- **`flockstop`** — Stop Flock camera detection.

Flock Safety cameras use known MAC prefixes (e.g., `FC:A1:3E`, `F0:A1:C0`). Detection is confidence-based: HIGH if wildcard probe is sent or SSID keyword matches, LOW if only OUI match.

## Camera

Available on boards with **`CONFIG_HAS_CAMERA`** (XIAO ESP32-S3 Sense and compatible boards).

- **`camerastream start`** — Start the MJPEG camera stream server.
- **`camerastream stop`** — Stop the camera stream.
- **`camerastream status`** — Show stream status and settings.
- **`camerastream quality <1-100>`** — Set JPEG quality.
- **`camerastream resolution <name>`** — Set resolution: QQVGA, QVGA, VGA, SVGA, XGA, SXGA, UXGA.
- **`camerastream fps <1-30>`** — Set target frames per second.
- **`motion start|stop|status`** — Start or stop motion detection.
- **`motion threshold <1-255>`** — Set pixel difference threshold (higher = less sensitive).
- **`motion interval <100-10000>`** — Set minimum time between frames in ms.
- **`motion percent <1-100>`** — Set motion trigger percentage (higher = more motion required).
- **`motion sample <1-32>`** — Set pixel sampling rate.
- **`motion snap <on|off>`** — Enable or disable SD card snapshots on motion.
- **`motion image <on|off>`** — Attach snapshot image to Discord webhook alerts.
- **`motion discord <url>`** — Set Discord webhook URL for motion alerts.
- **`motion cooldown <0-3600000>`** — Set minimum time between webhook alerts in milliseconds (max 1 hour). Persists across reboots.

For full setup, tuning, SD snapshots, and Discord webhook configuration, see the [Motion Detector]({{< relref "../camera/motion-detector.md" >}}) guide.

## RGB

- **`rgbmode <effect|color|off>`** — Run an LED effect immediately. Effects: `rainbow`, `police`, `strobe`, `knight`, `off`. Solid colors: `red`, `green`, `blue`, `yellow`, `twh-purple`, `cyan`, `orange`, `white`, `pink`.
- **`setrgbmode <normal|rainbow|stealth>`** — Persist the LED mode across reboots (only accepts `normal`, `rainbow`, or `stealth`). Use `rgbmode` to set any mode temporarily.
- **`setrgbpins <r> <g> <b>`** — Override discrete RGB GPIOs; pass the same pin for all three values to switch into single-wire NeoPixel mode on that data pin.
- **`setrgbcount <1-512>`** — Persist the number of RGB LEDs connected so effects span the correct length. Reinitializes immediately if pins are already configured.
- **`setneopixelbrightness <0-100>`** / **`getneopixelbrightness`** — Control NeoPixel intensity.

## Status display (if present)

Available on boards with an onboard OLED status display or when an external status display is configured.

- **`statusidle [list|set <mode>]`** — View or change the status OLED idle animation when `CONFIG_WITH_STATUS_DISPLAY` and a status display are enabled.
  - `statusidle` — Show the current idle animation and timeout.
  - `statusidle list` — List available idle animations.
  - `statusidle set <mode>` — Select the idle animation mode. Available modes: `life`, `ghost`, `starfield`, `hud`, `matrix`, `ghosts`, `spiral`, `leaves`, `bouncing`, or numeric `0`-`8`.

## IO expander buttons (if present)

Available on boards with **CONFIG_USE_IO_EXPANDER**. Three physical buttons (P10, P11 “Right button”, P12) can each run a custom CLI command or act as a joystick button when no command is set.

- **`iobtn <1|2|3> [command]`** — View or set the command for button 1 (P10), 2 (P11), or 3 (P12). Without `command`, prints the current command (or “(none)”). With `command`, saves it and runs it on the next press. Example: `iobtn 1 nfc read`.
- **`settings get io_btn_p10_cmd`** / **`settings set io_btn_p10_cmd <value>`** — Same for P10; use `io_btn_p11_cmd` and `io_btn_p12_cmd` for P11 and P12.

On press, the device switches to the terminal view and runs the command. To use a button as a normal joystick action instead, clear its command (e.g. `iobtn 1 ""` or `settings set io_btn_p10_cmd ""`).

**On-device UI:** **Settings → IO Buttons** lets you edit each button’s command with the keyboard; the current command is pre-filled when editing.

## Infrared

- **`ir list [path]`** — List `.ir` files (default: `/mnt/ghostesp/infrared/remotes`).
- **`ir show <path|remote_index>`** — Parse and display signals from an IR file. After `ir list`, you can pass a numeric remote index.
- **`ir send <path|remote_index> [button_index]`** — Transmit a signal from a file. Use `remote_index` from `ir list` and optional `button_index` from `ir show`.
- **`ir universals list [-all]`** — List universal IR files and, with `-all`, all built‑in universal signals.
- **`ir universals send <index>`** — Transmit a built‑in universal signal by index (see `ir universals list -all`).
- **`ir universals sendall <file|TURNHISTVOFF> <button_name> [delay_ms]`** — Transmit all signals for a named button from a universal file or the built‑in TURNHISTVOFF set. The built-in signals use the name `POWER` (e.g., `ir universals sendall TURNHISTVOFF POWER`). Can be stopped with `stop`.
- **`ir rx [timeout]`** — Wait up to `timeout` seconds (default 60) for a single IR signal, print it (decoded or RAW), then stop.
- **`ir learn [path]`** — Wait for a signal (10s). Without `path`, auto-create a new `.ir` file under `/mnt/ghostesp/infrared/remotes`; with `path`, append the learned signal to that file.
- **`ir dazzler [stop]`** — Start/stop continuous IR dazzler flood. Responses are machine-parsable: `IR_DAZZLER:STARTED`, `IR_DAZZLER:FAILED`, `IR_DAZZLER:ALREADY_RUNNING`, `IR_DAZZLER:STOPPING`, `IR_DAZZLER:NOT_RUNNING`.
- **`[IR/BEGIN]` / `[IR/CLOSE]` (UART IR envelope)**
  - **Usage:** Send `[IR/BEGIN]`, then a single IR message body, then `[IR/CLOSE]` on the same UART stream to trigger a one‑off transmit.
  - **Body format (`.ir` text block):** Same fields as a standard `.ir` file entry (for example: name, type, protocol, address, command).
  - **Body format (JSON):** Single JSON object carrying the same information as a `.ir` entry (parsed signal fields or raw timing data).
  - **Examples:**

    ```text
    [IR/BEGIN]
    name=Power
    type=parsed
    protocol=NEC
    addr=0x0000FFFF
    cmd=0x0000E718
    [IR/CLOSE]
    ```

    ```json
    [IR/BEGIN]
    {"name":"Power","type":"parsed","protocol":"NEC","addr":"0x0000FFFF","cmd":"0x0000E718"}
    [IR/CLOSE]
    ```

  - **CLI response on success:** `IR: send OK`, followed by a compact summary:
    - Parsed: `IR: signal [Name] protocol=NEC addr=0x0000FFFF cmd=0x0000E718`
    - Raw: `IR: signal raw len=N freq=38000Hz duty=0.33`

## GPS

- **`gpspin [pin]`** — View or set the GPS RX pin for external GPS modules. Without arguments, shows current pin. Setting persists to NVS; restart GPS commands to apply.
- **`gpsinfo [-s]`** — Stream current fix, satellites, and speed; pass `-s` to stop the display task.
- **`startwd [-s] [--helper] [--channels <csv>] [--hop <ms>] [--weighted]`** — Start wardriving (logs Wi-Fi/GPS to CSV). Use `-s` to stop. Use `--helper` to enable the GhostLink split-channel helper. Use `--channels` to specify a CSV of channels to hop (e.g., `1,6,11`). Use `--hop` to set the channel hop interval in ms (default: 125). Use `--weighted` to enable 5GHz weighted scanning.

## Ethernet
*(Requires `CONFIG_WITH_ETHERNET`)*

### Connection Management

- **`ethup`** — Initialize and bring up Ethernet interface; waits for link establishment and DHCP assignment.
- **`ethdown`** — Deinitialize and bring down Ethernet interface.
- **`ethinfo`** — Display Ethernet connection information (status, IP address, netmask, gateway, DNS servers, DHCP server).
- **`webuiap [on|off|toggle|status]`** — Restrict the web UI to clients connected to the onboard AP subnet (AP-only mode).

### Network Scanning

- **`ethfp`** — Fingerprint network hosts using mDNS, NetBIOS, and SSDP (discovers Apple devices, Chromecasts, printers, Windows PCs, routers, smart TVs).
- **`etharp`** — Perform ARP scan on local Ethernet network subnet (1-254) to discover active hosts.
- **`ethping`** — Perform ICMP ping scan on local Ethernet network subnet (1-254) to find alive hosts.
- **`ethports [ip] [all|start-end]`** — Scan TCP ports on a target IP address.
  - Without arguments: scans common ports on the gateway.
  - `local`: scan the local device itself.
  - `all`: scan all ports (1-65535).
  - `start-end`: custom port range (e.g., `80-443`).
  - Examples: `ethports`, `ethports 192.168.1.1`, `ethports 192.168.1.1 all`, `ethports 192.168.1.1 80-443`.

### Network Tools

- **`ethdns <hostname>`** — Perform forward DNS lookup.
- **`ethdns reverse <ip_address>`** — Perform reverse DNS lookup.
- **`ethtrace <hostname_or_ip> [max_hops]`** — Perform traceroute to a target host (default: 30 hops, max: 64).
- **`ethserv [ip_address]`** — Service discovery and banner grabbing on a target IP (default: gateway). Scans common services (FTP, SSH, Telnet, SMTP, HTTP, HTTPS, etc.).
- **`ethhttp <url> [lines|all]`** — Send HTTP/HTTPS GET request to a server and display response.
  - Default: shows first 25 lines
  - `[lines]`: show first N lines (e.g., `ethhttp http://example.com 50`)
  - `all`: show full response (e.g., `ethhttp http://example.com all`)
  - Supports both HTTP and HTTPS (TLS 1.2)
  - Examples: `ethhttp http://example.com`, `ethhttp https://www.google.com 100`, `ethhttp http://192.168.1.1/index.html all`
- **`ethntp [ntp_server]`** — Query NTP server and synchronize system time. Default server: `pool.ntp.org`. Examples: `ethntp`, `ethntp pool.ntp.org`, `ethntp time.google.com`.

### Configuration

- **`ethconfig dhcp`** — Use DHCP for automatic IP assignment.
- **`ethconfig static <ip> <netmask> <gateway>`** — Set static IP configuration.
  - Example: `ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1`
- **`ethconfig show`** — Show current IP configuration.
- **`ethmac`** — Display current MAC address.
- **`ethmac set <xx:xx:xx:xx:xx:xx>`** — Set Ethernet MAC address (may require reinitialization).
  - Example: `ethmac set 02:00:00:00:00:01`

### Statistics

- **`ethstats`** — Display Ethernet network statistics (link status, IP info, MAC address, packet statistics, ARP statistics).

## WiGLE

- **`wigle API <APIName>:<APIToken>`** — Set WiGLE credentials. Get your token from [wigle.net/account](https://wigle.net/account).
- **`wigle auto <on|off>`** — Enable or disable automatic upload when WiFi STA connects.
- **`wigle donate <on|off>`** — Enable or disable the donate flag (recommended: `on`).
- **`wigle show`** — Display current WiGLE settings and API key status.
- **`wigle list`** — List stored uploaded CSV memory.
- **`wigle files`** — List pending CSV files in `/mnt/ghostesp/gps/`.
- **`wigle upload <filename>`** — Upload a CSV file to WiGLE.
- **`wigle upload all`** — Upload all pending CSV files.
- **`wigle stats`** — Show WiGLE account statistics.

## Settings

- **`settings list`** — Dump available configuration keys.
- **`settings help`** — Show supported subcommands.
- **`settings get <key>`** / **`settings set <key> <value>`** — Inspect or change individual options.
- **`settings reset [key]`** — Restore all settings or a specific key to defaults.
- **`loadconfig`** — Load settings from `config.cfg` on the SD card (SSID, PASSKEY, WiGLE token, auto-upload, donate, auto-reconnect).

## Native SD Apps

Available on builds with `CONFIG_ENABLE_NATIVE_SD_APPS`.

- **`apps list`** — List all discovered SD apps.
- **`apps reload`** — Rescan the apps and packages directories for new or removed apps.
- **`apps info <id>`** — Show manifest details and failure diagnostic state for an app.
- **`apps run <id>`** — Launch an app by ID.
- **`apps stop`** — Stop the currently running app.
- **`apps reset <id>`** — Clear failure diagnostic state for an app.

## BadUSB

- **`badusb list`** — List scripts in `/mnt/ghostesp/badusb/`.
- **`badusb run <filename>`** — Run a script from `/mnt/ghostesp/badusb/`.
- **`badusb stop`** — Stop the current BadUSB run.
- **`badusb exec <size>`** — Prepare for a streamed script.
- **`badusb set_vid <hex>`** — Set USB VID for the next run.
- **`badusb set_pid <hex>`** — Set USB PID for the next run.
- **`badusb set_mfr <text>`** — Set USB manufacturer for the next run.
- **`badusb set_prod <text>`** — Set USB product for the next run.
- **`badusb set_rand <0|1>`** — Toggle per-run USB detail randomization.
- **`badusb set_layout <n>`** — Set keyboard layout for the next run (`0` US, `1` DE, `2` FR, `3` UK, `4` ES).

## USB Keyboard

- **`usbkbd [on|off|status]`** — Enable, disable, or check USB HID keyboard host mode (ESP32-S3 only).
