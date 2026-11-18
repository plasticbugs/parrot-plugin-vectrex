// vxcv.c -- see vxcv.h.
#include "vxcv.h"

#include <string.h>

// ---- lists (nescv.c's, from gbkernel.asm's costs) ---------------------------------
#define FRAME_T    (262 * 228)
#define VBLANK_T   (70 * 228)
#define C_START    480
#define C_RUN_VB   106
#define C_BYTE_VB  23
#define C_RUN_ACT  106
#define C_BYTE_ACT 31
#define C_PSG      75
#define C_PSG_BYTE 52
#define C_END_VB   54
#define C_END      52
#define VB_LIMIT   (VBLANK_T - 600)
#define LIST_LIMIT (FRAME_T - 1200)

// A frame of speech: the console goes from one list straight into the next
// (it finds the vertical blank already past), so a list's length is the
// frame's. C_FRAME: the kernel from a list's end mark to the next list's
// first command, not waiting (gbkernel.asm's frame: 45 instructions; MAME's
// console agrees, test/console.sh). PCM_L: the whole of it, a little over a
// TV frame, so the console is always a little late and never waits.
#define C_FRAME    357
#define PCM_L      (FRAME_T + 300)
#define PCM_FIRST  (C_FRAME + C_END_VB)
// a sample's two bytes; then from one sample to the next, to the 1/256 T
#define C_SAMPLE   (C_PSG + 2 * C_PSG_BYTE)
#define PCM_STEP   (((PCM_L - PCM_FIRST - C_SAMPLE - C_END) << 8) / (VXCV_PCM_N - 1))

#define PATTERNS 0x0000
#define COLOURS  0x2000
#define NAMES    0x1800
#define WHITE    0xF1               // white on black

typedef struct {
    uint8_t *b;
    size_t n;
    uint32_t t;
    bool vblank;
} list_t;

static void l_run_head(list_t *l, uint16_t addr, int n) {
    l->t += l->vblank ? C_RUN_VB + n * C_BYTE_VB : C_RUN_ACT + n * C_BYTE_ACT;
    l->b[l->n++] = 1;
    l->b[l->n++] = (uint8_t)addr;
    l->b[l->n++] = (uint8_t)((addr >> 8) & 0x3F) | 0x40;
    l->b[l->n++] = (uint8_t)n;
}

static void l_psg(list_t *l, const uint8_t *d, int n) {
    l->t += C_PSG + C_PSG_BYTE * n;
    l->b[l->n++] = 2;
    l->b[l->n++] = (uint8_t)n;
    memcpy(l->b + l->n, d, n);
    l->n += n;
}

static void l_end_vblank(list_t *l) {
    if (!l->vblank) return;
    l->vblank = false;
    l->t += C_END_VB;
    l->b[l->n++] = 0;
}

// bytes a run may have and still leave the list inside its part of the frame
static int room(const list_t *l) {
    int t = l->vblank ? ((int)VB_LIMIT - (int)l->t - C_END_VB - C_RUN_VB) / C_BYTE_VB
                      : ((int)LIST_LIMIT - (int)l->t - C_END - C_RUN_ACT) / C_BYTE_ACT;
    return t > 255 ? 255 : t;
}

// ---- the picture ----------------------------------------------------------------
// The Vectrex's screen stands upright in the middle of the TV's: VXCV_PW
// pixels across for the +-VXCV_XR units the tube shows from side to side,
// all 192 lines for its +-VXCV_YR up and down. A vector's ends are in 1/64
// units from the middle, y up (vx.h).
#define PX(x) (128 + (((x) * (int32_t)(VXCV_PW * 65536 / (2 * VXCV_XR * VX_UNIT)) + 32768) >> 16))
#define PY(y) (96 - (((y) * (int32_t)(VXCV_H * 65536 / (2 * VXCV_YR * VX_UNIT)) + 32768) >> 16))

// The beam's brightness as one of four colours, brightest first, for each
// tint (an overlay's colour): a colour byte, the colour on black.
static const uint8_t tints[VXCV_TINTS][4] = {
    {0xF1, 0xE1, 0x51, 0x41},       // white: white, grey, light blue, dark blue
    {0x71, 0x51, 0x41, 0x41},       // blue: cyan, light blue, dark blue
    {0x31, 0x21, 0xC1, 0xC1},       // green
    {0xB1, 0xA1, 0xA1, 0x61},       // yellow
    {0x91, 0x81, 0x61, 0x61},       // red
    {0xD1, 0xD1, 0x41, 0x41},       // magenta
};
static inline int z_level(uint8_t z) { return z >= 192 ? 0 : z >= 128 ? 1 : z >= 64 ? 2 : 3; }

