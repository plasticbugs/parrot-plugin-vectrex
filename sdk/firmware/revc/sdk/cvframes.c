// cvframes.c -- see cvframes.h. The bus loop and the select handling are
// sdplayer.c's Game Boy mode (serve_mega, on_select, settle_select), whose
// comments have the measurements behind them; the power watch and the way
// back to the menu are cvkernel.c's.
#include "cvframes.h"

#include <stddef.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "../sdplayer/perm.h"
#include "../sdplayer/htrace.h"

#define LED_PIN     PICO_DEFAULT_LED_PIN
#define LED_BIT     (1u << LED_PIN)
#define PINS        (ADDR_MASK | NCART_BIT)
#define OFF_US      50000           // nCART low this long = console off
#define BOOT_US     1000000         // a header read this long after core 1 starts is RESET
#define PARK_US     60000           // RESET: not parked this long after, the BIOS took the title path
#define PARK_GIVE_UP_US 20000000    // ...and reboot into the menu anyway after this
#define CONFIRM     3

#define BANK 0x4000
static uint8_t fixed_ram[BANK] __attribute__((aligned(4)));     // the kernel
static uint8_t handoff_ram[BANK] __attribute__((aligned(4)));   // handoff.asm, RST 0 past it, in both halves
static uint8_t list_ram[2][BANK] __attribute__((aligned(4)));   // the lists, permuted

#ifdef CVF_SERVE2
// cvfserve.S's loop (cvplugin.cmake: cvframes_serve2()) and what it shares
// with core 0, at the offsets it has for them
typedef struct {
    const uint8_t *volatile ready, *volatile taken;
    volatile uint32_t frames;
    volatile uint8_t rep[4];
    const uint8_t *fixed;
    uint32_t t_serve;
    volatile uint32_t unsel, early, unhdr;
} cvf_serve_t;
_Static_assert(offsetof(cvf_serve_t, taken) == 4 && offsetof(cvf_serve_t, frames) == 8 && offsetof(cvf_serve_t, rep) == 12 &&
               offsetof(cvf_serve_t, fixed) == 16 && offsetof(cvf_serve_t, t_serve) == 20 && offsetof(cvf_serve_t, unsel) == 24 &&
               offsetof(cvf_serve_t, early) == 28 && offsetof(cvf_serve_t, unhdr) == 32, "cvfserve.S's offsets");
static cvf_serve_t sv = {.rep = {0x0F, 0x03, 0x0F, 0}};
void cvf_serve2(cvf_serve_t *s);            // returns at RESET
#define f_ready  sv.ready
#define f_taken  sv.taken
#define f_rep    sv.rep
#define f_frames sv.frames
static volatile bool to_menu, parked;
#else
static const uint8_t *volatile f_ready, *volatile f_taken;
static volatile uint8_t f_rep[3] = {0x0F, 0x03, 0x0F};
static volatile uint32_t f_frames;
static volatile bool to_menu, parked;
static uint32_t t_serve;
#endif
static ht_unperm_t ht_u;

// ---- core 1 ---------------------------------------------------------------------

#ifndef CVF_SERVE2
// the settled pins of a select, or 0 if it settles on something else
static inline __attribute__((always_inline)) uint32_t settle_select(uint32_t in) {
    uint32_t held = 0;
    for (uint32_t i = 0; i < 32 && held < CONFIRM; i++) {
        uint32_t s = sio_hw->gpio_in;
        if ((s & PINS) == (in & PINS)) { held++; continue; }
        if ((s & NCART_BIT) && held && (s & ADDR_MASK) == (in & ADDR_MASK)) return in;   // the read ended
        if ((s & SEL_MASK) != SEL_ADDR) return 0;
        in = s;                            // another select value: still settling
        held = 0;
    }
    return held >= CONFIRM ? in : 0;
}

static inline __attribute__((always_inline)) bool settled(uint32_t in) {
    for (uint32_t i = 0; i < CONFIRM; i++)
        if ((sio_hw->gpio_in & PINS) != (in & PINS)) return false;
    return true;
}

// $FFC0+n: the controller (n < 48), or a frame's start, which takes the
// list published since the last one (f_taken set before f_ready clears, so
// core 0 never writes the bank the console runs), or runs the last again
static inline __attribute__((always_inline)) const uint8_t *frame_select(uint32_t n, const uint8_t *cur) {
    if (n < 48) {
        f_rep[n >> 4] = (uint8_t)(n & 15);
        return cur;
    }
    const uint8_t *b = f_ready;
    if (b) {
        f_taken = b;
        __dmb();
        f_ready = 0;
    } else {
        b = f_taken;
    }
    f_frames = f_frames + 1;
    return b;
}

