// cvxip: flash reads at full speed for an XIP plugin (cvcart_plugin ... XIP).
//
// The menu starts an XIP plugin with the bootrom's chain_image, which maps
// the plugin's flash slot at 0x10000000 but leaves the flash in the slow
// serial read (03h): a cache miss streams at about 1.5 MB/s. cvxip_fast()
// switches it to quad I/O reads (EBh) at clk_sys / 2, about 28 MB/s a miss
// at 150 MHz (xiptest/), keeping the slot's mapping. If the slot doesn't
// read back the same afterwards, the serial read comes back.
//
// Call it first thing in main(): before cvf_start() (core 1 must not be
// serving yet) and before cvpsram_init() (it flushes the XIP cache, which
// would lose PSRAM writes, and the bootrom call sets up the QMI's second
// chip select too).
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// true when the flash reads quad now
bool cvxip_fast(void);

#ifdef __cplusplus
}
#endif
