---
title: "Supported Hardware"
description: "Device compatibility matrix for GhostESP features"
weight: 10
toc: true
---

## Overview

GhostESP runs on a variety of ESP32 boards with varying feature support. This compatibility matrix helps you identify which features are built in or preconfigured for each release build.

## Compatibility Matrix

<div class="compat-controls" data-compat-controls data-table="#hardware-compat-table">
  <label class="compat-controls__search">
    <span>Search boards</span>
    <input type="search" placeholder="Cardputer, Banshee, ESP32..." autocomplete="off" data-compat-search>
  </label>
  <fieldset class="compat-controls__features">
    <legend>Require Features</legend>
    <label><input type="checkbox" data-compat-feature="Bluetooth"> Bluetooth</label>
    <label><input type="checkbox" data-compat-feature="NFC (PN532)"> PN532 NFC</label>
    <label><input type="checkbox" data-compat-feature="NFC (Chameleon)"> Chameleon NFC</label>
    <label><input type="checkbox" data-compat-feature="IR TX"> IR TX</label>
    <label><input type="checkbox" data-compat-feature="IR RX"> IR RX</label>
    <label><input type="checkbox" data-compat-feature="GPS Default"> GPS default</label>
    <label><input type="checkbox" data-compat-feature="Keyboard"> Keyboard</label>
    <label><input type="checkbox" data-compat-feature="Display"> Display</label>
    <label><input type="checkbox" data-compat-feature="SD Default"> SD default</label>
    <label><input type="checkbox" data-compat-feature="OTA"> OTA</label>
    <label><input type="checkbox" data-compat-feature="Native SD Apps"> Native SD Apps</label>
    <label><input type="checkbox" data-compat-feature="LoRa"> LoRa</label>
  </fieldset>
  <div class="compat-controls__status" aria-live="polite">
    <span data-compat-count></span>
    <button type="button" data-compat-clear>Clear filters</button>
  </div>
</div>

<p class="compat-empty" data-compat-empty hidden>No boards match the selected filters.</p>

<div class="compat-table">
  <table id="hardware-compat-table">
    <thead>
      <tr>
        <th>Board</th>
        <th>Bluetooth</th>
        <th>NFC (PN532)</th>
        <th>NFC (Chameleon)</th>
        <th>IR TX</th>
        <th>IR RX</th>
        <th>GPS Default</th>
        <th>Keyboard</th>
        <th>Display</th>
        <th>SD Default</th>
        <th>OTA</th>
        <th>Native SD Apps</th>
        <th>LoRa</th>
      </tr>
    </thead>
    <tbody>
      <tr><th scope="row">CYD2USB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CYDMicroUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CYDDualUSB</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CYD2432S028R</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CYD 2.4″ variants</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Waveshare 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Crowtech 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel 4.2″ E-paper</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>E-paper 400×300</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel 5.79″ E-paper</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>E-paper 792×272</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advance 2.4″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CrowPanel Advance 2.8″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CrowPanel Advance 3.5″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advance 4.3″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td></tr>
      <tr><th scope="row">CrowPanel Advance 5″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advance 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advanced P4 5″ RGB</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advanced P4 7/9/10.1″ (v1.2+)</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">CrowPanel Advanced P4 7/9/10.1″ (v1.1)</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Sunton 7″</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Cardputer</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Cardputer ADV</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">MarauderV4</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Marauder V8</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Marauder Pancake</th><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">MarauderV6</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">AwokMini</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Awok V5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">T-Dongle-S3</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">T-Dongle-C5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">T-Display S3 Touch</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">S3TWatch</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>has 4MB vfs partition</td><td>✓</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">TEmbed C1101</th><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Banshee</th><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">GhostBoard</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Poltergeist</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>Status Display</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">T-Deck</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">JCMK DevBoardPro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">RabbitLabs Minion</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Lolin S3 Pro</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">XIAO ESP32-S3 Sense</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">XIAO ESP32-C5</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">Flipper JCMK GPS</th><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32-S2 (generic)</th><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32-C3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32-S3 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32-C5 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">ESP32-C6 (generic)</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>Manual</td><td>✗</td><td>✗</td></tr>
      <tr><th scope="row">Heltec V3</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✗</td><td>Status Display</td><td>✓</td><td>Manual</td><td>✗</td><td>✓</td></tr>
      <tr><th scope="row">M5Stack CoreS3-SE</th><td>✓</td><td>✗</td><td>✓</td><td>✗</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✓</td><td>✗</td></tr>
      <tr><th scope="row">M5Stack AtomS3R</th><td>✓</td><td>✗</td><td>✓</td><td>✓</td><td>✗</td><td>✗</td><td>✓</td><td>✓</td><td>has 1MB vfs partition</td><td>Manual</td><td>✗</td><td>✗</td></tr>
    </tbody>
  </table>
