#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# A PRIVATE DISPLAY FOR WINDOWED RUNS — one that cannot be locked, obscured, or collided with.
#
# `:1` IS THE REAL DESKTOP AND THAT IS THE WHOLE PROBLEM. It locks (twice in one session on
# 2026-08-30, blocking a re-shoot for hours), it can be obscured, and several sessions photograph on
# it at once — which `venue-claim.sh` books but cannot enforce. Every one of those failures is SILENT:
# `import` exits 0 and hands back a frame. `venue-preflight.sh` says so in its own words and names
# this remedy: *"Unlock, or run captures on an Xvfb of their own."*
#
# So this is that. Xvfb was installed on explicit approval 2026-08-30 after the second lock.
#
# --------------------------------------------------------------------------------------------------
# TWO THINGS ARE REQUIRED AND NEITHER IS OPTIONAL. THE VENUE IS WRONG WITHOUT THEM, NOT MERELY SLOW.
# --------------------------------------------------------------------------------------------------
#
# 1. `VK_DRIVER_FILES` POINTING AT LAVAPIPE. `visage_graphics/renderer.cpp:180` HARDCODES
#    `RendererType::Vulkan` on Linux — there is no env override and no runtime choice — and the
#    hardware Vulkan driver needs DRI3 for presentation, which Xvfb does not provide. MEASURED: the
#    run dies with `vulkan: No DRI3 support detected - required for presentation` and widget-shot's
#    own colour guard REFUSES ("1 colours — not a drawn frame"). Lavapipe is Mesa's software Vulkan
#    ICD and presents fine.
#
# 2. `VISAGE_FULL_REDRAW=1`, AND THIS IS THE ONE THAT WOULD HAVE SHIPPED A LIE. Without it visage
#    repaints only damaged regions, and on lavapipe's slower rasterisation the capture RACES the
#    repaint. MEASURED 2026-08-30: a `--preview lfo` shot came back showing the orange cutoff trace
#    that variant A never draws, over a dashed blue waveform — remnants of the frame drawn before
#    `setPreview`. **The colour guard passed (6329 colours) and the two states' md5s differed.** Every
#    check we have said the frame was good and the picture was wrong. With full redraw the same shot
#    matches `:1` at RMSE 0.00067.
#
#    **ON visage v1.0.0 (2026-11-05) THIS VARIABLE DOES NOTHING.** v1.0.0 reads no `VISAGE_FULL_REDRAW`;
#    the export below is kept only so a build of the old pin still gets it. Full redraw is now the
#    BUILD option `-DFEATHERS_FULL_REDRAW=ON` in our fork (every layer invalidated in
#    `Canvas::submit`), and an ordinary build here has it OFF. The race above was the old bgfx
#    intermediate-layer fault and did not reproduce on v1.0.0 without full redraw: `--preview lfo`
#    and `dual` were byte-stable over 3 runs each and showed no remnant. **BUT THIS VENUE IS BLIND
#    TO v1.0.0's OWN DAMAGE CLASS**: lavapipe on Xvfb acquires one swap-chain image at age 1 every
#    frame, so the per-buffer damage history is never consulted, and a known-bad that deletes it
#    (fork option FEATHERS_KNOWN_BAD_NO_DAMAGE_HISTORY) stays GREEN here, as it did on `:1`.
#    A green from this venue says nothing about partial present.
#
# --------------------------------------------------------------------------------------------------
# WHAT WAS PROVEN BEFORE ANYTHING WAS ALLOWED TO RELY ON IT (2026-08-30, automod's LFO strip).
# --------------------------------------------------------------------------------------------------
#
# 1. IT DRAWS THE SAME CARD AS `:1`. Same shot, both venues, cropped to the card:
#
#        same state, different venue     RMSE 0.00067  (LFO)   0.00070  (with corner)
#        different state, same venue     RMSE 0.041    (:1)    0.051    (Xvfb)
#
#    Venue noise sits ~60x below the signal we measure. NOT byte-identical and it never will be —
#    different rasteriser — so compare the CARD, never the whole frame: whole-frame RMSE is dominated
#    by the animated starfield and put cross-venue noise (0.0115) level with the state difference
#    (0.0126), which would have made this proof look like a failure for the wrong reason.
#
#    **COMPARE THE REGION YOU ARE MAKING A CLAIM ABOUT, NOT THE FRAME IT SITS IN.** The claim here is
#    about one card; every other pixel in the window is a louder, irrelevant variable, and averaging
#    over them buries the thing being measured under the thing that happens to move most.
#
# 2. IT CAN GO RED. Pointed at the `kBottomPerRow = 6` known-bad, the label-fit case fails here and
#    names the numbers — `LFO MODE` at 53.32px in a 36px cell, a 38px face in the same. A venue shown
#    only able to PASS is not an instrument.
#
# 3. IT CANNOT BE FOOLED THE WAY `:1` WAS — with one exception, stated below. Two different states
#    give different md5s and 6245 / 6556 colours; the whole `test_gui_widgets` suite runs 499/499.
#
# 4. WHAT IT STILL CANNOT PROMISE:
#    - **A BLACK ROOT WINDOW IS NORMAL HERE.** Nothing draws an Xvfb root, so it is 1 colour by
#      design — which is the EXACT signature that detected the lock on `:1`. That check is dead in
#      this venue. What still works, and what caught the Vulkan failure above, is counting colours on
#      the CAPTURED WINDOW, which `widget-shot` already does and refuses on.
#    - Software rasterisation, so it is not a test of the GPU path. `:1` is radeonsi/Vulkan 4.6; this
#      is lavapipe. A defect that only appears on real hardware will not appear here.
#    - Geometry and DPI are pinned below to match `:1` exactly (1920x1080, 96x96, depth 24). If
#      the desktop display changes, this must too, or every pixel measured here is about a display nobody
#      uses.
#    - NEITHER VENUE IS BITWIG. `widget-shot` renders at 1x — the capture is 1240x700, exactly
#      `Gui::kLogicalWidth x kLogicalHeight` — so pixel counts from here are 1:1 logical and are NOT
#      what is seen at the real editor scale.
#
# --------------------------------------------------------------------------------------------------
# AND IT GIVES THE RUN A SCRATCH `XDG_CONFIG_HOME`, WHICH IS NOT HYGIENE — IT IS A MEASURED BUG.
# --------------------------------------------------------------------------------------------------
#
# The editor persists its preferences to `~/.config/feathers/editor.conf`: **the user's file, shared
# by every worktree and by the plugin the user plays.** Windowed tools reach it.
#
# TWO INCIDENTS, BOTH 2026-08-30, AND THE SECOND IS THE ONE THAT MATTERS:
#
#   - A measurement arm collapsed the fx lane, which persisted `rail.fx.collapsed=1`. Every later run
#     started with the lane collapsed — cards 236px wide became 14px, the cost fell 5x, **and it read
#     as a result.** Caught only because reps disagreed and the card width was in the log.
#
#   - `ModFixture` calls `setPopoutWindowForTest(false)`, and `EditorPrefs::setFlag` SAVES. So **every
#     `ctest` anybody runs on this machine writes `popout.window=0` into the user's config** — the default
#     is `true` and nothing in the editor's UI writes that key, so the value could not have been the user's
#     choice. Expanded panels were opening inline instead of as floating windows, on a build we had
#     just installed, with no way to trace it to a test fixture.
#
# **IT IS WORSE THAN A DIRTY WORKING TREE IN TWO SPECIFIC WAYS**: it lives outside the repo, so
# `git status` is clean and switching worktrees does not escape it — and it is SHARED WITH THE USER,
# so a run can silently reconfigure the editor the user opens next.
#
# The deep fix is that a `ForTest` seam must not reach a persistent store, and that is a decision
# about `gui.cpp` rather than something a venue can patch. What a venue CAN do is make the whole class
# unreachable from here: anything run through this script gets its own config directory, and no
# discipline is required of the person running it.
#
# `venue-claim.sh` STAYS for anything that genuinely needs the real desktop — that list is now short.
# Nothing here books `:1`, so this may run beside somebody else's windowed work.
#
# Usage:
#   scripts/venue-xvfb.sh run -- <command...>   # hold a private display for the command's lifetime
#   scripts/venue-xvfb.sh check                 # preconditions and geometry, without running anything
set -u

