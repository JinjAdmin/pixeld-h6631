"""Render Govee "diydial" / "graffiti_2d" scene files to plain frame lists.

Format (reverse-engineered from govee_app 1.00.91, functions 0x3446c, 0x349a0, 0x5011c, 0x4e48c, 0x4f28c):

diydial wrapper (optional):  [layer_count u8] then per layer [type u8][size u32 LE][data]
    type 1 = GIF (LVGL), type 2 = graffiti_2d payload (below), type 3 = 20-byte clock/overlay config
graffiti_2d payload:
    [bg_r][bg_g][bg_b]   background colour (01 01 01 = none)
    [b3]                 unknown (0x01 usually, 0x61 in clock themes)
    [type]               0xff for these assets; non-zero => middle group are timed frames
    3 groups in order: backgrounds (<=5), layers/frames (<=100), foregrounds (<=5)
    group = [count u8][item]*count
    item  = [u16 reclen][subtype u8 (0 = PNG)][u16 png_len][PNG]
            [K][A][B][C][u16 D][E][pad]
      K = motion of the source offset: 1 = image moves down, 2 up, 3 right, 4 left, 5..8 diagonals; wraps modulo
          the image size, so the image tiles and scrolls seamlessly
      A = speed 0-100 (0 = does not move, even with K set), B = 0-100 (opacity/brightness?), C = 1-based index, D = frame duration ms
Compositing: background colour, then backgrounds, then the current frame of the middle group,
then foregrounds; within every group item 1 is the top (drawn last); black (0,0,0) and fully transparent pixels are see-through.
"""
import struct

from minipng import read_png

W, H = 52, 32
# Motion direction K -> screen direction (dx, dy) in timed ("combo", type != 0) scenes. Govee moves the *source
# offset* there, so the picture travels opposite to the offset change: K=1 moves the image DOWN. Confirmed on the
# panel: Gliding (parachute, type 0xff, K=1) must fall.
# Plain stacked scenes (type 0) move the other way: K=1 moves UP. Confirmed with Govee-app scenes: smoke (K=1) must
# rise out of a pipe, leaves (K=8) must fall. Item.flip marks type-0 items.
DX = {0: (0, 0), 1: (0, 1), 2: (0, -1), 3: (1, 0), 4: (-1, 0), 5: (1, 1), 6: (-1, 1), 7: (1, -1), 8: (-1, -1)}


def direction(K, flip=False):
    dx, dy = DX.get(K, (0, 0))
    return (-dx, -dy) if flip else (dx, dy)


class Item:
    def __init__(self, rows, K, A, B, C, D, E):
        self.rows, self.K, self.A, self.B, self.C, self.D, self.E = rows, K, A, B, C, D, E
        self.h, self.w = len(rows), len(rows[0])
        self.flip = False   # set for type-0 scenes (see DX)

    def step_ms(self):
        # speed 0-100 -> ms per pixel step (tuned by eye; 100 = fastest)
        return max(25, 25 + (100 - self.A) * 3)


def parse_graffiti(d: bytes):
    bg = tuple(d[0:3])
    if bg == (1, 1, 1):
        bg = (0, 0, 0)
    gtype = d[4]
    pos = 5
    groups = []
    for g in range(3):
        n = d[pos]; pos += 1
        items = []
        for _ in range(n):
            sub = d[pos + 2]
            if sub != 0:
                raise ValueError(f"unsupported item subtype {sub} at {pos}")
            plen = d[pos + 3] | (d[pos + 4] << 8)
            png = d[pos + 5:pos + 5 + plen]
            pos += 5 + plen
            K, A, B, C = d[pos], d[pos + 1], d[pos + 2], d[pos + 3]
            D = d[pos + 4] | (d[pos + 5] << 8)
            E = d[pos + 6]
            pos += 8
            it = Item(read_png(png)[2], K, A, B, C, D, E)
            it.flip = gtype == 0
            items.append(it)
        groups.append(items)
    return bg, gtype, groups


def unwrap(d: bytes):
    """Return the graffiti payload from a diydial file (or the file itself if it is a bare payload)."""
    n = d[0]
    if 1 <= n <= 8 and d[1] in (1, 2, 3):
        pos, payload = 1, None
        for _ in range(n):
            t = d[pos]; size = struct.unpack('<I', d[pos + 1:pos + 5])[0]
            if pos + 5 + size > len(d):
                break
            if t == 2:
                payload = d[pos + 5:pos + 5 + size]
            pos += 5 + size
        if payload is not None:
            return payload
    return d


