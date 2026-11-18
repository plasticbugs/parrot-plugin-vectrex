// htrace.h - what the console did around a switch between sdplayer and PSG
// Synth, kept in RAM through chain_image and the watchdog reboot so that
// sdplayer can print it afterwards ('t' on its USB serial).
//
// Each side records its phase: core 1 the console's cart reads (settled,
// run-length: a run is one kind of read with no gap over 1 ms), core 0 its
// milestones (marks). Times are each firmware's own timer, so a phase's
// times compare only with that phase's.
//
//   S_HANDOFF  sdplayer, from COMMIT of PSG SYNTH until the chain
//   Y_BOOT     PSG Synth, from serving until the kernel runs (or RESET)
//   Y_RESET    PSG Synth, from RESET until the reboot into sdplayer
//   S_BOOT     sdplayer, its first 1.5 s after a boot
//
// 0x2007F000-0x2007FFFF: above every firmware's .bss (sdplayer's must end
// below 0x2007EC00, reserve.ld checks; PSG Synth's ends near 0x2004DB28),
// below scratch X; none uses the heap that would be there. A cold boot
// leaves garbage: the magic.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef HT_ADDR
#define HT_ADDR  0x2007F000u
#endif
#define HT_MAGIC 0x48543033u               // "HT03"
#define HT_EV    48
#define HT_MK    14
#define HT_GAP   1000                      // us: a silence this long starts a new run

enum { HT_S_HANDOFF, HT_Y_BOOT, HT_Y_RESET, HT_S_BOOT, HT_PHASES };
enum { HT_HDR = 1, HT_TITLE, HT_OTHER };   // a read of $8000-$8001, $8024-$807F, anything else
enum {                                     // marks
    HM_CHOOSE = 1, HM_ARM, HM_COMMIT, HM_CHAIN, HM_CHAIN_FAIL,   // sdplayer, S_HANDOFF
    HM_MAIN, HM_DRIVE, HM_UNDRIVE, HM_HDR_TITLE, HM_HDR_SKIP,    // either: MAIN val = reset cause
    HM_SERVE, HM_USBLOG, HM_RESET, HM_REBOOT,                    // PSG Synth (REBOOT val: 1 off, 2 parked, 3 gave up)
    HM_PARKED, HM_TITLE_PATH,                                    // either: the console parked; not, so AA 55
    HM_PAD_B, HM_PAD_EXIT,                                       // Movies: a pad's B or - seen; the kernel's exit made a jp (val 1: none to make)
    HM_USB,                                                      // the menu (chain) or Movies (going back): usbrole_packed()
    HM_BIOS,                                                     // the menu: the BIOS started over in the loader (RESET); val: how many
};

typedef struct { uint32_t t, t_end, n; uint16_t off; uint8_t kind, byte; } ht_ev_t;
typedef struct { uint32_t t, code, val; } ht_mk_t;
typedef struct {
    uint32_t nev, nmk, dropped, last_t;
    uint32_t last_kind;
    uint32_t nhdr, hdr_t, hdr_byte;        // header reads ($8000): how many, the last one's time and byte + 1
    ht_ev_t ev[HT_EV];
    ht_mk_t mk[HT_MK];
} ht_phase_t;
typedef struct { uint32_t magic, gen; ht_phase_t ph[HT_PHASES]; } htrace_t;
_Static_assert(sizeof(htrace_t) <= 0x1000, "htrace fits its 4K");

#define HTR ((htrace_t *)HT_ADDR)

static inline void ht_clear(int p) { memset(&HTR->ph[p], 0, sizeof(ht_phase_t)); }

// keep what's there if it's a trace; fresh = start a new one (gen + 1)
static inline void ht_init(bool fresh) {
    if (fresh || HTR->magic != HT_MAGIC) {
        uint32_t gen = HTR->magic == HT_MAGIC ? HTR->gen + 1 : 1;
        memset(HTR, 0, sizeof(htrace_t));
        HTR->magic = HT_MAGIC;
        HTR->gen = gen;
    }
}

// core 0
static inline void ht_mark(int p, uint32_t t, uint32_t code, uint32_t val) {
    ht_phase_t *ph = &HTR->ph[p];
    if (ph->nmk < HT_MK) {
        ph->mk[ph->nmk].t = t;
        ph->mk[ph->nmk].code = code;
        ph->mk[ph->nmk].val = val;
        ph->nmk++;
    }
}

// the console is parked in the handoff: this many reads of $8024-$8027 in a row
#define HT_PARKED 64
static inline bool ht_park_read(uint32_t off) { return off - 0x24 < 4; }

// cart offset from the address pins (GPIO 0-14): lo = GPIO 0-7, hi = 8-14
typedef struct { uint16_t lo[256], hi[128]; } ht_unperm_t;
static inline void ht_unperm_init(ht_unperm_t *u, const uint8_t addr_line[15]) {
    for (unsigned v = 0; v < 256; v++) {
        uint16_t a = 0, b = 0;
        for (unsigned g = 0; g < 8; g++) if (v >> g & 1) a |= (uint16_t)(1u << addr_line[g]);
        for (unsigned g = 0; g < 7; g++) if (v >> g & 1) b |= (uint16_t)(1u << addr_line[8 + g]);
        u->lo[v] = a;
        if (v < 128) u->hi[v] = b;
    }
}
static inline uint32_t ht_off(const ht_unperm_t *u, uint32_t pins) {
    return u->lo[pins & 0xFF] | u->hi[pins >> 8 & 0x7F];
}

// core 1: a read of $8000. The BIOS reads $8001 0.84 us after it starts,
// too soon for a record (~0.5 us): only a count and the last one.
static inline void ht_hdr(ht_phase_t *ph, uint32_t t, uint8_t byte) {
    ph->nhdr++;
    ph->hdr_t = t;
    ph->hdr_byte = byte + 1u;
}

// core 1: any other settled cart read (byte = what was served, in pin order)
static inline uint32_t ht_kind(uint32_t off) {
    return off <= 1 ? HT_HDR : off >= 0x24 && off < 0x80 ? HT_TITLE : HT_OTHER;
}
static inline void ht_bus(ht_phase_t *ph, uint32_t t, uint32_t off, uint8_t byte) {
    uint32_t kind = ht_kind(off);
    if (ph->nev && kind == ph->last_kind && kind != HT_HDR && t - ph->last_t < HT_GAP) {
        ph->ev[ph->nev - 1].n++;
        ph->ev[ph->nev - 1].t_end = t;
    } else if (ph->nev < HT_EV) {
        ht_ev_t *e = &ph->ev[ph->nev++];
        e->t = e->t_end = t;
        e->n = 1;
        e->off = (uint16_t)off;
        e->kind = (uint8_t)kind;
        e->byte = byte;
    } else ph->dropped++;
    ph->last_kind = kind;
    ph->last_t = t;
}
