// cvusb_test.c -- cvusb's readers on the Mac (test/run.sh): what the cart
// makes of a USB keyboard's and a mouse's boot reports (cvusb_keys.c), of a
// game pad's reports (hidpad.c and swpro.c, through gamepad_t), of USB-MIDI
// packets both ways and a MIDI device's descriptors (cvusb_midi.c), and
// player 1 as cvinput.h puts the console's controller and USB pad 1
// together.
#include <stdio.h>
#include <string.h>

#include "cvinput.h"
#include "cvusb_keys.h"
#include "cvusb_midi.h"
#include "hidpad.h"
#include "swpro.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

// ---- keyboards -------------------------------------------------------------------------------
static cvkey_t ev[32];
static int nev;
static void got(const cvkey_t *e, void *ctx) { (void)ctx; if (nev < 32) ev[nev++] = *e; }

static void kb(cvkeys_t *k, uint8_t mods, uint8_t k0, uint8_t k1, uint8_t k2) {
    const uint8_t r[8] = {mods, 0, k0, k1, k2, 0, 0, 0};
    nev = 0;
    cvkeys_report(k, r, 8, got, NULL);
}

static void test_keyboard(void) {
    cvkeys_t k;
    memset(&k, 0, sizeof k);
    kb(&k, 0, 0x04, 0, 0);                              // a
    CHECK(nev == 1 && ev[0].usage == 0x04 && ev[0].down && ev[0].ascii == 'a' && ev[0].mods == 0);
    kb(&k, 0, 0x04, 0x05, 0);                           // a held, b too
    CHECK(nev == 1 && ev[0].usage == 0x05 && ev[0].down && ev[0].ascii == 'b');
    kb(&k, 0, 0x05, 0, 0);                              // a let go (b moves to the first place)
    CHECK(nev == 1 && ev[0].usage == 0x04 && !ev[0].down);
    kb(&k, 0, 0, 0, 0);                                 // all up
    CHECK(nev == 1 && ev[0].usage == 0x05 && !ev[0].down);

    // left shift, then 1 with it: the modifier as a key (0xE1), then '!'
    kb(&k, 0x02, 0, 0, 0);
    CHECK(nev == 1 && ev[0].usage == 0xE1 && ev[0].down && ev[0].mods == 0x02 && ev[0].ascii == 0);
    kb(&k, 0x02, 0x1E, 0, 0);
    CHECK(nev == 1 && ev[0].usage == 0x1E && ev[0].down && ev[0].ascii == '!' && (ev[0].mods & CVKEY_SHIFT));
    // too many keys: the keyboard's error (1) changes no key; shift let go still counts
    kb(&k, 0, 0x01, 0x01, 0x01);
    CHECK(nev == 1 && ev[0].usage == 0xE1 && !ev[0].down);
    kb(&k, 0, 0x1E, 0, 0);                              // 1 still held: nothing new
    CHECK(nev == 0);
    // right ctrl + right alt, then unplugged: everything let go
    kb(&k, 0x50, 0x1E, 0x28, 0);
    CHECK(nev == 3 && ev[0].usage == 0xE4 && ev[1].usage == 0xE6 && ev[2].usage == 0x28 && ev[2].ascii == '\r');
    nev = 0;
    cvkeys_release(&k, got, NULL);
    CHECK(nev == 4 && !ev[0].down && !ev[3].down && ev[0].usage == 0x1E && ev[1].usage == 0x28);
    CHECK(k.mods == 0 && k.keys[0] == 0);
    // a short report: nothing
    const uint8_t shorty[4] = {0, 0, 0x04, 0};
    nev = 0;
    cvkeys_report(&k, shorty, 4, got, NULL);
    CHECK(nev == 0);

    // characters on a US keyboard
    CHECK(cvkey_ascii(0x1D, false) == 'z' && cvkey_ascii(0x1D, true) == 'Z');
    CHECK(cvkey_ascii(0x27, false) == '0' && cvkey_ascii(0x27, true) == ')');
    CHECK(cvkey_ascii(0x2C, false) == ' ' && cvkey_ascii(0x2D, true) == '_' && cvkey_ascii(0x38, true) == '?');
    CHECK(cvkey_ascii(0x34, false) == '\'' && cvkey_ascii(0x34, true) == '"');
    CHECK(cvkey_ascii(0x2A, false) == '\b' && cvkey_ascii(0x29, false) == 27 && cvkey_ascii(0x2B, false) == '\t');
    CHECK(cvkey_ascii(0x4C, false) == 127);
    CHECK(cvkey_ascii(0x59, false) == '1' && cvkey_ascii(0x62, false) == '0' && cvkey_ascii(0x58, false) == '\r');
    CHECK(cvkey_ascii(0x4F, false) == 0 && cvkey_ascii(0x3A, false) == 0 && cvkey_ascii(0xE0, false) == 0);   // right arrow, F1, ctrl
}

