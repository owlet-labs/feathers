/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "rack/rack_engine.h"
#include "device/registry.h"
#include "device_includes.inc"
#include "plugin/params.h"
#include "backplane/routing.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// EVERY DEVICE THIS BUILD SHIPS, IN ONE LIST — and the only file in the plugin that is allowed to
// name one.
//
// THIS IS THE FILE THAT LETS params.h STOP NAMING DEVICES, which is the whole point of phase 1.
// params.h reads two device headers today:
//
//     #include "fx/filter/filter_types.h"        // for the fx destination list
//     #include "mod/dahdsr/dahdsr_types.h"       // for kRackControlsPerSlot
//
// and the second is the deep pin: `kRackControlsPerSlot = device::modulatableIn(
// mod::dahdsr::kParamDescs)` sizes the ENTIRE rack destination space from one device at compile
// time, so every slot is assumed to hold something exactly that wide. A modulator with a different
// control count cannot exist however large the slot pool grows. The include is the pin; the count is
// only its symptom.
//
// WHAT REPLACES IT IS NOT A BIGGER CONSTANT. It is a stride declared where no device can see it
// (the retired control stride, in the device vocabulary) plus a RUNTIME answer to "what is actually
// in this slot", which is what this table provides. params.h then includes device/registry.h alone —
// bounds, no devices — and the plugin's parameter contract stops knowing that a DAHDSR exists.
//
// AND THE CHECK THAT THE BOUND IS HONEST LIVES HERE, not there, for the same reason: a static_assert
// over the real devices needs to see the real devices. A bound declared where nothing can measure it
// against reality is a bound that gets silently exceeded; a bound declared away from the devices and
// checked next to them is neither.
//
// REGISTRATION IS A TABLE ENTRY, WHICH IS THE POINT. Phase 1 is, by design, "open interface, builtins
// only" — no dlopen, no ABI to freeze, no sandboxing. What that buys is that a builtin registers by
// EXACTLY the mechanism a guest would, so the tiebreaker the modulator-rack doc names stays
// answerable: could a device written by somebody who has never read this repo be dropped in and look
// and behave native? Everything below this line is a `device::Descriptor` and nothing else.

namespace feathers {

/// EVERY DEVICE, EFFECTS AND MODULATORS TOGETHER, IN ONE TABLE.
///
/// NOT TWO TABLES SPLIT BY LANE, and that is deliberate rather than lazy. A device's lane is not a
/// property of the device — it is a property of where a patch put it — and the moment a note effect
/// both passes notes and publishes a modulation source a lane-split table has to
/// hold it twice or pick a side. What decides where a device MAY sit is its own declarations: a
/// device that publishes outputs can be a source, a device with modulatable controls can be a
/// destination, and most devices are both.
inline constexpr device::Descriptor kDevices[] = {
#include "device_list.inc"
};

inline constexpr size_t kDeviceCount = std::size(kDevices);


//------------------------------------------------------------------//
// THE BOUNDS, CHECKED AGAINST THE DEVICES THAT ACTUALLY EXIST.      //
//------------------------------------------------------------------//

/// The widest modulatable-control count in the table, computed rather than asserted, so the message
/// below can say what the real number is.
constexpr size_t widestDeviceControls() {
	size_t widest = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.modulatableCount() > widest) {
			widest = d.modulatableCount();
		}
	}
	return widest;
}

constexpr size_t widestDeviceOutputs() {
	size_t widest = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.outputCount() > widest) {
			widest = d.outputCount();
		}
	}
	return widest;
}

// A DEVICE THAT DOES NOT FIT THE SOURCE STRIDE IS A BUILD ERROR, NOT A RUNTIME SURPRISE. (Controls have no stride or cap any more: 2026-10-19.)
//
// This is the check that makes the split worth having. The stride is declared in the device
// vocabulary where it can name nothing; without this assert, adding a device wider than the stride
// would compile cleanly and then quietly write one slot's controls over the next slot's — the same
// class of silent annexation that isWeightSource and isPuckDestination each fell into, and which
// this project has now paid for three times.
//
static_assert(widestDeviceOutputs() <= device::kMaxDeviceOutputs,
              "a device publishes more modulation outputs than the source stride allows — see "
              "device::kMaxDeviceOutputs");

