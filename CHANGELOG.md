# Changelog

All notable changes. Versions follow `PROJECT_VER` in `CMakeLists.txt`.

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
