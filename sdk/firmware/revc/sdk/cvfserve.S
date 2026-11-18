// cvfserve.S -- core 1's bus loop for cvframes (cvframes.h), for a plugin
// built with cvframes_serve2() (cvplugin.cmake). It is cvplay/cvserve.S's
// MegaCart loop (CVSERVE.md has the bus timing and the console's captures
// behind it), with cvframes' two banks and its selects:
//
//   - the byte for the address on the pins goes out on every turn;
//   - nothing is made of one sample of the pins: two, 60 ns apart, must
//     agree before a byte goes out or an address is acted on. When the
//     74HCT245 switches the console's data bus (our byte changing with nCART
//     low, or the buffer turning on as nCART falls) the address lines and
//     nCART read wrong at the cart's pins for a sample or so; the loop in
//     cvframes.c samples once, 33 ns after its own write, puts out the byte
//     for what it saw, and stops serving altogether (on_select: its call,
//     the settling, the return) whenever one sample looks like a select;
//   - a select ($FFC0+n) is told from an address on its way by more samples
//     while its byte stays out, and one that isn't costs its read nothing:
//     the sample that differed is served at once;
//   - a header read ($8000) is RESET only if it holds for HCONFIRM samples
//     more, a second or more after the loop started.
//
// The loop's turn is cvserve.S's to the cycle (26: the lookup is 9 here
// too), so what the console has shown of that loop (CVBUS, Mappy and
// Mr. Do! Too with the Super Game Module, build 6) holds for this one's
// timing; sdk/test/cvframes_sim.py runs this file's object code against the
// Game Boy kernel's own bus cycles.
//
//   void cvf_serve2(cvf_serve_t *s);    returns at RESET
#include "pins.h"

#if GPIO_A0 != 0 || GPIO_A1 != 1 || GPIO_A2 != 2 || GPIO_A3 != 4 || GPIO_A4 != 6 || GPIO_A5 != 9
#error "cvfserve.S: A0-A5 on GPIO 0, 1, 2, 4, 6, 9 (the select's n)"
#endif
#if GPIO_A14 != 8
#error "cvfserve.S: A14 on GPIO 8 (the bank; perm.h's MEGA_INDEX)"
#endif

#define SIO_BASE     0xd0000000
#define GPIO_IN      4
#define GPIO_OUT     16
#define TIMERAWL     0x400b0028            // TIMER0 (timer_hw->timerawl)
#define LED_BIT      (1 << 25)
#define NCART        0x8000
#define SEL_ADDR     ((1<<GPIO_A6)|(1<<GPIO_A7)|(1<<GPIO_A8)|(1<<GPIO_A9)|(1<<GPIO_A10)|(1<<GPIO_A11)|(1<<GPIO_A12)|(1<<GPIO_A13)|(1<<GPIO_A14))
#define SEL_MASK     (SEL_ADDR | NCART)
#define BOOT_US      1000000               // the BIOS booting the kernel reads the header too

// cvf_serve_t (cvframes.c)
#define S_READY      0                     // the list published, not taken yet (or 0)
#define S_TAKEN      4                     // the list the console runs
#define S_FRAMES     8                     // frames the console has started
#define S_REP        12                    // the controller, as the kernel reported it (3 bytes)
#define S_FIXED      16                    // the kernel's bank
#define S_TSERVE     20                    // when the loop started (timerawl)
#define S_UNSEL      24                    // looked like a select, wasn't
#define S_EARLY      28                    // selects taken as their read ended
#define S_UNHDR      32                    // looked like $8000, wasn't

#define CONFIRM      3                     // samples a select must hold for after the two that showed it
#define HCONFIRM     5                     // ...a header read ($8000), 5 cycles apart

        .syntax unified
        .cpu cortex-m33
        .thumb

// r0 = the pins (gpio_in); r3 = the byte for them: 9 cycles. r1, r2 scratch.
// (r7 is the list's bank less the kernel's: A14, GPIO 8, picks.)
.macro LOOKUP
        sbfx    r2, r0, #8, #1              // A14: all ones, or 0
        ands    r2, r7
        add     r2, r12
        lsrs    r3, r0, #1                  // perm.h's MEGA_INDEX: GPIO 0-7 and 9-14
        and     r3, r3, #0x3f00
        uxtb    r1, r0
        orrs    r3, r1
        ldrb    r3, [r2, r3]
