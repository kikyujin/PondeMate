#!/usr/bin/env python3
"""Summary statistics for the lichess puzzle database.

Streams lichess_db_puzzle.csv.zst once (nothing is decompressed to disk) and
prints FEN/Moves lengths, how many puzzles fit a 256- or 128-byte set line,
theme counts and quality-field distributions.

    venv/bin/python stats.py lichess_db_puzzle.csv.zst [--out stats.txt]

Needs the `zstandard` package.
"""

import argparse
import csv
import io
import sys
from array import array

import zstandard

EXPECTED_HEADER = [
    "PuzzleId", "FEN", "Moves", "Rating", "RatingDeviation", "Popularity",
    "NbPlays", "Themes", "GameUrl", "OpeningTags", "DailyDate",
]
THEMES = ["mateIn1", "mateIn2", "hangingPiece", "fork", "pin", "endgame"]


def open_rows(path):
    raw = open(path, "rb")
    text = io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(raw), encoding="utf-8", newline="")
    reader = csv.reader(text)
    header = next(reader)
    if header != EXPECTED_HEADER:
        sys.exit(f"unexpected header: {header}")
    return reader


def quartiles(values):
    s = sorted(values)
    n = len(s)
    if n == 0:
        return "-"
    pick = lambda q: s[min(n - 1, int(q * (n - 1) + 0.5))]
    return f"min {s[0]}  p25 {pick(0.25)}  median {pick(0.5)}  p75 {pick(0.75)}  p90 {pick(0.9)}  max {s[-1]}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv_zst")
    parser.add_argument("--out", help="also write the report to this file")
    args = parser.parse_args()

    total = 0
    fen_min, fen_max, fen_sum = 10**9, 0, 0
    ply_min, ply_max, ply_sum = 10**9, 0, 0
    ply_hist = [0] * 31  # index 1..30; longer lines are counted in over30
    over30 = 0
    over255 = 0
    over127 = 0
    theme_counts = {t: 0 for t in THEMES}
    popularity = array("i")
    plays = array("i")
    rd = array("i")

    for row in open_rows(args.csv_zst):
        puzzle_id, fen, moves, rating = row[0], row[1], row[2], row[3]
        total += 1

        n = len(fen)
        fen_min, fen_max, fen_sum = min(fen_min, n), max(fen_max, n), fen_sum + n

        plies = len(moves.split())
        ply_min, ply_max, ply_sum = min(ply_min, plies), max(ply_max, plies), ply_sum + plies
        if plies <= 30:
            ply_hist[plies] += 1
        else:
            over30 += 1

        line_len = len(puzzle_id) + len(rating) + len(fen) + len(moves) + 3  # 3 TABs
        if line_len > 255:
            over255 += 1
        if line_len > 127:
            over127 += 1

        themes = row[7].split()
        for t in THEMES:
            if t in themes:
                theme_counts[t] += 1

        rd.append(int(row[4]))
        popularity.append(int(row[5]))
        plays.append(int(row[6]))

    lines = [
        f"source: {args.csv_zst}",
        f"rows: {total}",
        f"FEN chars: min {fen_min}  max {fen_max}  mean {fen_sum / total:.1f}",
        f"Moves plies: min {ply_min}  max {ply_max}  mean {ply_sum / total:.2f}",
        "plies histogram:",
    ]
    lines += [f"  {i:2d}: {ply_hist[i]}" for i in range(1, 31) if ply_hist[i]]
    if over30:
        lines.append(f"  >30: {over30}")
    lines += [
        "line = ID<TAB>Rating<TAB>FEN<TAB>Moves",
        f"  longer than 255 (does not fit 256-byte line): {over255}",
        f"  longer than 127 (does not fit 128-byte line): {over127}",
        "themes:",
    ]
    lines += [f"  {t}: {theme_counts[t]}" for t in THEMES]
    lines += [
        f"Popularity: {quartiles(popularity)}",
        f"NbPlays: {quartiles(plays)}",
        f"RatingDeviation: {quartiles(rd)}",
    ]
    report = "\n".join(lines) + "\n"
    print(report, end="")
    if args.out:
        with open(args.out, "w", encoding="ascii") as f:
            f.write(report)


if __name__ == "__main__":
    main()
