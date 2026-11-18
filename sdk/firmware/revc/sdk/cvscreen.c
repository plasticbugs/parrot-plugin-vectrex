// cvscreen.c -- see cvscreen.h (midihello's screen.c, with the UI as a callback).
#include "cvscreen.h"

#include <string.h>

void cvs_init(cvs_t *s) {
    memset(s, 0, sizeof *s);
    vdp_init(&s->vdp);
    canvas_init(&s->canvas);
}

void cvs_report(cvs_t *s, bool hello, uint8_t seq5, uint32_t now_us) {
    bool done = seq5 == (s->seq & 31);
    if (s->out && (done || (hello && now_us - s->sent_us > CVS_TIMEOUT_US))) s->out = false;
    if (!hello) return;
    s->ready = true;
    if (!(s->out && s->out_full)) s->full = true;   // else that is the answer to an earlier HELLO
}

size_t cvs_update(cvs_t *s, cvs_draw_fn draw, void *ctx, uint8_t *list, size_t max, uint32_t now_us) {
    if (!s->ready || s->out) return 0;
    bool full = s->full;
    if (full) {
        s->full = false;
        vdp_init(&s->vdp);
        canvas_init(&s->canvas);
        s->starts++;
    }
    draw(&s->canvas, full, ctx);
    size_t n = vdp_list(&s->vdp, &s->canvas, list, max);
    if (!n) return 0;
    s->seq++;
    s->out = true;
    s->out_full = full;
    s->sent_us = now_us;
    s->lists++;
    return n;
}

void cvs_line(canvas_t *c, int x, int y, int w, const char *s, uint8_t fg, uint8_t bg, int scale) {
    char buf[64];
    int n = w / (8 * scale);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (n < 0) n = 0;
    strncpy(buf, s ? s : "", (size_t)n);
    buf[n] = 0;
    canvas_fill(c, x, y, w, 8 * scale, bg);
    canvas_text(c, x, y, buf, fg, scale);
}
