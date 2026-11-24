// cvkernel.c -- see cvkernel.h. From midihello.c (PSG_SYNTH_CHAINED), whose
// comments have the measurements behind each choice.
#include "cvkernel.h"

#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "cvhome.h"
#include "cvlaunch.h"
#include "../sdplayer/htrace.h"
#include "../midihello/mailbox.h"

#define NCART_PIN   15
#define NCART_BIT   (1u << NCART_PIN)
#define PD_SHIFT    16
#define PD_MASK     (0xffu << PD_SHIFT)
#define LED_PIN     PICO_DEFAULT_LED_PIN
#define LED_BIT     (1u << LED_PIN)
#define OFF_US      50000           // nCART low this long = console off
#define BOOT_US     1000000         // a header read this long after core 1 starts is RESET
#define PARK_US     60000           // RESET: not parked this long after, the BIOS took the title path
#define PARK_GIVE_UP_US 20000000    // ...and reboot into the menu anyway after this
#define REPORT_GAP_US 5000          // the kernel reads each report twice: one a frame counts
#define SETTLED 3

static uint8_t rom_ram[0x8000] __attribute__((aligned(4)));
static uint8_t handoff_ram[0x8000] __attribute__((aligned(4)));   // handoff.asm, RST 0 past it
static volatile bool to_menu;               // core 1 -> core 0: RESET, the handoff is served
static uint32_t t_serve;                    // when core 1 started

static volatile uint32_t report[4], report_n[4];
static uint32_t win_mask, win_val, a5_gpio, a6_gpio;
static uint8_t line_gpio[15];

// $8000 (GP0-15 all low), settled: the BIOS reading the header. Within a
// second of starting, that is the kernel booting; later, RESET.
static bool __scratch_x("serve") __attribute__((noinline)) on_header(void) {
    for (uint i = 0; i < SETTLED; i++)
        if (sio_hw->gpio_in & 0xffffu) return false;
    return timer_hw->timerawl - t_serve > BOOT_US;
}

static ht_unperm_t ht_u;
static volatile bool parked;
static void __attribute__((noinline)) __not_in_flash_func(serve_traced)(const uint8_t *img, int p) {
    ht_phase_t *ph = &HTR->ph[p];
    uint32_t prev = ~0u, same = 0, park_n = 0;
    bool done = p == HT_Y_RESET;
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        sio_hw->gpio_out = (uint32_t)img[in & 0x7fffu] << PD_SHIFT | LED_BIT;
        uint32_t pins = in & 0xffffu;
        if (pins & NCART_BIT) { done = false; prev = ~0u; continue; }
        if (done) continue;
        if (pins != prev) { prev = pins; same = 0; continue; }
        if (++same < 2) continue;
        done = true;
        uint32_t off = ht_off(&ht_u, pins), t = timer_hw->timerawl;
        if (off == 0) {
            if (p == HT_Y_BOOT && t - t_serve > BOOT_US) {
                img = handoff_ram;          // RESET: both headers are 55 AA
                to_menu = true;
                p = HT_Y_RESET;
                ph = &HTR->ph[p];
            }
            ht_hdr(ph, t, img[0]);
            if (p == HT_Y_BOOT) return;     // the BIOS boots the kernel
            continue;
        }
        ht_bus(ph, t, off, img[pins & 0x7fffu]);
        if (p != HT_Y_RESET || !ht_park_read(off)) park_n = 0;
        else if (++park_n == HT_PARKED) parked = true;
    }
}

