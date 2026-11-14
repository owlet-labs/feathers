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

#include <atomic>
#include <thread>
#include "rack/rack_engine.h"
#include "rack/site_chains.h"

#include "device/modulatable.h"
#include "device/route_resolve.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <cmath>
#include <cstring>
#include <new>

namespace feathers::rack {

namespace {

/// **A DEVICE'S BLOCK-RATE OUTPUT STORAGE, FROM ITS OWN COUNT** (2026-11-07; `Slot::outputs`): one cell per output in each
/// ear, and each output's tick window, all zero as the fixed arrays they replace were. MESSAGE THREAD (a provision): it allocates.
void sizeOutputs(std::vector<float>& outputs, std::vector<float>& window, std::vector<float>& outputsR,
                 std::vector<float>& windowR, size_t count) {
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	outputs.assign(count, 0.0f);
	outputsR.assign(count, 0.0f);
	window.assign(count * stride, 0.0f);
	windowR.assign(count * stride, 0.0f);
}

/// **HOW MANY OUTPUTS A DEVICE KEEPS PER VOICE**: all of them when any one is declared `perVoice` (the layout is
/// `[output × voices + voice]`, so the per-voice ones index the whole run), none when none is — a device whose outputs are
/// one value for every voice is never written per voice (`writeOutputForVoice` asks the descriptor), and pays nothing.
size_t voiceOutputCount(const device::Descriptor* d) {
	if (d == nullptr) {
		return 0;
	}
	for (const device::SourceDesc& s : d->outputs) {
		if (s.perVoice) {
			return d->outputCount();
		}
	}
	return 0;
}

/// **AND ITS PER-VOICE OUTPUT STORAGE**, `voices × count` cells and windows (`Slot::outputsPerVoice`'s layout). MESSAGE
/// THREAD (a provision, `setVoices`): it allocates.
void sizeVoiceOutputs(std::vector<float>& cells, std::vector<float>& windows, std::vector<float>& cellsR,
                      std::vector<float>& windowsR, size_t voices, size_t count) {
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	cells.assign(voices * count, 0.0f);
	cellsR.assign(voices * count, 0.0f);
	windows.assign(voices * count * stride, 0.0f);
	windowsR.assign(voices * count * stride, 0.0f);
}

/// A RELAXED LOAD OF A CELL REACHED THROUGH A CONST PATH — `std::atomic_ref<const T>` is C++26 and Apple's
/// libc++ lacks it (macOS CI runs 34093203285 and 35154217625; `RackBridge::relaxedLoad` and
/// `relaxedLoadCell` in params.cpp are the same fix). SOUND because no output cell is const-DEFINED: each
/// is a slot's own `outputs` word, written through `atomic_ref<float>` where it is published.
float relaxedLoadCell(const float& cell) noexcept {
	return std::atomic_ref<float>(const_cast<float&>(cell)).load(std::memory_order_relaxed);
}

/// **HOW BIG THE EDGE POOL HAS TO BE NOW THAT ROUTES ARE EDGES.**
///
/// It was `deviceSlots * 2` — enough for the audio and note edges a chain implies, and nothing else,
/// because a modulation route was a cell of `RouteTable` and cost the pool nothing. A route is an
/// EDGE now, so it takes a slot in this pool, and
/// sixteen would have started refusing routes on a rack with a few modulators — silently, since
/// `Graph::route` answers false and the gesture was already gated at `mayRoute`.
///
/// **THE BOUND IS ONE ROUTE PER MODULATABLE CONTROL, plus the explicit edges.** Every control of
/// every device modulated once is far past any patch a hand builds, and it is still about 4 KB
/// against the 27,840-cell matrix this replaces — the saving survives being generous by two orders of
/// magnitude.
///
/// **IT IS A BOUND AND NOT GROWTH, AND THAT IS THE PART STILL OWED.** By design a pool is grown outside
/// the audio thread; growth cannot happen at the drain, because `RackBridge::drain()` runs at
/// `process()` — the audio thread — where allocation is forbidden. So growing it means the message
/// thread building a larger pool and the drain adopting it, which is `adoptDeviceAt`'s discipline
/// applied to edges and is its own commit.
constexpr size_t edgeCapacityFor(const backplane::Axes& axes) {
	return backplane::Graph::edgeCapacityFor(axes); // one rule for every graph a host builds
}

/// DESTRUCT THROUGH THE TABLE THAT CONSTRUCTED — audio, mod, notes, the order `provision` builds in.
/// Destructing through a different table than the one that constructed would run the wrong destructor
/// on the right bytes. One function now that a slot holds an instance per voice as well as its own.
void destructWith(const device::Descriptor& d, void* storage) noexcept {
	if (d.audio != nullptr) {
		d.audio->destruct(storage);
	}
	else if (d.mod != nullptr) {
		d.mod->destruct(storage);
	}
	else if (d.notes != nullptr) {
		d.notes->destruct(storage);
	}
}

/// **IS THIS DEVICE ONE THING PER VOICE THIS BLOCK?** Read off its own declaration and its own
/// resolved row, so the engine never names a control — `FxRunner`'s `perVoice`, over the rack's row.
/// The RESOLVED row, as `FxRunner` synced the modulated one: a route onto the switch flips it.
/// THE OUTPUT SCALE A MODULATOR'S ROW ASKS FOR (backlog DH) — its `amountControl`'s resolved value,
/// clamped to the declared span; unity for a device that names none or a row too short to carry it.
float amountOf(const device::Descriptor& d, const std::vector<float>& resolved) noexcept {
	const int control = d.traits.amountControl;
	if (control < 0 || static_cast<size_t>(control) >= resolved.size()) {
		return 1.0f;
	}
	const float amount = resolved[static_cast<size_t>(control)];
	return std::isfinite(amount) ? std::clamp(amount, -1.0f, 1.0f) : 1.0f;
}

bool polyInRow(const device::Descriptor& d, const std::vector<float>& resolved) noexcept {
	if (d.traits.perVoiceControl < 0) {
		return false;
	}
	const auto control = static_cast<size_t>(d.traits.perVoiceControl);
	// AND ONLY WHERE THE ROW LETS IT ACT (`Traits::perVoiceRequires`: the Follower's Poly needs Mode = Audio).
	return control < resolved.size() && resolved[control] >= 0.5f
	       && device::polyAllowed(d, [&](size_t c) { return c < resolved.size() ? resolved[c] : 0.0f; });
}

/// ONE AUDIO DEVICE'S LATENCY, asked with its STORED row (backlog JM, assumption L1). Zero for a device
/// that declares none, which is every device written before it.
uint32_t latencyOf(const Slot& slot, const device::Descriptor& d, double sampleRate) noexcept {
	if (d.audio == nullptr || d.audio->latencySamples == nullptr || slot.storage == nullptr) {
		return 0;
	}
	return d.audio->latencySamples(slot.storage.get(), slot.base.data(), slot.base.size(), sampleRate);
}

/// **WHETHER THE SLOT BLENDS THIS DEVICE'S FURNITURE** (backlog NR, D1): it declares a Mix role that is not its own
/// mix (`Traits::mixInDevice`), naming a control it has.
bool slotBlends(const device::Descriptor& d) noexcept {
	return d.audio != nullptr && !d.traits.mixInDevice && d.traits.mixControl >= 0
	       && static_cast<size_t>(d.traits.mixControl) < d.params.size();
}

/// **THE ENGINE'S ENABLE (backlog KW)**: an audio device with an enable control is switched by THIS engine, not by its
/// own DSP — a crossfade on the slot's furniture, the device kept built and handed its row with the enable read on
/// (`fieldsFor`). A two-stage device (a container) keeps its own bypass: its voices run inside it, out of this reach.
bool engineEnable(const device::Descriptor& d) noexcept {
	return d.audio != nullptr && d.container == nullptr && d.audio->beginPerVoiceBlock == nullptr
	       && d.traits.enableControl >= 0 && static_cast<size_t>(d.traits.enableControl) < d.params.size();
}

/// The enable as the block resolved it (the collapsed row: one switch for every voice —).
bool enabledIn(const std::vector<float>& resolved, const device::Descriptor& d) noexcept {
	const auto e = static_cast<size_t>(d.traits.enableControl);
	return e >= resolved.size() || device::enableOn(resolved[e]);
}

/// THE FURNITURE RAMP'S LENGTH — 5 ms at `sampleRate`, `InnerChain`'s.
int furnitureRampFor(double sampleRate) noexcept {
	return std::max(1, static_cast<int>(std::lround(0.005 * std::max(sampleRate, 1.0))));
}

/// WHETHER `control` DECIDES THAT ONE OF THE DEVICE'S OUTPUTS IS A PAIR (`SourceDesc::pairWhile`, backlog PA).
bool pairsFollow(const device::Descriptor& d, size_t control) noexcept {
	for (const device::SourceDesc& out : d.outputs) {
		if (out.pairWhile >= 0 && static_cast<size_t>(out.pairWhile) == control) {
			return true;
		}
	}
	return false;
}

} // namespace

bool RackEngine::holdsNoteDevice() const noexcept {
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i < slots_.size()) {
				const device::Descriptor* d = slots_[i].live_desc();
				if (d != nullptr && d->notes != nullptr) {
					return true;
				}
			}
		}
	}
	return false;
}

uint32_t RackEngine::latencySamples(double sampleRate) const noexcept {
	// THE ORDER THE NEXT BLOCK WILL WALK, acquired once — `orderFront_`, not `blockFront_`, because this is
	// also asked between blocks (a plugin's `activate`), where no block has acquired one.
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	uint64_t total = 0;
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i >= slots_.size()) {
				continue;
			}
			const device::Descriptor* d = slots_[i].live_desc();
			if (d != nullptr) {
				total += latencyOf(slots_[i], *d, sampleRate);
				// AND ITS SITE'S TWO CHAINS, in series with it (backlog NR): the slot's output is that much later again.
				if (const SiteChains* site = slots_[i].site.load(std::memory_order_acquire); site != nullptr) {
					total += site->latencySamples(sampleRate);
				}
			}
		}
	}
	return static_cast<uint32_t>(std::min<uint64_t>(total, UINT32_MAX));
}

bool RackEngine::allParked() const noexcept {
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	size_t devices = 0;
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i >= slots_.size()) {
				return false;
			}
			const Slot& slot = slots_[i];
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr || !engineEnable(*d) || !slot.furniture.parked) {
				return false;
			}
			const auto e = static_cast<size_t>(d->traits.enableControl);
			if (e >= slot.base.size() || device::enableOn(slot.base[e]) || enabledIn(slot.resolved, *d)) {
				return false;
			}
			++devices;
		}
	}
	return devices > 0;
}

bool RackEngine::holdsTail() const noexcept {
	// **A NOTE STILL ON ITS WAY TO A LATE DEVICE IS SOUND TO COME** (backlog LW, the SLEEP rule): HARM behind a linear-phase
	// EQ starts its harmonic `A(n)` after the note, out of nothing the EQ holds, so a host parked before then would
	// never hear it. The newest moment reaches the latest slot last.
	if (maxArrival_ > 0 && historyCount_ > 0
	    && noteHistory_[(historyHead_ + historyCount_ - 1) % noteHistory_.size()].at + maxArrival_ > busClock_) {
		return true;
	}
	// AND A WRITE STILL WAITING ITS TURN (step 3): the step it brings may be what sounds next.
	if (pendingCount_ > 0) {
		return true;
	}
	// THE SAME WALK AS `latencySamples`, over the order the next block will run.
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i >= slots_.size()) {
				continue;
			}
			const Slot& slot = slots_[i];
			// A PARKED DEVICE IS NOT RUN (backlog KW): neither it nor its site holds a tail; its output is the dry.
			if (slot.furniture.parked) {
				continue;
			}
			// ITS SITE FIRST (backlog NR): a POST chain runs on the bus whatever the device's arrangement.
			if (const SiteChains* site = slot.site.load(std::memory_order_acquire); site != nullptr && site->holdsTail()) {
				return true;
			}
			if (slot.polyThisBlock && !slot.voices.empty()) {
				continue; // its bus instance did not run (`processBus`)
			}
			const device::Descriptor* d = slot.live_desc();
			if (d != nullptr && d->audio != nullptr && d->audio->holdsTail != nullptr && slot.storage != nullptr
			    && d->audio->holdsTail(slot.storage.get())) {
				return true;
			}
		}
	}
	return false;
}

RackEngine::RackEngine(const backplane::Axes& axes, const device::Catalogue& catalogue)
    : axes_(axes), catalogue_(catalogue), ids_(),
      // EVERY GRAPH EDGE PLUS ONE HOST CELL PER DESTINATION — past any patch a hand builds, and a route
      // past it is dropped AND COUNTED (`RouteList::overflow`), never silently.
      graphOwned_(std::make_unique<backplane::Graph>(axes, 1, edgeCapacityFor(axes), ids_)),
      graphPtr_(graphOwned_.get()), slots_(axes.deviceSlots),
      macros_(axes.coreSources, 0.0f), macroRaw_(axes.coreSources, 0.0f), macroBipolar_(axes.coreSources, 0),
      macroType_(axes.coreSources, device::MacroType::Knob), macroName_(axes.coreSources),
      readings_(axes.deviceSlots), readingHops_(axes.deviceSlots) {
	for (size_t slot = 0; slot < readingHops_.size(); ++slot) {
		readingHops_[slot] = ReadingsHop { this, slot };
	}
	// (THE POOLS ROUND UP TO WHOLE CHUNKS, so they may reach past the graph's room; `capacity()` is the
	// graph's, and a lane past it is never handed out.)
	// THE PLAN AND ITS TWO ROUTE COPIES, AT THEIR STARTING ROOM — the old per-slot budget's size, so nothing
	// allocates more than it did until a patch routes past it. They GROW (`reserveRoutes`, backlog OJ); nothing in
	// them ever moves. `macros_` is sized above and never again — an input may point into it.
	reserveRoutes(axes.deviceSlots * 66 + axes.coreDestinations);
	inputs_ = inputPool_.view(); // what every rebuild re-takes (`rebuildInputs`)
	// THE PARKING, SIZED FROM THE AXES HERE — twice the slots plus slack — and never resized after
	// (backlog CF). In the body rather than the initialiser list so its place among the members is
	// free to move.
	retired_.resize(2 * axes.deviceSlots + 16);
	// **ONE CHAIN, AND THE WALK IS WRITTEN AS A LOOP OVER CHAINS ANYWAY.** Today's rack is one chain
	// — the vision's own vocabulary — so the graph is constructed with one. The block loops over
	// `graph().chainCount()` regardless, which runs once and costs nothing, so a rack holding chains in
	// parallel becomes a constructor argument later rather than a rewrite of the block.
	busNote_->grow(axes.deviceSlots); // the bus pass's notes, by slot — see `fillBusNotes`
	arrival_->grow(axes.deviceSlots); // each slot's arrival latency (backlog LW)
	lateClock_->grow(axes.deviceSlots);
	for (OrderSnapshot& snap : orderSnapshot_) {
		snap.chain.resize(graph().chainCount());
		for (ChainOrder& chain : snap.chain) {
			// SIZED ONCE, at the lane capacity, so `publishOrder` never allocates. It runs on the
			// message thread where it would be legal, but a mutator that allocated per edit would be
			// a surprise in a road the editor calls on every drag.
			chain.lane->grow(axes.deviceSlots);
			chain.path->grow(axes.deviceSlots);
			chain.count = 0;
		}
	}
	// SIZED ONCE, LIKE EVERYTHING ELSE THE AUDIO THREAD TOUCHES. `kMaxNotes` each, so a chain of
	// transforms never allocates and never needs to ask whether it may.
	notesA_.resize(kMaxNotes);
	notesB_.resize(kMaxNotes);
	blockNotesIn_.resize(kMaxNotes);
	blockNotesLeft_.resize(kMaxNotes);
	emitScratch_.resize(kMaxNotes);
}

RackEngine::~RackEngine() {
	// THE HOST GUARANTEES NO PROCESSING BY NOW, which is why this destructs unconditionally rather
	// than going through the handoff: `deactivate` precedes `destroy` in CLAP, so there is no block to
	// wait for and nothing to park.
	for (Slot& slot : slots_) {
		destroySlot(slot);
	}
	// THE SITE ENVELOPES STILL IN FLIGHT (backlog NR) — the envelopes only: every site is its converger's.
	for (std::atomic<SiteOffer*>* list : { &siteInbox_, &siteSpent_ }) {
		SiteOffer* offer = list->exchange(nullptr, std::memory_order_acquire);
		while (offer != nullptr) {
			SiteOffer* next = offer->next;
			delete offer;
			offer = next;
		}
	}
	// AND ANYTHING A REMOVAL PARKED AND NEVER GOT BACK. Destructed here rather than leaked, for the
	// same reason: nothing is inside the engine at this point.
	// THE PARKING IS EMPTIED WHATEVER THE EPOCH SAYS: nothing can be inside a device once the engine
	// is being destroyed, so the wait the reaper keeps is not needed here.
	retiredHead_.store(retiredTail_.load(std::memory_order_acquire), std::memory_order_relaxed);
	for (Retired& r : retired_) {
		if (r.desc != nullptr) {
			if (r.storage != nullptr) {
				destructWith(*r.desc, r.storage.get());
			}
			for (auto& voice : r.voices) {
				if (voice != nullptr) {
					destructWith(*r.desc, voice.get());
				}
			}
		}
		r.storage.reset();
		r.voices.clear();
		r.desc = nullptr;
	}
}

void RackEngine::setBlockNotes(const device::NoteEvent* notes, size_t count) noexcept {
	notesInA_ = true;
	notesCount_ = notes == nullptr ? 0 : std::min(count, kMaxNotes);
	for (size_t i = 0; i < notesCount_; ++i) {
		notesA_[i] = notes[i];
	}
}

std::span<const device::NoteEvent> RackEngine::blockNotesOut() const noexcept {
	const std::vector<device::NoteEvent>& live = notesInA_ ? notesA_ : notesB_;
	return { live.data(), notesCount_ };
}

size_t RackEngine::processNotesNow(const device::NoteEvent* in, size_t count, device::NoteEvent* out,
                                   size_t capacity) noexcept {
	setBlockNotes(in, count);
	// A NOTE DEVICE RESOLVES ITS ROW HERE, AHEAD OF THE BLOCK — so the host slots go in first, exactly as
	// `beginPiece` puts them in first (host slots, T5). Cheap, and idempotent within one block.
	applyHostSlots();
	advanceNotes();
	const std::span<const device::NoteEvent> result = blockNotesOut();
	const size_t delivered = (out == nullptr) ? 0 : std::min(result.size(), capacity);
	for (size_t i = 0; i < delivered; ++i) {
		out[i] = result[i];
	}
	// EMPTIED, so the block pass does not hand these notes to the chain a second time.
	notesCount_ = 0;
	notesInA_ = true;
	return delivered;
}

size_t RackEngine::emitNotesNow(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
                                size_t capacity, uint32_t* samplesUntilNext) noexcept {
	uint32_t soonest = std::numeric_limits<uint32_t>::max();
	size_t emitters = 0; // how many stand in the chain — `emitPiece` ends their notes when one leaves
	notesCount_ = 0;
	notesInA_ = true;
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	// NOTHING IS ASKED UNTIL AN EMITTER STANDS IN THE CHAIN — so a chain with none does no work at all, and a
	// pass-through's render is the one it always was. The host slots go in once, before the first emitter's row
	// is resolved, as `processNotesNow` puts them in first.
	bool slotsApplied = false;
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i >= slots_.size()) {
				continue;
			}
			Slot& slot = slots_[i];
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr || slot.storage == nullptr || d->notes == nullptr) {
				continue;
			}
			const bool emits = d->notes->emitNotes != nullptr;
			if (emits && emitters < kEmitterBuilds) {
				emitterBuildsNow_[emitters] = slot.built;
			}
			emitters += emits ? 1 : 0;
			if (!emits && notesCount_ == 0) {
				continue; // nothing reached it, and it makes nothing of its own
			}
			if (!slotsApplied) {
				applyHostSlots();
				slotsApplied = true;
			}
			resolveInto(slot, *d);
			d->notes->setFields(slot.storage.get(), slot.resolved.data(), slot.resolved.size());
			std::vector<device::NoteEvent>& in = notesInA_ ? notesA_ : notesB_;
			std::vector<device::NoteEvent>& next = notesInA_ ? notesB_ : notesA_;
			size_t produced = 0;
			if (notesCount_ > 0) {
				d->notes->processNotes(slot.storage.get(), in.data(), notesCount_, next.data(), kMaxNotes, &produced);
				produced = std::min(produced, kMaxNotes);
			}
			if (emits) {
				// ITS OWN EVENTS AFTER WHAT IT PASSED ON, in the room left. The count is clamped, as `advanceNotes`
				// clamps one: a device's word is not trusted with the next device's buffer.
				uint32_t until = std::numeric_limits<uint32_t>::max();
				const size_t made = d->notes->emitNotes(slot.storage.get(), clock, sampleNow, next.data() + produced,
				                                        kMaxNotes - produced, &until);
				produced += std::min(made, kMaxNotes - produced);
				soonest = std::min(soonest, std::max<uint32_t>(until, 1));
			}
			notesCount_ = produced;
			notesInA_ = !notesInA_;
		}
	}
	// **AN EMITTER LEFT THE CHAIN WITH NOTES SOUNDING — removed, or REPLACED**: it is gone, and so is its
	// off, so the offs go out here, BEFORE this call's own notes. By count AND by build: a patch or preset load retires
	// the old instance and adopts a new one under the same id, so the count alone saw nothing and the note sounded for
	// ever. A second arp still in the chain has its notes ended too and re-strikes at its next step (a provisional call, to be
	// confirmed): the set does not know which arp made which note.
	bool lost = emitters < emittersSeen_;
	const size_t nowCount = std::min(emitters, kEmitterBuilds);
	for (size_t a = 0; a < emitterBuildCount_ && !lost; ++a) {
		bool found = false;
		for (size_t b = 0; b < nowCount && !found; ++b) {
			found = emitterBuildsNow_[b] == emitterBuilds_[a];
		}
		lost = !found;
	}
	std::copy_n(emitterBuildsNow_.begin(), nowCount, emitterBuilds_.begin());
	emitterBuildCount_ = nowCount;
	emittersSeen_ = emitters;
	size_t delivered = 0;
	if (lost && out != nullptr) {
		delivered = inventedOffsInto(out, capacity, 0);
	}
	const std::vector<device::NoteEvent>& result = notesInA_ ? notesA_ : notesB_;
	const size_t made = out == nullptr ? 0 : std::min(notesCount_, capacity - delivered);
	for (size_t i = 0; i < made; ++i) {
		const device::NoteEvent& e = result[i];
		out[delivered++] = e;
		// WHAT IS SOUNDING DOWNSTREAM, by channel and key (an invented note carries no id to match on).
		if (e.channel >= 0 && e.channel < 16 && e.key >= 0 && e.key < 128) {
			if (e.kind == device::NoteEvent::Kind::On) {
				inventedHeld_[static_cast<size_t>(e.channel)][static_cast<size_t>(e.key)] = true;
			}
			else if (e.kind == device::NoteEvent::Kind::Off || e.kind == device::NoteEvent::Kind::Choke) {
				inventedHeld_[static_cast<size_t>(e.channel)][static_cast<size_t>(e.key)] = false;
			}
		}
	}
	notesCount_ = 0;
	notesInA_ = true;
	// **AND WHERE A LATE DEVICE'S NOTE CHANGES** (backlog LW): this is the call a host cuts its chunks by, so a device
	// `A(n)` late has its note move on its own sample there too. Nothing late, no limit added.
	if (maxArrival_ > 0) {
		soonest = std::min(soonest, std::max<uint32_t>(samplesUntilLateNote(), 1));
	}
	if (samplesUntilNext != nullptr) {
		*samplesUntilNext = soonest;
	}
	return delivered;
}

float RackEngine::outputAt(size_t lane, size_t output) const noexcept {
	if (lane >= slots_.size()) {
		return 0.0f;
	}
	const Slot& s = slots_[lane];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || s.storage == nullptr || d->mod == nullptr || d->mod->outputAt == nullptr
	    || output >= d->outputs.size()) {
		return 0.0f; // an absent modulator is a source at rest — a real answer, not a fallback
	}
	// **THE DEVICE'S OWN ANSWER, UNGATED** — what the modulator is doing underneath, switch or no switch.
	// Feathers' `envelopeValueForTest` is documented as exactly this and a case proves the device keeps
	// running while disabled by reading it. What a route RESOLVES against is `publishedOutputAt`.
	return d->mod->outputAt(s.storage.get(), output);
}

float RackEngine::publishedOutputAt(size_t lane, size_t output) const noexcept {
	if (lane >= slots_.size() || output >= slots_[lane].outputs.size()) {
		return 0.0f;
	}
	// **THE PUBLISHED CELL — the number every input pointing at this node reads**, with the disable rule
	// applied where it was published (`advanceModulators`). This is what `sourceValue` used to answer for
	// a device source, and a case that asks what a route SEES asks this rather than the device.
	return relaxedLoadCell(slots_[lane].outputs[output]);
}

float RackEngine::publishedOutputRAt(size_t lane, size_t output) const noexcept {
	if (lane >= slots_.size() || output >= slots_[lane].outputsR.size()) {
		return 0.0f;
	}
	return relaxedLoadCell(slots_[lane].outputsR[output]);
}

float RackEngine::outputForNoteAt(size_t lane, size_t output, int32_t noteId, int16_t channel,
                                  int16_t key) const noexcept {
	if (lane >= slots_.size()) {
		return 0.0f;
	}
	const Slot& s = slots_[lane];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || s.storage == nullptr || d->mod == nullptr) {
		return 0.0f;
	}
	// A DUAL DEVICE IN THE VOICES (see `advanceModulators`): the voice instance holding this note answers, by the whole
	// note as `resolveForNote` matches it. Switched off it rests, as the collapsed cell does; a note no voice holds rests.
	if (publishesFromVoices(s, *d)) {
		const int enable = d->traits.enableControl;
		const bool disabled = enable >= 0 && static_cast<size_t>(enable) < s.resolved.size()
		                      && s.resolved[static_cast<size_t>(enable)] < 0.5f;
		const size_t v = voiceHolding(noteId, channel, key);
		if (disabled || v >= s.voices.size() || output >= d->outputs.size() || d->mod->outputAt == nullptr) {
			return 0.0f;
		}
		const float value = d->mod->outputAt(s.voices[v].get(), output);
		return std::isfinite(value) ? value : 0.0f;
	}
	if (d->mod->outputForNote == nullptr) {
		return outputAt(lane, output);
	}
	return output < d->outputs.size() ? d->mod->outputForNote(s.storage.get(), output, noteId, channel, key) : 0.0f;
}

void RackEngine::advanceNotes() {
	// EVERY DEVICE IN CHAIN ORDER, AND ONE THAT DOES NOT DECLARE `NoteOps` IS SIMPLY NOT ASKED — the
	// stream reaches the next device untouched. That is the pass-through behaviour described, and it is
	// what makes the note lane free for the devices that want nothing to do with it.
	//
	// **ORDER MATTERS MORE HERE THAN ANYWHERE, because notes CHAIN.** A device writes what leaves it
	// and the next one sees that, so walking lanes rather than the path would feed an arp's output to
	// whatever happened to be added first. `graph.h`: *"notes likewise along each chain."*
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
		const size_t i = path.at(step);
		if (i >= slots_.size()) {
			continue;
		}
		Slot& slot = slots_[i];
		// ONE LOAD OF `desc`, held for the whole slot — the TOCTOU that crashed the removal case is
		// exactly as available here.
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || slot.storage == nullptr || d->notes == nullptr) {
			continue;
		}

		// SETTINGS FIRST, as for audio and modulators: a note device is configured by the same row.
		resolveInto(slot, *d);
		d->notes->setFields(slot.storage.get(), slot.resolved.data(), slot.resolved.size());
		// A DEVICE NEW TO THE LANE LEARNS THE HOST NOTES ALREADY SOUNDING THROUGH IT (backlog QU): one that took no part
		// in their ons must still let their offs through, or a voice stays on for ever. Once per built instance.
		if (d->notes->seedPassed != nullptr && slot.notesSeededBuild != slot.built) {
			slot.notesSeededBuild = slot.built;
			if (passSoundingCount_ > 0) {
				d->notes->seedPassed(slot.storage.get(), passSounding_.data(), passSoundingCount_);
			}
		}

		std::vector<device::NoteEvent>& in = notesInA_ ? notesA_ : notesB_;
		std::vector<device::NoteEvent>& out = notesInA_ ? notesB_ : notesA_;
		size_t produced = 0;
		d->notes->processNotes(slot.storage.get(), in.data(), notesCount_, out.data(), kMaxNotes,
		                       &produced);
		// THE DEVICE'S COUNT IS CLAMPED HERE TOO. `NoteOps` tells a device the capacity, and this does
		// not trust it to have honoured it: a count past the buffer would be read by the NEXT device.
		notesCount_ = std::min(produced, kMaxNotes);
		notesInA_ = !notesInA_;
		}
	}
	notePassSounding(); // what left the lane: the host notes sounding through it (backlog QU)
}
void RackEngine::notePassSounding() noexcept {
	for (const device::NoteEvent& e : blockNotesOut()) {
		if (e.kind == device::NoteEvent::Kind::On && e.key >= 0) {
			bool known = false;
			for (size_t i = 0; i < passSoundingCount_ && !known; ++i) {
				known = passSounding_[i].key == e.key && passSounding_[i].channel == e.channel && passSounding_[i].noteId == e.noteId;
			}
			if (!known && passSoundingCount_ < kPassSounding) {
				passSounding_[passSoundingCount_++] = e;
			}
		}
		else if (e.kind == device::NoteEvent::Kind::Off || e.kind == device::NoteEvent::Kind::Choke) {
			// CLAP'S WILDCARDS: a key, channel or id of −1 matches any.
			for (size_t i = 0; i < passSoundingCount_;) {
				const device::NoteEvent& p = passSounding_[i];
				const bool match = (e.key < 0 || p.key == e.key) && (e.channel < 0 || p.channel == e.channel)
				                   && (e.noteId < 0 || p.noteId < 0 || p.noteId == e.noteId);
				if (match) {
					passSounding_[i] = passSounding_[--passSoundingCount_];
				}
				else {
					++i;
				}
			}
		}
	}
}

