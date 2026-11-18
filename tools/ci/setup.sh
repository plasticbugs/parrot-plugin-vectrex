#!/bin/sh
# tools/ci/setup.sh -- the build's tools on a fresh Ubuntu runner, each at a
# fixed version (the workflows run it). Leaves the pico-sdk in
# $HOME/pico-sdk, and the Arm toolchain on the PATH.
set -eu
PICO_SDK_COMMIT=bddd20f928ce76142793bef434d4f75f4af6e433    # pico-sdk 2.1.1
PICOTOOL_COMMIT=2041936441b48a3cc53ae3da9e805229fe8f4e18    # picotool 2.3.1
PICOTOOL_SDK_COMMIT=079c6f39023649b154152db30f1d781e884879bc  # pico-sdk 2.3.1, only to build picotool
SJASMPLUS_COMMIT=69569f353fd1de38a6fb474ce9d543d87566f75a   # sjasmplus v1.23.1.1
# The compiler the cart's plugins are built and tested with: Arm GNU
# Toolchain 9-2019-q4-major (GCC 9.2.1). Ubuntu's own is newer, and makes
# a different .CVP.
ARM_GCC=gcc-arm-none-eabi-9-2019-q4-major
ARM_GCC_URL=https://developer.arm.com/-/media/Files/downloads/gnu-rm/9-2019q4/$ARM_GCC-x86_64-linux.tar.bz2
ARM_GCC_SHA256=bcd840f839d5bf49279638e9f67890b2ef3a7c9c7a9b25271e83ec4ff41d177a

sudo apt-get update -q
sudo apt-get install -y -q --no-install-recommends cmake ninja-build python3 build-essential git ca-certificates \
    curl bzip2 libusb-1.0-0-dev pkg-config zlib1g-dev

tmp=${RUNNER_TEMP:-/tmp}
curl -fsSL -o "$tmp/$ARM_GCC.tar.bz2" "$ARM_GCC_URL"
echo "$ARM_GCC_SHA256  $tmp/$ARM_GCC.tar.bz2" | sha256sum -c -
sudo tar -xjf "$tmp/$ARM_GCC.tar.bz2" -C /opt
export PATH="/opt/$ARM_GCC/bin:$PATH"
if [ -n "${GITHUB_PATH:-}" ]; then echo "/opt/$ARM_GCC/bin" >> "$GITHUB_PATH"; fi

# a repository at one commit, without its history
fetch() {
    git init -q "$2"
    git -C "$2" fetch -q --depth 1 "$1" "$3"
    git -C "$2" checkout -q FETCH_HEAD
}

fetch https://github.com/raspberrypi/pico-sdk "$HOME/pico-sdk" "$PICO_SDK_COMMIT"

# picotool 2.3.1, installed where CMake finds it (left to itself, pico-sdk
# 2.1.1 builds picotool's moving `develop` branch). Each plugin build runs its
# `seal`, and picotool 2.1.1 seals a plugin into a different image: 512 KB,
# with the scratch code left at the top of SRAM. It is built as Homebrew
# builds it, against pico-sdk 2.3.1 and that SDK's mbedtls (picotool has no
# `seal` without mbedtls). The plugin itself still builds with pico-sdk 2.1.1.
fetch https://github.com/raspberrypi/pico-sdk "$HOME/picotool-sdk" "$PICOTOOL_SDK_COMMIT"
fetch https://github.com/Mbed-TLS/mbedtls "$HOME/picotool-sdk/lib/mbedtls" \
    "$(git -C "$HOME/picotool-sdk" ls-tree HEAD lib/mbedtls | awk '{print $3}')"
fetch https://github.com/raspberrypi/picotool "$HOME/picotool" "$PICOTOOL_COMMIT"
cmake -S "$HOME/picotool" -B "$HOME/picotool/build" -G Ninja -DPICO_SDK_PATH="$HOME/picotool-sdk" -DCMAKE_BUILD_TYPE=Release
ninja -C "$HOME/picotool/build"
sudo cmake --install "$HOME/picotool/build"

fetch https://github.com/z00m128/sjasmplus "$HOME/sjasmplus" "$SJASMPLUS_COMMIT"
git -C "$HOME/sjasmplus" submodule update -q --init --depth 1
make -C "$HOME/sjasmplus" -j"$(nproc)"
sudo make -C "$HOME/sjasmplus" install

arm-none-eabi-gcc --version | head -1
picotool version
sjasmplus --version | head -1
