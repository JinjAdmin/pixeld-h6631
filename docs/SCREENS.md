# Info screens (`.scr`): spec, draft 1

Status: **renderer implemented in pixeld (`src/screens.c`, not yet deployed); pixel2mqtt editor in progress.**
This spec covers the firmware renderer (pixeld) and the screen editor in pixel2mqtt.

## Who does what

- **The display (pixeld)** draws screens from layout files. It knows nothing about printers, stocks or weather.
  It knows text, values, icons, bars, sparklines and shapes, plus rules for when to show them.
- **pixel2mqtt** edits the layouts, links each value name to a Home Assistant entity, and sends the values
  to the display as they change.
- **Home Assistant** owns every integration (printer, weather, markets, ...). Nothing integration-specific lives in
  the firmware, and the display never talks to the internet.

A layout file never names an entity. It refers to values by name (`printer.progress`), so the same screen works
in any house; only the bindings in pixel2mqtt differ.

## Files

`/mnt/UDISK/screens/<name>.scr`, UTF-8 text, at most 16 KB. Upload, list and delete them through
`/api/files/screens` (same rules as anims: names may not contain `/ \ "` or control characters).
pixeld rereads a file only when it changes.

```
PXS1                         <- magic, first line
# comment                    <- a '#' that starts the line or follows a space, and is followed by a space or the end
<directive or element> ...   <- one per line
```
Colours like `#ff0000` and `bg #000000` are therefore never comments.
Tokens are separated by spaces. `key=value` sets an option, and an option value with spaces must be quoted
(`if="a == 1 && b > 2"`). A token that starts with `"` is the text template (or an icon name), even when it
contains `=`. Inside quotes, `\"` and `\\` escape. Lines are at most 512 bytes long.
Errors don't stop the screen: a bad line is skipped and reported (see [Errors](#errors)).

### Example: 3D printer
```
PXS1
name    3D printer
needs   printer.status printer.progress
show    printer.status in running,pause,prepare
linger  printer.status in finish,failed 30m
stale   1h

text   0 0  w=52 font=tiny align=center overflow=scroll color=#9696af "{printer.name|replace:_= |upper|or:3D PRINT}"
icon   37 7 printer_build frame=printer.progress range=0,100
text   1 7  font=big color=#ffffff "{printer.progress|int}"  if=printer.status==running
text   1 7  font=big color=#ffcd3c "{printer.progress|int}"  if=printer.status==pause
text   1 11 font=small color=#3cdc78 "DONE!"                 if=printer.status==finish
bar    0 23 52 3 value=printer.progress max=100 fill=#00a0ff..#28e678 bg=#1e1e2a pulse=1
text   0 27 w=30 font=tiny color=#ffa046 "{printer.remaining|dur:min}"
text   30 27 w=22 font=tiny align=right color=#8c8ca0 "{printer.layer}/{printer.layers}"
```

## Directives (screen settings)

| Directive | Meaning |
|---|---|
| `name <text>` | Display name (HA Screen select, playlist). Defaults to the file name. |
| `needs <value> ...` | Hide the screen until all of these values exist and are fresh. |
| `show <condition>` | Show only while the condition is true (default: always). |
| `linger <condition> <duration>` | Keep showing for `<duration>` after the condition first became true, even though `show` is false. The clock only starts on a transition pixeld actually saw, so a restart doesn't bring back an old "done". |
| `stale <duration>` | A `needs` value older than this hides the screen (default `6h`). |
| `time <seconds>` | Time on screen in the info rotation (default: the Info rotate setting). |
| `order <n>` | Position in the rotation (default 100, then by name). |
| `bg <#rrggbb>` | Background colour (default black). |

Durations are written `90s`, `30m`, `2h` or `1d`.

## Elements

They are drawn in file order, so later lines are on top. Every element accepts `if=<condition>`,
`color=<colour or rules>` and `blink=<ms>` (shown for `ms`, hidden for `ms`, 50..10000; phase = `floor(fmod(t * 1000/ms, 2))`, hidden when 1).
Coordinates are pixels from the top-left (0..51, 0..31). They may be negative or run off the panel, in which case the element is clipped.

### `text x y [options] "template"`
A text box at (x, y). Its default size is the width of the text by one line.