void RackEngine::publishFromHop(void* context, const float* values, size_t count) noexcept {
	const auto* hop = static_cast<const ReadingsHop*>(context);
	if (hop != nullptr && hop->engine != nullptr) {
		hop->engine->publishReadings(hop->slot, values, count);
	}
}


void RackEngine::publishReadings(size_t slot, const float* values, size_t count) noexcept {
	if (slot >= readings_.size() || values == nullptr) {
		return;
	}
	Readings& r = readings_[slot];
	// CLAMPED RATHER THAN TRUNCATED SILENTLY: the count reported back is what was actually stored, so
	// a device publishing more than the store holds draws fewer meters rather than reading past them.
	const size_t n = std::min(count, kMaxReadings);
	for (size_t i = 0; i < n; ++i) {
		std::atomic_ref<float>(r.value[i]).store(values[i], std::memory_order_relaxed);
	}
	std::atomic_ref<size_t>(r.count).store(n, std::memory_order_relaxed);
}

size_t RackEngine::readingCountAt(size_t slot) const noexcept {
	if (slot >= readings_.size()) {
		return 0;
	}
	size_t& cell = const_cast<size_t&>(readings_[slot].count);
	return std::atomic_ref<size_t>(cell).load(std::memory_order_relaxed);
}

float RackEngine::readingAt(size_t slot, size_t index) const noexcept {
	if (index >= readingCountAt(slot)) {
		return 0.0f;
	}
	float& cell = const_cast<float&>(readings_[slot].value[index]);
	return std::atomic_ref<float>(cell).load(std::memory_order_relaxed);
}

bool RackEngine::blockPassed(unsigned spins) noexcept {
	const uint64_t seen = blockEpoch_.load(std::memory_order_seq_cst);
	if ((seen & 1u) == 0u) {
		// NOTHING IS INSIDE THE ENGINE. A block that starts after this has already seen the cleared
		// `desc` — that is what the seq_cst store before this load buys.
		return true;
	}
	for (unsigned i = 0; i < spins; ++i) {
		if (blockEpoch_.load(std::memory_order_seq_cst) != seen) {
			return true;
		}
		std::this_thread::yield();
	}
	return false;
}


void RackEngine::destroySlot(Slot& slot) {
	if (const device::Descriptor* d = slot.live_desc(); d != nullptr) {
		if (slot.storage != nullptr) {
			destructWith(*d, slot.storage.get());
		}
		for (auto& voice : slot.voices) {
			if (voice != nullptr) {
				destructWith(*d, voice.get());
			}
		}
	}
	slot.storage.reset();
	slot.site.store(nullptr, std::memory_order_release); // the converger's, never freed here
	slot.voices.clear();
	slot.desc.store(nullptr, std::memory_order_release);
	slot.base.clear();
	slot.resolved.clear();
	slot.resolvedVoice.clear();
	slot.resolvedR.clear();
	slot.resolvedVoiceR.clear();
	slot.anyPerVoiceInput = false;
}

void RackEngine::Provision::release() noexcept {
	if (desc != nullptr) {
		if (storage != nullptr) {
			destructWith(*desc, storage.get());
		}
		for (auto& voice : voices) {
			if (voice != nullptr) {
				destructWith(*desc, voice.get());
			}
		}
	}
	storage.reset();
	voices.clear();
}

RackEngine::Provision RackEngine::provision(const char* tag, bool withVoices) const {
	// **THE MESSAGE THREAD'S HALF OF AN ADD — everything that allocates, and the device's own
	// construction and preparation, done where allocation is allowed** (backlog CF). The drain then
	// ADOPTS the result with `adoptDeviceAt`, which moves three pointers and allocates nothing. Same
	// split Feathers makes between `provisionDevices` (message thread, builds) and `FxRunner::sync`
	// (audio thread, adopts). Pure with respect to the slots: it reads `sampleRate_` and nothing else
	// of this object's state, so it may run while blocks run.
	Provision p;
	// **FROM THE CATALOGUE THIS RACK WAS HANDED, never the build's.** The comment that stood above the
	// old lookup said *"A rack that admitted guests would take this as a parameter; today the table is
	// the build's"* — and a rack that can sit in a third party's chain, or hold a third party's device,
	// is that rack. See the constructor.
	const device::Descriptor* desc = catalogue_(tag);
	// NULL IS THE CAPABILITY IN ALL THREE CASES — a device this rack can run is one that processes
	// audio, publishes modulation, handles notes, or any combination. An arp is the case that makes the
	// third necessary: it is none of the first two and is still a device.
	if (desc == nullptr
	    || (desc->audio == nullptr && desc->mod == nullptr && desc->notes == nullptr)) {
		return p;
	}
	p.storage = buildInstance(*desc);
	// **A CONTAINER RUNS THIS ENGINE'S VOICES INSIDE ITSELF** (rack roadmap §8, the pass-through model):
	// told its voice count here, on the message thread, before anything inside is provisioned, so an
	// inner device that declares Poly gets one instance per OUTER voice.
	if (desc->container != nullptr && desc->container->setVoices != nullptr) {
		desc->container->setVoices(p.storage.get(), voices_);
	}
	// THE ROWS TOO — the two vectors an adopt used to resize on the drain.
	//
	// **`params.size() + extraState`, THE ROW RULE** (`device::Traits::extraState`, `device_row.h`). It
	// was `params.size()`, which dropped the floats a device carries beyond its parameters — automod's
	// folded travel, the shaper's phase space — so they never reached the device, never saved with the
	// rack, and a lane moved onto this engine would have lost them where `FxRunner` handed the whole
	// row. Extra floats start at zero, as a default row's do, and are nobody's destination.
	const size_t width = std::min(desc->params.size() + desc->traits.extraState, device::kMaxDeviceFields);
	p.base.assign(width, 0.0f);
	p.resolved.assign(width, 0.0f);
	p.resolvedVoice.assign(width, 0.0f);
	// THE DECLARED CONTROLS' TICK WINDOWS (`AudioOps::windowedControls`), every one FLAT until a resolve says it
	// moves — a zeroed window would read as moving and hand the device a run of zeros.
	if (desc->audio != nullptr && desc->audio->setFieldWindows != nullptr && !desc->audio->windowedControls.empty()) {
		p.controlWindow.assign(width * static_cast<size_t>(device::kTickWindowStride), 0.0f);
		for (size_t c = 0; c < width; ++c) {
			p.controlWindow[c * static_cast<size_t>(device::kTickWindowStride) + static_cast<size_t>(device::kTickWindow)] = 1.0f;
		}
	}
	// **ITS OUTPUTS, SIZED FROM ITS OWN COUNT** (2026-11-07): what the install swaps into the slot (see `Slot::outputs`).
	sizeOutputs(p.outputs, p.outputsWindow, p.outputsR, p.outputsWindowR, desc->outputCount());
	sizeVoiceOutputs(p.outputsPerVoice, p.outputsPerVoiceWindow, p.outputsPerVoiceR, p.outputsPerVoiceWindowR, voices_,
	                 voiceOutputCount(desc));
	// **AND ONE INSTANCE PER VOICE, when asked** — see `Slot::voices` for when that is.
	if (withVoices) {
		buildVoicesInto(p, *desc, width);
	}
	for (size_t c = 0; c < desc->params.size() && c < width; ++c) {
		p.base[c] = static_cast<float>(desc->params[c].def);
	}
	// **THE RESOLVED ROW STARTS AS THE BASE, NOT AS ZEROS.** Nothing reads it before the first block
	// resolves it — except a per-voice write that arrives before that block (`writeOutputForVoice`,
	// which scales by the slot's Amount, backlog DH): a resolved row of zeros there scaled the voice's
	// value by nothing, and a case that wrote then rendered read 0 for 1. The base is the honest answer
	// for a row no block has resolved: it is what the first resolve returns when nothing is routed.
	p.resolved = p.base;
	p.resolvedVoice = p.base;
	// THE RIGHT EAR'S ROWS, beside the left's and for the same reason: sized here, where allocation is allowed.
	p.resolvedR = p.base;
	p.resolvedVoiceR = p.base;
	// ONE EMPTY INPUT RANGE PER DECLARED CONTROL — the node's own edge bookkeeping, sized on the message
	// thread so the drain that adopts it and the rebuild that fills it allocate nothing.
	p.inputs.assign(desc->params.size(), InputRange {});
	// AND THE ROUTE SPAN PER CONTROL, ONCE — `routeSpanOf` for a modulatable control, a negative for one that
	// is no destination. `resolveInto` read both off the descriptor every tick for every control of every
	// modulator (`nthModulatableOfControl` is a scan of the params), a fixed cost the render benchmark measured
	// at about +10 µs a block with nothing routed; the descriptor cannot change while the slot is live, so
	// this is computed where the slot's other per-control storage is.
	p.routeSpan.assign(desc->params.size(), -1.0);
	for (size_t c = 0; c < desc->params.size(); ++c) {
		if (desc->params[c].modulatable) {
			p.routeSpan[c] = device::routeSpanOf(desc->params[c]);
		}
	}
	// **THE FURNITURE'S STATE, FOR A DEVICE WHOSE MIX THE SLOT BLENDS** (backlog NR): a ramp per voice instance, and —
	// for one that can declare a latency — the dry's delay, sized as the cheap win sizes a chain's.
	if (slotBlends(*desc) || engineEnable(*desc)) { // and the enable's crossfade (backlog KW)
		p.furnitureVoice.assign(p.voices.size(), FurnitureRamp {});
		if (desc->audio->latencySamples != nullptr) {
			p.furnitureDelay.prepare(sampleRate_ > 0.0 ? sampleRate_ : 48000.0, 0u);
		}
	}
	p.desc = desc;
	return p;
}

std::unique_ptr<std::byte[], AlignedFree> RackEngine::buildInstance(const device::Descriptor& d) const {
	const device::Descriptor* desc = &d;
	// ONE STORAGE, TAKEN FROM THE FIRST POINTER IN A FIXED ORDER. A device declaring several shares
	// its bytes — the tracker is both a note device and a modulator over ONE object — and the sizes
	// must agree because they describe the same class. The order is audio, mod, notes and it is
	// arbitrary except in being written down.
	const size_t bytes = desc->audio != nullptr  ? desc->audio->stateBytes
	                     : desc->mod != nullptr  ? desc->mod->stateBytes
	                                             : desc->notes->stateBytes;
	const size_t align = desc->audio != nullptr  ? desc->audio->stateAlign
	                     : desc->mod != nullptr  ? desc->mod->stateAlign
	                                             : desc->notes->stateAlign;
	// ONE INSTANCE, BUILT THE ONE WAY — the bus instance and every per-voice one come from here, so a
	// voice's instance cannot be missing a step the bus one had.
	// ALLOCATED AND DELETED THROUGH THE SAME ALIGNMENT — see `Slot::AlignedFree`. The deleter
	// carries the value because nothing else can: by the time the storage is freed, the descriptor
	// that declared the alignment may already have been cleared.
	auto storage = std::unique_ptr<std::byte[], AlignedFree>(
	    // The allocation FUNCTION, not a new-expression: `new (std::align_val_t { a }) std::byte[n]` names the usual
	    // aligned deallocation as its placement delete, which MSVC rejects (C2956). Same call, same storage.
	    static_cast<std::byte*>(::operator new[](bytes, std::align_val_t { align })), AlignedFree { align });
	if (desc->audio != nullptr) {
		desc->audio->construct(storage.get());
		// ITS SLOT IS TOLD AT ADOPTION (`adoptDevice`), where the engine chooses it — a provision is a
		// device, not a place (kill-lanes step 3a).
		if (sampleRate_ > 0.0) {
			desc->audio->prepare(storage.get(), sampleRate_, maxFrames_);
		}
	}
	else if (desc->mod != nullptr) {
		desc->mod->construct(storage.get());
		if (sampleRate_ > 0.0) {
			desc->mod->prepare(storage.get(), sampleRate_);
		}
	}
	else {
		// NOTE-ONLY. Constructed through the one table it has.
		desc->notes->construct(storage.get());
		if (sampleRate_ > 0.0) {
			desc->notes->prepare(storage.get(), sampleRate_);
		}
	}
	// **A DEVICE THAT HOLDS DEVICES IS HANDED THIS RACK'S CATALOGUE** — the rule every host obeys
	// (`ContainerOps::adopt`), and the whole of how a rack in a slot gets a list of devices to
	// build. Here, in the one place a rack builds anything, so a rack added by hand, a rack
	// restored from a file and a third party's chain all arrive with one. Before anything is
	// added, as the contract asks: this device was constructed a few lines ago.
	// ONE CHAIN DEEPER than this one (`Catalogue::depth`): the chains it holds sit below this rack's.
	if (desc->container != nullptr && desc->container->adopt != nullptr) {
		desc->container->adopt(storage.get(), catalogue_.inner());
	}
	return storage;
}

void RackEngine::buildVoicesInto(Provision& p, const device::Descriptor& d, size_t width) const {
	// **ONE PER VOICE, for an audio device that declares a per-voice road, in a rack that has voices.** A rack with
	// no voices builds none, and its Poly devices run on the bus.
	if (d.audio == nullptr || d.traits.perVoiceControl < 0 || voices_ == 0) {
		return;
	}
	p.voices.reserve(voices_);
	for (size_t v = 0; v < voices_; ++v) {
		p.voices.push_back(buildInstance(d));
		// AND EACH IS TOLD WHICH VOICE IT IS (`AudioOps::setVoice`), before any field or block reaches it.
		if (d.audio->setVoice != nullptr) {
			d.audio->setVoice(p.voices.back().get(), v);
		}
	}
	// THEIR TICK WINDOWS, FLAT, as the bus instance's are (`provision`).
	if (d.audio->setFieldWindows != nullptr && !d.audio->windowedControls.empty()) {
		p.controlWindowVoice.assign(p.voices.size() * width * static_cast<size_t>(device::kTickWindowStride), 0.0f);
		for (size_t c = 0; c < p.voices.size() * width; ++c) {
			p.controlWindowVoice[c * static_cast<size_t>(device::kTickWindowStride) + static_cast<size_t>(device::kTickWindow)] = 1.0f;
		}
	}
}

RackEngine::Provision RackEngine::provisionVoices(const device::Descriptor& desc) const {
	Provision p;
	buildVoicesInto(p, desc, std::min(desc.params.size() + desc.traits.extraState, device::kMaxDeviceFields));
	p.desc = &desc; // so a provision that dies unadopted destructs what it built
	// THE FURNITURE'S PER-VOICE RAMPS TRAVEL WITH THE INSTANCES THEY BLEND (backlog NR), as `provision` sizes them: a
	// flip that seated voices without them left every voice of a slot-blended device (Filter, Waypoint) with no ramp,
	// so its Mix and wet Gain silently did nothing under Poly.
	if (slotBlends(desc) || engineEnable(desc)) {
		p.furnitureVoice.assign(p.voices.size(), FurnitureRamp {});
	}
	return p;
}

bool RackEngine::adoptVoices(size_t slot, Provision& p) noexcept {
	if (slot >= slots_.size() || p.voices.empty()) {
		return false;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || d != p.desc || s.storage == nullptr || !s.voices.empty()) {
		return false;
	}
	// SWAPPED, the rule every adopt here follows: the slot's empty vectors go home in the provision.
	s.voices.swap(p.voices);
	s.controlWindowVoice.swap(p.controlWindowVoice);
	s.furnitureVoice.swap(p.furnitureVoice);
	// TOLD THEIR SLOT, as `adoptDevice` tells every instance it seats.
	if (d->audio != nullptr && d->audio->setSlot != nullptr) {
		for (const auto& voice : s.voices) {
			d->audio->setSlot(voice.get(), slot);
		}
	}
	return true;
}

bool RackEngine::releaseVoices(size_t slot, Provision& p) noexcept {
	if (slot >= slots_.size() || !p.voices.empty()) {
		return false;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || d != p.desc || s.voices.empty()) {
		return false;
	}
	// **NO BLOCK IS IN FLIGHT AT A DRAIN, so no block is inside them** — and the next one reads an empty vector,
	// runs the device on the bus, and resets nothing it no longer has (`beginBlock`'s flip).
	s.voices.swap(p.voices);
	s.controlWindowVoice.swap(p.controlWindowVoice);
	s.furnitureVoice.swap(p.furnitureVoice);
	return true;
}

size_t RackEngine::adoptDevice(size_t at, Provision&& p, DeviceInstanceId wanted) {
	// **THE DRAIN'S HALF OF AN ADD: NO ALLOCATION.** Three moves and the graph's bookkeeping, which
	// is pre-sized (measured: the graph's add and remove allocate nothing). A refusal hands the
	// provision back to the caller untouched — the caller still owns it and destroys it where it may.
	if (!p.live()) {
		return kNoSlot;
	}
	// **`at` IS A POSITION IN THE CHAIN; THE STORAGE GOES WHERE THIS ENGINE LIKES** — its lowest free slot
	// (kill-lanes step 3a: the pool index never leaves the pool). A position past the end of the path is a
	// refusal; a position INTO the path is an insert, which is legal and is what a drop between two
	// devices means. And one id is one device: an id this engine already runs is refused, not doubled.
	if (at > graph().chain(0).size() || graph().chain(0).size() >= capacity()
	    || (wanted != kNoInstance && graph().laneOf(wanted) != backplane::Graph::kNoLane)) {
		return kNoSlot;
	}
	const size_t lane = lowestFreeLane();
	if (lane >= capacity() || slots_[lane].live()) {
		return kNoSlot;
	}
	const device::Descriptor* desc = p.desc;
	Slot& slot = slots_[lane];
	slot.storage = std::move(p.storage);
	slot.built = ++buildStamp_;
	// **THE HOST'S EAR, INSTALLED WHERE THE SLOT IS KNOWN** (`AudioOps::setReadingsSink`): the device's meters
	// come back through `publishFromHop` into this slot's `readings_`, which a bridge mirrors to the surfaces.
	// Every host of this engine gets meters by this one road — a chain in a lane, the standalone rack.
	if (desc->audio != nullptr && desc->audio->setReadingsSink != nullptr && lane < readingHops_.size()) {
		desc->audio->setReadingsSink(slot.storage.get(), device::ReadingsSink { &readingHops_[lane], &RackEngine::publishFromHop });
	}
	// SWAPPED, NOT ASSIGNED: a slot whose last device left keeps its rows' buffers (a `clear` frees
	// nothing), and a move-assignment over them would free them HERE — two frees on the drain,
	// measured. A swap hands them to the provision, whose owner frees them on the message thread.
	slot.base.swap(p.base);
	slot.resolved.swap(p.resolved);
	slot.resolvedVoice.swap(p.resolvedVoice);
	slot.resolvedR.swap(p.resolvedR);
	slot.resolvedVoiceR.swap(p.resolvedVoiceR);
	slot.controlWindow.swap(p.controlWindow);
	slot.controlWindowVoice.swap(p.controlWindowVoice);
	// THE OUTPUTS THE SAME WAY (2026-11-07): the new device's own count in, the last device's buffers out into the husk,
	// which the message thread frees. Every input pointing at the old ones is re-taken at this drain (`routesDirty_`).
	slot.outputs.swap(p.outputs);
	slot.outputsWindow.swap(p.outputsWindow);
	slot.outputsR.swap(p.outputsR);
	slot.outputsWindowR.swap(p.outputsWindowR);
	slot.outputsPerVoice.swap(p.outputsPerVoice);
	slot.outputsPerVoiceWindow.swap(p.outputsPerVoiceWindow);
	slot.outputsPerVoiceR.swap(p.outputsPerVoiceR);
	slot.outputsPerVoiceWindowR.swap(p.outputsPerVoiceWindowR);
	slot.anyPerVoiceInput = false;
	// THE VOICES THE SAME WAY, for the same reason: the slot's old vector (emptied by the reaper, its
	// buffer kept) goes to the provision and is freed with it, off the drain.
	slot.voices.swap(p.voices);
	slot.inputs.swap(p.inputs);
	slot.routeSpan.swap(p.routeSpan);
	// A NEW ROW, A NEW DEVICE: whatever the last full resolve stamped describes the last one (`Slot::baseSerial`). Not the
	// only guard — the provision's `resolved` starts as its `base` (`RackEngine::provision`) and the routes rebuild after an add, so
	// a known-bad without this line stays green — but the stamp then never claims a row it did not resolve.
	++slot.baseSerial;
	// THE FURNITURE'S STATE THE SAME WAY (backlog NR), and a blend that starts at rest. NO SITE until the converger
	// offers this device's: the last device's belonged to the last device.
	slot.furnitureVoice.swap(p.furnitureVoice);
	std::swap(slot.furnitureDelay, p.furnitureDelay);
	slot.furniture = FurnitureRamp {};
	slot.site.store(nullptr, std::memory_order_release);
	// A NEW NODE PUBLISHES REST until it ticks — an input compiled to point here before then reads zero,
	// which is "contributes nothing", the same answer an absent source gives.
	std::fill(slot.outputs.begin(), slot.outputs.end(), 0.0f);
	// AND THE RIGHT EAR WITH IT (backlog KU): rest in both ears, mono until the drain says otherwise.
	std::fill(slot.outputsR.begin(), slot.outputsR.end(), 0.0f);
	for (uint64_t& word : slot.stereoLive.words) {
		std::atomic_ref<uint64_t>(word).store(0, std::memory_order_relaxed);
	}
	slot.stereoLivePending = {};
	slot.emitsPair = 0;
	// AND NO HOST MODULATION until a binding resolved to it says so (host slots, T5): the last device's
	// offset belonged to the last device.
	slot.hostMod.fill(0.0f);
	// A NEW DEVICE HAS NO ARRANGEMENT YET. Left standing, the last device's `polyLastBlock` would make
	// this one's first block look like a flip and reset instances that were just prepared — harmless —
	// or, worse, look like no flip when there was one.
	slot.polyThisBlock = false;
	slot.polyLastBlock = false;
	slot.tickAccum = 0;
	// AND NO METER until it has run (backlog MK): the last device's level belonged to the last device.
	slot.outputChunkPeak = 0.0f;
	slot.outputHeld = 0.0f;
	slot.outputHoldLeft = 0;
	slot.outputPeak.store(-1.0f, std::memory_order_relaxed);
	// PUBLISHED LAST, WHEN EVERYTHING BEHIND THE POINTER IS READY — see `Slot::desc`.
	slot.desc.store(desc, std::memory_order_release);

	// THE RECORD'S ID WHEN THERE IS ONE — see the declaration.
	const DeviceInstanceId id = wanted != kNoInstance ? wanted : ids_.issue();
	if (!graph().restore(*desc, id, lane) || !graph().insert(0, at, id)) {
		// UNWOUND WITHOUT A FREE: the provision goes back to the caller, exactly as it came.
		slot.desc.store(nullptr, std::memory_order_seq_cst);
		p.storage = std::move(slot.storage);
		p.base = std::move(slot.base);
		p.resolved = std::move(slot.resolved);
		p.resolvedVoice = std::move(slot.resolvedVoice);
		p.resolvedR = std::move(slot.resolvedR);
		p.resolvedVoiceR = std::move(slot.resolvedVoiceR);
		p.controlWindow = std::move(slot.controlWindow);
		p.controlWindowVoice = std::move(slot.controlWindowVoice);
		p.outputs.swap(slot.outputs);
		p.outputsWindow.swap(slot.outputsWindow);
		p.outputsR.swap(slot.outputsR);
		p.outputsWindowR.swap(slot.outputsWindowR);
		p.outputsPerVoice.swap(slot.outputsPerVoice);
		p.outputsPerVoiceWindow.swap(slot.outputsPerVoiceWindow);
		p.outputsPerVoiceR.swap(slot.outputsPerVoiceR);
		p.outputsPerVoiceWindowR.swap(slot.outputsPerVoiceWindowR);
		p.voices.swap(slot.voices);
		p.inputs.swap(slot.inputs);
		p.routeSpan.swap(slot.routeSpan);
		p.furnitureVoice.swap(slot.furnitureVoice);
		std::swap(p.furnitureDelay, slot.furnitureDelay);
		p.desc = desc;
		return kNoSlot;
	}
	slot.id = id;
	++layoutRevision_;
	// **TOLD ITS SLOT, NOT ITS POSITION — and told it ONCE, for the device's whole life**, now that the
	// engine has chosen it. A device publishes its meters against this number and `publishReadings` is
	// indexed by it. A plain setter (`AudioOps::setSlot`'s contract), so the drain may call it.
	// **A RACK INSIDE THIS ONE IS HANDED THE HOST OUTSIDE** (rack roadmap §7): how to read this engine's
	// sources, and whether one is upstream of it. Told once, at seating, like its slot — the reader
	// looks cells up by endpoint on every call, so nothing in it goes stale while this engine stands.
	if (desc->audio != nullptr && desc->audio->setOuterSources != nullptr) {
		desc->audio->setOuterSources(slot.storage.get(), outerSourcesFor(id));
	}
	if (desc->audio != nullptr && desc->audio->setSlot != nullptr) {
		desc->audio->setSlot(slot.storage.get(), lane);
		for (const auto& voice : slot.voices) {
			if (voice != nullptr) {
				desc->audio->setSlot(voice.get(), lane); // every instance was told, as the build told them
			}
		}
	}
	publishOrder();
	// A NEW NODE CHANGES NOTHING ROUTED TODAY, and the list is republished anyway: the lane's macro
	// cells are read by lane, and the next publish must see the slot's id to walk its edges.
	routesDirty_ = true;
	// **AND EVERY CONTAINER HERE RE-TAKES ITS OUTER SOURCES** (2026-11-07, per-device output counts): an inner engine reads
	// this engine's output cells through pointers it took at its own drain (`setOuterSources` marks it dirty), and this
	// install may have swapped a slot's output buffers out from under one of them. Re-handing the reader makes each inner
	// engine re-take at its next block, inside this one, before the husk holding the old buffers can be freed.
	for (size_t other = 0; other < slots_.size(); ++other) {
		Slot& s = slots_[other];
		const device::Descriptor* d = other == lane ? nullptr : s.live_desc();
		if (d != nullptr && s.storage != nullptr && d->audio != nullptr && d->audio->setOuterSources != nullptr) {
			d->audio->setOuterSources(s.storage.get(), outerSourcesFor(s.id));
		}
	}

	if (connector_) {
		connector_(desc->traits.tag, lane, slot.storage.get());
	}
	return lane;
}

size_t RackEngine::addDeviceAt(size_t at, const char* tag) {
	// PROVISION AND ADOPT IN ONE CALL — for callers that are not the drain: cases, and the inactive
	// patch road. The drain provisions on the message thread and adopts here; see `RackBridge`. The slot is
	// the engine's lowest free one (`adoptDevice`), and the id the engine's own: there is no record here.
	Provision p = provision(tag);
	const size_t placed = adoptDevice(at, std::move(p));
	// A REFUSED PROVISION DIES HERE, on the caller's thread — which for this road is never the drain.
	return placed;
}

