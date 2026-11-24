// cvhome.h -- back to the menu from the controllers, so nobody has to reach
// for the console's RESET: hold both fire buttons and * on controller 1, or
// Select and Start on a USB pad (- and + on a Nintendo pad: GP_MINUS and
// GP_PLUS), for 2 seconds.
//
// cvk_task() and cvf_task() watch for it, so every plugin has it and a
// plugin does nothing. When it has been held long enough, the task function
// writes RST 0 ($C7) over the kernel's NMI vector ($8021): at the next
// vertical blank the BIOS jumps there ($0066 is JP $8021), restarts ($0000),
// and reads the cartridge header, which core 1 takes as RESET. From there
// it is RESET's way back: the handoff, the console parked, the menu. If no
// NMI comes (a list turned the vertical blank interrupt off), every byte of
// the kernel but its 55 AA becomes RST 0 300 ms later.
//
// The plugin sees the buttons as usual while they are held. A plugin that
// uses both fire buttons and * held together for that long (none does yet)
// can turn it off with cvk_home(false) or cvf_home(false).
//
// Plain C, header-only: the firmware and the Mac models (test/host) share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CVHOME_HOLD_US  2000000         // held this long
#define CVHOME_GAP_US   100000          // let go for longer than this and it starts again
#define CVHOME_FORCE_US 300000          // no NMI took the RST 0 this long: RST 0 everywhere
#define CVHOME_NMI      0x8021          // both kernels' NMI vector (the BIOS's $0066 jumps here)
#define CVHOME_PAD      ((1u << 12) | (1u << 13))      // gamepad.h's GP_MINUS | GP_PLUS: Select and Start

typedef struct {
    uint32_t since, last;               // held since, last seen held
    bool on;
} cvhome_t;

// controller 1 as the kernels report it: both fire buttons down and the
// keypad's lines reading * (cvinput.h's cvin_decode_key: lines 9)
static inline bool cvhome_console(bool fire_l, bool fire_r, uint8_t key_lines) {
    return fire_l && fire_r && (key_lines & 15) == 9;
}

// one look at the controllers (held: the combination is down now), at least
// once a frame; true once it has been held for CVHOME_HOLD_US. A gap shorter
// than CVHOME_GAP_US (a frame's report lost on the bus) doesn't restart it.
static inline bool cvhome_step(cvhome_t *h, uint32_t now, bool held) {
    if (held) {
        if (!h->on) {
            h->on = true;
            h->since = now;
        }
        h->last = now;
        return now - h->since >= CVHOME_HOLD_US;
    }
    if (h->on && now - h->last > CVHOME_GAP_US) h->on = false;
    return false;
}
