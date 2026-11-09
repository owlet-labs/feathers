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

#include "backplane/axes.h"
#include "backplane/container_bodies.h"
#include "backplane/preset_identity.h"
#include "backplane/route_probe.h"
#include "backplane/routing.h"
// FOR `SourceView`, WHICH `sourceIsBipolarNow`'s DEFAULT IS WRITTEN IN TERMS OF — the default has to
// answer the declaration, and this is the one place that reads it.
#include "backplane/sources.h"
#include "device/extra_state.h"
#include "device/modulatable.h" // `HostSlotStatus`, which the host-slot queries answer in
#include "device/subscribers.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// WHAT A CARD NEEDS OF WHATEVER IS HOSTING ITS DEVICE — the seam the whole extraction turns on.
//
// A device's card reads and writes its controls, and draws the modulation reaching them. Today it
// does both through `ParamBridge`, which is Feathers' own road to its own audio thread; that one
// dependency is the last thing keeping the card layer inside the synth. This is that surface with the
// synth taken out: SIX operations, derived by enumerating what `FxControl` actually calls rather than
// by imagining what a host might offer.
//
//   fxChain()              -> fieldAt / fieldsPresentAt
//   setFxDeviceControl()   -> setField
//   fxRouteLaneAt()        -> (retired: a device address IS a lane in every host — BX step 4)
//   routes()               -> routeAmount / setRouteAmount
//   begin/endRouteGesture() -> the same, unchanged
//
// SO IT IS FIVE OPERATIONS NOW RATHER THAN SIX, and the one that went was the one that existed only
// because two hosts disagreed about what an address was.
//
// THE VALUE ROAD AND THE ROUTING ROAD ARE BOTH HERE AND THAT IS DELIBERATE. An earlier sketch split
// them, on the grounds that a host with no modulation could implement half. It cannot usefully: a
// control that can be edited but never shows what is reaching it is the honesty rule's failure case,
// and a host that genuinely has no routing implements the routing half by answering "nothing is
// routed", which costs five lines and keeps every card correct.
//
// WHY AN ABSTRACT CLASS RATHER THAN A TEMPLATE. The card layer holds these by reference and never
// copies them; there is exactly one per host, constructed once. A template would put the host's type
// into every card's type, which is the template infection `axes.h` already refused for the route
// table — and here it would be worse, because a card is a `visage::Frame` in a heterogeneous tree.

namespace feathers::backplane {

/// WHATEVER IS HOSTING A DEVICE — its values, its routes, and the shape of its modulation space.
class DeviceHost {
public:
	virtual ~DeviceHost() = default;

	/// The shape of this host's modulation space. Constant for the host's lifetime.
	virtual const Axes& axes() const noexcept = 0;

	/// **WHAT THIS HOST HOLDS, BY DEVICE** — what each device is and where it stands in the flow, asked
	/// by id (kill-lanes step 2: addresses are not sequential). It replaced a lane-indexed view: a card
	/// holds its DEVICE now, never a slot, and only the host knows where it keeps one. BY VALUE — function
	/// pointers over the host — so a card may ask per paint.
	virtual DeviceView devices() const noexcept = 0;

	/// What the device `id` is, or null for one this host does not hold — `devices()`, spelled for the
	/// commonest question.
	const device::Descriptor* descriptorOf(DeviceInstanceId id) const {
		const DeviceView v = devices();
		return v.descriptorOf != nullptr ? v.descriptorOf(v.host, id) : nullptr;
	}

	//--------------------------------------------------------------------------------------------//
	// THE VALUE ROAD — BY DEVICE                                                                   //
	//--------------------------------------------------------------------------------------------//

	/// **A LANE OR A SLOT IS NOT AN ID, AND A `size_t` WOULD NARROW INTO ONE WITHOUT A WORD** —
	/// `DeviceInstanceId` is a `uint32_t`, so a card still passing its old `size_t` address would compile
	/// and address whichever device wears that number as an id. These are the reads and writes a card
	/// makes through this seam, deleted for `size_t`, so such a site is an error someone reads.
	///
	/// **AN OVERRIDE HIDES THEM**, so a caller holding the derived type would narrow after all: every
	/// implementor brings them back in with `using DeviceHost::fieldAt;` and the rest, beside its overrides.
	float fieldAt(size_t, size_t) const = delete;
	void setField(size_t, size_t, float) = delete;
	size_t fieldsPresentAt(size_t) const = delete;
	float modulatedFieldAt(size_t, size_t) const = delete;
	size_t readingCountAt(size_t) const noexcept = delete;
	float readingAt(size_t, size_t) const noexcept = delete;
	bool drivePeakAt(size_t, float&) const = delete;
	bool outputPeakAt(size_t, float&) const = delete;
	bool playheadAt(size_t, uint32_t&, float&) const = delete;
	const device::Descriptor* descriptorOf(size_t) const = delete;
	std::span<const uint8_t> containerBodyOf(size_t) const = delete;
	uint64_t containerBodyGenerationOf(size_t) const = delete;
	void* containerSurfaceOf(size_t) const = delete;

	/// One control of one device, in the DEVICE'S OWN declared units. There is no normalised domain
	/// anywhere in this project and a second one here would be a second place for a range to be wrong.
	virtual float fieldAt(DeviceInstanceId device, size_t control) const = 0;

	/// Write one. The host coerces against the control's own declaration, so a widget may hand over
	/// whatever its drag produced without knowing the range.
	virtual void setField(DeviceInstanceId device, size_t control, float value) = 0;

	/// **A MOMENTARY'S RELEASE** (2026-11-07: Pattern's Go pressed and released between two blocks was LOST). Writes as
	/// `setField` does, except that a fall whose rise no block has read yet WAITS for one (`rack::HeldFalls`): the press is
	/// always seen. Only a hand's momentary takes this road — a preset, an undo or a load writes through `setField`. The
	/// default writes at once, for a host with no engine behind it.
	virtual void releaseField(DeviceInstanceId device, size_t control, float value) { setField(device, control, value); }

