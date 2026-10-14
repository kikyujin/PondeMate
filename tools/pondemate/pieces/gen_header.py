#!/usr/bin/env python3
"""Generate src/activities/puzzle/PieceBitmaps.h from pieces.py.

Run from a scratch directory: importing pieces.py writes its preview PNGs into
the current directory.

    cd /tmp/somewhere && <venv>/bin/python <repo>/tools/pondemate/pieces/gen_header.py \\
        --reference board_start_v4.png --shifts 1,3,5,7 \\
        --out <repo>/src/activities/puzzle/PieceBitmaps.h

Two bitmap sets per piece (ink and halo each):

- Stage A: 60x60 px in logical orientation, unshifted. 8 bytes per row
  (MSB = leftmost pixel, last 4 bits zero), 60 rows. Used for per-pixel drawing.
- Stage B: rotated into the Portrait panel layout and pre-shifted by s bits
  (one copy per --shifts value). 9 bytes per physical row, 60 rows. Written
  straight into the framebuffer.

Portrait mapping (lib/GfxRenderer/GfxRenderer.cpp rotateCoordinates):
    phyX = y, phyY = panelHeight - 1 - x
so a piece whose box starts at logical (lx0, ly0) covers physical rows
panelHeight - lx0 - 60 .. panelHeight - lx0 - 1, and its physical columns start
at bit s = ly0 % 8 of byte ly0 / 8. Physical row j, column k is logical pixel
(x = 59 - j, y = k) of the piece.

Both sets are checked against --reference (pieces.py's board_start_*.png):
stage A by redrawing the start position in logical space, stage B by blitting
into a virtual Portrait framebuffer and reading it back.
"""

import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pieces  # noqa: E402  (writes board_*.png previews into the cwd)
from PIL import Image  # noqa: E402

ROW_BYTES = 8
SHIFTED_ROW_BYTES = 9  # 60 px + up to 7 bits of shift fits 72 bits
# ChessCore piece codes (lib/ChessCore/ChessCore.h): Pawn=1 .. King=6, BlackFlag=8.
KINDS = [("P", "PAWN", 1), ("N", "KNIGHT", 2), ("B", "BISHOP", 3), ("R", "ROOK", 4), ("Q", "QUEEN", 5), ("K", "KING", 6)]
BLACK_FLAG = 8


def pack(img):
    """Stage A: L image (255 = set) -> 60 rows of 8 bytes."""
    px = img.load()
    rows = []
    for y in range(pieces.P):
        row = [0] * ROW_BYTES
        for x in range(pieces.P):
            if px[x, y] >= 128:
                row[x >> 3] |= 0x80 >> (x & 7)
        rows.append(row)
    return rows


def pack_shifted(img, shift):
    """Stage B: rotate into Portrait panel layout, shift right by `shift` bits -> 60 rows of 9 bytes."""
    px = img.load()
    p = pieces.P
    rows = []
    for j in range(p):
        row = [0] * SHIFTED_ROW_BYTES
        for k in range(p):
            if px[p - 1 - j, k] >= 128:
                bit = shift + k
                row[bit >> 3] |= 0x80 >> (bit & 7)
        rows.append(row)
    return rows


def unpack_bit(rows, x, y):
    return (rows[y][x >> 3] >> (7 - (x & 7))) & 1


def board_background():
    """Empty board drawn the way pieces.board() draws it."""
    sq = pieces.SQ
    img = Image.new("L", (sq * 8, sq * 8), 255)
    px = img.load()
    for r in range(8):
        for f in range(8):
            if (r + f) % 2 == 1:
                for y in range(sq):
                    for x in range(sq):
                        if (x + y) % 2 == 0:
                            px[f * sq + x, r * sq + y] = 0
    return img


def redraw_start_stage_a(packed):
    sq, off = pieces.SQ, pieces.OFF
    img = board_background()
    px = img.load()
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


