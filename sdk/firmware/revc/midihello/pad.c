// pad.c -- see pad.h.
#include "pad.h"

#define REPEAT_FIRST_US 400000
#define REPEAT_US       100000

int pad_key(uint8_t lines) {
    // the ColecoVision keypad's codes as read (MAME's controller agrees, and
    // gbcv/gbplay.c for 1-4, * and #)
    static const int8_t key[16] = {
        -1, 8, 4, 5, -1, 7, PAD_HASH, 2, -1, PAD_STAR, 0, 9, 3, 1, 6, -1};
    return key[lines & 15];
}

void pad_init(pad_t *p, int bpm, bool running) {
    p->raw[0] = p->raw[1] = 0xFF;
    p->joy = 0;
    p->key = -1;
    p->repeat_at = 0;
    p->bpm = bpm;
    p->running = running;
    p->pressed = -1;
}

static bool tempo(pad_t *p, uint8_t dirs) {
    int d = (dirs & 1 ? 1 : 0) - (dirs & 4 ? 1 : 0) + (dirs & 2 ? 10 : 0) - (dirs & 8 ? 10 : 0);
    int b = p->bpm + d;
    b = b < PAD_BPM_MIN ? PAD_BPM_MIN : b > PAD_BPM_MAX ? PAD_BPM_MAX : b;
    if (b == p->bpm) return false;
    p->bpm = b;
    return true;
}

bool pad_report(pad_t *p, uint8_t a, uint32_t now_us) {
    int k = a & PAD_KEYPAD ? 1 : 0;
    uint8_t v = a & 0x1F;
    if (v != p->raw[k]) {                       // new: it counts if the next is the same
        p->raw[k] = v;
        return false;
    }
    bool changed = false;
    if (k) {
        int key = pad_key(v & 15);
        if (key != p->key) {
            p->key = (int8_t)key;
            if (key >= 0) {
                p->pressed = (int8_t)key;
                changed = true;
            }
        }
        return changed;
    }
    uint8_t joy = (uint8_t)(~v & 0x1F), was = p->joy, dirs = joy & 15;
    p->joy = joy;
    if (joy & ~was & 0x10) {                    // the fire button went down
        p->running = !p->running;
        changed = true;
    }
    if (dirs && dirs != (was & 15)) {           // a new direction: at once
        changed |= tempo(p, dirs);
        p->repeat_at = now_us + REPEAT_FIRST_US;
    } else if (dirs && (int32_t)(now_us - p->repeat_at) >= 0) {
        changed |= tempo(p, dirs);
        p->repeat_at = now_us + REPEAT_US;
    }
    return changed;
}
