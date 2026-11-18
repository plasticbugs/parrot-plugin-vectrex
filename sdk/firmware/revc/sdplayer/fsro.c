// fsro.c -- see fsro.h
#include <stddef.h>
#include <string.h>
#include "fsro.h"

#define NONE 0xffffffffu
#define MAX_DIR_SECTORS 8192               // bounds a corrupt directory chain

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

static const uint8_t *get(fs_t *fs, uint32_t lba) {
    if (fs->cache_lba != lba) {
        if (!fs->read(lba, fs->cache)) { fs->cache_lba = NONE; return NULL; }
        fs->cache_lba = lba;
    }
    return fs->cache;
}

static uint32_t clus_lba(const fs_t *fs, uint32_t c) { return fs->heap_lba + ((c - 2) << fs->spc_shift); }

// next cluster in a chain; 0 at the end of the chain or on anything invalid
static uint32_t next_clus(fs_t *fs, uint32_t c) {
    const uint8_t *b;
    uint32_t v;
    if (fs->type == FS_FAT16) {
        if (!(b = get(fs, fs->fat_lba + c / 256))) return 0;
        v = le16(b + c % 256 * 2);
    } else {
        if (!(b = get(fs, fs->fat_lba + c / 128))) return 0;
        v = le32(b + c % 128 * 4);
        if (fs->type == FS_FAT32) v &= 0x0fffffff;
    }
    if (v < 2 || v >= fs->nclus + 2) return 0;   // free, bad, end of chain, out of range
    return v;
}

static int ctz32(uint32_t v) { int n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }

static int try_boot_sector(fs_t *fs, uint32_t lba) {
    const uint8_t *b = get(fs, lba);
    if (!b) return FS_EIO;
    if (!memcmp(b + 3, "EXFAT   ", 8)) {
        if (b[108] != 9 || b[109] > 16) return FS_ENOFS;          // 512-byte sectors only
        fs->type = FS_EXFAT;
        fs->part_lba = lba;
        fs->fat_lba = lba + le32(b + 80);
        fs->heap_lba = lba + le32(b + 88);
        fs->nclus = le32(b + 92);
        fs->root_clus = le32(b + 96);
        fs->spc_shift = b[109];
        fs->root_secs = 0;
        fs->nfats = b[110];
        fs->fat_secs = le32(b + 84);
        return FS_OK;
    }
    if ((b[0] != 0xEB && b[0] != 0xE9) || le16(b + 510) != 0xAA55 || le16(b + 11) != 512) return FS_ENOFS;
    uint32_t spc = b[13];
    if (!spc || (spc & (spc - 1))) return FS_ENOFS;
    uint32_t rsvd = le16(b + 14), nfats = b[16], rootent = le16(b + 17);
    uint32_t tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    uint32_t fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (!rsvd || !nfats || !fatsz || !tot) return FS_ENOFS;
    uint32_t root_secs = (rootent * 32 + 511) / 512;
    uint32_t data = rsvd + nfats * fatsz + root_secs;
    if (data >= tot) return FS_ENOFS;
    uint32_t nclus = (tot - data) / spc;
    if (nclus < 4085) return FS_ENOFS;                             // FAT12
    fs->type = nclus < 65525 ? FS_FAT16 : FS_FAT32;
    fs->nfats = nfats;
    fs->fat_secs = fatsz;
    fs->fsinfo_lba = fs->type == FS_FAT32 && le16(b + 48) && le16(b + 48) != 0xFFFF ? lba + le16(b + 48) : 0;
    fs->part_lba = lba;
    fs->fat_lba = lba + rsvd;
    fs->heap_lba = lba + data;
    fs->root_lba = lba + rsvd + nfats * fatsz;
    fs->root_secs = fs->type == FS_FAT16 ? root_secs : 0;
    fs->root_clus = fs->type == FS_FAT32 ? le32(b + 44) : 0;
    fs->nclus = nclus;
    fs->spc_shift = (uint32_t)ctz32(spc);
    return FS_OK;
}

static int try_gpt(fs_t *fs) {
    const uint8_t *h = get(fs, 1);
    if (!h) return FS_EIO;
    if (memcmp(h, "EFI PART", 8)) return FS_ENOFS;
    uint32_t ent_lba = (uint32_t)le64(h + 72), n = le32(h + 80), esz = le32(h + 84);
    if (esz < 128 || esz > 512 || 512 % esz) return FS_ENOFS;
    if (n > 128) n = 128;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = get(fs, ent_lba + i * esz / 512);
        if (!e) return FS_EIO;
        e += i * esz % 512;
        static const uint8_t zero[16];
        uint64_t first = le64(e + 32);
        if (!memcmp(e, zero, 16) || !first || first >> 32) continue;
        int r = try_boot_sector(fs, (uint32_t)first);
        if (r != FS_ENOFS) return r;
    }
    return FS_ENOFS;
}

int fs_mount(fs_t *fs, fs_read_fn rd) {
    memset(fs, 0, sizeof *fs);
    fs->read = rd;
    fs->cache_lba = NONE;
    int r = try_boot_sector(fs, 0);                    // no partition table
    if (r != FS_ENOFS) return r;
    const uint8_t *b = get(fs, 0);
    if (!b) return FS_EIO;
    if (le16(b + 510) != 0xAA55) return FS_ENOFS;
    uint8_t pt[64];
    memcpy(pt, b + 446, 64);
    for (int i = 0; i < 4; i++) {
        uint8_t type = pt[i * 16 + 4];
        uint32_t lba = le32(pt + i * 16 + 8);
        if (type == 0xEE) return try_gpt(fs);
        if (!type || !lba) continue;
        r = try_boot_sector(fs, lba);
        if (r != FS_ENOFS) return r;
    }
    return FS_ENOFS;
}

const char *fs_type_name(const fs_t *fs) {
    return fs->type == FS_EXFAT ? "exFAT" : fs->type == FS_FAT32 ? "FAT32" : fs->type == FS_FAT16 ? "FAT16" : "none";
}

// ---- directories ------------------------------------------------------------
static uint32_t dir_sector(fs_t *fs, fs_dir_t *d) {   // next directory sector, 0 at the end
    if (++d->count > MAX_DIR_SECTORS) return 0;
    if (d->fixed) {
        if (!d->left) return 0;
        d->left--;
        return d->lba++;
    }
    if (!d->left) {
        if (d->started) {
            if (d->contig) {
                if (!--d->clus_left) return 0;
                d->clus++;
            } else if (!(d->clus = next_clus(fs, d->clus))) return 0;
        }
        d->started = true;
        if (d->clus < 2 || d->clus >= fs->nclus + 2) return 0;
        d->lba = clus_lba(fs, d->clus);
        d->left = 1u << fs->spc_shift;
    }
    d->left--;
    return d->lba++;
}

