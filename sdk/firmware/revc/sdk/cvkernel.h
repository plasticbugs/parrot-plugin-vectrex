// cvkernel.h -- the console's side of a plugin whose ROM is PSG Synth's
// kernel (../midihello/midihello.asm): a 256x192 screen sent as VDP write
// lists, a sound mailbox (10 PSG bytes), and the kernel's reports (HELLO and
// ACK for the screen, the joystick, the keypad). mailbox.h has the protocol;
// cvscreen.h sends the screen.
//
//   core 1  serves the kernel's 32K image, one table lookup a read, and
//           notes the kernel's reports. On RESET (a settled header read
//           more than a second after it started) it switches to
//           handoff.asm inside that read, so the console parks in its RAM.
//   core 0  the plugin's own loop, which calls cvk_task() each pass: the
//           power watch (the data outputs driven only while nCART says the
//           console is on), and back to the menu once the console is parked
//           (or switched off): the launch block becomes a return block with
//           the plugin's note (cvlaunch.h), and the cart reboots into the
//           menu, which the parked console then boots.
//
// This is midihello.c's chained build (PSG_SYNTH_CHAINED), as a library; the
// handoff trace (../sdplayer/htrace.h) is recorded the same way, so 't' on
// the menu's USB serial prints a plugin's switch too.
//
// Needs the plugin to be a no_flash build (it runs from SRAM: the bus loops
// mustn't wait on flash).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cvlaunch.h"

typedef struct {
    const uint8_t *image;                  // the kernel's 32K image, permuted (bin2h_revc.py's rom_image)
    const uint8_t *addr_line;              // ...and its pin order: GPIO g carries address line addr_line[g]
    const uint8_t *data_bit;               // GPIO 16 + k drives data bit data_bit[k]
    const uint8_t *handoff;                // handoff.bin, as assembled
    size_t handoff_len;
} cvk_config_t;

// the pins, the image and mailbox, the handoff, the trace; then core 1 serves
void cvk_start(const cvk_config_t *c);

// each pass of the plugin's loop: the power watch, and back to the menu
// (it doesn't return then) after RESET, the console going off, or both fire
// buttons and * (a USB pad's Select and Start) held for 2 seconds (cvhome.h)
void cvk_task(uint32_t now_us);

// false: the fire buttons and * (Select and Start) held don't go back to the
// menu, for a plugin whose game needs them held together (true again: they do)
void cvk_home(bool on);

// the menu's bottom line when the plugin goes back (at most 32 characters; NULL: none)
void cvk_note(const char *note);

// the kernel's next report, one a frame of each kind: true and its kind
// (REPORT_KIND_*, mailbox.h) and A0-A5, or false if there's none new
bool cvk_report(uint32_t now_us, int *kind, uint8_t *bits);

// the mailbox ($FF00-$FF7F, mailbox.h): set bytes, then publish what
// changed (the sequence numbers last)
void cvk_mb(unsigned i, uint8_t v);
void cvk_publish(void);
// the sound: the PSG's 10 bytes as mailbox.h's SND has them (tone low and
// high and volume for channels 0-2, then the noise volume: the PSG's own
// latch bytes, so all zeros is a tone, not silence); cvk_silence mutes all
// four, and is what cvk_start() leaves
void cvk_sound(const uint8_t snd[10]);
extern const uint8_t cvk_silence[10];
// a VDP write list at LIST_BASE, then its LIST_FULL and LIST_SEQ (published)
void cvk_list(const uint8_t *list, size_t n, bool full, uint8_t seq);

bool cvk_console_on(void);                 // the data outputs are driven

// The launch block the menu started this plugin with (the file to open, if
// any: cvsd_open_launched()), or NULL: started some other way (picotool).
// It stays in place until the plugin goes back.
const cvl_t *cvk_launch(void);
