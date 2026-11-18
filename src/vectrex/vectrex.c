// vectrex -- the GCE Vectrex on the ColecoVision: the cart runs the machine
// (../vx.c) from the user's own system ROM on the card and the cartridge
// the menu opened, and sends the picture and the sound to the console as
// the Game Boy kernel's frame lists (../vxcv.c). RESET goes back to the menu.
//
//   core 1  serves the kernel and a list a TV frame (cvframes.c)
//   core 0  this loop: for each TV frame the console starts, a frame's worth
//           of the 6809 and the VIA, the beam's newest refresh drawn and
//           what changed of it, the sound, as the next list
//
// The system ROM (8K) is read from the card's /VECTREX folder, known by its
// CRC-32 (../vxcard.h). The file the menu opened is the cartridge; started
// with none, or with the system ROM itself, the ROM's own game runs (Mine
// Storm).
//
// Controls (../vxpad.h): the stick is the controller's joystick or a USB
// pad's left stick; buttons 1 to 4 are keypad 1 to 4 (3 and 4 also the two
// fire buttons) or the pad's left, top, bottom and right face buttons;
// keypad 0 or L changes the beam's colour. A second pad is player 2.
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/structs/sio.h"
#include "hardware/watchdog.h"

#include "cvdiag.h"
#include "cvframes.h"
#include "cvpad.h"
#include "cvsd.h"
#include "vdp.h"
#include "gbkernel.h"
#include "handoff.h"
#include "vxcv.h"
#include "vxpad.h"
#include "vxcard.h"
#include "pins.h"

#define OPEN_US     20000           // serve the console this long before the card's (blocking) start
#define WATCHDOG_MS 4000            // a hang reboots into the menu
#define NOTE_US     1000000         // the menu's note is brought up to date this often
#define DRAW_BY_US  10500           // a new picture is drawn only if the machine's frame was done by now

static vxcv_t enc;
static uint8_t list[VXCV_LIST_MAX];
static size_t list_len;
static bool list_pending;                   // built, not published yet
static uint32_t seen_frames;
static fs_t fs;
// a message's canvas is the pictures' room: the messages all come before the machine starts
#define canvas (*(canvas_t *)(void *)vx_scratch())
_Static_assert(sizeof(canvas_t) <= VX_SCRATCH, "the canvas fits the pictures' room");

// ---- the start, watched ---------------------------------------------------------------
// About one start in ten on a console showed only the top half of the
// picture: the first list, the one with the name table, hadn't done all its
// work, and nothing on the Mac does that (../README.md, "The start").
// So the cart looks at how the console takes the first list, from the
// pads' millisecond timer (this core's interrupt: core 1's loop is as it
// was), and says so in the menu's note:
//   reached/len  the furthest byte of the first list the console was seen
//                to read, of its length. Each millisecond while the list
//                runs the pins are looked at for some 12 us, long enough to
//                see one of its bytes read: a list run whole gets to within
//                200 of its end, one the console left early stops short.
//   g            frames the console started less than 12 ms after the one
//                before, in the first two seconds: a frame's start core 1
//                saw that the console didn't make (the kernel starts one
//                every 16.7 ms), which puts the next list at $C000 under
//                the one that is running
//   j            the same, two within a millisecond (at a frame's own
//                start: harmless but for the count), all the time
#define SW_NCART    (1u << 15)
#define SW_A14      (1u << GPIO_A14)
#define SW_REPORTS  0x3FC0          // $FFC0 on: the controller and the frame's start, not the list
#define SW_WINDOW   16000           // us from the first list's taking being seen (the next frame is 16,690 after it)
#define SW_LOOKS    300
#define SW_EARLY    120             // frames
static struct {
    volatile uint8_t state;         // 0: nothing out yet; 1: the first list is; 2: the console runs it; 3: done
    uint16_t len, reached;
    uint32_t t_take, f_last, t_last, frames;
    bool any;
    uint8_t shorts, doubles;
    uint16_t lo[256], hi[128];      // GPIO 0-7 and 8-14 to the address
} sw;

