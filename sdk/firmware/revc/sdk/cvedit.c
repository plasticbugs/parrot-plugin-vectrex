// cvedit.c -- see cvedit.h.
#include "cvedit.h"

#include <stdio.h>
#include <string.h>

#include "cvfont.h"

enum { CVE_EDIT, CVE_LIST, CVE_NAME, CVE_ASK, CVE_FOUND };
enum { CMD_SAVE = 1, CMD_OPEN, CMD_NEW };
enum { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT, NAV_HOME, NAV_END, NAV_PGUP, NAV_PGDN, NAV_TOP, NAV_BOTTOM };

#define NONE     0xFFFFFFFFu
#define W        (e->cfg.cols)
#define ROWS     (e->cfg.rows - 2)
#define T        (e->cfg.text)
#define TAB      4
#define HDR      1024               // a spare file's header, before the text
#define REPEAT_WAIT_US 400000
#define REPEAT_US      40000
#define SAVE_WAIT_US   300000

static const char MAGIC[8] = {'C', 'V', 'E', 'D', 'S', 'A', 'V', '1'};

// ---- the text as screen lines ----------------------------------------------------------
// a line end at i: its length (1: LF, 2: CR LF), or 0
static int eol_at(const cvedit_t *e, uint32_t i) {
    if (i >= e->len) return 0;
    if (T[i] == '\n') return 1;
    return T[i] == '\r' && i + 1 < e->len && T[i + 1] == '\n' ? 2 : 0;
}

static bool blank(uint8_t c) { return c == ' ' || c == '\t'; }

// the column after c at col. Blanks past the last column stay on it: they
// hang off the line's end, unseen, and never start a line.
static int after(const cvedit_t *e, int col, uint8_t c) {
    col = c == '\t' ? (col / TAB + 1) * TAB : col + 1;
    return col > W ? W : col;
}

// The screen line that starts at s. *end: where its characters end. Returns
// where the next line starts: *end itself if this one was wrapped, past the
// line end if the paragraph ends here, NONE if the text does.
//
// A line takes characters while they fit. When one doesn't, the line is
// broken after its last blank; with no blank in it, right there.
static uint32_t line_next(const cvedit_t *e, uint32_t s, uint32_t *end) {
    int col = 0;
    uint32_t i = s, brk = NONE;
    for (;; i++) {
        if (i >= e->len) { *end = i; return NONE; }
        const int n = eol_at(e, i);
        if (n) { *end = i; return i + (uint32_t)n; }
        const uint8_t c = T[i];
        if (blank(c)) {
            col = after(e, col, c);
            brk = i + 1;
        } else if (col >= W) {
            if (brk != NONE) i = brk;
            *end = i;
            return i;
        } else {
            col++;
        }
    }
}

// Finding the line a place is on means walking the lines from one whose
// start is known. A paragraph's start always is, but a paragraph can be the
// whole text (a file with no line ends), so the editor also keeps a hint: a
// line start some way before where it last worked, which walks start from
// when they'd pass it on their way back to the paragraph's start. Offset 0
// is always right. An edit moves it back if it has to (before_edit).
#define HINT_BACK 4096

static uint32_t walk_start(const cvedit_t *e, uint32_t p) {
    const uint32_t stop = e->hint <= p ? e->hint : 0;
    while (p > stop && T[p - 1] != '\n') p--;
    return p;
}

static void hint_at(cvedit_t *e, uint32_t l, uint32_t p) {    // l: a line start met on the way to p
    if (p - l >= HINT_BACK && (l > e->hint || e->hint > p)) e->hint = l;
}

// the screen line p is on: its start; *lim the last place the cursor can be
// on it; *next the next line's start (NONE: the last line)
static uint32_t line_of(cvedit_t *e, uint32_t p, uint32_t *lim, uint32_t *next) {
    uint32_t s = walk_start(e, p);
    for (;;) {
        uint32_t end;
        const uint32_t n = line_next(e, s, &end);
        const bool wrapped = n == end;
        if (n == NONE || (wrapped ? p < n : p <= end) || n <= s) {
            if (lim) *lim = wrapped && end > s ? end - 1 : end;
            if (next) *next = n;
            return s;
        }
        hint_at(e, s, p);
        s = n;
    }
}

// the start of the line k lines (1 to LINES_BACK) before the one that starts at s, or of the first line
#define LINES_BACK 24
static uint32_t lines_back(cvedit_t *e, uint32_t s, int k) {
    while (k > 0 && s > 0) {
        const uint32_t from = walk_start(e, s - 1);
        uint32_t ring[LINES_BACK], end;
        int n = 0;
        for (uint32_t l = from;;) {             // the lines from there up to the one before s
            ring[n++ % LINES_BACK] = l;
            const uint32_t next = line_next(e, l, &end);
            if (next == NONE || next >= s) break;
            hint_at(e, l, s);
            l = next;
        }
        if (n >= k) return ring[(n - k) % LINES_BACK];
        k -= n;
        s = from;
    }
    return s;
}

#ifdef CVEDIT_TEST
// For tests: the hint and the first line shown are line starts, found the long way.
bool cvedit_test_starts_ok(cvedit_t *e) {
    const uint32_t hint = e->hint;
    e->hint = 0;
    const bool ok = hint <= e->len && e->top <= e->len && line_of(e, hint, NULL, NULL) == hint && line_of(e, e->top, NULL, NULL) == e->top;
    e->hint = hint;
    return ok;
}
#endif

// The text is about to change from a on. Line starts up to the line before
// a's stay where they are (a line's break looks no further than the word
// that starts the next line), so the hint must be no later than that; and
// the line count kept for the top line must not be past a.
static uint32_t line_count(cvedit_t *e, uint32_t p);
static void before_edit(cvedit_t *e, uint32_t a) {
    const uint32_t ls = line_of(e, a, NULL, NULL);
    const uint32_t pl = ls ? lines_back(e, ls, 1) : 0;
    if (e->hint > pl) e->hint = pl;
    line_count(e, a);
}

static int col_of(const cvedit_t *e, uint32_t s, uint32_t p) {
    int col = 0;
    for (uint32_t i = s; i < p && i < e->len; i++) col = after(e, col, T[i]);
    return col;
}

// the place on the line from s nearest column goal without passing it
static uint32_t pos_at(const cvedit_t *e, uint32_t s, uint32_t lim, int goal) {
    int col = 0;
    uint32_t i = s;
    for (; i < lim; i++) {
        const int nc = after(e, col, T[i]);
        if (nc > goal) break;
        col = nc;
    }
    return i;
}

