// vdp.c -- see vdp.h.
#include "vdp.h"

#include <string.h>

#include "font8.h"

#define TILES   768
#define GAP     3               // unchanged bytes a copy carries rather than start a new command (3 bytes)
#define FILL_MIN 5              // identical bytes worth a fill command

const uint8_t vdp_rgb[16][3] = {
    {0, 0, 0}, {0, 0, 0}, {33, 200, 66}, {94, 220, 120}, {84, 85, 237}, {125, 118, 252},
    {212, 82, 77}, {66, 235, 245}, {252, 85, 84}, {255, 121, 120}, {212, 193, 84},
    {230, 206, 128}, {33, 176, 59}, {201, 91, 186}, {204, 204, 204}, {255, 255, 255}};

void vdp_init(vdp_t *v) {
    memset(v->vram, 0, sizeof v->vram);
    for (int i = 0; i < TILES; i++) v->vram[VRAM_NAME + i] = (uint8_t)i;
    v->vram[VRAM_SAT] = 0xD0;
    v->bad_segments = 0;
}

// ---- drawing -------------------------------------------------------------------------
void canvas_all_dirty(canvas_t *c) { memset(c->dirty, 1, sizeof c->dirty); }

void canvas_init(canvas_t *c) {
    memset(c->px, C_BLACK, sizeof c->px);
    canvas_all_dirty(c);
}

static inline void put(canvas_t *c, int x, int y, uint8_t colour) {
    if ((unsigned)x >= VDP_W || (unsigned)y >= VDP_H || c->px[y][x] == colour) return;
    c->px[y][x] = colour;
    c->dirty[y >> 3][x >> 3] = 1;
}

void canvas_fill(canvas_t *c, int x, int y, int w, int h, uint8_t colour) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(c, i, j, colour);
}

int canvas_text(canvas_t *c, int x, int y, const char *s, uint8_t colour, int scale) {
    for (; s && *s; s++) {
        int ch = (unsigned char)*s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch < 32 || ch > 95) ch = '?';
        const uint8_t *g = font8[ch - 32];
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] >> (7 - b) & 1)
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) put(c, x + b * scale + dx, y + r * scale + dy, colour);
        x += 8 * scale;
    }
    return x;
}

// ---- encoding ------------------------------------------------------------------------
// One segment's pattern and colour bytes: the two colours it uses, the way
// round that changes the fewest bytes already in VRAM.
static void encode(vdp_t *v, const uint8_t *px, uint16_t a, uint8_t *pat, uint8_t *col) {
    uint8_t k[8], ca = 0, cb = 0xFF, opat = v->vram[VRAM_PAT + a], ocol = v->vram[VRAM_COL + a];
    int na = 0, nb = 0;
    for (int i = 0; i < 8; i++) {
        k[i] = px[i] == C_BLACK ? 0 : px[i] & 15;
        if (i == 0) ca = k[0];
        if (k[i] == ca) na++;
        else if (cb == 0xFF || k[i] == cb) { cb = k[i]; nb++; }
        else { nb++; v->bad_segments++; }                 // a third colour: drawn as the second
    }
    uint8_t ofg = ocol >> 4, obg = ocol & 15;
    if (cb == 0xFF) {                                     // one colour
        if ((opat == 0xFF && ofg == ca) || (opat == 0x00 && obg == ca)) { *pat = opat; *col = ocol; }
        else if (ofg == ca) { *pat = 0xFF; *col = ocol; }
        else if (obg == ca) { *pat = 0x00; *col = ocol; }
        else { *pat = 0x00; *col = (uint8_t)(ofg << 4 | ca); }
        return;
    }
    uint8_t bits = 0;
    for (int i = 0; i < 8; i++) if (k[i] == ca) bits |= (uint8_t)(0x80 >> i);
    uint8_t p1 = bits, c1 = (uint8_t)(ca << 4 | cb), p2 = (uint8_t)~bits, c2 = (uint8_t)(cb << 4 | ca);
    int cost1 = (p1 != opat) + (c1 != ocol), cost2 = (p2 != opat) + (c2 != ocol);
    bool one = cost1 < cost2 || (cost1 == cost2 && na <= nb);   // a tie: the rarer colour in front
    *pat = one ? p1 : p2;
    *col = one ? c1 : c2;
}

typedef struct {
    vdp_t *v;
    uint8_t *list;
    size_t used, max;
} out_t;

static bool emit(out_t *o, uint16_t addr, const uint8_t *src, int n, bool fill) {
    size_t need = fill ? 4 : 3 + (size_t)n;
    if (o->used + need + 1 > o->max) return false;        // room for the end mark
    uint8_t *p = o->list + o->used;
    p[0] = (uint8_t)(fill ? 0x80 + n : n);
    p[1] = (uint8_t)addr;
    p[2] = (uint8_t)(addr >> 8 | 0x40);
    if (fill) p[3] = src[0];
    else memcpy(p + 3, src, (size_t)n);
    for (int i = 0; i < n; i++) o->v->vram[addr + i] = fill ? src[0] : src[i];
    o->used += need;
    return true;
}

