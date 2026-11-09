# Upstream PR draft for visage: `submitText` writes past the end of its transient vertex buffer

**Filed:** not yet — needs the maintainer's go (see `docs/upstream/README.md`).

**Repo:** github.com/VitalAudio/visage
**Size:** three lines moved, one local added. Fix is on `owlet-labs/visage @ fix/layer-destroy-deadlock` (7f159f6).

---

## Summary

`submitText` allocates a transient vertex buffer sized to the glyph quads that **overlap the invalid
rects**, then writes the per-vertex gradient fields for **every glyph in the text block**. Any text
block that is only partially inside the damage region therefore writes past the end of the
allocation — an out-of-bounds write of `(glyphs − overlapping) × 4` vertices.

It fires in ordinary use. Partial damage is what any application does when it repaints around a
moving control, and the more of the layout a small damage rectangle clips, the larger the overflow.

## Where

`visage_graphics/shape_batcher.cpp`, in `submitText`. The allocation is sized from the overlapping
count:

```cpp
int total_length = 0;
for (const auto& batch : batches) {
  auto count_pieces = [&batch](int sum, const TextBlock& text_block) {
    return sum + numTextPieces(text_block, batch.x, batch.y, *batch.invalid_rects);
  };
  total_length += std::accumulate(batch.shapes->begin(), batch.shapes->end(), 0, count_pieces);
}
...
TextureVertex* vertices = initQuadVertices<TextureVertex>(total_length);
```

`numTextPieces` counts only the quads that overlap. But inside the per-rect loop:

```cpp
PackedBrush::setVertexGradientPositions(text_block.brush, vertices + vertex_index,
                                        length * kVerticesPerQuad, x, y, batch.x, batch.y,
                                        x + text_block.width, y + text_block.height);

for (int i = 0; i < length; ++i) {
  if (!overlaps(text_block.quads[i]))
    continue;
  ...
  vertex_index += kVerticesPerQuad;
}
```

`length` is `text_block.quads.size()` — the whole block — while `vertex_index` advances only for the
glyphs that overlap. And `setVertexGradientPositions` (`visage_graphics/gradient.h`) writes every
vertex it is given, unconditionally:

```cpp
for (int i = 1; i < num_vertices; ++i) {
  vertices[i].gradient_texture_position = vertices[0].gradient_texture_position;
  vertices[i].gradient = vertices[0].gradient;
}
```

The function's own closing assertion already states the invariant that this breaks:

```cpp
VISAGE_ASSERT(vertex_index == total_length * kVerticesPerQuad);
```

The allocation is exactly the overlapping count. Nothing may write beyond it.

## How it was observed

Instrumented the comparison — `vertex_index + length * kVerticesPerQuad` against
`total_length * kVerticesPerQuad` — in a CLAP plugin editor built on visage. It reports on a plain
editor open, with a **single** invalid rect:

```
submitText writes 8 vertices past the end
(vertex_index=180 length=15 writes_to=240 allocated=232 rects=1)
```

A 15-glyph block of which 13 overlapped: two quads of overflow. With the patch below, the same
instrumentation on the same session reports zero events.

## Fix

Record the index before the quad loop and set the gradient positions afterwards, for the quads that
were actually written:

```cpp
const int block_start = vertex_index;

for (int i = 0; i < length; ++i) {
  if (!overlaps(text_block.quads[i]))
    continue;
  ...
  vertex_index += kVerticesPerQuad;
}

PackedBrush::setVertexGradientPositions(text_block.brush, vertices + block_start,
                                        vertex_index - block_start, x, y, batch.x, batch.y,
                                        x + text_block.width, y + text_block.height);
```

Same values into the same vertices. It also does slightly less work, since it no longer writes
gradient fields for glyphs that were skipped.

## What we are not claiming

We found this while hunting a different bug — a mid-drag geometry corruption in our own plugin — and
we do **not** think this is its cause. The overflow touches only the `gradient` and
`gradient_texture_position` fields, writing forward into transient-arena space that later allocations
in the same frame reclaim and overwrite, so in most frames it is invisible. It is reported because it
is unambiguous undefined behaviour on a common path, not because we can attribute a visible artefact
to it.

Whether it can become visible depends on bgfx's transient buffer layout in cases we have not
enumerated — a text block near the end of a frame's allocations, with a large glyph count and a small
overlap, writes furthest.

## Notes for whoever files this

**THE BRANCH CARRIES MORE THAN THIS FIX, and the PR command in `README.md` sends all of it.**
`owlet-labs/visage @ fix/layer-destroy-deadlock` now holds three commits: the window-destroy deadlock
fix, this one, and an **opt-in atlas trace that is ours rather than upstream's** (`c8ba573`,
`ead7efc`). A PR opened with `--head owlet-labs:fix/layer-destroy-deadlock` would carry the trace to
VitalAudio along with the fixes.

Two reasons that is wrong, and the second is the harder one:

1. **The trace is a diagnostic for one bug on one machine.** It is not a feature upstream asked for
   and not one we would defend in review.
2. **It would break their Windows build.** `clock_gettime` and `CLOCK_REALTIME` are POSIX, the header
   it lives in is included widely, and visage's CMake carries an MSVC branch. The C11 spelling that
   compiles everywhere is `timespec_get(&ts, TIME_UTC)` — noted beside the call rather than applied,
   since the trace is ours.

So this PR wants a branch of its own — the deadlock fix and this one, without the trace — rather than
the branch as it stands. Cherry-picking `7f159f6` onto a fresh branch off `main` is the whole job.
