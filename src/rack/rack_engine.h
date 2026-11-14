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

#include "backplane/block_context.h"
#include "backplane/chunked_pool.h"
#include "backplane/route_list.h"
#include "backplane/route_probe.h"
#include "backplane/graph.h"
#include "backplane/endpoint.h"
#include "backplane/axes.h"
#include "device/latency.h"
#include "device/tick_window.h"
#include "device/macro_type.h"
#include "device/registry.h"
#include "hostslots/slot_bus.h"
#include "mod/tracker/tracker.h"
#include "rack/chain_state.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include "rack/device_cost.h"

#include <memory>
#include <new>
#include <span>
#include <string>
#include <vector>

// THE RACK — a chain of devices, a table of routes, and eight macros, hosted by nothing in particular.
//
// IT WAS THE SECOND IMPLEMENTOR OF `backplane::DeviceHost`, and that was its first justification: an
// abstraction with one implementor is an abstraction shaped like its implementor (`device.h`: *"designed
// against its first two real implementors rather than guessed at from zero"*). **It is no host now
// (kill-lanes step 2)**: the seam addresses a device by its id, and the editor-facing half moved to
// `RackBridge` — which holds the record, and so the ids — while this is the pool the block runs. **It keeps
// each device under the record's id in a slot it chooses itself** (step 3a, `adoptDevice`): a slot is this
// pool's own index, and a reader outside it asks `slotOf(id)`. The lane-named calls below are that index,
// kept by name until step 5 renames them.
//
//------------------------------------------------------------------------------------------------//
// WHAT IT DOES NOT DO YET, AND THE ONE THAT IS A REAL GAP RATHER THAN A CHOICE                     //
//------------------------------------------------------------------------------------------------//
//
// **MODULATOR DEVICES CANNOT BE SOURCES HERE, BECAUSE NOTHING CAN DRIVE ONE GENERICALLY.** By design
// the rack hosts modulators, and it should — `feathers-mod` already links standalone, so the
// DSP costs nothing. What is missing is a vocabulary entry: `AudioOps` says how a host RUNS an audio
// device, and there is no `ModOps` saying how a host ADVANCES a modulator and reads its output.
//
// FEATHERS DOES IT BY NAME. `ModRack::dahdsrAt` asks `holds(slot, mod::dahdsr::kTraits.tag)` and then
// calls `mod::dahdsr::settingsOf` — a device's identity baked into the host, which is precisely the
// pin the whole device-browser arc removed everywhere else. **`AudioOps` unpinned the fx lane; the
// SOURCE side was never unpinned.** `params.h` stopped naming a modulator when `kRackControlsPerSlot`
// died, and the DESTINATION space became heterogeneous — but "what is in this slot and what is it
// publishing" is still answered by a type test.
//
// So this engine runs audio devices and publishes MACROS as sources, which is everything the
// backplane needs to be exercised end to end. Modulator sources arrive when `ModOps` does, and that
// is a change to `device/registry.h` (the device SDK) — a shared, permanent vocabulary — so it is a design
// conversation and not a thing to invent in a corner.
//
// NOT BUILT, DELIBERATELY: growth of the slot pool (see the brief §11.1 — sized at construction),
// per-voice arrangements (a rack has no voices), and the note road.

namespace feathers::rack {

/// FREE DEVICE STORAGE THE WAY IT WAS ALLOCATED.
///
/// **A PLAIN `unique_ptr<std::byte[]>` WAS A NEW/DELETE MISMATCH, AND ASan CALLS IT ONE.** A slot's
/// storage is allocated with `new (std::align_val_t{align}) std::byte[bytes]` — a device declares its
/// own alignment and some need more than `max_align_t` — while the default deleter calls the
/// UNALIGNED `delete[]`. That is undefined behaviour and in practice heap corruption: the allocator
/// is handed a pointer it did not issue from that pool, and the damage surfaces at some LATER
/// allocation rather than at the free.
///
/// MEASURED 2026-08-31: `AddressSanitizer: new-delete-type-mismatch ... in RackEngine::destroySlot`.
/// It had been there since the engine was written. Nothing in the suite could see it, because a
/// mismatched free is not a wrong VALUE — and g++ has no diagnostic for it at all.
///
/// THE ALIGNMENT TRAVELS IN THE DELETER because it has to: `operator delete` needs the same
/// `align_val_t` the allocation used, and by the time storage is freed the descriptor that declared
/// it may already have been cleared.
///
/// AT NAMESPACE SCOPE RATHER THAN NESTED IN `Slot`: as a nested type it failed `unique_ptr`'s
/// default-constructibility constraint while the enclosing class was still incomplete, and a `Slot`
/// that cannot be default-constructed cannot live in the vector the engine sizes once at construction.
struct AlignedFree {
	std::size_t align = alignof(std::max_align_t);
	void operator()(std::byte* p) const noexcept {
		::operator delete[](static_cast<void*>(p), std::align_val_t { align });
	}
};

/// **ONE ROUTE INTO ONE CONTROL, AS THE RESOLVE READS IT: where the source's value IS, and how much of
/// it.** Nothing here is an address of a device — `value` points at a cell the source node owns (a
/// slot's `outputs[]`, or a macro), resolved from the route's identity at a drain and rebuilt at the next
/// one. Between drains nothing in the pool is replaced, so the pointer is a contemporaneous handle for
/// exactly one run of blocks, never an identity (the ABA rule, satisfied by construction rather
/// than dodged). Kill-lanes §2.4b: nodes read their input addresses (2026-09-13).
struct Input {
	const float* value = nullptr;
	/// THE SOURCE'S PER-VOICE RUN, or null for a source with one value for every voice (backlog DU: a
	/// source declares whether it varies per voice — `SourceDesc::perVoice` — and only those have one).
	/// `value` is the source's block-rate cell either way; a reader resolving FOR a voice takes
	/// `perVoice[voice]` when this is set, and the collapsed cell when it is not.
	const float* perVoice = nullptr;
	float amount = 0.0f;
	/// THE CURVE THE SOURCE IS READ THROUGH before `amount` (`device/transfer.h`). Lin keeps the old expression.
	device::Transfer transfer = device::Transfer::Lin;
	/// The source's tick window (`Slot::outputsWindow`), or null for a source with none (a macro).
	const float* window = nullptr;
	/// The source's PER-VOICE tick windows, `[voice * kTickWindowStride]` (`Slot::outputsPerVoiceWindow`), or null — a device
	/// output declared per voice has them; a per-voice outer source does not, and a voice reads its cell instead.
	const float* perVoiceWindow = nullptr;
	/// **THE SOURCE'S RIGHT EAR** (backlog KU): `Slot::outputsR` for a device output, `value` itself for a macro or an
	/// outer source (one value, both ears). Designated in the fill, like every other pointer here.
	const float* valueR = nullptr;
	/// **AND ITS RIGHT EAR AS A TICK WINDOW** (`Slot::outputsWindowR`, 2026-10-05) — beside `window`, null where that is.
	/// A per-sample right-ear reader (Feathers' Phase) ramps it exactly as the left ear ramps `window`.
	const float* windowR = nullptr;
	/// The per-voice run's right ear — the source's `outputsPerVoiceR` while its output is a pair (backlog PA, re-pointed
	/// at the drain's fixed point), and `perVoice` itself otherwise.
	const float* perVoiceR = nullptr;
	/// **WHERE THE SOURCE LIVES, FOR THE STEREO-LIVE FIXED POINT** — its slot and output, or `UINT32_MAX` for a
	/// macro or outer source (never a pair).
	uint32_t sourceLane = UINT32_MAX;
	uint8_t sourceOutput = 0;
	/// WHETHER THE SOURCE IS THE HOST OUTSIDE'S (`Source::isOuter`) — read by the latency domains (backlog LW step 4):
	/// an outer source is as early as the host's own domain, so it is delayed by where the container stands as well.
	bool fromOuter = false;
};

/// WHICH OF THE ENGINE'S `inputs_` BELONG TO ONE CONTROL — a node's own bookkeeping of its own edges,
/// written at the drain beside the inputs themselves.
struct InputRange {
	uint32_t first = 0;
	uint32_t count = 0;
};

class SiteChains;

/// **THE FURNITURE'S BLEND, AS IT STANDS** (backlog NR) — the wet and dry coefficients the slot's Mix and wet Gain
/// make, ramped over 5 ms counted in SAMPLES so the ramp is partition-independent (`InnerChain`'s reasons). At rest
/// it is the identity, wet 1 and dry 0, which is what a device at its defaults gets and what the slot skips.
struct FurnitureRamp {
	float wet = 1.0f;
	float dry = 0.0f;
	float wetTarget = 1.0f;
	float dryTarget = 0.0f;
	float wetStep = 0.0f;
	float dryStep = 0.0f;
	int remaining = 0;
	bool primed = false;
	/// **SWITCHED OFF AND SETTLED (backlog KW)**: the enable crossfaded the wet to nothing, so the device is not run at
	/// all — its tail is cut, it costs no CPU, and a SLEEP is decided by the dry alone. It stays BUILT (nothing frees),
	/// and switching it on again resets it (`AudioOps::reset`, allocation-free) before it fades back in.
	bool parked = false;

	[[nodiscard]] bool identity() const noexcept { return remaining == 0 && wet == 1.0f && dry == 0.0f; }
	/// AIM AT `wet`/`dry` over `samples` — placed outright before the first block, so a device loaded at Mix 50%
	/// starts there rather than gliding in from the identity.
	void aim(float wetTo, float dryTo, int samples) noexcept {
		if (wetTo == wetTarget && dryTo == dryTarget && primed) {
			return;
		}
		wetTarget = wetTo;
		dryTarget = dryTo;
		if (!primed || samples <= 0) {
			wet = wetTo;
			dry = dryTo;
			wetStep = dryStep = 0.0f;
			remaining = 0;
			primed = true;
			return;
		}
		wetStep = (wetTo - wet) / static_cast<float>(samples);
		dryStep = (dryTo - dry) / static_cast<float>(samples);
		remaining = samples;
	}
	void step() noexcept {
		if (remaining > 0) {
			wet += wetStep;
			dry += dryStep;
			if (--remaining == 0) {
				wet = wetTarget;
				dry = dryTarget;
			}
		}
	}
};

/// One slot: what is in it, its settings, and its live DSP.
struct Slot {
	std::atomic<const device::Descriptor*> desc { nullptr };
	/// **WHICH BUILD STANDS HERE** — a monotonic stamp from `RackEngine::adoptDevice`, moved at every install. The id alone is not
	/// an instance: a patch load retires a slot and adopts a NEW device under the SAME id (and an address can come back).
	uint64_t built = 0;
	/// The build a note device was last seeded under (`NoteOps::seedPassed`, backlog QU): once per built instance.
	uint64_t notesSeededBuild = 0;
	/// The device's own row, in its declared units — the BASE values, before modulation.
	std::vector<float> base;
	/// HOW MANY WRITES TO THIS ROW WAIT THEIR TURN (backlog LW step 3, `RackEngine::deferField`).
	uint32_t pendingFields = 0;
	/// Base plus whatever modulation reaches each control, recomputed per block. Handed to the device.
	std::vector<float> resolved;
	/// THE SAME ROW RESOLVED FOR ONE VOICE — scratch the per-voice sync fills and hands each voice instance
	/// when `anyPerVoiceInput` (backlog DU: per-voice device controls migrate). Audio thread only; never read
	/// by the editor, which draws the collapsed `resolved`.
	std::vector<float> resolvedVoice;
	bool anyPerVoiceInput = false;
	/// **THE NODE'S OUTPUTS — what this device last published, one cell per declared output.** Written
	/// by the block after the device ticks (`advanceModulators`), read by every input that points here.
	/// **SIZED FROM ITS OWN DEVICE, AND NEVER FREED BY THE AUDIO THREAD** (2026-11-07):
	/// `outputCount()` cells, built in the `Provision` on the message thread and SWAPPED in at the install, so the
	/// slot's previous buffer leaves in the provision's husk and is freed by the message thread (`Converger::reap`),
	/// never here. It was a fixed array of `kMaxDeviceOutputs` for that reason, and the reason stands: a downstream
	/// slot's `Input::value` points into this, a removal ZEROES it rather than freeing it (a block in flight between the
	/// removal and the drain that rebuilds inputs reads rest), and the install sets `routesDirty_`, so every input is
	/// re-taken by `rebuildInputs` before any resolve reads one. An empty slot keeps its last device's buffer.
	std::vector<float> outputs;
	/// **EACH OUTPUT'S TICK AS A WINDOW** (`device/tick_window.h`), the ramp from the last publish to this one,
	/// `outputCount() × kTickWindowStride`, sized and swapped as `outputs` is: an `Input::window` points into it.
	std::vector<float> outputsWindow;
	/// **THE RIGHT EAR OF EACH OUTPUT** (backlog KU), sized and swapped as `outputs` is: an `Input::valueR` points
	/// into it. A mono output writes its left value here too (one store per output per tick), so the right-ear
	/// resolve reads one cell per input with no branch.
	std::vector<float> outputsR;
	/// **THE RIGHT EAR'S TICK WINDOWS** (2026-10-05; deferred from the first stereo round, plan Q3, until a per-sample
	/// reader arrived — Phase). `outputsWindow`'s twin, written at the
	/// same publish by the same rule from `outputsR`'s last value to its new one, so a mono output's right window IS its
	/// left one. Sized and swapped as `outputs` is: an `Input::windowR` points into it.
	std::vector<float> outputsWindowR;
	/// **THE ROW AS THE RIGHT EAR HEARS IT** — written only while `stereoLive.any()`; a control that is not
	/// stereo-live holds its `resolved` value. Published like `resolved` (relaxed atomic stores) for the editor.
	std::vector<float> resolvedR;
	/// One voice's right-ear row, `resolvedVoice`'s twin. Audio thread only.
	std::vector<float> resolvedVoiceR;
	/// **WHICH CONTROLS A STEREO ROUTE REACHES** — written at the drain (`rebuildInputs`), read through `atomic_ref`.
	device::ControlMask stereoLive {};
	/// `stereoLive` WHILE THE DRAIN WORKS IT OUT — the fixed point's scratch, audio thread only. Copied into
	/// `stereoLive` word by word through `atomic_ref` when it is done, so the editor never reads a half-solved mask.
	device::ControlMask stereoLivePending {};
	/// **WHICH OUTPUTS ARE PAIRS** — bit n is output n. `pairAlways` outputs, plus the `pairFrom` ones the drain's
	/// fixed point finds live.
	uint64_t emitsPair = 0;
	/// `emitsPair` as the last drain left it — the fixed point's scratch, so an output that becomes a pair is primed.
	uint64_t pairsBefore = 0;
	static_assert(device::kMaxDeviceOutputs <= 64, "emitsPair has a bit per output");
	/// **EACH DECLARED CONTROL, RESOLVED PER SAMPLE FOR THE TICK**, `[control * kTickWindowStride]`, flat flag last —
	/// sized only for a device that declares `AudioOps::windowedControls` (and the op that reads them), empty otherwise;
	/// only the declared controls are ever written, and every window starts FLAT.
	std::vector<float> controlWindow;
	/// **AND PER VOICE**, `[(voice * base.size() + control) * kTickWindowStride]` — for a device with voice instances whose
	/// row a per-voice input resolves per voice (`resolveVoiceInto`). Empty otherwise; every window starts FLAT.
	std::vector<float> controlWindowVoice;
	/// **THE NODE'S INPUTS, PER CONTROL** — `params.size()` ranges into the engine's `inputs_`, rebuilt
	/// at the drain from the routes by identity. A control with no route has an empty range and costs
	/// the resolve one comparison.
	std::vector<InputRange> inputs;
	/// THE ROUTE SPAN OF EACH CONTROL, or a negative for a control that is no destination — computed once at
	/// provision, so the tick does not ask the descriptor (see `provisionFor`).
	std::vector<double> routeSpan;
	/// **THE HOST'S MODULATION OF EACH CONTROL, IN NORMALISED UNITS** (host slots, T5) — a bound slot's
	/// `SlotValueBus::modOf`, copied here every block by `RackEngine::applyHostSlots` and added by
	/// `resolveControl` as `hostMod × span` on top of every route. Zero for a control no slot drives. A
	/// FIXED ARRAY, reset when a device is adopted into the slot, so a lane reused by another device
	/// inherits no offset. Audio thread only.
	std::array<float, device::kMaxDeviceFields> hostMod {};
	/// **WHAT THE LAST FULL RESOLVE WAS BUILT FROM** (the idle-overhead pass, 2026-11-05). A control no route reaches resolves
	/// to a pure function of its base, its host modulation and its install (`resolveControl`: no input, so the offset is
	/// the host's alone), so once the row has been resolved whole, only a control WITH an input can move until one of
	/// those three does. Serials, never addresses (CLAUDE.md's ABA rule): `baseSerial` moves at every write to `base` and
	/// at every install, `hostModSerial` at every host offset that changes, and the engine's `routesSerial_` at every
	/// rebuild of the inputs. `resolveInto` re-resolves the whole row when any of the three differs from what it stamped,
	/// and otherwise only the routed controls. MEASURED on a large patch: 26k controls resolved a 1024-frame block at rest,
	/// almost all of them a copy of a base that had not moved (a Pattern's 233 fields, about 85 times a block).
	uint64_t baseSerial = 1;
	uint64_t hostModSerial = 0;
	uint64_t resolvedBase = 0;
	uint64_t resolvedHostMod = 0;
	uint64_t resolvedRoutes = 0;
	uint32_t routedControls = 0; ///< controls an input reaches, counted by the last whole resolve
	/// **THE NODE'S OUTPUTS PER VOICE — "voices within an edge"**. Laid out
	/// `[output * voices + voice]`, so an input that varies per voice holds ONE pointer, at
	/// `[output * voices]`, and reads `perVoice[voice]`. Written by the host, which is what knows its
	/// voices and their notes (`writeOutputForVoice`); `voices × outputCount()` for a device that declares a `perVoice`
	/// output, none otherwise, built in the `Provision` and swapped in at the install like `outputs`, and resized by
	/// `setVoices` (message thread, at prepare, no block running).
	///
	/// **THE CONTRACT (2026-11-07), which replaced "sized once, never reallocated": RE-TAKEN AT THE DRAIN THAT INSTALLS; OLD
	/// STORAGE OUTLIVES THE EPOCH.** Every pointer into these pools or into `outputs`/`outputsR`/the windows is re-taken at
	/// the drain whose install swapped them: the engine's own inputs by `rebuildInputs` (the install sets `routesDirty_`),
	/// and the host's node by `ParamSet::rebuildCoreInputs` (`Synth::compileRoutes`, run by the structure revision the
	/// convergence bumps) — both before any resolve reads one. The old buffers leave in the provision's husk, which the
	/// converger frees on the message thread only once a whole block has passed since the drain handed it back
	/// (`Converger::Posted::retiredAt`).
	std::vector<float> outputsPerVoice;
	/// **THE TICK'S 64 SAMPLES PER OUTPUT PER VOICE**,
	/// `[output * voices * kTickWindowStride + voice * kTickWindowStride + i]` (device/tick_window.h) — the ramp from the last tick's value to this one.
	std::vector<float> outputsPerVoiceWindow;
	/// **AND THEIR RIGHT EARS** (backlog PA), laid out and sized exactly as the two above. Written only for an output that
	/// is a pair (`emitsPair`), by `writeOutputPairForVoice`; an input from any other output reads the left ear's run as
	/// its right (`Input::perVoiceR`), so a mono source never pays for these.
	std::vector<float> outputsPerVoiceR;
	std::vector<float> outputsPerVoiceWindowR;
	/// SAMPLES OWED TO THIS MODULATOR since its last tick. A device declares its own block length
	/// (`ModOps::blockSamples`), so the accumulator is per slot rather than one grid for the rack —
	/// which is what lets a guest modulator run at a rate the two builtins do not share.
	size_t tickAccum = 0;