int fs_dir_open(fs_t *fs, fs_dir_t *d, const fs_file_t *dir) {
    memset(d, 0, sizeof *d);
    d->k = 16;                                       // no sector yet
    if (dir && !dir->is_dir) return FS_ENOTDIR;
    if (!dir || !dir->first_clus) {                  // the root
        if (fs->type == FS_FAT16) { d->fixed = true; d->lba = fs->root_lba; d->left = fs->root_secs; }
        else d->clus = fs->root_clus;
        return FS_OK;
    }
    d->clus = dir->first_clus;
    if (dir->contig) {                               // exFAT NoFatChain: its size bounds it
        uint64_t cb = (uint64_t)512 << fs->spc_shift;
        uint64_t n = (dir->size + cb - 1) / cb;
        if (!n || dir->first_clus - 2 + n > fs->nclus) return FS_ECHAIN;
        d->contig = true;
        d->clus_left = (uint32_t)n;
    }
    return FS_OK;
}

static char ascii(uint16_t ch) { return ch >= 32 && ch < 127 ? (char)ch : '?'; }

static uint8_t sfn_sum(const uint8_t *e) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[i]);
    return s;
}

static void entry(fs_dir_t *d, fs_file_t *e, uint64_t size, uint32_t clus, bool contig, uint8_t attr) {
    strncpy(e->name, d->lname, FS_NAME - 1);
    e->name[FS_NAME - 1] = 0;
    e->size = size;
    e->first_clus = clus;
    e->contig = contig;
    e->attr = attr;
    e->is_dir = attr & FS_A_DIR;
}

int fs_dir_next(fs_t *fs, fs_dir_t *d, fs_file_t *out) {
    char *name = d->lname;
    for (;;) {
        if (d->done) return 0;
        if (d->k >= 16) {
            uint32_t lba = dir_sector(fs, d);
            if (!lba) { d->done = true; return 0; }
            const uint8_t *b = get(fs, lba);
            if (!b) return FS_EIO;
            memcpy(d->sec, b, 512);                  // next_clus() and reads reuse the cache
            d->k = 0;
        }
        const uint8_t *e = d->sec + d->k++ * 32;
        if (fs->type == FS_EXFAT) {
            uint8_t t = e[0];
            if (!t) { d->done = true; return 0; }    // end of directory
            if (!(t & 0x80)) { d->set_left = 0; continue; }   // unused entry
            if (t == 0x85) {                         // File: starts a set
                d->set_left = e[1]; d->attr = le16(e + 4);
                d->got_stream = false; d->nidx = 0; d->name_len = 0;
                continue;
            }
            if (!d->set_left) continue;              // bitmap, up-case table, label...
            d->set_left--;
            if (t == 0xC0) {                         // Stream Extension
                d->flags = e[1]; d->name_len = e[3];
                d->size = d->attr & FS_A_DIR ? le64(e + 24) : le64(e + 8);   // allocated; valid data length
                d->first = le32(e + 20);
                d->got_stream = true;
            } else if (t == 0xC1) {                  // File Name, 15 UTF-16 chars
                for (int c = 0; c < 15 && d->nidx < d->name_len && d->nidx < FS_LNAME - 1; c++)
                    name[d->nidx++] = ascii(le16(e + 2 + 2 * c));
            }
            if (!d->set_left && d->got_stream) {
                name[d->nidx] = 0;
                entry(d, out, d->size, d->first, d->flags & 2, (uint8_t)d->attr);
                return 1;
            }
        } else {
            if (!e[0]) { d->done = true; return 0; }
            if (e[0] == 0xE5) { d->lfn_ok = false; continue; }
            if (e[11] == 0x0F) {                     // long-name piece, last piece first
                uint8_t ord = e[0], seq = ord & 0x1f;
                if (ord & 0x40) { d->lfn_ok = true; d->lfn_sum = e[13]; memset(name, 0, FS_LNAME); }
                else if (!d->lfn_ok || e[13] != d->lfn_sum) { d->lfn_ok = false; continue; }
                if (!seq || seq > 20) { d->lfn_ok = false; continue; }
                static const uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int c = 0; c < 13; c++) {
                    uint16_t ch = le16(e + off[c]);
                    int pos = (seq - 1) * 13 + c;
                    if (ch && ch != 0xffff && pos < FS_LNAME - 1) name[pos] = ascii(ch);
                }
                continue;
            }
            if (e[11] & 0x08 || e[0] == '.') { d->lfn_ok = false; continue; }   // volume label, "." and ".."
            if (!(d->lfn_ok && sfn_sum(e) == d->lfn_sum && name[0])) {
                int n = 0;                           // 8.3 name; byte 12 bits 3/4 = lower-case name/extension
                for (int c = 0; c < 8 && e[c] != ' '; c++) {
                    char ch = (char)(c == 0 && e[0] == 0x05 ? 0xE5 : e[c]);
                    name[n++] = e[12] & 0x08 && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
                }
                if (e[8] != ' ') {
                    name[n++] = '.';
                    for (int c = 8; c < 11 && e[c] != ' '; c++)
                        name[n++] = e[12] & 0x10 && e[c] >= 'A' && e[c] <= 'Z' ? (char)(e[c] + 32) : (char)e[c];
                }
                name[n] = 0;
            }
            name[FS_LNAME - 1] = 0;
            d->lfn_ok = false;
            uint32_t clus = (fs->type == FS_FAT32 ? (uint32_t)le16(e + 20) << 16 : 0) | le16(e + 26);
            entry(d, out, le32(e + 28), clus, false, e[11]);
            if (out->is_dir) out->size = 0;
            return 1;
        }
    }
}

