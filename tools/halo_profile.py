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

"""A sound's glow must be brightest AT the sound.

WHY THIS EXISTS. The halos shipped inverted for a whole task and no test could see it: they were
drawn, in the right place, in the right colour, with the right blend mode, and every unit test and
harness verdict stayed green. The bug lived entirely in a gradient's shape — visage's focal radius is
a FRACTION of the radius rather than a length (visage_graphics/svg.h:86), so passing pixels put alpha
0 at the sound and ramped it up toward the rim. It took a user to notice, which is exactly the
failure mode the plan's "render it before you believe it" rule is about.

This is that rule made runnable. It measures the rendered brightness outward from a sound and insists
it DECREASES. A glow that brightens with distance from its source is not a glow.

USAGE
    DISPLAY=:1 ./build/guihost build/feathers.clap /tmp
    python3 tools/halo_profile.py /tmp/guihost_before.png

KNOWN-BAD, which is what makes a pass mean anything. In perform_stage.cpp's drawSounds, pass `inner`
instead of `inner / outer` as the focal radius and run it again: the profile inverts and this script
fails, naming the sound it failed on. Two incremental builds, and the recipe is the durable artefact
— a stale binary somebody has since rebuilt is no longer a control.
"""

import math
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover - the harness box has it; a bare one may not
    sys.exit("halo_profile: needs Pillow (pip install pillow)")

# The editor's own geometry, in the default 1240x700 window. Taken from the layout rather than
# guessed: the stage is what the header, tray and Engine rail rail leave, and the disk is the largest
# circle centred in it. See Gui::Impl::stageBounds and StageDisk.
HEADER_H = 52.0
TRAY_H = 114.0
RAIL_W = 168.0
RIM_INSET = 8.0

# Where the two sounds sit, from PerformStage::kPlaceA / kPlaceB.
PLACES = {"A": (-0.52, -0.28), "B": (0.55, -0.12)}

# WHERE to sample, as a fraction of the disk radius. Deliberately clear of the star glyph — its own
# halo, glint and core dominate everything inside ~8% and would answer this question for the glow.
# The glow's outer radius is 0.8 of the disk, so all four of these sit inside it where it should
# still be falling. The first version of this script sampled at 0% and PASSED THE KNOWN-BAD BUILD,
# because it was measuring the glyph; that is why the numbers below start where they do.
SAMPLES = (0.10, 0.18, 0.30, 0.45)

# Thresholds taken from measuring real builds rather than from expectation (median of 8 directions,
# summed RGB above the void floor, on the 1240x700 default):
#   shipped, default phase   A +100  B +116
#   shipped, at the trough   A  +87  B  +85   (FEATHERS_SHIMMER_AT=5.236 and 3.347)
#   inverted focal radius    A   +3  B   +1   — the void, and not falling
#   the superseded levels    A  +50  B  +68 at default, A +34 at the trough
# The floor below therefore does double duty: it still catches an inverted gradient, and it also
# catches a slide back toward the presence judged too faint on 2026-08-12. It is set under the
# trough, because the dimmest moment of the breathing is still a moment the glow must be there.
MIN_ABOVE_FLOOR = 60.0  # the glow is present, at any phase of its breathing
MIN_FALL = 12.0  # and it falls away from its source


def luminance(pixel):
    r, g, b = pixel[:3]
    return r + g + b


def profile(image, centre, radius, place):
    """Median brightness on rings of increasing radius around a sound."""
    cx = centre[0] + place[0] * radius
    cy = centre[1] + place[1] * radius
    width, height = image.size
    out = []
    for fraction in SAMPLES:
        offset = fraction * radius
        readings = []
        for step in range(8):
            angle = step * math.pi / 4.0
            x = int(cx + math.cos(angle) * offset)
            y = int(cy + math.sin(angle) * offset)
            if 0 <= x < width and 0 <= y < height:
                readings.append(luminance(image.getpixel((x, y))))
        if not readings:
            return None
        # The MEDIAN of eight directions, so a grid ring, the tour's dashes or the other sound
        # crossing one or two of them cannot decide the answer.
        readings.sort()
        out.append(readings[len(readings) // 2])
    return out


def voidFloor(image):
    """The stage's own background, from a corner the disk does not reach."""
    width, height = image.size
    stage_w = width - RAIL_W
    corners = [
        (RAIL_W + 6, HEADER_H + 6),
        (width - 6, HEADER_H + 6),
        (RAIL_W + 6, height - TRAY_H - 6),
        (width - 6, height - TRAY_H - 6),
    ]
    readings = [luminance(image.getpixel((int(x), int(y)))) for x, y in corners]
    readings.sort()
    return readings[len(readings) // 2]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    image = Image.open(path).convert("RGB")
    width, height = image.size

    stage_w = width - RAIL_W
    stage_h = height - HEADER_H - TRAY_H
    radius = min(stage_w, stage_h) / 2.0 - RIM_INSET
    centre = (RAIL_W + stage_w / 2.0, HEADER_H + stage_h / 2.0)

    floor = voidFloor(image)
    print(f"{path}: {width}x{height}, disk centre {centre}, radius {radius:.1f}, void floor {floor}")
    failed = []
    inverted = False
    faint = False
    for name, place in PLACES.items():
        readings = profile(image, centre, radius, place)
        if readings is None:
            print(f"  sound {name}: off the image — is this the right capture?")
            failed.append(name)
            continue
        near, far = readings[0], readings[-1]
        above = near - floor
        fall = near - far
        marks = "  ".join(f"{f*100:>3.0f}%={v:5.0f}" for f, v in zip(SAMPLES, readings))
        problems = []
        if above < MIN_ABOVE_FLOOR:
            problems.append("ABSENT")
        if fall < MIN_FALL:
            problems.append("NOT FALLING")
        # Both together is a gradient the wrong way round; ABSENT with a healthy falloff is simply a
        # glow that is too dim to be the one that was approved.
        if above < MIN_ABOVE_FLOOR and fall < MIN_FALL:
            inverted = True
        elif above < MIN_ABOVE_FLOOR:
            faint = True
        verdict = "ok" if not problems else " + ".join(problems)
        print(f"  sound {name}: {marks}   above floor={above:+.0f}  fall={fall:+.0f}  [{verdict}]")
        if problems:
            failed.append(name)

    if failed:
        print(f"\nFAIL: the glow around sound(s) {', '.join(failed)} is not what it should be.")
        # The two failures have different causes and the message has to say which, or it sends the
        # next reader after the wrong one. This script was written for the inverted gradient and then
        # given a second job — guarding the presence that was set — and for a while it reported the second
        # failure with the first one's explanation.
        if inverted:
            print("  INVERTED: level with the void at the sound and brighter further out. That is a")
            print("  gradient the wrong way round — check the focal radius in drawSounds, which visage")
            print("  takes as a FRACTION of the radius, not a length (visage_graphics/svg.h:86).")
        if faint:
            print("  TOO FAINT: the falloff is the right shape, there is simply not enough of it. This")
            print("  is the presence set on 2026-08-12 (glow alpha .28 +/- .04, mid stop .09);")
            print("  the superseded values read about +36 here and are what this floor exists to catch.")
        return 1
    print("\nPASS: every sound sits at the bright centre of its own glow.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
