# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

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
