// cvpsram.c -- see cvpsram.h. sdplayer.c's psram_probe, psram_timing and
// psram_setup (from ../selftest and ../gbbench).
#include "cvpsram.h"

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/xip_cache.h"

#define PSRAM_CS 47

static uint8_t id[8];

const uint8_t *cvpsram_id(void) { return id; }

static inline __attribute__((always_inline)) uint8_t qmi_xfer(uint8_t b, bool quad) {
    qmi_hw->direct_tx = (quad ? QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB : 0) | b;
    while (!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)) tight_loop_contents();
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
    return (uint8_t)qmi_hw->direct_rx;
}
static inline __attribute__((always_inline)) void cs1(bool on) {
    if (on) qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    else    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
}

// reset the PSRAM, read its ID over plain SPI, then switch it to QPI
static void __no_inline_not_in_flash_func(probe)(void) {
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;  // 5 MHz
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) tight_loop_contents();
    cs1(true); qmi_xfer(0xF5, true); cs1(false);         // leave QPI, in case it is in it
    cs1(true); qmi_xfer(0x66, false); cs1(false);        // reset enable
    cs1(true); qmi_xfer(0x99, false); cs1(false);        // reset
    for (volatile int i = 0; i < 2000; i++) ;
    cs1(true);
    qmi_xfer(0x9F, false);                               // read ID: cmd, 3 address bytes,
    qmi_xfer(0, false); qmi_xfer(0, false); qmi_xfer(0, false);
    for (int i = 0; i < 8; i++) id[i] = qmi_xfer(0xFF, false);   // then MF, KGD, EID x6
    cs1(false);
    if (id[1] == 0x5D) { cs1(true); qmi_xfer(0x35, false); cs1(false); }   // enter QPI
    qmi_hw->direct_csr = 0;
    restore_interrupts(irq);
}

// QPI read 0xEB (6 wait clocks) and write 0x38 at clk_sys / 2.
// APS6404L: CS low <= 8 us, CS high >= 18 ns
static void __no_inline_not_in_flash_func(timing)(void) {
    const uint divisor = 2;
    uint32_t sys = clock_get_hz(clk_sys);
    uint rxdelay = divisor + (sys / divisor > 100000000 ? 1 : 0);
    uint64_t period_fs = 1000000000000000ull / sys;
    uint max_select = (uint)(125000000ull / period_fs);          // 8 us in units of 64 clocks
    int min_deselect = (int)((18000000ull + period_fs - 1) / period_fs) - (int)((divisor + 1) / 2);
    if (min_deselect < 0) min_deselect = 0;
    const uint32_t quad = QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                          QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                          QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                          QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                          QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                          QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB;
    uint32_t irq = save_and_disable_interrupts();
    qmi_hw->m[1].timing = 1u << QMI_M1_TIMING_COOLDOWN_LSB |
                          QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
                          max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
                          (uint)min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
                          rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
                          divisor << QMI_M1_TIMING_CLKDIV_LSB;
    qmi_hw->m[1].rfmt = quad | QMI_M1_RFMT_DUMMY_LEN_VALUE_24 << QMI_M1_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xEB;
    qmi_hw->m[1].wfmt = quad;
    qmi_hw->m[1].wcmd = 0x38;
    restore_interrupts(irq);
}

bool cvpsram_init(void) {
    gpio_set_function(PSRAM_CS, GPIO_FUNC_XIP_CS1);
    probe();
    if (id[1] != 0x5D) return false;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
    timing();
    // a pattern at both ends, written past the cache and read back through it
    static const uint32_t at[] = {0, 1, 0x1FFFFE, 0x1FFFFF};
    for (int i = 0; i < 4; i++) CVPSRAM_UNCACHED[at[i]] = 0x5A0F3C00u ^ at[i] * 0x9E3779B1u;
    xip_cache_invalidate_all();
    for (int i = 0; i < 4; i++)
        if (((const volatile uint32_t *)CVPSRAM_CACHED)[at[i]] != (0x5A0F3C00u ^ at[i] * 0x9E3779B1u)) return false;
    return true;
}