static void __scratch_x("serve") __attribute__((noreturn)) serve(void) {
    const uint32_t mask = win_mask, val = win_val, a5 = a5_gpio, a6 = a6_gpio;
    const uint8_t *img = rom_ram;
    uint32_t prev = 0, same = 0;
    save_and_disable_interrupts();
    t_serve = timer_hw->timerawl;
    serve_traced(rom_ram, HT_Y_BOOT);       // returns as the kernel boots
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        sio_hw->gpio_out = (uint32_t)img[in & 0x7fffu] << PD_SHIFT | LED_BIT;
        if ((in & mask) != val) {
            prev = 0;
            if (!(in & 0xffffu) && on_header()) {       // RESET: the handoff, from the next sample on
                to_menu = true;
                serve_traced(handoff_ram, HT_Y_RESET);
            }
            continue;
        }
        in &= 0xffffu;
        if (in != prev) {
            prev = in;
            same = 1;
        } else if (++same == SETTLED) {
            uint32_t k = (in >> a5 & 1) | (in >> a6 & 1) << 1;
            report[k] = in;
            report_n[k] = report_n[k] + 1;
        }
    }
}

// ---- the mailbox ---------------------------------------------------------------
static uint8_t perm_lut[256];               // console byte -> GP16-23 order
static uint16_t fidx_lo[256], fidx_hi[128]; // ROM offset (A0-A7, A8-A14) -> rom_ram index bits
static uint8_t mb[MB_SIZE], mb_live[MB_SIZE];

const uint8_t cvk_silence[MB_SND_LEN] = {0x80, 0x00, 0x9F, 0xA0, 0x00, 0xBF, 0xC0, 0x00, 0xDF, 0xFF};

static inline uint32_t findex(uint32_t a) { return fidx_lo[a & 0xFF] | fidx_hi[a >> 8 & 0x7F]; }

static void mailbox_setup(const uint8_t *addr_line, const uint8_t *data_bit) {
    for (uint v = 0; v < 256; v++) {
        uint8_t p = 0;
        for (uint k = 0; k < 8; k++) if (v >> data_bit[k] & 1) p |= (uint8_t)(1u << k);
        perm_lut[v] = p;
    }
    for (uint g = 0; g < 15; g++) line_gpio[addr_line[g]] = (uint8_t)g;
    for (uint v = 0; v < 256; v++) {
        uint32_t lo = 0, hi = 0;
        for (uint l = 0; l < 8; l++) if (v >> l & 1) lo |= 1u << line_gpio[l];
        for (uint l = 0; l < 7; l++) if (v >> l & 1) hi |= 1u << line_gpio[8 + l];
        fidx_lo[v] = (uint16_t)lo;
        if (v < 128) fidx_hi[v] = (uint16_t)hi;
    }
    win_val = 0;
    for (uint l = 7; l < 15; l++) win_val |= 1u << line_gpio[l];
    win_mask = win_val | NCART_BIT;
    a5_gpio = line_gpio[5];
    a6_gpio = line_gpio[6];
}

static inline void mb_put(uint i, uint8_t v) {
    rom_ram[findex(MB_BASE - 0x8000 + i)] = perm_lut[v];
    mb_live[i] = v;
}

void cvk_mb(unsigned i, uint8_t v) { if (i < MB_SIZE) mb[i] = v; }

void cvk_publish(void) {
    for (uint i = 0; i < MB_SIZE; i++)
        if (i != MB_SND_SEQ && i != MB_LIST_SEQ && i != MB_NSEQ && mb[i] != mb_live[i]) mb_put(i, mb[i]);
    __dmb();
    if (mb[MB_NSEQ] != mb_live[MB_NSEQ]) mb_put(MB_NSEQ, mb[MB_NSEQ]);
    __dmb();
    if (mb[MB_SND_SEQ] != mb_live[MB_SND_SEQ]) mb_put(MB_SND_SEQ, mb[MB_SND_SEQ]);
    if (mb[MB_LIST_SEQ] != mb_live[MB_LIST_SEQ]) mb_put(MB_LIST_SEQ, mb[MB_LIST_SEQ]);
}

// the PSG's 10 bytes (mailbox.h's SND: tone and volume latches), then SND_SEQ
void cvk_sound(const uint8_t snd[MB_SND_LEN]) {
    memcpy(mb + MB_SND, snd, MB_SND_LEN);
    mb[MB_SND_SEQ]++;
    cvk_publish();
}

