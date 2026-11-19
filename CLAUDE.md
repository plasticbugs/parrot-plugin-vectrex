# CLAUDE.md

This is the Vectrex plugin for Parrot Cart. Parrot Cart is a ColecoVision
cartridge with an RP2350 microcontroller, 8 MB of PSRAM and a microSD card. A
plugin is a program for the cart, packed into one `.CVP` file that goes in the
card's `/PLUGINS` folder. It runs the GCE Vectrex on the cart (the 6809, the
VIA, the integrators and the AY) and draws the vectors on the TV.

README.md is for players. CART-README.md is the text the cart shows on the
plugin's details page. This file is for working on the code.

## The plugin

| | |
|---|---|
| File | `build/VECTREX.CVP` |
| ID | `parrotcart.vectrex` |
| CMake target | `vectrex` |
| Version | `VERSION` in the `cvcart_plugin(...)` call in CMakeLists.txt |
| License | BSD 3-Clause (LICENSE, THIRD-PARTY.md) |

The machine's emulation comes from MAME (THIRD-PARTY.md lists each part and
its authors).

## Layout

- `src/`: the plugin's code, and its icons (`*_icon.txt`, drawn as text).
- `sdk/`: a copy of the Parrot Cart SDK, from the commit in `sdk/SOURCE.txt`.
  Don't edit it here. SDK changes are made upstream, and `tools/update-sdk.sh`
  copies a newer SDK in. The starter kit
  (https://github.com/plasticbugs/parrot-plugin-template) has the same SDK and
  its docs.
- `CART-README.md`: packed into the .CVP and shown by the cart, 30 characters
  wide.
- `tools/ci/`: the CI's setup script, and `cvpinfo.py`, which reads a .CVP's
  header.

## Build

You need CMake, Ninja, Python 3, the Raspberry Pi pico-sdk 2.1.1 and the Arm
GNU Toolchain 9-2019-q4 (GCC 9.2.1). `tools/ci/setup.sh` installs those exact
versions on Ubuntu, and it lists the commits.

```sh
cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
ninja -C build
python3 tools/ci/cvpinfo.py build/VECTREX.CVP     # what the file's header says
```

The same source and compiler make the same file, byte for byte. `BUILD_EPOCH`
in CMakeLists.txt sets SOURCE_DATE_EPOCH for that. Another GCC makes a
different file. It may work, but it won't match the release.

To try it, copy `build/VECTREX.CVP` into `/PLUGINS` on the card, the Vectrex
system ROM in `/VECTREX`, and games (`.vec`, `.gam`) anywhere. Then start it
from the cart's menu. RESET on the console goes back to the menu.

## Releases

- Change the version in CMakeLists.txt and commit, then push a tag `vX.Y.Z`
  with the same number. `.github/workflows/release.yml` builds the plugin,
  checks that the .CVP's version matches the tag, and makes a GitHub release
  with the .CVP, the screenshots and RELEASE_NOTES.md. A tag that doesn't
  match fails.
- `screenshots/` holds up to three 256x192 PNGs of 100 KB at most. The first
  shows the cart's details page, which shows the version, so redraw it for a
  new version.
- The plugins site (https://plasticbugs.github.io/parrot-cart-plugins/) checks
  listed plugins once a day and adds new releases.
- `.github/workflows/build.yml` builds every push and pull request.

## Rules

- No ROMs, BIOS files or game data go in this repository, its releases or its
  screenshots. The system ROM and the games are the player's. Never add any of
  them, even Mine Storm.
- Keep the license notices: LICENSE, THIRD-PARTY.md and the credits in both
  READMEs. Anything new in `vendor/` gets a line in THIRD-PARTY.md.
- The SDK starts core 1, which answers the console's reads of the cartridge.
  The plugin runs on core 0. Whatever core 1 runs has to stay in RAM and
  quick, so read `sdk/firmware/revc/sdk/README.md` before changing it.
- READMEs and release notes are short and plain.

## Notes

- The code is in `src/vectrex/`. `src/starwars/` holds the parts it shares
  with the Star Wars plugin.

## The Claude skill

The starter kit (https://github.com/plasticbugs/parrot-plugin-template) has a
Claude skill for writing Parrot Cart plugins in
`.claude/skills/parrot-cart-plugin`. To use it in this repository, copy that
folder into `~/.claude/skills/`.
