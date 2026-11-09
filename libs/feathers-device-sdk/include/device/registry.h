/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/device.h"
#include "device/ears.h"
#include "device/endpoint.h"
#include "device/state_blocks.h" // `StateOps`, a device's keyed blocks (backlog NM)
#include "device/modulatable.h" // `HostSlotStatus`, what a nested control's host-slot query answers in
#include "hostslots/slot_bus.h" // the one bus every level's engine reads (`HostSlotLink::bus`); std only

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string>

// WHAT A HOST CAN LEARN ABOUT A DEVICE WITHOUT KNOWING WHICH DEVICE IT IS.
//
// device.h says what ONE control is. This file says what a whole DEVICE is, in the same vocabulary
// and under the same rule: it names no concrete device, includes nothing from src/plugin, and is
// the header a third-party device would compile against. THE HOST'S ACTUAL LIST OF DEVICES IS NOT
// HERE — it names DAHDSR and the filter, so it lives in src/plugin/device_registry.h where naming
// them is allowed. Putting the table here would make the device vocabulary depend on two devices,
// which is the boundary this whole module exists to refuse.
//
// WHY IT EXISTS: params.h USED TO READ
//
//     inline constexpr size_t kRackControlsPerSlot = device::modulatableIn(mod::dahdsr::kParamDescs);
//
// — the plugin sizing its entire rack destination space from ONE DEVICE, by name. Every rack slot was
// assumed to hold something with exactly that many modulatable controls, so a second modulator type
// with a different count could not exist however large the slot count grew. The include was the pin;
// the count was only its symptom. Both are gone, and a runtime answer to "what is actually in this
// slot" is what replaced them.
//
// WHAT THIS DELIBERATELY DOES NOT CARRY, AND WHY THE ABSENCE IS THE DESIGN. There are no
// construct/destruct function pointers and no storage size, because nothing yet instantiates a
// device through a pool — the rack still holds a concrete Settings struct. device.h's own warning
// applies to this file as much as to the interface it was written about: an abstraction written
// before its user "acquires the methods its author imagined instead of the ones its users need". A
// device pool is a real design with real realtime constraints (no allocation after activate, and the
// ABA hazard CLAUDE.md describes, since a pool exists precisely to reuse slots at fixed addresses).
// It gets designed when something needs it, against that something.
//
// So this is a DECLARATIVE registry: what devices exist and what each one says about itself.

namespace feathers::device {

/// ONE MODULATION SIGNAL A DEVICE PUBLISHES.
///
/// A DEVICE DECLARES ITS OUTPUTS RATHER THAN HAVING THEM INFERRED FROM WHERE IT SITS, and that is
/// the whole reason this struct exists rather than a bare count.
///
/// In the future, modulator effects will also be note/midi effects, which also inject the modulation
/// signal into the framework. That need not be over-optimized early, but it is kept in mind.
///
/// The assumption being avoided is *a device's lane implies its capabilities* — "it is in the rack,
/// therefore it has one modulation output; it is in the fx lane, therefore it has none". That is the
/// same shape as the pin above: a fact about one device baked into the host's structure. A note
/// effect that both passes notes and publishes a modulation source breaks it immediately, and it
/// breaks it in the one place that is expensive to move, because a SOURCE INDEX is what a saved
/// patch stores.
///
/// Declaring outputs costs a field and buys that door staying open. It is not the note lane, which
/// is explicitly not being built.
/// NO CONTROL MOVES THIS OUTPUT'S POLARITY — see `SourceDesc::bipolarControl`.
inline constexpr int kPolarityFixed = -1;

struct SourceDesc {
	/// **THE NAME A PATCH'S ROUTE CALLS THIS OUTPUT BY** (backlog JF §3.3, §3.8) — `ParamDesc::key`'s rules:
	/// `[a-z0-9_.]`, unique among the device's outputs, required, and it names a MEANING. First, for the
	/// same reason.
	const char* key = nullptr;
	/// Qualified by the device, for exactly the reason ParamDesc::name is — a device does not get to
	/// assume what else is in the list it lands in. "Envelope", not "Out".
	const char* name;

	/// Does this publish -1..1 rather than 0..1? The host needs it to draw a source's resting
	/// position and to decide what an amount of 1.0 means from it.
	///
	/// DECLARED BY THE DEVICE BECAUSE ONLY THE DEVICE KNOWS. The DAHDSR's is a runtime setting (its
	/// Bipolar switch), so what it declares here is the WIDEST range it can publish, not what it
	/// happens to be doing this block — a host that resized its thinking every time a switch moved
	/// would be a host whose route amounts change meaning under the user's hand.
	bool bipolar = false;

	/// **WHICH CONTROL SWITCHES THAT POLARITY RIGHT NOW, or `kPolarityFixed` when nothing does.**
	///
	/// **ONE FLAG WAS ANSWERING TWO QUESTIONS AND THEY HAVE DIFFERENT RIGHT ANSWERS.** `bipolar`
	/// above is the WIDEST signal this output can publish, and the paragraph above is right that
	/// ROUTING must be sized by it: an amount whose meaning changed when a switch moved would be a
	/// route that quietly re-scaled under the user's hand. **DRAWING is the other question.** A
	/// surface asking "which way does this reach from the notch" is asking what the source is doing
	/// NOW, and answering it from the widest declaration draws a reach the route does not currently
	/// have — the exact objection that argued for declaring the widest, arriving from the other side.
	///
	/// The model this implements: when the modulator pulls negative, the dot is ccw of the knob notch;
	/// if the modulation amount is negative, the amount arc goes ccw of the notch; positive modulation
	/// applied negatively pulls below the knob notch. The envelope publishes 0..1 — `Env Bipolar` defaults OFF — so a negative
	/// amount on it reaches one way, downward, and the symmetric arc was drawing a range nothing
	/// could reach.
	///
	/// **`kPolarityFixed` IS NOT "UNIPOLAR"**, and the tracker is the reason to say so: its Pitch
	/// output declares `bipolar = true` with no switch, because a note below the root really does
	/// modulate the other way — always. Fixed means *this output's polarity is whatever `bipolar`
	/// says and no control moves it*, which is the honest answer for both a permanently bipolar
	/// source and an ordinary unipolar one.
	///
	/// A CONTROL INDEX RATHER THAN A CALLBACK, for the reason the rest of this registry is
	/// declarative: a host reads the field it already knows how to read, and a device that gains a
	/// switch says so in one line rather than shipping code the host has to call.
	int bipolarControl = kPolarityFixed;
	/// **WITH `bipolarControl` A CHOICE: THE ONE ENTRY THAT MAKES THIS OUTPUT BIPOLAR** (backlog OP), or -1 for the
	/// switch reading (≥ 0.5 is bipolar). The Follower's Env is bipolar under its Signed detector alone.
	int bipolarChoice = -1;

	/// **DOES THIS OUTPUT'S VALUE DIFFER FROM ONE VOICE TO THE NEXT?** — backlog DU's decision point 1,
	/// declared rather than inferred.
	///
	/// There is a migration path where modulators become intrinsically per voice, while destinations
	/// have a default trivial handling until they migrate. DU spells out the trap that makes this a DECLARATION and not a host guess:
	/// *"'INTRINSICALLY PER VOICE' MUST MEAN THE VALUE TYPE, NOT THE INSTANCE. A rack LFO must stay
	/// ONE instance producing one number — twenty LFOs where one would do is pure cost for a source
	/// whose value is identical across voices."*
	///
	/// **SO A SOURCE SAYS WHETHER IT VARIES, and only those cost anything per voice.** An LFO, an
	/// envelope and a tracker all say false and are read exactly as they were; the note-expression
	/// device says true, because pressure on one finger is not pressure on another. **False is the
	/// answer for every device that existed before this flag**, which is what makes "day one is
	/// bit-identical" a thing a case can assert rather than a thing to argue.
	///
	/// **AND THE COLLAPSE RULE IS NOT HERE, deliberately.** The collapse rule belongs to the SOURCE, not
	/// the destination — but it belongs to the source as a musical CHOICE, not as a
	/// static fact, so it is a control on the device (`noteexpr::Param::Rule`) rather than a field
	/// here. This flag answers *"is there anything to collapse"*; the device answers *"how"*.
	bool perVoice = false;
	/// **THIS OUTPUT IS ALWAYS AN EAR PAIR** (backlog KU) — the Stereo Tool's Identity and Pan. Answered through
	/// `ModOps::outputPairAt`.
	bool pairAlways = false;
	/// **…OR A PAIR WHENEVER ONE OF THESE OF THE DEVICE'S OWN CONTROLS IS STEREO-LIVE** — bit n is control n
	/// (n < 32), and each named control is declared `stereo` (a case pins it). This is how "can emit a pair"
	/// passes THROUGH a device that only reshapes what reaches it: Math (A, B, Amount), each MACROS output (its own
	/// macro), the Stereo Tool's Width (In). Worked out at the drain, to a fixed point (`RackEngine::rebuildInputs`).
	uint32_t pairFrom = 0;
	/// **…OR A PAIR WHILE THIS CONTROL OF THE DEVICE'S OWN IS NOT ZERO** (backlog PA) — the LFO's Stereo Phase, a
	/// native pair: at 0 the output is mono and costs nothing, as `pairAlways`' absence does. Read from the control's
	/// BASE value at the drain (`RackEngine::rebuildInputs`), so it names a control no route reaches; a host write that
	/// moves it to or from zero re-runs the drain (`RackEngine::setField`). -1 is none.
	int pairWhile = -1;
	/// **NOT ON THE CARD UNTIL THE CARD SAYS SO** (Pattern design §7.10) — the card shows this output only when it asks for it
	/// (`DeviceHost::setOutputsShown`); before it asks, and on a host that keeps no mask, it is not drawn. A route to it
	/// reads as ever: this is view, not routing. The window shows every output.
	bool cardHidden = false;
};

/// HOW A HOST RUNS A DEVICE THAT PROCESSES AUDIO — and NULL IS THE CAPABILITY.
///
/// A device that has one of these can sit in an audio chain; a device that leaves it null cannot,
/// and that is the whole statement. There is no `processesAudio` flag and no `lane` field.
///
/// WHY THE ENTRY POINT RATHER THAN A BOOL: a function pointer cannot disagree with a flag beside it.
/// The same principle as FxDevice holding a DeviceRow rather than duplicating its identity, and as
/// CLAUDE.md's ABA rule — the fact travels with the thing that makes it true, because a second copy
/// is the copy that goes stale.
///
/// THIS COMPLETES THE ARGUMENT THIS FILE ALREADY MAKES RATHER THAN CONTRADICTING IT. The claim is
/// that where a device may sit is derivable from its own declarations, and that claim was right —
/// the vocabulary was one declaration short. Measured across all four devices before adding it:
///
///     publishes outputs        -> can be a modulation SOURCE   (rack membership, and it works:
///                                 outputCount() > 0 refuses the filter and HARM by itself)
///     has modulatable controls -> can be a DESTINATION          (all four have some, so it is not
///                                 the discriminator anybody hoped)
///     processes audio          -> can be in an AUDIO CHAIN      (nothing could say this)
///
/// A device that is two of them is still expressible, which is the property a `lane` field destroys
/// and the reason feathers-1a guarded against one.
///
/// STATE IS THE HOST'S TO PLACE. The device says how big it is and how to build it; where it lives
/// is the host's business, because the host is the one under a no-allocation-after-activate rule.
/// WHAT THE HOST KNOWS AND THE PATCH CANNOT.
///
/// A device's row carries what a patch stores. THIS CARRIES WHAT ONLY THE CALLER KNOWS — which note is
/// sounding and whether it is still held. Both are per-render and neither belongs in a saved row: a
/// patch that stored them would restore a note nobody is playing.
///
/// IT EXISTS BECAUSE setFields COULD NOT EXPRESS IT, and the omission had teeth. The filter's
/// keyOffsetSemitones lives in its Settings but not in its Param enum, so decoding a row yields a
/// fresh State with the offset at zero — meaning a device driven entirely through AudioOps filters at
/// a FIXED corner. It still filters, which is what makes it the partial effect CLAUDE.md warns about:
/// a check that the chain renders passes either way.
///
/// ONE PITCH, NOT A NOTE AND A BEND. The sounding pitch is the sum of note, glide and bend, and every
/// caller already has it as one number (Synth::renderVoices reads voice.lastTargetSemitones). Handing
/// over the parts would invite a device to add what the host had already added — and a device that
/// wants its own detune has it in its row, where it is patch state.
struct NoteContext {
	/// The full sounding pitch in semitones, A4 = 69. Glide and pitch bend are already folded in.
	float pitchSemitones = 60.0f;
	/// Whether a note is being held. Derived by the caller from the voices' own stages.
	bool gate = false;
	/// **THE VOICE'S STACK: how many parts sound in it, and the fan of cents across them** (voice stacking, backlog
	/// KE). The parts are summed BEFORE the per-voice devices, so a device whose sound depends on its input level
	/// needs to know how coherently they add (the Smooth Shaper's stack trim). The defaults are
	/// the unstacked voice, so a caller that says nothing describes a stack of one and no device changes.
	int stackCount = 1;
	float stackDetuneCents = 0.0f;
	/// **HOW ALIKE THE PARTS ARE AT 0 CENTS**: the coherence they would sum with if they did not beat
	/// — Σ_{i≠j} c_ij / ((N − 1) Σ_i c_ii), c_ij the mean over one cycle of the tables parts i and j play. 1 when every
	/// part plays the same table (a plain stack, exactly); below 1, and even below 0, when a Pos route or Width puts the
	/// parts on stands of their own. The synth computes it where it knows the stands (`Synth::updateStackCoherence`).
	float stackCoherence = 1.0f;
};
/// A HOST'S EAR FOR A DEVICE'S METERS — see `AudioOps::setReadingsSink`. Two words, so a device stores it
/// by value and calls it without owning anything of the host's.
struct ReadingsSink {
	void* context = nullptr;
	void (*publish)(void* context, const float* values, size_t count) = nullptr;
};


/// **HOW A RACK INSIDE A HOST READS THE HOST'S SOURCES** — rack roadmap §7 (the
/// address of the source is published and 'destinations' are actually volunteering to 'read' the
/// source, so only read capability need be passed in for upstream routes). Handed to a chain
/// device at seating (`AudioOps::setOuterSources`), read by its inner engine when it rebuilds its
/// inputs and when its gate is asked. `s` is spelled the OUTER host's way — the scope bit already off.
/// Function pointers over `void*`, the vocabulary's idiom; `self` is the chain's own id in the outer
/// host, so the upstream question is asked of the right node.
struct OuterSources {
	const void* host = nullptr;
	DeviceInstanceId self = kNoInstance;
	/// The block-rate cell this outer source writes, or null for one the host does not publish. Valid
	/// while the outer graph stands; the outer host hands a new reader whenever that changes.
	const float* (*cell)(const void* host, Source s) = nullptr;
	/// Whether `s` is upstream of the chain `self` in the outer host — the ordering half of the outer
	/// host's own gate, asked with the chain as the destination.
	bool (*upstream)(const void* host, DeviceInstanceId self, Source s) = nullptr;
	/// **HOW LATE THE CHAIN `self` STANDS IN THE OUTER HOST** (backlog LW): its arrival latency there, in samples. An outer
	/// source is read that much later inside, on top of the inner device's own arrival. Null: 0.
	uint32_t (*arrival)(const void* host, DeviceInstanceId self) = nullptr;

