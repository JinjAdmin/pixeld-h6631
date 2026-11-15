#!/bin/bash
# Build the custom H6631 rootfs (squashfs) from the stock slot-B image + firmware/overlay.
# Run on Linux/WSL: bash firmware/build_rootfs.sh <stock rootfsB dump>   -> firmware/out/rootfs-custom.squashfs
# <stock rootfsB dump> = your own device's rootfsB UBI volume (see docs/INSTALL.md). Needs squashfs-tools.
set -euo pipefail
P="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:?usage: $0 <stock rootfsB volume dump>}"
WORK=~/h6631/custom
OUT="$P/firmware/out"
VOL_BYTES=61931520   # size of the rootfsA UBI volume

rm -rf "$WORK"; mkdir -p "$WORK" "$OUT"
unsquashfs -no-xattrs -d "$WORK/root" "$SRC" >/dev/null 2>&1 || true   # dev/console can't be created as non-root; added via pseudo file below
R="$WORK/root"
[ -x "$R/bin/busybox" ] || { echo "extract failed"; exit 1; }

# 1. overlay files
cp -a "$P/firmware/overlay/." "$R/"
for f in $(find "$R/etc/init.d" -type f -newer "$SRC"); do tr -d '\r' < "$f" > "$f.tmp" && cat "$f.tmp" > "$f" && rm "$f.tmp"; done   # strip CRLF from Windows editors
chmod 755 "$R/etc/init.d/govee_init.sh"; chmod 644 "$R/etc/init.d/load_script.conf"

# 2. rc.final: drop telnetd (password login service)
sed -i 's|^telnetd -l /bin/login &|# telnetd removed (custom firmware)|' "$R/etc/init.d/rc.final"
grep -q '^telnetd' "$R/etc/init.d/rc.final" && { echo "telnetd still present"; exit 1; }

# 3. root's ~/.ssh -> persistent storage
rm -rf "$R/root/.ssh"; ln -s /data/custom/ssh "$R/root/.ssh"

# 4. marker so we can tell which firmware booted
echo "custom-$(date -u +%Y%m%d-%H%M%S)" > "$R/etc/custom_release"

rm -f "$OUT/rootfs-custom.squashfs"
mksquashfs "$R" "$OUT/rootfs-custom.squashfs" -comp xz -b 262144 -all-root -no-xattrs -noappend \
    -p '/dev/console c 600 0 0 5 1' >/dev/null
SZ=$(stat -c %s "$OUT/rootfs-custom.squashfs")
echo "image: $OUT/rootfs-custom.squashfs ($SZ bytes, volume $VOL_BYTES)"
[ "$SZ" -lt "$VOL_BYTES" ] || { echo "TOO BIG"; exit 1; }
md5sum "$OUT/rootfs-custom.squashfs" | tee "$OUT/rootfs-custom.md5"
cat "$R/etc/custom_release"
