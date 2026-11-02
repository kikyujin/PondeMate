#!/usr/bin/env python3
"""Converts PGN games into PondeMate game files for the game viewer.

    pgn2game.py [--out DIR] [--orient w|b | --player NAME] game.pgn...

Only the main line is kept; variations and comments are dropped. Each output
file is <PGN file name>.txt, ASCII, every line exactly 128 bytes (127 bytes
padded with spaces, then '\\n'):

    line 0   #<TAB>White<TAB>Black<TAB>Result<TAB>Date<TAB>plies<TAB>orientation
    line 1   0<TAB>-<TAB>-<TAB>start FEN<TAB>-<TAB>-
    line n   n<TAB>SAN<TAB>UCI<TAB>FEN after the move<TAB>eval<TAB>mark

eval is lichess's [%eval ...] copied as written ("-0.76", "#-3"); mark is the
move's annotation (? ?? ?! ! !! !?). Either is "-" when absent. Orientation is
"w" (white at the bottom) unless --orient says otherwise, or --player names the
player and that name is Black.

Games that are not Standard chess, PGN that does not parse, and lines that do
not fit 128 bytes are reported and skipped; nothing is truncated except player
names. Exits non-zero if any file was skipped.
"""

import argparse
import os
import re
import sys

import chess
import chess.pgn

LINE_BYTES = 128
MAX_FEN_CHARS = 95  # kMaxFenChars on the device
MAX_NAME_CHARS = 31
MAX_RESULT_CHARS = 7
MAX_DATE_CHARS = 10
MAX_PLIES = 9999

NAG_MARKS = {
    chess.pgn.NAG_GOOD_MOVE: "!",
    chess.pgn.NAG_MISTAKE: "?",
    chess.pgn.NAG_BRILLIANT_MOVE: "!!",
    chess.pgn.NAG_BLUNDER: "??",
    chess.pgn.NAG_SPECULATIVE_MOVE: "!?",
    chess.pgn.NAG_DUBIOUS_MOVE: "?!",
}
EVAL_RE = re.compile(r"\[%eval\s+([^\]\s,]+)")
EVAL_OK_RE = re.compile(r"^(#-?\d+|-?\d+(\.\d+)?)$")


class ConvertError(Exception):
    pass


def clean_text(value, limit):
    """Header text as printable ASCII without TABs, cut to `limit` characters."""
    text = "".join(c if 32 <= ord(c) < 127 else "?" for c in (value or "?").replace("\t", " ")).strip()
    return (text or "?")[:limit].rstrip() or "?"


def pad(line):
    data = line.encode("ascii")
    if len(data) > LINE_BYTES - 1:
        raise ConvertError(f"line is {len(data)} bytes, more than {LINE_BYTES - 1}: {line!r}")
    return data + b" " * (LINE_BYTES - 1 - len(data)) + b"\n"


def move_eval(node):
    found = EVAL_RE.search(node.comment or "")
    if not found:
        return "-"
    value = found.group(1)
    if not EVAL_OK_RE.match(value):
        raise ConvertError(f"unexpected eval {value!r} at {node.ply()}")
    return value


def move_mark(node):
    marks = [NAG_MARKS[n] for n in sorted(node.nags) if n in NAG_MARKS]
    return marks[0] if marks else "-"


def orientation(game, args):
    if args.orient:
        return args.orient
    if args.player and game.headers.get("Black", "") == args.player:
        return "b"
    return "w"


def convert(path, args):
    """Returns (output bytes, plies, evals, marks)."""
    with open(path, encoding="utf-8-sig") as f:
        game = chess.pgn.read_game(f)
        if game is None:
            raise ConvertError("no game in the file")
        if chess.pgn.read_game(f) is not None:
            raise ConvertError("more than one game in the file")
    if game.errors:
        raise ConvertError(f"PGN error: {game.errors[0]}")
    variant = game.headers.get("Variant", "Standard")
    if variant.lower() not in ("standard", "chess", "from position"):
        raise ConvertError(f"variant {variant!r} is not standard chess")

    board = game.board()
    if board.chess960:
        raise ConvertError("Chess960 castling is not supported")
    nodes = list(game.mainline())
    if len(nodes) > MAX_PLIES:
        raise ConvertError(f"{len(nodes)} plies, more than {MAX_PLIES}")

    h = game.headers
    header = "\t".join([
        "#",
        clean_text(h.get("White"), MAX_NAME_CHARS),
        clean_text(h.get("Black"), MAX_NAME_CHARS),
        clean_text(h.get("Result"), MAX_RESULT_CHARS),
        clean_text(h.get("Date"), MAX_DATE_CHARS),
        str(len(nodes)),
        orientation(game, args),
    ])
    lines = [pad(header)]

    def fen_of(b):
        fen = b.fen()
        if len(fen) > MAX_FEN_CHARS:
            raise ConvertError(f"FEN longer than {MAX_FEN_CHARS} characters: {fen}")
        return fen

    lines.append(pad("\t".join(["0", "-", "-", fen_of(board), "-", "-"])))
    evals = marks = 0
    for node in nodes:
        move = node.move
        san = board.san(move)
        board.push(move)
        ev = move_eval(node)
        mark = move_mark(node)
        evals += ev != "-"
        marks += mark != "-"
        lines.append(pad("\t".join([str(node.ply()), san, move.uci(), fen_of(board), ev, mark])))
    return b"".join(lines), len(nodes), evals, marks


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("pgn", nargs="+", help="PGN files, one game each")
    parser.add_argument("--out", default=os.path.expanduser("~/work/pondemate-data/games"),
                        help="output directory (default ~/work/pondemate-data/games)")
    side = parser.add_mutually_exclusive_group()
    side.add_argument("--orient", choices=["w", "b"], help="side at the bottom of the board")
    side.add_argument("--player", help="your name in the PGN; the board starts with your side at the bottom")
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    converted = []
    skipped = []
    for path in args.pgn:
        name = os.path.splitext(os.path.basename(path))[0]
        try:
            data, plies, evals, marks = convert(path, args)
        except (ConvertError, OSError, UnicodeDecodeError, ValueError) as e:
            skipped.append((path, str(e)))
            print(f"SKIP {path}: {e}", file=sys.stderr)
            continue
        out = os.path.join(args.out, name + ".txt")
        with open(out, "wb") as f:
            f.write(data)
        converted.append((name, plies, evals, marks))
        print(f"{name}: {plies} plies, eval on {evals}, marks {marks}")

    print(f"converted {len(converted)}, skipped {len(skipped)}")
    if converted:
        longest = max(converted, key=lambda c: c[1])
        print(f"longest: {longest[0]} ({longest[1]} plies)")
        print(f"with eval: {sum(1 for c in converted if c[2] > 0)}")
    for path, reason in skipped:
        print(f"  skipped {path}: {reason}")
    return 1 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
