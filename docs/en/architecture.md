# Architecture and power

[Français](../fr/architecture.md) · English

## Tasks and ownership

```
            ┌───────────── NimBLE host task (core 0) ─────────────┐
            │ ble.c   GAP, passkey security, battery service,       │
 iPhone ◄──►│         connection parameters, advertising, diag log  │
            │ ancs.c  ANCS + CTS + AMS discovery, command queue,    │
            │         Data Source reassembly (ancs_parse.c)         │
            └──────────┬───────────────────────────┬────────────────┘
                       │ g_notif_queue (copies)    │ app_post(event)
            ┌──────────▼───────────────────────────▼────────────────┐
 PMU IRQ ──►│ app task (core 1): sole owner of LVGL                 │
 touch INT  │ main.c   event loop / lv_timer_handler / screen sleep │
 esp_timers │ ui.c, face_*.c, app_*.c   notif_store.c               │
            │ display.c (CO5300 + CST9217)  power.c, motion.c       │
            └───────────────────────────────────────────────────────┘
```

Stability rules:

- **One owner per state.** LVGL and the notification store belong to the app task; ANCS
  state belongs to the NimBLE host task. They talk through queues and events, never through
  a shared LVGL mutex.
- **One ATT procedure in flight** on the ANCS side, with a generation number checked in every
  callback (late answers after a reconnection are ignored).
- **Data Source reassembled byte by byte**, tested on PC with random fragmentation.
- **iOS replay** filtered with `EventFlagPreExisting` plus content deduplication (FNV-1a,
  5-minute TTL); only the newest 32 notifications are kept.
- **iOS text sanitized** (truncated UTF-8, overlong forms, surrogates, characters no font
  can draw) before it reaches LVGL; emoji are kept (see *Text and emoji* below).
- **Level-triggered PMU interrupt**: no interrupt is lost during light sleep.
- **Events from anywhere, UI in one place**: timers, BLE and interrupts only post events;
  `app_post_event()` lets an app react while its screen is closed.

No `esp_lvgl_port` (its tick timer and task wake the CPU constantly), no PMU, touch, RTC or
haptic libraries (a few registers are enough; the Bosch BHI2xy API is the only sensor
driver), native `esp_lcd`, ESP-IDF 5 `i2c_master` on a single I2C bus.

## User interface

- **Scaling and safe area**: the UI was designed for 240×240 and is laid out ×1.5
  (`S()` and `UI_K` in `ui_geom.h`) inside a 360×442 safe area that keeps content out of the
  panel's rounded corners (`UI_PAD_X/Y`, see [hardware.md](hardware.md)). Fonts were
  regenerated at 1.5× (17, 24, 30, 66 and 108 px); the gesture thresholds of LVGL are scaled
  too. The *Pub* face and *Soar* draw over the full panel.

- **Navigation by origin**: every screen remembers where it was opened from; swipe → or the
  « ‹ » header always returns there.
- **Lazy screens**: settings, now playing, launcher, control center and the hosted app are
  built on demand and destroyed when the screen turns off, so RAM only holds what is shown.
  Permanent screens: watch face, notification list, detail, call, battery, pairing, about.
- **Apps** (`app.h`): see [apps-and-i18n.md](apps-and-i18n.md). The open app stays built
  when the screen turns off (it gets `on_hide()`, then `on_show()` on wake) and the watch
  wakes back to it.
- **Notification banner**: outside the watch face, a new notification slides in on the top
  layer instead of switching screens; tapping it opens the detail, which returns to the exact
  screen you were on. Incoming calls still take the full screen and return there afterwards.
- **Wi-Fi analyzer** (`app_wifi.c`): the Wi-Fi driver only exists while the app is visible
  with the screen on (`esp_wifi_init` in `on_show`, `esp_wifi_deinit` in `on_hide`); each
  fetch starts the radio, runs an active scan (~1.2 s) and stops it again for 4 s. BLE stays
  connected (software coexistence).
