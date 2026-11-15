#!/bin/sh
# /data/custom/start.sh - run at the end of the custom init (in the background).
# Persistent: edit over SSH, no re-flash needed. Put long-running programs at the bottom.
C=/data/custom

# Slot-A image custom-20261115-054030 predates this fix: dropbear wants root keys in /etc/dropbear.
if [ ! -s /etc/dropbear/authorized_keys ]; then
    mkdir -p /etc/dropbear && chmod 700 /etc/dropbear
    cp $C/ssh/authorized_keys /etc/dropbear/authorized_keys && chmod 600 /etc/dropbear/authorized_keys
fi

# Keep logs small: /data is only 11 MB (logs rotate once per boot)
for L in $C/apps.log $C/pixeld.log; do
    [ -f $L ] && [ $(wc -c < $L) -gt 131072 ] && mv $L $L.old
done

# Apps: each executable in $C/apps/autostart is started once.
mkdir -p $C/apps/autostart
for a in $C/apps/autostart/*; do
    [ -x "$a" ] && { echo "starting $a"; "$a" >> $C/apps.log 2>&1 & }
done
