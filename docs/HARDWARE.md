# H6631 hardware notes

These were found on one unit: board `H6631_MAIN_V1.4`, stock firmware 1.00.91.

## System
- **SoC:** Allwinner T113-S3 (dual Cortex-A7), 128 MB RAM (about 116 MB usable). 512 MB SPI NAND with UBI volumes.
- **Software:**
  - Tina Linux 5.0 (OpenWrt 21.02 based), Linux 5.4.61, BusyBox 1.33.2, glibc 2.29 armhf.
  - The root filesystem is squashfs (read-only), and `/etc` is a tmpfs overlay.
  - Persistent storage is `/data` (rootfs_data, 11 MB) and `/mnt/UDISK` (about 290 MB).
- **Boot:** BOOT0 → OP-TEE → U-Boot 2018.07 → kernel. A/B slots are selected with the env variables
  `boot_partition`, `root_partition` and `systemAB_next`.
- **Serial console** (`GND TX RX VCC5V` pads): BOOT0 runs at 115200 baud, everything after it at 1500000. It ends
  in a root shell with no login. Micro-USB (OTG) exposes adbd, but that asks for a login.
- **Wi-Fi/BT:** RTL8723DS combo. BLE is brought up with `rtk_hciattach -n -s 115200 /dev/ttyS1 rtk_h5` after
  resetting the `sunxi-bt` rfkill.

## Display
The panel driver board `H6631_LED_V1.3` uses SM16169W column drivers and SM5368PS row scan. The TCON is used as a
waveform generator, so `/dev/fb0` is *not* an image. Drive the panel through `/usr/lib/libgovee_display.so`:
```c
int LightKal_lcd_display_init(void);
int LightKal_lcd_display_lumi_set(uint8_t pct);          // 0-100; the default is 0 (black), so set it first
int LightKal_lcd_display_data_set(int w, int h, const uint8_t *rgb, int len);   // call with (32, 52, buf, 4992)
int LightKal_lcd_display_reset_frame(void);
int LightKal_lcd_display_deinit(void);
```
- `buf` is a plain row-major 52×32 RGB image with its origin at the top-left.
- The library applies brightness scaling and a power limit: if the sum of all subpixels exceeds 1 272 960,
  every value is scaled down.
- `src/pixeltest.c` is a minimal example.

## Other I/O
- **Buttons** (`/dev/input/event0`, value 0 = pressed): power 101, color 102, mode 104, volume 108.
  Left to right on the device: power, mode, color, volume.
- **Speaker:** the codec is `hw:0,0` and the amplifier enable is GPIO 129.
  - The codec pops when it powers down at the end of a stream. `chime.sh` avoids this: it opens the stream with
    silence, switches the amplifier on, plays the sound, and switches the amplifier off before the stream closes.
  - Square-wave chiptunes buzz on this speaker; a low-pass filter at about 2.5 kHz helps.
- **Microphone:** `arecord -D hw:0,0` (used by music mode).

## Stock assets (`/usr/res`)
Animations come in several formats:
- plain GIFs (some with `.txt` names);
- a Govee PNG-container format;
- layered "graffiti_2d"/"diydial" scenes: a background, up to 100 sprites with motion, speed, opacity and blink,
  then foregrounds.

`tools/graffiti.py` renders the layered scenes, using the motion direction, stacking order and timing found on
hardware. See [FORMATS.md](FORMATS.md). The stock artwork is Govee's: convert it on your own device, don't
redistribute it.
