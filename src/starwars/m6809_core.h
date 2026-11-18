// m6809_core.h -- the 6809 itself, as a template. Before including it:
//
//   #define M6809_NAME  main6809            the functions' prefix
//   #define M6809_RD(a)     ...             a byte read (a is a uint16_t)
//   #define M6809_WR(a, v)  ...             a byte write
//   #define M6809_FETCH(a)  ...             optional: a byte of the program (an
//                                           opcode or what follows it), where
//                                           that is quicker than M6809_RD
//   #define M6809_DUMMY(a)  ...             optional: a bus cycle that reads
//                                           address a for nothing (the byte
//                                           after an instruction, or $FFFF
//                                           while the CPU works: MAME's
//                                           dummy_read_opcode_arg and
//                                           dummy_vma, in MAME's places), for
//                                           a bus with something watching it
//   #define M6809_DUMMY_VMA(n) ...          with it: n such cycles at $FFFF
//   #define M6809_SLOW_STEP(c) ...          optional, with M6809_FETCH_BASE: an
//                                           instruction whose fbase is 0 isn't
//                                           run here but by this (c->pc in and
//                                           out; it keeps c's clock itself)
//   #define M6809_NO_TRACE                  no M6809_TRACE from this copy
//   #define M6809_FETCH_BASE(pc) ...        optional, with it: a uintptr_t found
//                                           once an instruction (fbase), for
//                                           M6809_FETCH to use
//
// It defines  static void NAME_reset(m6809_t *c)  and
//             static void NAME_run(m6809_t *c, int cycles).
// In the run loop the PC is a local (pc), in c->pc only around the calls that
// want it there; the time is m6809_now(c). The interrupt lines are looked at
// when a run starts and after an instruction that changes the masks: a
// handler that raises a line calls m6809_stop(), so the run that follows
// sees it at the same instruction boundary.
// Undocumented opcodes are 2-cycle no-ops.
#include "m6809.h"

#define M_CAT2(a, b) a##b
#define M_CAT(a, b) M_CAT2(a, b)
#define M_FN(n) M_CAT(M6809_NAME, n)

#define RD(a)       ((uint8_t)M6809_RD((uint16_t)(a)))
#ifndef M6809_FETCH
#define M6809_FETCH(a) M6809_RD(a)
#endif
#ifndef M6809_FETCH_BASE
#define M6809_FETCH_BASE(pc) 0
#endif
#define FETCH(a)    ((uint8_t)M6809_FETCH((uint16_t)(a)))
// the dummy cycles: DA(d) reads the byte d past the PC, DV(n) is n cycles at
// $FFFF, DM(a) reads a
#ifdef M6809_DUMMY
#define DM(a)       M6809_DUMMY((uint16_t)(a))
#define DA(d)       M6809_DUMMY((uint16_t)(pc + (d)))
#define DV(n)       M6809_DUMMY_VMA(n)
#else
#define DM(a)
#define DA(d)
#define DV(n)
#endif
#define WR(a, v)    M6809_WR((uint16_t)(a), (uint8_t)(v))

static inline uint16_t M_FN(_rd16)(m6809_t *c, uint16_t a) {
    (void)c;
    uint8_t h = RD(a);
    return (uint16_t)(h << 8 | RD(a + 1));
}
static inline void M_FN(_wr16)(m6809_t *c, uint16_t a, uint16_t v) {
    (void)c;
    WR(a, v >> 8);                  // the high byte first (the divider's start depends on it)
    WR(a + 1, v);
}
#define RD16(a)     M_FN(_rd16)(c, (uint16_t)(a))
#define WR16(a, v)  M_FN(_wr16)(c, (uint16_t)(a), (uint16_t)(v))
#define IMM8()      FETCH(pc++)
#define IMM16()     (pc += 2, (uint16_t)(FETCH(pc - 2) << 8 | FETCH(pc - 1)))
#define D()         ((uint16_t)(c->a << 8 | c->b))
#define SET_D(v)    do { uint16_t d_ = (uint16_t)(v); c->a = (uint8_t)(d_ >> 8); c->b = (uint8_t)d_; } while (0)

#define PUSHS8(v)   WR(--c->s, v)
#define PUSHS16(v)  do { uint16_t p_ = (uint16_t)(v); WR(--c->s, p_); WR(--c->s, p_ >> 8); } while (0)
#define PULLS8()    RD(c->s++)
#define PUSHU8(v)   WR(--c->u, v)
#define PUSHU16(v)  do { uint16_t p_ = (uint16_t)(v); WR(--c->u, p_); WR(--c->u, p_ >> 8); } while (0)
#define PULLU8()    RD(c->u++)

static inline uint16_t M_FN(_pulls16)(m6809_t *c) { uint16_t v = RD16(c->s); c->s += 2; return v; }
static inline uint16_t M_FN(_pullu16)(m6809_t *c) { uint16_t v = RD16(c->u); c->u += 2; return v; }
#define PULLS16()   M_FN(_pulls16)(c)
#define PULLU16()   M_FN(_pullu16)(c)

