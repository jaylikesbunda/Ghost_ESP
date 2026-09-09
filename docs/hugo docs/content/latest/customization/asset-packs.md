---
title: "Asset Packs"
description: "Customize GhostESP icons, colors, and backgrounds with asset packs"
weight: 10
---

Asset packs let you customize the GhostESP UI: colors, icons, and background images. Pack everything into a single `.gtheme` archive or an extracted folder on your SD card.

## What You Can Customize

| Feature | PSRAM Devices | Internal-Only Devices |
|---------|--------------|----------------------|
| Colors (accent, background, surface, text) | Yes | Yes |
| Icons (up to 32 cached) | Yes | 2 cached |
| Background images (tiled or scaled) | Yes | Yes, with low-RAM variants |
| Custom icon sizes per resolution | Yes | Yes |

## Creating a Pack

### 1. Source Folder Structure

Create a folder with a `manifest.json` and your PNG artwork:

```
my_pack/
  manifest.json
  art/
    icons/
      wifi.png
      bluetooth.png
    background.png
```

### 2. Manifest Format

```json
{
  "id": "my_pack",
  "name": "My Pack",
  "version": 1,
  "app_icon": "GESPAppGallery",
  "colors": {
    "accent": "0x39FF14",
    "background": "0x050008",
    "surface": "0x120018",
    "surface_alt": "0x24102A",
    "text": "0xFFFFFF",
    "text_muted": "0x9A80AA"
  },
  "icon_variants": [32, 64],
  "icon_sources": {
    "wifi": "art/icons/wifi.png",
    "bluetooth": "art/icons/bluetooth.png",
    "Map": "art/icons/map.png",
    "settings_icon": "art/icons/settings.png",
    "clock_icon": "art/icons/clock.png",
    "ghost": "art/icons/ghost.png",
    "terminal_icon": "art/icons/terminal.png",
    "rave": "art/icons/rave.png",
    "GESPAppGallery": "art/icons/gallery.png"
  },
  "background_sources": {
    "background": {
      "source": "art/background.png",
      "variants": true
    }
  }
}
```

**Color values** are hex RGB without alpha. Supported keys: `accent`, `background`, `surface`, `surface_alt`, `text`, `text_muted`.

**Icon variants** define pixel sizes. The firmware picks the best fit for the screen. Small screens use the smaller variant, large screens use the larger one.

**`app_icon`** is optional. When present, App Gallery entries without a matching app-specific icon use that asset-pack icon. Leave it out if you only want exact icon-key replacements.

**Supported icon keys** match the main menu and built-in app gallery:

| Key | Location |
|-----|----------|
| `wifi` | Main menu |
| `bluetooth` | Main menu |
| `Map` | Main menu |
| `clock_icon` | Main menu |
| `settings_icon` | Main menu |
| `GESPAppGallery` | Main menu / App gallery |
| `folder` | App gallery / plugin categories |
| `description` | App gallery |
| `storefront` | App gallery |
| `lock` | Main menu |
| `dualcomm` | Main menu |
| `lan_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48` | Main menu |
| `infrared`, `nfc_icon`, `nrf24`, `subghz`, `lora`, `usb` | Optional hardware menus |
| `compass`, `enviii`, `accelerometer_icon` | Optional sensor menus |
| `ghost` | App gallery |
| `terminal_icon` | App gallery |
| `rave` | App gallery |
| `speaker_50dp_FFFFFF_FILL0_wght400_GRAD0_opsz48` | Audio app |

**Background images** can be generated as a variant set from one source PNG by using `"variants": true` on the `background` source. GBT writes these runtime manifest entries:

| Variant | Default size | Default format | Use |
|---------|--------------|----------------|-----|
| `full` | 240x320 | `rgb565` | PSRAM boards |
| `half` | 120x160 | `indexed_4bpp` | Internal-only boards, scaled to fill |
| `tiny` | 80x107 | `indexed_4bpp` | Low-memory fallback, scaled to fill |
| `tile` | 32x32 | `indexed_4bpp` | Last-resort tiled fallback |

The firmware picks `full → tiny → tile` on PSRAM boards and `half → tiny → tile` on internal-only boards. Scaled `indexed_4bpp` backgrounds are rendered by a custom line scaler, so the 120x160 half variant stays around 9.5 KB in RAM instead of expanding to a full RGB565 copy. The main menu, app gallery, settings-style screens, and other shared-layout views all use the same background.

