#!/usr/bin/env python3

import argparse
import os
import struct
import subprocess

import numpy as np
from PIL import Image

SRC = os.path.join(os.path.dirname(__file__), "..", "images")
DEFAULT_OUT = os.path.join(os.path.dirname(__file__), "..", "sketches",
                           "dice-clock", "frames_800x480.lz4")

BBOX = (39, 239, 381, 678)

DEFAULT_TARGET_W = 180

DEFAULT_BG = (0x0A, 0x3D, 0x14)

COLON_ALPHA_MIN = 8

BRIGHTEN = 1.5

FIRST = 1
LAST = 450
COLON_ID = 0

LZ4_LEVEL = 12

LZ4_LEGACY_MAGIC = b"\x02\x21\x4c\x18"

TARGET_W = DEFAULT_TARGET_W
TARGET_H = 0
SCALE = 0.0
BG = DEFAULT_BG
COLON_TIGHT = False


def bake_background(im):
    rgb = Image.new("RGB", im.size, BG)
    rgb.paste(im, mask=im.getchannel("A"))
    return rgb


def apply_brighten(im):
    if BRIGHTEN == 1.0:
        return im
    a = np.asarray(im, dtype=np.float32)
    a[:, :, :3] = np.clip(a[:, :, :3] * BRIGHTEN, 0.0, 255.0)
    return Image.fromarray(a.astype(np.uint8))


def bake_digit(i):
    src = os.path.join(SRC, f"{i:04d}.png")
    im = Image.open(src).convert("RGBA").crop(BBOX)
    im = im.resize((TARGET_W, TARGET_H), Image.LANCZOS)
    return bake_background(apply_brighten(im))


def colon_dots_bbox(im):
    a = np.array(im.getchannel("A"))
    ys = np.where(a.max(axis=1) > COLON_ALPHA_MIN)[0]
    xs = np.where(a.max(axis=0) > COLON_ALPHA_MIN)[0]
    if len(ys) == 0 or len(xs) == 0:
        raise RuntimeError("colon.png has no opaque pixels")
    return (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1)


def bake_colon():
    src = os.path.join(SRC, "colon.png")
    im = Image.open(src).convert("RGBA")
    if COLON_TIGHT:
        im = im.crop(colon_dots_bbox(im))
        w = max(1, int(round(im.width * SCALE)))
        h = max(1, int(round(im.height * SCALE)))
        im = im.resize((w, h), Image.LANCZOS)
    else:
        w = im.width
        im = im.crop((0, BBOX[1], w, BBOX[3]))
        im = im.resize((round(w * SCALE), TARGET_H), Image.LANCZOS)
    return bake_background(im)


def to_rgb565_le(im):
    a = np.asarray(im.convert("RGB"), dtype=np.uint16)
    v = (((a[:, :, 0] >> 3) << 11) |
         ((a[:, :, 1] >> 2) << 5) |
         (a[:, :, 2] >> 3)).astype("<u2")
    return v.tobytes()


def lz4_block(raw):
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


def build_archive(out_path):
    entries = []
    payload = bytearray()
    shapes = {}

    def add(frame_id, im):
        shapes[frame_id] = im.size
        raw = to_rgb565_le(im)
        comp = lz4_block(raw)
        entries.append((frame_id, len(payload), len(comp), len(raw)))
        payload.extend(comp)

    for i in range(FIRST, LAST + 1):
        add(i, bake_digit(i))
    add(COLON_ID, bake_colon())

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

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(data)
    return entries, payload, shapes


def verify(entries, payload):
    for (fid, rel, csize, rsize) in entries:
        block = payload[rel:rel + csize]
        legacy = LZ4_LEGACY_MAGIC + struct.pack("<I", len(block)) + block
        dec = subprocess.run(["lz4", "-l", "-d", "-c"],
                             input=legacy, stdout=subprocess.PIPE, check=True).stdout
        if len(dec) != rsize:
            raise RuntimeError(f"frame {fid}: decoded {len(dec)} != {rsize}")
    print(f"verified {len(entries)} blocks")


