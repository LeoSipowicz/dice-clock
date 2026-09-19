# Dice Clock

A four-dice clock for the **Waveshare ESP32-S3-Touch-LCD-5** (800x480).

- Shows the time as four dice, ticking one minute per minute.
- The time comes from the board's on-board **PCF85063 RTC**, so with the CR927
  backup cell fitted the clock resumes where it left off instead of restarting.
  Without a cell it starts at **12:00** on every power-up.
- Three buttons on a **PCF8574** I2C expander set the time.

## Parts

| Qty | Part |
|----|------|
| 1 | Waveshare ESP32-S3-Touch-LCD-5 |
| 1 | CR927 lithium cell (RTC backup, non-rechargeable) |
| 1 | PCF8574 I2C expander module (8-channel, with A0/A1/A2 address jumpers) |
| 3 | Momentary push buttons (normally open) |
| 3 | 10 kΩ resistors (button pull-ups) |
| 1 | microSD card with `frames.lz4` in the root |
| — | Jumper/hookup wire, small flathead screwdriver |

> **Why three buttons and not four.** The sketch was written for four (POWER,
> SET, PLUS, MINUS), but this expander module only holds `P1`, `P2` and `P5`
> high — `P0`, `P3`, `P4`, `P6` and `P7` read low even with nothing attached, so
> a switch on those pins never registers. The three working pins carry SET, PLUS
> and MINUS, which is everything needed to set the clock; the backlight is simply
> left on. A genuine PCF8574 (or a TCA9554/PCA9554) would give all four.

## Wiring

The board's 16-way green screw terminal carries I2C on `SDA`/`SCL`. The expander
hangs off that bus, so nothing on the board is disturbed — the SD card, panel,
touch and serial all keep working.

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
   Each button: one leg to the expander pin, the other leg to expander GND.
   Plus a 10k pull-up from every button pin to 3V3 (see below).

        PCF8574                             3V3
        ┌────────┐                            │
   P1 ──┤        ├──[ SET   ]──┐       10k ───┤
   P2 ──┤        ├──[ PLUS  ]──┼── GND 10k ───┤
   P5 ──┤        ├──[ MINUS ]──┘       10k ───┘
        └────────┘
```

> **The pull-ups are required.** A PCF8574 does not have real pull-up
> resistors — each pin is only a ~100 µA current source when written high, which
> on a breadboard is not always enough to hold an open switch high. Fit one 10 kΩ
> resistor per button pin, from the pin to 3V3. Anything from 4.7 kΩ to 47 kΩ
> works (smaller is stronger).

### Setting the expander address

Every I2C chip answers to an address. The board's own **CH422G** already uses
**0x20**, so the expander must be moved off it. A PCF8574 has three address pins
`A0 A1 A2`, each a binary digit:

```
    address = 0x20 + A2*4 + A1*2 + A0
```

We want **0x21**, i.e. `A0` high and `A1`/`A2` low:

```
   A0 ──► VCC   (1)
   A1 ──► GND   (0)
   A2 ──► GND   (0)        =>  0x20 + 1 = 0x21