// top on a line start, and the cursor's line on the screen: scrolled by a
// line at a time, or with rows >= 0 put that many lines down
static void show(cvedit_t *e, int rows) {
    if (e->cursor > e->len) e->cursor = e->len;
    if (e->top > e->len) e->top = e->len;
    e->top = line_of(e, e->top, NULL, NULL);
    const uint32_t cl = line_of(e, e->cursor, NULL, NULL);
    bool seen = false;
    if (rows < 0 && cl >= e->top) {
        uint32_t l = e->top, end;
        for (int r = 0; r < ROWS && l != NONE; r++, l = line_next(e, l, &end))
            if (l == cl) { seen = true; break; }
        rows = ROWS - 1;
    } else if (rows < 0) {
        rows = 0;
    }
    if (!seen) e->top = rows ? lines_back(e, cl, rows) : cl;
    e->redraw = true;
}

static uint32_t left_of(const cvedit_t *e, uint32_t p) {
    if (p == 0) return 0;
    return p >= 2 && T[p - 1] == '\n' && T[p - 2] == '\r' ? p - 2 : p - 1;
}

static uint32_t right_of(const cvedit_t *e, uint32_t p) {
    if (p >= e->len) return e->len;
    const int n = eol_at(e, p);
    return p + (uint32_t)(n ? n : 1);
}

static void nav(cvedit_t *e, int how) {
    uint32_t lim, next;
    const uint32_t s = line_of(e, e->cursor, &lim, &next);
    int goal = e->goal >= 0 ? e->goal : col_of(e, s, e->cursor);
    int keep = -1;
    switch (how) {
    case NAV_LEFT: e->cursor = left_of(e, e->cursor); break;
    case NAV_RIGHT: e->cursor = right_of(e, e->cursor); break;
    case NAV_HOME: e->cursor = s; break;
    case NAV_END: e->cursor = lim; break;
    case NAV_TOP: e->cursor = 0; break;
    case NAV_BOTTOM: e->cursor = e->len; break;
    case NAV_UP:
    case NAV_PGUP: {
        const int by = how == NAV_UP ? 1 : ROWS - 1;
        const uint32_t l = lines_back(e, s, by);
        if (how == NAV_PGUP) e->top = lines_back(e, line_of(e, e->top, NULL, NULL), by);   // the screen goes with it
        if (l != s) {
            line_of(e, l, &lim, NULL);
            e->cursor = pos_at(e, l, lim, goal);
        }
        keep = goal;
        break;
    }
    case NAV_DOWN:
    case NAV_PGDN: {
        uint32_t l = s, end;
        for (int r = how == NAV_DOWN ? 1 : ROWS - 1; r > 0; r--) {
            const uint32_t n = line_next(e, l, &end);
            if (n == NONE) break;
            l = n;
            if (how == NAV_PGDN) {
                const uint32_t t = line_next(e, e->top, &end);
                if (t != NONE) e->top = t;
            }
        }
        if (l != s) {
            // (l is a line start, but so is the end of the line before it: ask from inside it)
            const uint32_t n = line_next(e, l, &end);
            lim = n == end && end > l ? end - 1 : end;
            e->cursor = pos_at(e, l, lim, goal);
        }
        keep = goal;
        break;
    }
    }
    e->goal = keep;
    show(e, -1);
}

static void say(cvedit_t *e, bool bad, const char *msg) {
    snprintf(e->msg, sizeof e->msg, "%s", msg ? msg : "");
    e->msg_bad = bad;
    e->redraw = true;
}

bool cvedit_insert(cvedit_t *e, const uint8_t *s, uint32_t n) {
    bool all = true;
    if (n > e->cfg.cap - e->len) {
        n = e->cfg.cap - e->len;
        all = false;
        say(e, true, "No room: the text is full");
    }
    if (n) {
        before_edit(e, e->cursor);
        memmove(T + e->cursor + n, T + e->cursor, e->len - e->cursor);
        memcpy(T + e->cursor, s, n);
        e->len += n;
        e->cursor += n;
        e->modified = true;
    }
    e->goal = -1;
    show(e, -1);
    return all;
}

static void cut(cvedit_t *e, uint32_t a, uint32_t b) {
    if (b > a) {
        before_edit(e, a);
        memmove(T + a, T + b, e->len - b);
        e->len -= b - a;
        e->modified = true;
    }
    e->cursor = a;
    e->goal = -1;
    show(e, -1);
}

// ---- names and paths -------------------------------------------------------------------
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static bool same_path(const char *a, const char *b) {
    while (*a == '/') a++;
    while (*b == '/') b++;
    for (; *a && *b; a++, b++)
        if (lower((unsigned char)*a) != lower((unsigned char)*b)) return false;
    while (*a == '/') a++;
    while (*b == '/') b++;
    return !*a && !*b;
}

static bool has_ext(const char *name, const char *ext) {
    const size_t n = strlen(name), x = strlen(ext);
    if (n <= x) return false;
    for (size_t i = 0; i < x; i++)
        if (lower((unsigned char)name[n - x + i]) != lower((unsigned char)ext[i])) return false;
    return true;
}

static void set_name(cvedit_t *e, const char *dir, const uint16_t *nm, int n) {
    snprintf(e->dir, sizeof e->dir, "%s", dir);
    if (nm != e->name16) memcpy(e->name16, nm, (size_t)n * 2);
    e->name_n = n;
    int i = 0;
    for (; i < n && i < FS_NAME - 1; i++) e->name[i] = nm[i] >= 32 && nm[i] < 127 ? (char)nm[i] : '?';
    e->name[i] = 0;
}

static int widen(uint16_t *out, const char *s) {
    int n = 0;
    for (; s[n] && n < FS_LNAME - 1; n++) out[n] = (uint8_t)s[n];
    return n;
}

static fs_t *card(cvedit_t *e) { return e->cfg.card ? e->cfg.card(e->cfg.ctx, false) : NULL; }

// cfg.home, or the top folder if the host named none
static const char *home_of(const cvedit_t *e) { return e->cfg.home && e->cfg.home[0] ? e->cfg.home : "/"; }

// cfg.home made, if dir is it and the card doesn't have it yet (a first save)
static int make_home(cvedit_t *e, fs_t *fs, const char *dir) {
    if (!same_path(dir, home_of(e))) return FS_ENOFILE;
    char up[FS_LNAME];
    snprintf(up, sizeof up, "%s", dir);
    size_t n = strlen(up);
    while (n > 1 && up[n - 1] == '/') up[--n] = 0;
    const char *slash = strrchr(up, '/');
    const char *last = slash ? slash + 1 : up;
    const int wn = widen(e->tmp16, last);
    if (!wn) return FS_ENOFILE;
    if (slash) up[slash == up ? 1 : slash - up] = 0;
    fs_file_t f;
    bool made;
    return fs_mkdir(fs, slash ? up : "/", e->tmp16, wn, &f, &made);
}

