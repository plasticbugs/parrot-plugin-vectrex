// cvinput.h -- player 1's controls as one: the console's own controller and,
// with PADS on, USB pad 1, so a plugin gets USB pads for nothing. With
// either kernel:
//
//   cvin_t in;  cvin_init(&in);
//   screen kernel (cvkernel):  while (cvk_report(t, &kind, &bits)) {
//                                  ... cvs_report() for HELLO and ACK ...
//                                  cvin_report(&in, kind, bits);
//                              }
//   frame kernel (cvframes):   cvf_input(&joy, &fire, &key);
//                              cvin_frame(&in, joy, fire, key);
//   then, once a frame:        cvin_update(&in);
//                              in.held          CVIN_UP | CVIN_FIRE_L ...: down now
//                              in.key           the keypad key held: 0-9, CVIN_STAR, CVIN_HASH, -1 none
//                              cvin_pressed(&in)   what went down since it last asked (each press once)
//                              cvin_key(&in)       the key that went down since it last asked, or -1
//
// A USB pad counts as the console's controller does in the cart's menu:
// the d-pad, or the left stick past about 40% of its way, is the stick;
// the bottom face button (GP_SOUTH: B on a Nintendo pad) the left fire, the
// right one (GP_EAST: A) the right fire; the top one (GP_NORTH: X) the
// keypad's *, the left one (GP_WEST: Y) its #. The pad's own state, all of
// it, is still cvusb_pad()'s.
//
// The screen kernel's reports count once they have come twice in a row
// (two frames, 33 ms): that rides out a contact's bounce and a bad sample on
// the bus. The frame kernel's controller is taken as the frame read it, as
// the cart's emulators take it.
//
// Plain C, header-only (it follows CVUSB_PADS): the plugin and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../midihello/mailbox.h"      // REPORT_KIND_*
#include "cvusb.h"                      // CVUSB_PADS, cvusb_pad()

enum {
    CVIN_UP = 1, CVIN_RIGHT = 2, CVIN_DOWN = 4, CVIN_LEFT = 8,
    CVIN_FIRE_L = 16,           // the left fire button
    CVIN_FIRE_R = 32,           // the right one
};
enum { CVIN_STAR = 10, CVIN_HASH = 11 };

typedef struct {
    uint8_t held;               // CVIN_*, 1 = down: the console's and the pad's together
    int8_t key;                 // the key held, -1 none
    uint8_t pressed;            // went down, not yet asked for
    int8_t key_pressed;         // went down, not yet asked for (-1 none)
    // each side's own
    uint8_t console_held;       // the console's controller
    int8_t console_key;
    uint8_t pad_held;           // USB pad 1, as the console's controller (0 with PADS off)
    int8_t pad_key;
    uint8_t raw[2];             // the screen kernel's last report of each kind, as it came
} cvin_t;

static inline void cvin_init(cvin_t *in) {
    in->held = in->pressed = in->console_held = in->pad_held = 0;
    in->key = in->key_pressed = in->console_key = in->pad_key = -1;
    in->raw[0] = in->raw[1] = 0xFF;
}

// the keypad's 4 lines as the console reads them -> the key (two keys at once read as none)
static inline int cvin_decode_key(uint8_t lines) {
    static const int8_t key[16] = {-1, 8, 4, 5, -1, 7, CVIN_HASH, 2, -1, CVIN_STAR, 0, 9, 3, 1, 6, -1};
    return key[lines & 15];
}

// The screen kernel: one report from cvk_report() (HELLO and ACK are
// ignored). True when the console's controller changed.
static inline bool cvin_report(cvin_t *in, int kind, uint8_t bits) {
    if (kind != REPORT_KIND_JOY && kind != REPORT_KIND_KEY) return false;
    const int k = kind == REPORT_KIND_KEY;
    const uint8_t v = bits & 0x1F;              // low = pressed
    if (v != in->raw[k]) {                      // new: it counts if the next one agrees
        in->raw[k] = v;
        return false;
    }
    const uint8_t held = in->console_held;
    const int8_t key = in->console_key;
    if (k) {                                    // the keypad's 4 lines, and the right fire
        in->console_key = (int8_t)cvin_decode_key(v);
        in->console_held = (uint8_t)((held & ~CVIN_FIRE_R) | (v & 0x10 ? 0 : CVIN_FIRE_R));
    } else {                                    // N E S W and the left fire, in CVIN_*'s order
        in->console_held = (uint8_t)((held & CVIN_FIRE_R) | (~v & 0x1F));
    }
    return in->console_held != held || in->console_key != key;
}

// The frame kernel: what cvf_input() read this frame (low = pressed).
static inline void cvin_frame(cvin_t *in, uint8_t joy, uint8_t fire, uint8_t key) {
    in->console_held = (uint8_t)((~joy & 0x0F) | (fire & 1 ? 0 : CVIN_FIRE_L) | (fire & 2 ? 0 : CVIN_FIRE_R));
    in->console_key = (int8_t)cvin_decode_key(key);
}

// Once a frame: USB pad 1 in (with PADS), the two together, and what went down.
static inline void cvin_update(cvin_t *in) {
    in->pad_held = 0;
    in->pad_key = -1;
#if CVUSB_PADS
    gamepad_t p;
    if (cvusb_pad(0, &p)) {
        const uint32_t d = cvusb_pad_dirs(&p);
        in->pad_held = (uint8_t)((d & GP_UP ? CVIN_UP : 0) | (d & GP_RIGHT ? CVIN_RIGHT : 0) |
                                 (d & GP_DOWN ? CVIN_DOWN : 0) | (d & GP_LEFT ? CVIN_LEFT : 0) |
                                 (p.buttons & GP_SOUTH ? CVIN_FIRE_L : 0) | (p.buttons & GP_EAST ? CVIN_FIRE_R : 0));
        in->pad_key = p.buttons & GP_NORTH ? CVIN_STAR : p.buttons & GP_WEST ? CVIN_HASH : -1;
    }
#endif
    const uint8_t held = (uint8_t)(in->console_held | in->pad_held);
    const int8_t key = in->console_key >= 0 ? in->console_key : in->pad_key;
    in->pressed |= (uint8_t)(held & ~in->held);
    if (key >= 0 && key != in->key) in->key_pressed = key;
    in->held = held;
    in->key = key;
}

static inline uint8_t cvin_pressed(cvin_t *in) {
    const uint8_t v = in->pressed;
    in->pressed = 0;
    return v;
}

static inline int cvin_key(cvin_t *in) {
    const int k = in->key_pressed;
    in->key_pressed = -1;
    return k;
}
