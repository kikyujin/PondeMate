#!/usr/bin/env python3
"""Generate src/activities/puzzle/PieceBitmaps.h from pieces.py.

Run from a scratch directory: importing pieces.py writes its preview PNGs into
the current directory.

    cd /tmp/somewhere && <venv>/bin/python <repo>/tools/pondemate/pieces/gen_header.py \\
        --out <repo>/src/activities/puzzle/PieceBitmaps.h

Each piece has an ink and a halo bitmap, 60x60 px, unrotated and unshifted:
8 bytes per row (MSB = leftmost pixel, last 4 bits zero), 60 rows.
After writing, the start position is redrawn from the packed bytes and compared
pixel by pixel with pieces.py's board_start_v2.png.
"""

import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pieces  # noqa: E402  (writes board_*_v2.png into the cwd)
from PIL import Image  # noqa: E402

ROW_BYTES = 8
# ChessCore piece codes (lib/ChessCore/ChessCore.h): Pawn=1 .. King=6, BlackFlag=8.
KINDS = [("P", "PAWN", 1), ("N", "KNIGHT", 2), ("B", "BISHOP", 3), ("R", "ROOK", 4), ("Q", "QUEEN", 5), ("K", "KING", 6)]
BLACK_FLAG = 8


def pack(img):
    """L image (255 = set) -> list of 60 rows, each a list of 8 bytes."""
    px = img.load()
    rows = []
    for y in range(pieces.P):
        row = [0] * ROW_BYTES
        for x in range(pieces.P):
            if px[x, y] >= 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        rows.append(row)
    return rows


def unpack_bit(rows, x, y):
    return (rows[y][x >> 3] >> (7 - (x & 7))) & 1


def redraw_start(packed):
    """Redraw the start position like pieces.board(), but from the packed bytes."""
    sq, off = pieces.SQ, pieces.OFF
    w = sq * 8
    img = Image.new("L", (w, w), 255)
    px = img.load()
    for r in range(8):
        for f in range(8):
            if (r + f) % 2 == 1:
                for y in range(sq):
                    for x in range(sq):
                        if (x + y) % 2 == 0:
                            px[f * sq + x, r * sq + y] = 0
    for r, row in enumerate(pieces.start):
        for f, c in enumerate(row):
            if c == ".":
                continue
            ink, halo = packed[(c.upper(), c.isupper())]
            ox, oy = f * sq + off, r * sq + off
            for y in range(pieces.P):
                for x in range(pieces.P):
                    if unpack_bit(halo, x, y):
                        px[ox + x, oy + y] = 255
            for y in range(pieces.P):
                for x in range(pieces.P):
                    if unpack_bit(ink, x, y):
                        px[ox + x, oy + y] = 0
    return img.convert("1")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    if pieces.P != 60 or pieces.SQ != 66 or pieces.OFF != 3:
        sys.exit(f"unexpected geometry: SQ={pieces.SQ} P={pieces.P} OFF={pieces.OFF}")

    with open(os.path.join(HERE, "pieces.py"), "rb") as f:
        pieces_sha = hashlib.sha256(f.read()).hexdigest()

    packed = {}
    for letter, _, _ in KINDS:
        for white in (True, False):
            ink, halo = pieces.piece(letter, white)
            packed[(letter, white)] = (pack(ink), pack(halo))

    out = []
    out.append("#pragma once")
    out.append("// Generated file. Do not edit by hand.")
    out.append("// Source: tools/pondemate/pieces/pieces.py (sha256 " + pieces_sha + ")")
    out.append("// Command: cd <scratch dir> && <venv>/bin/python tools/pondemate/pieces/gen_header.py \\")
    out.append("//              --out src/activities/puzzle/PieceBitmaps.h")
    out.append("//")
    out.append("// 1bpp, unrotated: 8 bytes per row (MSB = leftmost pixel), 60 rows per bitmap.")
    out.append("// Draw a piece at square origin + PIECE_OFFSET: halo bits white first, then ink bits black.")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("static constexpr int PIECE_SQUARE = 66;")
    out.append("static constexpr int PIECE_SIZE = 60;")
    out.append("static constexpr int PIECE_OFFSET = 3;")
    out.append("static constexpr int PIECE_ROW_BYTES = 8;")
    out.append("")
    out.append("// clang-format off")
    names = {}
    for letter, word, _ in KINDS:
        for white in (True, False):
            ink, halo = packed[(letter, white)]
            for part, rows in (("INK", ink), ("HALO", halo)):
                name = f"PIECE_{'W' if white else 'B'}_{word}_{part}"
                names[(letter, white, part)] = name
                out.append(f"static const uint8_t {name}[PIECE_ROW_BYTES * PIECE_SIZE] = {{")
                for row in rows:
                    out.append("  " + ", ".join(f"0x{b:02X}" for b in row) + ",")
                out.append("};")
    out.append("// clang-format on")
    out.append("")
    for part in ("INK", "HALO"):
        out.append(f"// Indexed by ChessCore piece code (type | BlackFlag); nullptr for empty/unused codes.")
        out.append(f"static const uint8_t* const PIECE_{part}_BY_CODE[16] = {{")
        for code in range(16):
            entry = "nullptr"
            for letter, _, kind in KINDS:
                if code == kind:
                    entry = names[(letter, True, part)]
                elif code == kind | BLACK_FLAG:
                    entry = names[(letter, False, part)]
            out.append(f"    {entry},  // {code}")
        out.append("};")
        out.append("")

    with open(args.out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")
    print(f"wrote {args.out}")

    # Self-check against the reference board pieces.py just rendered in the cwd.
    redrawn = redraw_start(packed)
    reference = Image.open("board_start_v2.png").convert("1")
    if redrawn.size != reference.size:
        sys.exit(f"self-check: size {redrawn.size} != {reference.size}")
    a, b = redrawn.load(), reference.load()
    w, h = redrawn.size
    diff = sum(1 for y in range(h) for x in range(w) if a[x, y] != b[x, y])
    print(f"self-check: redrawn start board vs board_start_v2.png: {diff} differing pixels")
    if diff:
        sys.exit(1)


if __name__ == "__main__":
    main()
