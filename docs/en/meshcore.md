# MeshCore — LoRa messages from the watch

[Français](../fr/meshcore.md) · **English**

The watch is a **MeshCore companion node**: its SX1262 LoRa radio joins the
[MeshCore](https://meshcore.co.uk) mesh, and the official **MeshCore app** on the iPhone
connects to it over Bluetooth to read and send messages, manage contacts and channels, and
set the radio. The protocol is the upstream companion firmware (v1.18.0), so the app sees the
watch as an ordinary MeshCore device.

![MeshCore app, MeshCore settings, Wi-Fi settings](../img/meshcore.png)

## Turning it on

1. Watch: *Settings › MeshCore*, switch **MeshCore** on. The LoRa radio starts and the watch
   advertises the MeshCore Bluetooth service next to its usual ones.
2. iPhone: the watch must already be paired (*Settings › Bluetooth › Pair an iPhone*, see the
   [README](../../README.md)). Open the MeshCore app and connect to **T-Watch-Ultra**. It is
   the same Bluetooth link as notifications: nothing else to pair.

The first start creates the node's identity (keys) and saves it on the watch. Contacts (up to
350), channels (up to 40) and the radio settings are stored in the `storage` partition and
survive restarts and updates.

## Radio settings

Default preset: **EU/UK Narrow**, 869.618 MHz, 62.5 kHz bandwidth, SF8, CR 4/8, 22 dBm. Every
radio parameter (frequency, bandwidth, spreading factor, coding rate, power) is set **from the
MeshCore app** and saved on the watch; the watch shows them but does not edit them.

**Antenna**: *Settings › MeshCore › External antenna* switches between the built-in antenna
(off) and an antenna on the USB-C connector (on). It applies immediately.

## The MeshCore app on the watch

The **MeshCore** app (first in the app list) shows:

- a status line: frequency and spreading factor, then *app connected*, or the number of
  messages waiting for the app;
- a radio line: packets sent (`TX`, with failures), received (`RX`, with damaged packets),
  and the noise floor in dBm. If the radio could not start, the line says why;
- the last messages, newest first: direct messages under the contact's name, channel
  messages under `#channel`. Emoji and any UTF-8 text display as on the phone.

The watch keeps the last 64 messages in memory and shows 30. **Reading a message on the
watch does not take it away from the app**: it stays in the app's queue (up to 256 messages)
and is delivered when the app connects.

When MeshCore is off, the app offers a button to turn it on.

## Power

With MeshCore on, the radio listens all the time: battery life drops noticeably. Turn it off
in *Settings › MeshCore* when you do not need it; the radio's power rail is then cut.

The SX1262 shares its SPI bus with the SD card and the NFC chip: while the radio runs, their
rails stay on with their chip select held high, otherwise the unpowered chips pull the bus
down and the radio does not answer (details in [hardware notes](hardware.md)).

## Troubleshooting

| What you see | Meaning |
|---|---|
| *LoRa radio stopped: ESP_ERR_NOT_FOUND* | The SX1262 does not answer on the SPI bus. Turn MeshCore off and on; the serial log (`sx1262:` lines) gives the chip status. |
| `TX` grows but nobody hears you | Check the antenna setting, and that the preset matches your local mesh. |
| `RX` stays at 0 | Same; a noise floor around −120 dBm is normal, much higher means local interference. |
| The app does not see the watch | MeshCore must be on, and the watch paired with this iPhone. |

## Not yet

Message alerts (banner and vibration), history on the SD card, low-power receive, and a
TCP link for desktop clients.