	/// **THE TRIG COUNT OF A DEVICE PUBLISHING FROM ITS VOICES, LATCHED** (`RackEngine::trigFiresAt`; backlog MY/OP). Under
	/// Poly the count a card reads is the NEWEST note's voice's — but each voice counts its own history, so reading
	/// whichever voice is newest would jump when the newest changes and flash a fire nobody made. So this count grows
	/// by the newest voice's own increase while it stays the newest, and by nothing on the tick another voice becomes
	/// it. Written on the audio thread after the slot's ticks; `trigVoice` is the voice it was last read from.
	uint32_t trigLatched = 0;
	uint32_t trigVoiceRaw = 0;
	size_t trigVoice = SIZE_MAX;

	/// **WHERE THIS DEVICE'S ROUTING COLUMNS ARE IS ITS OWN INDEX, AND THAT IS THE POINT OF STEP 2.**
	///
	/// There was a `routeLane` field here, because `slots_` was indexed by POSITION and a device had
	/// to carry its lane across every reorder. Now `slots_` is indexed by LANE and the graph owns the
	/// order, so a device's lane IS where it lives and a reorder moves nothing at all.
	///
	/// **THE FIELD WAS TWO COPIES OF ONE TRUTH AND IT COST US THREE BUGS**: a re-added device landing
	/// in the deleted one's place, the order/lane divergence in the drag road, and `removeDevice`
	/// clearing the wrong device's routes in a shipped build. Every one was position and identity
	/// disagreeing. They cannot disagree now.
	/// **THE DEVICE'S IDENTITY IN THE GRAPH.** The graph is keyed on this, not on where the device
	/// sits; `graph().laneOf(id)` is the inverse of this slot's own index.
	feathers::DeviceInstanceId id = feathers::kNoInstance;

	/// The DSP itself, `AudioOps::stateBytes` of it. Allocated on the message thread and never on the
	/// audio thread; `FxRunner`'s own header records the decision that construction-time allocation
	/// is fine and that widest-device pre-sizing is not.
	/// THE DEVICE'S STATE, FREED THE WAY IT WAS ALLOCATED.
	///
	/// **A PLAIN `unique_ptr<std::byte[]>` WAS A NEW/DELETE MISMATCH AND ASan CALLS IT ONE.** The
	/// storage is allocated with `new (std::align_val_t{align}) std::byte[bytes]` — a device declares
	/// its own alignment and some of them need more than `max_align_t` — and the default deleter calls
	/// the UNALIGNED `delete[]`. That is undefined behaviour and, in practice, heap corruption: the
	/// allocator is handed a pointer it did not issue from the aligned pool, and the damage surfaces
	/// at some LATER allocation rather than here.
	///
	/// MEASURED: `AddressSanitizer: new-delete-type-mismatch ... in RackEngine::destroySlot`. It had
	/// been there since the engine was written; nothing in the suite could see it, because a mismatched
	/// free is not a wrong VALUE and g++ has no diagnostic for it.
	///
	/// THE ALIGNMENT TRAVELS WITH THE POINTER, in the deleter, because it has to: `operator delete`
	/// needs the same `align_val_t` the allocation used, and the descriptor that declared it is not
	/// reachable from a deleter.
	std::unique_ptr<std::byte[], AlignedFree> storage;

	/// **ONE MORE INSTANCE PER VOICE, for a device that declares a per-voice road** — the lane's
	/// arrangement, which Feathers ran on a second runner (`FxRunner`'s per-voice instance) and this
	/// engine now runs itself, so the lane and the rack are one chain.
	///
	/// **BUILT ONLY WHILE THE SWITCH IS ON OR CAN BE FLIPPED ON THE AUDIO THREAD** (backlog NS, 2026-10-18). They
	/// were built whether or not it was, for `FxRunner`'s reason — a device BUILT the first time it became per-voice
	/// would allocate mid-block — and that cost 20 instances per device with Poly off: 672 KiB for the default
	/// patch's one Filter, 4.4 MiB for a Spectral, MEASURED. The converger builds them on the message thread when
	/// the record's Poly goes on and the drain swaps them in (`adoptVoices`); off, the drain swaps them out and the
	/// message thread frees them (`releaseVoices`). A switch a route or a host slot can reach is a switch the
	/// audio thread flips, so those devices keep the eager road (`Converger::wantsVoices`), as does every direct
	/// `addDeviceAt`. Empty for a device with no per-voice road, and for every device in a rack with no voices —
	/// the standalone rack — where a Poly device simply runs on the bus.
	std::vector<std::unique_ptr<std::byte[], AlignedFree>> voices;

	/// WHETHER THIS SLOT RUNS PER VOICE THIS BLOCK — decided once in `beginBlock` from the resolved row,
	/// so the voice pass and the bus pass of one block cannot disagree about it.
	bool polyThisBlock = false;
	/// WHAT IT WAS LAST BLOCK, so the instances that stop being responsible are reset rather than left
	/// holding a tail that a flip back would replay — the rule `FxRunner` pays for with a case.
	bool polyLastBlock = false;

	/// **THE COURTESY OUTPUT METER** (backlog MK): the loudest sample this device put out, held and falling — see
	/// `RackEngine::outputPeakAt`. `outputChunkPeak` gathers one chunk (the bus pass and every voice's), and the end of
	/// `processBus` folds it into the held peak. Audio thread only, all but the published value.
	float outputChunkPeak = 0.0f;
	/// How many times the bus instance has been run (test seam, backlog KW: a parked device is not run).
	uint64_t runs = 0;
	float outputHeld = 0.0f;
	int64_t outputHoldLeft = 0;
	/// WHAT THE EDITOR READS: the held peak, linear and unclamped (an over is the point), or NEGATIVE before this
	/// device has run a chunk — "not metered", which a card draws as no meter rather than a silent one.
	std::atomic<float> outputPeak { -1.0f };

	/// **THE CHAIN ACCESS SITE — this slot's PRE and POST chains, or null** (backlog NR). Owned by the host's converger
	/// and offered through `RackEngine::offerSite`; the drain installs it here only when its owner is this slot's `id`,
	/// and a removal clears it without freeing it. Read with acquire on every road that runs it.
	std::atomic<SiteChains*> site { nullptr };
	/// THE FURNITURE'S BLEND ON THE BUS, and ONE PER VOICE for a device with a per-voice road — sized in the provision
	/// (`Traits::mixControl` with the slot blending), empty otherwise. Audio thread only.
	FurnitureRamp furniture {};
	std::vector<FurnitureRamp> furnitureVoice;
	/// **THE FURNITURE'S DRY, WAITING FOR A LATENT DEVICE** — sized in the provision for a blending device that can
	/// declare a latency; a slot with a site uses the site's instead, which waits for the site's chains too.
	device::CompensationDelay furnitureDelay;

	/// WHAT THIS SLOT IS, READ WITH ACQUIRE ORDERING — the one road to `desc`.
	///
	/// **IT IS ATOMIC BECAUSE THE EDITOR PUBLISHES IT AND THE AUDIO THREAD READS IT**, with no lock
	/// between them. `addDeviceAt` builds the slot and then releases this pointer; every reader
	/// acquires it, which is what makes the construction that happened before the store visible to the
	/// thread that sees it. A plain pointer here was a data race in the language's own terms and a
	/// crash in Bitwig's — see `addDeviceAt`.
	///
	/// A FENCE BESIDE A PLAIN STORE WAS THE FIRST ATTEMPT AND IT WAS THE WEAKER TOOL: it is not
	/// analysable, and ThreadSanitizer refuses to compile it at all
	/// (*"'atomic_thread_fence' is not supported with '-fsanitize=thread'"*). An instrument declining
	/// to reason about your synchronisation is worth more than a green run of it.
	const device::Descriptor* live_desc() const noexcept {
		return desc.load(std::memory_order_acquire);
	}

	bool live() const noexcept { return live_desc() != nullptr && storage != nullptr; }
	bool isAudio() const noexcept {
		const device::Descriptor* d = live_desc();
		return d != nullptr && storage != nullptr && d->audio != nullptr;
	}
	bool isMod() const noexcept {
		const device::Descriptor* d = live_desc();
		return d != nullptr && storage != nullptr && d->mod != nullptr;
	}
};

class RackEngine final {
public:
	/// A LANE THAT HOLDS NOTHING — the graph's own sentinel, re-exported for the engine's lane API.
	static constexpr size_t kNoLane = backplane::Graph::kNoLane;

	/// **THE CATALOGUE IS WHOEVER HOSTS THIS RACK'S LIST OF DEVICES, NOT THE BUILD'S.** A rack may be
	/// compiled separately from its host — a third party's chain can hold one, and it can hold a third
	/// party's devices — so it cannot have a device list compiled in. The standalone plugin passes
	/// `hostCatalogue()`; a rack inside a chain is handed its host's through `ContainerOps::adopt`.
	///
	/// REQUIRED RATHER THAN DEFAULTED, so every place that builds a rack says where its devices come
	/// from. A default empty catalogue would make a forgotten one a rack that silently holds nothing.
	RackEngine(const backplane::Axes& axes, const device::Catalogue& catalogue);

	/// A RACK BUILT BEFORE ITS HOST COULD SAY — a rack that is itself a device is constructed by
	/// whatever builds devices and adopted after. MESSAGE THREAD, before anything is added.
	void setCatalogue(const device::Catalogue& catalogue) noexcept { catalogue_ = catalogue; }
	[[nodiscard]] const device::Catalogue& catalogue() const noexcept { return catalogue_; }
	~RackEngine();

	RackEngine(const RackEngine&) = delete;
	RackEngine& operator=(const RackEngine&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                               //
	//--------------------------------------------------------------------------------------------//

	/// Put a device in the first free slot. Returns the slot, or `kNoSlot` when the rack is full or
	/// the tag names nothing this build can run.
	///
	/// A DEVICE THAT IS NEITHER AN AUDIO DEVICE NOR A MODULATOR IS REFUSED — null IS the capability
	/// in both cases, so "can this rack run it" is `audio != nullptr || mod != nullptr` and nothing
	/// else. A device that is BOTH is run as both, which is the property keeping membership out of a
	/// single enum.
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).**
	/// `RackBridge::drain()` runs at `process()` and `paramsFlush()`, which CLAP guarantees never
	/// overlap, so a write from there cannot land mid-block. A UI-thread call does, and TSan has
	/// named both families it produces: `setField` against `resolveInto`, and `publishOrder`
	/// against `advanceNotes`. **This is an INVARIANT AND NOT A GUARANTEE**: these stay public
	/// because the loader (rack_state.cpp) and the test extension still call them, and re-routing
	/// a LOADER through a queue is a different design question from re-routing an editor — it is
	/// filed as its own item rather than smuggled in here.
	size_t addDevice(const char* tag);

	/// Put a device in a NAMED slot, or `kNoSlot` if that slot is taken, out of range, or the tag names
	/// nothing this build can run.
	///
	/// IT EXISTS FOR THE LOADER. A patch records which slot each device was in, so that a device this
	/// build cannot run leaves a HOLE rather than shifting everything after it — and the routes, which
	/// are keyed on slot, go on meaning what they meant. Filling densely on load would silently
	/// re-point every route above a skipped device, which is the ABA failure arriving through a file
	/// instead of through a pool.
	/// **PLACE A DEVICE AT A POSITION IN THE CHAIN.** `at` is a POSITION, not a lane — it is where in
	/// the path the device goes, which is what a drop between two cards means.
	///
	/// `wantLane` names the lane to build it at, or `kNoSlot` for the lowest free one. A patch load
	/// names it: a restored device must land on the lane it was saved with, or every route in the
	/// patch points at a different device. Returns the LANE, which is what every other accessor here
	/// is indexed by.
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).**
	/// `RackBridge::drain()` runs at `process()` and `paramsFlush()`, which CLAP guarantees never
	/// overlap, so a write from there cannot land mid-block. A UI-thread call does, and TSan has
	/// named both families it produces: `setField` against `resolveInto`, and `publishOrder`
	/// against `advanceNotes`. **This is an INVARIANT AND NOT A GUARANTEE**: these stay public
	/// because the loader (rack_state.cpp) and the test extension still call them, and re-routing
	/// a LOADER through a queue is a different design question from re-routing an editor — it is
	/// filed as its own item rather than smuggled in here.
	size_t addDeviceAt(size_t at, const char* tag);

	/// **A DEVICE BUILT WHERE ALLOCATION IS ALLOWED, WAITING TO BE ADOPTED** (backlog CF). The
	/// storage, the constructed and prepared device inside it, and the slot's two rows — everything
	/// an add used to allocate on the drain. Made by `provision` on the message thread; adopted by
	/// `adoptDeviceAt` on the drain with moves only. One that is never adopted is destroyed by
	/// whoever holds it, on that holder's thread; the drain never frees one.
	struct Provision {
		std::unique_ptr<std::byte[], AlignedFree> storage;
		/// The per-voice instances, built on the message thread beside the bus one. See `Slot::voices`.
		std::vector<std::unique_ptr<std::byte[], AlignedFree>> voices;
		const device::Descriptor* desc = nullptr;
		std::vector<float> base;
		std::vector<float> resolved;
		std::vector<float> resolvedVoice;
		std::vector<float> resolvedR;
		std::vector<float> resolvedVoiceR;
		std::vector<float> controlWindow;
		std::vector<float> controlWindowVoice;
		/// THE DEVICE'S OUTPUT STORAGE, sized from its own `outputCount()` (see `Slot::outputs`) — the four block-rate
		/// buffers and the four per voice. Swapped into the slot at the install; the slot's old ones come back here.
		std::vector<float> outputs;
		std::vector<float> outputsWindow;
		std::vector<float> outputsR;
		std::vector<float> outputsWindowR;
		std::vector<float> outputsPerVoice;
		std::vector<float> outputsPerVoiceWindow;
		std::vector<float> outputsPerVoiceR;
		std::vector<float> outputsPerVoiceWindowR;
		/// One empty range per declared control — sized here, where allocation is allowed, so the
		/// adopting drain has nothing to size.
		std::vector<InputRange> inputs;
		std::vector<double> routeSpan;
		/// THE FURNITURE'S PER-VOICE BLENDS AND ITS DRY DELAY (backlog NR) — see `Slot::furnitureVoice`.
		std::vector<FurnitureRamp> furnitureVoice;
		device::CompensationDelay furnitureDelay;
		bool live() const noexcept { return storage != nullptr && desc != nullptr; }

		/// **A PROVISION THAT DIES UNADOPTED DESTRUCTS THE DEVICES IT BUILT.** It freed their bytes and
		/// ran no destructor — the storage is a byte array, and a byte array's deleter knows nothing of
		/// what was constructed in it. Harmless for a device with no members that own anything; a LEAK
		/// for one that does, and a rack in a slot owns a whole engine of devices. Every refused add,
		/// every refused patch device and every displaced lane provision went this way.
		///
		/// AFTER AN ADOPT THERE IS NOTHING HERE TO DESTRUCT — the storage moved out and the voices
		/// vector is the slot's old, empty one — so the drain's move leaves a husk this frees quietly.
		Provision() = default;
		Provision(Provision&& other) noexcept { take(other); }
		Provision& operator=(Provision&& other) noexcept {
			if (this != &other) {
				release();
				take(other);
			}
			return *this;
		}
		Provision(const Provision&) = delete;
		Provision& operator=(const Provision&) = delete;
		~Provision() { release(); }

	private:
		void take(Provision& other) noexcept {
			storage = std::move(other.storage);
			voices = std::move(other.voices);
			desc = other.desc;
			base = std::move(other.base);
			resolved = std::move(other.resolved);
			resolvedVoice = std::move(other.resolvedVoice);
			resolvedR = std::move(other.resolvedR);
			resolvedVoiceR = std::move(other.resolvedVoiceR);
			controlWindow = std::move(other.controlWindow);
			controlWindowVoice = std::move(other.controlWindowVoice);
			outputs = std::move(other.outputs);
			outputsWindow = std::move(other.outputsWindow);
			outputsR = std::move(other.outputsR);
			outputsWindowR = std::move(other.outputsWindowR);
			outputsPerVoice = std::move(other.outputsPerVoice);
			outputsPerVoiceWindow = std::move(other.outputsPerVoiceWindow);
			outputsPerVoiceR = std::move(other.outputsPerVoiceR);
			outputsPerVoiceWindowR = std::move(other.outputsPerVoiceWindowR);
			inputs = std::move(other.inputs);
			routeSpan = std::move(other.routeSpan);
			furnitureVoice = std::move(other.furnitureVoice);
			furnitureDelay = std::move(other.furnitureDelay);
			other.desc = nullptr;
		}
		/// Destructs through the table that constructed — see `destructWith` in the .cpp.
		void release() noexcept;
	};
	/// MESSAGE THREAD. Allocates. Empty (`!live()`) for a tag this rack cannot run. Reads `sampleRate_`
	/// and nothing else of this object — **it builds a device, not a place**: where the device lives is
	/// the engine's to choose when it adopts it (kill-lanes step 3a).
	///
	/// **`withVoices` false BUILDS THE BUS INSTANCE ALONE** (backlog NS): a host that knows the device's Poly is
	/// off and cannot be flipped on the audio thread (the converger, `Converger::wantsVoices`) builds the voice
	/// instances later, with `provisionVoices`, when the switch goes on. True is the eager road every direct
	/// caller (`addDeviceAt`, the cases) has always had.
	Provision provision(const char* tag, bool withVoices = true) const;
	/// **THE VOICE INSTANCES ALONE, FOR A DEVICE ALREADY RUNNING** (backlog NS) — `desc`'s instance per voice and
	/// their tick windows, built exactly as `provision` builds them, in a provision with no bus storage (so it is
	/// never `live()`, and no add can take it). MESSAGE THREAD. Empty voices for a device with no per-voice road or
	/// a rack with no voices.
	Provision provisionVoices(const device::Descriptor& desc) const;
	/// **THE DRAIN TAKES THOSE INSTANCES** — swapped in, so nothing is allocated or freed here; the provision goes
	/// home holding the slot's old, empty vectors. False, and `p` untouched, unless the slot runs `p.desc` with no
	/// voices of its own. The next block reads the slot's Poly as it always has, and a flip on resets the bus
	/// instance, as it did when the voices had been waiting all along.
	bool adoptVoices(size_t slot, Provision& p) noexcept;
	/// **AND GIVES THEM BACK** when the switch is off: the slot's voice instances and their windows swapped INTO
	/// `p`, for whoever holds it to destruct and free off the drain. No block is in flight at a drain, so nothing is
	/// inside them. False, and nothing moved, unless the slot runs `p.desc` and holds voices.
	bool releaseVoices(size_t slot, Provision& p) noexcept;
	/// THE DRAIN. Allocates nothing. Puts the device at chain position `at`, UNDER THE ID ITS RECORD GAVE
	/// IT (`kNoInstance`: the engine issues one — a caller with no record), in the lowest slot this engine
	/// has free. Returns that slot — the engine's own business, which a caller may use only to ask this
	/// engine about the device — or `kNoSlot` with the provision handed back untouched in `p`: a position
	/// past the end, no free slot, or an id this engine already runs.
	///
	/// **THE RECORD'S ID, NOT A SECOND ONE.** This issued the engine's OWN id every time, so an engine's
	/// devices and its record's agreed only while two counters advanced in lockstep — and the cases that
	/// compared them were leaning on that without knowing. One device, one id, on both sides.
	size_t adoptDevice(size_t at, Provision&& p, DeviceInstanceId id = kNoInstance);

