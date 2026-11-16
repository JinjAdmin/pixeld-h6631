#!/bin/sh
# Runs ON the display (stock firmware), started by pixeld_install.py:
#   sh install.sh <target slot A|B> <rootfs md5> <rootfs size>
# Writes the custom root filesystem into the unused slot, copies the running kernel into it, and installs
# /data/custom. It never writes the running (stock) slot, and it does not switch slots: the PC does that after
# INSTALL-OK.
D=/mnt/UDISK/pixeld-install
C=/data/custom
T="$1"; MD5="$2"; SIZE="$3"
fail() { echo "INSTALL-FAIL: $*"; exit 1; }
step() { echo "== $*"; }

RUN=$(fw_printenv -n boot_partition 2>/dev/null | sed 's/^boot//')
case "$T" in A|B) ;; *) fail "bad target slot '$T'";; esac
[ -n "$RUN" ] || fail "can't read boot_partition"
[ "$T" != "$RUN" ] || fail "refusing to write the running slot ($RUN)"
[ -f /etc/custom_release ] && [ "$FORCE" != 1 ] && fail "this slot already runs custom firmware"
[ -c /dev/by-name/rootfs$T ] || fail "no rootfs$T volume"
[ "$(md5sum < $D/rootfs.sqfs | cut -d' ' -f1)" = "$MD5" ] || fail "image checksum mismatch (download damaged)"

step "writing the custom firmware to slot $T"
ubiupdatevol /dev/by-name/rootfs$T $D/rootfs.sqfs || fail "ubiupdatevol rootfs$T"
[ "$(head -c $SIZE /dev/by-name/rootfs$T | md5sum | cut -d' ' -f1)" = "$MD5" ] || fail "rootfs$T verify failed"
echo "rootfs$T verified"

step "copying the kernel (boot$RUN -> boot$T)"
dd if=/dev/by-name/boot$RUN of=$D/boot.img 2>/dev/null || fail "read boot$RUN"
BMD5=$(md5sum < $D/boot.img | cut -d' ' -f1)
ubiupdatevol /dev/by-name/boot$T $D/boot.img || fail "ubiupdatevol boot$T"
[ "$(head -c $(wc -c < $D/boot.img) /dev/by-name/boot$T | md5sum | cut -d' ' -f1)" = "$BMD5" ] || fail "boot$T verify failed"
echo "boot$T verified"

step "installing /data/custom"
mkdir -p $C/apps/autostart $C/ssh $C/dropbear || fail "mkdir $C"
put() {  # put <src> <dst> <mode>: replace via a temp file (works even if the old file is in use)
    cp "$1" "$2.new" && chmod "$3" "$2.new" && mv "$2.new" "$2" || fail "copy $2"
}
put $D/custom/start.sh $C/start.sh 755
put $D/custom/apps/pixeld $C/apps/pixeld 755
put $D/custom/apps/bled $C/apps/bled 755
put $D/custom/apps/chime.sh $C/apps/chime.sh 755
for f in $D/custom/apps/autostart/*; do put "$f" $C/apps/autostart/$(basename "$f") 755; done
[ -f $C/ble.conf ] || put $D/custom/ble.conf $C/ble.conf 644
if [ -s $D/config/tz ] && [ ! -s $C/tz ]; then put $D/config/tz $C/tz 644; fi
echo "time zone: $(cat $C/tz 2>/dev/null || echo 'UTC (default)')"
if [ -s $D/config/hostname ] && [ ! -s $C/hostname ]; then put $D/config/hostname $C/hostname 644; fi
if [ -s $D/config/mqtt.conf ]; then
    put $D/config/mqtt.conf $C/mqtt.conf 600
    rm -f $D/config/mqtt.conf                     # holds the broker password: don't leave a copy behind
    echo "MQTT: direct to $(sed -n 's/^host=//p' $C/mqtt.conf)"
elif [ -f $D/config/mqtt_remove ]; then
    rm -f $C/mqtt.conf
    echo "MQTT: through pixel2mqtt (no MQTT login on the display)"
else
    echo "MQTT: $([ -s $C/mqtt.conf ] && echo 'keeping the existing direct login' || echo 'through pixel2mqtt')"
fi
if [ -s $D/config/authorized_keys ]; then
    touch $C/ssh/authorized_keys
    while read -r k; do
        [ -n "$k" ] && ! grep -qxF "$k" $C/ssh/authorized_keys && echo "$k" >> $C/ssh/authorized_keys
    done < $D/config/authorized_keys
    chmod 700 $C/ssh; chmod 600 $C/ssh/authorized_keys
fi
echo "SSH keys: $(grep -c . $C/ssh/authorized_keys 2>/dev/null)"

# Wi-Fi: keep an existing config; otherwise reuse the network the stock firmware is connected to.
if [ -s $C/wpa_supplicant.conf ]; then
    echo "Wi-Fi: keeping the existing $C/wpa_supplicant.conf"
else
    P=$(pidof wpa_supplicant | cut -d' ' -f1); W=""
    [ -n "$P" ] && W=$(tr '\0' '\n' < /proc/$P/cmdline | awk 'f {print; exit} /^-c$/ {f=1; next} /^-c/ {print substr($0, 3); exit}')
    if [ -n "$W" ] && [ -s "$W" ] && grep -q 'ssid=' "$W"; then
        cp "$W" $C/wpa_supplicant.conf && chmod 600 $C/wpa_supplicant.conf
        echo "Wi-Fi: copied the stock firmware's network settings"
    else
        echo "Wi-Fi: no stock settings found; the display will open its setup hotspot (password shown on the panel)"
    fi
fi
echo "$RUN" > $C/stock_slot
sync
echo "INSTALL-OK"