// ---- mice ------------------------------------------------------------------------------------
static void test_mouse(void) {
    cvmouse_t m = {0};
    const uint8_t r1[3] = {0x01, 5, 0xFD};                  // left, 5 right, 3 up
    const uint8_t r2[4] = {0x02, 0x80, 10, 0xFF};           // right, 128 left, 10 down, wheel 1 toward you
    const uint8_t r3[5] = {0x04, 0, 0, 2, 0x55};            // middle, wheel 2 away (and a byte more)
    CHECK(cvmouse_report(&m, r1, 3) && m.buttons == 1 && m.dx == 5 && m.dy == -3 && m.wheel == 0);
    CHECK(cvmouse_report(&m, r2, 4) && m.buttons == 2 && m.dx == 5 - 128 && m.dy == 7 && m.wheel == -1);
    CHECK(cvmouse_report(&m, r3, 5) && m.buttons == 4 && m.wheel == 1);
    CHECK(!cvmouse_report(&m, r1, 2) && m.buttons == 4);
}

// ---- pads ------------------------------------------------------------------------------------
// a plain pad laid out as a DualShock 4's simple report begins: report 1,
// X Y Z Rz, the hat and 14 buttons, a counter. (The DualShock 4's own goes
// on with its triggers as Rx and Ry, which hidpad.c takes for the right
// stick when a pad has them: see the SDK README's "Not here yet".)
static const uint8_t ds4_desc[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
    0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0E, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x0E, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00, 0x25, 0x7F, 0x81, 0x02,
    0xC0};

// a keyboard's boot descriptor: not a pad
static const uint8_t kb_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x06, 0x75, 0x08,
    0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};

static void test_pads(void) {
    hidpad_t h;
    CHECK(!hidpad_parse(&h, kb_desc, sizeof kb_desc, 0x046D));
    CHECK(hidpad_parse(&h, ds4_desc, sizeof ds4_desc, 0x054C));
    CHECK(h.has_ids && h.report_id == 1 && !strcmp(h.layout, "GENERIC"));
    // sticks at rest a little off centre; the hat centred (8); cross (button 2) held
    uint8_t r[10] = {0x01, 0x80, 0x7F, 0x80, 0x80, 0x08 | 0x20, 0x00, 0x00, 0x00, 0x00};
    CHECK(hidpad_report(&h, r, sizeof r));
    CHECK(h.pad.buttons == GP_SOUTH && h.pad.lx == 0 && h.pad.ly == 16 && h.pad.rx == 0 && h.pad.ry == 0);
    r[1] = 0xFF; r[2] = 0x00;                           // left stick right and up
    r[5] = 0x02 | 0x40;                                 // hat: right; circle (3)
    r[6] = 0x02;                                        // R1 (6)
    CHECK(hidpad_report(&h, r, sizeof r));
    CHECK(h.pad.buttons == (GP_RIGHT | GP_EAST | GP_R) && h.pad.lx == 2032 && h.pad.ly == 2047);

    // the Switch Pro Controller's full report (0x30): A and the d-pad's up,
    // the left stick right as far as the default calibration goes
    swpro_t s;
    swpro_init(&s, 0);
    uint8_t q[64] = {0x30};
    q[3] = 0x08;                                        // A
    q[5] = 0x02;                                        // up
    const int x = 2048 + 1536, y = 2048;                // (the defaults: centre 2048, 1536 each way)
    q[6] = (uint8_t)x; q[7] = (uint8_t)((x >> 8) | (y & 0x0F) << 4); q[8] = (uint8_t)(y >> 4);
    q[9] = 0x00; q[10] = 0x08; q[11] = 0x80;            // right stick centred
    CHECK(swpro_report(&s, q, 64, 0));
    CHECK(s.pad.buttons == (GP_EAST | GP_UP) && s.pad.lx == GP_STICK_MAX && s.pad.ly == 0 && s.pad.rx == 0 && s.pad.ry == 0);
}

