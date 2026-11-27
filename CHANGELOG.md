# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

## 0.3.21 — 2026-11-27

- Build of main with the control center redesign (0.3.20), the Púca redesign (0.3.13) and
  Now playing (0.3.3).

## 0.3.20 — 2026-11-27

- Control center redesigned: the status line at the top (battery, Bluetooth, gear) is gone,
  the tiles move up and are slightly shorter, and a third row adds **MeshCore** (red badge
  with the unread count, refreshed every 2 s while shown; tap opens the app, long press turns
  it on or off), **Battery** (level icon with the percentage below, tap opens Energy) and
  **Settings**. The full-width MeshCore pill is removed.
- Control center: brightness and volume are large sliders like Now playing's volume bar
  (touch or drag, value shown inside), instead of − / + buttons. Brightness is saved and the
  volume test beep plays when the finger lifts. Swiping up from a slider closes the center
  and puts its value back.
- Control center: it closes only by pushing it back up (or the button); swiping right no
  longer closes it.
- Bluetooth tile reads "iPhone" when the phone is connected. Wi-Fi tile keeps the network
  name on one line.

## 0.3.13 — 2026-11-27

- Merge of the Púca redesign (0.3.10–0.3.12) with the Now playing work (0.3.1–0.3.3).
- Púca: the PSRAM cache (scenery, clouds, smoothed sprites, about 1 MB) is freed 5 minutes
  after leaving the app; reopening within that time stays immediate.

## 0.3.12 — 2026-11-27

- Púca: the eyes and mouth follow the head when the pet breathes.
- Púca opens faster: the scenery is computed with integer colour mixing, and the scenery,
  clouds and smoothed sprites stay in PSRAM after the first opening (about 1 MB), so the
  next openings are immediate.

## 0.3.11 — 2026-11-27

- Púca: no more black streaks or stray pixels behind the moving pet (the app now refreshes
  full-width bands only, started on the panel's TE).
- Púca: top buttons, gauges and the bottom row moved in from the rounded corners.

## 0.3.10 — 2026-11-27

- Púca redrawn: full-screen scene (rounded corners included), scenery at full resolution
  (dithered gradient sky, sun or moon crossing the sky with a halo, stars, soft drifting
  clouds, mountains, hills, flowery meadow), pixel art scaled ×6 with smoothed, anti-aliased
  outlines, soft shadow under the pet, 25 frames/s with only moving areas redrawn.
- Púca ergonomics: four big round buttons (88 px) instead of five small ones (*clean* and
  *heal* merged into *care*), larger gauges and texts, round back and sound buttons, big
  left/right buttons in the game, large meal cards.
## 0.3.3 — 2026-11-27

- Now playing: the volume bar's touch area reaches 30 px around it (down to the screen's
  bottom edge, almost up to the buttons), so a finger slightly off the bar sets the volume
  instead of swiping back.

## 0.3.2 — 2026-11-27

- Now playing: dragging along the volume bar no longer triggers the "back" swipe (LVGL passes
  gestures up to the screen by default).
- Now playing: while it is on screen, the Bluetooth link switches to its fast setting
  (15-30 ms, like with the MeshCore app) so volume steps follow the finger; it goes back to
  the economical link when the screen goes off or another screen opens.
- Now playing: the volume bar is narrower and higher (340 × 64 px) so its rounded ends stay
  inside the screen's rounded corners.

## 0.3.1 — 2026-11-27

- Now playing: the volume is a wide bar you touch or drag with a finger (like the iPhone)
  instead of the small − / + buttons; the watch sends the iPhone's volume steps one by one
  until the bar's level is reached (AMS has no direct volume setting). Bigger previous /
  play / next buttons, larger title and artist, album line removed.
- Now playing stays open while music plays (or was paused less than 10 minutes ago): the
  screen timing out or the button switches the screen off on it, and waking the watch comes
  straight back to it. A notification opened from it returns to it.

## 0.3.0 — 2026-11-26

- Version scheme: the minor number goes up with each new app or major feature, the patch
  number with each fix or test build; 1.0 once the firmware is stable for daily use.
  0.3.0 = everything up to 0.2.71 (voice recorder, GPS app).

## 0.2.71 — 2026-11-26

- New **GPS** app (u-blox MIA-M10Q GNSS): on/off switch (off by default, nothing powered),
  fix state, position, altitude, speed, satellites used / in view per constellation, signal of
  the 12 strongest satellites, time to first fix. App closed, the GNSS keeps tracking and the
  ESP32 reads it 2 s every 30 s so light sleep keeps working.
- MeshCore: the watch's adverts carry the GPS position when *share location* is on (same
  setting as the iPhone app, now also in the GPS app); the position is stored for reboots;
  custom variable `gps` lets the iPhone app turn the GPS on/off; location telemetry.
- The watch clock is set from the GPS when it drifts by more than 2 s.
- Host test for the NMEA reader (`host_tests/test_gnss.c`).

## 0.2.64 — 2026-11-26

- Recorder: works with the Wi-Fi hotspot on (it failed with "Microphone unavailable": the
  hotspot leaves little internal DMA RAM). The SD write buffer shrinks from 8 KB to 4, 2 or
  1 KB when internal RAM is short, keeping room for the microphone, and the microphone DMA
  buffers retry at 4, 2 then 1 KB.
- Recorder: "Microphone unavailable" diagnostics. Opening the microphone is retried 3 times,
  a read timeout must last 2 s before giving up, and the error message shows the failing step
  and the ESP-IDF code (also logged with the free internal DMA RAM). I2S DMA buffers reduced
  to 4 KB.

