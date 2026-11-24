#!/bin/sh
# test/run.sh -- the SDK's checks that run on the Mac:
#   1. cvusb_test: the USB readers (keyboards, mice, pads, MIDI both ways,
#      a MIDI device's descriptors) and cvinput.h's player 1
#   2. test_cvreadme.py: the README converter, rule by rule
# The parts that need the cart (TinyUSB, the timer, the port's choice) are
# tried on the console with ../../featuretest. Output in $TMPDIR/sdk-test.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
out=${TMPDIR:-/tmp}/sdk-test
mkdir -p "$out"
cd "$here"
cc -O2 -std=c11 -Wall -Wextra -Wno-missing-field-initializers -fsanitize=address,undefined -I. -DCVUSB_PADS=1 \
    test/cvusb_test.c cvusb_keys.c cvusb_midi.c hidpad.c swpro.c -o "$out/cvusb_test"
"$out/cvusb_test"
python3 -I test/test_cvreadme.py
echo "sdk: all checks passed"