	/// HOW MANY FIELDS THAT DEVICE'S ROW ACTUALLY CARRIES.
	///
	/// IT IS NOT THE DEVICE'S CONTROL COUNT AND MUST NOT BE ASSUMED TO BE. `device_row.h`'s rule is
	/// that the width travels WITH the payload, so a reader can tell "this device had fewer fields"
	/// from "these fields were zero" — a patch written by a build that declared fewer controls loads
	/// with the rest ABSENT, and absent means the control's declared default rather than zero. A
	/// host that answered the descriptor's size here would silently turn every absent field into a
	/// zero, which is the exact corruption that rule exists to prevent.
	virtual size_t fieldsPresentAt(DeviceInstanceId device) const = 0;
	/// **THE ROUTES THE LAST `setField` DELETED, TAKEN** (backlog OP) — a field that changes what its device may reach
	/// (the Follower's Mode) deletes the routes the new answer forbids, as a reorder does; the verb that wrote it names
	/// them and keeps them for its undo. Empty for a host whose record prunes nothing.
	[[nodiscard]] virtual std::vector<Route> takePrunedRoutes() { return {}; }

	//--------------------------------------------------------------------------------------------//
	// WHAT A CONTAINER HOLDS — its body, as the host carries it                                    //
	//--------------------------------------------------------------------------------------------//

	/// **A COMPOSITE DEVICE'S BODY, AS THE HOST CARRIES IT** — the bytes `ContainerOps::loadState` was (or
	/// will be) handed for this device: the inner rack's own file. A card draws what a chain HOLDS from
	/// this (D6: the summary is the head) without reaching into the running instance, which lives on the
	/// audio thread. Empty for a device that is no container, or one this host holds no body for.
	virtual std::span<const uint8_t> containerBodyOf(DeviceInstanceId) const { return {}; }

	/// WHICH VERSION OF THAT BODY — moves when the host stores a new one, so a reader re-parses only then.
	/// 0 for a device with no body.
	virtual uint64_t containerBodyGenerationOf(DeviceInstanceId) const { return 0; }

	/// **THE RUNNING INSTANCE OF A COMPOSITE DEVICE, AS AN OPAQUE HANDLE** — what the host's carrier filed
	/// for it at provisioning (`ContainerBodies::attach`), or null while none runs. The module that made the
	/// device is the one that knows what the pointer is (the rack's expanded view: `rack::bridgeOf`); this
	/// seam only carries it. A reader re-asks every tick and treats a changed answer as a different instance.
	virtual void* containerSurfaceOf(DeviceInstanceId) const { return nullptr; }

	//--------------------------------------------------------------------------------------------//
	// A HAND'S BLOCKS (`device::StateBlockDesc::handEdited`) — state the USER writes, too large for the row //
	//--------------------------------------------------------------------------------------------//

	/// THE STORE THIS HOST KEEPS A DEVICE'S BLOCKS IN (`::feathers::ContainerBodies`), or null for a host with none — which then
	/// refuses every block write. Message thread.
	virtual ::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) { return nullptr; }
	virtual const ::feathers::ContainerBodies* blockStoreOf(DeviceInstanceId) const { return nullptr; }
	/// A block of this device was written by a hand: the host is dirty (what `setField` tells it for a control). Default none.
	virtual void onBlockEdited(DeviceInstanceId) {}

	/// **WRITE A HAND'S BLOCK, WHOLE** (MESSAGE THREAD, the one writer): into the running instance (`StateOps::apply`, which the
	/// audio thread then pulls), into the host's store (so a rebuild of the instance, a save with no instance and a load all hold
	/// it) and the host dirty. One call is one atomic edit — the audio thread sees all of the old value or all of the new —
	/// and an undo record is just another call with the old floats (`ui::CurveEdit`). False for a device with no such block, a
	/// block that is not a hand's, or the wrong length; nothing is written then.
	bool setBlock(DeviceInstanceId device, size_t index, std::span<const float> floats) {
		const device::Descriptor* d = descriptorOf(device);
		::feathers::ContainerBodies* store = blockStoreOf(device);
		if (d == nullptr || d->state == nullptr || store == nullptr || index >= d->state->blocks.size()
		    || !d->state->blocks[index].handEdited || d->state->apply == nullptr || floats.size() != d->state->blocks[index].floats) {
			return false;
		}
		if (void* surface = store->surfaceOf(device); surface != nullptr && !d->state->apply(surface, index, floats.data(), floats.size())) {
			return false;
		}
		// THE STORE'S COPY, under the serial the instance now holds (or a fresh one when none runs), so a harvest of the
		// instance does not read it as news and a rebuild restores THIS.
		std::vector<float> now(floats.begin(), floats.end());
		uint64_t serial = 0;
		if (void* surface = store->surfaceOf(device); surface != nullptr && d->state->read != nullptr) {
			serial = d->state->read(surface, index, now.data(), now.size());
		}
		(void)store->refreshBlock(device, index, now, serial != 0 ? serial : device::nextExtraStateSerial());
		onBlockEdited(device);
		return true;
	}

	/// THE BLOCK AS THE HAND LAST WROTE IT (or as a load restored it), into `out` (`floats` long). False when the device has
	/// no such block or nothing was ever written (then `out` is untouched and the caller uses the block's default).
	bool blockAt(DeviceInstanceId device, size_t index, std::span<float> out) const {
		const device::Descriptor* d = descriptorOf(device);
		const ::feathers::ContainerBodies* store = blockStoreOf(device);
		if (d == nullptr || d->state == nullptr || store == nullptr || index >= d->state->blocks.size()
		    || out.size() != d->state->blocks[index].floats) {
			return false;
		}
		if (const void* surface = store->surfaceOf(device); surface != nullptr && d->state->read != nullptr
		                                                    && d->state->read(surface, index, out.data(), out.size()) != 0) {
			return true;
		}
		const std::vector<::feathers::ContainerBodies::Block>* stored = store->blocksOf(device);
		for (size_t s = 0; stored != nullptr && s < stored->size(); ++s) {
			if ((*stored)[s].index == index && (*stored)[s].floats.size() == out.size()) {
				std::copy((*stored)[s].floats.begin(), (*stored)[s].floats.end(), out.begin());
				return true;
			}
		}
		return false;
	}