	//--------------------------------------------------------------------------------------------//
	// THE PER-VOICE HALF.                                                                           //
	//--------------------------------------------------------------------------------------------//
	//
	// A source that varies per voice publishes a RUN of cells beside its collapsed one
	// (`SourceDesc::perVoice`; the lane's `Input::perVoice`). The host's voices run INSIDE a chain
	// (rack roadmap §8, the pass-through model — `ContainerOps::setVoices`), so the inner engine
	// resolves an outer per-voice source per inner voice exactly as the lane does its own: an inner
	// Poly device's voice `v` reads `run[v]`, and an inner mono device reads the collapsed cell, the
	// default-trivial handling every mono destination has.
	/// The run `[voices]` this outer source writes per voice, or null for one with a single value.
	const float* (*perVoice)(const void* host, Source s) = nullptr;
	/// How many voices the outer host runs — the run's length.
	size_t voices = 0;
};

/// **THE HOST OUTSIDE, FOR THE GATE INSIDE** — rack roadmap §7's message-thread half. A chain's inner
/// record asks it whether an outer source is upstream of the chain before it admits an inner route
/// reading that source. Handed at PROVISION (`ContainerOps::setOuterGate`), on the message thread,
/// before the body is attached or loaded — so a loaded patch's outer routes are gated too, panel or no
/// panel. `s` is spelled the outer host's way.
/// **WHAT A CONTAINER DROPPED** at its service — the count and the first pair by identity, the shape a
/// host's refusal notice already speaks (`rack::RouteRefusals`; this is the device-vocabulary twin, so a
/// container can answer without naming a host). `firstSource` is spelled the way the INNER table holds
/// it (outer-scoped); the host translates.
struct RouteDrop {
	size_t count = 0;
	Source firstSource = Source::nobody();
	Destination firstDestination = Destination::nobody();
};

struct OuterGate {
	const void* host = nullptr;
	DeviceInstanceId self = kNoInstance;
	bool (*upstream)(const void* host, DeviceInstanceId self, Source s) = nullptr;
	/// **AN EDIT INSIDE IS AN EDIT OF THE PATCH OUTSIDE** (backlog JV). A chain's body is saved in its host's
	/// patch, so a hand editing the chain has modified the host's patch, and only the host can tell the DAW.
	/// Null where the host keeps no dirty flag to raise; called on the message thread, by the edit.
	void* dirtyHost = nullptr;
	void (*markDirty)(void* dirtyHost) = nullptr;
	/// **THE HOLDER'S OWN CONTROL, WRITTEN BY WHAT IT HOLDS** (backlog NO): a Rack's chain level, mute and solo are kept in
	/// its body AND mirrored into the Rack's slot params' bases, so a route and its glow work over the real value. The
	/// container writes the mirror through this — the host's own field write, recording no undo step (the hand's edit
	/// recorded it). Null where the host has none; then nothing mirrors and the container hears its body.
	void (*setOwnField)(void* dirtyHost, DeviceInstanceId self, size_t control, float value) = nullptr;
	/// **WHY A POLY SWITCH IN HERE CHANGES NOTHING, or null** (backlog NO): a Rack hands its chains a gate saying so,
	/// because no host voice reaches inside a rack (`rack::parallel::Rack::setVoices`); a chain hands it on to every
	/// container it provisions, so a Chain inside a Rack says it too. Read by `DeviceHost::perVoiceUnavailable`.
	const char* perVoiceUnavailable = nullptr;
};

//------------------------------------------------------------------------------------//
// HOST SLOTS ACROSS A CONTAINER (backlog JU, T11; host slots design §7, plan §3).       //
//------------------------------------------------------------------------------------//

struct Descriptor;

/// **WHERE A DEVICE STANDS, FROM THE OUTERMOST PLUGIN DOWN** — the outer container's instance id, then each
/// nested container's, then the device's own. A slot's target is a path plus a control key (design §7):
/// instance ids are unique only within one level, so two racks' first devices are both id 1 and only the path
/// tells them apart. FIXED CAPACITY so a gesture can carry one up with no allocation; deeper than any rack may
/// nest (`rack::kMaxDepth`, static_asserted where both are visible). A path that would overflow is EMPTY,
/// which every reader treats as "no path" — never a shorter path that names somebody else.
struct SlotPath {
	static constexpr size_t kCapacity = 32;
	std::array<DeviceInstanceId, kCapacity> ids {};
	size_t depth = 0;

	[[nodiscard]] std::span<const DeviceInstanceId> span() const noexcept { return { ids.data(), depth }; }
	[[nodiscard]] bool empty() const noexcept { return depth == 0; }
	/// This path with `id` appended — or an EMPTY path when there is no room, or this one is empty.
	[[nodiscard]] SlotPath then(DeviceInstanceId id) const noexcept {
		SlotPath out;
		if (depth == 0 || depth >= kCapacity || id == kNoInstance) {
			return out;
		}
		out = *this;
		out.ids[out.depth++] = id;
		return out;
	}
	/// The path `[id]` — the outermost level's own device.
	[[nodiscard]] static SlotPath of(DeviceInstanceId id) noexcept {
		SlotPath out;
		if (id != kNoInstance) {
			out.ids[0] = id;
			out.depth = 1;
		}
		return out;
	}
	friend bool operator==(const SlotPath& a, const SlotPath& b) noexcept {
		if (a.depth != b.depth) {
			return false;
		}
		for (size_t i = 0; i < a.depth; ++i) {
			if (a.ids[i] != b.ids[i]) {
				return false;
			}
		}
		return true;
	}
};

/// ONE DEVICE A NESTED LEVEL HOLDS, AS IT REPORTS ITSELF UP (`HostSlotLink::report`): its id in that level and
/// its declaration — a static table, never freed, so the level above may keep the pointer.
struct HostSlotLevelDevice {
	DeviceInstanceId id = kNoInstance;
	const Descriptor* desc = nullptr;
	friend bool operator==(const HostSlotLevelDevice&, const HostSlotLevelDevice&) = default;
};

/// A NAME A CONTAINER GIVES ONE OF ITS OWN CONTROLS, overriding its declared caption in a slot's label — a
/// nested rack's renamed macro ("Chain · Wobble" rather than "Chain · Macro 1").
struct HostSlotCaption {
	size_t control = 0;
	std::string name;
	friend bool operator==(const HostSlotCaption&, const HostSlotCaption&) = default;
};

/// **THE POOL OUTSIDE, FOR THE CONTROLS INSIDE** — design §7: a control inside a nested rack takes its slot
/// from the OUTERMOST plugin, which alone has a host to declare parameters to. Modelled on `OuterGate` (plan
/// §0.4: the catalogue carries no identity) and handed down the same way, at provision, on the message thread,
/// before the body loads (`ContainerOps::setHostSlots`, made by `Converger::setSlotLinkMaker`).
///
/// **IT POINTS AT THE OWNER, NEVER AT A LEVEL IN BETWEEN.** `host` is the pool's owner (`rack::SlotOwner`, held
/// by Feathers' `ParamBridge` or the standalone rack's top `RackBridge`), whose address is fixed for the
/// plugin's life — the bus it hands every engine relies on the same. A level makes its children's links from
/// its own by APPENDING the child's id to `prefix` (`SlotPath::then`): that is "each level prepends itself" of
/// the plan, done once at the handover instead of on every call, so a request from any depth is one call to
/// the owner, and no call goes through a level that may already be gone (a rack's bridge is destroyed before
/// the engine that holds its children). NOTHING POINTS DOWN: the owner knows a nested level only by the path
/// and the declarations it reported, so a rebuilt editor, a re-provisioned rack or a destroyed one leaves
/// nothing dangling. Every call is the MESSAGE thread's, except `bus`, which the engines read.
///
/// A CONTAINER WITH NO LINK (a third party hosting our rack) answers every question "no slot".
struct HostSlotLink {
	void* host = nullptr;
	/// THIS CONTAINER'S PATH, from the outermost level, itself last. A device inside is `prefix.then(id)`.
	SlotPath prefix;
	/// WHICH OF THE CONTAINER'S CHAINS this link was handed to — SKLITTER holds two under one id, and both
	/// report under the same prefix; its two lanes issue ids from one counter, so a path still names one device.
	uint8_t lane = 0;
	/// THE ONE BUS, shared by every level: the engine of each level applies the slots bound to its own devices.
	hostslots::SlotValueBus<>* bus = nullptr;

	/// "THESE ARE MY DEVICES" — the level at `prefix` (and `lane`), in chain order. What the owner resolves a
	/// nested target against and names it by. Called when they change, and before any question about them.
	void (*report)(void* host, const SlotPath& prefix, uint8_t lane, std::span<const HostSlotLevelDevice> devices) =
	    nullptr;
	/// "MY OWN CONTROLS ARE CALLED THIS" — the container at `prefix` names some of its controls (a renamed macro).
	/// A slot on one is relabelled at once, so the host hears the rename.
	void (*captions)(void* host, const SlotPath& prefix, std::span<const HostSlotCaption> captions) = nullptr;
	/// "MY CONTROLS' TEXT CHANGED, THOUGH NO VALUE DID" — the container at `prefix` prints a value by its own state
	/// (the Rack's position names a chain by number, and a chain was added). Every slot at or under it is owed a
	/// rescan, through the same record a rename feeds (`SlotHost::rescanOwed`).
	void (*retext)(void* host, const SlotPath& prefix) = nullptr;
	/// THE RIGHT-CLICK'S QUESTION AND VERBS for the control `control` of the device at `path`.
	HostSlotStatus (*status)(const void* host, const SlotPath& path, size_t control) = nullptr;
	bool (*expose)(void* host, const SlotPath& path, size_t control, std::string* displaced) = nullptr;
	bool (*unpin)(void* host, const SlotPath& path, size_t control) = nullptr;
	/// A HAND TOOK THE CONTROL (host slots T9): the owner's recent ring gives it a slot, or bumps its recency.
	void (*touch)(void* host, const SlotPath& path, size_t control) = nullptr;
	/// A DEVICE AT `path` WAS REMOVED: its slots, and every slot under it, are freed (with notes, for an undo).
	void (*freeUnder)(void* host, const SlotPath& path) = nullptr;
	/// THE UNDO OF THAT (backlog OA): a device put back at `path` under its old id gets its freed slots back from
	/// their notes, where nothing took them meanwhile.
	void (*restoreUnder)(void* host, const SlotPath& path) = nullptr;
	/// A DEVICE MOVED A LEVEL (backlog OA, a group and its undo): every slot and note under `from` now names the
	/// same control under `to`. False when the pool refuses it whole (it would put two slots on one control).
	bool (*rebase)(void* host, const SlotPath& from, const SlotPath& to) = nullptr;
	/// A HAND TOOK / LET GO OF A CONTROL BOUND TO `slot`; and a hand's value on it (`hand`: a hand is on the
	/// control now, so a gesture opens if none is), as a normalised 0..1 for the host.
	void (*beginGesture)(void* host, const SlotPath& path, size_t control, uint32_t slot) = nullptr;
	void (*endGesture)(void* host, const SlotPath& path, size_t control) = nullptr;
	void (*handValue)(void* host, const SlotPath& path, size_t control, uint32_t slot, float value01, bool hand) =
	    nullptr;
	/// The pool's generation — a knob's mark caches on it (`DeviceHost::hostSlotGeneration`).
	uint64_t (*generation)(const void* host) = nullptr;
	/// **WHICH SLOTS DRIVE CONTROLS DIRECTLY UNDER `prefix`**, by device id, tag and key — each level resolves the
	/// key against its own record, so a binding that reached a level whose device changed under the id binds
	/// nothing. `bindingGeneration` moves whenever any level's answer may have; a level pulls when it moved.
	uint64_t (*bindingGeneration)(const void* host) = nullptr;
	void (*bindingsUnder)(const void* host, const SlotPath& prefix, void* ctx,
	                      void (*fn)(void* ctx, uint32_t slot, DeviceInstanceId device, const char* tag,
	                                 const char* key)) = nullptr;
	/// **"MY RECORD NOW BINDS WHAT YOU SAID AT `bindingGeneration`, AND IS PUBLISHED"** — the remap window across
	/// levels: a slot moved into or out of a level has no declaration on the audio thread until every level
	/// has said this, so a host write is never scaled for one control and applied by an engine to another.
	void (*adopted)(void* host, const SlotPath& prefix, uint8_t lane, uint64_t bindingGeneration) = nullptr;

	[[nodiscard]] bool live() const noexcept {
		return host != nullptr && !prefix.empty() && bus != nullptr && report != nullptr;
	}
	/// THE SAME LINK, for the level the device `self` inside this one holds — or none, from a level with none.
	[[nodiscard]] HostSlotLink forChild(DeviceInstanceId self) const noexcept {
		HostSlotLink out;
		if (!live()) {
			return out;
		}
		out = *this;
		out.prefix = prefix.then(self);
		out.lane = 0;
		return out.prefix.empty() ? HostSlotLink {} : out;
	}
	/// Whether two links reach the same owner at the same place — a re-handed link that changes nothing.
	[[nodiscard]] bool sameAs(const HostSlotLink& o) const noexcept {
		return host == o.host && prefix == o.prefix && lane == o.lane && bus == o.bus && report == o.report;
	}
};

/// **WHAT A RACK THAT HAS NOT BEEN PREPARED YET PREPARES ITS DEVICES WITH.** A rack builds devices before any
/// host has activated it (a UI opens first, a case builds and renders), at `RackEngine`'s stand-in rate, and it
/// needs a stand-in maximum beside it. The host's `activate` replaces both before a block of the host's is
/// rendered. 4096 because it is the number the two scratch owners chose before `prepare` carried one (backlog GO).
inline constexpr int kDefaultMaxFrames = 4096;

/// **THE MOST SCRATCH A DEVICE SIZES FROM AN ANNOUNCED MAXIMUM.** CLAP lets a host announce anything up to
/// INT32_MAX, and a maximum is a PROMISE about the longest block, not a budget: an offline render that announces
/// a million frames must not cost every scratch owner megabytes per buffer. A device sizes its scratch to
/// `scratchFrames(maxFrames)` and cuts anything longer — the same guard that covers a host breaking its promise.
inline constexpr int kMaxScratchFrames = 65536;
[[nodiscard]] constexpr int scratchFrames(int maxFrames) noexcept {
	return maxFrames < 1 ? 1 : (maxFrames > kMaxScratchFrames ? kMaxScratchFrames : maxFrames);
}

struct AudioOps {
	size_t stateBytes;
	size_t stateAlign;

