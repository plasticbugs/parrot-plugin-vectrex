// hidpad.c -- see hidpad.h.
#include "hidpad.h"

#include <string.h>

static const uint32_t map_8bitdo[HP_BUTTONS] = {
    GP_EAST, GP_SOUTH, 0, GP_NORTH, GP_WEST, 0, GP_L, GP_R,
    GP_ZL, GP_ZR, GP_MINUS, GP_PLUS, GP_HOME, GP_LSTICK, GP_RSTICK};
static const uint32_t map_generic[HP_BUTTONS] = {
    GP_WEST, GP_SOUTH, GP_EAST, GP_NORTH, GP_L, GP_R, GP_ZL, GP_ZR,
    GP_MINUS, GP_PLUS, GP_LSTICK, GP_RSTICK, GP_HOME, GP_CAPTURE};

// ---- the descriptor ------------------------------------------------------------------
typedef struct {
    uint16_t page;
    int32_t lmin, lmax;
    uint8_t lmax_n;                         // the maximum's item size, bytes
    uint32_t size, count;
    uint8_t id;
} globals_t;

#define MAX_USAGES 16
#define MAX_PUSH   4

bool hidpad_parse(hidpad_t *h, const uint8_t *d, uint16_t len, uint16_t vid) {
    memset(h, 0, sizeof *h);
    memset(h->button_bit, 0xFF, sizeof h->button_bit);
    static uint16_t off[256];               // each report's bits so far
    memset(off, 0, sizeof off);
    globals_t g = {0}, stack[MAX_PUSH];
    int sp = 0;
    uint32_t usage[MAX_USAGES], umin = 0, umax = 0;
    int nusage = 0;
    bool range = false;
    int depth = 0, pad_depth = -1;          // the pad's application collection: its depth
    bool chosen = false;                    // the report with the controls is picked

    for (uint16_t i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                    // long item: skip it
            if (i + 1 >= len) break;
            i = (uint16_t)(i + 2 + d[i]);
            continue;
        }
        int n = (b & 3) == 3 ? 4 : (b & 3);
        if (i + n > len) break;
        uint32_t u = 0;
        for (int k = 0; k < n; k++) u |= (uint32_t)d[i + k] << (8 * k);
        int32_t s = n == 1 ? (int8_t)u : n == 2 ? (int16_t)u : (int32_t)u;
        i = (uint16_t)(i + n);
        uint8_t type = b >> 2 & 3, tag = b >> 4;

        if (type == 1) {                    // global
            switch (tag) {
            case 0: g.page = (uint16_t)u; break;
            case 1: g.lmin = s; break;
            case 2: g.lmax = s; g.lmax_n = (uint8_t)n; break;
            case 7: g.size = u; break;
            case 8: g.id = (uint8_t)u; h->has_ids = true; break;
            case 9: g.count = u; break;
            case 10: if (sp < MAX_PUSH) stack[sp++] = g; break;
            case 11: if (sp > 0) g = stack[--sp]; break;
            }
            continue;
        }
        if (type == 2) {                    // local
            uint32_t full = n == 4 ? u : (uint32_t)g.page << 16 | u;
            if (tag == 0 && nusage < MAX_USAGES) usage[nusage++] = full;
            else if (tag == 1) { umin = full; range = true; }
            else if (tag == 2) { umax = full; range = true; }
            continue;
        }
        if (type != 0) continue;            // reserved
        switch (tag) {
        case 10:                            // collection
            if (u == 1 && pad_depth < 0 && nusage &&
                (usage[0] == 0x10004 || usage[0] == 0x10005))       // Joystick, Game Pad
                pad_depth = depth;
            depth++;
            break;
        case 12:                            // end collection
            if (depth > 0) depth--;
            if (depth == pad_depth) pad_depth = -1;
            break;
        case 8: {                           // input
            int32_t lmax = g.lmax;          // below the minimum: written unsigned (25 FF is 255)
            if (lmax < g.lmin) lmax = (int32_t)((uint32_t)g.lmax & (g.lmax_n == 1 ? 0xFFu : g.lmax_n == 2 ? 0xFFFFu : ~0u));
            bool use = pad_depth >= 0 && !(u & 1) && (u & 2) && (!chosen || g.id == h->report_id);
            for (uint32_t k = 0; use && k < g.count; k++) {
                uint32_t us = range ? umin + k : (int)k < nusage ? usage[k] : nusage ? usage[nusage - 1] : 0;
                if (range && us > umax) break;
                uint16_t bit = (uint16_t)(off[g.id] + k * g.size);
                uint16_t page = us >> 16, id = us & 0xFFFF;
                int which = -1;
                if (page == 1 && id >= 0x30 && id <= 0x35) which = HP_X + (id - 0x30);
                else if (page == 1 && id == 0x39) which = HP_HAT;
                if (which >= 0 && !h->f[which].size && g.size <= 32) {
                    h->f[which] = (hidpad_field_t){bit, (uint8_t)g.size, g.lmin, lmax};
                } else if (page == 9 && id >= 1 && id <= HP_BUTTONS && g.size == 1) {
                    h->button_bit[id - 1] = bit;
                    if (id > h->buttons) h->buttons = (uint8_t)id;
                } else {
                    continue;
                }
                if (!chosen) { chosen = true; h->report_id = g.id; }
            }
            off[g.id] = (uint16_t)(off[g.id] + g.size * g.count);
            break;
        }
        case 9: case 11: break;             // output, feature: not the pad's controls
        }
        nusage = 0;                         // locals last until the next main item
        range = false;
    }
    if (!chosen) return false;
    bool eightbitdo = vid == 0x2DC8;
    h->map = eightbitdo ? map_8bitdo : map_generic;
    h->layout = eightbitdo ? "8BITDO" : "GENERIC";
    h->hat = -1;
    return true;
}

