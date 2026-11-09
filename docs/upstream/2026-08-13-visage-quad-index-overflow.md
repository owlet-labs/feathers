# Upstream PR draft for visage: a large quad batch is silently dropped whole, and past that its indices wrap

**Filed:** not yet — needs the maintainer's go (see `docs/upstream/README.md`).

**Repo:** github.com/VitalAudio/visage
**Size:** both halves implemented on `owlet-labs/visage @ fix/layer-destroy-deadlock` (fda2e50) —
the arena sized from a census, and the batch split at the index ceiling. See "The fix".

---

## Summary

There are two ceilings on the size of a quad batch, one behind the other, and neither is announced.

1. **The transient vertex arena, at roughly 14000 quads per frame.** bgfx's transient vertex buffer is
   a fixed per-frame pool (6 MB by default) and visage never sets `Init::limits`. A `ShapeVertex` is
   112 bytes, so the pool holds 56173 vertices — about **14043 quads for the entire frame, shared
   across every batch in it**. When `allocTransientBuffers` cannot serve a batch,
   `initTransientQuadBuffers` returns false, `setupQuads` returns early, and **the whole batch is
   skipped**: every shape of that type, in every region of the layer, simply is not drawn. The only
   report is a `VISAGE_LOG`, which is compiled out under `NDEBUG` — so in a release build this is
   entirely silent.

2. **The 16-bit index space, at 16384 quads.** `initTransientQuadBuffers` writes quad indices as
   `uint16_t` with quad `i` addressing vertices `4i .. 4i+3`, so quad 16384 asks for 65536..65539 and
   those truncate to 0..3.

On a default build the first ceiling is lower than the second, so **the index wrap is currently
unreachable** for `ShapeVertex` shapes — the batch dies at ~14043 before it can get to 16385. Raising
the arena, which is the natural fix for (1), is exactly what makes (2) reachable. They want fixing
together, or the first fix converts a silently missing batch into a silently wrong one.

## Measurements

All from a windowless `Canvas` on Linux/Vulkan, bgfx defaults, release build:

```
sizeof(ShapeVertex)               = 112 bytes
index-space ceiling               = 16384 quads
transient arena                   = 56173 vertices = 14043 quads
largest batch that renders at all = 14041 quads
one more (14042)                  = renders nothing, silently
```

The arena is per **frame**, not per batch, and is shared by every batch in it:

```
8000 fills alone            -> draws
8000 circles alone          -> draws
8000 fills + 8000 circles   -> the fill batch vanishes entirely, circles draw
12000 fills + 3000 circles  -> the fill batch vanishes entirely
```

Two independent surfaces, each comfortably inside any per-surface budget an application might set,
remove each other's geometry.

## Where

`visage_graphics/shape_batcher.cpp`:

```cpp
bool initTransientQuadBuffers(int num_quads, const bgfx::VertexLayout& layout,
                              bgfx::TransientVertexBuffer* vertex_buffer,
                              bgfx::TransientIndexBuffer* index_buffer) {
  int num_vertices = num_quads * kVerticesPerQuad;
  int num_indices = num_quads * kIndicesPerQuad;
  if (!bgfx::allocTransientBuffers(vertex_buffer, layout, num_vertices, index_buffer, num_indices)) {
    VISAGE_LOG("Not enough transient buffer memory for %d quads", num_quads);   // (1), no-op in release
    return false;
  }

  uint16_t* indices = reinterpret_cast<uint16_t*>(index_buffer->data);
  for (int i = 0; i < num_quads; ++i) {
    int vertex_index = i * kVerticesPerQuad;
    int index = i * kIndicesPerQuad;
    for (int v = 0; v < kIndicesPerQuad; ++v)
      indices[index + v] = vertex_index + kQuadTriangles[v];                     // (2), no report at all
  }

  return true;
}
```

Every quad-drawing path reaches this function: `setupQuads` for the shape batches, `submitText`, and
the path atlas's clear pass.

## The count is per shape *per damage rectangle*

This is what makes either ceiling reachable in a real UI.

`setupQuads` emits one quad for each shape × each invalid rect the shape overlaps, and `numShapes`
sizes the allocation the same way through `numShapePieces`:

```cpp
for (const T& shape : *batch.shapes) {
  for (const IBounds& invalid_rect : *batch.invalid_rects) {
    ...
    vertex_index += kVerticesPerQuad;
  }
}
```