	void (*construct)(void* storage);
	void (*destruct)(void* storage);
	/// **THE RATE AND THE LONGEST BLOCK** (backlog GO) — the only moment a device may allocate, so the only moment it
	/// can size a scratch buffer. `maxFrames` is the host's (CLAP's `activate` `max_frames_count`), carried by every
	/// host down to here; a host may hand `processStereo` fewer frames but never more. A device that keeps scratch
	/// still CUTS a longer call to its scratch, because a promise is not a proof: a nested chain, a test driving a
	/// device directly, or a host that breaks the contract all reach `processStereo` without asking.
	void (*prepare)(void* storage, double sampleRate, int maxFrames);

	/// FORGET THE STATE AND THE FACT THAT IT WAS RUNNING — "as if just switched on", not merely "with
	/// the memory cleared". The distinction is audible: a filter that forgets its delay line but
	/// remembers it was active ramps its coefficients in from the last note's, so a staccato line
	/// hears the previous note's corner for ten milliseconds.
	///
	/// THIS WAS SPELLED OUT ONLY WHEN SOMETHING FINALLY CALLED IT. Until the per-voice runner landed
	/// this entry point had NO caller anywhere outside tests — FxRunner::reset destroys its instances
	/// rather than resetting them — and an uninterrogated capability drifts to whatever its
	/// implementor guessed. The filter's had drifted to the weaker of the two meanings and nothing
	/// could see it, which is the same gap this file's own comment records AudioOps shipping into.
	void (*reset)(void* storage);

	/// WHICH SLOT THIS DEVICE IS IN, so per-slot state (like meters) can be keyed by slot. Called by
	/// the lane after construction, before any audio is processed. nullptr for devices that don't need
	/// slot-keyed state.
	void (*setSlot)(void* storage, size_t slot) = nullptr;

	/// SETTINGS ARRIVE AS THE DEVICE'S OWN ROW, raw, with the count of fields that are really there —
	/// the rule plugin/device_row.h states, in the one form this header can take: a bare pointer and a
	/// length, because the device vocabulary cannot see the host's DeviceRow. The device decodes it
	/// with its own stateOf, which is what puts absent fields at their declared defaults.
	void (*setFields)(void* storage, const float* fields, size_t fieldsPresent);

	/// WHICH NOTE IS SOUNDING, or nullptr for a device that does not care — and the null IS the
	/// declaration, exactly as `audio` itself is. A device with no note road cannot be handed a note it
	/// will ignore, and there is no bool beside it to disagree with.
	///
	/// ORDER-INDEPENDENT BY CONTRACT: a device that uses both must latch each and recompute, because a
	/// host may deliver them in either order and will deliver fields far less often than notes.
	void (*setNoteContext)(void* storage, const NoteContext& note) = nullptr;

	/// **HOW HARD THIS DEVICE IS BEING DRIVEN, 0..1, OR NOTHING.** Optional, like `setSlot` and
	/// `setNoteContext` above: a device that has no such reading leaves it null and every surface then
	/// draws no meter, which is `DeviceHost::drivePeakAt` declining rather than answering a plausible
	/// zero.
	///
	/// THE UNIT IS THE ONE A TRANSFER PLOT NEEDS: the peak INPUT the device is seeing, in the same 0..1
	/// its x axis is drawn in. A device metering something else — gain reduction, output level — should
	/// leave this null rather than answer in its own units, because the caller cannot tell them apart.
	 float (*drivePeak)(const void* storage) = nullptr;

	/// WHAT TIME IT IS IN THE SONG — the host's tempo, handed to a device that syncs to it.
	///
	/// **THE FX LANE HAD NO CLOCK UNTIL NOW, AND THAT WAS A REAL GAP RATHER THAN AN OVERSIGHT.** The
	/// modulator rack has had one since the MSEG needed a window; an fx device's whole contract was
	/// `prepare(sampleRate)` plus `processStereo`, so nothing here could know a tempo. The
	/// automodulator's LFO wanted a synced rate and could not have one — the control was left
	/// UNDECLARED rather than shipped dead, and the absence was noticed, which is the honesty rule
	/// working as intended.
	///
	/// A SETTER RATHER THAN AN ARGUMENT, matching `setNoteContext` and for the same reason:
	/// `processStereo` is a hot loop with a fixed shape, and a tempo changes about as often as a note
	/// does. The lane calls this once per block, before processing.
	///
	/// NULLPTR IS THE DECLARATION, as everywhere else in this struct: a device that does not sync
	/// says so by not implementing it, and the lane skips it. Five of the six do exactly that — this
	/// widens the contract without obliging anybody.
	///
	/// AND IT IS `device::TransportClock`, WHICH MOVED HERE TO MAKE THIS POSSIBLE. It lived in
	/// `mod/modulator.h`, whose own comment predicted this: "the fx lane will want it the day
	/// anything there syncs, and a device in that lane should not have to read 'rack' to find out
	/// what time it is." `mod::TransportClock` is now an alias, so nothing in the rack changed.
	void (*setTransportClock)(void* storage, const TransportClock& clock) = nullptr;

	void (*processStereo)(void* storage, float* left, float* right, int frames);

	/// ONE CHANNEL, or nullptr for a device that only has a stereo road.
	///
	/// IT EXISTS BECAUSE THE PER-VOICE ROAD IS SOMETIMES MONO. A voice renders into one buffer
	/// whenever the character bank's stereo window is shut, and Synth's mono chunk path has no second
	/// channel to offer — it passed `nullptr` to the hardcoded per-voice filter, which called
	/// `processMono` by hand. A generic per-voice runner cannot do that: `processStereo` has no
	/// nullptr-right convention, and inventing one would mean every device must remember to check.
	/// HARM tolerates a null right by accident; MultimodeFilter dereferences it and would crash.
	///
	/// SO THE MONO ROAD IS DECLARED RATHER THAN CONVENTIONAL, on this header's own rule: the null IS
	/// the capability, and a host holding a mono buffer for a device that has no mono road knows to
	/// give it two channels instead of hoping.
	void (*processMono)(void* storage, float* mono, int frames) = nullptr;

	/// **WHERE THIS DEVICE'S METERS GO — a sink the HOST installs, once, when it adopts the instance.**
	/// Optional, like `drivePeak`: a device with nothing to meter leaves it null. A device that has one
	/// calls `sink.publish(sink.context, values, count)` from its process, every block, with the readings
	/// in the order its surfaces read them back (`DeviceHost::readingAt`) — OWLTT's ten, automod's two,
	/// bus utility's `Reading::kCount`. **ONE ROAD FOR EVERY HOST**: until this, meters
	/// reached a surface only through per-device callbacks a plugin wired for its own lane, so a device
	/// inside a chain — or in the standalone rack — metered nothing. The engine installs this on every
	/// device it adopts (`RackEngine::publishReadings`), and a bridge mirrors it to the surfaces.
	///
	/// A LATER INSTALL REPLACES THE EARLIER — Feathers' lane still wires its own callbacks after the engine
	/// has, and keeps its road; the two are meant to become one, this one.
	void (*setReadingsSink)(void* storage, ReadingsSink sink) = nullptr;
	/// **THE HOST OUTSIDE, READABLE FROM INSIDE** — rack roadmap §7. A device that is itself a rack (the
	/// chain) is handed, at seating, how to read the sources of the host it sits in and whether one
	/// is upstream of it; its inner routes may then name an outer source (`Source::isOuter`). Every
	/// other device leaves this null. A setter like `setNoteContext`, and for the same reason: the
	/// reader is a property of where the device SITS, which the block does not carry.
	void (*setOuterSources)(void* storage, const OuterSources& outer) = nullptr;

	//--------------------------------------------------------------------------------------------//
	// A TWO-STAGE DEVICE — rack roadmap §8, the pass-through model. A device that holds devices runs   //
	// the host's two stages INSIDE itself on ONE instance: the host's per-voice stage hands it each    //
	// sounding voice's channel, and its bus stage runs as any device's. It has no voice instances.    //
	//--------------------------------------------------------------------------------------------//

	/// **THE HOST'S BLOCK BEGINS — SAY WHETHER YOU RUN PER VOICE IN IT.** Called by the host's begin-block
	/// once per block, after the device's fields for the block are set, and it is the device's DRAIN
	/// POINT: a container adopts its pending edits here and nowhere else while hosted. It answers whether
	/// any device inside it is per voice this block (its own begin-block decides), and then takes the
	/// host's per-voice calls for it. NO FRAME COUNT, because the host may not know one — Feathers' lane
	/// begins a segment whose length the render decides chunk by chunk — so a container clocks its own
	/// modulators off the audio its bus stage is handed, not off this call. Null for a device that is
	/// not two-stage.
	bool (*beginPerVoiceBlock)(void* storage) = nullptr;
	/// **ONE VOICE'S CHANNEL, THROUGH THE DEVICES INSIDE THAT RUN PER VOICE** — the host's per-voice stage,
	/// for a sounding voice, on the one instance, with that voice's note. The bus stage (`processStereo`)
	/// still runs afterwards, on the sum.
	///
	/// **BOTH CHANNELS WHEN THE VOICE HAS TWO, `right` NULL WHEN IT HAS ONE** — the same pair the host hands a
	/// Poly device's own voice instance. It once took one channel (backlog KT), so in a voice
	/// rendered in stereo (a spread stack, stereo terrain, FM, Resonate's width) the right channel passed
	/// every Poly device inside a chain dry: a Poly filter in a chain filtered the left ear only, measured
	/// at -26.9 dB on the left and 0 dB on the right at the seventh partial of C4.
	void (*processVoice)(void* storage, size_t voice, float* left, float* right, int frames,
	                     const NoteContext& note) = nullptr;
	/// **WHICH CONTROLS THIS DEVICE READS PER SAMPLE WHEN THEY ARE MODULATED** — indices into `params`
	/// (backlog FK). Only these are resolved per sample, and only
	/// while a routed source into them is moving; every other control is handed once per tick through
	/// `setFields`, as it always was. Declared beside the op that consumes them, because the choice is the
	/// device's: a gain or a crossfade is read per sample, a coefficient at the device's own step.
	std::span<const uint8_t> windowedControls {};

	/// **THE DECLARED CONTROLS THAT MOVE INSIDE THIS CALL.** Called after `setFields`, before `processStereo`,
	/// when at least one of `windowedControls` has a moving source: `runs[c]` points at `frames` resolved values
	/// of control `c` for exactly the samples the next `processStereo` renders, or is null for a control that
	/// holds its `setFields` value across the call. Called only on a host that renders on the control-tick grid
	/// (Feathers); the standalone rack resolves per host block and hands no runs (backlog FK).
	void (*setFieldWindows)(void* storage, const float* const* runs, size_t count, int frames) = nullptr;

	/// **HOW MANY SAMPLES LATE THIS DEVICE'S OUTPUT IS, OR NULL FOR NONE** — backlog JM. Null IS the
	/// declaration of zero, as everywhere in this struct, so
	/// every device written before this is unchanged.
	///
	/// A LEAF answers from `fields` — the device's STORED row, never the modulated one (assumption L1: a
	/// control that changes a latency must not be modulatable) — and `sampleRate`, and ignores `storage`,
	/// so the answer is the same on every thread and before the device has processed anything. A device
	/// that HOLDS devices answers from `storage`: what its inner engine runs (`RackEngine::latencySamples`).
	///
	/// CALLED on the thread that runs the device, or while nothing runs it (a plugin's `activate`); once a
	/// block per host, so cheap and allocation-free. **THE DEVICE MUST DELAY BY EXACTLY THIS MANY SAMPLES**:
	/// its host delays every signal that meets it by the same count, and reports the sum up. A device
	/// answering non-zero is run on the bus, never per voice (assumption L2).
	uint32_t (*latencySamples)(const void* storage, const float* fields, size_t fieldsPresent,
	                           double sampleRate) = nullptr;
	/// **THE RIGHT EAR OF THE ROW, for the controls that have one this block** (backlog KU). Called after
	/// `setFields`, and ONLY while at least one control is stereo-live: `fieldsR` is the whole row as the right
	/// ear hears it, and `live` marks the controls a stereo route reaches; every unmarked entry equals `fields`.
	///
	/// **`setFields` MEANS MONO UNTIL THIS SAYS OTHERWISE.** A device forgets its right ear at every `setFields`, so
	/// a host that stops calling this has said "mono" with no second call to forget. NULL: the device has no right
	/// ear, and a stereo route into it is read at its left (the mono-destination rule).
	void (*setFieldsR)(void* storage, const float* fieldsR, size_t fieldsPresent, const ControlMask& live) = nullptr;
	/// **WHETHER THIS DEVICE STILL HOLDS SOUND THAT WILL COME OUT OF IT WITH NO NEW INPUT** (backlog MC). A host's
	/// SLEEP is a promise that nothing more sounds without new events, and an exactly silent output block cannot
	/// make it on its own: a delay's output is exactly zero between a short note and its first repeat while its
	/// line holds the repeat. MEASURED before this op: Feathers said SLEEP 12.9 s into a Clean delay's tail and
	/// sounded again five blocks later. So the host asks every device it runs, after the block.
	///
	/// NULL IS THE DECLARATION THAT THE DEVICE HOLDS NOTHING ITS OUTPUT DOES NOT ALREADY SHOW: a filter, whose
	/// output is non-zero while its state is, or a device whose only memory is a delay it reports as latency
	/// (`latencySamples`: the host waits that long after the last sound on its own). A device answers true only
	/// for content its OWN processing will put out. A disabled device that keeps a frozen line, or a line its
	/// current mode does not read, answers false, because only an event could bring that back and an event wakes
	/// the host. It must answer false again once the content has gone, and get there without a timeout. A device
	/// that sustains by design (a freeze, a held loop) answers true for as long as it sustains, which is right. A
	/// device that holds devices answers for them too. AUDIO THREAD, after a block; allocates nothing.
	bool (*holdsTail)(const void* storage) = nullptr;
	/// **THE SIDECHAIN, FOR A DEVICE THAT LISTENS TO ONE** (the Spectral vocoder): the plugin's "Sidechain" input for
	/// the frames `processStereo` is about to run, at the same offset into the host block. `left`/`right` null is silence;
	/// `sounding` false means nothing has sounded there for a while (`device::SidechainPresence`: no buffer, a constant-0 mask, or a second of exact
	/// silence); a device reads it for display only. Called on the BUS road only, right before `processStereo`; NULL IS THE DECLARATION that the device hears none.
	/// AUDIO THREAD; the pointers are valid for that one call.
	void (*setSidechain)(void* storage, const float* left, const float* right, int frames, bool sounding) = nullptr;