```

On the module this is either:

- **jumper caps** — a small header with `A0 A1 A2` next to `VCC`/`GND` rails.
  Put a cap linking the `A0` pin to `VCC`, and leave `A1`/`A2` alone (they
  default low). That is the whole change.
- **solder pads** — three little pads labelled `A0 A1 A2` beside `VCC` and
  `GND`. Bridge `A0` to `VCC` with a blob of solder and leave the others.

If the module has no way to change its address (fixed at 0x20) it cannot share
the bus with the CH422G — in that case a PCF8574**A** (base 0x38) or a TCA9554
is needed instead. The `I2C scan:` line printed at boot shows exactly what is on
the bus, so you can confirm rather than guess.

> **Power the expander from 3.3 V, never 5 V.** The module's on-board I2C
> pull-ups tie `SDA`/`SCL` to its `VCC`, so at 5 V it would drag the shared bus
> to 5 V and stress the ESP32 and the other chips on it.
>
> If `VOUT` measures 5 V, power the expander's `VCC` from a 3V3 pin elsewhere on
> the board instead.

## RTC and the backup cell

The board carries a **PCF85063** real-time clock on I2C address **0x51** — the
third device the `I2C scan:` reports. Fit a **CR927** cell in the holder on the
board and the RTC keeps running while the clock is unplugged, so the time is
still correct at the next power-up.

The cell is **non-rechargeable** and the holder is CR927-specific — do not
substitute a CR2032 or an LIR/ML rechargeable cell.

Worth being clear about what the cell does and does not do: it backs up **only
the RTC**, which draws about 0.22 µA. It cannot run the ESP32 or the display,
which draw hundreds of milliamps — a 25 mAh cell would last minutes.

How the sketch uses it:

- **On boot** it reads the RTC and starts from that time. If the chip reports
  that its oscillator stopped — first ever boot, no cell, or a flat cell — the
  sketch gives the calendar a valid date and starts it at **12:00**, then carries
  on from there.
- **While running** the RTC is the time source: the sketch reads it about four
  times a second, so the display follows the real minute and any drift in the
  ESP32's own timer is corrected automatically.
- **When you save** a new time in setting mode, it is written straight to the
  RTC, with seconds reset to zero so the minute boundary lands at that moment.
- **If there is no RTC** the sketch falls back to counting with `millis()`,
  starting from 12:00 — everything else keeps working.

Note the display is a 12-hour clock, so AM/PM is not shown. The sketch tracks
which half of the day the RTC is in so that writing a new time back keeps the
same half, but nothing on screen distinguishes 3:00 AM from 3:00 PM.

## Buttons

| Button | Expander pin | Action |
|-------|------|--------|
| SET   | P1 | Cycle: run → set hours → set minutes → save (back to run) |
| PLUS  | P2 | Active field +1 (hold to repeat) |
| MINUS | P5 | Active field −1 (hold to repeat) |

While setting hours or minutes the clock is paused; pressing SET through to
"save" returns to run and the time continues from the value you set. Holding
PLUS/MINUS auto-repeats after 0.5 s.

The backlight has no button on this build — it is switched on at boot and left
on. (The original design used a fourth button on `P0` for that, but this module
cannot read `P0`.)

## Build and flash

1. Regenerate the frame archive (only needed if frames or the background changed):
   ```
   python3 scripts/build_frames.py
   ```
2. Copy `sd_card/frames.lz4` to the **root of the microSD card**.
3. Open `sketches/dice-clock/dice-clock.ino` and select **Tools → Board →
   esp32 → ESP32S3 Dev Module**, then set the Tools menu as below. Only the
   three in **bold** differ from the defaults:

   | Tools setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | **PSRAM** | **OPI PSRAM** |
   | **Flash Size** | **16MB (128Mb)** |
   | **Partition Scheme** | **8M with spiffs (3MB APP/1.5MB SPIFFS)** |
   | USB CDC On Boot | Disabled |
   | CPU Frequency | 240MHz (WiFi) |
   | Flash Mode | QIO 80MHz |
   | Upload Speed | 921600 |
   | Arduino Runs On | Core 1 |
   | Events Run On | Core 1 |
   | Core Debug Level | Info (for the `dice:` boot logs) |
   | Erase All Flash Before Sketch Upload | Disabled |
4. Upload.

### Libraries

Install from **Library Manager** if not already present:

| Library | Version used here |
|---|---|
| esp32 (Espressif ESP32 core) | 3.3.11 |
| LovyanGFX (lovyan03) | 1.2.28 |
| ESP32_Display_Panel | 1.0.4 |
| ESP32_IO_Expander | 1.1.1 |
| esp-lib-utils | 0.3.0 |

`SPI`, `FS` and `SD` ship with the ESP32 core.

## Bring-up

The serial monitor (115200) prints the boot sequence and, once a second, a live
status line:

```
[dice] === Dice Clock ===
[dice] panel configured with 2 frame buffers
[dice] board begun
[dice] RTC ready at 0x51, time 12:00:00
[dice] framebuffers wrapped
[dice] sprites created
[dice] SD mounted: card type 2, 480 MB
[dice] archive ready: 451 frames, max block 16665 B
[dice] resting faces ready
[dice] first frame shown
[dice] I2C scan:
[dice]   found 0x20
[dice]   found 0x21
[dice]   found 0x51
[dice] PCF8574 responds at 0x21
[dice] buttons ready (PCF8574 @ 0x21), all pins at rest 0x26
[dice] buttons: SET=P1 PLUS=P2 MINUS=P5
[dice] setup complete
[dice] FPS 3 flips/s, panel 39 Hz, ... | pins now 0x26 seen 0x26
```

- **`all pins at rest 0x26`** is expected on this module: `P1`, `P2` and `P5`
  read high, everything else sits low. `0x26` is exactly those three bits. If
  one of the *three* button bits is `0` at rest, that switch is being held down
  or is wired to GND on both legs.
- The `pins now 0x?? seen 0x??` at the end of the one-second line is the live
  state of all eight pins (`bit7` = `P7` … `bit0` = `P0`). `seen` is every sample
  since the previous print, so a quick tap still shows up. Pressing a button
  drops its bit: **SET `0x24`**, **PLUS `0x22`**, **MINUS `0x06`**. Every change
  is also logged immediately as a `pins 0x??` line.
- Each press logs `button SET -> hours`, `button PLUS`, `button MINUS`.

If a press produces no `pins` line at all, that switch is not reaching the
expander pin it should — check the wire against the pin list above.

The clock still runs without the expander, so a bad button wire will not stop
the display.
