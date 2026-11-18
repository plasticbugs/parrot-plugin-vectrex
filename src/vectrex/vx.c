// vx.c -- see vx.h.
//
// Time is kept in quarters of a CPU cycle (vt_t): MAME delays the analog
// signals by 8,500 ns, 12.75 cycles, and everything else is whole cycles.
// It is 32 bits, counted from a base that is moved up every 20 seconds or
// so (rebase()): 64-bit times cost the M33 a third of this file's time.
// Nothing is stepped: the VIA's timers and the delayed analog changes are
// events with a time, run in time order (and, at the same time, in the order
// they were set: MAME's scheduler does the same) whenever the CPU touches
// the VIA and at the end of each run. Between two events the beam moves in
// a straight line.
#include "vx.h"

#include <stddef.h>
#include <string.h>

#include "../starwars/m6809.h"

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define VX_HOT __attribute__((section(".time_critical.vx")))
#else
#define VX_HOT
#endif

typedef int32_t vt_t;
#define VT_NEVER      INT32_MAX
#define CYC(n)        ((vt_t)(n) * 4)
#define ANALOG_DELAY  51            // 8,500 ns
#define INT_PER_CLOCK 550
#define IFR_DELAY     3

// a refresh: no shorter than this (a second start of timer 2 inside one
// doesn't cut the picture in two), and cut at this if the game never
// starts timer 2
#define REFRESH_MIN   12000
#define REFRESH_MAX   45000

enum { A_X, A_ZR, A_Z, A_AUDIO, A_Y };      // MAME's names: A_X is the multiplexer's channel 0, which is
                                            // the beam's up and down; A_Y is the DAC itself, left and right
enum { INT_CA2 = 1, INT_CA1 = 2, INT_SR = 4, INT_CB2 = 8, INT_CB1 = 0x10, INT_T2 = 0x20, INT_T1 = 0x40, INT_ANY = 0x80 };
enum { TM_T1, TM_T2, TM_SHIFT, TM_SHIFT_IRQ, TM_CA2, TM_N };
enum { EV_ANALOG, EV_RAMP, EV_ZERO };

#define FIFO_N 64                   // delayed analog changes waiting (51 quarters: a dozen at most)

static struct {
    uint8_t rom[VX_ROM_SIZE];
    uint8_t cart[VX_CART_MAX];
    uint8_t ram[0x400];
    const uint8_t *cart_base;       // the 32K at $0000 now
    uint32_t cart_size;
    bool cart_banked, cart_sram;
    uint64_t base;                  // the cycle that time 0 is

    // the VIA (MAME's via6522_device, the parts a Vectrex uses)
    uint8_t out_a, out_b, ddr_a, ddr_b, t1ll, t1lh, t2ll, t2lh, sr, acr, pcr, ier, ifr;
    uint8_t t1_active, t1_pb7, t2_active, shift_counter, out_ca2, out_cb1, out_cb2;
    vt_t time1, time2;
    struct { vt_t at; uint32_t seq; bool on; } tm[TM_N];
    uint32_t seq;
    vt_t cur;                       // the time of what is being done now
    vt_t next;                      // nothing is due before this

    // the analog side (MAME's vectrex_base_state)
    uint8_t via_out[2];             // PORTB, PORTA as the board sees them
    int8_t analog[5];
    uint8_t blank, ramp, cb2;
    uint8_t ramp_sched;             // RAMP as the last change set for it will leave it
    int32_t x_int, y_int;           // MAME's: 16.16 screen units, x the long way (up)
    vt_t vector_start;
    struct { vt_t at; uint32_t seq; uint32_t param; uint8_t kind; } fifo[FIFO_N];
    uint8_t fifo_r, fifo_w;
    uint32_t zero_seq;              // the zeroing now waiting (an earlier one is dropped)

    // the picture
    vx_vec_t vec[2][VX_VEC_MAX];
    int nvec[2];
    uint8_t draw;                   // the buffer being drawn into
    uint32_t pictures;
    int32_t last_x, last_y;         // the last point, as a vector's end
    bool have_dot;
    vt_t refresh_at;                // when this refresh started
    // raster text (vx.h)
    vx_text_t text[2][VX_TEXT_MAX];
    int ntext[2];
    bool txt_tag;                   // a sweep of text is being drawn: its vectors are marked
    uint8_t txt_row, txt_col;
    uint16_t txt_block;
    int16_t txt_x0, txt_y0;         // where this sweep started
    vt_t txt_last;                  // its last byte's time
    int txt_first_vec, txt_first_text;      // the block's first vector and character

    // the AY-3-8912
    uint8_t ay[16], ay_latch;
    bool ay_active;
    vt_t env_at;                    // when the envelope's shape was last written

    // the controllers
    uint8_t pot[4];                 // x1 y1 x2 y2, 0-255
    uint8_t buttons;                // as the AY's port reads them: 0 = pressed

    // the DAC's sound channel
    uint8_t dac_level;
    int32_t dac_rest;               // the resting level, << 8
    vt_t dac_next;
    int16_t samples[VX_SAMPLE_MAX];
    uint16_t nsamples;
    uint16_t dac_moves;
    uint8_t dac_playing;

    vx_stats_t stats;
} m;

static m6809_t cpu;
static uint32_t sub;                // bus cycles into the instruction now running

#define NZ1(i)  ((i) & 0x80 ? CC_N : (i) == 0 ? CC_Z : 0)
#define NZ4(i)  NZ1(i), NZ1((i) + 1), NZ1((i) + 2), NZ1((i) + 3)
#define NZ16(i) NZ4(i), NZ4((i) + 4), NZ4((i) + 8), NZ4((i) + 12)
#define NZ64(i) NZ16(i), NZ16((i) + 16), NZ16((i) + 32), NZ16((i) + 48)
const uint8_t m6809_nz[256] = {NZ64(0), NZ64(64), NZ64(128), NZ64(192)};

