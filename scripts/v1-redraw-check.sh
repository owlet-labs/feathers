#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation,
# either version 3 of the License, or (at your option) any later version.
#
# Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
# without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with Feathers. If not, see
# <https://www.gnu.org/licenses/>.
#
# DOES visage v1.0.0 NEED FEATHERS_FULL_REDRAW ON A REAL GPU? (visage v1 port, 2026-11-05)
#
# Run ON THE HOST, against a real display (default :1). venue-xvfb cannot answer this: lavapipe on
# Xvfb acquires one swap-chain image at age 1 every frame, so v1.0.0's per-buffer damage history is
# never consulted, and a known-bad deleting it stayed green there (assessment doc §10.3).
#
# FULL_REDRAW is OFF in both builds. Upstream's "X11 windows show every change when only part of the window
# redraws" is run as a good build (build-vtests-good) and as a KNOWN-BAD (build-vtests-kb, per-buffer
# damage history deleted, fork option FEATHERS_KNOWN_BAD_NO_DAMAGE_HISTORY).
#
# v2 (2026-11-05). v1 of this script on :1 had the GOOD build failing at step 0 as well, so the venue
# was invalid rather than evidence. The good build must now pass 3/3 before the known-bad is read
# (exit 3 = venue invalid, 4 = blind, 5 = inconclusive, 0 = valid). The widget-shot pair is gone:
# :1 returned identical good and known-bad shots, because a still capture shows the last presented
# buffer and cannot express this fault class.
#
#   scripts/v1-redraw-check.sh [display] [out-dir]      # defaults :1 and /tmp/v1-redraw-check
#   V1RC_DRY_RUN=1 scripts/venue-xvfb.sh run -- scripts/v1-redraw-check.sh   # exercise it, blind
set -u
cd "$(dirname "$0")/.." || exit 2
INHERITED=${DISPLAY:-}
OUT=${2:-/tmp/v1-redraw-check}

if [[ ${V1RC_DRY_RUN:-0} == 1 ]]; then
	# A DRY RUN USES ONLY THE PRIVATE DISPLAY ITS OWN venue-xvfb MADE, never one named by hand.
	# 2026-11-05: a dry run was handed :0 while its venue-xvfb had come up on :1, so its windows
	# opened on another session's private Xvfb. So: it must be inside venue-xvfb (which points Vulkan
	# at lavapipe and the config at a scratch dir), and a display argument, if given, must be the one
	# that venue-xvfb exported.
	if [[ -z $INHERITED || ${VK_DRIVER_FILES:-} != *lvp* || ${XDG_CONFIG_HOME:-} != */feathers-venue-config.* ]]; then
		echo "dry run refused: not inside scripts/venue-xvfb.sh run -- (no private display to use)"
		exit 2
	fi
	if [[ -n ${1:-} && $1 != "$INHERITED" ]]; then
		echo "dry run refused: asked for $1, but this venue-xvfb's display is $INHERITED"
		exit 2
	fi
	D=$INHERITED
else
	D=${1:-:1}
	# The real run goes through venue-preflight, which refuses a display without DPMS: an Xvfb, ours or
	# anyone's, can never pass it.
	scripts/venue-preflight.sh "$D" || { echo "display $D is not usable; nothing run"; exit 2; }
fi
export DISPLAY="$D"
mkdir -p "$OUT" || exit 2

# WHAT THE DESKTOP IS, recorded first: a 30-bit root visual breaks the test's 24-bit pixel compare,
# and a compositor/WM decides when the test window is really mapped.
{
	echo "--- display $D"
	xdpyinfo 2>/dev/null | grep -E 'dimensions|resolution|depth of root|default visual id' | sed 's/^ *//'
	xprop -root _NET_SUPPORTING_WM_CHECK _NET_WM_NAME 2>/dev/null
	xprop -root 2>/dev/null | grep -iE '^_NET_WM_CM|XFree86_has_VT|_MUTTER|_GNOME' | head -5
} | tee "$OUT/desktop.txt"

run_partial() { # $1 = good|kb
	scripts/venue-claim.sh run v1-redraw-check -- "build-vtests-$1/visage_app/VisageIntegrationTests" \
		"X11 windows show every change when only part of the window redraws" >"$OUT/partial-$1.log" 2>&1
	local rc=$?
	local step
	step=$(grep -oE 'failed_step == -1 \)|[0-9-]+ == -1' "$OUT/partial-$1.log" | grep -oE '^[0-9-]+' | tail -1)
	echo "$rc ${step:-none}"
}

# V2 PRE-CHECK (2026-11-05, after v1 on :1 returned GOOD and KNOWN-BAD both failing at step 0): step 0
# is the FIRST full draw — red left, blue right — read back with XGetImage. A good build failing THERE
# says the instrument cannot read this desktop, not anything about partial present. So the good build
# must pass, three times, before the known-bad's result is read at all.
for r in 1 2 3; do
	read -r rc step <<<"$(run_partial good)"
	cp "$OUT/partial-good.log" "$OUT/partial-good-$r.log"
	echo "pre-check good #$r: exit $rc, failed at step ${step}"
	if [[ $rc != 0 ]]; then
		echo "VENUE INVALID: the GOOD build fails the partial-present test on $D (step $step; 0 = the first"
		echo "full draw was never read back). Nothing about full redraw can be concluded here. See $OUT/desktop.txt."
		exit 3
	fi
done

read -r rc step <<<"$(run_partial kb)"
echo "known-bad: exit $rc, failed at step ${step}"
if [[ $rc == 0 ]]; then
	echo "VENUE BLIND: the KNOWN-BAD passes too, so this display reuses one swap-chain image (as Xvfb does)."
	echo "A green good build here says nothing; FULL_REDRAW stays ON."
	exit 4
fi
if [[ $step == 0 ]]; then
	echo "INCONCLUSIVE: the known-bad fails at step 0, where the good build passes; history is not used at step 0."
	exit 5
fi
echo "VENUE VALID: good passes 3/3, known-bad fails at step $step. On this display v1.0.0's partial present"
echo "is sound without FULL_REDRAW (for this test's pattern)."
