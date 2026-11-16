"""Minimal SquashFS 4.0 reader and writer (Python standard library only).

Reads the H6631's stock root filesystem (xz or gzip, with fragments, duplicates and export table) into a tree of
Node objects, and writes a tree back out as an xz SquashFS image that the device's Linux 5.4 kernel mounts.

The writer is deliberately simple: no fragments (file tails are stored as their own small blocks), no xattrs,
no export table, every file owned by root (like `mksquashfs -all-root -no-xattrs -no-fragments`). Identical files
are stored once.
"""
import hashlib
import lzma
import struct
import time
import zlib
from concurrent.futures import ThreadPoolExecutor

MAGIC = 0x73717368
META = 8192
NONE64 = 0xFFFFFFFFFFFFFFFF
NONE32 = 0xFFFFFFFF
UNCOMP_BLOCK = 1 << 24

# basic inode types (extended = basic + 7)
DIR, FILE, SYMLINK, BLKDEV, CHRDEV, FIFO, SOCKET = range(1, 8)
KIND_TYPE = {"dir": DIR, "file": FILE, "symlink": SYMLINK, "blk": BLKDEV, "chr": CHRDEV, "fifo": FIFO, "sock": SOCKET}
TYPE_KIND = {v: k for k, v in KIND_TYPE.items()}


class Node:
    __slots__ = ("kind", "mode", "mtime", "data", "target", "rdev", "children")

    def __init__(self, kind, mode=0o755, mtime=0, data=b"", target=b"", rdev=(0, 0)):
        self.kind = kind
        self.mode = mode & 0o7777
        self.mtime = mtime
        self.data = data          # file contents (bytes)
        self.target = target      # symlink target (bytes)
        self.rdev = rdev          # (major, minor) for devices
        self.children = {} if kind == "dir" else None   # name (bytes) -> Node

    # --- path helpers (paths like "/etc/init.d/rc.final")
    def lookup(self, path):
        n = self
        for part in [p for p in path.split("/") if p]:
            if n.kind != "dir" or part.encode() not in n.children:
                return None
            n = n.children[part.encode()]
        return n

    def put(self, path, node):
        parts = [p for p in path.split("/") if p]
        d = self.lookup("/".join(parts[:-1]))
        if d is None or d.kind != "dir":
            raise KeyError("no directory for " + path)
        d.children[parts[-1].encode()] = node
        return node

    def remove(self, path):
        parts = [p for p in path.split("/") if p]
        d = self.lookup("/".join(parts[:-1]))
        if d is not None and d.kind == "dir":
            d.children.pop(parts[-1].encode(), None)

    def walk(self, prefix=""):
        """yield (path, node) for this tree, parents before children."""
        yield prefix or "/", self
        if self.kind == "dir":
            for name in sorted(self.children):
                yield from self.children[name].walk(prefix + "/" + name.decode("utf-8", "surrogateescape"))


def _decode_dev(v):
    return ((v >> 8) & 0xFFF, (v & 0xFF) | ((v >> 12) & 0xFFF00))


def _encode_dev(major, minor):
    return (minor & 0xFF) | (major << 8) | ((minor & ~0xFF) << 12)