int fs_scan(fs_t *fs, fs_want_fn want, fs_file_t *files, int max) {
    int n = 0;
    fs_file_t e;
    int r = fs_dir_open(fs, &fs->dir, NULL);
    while (r == FS_OK && (r = fs_dir_next(fs, &fs->dir, &e)) > 0) {
        if (!e.is_dir && n < max && want(fs->dir.lname, e.size)) files[n++] = e;
        r = FS_OK;
    }
    return r < 0 ? r : n;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int fs_find(fs_t *fs, const char *path, fs_file_t *e) {
    memset(e, 0, sizeof *e);
    e->is_dir = true;                                // the root
    e->attr = FS_A_DIR;
    for (;;) {
        while (*path == '/') path++;
        if (!*path) return FS_OK;
        size_t n = 0;
        while (path[n] && path[n] != '/') n++;
        if (!e->is_dir) return FS_ENOTDIR;
        int r = fs_dir_open(fs, &fs->dir, e);
        if (r) return r;
        for (;;) {
            r = fs_dir_next(fs, &fs->dir, e);
            if (r <= 0) return r < 0 ? r : FS_ENOFILE;
            const char *l = fs->dir.lname;
            size_t i = 0;
            while (i < n && l[i] && lower((unsigned char)l[i]) == lower((unsigned char)path[i])) i++;
            if (i == n && !l[n]) break;
        }
        path += n;
    }
}

int fs_dir_open_path(fs_t *fs, fs_dir_t *d, const char *path) {
    fs_file_t e;
    int r = fs_find(fs, path, &e);
    return r ? r : fs_dir_open(fs, d, &e);
}

// ---- files --------------------------------------------------------------------
int fs_open(fs_t *fs, const fs_file_t *f) {
    if (f->is_dir) return FS_ENOFILE;
    fs->next = 0;
    fs->cursor = 0;
    fs->nsec = (uint32_t)((f->size + 511) / 512);
    uint32_t nclus = (fs->nsec + (1u << fs->spc_shift) - 1) >> fs->spc_shift;
    if (!nclus || f->size >> 32) return FS_ENOFILE;
    uint32_t c = f->first_clus;
    if (c < 2 || c >= fs->nclus + 2) return FS_ECHAIN;
    if (f->contig) {
        if (c - 2 + nclus > fs->nclus) return FS_ECHAIN;
        fs->ext[0] = (fs_ext_t){0, clus_lba(fs, c), nclus};
        fs->next = 1;
        return FS_OK;
    }
    for (uint32_t i = 0; i < nclus; i++) {
        if (!c) return FS_ECHAIN;
        fs_ext_t *x = fs->next ? &fs->ext[fs->next - 1] : NULL;
        uint32_t lba = clus_lba(fs, c);
        if (x && x->lba + (x->n << fs->spc_shift) == lba) x->n++;
        else {
            if (fs->next == FS_MAX_EXT) return FS_EFRAG;
            fs->ext[fs->next++] = (fs_ext_t){i, lba, 1};
        }
        if (i + 1 < nclus) c = next_clus(fs, c);
    }
    return FS_OK;
}

uint32_t fs_lba(fs_t *fs, uint32_t sec) {
    if (sec >= fs->nsec) return 0;
    uint32_t ci = sec >> fs->spc_shift;
    int k = (int)fs->cursor;
    if (k >= fs->next || fs->ext[k].fclus > ci) k = 0;
    while (k < fs->next && ci >= fs->ext[k].fclus + fs->ext[k].n) k++;
    if (k == fs->next) return 0;
    fs->cursor = (uint32_t)k;
    return fs->ext[k].lba + ((ci - fs->ext[k].fclus) << fs->spc_shift) + (sec & ((1u << fs->spc_shift) - 1));
}

// ---- writing --------------------------------------------------------------------
// For the Game Boy's save files (fsro.h). There is no clock: new entries are
// dated 2026-01-01 12:00.
#define FAT_DATE  ((2026 - 1980) << 9 | 1 << 5 | 1)
#define FAT_TIME  (12 << 11)
#define EX_STAMP  ((uint32_t)FAT_DATE << 16 | FAT_TIME)
#define RUN_MAX   21                       // directory entries a name takes, at most (FAT: 20 + 1)

void fs_set_writer(fs_t *fs, fs_write_fn wr) { fs->write = wr; }

static void st16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, uint32_t v) { st16(p, v); st16(p + 2, v >> 16); }
static void st64(uint8_t *p, uint64_t v) { st32(p, (uint32_t)v); st32(p + 4, (uint32_t)(v >> 32)); }

static bool put(fs_t *fs, uint32_t lba, const uint8_t *buf) {
    bool ok = fs->write && fs->write(lba, buf);
    if (fs->cache_lba == lba) {
        if (ok) memcpy(fs->cache, buf, 512);
        else fs->cache_lba = NONE;
    }
    return ok;
}

int fs_write_sec(fs_t *fs, uint32_t sec, const uint8_t *buf) {
    uint32_t lba = fs_lba(fs, sec);
    if (!lba) return FS_ENOFILE;
    return put(fs, lba, buf) ? FS_OK : FS_EIO;
}