## 0.2.63 — 2026-11-26

- Recorder: louder recordings (0.2.62 was too quiet on the watch). The automatic gain now
  aims at -1 dBFS peaks instead of -6 dBFS, reaches up to +48 dB (was +36 dB), starts at
  +30 dB and recovers at 6 dB/s after 0.5 s of calm.

## 0.2.62 — 2026-11-26

- New **Recorder** app (*Magnéto*): records the PDM microphone to the SD card as WAV
  (16-bit mono PCM, 32 kHz by default, 16 kHz "voice" option for twice the duration), in
  `/sd/REC/YYYYMMDD-HHMMSS.WAV`. Recording keeps running with the screen off and when the app
  is left; only the stop button ends it. Automatic gain (up to +36 dB) with a level meter,
  80 Hz high-pass. Samples are buffered in PSRAM (8 s) and written in 64 KB sector-aligned
  blocks; the WAV header is refreshed every 30 s so a crash or card removal keeps the audio.
  List of recordings (newest first) with playback on the speaker (pause, ±10 s) and delete.
- The speaker now uses I2S1: I2S0 is the only port with the PDM-to-PCM filter, needed by the
  microphone. Beeps and notifications still sound during a recording.
- Microphone icon added to the fonts.

## 0.2.61 — 2026-11-26

- Fix boot loop when the Wi-Fi hotspot is on: the hotspot password was read from NVS (flash)
  by the Wi-Fi task, whose stack is in PSRAM (assert in `spi_flash_disable_interrupts_caches`).
  It is now read, and the hotspot interface created, from a task with an internal stack.

## 0.2.60 — 2026-11-26

- Control center: full-width **MeshCore** pill; a tap turns MeshCore on or off, it shows the
  unread count when on, long press opens the MeshCore app. Brightness and volume rows are a
  little more compact to make room.
- Control center: a tap on **Wi-Fi** opens a choice **Off / Client / Hotspot** in three large
  buttons, with the state below (network and address, or hotspot name, password, address and
  connected devices). Long press still opens the Wi-Fi settings, which also get a
  *Client / Hotspot* selector.
- New **hotspot** mode for the global Wi-Fi: network *T-Watch-Ultra* (WPA2, same password as
  the update), web page and MeshCore TCP on 192.168.4.1, up to 4 devices, no light sleep while
  it runs. Remembered across restarts. An app that needs the Internet switches Wi-Fi back to
  client while it runs, then the hotspot returns.
- Radio and WebRX no longer turn Wi-Fi on when opened: if Wi-Fi is not already on as a client,
  a large *Turn on Wi-Fi* (or *Switch to client*) button starts it as a client for the
  listening session only.

## 0.2.59 — 2026-11-26

- Channel messages sent from the watch are retried: when no repeat is heard 17 s after an
  emission, the very same packet (same timestamp, text and region, so the same packet hash) is
  flooded again, up to the *Attempts* setting; nodes that already have it drop it as a
  duplicate. The bubble shows *No repeat, resend 2/3*; the first repeat heard stops the
  retries and repeats are then counted for one minute from the first emission.

## 0.2.58 — 2026-11-26

- T9: tapping a key opens its characters in large over the keyboard (2: a b c 2 à â ç) and
  the wanted one is tapped directly, instead of tapping the key several times with a one
  second timeout. ‹, a tap beside them or the back button close it; long press still gives
  the digit.

## 0.2.57 — 2026-11-26

- MeshCore › Radio: built-in / external LoRa antenna switch at the top of the tab (same
  setting as Settings › MeshCore), no trip to the settings needed.
- Contacts list: path of each contact like the iPhone app (*direct*, *N hop(s)*, *flood*).
- Search bar at the top of Contacts and Channels: opens the keyboard, filters by name
  (case and accents ignored); tap the filter to clear it. Cleared when switching tabs.
- Keyboard title stays on one line.

## 0.2.56 — 2026-11-26

- Contacts (and the paths learned for them) are saved when MeshCore stops, before an OTA
  reboot or a restart: MeshCore writes them 5 s after a change, so a path learned just before
  a reboot was lost.

## 0.2.55 — 2026-11-26

- Region discovery only asks repeaters reachable directly: known contact path (reply path =
  reversed), else the path of their last advert heard since boot (out path = reversed, reply
  path = as heard). 0.2.54 sent a 0-hop request to every repeater without a path, which only
  reaches neighbours, so most requests went nowhere.
- Discovery result says why nothing came back (no path, no reply, no region set) and lists the
  region names received.

## 0.2.54 — 2026-11-25

MeshCore regions on the watch (the node only stores its default scope; the iPhone app keeps
its region list and per-channel choice to itself):
- Region list in Radio › Regions (`main/meshcore/mc_region.c`, NVS `mcreg`): add with the
  keyboard (new `COMPOSE_NO_CAPS` flag), remove, **Discover**: anonymous `REGIONS` request
  (0x01) to each repeater contact reachable directly (known path with the reversed path as
  reply path, else 0-hop), up to 12, one every 1.5 s; names in the replies join the list.
- Per-channel region: chip at the top right of a channel conversation, picker with *Node
  default* and the list. Watch channel sends use that region's key (SHA-256 of `#name`, first
  16 bytes, like `RegionMap`).
- MeshCore patches: `Listener::onWatchResponse` (responses to the watch's own request tags are
  not passed to the app), `twatchScopeBegin(key)`.
- Conversation titles stay on one line.

## 0.2.53 — 2026-11-25

