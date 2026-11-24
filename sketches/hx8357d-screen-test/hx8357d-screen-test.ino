#include <SPI.h>
#include "lgfx_config.h"
LGFX lcd;
static constexpr uint32_t C_BLACK   = lgfx::color888(  0,   0,   0);
static constexpr uint32_t C_WHITE   = lgfx::color888(255, 255, 255);
static constexpr uint32_t C_RED     = lgfx::color888(255,   0,   0);
static constexpr uint32_t C_GREEN   = lgfx::color888(  0, 255,   0);
static constexpr uint32_t C_BLUE    = lgfx::color888(  0,   0, 255);
static constexpr uint32_t C_YELLOW  = lgfx::color888(255, 255,   0);
static constexpr uint32_t C_CYAN    = lgfx::color888(  0, 255, 255);
static constexpr uint32_t C_MAGENTA = lgfx::color888(255,   0, 255);
static constexpr uint32_t C_GREY    = lgfx::color888(128, 128, 128);
static uint8_t brightness = 255;
static bool    invertOn   = TFT_INVERT;
static bool    autoCycle  = true;
#if TFT_PIN_MISO >= 0
static void rawReadId(uint8_t cmd, uint8_t out[4]) {
    digitalWrite(TFT_PIN_CS, LOW);
    digitalWrite(TFT_PIN_DC, LOW);
    SPI.transfer(cmd);
    digitalWrite(TFT_PIN_DC, HIGH);
    for (int i = 0; i < 4; i++) out[i] = SPI.transfer(0x00);
    digitalWrite(TFT_PIN_CS, HIGH);
}
static const char *identifyController(uint32_t id) {
    switch (id) {
    case 0x9488: return "ILI9488 -> set TFT_PANEL to TFT_PANEL_ILI9488";
    case 0x9486: return "ILI9486 -> set TFT_PANEL to TFT_PANEL_ILI9486";
    case 0x9481: return "ILI9481 -> set TFT_PANEL to TFT_PANEL_ILI9481";
    case 0x7796: return "ST7796 -> set TFT_PANEL to TFT_PANEL_ST7796";
    case 0x9341: return "ILI9341 (240x320) - wrong panel size for this wiring";
    case 0x93BF: return "HX8357D (expected) - keep TFT_PANEL_HX8357D";
    case 0x9357: return "HX8357B? - try TFT_PANEL_HX8357B if colours stay wrong";
    case 0x0000:
    case 0xFFFFFF: return "inconclusive - see the note below";
    default:       return "unrecognised - report the raw bytes";
    }
}
static void preflight() {
    Serial.println(F("--- raw-SPI pre-flight (before LovyanGFX is involved) ---"));
    pinMode(TFT_PIN_CS, OUTPUT);   digitalWrite(TFT_PIN_CS, HIGH);
    pinMode(TFT_PIN_DC, OUTPUT);   digitalWrite(TFT_PIN_DC, HIGH);
    pinMode(TFT_PIN_RST, OUTPUT);
    pinMode(TFT_PIN_MISO, INPUT_PULLUP);
    digitalWrite(TFT_PIN_RST, LOW);
    delay(20);
    digitalWrite(TFT_PIN_RST, HIGH);
    delay(150);
    SPI.begin(TFT_PIN_SCLK, TFT_PIN_MISO, TFT_PIN_MOSI, -1);
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    static const struct { uint8_t cmd; const char *label; } probes[] = {
        { 0xD3, "RDID4 (ILI9341 style)" },
        { 0x04, "RDDID (HX8357D style)" },
        { 0xBF, "RDID1-3"               },
        { 0xDA, "RDID1"                 },
        { 0xDB, "RDID2"                 },
        { 0xDC, "RDID3"                 },
    };
    uint8_t b[4];
    uint8_t rddid[4] = { 0, 0, 0, 0 };
    bool anyResponse = false;
    Serial.printf("  MISO=GPIO %d, 1 MHz, SPI mode 0\n", TFT_PIN_MISO);
    for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        rawReadId(probes[i].cmd, b);
        Serial.printf("  0x%02X %-23s -> %02X %02X %02X %02X\n",
                      probes[i].cmd, probes[i].label, b[0], b[1], b[2], b[3]);
        if (probes[i].cmd == 0x04) {
            rddid[0] = b[0]; rddid[1] = b[1]; rddid[2] = b[2]; rddid[3] = b[3];
        }
        for (int j = 0; j < 4; j++) {
            if (b[j] != 0x00 && b[j] != 0xFF) anyResponse = true;
        }
    }
    const uint32_t iddid = ((uint32_t)rddid[1] << 16) |
                           ((uint32_t)rddid[2] << 8) | rddid[3];
    SPI.endTransaction();
    SPI.end();
    if (anyResponse) {
        Serial.println(F("  verdict      : the panel drove MISO"));
        Serial.printf ("  ID via 0x04  : 0x%06lX  %s\n",
                       (unsigned long)iddid, identifyController(iddid));
        Serial.println(F("  -> SCK, MOSI, MISO, CS, DC and power are all proven good."));
        Serial.println(F("     If the screen is still blank this is a driver problem,"));
        Serial.println(F("     not a wiring one. Try TFT_SPI_MODE 3, then"));
        Serial.println(F("     TFT_SPI_WRITE_HZ 10000000, one at a time."));
    } else {
        Serial.println(F("  verdict      : inconclusive (all 0x00 / 0xFF)"));
        Serial.println(F("  -> this does NOT prove a wiring fault. HX8357D often returns"));
        Serial.println(F("     0x00 from read-ID even when working perfectly, and 0xD3 is an"));
        Serial.println(F("     ILI9341 command it may not implement at all."));
        Serial.println(F("     Judge the panel by the screen. If it stays blank, check:"));
        Serial.println(F("       1. VIN = 5V and GND = GND, measured at the breakout's own"));
        Serial.println(F("          pins, not at the ESP32 header."));
        Serial.println(F("       2. Nothing is feeding 3Vo - that pin is the regulator's"));
        Serial.println(F("          OUTPUT, not a supply input."));
        Serial.println(F("       3. No two of CS / DC / RST / MOSI / SCK are swapped."));
    }
}
#else
static void preflight() {
    Serial.println(F("--- raw-SPI pre-flight skipped (TFT_PIN_MISO is -1) ---"));
    Serial.println(F("    Wire MISO to GPIO 13 to probe the controller."));
    pinMode(TFT_PIN_RST, OUTPUT);
    digitalWrite(TFT_PIN_RST, HIGH);
    digitalWrite(TFT_PIN_RST, LOW);
    delay(20);
    digitalWrite(TFT_PIN_RST, HIGH);
    delay(150);
}
#endif
static void printDiagnostics() {
    Serial.println(F("--- chip / build ---"));
    Serial.printf("chip         : %s rev %d, %d core(s) @ %lu MHz\n",
                  ESP.getChipModel(), (int)ESP.getChipRevision(),
                  (int)ESP.getChipCores(), (unsigned long)ESP.getCpuFreqMHz());
    Serial.printf("flash        : %lu MB\n",
                  (unsigned long)(ESP.getFlashChipSize() / (1024UL * 1024UL)));
    Serial.printf("psram        : %lu KB total, %lu KB free\n",
                  (unsigned long)(ESP.getPsramSize() / 1024UL),
                  (unsigned long)(ESP.getFreePsram() / 1024UL));
    Serial.printf("heap         : %lu KB free\n",
                  (unsigned long)(ESP.getFreeHeap() / 1024UL));
    if (ESP.getPsramSize() == 0) {
        Serial.println(F("               ^ 0 KB PSRAM: set Tools -> PSRAM to"));
        Serial.println(F("                 'OPI PSRAM' for the WROOM-2."));
    }
#ifdef ESP_ARDUINO_VERSION_MAJOR
    Serial.printf("arduino core : %d.%d.%d\n", ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
#endif
    Serial.println(F("--- panel / bus ---"));
    Serial.printf("panel        : TFT_PANEL=%d, 320x480 native\n", (int)TFT_PANEL);
    Serial.printf("bus          : SPI2_HOST, mode %d, %u Hz write, MISO=%d\n",
                  (int)TFT_SPI_MODE, (unsigned)TFT_SPI_WRITE_HZ, (int)TFT_PIN_MISO);
    Serial.printf("pins         : CS=%d DC=%d RST=%d BL=%d MOSI=%d SCK=%d\n",
                  TFT_PIN_CS, TFT_PIN_DC, TFT_PIN_RST, TFT_PIN_BL,
                  TFT_PIN_MOSI, TFT_PIN_SCLK);
    Serial.printf("colour depth : enum %d\n", (int)lcd.getColorDepth());
    Serial.printf("geometry     : rot %u, %ldx%ld\n",
                  (unsigned)lcd.getRotation(), (long)lcd.width(), (long)lcd.height());
}
static void printMenu() {
    Serial.println();
    Serial.println(F("commands: f fills | c colour bars | g grey ramps | t text/geometry"));
    Serial.println(F("          s splash | b backlight | d diagnostics | i invert"));
    Serial.println(F("          r rotation | a auto-cycle | p reboot+preflight | ? menu"));
    Serial.println();
}
static void announce(const __FlashStringHelper *name) {
    Serial.printf("[test] %s  (rot %u, %ldx%ld)\n", (const char *)name,
                  (unsigned)lcd.getRotation(), (long)lcd.width(), (long)lcd.height());
}
static void bootFlash() {
    static const struct { uint32_t c; const char *n; } f[] = {
        { C_RED,   "red"   },
        { C_GREEN, "green" },
        { C_BLUE,  "blue"  },
        { C_WHITE, "white" },
        { C_BLACK, "black" },
    };
    Serial.println(F("[test] boot flash: red green blue white black"));
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        lcd.fillScreen(f[i].c);
        delay(250);
    }
}
static void showSplash() {
    const int32_t w = lcd.width();
    const int32_t h = lcd.height();
    const int32_t cx = w / 2;
    char buf[64];
    lcd.fillScreen(C_BLACK);
    lcd.drawRect(1, 1, w - 2, h - 2, C_WHITE);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(C_GREEN, C_BLACK);
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.drawString("HX8357D", cx, 60);
    lcd.setTextColor(C_WHITE, C_BLACK);
    lcd.setFont(&fonts::Font2);
    lcd.drawString("If you can read this,", cx, 120);
    lcd.drawString("the panel is working.", cx, 140);
    lcd.setTextColor(C_YELLOW, C_BLACK);
    snprintf(buf, sizeof(buf), "panel %d   rot %u", (int)TFT_PANEL,
             (unsigned)lcd.getRotation());
    lcd.drawString(buf, cx, 185);
    snprintf(buf, sizeof(buf), "%ldx%ld   %u MHz", (long)w, (long)h,
             (unsigned)(TFT_SPI_WRITE_HZ / 1000000UL));
    lcd.drawString(buf, cx, 205);
    lcd.setTextColor(invertOn ? C_CYAN : C_GREY, C_BLACK);
    lcd.drawString(invertOn ? "inversion ON" : "inversion OFF", cx, 230);
    const int32_t m = 24;
    lcd.fillRect(3, 3, m, m, C_RED);
    lcd.fillRect(w - 3 - m, 3, m, m, C_GREEN);
    lcd.fillRect(3, h - 3 - m, m, m, C_BLUE);
    lcd.fillRect(w - 3 - m, h - 3 - m, m, m, C_YELLOW);
    lcd.setTextColor(C_WHITE, C_BLACK);
    lcd.drawString("red TL green TR", cx, h - 80);
    lcd.drawString("blue BL yellow BR", cx, h - 60);
    lcd.drawString("press ? for the menu", cx, h - 30);
    lcd.setTextDatum(top_left);
    Serial.println(F("[test] splash shown"));
}
static void testBacklight() {
    Serial.println(F("[test] backlight: fading 0 -> 255"));
    for (int b = 0; b <= 255; b += 5) {
        lcd.setBrightness((uint8_t)b);
        delay(6);
    }
    brightness = 255;
    lcd.setBrightness(brightness);
    Serial.println(F("       backlight at 255."));
    Serial.println(F("       If the screen never changed brightness, the BL wire or"));
    Serial.println(F("       the breakout's LITE pin is the problem, not the SPI data."));
}
static void testFills() {
    static const struct { uint32_t color; const char *name; } steps[] = {
        { C_RED,   "red"      },
        { C_GREEN, "green"    },
        { C_BLUE,  "blue"     },
        { C_WHITE, "white"    },
        { C_BLACK, "black"    },
        { C_GREY,  "mid grey" },
    };
    announce(F("full-screen fills"));
    for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        Serial.printf("       %s\n", steps[i].name);
        lcd.fillScreen(steps[i].color);
        delay(900);
    }
    Serial.println(F("       Expect six clean flat colours. A solid but wrong"));
    Serial.println(F("       colour means a data line, not a timing problem."));
}
static void testColorBars() {
    static const uint32_t bars[] = {
        C_WHITE, C_YELLOW, C_CYAN, C_GREEN, C_MAGENTA, C_RED, C_BLUE, C_BLACK,
    };
    const int n = (int)(sizeof(bars) / sizeof(bars[0]));
    const int32_t w = lcd.width();
    const int32_t h = lcd.height();
    announce(F("colour bars"));
    int32_t x = 0;
    for (int i = 0; i < n; i++) {
        const int32_t x2 = (w * (i + 1)) / n;
        lcd.fillRect(x, 0, x2 - x, h, bars[i]);
        x = x2;
    }
    lcd.setTextColor(C_BLACK, C_WHITE);
    lcd.setFont(&fonts::Font2);
    lcd.setTextDatum(top_left);
    lcd.drawString("white yellow cyan green magenta red blue black", 2, 2);
}
static void drawRamp(int32_t x0, int32_t y0, int32_t w, int32_t h, int channel) {
    for (int32_t i = 0; i < w; i++) {
        const uint8_t v = (uint8_t)((i * 255) / (w - 1));
        const uint32_t c = (channel == 0) ? lgfx::color888(v, 0, 0)
                         : (channel == 1) ? lgfx::color888(0, v, 0)
                         : (channel == 2) ? lgfx::color888(0, 0, v)
                                          : lgfx::color888(v, v, v);
        lcd.fillRect(x0 + i, y0, 1, h, c);
    }
}
static void testGreyRamps() {
    const int32_t w = lcd.width();
    const int32_t h = lcd.height();
    const int32_t band = h / 4;
    announce(F("ramps"));
    lcd.fillScreen(C_BLACK);
    drawRamp(0, 0,        w, band, 0);
    drawRamp(0, band,     w, band, 1);
    drawRamp(0, band * 2, w, band, 2);
    drawRamp(0, band * 3, w, h - band * 3, 3);
    lcd.setFont(&fonts::FreeSansBold9pt7b);
    lcd.setTextColor(C_WHITE, C_BLACK);
    lcd.setTextDatum(middle_left);
    lcd.drawString("R", 4, band / 2);
    lcd.drawString("G", 4, band + band / 2);
    lcd.drawString("B", 4, band * 2 + band / 2);
    lcd.drawString("W", 4, band * 3 + (h - band * 3) / 2);
    lcd.setTextDatum(top_left);
    Serial.println(F("       bands top->bottom: red, green, blue, greyscale"));
    Serial.println(F("       each should fade smoothly left to right"));
    Serial.println(F("       banding or a hard jump = missing/shorted data bit"));
}
static void testTextAndGeometry() {
    const int32_t w = lcd.width();
    const int32_t h = lcd.height();
    const int32_t cx = w / 2, cy = h / 2;
    char buf[64];
    announce(F("text + geometry"));
    lcd.fillScreen(C_BLACK);
    lcd.drawRect(1, 1, w - 2, h - 2, C_WHITE);
    const int32_t m = 24;
    lcd.fillRect(3, 3, m, m, C_RED);
    lcd.fillRect(w - 3 - m, 3, m, m, C_GREEN);
    lcd.fillRect(3, h - 3 - m, m, m, C_BLUE);
    lcd.fillRect(w - 3 - m, h - 3 - m, m, m, C_YELLOW);
    lcd.fillRect(cx - 20, cy, 41, 1, C_WHITE);
    lcd.fillRect(cx, cy - 20, 1, 41, C_WHITE);
    lcd.fillCircle(cx, cy, 4, C_RED);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(C_WHITE, C_BLACK);
    lcd.setFont(&fonts::FreeSansBold18pt7b);
    lcd.drawString("HX8357D", cx, cy - 60);
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.drawString("320x480 SPI", cx, cy + 55);
    lcd.setFont(&fonts::Font2);
    snprintf(buf, sizeof(buf), "rot %u   %ldx%ld", (unsigned)lcd.getRotation(),
             (long)w, (long)h);
    lcd.drawString(buf, cx, cy + 90);
    snprintf(buf, sizeof(buf), "SPI %u MHz", (unsigned)(TFT_SPI_WRITE_HZ / 1000000UL));
    lcd.drawString(buf, cx, cy + 115);
    snprintf(buf, sizeof(buf), "panel %d  %s", (int)TFT_PANEL,
             invertOn ? "INV on" : "INV off");
    lcd.drawString(buf, cx, cy + 140);
    lcd.setTextDatum(top_left);
    Serial.println(F("       red TL, green TR, blue BL, yellow BR, crosshair centre"));
    Serial.println(F("       border must be flush on all four edges"));
}
static void handleSerial() {
    while (Serial.available()) {
        const int c = Serial.read();
        switch (c) {
        case 'f': case 'F': autoCycle = false; testFills();           break;
        case 'c': case 'C': autoCycle = false; testColorBars();       break;
        case 'g': case 'G': autoCycle = false; testGreyRamps();       break;
        case 't': case 'T': autoCycle = false; testTextAndGeometry(); break;
        case 's': case 'S': autoCycle = false; showSplash();          break;
        case 'b': case 'B': autoCycle = false; testBacklight();       break;
        case 'd': case 'D': printDiagnostics();                       break;
        case 'i': case 'I':
            invertOn = !invertOn;
            lcd.invertDisplay(invertOn);
            Serial.printf("[test] inversion %s\n", invertOn ? "ON" : "OFF");
            break;
        case 'r': case 'R': {
            uint8_t rot = (uint8_t)((lcd.getRotation() + 1) & 3);
            lcd.setRotation(rot);
            Serial.printf("[test] rotation %u -> %ldx%ld\n", (unsigned)rot,
                          (long)lcd.width(), (long)lcd.height());
            lcd.fillScreen(C_BLACK);
            break;
        }
        case 'a': case 'A':
            autoCycle = true;
            Serial.println(F("[test] auto-cycle resumed"));
            break;
        case 'p': case 'P':
            Serial.println(F("[test] rebooting to re-run the pre-flight"));
            Serial.flush();
            delay(100);
            ESP.restart();
            break;
        case '?': case 'h': case 'H': case '\n': case '\r':
            printMenu();
            break;
        default:
            if (c >= 32) Serial.printf("unknown key '%c'\n", c);
            break;
        }
    }
}
typedef void (*TestFn)();
static const TestFn kAutoTests[] = {
    testColorBars, testFills, testGreyRamps, testTextAndGeometry,
};
static const unsigned kAutoTestCount = sizeof(kAutoTests) / sizeof(kAutoTests[0]);
static unsigned autoTestIndex = 0;
void setup() {
    Serial.begin(115200);
    delay(1500);
    Serial.println();
    Serial.println(F("=========================================="));
    Serial.println(F(" HX8357D 320x480 screen test (ESP32-S3)"));
    Serial.println(F("=========================================="));
    preflight();
    if (!lcd.init_without_reset()) {
        Serial.println(F("ERROR: panel init failed - panel or bus did not come up"));
        return;
    }
    Serial.println(F("lcd.init() ok  (init_without_reset: preflight() did the reset)"));
    lcd.setBrightness(brightness);
    Serial.printf("backlight -> %u\n", (unsigned)brightness);
    bootFlash();
    testBacklight();
    showSplash();
    printDiagnostics();
    printMenu();
    Serial.println(F("If you can read the splash text and the four corner squares"));
    Serial.println(F("are in the right corners, the panel is up. Auto-cycle starts in 4s."));
    delay(4000);
}
void loop() {
    handleSerial();
    if (autoCycle) {
        static uint32_t lastRun = 0;
        if (millis() - lastRun > 4000) {
            lastRun = millis();
            kAutoTests[autoTestIndex]();
            autoTestIndex = (autoTestIndex + 1) % kAutoTestCount;
        }
    }
    delay(10);
}
