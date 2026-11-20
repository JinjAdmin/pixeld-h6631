"""Draws the printer-head icons used by examples/screens/3D printer.scr (original art, not Govee's).

    python examples/icons/make_printer_head.py [out_dir]   (default: this folder)

printer_head.pxi        12x4, 10 frames x 160 ms: gantry bar with the nozzle sliding left and right
                        (1.6 s there and back) and a flickering hot-end pixel under it.
printer_head_still.pxi  12x4, 1 frame: the same gantry with the nozzle parked in the middle (paused, done, failed).

Rows: 0 nozzle top, 1 gantry bar, 2 nozzle tip, 3 hot pixel.
"""
import os
import struct
import sys

W, H = 12, 4
GANTRY = (170, 175, 190)
NOZZLE = (255, 140, 40)
HOT = (255, 230, 120)


def frame(nx, hot):
    px = [[(0, 0, 0, 0)] * W for _ in range(H)]
    for x in range(W):
        px[1][x] = GANTRY + (255,)
    for dx in (0, 1):
        px[0][nx + dx] = NOZZLE + (255,)
        px[1][nx + dx] = NOZZLE + (255,)
        px[2][nx + dx] = NOZZLE + (255,)
    if hot is not None:
        px[3][nx + hot] = HOT + (255,)
    return b"".join(bytes(c) for row in px for c in row)


def write(path, frames):
    with open(path, "wb") as f:
        f.write(b"PXI1" + struct.pack("<BBH", W, H, len(frames)))
        for delay, rgba in frames:
            f.write(struct.pack("<H", delay) + rgba)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    path = [0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1]  # nozzle x (left pixel), ping-pong over the 6-px part
    write(os.path.join(out, "printer_head.pxi"),
          [(1600 // len(path), frame(2 + x, i % 2)) for i, x in enumerate(path)])
    write(os.path.join(out, "printer_head_still.pxi"), [(1000, frame(5, None))])
    print("wrote printer_head.pxi, printer_head_still.pxi to", out)


main()