- T9: a letter key acts as soon as the finger touches it, and only that key is redrawn (the
  whole keyboard was redrawn on each tap, long enough for a quick second tap to be missed,
  so b and c could not be reached). Long press still gives the digit, replacing the letter.
- Write bar taller, slightly higher, with a larger touch area: its lower half did not react.
- Messages sent from the watch use the node's default flood scope (region), not the scope the
  iPhone app last set for one of its own sends.

## 0.2.52 — 2026-11-25

Writing MeshCore messages from the watch:
- Every conversation (contact or channel, even without messages) has a **Write** bar that opens
  a full-screen keyboard (`main/compose.c`): full AZERTY or QWERTY with keys chosen on release
  and shown large above the finger, long press for accents, automatic capitals, digits and
  symbols page, common emoji page, quick replies; or T9 multi-tap with huge keys (long press:
  digit). The keyboard mode switches from the keyboard itself and the last one is remembered;
  layout in Settings › MeshCore. Drafts survive closing. The keyboard is a single LVGL object
  drawn key by key, culled to each render strip.
- Direct messages are retried in the mesh task (app closed or screen off) until the recipient's
  ack, up to the new *Attempts* setting (1-5, default 3 like the iPhone app); the last attempt
  floods when the direct path stays silent. Channel messages count the repeats heard (packet
  hash of our flood seen again). States are shown under each bubble like the iPhone app
  (Sending… 2/3, ✓ Delivered · 1.8 s, ✓ Heard 3 repeat(s), ✗ Failed) and kept in history
  and in the SD archive (`"status"`). Tapping a failed message resends it.
- MeshCore patches: `MyMesh::Listener::onFloodHeard`, `twatchAdvert(flood)`, last flood
  packet hash.
- Radio tab: Flood advert and Zero-hop advert buttons.
- Channels tab: the Public channel comes first, below channels with unread messages.

## 0.2.51 — 2026-11-25

Pin audit against schematic V1.0, so that every pin is in a known state whatever an earlier
firmware left behind (the XL9555 and the AXP2101 keep their registers as long as the battery
powers them):
- XL9555: every direction is rewritten at boot. P12 SD_DET is the only input. P06, P07, P10 and
  P13 are outputs at their current level until their driver sets them. Unconnected pins
  (P00-P05, P11, P14-P17) are outputs driven high. Directions found different are logged.
- ESP32-S3: GPIO46, unconnected, joins the pins left high-impedance at boot. The other unused
  or unpowered peripherals (SPI bus, GNSS UART, I2S, PDM mic, RTC and NFC interrupts) were
  already handled; GPIO15/16 carry the 32 kHz crystal.
- AXP2101: USB input voltage threshold (VINDPM 4.36 V) and input current limit (900 mA) set
  explicitly instead of inherited.

## 0.2.50 — 2026-11-25

- XL9555: the SD card detect line (P12) is forced to input at boot. The expander keeps its
  configuration as long as the battery powers it, so an earlier firmware may have left it as
  an output; driven high, the card-detect switch shorts it to ground when a card is in. Seb
  measured 41 mA more with any card inserted, even one with taped-over contacts.

## 0.2.49 — 2026-11-25

- *Energy* screen, sleep line: also the last screen-off spell alone (how long, share asleep),
  and in measure mode the power-management locks held while the screen was off. With the SD
  card inserted the watch drew the same 25-29 mA screen on or off, as if it no longer slept.
- Measure mode also lowers the AXP2101 USB input voltage threshold (VINDPM) to 3.88 V and shows
  the USB voltage. An ammeter in series drops the voltage; under the threshold the AXP limits
  the USB current and the battery quietly supplies the rest, so readings plateaued near 28 mA
  (even during a Wi-Fi OTA).

## 0.2.48 — 2026-11-25

