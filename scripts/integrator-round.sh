#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# ONE INTEGRATOR ROUND, composed once (backlog NY item 4): build, ledger, install-on build, full ctest,
# the GUI suites under the private display, the named suites and cases, and the install only on green.
# It builds and tests the tree the script lives in, so run the copy in the tree you mean.
#
# TIERS. Two lanes, by what the round is for:
#   fast  (--fast, a local install): build, ledger, install-on build, ctest on the TOUCHED binaries only
#         (those named in --each, matched by ctest's own test names), the GUI ctest steps only for the
#         touched GUI binaries, then --each over the listed binaries with --jobs ${ROUND_EACH_JOBS:-3}.
#         No ASan, no windowed run, no preflight.
#   full  (the default, a release): the whole ctest, both GUI steps, and --each over the listed
#         binaries, also with --jobs. Preflight and the windowed run stay with the release procedure.
# ASAN IS NOT IN THIS SCRIPT (it is scripts/sanitize-gate.sh, run separately). Policy: ASan at a
# release; and before a local install only when the change touches object lifetimes (widget
# ownership, rack views, voice pools and stealing, plugin teardown), and then only on those suites.
#
# Usage: scripts/integrator-round.sh [--fast] [--each test_a,test_b] [--cases "binary::case name"]... [--install]
#   --fast   the fast lane above (touched binaries only; implies nothing about --install)
#   --each   comma-separated binaries under build/tests, each run through run-cases.sh --each
#   --cases  "binary::case name", run through run-cases.sh by name (repeatable)
#   --install  install both .claude files from build-install-on, only if every step was green
# Env: ROUND_LOG (log dir, default <repo>/worktrees/_briefs/logs/round-<timestamp>),
#      ROUND_JOBS (build/test -j, default 6), ROUND_JOBS_ON (install-on build -j, default 4).
#      ROUND_EACH_JOBS (case processes at once under --each, default 3).
# Exit status is non-zero on any red. Logs: one file per step in $ROUND_LOG.

set -u

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here" || exit 2

each_list=""
cases=()
install=0
fast=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--each) each_list=${2:-}; shift 2 ;;
		--cases) cases+=("${2:-}"); shift 2 ;;
		--install) install=1; shift ;;
		--fast) fast=1; shift ;;
		*) echo "usage: $0 [--fast] [--each a,b] [--cases \"binary::case name\"]... [--install]" >&2; exit 2 ;;
	esac
done

