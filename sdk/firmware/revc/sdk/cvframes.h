// cvframes.h -- the console's side of a plugin whose ROM is the Game Boy
// kernel (coleco/gbcv/kernel/gbkernel.asm): a picture and sound made by the
// cart, sent as one command list a TV frame (the NES plugin's nescv; the
// menu's own Game Boy mode works the same way).
//
//   core 1  serves the kernel as a MegaCart: its 16K at $8000-$BFFF, and at
//           $C000-$FFFF the list for this frame. Each frame the kernel reads
//           $FFC0+joystick, $FFD0+fires and $FFE0+keypad (the controller, in
//           the address), then $FFF0+seq, which makes the newest list the
//           plugin published the one at $C000. With nothing new, the last
//           list runs again: a list must leave the console as it found it if
//           it runs twice (nescv's and gbcv's do: VRAM writes, registers and
//           PSG bytes, all absolute). On RESET (a settled header read more
//           than a second after it started) it switches to handoff.asm
//           inside that read, so the console parks in its RAM.
//   core 0  the plugin's own loop, which calls cvf_task() each pass: the
//           power watch, and back to the menu once the console is parked
//           (cvkernel.h has the same).
//
// The list format and what each command costs are in gbkernel.asm's
// header; a list must keep inside its frame by them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cvlaunch.h"

typedef struct {
    const uint8_t *kernel;                 // gbkernel.bin, as assembled (16K)
    size_t kernel_len;
    const uint8_t *handoff;                // handoff.bin, as assembled
    size_t handoff_len;
} cvf_config_t;

// the pins, the kernel, the handoff, the trace; then core 1 serves
void cvf_start(const cvf_config_t *c);

// each pass of the plugin's loop: the power watch, and back to the menu
// (it doesn't return then) after RESET, the console going off, or both fire
// buttons and * (a USB pad's Select and Start) held for 2 seconds (cvhome.h)
void cvf_task(uint32_t now_us);

// false: the fire buttons and * (Select and Start) held don't go back to the
// menu, for a plugin whose game needs them held together (true again: they do)
void cvf_home(bool on);

// the menu's bottom line when the plugin goes back (at most 32 characters)
void cvf_note(const char *note);

// fn runs once on the way back to the menu, after RESET or the console going
// off (not through cvf_menu_now, a fault's way): core 1 has stopped and the console is parked in
// its RAM (or off), so it may take its time, a game's save to the card say.
// It may call cvf_note(); it must feed the watchdog if one runs.
void cvf_on_exit(void (*fn)(void));

// the note in the return block now, as if going back: it stays there through
// a watchdog reboot. The launch block is gone then (cvf_launch() is NULL).
void cvf_note_now(void);

// back to the menu now (the bus let go, the return block with the note), as
// after RESET; for a plugin that can't go on (cvdiag.h's fault handler)
void __attribute__((noreturn)) cvf_menu_now(void);

// TV frames the console has started (a read of $FFF0+seq each)
uint32_t cvf_frames(void);

// the controller as the kernel last read it, low = pressed: joystick bits
// 0-3 (N E S W), fires bit 0 (left) and 1 (right), the keypad's code ($F:
// none; gbplay.c's look_key() has the digits)
void cvf_input(uint8_t *joy, uint8_t *fire, uint8_t *key);

// a list for the console's next frame, copied into the bank it isn't
// running; false (and nothing copied) while the last one published hasn't
// been taken yet. n <= CVF_LIST_MAX.
#define CVF_LIST_MAX 0x4000
bool cvf_publish(const uint8_t *list, size_t n);
bool cvf_waiting(void);                    // one published, not taken yet

bool cvf_console_on(void);                 // the data outputs are driven

// what core 1's loop refused since it started, with cvframes_serve2()
// (cvfserve.S; all 0 without): addresses that looked like a select for two
// samples and weren't; selects taken as their read ended; addresses that
// looked like the header ($8000) and weren't
void cvf_bus_counts(uint32_t *unsel, uint32_t *early, uint32_t *unhdr);

// the launch block the menu started this plugin with, or NULL
const cvl_t *cvf_launch(void);
