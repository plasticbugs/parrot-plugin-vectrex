// m6809.h -- a Motorola 6809 for the Star Wars plugin. The core itself is
// m6809_core.h, a template: each CPU (the game's and the sound board's)
// includes it once with its own memory macros, so the bus decode is inlined
// into the instruction loop.
//
// Written for this plugin; MAME's m6809 (BSD-3-Clause, src/devices/cpu/m6809)
// was the reference for the flags, the cycle counts and the interrupt
// entry, and the test (test/trace.sh) compares it with MAME's, instruction
// by instruction.
#pragma once
#include <stdint.h>

enum { CC_C = 1, CC_V = 2, CC_Z = 4, CC_N = 8, CC_I = 0x10, CC_H = 0x20, CC_F = 0x40, CC_E = 0x80 };

typedef struct {
    uint16_t pc, x, y, u, s;
    uint8_t a, b, dp, cc;
    union {
        struct {
            uint8_t irq, firq, nmi; // the lines (nmi: an edge waiting)
            uint8_t wait;           // 1: SYNC, 2: CWAI (the state is stacked)
        };
        uint32_t pending;           // any of them
    };
    uint8_t wofs;                   // the last STA / STB's write: cycles into its instruction
    int32_t icount;                 // cycles left in this run
    uint64_t end;                   // the cycle count this run ends at
} m6809_t;

// cycles run since reset, to the start of the instruction now running
static inline uint64_t m6809_now(const m6809_t *c) { return c->end - (uint64_t)(int64_t)c->icount; }
// end the run after this instruction (a handler's: the other CPU's turn)
static inline void m6809_stop(m6809_t *c) {
    if (c->icount > 0) {
        c->end -= (uint64_t)c->icount;
        c->icount = 0;
    }
}

// N and Z for a byte (the core's loads, stores and logic)
extern const uint8_t m6809_nz[256];
