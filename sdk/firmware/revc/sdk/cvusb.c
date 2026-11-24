// cvusb.c -- see cvusb.h.
//
// The pieces, each built only when its feature is on (cvcart_features()):
//
//   the port            here: host or device, and the choice between them
//                       (the SCUMM plugin's usb_choose(), without its wait)
//   PADS                cvpad.c, swpro.c, hidpad.c, as cvpad_use() has them;
//                       built with CVUSB, cvpad.c leaves the port and its
//                       timer to this file and gets the HID interfaces that
//                       aren't a keyboard's or a mouse's
//   KEYBOARD, MOUSE     here, with cvusb_keys.c reading the reports (the
//                       SCUMM plugin's usbinput.c)
//   MIDI_IN             cvusb_midih.c (PSG Synth's midi_host.c)
//   MIDI_OUT            cvusb_midid.c (the SCUMM plugin's usbmidi_dev.c)
//   MIDI either way     cvusb_midi.c, the packets
//
// As a host, everything that touches TinyUSB runs in one context: a timer's
// interrupt every millisecond on the core that called cvusb_start(), as
// cvpad.c's task did. What it leaves for the loop (keys, the mouse, MIDI
// that came in) is taken with interrupts off for a moment.
#include "cvusb.h"

#include <string.h>

#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "tusb.h"

#include "cvusb_pvt.h"
#if CVUSB_PADS
#include "cvpad.h"
#endif
#if CVUSB_MIDI
#include "cvusb_midi.h"
#endif

#if !CVUSB_HOST_FEATURES && !CVUSB_MIDI_OUT
#error "cvusb.c with no USB feature on (cvcart_features() adds it only with one)"
#endif
#if TUSB_VERSION_NUMBER < 2100
#error "cvusb needs the SDK's TinyUSB: include sdk/cvpad_tinyusb.cmake before pico_sdk_import.cmake"
#endif

#define TICK_US  1000           // the host's task, this often
// the choice, as the SCUMM plugin makes it (its MIDI_WAIT_US, MIDI_ASK_US, MIDI_ALLOW_US)
#define WAIT_US  1500000        // a computer on the port configures the device in this (macOS: ~0.3 s)
#define ASK_US   700000         // a computer there, the device not taken yet: it's asking its user
#define ALLOW_US 60000000       // and that user has this long to allow it

static cvusb_mode_t mode;
static uint32_t t_start;
static uint32_t task_max_us;

// ---- what came in, for the loop ------------------------------------------------------------
#if CVUSB_KEYBOARD
#define KEYQ 32
static cvkey_t keyq[KEYQ];
static uint8_t kq_head, kq_tail;

static void push_key(const cvkey_t *e, void *ctx) {
    (void)ctx;
    const uint8_t next = (uint8_t)((kq_tail + 1) % KEYQ);
    if (next == kq_head) return;                    // full: dropped
    keyq[kq_tail] = *e;
    kq_tail = next;
}

bool cvusb_key(cvkey_t *k) {
    const uint32_t irq = save_and_disable_interrupts();
    const bool got = kq_head != kq_tail;
    if (got) {
        *k = keyq[kq_head];
        kq_head = (uint8_t)((kq_head + 1) % KEYQ);
    }
    restore_interrupts(irq);
    return got;
}
#endif

#if CVUSB_MOUSE
static cvmouse_t mouse;

bool cvusb_mouse(cvmouse_t *m) {
    cvusb_status_t s;
    cvusb_status(&s);
    const uint32_t irq = save_and_disable_interrupts();
    *m = mouse;
    mouse.dx = mouse.dy = mouse.wheel = 0;
    restore_interrupts(irq);
    return s.mice != 0;
}
#endif

#if CVUSB_MIDI
#define MIDIQ 64
static struct { uint8_t len, msg[3]; } midiq[MIDIQ];
static uint8_t mq_head, mq_tail;
static uint32_t midi_dropped;

// a packet from the MIDI device (the timer's interrupt) or the computer (the loop)
void cvusb_midi_packet(const uint8_t pkt[4]) {
    uint8_t msg[3];
    const int n = cvmidi_msg(pkt, msg);
    if (!n) return;
    const uint32_t irq = save_and_disable_interrupts();
    const uint8_t next = (uint8_t)((mq_tail + 1) % MIDIQ);
    if (next == mq_head) {
        midi_dropped++;
    } else {
        midiq[mq_tail].len = (uint8_t)n;
        memcpy(midiq[mq_tail].msg, msg, 3);
        mq_tail = next;
    }
    restore_interrupts(irq);
}

int cvusb_midi_read(uint8_t msg[3]) {
    const uint32_t irq = save_and_disable_interrupts();
    int n = 0;
    if (mq_head != mq_tail) {
        n = midiq[mq_head].len;
        memcpy(msg, midiq[mq_head].msg, 3);
        mq_head = (uint8_t)((mq_head + 1) % MIDIQ);
    }
    restore_interrupts(irq);
    return n;
}

uint32_t cvusb_midi_dropped(void) { return midi_dropped; }

