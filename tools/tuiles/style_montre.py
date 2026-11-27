#!/usr/bin/env python3
"""Generates tools/tuiles/style-montre.json (MapLibre style, OpenMapTiles schema).

Zooms here are MapLibre zooms: watch tile level = MapLibre zoom + 1.
"""
import json, sys

SRC = "openmaptiles"
FONT = ["Noto Sans Regular"]
NAME = ["coalesce", ["get", "name:fr"], ["get", "name"]]
HALO = "#ffffff"


def interp(*stops):
    e = ["interpolate", ["exponential", 1.5], ["zoom"]]
    for z, v in stops:
        e += [z, v]
    return e


def lin(*stops):
    e = ["interpolate", ["linear"], ["zoom"]]
    for z, v in stops:
        e += [z, v]
    return e


def scale(stops, k, add=0):
    return [(z, round(v * k + add, 2)) for z, v in stops]


L = []


def layer(id, type, src_layer=None, **kw):
    d = {"id": id, "type": type}
    if src_layer:
        d["source"] = SRC
        d["source-layer"] = src_layer
    for k in ("minzoom", "maxzoom", "filter", "layout", "paint"):
        if k in kw:
            d[k] = kw[k]
    L.append(d)


C = dict(
    bg="#f2efe9", wood="#bcd9a4", grass="#d3e8bd", park="#c8e4ae", wet="#cfe3da", ice="#f4f8fb",
    sand="#efe0b9", rock="#dedad3", resid="#e6e0d8", comm="#efdcdc", indus="#e5dbe6", cemet="#c9dbbf",
    water="#94c3e6", waterline="#6aa8d8", building="#d8cdc3", buildline="#b9ac9f",
    motorway="#e8735c", motorway_c="#a8432f", trunk="#f39b6d", trunk_c="#b55e34",
    primary="#f8cb69", primary_c="#a87d1f", secondary="#faeb85", secondary_c="#9f9446",
    minor="#ffffff", minor_c="#a99f93", path="#b5523b", track="#8c6a3e", rail="#6f6f6f",
    text="#1a1a1a", text2="#3d3d3d", water_text="#1f5e93", boundary="#7b4f9a",
)

# ------------------------------------------------------------------ fills
L.append({"id": "fond", "type": "background", "paint": {"background-color": C["bg"]}})
layer("couverture-bois", "fill", "landcover", filter=["==", ["get", "class"], "wood"],
      paint={"fill-color": C["wood"], "fill-antialias": False})
layer("couverture-herbe", "fill", "landcover", filter=["==", ["get", "class"], "grass"],
      paint={"fill-color": C["grass"], "fill-antialias": False})
layer("couverture-humide", "fill", "landcover", filter=["==", ["get", "class"], "wetland"],
      paint={"fill-color": C["wet"], "fill-antialias": False})
layer("couverture-glace", "fill", "landcover", filter=["==", ["get", "class"], "ice"],
      paint={"fill-color": C["ice"], "fill-antialias": False})
layer("couverture-sable", "fill", "landcover", filter=["==", ["get", "class"], "sand"],
      paint={"fill-color": C["sand"], "fill-antialias": False})
layer("couverture-roche", "fill", "landcover", filter=["==", ["get", "class"], "rock"],
      paint={"fill-color": C["rock"], "fill-antialias": False})
layer("usage-habitat", "fill", "landuse",
      filter=["match", ["get", "class"], ["residential", "suburb", "quarter", "neighbourhood"], True, False],
      paint={"fill-color": C["resid"], "fill-opacity": lin((4, 0.5), (9, 1))})
layer("usage-commerce", "fill", "landuse", minzoom=10,
      filter=["match", ["get", "class"], ["commercial", "retail"], True, False],
      paint={"fill-color": C["comm"]})
layer("usage-industrie", "fill", "landuse", minzoom=10,
      filter=["match", ["get", "class"], ["industrial", "garages", "dam"], True, False],
      paint={"fill-color": C["indus"]})
