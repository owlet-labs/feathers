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

#include "rack/rack_clap.h"

#include "plugin/clap_checking.h"
#include "plugin/event_log.h"
#include "plugin/state_streams.h"
#include "state/state_trailer.h"

#include "plugin/denormal_guard.h"
#ifdef FEATHERS_DIAGNOSTIC
#include "plugin/spike_threshold.h"
#include <chrono>
#include <limits>
#endif

#include "composite/composite_types.h"
#include "device/sidechain.h"
#include "plugin/device_row.h" // `belongsInModulationRack`: what the Note Rack offers
#include "device/transport_snap.h" // the host's position put on its sample (`device::onSample`)
#include "device/chain_tail.h"
#include "backplane/block_context.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_test_chain.h"
#include "rack/rack_entry.h"
#include "rack/rack_bridge.h"
#include "rack/rack_gui.h"
#include "plugin/ui/preset_browser.h" // the test chain's `preset_face` (backlog QC)
#include "rack/rack_state.h"
#include "rack/rack_test_ext.h"
#include "hostslots/slot_host.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/value_box.h"

#include "clap/helpers/plugin.hh"
#include "rack/device_publishers.h"

#include "clap/helpers/plugin.hxx"

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

// THE RACK'S CLAP BINDING — audio ports, eight macros, 64 host slots, and the state block.
//
// DELIBERATELY THIN, exactly as `feathers_clap.cpp` is: every decision about what a device IS lives
// in the vocabulary, and every decision about what the chain DOES lives in `RackEngine`, which a test
// can drive with no host at all.
//
//------------------------------------------------------------------------------------------------//
// EIGHT MACROS, THEN 64 HOST SLOTS                                                                 //
//------------------------------------------------------------------------------------------------//
//
// A device's
// controls live in the backplane; the DAW saw eight knobs and a bypass.
//
// **RETIRED FOR SLOTS ONLY, 2026-09-25** (backlog JU; by design the standalone rack owns a pool:
// 8 macros, then 64 slots). Recording automation by turning a
// device's knob is a requirement (design), and a control only the backplane can reach cannot record. So the rack declares 64
// HOST SLOTS after its macros (ids `0x1000 + n`, `hostslots/slot_host.h`), each a 0..1 parameter mapped at
// run time to one device control — by a gesture, a touch, or a default — and renamed to it. The pool is the
// top `RackBridge`'s (`RackBridge::HostSlots::OwnPool`), the same `rack::SlotOwner` Feathers' bridge holds.
// What the 2026-08-30 decision was against — the surface GROWING with the tenants — is still refused: the
// count is fixed, and a slot is a pre-declared id that a mapping only renames.
//
// THE MACROS ARE UNCHANGED, and still ship mapped to nothing. A macro pre-wired to something would be a
// control whose effect depends on which device happens to be in a slot, so one automation lane would mean
// different things in two patches — and a macro that appears to do something because of a route the user
// did not draw is a control lying about where its effect comes from. (A host slot is not that: it is named
// for the one control it drives, and its lane is keyed by an id the pool anchors — design §5.)
//
// THE PARAMETER LIST IS STILL FIXED-SIZE, which is what CLAP wants: `clap/ext/params.h:328` is emphatic
// that `CLAP_PARAM_RESCAN_ALL` requires a deactivated plugin, so a surface that grew with the chain would
// cost an audio dropout every time a device was dropped in. Eight plus 64, always.
//
// **THE LIST IS FIXED; A NAME IS NOT** (macro A9, and every slot). The user may name a macro, and a slot
// takes its control's name; the host is told with `CLAP_PARAM_RESCAN_INFO` — the flag `params.h` gives a
// name change, which "takes effect immediately" and needs no deactivation. The ids and the count never
// move, and a DAW keys automation by id, so a lane drawn on "Macro 1" plays on after it is called "Cutoff".

namespace feathers::rack {

namespace {

// The checking level: `plugin/clap_checking.h`, shared with Feathers.
using PluginBase = clap::helpers::Plugin<feathers::kClapMisbehaviour, feathers::kClapChecking>;

const char* const kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor kDescriptor = {
    CLAP_VERSION_INIT,
    "com.owlet.feathers-chain",
    "Feathers Chain",
    "Owlet Records",
    "https://owletrecords.com",
    "",
    "",
    FEATHERS_VERSION,
    "A chain of Feathers devices, with a modulation backplane",
    kFeatures,
};

// **THE NOTE RACK — THE SAME ENGINE AND EDITOR, AS A NOTE EFFECT**. `note-effect` is CLAP's word for a plugin that processes notes, and it is what a
// host files under Note FX (Bitwig places such a plugin before an instrument). Note in and note out, no audio; its
// offer is the modulators and the note effects. A second plugin in this bundle, so it installs with it.
const char* const kNoteRackFeatures[] = {
    CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    nullptr,
};

const clap_plugin_descriptor kNoteRackDescriptor = {
    CLAP_VERSION_INIT,
    "com.owlet.feathers.noterack",
    "Feathers Note Rack",
    "Owlet Records",
    "https://owletrecords.com",
    "",
    "",
    FEATHERS_VERSION,
    "Feathers' arps, patterns and modulators as a note effect, before any instrument",
    kNoteRackFeatures,
};

// THE RACK'S SHAPE lives in rack/rack_axes.h now: a rack inside a chain is this same engine and must
// be built with the same shape.


// **THE RACK'S CLAP SURFACE IS THE COMPOSITE SURFACE, PARTIALLY.** A rack presents its macros to a
// DAW exactly as a composite device presents them to a chain that holds one, so the count and the
// names come from one declaration rather than two that happen to agree.
//
// PARTIALLY, and the gap is named because it is the next commit's: `composite::kDescriptor` also
// declares Enabled, Mix and Gain, which this surface does not offer and the engine does not have.
// Widening the CLAP list is a change to what
// Bitwig sees and belongs with the engine that implements them, not here.
constexpr size_t kMacroCount = composite::kMacroCount;

const char* macroName(size_t i) {
	return i < kMacroCount
	         ? composite::kParamDescs[static_cast<size_t>(composite::Param::Macro1) + i].name
	         : "Macro";
}

class RackPlugin : public PluginBase {
public:
	/// ONE CLAP NOTE EVENT, TRANSLATED AND KEPT FOR THIS BLOCK.
	///
	/// A COPY RATHER THAN A DECISION: `device::NoteEvent` is shaped after CLAP's own event precisely so
	/// this function has nothing to choose. A field it dropped would be one no device could ever see,
	/// and the host would be the thing that lost it.
	void collectNote(device::NoteEvent::Kind kind, const clap_event_header* header) noexcept {
		if (noteCount_ >= notesIn_.size()) {
			// FULL. Dropping the LATEST is wrong for the same reason `NoteOps` gives — a lost note-off
			// is a stuck note — but a host sending 256 notes in one block has already left the
			// territory this rack is for, and silently growing on the audio thread is not the answer.
			return;
		}
		const auto* e = reinterpret_cast<const clap_event_note*>(header);
		device::NoteEvent& out = notesIn_[noteCount_++];
		out = device::NoteEvent {}; // a slot an expression used last block keeps none of its fields
		out.kind = kind;
		out.timeSamples = header->time;
		out.noteId = e->note_id;
		out.port = e->port_index;
		out.channel = e->channel;
		out.key = e->key;
		out.velocity = e->velocity;
	}

	/// **A MIDI 1.0 NOTE, DECODED INTO THE SAME STREAM** as `collectNote`'s: note id -1 (MIDI has none), the
	/// channel the status byte's low nibble, velocity the data byte over 127, and a note-on of velocity 0 a
	/// note-off. NOTES ONLY: the rack has no pitch-bend or sustain consumer, so those, the other CCs and the
	/// rest of MIDI are ignored rather than invented.
	void collectMidi(const clap_event_midi* e, uint32_t time) noexcept {
		const uint8_t status = e->data[0];
		const uint8_t type = status & 0xF0;
		if (type != 0x90 && type != 0x80) {
			return;
		}
		if (noteCount_ >= notesIn_.size()) {
			return;
		}
		const uint8_t vel = e->data[2] & 0x7F;
		device::NoteEvent& out = notesIn_[noteCount_++];
		out = device::NoteEvent {};
		const bool on = type == 0x90 && vel != 0;
		out.kind = on ? device::NoteEvent::Kind::On : device::NoteEvent::Kind::Off;
		out.timeSamples = time;
		out.noteId = -1;
		out.port = e->port_index;
		out.channel = static_cast<int16_t>(status & 0x0F);
		out.key = static_cast<int16_t>(e->data[1] & 0x7F);
		out.velocity = (on || type == 0x80) ? static_cast<double>(vel) / 127.0 : 0.0;
	}

	/// **A NOTE EXPRESSION, IN THE SAME STREAM AS ITS NOTE** — `device::NoteEvent`'s own model, which the
	/// rack did not take: it had no case for `CLAP_EVENT_NOTE_EXPRESSION`, so a Note Expressions modulator
	/// in the rack sat at neutral under an MPE controller and no keytracking device followed a per-note
	/// bend. Feathers has taken them since backlog DS. Copied, not interpreted, as `collectNote` copies.
	void collectExpression(const clap_event_header* header) noexcept {
		if (noteCount_ >= notesIn_.size()) {
			return;
		}
		const auto* e = reinterpret_cast<const clap_event_note_expression*>(header);
		device::NoteEvent& out = notesIn_[noteCount_++];
		out = device::NoteEvent {};
		out.kind = device::NoteEvent::Kind::Expression;
		out.timeSamples = header->time;
		out.noteId = e->note_id;
		out.port = e->port_index;
		out.channel = e->channel;
		out.key = e->key;
		out.velocity = 0.0;
		out.expressionId = e->expression_id;
		out.expressionValue = e->value;
	}
	static_assert(RackEngine::kTuningExpression == CLAP_NOTE_EXPRESSION_TUNING,
	              "the engine folds CLAP's tuning expression into the rack's note by its CLAP id");

	/// WHAT LEFT THE CHAIN, HANDED BACK TO THE HOST.
	///
	/// **PASS-THROUGH IS WHY THIS IS NOT CONDITIONAL.** A rack containing one filter emits exactly
	/// what it was given; a rack containing an arp emits what the arp made. Either way the port has
	/// something honest to carry, which is what lets it exist at all times rather than flickering with
	/// the chain's contents — a route that vanishes when a device is removed is one the player has to
	/// rebuild.
	void emitNotes(const clap_output_events* out) noexcept {
		if (out == nullptr) {
			return;
		}
		for (const device::NoteEvent& n : engine_.blockNotesOut()) {
			// **AN EXPRESSION LEAVES AS AN EXPRESSION.** Every kind that was not On or Off went out as a
			// CHOKE, which was harmless only while no expression could arrive: one passing through would
			// have choked its note in whatever follows the rack.
			if (n.kind == device::NoteEvent::Kind::Expression) {
				clap_event_note_expression x {};
				x.header.size = sizeof(x);
				x.header.time = n.timeSamples;
				x.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				x.header.type = CLAP_EVENT_NOTE_EXPRESSION;
				x.header.flags = 0;
				x.expression_id = n.expressionId;
				x.note_id = n.noteId;
				x.port_index = n.port;
				x.channel = n.channel;
				x.key = n.key;
				x.value = n.expressionValue;
				out->try_push(out, &x.header);
				continue;
			}
			clap_event_note e {};
			e.header.size = sizeof(e);
			e.header.time = n.timeSamples;
			e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			e.header.type = n.kind == device::NoteEvent::Kind::On    ? CLAP_EVENT_NOTE_ON
			                : n.kind == device::NoteEvent::Kind::Off ? CLAP_EVENT_NOTE_OFF
			                                                        : CLAP_EVENT_NOTE_CHOKE;
			e.header.flags = 0;
			e.note_id = n.noteId;
			e.port_index = n.port;
			e.channel = n.channel;
			e.key = n.key;
			e.velocity = n.velocity;
			out->try_push(out, &e.header);
		}
	}

