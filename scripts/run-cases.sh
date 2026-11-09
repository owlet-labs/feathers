#!/usr/bin/env bash
# Run named doctest cases and FAIL if a name matched nothing.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# Why this exists: doctest treats a comma in --test-case= as a FILTER SEPARATOR,
# and a filter that matches nothing exits 0 with the word SUCCESS:
#
#   $ test_gui_widgets --test-case="while it loops, the sustain box says what it has become"
#   [doctest] test cases: 0 | 0 passed | 0 failed | 341 skipped
#   [doctest] Status: SUCCESS!
#
# 329 of this repo's case names contain a comma, so the by-name run that
# CLAUDE.md's install protocol relies on is vacuous for most of the suite unless
# something checks the COUNT. That is this script's whole job.
#
# THE SAME TRAP EXISTS FOR `--subcase=` AND IS WORSE, so this script does not
# offer one and you should not hand-roll it either. A zero-match SUBCASE filter
# does not merely report zero cases — it reports the enclosing CASE as PASSING,
# with a plausible non-zero assertion count made up of the case-level REQUIREs
# that ran before the subcase would have. MEASURED 2026-08-28 by an agent who
# lost a conclusion to it:
#
#   [doctest] test cases: 1 | 1 passed | 0 failed
#   [doctest] assertions: 12 | 12 passed | 0 failed      <- the subcase never ran
#
# So the count cross-check below CANNOT catch the subcase version: the numbers it
# reads are all non-zero and all true. If you need one subcase, run the whole
# case and read which subcases printed, or give the subcase a comma-free name and
# check the assertion count against a known figure. **A subcase name containing a
# comma cannot be filtered at all** — doctest splits it and matches neither half.
#
# Usage:  scripts/run-cases.sh <test-binary> "case name" ["case name" ...]
#         scripts/run-cases.sh --each <test-binary>
#         RUN_CASES_JOBS=4 scripts/run-cases.sh --each <test-binary>   # up to 4 cases at once
# Commas in a name are turned into wildcards, so paste the name as written in
# the source. Exits non-zero if any name matched zero cases or any case failed.
#
#------------------------------------------------------------------------------
# --each: EVERY CASE IN ITS OWN PROCESS. For the known-bad protocol.
#------------------------------------------------------------------------------
#
# A KNOWN-BAD THAT CRASHES DOES NOT MERELY FAIL LOUDLY — IT ENDS THE RUN, and
# doctest reports the truncated tally in the same shape as a complete one:
#
#   [doctest] test cases: 12 | 8 passed | 4 failed | 3 skipped
#
# Nothing there says "and three cases never executed". MEASURED on the device
# pool, 2026-08-26: a mutation that made `retire()` destroy its argument was
# reported by the whole-suite run as implicating FOUR cases. Every case in its
# own process implicated FIVE — the extra one failing a plain
# `CHECK(probe.destructed == 0)` reading `2 == 0`, which had never run in any
# whole-suite invocation anybody had made.
#
# SO THE MASKING IS NOT ONLY A SHORT TALLY. What a known-bad PROVES is computed
# from the cases that survived it, and the ones it kills are the ones nearest
# the fault — the most likely to have something to say. A crash-red read off a
# single whole-suite run is a partial run reported as a verdict.
#
# One process per case fixes it because a crash then kills only its own case.
# This is also the death test doctest does not have: a case that must segfault
# is a case whose process must die, and nothing else's.
#
# DO NOT RE-ROLL THIS LOOP AT THE CALL SITE. Two independent hand-rolled
# versions of it were written while establishing the above, and one of them
# scored doctest's `====` separator lines as passing cases — the empty-filter
# trap this whole script exists to catch, reappearing inside the loop written
# to check the script. The count cross-check below is what makes that
# impossible here rather than unlikely.
#
# COST, MEASURED rather than estimated, on this host 2026-08-26:
#
#   test_device_instance_pool   15 cases    0.16s
#   test_gui_widgets           438 cases   55.62s
#
# About 0.13s of process spawn per case, which is free for a focused suite and
# distinctly not free for the big one. So --each belongs in the KNOWN-BAD
# PROTOCOL, pointed at the suite the mutation touches, and NOT in the ordinary
# ctest path: `ctest` still runs each binary once, as it always did.
#
# (The same 438-case run raised zero duplicate-name warnings and matched every
# name exactly once, which is the evidence that the comma-to-wildcard rule
# survives being pointed at a whole listing rather than a hand-picked name.)
#
# RUN_CASES_JOBS=N RUNS UP TO N CASE-PROCESSES AT ONCE (default 1: exactly the
# sequential run above). Every guarantee in this header is kept — one process
# per case, the per-case timeout, CRASH/FAIL/TIMEOUT told apart, output captured
# per case and replayed in listing order, and the tally cross-checked against
# the binary's own count so a worker that dies leaves a short total and a FAIL.
# See the loop itself for the known-bad that proved the last one.
#
# MEASURED 2026-09-02, test_gui_widgets (536 cases) under scripts/venue-xvfb.sh,
# Release -O3, this 16-core host, 1-min load at start -> end:
#
#   JOBS=1                         260 s   535 pass / 0 fail / 1 crash   load 17 -> 30
#   JOBS=4                         143 s   535 / 0 / 1                   load 27 -> 22
#   JOBS=8                         110 s   536 / 0 / 0                   load 22 -> 10
#   JOBS=4, RUN_CASES_TIMEOUT=600   112 s   536 / 0 / 0                   load  7 ->  7
#   JOBS=4, RUN_CASES_TIMEOUT=2400  103 s   536 / 0 / 0                   load  4 ->  8
#   JOBS=8, RUN_CASES_TIMEOUT=2400  186 s   535 / 1 / 0                   load  8 -> 14
#
# The set of passing NAMES was identical across the first five runs (diffed).
# The one "crash" in the first two is the same case both times — "a breakpoint
# at either extreme is as selectable as one in the middle" — hitting the 120 s
# DEFAULT timeout: it is 94 s of CPU on its own (92.6 s user in 94.2 s wall,
# measured alone), so at a load of 20+ it does not fit. It passed at JOBS=8
# because the load had dropped to 10 by then, and at JOBS=4 with the timeout
# raised. That is load, not concurrency.
#
# THE ONE FAIL, at JOBS=8 on the last row, is the same shape from inside a case:
# "the chart draws the sound's own neighbourhood, and holds still while a drag
# writes gamma" waits a bounded 8000 x 1 ms for the background terrain rebuild
# after the release (`REQUIRE(after)`, test_gui_widgets.cpp), and under eight
# widget processes at load 14 the bake did not finish in 8 s. It passes alone
# 3/3, draws to an offscreen canvas (no window, so not a display collision), and
# the code under test has no race in it — a test-side wall-clock bound was
# starved. So: FOUR IS THE SETTING (clean on every run, and it is what the
# sanitizer gate uses); eight is not one to put on a gate on a shared host. On a
# suite where every process opens its own window on the one Xvfb — which is also
# how ctest already runs it, 12 shards at once (tests/CMakeLists) — no case
# failed or crashed under parallelism for a reason other than time.