	/// **A DEVICE'S CHAIN ACCESS SITE** (backlog NR) — its PRE and POST chains, made on first use: true once the device
	/// has one (made now, or before). False for a device that offers none (a container, a modulator) or a host with no
	/// sites. The running site is the container surface filed under `siteKeyOf(device)` — `containerSurfaceOf` and the
	/// pins above serve it unchanged — and its body is the bytes filed there.
	virtual bool openSite(DeviceInstanceId) { return false; }
	/// Whether the device has a site (made, or loaded) — what the card's chip reads.
	virtual bool hasSite(DeviceInstanceId) const { return false; }
	/// **THE DEVICE'S SITE, REPLACED WHOLE BY `body`** (a site's `SIT1` bytes, as a device preset carries them; 2026-10-29). Opened first if the device had none; the running site takes the bytes through its own `applyState`
	/// (record-only), or, while none runs yet, the host files them for the build to load. False for a device that offers
	/// no site, a host with none, or bytes the site refuses — and then nothing changed.
	virtual bool loadSite(DeviceInstanceId, std::span<const uint8_t> /*body*/) { return false; }
	/// **HOW MANY CHAINS DOWN THIS HOST'S CHAIN STANDS** — 0 for a plugin's own (Feathers' lanes, the Chain plugin's),
	/// one more inside every container. What a road that deepens hands the depth gate (`rack::fitsNesting`), so a
	/// preset or a paste too tall for where it lands is refused whole rather than reloading empty.
	[[nodiscard]] virtual size_t nestingDepth() const noexcept { return 0; }
	/// **WHETHER A CONTAINER'S OR A SITE'S BODY MAY STAND IN THIS HOST'S CHAIN** — the depth gate on a preset's bytes:
	/// the chains it brings, and everything inside them, must be ones a file gives back full. A host that nests
	/// nothing has nothing to refuse.
	[[nodiscard]] virtual bool holdsBody(std::span<const uint8_t> /*body*/) const { return true; }
	/// **A SOURCE IN DEVICE `owner`'s PRE CHAIN, AS THIS HOST NAMES IT** (backlog NZ): `Source::exported(owner, row)`,
	/// with the row minted in the running site's export table if it had none. `inner` is spelled the way PRE's own
	/// record spells it. `Source::nobody()` when the device has no running site, when `inner` is not a PRE modulator
	/// output (a POST source reaches only what follows it), or when the table is full. A route from the answer reaches
	/// the device itself (this host's `setRouteAmount`) and its POST (the POST lane's, spelled `asOuter()`).
	virtual backplane::Source siteExport(DeviceInstanceId /*owner*/, backplane::Source /*inner*/) {
		return backplane::Source::nobody();
	}
	/// **A SOURCE INSIDE THE CHAIN OR RACK `container`, AS THIS HOST NAMES IT** (backlog NZ (b)): `Source::exported(
	/// container, row)`, the row minted in the container's own export table if it had none. `inner` is spelled the way
	/// the container's inner records spell it (a modulator output or a macro). `Source::nobody()` for anything else, a
	/// device that exports nothing, or a full table. What a Chain's or a Rack's panel relays its inner arm as.
	virtual backplane::Source containerExport(DeviceInstanceId /*container*/, backplane::Source /*inner*/) {
		return backplane::Source::nobody();
	}
	/// **WHETHER THIS HOST RUNS ANYTHING PER VOICE** — Feathers' lane does; a rack with no voices (the standalone rack,
	/// or a chain inside a Rack) does not. What `runsPerVoice` stands on.
	[[nodiscard]] virtual bool hostsVoices() const { return false; }
	/// **WHETHER DEVICE `device` RUNS INSIDE THE VOICES** by its own row: this host has voices, nothing outside says they
	/// are unavailable, and the device's Poly switch is on — the engine's own test (`polyInRow`), asked of the record. A
	/// site's PRE does not run then (D4), and its panel says so.
	[[nodiscard]] bool runsPerVoice(DeviceInstanceId device) const {
		const device::Descriptor* d = descriptorOf(device);
		if (d == nullptr || !hostsVoices() || perVoiceUnavailable() != nullptr || d->traits.perVoiceControl < 0) {
			return false;
		}
		const auto valueOf = [&](size_t c) {
			return c < fieldsPresentAt(device) ? fieldAt(device, c)
			       : c < d->params.size()     ? static_cast<float>(d->params[c].def)
			                                  : 0.0f;
		};
		return valueOf(static_cast<size_t>(d->traits.perVoiceControl)) >= 0.5f && device::polyAllowed(*d, valueOf);
	}
	/// **HOLD THAT INSTANCE'S MEMORY WHILE AN EDITOR IS BUILT OVER IT** (backlog JX): the host does not free
	/// a pinned instance, however it retired, until it is unpinned — its handle is still cleared on time, so
	/// the reader's next re-ask sees it gone. Counted; every pin is paired with one unpin. A host that
	/// files no handles has nothing to pin.
	virtual void pinContainerSurface(const void*) {}
	virtual void unpinContainerSurface(const void*) {}
	/// **THE ONE PIN AN EDITOR HOLDS**, as a value: `hold` moves it (the new one pinned before the old is
	/// let go), and it lets go when it dies. An owner declares it BEFORE whatever it builds over the
	/// instance, so the pin outlives that, whose teardown still reads the instance.
	class SurfacePin {
	public:
		explicit SurfacePin(DeviceHost& host) noexcept : host_(host) {}
		~SurfacePin() { hold(nullptr); }
		SurfacePin(const SurfacePin&) = delete;
		SurfacePin& operator=(const SurfacePin&) = delete;
		void hold(const void* surface) {
			if (surface == held_) {
				return;
			}
			if (surface != nullptr) {
				host_.pinContainerSurface(surface);
			}
			if (held_ != nullptr) {
				host_.unpinContainerSurface(held_);
			}
			held_ = surface;
		}
		[[nodiscard]] const void* held() const noexcept { return held_; }

	private:
		DeviceHost& host_;
		const void* held_ = nullptr;
	};

