# File formats

All integers are little-endian.

## `.anim`: animation (`/mnt/UDISK/anims`)
```
"PXA1"  u16 frame_count
frame_count × { u16 delay_ms, 52*32*3 bytes RGB (row-major from the top-left) }
```
pixeld loads at most 600 frames and clamps delays to at least 20 ms. Writers: `tools/anim_convert.py` and pixel2mqtt.

An optional sidecar `<name>.clock` (`cx cy Label`) makes the animation a **clock scene**: it appears in the Clock
style select, with HH:MM drawn centred on (cx, cy).

## `.pxi`: icon (`/mnt/UDISK/icons`)
```
"PXI1"  u8 w  u8 h  u16 frame_count
frame_count × { u16 delay_ms, w*h*4 bytes RGBA }
```
Pixels with alpha < 128 are transparent. pixeld loads at most 64 frames. Writer: `tools/icon_pack.py`.
pixeld uses these names:
- `wx_*` (15×18) and `wxs_*` (16×12) for weather;
- `env_temp`/`env_hum`;
- `fin_rise`/`fin_fall`/`fin_flat`;
- `nfl_<team>` (15×5) and `nba_<team>` (20×9);
- `hourglass`, `stopwatch`, and `clk_face`/`clk_left`/`clk_right` for the analog clock.

## `.scr`: layout screen (`/mnt/UDISK/screens`)
A text file starting with `PXS1`: see [SCREENS.md](SCREENS.md).

## Sounds
`.mp3`/`.wav`/`.ogg` files go in `/mnt/UDISK/sounds` or `/data/custom/sounds`; the built-ins are in
`/usr/res/sound[/tips]`.
- The label shown in HA is the file name without a leading `NN-` and with `_` turned into spaces.
- Each sound is decoded once to raw 48 kHz S16LE in `/mnt/UDISK/sound-cache`.

## Govee scene files (stock, read-only)
- **diydial:** `[n_layers]`, then per layer `[type][u32 size][data]`. Type 1 = GIF, 2 = graffiti_2d, 3 = 20-byte
  clock config (not fully decoded).
- **graffiti_2d:** `[bg RGB][b3][type]`, then three groups (backgrounds ≤ 5, layers ≤ 100, foregrounds ≤ 5).
  Each group is `[count]` plus its items.
- **Item:** `[u16 reclen][0][u16 png_len][PNG][K][A][B][C][u16 D][E][pad]`
  - K = motion of the *source offset*, so the image moves the opposite way:
    - 1 down, 2 up, 3 right, 4 left;
    - 5 down-right, 6 down-left, 7 up-right, 8 up-left;
    - the image tiles, wrapping modulo its size.
  - A = speed 0-100. 0 means static, even when K is set.
  - B = opacity %, C = index, D = duration in ms, E = 1 blinks with period D.
  - Within each group, item 1 is drawn on top. Black pixels are transparent.
  - If `type` ≠ 0, the middle group is a timed frame sequence.

`tools/graffiti.py` implements all of this, including seamless loop lengths.