// ---- flags ---------------------------------------------------------------------
static inline void M_FN(_nz8)(m6809_t *c, uint8_t r) {      // N Z, V = 0
    c->cc = (uint8_t)((c->cc & ~(CC_N | CC_Z | CC_V)) | m6809_nz[r]);
}
static inline void M_FN(_nz16)(m6809_t *c, uint16_t r) {
    c->cc = (uint8_t)((c->cc & ~(CC_N | CC_Z | CC_V)) | (r & 0x8000 ? CC_N : 0) | (r ? 0 : CC_Z));
}
static inline uint8_t M_FN(_add8)(m6809_t *c, uint8_t a, uint8_t b, unsigned cin) {
    unsigned r = a + b + cin;
    uint8_t cc = c->cc & ~(CC_H | CC_N | CC_Z | CC_V | CC_C);
    if ((a ^ b ^ r) & 0x10) cc |= CC_H;
    if (r & 0x80) cc |= CC_N;
    if (!(r & 0xFF)) cc |= CC_Z;
    if ((a ^ r) & (b ^ r) & 0x80) cc |= CC_V;
    if (r & 0x100) cc |= CC_C;
    c->cc = cc;
    return (uint8_t)r;
}
static inline uint8_t M_FN(_sub8)(m6809_t *c, uint8_t a, uint8_t b, unsigned cin) {
    unsigned r = a - b - cin;
    uint8_t cc = c->cc & ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x80) cc |= CC_N;
    if (!(r & 0xFF)) cc |= CC_Z;
    if ((a ^ b) & (a ^ r) & 0x80) cc |= CC_V;
    if (r & 0x100) cc |= CC_C;
    c->cc = cc;
    return (uint8_t)r;
}
static inline uint16_t M_FN(_add16)(m6809_t *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a + b;
    uint8_t cc = c->cc & ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x8000) cc |= CC_N;
    if (!(r & 0xFFFF)) cc |= CC_Z;
    if ((a ^ r) & (b ^ r) & 0x8000) cc |= CC_V;
    if (r & 0x10000) cc |= CC_C;
    c->cc = cc;
    return (uint16_t)r;
}
static inline uint16_t M_FN(_sub16)(m6809_t *c, uint16_t a, uint16_t b) {
    uint32_t r = (uint32_t)a - b;
    uint8_t cc = c->cc & ~(CC_N | CC_Z | CC_V | CC_C);
    if (r & 0x8000) cc |= CC_N;
    if (!(r & 0xFFFF)) cc |= CC_Z;
    if ((a ^ b) & (a ^ r) & 0x8000) cc |= CC_V;
    if (r & 0x10000) cc |= CC_C;
    c->cc = cc;
    return (uint16_t)r;
}
// the read-modify-write group ($00-$0F, $40-$7F), by the opcode's low nibble
static inline uint8_t M_FN(_rmw)(m6809_t *c, unsigned op, uint8_t m) {
    uint8_t r, cc = c->cc;
    switch (op & 15) {
    case 0x0: case 0x1:                 // NEG
        return M_FN(_sub8)(c, 0, m, 0);
    case 0x3: case 0x2:                 // COM
        r = (uint8_t)~m;
        M_FN(_nz8)(c, r);
        c->cc |= CC_C;
        return r;
    case 0x4: case 0x5:                 // LSR
        r = m >> 1;
        cc = (uint8_t)((cc & ~(CC_N | CC_Z | CC_C)) | (m & 1) | (r ? 0 : CC_Z));
        c->cc = cc;
        return r;
    case 0x6:                           // ROR
        r = (uint8_t)(m >> 1 | (cc & CC_C) << 7);
        cc = (uint8_t)((cc & ~(CC_N | CC_Z | CC_C)) | (m & 1) | (r ? 0 : CC_Z) | (r & 0x80 ? CC_N : 0));
        c->cc = cc;
        return r;
    case 0x7:                           // ASR
        r = (uint8_t)(m >> 1 | (m & 0x80));
        cc = (uint8_t)((cc & ~(CC_N | CC_Z | CC_C)) | (m & 1) | (r ? 0 : CC_Z) | (r & 0x80 ? CC_N : 0));
        c->cc = cc;
        return r;
    case 0x8:                           // ASL (H stays, as MAME has it)
        r = M_FN(_add8)(c, m, m, 0);
        c->cc = (uint8_t)((c->cc & ~CC_H) | (cc & CC_H));
        return r;
    case 0x9:                           // ROL
        r = M_FN(_add8)(c, m, m, cc & CC_C);
        c->cc = (uint8_t)((c->cc & ~CC_H) | (cc & CC_H));
        return r;
    case 0xA: case 0xB:                 // DEC
        r = (uint8_t)(m - 1);
        cc = (uint8_t)((cc & ~(CC_N | CC_Z | CC_V)) | (r ? 0 : CC_Z) | (r & 0x80 ? CC_N : 0) | (m == 0x80 ? CC_V : 0));
        c->cc = cc;
        return r;
    case 0xC:                           // INC
        r = (uint8_t)(m + 1);
        cc = (uint8_t)((cc & ~(CC_N | CC_Z | CC_V)) | (r ? 0 : CC_Z) | (r & 0x80 ? CC_N : 0) | (m == 0x7F ? CC_V : 0));
        c->cc = cc;
        return r;
    case 0xD:                           // TST
        M_FN(_nz8)(c, m);
        return m;
    default:                            // CLR ($F; $E is JMP, handled before)
        c->cc = (uint8_t)((cc & ~(CC_N | CC_V | CC_C)) | CC_Z);
        return 0;
    }
}