	/// **WHERE MODULATION ACTUALLY LEFT THIS CONTROL THIS BLOCK — the number the moving part of an
	/// indicator draws, while the printed value and the pointer stay on the base.**
	///
	/// `device::ModDisplay::modulated` has always documented this ("what the moving part of the
	/// indicator draws — the arc, the bar — while the number stays put") and nothing could answer it,
	/// so `DeviceControl::display()` assigned the base and said why: a host that publishes where
	/// modulation landed lets the editor draw the ENGINE'S arithmetic, and recomputing it editor-side
	/// would be a second implementation of it across a thread boundary. This is that road.
	///
	/// **DEFAULTED TO THE BASE, WHICH IS THE HONEST ANSWER AND NOT A STUB.** A host that resolves no
	/// modulation of its own answers what it actually knows: the arc sits at the base, exactly as
	/// before, rather than a surface inventing a number the engine never computed.
	///
	/// **THIS SAID "OR HAS NOT GROWN THE ROAD YET, WHICH IS FEATHERS TODAY", AND THAT STOPPED BEING
	/// TRUE.** BK gave Feathers the road — `BridgeDeviceHost` reads the modulated rows of both its
	/// chains — and the rack's bridge has its own, so every host in this build now overrides this. The default is what a NEW host gets before it has a channel, not a description of anyone
	/// shipping. Left uncorrected it would tell the next reader that Feathers draws the base, which is
	/// the opposite of what it does, and a comment that outlives its behaviour is a thing this
	/// codebase keeps paying for.
	///
	/// AUDIO-THREAD-WRITTEN, UI-READ. An implementor publishes it the way `RackEngine::publishReadings`
	/// publishes meters: relaxed atomic stores into a buffer that is sized before the slot is visible
	/// and never resized while it is.
	///
	/// IT COST AN EVENING OF TESTING TO BE MISSING (backlog BE). A modulation system whose
	/// indicator cannot move is indistinguishable from one that does not modulate: route drawn, arc
	/// present, nothing moving, and the modulation working the whole time.
	virtual float modulatedFieldAt(DeviceInstanceId device, size_t control) const {
		return fieldAt(device, control);
	}

	/// **WHERE THE RIGHT EAR IS** (backlog KU): true, with `right` filled, only while a stereo route reaches this
	/// control. False — mono — is the default and the answer for any host with no right-ear channel; the ring then
	/// draws as it always has, from `modulatedFieldAt` alone.
	virtual bool modulatedFieldRAt(DeviceInstanceId device, size_t control, float& right) const {
		(void)device;
		(void)control;
		(void)right;
		return false;
	}

	/// THE RATE THE HOST IS RUNNING AT, or 0 when it does not know yet.
	///
	/// **A SURFACE NEEDS THIS TO BE TRUTHFUL RATHER THAN DECORATIVE.** `WaypointCard` draws a filter's
	/// response, and a response curve is a function of the sample rate: drawn at 48k and played at
	/// 44.1k it is a picture of a filter nobody is hearing. That is the honesty rule pointed at a
	/// plot — the same reason `waypoint_card.cpp` already had the number and said why.
	///
	/// ZERO IS A REAL ANSWER, not an error. A host that has been constructed and not yet activated has
	/// no rate, and a caller must draw something sensible rather than divide by it — which is why this
	/// returns a value rather than asserting.
	///
	/// IT IS ON THE HOST AND NOT ON THE DEVICE because the device does not own it: the host is told by
	/// its own host, and every device in the rack runs at the same rate by construction.
	virtual double sampleRate() const noexcept = 0;

	/// **SECONDS PER BEAT, OR ZERO WHEN THE HOST HAS NO TEMPO** — the other half of "what time is it",
	/// beside `sampleRate`, and it is here for exactly the reason that one is: the host is told by its
	/// own host, and every device in the rack runs against the same tempo by construction.
	///
	/// **ZERO MEANS UNKNOWN, WHICH IS A REAL ANSWER RATHER THAN AN ERROR.** A standalone rack, or a DAW
	/// that offers no tempo, genuinely has none — `TransportClock::haveTempo()` says the same thing on
	/// the audio side — so a caller must draw something sensible rather than divide by it. Not
	/// asserting, for the same reason `sampleRate` does not.
	///
	/// **WHY A SURFACE WANTS IT: a synced control's period in SECONDS is not knowable without it.** A
	/// card can read the division and the scale and still not know how long a cycle is, which is what
	/// stopped the LFO's card drawing what its smoothing does to a synced wave — the shape of a
	/// one-pole's response depends on `tau / period`, and the card had the tau and not the period.
	/// `mseg.h` and `randomTrace` carry the same gap, each noted where it bites.
	///
	/// DEFAULTED RATHER THAN PURE, unlike `sampleRate`: four classes implement this interface and a
	/// tempo is meaningful to one of them today. A host with nothing to say answers zero, which is
	/// the honest reading of "no transport" and needs no code at the other three.
	virtual double secondsPerBeat() const noexcept { return 0.0; }

	/// LIVE READINGS A DEVICE PUBLISHES FROM THE AUDIO THREAD — meters, phases, levels.
	///
	/// **ANONYMOUS FLOATS, AND THAT IS THE DESIGN RATHER THAN A LIMITATION.** The host carries numbers
	/// it does not understand and the DEVICE'S OWN UI knows what the indices mean. Putting
	/// `OwlttMeterReading` in this seam would name one device in a shared vocabulary, which is the pin
	/// the whole device-browser arc removed everywhere else — and the next device with a level meter
	/// would need a second struct beside it.
	///
	/// IT IS EXACTLY HOW `outputs` ALREADY WORKS FOR MODULATORS. This rack has never known what an
	/// envelope's output MEANS, only that it publishes one; `readingAt` is that same arrangement for
	/// an audio device, which publishes for a picture rather than for a route.
	///
	/// A DEVICE THAT PUBLISHES NOTHING ANSWERS ZERO, and a host that has never been given a reading
	/// answers zero too. Both are correct and a caller cannot tell them apart, which is right: a
	/// meter with no data and a device with no meters both draw as nothing.
	///
	/// **BUS DEVICES ONLY, AND THIS IS A REAL LIMIT.** `fx_runner.h` records why: *"a connector
	/// installed on the per-voice runner would see twenty voices publish to one slot and stomp each
	/// other. Per-voice publishing needs a scheme, not an index, and nobody has asked for one."* A
	/// rack has no voices so the limit does not bind there; in Feathers it does, and a per-voice
	/// device's readings would be the last voice to write rather than anything meaningful.
	virtual size_t readingCountAt(DeviceInstanceId device) const noexcept = 0;
	virtual float readingAt(DeviceInstanceId device, size_t index) const noexcept = 0;

	//--------------------------------------------------------------------------------------------//
	// THE ROUTING ROAD                                                                             //
	//--------------------------------------------------------------------------------------------//

