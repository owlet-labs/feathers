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

// The CLAP binding: descriptor, entry point, ports, the forwarding of the params and state
// extensions to ParamSet, and the translation of a clap_process into note calls on the Synth.
// Deliberately thin — every decision about what a parameter *is* lives in params.h/.cpp and every
// decision about what a note DOES lives in synth.h/.cpp, both reachable by a test with no host.

#include "device/sidechain.h"
#include "device/transport_snap.h" // the host's position put on its sample (`device::onSample`)
#include "device/chain_tail.h"
#include "plugin/audition.h"
#include "plugin/clap_checking.h"
#include "plugin/feathers_entry.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/gui.h"
#include "plugin/editor_lanes.h"
#include "plugin/event_log.h"
#include "plugin/gui_bridge.h"
#include "rack/rack_bridge.h"
#include "plugin/params.h"
#include "plugin/state_block_order.h"
#include "plugin/state_streams.h"
#include "state/state_trailer.h"
#include "plugin/synth.h"
#include "plugin/denormal_guard.h"
#ifdef FEATHERS_DIAGNOSTIC
#include "plugin/spike_threshold.h"
#include <chrono>
#include <limits>
#endif

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"

#include "fx/automod/automod.h"
#include "fx/busutil/busutil_types.h"
#include "fx/owltt/owltt.h"

