#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# ONE INTEGRATOR ROUND, composed once (backlog NY item 4): build, ledger, install-on build, full ctest,
# the GUI suites under the private display, the named suites and cases, and the install only on green.
# It builds and tests the tree the script lives in, so run the copy in the tree you mean.
#
# Usage: scripts/integrator-round.sh [--each test_a,test_b] [--cases "binary::case name"]... [--install]
#   --each   comma-separated binaries under build/tests, each run through run-cases.sh --each
#   --cases  "binary::case name", run through run-cases.sh by name (repeatable)
#   --install  install both .claude files from build-install-on, only if every step was green
# Env: ROUND_LOG (log dir, default <repo>/worktrees/_briefs/logs/round-<timestamp>),
#      ROUND_JOBS (build/test -j, default 6), ROUND_JOBS_ON (install-on build -j, default 4).
# Exit status is non-zero on any red. Logs: one file per step in $ROUND_LOG.

set -u

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here" || exit 2

each_list=""
cases=()
install=0
while [[ $# -gt 0 ]]; do
	case "$1" in
		--each) each_list=${2:-}; shift 2 ;;
		--cases) cases+=("${2:-}"); shift 2 ;;
		--install) install=1; shift ;;
		*) echo "usage: $0 [--each a,b] [--cases \"binary::case name\"]... [--install]" >&2; exit 2 ;;
	esac
done

log=${ROUND_LOG:-$here/worktrees/_briefs/logs/round-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$log" || exit 2
jobs_n=${ROUND_JOBS:-6}
jobs_on=${ROUND_JOBS_ON:-4}

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

# 4. full ctest
ctest_s="ctest skipped (build red)"
if [[ $rc -eq 0 ]]; then
	step ctest ctest --test-dir build -j"$jobs_n" --timeout 2400 --label-exclude headless-incompatible --output-on-failure
	crc=$?
	tally=$(grep -a 'tests passed' "$log/ctest.log" | tail -1)
	ctest_s="ctest ${tally:-no tally} in ${STEP_SECS}s"
	[[ $crc -ne 0 ]] && ctest_s="ctest RED: $tally in ${STEP_SECS}s"
fi

# 5. GUI suites under the private display
gui_s="gui skipped (build red)"
if [[ $rc -eq 0 ]]; then
	FEATHERS_REDUCED_MOTION=1 step gui-a scripts/venue-xvfb.sh run -- \
		ctest --test-dir build -j"$jobs_n" --timeout 1500 -L headless-incompatible -E gui-widgets-controls
	ga=$(grep -a 'tests passed' "$log/gui-a.log" | tail -1)
	step gui-b scripts/venue-xvfb.sh run -- \
		ctest --test-dir build -j"$jobs_n" --timeout 1500 -R gui-widgets-controls
	gb=$(grep -a 'tests passed' "$log/gui-b.log" | tail -1)
	gui_s="gui: ${ga:-no tally} | controls: ${gb:-no tally}"
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
				step "each-$b" scripts/venue-xvfb.sh run -- scripts/run-cases.sh --each "build/tests/$b"
			else
				step "each-$b" "${xv[@]}" scripts/run-cases.sh --each "build/tests/$b"
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