// ---- MIDI ------------------------------------------------------------------------------------
// a USB MIDI keyboard's configuration (PSG Synth's test's): Audio Control,
// then MIDIStreaming with a bulk OUT and IN, then a HID interface after it
static const uint8_t cfg[] = {
    0x09, 0x02, 0x6E, 0x00, 0x03, 0x01, 0x00, 0x80, 0x32,
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,       // interface 0: Audio Control
    0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01,       //   AC header
    0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,       // interface 1: MIDIStreaming
    0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,                   //   MS header
    0x06, 0x24, 0x02, 0x01, 0x01, 0x00,                         //   IN jack, embedded
    0x06, 0x24, 0x02, 0x02, 0x02, 0x00,                         //   IN jack, external
    0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,       //   OUT jack, embedded
    0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00,       //   OUT jack, external
    0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,       //   bulk OUT 01
    0x05, 0x25, 0x01, 0x01, 0x01,
    0x09, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,       //   bulk IN 81
    0x05, 0x25, 0x01, 0x01, 0x03,
    0x09, 0x04, 0x02, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,       // interface 2: HID (a pad's knobs, say)
};

static void test_midi_desc(void) {
    cvmidi_found_t f;
    // TinyUSB 0.21 hands a driver each interface with the rest of the configuration
    CHECK(cvmidi_find(cfg + 9, sizeof cfg - 9, &f));
    CHECK(f.ms_itf == 1 && f.ep_in == cfg + 87 && f.ep_in[2] == 0x81 && f.ep_out == cfg + 73);
    CHECK(f.len == 101 - 9);                            // Audio Control through MIDIStreaming, not the HID interface
    CHECK(cvmidi_find(cfg + 27, sizeof cfg - 27, &f) && f.ms_itf == 1 && f.len == 101 - 27);
    CHECK(cvmidi_find(cfg + 9, 101 - 9, &f) && f.len == 101 - 9);  // ending with it
    CHECK(!cvmidi_find(cfg + 9, 18, &f));               // Audio Control alone
    CHECK(!cvmidi_find(cfg + 101, sizeof cfg - 101, &f));  // the HID interface
}

static void test_midi_in(void) {
    uint8_t m[3];
    const uint8_t on[4] = {0x09, 0x90, 60, 100}, off[4] = {0x08, 0x80, 60, 0}, cc[4] = {0x1B, 0xB1, 1, 64},
                  pc[4] = {0x0C, 0xC2, 5, 0x55}, bend[4] = {0x0E, 0xE0, 0, 0x40}, at[4] = {0x0D, 0xD3, 0x7F, 9},
                  clock[4] = {0x0F, 0xF8, 0, 0}, start[4] = {0x0F, 0xFA, 0, 0}, spp[4] = {0x03, 0xF2, 0x10, 0x02},
                  mtc[4] = {0x02, 0xF1, 0x23, 0}, tune[4] = {0x05, 0xF6, 0, 0}, sx[4] = {0x04, 0xF0, 0x7E, 0x7F},
                  sx_end[4] = {0x06, 0x09, 0xF7, 0}, sx1[4] = {0x05, 0xF7, 0, 0}, pad[4] = {0, 0, 0, 0},
                  liar[4] = {0x09, 0x80, 60, 0}, data[4] = {0x0F, 0x40, 0, 0};
    CHECK(cvmidi_msg(on, m) == 3 && m[0] == 0x90 && m[1] == 60 && m[2] == 100);
    CHECK(cvmidi_msg(off, m) == 3 && m[0] == 0x80);
    CHECK(cvmidi_msg(cc, m) == 3 && m[0] == 0xB1 && m[1] == 1 && m[2] == 64);   // (cable 1: the same)
    CHECK(cvmidi_msg(pc, m) == 2 && m[0] == 0xC2 && m[1] == 5 && m[2] == 0);
    CHECK(cvmidi_msg(bend, m) == 3 && m[2] == 0x40);
    CHECK(cvmidi_msg(at, m) == 2 && m[0] == 0xD3 && m[1] == 0x7F && m[2] == 0);
    CHECK(cvmidi_msg(clock, m) == 1 && m[0] == 0xF8);
    CHECK(cvmidi_msg(start, m) == 1 && m[0] == 0xFA);
    CHECK(cvmidi_msg(spp, m) == 3 && m[0] == 0xF2 && m[1] == 0x10 && m[2] == 0x02);
    CHECK(cvmidi_msg(mtc, m) == 2 && m[0] == 0xF1 && m[1] == 0x23);
    CHECK(cvmidi_msg(tune, m) == 1 && m[0] == 0xF6);
    CHECK(cvmidi_msg(sx, m) == 0 && cvmidi_msg(sx_end, m) == 0 && cvmidi_msg(sx1, m) == 0);
    CHECK(cvmidi_msg(pad, m) == 0 && cvmidi_msg(liar, m) == 0 && cvmidi_msg(data, m) == 0);
    // PSG Synth's channel messages only, as they were
    CHECK(cvmidi_channel_msg(on, m) && m[0] == 0x90 && !cvmidi_channel_msg(clock, m));
}

