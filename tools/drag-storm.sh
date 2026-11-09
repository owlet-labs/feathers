#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Drive a held-open guihost editor with rapid drags inside the blend disk, to chase the mid-drag
# visual corruption hit twice in Bitwig.
#
#   FEATHERS_GUIHOST_HOLD=50 build-dbgpanel/guihost build-dbgpanel/feathers.clap /tmp/out > hold.log &
#   tools/drag-storm.sh hold.log
#
# THREE STORMS WERE RUN BEFORE THIS SCRIPT EXISTED AND ALL THREE WERE INVALID, which is the only
# reason it is worth checking in. Each failure mode is guarded here:
#
#   - the press landed OUTSIDE the blend disk, which PerformStage ignores by design, so nothing moved;
#   - the window was never resolved, so xdotool aimed at whatever had focus;
#   - and the one that cost the most: pointer events at screen coordinates go to the window stacked
#     under the pointer, which resolves to guihost's PARENT rather than the plugin's editor child.
#     `import -window` captures the child's own pixels either way, so a storm that reached nothing
#     produced a screenshot indistinguishable from one that worked. Targeting the editor window
#     directly with window-relative coordinates is what fixed it.
#
# VERIFY THE STORM BEFORE BELIEVING ITS RESULT: read PATH and POS X/Y off the captured frame. If they
# have not moved, the storm did not happen and a clean run means nothing.
#
# AND THE FOURTH FAILURE MODE, added after it cost an afternoon: THE MACHINE WAS ASLEEP. A suspended
# machine parks swapchain presents in a futex wait, so a run hangs just after the editor opens and
# looks exactly like a wedged environment — on binaries of any age, which is what makes the obvious
# "but it reproduces on old builds too" reasoning point confidently at the wrong answer. It poisons
# clean results as surely as hangs: a display that never presented cannot show a glitch either.
#
# AND THE FIFTH, which had been live in this file the whole time: THE CAPTURE WENT NOWHERE. The
# destination below was a hard-coded scratchpad from a different, older session; that directory no
# longer exists, so `import` failed every time, `2>/dev/null` ate the message, and the script
# announced "captured" regardless. Every storm run from this script since that path died
# photographed NOTHING and said it was fine. See `shot` above — same shape as the three at the top:
# a run that reached nothing looking exactly like one that worked.

# ---------------------------------------------------------------------------------------------
# TAKE A SHOT, OR SAY WHY NOT. Every capture in this file used to be `import ... 2>/dev/null`
# with the exit status thrown away, which fails in two ways that both LOOK like success:
#
#   - THE DESTINATION DID NOT EXIST. Both storms wrote to a hard-coded scratchpad belonging to a
#     DIFFERENT, OLDER SESSION (another project'''s session directory, 03484d95-...). That directory is
#     gone, so `import` failed on every capture, stderr went to /dev/null, and the script still
#     announced "captured". These storms have been photographing NOTHING.
#   - THE FRAME CAME BACK BLANK. `import` returns 0 for an all-black grab of a window that was not
#     ready. Measured on this venue with guihost: 1 in 12 fresh-window captures, deviation exactly
#     0 against 0.068-0.073 for real frames.
#
# So: the output directory is made, the status is checked, and a frame that is a single flat
# colour is refused. Threshold 0.001 is MEASURED, two orders of magnitude below the real floor.
# Override the destination with FEATHERS_STORM_OUT.
STORM_OUT="${FEATHERS_STORM_OUT:-/tmp/feathers-storm}"
mkdir -p "$STORM_OUT" || { echo "[storm] cannot create $STORM_OUT" >&2; exit 1; }
shot() {
  local win="$1" dest="$2" err
  err="$(import -window "$win" "$dest" 2>&1)" || {
    echo "[storm] CAPTURE FAILED for $dest: ${err:-import exited non-zero}" >&2
    return 1
  }
  if [ "$(convert "$dest" -colorspace Gray -format '%[fx:standard_deviation>0.001]' info: 2>/dev/null)" != "1" ]; then
    echo "[storm] CAPTURE IS A SINGLE FLAT COLOUR: $dest — the window was not ready, and a blank" >&2
    echo "[storm] frame passes every other check this script makes" >&2
    return 1
  fi
  return 0
}

LOG="$1"
SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$SELF_DIR/../scripts/venue-preflight.sh" || exit 1
for i in $(seq 1 60); do sleep 0.5; grep -q "holding:  " "$LOG" && break; done
WID=$(grep -oP 'holding:.*window \K0x[0-9a-f]+' "$LOG" | head -1)
[ -z "$WID" ] && { echo "[storm] no window in log" >&2; exit 1; }
eval $(xdotool getwindowgeometry --shell "$((WID))" 2>/dev/null)
echo "[storm] window $WID geom ${WIDTH}x${HEIGHT} at $X,$Y" >&2
# The stage sits right of the Engine rail+stack rails and left of the audition strip; its disk is
# centred in the band above the knob tray. These are fractions of the editor, not guesses at pixels.
CX=$((X + WIDTH*62/100)); CY=$((Y + (HEIGHT-110)*48/100))
echo "[storm] disk centre approx $CX,$CY" >&2
# RAISE AND FOCUS FIRST. A pointer event goes to whatever window is stacked under the pointer, and
# `import -window` captures the window's own pixels either way — so a storm against a covered window
# looks identical to a storm that worked. Two runs were lost to that before this line existed.
xdotool windowraise "$((WID))" 2>/dev/null
xdotool windowactivate --sync "$((WID))" 2>/dev/null
sleep 1
xdotool mousemove $CX $CY 2>/dev/null
UNDER=$(xdotool getmouselocation --shell 2>/dev/null | grep -oP 'WINDOW=\K[0-9]+')
echo "[storm] window under pointer: $UNDER (want $((WID)))" >&2
for pass in $(seq 1 25); do
  RX=$((WIDTH*62/100)); RY=$(((HEIGHT-110)*48/100))
  xdotool mousemove --window "$((WID))" $RX $RY >/dev/null 2>&1
  xdotool mousedown --window "$((WID))" 1 >/dev/null 2>&1
  for i in $(seq 0 24); do
    DX=$(( ((i*37) % 120) - 60 )); DY=$(( ((i*53) % 100) - 50 ))
    xdotool mousemove --window "$((WID))" $((RX+DX)) $((RY+DY)) >/dev/null 2>&1
  done
  xdotool mouseup --window "$((WID))" 1 >/dev/null 2>&1
done
if shot "$((WID))" "$STORM_OUT/storm_after.png"; then
  echo "[storm] finished 25 passes x 25 moves, captured $STORM_OUT/storm_after.png" >&2
else
  echo "[storm] finished 25 passes x 25 moves, BUT THE CAPTURE FAILED — this run proves nothing" >&2
  exit 1
fi
