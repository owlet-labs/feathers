# The Feathers Device SDK — a permissive SDK beside a GPL Feathers

This note describes the split between the Feathers Device SDK (`libs/feathers-device-sdk/`, MIT) and
Feathers itself (GPL-3.0-or-later): what is in the SDK today, what still lives on the GPL side and why, the
include graph that decides it, the guards that keep the SDK clean, the proposed binary interface for devices
shipped as their own modules, and the stages that remain. It is not legal advice.

## 0. The goal

A third-party device, open or closed, should be able to adopt Feathers' device vocabulary and modulation
natively: declare its controls in Feathers' terms, publish modulation sources, be routed to, and draw its own
card or take the generic one. That needs the headers it compiles against under a permissive licence, because a
closed device cannot include GPL headers. **Feathers stays GPL-3.0-or-later**; only the device contract is MIT.

The standard has three tiers: (1) the required interface — the vocabulary plus `Modulatable`, what a widget
answers to join modulation; (2) stock widgets, opt-in; (3) theme roles, opt-in. Tier 1 is this note's subject;
tiers 2 and 3 are §5.

## 1. Provenance

All files in the SDK are Owlet Records' own work, so relicensing them under MIT is Owlet's decision. The
contributor licence agreement keeps that relicensing right for future contributions.

One header is held back on provenance grounds: `src/device/zone_param.h` is derived from ported firmware code,
and stays on the GPL side until that provenance has been reviewed. Two moved files mention a note-naming
convention (`device.h`'s `Unit::Note` comment and `format_value.cpp`'s note names: "60 is C3", octave
`n/12 - 2`). That is a convention plus a one-line formula, not copied code.

## 2. What is in the SDK, what stays GPL, and why

### 2.1 In `libs/feathers-device-sdk/` (MIT) — one source of truth

| file | what it is | includes (all std or SDK) |
|---|---|---|
| `include/device/device.h` | the vocabulary: `Unit`, `ParamDesc`, `Traits`, `ZoneName`, `ChoiceAlias`, `RetiredKey`, `FaceSection`, `TransportClock`, key validation | std |
| `include/device/instance_id.h` | `DeviceInstanceId` | std |
| `include/device/extra_state.h` | the extra-state snapshot (the road `ModOps::publishExtraState` writes) | instance_id |
| `include/device/state_blocks.h` | `StateOps`, keyed state blocks | extra_state |
| `include/device/ears.h` | stereo ears, control masks | std |
| `include/device/macro_type.h` | Knob / Latch / Momentary | std |
| `include/device/tick_window.h` | a source's tick as samples (smoothing) | std |
| `include/device/transport_lock.h`, `transport_snap.h` | song-locked phase; sample-snapped position | std |
| `include/device/latency.h` | declared latency, the delay a host inserts | std |
| `include/device/format_value.h`, `src/format_value.cpp` | a control's value as text, from its declaration | device.h |
| `include/device/registry.h` | what a host runs: `Descriptor`, `SourceDesc`, and the ops tables `AudioOps`, `ModOps`, `NoteOps`, `ContainerOps`, `TextOps` (plain function pointers over host-owned storage) | device, ears, endpoint, state_blocks, modulatable, slot_bus |
| `include/device/modulatable.h`, `src/modulatable.cpp` | the one interface a widget implements to be a modulation destination (tier 1) | endpoint |
| `include/device/endpoint.h` | what a route's two ends are | transfer_curve |
| `include/device/transfer_curve.h` | a route's transfer curves: the curve codes, their coefficients, labels and every curve's shape. The φ curve's evaluation is the host's, so `shapeTransferWith` takes it as a callable | std |
| `include/hostslots/slot_bus.h` | the lock-free bus a host's automation slots cross threads on | std |

Each carries `SPDX-License-Identifier: MIT` and an Owlet copyright (§6). `#include "device/device.h"` resolves
unchanged everywhere: the root `CMakeLists.txt` puts the SDK's `include/` on the directory-wide path before any
target, and `feathers-device` links `feathers-device-sdk` (which owns `format_value.cpp` and `modulatable.cpp`).

**`slot_bus.h` is host-side machinery** and is in the SDK only because `registry.h`'s `HostSlotLink` names its
bus. Splitting that link out of `registry.h` later would let `slot_bus.h` leave the SDK again.

### 2.2 Still in `src/device/` (GPL), and why

| file | why it has not moved |
|---|---|
| `transfer.h` | the φ curve's EVALUATION, which is Feathers' character code (`character/phi_triangle.hpp`). It includes the SDK's `transfer_curve.h` and gives the host `shapeTransfer`, which every resolve, readout and picture calls; it is bit-identical to the single header it was cut from (`test_transfer_pin`) |
| `route_resolve.h` | the host's route resolution, which evaluates transfers |
| `zone_param.h` | derived from ported firmware code (§1); held until its provenance has been reviewed |
| `background.h/.cpp` | the `Deriver` handle is device API, but the `.cpp` is the host's process-wide worker thread; to be split, handle in the SDK and worker in Feathers, reached through a host-services table (§4) |
| `chain_tail.h`, `device_groups.h` | host-side grouping over the registry |
| `subscribers.h`, `sidechain.h` | host-side machinery (listener tokens; host-buffer silence detection) — not SDK material |

