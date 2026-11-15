#!/bin/sh
# chime.sh <sound> [volume 0-100]   |   chime.sh stop
# Plays a sound through the speaker without amp pops: the ALSA stream is opened with silence
# first, the amplifier (GPIO 129) is switched on during that silence, the sound plays, and the
# amp is switched off again during trailing silence *before* the codec powers down.
# A new sound (or "stop") cuts off whatever is playing: amp off first, then the stream is killed.
# Only one chime runs at a time (newest wins); every other aplay is killed, so an orphaned
# stream can never keep the speaker busy.
# <sound> is a path, or a file name in /mnt/UDISK/sounds (pixel2mqtt uploads), /data/custom/sounds or
# /usr/res/sound[/tips] (extension optional).
# Decoded audio is cached as raw 48 kHz stereo S16LE in /mnt/UDISK/sound-cache (/data is only 11 MB).
AMP=/sys/class/gpio/gpio129/value
PIDF=/tmp/chime.pid
BPS=192000   # bytes per second of the raw format

stop_all() {
    echo 0 > $AMP                         # mute first so killing the stream doesn't pop
    P=$(cat $PIDF 2>/dev/null); rm -f $PIDF
    if [ -n "$P" ] && [ "$P" != "$$" ] && kill -0 "$P" 2>/dev/null; then usleep 300000; kill -TERM "$P" 2>/dev/null; fi
    if pidof aplay > /dev/null; then
        usleep 300000; killall aplay 2>/dev/null
        i=0; while pidof aplay > /dev/null && [ $i -lt 20 ]; do usleep 100000; i=$((i + 1)); done
        killall -9 aplay 2>/dev/null
        usleep 300000                         # let the driver release the device
    fi
}

S=$1; V=${2:-60}
[ -n "$S" ] || { echo "usage: $0 <sound> [volume 0-100] | stop"; exit 1; }
if [ "$S" = stop ]; then stop_all; exit 0; fi
for f in "$S" /mnt/UDISK/sounds/"$S" /mnt/UDISK/sounds/"$S".* /data/custom/sounds/"$S" /data/custom/sounds/"$S".* /usr/res/sound/tips/"$S" /usr/res/sound/tips/"$S".* /usr/res/sound/"$S" /usr/res/sound/"$S".*; do
    [ -f "$f" ] && { F=$f; break; }
done
[ -n "$F" ] || { echo "sound not found: $S"; exit 1; }

# claim the speaker: newest request wins; an older chime still decoding sees it lost and exits
echo $$ > /tmp/chime.want

D=/mnt/UDISK/sound-cache; mkdir -p $D
R=$D/$(basename "$F" | sed 's/\.[^.]*$//').raw
if [ ! -s "$R" ]; then
    gst-launch-1.0 -q filesrc location="$F" ! decodebin ! audioconvert ! audioresample \
        ! audio/x-raw,format=S16LE,rate=48000,channels=2 ! filesink location="$R.$$.tmp"
    [ -s "$R.$$.tmp" ] && mv "$R.$$.tmp" "$R"
    rm -f "$R.$$.tmp"
fi
[ -s "$R" ] || { echo "decode failed: $F"; exit 1; }
[ "$(cat /tmp/chime.want 2>/dev/null)" = "$$" ] || exit 0   # a newer sound was requested meanwhile

# serialise the hand-over (stop old stream -> open new one) between overlapping requests
LOCK=/tmp/chime.lock; n=0
until mkdir $LOCK 2>/dev/null; do
    n=$((n + 1)); [ $n -gt 30 ] && { rm -rf $LOCK; continue; }   # stale lock after ~3 s
    usleep 100000
done
[ "$(cat /tmp/chime.want 2>/dev/null)" = "$$" ] || { rmdir $LOCK; exit 0; }
stop_all
echo $$ > $PIDF
AP=
trap 'echo 0 > $AMP; [ -n "$AP" ] && kill $AP 2>/dev/null; [ "$(cat $PIDF 2>/dev/null)" = "$$" ] && rm -f $PIDF; rmdir $LOCK 2>/dev/null; exit 0' TERM INT

MS=$(( $(wc -c < "$R") / (BPS / 1000) ))   # sound length in ms
[ "$V" -gt 100 ] && V=100
amixer -q -D hw:audiocodec cset name='DAC Digital Volume' $((V * 63 / 100))
try=0
while :; do
    ( dd if=/dev/zero bs=$BPS count=1 2>/dev/null; cat "$R"; dd if=/dev/zero bs=$BPS count=2 2>/dev/null ) \
        | aplay -q -D hw:0,0 -f S16_LE -r 48000 -c 2 2>/dev/null &
    AP=$!
    usleep 300000
    kill -0 $AP 2>/dev/null && break           # stream opened
    try=$((try + 1)); [ $try -ge 5 ] && { echo "speaker busy"; rmdir $LOCK 2>/dev/null; exit 1; }
    killall aplay 2>/dev/null; usleep 300000   # device still held by a dying stream: retry
done
rmdir $LOCK 2>/dev/null                      # stream is open; let the next request in
# background sleeps + wait, so a TERM is handled immediately
usleep 200000 & wait $!
echo 1 > $AMP                                   # amp on at t=0.5 s, during the leading silence
usleep $(( (MS + 800) * 1000 )) & wait $!       # sound plays t=1 s .. 1 s+len; amp off 0.3 s after
echo 0 > $AMP                                   # amp off during the trailing silence
wait $AP
[ "$(cat $PIDF 2>/dev/null)" = "$$" ] && rm -f $PIDF
exit 0
