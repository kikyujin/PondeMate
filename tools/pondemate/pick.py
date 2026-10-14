#!/usr/bin/env python3
"""Pick a PondeMate puzzle set from the lichess puzzle database.

Streams lichess_db_puzzle.csv.zst, filters by theme, rating and quality,
shuffles with a fixed seed and writes one puzzle per 256-byte line:

    PuzzleId<TAB>Rating<TAB>FEN<TAB>Moves<spaces up to 255 bytes>\\n

Moves is copied from the CSV unchanged (UCI, space separated; the first move
is the opponent's). Puzzles whose line would exceed 255 bytes are dropped.

    venv/bin/python pick.py lichess_db_puzzle.csv.zst --themes mateIn1,mateIn2 \\
        --out sets/mate_r1000-1500.txt --manifest sets/manifest.json

Needs the `zstandard` package.
"""

import argparse
import csv
import datetime
import hashlib
import io
import json
import os
import random
import sys

import zstandard

EXPECTED_HEADER = [
    "PuzzleId", "FEN", "Moves", "Rating", "RatingDeviation", "Popularity",
    "NbPlays", "Themes", "GameUrl", "OpeningTags", "DailyDate",
]
LINE_BYTES = 256  # including the trailing newline


class HashingReader:
    """File wrapper that hashes the compressed bytes as they are read."""

    def __init__(self, f):
        self.f = f
        self.sha256 = hashlib.sha256()

    def read(self, size=-1):
        data = self.f.read(size)
        self.sha256.update(data)
        return data

    def readinto(self, buf):
        n = self.f.readinto(buf)
        self.sha256.update(memoryview(buf)[:n])
        return n

    def readable(self):
        return True


def last_modified(headers_path):
    if not headers_path or not os.path.exists(headers_path):
        return None
    with open(headers_path, encoding="latin-1") as f:
        for line in f:
            name, _, value = line.partition(":")
            if name.strip().lower() == "last-modified":
                return value.strip()
    return None


def update_manifest(path, entry, source):
    manifest = {"sets": {}}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            manifest = json.load(f)
    manifest["source"] = source
    manifest["sets"][entry["file"]] = entry

    # Overlap between sets: PuzzleIds that appear in more than one set file.
    base = os.path.dirname(path)
    seen = {}
    for name in manifest["sets"]:
        with open(os.path.join(base, name), encoding="ascii") as f:
            for line in f:
                seen.setdefault(line.split("\t", 1)[0], set()).add(name)
    shared = [ids for ids in seen.values() if len(ids) > 1]
    pairs = {}
    for ids in shared:
        key = " + ".join(sorted(ids))
        pairs[key] = pairs.get(key, 0) + 1
    manifest["overlap"] = {"puzzles_in_multiple_sets": len(shared), "by_sets": dict(sorted(pairs.items()))}
    manifest["updated"] = datetime.datetime.now().astimezone().isoformat(timespec="seconds")

    with open(path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
        f.write("\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv_zst")
    parser.add_argument("--themes", required=True, help="comma separated; a puzzle matches if it has ANY of them")
    parser.add_argument("--rating-min", type=int, default=1000)
    parser.add_argument("--rating-max", type=int, default=1500)
    parser.add_argument("--min-popularity", type=int, default=80)
    parser.add_argument("--min-plays", type=int, default=500)
    parser.add_argument("--max-rd", type=int, default=80)
    parser.add_argument("--max-plies", type=int, default=12, help="Moves count, opponent's first move included")
    parser.add_argument("--count", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20261014)
    parser.add_argument("--out", required=True)
    parser.add_argument("--manifest", help="JSON manifest to add this set to (created if missing)")
    parser.add_argument("--headers", help="curl -D headers of the download, for Last-Modified "
                        "(default: headers.txt next to the source)")
    args = parser.parse_args()

    themes = {t for t in args.themes.split(",") if t}
    stages = ["themes", "rating", "popularity", "plays", "rd", "plies", "fits_256"]
    remaining = dict.fromkeys(stages, 0)
    total = 0
    candidates = []

    with open(args.csv_zst, "rb") as raw:
        hashing = HashingReader(raw)
        text = io.TextIOWrapper(zstandard.ZstdDecompressor().stream_reader(hashing), encoding="utf-8", newline="")
        reader = csv.reader(text)
        if next(reader) != EXPECTED_HEADER:
            sys.exit("unexpected CSV header")
        for row in reader:
            total += 1
            if themes.isdisjoint(row[7].split()):
                continue
            remaining["themes"] += 1
            if not args.rating_min <= int(row[3]) <= args.rating_max:
                continue
            remaining["rating"] += 1
            if int(row[5]) < args.min_popularity:
                continue
            remaining["popularity"] += 1
            if int(row[6]) < args.min_plays:
                continue
            remaining["plays"] += 1
            if int(row[4]) > args.max_rd:
                continue
            remaining["rd"] += 1
            if len(row[2].split()) > args.max_plies:
                continue
            remaining["plies"] += 1
            body = f"{row[0]}\t{row[3]}\t{row[1]}\t{row[2]}"
            if not body.isascii() or len(body) > LINE_BYTES - 1:
                continue
            remaining["fits_256"] += 1
            candidates.append(body)
        while hashing.read(1 << 20):  # hash any trailing bytes the decoder did not need
            pass
        source_sha256 = hashing.sha256.hexdigest()

    print(f"rows: {total}")
    for stage in stages:
        print(f"  after {stage:<10} {remaining[stage]}")
    dropped_long = remaining["plies"] - remaining["fits_256"]
    print(f"  dropped for > 255 bytes: {dropped_long}")

    random.Random(args.seed).shuffle(candidates)
    chosen = candidates[: args.count]
    if len(chosen) < args.count:
        print(f"WARNING: only {len(chosen)} candidates for --count {args.count}")

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="ascii", newline="") as f:
        for body in chosen:
            f.write(body.ljust(LINE_BYTES - 1) + "\n")

    size = os.path.getsize(args.out)
    if size != LINE_BYTES * len(chosen):
        sys.exit(f"size check failed: {size} != {LINE_BYTES} * {len(chosen)}")
    print(f"wrote {len(chosen)} puzzles, {size} bytes -> {args.out}")

    if args.manifest:
        headers = args.headers or os.path.join(os.path.dirname(os.path.abspath(args.csv_zst)), "headers.txt")
        source = {
            "file": os.path.basename(args.csv_zst),
            "url": "https://database.lichess.org/lichess_db_puzzle.csv.zst",
            "license": "CC0",
            "sha256": source_sha256,
            "last_modified": last_modified(headers),
            "rows": total,
        }
        entry = {
            "file": os.path.relpath(os.path.abspath(args.out), os.path.dirname(os.path.abspath(args.manifest))),
            "count": len(chosen),
            "args": {
                "themes": sorted(themes),
                "rating_min": args.rating_min,
                "rating_max": args.rating_max,
                "min_popularity": args.min_popularity,
                "min_plays": args.min_plays,
                "max_rd": args.max_rd,
                "max_plies": args.max_plies,
                "count": args.count,
                "seed": args.seed,
            },
            "remaining": remaining,
            "created": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        }
        update_manifest(args.manifest, entry, source)
        print(f"manifest updated: {args.manifest}")


if __name__ == "__main__":
    main()
