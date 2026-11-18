// vxcard.c -- see vxcard.h.
#include "vxcard.h"

#include <string.h>

#define MAX_FILES 96                // 8K files of the folder looked at

static uint8_t sec[512];            // (not on the stack: the plugin's is 2K)

static const struct { uint32_t crc; const char *name; } known[] = {
    {0xBA13FB57, "exec rom"},                   // MAME's exec_rom.bin
    {0x6D2BD167, "exec rom intl"},              // exec_rom_intl_284001-1.bin
    {0xA6E4DAC4, "fast boot"},                  // the two quick-start hacks MAME's driver lists
    {0x71DCF0F4, "fast boot intl"},
};

uint32_t vxcard_crc32(const uint8_t *d, uint32_t n) {
    uint32_t crc = ~0u;
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}

const char *vxcard_system_rom(const uint8_t *d, uint32_t n) {
    if (n != VX_ROM_SIZE) return NULL;
    uint32_t crc = vxcard_crc32(d, n);
    for (unsigned i = 0; i < sizeof known / sizeof known[0]; i++)
        if (known[i].crc == crc) return known[i].name;
    return NULL;
}

int32_t vxcard_read(fs_t *fs, const fs_file_t *f, uint8_t *dst, uint32_t max, void (*tick)(void)) {
    if (f->is_dir || f->size > max || fs_open(fs, f) != FS_OK) return -1;
    uint32_t n = (uint32_t)f->size;
    for (uint32_t at = 0; at < n; at += 512) {
        uint32_t lba = fs_lba(fs, at / 512);
        if (!lba || !fs->read(lba, sec)) return -1;
        memcpy(dst + at, sec, n - at < 512 ? n - at : 512);
        if (tick) tick();
    }
    return (int32_t)n;
}

static bool named(const char *name, const char *want) {
    for (; *name && *want; name++, want++) {
        char c = *name;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != *want) return false;
    }
    return !*name && !*want;
}

const char *vxcard_load_rom(fs_t *fs, void (*tick)(void)) {
    static fs_dir_t dir;                // (too big for the stack)
    static fs_file_t files[MAX_FILES];
    int n = 0, own = -1;
    fs_file_t e;
    if (fs_dir_open_path(fs, &dir, VXCARD_HOME) != FS_OK) return NULL;
    while (n < MAX_FILES && fs_dir_next(fs, &dir, &e) == 1) {   // the listing first: reading a file moves fsro's map
        if (e.is_dir || e.size != VX_ROM_SIZE || dir.lname[0] == '.') continue;
        if (named(dir.lname, "EXEC_ROM.BIN") || named(dir.lname, "SYSTEM.BIN")) own = n;
        files[n++] = e;
    }
    // (a file with one of those names first: with a folder of games it saves reading every 8K one)
    for (int k = -1; k < n; k++) {
        int i = k < 0 ? own : k;
        if (i < 0 || (k >= 0 && i == own)) continue;
        if (vxcard_read(fs, &files[i], vx_rom(), VX_ROM_SIZE, tick) != VX_ROM_SIZE) continue;
        const char *name = vxcard_system_rom(vx_rom(), VX_ROM_SIZE);
        if (name) return name;
    }
    if (own >= 0 && vxcard_read(fs, &files[own], vx_rom(), VX_ROM_SIZE, tick) == VX_ROM_SIZE) return "your own";
    return NULL;
}