	RackPlugin(const clap_host* host, bool noteRack)
	    : PluginBase(noteRack ? &kNoteRackDescriptor : &kDescriptor, host), noteRack_(noteRack) {
		// THE NOTE RACK OFFERS MODULATORS AND NOTE EFFECTS ONLY — no audio effect in a rack with no audio.
		if (noteRack_) {
			gui_.offerFilter = [](const device::Descriptor& d) { return belongsInModulationRack(d); };
		}
		log_.say("create");
		// **WHO WANTS A DEVICE'S LIVE OUTPUT SAYS SO HERE, NOT IN THE ENGINE.** The engine calls this
		// once per device as it is constructed and matches nothing itself — the same boundary
		// `FxRunner::setDspConnector` settled on after `setOwlttDspConnector` put the question "which
		// devices publish" inside the transport.
		// MOVED OUT OF THIS CONSTRUCTOR AND INTO `device_publishers.cpp`, where a test can reach it.
		// It answered for OWLTT and not for automod, and no suite could ask: this TU's only suite cannot
		// run headless. See that file's header for the two-list argument.
		installDevicePublishers(engine_);
		// **THE HOST HEARS A MACRO'S NEW NAME** (macro A9) — Feathers' shape: see `onMacroRenamed`.
		macroRenamed_ = bridge_.subscribeMacroRename([this](size_t) { onMacroRenamed(); });
		// A FIRST GRAB IS SERVICED AT ITS BEGIN (`RackBridge::beginControlGesture`), as Feathers' is.
		bridge_.setFirstGrabService([this] { serviceSlots(); });
	}

	/// A HOST THAT DESTROYS US SAYS SO; a process that dies says so by this line's absence.
	~RackPlugin() override {
		log_.say("destroy");
#ifdef FEATHERS_DIAGNOSTIC
		if (spikeTimer_ != CLAP_INVALID_ID) {
			_host.timerSupportUnregister(spikeTimer_);
		}
#endif
	}

	//--------------------------------------------------------------------------------------------//
	// AUDIO PORTS — one stereo in, one stereo out. A rack is a wire with devices on it.            //
	//--------------------------------------------------------------------------------------------//

	bool implementsAudioPorts() const noexcept override { return true; }
	// AND A SIDECHAIN IN (design §10.1): input 1, NON-main stereo, for the devices that listen to one (the Spectral vocoder's
	// modulator; the main input is the carrier). Static, so no rescan is ever asked.
	// **THE NOTE RACK HAS NONE**: a note effect carries notes, not
	// audio, as Bitwig's own Note FX devices do; and a plugin with audio ports risks being offered as an audio effect. Lead's
	// call, to be confirmed, once it has been placed in Bitwig.
	uint32_t audioPortsCount(bool isInput) const noexcept override { return noteRack_ ? 0 : (isInput ? 2 : 1); }
	bool audioPortsInfo(uint32_t index, bool isInput,
	                    clap_audio_port_info* info) const noexcept override {
		if (info == nullptr) {
			return false;
		}
		if (isInput && index == 1) {
			info->id = 2;
			std::snprintf(info->name, sizeof(info->name), "Sidechain");
			info->flags = 0; // not main: an aux input
			info->channel_count = 2;
			info->port_type = CLAP_PORT_STEREO;
			info->in_place_pair = CLAP_INVALID_ID;
			return true;
		}
		if (index != 0) {
			return false;
		}
		info->id = isInput ? 0 : 1;
		std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "In" : "Out");
		info->flags = CLAP_AUDIO_PORT_IS_MAIN;
		info->channel_count = 2;
		info->port_type = CLAP_PORT_STEREO;
		info->in_place_pair = isInput ? 1 : 0;
		return true;
	}

	//--------------------------------------------------------------------------------------------//
	// NOTE PORTS — an input only, and only so the modulators have a gate.                          //
	//--------------------------------------------------------------------------------------------//
	//
	// A RACK HAS NO NOTE STACK OF ITS OWN, which `RackEngine::setGate` says outright. An effect that
	// accepts notes is unusual but not odd: it is how an envelope in the rack learns that anything is
	// playing, and CLAP allows a note port on an effect precisely for this.
	// **AND AN OUTPUT, WHICH EXISTS AT ALL TIMES BECAUSE NOTES PASS THROUGH.**
	//
	// A rack holding one filter
	// emits exactly what it was given — ordinary MIDI-thru — and one holding an arp emits what the arp
	// made. Either way there is something honest to carry.
	//
	// THE FIRST DESIGN HAD THIS PORT APPEAR ONLY WHEN A NOTE-PRODUCING DEVICE WAS PRESENT, on the
	// grounds that an empty route is a lie. It had it backwards: a port that flickers with the chain's
	// contents is a route that VANISHES when a device is removed, and the player has to rebuild it.
	// It would also have needed `clap_host_note_ports::rescan` on every chain edit, for nothing.
	bool implementsNotePorts() const noexcept override { return true; }
	uint32_t notePortsCount(bool) const noexcept override { return 1; }
	bool notePortsInfo(uint32_t index, bool isInput,
	                   clap_note_port_info* info) const noexcept override {
		if (index != 0 || info == nullptr) {
			return false;
		}
		info->id = isInput ? 0 : 1;
		info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
		info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
		// NAMED FOR WHAT IT CARRIES NOW RATHER THAN WHAT IT WAS FOR. "Gate" was honest when a note's
		// only survivor was a boolean; the port now carries whole notes to any device that wants them.
		std::snprintf(info->name, sizeof(info->name), isInput ? "Notes In" : "Notes Out");
		return true;
	}

	//--------------------------------------------------------------------------------------------//
	// PARAMS — the eight macros, then the 64 host slots (T10; see the header).                     //
	//--------------------------------------------------------------------------------------------//

	bool implementsParams() const noexcept override { return true; }
	/// FIXED WHILE ACTIVE, as every count must be: the slots are declared whether or not anything is mapped to
	/// them, and a mapping only renames. Feathers' shape (`FeathersPlugin::paramsCount`).
	uint32_t paramsCount() const noexcept override {
		return static_cast<uint32_t>(kMacroCount) + hostslots::kDeclaredSlots;
	}

	// MAIN THREAD, as CLAP guarantees for `params.get_info` — which is what lets it read the macro names, mutable
	// state the main thread owns (the bridge renames on it), with no lock.
	/// **A PARAM ID'S INDEX IN O(1)**, for `FeathersPlugin::getParamIndexForParamId`'s reason: clap-helpers asks it on
	/// every value and text call, and its own version walks `paramsInfo` over every param. A macro's id is its index;
	/// the slots follow, in slot order. -1 for anything else, as the base says.
	int32_t getParamIndexForParamId(clap_id paramId) const noexcept override {
		if (paramId < kMacroCount) {
			return static_cast<int32_t>(paramId);
		}
		if (hostslots::isSlotId(paramId)) {
			return static_cast<int32_t>(kMacroCount + (paramId - hostslots::kFirstSlotId));
		}
		return -1;
	}

	bool paramsInfo(uint32_t index, clap_param_info* info) const noexcept override {
		++infoBuilds_;
		if (info == nullptr) {
			return false;
		}
		if (index >= kMacroCount) {
			// PAST THE MACROS ARE THE SLOTS, in slot order; the pool (on the bridge) says what each names.
			return slotHost_.info(static_cast<hostslots::SlotIndex>(index - kMacroCount), info);
		}
		*info = clap_param_info {};
		info->id = index;
		info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
		info->min_value = 0.0;
		info->max_value = 1.0;
		info->default_value = 0.0;
		std::snprintf(info->name, sizeof(info->name), "%s", paramDisplayName(index).c_str());
		std::snprintf(info->module, sizeof(info->module), "Macros");
		return true;
	}

	/// **WHAT THE HOST CALLS PARAMETER `index` — THE ONE LOOKUP**, as `FeathersPlugin::paramDisplayName`
	/// is: the user's name for the macro if it has one, else the composite table's.
	std::string paramDisplayName(uint32_t index) const {
		return bridge_.macroRenamed(index) ? bridge_.macroName(index) : std::string(macroName(index));
	}

	/// **A NAME MOVED: THE HOST IS OWED A `CLAP_PARAM_RESCAN_INFO`** — sent at once for a rename, and once
	/// at the end of a state load however many macros the patch renamed (`FeathersPlugin::onMacroRenamed`
	/// has the argument).
	void onMacroRenamed() noexcept {
		infoRescanOwed_ = true;
		if (!loadingState_) {
			rescanParams(0);
		}
	}

	/// **THE ONE CALL TO `paramsRescan`** — `FeathersPlugin::rescanParams`: `flags`, plus INFO if a macro rename
	/// is owed, plus whatever the host slots' renames owe (`serviceSlots`: VALUES|INFO|TEXT) — so a load that renames
	/// macros AND restores slots costs the host ONE rescan, folded into the load's own VALUES|INFO. Nothing if
	/// all are empty, or to a host with no params extension.
	void rescanParams(clap_param_rescan_flags flags) noexcept {
		flags |= std::exchange(slotRescanOwed_, 0u);
		if (std::exchange(infoRescanOwed_, false)) {
			flags |= CLAP_PARAM_RESCAN_INFO;
		}
		if (flags != 0 && _host.canUseParams()) {
			_host.paramsRescan(flags);
		}
	}

	//--------------------------------------------------------------------------------------------//
	// THE HOST SLOTS' MAIN-THREAD SERVICE (plan §1.3)                                               //
	//--------------------------------------------------------------------------------------------//

	/// **THE SLOTS' SHARE OF A MAIN-THREAD VISIT** — Feathers' `serviceSlots`, on the rack's bridge: what
	/// stopped resolving is freed, host writes and recording touches are anchored, the bindings are rebuilt
	/// and the host's values mirrored into the record (`RackBridge::serviceHostSlots`); then whatever the pool
	/// renamed is owed as ONE `RESCAN_VALUES | RESCAN_INFO | RESCAN_TEXT`, sent now or, inside a state load, recorded for the
	/// load's own rescan. Cheap when nothing moved: two compares and two bit-takes.
	void serviceSlots() noexcept {
		(void)bridge_.serviceHostSlots();
		try {
			slotRescanOwed_ |= slotHost_.rescanOwed();
		}
		catch (...) {
			// Out of memory taking the renamed set: the renames stay owed in the pool for the next visit.
		}
		if (!loadingState_) {
			rescanParams(0);
		}
	}

	/// **THE HOST'S "CALL ME ON THE MAIN THREAD"** (`clap_host::request_callback`), which a host write on a slot
	/// asks for where no editor is ticking. Nothing else in the rack requests it, so this is the slots' visit —
	/// and, with no editor ticking, the one that tells the host a slot the visit NEWLY anchored made the patch
	/// dirty (`RackBridge::serviceHostSlots` marks it; `markDirtyIfEdited` passes it on).
	void onMainThread() noexcept override {
		serviceSlots();
		if (!loadingState_) {
			markDirtyIfEdited();
		}
	}

