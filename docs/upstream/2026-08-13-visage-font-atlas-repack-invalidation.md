**Status:** DRAFT — not filed. Filing is outward-facing and goes from the maintainer's account.

# A font atlas repack corrupts the retained frame, permanently

`PackedFont::resize()` destroys the atlas texture, repacks the map, and moves **every** glyph's atlas
coordinates. Any quad that was already batched keeps the coordinates it was given, so it samples the
new atlas with the old packing. That is one bad frame, which would be forgivable — but with a retained
buffer and partial repaints, nothing marks those rects invalid afterwards, so nothing ever redraws
them. The corruption is kept.

What it looks like in practice: a wedge of what reads as uninitialised-texture static, sitting over
part of the window, surviving every subsequent frame. It disappears only when something else happens
to damage that region — resizing the window clears it; clicking a control redraws that control's
rectangle and leaves the rest of the wedge intact.

## How we established that

We had the artefact for weeks with no mechanism. The discriminating observation was pressing a button
group while the wedge was live: the panel that was pressed redrew **on top of** the wedge, and
everywhere else the wedge persisted. That rules out live-draw corruption — a bad draw would recur — and
leaves stale pixels written once into the retained buffer.

The trigger fits: our occurrences came from text churn. Turning a value knob rewrites its digits every
frame, across every card that shows it, which grows the atlas. One occurrence had no pointer drag
involved at all — a knob turned with the wheel, digits churning.

## The change

**There is a precedent in the codebase for the shape of this fix**, which is part of why we are
offering it rather than only reporting the bug: the layer atlas already invalidates on its own repack —
`Layer::addPackedRegion` calls `invalidate()` in exactly this situation. Of the four atlases, the font
atlas was the one with no way to say it had moved. So:

- `visage::fontAtlasRepacks()` counts repacks, bumped inside `PackedFont::resize()` before the
  coordinates move;
- `Canvas::checkFontAtlasRepack()` compares that count against the one it last saw and invalidates
  every layer when it changes;
- `Canvas::submit()` calls it first thing.

Roughly twenty lines. It is a **mitigation, not a fix**, and the comments in the patch say so: the
corrupted frame is still drawn, it is simply no longer retained, so a permanent artefact becomes at
worst one frame of flicker.

## And the frame that gets corrupted in the first place

The mitigation above stops the bad frame being KEPT. A second commit stops it being DRAWN, and the two
compose rather than overlap:

`packGlyph()` repacked inline the moment a glyph did not fit — which is wherever a glyph is first
needed, and that can be in the middle of a submit. It now sets a flag instead, and `Canvas::submit()`
performs the repack at its top, where nothing is batched and no quad holds coordinates that are about
to move. The frame in which a glyph is first seen is then a frame with that glyph MISSING rather than a
window of garbage, and the invalidation is what brings it back on the frame that repacks.

Two details that are load-bearing, and both cost us a debugging pass:

- **A deferred glyph must be handed back as a null glyph, never a stale one.** `PackedAtlasMap::addRect`
  registers the id and pushes a rect at (0, 0) *before* asking the packer to place it, so a deferred
  glyph that took coordinates from the map would draw whatever sits at the origin — and rasterizing it
  would blit its bitmap over that glyph's pixels. Width zero is what `'\n'` already gets, so everything
  downstream already skips it.
- **It must not be re-added on the next attempt.** `addRect` asserts on a duplicate id, and in a
  release build the lookup would keep pointing at the older index. A `hasId()` guard is the whole fix.

The cost is one frame in which a newly seen glyph is missing and the string containing it measures
short. We think that is obviously better than the alternative, but it IS a behaviour change and you may
disagree — the two commits are independent, and the invalidation alone is worth having.

The third option we considered and did not take: **re-emit the affected batches**. Considerably harder,
since the vertices are already in transient buffers handed to bgfx, so the frame would have to be
re-recorded rather than patched.

## Testing

A standalone control in our tree drives a real `Canvas`. It reaches a steady state where a submit with
nothing new drawn renders nothing, then forces a repack by **measuring** strings — which packs glyphs
without drawing anything and without invalidating any region — and checks the whole sequence: that
measuring DEFERS the repack rather than performing it, that a frame performs the one it was asked for,
that the same frame redraws everything, that a deferred glyph is drawn once the repack has run, and
that the frame after settles back to quiet.

Each half fails on its own known-bad: repacking inline again fails the deferral assertions, and
removing the call from `submit()` fails only the two that say a *frame* performs it.

Worth knowing if you write your own: forcing the repack by *drawing* text does not work as a control,
because drawing dirties the region it draws into and the full redraw then proves nothing. That first
version reported a failure that was really a confound — and finding it established that packing
happens at draw time, three repacks inside a single `text()` call.

## Notes for whoever files this

Same branch hazard as the text-overflow PR — see the notes section of
`2026-08-13-visage-text-vertex-overflow.md`. `owlet-labs/visage @ fix/layer-destroy-deadlock` now
carries five commits, two of which are an opt-in atlas trace that is ours and would break upstream's
MSVC build. This wants a fresh branch off `main` with `00063e6` and `82cb97b` cherry-picked — in that
order; the second builds on the first — and it pairs naturally with `7f159f6`: same subsystem, same
repack event, one PR each or one PR with both.