	/// **THE AUDIO THREAD'S PULL OF A HAND'S BLOCKS** (`StateBlockDesc::handEdited`, `StateOps::apply`), or null for a device
	/// with none. The engine calls it right after `setFields`, for the slot's bus instance (`source` == `storage`) and for
	/// each of its voice instances (`source` the bus instance), so a hand's edit reaches every instance without anything
	/// writing into a voice from the message thread. THE CONTRACT: bounded and allocation-free; ONE attempt at the source's
	/// `HandBlock` (`tryRead`) and on a torn read the instance keeps its last good copy and tries again next block — never a
	/// spin, never a wait. A source that has been written nothing (serial 0) changes nothing.
	void (*pullHandBlocks)(void* storage, const void* source) = nullptr;
	/// **WHICH VOICE THIS INSTANCE IS** (backlog MZ): a host that builds one instance per voice tells each
	/// its voice slot, once, right after construction, on the message thread. The bus instance is never told. A STACK'S
	/// PARTS ARE SUMMED BEFORE THE PER-VOICE DEVICES, so a stack is one voice and one instance: the slot is the index.
	/// What it is for is identity that a seed can fold in — NOISE's Free streams, so voices started together on one
	/// note do not play the same noise. NULL IS THE DECLARATION that a device has no use for it. APPENDED, so every
	/// device written before it is unchanged.
	void (*setVoice)(void* storage, size_t voice) = nullptr;
	/// **THIS DEVICE'S AUDIO ROAD ONLY LISTENS — every buffer leaves exactly as it arrived** (backlog MY).
	/// `NoteOps::listensOnly`'s promise, for audio: a follower declares an audio road to be HANDED audio (the sidechain,
	/// or the signal at its slot) and writes nothing. A host may rely on it (it is a wire for latency, tails and
	/// sound), and a sweep over the devices that RENDER leaves it out, since every claim about a wire is vacuous.
	/// APPENDED, false by default, so every device written before it is unchanged.
	bool listensOnly = false;
	/// **THE BUS INSTANCE OF A PER-VOICE DEVICE, WHILE ITS VOICES RUN** (backlog OM): told the slot's COLLAPSED
	/// row and the block's frame count, once per block, before any voice runs, so it can keep what its voices SHARE (the
	/// table shaper's one table, built from the device's own row). The voices reach it through `pullHandBlocks` (`source`
	/// the bus instance). Never called while the device runs on the bus. AUDIO THREAD, allocation-free. NULL IS THE
	/// DECLARATION that the voices share nothing. APPENDED.
	void (*beginSharedBlock)(void* storage, const float* fields, size_t fieldsPresent, int frames) = nullptr;
};

/// WHAT A HOST TELLS A MODULATOR AT THE MOMENT IT ADVANCES IT.
///
/// SEPARATE FROM `NoteContext` ON PURPOSE, and the reason is the same one that keeps the note out of
/// the per-block context: `gate` is a LEVEL and `retrigger` is an EDGE. An edge is meaningful only at
/// the instant it is delivered — put it in a latched snapshot and it stays true until somebody
/// replaces it, which is a retrigger that fires for as long as nobody looked. Two facts that arrive
/// at the same moment are not one fact.
struct ModTickContext {
	/// Is a note being held, as the host's own note stack reckons it.
	bool gate = false;

	/// Did a note START this tick. TRUE FOR EXACTLY ONE TICK, never latched.
	bool retrigger = false;

	/// The transport. Same type the audio road takes; see TransportClock.
	TransportClock clock {};

	/// A scale on the modulator's own rate, 1.0 for none. IN THE CONTRACT RATHER THAN IN EACH DEVICE'S
	/// PARAMS because both existing modulators already take one, and a quantity two independent
	/// implementors both wanted is a property of the tier rather than of either device.
	float rateMul = 1.0f;

	/// **ONE CONTROL OF THIS MODULATOR, RESOLVED FOR ONE NOTE** (backlog MV, B) — or null: a host with no voices, or one
	/// that has not been told which note each voice holds. The row `setFields` delivered is resolved for the whole rack;
	/// a per-voice source (an envelope, a note expression) routed into a modulator's control has a value PER VOICE, and
	/// a Poly modulator runs a shape per NOTE. `resolve` joins the two: it answers true with `*out` set when a per-voice
	/// input reaches `control` and a voice holds that note, and false otherwise — the row's value then stands. AUDIO
	/// THREAD, inside `ModOps::tick`; allocates nothing. Valid for that one call.
	struct PerNoteResolver {
		const void* context = nullptr;
		bool (*resolve)(const void* context, size_t control, int32_t noteId, int16_t channel, int16_t key,
		                float* out) noexcept = nullptr;
	};
	const PerNoteResolver* perNote = nullptr;
};

/// HOW A HOST ADVANCES A MODULATOR AND READS WHAT IT PUBLISHES — and NULL IS THE CAPABILITY.
///
/// A device with one of these can be a modulation SOURCE; a device that leaves it null cannot, and
/// that is the whole statement. No `isModulator` flag, for `AudioOps`' reason: a function pointer
/// cannot disagree with a flag beside it.
///
/// WHAT IT REPLACES IS A TYPE TEST. `synth.cpp` holds `shapes_[]` and `envelopes_[]` as separate
/// arrays and picks between them with `rack_.holds(slot, mod::mseg::kTraits.tag)` — for the tick and
/// again for the read. That is a device's identity baked into its host, the same pin `Voice::filter`
/// was for the fx lane, and a third modulator type would need a third array and a third branch.
/// **`AudioOps` unpinned the fx lane; this is the source side, which was never unpinned.**
///
/// THE SHAPE IS TRANSCRIBED, NOT DESIGNED. `dahdsr` and `mseg` were written independently and already
/// share a public surface exactly this shape — prepare, reset, tick(settings, gate, retrigger, clock,
/// rateMul), value(). `device.h`'s condition for writing an abstraction at all is that it be designed
/// against two real implementors, and here they existed first.
/// ONE NOTE, AT THE SEAM.
///
/// **SHAPED AFTER CLAP'S EVENT RATHER THAN IMPROVED ON**, so the translation at either end is a copy
/// and not a decision. A field this struct did not carry would be one a device could never see, and
/// the host would be the thing that lost it.
///
/// `noteId` DEFAULTS TO -1 AND THAT IS A DOCUMENTED STATE, not a missing value. CLAP: *"host provided
/// note id >= 0, or -1 if unspecified or wildcard"*, with matching falling back to the
/// (port, channel, key) tuple. A device that INVENTS a note declines to claim an id rather than
/// forging one — see the note lane design, §3.1. What it costs is that per-note expression cannot
/// address an invented note, and what it buys is that no device has to own an id range no host will
/// reuse.
struct NoteEvent {
	/// **EXPRESSION IS A NOTE EVENT, WHICH IS CLAP'S OWN MODEL AND WAS NOT OURS.**
	///
	/// `clap/events.h` carries `CLAP_EVENT_NOTE_EXPRESSION` in the same stream as note on and off,
	/// addressed the same way. This lane did not: expressions went from `feathers_clap.cpp` straight
	/// to the voice manager, **bypassing the lane entirely**, so no device could see one — let alone
	/// change it. Notes are meant to be piped, and potentially mutated, as they transit the
	/// graph.
	///
	/// So an expression now transits with the notes it belongs to, and a device that wants to scale
	/// pressure or remap brightness does it the way an arp changes a key.
	enum class Kind : uint8_t { On, Off, Choke, Expression };

	Kind kind = Kind::On;
	/// Offset from the start of the block, in samples. Kept because a note's PLACE in a block is part
	/// of it: an arp that emitted everything at offset 0 would quantise itself to the block rate.
	uint32_t timeSamples = 0;
	int32_t noteId = -1;
	int16_t port = 0;
	int16_t channel = 0;
	int16_t key = 60;
	double velocity = 1.0;

	/// **WHICH EXPRESSION, BY CLAP'S OWN ID.** Meaningful only for `Kind::Expression`; -1 otherwise,
	/// which is not a legal CLAP id and so cannot be mistaken for one. Read the KIND before this
	/// field, exactly as `velocity` is only about an On or an Off.
	///
	/// CLAP'S NUMBERING RATHER THAN AN ENUM OF OURS, so a host's id passes through untouched and no
	/// device has to map between two vocabularies to answer the question "is this pressure".
	int32_t expressionId = -1;

	/// **WHAT THE EXPRESSION IS SET TO** — absolute, not a delta, because every CLAP note expression
	/// is a reading of a continuous controller rather than an increment. A device that scales this is
	/// scaling the reading; one that accumulates would ratchet.
	double expressionValue = 0.0;

	/// **A CHOKE THE SYNTH SENDS ABOUT ITS OWN STOLEN VOICE** (`Synth::chokeLaneNote`), not one the host sent. The two mean
	/// different things to a device that holds keys: a host's choke ends the note (CLAP's meaning — the key is up), while
	/// a steal's ends only the step a voice was playing, the key still down. Told apart by SOURCE, never by whether the
	/// choke names a key (a steal can end a step while the transport is off and no notes are held). False for every other event.
	bool fromSteal = false;
};

/// HOW A HOST OFFERS NOTES TO A DEVICE — the note lane, which `registry.h` spent months declining to
/// build.
///
/// **NOTES CHAIN, THEY DO NOT BROADCAST.** A device receives what reached it and writes what leaves
/// it, exactly as `AudioOps::processStereo` does with audio. An arp early in the chain therefore feeds
/// what follows. Broadcasting the host's raw input to everyone would make chain order meaningless for
/// notes while meaning everything for audio, which is an inconsistency a player finds by being
/// surprised.
///
/// **THE HOST DOES NOT INTERPRET, AND THAT WAS A CORRECTION.** The first design had the host collapse
/// a held chord to one pitch and hand it round. It should give all devices all notes and let
/// the devices decide what to do with them, and every other road in this seam already worked that
/// way. A tracker chooses last, lowest or highest ITSELF and can expose the choice as one of its own
/// controls, which is where a musical decision belongs.
///
/// **NULL IS THE CAPABILITY**, as everywhere else here: a device with no `NoteOps` is never consulted
/// and the notes reaching it pass through untouched. That is what makes a note lane cost nothing for
/// the devices that do not want one.
struct NoteOps {
	size_t stateBytes;
	size_t stateAlign;

	void (*construct)(void* storage);
	void (*destruct)(void* storage);
	void (*prepare)(void* storage, double sampleRate);

	void (*setFields)(void* storage, const float* fields, size_t fieldsPresent);

	/// TRANSFORM ONE BLOCK'S NOTES.
	///
	/// A device that only READS them — a pitch tracker — copies `in` to `out` and keeps what it
	/// learned; that is not a formality, it is what makes it a pass-through rather than a note sink.
	///
	/// **THE CAPACITY IS FIXED AND THE COUNT IS REPORTED BACK, because the alternative is allocating
	/// on the audio thread**, which this repo forbids after activate. A device that fills the buffer
	/// must drop LATER notes rather than earlier ones: a dropped note-OFF is a stuck note, and offs
	/// for held notes precede new ons in any sane stream.
	void (*processNotes)(void* storage, const NoteEvent* in, size_t inCount, NoteEvent* out,
	                     size_t outCapacity, size_t* outCount);

	/// **THIS DEVICE ONLY LISTENS — every event leaves exactly as it arrived.** A promise about `out`, and what it
	/// buys is ORDER: the graph orders consecutive note members of a chain because each hands the next what it
	/// wrote, and a listener writes nothing, so two listeners need no order between them (`graph.h`'s note walk).
	/// Without it, giving the MSEG and the LFO a per-note table ordered them by lane, and an LFO
	/// could no longer reach one to its left — backlog IZ's "a modulator's position in a lane is only grouping",
	/// undone by a note road that changes nothing. A listener still follows the last note member that DOES write.
	bool listensOnly = false;

	/// **THE PITCH THIS DEVICE HANDS THE BUS DEVICES AFTER IT, or false to leave theirs alone** (so filter keytrack works in mono
	/// mode, from the collapsed note per bus defaults or an upstream tracker card). A host's bus devices are handed one `NoteContext`, the collapsed note; a device that answers here
	/// replaces its PITCH for every bus device after it in the chain, and the nearest such device upstream wins
	/// (`RackEngine::busNoteForTest`). Null — the capability is the pointer, as everywhere here — for every device
	/// but the Tracker. Not asked of a switched-off device. Read on the audio thread after the notes and the
	/// modulators have run for the piece.
	bool (*busNote)(const void* storage, float* pitchSemitones) = nullptr;

	/// **NOTES THIS DEVICE MAKES ON ITS OWN CLOCK — an arp. NULL IS THE CAPABILITY**.
	/// `processNotes` runs only when a note ARRIVES, so a device that emits between arrivals needs a second call: a
	/// host makes this one at the start of every render chunk, with the clock AT THAT SAMPLE and a sample count that
	/// only grows (a free clock's time base). Write the events due AT THIS SAMPLE into `out` (at most `outCapacity`,
	/// dropping LATER ones first, offs before ons) and return how many; set `*samplesUntilNext` to how many samples
	/// from now the next call has something to emit (>= 1, `UINT32_MAX` for never), and the host cuts its render there,
	/// so every onset lands on its own sample. What is written here travels on through the devices after this one.
	///
	/// **EMIT HERE, NEVER FROM `processNotes`**: a host hands the lane one event at a time, so a chord arrives as three
	/// calls and an arp emitting from the first would play the chord's first note alone. A host calls this after every
	/// note at the same sample has gone through `processNotes`.
	size_t (*emitNotes)(void* storage, const TransportClock& clock, uint64_t sampleNow, NoteEvent* out,
	                    size_t outCapacity, uint32_t* samplesUntilNext) = nullptr;
	/// **THE NOTES ALREADY SOUNDING THROUGH THE LANE** when this device first stood in it (backlog QU): host ons that
	/// passed to the voices while it was published but not yet installed. A device that swallows its input (an arp)
	/// records them as passed, so their offs go on to the voices (with these ids) instead of being swallowed. Called by
	/// the host once per built instance, before its first `processNotes`. AUDIO THREAD. Optional.
	void (*seedPassed)(void* storage, const NoteEvent* ons, size_t count) = nullptr;
};

struct ModOps {
	size_t stateBytes;
	size_t stateAlign;