	/// WHERE A MODULATOR IS IN ITS SHAPE, IF ANYBODY PUBLISHES IT. Returns false when nobody does.
	///
	/// **THIS IS A READING, NOT A SETTING, AND THE DISTINCTION IS THE WHOLE POINT.** A playhead is
	/// produced by the engine as it ticks — like OWLTT's meters and automod's phase — rather than being
	/// state a card can compute from the device's field row. So a host without a publisher must be
	/// able to SAY SO, and a card must draw no marker rather than a marker frozen at zero. A stuck
	/// playhead looks exactly like a stopped envelope, which is why "return 0.0f" would be the wrong
	/// default: it is a plausible picture of a wrong thing.
	///
	/// PRIMITIVES RATHER THAN `mod::Playhead`, deliberately: this vocabulary is the one every device
	/// declares itself in, and `modulator.h` argues that a trigger mode and an envelope stage belong
	/// to the modulator tier and not here. Two out-parameters cost nothing and keep the layering.
	///
	/// The default declines, so a host gains this by overriding rather than by remembering to.
	/// **HOW HARD A DEVICE IS BEING DRIVEN, IF ANYBODY PUBLISHES IT.** Returns false when nobody does.
	///
	/// A READING, not a setting, and it declines by default for the reason `playheadAt` gives one line
	/// down: a meter frozen at zero is a plausible picture of a wrong thing, and a surface must be able
	/// to draw NO meter rather than a meter that says silence when it means "not published".
	///
	/// It was asked for on the shaper (a level meter in the background of the transfer curve display, to
	/// see how hard the actual table is driven), and the unit is the one that
	/// surface needs: the peak INPUT, in the same 0..1 the transfer's x axis is drawn in. A device that
	/// meters something else should not answer here.
	virtual bool drivePeakAt(DeviceInstanceId device, float& peak) const {
		(void)device;
		(void)peak;
		return false;
	}

	/// **HOW LOUD A DEVICE IS COMING OUT** — every fx card's courtesy output meter (backlog MK): the loudest sample the
	/// device put out, held then falling, LINEAR and NOT clamped (above 1.0 is the over). Measured by whoever runs the
	/// device (`RackEngine::outputPeakAt`), so no device publishes it. Declines by default, `drivePeakAt`'s reason: a
	/// card draws no meter rather than a silent one when nobody can say.
	virtual bool outputPeakAt(DeviceInstanceId device, float& peak) const {
		(void)device;
		(void)peak;
		return false;
	}

	virtual bool playheadAt(DeviceInstanceId device, uint32_t& ordinal, float& phase) const {
		(void)device;
		(void)ordinal;
		(void)phase;
		return false;
	}

	/// **AND THE LEVEL BESIDE THE PLAYHEAD, IF THE DEVICE PUBLISHES ONE** (`mod::Playhead::level`) — raw 0..1, for a
	/// shape whose picture cannot say where it stands from the position alone. Declines by default, as above, and a
	/// card then places its mark on its own drawn line.
	virtual bool playheadLevelAt(DeviceInstanceId device, float& level) const {
		(void)device;
		(void)level;
		return false;
	}
	/// **HOW MANY TIMES A MODULATOR'S TRIG HAS FIRED** (backlog MV) — a count that only grows, read by the Trig pad to
	/// light on each fire. False when nobody publishes one, and the pad then never lights.
	virtual bool trigFiresAt(DeviceInstanceId device, uint32_t& fires) const {
		(void)device;
		(void)fires;
		return false;
	}
	/// **THE NEWEST EVENTS A MODULATOR MADE, NEWEST FIRST** (`device::RecentEvent`, `ModOps::recentEvents`) — what a card
	/// that looks once a frame draws a fast device's in-between events from. Writes at most `capacity` and returns how
	/// many it wrote; an empty entry (ordinal 0) ends them. 0 by default: nobody publishes, and a card draws the newest.
	virtual size_t recentEventsAt(DeviceInstanceId device, device::RecentEvent* out, size_t capacity) const {
		(void)device;
		(void)out;
		(void)capacity;
		return 0;
	}

	/// **A DEVICE IS ADDRESSED BY ITS ID AT EVERY HOST — and there is no lane on this seam at all**
	/// (kill-lanes step 2: addresses are not sequential, and ordering comes only from the graph).
	/// `routeLaneOf` retired first, when every host's address became a lane; then the lane went, because
	/// a lane is a pool index and only the pool's owner may know it. A host keeps its devices wherever
	/// it likes and finds one from its id inside, once.
	///
	/// A CONTRACT THAT SAYS "BOTH" WHILE THREE EXIST is the shape `routeLaneOf` taught — it promised two
	/// implementations and there were three (`RackEngine`, Feathers' fx host, and a mod-rack host
	/// addressed by a SECOND slot space). An id has one space, so Feathers has one host now, and the
	/// rack engine is no host at all: its editor reaches it through `RackBridge`.

	/// **IS THIS SOURCE PUBLISHING BOTH WAYS RIGHT NOW — which is not always what it DECLARES.**
	///
	/// `device::SourceDesc::bipolar` is the WIDEST signal an output can publish, and routing is sized
	/// by it on purpose: an amount whose meaning changed when a switch moved would be a route that
	/// re-scaled under the user's hand. A SURFACE asks a different question. "Which way does this
	/// reach from the notch" is about this block, and answered from the widest declaration it draws a
	/// reach the route does not currently have — the same objection that argued for declaring the
	/// widest, arriving from the other side.
	///
	/// **THE DEFAULT IS THE DECLARATION, AND THAT IS HONEST RATHER THAN A STUB.** A host that cannot
	/// see its modulators' switches — because its sources are macros, or because it has no live
	/// state to read — answers what it actually knows, and a permanently bipolar output (the
	/// tracker's Pitch) answers the same either way. A host that CAN see them overrides and reads the
	/// control `SourceDesc::bipolarControl` names.
	///
	/// ASKED OF THE HOST RATHER THAN COMPUTED BY THE SEATING, for the reason `sourceIsBipolar`
	/// already gives about its own existence: the seatings that fill a `device::ModDisplay` must not
	/// each grow their own answer, because that is how one fact came to have two of them.
	virtual bool sourceIsBipolarNow(Source source) const {
		return SourceView({}, devices()).isBipolar(source);
	}