	/// **THE HOST WROTE SLOT `n`** — AUDIO THREAD in `process`; in `params.flush` whichever thread the host
	/// flushes on, which while the rack sleeps is the ONLY road a host write takes (plan §0.13). The owner
	/// denormalises against the slot's published declaration and writes the bus (`SlotOwner::onHostValue`);
	/// the engine applies it at the top of its next block, and the visit asked for here anchors and mirrors.
	/// A slot that drives nothing yet is ignored, as Feathers ignores it (its `onHostSlotValue` says why).
	void onHostSlotValue(hostslots::SlotIndex n, double value) noexcept {
		rack::SlotOwner* slots = bridge_.hostSlots();
		if (slots != nullptr) {
			// A WRITE THAT REACHED NO CONTROL still MARKS the id (2026-09-26), so the visit is asked for either way.
			(void)slots->onHostValue(n, value);
			_host.requestCallback();
		}
	}

	/// **THE PATCH STATE THE HOST CANNOT SEE** (backlog JV) — `FeathersPlugin::provisionAndDrain`'s last
	/// step. Nearly all of the rack is not a parameter, so nothing else tells the host an edit happened:
	/// without this a project closed after one is closed without the prompt to save it. Once per
	/// main-thread visit, after the editor's events, which is where every edit is made; main thread is
	/// where `clap_host_state.mark_dirty` must be called from.
	void markDirtyIfEdited() noexcept {
		if (bridge_.takeStateDirty() && _host.canUseState()) {
			_host.stateMarkDirty();
		}
		refreshTail(); // and a chain that gained or lost its last audio device changes what the host is told
	}

	/// **HOW LONG THE RACK MAY SOUND AFTER ITS INPUT STOPS** — `device::chainTailSamples`, the one answer
	/// Feathers gives too: infinite while the chain holds an audio device, zero otherwise. clap-wrapper
	/// reports it as VST3's `getTailSamples`; without it a VST3 host may stop calling the rack once its input
	/// is silent, and a reverb in it is cut where the audio stopped. `process` never answers SLEEP, so a
	/// CLAP host keeps calling it whatever this says.
	bool implementsTail() const noexcept override { return true; }
	/// `[main-thread, audio-thread]`, so it answers from an atomic the main thread computes.
	uint32_t tailGet() const noexcept override { return tail_.load(std::memory_order_relaxed); }

	/// MAIN THREAD: the tail of what the record holds now. The host hears it moved from `process`.
	void refreshTail() noexcept {
		tail_.store(device::chainTailSamples([&](auto&& visit) {
			for (size_t at = 0; at < bridge_.chainLength(); ++at) {
				visit(bridge_.descriptorOf(bridge_.deviceAtPosition(at)));
			}
		}),
		            std::memory_order_relaxed);
	}

	/// **`clap_host_tail.changed` IS `[audio-thread]`**, so the block tells it, once per change. The host's
	/// extension is asked for here rather than through clap-helpers, whose `HostProxy` never fetches it
	/// (`canUseTail` is false against every host — measured: a host offering it was told nothing).
	void tellTailIfMoved() noexcept {
		const uint32_t now = tail_.load(std::memory_order_relaxed);
		if (now != toldTail_) {
			toldTail_ = now;
			if (hostTail_ != nullptr && hostTail_->changed != nullptr) {
				hostTail_->changed(_host.host());
			}
		}
	}

	/// **HOW LATE THE RACK'S OUTPUT IS** (backlog JM) — what the ENGINE
	/// runs, the serial sum of what its devices declare, never what the record describes. Zero while no
	/// device declares one, which is every device that exists today.
	bool implementsLatency() const noexcept override { return true; }
	/// `[main-thread & (being-activated | active)]` — written only by `activate`, so it is the number the
	/// host was last told, not the one the engine has moved to since (that one asks for a restart).
	uint32_t latencyGet() const noexcept override { return reportedLatency_; }

	/// **A LATENCY MAY ONLY CHANGE DURING `activate`; WHILE ACTIVE, THE PLUGIN ASKS FOR A RESTART** (CLAP,
	/// `ext/latency.h`). AUDIO THREAD, after the block: the engine's compensation has already moved (the
	/// branches inside stay aligned), and the host is asked ONCE to deactivate and reactivate, where
	/// `activate` reports the new number. `request_restart` is `[thread-safe]`.
	void askRestartIfLatencyMoved() noexcept {
		if (!restartAsked_ && engine_.latencySamples(engine_.sampleRate()) != reportedLatency_) {
			restartAsked_ = true;
			_host.requestRestart();
		}
	}

	bool paramsValue(clap_id id, double* value) noexcept override {
		// **EVERY PARAMS CALL ASKS `isSlotId` FIRST** — a slot id is no macro, and read as one it would index
		// past the macros rather than fail loudly.
		if (hostslots::isSlotId(id)) {
			return slotHost_.value(id, value);
		}
		if (id >= kMacroCount || value == nullptr) {
			return false;
		}
		// THE BRIDGE'S MIRROR, NOT THE ENGINE — a main-thread read of an audio-thread value, which the
		// mirror's atomics make one; and it carries the value a load or a hand just wrote before the
		// drain has, so a host reading back what it set is not answered from a block ago.
		*value = static_cast<double>(bridge_.macro(id));
		return true;
	}

	bool paramsValueToText(clap_id id, double value, char* out,
	                       uint32_t size) noexcept override {
		if (hostslots::isSlotId(id)) {
			return slotHost_.valueToText(id, value, out, size); // the target's own text: "1.2 kHz", "Saw"
		}
		if (id >= kMacroCount || out == nullptr) {
			return false;
		}
		std::snprintf(out, size, "%.1f %%", value * 100.0);
		return true;
	}

	bool paramsTextToValue(clap_id id, const char* text, double* value) noexcept override {
		if (hostslots::isSlotId(id)) {
			return slotHost_.textToValue(id, text, value);
		}
		if (id >= kMacroCount || text == nullptr || value == nullptr) {
			return false;
		}
		*value = std::atof(text) / 100.0;
		return true;
	}

	void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override {
		drain(in);
		// **THE EDITOR'S CHANNEL, AT THE SECOND OF ITS TWO SITES (AS 2).** This is the one that runs
		// when NO BLOCK DOES — the transport stopped, the plugin inactive — which is the hole 1b
		// surfaced from the other side: an edit with no block to carry it would otherwise wait for
		// audio that may never come. CLAP puts this on the audio thread while active and the main
		// thread while inactive, and guarantees it never overlaps `process`; that guarantee, not a
		// lock, is what makes both sites safe. See editor_lanes.h for the same reading in Feathers.
		bridge_.drain();
		// **AND THE ROAD OUT, WHICH THE RACK DID NOT HAVE BEFORE THE HOST SLOTS** (T10): this site ignored its
		// output queue, because the rack told the host nothing. A hand on a slotted knob while the rack sleeps
		// reaches the host only here (plan §0.13), so the slots' outbox drains at BOTH sites.
		drainHostOutboxes(out);
	}

	/// THE SLOTS' OUTBOX TO THE HOST — `slot_host`'s drain, the one Feathers' two sites call. Begin, Value, End
	/// on the slot's id, with `SlotOutbox`'s debt rules. Audio thread (or the flush's).
	///
	/// **AND THE MACROS' (backlog NG)**: the editor's drag on a macro, Begin, Value, End on the macro's own id
	/// (0..7, `paramsInfo`), with the same debt rules — the road Feathers' `consumeEditorEvents` gives its macros.
	/// Before NG this site sent the slots alone, so a macro moved by hand never reached the host at all.
	void drainHostOutboxes(const clap_output_events* out) noexcept {
		if (rack::SlotOwner* slots = bridge_.hostSlots(); slots != nullptr) {
			hostslots::drainOutbox(slots->outbox(), out);
		}
		if (RackBridge::MacroOutbox* macros = bridge_.macroOutbox(); macros != nullptr) {
			macros->drain([out](hostslots::SlotEventKind kind, uint16_t macro, float value01) noexcept {
				return hostslots::emitParamEvent(out, kind, static_cast<clap_id>(macro), value01);
			});
		}
	}

	//--------------------------------------------------------------------------------------------//
	// AUDIO                                                                                        //
	//--------------------------------------------------------------------------------------------//

	bool init() noexcept override {
		// THE HOST, NAMED: a line from Bitwig and a line from a test host must not read alike.
		const clap_host* host = _host.host();
		log_.say(eventlog::initLine(host != nullptr ? host->name : nullptr, host != nullptr ? host->version : nullptr,
		                            host != nullptr ? host->vendor : nullptr));
		return true;
	}

	void deactivate() noexcept override {
		log_.say("deactivate");
#ifdef FEATHERS_DIAGNOSTIC
		drainSpikes();
#endif
	}

	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) noexcept override {
		log_.say("activate rate=" + std::to_string(static_cast<long>(sampleRate)) + " frames=" + std::to_string(minFrames)
		         + ".." + std::to_string(maxFrames));
		// THROUGH THE BRIDGE: devices built at the old rate and not yet adopted are built again at this one.
		// AND AT THE HOST'S LONGEST BLOCK (backlog GO), which every device sizes its scratch to.
		bridge_.prepare(sampleRate, static_cast<int>(maxFrames));
		// AND EVERY NOTE AN ARP HAD SOUNDING DOWNSTREAM GETS ITS OFF IN THE FIRST BLOCK: the prepare reset the arp, which
		// has forgotten them, and the instrument this rack plays has not.
		engine_.flushInventedNotes();
		sidechainRate_ = sampleRate;
		sidechain_.reset(sampleRate);
		// THE NOTE RACK'S SILENCE, sized to the longest block the host will hand (never resized on the audio thread).
		if (noteRack_) {
			scratchL_.assign(maxFrames, 0.0f);
			scratchR_.assign(maxFrames, 0.0f);
		}
		// **NO BLOCK CAN RUN, SO THIS THREAD TAKES THE DRAIN'S PART** — as `stateLoad` does while inactive —
		// and the latency below is the one of the devices the record holds, not of what the engine held before
		// (backlog JM, assumption L10). The first block would drain the same edits.
		//
		// **AND THE HOST SLOTS' BINDINGS BEFORE IT** (hs-int x JM): a pin or a load since the last visit is
		// bound and committed now, so the record this drain adopts already drives its slots, and the rebound
		// declarations are handed over after that commit (the remap window, `SlotOwner::service`). The bindings
		// only — the renames' rescan and a new anchor's dirty mark stay owed to the next main-thread visit, not
		// sent from inside `activate`.
		(void)bridge_.serviceHostSlots();
		bridge_.drain();
		bridge_.publish(engine_);
		// **THE LATENCY, DECIDED HERE AND ONLY HERE** (CLAP: it may change only during `activate`), and the
		// host told it moved — `clap_host_latency.changed` is `[main-thread & being-activated]`, which is now.
		const uint32_t latency = engine_.latencySamples(sampleRate);
		if (latency != reportedLatency_) {
			reportedLatency_ = latency;
			if (_host.canUseLatency()) {
				_host.latencyChanged();
			}
		}
		restartAsked_ = false;
		refreshTail();
		// THE HOST'S TAIL EXTENSION, asked for once on the main thread; `tellTailIfMoved` uses it from `process`.
		hostTail_ = static_cast<const clap_host_tail*>(_host.host()->get_extension(_host.host(), CLAP_EXT_TAIL));
		toldTail_ = tail_.load(std::memory_order_relaxed); // a host activating us asks for the tail itself
#ifdef FEATHERS_DIAGNOSTIC
		// THE CPU SPIKE RECORDER (backlog SR): the ring, the threshold (editor.conf `spike_threshold`, then
		// `$FEATHERS_SPIKE_THRESHOLD`, as feathers.clap reads it) and the one-second drain timer.
		try {
			spikes_.activate(sampleRate, spike::thresholdFromPrefsAndEnvironment());
		}
		catch (...) {
		}
		spikeFirst_ = true;
		if (spikeTimer_ == CLAP_INVALID_ID && _host.canUseTimerSupport()) {
			(void)_host.timerSupportRegister(1000, &spikeTimer_);
		}
#endif
		return true;
	}

