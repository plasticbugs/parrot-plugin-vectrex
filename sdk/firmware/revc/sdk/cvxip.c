// cvxip.c: see cvxip.h. Ported from xiptest/xiphello.c's set_mode.
#include "cvxip.h"

#include "pico/bootrom.h"
#include "hardware/structs/qmi.h"
#include "hardware/sync.h"

#define CHECK_WORDS 1024                    // the slot's first 4K

// the slot's first 4K as the flash holds it, past the translation; ATRANS0
// maps 0x10000000 onwards from its base (4K units, bits 11:0)
static uint32_t __no_inline_not_in_flash_func(slot_sum)(uint32_t at0) {
    const volatile uint32_t *p =
        (const volatile uint32_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + ((at0 & 0xFFFu) << 12));
    uint32_t s = 0;
    for (int i = 0; i < CHECK_WORDS; i++) s += p[i] ^ (uint32_t)i;
    return s;
}

// From SRAM with interrupts off: nothing may come from flash meanwhile. The
// bootrom call may reset the translation, so it's put back.
bool __no_inline_not_in_flash_func(cvxip_fast)(void) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t at[8], timing = qmi_hw->m[0].timing;
    for (int i = 0; i < 8; i++) at[i] = qmi_hw->atrans[i];
    const uint32_t ref = slot_sum(at[0]);
    rom_flash_select_xip_read_mode(BOOTROM_XIP_MODE_EBH_QUAD, 2);
    for (int i = 0; i < 8; i++) qmi_hw->atrans[i] = at[i];
    rom_flash_flush_cache();
    const bool ok = slot_sum(at[0]) == ref;
    if (!ok) {
        rom_flash_exit_xip();
        rom_flash_enter_cmd_xip();
        qmi_hw->m[0].timing = timing;
        for (int i = 0; i < 8; i++) qmi_hw->atrans[i] = at[i];
        rom_flash_flush_cache();
    }
    restore_interrupts(irq);
    return ok;
}