- Measure mode: tapping the *Powered* line switches the SD card rail (ALDO1) by hand, to check
  with a meter whether it really powers the card (a parked card with every rail off still added
  about 25 mA on Seb's watch). The average-consumption line reads "read the USB meter" while
  measuring instead of "unplug to measure".

## 0.2.47 — 2026-11-25

- *Energy* screen, new last line: measuring with a USB meter on the cable. A tap stops battery
  charging and lets the watch sleep while plugged in (normally it never sleeps on USB, to keep
  the console). The cable then powers the watch alone, so the meter reads its real consumption
  without waiting for a full battery. Not saved; unplugging the cable or another tap turns
  charging back on.

## 0.2.46 — 2026-11-25

- *Energy* screen: the SD card part of the *Powered* line also gives how much of the time the
  card has been powered since boot and how many times it was woken, to check the idle mode.

## 0.2.45 — 2026-11-25

- SD card powered only while it is used. A mounted card sitting idle drew about 40 mA (measured
  by Seb: without the card, the overnight drain disappears). After 30 s without access the card
  goes *idle*: the FAT is unmounted and its rail (ALDO1) cut; the next read or write (log and
  MeshCore archive flush every 10 min, web server, history) powers it and remounts it in a few
  hundred ms. For the rest of the firmware the card stays mounted.
- The SD rail no longer follows the LoRa radio. When the radio starts, and after the card goes
  idle, the radio checks it still answers on the shared SPI bus with the card unpowered; if it
  does not, the rail stays on until reboot as before. The 30 s radio heartbeat repeats the
  check and switches the rail back on if the radio goes mute.
- *Energy* screen: "SD card idle" or "SD card active" after the powered parts.

## 0.2.44 — 2026-11-25

- *Energy* screen laid out again as one scrolling column: every line wraps to its own height,
  so nothing is cut off or drawn over another line. Everything in the normal text size and in
  white for easy reading: battery at the top, then the consumption diagnostics (sleep, LoRa,
  powered parts, Bluetooth, screen, previous discharge, animation).

## 0.2.43 — 2026-11-25

- *Energy* screen scrolls vertically: in 0.2.42 the new LoRa and *Powered* lines ran past
  the bottom of the screen.

## 0.2.42 — 2026-11-25

- *Energy* screen, power diagnostics for the overnight drain (about 50 mA with the watch
  asleep 97 % of the time, so something stays powered or transmits):
  - LoRa line: packets sent and the total transmit time since boot, its share of the time
    and what it costs on average (the SX1262 draws about 120 mA at 22 dBm), packets
    received, and whether MeshCore relays other nodes' packets (*repeat*, set from the app).
  - *Powered* line: the AXP2101 rails that are on (LoRa, SD, NFC, sensor, GNSS, amp), Wi-Fi
    when its driver runs, touch when double tap is on, and whether the SD card is mounted.
  - The sleep line shows whole wakeups per minute so it fits on one line.
- The XL9555 expander pins wired to nothing (P00-P05, P11, P14-P17 on the V1.0 schematic)
  are driven high instead of left as floating inputs.

## 0.2.41 — 2026-11-24

- Clock: the time no longer goes back at boot. MeshCore assumes a board without an RTC and,
  when it starts, sets the clock to the last contact change + 1 s
  (`bootstrapRTCfromContacts()`), which overwrote the watch's valid time and RTC with the
  time of the last advert received (Seb saw 22:10 instead of 22:36 after a restart, fixed
  once the iPhone synced the time). The MeshCore clock now never moves a valid RTC
  backwards; the companion app can only move it forward anyway, and the iPhone time sync
  (CTS) still corrects it both ways.

## 0.2.40 — 2026-11-24

- Analog faces (Pub, Retro): the hands no longer jump when the screen wakes. LVGL timers do
  not run while the screen is off, so the baked face image still showed the hands from the
  moment the watch went to sleep, and the first frame painted before DISPON was stale until
  the bake timer caught up. The wake path now re-bakes a stale face image right away
  (`face_bake_flush()`, app task) before that first frame.

## 0.2.39 — 2026-11-24

- Speaker protection: every stream that follows the global volume (web radio, WebRX, demo,
  flight game, Púca) now goes through a 4th-order Butterworth high-pass at 250 Hz
  (-24 dB/octave) before the soft limiter. The small speaker cannot reproduce bass, but bass
  and sub-bass drove it into saturation (Seb, web radio). Two float biquads per sample, no
  extra RAM. The relay tone stream (fixed level) is left untouched.
- Demo and flight game much louder: their own gain was a leftover from before the global
  volume (×0.6 on a quiet mix, about -26 dBFS RMS for the demo and -36 dBFS for the game).
  With the bass filtered out, the demo now plays at about -17 dBFS RMS and the game music at
  about -19 dBFS, with the sound effects on top; the global volume does the rest.

## 0.2.38 — 2026-11-24

- Release of 0.2.37 together with the MeshCore hop-count fix of 0.2.27 (merged in parallel).

## 0.2.37 — 2026-11-24 (test)

- Flash and PSRAM clocks raised from 80 to 120 MHz. Both chips are rated for 133 MHz in quad
  mode (Winbond W25Q128JV flash, AP Memory APS6404L PSRAM); on the ESP32-S3 a 120 MHz quad
  PSRAM requires the flash at 120 MHz too. The W25Q128JV needs no high-performance mode or
  dummy-cycle change, so the bootloader is unchanged and OTA works (the app tunes the bus
  itself at start-up). ESP-IDF logs a warning at boot because it does not list this chip.
  Expected: faster PSRAM reads everywhere (analog faces, flight simulator).

## 0.2.36 — 2026-11-24

- Analog faces: Seb measured 0.2.35 at 26.2 ms (Pub) and 21.2 ms (Retro) per frame, down from
  41.7 and 35.2. The baked image is now written straight into the render strip (solid tiles
  filled, detailed rows copied) instead of going through hundreds of LVGL image and rectangle
  draw tasks per frame. LVGL's own path is kept for screen fades (inherited opacity).
- The once-a-minute re-render of a face also waits while a finger is on the screen, so a
  swipe that has not yet become an animation is no longer interrupted by a ~50 ms render.
- Benchmark (*Énergie* screen): columns are now T (all), L (without lines: seconds hand),
  I (without images, including the baked face), V (empty screen), plus D, the share of
  detailed tiles in the baked image.

## 0.2.35 — 2026-11-24

- Analog watch faces (Retro and Pub): the 0.2.34 benchmark showed about 35–42 ms per frame
  while sliding, 18 ms of it reading the decor image from PSRAM and 12–19 ms drawing the
  thick round-capped hands. Everything that changes at most once a minute (hour, minute and
  battery hands, date and status windows, labels, badges) is now part of the baked image,
  re-rendered on change (once a minute, never during an animation). Only the small seconds
  hand is still drawn live; it now sits above the main hands.
- The baked image is split into 16×16 tiles: uniform tiles (the Pub green, the Retro ivory,
  the black corners) are filled directly without reading PSRAM; only detailed tiles are read.
- The benchmark's "I" column no longer hides the baked face (it is not an image object).

## 0.2.34 — 2026-11-24

- 0.2.32 and 0.2.33 did not boot: baking the watch-face decor ran a full render from the
  `main` task at start-up, whose 3.5 KB stack overflowed. The image is now rendered from an
  LVGL timer, in the `app` task (8 KB, the one that draws the screen); until then the decor
  is drawn as objects.

## 0.2.33 — 2026-11-24

- *Énergie* screen: touching the "Animation" line runs a watch-face benchmark. It renders the
  current face six times without sending anything to the panel and shows the best render
  time for: everything (T), without line objects such as hands (L), without images such as
  the baked decor (I), without both (LI) and an empty screen (V). Also logged as `bench`.

## 0.2.32 — 2026-11-24

- Analog watch faces (Retro and Pub): sliding to or from the face was still choppy (about
  48 ms per frame, against well under 20 ms on the Digital face). Their fixed decor (dial and
  gradient, rims, minute tracks with 61 ticks, sub-dial scales, harp, shamrock, printed
  labels) is now drawn once into an RGB565 image in PSRAM (≈ 400 KB) when the face is
  created; only the live parts (hands, date and status windows, fuel/reserve label, badges)
  are still drawn every frame. Needs `CONFIG_LV_USE_SNAPSHOT`.

## 0.2.31 — 2026-11-24

- "Pub" watch face: slides over it (pulling the menus) dropped to 2–3 frames since 0.2.28.
  LVGL now draws the screen in 16-line strips and calls the face's decor callback for each
  strip; the ~250 lines, discs and arcs of the decor were all queued as draw tasks for every
  strip (about 8000 per frame, each allocated in PSRAM). Only the primitives that touch the
  strip are submitted now, and the 60 index positions are computed once.

## 0.2.30 — 2026-11-24

- No more tearing on heavy screens (e.g. pulling menus over the watch face). A full-screen
  frame that takes longer to draw than the panel's 16.7 ms scan now starts just before the
  next TE, early enough that the scan never catches up with the write (tear-free up to about
  32 ms per frame, i.e. 30 fps). Light frames still start on the TE (60 fps).
- Flight game: the landscape image sits in PSRAM (the largest free internal RAM block is
  46 KB, the image is 69 KB). Columns are now drawn into a small internal tile per core
  (6 KB each, only during the game) and copied row by row, instead of touching a different
  PSRAM cache line for every pixel.

## 0.2.29 — 2026-11-24

- *Énergie* screen: the animation frame rate is now measured between consecutive frames of
  the animation (0.2.28 divided by a whole second that included still time, so it read ~9).
- Flight game: HUD text is skipped on the screen strips it does not touch (each frame is
  sent in 32 strips and every text used to be walked for each one), so a frame goes out faster.
  The pause screen also shows the CPU clock, where the landscape image lives (internal RAM
  or PSRAM) and the largest free internal RAM block, to find what slows the rendering.

## 0.2.28 — 2026-11-24

- Smoother LVGL animations (screen slides, scrolling, MeshCore tabs). LVGL now draws straight
  into the two small DMA strips in internal RAM while the previous strip is being sent to the
  panel, instead of drawing a full 400 KB image in PSRAM and reading it back. PSRAM is much
  slower than internal RAM for pixel work, which held animations to roughly 15–20 frames per
  second. Same internal RAM budget (2 × 13 KB strips, was 2 × 10 KB), 400 KB of PSRAM freed.
  Full-screen frames still start on the panel's TE signal.
- *Énergie* screen: new line with the frame rate and refresh time of the last animation
  (also logged on the console), to compare builds.
- Flight game pause screen: average render and send times per frame, next to the frame rate.

## 0.2.27 — 2026-11-24

- MeshCore app: messages relayed by repeaters using 2- or 3-byte path hashes showed a wrong
  hop count (66 or 67 for 2 or 3 hops). The path length byte carries the hash size in its
  top 2 bits; the watch now keeps only the hop count. Messages already stored in the history
  or archived on the SD card are corrected when read. Frames sent to the companion app are
  unchanged (the app decodes the byte itself).

## 0.2.26 — 2026-11-24

- New *Settings › Wi-Fi › Turn Bluetooth off* (off by default, also on the web page): while
  Wi-Fi is on, Bluetooth is paused so Wi-Fi gets the whole radio (with both on, the
  ESP32-S3 shares its antenna and Wi-Fi misses beacons: pings of several seconds). Bluetooth
  comes back by itself when Wi-Fi stops. Turning Bluetooth on by hand still wins.
- Firmware update through the web server: Bluetooth is paused while the image arrives
  (LoRa MeshCore keeps receiving), and comes back at once if the update fails.

## 0.2.25 — 2026-11-24

- Web page: upload progress (firmware and SD files) showed "%%" instead of "%".

## 0.2.24 — 2026-11-24

- Wi-Fi with Bluetooth still answered slowly after 0.2.22. While the Wi-Fi radio runs, the
  Bluetooth controller's modem sleep is now off too (`ble_wifi_coex()`), so the coexistence
  arbiter keeps its timing on the access point's beacons. It comes back when Wi-Fi stops.
- Wi-Fi scans with Bluetooth on use the default active scan times, as the coexistence
  requires (the driver warned about the custom ones).

## 0.2.23 — 2026-11-24

- Crash (LoadStoreError in LVGL) when Bluetooth was turned back on after being turned off,
  for example from the control center after turning Wi-Fi off. NimBLE frees its event pool
  when Bluetooth stops, but the watch's own events kept their old pointer, and the next start
  wrote into memory that LVGL had taken back. The events are now released when Bluetooth
  stops (`ble.c`, `ancs.c`), and nothing is posted to the stack while it is stopping.

## 0.2.22 — 2026-11-24

- Wi-Fi with Bluetooth on answered in bursts (pings of 1 to 17 s, "host unreachable"):
  in light sleep with both radios the watch missed the access point's beacons. While the
  Wi-Fi radio runs with Bluetooth on, light sleep is now off (the Wi-Fi modem still sleeps
  between beacons and the CPU still scales down). Without Bluetooth, light sleep stays.
- Screen timeout: in apps, settings pages and Now playing the screen stays on for 1 min
  without a touch (or the longer setting), instead of the short watch-face delay (8 s).

## 0.2.21 — 2026-11-24

- Global Wi-Fi (`wifi_mgr.c`): one owner of the Wi-Fi driver. On/off from a new control-center
  tile (long press opens the Wi-Fi settings; Settings moved to a gear, top right) and from
  *Settings › Wi-Fi*, kept across restarts. Up to 5 saved networks (was 3), tried in order or a
  chosen one; automatic retries (5 s … 5 min, radio off during long waits). WebRX, Radio and the
  Wi-Fi analyzer reuse the connection when it is up, or hold Wi-Fi while they run.
- Wi-Fi turns off when the battery drops below 30 % on battery (switchable, on by default).
- Bluetooth coexistence: the global switch never stops Bluetooth; only WebRX, Radio and the
  analyzer may still pause it when internal RAM is short, and it comes back when they quit.
- Web server (off by default, *Settings › Web server*): status, settings, firmware update
  without turning Bluetooth or MeshCore off, Wi-Fi/WebRX/radio lists, and SD card files
  (optional: browse, download, upload ≤ 64 MB, new folder, delete; `/meshcore` read-only).
  Challenge-response login with a random password shown on the watch, HttpOnly SameSite
  session, lockout after failed attempts, CSRF header + Origin and Host checks, CSP. httpd
  stack in PSRAM; flash writes go through an internal-stack worker.
- MeshCore TCP server on port 5000 (*Settings › MeshCore*, off by default), same framing as
  the upstream `SerialWifiInterface`, for the MeshCore app (TCP/IP) or meshterm; works next
  to a Bluetooth app.
- FATFS API in UTF-8 (SD file names with accents over the web).
- Docs: `docs/fr|en/wifi.md`, new control-center screenshots.

## 0.2.20 — 2026-11-24

- MeshCore archive on the SD card in JSON Lines (`/meshcore/c-<key>.jsonl`, `g-<key>.jsonl`),
  readable on a computer: one message per line, padded to 512 bytes so the watch can still seek
  to older messages. Channel messages carry the sender in its own field. A line cut by a power
  loss is padded over before the next append. The old `.bin` files are no longer read (no
  conversion, as Seb asked).
- Fewer SD writes: log and archive are written every 10 min instead of every 1–5 min, sooner
  when the log buffer (now 64 KB of PSRAM) is half full, and before a restart (menu or update),
  an eject or sharing.
- Time sync log line reads "fuseau UTC+1:00" instead of "décalage +60 min".

## 0.2.19 — 2026-11-24

- SD card shared with a computer over USB: *Settings › SD card › Share with the computer*
  (shown while plugged in) unmounts the card on the watch and presents it as a USB drive
  (TinyUSB mass storage, read and write). Ejecting the drive on the computer, *Stop sharing*,
  unplugging or pulling the card ends it and the watch mounts the card again. The USB serial
  console is off while sharing. TinyUSB buffers live in PSRAM; 4 KB of internal RAM is only
  taken while sharing.
- SD card battery savings: with the screen off the card-detect line is read every 15 s instead
  of 3 s, buffers are written every 5 min instead of every minute (sooner if the log buffer is
  half full), the log flush no longer touches the card when there is nothing to write, and free
  space is only recomputed when the SD card page is opened.

## 0.2.18 — 2026-11-24

- MeshCore history from 0.2.13 and earlier is taken over (it was ignored, so channels
  looked empty after the update): messages are marked read and attached to their contact
  or channel by name. Messages still waiting in the iPhone app's queue but missing from
  the watch history are copied into it too, unread.
- SD card already in the watch at power-up: its rail is cut briefly before the radio
  starts (the AXP2101 keeps it on across a restart, leaving the card in the state the
  previous run left it), and mounting is tried three times, a second apart, before the
  card is called unreadable.
- SD card detection was inverted (the detect contact reads low when a card is in):
  a card in place was seen as absent, and a removed one as present.

## 0.2.17 — 2026-11-24

- 0.2.16 merged with 0.2.14 (Energy screen: light sleep share and wakeups). The *sd* task
  that polls the card-detect line every 3 s shows up in the wakeups per minute.

## 0.2.16 — 2026-11-24

- MeshCore app: contact and channel rows stay on one line (the last-message preview
  wrapped and overlapped the next row); channel sender names no longer collapse to "…";
  messages heard with 0 hops show as *direct*.

## 0.2.15 — 2026-11-24

- MeshCore app rebuilt with tabs: *Contacts* (chat and room contacts), *Channels* and
  *Radio* (radio status, counters). Lists are sorted alphabetically (leading emoji and
  punctuation ignored, case and accents ignored), with conversations that have unread
  messages first; each row shows the last message, its time and an unread badge, and the
  tab titles carry the unread count. Touching a row slides the conversation in: bubbles,
  oldest at the top, sender name on channels, our own messages (sent from the iPhone app)
  on the right, an *Unread* marker, and *Older messages* to read further back (ring, then
  SD card).
- Read state: opening a conversation on the watch marks it read; a message the MeshCore
  app has fetched, or a conversation the app just wrote in, becomes read on the watch.
  Messages read on the watch stay in the app's queue and still reach the app (the MeshCore
  protocol has no way to tell the app a message was read, so they show as new there).