	/// How far `source` reaches `destination`, in units of the destination's own range — BY ENDPOINT
	/// (kill-lanes step 2): a device end is its id and its own output or control index.
	virtual float routeAmount(Source source, Destination destination) const = 0;
	virtual void setRouteAmount(Source source, Destination destination, float amount) = 0;

	/// **WHAT THE ENGINE READS AROUND THIS ROUTE, FOR THE AS-MODULATED READOUT** (backlog PU; `route_probe.h`). Asking
	/// names the pair the audio thread answers from then on; true, with `out` filled, once an answer about THIS pair
	/// has arrived. **THE DEFAULT IS "CANNOT SAY", AND THE READOUT THEN SHOWS NOTHING** — a host whose engine is not
	/// in reach answers what it knows rather than a readout inventing a source's value. `unwatchRoute` stops it.
	virtual bool probeRoute(Source source, Destination destination, RouteProbe& out) {
		(void)source;
		(void)destination;
		(void)out;
		return false;
	}
	virtual void unwatchRoute() {}
	/// **WHOSE ROUTES THESE ARE** — one address per routing table, so the readout can tell an amount set through
	/// this host from one set through another object over the same table (Feathers' knobs write through the
	/// bridge, its cards and route list through this adapter). The host itself unless it adapts something.
	[[nodiscard]] virtual const void* routeIdentity() const noexcept { return this; }

	/// **THE CURVE A ROUTE READS ITS SOURCE THROUGH** — Lin for no
	/// route. A host that holds routes overrides all three; the defaults are a host with no curves, whose
	/// route list then draws no chip (`supportsRouteTransfers`).
	virtual bool supportsRouteTransfers() const { return false; }
	virtual Transfer routeTransfer(Source, Destination) const { return Transfer::Lin; }
	/// On an EXISTING route only — zero is not a route, so neither is a curve on one. Whether it was taken.
	virtual bool setRouteTransfer(Source, Destination, Transfer) { return false; }
	/// **WHETHER THE MENU OFFERS `t` FOR `source`** — the honesty rule on the menu: a rectifier changes
	/// nothing a unipolar source can say (Rectify and Pos half are its identity, Neg half its silence), so
	/// it is greyed while the source reads 0..1. A curve already held still SHOWS on the chip.
	virtual bool transferChoosable(Source source, Transfer t) const {
		return !device::transferNeedsBipolar(t) || sourceIsBipolarNow(source);
	}

	/// **WHETHER ANY SOURCE OTHER THAN `except` REACHES `destination`** — the "routed elsewhere" mark a
	/// control draws beside the armed source's ring. Asked of the host, which holds the routes: the
	/// widget used to walk every flat source index asking `routeAmount`, and there is no flat index now.
	virtual bool routedElsewhere(Destination destination, Source except) const = 0;

	/// Bracket an amount drag. A swallowed control is not a host parameter, so these mark the project
	/// dirty rather than latching automation — but they still have to be a pair.
	virtual void beginRouteGesture() = 0;
	virtual void endRouteGesture() = 0;

	/// **A HAND TOOK / LET GO OF A CONTROL'S VALUE** (host slots, T6b) — `DeviceControl::beginGesture` and
	/// `endGesture`, around the `setField`s the drag makes. A host that has exposed the control as a host
	/// parameter (a slot) brackets it for the DAW, so a drag records into the lane as one gesture; a host
	/// with no host parameters has nothing to tell and keeps these defaults. Always a pair.
	virtual void beginControlGesture(DeviceInstanceId device, size_t control) {
		(void)device;
		(void)control;
	}
	virtual void endControlGesture(DeviceInstanceId device, size_t control) {
		(void)device;
		(void)control;
	}
	/// **A HAND TOOK THIS CONTROL — the recent ring's one road in** (host slots, T9). Called by a control's
	/// `beginHostGesture` (a drag, a double-click reset, a card's verb) just BEFORE `beginControlGesture`, and
	/// by nothing else: an undo record and a row writer (`ControlGestureScope`) bracket the host but are not a
	/// hand, and a host write, a load or a preset never reaches here. A host with a pool gives an unslotted
	/// control a Recent slot, or moves a slotted one to the front of the recent order; a host with none keeps
	/// this default.
	virtual void touchControl(DeviceInstanceId device, size_t control) {
		(void)device;
		(void)control;
	}

	/// **WHERE A CONTROL STANDS WITH THIS HOST'S SLOTS, AND THE TWO VERBS A RIGHT-CLICK OFFERS** (host
	/// slots T7) — `DeviceControl`'s answers to `device::Modulatable::hostSlot` and its verbs, asked here
	/// because the host owns the pool. A host with no pool keeps these defaults: `Unavailable`, and every
	/// verb refuses, so its controls list no host entry. See `Modulatable::hostSlot` for each verb.
	[[nodiscard]] virtual device::HostSlotStatus hostSlotOf(DeviceInstanceId device, size_t control) const {
		(void)device;
		(void)control;
		return {};
	}
	virtual bool exposeToHost(DeviceInstanceId device, size_t control, std::string* displaced) {
		(void)device;
		(void)control;
		(void)displaced;
		return false;
	}
	virtual bool unpinFromHost(DeviceInstanceId device, size_t control) {
		(void)device;
		(void)control;
		return false;
	}
	/// **MOVES WHENEVER ANY ANSWER OF `hostSlotOf` MAY HAVE** — the pool's generation, which every mapping and
	/// state change bumps. A widget that draws a slot fact caches it on this rather than walking the pool on
	/// every paint. A host with no pool never moves it.
	[[nodiscard]] virtual uint64_t hostSlotGeneration() const noexcept { return 0; }

	//--------------------------------------------------------------------------------------------//
	// THE ARMED SOURCE'S OWN VIEW OF WHAT IT REACHES — a route list (rack roadmap §7f). Asked of the //
	// host, which holds the routes and names its own ends; the list that draws them is one widget   //
	// over this contract, in Feathers' Engine rail and in the rack's editor alike.                     //
	//--------------------------------------------------------------------------------------------//