static void sw_init(void) {
    for (unsigned v = 0; v < 256; v++) {
        uint16_t a = 0, b = 0;
        for (unsigned g = 0; g < 8; g++) {
            if (!(v >> g & 1)) continue;
            a |= (uint16_t)(1u << addr_line[g]);
            if (g < 7) b |= (uint16_t)(1u << addr_line[8 + g]);
        }
        sw.lo[v] = a;
        if (v < 128) sw.hi[v] = b;
    }
}

static void see_taken(uint32_t t);

static void __not_in_flash_func(sw_tick)(void) {
    uint32_t t = time_us_32(), f = cvf_frames();
    see_taken(t);
    if (f != sw.f_last) {
        if (sw.any) {
            if (f - sw.f_last > 1) { if (sw.doubles < 9) sw.doubles++; }
            else if (t - sw.t_last < 12000 && sw.frames < SW_EARLY && sw.shorts < 9) sw.shorts++;
        }
        sw.any = true;
        sw.frames += f - sw.f_last;
        sw.f_last = f;
        sw.t_last = t;
    }
    if (sw.state == 1 && !cvf_waiting()) {
        sw.state = 2;
        sw.t_take = t;
    }
    if (sw.state != 2) return;
    if (t - sw.t_take > SW_WINDOW) { sw.state = 3; return; }
    for (int i = 0; i < SW_LOOKS; i++) {
        uint32_t in = sio_hw->gpio_in;
        if ((in & (SW_NCART | SW_A14)) != SW_A14) continue;         // not a read of $C000-$FFFF
        if ((sio_hw->gpio_in ^ in) & 0xFFFF) continue;              // (the pins on the move)
        uint32_t off = (uint32_t)(sw.lo[in & 0xFF] | sw.hi[in >> 8 & 0x7F]) & 0x3FFF;
        if (off >= SW_REPORTS) continue;
        if (off > sw.reached) sw.reached = (uint16_t)off;
        break;
    }
}

// ---- the console, a list a TV frame ------------------------------------------------
// A list goes out only once the one before it has had its time on the
// console (13 ms at most, to be in time for the next frame). In a game that
// is so anyway: a frame of the machine takes 9 ms, a list 5. At the start
// it wasn't: the cart had the second list out a millisecond into the first,
// the long one with the name table, and from then on a frame's start that
// core 1 only thought it saw would have put it at $C000 under the kernel
// (../README.md, "The start"). When the console took a list is seen here
// and, to the millisecond whatever this loop is doing, by the pads' timer.
#define HOLD_MAX_US 13000
static volatile bool list_out;              // one is out and not seen taken yet
static volatile uint32_t t_taken;           // when the last one was
static uint32_t hold_us;                    // the time it wants from then

static void __not_in_flash_func(see_taken)(uint32_t t) {
    if (list_out && !cvf_waiting()) {
        t_taken = t;
        list_out = false;
    }
}

static void publish(void) {
    see_taken(time_us_32());
    if (!list_pending || list_out || time_us_32() - t_taken < hold_us || !cvf_publish(list, list_len)) return;
    list_pending = false;
#ifndef VX_PUBLISH_NOW                      // (a test build's: lists out as soon as they can be, as 0.1.0's were)
    hold_us = MIN(enc.stats.t_last * 143 / 512 + 150, (uint32_t)HOLD_MAX_US);   // (T to us, and the kernel's way to the list)
#endif
    list_out = true;
    if (!sw.state) {
        sw.len = (uint16_t)list_len;
        sw.state = 1;
    }
}

// TV frames the console has started since the last call (0: none yet, or
// the last list isn't out)
static uint32_t new_frames(void) {
    cvpad_task();
    cvf_task(time_us_32());
    watchdog_update();
    publish();
    if (list_pending) return 0;
    uint32_t f = cvf_frames(), n = f - seen_frames;
    seen_frames = f;
    return n;
}

static void send(void) {
    list_len = vxcv_tv_frame(&enc, list);
    list_pending = true;
    publish();
}