	/// THE HOST'S TRANSPORT AS A BLOCK CONTEXT — `process->transport` at the block's start, and each
	/// `CLAP_EVENT_TRANSPORT` inside it (backlog NJ (b)), read by one rule.
	static backplane::BlockContext contextOf(const clap_event_transport& tp, double sampleRate) noexcept {
		backplane::BlockContext block = backplane::unknownBlock();
		if ((tp.flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && tp.tempo > 0.0) {
			block.clock = device::TransportClock::fromSecondsPerBeat(60.0 / tp.tempo);
		}
		// **WHERE THE SONG IS — backlog DQ.** Both timelines, each only if the host says it has
		// one, because a device derives its phase from the timeline its own period is measured in.
		// Converting one into the other here would collapse that choice before any device sees it.
		//
		// `CLAP_TRANSPORT_IS_PLAYING` is read whatever the timelines say: a stopped host still
		// reports where it is stopped, and "stopped" is what tells a locked LFO to keep running
		// instead of freezing on one position.
		block.clock.playing = (tp.flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
		if ((tp.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0) {
			block.clock.songPositionSeconds =
			    device::onSample(static_cast<double>(tp.song_pos_seconds) / static_cast<double>(CLAP_SECTIME_FACTOR), sampleRate);
		}
		if ((tp.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0) {
			block.clock.songPositionBeats =
			    device::onSample(static_cast<double>(tp.song_pos_beats) / static_cast<double>(CLAP_BEATTIME_FACTOR),
			             block.clock.haveTempo() ? sampleRate * block.clock.secondsPerBeat : 0.0);
		}
		return block;
	}

	clap_process_status process(const clap_process* process) noexcept override {
#ifdef FEATHERS_DIAGNOSTIC
		const SpikeBlock spikeBlock(*this, process); // times the whole call, every return included (backlog SR)
#endif
		// DENORMALS FLUSHED FOR THIS CALL AND THE HOST'S STATE PUT BACK AFTER — see denormal_guard.h.
		const DenormalGuard denormals;
		if (process == nullptr) {
			return CLAP_PROCESS_ERROR;
		}
		transportAnchorCount_ = 0; // a flush's events since the last block are not this block's transport
		{
			const rack::StageScope stage(rack::Stage::Events); // the spike recorder's stages (backlog SR)
			drain(process->in_events);
		}
		tellTailIfMoved();
		// **WHETHER THE HOST IS RECORDING** (host slots, design §5): a hand on a slotted control while it is
		// anchors the slot. A host that offers no transport is not recording anything we can know of.
		bridge_.setHostRecording(process->transport != nullptr
		                         && (process->transport->flags & CLAP_TRANSPORT_IS_RECORDING) != 0);
		// THE SLOTS' OUTBOX, the first of its two sites (`paramsFlush` is the other) — before the block, so a
		// hand's gesture is not held behind a missing output bus below.
		drainHostOutboxes(process->out_events);

		// THE NOTE RACK IS HANDED NO AUDIO: the engine runs on silence of its own, so its notes still move every block.
		const bool scratch = noteRack_ && process->frames_count <= scratchL_.size();
		if (!scratch && (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2)) {
			noteCount_ = 0; // there is no block to deliver them into; left standing they replay with the next one
			return CLAP_PROCESS_CONTINUE;
		}
		float* left = scratch ? scratchL_.data() : process->audio_outputs[0].data32[0];
		float* right = scratch ? scratchR_.data() : process->audio_outputs[0].data32[1];
		const auto frames = static_cast<int>(process->frames_count);
		if (scratch) {
			std::fill_n(left, frames, 0.0f);
			std::fill_n(right, frames, 0.0f);
		}

		// IN PLACE WHEN THE HOST ALLOWS IT, copied when it does not. `in_place_pair` above says we can
		// take either; a host that hands distinct buffers still expects the input to reach the output.
		if (process->audio_inputs_count >= 1 && process->audio_inputs[0].channel_count >= 2) {
			const float* inL = process->audio_inputs[0].data32[0];
			const float* inR = process->audio_inputs[0].data32[1];
			if (inL != left) {
				std::memcpy(left, inL, sizeof(float) * static_cast<size_t>(frames));
			}
			if (inR != right) {
				std::memcpy(right, inR, sizeof(float) * static_cast<size_t>(frames));
			}
		}

		backplane::BlockContext block =
		    process->transport != nullptr ? contextOf(*process->transport, sidechainRate_) : backplane::unknownBlock();
		// **AND EVERY TRANSPORT THE HOST SENT INSIDE THE BLOCK, re-anchoring from its sample on** (backlog NJ (b)) —
		// what Feathers' `applyTransport` does from its event loop. Collected by `drain`, consumed by this block.
		engine_.setTransportAnchors(transportAnchors_.data(), transportAnchorCount_);
		transportAnchorCount_ = 0;

		// THE NOTES THIS BLOCK COLLECTED, BEFORE THE BLOCK RUNS, at host-block offsets. `processStereo`
		// offers each one in the 64-frame piece that holds it, at its offset in that tick, so a device
		// sees the stream in the block the host sent it and in the tick it belongs to.
		// **THE FORWARD CHANNEL, DRAINED BEFORE THE BLOCK (AS 2).** One of the two sites; the other is
		// `paramsFlush`. CLAP guarantees the two never run at once, which is what makes every write to
		// engine state land where no block can be running.
		// THE SIDECHAIN INPUT (design §10.1): its buffers and whether anything is connected (device::SidechainPresence).
		const float* sideL = nullptr;
		const float* sideR = nullptr;
		bool sideZero = false;
		if (process->audio_inputs_count >= 2 && process->audio_inputs[1].data32 != nullptr && process->audio_inputs[1].channel_count >= 1) {
			const clap_audio_buffer& in1 = process->audio_inputs[1];
			sideL = in1.data32[0];
			sideR = in1.channel_count >= 2 ? in1.data32[1] : in1.data32[0];
			const uint64_t both = in1.channel_count >= 2 ? 3u : 1u;
			sideZero = (in1.constant_mask & both) == both && sideL != nullptr && sideR != nullptr && frames > 0
			           && sideL[0] == 0.0f && sideR[0] == 0.0f;
		}
		engine_.setSidechain(sideL, sideR, sidechain_.update(sideL, sideR, static_cast<uint32_t>(frames), sideZero, sidechainRate_));
#ifdef FEATHERS_DIAGNOSTIC
		{
			const bool have = process->audio_inputs_count >= 2 && process->audio_inputs != nullptr;
			sidechain_.trace("rack", process->audio_inputs_count, have && process->audio_inputs[1].data32 != nullptr,
			                 have ? process->audio_inputs[1].channel_count : 0u, have ? process->audio_inputs[1].constant_mask : 0u);
		}
#endif
		{
			const rack::StageScope stage(rack::Stage::Inputs); // the patch drain
			bridge_.drain();
		}
		engine_.setBlockNotes(notesIn_.data(), noteCount_);
		{
			const rack::StageScope stage(rack::Stage::Bus); // its devices taken out, and the engine's own Resolve and Mods
			engine_.processStereo(left, right, frames, block);
		}
		// **THE MIRRORS, ONCE A BLOCK, AFTER THE WORK** — Feathers' shape (`bridge_.publish(params_)`
		// beside `publishEditorLanes`). After, not before, so the editor draws what this block did
		// rather than what the last one did.
		{
			const rack::StageScope stage(rack::Stage::Publish);
			bridge_.publish(engine_);
		}
		askRestartIfLatencyMoved();
		emitNotes(process->out_events);
		// AND THE COLLECTOR IS EMPTIED FOR THE NEXT BLOCK. Left standing, a note would be replayed
		// every block for the life of the plugin — silent for a filter, a stuck arp for anything that
		// reads them.
		noteCount_ = 0;
		return CLAP_PROCESS_CONTINUE;
	}

	//--------------------------------------------------------------------------------------------//
	// GUI — the shared window shell with the rack's surface in it.                                 //
	//--------------------------------------------------------------------------------------------//
	//
	// THIS IS SHORT BECAUSE THE WINDOW WAS EXTRACTED BEFORE IT WAS WRITTEN. Everything a plugin window
	// gets wrong lives in `ui::PluginWindow`, paid for once against a real host by Feathers.

	bool implementsGui() const noexcept override { return RackGui::isAvailable(); }

	bool guiIsApiSupported(const char* api, bool isFloating) noexcept override {
		return RackGui::isApiSupported(api, isFloating);
	}

	bool guiGetPreferredApi(const char** api, bool* isFloating) noexcept override {
#if defined(__APPLE__)
		*api = CLAP_WINDOW_API_COCOA;
#else
		*api = CLAP_WINDOW_API_X11;
#endif
		*isFloating = false;
		return true;
	}

	bool guiCreate(const char* api, bool isFloating) noexcept override {
#if defined(__linux__)
		// THE SAME REFUSAL FEATHERS MAKES, and for the same reason: without one of these the host never
		// drives the editor, and an editor that never ticks is a dead one that looks alive.
		if (!_host.canUsePosixFdSupport() && !_host.canUseTimerSupport()) {
			return false;
		}
#endif
		// ONLY NOW IS THERE A HOST TO ASK. The editor works out WHAT size it wants; asking is the
		// plugin's job because the plugin is what holds a `clap_host`.
		gui_.setResizeRequester([this](uint32_t w, uint32_t h) { _host.guiRequestResize(w, h); });
		const bool made = gui_.create(api, isFloating);
		log_.say(std::string(made ? "gui-create api=" : "gui-create FAILED api=") + (api != nullptr ? api : "?")
		         + (isFloating ? " floating" : " embedded"));
		return made;
	}

	void guiDestroy() noexcept override {
		log_.say("gui-destroy");
		if (guiFd_ >= 0) {
			_host.posixFdSupportUnregister(guiFd_);
			guiFd_ = -1;
		}
		gui_.destroy();
		// WHAT THE DETACH WAS ANSWERED (backlog GQ) — see Feathers' guiDestroy.
		if (gui_.lastXReport().errors > 0) {
			log_.say("gui-destroy detach " + ui::PluginWindow::describe(gui_.lastXReport()));
		}
	}
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
		// REGISTER THE FD ONLY ONCE THERE IS A WINDOW TO POLL. Before setParent there is no X
		// connection and `posixFd` answers -1; registering that would ask the host to poll nothing.
		if (const int fd = gui_.posixFd(); fd >= 0 && guiFd_ < 0 && _host.canUsePosixFdSupport()) {
			if (_host.posixFdSupportRegister(fd, CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE
			                                         | CLAP_POSIX_FD_ERROR)) {
				guiFd_ = fd;
			}
		}
		return true;
	}

	//--------------------------------------------------------------------------------------------//
	// AND SOMEBODY HAS TO PUMP IT.                                                                 //
	//--------------------------------------------------------------------------------------------//
	//
	// **WITHOUT THESE THE EDITOR NEVER PAINTS AT ALL.** Not late — never. The rack shipped its first
	// window as a black rectangle: `draw()` was called on a visible, correctly-sized frame with a
	// parent, and every canvas operation went nowhere, because nothing drained the queue they end up
	// on. `guiCreate` already REFUSED a host offering neither of these, which was the right check
	// aimed at the wrong party — it verified the host could pump and never asked to be pumped.
	//
	// BOTH, because on Linux the fd is the live road and the timer is the fallback, and on macOS it is
	// the other way round.

	bool implementsPosixFdSupport() const noexcept override { return true; }
	/// AND THE HOST SLOTS' VISIT AFTER THE EDITOR'S (plan §1.3), on whichever beat drives it: a card's ✕ or a
	/// knob moved a slot, and the host hears it within the beat. Cheap when nothing moved. THEN the dirty mark
	/// (JV), after the visit, so a slot it newly anchored is told in the same beat.
	void onPosixFd(int, clap_posix_fd_flags_t) noexcept override {
		gui_.processEvents();
		serviceSlots();
		markDirtyIfEdited();
	}

	bool implementsTimerSupport() const noexcept override { return true; }
	void onTimer(clap_id id) noexcept override {
#ifdef FEATHERS_DIAGNOSTIC
		if (id == spikeTimer_) {
			drainSpikes();
			return;
		}
#else
		(void)id;
#endif
		gui_.processEvents();
		serviceSlots();
		markDirtyIfEdited();
	}

#ifdef FEATHERS_DIAGNOSTIC
	/// MAIN THREAD: the ring into `<date>.spikes.jsonl` beside the likes log, and the minute's summary (backlog SR).
	void drainSpikes() noexcept {
		try {
			std::vector<std::string> lines;
			const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
			spikes_.drain("feathers-chain", now, &lines);
			(void)spike::append(lines, now);
		}
		catch (...) {
		}
	}

	/// ONE BLOCK, TIMED FROM ENTRY TO EVERY RETURN.
	struct SpikeBlock {
		RackPlugin& plugin;
		const clap_process* process;
		spike::BlockStart start;
		bool restart;
		SpikeBlock(RackPlugin& p, const clap_process* proc) noexcept
		    : plugin(p), process(proc), start(p.spikes_.beginBlock()), restart(p.restartAsked_) {}
		~SpikeBlock() { plugin.endSpike(*this); }
		SpikeBlock(const SpikeBlock&) = delete;
		SpikeBlock& operator=(const SpikeBlock&) = delete;
	};

	void endSpike(const SpikeBlock& b) noexcept {
		spike::BlockFacts facts;
		facts.frames = b.process != nullptr ? b.process->frames_count : 0;
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
	bool spikePlaying_ = false;
	bool spikeFirst_ = true;
#endif
	bool guiShow() noexcept override { return gui_.hasWindow(); }
	bool guiHide() noexcept override { return true; }
	bool guiSetScale(double scale) noexcept override { return gui_.setScale(scale); }
	bool guiCanResize() const noexcept override { return true; }
	bool guiAdjustSize(uint32_t* w, uint32_t* h) noexcept override { return gui_.adjustSize(w, h); }
	bool guiSetSize(uint32_t w, uint32_t h) noexcept override { return gui_.setSize(w, h); }
	bool guiGetSize(uint32_t* w, uint32_t* h) noexcept override { return gui_.getSize(w, h); }

	//--------------------------------------------------------------------------------------------//
	// STATE                                                                                        //
	//--------------------------------------------------------------------------------------------//

	bool implementsState() const noexcept override { return true; }

	/// **THE RECORD, WRITTEN** — what the rack IS, which the engine is at most a drain behind.
	bool stateSave(const clap_ostream* stream) noexcept override {
		// THE HOST SLOTS ARE SERVICED FIRST (plan §1.3): a host write no visit has seen yet is an anchor in the
		// saved pool, and no slot is saved naming a device that stopped resolving since the last visit.
		serviceSlots();
		if (stream == nullptr) {
			log_.say("state-save FAILED why=no-stream");
			return false;
		}
		// **THE WHOLE STREAM OR NOTHING, SEALED** — Feathers' rule (`FeathersPlugin::saveWalk`): serialised into one
		// buffer, the end marker appended (`state/state_trailer.h`), and only then handed to the host.
		std::vector<uint8_t> bytes;
		MemOStreamForState mem(&bytes);
		if (!writeRack(&mem.stream, bridge_.patch())) {
			log_.say("state-save FAILED bytes=0 why=unencodable; nothing written");
			return false;
		}
		state::appendTrailer(bytes);
		uint64_t written = 0;
		if (!state::writeWhole(stream, bytes, &written)) {
			log_.say("state-save FAILED stage=host-stream bytes=" + std::to_string(written) + " of "
			         + std::to_string(bytes.size()) + " why=host-short-write");
			return false;
		}
		log_.say("state-save ok bytes=" + std::to_string(bytes.size()));
		return true;
	}

	bool stateLoad(const clap_istream* stream) noexcept override {
		// **THE LOAD REPLACES THE RECORD, HERE, AND NEVER TOUCHES THE ENGINE**. `state.load` is `[main-thread]`, NOT `[main-thread & !active]`, and our
		// VST3 road forwards `setState` with no deactivate — so a block may be running. It does not
		// matter any more: the record is the message thread's, and the engine becomes it at a drain.
		//
		// **WHAT THIS RETIRED:** a patch channel to the drain, a bounded wait on it, refusals that
		// arrived after the call that caused them, and an open question about what to return
		// when that wait expired (backlog CD, the plan's §6). There is no wait, so there is no question:
		// the refusals are known before this returns, and success means the record is the patch.
		//
		// PARSE FIRST — nothing is changed by a stream that does not parse whole.
		RackPatch patch;
		LoadReport notLoaded;
		if (stream == nullptr) {
			log_.say("state-load FAILED why=no-stream; nothing changed");
			return false;
		}
		std::vector<uint8_t> bytes;
		if (!readWholeStream(stream, &bytes)) {
			log_.say("state-load FAILED bytes=" + std::to_string(bytes.size()) + " why=read-error-or-oversized; nothing changed");
			return false;
		}
		const size_t received = bytes.size();
		// **THE END MARKER** (`state/state_trailer.h`): a stream that ends in one must vouch for itself — its length
		// and CRC — or it is refused before a byte is parsed. A stream with NONE is a rack state saved before the
		// marker existed, and those patches may be kept: it is read as it always was, because the rack's patch is a JSON
		// document whose own closing brace is its end, so a cut one already fails to parse.
		size_t payload = 0;
		const state::TrailerCheck check = state::checkTrailer(bytes, &payload);
		if (check == state::TrailerCheck::LengthMismatch || check == state::TrailerCheck::Checksum) {
			log_.say("state-load FAILED stage=end-marker bytes=" + std::to_string(received) + " why="
			         + state::trailerWhy(check) + "; nothing changed");
			notLoaded.note(LoadReport::Kind::NotLoaded, "patch", "", "the saved state is damaged (its end marker does not match)");
			lastRefusals_ = notLoaded;
			gui_.sayLoadReport(notLoaded);
			return false;
		}
		// AND AN UNMARKED STREAM THAT STILL HOLDS THE MAGIC NEAR ITS END IS A MARKED ONE CUT INSIDE ITS MARKER — the
		// document's parser takes trailing bytes, so this is said here. No state before the marker ends in "FEND".
		if (check == state::TrailerCheck::Missing) {
			const size_t from = bytes.size() > state::kTrailerBytes ? bytes.size() - state::kTrailerBytes : 0;
			for (size_t i = from; i + 4 <= bytes.size(); ++i) {
				if (std::memcmp(bytes.data() + i, state::kEndMagic, 4) == 0) {
					log_.say("state-load FAILED stage=end-marker bytes=" + std::to_string(received)
					         + " why=no-end-marker (cut inside it); nothing changed");
					notLoaded.note(LoadReport::Kind::NotLoaded, "patch", "", "the saved state was cut short");
					lastRefusals_ = notLoaded;
					gui_.sayLoadReport(notLoaded);
					return false;
				}
			}
		}
		const bool trailered = check == state::TrailerCheck::Ok;
		if (trailered) {
			bytes.resize(payload);
		}
		BufferIStream in(bytes);
		if (!parseRack(&in.stream, &patch, &notLoaded)) {
			// NOTHING CHANGED, AND SAID SO — an old or damaged patch is refused whole (backlog JF).
			log_.say("state-load FAILED bytes=" + std::to_string(received) + " " + loadReportDetail(notLoaded)
			         + "; nothing changed");
			lastRefusals_ = notLoaded;
			gui_.sayLoadReport(notLoaded);
			return false;
		}
		// THE LOAD TELLS US ONCE PER MACRO WHOSE NAME MOVED; THE HOST HEARS IT ONCE, folded into the values'
		// rescan below. Nothing between the two assignments can return, so no scope is needed to settle the
		// debt on a refusal — and a refusal above changed nothing, so it owes the host no rescan at all.
		loadingState_ = true;
		const LoadReport refused = bridge_.loadPatch(patch);
		// THE SLOTS THE PATCH RESTORED ARE BOUND AND RENAMED NOW, before any host event: an anchored slot is
		// anchored when this returns (design §6). Their renames ride the macros' one rescan below.
		serviceSlots();
		loadingState_ = false;
		if (!isActive()) {
			// NO BLOCK CAN RUN, so this thread takes the drain's part: the engine settles now, and a
			// macro read back through the host's parameter road answers the patch's value at once.
			bridge_.drain();
			bridge_.publish(engine_);
		}
		// **EVERY MACRO'S VALUE JUST MOVED UNDER THE HOST'S CACHED COPY** (backlog JW) — Feathers' stateLoad
		// sends this too. After the settle above, so a host that reads the values back at once is answered
		// the patch's. A rename the patch made rides the same call.
		rescanParams(CLAP_PARAM_RESCAN_VALUES);
		refreshTail(); // the patch's chain, not the one it replaced
		if (isActive() && _host.canUseParams()) {
			// ACTIVE BUT PERHAPS STOPPED: ask for the flush where the other drain is, so the engine does
			// not wait for audio that may never come to become the rack the editor already shows.
			_host.paramsRequestFlush();
		}
		{
			LoadReport all = notLoaded; // what the parse noted on the way to success (missing keys, coerced values)
			all.merge(refused);
			const std::string detail = loadReportDetail(all);
			log_.say("state-load ok bytes=" + std::to_string(received) + (trailered ? " end-marker=ok" : " end-marker=none (legacy)")
			         + (detail.empty() ? "" : " " + detail));
		}
		lastRefusals_ = refused;
		// SAID, in the same gesture that caused it — and taken off the bridge, which latched it too, so the
		// tray's next look does not say it twice.
		(void)bridge_.takeLoadReport();
		gui_.sayLoadReport(refused);
		gui_.syncCards();
		return true;
	}

	/// What the last load did not load as saved, for whoever grows a tray to say it with.
	const LoadReport& lastRefusals() const noexcept { return lastRefusals_; }

	/// THE ENGINE, so a venue can reach the chain without a parameter for every verb. Adding a device
	/// is not a parameter and must not become one — see the header note on the static surface.
	RackEngine& engine() noexcept { return engine_; }

private:
	void drain(const clap_input_events* in) {
		if (in == nullptr) {
			return;
		}
		const uint32_t count = in->size(in);
		for (uint32_t i = 0; i < count; ++i) {
			const clap_event_header* header = in->get(in, i);
			if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID) {
				continue;
			}
			switch (header->type) {
			case CLAP_EVENT_PARAM_VALUE: {
				const auto* e = reinterpret_cast<const clap_event_param_value*>(header);
				// A HOST SLOT FIRST — `isSlotId` before the macros, as every params call site asks it.
				if (hostslots::isSlotId(e->param_id)) {
					onHostSlotValue(hostslots::slotOfId(e->param_id), e->value);
					break;
				}
				// **THROUGH THE BRIDGE (CD commit 4), and the ORDER is why it works.** This walk runs
				// inside `drain(const clap_input_events*)`, which both call sites invoke BEFORE
				// `bridge_.drain()` — so a macro queued here is applied by the very next statement in
				// the same visit, not a block later. It was already at a safe site; what it was not was
				// unreachable, and commit 4 is about reachability.
				bridge_.setMacro(e->param_id, static_cast<float>(e->value));
				break;
			}
			case CLAP_EVENT_PARAM_MOD: {
				// **GLOBAL MODULATION ON A SLOT IS AN OFFSET ON TOP OF ITS VALUE** (design §4), in the slot's 0..1
				// units, which the engine scales by the control's span. A PER-NOTE mod on a slot is ignored, as
				// Feathers ignores it: the rack has no voices to address. (A macro's modulation is not taken
				// here and never was: the macros are values only on this surface.)
				const auto* e = reinterpret_cast<const clap_event_param_mod*>(header);
				rack::SlotOwner* slots = bridge_.hostSlots();
				if (hostslots::isSlotId(e->param_id) && slots != nullptr && e->note_id == -1 && e->key == -1
				    && e->channel == -1) {
					slots->onHostMod(hostslots::slotOfId(e->param_id), e->amount);
					if (e->amount != 0.0) {
						_host.requestCallback(); // a non-zero amount anchors and marks the slot (2026-09-26)
					}
				}
				break;
			}
			case CLAP_EVENT_NOTE_ON:
				// **THE WHOLE EVENT, AND NOTHING ELSE.** The gate and the retrigger used to be decided here — a
				// counter, pushed through the bridge and applied at the top of the HOST block — so a note at
				// offset 300 opened the gate 300 samples early, a note that ended inside its block never
				// struck, and an off for a key nobody held closed the gate under one still down. The engine
				// derives both now, from the Tracker's held stack, in the piece that holds the event
				// (`RackEngine::updateNoteContext`): the gate is a summary every device would compute
				// identically, and now it is computed in one place, at the note's own sample.
				collectNote(device::NoteEvent::Kind::On, header);
				break;
			case CLAP_EVENT_NOTE_OFF:
			case CLAP_EVENT_NOTE_CHOKE:
				collectNote(header->type == CLAP_EVENT_NOTE_CHOKE ? device::NoteEvent::Kind::Choke
				                                                  : device::NoteEvent::Kind::Off,
				            header);
				break;
			case CLAP_EVENT_NOTE_EXPRESSION:
				collectExpression(header);
				break;
			case CLAP_EVENT_MIDI:
				collectMidi(reinterpret_cast<const clap_event_midi*>(header), header->time);
				break;
			case CLAP_EVENT_TRANSPORT:
				// A TRANSPORT INSIDE THE BLOCK (backlog NJ (b)) — a locate or a tempo change at its sample; the block
				// re-anchors on it from there (`RackEngine::setTransportAnchors`). Dropped past the engine's capacity.
				if (transportAnchorCount_ < transportAnchors_.size()) {
					transportAnchors_[transportAnchorCount_++] = RackEngine::TransportAnchor {
						static_cast<int>(header->time), contextOf(*reinterpret_cast<const clap_event_transport*>(header), sidechainRate_)
					};
				}
				break;
			default:
				break;
			}
		}
	}

	//--------------------------------------------------------------------------------------------//
	// THE TEST EXTENSION — see rack_test_ext.h for why a private road exists at all.               //
	//--------------------------------------------------------------------------------------------//

	static RackPlugin* self(const clap_plugin* plugin) {
		return plugin != nullptr ? static_cast<RackPlugin*>(plugin->plugin_data) : nullptr;
	}

	static int32_t CLAP_ABI chainParamIndexOf(const clap_plugin* plugin, uint32_t id) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? p->getParamIndexForParamId(id) : -1;
	}
	static uint64_t CLAP_ABI chainParamInfoBuilds(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? p->infoBuilds_ : 0;
	}

	static uint32_t CLAP_ABI testAddDevice(const clap_plugin* plugin, const char* tag) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || tag == nullptr) {
			return kNoInstance;
		}
		// **THROUGH THE BRIDGE, LIKE THE EDITOR** — `guihost` drives this against a plugin the host may be
		// processing, so it must not touch the engine. The record answers at once; the engine becomes it
		// at the next drain. `sync_cards` reconciles the card stack against the record.
		//
		// **THIS ABI STILL SPEAKS THE AXES' COORDINATES** (`rack_test_ext.h`: a venue computes a route's
		// indices from the axes, and `resolved_field` reads the engine by slot), so it answers the lane
		// the record keeps the new device at — the test road's own crossing, until the engine and this ABI
		// go by id (kill-lanes step 3).
		//
		// **ASKED OF THE RECORD'S GRAPH, WHICH IS WHERE A POOL INDEX LIVES NOW (step 3b).** The record itself
		// holds each chain's ORDER and answers by id alone; the graph is the one thing that still knows where
		// it parked a node, and it publishes that map for exactly this direction (`placement`). Nothing but
		// this ABI asks — and it asks because the number goes straight back out to a venue that computes
		// coordinates from the axes.
		// **THE ID, WHICH IS THE WHOLE OF WHAT A DEVICE IS NAMED BY (kill-lanes §2.4b).** It answered the
		// record's LANE, because the venue computed flat coordinates from it; the venue speaks endpoints now
		// and the one number it needs is this one.
		return p->bridge_.addDevice(tag);
	}

