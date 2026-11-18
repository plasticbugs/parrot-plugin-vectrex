# The plugin SDK

What a plugin for the SD cart's menu (`../sdplayer`) is built from: the
parts that took measuring on the console, so a plugin gets them right
without redoing that work. `docs/plugins.md` has the design; `../nesinfo`
is the example to copy.

| File | What it is |
|---|---|
| `cvlaunch.h` | The launch and return blocks: 1K at `0x2007EC00` that the menu fills before it starts a plugin (the plugin's ID, the file to open, where the menu was) and the plugin turns into a return block, with a note for the menu's bottom line. Header-only, portable. |
| `cvkernel.c/.h` | The console's side, for a plugin whose ROM is PSG Synth's kernel (`../midihello/midihello.asm`): core 1 serves it; the sound mailbox (`cvk_sound`); VDP write lists (`cvk_list`); the kernel's reports (`cvk_report`); the power watch; and RESET, which serves `handoff.asm` inside the BIOS's header read, waits for the console to park, writes the return block and reboots into the menu (`cvk_task`). `midihello.c`'s chained build, as a library. |
| `cvscreen.c/.h` | The screen: draw into a 256x192 canvas (`../midihello/vdp.h`), and what changed goes to the console one list at a time, with the kernel's HELLO / ACK protocol. Portable. |
| `cvsd.c/.h` | The microSD card on SPI0, as the menu drives it, and `cvsd_open_launched()`: the card mounted and the launch block's file opened by its first cluster, with no path lookup. `cvsd_resume()` instead of `cvsd_init()` carries on with the card as the menu left it, started (it answers CMD58 as a started card does), which saves the card's start: 0.1 to 1 s (`../cvplay` does; false means start it with `cvsd_init()`). The file system is `../sdplayer/fsro.h` (FAT16, FAT32, exFAT; MBR, GPT). |
| `cvframes.c/.h` | The console's side for a plugin that makes whole pictures on the cart (the NES and Game Boy plugins): the Game Boy kernel (`coleco/gbcv/kernel/gbkernel.asm`), which runs one command list a TV frame. Core 1 serves it as a MegaCart and hands the console the newest list at each frame's start (`cvf_publish`), the controller as the kernel read it (`cvf_input`), TV frames started (`cvf_frames`); the power watch and RESET as `cvkernel.c`. The menu's Game Boy mode, as a library. |
| `cvpsram.c/.h` | The cart's 8 MB PSRAM, set up as the menu's Game Boy mode does: QPI at clk_sys / 2, mapped through the XIP cache (`CVPSRAM_CACHED`), with an uncached alias for filling it. |
| `cvpad.c/.h` | USB game pads for a plugin: the USB port becomes a host (TinyUSB, through a powered hub), and up to two pads read as `gamepad_t` (`gamepad.h`: buttons named by where they are, sticks -2048..2047): the Switch Pro Controller and pads that act as one (`swpro.c`, which starts it), and plain HID pads (`hidpad.c`, from their report descriptor). `cvpad_dirs()`: the d-pad or the left stick as directions. Its TinyUSB configuration is `cvpad_tusb/tusb_config.h`. With `cvpad_tinyusb.cmake` included before `pico_sdk_import.cmake` (the NES plugin does) the USB host is `tinyusb/` (TinyUSB 0.21 and its fixes since: `tinyusb/VENDORED.md`) and the pads' task runs every millisecond from a timer's interrupt, never waiting; `cvpad_get()` latches a button tapped between two calls. Without it the host is the pico-sdk's 0.18, run from the plugin's loop by `cvpad_task()`: it sleeps there for 450 ms and more whenever a device is plugged in and can panic on a late buffer (NES 0.4.2 on the console). `cvpad_info()`, `cvpad_set_log()`: what `../padtest/plugin` (PADTEST.CVP) puts on the TV. `cvpad_task_max_us()`: the task's longest run so far. The drivers came from `../padtest`, which tests them (`padtest/test/run.sh`). |
| `cvheap.c/.h` | A heap for a plugin that needs `new` and `malloc` (an emulator core): `_sbrk` stops below the launch block, so `malloc` returns NULL there instead of overwriting it. `cvcart_plugin(... HEAP)` links it. `cvheap_reserve()`, before the first `malloc`: the whole heap from `_sbrk` at once, which newlib's `malloc` otherwise takes 4K blocks at a time and can't then use to the end. `cvheap_largest()`: the largest block still to be had (the NES plugin's note). |
| `mkpins.py` | `pins.h` (the address and data pin order) from `hw3/pins.py`, for `cvframes.c`; the same table as the menu's. |
| `cvplugin.cmake` | `cvkernel_use()` or `cvframes_use()`, `cvsd_use()`, `cvpsram_use()`, `cvpad_use()`, and `cvcart_plugin()`, which makes the no_flash binary, packages it and writes the `.CVP`. |
| `cvfserve.S` | With `cvframes_serve2(target)` after `cvframes_use()`: core 1's bus loop is this one, the games' loop (`cvplay/cvserve.S`, `cvplay/CVSERVE.md`) for the frame lists. Nothing is made of one sample of the pins, and a select-like address doesn't stop the serving. Without it the loop is `cvframes.c`'s own and the plugin builds as before. `test/cvframes.sh NEW.elf [OLD.elf]` runs either against the kernel's bus cycles. Use it in a new frame plugin: on the console (2026-11-18) it cured The Empire Strikes Back's glitches, and every frame plugin here is built with it. The old loop is still the default so a plugin that doesn't ask builds as before. |
| `cvreadme.py` | A plugin's `README.md` as the cart shows it: plain ASCII, 30 columns (`docs/plugins.md`, "The README"). `cvcart_plugin(... README README.md)` runs it through `mkcvp.py --readme`; by itself it prints the text. `test/test_cvreadme.py` checks each rule. |
| `reserve.ld` | Added to a plugin's link: its `.bss` must stop below the launch block (crt0 zeroes `.bss` before `main`). |

## A plugin

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/../sdk/cvplugin.cmake)    # after pico_sdk_init()
add_executable(nesinfo nesinfo.c nesread.c nesui.c ines.c)
cvkernel_use(nesinfo)
cvsd_use(nesinfo)
cvcart_plugin(nesinfo ID parrotcart.nesinfo NAME "NES Info" VERSION 1.0.0 BY "Parrot Cart"
              HOME /NES OPENS .nes ICON nesinfo_icon.txt FILE_ICON nes_file_icon.txt
              README README.md)
```

```c
#include "cvkernel.h"
#include "cvscreen.h"
#include "cvsd.h"
#include "handoff.h"         // generated by cvkernel_use
#include "rom_image.h"       // generated by cvkernel_use

int main(void) {
    static const cvk_config_t cfg = {rom_image, addr_line, data_bit, handoff_bin, HANDOFF_BIN_LEN};
    cvk_start(&cfg);                        // first: the console is parked, waiting
    const cvl_t *b = cvk_launch();          // the file to open, or NULL
    for (;;) {
        uint32_t t = time_us_32();
        cvk_task(t);                        // every pass: power, and RESET back to the menu
        int k; uint8_t bits;
        while (cvk_report(t, &k, &bits))
            if (k == REPORT_KIND_ACK || k == REPORT_KIND_HELLO)
                cvs_report(&scr, k == REPORT_KIND_HELLO, bits & 31, t);
        // ... the plugin's own work, in short steps ...
        size_t n = cvs_update(&scr, draw, ctx, list, sizeof list, t);   // at most every ~20 ms
        if (n) cvk_list(list, n, scr.out_full, scr.seq);
    }
}
```

Rules a plugin keeps:

- **Call `cvk_task()` often.** It drives the data bus only while the console
  is on, and it is what takes the console back to the menu. A blocking step
  (the card's start, up to 1.5 s) is fine once; a loop that never returns
  isn't.
- **No malloc, unless `HEAP`.** The SDK's heap would start at `.bss`'s end
  and grow into the launch block; `cvcart_plugin()` fails the build if
  `malloc` is linked without `HEAP` (`cvheap.h`), and with it `malloc`
  returns NULL at the launch block.
- **A tight heap: `cvheap_reserve()` first, and measure.** A plugin's code
  is in SRAM too, so every K linked is a K off the heap (the USB host is
  26K; newlib's printf, which one stray `fprintf` brings in, 19K). The Mac
  tests use the Mac's `malloc` and say nothing about it:
  `../nes/test/onarm.py` runs a plugin's ELF on an emulated M33 with the
  real one.
- **RAM below `0x2007EC00`**, stacks in scratch X and Y (the SDK's
  default). `reserve.ld` checks `.bss`.
- **The card: read anything, write only in place** (`fs_write_sec`), with
  `cvsd_open_launched(..., writable = true, ...)`.
- **The file's name** comes from the launch block's path; a character
  outside ASCII is `?` there, so open the file by the block's cluster
  (`cvsd_open_launched`), not by its path.
- **Leave a note** (`cvk_note("Saved")`) for the menu's bottom line when the
  plugin goes back: 32 characters at most.

## Pictures made on the cart: `cvframes`

`../nes/plugin` is the example. The console runs the Game Boy kernel, and
each TV frame the plugin publishes one list for the next frame (the list
format and its costs are in `gbkernel.asm`'s header).

In `CMakeLists.txt`, a new frame plugin should ask for the two-sample bus
loop:

```cmake
cvframes_use(myplugin)
cvframes_serve2(myplugin)    # core 1's bus loop is cvfserve.S
```

`cvframes_serve2()` is recommended. Without it core 1 runs `cvframes.c`'s
older loop, which acts on one sample of the pins and can hand the console a
wrong byte or take a stray address for RESET. The plugin's code is the same
either way. `cvf_bus_counts()` says what the new loop refused, for the note.
The loop is timed for the SDK's 150 MHz clock.

Then in `main()`:

```c
#include "cvframes.h"
#include "gbkernel.h"        // generated by cvframes_use
#include "handoff.h"

static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
cvf_start(&cfg);
uint32_t seen = 0;
for (;;) {
    cvf_task(time_us_32());
    if (cvf_frames() == seen || cvf_waiting()) continue;
    seen = cvf_frames();                    // a TV frame started: make the next list
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    size_t n = make_list(list, joy, fire, key);
    cvf_publish(list, n);                   // taken at the next frame's start
}
```

- **A list must be safe to run twice.** With nothing new published, the
  console runs the last list again; absolute VRAM writes, registers and PSG
  bytes are (`nescv` and the menu's `gbcv` keep to that).
- **Never drop a list** when the encoder models VRAM: keep it until
  `cvf_publish()` takes it.
- **Last words.** `cvf_on_exit(fn)`: fn runs once on the way back to the
  menu (RESET, or the console off), with the console parked: time to write
  a save (the Game Boy plugin's `.sav`). It may leave a note.

`../gbcv/plugin` is the second example: the menu's Game Boy mode as a
plugin, with saves on the card and USB pads, and a Mac test that runs its
`gb.c` as it is (`test/gb_host.c`: the card an image, the console a model).

## Not here yet

- A display kernel for ScummVM (its own Z80 side), USB host helpers (PSG
  Synth and the pad test have their own).
- `cv_pick_file` (a plugin asking the menu for a second file).
