# Upstream notes

Findings worth sending to the projects we vendor. Each file is a draft written to be pasted as an
issue or a PR description with as little rework as possible.

**None of these are filed.** Filing is outward-facing and goes from the maintainer's account, so it needs his
explicit go — see the status line at the top of each.

## The two branches on `owlet-labs/visage`, and which one moves

| branch | state | what it is for |
|---|---|---|
| `fix/layer-destroy-deadlock` | **FROZEN at `018cd86`. Never push it.** | PR #98 rides this branch. Anything pushed to it edits an open pull request under the reviewer's feet. |
| `feathers-vendor` | **The living branch.** Fork work is pushed here. | What the `libs/visage` gitlink has to be reachable from. |

This split exists because it was broken the other way round. The fork's work sat only in the local
clone: origin was nine-plus commits behind, so the gitlink in every recent feathers commit named an
object nobody else could fetch, `git submodule update --init` could not resolve it, and cmake then
failed with an error that says nothing about any of that. A fresh checkout of this repo could not be
built by anybody but the machine that made it.

Fixed by pushing the fork tip to `feathers-vendor` (`2432bc8`), and verified from a genuinely fresh
`git clone` of the fork rather than by assuming: every gitlink named by the last fifteen feathers
commits — `fda2e50`, `28ea7b9`, `4a777d3`, `1ce3ad2`, `1c06b1f`, `f419f74`, `2432bc8` — is an
ancestor of that tip and resolves in the fresh clone. `.gitmodules` pins no `branch =`, so the
gitlink SHA is the thing that has to be reachable, and now it is. Origin's
`fix/layer-destroy-deadlock` was confirmed still at `018cd86` afterwards.

A HAZARD LEFT IN PLACE DELIBERATELY: the local clone still has a branch named
`fix/layer-destroy-deadlock` sitting thirteen commits ahead of the frozen origin ref, because
rewinding a ref somebody else may be using is not a thing to do quietly. The working checkout has
been moved onto `feathers-vendor` so an ordinary `git push` cannot reach the frozen name — but a
deliberate `git push origin fix/layer-destroy-deadlock` would still land on PR #98. Do not.

## visage (github.com/VitalAudio/visage)

| draft | what it is | how to send it |
|---|---|---|
| `2026-08-12-visage-window-destroy-deadlock.md` | Destroying a window-paired layer deadlocks the caller on Vulkan. Cost a user a wedged DAW. | **A PR, not an issue** — we have the fix, four lines and a guard, running on `owlet-labs/visage @ fix/layer-destroy-deadlock` (018cd86) |
| `2026-08-12-visage-bgfx-shutdown.md` | `bgfx::shutdown` is never called; the graphics context and the driver's threads outlive `main`. ThreadSanitizer names the race on every run. | Issue. Related to the above but a separate defect — it is about the renderer's lifetime, not a window's |
| `2026-08-12-visage-close-veto.md` | `ApplicationWindow::close()` reads an empty callback list as a veto, so it is a silent no-op by default — and visage's own window-manager path reads the same empty list as permission. | Issue, one-line fix suggested |
| `2026-08-12-visage-focal-radius-units.md` | `Brush::radial`'s `focal_radius` is a fraction, not a length, sitting between two lengths. Passing pixels silently inverts the gradient. | Issue, doc/rename suggested |
| `2026-08-13-visage-text-vertex-overflow.md` | `submitText` writes the gradient fields for every glyph in a block into a buffer sized for the glyphs that overlap the damage. Out-of-bounds write on any partial repaint; measured on a plain editor open. | **A PR, not an issue** — the fix is three lines moved, on `owlet-labs/visage @ fix/layer-destroy-deadlock` (7f159f6) |
| `2026-08-13-visage-font-atlas-repack-invalidation.md` | A font atlas repack moves every glyph and the retained frame is never invalidated, so one corrupted frame is kept permanently. This is the wedge that cost us weeks. | **A PR, not an issue** — two commits on `owlet-labs/visage @ fix/layer-destroy-deadlock` — the invalidation (00063e6) and the deferral that stops the bad frame being drawn at all (82cb97b). Pairs with the text-overflow PR: same subsystem, same repack event |
| `2026-08-13-visage-quad-index-overflow.md` | Two ceilings on a quad batch, neither announced. The transient arena runs out at ~14043 quads PER FRAME across all batches (6 MB default, never configurable) and the whole batch is then skipped in silence — `VISAGE_LOG` is a no-op under NDEBUG. Behind it, the `uint16` index space wraps at 16384. Measured, with pixel-level confirmation that a dropped batch draws nothing. | **A PR, not an issue** — we now carry both halves on `owlet-labs/visage @ fix/layer-destroy-deadlock` (fda2e50): the arena sized from a census, and the batch split at 16384, which the arena raise is what makes necessary |
| `2026-08-13-visage-transient-alloc-clamp.md` | `DownsamplePostEffect::setInitialVertices` writes four vertices into a buffer bgfx may have shortened. The single-buffer `allocTransientVertexBuffer` CLAMPS on a nearly-full arena rather than failing, and reports the difference only through a compiled-out assert; the site's only guard is a null check, which cannot fire on a clamped allocation. | Issue, fix suggested. NOT reproduced — Feathers sets no post effect, so it never executes the call. Reported because the guard is wrong on inspection |

