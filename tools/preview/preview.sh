#!/usr/bin/env bash
# Renders a few frames of the game on your PC (no PS3, no SDK) to PNG/PPM
# so you can check the renderer. Needs gcc. Works on Linux/macOS/WSL.
set -euo pipefail
cd "$(dirname "$0")"
gcc -std=gnu99 -O2 -Wall -DBLOX_FIXED_DT -Istub ../../source/main.c -o preview.bin -lm
./preview.bin
python3 - <<'PY' 2>/dev/null || echo "frames written as .ppm (install Pillow to get PNGs)"
from PIL import Image
import glob
for f in glob.glob("frame*.ppm"):
    Image.open(f).save(f.replace(".ppm", ".png"))
print("wrote frame*.png")
PY
