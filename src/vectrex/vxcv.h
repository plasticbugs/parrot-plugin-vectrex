// vxcv: the cart's side of the Vectrex on the ColecoVision. Portable C (no
// pico-sdk), used by the plugin and by the Mac tests. Its lists, its way of
// sending only what changed and its DAC are the Star Wars plugin's
// (../starwars/swcv.c), unchanged; the picture's drawing and the sound's
// mapping are the Vectrex's.
//
// The console runs gbkernel (coleco/gbcv/kernel/gbkernel.asm): each TV frame
// it takes a command list from the cart and runs it (format and costs in its
// header). This makes those lists:
//
//   once:                 vxcv_init(e)
//   each new picture:     vxcv_vectors(e, v, n)     (or vxcv_canvas for a message)
//   each TV frame:        vxcv_sound(e, ay, samples, nsamples, playing)
//                         len = vxcv_tv_frame(e, list)
//
// The picture. The beam's lines are drawn into a 256 x 192 bitmap, upright
// in its middle, and the TMS9918 shows it in Graphics II with name i = tile
// i: one pattern byte and one colour byte for every 8 x 1 pixels. A list
// carries the bytes that differ from what the console holds, as many as the
// frame has time for (about 1,700); a picture that needs more takes two or
// three frames, and the newest picture is always the one going in. A cell's
// colour is its brightest vector's.
//
// The sound. The AY-3-8912's three tones and its noise play on the
// SN76489's, set once a frame. While a game plays samples through the DAC,
// tones 1 and 2 are parked at a period of 1 and their two attenuators make
// a DAC: VXCV_PCM_N levels a frame, spread evenly through the list between
// the VRAM runs, and the list is cut to end exactly one sample period before
// the next begins, so the console runs list after list without waiting for
// the vertical blank.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vx.h"

#define VXCV_W      256
#define VXCV_H      192
#define VXCV_BYTES  6144            // pattern bytes, and as many colour bytes
#define VXCV_LIST_MAX 3000          // (every command costs 23 T or more a byte, in a frame of 59,736)
#define VXCV_PCM_N  100             // sample levels a TV frame while the DAC plays: 5,990 a second

// the Vectrex's screen on the TV's: VXCV_PW pixels for 2 x VXCV_XR units
// across, the 192 lines for 2 x VXCV_YR (vx.h has the units)
#define VXCV_PW     160
#define VXCV_XR     135
#define VXCV_YR     165
#define VXCV_TINTS  6               // white, blue, green, yellow, red, magenta

#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define VXCV_HOT __attribute__((section(".time_critical.vxcv")))
#else
#define VXCV_HOT
#endif

typedef struct {
    uint32_t tv_frames, pictures, pat_bytes, col_bytes, runs, refreshed, pcm_frames;
    uint32_t settled;               // frames that ended with nothing left to send
    uint32_t t_max;                 // the longest list, in T
    uint32_t t_last;                // the last one
} vxcv_stats_t;

typedef struct {
    // the console's VRAM as the lists so far leave it, and the newest picture
    uint8_t vram_pat[VXCV_BYTES], vram_col[VXCV_BYTES];
    uint8_t want_pat[VXCV_BYTES], want_col[VXCV_BYTES];
    bool colour;                    // (always: a colour byte for each cell that has pixels)
    uint8_t tint;                   // the beam's colour, 0 to VXCV_TINTS - 1
    bool names_done;
    uint16_t names_at;
    uint16_t cursor;                // where the next list's scan starts
    uint16_t refresh_at;
    uint16_t names_hold;            // the rotation leaves the names out until this many lists (0; a test build's way to see a loss)
    bool names_quick;               // the names in the vertical blank part first, as 0.1.0 sent them (a test build's)
    bool dirty;                     // want differs from vram somewhere (or may)
    bool col_todo;                  // black and white: the colour table isn't all white yet

    // the PSG as the lists so far leave it
    int16_t psg_period[3];          // -1: not known
    int8_t psg_att[4];
    int16_t psg_noise;
    uint8_t voice_key[3];           // which AY channel each tone plays (0: none)
    // this frame's sound
    uint8_t tone_bytes[24];
    uint8_t tone_n;
    bool pcm;                       // tones 1 and 2 are the DAC
    uint8_t pcm_hold;               // frames the DAC stays after the last sample
    uint8_t pcm_gain;               // the DAC's level, 0-32 (it comes and goes over a few frames)
    uint8_t pcm_bytes[VXCV_PCM_N][2];
    int16_t speech_last;
    bool pcm_frame;                 // this frame's list carries samples
    uint8_t resync;                 // lists still to go before the console is back in step with the vertical blank
    vxcv_stats_t stats;
} vxcv_t;

void vxcv_init(vxcv_t *e);
void vxcv_set_tint(vxcv_t *e, int tint);

// a new picture: a refresh's vectors and its raster text (vx_picture(), vx_text())
void vxcv_vectors(vxcv_t *e, const vx_vec_t *v, int n, const vx_text_t *t, int nt);
// a new picture: 256 x 192 pixels, a TMS9918 colour each (0 or 1: black); for messages
void vxcv_canvas(vxcv_t *e, const uint8_t *px);

// this frame's sound: the AY as it is now, and the DAC's sound channel since
// the last call (vx_samples(): about 139 a frame)
void vxcv_sound(vxcv_t *e, const vx_ay_t *ay, const int16_t *samples, int nsamples, bool playing);

// this TV frame's list into out (VXCV_LIST_MAX bytes); returns its length
size_t vxcv_tv_frame(vxcv_t *e, uint8_t *out);

// the picture the console shows once the lists so far have run, a TMS9918
// colour a pixel (tests, previews)
void vxcv_screen(const vxcv_t *e, uint8_t *px);
// bytes the console still needs (tests; a whole scan)
uint32_t vxcv_backlog(const vxcv_t *e);
// the DAC's level for an attenuator pair (tests: the lists' sound as a WAV)
float vxcv_dac_level(int att1, int att2);