// ---- indexed addressing: the effective address, with the bytes it took after
// the postbyte (<< 16) and its extra cycles (<< 24) -----------------------------------
static uint32_t M6809_HOT M_FN(_indexed)(m6809_t *c, uint16_t pc) {
    uintptr_t fbase = M6809_FETCH_BASE(pc);
    (void)fbase;
    uint8_t pb = FETCH(pc++);
    uint16_t *r;
    switch (pb >> 5 & 3) {
    case 0: r = &c->x; break;
    case 1: r = &c->y; break;
    case 2: r = &c->u; break;
    default: r = &c->s; break;
    }
    if (!(pb & 0x80)) {                 // 5-bit offset
        DA(0); DV(1);
        return (uint16_t)(*r + ((pb & 0x10) ? (int)(pb & 0x1F) - 32 : (pb & 0x1F))) | 1u << 24;
    }
    uint16_t ea;
    uint32_t cyc = 0, bytes = 0;
    switch (pb & 15) {
    case 0x0: ea = *r; *r += 1; cyc = 2; DA(0); DV(2); break;
    case 0x1: ea = *r; *r += 2; cyc = 3; DA(0); DV(3); break;
    case 0x2: *r -= 1; ea = *r; cyc = 2; DA(0); DV(2); break;
    case 0x3: *r -= 2; ea = *r; cyc = 3; DA(0); DV(3); break;
    case 0x4: ea = *r; DA(0); break;
    case 0x5: ea = (uint16_t)(*r + (int8_t)c->b); cyc = 1; DA(0); DV(1); break;
    case 0x6: ea = (uint16_t)(*r + (int8_t)c->a); cyc = 1; DA(0); DV(1); break;
    case 0x8: ea = (uint16_t)(*r + (int8_t)FETCH(pc)); bytes = 1; cyc = 1; DA(1); break;
    case 0x9: ea = (uint16_t)(*r + (FETCH(pc) << 8 | FETCH(pc + 1))); bytes = 2; cyc = 4; DV(3); break;
    case 0xB: ea = (uint16_t)(*r + D()); cyc = 4; DA(0); DA(1); DV(3); break;
    case 0xC: ea = (uint16_t)(pc + 1 + (int8_t)FETCH(pc)); bytes = 1; cyc = 1; DV(1); break;
    case 0xD: ea = (uint16_t)(pc + 2 + (FETCH(pc) << 8 | FETCH(pc + 1))); bytes = 2; cyc = 5; DV(4); break;
    case 0xF: ea = (uint16_t)(FETCH(pc) << 8 | FETCH(pc + 1)); bytes = 2; cyc = 2; DV(1); break;
    default:  ea = *r; break;           // (undefined)
    }
    if (pb & 0x10) {                    // indirect
        ea = RD16(ea);
        cyc += 3;
        DV(1);
    }
    return ea | bytes << 16 | cyc << 24;
}
#define EA_IDX()  (idx = M_FN(_indexed)(c, pc), pc = (uint16_t)(pc + 1 + (idx >> 16 & 3)), cyc += (int)(idx >> 24), (uint16_t)idx)
#ifdef M6809_DUMMY
#define EA_DIR()  ({ uint16_t e_ = (uint16_t)(c->dp << 8 | IMM8()); DV(1); e_; })
#define EA_EXT()  ({ uint16_t e_ = IMM16(); DV(1); e_; })
#else
#define EA_DIR()  ((uint16_t)(c->dp << 8 | IMM8()))
#define EA_EXT()  IMM16()
#endif

// ---- TFR / EXG -------------------------------------------------------------------
static inline uint16_t M_FN(_getreg)(m6809_t *c, unsigned r) {
    switch (r) {
    case 0: return D();
    case 1: return c->x;
    case 2: return c->y;
    case 3: return c->u;
    case 4: return c->s;
    case 5: return c->pc;
    case 8: return (uint16_t)(0xFF00 | c->a);
    case 9: return (uint16_t)(0xFF00 | c->b);
    case 10: return (uint16_t)(c->cc << 8 | c->cc);
    case 11: return (uint16_t)(c->dp << 8 | c->dp);
    default: return 0xFFFF;
    }
}
static inline void M_FN(_setreg)(m6809_t *c, unsigned r, uint16_t v) {
    switch (r) {
    case 0: SET_D(v); break;
    case 1: c->x = v; break;
    case 2: c->y = v; break;
    case 3: c->u = v; break;
    case 4: c->s = v; break;
    case 5: c->pc = v; break;
    case 8: c->a = (uint8_t)v; break;
    case 9: c->b = (uint8_t)v; break;
    case 10: c->cc = (uint8_t)v; break;
    case 11: c->dp = (uint8_t)v; break;
    default: break;
    }
}

