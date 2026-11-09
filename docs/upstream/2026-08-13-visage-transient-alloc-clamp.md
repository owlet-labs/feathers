# `DownsamplePostEffect::setInitialVertices` writes 4 vertices into a buffer bgfx may have shortened

**Status:** DRAFT — not filed. Found while hunting an unrelated rendering glitch; the guard is wrong
whether or not that glitch is ever traced to it.

## The defect

`DownsamplePostEffect::setInitialVertices` (visage_graphics/post_effects.cpp) allocates a
four-vertex transient buffer and then writes four vertices into it:

```cpp
bgfx::TransientVertexBuffer first_sample_buffer {};
bgfx::allocTransientVertexBuffer(&first_sample_buffer, 4, UvVertex::layout());
UvVertex* uv_data = reinterpret_cast<UvVertex*>(first_sample_buffer.data);
if (uv_data == nullptr)
  return;

for (int i = 0; i < kVerticesPerQuad; ++i) { ... }   // writes uv_data[0..3]
uv_data[0].u = left;  /* ... */  uv_data[3].v = bottom;
```

The single-buffer `bgfx::allocTransientVertexBuffer` does **not** fail when the transient arena is
nearly full. It **clamps**. `Frame::allocTransientVertexBuffer` (bgfx_p.h) takes its count by
reference and overwrites it with whatever was actually available:

```cpp
uint32_t allocTransientVertexBuffer(uint32_t& _num, uint16_t _stride)
{
    uint32_t offset = bx::strideAlign(m_vboffset, _stride);
    uint32_t num    = getAvailTransientVertexBuffer(_num, _stride);
    m_vboffset = offset + num * _stride;
    _num = num;
    return offset;
}
```

bgfx does notice — but only with `BX_ASSERT`:

```cpp
BX_ASSERT(_num == _tvb->size / _layout.m_stride
    , "Failed to allocate transient vertex buffer (requested %d, available %d). ...");
```

which is compiled out of a release build. So in release, with fewer than four vertices left in the
arena, this call returns a buffer sized for one, two or three vertices — or for none — and the
writes above run past its end into the next transient allocation, or past the arena itself.

**The null check cannot catch it.** `data` is `m_vertexBuffer->data + offset`, which stays non-null
even when the clamped count is zero. The only guard present is the one guard that cannot fire.

## Why this is easy to get wrong, and where visage gets it right

The dual-buffer entry point has the opposite contract — it pre-checks and refuses rather than
clamping:

```cpp
bool allocTransientBuffers(...)
{
    if (_numVertices == getAvailTransientVertexBuffer(_numVertices, _layout)
    &&  _numIndices  == getAvailTransientIndexBuffer(_numIndices, _index32) )
    { ...; return true; }
    return false;
}
```

Every other transient allocation in visage uses that one and checks its result —
`initTransientQuadBuffers` in shape_batcher.cpp and the path fill in path.cpp. `setInitialVertices`
is the only caller of the clamping single-buffer form, and it is the only one without a real check.

Two entry points whose names differ by two words, with opposite behaviour on exhaustion, one of them
reporting the difference only under a compiled-out assert.

## Suggested fix

Check the returned size rather than the pointer:

```cpp
bgfx::allocTransientVertexBuffer(&first_sample_buffer, 4, UvVertex::layout());
if (first_sample_buffer.data == nullptr
    || first_sample_buffer.size < 4 * UvVertex::layout().getStride())
  return;
```

or use `bgfx::getAvailTransientVertexBuffer` first and bail. Skipping the effect for one frame is
the same behaviour a failed batch already gets everywhere else in visage.

## How reachable is it

It needs the transient vertex arena to be within three vertices of full at the moment a downsample
post effect sets up. On bgfx's 6 MB default that arena holds about 14,000 quads for a whole frame
shared across every batch, so a dense frame CAN reach the end of it — that is a documented failure
mode in this codebase, and it is exactly the condition under which this call stops being safe.

Not reproduced. Feathers sets no post effect on any frame, so nothing in this plugin executes the
call at all, and the hunt that found it went elsewhere. It is reported because the guard is wrong on
inspection, not because a frame is known to have been corrupted by it.

## Notes for whoever files this

Pairs naturally with the quad-index-overflow report: same arena, same class of failure, same reason
nobody saw it — the diagnostics for running out of transient memory are compiled out of the builds
where running out matters.