#include "device/format_value.h"
#include "hostslots/slot_host.h"
#include "plugin/bridge_device_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/preset_browser.h" // the test chain's `preset_face` (backlog QC)

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace feathers {

namespace {

// The checking level, and why it differs between debug and release: `plugin/clap_checking.h`.
using PluginBase = clap::helpers::Plugin<kClapMisbehaviour, kClapChecking>;

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_INSTRUMENT,
    CLAP_PLUGIN_FEATURE_SYNTHESIZER,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor kDescriptor = {
    CLAP_VERSION_INIT,
    "com.owlet.feathers",
    "Feathers",
    "Owlet Records",
    "https://owletrecords.com",
    "",
    "",
    FEATHERS_VERSION, // from project() in CMakeLists.txt, so a release bump cannot half-land here
    "Phi-triangle oscillator synthesiser",
    kFeatures,
};

class FeathersPlugin : public PluginBase, public GuiHost {
public:
	explicit FeathersPlugin(const clap_host* host) : PluginBase(&kDescriptor, host) {
		log_.say("create");
		// THE ENGINE'S ROUTE STORES GROW WITH THE BRIDGE'S LIST, before any publish needs them (backlog OJ).
		wireRouteGrowth(bridge_, params_, synth_);
		// The audition handle, seeded before anything can read it. Default unless the environment says
		// otherwise, and the default is the engine exactly as it was — see audition.h for why this is a
		// variable rather than a parameter, and why it announces itself.
		bridge_.restoreOrientation(orientationFromEnvironment());
		bridge_.setFalloffPower(falloffPowerFromEnvironment());
		double blueOvershoot = 0.0;
		double blueDoppler = 0.0;
		blueShiftScalesFromEnvironment(&blueOvershoot, &blueDoppler);
		bridge_.setBlueShiftScales(blueOvershoot, blueDoppler);
		bridge_.setV1Crossfade(v1CrossfadeFromEnvironment());
		// **THE HOST HEARS A MACRO'S NEW NAME** (macro A9). For the plugin's whole life, beside the editor's
		// own subscription, which is why the bridge keeps a list. See `onMacroRenamed`.
		macroRenamed_ = bridge_.subscribeMacroRename([this](size_t) { onMacroRenamed(); });
		// A FIRST GRAB IS SERVICED AT ITS BEGIN (`ParamBridge::beginControlGesture`): bound and rescanned now, so the
		// host hears the Begin in the next block on a slot it already knows by name. Main thread, as the hand is.
		bridge_.setFirstGrabService([this] { serviceSlots(); });
		// CONNECT THE OWLTT METERS — installed in the CONSTRUCTOR, not guiCreate, because FxRunner
		// invokes the meter connector once as each device is CONSTRUCTED. A OWLTT in a loaded patch
		// is constructed before any editor opens, so a guiCreate install reaches nothing. The bridge
		// is a plugin member and outlives every editor cycle — which is exactly why the wiring must
		// not wait for one. (Third time this wiring moved: no caller → editor-owned memory → caller
		// too late. A wiring step is not done when it exists; it is done when it provably reaches
		// every instance, including ones created by a road nobody was thinking about.)
		// **THE LIST OF DEVICES A RACK IN THE LANE MAY BUILD** — this build's, because Feathers is the
		// host. A rack in the lane is handed it and builds from it exactly as the standalone rack does.
		synth_.setCatalogue(hostCatalogue());
		// THE KIT PADS' FX LANES, for the kit window's rail (drum proposal §17 P3): the Synth's, built on the first ask to make one.
		bridge_.setPadLaneSource([this](size_t pad, bool make) { return make ? synth_.ensurePadLane(pad) : synth_.padLane(pad); });
		// **THE HOST SLOTS' BUS, FOR THE LANE'S WHOLE LIFE** (T6b): the bridge's, whose address is fixed for
		// the plugin's life. The engine applies it over every bound control before it resolves.
		synth_.setHostSlotBus(&bridge_.slotBus());
		// A CHAIN IN THE LANE IS TOLD THIS PLUGIN IS ITS HOST (rack roadmap §7): the bridge's record answers
		// whether one of Feathers' sources is upstream of the chain, so a control inside it may read it.
		// AND ITS EDITS MARK THIS PATCH DIRTY (`ParamBridge::outerGateOver`, backlog NO).
		synth_.setOuterGateMaker(&ParamBridge::outerGateOver, &bridge_);
		// AND ITS CONTROLS TAKE THEIR HOST SLOTS FROM THIS PLUGIN'S POOL (host slots T11, design §7): the bridge
		// owns it, and its address is fixed for the plugin's life.
		synth_.setSlotLinkMaker(&ParamBridge::slotLinkOver, &bridge_);

		synth_.setDspConnector([this](const char* tag, size_t slot, void* dspStorage) {
			if (dspStorage == nullptr) {
				return;
			}
			// **THE MATCH IS HERE AND NOT IN THE RUNNER, and this is the right side of the boundary**:
			// the thing that wants a device's readings is the thing that should know it wants them.
			// The tag is a PARAMETER, so it costs no capture — see the small-object-buffer notes
			// below, which are exactly as tight as they were.
			//
			// **ONE CONNECTOR, TWO DEVICES, AND THAT IS NOT A STYLE CHOICE.** `FxRunner` holds a
			// SINGLE `dspConnector_` and `setDspConnector` REPLACES it, so a second
			// `synth_.setDspConnector(...)` here would silently drop the first — a wiring bug that
			// reads as two installed roads at the source and that no test would notice, because the
			// road it deletes is the one nobody was looking at. So each device gets a branch rather
			// than a call.
			// **THE ENGINE HAS ALREADY INSTALLED THE GENERIC SINK ON THIS DEVICE** (`AudioOps::setReadingsSink`,
			// 2026-09-13 — the road a device inside a chain and the standalone rack meter by). The per-type
			// wiring below REPLACES it for the lane, because this bridge still reads its own per-type stores;
			// the day `BridgeDeviceHost::readingAt` reads the lane engine's readings instead, this block goes.
			if (feathers::device::sameTag(tag, feathers::fx::owltt::kTraits.tag)) {
				auto* owltt = static_cast<feathers::fx::owltt::Owltt*>(dspStorage);
				auto onMeters = [this, slot](size_t, const std::array<float, 3>& grDB,
				                                     const std::array<float, 3>& inputLevelDB,
				                                     const std::array<float, 3>& outputLevelDB, bool clipping,
				                                     const feathers::fx::owltt::Owltt::MeterExtras& extras) {
					bridge_.publishOwlttMeters(slot, grDB, inputLevelDB, outputLevelDB, clipping,
					                           extras.bandSaturating, extras.combinedGrDb, &extras.secondChannelGr,
					                           extras.noteHz);
				};
				// THIS CLOSURE IS CONSTRUCTED INTO A `std::function` ON THE AUDIO THREAD — the connector
				// runs from `FxRunner::sync`, once per OWLTT slot, every block. libstdc++ gives
				// `std::function` a 16-byte small-object buffer; this captures `this` and `slot`, which
				// is 16 bytes ON THE NOSE. One more captured value spills it to the heap, silently,
				// every block, and no rendered sample would change.
				//
				// ASSERTED HERE RATHER THAN IN A TEST, because a test can only assert about a COPY of
				// this closure — and a copy of the thing is not the thing. The runner suite has such a
				// case, honestly labelled "the plugin's shape, deliberately copied"; adding a capture
				// HERE leaves it green, which is the drift this line removes.
				// BOTH CLAUSES ARE LOAD-BEARING. libstdc++ stores locally only when the closure fits
				// AND is no more aligned than a pointer; `sizeof <= 16` alone is NECESSARY AND NOT
				// SUFFICIENT. MEASURED, g++-14 -O3, same signature: sizeof=16/alignof=8 allocates
				// nothing, sizeof=16/alignof=16 allocates ONE — so a closure capturing anything
				// `alignas(16)`, a SIMD value being the realistic way in, passes a size-only assert and
				// heap-allocates here every block. The first version of this assert had only the size
				// clause, and its known-bad was a third capture — which moves `sizeof` and therefore
				// never exercised the alignment condition at all. **Run the mutation the assertion
				// claims to catch, not the one that is easy to write.**
				static_assert(sizeof(onMeters) <= 16 && alignof(decltype(onMeters)) <= 8,
				              "meter callback no longer fits std::function's small-object buffer (it "
				              "must be <= 16 bytes AND <= 8-aligned): it would allocate on the audio "
				              "thread every block");
				owltt->setMeterCallback(onMeters);
				return;
			}

			// AND THE AUTOMODULATOR'S LFO PHASE, so the card's strip can mark where the waveform is.
			//
			// SAME REASONS FOR THE CONSTRUCTOR AS THE METERS ABOVE: the connector fires as each device
			// is CONSTRUCTED, and an automodulator in a loaded patch is constructed before any editor
			// opens, so a `guiCreate` install would reach nothing.
			if (feathers::device::sameTag(tag, feathers::fx::automod::kTraits.tag)) {
				auto* automod = static_cast<feathers::fx::automod::Automod*>(dspStorage);
				auto onPhase = [this, slot](float phase, float envelope) {
					bridge_.publishAutomodReadings(slot, phase, envelope);
				};
				// THE SAME 16-BYTE PROPERTY, ASSERTED THE SAME WAY AND FOR THE SAME REASON: this
				// closure is constructed into a `std::function` ON THE AUDIO THREAD, once per automod
				// slot, every block. It captures `this` and `slot` — 16 bytes on the nose.
				//
				// BOTH CLAUSES ARE LOAD-BEARING, and the measurement is the meters' note above:
				// libstdc++ stores locally only when the closure FITS **and** is no more aligned than
				// a pointer, so `sizeof <= 16` alone passes an `alignas(16)` capture that
				// heap-allocates here every block.
				static_assert(sizeof(onPhase) <= 16 && alignof(decltype(onPhase)) <= 8,
				              "phase callback no longer fits std::function's small-object buffer (it "
				              "must be <= 16 bytes AND <= 8-aligned): it would allocate on the audio "
				              "thread every block");
				automod->setReadingsCallback(onPhase);
				return;
			}
			// **BUS UTILITY'S METER, ONTO THE GENERIC CHANNEL.** A function pointer and a context
			// rather than a `std::function`, so there is no small-object buffer to overflow and no
			// `static_assert` above it that a future edit has to remember: two words cannot miss a
			// buffer they never use. The context is the bridge itself, and the slot rides in a
			// per-slot trampoline the same way the closure above captures it — see `busutil.h`.
			if (feathers::device::sameTag(tag, feathers::fx::busutil::kTraits.tag)) {
				if (readingsHops_.at(slot) == nullptr) {
					return;
				}
				readingsHops_[slot] = ReadingsHop { &bridge_, slot };
				feathers::fx::busutil::setReadingsCallback(
				    dspStorage,
				    [](void* context, const float* values, size_t count) {
					    auto* hop = static_cast<ReadingsHop*>(context);
					    hop->bridge->publishDeviceReadings(hop->slot, values, count);
				    },
				    &readingsHops_[slot]);
				return;
			}

			// **AND EVERY OTHER DEVICE'S READINGS, ONTO THE SAME GENERIC CHANNEL.** It was drawing an empty bed for ever, because nothing
			// published to the store its card reads.
			//
			// **THE ENGINE'S GENERIC SINK GOES SOMEWHERE ELSE.** The note above says so — it installs
			// `AudioOps::setReadingsSink` into the LANE ENGINE's readings, while
			// `BridgeDeviceHost::readingAt` reads this BRIDGE's per-slot store. For OWLTT, automod and
			// Bus Utility the branches above bridge that gap by hand, one device at a time; a device
			// with no branch had a sink that published where nobody was looking. That is the last line
			// of the note above — *"the day `BridgeDeviceHost::readingAt` reads the lane engine's
			// readings instead, this block goes"* — arriving as a defect rather than as a cleanup.
			//
			// **GENERIC RATHER THAN A FOURTH BRANCH**, so the next device to publish a picture needs no
			// entry here at all: any device whose ops offer the hook gets the bridge trampoline, using
			// the same per-slot hop and the same two words Bus Utility's uses.
			if (const device::Descriptor* desc = deviceForTag(tag);
			    desc != nullptr && desc->audio != nullptr && desc->audio->setReadingsSink != nullptr
			    && readingsHops_.at(slot) != nullptr) {
				readingsHops_[slot] = ReadingsHop { &bridge_, slot };
				desc->audio->setReadingsSink(
				    dspStorage, device::ReadingsSink { &readingsHops_[slot],
				                                       [](void* context, const float* values, size_t count) {
					                                       auto* hop = static_cast<ReadingsHop*>(context);
					                                       hop->bridge->publishDeviceReadings(hop->slot, values,
					                                                                          count);
				                                       } });
			}
		});
	}

	//---------//
	// GuiHost //
	//---------//

	void guiRequestFlush() noexcept override {
		if (_host.canUseParams()) {
			_host.paramsRequestFlush();
		}
	}

	void guiRequestResize(uint32_t width, uint32_t height) noexcept override {
		if (_host.canUseGui()) {
			_host.guiRequestResize(width, height);
		}
	}

	/// A HOST THAT DESTROYS US SAYS SO; a process that dies says so by this line's absence.
	~FeathersPlugin() override {
		log_.say("destroy");
		// **THE LANE LETS GO OF THE BRIDGE BEFORE THE BRIDGE GOES** (rack-teardown-uaf). `bridge_` is declared after
		// `synth_`, so it is destroyed FIRST — and every chain in the lane holds an outer gate and a host-slot link
		// pointing at it (`setOuterGateMaker`/`setSlotLinkMaker` in the constructor). The synth's teardown destroys
		// those chains, and a nested rack dying services its sites, which pull bindings through the link: a read of the
		// freed bridge's slot pool. Unhooked here, while both are alive, nothing below calls up.
		synth_.unhookHostLinks();
#ifdef FEATHERS_DIAGNOSTIC
		if (spikeTimer_ != CLAP_INVALID_ID) {
			_host.timerSupportUnregister(spikeTimer_);
		}
#endif
	}

	/// CAPTURE THE FULL PLUGIN STATE for the preset browser. Same walk as stateSave — parameters,
	/// then every tagged block in order — but to whatever stream the caller hands us.
	void guiCaptureState(clap_ostream_t* stream) noexcept override {
		if (stream == nullptr) {
			return;
		}
		// A PRESET CARRIES NO HOST SLOTS (plan §0.8): the chains block reads this while the walk runs.
		const PresetRoadScope preset(presetRoad_);
		(void)saveWalk(stream, "preset-capture");
	}

	/// RESTORE THE FULL PLUGIN STATE for the preset browser. Same walk as stateLoad — parameters,
	/// then the tagged blocks — from whatever stream the caller hands us.
	bool guiLoadState(clap_istream_t* stream) noexcept override {
		// EVERY REFUSAL ALONG THIS WALK IS NOW REPORTED rather than swallowed. The three early returns
		// were already here and already correct — what they lacked was a caller who could tell them
		// apart from success. See the declaration in gui.h.
		if (stream == nullptr) {
			return false;
		}
		const StateLoadScope loading(*this); // renames the walk makes reach the host once, below
		// **THE PRESET ROAD KEEPS THE PROJECT'S HOST SLOTS** (decided 2026-09-25): this is the one
		// caller that is not the host's `state.load`, so it is the one that tells the chains block so. The
		// pool stays, and only slots whose targets the preset's chains do not hold are freed.
		const PresetRoadScope preset(presetRoad_);
		if (!loadWalk(stream, "preset-load")) {
			return false;
		}
		serviceSlots(); // recorded, not sent: the load scope holds it for the rescan below
		rescanParams(CLAP_PARAM_RESCAN_VALUES);
		if (_host.canUseParams()) {
			_host.paramsRequestFlush();
		}
		return true;
	}

protected:
	//-------------//
	// Audio ports //
	//-------------//

	bool implementsAudioPorts() const noexcept override { return true; }

	// ONE STEREO OUT, AND A SIDECHAIN IN (design §10.1): a NON-main stereo input the fx lane's devices may listen to (the Spectral
	// vocoder's modulator; the synth is its carrier). Static, so no rescan is ever asked: a host reads it on instantiation.
	uint32_t audioPortsCount(bool) const noexcept override { return 1; }

	bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override {
		if (index != 0 || info == nullptr) {
			return false;
		}
		if (isInput) {
			info->id = 1;
			std::snprintf(info->name, sizeof(info->name), "Sidechain");
			info->flags = 0; // not main: an aux input
			info->channel_count = 2;
			info->port_type = CLAP_PORT_STEREO;
			info->in_place_pair = CLAP_INVALID_ID;
			return true;
		}
		info->id = 0;
		std::snprintf(info->name, sizeof(info->name), "Main");
		info->flags = CLAP_AUDIO_PORT_IS_MAIN;
		info->channel_count = 2;
		info->port_type = CLAP_PORT_STEREO;
		info->in_place_pair = CLAP_INVALID_ID;
		return true;
	}

	//------------//
	// Note ports //
	//------------//

	bool implementsNotePorts() const noexcept override { return true; }

	uint32_t notePortsCount(bool isInput) const noexcept override { return isInput ? 1 : 0; }

	bool notePortsInfo(uint32_t index, bool isInput, clap_note_port_info* info) const noexcept override {
		if (!isInput || index != 0) {
			return false;
		}
		info->id = 0;
		std::snprintf(info->name, sizeof(info->name), "Notes");
		// Both dialects accepted, CLAP preferred: the CLAP dialect carries note ids and per-note
		// expression, which is what Level's per-note modulation needs to mean anything.
		info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
		info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
		return true;
	}

	//--------//
	// Params //
	//--------//

	bool implementsParams() const noexcept override { return true; }

	/// THE CORE PARAMETERS, THEN THE 64 HOST SLOTS (backlog JU, plan T6a). Fixed while active, as every count
	/// must be: the slots are declared whether or not anything is mapped to them, and a mapping only renames.
	uint32_t paramsCount() const noexcept override {
		return static_cast<uint32_t>(kParamCount) + hostslots::kDeclaredSlots;
	}

	/// **A PARAM ID'S INDEX IN O(1).** clap-helpers asks this on EVERY `params.value`, `value_to_text` and
	/// `text_to_value` (its id check, even at `CheckingLevel::Minimal`), and its own version walks `paramsInfo` over
	/// every param until the id turns up. At about 1400 params a host's rescan, which reads each param's value and
	/// text, built about four million `clap_param_info` and stalled the main thread for about half a second (measured,
	/// test_clap_render "MEASURE: a first grab's rescan, …"). The answer is the walk's own, by construction: the
	/// core table's position for a core id (`paramsInfo` serves `paramDescs()` in order, and `paramIndexOf` indexes
	/// the same table), then the slots in slot order. -1 for anything else, as the base says.
	int32_t getParamIndexForParamId(clap_id paramId) const noexcept override {
		if (hostslots::isSlotId(paramId)) {
			return static_cast<int32_t>(kParamCount + (paramId - hostslots::kFirstSlotId));
		}
		return paramIndexOf(paramId);
	}

	// MAIN THREAD, as CLAP guarantees for `params.get_info` — which is what lets it read the macro names, mutable
	// state the main thread owns (the bridge renames on it), with no lock.
	bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override {
		++infoBuilds_;
		const std::span<const ParamDesc> descs = paramDescs();
		if (paramIndex >= descs.size()) {
			// PAST THE CORE TABLE ARE THE SLOTS, in slot order; the pool (on the bridge) says what each names.
			return slotHost_.info(static_cast<hostslots::SlotIndex>(paramIndex - descs.size()), info);
		}
		const ParamDesc& desc = descs[paramIndex];
		info->id = static_cast<clap_id>(desc.id);
		info->flags = desc.flags;
		// No cookie: the host would hand it back on every event, but it buys a pointer dereference
		// over a 24-entry scan, and a stale cookie across a reload is a crash rather than a wrong
		// value.
		info->cookie = nullptr;
		std::snprintf(info->name, sizeof(info->name), "%s", paramDisplayName(desc).c_str());
		std::snprintf(info->module, sizeof(info->module), "%s", desc.module);
		info->min_value = desc.min;
		info->max_value = desc.max;
		info->default_value = desc.def;
		return true;
	}

	/// **WHAT THE HOST CALLS A PARAMETER — THE ONE LOOKUP.** The parameter table's spelling, except where
	/// the user has named the thing: today that is a renamed macro, whose name is patch state the table
	/// cannot hold. The id and the list never move with it — a DAW keys its automation by id — so a
	/// rename costs the host a `CLAP_PARAM_RESCAN_INFO` and nothing else (see `onMacroRenamed`).
	///
	/// ONE FUNCTION rather than a branch inside paramsInfo, because a parameter whose name the user owns
	/// will not stay a macro-only idea, and the next one should be a case here, not a second road.
	/// Unrenamed, a macro answers the TABLE's name, so a patch that names nothing reports exactly what
	/// every build before this did.
	std::string paramDisplayName(const ParamDesc& desc) const {
		for (size_t i = 0; i < kMacroCount; ++i) {
			if (desc.id == macroParam(i) && bridge_.macroRenamed(i)) {
				return bridge_.macroName(i);
			}
		}
		return desc.name;
	}

	/// **A NAME MOVED: THE HOST IS OWED A `CLAP_PARAM_RESCAN_INFO`.** The flag is `clap/ext/params.h`'s own
	/// for a name change — "New info takes effect immediately", no deactivation, main thread — which is
	/// the thread the bridge tells us on.
	///
	/// ONE RESCAN PER GESTURE, NOT PER MACRO. A rename outside a load is one macro and is paid at once. A
	/// state load tells us once per macro whose name moved, so inside one the debt is only RECORDED, and
	/// the load pays it when its walk is done — folded into the `RESCAN_VALUES` it already sends, so a
	/// patch that renames three macros costs the host exactly the one rescan it cost before.
	void onMacroRenamed() noexcept {
		infoRescanOwed_ = true;
		if (!loadingState_) {
			rescanParams(0);
		}
	}

	/// THE ONE CALL TO `paramsRescan`: `flags`, plus INFO if a rename is owed. Nothing sent if both are
	/// empty, or to a host with no params extension.
	void rescanParams(clap_param_rescan_flags flags) noexcept {
		if (std::exchange(infoRescanOwed_, false)) {
			flags |= CLAP_PARAM_RESCAN_INFO;
		}
		flags |= std::exchange(slotRescanOwed_, 0u); // the host slots' renames, `serviceSlots` says
		if (flags != 0 && _host.canUseParams()) {
			_host.paramsRescan(flags);
		}
	}

	/// **THE HOST SLOTS' SHARE OF A MAIN-THREAD VISIT** (plan §1.3). T6a's part: whatever the pool renamed since
	/// the last visit — takeovers, frees, a restore — is owed to the host as ONE `RESCAN_VALUES | RESCAN_INFO | RESCAN_TEXT`,
	/// however many slots moved, exactly as a macro rename is: sent now, or, inside a state load, recorded
	/// for the load's own rescan (`StateLoadScope`). T6b adds the anchor fold and the binding republish here.
	void serviceSlots() noexcept {
		// THE FALLS A QUICK PRESS LEFT WAITING (`rack::HeldFalls`), once the synth has adopted the publish of their rises.
		bridge_.landHeldControls(synth_.chainsAdopted());
		// FIRST, FREE WHAT STOPPED RESOLVING (plan §1.3's reconcile): a device replaced under its id, or
		// dropped by a chain rebuild, leaves no removal behind for `freeUnder` to see. Gated on the record's
		// revision and the pool's generation, so a visit where nothing moved costs two compares.
		try {
			(void)bridge_.reconcileSlotsIfMoved();
		}
		catch (...) {
			// Out of memory on the main thread: the slot stays until the next visit, which tries again.
		}
		// THEN THE WRITE ROADS (T6b): host writes and recording touches anchored, the record's bindings rebuilt
		// from the pool, and the host's values mirrored into the record. Before the rescan below, and — through
		// `stateSave`, which opens with this call — before any save, so a host write no visit has seen yet is
		// an anchor in the saved pool (plan §1.3).
		(void)bridge_.serviceSlotRoads();
		// AND THE RACKS IN THE LANE (host slots T11): each reports its devices, pulls what the pool now binds
		// inside it (seeding the bus from its record), mirrors the host's writes into its record — so a save
		// right after automation carries the value inside the rack too — and tells the pool it adopted, which
		// is what hands the audio thread a nested slot's declaration.
		try {
			synth_.serviceContainers(bridge_.chainState(), &bridge_.fxBodies());
			bridge_.noteRefusedRoutes(synth_.takeOuterDrops());
			// AND THE CONTAINERS' OWN CONTROLS (backlog NO): released ones pruned before anything may take them again, a mirror a load owes written.
			(void)bridge_.serviceOwnControls();
			// AND THE EXPORT ROWS NO ROUTE READS ANY MORE (backlog NZ), once the audio thread has adopted a later publish.
			(void)bridge_.sweepExports(params_.routesGeneration());
		}
		catch (...) {
			// Out of memory on the main thread: the racks catch up at the next visit.
		}
		slotRescanOwed_ |= slotHost_.rescanOwed();
		if (!loadingState_) {
			rescanParams(0);
		}
	}

	/// **THE HOST'S "CALL ME ON THE MAIN THREAD"** (`clap_host::request_callback`), which a slot change asks for
	/// where no editor is ticking. Nothing else in this plugin requests it, so this is the slots' visit.
	/// With no editor ticking, this visit is also the one that tells the host its patch changed: a slot a host
	/// write NEWLY anchored is saved with the project (`serviceSlotRoads` marks it).
	void onMainThread() noexcept override {
		sayTransportOnce();
		serviceSlots();
		if (!loadingState_ && bridge_.takeStateDirty() && _host.canUseState()) {
			_host.stateMarkDirty();
		}
	}

	/// Ask for a main-thread visit, so a pool change reaches the host with no editor open.
	void requestSlotService() noexcept { _host.requestCallback(); }

	/// **WHILE A LOAD WALKS ITS BLOCKS, RENAMES ARE RECORDED, NOT SENT.** A scope rather than a pair of
	/// assignments, so the refusal that returns from the middle of a walk still settles what it owes: a
	/// block that renamed a macro before a later one failed has changed the name the host would read.
	class StateLoadScope {
	public:
		explicit StateLoadScope(FeathersPlugin& plugin) noexcept : plugin_(plugin) { plugin_.loadingState_ = true; }
		StateLoadScope(const StateLoadScope&) = delete;
		StateLoadScope& operator=(const StateLoadScope&) = delete;
		~StateLoadScope() {
			plugin_.loadingState_ = false;
			plugin_.rescanParams(0); // nothing, unless a failed walk left a rename owed
		}

	private:
		FeathersPlugin& plugin_;
	};

	/// **WHILE THE PRESET BROWSER'S WALK RUNS, THE CHAINS BLOCK KNOWS IT IS A PRESET** — so a capture writes
	/// no `hostSlots` and a load keeps the project's pool (plan §0.8). A scope for `StateLoadScope`'s reason:
	/// a walk that returns from the middle must not leave the next host `state.load` thinking it is a preset.
	class PresetRoadScope {
	public:
		explicit PresetRoadScope(bool& flag) noexcept : flag_(flag) { flag_ = true; }
		PresetRoadScope(const PresetRoadScope&) = delete;
		PresetRoadScope& operator=(const PresetRoadScope&) = delete;
		~PresetRoadScope() { flag_ = false; }

	private:
		bool& flag_;
	};

	// **EVERY ONE OF THESE ASKS `isSlotId` BEFORE `findParam`**: a slot id is outside the core table, where it
	// would read as "no such parameter" and hide the slot rather than fail loudly.
	bool paramsValue(clap_id paramId, double* value) noexcept override {
		if (hostslots::isSlotId(paramId)) {
			return slotHost_.value(paramId, value);
		}
		if (findParam(paramId) == nullptr) {
			return false;
		}
		// The base, not the resolved value: CLAP asks here for what automation and state own, and
		// reporting base+modulation would make a moving modulator look like a moving automation lane.
		*value = params_.base(paramId);
		return true;
	}

	bool paramsValueToText(clap_id paramId, double value, char* display,
	                       uint32_t size) noexcept override {
		if (hostslots::isSlotId(paramId)) {
			return slotHost_.valueToText(paramId, value, display, size);
		}
		return params_.valueToText(paramId, value, display, size);
	}

	bool paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept override {
		if (hostslots::isSlotId(paramId)) {
			return slotHost_.textToValue(paramId, display, value);
		}
		return params_.textToValue(paramId, display, value);
	}

	void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override {
		// Not a process call, so there are no samples to slice against and nothing to time events
		// against: every parameter in the list applies at once, and nothing but parameters is read.
		consumeParamEvents(in);

		// The other half of the editor's road. While the plugin is inactive there is no process() to
		// drain the queue, so a knob turned on a stopped transport arrives here instead.
		//
		// WHICH THREAD THIS IS depends on state, and the distinction is worth stating precisely because
		// the safety of everything below it rests on the right half. CLAP puts flush on the AUDIO
		// thread while the plugin is active and on the MAIN thread while it is inactive, so naming
		// either one as "the" flush thread is wrong. What holds in both cases is that flush and
		// process() never run at once — and mutual exclusion with process(), not main-thread-ness, is
		// what makes it safe to touch the modulation lane and to publish from here. See the threading
		// note in params.h, which states the same rule for ParamSet.
		consumeEditorEvents(out);
		// A routing dragged on a stopped transport arrives the same way a knob does: there is no
		// process() to snapshot it, so this is the other half of that road. Same mutual-exclusion
		// argument as the modulation lane above.
		// THE THREE LANES THAT ARE NOT PARAMETERS — routings, the rack's device settings and the
		// fx lane's. Written once and called from both entry points, because written twice at two
		// sites that must agree is exactly how setModRack came to exist at one of them and not the
		// other. See editor_lanes.h.
		publishEditorLanes(bridge_, params_, synth_);
		// AND THE SPECTRAL DRAWER'S STORED STATE (D2): on a sleeping plugin a flush is the only road a curve stroke or the
		// master switch has to the engine, and the drawer's plot is made from what the engine holds. Cleared before the
		// read, so an edit landing after it asks for another flush (ParamBridge::hasPendingWork).
		bridge_.clearSpectralMoved();
		synth_.setSpectralState(bridge_.spectralState());
		bridge_.publish(params_);
		// And where modulation has left the rack — the editor draws its bars from the ENGINE's own
		// arithmetic rather than repeating it. See Synth::modulatedRack.
		publishEngineReadings(bridge_, synth_);
		bridge_.publishMasterMeter(masterMeterReadingsOf(synth_.masterMeter())); // backlog DN
		bridge_.publishPadHits(synth_.padHits()); // the kit pads' pulse (pad_hits.h)
	}

	//-------//
	// State //
	//-------//

	bool implementsState() const noexcept override { return true; }

	/// THE ORDER OF THE TAGGED BLOCKS, IN ONE PLACE, ITERATED BY BOTH DIRECTIONS.
	///
	/// It used to live twice — a chain of && in stateSave and a run of if-blocks in stateLoad — and the
	/// two agreeing was a thing somebody had to notice. They stopped agreeing once already: the
	/// variations block and the modulation routings block were written in parallel trees, both appended
	/// "last", and both landed on the same position. Nothing in a compiler can see that, and the
	/// symptom is not a crash — every patch loads one block's bytes as the other's, silently.
	///
	/// So the order is DATA now, and both functions walk it. A new block is one entry appended here,
	/// and there is nowhere else to append it: a writer and a reader that disagree about the order is
	/// no longer a thing this file can express.
	///
	/// Each entry owns BOTH halves of its own block, including what absence means — which is the other
	/// thing that was easy to get wrong when the two directions were written apart. A block that is not
	/// in the stream is not an error: the patch predates it, and every loader below restores the value
	/// that reproduces such a patch exactly.
	///
	/// **A LOAD PARSES; IT DOES NOT APPLY.** Each `load` reads its block into a local and hands back, in
	/// `commit`, the one call that makes it live — run by `loadWalk` only once EVERY block has parsed. It
	/// used to apply as it read, after the parameters had already been committed, so a stream refused in its
	/// fifth block left new parameters, four new blocks and the rest of the old patch: a mix nobody saved,
	/// reported to the host as a failure it then did nothing about. `present` says whether the block was in
	/// the stream at all (see `loadWalk` for why its absence is now a refusal).
	using Commit = std::function<void()>;
	struct StateBlock {
		const char* name;
		std::function<bool(const clap_ostream*)> save;
		std::function<bool(const clap_istream*, Commit*, bool*)> load;
		/// The commit for a stream that PREDATES this block (`kStateBlockSince`): what absence meant when the stream was
		/// written. Only a block added since the oldest version this build reads needs one.
		std::function<void(Commit*)> absent = [](Commit* commit) { *commit = [] {}; };
	};

	/// The slot's LIVE explore box (V20) — what the active stance mirrors, read from the same
	/// state the blocks above save. The banking-at-save and the absent-member fill both build from
	/// this, so the two cannot disagree about what "live" means.
	RideAlongValues liveRideAlongFor(size_t slot) {
		RideAlongValues values { params_.base(static_cast<clap_id>(soundParam(slot, SoundField::Width))),
			     params_.base(static_cast<clap_id>(soundParam(slot, SoundField::Zone))),
			     params_.base(static_cast<clap_id>(soundParam(slot, SoundField::Offset))),
			     bridge_.soundGamma().base[slot],
			     bridge_.soundTravel().offset[slot].x,
			     bridge_.soundTravel().offset[slot].y,
			     bridge_.soundOrientations().pin[slot],
			     bridge_.soundResidue().banked[slot],
			     bridge_.soundTravel().orbit[slot].radius,
			     bridge_.soundTravel().orbit[slot].theta };
		// AND THE SOUND'S KNOBS AND DRAWER (2026-10-10), the one list a clone copies (perSoundKnobParam).
		for (size_t k = 0; k < kPerSoundKnobCount; ++k) {
			values.knobs[k] = params_.base(static_cast<clap_id>(perSoundKnobParam(slot, k)));
		}
		values.curve = bridge_.spectralCurve(slot);
		return values;
	}

	std::array<StateBlock, kStateBlockOrder.size()> stateBlocks() {
		// NAMES FROM THE CANONICAL ORDER, positionally — state_block_order.h is the one place the
		// sequence is written, and drawing the names from it means this table cannot change size or
		// spelling without the header (and the case pinning it) saying so out loud.
		return { {
		    { kStateBlockOrder[0], // orientation
		      [this](const clap_ostream* out) { return saveOrientation(out, bridge_.orientation()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SliceOrientationPin pin;
			      if (!loadOrientation(in, &pin, present)) {
				      return false;
			      }
			      // A patch that names no orientation is simply unpinned, and every sound derives its
			      // plane from its own home.
			      *commit = [this, pin = *present ? pin : SliceOrientationPin {}] { bridge_.restoreOrientation(pin); };
			      return true;
		      } },
		    { kStateBlockOrder[1], // drift
		      [this](const clap_ostream* out) { return saveSoundTravel(out, bridge_.soundTravel()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SoundTravel travel;
			      if (!loadSoundTravel(in, &travel, present)) {
				      return false;
			      }
			      // A patch that carries none has never drifted, and zero offsets reproduce it exactly.
			      *commit = [this, travel = *present ? travel : SoundTravel {}] { bridge_.setSoundTravel(travel); };
			      return true;
		      } },
		    { kStateBlockOrder[2], // planes
		      [this](const clap_ostream* out) {
			      return saveSoundOrientations(out, bridge_.soundOrientations());
		      },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SoundOrientations planes;
			      if (!loadSoundOrientations(in, &planes, present)) {
				      return false;
			      }
			      // Carrying none means every sound derives from its home — the engine as it always was.
			      *commit = [this, planes = *present ? planes : SoundOrientations {}] {
				      bridge_.setSoundOrientations(planes);
			      };
			      return true;
		      } },
		    { kStateBlockOrder[3], // residue
		      // Writes nothing at all unless something was banked — see saveSoundResidue. A patch that
		      // never turned a drifted sound is byte-identical to the one it would have been before
		      // the residue existed.
		      [this](const clap_ostream* out) { return saveSoundResidue(out, bridge_.soundResidue()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SoundResidue residue;
			      if (!loadSoundResidue(in, &residue, present)) {
				      return false;
			      }
			      // Absent means empty, which the decode adds exactly nothing of.
			      *commit = [this, residue = *present ? residue : SoundResidue {}] { bridge_.setSoundResidue(residue); };
			      return true;
		      } },
		    { kStateBlockOrder[4], // gamma
		      [this](const clap_ostream* out) { return saveSoundGamma(out, bridge_.soundGamma()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SoundGamma gamma;
			      if (!loadSoundGamma(in, &gamma, present)) {
				      return false;
			      }
			      // Absent is a patch from before the vernier, whose zero base is what reproduces it.
			      // `restore` rather than `set`, so opening a patch does not report it as modified.
			      *commit = [this, gamma = *present ? gamma : SoundGamma {}] { bridge_.restoreSoundGamma(gamma); };
			      return true;
		      } },
		    { kStateBlockOrder[5], // variations
		      // Written even when nothing is stored — see saveSoundVariations for why this block does
		      // not do the residue's conditional trick.
		      [this](const clap_ostream* out) {
			      // BANKED AT THE MOMENT OF SAVING — V19: while a stance is active its stored entry
			      // is dead and the live state IS the record, so a file that wrote the stale entry
			      // would reopen with the active stance disagreeing with the sound. The save banks
			      // each non-empty slot's active entry from the FULL explore box it mirrors (V20),
			      // exactly as switching away would.
			      SoundVariations banked = bridge_.soundVariations();
			      for (size_t slot = 0; slot < kMaxSounds; ++slot) {
				      SoundVariations::List& list = banked.sound[slot];
				      if (list.empty()) {
					      continue;
				      }
				      list.values[list.active] = liveRideAlongFor(slot);
			      }
			      // The dead x,y slots carry each slot's live place — see saveSoundVariations.
			      std::array<AnchorPoint, kMaxSounds> livePlaces {};
			      for (size_t slot = 0; slot < kMaxSounds; ++slot) {
				      livePlaces[slot] = {
					      params_.base(static_cast<clap_id>(soundParam(slot, SoundField::PlaceX))),
					      params_.base(static_cast<clap_id>(soundParam(slot, SoundField::PlaceY)))
				      };
			      }
			      return saveSoundVariations(out, banked, livePlaces);
		      },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      auto variations = std::make_shared<SoundVariations>();
			      size_t valuesRead = 0;
			      if (!loadSoundVariations(in, variations.get(), present, &valuesRead)) {
				      return false;
			      }
			      // Members a patch's stances predate get the migration fill: absent means the
			      // slot's LIVE value — off the state the stream loaded just before this block — so
			      // recalling an old stance is a no-op on the axes it never carried. Member-granular
			      // (fillAbsentStanceValues): a width-only FSV2 file keeps its widths, an FSV1 file
			      // fills everything.
			      //
			      // **SO THE FILL IS PART OF THE COMMIT, NOT THE PARSE**: "live" means the parameters and
			      // blocks this patch carries, and those are live only once the commits before this one
			      // have run — which `loadWalk` runs in this table's order.
			      *commit = [this, variations, present = *present, valuesRead] {
				      if (present && valuesRead < kRideAlongWireValues) {
					      std::array<RideAlongValues, kMaxSounds> live {};
					      for (size_t slot = 0; slot < kMaxSounds; ++slot) {
						      live[slot] = liveRideAlongFor(slot);
						      // BUT THE CURVE IS NOT LIVE YET: the `spectral` block commits AFTER this one, so the bridge still holds the
						      // previous patch's curves. A record short of the curve members was written before stances carried a curve
						      // (this build always writes them; a hand-built or foreign stream may not), and such a stream's sounds are
						      // meant to sound as they did then: with the flat curve. So that is the live value the fill means.
						      live[slot].curve = spectral::DrawnCurve {};
					      }
					      fillAbsentStanceValues(*variations, live, valuesRead);
				      }
				      // Absent is a patch from before the second level of the tree, whose sounds have
				      // exactly the placement their parameters hold — which is what empty lists mean here too.
				      bridge_.restoreSoundVariations(present ? *variations : SoundVariations {});
			      };
			      return true;
		      } },
		    { kStateBlockOrder[6], // hues
		      [this](const clap_ostream* out) { return saveSoundHues(out, bridge_.soundHues()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SoundHues hues;
			      if (!loadSoundHues(in, &hues, present)) {
				      return false;
			      }
			      // A patch from before colours could travel wears the IDENTITY — slot 0 the first
			      // colour, slot 1 the second — which is exactly what it drew, because a colour used
			      // to be its slot's index. So such a patch opens looking the way it looked when it
			      // was saved, which is the whole argument for appending this block at all.
			      // `restore` rather than `set`, so opening a patch does not report it as modified.
			      *commit = [this, hues = *present ? hues : SoundHues {}] { bridge_.restoreSoundHues(hues); };
			      return true;
		      } },
		    { kStateBlockOrder[7], // chains
		      // **BOTH CHAINS AND EVERY ROUTE IN THE RACK'S OWN FORMAT** (chain-as-a-device item 6). This
		      // was three blocks — the lane, the strip, the routings — with a device record of their own;
		      // the rack had a second. One format now, applied to one record.
		      // THE HOST SLOTS RIDE HERE, in the project's patch and never a preset's (plan §0.8, §2).
		      // THE RECORD CATCHES UP FIRST with what its devices changed themselves (backlog MX: a Turing machine's rewrites).
		      [this](const clap_ostream* out) {
			      bridge_.harvestExtraState();
			      return saveChainsBlock(out, bridge_.chainsPatch(!presetRoad_));
		      },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      auto patch = std::make_shared<rack::RackPatch>();
			      if (!loadChainsBlock(in, patch.get(), present, &chainsParseReport_)) {
				      return false;
			      }
			      if (!*present) {
				      *patch = rack::RackPatch {};
			      }
			      // ONE SIDE ONLY — THE BRIDGE — and what makes that correct: every restore PUBLISHES the
			      // bridge's record, and `publishEditorLanes` has the synth adopt the published chains
			      // whenever they moved (`Synth::adoptChains`, chain-as-a-device 5b C4), from BOTH entry
			      // points, `paramsFlush` and `process` — never overlapping a block. So what the bridge
			      // holds reaches the engine on the next block either way, and the devices it names are
			      // built by the provisioning visit below `stateLoad`'s block walk.
			      //
			      // **A SECOND WRITE HERE WOULD BE A DATA RACE**, and each of the three old blocks carried
			      // one at some point: `state.load` is `[main-thread]`, NOT `[main-thread & !active]`, and
			      // our VST3 road forwards `setState` into `load` with no deactivate — so a host restoring
			      // a preset with the transport running would land it against `render()` (backlog CD
			      // commit 3's bug; `rack_clap.cpp` names the mechanism).
			      //
			      // WHAT THE RULES DROPPED, handed over rather than discarded — latched on the bridge
			      // because an editor may not exist yet: a host restoring a project loads state long
			      // before anybody opens the window. A patch from before this block puts the chains a
			      // fresh instance has (`restoreChains`).
			      //
			      // AND THE HOST SLOTS: a project load (the host's `state.load`) restores the patch's pool; a
			      // preset load (`guiLoadState`) keeps the project's and frees what no longer resolves.
			      *commit = [this, patch] {
				      chainsRestoreReport_ = bridge_.restoreChains(*patch, presetRoad_ ? ParamBridge::SlotsOnLoad::Keep
				                                                                       : ParamBridge::SlotsOnLoad::Restore);
				      bridge_.noteLoadReport(chainsRestoreReport_);
			      };
			      return true;
		      } },
		    { kStateBlockOrder[8], // macros
		      // WHICH WAY EACH STRUCTURAL MACRO SWINGS (backlog EG). A patch from before the block
		      // reads as every macro unipolar, which is what it was; `restore` rather than `set`, so
		      // opening a patch does not report it as modified. The engine hears it at the next
		      // `publishEditorLanes`, like every other thing the bridge holds.
		      [this](const clap_ostream* out) { return saveMacroModes(out, bridge_.macroModes()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      MacroModes modes;
			      if (!loadMacroModes(in, &modes, present)) {
				      return false;
			      }
			      *commit = [this, modes = *present ? modes : MacroModes {}] { bridge_.restoreMacroModes(modes); };
			      return true;
		      } },
		    { kStateBlockOrder[9], // spectral
		      // THE SPECTRAL DRAWER'S MASTER SWITCH AND DRAWN CURVE (additive design §3.1), keyed. `restore`, not an edit.
		      // The engine hears it on process()'s schedule (`Synth::setSpectralState`), as it hears the gammas.
		      // OE: AND THE ENGINE VOCODER'S HELD ENVELOPE — the synth's snapshot (written on the audio thread whenever it changes), or, if
		      // a load has not reached the audio thread yet, the words that load carried (additive design §9.3).
		      [this](const clap_ostream* out) {
			      const SpectralState state = bridge_.spectralState();
			      std::array<float, kVocodeFreezeFloats> words {};
			      VocodeFreezeBlock check;
			      const float* freeze = nullptr;
			      if (synth_.vocodeFreezeLoadsTaken() == state.vocodeFreezeLoads) {
				      freeze = synth_.readVocodeFreeze(words.data(), words.size()) != 0 ? words.data() : nullptr;
			      }
			      else if (readVocodeFreezeBlock(state.vocodeFreeze.data(), state.vocodeFreeze.size(), &check)) {
				      freeze = state.vocodeFreeze.data();
			      }
			      return saveSpectralState(out, state, freeze);
		      },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      SpectralState state;
			      if (!loadSpectralState(in, &state, present)) {
				      return false;
			      }
			      // EVERY LOAD COUNTS (OE): the synth primes from it, or drops a held envelope the patch does not carry.
			      *commit = [this, state = *present ? state : SpectralState {}]() mutable {
				      state.vocodeFreezeLoads = bridge_.spectralState().vocodeFreezeLoads + 1;
				      bridge_.restoreSpectralState(state);
			      };
			      return true;
		      },
		      // A 50 OR 49 STREAM PREDATES THE DRAWER: the drawer on and the curve flat, which is the engine as it was.
		      [this](Commit* commit) {
			      *commit = [this] {
				      SpectralState state;
				      state.vocodeFreezeLoads = bridge_.spectralState().vocodeFreezeLoads + 1;
				      bridge_.restoreSpectralState(state);
			      };
		      } },
		    { kStateBlockOrder[10], // likes
		      // THE TERRAIN'S LIKES (backlog ON), keyed: each like's place and the partial envelope it was made of. Editor
		      // state on the main thread only — nothing that plays reads it. `restore`, not an edit.
		      [this](const clap_ostream* out) { return saveTerrainLikes(out, bridge_.terrainLikes()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      TerrainLikes likes;
			      if (!loadTerrainLikes(in, &likes, present)) {
				      return false;
			      }
			      *commit = [this, likes = *present ? likes : TerrainLikes {}] { bridge_.restoreTerrainLikes(likes); };
			      return true;
		      },
		      // A STREAM FROM BEFORE THE LIKES holds none.
		      [this](Commit* commit) { *commit = [this] { bridge_.restoreTerrainLikes(TerrainLikes {}); }; } },
		    { kStateBlockOrder[11], // preset
		      // WHICH PRESET THE PATCH CAME FROM (backlog QC),
		      // keyed. **NOT ON THE PRESET ROAD, EITHER WAY**: a preset file names no preset (the browser names the one it
		      // picked, after the load), so its capture writes none and its load leaves the name to the browser.
		      [this](const clap_ostream* out) {
			      return savePresetIdentity(out, presetRoad_ ? PresetIdentity {} : bridge_.presetIdentity());
		      },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      PresetIdentity identity;
			      if (!loadPresetIdentity(in, &identity, present)) {
				      return false;
			      }
			      if (presetRoad_) {
				      *commit = [] {};
				      return true;
			      }
			      *commit = [this, identity = *present ? identity : PresetIdentity {}] {
				      bridge_.restorePresetIdentity(identity);
			      };
			      return true;
		      },
		      // A STREAM FROM BEFORE THE NAME names no preset — except on the preset road, which leaves it to the browser.
		      [this](Commit* commit) {
			      if (presetRoad_) {
				      *commit = [] {};
				      return;
			      }
			      *commit = [this] { bridge_.restorePresetIdentity(PresetIdentity {}); };
		      } },
		    { kStateBlockOrder[12], // drumlanes (after the preset: kStateVersion 54)
		      // THE KIT PADS' FX LANES (drum proposal §17 P4), keyed by pad: each lane's own body (`InnerChain::saveState`).
		      // A pad with no lane, or an empty one, is not written. `restore`: a lane the patch names takes its body live
		      // (`applyState`, landing at the lane's next drain); a lane it does not name is emptied.
		      [this](const clap_ostream* out) { return saveDrumLanes(out, drumLanesNow()); },
		      [this](const clap_istream* in, Commit* commit, bool* present) {
			      DrumLanes lanes;
			      if (!loadDrumLanes(in, &lanes, present)) {
				      return false;
			      }
			      *commit = [this, lanes = *present ? std::move(lanes) : DrumLanes {}] { restoreDrumLanes(lanes); };
			      return true;
		      },
		      // A STREAM FROM BEFORE THE PAD LANES holds none: every pad's lane empty.
		      [this](Commit* commit) { *commit = [this] { restoreDrumLanes(DrumLanes {}); }; } },
		} };
	}

	/// THE KIT PADS' FX LANES AS BODIES (drum proposal §17 P4), MAIN THREAD: each lane that holds anything, saved through its
	/// own record (`InnerChain::saveState`); a pad with no lane or an empty one, nothing.
	DrumLanes drumLanesNow() const {
		DrumLanes lanes;
		for (size_t pad = 0; pad < lanes.size(); ++pad) {
			const rack::InnerChain* lane = synth_.padLane(pad);
			if (lane == nullptr || lane->bridge() == nullptr || lane->bridge()->chainLength(0) == 0) {
				continue;
			}
			std::vector<uint8_t>& body = lanes[pad];
			const device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
				                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
				                             out->insert(out->end(), data, data + bytes);
			                             } };
			if (!lane->saveState(sink)) {
				body.clear(); // a lane that will not serialise is left out rather than written in part
			}
		}
		return lanes;
	}
	/// AND BACK, MAIN THREAD: a body the patch carries is applied live to its pad's lane (built if it has none); a pad it does
	/// not name has its lane emptied, so a load never leaves a previous patch's pad fx behind.
	void restoreDrumLanes(const DrumLanes& lanes) {
		for (size_t pad = 0; pad < lanes.size(); ++pad) {
			const std::vector<uint8_t>& body = lanes[pad];
			if (!body.empty()) {
				if (rack::InnerChain* lane = synth_.ensurePadLane(pad); lane != nullptr && !lane->applyState(body.data(), body.size())) {
					log_.say("drumlanes pad " + std::string(1, static_cast<char>('A' + pad)) + " REFUSED its body");
				}
			}
			else if (rack::InnerChain* lane = synth_.padLane(pad); lane != nullptr) {
				(void)lane->applyState(nullptr, 0);
			}
		}
	}

	bool stateSave(const clap_ostream* stream) noexcept override {
		// THE HOST SLOTS ARE SERVICED FIRST (plan §1.3), so the pool the project saves is the reconciled one
		// — never a slot naming a device that stopped resolving since the last main-thread visit.
		serviceSlots();
		return saveWalk(stream, "state-save");
	}

	/// **THE PARAMETERS, THEN EVERY TAGGED BLOCK, IN THE ONE ORDER BOTH DIRECTIONS WALK** — for the host's save
	/// and the preset browser's capture alike, and logged either way. A failed save is the quieter half of
	/// "it forgot my patch": the host is told false, keeps no state, and the project reopens at defaults.
	bool saveWalk(const clap_ostream* stream, const char* road) {
		if (stream == nullptr) {
			log_.say(std::string(road) + " FAILED stage=stream why=no-stream");
			return false;
		}
		// **THE WHOLE STREAM OR NOTHING**. Serialised into one
		// buffer first — main thread, so the allocation is fine — and handed to the host only once it is complete
		// and sealed with its end marker. A part that refuses to serialise writes ZERO bytes to the host, where it
		// used to leave the parts before it there under a `false`.
		std::vector<uint8_t> bytes;
		bytes.reserve(16384);
		MemOStreamForState mem(&bytes);
		const auto refused = [&](const char* stage) {
			log_.say(std::string(road) + " FAILED stage=" + stage + " bytes=0 serialised=" + std::to_string(bytes.size())
			         + " why=unencodable; nothing written");
			return false;
		};
		if (!params_.save(&mem.stream)) {
			return refused("params");
		}
		for (const StateBlock& block : stateBlocks()) {
			if (!block.save(&mem.stream)) {
				return refused(block.name);
			}
		}
		const size_t payload = bytes.size();
		state::appendTrailer(bytes);
		uint64_t written = 0;
		if (!state::writeWhole(stream, bytes, &written)) {
			// THE HOST'S STREAM TOOK PART OF IT: what it keeps is the host's business, and it is truncated — which
			// the end marker is there to refuse on the way back in.
			log_.say(std::string(road) + " FAILED stage=host-stream bytes=" + std::to_string(written) + " of "
			         + std::to_string(bytes.size()) + " why=host-short-write");
			return false;
		}
		log_.say(std::string(road) + " ok bytes=" + std::to_string(bytes.size()) + " payload=" + std::to_string(payload)
		         + " version=" + std::to_string(kStateVersion) + " crc=" + hex32(state::crc32(std::span(bytes).first(payload))));
		return true;
	}

	/// **THE WHOLE PATCH PARSED, THEN THE WHOLE PATCH APPLIED — OR NOTHING.** For the host's `state.load` and
	/// the preset browser's alike.
	///
	/// THE HAZARD THIS CLOSES: the walk used to commit the parameters (`ParamSet::load`) and then apply each
	/// block as it read it, so a stream refused in a later block left the instance holding the new
	/// parameters, the blocks before the refusal, and the OLD rest — a patch nobody saved, under a `false`
	/// the host does nothing with (Bitwig keeps the instance as it is). Now the host's bytes are read whole,
	/// every part is parsed into a local, and only when the last block has parsed is any of it made live.
	///
	/// AND THE STREAM MUST VOUCH FOR ITS OWN END (kStateVersion 50, `state/state_trailer.h`). Each block's loader
	/// reads a stream that ends where it would start as "a patch from before this block" and puts the default —
	/// for `chains`, a FRESH INSTANCE'S chains — so a stream cut at a block boundary used to load as the patch
	/// with every later block reset: the "forgot its devices" shape. A 50 stream is refused unless its end
	/// marker's length and CRC match; a 49 stream, which has none, is refused unless EVERY block is present (every
	/// 49 writer wrote all of them). The "patch from before this block" reading is therefore gone for both.
	bool loadWalk(const clap_istream* stream, const char* road) {
		std::vector<uint8_t> bytes;
		if (stream == nullptr || !readWholeStream(stream, &bytes)) {
			log_.say(std::string(road) + " FAILED stage=stream bytes=" + std::to_string(bytes.size())
			         + (stream == nullptr ? " why=no-stream" : " why=read-error-or-oversized") + "; nothing changed");
			return false;
		}
		const size_t received = bytes.size();
		// THE END MARKER, FIRST — for a 50 stream. Checked before a byte is parsed, which is before anything is
		// committed; the payload is everything before it.
		const bool trailered = !bytes.empty() && bytes[0] >= kStateVersionTrailered && readableStateVersion(bytes[0]);
		if (trailered) {
			size_t payload = 0;
			if (const state::TrailerCheck check = state::checkTrailer(bytes, &payload); check != state::TrailerCheck::Ok) {
				log_.say(std::string(road) + " FAILED stage=end-marker bytes=" + std::to_string(received)
				         + " version=" + std::to_string(bytes[0]) + " why=" + state::trailerWhy(check)
				         + "; nothing changed");
				return false;
			}
			bytes.resize(payload);
		}
		BufferIStream in(bytes);
		const auto failed = [&](const std::string& stage, const std::string& why) {
			log_.say(std::string(road) + " FAILED stage=" + stage + " at=" + std::to_string(in.at)
			         + " bytes=" + std::to_string(received) + " why=" + why + "; nothing changed");
			return false;
		};

		// THE PARAMETERS, staged on the heap: kParamCount doubles is more stack than a host's thread owes us.
		auto staged = std::make_unique<ParamSet::Staged>();
		ParamSet::ReadFailure why = ParamSet::ReadFailure::None;
		uint8_t version = 0;
		if (!params_.read(&in.stream, staged.get(), &why, &version)) {
			switch (why) {
			case ParamSet::ReadFailure::Empty: return failed("params", "empty-stream");
			case ParamSet::ReadFailure::Version:
				// SAID IN THE WORDS A CI BUILD NEEDS: a downloaded build reads only these versions, so a project saved
				// by another one opens at defaults — and this line is what says that is what happened.
				return failed("params", "version-mismatch: saved by version " + std::to_string(version)
				                            + ", this build reads " + std::to_string(kStateVersionUntrailered) + " to "
				                            + std::to_string(kStateVersion));
			default: return failed("params", "short-read");
			}
		}

		chainsParseReport_ = {};
		chainsRestoreReport_ = {};
		std::array<Commit, kStateBlockOrder.size()> commits;
		const auto blocks = stateBlocks();
		for (size_t i = 0; i < blocks.size(); ++i) {
			// A BLOCK THIS STREAM PREDATES is not looked for: its absent default is what the patch was (kStateBlockSince).
			if (version < kStateBlockSince[i]) {
				blocks[i].absent(&commits[i]);
				continue;
			}
			const size_t start = in.at;
			bool present = false;
			if (!blocks[i].load(&in.stream, &commits[i], &present)) {
				std::string reason = in.hitEnd ? "truncated-inside-block" : "block-refused-its-bytes";
				if (const std::string detail = rack::loadReportDetail(chainsParseReport_); !detail.empty()) {
					reason += " " + detail;
				}
				in.at = start; // the position the block began at, which is what a reader of the log wants
				return failed(blocks[i].name, reason);
			}
			if (!present) {
				// EVERY WRITER OF 49 AND 50 WROTE THIS BLOCK, so a stream without it was cut here.
				return failed(blocks[i].name, version == kStateVersionUntrailered ? "v49-missing-block (truncated)"
				                                                                  : "missing-block");
			}
		}

		// EVERYTHING PARSED. Now, and only now, the patch becomes live — parameters first, then the blocks in
		// the table's order, which the variations fill relies on (see its loader).
		params_.commit(*staged);
		for (const Commit& commit : commits) {
			commit();
		}
		std::string line = std::string(road) + " ok bytes=" + std::to_string(received)
		                   + " version=" + std::to_string(version)
		                   + (trailered ? " end-marker=ok" : " end-marker=none (v49)");
		if (in.at != bytes.size()) {
			line += " unread=" + std::to_string(bytes.size() - in.at);
		}
		rack::LoadReport report = chainsParseReport_;
		report.merge(chainsRestoreReport_);
		if (const std::string detail = rack::loadReportDetail(report); !detail.empty()) {
			line += " chains: " + detail;
		}
		log_.say(line);
		return true;
	}

	static std::string hex32(uint32_t v) {
		char out[9];
		std::snprintf(out, sizeof(out), "%08x", v);
		return out;
	}

	bool stateLoad(const clap_istream* stream) noexcept override {
		// RENAMES ARE RECORDED DURING THE WALK AND SENT ONCE, WITH THE VALUES, below. The rescan is sent
		// from inside this call, as the values' always was: `state.load` and `params.rescan` are both
		// `[main-thread]`, and neither CLAP header asks for it to wait.
		const StateLoadScope loading(*this);
		if (!loadWalk(stream, "state-load")) {
			return false;
		}

		// AND THE DEVICES THE PATCH NAMES ARE BUILT BEFORE ANY OF IT CAN BE RENDERED. A chain arriving
		// here may hold devices the engine has never built, and building one allocates — which the
		// audio thread may not do. This is the MAIN thread (clap_plugin_state.load is main-thread) and
		// it is the only main-thread visit a patch load is guaranteed: a host restoring a project
		// commonly loads state with no editor open, so `pumpEditor`'s provisioning is not running.
		//
		// THE DRAIN IS THE OTHER HALF. A patch that replaces the lane retires whatever was in it on
		// the next block; without a drain the comb's delay line would be parked and never freed.
		readingsHops_.grow(bridge_.chainState().capacity()); // a hop for every position the lane can reach
		synth_.provisionDevices(bridge_.chainState(), bridge_.publishedChains().sequence(), &bridge_.fxBodies());
		bridge_.noteRefusedRoutes(synth_.takeOuterDrops()); // what a chain dropped inside, said by the tray
		synth_.drainRetiredDevices();

		// THE HOST SLOTS THE PATCH RESTORED ARE RENAMED IN THE POOL; recorded here (the load scope is open)
		// and paid in the one rescan below.
		serviceSlots();

		// Every value just moved underneath the host's cached copy, and underneath the editor's — and a
		// macro's name, if the patch renamed one, rides the same call.
		rescanParams(CLAP_PARAM_RESCAN_VALUES);
		refreshTail(); // the patch's lane, not the one it replaced
		if (_host.canUseParams()) {
			// The editor is fed from a snapshot taken in process() or flush(), and a preset loaded onto
			// a stopped transport reaches neither on its own. Asking for a flush is what makes the
			// editor show the patch that was just loaded rather than the one it replaced.
			_host.paramsRequestFlush();
		}
		return true;
	}

	//-----//
	// GUI //
	//-----//

	// Only claimed when an editor backend was built, so a host on a platform this does not cover yet
	// sees a plugin with no editor rather than one whose editor never opens. Either way the synth
	// plays: nothing below this line is on the path from a note to a sample.
	bool implementsGui() const noexcept override { return Gui::isAvailable(); }

	bool guiIsApiSupported(const char* api, bool isFloating) noexcept override {
		return Gui::isApiSupported(api, isFloating);
	}

	bool guiGetPreferredApi(const char** api, bool* isFloating) noexcept override {
		// THE API THIS PLATFORM'S EDITOR IS EMBEDDED WITH — the same answer is_api_supported gives,
		// so a host following the documented flow (get_preferred_api → is_api_supported → create)
		// lands on a window it can actually provide. This answered X11 unconditionally: written
		// when X11 was the only backend, harmless on Linux, and on macOS it offered the host an
		// API it has no window for — so the CLAP editor never opened at all while the VST3, whose
		// wrapper hardcodes Cocoa on MAC without ever asking, opened fine.
#if defined(__APPLE__)
		*api = CLAP_WINDOW_API_COCOA;
#else
		*api = CLAP_WINDOW_API_X11;
#endif
		*isFloating = false;
		return true;
	}

	bool guiCreate(const char* api, bool isFloating) noexcept override {
		// The X11 editor pumps EVERYTHING — paint and mouse both — out of main-thread time the host
		// hands back through one of these two extensions, so on Linux a host offering neither really
		// cannot show it. macOS is not that: AppKit delivers both on its own (the MTKView's display
		// link paints, the responder chain answers the mouse), and the extensions only decide how
		// promptly parameter changes reach the editor. Gating there refused the editor to macOS
		// hosts that could have shown it perfectly well.
#if defined(__linux__)
		if (!_host.canUsePosixFdSupport() && !_host.canUseTimerSupport()) {
			log_.say("gui-create REFUSED why=host-offers-neither-posix-fd-nor-timer");
			return false;
		}
#endif
 		if (!gui_.create(api, isFloating)) {
			log_.say(std::string("gui-create FAILED api=") + (api != nullptr ? api : "?"));
			return false;
		}
		// A newly built bridge holds the parameter defaults, which are not the patch. One flush fills it
		// with the real thing before the first frame is drawn.
		guiRequestFlush();
		log_.say(std::string("gui-create api=") + (api != nullptr ? api : "?") + (isFloating ? " floating" : " embedded"));
		return true;
	}

	void guiDestroy() noexcept override {
		log_.say("gui-destroy");
		unregisterGuiCallbacks();
		// The preview is cleared by Gui::destroy itself — one door, and it is the editor's own state.
		gui_.destroy();
		// WHAT THE DETACH WAS ANSWERED (backlog GQ). An error here is usually our window already gone with a container
		// the host destroyed first (backlog GP), which the next set_parent rebuilds.
		if (gui_.lastXReport().errors > 0) {
			log_.say("gui-destroy detach " + ui::PluginWindow::describe(gui_.lastXReport()));
		}
	}

	// Registration waits for the parent because there is no X11 connection to register, and no frames
	// to ask for, until the window has somewhere to be.
	bool guiSetParent(const clap_window* window) noexcept override {
		const int rebuildsBefore = gui_.windowRebuilds();
		if (!gui_.setParent(window)) {
			log_.say("gui-set-parent FAILED " + ui::PluginWindow::describe(gui_.lastXReport()));
			return false;
		}
		if (gui_.windowRebuilds() != rebuildsBefore) {
			log_.say("gui-set-parent REBUILT why=window-destroyed-with-container "
			         + ui::PluginWindow::describe(gui_.lastXReport()));
		}
		unregisterGuiCallbacks();

		if (const int fd = gui_.posixFd(); fd >= 0 && _host.canUsePosixFdSupport()) {
			if (_host.posixFdSupportRegister(fd, CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE |
			                                         CLAP_POSIX_FD_ERROR)) {
				guiFd_ = fd;
				return true;
			}
		}
		// Fallback: poll the same connection off a timer. Slightly coarser and no worse — visage's own
		// frame timer still decides when a draw is due, this only decides how promptly we notice.
		if (_host.canUseTimerSupport() && _host.timerSupportRegister(Gui::kTimerPeriodMs, &guiTimer_)) {
			return true;
		}
		// THE EMBED ALREADY SUCCEEDED. On X11 a host offering neither extension cannot pump events,
		// and that is refused at create() rather than here. On macOS AppKit paints and answers the
		// mouse itself — returning false after a successful set_parent is how the CLAP editor stayed
		// invisible while the VST3, whose wrapper ignores this return, drew (undersized). The timer
		// is how promptly parameters arrive, not whether the window exists.
		return true;
	}

	// The CLAP sequence ends with show(), and clap-helpers' default answer to it is a flat false. No
	// host we have met calls it — Bitwig 5.1.5 never does — but a host that follows the documented
	// order would have been told the editor refused to appear, which is the same silent nothing that
	// cost us this bug once already. An embedded visage window is on screen from set_parent onward,
	// so the truthful answer is simply whether there is a window.
	bool guiShow() noexcept override { return gui_.hasWindow(); }

	// Nothing to undo: hiding an embedded editor is the host unmapping the container it owns. Saying
	// so is better than a false that reads as failure.
	bool guiHide() noexcept override { return gui_.hasWindow(); }

	bool guiSetScale(double scale) noexcept override { return gui_.setScale(scale); }

	// Without this, clap-helpers' default answers every question with the size we already are
	// (plugin.hh:309, `return guiGetSize(...)`), whatever was asked — which told every host that
	// dragged a window edge to put it back where it started, and is half of why the window would not
	// settle. adjust_size is the authority in the resize conversation; clap-helpers even checks
	// set_size against it. So it has to answer about the size it was ASKED about.
	bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override {
		return gui_.adjustSize(width, height);
	}

	bool guiSetSize(uint32_t width, uint32_t height) noexcept override {
		return gui_.setSize(width, height);
	}
	bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override {
		return gui_.getSize(width, height);
	}

	// Resizable, with no constraint of our own: the editor is a layout, not a fixed picture, so
	// whatever the host offers is a size it can be drawn at.
	bool guiCanResize() const noexcept override { return true; }

	//---------------------//
	// Main-thread callins //
	//---------------------//

	bool implementsPosixFdSupport() const noexcept override { return true; }

	void onPosixFd(int fd, clap_posix_fd_flags_t) noexcept override {
		if (fd == guiFd_) {
			pumpEditor();
		}
	}

	bool implementsTimerSupport() const noexcept override { return true; }

	void onTimer(clap_id timerId) noexcept override {
		if (timerId == guiTimer_) {
			pumpEditor();
		}
#ifdef FEATHERS_DIAGNOSTIC
		if (timerId == spikeTimer_) {
			drainSpikes();
		}
#endif
	}

#ifdef FEATHERS_DIAGNOSTIC
	//------------------------------------------------------------------------------------------//
	// THE CPU SPIKE RECORDER (backlog SR, plugin/spike_log.h) — FEATHERS_DIAGNOSTIC builds only. //
	//------------------------------------------------------------------------------------------//

	/// MAIN THREAD, from activate: the ring, the threshold (editor.conf `spike_threshold`, then `$FEATHERS_SPIKE_THRESHOLD`),
	/// and the one-second drain timer. The devices and stages are timed into the recorder's own tree, bound per block.
	void activateSpikes(double sampleRate) noexcept {
		try {
			spikes_.activate(sampleRate, spike::thresholdFromPrefsAndEnvironment());
		}
		catch (...) {
		}
		spikeFirst_ = true;
		if (spikeTimer_ == CLAP_INVALID_ID && _host.canUseTimerSupport()) {
			(void)_host.timerSupportRegister(1000, &spikeTimer_);
		}
	}

	/// MAIN THREAD: the ring into `<date>.spikes.jsonl` beside the likes log, and the minute's summary.
	void drainSpikes() noexcept {
		try {
			std::vector<std::string> lines;
			const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
			spikes_.drain("feathers", now, &lines);
			(void)spike::append(lines, now);
		}
		catch (...) {
		}
	}

	/// ONE BLOCK, TIMED FROM ENTRY TO EVERY RETURN. What it flags is read as counters before and after.
	struct SpikeBlock {
		FeathersPlugin& plugin;
		const clap_process* process;
		spike::BlockStart start;
		uint64_t tables;
		bool restart;
		SpikeBlock(FeathersPlugin& p, const clap_process* proc) noexcept
		    : plugin(p), process(proc), start(p.spikes_.beginBlock()), tables(p.synth_.bankTablesMade()),
		      restart(p.restartAsked_) {}
		~SpikeBlock() { plugin.endSpike(*this); }
		SpikeBlock(const SpikeBlock&) = delete;
		SpikeBlock& operator=(const SpikeBlock&) = delete;
	};

	void endSpike(const SpikeBlock& b) noexcept {
		spike::BlockFacts facts;
		facts.frames = b.process != nullptr ? b.process->frames_count : 0;
		facts.voices = static_cast<uint16_t>(std::max(0, synth_.soundingVoiceCount()));
		facts.songBeats = std::numeric_limits<double>::quiet_NaN();
		facts.songSeconds = std::numeric_limits<double>::quiet_NaN();
		bool playing = false;
		if (b.process != nullptr && b.process->transport != nullptr) {
			const clap_event_transport& t = *b.process->transport;
			playing = (t.flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
			if ((t.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0) {
				facts.songBeats = static_cast<double>(t.song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR);
			}
			if ((t.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0) {
				facts.songSeconds = static_cast<double>(t.song_pos_seconds) / static_cast<double>(CLAP_SECTIME_FACTOR);
			}
		}
		const clap_input_events* in = b.process != nullptr ? b.process->in_events : nullptr;
		if (in != nullptr && in->size(in) > 0) {
			facts.flags |= spike::kFlagNotes;
		}
		if (synth_.bankTablesMade() != b.tables) {
			facts.flags |= spike::kFlagRebake;
		}
		if (synth_.likeWarpSerial() != spikeLikeSerial_) {
			facts.flags |= spike::kFlagLikesFit;
			spikeLikeSerial_ = synth_.likeWarpSerial();
		}
		if (restartAsked_ && !b.restart) {
			facts.flags |= spike::kFlagLatency;
		}
		if (playing != spikePlaying_) {
			facts.flags |= spike::kFlagTransport;
			spikePlaying_ = playing;
		}
		if (spikeFirst_) {
			facts.flags |= spike::kFlagFirst;
			spikeFirst_ = false;
		}
		spikes_.endBlock(b.start, facts);
	}

	spike::Recorder spikes_;
	clap_id spikeTimer_ = CLAP_INVALID_ID;
	uint64_t spikeLikeSerial_ = 0;
	bool spikePlaying_ = false;
	bool spikeFirst_ = true;
#endif

	/// The editor's slice of main-thread time, WHICHEVER CALLBACK BROUGHT IT.
	///
	/// There are two, and only one of them runs: guiSetParent registers the posix fd and returns, so
	/// the timer is a fallback for hosts without fd support. Anything that has to happen once per
	/// main-thread visit therefore has to live HERE rather than in either callback — the drain below
	/// was written into onTimer alone, which on Linux is the path that never runs, so a drifted sound
	/// went unsaved on exactly the host it was built for.
	void pumpEditor() noexcept {
		gui_.processEvents();
		// On Linux the fd pump above dispatches the Heartbeat, which has ALREADY run this once through
		// guiTick; running it again here is idempotent (provision builds only what is missing) and
		// keeps the fd road exactly what it was.
		provisionAndDrain();
	}

	/// **THE EDITOR'S OWN BEAT REACHES THE SYNTH — on every platform, not only on the ones whose host
	/// calls back.** DE, the first Mac build (2026-09-07): no effect worked besides the default filter
	/// (even another filter added later). This work lived in `pumpEditor` alone, which runs
	/// from `onPosixFd` (the X11 fd) and `onTimer` (the CLAP timer fallback) — and on the Mac build neither
	/// ever ran, while the editor ticked on by visage's own Heartbeat and drew every added card live.
	/// The Heartbeat fires on the message thread on every backend (AppKit's main-loop timer on
	/// macOS, the fd pump's dispatch on X11, the UI thread's WM_TIMER on win32), which is the thread
	/// this work assumes: provisioning ALLOCATES and the drain FREES, neither of which the audio
	/// thread may do. See GuiHost::guiTick; reproduced on Linux in test_clap_render.
	void guiTick() noexcept override { provisionAndDrain(); }

	void provisionAndDrain() noexcept {
		// THE DEVICES THE GESTURE THAT JUST RAN ASKED FOR. The editor's add, remove and reorder land in
		// the bridge's chain on the message thread; building here catches the new device in the SAME
		// visit that created it, and the slot is filled before the next block is rendered.
		// `FxRunner::sync` adopts; it cannot build, because building allocates and it runs on the
		// audio thread.
		//
		// AND THE DRAIN IS HERE FOR THE SAME REASON THE MARK-DIRTY BELOW IS: this is the once-per-
		// main-thread-visit place, whichever road brought us. An instance the audio thread dropped is
		// parked, not destroyed; this is where it is actually freed, which is what makes a device
		// REMOVED from the lane give its memory back rather than merely stop sounding.
		readingsHops_.grow(bridge_.chainState().capacity()); // a hop for every position the lane can reach
		synth_.provisionDevices(bridge_.chainState(), bridge_.publishedChains().sequence(), &bridge_.fxBodies());
		bridge_.noteRefusedRoutes(synth_.takeOuterDrops()); // what a chain dropped inside, said by the tray
		synth_.drainRetiredDevices();

		// THE PATCH STATE THE HOST CANNOT SEE. Drift offsets and the orientation pin are saved with the
		// patch but are not parameters, so nothing tells the host they moved — a project closed after
		// drifting a sound would be saved without the drift, and the loss would look like the plugin
		// forgetting rather than like the host never being told. Main thread, which is where
		// clap_host_state.mark_dirty has to be called from.
		//
		// AFTER THE HOST SLOTS' VISIT (hs-int x mainline): an editor's remove or undo renamed a slot, and the
		// host hears it once per visit — and a slot the visit NEWLY anchored changed the saved pool, which
		// `serviceSlotRoads` marks in the same flag, so the mark reaches the host in this visit, not the next.
		serviceSlots();
		if (bridge_.takeStateDirty() && _host.canUseState()) {
			_host.stateMarkDirty();
		}
		refreshTail(); // and a lane that gained or lost its last audio device changes what the host is told
	}

	//------//
	// Tail //
	//------//

	/// **HOW LONG THE PLUGIN MAY SOUND AFTER ITS LAST NOTE** — `device::chainTailSamples` over every chain the
	/// editor holds: infinite while any holds an audio device, zero otherwise. The voices' own release needs
	/// no tail: they are sounding, and SLEEP already waits for them and for a silent block. clap-wrapper
	/// reports this as VST3's `getTailSamples`, which is what keeps a VST3 host calling us through a reverb.
	bool implementsTail() const noexcept override { return true; }
	/// `[main-thread, audio-thread]`, so it answers from an atomic the main thread computes.
	uint32_t tailGet() const noexcept override { return tail_.load(std::memory_order_relaxed); }

	/// MAIN THREAD: the tail of what the chains hold now. The host hears it moved from `process`.
	void refreshTail() noexcept {
		const rack::ChainState& state = bridge_.chainState();
		tail_.store(device::chainTailSamples([&](auto&& visit) {
			for (size_t c = 0; c < state.chainCount(); ++c) {
				for (const rack::DeviceState& row : state.chain(c)) {
					if (row.live()) {
						visit(bridge_.descriptorOf(row.instanceId));
					}
				}
			}
		}),
		            std::memory_order_relaxed);
	}

	/// **`clap_host_tail.changed` IS `[audio-thread]`**, so the block tells it, once per change. The host's
	/// extension is asked for in `activate` rather than through clap-helpers, whose `HostProxy` never
	/// fetches it (`canUseTail` is false against every host). The rack's plugin does the same.
	void tellTailIfMoved() noexcept {
		const uint32_t now = tail_.load(std::memory_order_relaxed);
		if (now != toldTail_) {
			toldTail_ = now;
			if (hostTail_ != nullptr && hostTail_->changed != nullptr) {
				hostTail_->changed(_host.host());
			}
		}
	}

	//---------//
	// Latency //
	//---------//

	/// **HOW LATE THE PLUGIN'S OUTPUT IS** (backlog JM) — what the lane
	/// RUNS, the serial sum of what its devices declare; the voices add none. Zero while no device declares
	/// one, which is every device that exists today. The rack's plugin answers the same way.
	bool implementsLatency() const noexcept override { return true; }
	/// `[main-thread & (being-activated | active)]` — written only by `activate`.
	uint32_t latencyGet() const noexcept override { return reportedLatency_; }

	/// **WHILE ACTIVE A LATENCY MAY NOT CHANGE, SO THE HOST IS ASKED TO RESTART** (CLAP, `ext/latency.h`).
	/// AUDIO THREAD, after the render, once per activation; `activate` reports the new number. A patch
	/// loaded with a latent device costs one restart here, because the lane adopts its devices at the first
	/// block rather than at `activate` (assumption L5).
	void askRestartIfLatencyMoved() noexcept {
		if (!restartAsked_ && synth_.latencySamples() != reportedLatency_) {
			restartAsked_ = true;
			_host.requestRestart();
		}
	}

	//------------//
	// Processing //
	//------------//

	bool init() noexcept override {
		// THE HOST, NAMED: a line from Bitwig and a line from a test host must not read alike.
		const clap_host* host = _host.host();
		log_.say(eventlog::initLine(host != nullptr ? host->name : nullptr, host != nullptr ? host->version : nullptr,
		                            host != nullptr ? host->vendor : nullptr));
		return true;
	}

	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) noexcept override {
		log_.say("activate rate=" + std::to_string(static_cast<long>(sampleRate)) + " frames=" + std::to_string(minFrames)
		         + ".." + std::to_string(maxFrames));
		// THE HOST'S LONGEST BLOCK GOES WITH THE RATE (backlog GO): every lane device sizes its scratch to it.
		synth_.activate(sampleRate, static_cast<int>(maxFrames));
		sidechainRate_ = sampleRate;
		sidechain_.reset(sampleRate);
		// THE LANE THE EDITOR IS HOLDING, not the one the engine happens to have been handed. activate
		// is main-thread and it is where the runners are emptied and re-rated, so this is where the
		// chain's devices get rebuilt at the new sample rate — an instance is PREPARED at a rate, so
		// one built at 44.1k is not one to keep when the host reactivates at 96k.
		readingsHops_.grow(bridge_.chainState().capacity()); // a hop for every position the lane can reach
		synth_.provisionDevices(bridge_.chainState(), bridge_.publishedChains().sequence(), &bridge_.fxBodies());
		bridge_.noteRefusedRoutes(synth_.takeOuterDrops()); // what a chain dropped inside, said by the tray
		synth_.drainRetiredDevices();
		// AND THE EDITOR IS TOLD, because it draws something that depends on it: the filter card's
		// curve. The Camp B models clamp their corner at 0.3 x samplerate, so a card drawing at a
		// fixed 48k shows a filter the host is not running — measured at up to 1.67 dB out at 44.1k.
		bridge_.setSampleRate(sampleRate);
		// **THE LATENCY, DECIDED HERE AND ONLY HERE** (CLAP: it may change only during `activate`), and the
		// host told it moved — `clap_host_latency.changed` is `[main-thread & being-activated]`, which is now.
		const uint32_t latency = synth_.latencySamples();
		if (latency != reportedLatency_) {
			reportedLatency_ = latency;
			if (_host.canUseLatency()) {
				_host.latencyChanged();
			}
		}
		restartAsked_ = false;
		quietSamples_ = 0;
		quietLatency_ = 0;
		refreshTail();
		// THE HOST'S TAIL EXTENSION, asked for once on the main thread; `tellTailIfMoved` uses it from `process`.
		hostTail_ = static_cast<const clap_host_tail*>(_host.host()->get_extension(_host.host(), CLAP_EXT_TAIL));
		toldTail_ = tail_.load(std::memory_order_relaxed); // a host activating us asks for the tail itself
#ifdef FEATHERS_DIAGNOSTIC
		activateSpikes(sampleRate);
#endif
		return true;
	}

	void deactivate() noexcept override {
		log_.say("deactivate");
#ifdef FEATHERS_DIAGNOSTIC
		drainSpikes(); // what the last blocks recorded, before the instance goes quiet
#endif
		// The other half of "no orphaned preview": a plugin taken off the rails should not come back
		// soloing one sound because of a button somebody pressed before. Nothing else is torn down here
		// — the synth's own silence is reset()'s job.
		bridge_.clearPreview();
		// A-PH: THE OPTIMISER'S THREAD LIVES activate → deactivate (joined here, before anything could be freed).
		synth_.deactivate();
	}

	void reset() noexcept override {
		params_.clearModulation();
		synth_.reset();
	}

	clap_process_status process(const clap_process* process) noexcept override {
#ifdef FEATHERS_DIAGNOSTIC
		const SpikeBlock spikeBlock(*this, process); // times the whole call, every return included (backlog SR)
#endif
		// DENORMALS FLUSHED FOR THIS CALL AND THE HOST'S STATE PUT BACK AFTER — see denormal_guard.h.
		const DenormalGuard denormals;
		// Before anything else in the block: a move made in the editor since the last one takes effect
		// at its start, rather than a block later than the user saw it happen.
		{
			const rack::StageScope stage(rack::Stage::Events); // the spike recorder's stages (backlog SR)
			consumeEditorEvents(process->out_events);
		}
		tellTailIfMoved();
		std::optional<rack::StageScope> inputs(std::in_place, rack::Stage::Inputs);

		// The audition's handles: the plane the character coordinates are read on, and how local the
		// blend is. Once a block rather than once a slice: neither is an event and neither carries a
		// timestamp, so cutting the block for them would be inventing a precision they do not have.
		// Costs nothing when nothing has moved — the bank drops a request for the character it is
		// already playing.
		// WHERE THE SONG IS, taken here with the other not-an-event inputs and for the same reason:
		// the transport struct describes the START OF THE BLOCK and carries no timestamp of its own.
		// The synth adds however much it has rendered since, so a note-on lands at its exact instant
		// rather than at a block boundary — which is what makes the phase the same at any buffer
		// length the host happens to bounce with.
		applyTransport(process->transport);
		synth_.setTuning(bridge_.tuning());
		// And the other direction: what the puck is doing, measured by the engine and published for the
		// scope. Not a handle — nobody sets this, it is what the gesture is.
		bridge_.publishPuckVelocity(synth_.puckVelocity());
		// And how much of each sound that puck is letting through, on the same road and for the same
		// reason — measured, not set. Published for a surface to draw, and handed BACK to the parameters
		// so a route from a weight source can reach a destination: the reading is one block old at that
		// point, which is what keeps a weight routed to Path a feedback loop rather than a recursion.
		// See ParamSet::setWeightSources.
		const SoundWeights weights = synth_.soundWeights();
		bridge_.publishSoundWeights(weights);
		params_.setWeightSources(weights);
		// Patch state rather than a handle, but read on the same schedule and for the same reason: it
		// is not an event, it carries no timestamp, and cutting the block for it would be inventing a
		// precision it does not have.
		synth_.setSoundTravel(bridge_.soundTravel());
		synth_.setSoundOrientations(bridge_.soundOrientations());
		synth_.setSoundGamma(bridge_.soundGamma());
		// THE LIKES' PHASE WARPS: the newest published set, which every character this block builds is built through.
		{
			const auto& likes = bridge_.takeLikeWarps();
			synth_.setLikeWarps(likes.warps.data(), likes.engine.data(), likes.serial);
		}
		synth_.setSoundResidue(bridge_.soundResidue());
		// AND THE SPECTRAL DRAWER'S MASTER SWITCH AND CURVE (additive design §3.1): patch state on the same schedule.
		bridge_.clearSpectralMoved(); // before the read, so an edit landing after it asks for another flush (D2)
		synth_.setSpectralState(bridge_.spectralState());
		// And which sound, if any, a hand is listening to on its own. Not patch state and not a handle:
		// it is where the user's attention is, it is never saved, and it is cleared when the editor goes
		// away — see preview.h.
		synth_.setPreview(bridge_.preview());
		// And where the macros reach. Same schedule and the same argument, with one addition: this lands
		// on the PARAMETERS rather than on the synth, because what a routing changes is what a parameter
		// resolves to, and every consumer already reads it there.
		// THE THREE LANES THAT ARE NOT PARAMETERS — routings, the rack's device settings and the
		// fx lane's. Written once and called from both entry points, because written twice at two
		// sites that must agree is exactly how setModRack came to exist at one of them and not the
		// other. See editor_lanes.h.
		const bool chainsMoved = publishEditorLanes(bridge_, params_, synth_);
		inputs.reset();
		// **A DEVICE PUBLISHED SINCE THE LAST BLOCK IS INSTALLED BEFORE THIS BLOCK'S EVENTS** (backlog QU): the lane
		// converges at the top of `render`, and the events at the block's first sample are consumed before that, so a key
		// pressed then went to the lane as it WAS — past a Pattern not yet installed (the first block after start: it
		// reached the voices on its own), or to the instance a load was replacing (lost). Settled first, the device that
		// will play the key is the one that hears it. Only on a block whose chains moved.
		if (chainsMoved) {
			synth_.settleLaneBeforeEvents();
		}

		const uint32_t frames = process->frames_count;
		float* left = channelOf(process, 0);
		float* right = channelOf(process, 1);
		if (left == nullptr || right == nullptr) {
			// A host that hands us a port we cannot write is a host we cannot play for. The events are
			// still consumed — all of them, past the end of the block as well, exactly as the normal
			// path finishes — so notes and automation do not pile up or arrive twice.
			consumeEventsFrom(process->in_events, 0, UINT32_MAX);
			bridge_.publish(params_);
			publishEngineReadings(bridge_, synth_);
		bridge_.publishMasterMeter(masterMeterReadingsOf(synth_.masterMeter())); // backlog DN
		bridge_.publishPadHits(synth_.padHits()); // the kit pads' pulse (pad_hits.h)
			return CLAP_PROCESS_CONTINUE;
		}

		// The block is cut at every event time and rendered in the pieces between, so a note lands on
		// the sample the host asked for rather than at the start of whatever buffer it arrived in. At
		// 512 samples a block, applying everything at the front would quantise timing to 10 ms, which
		// is audible as a strummed chord losing its strum.
		// THE SIDECHAIN INPUT (design §10.1): its buffers, and whether anything is connected to it (device::SidechainPresence).
		const float* sideL = nullptr;
		const float* sideR = nullptr;
		bool sideZero = false;
		if (process->audio_inputs_count >= 1 && process->audio_inputs != nullptr && process->audio_inputs[0].data32 != nullptr
		    && process->audio_inputs[0].channel_count >= 1) {
			const clap_audio_buffer& in0 = process->audio_inputs[0];
			sideL = in0.data32[0];
			sideR = in0.channel_count >= 2 ? in0.data32[1] : in0.data32[0];
			const uint64_t both = in0.channel_count >= 2 ? 3u : 1u;
			sideZero = (in0.constant_mask & both) == both && sideL != nullptr && sideR != nullptr && frames > 0
			           && sideL[0] == 0.0f && sideR[0] == 0.0f;
		}
		const bool sideConnected = sidechain_.update(sideL, sideR, frames, sideZero, sidechainRate_);
#ifdef FEATHERS_DIAGNOSTIC
		{
			const bool have = process->audio_inputs_count >= 1 && process->audio_inputs != nullptr;
			sidechain_.trace("feathers", process->audio_inputs_count, have && process->audio_inputs[0].data32 != nullptr,
			                 have ? process->audio_inputs[0].channel_count : 0u, have ? process->audio_inputs[0].constant_mask : 0u);
		}
#endif

		const clap_input_events* in = process->in_events;
		const uint32_t count = in == nullptr ? 0 : in->size(in);

		uint32_t at = 0;
		uint32_t next = 0;
		while (at < frames) {
			synth_.setSidechain(sideL != nullptr ? sideL + at : nullptr, sideR != nullptr ? sideR + at : nullptr, sideConnected);
			// Everything up to and including this instant, however many events share the timestamp.
			{
				const rack::StageScope stage(rack::Stage::Events);
				next = consumeEventsFrom(in, next, at);
			}

			// The slice runs to the next event, or to the end of the block. Whatever is left to
			// consume is strictly later than `at` — that is what the call above guarantees — so the
			// slice is never empty and the loop always advances.
			uint32_t until = frames;
			if (next < count) {
				const clap_event_header* header = in->get(in, next);
				if (header != nullptr) {
					until = std::min(frames, header->time);
				}
			}
			synth_.render(left + at, right + at, static_cast<int>(until - at));
			at = until;
		}
		// Anything left: events timestamped past the end of the block, which a host should not send
		// and some do, plus the whole list when the block is empty.
		{
			const rack::StageScope stage(rack::Stage::Events);
			consumeEventsFrom(in, next, UINT32_MAX);
		}

		// Everything the editor reads, taken at one instant and after every event in the block has
		// landed — so it never shows a base from one moment beside a modulation from another.
		{
			const rack::StageScope stage(rack::Stage::Publish);
			bridge_.publish(params_);
			publishEngineReadings(bridge_, synth_);
			bridge_.publishMasterMeter(masterMeterReadingsOf(synth_.masterMeter())); // backlog DN
			bridge_.publishPadHits(synth_.padHits()); // the kit pads' pulse (pad_hits.h)
		}
		askRestartIfLatencyMoved(); // what the lane RAN this block, against what the host was told (backlog JM)
		sendNoteEnds(process->out_events, frames);

		// SLEEP is a promise that no further audio arrives without new events. It holds exactly when
		// nothing is sounding: release tails and steal fades are voices, and they keep it CONTINUE
		// until they have run out.
		const uint32_t laneLatency = synth_.latencySamples();
		if (laneLatency != quietLatency_) {
			quietLatency_ = laneLatency;
			quietSamples_ = 0;
		}
		if (synth_.soundingVoiceCount() > 0) {
			quietSamples_ = 0;
			return CLAP_PROCESS_CONTINUE;
		}
		// **AND THE FX CHAIN'S TAIL IS SOUNDING TOO** (2026-09-25: a crackle in the late, quiet tail of every
		// reverb that decays — Owl, which sustains, was clean). With no voice left this said SLEEP while a reverb or a
		// delay was still ringing, so a host may stop calling the plugin, or call it only now and then, and the tail
		// comes out in fragments that stop dead. So: not while this block carries a sample. The devices are held to
		// reaching EXACT silence at the end of a tail (`test_fx_devices`' silence sweeps), so zero is the test, and a
		// device that sustains for ever (Owl) keeps the plugin awake, as it should.
		for (uint32_t i = 0; i < frames; ++i) {
			if (left[i] != 0.0f || right[i] != 0.0f) {
				quietSamples_ = 0;
				return CLAP_PROCESS_CONTINUE;
			}
		}
		// **AND NO DEVICE HOLDS SOUND STILL TO COME** (backlog MC). A silent block is not a finished tail: a delay's
		// output is exactly zero between a short note and its first repeat while its line holds the repeat. MEASURED
		// before this check: SLEEP 12.9 s into a Clean delay's tail, sounding again five blocks later. The count
		// restarts too, because what a holding device puts out may yet pass through a latent one after it.
		if (synth_.holdsTail()) {
			quietSamples_ = 0;
			return CLAP_PROCESS_CONTINUE;
		}
		// **AND NO NOTE DEVICE HAS A STEP STILL TO COME.** An arp on a held chord with a short gate is exactly silent between
		// its steps once the release has run, and so is a pattern running on the transport with no key: SLEEP there and a
		// host that honours it stops calling, so the next step never fires. And a pattern ARMED on a stopped transport
		// (Trig Transport, or Run Transport over a held chord) starts with the transport, which wakes no sleeping plugin:
		// CLAP promises a call only on an event or a change in the audio input. Awake while an emitter names a next call;
		// one that could play only on a new event (no key under Gate, switched off) names none, and this may sleep.
		if (synth_.notesScheduled()) {
			quietSamples_ = 0;
			return CLAP_PROCESS_CONTINUE;
		}
		// **AND NOT WHILE THE SIDECHAIN SOUNDS**. With no note playing the output is silent, so this said SLEEP
		// while the sidechain carried audio. CLAP lets a host stay away "until the next event or variation in audio input", and a
		// host that wakes only for events never calls again: the presence is never re-read and the card's "sidechain silent" is
		// stale until a note. Awake while it sounds; a second after it stops (`SidechainPresence::kHoldSeconds`) this may sleep.
		if (sidechain_.sounding()) {
			quietSamples_ = 0;
			return CLAP_PROCESS_CONTINUE;
		}
		// **AND NOTHING IS STILL IN FLIGHT INSIDE A LATENT DEVICE** (EQ step 7). An exactly silent output says
		// nothing about audio a device is holding: a flat linear-phase EQ is a 9216-sample delay, so a note shorter
		// than that has ended with every block so far silent, and SLEEP there is broken the moment it comes out.
		// Everything in flight entered within the lane's latency of the last voice or sound, so once more quiet
		// samples than that have passed, it has come out — and it would have reset this count if it were not silent.
		quietSamples_ += frames;
		if (quietSamples_ <= laneLatency) {
			return CLAP_PROCESS_CONTINUE;
		}
		return CLAP_PROCESS_SLEEP;
	}

private:
	/// The output buffer for one channel of the main port, or null if the host did not supply one.
	/// Only 32-bit buffers are played: the plugin never sets CLAP_AUDIO_PORT_SUPPORTS_64BITS, so a
	/// host offering 64-bit ones is offering something we did not ask for.
	static float* channelOf(const clap_process* process, uint32_t channel) noexcept {
		if (process->audio_outputs_count == 0) {
			return nullptr;
		}
		const clap_audio_buffer& out = process->audio_outputs[0];
		if (out.data32 == nullptr || channel >= out.channel_count) {
			return nullptr;
		}
		return out.data32[channel];
	}

	/// Apply every event from index `from` whose time is at or before `until`, and return the index
	/// of the first one that is not.
	///
	/// Events arrive sorted by time, so this walks forward and never rescans.
	uint32_t consumeEventsFrom(const clap_input_events* in, uint32_t from, uint32_t until) noexcept {
		if (in == nullptr) {
			return from;
		}
		const uint32_t count = in->size(in);
		uint32_t i = from;
		for (; i < count; ++i) {
			const clap_event_header* header = in->get(in, i);
			if (header == nullptr) {
				continue;
			}
			if (header->time > until) {
				break;
			}
			handleEvent(header);
		}
		return i;
	}

	/// Apply the editor's queued moves, and tell the host about every one of them.
	///
	/// Both halves matter. Without the first the knob does not sound; without the second the host does
	/// not know the parameter moved, so an automation lane records nothing and the host's own display
	/// of the value goes stale. The gestures are what let it record a drag as one undoable edit
	/// instead of several hundred.
	void consumeEditorEvents(const clap_output_events* out) noexcept {
		// The editor's own moves FIRST, before any debt owed to the host from earlier calls.
		//
		// The order is the whole point. A host's output queue can be full, and try_push then refuses
		// everything offered to it — so paying debts first, and giving up when one fails, would mean a
		// blocked queue stops the drain and the knob the user is turning right now never reaches
		// ParamSet at all. The parameter would go silent because the HOST was busy. Applying first
		// makes that impossible: what a full queue can delay is telling the host, never making sound.
		bridge_.drain([&](const UiParamEvent& event) {
			switch (event.kind) {
			case UiParamEvent::Kind::Value:
				params_.setBase(event.id, event.value);
				// Report what was STORED, not what was asked for: setBase clamps and rounds, and a host
				// told the raw number would hold a value the plugin does not have.
				if (emitParamValue(out, event.id, params_.base(event.id))) {
					// This id is now current with the host, so any older unpaid value for it is moot.
					clearDebt(event.id, &ParamDebt::value);
				}
				else {
					oweDebt(event.id, &ParamDebt::value);
				}
				break;
			case UiParamEvent::Kind::GestureBegin:
				// Not retried, and the asymmetry with the end below is deliberate. A begin re-sent later
				// opens a fresh undo grouping around edits that are not part of the gesture, and its own
				// end has already gone by — so a dropped begin costs one drag its grouping, while a
				// re-sent one corrupts the grouping of whatever follows.
				emitGesture(out, event.id, CLAP_EVENT_PARAM_GESTURE_BEGIN);
				break;
			case UiParamEvent::Kind::GestureEnd:
				// An end IS worth retrying: it is safe on its own — a host that never saw the begin
				// ignores it — and if the begin did land, this is the only thing that closes the undo
				// grouping. Dropping it leaves the host accumulating every later edit into the user's
				// drag.
				if (emitGesture(out, event.id, CLAP_EVENT_PARAM_GESTURE_END)) {
					clearDebt(event.id, &ParamDebt::gestureEnd);
				}
				else {
					oweDebt(event.id, &ParamDebt::gestureEnd);
				}
				break;
			}
		});

		// THE HOST SLOTS' OWN RING, after the core params' queue and before their debts — the order the core
		// params take (a hand's moves before anything owed). Its debts are its own (`SlotOutbox`).
		drainSlotOutbox(out);

		payDebts(out);
	}

	/// What the host has not been told yet about one parameter.
	///
	/// A host's queue being full does not undo a parameter move — it is in ParamSet either way — but it
	/// leaves the host holding a stale copy, which surfaces as an automation lane that missed a move or
	/// an undo grouping that never closed. One entry per id, so a drag that overflows the queue for a
	/// second records the parameter once rather than hundreds of times.
	struct ParamDebt {
		clap_id id = 0;
		bool value = false;
		bool gestureEnd = false;
	};

	void oweDebt(clap_id id, bool ParamDebt::*what) noexcept {
		for (size_t i = 0; i < debtCount_; ++i) {
			if (debts_[i].id == id) {
				debts_[i].*what = true;
				return;
			}
		}
		if (debtCount_ < debts_.size()) {
			debts_[debtCount_] = ParamDebt{id, false, false};
			debts_[debtCount_].*what = true;
			++debtCount_;
		}
		// Full means every declared parameter is already waiting, so there is nothing left to record.
	}

	void clearDebt(clap_id id, bool ParamDebt::*what) noexcept {
		for (size_t i = 0; i < debtCount_; ++i) {
			if (debts_[i].id == id) {
				debts_[i].*what = false;
				return;
			}
		}
	}

	/// Re-send what the host refused earlier, and forget whatever it accepts.
	///
	/// Values are read fresh from ParamSet rather than replayed, so a re-send after several more moves
	/// carries the current value instead of the one that failed. A value goes before its gesture end,
	/// which is the order a host expects to see them in.
	void payDebts(const clap_output_events* out) noexcept {
		size_t remaining = 0;
		for (size_t i = 0; i < debtCount_; ++i) {
			ParamDebt& debt = debts_[i];
			if (debt.value && emitParamValue(out, debt.id, params_.base(debt.id))) {
				debt.value = false;
			}
			if (debt.gestureEnd && emitGesture(out, debt.id, CLAP_EVENT_PARAM_GESTURE_END)) {
				debt.gestureEnd = false;
			}
			// Still owed something: keep it, compacted to the front.
			if (debt.value || debt.gestureEnd) {
				debts_[remaining++] = debt;
			}
		}
		debtCount_ = remaining;
	}

	/// True if and only if the host now has this event.
	///
	/// A null queue answers false along with a full one, even though the two will never be retried
	/// with the same success. That is on purpose: the return means "delivered", and letting it also
	/// mean "do not bother" would put two conditions behind one bit, so a later reader could not tell
	/// a debt that was paid from one that was written off. CLAP always supplies an output queue to
	/// process() and flush(), so the null case is a defensive path — and there the debt simply sits,
	/// deduplicated and bounded by the parameter count.
	static bool emitParamValue(const clap_output_events* out, clap_id id, double value) noexcept {
		if (out == nullptr) {
			return false;
		}
		clap_event_param_value event = {};
		event.header.size = sizeof(event);
		event.header.time = 0;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.param_id = id;
		// Addressed to the whole plugin rather than to a voice: the editor moves the global parameter.
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = value;
		return out->try_push(out, &event.header);
	}

	/// True if and only if the host now has this event. Same reading as emitParamValue's.
	static bool emitGesture(const clap_output_events* out, clap_id id, uint16_t type) noexcept {
		if (out == nullptr) {
			return false;
		}
		clap_event_param_gesture event = {};
		event.header.size = sizeof(event);
		event.header.time = 0;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = type;
		event.param_id = id;
		return out->try_push(out, &event.header);
	}

	/// Apply only the parameter events in a list, which is the whole of what a flush may carry.
	///
	/// clap_plugin_params.flush is how a host moves parameters while the plugin is NOT processing,
	/// and it may be called from the main thread. A note event in that list is a host bug; playing it
	/// would start a voice from whatever thread the flush arrived on, into a synth the audio thread
	/// may be reading. Dropping it is both the contract and the only safe reading of it.
	void consumeParamEvents(const clap_input_events* in) noexcept {
		if (in == nullptr) {
			return;
		}
		const uint32_t count = in->size(in);
		for (uint32_t i = 0; i < count; ++i) {
			const clap_event_header* header = in->get(in, i);
			if (header != nullptr) {
				handleParamEvent(header);
			}
		}
	}

	/// **THE HOST WROTE SLOT `n`** (plan §1.2, T6b). AUDIO THREAD in `process`; in `params.flush` whichever
	/// thread the host flushes on — while the plugin sleeps, flush is the ONLY road a host write takes (plan
	/// §0.13), and this is the same call, so the value reaches the bus there too. The value is denormalised
	/// against the declaration the bridge published for the slot (a lock-free pointer, no allocation) and
	/// written to the bus, which sets the `hostWrote` bit the next main-thread visit folds into an anchor and
	/// mirrors into the record; the engine applies it at the top of its next block.
	///
	/// A SLOT THAT DRIVES NOTHING YET (Empty, or mapped since the last visit published its binding) is
	/// ignored: there is no declaration to denormalise against, and the bus's value would be in no control's
	/// units. The host will write again; a single write in that window is lost, and so is its anchor — but
	/// the ID IS MARKED (2026-09-26): the host has a lane there, so the pool reuses it last.
	void onHostSlotValue(hostslots::SlotIndex n, double value) noexcept {
		const device::ParamDesc* desc = bridge_.slotDescOf(n);
		if (desc == nullptr) {
			bridge_.slotBus().onHostTouched(n);
			requestSlotService();
			return;
		}
		bridge_.slotBus().onHostValue(n, static_cast<float>(device::fromNormal(*desc, value)));
		// The main thread mirrors and anchors; with no editor ticking, this is what brings it round.
		requestSlotService();
	}

	/// **THE SLOTS' OUTBOX TO THE HOST** — Begin, Value, End on the slot's id, with `SlotOutbox`'s debt rules
	/// (an owed End before a new Begin, plan §0.12; a Value re-read fresh; a Begin never retried). Both
	/// `process` and `params.flush` drain it, through `consumeEditorEvents`. The drain is `slot_host`'s, the one
	/// the standalone rack's two sites call too (T10).
	void drainSlotOutbox(const clap_output_events* out) noexcept { hostslots::drainOutbox(bridge_.slotOutbox(), out); }

	/// The parameter half of the event vocabulary. Returns true if it was one.
	bool handleParamEvent(const clap_event_header* header) noexcept {
		if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
			return false;
		}
		switch (header->type) {
		case CLAP_EVENT_PARAM_VALUE: {
			const auto* event = reinterpret_cast<const clap_event_param_value*>(header);
			// A HOST SLOT FIRST — `isSlotId` before the core table, as every params call site asks it.
			if (hostslots::isSlotId(event->param_id)) {
				onHostSlotValue(hostslots::slotOfId(event->param_id), event->value);
				return true;
			}
			params_.setBase(event->param_id, event->value);
			return true;
		}
		case CLAP_EVENT_PARAM_MOD: {
			const auto* event = reinterpret_cast<const clap_event_param_mod*>(header);
			if (hostslots::isSlotId(event->param_id)) {
				// **GLOBAL MODULATION ON A SLOT IS AN OFFSET ON TOP OF ITS VALUE** (design §4). A slot is a 0..1
				// param, so CLAP's amount — in the param's own units — IS the normalised offset the engine
				// scales by the control's span (`RackEngine::resolveControl`). A NON-ZERO amount ANCHORS AND MARKS
				// the slot as a host write does (the bus's rule, `onHostMod`;), so a visit is asked
				// for. A PER-NOTE mod on a slot is ignored in v1: a device control is not per voice
				// on this road, and handing a slot id to the synth's per-note table would address nothing.
				if (event->note_id == -1 && event->key == -1 && event->channel == -1) {
					bridge_.slotBus().onHostMod(hostslots::slotOfId(event->param_id),
					                            static_cast<float>(event->amount));
					if (event->amount != 0.0) {
						requestSlotService();
					}
				}
				return true;
			}
			// Addressed to the whole plugin, or to one note. The global lane is a param setting shared
			// by every voice; a per-note offset belongs to the voice and to nothing else, so the two
			// go to different places.
			if (event->note_id == -1 && event->key == -1 && event->channel == -1) {
				params_.setModOffset(event->param_id, event->amount);
			}
			else {
				synth_.paramModForNote(event->channel, event->key, event->note_id, event->param_id,
				                       event->amount);
			}
			return true;
		}
		default:
			return false;
		}
	}

	/// Take the host's transport, if it offered one.
	///
	/// A NULL POINTER IS A REAL ANSWER AND NOT AN ERROR: CLAP does not oblige a host to provide
	/// transport, and several do not while the plugin is played live rather than bounced. We say
	/// nothing to the synth in that case, which leaves Analog on the per-voice behaviour it shipped
	/// with — the honest fallback, since the alternative is to assume the song is at zero and hand
	/// every note an identical phase while calling it determinism.
	///
	/// SECONDS WHERE BOTH ARE OFFERED, and the choice is audible rather than arbitrary: the
	/// oscillator's frequency is in Hz, so locking phase to a beat timeline would make the same note
	/// at the same bar start at a different phase in a project at a different tempo.
	///
	/// AND BEATS DERIVED WHERE SECONDS ARE ABSENT, WHICH LOOKS WRONG AND IS NOT — the note is here
	/// because the next reader will see an inexact conversion and want to "fix" it back to a
	/// fallback. It IS inexact: tempo carries a per-sample ramp (`tempo_inc`), so beats-to-seconds is
	/// really an integral and a single instantaneous tempo cannot reproduce true elapsed seconds
	/// under tempo automation.
	///
	/// NONE OF THAT MATTERS, because bounce determinism does not require true elapsed seconds — it
	/// requires a DETERMINISTIC FUNCTION OF SONG POSITION. Beats and the tempo at that position are
	/// both read from the transport AT the note, so a phase derived from them is the same on every
	/// render of that position, which is the entire guarantee. What the derivation loses is only the
	/// correspondence to real elapsed time, and there is no listener for whom "locked to true
	/// seconds" is perceptibly different from "locked to the timeline". So a beats-only host keeps
	/// the feature instead of being dropped to the fallback for a property that buys nothing.
	void applyTransport(const clap_event_transport* transport) noexcept {
		reportTransportOnce(transport);
		// **WHETHER THE HOST IS RECORDING** (host slots, design §5): a hand on a slotted control while it is
		// anchors the slot. A host that offers no transport is not recording anything we can know of.
		bridge_.setHostRecording(transport != nullptr && (transport->flags & CLAP_TRANSPORT_IS_RECORDING) != 0);
		if (transport == nullptr) {
			return;
		}
		// TEMPO FIRST AND UNCONDITIONALLY, because it is a different question from position. The
		// early returns below are about which TIMELINE the host offers; a host may offer neither and
		// still report a tempo, and CLAP_TRANSPORT_HAS_TEMPO is a separate flag from
		// CLAP_TRANSPORT_IS_PLAYING. Reading it here means a stopped session — the commonest case a
		// hand meets — still drives a musically-timed modulator at the right rate.
		synth_.setTempo((transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 ? transport->tempo : 0.0);

		const bool playing = (transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;

		// **BOTH TIMELINES ARE CARRIED NOW, EACH ONLY IF THE HOST HAS IT — backlog DQ.** This used to
		// pick one and convert: seconds if offered, otherwise beats turned into seconds through the
		// tempo. That was right while the only consumer was oscillator phase, where a lock to "the
		// timeline" is a lock either way — the note above says so and it is still true of that road.
		//
		// It is wrong for a SYNCED LFO. Its period is N beats, so a beats-derived phase stays on the
		// grid through a tempo change where a seconds-derived one goes discontinuous; and a free Hz
		// rate wants seconds for the mirror-image reason. The device chooses, so the conversion
		// cannot happen here — it would collapse the choice before anything could make it.
		// **EACH PUT BACK ON THE SAMPLE IT STANDS ON** (`device::onSample`, 2026-10-31): CLAP's fixed point leaves a block
		// that starts on a sample a hair past it, and every transport-locked step landed one sample late — MEASURED here,
		// an Arp at 6001 for 6000 ("an Arp in Feathers' modulator lane steps on the host's song grid", test_clap_render).
		const bool haveTempo = (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && transport->tempo > 0.0;
		const double seconds = (transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0
		    ? device::onSample(secondsOf(transport->song_pos_seconds), sidechainRate_)
		    : mod::TransportClock::kNoPosition;
		const double beats = (transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0
		    ? device::onSample(beatsOf(transport->song_pos_beats), haveTempo ? sidechainRate_ * 60.0 / transport->tempo : 0.0)
		    : mod::TransportClock::kNoPosition;

		// **THE SECONDS FALLBACK SURVIVES, because the oscillator lock depends on it.** A beats-only
		// host still gets a usable seconds position, derived through the tempo — the paragraph above
		// this function explains why that is honest for a phase lock and not for a tempo claim. What
		// changed is that the beats are no longer thrown away once the conversion is made.
		const double secondsForLock = std::isfinite(seconds) ? seconds
		    : (std::isfinite(beats) && (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0
		       && transport->tempo > 0.0)
		        ? beats * 60.0 / transport->tempo
		        : mod::TransportClock::kNoPosition;

		// A transport with neither timeline is a transport that cannot say where the song is. The
		// flags exist precisely so this case is distinguishable from "position zero", and treating it
		// as zero is the mistake this distinction exists to avoid — `setTransport` receives NaN and
		// every reader asks `havePosition*()` rather than comparing against a magic number.
		synth_.setTransport(secondsForLock, beats, playing);
	}

	/// RECORD ONCE WHAT THIS HOST ACTUALLY OFFERS, and say it from the main thread (`sayTransportOnce`).
	///
	/// It exists because "does this host provide a seconds timeline" is not answerable from the
	/// headers and decides which road every note takes — and the only venue that can answer it is a
	/// real DAW, which is expensive to ask and free to overhear. Recorded on the first process call of
	/// the instance; this runs on the audio thread, so it writes one atomic and asks the host for a
	/// main-thread visit, and the stderr write happens there.
	void reportTransportOnce(const clap_event_transport* transport) noexcept {
		if (reportedTransport_) {
			return;
		}
		reportedTransport_ = true;
		uint8_t seen = kTransportSeen;
		if (transport != nullptr) {
			seen |= (transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0 ? kTransportSeconds : 0;
			seen |= (transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0 ? kTransportBeats : 0;
			seen |= (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 ? kTransportTempo : 0;
		} else {
			seen |= kTransportNone;
		}
		transportSeen_.store(seen, std::memory_order_release);
		_host.requestCallback();
	}

	/// The main-thread half of `reportTransportOnce`: prints what the audio thread recorded, once.
	void sayTransportOnce() noexcept {
		const uint8_t seen = transportSeen_.load(std::memory_order_acquire);
		if ((seen & kTransportSeen) == 0 || transportSaid_) {
			return;
		}
		transportSaid_ = true;
		if ((seen & kTransportNone) != 0) {
			std::fprintf(stderr, "[feathers] transport: none offered — Analog keeps its per-voice"
			                     " phase\n");
			return;
		}
		const bool seconds = (seen & kTransportSeconds) != 0;
		const bool beats = (seen & kTransportBeats) != 0;
		const bool tempo = (seen & kTransportTempo) != 0;
		std::fprintf(stderr,
		             "[feathers] transport: seconds %s, beats %s, tempo %s — Analog locks to %s\n",
		             seconds ? "yes" : "no", beats ? "yes" : "no", tempo ? "yes" : "no",
		             seconds          ? "the seconds timeline"
		             : (beats && tempo) ? "beats via tempo"
		                                : "nothing (per-voice phase)");
	}

	/// CLAP's fixed-point time, in the units the header defines: 1/2^31 of a second or of a beat.
	/// Written out rather than assumed, because the constant is the whole of the conversion and a
	/// wrong one would be a slow drift nobody could see in a single block.
	static double secondsOf(clap_sectime t) noexcept {
		return static_cast<double>(t) / static_cast<double>(CLAP_SECTIME_FACTOR);
	}
	static double beatsOf(clap_beattime t) noexcept {
		return static_cast<double>(t) / static_cast<double>(CLAP_BEATTIME_FACTOR);
	}

	/// **A MIDI 1.0 EVENT, THROUGH THE SAME DOORS THE CLAP NOTE EVENTS USE** — so the note lane and the voices
	/// hear it identically: the note id is -1 (MIDI has none, and matching falls back to channel and key) and
	/// the velocity is the data byte over 127, which is CLAP's 0..1. The channel is the status byte's low
	/// nibble; the port index is not read, as the CLAP events' is not.
	///
	/// TAKEN: note on (velocity 0 is a note off, as MIDI defines it), note off, pitch bend, CC 64 (sustain,
	/// 64 and above is down), CC 123 (all notes off: a wildcard release, the road CLAP's own wildcard
	/// note-off takes) and CC 120 (all sound off: a wildcard choke). IGNORED: every other message — v1 has
	/// no CC destinations, and the pressure messages have no consumer here.
	void handleMidi(const clap_event_midi* event) noexcept {
		const uint8_t status = event->data[0];
		const int16_t channel = static_cast<int16_t>(status & 0x0F);
		const uint8_t d1 = event->data[1] & 0x7F;
		const uint8_t d2 = event->data[2] & 0x7F;
		switch (status & 0xF0) {
		case 0x90:
			if (d2 == 0) {
				synth_.noteOff(channel, d1, -1, 0.0);
			} else {
				synth_.noteOn(channel, d1, -1, static_cast<double>(d2) / 127.0);
			}
			break;
		case 0x80:
			synth_.noteOff(channel, d1, -1, static_cast<double>(d2) / 127.0);
			break;
		case 0xE0:
			synth_.pitchBend14(channel, d1 | (d2 << 7));
			break;
		case 0xB0:
			if (d1 == 64) {
				synth_.sustainPedal(channel, d2 >= 64);
			} else if (d1 == 123) {
				synth_.noteOff(channel, -1, -1, 0.0);
			} else if (d1 == 120) {
				synth_.choke(channel, -1, -1);
			}
			break;
		default:
			break;
		}
	}

	void handleEvent(const clap_event_header* header) noexcept {
		if (header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
			return;
		}
		if (handleParamEvent(header)) {
			return;
		}
		switch (header->type) {
		case CLAP_EVENT_TRANSPORT: {
			// A HOST WITH SAMPLE-ACCURATE TEMPO OR LOCATE CHANGES sends these mid-block, and dropping
			// them would leave the song position stale until the next block — which is a locate the
			// oscillator does not hear, and therefore a bounce that does not match itself across the
			// join. Re-anchoring here is exactly what setTransport is for: the synth zeroes its
			// within-block counter, so what follows is measured from this instant.
			applyTransport(reinterpret_cast<const clap_event_transport*>(header));
			break;
		}
		case CLAP_EVENT_NOTE_ON: {
			const auto* event = reinterpret_cast<const clap_event_note*>(header);
			synth_.noteOn(event->channel, event->key, event->note_id, event->velocity);
			break;
		}
		case CLAP_EVENT_NOTE_OFF: {
			const auto* event = reinterpret_cast<const clap_event_note*>(header);
			synth_.noteOff(event->channel, event->key, event->note_id, event->velocity);
			break;
		}
		case CLAP_EVENT_NOTE_CHOKE: {
			const auto* event = reinterpret_cast<const clap_event_note*>(header);
			synth_.choke(event->channel, event->key, event->note_id);
			break;
		}
		case CLAP_EVENT_NOTE_EXPRESSION: {
			// **TAKEN NOW — backlog DS.** This read *"Declared unsupported and ignored rather than
			// misread"*, which was honest and was also the whole reason harm's pitch did not follow a
			// per-note bend. Now the instrument reacts to MPE/note
			// expressions, and effects like Harm respect them.
			//
			// NOTHING IS TRANSLATED HERE. The event's address goes to the manager and its expression
			// id goes through untouched; `Synth::Responder::setNoteExpression` decides which ones this
			// instrument answers to, which keeps the "what do we support" question in ONE place
			// instead of split across a dispatch and a responder that could disagree.
			const auto* event = reinterpret_cast<const clap_event_note_expression*>(header);
			synth_.noteExpression(event->channel, event->key, event->note_id, event->expression_id,
			                      event->value);
			break;
		}
		case CLAP_EVENT_MIDI:
			handleMidi(reinterpret_cast<const clap_event_midi*>(header));
			break;
		default:
			// Parameter events were taken above, so anything still here is something v1 does not
			// speak: MIDI 2.0 and SysEx (the port declares only the CLAP and MIDI 1.0 dialects it
			// reads), extension spaces.
			break;
		}
	}

	/// **NOTE_END FOR EVERY HOST NOTE WHOSE VOICE ENDED THIS BLOCK**, after the block's frames are processed, as the header
	/// has it ("time: ignored"): stamped at the block's last frame. One that the host's queue refuses stays queued for the
	/// next call.
	void sendNoteEnds(const clap_output_events* out, uint32_t frames) noexcept {
		if (out == nullptr || out->try_push == nullptr) {
			return;
		}
		while (const Synth::NoteEnd* end = synth_.frontNoteEnd()) {
			clap_event_note e {};
			e.header.size = sizeof(e);
			e.header.time = frames > 0 ? frames - 1 : 0;
			e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			e.header.type = CLAP_EVENT_NOTE_END;
			e.header.flags = 0;
			e.note_id = end->noteId;
			e.port_index = 0;
			e.channel = end->channel;
			e.key = end->key;
			e.velocity = 0.0;
			if (!out->try_push(out, &e.header)) {
				return;
			}
			synth_.dropNoteEnd();
		}
	}

	/// Drop whichever main-thread callback the editor was registered for. Safe to call when there is
	/// none, which is what lets guiDestroy and a re-parent share it.
	void unregisterGuiCallbacks() noexcept {
		if (guiFd_ >= 0) {
			_host.posixFdSupportUnregister(guiFd_);
			guiFd_ = -1;
		}
		if (guiTimer_ != CLAP_INVALID_ID) {
			_host.timerSupportUnregister(guiTimer_);
			guiTimer_ = CLAP_INVALID_ID;
		}
	}

	//------------------------------------------------------------------------------------------//
	// THE TEST-CHAIN EXTENSION -- plugin/feathers_test_chain.h says why it ships in every build. //
	//                                                                                           //
	// Thin forwardings onto the roads the editor's own widgets take. Nothing here DECIDES        //
	// anything, which is the property that keeps the photograph honest: the lane refuses an      //
	// unknown tag from the device's own declarations, and the bridge coerces a control against   //
	// the control's own. MAIN THREAD ONLY, as the header declares.                               //
	//------------------------------------------------------------------------------------------//

	static FeathersPlugin* fromClap(const clap_plugin* plugin) noexcept {
		// plugin_data holds the PluginBase subobject -- clap-helpers stores `this` from THAT class,
		// and FeathersPlugin has a second base (GuiHost), so the two addresses are not the same one.
		// Casting the void* straight across would be a silent offset error rather than a crash.
		return static_cast<FeathersPlugin*>(static_cast<PluginBase*>(plugin->plugin_data));
	}

	static uint32_t testChainAddDevice(const clap_plugin* plugin, const char* tag) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || tag == nullptr) {
			return UINT32_MAX;
		}
		const size_t slot = fromClap(plugin)->bridge_.addFxDevice(tag);
		return slot == kNoPosition ? UINT32_MAX : static_cast<uint32_t>(slot);
	}

	static bool testChainSetControl(const clap_plugin* plugin, uint32_t slot, const char* control,
	                                double value) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || control == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		if ((slot & CLAP_FEATHERS_TEST_STRIP) != 0) {
			// THE STRIP: resolved the same way, by name against the plugin's own registry, under the device's id.
			DeviceInstanceId id = kNoInstance;
			size_t index = 0;
			const device::Descriptor* desc = nullptr;
			if (!self->laneControlForTest(slot, control, &id, &index, &desc)) {
				return false;
			}
			self->bridge_.setDeviceControl(id, index, static_cast<float>(value));
			return true;
		}
		const FxChain chain = self->bridge_.fxChain();
		if (slot >= chain.count) {
			return false;
		}
		// RESOLVED AGAINST THE PLUGIN'S OWN REGISTRY, never against the caller's copy of it.
		const device::Descriptor* desc = deviceForTag(fxDeviceAtPosition(chain, slot).tag().data());
		if (desc == nullptr) {
			return false;
		}
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (desc->params[i].name != nullptr
			    && std::strcmp(desc->params[i].name, control) == 0) {
				// **THE ONE PLACE THAT CONVERTS, AND IT CONVERTS BECAUSE ITS CONTRACT IS POSITIONAL.**
				// `chain_add_device` hands a venue a POSITION and `feathers_test_chain.h` documents it as
				// one; that is an external ABI and the lane arc does not get to change it. So the position
				// is turned into the device's id HERE, at the edge, rather than the setter being given two
				// meanings.
				self->bridge_.setDeviceControl(self->bridge_.fxDeviceAt(slot), i, static_cast<float>(value));
				return true;
			}
		}
		return false;
	}

	static bool testChainAvailableDevice(const clap_plugin* plugin, uint32_t index, char* tagOut,
	                                     uint32_t tagCapacity) noexcept {
		// The plugin is not consulted: which devices exist is a property of the BUILD, not of an
		// instance. Taken anyway so every function in the extension has the same shape.
		(void)plugin;
		if (tagOut == nullptr || tagCapacity == 0) {
			return false;
		}
		uint32_t seen = 0;
		for (const device::Descriptor& desc : kDevices) {
			// WHAT THE LANE WILL TAKE -- `maySitInLane`, which is what addFxDevice refuses on, so a tag
			// listed here is one the very next call accepts. Modulators too, since 2026-10-04.
			if (!maySitInLane(desc) || desc.traits.tag == nullptr) {
				continue;
			}
			if (seen++ != index) {
				continue;
			}
			std::snprintf(tagOut, tagCapacity, "%s", desc.traits.tag);
			return true;
		}
		return false;
	}

	static uint32_t testChainDeviceCount(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		return static_cast<uint32_t>(fromClap(plugin)->bridge_.fxChain().count);
	}

	static bool testChainRenameMacro(const clap_plugin* plugin, uint32_t index, const char* name) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || name == nullptr || index >= kMacroCount) {
			return false;
		}
		// THE CAPTION'S OWN CALL (mod_panel's `rename`), so what the host is told is what a rename tells it.
		fromClap(plugin)->bridge_.setMacroName(index, name);
		return true;
	}

	/// The slot target for the lane device at `position`'s control called `control` (by NAME, `set_control`'s
	/// rule), or nullopt.
	std::optional<hostslots::Target> laneTargetForTest(uint32_t position, const char* control) const {
		const DeviceInstanceId device = bridge_.fxDeviceAt(position);
		const device::Descriptor* desc = bridge_.descriptorOf(device);
		if (desc == nullptr || control == nullptr) {
			return std::nullopt;
		}
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (desc->params[i].name != nullptr && std::strcmp(desc->params[i].name, control) == 0) {
				return bridge_.slotTargetOf(device, i);
			}
		}
		return std::nullopt;
	}

	/// `touch_slot` and `pin_slot`: the pool's own verbs, and a main-thread visit asked for — the rescan is
	/// the VISIT's, never the touch's (plan T6a: "three takeovers in one visit cost one RESCAN_INFO").
	static uint32_t testChainSlotVerb(const clap_plugin* plugin, uint32_t position, const char* control,
	                                  bool pin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return UINT32_MAX;
		}
		FeathersPlugin* self = fromClap(plugin);
		try {
			const std::optional<hostslots::Target> target = self->laneTargetForTest(position, control);
			if (!target) {
				return UINT32_MAX;
			}
			hostslots::SlotPool& pool = self->bridge_.slotPool();
			const hostslots::Allocation a = pin ? pool.pin(*target) : pool.touch(*target);
			self->requestSlotService();
			return a.slot == hostslots::kNoSlot ? UINT32_MAX : a.slot;
		}
		catch (...) {
			return UINT32_MAX;
		}
	}
	static uint32_t testChainTouchSlot(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		return testChainSlotVerb(plugin, position, control, false);
	}
	static uint32_t testChainPinSlot(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		return testChainSlotVerb(plugin, position, control, true);
	}

	/// The lane's remove, as the card's ✕ runs it (`FxLane::removeDeviceAtLane`): the device and its routes
	/// lifted first, so `undo_remove` can give them back as `DeviceSlotEdit::give` does.
	static bool testChainRemoveDevice(const clap_plugin* plugin, uint32_t position) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		try {
			const FxChain chain = self->bridge_.fxChain();
			if (position >= chain.count) {
				return false;
			}
			RemovedForTest held;
			held.position = position;
			held.device = fxDeviceAtPosition(chain, position);
			held.routes = self->bridge_.routesOf(self->bridge_.fxDeviceAt(position));
			if (!self->bridge_.removeFxDevice(position)) {
				return false;
			}
			held.held = true;
			self->removedForTest_ = std::move(held);
			self->requestSlotService();
			return true;
		}
		catch (...) {
			return false;
		}
	}
	static bool testChainUndoRemove(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		RemovedForTest& held = self->removedForTest_;
		if (!held.held) {
			return false;
		}
		const bool ok = self->bridge_.insertFxDevice(held.position, held.device, held.routes);
		held.held = false;
		self->requestSlotService();
		return ok;
	}

	/// The preset browser's two roads (`GuiHost::guiCaptureState` / `guiLoadState`), for a venue with no editor.
	static bool testChainCapturePreset(const clap_plugin* plugin, const clap_ostream* stream) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || stream == nullptr) {
			return false;
		}
		fromClap(plugin)->guiCaptureState(const_cast<clap_ostream*>(stream));
		return true;
	}
	static bool testChainLoadPreset(const clap_plugin* plugin, const clap_istream* stream) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || stream == nullptr) {
			return false;
		}
		return fromClap(plugin)->guiLoadState(const_cast<clap_istream*>(stream));
	}

	static bool testChainReplaceWithFilter(const clap_plugin* plugin, uint32_t position) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		if (position >= self->bridge_.fxChain().count) {
			return false;
		}
		self->bridge_.setFilterSettings(position, fx::filter::State {});
		self->requestSlotService();
		return true;
	}

	/// A TEST POSITION'S DEVICE: the lane's, or — with `CLAP_FEATHERS_TEST_STRIP` set — the modulator strip's.
	DeviceInstanceId deviceAtForTest(uint32_t position) const {
		return (position & CLAP_FEATHERS_TEST_STRIP) != 0 ? bridge_.rackDeviceAt(position & ~CLAP_FEATHERS_TEST_STRIP)
		                                                  : bridge_.fxDeviceAt(position);
	}

	/// The lane device at `position` and the index of its control called `control` (by NAME), or false.
	bool laneControlForTest(uint32_t position, const char* control, DeviceInstanceId* device, size_t* index,
	                        const device::Descriptor** descOut) const {
		const DeviceInstanceId id = deviceAtForTest(position);
		const device::Descriptor* desc = bridge_.descriptorOf(id);
		if (desc == nullptr || control == nullptr) {
			return false;
		}
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (desc->params[i].name != nullptr && std::strcmp(desc->params[i].name, control) == 0) {
				*device = id;
				*index = i;
				*descOut = desc;
				return true;
			}
		}
		return false;
	}

	static uint32_t testChainSlotState(const clap_plugin* plugin, uint32_t slot) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return UINT32_MAX;
		}
		const hostslots::SlotPool& pool = fromClap(plugin)->bridge_.slotPool();
		return slot < pool.capacity() ? static_cast<uint32_t>(pool.state(static_cast<hostslots::SlotIndex>(slot)))
		                              : UINT32_MAX;
	}

	static bool testChainEngineControl(const clap_plugin* plugin, uint32_t position, const char* control, double* base,
	                                   double* resolved) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		const FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(position, control, &device, &index, &desc)) {
			return false;
		}
		const LaneRack& lane = self->synth_.laneForTest();
		const size_t slot = lane.slotOf(device);
		if (slot == rack::RackEngine::kNoSlot) {
			return false;
		}
		const rack::RackEngine& engine = lane.engineForTest();
		if (base != nullptr) {
			*base = static_cast<double>(engine.fieldAt(slot, index));
		}
		if (resolved != nullptr) {
			*resolved = static_cast<double>(engine.modulatedFieldAt(slot, index));
		}
		return true;
	}

	static bool testChainKnobValue(const clap_plugin* plugin, uint32_t position, const char* control,
	                               double* value) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || value == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(position, control, &device, &index, &desc)) {
			return false;
		}
		const ui::DeviceControl knob(self->testDeviceHost_, device, *desc, index);
		*value = knob.value();
		return true;
	}

	static bool testChainDragControl(const clap_plugin* plugin, uint32_t position, const char* control,
	                                 const double* values, uint32_t count) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || (values == nullptr && count > 0)) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(position, control, &device, &index, &desc)) {
			return false;
		}
		try {
			// THE WIDGET'S OWN SEQUENCE over the editor's own host adapter — nothing here reaches past them.
			ui::DeviceControl knob(self->testDeviceHost_, device, *desc, index, nullptr, &self->testHistory_);
			knob.beginGesture();
			for (uint32_t i = 0; i < count; ++i) {
				knob.setValue(values[i]);
			}
			knob.endGesture();
			return true;
		}
		catch (...) {
			return false;
		}
	}

	static bool testChainUndo(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		if (!self->testHistory_.canUndo()) {
			return false;
		}
		try {
			self->testHistory_.undo();
			return true;
		}
		catch (...) {
			return false;
		}
	}

	static bool testChainResetControl(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(position, control, &device, &index, &desc)) {
			return false;
		}
		try {
			// THE BOX'S OWN PRESS HANDLER, handed a second click — nothing here restates what a reset does.
			ui::DeviceControl knob(self->testDeviceHost_, device, *desc, index);
			ui::ValueBox box(knob, "");
			visage::MouseEvent e;
			e.position = {1.0f, 1.0f};
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = visage::kMouseButtonLeft;
			e.is_down = true;
			e.repeat_click_count = 2;
			box.processMouseDown(e);
			return true;
		}
		catch (...) {
			return false;
		}
	}

	static bool testChainAbandonDrag(const clap_plugin* plugin, uint32_t position, const char* control,
	                                 double value) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(position, control, &device, &index, &desc)) {
			return false;
		}
		ui::DeviceControl knob(self->testDeviceHost_, device, *desc, index);
		knob.beginGesture();
		knob.setValue(value);
		return true; // and the control goes out of scope with the hand still down
	}

	static int32_t testChainParamIndexOf(const clap_plugin* plugin, uint32_t id) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return -1;
		}
		return fromClap(plugin)->getParamIndexForParamId(id);
	}
	static uint64_t testChainParamInfoBuilds(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		return fromClap(plugin)->infoBuilds_;
	}

	static bool testChainReleaseControl(const clap_plugin* plugin, uint32_t slot, const char* control, double value) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || control == nullptr || (slot & CLAP_FEATHERS_TEST_STRIP) == 0) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId id = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (!self->laneControlForTest(slot, control, &id, &index, &desc)) {
			return false;
		}
		self->bridge_.releaseDeviceControl(id, index, static_cast<float>(value));
		return true;
	}

	static uint32_t testChainAddModDevice(const clap_plugin* plugin, const char* tag) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || tag == nullptr) {
			return UINT32_MAX;
		}
		const size_t slot = fromClap(plugin)->bridge_.addRackDevice(tag);
		return slot >= kMaxRackSlots ? UINT32_MAX : static_cast<uint32_t>(slot);
	}

	static uint32_t testChainMaxFrames(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		return static_cast<uint32_t>(fromClap(plugin)->synth_.laneForTest().engineForTest().maxFrames());
	}

	static uint64_t testChainPrepareCount(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		return fromClap(plugin)->synth_.laneForTest().engineForTest().preparesForTest();
	}

	static bool testChainDeviceReading(const clap_plugin* plugin, uint32_t position, uint32_t index, float* out) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || out == nullptr) {
			return false;
		}
		const ParamBridge& bridge = fromClap(plugin)->bridge_;
		if (index >= bridge.deviceReadingCount(position)) {
			return false;
		}
		*out = bridge.deviceReading(position, index);
		return true;
	}

	static bool testChainSetCoreRoute(const clap_plugin* plugin, uint32_t source, uint32_t destination, double amount) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		ParamBridge& bridge = fromClap(plugin)->bridge_;
		const auto s = backplane::Source::core(source);
		const auto d = backplane::Destination::core(destination);
		bridge.setRouteAmount(s, d, static_cast<float>(amount));
		return bridge.routeAmount(s, d) == static_cast<float>(amount);
	}

	static uint32_t testChainEngineRouteCount(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		return static_cast<uint32_t>(fromClap(plugin)->params_.stagedRoutes().size());
	}

	static double testChainEngineCoreRouteAmount(const clap_plugin* plugin, uint32_t source, uint32_t destination) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0.0;
		}
		for (const backplane::Route& r : fromClap(plugin)->params_.stagedRoutes()) {
			if (r.source == backplane::Source::core(source) && r.destination == backplane::Destination::core(destination)) {
				return r.amount;
			}
		}
		return 0.0;
	}

	static bool testChainSetDeviceRoute(const clap_plugin* plugin, uint32_t stripSlot, uint32_t output, uint32_t position,
	                                    const char* control, double amount) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || control == nullptr || stripSlot >= kMaxRackSlots) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		DeviceInstanceId target = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		const DeviceInstanceId source = self->bridge_.rackDeviceAt(stripSlot);
		if (control[0] == '#') { // a HOST destination by number: the synth's own voice parameters
			if (source == kNoInstance) {
				return false;
			}
			const auto s = backplane::Source::output(source, output);
			const auto d = backplane::Destination::core(static_cast<size_t>(std::atoi(control + 1)));
			self->bridge_.setRouteAmount(s, d, static_cast<float>(amount));
			return self->bridge_.routeAmount(s, d) == static_cast<float>(amount);
		}
		if (source == kNoInstance || !self->laneControlForTest(position, control, &target, &index, &desc)) {
			return false;
		}
		const auto s = backplane::Source::output(source, output);
		const auto d = backplane::Destination::control(target, index);
		self->bridge_.setRouteAmount(s, d, static_cast<float>(amount));
		return self->bridge_.routeAmount(s, d) == static_cast<float>(amount);
	}

	static bool testChainDeliveredNote(const clap_plugin* plugin, uint32_t index, uint32_t* kind, int32_t* key,
	                                   uint64_t* sample) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		Synth& synth = fromClap(plugin)->synth_;
		if (index == UINT32_MAX) {
			synth.clearDeliveredForTest();
			return true;
		}
		const std::vector<Synth::DeliveredNote> log = synth.deliveredForTest();
		if (index >= log.size()) {
			return false;
		}
		if (kind != nullptr) {
			*kind = static_cast<uint32_t>(log[index].event.kind);
		}
		if (key != nullptr) {
			*key = log[index].event.key;
		}
		if (sample != nullptr) {
			*sample = log[index].sample;
		}
		return true;
	}

	static uint32_t testChainRouteRoomRefusals(const clap_plugin* plugin) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return 0;
		}
		const FeathersPlugin* self = fromClap(plugin);
		return static_cast<uint32_t>(self->params_.routeRoomRefusals() + self->synth_.routeRoomRefusals());
	}

	static bool testChainDeviceBody(const clap_plugin* plugin, uint32_t position, const clap_ostream* out) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || out == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		const DeviceInstanceId id = self->deviceAtForTest(position);
		const device::Descriptor* desc = self->bridge_.descriptorOf(id);
		if (desc == nullptr || !desc->hasBody()) {
			return false;
		}
		const ContainerBodies& bodies = self->bridge_.fxBodies();
		std::vector<uint8_t> body;
		bool taken = false;
		if (const void* surface = bodies.surfaceOf(id); surface != nullptr) {
			taken = rack::bodyOf(desc, surface, &body) && !body.empty();
		}
		if (!taken) {
			const std::span<const uint8_t> stored = bodies.bodyFor(id);
			body.assign(stored.begin(), stored.end());
		}
		return body.empty() || out->write(out, body.data(), body.size()) == static_cast<int64_t>(body.size());
	}

	static bool testChainSetDeviceBlock(const clap_plugin* plugin, uint32_t position, const char* key, const float* floats,
	                                    uint64_t count) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || key == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		const DeviceInstanceId id = self->deviceAtForTest(position);
		const device::Descriptor* desc = self->bridge_.descriptorOf(id);
		if (desc == nullptr || desc->state == nullptr) {
			return false;
		}
		const size_t index = device::blockIndexOf(desc->state, key);
		BridgeDeviceHost host { self->bridge_ }; // THE EDITOR'S ROAD: the window's hand writes go through exactly this
		return host.setBlock(id, index, std::span<const float>(floats, static_cast<size_t>(count)));
	}

	static bool testChainEditDeviceBody(const clap_plugin* plugin, uint32_t position, const void* data,
	                                    uint64_t size) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr) {
			return false;
		}
		FeathersPlugin* self = fromClap(plugin);
		const DeviceInstanceId id = self->deviceAtForTest(position);
		const device::Descriptor* desc = self->bridge_.descriptorOf(id);
		if (desc == nullptr || desc->bodyLoader() == nullptr) {
			return false;
		}
		void* surface = self->bridge_.fxBodies().surfaceOf(id);
		return surface != nullptr
		       && desc->bodyLoader()(surface, static_cast<const uint8_t*>(data), static_cast<size_t>(size));
	}

	static bool copyOut(const std::string& s, char* out, uint32_t capacity) {
		if (out == nullptr || capacity == 0 || s.size() + 1 > capacity) {
			return false;
		}
		std::memcpy(out, s.c_str(), s.size() + 1);
		return true;
	}
	static bool testChainSetPresetIdentity(const clap_plugin* plugin, const char* name, const char* category) {
		FeathersPlugin* self = fromClap(plugin);
		self->bridge_.setPresetIdentity(PresetIdentity { name != nullptr ? name : "", category != nullptr ? category : "" });
		return true;
	}
	static bool testChainPresetIdentity(const clap_plugin* plugin, char* nameOut, uint32_t nameCapacity, char* categoryOut,
	                                    uint32_t categoryCapacity) {
		FeathersPlugin* self = fromClap(plugin);
		const PresetIdentity& id = self->bridge_.presetIdentity();
		return copyOut(id.name, nameOut, nameCapacity) && copyOut(id.category, categoryOut, categoryCapacity);
	}
	static bool testChainPresetFace(const clap_plugin* plugin, char* nameOut, uint32_t capacity) {
		FeathersPlugin* self = fromClap(plugin);
		ui::PresetBrowser* browser = self->gui_.presetBrowserForTest();
		if (browser == nullptr) {
			return false;
		}
		const int at = browser->lastTouched();
		return copyOut(at >= 0 ? browser->currentName() : std::string {}, nameOut, capacity);
	}

	static bool testChainSetDevicePreset(const clap_plugin* plugin, uint32_t position, const char* name,
	                                     const char* category) {
		FeathersPlugin* self = fromClap(plugin);
		const DeviceInstanceId id = self->deviceAtForTest(position);
		if (self->bridge_.descriptorOf(id) == nullptr) {
			return false;
		}
		// WHERE THE PANEL'S ROAD (`BridgeDeviceHost::setDevicePreset`) LANDS.
		self->bridge_.setDevicePreset(id, PresetIdentity { name != nullptr ? name : "", category != nullptr ? category : "" });
		return true;
	}
	static bool testChainDevicePreset(const clap_plugin* plugin, uint32_t position, char* nameOut, uint32_t nameCapacity,
	                                  char* categoryOut, uint32_t categoryCapacity) {
		FeathersPlugin* self = fromClap(plugin);
		const DeviceInstanceId id = self->deviceAtForTest(position);
		if (self->bridge_.descriptorOf(id) == nullptr) {
			return false;
		}
		const PresetIdentity p = self->bridge_.devicePreset(id);
		return copyOut(p.name, nameOut, nameCapacity) && copyOut(p.category, categoryOut, categoryCapacity);
	}

	static bool testChainVoiceReading(const clap_plugin* plugin, uint32_t slot, uint32_t what, double* out) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || out == nullptr || slot >= static_cast<uint32_t>(kVoiceSlots)) {
			return false;
		}
		const Synth& synth = fromClap(plugin)->synth_;
		const int s = static_cast<int>(slot);
		switch (what) {
		case CLAP_FEATHERS_VOICE_PITCH: *out = synth.voicePitchForTest(s); return true;
		case CLAP_FEATHERS_VOICE_PHASE: *out = synth.voicePhaseForTest(s); return true;
		case CLAP_FEATHERS_VOICE_LEVEL: *out = static_cast<double>(synth.voiceEdgeLevelForTest(s)); return true;
		case CLAP_FEATHERS_VOICE_STAGE: *out = static_cast<double>(synth.voiceStageForTest(s)); return true;
		case CLAP_FEATHERS_VOICE_KEY: *out = synth.voiceKeyForTest(s); return true;
		case CLAP_FEATHERS_VOICE_NOTE_ID: *out = synth.voiceNoteIdForTest(s); return true;
		default: return false;
		}
	}

	static bool testChainSynthReading(const clap_plugin* plugin, uint32_t what, double* out) noexcept {
		if (plugin == nullptr || plugin->plugin_data == nullptr || out == nullptr) {
			return false;
		}
		return fromClap(plugin)->synth_.soakReadingForTest(what, out);
	}

	const void* extension(const char* id) noexcept override {
		static const clap_plugin_feathers_test_chain kTestChain{
			&FeathersPlugin::testChainAddDevice,
			&FeathersPlugin::testChainSetControl,
			&FeathersPlugin::testChainAvailableDevice,
			&FeathersPlugin::testChainDeviceCount,
			&FeathersPlugin::testChainRenameMacro,
			&FeathersPlugin::testChainTouchSlot,
			&FeathersPlugin::testChainPinSlot,
			&FeathersPlugin::testChainRemoveDevice,
			&FeathersPlugin::testChainUndoRemove,
			&FeathersPlugin::testChainCapturePreset,
			&FeathersPlugin::testChainLoadPreset,
			&FeathersPlugin::testChainReplaceWithFilter,
			&FeathersPlugin::testChainSlotState,
			&FeathersPlugin::testChainEngineControl,
			&FeathersPlugin::testChainKnobValue,
			&FeathersPlugin::testChainDragControl,
			&FeathersPlugin::testChainUndo,
			&FeathersPlugin::testChainResetControl,
			&FeathersPlugin::testChainAbandonDrag,
			&FeathersPlugin::testChainDeviceBody,
			&FeathersPlugin::testChainEditDeviceBody,
			&FeathersPlugin::testChainAddModDevice,
			&FeathersPlugin::testChainMaxFrames,
			&FeathersPlugin::testChainPrepareCount,
			&FeathersPlugin::testChainDeviceReading,
			&FeathersPlugin::testChainSetCoreRoute,
			&FeathersPlugin::testChainEngineRouteCount,
			&FeathersPlugin::testChainEngineCoreRouteAmount,
			&FeathersPlugin::testChainRouteRoomRefusals,
			&FeathersPlugin::testChainSetDeviceRoute,
			&FeathersPlugin::testChainDeliveredNote,
			&FeathersPlugin::testChainSetDeviceBlock,
			&FeathersPlugin::testChainSetPresetIdentity,
			&FeathersPlugin::testChainPresetIdentity,
			&FeathersPlugin::testChainPresetFace,
			&FeathersPlugin::testChainSetDevicePreset,
			&FeathersPlugin::testChainDevicePreset,
			&FeathersPlugin::testChainVoiceReading,
			&FeathersPlugin::testChainReleaseControl,
			&FeathersPlugin::testChainSynthReading,
			&FeathersPlugin::testChainParamIndexOf,
			&FeathersPlugin::testChainParamInfoBuilds,
		};
		if (id != nullptr && std::strcmp(id, CLAP_EXT_FEATHERS_TEST_CHAIN) == 0) {
			return &kTestChain;
		}
		return nullptr;
	}

	ParamSet params_;
	Synth synth_{params_};
	// Outlives the window: the editor can be closed and reopened, and anything queued in between is
	// still delivered.
	ParamBridge bridge_;

	/// **WHAT A C-STYLE CALLBACK CARRIES INSTEAD OF A CAPTURE.** Bus Utility's readings seam is a
	/// function pointer plus a context, so the slot cannot ride in a lambda's closure the way
	/// automod's does; it rides here. One per slot, a member so it outlives every install — a
	/// context pointing at a local would be a dangling pointer the audio thread dereferences every
	/// block, which is the failure this arrangement exists to make impossible rather than unlikely.
	struct ReadingsHop {
		ParamBridge* bridge = nullptr;
		size_t slot = 0;
	};
	/// A POOL (unbounded devices): it grows with the lane on the main thread and no hop ever moves, because a
	/// device holds a pointer to its own. A position past it gets no readings road rather than a stray store.
	backplane::ChunkedPool<ReadingsHop> readingsHops_ { kMaxFxSlots };

	/// **THE HOST SLOTS' VIEW OF THE DEVICES** — the bridge answers what a target is and where it stands.
	struct BridgeControlLookup final : hostslots::ControlLookup {
		explicit BridgeControlLookup(const ParamBridge& b) noexcept : bridge(b) {}
		bool lookup(const hostslots::Target& target, const device::ParamDesc** desc, double* value) const override {
			try {
				return bridge.resolveSlotTarget(target, desc, value);
			}
			catch (...) {
				return false;
			}
		}
		bool textSite(const hostslots::Target& target, hostslots::TextSite* site) const override {
			try {
				return bridge.slotTextSite(target, site);
			}
			catch (...) {
				return false;
			}
		}
		const ParamBridge& bridge;
	};
	BridgeControlLookup slotLookup_ { bridge_ };
	/// The slots as CLAP sees them (`hostslots/slot_host.h`), over the bridge's pool. After `bridge_`.
	hostslots::SlotHost slotHost_ { bridge_.slotPool(), slotLookup_ };
	/// What the pool's renames owe the host, folded into the next `rescanParams` (`serviceSlots`).
	clap_param_rescan_flags slotRescanOwed_ = 0;
	/// Every `paramsInfo` call, counted for the test seam's `param_info_builds`. Main thread, as `paramsInfo` is.
	mutable uint64_t infoBuilds_ = 0;
	/// Inside the preset browser's walk (`PresetRoadScope`): the chains block saves no slots and keeps the pool.
	bool presetRoad_ = false;

	/// THE TEST SEAM'S LAST REMOVAL, held the way `device_edits.h`'s undo holds it — the device and its
	/// routes — so `undo_remove` can put it back under the same id.
	struct RemovedForTest {
		size_t position = 0;
		FxDevice device {};
		std::vector<backplane::Route> routes;
		bool held = false;
	};
	RemovedForTest removedForTest_;
	/// THE TEST SEAM'S KNOB HOST AND UNDO HISTORY (`drag_control`, `undo`, `knob_value`) — the editor's own
	/// adapter over this plugin's bridge, and a history like the editor's. After `bridge_`; the history's
	/// edits hold the host by reference, so the host is declared first and outlives them.
	BridgeDeviceHost testDeviceHost_ { bridge_ };
	ui::EditHistory testHistory_;

	Gui gui_{*this, bridge_};
	int guiFd_ = -1;
	clap_id guiTimer_ = CLAP_INVALID_ID;

	/// Whether this instance has already said what its host offers for transport. See
	/// reportTransportOnce.
	bool reportedTransport_{false};
	/// What the audio thread saw (`reportTransportOnce`), for the main thread to say (`sayTransportOnce`).
	static constexpr uint8_t kTransportSeen = 0x01, kTransportSeconds = 0x02, kTransportBeats = 0x04,
	                         kTransportTempo = 0x08, kTransportNone = 0x10;
	std::atomic<uint8_t> transportSeen_ { 0 };
	bool transportSaid_ = false; // main thread only

	/// What the host has not been told yet. Bounded by the parameter count because it is deduplicated:
	/// every id can be waiting at most once, however long its queue stays full.
	std::array<ParamDebt, kParamCount> debts_{};
	size_t debtCount_ = 0;

	/// Inside a state load's block walk: renames are recorded, not sent (`onMacroRenamed`).
	bool loadingState_ = false;
	/// A rename the host has not been sent a `CLAP_PARAM_RESCAN_INFO` for yet.
	bool infoRescanOwed_ = false;
	/// See `refreshTail`: written on the main thread, read by `tailGet` on either.
	std::atomic<uint32_t> tail_ { 0 };
	/// What the host was last told, and its extension — the audio thread's (`tellTailIfMoved`).
	uint32_t toldTail_ = 0;
	const clap_host_tail* hostTail_ = nullptr;
	/// **THE LATENCY THE HOST WAS LAST TOLD** (backlog JM) — written only by `activate`, where CLAP lets it
	/// change; read by `latencyGet` and by the audio thread, which compares the engine against it.
	uint32_t reportedLatency_ = 0;
	/// Whether this activation has already asked the host for a restart. AUDIO THREAD; `activate` clears it.
	bool restartAsked_ = false;
	/// THE SIDECHAIN INPUT'S PRESENCE (device::SidechainPresence), and the rate it judges a second of silence at.
	device::SidechainPresence sidechain_ {};
	double sidechainRate_ = 48000.0;
	/// **SAMPLES SINCE ANYTHING SOUNDED OR PLAYED** — a voice, or a non-zero output sample. AUDIO THREAD. SLEEP
	/// waits for more of them than the lane is late (the end of `process`), because a latent device holds audio
	/// it has not yet put out: a linear-phase EQ is a pure delay when flat.
	uint64_t quietSamples_ = 0;
	/// The lane latency `quietSamples_` is counting against: when it moves (a Phase toggle, a device added), the
	/// count starts again, measured from the new value.
	uint32_t quietLatency_ = 0;
	/// **THIS INSTANCE'S LINES IN THE EVENT LOG** (plugin/event_log.h). Main thread only, like every caller.
	eventlog::Instance log_ { kDescriptor.id };
	/// What the last load's chains block said while it PARSED, and what restoring it said — the log's detail.
	rack::LoadReport chainsParseReport_ {};
	rack::LoadReport chainsRestoreReport_ {};
	/// **THE HOST'S RENAME SUBSCRIPTION, DECLARED AFTER `bridge_` SO IT IS DESTROYED BEFORE IT** — the
	/// callback captures `this`, and members die in reverse order. (The token would be safe the other
	/// way round too, `subscribers.h` says why; the order is so that it never has to be.)
	device::Subscription macroRenamed_;
};

