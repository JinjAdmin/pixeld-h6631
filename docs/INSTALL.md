# Installing pixeld on an H6631

Read the whole guide first. You need:
- an **ESP32 dev board** used as a USB-serial adapter (most ESP32 boards work). This guide uses an ESP32, but any
  3.3 V serial adapter that does **1 500 000 baud** works too: FT232R, CP2102N, CH340 or CH343 adapters (not the
  original CP2102), or a Raspberry Pi's GPIO UART (`/dev/ttyAMA0`);
- a Linux machine or WSL with `squashfs-tools`;
- Zig 0.13;
- about an hour.

**Risk:** you rewrite the device's flash. Back up everything first (step 3). The stock firmware stays in the other
slot, and U-Boot can always be reached over serial once `bootdelay` is set (step 4).

## 1. Serial console
- Open the case (power unplugged):
  - Remove the four T6H Torx screws and take off the bezel.
  - Carefully pry the case in half. Two ribbon cables and a small two-wire harness join the halves; disconnect
    them before pulling the halves apart.
  - Leave them disconnected for the whole procedure: the controller board boots and flashes without the LED panel.
    Reconnect the cables after step 8, once SSH works.
- The controller board (`H6631_MAIN_V1.4`) has four pads, `GND TX RX VCC5V`, next to the 12 V input.
- **ESP32 wiring:** connect the board's **EN to GND** so the ESP32 stays in reset and its USB-serial chip acts as a
  plain adapter. Then display **GND→GND, TX→TX0, RX→RX0** (not crossed: these are the USB chip's lines).
- **Any other adapter:** GND→GND, TX→RX, RX→TX (crossed).
- **Don't** connect `VCC5V`: the device powers itself, and the T113 pins are 3.3 V.
- BOOT0 prints at 115200 baud. Everything after it (U-Boot, kernel, shell) runs at **1500000 8N1**.
- The console ends in a **root shell with no login** (`root@Govee:/#`). `scripts/uart_cmd.ps1` sends one command
  and logs the reply. `scripts/uart_capture.ps1` just records.
- Long command lines (over about 400 characters) get mangled. Write longer scripts line by line with `echo '…' >> /tmp/x.sh`.

## 2. Look around
```sh
fw_printenv boot_partition root_partition systemAB_next bootdelay
for v in /sys/class/ubi/ubi0_*; do echo "$(basename $v) $(cat $v/name)"; done
```
The flash is SPI NAND with A/B slots: `bootA`/`rootfsA` and `bootB`/`rootfsB`.
- Note which slot is running (`boot_partition`). This guide assumes the stock firmware runs from **B** and the
  custom firmware goes into **A**. If yours runs from A, swap the letters everywhere below.
- Persistent storage is `rootfs_data` (11 MB, mounted at `/data`) and `UDISK` (about 290 MB, mounted at `/mnt/UDISK`).

## 3. Back up everything
Stream every partition to your PC. On the PC, run `scripts/recv_files.ps1 -OutDir backup -Names …` (one TCP
connection per file). On the device, send each file with busybox `nc`:
```sh
for d in /dev/mtd0 /dev/mtd1 /dev/mtd2 /dev/ubi0_0 /dev/ubi0_1 /dev/ubi0_2 /dev/ubi0_3 /dev/ubi0_4 /dev/ubi0_5 \
         /dev/ubi0_6 /dev/ubi0_7 /dev/ubi0_8 /dev/ubi0_9; do nc <pc-ip> 9123 < $d; sleep 1; done
fw_printenv > /tmp/env.txt; nc <pc-ip> 9123 < /tmp/env.txt
md5sum /dev/mtd[012] /dev/ubi0_[0-9]        # compare with your copies
```
The backup contains your Wi-Fi credentials and device keys. **Keep it private.**

## 4. Make U-Boot reachable
Stock `bootdelay` is 0, so U-Boot ignores keys. Set it to 3 so you can always stop at the `=>` prompt (for example
with `scripts/uboot_catch.ps1`) and switch slots back:
```sh
fw_setenv bootdelay 3
```

## 5. Build the image (PC)
```sh
bash firmware/build_rootfs.sh backup/ubi0_7          # your stock rootfsB dump -> firmware/out/rootfs-custom.squashfs
ZIG=~/zig/zig sh build.sh                             # build/pixeld, build/bled, build/pixeltest
```
The script unpacks your own stock rootfs and applies `firmware/overlay`:
- the Govee apps are replaced by a small init that starts Wi-Fi, SSH (key only), ntpd and `/data/custom/start.sh`;
- telnetd, adbd and swupdate autorun are disabled.

## 6. Flash slot A
Copy `rootfs-custom.squashfs` to `/mnt/UDISK/stage/` on the device, e.g. run `python -m http.server` on the PC and
`wget` it on the device. Check its md5, then:
```sh
ubiupdatevol /dev/by-name/rootfsA /mnt/UDISK/stage/rootfs-custom.squashfs
dd if=/dev/by-name/bootB of=/tmp/boot.img && ubiupdatevol /dev/by-name/bootA /tmp/boot.img   # same kernel in A
head -c $(wc -c < /mnt/UDISK/stage/rootfs-custom.squashfs) /dev/by-name/rootfsA | md5sum      # must match
```

## 7. Prepare `/data/custom`
These files survive reboots and image updates:
```
/data/custom/start.sh            firmware/data-custom/start.sh (chmod +x)
/data/custom/apps/               pixeld, bled (build/), chime.sh, autostart/* (chmod +x)
/data/custom/ble.conf            firmware/data-custom/ble.conf (optional)
/data/custom/wpa_supplicant.conf from scripts/make_wifi_conf.ps1 (or write it by hand)
/data/custom/ssh/authorized_keys your SSH public key: login is key-only
/data/custom/mqtt.conf           optional: host= port= user= pass= (scripts/make_mqtt_conf.ps1); not needed with pixel2mqtt
/data/custom/tz                  optional: POSIX TZ string, e.g. EST5EDT,M3.2.0,M11.1.0 (default UTC)
/data/custom/hostname            optional (default govee-pixel)
```
Keep large files off `/data` (11 MB). Animations, icons, uploaded sounds and the sound cache live in `/mnt/UDISK`.

## 8. Switch to slot A
```sh
fw_setenv boot_partition bootA; fw_setenv root_partition rootfsA; fw_setenv systemAB_next A; reboot
```
After about 30 s you can log in with `ssh root@<ip>` (busybox has no sftp; use `scp -O`).
- `/data/custom/boot.log` has the init log, and `pixeld.log` the daemon's.
- If Wi-Fi can't connect, the display opens the `govee-pixel-setup` hotspot and shows its password on the panel.
  With the panel still disconnected, check `/data/custom/wpa_supplicant.conf` over the serial console instead.
- Once SSH works: unplug the power, remove the serial wires, reconnect the two ribbon cables and the two-wire
  harness, and close the case.

## Rollback to stock
Over SSH or serial:
```sh
fw_setenv boot_partition bootB; fw_setenv root_partition rootfsB; fw_setenv systemAB_next B; reboot
```
If slot A doesn't boot, stop at the U-Boot `=>` prompt and run
`setenv boot_partition bootB; setenv root_partition rootfsB; setenv systemAB_next B; saveenv; boot`.

## Then
Install [pixel2mqtt](https://github.com/SamWiIIiams/pixel2mqtt). Adopt the display (a code appears on the panel),
then click **Import built-in animations & icons** to convert the stock artwork stored on your device.