bool cvusb_midi_send(const uint8_t *msg, uint32_t n) {
#if CVUSB_MIDI_OUT
    if (mode == CVUSB_DEVICE) {
        if (!cvmidid_mounted()) return false;
        if (cvmidid_write(msg, n)) return true;
        midi_dropped++;
        return false;
    }
#endif
#if CVUSB_MIDI_IN
    if (mode == CVUSB_HOST) {
        static uint8_t status;                      // running status, between calls
        uint8_t pk[64][4], st = status;
        const int k = cvmidi_pack(msg, n, pk, 64, &st);
        if (k < 0) {
            midi_dropped++;
            return false;
        }
        const uint32_t irq = save_and_disable_interrupts();
        const int room = cvmidih_room();
        const bool ok = room >= k;
        if (ok)
            for (int i = 0; i < k; i++) cvmidih_send(pk[i]);
        restore_interrupts(irq);
        if (room < 0) return false;                 // no MIDI device, or one that takes none
        if (!ok) {
            midi_dropped++;
            return false;
        }
        status = st;
        return true;
    }
#endif
    (void)msg;
    (void)n;
    return false;
}
#endif

// ---- the host ------------------------------------------------------------------------------
#if CVUSB_HOST_FEATURES
static repeating_timer_t timer;
static bool timer_on;

#if CVUSB_KEYBOARD || CVUSB_MOUSE
// the keyboards' and mice's HID interfaces (the rest are the pads')
enum { KIND_KEYBOARD = 1, KIND_MOUSE };
static struct {
    uint8_t daddr, idx, kind;           // kind 0: the entry is free
    bool rearm;                         // a receive was refused: ask again in the task
    cvkeys_t keys;
} hid[CFG_TUH_HID];

static int hid_of(uint8_t daddr, uint8_t idx) {
    for (int i = 0; i < CFG_TUH_HID; i++)
        if (hid[i].kind && hid[i].daddr == daddr && hid[i].idx == idx) return i;
    return -1;
}

static int hid_count(uint8_t kind) {
    int k = 0;
    for (int i = 0; i < CFG_TUH_HID; i++) k += hid[i].kind == kind;
    return k;
}
#endif

#if CVUSB_PADS || CVUSB_KEYBOARD || CVUSB_MOUSE
// TinyUSB's HID callbacks, all of them: a keyboard's or a mouse's boot
// interface is read here, and any other HID interface goes to the pads
// (cvpad.c says "NOT A PAD" to what isn't one).
void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc, uint16_t len) {
#if CVUSB_KEYBOARD || CVUSB_MOUSE
    const uint8_t proto = tuh_hid_interface_protocol(daddr, idx);
    uint8_t kind = 0;
    if (CVUSB_KEYBOARD && proto == HID_ITF_PROTOCOL_KEYBOARD) kind = KIND_KEYBOARD;
    if (CVUSB_MOUSE && proto == HID_ITF_PROTOCOL_MOUSE) kind = KIND_MOUSE;
    if (kind) {
        if (hid_of(daddr, idx) >= 0) return;
        for (int i = 0; i < CFG_TUH_HID; i++) {
            if (hid[i].kind) continue;
            memset(&hid[i], 0, sizeof hid[i]);
            hid[i].daddr = daddr;
            hid[i].idx = idx;
            hid[i].kind = kind;
            hid[i].rearm = !tuh_hid_receive_report(daddr, idx);
            return;
        }
        return;
    }
#endif
#if CVUSB_PADS
    cvpad_hid_mount_cb(daddr, idx, desc, len);
#else
    (void)desc;
    (void)len;
#endif
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
#if CVUSB_KEYBOARD || CVUSB_MOUSE
    const int i = hid_of(daddr, idx);
    if (i >= 0) {
#if CVUSB_KEYBOARD
        if (hid[i].kind == KIND_KEYBOARD) cvkeys_release(&hid[i].keys, push_key, NULL);   // what was held, let go
#endif
#if CVUSB_MOUSE
        if (hid[i].kind == KIND_MOUSE) mouse.buttons = 0;
#endif
        memset(&hid[i], 0, sizeof hid[i]);
        return;
    }
#endif
#if CVUSB_PADS
    cvpad_hid_umount_cb(daddr, idx);
#endif
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const *r, uint16_t len) {
#if CVUSB_KEYBOARD || CVUSB_MOUSE
    const int i = hid_of(daddr, idx);
    if (i >= 0) {
#if CVUSB_KEYBOARD
        if (hid[i].kind == KIND_KEYBOARD) cvkeys_report(&hid[i].keys, r, len, push_key, NULL);
#endif
#if CVUSB_MOUSE
        if (hid[i].kind == KIND_MOUSE) cvmouse_report(&mouse, r, len);
#endif
        // the next one: asked for on every path, or the device goes quiet
        hid[i].rearm = !tuh_hid_receive_report(daddr, idx);
        return;
    }
#endif
#if CVUSB_PADS
    cvpad_hid_report_received_cb(daddr, idx, r, len);
#else
    (void)r;
    (void)len;
#endif
}
#endif

