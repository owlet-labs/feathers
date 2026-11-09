# An intermediate layer's contents are not preserved between frames, and partial damage assumes they are

**Status:** DRAFT — not filed. The defect is confirmed by experiment; the exact door in bgfx is
identified as a class but not yet pinned to a line, and the aim points below are where a filer should
start rather than a diagnosis to submit as one.

**Where it belongs:** partly visage (which assumes preservation it never declares), partly bgfx
(which is where preservation is actually lost). The visage half is filable today. The bgfx half needs
one session with validation layers first — see "What is still unproven".

## The defect

visage is a partial-damage renderer. `Layer::clearInvalidRectAreas` paints opaque fills over the
invalid rectangles and over nothing else, and there is no `setViewClear` anywhere in the codebase, so
every pixel outside a frame's damage is expected to still be there next frame. That expectation is
never declared to the graphics API — it is simply assumed of the render target.

On Linux, where bgfx selects Vulkan, that assumption does not hold reliably. An intermediate layer's
texture loses part of its contents, and because only damaged rectangles are ever redrawn, whatever
replaced them stays on screen indefinitely. The result is a permanent corrupt region in exactly the
part of the interface that redraws least.

## How it presents

A wedge of fine vertical dashes over the areas that do not repaint, with the areas that DO repaint
pristine inside it. In the reported case: an editor's inspector rail and sound-stack column buried,
while the two cards being driven by parameter automation stayed sharp, and the constantly-animating
surface to their right was untouched.

Two properties make it hard to recognise:

- **It is additive, not noise.** The underlying interface stays legible with bright marks laid over
  it — measured at ~2641 pixels added against ~18 removed on an 80x80 patch. Undefined contents on
  this driver evidently preserve most of the previous image and corrupt part of it. An investigation
  that expects "uninitialised memory means a field of random values" will rule this out wrongly. It
  did, here, for several hours.
- **Nothing reports it.** No draw call is involved, so every draw-side check stays silent: no shape
  is misplaced, no batch is short, no atlas or texture event occurs anywhere near it. In the reported
  case the nearest texture event was 7.8 seconds away and the nearest damage on the affected area 33
  seconds away.

## How it was confirmed

By intervention rather than by inspection, after four instrumented routes had each been eliminated by
silence and a fifth log would have added nothing.

Repainting every layer every frame removes the surface the fault needs. Same session, same recipe,
minutes apart:

| arm | result |
| --- | --- |
| control | artifact reproduced twice — cycle 1 frame 13, cycle 2 frame 18 |
| every layer repainted every frame | zero stray pixels across three cycles; 21 of 21 armed frames byte-identical |

Two independent confirmations followed. A per-batch census logging every frame — not merely the
frames a capture happened to catch — found no batch whose bounding box fell anywhere in the affected
region, in any frame of a 90-second run. And the same census quantified the precondition: over that
run the affected regions were drawn 8 and 16 times, against 14095 for the animating surface beside
them. An area drawn eight times in ninety seconds displays whatever the render target holds for
practically the whole session.

## Aim points in bgfx

bgfx declares the render pass correctly: `loadOp = VK_ATTACHMENT_LOAD_OP_LOAD` with
`initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL` (`renderer_vk.cpp`, attachment
description setup). A driver discarding contents *there* would be out of spec, so that is not the
door.

The legal door is a layout transition carrying `oldLayout = VK_IMAGE_LAYOUT_UNDEFINED`, which permits
the implementation to discard the image's contents. Three places are worth examining under a
reproduction:

1. **`TextureVK::setImageMemoryBarrier`** takes its old layout from the tracked
   `m_currentImageLayout`. That is correct while the tracking is accurate — it is UNDEFINED at
   creation and reset to UNDEFINED on destroy, both legitimate — so the question is whether any path
   leaves it UNDEFINED while the image still holds content.
2. **Two direct `vk::setImageMemoryBarrier` calls** bypass that tracking entirely rather than going
   through the member function.
3. **The swapchain path** deliberately substitutes UNDEFINED when not presenting.

bgfx already guards against exactly this hazard in one place: `TextureVK::copyBufferToTexture`
substitutes the sampled layout when its tracking reports UNDEFINED, rather than passing UNDEFINED
through as an old layout. A guard written once, at one call site, is a hazard its authors met once
and patched locally. **The patch thesis is to generalise that guard rather than add a second special
case** — an image that holds content should never present UNDEFINED as an old layout, wherever the
transition is issued from.

The cadence that makes this reachable is worth stating, because it is what an intermediate layer
lives under in a partial-damage renderer: sampled by the composite every frame, re-rendered as a
colour attachment only on the frames it has damage, sampled again. Frames where it is sampled but not
re-rendered are the asymmetric ones.

## What is still unproven

- **Which of the three aim points fires.** Establishing that needs Vulkan validation layers or a
  frame capture against a reproduction. Nobody should file a bgfx bug naming a line on the strength
  of the reasoning above.
- **The boundary is a straight diagonal**, measured linear across 240 rows at a slope of 1.358
  horizontal pixels per row. Undefined contents ought to follow allocation or tile boundaries, which
  are rectangular. Two explanations were tested and both fail on the number: a quad's two triangles
  split on a diagonal, but the relevant quad's diagonal is 1240/700 = 1.771; and a row-pitch mismatch
  shears by a constant per row, but no pair of dimensions in play yields 1.358. **A tiling
  explanation was tested and is negative** — column periodicity peaks at lag 4 and its harmonics, not
  at 8, 16, 32 or 64. Four is one RGBA texel, not a tile. Something makes which memory survives vary
  linearly with y, and it is unexplained. It does not affect the fix direction, and it is recorded
  here so that nobody re-derives the two dead explanations.

## The visage half, which is filable now

Independent of where bgfx loses the contents, visage relies on preservation it never asks for. Either
of these would be a defensible upstream change:

- Declare it. Ensure a layer's contents are explicitly preserved across frames rather than assumed,
  so a driver that would otherwise be free to discard is not.
- Or make the failure survivable. Where preservation cannot be guaranteed, fall back to clearing and
  fully invalidating the layer, so a lost frame costs one repaint instead of persisting for a
  session.

The workaround in hand — repaint every layer every frame — is proven and coarse. It measured 5.9x the
quads per frame in the reported case (262 to 1554), which stayed comfortably inside the transient
arena but gives up partial damage entirely.
