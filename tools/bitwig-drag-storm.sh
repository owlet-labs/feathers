#!/usr/bin/env bash
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Storm the BITWIGHOST editor — the embed replica, which is the venue the mid-drag corruption has
# only ever appeared in. guihost opens the editor its own way; if the embedding is what makes the
# difference, only this one can show it.
#
#   FEATHERS_UI_SCALE=1.75 FEATHERS_BITWIGHOST_HOLD=120 \
#     build-dbgpanel/bitwighost build-dbgpanel/feathers.clap > bw.log 2>&1 &
#   tools/bitwig-drag-storm.sh bw.log
#
# CAPTURES MID-DRAG, with the button still down, which the first version did not: it shot after
# mouseup and found nothing, and that proves less than it looks — a full repaint on release clears
# the corruption before the shutter opens.
#
# It finds the plugin's editor as the container's child WIDER THAN 100px, because the host also keeps
# a 1x1 helper child at (-1,-1) and aiming at that one is aiming at nothing.
#
# THE PASSIVE ALTERNATIVE, which needs no storm and no pointer at all:
#
#   VISAGE_TRACE_ATLAS=1  — one stderr line per font-atlas repack, with an epoch-millisecond stamp.
#
# It is compiled into every build and costs one branch when the variable is unset, so an ORDINARY
# Bitwig session becomes the instrumented venue — which is the only place this fault has ever
# appeared. When the wedge is next seen, the screenshot's mtime lines up against those stamps and
# says whether a repack had just happened. Under flatpak the variable has to cross the sandbox:
#
#   flatpak run --env=VISAGE_TRACE_ATLAS=1 com.bitwig.BitwigStudio
#
# WHY A REPACK IS THE SUSPECT: FontAtlas::resize() destroys the texture handle, repacks the map, and
# moves every glyph's atlas coordinates, all in one call. A draw already submitted this frame holds
# the OLD coordinates into a texture that has just been repacked — which would sample garbage
# exactly where text is, and the wedge sat over the Engine rail.
#
# VERIFY THE STORM BEFORE BELIEVING ITS RESULT: read PATH and POS X/Y off the final frame. If they
# have not moved, nothing was dragged and a clean run means nothing. See tools/drag-storm.sh for the
# three ways a storm can silently reach no one — and for the fourth, a SLEEPING machine, which the
# preflight below is here to catch before either a hang or a clean frame is believed.

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
for i in $(seq 1 240); do sleep 0.5; grep -q "holding:  " "$LOG" && break; done
CID=$(grep -oP 'holding:.*container \K0x[0-9a-f]+' "$LOG" | head -1)
[ -z "$CID" ] && { echo "[storm] no container" >&2; exit 1; }
# The plugin's editor is the container's OTHER child (the host keeps a 1x1 helper at -1,-1).
KIDS=$(xdotool search --all --onlyvisible "" 2>/dev/null)
EDITOR=$(xwininfo -id $((CID)) -children 2>/dev/null | awk '/child|children/{f=1;next} f&&/0x/{print $1}' | while read w; do
  G=$(xdotool getwindowgeometry --shell "$((w))" 2>/dev/null); eval "$G"; [ "${WIDTH:-0}" -gt 100 ] && echo "$w"; done | head -1)
[ -z "$EDITOR" ] && EDITOR="$CID"
eval $(xdotool getwindowgeometry --shell "$((EDITOR))" 2>/dev/null)
echo "[storm] container $CID editor $EDITOR ${WIDTH}x${HEIGHT}" >&2
RX=$((WIDTH*62/100)); RY=$(((HEIGHT-110)*48/100))
for pass in $(seq 1 30); do
  xdotool mousemove --window "$((EDITOR))" $RX $RY >/dev/null 2>&1
  xdotool mousedown --window "$((EDITOR))" 1 >/dev/null 2>&1
  for i in $(seq 0 30); do
    DX=$(( ((i*41) % 140) - 70 )); DY=$(( ((i*59) % 110) - 55 ))
    xdotool mousemove --window "$((EDITOR))" $((RX+DX)) $((RY+DY)) >/dev/null 2>&1
    # CAPTURED MID-DRAG, with the button still down. The previous run captured after mouseup and found
    # nothing, which proves less than it looks: the corruption seen appeared DURING motion and a
    # full repaint on release would clear it before the shutter opened.
    if [ $((i % 10)) -eq 3 ]; then
      # A MID-DRAG FRAME IS THE POINT OF THIS SCRIPT, so a blank one here is scored as evidence
      # about a drag. Counted rather than fatal: one bad grab should not end a 30-pass storm, but
      # the total must reach the operator or the run is short without saying so.
      shot "$((EDITOR))" "$STORM_OUT/mid_${pass}_${i}.png" || MID_FAILED=$((MID_FAILED+1))
    fi
  done
  xdotool mouseup --window "$((EDITOR))" 1 >/dev/null 2>&1
done
if ! shot "$((EDITOR))" "$STORM_OUT/bw_final.png"; then
  echo "[storm] 30 passes done BUT THE FINAL CAPTURE FAILED — this run proves nothing" >&2
  exit 1
fi
echo "[storm] 30 passes x 31 moves done, captures in $STORM_OUT" >&2
if [ "${MID_FAILED:-0}" -gt 0 ]; then
  echo "[storm] $MID_FAILED MID-DRAG CAPTURE(S) FAILED OR CAME BACK BLANK — the mid-drag evidence" >&2
  echo "[storm] is short by that many frames, which is where this script's whole claim lives" >&2
fi
