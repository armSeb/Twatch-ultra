#!/usr/bin/env python3
"""Fabrique les fichiers de test de test_pmtiles.c avec le paquet Python
pmtiles : petit fichier (racine seule) et gros fichier (feuilles), tuiles
identiques regroupées, plus la liste attendue « z x y offset longueur »
(offset -1 : absente), relue avec le lecteur Python."""
import os, random, sys
from pmtiles.tile import zxy_to_tileid, TileType, Compression
from pmtiles.writer import Writer
from pmtiles.reader import Reader, MmapSource

OUT = sys.argv[1]
random.seed(5)

def build(name, zooms, density):
    tiles = {}
    for z in zooms:
        n = 1 << z
        for x in range(n):
            for y in range(n):
                if random.random() < density:
                    # un quart de tuiles identiques (mer), le reste distinct
                    tiles[(z, x, y)] = b"SEA" if random.random() < 0.25 else f"T{z}/{x}/{y}".encode() * 3
    path = os.path.join(OUT, name + ".pmtiles")
    with open(path, "wb") as f:
        w = Writer(f)
        for k in sorted(tiles, key=lambda k: zxy_to_tileid(*k)):
            w.write_tile(zxy_to_tileid(*k), tiles[k])
        w.finalize({"tile_type": TileType.PNG, "tile_compression": Compression.NONE,
                    "min_lon_e7": -1800000000, "min_lat_e7": -850000000, "max_lon_e7": 1800000000,
                    "max_lat_e7": 850000000, "center_zoom": 3, "center_lon_e7": 23522000,
                    "center_lat_e7": 488566000}, {})
    # positions attendues, données comprises (lues à la main)
    with open(path, "rb") as f:
        data = f.read()
    with open(path, "rb") as f:
        r = Reader(MmapSource(f))
        with open(os.path.join(OUT, name + ".txt"), "w") as lst:
            probe = list(tiles)
            for z in zooms:
                for _ in range(200):
                    n = 1 << z
                    probe.append((z, random.randrange(n), random.randrange(n)))
            probe.append((max(zooms) + 1, 0, 0))
            for (z, x, y) in probe:
                t = r.get(z, x, y)
                if t is None:
                    lst.write(f"{z} {x} {y} -1 0\n")
                else:
                    off = data.find(t, r.header()["tile_data_offset"])
                    lst.write(f"{z} {x} {y} {off} {len(t)}\n")
    return len(tiles)

print(build("small", range(0, 6), 0.7), build("big", range(0, 11), 0.035), "tuiles")
