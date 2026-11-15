"""Convert animations (GIF, PNG, Govee's PNG-container .txt) to pixeld .anim files.

  python tools/anim_convert.py <input> [<input> ...] -o <out_dir> [--name NAME]

.anim format (little-endian): b"PXA1", u16 frame_count, then per frame: u16 delay_ms + 52*32*3 RGB bytes
(row-major from the top-left). Frames smaller than 52x32 are centred on black; larger ones are
cropped to the centre. Transparent pixels show the previous frame (GIF/Govee overlay semantics).
No third-party dependencies.
"""
import argparse
import os
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from minipng import read_png  # noqa: E402

W, H = 52, 32
MIN_DELAY, MAX_DELAY, DEFAULT_DELAY = 20, 10000, 100


# ------------------------------------------------------------------ GIF decoder
def _lzw(data: bytes, min_code: int, npix: int) -> list:
    clear, end = 1 << min_code, (1 << min_code) + 1
    size = min_code + 1
    table = [[i] for i in range(clear)] + [[], []]
    out, prev = [], None
    bitpos, nbits = 0, len(data) * 8
    while bitpos + size <= nbits and len(out) < npix:
        code = 0
        for k in range(size):
            if data[(bitpos + k) >> 3] & (1 << ((bitpos + k) & 7)):
                code |= 1 << k
        bitpos += size
        if code == clear:
            size, table, prev = min_code + 1, [[i] for i in range(clear)] + [[], []], None
            continue
        if code == end:
            break
        if prev is None:
            entry = table[code]
        elif code < len(table):
            entry = table[code]
            table.append(prev + [entry[0]])
        else:
            entry = prev + [prev[0]]
            table.append(entry)
        out.extend(entry)
        prev = entry
        if len(table) == (1 << size) and size < 12:
            size += 1
    return out


def decode_gif(d: bytes):
    """Yield (delay_ms, canvas) with canvas = list of [r,g,b] rows of the logical screen size."""
    assert d[:6] in (b"GIF87a", b"GIF89a"), "not a GIF"
    sw, sh, flags, bgi, _ = struct.unpack("<HHBBB", d[6:13])
    pos = 13
    gct = None
    if flags & 0x80:
        n = 2 << (flags & 7)
        gct = [tuple(d[pos + i * 3:pos + i * 3 + 3]) for i in range(n)]
        pos += 3 * n
    canvas = [[(0, 0, 0)] * sw for _ in range(sh)]
    delay, transp, disposal = DEFAULT_DELAY, None, 0
    while pos < len(d):
        b = d[pos]
        if b == 0x3B:
            break
        if b == 0x21:  # extension
            label = d[pos + 1]; pos += 2
            if label == 0xF9:
                blk = d[pos + 1:pos + 1 + d[pos]]
                pf, dl, ti = blk[0], struct.unpack("<H", blk[1:3])[0], blk[3]
                delay = dl * 10 if dl else DEFAULT_DELAY
                transp = ti if pf & 1 else None
                disposal = (pf >> 2) & 7
            while d[pos]:
                pos += d[pos] + 1
            pos += 1
            continue
        if b != 0x2C:
            break
        x, y, w, h, f = struct.unpack("<HHHHB", d[pos + 1:pos + 10]); pos += 10
        ct = gct
        if f & 0x80:
            n = 2 << (f & 7)
            ct = [tuple(d[pos + i * 3:pos + i * 3 + 3]) for i in range(n)]
            pos += 3 * n
        min_code = d[pos]; pos += 1
        data = bytearray()
        while d[pos]:
            data += d[pos + 1:pos + 1 + d[pos]]; pos += d[pos] + 1
        pos += 1
        idx = _lzw(bytes(data), min_code, w * h)
        rows = list(range(h))
        if f & 0x40:  # interlaced
            rows = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
        saved = [r[:] for r in canvas] if disposal == 3 else None
        for i, ry in enumerate(rows):
            cy = y + ry
            if cy >= sh:
                continue
            for cx_off in range(w):
                k = i * w + cx_off
                if k >= len(idx):
                    break
                ci = idx[k]
                if ci == transp or x + cx_off >= sw or ci >= len(ct):
                    continue
                canvas[cy][x + cx_off] = ct[ci]
        yield delay, [r[:] for r in canvas]
        if disposal == 2:
            for ry in range(y, min(y + h, sh)):
                for cx in range(x, min(x + w, sw)):
                    canvas[ry][cx] = (0, 0, 0)
        elif disposal == 3 and saved:
            canvas = saved
        delay, transp, disposal = DEFAULT_DELAY, None, 0


