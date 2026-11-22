## What's new in v0.3.0
- **Layout screens:** info screens are now `.scr` files you design (text, values, icons, bars, sparklines, shapes,
  conditions, colour rules, blink), filled with Home Assistant values. Design them in
  [pixel2mqtt](https://github.com/JinjAdmin/pixel2mqtt) (v0.5.0+); the format is in `docs/SCREENS.md`, with examples
  in `examples/screens`.
- **Screens mode** replaces Info mode (HA Mode select, MQTT, playlists: `screens:40`). `info` is still accepted.
  *Info screen seconds* is now *Screen rotate seconds*.
- HA **Screen** select pins one screen; playlists can show one with `screen:<name>:<seconds>`.
- **Removed:** the hard-coded weather, room climate, stock and score screens and their `weather/climate/stock/score`
  topics. bled no longer feeds `climate/set`.

**Install:** download `pixeld-installer-<version>.zip` below, unzip it, wire up the serial console (see the README's
Installation section), then run:

```sh
python installer/pixeld_install.py
```

Needs Python 3.8+ on Windows, macOS or Linux. Nothing else to install. The zip contains the installer, the pixeld
and bled programs built for the H6631, and the firmware scripts. **No Govee software or artwork is included.** The
installer builds the custom firmware from your own device's stock firmware.

Then set up [pixel2mqtt](https://github.com/JinjAdmin/pixel2mqtt) (recommended) to bring the display into Home
Assistant and get the web editor, or choose direct MQTT in the installer for a standalone install.

Back to stock: `python installer/pixeld_install.py --rollback`.