# ------------------------------------------------------------------------------------------------------- reader
class _Image:
    def __init__(self, img: bytes):
        self.img = img
        sb = struct.unpack_from("<IIIIIHHHHHHQQQQQQQQ", img, 0)
        (magic, self.inode_count, self.mtime, self.block_size, self.frag_count, self.comp, self.block_log,
         self.flags, self.id_count, vmaj, vmin, self.root_ref, self.bytes_used, self.id_tab, self.xattr_tab,
         self.inode_tab, self.dir_tab, self.frag_tab, self.export_tab) = sb
        if magic != MAGIC:
            raise ValueError("not a SquashFS image")
        if (vmaj, vmin) != (4, 0):
            raise ValueError(f"unsupported SquashFS version {vmaj}.{vmin}")
        if self.comp not in (1, 4):
            raise ValueError(f"unsupported compression id {self.comp} (need gzip or xz)")
        # table boundaries: a metadata table ends where the next structure starts
        bounds = {self.inode_tab, self.dir_tab, self.bytes_used}
        for t, n in ((self.frag_tab, self.frag_count), (self.export_tab, self.inode_count), (self.id_tab, self.id_count)):
            if t != NONE64 and n:
                bounds.add(t)
                bounds.add(struct.unpack_from("<Q", img, t)[0])
        self.bounds = sorted(bounds)
        self.inodes = self._table(self.inode_tab)
        self.dirs = self._table(self.dir_tab)
        self.ids = self._lookup_table(self.id_tab, self.id_count, 4, "<I")
        self.frags = self._lookup_table(self.frag_tab, self.frag_count, 16, "<QII") if self.frag_count else []
        self._frag_cache = {}

    def decompress(self, data, out_max):
        if self.comp == 4:
            return lzma.decompress(data)
        return zlib.decompress(data)

    def _meta(self, pos):
        (h,) = struct.unpack_from("<H", self.img, pos)
        size = h & 0x7FFF
        raw = self.img[pos + 2:pos + 2 + size]
        data = raw if h & 0x8000 else self.decompress(raw, META)
        return data, pos + 2 + size

    def _table(self, start):
        end = min(b for b in self.bounds if b > start)
        stream, index, pos = bytearray(), {}, start
        while pos < end:
            index[pos - start] = len(stream)
            data, pos = self._meta(pos)
            stream += data
        return bytes(stream), index

    def _lookup_table(self, start, count, size, fmt):
        nblocks = (count * size + META - 1) // META
        ptrs = struct.unpack_from(f"<{nblocks}Q", self.img, start)
        raw = bytearray()
        for p in ptrs:
            raw += self._meta(p)[0]
        return [struct.unpack_from(fmt, raw, i * size) for i in range(count)]

    @staticmethod
    def _pos(table, block, offset):
        stream, index = table
        return index[block] + offset

    def read_inode(self, ref):
        s = self.inodes[0]
        p = self._pos(self.inodes, ref >> 16, ref & 0xFFFF)
        itype, mode, uid, gid, mtime, ino = struct.unpack_from("<HHHHII", s, p)
        p += 16
        base = itype if itype <= 7 else itype - 7
        info = {"type": base, "mode": mode, "mtime": mtime, "ino": ino, "nlink": 1}
        if itype == DIR:
            blk, nlink, fsize, off, parent = struct.unpack_from("<IIHHI", s, p)
            info.update(dir_block=blk, dir_off=off, dir_size=fsize, nlink=nlink)
        elif itype == DIR + 7:
            nlink, fsize, blk, parent, icount, off, xattr = struct.unpack_from("<IIIIHHI", s, p)
            info.update(dir_block=blk, dir_off=off, dir_size=fsize, nlink=nlink)
        elif itype in (FILE, FILE + 7):
            if itype == FILE:
                start, frag, foff, size = struct.unpack_from("<IIII", s, p)
                p += 16
            else:
                start, size, sparse, nlink, frag, foff, xattr = struct.unpack_from("<QQQIIII", s, p)
                p += 40
                info["nlink"] = nlink
            bs = self.block_size
            nblocks = size // bs if frag != NONE32 else (size + bs - 1) // bs
            sizes = struct.unpack_from(f"<{nblocks}I", s, p)
            info.update(start=start, size=size, frag=frag, frag_off=foff, sizes=sizes)
        elif itype in (SYMLINK, SYMLINK + 7):
            nlink, tsize = struct.unpack_from("<II", s, p)
            info.update(nlink=nlink, target=s[p + 8:p + 8 + tsize])
        elif itype in (BLKDEV, CHRDEV, BLKDEV + 7, CHRDEV + 7):
            nlink, dev = struct.unpack_from("<II", s, p)
            info.update(nlink=nlink, rdev=_decode_dev(dev))
        elif itype in (FIFO, SOCKET, FIFO + 7, SOCKET + 7):
            info["nlink"] = struct.unpack_from("<I", s, p)[0]
        else:
            raise ValueError(f"unknown inode type {itype}")
        return info

    def read_file(self, info):
        bs, out, pos = self.block_size, bytearray(), info["start"]
        for e in info["sizes"]:
            n = e & ~UNCOMP_BLOCK
            if n == 0:  # sparse block
                out += bytes(min(bs, info["size"] - len(out)))
                continue
            raw = self.img[pos:pos + n]
            out += raw if e & UNCOMP_BLOCK else self.decompress(raw, bs)
            pos += n
        if info["frag"] != NONE32:
            fstart, fsize, _ = self.frags[info["frag"]]
            blk = self._frag_cache.get(info["frag"])
            if blk is None:
                n = fsize & ~UNCOMP_BLOCK
                raw = self.img[fstart:fstart + n]
                blk = raw if fsize & UNCOMP_BLOCK else self.decompress(raw, bs)
                self._frag_cache[info["frag"]] = blk
            tail = info["size"] - len(out)
            out += blk[info["frag_off"]:info["frag_off"] + tail]
        if len(out) != info["size"]:
            raise ValueError("file size mismatch")
        return bytes(out)

    def read_dir(self, info):
        s = self.dirs[0]
        p = self._pos(self.dirs, info["dir_block"], info["dir_off"])
        end = p + info["dir_size"] - 3
        entries = []
        while p < end:
            count, start, base = struct.unpack_from("<III", s, p)
            p += 12
            for _ in range(count + 1):
                off, delta, _t, nsize = struct.unpack_from("<HhHH", s, p)
                name = s[p + 8:p + 9 + nsize]
                p += 9 + nsize
                entries.append((name, (start << 16) | off))
        return entries