// the tube's own edges: what the beam draws past them isn't seen
#define X_MIN (128 - VXCV_PW / 2)
#define X_MAX (128 + VXCV_PW / 2 - 1)
static int outcode(int x, int y) {
    return (x < X_MIN ? 1 : x > X_MAX ? 2 : 0) | (y < 0 ? 4 : y >= VXCV_H ? 8 : 0);
}

// A line, clipped to the tube's part of the screen (Cohen-Sutherland), then Bresenham's with the
// pixel's place in the bitmap kept as a byte address and a bit: a step across
// moves the bit, a step down the address (by 1 inside a cell, by 249 to the
// cell below). col 0: black and white, where the colour plane isn't looked at.
static inline __attribute__((always_inline)) void line(vxcv_t *e, int x0, int y0, int x1, int y1, uint8_t col) {
    int c0 = outcode(x0, y0), c1 = outcode(x1, y1);
    for (int i = 0; i < 8 && (c0 | c1); i++) {
        if (c0 & c1) return;
        int c = c0 ? c0 : c1, x, y;
        if (c & 8) { x = x0 + (x1 - x0) * (VXCV_H - 1 - y0) / (y1 - y0); y = VXCV_H - 1; }
        else if (c & 4) { x = x0 + (x1 - x0) * (0 - y0) / (y1 - y0); y = 0; }
        else if (c & 2) { y = y0 + (y1 - y0) * (X_MAX - x0) / (x1 - x0); x = X_MAX; }
        else { y = y0 + (y1 - y0) * (X_MIN - x0) / (x1 - x0); x = X_MIN; }
        if (c == c0) { x0 = x; y0 = y; c0 = outcode(x0, y0); }
        else { x1 = x; y1 = y; c1 = outcode(x1, y1); }
    }
    if (c0 | c1) return;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
    uint8_t *pat = e->want_pat + ((y0 >> 3) * 32 + (x0 >> 3)) * 8 + (y0 & 7);
    unsigned bit = 0x80u >> (x0 & 7);
    int n = dx > dy ? dx : dy, yin = y0 & 7;
    for (;; n--) {
        *pat |= (uint8_t)bit;
        if (col) pat[e->want_col - e->want_pat] = col;
        if (!n) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            if (sx > 0) { bit >>= 1; if (!bit) { bit = 0x80; pat += 8; } }
            else { bit <<= 1; if (bit == 0x100) { bit = 1; pat -= 8; } }
        }
        if (e2 < dx) {
            err += dx;
            if (sy > 0) { if (++yin == 8) { yin = 0; pat += 249; } else pat++; }
            else { if (--yin < 0) { yin = 7; pat -= 249; } else pat--; }
        }
    }
}

// A run of pixels on one line, in a colour.
static void span(vxcv_t *e, int x, int y, int w, uint8_t col) {
    if (y < 0 || y >= VXCV_H) return;
    for (; w > 0; w--, x++) {
        if (x < 0 || x >= VXCV_W) continue;
        unsigned a = (unsigned)(((y >> 3) * 32 + (x >> 3)) * 8 + (y & 7));
        e->want_pat[a] |= (uint8_t)(0x80 >> (x & 7));
        e->want_col[a] = col;
    }
}