| Option | Values |
|---|---|
| `font` | `tiny` 3×5 (capitals, digits and ``- + . , : % / * ! ? ( ) = _ $ # < > ' "`` and `°`), `small` 5×7 (full ASCII), `big` 10×14, `huge` 15×21 (`big` and `huge` are `small` scaled ×2 and ×3) |
| `w`, `h` | Box size. Needed for `align`, `valign` and `overflow` to mean anything. |
| `align` | `left` (default), `center`, `right` within `w` |
| `valign` | `top` (default), `middle`, `bottom` within `h` |
| `overflow` | `clip` (default), `scroll` (marquee when wider than `w`; `speed=` px/s, default 15), `shrink` (step down to the next smaller font until it fits, then clip) |
| `spacing` | Extra pixels between characters (default 0, may be negative) |

Lowercase letters in `tiny` are drawn as capitals, and other unsupported characters as blanks (the load check
reports them when they appear in literal text). Text is UTF-8; `°` is drawn in every font, and other non-ASCII
characters as `?`.

### `icon x y <name> [options]`
Draws a `.pxi` from the icon library, and plays it if it's animated.
- The icon name may contain `{value}` templates, e.g. `icon 1 1 "@coin:{ticker.symbol}"`.
- `map=<key>:<icon>,...,*:<icon>` picks the icon by value (split at the first `:`, so `clear-night:@moon` and
  `btc:@coin:BTC` work). Use it with `value=`; without `map=` the value itself is the icon name. E.g.
  `icon 1 1 value=weather.condition map=sunny:wx_sunny,rainy:wx_rain,pouring:wx_rain,*:wx_cloudy`.
- `frame=<value> range=<min>,<max>` picks the frame from a value instead of playing the animation (a progress
  icon).
- Drawn built-ins (no artwork needed) start with `@`: `@moon` (a fixed 12×12 crescent, not the real moon phase), `@degree`, `@up`, `@down` (5×3 triangles),
  `@coin:<SYMBOL>` (9×9 coin badge).

`bar`, `spark` and `rect` take their size as two more numbers (`bar 0 23 52 3`) or as `w=`/`h=` options.

### `bar x y w h value=<value> [options]`
A progress or level bar.
- `min=` and `max=` set the range (default 0, 100).
- `fill=#a` gives a solid fill, and `fill=#a..#b` a gradient along the bar.
- `bg=` colours the empty part (default none).
- `dir=right|left|up|down` (default right).
- `pulse=1` makes the leading edge blink.

### `spark x y w h series=<value> [options]`
A sparkline from a list value. The line is auto-scaled to the list's min and max.
- `area=1` shades under the line.
- `dot=1` makes the newest point blink.
- `color` rules can use the derived `<series>.trend`, which is the last value minus the first.

### `rect x y w h [fill=1]`, `line x1 y1 x2 y2`, `pixel x y`
Plain shapes in `color`.

## Templates
Text between `{` and `}` is replaced by a value, run through formatters left to right: `{name|fmt|fmt:arg}`.
Write `{{` and `}}` for literal braces. A missing value prints as empty (use `or:`).

| Formatter | Example |
|---|---|
| `int`, `dec:<n>` | `41.7` → `42`, `dec:1` → `41.7` |
| `thousands` | `61764.2` → `61,764` |
| `pct` | `-1.24` → `-1.2%` (printf `%.1f`, sign kept; use `pct\|sign` for `+1.2%`) |
| `sign` | adds `+` to positive numbers |
| `dur:min`, `dur:s`, `dur:h` | the input unit. Under a minute `42S`, under an hour `42M`, under a day `1H05M`, else `2D3H` |
| `temp` | converts to the Fahrenheit switch's unit using the value's unit (see Values), rounded |
| `time` | `"16:41"` or a timestamp → `16:41` or `4:41`, following the 12-hour switch |
| `ago` | timestamp → `JUST NOW`, `5M AGO`, `2H AGO` |
| `upper`, `lower`, `trunc:<n>` | text |
| `replace:<from>=<to>` | `replace:_= ` turns `Rack_mount` into `Rack mount` (spaces in the argument are kept) |
| `or:<text>` | fallback when missing or empty |
| `map:<k>=<text>,...,*=<text>` | `running` → `PRINTING` |

Values the display always has: `time`, `date` (e.g. `THU NOV 19`), `weekday`, `month`, `day`, `hour`, `minute`, `year`,
`second`.

## Conditions and colour rules
- **Conditions:** `<value> <op> <literal>` with `== != < > <= >=`; `<value> in a,b,c`; `has(<value>)`;
  `fresh(<value>)`; a bare `<value>` (true unless missing, empty, `0`, `false` or `off`); `!` in front of any term
  negates it. Join terms with `&&` or `||`; `&&` binds tighter, and there are no parentheses. Numbers compare as
  numbers, everything else as case-insensitive text. A comparison with a missing value is false (`!=` is true).
  Literals may be quoted. A condition is at most 400 characters.
- **Colours:** `#rrggbb`. **Rules:** `color="<condition>:#c;<condition>:#c;#default"`, separated by `;`. The first
  true condition wins. Example: `color="stock.change>0:#28dc50;stock.change<0:#ff3c3c;#b4b4b4"`.

## Values

Values arrive on one topic, `govee-pixel/values/set` (`<topic>/values/set` under pixel2mqtt), as a flat JSON object:
```json
{"printer.status": "running", "printer.progress": 42, "weather.temp": [48.2, "°F"],
 "btc.spark": [61210, 61340, 61764], "printer.name": null}
```
- Names are `[a-z0-9_.-]`, up to 31 characters, and shared by all screens.
- Accepted values: a number, a string (up to 63 bytes), `true`/`false` (stored as those strings), a list of up to
  64 numbers (for `spark`; longer lists keep the newest 64, `null` points are skipped), `[number, "unit"]` (the unit
  is used by `temp`), or `null` (deletes the value). Nested objects are ignored (logged).
- pixeld stores each value with its arrival time (for `stale` and `fresh()`), in RAM only. "Fresh" means *recently
  sent*, so pixel2mqtt resends every bound value every 10 minutes even when it hasn't changed, and everything when
  the display reconnects or reboots. A screen goes stale only when its source really stops.
- One message is at most 16 KB (the MQTT receive buffer is 16 KB). Send larger updates in several messages.

## Rotation and Home Assistant
- **Info mode** cycles through every screen whose `needs`, `show` and `linger` rules let it show, in `order`.
  The existing built-in info screens (weather, room climate, scores) stay in the rotation until example screens
  replace them.
- **Playlists:** `screen:<name>:<seconds>`.
- **HA select "Screen":** `Auto` (rotation) or one screen pinned. A pinned screen shows even when its rules say no;
  missing values print blank.

## Limits
24 screens, 32 elements per screen, 128 values (the oldest is replaced when full), 64 points per series,
16 KB per file, 512 bytes per line, 400 characters per condition or colour rule, 160 bytes of rendered text.

## Errors
pixeld skips a bad line and keeps the rest of the screen. When a file loads, every condition, colour and template
is also checked once. `GET /api/screens` lists each screen with
`{"name","file","order","visible","missing","errors":[{"line":7,"msg":"unknown font 'medium'"}]}` (up to 8 errors;
line 0 = a directive). `missing` is one string: the names of the `needs` values that are missing or stale,
separated by spaces (empty when none). The pixel2mqtt editor shows these next to the element.

## Previews and reference frames
`tools/screen_preview.c` builds pixeld's renderer for the PC and renders a `.scr` with a values file:
`screen_preview out.raw screen.scr <frames> <ms> [--time=<epoch>] [--check] values.json [fahrenheit=OFF ...]`
(build line at the top of the file). `tools/raw_png.py out.raw out.png` turns the frames into a PNG strip.
`tests/screens/` holds reference screens with values and the exact frame the device renders (`<name>.raw`, made
by `make_golden.sh` at a fixed time and settings). pixel2mqtt's browser renderer must match them byte for byte.

---

# Screen editor in pixel2mqtt

A third project type next to **Animation** and **Icon**: **Screen**. The editor saves a project (layout, bindings and
sample values). **Send to display** compiles it to a `.scr` file, uploads it and turns on its bindings.

## Canvas
- The 52×32 panel is zoomed and pixel-exact. The renderer is the same as the device's (a WebAssembly or JS port of the
  `.scr` renderer), so what you see is what the panel shows.