	static void CLAP_ABI testSetRoute(const clap_plugin* plugin, uint32_t sourceDevice, uint32_t sourceIndex,
	                                  uint32_t destinationDevice, uint32_t destinationIndex,
	                                  float amount) noexcept {
		if (RackPlugin* p = self(plugin)) {
			// ENDPOINTS IN, ENDPOINTS TO THE BRIDGE — the venue speaks the record's own vocabulary now
			// (kill-lanes §2.4b); `0` is the host end, as `kNoInstance` is everywhere.
			const backplane::Source s = sourceDevice == kNoInstance ? backplane::Source::core(sourceIndex)
			                                                        : backplane::Source::output(sourceDevice, sourceIndex);
			const backplane::Destination d =
			    destinationDevice == kNoInstance ? backplane::Destination::core(destinationIndex)
			                                     : backplane::Destination::control(destinationDevice, destinationIndex);
			p->bridge_.setRouteAmount(s, d, amount); // into the record, as the UI's does
		}
	}

	// **`testAxes` STOOD HERE AND IS DELETED (kill-lanes §2.4b).** It handed a venue the strides it needed
	// to compute a flat coordinate; there is no coordinate to compute.

	static void* CLAP_ABI testEditor(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? static_cast<void*>(p->gui_.editorForTest()) : nullptr;
	}