//-------//
// Entry //
//-------//

uint32_t CLAP_ABI factoryPluginCount(const clap_plugin_factory*) {
	return 1;
}

const clap_plugin_descriptor* CLAP_ABI factoryPluginDescriptor(const clap_plugin_factory*,
                                                               uint32_t index) {
	return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin* CLAP_ABI factoryCreatePlugin(const clap_plugin_factory*, const clap_host* host,
                                                const char* pluginId) {
	if (!clap_version_is_compatible(host->clap_version) || std::strcmp(pluginId, kDescriptor.id) != 0) {
		return nullptr;
	}
	// clap-helpers deletes this from clap_plugin.destroy.
	auto* plugin = new FeathersPlugin(host);
	return plugin->clapPlugin();
}

const clap_plugin_factory kPluginFactory = {
    factoryPluginCount,
    factoryPluginDescriptor,
    factoryCreatePlugin,
};

} // namespace

// External linkage, unlike everything above it: the clap_entry structure these three fill is built
// in feathers_entry.cpp, once per plugin format. See feathers_entry.h.
bool CLAP_ABI entryInit(const char*) {
	eventlog::arm(); // a host loaded us: the default log file is armed (plugin/event_log.h)
	return true;
}

void CLAP_ABI entryDeinit() {}

const void* CLAP_ABI entryGetFactory(const char* factoryId) {
	return std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kPluginFactory : nullptr;
}

} // namespace feathers
