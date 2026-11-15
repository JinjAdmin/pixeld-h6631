#!/bin/sh
# Cross-compile the device programs for the H6631 (Allwinner T113, armhf, glibc 2.29) with Zig.
#   ZIG=/path/to/zig sh build.sh          -> build/pixeld build/bled build/pixeltest
# -fPIE -pie is required: non-PIE Zig armhf builds crash in printf (wrong COPY relocation sizes).
set -e
ZIG=${ZIG:-zig}
T="-target arm-linux-gnueabihf.2.29 -mcpu=cortex_a7 -O2 -fPIE -pie -s -Wall -Wno-unused-function"
mkdir -p build
cd src
$ZIG cc $T -o ../build/pixeld pixeld.c -ldl -lm      # includes features.c and api.c (unity build)
$ZIG cc $T -o ../build/bled bled.c -lm
$ZIG cc $T -o ../build/pixeltest pixeltest.c -ldl
ls -l ../build
