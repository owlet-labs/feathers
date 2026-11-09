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

#include "device/device.h"
#include "device/registry.h"

#include <cstddef>

// A CHAIN, SEEN FROM OUTSIDE — the outward surface of a composite device.
//
// The chain-as-a-device design, step 6a.
//
//------------------------------------------------------------------------------------------------//
// WHAT THIS FILE IS, AND WHAT IT DELIBERATELY IS NOT                                               //
//------------------------------------------------------------------------------------------------//
//
// It is the DECLARATION of what a chain looks like to a host that holds one: a tag, a name, and
// eleven controls. It has no `AudioOps`, no `ModOps`, no state, and nothing constructs it. A
// composite cannot be RUN until three other things land (the doc's 6b, 6c, 6d), and this file is
// deliberately ahead of all of them, on step 1's pattern: new code beside existing code, landing
// with tests only and no host on it.
//
// **IT IS ALSO NOT REGISTERED IN `kDevices`, AND THAT IS THE HONESTY RULE.** A device in the
// catalogue is a device a browser offers and a hand can add. This one would then sit in a slot
// doing nothing, because `.audio` is null. It joins the catalogue in the commit that gives it ops.
//
//------------------------------------------------------------------------------------------------//
// THE CONTROLS ARE THE ESCALATION RULE, WRITTEN AS A DECLARATION                                   //
//------------------------------------------------------------------------------------------------//
//
// **DECIDED 2026-09-01 (vision D6): head, body, plus mix and gain.**
// A composite's card is its summary as the head and its MACROS as the body — the honesty rule read
// outward, because the macros are the only controls an outer host may route to (backplane §4.1) —
// plus mix and gain, the composite's own two.
//
// So `params` here is not a summary of the decision; it IS the decision, in the one form every host in
// this repo already reads. A route addresses a destination by its ORDINAL among a descriptor's
// modulatable controls (`Descriptor::controlOfNthModulatable`), and this descriptor's modulatable
// controls are exactly the macros, the mix and the gain. **A device inside the chain is not
// addressable through this surface because it is not in this span** — which makes the escalation
// rule a property of the vocabulary rather than a filter a picker has to remember to apply.
//
// **THAT IS THE OUTWARD SURFACE. IT IS NOT THE ONLY CROSSING — D9, and this comment has been wrong
// twice, so read the correction rather than the first two drafts.**
//
// Draft one said the other half of the rule was a refusal in `Graph::route`; D8 deleted that, because
// a composite's members are not nodes of the graph that holds it and there is no crossing edge to
// refuse. Draft two then said an inner device was "unnameable rather than merely un-routable", full
// stop. The objection is right, and the overstatement was this comment's.**
//
// WHAT IS ACTUALLY TRUE, MEASURED. Ordering is no obstacle: a modulator upstream of a composite is
// upstream of everything in it, trivially. And the SAVED form can already name an inner control — a
// route record is (kind, instance id, ordinal), and a container's ids are unique among the chains ITS
// HOLDER holds. What stops it is narrower: a
// LIVE destination resolves through `deviceDestinationOfOrdinal(slot, ordinal)` into a flat table
// bounded by `kModDestinationCount`, and an inner device holds its slot in the INNER table.
//
// **THAT SENTENCE SAID "GLOBALLY UNIQUE" AND "BORROWS ITS PARENT'S COUNTER" UNTIL 2026-09-20, AND THE
// CODE DISPROVED BOTH HALVES** (bolt-on-chains gap G3, measured in `tests/test_inner_chain.cpp`). Every
// `RackBridge` owned its counter, so two chains in ONE holder each issued id 1 and both answered
// `descriptorOf(1)` non-null. The fix is a counter per HOLDER, lent to each of its chains —
// **not per tree, because nothing could deliver a parent's counter**: `ContainerOps::adopt` hands over
// a `Catalogue` and nothing else, and there is no other seam. Holder-uniqueness is exactly what the
// four route verbs need, since each is dispatched through ONE holder's ops table and carries a
// `device::Destination{deviceId, index}` with no lane argument.
//
// **AND THE GUARANTEE IS NOT TOTAL, WHICH IS WORTH MORE THAN A ROUNDER CLAIM.** `applyPatchToRecord`
// (`rack/rack_state.cpp`) keeps a body's own id when `!record->holds(id)` — and `record` is the ONE
// lane being loaded, so a body whose ids overlap a SIBLING lane's is taken at face value and collides
// anyway. Our own save/load round trip cannot produce that (the ids were distinct when written); a body
// moved or copied between lanes could. Closing it needs a "taken elsewhere" predicate the holder
// supplies, which nothing asks for yet. Do not read the shared counter as more than it is.
//
// **SO OPACITY IS A DEFAULT, NOT A LAW** — DECISION D9: a container DECLARES whether its members may be
// routed to, on this vocabulary's own rule that null IS the capability. Our chain says yes; a
// third-party black box says nothing and stays shut, so the devkit boundary survives.
//
// AND THE EIGHT MACROS WERE DOING TWO JOBS, which is the error D6 baked in: (1) what the DAW sees and
// automates, which must be a fixed declared list because CLAP requires one, and (2) the crossing
// point for internal modulation, which needs no limit at all. The mechanism is identical either way —
// a route resolves to a value and the value reaches an inner control — so "direct" is only "an
// anonymous macro made on demand". Job 2 is out from under job 1's budget now.
//
// `backplane::Graph` still needs NO change for nesting, which is what D8 bought and is unaffected.
//
//------------------------------------------------------------------------------------------------//
// MIX AND GAIN DO NOT EXIST TODAY, AND THE MACROS DO                                               //
//------------------------------------------------------------------------------------------------//
//
// MEASURED 2026-09-10 on `06be4fe`: `grep -n 'bypass\|Mix\|Gain\|trim' src/rack/rack_engine.h`
// returns nothing. The rack has no mix, no gain and no bypass. Those three are NEW controls that
// arrive with the composite, so nothing here is a re-export of them and no existing behaviour
// disagrees with these defaults.
//
// The eight macros are the opposite case: the rack has presented exactly eight to its DAW since
// `rack_clap.cpp` was written, at 0..1 with a default of 0. `kMacroCount` now lives HERE so that
// both declarations are the same object rather than two numbers that agree today — the failure this
// repo names as *a person who remembers is not a mechanism*, and which cost it a whole flattened
// channel of instance ids once already (device_row.h's banner).