static const char *why(int r) {
    switch (r) {
    case FS_EIO: return "the card didn't answer";
    case FS_EFULL: return "no room on the card";
    case FS_ENOFILE: case FS_ENOTDIR: return "not found on the card";
    case FS_ENAME: return "the name can't be used";
    case FS_ENOWRITE: return "the card is read-only";
    default: return "the card's files are odd";
    }
}

static int fail(cvedit_t *e, const char *what, int r) {
    char s[sizeof e->msg];
    snprintf(s, sizeof s, "%s: %s", what, why(r));
    say(e, true, s);
    if (r == FS_EIO && e->cfg.card) e->cfg.card(e->cfg.ctx, true);
    return r;
}

// ---- the text ----------------------------------------------------------------------------
static void fresh(cvedit_t *e) {
    e->len = e->cursor = e->top = 0;
    e->hint = e->count_at = e->count = 0;
    e->goal = -1;
    e->crlf = e->modified = false;
    e->dir[0] = e->name[0] = 0;
    e->name_n = 0;
    e->mode = CVE_EDIT;
    e->save_due = false;
    e->redraw = true;
}

void cvedit_new(cvedit_t *e) {
    fresh(e);
    e->msg[0] = 0;
}

void cvedit_init(cvedit_t *e, const cvedit_config_t *cfg) {
    memset(e, 0, sizeof *e);
    e->cfg = *cfg;
    if (e->cfg.cols < 10) e->cfg.cols = 10;
    if (e->cfg.cols > 42) e->cfg.cols = 42;
    if (e->cfg.rows < 4) e->cfg.rows = 4;
    if (e->cfg.rows > 24) e->cfg.rows = 24;
    if (!e->cfg.ext) e->cfg.ext = ".txt";
    if (!e->cfg.stem) e->cfg.stem = "NOTE";
    fresh(e);
}

static void loaded(cvedit_t *e) {
    e->cursor = e->top = 0;
    e->hint = e->count_at = e->count = 0;
    e->goal = -1;
    e->crlf = false;
    for (uint32_t i = 0; i < e->len; i++)
        if (T[i] == '\n') { e->crlf = i > 0 && T[i - 1] == '\r'; break; }
    e->mode = CVE_EDIT;
    e->save_due = false;
    e->redraw = true;
}

// ---- spare files ---------------------------------------------------------------------------
static uint32_t crc_more(uint32_t c, const uint8_t *b, uint32_t n) {
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c;
}
static uint32_t crc_of(const uint8_t *b, uint32_t n) { return ~crc_more(0xFFFFFFFFu, b, n); }

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static int spare_name(uint16_t *out, int slot) {
    char s[16];
    snprintf(s, sizeof s, "EDITSAV%d.TMP", slot + 1);
    return widen(out, s);
}

static bool rd(fs_t *fs, uint32_t k, uint8_t *buf) {
    const uint32_t lba = fs_lba(fs, k);
    return lba && fs->read(lba, buf);
}

// The spare in dir's slot: 0 none, 1 there but not whole, 2 whole (its
// header in e->hdr, its file in *f), < 0 the card failed.
static int spare_state(cvedit_t *e, fs_t *fs, const char *dir, int slot, fs_file_t *f) {
    const int n = spare_name(e->tmp16, slot);
    bool made;
    int r = fs_create(fs, dir, e->tmp16, n, 0, f, &made);
    if (r == FS_ENOFILE) return 0;
    if (r == FS_ENAME) return 0;                // a folder of that name: not ours
    if (r) return r;
    if (f->size < HDR || f->size > HDR + (uint64_t)0x7FFFFFFF) return 1;
    if ((r = fs_open(fs, f))) return r == FS_EIO ? r : 1;
    if (!rd(fs, 0, e->hdr) || !rd(fs, 1, e->hdr + 512)) return FS_EIO;
    const uint32_t len = get32(e->hdr + 12);
    if (memcmp(e->hdr, MAGIC, 8) || get32(e->hdr + HDR - 4) != crc_of(e->hdr, HDR - 4)) return 1;
    if (len != f->size - HDR || get32(e->hdr + 20) < 1 || get32(e->hdr + 20) > 255) return 1;
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t at = 0, k = 2; at < len; at += 512, k++) {
        if (!rd(fs, k, e->sec)) return FS_EIO;
        c = crc_more(c, e->sec, len - at < 512 ? len - at : 512);
    }
    return ~c == get32(e->hdr + 16) ? 2 : 1;
}

// sector k of a file that is hdr (or nothing), then the text, then zeros
static void sector_of(const cvedit_t *e, bool hdr, uint32_t k, uint8_t *buf) {
    if (hdr && k < HDR / 512) {
        memcpy(buf, e->hdr + k * 512, 512);
        return;
    }
    const uint32_t at = (k - (hdr ? HDR / 512 : 0)) * 512;
    const uint32_t n = at >= e->len ? 0 : e->len - at < 512 ? e->len - at : 512;
    memcpy(buf, T + at, n);
    memset(buf + n, 0, 512 - n);
}

// the file written (a spare's first sector last) and read back
static int write_all(cvedit_t *e, fs_t *fs, const fs_file_t *f, bool hdr) {
    int r = fs_open(fs, f);
    if (r) return r;
    const uint32_t secs = (uint32_t)((f->size + 511) / 512);
    for (uint32_t i = 0; i < secs; i++) {
        const uint32_t k = hdr ? (i + 1 < secs ? i + 1 : 0) : i;
        sector_of(e, hdr, k, e->sec);
        if ((r = fs_write_sec(fs, k, e->sec))) return r;
        e->writes++;
    }
    for (uint32_t k = 0; k < secs; k++) {
        sector_of(e, hdr, k, e->sec);
        if (!rd(fs, k, e->sec2) || memcmp(e->sec, e->sec2, 512)) return FS_EIO;
    }
    return FS_OK;
}

// a spare's header wiped, then the spare deleted: its clusters never hold a
// header that looks whole once they are free
static int spare_drop(cvedit_t *e, fs_t *fs, const char *dir, int slot, const fs_file_t *f, bool wipe) {
    int r;
    if (wipe) {
        if ((r = fs_open(fs, f))) return r;
        memset(e->sec, 0, 512);
        if ((r = fs_write_sec(fs, 0, e->sec))) return r;
    }
    const int n = spare_name(e->tmp16, slot);
    r = fs_delete(fs, dir, e->tmp16, n);
    return r == FS_ENOFILE ? FS_OK : r;
}