void cvk_list(const uint8_t *list, size_t n, bool full, uint8_t seq) {
    for (size_t i = 0; i < n; i++) rom_ram[findex(LIST_BASE - 0x8000 + i)] = perm_lut[list[i]];
    mb[MB_LIST_FULL] = full;
    mb[MB_LIST_SEQ] = seq;
    __dmb();
    cvk_publish();
}

// ---- reports --------------------------------------------------------------------------
static uint32_t seen[4], heard_at[4];

bool cvk_report(uint32_t t, int *kind, uint8_t *bits) {
    for (uint k = 0; k < 4; k++) {
        uint32_t c = report_n[k];
        if (c == seen[k]) continue;
        seen[k] = c;
        if (t - heard_at[k] < REPORT_GAP_US) continue;     // the same frame's second read
        heard_at[k] = t;
        uint32_t in = report[k];
        uint8_t a = 0;
        for (uint l = 0; l < 6; l++) if (in >> line_gpio[l] & 1) a |= (uint8_t)(1u << l);
        *kind = (int)k;
        *bits = a;
        return true;
    }
    return false;
}

// ---- console power, and back to the menu ----------------------------------------------
static bool driving, console_off;
static uint32_t last_high;
static char note[33];

bool cvk_console_on(void) { return driving; }

const cvl_t *cvk_launch(void) {
    const cvl_t *b = CVL_BLOCK;
    return cvl_ok(b) && b->magic == CVL_LAUNCH ? b : NULL;
}

void cvk_note(const char *s) {
    memset(note, 0, sizeof note);
    if (s) strncpy(note, s, sizeof note - 1);
}

static inline void watch(uint32_t in, uint32_t t) {
    if (!(in & NCART_BIT) && driving && t - last_high > OFF_US)
        for (int i = 0; i < 256 && !(in & NCART_BIT); i++) in = sio_hw->gpio_in;
    if (in & NCART_BIT) {
        last_high = t;
        if (!driving) {
            sio_hw->gpio_oe_set = PD_MASK;
            driving = true;
            ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_DRIVE, 0);
        }
    } else if (driving && t - last_high > OFF_US) {
        sio_hw->gpio_oe_clr = PD_MASK;
        driving = false;
        console_off = true;
        ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_UNDRIVE, t - last_high);
    }
    bool led = driving || (t >> 19 & 1);
    if (led) sio_hw->gpio_oe_set = LED_BIT;
    else     sio_hw->gpio_oe_clr = LED_BIT;
}

// the menu is the flash's boot image (a watchdog reboot is a normal boot);
// the outputs go off first, so the parked console reads $00 until it's served
static void __attribute__((noreturn)) to_menu_now(uint32_t why) {
    multicore_reset_core1();
    ht_mark(HT_Y_RESET, timer_hw->timerawl, HM_REBOOT, why);
    sio_hw->gpio_oe_clr = PD_MASK;
    cvl_t *b = CVL_BLOCK;
    if (cvl_ok(b)) cvl_return(b, note[0] ? note : NULL);   // started by the menu: what to say
    watchdog_reboot(0, 0, 1);
    for (;;) tight_loop_contents();
}

// ---- back to the menu from the controllers (cvhome.h) ----------------------------------
bool cvpad_held(uint32_t buttons) __attribute__((weak));   // cvpad.c, if the plugin has USB pads
static cvhome_t home;
static bool home_off, home_forced;
static uint32_t t_home;

void cvk_home(bool on) { home_off = !on; }

// the kernel's last report of a kind, A0-A5 (cvk_report()'s bits), without taking it
static uint8_t report_bits(uint32_t k) {
    const uint32_t in = report[k];
    uint8_t a = 0;
    for (uint l = 0; l < 6; l++) if (in >> line_gpio[l] & 1) a |= (uint8_t)(1u << l);
    return a;
}

