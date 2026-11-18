// cvsd.h -- the microSD card for a plugin: SPI0 on the cart's card slot, the
// same code as the menu's (sdplayer.c), and the file the menu launched it
// with.
//
//   cvsd_init()     the pins and the card (CMD0 / 8 / 41 / 58), 25 MHz after
//   cvsd_resume()   the card carried on with, if the firmware before left it started
//   cvsd_read()     one 512-byte sector, three tries; fs_mount(fs, cvsd_read)
//   cvsd_write()    one sector (CMD24), waiting out the card's programming
//   cvsd_open_launched()   mount the card and open the launch block's file
//
// Pins (rev C): MISO GP32, CS GP33, SCK GP34, MOSI GP35, card detect GP36
// (low = a card in, pulled up).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cvlaunch.h"
#include "../sdplayer/fsro.h"

#define CVSD_MISO   32
#define CVSD_CS     33
#define CVSD_SCK    34
#define CVSD_MOSI   35
#define CVSD_DET    36

typedef struct {
    bool hc;                    // SDHC/SDXC: block addresses
    bool resumed;               // cvsd_resume() took it over, already started
    uint32_t mb, khz;           // its size, and the SPI clock it got
    char name[6];               // CID's product name
    uint32_t rd_sectors, rd_errors, wr_sectors, wr_errors;
} cvsd_info_t;

bool cvsd_card_in(void);        // the detect switch (after cvsd_init's pin setup)
bool cvsd_init(void);
// The card as the firmware before this one left it (the menu, which chained
// here; or a plugin, which rebooted into the menu): still started and in SPI
// mode, so its start (CMD0, then ACMD41 until it is ready: 0.1 to 1 s) can be
// left out. True if it answers as a started card does; false (a card put in
// since, or none): cvsd_init() then.
bool cvsd_resume(void);
bool cvsd_read(uint32_t lba, uint8_t *buf);
bool cvsd_write(uint32_t lba, const uint8_t *buf);
const cvsd_info_t *cvsd_info(void);

// The launch block's file as fsro has it, for fs_open(): its name (the
// path's last part), size, first cluster and layout. (Portable: the Mac
// tests use it as the plugins do.)
static inline void cvsd_launched_file(const cvl_t *b, fs_file_t *f) {
    memset(f, 0, sizeof *f);
    const char *slash = strrchr(b->path, '/');
    strncpy(f->name, slash ? slash + 1 : b->path, sizeof f->name - 1);
    f->size = b->file_size;
    f->first_clus = b->file_clus;
    f->contig = b->file_contig;
}

// The launch block's file (b->path non-empty): the card mounted on fs (with
// writes if writable), then the file opened by its first cluster, size and
// layout, so its sectors are fs_lba(fs, k) and its name needs no lookup
// (*f gets it). FS_OK, an FS_* error, or FS_ENOFILE for a block with no
// file. Calls cvsd_init() first.
int cvsd_open_launched(fs_t *fs, const cvl_t *b, bool writable, fs_file_t *f);