set -uo pipefail

each=0
if [[ ${1:-} == --each ]]; then
	each=1
	shift
fi

if [[ $each -eq 1 && $# -ne 1 ]] || [[ $each -eq 0 && $# -lt 2 ]]; then
	echo "usage: $0 <test-binary> \"case name\" [\"case name\" ...]" >&2
	echo "       $0 --each <test-binary>" >&2
	exit 2
fi

binary=$1
shift

if [[ ! -x $binary ]]; then
	echo "run-cases: not executable: $binary" >&2
	exit 2
fi

# A HUNG CASE IS THE MOST COMPLETE PARTIAL RUN THERE IS, and --each exists to be
# pointed at deliberately broken builds — a corrupted intrusive queue walks its
# own tail forever rather than crashing. Bounded only in --each mode, so the
# ordinary by-name path behaves exactly as it always has. `timeout` is coreutils
# and absent on a stock macOS, so its absence is tolerated rather than required.
timeout_cmd=()
if [[ $each -eq 1 ]] && command -v timeout >/dev/null 2>&1; then
	timeout_cmd=(timeout "${RUN_CASES_TIMEOUT:-120}")
fi

status=0

# ONE NAME, ONE PROCESS. Sets `verdict` to ok|fail|crash and emits the same
# lines it always did. Both paths go through here so the comma rule, the
# zero-match guard and the duplicate-name warning have exactly one home.
verdict=""
run_one() {
	local name=$1
	# A comma would split the filter in two; a '*' matches it instead.
	# Wrap the whole thing so a pasted fragment matches as a substring.
	local filter="*${name//,/*}*"
	local output rc line count failed
	output=$("${timeout_cmd[@]}" "$binary" --test-case="$filter" 2>&1)
	rc=$?
	line=$(grep -m1 'test cases:' <<<"$output" || true)
	count=$(sed -n 's/.*test cases: *\([0-9]*\).*/\1/p' <<<"$line")
	failed=$(sed -n 's/.*| *\([0-9]*\) failed.*/\1/p' <<<"$line")

	if [[ -z ${count:-} && $rc -eq 124 ]]; then
		# KILLED BY THE TIMEOUT BEFORE IT COULD PRINT A SUMMARY. Checked before the generic no-summary
		# branch below, because that one used to swallow this: doctest prints a summary on SIGTERM only
		# if it is in a position to, and a case deep in a render was reported as "the binary died" with
		# empty output — MEASURED 2026-09-01 under ASan, where a passing 4-minute case looked like a
		# silent crash. A timeout says nothing about the case except that it was slow.
		echo "CRASH timed out after ${RUN_CASES_TIMEOUT:-120}s with no doctest summary: $name" >&2
		verdict=crash
		status=1
	elif [[ -z ${count:-} ]]; then
		# NO SUMMARY AT ALL: the process died before printing one.
		echo "CRASH no doctest summary — the binary died: $name" >&2
		echo "$output" >&2
		verdict=crash
		status=1
	elif [[ $rc -ge 128 || $rc -eq 124 ]]; then
		# A SUMMARY IS NOT PROOF THE PROCESS SURVIVED, and this is the branch that
		# costs a reader their conclusion if it is missing. doctest installs signal
		# handlers: a segfaulting case prints "FATAL ERROR: test case CRASHED",
		# prints a normal-looking `test cases: 1 | 0 passed | 1 failed` line, and
		# exits 139. Classify on the SUMMARY alone and every crash-red is filed as
		# an ordinary assertion failure.
		#
		# MEASURED: the first draft of --each did exactly that, and reported
		# `crash=0` against all three of the device pool's crashing known-bads —
		# the distinction this mode was asked to preserve, deleted by trusting a
		# line the dying process had already printed.
		#
		# THE DISTINCTION IS EVIDENTIAL, not cosmetic. An assertion-red names the
		# value that was wrong. A crash-red names only the case that was running,
		# and says nothing about the ones it would have gone on to reach — which is
		# the whole reason for running each case in its own process.
		if [[ $rc -eq 124 ]]; then
			echo "CRASH timed out after ${RUN_CASES_TIMEOUT:-120}s: $name" >&2
		else
			echo "CRASH died on signal $((rc - 128)): $name" >&2
		fi
		echo "$output" >&2
		verdict=crash
		status=1
	elif [[ $count -eq 0 ]]; then
		echo "FAIL  matched 0 cases: $name" >&2
		echo "      (a zero-case run exits 0 and prints SUCCESS; that is the trap)" >&2
		verdict=fail
		status=1
	elif [[ ${failed:-0} -ne 0 ]]; then
		echo "FAIL  $failed of $count failed: $name" >&2
		echo "$output" >&2
		verdict=fail
		status=1
	elif [[ $count -gt 1 ]]; then
		# MATCHED MORE THAN ONE, WHICH IS SOMETIMES RIGHT AND SOMETIMES A DUPLICATE NAME.
		#
		# Running by name proves a case EXISTS and PASSES. It does not prove it exists ONCE. Two
		# TEST_CASEs sharing a name compile, run, and pass -- and this script reported "ok 2 case(s)"
		# on exactly that for an unknown length of time. FOUND 2026-08-24, and the pair was not a
		# harmless copy: a case had been REWRITTEN because its measurement was unsound, and the
		# superseded copy was left behind under the same name, still asserting the property the
		# rewrite had deliberately rejected. It passed only by luck of the current palette.
		#
		# NOT AN ERROR, because this script turns commas into wildcards, so a legitimate filter can
		# match siblings on purpose. A warning is the honest verdict: the caller knows whether they
		# asked for one case or a family.
		echo "ok    $count case(s): $name"
		echo "WARN  matched $count cases for one name — if you expected ONE, check for a duplicate:" >&2
		echo "      grep -c 'TEST_CASE(\"<name>\")' tests/*.cpp" >&2
		verdict=ok
	else
		echo "ok    $count case(s): $name"
		verdict=ok
	fi
}

if [[ $each -eq 0 ]]; then
	for name in "$@"; do
		run_one "$name"
	done
	exit $status
fi

#------------------------------------------------------------------------------
# --each
#------------------------------------------------------------------------------

listing=$("$binary" --list-test-cases 2>&1)

# THE BINARY'S OWN COUNT, NOT ONE THIS SCRIPT DERIVES. doctest ends the listing
# with "unskipped test cases passing the current filters: N", and that N is the
# only number here that does not come from this script's own parsing. Every
# other total below is checked against it, so a filter that drops a name — or
# invents one out of a separator line — is a hard failure rather than a quieter
# report. That cross-check IS the guarantee; without it this is just another
# hand-rolled loop.
declared=$(sed -n 's/.*test cases passing the current filters: *\([0-9]*\).*/\1/p' <<<"$listing" | tail -1)

if [[ -z ${declared:-} ]]; then
	echo "FAIL  could not read the case count from --list-test-cases" >&2
	echo "$listing" >&2
	exit 1
fi

# Names are the lines between doctest's two rules: not the `[doctest]` lines,
# and not the `====` rules themselves.
#
# AND NOT THE BUILD'S OWN BANNER. A diagnostic build prints `[FEATHERS] ...` lines before doctest
# says anything, and `build-install-on` is ALWAYS a diagnostic build — which is the one directory
# CLAUDE.md requires these by-name runs in. Without this filter the count check below fired on every
# `--each` there and refused to run, so the install protocol's own verification step could not be
# carried out in the directory it is written for. It failed SAFE rather than reporting a wrong
# number, which is why this was a nuisance and not a false green.
mapfile -t names < <(grep -v '^\[doctest\]' <<<"$listing" | grep -v '^\[FEATHERS\]' \
	| grep -v '^=\+$' | sed '/^[[:space:]]*$/d')

if [[ ${#names[@]} -ne $declared ]]; then
	echo "FAIL  parsed ${#names[@]} case names but the binary declares $declared" >&2
	echo "      the listing format changed; fix the filter rather than trusting either number" >&2
	exit 1
fi

pass=0
fail=0
crash=0
jobs=${RUN_CASES_JOBS:-1}
if ! [[ $jobs =~ ^[0-9]+$ ]] || [[ $jobs -lt 1 ]]; then
	echo "FAIL  RUN_CASES_JOBS must be a positive integer, got '$jobs'" >&2
	exit 2
fi

if [[ $jobs -eq 1 ]]; then
	for name in "${names[@]}"; do
		run_one "$name"
		case $verdict in
		ok) ((pass++)) ;;
		fail) ((fail++)) ;;
		crash) ((crash++)) ;;
		esac
	done
else
	# RUN_CASES_JOBS > 1: UP TO $jobs CASE-PROCESSES AT ONCE, EVERY GUARANTEE ABOVE KEPT.
	#
	# Each case still gets its own process, its own timeout and its own verdict — run_one is the same
	# function, called in a background subshell whose stdout, stderr and verdict go to three files
	# named by the case's INDEX in the listing. Nothing is printed while workers run; when they are all
	# reaped the files are replayed in listing order, so the report reads exactly as the sequential one
	# does and two crashing cases cannot interleave their stack traces.
	#
	# THE TALLY IS STILL THE BINARY'S, NOT THIS LOOP'S. A verdict is only counted if its worker wrote
	# one, so a worker that dies (killed, OOM, the shell itself) leaves a case with NO verdict, that
	# case is reported by name, and `total` comes up short of `declared` — the same FAIL the sequential
	# path raises for a partial run. Known-bad, 2026-09-02: a worker mutated to exit before writing its
	# verdict was reported as "accounted for N-1 of N — this run is PARTIAL", exit 1.
	#
	# `wait -n` (bash 4.3+) blocks until ANY worker exits, which is what keeps $jobs of them busy
	# rather than running in lockstep batches.
	scratch=$(mktemp -d "${TMPDIR:-/tmp}/run-cases.XXXXXX")
	trap 'rm -rf "$scratch"' EXIT
	running=0
	for i in "${!names[@]}"; do
		if [[ $running -ge $jobs ]]; then
			wait -n
			((running--))
		fi
		(
			run_one "${names[$i]}" >"$scratch/$i.out" 2>"$scratch/$i.err"
			printf '%s\n' "$verdict" >"$scratch/$i.verdict"
		) &
		((running++))
	done
	wait

	for i in "${!names[@]}"; do
		cat "$scratch/$i.out" 2>/dev/null
		cat "$scratch/$i.err" >&2 2>/dev/null
		v=$(cat "$scratch/$i.verdict" 2>/dev/null || true)
		case $v in
		ok) ((pass++)) ;;
		fail) ((fail++)) ;;
		crash) ((crash++)) ;;
		*)
			echo "FAIL  no verdict recorded — the worker for this case died: ${names[$i]}" >&2
			;;
		esac
	done
	if [[ $((pass + fail + crash)) -ne ${#names[@]} || $fail -ne 0 || $crash -ne 0 ]]; then
		status=1
	fi
fi

total=$((pass + fail + crash))
echo "--each: pass=$pass fail=$fail crash=$crash  total=$total of $declared  (jobs=$jobs)"

# A RUNNER FOR THIS PROBLEM MUST NOT BE ABLE TO REPORT A PARTIAL RUN AS A
# VERDICT. Reaching here with a short total means cases were listed and never
# accounted for, which is the exact failure --each exists to make impossible.
if [[ $total -ne $declared ]]; then
	echo "FAIL  accounted for $total of $declared cases — this run is PARTIAL" >&2
	status=1
fi

exit $status
