# sdk/: a copy of the cart's plugin SDK

Everything under here is copied from the Parrot Cart's own repository by
`tools/update-sdk.sh`, at the paths the files have there, so that the SDK's
CMake and its `#include`s work unchanged. `SOURCE.txt` says which commit.
**Don't edit these files**: a change belongs upstream, and the next update
would overwrite it.

| Path | What it is |
|---|---|
| `firmware/revc/sdk/` | the SDK: `cvplugin.cmake` and the `cv*` libraries. **Its `README.md` is the reference**: read it after the examples. |
| `firmware/revc/sdk/tinyusb/` | TinyUSB 0.21 with the cart's fixes: what plugins' USB is built from |
| `firmware/revc/sdk/cvreadme.py` | README.md to the cart's 30-column text |
| `firmware/revc/midihello/` | the screen kernel (`midihello.asm`, Z80), the parking ROM (`handoff.asm`), the canvas and font (`vdp.c`, `font8.h`), the kernel's mailbox (`mailbox.h`) |
| `gbcv/kernel/gbkernel.asm` | the frame kernel (Z80) |
| `firmware/revc/sdplayer/` | the file system (`fsro.c`), the packer (`mkcvp.py`), and headers the SDK shares with the menu |
| `firmware/revc/cvcart_revc/boards/` | the board definition for the pico-sdk |
| `firmware/revc/bin2h_revc.py`, `hw3/pins.py` | the cart's pin order, which the kernels' images are arranged by |

The SDK's own README and comments mention example plugins beside it
(`../nesinfo`, `../nes`, `../padtest`, `../scumm`) and `docs/plugins.md`:
those are in the cart's repository, not in this copy. The plugin
template, <https://github.com/plasticbugs/parrot-plugin-template>, has
the same copy of the SDK with examples and docs. The developer name
`Parrot Cart` and IDs starting `parrotcart.` are the cart's own plugins.

The names are historical: `midihello` was the first plugin (PSG Synth),
whose kernel became the SDK's screen kernel; `gbcv` is the Game Boy mode
the frame kernel was written for; `sdplayer` is the cart's menu.

To update: `tools/update-sdk.sh PATH_TO_THE_CART_REPOSITORY [REF]`, then
build.