size_t RackEngine::addDevice(const char* tag) {
	// **AT THE END OF THE CHAIN, WHICH IS NOW A ONE-LINE STATEMENT.**
	//
	// The old fix walked `slots_`
	// for the last live index and appended after it, plus a fallback that took the first hole when
	// the tail was full — twenty lines reconstructing an order from an array that did not hold one.
	//
	// The graph holds the order, so "the end" is `chainLength`. **AND THE LANE IS ALLOWED TO BE THE
	// HOLE**: `addDeviceAt` takes the lowest free one, so a device added after a removal reuses the
	// freed lane while going to the END of the chain. Those are different questions now and only the
	// second was ever what was seen — a device appearing in the middle of the ORDER. Reusing the lane
	// is what keeps the route table dense, and the new device's columns were cleared when the old one
	// left.
	return addDeviceAt(graph().chain(0).size(), tag);
}

bool RackEngine::removeDeviceAtLane(size_t slot) {
	if (slot >= slots_.size()) {
		return true;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* going = s.live_desc();
	if (going == nullptr) {
		return true; // nothing there; nothing to do, and not a refusal
	}
	// **ROOM IN THE PARKING FIRST, OR NOTHING HAPPENS.** Destruction frees, this runs on the drain,
	// and the parking is the only place the device can go (backlog CF). A full parking refuses the
	// remove outright — the device stays live, the caller says so — rather than block for the reaper,
	// drop the storage, or free it here. Checked before a single word is changed, so a refusal is
	// indistinguishable from the remove never having been asked.
	if (retiredCount() >= retired_.size()) {
		return false;
	}
	// **THE ROUTES ARE CLEARED ONCE, BY THE GRAPH.** `graph().remove` below writes the lane's empty slice
	// itself, and every live slot has a graph id (`adoptDeviceAt` sets it or unwinds), so an explicit
	// `clearDeviceSlotRoutes` here cleared the same lane twice — MEASURED on the 16-device load drain,
	// alternated at matched load: 138-160 us with it, 82-87 us without. The ABA guard the double clear
	// used to spell out — a reissued lane inherits no amounts — is the graph's clear, and the cases
	// that pin it are unchanged.

	// UNPUBLISHED FIRST, WITH SEQ_CST — see `blockEpoch_`. This stops any block that has not started
	// yet; the epoch stamped on the parked entry is what deals with one already running: the reaper
	// destructs only once the epoch has moved past it.
	s.desc.store(nullptr, std::memory_order_seq_cst);
	// ITS SITE IS LET GO, NOT FREED (backlog NR): the converger owns it and offers it to whatever slot holds the device
	// next — a replacement under the same id, or nothing.
	s.site.store(nullptr, std::memory_order_seq_cst);
	++layoutRevision_;
	if (s.id != feathers::kNoInstance) {
		graph().remove(s.id);
		s.id = feathers::kNoInstance;
		publishOrder();
		routesDirty_ = true; // the graph cleared every route into or out of this lane
	}
	// THE READINGS GO WITH THE DEVICE. A meter left standing after its device is removed is a picture
	// of something that is not there — and the next device in this slot would inherit it.
	std::atomic_ref<size_t>(readings_[slot].count).store(0, std::memory_order_relaxed);
	// PARKED, ALWAYS — never destroyed here, whether or not a block is in flight. The rows keep their
	// capacity (a `clear` frees nothing) so the next device on this lane adopts into them.
	park(s, going);
	s.base.clear();
	s.resolved.clear();
	s.resolvedVoice.clear();
	s.resolvedR.clear();
	s.resolvedVoiceR.clear();
	s.anyPerVoiceInput = false;
	s.inputs.clear();
	s.routeSpan.clear();
	// **THE OUTPUTS GO TO REST, NOT AWAY.** A downstream node's input may still point at these cells
	// until the drain rebuilds every input (`routesDirty_` is set above); a block in flight between
	// here and there must read a number, and zero is the one that contributes nothing. Atomic stores
	// because that block is reading them on another thread.
	for (float& cell : s.outputs) {
		std::atomic_ref<float>(cell).store(0.0f, std::memory_order_relaxed);
	}
	return true;
}

bool RackEngine::park(Slot& slot, const device::Descriptor* desc) noexcept {
	const size_t tail = retiredTail_.load(std::memory_order_relaxed);
	if (tail - retiredHead_.load(std::memory_order_acquire) >= retired_.size()) {
		return false;
	}
	Retired& r = retired_[tail % retired_.size()];
	r.storage = std::move(slot.storage);
	// SWAPPED: the entry's vector was emptied by the reaper and keeps its buffer, so this frees nothing
	// on the drain, and the slot is left holding an empty vector for its next device to swap into.
	r.voices.swap(slot.voices);
	r.desc = desc;
	r.epoch = blockEpoch_.load(std::memory_order_seq_cst);
	retiredTail_.store(tail + 1, std::memory_order_release);
	return true;
}

void RackEngine::reapRetired() noexcept {
	(void)reapGrowth(); // and a graph a growth replaced (unbounded devices step 4)
	// **THE MESSAGE THREAD'S HALF OF A REMOVE: destruct and free, where freeing is allowed.** One
	// consumer. An entry parked while a block was in flight (odd epoch) is left until the epoch has
	// moved past it — that block may have been inside the device — and everything behind it waits
	// too, because the ring is in order. Called from the bridge on the message thread, from
	// `prepare`, and from the destructor.
	size_t head = retiredHead_.load(std::memory_order_relaxed);
	const size_t tail = retiredTail_.load(std::memory_order_acquire);
	const uint64_t now = blockEpoch_.load(std::memory_order_seq_cst);
	for (; head != tail; ++head) {
		Retired& r = retired_[head % retired_.size()];
		if ((r.epoch & 1u) != 0u && now <= r.epoch) {
			break;
		}
		// HELD BY AN EDITOR OVER ITS INSIDE — see `setReapHold`. Waits like an entry a block may be in.
		if (reapHold_ != nullptr && r.storage != nullptr && reapHold_(reapHoldContext_, r.storage.get())) {
			break;
		}
		if (r.desc != nullptr) {
			if (r.storage != nullptr) {
				destructWith(*r.desc, r.storage.get());
			}
			for (auto& voice : r.voices) {
				if (voice != nullptr) {
					destructWith(*r.desc, voice.get());
				}
			}
		}
		r.storage.reset();
		// CLEARED, NOT SHRUNK: the buffer stays with the entry so the next `park` swaps without a free.
		r.voices.clear();
		r.desc = nullptr;
		retiredHead_.store(head + 1, std::memory_order_release);
	}
}

void RackEngine::prepare(double sampleRate, int maxFrames) {
	reapRetired();

	sampleRate_ = sampleRate;
	maxFrames_ = maxFrames;
	++prepares_;
	// THE FURNITURE'S SCRATCH AND RAMP (backlog NR), sized here and nowhere else.
	const auto scratch = static_cast<size_t>(device::scratchFrames(maxFrames));
	furnitureDryL_.assign(scratch, 0.0f);
	furnitureDryR_.assign(scratch, 0.0f);
	furnitureRampSamples_ = furnitureRampFor(sampleRate);
	// THE NOTE HISTORY A LATE DEVICE READS (backlog LW), sized here and nowhere else.
	noteHistory_.assign(kNoteHistory, NoteMoment {});
	historyHead_ = 0;
	historyCount_ = 0;
	// AND THE WRITES A LATE DEVICE'S ROW WAITS FOR (step 3) — any still waiting land first, early rather than lost.
	if (pendingCount_ > 0) {
		landDueFields(true);
	}
	pendingFields_.assign(kPendingFields, PendingField {});
	pendingCount_ = 0;
	// AND THE RINGS A LATE DEVICE READS ITS SOURCES THROUGH (step 4): one second of control ticks each, JM's ceiling.
	ringLen_ = static_cast<size_t>(std::ceil(device::kMaxLatencySeconds * std::max(sampleRate, 1.0) / device::kTickWindow)) + 2;
	modRings_.assign(kModRings, ModRing {});
	modRingBuf_.assign(kModRings * ringLen_ * 2, 0.0f);
	sizeVoiceDelays();
	routesDirty_ = true; // any ring the inputs held is gone; they re-take them at the next drain
	for (Slot& slot : slots_) {
		if (slot.isAudio()) {
			slot.live_desc()->audio->prepare(slot.storage.get(), sampleRate, maxFrames);
			for (auto& voice : slot.voices) {
				slot.live_desc()->audio->prepare(voice.get(), sampleRate, maxFrames);
			}
			// AND THE FURNITURE'S DRY DELAY, at the rate and to what the device declares now.
			if (slot.furnitureDelay.capacity() > 0) {
				slot.furnitureDelay.prepare(sampleRate, latencyOf(slot, *slot.live_desc(), sampleRate));
			}
		}
		else if (slot.isMod()) {
			slot.live_desc()->mod->prepare(slot.storage.get(), sampleRate);
		}
	}
}

void RackEngine::setMacro(size_t index, float value) {
	// A MACRO'S STORED VALUE IS 0..1 IN EITHER POLARITY (the bipolar cell is derived from it below), so a number
	// a host or a file hands past that is held to the ends, and one that is not a number is not a write: the
	// macro keeps what it had, since NaN would poison every route that reads the cell.
	if (!std::isfinite(value)) {
		return;
	}
	value = std::clamp(value, 0.0f, 1.0f);
	if (index < macros_.size()) {
		macroRaw_[index] = value;
		// THE CELL IS THE VALUE READ THROUGH ITS POLARITY (backlog EG) — derived here, at the write, so
		// every reader of the cell is one pointer load with nothing to decide.
		macros_[index] = macroBipolar_[index] != 0 ? 2.0f * value - 1.0f : value;
	}
}

float RackEngine::macro(size_t index) const {
	return index < macroRaw_.size() ? macroRaw_[index] : 0.0f;
}

void RackEngine::setMacroBipolar(size_t index, bool bipolar) {
	if (index < macros_.size()) {
		macroBipolar_[index] = bipolar ? 1 : 0;
		setMacro(index, macroRaw_[index]); // the cell follows the mode at once
	}
}

bool RackEngine::macroBipolar(size_t index) const {
	return index < macroBipolar_.size() && macroBipolar_[index] != 0;
}

device::MacroType RackEngine::macroType(size_t index) const {
	return index < macroType_.size() ? macroType_[index] : device::MacroType::Knob;
}

void RackEngine::setMacroType(size_t index, device::MacroType type) {
	// THE RACK HAS NO RELATIVE ROAD YET (backlog PQ): Relative is refused like a type past the last.
	if (index < macroType_.size() && device::isAbsoluteMacroType(type)) {
		macroType_[index] = type;
	}
}

std::string RackEngine::macroName(size_t index) const {
	return macroRenamed(index) ? macroName_[index] : device::defaultMacroName(index);
}

bool RackEngine::macroRenamed(size_t index) const {
	return index < macroName_.size() && !macroName_[index].empty();
}

void RackEngine::setMacroName(size_t index, std::string_view name) {
	if (index < macroName_.size()) {
		macroName_[index] = device::normalisedMacroName(index, name);
	}
}

void RackEngine::setGate(bool gate) {
	// THE EDGE IS DERIVED, never asked for — see the header. It is consumed by the next tick and
	// cleared there, so it is true for exactly one tick however many blocks pass before one happens.
	if (gate && !gate_) {
		retrigger_ = true;
	}
	gate_ = gate;
}

void RackEngine::retrigger() {
	// THE SAME LATCH THE EDGE SETS, so a note under a held gate and the first note of a phrase reach
	// the modulators by one road — see the header.
	retrigger_ = true;
}


void RackEngine::advanceModulators(int frames, const backplane::BlockContext& block) {
	device::ModTickContext ctx;
	ctx.gate = gate_;
	ctx.clock = block.clock;
	// **IN CHAIN ORDER, LIKE THE OTHER TWO PASSES.** A modulator's output does not feed the next one
	// today, so lane order would sound identical — which is exactly why it is worth doing rather than
	// arguing about: `graph.h`'s contract is *"first every chain's members with `mod` ops, in path
	// order"*, and a walk that is right only while nothing depends on it is a walk that breaks
	// silently the day something does.
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
		const size_t lane = path.at(step);
		if (lane >= slots_.size()) {
			continue;
		}
		Slot& slot = slots_[lane];
		// ONE LOAD OF `desc`, HELD FOR THE WHOLE SLOT. `isAudio()`/`isMod()` load the atomic and then
		// every `live_desc()` after them loads it AGAIN — and the editor can clear it in between, so
		// the check said "yes" and the dereference got a nullptr. That is what crashed the removal
		// case with SIGSEGV, and it is CLAUDE.md's pointer rule read from the other end: comparing two
		// pointers read in the SAME pass is fine, and this was two passes pretending to be one.
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || slot.storage == nullptr || d->mod == nullptr) {
			continue;
		}
		// SETTINGS FIRST — a modulator is configured by the same row an audio device is, and the same
		// resolve applies, so a routed modulator control is modulated exactly like any other.
		resolveInto(slot, *d);
		d->mod->setFields(slot.storage.get(), slot.resolved.data(), slot.resolved.size());
		// THE RIGHT EAR, only while a stereo route reaches one of this modulator's controls (backlog KU). A device
		// whose `setFields` was just called is mono until told otherwise, so silence here keeps it mono.
		if (slot.stereoLive.any() && d->mod->setFieldsR != nullptr) {
			d->mod->setFieldsR(slot.storage.get(), slot.resolvedR.data(), slot.resolvedR.size(), slot.stereoLive);
		}
		// **A DUAL DEVICE RUNNING IN THE VOICES PUBLISHES FROM THE VOICES** (backlog NA (b); env-follower's Tap rides the
		// same road). A device with an audio road AND a mod road, Poly on, in a rack with voices, sounds in
		// `slot.voices` — the instance in `slot.storage` makes no sound there, so what it would publish is about nothing
		// anyone hears. Each voice instance's mod road is set and ticked instead, with that voice's own row, and
		// `outputForNoteAt` reads the instance holding the note. A destination with no voice
		// reads the NEWEST note's voice (`newestVoice`), the rule every per-voice modulator uses (`PerNoteTable::newest`).
		const bool fromVoices = publishesFromVoices(slot, *d);
		if (fromVoices) {
			for (size_t v = 0; v < slot.voices.size(); ++v) {
				if (slot.anyPerVoiceInput) {
					resolveVoiceInto(slot, *d, v);
					d->mod->setFields(slot.voices[v].get(), slot.resolvedVoice.data(), slot.resolvedVoice.size());
				}
				else {
					d->mod->setFields(slot.voices[v].get(), slot.resolved.data(), slot.resolved.size());
				}
			}
		}

		// **AND A WAY TO RESOLVE ONE OF ITS CONTROLS FOR A NOTE** (backlog MV, B), only in a rack that has voices — see
		// `device::ModTickContext::perNote`. Built per slot, on the stack, for this slot's ticks alone.
		const NoteResolve noteResolve { this, &slot, d };
		const device::ModTickContext::PerNoteResolver perNote { &noteResolve, &RackEngine::resolveForNote };
		ctx.perNote = voices_ > 0 ? &perNote : nullptr;

		const size_t period = d->mod->blockSamples > 0 ? d->mod->blockSamples : 1;
		slot.tickAccum += static_cast<size_t>(frames);
		bool ticked = false; // a publish without a tick is not a tick's window
		while (slot.tickAccum >= period) {
			ticked = true;
			slot.tickAccum -= period;
			// THE EDGE IS SPENT BY THE FIRST TICK THAT SEES IT, and by every modulator that ticks in
			// this block — a retrigger is one fact about the host, not one per device.
			ctx.retrigger = retrigger_;
			if (fromVoices) {
				for (auto& voice : slot.voices) {
					d->mod->tick(voice.get(), ctx);
				}
			}
			else {
				d->mod->tick(slot.storage.get(), ctx);
			}
			ctx.retrigger = false;
		}
		ctx.perNote = nullptr; // it points into this iteration's stack
		if (fromVoices && d->mod->trigFires != nullptr) {
			latchVoiceTrigFires(slot, *d);
		}
		// **THE NODE PUBLISHES ITS OUTPUTS, HERE, AFTER ITS TICK** — into the cells every downstream input
		// points at. In chain order, so a modulator later in the walk resolves against this reading in the
		// same pass: pass-through along an edge (edges doc §20), which the graph's order is FOR.
		//
		// **A DISABLED MODULATOR PUBLISHES ITS REST VALUE, WHICH IS WHAT MAKES THE SWITCH HONEST.** A
		// disable button was asked for on the modulator heads; declaring `enableControl` draws the square, and
		// this is the half that makes it change sound — without it the switch would move a field nothing
		// reads. REST, NOT "OFF AS ZERO AT THE DESTINATION": modulation is additive, so a source reading 0
		// contributes nothing and every destination sits at its own base; a bipolar MSEG's rest is its
		// centre and forcing a destination to zero would be a different sound rather than an absence of
		// one. The device keeps TICKING while switched off, so switching back on lands where the music is
		// rather than where it left. This rule lived in `sourceValue`, the read side; it lives on the write
		// side now, once, so `outputAt`, the editor and every input read the one published number.
		const int enable = d->traits.enableControl;
		const bool disabled = enable >= 0 && static_cast<size_t>(enable) < slot.resolved.size()
		                      && slot.resolved[static_cast<size_t>(enable)] < 0.5f;
		// **THE AMOUNT, APPLIED HERE AND NOWHERE ELSE** (backlog DH): a modulator that names one
		// (`Traits::amountControl`) has every output scaled by its RESOLVED value — so the knob is
		// itself a destination, and a macro riding it rides every route — at the one publish every
		// reader takes. Unity for a device that names none.
		const float amount = amountOf(*d, slot.resolved);
		for (size_t output = 0; output < d->outputs.size() && output < slot.outputs.size(); ++output) {
			// FROM THE VOICES: the newest note's, or rest before any voice has held one.
			const size_t newest = fromVoices ? newestVoice() : 0;
			void* const source = !fromVoices ? slot.storage.get()
			                     : newest < slot.voices.size() ? slot.voices[newest].get() : nullptr;
			const float raw = disabled || source == nullptr || d->mod->outputAt == nullptr
			                      ? 0.0f
			                      : amount * d->mod->outputAt(source, output);
			// FINITE ONLY — the last gate before the number becomes an offset on every destination it
			// reaches. `ParamSet::setDeviceSource` kept this rule when the cell was its table's; the cell
			// is the node's now, so the rule is the publish's.
			const float published = std::isfinite(raw) ? raw : 0.0f;
			// THE TICK AS A WINDOW, from the last publish to this one — ONLY WHEN IT TICKED. A zero-frame
			// pass (a lane re-begun mid-render) republishes the same value and would flatten the tick's window; a
			// value that changes WITHOUT a tick (the Amount knob, the enable switch) holds flat at its new value.
			{
				float* win = &slot.outputsWindow[output * static_cast<size_t>(device::kTickWindowStride)];
				if (ticked) {
					device::fillTickWindowRamp(win, slot.outputs[output], published);
				}
				else if (published != slot.outputs[output]) {
					device::fillTickWindowRamp(win, published, published);
				}
			}
			// THE RIGHT EAR (backlog KU): a pair's own right value when this output is a pair this block, the left
			// copied otherwise — so a mono source costs one store here and the resolve reads one cell either way.
			float publishedR = published;
			if (!disabled && d->mod->outputPairAt != nullptr && ((slot.emitsPair >> output) & 1u) != 0) {
				const float rawR = amount * d->mod->outputPairAt(slot.storage.get(), output).right;
				publishedR = std::isfinite(rawR) ? rawR : 0.0f;
			}
			// AND ITS TICK WINDOW, by the left window's rule from the right ear's last value — so a mono output's
			// right window is its left one, and a pair ramps its right ear per sample as its left ramps (2026-10-05).
			{
				float* winR = &slot.outputsWindowR[output * static_cast<size_t>(device::kTickWindowStride)];
				if (ticked) {
					device::fillTickWindowRamp(winR, slot.outputsR[output], publishedR);
				}
				else if (publishedR != slot.outputsR[output]) {
					device::fillTickWindowRamp(winR, publishedR, publishedR);
				}
			}
			std::atomic_ref<float>(slot.outputs[output]).store(published, std::memory_order_relaxed);
			std::atomic_ref<float>(slot.outputsR[output]).store(publishedR, std::memory_order_relaxed);
		}
		}
	}
	retrigger_ = false;
	// AND THE SOURCES READ LATE ONE TICK ON, now that this tick's values are out (backlog LW step 4).
	tickModRings(frames);
}

const Slot& RackEngine::slotAtLane(size_t lane) const {
	static const Slot kEmpty;
	return lane < slots_.size() ? slots_[lane] : kEmpty;
}

size_t RackEngine::deviceAtPosition(size_t position, size_t chain) const noexcept {
	// ASKED OF THE GRAPH, WHICH OWNS THE ORDER. Message thread — a caller on the audio thread walks
	// the published snapshot instead, which is what `processStereo` does.
	const std::span<const DeviceInstanceId> path = graph().chain(chain);
	if (position >= path.size()) {
		return kNoSlot;
	}
	const size_t lane = graph().laneOf(path[position]);
	return lane == backplane::Graph::kNoLane ? kNoSlot : lane;
}

size_t RackEngine::positionOfLane(size_t lane) const noexcept {
	if (lane >= slots_.size() || !slots_[lane].live()) {
		return kNoSlot;
	}
	const size_t position = graph().positionOf(slots_[lane].id);
	return position == backplane::Graph::kNoPosition ? kNoSlot : position;
}

size_t RackEngine::chainLength(size_t chain) const noexcept {
	return graph().chain(chain).size();
}

void RackEngine::publishOrder() {
	// THE BACK BUFFER, FILLED THEN FLIPPED — see the declaration on why a single shared array would
	// let the audio thread read an order naming one lane twice.
	const size_t back = 1 - orderFront_.load(std::memory_order_relaxed);
	OrderSnapshot& snap = orderSnapshot_[back];
	// **EACH CHAIN'S MEMBERS IN THE GRAPH'S ORDER, NOT THE PATH'S (backlog IZ).** The two were the same
	// order while a chain ordered its consecutive modulators: every legal route ran forward along the
	// path. Since a modulator's place in a lane is only grouping, a route may run from a modulator into
	// one to its LEFT, and `order()` ticks the source first where the path would tick it a block late.
	// Where nothing is routed against the path the graph's tie-break IS the path (`recompute`), so a
	// host that converges this engine to a record's order — every product road — sees no change.
	// Message thread: `order()` is read here and copied, never handed across.
	const std::span<const backplane::Step> steps = graph().order();
	for (size_t c = 0; c < snap.chain.size() && c < graph().chainCount(); ++c) {
		ChainOrder& out = snap.chain[c];
		size_t n = 0;
		for (const backplane::Step& step : steps) {
			const size_t lane = step.lane;
			// ONLY THIS CHAIN'S MEMBERS — a detached node, or another chain's, is not walked here.
			// A NODE WITH NO LANE IS NOT WALKABLE and is skipped rather than written as a sentinel:
			// the audio thread reads `count` entries and every one of them must be a lane it can index.
			if (graph().chainOf(step.id) != c || lane == backplane::Graph::kNoLane || lane >= graph().capacity()
			    || lane >= out.lane->capacity()) {
				continue;
			}
			(*out.lane)[n] = static_cast<uint32_t>(lane);
			++n;
		}
		out.count = n;
		// AND THE PATH, IN THE CARDS' ORDER — what "upstream" means for the bus note (`fillBusNotes`).
		size_t p = 0;
		for (const DeviceInstanceId id : graph().chain(c)) {
			const size_t lane = graph().laneOf(id);
			if (lane == backplane::Graph::kNoLane || lane >= graph().capacity() || p >= out.path->capacity()) {
				continue;
			}
			(*out.path)[p++] = static_cast<uint32_t>(lane);
		}
		out.pathCount = p;
	}
	// RELEASE, so everything written above is visible to the thread that sees the new index.
	orderFront_.store(back, std::memory_order_release);
}

backplane::SlotView RackEngine::slots() const noexcept {
	// THE SLOTS' OWN DESCRIPTORS — `live_desc`, what the audio thread reads — while the graph's `descAtLane_` is
	// what the order is computed over, and both are written by the same three verbs. (A mirror array stood
	// here; the slots live in a chunked pool now, which has no span to hand out, so the view asks a slot.)
	//
	// IT USED TO HAND THE GRAPH'S RANK SPAN ACROSS TOO, so that a `mayRoute` over flat coordinates
	// could compare two ranks. That overload and `SlotView`'s rank half both went in kill-lanes (4b,
	// then 5): the order is still the graph's and still has one home in `Graph::recompute()`, but it
	// is the order the engine RUNS in rather than the rule that says what may route. That rule is the
	// `DeviceView` the record hands out (`ChainRecord::devices()`, whose `wouldCycle` is the graph's).
	return backplane::SlotView(
	    this,
	    [](const void* host, size_t lane) -> const device::Descriptor* {
		    const auto* engine = static_cast<const RackEngine*>(host);
		    const Slot* s = engine->slots_.at(lane);
		    return s != nullptr ? s->live_desc() : nullptr;
	    },
	    capacity());
}

float RackEngine::fieldAt(size_t slot, size_t control) const {
	const Slot& s = slotAtLane(slot);
	return control < s.base.size() ? s.base[control] : 0.0f;
}

void RackEngine::setField(size_t slot, size_t control, float value) {
	if (slot >= slots_.size()) {
		return;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || control >= s.base.size()) {
		return;
	}
	// A FLOAT PAST THE PARAMETERS IS ROW STATE, NOT A CONTROL — it has no declaration to coerce it
	// against, and the device that owns it is the only party that knows what it means. Stored as given.
	if (control >= d->params.size()) {
		if (std::bit_cast<uint32_t>(s.base[control]) != std::bit_cast<uint32_t>(value)) {
			++s.baseSerial; // resolved as a copy of the base (`Slot::baseSerial`)
		}
		s.base[control] = value;
		return;
	}
	// COERCED AGAINST THE CONTROL'S OWN DECLARATION — `device::coerceControl`, the rule a record holding
	// this row applies too, so the editor draws the number this uses.
	// **A HAND'S EDIT LANDS AT ONCE, LATE DEVICE OR NOT** (backlog LW step 3): a DAW does not delay a knob
	// turned on a plugin behind a latent one either, and this road carries the record whole — a preset, a load, a drain.
	// What waits its turn is the HOST's automation (`applyHostSlots`), which is written against the timeline.
	// A HOST WRITE STILL WAITING ITS TURN ON THIS CONTROL WINS over the row a drain writes back — the host-slot rule
	// (plan §0.3: a drain adopting a record older than a host write cannot revert it), kept while the write waits.
	if (s.pendingFields > 0 && hasPendingFor(slot, control)) {
		return;
	}
	writeBaseControl(s, *d, control, device::coerceControl(d->params[control], value));
}

bool RackEngine::hasPendingFor(size_t lane, size_t control) const noexcept {
	for (size_t k = 0; k < pendingCount_; ++k) {
		if (pendingFields_[k].lane == lane && pendingFields_[k].control == control) {
			return true;
		}
	}
	return false;
}

void RackEngine::writeBaseControl(Slot& s, const device::Descriptor& d, size_t control, float coerced) noexcept {
	if (control >= s.base.size()) {
		return;
	}
	const bool wasZero = s.base[control] == 0.0f;
	// THE ROW'S NEXT RESOLVE IS A WHOLE ONE only when this changed it (`Slot::baseSerial`): the converger writes every
	// control at every drain, and the same bits are no change. Compared as bits, so −0 over +0 is one.
	if (std::bit_cast<uint32_t>(s.base[control]) != std::bit_cast<uint32_t>(coerced)) {
		++s.baseSerial;
	}
	s.base[control] = coerced;
	// A CONTROL THAT DECIDES WHETHER AN OUTPUT IS A PAIR (`SourceDesc::pairWhile`, backlog PA) re-runs the drain when it
	// crosses zero, so the stereo-live fixed point follows the knob.
	if (wasZero != (s.base[control] == 0.0f) && pairsFollow(d, control)) {
		routesDirty_ = true;
	}
	// A ROW THAT DECIDES WHAT ITS DEVICE MAY REACH (backlog OP, the Follower's Mode) tells this engine's own graph, which
	// deletes the routes the answer forbids — the record's rule, for an engine whose routes are its graph's. A host
	// that owns the routes (`setHostRoutes`) pruned them in its record already; this graph then holds none to delete.
	if (d.mod != nullptr && d.mod->readsAudioOf != nullptr) {
		const std::optional<size_t> cleared =
		    graph().setReadsAudio(s.id, device::modReadsAudio(d, s.base.data(), s.base.size()));
		if (cleared.has_value() && *cleared > 0) {
			routesDirty_ = true;
		}
	}
}