def read(img: bytes) -> Node:
    """SquashFS image bytes -> Node tree (hard links become shared Node objects)."""
    im = _Image(img)
    seen = {}

    def build(ref):
        info = im.read_inode(ref)
        if info["ino"] in seen and info["type"] != DIR:
            return seen[info["ino"]]
        kind = TYPE_KIND[info["type"]]
        n = Node(kind, info["mode"], info["mtime"])
        if kind == "dir":
            for name, cref in im.read_dir(info):
                n.children[name] = build(cref)
        elif kind == "file":
            n.data = im.read_file(info)
        elif kind == "symlink":
            n.target = info["target"]
        elif kind in ("chr", "blk"):
            n.rdev = info["rdev"]
        seen[info["ino"]] = n
        return n

    return build(im.root_ref)


# ------------------------------------------------------------------------------------------------------- writer
def _xz(block_size, preset=6):
    filters = [{"id": lzma.FILTER_LZMA2, "preset": preset, "dict_size": block_size}]
    return lambda b: lzma.compress(b, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, filters=filters)


class _MetaWriter:
    def __init__(self, compress):
        self.compress, self.out, self.cur = compress, bytearray(), bytearray()

    def ref(self):
        return len(self.out), len(self.cur)

    def add(self, b):
        r = self.ref()
        self.cur += b
        while len(self.cur) >= META:
            self._emit(bytes(self.cur[:META]))
            del self.cur[:META]
        return r

    def _emit(self, block):
        c = self.compress(block)
        if len(c) < len(block):
            self.out += struct.pack("<H", len(c)) + c
        else:
            self.out += struct.pack("<H", len(block) | 0x8000) + block

    def finish(self):
        if self.cur:
            self._emit(bytes(self.cur))
            self.cur = bytearray()
        return bytes(self.out)