- Watch history: a 128-message ring in PSRAM, saved to internal flash, with the read state
  and messages sent from the app. With an SD card, every message is also archived on it,
  one file per conversation (`/meshcore`), and older messages are read back from it.
- SD card: detected when inserted (the card-detect line is polled every 3 s, its interrupt
  is not wired), mounted at once, unmounted when pulled out. *Settings > SD card* shows the
  free space and offers *Eject card* (buffers written first, then unmounted) and *Mount
  card again*. A card in the watch is now always initialised, so it no longer sits in its
  higher-current power-up state while the radio keeps its rail on.
- Logs on the SD card: every log line also goes, time-stamped, into a 32 KB PSRAM ring
  written to `/logs/YYYY-MM-DD.log` every minute (without a card, the last 32 KB are kept
  and written when one is inserted).
- SD and NFC rails are now shared by the radio and the SD card (reference counted), so
  the radio stopping no longer cuts the card's power and the reverse.

## 0.2.14 — 2026-11-24

- *Energy* screen: a new line shows, while the screen is off, how much of the time the
  watch actually spends in light sleep, how many times per minute it wakes up and, when
  MeshCore runs, how many LoRa interrupts the radio has raised. The AXP2101 cannot
  measure current, so this tells what keeps the watch awake when the average draw looks
  too high.
