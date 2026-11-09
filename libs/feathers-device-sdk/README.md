# Feathers Device SDK (DRAFT — not published)

MIT-licensed (see `LICENSE`). The vocabulary a device uses to describe itself to a Feathers host — its
controls, keyed patch state, units and formatting — so that devices of ANY licence, open or closed, can
look and behave native in Feathers. Feathers itself is GPL-3.0-or-later and vendors this directory.

## What is here

- `include/device/device.h` — `ParamDesc`, `Traits`, `Unit`, keys and their compile-time checks, `TransportClock`.
- `include/device/state_blocks.h`, `extra_state.h`, `instance_id.h` — state a device carries beyond its controls.
- `include/device/ears.h`, `macro_type.h`, `tick_window.h`, `transport_lock.h`, `transport_snap.h`, `latency.h` —
  stereo ears, macro types, control ticks, song-locked phase, declared latency.
- `include/device/format_value.h` + `src/format_value.cpp` — a control's value as text, from its declaration.
- `include/device/registry.h` — what a host runs: `Descriptor`, `SourceDesc`, and the ops tables `AudioOps`,
  `ModOps`, `NoteOps`, `ContainerOps`, `TextOps` (plain function pointers over host-owned storage).
- `include/device/modulatable.h` + `src/modulatable.cpp` — the one interface a widget implements to be a
  modulation destination; `include/device/endpoint.h` — what a route's ends are.
- `include/device/transfer_curve.h` — a route's transfer curves: codes, coefficients, labels and shapes. The φ
  curve's evaluation is the host's, so `shapeTransferWith` takes it as a callable.
- `include/hostslots/slot_bus.h` — the lock-free bus a host's automation slots cross threads on.
- `example/sine_mod.h` — a sine modulator built against this directory and nothing else, with its `Descriptor`;
  `example/sine_mod_check.cpp` drives it through that descriptor alone. Feathers' `test_sdk_device` registers it
  in the host's rack and routes it to a filter's cutoff, by value.

## What is not here yet

The binary boundary: today a device is compiled into the host. A frozen C ABI and a module loader, the stock
widgets and the theme roles are later stages (Feathers' `docs/sdk/design.md`).

## Rules for files in this directory

- Every file carries `SPDX-License-Identifier: MIT`, names no person, and includes only the standard library and
  other files in this directory. Feathers' `test_composite_surface` walks the directory and fails on anything else.
- Tabs, C++20, no allocation on the audio thread — the same discipline as the host.

## Building

In Feathers it is built by `add_subdirectory`. Its targets see only `include/`:
`feathers-device-sdk` (static library) and `feathers-device-sdk-example` (run as the `device-sdk-example` test).
