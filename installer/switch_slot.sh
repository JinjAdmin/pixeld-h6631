#!/bin/sh
# switch_slot.sh <A|B>: boot the given slot next time (run on the display; part of the installer payload).
case "$1" in A|B) ;; *) echo "usage: $0 A|B"; exit 1;; esac
fw_setenv boot_partition boot$1 && fw_setenv root_partition rootfs$1 && fw_setenv systemAB_next $1 || exit 1
fw_printenv boot_partition root_partition systemAB_next