**A device can now declare itself AND hand the host a `Descriptor` against the SDK alone.**

### 2.3 The include graph (measured over every `#include "…"`)

```
SDK (MIT)
device.h  ears.h  macro_type.h  tick_window.h  transport_lock.h  transport_snap.h  latency.h  format_value.h
instance_id.h ── extra_state.h ── state_blocks.h
transfer_curve.h ── endpoint.h ── modulatable.h
registry.h ──► device.h, ears.h, endpoint.h, state_blocks.h, modulatable.h, hostslots/slot_bus.h
                                        │
                                        ▼   (GPL → MIT: allowed)
src/device (GPL)
transfer.h ──► device/transfer_curve.h + character/phi_triangle.hpp   (the φ evaluation, injected)
route_resolve.h ──► transfer.h
```

No SDK file reaches outside the SDK; the only remaining MIT → GPL edge was `transfer.h → phi_triangle.hpp`,
cut by moving the curve codes into `transfer_curve.h` and passing the φ evaluation in as a callable.

## 3. The guards

- **The SDK's own targets see only `include/` and std.** `feathers-device-sdk` and the example are defined in
  the SDK's CMakeLists with no `src/` on their path, so a reach into Feathers fails their compile. (They also
  see `build/generated`, a directory-wide include set earlier in the root file; nothing in the SDK uses it.)
- **`test_composite_surface` "the device SDK includes only itself and the standard library, so its MIT licence
  holds"** walks every SDK file: each quoted include must exist under the SDK's `include/`, and each file must
  carry the MIT SPDX line, with non-vacuity floors. Known-bad check: appending
  `#include "character/phi_triangle.hpp"` to `latency.h` turns the case red, naming the line.
- **`device-sdk-example`** (ctest): `example/sine_mod.h` is a sine modulator built only against the SDK —
  keyed `ParamDesc`s with compile-time key checks, `Traits`, state in host-owned storage, and its own
  `Descriptor` with `ModOps`. `example/sine_mod_check.cpp` drives it through that descriptor alone.
- **`test_sdk_device`** "a device built against the SDK alone registers, runs as a modulator, and moves a
  routed control by value": the example registers in the host's rack and is routed to a filter's cutoff, and
  the cutoff is asserted by value.
- **`test_transfer_pin`**: every route transfer curve at every coefficient, and the φ curve with where each φ
  step peaks, are bit-identical to pinned outputs — the guard on the inline→out-of-line move of a float path.

## 4. The ABI boundary for a device shipped as its own binary

**What exists:** devices are a compile-time table (`plugin/device_registry.h`'s `kDevices`). Phase 1 is an open
interface with builtins only — no `dlopen`, no ABI to freeze, no sandboxing. The property that makes the SDK
possible: a builtin registers by exactly the mechanism a guest would — a `device::Descriptor` and nothing
else. So the shape is right; the binary boundary is not built yet.

**What is already C-ABI-shaped:** the ops tables. `AudioOps` is `stateBytes`, `stateAlign` and plain function
pointers taking `void* storage` and scalars/`const float*`; the host owns the bytes. `ModOps`/`NoteOps`/
`ContainerOps` follow the same rule. No virtuals, no exceptions across the boundary, no allocation contract
beyond `prepare`.

**What is C++-ABI-bound and must change for a cross-compiler binary:**
- `std::span` in `Descriptor::params/outputs`, `ParamDesc::names`, `Traits`' spans (retired keys, aliases,
  zone names, face sections) — layout is implementation-defined and differs between libstdc++ and libc++.
- `std::string` in `HostSlotCaption` (host-side, but in registry.h); `std::string` return from `formatValue`
  (a host-side utility, fine as a static library, never across the boundary).
- `Modulatable` is a C++ abstract class (vtable) — fine for widgets inside one binary, not across one.
- Default member initialisers and `bool` fields are fine in C if the struct is mirrored exactly.

