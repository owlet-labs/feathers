/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "composite/composite_types.h"
#include "device/registry.h"

#include <cstddef>

// THE RACK, AS A DEVICE. Not a new kind of thing: the same `RackEngine` the standalone rack plugin
// runs, wrapped in the ops every device in the catalogue declares, so it sits in a chain like any
// effect — Feathers' fx lane, or another rack. A rack inside a rack is this, inside this. Nothing here
// knows how deep it is, and nothing here is a "nesting" feature.
//
//
// WHAT IT ADDS TO THE ENGINE, and it is only the outward surface D6 decided: an enable, a mix and a
// gain around the engine's output (ramped over 5 ms, `fx::busutil`'s arrangement — a step change in a
// gain is a zipper), and the engine's eight macros as the device's controls. Inside, the rack routes
// modulation exactly as the standalone rack does: by edges on its graph. There is no second
// modulation system here.
//
// ITS STATE IS THE RACK'S OWN FILE FORMAT. `saveState`/`loadState` are `saveRack`/`loadRack` over
// bytes, so a rack in a chain saves exactly what the standalone rack saves.

namespace feathers::rack {

extern const device::AudioOps kDeviceAudioOps;
extern const device::ContainerOps kDeviceContainerOps;

/// **HOW DEEP A LOAD MAY RECURSE.** A rack's contents are loaded recursively, and bytes do not bound
/// depth — a thousand racks each holding one rack is a small file — so without a limit a patch could
/// exhaust the stack. A rack this deep refuses a body rather than recurse into it.
///
/// **THE GAP THIS CONSTANT WAS SHAPED AROUND IS CLOSED (2026-09-20), so the number means something
/// different now.** It used to read: *"The limit applies on LOAD; a hand adding racks by hand is not
/// stopped at any depth, because a rack does not yet know how deep it is. So a structure deeper than
/// this could be BUILT and would lose its deepest levels on reload."* The hole was found from the
/// other side (it ought to be turtles all the way down) and the decision was to bound the runtime: if
/// nesting goes over 20 deep, or as far as the plugin can be saved, show a popup. A rack IS told its
/// depth now (`ui::DeviceNest::depth`), and `ChainColumn::nestingFull` refuses the add at exactly the
/// level this constant refuses the body. **So this is no longer a guard sitting far past anything a
/// hand builds; it is the player-facing limit, and it is reached by hand.**
///
/// **TWENTY-ONE, BECAUSE THE QUANTITY A PLAYER COUNTS IS `kMaxUsefulDepth` AND THE LIMIT IS TWENTY.** The
/// loader's test is `depth + 1 >= kMaxDepth`, so the deepest chain whose CONTENTS survive is one less
/// than this — see below, where that is named rather than left to be re-derived at each call site.
///
/// **AND RAISING IT COSTS NOTHING, MEASURED RATHER THAN ASSUMED.** The other thing that can refuse a
/// body is `kMaxBodyBytes`, and if it bit first it would be the real limit: it does not. A nesting at
/// the old limit saves in 2960 bytes — about 197 a level — so the 32 MiB ceiling binds at roughly
/// 170,000 levels (`test_rack_nesting.cpp`, "…are ONE number…"). Depth is the only constraint that
/// bites, by four orders of magnitude, and the recursion a hostile file can force is still bounded.
inline constexpr size_t kMaxDepth = 21;

/// **THE DEEPEST NESTING WHOSE CONTENTS SURVIVE A SAVE — DERIVED, NEVER CHOSEN.**
///
/// The loader refuses a body at `depth + 1 >= kMaxDepth`, so the chain at level `kMaxDepth` comes back
/// EMPTY: the container itself is named in its parent's body and reappears, and everything a hand put
/// inside it is gone. That distinction is invisible to a count of levels — a first draft of the case
/// below compared depths and could not see it — so the number a surface must bound to is this one, and
/// it is spelled once here instead of as `kMaxDepth - 1` wherever somebody remembers.
///
/// TWENTY, which is the chosen number and is what `kMaxDepth` was set to deliver.
inline constexpr size_t kMaxUsefulDepth = kMaxDepth - 1;
/// AND MODULATION REACHES EVERY DEPTH NESTING ALLOWS: a host's source must still be spellable at the deepest device
/// (`Source::asOuter()` turns into nobody past `kMaxOuterHops`). Raise the nesting limit past it and this refuses to
/// compile, instead of a host macro quietly stopping a few hosts short.
static_assert(device::Source::kMaxOuterHops >= kMaxDepth, "modulation must reach as deep as nesting allows");

/// **WHAT A SURFACE SAYS WHEN IT REFUSES AN ADD FOR DEPTH — lower case by design**
/// (2026-09-20): a congratulation rather than an error, because a player only reaches it by nesting a
/// device inside a device `kMaxUsefulDepth` times and that is worth marking.
///
/// **IT LIVES BESIDE THE BOUND RATHER THAN IN EITHER SURFACE, BECAUSE TWO SURFACES SAY IT.** The chain
/// column draws it as a disabled row in its add menu; the rack editor says it on its notify tray, which
/// is where that editor already words *"The rack is full"*. Each surface refuses in the voice it
/// already refuses in — and one constant is what makes the two provably the same sentence rather than
/// two copies that drift the first time somebody edits one.
///
/// A STRING HERE AND NOT IN A UI HEADER is deliberate: this file is where the number is, the number is
/// the whole reason the sentence exists, and `rack/` links no UI either way.
/// **THE LOADER'S TEST** (`InnerChain::loadState`, the Chain device's `loadState`): a body read while `open` bodies are
/// already being read (`containerLoadDepth`) is refused. A chain `d` chains down is read with `d - 1` open — the
/// host's own load is not a body — so this is also the test for how deep a chain may be and still come back full.
[[nodiscard]] constexpr bool loadRefuses(size_t open) noexcept { return open + 1 >= kMaxDepth; }
/// **WHETHER A CHAIN `depth` CHAINS DOWN CAN HOLD ANYTHING A FILE WILL GIVE BACK** — the loader's test, restated.
[[nodiscard]] constexpr bool chainHolds(size_t depth) noexcept { return depth == 0 || !loadRefuses(depth - 1); }
/// **THE ONE GATE EVERY ROAD THAT DEEPENS ASKS** (2026-10-31): may content `height` chains tall (a leaf 0, an empty
/// container 1, a container holding a container 2, …; `deviceHeight`) go into a chain `depth` chains down? Its
/// deepest chain would sit at `depth + height`, and that chain must be one the loader gives back. The add menu, a
/// site, a group, a paste, a drag, a preset and an undo all ask THIS, so none can build live what reloads empty.
[[nodiscard]] constexpr bool fitsNesting(size_t depth, size_t height) noexcept { return chainHolds(depth + height); }
// ONE NUMBER, NOT TWO THAT NEARLY AGREE: the deepest chain the gate lets a hand build is the deepest the loader fills.
static_assert(chainHolds(kMaxUsefulDepth) && !chainHolds(kMaxUsefulDepth + 1),
              "the gate and the loader must stop at the same chain");

inline constexpr const char* kNestingFullMessage =
    "you've done it. you've nested as much as you can nest. you win. congratulations.";

/// **THE LARGEST BODY A RACK CAN LEGITIMATELY WRITE** — a ceiling for a reader, from the rack's own
/// counts: sixteen devices at a 128-float row, eight macros, the routes the table can hold, and at
/// each level the racks below it. Generous by construction; what a reader bounds is a walk, not an
/// allocation any real patch makes.
inline constexpr size_t kMaxBodyBytes = size_t { 32 } * 1024 * 1024;

inline constexpr device::Descriptor kDeviceDescriptor {
	.traits = composite::kTraits,
	.params = composite::kParamDescs,
	.outputs = {},
	.audio = &kDeviceAudioOps,
	.mod = nullptr,
	.notes = nullptr,
	.container = &kDeviceContainerOps,
	.category = device::Category::Containers,
};

class RackEngine;
class RackBridge;

/// TEST ONLY — the engine inside a rack device's storage, so a case can put devices in it and read
/// them back the way the standalone rack's own cases do.
RackEngine& engineForTest(void* storage);

/// **THE CHAIN'S OWN BRIDGE** — the message-thread editing road into the rack inside a chain device: a
/// `RackBridge` over the inner engine, built when the device adopts its catalogue, drained by the device's
/// own process. What a chain's expanded view edits through, reached by the opaque handle the host's carrier
/// files for the instance (`ContainerBodies::attach`, `DeviceHost::containerSurfaceOf`). Null before the
/// device has adopted a catalogue.
RackBridge* bridgeOf(void* storage);
/// THE ENGINE INSIDE — for a case reaching a voice instance, which has no bridge (rack roadmap §8).
RackEngine* engineOf(void* storage);

/// TEST ONLY — the rack device's own three controls as it decoded them. `test_device_ranges`' row-codec
/// sweep requires every registered device to round-trip every control, and a device that decodes
/// straight into members has no row to hand back — the tracker's shape.
struct Reading {
	bool enabled = true;
	float mix = 1.0f;
	float gain = 0.5f;
};
Reading readingForTest(const void* storage);

} // namespace feathers::rack
