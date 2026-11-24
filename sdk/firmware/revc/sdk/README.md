# The plugin SDK

These are the libraries you build a plugin for the cart's menu
(`../sdplayer`) with. They cover the parts we had to measure on a real
console, so your plugin gets them right without redoing that work.
`docs/plugins.md` describes the design, and `../nesinfo` is a good
example to copy.

| File | What it is |
|---|---|
| `cvlaunch.h` | The launch and return blocks, 1 KB at `0x2007EC00`. The menu fills in the launch block before it starts a plugin, with the plugin's ID, the file to open and where the menu was. The plugin turns it into a return block, with a note for the menu's bottom line. It's header-only and portable. |
| `cvkernel.c/.h` | The console side for a plugin whose ROM is PSG Synth's kernel (`../midihello/midihello.asm`). Core 1 supplies the ROM to the console. This part also has the sound mailbox (`cvk_sound`), VDP write lists (`cvk_list`), the kernel's reports (`cvk_report`) and the power check. On RESET, `cvk_task` gives the console `handoff.asm` during the BIOS's header read, waits for the console to park, writes the return block and reboots into the menu. It's `midihello.c`'s chained build, as a library. |
| `cvscreen.c/.h` | The screen. You draw into a 256x192 canvas (`../midihello/vdp.h`), and the changes go to the console one list at a time, using the kernel's HELLO / ACK protocol. It's portable. |
| `cvsd.c/.h` | The microSD card on SPI0, driven the same way the menu drives it. `cvsd_open_launched()` mounts the card and opens the launch block's file by its first cluster, with no path lookup. If you call `cvsd_resume()` instead of `cvsd_init()`, the plugin keeps using the card as the menu left it, already started (the card responds to CMD58 the way a started card does). That skips starting the card, which takes 0.1 to 1 s. `../cvplay` does this. If it returns false, start the card with `cvsd_init()`. The file system is `../sdplayer/fsro.h` (FAT16, FAT32 and exFAT, with MBR or GPT). |
| `cvframes.c/.h` | The console side for a plugin that makes whole pictures on the cart, like the NES and Game Boy plugins. It uses the Game Boy kernel (`coleco/gbcv/kernel/gbkernel.asm`), which runs one command list each TV frame. Core 1 presents it to the console as a MegaCart and gives the console the newest list at the start of each frame (`cvf_publish`). The plugin gets the controller as the kernel read it (`cvf_input`) and the count of TV frames started (`cvf_frames`). The power check and RESET work as in `cvkernel.c`. It's the menu's Game Boy mode, as a library. |
| `cvpsram.c/.h` | The cart's 8 MB PSRAM, set up the same way as in the menu's Game Boy mode. It runs QPI at clk_sys / 2, mapped through the XIP cache (`CVPSRAM_CACHED`), with an uncached alias for filling it. |
| `cvpad.c/.h` | USB game pads. The USB port becomes a host (TinyUSB, through a powered hub), and up to two pads are read as `gamepad_t` (`gamepad.h`, with buttons named by where they are and sticks from -2048 to 2047). It supports the Switch Pro Controller and pads that act like one (`swpro.c`, which starts them up), and plain HID pads (`hidpad.c`, which reads their report descriptor). `cvpad_dirs()` gives the d-pad or the left stick as directions. The TinyUSB configuration is `cvpad_tusb/tusb_config.h`. If you include `cvpad_tinyusb.cmake` before `pico_sdk_import.cmake`, as the NES plugin does, the USB host is `tinyusb/` (TinyUSB 0.21 and later fixes, listed in `tinyusb/VENDORED.md`). The pads' task then runs every millisecond from a timer interrupt and never waits, and `cvpad_get()` latches a button tapped between two calls. Without it, the host is the pico-sdk's TinyUSB 0.18, run from the plugin's loop by `cvpad_task()`. That version sleeps for 450 ms or more whenever a device is plugged in, and it can panic on a late buffer (as NES 0.4.2 did on the console). `cvpad_info()` and `cvpad_set_log()` give what `../padtest/plugin` (PADTEST.CVP) shows on the TV, and `cvpad_task_max_us()` gives the task's longest run so far. The drivers came from `../padtest`, which tests them (`padtest/test/run.sh`). |
| `cvusb.c/.h` | The USB port, with each feature as a switch. `cvcart_features(target PADS KEYBOARD MOUSE MIDI_IN MIDI_OUT)` links only the features you turn on, configures TinyUSB with only those classes (`cvusb_tusb/tusb_config.h`) and sets the `.CVP`'s USES. It covers pads (`cvpad.c`, built with `CVUSB`), keyboards and mice in boot protocol (`cvusb_keys.c`, from the SCUMM plugin's `usbinput.c`), a USB MIDI device with the cart as host (`cvusb_midih.c`, PSG Synth's `midi_host.c` on TinyUSB 0.21), the cart as a MIDI device on a computer (`cvusb_midid.c`, the SCUMM plugin's `usbmidi_dev.c`), and MIDI packets in both directions (`cvusb_midi.c`, from PSG Synth's `usbmidi.c`). With host features and MIDI_OUT together, the port tries the computer first and becomes a host if no computer connects, as the SCUMM plugin does. The calls are `cvusb_start()`, `cvusb_task()` and the getters. See "USB features" below. |
| `cvfont.c/.h` | A 6x8 font with lower case, for a canvas: 40 columns in the 240 pixels a TV shows. `cvfont_use()` links it. It's portable. |
| `cvedit.c/.h` | A plain text editor to embed: word wrap, keys from `cvusb.h`, and files on the card (open, and a save that survives a power cut). The host gives it a buffer and a region of the canvas, feeds it keys, and gets an event for the keys it doesn't use (keypad 4, Ctrl+R). The Text Editor plugin (`../textedit`) is a thin wrapper around it. `cvedit_use()` links it. It's portable. |
| `cvinput.h` | Player 1 as a single input. It combines the console's controller (from either kernel's reports) with USB pad 1 when PADS is on, mapped the way the menu maps a pad. It's header-only. |
| `cvheap.c/.h` | A heap for a plugin that needs `new` and `malloc`, such as an emulator core. `_sbrk` stops below the launch block, so `malloc` returns NULL there instead of overwriting it. `cvcart_plugin(... HEAP)` links it. Call `cvheap_reserve()` before the first `malloc` to take the whole heap from `_sbrk` at once. Otherwise newlib's `malloc` takes it 4 KB at a time and can't use it all the way to the end. `cvheap_largest()` gives the largest block still free (the NES plugin shows it in its note). |
| `mkpins.py` | Makes `pins.h` (the address and data pin order) from `hw3/pins.py`, for `cvframes.c`. It's the same table the menu uses. |
| `cvplugin.cmake` | The CMake functions `cvkernel_use()` or `cvframes_use()`, `cvcart_features()` (or by hand, `cvsd_use()`, `cvpsram_use()` and `cvpad_use()`), `cvfont_use()` and `cvedit_use()`, and `cvcart_plugin()`, which makes the no_flash binary, packages it and writes the `.CVP`. |
| `cvfserve.S` | The two-sample bus loop. With `cvframes_serve2(target)` after `cvframes_use()`, core 1 serves the frame lists with the games' loop (`cvplay/cvserve.S`, `cvplay/CVSERVE.md`). It never acts on one sample of the pins, and an address that looks like a select doesn't stop the serving. `test/cvframes.sh NEW.elf [OLD.elf]` runs either loop against the kernel's bus cycles. Use it in a new frame plugin: on the console (2026-11-18) it cured The Empire Strikes Back's glitches, and every frame plugin here is built with it. The old loop in `cvframes.c` is still the default, so a plugin that doesn't ask for this one builds as before. |
| `cvreadme.py` | Converts a plugin's `README.md` into what the cart shows, plain ASCII at 30 columns (`docs/plugins.md`, "The README"). `cvcart_plugin(... README README.md)` runs it through `mkcvp.py --readme`. Run by itself, it prints the text. `test/test_cvreadme.py` checks each rule. |
| `reserve.ld` | Added to a plugin's link to make sure its `.bss` stops below the launch block, because crt0 zeroes `.bss` before `main`. |

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
        // ... the plugin's work, in short steps ...
        size_t n = cvs_update(&scr, draw, ctx, list, sizeof list, t);   // at most every ~20 ms
        if (n) cvk_list(list, n, scr.out_full, scr.seq);
    }
}
```

A plugin needs to follow these rules:

- Call `cvk_task()` regularly. It drives the data bus only while the
  console is on, and it's what takes the console back to the menu. One
  blocking step, such as starting the card (up to 1.5 s), is fine, but a
  loop that never returns keeps the console from getting back to the
  menu.
- Use `malloc` only with `HEAP`. Without it, the heap would start at the
  end of `.bss` and grow into the launch block, so `cvcart_plugin()`
  fails the build if `malloc` is linked without `HEAP` (`cvheap.h`).
  With `HEAP`, `malloc` returns NULL when it reaches the launch block.
- If the heap is tight, call `cvheap_reserve()` first, and measure. A
  plugin's code is in SRAM too, so every KB you link is a KB less for
  the heap. The USB host takes 26 KB, and newlib's printf, which a single
  stray `fprintf` pulls in, takes 19 KB. The Mac tests use the Mac's
  `malloc`, so they don't show this. `../nes/test/onarm.py` runs a
  plugin's ELF on an emulated M33 with the real `malloc`.
- Keep RAM use below `0x2007EC00`, with the stacks in scratch X and Y
  (the SDK's default). `reserve.ld` checks `.bss`.
- On the card, your plugin can read anything, but it should only write
  in place (`fs_write_sec`), with
  `cvsd_open_launched(..., writable = true, ...)`.
- The file's name comes from the launch block's path, where any character
  outside ASCII is `?`. Open the file by the block's cluster
  (`cvsd_open_launched`) instead of by its path.
- Leave a note (`cvk_note("Saved")`) for the menu's bottom line when the
  plugin goes back. It can be up to 32 characters.

## USB features: `cvusb`

`../featuretest` (FEATTEST.CVP) is the example, with every feature on and
every input shown on the TV. In CMake, put this before `cvcart_plugin()`:

```cmake
include(${CMAKE_CURRENT_LIST_DIR}/../sdk/cvpad_tinyusb.cmake)   # before pico_sdk_import.cmake
...
cvcart_features(myplugin PADS KEYBOARD MOUSE MIDI_IN MIDI_OUT PSRAM CARD)
cvcart_plugin(myplugin ID you.myplugin NAME "My Plugin" VERSION 0.1.0)   # USES follow the features
```

| Feature | What plugs in | The port | USES | Code |
|---|---|---|---|---|
| `PADS` | up to two game pads, including the Switch Pro Controller (and pads in Switch mode) and plain HID pads | host, through a powered hub | usb-host | `cvpad.c`, `swpro.c`, `hidpad.c` |
| `KEYBOARD` | USB keyboards | host | usb-host | `cvusb_keys.c` |
| `MOUSE` | USB mice | host | usb-host | `cvusb_keys.c` |
| `MIDI_IN` | a USB MIDI keyboard, or any class-compliant MIDI device | host | usb-host | `cvusb_midih.c`, `cvusb_midi.c` |
| `MIDI_OUT` | a computer, with the cart as a MIDI device on it, named after the plugin | device | usb-device | `cvusb_midid.c`, `cvusb_midi.c` |
| `PSRAM` | | | psram | `cvpsram_use()` |
| `CARD` | | | | `cvsd_use()` |

```c
#include "cvusb.h"

