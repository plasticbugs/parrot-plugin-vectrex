// swpro.c -- see swpro.h.
#include "swpro.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// SPI flash: the sticks' factory calibration, 9 bytes each
#define CAL_LEFT_ADDR  0x603D
#define CAL_RIGHT_ADDR 0x6046
#define CAL_LEN        9

#define DEFAULT_CENTRE 2048
#define DEFAULT_SPAN   1536

static const uint8_t rumble_off[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

bool swpro_matches(uint16_t vid, uint16_t pid) { return vid == SWPRO_VID && pid == SWPRO_PID; }

static void say(swpro_t *s, const char *fmt, ...) {
    if (!s->log) return;
    char line[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    s->log(line);
}

static void axis_default(swpro_axis_t *a) {
    a->centre = DEFAULT_CENTRE;
    a->below = a->above = DEFAULT_SPAN;
}

void swpro_init(swpro_t *s, uint32_t now_us) {
    void (*log)(const char *) = s->log;
    memset(s, 0, sizeof *s);
    s->log = log;
    for (int i = 0; i < 4; i++) axis_default(&s->axis[i]);
    s->init_us = s->run_us = s->step_us = now_us;
}

const char *swpro_step_name(int step) {
    static const char *const name[] = {"HANDSHAKE", "3 MBAUD", "HANDSHAKE", "NO TIMEOUT",
                                       "CALIBRATION", "CALIBRATION", "FULL REPORTS", "LIGHT", "READY"};
    return step >= 0 && step <= SWPRO_RUN ? name[step] : "?";
}

int16_t swpro_axis(const swpro_axis_t *a, int raw) {
    int d = raw - a->centre, span = d >= 0 ? a->above : a->below;
    if (span < 64) span = 64;
    int v = d * GP_STICK_MAX / span;
    if (v > GP_STICK_MAX) v = GP_STICK_MAX;
    if (v < -GP_STICK_MAX - 1) v = -GP_STICK_MAX - 1;
    return (int16_t)v;
}

static void next(swpro_t *s, uint32_t now_us) {
    if (s->step < SWPRO_RUN) s->result[s->step] = (uint8_t)((s->tries & 0x7F) | (s->skipping ? SWPRO_SKIPPED : 0));
    s->skipping = false;
    s->step++;
    s->waiting = false;
    s->tries = 0;
    s->step_us = now_us;
    if (s->step == SWPRO_RUN) {
        s->run_us = now_us;
        s->ready_us = now_us;
        say(s, "SWITCH PRO READY");
    }
}

static void restart(swpro_t *s, uint32_t now_us) {
    s->step = SWPRO_HANDSHAKE;
    s->waiting = false;
    s->tries = 0;
    s->restarts++;
    memset(s->result, 0, sizeof s->result);
    s->ready_us = 0;
    s->init_us = s->run_us = s->step_us = now_us;
}

static bool subcommand(swpro_t *s, swpro_send_fn send, void *ctx, uint8_t sub, const uint8_t *args, int n) {
    uint8_t b[16];
    b[0] = s->counter & 15;
    memcpy(b + 1, rumble_off, 8);
    b[9] = sub;
    memcpy(b + 10, args, (size_t)n);
    if (!send(ctx, 0x01, b, (uint16_t)(10 + n))) return false;
    s->counter++;
    return true;
}

static bool send_step(swpro_t *s, swpro_send_fn send, void *ctx) {
    uint8_t a[5];
    switch (s->step) {
    case SWPRO_HANDSHAKE:
    case SWPRO_HANDSHAKE2: a[0] = 0x02; return send(ctx, 0x80, a, 1);
    case SWPRO_BAUD:       a[0] = 0x03; return send(ctx, 0x80, a, 1);
    case SWPRO_NO_TIMEOUT: a[0] = 0x04; return send(ctx, 0x80, a, 1);
    case SWPRO_CAL_LEFT:
    case SWPRO_CAL_RIGHT: {
        uint16_t addr = s->step == SWPRO_CAL_LEFT ? CAL_LEFT_ADDR : CAL_RIGHT_ADDR;
        a[0] = addr & 0xFF; a[1] = addr >> 8; a[2] = a[3] = 0; a[4] = CAL_LEN;
        return subcommand(s, send, ctx, 0x10, a, 5);
    }
    case SWPRO_MODE:  a[0] = 0x30; return subcommand(s, send, ctx, 0x03, a, 1);
    case SWPRO_LIGHT: a[0] = 0x01; return subcommand(s, send, ctx, 0x30, a, 1);
    }
    return true;
}

void swpro_task(swpro_t *s, uint32_t now_us, swpro_send_fn send, void *ctx) {
    // (times are compared signed: a now_us read before a report was stamped
    // is no time passed, not 71 minutes)
    if (s->step == SWPRO_RUN) {
        if ((int32_t)(now_us - s->run_us) > SWPRO_QUIET_US) {
            say(s, "NO REPORTS FOR 1 S: START OVER");
            restart(s, now_us);
        }
        return;
    }
    if (s->waiting) {
        bool cmd = s->step <= SWPRO_NO_TIMEOUT;
        uint32_t wait = s->step == SWPRO_NO_TIMEOUT ? 20000 : cmd ? SWPRO_CMD_WAIT_US : SWPRO_SUB_WAIT_US;
        if ((int32_t)(now_us - s->sent_us) < (int32_t)wait) return;
        if (s->step == SWPRO_NO_TIMEOUT) { next(s, now_us); return; }    // it never answers this one
        s->waiting = false;
        if (s->tries >= SWPRO_TRIES) {
            say(s, "NO REPLY: %s", swpro_step_name(s->step));
            s->skipping = true;
            next(s, now_us);
        }
        return;
    }
    if (!send_step(s, send, ctx)) {
        if ((int32_t)(now_us - s->step_us) > SWPRO_SEND_US) {
            say(s, "CAN'T SEND: %s", swpro_step_name(s->step));
            s->skipping = true;
            next(s, now_us);
        }
        return;
    }
    s->waiting = true;
    s->sent_us = now_us;
    s->tries++;
}

// the 12 bytes of state at r[1] (0x30 and 0x21 alike)
static void parse_state(swpro_t *s, const uint8_t *r) {
    uint8_t rb = r[3], sh = r[4], lb = r[5];
    uint32_t b = 0;
    if (rb & 0x01) b |= GP_WEST;        // Y
    if (rb & 0x02) b |= GP_NORTH;       // X
    if (rb & 0x04) b |= GP_SOUTH;       // B
    if (rb & 0x08) b |= GP_EAST;        // A
    if (rb & 0x40) b |= GP_R;
    if (rb & 0x80) b |= GP_ZR;
    if (sh & 0x01) b |= GP_MINUS;
    if (sh & 0x02) b |= GP_PLUS;
    if (sh & 0x04) b |= GP_RSTICK;
    if (sh & 0x08) b |= GP_LSTICK;
    if (sh & 0x10) b |= GP_HOME;
    if (sh & 0x20) b |= GP_CAPTURE;
    if (lb & 0x01) b |= GP_DOWN;
    if (lb & 0x02) b |= GP_UP;
    if (lb & 0x04) b |= GP_RIGHT;
    if (lb & 0x08) b |= GP_LEFT;
    if (lb & 0x40) b |= GP_L;
    if (lb & 0x80) b |= GP_ZL;
    gamepad_t *p = &s->pad;
    p->buttons = b;
    for (int k = 0; k < 2; k++) {
        const uint8_t *d = r + 6 + 3 * k;
        p->raw[2 * k] = (uint16_t)(d[0] | (d[1] & 0x0F) << 8);
        p->raw[2 * k + 1] = (uint16_t)(d[1] >> 4 | d[2] << 4);
    }
    p->lx = swpro_axis(&s->axis[0], p->raw[0]);
    p->ly = swpro_axis(&s->axis[1], p->raw[1]);
    p->rx = swpro_axis(&s->axis[2], p->raw[2]);
    p->ry = swpro_axis(&s->axis[3], p->raw[3]);
}

// 9 bytes of calibration: six 12-bit values, in an order that differs
// between the sticks (as hid-nintendo reads them)
static bool parse_cal(swpro_t *s, int stick, const uint8_t *d) {
    int v[6];
    for (int i = 0; i < 3; i++) {
        v[2 * i] = d[3 * i] | (d[3 * i + 1] & 0x0F) << 8;
        v[2 * i + 1] = d[3 * i + 1] >> 4 | d[3 * i + 2] << 4;
    }
    int xa, ya, xc, yc, xb, yb;
    if (stick == 0) { xa = v[0]; ya = v[1]; xc = v[2]; yc = v[3]; xb = v[4]; yb = v[5]; }
    else            { xc = v[0]; yc = v[1]; xb = v[2]; yb = v[3]; xa = v[4]; ya = v[5]; }
    const int c[2] = {xc, yc}, below[2] = {xb, yb}, above[2] = {xa, ya};
    for (int i = 0; i < 2; i++)                 // unprogrammed (FF FF ..) or nonsense: keep the defaults
        if (c[i] < 1000 || c[i] > 3100 || below[i] < 400 || below[i] > 2047 || above[i] < 400 || above[i] > 2047)
            return false;
    for (int i = 0; i < 2; i++) {
        swpro_axis_t *a = &s->axis[2 * stick + i];
        a->centre = (int16_t)c[i];
        a->below = (int16_t)below[i];
        a->above = (int16_t)above[i];
    }
    return true;
}

bool swpro_report(swpro_t *s, const uint8_t *r, uint16_t len, uint32_t now_us) {
    if (!len) return false;
    switch (r[0]) {
    case 0x81:                                  // a USB command's reply
        if (len >= 2 && s->waiting && s->step <= SWPRO_HANDSHAKE2 &&
            r[1] == (s->step == SWPRO_BAUD ? 0x03 : 0x02))
            next(s, now_us);
        return false;
    case 0x21: {                                // a subcommand's reply
        if (len < 15) return false;
        parse_state(s, r);
        uint8_t sub = r[14];
        bool ack = r[13] & 0x80;
        if (!s->waiting) return true;
        if ((s->step == SWPRO_CAL_LEFT || s->step == SWPRO_CAL_RIGHT) && sub == 0x10) {
            int stick = s->step == SWPRO_CAL_RIGHT;
            uint16_t want = stick ? CAL_RIGHT_ADDR : CAL_LEFT_ADDR;
            if (len < 20 + CAL_LEN || (r[15] | r[16] << 8) != want) return true;   // another read's
            s->calibrated[stick] = ack && parse_cal(s, stick, r + 20);
            if (!s->calibrated[stick]) say(s, "NO %s STICK CALIBRATION", stick ? "RIGHT" : "LEFT");
            next(s, now_us);
        } else if ((s->step == SWPRO_MODE && sub == 0x03) || (s->step == SWPRO_LIGHT && sub == 0x30)) {
            if (!ack) say(s, "SUBCOMMAND %02X REFUSED", sub);
            next(s, now_us);
        }
        return true;
    }
    case 0x30:                                  // the full report
        if (len < 12) return false;
        parse_state(s, r);
        s->reports++;
        if (s->step == SWPRO_RUN) s->run_us = now_us;
        return true;
    }
    return false;
}
