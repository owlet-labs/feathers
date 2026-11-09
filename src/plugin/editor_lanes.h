/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/synth.h"

// THE THREE LANES THAT ARE NOT PARAMETERS, AND THE ONE PLACE THEY ARE CARRIED.
//
// Almost everything the editor changes reaches the engine as a clap parameter, which has its own
// road: the host queues an event, process() or paramsFlush() drains it, and ParamSet holds it. These
// three do not.
//
//   * ROUTINGS — deliberately not parameters..
//   * THE MOD RACK's device settings — a modulator's controls are deliberately not parameters, so
//     this is the only road they have.
//   * THE FX LANE's device settings — the same, once the filter's controls stop being parameters.
//
// A control on one of these lanes reaches the engine HERE or it does not reach it at all.
//
// WHY THIS IS A FUNCTION RATHER THAN THREE LINES WRITTEN TWICE. It is called from process() and from
// paramsFlush(), which are the two entry points that can run — and it was written out at both, which
// is exactly how the rack's road came to exist at one and not the other. feathers_clap.cpp records
// what that cost: "setModRack was wired only to a patch load, so every knob on the envelope card
// wrote to the bridge and stopped there. The card drew the edit, the engine played the defaults."
// That showed up as three unrelated bugs — the times did nothing, the drawn curve disagreed with the
// sound, the loop never ran — and it was one missing line at one of two sites.
//
// Two call sites that must agree, written twice, is a coin flip on every future addition. Written
// once, a new lane joins both by construction.
//
// AND IT IS THE ONLY PART THE TWO SITES SHARE, which is worth stating because the obvious tidy-up is
// wrong. process() also hands over the tuning, the sound placements, the residues, the preview and
// the puck's own readings; paramsFlush does NOT, and should not. Those all ride the parameter road,
// which flush has already drained by the time it gets here, and re-publishing them from a flush would
// be answering a question nobody asked with a value nobody changed. Only these three have no other
// road.
//
// THREADING: both callers hold the same guarantee, and it is mutual exclusion rather than
// main-thread-ness. CLAP puts flush on the audio thread while the plugin is active and on the main
// thread while it is inactive, so neither is "the" flush thread — what holds in both cases is that
// flush and process never run at once. See the note above paramsFlush.