static void test_midi_out(void) {
    uint8_t p[16][4], st = 0, m[3];
    // a note on and off, the off by running status
    const uint8_t notes[] = {0x90, 60, 100, 60, 0};
    CHECK(cvmidi_pack(notes, sizeof notes, p, 16, &st) == 2);
    CHECK(!memcmp(p[0], "\x09\x90\x3C\x64", 4) && !memcmp(p[1], "\x09\x90\x3C\x00", 4) && st == 0x90);
    // running status carries over to the next call
    const uint8_t more[] = {62, 90};
    CHECK(cvmidi_pack(more, 2, p, 16, &st) == 1 && !memcmp(p[0], "\x09\x90\x3E\x5A", 4));
    // a program change (2 bytes), a clock tick, song position, tune request
    st = 0;
    const uint8_t mix[] = {0xC5, 7, 0xF8, 0xF2, 1, 2, 0xF6};
    CHECK(cvmidi_pack(mix, sizeof mix, p, 16, &st) == 4);
    CHECK(!memcmp(p[0], "\x0C\xC5\x07\x00", 4) && !memcmp(p[1], "\x0F\xF8\x00\x00", 4));
    CHECK(!memcmp(p[2], "\x03\xF2\x01\x02", 4) && !memcmp(p[3], "\x05\xF6\x00\x00", 4) && st == 0);
    // SysEx: 3 a packet, then its end with 1, 2 or 3 bytes; a clock inside goes on its own
    const uint8_t sx2[] = {0xF0, 0xF7}, sx3[] = {0xF0, 1, 0xF7}, sx4[] = {0xF0, 1, 2, 0xF7},
                  sx7[] = {0xF0, 0x41, 0xF8, 0x10, 0x16, 0x12, 0x20, 0xF7};
    CHECK(cvmidi_pack(sx2, 2, p, 16, &st) == 1 && !memcmp(p[0], "\x06\xF0\xF7\x00", 4));
    CHECK(cvmidi_pack(sx3, 3, p, 16, &st) == 1 && !memcmp(p[0], "\x07\xF0\x01\xF7", 4));
    CHECK(cvmidi_pack(sx4, 4, p, 16, &st) == 2 && !memcmp(p[0], "\x04\xF0\x01\x02", 4) && !memcmp(p[1], "\x05\xF7\x00\x00", 4));
    CHECK(cvmidi_pack(sx7, sizeof sx7, p, 16, &st) == 4);
    CHECK(!memcmp(p[0], "\x0F\xF8\x00\x00", 4) && !memcmp(p[1], "\x04\xF0\x41\x10", 4));
    CHECK(!memcmp(p[2], "\x04\x16\x12\x20", 4) && !memcmp(p[3], "\x05\xF7\x00\x00", 4));
    // a SysEx cut short by a status: dropped, the status kept; stray data dropped
    st = 0;
    const uint8_t cut[] = {5, 0xF0, 1, 2, 3, 0x80, 64, 0};
    CHECK(cvmidi_pack(cut, sizeof cut, p, 16, &st) == 2 && !memcmp(p[0], "\x04\xF0\x01\x02", 4) && !memcmp(p[1], "\x08\x80\x40\x00", 4));
    // too many for the room: -1
    CHECK(cvmidi_pack(notes, sizeof notes, p, 1, &st) == -1);
    // and back: every packet made reads as the message it came from
    st = 0;
    const uint8_t all[] = {0x80, 1, 2, 0x91, 3, 4, 0xA2, 5, 6, 0xB3, 7, 8, 0xC4, 9, 0xD5, 10, 0xE6, 11, 12};
    CHECK(cvmidi_pack(all, sizeof all, p, 16, &st) == 7);
    int at = 0;
    for (int i = 0; i < 7; i++) {
        const int n = cvmidi_msg(p[i], m);
        CHECK(n >= 2 && !memcmp(m, all + at, (size_t)n));
        at += n;
    }
    CHECK(at == (int)sizeof all);
}

