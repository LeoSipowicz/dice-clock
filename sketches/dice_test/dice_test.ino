#include "lgfx_config.h"
#include "dice_data.h"
#define SCREEN_WIDTH  800
#define SCREEN_HEIGHT 480
#define DICE_WIDTH  154
#define DICE_HEIGHT 300
LGFX lcd;
LGFX_Sprite canvas(&lcd);
void render() {
    canvas.fillScreen(TFT_BLACK);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextDatum(middle_center);
    canvas.drawString("good fucking luck", SCREEN_WIDTH / 2, 70);
    int dice_x = (SCREEN_WIDTH - DICE_WIDTH) / 2;
    int dice_y = 150;
    canvas.drawJpg(dice_jpg_data, dice_jpg_data_len, dice_x, dice_y);
    canvas.pushSprite(0, 0);
}
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== Dice Test (LovyanGFX) ===");
    lcd.init();
    lcd.setColorDepth(16);
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    if (!canvas.createSprite(SCREEN_WIDTH, SCREEN_HEIGHT)) {
        Serial.println("ERROR: could not allocate canvas in PSRAM");
        lcd.fillScreen(TFT_BLACK);
        lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        lcd.setFont(&fonts::FreeSansBold24pt7b);
        lcd.setTextDatum(middle_center);
        lcd.drawString("good fucking luck", SCREEN_WIDTH / 2, 70);
        lcd.drawJpg(dice_jpg_data, dice_jpg_data_len,
                    (SCREEN_WIDTH - DICE_WIDTH) / 2, 150);
        return;
    }
    render();
    Serial.println("Display complete!");
}
void loop() {
    delay(5000);
}
