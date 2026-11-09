#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# IS THE VENUE ACTUALLY AWAKE? Run this before any windowed run, and before believing either its
# hang or its clean result.
#
# THE SIXTH WAY A RUN CAN LIE, and it cost most of an afternoon. Every windowed guihost run was
# hanging immediately after the editor opened — main thread parked in a futex beside Mesa's WSI
# swapchain threads — and it reproduced on binaries built that morning AND on one built a day
# earlier. That looked like conclusive evidence of an environmental wedge, and it was written up as
# a possible deadlock-on-open worth escalating.
#
# The machine was ASLEEP. A suspended machine parks swapchain presents in exactly that futex wait,
# on a binary of any age, which is why the "it reproduces on old binaries too" reasoning pointed
# firmly at the wrong conclusion: it is equally true of a real regression and of a sleeping laptop.
# Waking the display and re-running gave a clean pass on the first try.
#
# So a windowed run has two failure modes that look identical from the outside — the code is broken,
# and the screen is asleep — and neither the exit code nor the log distinguishes them. This does.
# It matters in BOTH directions: a hang is not evidence of a defect until the venue is known awake,
# and a clean negative is not evidence of absence either, since a display that never presented
# cannot show a glitch.
#
# Usage:
#   scripts/venue-preflight.sh              # check :1 (or $DISPLAY), waking it
#   scripts/venue-preflight.sh :1           # check a named display
#
# Exit 0 = venue believed awake and answering. Non-zero = do not believe what a windowed run says.

set -u

DISPLAY_NAME="${1:-${DISPLAY:-:1}}"
export DISPLAY="$DISPLAY_NAME"

fail() {
	echo "[venue] NOT READY: $1" >&2
	echo "[venue] Do not read a hang OR a clean run on this display as evidence of anything." >&2
	exit 1
}

# 1. WAKE IT FIRST, then ask. Asking first would report the sleeping state we are trying to leave,
# and the wake is harmless on a display that is already on.
xset -dpms s off dpms force on >/dev/null 2>&1

# 2. DOES THE SERVER ANSWER, and answer promptly? A bounded call, because an X server that is going
# to hang will hang here rather than inside the run being set up.
if ! timeout 15 xdpyinfo >/dev/null 2>&1; then
	fail "$DISPLAY_NAME did not answer xdpyinfo within 15s"
fi

# 3. IS THE MONITOR ON? This is the check that would have caught the afternoon above. `xset q`
# reports the DPMS state, and "Monitor is On" is the difference between a venue that can present a
# frame and one that will park the presenting thread.
MONITOR=$(timeout 10 xset -q 2>/dev/null | grep -o "Monitor is .*" | head -1)
case "$MONITOR" in
	"Monitor is On") ;;
	"") fail "could not read the DPMS state from xset — treat the venue as unknown" ;;
	*)  fail "$MONITOR — the display is not presenting, so nothing windowed can be believed" ;;
esac

# 2b. AND IS ANYTHING COVERING IT? — the EIGHTH way a run can lie, and the first one a HUMAN causes
# rather than the machine. Found 2026-08-28 when the question arose whether the lock screen was interfering
# with the screengrabs agents take while the desktop is unattended. It was.
#
# X's own blanking is OFF on this host — `xset q` reports screensaver timeout 0, every DPMS timeout
# 0, and "Monitor is On" — so every check above passes. But `:1` is a REAL logged-in Xorg session,
# not an Xvfb, and GNOME's lock is a separate mechanism entirely: `org.gnome.desktop.session
# idle-delay` is 300 and `lock-enabled` is true, so after five idle minutes a lock surface is drawn
# OVER everything.
#
# WHAT THAT DOES TO A CAPTURE: `import -window <id>` on X11 without composite redirection grabs what
# is ON SCREEN in that region, so a covered window yields the lock surface or black — while `import`
# still exits 0 and writes a valid PNG. That is the measured ~8% blank-capture rate, and it spikes
# exactly when nobody is watching, which is when the long runs happen.
#
# AND IT POISONS NEGATIVES SILENTLY: for an ink-FINDING probe a blank frame contains no ink, which
# reads as a clean result. Two "0 ink findings" storms in the header-tear investigation were run
# unattended. See the playbook: distinct hashes catch an ASLEEP window and cannot catch a BLANK one.
# 2a. IS SOMEBODY ELSE USING IT? — the NINTH way a run can lie, and the only one another session
# causes. `:1` is one shared display and several sessions photograph on it. Two windowed runs that
# overlap do not merely fail: they CORRUPT EACH OTHER, in both directions, with no exit code
# changing — extra windows in their captures, their windows in yours.
#
# MEASURED 2026-08-28: a session began a seven-cycle probe at 24 minutes a cycle and broadcast a
# hold. The notice arrived AFTER the integrator had started a full `ctest`, which opens real windows
# in two suites; a cycle had to be discarded. Nobody was careless — a broadcast simply loses that
# race eventually. See `scripts/venue-claim.sh`, which is where the reservation lives; this is only
# the place it is enforced, because a check every windowed run already makes is the right gate.
if [ -x "$(dirname "$0")/venue-claim.sh" ]; then
	HOLDER="$("$(dirname "$0")/venue-claim.sh" who "$DISPLAY_NAME" 2>/dev/null || true)"
	HOLDER_PID="$(echo "$HOLDER" | sed -n 's/.*(pid \([0-9]*\)).*/\1/p')"
	# A HOLDER THAT IS US IS NOT A COLLISION. `venue-claim.sh run` exports its pid, so a command
	# running under its own claim passes here and keeps every other check in this file. Without this
	# the reservation deadlocks against itself — see the note in venue-claim.sh.
	# IS THE HOLDER US, OR SOMETHING WE ARE RUNNING UNDER? Two roads reach here and both must pass.
	# `run` exports its pid; `take <name> <pid>` books on behalf of a job that exports nothing, and
	# that job is our ANCESTOR when it calls preflight. Recognising the holder rather than trusting
	# one action to announce itself covers both — render-ghost's point, and they are right that a fix
	# teaching only `run` to exempt itself would leave `take` deadlocked, which is the path an agent
	# was told to use after a collision.
	if [ -n "$HOLDER_PID" ]; then
		probe=$$
		while [ -n "$probe" ] && [ "$probe" != "0" ] && [ "$probe" != "1" ]; do
			if [ "$probe" = "$HOLDER_PID" ]; then
				HOLDER=""
				break
			fi
			probe="$(ps -o ppid= -p "$probe" 2>/dev/null | tr -d ' ')"
		done
	fi
	if [ -n "$HOLDER" ] && [ "$HOLDER_PID" = "${FEATHERS_VENUE_HOLDER:-}" ]; then
		HOLDER=""
	fi
	if [ -n "$HOLDER" ]; then
		fail "$DISPLAY_NAME is claimed by $HOLDER — a windowed run here corrupts theirs and theirs corrupts yours. Wait, or use another display."
	fi
