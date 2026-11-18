// hidpad.h -- plain HID game pads (D-input), read the way their report
// descriptor lays them out.
//
// A pad whose descriptor has a Joystick or Game Pad application collection
// says where each of its controls is in its input report: the sticks (X, Y
// and Z, Rz, or Rx, Ry), the hat (the d-pad) and the buttons, numbered from
// 1. Their place is known that way; what each numbered button is is not, as
// makers number them as they like. So the button numbers go through a table
// chosen by maker:
//
//   8BitDo (2DC8:*), D-input   1 A (east), 2 B (south), 4 X (north),
//                              5 Y (west), 7 L, 8 R, 9 ZL, 10 ZR, 11 minus,
//                              12 plus, 13 home, 14 / 15 stick clicks
//   others                     1 west, 2 south, 3 east, 4 north, 5 L, 6 R,
//                              7 ZL, 8 ZR, 9 minus, 10 plus, 11 / 12 stick
//                              clicks, 13 home, 14 capture (DualShock 4 and
//                              Logitech's D mode number them this way)
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gamepad.h"

enum { HP_X, HP_Y, HP_Z, HP_RX, HP_RY, HP_RZ, HP_HAT, HP_FIELDS };
#define HP_BUTTONS 32

typedef struct {
    uint16_t bit;                   // from the report's start (after its id)
    uint8_t size;                   // bits; 0 = the pad has none
    int32_t lmin, lmax;             // its logical range
} hidpad_field_t;

typedef struct {
    bool has_ids;                   // reports start with their id
    uint8_t report_id;              // the one with the pad's controls
    hidpad_field_t f[HP_FIELDS];
    uint16_t button_bit[HP_BUTTONS];    // button n at button_bit[n - 1]; 0xFFFF: none
    uint8_t buttons;                // the highest button number
    const uint32_t *map;            // button n -> GP_* (map[n - 1])
    const char *layout;             // "8BITDO", "GENERIC"
    gamepad_t pad;
    uint32_t raw_buttons;           // button n at bit n - 1
    int8_t hat;                     // 0-7 from up, clockwise; -1 centred
} hidpad_t;

// Read a report descriptor. True if it is a pad (and h knows its layout).
bool hidpad_parse(hidpad_t *h, const uint8_t *desc, uint16_t len, uint16_t vid);

// A report from the pad (its id first, if it has ids). True if it was the
// one with the controls and h->pad is up to date.
bool hidpad_report(hidpad_t *h, const uint8_t *r, uint16_t len);
