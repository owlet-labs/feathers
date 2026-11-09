#!/usr/bin/env python3
# Copyright © 2026 Owlet Records
#
# This file is part of Feathers.
#
# Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
# General Public License as published by the Free Software Foundation, either version 3 of the
# License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with this program. If not,
# see <https://www.gnu.org/licenses/>.
#
# --- Additional terms under GNU GPL version 3 section 7 ---
# This file requires preservation of the above copyright notice and author attribution in all copies
# or substantial portions of this file.


"""How far the wave's tint travels when Path moves.

WHY THIS EXISTS. The scope tints its curve from sound A's hue to sound B's and brightens whichever
one the blend favours. That brightening shipped at about 7% of the mock's, because a stroked polyline
overdraws itself and compounds its own alpha, and it took a user noticing the curve looked flat. The
metric that makes it legible is the 3b reviewer's: hold one END of the curve and swing Path, then ask
how much that end's brightness changed. The mock's left end falls to 0.60 of itself; ours fell to
0.94, which is under the threshold of noticing.

AND MIND WHAT YOU ARE MEASURING. The first version of this took the brightest pixel in a window at
each end — and the "PHI MORPH" label sits inside the left-hand window. Its grey outshone a dimmed
curve end and inflated the ratio from 0.56 to 0.70, which read as a regression that was not there.
The curve is coloured and the labels are grey, so this only considers pixels with real chroma. A
measurement that can be fooled by furniture is not a measurement.

USAGE, on the two captures guihost already makes at Path 0 and Path 1:
    DISPLAY=:1 ./build/guihost build/feathers.clap /tmp
    python3 tools/wave_swing.py /tmp/guihost_before.png /tmp/guihost_after.png

KNOWN-BAD: set the segment colour back to `withAlpha(hue, .5 + .5*weight)` in wave_display.cpp and
the swing collapses toward 1.0 — the ramp is still computed, and the overdraw eats it.
"""

import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("wave_swing: needs Pillow (pip install pillow)")

# The scope's own ground, and where it sits in the default 1240x700 window.
GROUND = (3, 5, 9)
BOX = (24, 608, 316, 679)
LEFT = (24, 54)
RIGHT = (286, 316)

# Only pixels with real chroma count — see the note above about the label.
MIN_CHROMA = 0.30

# The mock's own figures, for comparison rather than as a pass mark: the design is a product decision.
MOCK_LEFT = 0.60


def peak(image, x0, x1):
    px = image.load()
    best = 0
    for x in range(x0, x1):
        for y in range(BOX[1], BOX[3]):
            colour = px[x, y]
            high, low = max(colour), min(colour)
            if high == 0 or (high - low) / high < MIN_CHROMA:
                continue
            best = max(best, sum(abs(a - b) for a, b in zip(colour, GROUND)))
    return best


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    at_a = Image.open(sys.argv[1]).convert("RGB")
    at_b = Image.open(sys.argv[2]).convert("RGB")

    left_a, left_b = peak(at_a, *LEFT), peak(at_b, *LEFT)
    right_a, right_b = peak(at_a, *RIGHT), peak(at_b, *RIGHT)
    if not (left_a and right_a):
        sys.exit("wave_swing: found no coloured curve — are these the scope's captures?")

    left = left_b / left_a
    right = right_b / right_a
    print(f"left end  (sound A): Path0={left_a:4d}  Path1={left_b:4d}   swing {left:.2f}   "
          f"(mock {MOCK_LEFT})")
    print(f"right end (sound B): Path0={right_a:4d}  Path1={right_b:4d}   swing {right:.2f}")

    if left > 0.85 or right < 1.15:
        print("\nFAIL: the tint barely moves with Path. A stroked polyline overdraws itself and")
        print("compounds its own alpha — composite the brightness into the colour instead.")
        return 1
    print("\nPASS: the tint travels with the blend.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