ASM_TEMPLATE = """    .section .rodata.frames, "a"
    .global frames_archive_start
    .global frames_archive_end
    .align 4
frames_archive_start:
    .incbin "{incbin_path}"
frames_archive_end:
"""


def write_asm(asm_path, archive_path):
    with open(asm_path, "w") as f:
        f.write(ASM_TEMPLATE.format(incbin_path=archive_path))
    print(f"wrote {os.path.relpath(asm_path)} "
          f"(absolute .incbin -> {archive_path})")


def parse_bg(text):
    s = text.lstrip("#")
    if len(s) != 6:
        raise argparse.ArgumentTypeError("background must be 6 hex digits, e.g. 00410A")
    try:
        return (int(s[0:2], 16), int(s[2:4], 16), int(s[4:6], 16))
    except ValueError:
        raise argparse.ArgumentTypeError("background must be 6 hex digits, e.g. 00410A")


def main():
    global TARGET_W, TARGET_H, SCALE, BG, COLON_TIGHT, BRIGHTEN

    ap = argparse.ArgumentParser(formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--target-w", type=int, default=DEFAULT_TARGET_W,
                    help=f"digit frame width in px (default {DEFAULT_TARGET_W})")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="output archive path (default "
                         "sketches/dice-clock/frames_800x480.lz4)")
    ap.add_argument("--bg", type=parse_bg, default=DEFAULT_BG,
                    help="background baked into every frame, RRGGBB "
                         f"(default {DEFAULT_BG[0]:02X}{DEFAULT_BG[1]:02X}{DEFAULT_BG[2]:02X})")
    ap.add_argument("--colon-tight", action="store_true",
                    help="bake the colon cropped to just its two dots and scaled "
                         "like the digits (for a portrait layout)")
    ap.add_argument("--emit-asm", action="store_true",
                    help="also write <out without extension>.S, the assembler file "
                         "that embeds the archive with .incbin so the frames become "
                         "part of the sketch image (needed by both sketches)")
    ap.add_argument("--brighten", type=float, default=BRIGHTEN,
                    help=f"multiply the dice artwork's RGB by this before "
                         f"compositing, e.g. 1.2 for 20%% brighter "
                         f"(default {BRIGHTEN:g}). The baked background and the "
                         f"colon are not affected.")
    args = ap.parse_args()

    TARGET_W = args.target_w
    SCALE = TARGET_W / (BBOX[2] - BBOX[0])
    TARGET_H = int(round((BBOX[3] - BBOX[1]) * SCALE))
    BG = args.bg
    COLON_TIGHT = args.colon_tight
    if args.brighten <= 0:
        raise SystemExit("--brighten must be greater than 0")
    BRIGHTEN = args.brighten

    out_path = os.path.abspath(args.out)

    entries, payload, shapes = build_archive(out_path)
    verify(entries, payload)

    comp = np.array([e[2] for e in entries])
    raw = np.array([e[3] for e in entries])
    total = os.path.getsize(out_path)
    colon_w, colon_h = shapes[COLON_ID]
    print(f"digit frame {TARGET_W}x{TARGET_H}, colon {colon_w}x{colon_h}, "
          f"bg #{BG[0]:02X}{BG[1]:02X}{BG[2]:02X}, dice brightness x{BRIGHTEN:g}")
    print(f"wrote {os.path.relpath(out_path)}: {len(entries)} frames, "
          f"{total / 1024 / 1024:.2f} MB")
    print(f"  compressed avg {comp.mean() / 1024:.1f} KB "
          f"(min {comp.min() / 1024:.1f}, max {comp.max() / 1024:.1f})  "
          f"compression {raw.sum() / comp.sum():.1f}:1")
    print(f"  sketch constants -> FRAME_W {TARGET_W}, FRAME_H {TARGET_H}, "
          f"COLON_W {colon_w}, COLON_H {colon_h}")

    if args.emit_asm:
        write_asm(os.path.splitext(out_path)[0] + ".S", out_path)


if __name__ == "__main__":
    main()
