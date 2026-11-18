// perm.h -- the rev C pin order (pins.h, from hw3/pins.py) as the bus
// loops see it.  Shared by the firmware and the host test.
//
// flat image (32K, plain ROMs and the loader): indexed by the raw GPIO 0-14
// mega bank (16K): indexed by GPIO 0-7 and 9-14; GPIO 8 (A14) picks the
//   fixed bank ($8000-$BFFF) or the selected one ($C000-$FFFF)
// bytes in both are stored in GP16-23 order (perm_lut)
#pragma once
#include <stdint.h>
#include "pins.h"

#define NCART_PIN   15                     // low while the console selects the cart
#define NCART_BIT   (1u << NCART_PIN)
#define ADDR_MASK   0x7fffu                // GPIO 0-14
#define PD_SHIFT    16
#define PD_MASK     (0xffu << PD_SHIFT)

_Static_assert(GPIO_A14 == 8, "MEGA_INDEX assumes A14 on GPIO 8");
#define BIT(l)      (1u << GPIO_A##l)
#define A14_BIT     BIT(14)
#define MEGA_INDEX(in) (((in) & 0xffu) | ((in) >> 1 & 0x3f00u))
#define SEL_ADDR    (BIT(6) | BIT(7) | BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(12) | BIT(13) | BIT(14))
#define SEL_MASK    (SEL_ADDR | NCART_BIT) // (in & SEL_MASK) == SEL_ADDR: $FFC0-$FFFF selected
#define SELECT_N(in) (((in) >> GPIO_A0 & 1) | ((in) >> GPIO_A1 & 1) << 1 | ((in) >> GPIO_A2 & 1) << 2 | \
                      ((in) >> GPIO_A3 & 1) << 3 | ((in) >> GPIO_A4 & 1) << 4 | ((in) >> GPIO_A5 & 1) << 5)

static uint8_t perm_lut[256];              // console byte -> GP16-23 order
static uint16_t flo[256], fhi[128];        // cart address (15 bits) -> flat index
static uint16_t blo[128], bhi[128];        // bank offset (14 bits) -> bank index

static inline uint32_t findex(uint32_t a) { return flo[a & 0xff] | fhi[a >> 8 & 0x7f]; }
static inline uint32_t bindex(uint32_t a) { return blo[a & 0x7f] | bhi[a >> 7 & 0x7f]; }

static void build_luts(void) {
    uint8_t line_gpio[15];
    for (unsigned g = 0; g < 15; g++) line_gpio[addr_line[g]] = (uint8_t)g;
    for (unsigned b = 0; b < 256; b++) {
        uint8_t v = 0;
        for (unsigned k = 0; k < 8; k++) if (b >> data_bit[k] & 1) v |= (uint8_t)(1u << k);
        perm_lut[b] = v;
    }
    for (unsigned v = 0; v < 256; v++) {
        uint16_t i = 0;
        for (unsigned l = 0; l < 8; l++) if (v >> l & 1) i |= (uint16_t)(1u << line_gpio[l]);
        flo[v] = i;
    }
    for (unsigned v = 0; v < 128; v++) {
        uint16_t i = 0, lo = 0, hi = 0;
        for (unsigned l = 0; l < 7; l++) {
            if (!(v >> l & 1)) continue;
            unsigned g = line_gpio[l], h = line_gpio[7 + l];      // GPIO 8 (A14) is not in the index
            i |= (uint16_t)(1u << line_gpio[8 + l]);
            lo |= (uint16_t)(1u << (g < 8 ? g : g - 1));
            hi |= (uint16_t)(1u << (h < 8 ? h : h - 1));
        }
        fhi[v] = i;
        blo[v] = lo;
        bhi[v] = hi;
    }
}
