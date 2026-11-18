// cvscreen.h -- a plugin's screen on PSG Synth's kernel: the plugin draws
// into a canvas (../midihello/vdp.h: 256x192, 15 colours, two in any 8x1
// run), and this sends the console what changed as VDP write lists, one at
// a time (the protocol is mailbox.h's; midihello's screen.h explains why
// each rule is there):
//
//   - the kernel says HELLO from its start until it has run a list: the
//     answer is the whole screen (draw() is told `all`, and the list is
//     LIST_FULL), against VRAM as vdp_init() leaves it
//   - after that each list is answered by an ACK carrying its seq, and only
//     then is the next one made, so a list is never rewritten while the Z80
//     may read it
//   - nothing for CVS_TIMEOUT_US: the console is off or elsewhere; a HELLO
//     then starts over whatever it carries
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../midihello/vdp.h"

#define CVS_LIST_MAX   8192         // a list's bytes: ~70 ms on the Z80
#define CVS_TIMEOUT_US 1000000

// Bring the canvas up to date. all: the console's screen is blank, draw
// everything (the canvas was cleared to black).
typedef void (*cvs_draw_fn)(canvas_t *c, bool all, void *ctx);

typedef struct {
    vdp_t vdp;                  // the console's VRAM, as the lists sent so far leave it
    canvas_t canvas;
    bool ready;                 // the kernel has said HELLO
    bool full;                  // the next list is the whole screen
    bool out, out_full;         // a list is out (and it is a whole screen)
    uint8_t seq;                // the newest list's LIST_SEQ
    uint32_t sent_us;
    uint32_t lists, starts;     // lists made, and whole screens among them
} cvs_t;

void cvs_init(cvs_t *s);

// A report from the kernel: HELLO or ACK, and the seq it carries (A0-A4).
void cvs_report(cvs_t *s, bool hello, uint8_t seq5, uint32_t now_us);

// When the kernel is there and no list is out: draw(), and if anything
// changed write the next list to list (at most max bytes) and return its
// length; the caller sends it (cvk_list(list, n, s->out_full, s->seq)).
// 0: nothing to send now.
size_t cvs_update(cvs_t *s, cvs_draw_fn draw, void *ctx, uint8_t *list, size_t max, uint32_t now_us);

// ---- drawing helpers (on top of vdp.h's) -------------------------------------
// Text in a box w pixels wide: the box filled with bg, then s (cut to fit).
void cvs_line(canvas_t *c, int x, int y, int w, const char *s, uint8_t fg, uint8_t bg, int scale);
