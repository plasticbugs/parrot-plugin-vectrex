// vxpad.h -- the controls: the ColecoVision controller and USB pads as the
// Vectrex's two controllers (an analog stick and four buttons in a row,
// 1 to 4 from the left). Portable, header-only.
//
//                     ColecoVision controller      USB pad
//   the stick         joystick (all the way)       left stick, analog; or the d-pad
//   button 1          keypad 1                     left face button (Nintendo Y)
//   button 2          keypad 2                     top (X)
//   button 3          keypad 3, left fire          bottom (B)
//   button 4          keypad 4, right fire         right (A)
//   the beam's colour keypad 0                     L
//
// The first pad and the console's controller are player 1 together; a
// second pad is player 2. (The kernel reads the console's first controller
// port only.)
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdk/gamepad.h"
#include "vx.h"

// the ColecoVision keypad's codes, as the kernel reports them
enum { VXKEY_0 = 0xA, VXKEY_1 = 0xD, VXKEY_2 = 0x7, VXKEY_3 = 0xC, VXKEY_4 = 0x2, VXKEY_5 = 0x3, VXKEY_6 = 0xE,
       VXKEY_7 = 0x5, VXKEY_8 = 0x1, VXKEY_9 = 0xB, VXKEY_STAR = 0x9, VXKEY_HASH = 0x6, VXKEY_NONE = 0xF };

#define VXPAD_DEAD 160              // a stick this near its middle (of 2047) is at rest

static inline int vxpad_axis(int v) {       // a stick's -2048..2047 as -127..127
    if (v > -VXPAD_DEAD && v < VXPAD_DEAD) return 0;
    v = v > 0 ? v - VXPAD_DEAD : v + VXPAD_DEAD;
    v = v * 127 / (2047 - VXPAD_DEAD);
    return v > 127 ? 127 : v < -127 ? -127 : v;
}

// a pad as a Vectrex controller: its stick into *x, *y, its buttons returned (bits 0-3)
static inline uint8_t vxpad_pad(const gamepad_t *pad, int8_t *x, int8_t *y) {
    uint32_t b = pad->buttons;
    int ax = vxpad_axis(pad->lx), ay = vxpad_axis(pad->ly);
    if (b & GP_RIGHT) ax = 127;
    if (b & GP_LEFT) ax = -127;
    if (b & GP_UP) ay = 127;
    if (b & GP_DOWN) ay = -127;
    *x = (int8_t)ax;
    *y = (int8_t)ay;
    return (uint8_t)((b & GP_WEST ? 1 : 0) | (b & GP_NORTH ? 2 : 0) | (b & GP_SOUTH ? 4 : 0) | (b & GP_EAST ? 8 : 0));
}

// One TV frame's inputs. joy, fire, key: the console's controller as
// cvf_input() gives it (low = pressed; key a VXKEY_ code). pad1, pad2: the
// USB pads' states, or NULL.
static inline void vxpad_frame(uint8_t joy, uint8_t fire, uint8_t key, const gamepad_t *pad1, const gamepad_t *pad2, vx_inputs_t *in) {
    int8_t x = 0, y = 0;
    uint8_t b = 0;
    if (pad1) b = vxpad_pad(pad1, &x, &y);
    if (!(joy & 2)) x = 127;                // N E S W: bits 0-3
    if (!(joy & 8)) x = -127;
    if (!(joy & 1)) y = 127;
    if (!(joy & 4)) y = -127;
    if (!(fire & 1)) b |= 4;
    if (!(fire & 2)) b |= 8;
    if (key == VXKEY_1) b |= 1;
    if (key == VXKEY_2) b |= 2;
    if (key == VXKEY_3) b |= 4;
    if (key == VXKEY_4) b |= 8;
    in->x[0] = x;
    in->y[0] = y;
    in->x[1] = in->y[1] = 0;
    if (pad2) b |= (uint8_t)(vxpad_pad(pad2, &in->x[1], &in->y[1]) << 4);
    in->buttons = b;
}
