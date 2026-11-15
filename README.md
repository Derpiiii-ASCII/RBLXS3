# blox-ps3

Blox PS3 prototype (PSL1GHT, runs on the Cell PPE).
Controls: left stick = move, right stick = camera, X = jump, L2 = sprint, START = pause menu.
Menus: D-pad or left stick, L1/R1 = tabs, X = select, O = back.

- **Main menu** (Xbox-dashboard style): PLAY, MAPS, SKINS, AVATAR, SETTINGS, QUIT.
- **Maps:** Baseplate, Castle (walls, towers, stairs to the wall walk, drawbridge, keep).
- **Skins:** 11 presets (NOOB, RED, ZOMBIE, NINJA, ROBOT, PINK, GOLD, TOP HAT MAN, SKATER BOY, KING, SNOW) + CUSTOM.
- **Avatar creator:** hat (top hat, skater cap, beanie, crown, headband, antenna), hat/skin/shirt/pants colours, face, randomize, reset.
- **Settings:** control tips, FPS counter, FPS in menus, sound, FPS cap (15/30/45/60/uncapped/custom).
- **Sound:** walk, jump and landing effects (synthesized, no audio files).
- Player models are drawn with culling disabled (`PLAYER_CULL 0` in `source/main.c`).
- Everything is saved to `/dev_hdd0/game/BLOX00001/USRDIR/blox-ps3.sav` and `/dev_hdd0/tmp/blox-ps3.sav`.

## Build
Dependencies (Docker) are installed automatically the first time if missing.
- **Windows:** double-click `windows\build.bat` (or `windows\build.ps1`). Uses winget to install Docker Desktop; you may need to approve a UAC prompt and restart once.
- **Linux/macOS/WSL:** `./linux/build.sh` (apt/dnf/yum/pacman/zypper/apk, or Homebrew on macOS). Uses a native PSL1GHT toolchain if `PSL1GHT` is set.
- Actions (all scripts): `self` = .self only (RPCS3); `pkg` = .pkg only, no .self; `clean`; `shell`. No argument = both.
- Set `PS3_NO_INSTALL=1` to stop any auto-install. Image: `ghcr.io/altps3/ps3dev:latest` (override with `PS3_IMAGE`).

Outputs land in the project root: `blox-ps3.self` (RPCS3, multiMAN) and `blox-ps3.pkg` (CFW/HEN). There is only ever one package file, `blox-ps3.pkg`: install that one. The XMB icon is `ICON0.PNG` (320x176); replace it to change the icon.

## Check the renderer without a PS3
`bash tools/preview/preview.sh` (needs gcc) writes frame PNGs.

## If `make pkg` fails
Copy the `pkg:` target from a PSL1GHT sample Makefile (e.g. samples/graphics/rsxtest).
