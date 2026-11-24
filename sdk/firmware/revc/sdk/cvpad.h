// cvpad -- USB game pads through the cart's USB port, for plugins. The port
// becomes a USB host (TinyUSB), through a powered hub: the cart can't power
// a pad itself. Up to CVPAD_MAX pads at once, each in the slot it took when
// it was plugged in (pad 0 is player 1).
//
//   the Nintendo Switch Pro Controller and pads that act as one (8BitDo's in
//   Switch mode): swpro.c, which starts the pad and reads its full reports
//   plain HID pads (D-input: 8BitDo's SN30 Pro, DualShock 4, Logitech's D
//   mode...): hidpad.c, from their report descriptor
//
// Keyboards and mice are let be (SCUMM's usbinput.c is for those).
//
//   cvpad_init()     once: the port becomes a host, and the pads' task
//                    starts: TinyUSB's work and the Switch pads' start, every
//                    millisecond from a timer's interrupt on the calling
//                    core, so a pad is read at its own rate whatever the
//                    plugin's loop is doing. It never waits, with TinyUSB
//                    from sdk/tinyusb (include sdk/cvpad_tinyusb.cmake
//                    before pico_sdk_import.cmake); the pico-sdk's 0.18
//                    sleeps in it for 450 ms and more each time a device is
//                    plugged in, and can panic (sdk/tinyusb/VENDORED.md).
//   cvpad_task()     nothing now (the timer does it); plugins written
//                    before call it each pass of their loop, harmlessly
//   cvpad_get(n, &p) pad n's state, once it has sent one. A button pressed
//                    and let go between two calls is still down in the
//                    second: a tap shorter than a frame isn't lost.
//
// TinyUSB is run from that timer only: a plugin must not call tuh_* from
// its loop (cvpad_set_tick() gives it a turn in the timer's context).
//
// The build: cvpad_use(target) in CMakeLists.txt (cvplugin.cmake). Its
// TinyUSB configuration is sdk/cvpad_tusb/tusb_config.h, or the plugin's
// own with cvpad_use(target OWN_TUSB_CONFIG).
//
// A new plugin uses cvusb.h instead (cvcart_features(target PADS ...)),
// which has the pads and keyboards, mice and MIDI too. There this file is
// built with CVUSB: cvusb owns the port and the timer, so cvpad_init(),
// cvpad_task(), cvpad_stop(), cvpad_drop(), cvpad_set_tick() and
// cvpad_task_max_us() aren't built, and cvusb_pad() is cvpad_get().
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "gamepad.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CVPAD_MAX 2

void cvpad_init(void);
void cvpad_task(void);
// the port let go of again (the task's timer stopped, the host shut down):
// cvpad_init() starts it afresh. The menu does this; a plugin has no need.
void cvpad_stop(void);

// the task's timer stopped and nothing more (no TinyUSB call): for firmware
// that resets the USB controller itself next (the menu, before a plugin)
void cvpad_drop(void);

// pad n's state (false while there's none in slot n, or it hasn't sent its
// state yet: a Switch pad takes a moment to start)
bool cvpad_get(int n, gamepad_t *p);

// some pad holds all of these buttons (GP_*) now. Unlike cvpad_get() it
// leaves the presses it latches alone: cvk_task() and cvf_task() look with
// it for Select and Start (cvhome.h)
bool cvpad_held(uint32_t buttons);

// pads plugged in now (started or not)
int cvpad_count(void);

// a pad has been in a slot since cvpad_init()
bool cvpad_seen(void);

// the longest a cvpad_task() call has taken so far, in microseconds: what
// a plugin's frame can lose to the pads (for its note to the menu)
uint32_t cvpad_task_max_us(void);

// what's in slot n, for a screen: "SWITCH PRO", "HID PAD", or NULL
const char *cvpad_kind(int n);

// the directions a pad holds (GP_UP, GP_DOWN, GP_LEFT, GP_RIGHT): its
// d-pad's, or with the d-pad at rest its left stick's past about 40% of
// its way; never both of a pair at once
uint32_t cvpad_dirs(const gamepad_t *p);

// ---- for a test's screen (padtest/plugin) and a plugin's notes ------------------
enum { CVPAD_NONE, CVPAD_SWITCH, CVPAD_HID };

typedef struct {
    uint8_t driver;             // CVPAD_*
    uint8_t daddr, idx;         // its device address and HID interface
    uint16_t vid, pid;
    uint16_t desc_len;          // its HID report descriptor's length
    bool live;                  // a state has come
    gamepad_t pad;              // the state now (unlatched)
    // reports: all that came; of them: the pad's state, empty ones (a
    // transfer that failed); by_id: Switch 0x30, 0x21, 0x81, others (a HID
    // pad: those read as its state, and those not)
    uint32_t reports, states, empty, by_id[4];
    uint32_t sent, send_busy;   // reports sent to the pad; tries refused (the endpoint busy)
    uint32_t rearm_failed, rearmed; // receives refused; (the count the task has made good)
    uint32_t mount_us, last_us; // when it was mounted; when its last report came
    uint32_t now_us;            // (the clock when this was read: now_us - last_us is the report's age)
    uint32_t gap_max_us;        // the longest between two reports since cvpad_info_lap()
    uint8_t raw[64];            // the last report as it came, its ID first when it has one
    uint16_t raw_len;
    // a Switch pad's start (swpro.h): the step it is on (SWPRO_*), the
    // tries so far at it, each step's result, the start's length once done
    uint8_t step, tries, result[8];
    uint32_t restarts, start_us;
    bool calibrated;
} cvpad_info_t;

// slot n's pad, as it is now; false: none there
bool cvpad_info(int n, cvpad_info_t *out);
void cvpad_info_lap(int n);     // gap_max_us from zero again

// lines about what happens (a pad mounted or unplugged, a HID interface
// that isn't a pad, the Switch start's troubles); fn runs in the timer's
// interrupt: keep it short. NULL: none.
void cvpad_set_log(void (*fn)(const char *line));

// fn runs after each run of the task, in the timer's interrupt: where a
// plugin may call tuh_* itself
void cvpad_set_tick(void (*fn)(void));

#ifdef __cplusplus
}
#endif