- Checked against the T-Watch Ultra V1.0 schematic: pins, expander bits and power rails
  all match. Two small savings come out of it: the PDM microphone sits on the always-on
  3.3 V rail, so its clock input is now pulled low instead of left floating, and the
  unused DLDO2 rail is switched off explicitly.

## 0.2.13 — 2026-11-23

(0.2.12 was a test build only: fast link and no light sleep only while frames flowed.)

- MeshCore app over Bluetooth: while the app is connected, whatever the watch is doing
  (screen on or off), the link stays fast (15–30 ms, no latency), light sleep is held off
  and the CPU never drops below 80 MHz, so the watch answers the app at once. Before, it
  went into light sleep between connection events and down to 40 MHz, and the link with
  the app was unstable. When the app leaves, the economical parameters (as for iPhone
  notifications) and light sleep come back. The app is only opened now and then (the watch
  shows received messages itself), so the battery cost stays limited.
- BLE journal (*About*): switches to the fast MeshCore link, and frames from the app the
  watch had to refuse.

## 0.2.11 — 2026-11-23

- Control center everywhere: pull down from the top edge of the screen (start within the
  top ~48 px, drag ~60 px down) in any app or menu. It opens on top of the app without
  quitting it (the radio keeps playing) and closes back onto it (swipe up or right, or the
  button). On the watch face, a swipe down anywhere still opens it.
