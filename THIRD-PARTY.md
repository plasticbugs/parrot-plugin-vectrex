# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the BSD 3-Clause license
(`LICENSE`). The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `sdk/firmware/revc/sdk/tinyusb/` | [TinyUSB](https://github.com/hathach/tinyusb) (Ha Thach and contributors), with the cart's fixes, listed in its `VENDORED.md` | MIT (`sdk/firmware/revc/sdk/tinyusb/LICENSE`) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and TinyUSB (MIT).

## MAME

The plugin's own code in `src/` was written for it. Parts of it are ports
of MAME's code, or were written with MAME's code as the reference. That
code is under the BSD 3-Clause license (`LICENSE`). It comes from MAME
(<https://github.com/mamedev/mame>, tag `mame0288`), and its copyright
holders are these.

| Here | From MAME | Copyright holders |
|---|---|---|
| `src/vectrex/vx.c`: the analog side, the inputs, the memory map | `src/mame/miltonbradley/vectrex.cpp`, `vectrex_m.cpp`, `vectrex_v.cpp` | Mathis Rosenhauer (with thanks there to Christopher Salomon and Bruce Tomlin) |
| `src/vectrex/vx.c`: the VIA | `src/devices/machine/6522via.cpp` | Peter Trauner, Mathis Rosenhauer |
| `src/vectrex/vx.c`: the cartridge | `src/devices/bus/vectrex` | Fabio Priuli |
| `src/vectrex/vx.c`: the AY's register access | `src/devices/sound/ay8910.cpp` | Couriersud |
| `src/starwars/m6809_core.h` (its flags, cycle counts and interrupt entry checked against MAME's) | `src/devices/cpu/m6809` | Nathan Woods |

The 6809 in `src/starwars/` was written for the cart's Star Wars plugin
(<https://github.com/plasticbugs/parrot-plugin-starwars>). The picture's
lists and the sound in `src/vectrex/vxcv.c` follow that plugin's too.

Vectrex is a trademark of its owner. The system ROM and the games belong
to their owners. This plugin contains none of them.

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