// ---- player 1 --------------------------------------------------------------------------------
// cvinput.h with PADS (this file is built with CVUSB_PADS=1): pad 1 is this
static gamepad_t usb_pad;
static bool usb_pad_in;
bool cvusb_pad(int n, gamepad_t *p) { if (n || !usb_pad_in) return false; *p = usb_pad; return true; }
uint32_t cvusb_pad_dirs(const gamepad_t *p) { return p->buttons & (GP_UP | GP_DOWN | GP_LEFT | GP_RIGHT); }

static void test_player(void) {
    cvin_t in;
    cvin_init(&in);
    // the screen kernel: up and the left fire (low = pressed), twice before it counts
    CHECK(!cvin_report(&in, REPORT_KIND_JOY, 0x1F & ~(0x01 | 0x10)));
    cvin_update(&in);
    CHECK(in.held == 0);
    CHECK(cvin_report(&in, REPORT_KIND_JOY, 0x1F & ~(0x01 | 0x10)));
    cvin_update(&in);
    CHECK(in.held == (CVIN_UP | CVIN_FIRE_L) && cvin_pressed(&in) == (CVIN_UP | CVIN_FIRE_L) && cvin_pressed(&in) == 0);
    // the keypad's 5 (lines 0011) with the right fire
    cvin_report(&in, REPORT_KIND_KEY, 0x03);
    cvin_report(&in, REPORT_KIND_KEY, 0x03);
    cvin_update(&in);
    CHECK(in.key == 5 && (in.held & CVIN_FIRE_R) && cvin_key(&in) == 5 && cvin_key(&in) == -1);
    CHECK(!cvin_report(&in, REPORT_KIND_HELLO, 0));
    // the frame kernel: right and the right fire; keypad * (lines 1001)
    cvin_frame(&in, 0x0F & ~0x02, 0x03 & ~0x02, 0x9);
    cvin_update(&in);
    CHECK(in.held == (CVIN_RIGHT | CVIN_FIRE_R) && in.key == CVIN_STAR);
    cvin_frame(&in, 0x0F, 0x03, 0xF);                   // let go
    cvin_update(&in);
    CHECK(in.held == 0 && in.key == -1);
    // USB pad 1: d-pad left, B (south) the left fire, A (east) the right, X (north) *
    usb_pad_in = true;
    usb_pad.buttons = GP_LEFT | GP_SOUTH;
    cvin_pressed(&in);
    cvin_update(&in);
    CHECK(in.held == (CVIN_LEFT | CVIN_FIRE_L) && in.pad_held == in.held && in.console_held == 0);
    CHECK(cvin_pressed(&in) == (CVIN_LEFT | CVIN_FIRE_L));
    usb_pad.buttons = GP_EAST | GP_NORTH;
    cvin_frame(&in, 0x0F & ~0x01, 0x03, 0xF);           // and the console's stick up
    cvin_update(&in);
    CHECK(in.held == (CVIN_UP | CVIN_FIRE_R) && in.key == CVIN_STAR);
    usb_pad.buttons = GP_WEST;
    cvin_frame(&in, 0x0F, 0x03, 0xD);                   // the console's 1 wins over the pad's #
    cvin_update(&in);
    CHECK(in.key == 1);
    cvin_frame(&in, 0x0F, 0x03, 0xF);
    cvin_update(&in);
    CHECK(in.key == CVIN_HASH);
    usb_pad_in = false;
    cvin_update(&in);
    CHECK(in.held == 0 && in.key == -1);
}

int main(void) {
    test_keyboard();
    test_mouse();
    test_pads();
    test_midi_desc();
    test_midi_in();
    test_midi_out();
    test_player();
    printf("cvusb: %d checks, %s\n", checks, fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