// ---- stacking --------------------------------------------------------------------
static inline void M_FN(_push_all)(m6809_t *c) {
    PUSHS16(c->pc); PUSHS16(c->u); PUSHS16(c->y); PUSHS16(c->x);
    PUSHS8(c->dp); PUSHS8(c->b); PUSHS8(c->a); PUSHS8(c->cc);
}

static void __attribute__((unused)) M_FN(_reset)(m6809_t *c) {
    c->dp = 0;
    c->cc = CC_I | CC_F;
    c->pending = 0;
    c->pc = RD16(0xFFFE);
    c->end += 4;                        // (MAME's count for the reset's vector fetch)
}

// the 8-bit operations of $80-$FF, in their four addressing modes
#define ALU8(opc, EXPR) \
    case opc:        m = IMM8(); EXPR; cyc += 2; break; \
    case opc + 0x10: ea = EA_DIR(); m = RD(ea); EXPR; cyc += 4; break; \
    case opc + 0x20: ea = EA_IDX(); m = RD(ea); EXPR; cyc += 4; break; \
    case opc + 0x30: ea = EA_EXT(); m = RD(ea); EXPR; cyc += 5; break;
#define ALU16(opc, EXPR, ci, cd, cx, ce) \
    case opc:        w = IMM16(); EXPR; cyc += ci; break; \
    case opc + 0x10: ea = EA_DIR(); w = RD16(ea); EXPR; cyc += cd; break; \
    case opc + 0x20: ea = EA_IDX(); w = RD16(ea); EXPR; cyc += cx; break; \
    case opc + 0x30: ea = EA_EXT(); w = RD16(ea); EXPR; cyc += ce; break;
// (wofs: the write is in the instruction's last cycle; a handler that keeps time adds it)
#define ST8(opc, REG) \
    case opc + 0x10: ea = EA_DIR(); M_FN(_nz8)(c, REG); c->wofs = 3; WR(ea, REG); cyc += 4; break; \
    case opc + 0x20: ea = EA_IDX(); M_FN(_nz8)(c, REG); c->wofs = (uint8_t)(cyc + 3); WR(ea, REG); cyc += 4; break; \
    case opc + 0x30: ea = EA_EXT(); M_FN(_nz8)(c, REG); c->wofs = 4; WR(ea, REG); cyc += 5; break;
#define ST16(opc, VAL, cd, cx, ce) \
    case opc + 0x10: ea = EA_DIR(); w = (VAL); M_FN(_nz16)(c, w); WR16(ea, w); cyc += cd; break; \
    case opc + 0x20: ea = EA_IDX(); w = (VAL); M_FN(_nz16)(c, w); WR16(ea, w); cyc += cx; break; \
    case opc + 0x30: ea = EA_EXT(); w = (VAL); M_FN(_nz16)(c, w); WR16(ea, w); cyc += ce; break;
#define BRANCH(cond) do { int8_t o_ = (int8_t)IMM8(); DV(1); if (cond) pc = (uint16_t)(pc + o_); cyc += 3; } while (0)
#define LBRANCH(cond) do { uint16_t o_ = IMM16(); DV(1); cyc += 5; if (cond) { pc = (uint16_t)(pc + o_); cyc += 1; DV(1); } } while (0)
#define F_C (c->cc & CC_C)
#define F_V (c->cc & CC_V)
#define F_Z (c->cc & CC_Z)
#define F_N (c->cc & CC_N)
#define F_NXV (((c->cc >> 3) ^ (c->cc >> 1)) & 1)

static inline bool M_FN(_cond)(m6809_t *c, unsigned n) {
    switch (n & 15) {
    case 0x0: return true;
    case 0x1: return false;
    case 0x2: return !(F_C || F_Z);
    case 0x3: return F_C || F_Z;
    case 0x4: return !F_C;
    case 0x5: return F_C;
    case 0x6: return !F_Z;
    case 0x7: return F_Z;
    case 0x8: return !F_V;
    case 0x9: return F_V;
    case 0xA: return !F_N;
    case 0xB: return F_N;
    case 0xC: return !F_NXV;
    case 0xD: return F_NXV;
    case 0xE: return !(F_NXV || F_Z);
    default:  return F_NXV || F_Z;
    }
}