// a select: switch, then keep serving until the read ends, so each select
// is taken once
static const uint8_t *__scratch_x("serve") __attribute__((noinline)) on_select(uint32_t in, const uint8_t *fixed, const uint8_t *cur) {
    if (!(in = settle_select(in))) return cur;
    cur = frame_select(SELECT_N(in), cur);
    do {
        in = sio_hw->gpio_in;
        const uint8_t *b = in & A14_BIT ? cur : fixed;
        sio_hw->gpio_out = (uint32_t)b[MEGA_INDEX(in)] << PD_SHIFT | LED_BIT;
    } while ((in & SEL_MASK) == SEL_ADDR);
    return cur;
}

// $8000, settled: the BIOS reading the header. Within a second of starting,
// that is the kernel booting; later, RESET.
static bool __scratch_x("serve") __attribute__((noinline)) on_header(uint32_t in) {
    if (!settled(in)) return false;
    return timer_hw->timerawl - t_serve > BOOT_US;
}

#endif

// after RESET: handoff.asm in both halves, each settled read traced
// (cvkernel.c's serve_traced, for a bank-indexed image), until the console
// parks (core 0 then reboots into the menu)
static void __attribute__((noinline, noreturn)) __not_in_flash_func(serve_handoff)(void) {
    const uint8_t *img = handoff_ram;
    ht_phase_t *ph = &HTR->ph[HT_Y_RESET];
    uint32_t prev = ~0u, same = 0, park_n = 0;
    bool done = true;                       // the header read that was RESET
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        sio_hw->gpio_out = (uint32_t)img[MEGA_INDEX(in)] << PD_SHIFT | LED_BIT;
        uint32_t pins = in & PINS;
        if (pins & NCART_BIT) { done = false; prev = ~0u; continue; }
        if (done) continue;
        if (pins != prev) { prev = pins; same = 0; continue; }
        if (++same < 2) continue;
        done = true;
        uint32_t off = ht_off(&ht_u, pins), t = timer_hw->timerawl;
        if (off == 0) {
            ht_hdr(ph, t, img[0]);
            continue;
        }
        ht_bus(ph, t, off, img[MEGA_INDEX(pins)]);
        if (!ht_park_read(off)) park_n = 0;
        else if (++park_n == HT_PARKED) parked = true;
    }
}

#ifdef CVF_SERVE2
static void __attribute__((noreturn, noinline)) __not_in_flash_func(serve)(void) {
    save_and_disable_interrupts();
    sv.fixed = fixed_ram;
    sv.t_serve = timer_hw->timerawl;
    cvf_serve2(&sv);
    // RESET: the handoff, from the next sample on (both headers are 55 AA)
    to_menu = true;
    serve_handoff();
}
#else
static void __scratch_x("serve") __attribute__((noreturn, noinline)) serve(void) {
    save_and_disable_interrupts();
    t_serve = timer_hw->timerawl;
    const uint8_t *fixed = fixed_ram, *cur = f_taken;
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        const uint8_t *b = in & A14_BIT ? cur : fixed;
        sio_hw->gpio_out = (uint32_t)b[MEGA_INDEX(in)] << PD_SHIFT | LED_BIT;
        if ((in & SEL_MASK) == SEL_ADDR) cur = on_select(in, fixed, cur);
        else if (!(in & PINS) && on_header(in)) break;
    }
    // RESET: the handoff, from the next sample on (both headers are 55 AA)
    to_menu = true;
    serve_handoff();
}
#endif

// ---- core 0 -----------------------------------------------------------------------

uint32_t cvf_frames(void) { return f_frames; }

void cvf_input(uint8_t *joy, uint8_t *fire, uint8_t *key) {
    *joy = f_rep[0];
    *fire = f_rep[1];
    *key = f_rep[2];
}

bool cvf_waiting(void) { return f_ready != 0; }

void cvf_bus_counts(uint32_t *unsel, uint32_t *early, uint32_t *unhdr) {
#ifdef CVF_SERVE2
    *unsel = sv.unsel;
    *early = sv.early;
    *unhdr = sv.unhdr;
#else
    *unsel = *early = *unhdr = 0;
#endif
}

bool cvf_publish(const uint8_t *list, size_t n) {
    if (f_ready || n > BANK) return false;
    const uint8_t *taken = f_taken;
    uint8_t *buf = list_ram[0] == taken ? list_ram[1] : list_ram[0];
    for (size_t i = 0; i < n; i++) buf[bindex((uint32_t)i)] = perm_lut[list[i]];
    __dmb();
    f_ready = buf;
    return true;
}

// ---- console power, and back to the menu (cvkernel.c's) --------------------------
static bool driving, console_off;
static uint32_t last_high;
static char note[33];

bool cvf_console_on(void) { return driving; }

const cvl_t *cvf_launch(void) {
    const cvl_t *b = CVL_BLOCK;
    return cvl_ok(b) && b->magic == CVL_LAUNCH ? b : NULL;
}