# ------------------------------------------------------------------ Govee PNG container
def decode_govee_png_container(d: bytes):
    """Yield (delay_ms, rgba_rows) for each embedded PNG. Per-frame record before each PNG:
    .. 0x64 idx u32(delay_ms; low 16 bits used) u16(record_len) 0x00 u16(png_len)."""
    frames, i = [], 0
    while True:
        s = d.find(b"\x89PNG\r\n\x1a\n", i)
        if s < 0:
            break
        e = d.find(b"IEND", s) + 8
        pre = d[s - 13:s]
        delay = None
        if len(pre) == 13 and pre[2] == 0x64 and struct.unpack("<H", pre[11:13])[0] == e - s:
            delay = struct.unpack("<H", pre[4:6])[0]
        frames.append([delay, d[s:e]])
        i = e
    known = [f[0] for f in frames if f[0]]
    for f in frames:
        if not f[0]:
            f[0] = known[0] if known else DEFAULT_DELAY
    for delay, png in frames:
        w, h, rows = read_png(png)
        yield delay, rows


# ------------------------------------------------------------------ compose to 52x32 frames
def place(canvas, rows, alpha: bool):
    """Draw rows (RGB or RGBA tuples) centred onto the 52x32 canvas (in place)."""
    h, w = len(rows), len(rows[0]) if rows else 0
    ox, oy = (W - w) // 2, (H - h) // 2
    for y in range(h):
        cy = y + oy
        if not 0 <= cy < H:
            continue
        for x in range(w):
            cx = x + ox
            if not 0 <= cx < W:
                continue
            p = rows[y][x]
            if alpha and len(p) == 4:
                a = p[3]
                if a == 0:
                    continue
                q = canvas[cy][cx]
                canvas[cy][cx] = tuple((p[c] * a + q[c] * (255 - a)) // 255 for c in range(3))
            else:
                canvas[cy][cx] = tuple(p[:3])


def convert(path: pathlib.Path):
    d = path.read_bytes()
    canvas = [[(0, 0, 0)] * W for _ in range(H)]
    out = []
    if d[:6] in (b"GIF87a", b"GIF89a"):
        for delay, gif_canvas in decode_gif(d):
            frame = [[(0, 0, 0)] * W for _ in range(H)]
            place(frame, gif_canvas, alpha=False)
            out.append((delay, frame))
    elif d[:8] == b"\x89PNG\r\n\x1a\n":
        place(canvas, read_png(d)[2], alpha=True)
        out.append((1000, canvas))
    elif b"\x89PNG\r\n\x1a\n" in d:
        try:  # Govee diydial / graffiti_2d scene (layered, with motion) -> pre-rendered frames
            import graffiti
            return [(tick, [r[:] for r in canvas]) for tick, canvas in graffiti.render(d)]
        except Exception as e:  # fall back to treating the PNGs as plain frames
            print(f"  (scene render failed: {e}; using plain frames)")
        for delay, rows in decode_govee_png_container(d):
            place(canvas, rows, alpha=True)  # transparent pixels keep the previous frame
            out.append((delay, [r[:] for r in canvas]))
    else:
        raise ValueError("unknown format")
    return out


def write_anim(path: pathlib.Path, frames) -> None:
    with open(path, "wb") as f:
        f.write(b"PXA1" + struct.pack("<H", len(frames)))
        for delay, fr in frames:
            f.write(struct.pack("<H", max(MIN_DELAY, min(MAX_DELAY, int(delay)))))
            f.write(bytes(c for row in fr for px in row for c in px))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--name", help="output name (single input only)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for inp in a.inputs:
        p = pathlib.Path(inp)
        name = a.name if a.name and len(a.inputs) == 1 else p.stem
        try:
            frames = convert(p)
        except Exception as e:  # keep going on bad files
            print(f"SKIP {p}: {e}")
            continue
        dst = pathlib.Path(a.out) / f"{name}.anim"
        write_anim(dst, frames)
        total = sum(min(MAX_DELAY, max(MIN_DELAY, x[0])) for x in frames)
        print(f"ok  {name}: {len(frames)} frames, {total / 1000:.1f}s, {dst.stat().st_size // 1024} KB")


if __name__ == "__main__":
    main()