// Raster text (vx.h), a block at a time: its characters' rows a dot a pixel
// where the beam's own scale would put two dots in one, about the middle of
// where the beam drew it. Wider than that already, it is drawn at its size.
// (8.8 pixels throughout.)
static void text_block(vxcv_t *e, const vx_text_t *t, int n) {
    int rows = 0, cols = 0, used = 0;
    const vx_text_t *r1 = NULL, *c1 = NULL;
    for (int i = 0; i < n; i++) {
        if (t[i].row >= rows) rows = t[i].row + 1;
        if (t[i].col >= cols) cols = t[i].col + 1;
        used |= t[i].bits;
        if (!r1 && t[i].row == 1) r1 = &t[i];
        if (!c1 && t[i].row == 0 && t[i].col == 1) c1 = &t[i];
    }
    if (!used) return;
    int lo = 0;
    while (!(used & 1 << lo)) lo++;
    int ncols = 8 - lo;                                     // the columns its characters use
    const int32_t ux = VXCV_PW * 256 / (2 * VXCV_XR), uy = VXCV_H * 256 / (2 * VXCV_YR);   // pixels a unit
    int32_t bit = t[0].pitch * ux / 1024;                   // a bit's width
    int32_t chr = c1 ? (c1->x - t[0].x) * ux / VX_UNIT : 9 * bit;
    int32_t row = r1 ? (t[0].y - r1->y) * uy / VX_UNIT : 256;
    int32_t left = 128 * 256 + t[0].x * ux / VX_UNIT, top = 96 * 256 - t[0].y * uy / VX_UNIT;
    int32_t mid_x = left + (cols * chr) / 2 - (chr - ncols * bit) / 2, mid_y = top + (rows - 1) * row / 2;
    if (bit < 256) bit = 256;
    if (chr < ncols * bit + 256) chr = ncols * bit + 256;
    if (row < 256) row = 256;
    int32_t w = (cols - 1) * chr + ncols * bit, h = (rows - 1) * row + 256;
    left = mid_x - w / 2;
    top = mid_y - h / 2 + 128;
    if (w <= VXCV_W * 256) {                                // all of it on the screen, if it can be
        if (left < 0) left = 0;
        if (left + w > VXCV_W * 256) left = VXCV_W * 256 - w;
    }
    if (top < 0) top = 0;
    if (top + h > VXCV_H * 256) top = VXCV_H * 256 - h;
    for (int i = 0; i < n; i++) {
        uint8_t col = tints[e->tint][z_level(t[i].z)];
        int y = (top + t[i].row * row) >> 8, y1 = (top + (t[i].row + 1) * row) >> 8;    // (large text: no gaps between its sweeps)
        int32_t x = left + t[i].col * chr;
        for (int b = 7; b >= lo; b--, x += bit) {
            if (!(t[i].bits & 1 << b)) continue;
            for (int yy = y; yy < y1; yy++) span(e, (x + 128) >> 8, yy, ((x + bit + 128) >> 8) - ((x + 128) >> 8), col);
        }
    }
}

// The dimmest first: where two brightnesses meet in a cell of eight pixels,
// the brighter one's colour is the one that stays. Then the text.
VXCV_HOT void vxcv_vectors(vxcv_t *e, const vx_vec_t *v, int n, const vx_text_t *t, int nt) {
    memset(e->want_pat, 0, sizeof e->want_pat);
    unsigned levels = 0;
    for (int i = 0; i < n; i++)
        if (!v[i].text) levels |= 1u << z_level(v[i].z);
    for (int l = 3; l >= 0; l--) {
        if (!(levels & 1u << l)) continue;
        uint8_t col = tints[e->tint][l];
        const vx_vec_t *p = v;
        for (int i = 0; i < n; i++, p++)
            if (!p->text && z_level(p->z) == l) line(e, PX(p->x0), PY(p->y0), PX(p->x1), PY(p->y1), col);
    }
    for (int i = 0; i < nt;) {
        int k = i + 1;
        while (k < nt && t[k].block == t[i].block) k++;
        text_block(e, t + i, k - i);
        i = k;
    }
    e->stats.pictures++;
    e->dirty = true;
}

void vxcv_canvas(vxcv_t *e, const uint8_t *px) {
    for (int y = 0; y < VXCV_H; y++)
        for (int c = 0; c < 32; c++, px += 8) {
            unsigned a = (unsigned)(((y >> 3) * 32 + c) * 8 + (y & 7));
            uint8_t pat = 0, col = 15;
            for (int x = 0; x < 8; x++)
                if (px[x] > 1) { pat |= (uint8_t)(0x80 >> x); col = px[x]; }
            e->want_pat[a] = pat;
            e->want_col[a] = (uint8_t)(e->colour ? col << 4 | 1 : WHITE);
        }
    e->stats.pictures++;
    e->dirty = true;
}

void vxcv_screen(const vxcv_t *e, uint8_t *px) {
    for (int y = 0; y < VXCV_H; y++)
        for (int x = 0; x < VXCV_W; x++) {
            unsigned a = (unsigned)(((y >> 3) * 32 + (x >> 3)) * 8 + (y & 7));
            uint8_t c = e->vram_col[a];
            c = (e->vram_pat[a] & (0x80 >> (x & 7))) ? c >> 4 : c & 15;
            px[y * VXCV_W + x] = c ? c : 1;
        }
}

// ---- what the console needs -------------------------------------------------------
// the colour byte place j should hold: in colour, its vector's where there
// are pixels (where there are none, whatever is there will do)
static inline uint8_t col_want(const vxcv_t *e, unsigned j) {
    if (!e->colour) return WHITE;
    return e->want_pat[j] ? e->want_col[j] : e->vram_col[j];
}
static inline bool need_pat(const vxcv_t *e, unsigned j) { return e->want_pat[j] != e->vram_pat[j]; }
static inline bool need_col(const vxcv_t *e, unsigned j) { return col_want(e, j) != e->vram_col[j]; }