	/// WHERE THIS ENGINE KEEPS DEVICE `id`, or `kNoSlot` — for a reader on the engine's side of the handover
	/// (the converger, the host that runs it, a mirror it publishes). The answer is a slot of THIS pool and
	/// means nothing anywhere else.
	[[nodiscard]] size_t slotOf(DeviceInstanceId id) const noexcept {
		const size_t lane = graph().laneOf(id);
		return lane == backplane::Graph::kNoLane ? kNoSlot : lane;
	}

	/// **HOW MANY TIMES A DEVICE HAS ARRIVED IN OR LEFT ONE OF THIS ENGINE'S SLOTS** — what a plan compiled
	/// against the engine's own layout compared to know it must compile again; `rebuildInputs` runs at the drain
	/// instead. A reorder moves no device between slots and does not count. AUDIO THREAD's, like the
	/// verbs that move it.
	[[nodiscard]] uint32_t layoutRevision() const noexcept { return layoutRevision_; }

	/// EVERY ROUTE THIS ENGINE HOLDS, BY ENDPOINT — `f(const backplane::Route&)`. MESSAGE THREAD, with no
	/// block running: for a host taking an engine as it finds it (`RackBridge`'s constructor).
	template <typename F>
	void forEachRoute(F&& f) const {
		graph().forEachRoute(std::forward<F>(f));
	}

	/// **THE MESSAGE THREAD'S HALF OF A REMOVE**: destruct and free what the drain parked, where freeing
	/// is allowed. One consumer; never the audio thread. An entry parked with a block in flight waits
	/// for the epoch to move past it. See `retired_`.
	void reapRetired() noexcept;

	//--------------------------------------------------------------------------------------------//
	// GROWTH — unbounded devices step 4                                                           //
	//--------------------------------------------------------------------------------------------//
	//
	// **THE ENGINE HOLDS AS MANY DEVICES AS IT HAS BEEN GROWN TO, AND GROWING IT NEVER ALLOCATES ON THE AUDIO
	// THREAD.** The per-slot stores are chunked pools (`backplane::ChunkedPool`): the main thread adds chunks
	// and nothing moves, so the drain and the block go on reading through them. The GRAPH cannot grow that
	// way — the drain mutates it — so a bigger one is built here and adopted at the next drain
	// (`adoptGrowth`, moves only), and the old one comes back to this thread to be freed (`reapGrowth`).

	/// MAIN THREAD, ALLOCATES. Make room for at least `slots` devices. The pools grow at once; the graph that
	/// lets a device be PLACED in the new room arrives at the next drain. Returns false — and grows nothing —
	/// while an earlier graph is still waiting for its drain; ask again after `reapGrowth`. A capacity never
	/// shrinks.
	bool requestCapacity(size_t slots);
	/// THE DRAIN'S HALF — the converger calls it before it places anything. Moves only.
	void adoptGrowth() noexcept;
	/// **THE SITE MAILBOX** (backlog NR) — how a chain access site the host built reaches the slot of the device it
	/// belongs to. The host's converger owns every site; an offer carries a pointer to one (or null: "this device has
	/// none"), and the drain installs it into the slot whose `id` is `id` — or into nothing, when no slot holds that
	/// device — and hands the envelope back. Nothing the audio thread does frees a site.
	struct SiteOffer {
		DeviceInstanceId id = kNoInstance;
		SiteChains* site = nullptr;
		SiteOffer* next = nullptr;
	};
	/// MESSAGE THREAD — a lock-free push. The envelope is the engine's until `takeSpentSiteOffers` returns it.
	void offerSite(SiteOffer* offer) noexcept;
	/// MESSAGE THREAD — every envelope the drain has finished with, to be freed or reused.
	[[nodiscard]] SiteOffer* takeSpentSiteOffers() noexcept;
	/// THE DRAIN'S HALF, in posting order, so the newest offer for a device is the one that stands. No allocation.
	void adoptSites() noexcept;
	/// Whether an offer is waiting — a drain that has one cannot be skipped (`Converger::settledAt`).
	[[nodiscard]] bool siteOffersPending() const noexcept {
		return siteInbox_.load(std::memory_order_acquire) != nullptr;
	}
	/// THE SITE INSTALLED FOR DEVICE `id`, or null. A contemporaneous handle, never an identity.
	[[nodiscard]] SiteChains* siteOf(DeviceInstanceId id) const noexcept;
	/// Whether any slot holds `site` now — the converger's first test before it frees one.
	[[nodiscard]] bool holdsSite(const SiteChains* site) const noexcept;
	/// MESSAGE THREAD, NO BLOCK RUNNING — forget every installed site (the converger that owns them is going).
	void dropSites() noexcept;
	/// THE BLOCK EPOCH NOW — odd while a block is in flight (`blockEpoch_`). What a site's free waits on.
	[[nodiscard]] uint64_t blockEpochNow() const noexcept { return blockEpoch_.load(std::memory_order_seq_cst); }

	/// Whether a bigger graph is waiting for `adoptGrowth` — a drain that has it cannot be skipped (`Converger::settledAt`).
	[[nodiscard]] bool growthPending() const noexcept { return growthState_.load(std::memory_order_acquire) == kGrowthPending; }
	/// MAIN THREAD: free a graph the drain has replaced. Also run by `reapRetired`. Returns whether it did.
	bool reapGrowth() noexcept;
	/// How many devices the engine can PLACE now — the graph's room, which is what a drain is bound by.
	[[nodiscard]] size_t capacity() const noexcept { return graph().capacity(); }
	/// How many the pools reach — the main thread's view, which may be ahead of `capacity()` by one drain.
	[[nodiscard]] size_t requestedCapacity() const noexcept { return slots_.capacity(); }
	/// Told, on the main thread, each time the pools grow — for a host that keeps a per-lane store beside
	/// the engine (a readings hop, a telemetry cell) and must have one for every lane a device can take.
	size_t addGrowthListener(std::function<void(size_t capacity)> listener) {
		growthListeners_.push_back(std::move(listener));
		return growthListeners_.size() - 1;
	}
	/// For a listener that dies before the engine does. Main thread.
	void removeGrowthListener(size_t token) {
		if (token < growthListeners_.size()) {
			growthListeners_[token] = nullptr;
		}
	}
	/// **AN INSTANCE SOMEBODY STILL HOLDS STAYS PARKED** (backlog JX). Asked of every parked instance before
	/// it is freed; true keeps it — and everything parked behind it, since the ring is in order — for a
	/// later reap. The converger installs it over its host's pins (`ContainerBodies::pin`): a chain's editor
	/// holds the bridge inside the instance until its next follow, and a reap between the two freed it.
	/// Message thread, like the reap. The destructor frees regardless: nothing may outlive the engine.
	using ReapHold = bool (*)(const void* context, const void* storage) noexcept;
	void setReapHold(ReapHold hold, const void* context) noexcept {
		reapHold_ = hold;
		reapHoldContext_ = context;
	}

