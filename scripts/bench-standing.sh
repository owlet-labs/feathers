#!/bin/sh
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
# the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License for more details.
#
# You should have received a copy of the GNU General Public License along with Feathers. If not, see
# <https://www.gnu.org/licenses/>.
#
# THE STANDING BENCHMARK SET (backlog OR). Runs the four standing plans through tests/bench_patch and either
# RECORDS the per-row medians as the baseline or COMPARES a fresh run against it and flags what moved.
#
#   scripts/bench-standing.sh record  [-r ROUNDS] [-p PLANS] [-b FILE]     write the baseline (default tests/bench_patch_plans/baseline.csv)
#   scripts/bench-standing.sh compare [-r ROUNDS] [-p PLANS] [-b FILE] [-t PCT] [-f US] [-o FILE]
#   scripts/bench-standing.sh show    [-b FILE]                            print the baseline's header
#
#   -r  rounds per plan, each round runs every line once, round-robin (default 5)
#   -p  comma list of plans: devices,engine,mod,idle (default all four; one plan alone fits the 10-minute tool bound),
#       and resonate (backlog PS: the factory presets that carry a Resonate, with and without it; not in the default)
#   -t  flag threshold in percent over baseline (default 40: see the perf-survey README for where it comes from)
#   -f  and a flagged row must also have moved by at least this many microseconds per block (default 60)
#   -o  also write the fresh run in the baseline's own format (to diff or to promote)
#   env: BENCH=path/to/bench_patch (default build/tests/bench_patch), CORE=15 (taskset; CORE= to not pin)
#
# Exit status: 0 recorded / compared clean, 1 a row was flagged, 2 a usage or run failure.
# Run from the repo root, after building bench_patch in Release. Records the load beside the numbers: this host
# is shared and thread CPU moves with it, so a flag is a place to look
# with the plan alone and more rounds, not a verdict.

set -u

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here" || exit 2
mode=${1:-}
[ $# -gt 0 ] && shift
rounds=5
plans=devices,engine,mod,idle
baseline=tests/bench_patch_plans/baseline.csv
thresh=40
floor=60
out=
input=
while getopts r:p:b:t:f:o:i: opt; do
	case $opt in
	r) rounds=$OPTARG ;;
	p) plans=$OPTARG ;;
	b) baseline=$OPTARG ;;
	t) thresh=$OPTARG ;;
	f) floor=$OPTARG ;;
	o) out=$OPTARG ;;
	i) input=$OPTARG ;;
	*) exit 2 ;;
	esac
done
bench=${BENCH:-build/tests/bench_patch}
core=${CORE-15}

if [ "$mode" = show ]; then
	grep '^#' "$baseline"
	exit 0
fi
if [ "$mode" != record ] && [ "$mode" != compare ]; then
	echo "usage: $0 record|compare|show [-r ROUNDS] [-p devices,engine,mod,idle] [-b FILE] [-t PCT] [-f US] [-o FILE]" >&2
	exit 2
fi
[ -x "$bench" ] || { echo "bench-standing: $bench is not built (ninja -C build bench_patch)" >&2; exit 2; }

scratch=${TMPDIR:-/tmp}/bench-standing.$$
mkdir -p "$scratch/home" || exit 2
trap 'rm -rf "$scratch"' EXIT
fresh=$scratch/fresh.csv
: >"$fresh"
load0=$(cut -d' ' -f1-3 /proc/loadavg)

pin=
if [ -n "$core" ] && command -v taskset >/dev/null 2>&1; then
	pin="taskset -c $core"
fi

if [ -n "$input" ]; then # compare a run recorded earlier (-o) instead of running: re-reading a pass under another threshold
	grep -v '^#' "$input" >"$fresh" || exit 2
	plans=$(awk -F, '!seen[$1]++ { printf "%s%s", (n++ ? "," : ""), $1 }' "$fresh")
