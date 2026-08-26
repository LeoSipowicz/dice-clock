# Dice Test Sketch (LovyanGFX)

This sketch displays the text "good fucking luck" and a JPEG dice image on the
Waveshare ESP32-S3 4.3" RGB LCD (ST7262, 800x480). It uses LovyanGFX.

## Files in This Sketch

- **dice_test.ino** - Main sketch. Draws text and the JPEG into an off-screen
  canvas, then pushes it to the panel in one shot.
- **lgfx_config.h** - LovyanGFX config for the RGB panel (pins and timings).
- **dice_data.h** - Embedded dice image (JPEG, 300x300, ~3.4KB).

## Prerequisites

Install LovyanGFX from the Arduino IDE Library Manager:

1. **Arduino IDE -> Sketch -> Include Library -> Manage Libraries**
2. Search for **LovyanGFX** (by lovyan03).
3. Install the latest version.

No other display library is needed. LovyanGFX decodes the JPEG and draws the
fonts.

## Board Settings

- **Tools -> Board:** your ESP32-S3 board.
- **Tools -> PSRAM:** enable PSRAM ("OPI PSRAM" for this board). The canvas
  needs about 768 KB of PSRAM.
- **Tools -> Port:** your USB port.

## Upload

1. Open `dice_test.ino`.
2. Select the board, PSRAM, and port.
3. Upload (Cmd+U).
4. Open Serial Monitor at baud 115200.

## How It Works

1. `lgfx_config.h` defines the RGB panel (pins, timings, framebuffer in PSRAM).
2. `setup()` creates a full-screen `LGFX_Sprite` (canvas) in PSRAM.
3. `render()` draws the text and the JPEG into the canvas.
4. `canvas.pushSprite(0, 0)` sends the finished frame to the panel at once.

Drawing into an off-screen canvas removes the flicker. The whole frame appears
in one update instead of pixel by pixel.

## Troubleshooting

### "LovyanGFX.hpp: No such file or directory"
Install LovyanGFX from the Library Manager (see Prerequisites).

### "ERROR: could not allocate canvas in PSRAM"
Enable PSRAM in Tools. The sketch falls back to drawing straight to the panel,
which works but can flicker.

### Wrong colors on the dice
The red and blue channels can be swapped on some panels. Tell me and I will
adjust the data-pin order in `lgfx_config.h`.
