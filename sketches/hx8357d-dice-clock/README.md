# Dice Clock - HX8357D 320x480

|  |  |
|---|---|
| Panel | 320x480 HX8357D over SPI, landscape (480x320) |
| Layout | die1 die2 : die3 die4 |
| Frames | 106x136 digits, 20x44 colon, embedded in flash |
| Sketch size | ~2.81 MB |

## Parts

| Qty | Part |
|----|------|
| 1 | Adafruit 3.5" 320x480 + touchscreen breakout w/MicroSD socket (HX8357D) |
| 1 | ESP32-S3-DevKitC-1 (ESP32-S3-WROOM-2, 32MB flash / 16MB PSRAM) |
| - | Breadboard and 8 jumper wires |
| - | 5V supply |

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
    GPIO 11 ───────────────────────────► MOSI     (SDI / DIN)
    GPIO 13 ───────────────────────────► MISO     (SDO / DO)
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
| `SD_CS` | *unused* |

## Rebuilding the archive

```
python3 scripts/build_frames.py --target-w 106 --colon-tight --brighten 1.2 --emit-asm \
    --bg 00410A --out sketches/hx8357d-dice-clock/frames_320x480.lz4
```

## Arduino IDE settings

| Tools setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| Partition Scheme | 32M Flash (13MB APP/6.75MB SPIFFS) |
| PSRAM | OPI PSRAM |
| Flash Size | 32MB (256Mb) |
| Flash Mode | OPI 80MHz |
| USB CDC On Boot | Enabled |
| Upload Mode | UART0 / Hardware CDC |
| CPU Frequency | 240MHz (WiFi) |
| Upload Speed | 921600 |
| Arduino Runs On | Core 1 |
| Events Run On | Core 1 |
| Core Debug Level | None |
| Erase All Flash Before Sketch Upload | Disabled |

## flash.sh

```sh
sketches/hx8357d-dice-clock/flash.sh
```

## Libraries

| Library | Version used here |
|---|---|
| esp32 (Espressif ESP32 core) | 3.3.11 |
| LovyanGFX (lovyan03) | 1.2.28 |

## Layout

```
  x=6         x=118            x=230   x=256           x=368
   ┌────────┐  ┌────────┐       ┌┐     ┌────────┐  ┌────────┐
   │  hour  │  │  hour  │       ││     │ minute │  │ minute │
   │  tens  │  │  ones  │       ││     │  tens  │  │  ones  │
   └────────┘  └────────┘       └┘     └────────┘  └────────┘
      die1        die2          colon     die3        die4

   each die 106x136, colon 20x44, 6px between them
```

## Tuning knobs

| Define | Default |
|---|---|
| `TFT_SPI_WRITE_HZ` | `20000000` |
| `TFT_SPI_MODE` | `0` |
| `TFT_INVERT` | `true` |
| `TFT_RGB_ORDER` | `false` |
| `TFT_PANEL` | `TFT_PANEL_HX8357D` |
