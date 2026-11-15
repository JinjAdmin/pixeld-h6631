"""Render a contact sheet of the .pxi icons (and first frames of .anim files) to a PNG for viewing.

  python tools/icon_sheet.py <icons_dir> <out.png> [anim files...]

Private preview only (Govee artwork) - keep the output under private/.
"""
import pathlib
import re
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from minipng import write_png  # noqa: E402

SRC = pathlib.Path(__file__).resolve().parent.parent / "src" / "features.c"
FONT = {m.group(1): [int(x) for x in m.group(2).split(",")]
        for m in re.finditer(r"\{'(.)',\{([0-9,]+)\}\}", SRC.read_text())}
BG, CELL_BG, LABEL = (24, 24, 28), (0, 0, 0), (140, 140, 150)
SCALE, MAXW, PAD = 4, 300, 3


def load_pxi(p):
    d = p.read_bytes()
    w, h = d[4], d[5]
    px = d[10:10 + w * h * 4]
    return w, h, [[tuple(px[(y * w + x) * 4:(y * w + x) * 4 + 4]) for x in range(w)] for y in range(h)]


def load_anim(p):
    d = p.read_bytes()
    px = d[8:8 + 4992]
    return 52, 32, [[tuple(px[(y * 52 + x) * 3:(y * 52 + x) * 3 + 3]) + (255,) for x in range(52)] for y in range(32)]


def text(canvas, s, x, y):
    for ch in s.upper():
        g = FONT.get(ch, FONT.get(" "))
        for j, bits in enumerate(g):
            for i in range(3):
                if bits & (4 >> i):
                    canvas[y + j][x + i] = LABEL
        x += 4


def main():
    icons_dir, out = pathlib.Path(sys.argv[1]), sys.argv[2]
    items = [(p.stem.replace("_", " ") if p.suffix == ".pxi" else p.stem.replace("Clock ", "").replace("Alarm ", "alarm"),
              (load_pxi if p.suffix == ".pxi" else load_anim)(p))
             for p in sorted(icons_dir.glob("*.pxi")) + [pathlib.Path(a) for a in sys.argv[3:]]]
    # flow layout
    placed, x, y, rowh = [], PAD, PAD, 0
    for name, (w, h, rows) in items:
        cw = max(w, len(name) * 4 - 1)
        if x + cw + PAD > MAXW:
            x, y, rowh = PAD, y + rowh + PAD, 0
        placed.append((x, y, cw, name, w, h, rows))
        x += cw + PAD
        rowh = max(rowh, h + 7)
    H, W = y + rowh + PAD, MAXW
    c = [[BG] * W for _ in range(H)]
    for x, y, cw, name, w, h, rows in placed:
        ox = x + (cw - w) // 2
        for j in range(h):
            for i in range(w):
                r, g, b, a = rows[j][i]
                c[y + j][ox + i] = (r, g, b) if a else CELL_BG
        text(c, name, x, y + h + 1)
    big = [[px for px in row for _ in range(SCALE)] for row in c for _ in range(SCALE)]
    write_png(out, W * SCALE, H * SCALE, big)
    print(out, W * SCALE, "x", H * SCALE)


if __name__ == "__main__":
    main()
