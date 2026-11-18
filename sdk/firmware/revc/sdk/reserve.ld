/* reserve.ld -- added to a plugin's link by cvcart_plugin() (an implicit
 * linker script): its RAM must stop below what the switch between firmwares
 * keeps in SRAM (cvlaunch.h: the launch block at 0x2007EC00, the handoff
 * trace above it). crt0 zeroes .bss before main, so a .bss reaching it would
 * wipe the launch block (the file to open) before the plugin could read it.
 * A plugin's stacks are in scratch X and Y; cvcart_plugin() refuses malloc,
 * whose heap would start at .bss's end and grow into the block. */
ASSERT(__bss_end__ <= 0x2007EC00, "the plugin's .bss reaches the switch's reserved RAM at 0x2007EC00 (sdk/cvlaunch.h)")
