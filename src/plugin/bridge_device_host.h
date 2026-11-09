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

#include "backplane/device_host.h"
#include "backplane/sources.h"
#include "fx/automod/automod_types.h"
#include "fx/busutil/busutil_types.h"
#include "fx/owltt/owltt_types.h"
#include "mod/modulator.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/expanded_views.h"
#include "rack/nesting.h"

#include <cmath>

// FEATHERS AS A `backplane::DeviceHost` — the adapter that makes the synth one implementor of the
// interface among others rather than the shape the interface was cut from.
//
// EVERYTHING HERE IS A FORWARD. There is no logic in this file and there must not be: the moment an
// adapter starts DECIDING something, the two hosts stop being two instantiations of one mechanism and
// become two mechanisms that agree for now. Every method below is one line onto a road `ParamBridge`
// already had.
//
// **ONE HOST FOR BOTH CHAINS, BECAUSE A DEVICE IS ADDRESSED BY ITS ID (kill-lanes step 2).** There were
// two: this one for the fx chain and `ModRackDeviceHost` for the modulator strip — a SECOND slot space,
// and handing a modulator card the fx host made it read and write an unrelated device, silently, because
// both spaces are rows of floats. It surfaced as six widget shards failing on values rather than as a
// type error. An id has one space: the host asks the bridge which chain holds the device, once, inside,
// and the per-chain mirrors it reads are this object's business and nobody's address.

namespace feathers {

/// **IS THE FOCUSED SOURCE BIPOLAR — AS DECLARED?** The one place that answers it for Feathers, so the
/// seatings that fill a `ModDisplay` cannot disagree.
///
/// **AN EMPTY CORE SPAN, WHICH `sources.h` EXPLICITLY BLESSES**: Feathers publishes no `SourceDesc` for
/// its core sources because they are macros and per-sound weights and every one is unipolar 0..1. So a
/// core source answers false — correct — and a device source is its device's own declaration. **IF A
/// CORE SOURCE EVER BECOMES BIPOLAR THIS GOES QUIETLY WRONG**; the fix then is to publish core descs.
inline bool sourceIsBipolar(const ParamBridge& bridge, backplane::Source source) noexcept {
	// **THE CORE SPAN IS NO LONGER EMPTY (backlog EG):** a macro switched bipolar declares itself so
	// through `ParamBridge::coreSourceDescs`, which is exactly the fix the note above named.
	return backplane::SourceView(bridge.coreSourceDescs(), bridge.devices()).isBipolar(source);
}

/// **AND WHAT IT IS PUBLISHING RIGHT NOW, which is the question a RING asks.**
///
/// `sourceIsBipolar` answers the DECLARATION — the widest signal an output can publish — and routing is
/// sized by that on purpose. A ring is drawn from this one instead: a DAHDSR whose `Env Bipolar` switch
/// is off publishes 0..1, so a route from it reaches one way, and an arc drawn symmetric would be claiming
/// a reach the route does not have.
///
/// **THE SWITCH IS NAMED BY THE DEVICE, NOT KNOWN BY THIS FUNCTION** — `SourceDesc::bipolarControl`. A
/// version that tested the device's tag for "DHDS" would be the compile-time slot branch this whole
/// registry exists to have retired.
///
/// FALSE FOR A SOURCE THAT DOES NOT EXIST: the unipolar rest is at the bottom, and drawing a one-sided
/// reach for something we cannot read beats drawing a symmetric one we cannot justify.
inline bool sourceIsBipolarNow(const ParamBridge& bridge, backplane::Source source) noexcept {
	// A MACRO'S POLARITY IS ITS MODE, NOT A CONTROL ON A DEVICE (backlog EG): its desc's flag IS the
	// live answer, and `bipolarControl` stays fixed on it, so the branch below returns it as declared.
	const device::SourceDesc* desc = backplane::SourceView(bridge.coreSourceDescs(), bridge.devices()).descOf(source);
	if (desc == nullptr || !desc->bipolar) {
		// NOT DECLARED BIPOLAR MEANS IT CANNOT BECOME SO — a switch may narrow an output, never widen
		// it past what routing was sized for.
		return false;
	}
	if (desc->bipolarControl == device::kPolarityFixed) {
		// NOTHING MOVES IT: declared bipolar and permanently so — the tracker's Pitch.
		return true;
	}
	// THE DEVICE'S OWN SWITCH, READ IN WHICHEVER CHAIN HOLDS IT — a modulator lives in the strip today,
	// but nothing in the addressing says a source must, so both are asked; each answers out of range for
	// a device that is not its own.
	const auto control = static_cast<size_t>(desc->bipolarControl);
	// A SWITCH READS ≥ 0.5; A CHOICE READS ITS ONE BIPOLAR ENTRY (`SourceDesc::bipolarChoice`).
	const auto bipolarAt = [&](float v) {
		return desc->bipolarChoice >= 0 ? std::lround(v) == desc->bipolarChoice : v >= 0.5f;
	};
	if (const size_t rackSlot = bridge.rackSlotOf(source.device); rackSlot != kNoPosition) {
		return bipolarAt(bridge.rackFieldAt(rackSlot, control));
	}
	if (const size_t fxSlot = bridge.fxSlotOf(source.device); fxSlot != kNoPosition) {
		return bipolarAt(bridge.fxFieldAt(fxSlot, control));
	}
	return false;
}

class BridgeDeviceHost final : public backplane::DeviceHost {
public:
	explicit BridgeDeviceHost(ParamBridge& bridge) : bridge_(bridge) {}

