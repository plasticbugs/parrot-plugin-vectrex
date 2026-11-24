// cvusb_keys.h -- USB keyboards and mice in their boot protocol, for
// cvusb.c: the reports turned into key presses and releases, and mouse
// movement. From the SCUMM plugin's usbinput.c (../scumm/plugin), with the
// reading split from TinyUSB so the Mac tests can run it.
//
// The boot protocol is the plain report every USB keyboard and mouse can
// send (a PC's BIOS reads them this way), so one reader fits all makes:
//
//   keyboard  8 bytes: modifiers (a bit each: left ctrl, shift, alt, GUI,
//             then the right ones), a reserved byte, and up to 6 keys
//             held, by their HID usage (4 'a' ... 0x27 '0'; 1-3 are the
//             keyboard's errors: too many keys at once)
//   mouse     3 bytes or more: buttons (bit 0 left, 1 right, 2 middle),
//             x and y moved (signed, y down), and on most mice a fourth,
//             the wheel (signed, + away from you)
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// a key pressed or let go
typedef struct {
    uint8_t usage;              // the key, by its HID usage (the USB standard's numbering)
    uint8_t mods;               // the modifiers then: CVKEY_CTRL ... (HID's bits)
    char ascii;                 // its character on a US keyboard, with Shift; 0 for none
    bool down;                  // pressed (true) or let go
} cvkey_t;

#define CVKEY_CTRL  0x11        // left or right
#define CVKEY_SHIFT 0x22
#define CVKEY_ALT   0x44
#define CVKEY_GUI   0x88        // Windows, Command

// one keyboard's state, for telling what changed between its reports
typedef struct {
    uint8_t keys[6];
    uint8_t mods;
} cvkeys_t;

// The changes since the last report: each is passed to emit (released keys
// first). A report of the keyboard's errors (too many keys) changes
// nothing; a short one is ignored.
void cvkeys_report(cvkeys_t *k, const uint8_t *r, uint16_t len, void (*emit)(const cvkey_t *e, void *ctx), void *ctx);

// The keyboard unplugged: every key still held, let go.
void cvkeys_release(cvkeys_t *k, void (*emit)(const cvkey_t *e, void *ctx), void *ctx);

// A key's character on a US keyboard (Shift held or not): letters, digits,
// punctuation, space, Enter '\r', Tab '\t', Backspace '\b', Escape 27,
// Delete 127; 0 for the rest (arrows, F-keys).
char cvkey_ascii(uint8_t usage, bool shift);

// the mice, added up between two reads
typedef struct {
    int32_t dx, dy, wheel;      // counts moved since the last read: x right, y down, the wheel + away
    uint8_t buttons;            // held now: bit 0 left, 1 right, 2 middle
} cvmouse_t;

// A mouse report, added in. False if it's too short to be one.
bool cvmouse_report(cvmouse_t *m, const uint8_t *r, uint16_t len);

#ifdef __cplusplus
}
#endif
