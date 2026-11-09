# Feathers — contributor guidelines

Feathers is a CLAP synthesizer and effects rack (GPL-3.0-or-later). The device SDK under
`libs/feathers-device-sdk/` is MIT. These are the conventions the codebase relies on; most of them exist
because breaking them once produced a real defect.

## Team-internal notes
- If a `private/` directory exists at the repository root, it holds the core team's internal notes. Read
  `private/CLAUDE.md` as well as this file: it adds project process and history that is not published.
  Never copy its content into tracked files, commit messages or anything public.

## Working tree
- Work on a branch, ideally in a worktree under `worktrees/` (gitignored):
  `git worktree add worktrees/<name> -b <branch> <base>`. A worktree has its own submodule checkout:
  run `git submodule update --init` in it, and check that `libs/visage` is at the pinned commit.
- Don't use `git stash` when several worktrees share one repository: the stash is shared between them.
- `git commit -- <file>` commits the whole file. Before committing a shared file (`CMakeLists.txt`,
  `tests/CMakeLists.txt`), check `git diff HEAD -- <file>` holds only your hunks.
- A commit that names source paths in a build file must leave the repository buildable from a fresh clone:
  `git cat-file -e HEAD:<path>` for every path it names. Untracked files build locally and nowhere else.

## Build
- `. scripts/env.sh`, then `cmake -B build -G Ninja && ninja -C build`. `env.sh` selects g++-14, lowers the
  build's CPU and I/O priority, and reads an optional untracked `.env.local` at the repository root, where
  `FEATHERS_TOOLCHAIN_DIR` can point at a directory holding `cmake/bin` and `ninja-build/bin`.
- `-DFEATHERS_BUILD_TOOLS=ON` builds the test hosts (`guihost`, `bitwighost`, `cyclehost` for editor
  open/close/reopen) and the capture tools. GUI and flush-seam behaviour is verified there, not in the
  standalone build.
- Don't edit sources while your own build is running: a header changed mid-build can leave stale objects.
  Touch the file and rebuild.
- `scripts/clang-preflight.sh build` syntax-checks under clang with the arm64 SIMDe road; run it before a
  push meant for a macOS CI build. `scripts/sanitize-gate.sh` builds the lifetime suites under ASan in
  `build-asan/` and runs them one process per case: it catches use-after-free that a green ctest cannot.
  It sanitises the tree the script lives in, so run the copy in your own worktree.
- On a shared machine, build and run only the binaries you touched while iterating, and run the full
  `ctest` once per milestone.

## Tests
- `ctest --test-dir build --output-on-failure`. The widget suite is six binaries under `tests/gui_widgets/`
  (controls, explore, lane, dahdsr, devices, mod_ring) over one fixture library.
- Run one case by name with `scripts/run-cases.sh <binary> "<case name>"`, never a bare `--test-case=`:
  doctest splits that filter on commas and many case names contain one, so the bare form can report
  success for a case it never ran.
- GUI suites run headless on a private display: `scripts/venue-xvfb.sh run -- <cmd>` (lavapipe Vulkan,
  `VISAGE_FULL_REDRAW=1`). It is software rasterisation, not a test of the GPU path.
- `vox-voice` and `flagship` are Linux-x86-64-only (they assert bake identity to the bit) and carry
  microsecond bounds; a red on them under heavy load is re-run alone before it is a finding.

## Proving a test proves something
- **Known-bads before believing a green.** Break the code a test is meant to guard and watch it go red.
  Run known-bads with `scripts/run-cases.sh --each <binary>`: one process per case, so a crash cannot end
  the run early and hide the cases nearest the fault, and the tally is checked against the binary's count.
- **A known-bad is applied only once it is in the built artefact.** The compiler may delete a mutation (a
  dead allocation, an unobservable store). A red known-bad proves itself; a green one needs the object
  checked (`objdump -dCr`) or an observable mutation.
- **Assert the value, not a rendered difference.** "The output changed" passes whenever anything changed,
  and is weakest against partial effects, which are the subtle bugs. Expose the quantity through a
  test-only seam and compute the expected value by an independent restatement; keep any difference check
  only as a second assertion showing the first is not vacuous.
- **A repaint cannot be observed headlessly**, so the code that delivers one reports it (`notify()` returns
  how many frames it told).
- **Windowed captures are evidence only when the venue could have shown the other outcome.** Hash the
  frames (identical frames from an obscured or asleep window look like a clean run), and capture under
  `FEATHERS_REDUCED_MOTION=1`, because the editor animates and two frames differ even when nothing updated.
