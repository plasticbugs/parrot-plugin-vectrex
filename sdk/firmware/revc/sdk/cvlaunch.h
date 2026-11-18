// cvlaunch.h -- the launch and return blocks: what the menu (sdplayer) tells
// a plugin it starts, and what a plugin tells the menu when it goes back.
//
// 1K of SRAM at CVL_ADDR, which no firmware's .bss reaches (sdplayer's link
// checks: reserve.ld), so it survives the bootrom's chain_image and a
// watchdog reboot. A cold boot leaves garbage there: a block counts only with
// its magic, its size and its CRC right.
//
//   the menu, starting a plugin   CVL_LAUNCH: the plugin's ID, the file to
//                                 open (or none), and where the menu was
//   the plugin, going back        CVL_RETURN: the same block, with a note
//                                 for the menu's bottom line (or none)
//
// The menu reads it once, at boot, and clears it. A plugin that reboots into
// the menu without writing CVL_RETURN leaves CVL_LAUNCH there, which brings
// the menu back where it was all the same.
//
// The file: its path as the menu shows it ('?' for a character outside
// ASCII, so fs_find() may not find it), and its first cluster, size and
// layout, which open it directly (cvsd_open_launched(), cvsd.h).
//
// Portable C, header-only: the firmware, the plugins and the host tests use it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CVL_ADDR    0x2007EC00u
#define CVL_SIZE    0x400u
#define CVL_LAUNCH  0x314C5643u            // "CVL1"
#define CVL_RETURN  0x31525643u            // "CVR1"
#define CVL_API     1

enum { CVL_BACK_CARD, CVL_BACK_HOME, CVL_BACK_PLUGINS };   // where the menu comes back to

typedef struct {
    uint32_t magic, api, size;             // size: sizeof(cvl_t)
    uint32_t seq;                          // a new one each launch
    char plugin[33];                       // the plugin's ID ("parrotcart.psgsynth")
    uint8_t file_contig;                   // exFAT NoFatChain: the file's clusters are consecutive
    uint8_t back;                          // CVL_BACK_*
    uint8_t back_row;                      // the row the bar was on
    uint32_t back_index;                   // CVL_BACK_CARD: the entry (in the folder: page * 19 + row)
    uint64_t file_size;
    uint32_t file_clus;                    // its first cluster (0: an empty file)
    char path[256];                        // the file to open, "/NES/Mega Man 2.nes"; "" for none
    char back_dir[256];                    // CVL_BACK_CARD: the folder ("" the root)
    char note[33];                         // CVL_RETURN: the menu's bottom line, once ("" for none)
    uint8_t pad[3];
    uint32_t crc;                          // CRC-32 of all before it
} cvl_t;
_Static_assert(sizeof(cvl_t) <= CVL_SIZE, "the block fits its 1K");
_Static_assert(offsetof(cvl_t, crc) % 4 == 0, "the CRC is a word");

static inline uint32_t cvl_crc(const void *p, size_t n) {   // CRC-32 (IEEE), bit by bit: 1K once a launch
    const uint8_t *b = (const uint8_t *)p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static inline void cvl_seal(cvl_t *b) {    // the size, then the CRC
    b->size = sizeof *b;
    b->crc = cvl_crc(b, offsetof(cvl_t, crc));
}

static inline bool cvl_ok(const cvl_t *b) {
    return (b->magic == CVL_LAUNCH || b->magic == CVL_RETURN) && b->size == sizeof *b &&
           b->crc == cvl_crc(b, offsetof(cvl_t, crc)) && !b->plugin[32] && !b->path[255] && !b->back_dir[255] &&
           !b->note[32];
}

// a plugin, going back: the block it was launched with, now a return, with a
// note for the menu (NULL: none). Then the plugin reboots into the menu.
static inline void cvl_return(cvl_t *b, const char *note) {
    b->magic = CVL_RETURN;
    memset(b->note, 0, sizeof b->note);
    if (note) strncpy(b->note, note, sizeof b->note - 1);
    cvl_seal(b);
}

// On the cart: the block itself (a host test has its own)
#ifndef CVL_BLOCK
#define CVL_BLOCK ((cvl_t *)CVL_ADDR)
#endif
