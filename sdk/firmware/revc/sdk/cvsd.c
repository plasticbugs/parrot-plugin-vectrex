// cvsd.c -- see cvsd.h. sdplayer.c's microSD code, as a library.
#include "cvsd.h"

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/watchdog.h"

static cvsd_info_t info;

static uint8_t sd_x(uint8_t b) {
    uint8_t r;
    spi_write_read_blocking(spi0, &b, &r, 1);
    return r;
}

static void sd_end(void) { gpio_put(CVSD_CS, 1); sd_x(0xFF); }

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg) {
    gpio_put(CVSD_CS, 1); sd_x(0xFF); gpio_put(CVSD_CS, 0);
    for (int i = 0; i < 5000 && sd_x(0xFF) != 0xFF; i++) ;   // wait while the card is busy
    sd_x(0x40 | cmd);
    sd_x(arg >> 24); sd_x(arg >> 16); sd_x(arg >> 8); sd_x(arg);
    sd_x(cmd == 0 ? 0x95 : cmd == 8 ? 0x87 : 0x01);
    uint8_t r = 0xFF;
    for (int i = 0; i < 10 && ((r = sd_x(0xFF)) & 0x80); i++) ;
    return r;
}

static bool sd_data(uint8_t *buf, uint n) {               // data token, n bytes, CRC
    uint8_t t;
    uint32_t t0 = time_us_32();
    do t = sd_x(0xFF); while (t == 0xFF && time_us_32() - t0 < 200000);
    if (t != 0xFE) return false;
    spi_read_blocking(spi0, 0xFF, buf, n);
    sd_x(0xFF); sd_x(0xFF);
    return true;
}

static bool pins_done;

