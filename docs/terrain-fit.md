# The fitted terrain map (Additive)

The Explore map colours every cell from five features of the sound at that place: energy, centroid, roughness,
parity and fundamental share. Measuring them means building the engine's cycle at each cell. For Additive the map
does not do that by default: it predicts the five features from the engine's decoded parameters with polynomials
fitted offline against the sound the engine really renders (`src/plugin/ui/terrain_fit.{h,cpp}`,
`terrain_fit_table.cpp`). A cell costs under a microsecond instead of 17 to 33.

**Additive's map is fitted by default. The real map is one right-click away**: right-click the chart and untick
"Fitted map". The item exists only while the engine is Additive; every other engine draws the real map, exactly as
if the fit were not there.

## What it is, and is not

- The fit is a picture of the sound at default controls. It reads the selected sound's six Additive bias knobs and
  Smoothness (the decode applies them as the bank does). Shape Amount, the likes' phase warp and the Pluck and Drum
  pictures are not modelled.
- It is a statistical model, not a measurement. The held-out error is in `terrain-fit-report.txt` (R2 from 0.78 for
  parity to 0.94 for the fundamental share on Additive).
- Where the view varies less than the fit's error, the picture is the error, not the sound. That is the case at
  tight zooms; `terrain-fit-zooms.txt` lists, per zoom, where the fit resolves a view and where it does not. Additive
  opens at x1/100, where it does. Untick "Fitted map" to see the real map at a tight zoom.
- `terrain-fit-cost.txt` has the per-cell cost of both roads.

## State

The switch is the chart's own, per editor instance, and is not saved with the patch: it is a way of drawing, not a
property of a sound. It is on at every open. `FEATHERS_TERRAIN_FIT=0` opens with the real map, `=1` with the fitted
one (where a table exists); the variable exists for captures, because a venue with no pointer cannot reach a menu.

## Regenerating

`scripts/fit-terrain-map.sh` renders stances through the real engine (build with `-DFEATHERS_BUILD_TOOLS=ON`) and
rewrites the table, the report and the evaluator's cross-check sample. The build never runs it. Forge was fitted the
same way and left out: its fit was too weak to draw from.
