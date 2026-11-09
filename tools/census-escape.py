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

"""READ A VISAGE_TRACE_QUADS CENSUS, and answer the only two questions it can answer honestly.

    VISAGE_TRACE_QUADS=1 build/windowed-probe 2> census.log
    tools/census-escape.py census.log [boundary-x]

1. ESCAPE — did a batch whose region sits RIGHT of a boundary put a quad LEFT of it? That is the
   cross-region author shape: the artefact sits over the left chrome column, so a stage batch
   reaching across the column edge would name itself.
2. JUMP — is each batch's bounding box stable frame to frame? A stray one-frame line shows up as an
   outlier extent even when its region is innocent, and it is read by comparing a frame against its
   neighbours rather than by waiting for a flag to fire.

THE CORRECTION THAT MAKES THIS USABLE, and it cost a false positive before it was written down: THE
CENSUS RECORDS QUAD RECTS, NOT INK. For an arc or a ring the quad rect is the bounding square of the
WHOLE circle while only the angularly visible sweep is painted — so PerformStage::puckArc, whose
reticle rings follow a puck that legitimately travels outside the disk, reports quads hundreds of
pixels outside its own stage on most frames while every pixel it paints is inside. Reading that as a
finding manufactures an author out of ordinary geometry. Hence:

  - "bbox crosses the boundary" is EVIDENCE only for shapes whose ink fills their rect — Fill,
    RoundedRectangle, TextBlock, and the segments. For *Arc it is not evidence at all.
  - Ink is separately bounded by the region's own clamp (shape_batcher.h applies ClampBounds at
    vertex time), so a bounded region cannot paint outside itself however wrong its coordinates are.
    The documented hole is a FULL-WINDOW region, which clamps to everything — so a census escape
    matters most for the root region and least for a small one.

WHICH IS WHY THE STABILITY HALF IS THE STRONGER READ. Only a region that COVERS the cards can paint
over them, so the question that settles the hunt is whether the root, Engine rail and stack batches
ever changed extent — and that is question 2, not question 1.
"""

import re
import sys
from collections import defaultdict

LINE = re.compile(
    r"\[VISAGE-QUADS\] (\S+) region \((-?\d+),(-?\d+)\) (\d+) quads "
    r"bbox \((-?[\d.e+-]+),(-?[\d.e+-]+)\)-\((-?[\d.e+-]+),(-?[\d.e+-]+)\) at (\d+)"
)

# Ink fills the rect for these; for an arc it does not, so an escape by an arc is not a finding.
INK_FILLS_RECT = ("Fill", "RoundedRectangle", "TextBlock", "FlatSegment", "RoundedSegment",
                  "Circle", "SampleRegion", "ImageWrapper")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    boundary = float(sys.argv[2]) if len(sys.argv) > 2 else 374.0

    samples = defaultdict(list)
    with open(sys.argv[1], errors="replace") as handle:
        for raw in handle:
            found = LINE.search(raw)
            if found is None:
                continue
            key = (found.group(1), int(found.group(2)), int(found.group(3)))
            samples[key].append((
                int(found.group(9)), int(found.group(4)),
                float(found.group(5)), float(found.group(6)),
                float(found.group(7)), float(found.group(8)),
            ))

    if not samples:
        print("no [VISAGE-QUADS] lines — was VISAGE_TRACE_QUADS=1 set on the run?")
        return 1

    print(f"census: {sum(len(v) for v in samples.values())} batch-frames, "
          f"{len(samples)} shape x region pairs, boundary x={boundary:g}")
    print()
    print(f"=== 1. ESCAPE: regions at or right of x={boundary:g} that put a quad left of it ===")
    escaped = False
    for key in sorted(samples):
        name, rx, ry = key
        if rx < boundary:
            continue
        bad = [s for s in samples[key] if s[2] < boundary]
        if not bad:
            continue
        escaped = True
        stamp, quads, left, top, right, bottom = min(bad, key=lambda s: s[2])
        verdict = "FINDING" if name in INK_FILLS_RECT else "quad rect only, NOT ink — see the header"
        print(f"  {name} region ({rx},{ry}): {len(bad)} of {len(samples[key])} batches, "
              f"widest ({left:g},{top:g})-({right:g},{bottom:g}) {quads} quads at {stamp}")
        print(f"      -> {verdict}")
    if not escaped:
        right_of = sum(1 for k in samples if k[1] >= boundary)
        print(f"  none — {right_of} shape x region pairs live right of the boundary, "
              f"every batch of every one stayed there")

    print()
    print("=== 2. JUMP: batches whose extent moved by more than a pixel ===")
    print("(a startup resize moves everything once; cut the log to the settled layout to read this)")
    moved = []
    for key in sorted(samples, key=lambda k: (k[1], k[2], k[0])):
        name, rx, ry = key
        rows = samples[key]
        edges = [[s[i] for s in rows] for i in (2, 3, 4, 5)]
        span = max(max(e) - min(e) for e in edges)
        if span < 1.0:
            continue
        moved.append((span, key))
        print(f"{name:<17}({rx},{ry})".ljust(30)
              + f"{len(rows):>6}  "
              + "  ".join(f"{min(e):>8.4g}..{max(e):<9.4g}" for e in edges))
    if not moved:
        print("  nothing moved: every batch held the same extent on every frame")
    print()
    for span, key in sorted(moved, reverse=True)[:6]:
        print(f"  widest movement: {key[0]} region ({key[1]},{key[2]}) moved an edge by {span:.1f}px")
    return 0


if __name__ == "__main__":
    sys.exit(main())