- **Click** to select an element and **drag** to move it. Text, bar, sparkline and shape boxes have **resize handles**.
- Arrow keys nudge by 1 px (Shift: 5 px). Guides snap to the panel centre lines and to other elements' edges.
- There are warnings for text that overflows its box, elements off the panel, and characters the tiny font can't draw.
- The layer list shows draw order: drag to reorder, eye icon to hide while editing, lock icon.

## Adding elements
A palette of **Text**, **Value**, **Icon**, **Icon by value**, **Progress bar**, **Sparkline**, **Rectangle** and
**Line**. Click or drag one onto the canvas. **Value** is a text element that opens the entity picker straight away.

## Inspector (the selected element)
- **Position and size**, as numbers.
- **Font:** a dropdown with each font rendered in pixels: Tiny 3×5, Small 5×7, Big 10×14, Huge 15×21. Font size *is* the
  choice of font, because the panel has no in-between sizes.
- **Alignment:** left/center/right and top/middle/bottom buttons, which act within the box.
- **Overflow:** clip, scroll (with speed) or shrink to fit.
- **Text:** a template field with an **Insert value** button and formatter chips (Integer, Decimals, Thousands, %,
  Duration, Temperature, Time, Ago, Upper, Fallback). It shows the formatted result live.
- **Colour:** a picker with the editor's palette, plus **Add colour rule** (value, operator, literal → colour).
- **Show when:** a condition builder (value, operator, literal; AND/OR). Empty means always.

