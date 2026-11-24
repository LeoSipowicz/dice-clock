#ifndef SEQUENCES_H
#define SEQUENCES_H
struct Range {
    int start;
    int end;
};
static const Range landing[10] = {
    {405, 420},
    {1, 15},
    {45, 60},
    {90, 105},
    {135, 150},
    {180, 195},
    {225, 240},
    {270, 285},
    {315, 330},
    {360, 375},
};
static const Range launching[10] = {
    {420, 450},
    {15, 45},
    {60, 90},
    {105, 135},
    {150, 180},
    {195, 225},
    {240, 270},
    {285, 315},
    {330, 360},
    {375, 405},
};
static inline int faceFrame(int value) {
    return landing[value].end;
}
#endif
