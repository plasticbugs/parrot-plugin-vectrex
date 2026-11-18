// cvpad.c: see cvpad.h.
#include "cvpad.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include "tusb.h"

#include "hidpad.h"
#include "swpro.h"

#define STICK_DIR 820                   // of GP_STICK_MAX (2047): a direction held past ~40%
#define TICK_US   1000                  // the pads' task runs this often, from a timer

typedef struct {
    uint8_t driver;                     // CVPAD_*; CVPAD_NONE: the slot is free
    uint8_t daddr, idx;                 // the pad's device and HID interface
    bool live;                          // its state has come
    uint32_t latched;                   // buttons seen down since the last cvpad_get()
    swpro_t sw;
    hidpad_t hp;
    cvpad_info_t info;                  // (its counters; the rest is filled in when asked)
} slot_t;

static slot_t pads[CVPAD_MAX];
static uint32_t task_max_us;            // the longest run of the task so far
static bool seen;                       // a pad has been in a slot
static void (*log_fn)(const char *line);
static void (*tick_fn)(void);
#if TUSB_VERSION_NUMBER >= 2100
static repeating_timer_t timer;
#endif
static bool timer_on;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    if (!log_fn) return;
    char line[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    log_fn(line);
}

static slot_t *slot_of(uint8_t daddr, uint8_t idx) {
    for (int n = 0; n < CVPAD_MAX; n++)
        if (pads[n].driver != CVPAD_NONE && pads[n].daddr == daddr && pads[n].idx == idx) return &pads[n];
    return NULL;
}

void tuh_hid_mount_cb(uint8_t daddr, uint8_t idx, uint8_t const *desc, uint16_t len) {
    if (slot_of(daddr, idx)) return;
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(daddr, &vid, &pid);
    slot_t *s = NULL;
    for (int n = 0; n < CVPAD_MAX && !s; n++)
        if (pads[n].driver == CVPAD_NONE) s = &pads[n];
    if (!s) {                           // both slots taken
        say("HID %u.%u %04X:%04X NO SLOT", daddr, idx, vid, pid);
        return;
    }
    memset(s, 0, sizeof *s);
    const uint32_t t = time_us_32();
    if (swpro_matches(vid, pid)) {
        s->sw.log = log_fn;
        swpro_init(&s->sw, t);
        s->driver = CVPAD_SWITCH;
    } else if (hidpad_parse(&s->hp, desc, len, vid)) {
        s->driver = CVPAD_HID;
    } else {                            // a keyboard, a mouse, a pad's other interface
        say("HID %u.%u D%u NOT A PAD", daddr, idx, len);
        return;
    }
    s->daddr = daddr;
    s->idx = idx;
    s->info.vid = vid;
    s->info.pid = pid;
    s->info.desc_len = len;
    s->info.mount_us = s->info.last_us = t;
    seen = true;
    say("PAD%d %u.%u D%u %s", (int)(s - pads) + 1, daddr, idx, len, s->driver == CVPAD_SWITCH ? "SWITCH" : "HID");
    if (!tuh_hid_receive_report(daddr, idx)) s->info.rearm_failed++;
}

void tuh_hid_umount_cb(uint8_t daddr, uint8_t idx) {
    slot_t *s = slot_of(daddr, idx);
    if (!s) return;
    say("PAD%d UNPLUGGED", (int)(s - pads) + 1);
    memset(s, 0, sizeof *s);
}

