# pixeld interfaces

## MQTT (direct mode)
These apply when `/data/custom/mqtt.conf` exists and no pixel2mqtt bridge is connected. The base topic is
`govee-pixel`, and pixeld publishes Home Assistant discovery under `homeassistant/`.

| Topic (`govee-pixel/<x>/set`) | Payload |
|---|---|
| `power`, `brightness` | `ON`/`OFF`, 1-100 |
| `mode` | clock, text, image, animation, effect, music, info, countdown, stopwatch, playlist |
| `text` | text to scroll (switches to text mode) |
| `image` | 4992 raw bytes (52×32 RGB) |
| `animation`, `effect`, `music_style`, `clock_style` | a name from the matching HA select |
| `notify` | text, or JSON `{"message","duration","color":"#rrggbb","sound","animation"}` |
| `chime` | `<sound> [volume]`, `doorbell`, `stop` |
| `sound`, `countdown_sound`, `volume` | doorbell sound, countdown-end sound, 0-100 |
| `countdown` / `countdown_min` | `m:ss`, `h:mm:ss`, seconds or `stop` / minutes |
| `stopwatch` | start, stop, toggle, reset |
| `playlist` | `kind[:arg]:secs,…`, e.g. `clock:30,info:40,animation:Moon:20` |
| `weather`, `climate`, `stock`, `score` | JSON for the info screens (see `ha/govee_pixel_package.yaml`) |
| `clock12`, `fahrenheit`, `info_rotate` | `ON`/`OFF`, seconds |

Published: `<x>/state` for each setting, `status` (online/offline, LWT), `button/<power|mode|color|volume>`
(short/long), `countdown/event`, and `sound_level/state` while music mode is on screen.

## LAN API (HTTP, port 80)
Port 80 also serves the Wi-Fi setup page. Requests are non-blocking (at most 8 connections) and uploads stream
straight to disk.

| Endpoint | Auth | |
|---|---|---|
| `GET /api/info` | – | id (wlan0 MAC), name, model, size, paired/pairing/mqtt_direct |
| `POST /api/pair` | – | shows a 6-digit code on the panel for 120 s |
| `POST /api/pair?code=NNNNNN` | – | returns `{"token": …}`; 5 wrong codes cancel pairing |
| `DELETE /api/pair` | token | forget the calling token |
| `GET /api/storage` | token | free/total bytes of `/data` and `/mnt/UDISK` |
| `GET /api/discovery` | token | every message pixeld would publish on connect (HA discovery + state) |
| `GET /api/events?since=N&wait=25&mqtt=1` | token | long-poll of everything pixeld publishes; `reset:true` = re-read discovery |
| `POST /api/cmd/<x>` | token | body = payload; same as MQTT `govee-pixel/<x>/set` |
| `GET /api/frame` | token | the last rendered frame (4992 bytes RGB) |
| `GET /api/files/<anims\|icons\|sounds>` | token | JSON list (sounds include the read-only built-ins) |
| `GET\|PUT\|DELETE /api/files/<kind>/<file>` | token | `.anim`/`.clock`, `.pxi`, `.mp3/.wav/.ogg` |
| `GET /api/res?path=/usr/res/…` | token | read-only stock resources (file, or JSON listing for a directory) |

The token goes in `Authorization: Bearer <token>`. Tokens are stored in `/data/custom/api.tokens` (up to 8).

**MQTT hand-over:** while a client long-polls `/api/events` with `mqtt=1`, meaning it is connected to the broker and
bridging, pixeld's own MQTT client stays off. Five minutes after the last such poll, pixeld goes back to direct MQTT.
After an upload or delete, pixeld rescans its files 2 s later and republishes discovery, which updates HA's dropdowns.

## UDP
- **7777:** raw 52×32 RGB frames (4992 bytes, row-major from the top-left). The panel shows them and returns to
  its mode 3 s after the last one. `tools/pixel_udp.py` is an example sender.
- **7778:** discovery. A datagram starting with `pixel2mqtt?` gets the `/api/info` JSON as the reply.