layer("usage-cimetiere", "fill", "landuse", minzoom=11, filter=["==", ["get", "class"], "cemetery"],
      paint={"fill-color": C["cemet"]})
layer("parcs", "fill", "park", paint={"fill-color": C["park"], "fill-opacity": lin((6, 0.6), (10, 1))})
layer("eau", "fill", "water", filter=["!=", ["get", "brunnel"], "tunnel"],
      paint={"fill-color": C["water"], "fill-antialias": False})
layer("cours-eau", "line", "waterway", filter=["!=", ["get", "brunnel"], "tunnel"],
      layout={"line-cap": "round", "line-join": "round"},
      paint={"line-color": C["waterline"],
             "line-width": ["match", ["get", "class"],
                            ["river", "canal"], interp((8, 1), (11, 2.5), (14, 6), (16, 14)),
                            interp((11, 0.8), (14, 2), (16, 4))]})
layer("pistes", "line", "aeroway", minzoom=10, filter=["match", ["get", "class"], ["runway", "taxiway"], True, False],
      paint={"line-color": "#bdbdc8", "line-width": interp((10, 1), (13, 6), (16, 30))})
layer("batiments", "fill", "building", minzoom=14,
      paint={"fill-color": C["building"], "fill-outline-color": C["buildline"]})

# ------------------------------------------------------------------ roads
W = {
    "motorway": [(4, 0.9), (7, 1.8), (10, 4), (12, 7), (14, 15), (16, 34)],
    "trunk": [(5, 0.8), (7, 1.6), (10, 3.5), (12, 6.5), (14, 14), (16, 32)],
    "primary": [(6, 0.7), (8, 1.4), (10, 3), (12, 6), (14, 13), (16, 30)],
    "secondary": [(8, 0.7), (10, 2), (12, 5), (14, 11), (16, 26)],
    "tertiary": [(9, 0.7), (11, 2.3), (12, 4), (14, 10), (16, 24)],
    "minor": [(11, 0.9), (12, 2.4), (14, 8), (16, 19)],
    "service": [(12, 0.7), (14, 3.5), (16, 9)],
}
MINZ = {"motorway": 4, "trunk": 5, "primary": 6, "secondary": 8, "tertiary": 9, "minor": 11, "service": 12}
FILL = {"motorway": "motorway", "trunk": "trunk", "primary": "primary", "secondary": "secondary",
        "tertiary": "minor", "minor": "minor", "service": "minor"}
CASE = {"motorway": "motorway_c", "trunk": "trunk_c", "primary": "primary_c", "secondary": "secondary_c",
        "tertiary": "minor_c", "minor": "minor_c", "service": "minor_c"}
ORDER = ["service", "minor", "tertiary", "secondary", "primary", "trunk", "motorway"]
CASE_FROM = {"motorway": 7, "trunk": 8, "primary": 9, "secondary": 10, "tertiary": 10, "minor": 12, "service": 13}


def road_filter(cls, brunnel):
    f = ["all", ["==", ["get", "class"], cls]]
    if brunnel == "tunnel":
        f.append(["==", ["get", "brunnel"], "tunnel"])
    else:
        f.append(["!=", ["get", "brunnel"], "tunnel"])
    return f


# tunnels: thin, pale, dashed casing
for cls in ORDER:
    layer(f"tunnel-{cls}", "line", "transportation", minzoom=max(MINZ[cls], 11), filter=road_filter(cls, "tunnel"),
          paint={"line-color": C[CASE[cls]], "line-opacity": 0.6, "line-dasharray": [1.5, 1],
                 "line-width": interp(*scale(W[cls], 0.6))})

# sentiers, chemins
layer("chemins", "line", "transportation", minzoom=12, filter=["==", ["get", "class"], "track"],
      paint={"line-color": C["track"], "line-dasharray": [3, 1.5], "line-width": interp((12, 1.2), (14, 2), (16, 3.5))})
layer("sentiers", "line", "transportation", minzoom=14,
      filter=["all", ["==", ["get", "class"], "path"], ["!=", ["get", "brunnel"], "tunnel"]],
      paint={"line-color": C["path"], "line-dasharray": [2, 1.2], "line-width": interp((14, 1.6), (16, 3))})