- Global volume (0–100 %, steps of 10) and a *Silent* tile in the control center. Every
  sound goes through it: beeps, timer alarm, Púca, game music, Radio and WebRX (whose own
  volume now sets their level relative to the global one). *Tone* is the one exception:
  its relay tone keeps its fixed level. Silent mode keeps the amplifier off; vibrations
  still work. The digital face shows a muted speaker in silent mode.
- Control center: six tiles (Bluetooth, Do not disturb, Silent, Torch, Now playing,
  Settings). Now playing, Settings and Torch quit the app the panel was pulled over.

## 0.2.10 — 2026-11-23

- Raise to wake works again: the motion sensor goes back to LilyGoLib's orientation (180°
  about Z), which Bosch's wrist gesture relies on; 0.2.6's change had broken it. The flight
  game's up/down is now set in the app, so steering stays as in 0.2.9.
- Serial log: each wrist gesture seen by the sensor, and the accelerometer values that
  accepted or refused it.

## 0.2.9 — 2026-11-23

- *Pub* face: rim radius 118, between the panel's 100 and 0.2.8's 135.
- Wi-Fi update: the image's whole size is erased once, before the transfer, instead of
  sector by sector along the way. Each erase stopped the flash cache for tens of
  milliseconds, the Wi-Fi dropped packets and TCP slowed down or paused. The transfer now
  starts after a few seconds of preparation and should run evenly.
- MeshCore keeps receiving during a Wi-Fi update (it was already running; the iPhone app
  simply cannot connect while Bluetooth is off). Before every restart (end of update,
  cancelled update, *Restart the watch*) MeshCore is stopped cleanly, so the app's queue and
  the watch's message history are saved.
- The watch's MeshCore history (64 messages) is now kept on flash and comes back after a
  restart; it is written after 30 s without new messages, or when MeshCore stops.

## 0.2.8 — 2026-11-23

- *Pub* face: rounder corners (brass rims radius 135 instead of following the panel's 100,
  rounder minute track) and a slightly darker bottle green.

## 0.2.7 — 2026-11-23

- MeshCore app: the status line no longer runs off the rounded edge (frequency, SF and app
  state; the node name is left out), the radio line shows the noise floor without a label.
- Settings: *External antenna* label fits on one line; Wi-Fi power buttons show plain numbers,
  the unit moves to the section title.
- Documentation: MeshCore guide (en/fr) with screenshots, settings, hardware pitfalls (shared
  SPI bus, antenna switch polarity, XL9555 state, sensor orientation), versioning, MeshCore in
  the third-party notices; watch face and screen captures refreshed.

## 0.2.6 — 2026-11-23

- Motion sensor: orientation fixed (front/back and face up/down were inverted). Tilt in the
  flight simulator follows the wrist, and the sensor's wrist-raise gesture sees the right side.
