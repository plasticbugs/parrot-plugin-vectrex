# Vectrex

Vectrex games on the ColecoVision. It opens `.vec` and `.gam` files from anywhere on the card; `/VECTREX` is its home folder.

## Setting it up

1. `VECTREX.CVP` into the card's `/PLUGINS` folder.
2. The Vectrex's system ROM into `/VECTREX`. It is one 8K file, often named `exec_rom.bin` or `Mine Storm (1982).vec`. The name doesn't matter. Without it nothing runs, and the screen says so.
3. Games anywhere on the card, named `.vec` or `.gam`. Rename a game that ends in `.bin`.

Started from the Plugins page with no game, it plays Mine Storm, the game inside the system ROM.

## Controls

On the ColecoVision controller:

- Joystick: the stick
- Keypad 1, 2, 3, 4: buttons 1 to 4
- Left fire: button 3
- Right fire: button 4
- Keypad 0: the picture's colour
- RESET on the console: back to the menu

On a USB pad:

- Left stick or D-pad: the stick
- Left face button (Nintendo Y): button 1
- Top face button (Nintendo X): button 2
- Bottom face button (Nintendo B): button 3
- Right face button (Nintendo A): button 4
- L: the picture's colour

## Notes

- USB pads need a powered hub on the cart's USB port. A Switch Pro Controller or a plain HID pad works. The first pad is player 1, with the console's controller. The second is player 2. Player 2 needs a USB pad.
- The picture stands upright in the middle of the screen, as the Vectrex's does. It is much coarser than the real one. Small text is redrawn a little larger so it can be read.
- The picture's colour stands in for a game's overlay: white, blue, green, yellow, red or magenta.
- Three voices and noise, as the Vectrex has. Its buzzing sounds become plain tones. Speech and samples play, a little rough.
- No 3-D Imager and no light pen. Games that need them don't work.
- Nothing is saved.

## Credits

- The Vectrex's VIA, beam and cartridge follow MAME's Vectrex driver by Mathis Rosenhauer and others. BSD-3-Clause. Source:
  https://github.com/mamedev/mame
- The 6809 was written for the cart's Star Wars plugin and checked against MAME's.
- MAME's licence text is in the plugin's source.
- TinyUSB by Ha Thach. MIT. Source:
  https://github.com/hathach/tinyusb
- The plugin itself is by Parrot Cart, and it's free. Its source is linked next to its download.
- It contains no Vectrex ROM. The system ROM and the games are yours to supply.