// ---- saving --------------------------------------------------------------------------------
static int save_to_card(cvedit_t *e, fs_t *fs) {
    const char *dir = e->dir[0] ? e->dir : "/";
    fs_file_t sf[2], f;
    int st[2], r;
    uint32_t seq[2] = {0, 0};
    bool ours[2] = {false, false}, made;
    for (int s = 0; s < 2; s++) {
        if ((st[s] = spare_state(e, fs, dir, s, &sf[s])) < 0) return st[s];
        if (st[s] != 2) continue;
        seq[s] = get32(e->hdr + 8);
        const int n = (int)get32(e->hdr + 20);
        ours[s] = n == e->name_n && !memcmp(e->hdr + 24, e->name16, (size_t)n * 2);
    }
    // the spare to write: one that holds nothing whole, or the older of two
    const int w = st[0] != 2 ? 0 : st[1] != 2 ? 1 : seq[0] < seq[1] ? 0 : 1;
    if (st[w] && (r = spare_drop(e, fs, dir, w, &sf[w], st[w] == 2))) return r;
    if (st[1 - w] == 1) {                       // rubbish in the other: away with it
        if ((r = spare_drop(e, fs, dir, 1 - w, &sf[1 - w], false))) return r;
        st[1 - w] = 0;
    }
    memset(e->hdr, 0, HDR);
    memcpy(e->hdr, MAGIC, 8);
    put32(e->hdr + 8, (seq[1 - w] > seq[w] ? seq[1 - w] : seq[w]) + 1);
    put32(e->hdr + 12, e->len);
    put32(e->hdr + 16, crc_of(T, e->len));
    put32(e->hdr + 20, (uint32_t)e->name_n);
    memcpy(e->hdr + 24, e->name16, (size_t)e->name_n * 2);
    put32(e->hdr + HDR - 4, crc_of(e->hdr, HDR - 4));
    int n = spare_name(e->tmp16, w);
    if ((r = fs_create(fs, dir, e->tmp16, n, HDR + e->len, &sf[w], &made))) return r;
    if (!made) return FS_ECHAIN;
    if ((r = write_all(e, fs, &sf[w], true))) return r;
    // the text is safe in spare w: an older spare of this file can go
    if (st[1 - w] == 2 && ours[1 - w] && (r = spare_drop(e, fs, dir, 1 - w, &sf[1 - w], true))) return r;
    // the old file away, the new one in its place
    r = fs_delete(fs, dir, e->name16, e->name_n);
    if (r && r != FS_ENOFILE) return r;
    if (e->len) {
        if ((r = fs_create(fs, dir, e->name16, e->name_n, e->len, &f, &made))) return r;
        if (!made) return FS_ECHAIN;
        if ((r = write_all(e, fs, &f, false))) return r;
    } else if ((r = fs_create_empty(fs, dir, e->name16, e->name_n, &f, &made))) {
        return r;
    }
    return spare_drop(e, fs, dir, w, &sf[w], true);
}

int cvedit_save(cvedit_t *e) {
    e->save_due = false;
    fs_t *fs = card(e);
    if (!fs) { say(e, true, "Not saved: no card"); return CVEDIT_SAVE_FAILED; }
    if (!e->name_n) {                           // a name first: the first free one offered
        snprintf(e->save_dir, sizeof e->save_dir, "%s", home_of(e));
        e->typed[0] = 0;
        for (int k = 1; k < 1000; k++) {
            char s[CVEDIT_NAME_MAX + 1];
            int n = snprintf(s, sizeof s, "%.20s%d", e->cfg.stem, k);
            for (const char *x = e->cfg.ext; *x && n < CVEDIT_NAME_MAX; x++) s[n++] = (char)upper((unsigned char)*x);
            s[n] = 0;
            fs_file_t f;
            bool made;
            const int r = fs_create(fs, e->save_dir, e->tmp16, widen(e->tmp16, s), 0, &f, &made);
            if (r == FS_ENOFILE) { snprintf(e->typed, sizeof e->typed, "%s", s); break; }
            if (r != FS_OK && r != FS_ENAME) { fail(e, "Not saved", r); return CVEDIT_SAVE_FAILED; }
        }
        e->mode = CVE_NAME;
        e->msg[0] = 0;
        e->redraw = true;
        return CVEDIT_NONE;
    }
    if (!fs->write) { fail(e, "Not saved", FS_ENOWRITE); return CVEDIT_SAVE_FAILED; }
    e->writes = 0;
    fs_file_t d;
    int r = fs_find(fs, e->dir[0] ? e->dir : "/", &d);
    if (r == FS_OK && !d.is_dir) r = FS_ENOTDIR;
    if (r == FS_ENOFILE && (r = make_home(e, fs, e->dir)) != FS_ENOFILE) {
        if (r) { fail(e, "Not saved", r); return CVEDIT_SAVE_FAILED; }  // (FAT16's top folder full, a file of that name)
    }
    if (r == FS_ENOFILE || r == FS_ENOTDIR) {   // another card, or a folder whose name fsro can't spell
        e->name_n = 0;
        e->dir[0] = 0;
        say(e, true, "Folder not found. Save again to name it");
        return CVEDIT_SAVE_FAILED;
    }
    if (!r) r = save_to_card(e, fs);
    if (r) { fail(e, "Not saved", r); return CVEDIT_SAVE_FAILED; }
    e->modified = false;
    e->saves++;
    char s[sizeof e->msg];
    snprintf(s, sizeof s, "Saved %.20s, %lu bytes", e->name, (unsigned long)e->len);
    say(e, false, s);
    return CVEDIT_SAVED;
}

// the name typed: checked, then the save
static int name_done(cvedit_t *e) {
    char s[CVEDIT_NAME_MAX + 8];
    size_t n = strlen(e->typed);
    while (n && e->typed[n - 1] == ' ') n--;
    if (!n || e->typed[0] == '.' || e->typed[0] == ' ') { say(e, true, "Type a name first"); return CVEDIT_NONE; }
    memcpy(s, e->typed, n);
    s[n] = 0;
    if (!strchr(s, '.')) {
        for (const char *x = e->cfg.ext; *x && n < CVEDIT_NAME_MAX; x++) s[n++] = (char)upper((unsigned char)*x);
        s[n] = 0;
    }
    fs_t *fs = card(e);
    if (!fs) { say(e, true, "Not saved: no card"); return CVEDIT_SAVE_FAILED; }
    fs_file_t f;
    bool made;
    const int wn = widen(e->tmp16, s);
    const int r = fs_create(fs, e->save_dir, e->tmp16, wn, 0, &f, &made);
    if (r == FS_OK || r == FS_ENAME) { say(e, true, r ? "That name can't be used" : "That name is taken"); return CVEDIT_NONE; }
    if (r != FS_ENOFILE) { fail(e, "Not saved", r); return CVEDIT_SAVE_FAILED; }
    set_name(e, e->save_dir, e->tmp16, wn);
    e->mode = CVE_EDIT;
    return cvedit_save(e);
}

