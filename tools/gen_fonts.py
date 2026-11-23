#!/usr/bin/env python3
"""Régénère les polices de main/fonts/ et la table de couverture Unicode.

    LVGL_DIR=~/esp/ext/lvgl python3 tools/gen_fonts.py

Besoins : lv_font_conv (npm i -g lv_font_conv), fontTools (pip install fonttools).
LVGL_DIR = clone de lvgl v9.2.2 (scripts/built_in_font/ : Montserrat, DejaVu
Sans, Font Awesome).

Textes : Montserrat-Medium (latin étendu, vietnamien, cyrillique, ponctuation,
monnaies), complétée par DejaVu Sans pour le grec, les flèches, les maths, les
formes géométriques et les symboles. Les icônes Font Awesome sont celles de
LV_SYMBOL_* utilisées par le code et les SYM_* de main/ui_kit.h.

Le même jeu de plages produit main/fonts/font_cov.c : text_sanitize() garde
exactement ce que les polices de texte savent dessiner (les émojis sont
traités à part).
"""
import os, re, subprocess, sys
from fontTools.ttLib import TTFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUT = os.path.join(ROOT, "main", "fonts")
LV = os.environ.get("LVGL_DIR") or sys.exit("LVGL_DIR=chemin vers lvgl")
BF = os.path.join(LV, "scripts", "built_in_font")
MONT = os.path.join(BF, "Montserrat-Medium.ttf")
DEJA = os.path.join(BF, "DejaVuSans.ttf")
FA = os.path.join(BF, "FontAwesome5-Solid+Brands+Regular.woff")
# lv_font_conv lancé via node avec une pile plus grande (sinon RangeError sur ~2000 glyphes)
LVFC = subprocess.run(["sh", "-c", "readlink -f \"$(command -v lv_font_conv)\""], capture_output=True, text=True).stdout.strip()

# plages voulues (le contenu réel dépend de la police)
LATIN = [(0x20, 0x7E), (0xA0, 0x24F), (0x250, 0x2AF), (0x2B0, 0x2FF), (0x300, 0x36F),
         (0x400, 0x52F), (0x1E00, 0x1EFF), (0x2000, 0x206F), (0x20A0, 0x20C0)]
SYMBOLS = [(0x370, 0x3FF), (0x1F00, 0x1FFF), (0x2100, 0x214F), (0x2150, 0x218F), (0x2190, 0x21FF),
           (0x2200, 0x22FF), (0x2300, 0x23FF), (0x2460, 0x24FF), (0x25A0, 0x25FF), (0x2600, 0x26FF),
           (0x2700, 0x27BF), (0x27C0, 0x27EF), (0x2B00, 0x2BFF), (0xFB00, 0xFB06)]
GREEK = [(0x370, 0x3FF)]
# invisibles et marques de contrôle : jamais dessinés
SKIP = set(range(0x200B, 0x2010)) | set(range(0x2028, 0x202F)) | set(range(0x2060, 0x2070)) | {0xAD}

FA_BASE = [0xF001, 0xF00B, 0xF00C, 0xF00D, 0xF011, 0xF013, 0xF015, 0xF021, 0xF053, 0xF054, 0xF077,
           0xF078, 0xF0E7, 0xF0F3, 0xF1F6, 0xF240, 0xF241, 0xF242, 0xF243, 0xF244, 0xF293, 0xF2ED,
           0xF0E0, 0xF095, 0xF048, 0xF04B, 0xF04C, 0xF051, 0xF027, 0xF028, 0xF04D, 0xF026]


def cmap(path):
    return set(TTFont(path).getBestCmap())


def pick(cm, ranges, taken=()):
    return sorted(c for a, b in ranges for c in range(a, b + 1) if c in cm and c not in taken and c not in SKIP)


def ranges_of(cps):
    out, start, prev = [], None, None
    for c in cps:
        if start is None:
            start = prev = c
        elif c == prev + 1:
            prev = c
        else:
            out.append((start, prev)); start = prev = c
    if start is not None:
        out.append((start, prev))
    return out


def rarg(cps):
    return ",".join(f"0x{a:X}" if a == b else f"0x{a:X}-0x{b:X}" for a, b in ranges_of(cps))


def ui_icons():
    """SYM_* de ui_kit.h (séquences UTF-8 \\xEF\\x..\\x..)"""
    src = open(os.path.join(ROOT, "main", "ui_kit.h"), encoding="utf-8").read()
    cps = set()
    for m in re.finditer(r'"((?:\\x[0-9A-Fa-f]{2})+)"', src):
        b = bytes(int(h, 16) for h in re.findall(r'\\x([0-9A-Fa-f]{2})', m.group(1)))
        cps.update(ord(ch) for ch in b.decode("utf-8"))
    return sorted(cps | set(FA_BASE))


def conv(name, size, parts, fallback=None):
    cmd = ["node", "--stack-size=60000", LVFC, "--no-compress", "--no-prefilter", "--bpp", "4", "--size", str(size)]
    for font, cps in parts:
        if cps:
            cmd += ["--font", font, "-r", rarg(cps)]
    if fallback:
        cmd += ["--lv-fallback", fallback]          # émojis en couleur (main/emoji.c)
    cmd += ["--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", name, "-o",
            os.path.join(OUT, name + ".c")]
    subprocess.run(cmd, check=True)
    n = sum(len(c) for _, c in parts)
    print(f"{name}: {size} px, {n} glyphes")


def main():
    mc, dc = cmap(MONT), cmap(DEJA)
    latin = pick(mc, LATIN)
    sym = pick(dc, SYMBOLS, set(latin))
    greek = pick(dc, GREEK, set(latin))
    icons = ui_icons()
    text_cov = sorted(set(latin) | set(sym))

    conv("font_small", 17, [(MONT, latin), (DEJA, sym)], "emoji_font_20")
    conv("font_text", 24, [(MONT, latin), (DEJA, sym), (FA, icons)], "emoji_font_28")
    conv("font_title", 30, [(MONT, latin), (DEJA, greek), (FA, icons)], "emoji_font_28")
    conv("font_num", 66, [(MONT, [0x20, 0x25] + list(range(0x2B, 0x3B)))])
    conv("font_time", 108, [(MONT, list(range(0x30, 0x3B)))])

    rs = ranges_of(text_cov)
    with open(os.path.join(OUT, "font_cov.c"), "w", encoding="utf-8") as f:
        f.write("/* Généré par tools/gen_fonts.py : caractères dessinés par font_small et\n"
                " * font_text (font_title couvre le latin, le cyrillique et le grec). */\n")
        f.write('#include "font_cov.h"\n\n')
        f.write("static const uint32_t COV[][2] = {\n")
        for i in range(0, len(rs), 4):
            f.write("    " + " ".join(f"{{ 0x{a:04X}, 0x{b:04X} }}," for a, b in rs[i:i + 4]) + "\n")
        f.write("};\n\n")
        f.write("bool font_cov_has(uint32_t cp)\n{\n"
                "    int lo = 0, hi = (int)(sizeof COV / sizeof COV[0]) - 1;\n"
                "    while (lo <= hi) {\n"
                "        int m = (lo + hi) / 2;\n"
                "        if (cp < COV[m][0]) hi = m - 1;\n"
                "        else if (cp > COV[m][1]) lo = m + 1;\n"
                "        else return true;\n"
                "    }\n    return false;\n}\n")
    print(f"font_cov.c: {len(text_cov)} caractères, {len(rs)} plages")


main()