// **AND A DEVICE'S PICTURE MUST FIT THE RACK'S READING STORE, checked here for the same reason and
// moved here for one more.** It lived in `rack_engine.h`, which reached across into
// `fx/sklitter/sklitter_types.h` to make it — and the rack must not include a device, because a device
// may HOLD a rack. This file is the one place that already sees both sides, which is what it is for.
//
// It is still SKLITTER's count by name rather than a sweep of `kDevices`, because a reading count is
// not on `Descriptor`: only the devices that publish a picture declare one, and SKLITTER's is the
// widest by an order of magnitude. A second such device adds a line here.
//
// **WHAT THE MOVE STOPPED COVERING, NAMED — because "it still fires" is a claim about where, and the
// where changed.** A namespace-scope `static_assert` in a header fires only where that header is
// INCLUDED, so relocating one can silently put it somewhere that agrees with everything. MEASURED by
// perturbation rather than read: `kMaxReadings` 144 → 8 emits the error in **18 translation units**,
// via `plugin/synth.cpp`, `plugin/gui_bridge.cpp` and `plugin/audition.cpp` — and `src/rack/rack_clap.cpp`
// includes this file for `hostCatalogue()`, so the STANDALONE RACK sees it too. Every binary this repo
// ships assembles `kDevices` and therefore instantiates this.
//
// **The uncovered build is a `feathers-rack`-only one holding a device that never reaches `kDevices`.**
// No such build exists. It is left uncovered DELIBERATELY: an assert inside `feathers-rack` would have
// to name `fx::sklitter::kReadingCount`, which restores the rack→device dependency the layering lift
// exists to remove — a `.cpp` dodges the header cycle but not the direction — and would make
// `tests/CMakeLists.txt`'s boundary comment claim something untrue again. Closing this gap would break
// the instrument that measures the thing the gap is about. Whoever creates that build is the person
// who should decide where this claim lives.
static_assert(fx::sklitter::kReadingCount <= rack::RackEngine::kMaxReadings,
              "a device publishes more readings than the rack stores; raise RackEngine::kMaxReadings — "
              "the clamp it guards is silent, which is why this is a compile error");
// AND RESONATE'S RING PICTURE (161 floats), which adds its own line for the same reason.
static_assert(fx::resonate::kReadingCount <= rack::RackEngine::kMaxReadings,
              "Resonate's picture is wider than the rack's reading store; raise RackEngine::kMaxReadings");
// **THE EQ, THE SECOND PICTURE** (its analyser frame): the line the note above asks for.
static_assert(fx::eq::kReadingCount <= rack::RackEngine::kMaxReadings,
              "the EQ publishes more readings than the rack stores; raise RackEngine::kMaxReadings");
// THE PHASER/FLANGER'S SWEEP READINGS — ten today, a line here so a later picture cannot outgrow the store quietly.
static_assert(fx::phaser::kReadingCount <= rack::RackEngine::kMaxReadings,
              "the Phaser/Flanger publishes more readings than the rack stores; raise RackEngine::kMaxReadings");
// THE DELAY'S TAPS STRIP (backlog LF) — the live heads, the multi-tap geometry and up to sixteen grains.
static_assert(fx::delay::kReadingCount <= rack::RackEngine::kMaxReadings,
              "the Delay's taps strip publishes more readings than the rack stores; raise RackEngine::kMaxReadings");
static_assert(fx::spectral::kReadingCount <= rack::RackEngine::kMaxReadings,
              "Spectral's input spectrum publishes more readings than the rack stores; raise RackEngine::kMaxReadings");

/// EVERY TAG IS DISTINCT, because a tag is what a saved chain record stores and two devices sharing
/// one would make a patch ambiguous about which was meant. The param table's own name-distinctness
/// case makes the same argument one level down; this is that argument for devices.
constexpr bool deviceTagsDistinct() {
	for (size_t i = 0; i < kDeviceCount; ++i) {
		for (size_t j = i + 1; j < kDeviceCount; ++j) {
			if (device::sameTag(kDevices[i].traits.tag, kDevices[j].traits.tag)) {
				return false;
			}
		}
	}
	return true;
}

static_assert(deviceTagsDistinct(), "two devices declare the same four-byte tag");

/// EVERY BUILTIN FILES ITSELF UNDER A HEADING. A guest's `None` is survivable — the menus file it under
/// "Other" — but a device of ours without one is an omission, and this is where it is caught.
constexpr bool everyDeviceCategorised() {
	for (const device::Descriptor& d : kDevices) {
		if (d.category == device::Category::None) {
			return false;
		}
	}
	return true;
}
static_assert(everyDeviceCategorised(),
              "a registered device declares no `.category` — give its descriptor one (device::Category)");

