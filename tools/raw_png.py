"""raw_png.py - turn 52x32 RGB frames (screen_preview output, /api/frame) into a scaled PNG contact strip.

    python tools/raw_png.py frames.raw out.png [scale=8] [max_frames=8]

Frames are laid out left to right with a 1-pixel (scaled) grey gap; each panel pixel becomes a scale x scale
square with a thin dark border so it looks like the LED grid.
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from minipng import write_png  # noqa: E402

W, H, FRAME = 52, 32, 52 * 32 * 3


def main():
    src, out = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    maxf = int(sys.argv[4]) if len(sys.argv) > 4 else 8
    data = pathlib.Path(src).read_bytes()
    n = min(len(data) // FRAME, maxf)
    if n == 0:
        sys.exit("no frames")
    gap = scale
    tw = n * W * scale + (n - 1) * gap
    rows = [[(40, 40, 40)] * tw for _ in range(H * scale)]
    for f in range(n):
        fr = data[f * FRAME:(f + 1) * FRAME]
        ox = f * (W * scale + gap)
        for y in range(H):
            for x in range(W):
                c = tuple(fr[(y * W + x) * 3:(y * W + x) * 3 + 3])
                for dy in range(scale):
                    row = rows[y * scale + dy]
                    for dx in range(scale):
                        edge = scale >= 4 and (dx == scale - 1 or dy == scale - 1)
                        row[ox + x * scale + dx] = (12, 12, 12) if edge else c
    write_png(out, tw, H * scale, rows)
    print(out, f"{n} frame(s)")


if __name__ == "__main__":
    main()
