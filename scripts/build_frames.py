#!/usr/bin/env python3
"""Prepare dice frames for the ESP32 SD card.

Crop each source frame to the die motion envelope, scale it down, and bake in
the background color. Each frame is stored as raw RGB565 (no alpha) compressed
with LZ4-HC: smaller than PNG on this content (~11 KB vs ~21 KB) and far cheaper
to decode, with no inflate and no per-pixel conversion. Frames 1-450 cover
landing_0-9 and launching_0-9; shuffle (451-510) is ignored.

All frames are packed into one archive so the device opens a single file and
seeks within it instead of doing 450 FAT lookups.

Output: sd_card/frames.lz4  (copy it to the SD card root)

Archive layout, all little-endian:
    char    magic[4]    "DLZ4"
    uint16  version     1
    uint16  count       number of entries
    uint16  width       digit frame width  (180)
    uint16  height      digit frame height (231)
    uint32  reserved    0
    Entry   entries[count]        sorted by id
    uint8   payload[]             concatenated LZ4 blocks

    Entry (16 bytes):
        uint16 id       0 = colon, 1..450 = die frames
        uint16 pad      0
        uint32 offset   absolute file offset of this frame's LZ4 block
        uint32 compSize compressed size in bytes
        uint32 rawSize  decompressed size in bytes (width*height*2)

If you change bgColor in dice-clock-render-test.ino, update BG below to the
matching RGB888 and re-run.
"""

import os
import struct
import subprocess

import numpy as np
from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), "..", "images")
OUT = os.path.join(os.path.dirname(__file__), "..", "sd_card", "frames.lz4")

# Die motion envelope for frames 1-450, measured from the alpha channel.
BBOX = (39, 239, 381, 678)  # left, top, right, bottom -> 342 x 439
# 180x231 fills the 800px screen width (4*180 + 79 colon).
TARGET_W = 180
SCALE = TARGET_W / (BBOX[2] - BBOX[0])
TARGET_H = round((BBOX[3] - BBOX[1]) * SCALE)

# Background baked into every frame: RGB888 for bgColor 0x0208 (dark green) in
# dice-clock-render-test.ino. Keep the two in sync.
BG = (0x00, 0x41, 0x42)

FIRST = 1
LAST = 450
COLON_ID = 0

# LZ4 compression level for the `lz4` CLI (>= 10 selects high-compression mode).
LZ4_LEVEL = 12

LZ4_LEGACY_MAGIC = b"\x02\x21\x4c\x18"


def bake_background(im):
    """Composite an RGBA image over BG and return an RGB image."""
    rgb = Image.new("RGB", im.size, BG)
    rgb.paste(im, mask=im.getchannel("A"))
    return rgb


def bake_digit(i):
    src = os.path.join(SRC, f"{i:04d}.png")
    im = Image.open(src).convert("RGBA").crop(BBOX)
    im = im.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    return bake_background(im)


def bake_colon():
    src = os.path.join(SRC, "colon.png")
    im = Image.open(src).convert("RGBA")
    w = im.width
    im = im.crop((0, BBOX[1], w, BBOX[3]))
    im = im.resize((round(w * SCALE), TARGET_H), Image.LANCZOS)
    return bake_background(im)


def to_rgb565_le(im):
    """RGB888 -> raw little-endian RGB565 bytes (what the panel framebuffer uses)."""
    a = np.asarray(im.convert("RGB"), dtype=np.uint16)
    v = (((a[:, :, 0] >> 3) << 11) |
         ((a[:, :, 1] >> 2) << 5) |
         (a[:, :, 2] >> 3)).astype("<u2")
    return v.tobytes()


def lz4_block(raw):
    """Compress `raw` into one raw LZ4 block via the `lz4` CLI.

    `lz4 -l` writes the legacy stream format: a 4-byte magic followed by
    [uint32 blockSize, block] records. Our inputs are far below the 8 MB legacy
    block size, so there is exactly one record; strip the 8-byte header.
    """
    out = subprocess.run(["lz4", f"-{LZ4_LEVEL}", "-l", "-c"],
                         input=raw, stdout=subprocess.PIPE, check=True).stdout
    if out[:4] != LZ4_LEGACY_MAGIC:
        raise RuntimeError(f"unexpected lz4 legacy magic: {out[:4].hex()}")
    blocks = []
    off = 4
    while off < len(out):
        (size,) = struct.unpack_from("<I", out, off)
        off += 4
        if size == 0:
            break
        blocks.append(out[off:off + size])
        off += size
    if len(blocks) != 1:
        raise RuntimeError(f"expected 1 lz4 block, got {len(blocks)}")
    return blocks[0]


def build_archive():
    entries = []      # (id, relative_offset, comp_size, raw_size)
    payload = bytearray()

    def add(frame_id, im):
        raw = to_rgb565_le(im)
        comp = lz4_block(raw)
        entries.append((frame_id, len(payload), len(comp), len(raw)))
        payload.extend(comp)

    for i in range(FIRST, LAST + 1):
        add(i, bake_digit(i))
    add(COLON_ID, bake_colon())

    # Sort by id so entries[i].id == i and the device can index directly.
    entries.sort(key=lambda e: e[0])
    count = len(entries)
    header_len = 16 + count * 16

    index = bytearray()
    for (fid, rel, csize, rsize) in entries:
        index += struct.pack("<HHIII", fid, 0, header_len + rel, csize, rsize)

    data = bytearray()
    data += b"DLZ4"
    data += struct.pack("<HHHHI", 1, count, TARGET_W, TARGET_H, 0)
    data += index
    data += payload

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(data)
    return entries, payload


def verify(entries, payload):
    """Decode every block back through the `lz4` CLI and check the sizes."""
    for (fid, rel, csize, rsize) in entries:
        block = payload[rel:rel + csize]
        legacy = LZ4_LEGACY_MAGIC + struct.pack("<I", len(block)) + block
        dec = subprocess.run(["lz4", "-l", "-d", "-c"],
                             input=legacy, stdout=subprocess.PIPE, check=True).stdout
        if len(dec) != rsize:
            raise RuntimeError(f"frame {fid}: decoded {len(dec)} != {rsize}")
    print(f"verified {len(entries)} blocks")


def main():
    entries, payload = build_archive()
    verify(entries, payload)

    comp = np.array([e[2] for e in entries])
    raw = np.array([e[3] for e in entries])
    total = os.path.getsize(OUT)
    print(f"wrote {os.path.relpath(OUT)}: {len(entries)} frames, {total / 1024 / 1024:.2f} MB")
    print(f"  compressed avg {comp.mean() / 1024:.1f} KB "
          f"(min {comp.min() / 1024:.1f}, max {comp.max() / 1024:.1f})  "
          f"compression {raw.sum() / comp.sum():.1f}:1")


if __name__ == "__main__":
    main()