// ---- opening ---------------------------------------------------------------------------------
static int load_file(cvedit_t *e, fs_t *fs, const char *dir, const fs_file_t *f) {
    char s[sizeof e->msg];
    if (f->is_dir) return fail(e, "Not opened", FS_ENAME);
    if (f->size > e->cfg.cap) {
        snprintf(s, sizeof s, "Too big: %lu KB. The most is %lu KB", (unsigned long)((f->size + 1023) / 1024),
                 (unsigned long)(e->cfg.cap / 1024));
        say(e, true, s);
        return FS_EFULL;
    }
    int r;
    // the name it is saved under, read before the text goes in (it uses fs's scratch, not ours)
    int n = f->first_clus >= 2 ? fs_name16(fs, dir, f->first_clus, e->tmp16, FS_LNAME - 1) : -1;
    if (n <= 0 && !strchr(f->name, '?') && strlen(f->name) < FS_NAME - 1) n = widen(e->tmp16, f->name);
    if (f->size && (r = fs_open(fs, f))) return fail(e, "Not opened", r);
    const uint32_t size = (uint32_t)f->size;
    for (uint32_t at = 0, k = 0; at < size; at += 512, k++) {
        if (!rd(fs, k, e->sec)) {               // the old text is gone by now: start empty
            fresh(e);
            return fail(e, "Not opened", FS_EIO);
        }
        memcpy(T + at, e->sec, size - at < 512 ? size - at : 512);
    }
    e->len = size;
    e->modified = false;
    if (n > 0) set_name(e, dir, e->tmp16, n);
    else {                                      // no name to save it under: Save will ask
        e->name_n = 0;
        e->dir[0] = 0;
        snprintf(e->name, sizeof e->name, "%s", f->name);
    }
    loaded(e);
    snprintf(s, sizeof s, "%lu bytes%s", (unsigned long)e->len, e->crlf ? ", CR LF lines" : "");
    say(e, false, s);
    return FS_OK;
}

static bool check_dir(cvedit_t *e, fs_t *fs, const char *dir) {
    fs_file_t f, best;
    int slot = -1;
    uint32_t seq = 0;
    for (int s = 0; s < 2; s++) {
        const int st = spare_state(e, fs, dir, s, &f);
        if (st == 1 && fs->write) spare_drop(e, fs, dir, s, &f, false);
        if (st == 2 && (slot < 0 || get32(e->hdr + 8) > seq)) {
            slot = s;
            seq = get32(e->hdr + 8);
            best = f;
        }
    }
    if (slot < 0) return false;
    if (spare_state(e, fs, dir, slot, &best) != 2) return false;        // its header again
    const int n = (int)get32(e->hdr + 20);
    int i = 0;
    for (; i < n && i < FS_NAME - 1; i++) {
        const uint16_t ch = (uint16_t)(e->hdr[24 + 2 * i] | e->hdr[25 + 2 * i] << 8);
        e->found_name[i] = ch >= 32 && ch < 127 ? (char)ch : '?';
    }
    e->found_name[i] = 0;
    snprintf(e->found_dir, sizeof e->found_dir, "%s", dir);
    e->found_slot = slot;
    e->mode = CVE_FOUND;
    e->msg[0] = 0;
    e->redraw = true;
    return true;
}

bool cvedit_check(cvedit_t *e, const char *dir) {
    fs_t *fs = card(e);
    if (!fs) return false;
    if (dir && dir[0] && check_dir(e, fs, dir)) return true;
    const char *home = home_of(e);
    return !(dir && dir[0] && same_path(dir, home)) && check_dir(e, fs, home);
}

static void split(const char *path, char *dir, size_t max, const char **last) {
    const char *slash = strrchr(path, '/');
    *last = slash ? slash + 1 : path;
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (n >= max) n = max - 1;
    memcpy(dir, path, n);
    dir[n] = 0;
    if (!n) snprintf(dir, max, "/");
}

int cvedit_load(cvedit_t *e, const char *path, const fs_file_t *f) {
    fs_t *fs = card(e);
    if (!fs) { say(e, true, "Not opened: no card"); return FS_EIO; }
    char dir[FS_LNAME];
    const char *last;
    split(path, dir, sizeof dir, &last);
    if (cvedit_check(e, dir)) {                 // first things first
        snprintf(e->wait_path, sizeof e->wait_path, "%s", path);
        e->wait = true;
        e->wait_has_file = f != NULL;
        if (f) e->wait_file = *f;
        return FS_OK;
    }
    fs_file_t found;
    if (!f) {
        const int r = fs_find(fs, path, &found);
        if (r) return fail(e, "Not opened", r);
        f = &found;
    }
    return load_file(e, fs, dir, f);
}

// the save found cut short: finished (the text from the spare, saved as usual), or dropped
static int found_answer(cvedit_t *e, bool finish) {
    fs_t *fs = card(e);
    fs_file_t f;
    e->mode = CVE_EDIT;
    if (!fs) { say(e, true, "No card"); return CVEDIT_NONE; }
    const int st = spare_state(e, fs, e->found_dir, e->found_slot, &f);
    if (st < 0) { fail(e, "Not done", st); return CVEDIT_NONE; }
    if (st == 2 && finish) {
        const uint32_t len = get32(e->hdr + 12);
        if (len > e->cfg.cap) { say(e, true, "It is too big for this editor"); return CVEDIT_NONE; }
        const int n = (int)get32(e->hdr + 20);
        for (int i = 0; i < n; i++) e->tmp16[i] = (uint16_t)(e->hdr[24 + 2 * i] | e->hdr[25 + 2 * i] << 8);
        for (uint32_t at = 0, k = 2; at < len; at += 512, k++) {
            if (!rd(fs, k, e->sec)) { fresh(e); fail(e, "Not done", FS_EIO); return CVEDIT_OPENED; }
            memcpy(T + at, e->sec, len - at < 512 ? len - at : 512);
        }
        e->len = len;
        set_name(e, e->found_dir, e->tmp16, n);
        loaded(e);
        e->modified = true;
        e->wait = false;
        cvedit_save(e);
        return CVEDIT_OPENED;
    }
    if (st == 2) {
        const int r = spare_drop(e, fs, e->found_dir, e->found_slot, &f, true);
        if (r) { fail(e, "Not dropped", r); return CVEDIT_NONE; }
    }
    if (e->wait) {
        e->wait = false;
        return cvedit_load(e, e->wait_path, e->wait_has_file ? &e->wait_file : NULL) == FS_OK ? CVEDIT_OPENED : CVEDIT_NONE;
    }
    e->redraw = true;
    return CVEDIT_NONE;
}

// ---- Open's list -----------------------------------------------------------------------------
static int cmp_entry(const cvedit_entry_t *a, const cvedit_entry_t *b) {
    if (a->where != b->where) return a->where - b->where;
    for (const char *p = a->name, *q = b->name;; p++, q++) {
        const int d = lower((unsigned char)*p) - lower((unsigned char)*q);
        if (d || !*p) return d;
    }
}