	/// THE BASE'S DELETED `size_t` OVERLOADS, VISIBLE HERE — an override hides them otherwise, and a caller
	/// holding this type would pass a slot as an id and compile (`device_host.h`).
	using backplane::DeviceHost::fieldAt;
	using backplane::DeviceHost::setField;
	using backplane::DeviceHost::fieldsPresentAt;
	using backplane::DeviceHost::modulatedFieldAt;
	using backplane::DeviceHost::readingCountAt;
	using backplane::DeviceHost::readingAt;
	using backplane::DeviceHost::drivePeakAt;
	using backplane::DeviceHost::outputPeakAt;
	using backplane::DeviceHost::playheadAt;

	const backplane::Axes& axes() const noexcept override { return feathersAxes(); }

	/// THE RECORD'S VIEW, BY ID — what each device is and where it stands, for the routing authority.
	backplane::DeviceView devices() const noexcept override { return bridge_.devices(); }

	/// **ONE LOAD, NOT A CHAIN COPY (BL).** This read `const FxChain chain = bridge_.fxChain();` — a
	/// whole ~4 KB chain copied PER FIELD, so one card's idle update copied the lane once per control it
	/// draws. inside measured it at 5.85 us for one card and 14-40 us for eight. The bridge answers a
	/// field directly, from whichever chain holds the device.
	float fieldAt(DeviceInstanceId device, size_t control) const override {
		if (const size_t rack = bridge_.rackSlotOf(device); rack != kNoPosition) {
			return bridge_.rackFieldAt(rack, control);
		}
		return bridge_.fxFieldAt(bridge_.fxSlotOf(device), control);
	}

	/// **WHERE MODULATION LEFT IT (BK)** — what the ENGINE arrived at, published once a block. Without
	/// it `DeviceHost`'s default answers `fieldAt`, the base, and a card's ring sits still however much
	/// is routed at it — as reported on 2026-09-03.
	float modulatedFieldAt(DeviceInstanceId device, size_t control) const override {
		if (const size_t rack = bridge_.rackSlotOf(device); rack != kNoPosition) {
			return bridge_.rackModulatedFieldAt(rack, control);
		}
		return bridge_.fxModulatedFieldAt(bridge_.fxSlotOf(device), control);
	}

	/// AND THE RIGHT EAR (backlog KU), by the same dispatch.
	bool modulatedFieldRAt(DeviceInstanceId device, size_t control, float& right) const override {
		if (const size_t rack = bridge_.rackSlotOf(device); rack != kNoPosition) {
			return bridge_.rackModulatedFieldRAt(rack, control, right);
		}
		return bridge_.fxModulatedFieldRAt(bridge_.fxSlotOf(device), control, right);
	}