uint8_t *vx_rom(void) { return m.rom; }
uint8_t *vx_cart(void) { return m.cart; }
uint8_t *vx_scratch(void) { return (uint8_t *)m.vec; }
_Static_assert(sizeof m.vec >= VX_SCRATCH, "the scratch room is there");

// ---- the picture -------------------------------------------------------------------
// MAME's vectrex_base_state::add_point, made into vectors as its screen_update
// hands them to the vector device: a line from the last point to this one,
// if this one's intensity isn't 0.
static inline int16_t unit(int32_t v) {     // 16.16 screen units to 1/64s, kept in an int16_t
    v >>= 10;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static void VX_HOT add_point(int32_t x, int32_t y, int intensity) {
    bool moved = x != m.last_x || y != m.last_y;
    m.stats.points++;
    if (intensity > 0) {
        int n = m.nvec[m.draw];
        vx_vec_t *v = &m.vec[m.draw][n];
        uint8_t z = (uint8_t)(intensity > 254 ? 254 : intensity);
        if (!moved && m.have_dot) {         // the beam still where it was: the same dot (its brightest)
            if (v[-1].z < z) v[-1].z = z;
        } else if (n < VX_VEC_MAX) {
            // (MAME's x is the screen's long side: up)
            v->x0 = unit(m.last_y - (150 << 16));
            v->y0 = unit(m.last_x - (200 << 16));
            v->x1 = unit(y - (150 << 16));
            v->y1 = unit(x - (200 << 16));
            v->z = z;
            v->text = m.txt_tag && m.cur - m.txt_last <= CYC(20);
            m.nvec[m.draw] = n + 1;
            m.have_dot = !moved;
        } else {
            m.stats.dropped++;
        }
    } else {
        m.have_dot = false;
    }
    m.last_x = x;
    m.last_y = y;
}

// the block of text so far is over: fewer than three sweeps weren't text
static void text_block_end(void) {
    if (m.txt_row < 2) {             // (0xFF: none open)
        for (int i = m.txt_first_vec; i < m.nvec[m.draw]; i++) m.vec[m.draw][i].text = 0;
        m.ntext[m.draw] = m.txt_first_text;
    }
    m.txt_tag = false;
    m.txt_row = 0xFF;
    m.txt_first_vec = m.nvec[m.draw];
    m.txt_first_text = m.ntext[m.draw];
}

static void end_refresh(void) {
    text_block_end();
    m.draw ^= 1;
    m.nvec[m.draw] = 0;
    m.ntext[m.draw] = 0;
    m.txt_first_vec = m.txt_first_text = 0;
    m.have_dot = false;
    m.pictures++;
    m.refresh_at = m.cur;
}

void vx_text(const vx_text_t **t, int *n) {
    *t = m.text[m.draw ^ 1];
    *n = m.ntext[m.draw ^ 1];
}

uint32_t vx_picture(const vx_vec_t **v, int *n) {
    *v = m.vec[m.draw ^ 1];
    *n = m.nvec[m.draw ^ 1];
    return m.pictures;
}

// ---- the analog side ---------------------------------------------------------------
static inline int32_t clamp_int(int64_t v) {    // (an integrator stops at its rail; MAME's just overflows)
    const int64_t lim = (int64_t)500 << 16;
    return (int32_t)(v > lim + (200 << 16) ? lim + (200 << 16) : v < -lim ? -lim : v);
}

// quarters of a cycle since the beam's last point (an integrator is at its rail long before this limit)
static inline int32_t beam_time(void) {
    int32_t dt = m.cur - m.vector_start;
    return dt > 2000000 ? 2000000 : dt;
}

static void VX_HOT update_vector(void) {
    if (!m.ramp) {
        int64_t length = INT_PER_CLOCK * beam_time() / 4;
        m.x_int = clamp_int(m.x_int + length * (m.analog[A_X] + m.analog[A_ZR]));
        m.y_int = clamp_int(m.y_int + length * (m.analog[A_Y] + m.analog[A_ZR]));
        add_point(m.x_int, m.y_int, 2 * m.analog[A_Z] * m.blank);
    } else if (m.blank) {
        add_point(m.x_int, m.y_int, 2 * m.analog[A_Z]);
    }
    m.vector_start = m.cur;
}

// A byte's first bit is leaving the shift register. If the beam is sweeping
// level, left to right, it is a character's row: next along this sweep, or
// the start of the next sweep down, or of another piece of text.
static void VX_HOT text_byte(void) {
    int h = m.analog[A_Y] + m.analog[A_ZR];
    if (m.ramp || h <= 0 || m.analog[A_X] + m.analog[A_ZR] != 0 || m.analog[A_Z] <= 0) return;
    int64_t length = INT_PER_CLOCK * beam_time() / 4;
    int16_t x = unit(clamp_int(m.y_int + length * h) - (150 << 16)), y = unit(m.x_int - (200 << 16));
    if (m.txt_tag && m.cur - m.txt_last <= CYC(30)) {
        m.txt_col++;
    } else {
        int dx = x - m.txt_x0, dy = m.txt_y0 - y;
        if (m.txt_row < 15 && m.cur - m.txt_last < CYC(4000) && dx > -2 * VX_UNIT && dx < 2 * VX_UNIT && dy > 0 && dy < 8 * VX_UNIT) {
            m.txt_row++;
        } else {
            text_block_end();
            m.txt_block++;
            if (!m.txt_block) m.txt_block = 1;
            m.txt_row = 0;
        }
        m.txt_col = 0;
        m.txt_x0 = x;
        m.txt_y0 = y;
        m.txt_tag = true;
    }
    m.txt_last = m.cur;
    int n = m.ntext[m.draw];
    if (n < VX_TEXT_MAX) {
        vx_text_t *t = &m.text[m.draw][n];
        t->x = x;
        t->y = y;
        t->pitch = (int16_t)((2 * INT_PER_CLOCK * h) >> 6);
        t->block = m.txt_block;
        t->row = m.txt_row;
        t->col = m.txt_col;
        t->bits = m.sr;
        t->z = (uint8_t)(2 * m.analog[A_Z]);
        m.ntext[m.draw] = n + 1;
    }
}

static inline void next_is(vt_t at) {
    if (at < m.next) m.next = at;
}

static void delayed(uint8_t kind, uint32_t param) {
    uint8_t w = m.fifo_w;
    m.fifo[w].at = m.cur + ANALOG_DELAY;
    m.fifo[w].seq = m.seq++;
    m.fifo[w].kind = kind;
    m.fifo[w].param = param;
    if (kind == EV_ZERO) m.zero_seq = m.fifo[w].seq;
    m.fifo_w = (uint8_t)((w + 1) % FIFO_N);
    if (m.fifo_w == m.fifo_r) m.fifo_r = (uint8_t)((m.fifo_r + 1) % FIFO_N);   // (never: it would take 64 writes in 13 cycles)
    next_is(m.fifo[w].at);
}

// the DAC's sound channel: the level held since the last write, a sample
// every VX_SAMPLE_DIV cycles
static void dac_to(vt_t t) {
    while (m.dac_next <= t) {
        int32_t s = ((int32_t)m.dac_level << 8) - m.dac_rest;
        m.dac_rest += s >> 8;                               // the resting level follows, slowly (30 ms)
        if (m.nsamples == VX_SAMPLE_MAX) {
            memmove(m.samples, m.samples + 1, sizeof m.samples - sizeof m.samples[0]);
            m.nsamples--;
        }
        m.samples[m.nsamples++] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        m.dac_next += CYC(VX_SAMPLE_DIV);
    }
}

static void multiplexer(int mux) {
    delayed(EV_ANALOG, (uint32_t)(m.via_out[1] << 9 | 0x100 | mux));
    if (mux == A_AUDIO) {
        uint8_t level = m.via_out[1] ^ 0x80;                // (the NOT gate on the schematic)
        m.stats.dac_writes++;
        if (level != m.dac_level) {
            dac_to(m.cur);
            m.dac_level = level;
            if (m.dac_moves < 0xFFFF) m.dac_moves++;
        }
    }
}

static void ay_write(bool address, uint8_t data) {
    if (address) {
        m.ay_active = (data >> 4) == 0;
        if (m.ay_active) m.ay_latch = data & 15;
    } else if (m.ay_active) {
        m.ay[m.ay_latch] = data;
        if (m.ay_latch == 13) m.env_at = m.cur;
    }
}

static void via_pb_w(uint8_t data) {
    // cartridge bank switching
    if (m.cart_banked && ((data ^ m.via_out[0]) & 0x40)) {
        m.cart_base = m.cart + ((data >> 6) & 1) * 0x8000;
        m.stats.bank_switches++;
    }
    // sound
    if (data & 0x10) ay_write(data & 0x08, m.via_out[1]);
    if (!(data & 1) && (m.via_out[0] & 1)) multiplexer((data >> 1) & 3);
    m.via_out[0] = data;
    // (MAME sets RAMP again on every write; one that leaves it as it will be
    // anyway would only cut the vector being drawn in two)
    if ((data & 0x80) != m.ramp_sched) {
        m.ramp_sched = data & 0x80;
        delayed(EV_RAMP, data & 0x80);
    }
}

static void via_pa_w(uint8_t data) {
    // the DAC's output always goes to the Y integrator
    m.via_out[1] = data;
    delayed(EV_ANALOG, A_Y);
    if (!(m.via_out[0] & 1)) multiplexer((m.via_out[0] >> 1) & 3);
}

static void via_ca2_w(int state) {
    if (state == 0) delayed(EV_ZERO, 0);
}

static void via_cb2_w(int state) {
    if (m.cb2 != state) {
        update_vector();            // (MAME's update_blank, a timer of no delay)
        m.blank = (uint8_t)state;
        m.cb2 = (uint8_t)state;
    }
}

static uint8_t via_pb_r(void) {
    int pot = m.pot[(m.via_out[0] & 6) >> 1] - 0x80;
    if (pot > (int8_t)m.via_out[1]) m.via_out[0] |= 0x20;
    else m.via_out[0] &= (uint8_t)~0x20;
    return m.via_out[0];
}

static uint8_t via_pa_r(void) {
    // BDIR inactive, BC1 active: the PSG can be read
    if (!(m.via_out[0] & 0x10) && (m.via_out[0] & 0x08)) {
        uint8_t v = 0xFF;
        if (m.ay_active) {
            if (m.ay_latch == 14) m.ay[14] = m.buttons;
            v = m.ay[m.ay_latch];
        }
        m.via_out[1] = v;
    }
    return m.via_out[1];
}

static void fifo_fire(void) {
    uint8_t r = m.fifo_r;
    uint8_t kind = m.fifo[r].kind;
    uint32_t param = m.fifo[r].param;
    uint32_t seq = m.fifo[r].seq;
    m.fifo_r = (uint8_t)((r + 1) % FIFO_N);
    switch (kind) {
    case EV_ANALOG:
        update_vector();
        if (param & 0x100) m.analog[param & 0xFF] = (int8_t)(param >> 9);
        else m.analog[param] = (int8_t)m.via_out[1];
        break;
    case EV_RAMP:
        update_vector();
        m.ramp = (uint8_t)param;
        break;
    default:                        // EV_ZERO (MAME's timer is adjusted, so only the last one set runs)
        if (seq != m.zero_seq) break;
        m.x_int = (200 << 16) + m.analog[A_ZR] * INT_PER_CLOCK;
        m.y_int = (150 << 16) + m.analog[A_ZR] * INT_PER_CLOCK;
        add_point(m.x_int, m.y_int, 0);
        break;
    }
}

// ---- the VIA -------------------------------------------------------------------------
#define T1_VALUE (m.t1ll + (m.t1lh << 8))
#define T2_VALUE (m.t2ll + (m.t2lh << 8))
#define SR_MODE  (m.acr & 0x1C)
#define SR_DISABLED   (SR_MODE == 0x00)
#define SI_T2_CONTROL (SR_MODE == 0x04)
#define SI_O2_CONTROL (SR_MODE == 0x08)
#define SI_EXT_CONTROL (SR_MODE == 0x0C)
#define SO_T2_RATE    (SR_MODE == 0x10)
#define SO_T2_CONTROL (SR_MODE == 0x14)
#define SO_O2_CONTROL (SR_MODE == 0x18)
#define SO_EXT_CONTROL (SR_MODE == 0x1C)

static void arm(int k, vt_t at) {
    m.tm[k].at = at;
    m.tm[k].seq = m.seq++;
    m.tm[k].on = true;
    next_is(at);
}

static void output_irq(void) {
    if (m.ier & m.ifr & 0x7F) {
        if (!(m.ifr & INT_ANY)) {
            m.ifr |= INT_ANY;
            cpu.irq = 1;
            m6809_stop(&cpu);       // the run that follows sees the line
            m.stats.irqs++;
        }
    } else if (m.ifr & INT_ANY) {
        m.ifr &= (uint8_t)~INT_ANY;
        cpu.irq = 0;
    }
}

static void set_int(uint8_t bits) {
    if (!(m.ifr & bits)) {
        m.ifr |= bits;
        output_irq();
    }
}

static void clear_int(uint8_t bits) {
    if (m.ifr & bits) {
        m.ifr &= (uint8_t)~bits;
        output_irq();
    }
}

static void output_pa(void) { via_pa_w((uint8_t)((m.out_a & m.ddr_a) | ~m.ddr_a)); }

static void output_pb(void) {
    uint8_t pb = (uint8_t)((m.out_b & m.ddr_b) | ~m.ddr_b);
    if (m.acr & 0x80) pb = (uint8_t)((pb & 0x7F) | m.t1_pb7 << 7);
    via_pb_w(pb);
}

static uint8_t input_pa(void) { return (uint8_t)((~m.ddr_a & via_pa_r()) | (m.out_a & m.ddr_a)); }

static uint8_t input_pb(void) {
    uint8_t pb = (uint8_t)~m.ddr_b;
    if (m.ddr_b != 0xFF) pb &= via_pb_r();
    pb |= m.out_b & m.ddr_b;
    if (m.acr & 0x80) pb = (uint8_t)((pb & 0x7F) | m.t1_pb7 << 7);
    return pb;
}

static void shift_out(void) {
    if (m.shift_counter == 15 && SO_O2_CONTROL) text_byte();
    if (m.shift_counter & 1) {      // the top bit goes out on a falling edge
        m.out_cb2 = (m.sr >> 7) & 1;
        m.sr = (uint8_t)(m.sr << 1 | m.out_cb2);
        via_cb2_w(m.out_cb2);
        if (m.shift_counter == 1 && SO_EXT_CONTROL) set_int(INT_SR);
    } else if (!SO_T2_RATE) {
        if (m.shift_counter == 0 && (SO_O2_CONTROL || SO_T2_CONTROL)) set_int(INT_SR);
    }
    m.shift_counter = (m.shift_counter - 1) & 15;
}

static void shift_in(void) {
    if (!(m.shift_counter & 1)) {
        m.sr = (uint8_t)(m.sr << 1);            // (CB2 as an input reads 0)
        if (m.shift_counter == 0 && !SR_DISABLED) arm(TM_SHIFT_IRQ, m.cur + CYC(1));
    }
    m.shift_counter = (m.shift_counter - 1) & 15;
}

static void shift_start(int edges) {            // after a read or a write of the shift register
    if (!(SI_EXT_CONTROL || SO_EXT_CONTROL)) {
        m.out_cb1 = 1;
        m.shift_counter = 15;
    } else {
        m.shift_counter = 16;                   // (CB1 as an input reads 0; nothing clocks it)
    }
    clear_int(INT_SR);
    if (SO_O2_CONTROL || SI_O2_CONTROL) arm(TM_SHIFT, m.cur + CYC(edges) / 2);
    else if (SO_T2_RATE || SO_T2_CONTROL || SI_T2_CONTROL) {
        if (edges == 7 && SO_T2_RATE) return;   // (a read leaves the free-running shifter alone)
        arm(TM_SHIFT, m.cur + CYC(m.t2ll + 2) / 2);
    } else m.tm[TM_SHIFT].on = false;
}

static void VX_HOT timer_fire(int k) {
    switch (k) {
    case TM_T1:
        if (m.acr & 0x40) {
            if (T1_VALUE > 0) m.t1_pb7 = !m.t1_pb7;
            arm(TM_T1, m.cur + CYC(T1_VALUE + IFR_DELAY));
        } else {
            m.t1_pb7 = 1;
            m.t1_active = 0;
            m.time1 = m.cur;
            m.tm[TM_T1].on = false;
        }
        if (m.acr & 0x80) output_pb();
        set_int(INT_T1);
        break;
    case TM_T2:
        m.t2_active = 0;
        m.time2 = m.cur;
        m.tm[TM_T2].on = false;
        set_int(INT_T2);
        break;
    case TM_SHIFT:
        m.tm[TM_SHIFT].on = false;
        m.out_cb1 ^= 1;
        if (SO_T2_RATE || SO_T2_CONTROL || SO_O2_CONTROL) shift_out();
        else if (SI_T2_CONTROL || SI_O2_CONTROL) shift_in();
        if (SO_T2_RATE || m.shift_counter < 15) {
            if (SI_O2_CONTROL || SO_O2_CONTROL) arm(TM_SHIFT, m.cur + CYC(1));
            else if (SO_T2_RATE || SO_T2_CONTROL || SI_T2_CONTROL) arm(TM_SHIFT, m.cur + CYC(m.t2ll + 2) / 2);
        }
        break;
    case TM_SHIFT_IRQ:
        m.tm[TM_SHIFT_IRQ].on = false;
        set_int(INT_SR);
        break;
    default:                        // TM_CA2: the end of a CA2 pulse
        m.tm[TM_CA2].on = false;
        m.out_ca2 = 1;
        via_ca2_w(1);
        break;
    }
}

// everything due by t, in order
static void VX_HOT events_to(vt_t t) {
    while (m.next <= t) {
        // the soonest (the one set first, of two at the same time), and the soonest of the rest
        int best = -1;
        vt_t at = VT_NEVER, other = VT_NEVER;
        uint32_t seq = 0;
        for (int k = 0; k < TM_N; k++) {
            if (!m.tm[k].on) continue;
            if (m.tm[k].at < at || (m.tm[k].at == at && (int32_t)(m.tm[k].seq - seq) < 0)) {
                other = at;
                best = k;
                at = m.tm[k].at;
                seq = m.tm[k].seq;
            } else if (m.tm[k].at < other) {
                other = m.tm[k].at;
            }
        }
        if (m.fifo_r != m.fifo_w) {
            vt_t fat = m.fifo[m.fifo_r].at;
            if (fat < at || (fat == at && (int32_t)(m.fifo[m.fifo_r].seq - seq) < 0)) {
                other = at;
                best = TM_N;
                at = fat;
            } else if (fat < other) {
                other = fat;
            }
        }
        if (best < 0) { m.next = VT_NEVER; break; }
        if (at > t) { m.next = at; break; }
        m.cur = at;
        m.stats.events++;
        if (best == TM_N) {
            fifo_fire();
        } else if (best == TM_SHIFT && SO_O2_CONTROL) {
            // The shift register clocked by the CPU's clock, an edge a cycle
            // (text, and every patterned line): its edges up to t, or to the
            // next thing due, in one go. (timer_fire's TM_SHIFT, for this mode.)
            vt_t limit = other <= t ? other - 1 : t;
            for (;;) {
                m.out_cb1 ^= 1;
                if (m.shift_counter & 1) {
                    if (m.shift_counter == 15) text_byte();
                    m.out_cb2 = (m.sr >> 7) & 1;
                    m.sr = (uint8_t)(m.sr << 1 | m.out_cb2);
                    if (m.out_cb2 != m.cb2) via_cb2_w(m.out_cb2);
                } else if (m.shift_counter == 0) {
                    set_int(INT_SR);
                }
                m.shift_counter = (m.shift_counter - 1) & 15;
                if (m.shift_counter == 15) { m.tm[TM_SHIFT].on = false; break; }
                at += CYC(1);
                if (at > limit) { arm(TM_SHIFT, at); break; }
                m.cur = at;
            }
        } else {
            timer_fire(best);
        }
        m.next = 0;                 // (found again on the next pass)
    }
    m.cur = t;
}

// The time of the bus cycle now running, and the events due before it.
//
// What falls due on that very cycle is MAME's to order, and it doesn't
// always order it the same way: its CPU's time inside a timeslice and its
// timers' times are rounded to the attosecond differently, so a timer that
// ends on the cycle its flag is read comes out a hair before the read or a
// hair after, by where the timeslice began. test/trace.sh shows what it
// mostly does, and that is done here: the read doesn't see the timer
// (timer 2 at the end of a refresh, a long timer 1 at the end of a line:
// hundreds of each, all that way), unless it is a short timer 1 (a
// patterned line's, a dot's: thousands that way, a handful the other).
// Every trace that parts from MAME's parts at one of that handful.
static inline vt_t bus_time(void) { return CYC((int32_t)(uint32_t)(m6809_now(&cpu) - m.base) + (int32_t)sub - 1); }

#define TIE_SHORT 50               // a timer 1 shorter than this is seen at a tie
static inline void bus_events(void) {
    vt_t t = bus_time();
    events_to(t - 1);
    if (m.tm[TM_T1].on && m.tm[TM_T1].at == t && !(m.acr & 0x40) && T1_VALUE < TIE_SHORT) {
        m.cur = t;
        timer_fire(TM_T1);
    }
    m.cur = t;
}

static uint16_t counter1(void) {
    if (m.t1_active) return (uint16_t)((m.tm[TM_T1].at - m.cur) / 4 - IFR_DELAY);
    return (uint16_t)(0xFFFF - (m.cur - m.time1) / 4 + 1);     // (one more than its source says: MAME's, in Spike's trace)
}

static uint8_t VX_HOT via_read(unsigned reg) {
    bus_events();
    m.stats.via_reads++;
    uint8_t val = 0;
    switch (reg & 15) {
    case 0:
        val = input_pb();
        clear_int(INT_CB1 | ((m.pcr & 0xA0) != 0x20 ? INT_CB2 : 0));
        break;
    case 1:
        val = input_pa();
        clear_int(INT_CA1 | ((m.pcr & 0x0A) != 0x02 ? INT_CA2 : 0));
        if (m.out_ca2 && ((m.pcr & 0x0E) == 0x0A || (m.pcr & 0x0C) == 0x08)) {
            m.out_ca2 = 0;
            via_ca2_w(0);
        }
        if ((m.pcr & 0x0E) == 0x0A) arm(TM_CA2, m.cur + CYC(1));
        break;
    case 15: val = input_pa(); break;
    case 2: val = m.ddr_b; break;
    case 3: val = m.ddr_a; break;
    case 4:
        clear_int(INT_T1);
        val = (uint8_t)counter1();
        break;
    case 5: val = (uint8_t)(counter1() >> 8); break;
    case 6: val = m.t1ll; break;
    case 7: val = m.t1lh; break;
    case 8:
    case 9: {
        uint16_t c;
        if ((reg & 15) == 8) clear_int(INT_T2);
        if (m.t2_active && m.tm[TM_T2].on) c = (uint16_t)((m.tm[TM_T2].at - m.cur) / 4);
        else if (m.acr & 0x20) c = (uint16_t)T2_VALUE;
        else c = (uint16_t)(0x10000 - (((m.cur - m.time2) / 4) & 0xFFFF) - 1);
        val = (reg & 15) == 8 ? (uint8_t)c : (uint8_t)(c >> 8);
        break;
    }
    case 10:
        val = m.sr;
        shift_start(7);
        break;
    case 11: val = m.acr; break;
    case 12: val = m.pcr; break;
    case 13: val = m.ifr; break;
    default: val = m.ier | 0x80; break;
    }
    return val;
}

static void VX_HOT via_write(unsigned reg, uint8_t data) {
    bus_events();
    m.stats.via_writes++;
    switch (reg & 15) {
    case 0:
        m.out_b = data;
        if (m.ddr_b != 0) output_pb();
        clear_int(INT_CB1 | ((m.pcr & 0xA0) != 0x20 ? INT_CB2 : 0));
        if (m.out_cb2 && ((m.pcr & 0xE0) == 0xA0 || (m.pcr & 0xC0) == 0x80)) {
            m.out_cb2 = 0;
            via_cb2_w(0);
        }
        break;
    case 1:
        m.out_a = data;
        if (m.ddr_a != 0) output_pa();
        clear_int(INT_CA1 | ((m.pcr & 0x0A) != 0x02 ? INT_CA2 : 0));
        if (m.out_ca2 && ((m.pcr & 0x0E) == 0x0A || (m.pcr & 0x0C) == 0x08)) {
            m.out_ca2 = 0;
            via_ca2_w(0);
        }
        if ((m.pcr & 0x0E) == 0x0A) arm(TM_CA2, m.cur + CYC(1));
        break;
    case 15:
        m.out_a = data;
        if (m.ddr_a != 0) output_pa();
        break;
    case 2:
        if (data != m.ddr_b) {
            m.ddr_b = data;
            output_pb();
        }
        break;
    case 3:
        if (data != m.ddr_a) {
            m.ddr_a = data;
            output_pa();
        }
        break;
    case 4:
    case 6: m.t1ll = data; break;
    case 7:
        m.t1lh = data;
        clear_int(INT_T1);
        break;
    case 5:
        m.t1lh = data;
        clear_int(INT_T1);
        m.t1_pb7 = 0;
        if (m.acr & 0x80) output_pb();
        arm(TM_T1, m.cur + CYC(T1_VALUE + IFR_DELAY));
        m.t1_active = 1;
        break;
    case 8: m.t2ll = data; break;
    case 9:
        m.t2lh = data;
        clear_int(INT_T2);
        if (!(m.acr & 0x20)) arm(TM_T2, m.cur + CYC(T2_VALUE + IFR_DELAY));
        else m.time2 = m.cur;
        m.t2_active = 1;
        // the game starts its refresh timer again: the picture so far is one refresh
        m.stats.t2_starts++;
        if (m.cur - m.refresh_at >= CYC(REFRESH_MIN)) end_refresh();
        break;
    case 10:
        m.sr = data;
        shift_start(6);
        break;
    case 12:
        m.pcr = data;
        if ((data & 0x0C) == 0x0C && m.out_ca2 != ((data & 0x02) >> 1)) {
            m.out_ca2 = (data & 0x02) >> 1;
            via_ca2_w(m.out_ca2);
        }
        if ((data & 0xC0) == 0xC0 && m.out_cb2 != ((data & 0x20) >> 5)) {
            m.out_cb2 = (data & 0x20) >> 5;
            via_cb2_w(m.out_cb2);
        }
        break;
    case 11: {
        uint16_t c1 = counter1();
        m.acr = data;
        output_pb();
        if (SR_DISABLED || SI_EXT_CONTROL || SO_EXT_CONTROL) m.tm[TM_SHIFT].on = false;
        if (m.acr & 0x40) {
            arm(TM_T1, m.cur + CYC(c1 + IFR_DELAY));
            m.t1_active = 1;
        }
        if (SI_T2_CONTROL || SI_O2_CONTROL || SI_EXT_CONTROL) {
            m.out_cb2 = 1;
            via_cb2_w(1);
        }
        break;
    }
    case 14:
        if (data & 0x80) m.ier |= data & 0x7F;
        else m.ier &= (uint8_t)~(data & 0x7F);
        output_irq();
        break;
    default:                        // 13
        clear_int(data & 0x7F);
        break;
    }
    // with interrupts on, the run is cut so the next one ends at the VIA's next event
    if (m.ier & 0x7F) m6809_stop(&cpu);
}

// ---- the CPU's bus ---------------------------------------------------------------------
static inline __attribute__((always_inline)) uint8_t bus_rd(uint16_t a) {
    if (a >= 0xE000) return m.rom[a & 0x1FFF];
    if (a < 0x8000) return m.cart_base[a];
    if ((a & 0xF800) == 0xC800) return m.ram[a & 0x3FF];
    if ((a & 0xF800) == 0xD000) return via_read(a);
    return 0;
}

static inline __attribute__((always_inline)) void bus_wr(uint16_t a, uint8_t v) {
    if ((a & 0xF800) == 0xC800) m.ram[a & 0x3FF] = v;
    else if ((a & 0xF800) == 0xD000) via_write(a, v);
    else if (a < 0x8000 && m.cart_sram) m.cart[a & (m.cart_size - 1)] = v;
}

#ifdef VX_TRACE
void VX_TRACE(const m6809_t *c);
#define M6809_TRACE(c) (sub = 0, VX_TRACE(c))
#else
#define M6809_TRACE(c) (sub = 0)        // (the core's hook before each instruction: the bus cycles start again)
#endif
#define M6809_HOT VX_HOT
#define M6809_NAME vx6809
// (functions, so two fetches in one expression each count)
static inline __attribute__((always_inline)) uint8_t cpu_rd(uint16_t a) { sub++; return bus_rd(a); }
static inline __attribute__((always_inline)) void cpu_wr(uint16_t a, uint8_t v) { sub++; bus_wr(a, v); }
#define M6809_FETCH(a) cpu_rd(a)
#define M6809_RD(a) cpu_rd(a)
#define M6809_WR(a, v) cpu_wr(a, v)
#define M6809_DUMMY(a) ((void)sub++)
#define M6809_DUMMY_VMA(n) ((void)(sub += (unsigned)(n)))
#include "../starwars/m6809_core.h"

// ---- the machine ---------------------------------------------------------------------
void vx_init(uint32_t cart_size, uint16_t seed) {
    uint32_t size = cart_size > VX_CART_MAX ? VX_CART_MAX : cart_size;
    if (!size) {
        memset(m.cart, 0, sizeof m.cart);                   // (MAME's empty slot reads 0)
    } else {
        if (size < 0x8000) memset(m.cart + size, 0xFF, 0x8000 - size);
        else if (size > 0x8000) memset(m.cart + size, 0xFF, VX_CART_MAX - size);
        if (size <= 0x8000) memset(m.cart + 0x8000, 0xFF, 0x8000);
    }
    m.cart_size = size;
    m.cart_banked = size > 0x8000;
    m.cart_sram = size && !memcmp(m.cart + 6, "SRAM", 4);
    if (m.cart_sram) {                                      // (its size as a mask: a power of two)
        uint32_t p = 0x1000;
        while (p < size) p <<= 1;
        m.cart_size = p;
    }
    memset(m.ram, 0, sizeof m.ram);
    m.ram[0x7E] = (uint8_t)(seed | 1);
    m.ram[0x7F] = (uint8_t)(seed >> 8 | 1);
    for (int k = 0; k < 4; k++) m.pot[k] = 0x80;
    m.buttons = 0xFF;
    vx_reset();
}

void vx_reset(void) {
    m.base = m6809_now(&cpu);
    memset((uint8_t *)&m + offsetof(__typeof__(m), out_a), 0, offsetof(__typeof__(m), vec) - offsetof(__typeof__(m), out_a));
    m.cur = 0;
    m.next = VT_NEVER;
    m.vector_start = m.cur;
    m.refresh_at = m.cur;
    m.dac_next = m.cur;
    m.dac_level = 0x80;
    m.txt_row = 0xFF;
    m.dac_rest = 0x80 << 8;
    m.cart_base = m.cart + (m.cart_banked ? 0x8000 : 0);    // (MAME: bank 1 until PB6 says)
    m.x_int = 200 << 16;
    m.y_int = 150 << 16;
    m.last_x = m.x_int;
    m.last_y = m.y_int;
    m.t1ll = 0xF3; m.t1lh = 0xB5; m.t2ll = 0xFF; m.t2lh = 0xFF;
    m.time1 = m.time2 = m.cur;
    // MAME's via6522_device::device_reset
    m.out_ca2 = m.out_cb1 = m.out_cb2 = 1;
    m.t1_pb7 = 1;
    output_pa();
    output_pb();
    via_ca2_w(1);
    via_cb2_w(1);
    memset(m.ay, 0, sizeof m.ay);
    cpu.pending = 0;
    sub = 0;
    vx6809_reset(&cpu);
}

void vx_inputs(const vx_inputs_t *in) {
    m.pot[0] = (uint8_t)(0x80 + in->x[0]);
    m.pot[1] = (uint8_t)(0x80 + in->y[0]);
    m.pot[2] = (uint8_t)(0x80 + in->x[1]);
    m.pot[3] = (uint8_t)(0x80 + in->y[1]);
    m.buttons = (uint8_t)~in->buttons;
}

// the VIA's next event that can raise an interrupt the game has asked for
static vt_t next_irq(void) {
    vt_t t = VT_NEVER;
    if ((m.ier & INT_T1) && m.tm[TM_T1].on && m.tm[TM_T1].at < t) t = m.tm[TM_T1].at;
    if ((m.ier & INT_T2) && m.tm[TM_T2].on && m.tm[TM_T2].at < t) t = m.tm[TM_T2].at;
    if (m.ier & INT_SR) {
        if (m.tm[TM_SHIFT].on && m.tm[TM_SHIFT].at < t) t = m.tm[TM_SHIFT].at;
        if (m.tm[TM_SHIFT_IRQ].on && m.tm[TM_SHIFT_IRQ].at < t) t = m.tm[TM_SHIFT_IRQ].at;
    }
    return t;
}

// Time 0 moved up to near now (by a multiple of 65,536 cycles: the timers'
// counts when they aren't running go by the time since they stopped, modulo
// that). What happened long ago is kept as long ago, no further back than
// the times can tell.
static void rebase(void) {
    const vt_t k = m.cur & ~((1 << 20) - 1), old = 1 << 28;
    m.base += (uint32_t)k / 4;
    m.cur -= k;
    if (m.next != VT_NEVER) m.next -= k;
    for (int i = 0; i < TM_N; i++)
        if (m.tm[i].on) m.tm[i].at -= k;
    for (uint8_t r = m.fifo_r; r != m.fifo_w; r = (uint8_t)((r + 1) % FIFO_N)) m.fifo[r].at -= k;
    vt_t *t[] = {&m.time1, &m.time2};
    for (int i = 0; i < 2; i++) {
        *t[i] -= k;
        if (m.cur - *t[i] > old) *t[i] += ((m.cur - *t[i] - old / 2) >> 18) << 18;
    }
    vt_t *c[] = {&m.vector_start, &m.refresh_at, &m.txt_last, &m.env_at, &m.dac_next};
    for (int i = 0; i < 5; i++) {
        *c[i] -= k;
        if (m.cur - *c[i] > old) *c[i] = m.cur - old;
    }
}

// MAME's 6809 takes an interrupt whose timer ended on an instruction's last
// cycle one instruction later (Spike's trace): the line is looked at a cycle
// after the timer.
#define VX_IRQ_LAG 1

void VX_HOT vx_run(uint32_t cycles) {
    uint64_t target = m6809_now(&cpu) + cycles;
    for (;;) {
        uint64_t now = m6809_now(&cpu);
        if (now >= target) break;
        int64_t n = (int64_t)(target - now);
        if (m.ier & 0x7F) {
            vt_t t = next_irq();
            if (t != VT_NEVER) {
                int64_t to = (t + 3) / 4 + VX_IRQ_LAG - (int64_t)(now - m.base);
                if (to < 1) to = 1;
                if (to < n) n = to;
            }
        }
        // The 6809 waiting in CWAI with its registers stacked, and the
        // interrupt there: the core charges an interrupt's whole entry (19
        // cycles), MAME the vector's fetch (4: test/trace.sh, Bedlam). That
        // entry alone is run, and the difference taken back.
        bool cwai = cpu.wait == 2 && cpu.irq;
        vx6809_run(&cpu, cwai ? 1 : (int)n);
        if (cwai && !cpu.wait) cpu.end -= 15;
        sub = 1;                    // (bus_time() is now itself)
        events_to(bus_time() - CYC(VX_IRQ_LAG));
    }
    events_to(bus_time());
    dac_to(m.cur);
    if (m.cur - m.refresh_at >= CYC(REFRESH_MAX)) end_refresh();    // a game that leaves timer 2 alone
    if (m.cur > (1 << 29)) rebase();
}

void vx_ay(vx_ay_t *ay) {
    memcpy(ay->reg, m.ay, 16);
    // the envelope: 16 steps a ramp, a step every 16 x its period cycles
    uint32_t period = (uint32_t)(m.ay[11] | m.ay[12] << 8);
    if (!period) period = 1;
    uint32_t step = (uint32_t)((m.cur - m.env_at) / 4) / (16 * period);
    unsigned shape = m.ay[13] & 15, level;
    bool attack = shape & 4;
    if (step < 16) {
        level = attack ? (unsigned)step : 15 - (unsigned)step;
    } else if (!(shape & 8) || (shape & 1)) {       // one ramp, then held: at 0, or (shapes 11 and 13) at the top
        level = (shape & 8) && ((shape & 2) ? !attack : attack) ? 15 : 0;
    } else {                                        // repeating: a saw (8, 12) or a triangle (10, 14)
        bool up = attack;
        if ((shape & 2) && ((step >> 4) & 1)) up = !up;
        level = up ? (unsigned)(step & 15) : 15 - (unsigned)(step & 15);
    }
    ay->env = (uint8_t)level;
}

int vx_samples(int16_t *out, int max, bool *playing) {
    int n = m.nsamples > max ? max : m.nsamples;
    memcpy(out, m.samples + m.nsamples - n, (size_t)n * sizeof out[0]);
    m.nsamples = 0;
    // something is playing while the level moves a few hundred times a second
    if (m.dac_moves >= 8) m.dac_playing = 4;
    else if (m.dac_playing) m.dac_playing--;
    m.dac_moves = 0;
    *playing = m.dac_playing != 0;
    if (!*playing) memset(out, 0, (size_t)n * sizeof out[0]);
    return n;
}

void vx_stats(vx_stats_t *s) {
    *s = m.stats;
    s->cycles = m6809_now(&cpu);
    s->pc = cpu.pc;
}

uint8_t vx_peek(uint16_t a) {
    if (a >= 0xE000) return m.rom[a & 0x1FFF];
    if (a < 0x8000) return m.cart_base[a];
    if ((a & 0xF800) == 0xC800) return m.ram[a & 0x3FF];
    return 0;
}