static void open_list(cvedit_t *e) {
    fs_t *fs = card(e);
    if (!fs) { say(e, true, "No card"); return; }
    snprintf(e->list_dir[0], FS_LNAME, "%s", home_of(e));
    e->list_dir[1][0] = 0;
    if (e->dir[0] && !same_path(e->dir, e->list_dir[0])) snprintf(e->list_dir[1], FS_LNAME, "%s", e->dir);
    e->list_n = 0;
    for (int w = 0; w < 2; w++) {
        if (!e->list_dir[w][0]) continue;
        int r = fs_dir_open_path(fs, &e->d, e->list_dir[w]);
        if (r == FS_ENOFILE || r == FS_ENOTDIR) continue;
        fs_file_t f;
        while (!r && (r = fs_dir_next(fs, &e->d, &f)) == 1) {
            r = 0;
            if (f.is_dir || f.attr & (FS_A_HIDDEN | FS_A_SYSTEM) || e->d.lname[0] == '.') continue;
            if (!has_ext(e->d.lname, e->cfg.ext) || e->list_n == CVEDIT_LIST_MAX) continue;
            cvedit_entry_t *x = &e->list[e->list_n++];
            snprintf(x->name, sizeof x->name, "%s", e->d.lname);
            x->clus = f.first_clus;
            x->size = f.size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)f.size;
            x->contig = f.contig;
            x->where = (uint8_t)w;
        }
        if (r < 0) { fail(e, "Not listed", r); return; }
    }
    if (!e->list_n) {
        char s[sizeof e->msg];
        snprintf(s, sizeof s, "No %.8s files in %.20s", e->cfg.ext, e->list_dir[0]);
        say(e, false, s);
        return;
    }
    for (int i = 1; i < e->list_n; i++) {       // by folder, then by name
        const cvedit_entry_t x = e->list[i];
        int j = i;
        for (; j > 0 && cmp_entry(&x, &e->list[j - 1]) < 0; j--) e->list[j] = e->list[j - 1];
        e->list[j] = x;
    }
    e->list_at = e->list_top = 0;
    e->mode = CVE_LIST;
    e->msg[0] = 0;
    e->redraw = true;
}

static void list_move(cvedit_t *e, int by) {
    int at = e->list_at + by;
    if (at < 0) at = 0;
    if (at >= e->list_n) at = e->list_n - 1;
    e->list_at = at;
    if (at < e->list_top) e->list_top = at;
    if (at >= e->list_top + ROWS) e->list_top = at - ROWS + 1;
    e->redraw = true;
}

static int list_pick(cvedit_t *e) {
    const cvedit_entry_t *x = &e->list[e->list_at];
    const char *dir = e->list_dir[x->where];
    e->mode = CVE_EDIT;
    e->redraw = true;
    fs_t *fs = card(e);
    if (!fs) { say(e, true, "Not opened: no card"); return CVEDIT_NONE; }
    if (cvedit_check(e, dir)) return CVEDIT_NONE;       // (pick the file again after)
    fs_file_t f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", x->name);
    f.size = x->size;
    f.first_clus = x->clus;
    f.contig = x->contig;
    return load_file(e, fs, dir, &f) == FS_OK ? CVEDIT_OPENED : CVEDIT_NONE;
}

// ---- keys ------------------------------------------------------------------------------------
static int command(cvedit_t *e, int cmd) {
    if (cmd == CMD_SAVE) {
        if (e->name_n) {                        // after SAVING... is on the screen (cvedit_task)
            say(e, false, "Saving...");
            e->save_due = true;
            e->drawn = false;
            e->save_t = e->now;
            return CVEDIT_NONE;
        }
        return cvedit_save(e);
    }
    if (e->modified && !(e->mode == CVE_ASK && e->ask == cmd)) {
        e->mode = CVE_ASK;
        e->ask = (uint8_t)cmd;
        e->msg[0] = 0;
        e->redraw = true;
        return CVEDIT_NONE;
    }
    e->mode = CVE_EDIT;
    e->redraw = true;
    if (cmd == CMD_OPEN) {
        open_list(e);
        return CVEDIT_NONE;
    }
    cvedit_new(e);
    return CVEDIT_OPENED;
}

// a key or button outside the text: -1 if this mode has no use for it
// (what: a CMD_*, or 'E' Enter, 'X' Esc, 'U' 'D' up and down, 'P' 'N' pages)
static int mode_key(cvedit_t *e, int what) {
    switch (e->mode) {
    case CVE_ASK:
        if (what == e->ask) return command(e, what);
        e->mode = CVE_EDIT;
        if (what == CMD_SAVE) return command(e, what);
        e->redraw = true;
        return CVEDIT_NONE;
    case CVE_FOUND:
        if (what == CMD_SAVE || what == CMD_NEW) return found_answer(e, what == CMD_SAVE);
        return CVEDIT_NONE;
    case CVE_LIST:
        if (what == 'U' || what == 'D') list_move(e, what == 'U' ? -1 : 1);
        else if (what == 'P' || what == 'N') list_move(e, what == 'P' ? -ROWS : ROWS);
        else if (what == 'E') return list_pick(e);
        else if (what == 'X' || what == CMD_OPEN) { e->mode = CVE_EDIT; e->redraw = true; }
        return CVEDIT_NONE;
    case CVE_NAME:
        if (what == 'E') return name_done(e);
        if (what == 'X') { e->mode = CVE_EDIT; e->msg[0] = 0; e->redraw = true; }
        return CVEDIT_NONE;
    }
    return -1;
}

