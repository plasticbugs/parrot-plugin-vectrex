// fsro -- minimal FAT16 / FAT32 / exFAT on the first usable partition (MBR,
// GPT, or none): list any directory, find a file by path, and map a file's
// sectors to card LBAs.  512-byte sectors only (every SD card).  Portable C:
// the firmware and the host test both build it.
//
// Writing (the Game Boy's save files, the menu's plugin manager): overwrite
// a file's sectors in place, make a new file in a folder (fs_create), which
// adds a cluster to the folder when it is full, and delete a file
// (fs_delete). Nothing is renamed, and a file never changes size.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define FS_NAME      64                    // stored name, truncated (matching uses the full name)
#define FS_LNAME     256                   // a full name, with its 0
#define FS_MAX_EXT   1024                  // fragments per open file (12 KB)

enum { FS_FAT16 = 16, FS_FAT32 = 32, FS_EXFAT = 64 };
enum { FS_OK = 0, FS_EIO = -1, FS_ENOFS = -2, FS_ENOFILE = -3, FS_ECHAIN = -4, FS_EFRAG = -5, FS_ENOTDIR = -6,
       FS_EFULL = -7, FS_ENAME = -8, FS_ENOWRITE = -9 };
enum { FS_A_RDONLY = 0x01, FS_A_HIDDEN = 0x02, FS_A_SYSTEM = 0x04, FS_A_DIR = 0x10 };   // fs_file_t.attr

typedef bool (*fs_read_fn)(uint32_t lba, uint8_t *buf);           // one 512-byte sector
typedef bool (*fs_write_fn)(uint32_t lba, const uint8_t *buf);    // one 512-byte sector
typedef bool (*fs_want_fn)(const char *name, uint64_t size);       // keep this root-directory file?

// A directory entry. The root is {is_dir, first_clus 0}.
typedef struct {
    char name[FS_NAME];
    uint64_t size;                         // a directory: its allocated bytes on exFAT, 0 on FAT
    uint32_t first_clus;
    bool contig;                           // exFAT NoFatChain: clusters are consecutive
    bool is_dir;
    uint8_t attr;                          // FS_A_*
} fs_file_t;

// A directory being listed. Each holds its own place and sector, so listings
// can interleave with reads; ~800 bytes, too big for a small stack.
typedef struct {
    uint32_t clus, lba, left, count, clus_left;
    bool fixed, started, contig, done;
    int k;                                 // the next entry in sec
    uint8_t sec[512];
    bool lfn_ok;                           // FAT: a long name being put together
    uint8_t lfn_sum;
    int set_left, nidx, name_len;          // exFAT: an entry set being put together
    bool got_stream;
    uint16_t attr;
    uint8_t flags;
    uint32_t first;
    uint64_t size;
    char lname[FS_LNAME];                  // the last entry's full name
} fs_dir_t;

typedef struct { uint32_t fclus, lba, n; } fs_ext_t;  // file cluster index, its LBA, clusters

typedef struct {
    fs_read_fn read;
    fs_write_fn write;                     // NULL: read-only (fs_set_writer)
    int type;
    uint32_t nfats, fat_secs, fsinfo_lba;  // FAT copies and their size; FAT32's FSInfo (0: none)
    uint32_t bmp_clus, bmp_len, up_clus, up_len;   // exFAT: allocation bitmap, up-case table (bytes)
    uint32_t part_lba, fat_lba, heap_lba;  // heap_lba = LBA of cluster 2
    uint32_t root_lba, root_secs;          // FAT16's fixed root directory
    uint32_t root_clus, nclus, spc_shift;  // log2 sectors per cluster
    uint32_t cache_lba;
    uint8_t cache[512];
    fs_dir_t dir;                          // fs_scan's and fs_find's
    // the open file
    fs_ext_t ext[FS_MAX_EXT];
    int next;
    uint32_t nsec, cursor;
} fs_t;

int fs_mount(fs_t *fs, fs_read_fn rd);
// the root directory's files that want() keeps -> files[], up to max, in
// directory order: how many, or < 0 an error
int fs_scan(fs_t *fs, fs_want_fn want, fs_file_t *files, int max);
int fs_open(fs_t *fs, const fs_file_t *f); // map a file's clusters

// Listing a directory: every entry but "." and "..", in directory order,
// hidden and system ones too (attr says). fs_dir_next: 1 = *e is the next
// entry (its full name in d->lname), 0 = the end, < 0 an error.
int fs_dir_open(fs_t *fs, fs_dir_t *d, const fs_file_t *dir);   // dir NULL: the root
int fs_dir_next(fs_t *fs, fs_dir_t *d, fs_file_t *e);
// A path: "/NES/Mario.nes", "NES/Mario.nes", "/" (the root). Names match in
// any case (ASCII); a character outside ASCII reads as '?'. Uses fs->dir.
int fs_find(fs_t *fs, const char *path, fs_file_t *e);
int fs_dir_open_path(fs_t *fs, fs_dir_t *d, const char *path);
uint32_t fs_lba(fs_t *fs, uint32_t sec);   // file sector -> LBA; 0 = past the end
const char *fs_type_name(const fs_t *fs);

// ---- writing ---------------------------------------------------------------------
void fs_set_writer(fs_t *fs, fs_write_fn wr);
// Names here are UTF-16 (n units, no 0), matched in any case (ASCII and
// Latin-1); folders are paths, as for fs_find. fs_name16: the full name of
// the file in dirpath whose first cluster is clus (a file from a listing):
// its length, or < 0. Both use fs->dir.
int fs_name16(fs_t *fs, const char *dirpath, uint32_t clus, uint16_t *name, int max);
// The file `name` in dirpath. If it is there, *out is it and *made false. If
// not and size > 0, a file of `size` bytes is made (its bytes are whatever
// the card held: write them all), *made true; size 0: FS_ENOFILE. Its
// clusters are contiguous where the card has a run free (exFAT: only then).
// A full folder gets a cluster more (FAT16's root can't). The FAT, the
// bitmap and FSInfo are written before the directory entries, so a write
// cut short leaves at worst clusters marked used that no file owns.
int fs_create(fs_t *fs, const char *dirpath, const uint16_t *name, int n, uint32_t size, fs_file_t *out, bool *made);
// Delete the file `name` (matched as fs_create matches) in dirpath: FS_ENOFILE
// if it isn't there, FS_ENAME if it's a folder. Its directory entries are
// marked deleted first (FAT: 0xE5 in the short-name entry and every long-name
// piece; exFAT: InUse off in each entry of its set), then its clusters freed
// (FAT: the chain, in every FAT copy, and FSInfo's free count and hint;
// exFAT: the bitmap, and the FAT chain if it has one). So a write cut short
// leaves at worst clusters marked used that no file owns, never an entry
// whose clusters are free. fs_create reuses both: a run of deleted entries
// that fits, and the first free clusters (FAT32: from FSInfo's hint). Uses
// fs->dir. If it was the file fs_open() mapped, don't write through that map
// after: it isn't told.
int fs_delete(fs_t *fs, const char *dirpath, const uint16_t *name, int n);
// sector sec of the file fs_open() opened: write buf there
int fs_write_sec(fs_t *fs, uint32_t sec, const uint8_t *buf);

#ifdef FSRO_TEST
extern int fs_test_straddles;              // deletes whose entries spanned two sectors
int fs_test_scramble(fs_t *fs, const char *dirpath, const uint16_t *name, int n);
#endif