static void pins(void) {
    if (pins_done) return;
    pins_done = true;
    gpio_init(CVSD_DET);
    gpio_pull_up(CVSD_DET);
    spi_init(spi0, 400000);
    gpio_set_function(CVSD_MISO, GPIO_FUNC_SPI);
    gpio_set_function(CVSD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(CVSD_MOSI, GPIO_FUNC_SPI);
    gpio_init(CVSD_CS);
    gpio_set_dir(CVSD_CS, GPIO_OUT);
    gpio_put(CVSD_CS, 1);
}

bool cvsd_card_in(void) {
    pins();
    return !gpio_get(CVSD_DET);
}

// CSD and CID: the card's size and name
static void sd_about(void) {
    uint8_t csd[16], cid[16];
    info.mb = 0;
    if (sd_cmd(9, 0) == 0 && sd_data(csd, 16)) {
        if (csd[0] >> 6 == 1) info.mb = (((uint32_t)(csd[7] & 0x3F) << 16 | csd[8] << 8 | csd[9]) + 1) / 2;
        else {
            uint32_t c_size = (uint32_t)(csd[6] & 3) << 10 | csd[7] << 2 | csd[8] >> 6;
            uint32_t mult = (csd[9] & 3) << 1 | csd[10] >> 7;
            info.mb = (uint32_t)(((uint64_t)(c_size + 1) << (mult + 2) << (csd[5] & 0xF)) >> 20);
        }
    }
    sd_end();
    memset(info.name, 0, sizeof info.name);
    if (sd_cmd(10, 0) == 0 && sd_data(cid, 16))
        for (int i = 0; i < 5; i++) info.name[i] = cid[3 + i] >= 32 && cid[3 + i] < 127 ? (char)cid[3 + i] : '.';
    sd_end();
}

// A started card answers CMD58 (read OCR) with R1 = 0 and its power-up bit
// set; one that was put in since is not in SPI mode and doesn't answer, and
// one that was reset answers "idle" (1).
bool cvsd_resume(void) {
    pins();
    spi_set_baudrate(spi0, 400000);
    gpio_put(CVSD_CS, 1);
    sd_x(0xFF);
    uint8_t b[4], r = sd_cmd(58, 0);
    for (int i = 0; i < 4; i++) b[i] = sd_x(0xFF);
    sd_end();
    if (r != 0x00 || !(b[0] & 0x80) || (b[1] == 0xFF && b[2] == 0xFF && b[3] == 0xFF)) return false;
    info.hc = (b[0] & 0x40) != 0;
    info.khz = spi_set_baudrate(spi0, 25000000) / 1000;
    sd_about();
    if (!info.mb) return false;            // (its CSD didn't read: start it properly)
    info.resumed = true;
    return true;
}

bool cvsd_init(void) {
    pins();
    info.resumed = false;
    spi_set_baudrate(spi0, 400000);
    gpio_put(CVSD_CS, 1);
    for (int i = 0; i < 10; i++) sd_x(0xFF);              // 80+ clocks with CS high
    uint8_t r = 0xFF;
    for (int i = 0; i < 3 && r != 0x01; i++) { r = sd_cmd(0, 0); sd_end(); }
    if (r != 0x01) return false;
    uint8_t b[4];
    r = sd_cmd(8, 0x1AA);
    for (int i = 0; i < 4; i++) b[i] = sd_x(0xFF);
    sd_end();
    bool v2 = r == 0x01 && (b[2] & 0x0F) == 0x01 && b[3] == 0xAA;
    uint32_t t0 = time_us_32();
    do {
        sd_cmd(55, 0); sd_end();
        r = sd_cmd(41, v2 ? 1u << 30 : 0); sd_end();
    } while (r == 0x01 && time_us_32() - t0 < 1500000);
    if (r != 0x00) return false;
    r = sd_cmd(58, 0);
    for (int i = 0; i < 4; i++) b[i] = sd_x(0xFF);
    sd_end();
    info.hc = r == 0 && (b[0] & 0x40);
    if (!info.hc) { sd_cmd(16, 512); sd_end(); }
    sd_about();
    info.khz = spi_set_baudrate(spi0, 25000000) / 1000;
    return true;
}

bool cvsd_read(uint32_t lba, uint8_t *buf) {
    for (int tries = 0; tries < 3; tries++) {
        bool ok = sd_cmd(17, info.hc ? lba : lba * 512) == 0 && sd_data(buf, 512);
        sd_end();
        if (ok) { info.rd_sectors++; return true; }
        info.rd_errors++;
    }
    return false;
}

// The card takes the block, then holds MISO low while it programs it (up
// to 250 ms for SDHC, 500 for SDXC); a slow card's wait isn't a lock-up,
// so it feeds the watchdog if one runs.
bool cvsd_write(uint32_t lba, const uint8_t *buf) {
    for (int tries = 0; tries < 3; tries++) {
        bool ok = false;
        if (sd_cmd(24, info.hc ? lba : lba * 512) == 0) {
            sd_x(0xFF);                                    // a byte's gap
            sd_x(0xFE);                                    // start block
            spi_write_blocking(spi0, buf, 512);
            sd_x(0xFF); sd_x(0xFF);                        // CRC (not checked in SPI mode)
            if ((sd_x(0xFF) & 0x1F) == 0x05) {             // data accepted
                uint32_t t0 = time_us_32();
                while (!(ok = sd_x(0xFF) == 0xFF) && time_us_32() - t0 < 600000)
                    if (watchdog_hw->ctrl & WATCHDOG_CTRL_ENABLE_BITS) watchdog_update();
            }
        }
        sd_end();
        if (ok) { info.wr_sectors++; return true; }
        info.wr_errors++;
    }
    return false;
}

const cvsd_info_t *cvsd_info(void) { return &info; }

int cvsd_open_launched(fs_t *fs, const cvl_t *b, bool writable, fs_file_t *f) {
    cvsd_launched_file(b, f);
    if (!b->path[0]) return FS_ENOFILE;
    if (!cvsd_card_in() || !cvsd_init()) return FS_EIO;
    int r = fs_mount(fs, cvsd_read);
    if (r != FS_OK) return r;
    if (writable) fs_set_writer(fs, cvsd_write);
    return fs_open(fs, f);
}