# MATCHING `:1` AS MEASURED, not as assumed — `xdpyinfo` on the desktop display, 2026-08-30.
GEOMETRY="1920x1080x24"
DPI=96
# A LARGER SCREEN ON REQUEST, for a capture of a big editor (terrain-render-bugs: 3440x1440, 5120x2880) — the pinned
# geometry above is still the default, and a run that sets this is no longer "matching :1".
if [ -n "${FEATHERS_XVFB_GEOMETRY:-}" ]; then
	GEOMETRY="$FEATHERS_XVFB_GEOMETRY"
fi
ICD="/usr/share/vulkan/icd.d/lvp_icd.json"

fail() {
	echo "[xvfb] NOT READY: $1" >&2
	exit 1
}

preconditions() {
	command -v Xvfb >/dev/null 2>&1 || fail "Xvfb is not installed (apt install xvfb)."
	# THE ICD IS CHECKED BY PATH RATHER THAN BY ASKING VULKAN, because the failure it prevents happens
	# inside a child process minutes later and reports as a blank frame.
	[ -r "$ICD" ] || fail "the lavapipe ICD is missing at $ICD — visage is Vulkan-only on Linux and
                 the hardware driver cannot present on Xvfb (no DRI3)."
}

# A DISPLAY NOBODY ELSE IS ON -- CHOSEN BY THE SERVER, NOT BY THIS SCRIPT.
#
# The first version asked `xdpyinfo` whether :99 (then :98 ...) was free and started `Xvfb :99` if
# so. That is check-then-act, and two invocations can pass the check together: both start `Xvfb :99`,
# the loser's server exits on the lock, and the loser's readiness poll then sees the WINNER's server
# up and runs on it. When the winner finishes, its cleanup kills that server and the loser's suite
# loses its display mid-run. MEASURED 2026-09-02 (backlog AU): a gate in one worktree and `ctest` in
# the main workspace ran their widget suites at once and gui-windowed, under a minute alone, FAILED
# after 1015 s. The venue's own header promised "cannot be collided with"; it could, with itself.
#
# `Xvfb -displayfd N` has the server take the first free display number under its own lock and
# write the number to fd N only once it is ready to accept connections. One process picks and one
# process owns, so there is nothing to race and nothing to poll: when the number arrives, the server
# it names is ours and is up. The number is whatever is free counting up from 0, not 99..95 -- read
# it from the "[xvfb] :N up" line, never assume it.