	/// Empty a slot, and CLEAR EVERY ROUTE INTO OR OUT OF ITS LANE FIRST.
	///
	/// **THE DEVICE IS PARKED, NEVER DESTROYED HERE** (backlog CF): destruction frees, and this runs
	/// on the drain. The message thread reaps the parking (`reapRetired`). Returns FALSE when the
	/// parking is full — the device stays, untouched, and the caller says so; the drain neither
	/// blocks, drops, nor frees. True when there was nothing to remove.
	///
	/// THE ABA GUARD, AND IT IS NOT OPTIONAL. A lane handed to a new device while still carrying the
	/// old one's amounts is a device that arrives pre-modulated by routes nobody drew. It is silent,
	/// it survives a save, and the first symptom is a sound nobody asked for.
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).**
	/// `RackBridge::drain()` runs at `process()` and `paramsFlush()`, which CLAP guarantees never
	/// overlap, so a write from there cannot land mid-block. A UI-thread call does, and TSan has
	/// named both families it produces: `setField` against `resolveInto`, and `publishOrder`
	/// against `advanceNotes`. **This is an INVARIANT AND NOT A GUARANTEE**: these stay public
	/// because the loader (rack_state.cpp) and the test extension still call them, and re-routing
	/// a LOADER through a queue is a different design question from re-routing an editor — it is
	/// filed as its own item rather than smuggled in here.
	bool removeDeviceAtLane(size_t slot);

	/// **MOVE A DEVICE FROM ONE SLOT TO ANOTHER, taking its routes with it.** Returns false and
	/// changes nothing if either index is out of range or `from` holds no device.
	///
	/// The device keeps its `routeLane`, so every route drawn to or from it survives the move
	/// untouched — see `Slot::routeLane`. What DOES have to be redone is `AudioOps::setSlot`, which
	/// tells a device its own index: a device that moved and was not told still reports the slot it
	/// used to be in, and OWLTT's meters publish against exactly that number.
	///
	/// EVERY DEVICE BETWEEN THE TWO POSITIONS SHIFTS BY ONE, which is what a drag along a chain means
	/// — not a swap. A swap would reorder two devices and leave the ones between them in an order the
	/// hand did not ask for.
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).**
	/// `RackBridge::drain()` runs at `process()` and `paramsFlush()`, which CLAP guarantees never
	/// overlap, so a write from there cannot land mid-block. A UI-thread call does, and TSan has
	/// named both families it produces: `setField` against `resolveInto`, and `publishOrder`
	/// against `advanceNotes`. **This is an INVARIANT AND NOT A GUARANTEE**: these stay public
	/// because the loader (rack_state.cpp) and the test extension still call them, and re-routing
	/// a LOADER through a queue is a different design question from re-routing an editor — it is
	/// filed as its own item rather than smuggled in here.
	/// The routes the reorder deleted, or `nullopt` for a move that could not be made — see
	/// `Graph::move`, whose return this is. Decided 2026-09-06: a reorder is never refused on account
	/// of a route; the route goes, and the count is how a caller says how much went.
	std::optional<size_t> moveDevice(size_t from, size_t to);

	/// **RESTORE A SAVED LANE — for the loader, and for nothing else.** `addDeviceAt` hands out the
	/// lowest free lane, which is correct for a new device and wrong for one being read back: its
	/// routes were saved against the lane it held. Named for its one caller so a future one has to
	/// think about why it wants this.

	/// THE RATE AND THE HOST'S LONGEST BLOCK (backlog GO), handed to every device it holds and to every device it
	/// builds from now on. While inactive.
	void prepare(double sampleRate, int maxFrames);

	/// **AS IF JUST SWITCHED ON — every device in the rack, audio and modulator alike.**
	///
	/// IT DID NOT EXIST, AND NOTHING ASKED UNTIL THE RACK BECAME A DEVICE. A standalone rack is never
	/// reset by anything but its own host's stop; a rack sitting in a chain is reset whenever that chain
	/// is, and `AudioOps::reset` is spelled out as the strong meaning — state goes, settings stay — so a
	/// rack that forwarded nothing would carry every member's history through its host's reset.
	///
	/// AUDIO THREAD, like `FxRunner::reset`. It walks the slots and calls each device's own reset, which
	/// allocates nothing; the descriptor is loaded ONCE per slot and held, the rule `processStereo`'s walk
	/// records paying for with a SIGSEGV.
	void reset() noexcept;

	/// A macro's position, 0..1.
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).**
	/// `RackBridge::drain()` runs at `process()` and `paramsFlush()`, which CLAP guarantees never
	/// overlap, so a write from there cannot land mid-block. A UI-thread call does, and TSan has
	/// named both families it produces: `setField` against `resolveInto`, and `publishOrder`
	/// against `advanceNotes`. **This is an INVARIANT AND NOT A GUARANTEE**: these stay public
	/// because the loader (rack_state.cpp) and the test extension still call them, and re-routing
	/// a LOADER through a queue is a different design question from re-routing an editor — it is
	/// filed as its own item rather than smuggled in here.
	void setMacro(size_t index, float value);
	/// The macro AS THE HAND HOLDS IT, 0..1 — what a patch saves and a knob writes back.
	float macro(size_t index) const;
	/// **WHICH WAY MACRO `index` IS READ (backlog EG).** Bipolar publishes `2v - 1` of the same 0..1
	/// value into the cell every route from it reads, so a route reaches both ways from the notch and
	/// the stored value — the DAW's parameter — never changes shape. Message thread, at the drain.
	void setMacroBipolar(size_t index, bool bipolar);
	[[nodiscard]] bool macroBipolar(size_t index) const;
	/// **EACH MACRO'S TYPE AND NAME, HELD FOR THE PATCH AND NOTHING ELSE** — for an engine loaded and
	/// saved with no bridge over it (`loadRack`/`saveRack`, a rack in a rack before its catalogue is
	/// adopted), and handed to the bridge that is built over it later. No block reads either: a type is how
	/// the HAND drives the value (`device/macro_type.h`), and the value is all the engine sees. MESSAGE
	/// THREAD ONLY — a name is a `std::string`, and it never crosses to the audio thread. Once a bridge
	/// exists it is the truth, as it is for the polarity; these are not written back.
	[[nodiscard]] device::MacroType macroType(size_t index) const;
	/// A type past the last is refused, as the bridge refuses it.
	void setMacroType(size_t index, device::MacroType type);
	/// The user's name, or "Macro N" when there is none (`device::defaultMacroName`).
	[[nodiscard]] std::string macroName(size_t index) const;
	[[nodiscard]] bool macroRenamed(size_t index) const;
	/// Clamped, and held empty when it is the default spelled out (`device::normalisedMacroName`).
	void setMacroName(size_t index, std::string_view name);

	/// WHETHER A NOTE IS BEING HELD, for the modulators.
	///
	/// THE GATE, BY HAND — for a host that drives it directly. **A RACK FED NOTE EVENTS DRIVES ITS OWN NOW**
	/// (2026-10-04): `updateNoteContext` sets it from the held stack in the piece that holds the event, so a
	/// note at offset 300 opens the gate at 300 and a note that starts and ends inside one host block still
	/// strikes. It was the wrapper's — a counter in `rack_clap.cpp`, applied at the top of the host block,
	/// which opened the gate up to a whole buffer early, dropped a short note entirely, and closed the gate
	/// on an off for a key nobody held. A host that sends no events (a test, a rack that is told its note)
	/// still sets it here, and nothing overrides it while no event arrives.
	///
	/// THE RETRIGGER EDGE IS DERIVED HERE for the first note: an edge that a caller had to remember
	/// to raise is an edge somebody forgets. Setting the gate from false to true IS a retrigger.
	void setGate(bool gate);

	/// **THE BUS NOTE — WHAT EVERY BUS DEVICE IS HANDED AS ITS `NoteContext`**.
	///
	/// THE DEFAULT is the collapsed note: the last key still held (`updateNoteContext`), kept after the release —
	/// or, for a rack told its note by its host, the host's (`setExternalNote`). **A TRACKER UPSTREAM REPLACES ITS
	/// PITCH** for every bus device after it in the chain, by the Tracker's own Rule and Glide
	/// (`NoteOps::busNote`); the nearest upstream one wins; a device before every Tracker keeps the default. The
	/// GATE stays the collapsed one — whether anything is held is a fact every device agrees on; which note is the
	/// Tracker's choice. "Upstream" is the chain's PATH order — where the card stands — not the graph's, which
	/// does not order a modulator against an audio device at all. Chains are walked in index order and a Tracker
	/// carries into the chains after its own, which is how Feathers' strip (chain 0, where its Trackers live)
	/// reaches its fx lane (chain 1). Per-voice instances are not handed this: a voice carries its own note.
	/// TEST ONLY: what the device at `slot` was handed in the last bus pass.
	[[nodiscard]] const device::NoteContext& busNoteForTest(size_t slot) const noexcept { return (*busNote_)[slot]; }
	/// The collapsed default, before any Tracker. TEST ONLY.
	[[nodiscard]] const device::NoteContext& noteContextForTest() const noexcept { return noteContext_; }
	/// HOW MANY MODULATION RINGS ARE IN USE (backlog LW step 4), and how many inputs found none free. TEST ONLY.
	[[nodiscard]] size_t modRingsInUseForTest() const noexcept {
		size_t n = 0;
		for (const ModRing& r : modRings_) {
			n += r.live ? 1 : 0;
		}
		return n;
	}
	[[nodiscard]] size_t modRingRefusalsForTest() const noexcept { return modRingRefusals_; }
	/// HOW MANY MODULATION RINGS AN ENGINE HOLDS — one per (source, late domain) a patch routes across a latency step.
	static constexpr size_t kModRings = 16;

	/// **HOW MANY SAMPLES UNTIL A LATE DEVICE'S NOTE CHANGES** (backlog LW): the soonest moment in the note history that
	/// a slot `A(n)` late has not reached yet, measured from now on the bus clock. A host cuts its next piece there, so a
	/// late device's note changes on its own sample — exactly `A(n)` after the collapsed one did. `UINT32_MAX` when
	/// nothing is late or nothing is pending. AUDIO THREAD; it records the current note first.
	[[nodiscard]] uint32_t samplesUntilLateNote() noexcept;
	/// THE SLOT'S ARRIVAL LATENCY `A(n)` as the last piece summed it, and its site's POST lane's. TEST ONLY.
	[[nodiscard]] uint32_t arrivalForTest(size_t slot) const noexcept { return slot < arrival_->capacity() ? (*arrival_)[slot].at : 0u; }
	[[nodiscard]] uint32_t arrivalPostForTest(size_t slot) const noexcept {
		return slot < arrival_->capacity() ? (*arrival_)[slot].post : 0u;
	}
	/// THE DELAY WORK DONE SO FAR (moments recorded, late notes handed, pieces cut) — 0 for an engine nothing latent ran in.
	[[nodiscard]] uint64_t lateWorkForTest() const noexcept { return lateWork_; }
	/// HOW MANY NOTE MOMENTS THE HISTORY KEEPS — fixed at `prepare`. Full, it makes room by `makeRoomInHistory`'s rule.
	static constexpr size_t kNoteHistory = 1024;
	/// WHAT A FULL HISTORY GAVE UP (TEST SEAM, and the honest count): changes merged under a held gate, and whole notes.
	[[nodiscard]] uint64_t lateMergesForTest() const noexcept { return lateMerges_; }
	[[nodiscard]] uint64_t lateNotesDroppedForTest() const noexcept { return lateNotesDropped_; }
	/// HOW MANY WRITES TO A LATE DEVICE'S ROW MAY WAIT AT ONCE — fixed at `prepare`.
	static constexpr size_t kPendingFields = 1024;
	/// TEST SEAM: how many writes wait now.
	[[nodiscard]] size_t pendingFieldsForTest() const noexcept { return pendingCount_; }
	/// CLAP'S ID FOR A TUNING EXPRESSION (`CLAP_NOTE_EXPRESSION_TUNING`, asserted equal in `rack_clap.cpp`,
	/// where CLAP is in reach): this layer does not include CLAP, as `mod/noteexpr` does not.
	static constexpr int32_t kTuningExpression = 2;

	/// **THE NOTE, FROM A HOST THAT HAS NO NOTE EVENTS TO GIVE** — a rack sitting in a chain as an
	/// effect. Its devices see this note (pitch AND gate), and its modulators this gate, exactly as they
	/// would from the stack the standalone rack derives. AUDIO THREAD, from `AudioOps::setNoteContext`.
	void setExternalNote(const device::NoteContext& note) noexcept;
	bool gate() const noexcept { return gate_; }

	/// A NOTE-ON, WHATEVER THE GATE IS DOING. The derived edge above is the whole of the retrigger
	/// only while every note-on raises the gate — and a note struck under a note already held does
	/// not, so a legato line never restarted anything and Single Trigger switched nothing. Reported
	/// 2026-09-05: retrig did not work if the envelope did not complete. MEASURED on the CLAP road before
	/// this existed: a routed control read 14.9487 before the second note and 14.9487 after it.
	///
	/// Consumed by the next tick and cleared there, like the edge, so it is one fact per note however
	/// many blocks pass before a tick. The gate is still the wrapper's to drive — this says a note
	/// ARRIVED, not that anything is held.
	void retrigger();

	/// **THE SLOT AT A LANE. A LANE, NOT A POSITION — AND THE NAME SAYS SO.**
	///
	/// This was `slotAt(size_t)` and it was indexed by POSITION in the chain. Under the graph, a
	/// device's storage lives at its LANE and never moves; where it sits in the chain is
	/// `graph().chain(c)`. The rename is deliberate and is the whole safety of this change: an
	/// unchanged signature whose meaning moved is the *"silent wrong-order bug rather than a crash"*
	/// `graph.h` warns the step-2 adopter about, and the compiler cannot see one. Renaming turns every
	/// call site into an error that someone reads.
	const Slot& slotAtLane(size_t lane) const;
	size_t slotCount() const noexcept { return capacity(); }

	/// **WHICH LANE HOLDS THE Nth DEVICE OF A CHAIN**, or `kNoSlot`. The other half of the rename: a
	/// caller that genuinely wants chain ORDER — drawing the rack left to right, or asking what comes
	/// after what — asks for it by name instead of assuming an array index means it.
	size_t deviceAtPosition(size_t position, size_t chain = 0) const noexcept;

	/// **THE INVERSE: WHERE IN THE CHAIN THE DEVICE AT `lane` SITS**, or `kNoSlot`. Both directions
	/// exist because both are asked — a card knows its lane and wants to know where it is being
	/// dropped; a layout knows the position and wants the device. Answered by the graph rather than by
	/// a table the caller keeps, which is what the editor's `slotOf` was and what it cost.
	size_t positionOfLane(size_t lane) const noexcept;

	/// How many devices are in a chain's path. Not `slotCount()`, which is the LANE capacity.
	size_t chainLength(size_t chain = 0) const noexcept;
	size_t chainCount() const noexcept { return graph().chainCount(); }

	static constexpr size_t kNoSlot = static_cast<size_t>(-1);

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD                                                                                 //
	//--------------------------------------------------------------------------------------------//

	/// Run the chain in slot order, in place.
	///
	/// RESOLVE THEN RUN, ONCE PER CONTROL TICK: the block is cut on the tick grid (`hostTickPhase_`) and each
	/// piece is a block of its own, so a row moves every 64 samples whatever the host's buffer, and a device
	/// that reads tick windows (`AudioOps::setFieldWindows`) follows its routes per sample inside the tick.
	void processStereo(float* left, float* right, int frames, const backplane::BlockContext& block);

	/// **A TRANSPORT THE HOST SENT INSIDE THE BLOCK** — a `CLAP_EVENT_TRANSPORT` at `at` frames in (backlog NJ (b)).
	/// A host with sample-accurate tempo or locate changes sends these mid-block; Feathers re-anchors its song clock
	/// at each one (`applyTransport`), and the rack dropped them, so its clock ran on from the block's start across a
	/// locate. Handed over before `processStereo`, which re-anchors every piece at or after `at` on it and forgets
	/// them at its end. In time order; at most `kMaxTransportAnchors` (later ones are dropped — a host sends one or two).
	struct TransportAnchor {
		int at = 0;
		backplane::BlockContext block {};
	};
	static constexpr size_t kMaxTransportAnchors = 16;
	void setTransportAnchors(const TransportAnchor* anchors, size_t count) noexcept {
		anchorCount_ = std::min(count, kMaxTransportAnchors);
		std::copy_n(anchors, anchorCount_, anchors_.begin());
	}

	//--------------------------------------------------------------------------------------------//
	// THE PER-VOICE ARRANGEMENT — the lane's, now this engine's.                                     //
	//--------------------------------------------------------------------------------------------//
	//
	// Feathers renders a device whose Poly switch is on INSIDE EACH VOICE, before the sum, in chain
	// order among the per-voice devices; every other device runs once on the summed bus. It did that on
	// a second runner. This engine does it itself, so Feathers' lane and the rack are one chain.
	//
	// A HOST WITH VOICES CALLS, PER BLOCK: `beginBlock` once; then, as often as it renders a piece of
	// the block, `processVoice` for each sounding voice and `processBus` on their sum; then `endBlock`
	// once. `processStereo` is all three over one piece, which is what a host without voices — the
	// standalone rack — has always called. **`beginBlock` and `endBlock` ARE A PAIR**: the block is in
	// flight between them (`blockEpoch_` odd), and a begin without its end would leave the epoch odd and
	// park every future removal.
	//
	// A "BLOCK" HERE IS WHATEVER THE HOST SETTLES BETWEEN TWO SETTINGS. Feathers re-decides its lane
	// mid-render, on the motion tick, when an envelope is routed — so its blocks are the segments
	// between those, and a device's arrangement is decided once per segment.

	/// **A HOST THAT OWNS ITS ROUTING HANDS THIS ENGINE THE ROUTES THEMSELVES, BY IDENTITY, AT A DRAIN.**
	/// The standalone rack's bridge and Feathers' lane alike: the record publishes a `RouteList`, the
	/// drain snapshots it whole and hands the snapshot here; this engine COPIES it (no allocation — grown
	/// beforehand by `reserveRoutes`) and rebuilds every node's inputs from it at the top of the next block. From then
	/// on this engine's own graph holds structure only, and its routes are whatever the host last handed
	/// over. `clearHostRoutes` gives the engine its own graph's routes back. DRAIN / AUDIO THREAD.
	///
	/// **THE HOST NODE'S OUTPUTS ARE THIS ENGINE'S MACRO CELLS, ALWAYS**. A host with core sources of its own — Feathers, with its macros and weights — pushes their
	/// values in through `setMacro`, one per source, after it publishes them and before anything reads
	/// them; a nested chain does the same with the parameters its host resolved (`rack_device.cpp`). This
	/// used to be a second road — a span of the host's cells that inputs pointed straight into — which
	/// saved twelve float copies per control tick and cost one more answer to "where does a rack's macro
	/// come from" than there is room for.
	void setHostRoutes(std::span<const backplane::Route> routes) noexcept;
	void setHostRoutes(const backplane::RouteRun& routes) noexcept;
	/// **ROOM FOR `routes` ROUTES — MESSAGE THREAD, ALLOCATES** (backlog OJ). The handed-over routes, the scratch the
	/// graph's own are gathered into, and every input grow IN PLACE (`ChunkedPool`), so the drain may be writing
	/// them meanwhile. A host calls it before it publishes a count that needs it (its list's growth hook). A list
	/// bigger than the room is REFUSED at `setHostRoutes` and counted (`routeRoomRefusals`), never truncated.
	void reserveRoutes(size_t routes);
	[[nodiscard]] size_t routeRoom() const noexcept { return std::min(hostRoutes_.capacity(), inputPool_.capacity()); }
	[[nodiscard]] size_t routeRoomRefusals() const noexcept { return routeRoomRefusals_.load(std::memory_order_relaxed); }
	void clearHostRoutes() noexcept;

	//--------------------------------------------------------------------------------------------//
	// HOST SLOTS — a device control driven by a host param (host slots plan §1.2 "Engine", T5)       //
	//--------------------------------------------------------------------------------------------//
	//
	// Host automation arrives on the AUDIO thread and an offline bounce has no main-thread cadence, so a
	// slot's value cannot wait for the record: the engine reads it straight from the `SlotValueBus` and
	// writes the bound control's BASE itself, every block, before anything resolves (`applyHostSlots`).
	//
	// THE BLOCK ORDER: drain (`Converger::converge` writes every row from the record, then hands this engine
	// the state's bindings — `setHostSlotBindings` — which resolves them and zeroes `lastGen`), then
	// `beginBlock`: routes rebuilt if dirty, then `applyHostSlots`, then notes, modulators, and every row
	// resolved (`resolveInto` → `resolveControl`, where `hostMod × span` joins the route offset).
	//
	// **THE BUS IS THE AUTHORITY FOR A BOUND CONTROL.** Every drain rewrites each row from the record, and
	// the zeroed `lastGen` makes the next `applyHostSlots` put the bus's value back over it — so a drain that
	// adopts a record older than a host write cannot revert it (plan §0.3). The other side of that bargain
	// is T6's: every write to a bound control that does NOT come from the host (a hand, a load, an undo)
	// must reach the bus too (`SlotValueBus::seed`), or the next drain puts the bus's older value back.
	// Seed BEFORE publishing the record, so no drain sees the new record with the old bus.
	//
	// **HOST MODULATION ON A CONTROL THAT IS NO MODULATION DESTINATION IS IGNORED.** Slots are declared
	// modulatable to the host (plan §0.1), but a control whose route span is negative (`Slot::routeSpan`) has
	// no span to scale a normalised offset by; its value road still works, its mod road is a no-op.
	//
	// **WHAT T6's RECORD MIRROR NEEDS FROM HERE: nothing new.** The value the engine applied is
	// `coerceControl(desc.params[control], bus.valueOf(slot))` and `fieldAt` reads it back; the mirror reads
	// the bus itself (value and generation) and commits that coerced number to the record.

	using SlotBus = hostslots::SlotValueBus<>;

	/// THE BUS THIS ENGINE READS ITS HOST SLOTS FROM, or null for none (slots off: bindings are kept and
	/// applied the moment a bus is set). MESSAGE THREAD with no block running (before activate), or a
	/// drain. Every bound control's host mod goes to zero and `lastGen` resets, so a new bus's values win.
	void setHostSlotBus(SlotBus* bus) noexcept;
	[[nodiscard]] SlotBus* hostSlotBus() const noexcept { return hostSlotBus_; }

	/// THE STATE'S BINDINGS, resolved from device id to this engine's slot. DRAIN ONLY (no block in flight):
	/// `Converger::converge` calls it after it has written every row. The previous bindings' host mod is
	/// zeroed, a binding whose device this engine does not run (or whose control it does not declare) is
	/// dropped until a later drain resolves it, and every `lastGen` is zeroed — see the banner.
	void setHostSlotBindings(std::span<const SlotBinding> bindings) noexcept;

	/// **THE BUS INTO THE BASES.** For every resolved binding whose bus generation moved since this engine
	/// last applied it, the control's base becomes the bus's value, coerced against its declaration
	/// (`device::coerceControl`, the rule `setField` applies); every binding's host mod is copied, moved or
	/// not. AUDIO THREAD, at the top of every `beginBlock`; allocates nothing. Public for a case.
	void applyHostSlots() noexcept;

	/// HOW MANY BINDINGS RESOLVED at the last `setHostSlotBindings` — for a case.
	[[nodiscard]] size_t hostSlotBindingCountForTest() const noexcept { return boundCount_; }

	/// **THE PER-VOICE CELLS OF ONE OUTPUT**, `voices()` floats from here, or null: a device this engine
	/// does not run, an output it does not declare or does not declare `perVoice`, a rack with no voices.
	/// For a host building its own node's per-voice inputs. Valid as `outputCellOf`'s is.
	[[nodiscard]] const float* outputCellForVoiceOf(DeviceInstanceId id, size_t output) const noexcept;
	/// The per-voice windows of `output`, `voices * kTickWindowStride` floats, or nullptr.
	[[nodiscard]] const float* outputWindowForVoiceOf(DeviceInstanceId id, size_t output) const noexcept;
	/// A block-rate output's tick window (`Slot::outputsWindow`), or nullptr.
	[[nodiscard]] const float* outputWindowOf(DeviceInstanceId id, size_t output) const noexcept {
		if (outputCellOf(id, output) == nullptr) return nullptr;
		return &slots_[slotOf(id)].outputsWindow[output * static_cast<size_t>(device::kTickWindowStride)];
	}
	/// **THE RIGHT EAR OF A BLOCK-RATE OUTPUT** (backlog KU, 2026-10-05) — its cell (`Slot::outputsR`) and its tick window
	/// (`Slot::outputsWindowR`), or nullptr as `outputCellOf` is. A mono output's right ear holds its left.
	[[nodiscard]] const float* outputCellROf(DeviceInstanceId id, size_t output) const noexcept {
		if (outputCellOf(id, output) == nullptr) return nullptr;
		return &slots_[slotOf(id)].outputsR[output];
	}
	[[nodiscard]] const float* outputWindowROf(DeviceInstanceId id, size_t output) const noexcept {
		if (outputCellOf(id, output) == nullptr) return nullptr;
		return &slots_[slotOf(id)].outputsWindowR[output * static_cast<size_t>(device::kTickWindowStride)];
	}
	/// **WHETHER AN OUTPUT SPEAKS TWO EARS NOW** — the word `Slot::emitsPair` (bit `output`), as the stereo-live fixed
	/// point last left it; nullptr as `outputCellOf` is. A pointer rather than an answer so a host node built at one drain
	/// reads the CURRENT pair state at every block. Audio thread only, like the fixed point that writes it.
	[[nodiscard]] const uint64_t* outputPairsOf(DeviceInstanceId id, size_t output) const noexcept {
		if (outputCellOf(id, output) == nullptr || output >= device::kMaxDeviceOutputs) return nullptr;
		return &slots_[slotOf(id)].emitsPair;
	}
	/// **SETTLE THE ROUTES NOW** — rebuild the inputs and the stereo-live fixed point if they are stale, so a host node
	/// about to build its own inputs reads this drain's pairs, not the last one's. AUDIO THREAD.
	void settleInputs() noexcept {
		if (routesDirty_) {
			rebuildInputs();
		}
	}
	/// **THE HOST WRITES A PER-VOICE READING** — it is what knows its voices and the note each is holding
	/// (`ModOps::outputForNote` asks by note id). AUDIO THREAD, after the tick.
	void writeOutputForVoice(DeviceInstanceId id, size_t output, size_t voice, float value) noexcept;
	/// **A VOICE STARTS A NOTE: ITS READING IS WHERE THE NEW NOTE STARTS, FROM THIS SAMPLE** (backlog FF). The cell and
	/// a flat window at `value`, not a ramp from the note before — a steal or a retrigger read the old note's value until
	/// the next tick. AUDIO THREAD, when the voice is handed its note.
	void startOutputForVoice(DeviceInstanceId id, size_t output, size_t voice, float value) noexcept;
	/// **AND ITS RIGHT EAR, WHILE THE OUTPUT IS A PAIR** (backlog PA) — `ModOps::outputPairForNote` for the note, into
	/// `Slot::outputsPerVoiceR` and its window, by the left ear's rules (the amount, the switch, `start`). Nothing at all
	/// for an output that is not a pair this block. Call it beside `writeOutputForVoice`/`startOutputForVoice`.
	void writeOutputPairForVoice(DeviceInstanceId id, size_t output, size_t voice, int32_t noteId, int16_t channel,
	                             int16_t key, bool start) noexcept;
	/// The right ear's per-voice tick windows of an output — `outputWindowForVoiceOf`'s twin, into
	/// `Slot::outputsPerVoiceWindowR`. Current only while the output is a pair (`outputPairsOf`), so read it only then.
	[[nodiscard]] const float* outputWindowRForVoiceOf(DeviceInstanceId id, size_t output) const noexcept;
	/// WHERE THE NEXT `processBus` STARTS INSIDE THE CONTROL TICK, 0..kTickWindow-1, or -1 when the host
	/// does not render on the tick grid (the standalone rack): a device is then handed no windows at all.
	void setTickPhase(int phase) noexcept { tickPhase_ = phase; }
	/// **WHAT TIME IT IS AT THE START OF THE NEXT `processBus` / `processVoice`** — for a host that cuts a
	/// segment into chunks between two `beginBlock`s (Feathers renders its lane in ≤32-frame chunks). The
	/// standalone rack's own host path advances `piece.clock` per piece; a host that calls `processBus`
	/// directly has to say so itself, or every chunk of the segment is told the segment's first sample.
	/// MEASURED (the 2026-09-24 SKLITTER crush): sixteen 32-frame calls in a 512-frame block all read one
	/// song position, SKLITTER seated its write head on it each call, and 480 of every 512 ring frames were
	/// never written — stale audio that survived silent recaptures and clicked at every chunk boundary.
	void setClock(const device::TransportClock& clock) noexcept { blockContext_.clock = clock; }
	/// THE CLOCK THE LAST BLOCK (or piece) RAN AGAINST — what `RackBridge::publish` mirrors the tempo from.
	[[nodiscard]] const device::TransportClock& clock() const noexcept { return blockContext_.clock; }
	/// `clock` moved on by `seconds` of rendered audio — the song position on both timelines, each only
	/// where the clock has one. The hosts that cut a call into pieces use this for each piece's clock.
	[[nodiscard]] static device::TransportClock advanced(const device::TransportClock& clock, double seconds) noexcept {
		device::TransportClock c = clock;
		if (clock.havePositionSeconds()) {
			c.songPositionSeconds = clock.songPositionSeconds + seconds;
		}
		if (clock.havePositionBeats() && clock.haveTempo()) {
			c.songPositionBeats = clock.songPositionBeats + seconds / clock.secondsPerBeat;
		}
		return c;
	}

	/// **WHERE A DEVICE'S OUTPUT CELL IS** — for a host building ITS OWN node's inputs (Feathers' synth
	/// parameters are the core node's controls, and their inputs point here). Null for a device this
	/// engine does not run or an output it does not declare. Valid until the next drain that moves a
	/// device, exactly as this engine's own inputs are. MESSAGE THREAD AT A DRAIN.
	[[nodiscard]] const float* outputCellOf(DeviceInstanceId id, size_t output) const noexcept;
	/// **HOW MANY INPUTS THE CONTROL AT `(lane, control)` HAS THIS BLOCK** — the pull machinery's own
	/// observable, for a case that asks whether a route reached its node rather than reading a value that
	/// might have got there some other way.
	[[nodiscard]] size_t inputCountForTest(size_t lane, size_t control) const noexcept;
	/// WHETHER A STEREO ROUTE REACHES THIS CONTROL as of the last drain (backlog KU). Test-only.
	[[nodiscard]] bool stereoLiveForTest(size_t lane, size_t control) const noexcept {
		return lane < slots_.size() && slots_[lane].stereoLive.test(control);
	}
	/// WHETHER THIS OUTPUT IS AN EAR PAIR as of the last drain (backlog KU). Test-only.
	[[nodiscard]] bool emitsPairForTest(size_t lane, size_t output) const noexcept {
		return lane < slots_.size() && output < 8 && ((slots_[lane].emitsPair >> output) & 1u) != 0;
	}

	/// HOW MANY VOICES THIS RACK RUNS PER-VOICE DEVICES FOR. MESSAGE THREAD, before devices are added —
	/// a device already in the rack keeps the instances it was built with. Zero, the default, is a rack
	/// with no voices, where a Poly device runs on the bus.
	void setVoices(size_t voices) noexcept;
	/// How many voices this rack runs per-voice devices and per-voice output cells for. See `setVoices`.
	[[nodiscard]] size_t voiceCount() const noexcept { return voices_; }

	/// **WHICH NOTE VOICE `voice` HOLDS, OR NONE** (backlog MV, B) — told by the host, which is what knows its voices
	/// (the engine's per-voice cells are by voice, a Poly modulator's shapes by note). It is what lets a modulator's
	/// control be resolved FOR A NOTE (`device::ModTickContext::perNote`): a per-voice source routed into a Poly
	/// envelope's Trig fires that note's envelope alone. AUDIO THREAD, before `tickModulators`; allocates nothing. A
	/// voice past `voiceCount()` is ignored.
	void setVoiceNote(size_t voice, int32_t noteId, int16_t channel, int16_t key) noexcept;
	void clearVoiceNote(size_t voice) noexcept;

	/// **WHICH INNER CHAIN THIS ENGINE IS, FOR THE SPIKE RECORDER'S PATHS (backlog SR)**: the name its holder gives it
	/// ("post", "grain", "pre", "chain") and, for a container's numbered chains, which one (`-1`: none). A device it runs
	/// is then named `SKLT/post/DELA`. Each device's bus and voice passes are timed into the thread's `CostTree` while
	/// one is bound (`rack::bindCostTree`; the recorder binds it for its block) and cost one branch while none is. `name`
	/// must outlive the engine (a literal). Set by the holder as it builds the chain, or by the AUDIO THREAD as it runs it.
	void setCostLane(const char* name, int index = -1) noexcept {
		costLane_ = name;
		costLaneIndex_ = index;
	}

	/// THE BLOCK BEGINS: notes, then modulators, then every audio device's row resolved and each
	/// device's arrangement for this block decided. AUDIO THREAD.
	void beginBlock(int frames, const backplane::BlockContext& block);

	/// ONE VOICE'S PASS: every device whose Poly is on, its instance for `voice`, in chain order, told
	/// that voice's note. `right` may be null for a mono voice; a device with no mono road is skipped
	/// rather than handed a null channel. AUDIO THREAD, between `beginBlock` and `endBlock`.
	void processVoice(size_t voice, float* left, float* right, int frames,
	                  const device::NoteContext& note);

	/// THE BUS PASS: every device NOT running per voice, on the summed signal, in chain order, with the
	/// clock `beginBlock` was handed. AUDIO THREAD, between `beginBlock` and `endBlock`.
	void processBus(float* left, float* right, int frames);

	/// THE BLOCK ENDS. Anything the drain parked while it ran becomes reapable. AUDIO THREAD.
	void endBlock() noexcept;

	/// WHETHER ANY DEVICE RUNS PER VOICE THIS BLOCK — so a host with nothing per-voice skips the voice
	/// pass's walk, as `FxRunner::anyActive` let it.
	[[nodiscard]] bool anyPerVoice() const noexcept { return anyPerVoice_; }
	/// **HOW LATE THE PER-VOICE ROAD IS THIS BLOCK**: the latency of every latent device running per voice (`Traits::latentPerVoice`,
	/// admitted only with `setAllowLatentPerVoice`). 0 otherwise, which is every patch without one. Set by `beginBlock`.
	[[nodiscard]] uint32_t perVoiceLatencySamples() const noexcept { return perVoiceLatency_; }
	/// **THE SIDECHAIN FOR THE FRAMES THE NEXT CALL RUNS** (design §10.1): `processStereo` reads it from its start and offsets it per
	/// piece; a host that calls `processBus` itself (Feathers' Synth, chunk by chunk) sets it before each call. Null is silence.
	/// Handed to every bus device that declares `AudioOps::setSidechain`. AUDIO THREAD; the pointers live for the call.
	void setSidechain(const float* left, const float* right, bool sounding) noexcept {
		sideL_ = left;
		sideR_ = right;
		sideSounding_ = sounding;
	}
	/// HOW MUCH PER-VOICE LATENCY THIS HOST COMPENSATES, IN ALL (it delays each voice's gain and free by perVoiceLatencySamples, in
	/// a ring of that size): 0, the default, admits no latent device per voice. Feathers' Synth sets it at activate; an inner chain
	/// leaves it 0 (its per-voice blend is uncompensated, L7). A latent device that would take the sum past it runs on the bus.
	/// MESSAGE THREAD, before audio.
	void setLatentPerVoiceCap(uint32_t samples) noexcept { latentPerVoiceCap_ = samples; }
	/// **WHETHER A PER-VOICE DEVICE HAS A STEREO-LIVE CONTROL THIS BLOCK** (backlog KU) — so a host that would
	/// render its voices in mono knows a voice's right ear has somewhere it must go. AUDIO THREAD.
	[[nodiscard]] bool anyPerVoiceStereo() const noexcept { return anyPerVoiceStereo_; }

	/// **HOW LATE THIS ENGINE'S OUTPUT IS, IN SAMPLES AT `sampleRate`** — backlog JM. The SERIAL sum of
	/// every audio device's `AudioOps::latencySamples` over the published order, each asked with its stored
	/// row: an engine runs its chains one after another on one buffer, so there is no branch inside it to
	/// align (assumption L9). A latent device is never per voice (`beginBlock`, L2), so the whole of this is
	/// on the bus. AUDIO THREAD, or while nothing runs the engine; allocates nothing.
	[[nodiscard]] uint32_t latencySamples(double sampleRate) const noexcept;

	/// **WHETHER ANY DEVICE ON THIS ENGINE'S BUS STILL HOLDS SOUND TO COME** (backlog MC) — `AudioOps::holdsTail` over
	/// the published order, asked of each bus instance the bus RUNS: a slot running per voice is skipped, because its
	/// bus instance is not processed and a line frozen there would answer true for ever. AUDIO THREAD, after a block,
	/// or while nothing runs the engine; allocates nothing.
	[[nodiscard]] bool holdsTail() const noexcept;
	/// **WHETHER EVERY DEVICE ON THIS ENGINE'S BUS IS SWITCHED OFF AND PARKED** (2026-11-05: a disabled device in the
	/// Delay's feedback lane changed the loop) — so what the bus hands back is what it was handed, delayed at most by the
	/// latency the parked devices keep for alignment. False for an empty bus, for anything the engine does not switch
	/// itself (a container), and for a device whose enable is on in its row or as resolved, so switching one back on is
	/// seen before the bus has run again (it is parked only by running). AUDIO THREAD, after the drain; allocates nothing.
	[[nodiscard]] bool allParked() const noexcept;

	/// A VOICE STARTS A NEW NOTE: its instances forget everything, as `FxRunner::resetInstance` did —
	/// a filter must not open on the last note's ringing. AUDIO THREAD.
	void resetVoice(size_t voice) noexcept;

	/// TEST ONLY — whether the slot at `lane` ran per voice in the last block.
	[[nodiscard]] bool polyForTest(size_t lane) const noexcept {
		return lane < slots_.size() && slots_[lane].polyThisBlock;
	}

	//--------------------------------------------------------------------------------------------//
	// THE POOL, BY LANE — what `RackBridge` mirrors and a case reads. It was this engine's half of   //
	// `backplane::DeviceHost` until the seam went by id (kill-lanes step 2); the lanes go in step 3.  //
	//--------------------------------------------------------------------------------------------//

	const backplane::Axes& axes() const noexcept { return axes_; }
	/// THIS RACK'S OWN RULE ABOUT ITS OWN AXES — none: its host sources are macros and it has no host
	/// destinations, so there is no pair to have a rule about.
	backplane::CorePolicy corePolicy() const noexcept { return &backplane::permitAll; }
	/// WHETHER `source` MAY REACH `destination`, at this pool's coordinates — the routing rule asked of the
	/// graph by the devices at those lanes (`backplane::mayRoute`). For the loader and the cases that still
	/// speak coordinates; the editor asks `RackBridge` by endpoint.
	/// BY ENDPOINT — the one vocabulary every host's routing speaks (kill-lanes step 2; the coordinate
	/// overload went with the coordinates, §2.4b).
	bool mayRoute(backplane::Source s, backplane::Destination d) const noexcept {
		return backplane::mayRoute(axes_, devices(), s, d, corePolicy());
	}
	/// THE GRAPH'S VIEW OF THIS ENGINE'S DEVICES, PLUS THE HOST OUTSIDE (rack roadmap §7): the view the
	/// gate asks, with `outerUpstream` answered through the reader this engine was handed, if any.
	backplane::DeviceView devices() const noexcept;

	//--------------------------------------------------------------------------------------------//
	// THE HOST OUTSIDE — rack roadmap §7. This engine as a chain device's inner rack.                //
	//--------------------------------------------------------------------------------------------//

	/// TAKE THE READER the outer host hands this chain at seating. An inner route whose source is
	/// `Source::isOuter()` resolves through it; the gate asks it whether that source is upstream.
	void setOuterSources(const device::OuterSources& outer) noexcept;
	[[nodiscard]] bool hasOuterSources() const noexcept { return outer_.cell != nullptr; }
	/// THE CELL A SOURCE OF THIS ENGINE WRITES — a macro (or the host's core cell), a device output,
	/// or an outer source through the reader. Null for one this engine cannot read. What `rebuildInputs`
	/// points an input at, and what this engine hands DOWN to a chain it seats.
	[[nodiscard]] const float* sourceCell(backplane::Source s) const noexcept;
	/// **THIS ENGINE AS THE HOST OUTSIDE, FOR A CHAIN AT `self`** (rack roadmap §7): the reader a container seated here,
	/// or a site's lanes (backlog NZ), resolve outer sources through. Looks cells up on every call.
	[[nodiscard]] device::OuterSources outerSourcesFor(DeviceInstanceId self) const noexcept;
	/// THE PER-VOICE RUN A SOURCE OF THIS ENGINE WRITES — `[voices]` entries for a device output declared
	/// `perVoice`; null for the host's own sources, for an output with one value, and for an outer source
	/// (two levels out is not a road). What this engine hands DOWN to a chain's voice instances.
	[[nodiscard]] const float* sourcePerVoiceRun(backplane::Source s) const noexcept;
	/// WHETHER `s` (one of this engine's own sources) IS UPSTREAM OF THE DEVICE `self` — the ordering
	/// half of `mayRoute` with `self` as the destination; a core source always is. What this engine
	/// answers for a chain it seats.
	[[nodiscard]] bool upstreamOf(DeviceInstanceId self, backplane::Source s) const noexcept;
	backplane::SlotView slots() const noexcept;
	// `rankAtLane` stood here, exposing the graph's rank "so the bridge can mirror it beside the
	// descriptor it already mirrors". No bridge mirrors a rank any more — the routing rule is
	// `wouldCycle` above — so it went in kill-lanes step 5. `Graph::rankAtLane` is still there for
	// anyone who wants the run order itself.

	float fieldAt(size_t slot, size_t control) const;
	/// **ONLY THE DRAIN MAY CALL THIS — a direct call races a block (AS commit 2).** It is the write
	/// half of the pair ThreadSanitizer named: this stores `slot.base[control]` as a plain float
	/// while `resolveInto` reads it on the audio thread. See the note at `addDevice` for why these
	/// stay public and what would make the invariant a compile-time guarantee instead.
	void setField(size_t slot, size_t control, float value);
	size_t fieldsPresentAt(size_t slot) const;

	/// WHERE MODULATION LEFT THIS CONTROL THIS BLOCK — see `DeviceHost::modulatedFieldAt`. The rack
	/// resolves every control every block, so it has the number; this publishes it.
	float modulatedFieldAt(size_t slot, size_t control) const;
	/// `modulatedFieldAt` for controls `0 .. count`, into `out` — the same loads, the slot found once (the idle-overhead
	/// pass, 2026-11-05: the synth read its strip's rows a control at a time, every tick).
	void modulatedRowAt(size_t slot, float* out, size_t count) const;
	/// **THE EDITOR'S RIGHT EAR** (backlog KU): true, with the right ear's resolved value, while a stereo route
	/// reaches this control; false otherwise (draw the mono ring). Read relaxed, a frame late at worst as the left is.
	bool modulatedFieldRAt(size_t slot, size_t control, float& right) const;
	/// **WHAT THIS ENGINE READS AROUND ONE ROUTE** (backlog PU) — the source's collapsed reading through the cell an
	/// input from it would point at, every OTHER input into the control summed in the order `resolveEar` sums them,
	/// the host's modulation and the span. False for a device this engine does not run, a control that is no
	/// destination, or a source it cannot read. AUDIO THREAD: the inputs are rebuilt at the drain.
	bool probeRoute(DeviceInstanceId device, size_t control, backplane::Source source,
	                backplane::RouteProbe& out) const noexcept;
	/// **ANSWER THE EDITOR'S WATCHED PAIR, IF ANY** — `probeRoute`, published into `channel`. AUDIO THREAD.
	void answerRouteProbe(backplane::RouteProbeChannel& channel) const noexcept;

	/// WHAT `prepare` WAS TOLD, or 0 before it has been called. The rack already held this to give
	/// each device its own `prepare`; the seam simply lets a surface ask the same question.
	double sampleRate() const noexcept { return sampleRate_; }
	/// WHAT `prepare` WAS TOLD THE LONGEST BLOCK IS — `device::kDefaultMaxFrames` before it has been called.
	int maxFrames() const noexcept { return maxFrames_; }
	/// TEST ONLY — how many times `prepare` has run: the moment every device's state is rebuilt, so a case can assert a
	/// reactivation re-prepared NOTHING (backlog GO's grow-only rule) as a count rather than infer it from audio.
	uint64_t preparesForTest() const noexcept { return prepares_; }

	/// THE MOST READINGS ONE DEVICE MAY PUBLISH. Sixteen covered OWLTT's nine meters and its clipping
	/// flag; a device wanting more is a conversation, not a silent truncation — `publishReadings`
	/// clamps and the count it reports is what was actually stored.
	///
	/// **THE CONVERSATION ARRIVED AS A TRUNCATION ANYWAY (2026-09-15).** SKLITTER publishes a PICTURE —
	/// a header plus two floats for each of sixteen steps, 37 in all — and at sixteen the rack stored
	/// the header and five and a half steps. The card would have drawn five chunks of a sixteen-step
	/// bar and been entirely plausible about it. The other half of the same defect showed first:
	/// the strip was not visible.
	///
	/// SIXTY-FOUR rather than 37, and asserted rather than trusted: the clamp is silent by
	/// construction, so the guard has to be a compile error and not a runtime check nobody reads.
	/// **160 SINCE BACKLOG HR ROUND 2** (2026-09-23) — SKLITTER's spark events took it to 145. Kept equal
	/// to `GuiBridge::kMaxDeviceReadings`, which carries the same publish on the other road.
	/// **224 SINCE BACKLOG IX, the same day** — a step is three floats (the ratchet's piece length), so
	/// SKLITTER's picture is 18 + 3 × 64 = 210.
	static constexpr size_t kMaxReadings = 224;
	// **THE ASSERT THAT CHECKS A DEVICE AGAINST THIS BOUND LIVES IN `src/plugin/device_registry.h`**,
	// beside `widestDeviceControls` and `widestDeviceOutputs`, which are the same kind of claim: "this
	// BUILD's devices fit this host's limits". It cannot live here. The rack must not include a device,
	// because a device may HOLD a rack — SKLITTER is about to be the first — and `rack_engine.h`
	// including `fx/sklitter/sklitter_types.h` made that a cycle at the header as well as at the
	// library. The check itself is unchanged and still a compile error, which is the property that
	// matters: the clamp it guards is silent by construction.

	/// TEST ONLY — WHAT A DEVICE HAS ACTUALLY PUBLISHED INTO THIS ENGINE. A picture is not recoverable
	/// from the audio that produced it, and a clamped channel truncates SILENTLY: the count it reports
	/// is what was stored, so a short picture and a complete one are indistinguishable downstream. The
	/// only way to see it is to read the numbers here.
	[[nodiscard]] size_t readingCountForTest(size_t slot) const noexcept {
		return slot < readings_.size() ? readings_[slot].count : 0;
	}
	[[nodiscard]] float readingForTest(size_t slot, size_t index) const noexcept {
		if (slot >= readings_.size() || index >= readings_[slot].value.size()) {
			return 0.0f;
		}
		return readings_[slot].value[index];
	}

	/// PUBLISH FROM THE AUDIO THREAD. Called by whatever sink a host installed on the device.
	///
	/// NO ALLOCATION AND NO LOCK: the store is sized at construction and each cell is written through
	/// `std::atomic_ref` — relaxed, because every reading is an independent scalar and no reader draws
	/// a conclusion from two of them together. Same discipline as `RouteTable`, and for the same
	/// reason: the editor reads these while the audio thread writes them, and a plain float would be a
	/// data race the compiler is free to make real.
	void publishReadings(size_t slot, const float* values, size_t count) noexcept;

	/// THE MOST NOTES ONE BLOCK MAY CARRY, in or out.
	///
	/// FIXED BECAUSE THE AUDIO THREAD MAY NOT ALLOCATE, and generous because the failure is a dropped
	/// note rather than a wrong number: 256 is far past any hand and past most arps. `NoteOps`
	/// records the rule a device must follow when it fills the buffer — drop LATER notes, never
	/// earlier ones, because a dropped note-off is a stuck note.
	static constexpr size_t kMaxNotes = 256;

	/// **THE HELD-NOTE STACK, AND THE CONTEXT DERIVED FROM IT.**
	///
	/// A pitched audio device — HARM is the one that found this — needs to know WHAT NOTE it is acting
	/// on. Feathers' `FxRunner` delivers that per voice through `AudioOps::setNoteContext`; **this
	/// engine never called it at all**, so HARM's `settings.gate` stayed false, its envelope never
	/// opened, and silence came from an enabled device with its level up.
	///
	/// **THE COLLAPSED NOTE'S STACK — THE TRACKER'S OWN** (`mod::tracker::HeldKeys`), picked by Last: the note a
	/// hand just played is the one it means. Shared code, so the default and a Tracker set to Last cannot disagree.
	mod::tracker::HeldKeys heldKeys_;
	/// EACH KEY'S TUNING EXPRESSION, in semitones — see `updateNoteContext` and `kTuningExpression`.
	std::array<float, 128> keyTuning_ {};
	device::NoteContext noteContext_ {};

	/// **A NOTE HANDED TO THE RACK BY ITS HOST**, for a rack that is a device in a chain. A standalone
	/// rack is fed note EVENTS and derives its note from the held stack; a rack in Feathers' lane is
	/// handed a note CONTEXT — pitch and gate — which is the road every effect in a chain gets. Once a
	/// host has used that road, it is the note this rack's devices see. One engine, fed by whichever
	/// road its host has. Audio thread, like the stack it stands in for.
	device::NoteContext externalNote_ {};
	bool externalNoteSet_ = false;
	/// The reader from the host outside, if this engine is a chain's — see `setOuterSources`.
	device::OuterSources outer_ {};

	/// Fold this block's note events into the held stack and update `noteContext_`.
	void updateNoteContext();

	/// HAND THE BLOCK'S INPUT NOTES TO THE ENGINE, before `processStereo`.
	///
	/// SEPARATE FROM `setGate` RATHER THAN REPLACING IT. The gate is a derived convenience for
	/// modulators that do not want to parse a stream — an envelope wants "is anything held". A gate is
	/// a summary every device would compute identically; a pitch is one they would not, which is why
	/// the stream is offered whole and the pitch is nobody's business but the device's.
	void setBlockNotes(const device::NoteEvent* notes, size_t count) noexcept;

	/// WHAT LEFT THE END OF THE CHAIN — valid until the next `processStereo`.
	///
	/// **PASS-THROUGH MEANS THIS IS NEVER EMPTY WHEN THE INPUT WAS NOT.** A rack holding one filter emits exactly what it was
	/// given, which is ordinary MIDI-thru and is what makes the output port honest at all times rather
	/// than flickering with the chain's contents.
	std::span<const device::NoteEvent> blockNotesOut() const noexcept;

	/// **THE NOTE PASS, NOW, FOR ONE ARRIVAL** — for a host that runs its notes the moment they arrive
	/// rather than once per block. Feathers does: its render cuts the block at every event, so the
	/// instant a note reaches the lane IS its sample, and quantising it to a block would delay every
	/// note (measured once, and reverted: a one-tick delay across fifteen timing cases). The same walk
	/// `beginBlock` runs — every note device in chain order, settings first, counts clamped — over
	/// `in`, with what left the chain copied to `out` up to `capacity`. The stream is emptied after,
	/// so the next block's pass does not play these notes again. AUDIO THREAD. Returns what was copied.
	size_t processNotesNow(const device::NoteEvent* in, size_t count, device::NoteEvent* out,
	                       size_t capacity) noexcept;

	/// **THE CLOCK PASS — WHAT SELF-CLOCKED DEVICES EMIT AT THIS SAMPLE** (`NoteOps::emitNotes`). A host
	/// calls it at the start of every render chunk. Walks the chain in the order
	/// `advanceNotes` does, starting from an EMPTY stream: an emitter adds its events, and every note device after it
	/// receives them through `processNotes`. A device after nothing that emitted is not asked, so a chain with no
	/// emitter does no work and returns 0 with `*samplesUntilNext` at `UINT32_MAX`, which leaves a pass-through's
	/// chunking exactly as it was. `*samplesUntilNext` is the soonest any emitter wants its next call. AUDIO THREAD.
	/// It uses the stream `setBlockNotes` fills, so a host that hands notes per block calls it before that or after
	/// the block's pass, never between them.
	size_t emitNotesNow(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
	                    size_t capacity, uint32_t* samplesUntilNext) noexcept;
	/// HOW MANY SELF-CLOCKED DEVICES THE LAST `emitNotesNow` FOUND IN THE CHAIN — a holder ending the notes they invented
	/// when the last one leaves (a Rack's chain, backlog RN). AUDIO THREAD.
	[[nodiscard]] size_t emittersSeen() const noexcept { return emittersSeen_; }
	/// **WHETHER A SELF-CLOCKED DEVICE HAS AN EVENT STILL TO COME WITH NO NEW INPUT** (the SLEEP rule): the last
	/// `emitNotesNow` found an emitter that named a next call, so an arp holding a chord, or a pattern running on the
	/// transport, will strike again after a silent gap; and one ARMED on a stopped transport (Run Transport over a chord,
	/// Trig Transport with none) names one too, because the transport starting is no input event a sleeping plugin is
	/// woken by. A host told SLEEP there may stop calling, and the step never sounds. False once no emitter could play
	/// without a new event (no key under Gate, switched off, none in the chain), so SLEEP is still reached. AUDIO THREAD.
	[[nodiscard]] bool notesScheduled() const noexcept { return notesScheduled_; }
	/// WHETHER ANY DEVICE IN THE PUBLISHED ORDER TAKES NOTES (`NoteOps`) — a chain that holds none hands its notes on
	/// untouched. Any thread that may read the order; allocates nothing.
	[[nodiscard]] bool holdsNoteDevice() const noexcept;

	/// **END EVERY INVENTED NOTE AT THE NEXT BLOCK'S FIRST SAMPLE** — for a host that has just re-activated: an activate
	/// resets the arp (it forgets what it sounded), while the instrument this rack plays may still hold it. `processStereo`
	/// does the same on its own when an emitter leaves the chain mid-note. Pass-through notes are the host's to end.
	void flushInventedNotes() noexcept { flushInvented_ = true; }
	/// TEST SEAM: how many invented notes are sounding downstream.
	[[nodiscard]] size_t inventedHeldForTest() const noexcept;

	/// **THE MODULATORS ADVANCE `frames` SAMPLES ON THE HOST'S OWN GRID** — Feathers ticks its strip on
	/// its control tick, between the settings and the publish, not at the top of a block. The same
	/// pass `beginBlock` runs: each modulator's row resolved, then ticked as many times as ITS OWN
	/// declared `blockSamples` fits into the samples owed (2026-09-11: the declared rate — a 32-
	/// sample device ticks twice in a 64-sample control tick). AUDIO THREAD.
	void tickModulators(int frames, const backplane::BlockContext& block);

	/// WHAT A MODULATOR'S OUTPUT READS, UNGATED — the raw value, before a host decides what a switched-off
	/// device publishes. Zero for a lane holding no modulator or an output it does not declare.
	[[nodiscard]] float outputAt(size_t lane, size_t output) const noexcept;
	/// **ONE VOICE'S VALUE OF A PER-VOICE OUTPUT** — `ModOps::outputForNote`, and the ordinary output for
	/// a device that does not vary per voice (null is the capability). Zero for an empty lane.
	[[nodiscard]] float outputForNoteAt(size_t lane, size_t output, int32_t noteId, int16_t channel,
	                                    int16_t key) const noexcept;
	/// **WHAT A ROUTE FROM `(lane, output)` RESOLVES AGAINST** — the node's published cell, the disable rule
	/// applied (kill-lanes §2.4b). `outputAt` above is the device's own ungated reading; this is the one
	/// every input pointing at the node reads, and the one `sourceValue` used to answer.
	[[nodiscard]] float publishedOutputAt(size_t lane, size_t output) const noexcept;
	/// THE RIGHT EAR OF `publishedOutputAt` (backlog KU) — equal to it for every mono output.
	[[nodiscard]] float publishedOutputRAt(size_t lane, size_t output) const noexcept;

	size_t readingCountAt(size_t slot) const noexcept;
	float readingAt(size_t slot, size_t index) const noexcept;

	/// WHERE A MODULATOR IN A SLOT IS IN ITS SHAPE — the rack's answer to `DeviceHost::playheadAt`.
	///
	/// **THE RACK USED TO DECLINE THIS AND THE DECLINE WAS HONEST BUT INCOMPLETE.** Nothing published
	/// a playhead, so the cards drew no star — deliberately, since a marker frozen at ordinal zero
	/// looks exactly like a stopped modulator. The star was asked for, so the road now exists rather
	/// than the decline being dressed up: `ModOps::playhead` is the device's own accessor and this
	/// asks it. A device that does not offer one still declines, and still draws nothing.
	bool playheadAt(size_t slot, uint32_t& ordinal, float& phase) const;
	/// And the level beside it, for a device that publishes one (`ModOps::playheadLevel`); false otherwise, with
	/// `level` untouched.
	bool playheadLevelAt(size_t slot, float& level) const;
	/// How many times the Trig of the modulator in `slot` has fired (`ModOps::trigFires`); false for a device with none.
	bool trigFiresAt(size_t slot, uint32_t& fires) const;
	/// The newest events the modulator in `slot` made, newest first (`ModOps::recentEvents`), read where its publish is
	/// (the newest note's voice for a device publishing from its voices); 0 for a device with none.
	size_t recentEventsAt(size_t slot, device::RecentEvent* out, size_t capacity) const;
	/// THE EXTRA STATE THE MODULATOR IN `slot` HAS CHANGED ITSELF (`ModOps::publishExtraState`, backlog MX): its serial, or
	/// 0 for none; `*count` floats written into `extra`. The audio thread, after the tick, as every reading here.
	uint64_t extraStateAt(size_t slot, float* extra, size_t capacity, size_t* count) const;

	/// **HOW HARD A SLOT'S DEVICE IS BEING DRIVEN**, or false when it publishes no such reading. See
	/// the definition: same declining contract as `FxRunner::drivePeakAt`, read directly rather than
	/// through a channel because this host owns the storage.
	bool drivePeakAt(size_t slot, float& peak) const;

	/// **THE LOUDEST SAMPLE A SLOT'S DEVICE PUT OUT, HELD THEN FALLING** — every fx card's courtesy output meter
	/// (backlog MK). Linear and NOT clamped: above 1.0 is the over a card draws from the line's other end. Measured by
	/// the engine after the device's own process call, so every audio device has one without publishing anything;
	/// a device that ran per voice is the loudest of its voices (the sum is heard after the voices, where no single
	/// device stands). False — `peak` untouched — for a slot holding no audio device, or one that has not run yet.
	bool outputPeakAt(size_t slot, float& peak) const;
	/// THE METER'S BALLISTICS: a new peak holds this long, then falls at `kOutputMeterFallDbPerSecond` (PROVISIONAL).
	static constexpr double kOutputMeterHoldSeconds = 0.5;
	static constexpr double kOutputMeterFallDbPerSecond = 24.0;
	/// UNDER THIS THE HELD PEAK IS EXACTLY ZERO, so a fallen meter draws nothing rather than a denormal's sliver.
	static constexpr float kOutputMeterSilence = 1.0e-5f;

	/// A ROAD FROM A FRESHLY BUILT DEVICE TO WHOEVER WANTS TO PUBLISH FROM IT.
	///
	/// **THE SAME SHAPE AS `FxRunner::setDspConnector`, DELIBERATELY.** That one was
	/// `setOwlttDspConnector` with the tag matched inside the runner, and mainline generalised it
	/// because *"a second publisher meant editing the runner, which is the shape a shared contract
	/// should never have"*. The rack IS that second publisher; copying the generalised shape rather
	/// than inventing a rack-specific one is the whole benefit of it having been generalised.
	///
	/// CALLED ONCE PER DEVICE AS IT IS CONSTRUCTED, with the tag, the slot and the device's storage.
	/// What the callee does with it is its own business, and a device nobody cares about is simply not
	/// matched.
	void setDspConnector(std::function<void(const char* tag, size_t slot, void* dspStorage)> c) {
		connector_ = std::move(c);
	}

	/// WHICH DEVICES THIS RACK'S EDITOR CAN OPEN A PANEL FOR — installed by the editor, because the
	/// engine does not know and must not: this library links no UI (the CMake note on `feathers-rack`
	/// is the rule), and the table of buildable panels is the editor's. The same shape as
	/// `setDspConnector` above — a road installed by whoever owns the answer — and until one is
	/// installed the engine declines, so a headless rack offers no way in and draws none.
	void setExpandedViewQuery(std::function<bool(const device::Descriptor&)> q) {
		expandedViewQuery_ = std::move(q);
	}
	bool hasExpandedViewFor(const device::Descriptor& desc) const {
		return expandedViewQuery_ && expandedViewQuery_(desc);
	}

	// **`routeLaneOf` STOOD HERE AND IS DELETED (kill-lanes step 5; §2.5 named it as the one dead
	// survivor to simply delete).** Its own comment had already conceded what it was: *"LANE EQUALS
	// SLOT AGAIN, AND THIS TIME IT IS TRUE BY CONSTRUCTION"* — step 2 made `slots_` lane-indexed and
	// put the order in the graph, so the two numbers stopped being two numbers. What it added on top
	// was a liveness test, and every caller had already established liveness one line earlier; a
	// caller that wants that question asks `slots().descriptorAtLane(slot)`, which answers it plainly.
	//
	// IT IS WORTH ONE LINE OF WHY IT SURVIVED SO LONG: a name that outlived its behaviour reads as a
	// crossing between two spaces, so every reader spends a moment deciding which space they are in.
	// `fx_card.h` and `fx_lane.cpp` both cite it by name for exactly that lesson.

	/// The lowest lane no slot holds — see `Slot::routeLane`.
	size_t lowestFreeLane() const noexcept;
	/// **THE ROUTING API, BY ENDPOINT.** It took two flat coordinates and decoded them through
	/// `Graph::endpointsOf`; the coordinates are gone (kill-lanes §2.4b) and every caller — the loader,
	/// the bridge, a case — already held the identities it was encoding.
	float routeAmount(backplane::Source source, backplane::Destination destination) const;
	/// **PLANT A ROUTE PAST THE RULE — TESTS ONLY.** Every product write goes through `Graph::route`,
	/// which refuses a backwards or self route, and the loader gates on `mayRoute` first — so there is no
	/// product road that can put an illegal route into a patch, which is the point, and the case that
	/// proves the LOADER refuses one needs a road that is not a product road. This is that road, for any
	/// pair of ends (a host cell included, since every route is an edge). See `Graph::plantRouteForTest`.
	bool plantRouteForTest(backplane::Source s, backplane::Destination d, float amount) {
		routesDirty_ = true;
		return graph().plantRouteForTest(s, d, amount);
	}
	void setRouteAmount(backplane::Source source, backplane::Destination destination, float amount);
	/// THE CURVE ON A ROUTE THIS ENGINE'S OWN GRAPH HOLDS (`setRouteAmount`'s road) — refused for none.
	bool setRouteTransfer(backplane::Source source, backplane::Destination destination, backplane::Transfer t) {
		routesDirty_ = true;
		return graph().setTransfer(source, destination, t);
	}

	// **`sourceValue(size_t source)` STOOD HERE AND IS DELETED (kill-lanes §2.4b).** It decoded a flat
	// source coordinate into a macro or a device output and read it; a resolve now reads the cell its
	// input points at, and a case reads `macro(i)` or `outputAt(lane, output)` — the two things it was
	// one of.

	/// Base plus every route reaching this control, clamped to the control's declared range unless it
	/// says otherwise. Exposed because a claim about a VALUE cannot be read back out of a rendered
	/// buffer — CLAUDE.md's rule, which cost two sessions independently before it was written down.
	float resolvedFieldForTest(size_t slot, size_t control) const;
	/// The right ear of `resolvedFieldForTest` — the left for a control no stereo route reaches. Test-only.
	float resolvedFieldRForTest(size_t slot, size_t control) const;
	/// TEST ONLY: control `control`'s per-sample window for `voice` in `slot` (flat flag last), or nullptr.
	[[nodiscard]] const float* controlWindowForVoiceForTest(size_t slot, size_t control, size_t voice) const {
		if (slot >= slots_.size()) return nullptr;
		const Slot& s = slots_[slot];
		const auto stride = static_cast<size_t>(device::kTickWindowStride);
		const size_t at = (voice * s.base.size() + control) * stride;
		return control < s.base.size() && at + stride <= s.controlWindowVoice.size() ? &s.controlWindowVoice[at] : nullptr;
	}
	/// TEST ONLY: control `control`'s per-sample window in `slot` (flat flag last), or nullptr.
	[[nodiscard]] const float* controlWindowForTest(size_t slot, size_t control) const {
		if (slot >= slots_.size()) return nullptr;
		const Slot& s = slots_[slot];
		const auto stride = static_cast<size_t>(device::kTickWindowStride);
		return (control + 1) * stride <= s.controlWindow.size() ? &s.controlWindow[control * stride] : nullptr;
	}
	/// TEST ONLY: the device instance's storage in `slot`, for a device's own test seams.
	[[nodiscard]] const void* storageForTest(size_t slot) const { return slot < slots_.size() ? slots_[slot].storage.get() : nullptr; }
	/// The descriptor of the device at `slot`, or null. Test-only (the soak finds its Pattern by tag).
	[[nodiscard]] const device::Descriptor* descriptorForTest(size_t slot) const {
		return slot < slots_.size() ? slots_[slot].desc.load(std::memory_order_acquire) : nullptr;
	}
	[[nodiscard]] size_t slotsForTest() const noexcept { return slots_.size(); }
	/// TEST ONLY (backlog KW): the bus furniture — the enable's crossfade and whether the device is parked.
	[[nodiscard]] const FurnitureRamp* furnitureForTest(size_t slot) const {
		return slot < slots_.size() ? &slots_[slot].furniture : nullptr;
	}
	/// TEST ONLY (backlog KW): how many times the device in `slot` (its bus instance) has been run.
	[[nodiscard]] uint64_t runsForTest(size_t slot) const { return slot < slots_.size() ? slots_[slot].runs : 0; }
	/// TEST ONLY: the instance `voice` of the device in `slot` (its per-voice road), or nullptr when none is built.
	[[nodiscard]] const void* voiceStorageForTest(size_t slot, size_t voice) const {
		return slot < slots_.size() && voice < slots_[slot].voices.size() ? slots_[slot].voices[voice].get() : nullptr;
	}
	/// THE SAME CONTROL RESOLVED FOR ONE VOICE — what a per-voice instance of the device at `slot` is handed
	/// when a per-voice source reaches it (backlog DU). Runs the resolve; a test-only seam, not a reading.
	float resolvedFieldForVoiceForTest(size_t slot, size_t control, size_t voice);
	/// One voice's right-ear row entry, `resolvedFieldForVoiceForTest`'s twin. Test-only.
	float resolvedFieldRForVoiceForTest(size_t slot, size_t control, size_t voice);
	/// HOW MANY RIGHT-EAR RESOLVE PASSES HAVE RUN — zero for a rack no stereo route reaches (the zero-cost promise).
	[[nodiscard]] uint64_t rightPassesForTest() const noexcept { return rightPasses_; }
	/// **THE OUTPUT STORAGE A SLOT HOLDS** (2026-11-07, `Slot::outputs`): its cells, its windows and its per-voice cells,
	/// as counts — what the install sized from the device, read back by value.
	struct OutputStorage {
		size_t cells = 0;
		size_t windows = 0;
		size_t cellsR = 0;
		size_t windowsR = 0;
		size_t perVoice = 0;
		size_t perVoiceWindows = 0;
	};
	[[nodiscard]] OutputStorage outputStorageForTest(size_t lane) const noexcept {
		if (lane >= slots_.size()) {
			return {};
		}
		const Slot& s = slots_[lane];
		return { s.outputs.size(), s.outputsWindow.size(), s.outputsR.size(), s.outputsWindowR.size(), s.outputsPerVoice.size(),
			     s.outputsPerVoiceWindow.size() };
	}
	[[nodiscard]] size_t voicesForTest() const noexcept { return voices_; }
	/// HOW MANY WHOLE-ROW RESOLVES HAVE RUN (`Slot::baseSerial`): at rest, with nothing written, routed or host-moved, none —
	/// only the routed controls are resolved again. Counted so a case can pin that by value.
	[[nodiscard]] uint64_t fullResolvesForTest() const noexcept { return fullResolves_; }

	/// **HOW MANY DEVICES ARE PARKED, BUILT AND NOT YET DESTROYED.** The retire queue is the whole of
	/// the removal handoff and it is otherwise unobservable: a removal that destroys and a removal
	/// that parks leave the same rack behind, and only one of them is safe while a block is running.
	[[nodiscard]] size_t retiredCountForTest() const noexcept { return retiredCount(); }
	[[nodiscard]] size_t retiredCapacityForTest() const noexcept { return retired_.size(); }
	/// THE PUBLISHED ROUTES, as the last block resolved through them.
	/// WHETHER A BLOCK READS AS IN FLIGHT — the epoch's parity, which is what every reader of the handoff
	/// asks. A host that begins a block and never ends it leaves this true while its audio thread idles.
	[[nodiscard]] bool inBlockForTest() const noexcept {
		return (blockEpoch_.load(std::memory_order_seq_cst) & 1u) != 0u;
	}

	/// **PRETEND A BLOCK IS IN FLIGHT — the two lines `processStereo` runs at its ends, and nothing
	/// else.** `blockPassed` returns immediately when the epoch is EVEN, so an idle test can never
	/// reach the parking road: every removal it makes destroys at once, and the branch that exists to
	/// keep a device alive under a running block goes unexercised by construction.
	///
	/// IT IS NOT A LIE ABOUT THE MECHANISM, which is why it is two lines rather than a flag: the epoch
	/// IS the handoff, and these move it exactly as the audio thread does. A test holding it odd is
	/// indistinguishable, to every road that reads it, from a block that has not finished.
	void enterBlockForTest() noexcept { blockEpoch_.fetch_add(1, std::memory_order_seq_cst); }
	void leaveBlockForTest() noexcept { blockEpoch_.fetch_add(1, std::memory_order_seq_cst); }

	/// Destroy whatever is parked and can now be destroyed. The message thread's own road, exposed so
	/// a case can say WHEN rather than waiting for the next add to do it incidentally.
	void reapRetiredForTest() noexcept { reapRetired(); }