log=${ROUND_LOG:-$here/worktrees/_briefs/logs/round-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$log" || exit 2
jobs_n=${ROUND_JOBS:-6}
jobs_on=${ROUND_JOBS_ON:-4}
each_jobs=${ROUND_EACH_JOBS:-3}

# shellcheck disable=SC1091
. scripts/env.sh

red=0
reds=()
summary=()
start=$SECONDS

# step <name> <command...>: logs to $log/<name>.log, returns the command's status, notes a red.
step() {
	local name=$1
	shift
	local t=$SECONDS
	"$@" >"$log/$name.log" 2>&1
	local rc=$?
	STEP_SECS=$((SECONDS - t))
	if [[ $rc -ne 0 ]]; then
		red=1
		reds+=("$name")
	fi
	return $rc
}

# doctest_tally <log>: the last "test cases:" line, squeezed.
doctest_tally() {
	local l
	l=$(grep -a '^--each:' "$1" | tail -1)
	[[ -z $l ]] && l=$(grep -a '^ok \|^FAIL\|^fail' "$1" | tail -1)
	echo "${l:-no tally}"
}

# 1. build
step build ninja -C build -j"$jobs_n"
rc=$?
errs=$(grep -ac 'error:\|FAILED:' "$log/build.log")
if [[ $rc -eq 0 ]]; then build_s="build ok (${STEP_SECS}s)"; else build_s="build RED ($errs errors, build.log)"; fi

# 2. ledger
ledger_s="ledger skipped (build red)"
if [[ -x build/tests/test_state_ledger ]] && [[ $rc -eq 0 ]]; then
	if step ledger build/tests/test_state_ledger; then ledger_s="ledger ok"; else ledger_s="ledger RED"; fi
fi

# 3. install-on build
if [[ -d build-install-on ]]; then
	step build-install-on ninja -C build-install-on -j"$jobs_on"
	if [[ $? -eq 0 ]]; then on_s="install-on ok"; else on_s="install-on RED"; fi
else
	# a tree without the permanent install dir cannot install; only a red when an install was asked for
	on_s="install-on absent in this tree"
	if [[ $install -eq 1 ]]; then red=1; reds+=(build-install-on); fi
fi

# Fast lane: the ctest names that run the touched binaries (those named in --each and --cases), read from
# ctest's own command lines, split into plain and windowed (headless-incompatible) tests.
fast_plain=""
fast_gui_a=""
fast_gui_b=""
fast_note=""
if [[ $fast -eq 1 && $rc -eq 0 ]]; then
	touched=()
	IFS=',' read -r -a _eb <<<"$each_list"
	for b in "${_eb[@]}"; do [[ -n $b ]] && touched+=("$b"); done
	for spec in "${cases[@]+"${cases[@]}"}"; do touched+=("${spec%%::*}"); done
	all_names=$(ctest --test-dir build -N -V 2>/dev/null | awk -v bins="${touched[*]}" '
		BEGIN { n = split(bins, b, " ") }
		/^[0-9]+: Test command:/ { hit = 0; for (i = 1; i <= n; i++) if (index($0, "/" b[i] " ") || index($0, "/" b[i] "\"") || $0 ~ ("/" b[i] "$")) hit = 1 }
		/^ +Test +#[0-9]+:/ { if (hit) { name = $0; sub(/^ +Test +#[0-9]+: */, "", name); print name } hit = 0 }')
	gui_names=$(ctest --test-dir build -N -L headless-incompatible 2>/dev/null | sed -n 's/^ *Test *#[0-9]*: *//p')
	while IFS= read -r nm; do
		[[ -z $nm ]] && continue
		if grep -qxF -- "$nm" <<<"$gui_names"; then
			if [[ $nm == gui-widgets-controls* ]]; then fast_gui_b+="${fast_gui_b:+|}$nm"; else fast_gui_a+="${fast_gui_a:+|}$nm"; fi
		else
			fast_plain+="${fast_plain:+|}$nm"
		fi
	done <<<"$all_names"
	fast_note=" [fast: touched ${touched[*]:-none}]"
fi

# 4. ctest (full: everything; fast: the touched binaries only)
ctest_s="ctest skipped (build red)"
if [[ $rc -eq 0 ]]; then
	if [[ $fast -eq 1 ]]; then
		if [[ -n $fast_plain ]]; then
			step ctest ctest --test-dir build -j"$jobs_n" --timeout 2400 -R "^(${fast_plain})\$" --output-on-failure
			crc=$?
			tally=$(grep -a 'tests passed' "$log/ctest.log" | tail -1)
			ctest_s="ctest${fast_note} ${tally:-no tally} in ${STEP_SECS}s"
			[[ $crc -ne 0 ]] && ctest_s="ctest RED${fast_note}: $tally in ${STEP_SECS}s"
		else
			ctest_s="ctest${fast_note}: no plain ctest tests for the touched binaries"
		fi
	else
		step ctest ctest --test-dir build -j"$jobs_n" --timeout 2400 --label-exclude headless-incompatible --output-on-failure
		crc=$?
		tally=$(grep -a 'tests passed' "$log/ctest.log" | tail -1)
		ctest_s="ctest ${tally:-no tally} in ${STEP_SECS}s"
		[[ $crc -ne 0 ]] && ctest_s="ctest RED: $tally in ${STEP_SECS}s"
	fi
fi

# 5. GUI suites under the private display (fast: only the touched GUI binaries' tests)
gui_s="gui skipped (build red)"
if [[ $rc -eq 0 ]]; then
	if [[ $fast -eq 1 ]]; then
		ga="no touched gui tests"
		gb="no touched gui tests"
		if [[ -n $fast_gui_a ]]; then
			FEATHERS_REDUCED_MOTION=1 step gui-a scripts/venue-xvfb.sh run -- \
				ctest --test-dir build -j"$jobs_n" --timeout 1500 -R "^(${fast_gui_a})\$"
			ga=$(grep -a 'tests passed' "$log/gui-a.log" | tail -1)
		fi
		if [[ -n $fast_gui_b ]]; then
			step gui-b scripts/venue-xvfb.sh run -- \
				ctest --test-dir build -j"$jobs_n" --timeout 1500 -R "^(${fast_gui_b})\$"
			gb=$(grep -a 'tests passed' "$log/gui-b.log" | tail -1)
		fi
		gui_s="gui: ${ga:-no tally} | controls: ${gb:-no tally}"
	else
		FEATHERS_REDUCED_MOTION=1 step gui-a scripts/venue-xvfb.sh run -- \
			ctest --test-dir build -j"$jobs_n" --timeout 1500 -L headless-incompatible -E gui-widgets-controls
		ga=$(grep -a 'tests passed' "$log/gui-a.log" | tail -1)
		step gui-b scripts/venue-xvfb.sh run -- \
			ctest --test-dir build -j"$jobs_n" --timeout 1500 -R gui-widgets-controls
		gb=$(grep -a 'tests passed' "$log/gui-b.log" | tail -1)
		gui_s="gui: ${ga:-no tally} | controls: ${gb:-no tally}"
	fi
fi

# 6. named suites and cases
# headless, the real-window cases (test_rack_clap) go red; under the private display they pass
xv=(env FEATHERS_REDUCED_MOTION=1 scripts/venue-xvfb.sh run --)
each_lines=()
if [[ $rc -eq 0 ]]; then
	if [[ -n $each_list ]]; then
		IFS=',' read -r -a each_bins <<<"$each_list"
		for b in "${each_bins[@]}"; do
			[[ -z $b ]] && continue
			# the controls binary holds the animation cases, which need motion (as the gui-b ctest step runs it)
			if [[ $b == test_gui_widgets_controls ]]; then
				step "each-$b" scripts/venue-xvfb.sh run -- scripts/run-cases.sh --each --jobs "$each_jobs" "build/tests/$b"
			else
				step "each-$b" "${xv[@]}" scripts/run-cases.sh --each --jobs "$each_jobs" "build/tests/$b"
			fi
			each_lines+=("each $b: $(doctest_tally "$log/each-$b.log")")
		done
	fi
	n=0
	for spec in "${cases[@]+"${cases[@]}"}"; do
		n=$((n + 1))
		bin=${spec%%::*}
		name=${spec#*::}
		step "case-$n" "${xv[@]}" scripts/run-cases.sh "build/tests/$bin" "$name"
		each_lines+=("case $bin::$name: $(doctest_tally "$log/case-$n.log")")
	done
	sleep_case="a reverb's tail keeps the plugin awake: SLEEP is never promised while the output is still sounding"
	step case-sleep "${xv[@]}" scripts/run-cases.sh build/tests/test_clap_render "$sleep_case"
	each_lines+=("SLEEP case: $(doctest_tally "$log/case-sleep.log")")
fi

# 7. install, only on green
if [[ $red -eq 0 && $install -eq 1 ]]; then
	if step install scripts/install-clap.sh build-install-on/feathers.clap &&
		step install-chain scripts/install-clap.sh build-install-on/feathers-chain.clap; then
		a=$(sha256sum "$HOME/.clap/feathers.clap" | cut -c1-12)
		b=$(sha256sum "$HOME/.clap/feathers-chain.clap" | cut -c1-12)
		install_s="installed feathers.clap $a, feathers-chain.clap $b"
	else
		install_s="install FAILED (install.log)"
	fi
elif [[ $install -eq 1 ]]; then
	install_s="not installed (red)"
else
	install_s="install not requested"
fi

# summary
failing=""
if [[ $red -ne 0 ]]; then
	failing="red steps: ${reds[*]}"
	for f in "$log"/ctest.log "$log"/gui-a.log "$log"/gui-b.log; do
		[[ -f $f ]] && failing+=" | $(grep -a 'tests failed\|(Failed)\|\*\*\*Failed\|Timeout' "$f" | sed 's/^ *[0-9]* *- *//' | head -4 | tr '\n' ';')"
	done
fi
verdict=GREEN
[[ $red -ne 0 ]] && verdict=RED
{
	echo "ROUND $verdict ($((SECONDS - start))s, logs $log)"
	echo "$build_s; $ledger_s; $on_s"
	echo "$ctest_s"
	echo "$gui_s"
	for l in "${each_lines[@]+"${each_lines[@]}"}"; do echo "$l"; done
	[[ -n $failing ]] && echo "$failing"
	echo "$install_s"
} | tee "$log/summary.txt"
exit $red