	static void CLAP_ABI testSyncCards(const clap_plugin* plugin) noexcept {
		if (RackPlugin* p = self(plugin)) {
			p->gui_.syncCards();
		}
	}

	static void* CLAP_ABI testBridge(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? static_cast<void*>(&p->bridge_) : nullptr;
	}

	static float CLAP_ABI testResolvedField(const clap_plugin* plugin, uint32_t id, size_t control) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr) {
			return 0.0f;
		}
		// BY THE ID `add_device` ANSWERED — the engine keeps the device in a slot of its own choosing
		// (kill-lanes step 3a) and `slotOf` is the one road to it. It took the RECORD's lane and crossed
		// twice; there is no lane to cross from.
		const size_t engineSlot = p->engine_.slotOf(id);
		return engineSlot != RackEngine::kNoSlot ? p->engine_.resolvedFieldForTest(engineSlot, control) : 0.0f;
	}

	//--------------------------------------------------------------------------------------------//
	// AND THE SYNTH'S OWN TEST-CHAIN EXTENSION, so the tools that already exist work here.          //
	//--------------------------------------------------------------------------------------------//
	//
	// `guihost` seeds a chain through `CLAP_EXT_FEATHERS_TEST_CHAIN` and photographs the editor. The
	// rack speaks its own richer dialect (`rack_test_ext.h`) and so could not be driven by the one
	// venue in this repo that can take a picture of a laid-out editor — which is how its first three
	// bugs reached a human instead of a suite.
	//
	// SO IT SPEAKS BOTH. This is four forwards onto the same engine, not a second mechanism: a rack
	// device IS an fx device to anything that only wants to put one in a chain and look at it.

	static uint32_t CLAP_ABI chainAddDevice(const clap_plugin* plugin, const char* tag) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || tag == nullptr) {
			return UINT32_MAX;
		}
		// AS ABOVE: into the record, never the engine. **THE ANSWER IS A POSITION** — the chain
		// extension's contract (`feathers_test_chain.h`), which Feathers already keeps — and an append
		// lands at the end.
		const DeviceInstanceId added = p->bridge_.addDevice(tag);
		if (added == kNoInstance) {
			return UINT32_MAX;
		}
		p->gui_.syncCards();
		return static_cast<uint32_t>(p->bridge_.positionOf(added));
	}

	static bool CLAP_ABI chainSetControl(const clap_plugin* plugin, uint32_t slot,
	                                     const char* control, double value) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || control == nullptr) {
			return false;
		}
		// **THE DESCRIPTOR FROM THE RECORD, NOT FROM THE ENGINE, and it has to be.** A device added
		// through `chain_add_device` above is the record's at once and the engine's only after the next
		// drain, so asking the engine would answer null for the device this venue just added.
		// THE POSITION `chain_add_device` ANSWERED, turned into the device standing there — the one
		// crossing, at the edge of an ABI whose contract is positional.
		const DeviceInstanceId device = p->bridge_.deviceAtPosition(slot);
		const device::Descriptor* desc = p->bridge_.descriptorOf(device);
		if (desc == nullptr) {
			return false;
		}
		// BY THE CONTROL'S OWN NAME, because a venue naming an index would be naming a position in a
		// list it cannot see.
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (std::strcmp(desc->params[i].name, control) == 0) {
				p->bridge_.setField(device, i, static_cast<float>(value));
				return true;
			}
		}
		return false;
	}

	static bool CLAP_ABI chainAvailableDevice(const clap_plugin*, uint32_t index, char* tagOut,
	                                          uint32_t capacity) noexcept {
		if (tagOut == nullptr || capacity == 0) {
			return false;
		}
		uint32_t seen = 0;
		for (const device::Descriptor& d : kDevices) {
			// WHAT THE RACK CAN RUN — audio or modulation. Enumerating one the next call rejects is
			// what `feathers_clap.cpp` refuses to do, for the same reason.
			if ((d.audio == nullptr && d.mod == nullptr) || d.traits.tag == nullptr) {
				continue;
			}
			if (seen++ != index) {
				continue;
			}
			std::snprintf(tagOut, capacity, "%s", d.traits.tag);
			return true;
		}
		return false;
	}

	static uint32_t CLAP_ABI chainDeviceCount(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr) {
			return 0;
		}
		// THE RECORD'S COUNT — what the rack IS; the engine is at most a drain behind it.
		return static_cast<uint32_t>(p->bridge_.chainLength());
	}

	static bool CLAP_ABI chainRenameMacro(const clap_plugin* plugin, uint32_t index, const char* name) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || name == nullptr || index >= kMacroCount) {
			return false;
		}
		// THE CAPTION'S OWN CALL (`RackEditor`'s macro naming), so the host is told what a rename tells it.
		p->bridge_.setMacroName(index, name);
		return true;
	}

	//--------------------------------------------------------------------------------------------//
	// THE HOST SLOT ENTRIES (test-chain /3), FOR REAL SINCE T10 — the rack owns a pool now. Each      //
	// names the device at `position` and its control by NAME, `set_control`'s rule, and goes the    //
	// road the product goes: the pool's own verbs, the bridge's removal, a `ui::DeviceControl` over  //
	// the bridge for a hand. Four still refuse, each because the rack has no such road to drive:     //
	// `undo_remove` (a removal here is not undoable — `RackBridge::removeDevice` never re-inserts     //
	// under an old id), `capture_preset`/`load_preset` (the rack has no preset road apart from the   //
	// host's state), and `replace_with_filter` (nothing here stamps a device over another's id).     //
	//--------------------------------------------------------------------------------------------//

	/// The device at `position` and the index of its control called `control`, or false.
	bool controlAt(uint32_t position, const char* control, DeviceInstanceId* device, size_t* index,
	               const device::Descriptor** descOut) const noexcept {
		const DeviceInstanceId id = bridge_.deviceAtPosition(position);
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

	/// `touch_slot` and `pin_slot`: the pool's own verbs, and a main-thread visit asked for — the rescan is
	/// the VISIT's, never the touch's.
	static uint32_t chainSlotVerb(const clap_plugin* plugin, uint32_t position, const char* control, bool pin) noexcept {
		RackPlugin* p = self(plugin);
		rack::SlotOwner* slots = p != nullptr ? p->bridge_.hostSlots() : nullptr;
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (slots == nullptr || !p->controlAt(position, control, &device, &index, &desc)) {
			return UINT32_MAX;
		}
		try {
			const std::optional<hostslots::Target> target = slots->targetOf(device, index);
			if (!target) {
				return UINT32_MAX;
			}
			const hostslots::Allocation a = pin ? slots->pool().pin(*target) : slots->pool().touch(*target);
			p->_host.requestCallback();
			return a.slot == hostslots::kNoSlot ? UINT32_MAX : a.slot;
		}
		catch (...) {
			return UINT32_MAX;
		}
	}
	static uint32_t CLAP_ABI chainTouchSlot(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		return chainSlotVerb(plugin, position, control, false);
	}
	static uint32_t CLAP_ABI chainPinSlot(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		return chainSlotVerb(plugin, position, control, true);
	}

	/// The card's ✕ (`RackEditor::removeDevice` → `RackBridge::removeDevice`), and a visit asked for.
	static bool CLAP_ABI chainRemoveDevice(const clap_plugin* plugin, uint32_t position) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr) {
			return false;
		}
		const DeviceInstanceId device = p->bridge_.deviceAtPosition(position);
		if (device == kNoInstance) {
			return false;
		}
		try {
			p->bridge_.removeDevice(device);
		}
		catch (...) {
			return false;
		}
		p->_host.requestCallback();
		return p->bridge_.positionOf(device) == RackBridge::kNoSlot;
	}

	static uint32_t CLAP_ABI chainSlotState(const clap_plugin* plugin, uint32_t slot) noexcept {
		const RackPlugin* p = self(plugin);
		const rack::SlotOwner* slots = p != nullptr ? p->bridge_.hostSlots() : nullptr;
		if (slots == nullptr || slot >= slots->pool().capacity()) {
			return UINT32_MAX;
		}
		return static_cast<uint32_t>(slots->pool().state(static_cast<hostslots::SlotIndex>(slot)));
	}

	/// The ENGINE's base and resolved value of the control — what the device actually ran with, by the id the
	/// engine keeps it under. Only with no block in flight.
	static bool CLAP_ABI chainEngineControl(const clap_plugin* plugin, uint32_t position, const char* control,
	                                        double* base, double* resolved) noexcept {
		RackPlugin* p = self(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (p == nullptr || !p->controlAt(position, control, &device, &index, &desc)) {
			return false;
		}
		const size_t slot = p->engine_.slotOf(device);
		if (slot == RackEngine::kNoSlot) {
			return false;
		}
		if (base != nullptr) {
			*base = static_cast<double>(p->engine_.fieldAt(slot, index));
		}
		if (resolved != nullptr) {
			*resolved = static_cast<double>(p->engine_.modulatedFieldAt(slot, index));
		}
		return true;
	}

	/// What the control's KNOB reads — `DeviceControl::value`, over the bridge the rack's cards are built on.
	static bool CLAP_ABI chainKnobValue(const clap_plugin* plugin, uint32_t position, const char* control,
	                                    double* value) noexcept {
		RackPlugin* p = self(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (p == nullptr || value == nullptr || !p->controlAt(position, control, &device, &index, &desc)) {
			return false;
		}
		const ui::DeviceControl knob(p->bridge_, device, *desc, index);
		*value = knob.value();
		return true;
	}

	/// A HAND ON THE KNOB — one `DeviceControl` gesture over the bridge, as a card drives it, recording its undo
	/// step into this seam's own history.
	static bool CLAP_ABI chainDragControl(const clap_plugin* plugin, uint32_t position, const char* control,
	                                      const double* values, uint32_t count) noexcept {
		RackPlugin* p = self(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (p == nullptr || (values == nullptr && count > 0) || !p->controlAt(position, control, &device, &index, &desc)) {
			return false;
		}
		try {
			ui::DeviceControl knob(p->bridge_, device, *desc, index, nullptr, &p->testHistory_);
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

	static bool CLAP_ABI chainUndo(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || !p->testHistory_.canUndo()) {
			return false;
		}
		try {
			p->testHistory_.undo();
			return true;
		}
		catch (...) {
			return false;
		}
	}

	// THE FOUR ROADS THE RACK DOES NOT HAVE (see the banner): a venue that asks hears "no" rather than calling
	// through a null pointer.
	static bool CLAP_ABI chainNoUndoRemove(const clap_plugin*) noexcept { return false; }
	static bool CLAP_ABI chainNoCapture(const clap_plugin*, const clap_ostream*) noexcept { return false; }
	static bool CLAP_ABI chainNoLoad(const clap_plugin*, const clap_istream*) noexcept { return false; }
	static bool CLAP_ABI chainNoReplace(const clap_plugin*, uint32_t) noexcept { return false; }

	/// A DOUBLE-CLICK ON A `ValueBox` over the control — the box's own reset road, over the bridge the rack's
	/// cards are built on (Feathers' `reset_control`, T6b review). Nothing here restates what a reset does.
	static bool CLAP_ABI chainResetControl(const clap_plugin* plugin, uint32_t position, const char* control) noexcept {
		RackPlugin* p = self(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (p == nullptr || !p->controlAt(position, control, &device, &index, &desc)) {
			return false;
		}
		try {
			ui::DeviceControl knob(p->bridge_, device, *desc, index);
			ui::ValueBox box(knob, "");
			visage::MouseEvent e;
			e.position = { 1.0f, 1.0f };
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

	/// A HAND TAKES THE CONTROL, MOVES IT TO `value`, AND THE CONTROL IS DESTROYED WITH THE HAND STILL DOWN
	/// (Feathers' `abandon_drag`, T6b review): the destructor's End is what closes the host's gesture.
	static bool CLAP_ABI chainAbandonDrag(const clap_plugin* plugin, uint32_t position, const char* control,
	                                      double value) noexcept {
		RackPlugin* p = self(plugin);
		DeviceInstanceId device = kNoInstance;
		size_t index = 0;
		const device::Descriptor* desc = nullptr;
		if (p == nullptr || !p->controlAt(position, control, &device, &index, &desc)) {
			return false;
		}
		ui::DeviceControl knob(p->bridge_, device, *desc, index);
		knob.beginGesture();
		knob.setValue(value);
		return true; // and the control goes out of scope with the hand still down
	}

	/// A DEVICE'S BODY, as a save would write it — the running instance's when one is on file (`feathers_test_chain.h`).
	// WHICH PRESET (backlog QC): the rack plugin's own browser's name, and each device panel's.
	static bool copyOut(const std::string& s, char* out, uint32_t capacity) {
		if (out == nullptr || capacity == 0 || s.size() + 1 > capacity) {
			return false;
		}
		std::memcpy(out, s.c_str(), s.size() + 1);
		return true;
	}
	static bool CLAP_ABI chainSetPresetIdentity(const clap_plugin* plugin, const char* name, const char* category) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr) {
			return false;
		}
		p->bridge_.setTopPreset(PresetIdentity { name != nullptr ? name : "", category != nullptr ? category : "" });
		return true;
	}
	static bool CLAP_ABI chainPresetIdentity(const clap_plugin* plugin, char* nameOut, uint32_t nameCapacity,
	                                         char* categoryOut, uint32_t categoryCapacity) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr && copyOut(p->bridge_.topPreset().name, nameOut, nameCapacity)
		       && copyOut(p->bridge_.topPreset().category, categoryOut, categoryCapacity);
	}
	static bool CLAP_ABI chainPresetFace(const clap_plugin* plugin, char* nameOut, uint32_t capacity) noexcept {
		RackPlugin* p = self(plugin);
		ui::PresetBrowser* browser = p != nullptr ? p->gui_.presetBrowserForTest() : nullptr;
		if (browser == nullptr) {
			return false;
		}
		return copyOut(browser->lastTouched() >= 0 ? browser->currentName() : std::string {}, nameOut, capacity);
	}
	static bool CLAP_ABI chainSetDevicePreset(const clap_plugin* plugin, uint32_t position, const char* name,
	                                          const char* category) noexcept {
		RackPlugin* p = self(plugin);
		const DeviceInstanceId id = p != nullptr ? p->bridge_.deviceAtPosition(position) : kNoInstance;
		if (p == nullptr || p->bridge_.descriptorOf(id) == nullptr) {
			return false;
		}
		p->bridge_.setDevicePreset(id, PresetIdentity { name != nullptr ? name : "", category != nullptr ? category : "" });
		return true;
	}
	static bool CLAP_ABI chainDevicePreset(const clap_plugin* plugin, uint32_t position, char* nameOut,
	                                       uint32_t nameCapacity, char* categoryOut, uint32_t categoryCapacity) noexcept {
		RackPlugin* p = self(plugin);
		const DeviceInstanceId id = p != nullptr ? p->bridge_.deviceAtPosition(position) : kNoInstance;
		if (p == nullptr || p->bridge_.descriptorOf(id) == nullptr) {
			return false;
		}
		const PresetIdentity preset = p->bridge_.devicePreset(id);
		return copyOut(preset.name, nameOut, nameCapacity) && copyOut(preset.category, categoryOut, categoryCapacity);
	}

	static bool CLAP_ABI chainDeviceBody(const clap_plugin* plugin, uint32_t position, const clap_ostream* out) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr || out == nullptr) {
			return false;
		}
		const DeviceInstanceId id = p->bridge_.deviceAtPosition(position);
		const device::Descriptor* desc = p->bridge_.descriptorOf(id);
		if (desc == nullptr || !desc->hasBody()) {
			return false;
		}
		std::vector<uint8_t> body;
		bool taken = false;
		if (const void* surface = p->bridge_.containerSurfaceOf(id); surface != nullptr) {
			taken = bodyOf(desc, surface, &body) && !body.empty();
		}
		if (!taken) {
			const std::span<const uint8_t> stored = p->bridge_.containerBodyOf(id);
			body.assign(stored.begin(), stored.end());
		}
		return body.empty() || out->write(out, body.data(), body.size()) == static_cast<int64_t>(body.size());
	}

	/// AN EDIT IN THE DEVICE'S OWN EDITOR: the bytes loaded into the RUNNING instance, where alone it then lives.
	static bool CLAP_ABI chainEditDeviceBody(const clap_plugin* plugin, uint32_t position, const void* data,
	                                         uint64_t size) noexcept {
		RackPlugin* p = self(plugin);
		if (p == nullptr) {
			return false;
		}
		const DeviceInstanceId id = p->bridge_.deviceAtPosition(position);
		const device::Descriptor* desc = p->bridge_.descriptorOf(id);
		if (desc == nullptr || desc->bodyLoader() == nullptr) {
			return false;
		}
		void* surface = p->bridge_.containerSurfaceOf(id);
		return surface != nullptr
		       && desc->bodyLoader()(surface, static_cast<const uint8_t*>(data), static_cast<size_t>(size));
	}

	/// The standalone rack has no modulator strip apart from its chain.
	static uint32_t CLAP_ABI chainNoModDevice(const clap_plugin*, const char*) noexcept { return UINT32_MAX; }
	static uint32_t CLAP_ABI chainMaxFrames(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? static_cast<uint32_t>(p->engine_.maxFrames()) : 0;
	}
	static uint64_t CLAP_ABI chainPrepareCount(const clap_plugin* plugin) noexcept {
		RackPlugin* p = self(plugin);
		return p != nullptr ? p->engine_.preparesForTest() : 0;
	}

	const void* extension(const char* id) noexcept override {
		static const clap_plugin_feathers_rack_test kTest {
			&RackPlugin::testAddDevice,
			&RackPlugin::testSetRoute,
			&RackPlugin::testResolvedField,
			&RackPlugin::testEditor,
			&RackPlugin::testSyncCards,
			&RackPlugin::testBridge,
		};
		static const clap_plugin_feathers_test_chain kChain {
			&RackPlugin::chainAddDevice,
			&RackPlugin::chainSetControl,
			&RackPlugin::chainAvailableDevice,
			&RackPlugin::chainDeviceCount,
			&RackPlugin::chainRenameMacro,
			&RackPlugin::chainTouchSlot,
			&RackPlugin::chainPinSlot,
			&RackPlugin::chainRemoveDevice,
			&RackPlugin::chainNoUndoRemove,
			&RackPlugin::chainNoCapture,
			&RackPlugin::chainNoLoad,
			&RackPlugin::chainNoReplace,
			&RackPlugin::chainSlotState,
			&RackPlugin::chainEngineControl,
			&RackPlugin::chainKnobValue,
			&RackPlugin::chainDragControl,
			&RackPlugin::chainUndo,
			&RackPlugin::chainResetControl,
			&RackPlugin::chainAbandonDrag,
			&RackPlugin::chainDeviceBody,
			&RackPlugin::chainEditDeviceBody,
			&RackPlugin::chainNoModDevice,
			&RackPlugin::chainMaxFrames,
			&RackPlugin::chainPrepareCount,
			nullptr, // device_reading: the rack's cases read `RackBridge::readingAt` directly
			nullptr, // set_core_route: the rack has no core destinations; its cases route through `testSetRoute`
			nullptr,
			nullptr,
			nullptr,
			nullptr, // set_device_route
			nullptr, // delivered_note: the rack has no voices
			nullptr, // set_device_block: not this host's road yet
			&RackPlugin::chainSetPresetIdentity, // the rack plugin's own browser (backlog QC)
			&RackPlugin::chainPresetIdentity,
			&RackPlugin::chainPresetFace,
			&RackPlugin::chainSetDevicePreset,
			&RackPlugin::chainDevicePreset,
			nullptr, // voice_reading: the rack has no voices
			nullptr, // release_control (/10): the rack's road is `RackBridge::releaseField`, asked directly by its cases
			nullptr, // synth_reading (/11): the rack has no voices
			&RackPlugin::chainParamIndexOf,
			&RackPlugin::chainParamInfoBuilds,
		};
		if (id != nullptr && std::strcmp(id, CLAP_EXT_FEATHERS_RACK_TEST) == 0) {
			return &kTest;
		}
		if (id != nullptr && std::strcmp(id, CLAP_EXT_FEATHERS_TEST_CHAIN) == 0) {
			return &kChain;
		}
		return nullptr;
	}

	/// THE STANDALONE RACK IS ITS OWN HOST, SO IT HANDS ITSELF THIS BUILD'S DEVICES — the same engine a
	/// rack inside a chain runs, handed its host's list instead.
	RackEngine engine_ { kRackAxes, hostCatalogue() };
	/// **THE EDITOR'S HOST, OWNED HERE AND NOT BY THE GUI.** The mirrors are published from `process`
	/// and the GUI does not exist between editor closes, so the bridge must outlive it — the same
	/// reason Feathers' plugin owns `ParamBridge`. See rack_bridge.h for the contract.
	///
	/// **AND THE OUTERMOST RACK, SO IT OWNS THE HOST SLOT POOL** (T10, plan §0.5): a rack inside a chain is
	/// built without one (`RackBridge::HostSlots::None`).
	RackBridge bridge_ { engine_, nullptr, RackBridge::HostSlots::OwnPool };

	/// **THE HOST SLOTS' VIEW OF THE DEVICES** — the bridge's pool answers what a target is and where it stands.
	struct BridgeControlLookup final : hostslots::ControlLookup {
		explicit BridgeControlLookup(const RackBridge& b) noexcept : bridge(b) {}
		bool lookup(const hostslots::Target& target, const device::ParamDesc** desc, double* value) const override {
			const rack::SlotOwner* slots = bridge.hostSlots();
			try {
				return slots != nullptr && slots->resolve(target, desc, value);
			}
			catch (...) {
				return false;
			}
		}
		bool textSite(const hostslots::Target& target, hostslots::TextSite* site) const override {
			const rack::SlotOwner* slots = bridge.hostSlots();
			DeviceInstanceId device = kNoInstance;
			try {
				if (slots == nullptr || !slots->textSite(target, &device, site)) {
					return false;
				}
			}
			catch (...) {
				return false;
			}
			site->surface = bridge.containerSurfaceOf(device);
			return true;
		}
		const RackBridge& bridge;
	};
	BridgeControlLookup slotLookup_ { bridge_ };
	/// The slots as CLAP sees them (`hostslots/slot_host.h`), over the bridge's pool. After `bridge_`.
	hostslots::SlotHost slotHost_ { bridge_.hostSlots()->pool(), slotLookup_ };
	/// What the pool's renames owe the host, folded into the next `rescanParams` (`serviceSlots`).
	clap_param_rescan_flags slotRescanOwed_ = 0;
	/// Every `paramsInfo` call, counted for the test seam's `param_info_builds`. Main thread, as `paramsInfo` is.
	mutable uint64_t infoBuilds_ = 0;
	/// THE TEST SEAM'S UNDO HISTORY (`drag_control`, `undo`) — a history like the editor's; its edits hold the
	/// bridge by reference, and the bridge is declared first.
	ui::EditHistory testHistory_;

	RackGui gui_ { bridge_ };
	/// **THE NOTE RACK** (`kNoteRackDescriptor`): the same engine and editor with no audio ports and a notes-only offer.
	bool noteRack_ = false;
	/// Its audio: silence the engine runs on, sized at activate (a note rack is handed no buffers).
	std::vector<float> scratchL_, scratchR_;
	int guiFd_ = -1;
	LoadReport lastRefusals_ {};

	/// THIS BLOCK'S INPUT NOTES, translated from CLAP once and handed to the engine whole.
	std::array<device::NoteEvent, RackEngine::kMaxNotes> notesIn_ {};
	size_t noteCount_ = 0;
	/// THE TRANSPORTS THIS BLOCK CARRIED INSIDE IT, from `drain` — see `RackEngine::setTransportAnchors`.
	std::array<RackEngine::TransportAnchor, RackEngine::kMaxTransportAnchors> transportAnchors_ {};
	size_t transportAnchorCount_ = 0;

	/// Inside `stateLoad`'s `loadPatch`: renames are recorded, not sent (`onMacroRenamed`).
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
	/// **THIS INSTANCE'S LINES IN THE EVENT LOG** (plugin/event_log.h). Main thread only, like every caller.
	eventlog::Instance log_ { kDescriptor.id };
	/// **DECLARED AFTER `bridge_`, SO DESTROYED BEFORE IT** — the callback captures `this`.
	device::Subscription macroRenamed_;
};

} // namespace