bool RackEngine::deferField(Slot& s, size_t lane, const device::Descriptor& d, size_t control, float coerced) noexcept {
	// ONLY A LATE AUDIO DEVICE'S CONTROLS WAIT: a modulator runs in its own domain (its output follows through the rings,
	// step 4), and with nothing late nothing waits — the row is written as it always was.
	if (maxArrival_ == 0 || d.audio == nullptr || lane >= arrival_->capacity() || (*arrival_)[lane].at == 0
	    || control >= s.base.size() || pendingFields_.empty()) {
		return false;
	}
	// WHAT THE ROW WILL HOLD ONCE EVERYTHING WAITING HAS LANDED — the newest write waiting for this control, or the row.
	// A host that writes every control at every drain (the converger) then queues only what changed.
	float target = s.base[control];
	if (s.pendingFields > 0) {
		for (size_t k = pendingCount_; k-- > 0;) {
			const PendingField& p = pendingFields_[k];
			if (p.lane == lane && p.control == control && p.id == s.id) {
				target = p.value;
				break;
			}
		}
	}
	if (target == coerced) {
		return true; // nothing new: what is waiting (or the row) already says it
	}
	if (pendingCount_ == pendingFields_.size()) {
		return false; // FULL: written now, early rather than lost
	}
	pendingFields_[pendingCount_++] = PendingField { busClock_ + (*arrival_)[lane].at, static_cast<uint32_t>(lane),
		                                             static_cast<uint32_t>(control), s.id, coerced };
	++s.pendingFields;
	++lateWork_;
	return true;
}

void RackEngine::landDueFields(bool all) noexcept {
	if (pendingCount_ == 0) {
		return;
	}
	// IN THE ORDER THEY WERE MADE, each when its turn comes — a device's own `A` decides that, so a later write for an
	// earlier device may land first and the queue is compacted around what still waits.
	size_t kept = 0;
	for (size_t k = 0; k < pendingCount_; ++k) {
		const PendingField p = pendingFields_[k];
		Slot* s = p.lane < slots_.size() ? &slots_[p.lane] : nullptr;
		const device::Descriptor* d = s != nullptr ? s->live_desc() : nullptr;
		const bool mine = d != nullptr && s->id == p.id;
		// NO LONGER LATE (a latency that fell to zero, or nothing late at all): its writes land now, not a delay later.
		const bool stillLate = mine && maxArrival_ > 0 && p.lane < arrival_->capacity() && (*arrival_)[p.lane].at > 0;
		if (all || !mine || !stillLate || p.due <= busClock_) {
			if (mine) {
				writeBaseControl(*s, *d, p.control, p.value);
				if (s->pendingFields > 0) {
					--s->pendingFields;
				}
			}
			continue;
		}
		pendingFields_[kept++] = p;
	}
	pendingCount_ = kept;
}

size_t RackEngine::fieldsPresentAt(size_t slot) const {
	return slotAtLane(slot).base.size();
}

bool RackEngine::playheadAt(size_t slot, uint32_t& ordinal, float& phase) const {
	if (slot >= slots_.size()) {
		return false;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	// A SLOT THAT IS NOT A LIVE MODULATOR HAS NO POSITION, and neither has one whose device declines
	// to offer the accessor. **DECLINING LEAVES THE OUT-PARAMETERS UNTOUCHED**, which is what lets a
	// caller seed them with idle and trust the seed — `MsegCard` depends on exactly that, because
	// ordinal 0 is a valid segment for an MSEG and would draw a marker rather than none.
	if (d == nullptr || d->mod == nullptr || d->mod->playhead == nullptr || s.storage == nullptr) {
		return false;
	}
	d->mod->playhead(s.storage.get(), &ordinal, &phase);
	return true;
}

uint64_t RackEngine::extraStateAt(size_t slot, float* extra, size_t capacity, size_t* count) const {
	*count = 0;
	if (slot >= slots_.size()) {
		return 0;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	if (d == nullptr || d->mod == nullptr || d->mod->publishExtraState == nullptr || s.storage == nullptr) {
		return 0;
	}
	const uint64_t serial = d->mod->publishExtraState(s.storage.get(), extra, capacity);
	*count = serial != 0 ? std::min(capacity, d->traits.extraState) : 0;
	return serial;
}

bool RackEngine::trigFiresAt(size_t slot, uint32_t& fires) const {
	if (slot >= slots_.size()) {
		return false;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	if (d == nullptr || d->mod == nullptr || d->mod->trigFires == nullptr || s.storage == nullptr) {
		return false;
	}
	// **A DEVICE PUBLISHING FROM ITS VOICES IS READ WHERE ITS PUBLISH IS** (`playheadLevelAt`'s rule): the instance in
	// `s.storage` hears nothing under Poly, so its count never moved and the Follower plot's pulse marks were blank.
	// The newest voice's count, latched across a change of voice (`Slot::trigLatched`).
	if (publishesFromVoices(s, *d)) {
		fires = s.trigLatched;
		return true;
	}
	fires = d->mod->trigFires(s.storage.get());
	return true;
}

size_t RackEngine::recentEventsAt(size_t slot, device::RecentEvent* out, size_t capacity) const {
	if (slot >= slots_.size() || out == nullptr) {
		return 0;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	if (d == nullptr || d->mod == nullptr || d->mod->recentEvents == nullptr || s.storage == nullptr) {
		return 0;
	}
	if (publishesFromVoices(s, *d)) {
		const size_t newest = newestVoice();
		return newest < s.voices.size() && s.voices[newest] != nullptr
		           ? d->mod->recentEvents(s.voices[newest].get(), out, capacity)
		           : 0;
	}
	return d->mod->recentEvents(s.storage.get(), out, capacity);
}

void RackEngine::latchVoiceTrigFires(Slot& slot, const device::Descriptor& d) noexcept {
	const size_t newest = newestVoice();
	if (newest >= slot.voices.size() || slot.voices[newest] == nullptr) {
		slot.trigVoice = SIZE_MAX; // no voice has held a note: nothing to count, and the next one is a new voice
		return;
	}
	const uint32_t raw = d.mod->trigFires(slot.voices[newest].get());
	// THE SAME VOICE, COUNTING UP: its increase is fires. A new newest voice, or one whose count fell (a reset), only
	// sets where counting goes on from — no flash.
	if (newest == slot.trigVoice && raw >= slot.trigVoiceRaw) {
		slot.trigLatched += raw - slot.trigVoiceRaw;
	}
	slot.trigVoice = newest;
	slot.trigVoiceRaw = raw;
}

bool RackEngine::playheadLevelAt(size_t slot, float& level) const {
	if (slot >= slots_.size()) {
		return false;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	if (d == nullptr || d->mod == nullptr || d->mod->playheadLevel == nullptr || s.storage == nullptr) {
		return false;
	}
	// **A DEVICE PUBLISHING FROM ITS VOICES IS READ WHERE ITS PUBLISH IS** (backlog MY): under Poly the instance in
	// `s.storage` hears nothing, so its level is flat while the voices follow. The card draws the newest note's voice,
	// `advanceModulators`' rule for a destination with no voice, and rest before any voice has held a note.
	if (publishesFromVoices(s, *d)) {
		const size_t newest = newestVoice();
		level = newest < s.voices.size() && s.voices[newest] != nullptr ? d->mod->playheadLevel(s.voices[newest].get())
		                                                                  : 0.0f;
		return true;
	}
	level = d->mod->playheadLevel(s.storage.get());
	return true;
}

void RackEngine::updateNoteContext() {
	// **THE STACK, NOT A PITCH** — `mod::tracker::HeldKeys`, the Tracker's own, and for the reason its
	// own note gives: releasing the top note should fall back to the one still held, which a single
	// latched pitch cannot express; and the rule may change under held notes. Bounded, so the audio
	// thread never allocates (a full stack drops its oldest key). Only an Off or a Choke releases — an
	// Expression transits with its note and lifts nothing — and a negative key is CLAP's wildcard.
	const std::span<const device::NoteEvent> events = blockNotesOut();
	// **A KEY'S TUNING EXPRESSION, IN SEMITONES, FOLDED INTO ITS PITCH** — what Feathers' bus note has always
	// carried (`Voice::lastTargetSemitones`), so a keytracking device follows a per-note bend in both hosts.
	// A fresh note is untuned; a negative key is CLAP's wildcard and tunes every key.
	for (const device::NoteEvent& e : events) {
		if (e.kind == device::NoteEvent::Kind::On && e.key >= 0 && static_cast<size_t>(e.key) < keyTuning_.size()) {
			keyTuning_[static_cast<size_t>(e.key)] = 0.0f;
		}
		else if (e.kind == device::NoteEvent::Kind::Expression && e.expressionId == kTuningExpression
		         && std::isfinite(e.expressionValue)) {
			if (e.key < 0) {
				keyTuning_.fill(static_cast<float>(e.expressionValue));
			}
			else if (static_cast<size_t>(e.key) < keyTuning_.size()) {
				keyTuning_[static_cast<size_t>(e.key)] = static_cast<float>(e.expressionValue);
			}
		}
	}
	const bool struck = heldKeys_.fold(events.data(), events.size());
	noteContext_.gate = !heldKeys_.empty();
	if (noteContext_.gate) {
		// THE RULE'S NOTE. The pitch is only updated while something is down, so a device releasing its
		// envelope keeps the note it was playing rather than snapping to a default on the way out — and a
		// keytracking filter keeps its corner rather than jumping to the reference key's.
		const int16_t key = heldKeys_.pick(mod::tracker::Rule::Last);
		const float tuning = key >= 0 && static_cast<size_t>(key) < keyTuning_.size()
		                         ? keyTuning_[static_cast<size_t>(key)] : 0.0f;
		noteContext_.pitchSemitones = static_cast<float>(key) + tuning;
	}
	// **A HOST'S NOTE WINS OVER AN EMPTY STACK.** A rack in a chain receives no note events, so its
	// stack is always empty and would say "no note, gate off" every block — silencing every device in
	// it that keys off the gate. The note its host handed it is the truth for that rack.
	if (externalNoteSet_) {
		noteContext_ = externalNote_;
		return;
	}
	// **AND THE MODULATORS' GATE, IN THE PIECE THAT HOLDS THE EVENT** — see `setGate`. Only when an event
	// arrived, so a host that drives the gate by hand and sends nothing is not overridden. Every note-on
	// retriggers, not only the first of a phrase: a legato line restarts what Single Trigger says it
	// should, which is what the wrapper's `retrigger()` per note-on did.
	if (!events.empty()) {
		setGate(noteContext_.gate);
		if (struck) {
			retrigger();
		}
	}
}

void RackEngine::setExternalNote(const device::NoteContext& note) noexcept {
	externalNote_ = note;
	externalNoteSet_ = true;
	// THE MODULATORS' GATE TOO, through the one door that derives the retrigger edge from it.
	setGate(note.gate);
}

size_t RackEngine::lowestFreeLane() const noexcept {
	// LINEAR IN THE SLOT COUNT AND CALLED ONCE PER ADD, which is a message-thread event. A held free
	// list would be faster and would be a second thing to keep in step with the slots themselves.
	// **A LANE IS FREE WHEN NOTHING LIVES AT IT — one test, not a search.** This used to sweep every
	// slot looking for one holding the lane as a FIELD, which was O(n²) and, more to the point, was
	// asking a question that could have two answers. A lane is an index now, so the question is
	// whether that index holds a device.
	for (size_t lane = 0; lane < capacity(); ++lane) {
		if (!slots_[lane].live()) {
			return lane;
		}
	}
	return kNoLane;
}


std::optional<size_t> RackEngine::moveDevice(size_t from, size_t to) {
	// **THE WHOLE BODY OF THIS FUNCTION WAS DELETED, AND THAT IS WHAT STEP 2 BOUGHT.**
	//
	// It used to unpublish every descriptor in the affected range, lift each slot's payload into a
	// temporary — base, resolved, tickAccum, routeLane, storage — hand-rotate the temporaries because
	// a `Slot` holds an atomic and cannot be rotated wholesale, write them all back, rotate the
	// descriptor mirror to match, re-issue `setSlot` across the range, and republish in a carefully
	// argued order. Sixty-five lines, every one of them keeping a POSITION-indexed array in step with
	// an identity that had to be carried along by hand.
	//
	// Now the order lives in the graph and a device's storage lives at its LANE, so a reorder moves
	// NOTHING. No payload lifts, no descriptor mirror rotation, no `setSlot` re-issue — a device's
	// slot number did not change, because a device's slot number is its lane and its lane is what it
	// keeps. The three bugs that shape produced (a re-add landing in a deleted device's place, the
	// order/lane divergence in the drag road, and `removeDevice` clearing another device's routes)
	// were all position and identity disagreeing; they cannot now.
	//
	// `from` AND `to` ARE POSITIONS IN THE CHAIN, which is what a drag means and what they always
	// meant. The graph validates them.
	if (from == to) {
		return std::nullopt;
	}
	const std::optional<size_t> cleared = graph().move(0, from, to);
	if (!cleared) {
		return std::nullopt;
	}
	// **PUBLISHED AFTER THE GRAPH HAS MOVED, and that ordering is the only one left to get right.**
	// The audio thread walks the snapshot; until this store it walks the old order, which is a
	// complete and valid order rather than a half-applied one.
	publishOrder();
	// NOTHING TOUCHES THE ROUTE TABLE, and that is still the point of the lane: the moved device's
	// columns are indexed by where it lives, and it did not move. BUT A MOVE CAN CLEAR EDGES THE NEW
	// ORDER FORBIDS (`cleared` counts them), so the published list catches up.
	if (*cleared > 0) {
		routesDirty_ = true;
	}
	return cleared;
}


float RackEngine::routeAmount(backplane::Source source, backplane::Destination destination) const {
	// **THE ROUTES THIS ENGINE RESOLVES THROUGH.** When a host handed them over (`setHostRoutes`) that
	// is the host's list — this engine's own graph then holds structure only, and answering from it would
	// report a store nothing resolves. Otherwise the graph's, by endpoint, every route an edge.
	if (hostOwnsRoutes_) {
		for (size_t i = 0; i < hostRouteCount_; ++i) {
			if (hostRoutes_[i].source == source && hostRoutes_[i].destination == destination) {
				return hostRoutes_[i].amount;
			}
		}
		return 0.0f;
	}
	return graph().amountOf(source, destination);
}

void RackEngine::setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) {
	// **THROUGH THE GRAPH'S DOOR (rack routing rule, commit 3).** This wrote the engine's own table
	// directly — no gate — while `Graph::route` sat beside it enforcing pass-through and called by
	// nothing but its own tests (plan §7.3). The engine's table is gone; the graph's is the table, and
	// a device-to-device route goes in through `route()`, which REFUSES a backwards or self route by
	// construction (`reaches(to, from)`, self included). That is the routing rule at the engine:
	// what `mayRoute` will not offer, this will not write, and `resolveInto` needs no mask because an
	// illegal route cannot exist in the table it reads.
	//
	// **A CORE END IS AN ENDPOINT LIKE ANY OTHER** (kill-lanes step 2): a macro's route is an edge from the
	// host, with no node and so no precedence — `route()` takes it and orders nothing by it.
	//
	// THE REFUSAL IS SILENT HERE AND THAT IS DELIBERATE: the gesture was already refused at `mayRoute` and
	// the loader already gated on it, so a write reaching here that `route()` declines is a caller that
	// bypassed both — the test extension, or a patch written by hand — and the honest outcome is no route,
	// which `routeAmount` reports.
	// WHATEVER HAPPENS BELOW, THE INPUTS ARE ASKED TO CATCH UP — see `inputs_`. BY ENDPOINT since
	// kill-lanes §2.4b: the two flat coordinates this took were decoded through `Graph::endpointsOf`,
	// and every caller already held the identities it was encoding.
	routesDirty_ = true;
	(void)graph().route(source, destination,
	                   std::isfinite(amount) ? std::clamp(amount, -device::kMaxAmount, device::kMaxAmount) : 0.0f);
	// THE GRAPH GREW ITS EDGES IF IT HAD TO (`Graph::makeEdgeRoom`, under this verb's no-block contract); the scratch
	// its routes are gathered into, and the inputs they compile to, keep pace here, where allocating is allowed.
	if (graph().edgeCount() > routeScratch_.capacity()) {
		reserveRoutes(graph().edgeCount());
	}
	// **AND THE ORDER, BECAUSE A ROUTE CAN MOVE IT (backlog IZ)** — a modulator routed into one to its
	// left is ticked first from now on (`publishOrder`). Under the same contract as the three structural
	// verbs that publish it: message thread, at a drain.
	publishOrder();
}

template <typename Routes>
void RackEngine::adoptHostRoutes(const Routes& routes) noexcept {
	// COPIED, NOT BORROWED — the host's staging is its own to reuse the moment this returns. WHOLE OR NOT AT ALL
	// (backlog OJ): a list bigger than the room `reserveRoutes` made is refused and counted, and the routes this
	// engine already had stay — a truncated copy would draw routes the engine never hears.
	// THE HOST'S SOURCE VALUES ARE NOT HANDED OVER HERE: they arrive through `setMacro`, into this
	// engine's own cells, which is where an input from a host source points (see the declaration).
	const auto into = hostRoutes_.view();
	if (routes.size() > into.capacity()) {
		routeRoomRefusals_.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	for (size_t i = 0; i < routes.size(); ++i) {
		into[i] = routes[i];
	}
	hostRouteCount_ = routes.size();
	hostOwnsRoutes_ = true;
	routesDirty_ = true;
}

void RackEngine::setHostRoutes(std::span<const backplane::Route> routes) noexcept { adoptHostRoutes(routes); }
void RackEngine::setHostRoutes(const backplane::RouteRun& routes) noexcept { adoptHostRoutes(routes); }

void RackEngine::clearHostRoutes() noexcept {
	hostOwnsRoutes_ = false;
	hostRouteCount_ = 0;
	routesDirty_ = true;
}

const float* RackEngine::outputCellForVoiceOf(DeviceInstanceId id, size_t output) const noexcept {
	const size_t slot = slotOf(id);
	if (slot == kNoSlot || voices_ == 0) {
		return nullptr;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || output >= d->outputs.size() || !d->outputs[output].perVoice
	    || (output + 1) * voices_ > s.outputsPerVoice.size()) {
		return nullptr;
	}
	return &s.outputsPerVoice[output * voices_];
}

void RackEngine::writeOutputForVoice(DeviceInstanceId id, size_t output, size_t voice, float value) noexcept {
	writeVoiceOutput(id, output, voice, value, false);
}

void RackEngine::startOutputForVoice(DeviceInstanceId id, size_t output, size_t voice, float value) noexcept {
	writeVoiceOutput(id, output, voice, value, true);
}

void RackEngine::writeVoiceOutput(DeviceInstanceId id, size_t output, size_t voice, float value, bool start) noexcept {
	const size_t slot = slotOf(id);
	if (slot == kNoSlot || voice >= voices_) {
		return;
	}
	Slot& s = slots_[slot];
	const size_t at = output * voices_ + voice;
	if (at >= s.outputsPerVoice.size()) {
		return;
	}
	// THE AMOUNT REACHES THE PER-VOICE RUN TOO (backlog DH), by the block's resolved value — one knob,
	// every voice; a per-voice source's amount is not itself resolved per voice.
	if (const device::Descriptor* d = s.live_desc(); d != nullptr) {
		value *= amountOf(*d, s.resolved);
		// **AND THE ENABLE SWITCH, WHICH THE COLLAPSED PUBLISH APPLIES AND THIS ROAD DID NOT** (backlog
		// EH, shape 6 — a guard asymmetry inside one function). The amount was carried here for DH; the
		// switch was not, so a per-voice source switched OFF kept reaching every voice: in the default
		// patch the strip envelope, routed into Level at full amount, could not be switched off at all.
		const int enable = d->traits.enableControl;
		if (enable >= 0 && static_cast<size_t>(enable) < s.resolved.size()
		    && s.resolved[static_cast<size_t>(enable)] < 0.5f) {
			value = 0.0f;
		}
	}
	// FINITE ONLY — the last gate before the number becomes an offset on every destination it reaches,
	// the rule `ParamSet::setDeviceSourceForVoice` kept before this cell was the node's own.
	const float next = std::isfinite(value) ? value : 0.0f;
	// THE TICK'S WINDOW, from where this voice's value stood to where it stands now — or, when the voice starts a
	// note, flat at where the new note starts (backlog FF): the old note's value is not this note's to ramp from.
	{
		const size_t w = at * static_cast<size_t>(device::kTickWindowStride);
		if (w + static_cast<size_t>(device::kTickWindowStride) <= s.outputsPerVoiceWindow.size()) {
			device::fillTickWindowRamp(&s.outputsPerVoiceWindow[w], start ? next : s.outputsPerVoice[at], next);
		}
	}
	std::atomic_ref<float>(s.outputsPerVoice[at]).store(next, std::memory_order_relaxed);
}

void RackEngine::writeOutputPairForVoice(DeviceInstanceId id, size_t output, size_t voice, int32_t noteId,
                                         int16_t channel, int16_t key, bool start) noexcept {
	const size_t slot = slotOf(id);
	if (slot == kNoSlot || voice >= voices_ || output >= 8) {
		return;
	}
	Slot& s = slots_[slot];
	const size_t at = output * voices_ + voice;
	const device::Descriptor* d = s.live_desc();
	if (((s.emitsPair >> output) & 1u) == 0 || d == nullptr || d->mod == nullptr || at >= s.outputsPerVoiceR.size()) {
		return;
	}
	// THE DEVICE'S OWN PAIR FOR THE NOTE, through the left ear's rules (the amount and the switch, as `writeVoiceOutput`
	// applies them); or, for a device with no pair per note, the collapsed pair's right ear, already scaled and gated.
	float value = relaxedLoadCell(s.outputsR[output]);
	if (d->mod->outputPairForNote != nullptr && !publishesFromVoices(s, *d)) {
		value = d->mod->outputPairForNote(s.storage.get(), output, noteId, channel, key).right * amountOf(*d, s.resolved);
		const int enable = d->traits.enableControl;
		if (enable >= 0 && static_cast<size_t>(enable) < s.resolved.size()
		    && s.resolved[static_cast<size_t>(enable)] < 0.5f) {
			value = 0.0f;
		}
	}
	const float next = std::isfinite(value) ? value : 0.0f;
	const size_t w = at * static_cast<size_t>(device::kTickWindowStride);
	if (w + static_cast<size_t>(device::kTickWindowStride) <= s.outputsPerVoiceWindowR.size()) {
		device::fillTickWindowRamp(&s.outputsPerVoiceWindowR[w], start ? next : s.outputsPerVoiceR[at], next);
	}
	std::atomic_ref<float>(s.outputsPerVoiceR[at]).store(next, std::memory_order_relaxed);
}

const float* RackEngine::outputWindowRForVoiceOf(DeviceInstanceId id, size_t output) const noexcept {
	if (outputCellForVoiceOf(id, output) == nullptr) {
		return nullptr;
	}
	const Slot& s = slots_[slotOf(id)];
	const size_t w = output * voices_ * static_cast<size_t>(device::kTickWindowStride);
	return w + voices_ * static_cast<size_t>(device::kTickWindowStride) <= s.outputsPerVoiceWindowR.size()
	           ? &s.outputsPerVoiceWindowR[w]
	           : nullptr;
}

const float* RackEngine::outputWindowForVoiceOf(DeviceInstanceId id, size_t output) const noexcept {
	const float* cell = outputCellForVoiceOf(id, output);
	if (cell == nullptr) {
		return nullptr;
	}
	const Slot& s = slots_[slotOf(id)];
	const size_t w = output * voices_ * static_cast<size_t>(device::kTickWindowStride);
	return w + voices_ * static_cast<size_t>(device::kTickWindowStride) <= s.outputsPerVoiceWindow.size() ? &s.outputsPerVoiceWindow[w] : nullptr;
}

void RackEngine::tickModulators(int frames, const backplane::BlockContext& block) {
	const StageScope stage(Stage::Mods); // the spike recorder's, at the top of the tree only (backlog SR)
	// THE INPUTS FIRST, IF THEY MOVED: Feathers ticks its strip between blocks, before the drain's
	// `beginBlock` would have rebuilt them, and a tick must not resolve against yesterday's plan.
	if (routesDirty_) {
		rebuildInputs();
	}
	// AND THE HOST SLOTS, for the same reason: the modulators resolve their own rows in this tick, and a host
	// write that arrived in this `process` belongs to it (host slots, T5; `beginPiece` does the same).
	applyHostSlots();
	advanceModulators(frames, block);
}

const float* RackEngine::outputCellOf(DeviceInstanceId id, size_t output) const noexcept {
	const size_t slot = slotOf(id);
	if (slot == kNoSlot || output >= slots_[slot].outputs.size()) {
		return nullptr;
	}
	const device::Descriptor* d = slots_[slot].live_desc();
	return d != nullptr && output < d->outputs.size() ? &slots_[slot].outputs[output] : nullptr;
}

size_t RackEngine::inputCountForTest(size_t lane, size_t control) const noexcept {
	return lane < slots_.size() && control < slots_[lane].inputs.size() ? slots_[lane].inputs[control].count : 0;
}

device::OuterSources RackEngine::outerSourcesFor(DeviceInstanceId self) const noexcept {
	device::OuterSources outer;
	outer.host = this;
	outer.self = self;
	outer.cell = [](const void* host, backplane::Source s) {
		return static_cast<const RackEngine*>(host)->sourceCell(s);
	};
	outer.upstream = [](const void* host, DeviceInstanceId who, backplane::Source s) {
		return static_cast<const RackEngine*>(host)->upstreamOf(who, s);
	};
	outer.perVoice = [](const void* host, backplane::Source s) {
		return static_cast<const RackEngine*>(host)->sourcePerVoiceRun(s);
	};
	outer.voices = voices_;
	outer.arrival = [](const void* host, DeviceInstanceId who) -> uint32_t {
		const auto* e = static_cast<const RackEngine*>(host);
		const size_t lane = e->slotOf(who);
		return lane < e->arrival_->capacity() ? (*e->arrival_)[lane].at : 0u;
	};
	return outer;
}

const float* RackEngine::sourceCell(backplane::Source s) const noexcept {
	// AN OUTER SOURCE READS THROUGH THE READER the host outside handed this engine (rack roadmap §7);
	// none handed, none read — the route stays in the table and moves nothing, and the gate refuses
	// to make another until the reader arrives.
	if (s.isOuter()) {
		return outer_.cell != nullptr ? outer_.cell(outer_.host, s.inner()) : nullptr;
	}
	// A SOURCE IN A DEVICE'S PRE CHAIN, EXPORTED (backlog NZ): the site's own cell for the row, which PRE's run fills.
	// The site is the device's (`offerSite` checks the owner at the drain), and its cell lives as long as it does, so
	// the pointer is rebuilt whenever a site is installed or let go (`adoptSites` marks the routes dirty).
	if (s.isExport()) {
		const size_t at = slotOf(s.device);
		// A CONTAINER'S OWN EXPORT (backlog NZ (b)): its table's cell, in the storage this slot runs; a drain that
		// replaces the storage rebuilds this pointer with every other. THE ROAD IS THE SOURCE'S BIT (2026-10-29): a Rack
		// may hold a site too, and its row 0 and the site's row 0 are two cells.
		if (s.isContainerExport()) {
			if (at == kNoSlot || at >= slots_.size() || !slots_[at].live()) {
				return nullptr;
			}
			const device::Descriptor* d = slots_[at].live_desc();
			return device::exportsContainer(*d) ? d->container->exportCell(slots_[at].storage.get(), s.exportRow())
			                                    : nullptr;
		}
		const SiteChains* site = at != kNoSlot && at < slots_.size() ? slots_[at].site.load(std::memory_order_acquire) : nullptr;
		return site != nullptr && site->owner() == s.device ? site->exportCell(s.exportRow()) : nullptr;
	}
	if (s.isDevice()) {
		return outputCellOf(s.device, s.index);
	}
	// A CORE SOURCE IS THIS ENGINE'S OWN CELL, whoever fills it — the hand on a standalone rack, the
	// chain device nested, Feathers' synth on its lane — read through the macro's polarity at the write.
	return s.index < macros_.size() ? &macros_[s.index] : nullptr;
}

const float* RackEngine::sourcePerVoiceRun(backplane::Source s) const noexcept {
	if (s.isOuter() || s.isExport() || !s.isDevice()) {
		return nullptr; // the host's own sources have one value; two levels out is not a road
	}
	return outputCellForVoiceOf(s.device, s.index);
}

bool RackEngine::upstreamOf(DeviceInstanceId self, backplane::Source s) const noexcept {
	if (s.isOuter()) {
		// **FURTHER OUT STILL**: not this engine's to judge — the host outside
		// it decides whether the source is upstream of the chain THIS engine is, by the same question one hop out.
		return outer_.upstream != nullptr && outer_.upstream(outer_.host, outer_.self, s.inner());
	}
	if (sourceCell(s) == nullptr) {
		return false; // not a source this engine publishes
	}
	if (!s.isDevice()) {
		return true; // a core source is upstream of everything
	}
	if (s.isExport()) {
		// A site's own PRE export, read by its POST, or one read inside a container after the holder (NZ rung 5): the
		// same answer `backplane::sourceUpstreamOf` gives the message thread, asked of this engine's graph.
		if (s.device == self) {
			// NOT THE HOLDER'S OWN (2026-10-29): a site's POST is admitted by the site's own gate, and a Rack's or a
			// Chain's chains, asking with the same id, reach neither its PRE (rule 5 stays out) nor its own inside.
			return false;
		}
		const auto* node = graph().nodeOf(self);
		const device::Descriptor* container = node != nullptr ? node->desc : nullptr;
		return container != nullptr && container->audio != nullptr && container->mod == nullptr
		    && !graph().wouldCycle(s, backplane::Destination::control(self, 0));
	}
	// A HOLDER IS NOT UPSTREAM OF ITSELF, SAID OUTRIGHT (trap T1) — see `backplane::sourceUpstreamOf`: a
	// declared self-route (backlog HS) is about the holder's own controls, and `control(self, 0)` below
	// must not let whatever control 0 is answer for the chain inside.
	if (s.device == self) {
		return false;
	}
	// THE ORDERING HALF OF `mayRoute`, with the chain as the destination: a device output may reach the
	// chain iff a route into the chain from it would not have to be read before it is written.
	return !graph().wouldCycle(s, backplane::Destination::control(self, 0));
}

void RackEngine::setOuterSources(const device::OuterSources& outer) noexcept {
	outer_ = outer;
	routesDirty_ = true; // an inner route naming an outer source can resolve now
}

backplane::DeviceView RackEngine::devices() const noexcept {
	backplane::DeviceView view = graph().devices();
	// THE VIEW THE GRAPH GIVES KNOWS THIS ENGINE'S DEVICES; the host outside is this engine's to add.
	// `host` stays the graph's, so the graph's own two answers keep working; the outer question is
	// answered through THIS engine, reached by a second pointer the reader carries.
	if (outer_.upstream != nullptr) {
		view.outerHost = this;
		view.outerUpstream = [](const void* host, backplane::Source s) {
			const auto* engine = static_cast<const RackEngine*>(host);
			return engine->outer_.upstream != nullptr && engine->outer_.upstream(engine->outer_.host, engine->outer_.self, s);
		};
	}
	return view;
}

void RackEngine::rebuildInputs() noexcept {
	// **EVERY NODE'S INPUTS, FROM THE ROUTES BY IDENTITY — the whole plan, rebuilt at the drain
	// (kill-lanes §2.4b).** Two passes over the routes and no allocation: count what reaches each
	// control, lay the ranges out end to end, then fill. A route whose ends this engine cannot place —
	// a device it does not run, an output or control the device does not declare, a core destination
	// (the HOST's node, which builds its own inputs) — is simply not an input here; nothing is guessed.
	//
	// WHICH ROUTES: the ones a host handed over (`setHostRoutes`, the standalone rack's bridge) or this
	// engine's own graph's. Either way the walk is over identities, and the only address it produces is
	// the pointer each input carries, valid until the next rebuild — see `Input`.
	// THE STORES' DIRECTORIES, TAKEN ONCE HERE (backlog OJ): whatever room the message thread has added by now, and
	// every index below bounded by it. `inputs_` is the view every resolve reads until the next rebuild.
	inputs_ = inputPool_.view();
	// EVERY SLOT'S NEXT RESOLVE IS A WHOLE ONE: which controls an input reaches, and what it reads, may have moved.
	++routesSerial_;
	const auto scratch = routeScratch_.view();
	size_t n = 0;
	if (hostOwnsRoutes_) {
		n = hostRouteCount_;
	}
	else {
		graph().forEachRoute([&](const backplane::Route& r) {
			if (n < scratch.capacity()) { // `setRouteAmount` grows it with the edges, so never short
				scratch[n++] = r;
			}
		});
	}
	const backplane::RouteRun routes { hostOwnsRoutes_ ? backplane::RouteStage::ConstView(hostRoutes_.view()) : backplane::RouteStage::ConstView(scratch), n };

	// WHERE A ROUTE'S ENDS ARE, OR NOWHERE.
	const auto cellOf = [this](const backplane::Source& s) -> const float* { return sourceCell(s); };
	const auto controlOf = [this](const backplane::Destination& d, Slot** slot) -> size_t {
		if (!d.isDevice()) {
			return SIZE_MAX; // the host's own control — not this engine's to resolve
		}
		const size_t at = slotOf(d.device);
		if (at == kNoSlot) {
			return SIZE_MAX;
		}
		const device::Descriptor* desc = slots_[at].live_desc();
		if (desc == nullptr || d.index >= desc->params.size() || d.index >= slots_[at].inputs.size()
		    || !desc->params[d.index].modulatable) {
			return SIZE_MAX;
		}
		*slot = &slots_[at];
		return d.index;
	};

	// PASS ONE: COUNT. Every live node's ranges to zero first, so a control that lost its last route
	// this drain reads empty rather than yesterday's.
	for (Slot& slot : slots_) {
		slot.anyPerVoiceInput = false;
		for (InputRange& range : slot.inputs) {
			range = InputRange {};
		}
	}
	for (const backplane::Route& r : routes) {
		Slot* slot = nullptr;
		const size_t control = controlOf(r.destination, &slot);
		if (control == SIZE_MAX || cellOf(r.source) == nullptr) {
			continue;
		}
		++slot->inputs[control].count;
	}
	// LAY OUT: each control's range starts where the previous one ended. Then the counts become fill
	// cursors and climb back to where they were.
	uint32_t next = 0;
	for (Slot& slot : slots_) {
		for (InputRange& range : slot.inputs) {
			range.first = next;
			next += range.count;
			range.count = 0;
		}
	}
	// PASS TWO: FILL. The same tests as pass one, so the two agree on every route.
	for (const backplane::Route& r : routes) {
		Slot* slot = nullptr;
		const size_t control = controlOf(r.destination, &slot);
		const float* cell = control == SIZE_MAX ? nullptr : cellOf(r.source);
		if (cell == nullptr) {
			continue;
		}
		InputRange& range = slot->inputs[control];
		const size_t at = static_cast<size_t>(range.first) + range.count;
		if (at < inputs_.size()) {
			// THE PER-VOICE RUN TOO, when the source has one (a device output declared `perVoice`); the host's
			// own sources have none. A slot with any such input resolves its per-voice instances per voice.
			// AN OUTER SOURCE'S RUN COMES THROUGH THE READER (rack roadmap §7/§8, the pass-through model):
			// this engine runs the outer host's voice count, so voice v reads entry v.
			const float* perVoice = r.source.isOuter()
			                            ? (outer_.perVoice != nullptr ? outer_.perVoice(outer_.host, r.source.inner()) : nullptr)
			                            : (r.source.isDevice() && !r.source.isExport() ? outputCellForVoiceOf(r.source.device, r.source.index) : nullptr);
			// DESIGNATED, because two pointers side by side in a positional initialiser swapped once and every
			// reader read the other one's cell (2026-09-13, caught by the DU cases before it left the worktree).
			// A DEVICE SOURCE'S TICK WINDOW beside its cell; a macro or an outer source has none.
			const float* window = nullptr;
			// AND ITS RIGHT EAR (backlog KU): the slot's `outputsR` for a device output; a macro or an outer source is
			// one value in both ears, so its right ear is its cell.
			const float* valueR = cell;
			const float* windowR = nullptr;
			uint32_t sourceLane = UINT32_MAX;
			if (r.source.isDevice()) {
				if (const size_t src = slotOf(r.source.device); src != kNoSlot && r.source.index < slots_[src].outputs.size()) {
					window = &slots_[src].outputsWindow[r.source.index * static_cast<size_t>(device::kTickWindowStride)];
					valueR = &slots_[src].outputsR[r.source.index];
					windowR = &slots_[src].outputsWindowR[r.source.index * static_cast<size_t>(device::kTickWindowStride)];
					sourceLane = static_cast<uint32_t>(src);
				}
			}
			const float* perVoiceWindow = r.source.isDevice() && perVoice != nullptr ? outputWindowForVoiceOf(r.source.device, r.source.index) : nullptr;
			inputs_[at] = Input { .value = cell, .perVoice = perVoice, .amount = r.amount, .transfer = r.transfer, .window = window,
			                      .perVoiceWindow = perVoiceWindow, .valueR = valueR, .windowR = windowR, .perVoiceR = perVoice,
			                      .sourceLane = sourceLane, .sourceOutput = static_cast<uint8_t>(r.source.index),
			                      .fromOuter = r.source.isOuter() };
			slot->anyPerVoiceInput = slot->anyPerVoiceInput || perVoice != nullptr;
			++range.count;
		}
	}
	// PASS THREE: WHICH CONTROLS HEAR TWO EARS, AND WHICH OUTPUTS SPEAK TWO (backlog KU, design §1). A control is
	// stereo-live when it is declared `stereo` and an input reaches it from an output that is a pair; an output is a
	// pair when it says so (`pairAlways`) or when a control it names (`pairFrom`) is stereo-live. TO A FIXED POINT,
	// not one walk in graph order: a route may run leftward (backlog IZ) and this is the routes' walk, not the
	// graph's. Bits are only ever SET, so it ends — within one pass per slot on an acyclic graph. Worked in a local
	// mask per slot (`Slot::stereoLivePending`), then STORED through `atomic_ref` at the end,
	// because the editor reads `Slot::stereoLive` from the message thread.
	// AND `pairWhile` (backlog PA): a pair while the named control's base value is not zero — the LFO's Stereo Phase.
	const auto pairsOf = [](const device::Descriptor& d, const device::ControlMask& live, const Slot& slot) {
		uint64_t pairs = 0;
		for (size_t o = 0; o < d.outputs.size() && o < device::kMaxDeviceOutputs; ++o) {
			const int on = d.outputs[o].pairWhile;
			bool pair = d.outputs[o].pairAlways
			            || (on >= 0 && static_cast<size_t>(on) < slot.base.size() && slot.base[static_cast<size_t>(on)] != 0.0f);
			for (size_t c = 0; !pair && c < 32; ++c) {
				pair = ((d.outputs[o].pairFrom >> c) & 1u) != 0 && live.test(c);
			}
			pairs |= pair ? uint64_t { 1 } << o : 0;
		}
		return pairs;
	};
	// THE PAIRS BEFORE THIS DRAIN, so an output that BECOMES one below has its per-voice right ear primed from its left.
	for (Slot& slot : slots_) {
		slot.pairsBefore = slot.emitsPair;
		slot.stereoLivePending = {};
		const device::Descriptor* d = slot.live_desc();
		slot.emitsPair = d != nullptr ? pairsOf(*d, slot.stereoLivePending, slot) : 0;
	}
	bool changed = true;
	for (size_t pass = 0; changed && pass <= slots_.size(); ++pass) {
		changed = false;
		for (Slot& slot : slots_) {
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr) {
				continue;
			}
			for (size_t c = 0; c < slot.inputs.size() && c < d->params.size(); ++c) {
				if (!d->params[c].stereo || slot.stereoLivePending.test(c)) {
					continue;
				}
				const InputRange range = slot.inputs[c];
				for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
					const Input& in = inputs_[i];
					if (in.sourceLane < slots_.size() && ((slots_[in.sourceLane].emitsPair >> in.sourceOutput) & 1u) != 0) {
						slot.stereoLivePending.set(c);
						changed = true;
						break;
					}
				}
			}
			if (const uint64_t pairs = pairsOf(*d, slot.stereoLivePending, slot); pairs != slot.emitsPair) {
				slot.emitsPair = pairs;
				changed = true;
			}
		}
	}
	for (Slot& slot : slots_) {
		for (size_t w = 0; w < slot.stereoLive.words.size(); ++w) {
			std::atomic_ref<uint64_t>(slot.stereoLive.words[w]).store(slot.stereoLivePending.words[w], std::memory_order_relaxed);
		}
		// AN OUTPUT THAT HAS JUST BECOME A PAIR starts its per-voice right ear where its left stands (backlog PA): until the
		// next tick writes it, the right ear is the left, which is what every reader saw a moment ago.
		const uint64_t became = slot.emitsPair & ~slot.pairsBefore;
		for (size_t o = 0; became != 0 && o < device::kMaxDeviceOutputs; ++o) {
			if (((became >> o) & 1u) == 0 || (o + 1) * voices_ > slot.outputsPerVoice.size()) {
				continue;
			}
			std::copy_n(&slot.outputsPerVoice[o * voices_], voices_, &slot.outputsPerVoiceR[o * voices_]);
			const size_t stride = voices_ * static_cast<size_t>(device::kTickWindowStride);
			std::copy_n(&slot.outputsPerVoiceWindow[o * stride], stride, &slot.outputsPerVoiceWindowR[o * stride]);
		}
	}
	// AND EVERY PER-VOICE INPUT'S RIGHT EAR POINTS AT ITS SOURCE'S RIGHT RUN WHILE THAT OUTPUT IS A PAIR, at its left run
	// otherwise (backlog PA) — so a mono source's per-voice right ear costs nothing, and a pair's is its own.
	for (const Slot& slot : slots_) {
		for (const InputRange range : slot.inputs) {
			for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
				Input& in = inputs_[i];
				if (in.perVoice == nullptr || in.sourceLane >= slots_.size()) {
					continue;
				}
				const Slot& src = slots_[in.sourceLane];
				const bool pair = ((src.emitsPair >> in.sourceOutput) & 1u) != 0;
				const size_t at = static_cast<size_t>(in.sourceOutput) * voices_;
				in.perVoiceR = pair && at + voices_ <= src.outputsPerVoiceR.size() ? &src.outputsPerVoiceR[at] : in.perVoice;
			}
		}
	}
	// AND A LATE DEVICE'S INPUTS READ THEIR SOURCES LATE (backlog LW step 4), last, over pointers now final.
	assignModRings();
	routesDirty_ = false;
}

void RackEngine::assignModRings() noexcept {
	for (ModRing& ring : modRings_) {
		ring.kept = false;
	}

	const uint32_t outer = outerArrival_;
	if ((maxArrival_ > 0 || outer > 0) && !modRings_.empty() && ringLen_ > 1) {
		for (size_t lane = 0; lane < slots_.size(); ++lane) {
			Slot& slot = slots_[lane];
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr || d->audio == nullptr || lane >= arrival_->capacity()) {
				continue;
			}
			const uint32_t own = (*arrival_)[lane].at;
			for (const InputRange range : slot.inputs) {
				for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
					Input& in = inputs_[i];
					const uint64_t late = static_cast<uint64_t>(own) + (in.fromOuter ? outer : 0u);
					// IN CONTROL TICKS, NEAREST: a route resolves once a tick, so that is the grain it can follow.
					auto delay = static_cast<uint32_t>(
					    std::min<uint64_t>((late + device::kTickWindow / 2) / device::kTickWindow, ringLen_ - 1));
					// AN OUTER CELL IS ALREADY THIS TICK'S WHEN THIS ENGINE RESOLVES — a hosted engine resolves a tick before
					// it ticks, and its rings sample at the tick — so its ring is one tick shorter (`rack_device`'s order).
					if (in.fromOuter && delay > 0) {
						--delay;
					}
					if (delay == 0 || in.value == nullptr) {
						continue;
					}
					const float* srcR = in.valueR != nullptr && in.valueR != in.value ? in.valueR : nullptr;
					ModRing* ring = nullptr;
					for (ModRing& r : modRings_) {
						if (r.live && r.src == in.value && r.srcR == srcR && r.delay == delay) {
							ring = &r; // one ring for every route on this (source, late domain) pair, its history kept
							break;
						}
					}
					if (ring == nullptr) {
						for (size_t k = 0; k < modRings_.size(); ++k) {
							ModRing& r = modRings_[k];
							if (r.live) {
								continue;
							}
							// A NEW RING STARTS FULL OF WHAT THE SOURCE SAYS NOW: nothing is late until the source moves.
							r = ModRing { in.value, srcR, delay, 0, *in.value, srcR != nullptr ? *srcR : *in.value, true, false };
							float* buf = &modRingBuf_[k * ringLen_ * 2];
							std::fill_n(buf, ringLen_, r.out);
							std::fill_n(buf + ringLen_, ringLen_, r.outR);
							ring = &r;
							break;
						}
					}
					if (ring == nullptr) {
						++modRingRefusals_; // none free: read on time
						continue;
					}
					ring->kept = true;
					in.value = &ring->out;
					in.valueR = srcR != nullptr ? &ring->outR : &ring->out;
					in.window = nullptr; // flat across its tick: a late read follows to the tick, not the sample
					in.windowR = nullptr;
				}
			}
		}
	}
	for (ModRing& ring : modRings_) {
		if (ring.live && !ring.kept) {
			ring = ModRing {};
		}
	}
}