// controller 1: the joystick report's A4 is the left fire, the keypad's the
// right one (low = pressed), the keypad's A0-A3 its lines
static bool console_held(void) {
    if (!report_n[REPORT_KIND_JOY] || !report_n[REPORT_KIND_KEY]) return false;
    const uint8_t j = report_bits(REPORT_KIND_JOY), k = report_bits(REPORT_KIND_KEY);
    return cvhome_console(!(j & 0x10), !(k & 0x10), k);
}

static void home_task(uint32_t t) {
    if (!t_home) {
        if (home_off) return;
        const bool held = console_held() || (cvpad_held && cvpad_held(CVHOME_PAD));
        if (!cvhome_step(&home, t, held)) return;
        t_home = t | 1;
        rom_ram[findex(CVHOME_NMI - 0x8000)] = perm_lut[0xC7];  // the next NMI restarts the BIOS
        ht_mark(HT_Y_BOOT, t, HM_RESET, 1);
    } else if (!home_forced && t - t_home > CVHOME_FORCE_US) {
        home_forced = true;                 // no NMI: RST 0 wherever it runs; the header stays 55 AA
        for (uint32_t a = 2; a < sizeof rom_ram; a++) rom_ram[findex(a)] = perm_lut[0xC7];
    }
}

static uint32_t t_menu;
static bool titled;

void cvk_task(uint32_t t) {
    watch(sio_hw->gpio_in, t);
    if (to_menu && !t_menu) {
        t_menu = t | 1;
        ht_mark(HT_Y_RESET, t, HM_RESET, 0);
    }
    if (console_off) to_menu_now(1);
    if (!t_menu) {
        if (driving) home_task(t);
        return;
    }
    int32_t since = (int32_t)(t - t_menu);
    if (parked) {
        ht_mark(HT_Y_RESET, t, HM_PARKED, 0);
        to_menu_now(2);
    }
    if (!titled && since > PARK_US) {      // it missed the header: the title path reads AA 55 at 252 ms
        titled = true;
        handoff_ram[0] = perm_lut[0xAA];
        handoff_ram[findex(1)] = perm_lut[0x55];
        ht_mark(HT_Y_RESET, t, HM_TITLE_PATH, 0);
    }
    if (since > PARK_GIVE_UP_US) to_menu_now(3);
}

void cvk_start(const cvk_config_t *c) {
    __asm volatile ("cpsie i" ::: "memory"); // as after a reset, whatever the menu's chain left
    for (uint p = 0; p < 15; p++) {         // address lines: plain inputs, driven at 5V
        gpio_init(p);
        gpio_disable_pulls(p);
    }
    gpio_init(NCART_PIN);
    gpio_pull_down(NCART_PIN);              // reads low while U5 is unpowered
    for (uint p = PD_SHIFT; p < PD_SHIFT + 8; p++)
        gpio_init(p);                       // outputs only once watch() sees the console
    gpio_init(LED_PIN);
    memcpy(rom_ram, c->image, sizeof rom_ram);
    mailbox_setup(c->addr_line, c->data_bit);
    memset(handoff_ram, perm_lut[0xC7], sizeof handoff_ram);
    for (uint32_t a = 0; a < c->handoff_len && a < sizeof handoff_ram; a++) handoff_ram[findex(a)] = perm_lut[c->handoff[a]];
    ht_init(false);                         // the menu started it: add this side
    ht_clear(HT_Y_BOOT);
    ht_clear(HT_Y_RESET);
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_MAIN, 0);
    ht_unperm_init(&ht_u, c->addr_line);
    memcpy(mb + MB_SND, cvk_silence, MB_SND_LEN);   // the raw PSG bytes: zeros would be a tone
    for (uint i = 0; i < MB_SIZE; i++) mb_put(i, mb[i]);
    __dmb();
    multicore_launch_core1(serve);
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_SERVE, 0);
    watch(sio_hw->gpio_in, time_us_32());   // drive the bus now if the console is on
}
