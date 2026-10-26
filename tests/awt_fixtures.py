#!/usr/bin/env python3
# awt_fixtures.py <dir>: PNG fixtures of every colour type / bit depth / filter / interlace, written independently of the code under test, with <name>.expect = the ARGB value of every pixel (hex).
import os, random, struct, sys, zlib
d = sys.argv[1]; os.makedirs(d, exist_ok=True); rnd = random.Random(1234)
def chunk(t, data): return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)
def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c
def filt(rows, bpp, ftype_for_row):
    out = bytearray(); prev = bytes(len(rows[0]))
    for i, row in enumerate(rows):
        f = ftype_for_row(i); out.append(f); cur = bytearray()
        for j, x in enumerate(row):
            a = row[j - bpp] if j >= bpp else 0; b = prev[j]; c = prev[j - bpp] if j >= bpp else 0
            pr = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]; cur.append((x - pr) & 255)
        out += cur; prev = row
    return bytes(out)
def pack(samples, depth):   # list of ints -> bytes (row)
    if depth == 8: return bytes(samples)
    if depth == 16: return b"".join(struct.pack(">H", s) for s in samples)
    per = 8 // depth; out = bytearray()
    for i in range(0, len(samples), per):
        v = 0
        for k in range(per):
            s = samples[i + k] if i + k < len(samples) else 0; v |= s << (8 - depth * (k + 1))
        out.append(v)
    return bytes(out)
def make(name, w, h, ctype, depth, interlace, trns=None, plte=None):
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]; mx = (1 << depth) - 1
    pix = [[[rnd.randint(0, mx) if not (ctype == 3) else rnd.randrange(len(plte) // 3) for _ in range(ch)] for _ in range(w)] for _ in range(h)]
    bpp = max(1, ch * depth // 8)
    def argb(p):
        s8 = lambda v: v if depth == 8 else v >> 8 if depth == 16 else v * 255 // mx
        if ctype == 0: a = 0 if trns is not None and p[0] == trns[0] else 255; g = s8(p[0]); return (a << 24) | (g << 16) | (g << 8) | g
        if ctype == 2: a = 0 if trns is not None and tuple(p) == tuple(trns) else 255; return (a << 24) | (s8(p[0]) << 16) | (s8(p[1]) << 8) | s8(p[2])
        if ctype == 3: i = p[0]; a = trns[i] if trns is not None and i < len(trns) else 255; return (a << 24) | (plte[3 * i] << 16) | (plte[3 * i + 1] << 8) | plte[3 * i + 2]
        if ctype == 4: g = s8(p[0]); return (s8(p[1]) << 24) | (g << 16) | (g << 8) | g
        return (s8(p[3]) << 24) | (s8(p[0]) << 16) | (s8(p[1]) << 8) | s8(p[2])
    def rowbytes(ys, xs): return [pack([s for x in xs for s in pix[y][x]], depth) for y in ys]
    raw = b""
    if interlace:
        for x0, y0, dx, dy in [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]:
            ys = list(range(y0, h, dy)); xs = list(range(x0, w, dx))
            if not ys or not xs: continue
            raw += filt(rowbytes(ys, xs), bpp, lambda i: rnd.randint(0, 4))
    else: raw = filt(rowbytes(range(h), range(w)), bpp, lambda i: rnd.randint(0, 4))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 1 if interlace else 0))
    if plte: png += chunk(b"PLTE", bytes(plte))
    if trns is not None:
        png += chunk(b"tRNS", bytes(trns) if ctype == 3 else b"".join(struct.pack(">H", t) for t in (trns if ctype == 2 else trns)))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(os.path.join(d, name + ".png"), "wb").write(png)
    open(os.path.join(d, name + ".expect"), "w").write(" ".join("%08x" % argb(pix[y][x]) for y in range(h) for x in range(w)) + "\n")
pal = [rnd.randint(0, 255) for _ in range(3 * 16)]
make("rgba8", 37, 23, 6, 8, False); make("rgba8_i", 37, 23, 6, 8, True); make("rgb8", 20, 9, 2, 8, False); make("rgb8_i", 31, 13, 2, 8, True)
make("rgba16", 9, 7, 6, 16, False); make("rgb16", 9, 7, 2, 16, True); make("ga8", 11, 5, 4, 8, False); make("ga16_i", 11, 5, 4, 16, True)
for dp in (1, 2, 4, 8, 16): make("gray%d" % dp, 13, 6, 0, dp, False); make("gray%d_i" % dp, 13, 6, 0, dp, True)
make("gray8_key", 13, 6, 0, 8, False, trns=[7]); make("rgb8_key", 13, 6, 2, 8, False, trns=[3, 4, 5])
for dp, n in ((1, 2), (2, 4), (4, 16), (8, 16)): make("pal%d" % dp, 17, 11, 3, dp, False, plte=pal[:3 * n]); make("pal%d_i" % dp, 17, 11, 3, dp, True, plte=pal[:3 * n])
make("pal8_trns", 17, 11, 3, 8, False, plte=pal, trns=[0, 40, 255, 128])
make("one", 1, 1, 6, 8, False); make("wide", 300, 2, 6, 8, True)