void RackEngine::sizeVoiceDelays() {
	voiceHistory_.assign(voices_ * kVoiceMoments, NoteMoment {});
	voiceHead_.assign(voices_, 0);
	voiceCount_.assign(voices_, 0);
}

void RackEngine::recordVoiceMoment(size_t voice, const device::NoteContext& note) noexcept {
	if (voice >= voiceHead_.size()) {
		return;
	}
	NoteMoment* ring = &voiceHistory_[voice * kVoiceMoments];
	uint32_t& head = voiceHead_[voice];
	uint32_t& count = voiceCount_[voice];
	if (count > 0) {
		NoteMoment& newest = ring[(head + count - 1) % kVoiceMoments];
		if (newest.note.gate == note.gate && newest.note.pitchSemitones == note.pitchSemitones
		    && newest.note.stackCount == note.stackCount && newest.note.stackDetuneCents == note.stackDetuneCents
		    && newest.note.stackCoherence == note.stackCoherence) {
			return;
		}
		if (newest.at == busClock_) {
			newest.note = note;
			return;
		}
	}
	if (count == kVoiceMoments) {
		head = (head + 1) % kVoiceMoments; // full: the oldest goes, read early, never stuck
		--count;
	}
	ring[(head + count) % kVoiceMoments] = NoteMoment { busClock_, note, {} };
	++count;
	++lateWork_;
}

const device::NoteContext& RackEngine::voiceNoteAt(size_t voice, uint64_t at, const device::NoteContext& now) const noexcept {
	if (voice >= voiceHead_.size() || voiceCount_[voice] == 0) {
		return now;
	}
	const NoteMoment* ring = &voiceHistory_[voice * kVoiceMoments];
	const uint32_t head = voiceHead_[voice];
	for (size_t k = voiceCount_[voice]; k-- > 0;) {
		const NoteMoment& m = ring[(head + k) % kVoiceMoments];
		if (m.at <= at) {
			return m.note;
		}
	}
	// BEFORE ITS FIRST MOMENT THE VOICE WAS NOT SOUNDING: a voice's history starts at its note, so a device the note has
	// not reached yet is handed that note's pitch with the gate down.
	silentVoiceNote_ = ring[head].note;
	silentVoiceNote_.gate = false;
	return silentVoiceNote_;
}

void RackEngine::tickModRings(int frames) noexcept {
	if (frames <= 0 || ringLen_ == 0) {
		return;
	}
	const int ticks = (frames + device::kTickWindow - 1) / device::kTickWindow;
	for (size_t k = 0; k < modRings_.size(); ++k) {
		ModRing& r = modRings_[k];
		if (!r.live) {
			continue;
		}
		float* left = &modRingBuf_[k * ringLen_ * 2];
		float* right = left + ringLen_;
		for (int t = 0; t < ticks; ++t) {
			left[r.head] = *r.src;
			right[r.head] = r.srcR != nullptr ? *r.srcR : left[r.head];
			const size_t back = (r.head + ringLen_ - r.delay) % ringLen_;
			r.out = left[back];
			r.outR = right[back];
			r.head = static_cast<uint32_t>((r.head + 1) % ringLen_);
		}
		++lateWork_;
	}
}

float RackEngine::resolveControl(const Slot& slot, const device::Descriptor& desc, size_t control, size_t voice) const {
	return resolveEar<false>(slot, desc, control, voice);
}

bool RackEngine::probeRoute(DeviceInstanceId device, size_t control, backplane::Source source,
                            backplane::RouteProbe& out) const noexcept {
	// `resolveEar`'s walk with one input set aside (backlog PU): the readout adds that one back with the amount
	// under the hand, so its sum is this engine's sum with the amount it has not adopted yet.
	const size_t at = slotOf(device);
	if (at == kNoSlot || at >= slots_.size() || !slots_[at].live()) {
		return false;
	}
	const Slot& slot = slots_[at];
	const double span = control < slot.routeSpan.size() ? slot.routeSpan[control] : -1.0;
	const float* watched = sourceCell(source);
	if (span < 0.0 || watched == nullptr || control >= slot.hostMod.size()) {
		return false;
	}
	backplane::RouteProbe p;
	p.source = relaxedLoadCell(*watched);
	p.span = span;
	bool past = false;
	if (control < slot.inputs.size()) {
		const InputRange range = slot.inputs[control];
		for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
			const Input& in = inputs_[i];
			if (in.value == watched) {
				past = true;
				continue;
			}
			(past ? p.after : p.before) += device::routeTerm(in.amount, in.transfer, relaxedLoadCell(*in.value), span);
		}
	}
	// THE HOST'S MODULATION LAST, as `resolveEar` adds it — after every route, the watched one included (a route
	// not seated yet is read as the last input, where a new one lands).
	p.after += static_cast<double>(slot.hostMod[control]) * span;
	out = p;
	return true;
}

void RackEngine::answerRouteProbe(backplane::RouteProbeChannel& channel) const noexcept {
	backplane::Source source;
	backplane::Destination destination;
	if (!channel.watched(source, destination)) {
		return;
	}
	backplane::RouteProbe p;
	if (probeRoute(destination.device, destination.index, source, p)) {
		channel.publish(source, destination, p);
	}
}

template <bool kRight>
float RackEngine::resolveEar(const Slot& slot, const device::Descriptor& desc, size_t control, size_t voice) const {
	float value = slot.base[control];
	// THE TABLE, NOT THE DESCRIPTOR. `routeSpan` was computed once at provision — `routeSpanOf` for a
	// modulatable control, a negative for one that is no destination. This asked the descriptor per
	// control per tick (`nthModulatableOfControl` is a scan of the params, then `routeSpanOf`), which the
	// render benchmark measured as a fixed ~10 µs a block with nothing routed at all.
	const double span = control < slot.routeSpan.size() ? slot.routeSpan[control] : -1.0;
	if (span < 0.0) {
		return value;
	}
	const device::ParamDesc& p = desc.params[control];
	// **FEATHERS' RESOLVE, ASKED OF THE SAME EDGE LIST** — `ParamSet::rackOffsetAt`'s shape: what reaches
	// this destination, each amount times the destination's own span times the source's reading, summed
	// in double. THE SPAN IS `device::routeSpanOf` — a declared `modSpan` wins, a zoned control moves in
	// zone widths, everything else by its range. `amount * source * span`, in that order, so a lane moved
	// onto this engine resolves to the bit.
	double offset = 0.0;
	if (control < slot.inputs.size()) {
		// **THE PULL.** This control's inputs, each pointing at the cell its source owns — a slot's output
		// written when that device ticked earlier in this same pass (the graph's order is what makes
		// "earlier" true), or a macro. No bus, no mask, no lookup: `amount × f(value) × span`, `f` the route's transfer.
		//
		// **FOR A VOICE, THE SOURCE'S OWN CELL FOR THAT VOICE when it has one** (backlog DU: a source
		// declares whether it varies per voice; an envelope or a note expression does, an LFO does not),
		// and the collapsed cell otherwise — so a voice-less resolve and a voice's resolve of an unvarying
		// source are the same number, which is what makes migrating a destination bit-identical until a
		// varying source actually reaches it.
		const InputRange range = slot.inputs[control];
		for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
			const Input& in = inputs_[i];
			// THE EAR (backlog KU): the right reads each input's right-ear cell — a mono source's is its left.
			const float* perVoiceCell = kRight ? in.perVoiceR : in.perVoice;
			const float* cell = voice != kNoVoice && perVoiceCell != nullptr ? perVoiceCell + voice : (kRight ? in.valueR : in.value);
			// `device::routeTerm` — the one spelling the editor's as-modulated readout calls too (backlog PU).
			offset += device::routeTerm(in.amount, in.transfer, relaxedLoadCell(*cell), span);
		}
	}
	// **AND THE HOST'S MODULATION, ON TOP OF EVERY ROUTE** (host slots, T5): a bound slot's normalised offset
	// times the same span, so the ring draws it through the same `resolved` as any route. Added AFTER the
	// routes so a rack with no host mod sums exactly what it always did. Zero for an unbound control.
	offset += static_cast<double>(slot.hostMod[control]) * span;
	// A STEPPED CONTROL LANDS ON A STEP, AND THE RANGE STOPS IT UNLESS THE CONTROL SAYS OTHERWISE —
	// `device::withRouteOffset`, the rule every host shares. Only when something actually reached it, as
	// Feathers has always done: an unrouted control is its base.
	// `device::resolveRouted` is that rule, shared with the readout (backlog PU).
	return static_cast<float>(device::resolveRouted(p, static_cast<double>(value), offset));
}

void RackEngine::resolveInto(Slot& slot, const device::Descriptor& desc) {
	// **THE WHOLE ROW ONLY WHEN SOMETHING A ROUTE DOES NOT CARRY HAS MOVED** (`Slot::baseSerial`). A control with no input
	// resolves from its base, its span and its host offset alone, so with all three as they were at the last whole
	// resolve its `resolved` already holds the number this would compute; only a control an input reaches reads cells
	// that move between calls, and only those are resolved again. The same function either way, so the same bits.
	const bool whole = slot.resolvedBase != slot.baseSerial || slot.resolvedHostMod != slot.hostModSerial
	                   || slot.resolvedRoutes != routesSerial_;
	const auto routed = [&slot](size_t control) {
		return control < slot.inputs.size() && slot.inputs[control].count > 0;
	};
	if (whole) {
		++fullResolves_;
		slot.resolvedBase = slot.baseSerial;
		slot.resolvedHostMod = slot.hostModSerial;
		slot.resolvedRoutes = routesSerial_;
		// AND HOW MANY OF ITS CONTROLS AN INPUT REACHES — fixed until the routes rebuild or the row is reinstalled, both of
		// which make the next resolve a whole one again. None (a Pattern's row, usually): nothing below runs at all.
		uint32_t n = 0;
		for (size_t control = 0; control < slot.base.size(); ++control) {
			n += routed(control) ? 1u : 0u;
		}
		slot.routedControls = n;
	}
	const bool any = whole || slot.routedControls > 0;
	for (size_t control = 0; any && control < slot.base.size(); ++control) {
		if (!whole && !routed(control)) {
			continue;
		}
		const float value = resolveControl(slot, desc, control, kNoVoice);
		// **PUBLISHED, NOT MERELY STORED.** The editor reads this to draw the moving arc
		// (`DeviceHost::modulatedFieldAt`), so the write is a relaxed atomic store exactly as
		// `publishReadings` makes its meter stores — the audio thread writes, the UI thread reads, and
		// a plain write beside a plain read is a data race in the language's own terms whatever it
		// does in practice. `resolved` is sized in the provision BEFORE the slot's `desc` is published
		// and never resized while the slot is live — that ordering is itself the fix for a crash in
		// a user's hands — so it can be the published buffer.
		std::atomic_ref<float>(slot.resolved[control]).store(value, std::memory_order_relaxed);
	}
	// **THE RIGHT EAR, FOR THE CONTROLS A STEREO ROUTE REACHES** (backlog KU). Skipped whole when none does — the
	// zero-cost promise, counted (`rightPassesForTest`). Every other control's right ear is its left, so a device
	// handed this row by `setFieldsR` reads one row either way.
	if (slot.stereoLive.any()) {
		++rightPasses_;
		for (size_t control = 0; any && control < slot.base.size() && control < slot.resolvedR.size(); ++control) {
			if (!whole && !routed(control)) {
				continue; // its right ear is its left, which did not move (a stereo-live control has an input)
			}
			const float value = slot.stereoLive.test(control) ? resolveEar<true>(slot, desc, control, kNoVoice)
			                                                  : slot.resolved[control];
			std::atomic_ref<float>(slot.resolvedR[control]).store(value, std::memory_order_relaxed);
		}
	}
	// **THE DECLARED CONTROLS, PER SAMPLE, FOR THE TICK** (`AudioOps::windowedControls`, backlog FK). A declared
	// control with a MOVING windowed input is resolved at every sample of the tick through the same rule as the
	// scalar above (`withRouteOffset` over `base + Σ amount × source × span`); one whose inputs are all flat is
	// marked flat, and the device reads its `resolved` value as before. Undeclared controls are never touched.
	if (slot.controlWindow.empty() || desc.audio == nullptr) {
		return;
	}
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	for (const uint8_t declared : desc.audio->windowedControls) {
		const auto control = static_cast<size_t>(declared);
		if (control >= slot.base.size() || (control + 1) * stride > slot.controlWindow.size()) {
			continue;
		}
		resolveWindowInto(slot, desc, control, &slot.controlWindow[control * stride], kNoVoice);
	}
}