static int key_down(cvedit_t *e, const cvkey_t *k) {
    const uint8_t u = k->usage;
    if (u >= 0xE0) return CVEDIT_NONE;          // a modifier alone
    const bool ctrl = k->mods & CVKEY_CTRL;
    if (k->mods & (CVKEY_ALT | CVKEY_GUI)) return CVEDIT_NONE;
    const int letter = ctrl && u >= 0x04 && u <= 0x1D ? 'A' + u - 4 : 0;
    const int cmd = letter == 'S' ? CMD_SAVE : letter == 'O' ? CMD_OPEN : letter == 'N' ? CMD_NEW : 0;
    if (e->mode != CVE_EDIT) {
        if (e->mode == CVE_NAME && !ctrl && k->ascii >= 32 && k->ascii < 127) {
            const size_t n = strlen(e->typed);
            if (strchr("\"*/:<>?\\|", k->ascii)) say(e, true, "A name can't have that character");
            else if (n < CVEDIT_NAME_MAX && (int)n < W - 8) { e->typed[n] = k->ascii; e->typed[n + 1] = 0; e->msg[0] = 0; }
            e->redraw = true;
            return CVEDIT_NONE;
        }
        if (e->mode == CVE_NAME && k->ascii == '\b') {
            const size_t n = strlen(e->typed);
            if (n) e->typed[n - 1] = 0;
            e->msg[0] = 0;
            e->redraw = true;
            return CVEDIT_NONE;
        }
        const int digit = !ctrl && k->ascii >= '1' && k->ascii <= '3' ? k->ascii - '0' : 0;   // as the keypad's
        const int what = cmd ? cmd : digit ? digit : k->ascii == '\r' ? 'E' : k->ascii == 27 ? 'X' : u == 0x52 ? 'U' : u == 0x51 ? 'D' :
                         u == 0x4B ? 'P' : u == 0x4E ? 'N' : 0;
        return mode_key(e, what);
    }
    if (e->msg[0]) { e->msg[0] = 0; e->redraw = true; }
    if (cmd) return command(e, cmd);
    if (ctrl && (u == 0x4A || u == 0x4D)) { nav(e, u == 0x4A ? NAV_TOP : NAV_BOTTOM); return CVEDIT_NONE; }
    if (letter) return CVEDIT_HOST_CTRL + letter;
    if (ctrl) return CVEDIT_NONE;
    if (u >= 0x3A && u <= 0x45) return CVEDIT_HOST_FKEY + 1 + (u - 0x3A);
    switch (u) {
    case 0x4F: nav(e, NAV_RIGHT); return CVEDIT_NONE;
    case 0x50: nav(e, NAV_LEFT); return CVEDIT_NONE;
    case 0x51: nav(e, NAV_DOWN); return CVEDIT_NONE;
    case 0x52: nav(e, NAV_UP); return CVEDIT_NONE;
    case 0x4A: nav(e, NAV_HOME); return CVEDIT_NONE;
    case 0x4D: nav(e, NAV_END); return CVEDIT_NONE;
    case 0x4B: nav(e, NAV_PGUP); return CVEDIT_NONE;
    case 0x4E: nav(e, NAV_PGDN); return CVEDIT_NONE;
    }
    const uint8_t ch = (uint8_t)k->ascii;
    if (ch == '\r') cvedit_insert(e, (const uint8_t *)"\r\n" + (e->crlf ? 0 : 1), e->crlf ? 2 : 1);
    else if (ch == '\b') cut(e, left_of(e, e->cursor), e->cursor);
    else if (ch == 127) cut(e, e->cursor, right_of(e, e->cursor));
    else if (ch == '\t' || (ch >= 32 && ch < 127)) cvedit_insert(e, &ch, 1);
    return CVEDIT_NONE;
}

static bool repeats(const cvedit_t *e, const cvkey_t *k) {
    if (k->mods & (CVKEY_CTRL | CVKEY_ALT | CVKEY_GUI) || k->usage >= 0xE0) return false;
    if (k->ascii == 27 || (k->usage >= 0x3A && k->usage <= 0x45)) return false;
    if (e->mode == CVE_EDIT) return true;
    return k->ascii != '\r' && (e->mode == CVE_LIST || e->mode == CVE_NAME);
}

int cvedit_key(cvedit_t *e, const cvkey_t *k) {
    if (!k->down) {
        if (e->held.usage == k->usage) e->held.down = false;
        return CVEDIT_NONE;
    }
    e->held = *k;
    e->held_t = e->now;
    e->repeating = false;
    return key_down(e, k);
}

int cvedit_pad(cvedit_t *e, uint8_t pressed, int key) {
    int ev = CVEDIT_NONE;
    if (key >= 0) {
        const int cmd = key == 1 ? CMD_SAVE : key == 2 ? CMD_OPEN : key == 3 ? CMD_NEW : 0;
        if (e->mode != CVE_EDIT) {
            ev = mode_key(e, cmd ? cmd : key == CVEDIT_STAR ? 'X' : 0);
        } else {
            if (e->msg[0]) { e->msg[0] = 0; e->redraw = true; }
            ev = cmd ? command(e, cmd) : CVEDIT_HOST_KEYPAD + key;
        }
        if (ev < 0) ev = CVEDIT_NONE;
    }
    if (!pressed || ev) return ev;
    if (e->mode == CVE_EDIT) {
        if (pressed & CVEDIT_UP) nav(e, NAV_UP);
        if (pressed & CVEDIT_DOWN) nav(e, NAV_DOWN);
        if (pressed & CVEDIT_LEFT) nav(e, NAV_LEFT);
        if (pressed & CVEDIT_RIGHT) nav(e, NAV_RIGHT);
    } else if (e->mode == CVE_LIST) {
        if (pressed & CVEDIT_UP) mode_key(e, 'U');
        if (pressed & CVEDIT_DOWN) mode_key(e, 'D');
        if (pressed & (CVEDIT_FIRE_L | CVEDIT_FIRE_R)) ev = mode_key(e, 'E');
    }
    return ev < 0 ? CVEDIT_NONE : ev;
}

int cvedit_task(cvedit_t *e, uint32_t now_us) {
    e->now = now_us;
    if (e->save_due && (e->drawn || now_us - e->save_t > SAVE_WAIT_US)) return cvedit_save(e);
    if (e->held.down && now_us - e->held_t >= (e->repeating ? REPEAT_US : REPEAT_WAIT_US)) {
        if (!repeats(e, &e->held)) { e->held.down = false; return CVEDIT_NONE; }
        e->held_t = now_us;
        e->repeating = true;
        const int ev = key_down(e, &e->held);
        return ev < 0 ? CVEDIT_NONE : ev;
    }
    return CVEDIT_NONE;
}

// ---- for the host ------------------------------------------------------------------------------
// the line ends before p, counted from where they were last counted to
static uint32_t line_count(cvedit_t *e, uint32_t p) {
    if (p > e->len) p = e->len;
    if (e->count_at > e->len) e->count_at = e->count = 0;
    for (; e->count_at < p; e->count_at++) e->count += T[e->count_at] == '\n';
    for (; e->count_at > p; e->count_at--) e->count -= T[e->count_at - 1] == '\n';
    return e->count;
}

uint32_t cvedit_line(const cvedit_t *e) { return line_count((cvedit_t *)e, e->cursor) + 1; }

void cvedit_goto(cvedit_t *e, uint32_t line) {
    uint32_t at = 0;
    for (uint32_t i = 0, n = 1; i < e->len && n < line; i++)
        if (T[i] == '\n') { n++; at = i + 1; }
    e->cursor = at;
    e->goal = -1;
    show(e, ROWS / 3);
}

void cvedit_bars(cvedit_t *e, const char *top, const char *bottom) {
    e->top_text = top;
    e->keys = bottom;
    e->redraw = true;
}