`Layer::invalidateRectInRegion` keeps damage as a list of non-overlapping rectangles and **breaks
overlapping damage into more pieces**, so a busy frame has more rectangles, not fewer. Any shape
spanning several of them is batched once per rectangle.

We measured this too: 9000 retained shapes are quiet when the frame repaints one area and cross the
ceiling when the same 9000 are repainted across two, with nothing redrawn in between.

Batches are also merged **across regions** — `Layer::submit` collects every matching `SubmitBatch`
from all region positions into a single `BatchVector` and submits it as one draw — so the budget is
per shape type per layer, not per widget.

## How it was observed

In our application a heat-map style surface drew one small `fill` per texel of a 128×128 map. Past a
certain zoom the whole surface disappeared for a frame at a time; because the frame buffer is not
cleared and the surrounding chrome does not repaint, what stayed on screen was the previous content,
which reads as a persistent artefact rather than as a dropped frame.

We added detection to our fork (`owlet-labs/visage @ fix/layer-destroy-deadlock`) reporting a batch's
quad count against both ceilings, plus a pixel-level control in a windowless canvas confirming that a
batch past the arena draws nothing at all. That is how the numbers above were obtained. It is
detection, not a fix, and it is written for us rather than for upstream.

## The fix

Two changes, and they have to land together — raising the arena is precisely what makes the index
wrap reachable, so the first without the second trades a silently missing batch for a silently wrong
one.

**1. Size the arena.** `bgfx::Init::limits.transientVbSize` was never set, so every visage app got
6 MB whatever it drew. We set 32 MB, which is 74898 quads, chosen from a census of the surface that
was failing rather than by eye: its worst frame asks for 13067 quads, so this is a little over 4x
that, with the remaining headroom for the multi-rect frames that multiply every shape spanning them.
Upstream would presumably rather expose the knob than pick a number for everyone; either way the
default deserves to be a decision.

**2. Split the batch at 16384 quads.** Rather than wrap, `submitBaseShapes` now submits in runs of at
most 16384, with a cursor over (batch, shape, rect) so a piece straddling a run boundary resumes
correctly. One extra draw call per run, paid only by batches that would otherwise have been wrong.

Two details that are easy to miss and that we got wrong first:

- **Per-run uniform binding is mandatory.** `bgfx::submit` defaults to `BGFX_DISCARD_ALL`, so the
  image-atlas, graph-data, heat-map and path-data bindings that were set once before submitting now
  have to be re-applied for every run, or every run after the first draws with nothing bound.
- **A run that cannot allocate keeps what already drew,** so an exhausted arena costs partial
  geometry instead of the entire batch.

**32-bit indices remain an option we did not take.** The `_index32` flag on `allocTransientBuffers`
with `BGFX_CAPS_INDEX32` would remove the ceiling on backends that support it, but it still needs a
fallback for those that do not, so chunking is needed either way.

**The reporting is ours, not part of the proposal.** The dropped-batch path deserves something that
survives `NDEBUG` — that it was silent in release is most of why this cost us what it did — but our
diagnostics are shaped for one bug on one machine.

## What we are not claiming

We are not claiming this is a common failure. It needs a single shape type to reach five figures of
quads in one frame, which most UIs never do. Applications drawing per-cell grids, dense scatter plots,
particles or heat maps do reach it, and they meet it as a silent rendering fault with nothing in any
log to correlate against.

We also want to be precise about what the index wrap would look like if it were reachable, because we
assumed worse at first and it cost us time. Both buffers are allocated at the full requested size, so
nothing is written out of bounds; only the index *value* wraps, and every vertex a wrapped index
points at was written earlier in the same pass. The tail of the batch would therefore redraw the head
— duplicate geometry and missing tail shapes, not arbitrary garbage.

## Notes for whoever files this

This is now a **PR** rather than an issue: both halves are implemented and controlled. The arena size
is the part upstream may well want to decide differently — exposing the limit rather than picking
32 MB for every application would be a reasonable counter-proposal, and the census tool that produced
our number is ours to describe rather than to send.

Our detection adds a `std::string_view` parameter to `initTransientQuadBuffers`,
`initQuadVerticesWithLayout` and `initQuadVertices` so a report can name the shape type. That is a
signature change to internal helpers and it is ours, not something to send. Unlike the atlas trace on
the same branch it adds **no POSIX dependency** and would not break an MSVC build — but it is still a
diagnostic for one bug on one machine, and the branch it lives on should not be the head of any PR
here. See the notes section of `2026-08-13-visage-text-vertex-overflow.md`.
