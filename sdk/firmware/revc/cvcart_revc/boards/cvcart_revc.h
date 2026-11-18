// ColecoVision streaming cart, rev C: chip-down RP2350B (QFN-80), W25Q128
// 16MB flash on QMI CS0, APS6404L 8MB PSRAM on QMI CS1 (GPIO47), 12MHz
// ABM8-272-T3 crystal -- the Raspberry Pi minimal-design circuit.
#ifndef _BOARDS_CVCART_REVC_H
#define _BOARDS_CVCART_REVC_H

// pico_cmake_set PICO_PLATFORM=rp2350

#define PICO_RP2350A 0                     // the 80-pin B part: GPIO 0..47
#define PICO_FLASH_SPI_CLKDIV 2
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

// cut the crystal start-up wait (SDK default is 6x): every millisecond before
// the bus loop runs is a millisecond the console's BIOS may read the cart
// header too early (it reads at ~4.3 ms after the console's 5V appears)
#define PICO_XOSC_STARTUP_DELAY_MULTIPLIER 1

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25            // D3 (red) via R17
#endif

#endif