const clap_plugin_descriptor* descriptor() noexcept {
	return &kDescriptor;
}

namespace {

//------------------------------------------------------------------------------------------------//
// THE FACTORY — two plugins (the Chain, the Note Rack), and the entry points a host dlsyms its way to.        //
//------------------------------------------------------------------------------------------------//

uint32_t CLAP_ABI factoryPluginCount(const clap_plugin_factory*) noexcept {
	return 2; // the Chain, and the Note Rack
}

const clap_plugin_descriptor* CLAP_ABI factoryPluginDescriptor(const clap_plugin_factory*,
                                                               uint32_t index) noexcept {
	return index == 0 ? &kDescriptor : index == 1 ? &kNoteRackDescriptor : nullptr;
}

const clap_plugin* CLAP_ABI factoryCreatePlugin(const clap_plugin_factory*, const clap_host* host,
                                                const char* pluginId) noexcept {
	if (host != nullptr && pluginId != nullptr && std::strcmp(pluginId, kNoteRackDescriptor.id) == 0) {
		return createNoteRack(host);
	}
	if (host == nullptr || pluginId == nullptr || std::strcmp(pluginId, kDescriptor.id) != 0) {
		return nullptr;
	}
	return create(host);
}

const clap_plugin_factory kPluginFactory = {
    factoryPluginCount,
    factoryPluginDescriptor,
    factoryCreatePlugin,
};

} // namespace

bool CLAP_ABI entryInit(const char*) {
	eventlog::arm(); // a host loaded us: the default log file is armed (plugin/event_log.h)
	return true;
}

void CLAP_ABI entryDeinit() {}

const void* CLAP_ABI entryGetFactory(const char* factoryId) {
	return factoryId != nullptr && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0
	           ? &kPluginFactory
	           : nullptr;
}

const clap_plugin* create(const clap_host* host) noexcept {
	auto* plugin = new RackPlugin(host, false);
	return plugin->clapPlugin();
}

const clap_plugin* createNoteRack(const clap_host* host) noexcept {
	auto* plugin = new RackPlugin(host, true);
	return plugin->clapPlugin();
}

const clap_plugin_descriptor* noteRackDescriptor() noexcept {
	return &kNoteRackDescriptor;
}

} // namespace feathers::rack