// The next bytes to send, from the cursor on (once round): a place's colour
// bytes before its pattern bytes. *m: the run's natural length, with gaps of
// up to 3 bytes that are right already taken in (a new run costs more).
// Most of the screen is right already, and is passed over a word at a time:
// four pattern bytes the same as the console's need nothing, unless colour
// bytes are still to go (col_todo: in black and white only until the colour
// table's one fill is in; in colour, wherever there are pixels).
VXCV_HOT static bool next_run(vxcv_t *e, bool *colour, unsigned *s, int *m) {
    if (!e->dirty) return false;
    unsigned i = e->cursor;
    for (unsigned k = 0; k < VXCV_BYTES;) {
        if (!(i & 3) && k + 4 <= VXCV_BYTES) {
            uint32_t w, v;
            memcpy(&w, e->want_pat + i, 4);
            memcpy(&v, e->vram_pat + i, 4);
            if (w == v && (e->colour ? !w : !e->col_todo)) {
                k += 4;
                i = i + 4 == VXCV_BYTES ? 0 : i + 4;
                continue;
            }
        }
        bool nc = (e->colour || e->col_todo) && need_col(e, i);
        if (!nc && !need_pat(e, i)) {
            k++;
            i = i + 1 == VXCV_BYTES ? 0 : i + 1;
            continue;
        }
        e->cursor = (uint16_t)i;
        *colour = nc;
        *s = i;
        unsigned end = i + 1, gap = 0;
        for (unsigned j = i + 1; j < VXCV_BYTES && j - i < 255; j++) {
            if (nc ? need_col(e, j) : need_pat(e, j)) { end = j + 1; gap = 0; }
            else if (++gap > 3) break;
        }
        *m = (int)(end - i);
        return true;
    }
    e->dirty = false;                   // a whole round with nothing to send
    e->col_todo = false;
    return false;
}

static void put_run(vxcv_t *e, list_t *l, bool colour, unsigned s, int n) {
    l_run_head(l, (uint16_t)((colour ? COLOURS : PATTERNS) + s), n);
    uint8_t *d = l->b + l->n;
    if (colour) {
        for (int k = 0; k < n; k++) d[k] = e->vram_col[s + k] = col_want(e, s + k);
        e->stats.col_bytes += (unsigned)n;
    } else {
        memcpy(d, e->want_pat + s, n);
        memcpy(e->vram_pat + s, d, n);
        e->stats.pat_bytes += (unsigned)n;
    }
    l->n += n;
    e->stats.runs++;
}

// what's on screen written again as it is: a write the console lost or took
// wrong doesn't stay
// (the name table too: it is written once, and a lost write there would
// leave its rows blank for good. The rotation starts with it, so a first
// list the console didn't run whole, the one with the names, is made good
// by the first lists that have time left: test/start.sh)
#define REFRESH_SPAN (2 * VXCV_BYTES + 768)
#define REFRESH_NAMES (2 * VXCV_BYTES)
// where the rotation is, the names passed over while they are held back
static unsigned refresh_place(vxcv_t *e) {
    if (e->refresh_at >= REFRESH_NAMES && e->stats.tv_frames < e->names_hold) e->refresh_at = 0;
    return e->refresh_at;
}
static void refresh_run(vxcv_t *e, list_t *l, int n) {
    unsigned at = refresh_place(e);
    if (at >= 2 * VXCV_BYTES) {
        unsigned s = at - 2 * VXCV_BYTES;
        if (s + n > 768) n = (int)(768 - s);
        l_run_head(l, (uint16_t)(NAMES + s), n);
        for (int k = 0; k < n; k++) l->b[l->n + k] = (uint8_t)(s + k);
    } else {
        bool colour = at >= VXCV_BYTES;
        unsigned s = colour ? at - VXCV_BYTES : at;
        if (s + n > VXCV_BYTES) n = (int)(VXCV_BYTES - s);
        l_run_head(l, (uint16_t)((colour ? COLOURS : PATTERNS) + s), n);
        memcpy(l->b + l->n, (colour ? e->vram_col : e->vram_pat) + s, n);
    }
    l->n += n;
    e->refresh_at = (uint16_t)((at + n) % REFRESH_SPAN);
    e->stats.refreshed += (unsigned)n;
}