- **WebRX** (`app_webrx.c`, `webrx.c`, `webrx_cfg.c`): OpenWebRX / OpenWebRX+ client (user
  guide: [webrx.md](webrx.md)). The Wi-Fi networks (up to 3) and the servers (up to 4, name
  and URL; listening state kept per server) are entered on a page served by the watch
  (`/webrx`, same access point and web server as the update; the session ends with a restart
  that reopens the app). `webrx.c` runs a task on core 0: Wi-Fi station (known networks in
  order of preference, modem sleep), `wss://` through `esp_transport` (TLS with the ESP-IDF
  root bundle, mbedTLS buffers in PSRAM; `user:password@` in the URL becomes HTTP Basic auth),
  the OpenWebRX handshake, `connectionproperties` (12 kHz audio for every mode),
  `selectprofile` and `dspcontrol`. The server's `modes` list (custom modes included) gives
  the listenable modes and their bandpass; digital modes fall back to their underlying mode.
  Audio frames (IMA-ADPCM with sync words) are decoded into a 2.7 s ring in PSRAM; a second
  task on core 1 keeps ~0.4 s ahead and feeds the I2S amplifier, skipping or doubling one
  sample per 20 ms block to follow the server's clock; a tone filter (high-pass, presence bump,
  2nd/4th-order low-pass, 5 steps) suits the small speaker. Text frames are built and sent in
  a single write: the OpenWebRX server drops a client whose frame header and payload arrive
  in separate TCP segments, which is what `esp_transport_ws` does. FFT frames become the mini waterfall
  only while the screen is on. CPU is capped at 160 MHz (80 MHz screen off) while the radio
  runs, and Bluetooth is paused only if the Wi-Fi driver cannot get its internal RAM.
  The Wi-Fi station part (scan, known networks, reasons, BLE fallback) lives in `wifi_sta.c`,
  the 5-step tone filter in `audio_fx.c`; both are shared with Radio.
- **Radio** (`app_radio.c`, `radio.c`; stations in `webrx_cfg.c`, user guide:
  [radio.md](radio.md)): web radio player. Up to 12 stations (name, URL) are entered on the
  same `/webrx` page. Task `rad` (core 0) connects Wi-Fi, opens the stream with
  `esp_http_client` (TLS with the root bundle, redirects followed by hand, `Icy-MetaData: 1`),
  follows `.m3u`/`.pls` playlists, picks the codec from `Content-Type` or by sniffing the first
  bytes, strips ICY metadata blocks (`StreamTitle`, Latin-1 converted to UTF-8) and fills a
  128 KB compressed ring in PSRAM; playback starts after ≈2 s of audio (from `icy-br`). Task
  `radplay` (core 1) decodes with the Helix fixed-point decoders (`components/helix_mp3`,
  `components/helix_aac`: MP3, AAC-LC, HE-AAC/SBR), downmixes to mono, applies the tone filter
  and the volume, and feeds the I2S amplifier through `sound_stream_*`. It measures the share
  of time spent decoding: above 60 % it takes a `CPU_FREQ_MAX` lock (160 MHz), below 25 % it
  releases it, so the screen-off cap of 80 MHz holds whenever the stream allows. Every 2 s,
  with the screen off and RSSI above −65 dBm, the Wi-Fi TX power is lowered to 11 dBm
  (19.5 dBm otherwise). Failures retry after 5, 10, 20, 30 then 60 s; a dropped stream
  resumes after 2 s. Tested on the host with the real decoders and a fake HTTP server
  (redirect, `.pls`, ICY titles).