def redraw_start_stage_b(shifted, panel_w, panel_h, board_top):
    """Blit stage B bitmaps into a virtual Portrait framebuffer (1 = white) and read the board back."""
    sq, off, p = pieces.SQ, pieces.OFF, pieces.P
    width_bytes = panel_w // 8
    fb = bytearray(b"\xff" * (width_bytes * panel_h))

    def set_logical(x, y, black):
        phy_x, phy_y = y, panel_h - 1 - x
        i = phy_y * width_bytes + (phy_x >> 3)
        bit = 0x80 >> (phy_x & 7)
        if black:
            fb[i] &= ~bit & 0xFF
        else:
            fb[i] |= bit

    def get_logical(x, y):
        phy_x, phy_y = y, panel_h - 1 - x
        return (fb[phy_y * width_bytes + (phy_x >> 3)] >> (7 - (phy_x & 7))) & 1

    bg = board_background().load()
    for y in range(sq * 8):
        for x in range(sq * 8):
            if bg[x, y] < 128:
                set_logical(x, board_top + y, True)

    for r, row in enumerate(pieces.start):
        for f, c in enumerate(row):
            if c == ".":
                continue
            lx0, ly0 = f * sq + off, board_top + r * sq + off
            s = ly0 & 7
            ink, halo = shifted[(c.upper(), c.isupper(), s)]
            phy_y0 = panel_h - lx0 - p
            byte_x = ly0 >> 3
            for j in range(p):
                base = (phy_y0 + j) * width_bytes + byte_x
                for i in range(SHIFTED_ROW_BYTES):
                    fb[base + i] |= halo[j][i]
                    fb[base + i] &= ~ink[j][i] & 0xFF

    img = Image.new("L", (sq * 8, sq * 8), 255)
    px = img.load()
    for y in range(sq * 8):
        for x in range(sq * 8):
            px[x, y] = 255 if get_logical(x, board_top + y) else 0
    return img.convert("1")


def count_diff(a, b):
    if a.size != b.size:
        return None
    pa, pb = a.load(), b.load()
    w, h = a.size
    return sum(1 for y in range(h) for x in range(w) if pa[x, y] != pb[x, y])


