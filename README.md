# pixeld-h6631

**Unofficial custom firmware for the Govee H6631 Gaming Pixel Light** (52×32 RGB pixel display).
It replaces Govee's apps with **pixeld**, a small local-only display daemon:
- no cloud connection;
- Home Assistant over MQTT, either directly or through
  [pixel2mqtt](https://github.com/SamWiIIiams/pixel2mqtt) (bridge and web editor);
- a LAN API for uploads, previews and live frames.

> Not affiliated with or endorsed by Govee. Installing this means opening the device, using its serial console
> and rewriting its flash. You can brick your unit; you do this at your own risk. The stock firmware stays in
> the other A/B slot, so a rollback is possible (see [docs/INSTALL.md](docs/INSTALL.md)).

## What it does
- **Display modes:**
  - clock: digital, analog, or a clock drawn over one of the animated scenes;
  - scrolling text;
  - images and animations;
  - generative effects: Snake, Bouncing Balls, Match 3, Flow, Fireworks, Starfield, Rain and more;
  - music visualiser, using the built-in microphone;
  - info screens: weather, room climate, stocks, NFL/NBA scores;
  - countdown and stopwatch;
  - a playlist that rotates through all of the above.
- **Home Assistant:** MQTT discovery for a light, mode/animation/effect selects, text, notify, chime volume,
  buttons, countdown/stopwatch, and device triggers for the four physical buttons.
- **Notifications** with a colour, sound and/or animation, and a **doorbell chime** through the speaker
  (with no amplifier pops).
- **Bluetooth LE sensor bridge (`bled`):** passive scanning for Govee, BTHome and ATC/pvvx thermometers,
  published to MQTT.
- **Wi-Fi setup hotspot:** if the display can't reach its network, or you hold the Mode button for 5 s,
  it starts a hotspot with a setup page.
- **LAN API** (HTTP port 80, token pairing via a code on the panel) and raw UDP frames on port 7777.
  See [docs/API.md](docs/API.md).

**No Govee artwork is included.** The display's own firmware still contains Govee's stock animations and icons
in `/usr/res`. [pixel2mqtt](https://github.com/SamWiIIiams/pixel2mqtt) converts them on your device with one
click, using the scene renderer in `tools/graffiti.py`.

## Layout
| Path | |
|---|---|
| `src/pixeld.c` (+ `features.c`, `api.c`) | Display daemon (single-threaded, 30 fps, unity build) |
| `src/bled.c` | BLE thermometer bridge |
| `src/pixeltest.c` | Panel test tool |
| `firmware/` | Rootfs build script, init overlay, and the persistent `/data/custom` scripts (autostart, Wi-Fi manager, chime) |
| `tools/` | PC tools: animation/icon converters, Govee scene renderer, UDP/MQTT test clients |
| `scripts/` | Windows helpers for the serial console and for writing the Wi-Fi/MQTT configs |
| `ha/` | Example Home Assistant package (weather, climate, stocks, scores, notifications) |
| `docs/` | Hardware notes, install guide, API, file formats |

## Building
The device runs Linux 5.4 with glibc 2.29 (armhf, Cortex-A7). [Zig](https://ziglang.org/) 0.13 cross-compiles
for that exact glibc:
```sh
ZIG=~/zig/zig sh build.sh
```
Then copy `build/pixeld` to `/data/custom/apps/pixeld` on the device and restart it (`killall pixeld`;
the autostart loop restarts it).

Two Zig/glibc-2.29 traps:
- `stat()` always fails; use `opendir`, or `open` + `lseek`.
- Absolute times overflow 32-bit `int`/`long` (`(int)(now * 2)`); use `fmod` or relative times.

## Status
Runs daily on one H6631 (firmware 1.00.91, board `H6631_MAIN_V1.4`). Other hardware revisions are untested.

## Licence
MIT (see [LICENSE](LICENSE)). Govee artwork is not covered and is not included.