ACTION="${1:-check}"

if [ "$ACTION" = "check" ]; then
	preconditions
	Xvfb -help 2>&1 | grep -q -- '-displayfd' || fail "this Xvfb has no -displayfd; the venue cannot claim a display safely."
	echo "[xvfb] ready: Xvfb present with -displayfd, lavapipe ICD present, would run $GEOMETRY @ ${DPI}dpi"
	exit 0
fi

[ "$ACTION" = "run" ] || fail "usage: venue-xvfb.sh run -- <command...>  |  venue-xvfb.sh check"
shift
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || fail "nothing to run."

preconditions

# The display number arrives on fd 3, through a file of our own: Xvfb writes "N\n" there when the
# server is ready, and nothing else ever will. Polled on a bound rather than slept for -- a fixed
# sleep is either slower than it needs to be or, on a loaded host, not long enough, and the second
# failure looks exactly like a broken build. 60 s because a loaded host has taken 10+ s to start one.
DISPLAYFD_FILE=$(mktemp -t feathers-venue-display.XXXXXXXX) || fail "could not make a display-number file."
Xvfb -displayfd 3 -screen 0 "$GEOMETRY" -dpi "$DPI" -nolisten tcp 3>"$DISPLAYFD_FILE" >/dev/null 2>&1 &
XVFB_PID=$!

NUM=""
for _ in $(seq 1 300); do
	NUM=$(tr -d '[:space:]' <"$DISPLAYFD_FILE")
	[ -n "$NUM" ] && break
	if ! kill -0 "$XVFB_PID" 2>/dev/null; then
		break
	fi
	sleep 0.2
done
rm -f "$DISPLAYFD_FILE"
if [ -z "$NUM" ]; then
	kill "$XVFB_PID" 2>/dev/null
	fail "Xvfb never reported a display number (did it die? is -displayfd supported?)."
fi
# It said it was ready; hold it to that, because everything downstream trusts this line.
xdpyinfo -display ":$NUM" >/dev/null 2>&1 || {
	kill "$XVFB_PID" 2>/dev/null
	fail "Xvfb reported :$NUM but the display does not answer."
}

# TORN DOWN WHENEVER THIS SCRIPT LEAVES, including on interrupt — an orphaned Xvfb holds its display
# number and the next run silently picks a different one, which is how two sessions end up measuring
# on displays they did not configure.
# A CONFIG DIRECTORY OF ITS OWN, torn down with the display. `mktemp -d` rather than a fixed path so
# two concurrent runs cannot share one — the point is isolation, and a shared scratch would reproduce
# the bug in miniature.
SCRATCH_CONFIG=$(mktemp -d -t feathers-venue-config.XXXXXXXX) || fail "could not make a scratch config dir."
# AND A SCRATCH DATA DIR, FOR THE SAME REASON AND THE ONE THIS VENUE MISSED. Preferences are XDG
# CONFIG; the PRESET LIBRARY is XDG DATA (see preset_store.h), and redirecting one and not the other
# leaves every tool that saves a preset writing into the user's library. `test_gui_widgets`'s main
# already learned this and says so in as many words — this venue had only half of it. SINCE BACKLOG FO
# PHASE 2 the preset library is ~/Documents/Owlet/Feathers/Presets, not XDG data, so it is redirected by
# its own variable, FEATHERS_PRESET_ROOT, into the same scratch dir.
SCRATCH_DATA=$(mktemp -d -t feathers-venue-data.XXXXXXXX) || fail "could not make a scratch data dir."

cleanup() {
	kill "$XVFB_PID" 2>/dev/null
	wait "$XVFB_PID" 2>/dev/null
	rm -rf "$SCRATCH_CONFIG" "$SCRATCH_DATA"
}
trap cleanup EXIT INT TERM

echo "[xvfb] :$NUM up ($GEOMETRY @ ${DPI}dpi, lavapipe, full-redraw, scratch config+data) for: $*"
DISPLAY=":$NUM" \
	XDG_CONFIG_HOME="$SCRATCH_CONFIG" \
	XDG_DATA_HOME="$SCRATCH_DATA" \
	FEATHERS_PRESET_ROOT="$SCRATCH_DATA/presets" \
	FEATHERS_IR_ROOT="$SCRATCH_DATA/irs" \
	VK_DRIVER_FILES="$ICD" \
	VK_ICD_FILENAMES="$ICD" \
	VISAGE_FULL_REDRAW=1 \
	"$@"
STATUS=$?
echo "[xvfb] :$NUM down, command exited $STATUS"
exit "$STATUS"
