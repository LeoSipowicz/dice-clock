#include <Arduino.h>
#include <SPI.h>
#include <esp32-hal-psram.h>
#include "lgfx_config.h"
#include "lz4dec.h"
#include "sequences.h"
#define LOGI(fmt, ...) Serial.printf("dice: " fmt "\n", ##__VA_ARGS__)
#define LOGW(fmt, ...) Serial.printf("dice: " fmt "\n", ##__VA_ARGS__)
#define LOGE(fmt, ...) Serial.printf("dice: " fmt "\n", ##__VA_ARGS__)
LGFX lcd;
#define SCREEN_W   480
#define SCREEN_H   320
#define FRAME_W    106
#define FRAME_H    136
#define COLON_W     20
#define COLON_H     44
#define GAP          6
#define ROW_W   ((4 * FRAME_W) + COLON_W + (4 * GAP))
#define ROW_X   ((SCREEN_W - ROW_W) / 2)
#define ROW_Y   ((SCREEN_H - FRAME_H) / 2)
static_assert(ROW_W <= SCREEN_W, "the dice row does not fit the screen width");
static_assert(ROW_Y + FRAME_H <= SCREEN_H, "the dice row does not fit the screen height");
static const int digitX[4] = {
    ROW_X,
    ROW_X + (FRAME_W + GAP),
    ROW_X + 2 * (FRAME_W + GAP) + COLON_W + GAP,
    ROW_X + 3 * (FRAME_W + GAP) + COLON_W + GAP,
};
static const int digitY[4] = { ROW_Y, ROW_Y, ROW_Y, ROW_Y };
static const int colonX = ROW_X + 2 * (FRAME_W + GAP);
static const int colonY = ROW_Y + (FRAME_H - COLON_H) / 2;
static const uint16_t BG_COLOR = 0x0201;
#define FPS       24
#define TIMESTEP  (1000.0f / FPS)
enum Phase { IDLE, LAUNCHING, LANDING };
struct Digit {
    int displayedValue;
    int targetValue;
    int frame;
    Range seq;
    Phase phase;
    int cachedFrame;
};
Digit digits[4];
LGFX_Sprite digitSprite[4];
LGFX_Sprite colonSprite;
LGFX_Sprite restSprite[10];
int pushedFrame[4];
static int restingFaceIndex(int frame) {
    for (int v = 0; v < 10; v++) {
        if (frame == faceFrame(v)) return v;
    }
    return -1;
}
bool ready = false;
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
uint32_t minutesSinceNoon = 0;
uint32_t lastMinuteMs = 0;
void clockTick() {
    uint32_t now = millis();
    while (now - lastMinuteMs >= 60000UL) {
        lastMinuteMs += 60000UL;
        minutesSinceNoon++;
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
extern "C" const uint8_t frames_archive_start[];
extern "C" const uint8_t frames_archive_end[];
#define ARCHIVE_LABEL      "frames_320x480.lz4 (linked into flash)"
#define ARCHIVE_COLON_ID   0
#define ARCHIVE_MAX_FRAMES 512
struct FrameEntry {
    uint16_t id;
    uint16_t pad;
    uint32_t offset;
    uint32_t compSize;
    uint32_t rawSize;
};
const FrameEntry *frameIndex = nullptr;
uint16_t frameCount = 0;
size_t   archiveSize = 0;
uint32_t timDecodeUs = 0, timDecodeMaxUs = 0, timDecodeCount = 0;
uint32_t timRestUs   = 0, timRestMaxUs   = 0, timRestCount   = 0;
uint32_t timLz4Us    = 0, timLz4MaxUs    = 0, timLz4Count    = 0;
uint32_t timPushUs   = 0, timPushMaxUs   = 0, timPushCount   = 0;
uint32_t pushCount   = 0;
uint32_t lastRenderMs = 0, maxRenderMs = 0;
static inline void notePush(uint32_t dtUs) {
    timPushUs += dtUs;
    if (dtUs > timPushMaxUs) timPushMaxUs = dtUs;
    timPushCount++;
}
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
    if (version != 1 || frameCount == 0 || frameCount > ARCHIVE_MAX_FRAMES) {
        LOGE("archive v%u with %u frames looks wrong", version, frameCount);
        return false;
    }
    if (width != FRAME_W || height != FRAME_H) {
        LOGE("archive is %ux%u but this layout needs %ux%u", width, height,
             (unsigned)FRAME_W, (unsigned)FRAME_H);
        LOGE("rebuild with: python3 scripts/build_frames.py --target-w %u --colon-tight "
             "--bg 00410A --out sketches/hx8357d-dice-clock/%s",
             (unsigned)FRAME_W, ARCHIVE_LABEL);
        return false;
    }
    const size_t indexBytes = (size_t)frameCount * sizeof(FrameEntry);
    if (16 + indexBytes > archiveSize) {
        LOGE("archive index (%lu B) does not fit in %lu B", (unsigned long)indexBytes,
             (unsigned long)archiveSize);
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
    const size_t expected = (size_t)sprite.width() * sprite.height() * 2;
    if (e->rawSize != expected) {
        LOGE("frame %u is %lu B, sprite wants %lu B", id,
             (unsigned long)e->rawSize, (unsigned long)expected);
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
static bool createSprites() {
    for (int i = 0; i < 4; i++) {
        digitSprite[i].setPsram(true);
        digitSprite[i].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
        if (!digitSprite[i].createSprite(FRAME_W, FRAME_H)) {
            LOGE("digit sprite %d alloc failed", i);
            return false;
        }
    }
    colonSprite.setPsram(true);
    colonSprite.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    if (!colonSprite.createSprite(COLON_W, COLON_H)) {
        LOGE("colon sprite alloc failed");
        return false;
    }
    for (int v = 0; v < 10; v++) {
        restSprite[v].setPsram(true);
        restSprite[v].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
        if (!restSprite[v].createSprite(FRAME_W, FRAME_H)) {
            LOGE("resting-face sprite %d alloc failed", v);
            return false;
        }
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
static void paintAll() {
    uint32_t t = micros();
    lcd.fillScreen(BG_COLOR);
    notePush(micros() - t);
    t = micros();
    colonSprite.pushSprite(&lcd, colonX, colonY);
    notePush(micros() - t);
    for (int i = 0; i < 4; i++) {
        t = micros();
        digitSprite[i].pushSprite(&lcd, digitX[i], digitY[i]);
        notePush(micros() - t);
        pushedFrame[i] = digits[i].frame;
    }
    pushCount++;
}
static void render() {
    uint32_t t0 = millis();
    bool anyChanged = false;
    for (int i = 0; i < 4; i++) {
        if (digits[i].frame != digits[i].cachedFrame) bakeDigit(i);
        if (digits[i].frame != pushedFrame[i]) {
            uint32_t t = micros();
            digitSprite[i].pushSprite(&lcd, digitX[i], digitY[i]);
            notePush(micros() - t);
            pushedFrame[i] = digits[i].frame;
            anyChanged = true;
        }
    }
    if (anyChanged) pushCount++;
    lastRenderMs = millis() - t0;
    if (lastRenderMs > maxRenderMs) maxRenderMs = lastRenderMs;
}
static void showMessage(const char *line1, const char *line2) {
    lcd.fillScreen(BG_COLOR);
    lcd.setTextDatum(middle_center);
    lcd.setTextColor(TFT_WHITE, BG_COLOR);
    lcd.setFont(&fonts::FreeSansBold12pt7b);
    lcd.drawString(line1, SCREEN_W / 2, 150);
    if (line2 != nullptr) {
        lcd.setFont(&fonts::Font2);
        lcd.drawString(line2, SCREEN_W / 2, 210);
    }
    lcd.setTextDatum(top_left);
}
static void hardResetPanel() {
    pinMode(TFT_PIN_RST, OUTPUT);
    digitalWrite(TFT_PIN_RST, LOW);
    delay(20);
    digitalWrite(TFT_PIN_RST, HIGH);
    delay(150);
}
void setup() {
    Serial.begin(115200);
    delay(800);
    LOGI("=== Dice clock (HX8357D 320x480) ===");
    hardResetPanel();
    if (!lcd.init_without_reset()) {
        LOGE("panel init failed - panel or bus did not come up");
        return;
    }
    lcd.setRotation(TFT_ROTATION);
    if (lcd.width() != SCREEN_W || lcd.height() != SCREEN_H) {
        LOGE("after rotation %d the panel is %ldx%ld but this layout needs %dx%d",
             TFT_ROTATION, (long)lcd.width(), (long)lcd.height(),
             (unsigned)SCREEN_W, (unsigned)SCREEN_H);
        showMessage("Wrong rotation", "see TFT_ROTATION in lgfx_config.h");
        return;
    }
    lcd.setBrightness(255);
    LOGI("panel up: %ldx%ld (rotation %d), PSRAM %lu KB free", (long)lcd.width(),
         (long)lcd.height(), TFT_ROTATION, (unsigned long)(ESP.getFreePsram() / 1024));
    if (!archiveOpen()) {
        showMessage("Bad frame archive", "rebuild frames_320x480.lz4");
        return;
    }
    if (!createSprites()) {
        showMessage("Out of PSRAM", "set Tools -> PSRAM to OPI PSRAM");
        return;
    }
    LOGI("sprites created");
    for (int v = 0; v < 10; v++) {
        if (!decodeFrameInto(restSprite[v], (uint16_t)faceFrame(v))) {
            LOGW("resting face %d pre-decode failed", v);
        }
    }
    LOGI("resting faces ready");
    lastMinuteMs = millis();
    int start[4];
    getCurrentTimeDigits(start);
    for (int i = 0; i < 4; i++) setDigitImmediate(digits[i], start[i]);
    bakeColon();
    for (int i = 0; i < 4; i++) bakeDigit(i);
    paintAll();
    LOGI("first frame shown, time %d%d:%d%d", start[0], start[1], start[2], start[3]);
    ready = true;
    LOGI("setup complete");
}
float acc = 0;
uint32_t lastLoopMs = 0;
uint32_t lastReportMs = 0;
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
    if (!ready) {
        delay(1000);
        return;
    }
    clockTick();
    uint32_t now = millis();
    if (lastLoopMs == 0) lastLoopMs = now;
    acc += now - lastLoopMs;
    lastLoopMs = now;
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
        uint32_t restAvg = timRestCount   ? timRestUs   / timRestCount   : 0;
        uint32_t pushAvg = timPushCount   ? timPushUs   / timPushCount   : 0;
        uint32_t lz4Avg  = timLz4Count    ? timLz4Us    / timLz4Count    : 0;
        LOGI("pushes %u/s, render last/max %lu/%lu ms | %lu decodes/s, "
             "%lu rest blits/s",
             (unsigned)(pushCount * 1000 / interval),
             (unsigned long)lastRenderMs, (unsigned long)maxRenderMs,
             (unsigned long)timDecodeCount, (unsigned long)timRestCount);
        statsReset();
        printMs2("lz4", lz4Avg, timLz4MaxUs);
        printMs2("decode", decAvg, timDecodeMaxUs);
        printMs2("rest", restAvg, timRestMaxUs);
        printMs2("push", pushAvg, timPushMaxUs);
        LOGI("%s", statsBuf);
        pushCount = 0;
        maxRenderMs = 0;
        lastReportMs = now2;
        timDecodeUs = timDecodeMaxUs = 0; timDecodeCount = 0;
        timRestUs = timRestMaxUs = 0; timRestCount = 0;
        timLz4Us = timLz4MaxUs = 0; timLz4Count = 0;
        timPushUs = timPushMaxUs = 0; timPushCount = 0;
    }
}
