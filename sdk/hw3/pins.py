"""The cart's pin order: which RP2350 GPIO carries each console address and
data line. It is the only part of the board the firmware and the plugin SDK
build from (the kernels' images and the bus code are arranged by it), so it
is what the public SDK carries. netlist.py, the whole circuit, imports it
from here.
"""

# ADDR_GPIO[g] = cartridge address line on GPIO g (g = 0..14): fingers left->right
# map to the chip's left side read from its top-left corner downwards.
ADDR_GPIO = [0, 1, 2, 11, 3, 10, 4, 13, 14, 5, 6, 12, 7, 9, 8]
# DATA_GPIO[k] = console data bit carried by GPIO 16+k (through the data buffer)
DATA_GPIO = [2, 1, 3, 0, 4, 5, 6, 7]
assert sorted(ADDR_GPIO) == list(range(15)) and sorted(DATA_GPIO) == list(range(8))
