#!/usr/bin/env python3
"""Fabrique les PNG de test de test_png.c (Pillow) : un par format de tuile,
avec l'image attendue en RGB565 (fond OSM sous la transparence)."""
import os, random, struct, sys
from PIL import Image

OUT = sys.argv[1]
BG = (0xF2, 0xEF, 0xE9)
random.seed(7)

def pattern(size, mode):
    im = Image.new("RGBA", (size, size))
    px = im.load()
    for y in range(size):
        for x in range(size):
            px[x, y] = ((x * 7 + y) & 255, (y * 5) & 255, (x ^ y) & 255,
                        255 if (x // 16 + y // 16) % 3 else (x * 3) & 255)
    return im

def expected(im, size):
    rgba = im.convert("RGBA")
    flat = Image.new("RGB", rgba.size, BG)
    flat.paste(rgba, mask=rgba.split()[3])
    if size == 512:
        # moyenne 2x2 exacte, comme le décodeur
        p = flat.load()
        out = Image.new("RGB", (256, 256))
        q = out.load()
        for y in range(256):
            for x in range(256):
                s = [p[2 * x + (k & 1), 2 * y + (k >> 1)] for k in range(4)]
                q[x, y] = tuple((sum(c[i] for c in s) + 2) // 4 for i in range(3))
        flat = out
    data = bytearray()
    for r, g, b in flat.convert("RGB").tobytes() and zip(*[iter(flat.convert("RGB").tobytes())] * 3):
        data += struct.pack("<H", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    return bytes(data)

cases = []
base = pattern(256, "RGBA")
cases.append(("rgba8", base))
cases.append(("rgb8", base.convert("RGB")))
cases.append(("gray8", base.convert("L")))
cases.append(("graya8", base.convert("LA")))
cases.append(("pal8", base.convert("RGB").quantize(200)))
cases.append(("pal4", base.convert("RGB").quantize(16)))
cases.append(("pal1", base.convert("1").convert("P")))
cases.append(("bw1", base.convert("1")))
cases.append(("rgba512", pattern(512, "RGBA")))

for name, im in cases:
    path = os.path.join(OUT, name + ".png")
    kw = {}
    if name == "pal4": kw["bits"] = 4
    if name == "pal1": kw["bits"] = 1
    im.save(path, optimize=False, **kw)
    exp = expected(Image.open(path), im.size[0])
    open(os.path.join(OUT, name + ".565"), "wb").write(exp)
# palette avec transparence (tRNS)
im = base.convert("RGB").quantize(32)
p = os.path.join(OUT, "palt8.png")
im.save(p, transparency=bytes([0, 128] + [255] * 30))
open(os.path.join(OUT, "palt8.565"), "wb").write(expected(Image.open(p), 256))
# 16 bits par canal
im16 = Image.new("I;16", (256, 256))
im16.putdata([(x * 257 + y * 3) & 0xFFFF for y in range(256) for x in range(256)])
p = os.path.join(OUT, "gray16.png")
im16.save(p)
g8 = Image.new("L", (256, 256))
g8.putdata([((x * 257 + y * 3) & 0xFFFF) >> 8 for y in range(256) for x in range(256)])
open(os.path.join(OUT, "gray16.565"), "wb").write(expected(g8, 256))
print(len(cases) + 2, "PNG")
