#!/usr/bin/env bash
# The sanitizer gate: build the lifetime suites under AddressSanitizer and FAIL if it speaks.
#
# This file is part of Feathers.
# Copyright (C) 2026 Owlet Records — GPL-3.0-or-later.
#
# Usage:  scripts/sanitize-gate.sh                 # configure (once), build, run, exit non-zero on any finding
#         scripts/sanitize-gate.sh --no-build      # run only, against whatever build-asan holds
#         FEATHERS_ASAN_DIR=elsewhere scripts/sanitize-gate.sh
#
# WHY THIS EXISTS. Three use-after-frees shipped through a green `ctest` in the week of 2026-08-31
# (b36e197: a Knob unwatching a ModState that had already been destroyed; e9b84ee: a watcher
# registered and never unregistered; and an aligned `new` freed by an unaligned `delete[]` in the
# rack engine, present since the engine was written). None of them fault: freed memory is still
# mapped and plausible, and every test in this repo asserts VALUES. `ctest` is structurally blind to
# the class. ASan is the instrument that sees it, and until this script it was run by hand, which is
# to say it was run twice and skipped every other day. An instrument wired into nothing is one a tired
# session skips, and its absence is invisible because nothing else goes red to mark it.
#
# WHAT IT DOES, in order, and each step fails on its own:
#
#   1. Configures a SEPARATE build directory (default `build-asan/`) with -DFEATHERS_SANITIZE=address.
#      Never `build`, never `build-install-on` — those two have identities of their own (CLAUDE.md),
#      and a sanitizer configure is a different program. If the ordinary `build/` has already fetched
#      bgfx, freetype and the clap-wrapper SDKs, they are reused by path so the configure does not
#      clone them again; the tags are pinned by visage and clap-wrapper, not by the path, so a stale
#      copy is a configure error rather than a silent substitution.
#
#   2. Checks the REAL compile line in that directory's build.ninja — for a plugin TU and for the
#      visage graphics TU — for `-fsanitize=address` and for `-UNDEBUG` AFTER `-DNDEBUG`. The second
#      half is the one worth stating: RelWithDebInfo defines NDEBUG, which compiles `~FrameWatchers`'s
#      ordering assert to nothing, and that assert is the one written for exactly this bug class.
#      This is the check CLAUDE.md requires of an install for the same reason: it cannot be fooled by
#      an option that was set and then not applied.
#
#   3. Builds the suites where the lifetime bugs live: the six test_gui_widgets_* binaries (the editor,
#      assembled -- one file until the 2026-09-03 split, see tests/gui_widgets/fixtures.h),
#      test_rack_editor, test_rack_engine, test_rack_state, test_synth, the four SKLITTER suites
#      (added 2026-09-14: that device is deliberately NOT in `kDevices` yet, so every other entry
#      here reaches devices through the rack and would never construct one -- a green about a device
#      the gate never touched, which is the exact shape this script exists to refuse), and the rack
#      engine Feathers' lane and strip run on (test_rack_voices, test_lane_rack — ModRunner and
#      DeviceInstancePool, which held the modulator lifetimes until 2026-09-11, are gone), and
#      test_converger, the desired-state road every host's engine takes (its mailboxes and hand-back),
#      and test_chain_record, the record that feeds it, end to end through a publish.
#
#   4. Runs `ctest -L sanitize` there. Those ctest entries (tests/CMakeLists.txt, registered only when
#      FEATHERS_SANITIZE is set) run each suite through `scripts/run-cases.sh --each`: ONE PROCESS PER
#      CASE, which is what makes an ASan abort kill only the case that caused it rather than ending the
#      run — and run-cases.sh cross-checks its own tally against the count the binary declares, so a
#      run cut short is a FAIL rather than a shorter green (playbook §8n-quater). The widget suite runs
#      under `scripts/venue-xvfb.sh`, which gives it a private display, a scratch XDG_CONFIG_HOME and
#      XDG_DATA_HOME and FEATHERS_PRESET_ROOT (otherwise it writes the user's preferences and preset
#      library), and lavapipe.
#
#   5. Counts the sanitizer REPORT BLOCKS in the ctest log (AddressSanitizer and ThreadSanitizer alike,
#      one per `==<pid>==` opener -- run-cases' `ok` is never evidence of sanitizer silence), tells a
#      libX11 SEGV with no frame of ours apart from a finding by retrying that one case once (backlog
#      BO; the rules are at step 5 below), and prints what it found, so the exit code is
#      accompanied by WHAT the tool said, not only that it said something.
#
# ASAN_OPTIONS ARE SET ON THE ctest ENTRIES, NOT HERE, so that `ctest --test-dir build-asan -L sanitize`
# run by hand behaves identically. `abort_on_error=1` so a report ends the case's process on SIGABRT,
# which run-cases.sh classifies as CRASH and names, rather than ASan's default `_exit(1)` which prints
# no doctest summary and reads as "the binary died" with no signal. `detect_leaks=0` because a leak at
# process exit is a different class from a use-after-free and its noise floor under lavapipe and bgfx
# has NOT been measured — turning it on is a one-line experiment, not a policy this script has earned.
#
# COST, MEASURED 2026-09-01 on a host running three other builds (load 30-60 while building, 3-10
# while running) — so these are upper bounds about other sessions as much as about this script:
#   - first configure: under a second when build/ has already fetched the dependencies (see step 1)
#   - first build of the four suites: 2347 s wall, dominated by test_gui_widgets.cpp under ASan at
#     -O2 (~25 min at 60% of a core for that one TU). A header touch that reaches the UI rebuilds
#     most of it again (~15 min here); a leaf edit is seconds.
#   - the run, ctest -j4: rack suites 1-4 s each; the widget suite 622 s for 527 cases one per
#     process, of which two cases are 4.5 and 8.3 minutes on their own (RUN_CASES_TIMEOUT is raised
#     for that entry — see tests/CMakeLists.txt). So: about ten minutes per run once built.
#
# AND RE-MEASURED 2026-09-03 AFTER THE WIDGET SUITE BECAME SIX BINARIES (517e65a), both halves on a
# loaded host again, so the same caveat applies:
#   - first build of all eleven targets: 14m40s wall at load 25-42 (ninja's log; the same tree that
#     took 2347 s as one TU). The six widget TUs under ASan compile in 119-300 s each (dahdsr 300,
#     devices 185, controls 171, explore 168, lane 157, mod_ring 119) and in parallel, so a header
#     touch that reaches the UI now costs the LONGEST of them rather than the sum, about five minutes
#     here. gui_widgets/main.cpp is 67-93 s per binary under ASan (doctest's implementation, compiled
#     six times) — a cost the fixture library could absorb if it ever matters.
#   - AND THAT 67-93 s WAS WRONG, corrected the same evening by the session that wrote it. It was read
#     off a PARALLEL ninja log, so it is CONTENTION, NOT COST: a per-object time from a `-j16` build
#     measures the machine's load as much as the object. Measured SERIALLY instead, one object at a
#     time with ccache disabled on a quiet host, that TU is ~12 s (10.6-11.8 s under ASan with the
#     implementation in it, 0.92 s without; the single shared TU 12.0 s) — so the six binaries were
#     paying about 72 s of compile for one framework, not 400-560. The saving was real and an ORDER
#     OF MAGNITUDE smaller than this note implied, and doctest's implementation now compiles once in
#     the fixture library. Every other per-object number in this section comes from the same parallel
#     log and carries the same caveat: read them as upper bounds under load, never as the cost of a
#     TU. Playbook 8av from the other end — measure the thing on the tree and the machine state your
#     decision will actually live on.
#   - the run, ctest -j4, eleven entries: 830 s wall, load 12.5 at the start and 33.7 at the end,
#     0 reports — AND IT IS BOUNDED BY ONE SHARD. sanitize-gui-widgets-dahdsr takes 823 s alone (156
#     cases at RUN_CASES_JOBS=4; the 8.3-minute breakpoint case lives there), explore 119 s, devices
#     8.8 s, mod-ring 1.2 s. So -j4 buys nothing while dahdsr is the long pole: the lever is
#     dahdsr's own JOBS or splitting it, measured before it is changed, not this script's -j.
#   - CORRECTED THE SAME EVENING: the 823 s entry was measured on 517e65a, BEFORE BK. The pole was
#     never the case's grid; it was backlog BL — MsegCard::layoutNow() reads its row through the
#     device host, ~38 fields, and the host copied the WHOLE ModRack per field, so each of the 1.6 M
#     hit tests copied the rack ~38 times. T's per-field readers plus BK's swap of the seam onto them
#     were the fix. At HEAD (94b5348), same code in mseg: the case is 3.2 s in the ordinary build and
#     13.8 s under ASan; the dahdsr binary --each at JOBS=4 is 44.8 s (was 823), top five below,
#     so the gate's run is bounded by EXPLORE now: sanitize-gui-widgets-explore 97.5 s at HEAD (was 119 s
#     before BK; measured 94b5348, load 5.2 -> 4.0). Re-measure the pole at HEAD before designing its
#     lever -- playbook 8av.
#           42.7 s  every field the terrain builder reads is in the content key
#           15.1 s  a breakpoint at either extreme is as selectable as one in the middle
#            3.3 s  undoing a remove puts the device back with the routings that came out with it
#            2.4 s  the terrain window is assembled from bases, not from modulated readings
#            2.3 s  a card destroyed in the fx lane leaves nothing behind on the MODULATION watcher list
# The ordinary `ninja -C build` is untouched: FEATHERS_SANITIZE is off unless asked for, and this
# script never configures anything but its own directory.
#
# WHAT A GREEN HERE MEANS, SAID EXACTLY: no case in these suites, run one at a time, performed
# a memory access ASan classifies as an error -- or, if a report was classified INFRASTRUCTURE (a
# libX11 SEGV, no frame of ours, that case retried green), the summary line says so in capitals and
# the raw report is still in the log above it. It says nothing about a race (TSan; the rack has the
# threaded cases for that), nothing about a suite not listed, and nothing about code no case reaches.
# And it was shown able to go RED before its green was believed: the commit that added it checked out
# 44a18a8 (the commit before the b36e197 fix), built it with this same configure, and watched the
# widget suite go red on `Knob::~Knob -> ModState::unwatch` reading a freed vector. A gate that has
# never been red is a paragraph.