uint32_t vxcv_backlog(const vxcv_t *e) {
    uint32_t n = 0;
    if (!e->dirty) return 0;
    for (unsigned j = 0; j < VXCV_BYTES; j++) n += need_pat(e, j) + need_col(e, j);
    return n;
}

// ---- a frame's list, the console waiting for the vertical blank --------------------
static void names(vxcv_t *e, list_t *l) {
    // at the display's pace (31 T a byte), not the vertical blank's: the
    // table goes out once, so none of it may land where the TMS9918 drops a
    // quick write (a list that starts late in its frame)
    if (!e->names_quick) l_end_vblank(l);
    while (e->names_at < 768) {
        int n = room(l), left = 768 - e->names_at;
        if (n < 16 && l->vblank) { l_end_vblank(l); continue; }
        if (n < 1) return;
        if (n > left) n = left;
        l_run_head(l, (uint16_t)(NAMES + e->names_at), n);
        for (int k = 0; k < n; k++) l->b[l->n++] = (uint8_t)(e->names_at + k);
        e->names_at = (uint16_t)(e->names_at + n);
    }
    e->names_done = true;
}

VXCV_HOT static void plain_list(vxcv_t *e, list_t *l) {
    // After speech the console is out of step: its lists ran back to back,
    // so this one starts anywhere in the frame, and the vertical blank part's
    // quick writes (23 T apart) could fall in the active display, where the
    // TMS9918 takes one only every 28. So first a list with the sound alone:
    // it ends before the next vertical blank, or just past it, and the
    // console waits again; then one written at the display's pace
    // throughout; then as usual.
    if (e->resync) {
        l_end_vblank(l);
        if (e->tone_n) l_psg(l, e->tone_bytes, e->tone_n);
        if (e->resync-- == 2) return;
    } else if (e->tone_n) {
        l_psg(l, e->tone_bytes, e->tone_n);
    }
    bool with_names = !e->names_done;
    if (with_names) names(e, l);
    bool colour;
    unsigned s;
    int m;
    while (e->names_done && next_run(e, &colour, &s, &m)) {
        int n = room(l);
        if (n < m && l->vblank) {           // the rest of the vertical blank is too short for it
            if (n >= 8) put_run(e, l, colour, s, n);
            l_end_vblank(l);
            continue;
        }
        if (n < 1) break;
        put_run(e, l, colour, s, n < m ? n : m);
    }
    // with the time left (not in the list the names went out in: they are
    // the first thing written again, and that must be in another list)
    for (int k = 0; k < 2 && e->names_done && !with_names; k++) {
        if (refresh_place(e) >= REFRESH_NAMES) l_end_vblank(l);     // (the names at the display's pace, as names() sends them)
        int n = room(l);
        if (n < 32) break;
        refresh_run(e, l, n);
    }
    l_end_vblank(l);
}

// ---- a frame's list with speech in it ----------------------------------------------
// One VRAM run that takes all the time to `until` (less than a byte's worth
// left over): from the next byte the console needs, carrying on over bytes
// that are right already; with nothing needed, what's there written again.
VXCV_HOT static void fill(vxcv_t *e, list_t *l, uint32_t until) {
    int avail = (int)until - (int)l->t;
    if (avail < C_RUN_ACT + C_BYTE_ACT) return;
    int n = (avail - C_RUN_ACT) / C_BYTE_ACT;
    if (n > 255) n = 255;
    bool colour;
    unsigned s;
    int m;
    if (!e->names_done || !next_run(e, &colour, &s, &m)) {
        // (the run must take all its time: one that would pass its table's end starts n short of it)
        unsigned at = refresh_place(e);
        if (at >= REFRESH_NAMES) {
            if (at + (unsigned)n > REFRESH_SPAN) e->refresh_at = (uint16_t)(REFRESH_SPAN - (unsigned)n);
        } else if (at % VXCV_BYTES + (unsigned)n > VXCV_BYTES) {
            e->refresh_at = (uint16_t)(at / VXCV_BYTES * VXCV_BYTES + VXCV_BYTES - (unsigned)n);
        }
        refresh_run(e, l, n);
        return;
    }
    if (s + (unsigned)n > VXCV_BYTES) s = VXCV_BYTES - (unsigned)n;
    put_run(e, l, colour, s, n);
}