void cvf_note(const char *s) {
    memset(note, 0, sizeof note);
    if (s) strncpy(note, s, sizeof note - 1);
}

static inline void watch(uint32_t in, uint32_t t) {
    if (!(in & NCART_BIT) && driving && t - last_high > OFF_US)
        for (int i = 0; i < 256 && !(in & NCART_BIT); i++) in = sio_hw->gpio_in;
    if (in & NCART_BIT) {
        last_high = t;
        if (!driving) {
            sio_hw->gpio_oe_set = PD_MASK;
            driving = true;
            ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_DRIVE, 0);
        }
    } else if (driving && t - last_high > OFF_US) {
        sio_hw->gpio_oe_clr = PD_MASK;
        driving = false;
        console_off = true;
        ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_UNDRIVE, t - last_high);
    }
    bool led = driving || (t >> 19 & 1);
    if (led) sio_hw->gpio_oe_set = LED_BIT;
    else     sio_hw->gpio_oe_clr = LED_BIT;
}

// the menu is the flash's boot image (a watchdog reboot is a normal boot);
// the outputs go off first, so the parked console reads $00 until it's served
static void (*on_exit)(void);
void cvf_on_exit(void (*fn)(void)) { on_exit = fn; }

static void __attribute__((noreturn)) to_menu_now(uint32_t why) {
    multicore_reset_core1();
    ht_mark(HT_Y_RESET, timer_hw->timerawl, HM_REBOOT, why);
    sio_hw->gpio_oe_clr = PD_MASK;
    void (*fn)(void) = on_exit;
    on_exit = NULL;                         // once, whatever it does
    if (fn && why != 4) fn();               // (4: a fault: nothing more)
    cvl_t *b = CVL_BLOCK;
    if (cvl_ok(b)) cvl_return(b, note[0] ? note : NULL);
    watchdog_reboot(0, 0, 1);
    for (;;) tight_loop_contents();
}

void cvf_menu_now(void) { to_menu_now(4); }

void cvf_note_now(void) {
    cvl_t *b = CVL_BLOCK;
    if (cvl_ok(b)) cvl_return(b, note[0] ? note : NULL);
}

static uint32_t t_menu;
static bool titled;

void cvf_task(uint32_t t) {
    watch(sio_hw->gpio_in, t);
    if (to_menu && !t_menu) {
        t_menu = t | 1;
        ht_mark(HT_Y_RESET, t, HM_RESET, 0);
    }
    if (console_off) to_menu_now(1);
    if (!t_menu) return;
    int32_t since = (int32_t)(t - t_menu);
    if (parked) {
        ht_mark(HT_Y_RESET, t, HM_PARKED, 0);
        to_menu_now(2);
    }
    if (!titled && since > PARK_US) {      // it missed the header: the title path reads AA 55 at 252 ms
        titled = true;
        handoff_ram[bindex(0)] = perm_lut[0xAA];
        handoff_ram[bindex(1)] = perm_lut[0x55];
        ht_mark(HT_Y_RESET, t, HM_TITLE_PATH, 0);
    }
    if (since > PARK_GIVE_UP_US) to_menu_now(3);
}

void cvf_start(const cvf_config_t *c) {
    __asm volatile ("cpsie i" ::: "memory"); // as after a reset, whatever the menu's chain left
    for (uint p = 0; p < 15; p++) {         // address lines: plain inputs, driven at 5V
        gpio_init(p);
        gpio_disable_pulls(p);
    }
    gpio_init(NCART_PIN);
    gpio_pull_down(NCART_PIN);              // reads low while U5 is unpowered
    for (uint p = PD_SHIFT; p < PD_SHIFT + 8; p++)
        gpio_init(p);                       // outputs only once watch() sees the console
    gpio_init(LED_PIN);
    build_luts();
    memset(fixed_ram, perm_lut[0xC7], sizeof fixed_ram);
    for (uint32_t a = 0; a < c->kernel_len && a < BANK; a++) fixed_ram[bindex(a)] = perm_lut[c->kernel[a]];
    memset(handoff_ram, perm_lut[0xC7], sizeof handoff_ram);
    for (uint32_t a = 0; a < c->handoff_len && a < BANK; a++) handoff_ram[bindex(a)] = perm_lut[c->handoff[a]];
    memset(list_ram, perm_lut[0], sizeof list_ram);   // empty lists: 0 0
    f_taken = list_ram[0];
    f_ready = 0;
    ht_init(false);                         // the menu started it: add this side
    ht_clear(HT_Y_BOOT);
    ht_clear(HT_Y_RESET);
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_MAIN, 0);
    ht_unperm_init(&ht_u, addr_line);
    __dmb();
    multicore_launch_core1(serve);
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_SERVE, 0);
    watch(sio_hw->gpio_in, time_us_32());   // drive the bus now if the console is on
}