.endm
.macro PUT                                  // r3 out on GP16-23: 3 cycles
        orr     r3, r8, r3, lsl #16
        str     r3, [r6, #GPIO_OUT]
.endm
.macro SERVE
        LOOKUP
        PUT
.endm

// r0 the pins being served, r1 a later sample, r2-r3 scratch, r4 SEL_ADDR,
// r5 SEL_MASK, r6 SIO, r7 the list's bank less the kernel's, r8 LED, r9 the
// pins being confirmed, r10 a count, r11 cvf_serve_t, r12 the kernel's bank
        .section .scratch_x.cvf_serve2, "ax"
        .global cvf_serve2
        .type   cvf_serve2, %function
        .thumb_func
        .align  2
cvf_serve2:
        push    {r4, r5, r6, r7, r8, r9, r10, r11, lr}
        mov     r11, r0
        ldr     r12, [r11, #S_FIXED]
        ldr     r7, [r11, #S_TAKEN]
        sub     r7, r7, r12
        mov     r6, #SIO_BASE
        movw    r4, #SEL_ADDR
        movw    r5, #SEL_MASK
        mov     r8, #LED_BIT

// ---- every read: 26 cycles a turn -------------------------------------------
// The lookup for the first sample is done while waiting for the second.
1:      ldr     r0, [r6, #GPIO_IN]          // 2
2:      LOOKUP                              // 9
        ldr     r1, [r6, #GPIO_IN]          // 2
        eor     r2, r0, r1                  // 1
        lsls    r2, r2, #16                 // 1
        bne     5f                          // 1
        PUT                                 // 3
        and     r2, r0, r5                  // 1
        cmp     r2, r4                      // 1
        beq     10f                         // 1  $FFC0-$FFFF, nCART low?
        lsls    r3, r0, #17                 // 1  A0-A14 all low? C = nCART
        nop                                 // 1  (cvserve.S counts a turn here)
        bne     1b                          // 2
        bcs     1b                          //    nCART high: not a read
        b       20f                         //    $8000, nCART low
5:      mov     r0, r1                      // they differ: the later one, and another after it
        b       2b

// ---- it looks like a select -------------------------------------------------
// r0 looked like one and has been served. CONFIRM more samples must show the
// same pins, 8 cycles (53 ns) apart from 80 ns after the byte went out. A
// sample that differs is looked at again 53 ns on, and only two running
// count as the pins having moved (one alone is a disturbance).
10:     uxth    r9, r0
        movs    r10, #CONFIRM
    .rept 6
        nop
    .endr
15:     ldr     r1, [r6, #GPIO_IN]
        uxth    r3, r1
        cmp     r3, r9
        bne     11f
17:     subs    r10, #1
        bne     15b
// taken: n = A0-A5 (GPIO 0, 1, 2, 4, 6, 9)
13:     and     r1, r9, #7
        lsrs    r2, r9, #1
        and     r2, r2, #8
        orrs    r1, r2
        lsrs    r2, r9, #2
        and     r2, r2, #16
        orrs    r1, r2
        lsrs    r2, r9, #4
        and     r2, r2, #32
        orrs    r1, r2
        cmp     r1, #48
        bhs     30f
// $FFC0+n, n < 48: the controller, a nibble in the address
        lsrs    r2, r1, #4
        and     r1, r1, #15
        add     r2, r2, r11
        strb    r1, [r2, #S_REP]
        b       14f
// $FFF0+seq: a frame's start. The list published since the last one (taken
// set before ready clears, so core 0 never writes the bank the console
// runs), or the last one again
30:     ldr     r2, [r11, #S_READY]
        cbz     r2, 31f
        str     r2, [r11, #S_TAKEN]
        dmb
        movs    r3, #0
        str     r3, [r11, #S_READY]
        sub     r7, r2, r12
31:     ldr     r2, [r11, #S_FRAMES]
        adds    r2, #1
        str     r2, [r11, #S_FRAMES]
// served on until the read ends: each select is taken once. The address
// must differ from the select's twice running to end it; if the pins are
// then another select value, that is looked at as a new one.
14:     ldr     r0, [r6, #GPIO_IN]
        SERVE
    .rept 12
        nop
    .endr
18:     ldr     r0, [r6, #GPIO_IN]
        eor     r3, r0, r9
        lsls    r3, r3, #17                 // (the address: nCART may have risen)
        beq     18b                         // (nothing to put out: the byte is as it was)
    .rept 4
        nop
    .endr
        ldr     r0, [r6, #GPIO_IN]
        eor     r3, r0, r9
        lsls    r3, r3, #17
        beq     18b
        and     r2, r0, r5
        cmp     r2, r4
        bne     2b                          // the read is over: r0 is the next address
        b       10b

// a sample differed. nCART risen and the address as it was is the read
// ending, and the select counts. An address on its way elsewhere does not
// look like this.
11:     cmp     r10, #CONFIRM               // (not before one sample has held)
        beq     7f
        eor     r2, r3, r9
        cmp     r2, #NCART
        beq     16f
7:
// else look again: only two samples running count as the pins having moved
        ldr     r1, [r6, #GPIO_IN]
        uxth    r3, r1
        cmp     r3, r9
        beq     17b                         // back as it was
        cmp     r10, #CONFIRM
        beq     12f
        eor     r2, r3, r9
        cmp     r2, #NCART
        bne     12f
16:     ldr     r2, [r11, #S_EARLY]
        adds    r2, #1
        str     r2, [r11, #S_EARLY]
        b       13b
// another select value: the address was settling, follow it. Anything else:
// it wasn't a select, and the latest sample is the one to serve
12:     and     r2, r1, r5
        cmp     r2, r4
        bne     19f
        mov     r9, r3
        movs    r10, #CONFIRM
        b       15b
19:     ldr     r2, [r11, #S_UNSEL]
        adds    r2, #1
        str     r2, [r11, #S_UNSEL]
        mov     r0, r1
        b       2b

// ---- it looks like the header ($8000) ---------------------------------------
// r0 showed it (twice) and has been served: HCONFIRM more samples. Within a
// second of starting that is the BIOS booting the kernel; later, RESET, and
// serve() goes on with the handoff inside this read (both images' headers
// are 55 AA).
20:
    .rept HCONFIRM
        ldr     r1, [r6, #GPIO_IN]
        lsls    r3, r1, #16
        bne     29f
    .endr
        ldr     r2, =TIMERAWL
        ldr     r2, [r2]
        ldr     r3, [r11, #S_TSERVE]
        subs    r2, r2, r3
        ldr     r3, =BOOT_US
        cmp     r2, r3
        bls     1b
        pop     {r4, r5, r6, r7, r8, r9, r10, r11, pc}
29:     ldr     r2, [r11, #S_UNHDR]
        adds    r2, #1
        str     r2, [r11, #S_UNHDR]
        mov     r0, r1
        b       2b
        .ltorg
        .size   cvf_serve2, . - cvf_serve2
