// cvdiag.c: see cvdiag.h.
#include "cvdiag.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/exception.h"
#include "hardware/structs/systick.h"
#include "hardware/watchdog.h"

#include "cvframes.h"
#include "../sdplayer/htrace.h"

#define TICK_HZ   20
#define WARN_US   1000000                   // the watchdog this close: note where core 0 is
#define PAINT     0x57AC57ACu               // an unused stack word

#define CFSR  (*(volatile uint32_t *)0xE000ED28u)
#define MMFAR (*(volatile uint32_t *)0xE000ED34u)
#define BFAR  (*(volatile uint32_t *)0xE000ED38u)

enum { MK_STAGE = 40, MK_PC, MK_LR, MK_FAULT, MK_SP };
enum { R_PC, R_LR, R_SP, R_STAGE, RESERVED };   // the marks cvd_start() keeps, in place

static int res_at = -1;                     // the first of them
static bool warned;
static uint32_t *stk_lo, *stk_hi;           // the stack cvd_stack() painted
static uint32_t sp_min = ~0u;

static ht_phase_t *phase(void) { return &HTR->ph[HT_Y_BOOT]; }

static void __not_in_flash_func(res_set)(int k, uint32_t t, uint32_t val) {
    if (res_at < 0) return;
    ht_mk_t *m = &phase()->mk[res_at + k];
    m->t = t;
    m->val = val;
}

// "<a>%08x<b>%08x" without printf (an interrupt may have stopped one)
static void __not_in_flash_func(note2)(const char *a, uint32_t x, const char *b, uint32_t y) {
    char n[33], *o = n;
    for (int k = 0; k < 2; k++) {
        for (const char *s = k ? b : a; *s; s++) *o++ = *s;
        uint32_t v = k ? y : x;
        for (int i = 28; i >= 0; i -= 4) *o++ = "0123456789abcdef"[v >> i & 15];
    }
    *o = 0;
    cvf_note(n);
}

void cvd_stage(uint32_t n) { res_set(R_STAGE, timer_hw->timerawl, n); }

void cvd_stack(void *base, uint32_t bytes) {
    stk_lo = (uint32_t *)base;
    stk_hi = (uint32_t *)((uint8_t *)base + bytes);
    for (uint32_t *p = stk_lo; p < stk_hi; p++) *p = PAINT;
}

uint32_t cvd_stack_used(void) {
    if (!stk_lo) return 0;
    const uint32_t *p = stk_lo;
    while (p < stk_hi && *p == PAINT) p++;
    return (uint32_t)((const uint8_t *)stk_hi - (const uint8_t *)p);
}

// the exception's frame: r0-r3, r12, lr, pc, xpsr (EXC_RETURN bit 2: the
// process stack). The fault's stub first lifts the stack limit (MSPLIM): a
// stack overflow's fault runs at the limit.
#define FRAME_STUB(name, handler, first)                                       \
    void __attribute__((naked)) __not_in_flash_func(name)(void) {              \
        __asm volatile(first                                                   \
                       "tst lr, #4\n\t"                                        \
                       "ite eq\n\t"                                            \
                       "mrseq r0, msp\n\t"                                     \
                       "mrsne r0, psp\n\t"                                     \
                       "ldr r1, =" #handler "\n\t"                             \
                       "bx r1\n\t"                                             \
                       ".ltorg\n");                                            \
    }

void __not_in_flash_func(cvd_tick)(const uint32_t *frame) {
    uint32_t t = timer_hw->timerawl;
    res_set(R_PC, t, frame[6]);
    res_set(R_LR, t, frame[5]);
    if ((uint32_t)frame < sp_min) {
        sp_min = (uint32_t)frame;
        res_set(R_SP, t, sp_min);
    }
    if (!warned && (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) &&
        (watchdog_hw->ctrl & WATCHDOG_CTRL_TIME_BITS) < WARN_US) {
        warned = true;
        note2("hung pc", frame[6], " lr", frame[5]);
        cvf_note_now();
    }
}

// A stack overflow's frame isn't stacked (the entry would pass the limit
// too; SP is left at it), so its PC is the paint. What ran it out is in the
// deepest frames instead: the return addresses (odd, in the flash's code)
// most often there, 'from' up 4K, the k likeliest first (0 past the last).
#define SCAN_WORDS 1024
#define SCAN_KEEP  16
// return addresses: the code's range (an XIP plugin's flash; else, as for a
// plugin that runs from SRAM, its image below .bss)
extern char __flash_binary_start[] __attribute__((weak)), __flash_binary_end[] __attribute__((weak));
extern char __logical_binary_start[], __bss_start__[];

static void __not_in_flash_func(deep_returns)(const uint32_t *from, uint32_t *out, int k) {
    uint32_t v[SCAN_KEEP], c[SCAN_KEEP];
    int n = 0;
    uint32_t lo = (uint32_t)__flash_binary_start, hi = (uint32_t)__flash_binary_end;
    if (!hi) { lo = (uint32_t)__logical_binary_start; hi = (uint32_t)__bss_start__; }
    for (const uint32_t *p = from; p < from + SCAN_WORDS && p < stk_hi; p++) {
        uint32_t x = *p;
        if (!(x & 1) || x < lo || x >= hi) continue;
        int i = 0;
        while (i < n && v[i] != x) i++;
        if (i == n) {
            if (n == SCAN_KEEP) continue;
            v[n] = x;
            c[n++] = 0;
        }
        c[i]++;
    }
    for (int j = 0; j < k; j++) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (c[i] && (best < 0 || c[i] > c[best])) best = i;
        out[j] = best < 0 ? 0 : v[best];
        if (best >= 0) c[best] = 0;
    }
}