static uint16_t fold(uint16_t c) {         // case, for matching names: ASCII and Latin-1
    return (c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ? (uint16_t)(c - 32) : c;
}

static bool same16(const uint16_t *a, int na, const uint16_t *b, int nb) {
    if (na != nb) return false;
    for (int i = 0; i < na; i++)
        if (fold(a[i]) != fold(b[i])) return false;
    return true;
}

// ---- a directory's raw entries, where each one is
typedef struct { uint32_t lba; uint8_t k; } pos_t;

typedef struct {
    // asked: a name, or (name NULL) the file, or with dirs the folder, whose
    // first cluster is clus; and a run of need free entries
    const uint16_t *name;
    int n, need;
    uint32_t clus;
    bool dirs;
    uint8_t sfn[11];                       // FAT: the short name we'd make ("~" + digit at 6, 7)
    // found
    bool found;
    fs_file_t file;
    uint16_t lname[FS_LNAME];
    int ln;
    uint8_t set[RUN_MAX * 32];             // its entries, raw, and where each one is (exFAT: the
                                           // entry set; FAT: the long-name pieces, the short name)
    pos_t setpos[RUN_MAX];
    int nset;
    uint16_t tails;                        // FAT: digits after "~" taken for sfn's basis
    pos_t run[RUN_MAX];
    int nrun;                              // need: a run; fewer: the free entries the directory ends with
    uint32_t last_clus;                    // the directory's last cluster (0: FAT16's fixed root)
} scan_t;

// the writer's scratch: scans (one for the folder being written, one for a
// look elsewhere while it is: the root's bitmap, a folder's parent), the
// clusters of a chain and their links, a new file's entries. Static (the
// firmware's stack is small), and shared, as none is in use twice at once.
static scan_t w_sc, w_aux;
#define ALLOC_MAX 256                      // clusters a chain written at once (a 128K file in 512-byte clusters)
static uint32_t w_cl[ALLOC_MAX], w_nx[ALLOC_MAX];
static uint8_t w_ents[RUN_MAX * 32];
static uint8_t w_buf[512];

static void file_out(fs_file_t *f, const uint16_t *nm, int n, uint64_t size, uint32_t clus, bool contig, uint8_t attr) {
    memset(f, 0, sizeof *f);
    int i = 0;
    for (; i < n && i < FS_NAME - 1; i++) f->name[i] = ascii(nm[i]);
    f->name[i] = 0;
    f->size = size;
    f->first_clus = clus;
    f->contig = contig;
    f->attr = attr;
    f->is_dir = attr & FS_A_DIR;
}

static bool matches(scan_t *sc, const uint16_t *nm, int n, uint32_t clus, uint8_t attr) {
    if (sc->name) return same16(sc->name, sc->n, nm, n);
    return clus == sc->clus && !(attr & FS_A_DIR) == !sc->dirs;
}

static void found(scan_t *sc, const uint16_t *nm, int n, uint64_t size, uint32_t clus, bool contig, uint8_t attr) {
    sc->found = true;
    file_out(&sc->file, nm, n, size, clus, contig, attr);
    memcpy(sc->lname, nm, (size_t)n * 2);
    sc->ln = n;
}

static void keep(scan_t *sc, const uint8_t *e, uint32_t lba, int k) {
    if (sc->nset == RUN_MAX) return;
    memcpy(sc->set + sc->nset * 32, e, 32);
    sc->setpos[sc->nset++] = (pos_t){lba, (uint8_t)k};
}

// Read dir's entries until sc's name or cluster turns up, noting a run of
// free ones (or the free ones it ends with) on the way. Uses fs->dir.
static int scan(fs_t *fs, const fs_file_t *dir, scan_t *sc) {
    fs_dir_t *d = &fs->dir;
    int r = fs_dir_open(fs, d, dir);
    if (r) return r;
    pos_t tmp[RUN_MAX];
    int cur = 0;
    bool ended = false, lfn_ok = false;
    uint8_t lfn_sum = 0;
    int set_left = 0, nidx = 0, name_len = 0;
    uint16_t attr = 0;
    uint8_t flags = 0;
    uint32_t first = 0;
    uint64_t size = 0;
    bool got_stream = false;
    static uint16_t nm[FS_LNAME];          // the name being put together
    sc->last_clus = 0;
    for (;;) {
        uint32_t lba = dir_sector(fs, d);
        if (!lba) {                        // the end: the free entries it ends with
            if (sc->nrun < sc->need) { memcpy(sc->run, tmp, sizeof tmp[0] * (size_t)cur); sc->nrun = cur; }
            return FS_OK;
        }
        if (!d->fixed) sc->last_clus = d->clus;
        const uint8_t *b = get(fs, lba);
        if (!b) return FS_EIO;
        memcpy(d->sec, b, 512);
        for (int k = 0; k < 16; k++) {
            const uint8_t *e = d->sec + k * 32;
            bool free_;
            if (fs->type == FS_EXFAT) free_ = ended || !(e[0] & 0x80);
            else free_ = ended || !e[0] || e[0] == 0xE5;
            if (free_) {
                if (sc->nrun < sc->need) {
                    tmp[cur++] = (pos_t){lba, (uint8_t)k};
                    if (cur == sc->need) { memcpy(sc->run, tmp, sizeof tmp[0] * (size_t)cur); sc->nrun = cur; }
                }
                if (!e[0]) ended = true;
                lfn_ok = false;
                set_left = 0;
                if (ended && sc->nrun >= sc->need) return FS_OK;
                continue;
            }
            cur = 0;
            if (fs->type == FS_EXFAT) {
                uint8_t t = e[0];
                if (t == 0x81 && !dir->first_clus) { fs->bmp_clus = le32(e + 20); fs->bmp_len = (uint32_t)le64(e + 24); }
                if (t == 0x82 && !dir->first_clus) { fs->up_clus = le32(e + 20); fs->up_len = (uint32_t)le64(e + 24); }
                if (t == 0x85) {
                    set_left = e[1]; attr = le16(e + 4); got_stream = false; nidx = 0; name_len = 0;
                    sc->nset = 0;
                    keep(sc, e, lba, k);
                    continue;
                }
                if (!set_left) continue;
                set_left--;
                keep(sc, e, lba, k);
                if (t == 0xC0) {
                    flags = e[1]; name_len = e[3];
                    size = attr & FS_A_DIR ? le64(e + 24) : le64(e + 8);
                    first = le32(e + 20);
                    got_stream = true;
                } else if (t == 0xC1) {
                    for (int c = 0; c < 15 && nidx < name_len && nidx < FS_LNAME - 1; c++) nm[nidx++] = le16(e + 2 + 2 * c);
                }
                if (!set_left && got_stream && matches(sc, nm, nidx, first, (uint8_t)attr)) {
                    found(sc, nm, nidx, size, first, flags & 2, (uint8_t)attr);
                    return FS_OK;
                }
            } else {
                if (e[11] == 0x0F) {               // a long-name piece, the last first
                    uint8_t ord = e[0], seq = ord & 0x1f;
                    if (ord & 0x40) { lfn_ok = true; lfn_sum = e[13]; name_len = 0; sc->nset = 0; }
                    else if (!lfn_ok || e[13] != lfn_sum) { lfn_ok = false; continue; }
                    if (!seq || seq > 20) { lfn_ok = false; continue; }
                    keep(sc, e, lba, k);
                    static const uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int c = 0; c < 13; c++) {
                        uint16_t ch = le16(e + off[c]);
                        int p = (seq - 1) * 13 + c;
                        if (!ch || ch == 0xFFFF || p >= FS_LNAME - 1) break;
                        nm[p] = ch;
                        if (p + 1 > name_len) name_len = p + 1;
                    }
                    continue;
                }
                if (e[11] & 0x08 || e[0] == '.') { lfn_ok = false; continue; }
                if (!memcmp(e, sc->sfn, 6) && e[6] == '~' && e[7] >= '1' && e[7] <= '9' && !memcmp(e + 8, sc->sfn + 8, 3))
                    sc->tails |= (uint16_t)(1u << (e[7] - '0'));
                int n = name_len;
                if (!(lfn_ok && sfn_sum(e) == lfn_sum && n)) {
                    n = 0;
                    sc->nset = 0;                  // (no long name of its own)
                    for (int c = 0; c < 8 && e[c] != ' '; c++) {
                        uint8_t ch = c == 0 && e[0] == 0x05 ? 0xE5 : e[c];
                        nm[n++] = e[12] & 0x08 && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
                    }
                    if (e[8] != ' ') {
                        nm[n++] = '.';
                        for (int c = 8; c < 11 && e[c] != ' '; c++) nm[n++] = e[12] & 0x10 && e[c] >= 'A' && e[c] <= 'Z' ? e[c] + 32 : e[c];
                    }
                }
                lfn_ok = false;
                keep(sc, e, lba, k);               // the short-name entry, last of the set
                uint32_t clus = (fs->type == FS_FAT32 ? (uint32_t)le16(e + 20) << 16 : 0) | le16(e + 26);
                if (matches(sc, nm, n, clus, e[11])) {
                    found(sc, nm, n, e[11] & FS_A_DIR ? 0 : le32(e + 28), clus, false, e[11]);
                    return FS_OK;
                }
            }
        }
    }
}

// a folder by path; *parent its parent, if parent isn't NULL (the root's: itself)
static int find_dir(fs_t *fs, const char *path, fs_file_t *dir, fs_file_t *parent) {
    int r = fs_find(fs, path, dir);
    if (r) return r;
    if (!dir->is_dir) return FS_ENOTDIR;
    if (!parent) return FS_OK;
    static char up[FS_LNAME];
    size_t n = strlen(path);
    if (n >= sizeof up) return FS_ENAME;
    memcpy(up, path, n + 1);
    while (n && up[n - 1] == '/') up[--n] = 0;     // "/gbc/" -> "/gbc"
    while (n && up[n - 1] != '/') up[--n] = 0;     // -> "/"
    return fs_find(fs, up, parent);
}