// want[s..e) to VRAM at base + s: fills for runs of identical bytes, copies between
static bool emit_range(out_t *o, uint16_t base, const uint8_t *want, int s, int e) {
    int i = s;
    while (i < e) {
        int r = 1;
        while (i + r < e && want[i + r] == want[i] && r < LIST_FILL_MAX) r++;
        if (r >= FILL_MIN) {
            if (!emit(o, (uint16_t)(base + i), want + i, r, true)) return false;
            i += r;
            continue;
        }
        int j = i, room = (int)o->max - (int)o->used - 4;   // a copy's header and the end mark
        if (room > LIST_COPY_MAX) room = LIST_COPY_MAX;
        if (room < 1) return false;
        while (j < e && j - i < room) {
            int rr = 1;
            while (j + rr < e && want[j + rr] == want[j] && rr < FILL_MIN) rr++;
            if (rr >= FILL_MIN && j > i) break;
            j++;
        }
        if (!emit(o, (uint16_t)(base + i), want + i, j - i, false)) return false;
        i = j;
    }
    return true;
}

// the bytes of [s, e) (table offsets) that differ from VRAM, as few commands as is sensible
static bool diff_range(out_t *o, uint16_t base, const uint8_t *want, int s, int e) {
    const uint8_t *have = o->v->vram + base;
    int p = s;
    while (p < e) {
        if (want[p] == have[p]) { p++; continue; }
        int last = p;
        for (int q = p + 1; q < e && q - last <= GAP; q++)
            if (want[q] != have[q]) last = q;
        if (!emit_range(o, base, want, p, last + 1)) return false;
        p = last + 1;
    }
    return true;
}

size_t vdp_list(vdp_t *v, canvas_t *c, uint8_t *list, size_t max) {
    static uint8_t want_pat[TILES * 8], want_col[TILES * 8];
    out_t o = {v, list, 0, max};
    int t = 0;
    while (t < TILES) {
        if (!c->dirty[t >> 5][t & 31]) { t++; continue; }
        int first = t;                                    // a run of dirty tiles, in VRAM order
        for (; t < TILES && c->dirty[t >> 5][t & 31]; t++) {
            int ty = t >> 5, cx = t & 31;
            for (int l = 0; l < 8; l++) {
                uint16_t a = (uint16_t)(t * 8 + l);
                encode(v, &c->px[ty * 8 + l][cx * 8], a, &want_pat[a], &want_col[a]);
            }
        }
        if (!diff_range(&o, VRAM_PAT, want_pat, first * 8, t * 8) ||
            !diff_range(&o, VRAM_COL, want_col, first * 8, t * 8))
            break;                                        // full: these tiles and the rest stay dirty
        for (int i = first; i < t; i++) c->dirty[i >> 5][i & 31] = 0;
    }
    if (!o.used) return 0;
    list[o.used++] = LIST_END;
    return o.used;
}

size_t vdp_list_reg(uint8_t *list, int reg, uint8_t value) {
    list[0] = 0;
    list[1] = value;
    list[2] = (uint8_t)(0x80 | reg);
    list[3] = LIST_END;
    return 4;
}

bool vdp_run(uint8_t vram[VRAM_SIZE], uint8_t regs[8], const uint8_t *list, size_t len) {
    size_t i = 0;
    while (i < len) {
        uint8_t h = list[i++];
        if (h == LIST_END) return true;
        if (i + 2 > len) return false;
        uint8_t lo = list[i], hi = list[i + 1];
        i += 2;
        if (h == 0) {
            if ((hi & 0xF8) != 0x80) return false;
            if (regs) regs[hi & 7] = lo;
            continue;
        }
        if ((hi & 0xC0) != 0x40) return false;
        uint16_t a = (uint16_t)(lo | (hi & 0x3F) << 8);
        int n = h & 0x80 ? h - 0x80 : h;
        if (h & 0x80) {
            if (i + 1 > len || n < 1) return false;
            for (int k = 0; k < n; k++) vram[(a + k) & 0x3FFF] = list[i];
            i++;
        } else {
            if (i + (size_t)n > len) return false;
            for (int k = 0; k < n; k++) vram[(a + k) & 0x3FFF] = list[i + k];
            i += (size_t)n;
        }
    }
    return false;                                         // no end mark
}

void vdp_picture(const uint8_t vram[VRAM_SIZE], uint8_t backdrop, uint8_t out[VDP_H][VDP_W]) {
    for (int y = 0; y < VDP_H; y++)
        for (int cx = 0; cx < 32; cx++) {
            int third = y >> 6, name = vram[VRAM_NAME + (y >> 3) * 32 + cx];
            uint16_t a = (uint16_t)((third * 256 + name) * 8 + (y & 7));
            uint8_t p = vram[VRAM_PAT + a], c = vram[VRAM_COL + a];
            for (int b = 0; b < 8; b++) {
                uint8_t k = p >> (7 - b) & 1 ? c >> 4 : c & 15;
                out[y][cx * 8 + b] = k ? k : backdrop;
            }
        }
}
