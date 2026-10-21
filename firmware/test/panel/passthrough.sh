#!/bin/bash
# On the real panel: does its decoder draw partial and transparent GIF
# frames right? (QUESTIONS.md, question 2.) Turns passthrough_partial on,
# sends each test GIF as it is and says what to look for; restores the
# setting at the end.
#
#   firmware/test/panel/passthrough.sh [http://xyaoled.local]
#
# Before each GIF a solid blue still is sent, so anything left over from
# the previous image shows through transparent pixels as blue. The GIFs
# are made by `make -C firmware/test/host fixtures`; open them in a
# browser to see what they should look like.
set -euo pipefail
BASE=${1:-http://xyaoled.local}
FIX="$(dirname "$0")/../host/fixtures"

config() { curl -fsS -H 'Content-Type: application/json' -d "$1" "$BASE/api/config" >/dev/null; }
upload() { curl -fsS -F "file=@$FIX/$1" "$BASE/upload"; echo; }
step() {
	echo
	echo "== $1"
	upload pt_solid.png
	read -rp "   The panel is all blue? [enter] "
	upload "$1"
	echo "   Expected: $2"
	read -rp "   What it shows (blank = as expected): " seen
	results+=("$1: ${seen:-as expected}")
}

trap 'config "{\"passthrough_partial\": false}"' EXIT
config '{"passthrough": true, "passthrough_partial": true}'
results=()

step pt_transparent.gif "left half black (not blue), right half switching green/red every second"
step pt_partial.gif "yellow, a red square steps right leaving a red trail, restarts yellow"
step pt_dispose.gif "yellow, a red square steps right; each spot it leaves goes black (blue = old image shows through; a red trail = disposal ignored)"

echo
echo "Results (paste into QUESTIONS.md):"
printf '  %s\n' "${results[@]}"
echo "The serial log should say \"passthrough: ... partial frames\" / \"transparency\" for each GIF."