Deliberately unbundled: they are independent defects, and bundling them would make the one with a
patch harder to accept.

## Ordering, if they are sent together

Lead with the deadlock PR — it is the one with a fix, a stack and a reproduction, and it is the most
likely to be wanted. The text-overflow PR is the natural second, since it also carries its fix and
sits on the same branch. The other four stand on their own and can follow at any pace.

## Filing them

Prepared but NOT filed: the publish step was blocked by this environment's permission system, which
is the right default for anything outward-facing. Everything below is ready to paste by someone who
holds that permission. The bodies are the draft files themselves — `--body-file` takes them as-is.

**BEFORE ANY PR: THE FORK'S WORK IS NOT ALL UPSTREAMABLE.** The living branch `feathers-vendor` now
carries fourteen commits past `main`, several of which are diagnostics that are ours rather than
upstream's: the opt-in atlas trace (POSIX-only, and it would break upstream's MSVC build) and the
opt-in batch trace and its short-write/unwalked reports (portable, but they change the signatures of
three internal batching helpers to carry a name). Filing from a whole branch would send all of it.
Every PR here wants a branch cut fresh off `main` with only its own commits cherry-picked —
`018cd86`, `7f159f6`, then `00063e6` and `82cb97b` in that order — see the notes section of
`2026-08-13-visage-text-vertex-overflow.md`.

`fix/layer-destroy-deadlock` on origin is exactly that already-cut branch for PR #98, which is why
it is frozen: it holds `018cd86` alone and nothing since.

The PR first, since it carries the fix. `owlet-labs/visage` is already a fork of the upstream repo,
so the cross-repo PR works, and `fix/layer-destroy-deadlock` (018cd86) is pushed:

```sh
gh pr create --repo VitalAudio/visage \
  --base main --head owlet-labs:fix/layer-destroy-deadlock \
  --title "Flush a frame buffer's deferred destroy while its window still exists (fixes Vulkan deadlock on window teardown)" \
  --body-file docs/upstream/2026-08-12-visage-window-destroy-deadlock.md
```

Then the four issues:

```sh
gh issue create --repo VitalAudio/visage \
  --title "bgfx is initialised once and never shut down; driver threads outlive main" \
  --body-file docs/upstream/2026-08-12-visage-bgfx-shutdown.md

gh issue create --repo VitalAudio/visage \
  --title "ApplicationWindow::close() treats an empty onCloseRequested list as a veto" \
  --body-file docs/upstream/2026-08-12-visage-close-veto.md

gh issue create --repo VitalAudio/visage \
  --title "Brush::radial's focal_radius is a fraction of the radius, not a length" \
  --body-file docs/upstream/2026-08-12-visage-focal-radius-units.md

gh issue create --repo VitalAudio/visage \
  --title "A quad batch too large for the transient arena is dropped silently; past that its uint16 indices wrap" \
  --body-file docs/upstream/2026-08-13-visage-quad-index-overflow.md
```

Two things to do by hand before sending, since the drafts are written for us as much as for them:

- strip the `**Status:** DRAFT` line from each body (it is an internal marker);
- the PR body should be the "Reproduction / Where it blocks / Mechanism / The change / Testing"
  content rather than the whole file — the upstream repo has no CONTRIBUTING.md, so there is no
  house format to follow, but our internal notes about the workaround are not their business.

After filing, put the URL in each draft's status line so the record points at the conversation.