static int M_FN(_psh)(m6809_t *c, uint8_t pb, bool s_stack) {
    int n = 0;
    if (s_stack) {
        if (pb & 0x80) { PUSHS16(c->pc); n += 2; }
        if (pb & 0x40) { PUSHS16(c->u); n += 2; }
        if (pb & 0x20) { PUSHS16(c->y); n += 2; }
        if (pb & 0x10) { PUSHS16(c->x); n += 2; }
        if (pb & 0x08) { PUSHS8(c->dp); n++; }
        if (pb & 0x04) { PUSHS8(c->b); n++; }
        if (pb & 0x02) { PUSHS8(c->a); n++; }
        if (pb & 0x01) { PUSHS8(c->cc); n++; }
    } else {
        if (pb & 0x80) { PUSHU16(c->pc); n += 2; }
        if (pb & 0x40) { PUSHU16(c->s); n += 2; }
        if (pb & 0x20) { PUSHU16(c->y); n += 2; }
        if (pb & 0x10) { PUSHU16(c->x); n += 2; }
        if (pb & 0x08) { PUSHU8(c->dp); n++; }
        if (pb & 0x04) { PUSHU8(c->b); n++; }
        if (pb & 0x02) { PUSHU8(c->a); n++; }
        if (pb & 0x01) { PUSHU8(c->cc); n++; }
    }
    return n;
}
static int M_FN(_pul)(m6809_t *c, uint8_t pb, bool s_stack) {
    int n = 0;
    if (s_stack) {
        if (pb & 0x01) { c->cc = PULLS8(); n++; }
        if (pb & 0x02) { c->a = PULLS8(); n++; }
        if (pb & 0x04) { c->b = PULLS8(); n++; }
        if (pb & 0x08) { c->dp = PULLS8(); n++; }
        if (pb & 0x10) { c->x = PULLS16(); n += 2; }
        if (pb & 0x20) { c->y = PULLS16(); n += 2; }
        if (pb & 0x40) { c->u = PULLS16(); n += 2; }
        if (pb & 0x80) { c->pc = PULLS16(); n += 2; }
    } else {
        if (pb & 0x01) { c->cc = PULLU8(); n++; }
        if (pb & 0x02) { c->a = PULLU8(); n++; }
        if (pb & 0x04) { c->b = PULLU8(); n++; }
        if (pb & 0x08) { c->dp = PULLU8(); n++; }
        if (pb & 0x10) { c->x = PULLU16(); n += 2; }
        if (pb & 0x20) { c->y = PULLU16(); n += 2; }
        if (pb & 0x40) { c->s = PULLU16(); n += 2; }
        if (pb & 0x80) { c->pc = PULLU16(); n += 2; }
    }
    return n;
}

// interrupts taken between instructions; returns the cycles used (0: none)
static inline int M_FN(_interrupts)(m6809_t *c) {
#ifdef M6809_DUMMY
    uint16_t pc = c->pc;
    uintptr_t fbase = M6809_FETCH_BASE(pc);
    (void)fbase;
#endif
    if (c->nmi) {
        c->nmi = 0;
        if (c->wait != 2) { DA(0); DA(0); DV(1); c->cc |= CC_E; M_FN(_push_all)(c); }
        c->wait = 0;
        c->cc |= CC_I | CC_F;
        DV(1);
        c->pc = RD16(0xFFFC);
        DV(1);
        return 19;
    }
    if (c->firq && !(c->cc & CC_F)) {
        if (c->wait != 2) { DA(0); DA(0); DV(1); c->cc &= (uint8_t)~CC_E; PUSHS16(c->pc); PUSHS8(c->cc); }
        c->wait = 0;
        c->cc |= CC_I | CC_F;
        DV(1);
        c->pc = RD16(0xFFF6);
        DV(1);
        return 10;
    }
    if (c->irq && !(c->cc & CC_I)) {
        if (c->wait != 2) { DA(0); DA(0); DV(1); c->cc |= CC_E; M_FN(_push_all)(c); }
        c->wait = 0;
        c->cc |= CC_I;
        DV(1);
        c->pc = RD16(0xFFF8);
        DV(1);
        return 19;
    }
    if (c->wait == 1 && (c->irq || c->firq)) c->wait = 0;     // SYNC ends on a masked one too
    return 0;
}

