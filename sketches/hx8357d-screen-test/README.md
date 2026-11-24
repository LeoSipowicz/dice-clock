# HX8357D 320x480 UI Test

## Parts

| Qty | Part |
|----|------|
| 1 | Adafruit 3.5" 320x480 + touchscreen breakout w/MicroSD socket (HX8357D) |
| 1 | ESP32-S3-DevKitC-1 (ESP32-S3-WROOM-2, 32MB flash / 16MB PSRAM) |
| — | Breadboard and 8 jumper wires (9 for the ID probe) |
| — | 5V supply |

## Wiring

```
  ESP32-S3-DevKitC-1                     3.5" HX8357D breakout
  ──────────────────                     ─────────────────────
    5V    ─────────────────────────────► VIN      (5V, NOT 3V3)
    GND   ─────────────────────────────► GND
    GPIO 10 ───────────────────────────► CS       (TFT_CS / SS)
    GPIO  9 ───────────────────────────► DC       (D/C / RS / A0)
    GPIO  8 ───────────────────────────► RST      (RESET / RSTB)
    GPIO 12 ───────────────────────────► SCK      (CLK / SCL)
    GPIO 11 ───────────────────────────► MOSI     (SDI / DIN / MO)
    GPIO 13 ───────────────────────────► MISO     (SDO / DO / MI)  optional
    GPIO  7 ───────────────────────────► LITE     (LED / BL / BLK)

  Leave empty:  3Vo / 3V3   SD_CS   T_CS   T_IRQ   X+ X- Y+ Y-
```

| Breakout pin | ESP32-S3 |
|---|---|
| `VIN` | **5V** |
| `GND` | GND |
| `3Vo` / `3V3` | *leave empty* |
| `CS` | GPIO 10 |
| `DC` | GPIO 9 |
| `RST` | GPIO 8 |
| `SCK` | GPIO 12 |
| `MOSI` | GPIO 11 |
| `MISO` | GPIO 13 |
| `LITE` | GPIO 7 |

## Arduino IDE settings

| Tools setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | OPI PSRAM |
| Flash Size | 32MB (256Mb) |
| Flash Mode | OPI 80MHz |
| USB CDC On Boot | Enabled |
| Upload Mode | UART0 / Hardware CDC |
| CPU Frequency | 240MHz (WiFi) |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 921600 |
| Arduino Runs On | Core 1 |
| Events Run On | Core 1 |
| Core Debug Level | None |
| Erase All Flash Before Sketch Upload | Disabled |
| JTAG Adapter | Disabled |

## flash.sh

```sh
sketches/hx8357d-screen-test/flash.sh
```

## Libraries

| Library | Version used here |
|---|---|
| esp32 (Espressif ESP32 core) | 3.3.11 |
| LovyanGFX (lovyan03) | 1.2.28 |

## Serial commands

| Key | Action |
|---|---|
| `f` | full-screen fills |
| `c` | colour bars |
| `g` | per-channel ramps |
| `t` | text + geometry |
| `s` | splash |
| `b` | backlight fade 0 → 255 |
| `d` | chip / PSRAM / panel / pin diagnostics |
| `i` | toggle inversion |
| `r` | next rotation |
| `a` | resume auto-cycle |
| `p` | reboot, re-run raw-SPI pre-flight |
| `?` | menu |

## Controller IDs

| ID | Controller |
|---|---|
| `0x93BF` | HX8357D |
| `0x9357` | HX8357B |
| `0x9488` | ILI9488 |
| `0x9486` | ILI9486 |
| `0x9481` | ILI9481 |
| `0x7796` | ST7796 |
| `0x9341` | ILI9341 (240x320) |

## Tuning knobs

| Define | Default |
|---|---|
| `TFT_PANEL` | `TFT_PANEL_HX8357D` |
| `TFT_SPI_WRITE_HZ` | `20000000` |
| `TFT_SPI_MODE` | `0` |
| `TFT_INVERT` | `true` |
| `TFT_RGB_ORDER` | `false` |
| `TFT_PIN_MISO` | `13` |

```
build.extra_flags=-DTFT_PANEL=TFT_PANEL_ILI9488
```