private:
	/// RESOLVE A SLOT'S ROW, AGAINST THE DESCRIPTOR THE CALLER ALREADY HELD.
	///
	/// **THE DESCRIPTOR IS A PARAMETER, NOT A RE-READ.** This used to load `slot.desc` itself, which
	/// made it a second pass over an atomic the editor can clear — the caller checks, this dereferences,
	/// and the removal in between is a null dereference on the audio thread. Taking it as an argument
	/// makes that unrepresentable rather than remembered.
	void resolveInto(Slot& slot, const device::Descriptor& desc);
	/// ONE CONTROL, RESOLVED — for the block (`kNoVoice`) or for one voice, which reads a per-voice source's
	/// own cell for that voice. The one body both rows above come from.
	static constexpr size_t kNoVoice = static_cast<size_t>(-1);
	/// One declared control's tick window into `out`, for the block (`kNoVoice`) or a voice — see the definition.
	void resolveWindowInto(Slot& slot, const device::Descriptor& desc, size_t control, float* out, size_t voice) const;
	/// Hand a device instance the moving runs of a row of `width` windows for its next call — see the definition.
	void handWindows(const device::AudioOps& ops, void* instance, const float* windows, size_t width, int frames) const;
	/// The loudest |sample| of a chunk, both channels (`right` may be null) — the courtesy meter's measure (backlog MK).
	[[nodiscard]] static float peakOf(const float* left, const float* right, int frames) noexcept;
	/// Fold each audio slot's chunk peak into its held, falling one and publish it — see `outputPeakAt`.
	void advanceOutputMeters(int frames) noexcept;
	[[nodiscard]] float resolveControl(const Slot& slot, const device::Descriptor& desc, size_t control, size_t voice) const;
	/// `resolveControl` FOR ONE EAR (backlog KU) — `false` IS `resolveControl`, the same expression; `true` reads each
	/// input's right-ear cell (`Input::valueR`/`perVoiceR`).
	template <bool kRight>
	[[nodiscard]] float resolveEar(const Slot& slot, const device::Descriptor& desc, size_t control, size_t voice) const;
	/// `slot.resolvedVoice` for `voice` — the collapsed row except where a per-voice source reaches a control.
	void resolveVoiceInto(Slot& slot, const device::Descriptor& desc, size_t voice);
	/// **WHETHER A SLOT PUBLISHES FROM ITS VOICE INSTANCES** (backlog NA (b)): an audio device that is also a modulator,
	/// Poly on in its row, in a rack with voices. Its voices are ticked on the mod road and `outputForNoteAt` reads the
	/// one holding the note; the collapsed cell rests. Every other slot publishes from `storage`, as before.
	[[nodiscard]] static bool publishesFromVoices(const Slot& slot, const device::Descriptor& d) noexcept;
	/// THE VOICE HOLDING A NOTE (`setVoiceNote`), matched as `resolveForNote` matches; `voices_` for none.
	[[nodiscard]] size_t voiceHolding(int32_t noteId, int16_t channel, int16_t key) const noexcept;
	/// THE VOICE THAT TOOK A NOTE MOST RECENTLY, held or not — `PerNoteTable::newest`'s rule, the one every per-voice
	/// modulator answers a destination with no voice by; `voices_` when no voice has held one.
	[[nodiscard]] size_t newestVoice() const noexcept;
	/// `Slot::trigLatched`'s step, after a slot publishing from its voices has ticked.
	void latchVoiceTrigFires(Slot& slot, const device::Descriptor& d) noexcept;

	/// Run the block's notes through every device that declares , in chain order.
	void advanceNotes();
	/// **THE SELF-CLOCKED DEVICES' NOTES IN ONE PIECE OF A HOST BLOCK** (an arp). Asked at
	/// the piece's first sample and at every sample an emitter names after it (`emitNotesNow`), the clock advanced to
	/// that sample; what leaves the chain is appended to `blockNotesLeft_` at `pieceStart` + the sample. Nothing at all
	/// with no emitter in the chain, so a pass-through block is the one it always was.
	void emitPiece(const device::TransportClock& clock, int pieceStart, int frames, size_t& leftCount) noexcept;
	/// `beginBlock`, with the note pass only when `notes` — a host block cut into tick pieces runs its notes once.
	void beginPiece(int frames, const backplane::BlockContext& block, bool notes);
	/// `writeOutputForVoice` and `startOutputForVoice`: a tick's ramp into `value`, or a start flat at it.
	void writeVoiceOutput(DeviceInstanceId id, size_t output, size_t voice, float value, bool start) noexcept;
	void advanceModulators(int frames, const backplane::BlockContext& block);
	void destroySlot(Slot& slot);
	/// ONE INSTANCE OF `desc`, constructed, prepared at this engine's rate and (a container) handed its catalogue —
	/// the bus instance and every voice's come from here. MESSAGE THREAD; allocates.
	[[nodiscard]] std::unique_ptr<std::byte[], AlignedFree> buildInstance(const device::Descriptor& desc) const;
	/// `desc`'s instance per voice into `p`, each told its voice, and their tick windows for a row `width` wide.
	/// Nothing for a device with no per-voice road or a rack with no voices. MESSAGE THREAD; allocates.
	void buildVoicesInto(Provision& p, const device::Descriptor& desc, size_t width) const;

	/// HAS ANY BLOCK THAT WAS RUNNING FINISHED? — the whole handoff, in one question.
	///
	/// `blockEpoch_` is incremented by the audio thread at the START and the END of every block, so it
	/// is ODD exactly while one is in flight. Even means nothing is inside the engine right now; odd
	/// means wait for it to change, which it will at the end of the block already running.
	///
	/// **BOUNDED, AND FALSE IS A REAL ANSWER.** If the audio thread is stalled, or the host stopped
	/// calling `process` mid-block, an unbounded wait would hang the EDITOR — so this gives up and the
	/// caller parks the storage instead. Never freeing early is the requirement; freeing promptly is
	/// only a preference.
	bool blockPassed(unsigned spins) noexcept;

	/// HOW LONG A REMOVAL WAITS FOR A BLOCK TO END BEFORE PARKING INSTEAD.
	///
	/// A NUMBER OF YIELDS RATHER THAN A DURATION, because the thing being waited on is one audio block
	/// and the editor has nothing better to do meanwhile. At 48k/512 a block is ~10ms and a yield is
	/// microseconds, so this is generous by orders of magnitude — and being generous is free, because
	/// the fallback is correct too. It exists to bound the EDITOR, not to bound the audio thread.
	static constexpr unsigned kRemovalSpins = 20000;

	/// DEVICE STATE THAT HAS BEEN TAKEN OUT AND NOT YET DESTRUCTED.
	///
	/// **BOTH HALVES MUST WAIT, NOT JUST THE FREE.** `destroySlot` runs the device's own `destruct`
	/// before releasing the storage, and a destructor running while a block is inside that same object
	/// is no better than freeing it. So the descriptor is parked alongside the bytes.
	struct Retired {
		std::unique_ptr<std::byte[], AlignedFree> storage;
		/// The slot's per-voice instances, parked with it — SWAPPED in, never assigned, so the drain
		/// frees no buffer (the rule `adoptDeviceAt`'s rows already follow).
		std::vector<std::unique_ptr<std::byte[], AlignedFree>> voices;
		const device::Descriptor* desc = nullptr;
		/// `blockEpoch_` when it was parked: odd means a block was in flight and may have been inside
		/// the device; the reaper waits for the epoch to move past it before destructing.
		uint64_t epoch = 0;
	};

	/// **THE PARKING — a bounded ring, one producer (the drain, `park`), one consumer (the message
	/// thread, `reapRetired`).** Sized from the axes — twice the slots plus slack — it holds a full rack replaced by a patch
	/// load plus a queue of removes; when it is full the drain REFUSES the remove rather than block,
	/// drop or free — the case for that path is the one that matters (backlog CF, by design).
	/// `reapRetired` destructs and frees where freeing is allowed; an entry parked with a
	/// block in flight waits for the epoch to move past it.
	/// SIZED FROM THE AXES AT CONSTRUCTION — twice the slots (a full rack replaced by a load) plus slack
	/// for a queue of removes — where allocation is allowed; never resized after.
	bool park(Slot& slot, const device::Descriptor* desc) noexcept;
	size_t retiredCount() const noexcept {
		return retiredTail_.load(std::memory_order_acquire) - retiredHead_.load(std::memory_order_acquire);
	}

	std::vector<Retired> retired_;
	std::atomic<size_t> retiredHead_ { 0 }; ///< consumer's
	std::atomic<size_t> retiredTail_ { 0 }; ///< producer's
	/// See `setReapHold`. Null: every instance whose epoch allows it is freed.
	ReapHold reapHold_ = nullptr;
	const void* reapHoldContext_ = nullptr;

	/// ODD WHILE A BLOCK IS IN FLIGHT. Two relaxed-looking increments per block, and they are
	/// `seq_cst` on purpose: this is the store-load half of a Dekker pattern — the editor stores
	/// `desc = nullptr` and then reads this, while the audio thread increments this and then reads
	/// `desc`. Acquire/release alone does not order a store against a later load on the same thread,
	/// and getting that wrong would free storage a block was about to enter.
	std::atomic<uint64_t> blockEpoch_ { 0 };

	backplane::Axes axes_;



	/// **THE ORDER, AND THE IDENTITIES — the graph owns both now.**
	///
	/// MESSAGE THREAD ONLY. `graph.h`'s THREADS section: every mutator is message thread, and
	/// `chain(c)` is a span into storage the message thread rewrites — so the audio thread never
	/// touches it. It reads `orderSnapshot_` instead, published below.
	/// WHERE THIS RACK'S DEVICES COME FROM. See the constructor.
	device::Catalogue catalogue_;
	/// SEE `setVoices`.
	size_t voices_ = 0;
	/// THE NOTE EACH VOICE HOLDS — `setVoiceNote`. Sized by `setVoices`, written and read on the audio thread.
	struct VoiceNote {
		int32_t noteId = -1;
		int16_t channel = 0;
		int16_t key = 0;
		bool live = false;
		/// WHEN THIS VOICE TOOK ITS NOTE, from `voiceNoteSerial_` — the newest is what a destination with no voice reads
		/// of a device publishing from its voices (`newestVoice`). 0 for a voice that never held one.
		uint64_t serial = 0;
	};
	std::vector<VoiceNote> voiceNotes_;
	uint64_t voiceNoteSerial_ = 0;
	/// WHAT `ModTickContext::perNote` POINTS AT for one modulator's tick — see `advanceModulators`.
	struct NoteResolve {
		const RackEngine* engine = nullptr;
		const Slot* slot = nullptr;
		const device::Descriptor* desc = nullptr;
	};
	static bool resolveForNote(const void* context, size_t control, int32_t noteId, int16_t channel, int16_t key,
	                           float* out) noexcept;
	backplane::InstanceCounter ids_;
	/// **THE GRAPH, BY POINTER** (unbounded devices step 4). The drain mutates it (`addDeviceAt`, the
	/// converger), so a bigger one cannot be grown in place there: the main thread builds a bigger graph
	/// (`requestCapacity`), the drain copies this one into it with no allocation (`Graph::assignGrown`) and
	/// swaps the pointer, and the main thread frees the old one (`reapGrowth`). One acquire per use.
	std::unique_ptr<backplane::Graph> graphOwned_;
	std::atomic<backplane::Graph*> graphPtr_ { nullptr };
	/// A BIGGER GRAPH IN FLIGHT: Idle, Pending (built, waiting for a drain), Taken (the drain swapped, so it
	/// holds the OLD graph), then freed by `reapGrowth`.
	std::unique_ptr<backplane::Graph> nextGraph_;
	std::atomic<int> growthState_ { 0 };
	static constexpr int kGrowthIdle = 0;
	static constexpr int kGrowthPending = 1;
	static constexpr int kGrowthTaken = 2;
	std::vector<std::function<void(size_t)>> growthListeners_;
	/// THE SITE MAILBOX AND ITS RETURN (backlog NR) — see `offerSite`. Lock-free stacks; the drain reverses what it
	/// takes into posting order.
	std::atomic<SiteOffer*> siteInbox_ { nullptr };
	std::atomic<SiteOffer*> siteSpent_ { nullptr };
	/// **THE FURNITURE'S DRY, ONE CALL AT A TIME** — sized in `prepare` to the longest call. One pair serves every slot
	/// and every voice: each blend copies, runs and blends before the next slot touches it.
	std::vector<float> furnitureDryL_;
	std::vector<float> furnitureDryR_;
	/// **THE ROW A SWITCHED-OFF DEVICE IS HANDED (backlog KW)**: its resolved row with the enable read ON, so the device
	/// never takes its own hard bypass under this engine — the engine crossfades instead. Audio thread, used at once.
	std::array<float, device::kMaxDeviceFields> enableOnRow_ {};
	/// The inner chain this engine is, as the spike recorder names it (`setCostLane`).
	const char* costLane_ = nullptr;
	int costLaneIndex_ = -1;
	[[nodiscard]] const float* fieldsFor(const device::Descriptor& d, const std::vector<float>& row) noexcept;
	/// The furniture ramp's length in samples at the running rate (5 ms).
	int furnitureRampSamples_ = 1;
	/// RUN ONE AUDIO SLOT'S DEVICE ON THE BUS WITH ITS SITE AND ITS FURNITURE AROUND IT — see `processBus`.
	void runBusSlot(Slot& slot, size_t lane, const device::Descriptor& d, float* left, float* right, int frames);
	/// The device's own bus call — `processBus`'s body before NR, unchanged.
	void runDevice(Slot& slot, size_t lane, const device::Descriptor& d, float* left, float* right, int frames);
	/// Whether the slot blends its furniture (a declared Mix, not the device's own), and the targets it aims at.
	[[nodiscard]] bool furnitureTargets(const Slot& slot, const device::Descriptor& d, size_t voice, float* wet,
	                                    float* dry) const;
	/// Size every new slot as `setVoices` sized the ones before it, and give every new hop its index.
	void growPools(size_t slots);
	backplane::Graph& graph() noexcept { return *graphPtr_.load(std::memory_order_acquire); }
	const backplane::Graph& graph() const noexcept { return *graphPtr_.load(std::memory_order_acquire); }

	/// **EVERY INPUT OF EVERY NODE, IN ONE ARRAY THE NODES' `InputRange`s INDEX INTO** — the compiled plan
	/// (kill-lanes §2.4). Rebuilt whole at the drain (`rebuildInputs`) from the routes by identity — this
	/// engine's own graph's, or the ones a host handed over (`setHostRoutes`). GROWN IN PLACE on the message thread
	/// (`reserveRoutes`, backlog OJ): `inputPool_` is the store, and `inputs_` the directory the last rebuild took, which
	/// every reader indexes — so a hot resolve reads two plain loads per input and no atomic. A growth after the rebuild
	/// adds room no range reaches yet; the view stays valid (the pool frees no directory). AUDIO THREAD ONLY, both
	/// writing and reading through the view; the message thread only adds chunks.
	backplane::ChunkedPool<Input, 16, 3, 2> inputPool_;
	backplane::ChunkedPool<Input, 16, 3, 2>::View inputs_;
	/// See `routeRoomRefusals`. Written at the drain.
	std::atomic<size_t> routeRoomRefusals_ { 0 };
	/// SET BY EVERY MUTATION THAT CAN CHANGE WHAT REACHES WHAT — a route written, a device added,
	/// removed or moved (a move can clear routes the new order forbids), or a host handing routes over.
	/// Drain/audio thread only.
	bool routesDirty_ = true;
	/// RIGHT-EAR RESOLVE PASSES RUN (backlog KU) — counted so a case can pin "no stereo route, no right-ear work".
	uint64_t rightPasses_ = 0;
	/// WHOLE-ROW RESOLVES RUN (`fullResolvesForTest`).
	uint64_t fullResolves_ = 0;
	/// MOVES AT EVERY REBUILD OF THE INPUTS — what a slot's resolve stamp compares (`Slot::resolvedRoutes`). Starts at 1 so a
	/// slot that has never been resolved (stamp 0) cannot match it.
	uint64_t routesSerial_ = 1;
	int tickPhase_ = -1; // see `setTickPhase`
	/// WHERE `processStereo` STANDS INSIDE THE CONTROL TICK, across host blocks — the standalone rack's own tick grid.
	int hostTickPhase_ = 0;
	/// THE ROUTES GATHERED FROM THE GRAPH, at a rebuild. Grown with the graph's edges by `setRouteAmount`.
	backplane::RouteStage routeScratch_;
	/// **THE ROUTES A HOST HANDED OVER** (`setHostRoutes`) — this engine's own copy, so the host's staging
	/// may be reused the moment the call returns. `hostOwnsRoutes_` says whether they, or the graph's, are
	/// what `rebuildInputs` reads.
	backplane::RouteStage hostRoutes_;
	size_t hostRouteCount_ = 0;
	bool hostOwnsRoutes_ = false;
	/// `setHostRoutes`'s body, for a span or a stage's run.
	template <typename Routes>
	void adoptHostRoutes(const Routes& routes) noexcept;
	/// Rebuild every node's inputs from the routes. See `inputs_`.
	void rebuildInputs() noexcept;

	/// WHICH LANE IS AT WHICH POSITION, PUBLISHED FOR THE AUDIO THREAD TO WALK.
	///
	/// **DOUBLE-BUFFERED WITH AN ATOMIC FLIP, WHICH IS THE MAILBOX PATTERN `FxRunner` ALREADY USES.**
	/// A single shared array would let the audio thread read a half-written order — and a torn order
	/// is not a stale order: it can name one lane twice (a device runs twice in a block, audibly) or
	/// skip one. The message thread fills the back buffer and flips; the audio thread reads whichever
	/// index is current for the whole block. Sized at construction and never again.
	struct ChainOrder {
		/// A POOL, so the main thread can grow it while the drain writes and the block reads (unbounded devices).
		std::unique_ptr<backplane::ChunkedPool<uint32_t, 64>> lane = std::make_unique<backplane::ChunkedPool<uint32_t, 64>>();
		size_t count = 0;
		/// **AND IN THE PATH'S ORDER** — where each card stands, which is what "upstream" means for the bus note
		/// (`busNoteForTest`). The graph's order above does not order a Tracker against an audio device.
		std::unique_ptr<backplane::ChunkedPool<uint32_t, 64>> path = std::make_unique<backplane::ChunkedPool<uint32_t, 64>>();
		size_t pathCount = 0;
		[[nodiscard]] uint32_t at(size_t step) const noexcept { return (*lane)[step]; }
	};
	struct OrderSnapshot {
		std::vector<ChainOrder> chain;
	};
	std::array<OrderSnapshot, 2> orderSnapshot_ {};
	/// **WHAT EACH BUS DEVICE IS HANDED THIS PASS**, by slot — the collapsed note, or its nearest upstream
	/// Tracker's pitch on it. Filled by `fillBusNotes` at the top of every bus pass; grown with the slots.
	std::unique_ptr<backplane::ChunkedPool<device::NoteContext, 64>> busNote_ =
	    std::make_unique<backplane::ChunkedPool<device::NoteContext, 64>>();
	void fillBusNotes(const OrderSnapshot& order) noexcept;

	//--------------------------------------------------------------------------------------------//
	// LATENCY DOMAINS — backlog LW (option A)                                              //
	//--------------------------------------------------------------------------------------------//
	/// **EACH SLOT'S ARRIVAL LATENCY `A(n)`**: how late, in samples, the audio reaching it is — the declared latency of
	/// everything before it on the path, its own site's PRE included — and `post`, the same for its site's POST lane
	/// (PRE and the device before it). Summed in `beginPiece`'s walk, which already asks each device its latency, so it
	/// costs a few adds. Grown with the slots.
	struct Arrival {
		uint32_t at = 0;
		uint32_t post = 0;
	};
	std::unique_ptr<backplane::ChunkedPool<Arrival, 64>> arrival_ = std::make_unique<backplane::ChunkedPool<Arrival, 64>>();
	/// THE LATEST ARRIVAL ANY SLOT HAS THIS PIECE — 0 means no device is latent, and every piece of the machinery below
	/// is then skipped: nothing recorded, nothing cut, every device handed exactly what it was before.
	uint32_t maxArrival_ = 0;
	/// THE PER-VOICE ROAD'S LATENCY AS THE LAST WALK FOUND IT — every bus device's arrival starts from it.
	uint32_t voiceShare_ = 0;
	/// **THE COLLAPSED NOTE'S HISTORY**, at the bus clock it took effect: what a device `A` late is handed is the note
	/// `A` samples ago. A ring sized at `prepare` (`kNoteHistory`), recorded only while something is latent; full, it
	/// forgets its oldest moment — a late device then reads that one early, never a note that does not end.
	struct NoteMoment {
		uint64_t at = 0;
		device::NoteContext note {};
		/// AND THE SONG'S CLOCK (step 3), kept only where it JUMPED — a tempo, a locate, play or stop; between moments a
		/// late device's clock is the moment's moved on by what has run since (`advanced`).
		device::TransportClock clock {};
	};
	std::vector<NoteMoment> noteHistory_;
	size_t historyHead_ = 0;  ///< the oldest moment
	size_t historyCount_ = 0;
	/// THE BUS'S OWN CLOCK: the samples `processBus` has run, in both hosts (Feathers runs it every chunk).
	uint64_t busClock_ = 0;
	/// TEST SEAM: delay work done — moments recorded, late notes handed, pieces cut. Zero while nothing is latent.
	uint64_t lateWork_ = 0;
	/// The collapsed note into the history at the bus clock, if it moved. AUDIO THREAD.
	void recordNoteMoment() noexcept;
	/// ONE MOMENT'S ROOM IN A FULL HISTORY, by the rule written at its definition — never an off alone. AUDIO THREAD.
	void makeRoomInHistory() noexcept;
	void removeMoment(size_t k) noexcept;
	uint64_t lateMerges_ = 0;       ///< changes under a held gate merged into the one before (a full history)
	uint64_t lateNotesDropped_ = 0; ///< whole notes, on and off together, a full history could not keep
	/// The note in effect at bus time `at` (the oldest kept, for a time before it).
	[[nodiscard]] const device::NoteContext& noteAt(uint64_t at) const noexcept;
	/// The moment in effect at bus time `at`, or null with nothing kept.
	[[nodiscard]] const NoteMoment* momentAt(uint64_t at) const noexcept;
	/// **THE SONG'S CLOCK AS IT WAS AT BUS TIME `at`** (step 3): the moment's clock moved on to `at` while it played.
	[[nodiscard]] device::TransportClock clockAt(uint64_t at) const noexcept;
	/// **WHAT EACH LATE BUS DEVICE IS HANDED AS ITS CLOCK** (step 3), by slot, filled with its note in `fillBusNotes` —
	/// read only for a slot whose arrival is not zero; `postClock` for its site's POST lane.
	struct LateClock {
		device::TransportClock clock {};
		device::TransportClock postClock {};
	};
	std::unique_ptr<backplane::ChunkedPool<LateClock, 64>> lateClock_ = std::make_unique<backplane::ChunkedPool<LateClock, 64>>();
	/// The clock a bus slot is handed this pass — its late one when it is late, the block's otherwise.
	[[nodiscard]] const device::TransportClock& clockFor(size_t lane) const noexcept;

	/// **AUTOMATION FOR A LATE DEVICE, WAITING ITS TURN** (step 3): a host's write to a late audio device's stored row
	/// (`applyHostSlots`) lands `A(n)` after it was made, so the step a DAW automates at a bar line reaches HARM behind the
	/// linear EQ when the EQ's audio of that bar line does. A hand's edit lands at once (`setField`). Sized at `prepare` (`kPendingFields`); full, a write lands at once —
	/// early, never lost. Each write carries the device's id, so a slot refilled meanwhile is not handed another's.
	struct PendingField {
		uint64_t due = 0;
		uint32_t lane = 0;
		uint32_t control = 0;
		DeviceInstanceId id = kNoInstance;
		float value = 0.0f;
	};
	std::vector<PendingField> pendingFields_;
	size_t pendingCount_ = 0;

	/// **A SOURCE READ LATE** (backlog LW step 4): one ring per (source cell, delay in control ticks) a route crosses a
	/// latency step on, shared by every route on that pair — the source's value each tick, read back `delay` ticks
	/// later. A late input points at `out` (and `outR`) instead of the source, flat across its tick: modulation follows
	/// the audio to within one control tick, the resolution routes resolve at. Sized at `prepare` (`kModRings`, each
	/// one second of ticks, JM's latency ceiling); with none free an input reads its source as before, and is counted.
	struct ModRing {
		const float* src = nullptr;
		const float* srcR = nullptr; ///< its right ear, when it is not the left
		uint32_t delay = 0;
		uint32_t head = 0;
		float out = 0.0f;
		float outR = 0.0f;
		bool live = false;
		bool kept = false;
	};
	std::vector<ModRing> modRings_;
	std::vector<float> modRingBuf_; ///< `kModRings × ringLen_ × 2`, left then right
	size_t ringLen_ = 0;
	size_t modRingRefusals_ = 0;
	/// WHERE THE CONTAINER THIS ENGINE RUNS IN STANDS IN ITS HOST, as last asked — an outer source is that much later.
	uint32_t outerArrival_ = 0;

	/// **THE VOICE ROAD** (step 4): a device running per voice behind a latent per-voice one (`latentPerVoice`, ≤ 80 ms)
	/// is handed its voice's note as it was `A` ago, from a short history per voice. A voice is not reused until its
	/// delayed tail has drained (the synth's drain rule, which holds a released voice for the whole per-voice road), so
	/// the history a voice keeps is its own note's. Its PER-VOICE modulation is still read on time: its
	/// collapsed reads go through the bus rings above.
	bool lateInVoice_ = false;
	mutable device::NoteContext silentVoiceNote_ {}; ///< what `voiceNoteAt` hands before a voice's first moment
	static constexpr size_t kVoiceMoments = 64;
	std::vector<NoteMoment> voiceHistory_; ///< `voices × kVoiceMoments`
	std::vector<uint32_t> voiceHead_;
	std::vector<uint32_t> voiceCount_;
	void recordVoiceMoment(size_t voice, const device::NoteContext& note) noexcept;
	[[nodiscard]] const device::NoteContext& voiceNoteAt(size_t voice, uint64_t at, const device::NoteContext& now) const noexcept;
	/// The voice road's history, at this voice count. MESSAGE THREAD (`prepare`, `setVoices`).
	void sizeVoiceDelays();
	/// Every late input re-pointed at its ring, at the end of `rebuildInputs`. AUDIO THREAD (a drain).
	void assignModRings() noexcept;
	/// Each ring one control tick on, `frames` worth. AUDIO THREAD, after the modulators tick.
	void tickModRings(int frames) noexcept;
	/// Whether this write to a stored control waits (and is queued); false writes it now. AUDIO THREAD.
	bool deferField(Slot& s, size_t lane, const device::Descriptor& d, size_t control, float coerced) noexcept;
	/// Every waiting write whose turn has come — all of them when `all`, or once its device is no longer late.
	void landDueFields(bool all) noexcept;
	/// Whether a host write to this control waits its turn.
	[[nodiscard]] bool hasPendingFor(size_t lane, size_t control) const noexcept;
	/// THE STORED ROW'S ONE WRITE, and what a control's crossing tells the graph — shared by an edit, a host slot and a
	/// write whose turn came.
	void writeBaseControl(Slot& s, const device::Descriptor& d, size_t control, float coerced) noexcept;
	std::atomic<size_t> orderFront_ { 0 };
	/// THE FRONT `beginBlock` ACQUIRED, read by every pass of that block. AUDIO THREAD ONLY.
	size_t blockFront_ = 0;
	/// THE CLOCK `beginBlock` WAS HANDED, which the bus pass delivers. AUDIO THREAD ONLY.
	backplane::BlockContext blockContext_ = backplane::unknownBlock();
	/// See `setTransportAnchors`.
	std::array<TransportAnchor, kMaxTransportAnchors> anchors_ {};
	size_t anchorCount_ = 0;
	/// SEE `anyPerVoice`. AUDIO THREAD ONLY.
	bool anyPerVoice_ = false;
	uint32_t perVoiceLatency_ = 0;
	const float* sideL_ = nullptr;
	const float* sideR_ = nullptr;
	bool sideSounding_ = false;
	uint32_t latentPerVoiceCap_ = 0;
	/// SEE `anyPerVoiceStereo`. AUDIO THREAD ONLY.
	bool anyPerVoiceStereo_ = false;

	/// HOST SLOTS (T5) — see `applyHostSlots`. The bus is the host's; the rest is this engine's own FIXED
	/// storage, written at a drain and read by the block, so neither allocates.
	SlotBus* hostSlotBus_ = nullptr;
	/// ONE BINDING RESOLVED TO WHERE THIS ENGINE KEEPS ITS DEVICE. `device` is kept beside `lane` and
	/// checked every block: a lane is an address, and an address is only a contemporaneous handle.
	struct BoundSlot {
		size_t lane = kNoSlot;
		DeviceInstanceId device = kNoInstance;
		uint16_t control = 0;
		uint16_t slot = 0;
	};
	std::array<BoundSlot, kSlotBindingCapacity> bound_ {};
	size_t boundCount_ = 0;
	/// THE BUS GENERATION EACH SLOT WAS LAST APPLIED AT. Zeroed at every `setHostSlotBindings` (every
	/// drain), so the bus's value is put back over the row the drain just wrote. A bus generation of 0 is
	/// a slot nobody has written or seeded, and is never applied.
	std::array<uint32_t, hostslots::kSlotBusCapacity> lastGen_ {};
	/// Zero the host mod of every resolved binding. Drain / no block in flight.
	void clearBoundHostMod() noexcept;

	/// MESSAGE THREAD. Rebuild the back snapshot from the graph and flip. Called by every mutator —
	/// there is no road that changes the order and does not come through here.
	void publishOrder();

	backplane::ChunkedPool<Slot> slots_;
	/// THE PUBLISHED MACRO CELLS — what every route from a macro reads, through the pointer
	/// `cellOf` hands out; `macroRaw_` is the 0..1 value underneath and `macroBipolar_` says how each
	/// cell is derived from it (backlog EG). Sized once, never moved: the pointers depend on it.
	std::vector<float> macros_;
	std::vector<float> macroRaw_;
	std::vector<uint8_t> macroBipolar_;
	/// Each macro's type and name — the patch's, message thread only (see `setMacroType`). Sized like the rest.
	std::vector<device::MacroType> macroType_;
	std::vector<std::string> macroName_;
	/// **48 kHz UNTIL A HOST SPEAKS, NOT ZERO** — the same default `GuiBridge` and `FxRunner` carry,
	/// and this was the one road that did not.
	///
	/// A UI opens before `prepare` and draws immediately, and this number goes straight into
	/// `flt::gainProfile`. MEASURED: at a rate of 0 that returns NaN for every point, so the filter
	/// card drew an EMPTY BOX — which is what a rack-hosted filter did the moment it got a real card
	/// instead of a knob list. Nothing crashed and nothing logged; the picture was simply blank.
	///
	/// A PLAUSIBLE RATE IS NOT A LIE HERE. `prepare` overwrites it with the host's before a sample is
	/// rendered, and a curve drawn at 48k for one frame is a curve; a curve drawn at 0 is nothing at
	/// all. Zero is not "unknown" to arithmetic, it is a division.
	double sampleRate_ = 48000.0;
	/// **THE HOST'S LONGEST BLOCK, BESIDE ITS RATE AND FOR THE SAME REASON** (backlog GO): a device built before a
	/// host speaks is prepared with a stand-in, and `prepare` replaces it before a block of the host's is rendered.
	int maxFrames_ = device::kDefaultMaxFrames;
	uint64_t prepares_ = 0;

	/// PER-SLOT READINGS. Sized at construction, never resized — see `publishReadings`.
	struct Readings {
		std::array<float, kMaxReadings> value {};
		size_t count = 0;
	};
	backplane::ChunkedPool<Readings> readings_;
	/// ONE PER SLOT, STABLE FOR THE ENGINE'S LIFE — the context a device's readings sink carries back here
	/// (`publishFromHop`), so a device knows only two words of its host. Installed at adoption.
	struct ReadingsHop {
		RackEngine* engine = nullptr;
		size_t slot = 0;
	};
	backplane::ChunkedPool<ReadingsHop> readingHops_;
	static void publishFromHop(void* context, const float* values, size_t count) noexcept;
	std::function<void(const char*, size_t, void*)> connector_;
	std::function<bool(const device::Descriptor&)> expandedViewQuery_;

	/// THE NOTE STREAM, DOUBLE-BUFFERED. A device reads one and writes the other, then they swap — so
	/// no device is ever reading a buffer somebody is writing, and neither is allocated per block.
	std::vector<device::NoteEvent> notesA_;
	std::vector<device::NoteEvent> notesB_;
	/// `processStereo`'S TWO SIDES OF THE PIECE SPLIT: the host block's notes, set aside so each piece is
	/// offered its own, and what left the chain across the pieces, in host-block time. Sized with the two
	/// above, once.
	std::vector<device::NoteEvent> blockNotesIn_;
	std::vector<device::NoteEvent> blockNotesLeft_;
	/// **WHAT A SELF-CLOCKED DEVICE EMITTED AT ONE CLOCK CALL** (`emitPiece`), sized with the others; and the samples the
	/// host has run through `processStereo`, which only grows — the time base `NoteOps::emitNotes` asks for.
	std::vector<device::NoteEvent> emitScratch_;
	uint64_t emitSample_ = 0;
	/// **THE NOTES THE SELF-CLOCKED DEVICES INVENTED THAT ARE STILL SOUNDING DOWNSTREAM**, by channel and key — what the
	/// host must be told to end when their maker cannot: it left the chain or was replaced mid-note (`emitNotesNow`), or an
	/// activate reset it (`flushInventedNotes`). An on sets one, an off or a choke for its key clears it. Kept by
	/// `emitNotesNow`, which BOTH hosts call — the rack plugin through `emitPiece`, the synth's lane through
	/// `Synth::emitLaneNotes` — so neither can play a voice whose off has no maker left.
	std::array<std::array<bool, 128>, 16> inventedHeld_ {};
	size_t emittersSeen_ = 0;
	/// What `notesScheduled` answers: the last `emitNotesNow` heard an emitter name a next call.
	bool notesScheduled_ = false;
	/// **WHICH EMITTERS STOOD IN THE CHAIN LAST CALL, BY BUILD** (`Slot::built`, not the id: a load keeps the id) — a count
	/// alone misses one REPLACED by another (a patch or preset load puts a new Pattern where the old one stood, or a
	/// classic Arp under the old Pattern's id: the count holds, the old instance's notes have no maker left). Fixed room:
	/// past it, the count is the only witness.
	static constexpr size_t kEmitterBuilds = 64;
	std::array<uint64_t, kEmitterBuilds> emitterBuilds_ {};
	std::array<uint64_t, kEmitterBuilds> emitterBuildsNow_ {};
	/// The last `Slot::built` handed out; never reused, so a remembered stamp cannot name a later instance.
	uint64_t buildStamp_ = 0;
	size_t emitterBuildCount_ = 0;
	/// **THE HOST'S NOTES SOUNDING THROUGH THE LANE** (backlog QU): ons that left the note road (`advanceNotes`) and
	/// whose off has not, by key, channel and id. A note device installed while one sounds is seeded with them
	/// (`NoteOps::seedPassed`), so their offs go on to the voices instead of being swallowed. Fixed room: AUDIO THREAD.
	static constexpr size_t kPassSounding = 64;
	std::array<device::NoteEvent, kPassSounding> passSounding_ {};
	size_t passSoundingCount_ = 0;
	void notePassSounding() noexcept;
	bool flushInvented_ = false;
	/// Offs for every invented note still sounding, written to `out` (at most `room`), each at `at`; those written leave
	/// the set. A full buffer keeps the rest held, so the next call ends them: delayed, never lost.
	size_t inventedOffsInto(device::NoteEvent* out, size_t room, uint32_t at) noexcept;
	/// The same, appended to `blockNotesLeft_`.
	void appendInventedOffs(uint32_t at, size_t& leftCount) noexcept;
	size_t notesCount_ = 0;
	/// WHICH OF THE TWO HOLDS THE CURRENT STREAM. Flipped by each device that transforms it.
	bool notesInA_ = true;
	bool gate_ = false;
	bool retrigger_ = false;
	/// See `layoutRevision()`.
	uint32_t layoutRevision_ = 0;
};

} // namespace feathers::rack
