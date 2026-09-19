// Dice clock for the Waveshare ESP32-S3-Touch-LCD-5 (800x480).
//
// Shows the time on four dice, starting at 12:00 and advancing one minute per
// minute. There is no RTC battery, so it restarts at 12:00 on power-up. Three
// buttons on a PCF8574 I2C expander set the time: SET cycles run -> set hours ->
// set minutes -> run, and PLUS/MINUS adjust the active field. Wiring is
// documented in README.md.
//
// Rendering: ESP32_Display_Panel drives the RGB panel with two framebuffers.
// Each frame is drawn into the back buffer, then switchFrameBufferTo() makes the
// DMA flip at the next VSYNC, so the panel never scans a half-updated frame.
// Animation frames come from /frames.lz4 (see scripts/build_frames.py): raw
// RGB565, one LZ4 block per frame, unpacked into offscreen LovyanGFX sprites.
//
// Do NOT include <Wire.h>. ESP32_Display_Panel / ESP32_IO_Expander use the
// legacy ESP-IDF I2C driver, which aborts at startup if the new driver is also
// linked. i2c_stubs.cpp keeps LovyanGFX's references to it out of the binary.

#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include <SD.h>

#include <LovyanGFX.hpp>
#include <esp_display_panel.hpp>
#include <esp32-hal-psram.h>
#include <esp_io_expander.hpp>
#include <driver/i2c.h> // legacy driver, shared with the panel; used to scan the bus

#include "lz4dec.h"
#include "sequences.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

// Diagnostics go to Serial *and* ESP_LOG.
//
// Important: with CONFIG_ARDUHAL_ESP_LOG set, the Arduino core redefines
// ESP_LOGI/LOGW/LOGE to its own log_i()/log_w()/log_e(), which are compile-time
// gated by ARDUHAL_LOG_LEVEL (= CORE_DEBUG_LEVEL, "None" by default). In this
// file that makes ESP_LOGx() a no-op, while the libraries -- which include
// <esp_log.h> directly -- keep the real IDF macros and log normally. Serial is
// not gated, so it is the channel that always works.
#define LOG_TAG "dice"

