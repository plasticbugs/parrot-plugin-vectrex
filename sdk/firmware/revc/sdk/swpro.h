// swpro.h -- the Nintendo Switch Pro Controller over USB (057E:2009), and
// pads that act as one (8BitDo's in Switch mode).
//
// Plugged in, the pad says nothing until the host starts it. Its HID
// interface carries reports by their first byte:
//
//   host -> pad   0x80 cmd                    USB commands: 02 handshake,
//                                             03 3 Mbaud to its radio chip,
//                                             04 no timeout (stay on USB)
//                 0x01 n rumble[8] sub args   a subcommand (n counts 0-15)
//   pad -> host   0x81 cmd ...                the reply to a USB command
//                                             (none to 04)
//                 0x21 state[12] ack sub ...  the reply to a subcommand
//                 0x30 state[12] imu[36]      the full report, every 8 ms
//
// The start is the one Linux's hid-nintendo uses: handshake, 3 Mbaud,
// handshake, no timeout; then the sticks' factory calibration from the
// pad's SPI flash (subcommand 10), full reports (03 30), the player 1 light
// (30 01). Each step waits for its reply and is tried 3 times; a step that
// never gets one is skipped, so a pad that does only part of this still
// comes up (and so does one that never takes a report at all, after half a
// second a step). Without a full report for a second it starts over.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gamepad.h"

#define SWPRO_VID 0x057E
#define SWPRO_PID 0x2009

#define SWPRO_CMD_WAIT_US   100000      // a USB command's reply
#define SWPRO_SUB_WAIT_US   150000      // a subcommand's
#define SWPRO_TRIES         3
#define SWPRO_QUIET_US      1000000     // no full report this long: start over
#define SWPRO_SEND_US       500000      // a step that can't be sent this long is skipped
#define SWPRO_SKIPPED       0x80        // in result[]

enum {
    SWPRO_HANDSHAKE, SWPRO_BAUD, SWPRO_HANDSHAKE2, SWPRO_NO_TIMEOUT,
    SWPRO_CAL_LEFT, SWPRO_CAL_RIGHT, SWPRO_MODE, SWPRO_LIGHT, SWPRO_RUN,
};

// Sends a report on the pad's interrupt OUT endpoint: id, then len bytes.
// False if it can't now (the endpoint is busy); it is tried again later.
typedef bool (*swpro_send_fn)(void *ctx, uint8_t id, const uint8_t *data, uint16_t len);

typedef struct { int16_t centre, below, above; } swpro_axis_t;   // raw centre; the span each side

typedef struct {
    int step;                   // SWPRO_*
    bool waiting;               // the step's report is out
    bool skipping;              // the step is being given up
    uint32_t step_us, sent_us;  // when the step began; when its report went
    uint32_t run_us;            // when RUN began (or the last full report)
    uint8_t tries, counter;
    swpro_axis_t axis[4];       // lx, ly, rx, ry
    bool calibrated[2];         // left, right: from the pad (else the defaults)
    gamepad_t pad;
    uint32_t reports, restarts; // full reports so far; times started over
    // how each step of the start went, for a screen: the tries it took
    // (0: not reached yet), | SWPRO_SKIPPED when it got no reply or couldn't
    // be sent; and how long the start took
    uint8_t result[8];
    uint32_t init_us, ready_us; // when the start began; when RUN began (0: not yet)
    void (*log)(const char *line);  // may be NULL
} swpro_t;

bool swpro_matches(uint16_t vid, uint16_t pid);

void swpro_init(swpro_t *s, uint32_t now_us);

// Send what's due. Call often.
void swpro_task(swpro_t *s, uint32_t now_us, swpro_send_fn send, void *ctx);

// A report from the pad, its id first. True if it was the pad's state
// (0x30 or 0x21) and s->pad is up to date; false for anything else.
bool swpro_report(swpro_t *s, const uint8_t *r, uint16_t len, uint32_t now_us);

// What it is doing, for the screen: "HANDSHAKE", ..., "READY".
const char *swpro_step_name(int step);

// The sticks: a raw value (0-4095) on an axis, calibrated.
int16_t swpro_axis(const swpro_axis_t *a, int raw);
