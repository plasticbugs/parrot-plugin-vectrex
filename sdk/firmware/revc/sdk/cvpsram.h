// cvpsram.h -- the cart's PSRAM (U6, APS6404L, 8 MB on QMI chip select 1,
// GP47) for a plugin: reset, ID, QPI at clk_sys / 2, then memory-mapped
// through the XIP cache, as the menu's Game Boy mode uses it (sdplayer.c).
//
// A no_flash plugin runs from SRAM, so the 16K XIP cache serves the PSRAM
// alone. Write through CVPSRAM_UNCACHED (32-bit words) and then
// xip_cache_invalidate_all(), or write through CVPSRAM_CACHED and
// xip_cache_clean_all() before reading it uncached.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "hardware/regs/addressmap.h"

#define CVPSRAM_SIZE     0x00800000u
#define CVPSRAM_CACHED   ((uint8_t *)(XIP_BASE + 0x01000000u))
#define CVPSRAM_UNCACHED ((volatile uint32_t *)(XIP_NOCACHE_NOALLOC_BASE + 0x01000000u))

// true if the PSRAM answered with its ID and a pattern read back
bool cvpsram_init(void);
// its ID bytes (MF, KGD, EID x6), as read
const uint8_t *cvpsram_id(void);