static void M6809_HOT M_FN(_run)(m6809_t *c, int cycles) {
    c->end = m6809_now(c) + (uint64_t)(int64_t)cycles;
    c->icount = cycles;
    uint16_t pc = c->pc;
    while (c->icount > 0) {
        if (c->pending) {
            c->pc = pc;
            int took = M_FN(_interrupts)(c);
            pc = c->pc;
            if (took) { c->icount -= took; continue; }
            if (c->wait) {              // nothing to do until a line moves
                c->icount = 0;
                break;
            }
        }
        bool recheck = false;           // the masks changed: the lines again
        do {
        int cyc = 0;
        uint16_t ea, w;
        uint32_t idx;
        uint8_t m;
#if defined(M6809_TRACE) && !defined(M6809_NO_TRACE)
        c->pc = pc;
        M6809_TRACE(c);                 // (tests: the registers before each instruction)
#endif
        uintptr_t fbase = M6809_FETCH_BASE(pc);
        (void)fbase;
#ifdef M6809_SLOW_STEP
        if (__builtin_expect(!fbase, 0)) {
            c->pc = pc;
            M6809_SLOW_STEP(c);
            pc = c->pc;
            recheck = true;
            continue;
        }
#endif
        unsigned op = IMM8();
        switch (op) {
        // ---- $00-$0F direct, $60-$6F indexed, $70-$7F extended: read-modify-write
        case 0x00: case 0x03: case 0x04: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0C: case 0x0F:
        case 0x01: case 0x02: case 0x05: case 0x0B:
            ea = EA_DIR(); m = M_FN(_rmw)(c, op, RD(ea)); DA(0); WR(ea, m); cyc += 6; break;
        case 0x0D: ea = EA_DIR(); M_FN(_nz8)(c, RD(ea)); DV(2); cyc += 6; break;
        case 0x0E: ea = EA_DIR(); pc = ea; cyc += 3; break;
        case 0x60: case 0x63: case 0x64: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6C: case 0x6F:
        case 0x61: case 0x62: case 0x65: case 0x6B:
            ea = EA_IDX(); m = M_FN(_rmw)(c, op, RD(ea)); DA(0); WR(ea, m); cyc += 6; break;
        case 0x6D: ea = EA_IDX(); M_FN(_nz8)(c, RD(ea)); DV(2); cyc += 6; break;
        case 0x6E: ea = EA_IDX(); pc = ea; cyc += 3; break;
        case 0x70: case 0x73: case 0x74: case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7C: case 0x7F:
        case 0x71: case 0x72: case 0x75: case 0x7B:
            ea = EA_EXT(); m = M_FN(_rmw)(c, op, RD(ea)); DA(0); WR(ea, m); cyc += 7; break;
        case 0x7D: ea = EA_EXT(); M_FN(_nz8)(c, RD(ea)); DV(2); cyc += 7; break;
        case 0x7E: ea = EA_EXT(); pc = ea; cyc += 4; break;
        // ---- $40-$5F: the same on A and B
        case 0x40: case 0x43: case 0x44: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4C: case 0x4D: case 0x4F:
        case 0x41: case 0x42: case 0x45: case 0x4B: case 0x4E:
            c->a = M_FN(_rmw)(c, op, c->a); if ((op & 15) == 0xD) { DV(1); } else { DA(0); } cyc += 2; break;
        case 0x50: case 0x53: case 0x54: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5C: case 0x5D: case 0x5F:
        case 0x51: case 0x52: case 0x55: case 0x5B: case 0x5E:
            c->b = M_FN(_rmw)(c, op, c->b); if ((op & 15) == 0xD) { DV(1); } else { DA(0); } cyc += 2; break;

        // ---- $10-$1F
        case 0x12: DA(0); cyc += 2; break;                          // NOP
        case 0x13: DA(0); c->wait = 1; recheck = true; cyc += 4; break;     // SYNC
        case 0x16: { uint16_t o = IMM16(); DV(2); pc = (uint16_t)(pc + o); cyc += 5; break; }      // LBRA
        case 0x17: { uint16_t o = IMM16(); DV(4); PUSHS16(pc); pc = (uint16_t)(pc + o); cyc += 9; break; }    // LBSR
        case 0x19: {                                                // DAA
            unsigned a = c->a, cf = 0;
            if ((a & 0x0F) > 9 || (c->cc & CC_H)) cf |= 0x06;
            if (a > 0x99 || (c->cc & CC_C) || ((a & 0xF0) > 0x80 && (a & 0x0F) > 9)) cf |= 0x60;
            unsigned r = a + cf;
            uint8_t cc = c->cc & ~(CC_N | CC_Z | CC_V);
            if (r & 0x100 || (cf & 0x60)) cc |= CC_C;
            c->a = (uint8_t)r;
            if (c->a & 0x80) cc |= CC_N;
            if (!c->a) cc |= CC_Z;
            c->cc = cc;
            DA(0);
            cyc += 2;
            break;
        }
        case 0x1A: c->cc |= IMM8(); DA(0); cyc += 3; break;         // ORCC
        case 0x1C: c->cc &= IMM8(); DA(0); recheck = true; cyc += 3; break; // ANDCC
        case 0x1D: c->a = (c->b & 0x80) ? 0xFF : 0; M_FN(_nz16)(c, D()); DA(0); cyc += 2; break;      // SEX
        case 0x1E: {                                                // EXG
            uint8_t pb = IMM8();
            c->pc = pc;
            uint16_t t1 = M_FN(_getreg)(c, pb >> 4), t2 = M_FN(_getreg)(c, pb & 15);
            M_FN(_setreg)(c, pb >> 4, t2);
            M_FN(_setreg)(c, pb & 15, t1);
            pc = c->pc;
            DV(6);
            recheck = true;
            cyc += 8;
            break;
        }
        case 0x1F: {                                                // TFR
            uint8_t pb = IMM8();
            c->pc = pc;
            M_FN(_setreg)(c, pb & 15, M_FN(_getreg)(c, pb >> 4));
            pc = c->pc;
            DV(4);
            recheck = true;
            cyc += 6;
            break;
        }
        // ---- $20-$2F: short branches
        case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
        case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
            BRANCH(M_FN(_cond)(c, op)); break;
        // ---- $30-$3F
        case 0x30: c->x = EA_IDX(); c->cc = (uint8_t)((c->cc & ~CC_Z) | (c->x ? 0 : CC_Z)); DV(1); cyc += 4; break;
        case 0x31: c->y = EA_IDX(); c->cc = (uint8_t)((c->cc & ~CC_Z) | (c->y ? 0 : CC_Z)); DV(1); cyc += 4; break;
        case 0x32: c->s = EA_IDX(); DV(1); cyc += 4; break;
        case 0x33: c->u = EA_IDX(); DV(1); cyc += 4; break;
        case 0x34: m = IMM8(); DV(2); DM(c->s); c->pc = pc; cyc += 5 + M_FN(_psh)(c, m, true); break;
        case 0x35: m = IMM8(); DV(2); c->pc = pc; cyc += 5 + M_FN(_pul)(c, m, true); pc = c->pc; DM(c->s); recheck = true; break;
        case 0x36: m = IMM8(); DV(2); DM(c->u); c->pc = pc; cyc += 5 + M_FN(_psh)(c, m, false); break;
        case 0x37: m = IMM8(); DV(2); c->pc = pc; cyc += 5 + M_FN(_pul)(c, m, false); pc = c->pc; DM(c->u); recheck = true; break;
        case 0x39: DA(0); pc = PULLS16(); DM(c->s); cyc += 5; break;        // RTS
        case 0x3A: c->x = (uint16_t)(c->x + c->b); DA(0); DV(1); cyc += 3; break;   // ABX
        case 0x3B:                                                  // RTI
            DA(0);
            c->cc = PULLS8();
            if (c->cc & CC_E) {
                c->a = PULLS8(); c->b = PULLS8(); c->dp = PULLS8();
                c->x = PULLS16(); c->y = PULLS16(); c->u = PULLS16();
                cyc += 9;
            }
            pc = PULLS16();
            DM(c->s);
            recheck = true;
            cyc += 6;
            break;
        case 0x3C:                                                  // CWAI
            c->cc &= IMM8();
            DA(0); DV(1);
            c->cc |= CC_E;
            c->pc = pc;
            M_FN(_push_all)(c);
            c->wait = 2;
            recheck = true;
            cyc += 20;
            break;
        case 0x3D: {                                                // MUL
            uint16_t r = (uint16_t)(c->a * c->b);
            SET_D(r);
            c->cc = (uint8_t)((c->cc & ~(CC_Z | CC_C)) | (r ? 0 : CC_Z) | (r & 0x80 ? CC_C : 0));
            DA(0); DV(9);
            cyc += 11;
            break;
        }
        case 0x3F:                                                  // SWI
            c->cc |= CC_E;
            c->pc = pc;
            DA(0); DV(1);
            M_FN(_push_all)(c);
            c->cc |= CC_I | CC_F;
            DV(1);
            pc = RD16(0xFFFA);
            DV(1);
            cyc += 19;
            break;

        // ---- $80-$FF: A and B
        ALU8(0x80, c->a = M_FN(_sub8)(c, c->a, m, 0))
        ALU8(0x81, (void)M_FN(_sub8)(c, c->a, m, 0))
        ALU8(0x82, c->a = M_FN(_sub8)(c, c->a, m, F_C))
        ALU16(0x83, SET_D(M_FN(_sub16)(c, D(), w)); DA(0), 4, 6, 6, 7)
        ALU8(0x84, c->a &= m; M_FN(_nz8)(c, c->a))
        ALU8(0x85, M_FN(_nz8)(c, c->a & m))
        ALU8(0x86, c->a = m; M_FN(_nz8)(c, m))
        ST8(0x87, c->a)
        ALU8(0x88, c->a ^= m; M_FN(_nz8)(c, c->a))
        ALU8(0x89, c->a = M_FN(_add8)(c, c->a, m, F_C))
        ALU8(0x8A, c->a |= m; M_FN(_nz8)(c, c->a))
        ALU8(0x8B, c->a = M_FN(_add8)(c, c->a, m, 0))
        ALU16(0x8C, (void)M_FN(_sub16)(c, c->x, w); DA(0), 4, 6, 6, 7)
        case 0x8D: { int8_t o = (int8_t)IMM8(); DV(3); PUSHS16(pc); pc = (uint16_t)(pc + o); cyc += 7; break; }    // BSR
        case 0x9D: ea = EA_DIR(); DA(0); DV(1); PUSHS16(pc); pc = ea; cyc += 7; break;              // JSR
        case 0xAD: ea = EA_IDX(); DA(0); DV(1); PUSHS16(pc); pc = ea; cyc += 7; break;
        case 0xBD: ea = EA_EXT(); DA(0); DV(1); PUSHS16(pc); pc = ea; cyc += 8; break;
        ALU16(0x8E, c->x = w; M_FN(_nz16)(c, w), 3, 5, 5, 6)
        ST16(0x8F, c->x, 5, 5, 6)

        ALU8(0xC0, c->b = M_FN(_sub8)(c, c->b, m, 0))
        ALU8(0xC1, (void)M_FN(_sub8)(c, c->b, m, 0))
        ALU8(0xC2, c->b = M_FN(_sub8)(c, c->b, m, F_C))
        ALU16(0xC3, SET_D(M_FN(_add16)(c, D(), w)); DA(0), 4, 6, 6, 7)
        ALU8(0xC4, c->b &= m; M_FN(_nz8)(c, c->b))
        ALU8(0xC5, M_FN(_nz8)(c, c->b & m))
        ALU8(0xC6, c->b = m; M_FN(_nz8)(c, m))
        ST8(0xC7, c->b)
        ALU8(0xC8, c->b ^= m; M_FN(_nz8)(c, c->b))
        ALU8(0xC9, c->b = M_FN(_add8)(c, c->b, m, F_C))
        ALU8(0xCA, c->b |= m; M_FN(_nz8)(c, c->b))
        ALU8(0xCB, c->b = M_FN(_add8)(c, c->b, m, 0))
        ALU16(0xCC, SET_D(w); M_FN(_nz16)(c, w), 3, 5, 5, 6)
        ST16(0xCD, D(), 5, 5, 6)
        ALU16(0xCE, c->u = w; M_FN(_nz16)(c, w), 3, 5, 5, 6)
        ST16(0xCF, c->u, 5, 5, 6)

        // ---- page 2
        case 0x10: {
            unsigned op2 = IMM8();
            switch (op2) {
            case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
            case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
                LBRANCH(M_FN(_cond)(c, op2)); break;
            case 0x3F:                                              // SWI2
                c->cc |= CC_E;
                c->pc = pc;
                M_FN(_push_all)(c);
                pc = RD16(0xFFF4);
                cyc += 20;
                break;
            ALU16(0x83, (void)M_FN(_sub16)(c, D(), w); DA(0), 5, 7, 7, 8)
            ALU16(0x8C, (void)M_FN(_sub16)(c, c->y, w); DA(0), 5, 7, 7, 8)
            ALU16(0x8E, c->y = w; M_FN(_nz16)(c, w), 4, 6, 6, 7)
            ST16(0x8F, c->y, 6, 6, 7)
            ALU16(0xCE, c->s = w; M_FN(_nz16)(c, w), 4, 6, 6, 7)
            ST16(0xCF, c->s, 6, 6, 7)
            default: cyc += 2; break;
            }
            break;
        }
        // ---- page 3
        case 0x11: {
            unsigned op2 = IMM8();
            switch (op2) {
            case 0x3F:                                              // SWI3
                c->cc |= CC_E;
                c->pc = pc;
                M_FN(_push_all)(c);
                pc = RD16(0xFFF2);
                cyc += 20;
                break;
            ALU16(0x83, (void)M_FN(_sub16)(c, c->u, w); DA(0), 5, 7, 7, 8)
            ALU16(0x8C, (void)M_FN(_sub16)(c, c->s, w); DA(0), 5, 7, 7, 8)
            default: cyc += 2; break;
            }
            break;
        }
        default: cyc += 2; break;
        }
        c->icount -= cyc;
        } while (c->icount > 0 && !recheck);
    }
    c->pc = pc;
}

#undef RD
#undef DM
#undef DA
#undef DV
#undef M6809_DUMMY
#undef M6809_DUMMY_VMA
#undef M6809_SLOW_STEP
#undef M6809_NO_TRACE
#undef FETCH
#undef M6809_FETCH
#undef M6809_FETCH_BASE
#undef WR
#undef RD16
#undef WR16
#undef IMM8
#undef IMM16
#undef D
#undef SET_D
#undef PUSHS8
#undef PUSHS16
#undef PULLS8
#undef PUSHU8
#undef PUSHU16
#undef PULLU8
#undef PULLS16
#undef PULLU16
#undef EA_IDX
#undef EA_DIR
#undef EA_EXT
#undef ALU8
#undef ALU16
#undef ST8
#undef ST16
#undef BRANCH
#undef LBRANCH
#undef F_C
#undef F_V
#undef F_Z
#undef F_N
#undef F_NXV
#undef M_FN
#undef M_CAT
#undef M_CAT2
#undef M6809_NAME
#undef M6809_RD
#undef M6809_WR
