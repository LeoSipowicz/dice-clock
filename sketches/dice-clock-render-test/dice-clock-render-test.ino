// Dice clock render test for the Waveshare ESP32-S3-Touch-LCD-4.3B (800x480).
//
// ESP32_Display_Panel drives the RGB panel with two framebuffers. Each frame is
// drawn into the back buffer, then switchFrameBufferTo() makes the DMA flip at
// the next VSYNC, so the panel never scans a half-updated frame. Animation
// frames come from /frames.lz4 (see scripts/build_frames.py): raw RGB565, one
// LZ4 block per frame, unpacked straight into offscreen LovyanGFX sprites.
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

#include "lz4dec.h"
#include "sequences.h"

using namespace esp_panel::board;
using namespace esp_panel::drivers;

// 4 x 180px digits + 79px colon = 799px, matching the frame layout.
#define SCREEN_W 800
#define SCREEN_H 480
#define FRAME_W  180
#define FRAME_H  231
#define COLON_W  79

#define FPS       24
#define TIMESTEP  (1000.0f / FPS)

// Fake clock: display one minute per this many milliseconds.
#define FAKE_MS_PER_MINUTE 3000

enum Phase { IDLE, LAUNCHING, LANDING };

// SD card is on SPI; its CS (EXIO4) is driven through the CH422G expander.
#define SD_PIN_MOSI 11
#define SD_PIN_SCK  12
#define SD_PIN_MISO 13
#define SD_DUMMY_CS 43
#define CH422G_SD_CS 4

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
        Serial.printf("SD.begin at %lu Hz ... ", (unsigned long)speeds[i]);
        if (SD.begin(SD_DUMMY_CS, SPI, speeds[i])) {
            Serial.println("ok");
            Serial.printf("card type: %d, size: %llu MB\n",
                          SD.cardType(), SD.cardSize() / (1024ULL * 1024ULL));
            return true;
        }
        Serial.println("fail");
        SD.end();
        delay(50);
    }
    return false;
}

// ---- state machine ----