void __not_in_flash_func(cvd_fault)(const uint32_t *frame) {
    uint32_t t = timer_hw->timerawl, cfsr = CFSR;
    uint32_t addr = cfsr & 0x8000u ? BFAR : cfsr & 0x80u ? MMFAR : 0;
    ht_phase_t *ph = phase();
    if (ph->nmk > HT_MK - 5) ph->nmk = HT_MK - 5;   // room for these
    if ((cfsr & 0x00100000u) && stk_lo) {
        uint32_t r[4];
        deep_returns(frame, r, 4);
        for (int j = 0; j < 4; j++) ht_mark(HT_Y_BOOT, t, MK_FAULT, r[j]);
        ht_mark(HT_Y_BOOT, t, MK_SP, (uint32_t)frame);
        note2("stack over ", r[0], " ", r[1]);
    } else {
        ht_mark(HT_Y_BOOT, t, MK_FAULT, frame[6]);
        ht_mark(HT_Y_BOOT, t, MK_FAULT, frame[5]);
        ht_mark(HT_Y_BOOT, t, MK_FAULT, cfsr);
        ht_mark(HT_Y_BOOT, t, MK_FAULT, addr);
        ht_mark(HT_Y_BOOT, t, MK_SP, (uint32_t)frame);
        if (cfsr & 0x00100000u) note2("stack sp", (uint32_t)frame, " pc", frame[6]);
        else note2("fault pc", frame[6], " cfsr", cfsr);
    }
    cvf_menu_now();
}

// panic() (cvplugin.cmake: PICO_PANIC_FUNCTION=cvd_panic): the SDK's pushes
// its caller's return address and calls this. Out of memory, a hard assert,
// TinyUSB's host: each the same _exit() otherwise. The note is the message
// (to its first '%' or line end) and that caller; the marks the caller, the
// message's address and the heap left; then back to the menu, as a fault.
static bool started;
extern uint32_t cvheap_free(void) __attribute__((weak));   // a HEAP plugin's (cvheap.c)

void __not_in_flash_func(cvd_panic_note)(const char *fmt, uint32_t lr) {
    if (!started) for (;;) __breakpoint();
    uint32_t t = timer_hw->timerawl;
    ht_phase_t *ph = phase();
    if (ph->nmk > HT_MK - 3) ph->nmk = HT_MK - 3;
    ht_mark(HT_Y_BOOT, t, MK_FAULT, lr);
    ht_mark(HT_Y_BOOT, t, MK_FAULT, (uint32_t)fmt);
    ht_mark(HT_Y_BOOT, t, MK_FAULT, cvheap_free ? cvheap_free() : 0);
    char n[33], *o = n;
    for (const char *s = "panic "; *s; s++) *o++ = *s;
    for (const char *s = fmt; s && *s && *s != '%' && *s != '\n' && o < n + 21; s++)   // 30 in all: what the menu shows
        *o++ = *s >= ' ' && *s < 127 ? *s : '?';
    while (o > n + 6 && o[-1] == ' ') o--;
    *o++ = ' ';
    for (int i = 28; i >= 0; i -= 4) *o++ = "0123456789abcdef"[lr >> i & 15];
    *o = 0;
    cvf_note(n);
    cvf_menu_now();
}

void __attribute__((naked, noreturn)) __not_in_flash_func(cvd_panic)(__unused const char *fmt, ...) {
    __asm volatile("ldr r1, [sp]\n\t"          // panic()'s caller
                   "ldr r2, =cvd_panic_note\n\t"
                   "bx r2\n\t"
                   ".ltorg\n");
}

FRAME_STUB(cvd_tick_stub, cvd_tick, "")
FRAME_STUB(cvd_fault_stub, cvd_fault, "movs r1, #0\n\tmsr msplim, r1\n\t")

void cvd_start(void) {
    ht_phase_t *ph = phase();
    if (ph->nmk + RESERVED <= HT_MK) {
        res_at = (int)ph->nmk;
        static const uint8_t code[RESERVED] = {MK_PC, MK_LR, MK_SP, MK_STAGE};
        for (int k = 0; k < RESERVED; k++) {
            ph->mk[res_at + k].code = code[k];
            ph->mk[res_at + k].t = timer_hw->timerawl;
            ph->mk[res_at + k].val = 0;
        }
        ph->nmk += RESERVED;
    }
    started = true;
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, cvd_fault_stub);
    exception_set_exclusive_handler(SYSTICK_EXCEPTION, cvd_tick_stub);
    systick_hw->csr = 0;
    systick_hw->rvr = clock_get_hz(clk_sys) / TICK_HZ - 1;
    systick_hw->cvr = 0;
    systick_hw->csr = 7;                    // the processor's clock, the interrupt, on
}
