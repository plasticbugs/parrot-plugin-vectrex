// pad.h -- controller 1, as the kernel reports it to the cart.
//
// Once a frame midihello.asm reads the joystick and the keypad and then
// reads ROM at an address that carries what it read:
//
//   $FFC0 | lines | fire << 4    joystick: lines N E S W, the left fire button
//   $FFE0 | lines | fire << 4    keypad: the 4 code lines, the right fire button
//
// all as read: low = pressed. The cart sees the address (A0-A5) and hands
// it to pad_report(). Plain C: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PAD_REPORT_BASE  0xFFC0     // A5 = 0 joystick, 1 keypad
#define PAD_KEYPAD       0x20

enum { PAD_STAR = 10, PAD_HASH = 11 };

#define PAD_BPM_MIN      30
#define PAD_BPM_MAX      300

// The keypad's 4 lines as read -> 0-9, PAD_STAR, PAD_HASH; -1 for no key
// (or two at once).
int pad_key(uint8_t lines);

typedef struct {
    uint8_t raw[2];             // the last report of each kind
    uint8_t joy;                // accepted: bit 0 up, 1 right, 2 down, 3 left, 4 fire (1 = pressed)
    int8_t key;                 // accepted key, -1 none
    uint32_t repeat_at;         // when a held direction acts again
    int bpm;                    // the tempo the stick sets
    bool running;               // the clock's play/stop, toggled by the left fire button
    int8_t pressed;             // a key that just went down (-1 none); the caller clears it
} pad_t;

void pad_init(pad_t *p, int bpm, bool running);

// One report (the read's A0-A5) at now_us. A report counts once it comes
// twice in a row (two frames): that rides out a bad sample and a key's
// bounce. The stick moves the tempo by 1 (up, down) or 10 (right, left),
// again every 100 ms once held for 400 ms. Returns true when bpm, running
// or pressed changed.
bool pad_report(pad_t *p, uint8_t a, uint32_t now_us);