- **Púca** (`app_puca.c` view, `puca_model.c` life, `puca_gfx.c` art, `puca_sfx.c` sounds;
  user guide: [puca.md](puca.md)): virtual pet. The model has no task and no timer: on each
  display it is advanced in 5-minute steps from the last visit (up to 7 days; tested on the
  host), state saved as one 76-byte NVS blob. Steps of the day turn into shamrocks. The view is
  a tiny sprite engine: an 80×50 buffer of palette indices redrawn at 12.5 frames/s while the
  screen is on (sky keyframes placed around the day's real sunrise and sunset — `puca_sun()`,
  computed for central France with the local offset, so winter evenings get dark early — stars, moon or sun, drifting clouds, hills,
  droppings, the pet, particles), then scaled ×4 into a 320×240 RGB565 canvas in PSRAM (10 extra rows of sky on top).
  Expressions are composed: bodies have no face, shared 3×3 eye and mouth glyphs are placed at
  per-body anchors, so every form gets open/blink/happy/sleep/sad/sick faces. Night with the
  light off dims every palette entry except the "glowing" ones (eyes, moon, stars). All sprites
  live as text grids in `tools/puca_art.py`, which generates `puca_gfx.c/.h` and a preview
  sheet. Sound effects are short square-wave tunes played by a one-shot task through the I2S
  stream.
- **Mouse** (`app_mouse.c`, `ble_hid.c`, guide: [mouse.md](mouse.md)): HID over GATT.
  `ble_set_hid_mode()` then `ble_start()` restart NimBLE with another identity (static address
  derived differently from the MAC, name, appearance 0x03C2), the HID services (mouse report 1,
  keyboard 2, media 3, report references, protocol mode), DIS (PnP ID) and battery, without
  ANCS, "Just Works"; its own GAP events (no eco parameters, 11.25–27.5 ms link with latency 4, no IRK distributed).
  The touchpad reads the touch screen every 10 ms, accelerates with speed and batches moves
  (~80 reports/s at most).