**Proposal (not built):**
1. **Two layers.** A frozen C header `feathers_device.h` (`extern "C"`, POD only: `{ptr, count}` pairs instead of
   spans, `uint8_t` for bools, explicit enum widths, a `struct_size` first field for growth) — and the C++
   vocabulary (today's headers) as an inline, header-only convenience layer that builds the C structs from
   `constexpr` declarations. A device author writes C++ as now; the binary exports C.
2. **Discovery, CLAP-style.** A module exports one symbol, `const feathers_device_entry* feathers_device_entry_v1`,
   with `{ abi_version, init(host_services*), deinit(), count(), get(i) -> const feathers_device_descriptor* }`.
   Feathers scans a folder at activate (never on the audio thread), `dlopen`/`LoadLibrary`s each module, checks
   `abi_version` (major must match, minor ≤ host's), and appends its descriptors to the catalogue that
   `Catalogue` already models for containers.
3. **Host services** passed at `init`: the background-derivation worker (`Deriver`), logging, the transport
   clock. This is what lets `background.cpp` stay in Feathers.
4. **Identity:** a device tag becomes reverse-DNS (`com.vendor.device`) for guests; builtins keep their short
   tags. Keys stay as they are — the schema ledger is already per-device and key-based, so a guest's patch state
   needs no new mechanism.
5. **Versioning:** the C header carries `FEATHERS_DEVICE_ABI_MAJOR/MINOR`; the vocabulary's own stored meaning
   is already versioned per device by `Traits::schema` and keys.

**Open questions:** sandboxing (in-process only at first, as CLAP plugins are?); whether a guest's card may be
its own visage frame (needs the visage ABI to match — realistically only for guests built from the same visage
pin, i.e. source-distributed) or must be described declaratively (the generic card from descriptors works today
across any boundary).

## 5. Tier 2 and tier 3 — stock widgets and theme roles

visage is MIT (`libs/visage/LICENSE`, Copyright Vital Audio, LLC). So a widget layer that includes only visage
and the SDK could ship MIT. What blocks it today, by include:

| widget | GPL / Feathers includes |
|---|---|
| `theme.h` | `dsp/constellation.h` (for `kMaxSounds`), `chrome_hue.h`; and the generated `tokens` header (from `mock/feathers-ui-mock.html`, Owlet's and relicensable, but generated at build time from a GPL-tree file) |
| `knob.h` | `edit_history`, `macro_menu`, `mod_state`, `tooltip`, `type_in`, `value_box`, `notify_tray`, `theme` |
| `value_box.h` | `drag_acceleration`, `frame_watchers`, `tooltip`, `type_in` |
| `source_arm.h` | `mod_state`, `theme` |
| `fx_card.h`, `generic_card.h`, `device_control.h` | **`backplane/device_host.h`**, `card_selection`, `mod_readout*`, `mod_route_menu`, `card_meter`, `switch_chip`, `zone_coord_cell` |

With `modulatable.h` and `registry.h` now in the SDK, the floor needs, in order: `backplane/` relicensed (all
Owlet's), `theme.h`'s `constellation.h` include dropped (move the one `static_assert` to a Feathers TU),
`edit_history`'s generic stack split from `SoundIdentity`, and an audit of each remaining `plugin/ui/` helper.
Then an `sdk-ui/` library, MIT, depending on visage and the SDK. **Tier 3 (theme roles) is the cheapest:**
publish the role names and let the host fill them; the token values need not ship.

## 6. Licence text, headers, and how the GPL repository vendors the SDK

- `libs/feathers-device-sdk/LICENSE` — the MIT text, `Copyright (c) 2026 Owlet Records`.
- Every SDK file:
  ```
  /*
   * Copyright © 2026 Owlet Records
   * SPDX-License-Identifier: MIT
   *
   * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
   * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
   * which vendors this SDK, is GPL-3.0-or-later.
   */
  ```
  The GPL headers' §7 additional terms (attribution preservation) are dropped; MIT's own notice clause covers it.
- **Vendoring back:** GPL code may include MIT code; the combined Feathers binary is GPL-3, and the SDK files
  stay MIT. Today the SDK is an in-tree directory (`libs/feathers-device-sdk/`), built by `add_subdirectory`.
  Once published it becomes its own repository with `libs/feathers-device-sdk` a submodule at a pinned tag —
  the same shape as `libs/visage`. Feathers' third-party notices should name it once it is separate.
- New files under `libs/feathers-device-sdk/` carry the MIT header above, not Feathers' GPL header.

## 7. Stages

| stage | what | size | gate |
|---|---|---|---|
| **S0 (done)** | move the 11 clean headers + `format_value.cpp`; MIT headers; the include-walk test; the example | done | full build + ctest + ledger |
| **S1 (done)** | `slot_bus.h` into the SDK (it rides with `registry.h`'s host-slot link) | done | same |
| **S2 (done)** | `transfer.h` split: curve codes and shapes into `transfer_curve.h`, the φ evaluation injected and kept GPL; then `endpoint.h`, `modulatable.{h,cpp}` and `registry.h` (the ops tables and `Descriptor`) moved; the example registers a real `Descriptor` | done | `test_transfer_pin` (bit-identical curves), `test_sdk_device` (routed to a filter's cutoff, by value) |
| S3 | `zone_param.h` provenance review, then move | review | — |
| S4 | `backplane/` contract headers (axes, routing, device_host, sources, block_context) into the SDK; split `HostSlotLink` out of `registry.h` so `slot_bus.h` can leave | medium | — |
| S5 | the C ABI header + `dlopen` loader + host services (§4) | large | a guest `.so` built out-of-tree loads, routes and round-trips its state |
| S6 | floor widgets as an MIT `sdk-ui` library (§5) | large | the card-floor probe as a link test |
| S7 | separate repository + submodule; third-party notices | admin | — |