# casings, then fills (so crossings look clean)
for cls in ORDER:
    layer(f"bord-{cls}", "line", "transportation", minzoom=CASE_FROM[cls], filter=road_filter(cls, "none"),
          layout={"line-cap": "round", "line-join": "round"},
          paint={"line-color": C[CASE[cls]], "line-width": interp(*scale(W[cls], 1, 2.2))})
for cls in ORDER:
    # below the casing zoom, minor roads are drawn grey (white on cream is invisible)
    col = C[FILL[cls]]
    if FILL[cls] == "minor":
        col = ["step", ["zoom"], C["minor_c"], CASE_FROM[cls], C["minor"]]
    layer(f"route-{cls}", "line", "transportation", minzoom=MINZ[cls], filter=road_filter(cls, "none"),
          layout={"line-cap": "round", "line-join": "round"},
          paint={"line-color": col, "line-width": interp(*W[cls])})

layer("rails", "line", "transportation", minzoom=8,
      filter=["all", ["==", ["get", "class"], "rail"], ["!=", ["get", "brunnel"], "tunnel"],
              ["!", ["match", ["get", "service"], ["yard", "siding", "spur"], True, False]]],
      paint={"line-color": C["rail"], "line-width": interp((8, 0.8), (12, 1.8), (16, 4))})
layer("rails-tirets", "line", "transportation", minzoom=12,
      filter=["all", ["==", ["get", "class"], "rail"], ["!=", ["get", "brunnel"], "tunnel"],
              ["!", ["match", ["get", "service"], ["yard", "siding", "spur"], True, False]]],
      paint={"line-color": "#ffffff", "line-dasharray": [3, 3], "line-width": interp((12, 0.8), (16, 2))})
layer("tram", "line", "transportation", minzoom=12,
      filter=["all", ["==", ["get", "class"], "transit"], ["!=", ["get", "brunnel"], "tunnel"]],
      paint={"line-color": C["rail"], "line-width": interp((12, 0.8), (16, 2))})
layer("bacs", "line", "transportation", minzoom=8, filter=["==", ["get", "class"], "ferry"],
      paint={"line-color": "#4a86c5", "line-dasharray": [2, 2], "line-width": interp((8, 0.8), (14, 2))})

# ------------------------------------------------------------- boundaries
layer("limite-region", "line", "boundary", minzoom=4,
      filter=["all", ["==", ["get", "admin_level"], 4], ["!=", ["get", "maritime"], 1]],
      paint={"line-color": C["boundary"], "line-opacity": 0.45, "line-dasharray": [3, 2],
             "line-width": interp((4, 0.8), (10, 2), (14, 3))})
layer("limite-pays", "line", "boundary",
      filter=["all", ["==", ["get", "admin_level"], 2], ["!=", ["get", "maritime"], 1]],
      layout={"line-join": "round"},
      paint={"line-color": C["boundary"], "line-opacity": 0.8, "line-width": interp((0, 0.8), (6, 2), (12, 4))})

# ----------------------------------------------------------------- labels
def text_paint(color=C["text"], halo=2.5):
    return {"text-color": color, "text-halo-color": HALO, "text-halo-width": halo, "text-halo-blur": 0.5}


layer("nom-cours-eau", "symbol", "waterway", minzoom=12, filter=["has", "name"],
      layout={"symbol-placement": "line", "text-field": NAME, "text-font": FONT, "text-size": lin((12, 16), (16, 19)),
              "symbol-spacing": 400, "text-max-angle": 30, "text-letter-spacing": 0.05},
      paint=text_paint(C["water_text"], 2))
layer("nom-eau", "symbol", "water_name", filter=["has", "name"],
      layout={"text-field": NAME, "text-font": FONT, "text-max-width": 8,
              "text-size": ["match", ["get", "class"], ["ocean", "sea"], lin((3, 15), (8, 22)), lin((8, 15), (14, 19))]},
      paint=text_paint(C["water_text"], 2))