</div>

**OTA:** `✓` means the release build uses a dual-partition OTA layout. `Manual` means USB/manual flashing is required for that release build. Banshee C5 self-OTA (previously `Updater + peer`) is disabled in v2.1.2 due to bad UX and flash overflow; use USB/manual flashing - OTA will return in a future leaner layout.

**Native SD Apps:** `✓` means the release build supports loading native `.gapp` apps from SD.

**GPS Default / SD Default:** these columns describe the release build's default or known board wiring. GPS and SD card pins can be configured at runtime on most builds, so `✗` does not mean the feature is impossible on custom wiring.

**M5Stack Grove ports:** the M5Stack CoreS3-SE and AtomS3R configs expose their HY2.0-4P Grove connectors on I2C port 1 (`PORT.A`: SDA=G2, SCL=G1; the CoreS3-SE also has `PORT.B` G8/G9 and `PORT.C` G17/G18). Plug an ST25R3916 NFC module (I2C, 0x50) and/or an M5Stack ENV III unit (SHT30 0x44 + QMP6988 0x70) into Grove `PORT.A` and open the NFC or ENV III app — both devices share the same bus.

## Camera Support

- **XIAO ESP32-S3 Sense** currently has the dedicated camera-enabled build and supports the [Motion Detector]({{< relref "../camera/motion-detector.md" >}}) feature.
- The current motion detector implementation uses **QQVGA grayscale** capture for low RAM usage and fast comparisons.
- **XIAO ESP32-C5** is a supported board config, but it does not include the onboard camera motion detector path used by the S3 Sense build.

## Vendor Boards

The following table lists the vendor-specific boards supported by GhostESP with their corresponding build names:

