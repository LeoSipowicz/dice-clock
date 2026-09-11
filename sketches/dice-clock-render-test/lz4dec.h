#ifndef LZ4DEC_H
#define LZ4DEC_H

#include <stdint.h>
#include <string.h>

// Minimal decoder for a single LZ4 block (raw block format, no frame header):
// the payload left after stripping the 8-byte header from an `lz4 -l` stream.
// Returns bytes written to dst, or -1 if the block is malformed or too large.
static int lz4_decompress_block(const uint8_t *src, int srcSize,
                                uint8_t *dst, int dstCapacity) {
    const uint8_t *ip = src;
    const uint8_t *iend = src + srcSize;
    uint8_t *op = dst;
    uint8_t *oend = dst + dstCapacity;

    while (ip < iend) {
        unsigned token = *ip++;

        // Literal run.
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

        if (ip == iend) break;  // the final sequence is literals only

        // Back-reference (match) copy.
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
        matchLen += 4;  // MINMATCH
        if ((size_t)(oend - op) < matchLen) return -1;

        // Overlapping matches (offset < matchLen) are common here -- the flat
        // background is one huge match with a 2-byte offset -- so copy the
        // period once, then double the written run with non-overlapping memcpys.
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

#endif  // LZ4DEC_H