layer("nom-sommet", "symbol", "mountain_peak", minzoom=11, filter=["has", "name"],
      layout={"text-field": ["concat", NAME, ["case", ["has", "ele"], ["concat", "\n", ["to-string", ["get", "ele"]], " m"], ""]],
              "text-font": FONT, "text-size": 16, "text-max-width": 8},
      paint=text_paint("#6b4423", 2))

ROADLABEL = ["primary", "secondary", "tertiary", "minor", "trunk", "service", "motorway"]
layer("nom-rue", "symbol", "transportation_name", minzoom=12,
      filter=["all", ["has", "name"], ["match", ["get", "class"], ROADLABEL, True, False]],
      layout={"symbol-placement": "line", "text-field": NAME, "text-font": FONT,
              "text-size": lin((12, 16), (14, 19), (16, 22)), "symbol-spacing": 350, "text-max-angle": 30,
              "text-padding": 2},
      paint=text_paint(C["text2"], 2.5))
layer("numero-route", "symbol", "transportation_name", minzoom=7,
      filter=["all", ["has", "ref"], ["match", ["get", "class"], ["motorway", "trunk"], True, False],
              ["<=", ["get", "ref_length"], 6]],
      layout={"symbol-placement": "line", "text-field": ["get", "ref"], "text-font": FONT,
              "text-size": lin((7, 14), (12, 17)), "symbol-spacing": 500, "text-rotation-alignment": "viewport",
              "text-padding": 10},
      paint={"text-color": "#ffffff", "text-halo-color": C["motorway_c"], "text-halo-width": 3})

PL = [
    # class, minzoom, sizes, color, transform
    ("hamlet isolated_dwelling neighbourhood locality", 12, [(12, 15), (14, 17), (16, 19)], C["text2"]),
    ("suburb quarter", 11, [(11, 16), (14, 20)], C["text2"]),
    ("village", 9, [(9, 16), (11, 19), (14, 22)], C["text"]),
    ("town", 6, [(6, 14), (8, 18), (11, 23), (14, 26)], C["text"]),
    ("city", 3, [(3, 14), (6, 18), (9, 25), (12, 30)], C["text"]),
]
for i, (cls, mz, sizes, col) in enumerate(PL):
    layer(f"lieu-{cls.split()[0]}", "symbol", "place", minzoom=mz,
          filter=["match", ["get", "class"], cls.split(), True, False],
          layout={"text-field": NAME, "text-font": FONT, "text-size": lin(*sizes), "text-max-width": 7,
                  "text-padding": 3, "symbol-sort-key": ["coalesce", ["get", "rank"], 99]},
          paint=text_paint(col, 2.5))
layer("lieu-region", "symbol", "place", minzoom=4, maxzoom=8, filter=["==", ["get", "class"], "state"],
      layout={"text-field": NAME, "text-font": FONT, "text-size": lin((4, 14), (7, 17)), "text-max-width": 8,
              "text-transform": "uppercase", "text-letter-spacing": 0.08},
      paint=text_paint("#6e4f86", 2))
layer("lieu-pays", "symbol", "place", maxzoom=6, filter=["==", ["get", "class"], "country"],
      layout={"text-field": NAME, "text-font": FONT, "text-size": lin((1, 14), (5, 20)), "text-max-width": 8,
              "text-transform": "uppercase", "text-letter-spacing": 0.1},
      paint=text_paint("#4f3366", 2.5))

style = {
    "version": 8,
    "name": "T-Watch Ultra — lisible",
    "metadata": {"generateur": "tools/tuiles (style fait main pour l'écran de la montre)"},
    "sources": {SRC: {"type": "vector", "url": "mbtiles://openmaptiles", "maxzoom": 14,
                      "attribution": "© OpenMapTiles © contributeurs OpenStreetMap"}},
    "glyphs": "fonts://{fontstack}/{range}.pbf",
    "layers": L,
}
json.dump(style, open(sys.argv[1], "w"), ensure_ascii=False, indent=1)
print(len(L), "layers")
