#!/bin/sh
# Custom H6631 init (replaces Govee's govee_init.sh; called last from rc.final).
# No Govee apps, no cloud, no OTA. Brings up Wi-Fi from /data/custom, SSH (key only), time sync,
# then hands over to /data/custom/start.sh so new programs can be installed without re-flashing.

/etc/init.d/usr_mount.sh            # /overlay (persistent) + /data /govee /matter binds + /etc tmpfs overlay

C=/data/custom
LOG=$C/boot.log
mkdir -p $C/ssh $C/dropbear
[ -f $LOG ] && [ $(wc -c < $LOG) -gt 65536 ] && mv $LOG $LOG.old
exec >> $LOG 2>&1
echo "=== custom init $(date) ==="

# Panel timing + power, exactly as Govee's init did
/usr/bin/lcd_display_refresh.sh
for g in 129 135; do echo $g > /sys/class/gpio/export; echo out > /sys/class/gpio/gpio$g/direction; done
echo 0 > /sys/class/gpio/gpio129/value
echo 1 > /sys/class/gpio/gpio135/value

# Hostname
[ -f $C/hostname ] && hostname "$(cat $C/hostname)" || hostname govee-pixel

# Wi-Fi
if [ -f $C/wpa_supplicant.conf ]; then
    mkdir -p /var/run/wpa_supplicant
    ifconfig wlan0 up
    wpa_supplicant -B -Dnl80211 -iwlan0 -c $C/wpa_supplicant.conf
    udhcpc -i wlan0 -b -R -x hostname:"$(hostname)"
else
    echo "no $C/wpa_supplicant.conf - Wi-Fi not started"
fi

# Time (override servers with $C/ntp_servers, one per line)
NTP="pool.ntp.org"
[ -f $C/ntp_servers ] && NTP="$(cat $C/ntp_servers)"
set -- ; for s in $NTP; do set -- "$@" -p "$s"; done
ntpd "$@"

# SSH: key-only root login. authorized_keys lives in $C/ssh (/root/.ssh links there).
for t in ed25519 rsa; do
    k=$C/dropbear/dropbear_${t}_host_key
    [ -s $k ] || dropbearkey -t $t -f $k
done
# OpenWrt's dropbear reads root's keys from /etc/dropbear/authorized_keys (/etc is a tmpfs overlay)
mkdir -p /etc/dropbear && chmod 700 /etc/dropbear
cp $C/ssh/authorized_keys /etc/dropbear/authorized_keys 2>/dev/null && chmod 600 /etc/dropbear/authorized_keys
dropbear -s -g -p 22 -r $C/dropbear/dropbear_ed25519_host_key -r $C/dropbear/dropbear_rsa_host_key

# User programs
if [ -x $C/start.sh ]; then
    echo "running $C/start.sh"
    $C/start.sh &
fi
echo "=== custom init done ==="
