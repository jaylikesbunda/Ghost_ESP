---
title: "LoRa Hardware"
description: "SX1262 wiring, power, and board notes for GhostESP LoRa"
keywords: ["LoRa", "SX1262", "Heltec V3", "Heltec V3.2", "Vext", "SPI", "Kconfig"]
weight: 50
---

## Heltec V3 / V3.2

Ship with ESP32-S3 + SX1262 + SX1262 TCXO + OLED on the same PCB. Board configs power the radio via **Vext**.

The onboard radio uses SCK GPIO9, MISO GPIO11, MOSI GPIO10, CS GPIO8, DIO1 GPIO14, BUSY GPIO13, and RESET GPIO12 on both V3 and V3.2.

- **Vext / VEXT power:** GPIO drives the external 3.3 V rail for SX1262 and OLED. GhostESP asserts Vext at `lora start` and de-asserts it at `lora stop`.
- **Heltec V3.2 notes:** same pins as V3 but with updated TCXO and corrected antenna switch. Use the `Heltec V3.2` board config; V3 config also works but may log TCXO warm-up warnings. Battery voltage divider is on a different ADC channel — fuel gauge scaling differs.

## SPI and optional SD on S3

Heltec V3 does not include an SD socket. If no external SD board is fitted, messages such as `mosi not valid` or `Failed to find a free SPI host for SD` mean the optional SD pins are not configured; they do not indicate a LoRa failure.

- A following `SX1262 ready` line confirms that the radio initialized successfully.
- For an external SD board, configure valid pins and either use a separate SPI host or the supported shared-bus wiring. Do not assign the SX1262 chip-select pin to the SD card.

## Elecrow CrowPanel Advance 4.3 (Meshtastic wireless module)

Use the `sdkconfig.crowpanel_advance43` profile with the Elecrow SX1262 wireless module. Before powering the panel, set the rear function switch to **`01 (WM)`**. This routes the shared connector to the wireless module; **TF/SD is not available in this mode**.

The profile matches Elecrow/Meshtastic wiring: SPI `SCK=GPIO5`, `MISO=GPIO4`, `MOSI=GPIO6`, `NSS=GPIO0`, `DIO1=GPIO20`, `BUSY/DIO2=GPIO2`, and `RESET=GPIO19`. The module’s DIO3 TCXO is configured for **3.3 V**. After flashing, run `lora start`, then confirm `SX1262 ready` and use `lora status`/`lora diag`.

Expected ready-log suffix: `pins=6/4/5/0 irq=20 busy=2 rst=19 tcxo=0x07`.

## Elecrow CrowPanel Advanced P4 7/9/10.1 (Meshtastic wireless module)

Use `sdkconfig.crowpanel_advanced_p4_mipi_1024x600` (v1.2+) or
`sdkconfig.crowpanel_advanced_p4_mipi_1024x600_v11` (v1.1) with the Elecrow
SX1262 module in the wireless socket. The module shares the SPI3/GPIO27/28
expansion path with the optional nRF24 and UART passthrough, so only one of
those functions can be enabled per build.

The socket follows Elecrow's `RADIO_GPIO_*` defines: SPI `SCK=GPIO8`, `MISO=GPIO7`,
`MOSI=GPIO6`, `NSS=GPIO10`, and `BUSY=GPIO9`. **DIO1/RESET are GPIO27/28** on
v1.2+ boards or **GPIO53/54** on v1.1. The module's DIO3 TCXO is configured for
**3.3 V**. Unlike the S3 Advance boards, the onboard C6 SDIO (GPIO14-19) and the
microSD (SDMMC GPIO39/43/44) use separate pins, so Wi-Fi/BT and SD stay
available while LoRa is running. The profile also parks the default GhostLink
UART pins (TX GPIO6 / RX GPIO7) so boot never claims the radio's MOSI/MISO.
After flashing, run `lora start`, then confirm `SX1262 ready` and use
`lora status`/`lora diag`.

Expected ready-log suffix (v1.2+): `pins=6/7/8/10 irq=27 busy=9 rst=28 tcxo=0x07`.
Expected ready-log suffix (v1.1): `pins=6/7/8/10 irq=53 busy=9 rst=54 tcxo=0x07`.

## Elecrow CrowPanel Advance 2.4 / 2.8 (wireless module)

Use `sdkconfig.crowpanel_advance24` or `sdkconfig.crowpanel_advance28` with the Elecrow SX1262 module attached. The firmware drives the module-select pin during `lora start`; these small-panel profiles keep the display on SPI2 and reserve SPI3 for the radio, so SD/TF is intentionally skipped while LoRa is enabled.

The factory wireless-module wiring is `SCK=GPIO10`, `MISO=GPIO9`, `MOSI=GPIO3`, `NSS=GPIO0`, `DIO1=GPIO1`, `BUSY=GPIO46`, `RESET=GPIO2`, with **GPIO45 driven LOW** to select the module. The module uses a 3.3 V DIO3 TCXO. Expected ready-log suffix: `pins=3/9/10/0 irq=1 busy=46 rst=2 tcxo=0x07`.

## Custom board Kconfig pins

For a generic SX1262 on any ESP32, wire via custom board wiring and set:

```text
CONFIG_HAS_LORA=y
CONFIG_LORA_NSS_PIN=<gpio>
CONFIG_LORA_SCK_PIN=<gpio>
CONFIG_LORA_MOSI_PIN=<gpio>
CONFIG_LORA_MISO_PIN=<gpio>
CONFIG_LORA_RST_PIN=<gpio>
CONFIG_LORA_BUSY_PIN=<gpio>
CONFIG_LORA_DIO1_PIN=<gpio>
CONFIG_LORA_VEXT_PIN=<gpio or -1 if not used>
CONFIG_LORA_ENABLE_PIN=<gpio or -1 if a module-select pin is used>
```

- **DIO1 / BUSY / RESET** are mandatory; `DIO2`/`DIO3` are driven by driver for TCXO/RF switch.
- `LORA_ENABLE_PIN` is optional and is driven before radio bring-up; the Elecrow 2.4/2.8 wireless module uses GPIO45 active-low.
- Frequency and region are runtime (`lora set region`), not Kconfig.
- Verify with `lora status` and `lora diag` after flashing. See [Supported Hardware]({{< relref "../getting-started/supported-hardware.md" >}}) for board matrix.

For wiring help, see [Development: Custom Board Configs]({{< relref "../development/custom-board-configs.md" >}}).
