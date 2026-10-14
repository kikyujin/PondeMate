# PondeMate puzzle tools

PC-side tools that pick chess puzzles for PondeMate from the lichess puzzle
database and check every picked puzzle with the same ChessCore the device runs.

## Source data

- URL: https://database.lichess.org/lichess_db_puzzle.csv.zst
- License: CC0
- Columns: `PuzzleId,FEN,Moves,Rating,RatingDeviation,Popularity,NbPlays,Themes,GameUrl,OpeningTags,DailyDate`
- `FEN` is the position before the opponent's move; the first UCI move in
  `Moves` is the opponent's, the player's solution starts at the second.

Keep the download, the Python venv and the generated sets outside the
repository (for example `~/work/pondemate-data/`). The scripts stream the
`.zst` file and never write the decompressed CSV to disk.

```sh
mkdir -p ~/work/pondemate-data && cd ~/work/pondemate-data
curl -fL -D headers.txt -o lichess_db_puzzle.csv.zst https://database.lichess.org/lichess_db_puzzle.csv.zst
python3 -m venv venv && venv/bin/pip install zstandard
```

## stats.py

One pass over the database: FEN/Moves lengths, ply histogram, how many puzzles
do not fit a 256- or 128-byte line, theme counts, and quartiles of
`Popularity`, `NbPlays` and `RatingDeviation`.

```sh
venv/bin/python <repo>/tools/pondemate/stats.py lichess_db_puzzle.csv.zst --out stats.txt
```

## pick.py

Filters, shuffles with a fixed seed and writes one set file. Prints how many
puzzles remain after each filter.

```sh
venv/bin/python <repo>/tools/pondemate/pick.py lichess_db_puzzle.csv.zst \
    --themes mateIn1,mateIn2 --out sets/mate_r1000-1500.txt --manifest sets/manifest.json
```

| Argument | Default | Meaning |
|---|---|---|
| `--themes` | (required) | Comma separated; a puzzle matches if it has ANY of them |
| `--rating-min` / `--rating-max` | 1000 / 1500 | Inclusive |
| `--min-popularity` | 80 | `Popularity` at least |
| `--min-plays` | 500 | `NbPlays` at least |
| `--max-rd` | 80 | `RatingDeviation` at most |
| `--max-plies` | 12 | Number of moves in `Moves`, opponent's first move included |
| `--count` | 1000 | Puzzles to write; fewer if there are fewer candidates |
| `--seed` | 20261014 | Shuffle seed |
| `--out` | (required) | Output set file |
| `--manifest` | - | JSON manifest to add this set to: arguments, counts after each filter, source sha256 and Last-Modified, overlap between sets |
| `--headers` | `headers.txt` next to the source | `curl -D` headers, read for Last-Modified |

### Set file format

ASCII, one puzzle per line, every line exactly 256 bytes including the newline:

```
PuzzleId<TAB>Rating<TAB>FEN<TAB>Moves<spaces up to byte 255>\n
```

`Moves` is copied from the database unchanged (UCI, space separated). Puzzle
`n` (0-based) starts at byte `n * 256`, so the device can seek straight to it.
Puzzles whose line would not fit are dropped. The order in the file is the
order the device presents them.

## validate.sh / validate.cpp

Builds `validate.cpp` against `lib/ChessCore` with the host compiler and checks
every `*.txt` in a sets directory:

```sh
tools/pondemate/validate.sh [sets_dir]    # default ~/work/pondemate-data/sets
```

For each line: 256 bytes ending in `\n`, four TAB-separated fields, the FEN
parses, and every UCI move matches a legal move (`moveToString`), is converted
to SAN and played. Files named `mate_*.txt` must also end in checkmate. The
first three puzzles of each set are printed with the opponent's move and the
solution in SAN. Exits non-zero if any puzzle fails.

## pieces/ (piece bitmaps)

`pieces/pieces.py` is the piece silhouette prototype (Pillow). `pieces/gen_header.py`
turns its 12 pieces into `src/activities/puzzle/PieceBitmaps.h`: an ink and a
halo bitmap per piece, 60x60 px, 8 bytes per row (MSB = leftmost pixel), plus
tables indexed by ChessCore piece code. Run it from a scratch directory, since
importing `pieces.py` writes preview PNGs into the current directory:

```sh
venv/bin/pip install pillow
cd "$(mktemp -d)" && <venv>/bin/python <repo>/tools/pondemate/pieces/gen_header.py \
    --out <repo>/src/activities/puzzle/PieceBitmaps.h
```

The script redraws the start position from the packed bytes and fails unless it
matches `pieces.py`'s `board_start_v2.png` pixel for pixel.
