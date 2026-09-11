#ifndef SEQUENCES_H
#define SEQUENCES_H

// Frame ranges from assets/map.json. Each value 0-9 has a landing and a
// launching range; frame ids match the archive entries (1..450). Shuffle is
// not implemented.

struct Range {
    int start;
    int end;
};

// landing[value] and launching[value]
static const Range landing[10] = {
    {405, 420}, // 0
    {1, 15},    // 1
    {45, 60},   // 2
    {90, 105},  // 3
    {135, 150}, // 4
    {180, 195}, // 5
    {225, 240}, // 6
    {270, 285}, // 7
    {315, 330}, // 8
    {360, 375}, // 9
};

static const Range launching[10] = {
    {420, 450}, // 0
    {15, 45},   // 1
    {60, 90},   // 2
    {105, 135}, // 3
    {150, 180}, // 4
    {195, 225}, // 5
    {240, 270}, // 6
    {285, 315}, // 7
    {330, 360}, // 8
    {375, 405}, // 9
};

// The face frame is the last frame of a landing: the die at rest on `value`.
static inline int faceFrame(int value) {
    return landing[value].end;
}

#endif // SEQUENCES_H
