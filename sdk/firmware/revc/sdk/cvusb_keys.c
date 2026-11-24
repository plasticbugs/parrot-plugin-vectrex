// cvusb_keys.c -- see cvusb_keys.h.
#include "cvusb_keys.h"

#include <string.h>

char cvkey_ascii(uint8_t usage, bool shift) {
    static const char DIGIT[] = "1234567890", DIGIT_SHIFT[] = "!@#$%^&*()";
    // 0x2C-0x38: space - = [ ] \ (non-US #) ; ' ` , . /
    static const char PUNCT[] = " -=[]\\#;'`,./", PUNCT_SHIFT[] = " _+{}|~:\"~<>?";
    static const char KEYPAD[] = "/*-+\r1234567890.";            // 0x54-0x63
    const unsigned u = usage;
    if (u >= 0x04 && u <= 0x1D) return (char)((shift ? 'A' : 'a') + (u - 0x04));
    if (u >= 0x1E && u <= 0x27) return shift ? DIGIT_SHIFT[u - 0x1E] : DIGIT[u - 0x1E];
    if (u >= 0x2C && u <= 0x38) return shift ? PUNCT_SHIFT[u - 0x2C] : PUNCT[u - 0x2C];
    if (u >= 0x54 && u <= 0x63) return KEYPAD[u - 0x54];
    switch (usage) {
    case 0x28: return '\r';                     // Enter
    case 0x29: return 27;                       // Escape
    case 0x2A: return '\b';                     // Backspace
    case 0x2B: return '\t';                     // Tab
    case 0x4C: return 127;                      // Delete
    default:   return 0;
    }
}

static bool has(const uint8_t *keys, uint8_t k) {
    for (int i = 0; i < 6; i++)
        if (keys[i] == k) return true;
    return false;
}

static void send(void (*emit)(const cvkey_t *, void *), void *ctx, uint8_t usage, uint8_t mods, bool down) {
    const cvkey_t e = {usage, mods, cvkey_ascii(usage, mods & CVKEY_SHIFT), down};
    emit(&e, ctx);
}

void cvkeys_report(cvkeys_t *k, const uint8_t *r, uint16_t len, void (*emit)(const cvkey_t *, void *), void *ctx) {
    if (len < 8) return;
    // modifiers, reserved, six keys; 1-3 in the first place: the keyboard's
    // errors (too many keys at once), which say nothing of which are held
    const uint8_t mods = r[0], *keys = r + 2;
    const bool error = keys[0] >= 1 && keys[0] <= 3;
    uint8_t now[6] = {0};
    if (!error)
        for (int i = 0; i < 6; i++) now[i] = keys[i] > 3 ? keys[i] : 0;
    else
        memcpy(now, k->keys, sizeof now);
    // what was let go, then what went down: the modifiers as keys too
    // (HID's 0xE0 left ctrl ... 0xE7 right GUI), between the two
    for (int i = 0; i < 6; i++)
        if (k->keys[i] && !has(now, k->keys[i])) send(emit, ctx, k->keys[i], mods, false);
    for (int b = 0; b < 8; b++)
        if ((k->mods & ~mods) >> b & 1) send(emit, ctx, (uint8_t)(0xE0 + b), mods, false);
    for (int b = 0; b < 8; b++)
        if ((mods & ~k->mods) >> b & 1) send(emit, ctx, (uint8_t)(0xE0 + b), mods, true);
    for (int i = 0; i < 6; i++)
        if (now[i] && !has(k->keys, now[i])) send(emit, ctx, now[i], mods, true);
    memcpy(k->keys, now, sizeof now);
    k->mods = mods;
}

void cvkeys_release(cvkeys_t *k, void (*emit)(const cvkey_t *, void *), void *ctx) {
    for (int i = 0; i < 6; i++)
        if (k->keys[i]) send(emit, ctx, k->keys[i], 0, false);
    for (int b = 0; b < 8; b++)
        if (k->mods >> b & 1) send(emit, ctx, (uint8_t)(0xE0 + b), 0, false);
    memset(k, 0, sizeof *k);
}

bool cvmouse_report(cvmouse_t *m, const uint8_t *r, uint16_t len) {
    if (len < 3) return false;
    m->buttons = r[0] & 7;
    m->dx += (int8_t)r[1];
    m->dy += (int8_t)r[2];
    if (len >= 4) m->wheel += (int8_t)r[3];
    return true;
}
