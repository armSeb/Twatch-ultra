#!/usr/bin/env python3
"""Construit assets/emoji.bin (partition « emoji ») depuis les PNG de Twemoji.

    TWEMOJI_DIR=…/twemoji/assets/72x72 python3 tools/gen_emoji.py

Twemoji : https://github.com/jdecked/twemoji, graphismes CC BY 4.0.
Besoins : Pillow.

Contenu : chaque émoji « de base » (sans ton de peau : la montre affiche la
variante jaune), les drapeaux, les keycaps et les séquences ZWJ, en deux tailles
(SIZES). Chaque image est au format LVGL RGB565A8 (plan couleur RGB565 puis
plan alpha), compressée en deflate brut (décodée par le tinfl de la ROM).

Format (petit-boutiste) :
  en-tête  : 'EMJ1', u16 nb, u8 nb_tailles, u8 version, u8 tailles[4], u32 off_seq, u32 off_img
  entrées  : nb × { u32 seq (décalage dans la zone des séquences), u8 n_cp,
                     u8 res[3], { u32 décalage, u32 longueur }[nb_tailles] dans la zone des images }
             triées par séquence (comparaison lexicographique des points de code)
  séquences: points de code u32, FE0F retirés
  images   : flux deflate bruts
"""
import os, struct, sys, zlib
from PIL import Image

SRC = os.environ.get("TWEMOJI_DIR") or sys.exit("TWEMOJI_DIR=…/assets/72x72")
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "assets", "emoji.bin")
SIZES = (20, 28)
SKIN = set(range(0x1F3FB, 0x1F400))


def seq_of(name):
    return [int(h, 16) for h in name.split("-") if int(h, 16) != 0xFE0F]


def pack(im):
    rgb, alpha = bytearray(), bytearray()
    for r, g, b, a in im.get_flattened_data() if hasattr(im, "get_flattened_data") else im.getdata():
        c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3) if a else 0
        rgb += struct.pack("<H", c)
        alpha.append(a)
    z = zlib.compressobj(9, zlib.DEFLATED, -15)
    return z.compress(bytes(rgb + alpha)) + z.flush()


def main():
    items = []
    for fn in sorted(os.listdir(SRC)):
        if not fn.endswith(".png"):
            continue
        seq = seq_of(fn[:-4])
        if any(c in SKIN for c in seq):
            continue
        items.append((seq, os.path.join(SRC, fn)))
    items.sort(key=lambda t: t[0])
    seq_blob, img_blob, entries = bytearray(), bytearray(), []
    for seq, path in items:
        src = Image.open(path).convert("RGBA")
        offs = []
        for s in SIZES:
            z = pack(src.resize((s, s), Image.LANCZOS))
            offs.append((len(img_blob), len(z)))
            img_blob += z
        entries.append((len(seq_blob), len(seq), offs))
        seq_blob += b"".join(struct.pack("<I", c) for c in seq)
    ns = len(SIZES)
    ent_sz = 8 + 8 * ns
    hdr_sz = 4 + 2 + 1 + 1 + 4 + 4 + 4
    off_seq = hdr_sz + ent_sz * len(entries)
    off_img = off_seq + len(seq_blob)
    out = bytearray(b"EMJ1")
    out += struct.pack("<HBB", len(entries), ns, 1)
    out += bytes(list(SIZES) + [0] * (4 - ns))
    out += struct.pack("<II", off_seq, off_img)
    for so, n, offs in entries:
        out += struct.pack("<IB3x", so, n) + b"".join(struct.pack("<II", o, l) for o, l in offs)
    out += seq_blob + img_blob
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(out)
    raw = sum(s * s * 3 for s in SIZES) * len(entries)
    print(f"{len(entries)} émojis, tailles {SIZES}, {len(out) / 1024:.0f} Ko (brut {raw / 1024:.0f} Ko)")


main()
