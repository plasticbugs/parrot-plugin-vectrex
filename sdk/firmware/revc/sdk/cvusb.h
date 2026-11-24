// cvusb.h -- the cart's USB port for a plugin, each feature a switch.
//
// A plugin turns on what it wants in its CMakeLists.txt:
//
//   cvcart_features(myplugin PADS KEYBOARD MOUSE MIDI_IN MIDI_OUT)
//
// and only that code is linked, TinyUSB is configured with only those
// classes (cvusb_tusb/tusb_config.h), and the .CVP says what it uses
// (usb-host, usb-device). The features:
//
//   PADS      up to two game pads (gamepad.h): the Switch Pro Controller and
//             pads that act as one, and plain HID pads (cvpad.c)
//   KEYBOARD  USB keyboards: each key pressed and let go, with its character
//   MOUSE     USB mice: movement, buttons and the wheel
//   MIDI_IN   a USB MIDI keyboard or other MIDI device (PSG Synth's driver)
//   MIDI_OUT  the cart as a USB MIDI device on a computer (the SCUMM
//             plugin's), so a synth there plays what the plugin sends
//
// The port is a host or a device, never both at once:
//
//   - PADS, KEYBOARD, MOUSE and MIDI_IN make it a host. Those devices go in
//     a powered USB hub (the cart can't power them), and the hub in the cart.
//   - MIDI_OUT makes it a device: the cart plugged into a computer.
//   - With both, the port tries the computer first, as the SCUMM plugin
//     does: if one takes the cart within 1.5 s it stays a device; if not
//     (a hub, or nothing), it becomes a host. A computer that answers but
//     hasn't taken the cart yet is asking its user (macOS, the first time:
//     "Allow accessory to connect?"): that is waited for, up to a minute,
//     and the mode is CVUSB_ASKING meanwhile. The choice is made once, at
//     the start: plug in before the plugin starts.
//
// MIDI goes both ways on whatever MIDI there is: cvusb_midi_read() gets what
// a MIDI keyboard plays (host) or what the computer sends (device), and
// cvusb_midi_send() goes to the computer (device) or into the MIDI device,
// if it takes MIDI (host).
//
// The C side: cvusb_start() once, cvusb_task() every pass of the loop, and
// the getters when the plugin wants to know. As a host, TinyUSB runs every
// millisecond from a timer's interrupt (as cvpad.c's did), so the inputs are
// read at their own rate whatever the loop does; never call tuh_* yourself.
// As a device, TinyUSB runs in cvusb_task().
//
// Calling a getter for a feature that is off is a build error, which names
// the switch to turn on.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "cvusb_keys.h"
#include "gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define CVUSB_NEEDS(what) __attribute__((error("turn on " what " in cvcart_features() (CMakeLists.txt)")))
#else
#define CVUSB_NEEDS(what)
#endif

#ifndef CVUSB_PADS
#define CVUSB_PADS 0
#endif
#ifndef CVUSB_KEYBOARD
#define CVUSB_KEYBOARD 0
#endif
#ifndef CVUSB_MOUSE
#define CVUSB_MOUSE 0
#endif
#ifndef CVUSB_MIDI_IN
#define CVUSB_MIDI_IN 0
#endif
#ifndef CVUSB_MIDI_OUT
#define CVUSB_MIDI_OUT 0
#endif
#define CVUSB_HOST_FEATURES (CVUSB_PADS || CVUSB_KEYBOARD || CVUSB_MOUSE || CVUSB_MIDI_IN)
#define CVUSB_MIDI          (CVUSB_MIDI_IN || CVUSB_MIDI_OUT)

// a getter whose feature is off: a build error if it's called
#if CVUSB_PADS
#define CVUSB_IF_PADS
#else
#define CVUSB_IF_PADS CVUSB_NEEDS("PADS")
#endif
#if CVUSB_KEYBOARD
#define CVUSB_IF_KEYBOARD
#else
#define CVUSB_IF_KEYBOARD CVUSB_NEEDS("KEYBOARD")
#endif
#if CVUSB_MOUSE
#define CVUSB_IF_MOUSE
#else
#define CVUSB_IF_MOUSE CVUSB_NEEDS("MOUSE")
#endif
#if CVUSB_MIDI
#define CVUSB_IF_MIDI
#else
#define CVUSB_IF_MIDI CVUSB_NEEDS("MIDI_IN or MIDI_OUT")
#endif

