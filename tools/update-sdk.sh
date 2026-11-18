#!/bin/sh
# tools/update-sdk.sh -- refresh sdk/ from a checkout of the cart's repository.
#
#   tools/update-sdk.sh ../parrot-cart                 its HEAD
#   tools/update-sdk.sh ../parrot-cart v1.2            a tag, branch or commit
#
# sdk/ is a copy of the files a plugin's build needs, at the paths they have
# in the cart's repository (so its CMake and its #includes work unchanged):
# the plugin SDK itself, the two Z80 kernels, the file system, the packer and
# the board definition. Only committed files are copied (git archive), and
# sdk/SOURCE.txt records the commit they came from. Don't edit files under
# sdk/: change them upstream and run this again.
set -e
src=${1:?usage: tools/update-sdk.sh COLECO_CHECKOUT [REF]}
ref=${2:-HEAD}
here=$(cd "$(dirname "$0")/.." && pwd)
commit=$(git -C "$src" rev-parse --verify "$ref^{commit}")

# what a plugin's build reads (sdk/README.md has the table)
paths="
firmware/revc/sdk
firmware/revc/cvcart_revc/boards
firmware/revc/bin2h_revc.py
firmware/revc/midihello/midihello.asm
firmware/revc/midihello/handoff.asm
firmware/revc/midihello/mailbox.h
firmware/revc/midihello/vdp.c
firmware/revc/midihello/vdp.h
firmware/revc/midihello/font8.h
firmware/revc/midihello/pad.c
firmware/revc/midihello/pad.h
firmware/revc/midihello/tinyusb_fix/hub.c
firmware/revc/sdplayer/fsro.c
firmware/revc/sdplayer/fsro.h
firmware/revc/sdplayer/htrace.h
firmware/revc/sdplayer/perm.h
firmware/revc/sdplayer/bin2c.py
firmware/revc/sdplayer/mkcvp.py
gbcv/kernel/gbkernel.asm
hw3/pins.py
"
rm -rf "$here/sdk/firmware" "$here/sdk/gbcv" "$here/sdk/hw3"
mkdir -p "$here/sdk"
# shellcheck disable=SC2086
git -C "$src" archive "$commit" -- $paths | tar -x -C "$here/sdk"
{
    echo "commit $commit"
    echo "date   $(git -C "$src" show -s --format=%cI "$commit")"
    echo "about  $(git -C "$src" show -s --format=%s "$commit")"
    echo "copied $(date -u +%Y-%m-%dT%H:%M:%SZ) by tools/update-sdk.sh"
} > "$here/sdk/SOURCE.txt"
echo "sdk/ is now $(git -C "$src" rev-parse --short "$commit"): $(find "$here/sdk" -type f | wc -l | tr -d ' ') files"