// A sample's two bytes, then VRAM to the next sample's time. The run's
// bytes are 31 T each; a sample byte sent again is 52 T (the level is the
// same), and none, one or two of those bring the next sample within 10 T of
// its time. A short run (a lone byte or two: a flat line's) goes in as one
// of two runs when there is room.
VXCV_HOT static void pcm_list(vxcv_t *e, list_t *l) {
    l->t = C_FRAME;
    l_end_vblank(l);
    unsigned tone_at = 0;
    for (int k = 0; k < VXCV_PCM_N; k++) {
        uint8_t b[10];
        int n = 0;
        b[n++] = e->pcm_bytes[k][0];
        b[n++] = e->pcm_bytes[k][1];
        // the tones' changes, a register or two with each sample (a register's
        // second byte must follow its first)
        while (tone_at < e->tone_n && n < 5) {
            unsigned g = 1;
            while (tone_at + g < e->tone_n && !(e->tone_bytes[tone_at + g] & 0x80)) g++;
            if (n + (int)g > 6) break;
            memcpy(b + n, e->tone_bytes + tone_at, g);
            n += (int)g;
            tone_at += g;
        }
        if (k + 1 == VXCV_PCM_N) {
            l_psg(l, b, n);
            break;
        }
        uint32_t until = PCM_FIRST + (((uint32_t)(k + 1) * PCM_STEP) >> 8);
        int avail = (int)until - (int)l->t - (C_PSG + C_PSG_BYTE * n);
        bool colour;
        unsigned s;
        int m = 0;
        bool two = e->names_done && next_run(e, &colour, &s, &m) && m <= 2 &&
                   avail >= 2 * C_RUN_ACT + C_BYTE_ACT * (m + 2);
        int base = avail - (two ? 2 : 1) * C_RUN_ACT, best = 0, best_left = 99;
        for (int r = 0; r < 3 && base - C_PSG_BYTE * r >= C_BYTE_ACT * (two ? m + 1 : 1); r++) {
            int left = (base - C_PSG_BYTE * r) % C_BYTE_ACT;
            if (left < best_left) { best_left = left; best = r; }
        }
        while (best--) b[n++] = e->pcm_bytes[k][1];
        l_psg(l, b, n);
        if (two) put_run(e, l, colour, s, m);
        fill(e, l, until);
    }
    e->stats.pcm_frames++;
    e->resync = 2;
}

size_t vxcv_tv_frame(vxcv_t *e, uint8_t *out) {
    list_t l = {out, 0, C_START, true};
    if (e->pcm_frame) pcm_list(e, &l);
    else plain_list(e, &l);
    l.t += C_END;
    l.b[l.n++] = 0;
    e->tone_n = 0;
    e->stats.tv_frames++;
    e->stats.t_last = l.t;
    if (l.t > e->stats.t_max) e->stats.t_max = l.t;
    // (nothing left: the scan went all the way round and found the console up to date)
    if (!e->dirty) e->stats.settled++;
    return l.n;
}

// ---- the sound ---------------------------------------------------------------------
// The DAC: tones 1 and 2 at a period of 1, their attenuators as measured on
// the user's console (coleco/ladder.json, channels 1 and 2), summed. Level i
// of DAC_LEVELS is i / (DAC_LEVELS - 1) of DAC_TOP. (Star Wars' speech goes
// out the same way: ../starwars/swcv.c.)
#define DAC_LEVELS 128
#define DAC_TOP    1.40f
#define DAC_MID    64               // silence while it plays
// the Vectrex's samples: its DAC's whole swing is 104 levels either side,
// so only one louder than most is clipped
#define SAMPLE_SCALE 104
// the tone and noise under the samples, this many 2 dB steps quieter
#define SAMPLE_DUCK  2
#define ATT_SENT(e, att) (att)
static const float ladder[2][16] = {
    {1.0f, 0.7816f, 0.6125f, 0.4850f, 0.3925f, 0.3109f, 0.2416f, 0.1872f, 0.1373f, 0.1058f, 0.0828f, 0.0653f, 0.0533f, 0.0412f, 0.0306f, 0.0f},
    {1.0f, 0.7979f, 0.6229f, 0.4921f, 0.3942f, 0.3004f, 0.2348f, 0.1874f, 0.1419f, 0.1106f, 0.0846f, 0.0677f, 0.0534f, 0.0420f, 0.0328f, 0.0f}};
static uint8_t dac_pair[DAC_LEVELS];        // attenuations: channel 1's << 4 | channel 2's
static bool dac_ready;

float vxcv_dac_level(int a1, int a2) { return ladder[0][a1 & 15] + ladder[1][a2 & 15]; }