	void (*construct)(void* storage);
	void (*destruct)(void* storage);
	void (*prepare)(void* storage, double sampleRate);
	/// As if just switched on — the meaning `AudioOps::reset` had to have spelled out once something
	/// finally called it.
	void (*reset)(void* storage);

	/// Settings arrive as the device's own row, raw, with the count of fields really present.
	/// IDENTICAL TO `AudioOps::setFields` deliberately: a device's settings travel one way whatever
	/// kind of device it is, which is `device_row.h`'s rule and not a coincidence worth diverging on.
	void (*setFields)(void* storage, const float* fields, size_t fieldsPresent);

	/// HOW MANY SAMPLES ONE `tick` ADVANCES — declared by the device, not known by the host.
	///
	/// A FIRST DRAFT OF THIS PUT THE NUMBER IN THE DEVICE VOCABULARY as a shared constant, and
	/// `mod/modulator.h` had already argued against exactly that: it says `kBlockSamples` is
	/// *"deliberately NOT `device/device.h`"* because that file is *"the vocabulary a device declares
	/// itself in, and an fx unit uses it too; a trigger mode and an envelope tick are the modulator
	/// tier's, and an fx unit has no business with either."*
	///
	/// That argument is right and it settles this the other way: the vocabulary must not learn a
	/// modulator-tier constant, so the DEVICE declares its own tick length and the host reads it. Both
	/// existing modulators declare `mod::kBlockSamples`; a guest may declare anything.
	size_t blockSamples;

	/// ADVANCE ONE BLOCK and publish. The host calls this once per `blockSamples` samples of audio.
	void (*tick)(void* storage, const ModTickContext& context);

	/// WHAT OUTPUT `n` IS PUTTING OUT, after the most recent tick. `n` indexes `Descriptor::outputs`,
	/// which the device already declares.
	///
	/// IT IS THE SMOOTHED READING. Measured: `synth.cpp:417` and `:1245` both route from `value()`,
	/// and `rawValue()` has no routing caller anywhere. `rawValue` is a second READING of one output
	/// rather than a second output — a device that genuinely wants both published declares two
	/// `SourceDesc`s and answers both here.
	float (*outputAt)(const void* storage, size_t output);

	/// **WHAT THIS OUTPUT IS PUBLISHING FOR ONE NOTE — the per-voice road, and NULL is the capability.**
	///
	/// A device whose `SourceDesc::perVoice` is true answers here; one whose sources are the same for
	/// every voice leaves it null and the host reads `outputAt` for all of them. Backlog DU's decision
	/// point 1: a rack LFO stays ONE instance producing one number.
	///
	/// **BY NOTE ID RATHER THAN BY VOICE, and that is what keeps this layer portable.** A voice is the
	/// HOST's idea — `src/mod/` knows nothing about voice pools or stealing, and a device that took a
	/// voice index would be a device that had to be told the host's polyphony. A note id is what the
	/// device already saw on the note lane, so it can answer from what it kept.
	///
	/// A note the device is not holding gets the COLLAPSED answer — the same number `outputAt` gives —
	/// so a voice releasing does not read zero mid-tail.
	///
	/// **BY THE WHOLE NOTE — id, channel AND key — matched the way CLAP addresses, -1 a wildcard**
	/// (a wildcard query once left envelopes high and the synth droning). A MIDI keyboard gives every
	/// note id -1, so a query by id alone matched the FIRST note the device held, for every voice at
	/// once; the channel and key are what tell them apart, and the device already keeps all three.
	float (*outputForNote)(const void* storage, size_t output, int32_t noteId, int16_t channel,
	                       int16_t key) = nullptr;
	/// **`outputForNote` AS AN EAR PAIR** (backlog PA) — asked only for an output the host has found to be a pair this
	/// block, as `outputPairAt` is; `left` equals `outputForNote`. Null: the per-voice right ear is the collapsed pair's
	/// right (`outputPairAt`), or the left where there is no pair at all.
	EarPair (*outputPairForNote)(const void* storage, size_t output, int32_t noteId, int16_t channel,
	                             int16_t key) = nullptr;

	/// WHERE THIS MODULATOR IS IN ITS SHAPE — the segment or stage, and how far through it.
	///
	/// **MAY BE NULL, and a host must treat null as "cannot say" rather than as zero.** Ordinal 0 is
	/// `Stage::Idle` for a DAHDSR but a VALID SEGMENT for an MSEG, so a host that defaulted to zero
	/// would draw an MSEG's marker frozen at the start of segment 0 — a still picture that looks
	/// exactly like a stopped modulator.
	///
	/// PRIMITIVES RATHER THAN `mod::Playhead`, and that is this file's own rule rather than a
	/// preference: `modulator.h` argues that `kBlockSamples` is *"deliberately NOT device/device.h"*
	/// because this is *"the vocabulary a device declares itself in, and an fx unit uses it too"*. A
	/// stage ordinal belongs to the modulator tier for the same reason, so it crosses as two numbers.
	///
	/// **IT EXISTS BECAUSE THE RACK COULD NOT DRAW THE STAR.** Feathers' synth reads
	/// `envelopes_[slot].playhead()` directly, because it knows it holds envelopes; the rack holds
	/// DEVICES and can only ask through this table. Without it the modulators showed no
	/// phase star.
	void (*playhead)(const void* storage, uint32_t* ordinal, float* phase);
	/// **THE LEVEL BESIDE THE PLAYHEAD — MAY BE NULL, which is "the picture already says it".** For a device
	/// whose drawn line cannot tell where it stands from (ordinal, phase) alone: the DAHDSR, whose release starts
	/// wherever the gate fell (`mod::Playhead::level`). Raw 0..1, the level the card's line is drawn in.
	float (*playheadLevel)(const void* storage) = nullptr;

	/// **WHETHER THIS DEVICE'S MOD ROAD READS ITS OWN AUDIO** — static; see `readsAudioOf` for a row that decides — and the
	/// declaration the graph's two instants turn on (backlog IZ). For modulation,
	/// "downstream" means COMPUTED FROM DOWNSTREAM AUDIO, and a modulator's position in a lane is only
	/// grouping. So a device has a MOD instant (its tick and publish) and an AUDIO instant (its audio and
	/// note roads), and MOD comes first — which is what every host already executes: `RackEngine::
	/// beginPiece` runs `advanceModulators` before the audio walk, and `tick` above is handed a
	/// `ModTickContext` and no signal.
	///
	/// A FOLLOWER — a device whose outputs are computed FROM the audio passing through it — would set
	/// this, and the graph then puts its MOD instant AFTER its AUDIO instant, so its outputs are
	/// downstream of everything upstream of its audio. No device can honestly set it until a host ticks
	/// such a device after its audio; the graph's half is here so that day is a declaration and not a
	/// special case.
	bool readsAudio = false;
	/// **AND WHEN THE ROW DECIDES IT — NULL IS "`readsAudio` SAYS"** (backlog OP). The Follower reads its control
	/// Input in Control mode and the audio at its slot in Audio mode, so what it may reach follows the row. The graph
	/// keeps the answer per node (`Graph::setReadsAudio`), the record re-asks on every write of the row and deletes
	/// the routes a flip forbids (`ChainRecord::takePrunedRoutes`). Ask `device::modReadsAudio`, never this directly.
	bool (*readsAudioOf)(const float* fields, size_t count) = nullptr;
	/// **ONE OUTPUT AS AN EAR PAIR — NULL IS THE CAPABILITY** (backlog KU). Asked only for an output the host has
	/// found to be a pair this block (`SourceDesc::pairAlways`, or a `pairFrom` control stereo-live); every other
	/// output is `outputAt`, copied into both ears. `left` equals `outputAt` for the same output.
	EarPair (*outputPairAt)(const void* storage, size_t output) = nullptr;
	/// `AudioOps::setFieldsR`'s contract, for a modulator's own controls — Math's inputs, MACROS' values, the Stereo
	/// Tool's In. Called after `setFields`, only while one of them is stereo-live.
	void (*setFieldsR)(void* storage, const float* fieldsR, size_t fieldsPresent, const ControlMask& live) = nullptr;

	/// **HOW MANY TIMES THIS MODULATOR'S TRIG HAS FIRED** (backlog MV) — a count that only grows, so a surface polling at
	/// its own rate sees a one-tick fire it would miss as a level. Null for a device with no Trig.
	uint32_t (*trigFires)(const void* storage) = nullptr;

	/// **THE NEWEST EVENTS IT MADE, NEWEST FIRST** (`RecentEvent`) — what a card that looks once a frame draws a fast
	/// device's in-between events from (a S&H's samples, a Random Trigger's hits). Writes at most `capacity` and returns
	/// how many; under Poly, the newest note's. Null for a device whose events a card does not draw one by one.
	size_t (*recentEvents)(const void* storage, RecentEvent* out, size_t capacity) = nullptr;

	/// **THE EXTRA STATE AS THE DEVICE HAS CHANGED IT, for the record** (backlog MX; `device/extra_state.h`). Optional:
	/// null for every device whose extra state only a hand writes. AUDIO THREAD, after `tick`: write the device's WHOLE
	/// extra state (`Traits::extraState` floats, at most `capacity`) into `extra` and return the serial it was taken
	/// under (`nextExtraStateSerial`), or 0 when it has never changed it. A host copies it into a snapshot, and the
	/// message thread into the record whenever the serial moves and once more before a save.
	uint64_t (*publishExtraState)(const void* storage, float* extra, size_t capacity) = nullptr;
	/// **WHICH FLOAT OF THE EXTRA STATE ONLY THE RECORD WRITES** — an index into the extra state, required with
	/// `publishExtraState`. A hand's edit and a load each give it a fresh value (`freshExtraStateBasis`); the device
	/// echoes the value it last read, and the message thread applies a snapshot only while the record's still equals the
	/// snapshot's, so a snapshot taken before a hand's edit or a load never lands over it.
	int extraStateBasis = -1;
};

/// DECLARED AHEAD OF `Catalogue`, which returns one and is itself reached through
/// `Descriptor::container`. The cycle is real and a forward declaration is the whole of the fix.
struct Descriptor;

//------------------------------------------------------------------------------------//
// A DEVICE THAT HOLDS OTHER DEVICES.                                                    //
//------------------------------------------------------------------------------------//

// **ONE PIECE OF CODE, AND THIS IS ALL IT NEEDS FROM THE VOCABULARY.** The lane, the standalone effect and
// the chain inside the chain are all the same code. A rack is a chain
// that holds devices; as a device it sits in a chain; so it sits in a rack. Third party effects
// that include these headers become mod assignable when they are downstream
// in a graph/chain/rack (same thing), without compiling into the host — and so a third
// party could implement their own chain device, which could host our chain and nest inside feathers.
//
//------------------------------------------------------------------------------------------------//
// WHAT A DEVICE THAT HOLDS DEVICES NEEDS FROM WHOEVER HOSTS IT: THE CATALOGUE, AND NOTHING ELSE      //
//------------------------------------------------------------------------------------------------//
//
// It builds its members itself, from their descriptors, through the same `AudioOps` every host uses.
// It routes modulation among them by its own edges. It saves them as its own bytes. What it CANNOT
// have compiled in is the list of devices that exist — it may be built separately from whatever hosts
// it, a third party's binary or ours — so its host hands it the list.
//
// **THE RULE, AND IT IS THE SAME FOR EVERY HOST:** whoever builds a device that declares `.container`
// adopts it with a catalogue before adding anything to it. Feathers' lane does it for a rack in the
// lane; a rack does it for a rack in a slot; a third party's chain does it for our rack inside theirs.
// Nothing in that is about nesting — nesting is what the rule produces when the device built happens
// to hold devices too.
//
// FUNCTION POINTERS OVER A `void*`, not a type: `src/device/` includes nothing from `plugin/` or
// `backplane/`, which is what lets anyone compile a device against these headers alone.
//
// AN EARLIER VERSION OF THIS SECTION HANDED A CONTAINER A POOL, AN ID MINT AND A MEMBER BUILDER, and a
// second road for outer modulation by ordinal and offset. All of it was built for a separate imitation
// chain rather than for the rack, and none of it is what a device that holds devices needs: the rack
// builds its own members, and modulation is edges. Deleted with the imitation.

/// **THE DEVICES THAT EXIST, AS WHOEVER HOSTS YOU KNOWS THEM.** `find` turns a tag into a descriptor,
/// or null for a tag this host has never heard of — which is a real answer: a saved chain naming a
/// device this build lacks keeps its place and plays nothing.
struct Catalogue {
	void* host = nullptr;
	const Descriptor* (*find)(void* host, const char* tag) = nullptr;
	/// **HOW MANY CHAINS DOWN the chain holding this catalogue sits** — 0 for a host's own chain (the Chain plugin's,
	/// Feathers' lanes), one more for every container between (the depth gate). It rides the catalogue
	/// because the catalogue is the one thing every host already hands every container it builds (`adopt`), so a
	/// chain knows its depth from the moment it can hold anything, with or without an editor watching it.
	uint32_t depth = 0;

	[[nodiscard]] const Descriptor* operator()(const char* tag) const {
		return find != nullptr ? find(host, tag) : nullptr;
	}
	/// The same catalogue as a container built in this chain hands its own chains: one deeper.
	[[nodiscard]] Catalogue inner() const noexcept { return Catalogue { host, find, depth + 1 }; }
};

/// **WHERE A DEVICE WRITES ITS BODY.** `write` appends; the host decides where the bytes go — a buffer,
/// a file, a parent's own body. A function pointer over a `void*`, for the vocabulary's usual reason.
struct ByteSink {
	void* ctx = nullptr;
	void (*write)(void* ctx, const uint8_t* data, size_t bytes) = nullptr;

