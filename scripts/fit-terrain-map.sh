#!/usr/bin/env bash
# Regenerates the fitted terrain map's table (src/plugin/ui/terrain_fit_table.cpp), its held-out report and the
# evaluator's cross-check sample, from rendered stances of the real engine. The build never runs this.
#
#   scripts/fit-terrain-map.sh [work-dir]      (default: .scratch/fitmap/data)
#
# Needs a build with -DFEATHERS_BUILD_TOOLS=ON (ninja -C build fit-terrain-map). The stances are drawn from a fixed
# seed, so the same tree regenerates the same files byte for byte; the rendered CSVs are kept in the work dir and a
# second run reuses them (delete them to render again).
#
# 12000 stances per engine, 4000 each at travel ranges +-1, +-4 and +-24, rendered one fixed note (MIDI 48) for 0.5 s.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tool="$here/build/fit-terrain-map"
work="${1:-$here/.scratch/fitmap/data}"
count="${FEATHERS_FIT_COUNT:-4000}"
hold=0.5
mkdir -p "$work"
[ -x "$tool" ] || { echo "build $tool first" >&2; exit 1; }

i=0
for range in 1 4 24; do
	i=$((i + 1))
	for engine in additive; do
		out="$work/${engine}_r${range}.csv"
		if [ ! -s "$out" ]; then
			# one seed per (engine, range): the stream of a range never depends on another's
			"$tool" gen "$engine" "$count" "$((20261100 + i))" "$hold" "$range" "$out.part" && mv "$out.part" "$out"
		fi
	done
done

flags=()
[ "${FEATHERS_FIT_CALIBRATE_RATIOS:-0}" = 1 ] && flags+=(--calibrate-ratios)
"$tool" fit "$here/src/plugin/ui/terrain_fit_table.cpp" "$here/docs/terrain-fit-report.txt" \
	"$here/tests/fixtures/terrain_fit_sample.inc" \
	--additive "$work"/additive_r1.csv "$work"/additive_r4.csv "$work"/additive_r24.csv "${flags[@]}"