| Board Name | Build Name | Image |
|---|---|---|
| CYDMicroUSB | `CYDMicroUSB.zip` | |
| Elecrow CrowPanel 4.2″ E-paper | `CrowPanel_4.2_Epaper.zip` | |
| Elecrow CrowPanel 5.79″ E-paper | `CrowPanel_5.79_Epaper.zip` | |
| Elecrow CrowPanel Advance 2.4″ | `CrowPanel_Advance_24.zip` | |
| Elecrow CrowPanel Advance 2.8″ | `CrowPanel_Advance_28.zip` | |
| Elecrow CrowPanel Advance 3.5″ | `CrowPanel_Advance_35.zip` | |
| Elecrow CrowPanel Advance 4.3″ | `CrowPanel_Advance_43.zip` | |
| Elecrow CrowPanel Advance 5″ | `CrowPanel_Advance_5.zip` | |
| Elecrow CrowPanel Advance 7″ | `CrowPanel_Advance_7inch.zip` | |
| Elecrow CrowPanel Advanced P4 5″ RGB | `CrowPanel_Advanced_P4_5inch.zip` | |
| Elecrow CrowPanel Advanced P4 7/9/10.1″ (v1.2+) | `CrowPanel_Advanced_P4_7_9_10.1inch.zip` | |
| Elecrow CrowPanel Advanced P4 7/9/10.1″ (v1.1) | `CrowPanel_Advanced_P4_7_9_10.1inch_v1.1.zip` | |
| CYDDualUSB | `CYDDualUSB.zip` | |
| CYD2432S028R | `CYD2432S028R.zip` | <img src="../images/CYD2432S028R.jpg" alt="CYD2432S028R"> |
| Waveshare 7″ | `Waveshare_LCD.zip` | |
| Crowtech 7″ | `Crowtech_LCD.zip` | |
| Sunton 7″ | `Sunton_LCD.zip` | |
| Cardputer | `ESP32-S3-Cardputer.zip` | <img src="../images/m5_cardputer.jpg" alt="M5 Stack Cardputer"> |
| Cardputer ADV | `CardputerADV.zip` | |
| MarauderV4 | `MarauderV4_FlipperHub.zip` | |
| Marauder V8 | `MarauderV8.zip` | |
| Marauder Pancake | `MarauderPancake.zip` | |
| MarauderV6 & AwokDual | `MarauderV6_AwokDual.zip` | |
| AwokMini | `AwokMini.zip` | |
| Awok V5 | `esp32v5_awok.zip` | |
| LilyGo T-Dongle-S3 | `LilyGo-TDongleS3.zip` | |
| LilyGo T-Dongle-C5 | `LilyGo-TDongleC5.zip` | |
| T-Display S3 Touch | `LilyGo-TDisplayS3-Touch.zip` | |
| S3TWatch | `LilyGo-S3TWatch-2020.zip` | |
| TEmbed CC1101 | `LilyGo-TEmbedC1101.zip` | <img src="../images/lilygo_tembed_cc1101.jpg" alt="Lily Go Tembed cc1101"> |
| GhostBoard | `ghostboard.zip` | <img src="../images/rabbit_labs_ghost_board_black.jpg" alt="Black Rabbit Labs Ghost Board"> |
| RabbitLabs Poltergeist | `Poltergeist.zip` | |
| T-Deck | `LilyGo-T-Deck.zip` | <img src="../images/lilygo_tdeck_plus.jpg" alt="LilyGo T-Deck Plus"> |
| JCMK DevBoardPro | `JCMK_DevBoardPro.zip` | |
| RabbitLabs Minion | `RabbitLabs_Minion.zip` | <img src="../images/rabbit_labs_minion.jpg" alt="Rabbit Labs Minion"> |
| RabbitLabs Phantom | `CYD2USB2.4Inch.zip` | <img src="../images/rabbit_labs_phantom.jpg" alt="Rabbit Labs Phantom"> |
| Lolin S3 Pro | `Lolin_S3_Pro.zip` | <img src="../images/lolin_s3_pro.jpg" alt="Lolin S3 Pro"> |
| Seeed Studio XIAO ESP32-S3 Sense | `XIAO_S3_Sense.zip` | |
| Seeed Studio XIAO ESP32-C5 | `XIAO_C5.zip` | |
| Flipper JCMK GPS | `Flipper_JCMK_GPS.zip` | <img src="../images/flipper_wifi_devboard.jpg" alt="Flipper Wifi Dev Board + JCMK GPS Mod"> |
| JC3248W535EN | `JC3248W535EN_LCD.zip` | |
| Wired Hatters ESPRocket | `esp32-generic.zip` | <img src="../images/wired_hatters_rocket.jpg" alt="Wired Hatters ESPRocket"> |
| Wired Hatters Ultimate Marauder | Red Port: `esp32-generic.zip` and Blue Port: `MarauderV4_FlipperHub.zip` | <img src="../images/wired_hatters_ultimate_marauder.jpg" alt="Wired Hatters Ultimate Marauder"> |
| Heltec V3 | `HeltecV3.zip` | |
| Wired Hatters Banshee C5 | `Banshee_C5.zip` | |
| Wired Hatters Banshee S3 | `Banshee_S3.zip` | |
| M5Stack CoreS3-SE | `M5Stack_CoreS3-SE.zip` | |
| M5Stack AtomS3R | `M5Stack_AtomS3R.zip` | |

> **Note:** Images are being added as they become available.