	void operator()(const uint8_t* data, size_t bytes) const {
		if (write != nullptr && bytes != 0) {
			write(ctx, data, bytes);
		}
	}
};

/// **A BODY FOR A DEVICE THAT HOLDS NOTHING** — bytes beyond its row of floats, for a leaf (backlog JD:
/// Featherverb's impulse response, saved as a filename). The same three entries as `ContainerOps`' and
/// the same contracts: message thread, one sink written once, `loadState` all-or-nothing, a bound on the
/// largest body. A device declares this OR `ContainerOps`, never both — a container's body is its
/// contents, and a leaf that grew one would be a container. Every road a body travels asks
/// `Descriptor::bodySaver()` / `bodyLoader()`, which answer for either, so a leaf's body is saved, parsed,
/// stored and handed to its instance exactly as a chain's is; the roads that mean "HOLDS DEVICES"
/// (nesting, adopt, outer gates, inner routes) keep asking `container`.
struct BodyOps {
	bool (*saveState)(const void* storage, const ByteSink& sink) = nullptr;
	bool (*loadState)(void* storage, const uint8_t* data, size_t bytes) = nullptr;
	size_t maxBodyBytes = 0;
};

/// HOW TO RUN A DEVICE THAT HOLDS OTHER DEVICES, or null for one that does not.
///
/// NO `stateBytes` OF ITS OWN: a device declaring several capabilities shares one storage, and a rack
/// declares `.audio` (it renders) and `.container` (it holds) — the bytes are the audio road's.
struct ContainerOps {
	/// **THE HOST HANDS OVER ITS CATALOGUE**, once, before anything is added — before `loadState`, and
	/// before any road that builds a member. After `construct` and `prepare` is fine: the road every
	/// device is built by does those two together, and neither needs a catalogue.
	void (*adopt)(void* storage, const Catalogue& catalogue) = nullptr;

	/// **WHAT IT HOLDS, WRITTEN ONCE INTO A SINK THE HOST OWNS.** False if it could not write.
	///
	/// **IT WAS "RETURN THE SIZE, AND WRITE ONLY IF THERE IS ROOM", AND THAT IS EXPONENTIAL UNDER
	/// NESTING.** A host asked for the size and then for the bytes, so every body was serialised twice —
	/// and a rack inside a rack serialised ITS child twice for each of those. 2^depth: sixty-four times
	/// at six levels, which passed, and 262,144 times at eighteen, which hung. A sink is still one entry
	/// point that cannot disagree with itself, and each level serialises its child exactly once.
	///
	/// MESSAGE THREAD: a body is saved and loaded, never on the audio path. Writing nothing is a real
	/// answer — a device that holds nothing.
	bool (*saveState)(const void* storage, const ByteSink& sink) = nullptr;

	/// **TAKE A BODY BACK**, after `adopt`. False for bytes this device cannot read, and a device that
	/// returns false has left itself as it was: the host cannot tell a truncated body from a short one,
	/// so the device validates its own bytes, all or nothing.
	bool (*loadState)(void* storage, const uint8_t* data, size_t bytes) = nullptr;

	/// **THE LARGEST BODY IT CAN LEGITIMATELY WRITE**, so a host can bound a length it reads out of a
	/// file. A patch is a file on somebody's disk and the length in it decides how far a reader walks;
	/// only the device knows its own worst case.
	size_t maxBodyBytes = 0;
	/// THE GATE OUTSIDE — see `OuterGate`. Null for a container that admits no outer source.
	void (*setOuterGate)(void* storage, const OuterGate& gate) = nullptr;
	/// **THE POOL OUTSIDE** — see `HostSlotLink` (host slots T11). Handed beside the gate, at provision, before
	/// the body loads, and again whenever the host re-files the instance. Null for a container whose controls
	/// take no host slots: they answer "no slot".
	void (*setHostSlots)(void* storage, const HostSlotLink& link) = nullptr;
	/// **THE CONTAINER'S OWN MESSAGE-THREAD SERVICE, ON THE HOST'S BEAT** — rack roadmap §7, hygiene across
	/// the boundary: the host calls this for every live container when it services its own record, and
	/// the container prunes every inner route whose OUTER source the gate no longer admits (the outer
	/// device removed, or moved downstream — the rule: if you reorder and routes get broken, they
	/// get deleted and are gone forever), reporting what it dropped so the host can SAY it. Null for a
	/// container with nothing to service.
	RouteDrop (*service)(void* storage) = nullptr;

	//--------------------------------------------------------------------------------------------//
	// THE ROUTES INSIDE, AS THE HOST OUTSIDE SEES THEM — rack roadmap §7, the other half of hygiene:  //
	// an outer source's own view of what it modulates ("routes" in the Engine rail) must show the      //
	// routes that live in a chain's inner table, and let the hand edit or remove them from outside. //
	// `outer` is spelled the outer host's way (the container puts the scope bit on).                //
	//--------------------------------------------------------------------------------------------//

	//--------------------------------------------------------------------------------------------//
	// ONE BODY, N INSTANCES — rack roadmap §8: a container as a Poly device.                        //
	//--------------------------------------------------------------------------------------------//

	/// **HOW MANY VOICES THE HOST RUNS** — told at provision, on the message thread, before anything is
	/// provisioned inside: the container's inner engine takes the same voice count, so an inner device
	/// that declares Poly runs one instance per OUTER voice and a per-voice outer source's run
	/// (`OuterSources::perVoice`) is indexed by the same voice. It is a pass-through model
	/// — the chain is not Poly; its devices are, each for itself, exactly as in the host's own lane.
	void (*setVoices)(void* storage, size_t voices) = nullptr;

	/// Every inner route reading `outer`: `fn(ctx, innerDestination, amount)`.
	void (*forEachRouteFrom)(const void* storage, Source outer, void* ctx,
	                         void (*fn)(void* ctx, Destination inner, float amount)) = nullptr;
	/// The amount of one such route, 0 for none.
	float (*routeAmountFrom)(const void* storage, Source outer, Destination inner) = nullptr;
	/// Make, change or remove one such route — through the container's own gate. Returns whether taken.
	bool (*setRouteFrom)(void* storage, Source outer, Destination inner, float amount) = nullptr;
	/// The curve of one such route, Lin for none.
	Transfer (*routeTransferFrom)(const void* storage, Source outer, Destination inner) = nullptr;
	/// Set the curve of one EXISTING such route. Returns whether taken.
	bool (*setRouteTransferFrom)(void* storage, Source outer, Destination inner, Transfer t) = nullptr;
	/// The inner destination's name as the container spells it ("Filter · Cutoff"), NUL-terminated into
	/// `out`; returns the length written. A host prefixes the chain.
	size_t (*nameDestination)(const void* storage, Destination inner, char* out, size_t capacity) = nullptr;
	/// **WHAT THE CONTAINER CALLS ITS MACRO `macro`**, NUL-terminated into `out`; returns the length written, 0 for a
	/// macro with no name of its own (the default, "Macro N"). The LIVE name — the one the container's own window renames
	/// (backlog OX) — so the device's card, which only has the host's stored body, reads the same name the window shows.
	size_t (*macroLabel)(const void* storage, size_t macro, char* out, size_t capacity) = nullptr;

	//--------------------------------------------------------------------------------------------//
	// A BODY INTO A RUNNING INSTANCE — a device preset's road (backlog FO)                          //
	//--------------------------------------------------------------------------------------------//

	/// **TAKE A BODY WHILE RUNNING, THROUGH THE CONTAINER'S OWN RECORD, AND NEVER TOUCH ITS ENGINE.**
	/// `loadState` is the PROVISIONING road — it may drain the inner engine on the calling thread, which
	/// is sound only because nothing is running it yet. A preset lands on an instance a block may be
	/// running, so this is the other half of the same desired-state rule the rack's own `stateLoad`
	/// keeps: the record becomes the body here, on the message thread, and the engine converges at its
	/// own drain point.
	///
	/// ALL OR NOTHING, as `loadState`. **AN EMPTY BODY (`bytes == 0`) IS A CONTAINER THAT HOLDS
	/// NOTHING** — `rack::bodyOf`'s own reading of an empty body — so it empties every chain rather than
	/// being refused; that is what "Init" means for a device that holds devices.
	///
	/// MESSAGE THREAD, on the handle a host files (`DeviceHost::containerSurfaceOf`). Null for a container
	/// that cannot take a body while it runs; a caller then says so rather than falling back to
	/// `loadState`, whose drain would race the audio thread.
	bool (*applyState)(void* storage, const uint8_t* data, size_t bytes) = nullptr;

	//--------------------------------------------------------------------------------------------//
	// ITS INNER SOURCES, EXPORTED (backlog NZ part (b))                                           //
	//--------------------------------------------------------------------------------------------//
	//
	// A modulator inside a Chain or a Rack reaches the devices AFTER the container in its host (it flows out
	// the back of the compound chain as would be normal): spelled there `Source::exported(container,
	// row)`, a row of the container's own export table, saved in its body. All three or none.
	/// MESSAGE THREAD. The row exporting `inner` (spelled as the container's inner records spell it: a
	/// modulator output, or a macro), minted if it has none; `SIZE_MAX` for anything else or a full table.
	size_t (*exportRow)(void* storage, Source inner) = nullptr;
	/// The inner source a row names, or `Source::nobody()`.
	Source (*exportedSource)(const void* storage, size_t row) = nullptr;
	/// AUDIO THREAD. The cell the host reads for a row, written at the end of each of the container's blocks;
	/// valid while the storage lives. Null past the table.
	const float* (*exportCell)(const void* storage, size_t row) = nullptr;
	/// MESSAGE THREAD. Free the rows `usedRows` (a `const std::vector<bool>*` indexed by row) says no route reads, once
	/// the audio thread has drained the publish that dropped them (`ExportTable::sweep`). Returns how many it freed.
	size_t (*sweepExports)(void* storage, const void* usedRows, uint32_t published, uint32_t drained) = nullptr;
	/// **CONTROLS THE CONTAINER GAVE UP, AWAITING THE HOST'S PRUNE** (backlog NO: a removed chain's slot params). MESSAGE
	/// THREAD, on the host's service beat: the host removes every route into each control index written to `out` (up to
	/// `capacity`; the count returned), then calls `controlsReleased` — and only then may the container reuse them, so a
	/// new occupant never inherits the old one's routes.
	size_t (*releasedControls)(const void* storage, size_t* out, size_t capacity) = nullptr;
	void (*controlsReleased)(void* storage) = nullptr;
	/// **THE MIRROR A LOAD OR A NEW GATE LEFT OWING, WRITTEN** (backlog NO) — MESSAGE THREAD, on the host's service beat,
	/// beside the prune: the container writes its own controls through `OuterGate::setOwnField`, which it may not do inside
	/// the host's provisioning (a host edit there provisions again). Returns whether it wrote.
	bool (*serviceMirror)(void* storage) = nullptr;
	/// **THE OTHER WAY: A BASE THE HOST MOVED, ADOPTED INTO THE BODY** (backlog NO item 2 follow-up: a chain control on a
	/// host slot). MESSAGE THREAD, on the same beat, before `serviceMirror`: `fields` is the holder's record row (the
	/// bases, `present` of them). A base that differs from the body while no mirror is owed was written by the host (a
	/// DAW automating the slot: the slot owner's visit puts its value in the record), since every hand edit mirrors at
	/// once — so the body takes it, and the knob, the save and the next mirror follow. Returns whether anything moved.
	bool (*adoptOwnFields)(void* storage, const float* fields, size_t present) = nullptr;
};

//------------------------------------------------------------------------------------//
// A DEVICE WHOSE OUTPUTS MAY REACH ITS OWN CONTROLS (backlog HS).                       //
//------------------------------------------------------------------------------------//

/// **WHICH OF ITS OWN CONTROLS A DEVICE'S OUTPUTS ARE COMPUTED FROM — and, by declaring it, the promise
/// that every OTHER control is read only AFTER the outputs are published.**
///
/// SKLITTER's main controls besides perform-impacting can sit downstream
/// of perform, so perform can modulate them. The routing rule refuses a device's output into its
/// own control because a node is not upstream of itself — and for a device that says nothing here it
/// still does, exactly. What the rule could not know is that a device with TWO roads is two instants:
/// `RackEngine::beginPiece` runs `advanceModulators` first (resolve, `ModOps::setFields`, tick, PUBLISH)
/// and only then resolves the row again for `AudioOps::setFields`. So a control the MOD road never reads
/// is genuinely downstream of the outputs, in the same tick, with no ordering change; and a control the
/// mod road DOES read would be a loop. Nothing in the descriptor said which is which. This does.
///
/// **THE DECLARATION IS A CLAIM THE HOST CANNOT CHECK, so it is the device's to keep true:** every
/// control `ModOps::setFields`/`tick`/`outputAt` reads is in `feedsOutputs`. The two controls the ENGINE
/// reads on the publish — `Traits::enableControl` and `Traits::amountControl` — are added by
/// `Descriptor::mayRouteToSelf` itself, so a device cannot forget them.
///
/// ONE SET FOR THE DEVICE, NOT ONE PER OUTPUT: every output is published by one tick from one row. A
/// device whose outputs have different inputs loses nothing but precision — the union is always safe.
struct SelfRouting {
	/// Control indices (into `Descriptor::params`) the device's outputs are a function of. A self-route
	/// INTO one of these is refused; into any other modulatable control it is a route like any other.
	std::span<const uint8_t> feedsOutputs;
};

/// **WHAT A TEXT HOOK MAY READ: THE DEVICE'S OWN FIELDS AND ITS OWN PUBLISHED READINGS, AND NOTHING
/// ELSE.** Function pointers over a `void*`, for the reason `Catalogue` gives — this header includes
/// nothing from `plugin/` or `backplane/`. `field` answers with the declared default for a field the
/// host does not hold, which is the host's rule and not the hook's to re-derive; `readingCount` is 0
/// until the device has published.
struct TextView {
	const void* host = nullptr;
	float (*field)(const void* host, size_t control) = nullptr;
	size_t (*readingCount)(const void* host) = nullptr;
	float (*reading)(const void* host, size_t index) = nullptr;
	/// **THE INSTANCE'S MESSAGE-THREAD SURFACE** (`DeviceHost::containerSurfaceOf`) — what a container's words may
	/// depend on that no field says: the Rack's chain count (rack stage 2, §3). Null where none is filed, or where
	/// the asker has none; a hook then declines. Read on the main thread only, as every asker of this view is.
	const void* surface = nullptr;
};

/// **A DEVICE THAT PRINTS A CONTROL BY WHAT IT DOES RATHER THAN BY ITS DECLARED UNIT.** Backlog GS:
/// SKLITTER's Rate is a 0..1 position whose meaning — a note length, or how many slices a bar is cut
/// into — depends on the Mode, and during a quantized gesture the position PLAYING is not the knob's.
/// No `device::Unit` can say that, because a unit is a function of one number and this is a function
/// of the device's state.
///
/// **NULL BY DEFAULT, AND A DEVICE THAT DECLARES NOTHING PRINTS EXACTLY WHAT IT DID.** `format` writes
/// a NUL-terminated string into `out` and returns true, or returns false to hand the control back to
/// the declared unit — so a device answers only for the controls it has something to say about.
/// PURE: it reads `value` and `view` and nothing else, and allocates nothing.
struct TextOps {
	bool (*format)(size_t control, double value, const TextView& view, char* out, size_t size) = nullptr;
	/// **`format`'S INVERSE** (rack stage 2, §3: typing a chain's number into the DAW selects that chain): `text` as
	/// the device's own words, written to `*value` in the control's units, and true — or false, and the declared
	/// unit reads it. **A DEVICE THAT DECLARES THIS IS ALSO PRINTED IN THE DAW** (`hostslots::SlotHost`): its words
	/// reach the host only when the host can type them back. Pure, as `format`.
	bool (*parse)(size_t control, const char* text, const TextView& view, double* value) = nullptr;
};

/// **WHAT KIND OF DEVICE THIS IS, TO A PLAYER LOOKING FOR ONE** — the first level of every add menu.
/// With a lot of effects, a type tag on all of them creates a first order hierarchy on the add effect
/// context menu.
///
/// **AN ENUM AND NOT A STRING**, so a typo cannot found a category of one. The declaration order IS the
/// menu order. `None` is what a device that forgot to say gets; the registry refuses it at compile time
/// (`device_registry.h`), and a menu files a guest's `None` last, under "Other", rather than dropping it.
///
/// **DISPLAY METADATA ONLY.** Not stored, not in a patch, not in the schema ledger, and
/// nothing about how a device runs or where it may sit reads it — what a menu OFFERS is still asked of
/// the device's capabilities; this only decides which heading the offer is filed under.
enum class Category : uint8_t {
	None,
	FilterEq,
	Dynamics,
	DriveColour,
	TimeSpace,
	Pitch,
	RhythmGlitch,
	ModulationFx,
	Utility,
	Modulators,
	/// **A DEVICE THAT MAKES NOTES FROM NOTES — the arps** (a category of their own, so pattern and
	/// arp do not ride along with modulators). Its own heading, after the modulators it sits beside in Feathers' strip.
	Notes,
	/// A device that holds devices — the Chain. Its own heading, last.
	Containers,
};

inline constexpr size_t kCategoryCount = static_cast<size_t>(Category::Containers) + 1;

/// What the menu heading says.
constexpr const char* categoryName(Category c) {
	switch (c) {
	case Category::FilterEq: return "Filter & EQ";
	case Category::Dynamics: return "Dynamics";
	case Category::DriveColour: return "Drive & Colour";
	case Category::TimeSpace: return "Time & Space";
	case Category::Pitch: return "Pitch";
	case Category::RhythmGlitch: return "Rhythm & Glitch";
	case Category::ModulationFx: return "Modulation FX";
	case Category::Utility: return "Utility";
	case Category::Modulators: return "Modulators";
	case Category::Notes: return "Note FX"; // Bitwig's word for the slot before an instrument
	case Category::Containers: return "Containers";
	case Category::None: break;
	}
	return "Other";
}

/// Where a category sorts in a menu: declaration order, with `None` after every real one.
constexpr size_t categoryRank(Category c) {
	return c == Category::None ? kCategoryCount : static_cast<size_t>(c);
}

/// EVERYTHING ONE DEVICE TYPE SAYS ABOUT ITSELF.
///
/// Assembled from the constexpr declarations a device already publishes — `kTraits` and
/// `kParamDescs` — rather than being a second copy of them. Both existing devices already declare
/// both, at the same shape and under the same names, which is what makes one table over the two of
/// them possible at all.
struct Descriptor {
	/// tag, name, hasCard, hasExpanded. The tag is the device's identity in a saved chain record.
	Traits traits;

