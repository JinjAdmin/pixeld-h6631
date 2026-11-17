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
