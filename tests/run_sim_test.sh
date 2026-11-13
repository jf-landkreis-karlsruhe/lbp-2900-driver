#!/bin/sh
# End-to-end test without hardware:
#   document -> Ghostscript (CUPS raster, same settings as the PPD) -> capt backend
#   -> simulated printer (decodes Hi-SCoA) -> pixel comparison with the raster.
#
# usage: [JOBS=2] [PAPER=a5] [CAPT_OPTIONS="capt-geometry=canon"] tests/run_sim_test.sh [BACKEND] [DOCUMENT] [--no-paper-once] [--small-buffer]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(dirname "$HERE")
BACKEND=$(realpath "${1:-$TOP/capt}")
DOC=$(realpath "${2:-$HERE/testpage.ps}")
shift 2 2>/dev/null || shift $#
EXTRA="$*"
PAPER=${PAPER:-a4}
JOBS=${JOBS:-1}
WORK=${WORK:-$(mktemp -d)}
mkdir -p "$WORK/out" "$WORK/tmp"

echo "== rendering $DOC"
PPD="$TOP/ppd/Canon-LBP2900-capt.ppd" TMPDIR="$WORK/tmp" gs -q -dSAFER -dNOPAUSE -dBATCH \
	-sDEVICE=cups -r600 -dcupsColorSpace=3 -dcupsBitsPerColor=1 -dcupsRowCount=70 \
	-sPAPERSIZE=$PAPER -sOutputFile="$WORK/job.ras" "$DOC"

echo "== starting simulator"
cd "$WORK"   # keep the Unix socket path short
rm -f "$WORK/out/"*.pbm
python3 "$HERE/sim_printer.py" sim.sock "$WORK/out" --jobs=$JOBS $EXTRA &
SIM=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S sim.sock ] && break; sleep 0.2; done

for j in $(seq 1 "$JOBS"); do
	echo "== running backend (job $j of $JOBS)"
	DEVICE_URI="capt-sim:sim.sock" CAPT_LOG="$WORK/backend$j.log" \
		"$BACKEND" $j tester "sim test" 1 "${CAPT_OPTIONS:-}" "$WORK/job.ras" 2>"$WORK/backend$j.stderr" || {
		echo "backend failed:"; grep -v '^DEBUG' "$WORK/backend$j.stderr"; wait $SIM || true; exit 1; }
	grep -E '^(PAGE|STATE|WARNING|ERROR)' "$WORK/backend$j.stderr" | sed 's/^/   /'
done
wait $SIM

echo "== comparing"
python3 "$HERE/compare.py" "$WORK/job.ras" "$WORK/out" "$JOBS"
echo "(work dir: $WORK)"
