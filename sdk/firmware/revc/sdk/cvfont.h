// cvfont.h -- the SDK's small font: 6x8 cells, upper and lower case, every
// character from space to ~. 40 columns fit in the 240 pixels a TV shows for
// sure (x 8 to 247), against 30 of vdp.h's 8x8 upper-case font.
//
// Letters are 5x7 in the cell's top left, the eighth row is for descenders
// (g j p q y, the comma) and the sixth column is the gap. The font was drawn
// for this SDK (mkcvfont.py holds the drawings) and comes under the SDK's
// licence: use it in any plugin.
//
// A cell is drawn whole, its background too, so text is overwritten in
// place. Mind the VDP's rule (vdp.h): two colours in any 8x1 run, and cells
// of 6 don't line up with those runs, so keep one pair of colours along a
// row of text, at least where two cells share a run.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdint.h>

#include "../midihello/vdp.h"

#define CVFONT_W 6
#define CVFONT_H 8

// The glyphs, space first: 8 rows from the top, bit 7 the left pixel.
extern const uint8_t cvfont[95][8];

// One character's cell at x, y: its pixels in fg, the rest in bg. Anything
// outside space to ~ is drawn as '?'.
void cvfont_char(canvas_t *c, int x, int y, int ch, uint8_t fg, uint8_t bg);

// A line of exactly cols cells: s, cut or padded with spaces (s may be NULL).
void cvfont_line(canvas_t *c, int x, int y, int cols, const char *s, uint8_t fg, uint8_t bg);