#define LOG_IMPL(esp_log_call, fmt, ...)     \
    do {                                     \
        Serial.printf(fmt "\n", ##__VA_ARGS__); \
        esp_log_call(LOG_TAG, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOGI(fmt, ...) LOG_IMPL(ESP_LOGI, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_IMPL(ESP_LOGW, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_IMPL(ESP_LOGE, fmt, ##__VA_ARGS__)

// 4 x 180px digits + 79px colon = 799px, matching the frame layout.
#define SCREEN_W 800
#define SCREEN_H 480
#define FRAME_W  180
#define FRAME_H  231
#define COLON_W  79

#define FPS       24
#define TIMESTEP  (1000.0f / FPS)

enum Phase { IDLE, LAUNCHING, LANDING };

// SD card is on SPI; its CS (EXIO4) is driven through the CH422G expander.
#define SD_PIN_MOSI 11
#define SD_PIN_SCK  12
#define SD_PIN_MISO 13
#define SD_DUMMY_CS 43
#define CH422G_SD_CS 4

// Buttons hang off a PCF8574 on the shared I2C bus (SDA=8, SCL=9, host 0).
// 0x21 keeps clear of the on-board CH422G at 0x20.
//
// This expander module only holds P1, P2 and P5 high: P0, P3, P4, P6 and P7 read
// low even with nothing connected, so switches on those pins never register. The
// three buttons therefore live on the three pins that work, which means no
// backlight button -- the display is simply left on.
#define BTN_I2C_ADDR 0x21

enum { BTN_SET, BTN_PLUS, BTN_MINUS, BTN_COUNT };
static const uint8_t btnPin[BTN_COUNT] = {1, 2, 5}; // P1=SET, P2=PLUS, P5=MINUS
#define BTN_MASK ((1u << 1) | (1u << 2) | (1u << 5))

#define BTN_DEBOUNCE_MS     20
#define BTN_REPEAT_DELAY_MS 500
#define BTN_REPEAT_RATE_MS  120

uint16_t bgColor = 0x0208; // dark green

struct Digit {
    int displayedValue;
    int targetValue;
    int frame;
    Range seq;
    Phase phase;
    int cachedFrame; // frame currently in the sprite, -1 = none
};

Digit digits[4];

Board *board = nullptr;
LCD   *lcd   = nullptr;

// Offscreen compositing sprites (PSRAM).
LGFX_Sprite digitSprite[4];
LGFX_Sprite colonSprite;
LGFX_Sprite restSprite[10]; // pre-decoded resting faces, so idle digits never read SD

// Return the die value (0-9) if `frame` is its resting face, else -1.
static int restingFaceIndex(int frame) {
    for (int v = 0; v < 10; v++) {
        if (frame == faceFrame(v)) return v;
    }
    return -1;
}

// fbSprite[front] is being scanned out by the DMA; draw into fbSprite[back].
LGFX_Sprite fbSprite[2];
int frontIndex = 0;
int backIndex  = 1;

// Frame held by each framebuffer. Partial updates compare against the back
// buffer's copy, so a digit that just finished animating is not reverted when
// another digit changes and the buffers swap.
int frontFrame[4];
int backFrame[4];

bool panelReady = false;
bool sdReady    = false;

const int digitX[4] = {0, 180, 439, 619};
const int digitY = (SCREEN_H - FRAME_H) / 2;
const int colonX = 360;

static inline bool sdBegin() {
    SPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_DUMMY_CS);

    const uint32_t speeds[] = {20000000, 10000000, 4000000, 1000000};
    for (uint32_t i = 0; i < sizeof(speeds) / sizeof(speeds[0]); i++) {
        LOGI("SD.begin at %lu Hz", (unsigned long)speeds[i]);
        if (SD.begin(SD_DUMMY_CS, SPI, speeds[i])) {
            LOGI("SD mounted: card type %d, %llu MB", SD.cardType(),
                 SD.cardSize() / (1024ULL * 1024ULL));
            return true;
        }
        LOGW("SD.begin at %lu Hz failed", (unsigned long)speeds[i]);
        SD.end();
        delay(50);
    }
    return false;
}

// ---- digit state machine ----

void initDigit(Digit &d) {
    d.displayedValue = 0;
    d.targetValue = 0;
    d.frame = faceFrame(0);
    d.seq = landing[0];
    d.phase = IDLE;
    d.cachedFrame = -1;
}

// Put a digit straight on a value with no launch/land animation. Used for the
// first paint, where there is no previous value worth rolling from.
void setDigitImmediate(Digit &d, int value) {
    d.displayedValue = value;
    d.targetValue = value;
    d.seq = landing[value];
    d.frame = faceFrame(value);
    d.phase = IDLE;
}

void setDigitTarget(Digit &d, int value) {
    if (d.displayedValue == value) return;
    if (d.targetValue == value && d.phase != IDLE) return;

    d.targetValue = value;
    d.seq = launching[d.displayedValue];
    d.frame = d.seq.start;
    d.phase = LAUNCHING;
}

void beginLanding(Digit &d, int targetVal) {
    d.targetValue = targetVal;
    d.seq = landing[targetVal];
    d.frame = d.seq.start;
    d.phase = LANDING;
}

void updateDigit(Digit &d, int realValue) {
    if (d.phase == IDLE) return;

    if (d.frame < d.seq.end) {
        d.frame++;
        return;
    }
    if (d.phase == LAUNCHING) {
        beginLanding(d, realValue);
        return;
    }
    if (d.phase == LANDING) {
        d.displayedValue = d.targetValue;
        d.frame = faceFrame(d.targetValue);
        d.phase = IDLE;
    }
}

// ---- clock ----
// minutesSinceNoon counts from 12:00 (0 == 12:00) and only ever spans 12 hours,
// because the display is a 12-hour clock. halfDay keeps the AM/PM half so the
// time can be written back to the RTC, but nothing on screen shows it.
// The board's RTC drives the time; without one it falls back to millis().

enum Mode { MODE_RUN, MODE_HOURS, MODE_MINUTES };

Mode mode = MODE_RUN;
uint32_t minutesSinceNoon = 0;
uint32_t lastMinuteMs = 0;
uint8_t halfDay = 0;      // 0 = AM, 1 = PM
bool rtcReady = false;    // set once the PCF85063 has answered

// Fallback ticker, used only when there is no RTC.
void clockTick() {
    if (rtcReady) return;
    uint32_t now = millis();
    while (now - lastMinuteMs >= 60000UL) {
        lastMinuteMs += 60000UL;
        if (mode == MODE_RUN) minutesSinceNoon++;
    }
}

void getCurrentTimeDigits(int out[4]) {
    uint32_t m = minutesSinceNoon % (12UL * 60UL);
    int hour = m / 60;
    if (hour == 0) hour = 12;
    int minute = m % 60;

    out[0] = hour / 10;
    out[1] = hour % 10;
    out[2] = minute / 10;
    out[3] = minute % 10;
}

// ---- buttons (PCF8574, active low) ----

esp_expander::HT8574 *btnExpander = nullptr;

struct ButtonState {
    bool raw = false;      // last sample, true = pressed
    bool stable = false;   // debounced state
    bool armed = false;    // must be seen released once before it can fire
    uint32_t rawMs = 0;
    uint32_t nextRepeatMs = 0;
    bool pressed = false;  // set for one poll on a debounced press
    bool repeated = false; // set for one poll on an auto-repeat
};
ButtonState btn[BTN_COUNT];

// Raw state of all eight expander pins, reported once a second. btnSeen is the
// AND of every sample since the last report, so a quick tap still shows up in
// that line rather than being missed between prints.
uint8_t btnLevels = 0xFF;
uint8_t btnSeen = 0xFF;

// For the per-change log line.
uint8_t lastPinChange = 0xFF;
uint32_t lastPinLogMs = 0;

// Address the PCF8574 actually answered on (0 = not found).
uint8_t btnAddr = 0;

// Raw single-byte transfers on the bus the board already brought up.
static bool i2cWriteByte(uint8_t addr, uint8_t value) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, value, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool i2cReadByte(uint8_t addr, uint8_t &value) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &value, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

// Register-addressed transfers, for the RTC. The PCF8574 has no register
// pointer, so it cannot use these.
static bool i2cWriteRegs(uint8_t addr, uint8_t reg, const uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write(cmd, (uint8_t *)buf, len, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

static bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err == ESP_OK;
}

// ---- PCF85063 RTC (on the board, address 0x51) ----
// Register-based, unlike the PCF8574: the byte after the address selects a
// register and the data follows. With the CR927 backup cell fitted it keeps time
// while the board is unpowered, so the clock resumes instead of restarting.
#define RTC_I2C_ADDR  0x51
#define RTC_REG_SEC   0x02
#define RTC_REG_MIN   0x03
#define RTC_REG_HOUR  0x04
#define RTC_REG_DAY   0x05
#define RTC_REG_MONTH 0x07
#define RTC_REG_YEAR  0x08
#define RTC_OS_BIT    0x80 // seconds bit 7: oscillator stopped, time not trustworthy

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

uint8_t rtcRaw[3]; // last raw seconds/minutes/hours bytes, logged on every change

static bool rtcRead(uint8_t &hour24, uint8_t &minute, uint8_t &second, bool &valid) {
    uint8_t b[3];
    if (!i2cReadRegs(RTC_I2C_ADDR, RTC_REG_SEC, b, 3)) return false;
    rtcRaw[0] = b[0]; rtcRaw[1] = b[1]; rtcRaw[2] = b[2];
    valid  = (b[0] & RTC_OS_BIT) == 0;
    second = bcd2dec(b[0] & 0x7F);
    minute = bcd2dec(b[1] & 0x7F);
    hour24 = bcd2dec(b[2] & 0x3F); // chip is in 24-hour mode
    return true;
}

// Writing the seconds register also clears the OS flag, so this sets the time
// and marks it valid at once. Seconds go to 0, putting the minute boundary here.
static bool rtcWrite(uint8_t hour24, uint8_t minute) {
    uint8_t b[3] = { dec2bcd(0), dec2bcd(minute), dec2bcd(hour24) };
    return i2cWriteRegs(RTC_I2C_ADDR, RTC_REG_SEC, b, 3);
}

// 24-hour RTC value -> the 12-hour value on screen, remembering the half.
static void clockSetFromRtc(uint8_t hour24, uint8_t minute) {
    halfDay = (hour24 >= 12) ? 1 : 0;
    minutesSinceNoon = (uint32_t)(hour24 % 12) * 60 + minute;
}

static void clockSaveToRtc() {
    if (!rtcReady) return;
    uint8_t h12 = (uint8_t)(minutesSinceNoon / 60); // 0..11, 0 == 12
    uint8_t hour24 = (h12 == 0) ? (halfDay ? 12 : 0)
                                : (uint8_t)(h12 + (halfDay ? 12 : 0));
    uint8_t minute = (uint8_t)(minutesSinceNoon % 60);
    if (rtcWrite(hour24, minute)) {
        LOGI("RTC set to %02u:%02u", (unsigned)hour24, (unsigned)minute);
    } else {
        LOGW("RTC write failed");
    }
}

// Called from loop(). While running the RTC is the time source, so this advances
// the display and corrects drift within 250 ms of a real minute.
static void clockSyncFromRtc() {
    static uint32_t lastMs = 0;
    if (!rtcReady || mode != MODE_RUN) return;
    uint32_t now = millis();
    if (now - lastMs < 250) return;
    lastMs = now;

    uint8_t hour = 0, minute = 0, second = 0;
    bool valid = false;
    if (!rtcRead(hour, minute, second, valid) || !valid) return;

    // Read again and require the two to agree. A single flipped byte -- a glitch
    // on the bus, or a read landing mid-update -- would otherwise jump the clock
    // and restart the dice animation over and over.
    uint8_t hour2 = 0, minute2 = 0, second2 = 0;
    bool valid2 = false;
    if (!rtcRead(hour2, minute2, second2, valid2) || !valid2) return;
    if (hour2 != hour || minute2 != minute) return;

    halfDay = (hour >= 12) ? 1 : 0;
    uint32_t m = (uint32_t)(hour % 12) * 60 + minute;
    if (m != minutesSinceNoon) {
        minutesSinceNoon = m;
        lastMinuteMs = millis();
        LOGI("RTC %02u:%02u:%02u raw %02x %02x %02x",
             (unsigned)hour, (unsigned)minute, (unsigned)second,
             rtcRaw[0], rtcRaw[1], rtcRaw[2]);
    }
}

static bool rtcBegin() {
    uint8_t hour = 0, minute = 0, second = 0;
    bool valid = false;
    if (!rtcRead(hour, minute, second, valid)) {
        LOGW("no RTC at 0x%02x - time restarts at 12:00 on every boot", RTC_I2C_ADDR);
        return false;
    }

    if (!valid) {
        // Never set, or the backup cell is flat. Give the calendar a real date
        // (2026-01-01) then start it at 12:00, which clears the OS flag.
        uint8_t date[4] = { dec2bcd(1), 4, dec2bcd(1), dec2bcd(26) };
        i2cWriteRegs(RTC_I2C_ADDR, RTC_REG_DAY, date, 4);
        rtcWrite(12, 0);
        rtcRead(hour, minute, second, valid);
        LOGI("RTC had stopped - started it at 12:00");
    }

    rtcReady = true;
    clockSetFromRtc(hour, minute);
    LOGI("RTC ready at 0x%02x, time %02u:%02u:%02u",
         RTC_I2C_ADDR, (unsigned)hour, (unsigned)minute, (unsigned)second);
    return true;
}

// Identify the PCF8574 by behaviour instead of by jumper guesswork: latch every
// pin low, then high, and require the reads to follow. P4-P7 are unconnected so
// a healthy read has them high. 0x20 is skipped on purpose -- that is the
// board's CH422G, and writing to it would disturb the panel and the SD card.
static int findExpanderAddress() {
    static const uint8_t candidates[] = {
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    };
    for (uint8_t addr : candidates) {
        uint8_t hi = 0, lo = 0, hi2 = 0;
        if (!i2cWriteByte(addr, 0xFF) || !i2cReadByte(addr, hi)) continue;
        // Log every address that answers, whatever it turns out to be.
        LOGI("probe 0x%02x: ACK, 0xFF -> 0x%02x", addr, hi);
        if (!i2cWriteByte(addr, 0x00) || !i2cReadByte(addr, lo)) continue;
        LOGI("probe 0x%02x: 0x00 -> 0x%02x", addr, lo);
        i2cWriteByte(addr, 0xFF);
        // A PCF8574 is the only thing here that drives every pin low on command
        // and lets at least one float high again. (P4-P7 may be loaded, so the
        // earlier "high nibble must be 0xF" test was too strict.)
        if (lo != 0x00 || hi == 0x00) continue;
        if (!i2cReadByte(addr, hi2) || hi2 == 0x00) continue;
        return (int)addr;
    }
    return -1;
}

// Attach to the bus the board already brought up; do not re-init the host.
static bool buttonsBegin() {
    int addr = findExpanderAddress();

    // A breadboard bus pulled up by only ~10 k in parallel with the ESP32's weak
    // internal pull-ups can be too slow to settle before the ACK bit is sampled
    // at 400 kHz. If nothing answers, retry slower. If it appears only when
    // slowed down, the cure is stronger pull-ups (or just run the bus slower).
    // APB is 80 MHz, so N APB cycles per half period = 80e6 / (2*N) Hz.
    if (addr < 0) {
        LOGW("nothing at 400 kHz, retrying at 100 kHz");
        LOGI("set_period -> %d", (int)i2c_set_period(I2C_NUM_0, 400, 400));
        addr = findExpanderAddress();
    }
    if (addr < 0) {
        LOGW("nothing at 100 kHz, retrying at 50 kHz");
        LOGI("set_period -> %d", (int)i2c_set_period(I2C_NUM_0, 800, 800));
        addr = findExpanderAddress();
    }
    if (addr < 0) {
        LOGW("no PCF8574 at any bus speed - address jumpers or wiring");
        return false;
    }

    btnAddr = (uint8_t)addr;
    LOGI("PCF8574 responds at 0x%02x", btnAddr);

    // Read the pins back at three bus speeds. If the value changes with speed,
    // the bus edges are marginal and the reads can't be trusted. If it is the
    // same at every speed, the chip really is reporting those pins low.
    // APB is 80 MHz, so N APB cycles per half period = 80e6 / (2*N) Hz.
    {
        uint8_t v = 0;
        i2cWriteByte(btnAddr, 0xFF);
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @400 kHz: 0x%02x", v);
        i2c_set_period(I2C_NUM_0, 400, 400); // 100 kHz
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @100 kHz: 0x%02x", v);
        i2c_set_period(I2C_NUM_0, 800, 800); // 50 kHz
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @50 kHz:  0x%02x", v);
        i2c_set_period(I2C_NUM_0, 100, 100); // back to 400 kHz
        i2cWriteByte(btnAddr, 0xFF);
    }

    btnExpander = new esp_expander::HT8574(I2C_NUM_0, btnAddr);
    if (btnExpander == nullptr) return false;
    btnExpander->configHostSkipInit(true);

    if (!btnExpander->init()) return false;
    if (!btnExpander->begin()) return false;

    // The driver latches 0xFF at begin(), so every pin is already an input with
    // its weak pull-up. Only the software direction needs setting -- writing a
    // level to an input pin is rejected by the driver.
    for (int i = 0; i < BTN_COUNT; i++) {
        btnExpander->pinMode(btnPin[i], INPUT);
    }

    return btnExpander->multiDigitalRead(BTN_MASK) >= 0;
}

// Report every device that ACKs on the shared bus. Send-only, so it is safe to
// run before we know what is out there.
static void i2cScan() {
    LOGI("I2C scan:");
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        if (i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20)) == ESP_OK) {
            LOGI("  found 0x%02x", addr);
        }
        i2c_cmd_link_delete(cmd);
    }
}

static void pollButtons() {
    if (btnExpander == nullptr) return;

    // Read all eight pins, not just the four the buttons use. Anything landing on
    // a different pin then shows up in the report instead of being invisible.
    int64_t levels = btnExpander->multiDigitalRead(0xFF);
    if (levels < 0) return; // I2C error: keep the previous state
    btnLevels = (uint8_t)(levels & 0xFF);
    btnSeen &= btnLevels;

    uint32_t now = millis();

    // Log every change of the raw byte, rate-limited. Whatever pin each switch is
    // really on shows up here, which is what identifies the wiring.
    if (btnLevels != lastPinChange && now - lastPinLogMs > 150) {
        lastPinChange = btnLevels;
        lastPinLogMs = now;
        LOGI("pins 0x%02x", btnLevels);
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        bool pressed = ((levels >> btnPin[i]) & 1) == 0; // active low
        if (pressed != btn[i].raw) {
            btn[i].raw = pressed;
            btn[i].rawMs = now;
        }
        // A pin that is already low at boot (a stuck switch, bad wiring, or a
        // held button) must not fire until it has been released once.
        if (!pressed) btn[i].armed = true;

        if (now - btn[i].rawMs < BTN_DEBOUNCE_MS) continue;

        if (pressed != btn[i].stable) {
            btn[i].stable = pressed;
            if (pressed) {
                if (!btn[i].armed) continue;
                btn[i].pressed = true;
                btn[i].nextRepeatMs = now + BTN_REPEAT_DELAY_MS;
            }
        } else if (pressed && btn[i].armed && now >= btn[i].nextRepeatMs) {
            btn[i].repeated = true;
            btn[i].nextRepeatMs = now + BTN_REPEAT_RATE_MS;
        }
    }
}

// Nudge the field currently being set. Also restarts the minute window so the
// clock does not immediately roll over after a manual change.
static void adjustTime(int delta) {
    uint32_t m = minutesSinceNoon % (12UL * 60UL);
    int hour = m / 60; // 0..11, 0 == 12
    int minute = m % 60;

    if (mode == MODE_HOURS) {
        hour = (hour + delta + 12) % 12;
    } else if (mode == MODE_MINUTES) {
        minute = (minute + delta + 60) % 60;
    } else {
        return;
    }

    minutesSinceNoon = (uint32_t)hour * 60 + minute;
    lastMinuteMs = millis();
}

static void onButton(int idx, bool repeat) {
    if (idx == BTN_SET) {
        if (repeat) return;
        if (mode == MODE_MINUTES) {
            mode = MODE_RUN;   // leaving setting mode: store the new time
            clockSaveToRtc();
        } else {
            mode = (mode == MODE_RUN) ? MODE_HOURS : MODE_MINUTES;
        }
        lastMinuteMs = millis();
        LOGI("button SET -> %s",
             mode == MODE_RUN ? "run" : mode == MODE_HOURS ? "hours" : "minutes");
        return;
    }

    adjustTime(idx == BTN_PLUS ? +1 : -1);
    if (!repeat) LOGI("button %s", idx == BTN_PLUS ? "PLUS" : "MINUS");
}

// ---- frame archive (/frames.lz4) ----
// build_frames.py packs every frame as one LZ4 block of raw little-endian
// RGB565, behind a 16-byte header and a 16-byte-per-entry index. Layout in
// scripts/build_frames.py.
#define ARCHIVE_PATH       "/frames.lz4"
#define ARCHIVE_COLON_ID   0
#define ARCHIVE_MAX_FRAMES 512

struct FrameEntry {
    uint16_t id;
    uint16_t pad;
    uint32_t offset;
    uint32_t compSize;
    uint32_t rawSize;
};

// ---- timing instrumentation ----
// Per-stage microseconds, accumulated between serial reports and reset when
// printed. sd/lz4 are subsets of decode; blit is one pushSprite/fillScreen into
// a panel framebuffer; vsync is the wait for a buffer swap to commit.
uint32_t timDecodeUs = 0, timDecodeMaxUs = 0, timDecodeCount = 0;
uint32_t timRestUs   = 0, timRestMaxUs   = 0, timRestCount   = 0;
uint32_t timSdUs     = 0, timSdMaxUs     = 0, timSdCalls = 0, timSdBytes = 0;
uint32_t timLz4Us    = 0, timLz4MaxUs    = 0, timLz4Count   = 0;
uint32_t timBlitUs   = 0, timBlitMaxUs   = 0, timBlitCount = 0;
uint32_t timVsyncUs  = 0, timVsyncMaxUs  = 0, timVsyncCount = 0;

static inline void noteBlit(uint32_t dtUs) {
    timBlitUs += dtUs;
    if (dtUs > timBlitMaxUs) timBlitMaxUs = dtUs;
    timBlitCount++;
}

// ---- archive state ----
FrameEntry *frameIndex = nullptr; // sorted so index == id
uint16_t frameCount = 0;
File archiveFile;
uint8_t *compBuf = nullptr;       // PSRAM scratch for one compressed block
uint32_t compBufSize = 0;

static void *psAlloc(size_t n) {
    void *p = ps_malloc(n);
    return p ? p : malloc(n);
}

// Open the archive and load its index. The file stays open; each frame is read
// by seeking within it.
static bool archiveOpen() {
    archiveFile = SD.open(ARCHIVE_PATH, FILE_READ);
    if (!archiveFile) {
        LOGE("cannot open %s", ARCHIVE_PATH);
        return false;
    }

    uint8_t hdr[16];
    if (archiveFile.read(hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        memcmp(hdr, "DLZ4", 4) != 0) {
        LOGE("bad archive header");
        return false;
    }
    uint16_t version = (uint16_t)(hdr[4] | (hdr[5] << 8));
    frameCount       = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t width   = (uint16_t)(hdr[8] | (hdr[9] << 8));
    uint16_t height  = (uint16_t)(hdr[10] | (hdr[11] << 8));
    if (version != 1 || frameCount == 0 || frameCount > ARCHIVE_MAX_FRAMES ||
        width != FRAME_W || height != FRAME_H) {
        LOGE("archive v%u, %u frames of %ux%u", version, frameCount, width, height);
        return false;
    }

    size_t indexBytes = (size_t)frameCount * sizeof(FrameEntry);
    frameIndex = (FrameEntry *)psAlloc(indexBytes);
    if (frameIndex == nullptr ||
        archiveFile.read((uint8_t *)frameIndex, indexBytes) != (int)indexBytes) {
        LOGE("cannot read archive index");
        return false;
    }

    for (uint16_t i = 0; i < frameCount; i++) {
        if (frameIndex[i].compSize > compBufSize) compBufSize = frameIndex[i].compSize;
    }
    compBuf = (uint8_t *)psAlloc(compBufSize);
    if (compBuf == nullptr) {
        LOGE("cannot allocate decode buffer");
        return false;
    }

    LOGI("archive ready: %u frames, max block %lu B", frameCount,
         (unsigned long)compBufSize);
    return true;
}

// ids are 0..frameCount-1 and sorted, so the id is the index. Returns an index
// rather than a FrameEntry* because the Arduino builder hoists prototypes above
// the struct definition.
static int findFrame(uint16_t id) {
    if (id < frameCount && frameIndex[id].id == id) return (int)id;
    for (uint16_t i = 0; i < frameCount; i++) {
        if (frameIndex[i].id == id) return (int)i;
    }
    return -1;
}

// Read and unpack one frame straight into a sprite's pixel buffer.
static bool decodeFrameInto(LGFX_Sprite &sprite, uint16_t id) {
    int idx = findFrame(id);
    if (idx < 0) {
        LOGE("frame %u not in archive", id);
        return false;
    }
    const FrameEntry *e = &frameIndex[idx];
    size_t expected = (size_t)sprite.width() * sprite.height() * 2;
    if (e->rawSize != expected || e->compSize > compBufSize) {
        LOGE("frame %u size mismatch", id);
        return false;
    }

    uint32_t t0 = micros();
    bool seeked = archiveFile.seek(e->offset);
    int got = seeked ? archiveFile.read(compBuf, e->compSize) : -1;
    uint32_t t1 = micros();
    timSdUs += t1 - t0;
    if ((t1 - t0) > timSdMaxUs) timSdMaxUs = t1 - t0;
    timSdCalls++;
    if (got > 0) timSdBytes += (uint32_t)got;
    if (!seeked || got != (int)e->compSize) {
        LOGE("frame %u short read", id);
        return false;
    }

    int out = lz4_decompress_block(compBuf, (int)e->compSize,
                                   (uint8_t *)sprite.getBuffer(), (int)expected);
    uint32_t t2 = micros();
    timLz4Us += t2 - t1;
    if ((t2 - t1) > timLz4MaxUs) timLz4MaxUs = t2 - t1;
    timLz4Count++;
    if ((size_t)out != expected) {
        LOGE("frame %u LZ4 -> %d", id, out);
        return false;
    }
    return true;
}

void bakeDigit(int i) {
    int frame = digits[i].frame;
    int rest = restingFaceIndex(frame);
    uint32_t t0 = micros();
    if (rest >= 0) {
        restSprite[rest].pushSprite(&digitSprite[i], 0, 0);
    } else {
        decodeFrameInto(digitSprite[i], (uint16_t)frame);
    }
    uint32_t dt = micros() - t0;
    if (rest >= 0) {
        timRestUs += dt;
        if (dt > timRestMaxUs) timRestMaxUs = dt;
        timRestCount++;
    } else {
        timDecodeUs += dt;
        if (dt > timDecodeMaxUs) timDecodeMaxUs = dt;
        timDecodeCount++;
    }
    digits[i].cachedFrame = frame;
}

void bakeColon() {
    uint32_t t0 = micros();
    decodeFrameInto(colonSprite, ARCHIVE_COLON_ID);
    uint32_t dt = micros() - t0;
    timDecodeUs += dt;
    if (dt > timDecodeMaxUs) timDecodeMaxUs = dt;
    timDecodeCount++;
}

// Composite background + colon + all digits into one panel framebuffer.
void compositeFullFrame(int fbIndex) {
    LGFX_Sprite &fb = fbSprite[fbIndex];
    uint32_t t = micros();
    fb.fillScreen(bgColor);
    noteBlit(micros() - t);
    t = micros();
    colonSprite.pushSprite(&fb, colonX, digitY);
    noteBlit(micros() - t);
    for (int i = 0; i < 4; i++) {
        t = micros();
        digitSprite[i].pushSprite(&fb, digitX[i], digitY);
        noteBlit(micros() - t);
    }
}

volatile uint32_t vsyncCount = 0; // incremented by the panel refresh ISR

IRAM_ATTR bool onRefreshFinish(void *user_data) {
    vsyncCount++;
    return false;
}

// Request the flip, then wait for the next VSYNC. Until it commits the DMA is
// still scanning what is about to become the back buffer, so drawing into it
// would tear. The panel runs ~39 Hz, so allow more than one frame.
void flip() {
    lcd->switchFrameBufferTo(fbSprite[backIndex].getBuffer());

    // This wait is the longest blocking stretch in the loop (~25-40 ms), long
    // enough for a short button press to begin and end unseen. Poll here too so
    // the buttons keep being sampled while the panel finishes its frame.
    uint32_t before = vsyncCount;
    uint32_t waitStartUs = micros();
    while (vsyncCount == before && micros() - waitStartUs < 40000) {
        pollButtons();
        delay(1);
    }
    uint32_t waitUs = micros() - waitStartUs;
    timVsyncUs += waitUs;
    if (waitUs > timVsyncMaxUs) timVsyncMaxUs = waitUs;
    timVsyncCount++;

    int tmp = frontIndex;
    frontIndex = backIndex;
    backIndex = tmp;
}

bool needFullRedraw = false; // set after a background color change

uint32_t flipCount = 0;
uint32_t lastRenderMs = 0;
uint32_t maxRenderMs = 0;

void render() {
    uint32_t t0 = millis();

    if (needFullRedraw) {
        bakeColon();
        for (int i = 0; i < 4; i++) bakeDigit(i);
        needFullRedraw = false;
        // Repaint both buffers so partial updates always start from a complete frame.
        compositeFullFrame(0);
        compositeFullFrame(1);
        for (int i = 0; i < 4; i++) frontFrame[i] = backFrame[i] = digits[i].frame;
        flip();
        flipCount++;
    } else {
        bool anyChanged = false;
        for (int i = 0; i < 4; i++) {
            if (digits[i].frame != digits[i].cachedFrame) bakeDigit(i);
            if (digits[i].frame != backFrame[i]) {
                uint32_t t = micros();
                digitSprite[i].pushSprite(&fbSprite[backIndex], digitX[i], digitY);
                noteBlit(micros() - t);
                backFrame[i] = digits[i].frame;
                anyChanged = true;
            }
        }
        if (!anyChanged) return; // idle: keep showing the current front buffer
        flip();
        flipCount++;
        // The buffers swapped; the new back buffer holds the old front's frames.
        for (int i = 0; i < 4; i++) {
            int t = frontFrame[i];
            frontFrame[i] = backFrame[i];
            backFrame[i] = t;
        }
    }

    lastRenderMs = millis() - t0;
    if (lastRenderMs > maxRenderMs) maxRenderMs = lastRenderMs;
}

// ---- main ----

void setup() {
    Serial.begin(115200);
    delay(500);

    // The core calls esp_log_level_set("*", CONFIG_LOG_DEFAULT_LEVEL) at boot,
    // which defaults to ERROR and would filter our INFO lines out at runtime.
    esp_log_level_set("*", ESP_LOG_INFO);

    LOGI("=== Dice Clock ===");

    board = new Board();
    if (!board->init()) {
        LOGE("board init failed");
        return;
    }

    // Two framebuffers must be requested before the panel begins.
    lcd = board->getLCD();
    if (lcd == nullptr || !lcd->configFrameBufferNumber(2)) {
        LOGE("could not configure 2 frame buffers");
        return;
    }
    LOGI("panel configured with 2 frame buffers");

    if (!board->begin()) {
        LOGE("board begin failed");
        return;
    }
    LOGI("board begun");

    // The board brings the I2C bus up during begin(), so the RTC is reachable
    // here. Done before the first frame so the display starts on the real time.
    rtcBegin();

    lcd->attachRefreshFinishCallback(onRefreshFinish);

    auto backlight = board->getBacklight();
    if (backlight != nullptr) backlight->on();

    // Wrap the framebuffers in sprites (no extra memory). rgb565_nonswapped is
    // the byte order the ESP-IDF RGB panel expects; LovyanGFX's default
    // rgb565_2Byte is byte-swapped and shows wrong colors here.
    void *fb0 = lcd->getFrameBufferByIndex(0);
    void *fb1 = lcd->getFrameBufferByIndex(1);
    if (fb0 == nullptr || fb1 == nullptr) {
        LOGE("could not get frame buffers");
        return;
    }
    fbSprite[0].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    fbSprite[0].setBuffer(fb0, SCREEN_W, SCREEN_H);
    fbSprite[1].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    fbSprite[1].setBuffer(fb1, SCREEN_W, SCREEN_H);
    panelReady = true;
    LOGI("framebuffers wrapped");

    fbSprite[0].fillScreen(TFT_BLACK);
    fbSprite[1].fillScreen(TFT_BLACK);
    flip();

    for (int i = 0; i < 4; i++) {
        digitSprite[i].setPsram(true);
        digitSprite[i].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
        digitSprite[i].createSprite(FRAME_W, FRAME_H);
        initDigit(digits[i]);
    }
    colonSprite.setPsram(true);
    colonSprite.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    colonSprite.createSprite(COLON_W, FRAME_H);
    for (int v = 0; v < 10; v++) {
        restSprite[v].setPsram(true);
        restSprite[v].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
        restSprite[v].createSprite(FRAME_W, FRAME_H);
    }
    LOGI("sprites created");

    lastMinuteMs = millis(); // start the minute window at boot

    // Hold SD_CS (CH422G EXIO4) low and mount the card.
    auto expander = board->getIO_Expander()->getBase();
    expander->digitalWrite(CH422G_SD_CS, 1);
    delay(10);
    expander->digitalWrite(CH422G_SD_CS, 0);
    delay(100);

    sdReady = sdBegin();
    if (!sdReady) {
        LOGE("SD mount failed");
        fbSprite[backIndex].fillScreen(TFT_RED);
        flip();
        return;
    }
    LOGI("SD mounted");

    if (!archiveOpen()) {
        LOGE("archive open failed");
        fbSprite[backIndex].fillScreen(TFT_RED);
        flip();
        return;
    }

    for (int v = 0; v < 10; v++) {
        if (!decodeFrameInto(restSprite[v], (uint16_t)faceFrame(v))) {
            LOGW("resting face %d pre-decode failed", v);
        }
    }
    LOGI("resting faces ready");

    // The RTC was already read during board init, so there is no "before" to
    // animate from: put the dice straight into position and paint that as the
    // first frame. The clock is correct the instant it lights up.
    int start[4];
    getCurrentTimeDigits(start);
    for (int i = 0; i < 4; i++) setDigitImmediate(digits[i], start[i]);

    bakeColon();
    for (int i = 0; i < 4; i++) bakeDigit(i);
    compositeFullFrame(0);
    compositeFullFrame(1);
    for (int i = 0; i < 4; i++) frontFrame[i] = backFrame[i] = digits[i].frame;
    flip();
    LOGI("first frame shown");

    // Buttons share the I2C bus the board brought up. Done last so a fault here
    // cannot stop the clock from starting.
    i2cScan();
    if (buttonsBegin()) {
        uint8_t idle = (uint8_t)(btnExpander->multiDigitalRead(0xFF) & 0xFF);
        LOGI("buttons ready (PCF8574 @ 0x%02x), all pins at rest 0x%02x",
             btnAddr, idle);
        LOGI("buttons: SET=P1 PLUS=P2 MINUS=P5");
        if ((idle & BTN_MASK) != BTN_MASK) {
            LOGW("a button pin reads LOW at rest - check its switch wiring");
        }
    } else {
        LOGW("buttons unavailable - see the scan above");
    }
    LOGI("setup complete");
}

float acc = 0;
uint32_t lastMs = 0;
uint32_t lastReportMs = 0;
uint32_t lastVsyncCount = 0;

// The stats line is assembled in a buffer and emitted in one piece, because the
// logger (unlike Serial) prefixes every call.
static char statsBuf[256];
static int statsOff = 0;

static void statsReset() {
    statsOff = 0;
    statsBuf[0] = '\0';
}

static void statsAppend(const char *fmt, ...) {
    if (statsOff >= (int)sizeof(statsBuf) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(statsBuf + statsOff, sizeof(statsBuf) - (size_t)statsOff, fmt, ap);
    va_end(ap);
    if (n > 0) statsOff += n;
    if (statsOff > (int)sizeof(statsBuf) - 1) statsOff = (int)sizeof(statsBuf) - 1;
}

static void printMs2(const char *label, uint32_t avgUs, uint32_t maxUs) {
    statsAppend("  %s %lu.%02lu/%lu.%02lu", label,
                (unsigned long)(avgUs / 1000), (unsigned long)((avgUs % 1000) / 10),
                (unsigned long)(maxUs / 1000), (unsigned long)((maxUs % 1000) / 10));
}

void loop() {
    if (!panelReady || !sdReady) {
        delay(1000);
        return;
    }

    pollButtons();
    for (int i = 0; i < BTN_COUNT; i++) {
        if (btn[i].pressed) onButton(i, false);
        else if (btn[i].repeated) onButton(i, true);
        btn[i].pressed = btn[i].repeated = false;
    }

    clockTick();
    clockSyncFromRtc();

    uint32_t now = millis();
    if (lastMs == 0) lastMs = now;
    acc += now - lastMs;
    lastMs = now;

    int real[4];
    int steps = 0;
    while (acc >= TIMESTEP && steps < 8) {
        getCurrentTimeDigits(real);
        for (int i = 0; i < 4; i++) setDigitTarget(digits[i], real[i]);
        for (int i = 0; i < 4; i++) updateDigit(digits[i], real[i]);
        acc -= TIMESTEP;
        steps++;
    }

    render();

    // Once per second, log the pipeline stage timings.
    uint32_t now2 = millis();
    uint32_t interval = now2 - lastReportMs;
    if (interval >= 1000) {
        uint32_t decAvg = timDecodeCount ? timDecodeUs / timDecodeCount : 0;
        uint32_t restAvg = timRestCount ? timRestUs / timRestCount : 0;
        uint32_t blitAvg = timBlitCount ? timBlitUs / timBlitCount : 0;
        uint32_t vsAvg = timVsyncCount ? timVsyncUs / timVsyncCount : 0;
        uint32_t sdAvg = timSdCalls ? timSdUs / timSdCalls : 0;
        uint32_t lz4Avg = timLz4Count ? timLz4Us / timLz4Count : 0;

        LOGI("FPS %u flips/s, panel %u Hz, render last/max %lu/%lu ms | "
             "%lu decodes/s, %lu rest blits/s | pins now 0x%02x seen 0x%02x",
             (unsigned)(flipCount * 1000 / interval),
             (unsigned)((vsyncCount - lastVsyncCount) * 1000 / interval),
             (unsigned long)lastRenderMs, (unsigned long)maxRenderMs,
             (unsigned long)timDecodeCount, (unsigned long)timRestCount,
             (unsigned)btnLevels, (unsigned)btnSeen);
        btnSeen = 0xFF;

        statsReset();
        printMs2("decode", decAvg, timDecodeMaxUs);
        printMs2("rest", restAvg, timRestMaxUs);
        printMs2("blit", blitAvg, timBlitMaxUs);
        printMs2("vsync", vsAvg, timVsyncMaxUs);
        printMs2("sd/call", sdAvg, timSdMaxUs);
        printMs2("lz4", lz4Avg, timLz4MaxUs);
        statsAppend("  sd %lu KB/s in %lu calls",
                    (unsigned long)(timSdBytes / interval), (unsigned long)timSdCalls);
        LOGI("%s", statsBuf);

        flipCount = 0;
        maxRenderMs = 0;
        lastVsyncCount = vsyncCount;
        lastReportMs = now2;

        timDecodeUs = timDecodeMaxUs = 0; timDecodeCount = 0;
        timRestUs = timRestMaxUs = 0; timRestCount = 0;
        timSdUs = timSdMaxUs = 0; timSdCalls = 0; timSdBytes = 0;
        timLz4Us = timLz4MaxUs = 0; timLz4Count = 0;
        timBlitUs = timBlitMaxUs = 0; timBlitCount = 0;
        timVsyncUs = timVsyncMaxUs = 0; timVsyncCount = 0;
    }
}