fi

if command -v gdbus >/dev/null 2>&1; then
	LOCKED=$(gdbus call --session --dest org.gnome.ScreenSaver --object-path /org/gnome/ScreenSaver \
		--method org.gnome.ScreenSaver.GetActive 2>/dev/null || true)
	case "$LOCKED" in
		*true*) fail "the screen is LOCKED — a capture here photographs the lock surface or black, and import will still exit 0. Unlock, or run captures on a private display: scripts/venue-xvfb.sh run -- <command>" ;;
	esac
fi

echo "[venue] $DISPLAY_NAME awake: xdpyinfo answered, $MONITOR"

# 4. AND OPTIONALLY THE REAL TEST, which is a frame actually reaching the screen. X answering says
# the server is alive; it says nothing about the swapchain, and the swapchain is what slept. Pass a
# guihost and a plugin to have it proved rather than assumed — a bounded plain run that must reach
# its own RESULT line.
if [ $# -ge 3 ]; then
	GUIHOST="$2"
	PLUGIN="$3"
	OUT=$(mktemp -d)
	if ! timeout 120 "$GUIHOST" "$PLUGIN" "$OUT" 2>/dev/null | grep -q "RESULT:"; then
		rm -rf "$OUT"
		fail "the display answers but a plain guihost run did not complete — the swapchain, not the server"
	fi
	echo "[venue] and a real editor presented and completed within 120s"

	# 5. AND THE CAPTURES MUST DIFFER FROM ONE ANOTHER — the SEVENTH way a run can lie, found by
	# impl-ui-7 while measuring zoom.
	#
	# A run can complete, write every file it promised, and still have photographed nothing that moved:
	# a window that is frozen, obscured by another window, or composited away yields captures that are
	# BYTE-IDENTICAL to each other. Every frame then compares equal, every pixel assertion passes, and
	# the result reads as "no glitch" when it is really "no evidence". That is the same shape as the
	# 1x1-helper trap in bitwighost and the three dead storms in drag-storm.sh: a probe that reached
	# nothing looks exactly like a probe that found nothing wrong.
	#
	# guihost's own captures are taken either side of a parameter change and at a second window size,
	# so at least two of them MUST differ. If they are all the same file, the venue is not presenting
	# live content whatever X and DPMS claim.
	SHOTS=$(find "$OUT" -name '*.png' -type f 2>/dev/null | wc -l)
	DISTINCT=$(find "$OUT" -name '*.png' -type f -exec md5sum {} + 2>/dev/null | awk '{print $1}' | sort -u | wc -l)
	rm -rf "$OUT"

	if [ "$SHOTS" -eq 0 ]; then
		fail "the run completed but wrote no captures — is ImageMagick's 'import' installed?"
	fi
	if [ "$DISTINCT" -lt 2 ]; then
		fail "all $SHOTS captures are byte-identical — the window is frozen, obscured or composited away, so nothing photographed here is evidence"
	fi
	echo "[venue] and its captures are live: $DISTINCT distinct of $SHOTS"
fi