static void dac_init(void) {
    for (int i = 0; i < DAC_LEVELS; i++) {
        float want = DAC_TOP * (float)i / (DAC_LEVELS - 1), best = 1e9f;
        for (int a = 0; a < 16; a++)
            for (int b = 0; b < 16; b++) {
                float d = ladder[0][a] + ladder[1][b] - want;
                if (d < 0) d = -d;
                if (d < best) { best = d; dac_pair[i] = (uint8_t)(a << 4 | b); }
            }
    }
    dac_ready = true;
}

// an AY volume (0-15, 3 dB a step) as an SN76489 attenuation (2 dB a step)
static const uint8_t vol_att[16] = {15, 15, 15, 15, 14, 14, 13, 12, 11, 9, 8, 6, 5, 3, 2, 0};

typedef struct {
    uint16_t period;                // the SN76489's, 1-1023
    uint8_t vol, key;               // key: 1 + the AY's channel
} voice_t;

// The AY's three channels as tones and one noise. The Vectrex clocks it at
// 1.5 MHz: a tone's period n there is n x 1.1932 at the PSG's 3.579545 MHz
// (both divide by 16 a half wave). A channel with tone and noise both on
// gives both.
static int ay_voices(const vx_ay_t *a, voice_t *v, int *noise_vol, int *noise_rate) {
    int n = 0, nv = 0;
    uint8_t mix = a->reg[7];
    for (int c = 0; c < 3; c++) {
        int vol = (a->reg[8 + c] & 0x10) ? a->env : a->reg[8 + c] & 15;
        if (!vol) continue;
        if (!(mix & (8 << c)) && vol > nv) nv = vol;
        if (mix & (1 << c)) continue;
        uint32_t period = (uint32_t)(a->reg[2 * c] | (a->reg[2 * c + 1] & 15) << 8);
        uint32_t q = (period * 78196u + 32768u) >> 16;
        if (q < 5) continue;                        // above hearing (a level only)
        while (q > 1023) q >>= 1;                   // below the PSG's lowest: up an octave
        int same = -1;
        for (int i = 0; i < n; i++)
            if (v[i].period == q) same = i;
        if (same >= 0) {                            // in unison: one voice, a little louder
            if (vol > v[same].vol) v[same].vol = (uint8_t)vol;
            if (v[same].vol < 15) v[same].vol++;
            continue;
        }
        v[n].period = (uint16_t)q;
        v[n].vol = (uint8_t)vol;
        v[n].key = (uint8_t)(1 + c);
        n++;
    }
    // the noise's shift clock is 93,750 Hz / its period; the PSG's nearest of 6,991, 3,496 and 1,748
    int np = a->reg[6] & 31;
    *noise_vol = nv;
    *noise_rate = np <= 19 ? 0 : 1;
    return n;
}

static void tone_set(vxcv_t *e, int c, int period, int att) {
    uint8_t *b = e->tone_bytes;
    if (period > 0 && period != e->psg_period[c]) {
        b[e->tone_n++] = (uint8_t)(0x80 | c << 5 | (period & 15));
        b[e->tone_n++] = (uint8_t)(period >> 4 & 0x3F);
        e->psg_period[c] = (int16_t)period;
    }
    if (att != e->psg_att[c]) {
        b[e->tone_n++] = (uint8_t)(0x90 | c << 5 | ATT_SENT(e, att));
        e->psg_att[c] = (int8_t)att;
    }
}