void cvedit_message(cvedit_t *e, const char *msg, bool bad) { say(e, bad, msg); }
void cvedit_redraw(cvedit_t *e) { e->redraw = true; }
bool cvedit_modified(const cvedit_t *e) { return e->modified; }
bool cvedit_editing(const cvedit_t *e) { return e->mode == CVE_EDIT; }

void cvedit_path(const cvedit_t *e, char *buf, size_t max) {
    if (!e->name_n) { if (max) buf[0] = 0; return; }
    snprintf(buf, max, "%s/%s", e->dir[0] && strcmp(e->dir, "/") ? e->dir : "", e->name);
}

// ---- the screen ----------------------------------------------------------------------------------
static void bar(const cvedit_t *e, canvas_t *c, int row, const char *s, uint8_t fg, uint8_t bg) {
    const int y = e->cfg.y + row * CVFONT_H;
    char cutl[48];
    snprintf(cutl, sizeof cutl, "%.*s", W, s);   // the cell past the last column stays empty
    cvfont_line(c, e->cfg.x, y, W + 1, cutl, fg, bg);
    // the bar runs a little past the text each side (to the 8-pixel runs' edges, where colours change)
    const int x1 = e->cfg.x + (W + 1) * CVFONT_W, x2 = (x1 + 7) & ~7;
    if (e->cfg.x >= 8) canvas_fill(c, e->cfg.x - 8, y, 8, CVFONT_H, bg);
    if (x2 <= VDP_W) canvas_fill(c, x1, y, x2 - x1, CVFONT_H, bg);
}

static void draw_text(const cvedit_t *e, canvas_t *c) {
    const bool cursor_on = e->mode == CVE_EDIT || e->mode == CVE_ASK;
    uint32_t s = e->top;
    for (int r = 0; r < ROWS; r++) {
        const int y = e->cfg.y + (1 + r) * CVFONT_H;
        int col = 0, ccol = -1, cch = ' ';
        if (s != NONE) {
            uint32_t end;
            const uint32_t n = line_next(e, s, &end);
            const bool mine = cursor_on && e->cursor >= s && (n == NONE || (n == end ? e->cursor < n : e->cursor <= end));
            for (uint32_t i = s; i < end; i++) {
                const uint8_t ch = T[i];
                const int nc = after(e, col, ch);
                const int shown = ch == '\t' ? ' ' : ch < 32 || ch > 126 ? '?' : ch;
                if (mine && i == e->cursor) { ccol = col; cch = shown; }
                // (a blank that hangs off the end has no cell: nc == col)
                for (int k = col; k < nc; k++) cvfont_char(c, e->cfg.x + k * CVFONT_W, y, k == col ? shown : ' ', C_WHITE, C_BLACK);
                col = nc;
            }
            if (mine && ccol < 0) ccol = col;   // at the line's end
            s = n;
        }
        for (int k = col; k <= W; k++) cvfont_char(c, e->cfg.x + k * CVFONT_W, y, ' ', C_WHITE, C_BLACK);
        if (ccol >= 0) cvfont_char(c, e->cfg.x + ccol * CVFONT_W, y, ccol < W ? cch : ' ', C_BLACK, C_WHITE);
    }
}

static void draw_list(const cvedit_t *e, canvas_t *c) {
    for (int r = 0; r < ROWS; r++) {
        const int i = e->list_top + r, y = e->cfg.y + (1 + r) * CVFONT_H;
        char s[64] = "";
        if (i < e->list_n) {
            const cvedit_entry_t *x = &e->list[i];
            char size[16];
            if (x->size < 10000) snprintf(size, sizeof size, "%lu", (unsigned long)x->size);
            else snprintf(size, sizeof size, "%luK", (unsigned long)((x->size + 1023) / 1024));
            snprintf(s, sizeof s, " %-*.*s %6s", W - 9, W - 9, x->name, size);
        }
        const bool at = i == e->list_at && i < e->list_n;
        cvfont_line(c, e->cfg.x, y, W + 1, s, at ? C_BLACK : C_WHITE, at ? C_WHITE : C_BLACK);
    }
}

void cvedit_draw(cvedit_t *e, canvas_t *c, bool all) {
    if (all) e->redraw = true;
    if (!e->redraw) return;
    e->redraw = false;
    e->drawn = true;
    char s[96], t[64];
    // the top line
    if (e->mode == CVE_LIST) {
        snprintf(s, sizeof s, "Open from %s", e->list_dir[e->list[e->list_at].where]);
    } else if (e->mode == CVE_NAME) {
        snprintf(s, sizeof s, "Save in %s as:", e->save_dir);
    } else if (e->mode == CVE_FOUND) {
        snprintf(s, sizeof s, "A save of %.18s was cut short", e->found_name);
    } else if (e->top_text) {
        snprintf(s, sizeof s, "%s", e->top_text);
    } else {
        snprintf(t, sizeof t, "Line %lu", (unsigned long)cvedit_line(e));
        const int room = W - (int)strlen(t) - 2;
        snprintf(s, sizeof s, "%.*s%s", room > 0 ? room - 1 : 0, e->name[0] ? e->name : "New text", e->modified ? "*" : "");
        snprintf(s + strlen(s), sizeof s - strlen(s), "%*s", W - (int)strlen(s), t);
    }
    bar(e, c, 0, s, C_BLACK, C_CYAN);
    // the text, or the list
    if (e->mode == CVE_LIST) draw_list(e, c);
    else draw_text(e, c);
    // the bottom line
    uint8_t fg = C_BLACK, bg = C_GRAY;
    const char *keys = e->keys ? e->keys : e->cfg.keys ? e->cfg.keys : "";
    if (e->msg[0]) {
        snprintf(s, sizeof s, "%s", e->msg);
        fg = e->msg_bad ? C_WHITE : C_BLACK;
        bg = e->msg_bad ? C_DRED : C_LYELLOW;
    } else if (e->mode == CVE_ASK) {
        snprintf(s, sizeof s, "Not saved! %d again: drop it  1: save", e->ask);
        bg = C_LYELLOW;
    } else if (e->mode == CVE_FOUND) {
        snprintf(s, sizeof s, "1: finish the save  3: drop it");
        bg = C_LYELLOW;
    } else if (e->mode == CVE_LIST) {
        snprintf(s, sizeof s, "Enter or fire: open   Esc or 2: back");
    } else if (e->mode == CVE_NAME) {
        snprintf(s, sizeof s, "Name: %s", e->typed);
    } else {
        snprintf(s, sizeof s, "%s", keys);
    }
    bar(e, c, e->cfg.rows - 1, s, fg, bg);
    if (e->mode == CVE_NAME && !e->msg[0]) {    // the name's cursor
        const int col = 6 + (int)strlen(e->typed);
        if (col < W) cvfont_char(c, e->cfg.x + col * CVFONT_W, e->cfg.y + (e->cfg.rows - 1) * CVFONT_H, ' ', bg, fg);
    }
}
