"""Tiny dependency-free PNG reader/writer (8-bit, non-interlaced; gray/RGB/palette/alpha)."""
import struct
import zlib


def read_png(data: bytes):
    """Return (width, height, rows) where rows is a list of lists of (r, g, b, a)."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
        pos += 12 + ln
    assert depth == 8 and interlace == 0, f"unsupported PNG depth={depth} interlace={interlace}"
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        row = []
        for x in range(w):
            px = line[x * ch:(x + 1) * ch]
            if ctype == 0: row.append((px[0], px[0], px[0], 255))
            elif ctype == 2: row.append((px[0], px[1], px[2], 255))
            elif ctype == 3:
                r, g, b = plte[px[0]]
                row.append((r, g, b, trns[px[0]] if trns and px[0] < len(trns) else 255))
            elif ctype == 4: row.append((px[0], px[0], px[0], px[1]))
            else: row.append(tuple(px))
        rows.append(row)
    return w, h, rows


def write_png(path: str, w: int, h: int, rows) -> None:
    """rows: list of lists of (r, g, b)."""
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px[:3]) for row in rows)

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def to_rgb_frame(rows, bg=(0, 0, 0)) -> bytes:
    """Flatten RGBA rows onto a background colour -> raw RGB bytes (row-major)."""
    out = bytearray()
    for row in rows:
        for r, g, b, a in row:
            out += bytes((
                (r * a + bg[0] * (255 - a)) // 255,
                (g * a + bg[1] * (255 - a)) // 255,
                (b * a + bg[2] * (255 - a)) // 255,
            ))
    return bytes(out)
