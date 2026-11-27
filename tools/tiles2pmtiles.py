#!/usr/bin/env python3
"""Regroupe un dossier de tuiles tiles/<z>/<x>/<y>.png en un seul fichier PMTiles
lu par l'app Cartes de la montre (beaucoup plus rapide à copier et à lire).

    pip install pmtiles
    python3 tools/tiles2pmtiles.py /chemin/vers/tiles tiles.pmtiles

Puis copier tiles.pmtiles à la racine de la carte SD.
"""
import math
import os
import sys

try:
    from pmtiles.tile import zxy_to_tileid, TileType, Compression
    from pmtiles.writer import Writer
except ImportError:
    sys.exit("Il manque le paquet Python pmtiles : pip install pmtiles")


def lon_of(x, z):
    return x / 2 ** z * 360.0 - 180.0


def lat_of(y, z):
    return math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / 2 ** z))))


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    tiles = []
    for zd in os.listdir(src):
        if not zd.isdigit():
            continue
        z = int(zd)
        for xd in os.listdir(os.path.join(src, zd)):
            if not xd.isdigit():
                continue
            for f in os.listdir(os.path.join(src, zd, xd)):
                if f.endswith(".png") and f[:-4].isdigit():
                    tiles.append((zxy_to_tileid(z, int(xd), int(f[:-4])), z, int(xd), int(f[:-4])))
    if not tiles:
        sys.exit(f"Aucune tuile <z>/<x>/<y>.png dans {src}")
    tiles.sort()
    zmin = min(t[1] for t in tiles)
    wide = [t for t in tiles if t[1] == zmin]
    x0, x1 = min(t[2] for t in wide), max(t[2] for t in wide) + 1
    y0, y1 = min(t[3] for t in wide), max(t[3] for t in wide) + 1
    with open(dst, "wb") as out:
        w = Writer(out)
        for i, (tid, z, x, y) in enumerate(tiles):
            with open(os.path.join(src, str(z), str(x), f"{y}.png"), "rb") as f:
                w.write_tile(tid, f.read())
            if i % 5000 == 0:
                print(f"\r{i}/{len(tiles)} tuiles", end="", flush=True)
        w.finalize({
            "tile_type": TileType.PNG,
            "tile_compression": Compression.NONE,
            "min_lon_e7": int(lon_of(x0, zmin) * 1e7), "max_lon_e7": int(lon_of(x1, zmin) * 1e7),
            "min_lat_e7": int(lat_of(y1, zmin) * 1e7), "max_lat_e7": int(lat_of(y0, zmin) * 1e7),
            "center_zoom": zmin,
            "center_lon_e7": int(lon_of((x0 + x1) / 2, zmin) * 1e7),
            "center_lat_e7": int(lat_of((y0 + y1) / 2, zmin) * 1e7),
        }, {"name": "twatch-ultra", "attribution": "© OpenStreetMap contributors"})
    print(f"\r{len(tiles)} tuiles → {dst} ({os.path.getsize(dst) / 1e6:.1f} Mo)")


if __name__ == "__main__":
    main()