// ---- reports -------------------------------------------------------------------------
static int32_t field(const hidpad_field_t *f, const uint8_t *r, uint16_t len) {
    uint32_t v = 0;
    for (int k = 0; k < f->size; k++) {
        uint32_t bit = f->bit + (uint32_t)k;
        if (bit / 8 >= len) break;
        v |= (uint32_t)(r[bit / 8] >> (bit % 8) & 1) << k;
    }
    if (f->lmin < 0 && f->size < 32 && (v >> (f->size - 1) & 1)) v |= ~0u << f->size;   // signed
    return (int32_t)v;
}

// a stick axis, -2048..2047 about the middle of its range
static int16_t axis(const hidpad_field_t *f, int32_t v) {
    int64_t lo = f->lmin, hi = f->lmax;
    if (hi <= lo) return 0;
    int64_t span = hi - lo + 1, x = ((int64_t)v - lo) * 4096 / span - 2048;
    if (x > GP_STICK_MAX) x = GP_STICK_MAX;
    if (x < -GP_STICK_MAX - 1) x = -GP_STICK_MAX - 1;
    return (int16_t)x;
}

static int16_t flip(int16_t v) { return v == -GP_STICK_MAX - 1 ? GP_STICK_MAX : (int16_t)-v; }

bool hidpad_report(hidpad_t *h, const uint8_t *r, uint16_t len) {
    if (h->has_ids) {
        if (!len || r[0] != h->report_id) return false;
        r++;
        len--;
    }
    gamepad_t *p = &h->pad;
    int32_t v[HP_FIELDS] = {0};
    for (int i = 0; i < HP_FIELDS; i++) if (h->f[i].size) v[i] = field(&h->f[i], r, len);
    const hidpad_field_t *f = h->f;
    int rx = HP_Z, ry = HP_RZ;                      // the right stick: Z, Rz, or Rx, Ry
    if (f[HP_RX].size && f[HP_RY].size) { rx = HP_RX; ry = HP_RY; }
    p->lx = f[HP_X].size ? axis(&f[HP_X], v[HP_X]) : 0;
    p->ly = f[HP_Y].size ? flip(axis(&f[HP_Y], v[HP_Y])) : 0;     // HID: down is up the range
    p->rx = f[rx].size ? axis(&f[rx], v[rx]) : 0;
    p->ry = f[ry].size ? flip(axis(&f[ry], v[ry])) : 0;
    p->raw[0] = (uint16_t)v[HP_X];
    p->raw[1] = (uint16_t)v[HP_Y];
    p->raw[2] = (uint16_t)v[rx];
    p->raw[3] = (uint16_t)v[ry];

    uint32_t b = 0, raw = 0;
    for (int n = 0; n < h->buttons; n++) {
        if (h->button_bit[n] == 0xFFFF) continue;
        hidpad_field_t bf = {h->button_bit[n], 1, 0, 1};
        if (field(&bf, r, len)) {
            raw |= 1u << n;
            b |= h->map[n];
        }
    }
    h->hat = -1;
    if (f[HP_HAT].size) {
        int32_t d = v[HP_HAT] - f[HP_HAT].lmin, range = f[HP_HAT].lmax - f[HP_HAT].lmin;
        if (range == 7 && d >= 0 && d <= 7) h->hat = (int8_t)d;
        else if (range == 3 && d >= 0 && d <= 3) h->hat = (int8_t)(2 * d);     // four ways
    }
    static const uint8_t hat_dirs[8] = {
        1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4, 1 | 4};   // up 1, down 2, left 4, right 8
    if (h->hat >= 0) {
        uint8_t m = hat_dirs[h->hat];
        if (m & 1) b |= GP_UP;
        if (m & 2) b |= GP_DOWN;
        if (m & 4) b |= GP_LEFT;
        if (m & 8) b |= GP_RIGHT;
    }
    p->buttons = b;
    h->raw_buttons = raw;
    return true;
}
