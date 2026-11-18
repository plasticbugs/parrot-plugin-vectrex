// vxcard.h -- the plugin's files on the card, through fsro: the system ROM
// from /VECTREX, and the cartridge the menu opened. Portable: the plugin and
// the Mac test (test/card.sh, on disk images) share it.
//
// The system ROM is known by its size (8K) and CRC-32, whatever its name,
// as the Star Wars plugin knows its ROMs: MAME's exec_rom.bin, which
// GoodVect has as "Mine Storm (1982).vec", or one of the other three MAME
// lists. Failing those, an 8K file named EXEC_ROM.BIN or SYSTEM.BIN is taken
// as it is (a ROM of one's own).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "../sdplayer/fsro.h"
#include "vx.h"

#define VXCARD_HOME "/VECTREX"

uint32_t vxcard_crc32(const uint8_t *d, uint32_t n);
// are these 8K a system ROM we know? (its name for the note, or NULL)
const char *vxcard_system_rom(const uint8_t *d, uint32_t n);

// a file's bytes into dst (at most max): its size, or -1 (too big, or a read
// failed). tick: called between sectors (the plugin serves the console), or NULL.
int32_t vxcard_read(fs_t *fs, const fs_file_t *f, uint8_t *dst, uint32_t max, void (*tick)(void));

// the system ROM out of VXCARD_HOME into vx_rom(): its name, or NULL if
// there is none there
const char *vxcard_load_rom(fs_t *fs, void (*tick)(void));
