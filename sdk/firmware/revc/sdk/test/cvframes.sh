#!/bin/sh
# The bus loop of a cvframes plugin against the Game Boy kernel's bus cycles
# (cvframes_sim.py has what is run and checked).
#   test/cvframes.sh NEW.elf [OLD.elf]
# NEW.elf: a plugin built with cvframes_serve2() (cvfserve.S). It must serve
# every read right on the plain bus, at both paces of publishing, and with
# the pins disturbed as the console's captures show them (there a byte may
# be a few ns late by the simulator's high cycle counts, as the games' loop
# is in cvserve_sim.py --bounce; never wrong, and no select missed or false).
# OLD.elf (optional): one built without. With the pins disturbed it must
# hand the Z80 wrong bytes: the fault cvfserve.S is for.
set -e
here=$(cd "$(dirname "$0")" && pwd)
new=${1:?a plugin ELF built with cvframes_serve2()}
python3 -I "$here/cvframes_sim.py" "$new" --frames 4 --seeds 2 --quiet
python3 -I "$here/cvframes_sim.py" "$new" --frames 4 --seeds 1 --publish-us 9000 --quiet
python3 -I "$here/cvframes_sim.py" "$new" --frames 4 --seeds 2 --bounce --quiet
python3 -I "$here/cvframes_sim.py" "$new" --frames 4 --seeds 1 --bounce --hit 0.05 --quiet
[ -n "$2" ] && python3 -I "$here/cvframes_sim.py" "$2" --frames 4 --seeds 1 --bounce --hit 0.05 --expect-bad --quiet
echo "cvframes.sh: ok"