/// **EVERY ENABLE IS A MODULATION DESTINATION (backlog KW + "`Enabled` should be a modulation target").** A gate at 0.5
/// (`device::enableOn`); the engine crossfades a flip and keeps the device built and warm, so routing it is free. A device
/// whose header chip could not be routed while every other one could would be the guard asymmetry this catches.
constexpr bool everyEnableModulatable() {
	for (const device::Descriptor& d : kDevices) {
		const int e = d.traits.enableControl;
		if (e >= 0 && (static_cast<size_t>(e) >= d.params.size() || !d.params[static_cast<size_t>(e)].modulatable)) {
			return false;
		}
	}
	return true;
}
static_assert(everyEnableModulatable(), "a device's enable control is not `.modulatable` (backlog KW)");

//------------------------------------------------------------------------------------------------//
// EVERY STORED THING HAS A KEY (backlog JF step 1b)                                                 //
//------------------------------------------------------------------------------------------------//
//
// **A COMPILE ERROR, NOT A TEST, because the sessions adding controls are not the session that added
// keys.** A patch stores each control under `ParamDesc::key`, each output a route names under
// `SourceDesc::key`, and a device's extra state under `Traits::extraStateKey`; a control merged in without
// one would save nowhere. `test_state_ledger.cpp` names the offending device and key when this fires.

/// Every control, output and extra-state key is present and `[a-z0-9_.]`; controls, the extra state and
/// the keyed state blocks share one namespace and no two collide; outputs are distinct among themselves.
constexpr bool deviceKeysValid(const device::Descriptor& d) {
	if ((d.traits.extraState != 0) != (d.traits.extraStateKey != nullptr)) {
		return false;
	}
	std::vector<const char*> controls;
	for (const device::ParamDesc& p : d.params) {
		if (!device::validStateKey(p.key)) {
			return false;
		}
		controls.push_back(p.key);
	}
	if (d.traits.extraStateKey != nullptr) {
		if (!device::validStateKey(d.traits.extraStateKey)) {
			return false;
		}
		controls.push_back(d.traits.extraStateKey);
	}
	// THE KEYED BLOCKS (backlog NM): each named, sized, readable and restorable, and in the controls' namespace.
	if (d.state != nullptr) {
		if (d.state->read == nullptr || d.state->restore == nullptr || d.state->blocks.empty()) {
			return false;
		}
		for (const auto& b : d.state->blocks) {
			if (!device::validStateKey(b.key) || b.floats == 0) {
				return false;
			}
			controls.push_back(b.key);
		}
	}
	if (!device::keysDistinct(controls)) {
		return false;
	}
	std::vector<const char*> outputs;
	for (const device::SourceDesc& s : d.outputs) {
		if (!device::validStateKey(s.key)) {
			return false;
		}
		outputs.push_back(s.key);
	}
	return device::keysDistinct(outputs);
}

/// A retired key names nothing any more — no control, no output, not the extra state.
constexpr bool retiredKeysUnused(const device::Descriptor& d) {
	for (const device::RetiredKey& r : d.traits.retiredKeys) {
		if (!device::validStateKey(r.key)) {
			return false;
		}
		for (const device::ParamDesc& p : d.params) {
			if (device::sameKey(p.key, r.key)) {
				return false;
			}
		}
		for (const device::SourceDesc& s : d.outputs) {
			if (device::sameKey(s.key, r.key)) {
				return false;
			}
		}
		if (d.traits.extraStateKey != nullptr && device::sameKey(d.traits.extraStateKey, r.key)) {
			return false;
		}
		for (size_t b = 0; d.state != nullptr && b < d.state->blocks.size(); ++b) {
			if (device::sameKey(d.state->blocks[b].key, r.key)) {
				return false;
			}
		}
	}
	return true;
}

/// An alias maps a label the control no longer has onto one it has now.
constexpr bool choiceAliasesSound(const device::Descriptor& d) {
	for (const device::ParamDesc& p : d.params) {
		for (const device::ChoiceAlias& a : p.choiceAliases) {
			bool fromLive = false;
			bool toLive = false;
			for (const char* label : p.names) {
				fromLive = fromLive || device::sameKey(label, a.from);
				toLive = toLive || device::sameKey(label, a.to);
			}
			if (a.from == nullptr || fromLive || !toLive) {
				return false;
			}
		}
	}
	return true;
}

