/**
 * GhostESP Command Registry
 * Ported from Android GhostCommand.kt
 *
 * Provides type-safe command generation with metadata:
 * - isRisky: may disconnect WebUI / disrupt radio
 * - requiresStopFirst: should send 'stop' before this command
 * - category: UI grouping
 * - description: human-readable help
 */

const CMD = {
  // Core
  help:            () => ({ cmd: 'help',            risky: false, stopFirst: false, cat: 'System', desc: 'Show help' }),
  chipinfo:        () => ({ cmd: 'chipinfo',        risky: false, stopFirst: false, cat: 'System', desc: 'Chip information' }),
  stop:            () => ({ cmd: 'stop',            risky: false, stopFirst: false, cat: 'System', desc: 'Stop all operations' }),
  reboot:          () => ({ cmd: 'reboot',          risky: true,  stopFirst: false, cat: 'System', desc: 'Reboot device' }),
  identify:        () => ({ cmd: 'identify',        risky: false, stopFirst: false, cat: 'System', desc: 'Identify device' }),
  mem:             (sub = 'status') => ({ cmd: sub === 'status' ? 'mem' : `mem ${sub}`, risky: false, stopFirst: false, cat: 'System', desc: 'Memory info' }),

  // WiFi Scan
  scanAp:          (duration, live) => ({ cmd: live ? 'scanap -live' : duration ? `scanap ${duration}` : 'scanap', risky: true, stopFirst: true, cat: 'WiFi Scan', desc: 'Scan access points' }),
  scanSta:         () => ({ cmd: 'scansta',         risky: true, stopFirst: true, cat: 'WiFi Scan', desc: 'Scan stations' }),
  scanAll:         (duration) => ({ cmd: duration ? `scanall ${duration}` : 'scanall', risky: true, stopFirst: true, cat: 'WiFi Scan', desc: 'Combined AP + STA scan' }),
  stopScan:        () => ({ cmd: 'stopscan',        risky: false, stopFirst: false, cat: 'WiFi Scan', desc: 'Stop WiFi scan' }),
  listAp:          () => ({ cmd: 'list -a',         risky: false, stopFirst: false, cat: 'WiFi Scan', desc: 'List AP results' }),
  listSta:         () => ({ cmd: 'list -s',         risky: false, stopFirst: false, cat: 'WiFi Scan', desc: 'List station results' }),

  // WiFi Select
  selectAp:        (indices) => ({ cmd: `select -a ${indices}`, risky: false, stopFirst: false, cat: 'WiFi Select', desc: 'Select AP by index' }),
  selectSta:       (indices) => ({ cmd: `select -s ${indices}`, risky: false, stopFirst: false, cat: 'WiFi Select', desc: 'Select station by index' }),
  connect:         (ssid, pass) => {
    if (!ssid) return { cmd: 'connect', risky: false, stopFirst: false, cat: 'WiFi Select', desc: 'Connect to saved WiFi' };
    return { cmd: pass ? `connect "${ssid}" "${pass}"` : `connect "${ssid}"`, risky: true, stopFirst: false, cat: 'WiFi Select', desc: 'Connect to WiFi' };
  },
  disconnect:      () => ({ cmd: 'disconnect',      risky: false, stopFirst: false, cat: 'WiFi Select', desc: 'Disconnect WiFi' }),
  wifiStatus:      () => ({ cmd: 'wifistatus',      risky: false, stopFirst: false, cat: 'WiFi Select', desc: 'WiFi connection status' }),

  // WiFi Attacks
  deauth:          () => ({ cmd: 'attack -d',       risky: true, stopFirst: true, cat: 'WiFi Attack', desc: 'Deauth attack on selected APs' }),
  eapol:           () => ({ cmd: 'attack -e',       risky: true, stopFirst: true, cat: 'WiFi Attack', desc: 'EAPOL logoff attack' }),
  stopDeauth:      () => ({ cmd: 'stopdeauth',      risky: false, stopFirst: false, cat: 'WiFi Attack', desc: 'Stop deauth attack' }),
  beaconSpam:      (mode = 'random') => {
    const map = { random: '-r', rickroll: '-rr', list: '-l' };
    return { cmd: `beaconspam ${map[mode] || mode}`, risky: true, stopFirst: true, cat: 'WiFi Attack', desc: 'Beacon spam' };
  },
  stopSpam:        () => ({ cmd: 'stopspam',        risky: false, stopFirst: false, cat: 'WiFi Attack', desc: 'Stop beacon spam' }),
  karmaStart:      (ssids) => ({ cmd: ssids?.length ? `karma start ${ssids.map(s => /\s/.test(s) ? `"${s}"` : s).join(' ')}` : 'karma start', risky: true, stopFirst: true, cat: 'WiFi Attack', desc: 'Start karma attack' }),
  karmaStop:       () => ({ cmd: 'karma stop',      risky: false, stopFirst: false, cat: 'WiFi Attack', desc: 'Stop karma attack' }),
  saeFlood:        (pass) => ({ cmd: `saeflood ${pass}`, risky: true, stopFirst: true, cat: 'WiFi Attack', desc: 'SAE flood attack' }),
  stopSaeFlood:    () => ({ cmd: 'stopsaeflood',    risky: false, stopFirst: false, cat: 'WiFi Attack', desc: 'Stop SAE flood' }),

  // WiFi Tracking
  trackAp:         () => ({ cmd: 'trackap',         risky: true, stopFirst: true, cat: 'WiFi Track', desc: 'Track selected AP RSSI' }),
  trackSta:        () => ({ cmd: 'tracksta',        risky: true, stopFirst: true, cat: 'WiFi Track', desc: 'Track selected station RSSI' }),

  // WiFi Capture
  capture:         (mode, channel) => {
    const m = { probe: '-probe', deauth: '-deauth', beacon: '-beacon', raw: '-raw', wps: '-wps', eapol: '-eapol', ieee802154: '-802154' };
    let c = `capture ${m[mode] || mode}`;
    if (channel != null) c += ` -c ${channel}`;
    return { cmd: c, risky: true, stopFirst: true, cat: 'WiFi Capture', desc: `Capture ${mode}` };
  },
  stopCapture:     () => ({ cmd: 'capture -stop',   risky: false, stopFirst: false, cat: 'WiFi Capture', desc: 'Stop capture' }),
  startEapol:      (ch) => ({ cmd: ch != null ? `capture -eapol -c ${ch}` : 'capture -eapol', risky: true, stopFirst: true, cat: 'WiFi Capture', desc: 'Start EAPOL capture' }),

  // Environment
  sweep:           (stop) => ({ cmd: stop ? 'stop' : 'sweep', risky: !stop, stopFirst: !stop, cat: 'Environment', desc: 'WiFi/BLE/GPS sweep' }),
  pineap:          () => ({ cmd: 'pineap',          risky: true, stopFirst: true, cat: 'Environment', desc: 'Detect WiFi Pineapples' }),
  congestion:      () => ({ cmd: 'congestion',      risky: false, stopFirst: false, cat: 'Environment', desc: 'Channel congestion' }),
  listenProbes:    (stop) => ({ cmd: stop ? 'listenprobes -s' : 'listenprobes', risky: !stop, stopFirst: !stop, cat: 'Environment', desc: 'Listen for probe requests' }),

  // Network
  scanPorts:       (target, start, end) => {
    let c = `scanports ${target}`;
    if (start != null) c += ` ${start}`;
    if (end != null) c += ` ${end}`;
    return { cmd: c, risky: false, stopFirst: false, cat: 'Network', desc: 'Port scan' };
  },
  scanArp:         () => ({ cmd: 'scanarp',         risky: false, stopFirst: false, cat: 'Network', desc: 'ARP scan local network' }),
  scanLocal:       () => ({ cmd: 'scanlocal',       risky: false, stopFirst: false, cat: 'Network', desc: 'Local IP lookup' }),
  scanSsh:         (target) => ({ cmd: `scanssh ${target}`, risky: false, stopFirst: false, cat: 'Network', desc: 'Scan for SSH' }),
  dhcpStarve:      (stop) => ({ cmd: stop ? 'dhcpstarve stop' : 'dhcpstarve start', risky: !stop, stopFirst: !stop, cat: 'Network', desc: 'DHCP starvation' }),

  // Evil Portal
  startPortal:     (path, ssid, password) => {
    let c = `startportal ${path || 'default'} "${ssid || 'FreeWiFi'}"`;
    if (password) c += ` "${password}"`;
    return { cmd: c, risky: true, stopFirst: true, cat: 'Portal', desc: 'Start evil portal' };
  },
  stopPortal:      () => ({ cmd: 'stopportal',      risky: false, stopFirst: false, cat: 'Portal', desc: 'Stop evil portal' }),
  listPortals:     () => ({ cmd: 'listportals',     risky: false, stopFirst: false, cat: 'Portal', desc: 'List portal files' }),

  // BLE Scan
  bleScan:         (mode, stop) => {
    const m = { flipper: '-f', spam: '-ds', airtag: '-a', raw: '-r', gatt: '-g' };
    return { cmd: stop ? 'blescan -s' : `blescan ${m[mode] || ''}`.trim(), risky: !stop, stopFirst: !stop, cat: 'BLE Scan', desc: `BLE scan ${mode}` };
  },
  bleSpam:         (mode, stop) => {
    const m = { apple: '-apple', microsoft: '-ms', samsung: '-samsung', google: '-google', random: '-random' };
    return { cmd: stop ? 'blespam -s' : `blespam ${m[mode] || ''}`.trim(), risky: !stop, stopFirst: !stop, cat: 'BLE Attack', desc: `BLE spam ${mode}` };
  },
  listFlippers:    () => ({ cmd: 'listflippers',    risky: false, stopFirst: false, cat: 'BLE Scan', desc: 'List Flipper devices' }),
  listAirTags:     () => ({ cmd: 'listairtags',     risky: false, stopFirst: false, cat: 'BLE Scan', desc: 'List AirTags' }),
  listGatt:        () => ({ cmd: 'listgatt',        risky: false, stopFirst: false, cat: 'BLE Scan', desc: 'List GATT devices' }),
  enumGatt:        () => ({ cmd: 'enumgatt',        risky: false, stopFirst: false, cat: 'BLE Scan', desc: 'Enumerate GATT services' }),
  selectGatt:      (indices) => ({ cmd: `selectgatt ${indices}`, risky: false, stopFirst: false, cat: 'BLE Select', desc: 'Select GATT device' }),
  trackGatt:       () => ({ cmd: 'trackgatt',       risky: true, stopFirst: true, cat: 'BLE Track', desc: 'Track GATT device' }),
  trackFlipper:    (index) => ({ cmd: `selectflipper ${index}`, risky: true, stopFirst: true, cat: 'BLE Track', desc: 'Track Flipper' }),
  spoofAirTag:     (start) => ({ cmd: start ? 'spoofairtag' : 'stopspoof', risky: start, stopFirst: start, cat: 'BLE Attack', desc: 'Spoof AirTag' }),
  bleWardrive:     (stop) => ({ cmd: stop ? 'blewardriving -s' : 'blewardriving', risky: !stop, stopFirst: !stop, cat: 'BLE Scan', desc: 'BLE wardriving' }),

  // NFC
  // Chameleon Ultra exposes subcommands like connect/scanhf/scanlf/etc.
  // No single 'scan' verb exists; a full NFC view would need its own UI.

  // IR
  irList:          (path) => ({ cmd: path ? `ir list ${path}` : 'ir list', risky: false, stopFirst: false, cat: 'IR', desc: 'List IR remotes' }),
  irSend:          (remote, btn) => ({ cmd: btn != null ? `ir send ${remote} ${btn}` : `ir send ${remote}`, risky: false, stopFirst: false, cat: 'IR', desc: 'Send IR signal' }),
  irLearn:         (path) => ({ cmd: path ? `ir learn ${path}` : 'ir learn', risky: true, stopFirst: true, cat: 'IR', desc: 'Learn IR signal' }),
  irDazzler:       (stop) => ({ cmd: stop ? 'ir dazzler stop' : 'ir dazzler', risky: !stop, stopFirst: !stop, cat: 'IR', desc: 'IR dazzler' }),
  irShow:          (remote) => ({ cmd: `ir show ${remote}`, risky: false, stopFirst: false, cat: 'IR', desc: 'Show IR remote buttons' }),

  // BadUSB
  badusbList:      () => ({ cmd: 'badusb list',     risky: false, stopFirst: false, cat: 'BadUSB', desc: 'List BadUSB scripts' }),
  badusbRun:       (file) => ({ cmd: `badusb run ${file}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Run BadUSB script' }),
  badusbStop:      () => ({ cmd: 'badusb stop',     risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Stop BadUSB' }),
  badusbRunBuiltin:() => ({ cmd: 'badusb run "Ghost Art (Built-in)"', risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Run built-in Ghost Art script' }),
  badusbKeyboardStart: () => ({ cmd: 'badusb keyboard_start', risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Enable USB keyboard mode' }),
  badusbKeyboardStop:  () => ({ cmd: 'badusb keyboard_stop',  risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Disable USB keyboard mode' }),
  badusbType:      (text) => ({ cmd: `badusb type ${JSON.stringify(text || '')}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Type via USB keyboard' }),
  badusbTypeChar:  (ch) => ({ cmd: `badusb type_char ${(ch || '').charCodeAt(0)}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Send a single ASCII char' }),
  badusbKeysend:   (modifier, keycode) => ({ cmd: `badusb keysend ${modifier} ${keycode}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Send single HID key (modifier, keycode)' }),
  badusbJiggleStart: () => ({ cmd: 'badusb jiggle_start', risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Start mouse jiggler' }),
  badusbJiggleStop:  () => ({ cmd: 'badusb jiggle_stop',  risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Stop mouse jiggler' }),
  badusbTrackpadStart: () => ({ cmd: 'badusb trackpad_start', risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Enable relative mouse (trackpad) mode' }),
  badusbTrackpadStop:  () => ({ cmd: 'badusb trackpad_stop',  risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Disable trackpad mode' }),
  badusbTrackpadMove:  (dx, dy) => ({ cmd: `badusb trackpad_move ${dx} ${dy}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Relative mouse move (int8 dx dy)' }),
  badusbTrackpadWheel: (delta) => ({ cmd: `badusb trackpad_wheel ${delta}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Vertical mouse wheel delta (int8)' }),
  badusbTrackpadButton:(mask) => ({ cmd: `badusb trackpad_button ${mask}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Mouse button mask: 1=L 2=R 4=M 0=release' }),
  badusbSetVid:    (hex) => ({ cmd: `badusb set_vid ${hex}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Set BadUSB VID (next run)' }),
  badusbSetPid:    (hex) => ({ cmd: `badusb set_pid ${hex}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Set BadUSB PID (next run)' }),
  badusbSetMfr:    (text) => ({ cmd: `badusb set_mfr ${JSON.stringify(text || '')}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Set manufacturer string' }),
  badusbSetProd:   (text) => ({ cmd: `badusb set_prod ${JSON.stringify(text || '')}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Set product string' }),
  badusbSetRand:   (on) => ({ cmd: `badusb set_rand ${on ? '1' : '0'}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Toggle VID/PID randomize' }),
  badusbSetLayout: (n) => ({ cmd: `badusb set_layout ${n}`, risky: false, stopFirst: false, cat: 'BadUSB', desc: 'Keyboard layout (0=US 1=DE 2=FR 3=UK 4=ES)' }),

  // BadBLE (BLE HID keyboard)
  badbleStatus:    () => ({ cmd: 'badble status',    risky: false, stopFirst: false, cat: 'BadBLE', desc: 'BadBLE state (advertising / connected / script)' }),
  badbleList:      () => ({ cmd: 'badble list',      risky: false, stopFirst: false, cat: 'BadBLE', desc: 'List DuckyScripts in /mnt/ghostesp/badble/' }),
  badbleStart:     () => ({ cmd: 'badble keyboard_start', risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Advertise BLE HID keyboard for live typing' }),
  badbleStop:      () => ({ cmd: 'badble stop',      risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Stop BadBLE and restore BLE stack' }),
  badbleName:      () => ({ cmd: 'badble name',      risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Show advertised BadBLE name' }),
  badbleSetName:   (text) => ({ cmd: `badble set_name ${JSON.stringify(text || '')}`, risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Set advertised BadBLE name (max 31 chars)' }),
  badbleRun:       (file) => ({ cmd: `badble run ${file}`, risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Wait for a BLE host, then run a DuckyScript' }),
  badbleRunBuiltin:() => ({ cmd: 'badble run "Ghost Art (Built-in)"', risky: false, stopFirst: false, cat: 'BadBLE', desc: 'Run the built-in Ghost Art script over BLE' }),

  // GPS
  gpsInfo:         (stop) => ({ cmd: stop ? 'gpsinfo -s' : 'gpsinfo', risky: false, stopFirst: false, cat: 'GPS', desc: 'GPS information' }),
  startWardrive:   (stop) => ({ cmd: stop ? 'startwd -s' : 'startwd', risky: !stop, stopFirst: !stop, cat: 'GPS', desc: 'Start wardriving' }),

  // SD Card
  sdStatus:        () => ({ cmd: 'sd status',       risky: false, stopFirst: false, cat: 'Files', desc: 'SD card status' }),
  sdList:          (path) => ({ cmd: path ? `sd list ${path}` : 'sd list', risky: false, stopFirst: false, cat: 'Files', desc: 'List SD files' }),
  sdRead:          (path, offset, length) => ({ cmd: `sd read ${path}${offset != null ? ' ' + offset : ''}${length != null ? ' ' + length : ''}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Read SD file' }),
  sdSize:          (path) => ({ cmd: `sd size ${path}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Get file size' }),
  sdWrite:         (path, b64) => ({ cmd: `sd write ${path} ${b64}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Write file' }),
  sdAppend:        (path, b64) => ({ cmd: `sd append ${path} ${b64}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Append to file' }),
  sdMkdir:         (path) => ({ cmd: `sd mkdir ${path}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Create directory' }),
  sdRm:            (path) => ({ cmd: `sd rm ${path}`, risky: false, stopFirst: false, cat: 'Files', desc: 'Delete file/dir' }),

  // Aerial
  aerialScan:      (duration, stop) => ({ cmd: stop ? 'aerialstop' : `aerialscan ${duration || 30}`, risky: !stop, stopFirst: !stop, cat: 'Aerial', desc: 'Aerial scan' }),
  aerialList:      () => ({ cmd: 'aeriallist',      risky: false, stopFirst: false, cat: 'Aerial', desc: 'List aerial devices' }),
  aerialTrack:     (id) => ({ cmd: `aerialtrack ${id}`, risky: true, stopFirst: true, cat: 'Aerial', desc: 'Track aerial device' }),
  aerialSpoof:     (id, lat, lon, alt) => ({ cmd: `aerialspoof "${id || 'GHOST-TEST'}" ${lat || 37.7749} ${lon || -122.4194} ${alt || 100}`, risky: true, stopFirst: true, cat: 'Aerial', desc: 'Spoof aerial device' }),
  aerialSpoofStop: () => ({ cmd: 'aerialspoofstop', risky: false, stopFirst: false, cat: 'Aerial', desc: 'Stop aerial spoof' }),

  // Ethernet
  ethUp:           () => ({ cmd: 'ethup',           risky: false, stopFirst: false, cat: 'Ethernet', desc: 'Ethernet up' }),
  ethDown:         () => ({ cmd: 'ethdown',         risky: false, stopFirst: false, cat: 'Ethernet', desc: 'Ethernet down' }),
  ethInfo:         () => ({ cmd: 'ethinfo',         risky: false, stopFirst: false, cat: 'Ethernet', desc: 'Ethernet info' }),
  ethArp:          () => ({ cmd: 'etharp',          risky: false, stopFirst: false, cat: 'Ethernet', desc: 'Ethernet ARP scan' }),

  // Settings
  settingsList:    () => ({ cmd: 'settings list',   risky: false, stopFirst: false, cat: 'Settings', desc: 'List all settings' }),
  settingsGet:     (key) => ({ cmd: `settings get ${key}`, risky: false, stopFirst: false, cat: 'Settings', desc: 'Get setting' }),
  settingsSet:     (key, value) => ({ cmd: `settings set ${key} ${value}`, risky: false, stopFirst: false, cat: 'Settings', desc: 'Set setting' }),
  settingsReset:   (name) => ({ cmd: name ? `settings reset ${name}` : 'settings reset', risky: false, stopFirst: false, cat: 'Settings', desc: 'Reset settings to defaults' }),
  settingsBackupExport: () => ({ cmd: 'settings backup export', risky: false, stopFirst: false, cat: 'Settings', desc: 'Export settings to SD' }),
  settingsBackupImport: () => ({ cmd: 'settings backup import', risky: false, stopFirst: false, cat: 'Settings', desc: 'Import settings from SD' }),

  // GhostLink
  commStatus:      () => ({ cmd: 'commstatus',      risky: false, stopFirst: false, cat: 'GhostLink', desc: 'GhostLink status' }),
  commDiscovery:   () => ({ cmd: 'commdiscovery',   risky: false, stopFirst: false, cat: 'GhostLink', desc: 'Discover peers' }),
  commConnect:     (peer) => ({ cmd: `commconnect ${peer}`, risky: false, stopFirst: false, cat: 'GhostLink', desc: 'Connect to peer' }),
  commSend:        (command, data) => ({ cmd: data ? `commsend ${command} ${data}` : `commsend ${command}`, risky: false, stopFirst: false, cat: 'GhostLink', desc: 'Send via GhostLink' }),
  commDisconnect:  () => ({ cmd: 'commdisconnect',  risky: false, stopFirst: false, cat: 'GhostLink', desc: 'Disconnect GhostLink' }),
  commSetPins:     (tx, rx) => ({ cmd: `commsetpins ${tx} ${rx}`, risky: false, stopFirst: false, cat: 'GhostLink', desc: 'Set GhostLink pins' }),

  // Misc
  dialconnect:     (device, all) => ({ cmd: all ? 'dialconnect all' : device ? `dialconnect ${device}` : 'dialconnect', risky: false, stopFirst: false, cat: 'Misc', desc: 'DIAL connect to TV' }),
  powerprinter:    (ip, text, fontSize, align) => ({ cmd: `powerprinter ${ip} "${text}" ${fontSize || 16} ${align || 'CM'}`, risky: false, stopFirst: false, cat: 'Misc', desc: 'Send to power printer' }),
  mirror:          (sub) => ({ cmd: `mirror ${sub}`, risky: false, stopFirst: false, cat: 'Misc', desc: 'Screen mirror' }),
  rgbmode:         (mode) => ({ cmd: `rgbmode ${mode}`, risky: false, stopFirst: false, cat: 'Misc', desc: 'Set RGB mode' }),
  timezone:        (tz) => ({ cmd: `timezone ${tz}`, risky: false, stopFirst: false, cat: 'Misc', desc: 'Set timezone' }),
  apcred:          (ssid, pass, reset) => {
    if (reset) return { cmd: 'apcred -r', risky: false, stopFirst: false, cat: 'Misc', desc: 'Reset AP creds' };
    if (ssid && pass) return { cmd: `apcred "${ssid}" "${pass}"`, risky: false, stopFirst: false, cat: 'Misc', desc: 'Set AP credentials' };
    return { cmd: 'apcred', risky: false, stopFirst: false, cat: 'Misc', desc: 'Show AP credentials' };
  },
  apenable:        (on) => ({ cmd: `apenable ${on ? 'on' : 'off'}`, risky: true, stopFirst: false, cat: 'Misc', desc: 'Enable/disable AP' }),

  // LoRa (SX1262-family mesh: stock framing + default-key crypto + BLE app link)
  loraStatus:      () => ({ cmd: 'lora status',      risky: false, stopFirst: false, cat: 'LoRa', desc: 'LoRa status' }),
  loraStart:       () => ({ cmd: 'lora start',       risky: true,  stopFirst: true,  cat: 'LoRa', desc: 'Start LoRa radio' }),
  loraStop:        () => ({ cmd: 'lora stop',        risky: false, stopFirst: false, cat: 'LoRa', desc: 'Stop LoRa radio' }),
  loraChat:        (text) => ({ cmd: text ? `lora chat ${text}` : 'lora chat', risky: false, stopFirst: false, cat: 'LoRa', desc: 'Send/list LoRa chat' }),
  loraNodes:       () => ({ cmd: 'lora nodes',       risky: false, stopFirst: false, cat: 'LoRa', desc: 'List LoRa nodes' }),
  loraDiag:        () => ({ cmd: 'lora diag',        risky: false, stopFirst: false, cat: 'LoRa', desc: 'LoRa diagnostics' }),
  loraBle:         (on) => ({ cmd: on == null ? 'lora ble' : on ? 'lora ble on' : 'lora ble off', risky: false, stopFirst: false, cat: 'LoRa', desc: 'LoRa BLE app link' }),
  loraApp:         () => ({ cmd: 'lora app',         risky: false, stopFirst: false, cat: 'LoRa', desc: 'Meshtastic app link status' }),
};

/** Registry of UI action definitions for WiFi page groups */
const WIFI_GROUPS = {
  'Scan & Select': [
    { label: 'Scan Access Points',    factory: () => CMD.scanAp(),           refresh: () => CMD.listAp(),   refreshLabel: 'List APs' },
    { label: 'Scan APs Live',         factory: () => CMD.scanAp(null, true),  refresh: () => CMD.listAp(),   refreshLabel: 'List APs' },
    { label: 'Scan Stations',         factory: () => CMD.scanSta(),           refresh: () => CMD.listSta(),  refreshLabel: 'List Stations' },
    { label: 'Scan AP + STA',         factory: () => CMD.scanAll(),           refresh: () => CMD.listAp(),   refreshLabel: 'List APs' },
    { label: 'List Access Points',    factory: () => CMD.listAp() },
    { label: 'List Stations',         factory: () => CMD.listSta() },
  ],
  'Attacks': [
    { label: 'Start Deauth Attack',   factory: () => CMD.deauth() },
    { label: 'Start EAPOL Logoff',    factory: () => CMD.eapol() },
    { label: 'Beacon Spam - Random',  factory: () => CMD.beaconSpam('random') },
    { label: 'Beacon Spam - Rickroll',factory: () => CMD.beaconSpam('rickroll') },
    { label: 'Beacon Spam - List',    factory: () => CMD.beaconSpam('list') },
    { label: 'Stop Attacks',          factory: () => CMD.stopDeauth() },
    { label: 'Stop Beacon Spam',      factory: () => CMD.stopSpam() },
  ],
  'Capture': [
    { label: 'Capture Probe',         factory: () => CMD.capture('probe') },
    { label: 'Capture Deauth',        factory: () => CMD.capture('deauth') },
    { label: 'Capture Beacon',        factory: () => CMD.capture('beacon') },
    { label: 'Capture Raw',           factory: () => CMD.capture('raw') },
    { label: 'Capture WPS',           factory: () => CMD.capture('wps') },
    { label: 'Stop Capture',          factory: () => CMD.stopCapture() },
  ],
  'Environment': [
    { label: 'Sweep',                 factory: () => CMD.sweep() },
    { label: 'PineAP Detection',      factory: () => CMD.pineap() },
    { label: 'Channel Congestion',    factory: () => CMD.congestion() },
  ],
  'Network': [
    { label: 'Scan LAN Devices',      factory: () => CMD.scanPorts('local', null, null) },
    { label: 'ARP Scan Network',      factory: () => CMD.scanArp() },
  ],
  'Evil Portal': [
    { label: 'Start Evil Portal',     factory: () => CMD.startPortal('default', 'FreeWiFi') },
    { label: 'Stop Evil Portal',      factory: () => CMD.stopPortal() },
    { label: 'List Portals',          factory: () => CMD.listPortals() },
  ],
  'Connection': [
    { label: 'WiFi Status',           factory: () => CMD.wifiStatus() },
    { label: 'Connect Saved WiFi',    factory: () => CMD.connect() },
    { label: 'Disconnect WiFi',       factory: () => CMD.disconnect() },
  ],
  'Misc': [
    { label: 'TV Cast',               factory: () => CMD.dialconnect() },
    { label: 'Stop All',              factory: () => CMD.stop() },
  ]
};

/** BLE action definitions (grouped, mirroring WiFi layout) */
const BLE_GROUPS = {
  'Scan & Select': [
    { label: 'Detect Devices',     factory: () => CMD.bleScan('spam'),    refresh: () => CMD.bleScan('spam'),    refreshLabel: 'Re-scan' },
    { label: 'Find Flippers',      factory: () => CMD.bleScan('flipper') },
    { label: 'AirTag Scanner',     factory: () => CMD.bleScan('airtag') },
    { label: 'Raw BLE Scanner',    factory: () => CMD.bleScan('raw') },
    { label: 'GATT Scanner',       factory: () => CMD.bleScan('gatt'),   refresh: () => CMD.listGatt(),         refreshLabel: 'List GATT' },
    { label: 'Stop BLE Scan',      factory: () => CMD.bleScan(null, true) },
  ],
  'Attacks': [
    { label: 'BLE Spam - Apple',     factory: () => CMD.bleSpam('apple') },
    { label: 'BLE Spam - Microsoft', factory: () => CMD.bleSpam('microsoft') },
    { label: 'BLE Spam - Samsung',   factory: () => CMD.bleSpam('samsung') },
    { label: 'BLE Spam - Google',    factory: () => CMD.bleSpam('google') },
    { label: 'Stop BLE Spam',        factory: () => CMD.bleSpam(null, true) },
  ],
  'Tracking': [
    { label: 'BLE Wardriving',     factory: () => CMD.bleWardrive() },
    { label: 'Stop Wardriving',    factory: () => CMD.bleWardrive(true) },
    { label: 'Spoof AirTag',       factory: () => CMD.spoofAirTag(true) },
    { label: 'Stop AirTag Spoof',  factory: () => CMD.spoofAirTag(false) },
  ],
  'Misc': [
    { label: 'List Flippers',      factory: () => CMD.listFlippers() },
    { label: 'List AirTags',       factory: () => CMD.listAirTags() },
    { label: 'List GATT Devices',  factory: () => CMD.listGatt() },
    { label: 'Stop All',           factory: () => CMD.stop() },
  ]
};

const BLE_ACTIONS = Object.values(BLE_GROUPS).flat();

/** BadBLE (BLE HID keyboard) action definitions. Kept as its own group inside
 * the BLE page because it advertises a HID profile and cannot run at the same
 * time as BLE scans or the bridge. */
const BADBLE_GROUPS = {
  'Keyboard': [
    { label: 'Run Built-in Script', factory: () => CMD.badbleRunBuiltin() },
    { label: 'Start Keyboard',     factory: () => CMD.badbleStart() },
    { label: 'Stop Keyboard',      factory: () => CMD.badbleStop() },
    { label: 'Status',             factory: () => CMD.badbleStatus() },
    { label: 'List Scripts',       factory: () => CMD.badbleList() },
    { label: 'Show Name',          factory: () => CMD.badbleName() },
    { label: 'Set Name',           factory: () => CMD.badbleSetName('GhostESP BadBLE') },
  ]
};

/** Settings schema with categories, descriptions, and validation */
const SETTINGS_SCHEMA = [
  {
    category: 'WiFi',
    icon: 'WF',
    description: 'Access Point and Station configuration',
    fields: [
      { id: 'ap_ssid',        label: 'AP SSID',            type: 'text',   max: 33,  hint: 'Broadcast network name' },
      { id: 'ap_password',    label: 'AP Password',        type: 'text',   max: 65,  hint: '8-63 chars; leave empty for open AP' },
      { id: 'ap_enabled',     label: 'AP Enabled',         type: 'bool' },
      { id: 'sta_ssid',       label: 'STA SSID',           type: 'text',   max: 65,  hint: 'Network to connect to' },
      { id: 'sta_password',   label: 'STA Password',       type: 'text',   max: 65 },
    ]
  },
  {
    category: 'Portal',
    icon: 'EP',
    description: 'Evil Portal captive portal settings',
    fields: [
      { id: 'portal_url',       label: 'Portal File Path',   type: 'text',   max: 129, hint: 'e.g. /mnt/ghostesp/portals/default.html' },
      { id: 'portal_ssid',      label: 'Portal SSID',        type: 'text',   max: 33 },
      { id: 'portal_password',  label: 'Portal Password',    type: 'text',   max: 65 },
      { id: 'portal_ap_ssid',   label: 'Portal AP SSID',     type: 'text',   max: 33,  hint: 'SSID for the portal AP' },
      { id: 'portal_domain',    label: 'Portal Domain',      type: 'text',   max: 65,  hint: 'e.g. ghost.net' },
      { id: 'portal_offline',   label: 'Portal Offline Mode',type: 'bool' },
    ]
  },
  {
    category: 'Printer',
    icon: 'PR',
    description: 'Thermal printer configuration',
    fields: [
      { id: 'printer_ip',        label: 'Printer IP',         type: 'text',   max: 16,  hint: 'e.g. 192.168.1.50' },
      { id: 'printer_text',      label: 'Printer Text',       type: 'textarea',max: 257, hint: 'Default text to print' },
      { id: 'printer_font_size', label: 'Printer Font Size',  type: 'number', min: 1, max: 255 },
      { id: 'printer_alignment', label: 'Printer Alignment',  type: 'enum',   options: [['0','Center Middle'],['1','Top Left'],['2','Top Right'],['3','Bottom Right'],['4','Bottom Left']] },
    ]
  },
  {
    category: 'Display',
    icon: 'DP',
    description: 'Screen, brightness and theme settings',
    fields: [
      { id: 'display_timeout', label: 'Display Timeout (ms)', type: 'number', min: 0, hint: '0 = always on' },
      { id: 'max_bright',      label: 'Max Brightness',       type: 'number', min: 0, max: 100 },
      { id: 'invert_colors',   label: 'Invert Colors',        type: 'bool' },
      { id: 'terminal_color',  label: 'Terminal Color',       type: 'color' },
      { id: 'theme_bg_fx',     label: 'Background Effects',   type: 'bool' },
      { id: 'menu_theme',      label: 'Menu Theme',           type: 'enum', options: [
        ['0', 'Ghost'], ['1', 'Catppuccin Mocha'], ['2', 'Flexoki'], ['3', 'Bright'],
        ['4', 'Solarized Dark'], ['5', 'Monochrome'], ['6', 'Rose Pine'], ['7', 'Dracula'],
        ['8', 'Nord'], ['9', 'Gruvbox Dark'], ['10', 'Everforest Dark'], ['11', 'Tokyo Night'],
        ['12', 'Catppuccin Latte'], ['13', 'Solarized Light'], ['14', 'Kanagawa Wave'],
        ['15', 'Rose Pine Dawn'], ['16', 'PaperColor Light'],
        ['17', 'Cherry'], ['18', 'Glacier'], ['19', 'Orchid'], ['20', 'Umber']
      ] },
    ]
  },
  {
    category: 'RGB',
    icon: 'RGB',
    description: 'LED strip and NeoPixel configuration',
    fields: [
      { id: 'rgb_mode',        label: 'RGB Mode',           type: 'enum',   options: [['0','Static'],['1','Breath'],['2','Rainbow']] },
      { id: 'rgb_speed',       label: 'RGB Speed',          type: 'number', min: 0, max: 255 },
      { id: 'rgb_data_pin',    label: 'RGB Data Pin',       type: 'number' },
      { id: 'rgb_red_pin',     label: 'RGB Red Pin',        type: 'number' },
      { id: 'rgb_green_pin',   label: 'RGB Green Pin',      type: 'number' },
      { id: 'rgb_blue_pin',    label: 'RGB Blue Pin',       type: 'number' },
      { id: 'neopixel_bright', label: 'NeoPixel Brightness',type: 'number', min: 0, max: 100 },
    ]
  },
  {
    category: 'System',
    icon: 'SY',
    description: 'Firmware behaviour and power management',
    fields: [
      { id: 'broadcast_speed',  label: 'Broadcast Speed (ms)', type: 'number', min: 0, max: 65535, hint: 'WebUI update interval' },
      { id: 'gps_rx_pin',       label: 'GPS RX Pin',           type: 'number' },
      { id: 'channel_delay',    label: 'Channel Delay',        type: 'number', hint: 'ms between channel hops' },
      { id: 'power_save',       label: 'Power Save',           type: 'bool' },
      { id: 'zebra_menus',      label: 'Zebra Menus',          type: 'bool' },
      { id: 'nav_buttons',      label: 'Nav Buttons',          type: 'bool' },
      { id: 'menu_layout',      label: 'Menu Layout',          type: 'enum', options: [['0','Carousel'],['1','Grid'],['2','List'],['3','Compact'],['4','Hero']] },
      { id: 'infrared_easy',    label: 'Infrared Easy Mode',   type: 'bool' },
      { id: 'web_auth',         label: 'Web Auth Enabled',     type: 'bool' },
      { id: 'rts_enabled',      label: 'RTS Enabled',          type: 'bool' },
      { id: 'third_ctrl',       label: 'Third Control Enabled',type: 'bool' },
      { id: 'auto_save_scans',  label: 'Auto Save Scans',      type: 'bool' },
    ]
  },
  {
    category: 'Date & Time',
    icon: 'DT',
    description: 'Clock and timezone configuration',
    fields: [
      { id: 'timezone',     label: 'Timezone',     type: 'text',  max: 25, hint: 'e.g. UTC-5' },
    ]
  },
  {
    category: 'GhostLink',
    icon: 'GL',
    description: 'Dual-device serial bridge pin mapping',
    fields: [
      { id: 'esp_comm_tx_pin',  label: 'GhostLink TX Pin', type: 'number', hint: 'GPIO for TX' },
      { id: 'esp_comm_rx_pin',  label: 'GhostLink RX Pin', type: 'number', hint: 'GPIO for RX' },
    ]
  },
  {
    category: 'Personalisation',
    icon: 'CU',
    description: 'Custom names and identity',
    fields: [
      { id: 'flappy_name',  label: 'Flappy Name',  type: 'text',  max: 65, hint: 'In-game name' },
    ]
  },
  {
    category: 'IO Button',
    icon: 'IO',
    description: 'Hardware button command mappings',
    fields: [
      { id: 'io_btn_p10_cmd', label: 'IO Button P10 Command', type: 'text', max: 129, hint: 'Command on P10 press' },
      { id: 'io_btn_p11_cmd', label: 'IO Button P11 Command', type: 'text', max: 129, hint: 'Command on P11 press' },
      { id: 'io_btn_p12_cmd', label: 'IO Button P12 Command', type: 'text', max: 129, hint: 'Command on P12 press' },
    ]
  },
];

/** LoRa action definitions (Terminal + future dedicated page). */
const LORA_GROUPS = {
  'Radio': [
    { label: 'LoRa Status',  factory: () => CMD.loraStatus() },
    { label: 'Start LoRa',   factory: () => CMD.loraStart() },
    { label: 'Stop LoRa',    factory: () => CMD.loraStop() },
    { label: 'Diagnostics',  factory: () => CMD.loraDiag() },
  ],
  'Mesh': [
    { label: 'List Messages', factory: () => CMD.loraChat() },
    { label: 'List Nodes',    factory: () => CMD.loraNodes() },
  ],
  'App': [
    { label: 'App Link Status', factory: () => CMD.loraApp() },
    { label: 'BLE On',          factory: () => CMD.loraBle(true) },
    { label: 'BLE Off',         factory: () => CMD.loraBle(false) },
  ],
};
/** Helper to build a command string with metadata */
function buildCommand(factoryResult) {
  if (!factoryResult || typeof factoryResult !== 'object') return null;
  if (factoryResult.cmd) return factoryResult;
  return null;
}

/** Determine if a raw command string is risky */
function isRiskyCommand(commandString) {
  const riskyPatterns = [
    /^scanap\b/i, /^scansta\b/i, /^scanall\b/i,
    /^attack\b/i, /^beaconspam\b/i,
    /^capture\b/i, /^listenprobes\b/i, /^pineap\b/i, /^karma\b/i,
    /^blescan\b/i, /^blespam\b/i, /^blewardriving\b/i,
    /^startwd\b/i, /^startportal\b/i, /^sweep\b/i, /^dhcpstarve\b/i,
    /^trackap\b/i, /^tracksta\b/i, /^trackgatt\b/i, /^selectflipper\b/i,
    /^spoofairtag\b/i, /^aerialscan\b/i, /^aerialspoof\b/i,
    /^ir dazzler\b/i, /^ir learn\b/i, /^badusb run\b/i,
    /^reboot\b/i, /^apenable\b/i, /^lora start\b/i,
  ];
  return riskyPatterns.some(p => p.test(commandString.trim()));
}

/** Map a command string to its category for UI organisation */
function commandCategory(commandString) {
  const c = commandString.trim().toLowerCase();
  if (c.startsWith('scan') || c.startsWith('list') || c.startsWith('select') || c.startsWith('connect') || c.startsWith('disconnect') || c.startsWith('wifistatus')) return 'WiFi';
  if (c.startsWith('attack') || c.startsWith('beacon') || c.startsWith('stop') || c.startsWith('karma') || c.startsWith('saeflood')) return 'Attack';
  if (c.startsWith('capture')) return 'Capture';
  if (c.startsWith('ble')) return 'BLE';
  if (c.startsWith('startportal') || c.startsWith('stopportal') || c.startsWith('listportals')) return 'Portal';
  if (c.startsWith('sd ')) return 'Files';
  if (c.startsWith('badusb')) return 'BadUSB';
  if (c.startsWith('badble')) return 'BadBLE';
  if (c.startsWith('lora')) return 'LoRa';
  if (c.startsWith('comm')) return 'GhostLink';
  if (c.startsWith('settings')) return 'Settings';
  return 'System';
}
