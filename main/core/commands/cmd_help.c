// cmd_help.c
// Interactive help command.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"

#include "core/network_constants.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Help command
void handle_help(int argc, char **argv) {
    const char *category = (argc > 1) ? argv[1] : "unknown"; // Default to "unknown" if no category is provided to fall through ifs

    // List of all categories to print in order
    const char *all_categories[] = {
        "wifi", "ble", "chameleon", "comm", "sd", "led", "gps", "shell", "misc", "portal", "printer", "cast", "gadgets", "capture", "beacon", "attack"
#ifdef CONFIG_HAS_INFRARED
        , "ir"
#endif
#ifdef CONFIG_HAS_CAMERA
        , "camera"
#endif
#ifdef CONFIG_WITH_ETHERNET
        , "ethernet"
#endif
    };
    int num_categories = sizeof(all_categories) / sizeof(all_categories[0]);

    if (strcmp(category, "all") == 0) {
        for (int i = 0; i < num_categories; ++i) {
            // Recursively call this function for each category
            char *fake_argv[] = { "help", (char *)all_categories[i] };
            handle_help(2, fake_argv);
        }
        return;
    }


    if (strcmp(category, "wifi") == 0) {
        glog("\nWi-Fi Commands:\n\n");
        glog("scanap\n");
        glog("    Description: Start a Wi-Fi access point (AP) scan.\n");
        glog("    Usage: scanap [seconds]\n\n");
        glog("scansta\n");
        glog("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
        glog("    Usage: scansta\n\n");
        glog("stopscan\n");
        glog("    Description: Stop any ongoing Wi-Fi scan.\n");
        glog("    Usage: stopscan\n\n");
        glog("attack\n");
        glog("    Description: Launch an attack (e.g., deauthentication attack).\n");
        glog("                 Supports multiple selected APs when using 'select -a 1,2,3'.\n");
        glog("    Usage: attack -d (deauth) | attack -hsd (handshake+deauth) | attack -c (channel switch) | attack -e (EAPOL logoff) | attack -p (probe flood) | attack -b (bad msg) | attack -a (auth flood) | attack -s (SAE flood)\n");
        glog("    Arguments:\n");
        glog("        -d  : Start deauth attack (supports multiple APs)\n");
        glog("        -hsd: Start handshake capture + deauth attack (forces handshakes)\n");
        glog("        -c  : Start channel switch attack (supports multiple APs)\n");
        glog("        -e  : Start EAPOL logoff attack\n");
        glog("        -p  : Start probe request flood attack (supports multiple APs)\n");
        glog("        -b  : Start bad msg attack (EAPOL key install + zero MIC)\n");
        glog("        -a  : Start auth flood attack (supports multiple APs)\n");
        glog("        -s  : Start SAE flood attack (ESP32-C5/C6 only)\n\n");
        glog("list\n");
        glog("    Description: List Wi-Fi scan results or connected stations.\n");
        glog("    Usage: list -a | list -s | list -airtags\n");
        glog("    Arguments:\n");
        glog("        -a  : Show access points from Wi-Fi scan\n");
        glog("        -s  : List connected stations\n");
        glog("        -airtags: List discovered AirTags\n\n");
         glog("wpa3check\n");
         glog("    Description: Run a passive Wi-Fi security check on the currently selected AP.\n");
        glog("                 If no AP is selected, scans all APs and prints a\n");
        glog("                 summary table with WPA3 presence, transition mode,\n");
         glog("                 PMF, cipher, Enterprise/EAP limitations, and a finding per AP.\n");
        glog("    Usage: wpa3check (after 'scanap' and optionally 'select -a <index>')\n\n");
        glog("beaconspam\n");
        glog("    Description: Start beacon spam with different modes.\n");
        glog("    Usage: beaconspam [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -r   : Start random beacon spam\n");
        glog("        -rr  : Start Rickroll beacon spam\n");
        glog("        -l   : Start AP List beacon spam\n");
        glog("        [SSID]: Use specified SSID for beacon spam\n\n");
        glog("stopspam\n");
        glog("    Description: Stop ongoing beacon spam.\n");
        glog("    Usage: stopspam\n\n");
        glog("hop\n");
        glog("    Description: Set the channel plan used by WiFi hopping features (deauth, beacon spam, scans, monitors).\n");
        glog("    Usage: hop [auto | all | basic | custom <channels> | <channels>]\n");
        glog("    Examples:\n");
        glog("        hop             : Show current hop mode and channel plan\n");
        glog("        hop 1,6,11      : Hop only channels 1, 6, and 11\n");
        glog("        hop all         : Hop every legal channel for this device\n");
        glog("        hop basic       : Fixed 1,6,11\n");
        glog("        hop auto        : Default per-feature channel plan (country / AP list)\n\n");
        glog("stopdeauth\n");
        glog("    Description: Stop ongoing deauthentication attack.\n");
        glog("    Usage: stopdeauth\n\n");
        glog("select\n");
        glog("    Description: Select access point(s), station, or AirTag by index from the scan results.\n");
        glog("    Usage: select -a <num[,num,...]> | select -s <num> | select -airtag <num>\n");
        glog("    Arguments:\n");
        glog("        -a      : AP selection index (supports multiple: 1,3,5)\n");
        glog("        -s      : Station selection index\n");
        glog("        -airtag : AirTag selection index\n");
        glog("    Examples:\n");
        glog("        select -a 4      : Select single AP at index 4\n");
        glog("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
        glog("scanall\n");
        glog("    Description: Perform combined AP and Station scan, display results.\n");
        glog("    Usage: scanall [seconds]\n\n");
        glog("wdstream\n");
        glog("    Description: Stream WiFi/BLE observations over serial for companion-app wardriving.\n");
        glog("    Usage: wdstream start [-wifi] [-ble] [-i <ms>] [-ch auto|1|1,6,11]\n");
        glog("           wdstream stop | wdstream status\n\n");
        glog("sweep\n");
        glog("    Description: Full environment sweep - scans WiFi APs, stations, BLE devices\n");
        glog("                 and saves comprehensive report to SD card.\n");
        glog("    Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
        glog("    Arguments:\n");
        glog("        -w  : WiFi scan duration per phase in seconds (default: 5)\n");
        glog("        -b  : BLE scan duration per phase in seconds (default: 5)\n");
        glog("    Output: /mnt/ghostesp/sweeps/sweep_N.csv\n\n");
        glog("congestion\n");
        glog("    Description: Display Wi-Fi channel congestion chart.\n");
        glog("    Usage: congestion\n\n");
        glog("connect\n");
        glog("    Description: Connects to Specific WiFi Network and saves credentials.\n");
        glog("    Usage: connect <SSID> [Password]\n\n");
        glog("autoreconnect\n");
        glog("    Description: Toggle WiFi station auto-reconnect after involuntary disconnects.\n");
        glog("    Usage: autoreconnect <on|off>\n");
        glog("    Arguments:\n");
        glog("        on  : Reconnect automatically (default)\n");
        glog("        off : Stay disconnected until manually reconnected\n\n");
        glog("apcred\n");
        glog("    Description: Change or reset the GhostNet AP credentials\n");
        glog("    Usage: apcred <ssid> <password>\n");
        glog("           apcred -r (reset to defaults)\n");
        glog("    Arguments:\n");
        glog("        <ssid>     : New SSID for the AP\n");
        glog("        <password> : New password (min 8 characters)\n");
        glog("        -r        : Reset to default (GhostNet/GhostNet)\n\n");
        glog("apenable\n");
        glog("    Description: Enable or disable the Access Point across reboots\n");
        glog("    Usage: apenable <on|off>\n");
        glog("    Arguments:\n");
        glog("        on  : Enable the Access Point (requires restart)\n");
        glog("        off : Disable the Access Point (requires restart)\n\n");
        glog("listenprobes\n");
        glog("    Description: Listen for and log probe requests.\n");
        glog("    Usage: listenprobes [channel] [stop]\n");
        glog("    Arguments:\n");
        glog("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
        glog("        stop      : Stop probe request listening\n\n");
        glog("karma\n");
        glog("    Description: Start or stop the Karma attack (responds to probe requests with specified or all SSIDs).\n");
        glog("    Usage: karma start [ssid1 ssid2 ...]\n");
        glog("           karma stop\n");
        glog("    Arguments:\n");
        glog("        start : Begin Karma attack. Optionally specify SSIDs to respond with (default: all known SSIDs).\n");
        glog("        stop  : Stop Karma attack.\n");
        glog("    Examples:\n");
        glog("        karma start\n");
        glog("        karma start FreeWiFi Starbucks\n");
        glog("        karma stop\n\n");
        glog("trackap\n");
        glog("    Description: track selected ap signal strength (rssi)\n");
        glog("    Usage: trackap\n");
        glog("    Note: select an ap first with 'select -a <index>'\n\n");
        glog("tracksta\n");
        glog("    Description: track selected station signal strength (rssi)\n");
        glog("    Usage: tracksta\n");
        glog("    Note: select a station first with 'select -s <index>'\n\n");
        glog("setcountry\n");
        glog("    Description: Set the Wi-Fi country code. 2.4 GHz-only targets use the full\n");
        glog("                 1-13 channel range regardless of the code; the C5 applies the real\n");
        glog("                 regulatory range.\n");
        glog("    Usage: setcountry <CC>\n");
        glog("    Arguments:\n");
        glog("        <CC> : Country code (\"01\" world-safe) or two-letter ISO (e.g., US)\n");
        glog("    Persisted across reboots: US, GB, JP, AU, CN, 01\n");
        glog("    Supported: 01, AT, AU, BE, BG, BR, CA, CH, CN, CY, CZ, DE, DK, EE, ES, FI, FR, GB, GR, HK, HR, HU,\n");
        glog("               IE, IN, IS, IT, JP, KR, LI, LT, LU, LV, MT, MX, NL, NO, NZ, PL, PT, RO, SE, SI, SK, TW, US\n\n");
        return;
    }

#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (strcmp(category, "ble") == 0) {
        glog("\nBLE Commands:\n\n");
        glog("blescan\n");
        glog("    Description: Handle BLE scanning with various modes.\n");
        glog("    Usage: blescan [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -f   : Start 'Find the Flippers' mode\n");
        glog("        -ds  : Start BLE spam detector\n");
        glog("        -a   : Start AirTag scanner\n");
        glog("        -adv : Start parsed BLE advertiser scan\n");
        glog("        -oui <prefix>    : Scan BLE advertisers matching an OUI prefix\n");
        glog("        -vendor <vendor> : Scan BLE advertisers matching an OUI vendor\n");
        glog("        -g   : Start GATT scanner for connectable devices\n");
        glog("        -r   : Scan for raw BLE packets\n");
        glog("        -s   : Stop BLE scanning\n\n");
        glog("bledetect\n");
        glog("    Description: Detect trackers, skimmers and beacons by signature\n");
        glog("    Usage: bledetect [-s|-l|-c|-i|-t <index>|-u|-sp <index>|-h]\n");
        glog("    Arguments:\n");
        glog("        (none)      : Begin scanning for detectable BLE devices\n");
        glog("        -s          : Stop the scan, keeping discovered devices\n");
        glog("        -l          : List discovered devices with type, name/MAC and RSSI\n");
        glog("        -c          : Drop stored results (scan must be stopped)\n");
        glog("        -i          : Show scan state, device count and tracking info\n");
        glog("        -t <index>  : Follow one device and log its live RSSI\n");
        glog("        -u          : Stop following the tracked device\n");
        glog("        -sp <index> : Advertise as a discovered AirTag; 'stopspoof' ends it\n\n");
        glog("blespam\n");
        glog("    Description: Start BLE advertisement spam attacks.\n");
        glog("    Usage: blespam [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -apple     : Apple device spam (AirPods, Apple TV, etc.)\n");
        glog("        -ms        : Microsoft Swift Pair spam\n");
        glog("        -samsung   : Samsung Galaxy Watch spam\n");
        glog("        -google    : Google Fast Pair spam\n");
        glog("        -random    : Random spam (cycles through all types)\n");
        glog("        -s         : Stop BLE spam\n\n");
        glog("blewardriving\n");
        glog("    Description: Start/Stop BLE wardriving with GPS logging\n");
        glog("    Usage: blewardriving [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop BLE wardriving\n\n");
        glog("dualwd\n");
        glog("    Description: Start/Stop BLE + WiFi coexistence wardriving (PSRAM only)\n");
        glog("    Usage: dualwd [start|-s]\n");
        glog("    Arguments:\n");
        glog("        start : Start dual wardriving (default)\n");
        glog("        -s    : Stop dual wardriving\n\n");
        glog("list -airtags\n");
        glog("    Description: List discovered AirTags\n");
        glog("    Usage: list -airtags\n\n");
        glog("select -airtag <index>\n\n");
        glog("listadv\n");
        glog("    Description: List parsed BLE advertisers from blescan -adv\n");
        glog("    Usage: listadv\n\n");
        return;
    }

    if (strcmp(category, "chameleon") == 0) {
        glog("\nChameleon Ultra Commands:\n\n");
        glog("chameleon connect [timeout] [pin]\n");
        glog("    Description: Connect to a Chameleon Ultra device via BLE\n");
        glog("    Usage: chameleon connect [timeout_seconds] [pin]\n");
        glog("    Arguments:\n");
        glog("        timeout_seconds : Connection timeout (default: 10)\n");
        glog("        pin            : PIN for authentication (4-6 digits, optional)\n\n");
        glog("chameleon disconnect\n");
        glog("    Description: Disconnect from the Chameleon Ultra device\n");
        glog("    Usage: chameleon disconnect\n\n");
        glog("chameleon status\n");
        glog("    Description: Check connection status with Chameleon Ultra\n");
        glog("    Usage: chameleon status\n\n");
        glog("chameleon scanhf\n");
        glog("    Description: Scan for High Frequency (HF) RFID tags\n");
        glog("    Usage: chameleon scanhf\n\n");
        glog("chameleon scanlf\n");
        glog("    Description: Scan for Low Frequency (LF) RFID tags\n");
        glog("    Usage: chameleon scanlf\n\n");
        glog("chameleon battery\n");
        glog("    Description: Get battery information from Chameleon Ultra\n");
        glog("    Usage: chameleon battery\n\n");
        glog("chameleon reader\n");
        glog("    Description: Set Chameleon Ultra to reader mode\n");
        glog("    Usage: chameleon reader\n\n");
        glog("chameleon emulator\n");
        glog("    Description: Set Chameleon Ultra to emulator mode\n");
        glog("    Usage: chameleon emulator\n\n");
        return;
    }
#endif

    if (strcmp(category, "comm") == 0) {
        glog("\nCommunication Commands:\n\n");
        glog("commdiscovery\n    Check discovery status.\n    Usage: commdiscovery\n\n");
        glog("commconnect\n    Connect to a discovered peer ESP32.\n    Usage: commconnect <peer_name>\n    Example: commconnect ESP_A1B2C3\n\n");
        glog("commsend\n    Send a command to connected peer ESP32.\n    Usage: commsend <command> [data]\n    Example: commsend scanap\n    Example: commsend hello world\n\n");
        glog("commstatus\n    Show communication status.\n    Usage: commstatus\n\n");
        glog("commdisconnect\n    Disconnect from current peer.\n    Usage: commdisconnect\n\n");
        glog("commsetpins\n    Change communication GPIO pins at runtime.\n    Usage: commsetpins <tx_pin> <rx_pin>\n    Example: commsetpins 4 5\n\n");
        glog("glbench\n    Measure GhostLink throughput in both directions.\n    Usage: glbench [send|recv|both] [kb]\n           glbench stop | glbench status\n    Example: glbench send 512\n\n");
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
        glog("blebridge\n    Start/status/stop the BLE GhostLink bridge.\n    Usage: blebridge [start|stop|status|pair <peer_name>]\n\n");
#endif
        return;
    }

    if (strcmp(category, "sd") == 0) {
        glog("\nSD Card Commands:\n\n");
        glog("-- File Operations (machine-parsable) --\n");
        glog("sd status\n    Show SD mount status, type, capacity, usage.\n    Usage: sd status\n\n");
        glog("sd list\n    List files/dirs with indices.\n    Usage: sd list [path]\n\n");
        glog("sd info\n    Show file/dir details.\n    Usage: sd info <index|path>\n\n");
        glog("sd size\n    Get file size.\n    Usage: sd size <index|path>\n\n");
        glog("sd read\n    Read file (chunked downloads).\n    Usage: sd read <index|path> [offset] [length] [--base64]\n\n");
        glog("sd write\n    Create/overwrite file with base64 data.\n    Usage: sd write <path> <base64>\n\n");
        glog("sd append\n    Append base64 data to file.\n    Usage: sd append <path> <base64>\n\n");
        glog("sd mkdir\n    Create directory.\n    Usage: sd mkdir <path>\n\n");
        glog("sd rm\n    Delete file or empty directory.\n    Usage: sd rm <index|path>\n\n");
        glog("sd tree\n    Recursive listing.\n    Usage: sd tree [path] [depth]\n\n");
        glog("-- Pin Configuration --\n");
        glog("sd_config\n    Show current SD GPIO pin configuration.\n    Usage: sd_config\n\n");
        glog("sd_pins_mmc\n    Set GPIO pins for SDMMC mode.\n    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");
        glog("sd_pins_spi\n    Set GPIO pins for SPI mode.\n    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");
        glog("sd_save_config\n    Save pin config to NVS.\n    Usage: sd_save_config\n\n");
        return;
    }

#ifdef CONFIG_HAS_USB_MSC_SD
    if (strcmp(category, "usbsd") == 0) {
        glog("\nUSB SD Passthrough Commands:\n\n");
        glog("usbsd\n    Toggle USB SD passthrough (card becomes a USB drive on the host PC).\n    Usage: usbsd\n\n");
        glog("usbsd on\n    Start passthrough: SD is handed to the USB host, serial console detaches.\n    Usage: usbsd on\n\n");
        glog("usbsd off\n    Stop passthrough and remount the SD card.\n    Usage: usbsd off\n\n");
        glog("usbsd status\n    Show passthrough state.\n    Usage: usbsd status\n\n");
        glog("Note: use WiFi/WebUI to exit if the serial console is detached.\n\n");
        return;
    }
#endif

    if (strcmp(category, "led") == 0) {
        glog("\nLED & RGB Commands:\n\n");
        glog("rgbmode\n    Control LED effects (rainbow, police, strobe, knight, off)\n    Usage: rgbmode <rainbow|police|strobe|knight|off|color>\n\n");
        glog("setrgbpins\n    Change RGB LED pins\n    Usage: setrgbpins <red> <green> <blue>\n           (use same value for all pins for single-pin LED strips)\n\n");
        glog("setrgbcount\n    Configure how many RGB LEDs are attached\n    Usage: setrgbcount <1-512>\n\n");
        glog("setneopixelbrightness\n    Set maximum neopixel brightness (percent)\n    Usage: setneopixelbrightness <0-100>\n\n");
        glog("getneopixelbrightness\n    Show current neopixel max brightness (percent)\n    Usage: getneopixelbrightness\n\n");
        return;
    }

    if (strcmp(category, "misc") == 0) {
        glog("\nMiscellaneous Commands:\n\n");
        glog("help\n");
        glog("    Description: Display this help message.\n");
        glog("    Usage: help [category]\n\n");
#if CONFIG_ENABLE_GHOSTSCRIPT
        glog("script\n");
        glog("    Description: List, launch, monitor, or stop GhostScripts from the SD card.\n");
        glog("    Usage: script list | script run <index> | script status | script stop\n\n");
#endif
        glog("chipinfo\n");
        glog("    Description: Display chip information including model, revision, and features\n");
        glog("    Usage: chipinfo\n");
        glog("    Shows:\n");
        glog("        - Chip model and revision\n");
        glog("        - CPU cores and features\n");
        glog("        - Flash size and memory info\n");
        glog("        - ESP-IDF version\n\n");
        glog("devices\n");
        glog("    Description: List enabled hardware devices with their active pins and state\n");
        glog("    Usage: devices\n\n");
        glog("irpin\n");
        glog("    Description: Show or override the IR TX/RX pins (persisted in settings)\n");
        glog("    Usage: irpin | irpin tx|rx <pin|-1>\n");
        glog("    TX changes apply after reboot; RX changes apply at the next IR learn\n\n");
        glog("crash\n");
        glog("    Description: Intentionally trigger a crash (for coredump testing).\n");
        glog("    Usage: crash\n");
        glog("    The device will panic and save a coredump to flash; use idf.py coredump-info to inspect.\n\n");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        glog("coredump [dump|erase]\n");
        glog("    Description: Read or clear coredump in flash.\n");
        glog("    Usage: coredump        - Print summary (partition size, whether coredump present).\n");
        glog("           coredump dump   - Stream coredump as base64; save to file and run idf.py coredump-info -c <file> on host.\n");
        glog("           coredump erase  - Erase coredump partition (clears saved crash).\n");
        glog("    With device connected, 'idf.py coredump-info' on host shows full panic reason and backtrace.\n\n");
#endif
        glog("timezone\n");
        glog("    Description: Set the display timezone for the clock view.\n");
        glog("    Usage: timezone <TZ_STRING>\n\n");
        glog("webauth\n");
        glog("    Description: Enable/disable web authentication.\n");
        glog("    Usage: webauth [on|off|toggle|status]\n\n");
        glog("pineap\n");
        glog("    Description: Start/Stop detecting WiFi Pineapples.\n");
        glog("    Usage: pineap [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop PineAP detection\n\n");
        glog("flockscan\n");
        glog("    Description: Start Flock Safety camera detection.\n");
        glog("    Usage: flockscan\n\n");
        glog("flocklist\n");
        glog("    Description: List detected Flock cameras.\n");
        glog("    Usage: flocklist\n\n");
        glog("flockstop\n");
        glog("    Description: Stop Flock camera detection.\n");
        glog("    Usage: flockstop\n\n");
        glog("Port Scanner\n");
        glog("    Description: Scan ports on local subnet or specific IP\n");
        glog("    Usage: scanports local\n");
        glog("           scanports <IP> [all | start-end]\n");
        glog("    Arguments:\n");
        glog("        all  : Scan all ports (1-65535)\n");
        glog("        start-end : Custom port range (e.g. 80-443)\n");
        glog("        (no range) : Scan common ports (default)\n\n");
        glog("scanarp\n");
        glog("    Description: Perform ARP scan on local network to discover active hosts\n");
        glog("    Usage: scanarp\n");
        glog("           scanarp monitor [duration-seconds]\n\n");
        glog("scanssh\n");
        glog("    Description: Scan a host or local subnet for SSH services and grab banners\n");
        glog("    Usage: scanssh\n");
        glog("           scanssh <IP>\n\n");
        glog("netbiosscan\n");
        glog("    Description: Scan for NetBIOS Name Service hosts on local subnet or specific IP\n");
        glog("    Usage: netbiosscan\n");
        glog("           netbiosscan <IP>\n");
        glog("           netbiosscan subnet <a.b.c[.0|.]>\n\n");
        glog("httpbannerscan\n");
        glog("    Description: Scan for HTTP/HTTPS services and grab Server banners\n");
        glog("    Usage: httpbannerscan\n");
        glog("           httpbannerscan <IP>\n");
        glog("           httpbannerscan subnet <a.b.c[.0|.]>\n\n");
        glog("snmpprobe\n");
        glog("    Description: Probe SNMP v1/v2c services with common communities\n");
        glog("    Usage: snmpprobe\n");
        glog("           snmpprobe <IP>\n");
        glog("           snmpprobe subnet <a.b.c[.0|.]>\n");
        glog("           snmpprobe walk <IP> [OID]\n");
        glog("           snmpprobe communities <c1,c2,...|file>\n\n");
        glog("mdnssniff\n");
        glog("    Description: Passively sniff local names (mDNS/LLMNR/SSDP/NetBIOS) per host\n");
        glog("    Usage: mdnssniff <IP|all>\n");
        glog("           mdnssniff stop\n\n");
        glog("settings\n");
        glog("    Description: Manage NVS stored settings via command line\n");
        glog("    Usage: settings <command> [arguments]\n");
        glog("    Commands:\n");
        glog("        list                    - List all available settings\n");
        glog("        get <setting>           - Get current value of a setting\n");
        glog("        set <setting> <value>   - Set a setting to a value\n");
        glog("        reset [setting]         - Reset setting(s) to defaults\n");
        glog("        help                    - Show settings help\n");
        glog("    Examples:\n");
        glog("        settings list\n");
        glog("        settings get ap_ssid\n");
        glog("        settings set rgb_mode 1\n");
        glog("        settings reset\n\n");
        glog("loglevel\n");
        glog("    Description: View or change the global ESP-IDF log level.\n");
        glog("    Usage: loglevel [none|error|warn|info|debug|verbose]\n\n");
        glog("    Description: View or change the status display idle animation (status OLED only).\n");
        glog("    Usage: statusidle [list|set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0|1|2|3|4|5|6|7|8>]\n\n");
        return;
    }
    if (strcmp(category, "gps") == 0) {
        glog("\nGPS Commands:\n\n");
        glog("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo [-s]\n\n");
        glog("gpspin\n    Set GPS RX pin for external GPS module.\n    Usage: gpspin <pin>\n\n");
        glog("gpsbaud\n    Set GPS baud rate or auto-detect it.\n    Usage: gpsbaud <auto|0|4800|9600|19200|38400|57600|115200>\n\n");
        glog("startwd\n    Start GPS wardriving.\n    Usage: startwd [-s] [--active|--monitor] [--helper] [--channels <csv>] [--hop <ms>] [--weighted]\n    C5/S3 default to driver active scans; --monitor selects beacon/probe capture.\n\n");
        return;
    }
    if (strcmp(category, "shell") == 0) {
        glog("\nHeadless Shell Commands:\n\n");
        glog("echo <text>                 Print text; supports \\n and \\t escapes.\n");
        glog("ifconfig                    Show STA, AP, and Ethernet interfaces.\n");
        glog("ping <host> [count]         Send ICMP echo requests.\n");
        glog("version                     Show firmware, build, git, and IDF versions.\n");
        glog("uuid | macaddr              Show stable device identifiers.\n");
        glog("uptime | date               Show uptime or current time.\n");
        glog("whoami | status             Show device identity or a system summary.\n");
        glog("hostname [name]             View or set the prompt hostname.\n");
        glog("color [name|0-255|off]      Set ANSI prompt color (also cli_color).\n");
        glog("banner [on|off|status]      Control the boot banner.\n");
        glog("clear                       Clear an ANSI terminal.\n");
        glog("alias [name command]        Create a persistent command shortcut.\n");
        glog("unalias <name|all>          Remove shortcuts.\n");
        glog("history [-c]                Show or clear command history.\n");
        glog("ps | top                   Show FreeRTOS task information.\n");
        glog("df                         Show /mnt filesystem usage.\n");
        glog("tail <file> [lines]         Print the end of an SD file.\n");
        glog("grep <pattern> <file>       Filter an SD file.\n");
        glog("source <file>               Run CLI commands from an SD file.\n");
        glog("tee <file> <text>           Append text to an SD file and echo it.\n");
        glog("env | export NAME=value     View or persist simple shell variables.\n");
        glog("watch <seconds> <command>  Repeat a command; use watch stop to end it.\n");
        glog("Unknown commands get a 'Did you mean?' suggestion automatically.\n\n");
        return;
    }
    if (strcmp(category, "wigle") == 0) {
        glog("\nWiGLE Commands:\n\n");
        glog("wigle API <encoded|name:token>\n    Set WiGLE API credentials (encoded token or legacy format).\n\n");
        glog("wigle auto on/off\n    Enable/disable auto-upload.\n\n");
        glog("wigle donate on/off\n    Enable/disable WiGLE donate flag.\n\n");
        glog("wigle show\n    Show WiGLE settings.\n\n");
        glog("wigle list\n    Show uploaded CSV memory.\n\n");
        glog("wigle files [page]\n    List CSVs in /mnt/ghostesp/gps/ for manual upload.\n\n");
        glog("wigle upload <filename>\n    Upload a specific CSV file.\n\n");
        glog("wigle upload all\n    Upload all pending queue files.\n\n");
        glog("wigle stats\n    Show account stats for current API key.\n\n");
        return;
    }
    if (strcmp(category, "portal") == 0) {
        glog("\nEvil Portal Commands:\n\n");
        glog("startportal\n");
        glog("    Description: Start an Evil Portal using a local file or the default embedded page.\n");
        glog("                 /mnt/ prefix is added automatically to file paths if missing.\n");
        glog("    Usage: startportal [FilePath] [AP_SSID] [PSK]\n");
        glog("           PSK is optional for an open network.\n");
        glog("    Use 'default' as the file path for the default Evil Portal.\n");
        glog("\n");
        glog("evilportal\n");
        glog("    Description: Configure Evil Portal HTML content via UART buffer.\n");
        glog("    Usage: evilportal -c sethtmlstr\n");
        glog("    Steps:\n");
        glog("      1. Run: evilportal -c sethtmlstr\n");
        glog("      2. Send [HTML/BEGIN] marker over UART\n");
        glog("      3. Send HTML content over UART\n");
        glog("      4. Send [HTML/CLOSE] marker over UART\n");
        glog("      5. Run startportal (will use buffered HTML)\n");
        glog("\n");
        glog("stopportal\n");
        glog("    Description: Stop Evil Portal\n");
        glog("    Usage: stopportal\n\n");
        glog("listportals\n    List available Evil Portal files.\n    Usage: listportals\n\n");
        return;
    }

    if (strcmp(category, "printer") == 0) {
        glog("\nPrinter Commands:\n\n");
        glog("powerprinter\n");
        glog("    Description: Print Custom Text to a Printer on your LAN (Requires You to Run Connect First)\n");
        glog("    Usage: powerprinter <Printer IP> <Text> <FontSize> <alignment>\n");
        glog("    alignment options: CM = Center Middle, TL = Top Left, TR = Top Right, BR = Bottom Right, BL = Bottom Left\n\n");
        return;
    }

    if (strcmp(category, "cast") == 0) {
        glog("\nYouTube Cast Commands:\n\n");
        glog("dialconnect\n");
        glog("    Description: Cast a Random Youtube Video on all Smart TV's on your LAN (Requires You to Run Connect First)\n");
        glog("    Usage: dialconnect\n\n");
        glog("dialconnect\n");
        glog("    Cast a random YouTube video to all smart TVs on your LAN.\n");
        glog("    Usage: dialconnect\n\n");
        return;
    }

    if (strcmp(category, "gadgets") == 0) {
        glog("wol <MAC|IP> [broadcast IP]\n");
        glog("govee scan | govee <IP> on|off|brightness N|color RRGGBB\n");
        return;
    }

    if (strcmp(category, "capture") == 0) {
        glog("\nCapture Commands:\n\n");
        glog("capture\n");
        glog("    Description: Start a WiFi Capture (Requires SD Card or Flipper)\n");
        glog("    Usage: capture [OPTION] [-channel <n>|-c <n>]\n");
        glog("    Arguments:\n");
        glog("        -probe     : Start Capturing Probe Packets\n");
        glog("        -beacon    : Start Capturing Beacon Packets\n");
        glog("        -deauth    : Start Capturing Deauth Packets\n");
        glog("        -raw       : Start Capturing Raw Packets\n");
        glog("        -wps       : Start Capturing WPS Packets and there Auth Type\n");
        glog("        -pwn       : Start Capturing Pwnagotchi Packets\n");
        glog("        -eapol     : Start Capturing EAPOL (handshake) Packets\n");
        glog("        -list      : Browse saved PCAPs with +/- hc22000 markers\n");
        glog("        -export    : Export PCAP to hc22000 (PMKID + M2/M3)\n");
        glog("                    Usage: capture -export <pcap-file>\n");
        glog("        -wireshark : Stream raw PCAP to USB/UART for Wireshark\n");
        glog("                    Usage: capture -wireshark [-c <channel>|-channel <channel>]\n");
        glog("                    -channel <n>: Lock to specific channel (1-%d)\n", MAX_WIFI_CHANNEL);
        glog("        -wiresharkble : Stream BLE PCAP to USB/UART for Wireshark\n");
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
        glog("        -ble       : Start BLE packet capture\n");
        glog("        -skimmer   : Start skimmer (BLE) detection\n");
        #endif
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("        -802154    : Start Capturing IEEE 802.15.4 Packets [C5/C6]\n");
        glog("                    Usage: capture -802154 [ch<n>|-channel <n>]\n");
        glog("                    -channel <n>: Lock to 802.15.4 channel (11-26)\n");
        #endif
        glog("        -stop      : Stops the active capture\n\n");
        glog("capture\n");
        glog("    Start a WiFi packet capture.\n");
        glog("    Usage: capture [OPTION] [-channel <n>|-c <n>]\n");
        glog("    -channel <n>: Lock the radio to channel <n> during the capture.\n");
        glog("                  Accepted by: -probe, -deauth, -beacon, -raw, -eapol, -pwn, -wps, -wireshark");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog(", -802154 (11-26 only)");
        #endif
        glog(".\n");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -eapol, -list, -export, -wireshark, -wiresharkble, -802154, -stop\n\n");
        #else
        glog("    Options: -probe, -beacon, -deauth, -raw, -wps, -pwn, -eapol, -list, -export, -wireshark, -wiresharkble, -stop\n\n");
        #endif
        return;
    }

    if (strcmp(category, "beacon") == 0) {
        glog("\nBeacon Spam Commands:\n\n");
        glog("beaconadd\n    Add an SSID to the beacon spam list.\n    Usage: beaconadd <SSID>\n\n");
        glog("beaconremove\n    Remove an SSID from the beacon spam list.\n    Usage: beaconremove <SSID>\n\n");
        glog("beaconclear\n    Clear the beacon spam list.\n    Usage: beaconclear\n\n");
        glog("beaconshow\n    Show the current beacon spam list.\n    Usage: beaconshow\n\n");
        glog("beaconspamlist\n    Start beacon spamming using the beacon spam list.\n    Usage: beaconspamlist\n\n");
        return;
    }

    if (strcmp(category, "attack") == 0) {
        glog("\nAttack Commands:\n\n");
        glog("dhcpstarve\n");
        glog("    Description: DHCP starvation flood attack\n");
        glog("    Usage: dhcpstarve start [threads]\n");
        glog("           dhcpstarve stop\n");
        glog("           dhcpstarve display\n\n");
        glog("saeflood\n");
        glog("    Description: SAE handshake flooding attack (ESP32-C5/C6 only)\n");
        glog("    Usage: saeflood <password> (requires selected WPA3 AP)\n\n");
        glog("stopsaeflood\n    Stop SAE flood attack.\n    Usage: stopsaeflood\n\n");
        glog("saefloodhelp\n    Show detailed SAE flood attack help.\n    Usage: saefloodhelp\n\n");
        return;
    }
    
#ifdef CONFIG_HAS_INFRARED
    if (strcmp(category, "ir") == 0) {
        glog("\nInfrared Commands:\n\n");
        glog("ir send\n");
        glog("    Description: Send an IR signal from a file.\n");
        glog("    Usage: ir send <path> [index]\n\n");

        glog("ir learn\n");
        glog("    Description: Learn an IR signal and save to file.\n");
        glog("    Usage: ir learn <path>\n\n");
        glog("ir list\n");
        glog("    Description: List IR files in default directory.\n");
        glog("    Usage: ir list [path]\n\n");
        glog("ir rx\n");
        glog("    Description: Receive and display IR signals (Matrix mode).\n");
        glog("    Usage: ir rx [timeout]\n\n");
        glog("ir show\n");
        glog("    Description: Show content of an IR file.\n");
        glog("    Usage: ir show <path>\n\n");
        glog("ir universals\n");
        glog("    Description: Manage universal IR signals (files and built-ins).\n");
        glog("    Usage: ir universals list [-all]\n");
        glog("           ir universals send <index>\n");
        glog("           ir universals sendall <file|TURNHISTVOFF> [delay_ms]\n");
        glog("           ir universals show <file|TURNHISTVOFF>\n\n");
        glog("ir dazzler\n");
        glog("    Description: IR dazzler mode - emit continuous IR to interfere with cameras.\n");
        glog("    Usage: ir dazzler [stop]\n\n");
        return;
    }
#endif

#ifdef CONFIG_WITH_ETHERNET
    if (strcmp(category, "ethernet") == 0) {
        glog("\nEthernet Commands:\n\n");
        printf("ethup\n");
        printf("    Description: Initialize and bring up Ethernet interface.\n");
        printf("    Usage: ethup\n");
        printf("    Note: Waits for link establishment and DHCP assignment.\n\n");
        printf("ethdown\n");
        printf("    Description: Deinitialize and bring down Ethernet interface.\n");
        printf("    Usage: ethdown\n\n");
        printf("ethinfo\n");
        printf("    Description: Display Ethernet connection information.\n");
        printf("    Usage: ethinfo\n");
        printf("    Shows: Status, IP address, netmask, gateway, DNS servers, DHCP server\n\n");
        printf("ethfp\n");
        printf("    Description: Fingerprint network hosts using mDNS, NetBIOS, and SSDP.\n");
        printf("    Usage: ethfp\n");
        printf("    Discovers: Apple devices, Chromecasts, printers, Windows PCs, routers, smart TVs\n\n");
        printf("etharp\n");
        printf("    Description: Perform ARP scan on local Ethernet network.\n");
        printf("    Usage: etharp\n");
        printf("    Scans: Local subnet (1-254) to discover active hosts\n\n");
        printf("ethports\n");
        printf("    Description: Scan TCP ports on a target IP address.\n");
        printf("    Usage: ethports [IP] [all | start-end]\n");
        printf("    Arguments:\n");
        printf("        [IP]      : Target IP address (default: gateway)\n");
        printf("        all       : Scan all ports (1-65535)\n");
        printf("        start-end  : Custom port range (e.g., 80-443)\n");
        printf("        (no range): Scan common ports (default)\n");
        printf("    Examples:\n");
        printf("        ethports\n");
        printf("        ethports 192.168.1.1\n");
        printf("        ethports 192.168.1.1 all\n");
        printf("        ethports 192.168.1.1 80-443\n\n");
        printf("ethping\n");
        printf("    Description: Perform ICMP ping scan on local Ethernet network.\n");
        printf("    Usage: ethping\n");
        printf("    Scans: Local subnet (1-254) to find alive hosts\n\n");
        printf("ethdns\n");
        printf("    Description: Perform DNS lookup or reverse DNS lookup.\n");
        printf("    Usage: ethdns <hostname>\n");
        printf("           ethdns reverse <ip_address>\n");
        printf("    Examples:\n");
        printf("        ethdns google.com\n");
        printf("        ethdns reverse 8.8.8.8\n\n");
        printf("ethtrace\n");
        printf("    Description: Perform traceroute to a target host.\n");
        printf("    Usage: ethtrace <hostname_or_ip> [max_hops]\n");
        printf("    Arguments:\n");
        printf("        hostname_or_ip : Target hostname or IP address\n");
        printf("        max_hops       : Maximum number of hops (default: 30, max: 64)\n");
        printf("    Examples:\n");
        printf("        ethtrace 8.8.8.8\n");
        printf("        ethtrace google.com 30\n\n");
        printf("ethstats\n");
        printf("    Description: Display Ethernet network statistics.\n");
        printf("    Usage: ethstats\n");
        printf("    Shows: Link status, IP info, MAC address, packet statistics, ARP statistics\n\n");
        printf("ethconfig\n");
        printf("    Description: Configure Ethernet IP settings (DHCP or static).\n");
        printf("    Usage: ethconfig <command>\n");
        printf("    Commands:\n");
        printf("        dhcp                    - Use DHCP (automatic IP)\n");
        printf("        static <ip> <netmask> <gateway> - Set static IP\n");
        printf("        show                    - Show current configuration\n");
        printf("    Examples:\n");
        printf("        ethconfig dhcp\n");
        printf("        ethconfig static 192.168.1.100 255.255.255.0 192.168.1.1\n");
        printf("        ethconfig show\n\n");
        printf("ethmac\n");
        printf("    Description: View or set Ethernet MAC address.\n");
        printf("    Usage: ethmac\n");
        printf("           ethmac set <xx:xx:xx:xx:xx:xx>\n");
        printf("    Examples:\n");
        printf("        ethmac\n");
        printf("        ethmac set 02:00:00:00:00:01\n");
        printf("    Note: MAC address changes may require reinitialization\n\n");
        printf("ethserv\n");
        printf("    Description: Service discovery and banner grabbing on a target IP.\n");
        printf("    Usage: ethserv [ip_address]\n");
        printf("    Arguments:\n");
        printf("        [ip_address] : Target IP address (default: gateway)\n");
        printf("    Scans: Common services (FTP, SSH, Telnet, SMTP, HTTP, HTTPS, etc.)\n");
        printf("    Example: ethserv 192.168.1.1\n\n");
        printf("ethntp\n");
        printf("    Description: Query NTP server and synchronize system time.\n");
        printf("    Usage: ethntp [ntp_server]\n");
        printf("    Arguments:\n");
        printf("        [ntp_server] : NTP server hostname or IP (default: pool.ntp.org)\n");
        printf("    Examples:\n");
        printf("        ethntp\n");
        printf("        ethntp pool.ntp.org\n");
        printf("        ethntp time.google.com\n");
        printf("    Note: Requires Ethernet connection to be active\n\n");
        printf("ethhttp\n");
        printf("    Description: Send HTTP/HTTPS GET request to a server and display response.\n");
        printf("    Usage: ethhttp <url> [lines|all]\n");
        printf("    Arguments:\n");
        printf("        <url>  : Full URL including protocol (http:// or https://)\n");
        printf("        [lines]: Optional - show first N lines (default: 25, use 'all' for full)\n");
        printf("    Examples:\n");
        printf("        ethhttp http://example.com  (shows first 25 lines)\n");
        printf("        ethhttp https://www.google.com 50  (shows first 50 lines)\n");
        printf("        ethhttp http://192.168.1.1/index.html all  (shows full response)\n");
        printf("        ethhttp https://example.com:8443/api/data 100\n");
        printf("    Note: Default is 25 lines. Use 'all' for complete responses. HTTPS uses TLS 1.2.\n\n");
        TERMINAL_VIEW_ADD_TEXT("ethup, ethdown, ethinfo, ethfp, etharp, ethports, ethping, ethdns, ethtrace, ethstats, ethconfig, ethmac, ethserv, ethntp, ethhttp\n");
        return;
    }
#endif

#ifdef CONFIG_HAS_CAMERA
    if (strcmp(category, "camera") == 0) {
        glog("\nCamera Commands:\n\n");
        glog("camerastream start\n");
        glog("    Description: Start the live camera stream server.\n");
        glog("    Usage: camerastream start\n");
        glog("    Access the stream at http://ghostesp.local/camera\n\n");
        glog("camerastream stop\n");
        glog("    Description: Stop the camera stream and release the camera.\n");
        glog("    Usage: camerastream stop\n\n");
        glog("camerastream status\n");
        glog("    Description: Show current stream state.\n");
        glog("    Usage: camerastream status\n\n");
        glog("camerastream quality <1-100>\n");
        glog("    Description: Set JPEG compression quality.\n");
        glog("    Usage: camerastream quality 80\n\n");
        glog("camerastream resolution <name>\n");
        glog("    Description: Set camera resolution.\n");
        glog("    Usage: camerastream resolution SVGA\n");
        glog("    Options: QQVGA, QVGA, VGA, SVGA, XGA, SXGA, UXGA\n\n");
        glog("camerastream fps <1-30>\n");
        glog("    Description: Set target framerate.\n");
        glog("    Usage: camerastream fps 15\n\n");
        glog("motion start\n");
        glog("    Description: Start on-device motion detection.\n");
        glog("    Usage: motion start\n\n");
        glog("motion stop\n");
        glog("    Description: Stop motion detection.\n");
        glog("    Usage: motion stop\n\n");
        glog("motion status\n");
        glog("    Description: Show motion detector state.\n");
        glog("    Usage: motion status\n\n");
        glog("motion threshold <1-255>\n");
        glog("    Description: Set pixel difference threshold.\n");
        glog("    Usage: motion threshold 30\n\n");
        glog("motion interval <100-10000>\n");
        glog("    Description: Set frame interval in milliseconds.\n");
        glog("    Usage: motion interval 500\n\n");
        glog("motion percent <1-100>\n");
        glog("    Description: Set trigger percentage.\n");
        glog("    Usage: motion percent 10\n\n");
        glog("motion sample <1-32>\n");
        glog("    Description: Compare every Nth pixel.\n");
        glog("    Usage: motion sample 4\n\n");
        glog("motion snap <on|off>\n");
        glog("    Description: Enable/disable SD card snapshots.\n");
        glog("    Usage: motion snap on\n\n");
        glog("motion image <on|off>\n");
        glog("    Description: Attach image to Discord alerts.\n");
        glog("    Usage: motion image on\n\n");
        glog("motion discord <url|off>\n");
        glog("    Description: Set or disable Discord webhook URL.\n");
        glog("    Usage: motion discord https://discord.com/api/webhooks/...\n\n");
        glog("motion cooldown <ms>\n");
        glog("    Description: Set minimum time between webhook alerts.\n");
        glog("    Usage: motion cooldown 60000\n\n");
        glog("Note: Camera stream and motion detection are mutually exclusive.\n\n");
        return;
    }
#endif

    glog("\nGhost ESP Command Categories:\n\n");

    glog("  help wifi      - Wi-Fi commands\n");
    glog("  help ble       - Bluetooth/BLE commands\n");
    glog("  help comm      - ESP32 communication commands\n");
    glog("  help sd        - SD card commands\n");
#ifdef CONFIG_HAS_USB_MSC_SD
    glog("  help usbsd     - USB SD passthrough commands\n");
#endif
    glog("  help led       - LED/RGB commands\n");
    glog("  help gps       - GPS commands\n");
    glog("  help shell     - Headless shell commands\n");
    glog("  help wigle     - WiGLE commands\n");
    glog("  help misc      - Miscellaneous commands\n");
    glog("  help portal    - Evil Portal commands\n");
    glog("  help printer   - Printer commands\n");
    glog("  help cast      - YouTube cast commands\n");
    glog("  help capture   - Wi-Fi packet capture commands\n");
    glog("  help beacon    - Beacon spam commands\n");
    glog("  help attack    - Attack/flood commands\n");
#ifdef CONFIG_HAS_INFRARED
    glog("  help ir        - Infrared commands\n");
#endif
#ifdef CONFIG_HAS_CAMERA
    glog("  help camera    - Camera & motion detection commands\n");
#endif
#ifdef CONFIG_WITH_ETHERNET
    glog("  help ethernet  - Ethernet commands\n");
#endif
    glog("  help all       - All commands\n\n");

    glog("Type 'help <category>' for details on that category.\n\n");
}