	/// **A ROW INDEX PAST THE DECLARED PARAMS IS THE DEVICE'S STATE BLOB, AND IT TAKES THE OTHER ROAD.**
	/// `setDeviceControl` refuses such an index on purpose — it coerces against a `ParamDesc` and a blob
	/// float has none — so this dispatches rather than widening that guard. The two roads stay honest:
	/// one validated, one bounded by `extraState` and unvalidated by design.
	///
	/// **ONLY A DEVICE THAT DECLARES A BLOB CAN BE WRITTEN PAST ITS PARAMS**, and asking that first keeps
	/// this dispatch invisible to every device that does not. Belt-and-braces rather than a fix for a
	/// known fault: a version without the `extraState` clause is behaviourally identical for a device
	/// with no blob. (It once appeared to fix an intermittent windowed case, which then failed again with
	/// it in — flaky, not caused by this line. Narrower is right on its own merits.)
	void setField(DeviceInstanceId device, size_t control, float value) override {
		if (const device::Descriptor* desc = bridge_.descriptorOf(device);
		    desc != nullptr && desc->traits.extraState > 0 && control >= desc->params.size()) {
			bridge_.setDeviceState(device, control - desc->params.size(), value);
			return;
		}
		bridge_.setDeviceControl(device, control, value);
	}
	void releaseField(DeviceInstanceId device, size_t control, float value) override {
		if (const device::Descriptor* desc = bridge_.descriptorOf(device);
		    desc != nullptr && desc->traits.extraState > 0 && control >= desc->params.size()) {
			setField(device, control, value); // extra state: no engine reads a rise of it
			return;
		}
		bridge_.releaseDeviceControl(device, control, value);
	}
	std::vector<backplane::Route> takePrunedRoutes() override { return bridge_.fragmentRecord().takePrunedRoutes(); }
	/// THE HOST SLOTS' GESTURE ROAD (T6b) — the bridge owns the pool and the outbox.
	void beginControlGesture(DeviceInstanceId device, size_t control) override {
		bridge_.beginControlGesture(device, control);
	}
	void endControlGesture(DeviceInstanceId device, size_t control) override {
		bridge_.endControlGesture(device, control);
	}
	/// THE RECENT RING (T9): a hand took the control.
	void touchControl(DeviceInstanceId device, size_t control) override { bridge_.touchControl(device, control); }
	/// THE HOST SLOTS' MENU ROAD (T7) — the bridge owns the pool, one line each.
	device::HostSlotStatus hostSlotOf(DeviceInstanceId device, size_t control) const override {
		return bridge_.hostSlotStatusOf(device, control);
	}
	bool exposeToHost(DeviceInstanceId device, size_t control, std::string* displaced) override {
		return bridge_.exposeToHost(device, control, displaced);
	}
	bool unpinFromHost(DeviceInstanceId device, size_t control) override {
		return bridge_.unpinFromHost(device, control);
	}
	uint64_t hostSlotGeneration() const noexcept override { return bridge_.hostSlotGeneration(); }

	size_t fieldsPresentAt(DeviceInstanceId device) const override {
		if (const size_t rack = bridge_.rackSlotOf(device); rack != kNoPosition) {
			return bridge_.rackFieldsPresentAt(rack);
		}
		return bridge_.fxFieldsPresentAt(bridge_.fxSlotOf(device));
	}

