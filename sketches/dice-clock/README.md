# Dice Clock

## Parts

| Qty | Part |
|----|------|
| 1 | Waveshare ESP32-S3-Touch-LCD-5 |
| 1 | CR927 lithium cell |
| 1 | PCF8574 I2C expander module |
| 3 | Momentary push buttons (normally open) |
| 3 | 10 kΩ resistors |
| — | Jumper/hookup wire, small flathead screwdriver |

## Wiring

```
   ESP32-S3-Touch-LCD-5                 PCF8574 module
   (green screw terminal)               (powered at 3.3 V)
   ──────────────────────               ─────────────────
     VOUT  (3.3 V)   ──────────────────►  VCC
     GND             ──────────────────►  GND
     SDA  (GPIO 8)   ──────────────────►  SDA
     SCL  (GPIO 9)   ──────────────────►  SCL

   Leave every other terminal empty:
     VIN, GND, CANL, CANH, 485B, 485A, DO0, DO1, DICOM, GND, DI0, DI1
```

```
        PCF8574                             3V3
        ┌────────┐                            │
   P1 ──┤        ├──[ SET   ]──┐       10k ───┤
   P2 ──┤        ├──[ PLUS  ]──┼── GND 10k ───┤
   P5 ──┤        ├──[ MINUS ]──┘       10k ───┘
        └────────┘
```

## Expander address

```
address = 0x20 + A2*4 + A1*2 + A0
```

```
A0 ──► VCC
A1 ──► GND
A2 ──► GND
```

## Buttons

| Button | Expander pin | Action |
|-------|------|--------|
| SET   | P1 | run → set hours → set minutes → run |
| PLUS  | P2 | +1 (hold to repeat) |
| MINUS | P5 | −1 (hold to repeat) |

## Colours

| What | Where |
|---|---|
| Background | `--bg RRGGBB` in `build_frames.py`, `bgColor` in the sketch |
| Dice brightness | `--brighten N` in `build_frames.py` |

```sh
python3 scripts/build_frames.py --emit-asm
python3 scripts/build_frames.py --brighten 1.8 --emit-asm
python3 scripts/build_frames.py --bg 0A3D14 --emit-asm
```

## Panel orientation

| `FLIP_180` | Panel |
|---|---|
| `1` | mounted upside down |
| `0` | mounted right-side up |

## Build and flash

### Arduino IDE

| Tools setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | OPI PSRAM |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | Custom |
| USB CDC On Boot | Enabled |
| CPU Frequency | 240MHz (WiFi) |
| Flash Mode | QIO 80MHz |
| Upload Speed | 921600 |
| Arduino Runs On | Core 1 |
| Events Run On | Core 1 |
| Core Debug Level | Info |
| Erase All Flash Before Sketch Upload | Disabled |

### flash.sh

```sh
sketches/dice-clock/flash.sh
sketches/dice-clock/flash.sh /dev/cu.usbmodem2101
FQBN=esp32:esp32:esp32s3:PSRAM=opi,FlashSize=8M,FlashMode=qio,PartitionScheme=custom,CDCOnBoot=cdc \
    sketches/dice-clock/flash.sh
```

### Frames in flash

```sh
python3 scripts/build_frames.py --emit-asm
```

## Libraries

| Library | Version used here |
|---|---|
| esp32 (Espressif ESP32 core) | 3.3.11 (3.3.12 also builds) |
| LovyanGFX (lovyan03) | 1.2.28 (1.2.30 also builds) |
| ESP32_Display_Panel | 1.0.4 |
| ESP32_IO_Expander | 1.1.1 |
| esp-lib-utils | 0.3.0 |