namespace feathers {

/// **THE ENGINE'S ROUTE STORES GROW WITH THE BRIDGE'S LIST** (backlog OJ). Called once, on the message thread,
/// where the three meet (the plugin's constructor; a fixture that routes past `kInitialRouteRoom`): the bridge's
/// list then grows the params' staged snapshot and inputs and the lane engine's stores BEFORE it publishes a count
/// that needs them, so the audio thread never sees a list it has no room for. All three must outlive the bridge's
/// next publish; the plugin owns them together.
inline void wireRouteGrowth(ParamBridge& bridge, ParamSet& params, Synth& synth) {
	bridge.setRouteGrowthListener([&params, &synth](size_t capacity) {
		params.reserveRoutes(capacity);
		synth.reserveRoutes(capacity);
	});
}

/// Carry the three non-parameter lanes from the editor's bridge into the engine. True when the CHAINS moved (a device
/// added, replaced or loaded): the caller settles the lane before the block's events then (backlog QU).
inline bool publishEditorLanes(const ParamBridge& bridge, ParamSet& params, Synth& synth) {
	// WHAT A ROUTING CHANGES IS WHAT A PARAMETER RESOLVES TO, so this lands on the parameters rather
	// than on the synth, and every consumer already reads it there.
	//
	// **AND IT IS SKIPPED ON EVERY BLOCK WHERE NO HAND MOVED A RING, which is almost all of them.**
	// This road WAS dense at both ends — `bridge.routes()` was 27,840 atomic loads returning a 111 KB
	// value, and `setRoutes` then walked it — and it ran unconditionally, once per audio block, on a
	// table that changes only when the editor is touched. §14.3 measured the pair at 0.37 % of a core.
	//
	// **THE GENERATION IS COMPARED BEFORE ANYTHING IS COPIED**, which is the whole point: copying and
	// then discovering nothing changed would pay the expensive half anyway. The design's
	// pointer swap (swap the routes on the audio thread when the running one is marked dirty) in the
	// form this side of the thread boundary can take.
	//
	// **AND THE COPY IS OF ROUTES BY IDENTITY, WHOLE OR NOT AT ALL.** The bridge publishes a `RouteList`; this
	// takes a snapshot of it (`adoptRoutes`), and one that straddled the editor's publish is thrown
	// away — the engine keeps the table it had and this block's comparison is made again next block.
	// The dense copy could not lose a route that way and the edge copy could, which is why it is a
	// snapshot and not a walk.
	//
	// THE CHAINS, TAKEN WHOLE FROM WHAT THE BRIDGE'S RECORD PUBLISHED — and only when that moved. This
	// read `bridge.modRack()` and `bridge.fxChain()` every block, two copies assembled from ~40 arrays of
	// relaxed atomics whose ordering was defended by comment; the record is the message thread's now,
	// and this is a snapshot of it (chain-as-a-device 5b, C4).
	//
	// **THE CHAINS FIRST, NOW, THEN THE ROUTES** (kill-lanes step 1): the bridge publishes its routes BY
	// IDENTITY and they are compiled into this engine's coordinates through the synth's own map of where
	// each device runs — so the map has to be the one this block will run. `adoptRoutes` compares both
	// numbers itself and does nothing on a block where neither moved.
	const bool chainsMoved = synth.adoptChains(bridge.publishedChains());
	// BY IDENTITY, WHOLE (kill-lanes §2.4b): the engine and the params build their nodes' inputs from this
	// snapshot at the synth's next drain — there is nothing to compile it against here.
	(void)params.adoptRoutes(bridge.routeList());
	// AND HOW EACH MACRO IS READ (backlog EG) — one word, stored every time because comparing it would
	// cost what storing it costs; `publishCoreSources` reads it at the block.
	params.setMacroBipolarMask(bridge.macroBipolarMask());
	// AND WHICH ARE RELATIVE (backlog PQ): those rest at 0 for the engine, their routes being edits of bases.
	params.setMacroRelativeMask(bridge.macroRelativeMask());
	return chainsMoved;
}

/// **THE OTHER DIRECTION: where modulation has left things, engine to editor — written once for the
/// reason above.** The plugin publishes this from THREE places (flush, the silent-port early return,
/// the end of a block); when the travel pulls joined the modulated rack they would have been a line
/// added at three sites that must agree, which is the coin flip this file exists to refuse. Written
/// here, and a test can stand a Synth beside a ParamBridge and ask whether the editor was told what
/// the engine played (test_synth, the played-place case) — which nothing can ask of a line inside
/// the plugin's process(), because nothing outside the plugin can see its bridge.
inline void publishEngineReadings(ParamBridge& bridge, const Synth& synth) {
	// Where modulation has left the rack — the editor draws its bars from the ENGINE's own arithmetic
	// rather than repeating it. See Synth::modulatedRack.
	bridge.publishModulated(synth.modulatedRack(), synth.modulatedFxChain(), synth.rackPlayheads(),
	                        synth.rackClock(), synth.fxDrivePeaks(), synth.fxPlayheads());
	// AND EVERY FX CARD'S COURTESY OUTPUT METER (backlog MK), beside the drive peaks.
	bridge.publishFxOutputPeaks(synth.fxOutputPeaks());
	// AND THE RIGHT EAR (backlog KU) — only the mask and the stereo-live values; a mono control publishes nothing new.
	// Bounded as `publishModulated` bounds its own loops: the chain's positions, the rack's slots.
	for (size_t position = 0; position < synth.modulatedFxChain().device.size(); ++position) {
		std::array<float, kMaxDeviceFields> right {};
		bridge.publishFxRightEar(position, synth.fxRightEar(position, right.data(), right.size()), right);
	}
	for (size_t slot = 0; slot < synth.modulatedRack().device.size(); ++slot) {
		std::array<float, kMaxDeviceFields> right {};
		bridge.publishRackRightEar(slot, synth.stripRightEar(slot, right.data(), right.size()), right);
	}
	// AND WHAT A DEVICE CHANGED OF ITS OWN STATE (backlog MX) — the step sequencer's Turing rewrites — into a snapshot
	// per strip slot and lane position; the main thread writes it into the record (`ParamBridge::harvestExtraState`).
	// A device that publishes nothing costs one null test here.
	{
		std::array<float, device::kMaxExtraStateFloats> extra {};
		for (size_t slot = 0; slot < synth.modulatedRack().device.size(); ++slot) {
			size_t count = 0;
			DeviceInstanceId id = kNoInstance;
			if (const uint64_t serial = synth.stripExtraState(slot, extra.data(), extra.size(), &count, &id); serial != 0) {
				bridge.publishRackExtraState(slot, id, serial, extra.data(), count);
			}
		}
		for (size_t position = 0; position < synth.modulatedFxChain().device.size(); ++position) {
			size_t count = 0;
			DeviceInstanceId id = kNoInstance;
			if (const uint64_t serial = synth.fxExtraState(position, extra.data(), extra.size(), &count, &id);
			    serial != 0) {
				bridge.publishFxExtraState(position, id, serial, extra.data(), count);
			}
		}
	}
	// And where it has left each sound's PLACE, so Explore draws the star where it is played.
	bridge.publishTravelPulls(synth.travelPulls());
	// AND THE ROUTE THE EDITOR IS SETTING AN AMOUNT ON, if any (backlog PU) — one probe, nothing when none is watched.
	synth.answerRouteProbe(bridge.routeProbe());
	// AND THE SPECTRUM DRAWER'S PLOT (D2), only while the editor's drawer is open: what the engine renders, published under
	// a new serial only when it moved. A closed drawer costs this one load and builds nothing.
	if (bridge.plotWanted()) {
		SpectralPlotFrame frame;
		synth.plotFrame(frame);
		(void)bridge.publishPlot(frame);
	}
}

} // namespace feathers
