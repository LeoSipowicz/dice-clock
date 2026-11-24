#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <esp_display_panel.hpp>
#include <esp32-hal-psram.h>
#include <esp_io_expander.hpp>
#include <driver/i2c.h>
#include "lz4dec.h"
#include "sequences.h"
using namespace esp_panel::board;
using namespace esp_panel::drivers;
#define LOG_TAG "dice"
#define LOG_IMPL(esp_log_call, fmt, ...)     \
    do {                                     \
        Serial.printf(fmt "\n", ##__VA_ARGS__); \
        esp_log_call(LOG_TAG, fmt, ##__VA_ARGS__); \
    } while (0)
#define LOGI(fmt, ...) LOG_IMPL(ESP_LOGI, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) LOG_IMPL(ESP_LOGW, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_IMPL(ESP_LOGE, fmt, ##__VA_ARGS__)
#define SCREEN_W 800
#define SCREEN_H 480
#define FRAME_W  180
#define FRAME_H  231
#define COLON_W  79
#define FLIP_180 1
#define FPS       24
#define TIMESTEP  (1000.0f / FPS)
enum Phase { IDLE, LAUNCHING, LANDING };
#define BTN_I2C_ADDR 0x21
enum { BTN_SET, BTN_PLUS, BTN_MINUS, BTN_COUNT };
static const uint8_t btnPin[BTN_COUNT] = {1, 2, 5};
#define BTN_MASK ((1u << 1) | (1u << 2) | (1u << 5))
#define BTN_DEBOUNCE_MS     20
#define BTN_REPEAT_DELAY_MS 500
#define BTN_REPEAT_RATE_MS  120
uint16_t bgColor = 0x09E2;
uint16_t modeBoxColor = 0x0000;
struct Digit {
    int displayedValue;
    int targetValue;
    int frame;
    Range seq;
    Phase phase;
    int cachedFrame;
};
Digit digits[4];
Board *board = nullptr;
LCD   *lcd   = nullptr;
LGFX_Sprite digitSprite[4];
LGFX_Sprite colonSprite;
LGFX_Sprite fbSprite[2];
int frontIndex = 0;
int backIndex  = 1;
int frontFrame[4];
int backFrame[4];
bool panelReady   = false;
bool archiveReady = false;
const int digitX[4] = {0, 180, 439, 619};
const int digitY = (SCREEN_H - FRAME_H) / 2;
const int colonX = 360;
static inline int destX(int x, int w) {
#if FLIP_180
    return SCREEN_W - x - w;
#else
    (void)w;
    return x;
#endif
}
static inline int destY(int y, int h) {
#if FLIP_180
    return SCREEN_H - y - h;
#else
    (void)h;
    return y;
#endif
}
static void rotateSprite180(LGFX_Sprite &s) {
    uint16_t *p = (uint16_t *)s.getBuffer();
    size_t n = (size_t)s.width() * (size_t)s.height();
    for (size_t i = 0, j = n - 1; i < j; i++, j--) {
        uint16_t t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
}
void initDigit(Digit &d) {
    d.displayedValue = 0;
    d.targetValue = 0;
    d.frame = faceFrame(0);
    d.seq = landing[0];
    d.phase = IDLE;
    d.cachedFrame = -1;
}
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
enum Mode { MODE_RUN, MODE_HOURS, MODE_MINUTES };
Mode mode = MODE_RUN;
uint32_t minutesSinceNoon = 0;
uint32_t lastMinuteMs = 0;
uint8_t halfDay = 0;
bool rtcReady = false;
bool needFullRedraw = false;
void clockTick() {
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
esp_expander::HT8574 *btnExpander = nullptr;
struct ButtonState {
    bool raw = false;
    bool stable = false;
    bool armed = false;
    uint32_t rawMs = 0;
    uint32_t nextRepeatMs = 0;
    bool pressed = false;
    bool repeated = false;
};
ButtonState btn[BTN_COUNT];
uint8_t btnLevels = 0xFF;
uint8_t btnSeen = 0xFF;
uint8_t lastPinChange = 0xFF;
uint32_t lastPinLogMs = 0;
uint8_t btnAddr = 0;
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
#define RTC_I2C_ADDR   0x51
#define RTC_REG_CTRL1  0x00
#define RTC_REG_CTRL2  0x01
#define RTC_REG_SEC    0x04
#define RTC_REG_MIN    0x05
#define RTC_REG_HOUR   0x06
#define RTC_REG_DAY    0x07
#define RTC_REG_WDAY   0x08
#define RTC_REG_MONTH  0x09
#define RTC_REG_YEAR   0x0A
#define RTC_OS_BIT     0x80
#define RTC_CTRL1_CAP_SEL 0x01
static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
uint8_t rtcRaw[3];
static bool bcdValid(uint8_t v) {
    return (v & 0x0F) <= 9 && ((v >> 4) & 0x0F) <= 9;
}
static bool rtcRead(uint8_t &hour24, uint8_t &minute, uint8_t &second, bool &valid) {
    uint8_t b[3];
    if (!i2cReadRegs(RTC_I2C_ADDR, RTC_REG_SEC, b, 3)) return false;
    rtcRaw[0] = b[0]; rtcRaw[1] = b[1]; rtcRaw[2] = b[2];
    if ((uint8_t)(b[0] & b[1] & b[2]) == 0xFF) return false;
    uint8_t sv = b[0] & 0x7F;
    uint8_t mv = b[1] & 0x7F;
    uint8_t hv = b[2] & 0x3F;
    if (!bcdValid(sv) || !bcdValid(mv) || !bcdValid(hv)) return false;
    valid  = (b[0] & RTC_OS_BIT) == 0;
    second = bcd2dec(sv);
    minute = bcd2dec(mv);
    hour24 = bcd2dec(hv);
    return true;
}
static bool rtcTimeSane(uint8_t hour24, uint8_t minute, uint8_t second) {
    return hour24 < 24 && minute < 60 && second < 60;
}
static bool rtcReadStable(uint8_t &hour24, uint8_t &minute, uint8_t &second) {
    uint8_t prevH = 0, prevM = 0;
    bool have = false;
    for (int i = 0; i < 6; i++) {
        uint8_t h = 0, m = 0, s = 0;
        bool valid = false;
        if (!rtcRead(h, m, s, valid)) {
            delay(2);
            continue;
        }
        if (!valid || !rtcTimeSane(h, m, s)) return false;
        if (have && h == prevH && m == prevM) {
            hour24 = h;
            minute = m;
            second = s;
            return true;
        }
        prevH = h;
        prevM = m;
        have = true;
        delay(2);
    }
    return false;
}
static bool rtcWrite(uint8_t hour24, uint8_t minute) {
    uint8_t b[7] = {
        dec2bcd(0), dec2bcd(minute), dec2bcd(hour24),
        dec2bcd(1), 4, dec2bcd(1), dec2bcd(26),
    };
    return i2cWriteRegs(RTC_I2C_ADDR, RTC_REG_SEC, b, 7);
}
static bool clockSeedFromRtc() {
    if (!rtcReady) return false;
    uint8_t hour = 0, minute = 0, second = 0;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        ok = rtcReadStable(hour, minute, second);
        if (!ok) delay(5);
    }
    if (!ok) {
        LOGW("RTC time not usable (raw %02x %02x %02x) - starting at 12:00; "
             "save a time with the buttons and the chip will keep it",
             rtcRaw[0], rtcRaw[1], rtcRaw[2]);
        return false;
    }
    halfDay = (hour >= 12) ? 1 : 0;
    minutesSinceNoon = (uint32_t)(hour % 12) * 60 + minute;
    lastMinuteMs = millis() - (uint32_t)second * 1000UL;
    LOGI("clock seeded from RTC %02u:%02u:%02u (raw %02x %02x %02x)",
         (unsigned)hour, (unsigned)minute, (unsigned)second,
         rtcRaw[0], rtcRaw[1], rtcRaw[2]);
    return true;
}
static void clockSaveToRtc() {
    if (!rtcReady) return;
    uint32_t m = minutesSinceNoon % (12UL * 60UL);
    uint8_t h12 = (uint8_t)(m / 60);
    uint8_t hour24 = (h12 == 0) ? (halfDay ? 12 : 0)
                                : (uint8_t)(h12 + (halfDay ? 12 : 0));
    uint8_t minute = (uint8_t)(m % 60);
    if (rtcWrite(hour24, minute)) {
        LOGI("RTC set to %02u:%02u", (unsigned)hour24, (unsigned)minute);
    } else {
        LOGW("RTC write failed");
    }
}
static bool rtcBegin() {
    uint8_t ctrl1 = RTC_CTRL1_CAP_SEL;
    if (!i2cWriteRegs(RTC_I2C_ADDR, RTC_REG_CTRL1, &ctrl1, 1)) {
        LOGW("no RTC at 0x%02x - time restarts at 12:00 on every boot", RTC_I2C_ADDR);
        return false;
    }
    uint8_t ctrl2 = 0x00;
    i2cWriteRegs(RTC_I2C_ADDR, RTC_REG_CTRL2, &ctrl2, 1);
    rtcReady = true;
    uint8_t hour = 0, minute = 0, second = 0;
    bool valid = false;
    if (!rtcRead(hour, minute, second, valid)) {
        LOGW("RTC did not answer at 0x%02x (raw %02x %02x %02x)",
             RTC_I2C_ADDR, rtcRaw[0], rtcRaw[1], rtcRaw[2]);
    } else if (valid) {
        LOGI("RTC running, stored %02u:%02u:%02u (raw %02x %02x %02x)",
             (unsigned)hour, (unsigned)minute, (unsigned)second,
             rtcRaw[0], rtcRaw[1], rtcRaw[2]);
    } else {
        LOGW("RTC lost power since it was last set (OS bit in raw %02x %02x %02x) "
             "- it will read 12:00 until a time is saved", rtcRaw[0], rtcRaw[1], rtcRaw[2]);
    }
    return true;
}
static int findExpanderAddress() {
    static const uint8_t candidates[] = {
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    };
    for (uint8_t addr : candidates) {
        uint8_t hi = 0, lo = 0, hi2 = 0;
        if (!i2cWriteByte(addr, 0xFF) || !i2cReadByte(addr, hi)) continue;
        LOGI("probe 0x%02x: ACK, 0xFF -> 0x%02x", addr, hi);
        if (!i2cWriteByte(addr, 0x00) || !i2cReadByte(addr, lo)) continue;
        LOGI("probe 0x%02x: 0x00 -> 0x%02x", addr, lo);
        i2cWriteByte(addr, 0xFF);
        if (lo != 0x00 || hi == 0x00) continue;
        if (!i2cReadByte(addr, hi2) || hi2 == 0x00) continue;
        return (int)addr;
    }
    return -1;
}
static bool buttonsBegin() {
    int addr = findExpanderAddress();
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
    {
        uint8_t v = 0;
        i2cWriteByte(btnAddr, 0xFF);
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @400 kHz: 0x%02x", v);
        i2c_set_period(I2C_NUM_0, 400, 400);
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @100 kHz: 0x%02x", v);
        i2c_set_period(I2C_NUM_0, 800, 800);
        i2cReadByte(btnAddr, v);
        LOGI("pin readback @50 kHz:  0x%02x", v);
        i2c_set_period(I2C_NUM_0, 100, 100);
        i2cWriteByte(btnAddr, 0xFF);
    }
    btnExpander = new esp_expander::HT8574(I2C_NUM_0, btnAddr);
    if (btnExpander == nullptr) return false;
    btnExpander->configHostSkipInit(true);
    if (!btnExpander->init()) return false;
    if (!btnExpander->begin()) return false;
    for (int i = 0; i < BTN_COUNT; i++) {
        btnExpander->pinMode(btnPin[i], INPUT);
    }
    return btnExpander->multiDigitalRead(BTN_MASK) >= 0;
}
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
    int64_t levels = btnExpander->multiDigitalRead(0xFF);
    if (levels < 0) return;
    btnLevels = (uint8_t)(levels & 0xFF);
    btnSeen &= btnLevels;
    uint32_t now = millis();
    if (btnLevels != lastPinChange && now - lastPinLogMs > 150) {
        lastPinChange = btnLevels;
        lastPinLogMs = now;
        LOGI("pins 0x%02x", btnLevels);
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        bool pressed = ((levels >> btnPin[i]) & 1) == 0;
        if (pressed != btn[i].raw) {
            btn[i].raw = pressed;
            btn[i].rawMs = now;
        }
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
static void adjustTime(int delta) {
    uint32_t m = minutesSinceNoon % (12UL * 60UL);
    int hour = m / 60;
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
            mode = MODE_RUN;
            clockSaveToRtc();
        } else {
            mode = (mode == MODE_RUN) ? MODE_HOURS : MODE_MINUTES;
        }
        lastMinuteMs = millis();
        needFullRedraw = true;
        LOGI("button SET -> %s",
             mode == MODE_RUN ? "run" : mode == MODE_HOURS ? "hours" : "minutes");
        return;
    }
    adjustTime(idx == BTN_PLUS ? +1 : -1);
    if (!repeat) LOGI("button %s", idx == BTN_PLUS ? "PLUS" : "MINUS");
}
#define ARCHIVE_COLON_ID   0
#define ARCHIVE_MAX_FRAMES 512
struct FrameEntry {
    uint16_t id;
    uint16_t pad;
    uint32_t offset;
    uint32_t compSize;
    uint32_t rawSize;
};
extern "C" const uint8_t frames_archive_start[];
extern "C" const uint8_t frames_archive_end[];
uint32_t timDecodeUs = 0, timDecodeMaxUs = 0, timDecodeCount = 0;
uint32_t timLz4Us    = 0, timLz4MaxUs    = 0, timLz4Count   = 0;
uint32_t timBlitUs   = 0, timBlitMaxUs   = 0, timBlitCount = 0;
uint32_t timVsyncUs  = 0, timVsyncMaxUs  = 0, timVsyncCount = 0;
static inline void noteBlit(uint32_t dtUs) {
    timBlitUs += dtUs;
    if (dtUs > timBlitMaxUs) timBlitMaxUs = dtUs;
    timBlitCount++;
}
const FrameEntry *frameIndex = nullptr;
uint16_t frameCount = 0;
size_t   archiveSize = 0;
static bool archiveOpen() {
    const uint8_t *base = frames_archive_start;
    archiveSize = (size_t)(frames_archive_end - frames_archive_start);
    if (archiveSize < 16 || memcmp(base, "DLZ4", 4) != 0) {
        LOGE("linked archive is missing or has no DLZ4 magic (%lu B)",
             (unsigned long)archiveSize);
        return false;
    }
    uint16_t version = (uint16_t)(base[4] | (base[5] << 8));
    frameCount       = (uint16_t)(base[6] | (base[7] << 8));
    uint16_t width   = (uint16_t)(base[8] | (base[9] << 8));
    uint16_t height  = (uint16_t)(base[10] | (base[11] << 8));
    if (version != 1 || frameCount == 0 || frameCount > ARCHIVE_MAX_FRAMES ||
        width != FRAME_W || height != FRAME_H) {
        LOGE("archive v%u, %u frames of %ux%u but this layout needs %ux%u",
             version, frameCount, width, height,
             (unsigned)FRAME_W, (unsigned)FRAME_H);
        return false;
    }
    size_t indexBytes = (size_t)frameCount * sizeof(FrameEntry);
    if (16 + indexBytes > archiveSize) {
        LOGE("archive index (%lu B) does not fit in %lu B",
             (unsigned long)indexBytes, (unsigned long)archiveSize);
        return false;
    }
    frameIndex = (const FrameEntry *)(base + 16);
    LOGI("archive in flash: %lu B, %u frames of %ux%u",
         (unsigned long)archiveSize, frameCount, width, height);
    return true;
}
static int findFrame(uint16_t id) {
    if (id < frameCount && frameIndex[id].id == id) return (int)id;
    for (uint16_t i = 0; i < frameCount; i++) {
        if (frameIndex[i].id == id) return (int)i;
    }
    return -1;
}
static bool decodeFrameInto(LGFX_Sprite &sprite, uint16_t id) {
    int idx = findFrame(id);
    if (idx < 0) {
        LOGE("frame %u not in archive", id);
        return false;
    }
    const FrameEntry *e = &frameIndex[idx];
    size_t expected = (size_t)sprite.width() * sprite.height() * 2;
    if (e->rawSize != expected) {
        LOGE("frame %u size mismatch", id);
        return false;
    }
    if ((size_t)e->offset + e->compSize > archiveSize) {
        LOGE("frame %u extends past the archive", id);
        return false;
    }
    uint32_t t0 = micros();
    const uint8_t *src = frames_archive_start + e->offset;
    int out = lz4_decompress_block(src, (int)e->compSize,
                                   (uint8_t *)sprite.getBuffer(), (int)expected);
    uint32_t dt = micros() - t0;
    timLz4Us += dt;
    if (dt > timLz4MaxUs) timLz4MaxUs = dt;
    timLz4Count++;
    if ((size_t)out != expected) {
        LOGE("frame %u LZ4 -> %d, wanted %lu", id, out, (unsigned long)expected);
        return false;
    }
    return true;
}
void bakeDigit(int i) {
    int frame = digits[i].frame;
    uint32_t t0 = micros();
    decodeFrameInto(digitSprite[i], (uint16_t)frame);
#if FLIP_180
    rotateSprite180(digitSprite[i]);
#endif
    uint32_t dt = micros() - t0;
    timDecodeUs += dt;
    if (dt > timDecodeMaxUs) timDecodeMaxUs = dt;
    timDecodeCount++;
    digits[i].cachedFrame = frame;
}
void bakeColon() {
    uint32_t t0 = micros();
    decodeFrameInto(colonSprite, ARCHIVE_COLON_ID);
#if FLIP_180
    rotateSprite180(colonSprite);
#endif
    uint32_t dt = micros() - t0;
    timDecodeUs += dt;
    if (dt > timDecodeMaxUs) timDecodeMaxUs = dt;
    timDecodeCount++;
}
void drawModeOverlay(LGFX_Sprite &fb) {
    if (mode == MODE_RUN) return;
    bool hours = (mode == MODE_HOURS);
    int x0 = hours ? digitX[0] : digitX[2];
    int x1 = hours ? (digitX[1] + FRAME_W) : (digitX[3] + FRAME_W);
    int bw = (x1 - x0) - 6;
    int bh = FRAME_H + 16;
    int bx = destX(x0 + 3, bw);
    int by = destY(digitY - 8, bh);
    const int t = 4;
    fb.fillRect(bx, by, bw, t, modeBoxColor);
    fb.fillRect(bx, by + bh - t, bw, t, modeBoxColor);
    fb.fillRect(bx, by, t, bh, modeBoxColor);
    fb.fillRect(bx + bw - t, by, t, bh, modeBoxColor);
}
void compositeFullFrame(int fbIndex) {
    LGFX_Sprite &fb = fbSprite[fbIndex];
    uint32_t t = micros();
    fb.fillScreen(bgColor);
    noteBlit(micros() - t);
    t = micros();
    colonSprite.pushSprite(&fb, destX(colonX, COLON_W), destY(digitY, FRAME_H));
    noteBlit(micros() - t);
    for (int i = 0; i < 4; i++) {
        t = micros();
        digitSprite[i].pushSprite(&fb, destX(digitX[i], FRAME_W), destY(digitY, FRAME_H));
        noteBlit(micros() - t);
    }
    drawModeOverlay(fb);
}
volatile uint32_t vsyncCount = 0;
IRAM_ATTR bool onRefreshFinish(void *user_data) {
    vsyncCount++;
    return false;
}
void flip() {
    lcd->switchFrameBufferTo(fbSprite[backIndex].getBuffer());
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
uint32_t flipCount = 0;
uint32_t lastRenderMs = 0;
uint32_t maxRenderMs = 0;
void render() {
    uint32_t t0 = millis();
    if (needFullRedraw) {
        bakeColon();
        for (int i = 0; i < 4; i++) bakeDigit(i);
        needFullRedraw = false;
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
                digitSprite[i].pushSprite(&fbSprite[backIndex],
                                           destX(digitX[i], FRAME_W), destY(digitY, FRAME_H));
                noteBlit(micros() - t);
                backFrame[i] = digits[i].frame;
                anyChanged = true;
            }
        }
        if (!anyChanged) return;
        drawModeOverlay(fbSprite[backIndex]);
        flip();
        flipCount++;
        for (int i = 0; i < 4; i++) {
            int t = frontFrame[i];
            frontFrame[i] = backFrame[i];
            backFrame[i] = t;
        }
    }
    lastRenderMs = millis() - t0;
    if (lastRenderMs > maxRenderMs) maxRenderMs = lastRenderMs;
}
void setup() {
    Serial.begin(115200);
    delay(500);
    esp_log_level_set("*", ESP_LOG_INFO);
    LOGI("=== Dice Clock ===");
    board = new Board();
    if (!board->init()) {
        LOGE("board init failed");
        return;
    }
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
    rtcBegin();
    lcd->attachRefreshFinishCallback(onRefreshFinish);
    auto backlight = board->getBacklight();
    if (backlight != nullptr) backlight->on();
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
    LOGI("sprites created (%lu KB PSRAM free)", (unsigned long)(ESP.getFreePsram() / 1024));
    lastMinuteMs = millis();
    if (!archiveOpen()) {
        LOGE("archive open failed");
        fbSprite[backIndex].fillScreen(TFT_RED);
        flip();
        return;
    }
    archiveReady = true;
    if (!clockSeedFromRtc()) {
        lastMinuteMs = millis();
    }
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
    if (!panelReady || !archiveReady) {
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
    uint32_t now2 = millis();
    uint32_t interval = now2 - lastReportMs;
    if (interval >= 1000) {
        uint32_t decAvg  = timDecodeCount ? timDecodeUs / timDecodeCount : 0;
        uint32_t blitAvg = timBlitCount ? timBlitUs / timBlitCount : 0;
        uint32_t vsAvg   = timVsyncCount ? timVsyncUs / timVsyncCount : 0;
        uint32_t lz4Avg  = timLz4Count ? timLz4Us / timLz4Count : 0;
        LOGI("FPS %u flips/s, panel %u Hz, render last/max %lu/%lu ms | "
             "%lu decodes/s | pins now 0x%02x seen 0x%02x",
             (unsigned)(flipCount * 1000 / interval),
             (unsigned)((vsyncCount - lastVsyncCount) * 1000 / interval),
             (unsigned long)lastRenderMs, (unsigned long)maxRenderMs,
             (unsigned long)timDecodeCount,
             (unsigned)btnLevels, (unsigned)btnSeen);
        btnSeen = 0xFF;
        statsReset();
        printMs2("decode", decAvg, timDecodeMaxUs);
        printMs2("blit", blitAvg, timBlitMaxUs);
        printMs2("vsync", vsAvg, timVsyncMaxUs);
        printMs2("lz4", lz4Avg, timLz4MaxUs);
        LOGI("%s", statsBuf);
        flipCount = 0;
        maxRenderMs = 0;
        lastVsyncCount = vsyncCount;
        lastReportMs = now2;
        timDecodeUs = timDecodeMaxUs = 0; timDecodeCount = 0;
        timLz4Us = timLz4MaxUs = 0; timLz4Count = 0;
        timBlitUs = timBlitMaxUs = 0; timBlitCount = 0;
        timVsyncUs = timVsyncMaxUs = 0; timVsyncCount = 0;
    }
}