	/// THE CHAIN'S BODY, from the bridge's own carrier — the same bytes the synth is handed at provisioning.
	std::span<const uint8_t> containerBodyOf(DeviceInstanceId device) const override {
		return bridge_.fxBodies().bodyFor(device);
	}
	uint64_t containerBodyGenerationOf(DeviceInstanceId device) const override {
		return bridge_.fxBodies().generationOf(device);
	}
	void* containerSurfaceOf(DeviceInstanceId device) const override { return bridge_.fxBodies().surfaceOf(device); }
	/// A HAND'S BLOCKS (`DeviceHost::setBlock`): in the lane's carrier, and an edit of one dirties the patch.
	::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) override { return &bridge_.fxBodies(); }
	const ::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) const override { return &bridge_.fxBodies(); }
	void onBlockEdited(DeviceInstanceId) override { bridge_.markStateDirty(); }
	/// A CHAIN ACCESS SITE (backlog NR), through the bridge's own carrier — `ParamBridge::openFxSite`.
	bool openSite(DeviceInstanceId device) override { return bridge_.openFxSite(device); }
	bool hasSite(DeviceInstanceId device) const override { return bridge_.fxBodies().has(siteKeyOf(device)); }
	bool loadSite(DeviceInstanceId device, std::span<const uint8_t> body) override {
		return holdsBody(body) && bridge_.loadFxSite(device, body);
	}
	/// THE LANE IS A PLUGIN'S OWN CHAIN (depth 0), so only a body taller than any file reopens is refused here.
	bool holdsBody(std::span<const uint8_t> body) const override {
		return rack::containerBodyFits(bridge_.fragmentRecord().depth(), body, bridge_.fragmentRecord().catalogue());
	}
	backplane::Source siteExport(DeviceInstanceId owner, backplane::Source inner) override {
		return bridge_.fxSiteExport(owner, inner);
	}
	backplane::Source containerExport(DeviceInstanceId container, backplane::Source inner) override {
		return bridge_.fxContainerExport(container, inner);
	}
	/// Feathers' fx lane runs a Poly device inside its voices.
	bool hostsVoices() const override { return true; }
	/// Held against the lane's reap (backlog JX) — the carrier the converger reads its pins from.
	void pinContainerSurface(const void* surface) override { bridge_.fxBodies().pin(surface); }
	void unpinContainerSurface(const void* surface) override { bridge_.fxBodies().unpin(surface); }

	/// THE ARMED SOURCE'S ROUTE LIST (rack roadmap §7f), through the bridge: its own routes, its names,
	/// and the routes inside its chains.
	void forEachRouteFrom(backplane::Source source, void* ctx,
	                      void (*f)(void*, backplane::Destination)) const override {
		bridge_.forEachRoute([&](const backplane::Route& r) {
			if (r.source == source) {
				f(ctx, r.destination);
			}
		});
	}
	void forEachRouteInto(backplane::Destination destination, void* ctx,
	                      void (*f)(void*, backplane::Source)) const override {
		bridge_.forEachRoute([&](const backplane::Route& r) {
			if (r.destination == destination) {
				f(ctx, r.source);
			}
		});
	}
	std::string destinationName(backplane::Destination destination) const override {
		return bridge_.destinationName(destination);
	}
	std::vector<InnerRoute> innerRoutesFrom(backplane::Source source) const override {
		return bridge_.innerRoutesFrom(source);
	}
	float innerRouteAmount(backplane::Source source, DeviceInstanceId chain,
	                       backplane::Destination inner) const override {
		return bridge_.innerRouteAmount(source, chain, inner);
	}
	bool setInnerRouteAmount(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                         float amount) override {
		return bridge_.setInnerRouteAmount(source, chain, inner, amount);
	}
	backplane::Transfer innerRouteTransfer(backplane::Source source, DeviceInstanceId chain,
	                                       backplane::Destination inner) const override {
		return bridge_.innerRouteTransfer(source, chain, inner);
	}
	bool setInnerRouteTransfer(backplane::Source source, DeviceInstanceId chain, backplane::Destination inner,
	                           backplane::Transfer t) override {
		return bridge_.setInnerRouteTransfer(source, chain, inner, t);
	}
	std::string innerDestinationName(DeviceInstanceId chain, backplane::Destination inner) const override {
		return bridge_.innerDestinationName(chain, inner);
	}

	/// STRAIGHT THROUGH, like everything else here — the bridge already knows the rate its host gave it.
	double sampleRate() const noexcept override { return bridge_.sampleRate(); }

	/// THE TEMPO, FROM THE SAME PLACE AND FOR THE SAME REASON AS THE RATE — see
	/// `DeviceHost::secondsPerBeat`. Zero when the host has not said; the bridge refuses a non-finite
	/// or negative one at the publish, so a card can divide by whatever is non-zero here.
	double secondsPerBeat() const noexcept override { return bridge_.rackSecondsPerBeat(); }

	/// OWLTT'S METERS, FLATTENED. TEN, NOT NINE: the clipping flag rides as a float because the seam
	/// carries floats. **THE ADAPTER KNOWS THE LAYOUT AND THE SEAM DOES NOT** — this is the one place in
	/// Feathers that maps OWLTT's struct onto indices; `OwlttGraph` is the one place that maps them back.
	/// `fx::owltt::Owltt::kReadingCount`, restated: ten meters, then a light per band and the combined
	/// gain reduction (backlog EL, 2026-09-16). test_gui_widgets_devices pins the two agree.
	static constexpr size_t kOwlttReadings = 18; // + each band's second-channel GR, backlog LQ; + Key Track's note

	/// AUTOMOD PUBLISHES TWO READINGS — [0] its LFO phase, [1] its envelope follower's level.
	///
	/// **THERE ARE TWO ROADS TO THIS SURFACE AND TEACHING ONE IS NOT TEACHING BOTH.** When the envelope
	/// was added, the bridge's WRITE side learned it and this READ side did not — this constant still said
	/// 1, and the trace drew at rest in the plugin while working in the rack. NOTHING IN `ctest` SAW IT;
	/// it was caught by PHOTOGRAPHING THE CARD.
	static constexpr size_t kAutomodReadings = 2;

	/// **AN FX DEVICE'S READINGS; NOTHING IN THE STRIP PUBLISHES ANY**, and answering zero for a strip
	/// device is what stops a card reading an fx device's meters through the wrong chain.
	size_t readingCountAt(DeviceInstanceId device) const noexcept override {
		const size_t slot = bridge_.fxSlotOf(device);
		const FxChain chain = bridge_.fxChain();
		if (slot >= chain.count || slot >= chain.device.size()) {
			return 0;
		}
		const char* tag = fxDeviceAtPosition(chain, slot).tag().data();
		if (device::sameTag(tag, fx::owltt::kTraits.tag)) {
			return kOwlttReadings;
		}
		if (device::sameTag(tag, fx::automod::kTraits.tag)) {
			return kAutomodReadings;
		}
		// **THE GENERIC CHANNEL, ASKED LAST AND NOT BY TAG.** Bus Utility publishes seven readings through
		// `ParamBridge::publishDeviceReadings`, and the count comes back from what was actually published.
		return bridge_.deviceReadingCount(slot);
	}

	float readingAt(DeviceInstanceId device, size_t index) const noexcept override {
		// **THE COUNT IS ASKED OF THE SAME DEVICE, NOT OF A NUMBER DERIVED FROM IT.** This asked the count
		// with a position where it took a lane — equal until the first move, and then OWLTT's bars froze
		// and automod's star died the moment the card was dragged (DG). An id cannot be
		// converted twice.
		if (index >= readingCountAt(device)) {
			return 0.0f;
		}
		const size_t slot = bridge_.fxSlotOf(device);
		const FxChain chain = bridge_.fxChain();
		const char* tag = fxDeviceAtPosition(chain, slot).tag().data();
		if (device::sameTag(tag, fx::automod::kTraits.tag)) {
			// BY INDEX, NOT BY DEVICE — a second reading must not answer the first one's value.
			return (index == 0) ? bridge_.automodPhase(slot) : bridge_.automodEnvelope(slot);
		}
		// **OWLTT BY TAG, THE GENERIC CHANNEL BY DEFAULT — the same shape `readingCountAt` uses, and
		// they disagreed.** The count fell through to `deviceReadingCount` for any device that was not
		// OWLTT or automod, while the VALUE fell through to OWLTT'S METERS and only Bus Utility was
		// named onto the generic road. So a third device publishing readings got a truthful count and
		// somebody else's numbers — SKLITTER's band read gain reduction and clipping. Asking the two
		// questions the same way is what stops that; a device is named here only when its readings are
		// NOT on the generic channel.
		if (device::sameTag(tag, fx::owltt::kTraits.tag)) {
			const ParamBridge::OwlttMeterReading m = bridge_.owlttMeters(slot);
			if (index < 3) {
				return m.grDB[index];
			}
			if (index < 6) {
				return m.inputLevelDB[index - 3];
			}
			if (index < 9) {
				return m.outputLevelDB[index - 6];
			}
			if (index == 9) {
				return m.clipping ? 1.0f : 0.0f;
			}
			// BACKLOG EL: a light per band (10..12) and the combined gain reduction in dB (13) — the
			// same order the rack's publisher and the generic readings sink flatten them in.
			if (index < 13) {
				return m.bandSaturating[index - 10] ? 1.0f : 0.0f;
			}
			if (index == 13) {
				return m.combinedGrDb;
			}
			// BACKLOG LQ: each band's second channel's gain reduction (14..16), readings 0..2 being the first's.
			if (index < 17) {
				return m.grSecond[index - 14];
			}
			return index == 17 ? m.noteHz : 0.0f; // KEY TRACK's note (17)
		}
		return bridge_.deviceReading(slot, index);
	}

	/// **THE STRIP'S PLAYHEAD; AN FX DEVICE DECLINES.** A playhead is the MODULATOR tier's shape — an fx
	/// device's live values go through `readingAt` (automod's phase is reading 0 there). The fx host once
	/// answered `rackPlayhead(slot)` with an FX slot, and the low slots COLLIDED while the high ones
	/// declined: partial, plausible, silent. An id cannot be read in the wrong chain.
	/// **AND A LANE MODULATOR'S, BY ITS OWN CHANNEL** (2026-10-04): the lane holds modulators now, and its
	/// position is asked of the lane's playheads — never of the strip's, which is the collision named above.
	bool playheadAt(DeviceInstanceId device, uint32_t& ordinal, float& phase) const override {
		mod::Playhead where;
		if (!publishedPlayhead(device, where)) {
			return false;
		}
		ordinal = where.ordinal;
		phase = where.phase;
		return true;
	}

	/// THE LEVEL BESIDE IT, from the same channel (`mod::Playhead::level`).
	bool playheadLevelAt(DeviceInstanceId device, float& level) const override {
		mod::Playhead where;
		if (!publishedPlayhead(device, where) || !where.haveLevel) {
			return false;
		}
		level = where.level;
		return true;
	}

	/// THE TRIG'S FIRE COUNT, from the same channel (`mod::Playhead::trigFires`, backlog MV).
	bool trigFiresAt(DeviceInstanceId device, uint32_t& fires) const override {
		mod::Playhead where;
		if (!publishedPlayhead(device, where) || !where.haveTrigFires) {
			return false;
		}
		fires = where.trigFires;
		return true;
	}

	/// THE NEWEST EVENTS, from the same channel (`mod::Playhead::recent`).
	size_t recentEventsAt(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const override {
		mod::Playhead where;
		if (out == nullptr || !publishedPlayhead(device, where)) {
			return 0;
		}
		size_t n = 0;
		while (n < capacity && n < where.recent.size() && where.recent[n].ordinal != 0) {
			out[n] = where.recent[n];
			++n;
		}
		return n;
	}

	/// **THE DRIVE METER, ANSWERED FROM THE BRIDGE'S CHANNEL.** It declines for a device nobody publishes
	/// for, which is every device but the shaper today — the sentinel is negative precisely so
	/// "unmetered" and "driven by silence" are different answers.
	bool drivePeakAt(DeviceInstanceId device, float& peak) const override {
		return bridge_.fxDrivePeak(bridge_.fxSlotOf(device), peak);
	}
	/// **THE COURTESY OUTPUT METER** (backlog MK), from the bridge's channel — a lane device only; the strip meters none.
	bool outputPeakAt(DeviceInstanceId device, float& peak) const override {
		return bridge_.fxOutputPeak(bridge_.fxSlotOf(device), peak);
	}

	float routeAmount(backplane::Source source, backplane::Destination destination) const override {
		return bridge_.routeAmount(source, destination);
	}

	void setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) override {
		bridge_.setRouteAmount(source, destination, amount);
	}

	/// THE AS-MODULATED READOUT'S ANSWER (backlog PU) — the lane engine's, through the bridge's channel.
	bool probeRoute(backplane::Source source, backplane::Destination destination, backplane::RouteProbe& out) override {
		bridge_.routeProbe().watch(source, destination);
		return bridge_.routeProbe().read(source, destination, out);
	}
	void unwatchRoute() override { bridge_.routeProbe().unwatch(); }
	/// THE BRIDGE IS THE TABLE — the identity `ParamControl` names too (backlog PU).
	const void* routeIdentity() const noexcept override { return &bridge_; }

	bool supportsRouteTransfers() const override { return true; }
	backplane::Transfer routeTransfer(backplane::Source source, backplane::Destination destination) const override {
		return bridge_.routeTransfer(source, destination);
	}
	bool setRouteTransfer(backplane::Source source, backplane::Destination destination, backplane::Transfer t) override {
		return bridge_.setRouteTransfer(source, destination, t);
	}
	/// THE CONTRACT'S POLARITY RULE, AND FEATHERS' OWN ON TOP — the stack index is Lin only.
	bool transferChoosable(backplane::Source source, backplane::Transfer t) const override {
		return bridge_.transferAllowedFor(source, t) && DeviceHost::transferChoosable(source, t);
	}

	bool routedElsewhere(backplane::Destination destination, backplane::Source except) const override {
		return bridge_.routedElsewhere(destination, except);
	}

	/// **WHAT THE SOURCE IS PUBLISHING THIS BLOCK, not the widest it could.** Feathers can read the
	/// switch, so it does. See `sourceIsBipolarNow`.
	bool sourceIsBipolarNow(backplane::Source source) const override {
		return feathers::sourceIsBipolarNow(bridge_, source);
	}

	void beginRouteGesture() override { bridge_.beginRouteGesture(); }
	void endRouteGesture() override { bridge_.endRouteGesture(); }

	/// FEATHERS' REGISTRY OF THE PANELS THIS BUILD CAN CONSTRUCT, answered from here so that no card
	/// has to include it. One line onto a table the plugin already owned — the adapter's rule.
	bool hasExpandedViewFor(const device::Descriptor& desc) const override {
		return ui::hasExpandedView(desc);
	}

	/// THE BRIDGE'S OUTPUT LABELS (`ParamBridge::outputLabel`), one line each — the adapter's rule.
	std::string outputLabel(DeviceInstanceId device, size_t output) const override {
		return bridge_.outputLabel(device, output);
	}
	bool outputRelabelled(DeviceInstanceId device, size_t output) const noexcept override {
		return bridge_.outputRelabelled(device, output);
	}
	void setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label) override {
		bridge_.setOutputLabel(device, output, label);
	}
	uint64_t outputsShown(DeviceInstanceId device) const noexcept override { return bridge_.outputsShown(device); }
	void setOutputsShown(DeviceInstanceId device, uint64_t mask) override { bridge_.setOutputsShown(device, mask); }
	device::Subscription subscribeOutputLabel(std::function<void(DeviceInstanceId, size_t)> listener) override {
		return bridge_.subscribeOutputLabel(std::move(listener));
	}
	/// WHICH PRESET A DEVICE'S PANEL CAME FROM (backlog QC) — the bridge's label map holds it.
	PresetIdentity devicePreset(DeviceInstanceId device) const override { return bridge_.devicePreset(device); }
	void setDevicePreset(DeviceInstanceId device, const PresetIdentity& identity) override {
		bridge_.setDevicePreset(device, identity);
	}

	/// FEATHERS' OWN RULE ABOUT ITS OWN AXES — a weight source may not reach a puck destination. The
	/// device half is the backplane's and is not repeated here, which is the entire content of the
	/// split `routing.h` describes.
	backplane::CorePolicy corePolicy() const noexcept override {
		return &detail::maySourceRouteToIgnoringDevice;
	}

	ParamBridge& bridge() noexcept { return bridge_; }
	const ParamBridge& bridge() const noexcept { return bridge_; }

private:
	/// WHERE THE DEVICE'S PLAYHEAD IS PUBLISHED — the strip's channel, or the lane's for a lane modulator; false
	/// for anything that publishes none. See `playheadAt`.
	bool publishedPlayhead(DeviceInstanceId device, mod::Playhead& where) const {
		const size_t rack = bridge_.rackSlotOf(device);
		if (rack == kNoPosition) {
			const size_t position = bridge_.fxSlotOf(device);
			const device::Descriptor* desc = bridge_.descriptorOf(device);
			if (position == kNoPosition || desc == nullptr || desc->mod == nullptr) {
				return false;
			}
			where = bridge_.fxPlayhead(position);
			return true;
		}
		where = bridge_.rackPlayhead(rack);
		return true;
	}

	ParamBridge& bridge_;
};

} // namespace feathers