	/// Its controls, in the device's own order. A control is addressed by its INDEX in this span,
	/// never by a host identifier — device.h's permanent rule.
	std::span<const ParamDesc> params;

	/// Its modulation outputs. EMPTY IS THE COMMON CASE and is not a defect: the filter publishes
	/// nothing, and most effects never will.
	std::span<const SourceDesc> outputs;

	/// HOW TO RUN IT AS AN AUDIO DEVICE, or null for one that is not. See AudioOps: null IS the
	/// capability, rather than a flag beside a pointer that could contradict it.
	const AudioOps* audio = nullptr;

	/// HOW TO ADVANCE IT AS A MODULATOR, or null for one that is not. See ModOps.
	///
	/// INDEPENDENT OF `audio`, which is the property a `lane` field would have destroyed: a device
	/// that is BOTH — a note effect that passes notes and publishes a modulation signal — is
	/// expressible here and would not be expressible if membership were a single enum.
	const ModOps* mod = nullptr;

	/// HOW IT HANDLES NOTES, or null for the devices that do not.
	///
	/// INDEPENDENT OF BOTH `audio` AND `mod`, for the reason the paragraph above gives about `lane`:
	/// Modulator effects were always expected to become note/midi effects too, injecting the
	/// modulation signal into the framework — and a device that reads
	/// notes AND publishes a modulation source is the first caller the note lane is being built for.
	/// Three independent pointers express that; a single enum could not.
	///
	/// A DEVICE DECLARING SEVERAL SHARES ONE STORAGE, and the sizes must agree — `RackEngine` takes
	/// the first non-null in a fixed order and allocates once. That convention predates this field:
	/// `audio` and `mod` have always shared a slot's bytes.
	const NoteOps* notes = nullptr;

	/// HOW TO RUN IT AS A CONTAINER, or null for the devices that hold nothing — which is every
	/// device in the tree today. See ContainerOps.
	///
	/// INDEPENDENT OF THE OTHER THREE, on this struct's standing rule. A composite declares `.audio`
	/// AND `.container`: it renders like an effect and holds like a chain, and neither fact is the
	/// other's. A `kind` enum could not have said both.
	const ContainerOps* container = nullptr;

	/// A LEAF'S BODY, or null — see `BodyOps`. Never set beside `container`.
	const BodyOps* body = nullptr;

	/// **WHO WRITES THIS DEVICE'S BODY**, a container or a leaf — null for a device with none. Every road a
	/// body travels asks this, not `container`, so a leaf's body is carried exactly as a chain's.
	using BodySaveFn = bool (*)(const void* storage, const ByteSink& sink);
	using BodyLoadFn = bool (*)(void* storage, const uint8_t* data, size_t bytes);
	[[nodiscard]] constexpr BodySaveFn bodySaver() const {
		return container != nullptr ? container->saveState : body != nullptr ? body->saveState : nullptr;
	}
	[[nodiscard]] constexpr BodyLoadFn bodyLoader() const {
		return container != nullptr ? container->loadState : body != nullptr ? body->loadState : nullptr;
	}
	/// Whether it has a body at all (so a host files its running instance beside it).
	[[nodiscard]] constexpr bool hasBody() const { return container != nullptr || body != nullptr; }

	/// **THE STATE IT ARRIVED AT ITSELF, IN KEYED BLOCKS** (backlog NM, `device/state_blocks.h`), or null for a device
	/// with none. Independent of `body`: a leaf may declare both.
	const StateOps* state = nullptr;

	/// **WHETHER A HOST KEEPS ANYTHING FOR IT BESIDE ITS ROW** — a body or blocks. Such a device's running instance is
	/// filed by id (`ContainerBodies::attach`) so a save can ask it, and a load that stores either builds it again.
	[[nodiscard]] constexpr bool hasStoredState() const { return hasBody() || state != nullptr; }

	/// WHETHER ITS OUTPUTS MAY REACH ITS OWN CONTROLS, AND WHICH NOT — or null, which is every device
	/// but one and means what it always meant: a device is not upstream of itself. See `SelfRouting`.
	/// NOT STORED STATE: it is a fact about the type, read at the routing rule and nowhere in a patch.
	const SelfRouting* selfRouting = nullptr;

	/// HOW IT PRINTS A CONTROL THE DECLARED UNIT CANNOT DESCRIBE, or null — which is every device but
	/// one, and prints exactly what it always printed. See `TextOps`.
	const TextOps* text = nullptr;

	/// WHICH HEADING AN ADD MENU FILES IT UNDER — see `Category`. Last, so a device declares it on the
	/// last line of its descriptor whatever else it declares.
	Category category = Category::None;

	/// **MAY ONE OF THIS DEVICE'S OUTPUTS REACH ITS OWN CONTROL `controlIndex`?** The self-route half of
	/// the routing rule, asked of the declaration: no declaration, no; a control the outputs are computed
	/// from — declared, or one of the two the engine's publish reads (`enableControl`, `amountControl`) —
	/// no; anything else, yes. Whether the control is a destination at all is `mayRoute`'s other test.
	constexpr bool mayRouteToSelf(size_t controlIndex) const {
		if (selfRouting == nullptr || controlIndex >= params.size()) {
			return false;
		}
		if ((traits.enableControl >= 0 && static_cast<size_t>(traits.enableControl) == controlIndex)
		    || (traits.amountControl >= 0 && static_cast<size_t>(traits.amountControl) == controlIndex)) {
			return false;
		}
		for (const uint8_t feeds : selfRouting->feedsOutputs) {
			if (static_cast<size_t>(feeds) == controlIndex) {
				return false;
			}
		}
		return true;
	}

	/// HOW MANY OF ITS CONTROLS ARE MODULATION DESTINATIONS — counted from its own flags, so a
	/// control that gains or loses `modulatable` moves this and nothing else.
	///
	/// This is the number that replaces `kRackControlsPerSlot`. The difference is not the value, it
	/// is WHO IS ASKED: a constant computed from `mod::dahdsr::kParamDescs` at compile time answers
	/// for every slot at once, and this answers for the device actually in front of you.
	constexpr size_t modulatableCount() const { return modulatableIn(params); }

	/// How many sources it contributes to the modulation bus.
	constexpr size_t outputCount() const { return outputs.size(); }
	/// **THE OUTPUTS A CARD SHOWS BEFORE IT ASKS FOR OTHERS** — a bit per output, clear where `SourceDesc::cardHidden`.
	constexpr uint64_t outputsShownByDefault() const {
		uint64_t m = 0;
		for (size_t i = 0; i < outputs.size() && i < 64; ++i) {
			m |= outputs[i].cardHidden ? 0u : uint64_t { 1 } << i;
		}
		return m;
	}

	//----------------------------------------------------------------------------------//
	// THE ORDINAL <-> CONTROL-INDEX MAPPING — the pair that lets a host address a         //
	// device's modulatable controls without knowing which device it is holding.          //
	//----------------------------------------------------------------------------------//

	// A HOST ADDRESSES A MODULATION DESTINATION BY ORDINAL — "the third modulatable control of the
	// device in slot 1" — and never by the device's own control index. The distinction looks like
	// pedantry and is the entire reason params.h can stop including a device header: an ordinal is a
	// number the host can compute from its own stride arithmetic, while a control index only means
	// something to the device that declared it.
	//
	// The two differ whenever a device has non-modulatable controls between its modulatable ones,
	// which is every device that has a switch. The DAHDSR declares fifteen controls of which ten are
	// modulatable, so its ordinal 9 is control index 9 by luck of ordering and its neighbours are
	// not; the filter's six modulatable sit inside ten declared. Nothing may assume they coincide.
	//
	// THIS REPLACES `kRackControlIndices` IN params.h, which was the same table computed once for one
	// device at compile time. The knowledge has not moved so much as gone home: which of a device's
	// controls are modulatable is the device's own declaration, and a host holding a second copy is a
	// host that must be edited every time a device grows a knob — the exact argument mod_rack.h
	// already makes about field order.

	/// The index in `params` of the n-th modulatable control, or `params.size()` when there is no
	/// such control. The out-of-range answer is a valid subscript nowhere, so a caller that ignores
	/// it gets a bounds failure rather than a wrong control — the opposite of the silent annexation
	/// this project has now paid for three times.
	constexpr size_t controlOfNthModulatable(size_t n) const {
		size_t seen = 0;
		for (size_t i = 0; i < params.size(); ++i) {
			if (!params[i].modulatable) {
				continue;
			}
			if (seen == n) {
				return i;
			}
			++seen;
		}
		return params.size();
	}

	/// **THE n-TH DESTINATION THAT IS NOT THE ENABLE (backlog KW).** Every enable is a destination now, and it is
	/// usually a device's FIRST control, so "the device's first destination" in the sense a caller means — the first
	/// thing it DOES — skips the switch. `params.size()` when there is no such control, as above.
	constexpr size_t controlOfNthModulatableBesideEnable(size_t n) const {
		size_t seen = 0;
		for (size_t i = 0; i < params.size(); ++i) {
			if (!params[i].modulatable || static_cast<int>(i) == traits.enableControl) {
				continue;
			}
			if (seen == n) {
				return i;
			}
			++seen;
		}
		return params.size();
	}

