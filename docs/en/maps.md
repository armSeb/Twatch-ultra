# Maps — OpenStreetMap tiles on the SD card

[Français](../fr/maps.md) · **English**

The **Maps** app shows OpenStreetMap maps stored on the micro-SD card, offline: your GPS
position (arrow pointing where you walk) and the MeshCore nodes that advertise theirs
(contacts, repeaters, rooms, sensors).

![Maps app: following the position, zoomed in, wide view](../img/maps.png)

*PC simulator screenshots, with demo tiles (made-up data).*

## Use

| Where | Action |
|---|---|
| Drag | moves the map, which stops as soon as you let go; stops following your position |
| Pinch, or double tap | zoom in / out around the fingers |
| **−** / **+** | zoom out / in (greyed at the limit) |
| **➤** (bottom centre) | centres on your last known position and follows it (blue: following). It does not turn the GPS on: the position is updated from the GPS app |
| Tap a node | its name, distance and age of its advert |
| **‹** or the watch button | back (swiping right moves the map) |

- **Me**: blue arrow in the direction of travel (above 2.5 km/h), blue dot when still; grey
  when the position is old (no fix for 10 s). Pale disc: accuracy, when larger than the marker.
- **MeshCore nodes**: green chat, orange repeater, purple room, teal sensor. Names never
  overlap: a name without room is not drawn (tap the dot). The list is re-read every 15 s.
- **Scale** at the top centre.
- On first open the map centres on your position, else MeshCore's, else the middle of the
  tiles found. It then keeps its view until a reboot.
- You can zoom **three levels past** the most detailed one on the card: the tile is enlarged.
  A missing tile is replaced by an enlarged piece of a wider level (up to 8 levels), so an
  incomplete map stays readable.

## Preparing the SD card

**A single `tiles.pmtiles` file** at the root of the SD card (standard
[PMTiles](https://github.com/protomaps/PMTiles) format, PNG tiles). This is the recommended
way: copying is as fast as for any big file, and the watch finds each tile through its index
in memory, with a single card access. A `*.pmtiles` file in `/tiles` is read too. Size: 2 GB
at most.

- The watch's tile generator writes it directly (see below).
- To pack an existing tile folder: `python3 -m venv ~/venv-pmtiles && ~/venv-pmtiles/bin/pip install pmtiles`, then
  `~/venv-pmtiles/bin/python tools/tiles2pmtiles.py path/to/tiles tiles.pmtiles`.

**Or a folder** `/tiles/<zoom>/<x>/<y>.png`, the MCLite / MeshCore T-Deck layout. It works,
but thousands of small files are very slow to copy, and FAT, having no index, looks each file
up one by one in its folder. Without a pack the watch reads the folder; with both, the pack
first, then the folder for what it lacks.

- 256 × 256 "slippy map" tiles (512 × 512 accepted, halved); palette, grey, RGB or RGBA PNG,
  not interlaced.
- Zoom levels found on the card are detected automatically. For a city, 12 to 16 are enough
  (a few MB to a few tens); 17 and 18 for street detail, much bigger.
- **Making the tiles**: the `tools/tuiles/generer-tuiles.sh` script renders them on the PC
  from a Geofabrik OpenStreetMap extract, with a style readable on the watch (see
  [Offline maps — making the tiles](tiles.md)). Then copy the `tiles` folder to the root of
  the SD card, over USB (Settings › SD card › Share with the PC) or with a card reader.
- **Licence**: data © OpenStreetMap contributors (ODbL). The official
  `tile.openstreetmap.org` server **forbids bulk downloading**, which is why you render
  your own tiles.

Without an SD card, without a `tiles` folder, or while the card is shared with the PC, the
app says so on screen and reads the card again as soon as it is back.

## Power and memory

- While the app is open, the GPS (if on) is read continuously, as in the GPS app.
- Decoded tiles live in **PSRAM** (a cache of 12 to 20 tiles, ≈ 128 KB each, leaving 1.5 MB to
  the rest of the watch), freed when the app closes. Nothing in internal RAM.
- The SD card is held only while a file is read: MeshCore (same SPI bus) keeps receiving.

## Technical details

- `main/maptiles.c`: "tiles" task (PSRAM stack: the SD card is not flash), LRU cache,
  priority queue (screen centre first, then one ring ahead), zoom levels and centre of the
  covered area.
- `main/png_tile.c`: PNG → RGB565 decoder, inflated by the ESP32-S3 ROM `tinfl` (no zlib code
  added). Host-tested (`host_tests/test_png.c`: every format, truncated or corrupted files).
- `main/app_map.c`: a single LVGL object draws tiles, nodes, position and scale. While a tile
  loads, the neighbouring level already in cache is drawn scaled, so zooming shows no hole.
- Pinch: the touch controller (CST9217) tracks two fingers; `display_touch_second()` returns
  the second one.