set -uo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here" || exit 2

dir=${FEATHERS_ASAN_DIR:-build-asan}
build=1
for arg in "$@"; do
	case $arg in
	--no-build) build=0 ;;
	-h | --help)
		sed -n '2,10p' "$0"
		exit 0
		;;
	*)
		echo "sanitize-gate: unknown argument: $arg" >&2
		exit 2
		;;
	esac
done

# This host keeps cmake/ninja/g++-14 out of PATH (scripts/env.sh). Elsewhere the toolchain is the
# system's; sourcing env.sh there would point CC at a compiler that does not exist.
if [[ -f scripts/env.sh ]] && command -v g++-14 >/dev/null 2>&1; then
	# shellcheck disable=SC1091
	. scripts/env.sh
fi

step() { echo; echo "== sanitize-gate: $*"; }
die() {
	echo "sanitize-gate: FAIL — $*" >&2
	exit 1
}

if [[ $build -eq 1 ]]; then
	#-------------------------------------------------------------------- 1. configure
	if [[ ! -f $dir/build.ninja ]] || ! grep -q '^FEATHERS_SANITIZE:STRING=address$' "$dir/CMakeCache.txt" 2>/dev/null; then
		step "configuring $dir (-DFEATHERS_SANITIZE=address)"
		reuse=()
		[[ -d build/_deps/bgfx-src ]] && reuse+=(-DFETCHCONTENT_SOURCE_DIR_BGFX="$here/build/_deps/bgfx-src")
		[[ -d build/_deps/freetype-src ]] && reuse+=(-DFETCHCONTENT_SOURCE_DIR_FREETYPE="$here/build/_deps/freetype-src")
		[[ -d build/cpm/rtaudio ]] && reuse+=(-DRTAUDIO_SDK_ROOT="$here/build/cpm/rtaudio")
		[[ -d build/cpm/rtmidi ]] && reuse+=(-DRTMIDI_SDK_ROOT="$here/build/cpm/rtmidi")
		[[ -d build/cpm/vst3sdk ]] && reuse+=(-DVST3_SDK_ROOT="$here/build/cpm/vst3sdk")
		[[ ${#reuse[@]} -gt 0 ]] && echo "   reusing ${#reuse[@]} fetched source tree(s) from build/"
		cmake -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFEATHERS_SANITIZE=address "${reuse[@]}" ||
			die "configure failed"
	else
		step "$dir is already configured for address; reusing it"
	fi

	#-------------------------------------------------------------------- 2. the real compile line
	step "checking the compile line in $dir/build.ninja"
	check_flags() {
		local pattern=$1 label=$2 flags
		flags=$(awk -v pat="$pattern" '$0 ~ pat && /\.o:/ {f=1} f && /FLAGS =/ {print; exit}' "$dir/build.ninja")
		[[ -n $flags ]] || die "no compile rule matched '$pattern' ($label) in $dir/build.ninja"
		grep -q -- '-fsanitize=address' <<<"$flags" || die "$label is compiled WITHOUT -fsanitize=address: $flags"
		# -UNDEBUG must come after -DNDEBUG or the assert is still dead. Strip everything up to the
		# last -DNDEBUG and require -UNDEBUG in what remains.
		local after=${flags##*-DNDEBUG}
		grep -q -- '-UNDEBUG' <<<"$after" || die "$label has NDEBUG defined (no -UNDEBUG after -DNDEBUG): $flags"
		echo "   ok: $label — sanitized, assert live"
	}
	check_flags 'src/plugin/gui\.cpp' "plugin TU (gui.cpp)"
	check_flags 'VisageGraphics' "visage graphics TU"
	check_flags 'rack/.*\.cpp' "rack TU"

	#-------------------------------------------------------------------- 3. build
	#
	# **THIS LIST AND THE `foreach` IN tests/CMakeLists.txt MUST AGREE, AND NOTHING ENFORCES IT.**
	# MEASURED 2026-09-10: three suites were added to the ctest side (container-host,
	# composite-chain, container-bodies) and not here, so their binaries were never built and ctest
	# failed them in **0.00 sec** apiece.
	#
	# THE GATE WENT RED RATHER THAN GREEN, WHICH IS THE ONE THING THAT MATTERS — a hand-list the code
	# has stopped matching normally fails in the SAFE-LOOKING direction, and this one cannot: a
	# missing target is a missing binary is a failed test. **A `sanitize-*` entry failing in 0.00 sec
	# is this, every time.** Add the target here and re-run; do not read it as a flake.
	#
	# It stays an explicit list rather than becoming `ninja -C "$dir"` because building everything
	# under ASan costs the whole tree for no extra coverage — the gate's header has the 14m40s
	# measurement — and because the failure mode above is loud.
	# -j4 BY DEFAULT (feathers-2c, 2026-11-07): ninja's own default (cores + 2 = 18) drove the host to load ~26 while
	# keeps the DAW playing cleanly on the shared host (CLAUDE.md, "builds share a host with the DAW"). SANITIZE_GATE_JOBS overrides it.
	step "building the lifetime suites (the widget binaries, the rack's, synth, lane, container, sklitter, delay, shaper, eq, takeover, the two clap hosts)"
	ninja -C "$dir" -j "${SANITIZE_GATE_JOBS:-4}" test_gui_widgets_controls test_gui_widgets_explore test_gui_widgets_lane test_gui_widgets_dahdsr test_gui_widgets_devices test_gui_widgets_mod_ring test_gui_widgets_nest test_gui_widgets_presets test_gui_widgets_tooltips test_gui_widgets_undo test_rack_editor test_rack_engine test_rack_state test_synth test_container_bodies test_container_state test_rack_nesting test_rack_voices test_lane_rack test_converger test_chain_record \
		test_fx_sklitter test_fx_sklitter_modes test_fx_sklitter_ring test_fx_sklitter_engage \
		test_inner_chain test_sklitter_chain test_fx_delay test_fx_pitch test_fx_eroder test_fx_disperser test_fx_phaser test_fx_comp test_fx_clip test_file_dialog test_state_json \
		test_fx_shaper test_fx_eq test_takeover test_rack_clap test_clap_render test_pattern_device test_group_copy \
		|| die "build failed"
fi

#-------------------------------------------------------------------- 4. run
[[ -f $dir/build.ninja ]] || die "$dir is not configured (drop --no-build)"
if [[ -n ${SANITIZE_GATE_CLASSIFY_LOG:-} ]]; then
	# SELF-TEST HOOK (see step 5): classify an existing log instead of running. The retry is real.
	step "classifying $SANITIZE_GATE_CLASSIFY_LOG against $dir (no run)"
	log=$SANITIZE_GATE_CLASSIFY_LOG
	rc=0; grep -q "^The following tests FAILED:" "$log" && rc=8
	elapsed=0
else
step "ctest -L sanitize in $dir (one process per case; the widget suite under a private Xvfb)"
log=$(mktemp -t sanitize-gate.XXXXXXXX)
start=$(date +%s)
ctest --test-dir "$dir" -L sanitize -j4 --output-on-failure 2>&1 | tee "$log"
rc=${PIPESTATUS[0]}
elapsed=$(( $(date +%s) - start ))
fi

#-------------------------------------------------------------------- 5. say what it found
#
# THE REPORT COUNT FROM THE LOG IS THE INSTRUMENT, FOR BOTH SANITIZERS — and run-cases' `ok` is
# never evidence of sanitizer silence. MEASURED 2026-09-03 on the first ThreadSanitizer run of the
# rack engine: the case printed `WARNING: ThreadSanitizer: data race` and exited 66, and
# run-cases.sh reported `ok 1 case(s)`, because its contract is doctest's summary and the tally, not
# the exit code. MEASURED TWICE, AND THE SECOND IS THE ONE THAT SHOWS THE SIZE OF IT: the whole
# 43-case rack-engine suite through `--each` reported `43/43 ok` while the binary was writing SIX
# ThreadSanitizer blocks, and the silence was nearly filed as a result -- one case under `ok` is a
# missed report, a whole suite under `ok` is a clean bill of health nobody earned (playbook 8aw,
# and backlog BS is the five races that were in it). ASan was covered only because this script
# greps the log itself. So the grep covers
# both, and counts report BLOCKS — each opens with `==<pid>==ERROR:` / `==<pid>==WARNING:` — not
# lines, because ctest --output-on-failure repeats a failed entry's output in its failure summary and
# a line count doubles (one SEGV was reported as "2 reports" that way).
#
# WHY A GREP OVER ctest's LOG SEES A REPORT AT ALL — A CHAIN, AND EVERY LINK IS LOAD-BEARING. This
# script greps the log of `ctest --output-on-failure`, which carries a case's OUTPUT ONLY FOR
# FAILING ENTRIES. So a report is visible only because: `tests/CMakeLists.txt` sets
# `ASAN_OPTIONS=abort_on_error=1:halt_on_error=1` on the sanitize entries, so a report ABORTS the
# case's process; run-cases.sh sees it died on a signal, prints CRASH and DUMPS that process's
# output; the entry therefore exits non-zero; `--output-on-failure` puts the whole dump in the log;
# and the grep finds it there. The entries carry a second, independent link for ASan —
# `FAIL_REGULAR_EXPRESSION "ERROR: AddressSanitizer"`, which reds the entry on the TEXT even if the
# exit code were clean.
#
# THE RULE THAT FALLS OUT OF IT, for whoever edits the entries next: drop `abort_on_error`, admit a
# report class that does not abort, or add an entry for a sanitizer the fail-regex does not name,
# and this gate goes BLIND WHILE PRINTING OK. ThreadSanitizer is exactly that case today — it does
# not halt by default and the regex names AddressSanitizer only — so a TSan sanitize entry MUST set
# `TSAN_OPTIONS=halt_on_error=1` AND extend the regex to `WARNING: ThreadSanitizer`, or this script
# must read run-cases' own output rather than ctest's failure dump. (backlog BO; playbook 8aw.)
#
# AND A REPORT THAT NAMES NONE OF OUR CODE IS NOT A FINDING ABOUT OUR CODE (backlog BO). Twice on
# 2026-09-03, in full gate runs with the widget shards in parallel, an X connection failed to open
# partway through a one-process-per-case run: `SEGV on unknown address` inside XLockDisplay, once
# with a null Display* and once at 0x03e8…, via visage's DisplayLock <- monitorInfoForPosition <-
# defaultDpiScale <- ApplicationWindow::setWindowDimensions <- PluginWindow::create <- Gui::create;
# no frame of ours anywhere. run-cases did its job — CRASH for that one case, every other case
# executed, tally complete — and this script then counted the SEGV line and died. A gate that fails
# spuriously teaches re-rolling until green, which is how a real report gets waved through. So the
# two are told apart HERE, by the one signal that separates them: a genuine report has a frame under
# this repository's src/ in its stack, and an X connection that failed to open never does.
#
# THE CLASSIFICATION IS NARROW AND THE RETRY IS ONE CASE, ONCE. A report is INFRASTRUCTURE only if
# ALL of these hold: (1) it is `SEGV on unknown address` (never matched on the address — the second
# instance was not null); (2) the faulting frame #0 is XLockDisplay; (3) every frame between it and
# the FIRST frame of ours is libX11 or libs/visage, with at least one visage frame among them --
# our code IS in the real stack (Gui::create, PluginWindow::create, printed `../src/...` relative to
# the build dir), below the windowing layer, and "no frame of ours anywhere" would have rejected it;
# running -- named by run-cases' CRASH line or doctest's TEST CASE
# line before the report, or a CRASH line after it (see the note at the read) -- is run
# ONCE MORE by name through run-cases.sh under the venue and passes; (5) that case appears in only
# one report in this run — a second occurrence of the same case in one run is a finding regardless
# of the retry, because a connection that fails twice on one case is not the host's mood. Anything
# else is a finding and the gate dies as before. THE RAW REPORT STAYS IN THE LOG IN FULL: the
# INFRASTRUCTURE line is added, nothing is removed or rewritten, so a real bug wearing a libX11 stack
# can still be read by a person. Frequency when this was written: 2 in ~1,700 ASan case-runs under
# full gates, 0 in ~2,200 ordinary-build case-runs (the fourteen-shard A/B) — rare, and the summary
# line says INFRASTRUCTURE in capitals so it is never read as green-by-default.
#
# SELF-TEST HOOK: `SANITIZE_GATE_CLASSIFY_LOG=<file> scripts/sanitize-gate.sh --no-build` skips the
# run and classifies that log against $dir's binaries (the retry is real). Used to prove each branch
# of this classifier on synthetic logs before it was trusted; see the commit that added it.
reports_all=$(grep -oE '==[0-9]+==(ERROR: AddressSanitizer|WARNING: ThreadSanitizer)' "$log" | sort -u | wc -l)
ran=$(grep -c '^ *[0-9]*/[0-9]* Test' "$log" || true)
infra=0
infra_entries=""
declare -A seen_case
if [[ $reports_all -gt 0 ]]; then
	for marker in $(grep -oE '==[0-9]+==(ERROR: AddressSanitizer|WARNING: ThreadSanitizer)' "$log" | grep -oE '^==[0-9]+==' | sort -u); do
		pid=${marker#==}; pid=${pid%%=*}
		# The block: from this pid's opening line to its SUMMARY line.
		open=$(grep -n -m1 -E "^${marker}(ERROR: AddressSanitizer|WARNING: ThreadSanitizer)" "$log" | cut -d: -f1)
		[[ -n $open ]] || continue
		close=$(awk -v s="$open" 'NR>s && /SUMMARY: (Address|Thread)Sanitizer/ {print NR; exit}' "$log")
		[[ -n $close ]] || close=$open
		block=$(sed -n "${open},${close}p" "$log")
		kind=$(grep -oE '(ERROR: AddressSanitizer|WARNING: ThreadSanitizer): [A-Za-z-]+( race| on unknown address)?' <<<"$block" | head -1)
		echo "   report: $kind (pid $pid, log lines $open-$close)"
		# THE SHAPE, READ OFF THE REAL STACK (gate46.txt), NOT OFF A DESCRIPTION OF IT. Our code IS in
		# that stack -- Gui::create and PluginWindow::create, printed `../src/plugin/...` relative to the
		# build dir -- BELOW visage's windowing layer (DisplayLock, monitorInfoForPosition,
		# setWindowDimensions), which is below XLockDisplay at #0. "No frame of ours anywhere" would
		# have rejected the real instance; the honest test is: the fault is XLockDisplay, and every
		# frame between it and the FIRST frame of ours is libX11 or libs/visage, with at least one
		# visage frame among them (our code never calls XLockDisplay itself -- a stack where ours sits
		# directly under it is not this shape).
		frames=$(grep -E '^[[:space:]]+#[0-9]+ ' <<<"$block")
		ours='(^|[[:space:](])(\.\./)+src/|'"$here"'/src/'
		above=$(awk -v re="$ours" '$0 ~ re {exit} {print}' <<<"$frames")
		if ! grep -q 'SEGV on unknown address' <<<"$block" \
			|| ! head -1 <<<"$frames" | grep -q 'XLockDisplay' \
			|| ! grep -q 'libs/visage' <<<"$above" \
			|| grep -vEq 'libX11|libs/visage' <<<"$above"; then
			continue # a finding: not the shape, or something of ours between the fault and the windowing layer
		fi
		# The entry: the ctest line before the block, which names the binary.
		# THE CASE THAT WAS RUNNING, READ FROM THE LOG'S REAL SHAPE, NOT A MODEL OF IT. On the real log
		# of BO's second instance (gate46.txt) run-cases' `CRASH died on signal N: <name>` line comes
		# BEFORE the dumped case output, and inside that output doctest's own handler prints
		# `TEST CASE:  <name>` and `FATAL ERROR: test case CRASHED: SIGSEGV` ahead of the ASan block;
		# the first cut of this classifier searched forward only and kept a real infrastructure SEGV as
		# a finding (safe, but not the point). So: the nearest line BEFORE the block that names the case
		# — run-cases' CRASH line or doctest's TEST CASE line, whichever is closer — and, failing both,
		# a CRASH line after the block (a process that died before doctest could print anything).
		name=$(awk -v s="$open" 'NR<s && /CRASH died on signal [0-9]+: / {n=$0; sub(/.*CRASH died on signal [0-9]+: /,"",n); ln=NR} NR<s && /^TEST CASE:  / {t=$0; sub(/^TEST CASE:  /,"",t); lt=NR} END{ if (ln>lt) print n; else if (lt>0) print t }' "$log")
		[[ -n $name ]] || name=$(awk -v s="$close" 'NR>s && /CRASH died on signal [0-9]+: / {sub(/.*CRASH died on signal [0-9]+: /,""); print; exit}' "$log")
		entry=$(awk -v s="$open" 'NR<s && /Test +#[0-9]+: sanitize-/ {e=$0} END{sub(/.*Test +#[0-9]+: /,"",e); sub(/ .*/,"",e); print e}' "$log")
		if [[ -z $name || -z $entry ]]; then
			echo "   report: the infrastructure shape, but no CRASH or TEST CASE line names the case — kept as a finding"
			continue
		fi
		if [[ -n ${seen_case[$name]:-} ]]; then
			echo "   NOT infrastructure: \"$name\" crashed TWICE in this run — a finding regardless of retry"
			continue
		fi
		seen_case[$name]=1
		suite=${entry#sanitize-}
		binary="$dir/tests/test_${suite//-/_}"
		[[ -x $binary ]] || { echo "   report: cannot find $binary for $entry — kept as a finding"; continue; }
		echo "   INFRASTRUCTURE? XLockDisplay SEGV through visage's windowing, nothing of ours above it; retrying once by name: $name"
		if "$here/scripts/venue-xvfb.sh" run -- "$here/scripts/run-cases.sh" "$binary" "$name" >/dev/null 2>&1; then
			echo "   INFRASTRUCTURE: retried green — the X connection failed to open under load; not a finding"
			((infra++))
			infra_entries+="$entry "
		else
			echo "   NOT infrastructure: retried and failed again — a finding"
		fi
	done
fi
findings=$((reports_all - infra))
echo
echo "== sanitize-gate: ${elapsed}s, $ran ctest entr$([[ $ran -eq 1 ]] && echo y || echo ies), $reports_all sanitizer report(s): $findings finding(s), $infra INFRASTRUCTURE (retried green)"
if [[ $findings -gt 0 ]]; then
	grep -oE '(ERROR: AddressSanitizer|WARNING: ThreadSanitizer): [A-Za-z-]+' "$log" | sort | uniq -c | sed 's/^/   /'
	grep -m5 -A1 -E 'SUMMARY: (Address|Thread)Sanitizer' "$log" | sed 's/^/   /'
fi
if [[ $ran -eq 0 && -z ${SANITIZE_GATE_CLASSIFY_LOG:-} ]]; then
	# Nothing ran is not a green (playbook §8n-quater). ctest with no matching label exits 0.
	die "ctest ran NO sanitize entries — is $dir configured with FEATHERS_SANITIZE?"
fi
if [[ $findings -gt 0 ]]; then
	die "$findings sanitizer finding(s)"
fi
if [[ $rc -ne 0 ]]; then
	# ctest is red. It is allowed to be red ONLY if every failed entry is one whose reports were all
	# classified INFRASTRUCTURE above — anything else that failed (a plain assertion, a timeout, a
	# crash with no report) is a failure this script has no business excusing.
	failed=$(awk '/^The following tests FAILED:/{f=1; next} f && /^[[:space:]]*[0-9]+ - /{sub(/^[[:space:]]*[0-9]+ - /,""); sub(/ .*/,""); print}' "$log")
	unexplained=""
	for e in $failed; do
		[[ " $infra_entries " == *" $e "* ]] || unexplained+="$e "
	done
	if [[ -n $unexplained ]]; then
		# A RED WITH NO REPORT IS A TEST FAILURE, AND SAYING SO IS THE WHOLE POINT OF THIS BRANCH
		# (backlog BU). The gate's name makes every red read as "the sanitizer found something", and
		# it happened: a run with ZERO reports failed on an ordinary assertion inside
		# sanitize-gui-widgets-explore at JOBS=4, and the natural reading — a sanitizer finding, or
		# "the gate is flaky" — is wrong twice over. What it actually was: a test that fails under
		# contention, which is a finding about the test or the machine and is nobody's licence to
		# re-roll. So name the entry, name the case run-cases already named, and say what to do next.
		#
		# THERE IS DELIBERATELY NO RETRY HERE. The infrastructure retry above exists for reports whose
		# stack contains nothing of ours; extending it to a red case with no report would auto-pass an
		# intermittent REAL defect on a green second run, which is the hazard BO named from the other
		# side. The gate keeps failing; a human re-runs the named entry alone and reads what it says.
		for e in $unexplained; do
			# run-cases names its own verdicts; the entry's dump is between its ctest header and the
			# next one, and FAIL/CRASH lines carry the case after the colon.
			cases=$(awk -v entry="$e" '
				$0 ~ ("Test +#[0-9]+: " entry "([^A-Za-z0-9_-]|$)") {inside = 1; next}
				inside && /Test +#[0-9]+: / {inside = 0}
				inside && /^(FAIL|CRASH) / {
					line = $0
					sub(/^(FAIL|CRASH)[^:]*: /, "", line)
					verdict = ($1 == "CRASH") ? "crashed/timed out" : "failed"
					print "                  " verdict ": " line
				}' "$log" | sort -u)
			echo "   $e — red with no sanitizer report: this is a TEST failure, not a finding." >&2
			[[ -n $cases ]] && echo "$cases" >&2
			# THROUGH ctest, NOT BY HAND, and that is not a style note. A sanitize entry carries an
			# ENVIRONMENT — RUN_CASES_JOBS and a RUN_CASES_TIMEOUT of 2400 — and `run-cases.sh --each
			# <binary>` typed at a shell carries neither, so it is a DIFFERENT INSTRUMENT wearing the
			# same name: run-cases' own default timeout is 120 s. MEASURED 2026-09-04, explore's
			# slowest case ("the terrain is one quad…"): 72 s alone on a quiet host by doctest's own
			# clock, and 143 s under four-way contention — over that 120 s default and nowhere near
			# the entry's 2400. That is where BU's "a case timing out at 120 s" came from: a hand
			# re-run, not the entry. `ctest -R` re-runs the entry WITH its environment, so the
			# instrument is the same one that went red.
			echo "                  ctest --test-dir $dir -R '^$e\$' --output-on-failure" >&2
		done
		echo "   Re-run that entry ALONE, with the command above, before reading anything into it —" >&2
		echo "   under load a case can fail or time out here and pass by itself, and WHICH case moves" >&2
		echo "   between runs (BU). A bare 'run-cases.sh --each' is NOT that entry: it has neither the" >&2
		echo "   entry's JOBS nor its 2400 s per-case timeout, and its own default of 120 s is under" >&2
		echo "   the measured cost of at least one real case. Do not re-roll the whole gate either:" >&2
		echo "   a red that moves is evidence about the test or the host, and re-rolling until green" >&2
		echo "   is how an intermittent real defect becomes 'the gate is flaky'." >&2
		die "ctest exit $rc with $reports_all sanitizer report(s) — failed entries not explained by an infrastructure retry: $unexplained"
	fi
	echo "sanitize-gate: OK (ctest exit $rc explained entirely by $infra INFRASTRUCTURE report(s), each retried green)"
	[[ -n ${SANITIZE_GATE_CLASSIFY_LOG:-} ]] || rm -f "$log"
	exit 0
fi
[[ -n ${SANITIZE_GATE_CLASSIFY_LOG:-} ]] || rm -f "$log"
echo "sanitize-gate: OK"