// ---- the port ----------------------------------------------------------------------
// Once, after cvk_start() or cvf_start().
void cvusb_start(void);

// Every pass of the plugin's loop. It never waits.
void cvusb_task(void);

typedef enum {
    CVUSB_NONE,         // not started
    CVUSB_WAITING,      // a device, no computer yet (with host features: for 1.5 s, then a host)
    CVUSB_ASKING,       // a device: a computer answered and is asking its user to allow the cart
    CVUSB_DEVICE,       // a device: a computer has taken the cart
    CVUSB_HOST,         // a host: pads, keyboards, mice, MIDI devices, through a powered hub
} cvusb_mode_t;

cvusb_mode_t cvusb_mode(void);

// The mode, for a screen: "OFF", "LOOKING FOR A COMPUTER", "ALLOW IT ON THE
// COMPUTER", "ON A COMPUTER", "HOST".
const char *cvusb_mode_name(cvusb_mode_t mode);

// While waiting for a computer: stop, and be a host now (with host features
// only; a plugin can offer this on a button while CVUSB_ASKING).
void cvusb_host_now(void);

// What is plugged in now.
typedef struct {
    uint8_t pads;           // game pads (started or not)
    uint8_t keyboards;
    uint8_t mice;
    bool midi;              // a MIDI device (host), or a computer that has taken the cart (device)
} cvusb_status_t;

void cvusb_status(cvusb_status_t *s);

// The longest the host's task has taken so far, in microseconds: what the
// plugin's loop can lose to it.
uint32_t cvusb_task_max_us(void);

// ---- PADS ----------------------------------------------------------------------------
#define CVUSB_PAD_MAX 2

// Pad n's state (0: player 1, the first plugged in): buttons named by where
// they are (GP_SOUTH, the bottom face button, whatever its label), sticks
// -2048..2047 with y up. False while there's none in slot n, or it hasn't
// sent its state yet (a Switch pad takes a moment to start). A button
// pressed and let go between two calls still shows in the second: ask once
// a frame.
bool cvusb_pad(int n, gamepad_t *p) CVUSB_IF_PADS;

// What is in slot n, for a screen: "SWITCH PRO", "HID PAD", or NULL.
const char *cvusb_pad_kind(int n) CVUSB_IF_PADS;

// The directions a pad holds (GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT): its
// d-pad's, or with the d-pad at rest its left stick's past about 40% of
// its way; never both of a pair at once.
uint32_t cvusb_pad_dirs(const gamepad_t *p) CVUSB_IF_PADS;

// ---- KEYBOARD ------------------------------------------------------------------------
// The next key pressed or let go, oldest first (cvusb_keys.h: its HID usage,
// the modifiers, its character on a US keyboard, down or up). False when
// there's none. Up to 32 wait; more are dropped.
bool cvusb_key(cvkey_t *k) CVUSB_IF_KEYBOARD;

// ---- MOUSE ---------------------------------------------------------------------------
// The mice's movement since the last call, and their buttons now
// (cvusb_keys.h: dx right, dy down, the wheel + away from you, buttons bit 0
// left, 1 right, 2 middle). False while no mouse is plugged in. The wheel
// is there if the mouse sends it in its boot reports, as most do.
bool cvusb_mouse(cvmouse_t *m) CVUSB_IF_MOUSE;

// ---- MIDI_IN, MIDI_OUT ---------------------------------------------------------------
// The next MIDI message that came in: its bytes in msg, status first
// (0x90 note on, channel 1, ...), and their count, 1 to 3; 0 when there's
// none. SysEx isn't passed on. Up to 64 wait; more are dropped.
int cvusb_midi_read(uint8_t msg[3]) CVUSB_IF_MIDI;

// Send MIDI: whole messages (a SysEx from F0 to F7 too). False when there is
// nowhere to send it (no computer, or a MIDI device that takes none) or no
// room for it now: as a device 4K wait (CVUSB_MIDI_OUT_QUEUE, a power of 2,
// changes it), as a host 64 packets of 3 bytes.
bool cvusb_midi_send(const uint8_t *msg, uint32_t n) CVUSB_IF_MIDI;

// Messages cvusb_midi_send() turned away (no room), and messages that came
// in with no room to keep them.
uint32_t cvusb_midi_dropped(void) CVUSB_IF_MIDI;

#ifdef __cplusplus
}
#endif