- **Demo** (`app_demo.c`, guide: [demo.md](demo.md)) and **Soar** (`app_flight.c`, guide:
  [flight.md](flight.md)) share three modules. `demo_gfx.c`: RGB565 colours, 5×7 font with
  accents, triangles, scaling into the canvas. `demo_synth.c`: tracker-style synth in its
  `demosnd` task (core 0), I2S stream at 16 kHz kept open from start to end (muting only sends
  silence), a 64-bar tune for the demo or a loop for the game, sound effects mixed on top; it
  is the clock: each frame reads its position in the tune (and the kick envelope) rather than a
  frame counter. `voxel.c`: a 512×512 world (heights 256 KB, lit colours 512 KB, in PSRAM)
  generated by a low-priority `voxgen` task on core 0 — terrain, rivers, Voronoi meadows with
  hedges, forests, farms, lighting with cast shadows ray-marched towards the sun — then rendered
  column by column with a height buffer, foreground interpolation, fog, reflections, projected
  clouds, per-column roll and an optional occlusion table (top of the terrain at 16 distances
  per column). After generation, height and colour are merged into one 32-bit word per cell
  (1 MB): a single PSRAM read per sample, which is what bounds the frame rate. Rendering is
  split between the two cores: a `voxrend` task (core 0, created on the first render, stopped
  with the world) computes the left half while the caller computes the right, synchronised by
  two semaphores. The demo
  renders at 120×120 tripled into a 360×360 LVGL canvas; the game at 205×168, columns
  doubled and rows tripled to fill the whole 410×502 panel, and sent straight to the display
  (`display_direct()`: two 26-line strips in DMA RAM, one filled while the other is sent,
  started on the panel's TE signal, no PSRAM canvas); its HUD is drawn at full resolution in
  the safe area. Both hold 240 MHz while shown and read the accelerometer live
  (`motion_accel_*`: the BHI260AP's corrected accelerometer at 100 Hz, its rail switched on
  for the game if needed). Soar's best score is an NVS blob (`flight`/`best`).
- **Language change** rebuilds every screen in place, without a restart.

## Text and emoji

- **Fonts**: Montserrat (Latin, extended Latin, Vietnamese, Cyrillic, punctuation, currency)
  completed by DejaVu Sans (Greek, arrows, maths, geometric shapes, symbols, dingbats), plus
  the Font Awesome icons. `tools/gen_fonts.py` also writes `font_cov.c`, the exact list of
  characters the text fonts can draw; `notif_store.c` keeps those and the emoji ranges, and
  drops the rest.
- **Emoji pack** (`assets/emoji.bin`, built by `tools/gen_emoji.py`, flashed to the `emoji`
  partition): 1,949 Twemoji images — base forms (no skin tones: the watch shows the yellow
  one), flags, keycaps and ZWJ sequences — at 20 and 28 px, RGB565 + alpha, each compressed
  with raw deflate (2.6 MB). The partition is memory-mapped; an image is inflated on demand by
  the ROM's `tinfl` into a PSRAM cache (192 per size).
- **Sequences** (`emoji.c`): a sequence is not drawn code point by code point. `emoji_text()`
  finds the longest known sequence at each position (binary search over the sorted pack,
  following ZWJ, keycap, tag and regional-indicator rules), replaces it with one private
  code point `U+F0000 + index`, and drops variation selectors, orphan ZWJ and skin tones.
  `emoji_font_20` and `emoji_font_28` are LVGL fonts that draw those code points as images
  (`LV_FONT_GLYPH_FORMAT_IMAGE`); they are the fallbacks of `font_small`, `font_text` and
  `font_title`, so any label shows emoji once its text went through `emoji_text()`
  (`emoji_label_set_text()`).
- LVGL objects are allocated from the heap; allocations above 4 KB go to PSRAM, the internal
  RAM is kept for stacks, DMA buffers and the radio.

## Power

The ESP32-S3 takes its Bluetooth low-power clock from the external 32.768 kHz crystal
(`BT_CTRL_LPCLK_SEL_EXT_32K_XTAL`): the controller sleeps between connection events, so
**automatic light sleep stays possible with the iPhone connected**.

| Lever | Setting |
|---|---|
| Frequency scaling | CPU at **40 MHz** at rest; **240 MHz** only during interaction; radio apps capped at 160 MHz (80 MHz screen off) |
| Light sleep | automatic (tickless idle), with the PMU, touch and motion interrupts as level-triggered wake sources |
| BLE link | after ANCS is ready, economical parameters are requested (150–300 ms interval, latency 4, 6 s timeout, within Apple's rules) and **re-requested until accepted**, with growing back-off. While the MeshCore app is connected, whatever the watch is doing: 15–30 ms link with no latency, no light sleep and CPU kept at 80 MHz or more; when the app leaves, back to the economical parameters and light sleep |
| Tasks | no periodic timer with the screen off; the app task sleeps until an event (60 s max); LVGL tick read on demand |
| Display | CO5300 in sleep mode (SLPIN); touch controller left scanning slowly when double tap wakes the watch, otherwise in deep sleep (woken by reset) |
| AXP2101 | every unused rail off (GNSS, DCDC2–5; LoRa, SD and NFC unless MeshCore runs); speaker amplifier rail on only while a sound plays; BHI260AP rail off when neither raise to wake nor steps are on |
| Peripherals | pins of unpowered chips left floating (no phantom supply through protection diodes) |
| Airplane mode | Bluetooth stopped |

The AXP2101 gives a battery percentage (its own fuel gauge, loaded with a battery model at
first boot) but no battery current. The *Battery* screen therefore estimates the average
consumption from the fall of that percentage over the current discharge (1 % ≈ 11 mAh), and
keeps the previous discharge and the screen-on share in NVS.

## Motion sensor (`motion.c`)

The BHI260AP is a programmable sensor hub. When *Raise to wake* or the *Step counter* is on,
`motion_init()` (called from `power_init()`) powers ALDO4, uploads the Bosch firmware
(`components/bhy2/`, BSD-3-Clause) into its RAM (about 1 s), and enables the virtual sensors
needed: step counter and wrist-tilt gesture. Both off: the rail is cut and the sensor draws
nothing.

- **Raise to wake**: the gesture arrives on the interrupt line (GPIO8, active high, also a
  light-sleep wake source) as `EV_MOTION`; the app task reads the FIFO and, with the screen
  off, wakes it. `motion_wrist_confirm()` then checks the watch position according to the
  sensitivity: medium = gravity mostly on Z (screen facing up) on two samples 250 ms apart
  with little movement; low = stricter, and a watch lying flat is refused; high = the Bosch
  gesture alone.
- **Glance** (`display_set_glance()`): the screen lights at the glance brightness without the
  `CPU_FREQ_MAX` lock and turns off after the glance duration; a touch or the button makes it
  a normal wake.
- **Step counter**: the sensor counts on its own. Its counter cannot be reset without
  restarting its firmware, so the watch keeps the value at its last software reset and
  subtracts it. Today's total = a base kept in NVS + the counter since that reset. At local
  midnight the total goes into a 7-day history. The total is saved to NVS every 15 minutes if
  it changed, on a day change, when the counter is turned off, and before any restart.

## Wi-Fi update (OTA)

- The watch stops Bluetooth, starts a WPA2 access point (`twatch-ultra-OTA`, password drawn once
  and stored in NVS), a small DNS server answering 192.168.4.1 to everything, and an HTTP
  server. Apple and Android connectivity probes get "success" answers so phones stay on the
  network.
- Upload is streamed in 4 KB chunks into the inactive slot (5 MB each). The first chunk is
  checked (ESP32-S3 image header and application descriptor with project name
  `twatch_ultra`), the full image is verified (SHA-256) before activation. The emoji
  partition is not part of the update.
- Rollback: a new image boots "pending verification" and is marked valid after 30 s of
  operation; a crash before that returns to the previous slot.
- Cancelling restarts the watch (clean return to Bluetooth). During an upload the restart is
  immediate: stopping the HTTP server would wait for the handler, which keeps receiving.
- *Go back to x.y.z* verifies the other slot completely before booting it.

## Source files

| File | Role |
|---|---|
| `main/main.c` | `app_main`, app task, event loop, screen sleep |
| `main/board.h`, `ui_geom.h` | pins, rails, panel geometry, UI scale and safe area |
| `main/ble.c` | NimBLE: GAP, security, advertising, connection parameters, battery service, airplane mode, diagnostic log |
| `main/ancs.c` / `ancs_parse.c` | ANCS + CTS + AMS client (`media.h`); Data Source reassembly |
| `main/notif_store.c` | storage, sort by date, dedup, UTF-8 cleanup |
| `main/emoji.c`, `fonts/` | emoji pack, sequence matching, image fonts; generated text fonts and coverage table |
| `main/ui.c`, `ui_kit.h` | screens, navigation, shared UI building blocks |
| `main/face_classic.c`, `face_pub.c` | analog watch faces (drawn in code) |
| `main/app.h`, `apps.c`, `app_*.c` | app framework, registry, bundled apps |
| `main/display.c` | CO5300 (`esp_lcd`, QSPI, TE sync), CST9217, LVGL glue, direct drawing, double tap |
| `main/power.c`, `axp2101.c` | PMIC, rails, battery, PM locks, brightness, vibration patterns, energy history |
| `main/haptic.c` | DRV2605 haptic driver |
| `main/xl9555.c`, `i2c_bus.c` | I/O expander, shared I2C bus |
| `main/motion.c` | BHI260AP: firmware upload, steps, raise to wake, accelerometer for games |
| `main/sound.c` | beeps and streams on the I2S amplifier, global volume and silent mode |
| `main/wclock.c` | PCF85063 (UTC), time zone, system clock resync |
| `main/settings.c` | settings in NVS |
| `main/i18n.c` | French → English table |
| `main/ota.c` | Wi-Fi update: access point, web page, OTA write, validation, going back |
| `components/bhy2/` | Bosch BHI2xy Sensor API and BHI260AP firmware (BSD-3-Clause) |
| `components/helix_mp3/`, `helix_aac/` | Helix decoders for Radio (RPSL / RCSL) |