void RackEngine::resolveWindowInto(Slot& slot, const device::Descriptor& desc, size_t control, float* out,
                                   size_t voice) const {
	// ONE DECLARED CONTROL'S TICK WINDOW, for the collapsed row (`voice == kNoVoice`) or one voice's. An input reads, per
	// sample, the most particular thing it has: for a voice, its source's per-voice window, else that voice's cell; then
	// the source's block-rate window; then its cell. Flat when no input moves — the device then reads its scalar row.
	const double span = control < slot.routeSpan.size() ? slot.routeSpan[control] : -1.0;
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	const bool forVoice = voice != kNoVoice;
	const auto windowOf = [&](const Input& in) -> const float* {
		if (forVoice && in.perVoice != nullptr) {
			return in.perVoiceWindow != nullptr ? in.perVoiceWindow + voice * stride : nullptr;
		}
		return in.window;
	};
	bool moving = false;
	if (span >= 0.0 && control < slot.inputs.size()) {
		const InputRange range = slot.inputs[control];
		for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
			const float* w = windowOf(inputs_[i]);
			if (w != nullptr && !device::tickWindowFlat(w)) {
				moving = true;
				break;
			}
		}
	}
	if (!moving) {
		out[device::kTickWindow] = 1.0f;
		return;
	}
	const device::ParamDesc& p = desc.params[control];
	const InputRange range = slot.inputs[control];
	for (int s = 0; s < device::kTickWindow; ++s) {
		double offset = 0.0;
		for (size_t i = range.first; i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
			const Input& in = inputs_[i];
			const float* w = windowOf(in);
			const float v = w != nullptr ? w[s]
			                : forVoice && in.perVoice != nullptr ? relaxedLoadCell(in.perVoice[voice])
			                                                     : relaxedLoadCell(*in.value);
			offset += device::routeTerm(in.amount, in.transfer, v, span);
		}
		// THE HOST'S MODULATION, held for the tick — the scalar resolve's rule (`resolveControl`).
		offset += static_cast<double>(slot.hostMod[control]) * span;
		out[s] = static_cast<float>(device::resolveRouted(p, static_cast<double>(slot.base[control]), offset));
	}
	out[device::kTickWindow] = 0.0f;
}

void RackEngine::resolveVoiceInto(Slot& slot, const device::Descriptor& desc, size_t voice) {
	// THE ROW FOR ONE VOICE. Only a control with a per-voice input can differ from the collapsed row, so
	// every other control is the collapsed value copied — the same number the voice was handed before
	// this existed, which is the "default trivial handling" DU's design asked for.
	for (size_t control = 0; control < slot.base.size() && control < slot.resolvedVoice.size(); ++control) {
		bool varies = false;
		if (control < slot.inputs.size()) {
			const InputRange range = slot.inputs[control];
			for (size_t i = range.first; !varies && i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
				varies = inputs_[i].perVoice != nullptr;
			}
		}
		slot.resolvedVoice[control] = varies ? resolveControl(slot, desc, control, voice) : slot.resolved[control];
	}
	// AND THE VOICE'S RIGHT EAR (backlog KU), only for a slot a stereo route reaches: a control that is not stereo-live
	// is its left; one that is takes its own per-voice right resolve when a per-voice input reaches it, the collapsed
	// right ear otherwise.
	if (slot.stereoLive.any()) {
		++rightPasses_;
		for (size_t control = 0; control < slot.base.size() && control < slot.resolvedVoiceR.size(); ++control) {
			bool varies = false;
			if (control < slot.inputs.size()) {
				const InputRange range = slot.inputs[control];
				for (size_t i = range.first; !varies && i < static_cast<size_t>(range.first) + range.count && i < inputs_.size(); ++i) {
					varies = inputs_[i].perVoice != nullptr;
				}
			}
			slot.resolvedVoiceR[control] = !slot.stereoLive.test(control) ? slot.resolvedVoice[control]
			                               : varies                        ? resolveEar<true>(slot, desc, control, voice)
			                                                               : slot.resolvedR[control];
		}
	}
	// AND THE VOICE'S DECLARED CONTROLS, PER SAMPLE (backlog FK) — the collapsed windows serve a voice no per-voice input
	// reaches, so only a slot with one fills these (`processVoice` picks).
	if (slot.controlWindowVoice.empty() || desc.audio == nullptr || !slot.anyPerVoiceInput) {
		return;
	}
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	const size_t width = slot.base.size();
	for (const uint8_t declared : desc.audio->windowedControls) {
		const auto control = static_cast<size_t>(declared);
		const size_t at = (voice * width + control) * stride;
		if (control >= width || at + stride > slot.controlWindowVoice.size()) {
			continue;
		}
		resolveWindowInto(slot, desc, control, &slot.controlWindowVoice[at], voice);
	}
}

float RackEngine::resolvedFieldForVoiceForTest(size_t slot, size_t control, size_t voice) {
	if (slot >= slots_.size() || voice >= voices_) {
		return 0.0f;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || control >= s.resolvedVoice.size()) {
		return 0.0f;
	}
	if (routesDirty_) {
		rebuildInputs();
	}
	resolveInto(s, *d);
	resolveVoiceInto(s, *d, voice);
	return s.resolvedVoice[control];
}

float RackEngine::resolvedFieldForTest(size_t slot, size_t control) const {
	return modulatedFieldAt(slot, control);
}

float RackEngine::modulatedFieldAt(size_t slot, size_t control) const {
	const Slot& s = slotAtLane(slot);
	if (control >= s.resolved.size()) {
		// **THE BASE, NOT ZERO** — a control the resolved row does not reach is unmodulated, not
		// silent, and `fieldAt` is where the base lives. Answering 0 here would draw every out-of-range
		// control's arc at the bottom of its travel.
		return fieldAt(slot, control);
	}
	// THE READER'S HALF OF THE STORE IN `resolveInto`.
	return std::atomic_ref<float>(const_cast<float&>(s.resolved[control]))
	    .load(std::memory_order_relaxed);
}

void RackEngine::modulatedRowAt(size_t slot, float* out, size_t count) const {
	const Slot& s = slotAtLane(slot);
	for (size_t control = 0; control < count; ++control) {
		// `modulatedFieldAt` exactly: the resolved row where it reaches, the base (`fieldAt`) past it, 0 past both.
		out[control] = control < s.resolved.size()
		                   ? std::atomic_ref<float>(const_cast<float&>(s.resolved[control])).load(std::memory_order_relaxed)
		                   : (control < s.base.size() ? s.base[control] : 0.0f);
	}
}

namespace {

/// `Slot::stereoLive`, READ FROM ANOTHER THREAD — the reader's half of the drain's word-wise `atomic_ref` stores.
bool stereoLiveRelaxed(const Slot& s, size_t control) noexcept {
	if (control >= device::kControlMaskBits) {
		return false;
	}
	const uint64_t word = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(s.stereoLive.words[control >> 6]))
	                          .load(std::memory_order_relaxed);
	return ((word >> (control & 63)) & 1u) != 0;
}

} // namespace

float RackEngine::resolvedFieldRForTest(size_t slot, size_t control) const {
	float right = 0.0f;
	// THE RIGHT EAR OF A MONO CONTROL IS ITS LEFT.
	return modulatedFieldRAt(slot, control, right) ? right : modulatedFieldAt(slot, control);
}

float RackEngine::resolvedFieldRForVoiceForTest(size_t slot, size_t control, size_t voice) {
	if (slot >= slots_.size() || voice >= voices_) {
		return 0.0f;
	}
	Slot& s = slots_[slot];
	const device::Descriptor* d = s.live_desc();
	if (d == nullptr || control >= s.resolvedVoiceR.size()) {
		return 0.0f;
	}
	if (routesDirty_) {
		rebuildInputs();
	}
	resolveInto(s, *d);
	resolveVoiceInto(s, *d, voice);
	return s.stereoLive.test(control) ? s.resolvedVoiceR[control] : s.resolvedVoice[control];
}

bool RackEngine::modulatedFieldRAt(size_t slot, size_t control, float& right) const {
	// **THE EDITOR'S RIGHT EAR** — true and the value while a stereo route reaches the control, false otherwise
	// (draw the mono ring). The mask and the row are both read relaxed; a frame late at worst, as the left is.
	const Slot& s = slotAtLane(slot);
	if (control >= s.resolvedR.size() || !stereoLiveRelaxed(s, control)) {
		return false;
	}
	right = std::atomic_ref<float>(const_cast<float&>(s.resolvedR[control])).load(std::memory_order_relaxed);
	return true;
}

//-------------------------------------------------------------------------------------------------//
// HOST SLOTS (T5) — see the banner in the header                                                    //
//-------------------------------------------------------------------------------------------------//

void RackEngine::clearBoundHostMod() noexcept {
	for (size_t i = 0; i < boundCount_; ++i) {
		const BoundSlot& b = bound_[i];
		if (b.lane < slots_.size()) {
			slots_[b.lane].hostMod[b.control] = 0.0f;
			++slots_[b.lane].hostModSerial;
		}
	}
}

void RackEngine::setHostSlotBus(SlotBus* bus) noexcept {
	clearBoundHostMod();
	hostSlotBus_ = bus;
	lastGen_.fill(0);
}

void RackEngine::setHostSlotBindings(std::span<const SlotBinding> bindings) noexcept {
	// THE OLD BINDINGS' OFFSETS GO FIRST: a slot unbound (or bound elsewhere) must not leave its last
	// modulation standing on the control it used to drive. The lane may hold another device by now; zeroing
	// one of its offsets is harmless, because the block rewrites every offset a current binding owns.
	clearBoundHostMod();
	boundCount_ = 0;
	for (size_t n = 0; n < bindings.size(); ++n) {
		const SlotBinding& b = bindings[n];
		// ENTRY `n` IS SLOT `n` (`ChainState::setSlotBinding` refuses anything else), so one slot drives one
		// control and `lastGen_[slot]` has one reader. Checked again here for a table built some other way.
		if (!b.live || b.slot != n || b.slot >= hostslots::kSlotBusCapacity || boundCount_ >= bound_.size()) {
			continue;
		}
		const size_t lane = slotOf(b.device);
		if (lane >= slots_.size()) {
			continue; // not running here (yet) — a later drain resolves it
		}
		const device::Descriptor* d = slots_[lane].live_desc();
		if (d == nullptr || b.control >= d->params.size() || b.control >= slots_[lane].hostMod.size()) {
			continue; // a control this device does not declare: not a control, so nothing to drive
		}
		bound_[boundCount_++] = BoundSlot { .lane = lane, .device = b.device, .control = b.control, .slot = b.slot };
	}
	// **EVERY DRAIN FORGETS WHAT IT APPLIED** (plan §0.3). The converger has just written every row from
	// the record, which may be older than the host's last write; with `lastGen` at zero, the next
	// `applyHostSlots` puts every written slot's value back over it. This is THE line the stale-record case
	// exists for.
	lastGen_.fill(0);
}

void RackEngine::applyHostSlots() noexcept {
	SlotBus* const bus = hostSlotBus_;
	if (bus == nullptr) {
		return;
	}
	for (size_t i = 0; i < boundCount_; ++i) {
		const BoundSlot& b = bound_[i];
		if (b.lane >= slots_.size()) {
			continue;
		}
		Slot& s = slots_[b.lane];
		// THE DEVICE, NOT THE ADDRESS: a lane emptied and refilled outside a drain holds another device,
		// and this binding is not its.
		const device::Descriptor* d = s.live_desc();
		if (s.id != b.device || d == nullptr || b.control >= d->params.size() || b.control >= s.base.size()) {
			continue;
		}
		// ONE SNAPSHOT — the value with the generation that wrote it (`SlotValueBus::load`), so the generation
		// remembered is never one whose value was not the one applied.
		const SlotBus::Snapshot seen = bus->load(b.slot);
		if (seen.gen != lastGen_[b.slot]) {
			// THE HOST'S WRITE — DAW automation — waits its turn on a late device as an edit does (backlog LW step 3).
			const float coerced = device::coerceControl(d->params[b.control], seen.value);
			if (!deferField(s, b.lane, *d, b.control, coerced)) {
				const bool wasZero = s.base[b.control] == 0.0f;
				s.base[b.control] = coerced;
				++s.baseSerial; // as `writeBaseControl`
				if (wasZero != (s.base[b.control] == 0.0f) && pairsFollow(*d, b.control)) {
					routesDirty_ = true; // a pair switched by the host (`SourceDesc::pairWhile`) — as `setField`
				}
			}
			lastGen_[b.slot] = seen.gen;
		}
		// WRITTEN EVERY BLOCK, COUNTED ONLY WHEN IT MOVES (`Slot::hostModSerial`), compared as bits so a −0 or a NaN
		// replacing its own bits is no change and anything else is one.
		const float mod = bus->modOf(b.slot);
		if (std::bit_cast<uint32_t>(mod) != std::bit_cast<uint32_t>(s.hostMod[b.control])) {
			s.hostMod[b.control] = mod;
			++s.hostModSerial;
		}
	}
}

void RackEngine::reset() noexcept {
	passSoundingCount_ = 0; // the voices downstream reset with the devices: nothing passed is sounding any more
	// THE LATE DEVICES' NOTE HISTORY GOES WITH THE TAILS (backlog LW): what they still owed is gone with the audio it
	// belonged to, so they take the note as it is from here.
	historyHead_ = 0;
	historyCount_ = 0;
	landDueFields(true); // and every write still waiting lands now, as the device it was for is reset
	for (Slot& slot : slots_) {
		// ONE LOAD, HELD — see processStereo's walk for the crash that rule came from.
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || slot.storage == nullptr) {
			continue;
		}
		if (d->audio != nullptr && d->audio->reset != nullptr) {
			d->audio->reset(slot.storage.get());
			for (auto& voice : slot.voices) {
				d->audio->reset(voice.get());
			}
		} else if (d->mod != nullptr && d->mod->reset != nullptr) {
			d->mod->reset(slot.storage.get());
		}
		// THE FURNITURE SETTLES WHERE IT WAS AIMED, ITS DRY FORGETS, AND THE SITE DROPS ITS TAILS (backlog NR).
		const auto settle = [](FurnitureRamp& ramp) {
			ramp.wet = ramp.wetTarget;
			ramp.dry = ramp.dryTarget;
			ramp.remaining = 0;
		};
		settle(slot.furniture);
		for (FurnitureRamp& ramp : slot.furnitureVoice) {
			settle(ramp);
		}
		slot.furnitureDelay.reset();
		if (SiteChains* site = slot.site.load(std::memory_order_acquire); site != nullptr) {
			site->reset();
		}
		// A RESET NODE IS AT REST AND PUBLISHES SO — a downstream input must not read the value it was
		// holding until the next tick republishes. Same stores the publish makes, same reason.
		for (float& cell : slot.outputs) {
			std::atomic_ref<float>(cell).store(0.0f, std::memory_order_relaxed);
		}
		// AND ITS METER FALLS TO REST WITH IT (backlog MK) — still metered, now at silence.
		slot.outputChunkPeak = 0.0f;
		slot.outputHeld = 0.0f;
		slot.outputHoldLeft = 0;
		if (slot.outputPeak.load(std::memory_order_relaxed) > 0.0f) {
			slot.outputPeak.store(0.0f, std::memory_order_relaxed);
		}
	}
}

void RackEngine::resetVoice(size_t voice) noexcept {
	// EVERY SLOT, NOT ONLY THE ONES RUNNING PER VOICE THIS BLOCK — `FxRunner::resetInstance`'s rule. An
	// instance that is not responsible now may be the moment the switch flips, and it must not open on
	// the note before.
	for (Slot& slot : slots_) {
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || d->audio == nullptr || d->audio->reset == nullptr
		    || voice >= slot.voices.size()) {
			continue;
		}
		d->audio->reset(slot.voices[voice].get());
	}
}

void RackEngine::processStereo(float* left, float* right, int frames,
                               const backplane::BlockContext& block) {
	if (left == nullptr || right == nullptr || frames <= 0) {
		anchorCount_ = 0;
		return;
	}
	// A HOST WITHOUT VOICES: THE BLOCK CUT ON THE CONTROL TICK (backlog FK), each piece its beginning, one bus
	// pass and its end — the grid Feathers renders its lane on. Every device that is not running per voice runs
	// in `processBus`, and in a rack with no voices that is every device. Resolved once per HOST block, a route
	// was a stair as long as the host's buffer and the sound depended on the buffer size; on the tick, every
	// modulator advances a whole tick at a tick's start, a row is resolved per tick, and a device that reads
	// windows is handed its piece of the tick. The song clock moves forward by what has been rendered, as
	// Feathers' `rackClock()` does.
	//
	// **AND EACH NOTE IS OFFERED IN THE PIECE THAT HOLDS IT, AT ITS OFFSET IN THAT TICK.** The notes used to
	// run once, in the first piece, at host-block offsets — so every device that acts on the tick it is told
	// of a note (DAHDSR does; no device read the offset) started a note at 300 four ticks early, and the
	// held stack, which `updateNoteContext` folds from what left the chain in EVERY piece, pushed each
	// note-on once per piece. `timeSamples` now means what it means in Feathers' lane, where a note is
	// delivered at its sample with its offset in the control tick (`Synth::noteOffsetInTick`): one meaning
	// in both hosts. What leaves the chain is put back in host-block time for `blockNotesOut`, so the note
	// port hands the host the offsets it sent.
	const std::vector<device::NoteEvent>& sent = notesInA_ ? notesA_ : notesB_;
	const size_t sentCount = std::min(notesCount_, kMaxNotes);
	std::copy_n(sent.begin(), sentCount, blockNotesIn_.begin());
	size_t nextNote = 0;
	size_t leftCount = 0;
	backplane::BlockContext piece = block;
	backplane::BlockContext anchor = block; // the clock the pieces are measured from — see the loop
	int anchorAt = 0;
	size_t nextAnchor = 0;
	int done = 0;
	// THE SIDECHAIN AT THE BLOCK'S START, offset per piece below (the same frames the bus runs).
	const float* sideL = sideL_;
	const float* sideR = sideR_;
	while (done < frames) {
		const int phase = hostTickPhase_;
		const int m = std::min(frames - done, device::kTickWindow - phase);
		// THIS PIECE'S NOTES: those before its end, and in the last piece everything left — a host offset
		// past the block (CLAP forbids it; a host may send it) plays at the block's last sample rather than
		// being dropped, because a lost off is a stuck note. In the host's order, which is the stream's.
		const bool lastPiece = done + m >= frames;
		notesInA_ = true;
		notesCount_ = 0;
		while (nextNote < sentCount
		       && (lastPiece || blockNotesIn_[nextNote].timeSamples < static_cast<uint32_t>(done + m))) {
			device::NoteEvent e = blockNotesIn_[nextNote++];
			// CLAMPED INTO THE PIECE, so an out-of-order or past-the-block time cannot rebase below zero or
			// past the tick.
			const uint32_t t = std::clamp(e.timeSamples, static_cast<uint32_t>(done),
			                              static_cast<uint32_t>(done + m - 1));
			e.timeSamples = t - static_cast<uint32_t>(done) + static_cast<uint32_t>(phase);
			notesA_[notesCount_++] = e;
		}
		const int pieceStart = done;
		// **A TRANSPORT THE HOST SENT INSIDE THE BLOCK RE-ANCHORS FROM HERE ON** (backlog NJ (b)): the latest one at
		// or before this piece's start is the clock, moved on by what has been rendered since it — its tempo, its
		// position, whether it plays. Before any, the block's own.
		while (nextAnchor < anchorCount_ && anchors_[nextAnchor].at <= done) {
			anchor = anchors_[nextAnchor].block;
			anchorAt = std::max(0, anchors_[nextAnchor].at);
			++nextAnchor;
		}
		// **CUT WHERE A LATE DEVICE'S NOTE CHANGES** (backlog LW): the piece runs in parts, each ending where the note
		// a device `A(n)` late is handed moves, so it moves on its own sample. The parts after the first are what a host
		// cutting its block there would have run: no notes of their own (the piece's went with its first part, as they
		// always did), no modulator tick. Nothing late, one part — the piece it always was.
		int part = 0;
		while (part < m) {
			const int at = done + part;
			if (at > 0 || anchorAt > 0 || nextAnchor > 0) {
				piece.clock = advanced(anchor.clock, static_cast<double>(at - anchorAt) / sampleRate_);
			}
			const int partPhase = (phase + part) % device::kTickWindow;
			if (part > 0) {
				notesInA_ = true;
				notesCount_ = 0;
			}
			beginPiece(part == 0 && phase == 0 ? device::kTickWindow : 0, piece, true);
			int len = m - part;
			if (maxArrival_ > 0) {
				const uint32_t until = samplesUntilLateNote();
				if (until > 0 && until < static_cast<uint32_t>(len)) {
					len = static_cast<int>(until);
					++lateWork_;
				}
			}
			// WHAT LEFT THE CHAIN THIS PIECE, BACK IN HOST-BLOCK TIME. A device's offset is taken as inside
			// this tick; one it wrote past the piece is kept as written rather than wrapped.
			for (const device::NoteEvent& e : blockNotesOut()) {
				if (leftCount >= kMaxNotes) {
					break;
				}
				device::NoteEvent back = e;
				back.timeSamples = static_cast<uint32_t>(
				    std::max<int64_t>(0, static_cast<int64_t>(e.timeSamples) - phase + pieceStart));
				blockNotesLeft_[leftCount++] = back;
			}
			// AND WHAT THE SELF-CLOCKED DEVICES MAKE IN THIS PIECE, after its host notes reached them (an arp hears a chord
			// change in the piece it falls in, before it is asked for a step there).
			emitPiece(piece.clock, at, len, leftCount);
			tickPhase_ = partPhase;
			sideL_ = sideL != nullptr ? sideL + at : nullptr;
			sideR_ = sideR != nullptr ? sideR + at : nullptr;
			processBus(left + at, right + at, len);
			endBlock();
			part += len;
		}
		hostTickPhase_ = (phase + m) % device::kTickWindow;
		done += m;
	}
	// THE BLOCK'S OUTPUT IS THE STREAM `blockNotesOut` REPORTS, as it was when the notes ran once.
	anchorCount_ = 0; // a mid-block transport belongs to the block it came in
	sideL_ = nullptr; // the host's buffers end with its call: nothing may read them after it
	sideR_ = nullptr;
	// IN TIME ORDER, which CLAP asks of an output queue: a piece's emitted notes were appended after its host notes, so
	// they can sit ahead of a later host note in the same piece. An insertion sort, STABLE so two events at one sample keep
	// the order they left the chain in (an off before the on it makes room for), and with no allocation on this thread.
	for (size_t i = 1; i < leftCount; ++i) {
		const device::NoteEvent e = blockNotesLeft_[i];
		size_t j = i;
		while (j > 0 && blockNotesLeft_[j - 1].timeSamples > e.timeSamples) {
			blockNotesLeft_[j] = blockNotesLeft_[j - 1];
			--j;
		}
		blockNotesLeft_[j] = e;
	}
	for (size_t i = 0; i < leftCount; ++i) {
		notesA_[i] = blockNotesLeft_[i];
	}
	notesInA_ = true;
	notesCount_ = leftCount;
}

size_t RackEngine::inventedHeldForTest() const noexcept {
	size_t n = 0;
	for (const auto& channel : inventedHeld_) {
		for (const bool held : channel) {
			n += held ? 1 : 0;
		}
	}
	return n;
}

size_t RackEngine::inventedOffsInto(device::NoteEvent* out, size_t room, uint32_t at) noexcept {
	size_t n = 0;
	for (size_t ch = 0; ch < inventedHeld_.size(); ++ch) {
		for (size_t key = 0; key < inventedHeld_[ch].size(); ++key) {
			if (!inventedHeld_[ch][key] || n >= room) {
				continue; // a full buffer keeps the note held, so the next call ends it: delayed, never lost
			}
			device::NoteEvent off {};
			off.kind = device::NoteEvent::Kind::Off;
			off.timeSamples = at;
			off.noteId = -1;
			off.channel = static_cast<int16_t>(ch);
			off.key = static_cast<int16_t>(key);
			off.velocity = 0.0;
			out[n++] = off;
			inventedHeld_[ch][key] = false;
		}
	}
	return n;
}

void RackEngine::appendInventedOffs(uint32_t at, size_t& leftCount) noexcept {
	leftCount += inventedOffsInto(blockNotesLeft_.data() + leftCount, kMaxNotes - std::min(leftCount, kMaxNotes), at);
}

void RackEngine::emitPiece(const device::TransportClock& clock, int pieceStart, int frames, size_t& leftCount) noexcept {
	// AN ACTIVATE RESET THE ARP, which forgot what it sounded; the instrument downstream did not. Ended first.
	if (flushInvented_) {
		appendInventedOffs(static_cast<uint32_t>(pieceStart), leftCount);
		flushInvented_ = inventedHeldForTest() > 0;
	}
	int at = 0;
	while (at < frames) {
		const device::TransportClock now = at == 0 ? clock : advanced(clock, static_cast<double>(at) / sampleRate_);
		uint32_t until = std::numeric_limits<uint32_t>::max();
		// AT MOST THE ROOM LEFT, so every event `emitNotesNow` counts as sounding (or ended) is one the host is handed.
		const size_t made = emitNotesNow(now, emitSample_ + static_cast<uint64_t>(at), emitScratch_.data(),
		                                 std::min(emitScratch_.size(), kMaxNotes - leftCount), &until);
		for (size_t i = 0; i < made; ++i) {
			device::NoteEvent e = emitScratch_[i];
			e.timeSamples = static_cast<uint32_t>(pieceStart + at);
			blockNotesLeft_[leftCount++] = e;
		}
		if (until == std::numeric_limits<uint32_t>::max()) {
			break; // nothing in the chain emits: one cheap walk a piece
		}
		at += static_cast<int>(std::min<uint32_t>(until, static_cast<uint32_t>(frames - at)));
	}
	emitSample_ += static_cast<uint64_t>(frames);
}

void RackEngine::beginBlock(int frames, const backplane::BlockContext& block) {
	beginPiece(frames, block, true);
}