cvusb_start();                          // once, after cvk_start() / cvf_start()
for (;;) {
    cvusb_task();                       // every pass: it never waits
    gamepad_t p;  if (cvusb_pad(0, &p)) ...         // PADS: buttons, sticks -2048..2047
    cvkey_t k;    while (cvusb_key(&k)) ...         // KEYBOARD: usage, mods, ascii, down
    cvmouse_t m;  if (cvusb_mouse(&m)) ...          // MOUSE: dx, dy, wheel, buttons
    uint8_t msg[3]; int n;
    while ((n = cvusb_midi_read(msg)) > 0) ...      // MIDI in, whichever side it came from
    cvusb_midi_send(note_on, 3);                    // MIDI out, likewise
    cvusb_mode();  cvusb_status(&s);                // host, device; what is plugged in
}
```

- Host features make the port a host, and MIDI_OUT makes it a device.
  With both, it starts as a device. If a computer connects within 1.5 s,
  the port stays a device, and anything else (a hub, or nothing) makes it
  a host. If a computer responds but doesn't connect, it's asking its
  user ("Allow accessory to connect?" on a Mac). The port waits up to a
  minute for that (`CVUSB_ASKING`), and `cvusb_host_now()` stops
  waiting. The port picks once, at the start.
- As a host, TinyUSB runs every millisecond from a timer interrupt, as it
  did in `cvpad`, so please don't call `tuh_*` yourself. As a device, it
  runs in `cvusb_task()`.
- Calling a getter for a feature that's off is a build error that names
  the switch (`cvusb.h`, using GCC's `error` attribute).
- Here's what each feature costs, compared with a plugin with none (GCC
  9.2.1, `-O3`). A plugin runs from RAM, so its code uses RAM too. Most
  of the cost is TinyUSB's host code, which the host features share:

  | Features | Code | Data |
  |---|---|---|
  | `PADS` | 41.7K | 4.8K |
  | `KEYBOARD` | 32.5K | 3.8K |
  | `MOUSE` | 30.2K | 3.7K |
  | `MIDI_IN` | 28.9K | 3.4K |
  | `MIDI_OUT` | 14.1K | 7.1K |
  | `PADS MIDI_OUT` | 53.8K | 12.0K |
  | all five | 60.8K | 13.2K |

  `PSRAM` and `CARD` cost only what the plugin calls.
- `cvinput.h` combines player 1's inputs for either kernel. USB pad 1
  counts as the console's controller, with the d-pad or stick as the
  stick, B as the left fire button, A as the right, and X and Y as the
  keypad's `*` and `#`.