void tuh_hid_report_received_cb(uint8_t daddr, uint8_t idx, uint8_t const *r, uint16_t len) {
    slot_t *s = slot_of(daddr, idx);
    if (s) {
        const uint32_t t = time_us_32();
        cvpad_info_t *i = &s->info;
        const uint32_t gap = t - i->last_us;
        if (i->reports && gap > i->gap_max_us) i->gap_max_us = gap;
        i->last_us = t;
        i->reports++;
        bool state = false;
        if (!len) {
            i->empty++;                 // a transfer that failed: nothing to read
        } else {
            i->raw_len = len < sizeof i->raw ? len : sizeof i->raw;
            memcpy(i->raw, r, i->raw_len);
            if (s->driver == CVPAD_SWITCH) {
                i->by_id[r[0] == 0x30 ? 0 : r[0] == 0x21 ? 1 : r[0] == 0x81 ? 2 : 3]++;
                state = swpro_report(&s->sw, r, len, t);
            } else {
                state = hidpad_report(&s->hp, r, len);
                i->by_id[state ? 0 : 3]++;
            }
        }
        if (state) {
            s->live = true;
            i->states++;
            s->latched |= s->driver == CVPAD_SWITCH ? s->sw.pad.buttons : s->hp.pad.buttons;
        }
    }
    // the next one: asked for here on every path, or the pad goes quiet
    // (task() asks again if this is refused)
    if (!tuh_hid_receive_report(daddr, idx) && s) s->info.rearm_failed++;
}

static bool send(void *ctx, uint8_t id, const uint8_t *data, uint16_t len) {
    slot_t *s = ctx;
    if (tuh_hid_send_ready(s->daddr, s->idx) && tuh_hid_send_report(s->daddr, s->idx, id, data, len)) {
        s->info.sent++;
        return true;
    }
    s->info.send_busy++;
    return false;
}

// The task: TinyUSB's events, an enumeration's next step, the Switch pads'
// start. Nothing in it waits (sdk/tinyusb's TinyUSB defers an enumeration's
// delays; the pico-sdk's 0.18 sleeps through them, 450 ms at a time).
static void task(void) {
    const uint32_t t0 = time_us_32();
    tuh_task_ext(0, false);
    // The clock for swpro_task() is read after TinyUSB's callbacks, which
    // stamp a report with theirs: with an earlier one, "now - the last
    // report" wraps to four thousand seconds and the pad is started over
    // each time a report and the task meet (NES 0.4.3 and 0.4.4 did that:
    // "NO REPORTS FOR 1 S" a moment after READY, for ever).
    const uint32_t t = time_us_32();
    for (int n = 0; n < CVPAD_MAX; n++) {
        slot_t *s = &pads[n];
        if (s->driver == CVPAD_SWITCH) swpro_task(&s->sw, t, send, s);
        // a receive that was refused (the endpoint still claimed): again
        if (s->driver != CVPAD_NONE && s->info.rearm_failed != s->info.rearmed &&
            tuh_hid_receive_report(s->daddr, s->idx))
            s->info.rearmed = s->info.rearm_failed;
    }
    if (tick_fn) tick_fn();
    const uint32_t us = time_us_32() - t0;
    if (us > task_max_us) task_max_us = us;
}

#if TUSB_VERSION_NUMBER >= 2100
static bool on_timer(repeating_timer_t *rt) {
    (void)rt;
    task();
    return true;
}
#endif

void cvpad_init(void) {
    memset(pads, 0, sizeof pads);
    tusb_rhport_init_t host = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &host);
    // The task every millisecond, from a timer's interrupt on this core, so
    // the pads are read at their own rate (a Switch Pro reports every 8 ms)
    // whatever the plugin's loop is doing: an emulator's frame is 10 ms
    // with no call in it. Everything that touches TinyUSB runs in that one
    // context (never call tuh_* from the plugin's loop).
    // (Not with the pico-sdk's TinyUSB 0.18: its enumeration sleeps, which
    // an interrupt can't. There the plugin's loop calls cvpad_task().)
#if TUSB_VERSION_NUMBER >= 2100
    timer_on = add_repeating_timer_us(-TICK_US, on_timer, NULL, &timer);
#endif
}

// The port let go of (the menu, which is a USB device again when no pad is
// there): the task's timer stopped, then TinyUSB's host. Not from the
// timer's interrupt.
void cvpad_stop(void) {
#if TUSB_VERSION_NUMBER >= 2100
    if (timer_on) cancel_repeating_timer(&timer);
#endif
    timer_on = false;
    if (tuh_inited()) tuh_deinit(0);
    memset(pads, 0, sizeof pads);
}