int fs_name16(fs_t *fs, const char *dirpath, uint32_t clus, uint16_t *name, int max) {
    scan_t *sc_ = &w_sc;
    #define sc (*sc_)
    memset(&sc, 0, sizeof sc);
    sc.clus = clus;
    memset(sc.sfn, 0xFF, sizeof sc.sfn);
    fs_file_t dir;
    int r = find_dir(fs, dirpath, &dir, NULL);
    if (r || (r = scan(fs, &dir, &sc))) return r;
    if (!sc.found) return FS_ENOFILE;
    int n = sc.ln < max ? sc.ln : max;
    memcpy(name, sc.lname, (size_t)n * 2);
    return n;
    #undef sc
}

// ---- the FAT (exFAT: only for the root and fragmented folders)
static uint32_t fat_eoc(const fs_t *fs) { return fs->type == FS_FAT16 ? 0xFFFF : fs->type == FS_FAT32 ? 0x0FFFFFFF : 0xFFFFFFFF; }

static int fat_get(fs_t *fs, uint32_t c, uint32_t *v) {
    uint32_t off = fs->type == FS_FAT16 ? c * 2 : c * 4;
    const uint8_t *b = get(fs, fs->fat_lba + off / 512);
    if (!b) return FS_EIO;
    *v = fs->type == FS_FAT16 ? le16(b + off % 512) : le32(b + off % 512) & (fs->type == FS_FAT32 ? 0x0FFFFFFF : 0xFFFFFFFF);
    return FS_OK;
}

// entries c[0..n-1] = v[0..n-1], in every copy of the FAT, a sector at a time
static int fat_set(fs_t *fs, const uint32_t *c, const uint32_t *v, int n) {
    uint8_t *buf = w_buf;
    for (int i = 0; i < n;) {
        uint32_t off = fs->type == FS_FAT16 ? c[i] * 2 : c[i] * 4, sec = off / 512;
        for (uint32_t copy = 0; copy < fs->nfats; copy++) {
            uint32_t lba = fs->fat_lba + copy * fs->fat_secs + sec;
            const uint8_t *b = get(fs, lba);
            if (!b) return FS_EIO;
            memcpy(buf, b, 512);
            for (int j = i; j < n; j++) {
                uint32_t o = fs->type == FS_FAT16 ? c[j] * 2 : c[j] * 4;
                if (o / 512 != sec) continue;
                if (fs->type == FS_FAT16) st16(buf + o % 512, v[j]);
                else if (fs->type == FS_FAT32) st32(buf + o % 512, (le32(buf + o % 512) & 0xF0000000u) | (v[j] & 0x0FFFFFFF));
                else st32(buf + o % 512, v[j]);
            }
            if (!put(fs, lba, buf)) return FS_EIO;
        }
        while (i < n && (fs->type == FS_FAT16 ? c[i] * 2 : c[i] * 4) / 512 == sec) i++;
    }
    return FS_OK;
}

// FAT16/32: n free clusters, chained, the last marked as the end: a run if
// the card has one free (first fit from FSInfo's hint), else the first n free
static int fat_alloc(fs_t *fs, uint32_t n, uint32_t *first) {
    uint32_t *cl = w_cl, *nx = w_nx;
    if (!n || n > ALLOC_MAX) return FS_EFULL;
    uint32_t hint = 2, end = fs->nclus + 2;
    static uint8_t fi[512];
    fi[0] = 0;
    if (fs->fsinfo_lba) {
        const uint8_t *b = get(fs, fs->fsinfo_lba);
        if (b && le32(b) == 0x41615252 && le32(b + 484) == 0x61417272) {
            memcpy(fi, b, 512);
            uint32_t h = le32(fi + 492);
            if (h >= 2 && h < end) hint = h;
        }
    }
    uint32_t got = 0, run = 0, start = 0;
    for (uint32_t i = 0; i < fs->nclus && run < n; i++) {
        uint32_t c = hint + i, v;
        if (c >= end) c -= fs->nclus;
        if (c == 2 && i) run = 0;          // a run doesn't wrap
        int r = fat_get(fs, c, &v);
        if (r) return r;
        if (v) { run = 0; continue; }
        if (!run++) start = c;
        if (got < n) cl[got++] = c;
    }
    if (run >= n) for (uint32_t i = 0; i < n; i++) cl[i] = start + i;
    else if (got < n) return FS_EFULL;
    for (uint32_t i = 0; i < n; i++) nx[i] = i + 1 < n ? cl[i + 1] : fat_eoc(fs);
    int r = fat_set(fs, cl, nx, (int)n);
    if (r) return r;
    *first = cl[0];
    if (le32(fi) == 0x41615252) {          // FSInfo: free count and hint, if it keeps them
        uint32_t fc = le32(fi + 488);
        if (fc != 0xFFFFFFFF && fc >= n) st32(fi + 488, fc - n);
        st32(fi + 492, cl[n - 1] + 1 < end ? cl[n - 1] + 1 : 2);
        if (!put(fs, fs->fsinfo_lba, fi)) return FS_EIO;
    }
    return FS_OK;
}

static int zero_clus(fs_t *fs, uint32_t c) {
    static const uint8_t z[512];
    for (uint32_t s = 0; s < 1u << fs->spc_shift; s++)
        if (!put(fs, clus_lba(fs, c) + s, z)) return FS_EIO;
    return FS_OK;
}

// ---- exFAT's allocation bitmap (bit c - 2: cluster c)
// sector s of a FAT-chained system file at clus
static uint32_t chain_lba(fs_t *fs, uint32_t clus, uint32_t s) {
    for (uint32_t i = s >> fs->spc_shift; i; i--)
        if (!(clus = next_clus(fs, clus))) return 0;
    return clus < 2 || clus >= fs->nclus + 2 ? 0 : clus_lba(fs, clus) + (s & ((1u << fs->spc_shift) - 1));
}

// clusters c..c+n-1 marked used (or free)
static int bmp_mark(fs_t *fs, uint32_t c, uint32_t n, bool used) {
    uint8_t *buf = w_buf;
    for (c -= 2; n;) {
        uint32_t sec = c / 4096, lba = chain_lba(fs, fs->bmp_clus, sec);
        const uint8_t *b = lba ? get(fs, lba) : NULL;
        if (!b) return FS_EIO;
        memcpy(buf, b, 512);
        for (; n && c / 4096 == sec; c++, n--)
            if (used) buf[c % 4096 / 8] |= (uint8_t)(1u << (c % 8));
            else buf[c % 4096 / 8] &= (uint8_t)~(1u << (c % 8));
        if (!put(fs, lba, buf)) return FS_EIO;
    }
    return FS_OK;
}

// n free clusters in a run, the first that fits, marked used
static int bmp_alloc(fs_t *fs, uint32_t n, uint32_t *first) {
    uint8_t *sec = w_buf;                  // (bmp_mark's after the search)
    uint32_t run = 0, start = 0;
    for (uint32_t c = 0; c < fs->nclus && run < n;) {
        if (c % 4096 == 0) {
            uint32_t lba = chain_lba(fs, fs->bmp_clus, c / 4096);
            const uint8_t *b = lba ? get(fs, lba) : NULL;
            if (!b) return FS_EIO;
            memcpy(sec, b, 512);
        }
        uint8_t byte = sec[c % 4096 / 8];
        if (c % 8 == 0 && byte == 0xFF) { run = 0; c += 8; continue; }
        if (byte >> (c % 8) & 1) run = 0;
        else if (!run++) start = c;
        c++;
    }
    if (run < n) return FS_EFULL;
    *first = start + 2;
    return bmp_mark(fs, start + 2, n, true);
}