constexpr bool everyDeviceKeyed() {
	for (const device::Descriptor& d : kDevices) {
		if (!deviceKeysValid(d)) {
			return false;
		}
	}
	return true;
}
constexpr bool noRetiredKeyReused() {
	for (const device::Descriptor& d : kDevices) {
		if (!retiredKeysUnused(d)) {
			return false;
		}
	}
	return true;
}
constexpr bool everyChoiceAliasSound() {
	for (const device::Descriptor& d : kDevices) {
		if (!choiceAliasesSound(d)) {
			return false;
		}
	}
	return true;
}
static_assert(everyDeviceKeyed(),
              "a device control, output or extra state has no valid key, or two share one — every ParamDesc "
              "and SourceDesc starts with its key (`[a-z0-9_.]`, unique in the device, a MEANING: never reuse "
              "one); see ParamDesc::key. test_state_ledger names which");
static_assert(noRetiredKeyReused(), "a key on a device's `retiredKeys` list names something again — never reuse a key");
static_assert(everyChoiceAliasSound(),
              "a ChoiceAlias maps FROM a label the control still has, or TO one it does not have");

/// WHETHER THIS SOURCE MAY REACH THIS DESTINATION AT ALL — THE ONE AUTHORITY.
///
/// params.h states why there must be exactly one: every road that can create a route asks this, and
/// so does every surface that offers one — the knob's `mappable`, which is both the armed glow and
/// the gate on the drag, and the loader, so a patch edited by hand cannot carry in a route the
/// editor would refuse to draw. Two answers would be a glow that lies.
///
/// (`feathersAxes()` STOOD HERE and moved DOWN once more, into `params.h` beside the five constants it
/// is made of, when `ParamSet` began compiling the editor's routes into those axes itself — kill-lanes
/// step 1. Every caller above resolves it through the include chain it already had.)

// **FEATHERS' OWN `maySourceRouteTo` STOOD HERE, AND IS DELETED (kill-lanes step 5).** It took a
// `DeviceSlots` and two flat coordinates, wrapped them as a `SlotView`, and handed them to the
// backplane's coordinate overload of `mayRoute` — which compared two BAND-DERIVED RANKS for the
// ordering half. Step 4b established that a rank comparison is not the rule (a rank is a position in
// one linearisation of a partial order, and adding a route changes it), so this was answering with a
// rule the product no longer had.
//
// **THE AUTHORITY IS `ChainRecord::mayRoute`**, which asks the graph whether the route would close a
// cycle; a surface reaches it through `ParamBridge::mayRoute`. That is one authority rather than two,
// which is what this file's own warning about the arm-glow bug asks for: two implementations of one
// question is the shape that produced it.
//
// `detail::maySourceRouteToIgnoringDevice` in `params.h` SURVIVES and is unrelated — it is the host's
// own policy half (a weight may not reach the puck), which the real authority takes as its
// `CorePolicy`.

/// THE DEVICE WITH THIS TAG, OR NULLPTR — how a loader turns four bytes out of a file into something
/// it can ask questions of. This is what replaced the hard-coded memcmp against one device's tag.
constexpr const device::Descriptor* deviceForTag(const char* tag) {
	for (const device::Descriptor& d : kDevices) {
		if (device::sameTag(d.traits.tag, tag)) {
			return &d;
		}
	}
	return nullptr;
}

/// **THIS BUILD'S DEVICES, AS A CATALOGUE** — what Feathers and the standalone rack hand a device that
/// holds devices, and what a rack at the top of its own plugin is built with. The rack engine never
/// reads `kDevices` itself: it may be built separately from whatever hosts it, so the list is
/// whatever its host says it is. This is the host saying it.
/// **TEST ONLY — ONE DEVICE THIS BUILD DOES NOT SHIP, served by `hostCatalogue` beside `kDevices`** (backlog JM,
/// assumption L11). The standalone rack builds every device through the catalogue, so a CLAP-level case can seat
/// a device whose behaviour it controls — a pure delay declaring a latency — without one being registered, which
/// would put it in the browser, the ledger and every sweep. Null in the product, where the lookup below costs
/// one compare. Set it before the plugin is made; set it back to null after.
inline const device::Descriptor*& seatedDeviceForTest() noexcept {
	static const device::Descriptor* seated = nullptr;
	return seated;
}

inline device::Catalogue hostCatalogue() {
	return device::Catalogue { nullptr, [](void*, const char* tag) {
		const device::Descriptor* seated = seatedDeviceForTest();
		if (seated != nullptr && device::sameTag(seated->traits.tag, tag)) {
			return seated;
		}
		return deviceForTag(tag);
	} };
}

} // namespace feathers