	/// EVERY DESTINATION `source` REACHES AMONG THIS HOST'S OWN ROUTES: `f(ctx, destination)`, in no
	/// particular order — a list sorts. Function pointer over `void*`, the vocabulary's idiom.
	virtual void forEachRouteFrom(Source source, void* ctx, void (*f)(void* ctx, Destination d)) const = 0;
	/// **EVERY SOURCE ROUTED INTO `destination` AMONG THIS HOST'S OWN ROUTES**: `f(ctx, source)`, in no particular order
	/// (backlog NO item 2: a removed Rack chain's routes, kept in its undo record so an undo puts them back). Defaulted
	/// to none, for a host that keeps no routes of its own.
	virtual void forEachRouteInto(Destination /*destination*/, void* /*ctx*/, void (* /*f*/)(void* ctx, Source s)) const {}

	/// THE NAME A LIST PRINTS FOR `destination` — "Cutoff 2": the host's, because only the host knows
	/// one of its own parameters from a device's control, and where that device stands in its chain.
	virtual std::string destinationName(Destination destination) const = 0;

	/// **THE ROUTES INSIDE THE CONTAINERS THIS HOST HOLDS, FROM ONE OF ITS OWN SOURCES** — a chain's
	/// inner route naming an outer source (rack roadmap §7). Listed beside the host's own and edited from
	/// there, through the chain's own gate (`ContainerOps::forEachRouteFrom` and its three siblings;
	/// `inner_routes.h` restates them over a descriptor and a surface, so a host's override is a walk of
	/// its containers and nothing more). A host with no containers leaves the defaults: none.
	struct InnerRoute {
		DeviceInstanceId chain = kNoInstance;
		Destination destination {}; ///< in the CHAIN's id space
		float amount = 0.0f;
	};
	virtual std::vector<InnerRoute> innerRoutesFrom(Source) const { return {}; }
	virtual float innerRouteAmount(Source, DeviceInstanceId, Destination) const { return 0.0f; }
	/// Through the chain's own gate; whether it was taken.
	virtual bool setInnerRouteAmount(Source, DeviceInstanceId, Destination, float) { return false; }
	/// The inner route's curve, and its write — through the chain's own gate, as the amount.
	virtual Transfer innerRouteTransfer(Source, DeviceInstanceId, Destination) const { return Transfer::Lin; }
	virtual bool setInnerRouteTransfer(Source, DeviceInstanceId, Destination, Transfer) { return false; }
	/// "Chain 2 · Filter · Cutoff" — the chain by its place, the rest as the chain spells it.
	virtual std::string innerDestinationName(DeviceInstanceId, Destination) const { return "Chain"; }

	//--------------------------------------------------------------------------------------------//
	// PRESENTATION                                                                                 //
	//--------------------------------------------------------------------------------------------//

	/// WHETHER THIS HOST CAN OPEN AN EXPANDED VIEW FOR THAT DEVICE — asked by the card's head, so the
	/// way in is drawn exactly when pressing it would open something. The honesty rule, in the form
	/// it takes on a card.
	///
	/// **THE CARD ASKS; THE HOST ANSWERS FROM A TABLE IT OWNS.** `FxCard` used to answer this itself
	/// from `ui::hasExpandedView`, Feathers' registry of the panels THIS BUILD can construct — which
	/// put `plugin/params.h` under the base class every card in both hosts wears, and the card floor
	/// stopped compiling without the synth (backlog X; `feathers-device-ui-probe` is the instrument).
	/// A registry of presentable views is a host's private fact, like its axes: Feathers answers from
	/// its own, the rack from whatever its editor installed, and a card in either never names one.
	///
	/// IT IS A BOOL AND NOT A FRAME, which is what keeps it on this rendering-neutral seam: the card
	/// needs only "would a press open something", and the host that says yes is the one that builds
	/// it when the press arrives (`onExpand`).
	///
	/// **THE DEFAULT DECLINES**, as `playheadAt` does: null/false is the capability, so a host with
	/// no panels to show gains this by writing nothing, and a card on it draws no way in.
	virtual bool hasExpandedViewFor(const device::Descriptor& desc) const {
		(void)desc;
		return false;
	}

	/// **WHAT A DEVICE'S OUTPUT IS CALLED ON THIS INSTANCE** — the player's label, else the name the
	/// device declares, else "Source". Both hosts keep a label per (device, output) beside the card folds
	/// and save it with the patch (`ParamBridge::outputLabel`, `RackBridge::outputLabel`); this is that
	/// verb on the seam, because the MACROS card is built from a `DeviceHost` (`card_factory.h`) and
	/// renames its own outputs. Message thread.
	///
	/// **THE DEFAULTS ARE A HOST THAT KEEPS NO LABELS**: the declared name, never relabelled, a rename
	/// that does nothing and a subscription that is never told — so a slot over such a host prints the
	/// declared name and its field commits into nothing, which is what that host can honestly offer.
	virtual std::string outputLabel(DeviceInstanceId device, size_t output) const {
		const device::Descriptor* desc = descriptorOf(device);
		const char* name = desc != nullptr && output < desc->outputs.size() ? desc->outputs[output].name : nullptr;
		return name != nullptr && name[0] != '\0' ? std::string(name) : std::string("Source");
	}
	/// Whether the output carries a label of its own rather than its declared name.
	virtual bool outputRelabelled(DeviceInstanceId device, size_t output) const noexcept {
		(void)device;
		(void)output;
		return false;
	}
	/// The relabel — each host's own rules (clamped; empty or the declared name clears).
	virtual void setOutputLabel(DeviceInstanceId device, size_t output, std::string_view label) {
		(void)device;
		(void)output;
		(void)label;
	}
	/// **WHICH PRESET A DEVICE'S PANEL LAST LOADED OR SAVED** (backlog QC: an engine restart used to forget
	/// which preset it had loaded) — patch state beside the device's labels (`rack::DeviceLabels::presetOf`), so the panel
	/// reopens on it after a restart, a reopened project or a closed window. **THE DEFAULTS ARE A HOST THAT KEEPS NONE**:
	/// a nested chain's inner host keeps its devices' state inside the container's body, where no label map reaches yet,
	/// so a panel opened there opens on its bank's first entry as it always did.
	[[nodiscard]] virtual PresetIdentity devicePreset(DeviceInstanceId device) const {
		(void)device;
		return {};
	}
	/// Name it — the panel's browser, on a load or a save. Not an edit: naming changes no sound.
	virtual void setDevicePreset(DeviceInstanceId device, const PresetIdentity& identity) {
		(void)device;
		(void)identity;
	}
	/// **A DEVICE'S BODY WAS EDITED THROUGH ITS SURFACE** (backlog JV) — an IR loaded or reshaped, a preset's
	/// body applied — which no verb here carried, so the host could not see it. Said by the editor that made
	/// the edit, on the message thread, after it took; a host with a dirty flag raises it. Not for a load.
	virtual void bodyEdited(DeviceInstanceId device) { (void)device; }
	/// **WHY A POLY SWITCH ON THIS HOST'S DEVICES CHANGES NOTHING, or null** (backlog NO) — inside a Rack no host voice
	/// reaches, so a device there runs once on the bus. `DeviceControl::unavailable` asks it of a device's Poly control.
	[[nodiscard]] virtual const char* perVoiceUnavailable() const { return nullptr; }
	/// **WHICH OF A DEVICE'S OUTPUTS ITS CARD SHOWS** (Pattern design §7.10: a card shows the open tab's identity and
	/// lanes, the window all of them) — a bit per output, every bit set by default. VIEW STATE ONLY: a route names its
	/// source by (instance, index) and reads a hidden output exactly as a shown one. Set by the card on the message thread;
	/// a change tells `subscribeOutputLabel`'s listeners for each output that moved, so every host re-lays its chips.
	/// **THE DEFAULTS ARE A HOST THAT KEEPS NONE**: everything shown, and a set that does nothing.
	[[nodiscard]] virtual uint64_t outputsShown(DeviceInstanceId device) const noexcept {
		const device::Descriptor* d = descriptorOf(device);
		return d != nullptr ? d->outputsShownByDefault() : ~uint64_t { 0 };
	}
	virtual void setOutputsShown(DeviceInstanceId device, uint64_t mask) {
		(void)device;
		(void)mask;
	}
	/// **TOLD WHEN AN OUTPUT'S LABEL MOVES** — a rename, or a patch load that changed one. Hold the token.
	[[nodiscard]] virtual device::Subscription
	subscribeOutputLabel(std::function<void(DeviceInstanceId, size_t)> listener) {
		(void)listener;
		return {};
	}