def write(root: Node, block_size=262144, mtime=None, preset=6, workers=None) -> bytes:
    """Node tree -> xz SquashFS 4.0 image (padded to 4 KiB)."""
    compress = _xz(block_size, preset)
    mtime = int(time.time()) if mtime is None else mtime

    # 1. inode numbers, post-order (children before their directory; the root gets the highest number)
    order, number = [], {}

    def number_tree(n):
        if id(n) in number:
            return
        if n.kind == "dir":
            for name in sorted(n.children):
                number_tree(n.children[name])
        number[id(n)] = len(order) + 1
        order.append(n)

    number_tree(root)
    nlinks = {}
    for n in order:
        if n.kind == "dir":
            for c in n.children.values():
                if c.kind != "dir":
                    nlinks[id(c)] = nlinks.get(id(c), 0) + 1

    # 2. data blocks (compressed in parallel; identical files stored once)
    out = bytearray(96)
    files = [n for n in order if n.kind == "file"]
    unique, layout = {}, {}
    for n in files:
        unique.setdefault(hashlib.sha256(n.data).digest(), n)
    jobs = []
    for h, n in unique.items():
        for i in range(0, len(n.data), block_size):
            jobs.append((h, n.data[i:i + block_size]))
    with ThreadPoolExecutor(workers) as ex:
        packed = list(ex.map(lambda j: (j[0], j[1], compress(j[1])), jobs))
    for h, raw, c in packed:
        start, sizes = layout.setdefault(h, (len(out), []))
        if len(c) < len(raw):
            out += c
            sizes.append(len(c))
        else:
            out += raw
            sizes.append(len(raw) | UNCOMP_BLOCK)

    # 3. inode + directory tables
    inodes, dirs = _MetaWriter(compress), _MetaWriter(compress)
    refs = {}
    parent = {id(root): len(order) + 1}
    for n in order:
        if n.kind == "dir":
            for c in n.children.values():
                if c.kind == "dir":
                    parent[id(c)] = number[id(n)]

    def header(t, n):
        return struct.pack("<HHHHII", t, n.mode, 0, 0, n.mtime, number[id(n)])

    for n in order:
        ino = number[id(n)]
        nl = nlinks.get(id(n), 1)
        if n.kind == "dir":
            names = sorted(n.children)
            listing, i = bytearray(), 0
            while i < len(names):
                first = n.children[names[i]]
                blk = refs[id(first)][0]
                base = number[id(first)]
                group = []
                while i < len(names) and len(group) < 256:
                    c = n.children[names[i]]
                    r = refs[id(c)]
                    d = number[id(c)] - base
                    if r[0] != blk or not -32768 <= d <= 32767:
                        break
                    group.append((names[i], c, r, d))
                    i += 1
                listing += struct.pack("<III", len(group) - 1, blk, base)
                for name, c, r, d in group:
                    listing += struct.pack("<HhHH", r[1], d, KIND_TYPE[c.kind], len(name) - 1) + name
            dblk, doff = dirs.ref()
            dirs.add(bytes(listing))
            fsize = len(listing) + 3
            nsub = sum(1 for c in n.children.values() if c.kind == "dir")
            if fsize <= 0xFFFF:
                body = header(DIR, n) + struct.pack("<IIHHI", dblk, 2 + nsub, fsize, doff, parent[id(n)])
            else:
                body = header(DIR + 7, n) + struct.pack("<IIIIHHI", 2 + nsub, fsize, dblk, parent[id(n)], 0, doff, NONE32)
        elif n.kind == "file":
            h = hashlib.sha256(n.data).digest()
            start, sizes = layout.get(h, (0, []))
            if nl == 1 and start < 2 ** 32 and len(n.data) < 2 ** 32:
                body = header(FILE, n) + struct.pack("<IIII", start, NONE32, 0, len(n.data))
            else:
                body = header(FILE + 7, n) + struct.pack("<QQQIIII", start, len(n.data), 0, nl, NONE32, 0, NONE32)
            body += struct.pack(f"<{len(sizes)}I", *sizes)
        elif n.kind == "symlink":
            body = header(SYMLINK, n) + struct.pack("<II", nl, len(n.target)) + n.target
        elif n.kind in ("chr", "blk"):
            body = header(KIND_TYPE[n.kind], n) + struct.pack("<II", nl, _encode_dev(*n.rdev))
        else:
            body = header(KIND_TYPE[n.kind], n) + struct.pack("<I", nl)
        refs[id(n)] = inodes.add(body)

    root_blk, root_off = refs[id(root)]
    inode_tab = len(out)
    out += inodes.finish()
    dir_tab = len(out)
    out += dirs.finish()
    frag_tab = len(out)          # no fragments
    id_block = len(out)          # one id (0 = root)
    ids = _MetaWriter(compress)
    ids.add(struct.pack("<I", 0))
    out += ids.finish()
    id_tab = len(out)
    out += struct.pack("<Q", id_block)
    bytes_used = len(out)
    flags = 0x0010 | 0x0200      # NO_FRAGMENTS | NO_XATTRS
    struct.pack_into("<IIIIIHHHHHHQQQQQQQQ", out, 0, MAGIC, len(order), mtime, block_size, 0, 4,
                     block_size.bit_length() - 1, flags, 1, 4, 0, (root_blk << 16) | root_off, bytes_used,
                     id_tab, NONE64, inode_tab, dir_tab, frag_tab, NONE64)
    out += bytes(-len(out) % 4096)
    return bytes(out)