void RackEngine::beginPiece(int frames, const backplane::BlockContext& block, bool notes) {
	const StageScope stage(Stage::Resolve); // the spike recorder's, at the top of the tree only (backlog SR)
	// THE BLOCK IS IN FLIGHT FROM HERE UNTIL `endBlock`, and `blockEpoch_` says so: odd while inside,
	// even while out. The editor reads it before destructing anything this block might be holding —
	// see `blockPassed`.
	//
	// `processStereo` guards its null/zero-frame case BEFORE calling this, so an early return cannot
	// leave the epoch odd forever, which would park every future removal permanently. A host calling
	// this directly owes the matching `endBlock` — see the header.
	blockEpoch_.fetch_add(1, std::memory_order_seq_cst);
	blockContext_ = block;
	anyPerVoice_ = false;
	perVoiceLatency_ = 0;
	anyPerVoiceStereo_ = false;
	// THE WRITES WHOSE TURN HAS COME (backlog LW step 3), before anything below reads a row.
	landDueFields(false);
	// THE ROUTES, REPUBLISHED ONLY IF THE DRAIN MOVED THEM — before anything below resolves a row.
	if (routesDirty_) {
		rebuildInputs();
	}
	// **THE HOST SLOTS INTO THE BASES, BEFORE ANYTHING BELOW RESOLVES A ROW** (host slots, T5) — so a host
	// write that arrived in this `process` moves its control in this block, modulators' own controls
	// included. See `applyHostSlots`.
	applyHostSlots();

	// NOTES BEFORE MODULATORS, because a modulator's gate and a tracker's pitch are both facts about
	// THIS block's notes: running the stream after them would make every note-driven source a block
	// late, which is the same error the modulators-before-audio order exists to avoid.
	if (notes) {
		advanceNotes();
	}

	// MODULATORS FIRST, so an audio device is resolved against what the sources are putting out THIS
	// block rather than last one. A rack that ran them the other way round would be a block late on
	// every route, which is inaudible on a slow envelope and obvious on a fast one.
	// THE NOTES FIRST, so a device that starts this block on a new note acts on it rather than on the
	// last one. Same reasoning `FxRunner` gives for delivering a note NOW rather than at the next sync.
	updateNoteContext();
	advanceModulators(frames, block);

	// **WALKED IN CHAIN ORDER, THROUGH THE PUBLISHED SNAPSHOT — never `graph().chain()` directly.**
	//
	// `graph.h`'s contract: `chain(c)` is a span into storage the message thread rewrites, so the
	// audio thread reads a copy published per block instead. `orderFront_` is acquired ONCE here and
	// held for the whole walk, so a mutator landing mid-block cannot make this loop see half of one
	// order and half of another.
	//
	// TWO PASSES, PER CAPABILITY, WHICH IS THE CONTRACT'S OWN SHAPE: the notes and the modulators have
	// already run above, and this is the audio pass. A device that is both is ticked there and run
	// here, which is exactly where `order()` would put its two roles.
	//
	// THE CHAIN LOOP RUNS ONCE TODAY and is written as a loop anyway — a rack holding chains in
	// parallel becomes a constructor argument rather than a rewrite of this block.
	//
	// **ACQUIRED ONCE FOR THE WHOLE BLOCK, not once per pass.** The voice passes and the bus pass read
	// the front this acquired, so one block cannot run its voices in one order and its bus in another.
	blockFront_ = orderFront_.load(std::memory_order_acquire);
	const OrderSnapshot& order = orderSnapshot_[blockFront_];
	// **EACH SLOT'S ARRIVAL LATENCY `A(n)`** (backlog LW, step 1), summed in this walk, which asks every audio device its
	// latency anyway: the declared latency of everything before it in path order, its own site's PRE before it and its
	// POST after it. The engine's chains run one after another on one buffer (`latencySamples`), so the sum is serial;
	// a Rack's chains each sum their own inside it, and a container's inner devices add theirs to what it is handed.
	// **TWO TIMELINES, AS THE AUDIO RUNS**: a device running inside the voices sums only the per-voice devices before it
	// (the voices run before the bus, whatever the order says); a bus device sums the bus devices before it and, once
	// the walk knows it, the whole per-voice road (`perVoiceLatency_`), which every voice reaches the bus that late by.
	uint32_t upstream = voiceShare_; // the bus's: every voice reaches it the per-voice road late (last piece's; fixed below)
	uint32_t voiceUp = 0;            // the voices'
	uint32_t latest = 0;
	bool moved = false;
	bool voiceLate = false;
	const auto arrive = [&](size_t i, bool inVoice, bool hasSite, uint32_t pre, uint32_t own, uint32_t post) {
		Arrival& a = (*arrival_)[i];
		uint32_t& up = inVoice ? voiceUp : upstream;
		moved = moved || a.at != up + pre;
		a.at = up + pre;
		voiceLate = voiceLate || (inVoice && a.at > 0);
		a.post = hasSite ? a.at + own : a.at; // a POST lane hears the device's output; no site, no lane
		const uint64_t after = static_cast<uint64_t>(a.at) + own + post;
		up = static_cast<uint32_t>(std::min<uint64_t>(after, UINT32_MAX));
		latest = std::max({ latest, a.at, a.post });
	};
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
		const size_t i = path.at(step);
		if (i >= slots_.size()) {
			continue;
		}
		Slot& slot = slots_[i];
		if (i < arrival_->capacity() && !slot.isAudio()) {
			// A MODULATOR OR A NOTE DEVICE has the arrival of the audio at its place; an audio device's is set below,
			// once its own latency and its site's are known (`arrive`).
			Arrival& a = (*arrival_)[i];
			a.at = upstream;
			a.post = upstream;
		}
		// ONE LOAD OF `desc`, HELD FOR THE WHOLE SLOT. `isAudio()`/`isMod()` load the atomic and then
		// every `live_desc()` after them loads it AGAIN — and the editor can clear it in between, so
		// the check said "yes" and the dereference got a nullptr. That is what crashed the removal
		// case with SIGSEGV, and it is CLAUDE.md's pointer rule read from the other end: comparing two
		// pointers read in the SAME pass is fine, and this was two passes pretending to be one.
		const device::Descriptor* d = slot.live_desc();
		if (d == nullptr || slot.storage == nullptr || d->audio == nullptr) {
			slot.polyThisBlock = false;
			continue;
		}
		const device::AudioOps& ops = *d->audio;
		// RESOLVE THE WHOLE ROW BEFORE THE DEVICE SEES ANY OF IT — once per block, for whichever
		// instances run it.
		resolveInto(slot, *d);

		// **WHERE THIS DEVICE RUNS THIS BLOCK: inside each voice, or once on the bus.** Only a slot
		// that HAS voice instances can be per-voice; in a rack with no voices the Poly switch is a
		// setting the device reads and the engine runs it on the bus.
		// **A TWO-STAGE DEVICE DECIDES FOR ITSELF** (rack roadmap §8, the pass-through model): a container
		// runs this engine's two stages inside itself on ONE instance. Its fields for the block are set
		// now — the bus stage sets them again, harmlessly — so its begin-block reads current macros, and
		// it answers whether anything inside it is per voice this block. No voice instances, no resets.
		// `frames` is not handed on: it is what THIS engine's modulators advance by, and a host that
		// ticks its own strip on its own grid passes zero here (Feathers' lane) — the container clocks
		// its inner modulators off the audio its bus stage is handed (`AudioOps::beginPerVoiceBlock`).
		// ITS SITE'S TWO LANES, either side of it in series (backlog NR) — for `A(n)`.
		const SiteChains* site = slot.site.load(std::memory_order_acquire);
		const uint32_t sitePre = site != nullptr ? site->pre().latencySamples(sampleRate_) : 0u;
		const uint32_t sitePost = site != nullptr ? site->post().latencySamples(sampleRate_) : 0u;
		if (ops.beginPerVoiceBlock != nullptr && ops.processVoice != nullptr) {
			if (i < arrival_->capacity()) {
				arrive(i, false, site != nullptr, sitePre, latencyOf(slot, *d, sampleRate_), sitePost);
			}
			ops.setFields(slot.storage.get(), slot.resolved.data(), slot.resolved.size());
			if (ops.setTransportClock != nullptr) {
				// A LATE CONTAINER'S CLOCK IS ITS OWN (step 3) — the bus stage sets it again from `clockFor`.
				const uint32_t a = i < arrival_->capacity() ? (*arrival_)[i].at : 0u;
				ops.setTransportClock(slot.storage.get(),
				                      maxArrival_ > 0 && a > 0 ? clockAt(busClock_ >= a ? busClock_ - a : 0) : block.clock);
			}
			const bool inside = ops.beginPerVoiceBlock(slot.storage.get());
			slot.polyThisBlock = inside;
			slot.polyLastBlock = inside;
			anyPerVoice_ = anyPerVoice_ || inside;
			continue;
		}

		// **A LATENT DEVICE IS NEVER PER VOICE** (backlog JM, assumption L2): its Poly switch reads as off
		// while it declares a latency, so every latency is on the bus, where its compensation is — UNLESS it opts in
		// (`Traits::latentPerVoice`) and this host compensates per voice up to `latentPerVoiceCap_` IN ALL — a device that would
		// take the sum past it runs on the bus as L2 says, Poly read as off — and then its latency is
		// the per-voice road's, summed into `perVoiceLatency_` for the host to delay each voice's gain by.
		const uint32_t latency = latencyOf(slot, *d, sampleRate_);
		const bool latentOk = latency == 0
		                      || (d->traits.latentPerVoice && perVoiceLatency_ + static_cast<uint64_t>(latency) <= latentPerVoiceCap_);
		const bool poly = !slot.voices.empty() && polyInRow(*d, slot.resolved) && latentOk;
		if (i < arrival_->capacity()) {
			arrive(i, poly, site != nullptr, sitePre, latency, sitePost);
		}
		if (poly) {
			perVoiceLatency_ += latency;
		}
		if (poly != slot.polyLastBlock && ops.reset != nullptr) {
			// **THE INSTANCES THAT STOP BEING RESPONSIBLE GO QUIET** — `FxRunner::sync`'s rule, which it
			// measured: without it the Poly switch is a delay line, and a round trip replays the tail
			// the old instances were holding. State goes, settings stay; `reset` allocates nothing.
			if (poly) {
				ops.reset(slot.storage.get());
			}
			else {
				for (auto& voice : slot.voices) {
					ops.reset(voice.get());
				}
			}
		}
		slot.polyThisBlock = poly;
		slot.polyLastBlock = poly;
		if (!poly) {
			continue;
		}
		anyPerVoice_ = true;
		// **WHAT THE VOICES SHARE, KEPT BY THE BUS INSTANCE** (`AudioOps::beginSharedBlock`, backlog OM): the collapsed row and
		// the block's frames, before any voice reads it this block through `pullHandBlocks`.
		if (ops.beginSharedBlock != nullptr) {
			ops.beginSharedBlock(slot.storage.get(), slot.resolved.data(), slot.resolved.size(), frames);
		}
		// **A STEREO-LIVE CONTROL IN A VOICE NEEDS A RIGHT CHANNEL TO PUT IT IN** (backlog KU): the host's chunk choice
		// reads this, or a mono chunk hands `processMono` a voice whose right ear has nowhere to go.
		anyPerVoiceStereo_ = anyPerVoiceStereo_ || slot.stereoLive.any();
		// THE SETTINGS AND THE CLOCK ONCE PER BLOCK FOR EVERY VOICE — one row, because the row is the
		// slot's. The note is per voice and per chunk, so it waits for `processVoice`. A voice with no
		// note this block is simply never processed, and holds settings it will be given again.
		for (size_t vi = 0; vi < slot.voices.size(); ++vi) {
			auto& voice = slot.voices[vi];
			// PER VOICE WHEN A PER-VOICE SOURCE REACHES THIS DEVICE (backlog DU, 2026-09-13) — the row resolved
			// with that voice's own cells; the collapsed row otherwise, which is the row every voice got before.
			if (slot.anyPerVoiceInput) {
				resolveVoiceInto(slot, *d, vi);
				ops.setFields(voice.get(), fieldsFor(*d, slot.resolvedVoice), slot.resolvedVoice.size());
				if (ops.pullHandBlocks != nullptr) {
					ops.pullHandBlocks(voice.get(), slot.storage.get());
				}
			}
			else {
				ops.setFields(voice.get(), fieldsFor(*d, slot.resolved), slot.resolved.size());
				if (ops.pullHandBlocks != nullptr) {
					ops.pullHandBlocks(voice.get(), slot.storage.get());
				}
			}
			// THE RIGHT EAR (backlog KU) — `resolvedVoiceR` is the row `resolveVoiceInto` just resolved for THIS voice.
			if (slot.stereoLive.any() && ops.setFieldsR != nullptr) {
				const std::vector<float>& rowR = slot.anyPerVoiceInput ? slot.resolvedVoiceR : slot.resolvedR;
				ops.setFieldsR(voice.get(), rowR.data(), rowR.size(), slot.stereoLive);
			}
			if (ops.setTransportClock != nullptr) {
				ops.setTransportClock(voice.get(), block.clock);
			}
		}
		}
	}
	// THE BUS DEVICES' SHARE OF THE PER-VOICE ROAD, corrected now that it is known — only when it moved (a latent device
	// turning per voice, or its latency changing), so every other piece takes no second walk.
	if (perVoiceLatency_ != voiceShare_) {
		const int64_t delta = static_cast<int64_t>(perVoiceLatency_) - static_cast<int64_t>(voiceShare_);
		const auto shift = [delta](uint32_t v) {
			return static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(v) + delta, 0, UINT32_MAX));
		};
		latest = 0;
		for (const ChainOrder& path : order.chain) {
			for (size_t step = 0; step < path.count; ++step) {
				const size_t i = path.at(step);
				if (i >= slots_.size() || i >= arrival_->capacity() || !slots_[i].isAudio()) {
					continue;
				}
				Arrival& a = (*arrival_)[i];
				if (!slots_[i].polyThisBlock) {
					a.at = shift(a.at);
					a.post = shift(a.post);
				}
				latest = std::max({ latest, a.at, a.post });
			}
		}
		voiceShare_ = perVoiceLatency_;
		moved = true;
	}
	// **NOTHING LATE, NOTHING KEPT** (backlog LW): the history starts when the first device turns latent and is dropped
	// when the last one stops, so an engine with no latent device does none of the delay work at all.
	if (latest == 0 && maxArrival_ != 0) {
		historyHead_ = 0;
		historyCount_ = 0;
	}
	maxArrival_ = latest;
	if (!voiceLate && lateInVoice_) {
		std::fill(voiceCount_.begin(), voiceCount_.end(), 0u); // nothing late in a voice: its history goes
	}
	lateInVoice_ = voiceLate;
	// A DEVICE THAT MOVED DOMAIN READS ITS SOURCES THROUGH ANOTHER RING (step 4), and so does one whose container moved in
	// its host: the inputs are re-taken at the next drain.
	const uint32_t outerNow = outer_.arrival != nullptr ? outer_.arrival(outer_.host, outer_.self) : 0u;
	if (moved || outerNow != outerArrival_) {
		outerArrival_ = outerNow;
		routesDirty_ = true;
	}
}

void RackEngine::recordNoteMoment() noexcept {
	if (maxArrival_ == 0 || noteHistory_.empty()) {
		return;
	}
	const auto same = [](const device::NoteContext& a, const device::NoteContext& b) {
		return a.gate == b.gate && a.pitchSemitones == b.pitchSemitones && a.stackCount == b.stackCount
		       && a.stackDetuneCents == b.stackDetuneCents && a.stackCoherence == b.stackCoherence;
	};
	// THE CLOCK RUNS ON BY ITSELF: it is only a moment where it did something else — a tempo, a locate, play or stop —
	// read against the newest moment moved on to now, or held where it was (a host that holds a stopped position).
	const device::TransportClock& clock = blockContext_.clock;
	const auto near = [](double a, double b) {
		return (std::isnan(a) && std::isnan(b)) || (std::isfinite(a) && std::isfinite(b) && std::abs(a - b) < 1e-6);
	};
	const auto flowing = [&](const NoteMoment& m) {
		if (m.clock == clock) {
			return true;
		}
		const device::TransportClock on = advanced(m.clock, static_cast<double>(busClock_ - m.at) / std::max(sampleRate_, 1.0));
		return on.secondsPerBeat == clock.secondsPerBeat && on.playing == clock.playing
		       && near(on.songPositionSeconds, clock.songPositionSeconds) && near(on.songPositionBeats, clock.songPositionBeats);
	};
	if (historyCount_ > 0) {
		NoteMoment& newest = noteHistory_[(historyHead_ + historyCount_ - 1) % noteHistory_.size()];
		if (same(newest.note, noteContext_) && flowing(newest)) {
			return;
		}
		if (newest.at == busClock_) {
			newest.note = noteContext_; // moved again before any sample ran under it
			newest.clock = clock;
			return;
		}
	}
	if (historyCount_ == noteHistory_.size()) {
		makeRoomInHistory();
	}
	noteHistory_[(historyHead_ + historyCount_) % noteHistory_.size()] = NoteMoment { busClock_, noteContext_, clock };
	++historyCount_;
	++lateWork_;
}

void RackEngine::removeMoment(size_t k) noexcept {
	const size_t n = noteHistory_.size();
	for (size_t j = k; j + 1 < historyCount_; ++j) {
		noteHistory_[(historyHead_ + j) % n] = noteHistory_[(historyHead_ + j + 1) % n];
	}
	--historyCount_;
}

void RackEngine::makeRoomInHistory() noexcept {
	// **A FULL HISTORY, DEFINED** (backlog LW): more than `kNoteHistory` moments inside the latest
	// device's window — a moment per piece, so pieces of a few samples behind a 9216-sample EQ. Nothing goes missing
	// silently and no OFF is ever dropped alone, so nothing can be left sounding:
	//  1. A moment no late device can read any more goes first: one older than the next, both before `now − maxArrival`.
	//  2. Then a change made while the gate stays UP (a pitch step, a retrigger under a held note) merges into the one
	//     before it: the late device holds the earlier pitch until the next moment. Counted (`lateMergesForTest`).
	//  3. Then a whole NOTE — its on and the off after it, together — the oldest: the late device does not play it, and
	//     never holds it. Counted (`lateNotesDroppedForTest`).
	//  4. Never reached with on/off moments, but defined: the oldest goes.
	const size_t n = noteHistory_.size();
	const auto at = [&](size_t k) -> const NoteMoment& { return noteHistory_[(historyHead_ + k) % n]; };
	const uint64_t horizon = busClock_ >= maxArrival_ ? busClock_ - maxArrival_ : 0;
	if (historyCount_ >= 2 && at(1).at <= horizon) {
		historyHead_ = (historyHead_ + 1) % n;
		--historyCount_;
		return;
	}
	for (size_t k = 1; k < historyCount_; ++k) {
		if (at(k).note.gate && at(k - 1).note.gate) {
			removeMoment(k);
			++lateMerges_;
			return;
		}
	}
	for (size_t k = 1; k + 1 < historyCount_; ++k) {
		if (at(k).note.gate && !at(k - 1).note.gate && !at(k + 1).note.gate) {
			removeMoment(k + 1);
			removeMoment(k);
			++lateNotesDropped_;
			return;
		}
	}
	historyHead_ = (historyHead_ + 1) % n;
	--historyCount_;
}

const RackEngine::NoteMoment* RackEngine::momentAt(uint64_t at) const noexcept {
	if (historyCount_ == 0) {
		return nullptr;
	}
	// NEWEST FIRST: a late device reads a moment near the end of the history, and the history is in time order.
	for (size_t k = historyCount_; k-- > 0;) {
		const NoteMoment& m = noteHistory_[(historyHead_ + k) % noteHistory_.size()];
		if (m.at <= at) {
			return &m;
		}
	}
	return &noteHistory_[historyHead_];
}

const device::NoteContext& RackEngine::noteAt(uint64_t at) const noexcept {
	const NoteMoment* m = momentAt(at);
	return m != nullptr ? m->note : noteContext_;
}

device::TransportClock RackEngine::clockAt(uint64_t at) const noexcept {
	const NoteMoment* m = momentAt(at);
	if (m == nullptr) {
		return blockContext_.clock;
	}
	// MOVED ON FROM ITS MOMENT WHILE IT PLAYED; a stopped clock stands where it stopped. Before the oldest moment (a
	// device that has just turned late), the oldest is read as it was.
	if (!m->clock.playing || at <= m->at) {
		return m->clock;
	}
	return advanced(m->clock, static_cast<double>(at - m->at) / std::max(sampleRate_, 1.0));
}

const device::TransportClock& RackEngine::clockFor(size_t lane) const noexcept {
	if (maxArrival_ > 0 && lane < arrival_->capacity() && lane < lateClock_->capacity() && (*arrival_)[lane].at > 0) {
		return (*lateClock_)[lane].clock;
	}
	return blockContext_.clock;
}

uint32_t RackEngine::samplesUntilLateNote() noexcept {
	if (maxArrival_ == 0) {
		return UINT32_MAX;
	}
	recordNoteMoment();
	// THE SOONEST MOMENT A LATE SLOT HAS NOT REACHED: for each arrival `A`, the first moment after `now - A`, due at
	// `moment + A`. Consecutive slots after one latent device share their `A`, so each distinct value is asked once.
	uint64_t soonest = UINT64_MAX;
	uint32_t asked = 0;
	const auto ask = [&](uint32_t a) {
		if (a == 0 || a == asked) {
			return;
		}
		asked = a;
		for (size_t k = 0; k < historyCount_; ++k) {
			const NoteMoment& m = noteHistory_[(historyHead_ + k) % noteHistory_.size()];
			const uint64_t due = m.at + a;
			if (due > busClock_) {
				soonest = std::min(soonest, due);
				break;
			}
		}
	};
	const OrderSnapshot& order = orderSnapshot_[orderFront_.load(std::memory_order_acquire)];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i < arrival_->capacity() && i < slots_.size() && slots_[i].isAudio()) {
				ask((*arrival_)[i].at);
				ask((*arrival_)[i].post);
			}
		}
	}
	// AND EVERY VOICE'S NOTE A DEVICE BEHIND A LATENT PER-VOICE ONE HAS NOT REACHED (the voice road).
	if (lateInVoice_) {
		for (const ChainOrder& path : order.chain) {
			for (size_t step = 0; step < path.count; ++step) {
				const size_t i = path.at(step);
				if (i >= slots_.size() || i >= arrival_->capacity() || !slots_[i].polyThisBlock) {
					continue;
				}
				const uint32_t a = (*arrival_)[i].at;
				for (size_t v = 0; a > 0 && v < voiceHead_.size(); ++v) {
					const NoteMoment* ring = &voiceHistory_[v * kVoiceMoments];
					for (size_t k = 0; k < voiceCount_[v]; ++k) {
						const uint64_t due = ring[(voiceHead_[v] + k) % kVoiceMoments].at + a;
						if (due > busClock_) {
							soonest = std::min(soonest, due);
							break;
						}
					}
				}
			}
		}
	}
	// AND EVERY WRITE WAITING ITS TURN (step 3), so a step of automation lands on its own sample too.
	for (size_t k = 0; k < pendingCount_; ++k) {
		if (pendingFields_[k].due > busClock_) {
			soonest = std::min(soonest, pendingFields_[k].due);
		}
	}
	if (soonest == UINT64_MAX) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>(std::min<uint64_t>(soonest - busClock_, UINT32_MAX));
}

void RackEngine::processVoice(size_t voice, float* left, float* right, int frames,
                              const device::NoteContext& note) {
	if (left == nullptr || frames <= 0) {
		return;
	}
	// THE VOICE ROAD (backlog LW step 4): the voice's note into its history while a device behind a latent per-voice
	// one runs in it, so that device is handed the note its audio belongs to.
	if (lateInVoice_) {
		recordVoiceMoment(voice, note);
	}
	// IN CHAIN ORDER, through the order `beginBlock` acquired — among the per-voice devices, which is
	// the order the lane has always run them in.
	const OrderSnapshot& order = orderSnapshot_[blockFront_];
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.count; ++step) {
			const size_t i = path.at(step);
			if (i >= slots_.size()) {
				continue;
			}
			Slot& slot = slots_[i];
			if (!slot.polyThisBlock) {
				continue;
			}
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr || d->audio == nullptr) {
				continue;
			}
			const CostScope cost(i, d->traits.tag, costLane_, costLaneIndex_); // this voice's share of the device (backlog SR)
			const device::AudioOps& ops = *d->audio;
			const uint32_t lateBy = lateInVoice_ && i < arrival_->capacity() ? (*arrival_)[i].at : 0u;
			const device::NoteContext& handed =
			    lateBy > 0 ? voiceNoteAt(voice, busClock_ >= lateBy ? busClock_ - lateBy : 0, note) : note;
			// A TWO-STAGE DEVICE takes the voice's channels on its one instance (rack roadmap §8) — BOTH of
			// them when the voice is stereo, as a voice instance below is handed them (backlog KT).
			if (slot.voices.empty()) {
				if (ops.processVoice != nullptr && slot.storage != nullptr) {
					ops.processVoice(slot.storage.get(), voice, left, right, frames, handed);
				}
				continue;
			}
			if (voice >= slot.voices.size()) {
				continue;
			}
			void* instance = slot.voices[voice].get();
			// DELIVERED NOW, per chunk, as `FxRunner::setNote` did: a voice's note moves once per chunk,
			// and a note latched at the top of the block would leave the first chunk of a new note
			// tracking the note before it.
			if (ops.setNoteContext != nullptr) {
				ops.setNoteContext(instance, handed);
			}
			// THE VOICE'S MOVING CONTROLS, PER SAMPLE (backlog FK): its own windows when a per-voice input reaches the row,
			// the collapsed row's otherwise — the same choice `beginBlock` made for its scalar row.
			if (!slot.controlWindow.empty()) {
				const size_t width = slot.base.size();
				const auto stride = static_cast<size_t>(device::kTickWindowStride);
				const bool own = slot.anyPerVoiceInput && (voice + 1) * width * stride <= slot.controlWindowVoice.size();
				handWindows(ops, instance, own ? &slot.controlWindowVoice[voice * width * stride] : slot.controlWindow.data(), width, frames);
			}
			// **THE FURNITURE, PER VOICE** (backlog NR): the voice's own Mix and wet Gain, its own ramp, against what entered
			// the voice's device. No delay: a latent device runs per voice only by opting in (`latentPerVoice`), and none of
			// those lets the slot blend.
			float wet = 1.0f;
			float dry = 0.0f;
			FurnitureRamp* ramp = voice < slot.furnitureVoice.size() && furnitureTargets(slot, *d, voice, &wet, &dry)
			                          ? &slot.furnitureVoice[voice]
			                          : nullptr;
			// THE ENABLE, PER VOICE INSTANCE (backlog KW), as on the bus: reset coming back from parked, parked once faded.
			const bool gates = ramp != nullptr && engineEnable(*d);
			const bool on = !gates || enabledIn(slot.resolved, *d);
			if (gates && on && ramp->parked) {
				if (ops.reset != nullptr) {
					ops.reset(instance);
				}
				ramp->parked = false;
			}
			if (ramp != nullptr) {
				ramp->aim(wet, dry, furnitureRampSamples_);
			}
			if (gates && !on && ramp->remaining == 0) {
				ramp->parked = true;
			}
			if (ramp != nullptr && ramp->parked) {
				// The voice's output is what entered it, and that is still what the device's meter reads (backlog MK).
				slot.outputChunkPeak = std::max(slot.outputChunkPeak, peakOf(left, right, frames));
				continue;
			}
			const bool runs = right != nullptr || ops.processMono != nullptr;
			const bool blend = runs && ramp != nullptr && !ramp->identity() && static_cast<size_t>(frames) <= furnitureDryL_.size();
			if (blend) {
				std::copy_n(left, static_cast<size_t>(frames), furnitureDryL_.begin());
				if (right != nullptr) {
					std::copy_n(right, static_cast<size_t>(frames), furnitureDryR_.begin());
				}
			}
			if (right != nullptr) {
				ops.processStereo(instance, left, right, frames);
			}
			else if (ops.processMono != nullptr) {
				// A DEVICE WITH NO MONO ROAD IS SKIPPED rather than handed a null second channel.
				ops.processMono(instance, left, frames);
			}
			if (blend) {
				for (int k = 0; k < frames; ++k) {
					const auto i = static_cast<size_t>(k);
					ramp->step();
					left[k] = left[k] * ramp->wet + furnitureDryL_[i] * ramp->dry;
					if (right != nullptr) {
						right[k] = right[k] * ramp->wet + furnitureDryR_[i] * ramp->dry;
					}
				}
			}
			// THE COURTESY METER (backlog MK): the loudest of the voices, folded at the end of `processBus`.
			slot.outputChunkPeak = std::max(slot.outputChunkPeak, peakOf(left, right, frames));
		}
	}
}

float RackEngine::peakOf(const float* left, const float* right, int frames) noexcept {
	// `a > peak` RATHER THAN `std::max`, so a NaN sample is skipped instead of carried: a meter reading NaN draws nowhere.
	float peak = 0.0f;
	for (int i = 0; i < frames; ++i) {
		const float a = std::fabs(left[i]);
		if (a > peak) {
			peak = a;
		}
	}
	if (right != nullptr) {
		for (int i = 0; i < frames; ++i) {
			const float a = std::fabs(right[i]);
			if (a > peak) {
				peak = a;
			}
		}
	}
	return peak;
}

void RackEngine::advanceOutputMeters(int frames) noexcept {
	// **ONE STEP OF EVERY AUDIO SLOT'S BALLISTICS PER CHUNK**, the bus pass and the voices' gathered into
	// `outputChunkPeak`: a peak at or above the held one restarts the hold; past the hold it falls. A slot that did
	// not run this chunk gathered zero and falls with the rest, so a meter never sticks on a device that went quiet.
	const double seconds = static_cast<double>(frames) / std::max(sampleRate_, 1.0);
	const auto holdSamples = static_cast<int64_t>(kOutputMeterHoldSeconds * sampleRate_);
	const auto fall = static_cast<float>(std::pow(10.0, -kOutputMeterFallDbPerSecond * seconds / 20.0));
	for (Slot& slot : slots_) {
		if (!slot.isAudio()) {
			continue;
		}
		const float chunk = slot.outputChunkPeak;
		slot.outputChunkPeak = 0.0f;
		if (chunk >= slot.outputHeld) {
			slot.outputHeld = chunk;
			slot.outputHoldLeft = holdSamples;
		}
		else if (slot.outputHoldLeft > 0) {
			slot.outputHoldLeft -= frames;
		}
		else {
			slot.outputHeld *= fall;
			if (slot.outputHeld < kOutputMeterSilence) {
				slot.outputHeld = 0.0f;
			}
		}
		slot.outputPeak.store(slot.outputHeld, std::memory_order_relaxed);
	}
}