void initDigit(Digit &d) {
    d.displayedValue = 0;
    d.targetValue = 0;
    d.frame = faceFrame(0);
    d.seq = landing[0];
    d.phase = IDLE;
    d.cachedFrame = -1;
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

// ---- fake clock ----

void getCurrentTimeDigits(int out[4]) {
    long totalMin = millis() / FAKE_MS_PER_MINUTE;
    int minutes = totalMin % 60;
    int hours = (totalMin / 60) % 12;
    if (hours == 0) hours = 12;

    out[0] = hours / 10;
    out[1] = hours % 10;
    out[2] = minutes / 10;
    out[3] = minutes % 10;
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
        Serial.printf("ERROR: cannot open %s\n", ARCHIVE_PATH);
        return false;
    }

    uint8_t hdr[16];
    if (archiveFile.read(hdr, sizeof(hdr)) != (int)sizeof(hdr) ||
        memcmp(hdr, "DLZ4", 4) != 0) {
        Serial.println("ERROR: bad archive header");
        return false;
    }
    uint16_t version = (uint16_t)(hdr[4] | (hdr[5] << 8));
    frameCount       = (uint16_t)(hdr[6] | (hdr[7] << 8));
    uint16_t width   = (uint16_t)(hdr[8] | (hdr[9] << 8));
    uint16_t height  = (uint16_t)(hdr[10] | (hdr[11] << 8));
    if (version != 1 || frameCount == 0 || frameCount > ARCHIVE_MAX_FRAMES ||
        width != FRAME_W || height != FRAME_H) {
        Serial.printf("ERROR: archive v%u, %u frames of %ux%u\n",
                      version, frameCount, width, height);
        return false;
    }

    size_t indexBytes = (size_t)frameCount * sizeof(FrameEntry);
    frameIndex = (FrameEntry *)psAlloc(indexBytes);
    if (frameIndex == nullptr ||
        archiveFile.read((uint8_t *)frameIndex, indexBytes) != (int)indexBytes) {
        Serial.println("ERROR: cannot read archive index");
        return false;
    }

    for (uint16_t i = 0; i < frameCount; i++) {
        if (frameIndex[i].compSize > compBufSize) compBufSize = frameIndex[i].compSize;
    }
    compBuf = (uint8_t *)psAlloc(compBufSize);
    if (compBuf == nullptr) {
        Serial.println("ERROR: cannot allocate decode buffer");
        return false;
    }

    Serial.printf("Archive ready: %u frames, max block %lu B\n",
                  frameCount, (unsigned long)compBufSize);
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
        Serial.printf("ERROR: frame %u not in archive\n", id);
        return false;
    }
    const FrameEntry *e = &frameIndex[idx];
    size_t expected = (size_t)sprite.width() * sprite.height() * 2;
    if (e->rawSize != expected || e->compSize > compBufSize) {
        Serial.printf("ERROR: frame %u size mismatch\n", id);
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
        Serial.printf("ERROR: frame %u short read\n", id);
        return false;
    }

    int out = lz4_decompress_block(compBuf, (int)e->compSize,
                                   (uint8_t *)sprite.getBuffer(), (int)expected);
    uint32_t t2 = micros();
    timLz4Us += t2 - t1;
    if ((t2 - t1) > timLz4MaxUs) timLz4MaxUs = t2 - t1;
    timLz4Count++;
    if ((size_t)out != expected) {
        Serial.printf("ERROR: frame %u LZ4 -> %d\n", id, out);
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

    uint32_t before = vsyncCount;
    uint32_t waitStartUs = micros();
    while (vsyncCount == before && micros() - waitStartUs < 40000) {
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
    Serial.println("\n\n=== Dice Clock render test ===");

    board = new Board();
    if (!board->init()) {
        Serial.println("ERROR: board init failed");
        return;
    }

    // Two framebuffers must be requested before the panel begins.
    lcd = board->getLCD();
    if (lcd == nullptr || !lcd->configFrameBufferNumber(2)) {
        Serial.println("ERROR: could not configure 2 frame buffers");
        return;
    }
    Serial.println("RGB panel configured with 2 frame buffers");

    if (!board->begin()) {
        Serial.println("ERROR: board begin failed");
        return;
    }

    lcd->attachRefreshFinishCallback(onRefreshFinish);

    auto backlight = board->getBacklight();
    if (backlight != nullptr) backlight->on();

    // Wrap the framebuffers in sprites (no extra memory). rgb565_nonswapped is
    // the byte order the ESP-IDF RGB panel expects; LovyanGFX's default
    // rgb565_2Byte is byte-swapped and shows wrong colors here.
    void *fb0 = lcd->getFrameBufferByIndex(0);
    void *fb1 = lcd->getFrameBufferByIndex(1);
    if (fb0 == nullptr || fb1 == nullptr) {
        Serial.println("ERROR: could not get frame buffers");
        return;
    }
    fbSprite[0].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    fbSprite[0].setBuffer(fb0, SCREEN_W, SCREEN_H);
    fbSprite[1].setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    fbSprite[1].setBuffer(fb1, SCREEN_W, SCREEN_H);
    panelReady = true;

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

    // Hold SD_CS (CH422G EXIO4) low and mount the card.
    auto expander = board->getIO_Expander()->getBase();
    expander->digitalWrite(CH422G_SD_CS, 1);
    delay(10);
    expander->digitalWrite(CH422G_SD_CS, 0);
    delay(100);

    sdReady = sdBegin();
    if (!sdReady) {
        Serial.println("ERROR: SD mount failed");
        fbSprite[backIndex].fillScreen(TFT_RED);
        flip();
        return;
    }
    Serial.println("SD mounted");

    if (!archiveOpen()) {
        fbSprite[backIndex].fillScreen(TFT_RED);
        flip();
        return;
    }

    for (int v = 0; v < 10; v++) {
        if (!decodeFrameInto(restSprite[v], (uint16_t)faceFrame(v))) {
            Serial.printf("resting face %d pre-decode failed\n", v);
        }
    }
    Serial.println("Resting faces ready");

    bakeColon();
    for (int i = 0; i < 4; i++) bakeDigit(i);
    compositeFullFrame(0);
    compositeFullFrame(1);
    for (int i = 0; i < 4; i++) frontFrame[i] = backFrame[i] = digits[i].frame;
    flip();
}

float acc = 0;
uint32_t lastMs = 0;
uint32_t lastReportMs = 0;
uint32_t lastVsyncCount = 0;

static void printMs2(const char *label, uint32_t avgUs, uint32_t maxUs) {
    Serial.printf("%s %lu.%02lu/%lu.%02lu", label,
                  (unsigned long)(avgUs / 1000), (unsigned long)((avgUs % 1000) / 10),
                  (unsigned long)(maxUs / 1000), (unsigned long)((maxUs % 1000) / 10));
}

void loop() {
    if (!panelReady || !sdReady) {
        delay(1000);
        return;
    }

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

        Serial.printf("FPS %u flips/s, panel %u Hz, render last/max %lu/%lu ms | "
                      "%lu decodes/s, %lu rest blits/s\n",
                      (unsigned)(flipCount * 1000 / interval),
                      (unsigned)((vsyncCount - lastVsyncCount) * 1000 / interval),
                      (unsigned long)lastRenderMs, (unsigned long)maxRenderMs,
                      (unsigned long)timDecodeCount, (unsigned long)timRestCount);
        Serial.print("   ");
        printMs2("decode", decAvg, timDecodeMaxUs);
        Serial.print("  ");
        printMs2("rest", restAvg, timRestMaxUs);
        Serial.print("  ");
        printMs2("blit", blitAvg, timBlitMaxUs);
        Serial.print("  ");
        printMs2("vsync", vsAvg, timVsyncMaxUs);
        Serial.print("  ");
        printMs2("sd/call", sdAvg, timSdMaxUs);
        Serial.print("  ");
        printMs2("lz4", lz4Avg, timLz4MaxUs);
        Serial.printf("  sd %lu KB/s in %lu calls\n",
                      (unsigned long)(timSdBytes / interval), (unsigned long)timSdCalls);

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
