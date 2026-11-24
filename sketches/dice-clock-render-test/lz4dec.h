#ifndef LZ4DEC_H
#define LZ4DEC_H
#include <stdint.h>
#include <string.h>
static int lz4_decompress_block(const uint8_t *src, int srcSize,
                                uint8_t *dst, int dstCapacity) {
    const uint8_t *ip = src;
    const uint8_t *iend = src + srcSize;
    uint8_t *op = dst;
    uint8_t *oend = dst + dstCapacity;
    while (ip < iend) {
        unsigned token = *ip++;
        unsigned litLen = token >> 4;
        if (litLen == 15) {
            unsigned s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                litLen += s;
            } while (s == 255);
        }
        if ((size_t)(iend - ip) < litLen) return -1;
        if ((size_t)(oend - op) < litLen) return -1;
        memcpy(op, ip, litLen);
        ip += litLen;
        op += litLen;
        if (ip == iend) break;
        if ((size_t)(iend - ip) < 2) return -1;
        unsigned offset = (unsigned)ip[0] | ((unsigned)ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (unsigned)(op - dst)) return -1;
        unsigned matchLen = token & 15;
        if (matchLen == 15) {
            unsigned s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                matchLen += s;
            } while (s == 255);
        }
        matchLen += 4;
        if ((size_t)(oend - op) < matchLen) return -1;
        const uint8_t *match = op - offset;
        if (offset >= matchLen) {
            memcpy(op, match, matchLen);
        } else {
            memcpy(op, match, offset);
            unsigned written = offset;
            while (written < matchLen) {
                unsigned chunk = (written < matchLen - written)
                                     ? written : (matchLen - written);
                memcpy(op + written, op, chunk);
                written += chunk;
            }
        }
        op += matchLen;
    }
    return (int)(op - dst);
}
#endif