void cvpad_drop(void) {
#if TUSB_VERSION_NUMBER >= 2100
    if (timer_on) cancel_repeating_timer(&timer);
#endif
    timer_on = false;
}

void cvpad_task(void) {
    if (!timer_on && tuh_inited()) task();  // (no timer to be had: the caller's loop it is)
}

uint32_t cvpad_task_max_us(void) { return task_max_us; }

bool cvpad_seen(void) { return seen; }

void cvpad_set_log(void (*fn)(const char *line)) { log_fn = fn; }

void cvpad_set_tick(void (*fn)(void)) { tick_fn = fn; }

bool cvpad_get(int n, gamepad_t *p) {
    if (n < 0 || n >= CVPAD_MAX) return false;
    slot_t *s = &pads[n];
    const uint32_t irq = save_and_disable_interrupts();     // the timer's task writes them
    const bool ok = s->driver != CVPAD_NONE && s->live;
    if (ok) {
        *p = s->driver == CVPAD_SWITCH ? s->sw.pad : s->hp.pad;
        // a button pressed and let go since the last call still counts once
        p->buttons |= s->latched;
        s->latched = 0;
    }
    restore_interrupts(irq);
    return ok;
}

bool cvpad_info(int n, cvpad_info_t *out) {
    if (n < 0 || n >= CVPAD_MAX) return false;
    slot_t *s = &pads[n];
    const uint32_t irq = save_and_disable_interrupts();
    const bool ok = s->driver != CVPAD_NONE;
    if (ok) {
        *out = s->info;
        out->driver = s->driver;
        out->daddr = s->daddr;
        out->idx = s->idx;
        out->live = s->live;
        out->pad = s->driver == CVPAD_SWITCH ? s->sw.pad : s->hp.pad;
        out->now_us = time_us_32();
        if (s->driver == CVPAD_SWITCH) {
            out->step = (uint8_t)s->sw.step;
            out->tries = s->sw.tries;
            out->restarts = s->sw.restarts;
            memcpy(out->result, s->sw.result, sizeof out->result);
            out->start_us = s->sw.ready_us ? s->sw.ready_us - s->sw.init_us : 0;
            out->calibrated = s->sw.calibrated[0] && s->sw.calibrated[1];
        }
    }
    restore_interrupts(irq);
    return ok;
}

void cvpad_info_lap(int n) {
    if (n >= 0 && n < CVPAD_MAX) pads[n].info.gap_max_us = 0;
}

int cvpad_count(void) {
    int k = 0;
    for (int n = 0; n < CVPAD_MAX; n++) k += pads[n].driver != CVPAD_NONE;
    return k;
}

const char *cvpad_kind(int n) {
    if (n < 0 || n >= CVPAD_MAX) return NULL;
    return pads[n].driver == CVPAD_SWITCH ? "SWITCH PRO" : pads[n].driver == CVPAD_HID ? "HID PAD" : NULL;
}

// The d-pad when it holds a direction, else the left stick: a stick resting
// off its centre (or one that never gave its calibration) can't cancel or
// add to a d-pad press.
uint32_t cvpad_dirs(const gamepad_t *p) {
    uint32_t d = p->buttons & (GP_UP | GP_DOWN | GP_LEFT | GP_RIGHT);
    if (!d) {
        if (p->ly > STICK_DIR) d |= GP_UP;
        if (p->ly < -STICK_DIR) d |= GP_DOWN;
        if (p->lx < -STICK_DIR) d |= GP_LEFT;
        if (p->lx > STICK_DIR) d |= GP_RIGHT;
    }
    if ((d & (GP_UP | GP_DOWN)) == (GP_UP | GP_DOWN)) d &= ~(uint32_t)(GP_UP | GP_DOWN);
    if ((d & (GP_LEFT | GP_RIGHT)) == (GP_LEFT | GP_RIGHT)) d &= ~(uint32_t)(GP_LEFT | GP_RIGHT);
    return d;
}
