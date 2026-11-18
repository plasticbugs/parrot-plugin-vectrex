// vx.h -- the Vectrex (GCE, 1982): a 6809 at 1.5 MHz, a 6522 VIA, the DAC
// with its sample-and-holds and the two integrators that move the beam, an
// AY-3-8912, 1K of RAM, the 8K system ROM and a cartridge. Portable C: the
// plugin and the Mac tests share it.
//
// The 6809 is the Star Wars plugin's (../starwars/m6809_core.h), with every
// bus cycle counted so a VIA access knows its cycle. The VIA, the analog
// side and the cartridge are ports of MAME 0.288's (BSD-3-Clause):
// src/mame/miltonbradley/vectrex.cpp, vectrex_m.cpp, vectrex_v.cpp (Mathis
// Rosenhauer; thanks there to Christopher Salomon and Bruce Tomlin),
// src/devices/machine/6522via.cpp (Peter Trauner, Mathis Rosenhauer),
// src/devices/bus/vectrex (Fabio Priuli).
//
// No ROM is in here: the system ROM and the cartridge are the user's own,
// put in their places (vx_rom(), vx_cart()) before vx_init().
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VX_ROM_SIZE   0x2000        // the system ROM, $E000-$FFFF
#define VX_CART_MAX   0x10000       // a cartridge: 32K at $0000, or two banks of it chosen by PB6
#define VX_CPU_HZ     1500000       // 6 MHz / 4
// a TV frame (the console's 59.9227 a second) of the Vectrex's clock, << 8
#define VX_FRAME_CYCLES_Q8 ((uint32_t)((uint64_t)VX_CPU_HZ * 256 * 10000 / 599227))

uint8_t *vx_rom(void);              // where the system ROM's 8K goes
uint8_t *vx_cart(void);             // where a cartridge's bytes go (VX_CART_MAX)
// the pictures' room, free until vx_init(): the plugin's messages are drawn there
#define VX_SCRATCH    50000
uint8_t *vx_scratch(void);
// once those are in place; cart_size 0: no cartridge (the ROM's own game).
// seed: two bytes for the RAM the ROM's game takes its random numbers from
// (MAME does the same: Mine Storm's mines aren't scattered with a seed of 0)
void vx_init(uint32_t cart_size, uint16_t seed);
void vx_reset(void);

// the two controllers: the sticks -128..127 (x right, y up), and the eight
// buttons, 1 = pressed: bits 0-3 player 1's buttons 1-4, bits 4-7 player 2's
typedef struct {
    int8_t x[2], y[2];
    uint8_t buttons;
} vx_inputs_t;
void vx_inputs(const vx_inputs_t *in);

void vx_run(uint32_t cycles);       // cycles of the 1.5 MHz clock

// A vector as the beam drew it, in 1/64 of MAME's screen units from the
// screen's middle, x right and y up. The integrators move 550/65536 of a
// unit a cycle for each step of the DAC, so the ROM's full-size square
// (127 at scale $7F) reaches +-135 units; the screen shows about +-140
// across and +-175 up and down. A dot is a vector of no length.
typedef struct {
    int16_t x0, y0, x1, y1;
    uint8_t z;                      // brightness, 1-254 (twice the Z level)
    uint8_t text;                   // 1: part of raster text, which vx_text() has as characters
} vx_vec_t;
#define VX_VEC_MAX 4000         // (the most seen in 236 games and demos: 3,395)
#define VX_UNIT    64

// Raster text. The system ROM writes text as a television would: seven
// sweeps a line, the beam switched by the VIA's shift register, eight bits
// a character. On the tube a dot is a fraction of a millimetre; on a
// picture 160 pixels wide two dots fall in one pixel and nothing can be
// read. So each byte shifted out in such a sweep is also kept as what it
// is, a row of a character, for the picture's maker to draw a dot a pixel
// (vxcv.c). Three or more sweeps under one another are text; fewer (a
// dashed line) stay vectors only.
typedef struct {
    int16_t x, y;                   // where its first bit starts, in 1/64 units
    int16_t pitch;                  // a bit's width, in 1/1024 units
    uint16_t block;                 // the lines of one piece of text share this
    uint8_t row, col;               // the sweep within it (0 the first), the character along it
    uint8_t bits;                   // the byte: bit 7 first
    uint8_t z;
} vx_text_t;
#define VX_TEXT_MAX 1400

// The picture: a count that goes up when a refresh is complete (the game
// started the VIA's timer 2 again), and that refresh's vectors, valid until
// the next vx_run().
uint32_t vx_picture(const vx_vec_t **v, int *n);
void vx_text(const vx_text_t **t, int *n);      // that picture's raster text

// the AY-3-8912 as it is now: its registers, and its envelope's level (0-15)
typedef struct {
    uint8_t reg[16];
    uint8_t env;
} vx_ay_t;
void vx_ay(vx_ay_t *ay);

// The DAC's sound channel (samples, speech): the levels since the last
// call, VX_SAMPLE_DIV cycles apart, with the resting level taken off,
// -32768..32767 (at most max; the newest are kept). *playing: the level
// moved often enough that something is being played.
#define VX_SAMPLE_DIV 180           // 8,333 a second: about 139 a TV frame
#define VX_SAMPLE_MAX 512
int vx_samples(int16_t *out, int max, bool *playing);

// for the tests and the note
typedef struct {
    uint64_t cycles;
    uint16_t pc;
    uint32_t via_reads, via_writes, events, irqs, t2_starts, points, dropped, dac_writes, bank_switches;
} vx_stats_t;
void vx_stats(vx_stats_t *s);
uint8_t vx_peek(uint16_t a);        // RAM, ROM and cartridge (not the VIA)