static void quiet_frame(void) {
    static const vx_ay_t quiet;
    vxcv_sound(&enc, &quiet, NULL, 0, false);
    send();
}

// ---- messages ---------------------------------------------------------------------
static void centre(int y, const char *s, uint8_t colour, int scale) {
    int w = (int)strlen(s) * 8 * scale;
    canvas_text(&canvas, w < VDP_W ? (VDP_W - w) / 2 : 0, y, s, colour, scale);
}

static void message(const char *head, const char *l1, const char *l2, const char *l3) {
    canvas_init(&canvas);
    centre(32, "VECTREX", C_LYELLOW, 3);
    if (head) centre(80, head, C_WHITE, 1);
    if (l1) centre(100, l1, C_GRAY, 1);
    if (l2) centre(112, l2, C_GRAY, 1);
    if (l3) centre(124, l3, C_GRAY, 1);
    centre(176, "RESET: BACK TO THE MENU", C_LBLUE, 1);
    vxcv_canvas(&enc, &canvas.px[0][0]);
}

// serve the console, a list a frame, for us microseconds (forever: 0)
static void serve_for(uint32_t us) {
    uint32_t t0 = time_us_32();
    while (!us || time_us_32() - t0 < us)
        if (new_frames()) quiet_frame();
}

static void __attribute__((noreturn)) fail(const char *why, const char *l2, const char *l3, const char *menu_note) {
    message("IT CAN'T START", why, l2, l3);
    cvf_note(menu_note);
    serve_for(0);
    for (;;) tight_loop_contents();
}

// between a file's sectors: the console keeps its picture
static void card_tick(void) {
    if (new_frames()) quiet_frame();
}

// ---- the game -----------------------------------------------------------------------
static uint8_t last_key = VXKEY_NONE;
static uint32_t last_usb, seen_pic, cyc_frac;
static uint32_t late, us_max, pics, pics_at_note;
static int16_t samples[VX_SAMPLE_MAX];

static void game_frame(uint32_t elapsed) {
    uint32_t t0 = time_us_32();
    uint8_t joy, fire, key;
    cvf_input(&joy, &fire, &key);
    gamepad_t g1, g2;
    bool have1 = cvpad_get(0, &g1), have2 = cvpad_get(1, &g2);
    bool tint = key == VXKEY_0 && last_key != VXKEY_0;      // the beam's colour
    last_key = key;
    if (have1) {
        if (g1.buttons & ~last_usb & GP_L) tint = true;
        last_usb = g1.buttons;
    } else last_usb = 0;
    if (tint) vxcv_set_tint(&enc, enc.tint + 1);
    vx_inputs_t in;
    vxpad_frame(joy, fire, key, have1 ? &g1 : NULL, have2 ? &g2 : NULL, &in);
    vx_inputs(&in);

    // the machine, for the frames since the last list (two at most: a slow
    // stretch slows the game rather than piling up)
    if (elapsed > 2) elapsed = 2;
    cyc_frac += VX_FRAME_CYCLES_Q8 * elapsed;
    vx_run(cyc_frac >> 8);
    cyc_frac &= 255;

    const vx_vec_t *v;
    int nv;
    uint32_t pic = vx_picture(&v, &nv);
    static uint8_t put_off;
    if (pic != seen_pic || tint) {
        // a frame whose machine took most of its time already leaves the
        // drawing to the next, twice at most: the list is out in time, and
        // the console keeps the picture it has
        if (time_us_32() - t0 > DRAW_BY_US && put_off < 2) {
            put_off++;
        } else {
            const vx_text_t *t;
            int nt;
            vx_text(&t, &nt);
            put_off = 0;
            pics += pic - seen_pic;
            seen_pic = pic;
            vxcv_vectors(&enc, v, nv, t, nt);
        }
    }
    vx_ay_t ay;
    vx_ay(&ay);
    bool playing;
    int ns = vx_samples(samples, VX_SAMPLE_MAX, &playing);
    vxcv_sound(&enc, &ay, samples, ns, playing);
    send();
    uint32_t us = time_us_32() - t0;
    if (us > us_max) us_max = us;
}