def emit_array(out, name, rows):
    width = len(rows[0])
    out.append(f"static const uint8_t {name}[{width} * PIECE_SIZE] = {{")
    for row in rows:
        out.append("  " + ", ".join(f"0x{b:02X}" for b in row) + ",")
    out.append("};")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", required=True)
    parser.add_argument("--reference", required=True, help="pieces.py's start-position board PNG to check against")
    parser.add_argument("--shifts", required=True, help="comma separated bit shifts (0-7) to emit stage B bitmaps for")
    parser.add_argument("--board-top", type=int, default=10, help="logical y of the board's top edge (theme topPadding)")
    parser.add_argument("--panel", default="792x528", help="physical panel WIDTHxHEIGHT for the stage B self-check")
    args = parser.parse_args()

    if pieces.P != 60 or pieces.SQ != 66 or pieces.OFF != 3:
        sys.exit(f"unexpected geometry: SQ={pieces.SQ} P={pieces.P} OFF={pieces.OFF}")
    shifts = sorted({int(s) for s in args.shifts.split(",") if s != ""})
    if not shifts or any(s < 0 or s > 7 for s in shifts):
        sys.exit(f"--shifts must be values 0-7: {args.shifts}")
    panel_w, panel_h = (int(v) for v in args.panel.lower().split("x"))
    needed = sorted({(args.board_top + r * pieces.SQ + pieces.OFF) & 7 for r in range(8)})

    with open(os.path.join(HERE, "pieces.py"), "rb") as f:
        pieces_sha = hashlib.sha256(f.read()).hexdigest()

    packed = {}
    shifted = {}
    for letter, _, _ in KINDS:
        for white in (True, False):
            ink, halo = pieces.piece(letter, white)
            packed[(letter, white)] = (pack(ink), pack(halo))
            for s in shifts:
                shifted[(letter, white, s)] = (pack_shifted(ink, s), pack_shifted(halo, s))

    out = []
    out.append("#pragma once")
    out.append("// Generated file. Do not edit by hand.")
    out.append("// Source: tools/pondemate/pieces/pieces.py (sha256 " + pieces_sha + ")")
    out.append("// Command: cd <scratch dir> && <venv>/bin/python tools/pondemate/pieces/gen_header.py \\")
    out.append(f"//              --reference {os.path.basename(args.reference)} --shifts {','.join(map(str, shifts))} \\")
    out.append("//              --out src/activities/puzzle/PieceBitmaps.h")
    out.append("//")
    out.append("// Stage A (PIECE_*_INK/HALO): 1bpp, logical orientation, 8 bytes per row (MSB = leftmost")
    out.append("// pixel), 60 rows. Draw halo bits white, then ink bits black.")
    out.append("//")
    out.append("// Stage B (PIECE_*_S<n>): rotated for Portrait (phyX = y, phyY = panelHeight - 1 - x) and")
    out.append("// shifted right by n bits; 9 bytes per physical row, 60 rows. Physical row j, bit n + k is")
    out.append("// logical pixel (59 - j, k) of the piece. The piece box at logical (lx0, ly0) starts at")
    out.append("// physical row panelHeight - lx0 - 60, byte ly0 / 8, with n = ly0 % 8. Framebuffer bits are")
    out.append("// 1 = white: OR the halo mask, then AND NOT the ink mask.")
    out.append(f"// Shifts emitted: {','.join(map(str, shifts))}. Assumes the board top at logical y = {args.board_top}")
    out.append(f"// (theme topPadding), 66 px squares and a 3 px piece inset: n = ({args.board_top} + 3 + 66 * row) % 8")
    out.append(f"// = {','.join(map(str, needed))}. Other shifts fall back to stage A.")
    out.append("")
    out.append("#include <cstdint>")
    out.append("")
    out.append("static constexpr int PIECE_SQUARE = 66;")
    out.append("static constexpr int PIECE_SIZE = 60;")
    out.append("static constexpr int PIECE_OFFSET = 3;")
    out.append("static constexpr int PIECE_ROW_BYTES = 8;")
    out.append(f"static constexpr int PIECE_SHIFTED_ROW_BYTES = {SHIFTED_ROW_BYTES};")
    out.append("")
    out.append("// clang-format off")
    names = {}
    for letter, word, _ in KINDS:
        for white in (True, False):
            ink, halo = packed[(letter, white)]
            for part, rows in (("INK", ink), ("HALO", halo)):
                name = f"PIECE_{'W' if white else 'B'}_{word}_{part}"
                names[(letter, white, part, None)] = name
                emit_array(out, name, rows)
    for letter, word, _ in KINDS:
        for white in (True, False):
            for s in shifts:
                ink, halo = shifted[(letter, white, s)]
                for part, rows in (("INK", ink), ("HALO", halo)):
                    name = f"PIECE_{'W' if white else 'B'}_{word}_{part}_S{s}"
                    names[(letter, white, part, s)] = name
                    emit_array(out, name, rows)
    out.append("// clang-format on")
    out.append("")

    def entry_for(code, part, shift):
        for letter, _, kind in KINDS:
            if code == kind:
                return names.get((letter, True, part, shift), "nullptr")
            if code == kind | BLACK_FLAG:
                return names.get((letter, False, part, shift), "nullptr")
        return "nullptr"

    for part in ("INK", "HALO"):
        out.append("// Indexed by ChessCore piece code (type | BlackFlag); nullptr for empty/unused codes.")
        out.append(f"static const uint8_t* const PIECE_{part}_BY_CODE[16] = {{")
        for code in range(16):
            out.append(f"    {entry_for(code, part, None)},  // {code}")
        out.append("};")
        out.append("")
    for part in ("INK", "HALO"):
        out.append("// Indexed by [piece code][shift]; nullptr where no bitmap was generated.")
        out.append(f"static const uint8_t* const PIECE_{part}_SHIFTED[16][8] = {{")
        for code in range(16):
            cells = ", ".join(entry_for(code, part, s) for s in range(8))
            out.append(f"    {{{cells}}},  // {code}")
        out.append("};")
        out.append("")

    with open(args.out, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(out).rstrip("\n") + "\n")
    print(f"wrote {args.out}")

    reference = Image.open(args.reference).convert("1")
    diff_a = count_diff(redraw_start_stage_a(packed), reference)
    print(f"self-check A: redrawn start board vs {args.reference}: {diff_a} differing pixels")

    missing = [s for s in needed if s not in shifts]
    if missing:
        print(f"self-check B: skipped, board top {args.board_top} needs shifts {needed}, missing {missing}")
        sys.exit(1)
    diff_b = count_diff(redraw_start_stage_b(shifted, panel_w, panel_h, args.board_top), reference)
    print(f"self-check B: {panel_w}x{panel_h} Portrait framebuffer, board top {args.board_top}, "
          f"shifts used {needed}: {diff_b} differing pixels")
    if diff_a != 0 or diff_b != 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