void vxcv_sound(vxcv_t *e, const vx_ay_t *ay, const int16_t *speech, int nspeech, bool talking) {
    if (!dac_ready) dac_init();
    e->tone_n = 0;

    // speech: the DAC comes up when the chip starts talking and goes a few
    // frames after its last sample
    bool heard = talking;
    for (int i = 0; i < nspeech && !heard; i++) heard = speech[i] > 256 || speech[i] < -256;
    if (heard) e->pcm_hold = 6;
    else if (e->pcm_hold) e->pcm_hold--;
    bool was = e->pcm;
    e->pcm_frame = e->pcm_hold > 0 || e->pcm_gain > 0;
    if (e->pcm_frame && !was) {             // tones 1 and 2 become the DAC
        static const uint8_t park[] = {0xA1, 0x00, 0xC1, 0x00};
        memcpy(e->tone_bytes, park, 4);
        e->tone_n = 4;
        e->psg_period[1] = e->psg_period[2] = 1;
        e->voice_key[1] = e->voice_key[2] = 0;
    }
    e->pcm = e->pcm_frame;
    if (e->pcm_frame) {
        for (int k = 0; k < VXCV_PCM_N; k++) {
            // this frame's samples, 8,400 a second in, VXCV_PCM_N a frame out
            int32_t s = e->speech_last;
            if (nspeech > 0) {
                uint32_t pos = (uint32_t)k * (uint32_t)nspeech * 256 / VXCV_PCM_N;
                unsigned i = pos >> 8, f = pos & 255;
                int32_t a = speech[i], b = speech[i + 1 < (unsigned)nspeech ? i + 1 : i];
                s = a + (((b - a) * (int32_t)f) >> 8);
            }
            if (e->pcm_hold > 2) { if (e->pcm_gain < 32 && !(k & 3)) e->pcm_gain++; }
            else if (e->pcm_gain && !(k & 3)) e->pcm_gain--;
            // the level: silence at DAC_MID, half the chip's full scale 60 levels either side, by the gain
            int32_t lvl = DAC_MID + ((s * SAMPLE_SCALE) >> 15);
            if (lvl < 1) lvl = 1;
            if (lvl > DAC_LEVELS - 1) lvl = DAC_LEVELS - 1;
            lvl = lvl * e->pcm_gain / 32;
            uint8_t pair = dac_pair[lvl];
            e->pcm_bytes[k][0] = (uint8_t)(0xB0 | pair >> 4);
            e->pcm_bytes[k][1] = (uint8_t)(0xD0 | (pair & 15));
        }
        e->psg_att[1] = e->psg_att[2] = -1;
        if (nspeech > 0) e->speech_last = speech[nspeech - 1];
    } else {
        e->speech_last = 0;
    }

    // the tones: the loudest voices, each staying on the channel it has
    voice_t v[3];
    int noise_vol, noise_rate;
    int n = ay_voices(ay, v, &noise_vol, &noise_rate), chans = e->pcm ? 1 : 3;
    int pick[3] = {-1, -1, -1};
    bool taken[3] = {false};
    for (int c = 0; c < chans; c++) {       // the loudest `chans` of them
        int best = -1, best_score = -1;
        for (int i = 0; i < n; i++) {
            if (taken[i]) continue;
            int score = v[i].vol * 2;
            for (int k = 0; k < chans; k++)
                if (e->voice_key[k] == v[i].key) score++;   // one playing already wins a tie
            if (score > best_score) { best_score = score; best = i; }
        }
        if (best < 0) break;
        taken[best] = true;
        pick[c] = best;
    }
    int chan_of[3] = {-1, -1, -1};          // channel -> voice
    for (int c = 0; c < chans; c++)         // those playing keep their channel
        for (int k = 0; k < chans; k++)
            if (pick[k] >= 0 && e->voice_key[c] == v[pick[k]].key && chan_of[c] < 0) { chan_of[c] = pick[k]; pick[k] = -1; }
    for (int k = 0; k < chans; k++) {
        if (pick[k] < 0) continue;
        for (int c = 0; c < chans; c++)
            if (chan_of[c] < 0) { chan_of[c] = pick[k]; break; }
    }
    for (int c = 0; c < chans; c++) {
        if (chan_of[c] >= 0) {
            int att = vol_att[v[chan_of[c]].vol] + (e->pcm ? SAMPLE_DUCK : 0);
            tone_set(e, c, v[chan_of[c]].period, att > 15 ? 15 : att);
            e->voice_key[c] = v[chan_of[c]].key;
        } else {
            tone_set(e, c, 0, 15);
            e->voice_key[c] = 0;
        }
    }
    int natt = vol_att[noise_vol] + (e->pcm ? SAMPLE_DUCK : 0);
    if (natt > 15) natt = 15;
    if (noise_vol && e->psg_noise != (4 | noise_rate)) {   // (a write restarts the shift register: only on a change)
        e->tone_bytes[e->tone_n++] = (uint8_t)(0xE0 | 4 | noise_rate);
        e->psg_noise = (int16_t)(4 | noise_rate);
    }
    if (natt != e->psg_att[3]) {
        e->tone_bytes[e->tone_n++] = (uint8_t)(0xF0 | ATT_SENT(e, natt));
        e->psg_att[3] = (int8_t)natt;
    }
}

void vxcv_set_tint(vxcv_t *e, int tint) {
    e->tint = (uint8_t)(tint % VXCV_TINTS);
    e->dirty = true;
}

void vxcv_init(vxcv_t *e) {
    memset(e, 0, sizeof *e);
    for (int c = 0; c < 3; c++) e->psg_period[c] = -1;
    for (int c = 0; c < 4; c++) e->psg_att[c] = 15;     // the kernel's start: all off
    e->psg_noise = -1;
    e->dirty = true;
    e->col_todo = true;
    e->colour = true;
    e->refresh_at = REFRESH_NAMES;
}