def _draw(canvas, it: Item, ox: int, oy: int, tile: bool):
    for y in range(H):
        sy = y - oy
        if tile:
            sy %= it.h
        elif not 0 <= sy < it.h:
            continue
        row = it.rows[sy]
        for x in range(W):
            sx = x - ox
            if tile:
                sx %= it.w
            elif not 0 <= sx < it.w:
                continue
            p = row[sx]
            a = (p[3] if len(p) > 3 else 255) * min(100, it.B) // 100   # B = opacity %
            if a == 0 or (p[0] == 0 and p[1] == 0 and p[2] == 0):
                continue
            if a == 255:
                canvas[y][x] = (p[0], p[1], p[2])
            else:
                q = canvas[y][x]
                canvas[y][x] = tuple((p[c] * a + q[c] * (255 - a)) // 255 for c in range(3))


class Track:
    """Continuous scrolling motion for one item (or a whole timed frame group)."""

    def __init__(self, K, span, rate, flip=False):
        self.dx, self.dy = direction(K, flip)
        self.span, self.rate = span, rate      # rate in px per ms

    def period(self):
        return self.span / self.rate if self.rate else 0

    def fit(self, loop_ms):
        # adjust the speed slightly so the motion wraps a whole number of times per loop (seamless loop)
        if self.rate:
            wraps = max(1, round(loop_ms * self.rate / self.span))
            self.rate = wraps * self.span / loop_ms

    def pos(self, t):
        k = int(t * self.rate)
        return self.dx * k, self.dy * k


def _track_for(items):
    moving = [it for it in items if it.K and it.A]   # speed 0 = stationary, whatever the direction
    if not moving or len(moving) * 2 < len(items):
        return Track(0, 1, 0)
    it0 = moving[0]
    dx, dy = direction(it0.K, it0.flip)
    span = max(it0.w if dx else 0, it0.h if dy else 0)
    rate = sum(1.0 / it.step_ms() for it in moving) / len(moving)
    return Track(it0.K, span, rate, it0.flip)


def render(d: bytes, tick_ms: int = 33, max_frames: int = 450):
    bg, gtype, (bgs, layers, fgs) = parse_graffiti(unwrap(d))
    timed = bool(gtype)  # middle group = frame sequence
    seq_ms = sum(max(20, it.D) for it in layers) if timed and layers else 0
    tracks = {}
    for it in bgs + fgs + ([] if timed else layers):
        tracks[id(it)] = _track_for([it])
    group = _track_for(layers) if timed and layers else None
    all_tracks = list(tracks.values()) + ([group] if group else [])
    # loop length: long enough for the slowest mover to wrap once, a whole number of frame
    # sequences and blink cycles; every mover is then tuned to wrap a whole number of times
    length = max([seq_ms] + [tr.period() for tr in all_tracks])
    blink = [2 * max(20, it.D) for it in layers if it.E == 1 and not timed]
    for b in blink:
        length = max(length, b)
    if length <= 0:
        length = 1000
    unit = seq_ms or (max(blink) if blink else 0)
    if unit:
        length = -(-int(length) // unit) * unit
    for tr in all_tracks:
        tr.fit(length)
    if timed and layers:                     # sample timed frame sequences at least at their own rate
        tick_ms = max(20, min(tick_ms, min(max(20, it.D) for it in layers)))
    n = int(min(max_frames, max(1, length // tick_ms)))
    tick = length / n
    out = []
    for f in range(n):
        t = f * tick
        canvas = [[bg] * W for _ in range(H)]

        def pos(it):
            tr = tracks.get(id(it)) or group
            return tr.pos(t) if tr else (0, 0)

        # like the layer group, item 1 is the top: draw backgrounds/foregrounds last-to-first
        for it in reversed(bgs):
            ox, oy = pos(it)
            _draw(canvas, it, ox, oy, tile=True)
        if layers:
            if timed and seq_ms:
                tt = t % seq_ms
                cur = layers[-1]
                for it in layers:
                    if tt < max(20, it.D):
                        cur = it
                        break
                    tt -= max(20, it.D)
                ox, oy = pos(cur)
                _draw(canvas, cur, ox, oy, tile=bool(cur.K))
            else:
                # stacked scene: layer 1 is the top, so draw from the last layer up;
                # E=1 layers blink (visible for D ms, hidden for D ms)
                for it in reversed(layers):
                    if it.E == 1 and (t // max(20, it.D)) % 2:
                        continue
                    ox, oy = pos(it)
                    _draw(canvas, it, ox, oy, tile=bool(it.K))
        for it in reversed(fgs):
            ox, oy = pos(it)
            _draw(canvas, it, ox, oy, tile=bool(it.K))
        # frame delays as whole ms that add up exactly to the loop length
        out.append((int(round((f + 1) * tick)) - int(round(f * tick)), canvas))
    return out


def describe(d: bytes) -> str:
    bg, gtype, groups = parse_graffiti(unwrap(d))
    s = [f"bg={bg} type={gtype:#x}"]
    for name, items in zip(("bg", "layer", "fg"), groups):
        for it in items:
            s.append(f"  {name}{it.C}: {it.w}x{it.h} K={it.K} A={it.A} B={it.B} D={it.D} E={it.E}")
    return "\n".join(s)