	/// Which ordinal a control index is, or `modulatableCount()` for a control that is not a
	/// destination at all. The way back, so a card holding one of its own controls can ask where it
	/// sits in the host's table without knowing how that table is laid out.
	constexpr size_t nthModulatableOfControl(size_t controlIndex) const {
		size_t seen = 0;
		for (size_t i = 0; i < params.size() && i < controlIndex; ++i) {
			if (params[i].modulatable) {
				++seen;
			}
		}
		if (controlIndex >= params.size() || !params[controlIndex].modulatable) {
			return modulatableCount();
		}
		return seen;
	}
};

/// **WHETHER THIS DEVICE'S MOD ROAD READS ITS AUDIO, BY THIS ROW** (backlog OP) — `ModOps::readsAudioOf` when the
/// device declares one, its static `readsAudio` otherwise; false for a device with no mod road. `fields` holds
/// `count` floats; a field past them is its default, as every decode reads it.
[[nodiscard]] inline bool modReadsAudio(const Descriptor& d, const float* fields, size_t count) noexcept {
	if (d.mod == nullptr) {
		return false;
	}
	return d.mod->readsAudioOf != nullptr ? d.mod->readsAudioOf(fields, count) : d.mod->readsAudio;
}

/// **WHETHER THE ROW LETS THE POLY SWITCH ACT** (`Traits::perVoiceRequires`). `valueOf(control)` reads the control
/// the way the caller reads its row (resolved, stored, or the host's field), so each site keeps its own source.
template <typename ValueOf>
[[nodiscard]] bool polyAllowed(const Descriptor& d, ValueOf&& valueOf) {
	const int c = d.traits.perVoiceRequires;
	return c < 0 || static_cast<size_t>(c) >= d.params.size() || valueOf(static_cast<size_t>(c)) >= 0.5f;
}

//------------------------------------------------------------------------------------//
// THE BOUNDS A HOST LAYS OUT ITS TABLES AGAINST — declared here because they name no    //
// device, and that is the entire trick that lets params.h stop including one.           //
//------------------------------------------------------------------------------------//

// WHY THESE ARE HERE AND NOT IN THE HOST'S TABLE. params.h needs a compile-time width for the
// modulation matrix. If it took that width from the host's device list, it would transitively
// include every device again and nothing would have changed — the pin would have moved one file
// deeper and become harder to see. So the BOUND is declared here, where it is nobody's device, and
// the check that real devices actually fit under it lives in src/plugin/device_registry.h, which is
// allowed to name them. A bound that no device can see is a bound no device can silently exceed.
//
// A HOST LAYS OUT ITS MATRIX AT THIS STRIDE, NOT AT THE ACTUAL DEVICE'S WIDTH. Address control M of
// slot N at `base + N * the retired control stride + M` and it does not matter what is in slot N-1, or
// whether slot N-1 is empty, or what the user just added.
//
// THE REASON IS A LIVE RUNTIME BUG, NOT A FILE-COMPATIBILITY ONE, and the distinction decides
// whether the ten kilobytes below are worth paying. The first draft of this comment argued that
// packed bases would re-point routings in SAVED PATCHES — true, and worth nothing now: patch
// compatibility is not a constraint, so an argument that rests on old files rests on nothing.
//
// The version that survives that decision is about the RUNNING INSTRUMENT, with no file involved. A
// browser that inserts or removes a device in slot 1 would, with packed bases, silently re-point
// every routing to slots 2 and above — MID-SESSION, under the user's hand. Route an envelope to a
// filter's cutoff, add a device above it, and the route now points at something else. No decision
// about saved presets touches that, and no amount of "there are no old patches" makes it acceptable.
//
// WHAT THE WASTE COSTS, measured rather than waved at: at four slots the matrix goes from 78
// destination columns to 166, and from 1248 to 2656 float cells — about 10 KB of atomics, computed
// once and never allocated on the audio thread. The FILE is unaffected, because a patch stores a
// sparse list of the routes that exist rather than the matrix.
//
// AND THE SOURCE AXIS GROWS TOO, which is the half with a cost on the audio thread rather than in
// bytes: sources go 16 → 28, and ParamSet::sourceOffsetAt loops over EVERY source for each
// destination read, on the audio thread. feathers-1a asked for a number rather than a shrug here,
// and was right to — this project has a rule about estimates standing in for measurements, and four
// cases in one day where the estimate was wrong exactly where a threshold sat.
//
// MEASURED IN SITU AT EVERY WIDTH THIS BRANCH HAS HAD, x86-64, -O2, one full sweep of all 107
// params. The control tick is 64 samples = 1333 us at 48 kHz.
//
//                    16s/78d   16s/166d   28s/166d   60s/422d   % of tick
//   no routes         2.74 us    2.57 us    3.19 us    2.61 us      0.20%
//   one route         5.22 us    4.39 us    7.09 us    6.98 us      0.52%
//   saturated        13.60 us   12.28 us   17.13 us   16.75 us      1.26%
//
// EIGHTY TIMES HEADROOM at the widest, with every legal pair routed — 25,296 of them, which no
// patch will ever hold.
//
// AND THE LAST COLUMN IS THE INTERESTING ONE, because it is FLAT. Sources went 28 -> 60 and
// destinations 166 -> 422, and the cost did not move (17.13 -> 16.75 us). Per source-iteration the
// loop got CHEAPER, 5.7 ns -> 2.6 ns; the wider stride evidently suits the prefetcher better than
// the narrow one did. Whatever the mechanism, the number is measured twice by the same method.
//
// THIS IS THE THIRD PROJECTION ON THIS BRANCH THAT THE INSTRUMENT OVERTURNED, AND THE FIRST IN THE
// OTHER DIRECTION — which is the part worth keeping. Scaling 28 -> 60 sources naively predicts
// 2.75% of tick; the measurement says 1.26%. An earlier projection here was OPTIMISTIC by 18.7%
// (CLAUDE.md's scratchpad case was optimistic by 22%). So the lesson is not "estimates run
// optimistic" — it is the plainer and less comfortable one this project already wrote down:
// ESTIMATES ARE WRONG AT EXACTLY THE POINT A THRESHOLD SITS, in whichever direction, and the only
// cure is the instrument. Take the shape from the model; take the number from the measurement.
//
// If it ever does bite, the fix is cheap and local, and feathers-1a named it: a per-destination
// "anything routed here" bit, so an unrouted destination costs one load instead of sixty.
//
// ONE THING THE MEASUREMENT FOUND THAT NOBODY WAS LOOKING FOR: the jump from "no routes" to "one
// route" is 23 ns, far more than one extra source costs. Most of it is modDestinationIndexOf, which
// LINEAR-SCANS the 38-entry destination table on every single read. That is untouched by these
// strides (rack destinations are not in that array) and so is not this change's problem — but it is
// the biggest single cost on this path, and worth someone's attention before the source loop is.

/// THE MOST MODULATION OUTPUTS ANY ONE DEVICE MAY PUBLISH. A stride for the source axis.
///
/// **SEVEN, BECAUSE THAT IS WHAT CLAP DEFINES.** It was four, and four is what stopped the
/// note-expression modulator from existing: this constant stopped a device from
/// declaring the seven outputs CLAP defines. `clap/events.h` has VOLUME, PAN, TUNING, VIBRATO, EXPRESSION, BRIGHTNESS, PRESSURE —
/// ids 0 through 6 — so a device that speaks note expressions wants exactly seven, and picking any
/// other number would be picking one CLAP did not.
///
/// **THE "COSTS A FORMAT CHANGE" CLAUSE THAT STOOD HERE WAS STALE, the same stale comment already
/// caught on the destination axis.** It read: "moving it later renumbers every rack destination and
/// costs a format change." A saved route stores `(instance id, output
/// index)` — `device_row.h` says so and `rack_state.h`'s `PatchCoord` is built that way — so the
/// flat source index is a LIVE address and moving the stride renumbers nothing in a file.
///
/// **AND THE COST WAS MEASURED RATHER THAN EXTRAPOLATED, because the obvious arithmetic is wrong.**
/// Going 4 → 7 takes the source axis from 60 to 96, and a proportional model says the per-node bus
/// fill gets 60 % dearer. MEASURED: 299 ns → 318 ns, **+6 %** (0.135 % → 0.143 % of a 128-frame
/// block), because an unpopulated device source is an atomic load of a zero and the fill's real cost
/// is the four macro `resolved()` calls, which do not move. See `test_mod_rack.cpp`'s bus-fill case.
///
/// **A BOUND, NOT A STRIDE**. Each engine
/// slot's output storage is sized from ITS OWN device's `outputCount()` at the install, so this sizes nothing on the
/// audio path: it bounds a route's source index (`backplane::Axes::outputStride`, `PatchCoord`), what any one device may
/// declare (`widestDeviceOutputs`), and the pair mask's width. Raised 7 -> 64 for Pattern's 43 (7 + 4 identity gates +
/// 32 Modulation-mode lanes).
inline constexpr size_t kMaxDeviceOutputs = 64;
/// THE PAIR MASK IS ONE `uint64_t` — bit n is output n (`rack::Slot::emitsPair`, `ParamSet`'s `pairNow`).
static_assert(kMaxDeviceOutputs <= 64, "a device's stereo pair mask is one uint64_t: one bit per output");

/// **THE WIDEST SETTINGS ROW ANY DEVICE MAY DECLARE** — a bound on transport, not a statement about
/// any device.
///
/// IT MOVED HERE FROM `plugin/device_row.h`, and the argument is the one the two
/// bounds above already make: *"the BOUND is declared here, where it is nobody's device, and the
/// check that real devices actually fit under it lives in src/plugin/device_registry.h, which is
/// allowed to name them."* A row's width is a transport contract in exactly the sense those are.
///
/// **WHAT FORCED THE MOVE: a container has to carry its members' rows.** A device cannot be ASKED
/// for its fields — `AudioOps` has `setFields` and no counterpart — which is the whole reason
/// `DeviceRow` exists on the host side. A container saving its body therefore has to have kept each
/// member's row, and it cannot include `plugin/device_row.h` to learn how wide one is.
///
/// DISTINCT FROM the retired control stride, and confusing the two would be a real bug. That one is the
/// MODULATABLE stride, which sizes the destination matrix; this is the count of DECLARED controls,
/// which sizes a settings row. The MSEG is why: it declares thirty-seven controls of which four are
/// modulatable.
///
/// **RAISED 128 -> 160 (backlog NR)** so DRAWN, at 127, could take the slot's Mix and
/// wet Gain furniture like every other slot-blended device; it is now at 129 of 160. The cost is the row's
/// 32 extra floats in every carrier that holds one (`DeviceRow`, `rack::DeviceState`, the published chain's
/// atomics, the telemetry cells), all fixed arrays, so nothing allocates; `ControlMask` went to three words.
/// The guard that every device fits is the static_assert in `plugin/device_row.h`, which stays there
/// because that is the file allowed to walk `kDevices`.
///
/// **RAISED 160 -> 256**
/// for Pattern (157) and the Rack's chain slots (32 -> 64). MEASURED: +106 KB of heap per instance at the default patch
/// (4.04 -> 4.15 MB, +2.6%), nothing per voice, nothing per device added (the carriers are preallocated pools), and no
/// render cost (bench_render A/B within noise). `ControlMask` went to four words.
///
/// **RAISED 256 -> 320** for Pattern, which needs about 268 once each tab has its Span, Fill,
/// Start Phase and Fill Trigger. The same pools, wider: MEASURED.
/// `ControlMask` went to five words.
inline constexpr size_t kMaxDeviceFields = 320;
static_assert(kMaxDeviceFields <= kControlMaskBits, "a ControlMask must have a bit for every control a device may declare");

//------------------------------------------------------------------------------------------------//
// HOW WIDE A DEVICE TAG IS — ONE NUMBER, IN ONE PLACE.                                             //
//------------------------------------------------------------------------------------------------//
//
// **IT WAS FOUR, AND FOUR WAS TOO FEW.** The width is on the wire, so it bounded every device's
// name-on-disk: "OWLTT" had to be stored as `OWLT` and "SCATTER" (now SKLITTER) as `SCAT`, and a reader meeting
// either had to be told the truncation was deliberate rather than a typo. A longer
// identifier was wanted, and patch compatibility is not a constraint yet,
// so the format simply changes.
//
// EIGHT, and the number is chosen rather than assumed: it holds every name in the registry today
// (the longest are "SKLITTER" and "NoteExpr", both at eight — the whole width, which the patch tag
// allows because it is stored NUL-terminated in `kTagBytes + 1`) and it is the widest that still lets
// `gui_bridge` carry a packed tag in ONE lock-free atomic word — `std::atomic<uint64_t>` is
// lock-free on both targets this ships to, and sixteen would not be.
//
// **THIS IS NOT THE WIDTH OF A STREAM SECTION MARKER.** `rack_state.cpp`'s `kMacroTag`, `kChainTag`,
// `kRouteTag` and `kCanvasTag` are four-byte markers in the state stream and have nothing to do with
// devices; `tagIs` compares those and stays at four. Widening it would move every section boundary.
inline constexpr size_t kTagBytes = 8;

/// A DEVICE TAG AS THE RECORD HOLDS IT. Not NUL-terminated when the tag fills the field, which is
/// why `sameTag` compares at most `kTagBytes` and stops early on a NUL rather than calling `strcmp`.
using Tag = std::array<char, kTagBytes>;

/// COPY A DECLARED TAG INTO THE FIELD, SAFELY.
///
/// **THE OBVIOUS LOOP IS AN OUT-OF-BOUNDS READ AND WAS ONE.** `traits.tag` is a string LITERAL, so
/// `"flt1"` is `const char[5]`; a loop running to `kTagBytes` reads past its end. At four bytes it
/// read the terminating NUL and got away with it, and widening to eight turned it into a genuine
/// overrun that `-Werror=array-bounds` caught in `defaultRowFor`. Stop at the NUL and zero-fill the
/// rest, in ONE place, so the next copy cannot reintroduce it.
[[nodiscard]] inline Tag tagOf(const char* declared) {
	Tag out {};
	if (declared == nullptr) {
		return out;
	}
	for (size_t i = 0; i < out.size() && declared[i] != '\0'; ++i) {
		out[i] = declared[i];
	}
	return out;
}


/// WHETHER TWO TAGS ARE THE SAME DEVICE. Four bytes, compared without assuming either is
/// NUL-terminated at four — a tag read out of a file is whatever the file said.
constexpr bool sameTag(const char* a, const char* b) {
	if (a == nullptr || b == nullptr) {
		return false;
	}
	for (size_t i = 0; i < kTagBytes; ++i) {
		if (a[i] != b[i]) {
			return false;
		}
		if (a[i] == '\0') {
			return true;
		}
	}
	return true;
}

/// **HOW MANY INNER SOURCES ONE SITE MAY EXPORT** (backlog NZ) — the rows of its export table
/// (`Source::exported`). A row is minted the first time a route needs one, so this bounds the distinct PRE
/// sources reaching out of one site, not the routes.
inline constexpr size_t kSiteExportRows = 16;

/// **DOES A DEVICE OF THIS KIND EXPORT ITS SITE'S SOURCES?** One that offers a PRE chain does — the lane
/// upstream of it, whose modulators reach the device and its POST (the site scope, decided O1/O3). A POST
/// modulator reaches only what comes after it, never the holder, so a POST-only site exports nothing. A Rack or a
/// Chain does too (D3 lifted), beside its container's own road: the SOURCE's bit says which road a
/// row is (`Source::isSiteExport`), this says only that the kind has the road at all.
[[nodiscard]] constexpr bool exportsSite(const Descriptor& d) noexcept {
	return d.audio != nullptr && d.traits.siteLanes == Traits::SiteLanes::PreAndPost;
}

/// **DOES A DEVICE OF THIS KIND EXPORT ITS CONTAINER'S INNER SOURCES?** (backlog NZ (b)) A Chain or a Rack: its
/// inner modulators reach the audio devices after it in its host, never its own controls (they are inside it).
[[nodiscard]] constexpr bool exportsContainer(const Descriptor& d) noexcept {
	return d.audio != nullptr && d.container != nullptr && d.container->exportCell != nullptr;
}
/// **WHETHER `d`'s KIND HAS THE ROAD THIS EXPORT NAMES** — read from the source's bit, never guessed from the kind
/// (a Rack has both roads).
[[nodiscard]] constexpr bool exportsRoadOf(const Descriptor& d, Source s) noexcept {
	return s.isSiteExport() ? exportsSite(d) : s.isContainerExport() && exportsContainer(d);
}

} // namespace feathers::device