## Values panel (bindings)
- **Insert value** opens the entity picker (below). Picking `sensor.p1s_..._print_progress` creates a binding with a
  suggested name (`printer.progress`), which you can rename.
- A binding can be the entity state or an attribute. It can also be a **history** (last N hours, M points, from the
  HA recorder), which feeds a sparkline.
- Each binding shows its live value and unit, and the screens that use it.
- **Preview values:** switch between **Live** (from HA) and **Sample** (editable). Sample lets you try states such as
  `printer.status = finish` without waiting for a print.
- **Live on display** streams the canvas to the panel, as the animation editor does.

## Entity picker
A big house has thousands of entities, so the picker is never a single dropdown. It has two ways in, both in the
same popover:
- **Search (the default; the cursor starts in the box):**
  - Typing matches the friendly name, entity id, device, area and integration, with words in any order. For
    example, `p1s remain` finds *P1S Remaining time*, and `kitchen temp` finds the kitchen sensor's temperature.
  - Results are grouped by device and show the live state and unit.
  - Filter chips narrow by kind: Number, Text/state, On/off, Weather, Has history. The chips are set from the
    element being bound, so a progress bar offers numbers first.
- **Browse (Finder-style columns):** **Area → Device → Entity → Attribute.** A column for entities with no device,
  and a switch to **Integration → Device → …**, cover the rest. On a narrow screen the columns become a drill-down
  list with a back button.
- Above both, **Recent** and **Used on this display** give one-click access to entities you have already bound.
- The data comes from Home Assistant's area, device and entity registries plus live states (one websocket
  connection). It's cached and kept current while the editor is open.
- Picking an entity offers **State** or one of its **attributes**, and, for numeric entities, **History** (for
  sparklines).

## Screen settings
Name, needs, show, linger, stale, time on screen, order and background: the directives above, as form fields.

## How values reach the display
- After **Send to display**, pixel2mqtt subscribes to the bound entities and publishes changes to
  `<topic>/values/set`. Updates are batched and throttled (at most about one message per second per display),
  everything is resent every 10 minutes (freshness), and everything is resent when the display reconnects.
- **HA access:**
  - As an HA OS add-on, it's automatic (Supervisor API).
  - In Docker, set `HA_URL` and `HA_TOKEN_FILE` (a long-lived token).
  - Without HA access, bindings are just names: the Values panel shows the names to publish and an example
    automation to copy.
- Deleting a screen from the display turns off the bindings that only it used.

## Examples
**New screen → From example** offers Today, Forecast, 3D printer and Ticker. They are plain projects you edit and
bind like any other. They use only drawn built-ins (`@moon`, `@coin:BTC`, ...) and the display's own icon library,
so no artwork ships with pixel2mqtt.

## Open questions
1. Should screens also be usable as **notification overlays** (a screen shown briefly over the current mode when a
   value changes, such as a volume bar or a Wi-Fi icon)? That would cover the pop-up idea with the same files.
2. Should a `huge` font (3×) exist, or is `big` enough on 32 rows?
3. Should the legacy weather, climate and scores screens be removed once the examples cover them?