- `cvpad_use()` and the plugins built with it (NES, SMS, Game Boy, Star
  Wars, the pad test and the menu) work as before, because `cvpad.c`
  built without `CVUSB` is the same code.

`test/run.sh` tests the readers on the Mac, including keyboard and mouse
reports, pad reports, MIDI in both directions, a MIDI device's
descriptors, and `cvinput.h`.

## Pictures made on the cart: `cvframes`

`../nes/plugin` is the example. The console runs the Game Boy kernel,
and each TV frame, the plugin publishes one list for the next frame. The
list format and its costs are in the header of `gbkernel.asm`.

In `CMakeLists.txt`, a new frame plugin should ask for the two-sample bus
loop:

```cmake
cvframes_use(myplugin)
cvframes_serve2(myplugin)    # core 1's bus loop is cvfserve.S
```

`cvframes_serve2()` is recommended. Without it, core 1 runs `cvframes.c`'s
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

- Make each list safe to run twice. If nothing new is published, the
  console runs the last list again. Absolute VRAM writes, registers and
  PSG bytes are safe to repeat, and `nescv` and the menu's `gbcv` stick
  to those.
- If your encoder models VRAM, keep each list until `cvf_publish()` takes
  it, so the model stays in step with the console.
- `cvf_on_exit(fn)` runs fn once on the way back to the menu (on RESET,
  or when the console is switched off), with the console parked. It's a
  good time to write a save, as the Game Boy plugin does with its `.sav`.
  The function can also leave a note.

`../gbcv/plugin` is the second example. It's the menu's Game Boy mode as
a plugin, with saves on the card and USB pads. Its Mac test runs `gb.c`
unchanged (`test/gb_host.c`), with a disk image as the card and a model
of the console.

## Not here yet

- A display kernel for ScummVM, with a separate Z80 side.
- PSG Synth, the SCUMM plugin and the pad test still have separate USB
  code. Only new plugins use `cvusb`.
- Keyboards and mice in report protocol. Only the boot protocol is read,
  so a keyboard's media keys and a mouse's extra buttons don't come
  through, and the wheel only works if the mouse sends it in its boot
  reports.
- Keyboard layouts other than US. The usage is right, but `ascii` is the
  US key's character.
- MIDI over more than one cable (port) of a device. Cable numbers are
  ignored, and only one MIDI device is read at a time.
- `hidpad.c` takes a pad's Rx and Ry for its right stick when it has
  them. A DualShock 4 reports its right stick as Z and Rz and its
  triggers as Rx and Ry, so its right stick would read as the triggers.
- Switching the port between host and device after the start, for
  example when a computer is plugged in later.
- `cv_pick_file`, for a plugin to ask the menu for a second file.
