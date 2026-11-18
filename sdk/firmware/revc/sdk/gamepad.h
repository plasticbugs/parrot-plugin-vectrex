// gamepad.h -- a USB pad's state, the same whichever driver read it.
//
// Buttons are named by where they are, not what they're labelled, as the
// labels move between makers: SOUTH is the bottom face button (B on a
// Nintendo pad, A on an Xbox one), EAST the right one (Nintendo A), WEST the
// left (Nintendo Y), NORTH the top (Nintendo X). Sticks are centred on 0,
// -2048..2047, x right and y up.
//
// Plain C: the firmware and the Mac tests share it.
#pragma once
#include <stdint.h>

enum {
    GP_UP      = 1u << 0,  GP_DOWN    = 1u << 1,  GP_LEFT  = 1u << 2,  GP_RIGHT   = 1u << 3,
    GP_SOUTH   = 1u << 4,  GP_EAST    = 1u << 5,  GP_WEST  = 1u << 6,  GP_NORTH   = 1u << 7,
    GP_L       = 1u << 8,  GP_R       = 1u << 9,  GP_ZL    = 1u << 10, GP_ZR      = 1u << 11,
    GP_MINUS   = 1u << 12, GP_PLUS    = 1u << 13, GP_HOME  = 1u << 14, GP_CAPTURE = 1u << 15,
    GP_LSTICK  = 1u << 16, GP_RSTICK  = 1u << 17,
};
#define GP_BUTTONS 18

#define GP_STICK_MAX 2047

typedef struct {
    uint32_t buttons;               // GP_*, 1 = held
    int16_t lx, ly, rx, ry;         // calibrated, -2048..2047, y up
    uint16_t raw[4];                // the sticks as the pad sent them: lx, ly, rx, ry
} gamepad_t;