// exFAT: the root's bitmap and up-case entries (fs->bmp_clus, fs->up_clus), once
static int ex_meta(fs_t *fs) {
    if (fs->bmp_clus) return FS_OK;
    scan_t *rs = &w_aux;
    memset(rs, 0, sizeof *rs);
    rs->clus = 0xFFFFFFFF;                 // (matches nothing: the whole root)
    fs_file_t root = {.is_dir = true, .attr = FS_A_DIR};
    int r = scan(fs, &root, rs);
    if (r) return r;
    return fs->bmp_clus ? FS_OK : FS_ENOFS;
}

// the volume's up-case table, for ch < 256 (the name hash wants it)
static int upcase_map(fs_t *fs, uint16_t *map) {
    for (int i = 0; i < 256; i++) map[i] = (uint16_t)i;
    if (!fs->up_clus) return FS_OK;        // none read: identity
    uint32_t idx = 0;
    bool run = false;
    for (uint32_t off = 0; off + 1 < fs->up_len && idx < 256; off += 2) {
        uint32_t lba = chain_lba(fs, fs->up_clus, off / 512);
        const uint8_t *b = lba ? get(fs, lba) : NULL;
        if (!b) return FS_EIO;
        uint16_t v = le16(b + off % 512);
        if (run) { idx += v; run = false; }
        else if (v == 0xFFFF) run = true;
        else map[idx++] = v;
    }
    return FS_OK;
}

static uint16_t set_sum(const uint8_t *ents, int n) {
    uint16_t sum = 0;
    for (int i = 0; i < n * 32; i++)
        if (i != 2 && i != 3) sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + ents[i]);
    return sum;
}

// entries[i] (32 bytes each) into run[i]: forward (FAT: the short-name entry,
// which makes the file, last) or backward (exFAT: the File entry, first in
// the set, last)
static int put_entries(fs_t *fs, const pos_t *run, int n, const uint8_t *ents, bool backward) {
    uint8_t *buf = w_buf;
    for (int s = 0; s < n;) {
        int i = backward ? n - 1 - s : s;
        uint32_t lba = run[i].lba;
        const uint8_t *b = get(fs, lba);
        if (!b) return FS_EIO;
        memcpy(buf, b, 512);
        for (int j = 0; j < n; j++)
            if (run[j].lba == lba) memcpy(buf + run[j].k * 32, ents + j * 32, 32);
        if (!put(fs, lba, buf)) return FS_EIO;
        while (s < n && run[backward ? n - 1 - s : s].lba == lba) s++;
    }
    return FS_OK;
}

// ---- growing a folder by a cluster
// exFAT: a folder other than the root is sized by its entry in its parent
static int grow_exfat_sub(fs_t *fs, const fs_file_t *dir, const fs_file_t *parent, uint32_t last, uint32_t *c) {
    scan_t *ps_ = &w_aux;
    #define ps (*ps_)
    memset(&ps, 0, sizeof ps);
    ps.clus = dir->first_clus;
    ps.dirs = true;
    int r = scan(fs, parent, &ps);
    if (r) return r;
    uint8_t *st = ps.set + 32;
    if (!ps.found || ps.nset < 2 || ps.nset != ps.set[1] + 1 || st[0] != 0xC0) return FS_ECHAIN;
    uint32_t cbytes = 512u << fs->spc_shift, first = le32(st + 20);
    uint64_t len = le64(st + 24);
    uint32_t n = (uint32_t)(len / cbytes);
    if (!n || len % cbytes || n >= ALLOC_MAX || first - 2 + n > fs->nclus) return FS_ECHAIN;
    bool contig = st[1] & 2;               // (a run of clusters becomes a FAT chain)
    if ((r = bmp_alloc(fs, 1, c)) || (r = zero_clus(fs, *c))) return r;
    uint32_t *cl = w_cl, *nx = w_nx;
    int k = 0;
    if (contig)
        for (uint32_t i = 0; i < n; i++) { cl[k] = first + i; nx[k++] = i + 1 < n ? first + i + 1 : *c; }
    else { cl[k] = last; nx[k++] = *c; }
    cl[k] = *c; nx[k++] = 0xFFFFFFFF;
    if ((r = fat_set(fs, cl, nx, k))) return r;
    st[1] &= (uint8_t)~2;                  // NoFatChain off
    st64(st + 8, len + cbytes);            // ValidDataLength, DataLength
    st64(st + 24, len + cbytes);
    st16(ps.set + 2, set_sum(ps.set, ps.nset));
    return put_entries(fs, ps.setpos, ps.nset, ps.set, true);
    #undef ps
}

// a cluster added to the end of dir, zeroed: *lba its first sector
static int grow(fs_t *fs, const fs_file_t *dir, const fs_file_t *parent, uint32_t last, uint32_t *lba) {
    uint32_t c;
    int r;
    if (fs->type != FS_EXFAT) {
        if (!last) return FS_EFULL;        // FAT16's fixed root
        if ((r = fat_alloc(fs, 1, &c)) || (r = zero_clus(fs, c)) || (r = fat_set(fs, &last, &c, 1))) return r;
    } else if (!dir->first_clus) {         // the root: always a FAT chain
        if (!last) return FS_ECHAIN;
        if ((r = bmp_alloc(fs, 1, &c)) || (r = zero_clus(fs, c))) return r;
        uint32_t cl[2] = {c, last}, nx[2] = {0xFFFFFFFF, c};
        if ((r = fat_set(fs, cl, nx, 2))) return r;
    } else if ((r = grow_exfat_sub(fs, dir, parent, last, &c))) return r;
    *lba = clus_lba(fs, c);
    return FS_OK;
}

// room for sc->need entries: the run scan() found, or the free entries the
// folder ends with and a cluster added to it
static int room(fs_t *fs, const fs_file_t *dir, const fs_file_t *parent, scan_t *sc) {
    if (sc->nrun >= sc->need) return FS_OK;
    uint32_t lba;
    int r = grow(fs, dir, parent, sc->last_clus, &lba);
    if (r) return r;
    for (uint32_t i = 0; sc->nrun < sc->need; i++) sc->run[sc->nrun++] = (pos_t){lba + i / 16, (uint8_t)(i % 16)};
    return FS_OK;
}

// ---- new files
static bool sfn_char(uint16_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && c < 128 && strchr("$%'-_@~`!(){}^#&", (int)c));
}