bool RackEngine::outputPeakAt(size_t slot, float& peak) const {
	if (slot >= slots_.size() || !slots_[slot].isAudio()) {
		return false;
	}
	const float held = slots_[slot].outputPeak.load(std::memory_order_relaxed);
	if (held < 0.0f) {
		return false; // not run yet: no meter, not a silent one
	}
	peak = held;
	return true;
}

void RackEngine::handWindows(const device::AudioOps& ops, void* instance, const float* windows, size_t width,
                             int frames) const {
	// **THE DECLARED CONTROLS THAT MOVE INSIDE THIS CALL**, as runs over exactly its samples: the tick phase says where the
	// call starts inside the tick (`setTickPhase`), and a host off the tick grid set none. `windows` is a row of `width`
	// windows — the collapsed row's, or one voice's.
	if (ops.setFieldWindows == nullptr || windows == nullptr || tickPhase_ < 0 || tickPhase_ + frames > device::kTickWindow) {
		return;
	}
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	const size_t count = std::min(width, static_cast<size_t>(device::kMaxDeviceFields));
	std::array<const float*, device::kMaxDeviceFields> runs {};
	bool any = false;
	for (const uint8_t declared : ops.windowedControls) {
		const auto c = static_cast<size_t>(declared);
		if (c >= count) {
			continue;
		}
		const float* w = windows + c * stride;
		if (!device::tickWindowFlat(w)) {
			runs[c] = w + tickPhase_;
			any = true;
		}
	}
	if (any) {
		ops.setFieldWindows(instance, runs.data(), count, frames);
	}
}

void RackEngine::fillBusNotes(const OrderSnapshot& order) noexcept {
	// **THE COLLAPSED NOTE, AND A TRACKER'S PITCH ON IT FOR EVERY DEVICE AFTER THE TRACKER** — see `busNoteForTest`.
	// In PATH order, chain after chain, so a Tracker carries into the chains after its own (Feathers' strip into its
	// fx lane). A device is handed what the walk held BEFORE it, so a Tracker never governs itself and a device ahead
	// of every Tracker keeps the default. A switched-off Tracker is not asked: switched off is absent.
	device::NoteContext running = noteContext_;
	// **A LATE DEVICE IS HANDED THE NOTE AS IT WAS `A(n)` AGO** (backlog LW, step 2): the note its audio belongs to,
	// so HARM behind a linear-phase EQ starts its harmonic when the EQ's output of that note does, not 0.19 s before.
	// Nothing at all while nothing is latent. A Tracker's pitch is still its own, now (to be confirmed: it is a
	// modulator's answer, and modulation follows in step 4).
	const bool late = maxArrival_ > 0;
	if (late) {
		recordNoteMoment();
	}
	bool tracked = false;
	for (const ChainOrder& path : order.chain) {
		for (size_t step = 0; step < path.pathCount; ++step) {
			const size_t i = (*path.path)[step];
			if (i >= slots_.size() || i >= busNote_->capacity()) {
				continue;
			}
			(*busNote_)[i] = running;
			if (late && i < arrival_->capacity() && (*arrival_)[i].at > 0) {
				const Arrival& a = (*arrival_)[i];
				const uint64_t when = busClock_ >= a.at ? busClock_ - a.at : 0;
				device::NoteContext then = noteAt(when);
				if (tracked) {
					then.pitchSemitones = running.pitchSemitones;
				}
				(*busNote_)[i] = then;
				// AND ITS CLOCK (step 3): the song as it stood when its audio was played.
				if (i < lateClock_->capacity()) {
					LateClock& lc = (*lateClock_)[i];
					lc.clock = clockAt(when);
					lc.postClock = a.post != a.at ? clockAt(busClock_ >= a.post ? busClock_ - a.post : 0) : lc.clock;
				}
				++lateWork_;
			}
			const Slot& slot = slots_[i];
			const device::Descriptor* d = slot.live_desc();
			if (d == nullptr || slot.storage == nullptr || d->notes == nullptr || d->notes->busNote == nullptr) {
				continue;
			}
			const int enable = d->traits.enableControl;
			if (enable >= 0 && static_cast<size_t>(enable) < slot.resolved.size()
			    && slot.resolved[static_cast<size_t>(enable)] < 0.5f) {
				continue;
			}
			float pitch = running.pitchSemitones;
			if (d->notes->busNote(slot.storage.get(), &pitch) && std::isfinite(pitch)) {
				running.pitchSemitones = pitch;
				tracked = true;
			}
		}
	}
}

void RackEngine::offerSite(SiteOffer* offer) noexcept {
	if (offer == nullptr) {
		return;
	}
	offer->next = siteInbox_.load(std::memory_order_relaxed);
	while (!siteInbox_.compare_exchange_weak(offer->next, offer, std::memory_order_release, std::memory_order_relaxed)) {
	}
}

RackEngine::SiteOffer* RackEngine::takeSpentSiteOffers() noexcept {
	return siteSpent_.exchange(nullptr, std::memory_order_acquire);
}

void RackEngine::adoptSites() noexcept {
	SiteOffer* taken = siteInbox_.exchange(nullptr, std::memory_order_acquire);
	// THE STACK REVERSED INTO POSTING ORDER, so of two offers for one device the later one is installed last.
	SiteOffer* ordered = nullptr;
	while (taken != nullptr) {
		SiteOffer* next = taken->next;
		taken->next = ordered;
		ordered = taken;
		taken = next;
	}
	while (ordered != nullptr) {
		SiteOffer* offer = ordered;
		ordered = offer->next;
		// INTO THE SLOT THAT HOLDS THE DEVICE, AND ONLY ONE THAT OFFERS A SITE — or nowhere: the converger offers again
		// once the device has landed (`Converger::serviceSites`).
		const size_t slot = slotOf(offer->id);
		if (slot != kNoSlot && slot < slots_.size()) {
			Slot& s = slots_[slot];
			const device::Descriptor* d = s.live_desc();
			const bool offers = d != nullptr && siteLanesOf(*d) != device::Traits::SiteLanes::None;
			if (offer->site == nullptr || (offers && offer->site->owner() == s.id)) {
				if (s.site.load(std::memory_order_relaxed) != offer->site) {
					routesDirty_ = true; // a route from this site's exports points into it, or did (backlog NZ)
				}
				s.site.store(offer->site, std::memory_order_release);
			}
		}
		offer->next = siteSpent_.load(std::memory_order_relaxed);
		while (!siteSpent_.compare_exchange_weak(offer->next, offer, std::memory_order_release,
		                                         std::memory_order_relaxed)) {
		}
	}
}

SiteChains* RackEngine::siteOf(DeviceInstanceId id) const noexcept {
	const size_t slot = slotOf(id);
	return slot != kNoSlot && slot < slots_.size() ? slots_[slot].site.load(std::memory_order_acquire) : nullptr;
}

bool RackEngine::holdsSite(const SiteChains* site) const noexcept {
	if (site == nullptr) {
		return false;
	}
	for (size_t i = 0; i < slots_.size(); ++i) {
		const Slot& slot = slots_[i];
		if (slot.site.load(std::memory_order_acquire) == site) {
			return true;
		}
	}
	return false;
}

void RackEngine::dropSites() noexcept {
	for (size_t i = 0; i < slots_.size(); ++i) {
		slots_[i].site.store(nullptr, std::memory_order_seq_cst);
	}
	routesDirty_ = true; // no input may go on pointing into a site let go (backlog NZ)
}

void RackEngine::processBus(float* left, float* right, int frames) {
	if (left != nullptr && right != nullptr && frames > 0) {
		const OrderSnapshot& order = orderSnapshot_[blockFront_];
		fillBusNotes(order); // the collapsed note, and each Tracker's pitch for the devices after it
		for (const ChainOrder& path : order.chain) {
			for (size_t step = 0; step < path.count; ++step) {
				const size_t i = path.at(step);
				if (i >= slots_.size()) {
					continue;
				}
				Slot& slot = slots_[i];
				// ONE LOAD, HELD — see `beginBlock`.
				const device::Descriptor* d = slot.live_desc();
				if (d == nullptr || slot.storage == nullptr || d->audio == nullptr) {
					continue;
				}
				const CostScope cost(i, d->traits.tag, costLane_, costLaneIndex_); // the spike recorder's, when one is set (backlog SR)
				runBusSlot(slot, i, *d, left, right, frames);
			}
		}
	}
	if (frames > 0) {
		advanceOutputMeters(frames);
		busClock_ += static_cast<uint64_t>(frames); // the bus clock the note history is kept on (backlog LW)
	}
}

const float* RackEngine::fieldsFor(const device::Descriptor& d, const std::vector<float>& row) noexcept {
	// ON ALREADY, OR NOT THE ENGINE'S TO SWITCH: the row itself, so a device that is on is handed exactly what it was.
	if (!engineEnable(d) || enabledIn(row, d) || row.size() > enableOnRow_.size()) {
		return row.data();
	}
	std::copy(row.begin(), row.end(), enableOnRow_.begin());
	enableOnRow_[static_cast<size_t>(d.traits.enableControl)] = 1.0f;
	return enableOnRow_.data();
}

bool RackEngine::furnitureTargets(const Slot& slot, const device::Descriptor& d, size_t voice, float* wet,
                                  float* dry) const {
	// THE TWO ROLES' VALUES AS RESOLVED (backlog NR): the collapsed row for the bus, the voice's own for a voice — so a
	// per-voice route to the Mix moves each voice's blend. Out of range or not finite reads as the identity's end.
	const bool mixes = slotBlends(d);
	const bool gates = engineEnable(d);
	if (!mixes && !gates) {
		return false;
	}
	const auto value = [&](int control, float fallback) {
		const auto c = static_cast<size_t>(control);
		if (control < 0 || c >= slot.resolved.size()) {
			return fallback;
		}
		const float v = voice == kNoVoice ? slot.resolved[c] : resolveControl(slot, d, c, voice);
		return std::isfinite(v) ? v : fallback;
	};
	const float mix = mixes ? std::clamp(value(d.traits.mixControl, 1.0f), 0.0f, 1.0f) : 1.0f;
	const float gain = mixes && d.traits.wetGainControl >= 0 ? siteFaderGain(value(d.traits.wetGainControl, 0.5f)) : 1.0f;
	// THE ENABLE (backlog KW): switched off, the slot crossfades to the dry, as a Mix at zero would. On, exactly 1, so a
	// device that is on blends exactly as it did. One switch per block for every voice (the collapsed row).
	const float on = gates && !enabledIn(slot.resolved, d) ? 0.0f : 1.0f;
	*wet = mix * gain * on;
	*dry = 1.0f - mix * on;
	return true;
}

void RackEngine::runDevice(Slot& slot, size_t lane, const device::Descriptor& d, float* left, float* right, int frames) {
	const backplane::BlockContext& block = blockContext_;
	const device::AudioOps& ops = *d.audio;
	ops.setFields(slot.storage.get(), fieldsFor(d, slot.resolved), slot.resolved.size());
	++slot.runs;
	if (ops.pullHandBlocks != nullptr) {
		ops.pullHandBlocks(slot.storage.get(), slot.storage.get());
	}
	if (slot.stereoLive.any() && ops.setFieldsR != nullptr) {
		ops.setFieldsR(slot.storage.get(), slot.resolvedR.data(), slot.resolvedR.size(), slot.stereoLive);
	}
	if (ops.setTransportClock != nullptr) {
		ops.setTransportClock(slot.storage.get(), maxArrival_ > 0 ? clockFor(lane) : block.clock);
	}
	// **THE NOTE, WHICH THIS ENGINE NEVER DELIVERED.** `FxRunner` has always called this and
	// the rack never did, so a pitched device in a chain had no pitch and — worse — no GATE:
	// HARM sets `settings.gate` from here and keys its envelope off it, so an enabled HARM
	// with its level up produced silence.
	// Not user error.
	if (ops.setNoteContext != nullptr) {
		ops.setNoteContext(slot.storage.get(), (*busNote_)[lane]);
	}
	// **THE DECLARED CONTROLS THAT MOVE INSIDE THIS CALL**, as runs over exactly its samples: the tick phase
	// says where this call starts inside the tick (`setTickPhase`), and a host off the tick grid set none.
	if (!slot.controlWindow.empty()) {
		handWindows(ops, slot.storage.get(), slot.controlWindow.data(), slot.base.size(), frames);
	}
	if (ops.setSidechain != nullptr) {
		ops.setSidechain(slot.storage.get(), sideL_, sideR_, frames, sideSounding_);
	}
	ops.processStereo(slot.storage.get(), left, right, frames);
	// THE COURTESY METER (backlog MK): what this device handed the next one, before anything after it.
	slot.outputChunkPeak = std::max(slot.outputChunkPeak, peakOf(left, right, frames));
}

void RackEngine::runBusSlot(Slot& slot, size_t lane, const device::Descriptor& d, float* left, float* right,
                            int frames) {
	// **THE SLOT AS A CHAIN ACCESS SITE** (backlog NR): the signal that entered → PRE → the device → POST → × wet Gain,
	// crossfaded by the Mix against what entered, delayed by everything the slot's path takes. Each part costs nothing
	// while it is absent: no site is one load, and a Mix at 100% with the wet Gain at unity — every device's default —
	// is the identity, which the slot does not blend at all. Bit for bit the device's own call then.
	SiteChains* site = slot.site.load(std::memory_order_acquire);
	if (site != nullptr && site->owner() != slot.id) {
		site = nullptr; // never another device's (the drain checks this too; a contemporaneous read is cheap)
	}
	const device::TransportClock& clock = maxArrival_ > 0 ? clockFor(lane) : blockContext_.clock;
	const device::NoteContext& note = (*busNote_)[lane];
	// ITS SITE'S POST LANE HEARS THE DEVICE'S OUTPUT, later again by PRE and the device (backlog LW) — its note and its
	// clock both.
	device::NoteContext postNote = note;
	device::TransportClock postClock = clock;
	if (site != nullptr && maxArrival_ > 0 && lane < arrival_->capacity()) {
		const Arrival& a = (*arrival_)[lane];
		if (a.post != a.at) {
			const uint64_t when = busClock_ >= a.post ? busClock_ - a.post : 0;
			postNote = noteAt(when);
			postClock = clockAt(when);
			++lateWork_;
		}
	}
	if (slot.polyThisBlock && !slot.voices.empty()) {
		// IT RAN INSIDE THE VOICES, with its furniture there (`processVoice`). Its POST runs here, ON THE VOICES' SUM, at
		// the device's place in the chain (D4); no PRE can feed instances that have already run.
		if (site != nullptr) {
			site->process(site->post(), left, right, frames, postClock, postNote);
		}
		return;
	}
	float wet = 1.0f;
	float dry = 0.0f;
	const bool blends = furnitureTargets(slot, d, kNoVoice, &wet, &dry);
	// THE ENABLE (backlog KW): back on from parked, the device is reset first — its state is from before it was
	// switched off — then fades in from the dry. Allocation-free (`AudioOps::reset`).
	const bool gates = engineEnable(d);
	const bool on = !gates || enabledIn(slot.resolved, d);
	if (gates && on && slot.furniture.parked) {
		if (d.audio->reset != nullptr) {
			d.audio->reset(slot.storage.get());
		}
		slot.furniture.parked = false;
	}
	if (blends) {
		slot.furniture.aim(wet, dry, furnitureRampSamples_);
	}
	// OFF AND THE FADE DONE: parked from this block on. A device loaded switched off is parked at once (the ramp is
	// placed, not glided, before the first block).
	if (gates && !on && slot.furniture.remaining == 0) {
		slot.furniture.parked = true;
	}
	// THE DRY IS NEEDED WHILE THE BLEND IS NOT THE IDENTITY, AND FED WHILE THE PATH IS LATE so it is continuous when the
	// Mix comes down (a delay coming up from zero starts from a cleared ring). A call longer than the scratch cannot be
	// held, and passes the wet: the scratch is sized to the host's longest block, so only a host breaking its own
	// promise meets that.
	const uint32_t latency = blends ? latencyOf(slot, d, sampleRate_) + (site != nullptr ? site->latencySamples(sampleRate_) : 0u)
	                                : 0u;
	const bool fits = static_cast<size_t>(frames) <= furnitureDryL_.size();
	if (slot.furniture.parked) {
		// PARKED: the slot's output is the dry, delayed by what its path takes so the chain's alignment holds; the
		// device and its site do not run, so a tail is cut and nothing it held can keep the plugin awake.
		if (latency > 0 && fits) {
			const auto n = static_cast<size_t>(frames);
			std::copy_n(left, n, furnitureDryL_.begin());
			std::copy_n(right, n, furnitureDryR_.begin());
			device::CompensationDelay& delay = site != nullptr ? site->dryDelay() : slot.furnitureDelay;
			delay.process(furnitureDryL_.data(), furnitureDryR_.data(), frames, latency);
			std::copy_n(furnitureDryL_.begin(), n, left);
			std::copy_n(furnitureDryR_.begin(), n, right);
		}
		// STILL METERED (backlog MK): switched off is still in the chain, and what it hands on is the dry.
		slot.outputChunkPeak = std::max(slot.outputChunkPeak, peakOf(left, right, frames));
		return;
	}
	const bool blend = blends && fits && !slot.furniture.identity();
	const bool feed = blends && fits && (blend || latency > 0);
	if (!feed && site == nullptr) {
		runDevice(slot, lane, d, left, right, frames);
		return;
	}
	const auto n = static_cast<size_t>(frames);
	if (feed) {
		std::copy_n(left, n, furnitureDryL_.begin());
		std::copy_n(right, n, furnitureDryR_.begin());
	}
	if (site != nullptr && siteLanesOf(d) == device::Traits::SiteLanes::PreAndPost) {
		site->process(site->pre(), left, right, frames, clock, note);
	}
	runDevice(slot, lane, d, left, right, frames);
	if (site != nullptr) {
		site->process(site->post(), left, right, frames, postClock, postNote);
	}
	if (!feed) {
		return;
	}
	device::CompensationDelay& delay = site != nullptr ? site->dryDelay() : slot.furnitureDelay;
	delay.process(furnitureDryL_.data(), furnitureDryR_.data(), frames, latency);
	if (!blend) {
		return; // fed and not heard: at the identity the slot's output is the wet, untouched
	}
	FurnitureRamp& ramp = slot.furniture;
	for (size_t i = 0; i < n; ++i) {
		ramp.step();
		left[i] = left[i] * ramp.wet + furnitureDryL_[i] * ramp.dry;
		right[i] = right[i] * ramp.wet + furnitureDryR_[i] * ramp.dry;
	}
}

void RackEngine::endBlock() noexcept {
	// OUT, because `beginBlock` went in. Anything the editor parked while the block ran becomes
	// reapable now.
	blockEpoch_.fetch_add(1, std::memory_order_seq_cst);
}


bool RackEngine::drivePeakAt(size_t slot, float& peak) const {
	// **THE READ HALF OF THE DRIVE METER, ON THE RACK.** It existed only on Feathers'
	// `BridgeDeviceHost`, so a shaper card in the RACK asked a host that takes `DeviceHost`'s
	// declining default and drew no meter, in the rack. The seam
	// behaving exactly as designed on a host that implements nothing, which is what makes a silent
	// decline hard to tell from a broken publisher.
	//
	// **THE SAME CONTRACT AS `FxRunner::drivePeakAt`, DELIBERATELY**: decline when the device offers
	// no hook, leave the out-parameter untouched when declining (`playheadAt` above depends on that
	// and so does this), and never answer a plausible zero — a slot with no meter must be
	// distinguishable from one being driven by silence.
	//
	// **AND NO ATOMIC CHANNEL HERE, unlike Feathers, which is not an inconsistency.** Feathers needed
	// one because `BridgeDeviceHost` can reach nothing but the bridge, so the value had to be gathered
	// on the audio thread and published. The rack engine IS the engine and holds the storage, so this
	// is a direct read — the same trade OWLTT's meters take, and the same one `device_host.h` records
	// for playheads: a torn float is one frame of a meter.
	if (slot >= slots_.size()) {
		return false;
	}
	const Slot& s = slots_[slot];
	const device::Descriptor* d = s.desc;
	if (d == nullptr || d->audio == nullptr || d->audio->drivePeak == nullptr || s.storage == nullptr) {
		return false;
	}
	peak = d->audio->drivePeak(s.storage.get());
	return true;
}
void RackEngine::setVoiceNote(size_t voice, int32_t noteId, int16_t channel, int16_t key) noexcept {
	if (voice < voiceNotes_.size()) {
		// THE HOST SAYS THIS EVERY TICK; a serial is stamped only when the voice takes a note it was not holding.
		const VoiceNote& was = voiceNotes_[voice];
		const bool fresh = !was.live || was.noteId != noteId || was.channel != channel || was.key != key;
		voiceNotes_[voice] = VoiceNote { noteId, channel, key, true, fresh ? ++voiceNoteSerial_ : was.serial };
	}
}

size_t RackEngine::voiceHolding(int32_t noteId, int16_t channel, int16_t key) const noexcept {
	for (size_t v = 0; v < voiceNotes_.size() && v < voices_; ++v) {
		const VoiceNote& n = voiceNotes_[v];
		if (n.live && n.channel == channel && n.key == key && (noteId == -1 || n.noteId == -1 || n.noteId == noteId)) {
			return v;
		}
	}
	return voices_;
}

size_t RackEngine::newestVoice() const noexcept {
	size_t best = voices_;
	uint64_t serial = 0;
	for (size_t v = 0; v < voiceNotes_.size() && v < voices_; ++v) {
		if (voiceNotes_[v].serial > serial) {
			serial = voiceNotes_[v].serial;
			best = v;
		}
	}
	return best;
}

bool RackEngine::publishesFromVoices(const Slot& slot, const device::Descriptor& d) noexcept {
	return d.audio != nullptr && d.mod != nullptr && !slot.voices.empty() && polyInRow(d, slot.resolved);
}

void RackEngine::clearVoiceNote(size_t voice) noexcept {
	if (voice < voiceNotes_.size()) {
		voiceNotes_[voice].live = false;
	}
}

bool RackEngine::resolveForNote(const void* context, size_t control, int32_t noteId, int16_t channel, int16_t key,
                                float* out) noexcept {
	const auto& r = *static_cast<const NoteResolve*>(context);
	const Slot& slot = *r.slot;
	const RackEngine& engine = *r.engine;
	if (out == nullptr || control >= slot.inputs.size() || control >= r.desc->params.size()) {
		return false;
	}
	// ONLY A PER-VOICE INPUT MAKES A NOTE'S VALUE DIFFER FROM THE ROW — `resolveVoiceInto`'s test. Without one the row's
	// value stands, and the caller follows the device's own edge.
	const InputRange range = slot.inputs[control];
	bool varies = false;
	for (size_t i = range.first; !varies && i < static_cast<size_t>(range.first) + range.count && i < engine.inputs_.size(); ++i) {
		varies = engine.inputs_[i].perVoice != nullptr;
	}
	if (!varies) {
		return false;
	}
	// THE VOICE HOLDING THIS NOTE, by the whole note — `PerNoteTable::indexOfNote`'s match: -1 on either side is a
	// wildcard id, so a keyboard's notes are found by channel and key.
	for (size_t v = 0; v < engine.voiceNotes_.size() && v < engine.voices_; ++v) {
		const VoiceNote& n = engine.voiceNotes_[v];
		if (!n.live || n.channel != channel || n.key != key) {
			continue;
		}
		if (noteId == -1 || n.noteId == -1 || n.noteId == noteId) {
			*out = engine.resolveControl(slot, *r.desc, control, v);
			return true;
		}
	}
	return false;
}

void RackEngine::setVoices(size_t voices) noexcept {
	voices_ = voices;
	voiceNotes_.assign(voices, VoiceNote {});
	sizeVoiceDelays(); // the voice road's history and rings (backlog LW step 4)
	// **THE PER-VOICE CELLS, FOR EVERY SLOT, ONCE.** A host node's input may point into a slot's cells
	// across an adoption, so they are sized here — message thread, before any device is added, where
	// allocation is allowed — and never again. A slot with no per-voice output simply never has them
	// written. See `Slot::outputsPerVoice`.
	// MESSAGE THREAD (a host's prepare, a container's provision): each slot's per-voice outputs at the new count, for the
	// device it holds — none for an empty slot.
	for (Slot& slot : slots_) {
		const device::Descriptor* d = slot.live_desc();
		sizeVoiceOutputs(slot.outputsPerVoice, slot.outputsPerVoiceWindow, slot.outputsPerVoiceR, slot.outputsPerVoiceWindowR,
		                 voices, voiceOutputCount(d));
	}
}
} // namespace feathers::rack

namespace feathers::rack {

//------------------------------------------------------------------------------------------------//
// GROWTH — unbounded devices step 4                                                                //
//------------------------------------------------------------------------------------------------//

void RackEngine::growPools(size_t slots) {
	// A NEW SLOT HOLDS NO DEVICE, so no outputs: its first install brings its device's own (see `Slot::outputs`).
	slots_.grow(slots, [](Slot&, size_t) {});
	readings_.grow(slots);
	busNote_->grow(slots);
	arrival_->grow(slots);
	lateClock_->grow(slots);
	readingHops_.grow(slots, [this](ReadingsHop& hop, size_t index) { hop = ReadingsHop { this, index }; });
	for (OrderSnapshot& snap : orderSnapshot_) {
		for (ChainOrder& chain : snap.chain) {
			chain.lane->grow(slots);
			chain.path->grow(slots);
		}
	}
	for (const auto& listener : growthListeners_) {
		if (listener) {
			listener(slots_.capacity());
		}
	}
}

bool RackEngine::requestCapacity(size_t slots) {
	(void)reapGrowth();
	const int state = growthState_.load(std::memory_order_acquire);
	const size_t have = state == kGrowthPending ? nextGraph_->capacity() : capacity();
	if (slots <= have) {
		return true;
	}
	if (state != kGrowthIdle) {
		return false; // the last graph has not met its drain yet
	}
	// AT LEAST DOUBLING, like the pools, so a run of single adds costs a handful of graphs.
	const size_t want = std::max(slots, 2 * have);
	growPools(want);
	backplane::Axes axes = axes_;
	axes.deviceSlots = want;
	// THE EDGES CARRY OVER AT THE ROOM THEY HAVE GROWN TO (backlog OJ) — `assignGrown` refuses a graph with fewer.
	nextGraph_ = std::make_unique<backplane::Graph>(axes, graph().chainCount(),
	                                                std::max(edgeCapacityFor(axes), graph().edgeCapacity()), ids_);
	growthState_.store(kGrowthPending, std::memory_order_release);
	return true;
}

void RackEngine::adoptGrowth() noexcept {
	if (growthState_.load(std::memory_order_acquire) != kGrowthPending) {
		return;
	}
	// THE NODES, CHAINS AND EDGES COPIED ACROSS AND THE ORDER RECOMPUTED, INTO ROOM ALREADY ALLOCATED. A graph
	// that cannot take them (it never fails for a bigger graph of the same chains) is dropped rather than
	// half-adopted: the old one stays, and the next request builds another.
	if (!nextGraph_->assignGrown(*graphOwned_)) {
		growthState_.store(kGrowthTaken, std::memory_order_release);
		return;
	}
	graphOwned_.swap(nextGraph_);
	graphPtr_.store(graphOwned_.get(), std::memory_order_release);
	publishOrder();
	growthState_.store(kGrowthTaken, std::memory_order_release);
}

bool RackEngine::reapGrowth() noexcept {
	if (growthState_.load(std::memory_order_acquire) != kGrowthTaken) {
		return false;
	}
	nextGraph_.reset();
	growthState_.store(kGrowthIdle, std::memory_order_release);
	return true;
}

} // namespace feathers::rack

namespace feathers::rack {

//------------------------------------------------------------------------------------------------//
// ROUTE ROOM — backlog OJ                                                                           //
//------------------------------------------------------------------------------------------------//

void RackEngine::reserveRoutes(size_t routes) {
	// CHUNKS ADDED, NOTHING MOVED: the drain may be writing an input, or copying handed-over routes, right now.
	inputPool_.grow(routes);
	hostRoutes_.grow(routes);
	routeScratch_.grow(routes);
}

} // namespace feathers::rack