	//--------------------------------------------------------------------------------------------//
	// POLICY                                                                                       //
	//--------------------------------------------------------------------------------------------//

	/// THE HOST'S OWN RULES ABOUT ITS OWN AXES — see `routing.h`. Defaulted to `permitAll`, because a
	/// host with no parameters of its own has no pair it could have a rule about, and making every
	/// such host write that out would be asking it to declare an absence.
	virtual CorePolicy corePolicy() const noexcept { return &permitAll; }

	/// WHETHER THIS SOURCE MAY REACH THIS DESTINATION — assembled, not overridden.
	///
	/// NON-VIRTUAL ON PURPOSE. `params.h` states why there must be exactly one authority: every road
	/// that creates a route asks it and so does every surface that offers one, and the honesty rule
	/// breaks the moment those two are computed separately. A host supplies its policy and its slots;
	/// it does not get to answer this question a second way.
	bool mayRoute(Source source, Destination destination) const {
		return backplane::mayRoute(axes(), devices(), source, destination, corePolicy());
	}

	/// **IS `source` — ONE OF THIS HOST'S OWN — UPSTREAM OF THE DEVICE `self`?** Rack roadmap §7: what
	/// this host answers for a chain it holds, so the chain's INNER gate can admit an inner route
	/// reading this source. The source must exist here (a core index in the table; a device output a
	/// modulator this host holds publishes), and a route from it into `self` must not have to be read
	/// before it is written — `mayRoute`'s own ordering half with the chain as the destination; the
	/// control index is not consulted by that half. Non-virtual for `mayRoute`'s reason: one authority.
	bool sourceUpstreamOf(Source source, DeviceInstanceId self) const {
		return backplane::sourceUpstreamOf(axes(), devices(), source, self);
	}
};

/// **ONE USER ACTION OVER SEVERAL FIELDS OF ONE DEVICE, BRACKETED FOR THE HOST** (host slots, T6b review) — a
/// card writing a whole row (an LFO or MSEG shape switch, an envelope's settings, drawn's compound edits, and
/// their undo records). Each CONTROL the action actually changes gets one Begin before its first write and
/// one End when the scope closes, so a host recording automation takes the action as one gesture per
/// param; a field written at the value it already had gets no gesture (a host recording would otherwise
/// write a flat point into every lane the row happens to carry). No undo of its own: the caller records
/// that as it always has.
class ControlGestureScope {
public:
	ControlGestureScope(DeviceHost& host, DeviceInstanceId device) : host_(host), device_(device) {}
	ControlGestureScope(const ControlGestureScope&) = delete;
	ControlGestureScope& operator=(const ControlGestureScope&) = delete;
	~ControlGestureScope() {
		for (size_t control : begun_) {
			host_.endControlGesture(device_, control);
		}
	}

	/// `host.setField(device, index, value)`, inside the action's gesture if it moves a control.
	void setField(size_t index, float value) {
		const device::Descriptor* d = host_.descriptorOf(device_);
		const bool isControl = d != nullptr && index < d->params.size();
		// **"UNCHANGED" AS THE SLOT OWNER READS IT** (`SlotOwner::recordHolds`, integration review): the
		// value COERCED by the control's declaration, against the record's value — its field, or its declared
		// default past `fieldsPresent`. The raw value, or "past the row is changed", opened a gesture on a
		// field the write merely carried; that gesture made it a hand's write, which seeded the record's
		// stale value over an unmirrored host write and sent the host a spurious Begin/Value/End.
		const bool unchanged = isControl && heldValue(*d, index) == device::coerceControl(d->params[index], value);
		if (isControl && !unchanged && std::find(begun_.begin(), begun_.end(), index) == begun_.end()) {
			host_.beginControlGesture(device_, index);
			begun_.push_back(index);
		}
		host_.setField(device_, index, value);
	}

private:
	/// The record's value of control `index`: its field, or its declared default past `fieldsPresent`.
	[[nodiscard]] float heldValue(const device::Descriptor& d, size_t index) const {
		return index < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, index)
		                                              : static_cast<float>(d.params[index].def);
	}

	DeviceHost& host_;
	DeviceInstanceId device_;
	std::vector<size_t> begun_;
};

} // namespace feathers::backplane