// the short name: two characters of the name, four hex digits of a hash of
// it, "~1" (scan() says which digits are taken), the extension's first three
static void make_sfn(uint8_t *sfn, const uint16_t *name, int n) {
    memset(sfn, ' ', 11);
    int dot = n;
    for (int i = 0; i < n; i++) if (name[i] == '.') dot = i;
    int k = 0;
    for (int i = 0; i < dot && k < 2; i++) {
        uint16_t c = fold(name[i]);
        if (sfn_char(c)) sfn[k++] = (uint8_t)c;
    }
    while (k < 2) sfn[k++] = '_';
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ fold(name[i])) * 16777619u;
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 0; i < 4; i++) sfn[2 + i] = (uint8_t)hx[h >> (28 - 4 * i) & 15];
    sfn[6] = '~';
    sfn[7] = '1';
    k = 8;
    for (int i = dot + 1; i < n && k < 11; i++) {
        uint16_t c = fold(name[i]);
        if (sfn_char(c)) sfn[k++] = (uint8_t)c;
    }
}

static int create_fat(fs_t *fs, const fs_file_t *dir, const fs_file_t *parent, scan_t *sc, uint32_t size, fs_file_t *out) {
    int need = sc->need;
    int d = 1;                             // the short name's tail: the first digit not taken
    while (d <= 9 && sc->tails >> d & 1) d++;
    if (d > 9) return FS_ENAME;
    sc->sfn[7] = (uint8_t)('0' + d);
    uint32_t cbytes = 512u << fs->spc_shift, first;
    int r = room(fs, dir, parent, sc);
    if (r || (r = fat_alloc(fs, (size + cbytes - 1) / cbytes, &first))) return r;
    uint8_t *ents = w_ents;
    memset(ents, 0, sizeof w_ents);
    int nl = need - 1;
    uint8_t sum = sfn_sum(sc->sfn);
    static const uint8_t off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    for (int i = 0; i < nl; i++) {        // entry i holds piece nl - i (the last piece first)
        uint8_t *e = ents + i * 32;
        int seq = nl - i;
        e[0] = (uint8_t)(seq | (i == 0 ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = sum;
        for (int c = 0; c < 13; c++) {
            int p = (seq - 1) * 13 + c;
            st16(e + off[c], p < sc->n ? sc->name[p] : p == sc->n ? 0 : 0xFFFF);
        }
    }
    uint8_t *e = ents + nl * 32;
    memcpy(e, sc->sfn, 11);
    e[11] = 0x20;                          // archive
    st16(e + 14, FAT_TIME); st16(e + 16, FAT_DATE); st16(e + 18, FAT_DATE);
    st16(e + 20, fs->type == FS_FAT32 ? first >> 16 : 0);
    st16(e + 22, FAT_TIME); st16(e + 24, FAT_DATE);
    st16(e + 26, first & 0xFFFF);
    st32(e + 28, size);
    if ((r = put_entries(fs, sc->run, need, ents, false))) return r;
    file_out(out, sc->name, sc->n, size, first, false, 0x20);
    return FS_OK;
}

static int create_exfat(fs_t *fs, const fs_file_t *dir, const fs_file_t *parent, scan_t *sc, uint32_t size, fs_file_t *out) {
    int need = sc->need;
    static uint16_t map[256];
    uint32_t cbytes = 512u << fs->spc_shift, first;
    int r = ex_meta(fs);
    if (r || (r = upcase_map(fs, map)) || (r = room(fs, dir, parent, sc)) || (r = bmp_alloc(fs, (size + cbytes - 1) / cbytes, &first))) return r;
    uint16_t hash = 0;
    for (int i = 0; i < sc->n; i++) {
        uint16_t u = sc->name[i] < 256 ? map[sc->name[i]] : sc->name[i];
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u & 0xFF));
        hash = (uint16_t)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (u >> 8));
    }
    uint8_t *ents = w_ents;
    memset(ents, 0, sizeof w_ents);
    uint8_t *f = ents, *st = ents + 32;
    f[0] = 0x85;
    f[1] = (uint8_t)(need - 1);
    st16(f + 4, 0x20);
    st32(f + 8, EX_STAMP); st32(f + 12, EX_STAMP); st32(f + 16, EX_STAMP);
    st[0] = 0xC0;
    st[1] = 0x03;                          // allocation possible, no FAT chain
    st[3] = (uint8_t)sc->n;
    st16(st + 4, hash);
    st64(st + 8, size);
    st32(st + 20, first);
    st64(st + 24, size);
    for (int i = 2; i < need; i++) {
        uint8_t *e = ents + i * 32;
        e[0] = 0xC1;
        for (int c = 0; c < 15; c++) {
            int p = (i - 2) * 15 + c;
            st16(e + 2 + 2 * c, p < sc->n ? sc->name[p] : 0);
        }
    }
    st16(f + 2, set_sum(ents, need));
    if ((r = put_entries(fs, sc->run, need, ents, true))) return r;
    file_out(out, sc->name, sc->n, size, first, true, 0x20);
    return FS_OK;
}

int fs_create(fs_t *fs, const char *dirpath, const uint16_t *name, int n, uint32_t size, fs_file_t *out, bool *made) {
    *made = false;
    if (n < 1 || n > 255) return FS_ENAME;
    for (int i = 0; i < n; i++)
        if (name[i] < 0x20 || (name[i] < 128 && strchr("\"*/:<>?\\|", name[i]))) return FS_ENAME;
    scan_t *sc_ = &w_sc;
    #define sc (*sc_)
    memset(&sc, 0, sizeof sc);
    sc.name = name;
    sc.n = n;
    sc.need = fs->type == FS_EXFAT ? 2 + (n + 14) / 15 : (n + 12) / 13 + 1;
    if (sc.need > RUN_MAX) return FS_ENAME;
    if (fs->type != FS_EXFAT) make_sfn(sc.sfn, name, n);
    else memset(sc.sfn, 0xFF, sizeof sc.sfn);
    fs_file_t dir, parent;
    int r = find_dir(fs, dirpath, &dir, &parent);
    if (r || (r = scan(fs, &dir, &sc))) return r;
    if (sc.found) {
        if (sc.file.is_dir) return FS_ENAME;  // a folder of that name
        *out = sc.file;
        return FS_OK;
    }
    if (!size) return FS_ENOFILE;
    if (!fs->write) return FS_ENOWRITE;
    r = fs->type == FS_EXFAT ? create_exfat(fs, &dir, &parent, &sc, size, out) : create_fat(fs, &dir, &parent, &sc, size, out);
    if (!r) *made = true;
    return r;
    #undef sc
}

// ---- deleting a file
#ifdef FSRO_TEST
int fs_test_straddles;
#endif
// a file's entries marked deleted, a sector at a time in the set's order:
// FAT: the long-name pieces, then the short-name entry, which makes the
// file; exFAT: the File entry first, after which the rest belong to nothing
static int unmark(fs_t *fs, const pos_t *pos, int n) {
    uint8_t *buf = w_buf;
    for (int s = 0; s < n;) {
        uint32_t lba = pos[s].lba;
        const uint8_t *b = get(fs, lba);
        if (!b) return FS_EIO;
        memcpy(buf, b, 512);
        for (int j = 0; j < n; j++)
            if (pos[j].lba == lba) {
                uint8_t *e = buf + pos[j].k * 32;
                if (fs->type == FS_EXFAT) e[0] &= 0x7F;    // InUse off
                else e[0] = 0xE5;
            }
        if (!put(fs, lba, buf)) return FS_EIO;
        while (s < n && pos[s].lba == lba) s++;
    }
#ifdef FSRO_TEST
    if (n && pos[n - 1].lba != pos[0].lba) fs_test_straddles++;
#endif
    return FS_OK;
}