- Louder sound: beeps +10 dB, Púca sounds +4 dB, radio and web radio up to ×4 at 100 % with a
  soft limiter instead of hard clipping.

## 0.2.5 — 2026-11-23

- Display: LVGL renders into a full frame in PSRAM; two small internal DMA strips stream it to
  the panel (byte swap during the copy). A full frame leaves in one go, started on TE and
  faster than the panel scan, so tearing no longer depends on render time. 13 KB of internal
  RAM freed (19.7 KB of strips instead of 32 KB of draw buffers).
- *Pub* face 4 px smaller again (rims and minute track).

## 0.2.4 — 2026-11-23

- *Pub* face slightly smaller: brass rims 4 px further in, minute track 6 px further in.
- Display bus at 80 MHz (as LilyGoLib): a full frame takes about 10.5 ms, faster than one panel
  scan, so slides stay smooth and, starting on TE, no longer tear. The watch face slides again.

## 0.2.3 — 2026-11-23

- Leaving or returning to the watch face switches at once instead of sliding: a full frame
  takes about 21 ms at 40 MHz, longer than one panel scan, so the slide stuttered and tore.
- Large redraws start on the panel's TE signal, which limits tearing on the other screens.

## 0.2.2 — 2026-11-23

- WebRX: the waterfall stops just above the − ■ + buttons instead of running under them.

## 0.2.1 — 2026-11-23

- Internal RAM freed for audio (web radio and WebRX had no DMA memory left with MeshCore on):
  stacks of the radio, WebRX audio, LoRa, sound, flight-sim, demo and DNS tasks in PSRAM
  (`task_psram.h`: tasks that never touch flash); MeshCore packet pool, queues and BLE buffer
  in PSRAM; large static buffers moved to PSRAM.
- Settings: new *Wi-Fi* page (transmit power 8, 13, 17 or 20 dBm, applied at once) and
  *MeshCore* page (on/off, built-in or external LoRa antenna).
- MeshCore task logs its stack margin and free internal RAM every 2 minutes.

## 0.2.0 — 2026-11-23

First version tested on the real watch.

- **MeshCore** (companion v1.18) on the SX1262, EU/UK Narrow preset by default (869.618 MHz,
  BW 62.5 kHz, SF8, CR 4/8). The official MeshCore app connects over Bluetooth (NUS service)
  and configures the radio. Messages read on the watch stay in the offline queue for the app;
  the queue survives reboots. *MeshCore* app on the watch: received messages with emoji,
  radio and app status. On/off in Settings › Bluetooth.
- Display: the panel supply (XL9555 pin 7) is power-cycled at boot. The expander keeps its
  state across ESP resets, so the AMOLED stayed black while its logic ran. Bus at 40 MHz.
- Visible panel area measured with a test pattern (`DISPLAY_GEOM_TEST`): the *Pub* face rim
  follows it; flat background instead of a banded RGB565 gradient.
- Boot after an OTA update: GPIO interrupts left armed by the previous firmware are cleared.
- Internal RAM: LVGL renders from flash, 20-line draw buffers. Web radio and WebRX play again
  (the I2S and Wi-Fi ran out of memory).
- BHI260AP: no more endless FIFO loop (task watchdog) on an event of unknown size.
- LoRa: the SD card and NFC rails are powered (chip selects held high) while the radio runs.
  Unpowered, they clamped the shared SPI bus and the SX1262 never answered.
- LoRa antenna selectable in Settings › Bluetooth: built-in (XL9555 pin 11 low, checked on
  the watch) or external. MeshCore app shows TX/RX counters and the noise floor.
- Emoji: the decompressor state no longer sits on the UI task stack (overflow on the first
  emoji drawn).
- Light sleep keeps display, SPI and LoRa pins.
- USB plugged in: no light sleep, the serial console stays alive.
- WebRX waterfall fills the screen height. Default radio volume 50 %.
- Bluetooth name `T-Watch-Ultra`, Wi-Fi access point `twatch-ultra-OTA`.

## 0.1.0 — 2026-11-23

First version for the **LilyGo T-Watch Ultra**, ported from
[twatch-idf](https://github.com/armSeb/twatch-idf) 0.33.1 (T-Watch 2020 V1).

- Hardware: ESP32-S3, CO5300 AMOLED 410×502 over QSPI (TE-synchronised frames, even-aligned
  windows), CST9217 touch, AXP2101 PMIC (every unused rail off), BHI260AP smart sensor (steps
  and raise to wake inside the sensor), DRV2605 haptics, PCF85063 RTC, XL9555 expander,
  MAX98357A speaker. Bluetooth low-power clock on the 32 kHz crystal: light sleep with the
  iPhone connected.
- UI laid out ×1.5 inside a safe area clear of the panel's rounded corners; fonts regenerated
  at 17/24/30/66/108 px. The *Pub* face covers the whole panel and follows its rounded edge.
- **Full UTF-8 and colour emoji**: Montserrat + DejaVu Sans (Latin, Vietnamese, Cyrillic,
  Greek, symbols), 1,949 Twemoji images (flags, keycaps, ZWJ sequences) in their own flash
  partition, in notifications, the banner, calls and now playing.
- Games full size: *Soar* renders over the whole panel, the demo is tripled to 360×360,
  *Púca*'s scene is scaled ×4.
- Energy screen rewritten for the AXP2101 (fuel gauge, estimated consumption).
- 16 MB flash layout: two 5 MB OTA slots, 3 MB emoji partition, 2.8 MB FAT storage.
- Removed (no hardware on the Ultra): SDR, Carrier, TV off, Remote.
