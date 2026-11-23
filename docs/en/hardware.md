# Hardware notes — T-Watch Ultra

[Français](../fr/hardware.md) · English

References: [LilyGoLib](https://github.com/Xinyuan-LilyGO/LilyGoLib) (board definition for
the T-Watch Ultra), [SensorLib](https://github.com/lewisxhe/SensorLib) and
[XPowersLib](https://github.com/lewisxhe/XPowersLib). All pins live in `main/board.h`.

ESP32-S3 with external 16 MB flash and 8 MB QSPI PSRAM, and an
external 32.768 kHz crystal used as the Bluetooth low-power clock.

## Pins

| Function | Pins |
|---|---|
| CO5300 AMOLED 410×502 (QSPI, SPI2) | CS 41, SCLK 40, D0–D3 38/39/42/45, RST 37, TE 6 |
| I2C (all chips) | SDA 3, SCL 2 — CST9217 touch `0x1A`, XL9555 expander `0x20`, BHI260AP `0x28`, AXP2101 `0x34`, PCF85063 RTC `0x51`, DRV2605 haptic `0x5A` |
| Interrupts | RTC 1, NFC 5, PMU 7, BHI260AP 8 (active high), touch 12, LoRa DIO1 14 (active high); the others are active low |
| Shared SPI (SPI3) | MOSI 34, MISO 33, SCK 35 — SD card CS 21, NFC (ST25R3916) CS 4, SX1262 CS 36 / RST 47 / BUSY 48 |
| MAX98357A I2S amplifier | BCLK 9, WCLK 10, DOUT 11 |
| PDM microphone | CLK 17, DATA 18 |
| GNSS u-blox MIA-M10Q | TX 43 (ESP → GNSS), RX 44, PPS 13 |
| BOOT button | 0 (active low); the side button is the AXP2101 power key |

**XL9555 expander**: DRV2605 enable (6), display power (7), touch reset (8), SD card detect
(10), LoRa antenna switch (11).

## AXP2101 power rails

| Rail | Feeds | Firmware |
|---|---|---|
| DCDC1 3.3 V | ESP32-S3 | always on, never touched |
| ALDO1 3.3 V | SD card | off until used; on while the LoRa radio runs (shared SPI bus) |
| ALDO2 3.3 V | display | on |
| ALDO3 3.3 V | SX1262 LoRa | on only while MeshCore runs |
| ALDO4 1.8 V | BHI260AP | on only if raise to wake or the step counter is on |
| BLDO1 3.3 V | GNSS | off until used |
| BLDO2 3.3 V | MAX98357A amplifier | on only while a sound plays |
| DLDO1 3.3 V | NFC | off until used; on while the LoRa radio runs (shared SPI bus) |
| VRTC | GNSS backup | always on |
| DCDC2–5 | not used | off |

**The AXP2101 keeps its registers as long as the battery is connected**: a previous firmware
that left a rail on leaves it on for the next one, so every rail and voltage is set
explicitly at boot. The pins of an unpowered chip are left floating (neither input nor
output): an I/O held at 3.3 V towards an unpowered chip feeds it through its protection
diodes.

The AXP2101 has no battery current measurement: the average consumption shown on the
*Battery* screen is estimated from the fall of its fuel gauge percentage (1 % ≈ 11 mAh on
the 1100 mAh cell), so it needs a few hours. The charge current is 400 mA (below 0.5 C, and
the AXP2101 heats up above 500 mA).

## Display

- The CO5300 is driven like an SH8601: a register is written with instruction `0x02` on one
  line then the address `0x00 RR 00`; pixels go with instruction `0x32`, address `0x002C00`,
  data on 4 lines. `esp_lcd` does exactly that in `quad_mode` with a 32-bit command.
- The panel sits in the middle of the CO5300's RAM: columns 22 to 431 (`BOARD_LCD_GAP_X`).
- The write window must start and end on even columns and rows: LVGL's invalidated areas are
  rounded (`LV_EVENT_INVALIDATE_AREA`).
- Frames start on the TE signal, so the panel never shows a half-drawn frame. At 80 MHz on
  4 lines, a full frame (≈400 KB) takes about 10 ms.
- No backlight: brightness is a CO5300 register. Black pixels are off.
- **Rounded corners**: the panel's corner radius is about 84 px (`BOARD_LCD_CORNER_R`, to be
  confirmed on the watch). Screens keep their content inside a safe area 25 px from the sides
  and 30 px from the top and bottom (`BOARD_SAFE_INSET_X/Y`); the *Pub* face and *Soar* use
  the full panel.

## Touch (CST9217)

Registers on 16 bits: read 12 bytes at `0xD000`, then acknowledge with `0xAB`. Byte 6 must
be `0xAB`, byte 0 low nibble `0x06` means "pressed", byte 5 holds the number of points.
Reset through the XL9555.

## Motion (BHI260AP)

The BHI260AP is a programmable sensor hub: the Bosch firmware (`components/bhy2/`) is
uploaded into its RAM at boot (about 1 s), then the step counter and the wrist-tilt gesture
run inside it. Its step counter cannot be reset without restarting that firmware, so the
watch keeps the value at its last software reset and subtracts it.

The chip is turned 180° about Z (LilyGoLib's `TOP_LAYER_BOTTOM_RIGHT_CORNER`): the orientation
matrix given to the accelerometer and gyroscope is `{ -1, 0, 0,  0, -1, 0,  0, 0, 1 }`. Bosch's
wrist gesture relies on that frame (Z positive with the screen facing up); 0.2.6 tried a 180°
turn about Y instead, which broke it. The flight game's pitch sign is set in the app.

## Pitfalls

- **NimBLE host-based privacy**: reconnection fails after the iPhone rotates its private
  address (~15 min, SMP timeout). Address resolution is left to the controller.
- **`BT_NIMBLE_GATT_CLIENT` depends on `BT_NIMBLE_ROLE_CENTRAL`** in ESP-IDF 5.5: disabling
  the central role silently removes `ble_gattc_*`, hence ANCS.
- **iOS sometimes ignores the Security Request**: the watch sends it 500 ms after connecting
  and drops the link if encryption has not happened 8 s later, then advertises again.
- **Connection parameter collisions** (0x22A) when the watch asks for economical parameters
  while iOS changes them: retried with back-off until accepted.
- **Cancelling an OTA upload**: `httpd_stop()` waits for the upload handler, which keeps
  receiving while the phone sends, and the watch freezes. It restarts directly instead.
- **The XL9555 keeps its outputs across ESP32 resets**: the panel can stay black after a
  reset. The display power pin (7) is cut and restored at boot.
- **LoRa antenna switch (XL9555 pin 11)**: LOW selects the built-in antenna, HIGH the external
  one on the USB-C connector, the opposite of LilyGoLib's comments (checked on the watch).
- **SX1262, SD card and NFC share SPI3**: an unpowered SD card or NFC chip pulls the bus down
  through its protection diodes and the SX1262 never answers (status 0x00). While the radio
  runs, both rails stay on with their chip selects held high.
- **SX1262 settings** (as Meshtastic's t-watch-ultra variant): TCXO at 1.8 V on DIO3, DIO2 as
  RF switch, DC-DC regulator, boosted receive gain.
- **Full image at 0x0 erases NVS** (pairings). For USB updates, write the app at `0x20000`
  with `ota_data_initial.bin`.

## Still to check on the watch

Corner radius, panel orientation, touch coordinate mapping, panel reset timing, real
standby consumption.
