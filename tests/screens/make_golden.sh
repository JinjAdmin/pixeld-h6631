#!/bin/sh
# Regenerate the golden frames (<name>.raw) from the C renderer. Fixed clock and settings so they are reproducible:
#   time 1763600000 (2025-11-20 00:53:20 UTC), TZ=UTC0, Fahrenheit ON, 12-hour clock OFF, no icon files.
# pixel2mqtt's browser renderer must reproduce these byte for byte.
#   sh tests/screens/make_golden.sh [path/to/screen_preview]
set -e
cd "$(dirname "$0")"
P=${1:-../../build/screen_preview}
for scr in *.scr; do
  n=${scr%.scr}
  rm -f "$n.raw"
  TZ=UTC0 "$P" "$n.raw" "$scr" 1 33 --time=1763600000 "$n.values.json" fahrenheit=ON clock12=OFF
done