- Test fixtures take their values from the platform's contract, never from what our code produces.
- A model is evidence about mechanism, not magnitude: when a decision turns on which side of a threshold a
  number falls, measure it. Measure on the configuration users actually run (the default engine and mode).

## Engine rules
- **No allocation on the audio thread after activate.** That includes lazily built statics.
- `-ffp-contract=off` on the character, evaluation and test translation units: bit-identity tests depend on it.
- The character layer renders in float only (`src/dsp/morph_renderer.*`, `stair_renderer.*`); its
  fixed-point renderers were removed, and comments that cite them describe what the float code replaced.
- **"Done" is decided by the output.** Any sleep, idle, bypass or quiet decision must hold for the output
  of the whole chain after it: CLAP_PROCESS_SLEEP also requires an exactly silent block. Tailed devices
  must reach exact zero (they are in `test_fx_devices`' silence sweep); devices that sustain by design stay
  awake, with no timeout. Copy `test_clap_render`'s reverb-tail case for any new tail or idle path.
- **An address is not an identity under a fixed pool.** A slot gets reused, so a cache keyed on a pointer
  is ABA-prone (worst with two slots). Key on a monotonic build serial stamped where the thing is built.
  A double-buffered publish must not happen while a reader is inside a block (use a drain point).
- **Where code looks redundant, find out what the redundancy buys before removing it.** Several such
  "waste" sites in the voice and steal paths are load-bearing.

## Patch state and the schema ledger
- Every stored value is named by a key (`ParamDesc::key`, `SourceDesc::key`, `Traits::extraStateKey`),
  `[a-z0-9_.]`, unique in its device. Inserting, removing or reordering controls, and growing or reordering
  a Choice (stored by label), does not break saved patches.
- A key names a meaning. Rename a control and keep its key; change what its number means (units, rescale,
  switch to scaler) and give it a new key, moving the old one to `Traits::retiredKeys` with a date and
  reason. Never reuse a key. A renamed Choice label gets a `ChoiceAlias`.
- Run `test_state_ledger` after any device edit: a new line is added, a harmless change is refreshed with
  `FEATHERS_LEDGER_WRITE=1`, and a meaning change is refused until it has a new key or a `Traits::schema`
  bump. `kStateVersion` moves only when the state's own shape changes (see `state_block_order.h`).
- **Random and captured state persists with the patch**: seeds, held sample-and-hold values, Turing
  registers, freezes. Where a value can be derived from the seed and song position, derive it instead, so
  a reload reproduces it by construction. A seed taken from anything incidental (a construction count, an
  address, the clock) is a bug. Audio buffers are not persisted. Anything that deliberately does not
  persist says why in the code. The test shape: capture, save, load into a fresh instance, compare.

## UI rules
- **Honesty rule:** a control is enabled only if it changes sound through a real parameter. Verify by
  reading the engine, not by searching for parameter ids. A control that only an engine switch could enable
  is hidden, not greyed.
- **A device's window is a superset of its card**: every control on the card is also in the expanded window,
  in every state. `test_gui_widgets_tooltips`' "a device's window offers every control its card offers"
  enforces it through `ui::ControlFace`; a frame that drives a device control must declare it there.
- **No multivalue toggles.** A control shows the choices before one is applied: `SwitchChip` for two states,
  `SegmentedControl` when all fit on the face, `ValueBox` with `showChoiceMenu()` otherwise.
- **A scale change moves nothing logical**, so a `setBounds` guarded on the logical rectangle is skipped by
  it. Re-set bounds unconditionally (it de-duplicates) and hook `onDpiChange`; assert native widths.
- No user-facing string names the hardware the ports derive from (`test_device_tooltips`).
- Render geometry before believing it, and verify a visual fix against a known-bad build.
- `mock/feathers-ui-mock.html` is the UI spec; the design tokens are generated from it at build time.

## Files and licences
- Tabs. New files carry the Owlet Records GPL header ("This file is part of Feathers."), except under
  `libs/feathers-device-sdk/`, where a file carries the SPDX MIT header and includes only the standard
  library and other SDK files (`test_composite_surface` enforces both).
- Static linking: the `runtime-deps` test checks the plugin's runtime dependencies.
- `libs/` is vendored (except the SDK): don't edit it in place.

## Further reading
- `docs/sdk/design.md` — the device SDK: what is MIT, the include graph, the proposed binary interface.
- `README.md` — building, the device list, licensing.