// the chain from c freed, ALLOC_MAX clusters at a time (each batch's links
// read before any is cleared): exFAT's bitmap bits, then the FAT entries in
// every copy. *n counts them, *low is the lowest.
static int free_chain(fs_t *fs, uint32_t c, uint32_t *n, uint32_t *low) {
    uint32_t *cl = w_cl, *nx = w_nx;
    if (c < 2 || c >= fs->nclus + 2) return FS_OK;
    while (c) {
        int k = 0;
        for (; c && k < ALLOC_MAX; c = next_clus(fs, c)) {
            if (++*n > fs->nclus) return FS_ECHAIN;      // a loop
            cl[k] = c;
            nx[k++] = 0;
            if (c < *low) *low = c;
        }
        int r;
        if (fs->type == FS_EXFAT)
            for (int i = 0, j; i < k; i = j) {          // runs of consecutive clusters
                for (j = i + 1; j < k && cl[j] == cl[j - 1] + 1; j++) {}
                if ((r = bmp_mark(fs, cl[i], (uint32_t)(j - i), false))) return r;
            }
        if ((r = fat_set(fs, cl, nx, k))) return r;
    }
    return FS_OK;
}

// FAT32: FSInfo's free count up by n, and its hint back to the lowest freed
static int fsinfo_freed(fs_t *fs, uint32_t n, uint32_t low) {
    if (!fs->fsinfo_lba || !n) return FS_OK;
    const uint8_t *b = get(fs, fs->fsinfo_lba);
    if (!b) return FS_EIO;
    if (le32(b) != 0x41615252 || le32(b + 484) != 0x61417272) return FS_OK;
    memcpy(w_buf, b, 512);
    uint32_t fc = le32(w_buf + 488), h = le32(w_buf + 492);
    if (fc != 0xFFFFFFFF) st32(w_buf + 488, fc + n > fs->nclus ? fs->nclus : fc + n);
    if (h < 2 || h >= fs->nclus + 2 || low < h) st32(w_buf + 492, low);
    return put(fs, fs->fsinfo_lba, w_buf) ? FS_OK : FS_EIO;
}

int fs_delete(fs_t *fs, const char *dirpath, const uint16_t *name, int n) {
    if (n < 1 || n > 255) return FS_ENAME;
    scan_t *sc_ = &w_sc;
    #define sc (*sc_)
    memset(&sc, 0, sizeof sc);
    sc.name = name;
    sc.n = n;
    memset(sc.sfn, 0xFF, sizeof sc.sfn);
    fs_file_t dir;
    int r = find_dir(fs, dirpath, &dir, NULL);
    if (r || (r = scan(fs, &dir, &sc))) return r;
    if (!sc.found) return FS_ENOFILE;
    if (sc.file.is_dir) return FS_ENAME;   // a folder: not this
    if (!fs->write) return FS_ENOWRITE;
    uint32_t first = sc.file.first_clus, freed = 0, low = NONE;
    uint64_t len = 0;
    bool contig = false;
    if (fs->type == FS_EXFAT) {            // its clusters, from the stream entry
        const uint8_t *st = sc.set + 32;
        if (sc.nset < 2 || sc.nset != sc.set[1] + 1 || st[0] != 0xC0) return FS_ECHAIN;
        contig = st[1] & 2;
        len = le64(st + 24);               // DataLength: what it has, not what was written
        first = le32(st + 20);
        if ((r = ex_meta(fs))) return r;
    } else if (!sc.nset || sc.set[(sc.nset - 1) * 32 + 11] == 0x0F)
        return FS_ECHAIN;                  // (the short-name entry must be in the set: a long run of pieces fills it)
    if ((r = unmark(fs, sc.setpos, sc.nset))) return r;
    if (first < 2 || first >= fs->nclus + 2) return FS_OK;   // no clusters (an empty file)
    if (contig) {                          // exFAT NoFatChain: a run, in the bitmap only
        uint64_t cb = (uint64_t)512 << fs->spc_shift, k = (len + cb - 1) / cb;
        if (first - 2 + k > fs->nclus) return FS_ECHAIN;
        return k ? bmp_mark(fs, first, (uint32_t)k, false) : FS_OK;
    }
    if ((r = free_chain(fs, first, &freed, &low))) return r;
    return fs->type == FS_FAT32 ? fsinfo_freed(fs, freed, low) : FS_OK;
    #undef sc
}

#ifdef FSRO_TEST
// For tests: a file's clusters relinked as a FAT chain in another order
// (every other one, then the rest), which makes a fragmented file; on exFAT
// NoFatChain goes off. Its contents are then in that order: write them after.
int fs_test_scramble(fs_t *fs, const char *dirpath, const uint16_t *name, int n) {
    static uint32_t cl[ALLOC_MAX], ord[ALLOC_MAX], nx[ALLOC_MAX];
    scan_t *sc = &w_sc;
    memset(sc, 0, sizeof *sc);
    sc->name = name;
    sc->n = n;
    memset(sc->sfn, 0xFF, sizeof sc->sfn);
    fs_file_t dir;
    int r = find_dir(fs, dirpath, &dir, NULL);
    if (r || (r = scan(fs, &dir, sc))) return r;
    if (!sc->found || sc->file.is_dir) return FS_ENOFILE;
    uint32_t k = 0, c = sc->file.first_clus;
    uint8_t *st = sc->set + 32;
    if (fs->type == FS_EXFAT && (st[1] & 2)) {
        uint64_t cb = (uint64_t)512 << fs->spc_shift;
        for (uint64_t i = 0; i < (le64(st + 24) + cb - 1) / cb && k < ALLOC_MAX; i++) cl[k++] = c + (uint32_t)i;
    } else
        for (; c && k < ALLOC_MAX; c = next_clus(fs, c)) cl[k++] = c;
    if (k < 3) return FS_EFRAG;
    uint32_t m = 0;
    for (uint32_t i = 0; i < k; i += 2) ord[m++] = cl[i];
    for (uint32_t i = 1; i < k; i += 2) ord[m++] = cl[i];
    for (uint32_t i = 0; i < k; i++) nx[i] = i + 1 < k ? ord[i + 1] : fat_eoc(fs);
    if ((r = fat_set(fs, ord, nx, (int)k))) return r;
    if (fs->type != FS_EXFAT) return FS_OK;
    st[1] &= (uint8_t)~2;
    st16(sc->set + 2, set_sum(sc->set, sc->nset));
    return put_entries(fs, sc->setpos, sc->nset, sc->set, true);
}
#endif