fi
for plan in $(test -z "$input" && echo "$plans" | tr ',' ' '); do
	file=tests/bench_patch_plans/standing-$plan.plan
	[ -f "$file" ] || { echo "bench-standing: no plan $file" >&2; exit 2; }
	echo "bench-standing: $plan x $rounds rounds (load $(cut -d' ' -f1-3 /proc/loadavg))" >&2
	# The per-configuration summary is on stderr; the per-run rows on stdout are not needed here.
	HOME=$scratch/home $pin "$bench" --plan "$file" --rounds "$rounds" 2>"$scratch/err.$plan" >"$scratch/out.$plan" || {
		echo "bench-standing: bench_patch failed on $plan:" >&2
		grep -v '^\[feathers\]' "$scratch/err.$plan" | tail -5 >&2
		exit 2
	}
	# THE IDLE PHASE'S SLEEP: first_sleep_block is column 20 of the per-run rows; -1 in every round means the plugin never returned SLEEP with no notes.
	awk -F, -v p="$plan" 'NR > 1 && $3 == "idle" && $20 != 0 { bad[$2] = 1 } END { for (l in bad) print p "/" l }' "$scratch/out.$plan" >>"$scratch/awake.raw"
	# plan,row,n,cpu_mean_med,cpu_mean_min,cpu_mean_max,cpu_p99_med,allocs_max,locks_max (columns 1..3,4,5,6,7,13,14 of the summary)
	grep -E '^[A-Za-z0-9_]+/(idle|held|tail),' "$scratch/err.$plan" |
		awk -F, -v p="$plan" '{ printf "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", p, $1, $2, $3, $4, $5, $6, $13, $14 }' >>"$fresh"
done
[ -f "$scratch/awake.raw" ] || : >"$scratch/awake.raw"
awake=$(sort "$scratch/awake.raw" | tr '\n' ' ' | sed 's/ $//')
load1=$(cut -d" " -f1-3 /proc/loadavg)
rows=$(wc -l <"$fresh")
[ "$rows" -gt 0 ] || { echo "bench-standing: no summary rows came back" >&2; exit 2; }

header() {
	echo "# bench-standing baseline (backlog OR). plan,row,n,cpu_mean_med_us,cpu_mean_min_us,cpu_mean_max_us,cpu_p99_med_us,allocs_max,locks_max"
	echo "# recorded $(date +%Y-%m-%d) on $(hostname), $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ //')"
	echo "# commit $(git rev-parse --short HEAD 2>/dev/null) ($(git rev-parse --abbrev-ref HEAD 2>/dev/null)), last record: plans $plans, $rounds rounds (a subset record keeps the other plans' rows), pinned to core ${core:-none}"
	echo "# loadavg (1/5/15 min) before $load0, after $load1"
	echo "# idle never sleeps (rows whose no-notes phase did not return CLAP_PROCESS_SLEEP at block 0): ${awake:-none}"
	echo "# the number is the MEDIAN over rounds of each run's mean thread-CPU microseconds per 256-frame block (5333 us = 100% of real time at 48 kHz)"
}

# The sleep entries of the baseline, `plan/row` tokens: those of the plans in this run (in) or of the others (out).
sleepers() {
	grep '^# idle never sleeps' "$baseline" 2>/dev/null | sed 's/^[^:]*: //; s/^none$//' | tr ' ' '\n' |
		awk -F/ -v P=",$plans," -v W="$1" 'NF && ((index(P, "," $1 ",") > 0) == (W == "in"))'
}

if [ "$mode" = record ]; then
	# A SUBSET RECORD REPLACES ONLY ITS OWN PLANS' ROWS and keeps the rest (one plan fits the tool bound; all three do not).
	keep=$scratch/keep.csv
	: >"$keep"
	if [ -f "$baseline" ]; then
		grep -v '^#' "$baseline" | awk -F, -v P=",$plans," 'index(P, "," $1 ",") == 0' >"$keep"
		awake=$({ sleepers out; echo "$awake" | tr ' ' '\n'; } | awk 'NF' | sort | tr '\n' ' ' | sed 's/ $//')
	fi
	planlines=$scratch/planlines
	: >"$planlines"
	if [ -f "$baseline" ]; then
		grep '^# plan ' "$baseline" | awk -v P=",$plans," '{ split($3, a, ":"); if (index(P, "," a[1] ",") == 0) print }' >"$planlines"
	fi
	for p in $(echo "$plans" | tr ',' ' '); do
		echo "# plan $p: $(date +%Y-%m-%d) commit $(git rev-parse --short HEAD 2>/dev/null), $rounds rounds, loadavg before $load0, after $load1" >>"$planlines"
	done
	{ header; sort "$planlines"; cat "$keep" "$fresh"; } >"$baseline.new" && mv "$baseline.new" "$baseline"
	echo "bench-standing: recorded $rows rows to $baseline" >&2
	exit 0
fi

[ -f "$baseline" ] || { echo "bench-standing: no baseline $baseline (run: $0 record)" >&2; exit 2; }
if [ -n "$out" ]; then
	{ header; cat "$fresh"; } >"$out"
