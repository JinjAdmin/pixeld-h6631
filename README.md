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

## Installation
This is the overview. The full commands are in [docs/INSTALL.md](docs/INSTALL.md); read it all before you start.

**You need:**
- a **T6H** Torx screwdriver and a plastic pry tool;
- an **ESP32 dev board** to use as a USB-serial adapter (most ESP32 boards work) and four jumper wires.
  This guide is written for an ESP32, but any 3.3 V serial adapter that supports 1 500 000 baud also works:
  USB-serial adapters with an FT232R, CP2102N, CH340 or CH343 chip (not the original CP2102, which is too slow),
  or a Raspberry Pi's GPIO UART (`/dev/ttyAMA0`). With those, wire TX→RX and RX→TX;
- a PC with Linux or WSL, `squashfs-tools` and [Zig](https://ziglang.org/) 0.13;
- about an hour.

### 1. Open the case
<!-- photos to come -->
1. Unplug the power.
2. Remove the **four T6H Torx screws** and take off the bezel.
3. Carefully pry the case in half. It comes apart easily, but go slowly: the halves are joined by
   **two ribbon cables and a small two-wire harness**. Disconnect them before you pull the halves fully apart;
   don't yank them.
4. Leave the cables disconnected. The controller board boots and flashes fine without the LED panel attached;
   you only reconnect it once the new firmware is installed.

### 2. Connect the serial console
The controller board (`H6631_MAIN_V1.4`) has four pads labelled `GND TX RX VCC5V`, next to the 12 V input.
The ESP32 board's own USB-serial chip does the work; the ESP32 itself is held in reset so it stays off the line.
- On the ESP32 board, connect **EN to GND** (holds the ESP32 in reset).
- Connect the display's **GND→GND, TX→TX0 and RX→RX0**. These are *not* crossed, because you're wiring straight
  to the USB chip's lines.
- **Don't connect `VCC5V`.** The display powers itself from its own supply, and the board's pins are 3.3 V.
- Plug the ESP32 into your PC, open its serial port at **1500000 8N1** (PuTTY, `picocom`, or
  `scripts/uart_cmd.ps1`), then plug the display back in. After the boot messages you get a root shell
  (`root@Govee:/#`) with no login.

### 3. Back up, build, flash
All on the serial console, following [docs/INSTALL.md](docs/INSTALL.md):
1. **Back up every flash partition** to your PC. The backup holds your Wi-Fi credentials and device keys, so keep
   it private.
2. Set `bootdelay` to 3, so you can always reach U-Boot and switch back to stock.
3. On the PC, build the custom root filesystem **from your own stock backup** with `firmware/build_rootfs.sh`, and
   build pixeld with `build.sh`. No Govee software or artwork comes from this repo.
4. Write the image to the unused A/B slot. The stock firmware stays untouched in the other slot.
5. Put your Wi-Fi settings, SSH public key and the programs in `/data/custom`.
6. Switch to the new slot and reboot.

### 4. Check it, close it up
- After about 30 s, `ssh root@<display-ip>` should work (key only).
- From here on, updates and recovery work over SSH. Unplug the power, disconnect the serial adapter, reconnect the
  two ribbon cables and the two-wire harness, and screw the case back together. Keep the adapter in case you ever
  need U-Boot to roll back.
- Power it up: the panel shows the clock. If Wi-Fi doesn't connect, the display opens a `govee-pixel-setup`
  hotspot and shows its password on the panel.

### 5. Home Assistant
Install [pixel2mqtt](https://github.com/SamWiIIiams/pixel2mqtt) and adopt the display (it shows a pairing code).
Then click **Import built-in animations & icons** to convert the stock artwork that's already on your device.

**Going back to stock** is one command that switches the boot slot back
(see [Rollback](docs/INSTALL.md#rollback-to-stock)).

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