namespace feathers::composite {

/// **HOW MANY MACROS A COMPOSITE HAS.** Read by `rack_clap.cpp` for the rack's CLAP parameter list,
/// and by `kParamDescs` below, so the two cannot drift.
///
/// EIGHT IS NOT A DESIGN DECISION MADE HERE — it is the number the rack has been presenting to
/// Bitwig since it had a CLAP surface at all, moved rather than chosen. The decision this file makes
/// is that there is ONE of it.
inline constexpr size_t kMacroCount = 8;

/// THE ID BLOCK IS ASSIGNED IN THIS ORDER, so APPENDING IS THE ONLY SAFE EDIT — a device row is
/// index-aligned with this enum.
///
/// THE MACROS ARE LAST AND CONTIGUOUS, WHICH IS LOAD-BEARING RATHER THAN TIDY. `Macro1 + n` has to
/// be the n-th macro for a host that walks them, and a control inserted between two of them would
/// make that arithmetic silently wrong for every macro after the insert. The static_assert below
/// pins it.
enum class Param : size_t {
	Enabled = 0,
	Mix,
	Gain,
	Macro1,
	Macro2,
	Macro3,
	Macro4,
	Macro5,
	Macro6,
	Macro7,
	Macro8,
	kCount,
};

/// OFF/ON, declared here rather than shared — five device headers carry their own copy and that is
/// the vocabulary working as intended: a device includes nothing from the plugin and nothing from
/// its neighbours.
inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE FADER POSITION THAT IS UNITY. Same convention and same value as `fx::busutil`, deliberately:
/// two devices whose gain control reads differently at the same knob position is a trap for a hand,
/// not a design choice.
inline constexpr double kFaderUnity = 0.5;

inline constexpr const char* kTags[] = { "utility", "routing" };

inline constexpr device::Traits kTraits {
	.tag = "cmps",
	.name = "Chain",
	.hasCard = true,
	/// **AN EXPANDED VIEW IS THE WHOLE POINT OF THIS DEVICE**, since the body a hand needs is the
	/// inner chain and no card is going to hold one. Declared true here even though
	/// `makeExpandedView` has no entry yet — which is a state this repo has explicitly decided
	/// against (*"hasExpanded without a makeExpandedView entry"* is what gave), and so it is FALSE until 6f builds the view. Both halves or
	/// neither.
	.hasExpanded = true, // the inner rack's own editor, over the bridge inside the device (rack roadmap §6)
	/// **NO PER-VOICE ROAD, AND NOT BECAUSE ONE IS UNIMAGINABLE.** A per-voice chain is a coherent
	/// idea and probably a wanted one. device.h's standing rule is that a device which has not
	/// implemented a per-voice road must not claim one, because the host's answer to "poly is on" is
	/// to stop running it on the bus — so a claim here would silence the device rather than
	/// multiply it.
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	/// **THE SITE'S FURNITURE IS THIS DEVICE'S OWN MIX AND GAIN** (D1, 2026-10-29, lifting D3 for the Rack and the
	/// Chain): the device mixes, so a POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::Gain),
	.mixInDevice = true,
};

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT 
    {"chain_enabled", "Chain Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Whether the chain is in the signal path"},
    // **FULLY WET BY DEFAULT, AND THAT IS THE IDENTITY RATHER THAN A TASTE.** A composite added to a
    // lane must sound like the chain inside it; a 50% default would make every freshly added chain
    // quieter and phasier than the devices it holds, for no reason a hand asked for.
    {"chain_mix", "Chain Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry/wet across the whole chain — 100% is the chain alone, 0% bypasses it without stopping it"},
    {"chain_gain", "Chain Gain", 0.0, 1.0, kFaderUnity, false, device::Unit::Fader, {}, true, false,
     "Output fader for the chain — unity at twelve o'clock"},
    // THE MACROS. 0..1 with a default of 0, which is what `rack_clap.cpp` has always told a DAW.
    // MODULATABLE IS THE LOAD-BEARING BIT: these are the only controls an outer host may route to
    // (D6), and the way it may is that they are in this span and an inner device is not.
    {"macro_1", "Macro 1", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_2", "Macro 2", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_3", "Macro 3", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_4", "Macro 4", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_5", "Macro 5", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_6", "Macro 6", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_7", "Macro 7", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
    {"macro_8", "Macro 8", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the chain's own, wired to whatever inside it the patch says"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE MACRO BLOCK IS CONTIGUOUS AND ENDS THE ENUM — see `Param`. A control inserted among the
/// macros would make `Macro1 + n` address the wrong one, silently, for every host that walks them.
static_assert(static_cast<size_t>(Param::Macro1) + kMacroCount
              == static_cast<size_t>(Param::kCount));

/// AND EVERY MACRO IS MODULATABLE, checked rather than eyeballed. This is D6's escalation rule
/// stated as an assertion: if one of them stopped being a destination, an outer host would lose a
/// route it is entitled to and nothing else in the build would say so.
static_assert([] {
	for (size_t n = 0; n < kMacroCount; ++n) {
		if (!kParamDescs[static_cast<size_t>(Param::Macro1) + n].modulatable) {
			return false;
		}
	}
	return true;
}());

/// A COMPOSITE IS NOT AN AUDIO DEVICE YET, AND THE NULLS SAY SO. Every field spelled rather than
/// defaulted, because -Werror=missing-field-initializers makes each of them a decision — and here
/// the decision being recorded is that this descriptor is a surface with nothing behind it.
///
/// `.outputs = {}` is the one that will change first: a chain that holds a modulator publishes it,
/// which is the modulation port's *"a nested chain that publishes its outputs"*. It is empty
/// because a composite with no ops has nothing to publish FROM, not because a chain never will.
inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = nullptr,
	.mod = nullptr,
	.notes = nullptr,
};

} // namespace feathers::composite