fi

# THE COMPARISON. The host's clock moves thread CPU by 1.5x to 2.5x between two passes minutes apart at the same loadavg
# (MEASURED 2026-10-29: the whole of the devices plan read 1.5-2.5x its baseline), so an absolute ratio is the host's. What holds
# is a row against the REST OF ITS OWN PLAN in the same pass: HOST is the median, over the plan's rows that cost at least 100 us
# in the baseline, of fresh/baseline. A row's REL is its own ratio divided by HOST, and the expected cost is baseline x HOST.
# A row is FLAGGED when REL is over 1 + THRESH percent AND the fresh median is at least FLOOR microseconds over the
# expected cost (so a near-zero row cannot flag on noise) AND the fresh MIN is over the expected min by half the threshold
# (a load spike lifts the median of a few rounds; the min only moves when the work did).
# Allocation or lock counts above zero on the audio thread are flagged whatever the timing.
echo "bench-standing: load before $load0, after $load1; threshold ${thresh}% and ${floor} us; baseline $baseline" >&2
grep '^# recorded\|^# commit\|^# loadavg' "$baseline" | sed 's/^# /baseline: /' >&2
awk -F, -v T="$thresh" -v F="$floor" '
	FNR == NR { if ($0 ~ /^#/) next; k = $1 "," $2; bmed[k] = $4; bmin[k] = $5; next }
	{
		k = $1 "," $2
		order[++n] = k; plan_of[k] = $1; fmed[k] = $4; fmin[k] = $5; fal[k] = $8; flk[k] = $9
		if (k in bmed && bmed[k] >= 100) { c[$1]++; v[$1, c[$1]] = $4 / bmed[k] }
	}
	END {
		for (p in c) {
			m = c[p]
			for (i = 2; i <= m; i++) { x = v[p, i]; j = i - 1; while (j >= 1 && v[p, j] > x) { v[p, j + 1] = v[p, j]; j-- } v[p, j + 1] = x }
			host[p] = (m % 2) ? v[p, (m + 1) / 2] : (v[p, m / 2] + v[p, m / 2 + 1]) / 2
			printf "host factor, plan %s: x%.2f (median fresh/baseline over %d rows)\n", p, host[p], m
		}
		flagged = 0; missing = 0
		printf "%-34s %9s %9s %7s %7s  %s\n", "row", "base_us", "now_us", "abs", "rel", "flag"
		for (i = 1; i <= n; i++) {
			k = order[i]; split(k, kk, ",")
			if (!(k in bmed)) { missing++; printf "%-34s %9s %9.1f %7s %7s  NEW (not in baseline)\n", kk[2], "-", fmed[k], "-", "-"; continue }
			h = (kk[1] in host) ? host[kk[1]] : 1
			abs = bmed[k] > 0 ? fmed[k] / bmed[k] : 1
			rel = abs / h
			flag = ""
			if (rel > 1 + T / 100 && fmed[k] - bmed[k] * h >= F && fmin[k] > bmin[k] * h * (1 + T / 200)) flag = "SLOWER"
			if (fal[k] > 0 || flk[k] > 0) flag = flag " ALLOC/LOCK on the audio thread (" fal[k] " allocs, " flk[k] " locks)"
			if (flag != "") flagged++
			if (flag != "" || ENVIRON["BENCH_ALL"] != "") printf "%-34s %9.1f %9.1f %7.2f %7.2f  %s\n", kk[2], bmed[k], fmed[k], abs, rel, flag
		}
		for (k in bmed) if (!(k in fmed)) { split(k, kk, ","); seen = 0; for (j = 1; j <= n; j++) if (plan_of[order[j]] == kk[1]) seen = 1
			if (seen) printf "%-34s MISSING from this run\n", kk[2] }
		printf "%d rows compared, %d flagged, %d new\n", n - missing, flagged, missing
		exit flagged > 0 ? 1 : 0
	}' "$baseline" "$fresh"
status=$?
if [ -z "$input" ]; then
	was=$(sleepers in | sort | tr '\n' ' ' | sed 's/ $//')
	cur=$(echo "$awake" | tr ' ' '\n' | awk 'NF' | sort | tr '\n' ' ' | sed 's/ $//')
	if [ "$cur" != "$was" ]; then
		echo "SLEEP CHANGED: idle rows that never sleep were [${was:-none}], are now [${cur:-none}]" >&2
		status=1
	fi
fi
exit $status