Legacy packs can still provide `bg_tile`; newer packs should prefer the `background` variant source above.

### 3. Building

```bash
# Build an extracted folder
gbt asset pack ./my_pack --out ./dist

# Build a .gtheme archive
gbt asset pack ./my_pack --out ./dist --archive
```

Source PNGs must be 8-bit RGBA. The converter handles resizing to the target dimensions.

## Installing

### Option A: Extracted Folder

Copy the generated folder to your SD card:

```
/mnt/ghostesp/themes/my_pack/
  manifest.json
  icons/
    wifi_l.gimg
    wifi_s.gimg
    ...
  bg/
    bg_full.gimg
    bg_half.gimg
    bg_tiny.gimg
    bg_tile.gimg
```

### Option B: .gtheme Archive

Copy the archive to:

```
/mnt/ghostesp/themes/my_pack.gtheme
```

The firmware extracts it on first load to:

```
/mnt/ghostesp/themes/.cache/my_pack/
```

Subsequent boots and pack switches reuse that cached extraction if the archive size and modified time haven't changed. Updating `my_pack.gtheme` automatically invalidates the cached copy and extracts it again.

## Selecting a Pack

1. Insert SD card with your pack
2. Go to **Settings → Display & Brightness → Appearance → Asset Pack**
3. Press **left/right** to cycle through installed packs
4. The pack loads immediately and the selection is saved

Missing icons fall back to the compiled-in defaults. You don't need to include every icon.

## Image Formats

| Format | Code | Description |
|--------|------|-------------|
| `rgb565` | 1 | 16-bit color, no alpha. Used for backgrounds. |
| `rgb565a8` | 2 | 16-bit color + 8-bit alpha plane. Used for icons. |
| `indexed_4bpp` | 3 | 16-color indexed (4 bits/pixel + 64-byte palette). Smallest icon and low-RAM background format. |

The `rgb565a8` format stores RGB565 and alpha as separate planes (not interleaved). The `gbt asset pack` tool handles this automatically. GIMG files store standard RGB565; firmware adapts the in-memory byte order for boards built with `LV_COLOR_16_SWAP`.

The `indexed_4bpp` format quantizes source PNGs to a 16-color palette at build time and packs two pixels per byte in the payload. A 32x32 indexed icon uses ~576 bytes (64-byte palette + 512 bytes of packed indices) — roughly 1/5 the size of the equivalent `rgb565a8` image and ideal for internal-only devices where every kilobyte of icon cache counts. A 120x160 indexed background uses ~9.5 KB. The 16-color limit is enough for most line-art and flat-color menu icons; smooth gradients show visible banding. The firmware renders indexed icons natively through LVGL and renders scaled indexed backgrounds with a custom line scaler, avoiding full-image RGB expansion. Packs can mix formats per image source via the manifest's `icon_format` and per-background `format` fields.

## PSRAM vs Internal RAM

**PSRAM devices** (most ESP32-S3 boards):
- Up to 32 icons cached in PSRAM
- Background images supported (full, scaled, or tiled)
- Each 64x64 icon uses ~12 KB PSRAM
- Full-screen 240x320 background uses ~150 KB PSRAM
- Any image format works; deflate-compressed payloads decode into PSRAM
- On boot and runtime pack switches, the background is loaded first and icon preload can be deferred so the UI becomes responsive sooner. Missing or not-yet-warmed icons temporarily fall back to compiled-in artwork.

**Internal-only devices** (ESP32-C5, some S2):
- 2 icons cached in internal RAM
- Scaled `half`/`tiny` indexed backgrounds are supported without full RGB expansion; 32x32 tiled background remains the last fallback
- Icon size is capped at 32x32 RGB565A8 (3 KB) per slot — anything bigger is rejected
- Deflate-compressed payloads are rejected; only uncompressed `.gimg` files load
- Use `indexed_4bpp` for icons and low-RAM background variants. A 32x32 indexed icon is 576 bytes vs 3,072 bytes for RGB565A8
- Colors and icon mappings still work; icons fall back to compiled-in artwork when the cache is full

## Converting a Single Image

```bash
gbt asset image ./icon.png --out ./icon.gimg --width 64 --height 64 --format rgb565a8
```

## Example Pack

See `examples/asset_packs/neon_ghost/` in the repository for a working sample pack.
