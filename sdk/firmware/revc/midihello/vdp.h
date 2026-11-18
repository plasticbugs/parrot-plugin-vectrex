// vdp.h -- screens drawn on the cart, sent to the console's TMS9918A.
//
// The kernel runs the VDP in Graphics II as a bitmap: name i is tile i, so
// every 8x1 run of pixels (a "segment") has its own pattern byte and colour
// byte, and the screen is a 256x192 picture with at most two colours in any
// segment. The cart draws into a canvas (a colour per pixel), encodes the
// tiles it touched, and sends the console only the VRAM bytes that differ
// from what it already has, as a write list the kernel runs:
//
//   n (1-127), address low, address high | $40, n bytes     copy
//   $80 + n (n 1-126), address low, address high | $40, v    fill n bytes with v
//   0, value, $80 | register                                 VDP register
//   $FF                                                      end
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VDP_W       256
#define VDP_H       192
#define VRAM_PAT    0x0000          // pattern table: tile i's 8 rows at i * 8
#define VRAM_COL    0x2000          // colour table, the same layout
#define VRAM_NAME   0x3800          // names: 0-255 in each third (the kernel sets them)
#define VRAM_SAT    0x3B00          // sprite attributes: $D0, no sprites (the kernel sets it)
#define VRAM_SIZE   0x4000

#define LIST_END    0xFF
#define LIST_COPY_MAX  64           // bytes a copy: the kernel polls the sound between commands
#define LIST_FILL_MAX  126

// The kernel leaves black (colour 0 shows the black backdrop); canvas colour
// 1 is black too, and is sent as 0.
enum { C_BLACK = 1, C_MGREEN, C_LGREEN, C_DBLUE, C_LBLUE, C_DRED, C_CYAN, C_MRED, C_LRED,
       C_DYELLOW, C_LYELLOW, C_DGREEN, C_MAGENTA, C_GRAY, C_WHITE };

typedef struct {
    uint8_t px[VDP_H][VDP_W];       // a colour (0-15) a pixel
    uint8_t dirty[VDP_H / 8][32];   // tiles drawn since they were last sent
} canvas_t;

typedef struct {
    uint8_t vram[VRAM_SIZE];        // the console's VRAM once every list made so far has run
    uint32_t bad_segments;          // segments drawn with more than two colours (a drawing bug)
} vdp_t;

// VRAM as the kernel leaves it at start: zero, the names, no sprites.
void vdp_init(vdp_t *v);

void canvas_init(canvas_t *c);      // black, all tiles dirty
void canvas_all_dirty(canvas_t *c);
// Drawing marks only tiles whose pixels change.
void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint8_t colour);
// Text in the 8x8 font (upper case; what it lacks shows as '?'), scaled 1x or
// more, set pixels only. Returns the x after it.
int canvas_text(canvas_t *c, int x, int y, const char *s, uint8_t colour, int scale);

// The VRAM writes that bring the console's screen to the canvas: appended to
// list (at most max bytes, the end mark included), and taken as done in
// v->vram. Tiles that didn't fit stay dirty for the next list (max must be 5
// or more). Returns the bytes used, 0 if nothing changed.
size_t vdp_list(vdp_t *v, canvas_t *c, uint8_t *list, size_t max);

// A list that writes VDP register reg (and nothing else) at list[0..3].
size_t vdp_list_reg(uint8_t *list, int reg, uint8_t value);

// What the kernel does with a list, for the tests: runs it on vram (regs may be NULL).
// Returns false on a malformed list.
bool vdp_run(uint8_t vram[VRAM_SIZE], uint8_t regs[8], const uint8_t *list, size_t len);

// The screen as the VDP shows it (colour indices, 0 = the backdrop), for tests and previews.
void vdp_picture(const uint8_t vram[VRAM_SIZE], uint8_t backdrop, uint8_t out[VDP_H][VDP_W]);

// The TMS9918A's colours as RGB, for previews.
extern const uint8_t vdp_rgb[16][3];