// TinyUSB's events, an enumeration's next step, the Switch pads' start,
// the MIDI device's transfers. Nothing in it waits (sdk/tinyusb defers an
// enumeration's delays).
static void host_task(void) {
    const uint32_t t0 = time_us_32();
    tuh_task_ext(0, false);
    // the clock for the pads after TinyUSB's callbacks, which stamp a
    // report with theirs (cvpad.c's task says why)
    const uint32_t t = time_us_32();
#if CVUSB_PADS
    cvpad_poll(t);
#else
    (void)t;
#endif
#if CVUSB_KEYBOARD || CVUSB_MOUSE
    for (int i = 0; i < CFG_TUH_HID; i++)
        if (hid[i].kind && hid[i].rearm) hid[i].rearm = !tuh_hid_receive_report(hid[i].daddr, hid[i].idx);
#endif
#if CVUSB_MIDI_IN
    cvmidih_task();
#endif
    const uint32_t us = time_us_32() - t0;
    if (us > task_max_us) task_max_us = us;
}

static bool on_timer(repeating_timer_t *rt) {
    (void)rt;
    host_task();
    return true;
}

static void host_start(void) {
#if CVUSB_KEYBOARD || CVUSB_MOUSE
    tuh_hid_set_default_protocol(HID_PROTOCOL_BOOT);   // keyboards and mice: their boot reports
#endif
    tusb_rhport_init_t host = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &host);
    mode = CVUSB_HOST;
    timer_on = add_repeating_timer_us(-TICK_US, on_timer, NULL, &timer);
}
#endif

// ---- the port ------------------------------------------------------------------------------
#if CVUSB_MIDI_OUT
static bool taken;                      // a computer has had the device: it stays one
#endif

void cvusb_start(void) {
    if (mode != CVUSB_NONE) return;
    t_start = time_us_32();
#if CVUSB_MIDI_OUT
    cvmidid_start();                    // a device first: a computer takes it at once, a hub never
    mode = CVUSB_WAITING;
#else
    host_start();
#endif
}

void cvusb_host_now(void) {
#if CVUSB_MIDI_OUT && CVUSB_HOST_FEATURES
    if (mode != CVUSB_WAITING && mode != CVUSB_ASKING) return;
    cvmidid_stop();
    host_start();
#endif
}

void cvusb_task(void) {
#if CVUSB_MIDI_OUT
    if (mode == CVUSB_WAITING || mode == CVUSB_ASKING || mode == CVUSB_DEVICE) {
        const uint32_t t0 = time_us_32();
        cvmidid_task();
        if (cvmidid_mounted()) taken = true;
        const uint32_t dt = t0 - t_start;
        const bool host = cvmidid_host_seen();
        mode = taken ? CVUSB_DEVICE : host && dt >= ASK_US ? CVUSB_ASKING : CVUSB_WAITING;
#if CVUSB_HOST_FEATURES
        // nobody took it: a hub there, or nothing; a computer asking its
        // user gets its minute
        if (!taken && dt >= (host ? ALLOW_US : WAIT_US)) cvusb_host_now();
#endif
        return;
    }
#endif
#if CVUSB_HOST_FEATURES
    if (mode == CVUSB_HOST && !timer_on) host_task();      // (no timer to be had: the loop's turn)
#endif
}

cvusb_mode_t cvusb_mode(void) { return mode; }

const char *cvusb_mode_name(cvusb_mode_t m) {
    switch (m) {
    case CVUSB_WAITING: return "LOOKING FOR A COMPUTER";
    case CVUSB_ASKING:  return "ALLOW IT ON THE COMPUTER";
    case CVUSB_DEVICE:  return "ON A COMPUTER";
    case CVUSB_HOST:    return "HOST";
    default:            return "OFF";
    }
}

void cvusb_status(cvusb_status_t *s) {
    memset(s, 0, sizeof *s);
    const uint32_t irq = save_and_disable_interrupts();
    if (mode == CVUSB_HOST) {
#if CVUSB_PADS
        s->pads = (uint8_t)cvpad_count();
#endif
#if CVUSB_KEYBOARD
        s->keyboards = (uint8_t)hid_count(KIND_KEYBOARD);
#endif
#if CVUSB_MOUSE
        s->mice = (uint8_t)hid_count(KIND_MOUSE);
#endif
#if CVUSB_MIDI_IN
        s->midi = cvmidih_mounted();
#endif
    }
#if CVUSB_MIDI_OUT
    if (mode == CVUSB_DEVICE) s->midi = true;
#endif
    restore_interrupts(irq);
}

uint32_t cvusb_task_max_us(void) { return task_max_us; }

#if CVUSB_PADS
bool cvusb_pad(int n, gamepad_t *p) { return cvpad_get(n, p); }
const char *cvusb_pad_kind(int n) { return cvpad_kind(n); }
uint32_t cvusb_pad_dirs(const gamepad_t *p) { return cvpad_dirs(p); }
#endif