// the menu's bottom line, should RESET come: the game's pictures a second,
// TV frames the cart was late for, its slowest frame
static void update_note(void) {
    static uint32_t t_last, late_last;
    uint32_t t = time_us_32();
    if (t - t_last < NOTE_US) return;
    unsigned per_s = MIN(pics - pics_at_note, 99u), dl = MIN(late - late_last, 99u), ms = MIN((us_max + 500) / 1000, 99u);
    char n[48];
    // (32 characters at most: "VX 99/s 99L 99ms 2999/2999 g9j9" is 31)
    snprintf(n, sizeof n, "VX %u/s %uL %ums %u/%u g%uj%u", per_s, dl, ms, (unsigned)sw.reached, (unsigned)sw.len, (unsigned)sw.shorts,
             (unsigned)sw.doubles);
    cvf_note(n);
    t_last = t;
    late_last = late;
    pics_at_note = pics;
    us_max = 0;
}

int main(void) {
    static const cvf_config_t cfg = {gbkernel_bin, GBKERNEL_BIN_LEN, handoff_bin, HANDOFF_BIN_LEN};
    cvf_start(&cfg);
    cvd_start();
    cvd_stage(1);
    cvpad_init();
    sw_init();
    cvpad_set_tick(sw_tick);
    vxcv_init(&enc);
#ifdef VX_NAMES_HOLD
    enc.names_hold = VX_NAMES_HOLD;         // (a test build: a name table the console lost stays lost this many frames, to be seen,
    enc.names_quick = true;                 // and it goes out as 0.1.0's did)
#endif
    message("LOOKING FOR THE SYSTEM ROM", "IN " VXCARD_HOME " ON THE CARD", NULL, NULL);
    serve_for(OPEN_US);

    if (!cvsd_card_in() || !cvsd_init())
        fail(cvsd_card_in() ? "THE CARD DIDN'T ANSWER" : "NO CARD IN THE SLOT", NULL, NULL, "Vectrex: no card");
    if (fs_mount(&fs, cvsd_read) != FS_OK) fail("NO FAT OR EXFAT ON THE CARD", NULL, NULL, "Vectrex: the card didn't mount");
    cvd_stage(2);
    watchdog_enable(WATCHDOG_MS, true);

    // the cartridge: the file the menu opened, if any
    const cvl_t *b = cvf_launch();
    uint32_t cart_size = 0;
    bool have_rom = false;
    if (b && b->path[0]) {
        fs_file_t f;
        cvsd_launched_file(b, &f);
        if (f.size > VX_CART_MAX) fail("THAT FILE IS TOO BIG", "A CARTRIDGE IS 64K AT MOST", NULL, "Vectrex: the file is over 64K");
        int32_t n = vxcard_read(&fs, &f, vx_cart(), VX_CART_MAX, card_tick);
        if (n < 0) fail("THE FILE DIDN'T READ", NULL, NULL, "Vectrex: the file didn't read");
        cart_size = (uint32_t)n;
        // the system ROM itself, opened as a game: the ROM's own game
        if (vxcard_system_rom(vx_cart(), cart_size)) {
            memcpy(vx_rom(), vx_cart(), VX_ROM_SIZE);
            have_rom = true;
            cart_size = 0;
        }
    }
    cvd_stage(3);
    if (!have_rom && !vxcard_load_rom(&fs, card_tick))
        fail("THE SYSTEM ROM ISN'T ON THE CARD", "PUT ITS 8K FILE IN " VXCARD_HOME, "(EXEC_ROM.BIN, OR MINE STORM)",
             "Vectrex: no system ROM in " VXCARD_HOME);
    vx_init(cart_size, (uint16_t)time_us_32());
    while (list_pending) new_frames();      // (the message's last list out before the game's first is made)
    cvd_stage(4);
    cvf_note("Vectrex");

    for (;;) {
        uint32_t n = new_frames();
        if (!n) continue;
        if (n > 1) late += n - 1;
        game_frame(n);
        update_note();
    }
}
