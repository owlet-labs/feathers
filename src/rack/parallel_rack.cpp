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

#include "rack/parallel_rack.h"

#include "rack/nesting.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "dsp/analyser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace feathers::rack::parallel {

namespace {

/// 5 ms, counted in samples and carried across blocks — the Chain's ramp (`fx::busutil`'s reasons). Also each half of
/// a mode change's fade to silence and back (stage 2, §2), and Blend's glide when the position moves.
constexpr double kRampSeconds = 0.005;
/// SELECT'S CHANGE OF CHAIN: 10 ms, equal-gain (DECIDED 2026-10-27, Q3) — the chains share an input, so their outputs
/// are correlated and an equal-power law would bump at the midpoint.
constexpr double kSelectSeconds = 0.010;

/// THE CHAIN METERS' BALLISTICS — `RackEngine::kOutputMeterHoldSeconds`, `kOutputMeterFallDbPerSecond` and
/// `kOutputMeterSilence`, restated because those are private to the engine (PROVISIONAL there; change them together).
constexpr double kMeterHoldSeconds = 0.5;
constexpr double kMeterFallDbPerSecond = 24.0;
constexpr float kMeterSilence = 1.0e-5f;

/// AN APPENDING SINK OVER A VECTOR — a chain's body is written here first, so its length can lead it.
device::ByteSink vectorSink(std::vector<uint8_t>* out) {
	return device::ByteSink { out, [](void* ctx, const uint8_t* data, size_t bytes) {
		                         auto* v = static_cast<std::vector<uint8_t>*>(ctx);
		                         v->insert(v->end(), data, data + bytes);
	                         } };
}

template <typename T> void put(const device::ByteSink& sink, T value) {
	uint8_t bytes[sizeof(T)];
	std::memcpy(bytes, &value, sizeof(T));
	sink(bytes, sizeof(T));
}

/// A BOUNDED READER — every read checks the bytes remaining, so a short or lying body is refused, never walked past.
struct Reader {
	const uint8_t* data = nullptr;
	size_t size = 0;
	size_t at = 0;
	template <typename T> bool get(T& out) {
		if (size - at < sizeof(T)) {
			return false;
		}
		std::memcpy(&out, data + at, sizeof(T));
		at += sizeof(T);
		return true;
	}
	bool skip(size_t n) {
		if (size - at < n) {
			return false;
		}
		at += n;
		return true;
	}
};


/// THE EXPORT TABLE'S BYTES (backlog NZ (b)), after the chains: count u16, then row u16 · id u32 (0 for a macro) ·
/// index u32 each — the WHOLE index since version 3 (NZ, any depth: a row may name an inner holder's export, whose
/// scope bits ride in the index). Version 2 wrote it u16 and still reads.
void putExports(const device::ByteSink& sink, const std::vector<RackPatch::Export>& exports) {
	put(sink, static_cast<uint16_t>(exports.size()));
	for (const RackPatch::Export& e : exports) {
		put(sink, static_cast<uint16_t>(e.row));
		put(sink, static_cast<uint32_t>(e.device == kNoInstance ? 0u : e.device));
		put(sink, static_cast<uint32_t>(e.index));
	}
}
bool getExports(Reader& in, std::vector<RackPatch::Export>* out, uint16_t version = kBodyVersion) {
	uint16_t count = 0;
	if (!in.get(count) || count > device::kSiteExportRows) {
		return false;
	}
	for (uint16_t i = 0; i < count; ++i) {
		uint16_t row = 0;
		uint32_t id = 0;
		uint32_t index = 0;
		if (!in.get(row) || !in.get(id)) {
			return false;
		}
		if (version >= 3) {
			if (!in.get(index)) {
				return false;
			}
		}
		else {
			uint16_t narrow = 0;
			if (!in.get(narrow)) {
				return false;
			}
			index = narrow;
		}
		if (row >= device::kSiteExportRows) {
			return false;
		}
		out->push_back({ row, id == 0 ? kNoInstance : static_cast<DeviceInstanceId>(id), index });
	}
	return true;
}
} // namespace

//------------------------------------------------------------------------------------------------//
// A CHAIN, AND A LINEUP OF THEM                                                                    //
//------------------------------------------------------------------------------------------------//

struct Rack::Branch {
	Branch(uint16_t chainId, backplane::InstanceCounter* ids) : id(chainId), chain(kRackAxes, ids) {}

	uint16_t id = 0;
	InnerChain chain;
	/// HOW MANY SELF-CLOCKED DEVICES ITS LAST NOTE CALL FOUND (backlog RN): when it falls to none, the notes they invented
	/// are ended for them. AUDIO THREAD.
	size_t emitters = 0;
	/// THE HAND'S VALUES — written on the message thread, read by the audio thread once a call.
	std::atomic<float> level { static_cast<float>(composite::kFaderUnity) };
	std::atomic<bool> mute { false };
	std::atomic<bool> solo { false };
	/// **ITS SLOT** (backlog NO): the block of slot params its level, mute and solo are, for its life — set on the message
	/// thread before the lineup that holds it is published, so the audio thread reads it plainly.
	uint8_t slot = kNoChainSlot;
	/// **WHETHER THE WINDOW SHOWS THIS CHAIN OPEN** (backlog OC) — view state, kept with the patch under a stable chain id so it
	/// survives closing the editor and a save/load. Message thread only; the audio thread never reads it. A chain starts closed.
	bool open = false;

	// AUDIO THREAD.
	/// **THIS CHAIN WAITS FOR THE SLOWEST** — delayed by (the rack's latency − this chain's).
	device::CompensationDelay align;
	/// What `chain.setEnabled` was last told, so a block that changes nothing re-aims nothing.
	bool chainEnabled = true;
	float gainNow = 1.0f;
	float gainTarget = 1.0f;
	float gainStep = 0.0f;
	int remaining = 0;
	bool primed = false;
	/// **THE MODE'S WEIGHT** (stage 2, §1's w_k): 1 in Parallel, Select's 0/1, Blend's triangle — multiplying the gain
	/// above. Ramped by `Rack::aimWeights`, which retargets every chain on one count so the weights keep their sum.
	float weightNow = 1.0f;
	float weightTarget = 1.0f;
	float weightStep = 0.0f;
	int weightRemaining = 0;
	bool weightPrimed = false;
	/// **A CHAIN THE MODE GIVES NO ROLE** (§2a). `dormant`: not run at all — Stop took it, or Ring out heard its tail
	/// reach exact zero. `stoppedHard`: Stop took it, so it is reset before it runs again (a frozen tail must not
	/// resume). `runs` counts the calls it ran, for the cases.
	bool dormant = false;
	bool stoppedHard = false;
	uint64_t runs = 0;
	/// How many samples in a row this chain's output has been exactly zero — what its alignment delay holds.
	uint32_t quiet = 0;
	/// **THE CHAIN'S COURTESY METER** (backlog OC): the peak of what this chain puts INTO the sum (after its fader, mute
	/// and solo, so a silenced chain reads silent), gathered per call, held and falling with `RackEngine`'s ballistics
	/// (`kOutputMeterHoldSeconds`/`kOutputMeterFallDbPerSecond`), and published as one atomic float. NEGATIVE until the
	/// chain has run, so "never ran" is not drawn as "silent". Audio thread, but `peak` — which the editor reads.
	float chunkPeak = 0.0f;
	float heldPeak = 0.0f;
	int64_t holdLeft = 0;
	std::atomic<float> peak { -1.0f };
};

struct Rack::Spectrum {
	dsp::Analyser<32> analyser;
};

struct Rack::Lineup {
	uint64_t serial = 0;
	std::vector<Branch*> branches;
};

Rack::Rack() = default;

Rack::~Rack() {
	// The audio thread is done with this instance (the host destroys a device only after it stops running it).
	live_.store(nullptr, std::memory_order_relaxed);
	pending_.store(nullptr, std::memory_order_relaxed);
}

//------------------------------------------------------------------------------------------------//
// MESSAGE THREAD                                                                                   //
//------------------------------------------------------------------------------------------------//

InnerChain* Rack::chainAt(size_t index) noexcept {
	return index < chains_.size() ? &chains_[index]->chain : nullptr;
}

const InnerChain* Rack::chainAt(size_t index) const noexcept {
	return index < chains_.size() ? &chains_[index]->chain : nullptr;
}


size_t Rack::indexOfChain(uint16_t id) const noexcept {
	for (size_t i = 0; i < chains_.size(); ++i) {
		if (chains_[i]->id == id) {
			return i;
		}
	}
	return SIZE_MAX;
}
uint16_t Rack::chainIdAt(size_t index) const noexcept {
	return index < chains_.size() ? chains_[index]->id : uint16_t { 0xFFFF };
}

device::HostSlotLink Rack::linkFor(const Branch& branch) const {
	// EACH CHAIN REPORTS UNDER ITS OWN `lane` (SKLITTER's rule), by its stable id so an add or a remove re-stamps
	// nothing. The field is eight bits: past 256 chains two share a lane's report (host-slot naming only — the
	// shared counter keeps every device's path distinct). Backlog NO records it as open.
	device::HostSlotLink link = slotLink_;
	link.lane = static_cast<uint8_t>(branch.id & 0xFFu);
	return link;
}

void Rack::prepareBranch(Branch& branch) {
	branch.chain.prepare(sampleRate_, maxFrames_);
	branch.align.prepare(sampleRate_);
}

std::unique_ptr<Rack::Branch> Rack::makeBranch(uint16_t id) {
	auto branch = std::make_unique<Branch>(id, &ids_);
	branch->chain.setVoices(0); // see `setVoices`
	branch->chain.setOuterGate(chainGate());
	branch->chain.setHostSlots(linkFor(*branch));
	if (adopted_) {
		branch->chain.adopt(catalogue_);
		// THE RACK'S MACROS ARE THIS DEVICE'S `Macro1..8` in every chain (T11), as the Chain's are.
		branch->chain.bridge()->setMacrosAreDeviceControls(true);
		branch->chain.bridge()->setOwnerExports(&exports_); // NZ, any depth: a re-export here keeps the row it names
		branch->chain.setHostSlots(linkFor(*branch));
	}
	if (sampleRate_ > 0.0) {
		prepareBranch(*branch);
	}
	return branch;
}

void Rack::publish() {
	auto lineup = std::make_unique<Lineup>();
	lineup->serial = nextSerial_++;
	lineup->branches.reserve(chains_.size());
	for (const auto& branch : chains_) {
		lineup->branches.push_back(branch.get());
	}
	Lineup* raw = lineup.get();
	lineups_.push_back(std::move(lineup));
	pending_.store(raw, std::memory_order_release);
	++revision_;
	reap();
	retextIfCountMoved();
}

void Rack::retextIfCountMoved() {
	if (chains_.size() == textedCount_) {
		return;
	}
	textedCount_ = chains_.size();
	if (slotLink_.host != nullptr && !slotLink_.prefix.empty() && slotLink_.retext != nullptr) {
		slotLink_.retext(slotLink_.host, slotLink_.prefix);
	}
}

void Rack::reap() {
	const uint64_t adopted = adoptedSerial_.load(std::memory_order_acquire);
	const Lineup* live = live_.load(std::memory_order_acquire);
	std::erase_if(lineups_, [&](const std::unique_ptr<Lineup>& l) { return l->serial < adopted && l.get() != live; });
	std::erase_if(retired_, [&](const Retired& r) { return r.serial <= adopted; });
}

device::OuterGate Rack::chainGate() const {
	device::OuterGate gate = outerGate_;
	gate.perVoiceUnavailable = kPolyInsideRack;
	gate.setOwnField = nullptr; // the rack's own controls are the rack's to mirror, not its chains'
	return gate;
}

uint8_t Rack::freeSlot(uint8_t wanted) const noexcept {
	const auto taken = [&](uint8_t s) {
		for (const auto& b : chains_) {
			if (b->slot == s) {
				return true;
			}
		}
		return std::find(releasing_.begin(), releasing_.end(), s) != releasing_.end();
	};
	if (wanted < kChainSlots && !taken(wanted)) {
		return wanted;
	}
	for (uint8_t s = 0; s < kChainSlots; ++s) {
		if (!taken(s)) {
			return s;
		}
	}
	return kNoChainSlot;
}

void Rack::mirror(const Branch& b) const {
	if (b.slot >= kChainSlots || outerGate_.setOwnField == nullptr) {
		return;
	}
	outerGate_.setOwnField(outerGate_.dirtyHost, outerGate_.self, slotParam(b.slot, SlotControl::Level),
	                       b.level.load(std::memory_order_relaxed));
	outerGate_.setOwnField(outerGate_.dirtyHost, outerGate_.self, slotParam(b.slot, SlotControl::Mute),
	                       b.mute.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
	outerGate_.setOwnField(outerGate_.dirtyHost, outerGate_.self, slotParam(b.slot, SlotControl::Solo),
	                       b.solo.load(std::memory_order_relaxed) ? 1.0f : 0.0f);
}

void Rack::remirror() const {
	for (const auto& b : chains_) {
		mirror(*b);
	}
}

void Rack::markMirrorDue() {
	// NOT WRITTEN HERE: a load and a new gate arrive inside the host's provisioning, and a mirror write is a host edit
	// that provisions again (measured: the nested rack's case recursed until the stack went). The host's service beat
	// writes them (`serviceMirror`); until it has, the body is what is heard, so a stale base is never.
	mirrorDue_ = true;
	mirrored_.store(false, std::memory_order_relaxed);
}

bool Rack::adoptOwnFields(const float* fields, size_t present) {
	// NOTHING TO ADOPT WHILE A MIRROR IS OWED: then the bases are stale and the body is the truth they will be put to.
	if (fields == nullptr || mirrorDue_ || outerGate_.setOwnField == nullptr) {
		return false;
	}
	bool moved = false;
	for (const auto& b : chains_) {
		if (b->slot >= kChainSlots) {
			continue;
		}
		const auto at = [&](SlotControl c) {
			const size_t i = slotParam(b->slot, c);
			return i < present && std::isfinite(fields[i]) ? fields[i] : std::numeric_limits<float>::quiet_NaN();
		};
		const float level = at(SlotControl::Level);
		const float mute = at(SlotControl::Mute);
		const float solo = at(SlotControl::Solo);
		if (!std::isnan(level) && b->level.exchange(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed) != std::clamp(level, 0.0f, 1.0f)) {
			moved = true;
		}
		if (!std::isnan(mute) && b->mute.exchange(mute >= 0.5f, std::memory_order_relaxed) != (mute >= 0.5f)) {
			moved = true;
		}
		if (!std::isnan(solo) && b->solo.exchange(solo >= 0.5f, std::memory_order_relaxed) != (solo >= 0.5f)) {
			moved = true;
		}
	}
	if (moved) {
		++revision_;
		markDirty();
	}
	return moved;
}

bool Rack::serviceMirror() {
	if (!mirrorDue_ || outerGate_.setOwnField == nullptr) {
		return false;
	}
	mirrorDue_ = false;
	remirror();
	mirrored_.store(true, std::memory_order_relaxed);
	return true;
}

uint8_t Rack::slotOf(size_t index) const noexcept { return index < chains_.size() ? chains_[index]->slot : kNoChainSlot; }

size_t Rack::releasedControls(size_t* out, size_t capacity) const noexcept {
	size_t n = 0;
	for (const uint8_t s : releasing_) {
		for (size_t c = 0; c < kSlotControls && n < capacity; ++c) {
			out[n++] = slotParam(s, static_cast<SlotControl>(c));
		}
	}
	return n;
}

void Rack::controlsReleased() noexcept { releasing_.clear(); }

void Rack::markDirty() const {
	// THE HOLDER'S FLAG, through the gate it handed this rack (backlog JV) — a load never comes here.
	if (outerGate_.markDirty != nullptr) {
		outerGate_.markDirty(outerGate_.dirtyHost);
	}
}

size_t Rack::addChain() {
	if (!adopted_) {
		return SIZE_MAX;
	}
	auto branch = makeBranch(nextChainId_++);
	branch->slot = freeSlot(kNoChainSlot);
	mirror(*branch); // a reused slot's params put back to a fresh chain's values
	chains_.push_back(std::move(branch));
	publish();
	markDirty();
	return chains_.size() - 1;
}

bool Rack::removeChain(size_t index) {
	if (index >= chains_.size()) {
		return false;
	}
	// ITS SLOT WAITS FOR THE HOST TO PRUNE THE ROUTES INTO IT before another chain may take it (backlog NO).
	if (chains_[index]->slot < kChainSlots) {
		releasing_.push_back(chains_[index]->slot);
	}
	// FREED ONCE THE AUDIO THREAD HAS ADOPTED THE LINEUP PUBLISHED NEXT, the first one without it.
	retired_.push_back({ nextSerial_, std::move(chains_[index]) });
	chains_.erase(chains_.begin() + static_cast<std::ptrdiff_t>(index));
	publish();
	markDirty();
	return true;
}

Rack::ChainSnapshot Rack::snapshotChain(size_t index) const {
	ChainSnapshot out;
	if (index >= chains_.size()) {
		return out;
	}
	const Branch& b = *chains_[index];
	out.id = b.id;
	out.level = b.level.load(std::memory_order_relaxed);
	out.mute = b.mute.load(std::memory_order_relaxed);
	out.solo = b.solo.load(std::memory_order_relaxed);
	out.open = b.open;
	out.slot = b.slot;
	(void)b.chain.saveState(vectorSink(&out.body));
	return out;
}

size_t Rack::insertChain(size_t index, const ChainSnapshot& snap) {
	if (!adopted_ || snap.id == 0xFFFF || indexOfChain(snap.id) != SIZE_MAX) {
		return SIZE_MAX;
	}
	auto branch = makeBranch(snap.id);
	// NOT YET IN ANY LINEUP, so nothing runs it: the provisioning road is the right one.
	if (!snap.body.empty() && !branch->chain.loadState(snap.body.data(), snap.body.size())) {
		return SIZE_MAX;
	}
	branch->level.store(std::clamp(snap.level, 0.0f, 1.0f), std::memory_order_relaxed);
	branch->mute.store(snap.mute, std::memory_order_relaxed);
	branch->solo.store(snap.solo, std::memory_order_relaxed);
	branch->open = snap.open;
	branch->slot = freeSlot(snap.slot);
	mirror(*branch);
	const size_t at = std::min(index, chains_.size());
	chains_.insert(chains_.begin() + static_cast<std::ptrdiff_t>(at), std::move(branch));
	nextChainId_ = std::max<uint16_t>(nextChainId_, static_cast<uint16_t>(snap.id + 1));
	publish();
	markDirty();
	return at;
}

float Rack::level(size_t index) const noexcept {
	return index < chains_.size() ? chains_[index]->level.load(std::memory_order_relaxed)
	                              : static_cast<float>(composite::kFaderUnity);
}

bool Rack::muted(size_t index) const noexcept {
	return index < chains_.size() && chains_[index]->mute.load(std::memory_order_relaxed);
}

bool Rack::soloed(size_t index) const noexcept {
	return index < chains_.size() && chains_[index]->solo.load(std::memory_order_relaxed);
}

// A WRITE THAT CHANGES WHAT A SAVE WOULD WRITE MARKS THE HOLDER DIRTY; one that changes nothing does not (JV's policy).
void Rack::setLevel(size_t index, float position) noexcept {
	const float v = std::clamp(position, 0.0f, 1.0f);
	if (index < chains_.size() && chains_[index]->level.exchange(v, std::memory_order_relaxed) != v) {
		++revision_;
		mirror(*chains_[index]);
		markDirty();
	}
}

void Rack::setMuted(size_t index, bool muted) noexcept {
	if (index < chains_.size() && chains_[index]->mute.exchange(muted, std::memory_order_relaxed) != muted) {
		++revision_;
		mirror(*chains_[index]);
		markDirty();
	}
}

void Rack::setSoloed(size_t index, bool soloed) noexcept {
	if (index < chains_.size() && chains_[index]->solo.exchange(soloed, std::memory_order_relaxed) != soloed) {
		++revision_;
		mirror(*chains_[index]);
		markDirty();
	}
}

bool Rack::open(size_t index) const noexcept { return index < chains_.size() && chains_[index]->open; }

void Rack::setOpen(size_t index, bool open) noexcept {
	// VIEW STATE, kept with the patch like a card's fold (`ParamBridge::setDeviceFolded`): a change marks the holder dirty,
	// it is NOT an undo step, and a write that changes nothing is not an edit.
	if (index < chains_.size() && chains_[index]->open != open) {
		chains_[index]->open = open;
		markDirty();
	}
}

void Rack::adopt(const device::Catalogue& catalogue) {
	catalogue_ = catalogue;
	adopted_ = true;
	for (auto& branch : chains_) {
		branch->chain.adopt(catalogue_);
		branch->chain.bridge()->setMacrosAreDeviceControls(true);
		branch->chain.bridge()->setOwnerExports(&exports_);
		branch->chain.setHostSlots(linkFor(*branch));
	}
	// **A FRESH RACK HOLDS ONE EMPTY CHAIN, WHICH IS THE IDENTITY** — an empty chain is the dry path, so one of
	// them sums to the input. Two would sum to +6 dB. A body loaded next replaces it.
	if (chains_.empty()) {
		chains_.push_back(makeBranch(nextChainId_++));
		chains_.back()->slot = freeSlot(kNoChainSlot); // its slot params' defaults are a fresh chain's values already
		publish();
	}
	// PROVISIONING, so nothing runs this instance yet: the lineup is live now rather than at a drain.
	adoptPending();
	reap();
}

void Rack::prepare(double sampleRate, int maxFrames) {
	// THE NOTE ROAD'S STORES (backlog RN), here and nowhere else.
	noteRecords_.assign(kMaxNoteRecords, NoteRecord {});
	staged_.assign(kMaxStagedNotes, StagedNote {});
	chainNotes_.assign(RackEngine::kMaxNotes, device::NoteEvent {});
	owedOffs_.assign(RackEngine::kMaxNotes, device::NoteEvent {});
	recordCount_ = 0;
	stagedCount_ = 0;
	owedCount_ = 0;
	sampleRate_ = sampleRate;
	maxFrames_ = maxFrames;
	reap();
	const auto scratch = static_cast<size_t>(device::scratchFrames(maxFrames));
	for (std::vector<float>* v : { &inL_, &inR_, &workL_, &workR_, &sumL_, &sumR_ }) {
		v->assign(scratch, 0.0f);
	}
	dryDelay_.prepare(sampleRate);
	rampSamples_ = std::max(1, static_cast<int>(std::lround(kRampSeconds * std::max(sampleRate, 1.0))));
	selectSamples_ = std::max(1, static_cast<int>(std::lround(kSelectSeconds * std::max(sampleRate, 1.0))));
	for (Phantom& p : phantoms_) {
		p.align.prepare(sampleRate);
	}
	for (int k = 0; k < kMaxBands; ++k) {
		bandL_[k].assign(scratch, 0.0f);
		bandR_[k].assign(scratch, 0.0f);
	}
	lr4Engine_.reset();
	lr2Engine_.reset();
	coeffsValid_ = false;
	// THE ANALYSER ALLOCATES HERE, never on the audio thread; it runs only while a view holds the lease.
	if (spectrum_ == nullptr) {
		spectrum_ = std::make_unique<Spectrum>();
	}
	spectrum_->analyser.prepare(sampleRate);
	spectrum_->analyser.setTaps(true, true);
	leaseSamples_ = static_cast<int64_t>(0.5 * std::max(sampleRate, 1.0));
	tapL_.assign(scratch, 0.0f);
	tapR_.assign(scratch, 0.0f);
	readings_.fill(dsp::kAnalyserFloorDb);
	readings_[static_cast<size_t>(Reading::FrameA)] = readings_[static_cast<size_t>(Reading::FrameB)] = 0.0f;
	// EVERY CHAIN THE AUDIO THREAD COULD STILL HOLD — a removed one may be in the live lineup until the next drain.
	for (auto& branch : chains_) {
		prepareBranch(*branch);
	}
	for (Retired& r : retired_) {
		prepareBranch(*r.branch);
	}
}

void Rack::reset() {
	releaseAll_ = true; // every note the rack let out is ended at its next note call (backlog RN): nothing sticks
	dryDelay_.reset();
	primed_ = false;
	wetNow_ = wetTarget_;
	dryNow_ = dryTarget_;
	wetStep_ = 0.0f;
	dryStep_ = 0.0f;
	remaining_ = 0;
	// A RESET FORGETS THE NOTE ("as if just switched on": Key Track falls back to the knobs until a key is held).
	heardNote_ = false;
	cornersPrimed_ = false;
	// A RESET LANDS IN THE MODE ASKED FOR, AT FULL LEVEL: nothing is sounding to fade.
	takeWantedStructure();
	lr4Engine_.reset();
	lr2Engine_.reset();
	modeGain_ = 1.0f;
	modeStep_ = 0.0f;
	modeRemaining_ = 0;
	for (Phantom& p : phantoms_) {
		p.align.reset();
		p.primed = false;
		p.remaining = 0;
		p.quiet = UINT32_MAX;
	}
	if (const Lineup* live = live_.load(std::memory_order_acquire); live != nullptr) {
		for (Branch* b : live->branches) {
			b->chain.reset();
			b->align.reset();
			b->primed = false;
			b->weightPrimed = false;
			b->weightRemaining = 0;
			b->remaining = 0;
			b->quiet = 0;
			b->chunkPeak = b->heldPeak = 0.0f;
			b->holdLeft = 0;
			b->peak.store(-1.0f, std::memory_order_relaxed);
		}
	}
}

void Rack::setVoices(size_t voices) {
	// **THE HOST'S VOICES DO NOT PASS THROUGH A RACK** (backlog NO, option A — stop-and-ask, 2026-10-18). A chain
	// passes them through (rack roadmap §8) because its per-voice road can carry its one answer back in the voice:
	// a rack would need ONE ANSWER PER CHAIN, and the voice carries one signal. MEASURED: the synth applies a voice's
	// level and edge AFTER the per-voice stage (`Synth::renderChunk`), so a rack that kept the voice dry and added
	// each chain's per-voice difference on the bus was wrong by 0.88 against a 0.26 peak on every attack. So the
	// chains are told NO voices: a Poly device inside a rack runs once, on the bus, as it would with Poly off — the
	// "default trivial handling" (DU). Making it exact needs the host to hand a device each voice's post-gain;
	// backlog NO records that as open.
	voices_ = voices;
}

void Rack::setOuterGate(const device::OuterGate& gate) {
	// A NEW HOLDER (or a first one) MIRRORS AFRESH; the same gate handed again at a re-provision changes nothing.
	const bool newHolder = gate.setOwnField != outerGate_.setOwnField || gate.dirtyHost != outerGate_.dirtyHost
	                       || gate.self != outerGate_.self;
	outerGate_ = gate;
	if (gate.setOwnField == nullptr) {
		mirrorDue_ = false;
		mirrored_.store(false, std::memory_order_relaxed);
	}
	else if (newHolder) {
		markMirrorDue();
	}
	for (auto& branch : chains_) {
		branch->chain.setOuterGate(chainGate());
	}
}

void Rack::setHostSlots(const device::HostSlotLink& link) {
	slotLink_ = link;
	for (auto& branch : chains_) {
		branch->chain.setHostSlots(linkFor(*branch));
	}
}

device::RouteDrop Rack::service() {
	reap();
	device::RouteDrop merged {};
	for (auto& branch : chains_) {
		const device::RouteDrop one = branch->chain.service();
		if (one.count == 0) {
			continue;
		}
		if (merged.count == 0) {
			merged.firstSource = one.firstSource;
			merged.firstDestination = one.firstDestination;
		}
		merged.count += one.count;
	}
	return merged;
}

InnerChain* Rack::holding(DeviceInstanceId id) const noexcept {
	for (const auto& branch : chains_) {
		const RackBridge* bridge = branch->chain.bridge();
		if (bridge != nullptr && bridge->descriptorOf(id) != nullptr) {
			return &branch->chain;
		}
	}
	return nullptr;
}

//------------------------------------------------------------------------------------------------//
// THE BODY                                                                                         //
//                                                                                                  //
//     magic u32  kBodyMagic        version u16  kBodyVersion        count u32  chains, display order //
//     per chain: id u16 · fields u8 · per field { keyLen u8 · key · value f32 } · bodyLen u32 · body  //
//                                                                                                  //
// THE CHAIN'S OWN CONTROLS ARE STORED BY KEY (`kLevelKey`, …), the schema ledger's rule applied inside //
// the body: a reader skips a key it does not know and defaults a key that is absent. The body is      //
// exactly `InnerChain::saveState`'s.                                                                 //
//------------------------------------------------------------------------------------------------//

bool Rack::saveState(const device::ByteSink& sink) const {
	put(sink, kBodyMagic);
	put(sink, kBodyVersion);
	put(sink, static_cast<uint32_t>(chains_.size()));
	for (const auto& branch : chains_) {
		put(sink, branch->id);
		const struct {
			const char* key;
			float value;
		} fields[] = {
			{ kLevelKey, branch->level.load(std::memory_order_relaxed) },
			{ kMuteKey, branch->mute.load(std::memory_order_relaxed) ? 1.0f : 0.0f },
			{ kSoloKey, branch->solo.load(std::memory_order_relaxed) ? 1.0f : 0.0f },
			{ kOpenKey, branch->open ? 1.0f : 0.0f },
			{ kSlotKey, branch->slot < kChainSlots ? static_cast<float>(branch->slot) : -1.0f },
		};
		put(sink, static_cast<uint8_t>(std::size(fields)));
		for (const auto& f : fields) {
			const auto len = static_cast<uint8_t>(std::strlen(f.key));
			put(sink, len);
			sink(reinterpret_cast<const uint8_t*>(f.key), len);
			put(sink, f.value);
		}
		std::vector<uint8_t> body;
		if (!branch->chain.saveState(vectorSink(&body))) {
			return false;
		}
		put(sink, static_cast<uint32_t>(body.size()));
		sink(body.data(), body.size());
	}
	std::vector<RackPatch::Export> exports;
	exports_.save(&exports);
	putExports(sink, exports);
	return true;
}

bool Rack::loadState(const uint8_t* data, size_t bytes) { return readBody(data, bytes, true); }

bool Rack::applyState(const uint8_t* data, size_t bytes) {
	if (!adopted_) {
		return false; // nothing could go in a chain: `addChain`'s rule
	}
	if (bytes == 0) {
		// A FRESH RACK — one empty chain, the identity (`adopt`) — and no exports. The old chains retire as a removal's do.
		for (auto& old : chains_) {
			if (old->slot < kChainSlots) {
				releasing_.push_back(old->slot);
			}
			retired_.push_back({ nextSerial_, std::move(old) });
		}
		chains_.clear();
		nextChainId_ = 0;
		chains_.push_back(makeBranch(nextChainId_++));
		chains_.back()->slot = freeSlot(kNoChainSlot);
		markMirrorDue();
		exports_.load({});
		publish();
		markDirty();
		return true;
	}
	// THE DEPTH GATE (`fitsNesting`): a preset too tall for where this Rack's chains stand is refused whole, before
	// any chain is built from it (2026-10-31).
	if (!bodyFitsChainsAt(catalogue_.depth, std::span<const uint8_t>(data, bytes), catalogue_)) {
		return false;
	}
	if (!readBody(data, bytes, false)) {
		return false;
	}
	markDirty();
	return true;
}

bool Rack::readBody(const uint8_t* data, size_t bytes, bool provision) {
	if (data == nullptr || bytes == 0 || bytes > kMaxRackBodyBytes) {
		return false;
	}
	Reader in { data, bytes, 0 };
	uint32_t magic = 0;
	uint16_t version = 0;
	uint32_t count = 0;
	if (!in.get(magic) || magic != kBodyMagic || !in.get(version) || (version != kBodyVersion && version != 2) || !in.get(count)) {
		return false;
	}
	// ALL OR NOTHING: every chain is built and loaded BESIDE the running ones, and only a body that reads whole
	// replaces them. Nothing of the old rack is touched on a refusal.
	std::vector<std::unique_ptr<Branch>> next;
	uint16_t highest = 0;
	for (uint32_t c = 0; c < count; ++c) {
		uint16_t id = 0;
		uint8_t fields = 0;
		if (!in.get(id) || !in.get(fields)) {
			return false;
		}
		float level = static_cast<float>(composite::kFaderUnity);
		bool mute = false;
		bool solo = false;
		bool open = false;
		uint8_t slot = kNoChainSlot;
		for (uint8_t f = 0; f < fields; ++f) {
			uint8_t len = 0;
			if (!in.get(len) || in.size - in.at < len) {
				return false;
			}
			const std::string key(reinterpret_cast<const char*>(in.data + in.at), len);
			(void)in.skip(len);
			float value = 0.0f;
			if (!in.get(value) || !std::isfinite(value)) {
				return false;
			}
			if (key == kLevelKey) {
				level = std::clamp(value, 0.0f, 1.0f);
			}
			else if (key == kMuteKey) {
				mute = value >= 0.5f;
			}
			else if (key == kOpenKey) {
				open = value >= 0.5f;
			}
			else if (key == kSoloKey) {
				solo = value >= 0.5f;
			}
			else if (key == kSlotKey) {
				slot = value >= 0.0f && value < static_cast<float>(kChainSlots) ? static_cast<uint8_t>(value) : kNoChainSlot;
			}
		}
		uint32_t bodyBytes = 0;
		if (!in.get(bodyBytes) || bodyBytes > kMaxBodyBytes || in.size - in.at < bodyBytes) {
			return false;
		}
		for (const auto& b : next) {
			if (b->id == id) {
				return false; // two chains under one id is not a body this rack wrote
			}
		}
		auto branch = makeBranch(id);
		if (bodyBytes > 0 && !branch->chain.loadState(in.data + in.at, bodyBytes)) {
			return false;
		}
		(void)in.skip(bodyBytes);
		branch->level.store(level, std::memory_order_relaxed);
		branch->mute.store(mute, std::memory_order_relaxed);
		branch->solo.store(solo, std::memory_order_relaxed);
		branch->open = open;
		branch->slot = slot; // taken (or re-chosen) below, once the old lineup's slots are accounted for
		highest = std::max(highest, id);
		next.push_back(std::move(branch));
	}
	std::vector<RackPatch::Export> exports;
	if (!getExports(in, &exports, version) || in.at != bytes) {
		return false;
	}
	// THE OLD CHAINS' SLOTS: a project load (`provision`) brings its routes with it, so they are the body's to keep; a
	// body applied into a running rack (a preset) leaves routes into the old chains, which the host prunes first. And a
	// prune still owed from before a project load is owed to a graph the load replaced: it would cut the loaded routes.
	if (provision) {
		releasing_.clear();
	}
	for (auto& old : chains_) {
		if (!provision && old->slot < kChainSlots) {
			releasing_.push_back(old->slot);
		}
		retired_.push_back({ nextSerial_, std::move(old) });
	}
	chains_.clear();
	for (auto& b : next) {
		const uint8_t wanted = b->slot;
		b->slot = kNoChainSlot;
		chains_.push_back(std::move(b));
		chains_.back()->slot = freeSlot(wanted);
	}
	nextChainId_ = static_cast<uint16_t>(chains_.empty() ? 0 : highest + 1);
	markMirrorDue();
	exports_.load(exports);
	publish();
	if (!provision) {
		return true; // A RUNNING RACK: the audio thread adopts the lineup at its drain, and `reap` frees the old after it
	}
	// PROVISIONING (the contract of `ContainerOps::loadState`): nothing runs this instance, so the lineup is live now.
	adoptPending();
	reap();
	return true;
}

//------------------------------------------------------------------------------------------------//
// AUDIO THREAD                                                                                     //
//------------------------------------------------------------------------------------------------//

void Rack::installLive(Lineup* lineup) noexcept {
	live_.store(lineup, std::memory_order_release);
	// A CHAIN THAT ARRIVED BETWEEN TWO BLOCKS has missed what the host said this block: say it now.
	for (Branch* b : lineup->branches) {
		if (b->chainEnabled != enabled_) {
			b->chain.setEnabled(enabled_);
			b->chainEnabled = enabled_;
		}
		for (size_t n = 0; n < composite::kMacroCount; ++n) {
			b->chain.engine().setMacro(n, macros_[n]);
		}
		b->chain.setTransportClock(clock_);
		b->chain.setNoteContext(note_);
		if (haveOuter_) {
			b->chain.setOuterSources(outer_);
		}
	}
	adoptedSerial_.store(lineup->serial, std::memory_order_release);
}

void Rack::adoptPending() noexcept {
	if (Lineup* next = pending_.exchange(nullptr, std::memory_order_acq_rel); next != nullptr) {
		installLive(next);
	}
}

void Rack::aim() noexcept {
	const float g = faderGain(gain_);
	wetTarget_ = enabled_ ? mix_ * g : 0.0f;
	dryTarget_ = enabled_ ? (1.0f - mix_) * g : 1.0f;
	if (!primed_) {
		wetNow_ = wetTarget_;
		dryNow_ = dryTarget_;
		wetStep_ = 0.0f;
		dryStep_ = 0.0f;
		remaining_ = 0;
		return;
	}
	if (wetTarget_ == wetNow_ && dryTarget_ == dryNow_ && remaining_ == 0) {
		return;
	}
	const auto n = static_cast<float>(rampSamples_);
	wetStep_ = (wetTarget_ - wetNow_) / n;
	dryStep_ = (dryTarget_ - dryNow_) / n;
	remaining_ = rampSamples_;
}

namespace {

/// **w_k** (§1): the weight `mode` gives chain `k` of `chains` at `position` — 1 in Parallel, the nearest chain in
/// Select, the two neighbours' linear triangle in Blend (weights summing to one, so identical chains sound the same at
/// every position; DECIDED 2026-10-27, Q3).
float modeWeight(Mode mode, double position, size_t k, size_t chains, int bands) noexcept {
	switch (mode) {
	case Mode::Select:
	case Mode::Blend: return positionWeight(mode, position, k, chains);
	case Mode::MidSide:
	case Mode::LeftRight:
	case Mode::Bands: return k < roleCount(mode, chains, bands) ? 1.0f : 0.0f; // the roles at unity, the rest unrouted
	case Mode::Parallel:
	case Mode::kCount: break;
	}
	return 1.0f;
}

} // namespace

void Rack::aimWeights(const Lineup* live, bool snap) noexcept {
	const size_t count = live->branches.size();
	bool moved = false;
	for (size_t k = 0; k < count; ++k) {
		Branch* b = live->branches[k];
		const float target = modeWeight(mode_, position_, k, count, bands_);
		if (snap) {
			b->weightNow = b->weightTarget = target;
			b->weightStep = 0.0f;
			b->weightRemaining = 0;
			b->weightPrimed = true;
			continue;
		}
		if (!b->weightPrimed) {
			// A CHAIN THAT JOINS MID-RUN: at once in Parallel (stage 1 as built), else from silence with the rest, so
			// the weights glide on one count and keep their sum.
			b->weightNow = b->weightTarget = mode_ == Mode::Parallel ? target : 0.0f;
			b->weightRemaining = 0;
			b->weightPrimed = true;
		}
		moved = moved || target != b->weightTarget;
	}
	if (!moved) {
		return;
	}
	// EVERY CHAIN ON ONE COUNT: Select's crossfade is equal-gain only if the outgoing and incoming weights move together.
	const int samples = mode_ == Mode::Select ? selectSamples_ : rampSamples_;
	for (size_t k = 0; k < count; ++k) {
		Branch* b = live->branches[k];
		b->weightTarget = modeWeight(mode_, position_, k, count, bands_);
		b->weightStep = (b->weightTarget - b->weightNow) / static_cast<float>(samples);
		b->weightRemaining = samples;
	}
}

void Rack::setFields(const float* fields, size_t present) {
	const auto at = [&](Param p) {
		const auto i = static_cast<size_t>(p);
		return i < present && fields != nullptr ? fields[i] : static_cast<float>(kParamDescs[i].def);
	};
	enabled_ = at(Param::Enabled) >= 0.5f;
	mix_ = std::clamp(at(Param::Mix), 0.0f, 1.0f);
	gain_ = std::clamp(at(Param::Gain), 0.0f, 1.0f);
	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		macros_[n] = std::clamp(at(static_cast<Param>(static_cast<size_t>(Param::Macro1) + n)), 0.0f, 1.0f);
	}
	// A ROW WRITTEN BEFORE STAGE 2 STOPS AT THE MACROS: absent is the default, Parallel — stage 1 as built.
	const float mode = at(Param::Mode);
	const float position = at(Param::Position);
	modeWanted_ = static_cast<Mode>(
	    std::isfinite(mode) ? std::lround(std::clamp(mode, 0.0f, static_cast<float>(Mode::kCount) - 1.0f)) : 0L);
	position_ = std::isfinite(position) ? std::clamp(position, 0.0f, 1.0f) : 0.0f;
	unusedStop_ = at(Param::Unused) >= 0.5f;
	const auto choice = [&](Param p, int last) {
		const float v = at(p);
		return std::isfinite(v) ? static_cast<int>(std::lround(std::clamp(v, 0.0f, static_cast<float>(last)))) : 0;
	};
	bandsWanted_ = kBandCounts[choice(Param::BandCount, 2)];
	slopeWanted_ = static_cast<Slope>(choice(Param::Slope, 1));
	filteringWanted_ = static_cast<Filtering>(choice(Param::Filtering, 2));
	keyTrack_ = at(Param::KeyTrack) >= 0.5f;
	for (size_t k = 0; k < 4; ++k) {
		const float v = at(static_cast<Param>(static_cast<size_t>(Param::Cut1) + k));
		cutLog2_[k] = std::isfinite(v) ? v : static_cast<float>(kParamDescs[static_cast<size_t>(Param::Cut1) + k].def);
	}
	// THE CHAIN SLOTS (backlog NO), as resolved — base (the mirrored hand value) plus routes. Kept whenever the row
	// carries them; HEARD only where the holder mirrors and owes no mirror, else a bare rack hears its body.
	const bool slotsPresent = fields != nullptr && present >= kAllParamCount;
	if (slotsPresent) {
		for (size_t i = 0; i < slotFields_.size(); ++i) {
			const float v = fields[kSlotParamBase + i];
			slotFields_[i] = std::isfinite(v) ? v : static_cast<float>(kAllParamDescs[kSlotParamBase + i].def);
		}
	}
	slotFieldsLive_ = slotsPresent && mirrored_.load(std::memory_order_relaxed);
	aim();
	if (const Lineup* live = live_.load(std::memory_order_relaxed); live != nullptr) {
		for (Branch* b : live->branches) {
			// THE RACK'S BYPASS IS EVERY CHAIN'S: nothing inside runs while it is off, and each chain fades itself
			// over the same 5 ms the rack fades over.
			if (b->chainEnabled != enabled_) {
				b->chain.setEnabled(enabled_);
				b->chainEnabled = enabled_;
			}
			for (size_t n = 0; n < composite::kMacroCount; ++n) {
				b->chain.engine().setMacro(n, macros_[n]);
			}
		}
	}
}

void Rack::setNoteContext(const device::NoteContext& note) {
	note_ = note;
	// KEY TRACK: LATCHED ONLY WHILE A KEY IS HELD, so the corners hold the last note after the release (the Phaser's
	// rule, `fx::phaser::Phaser::setNoteContext`, frozen at the release by the same rule). Forgotten at `reset`.
	if (note.gate && std::isfinite(note.pitchSemitones)) {
		heardNote_ = true;
		noteSemitones_ = static_cast<double>(note.pitchSemitones);
	}
	if (const Lineup* live = live_.load(std::memory_order_relaxed); live != nullptr) {
		for (Branch* b : live->branches) {
			b->chain.setNoteContext(note);
		}
	}
}

void Rack::setTransportClock(const device::TransportClock& clock) {
	clock_ = clock;
	if (const Lineup* live = live_.load(std::memory_order_relaxed); live != nullptr) {
		for (Branch* b : live->branches) {
			b->chain.setTransportClock(clock);
		}
	}
}

void Rack::setOuterSources(const device::OuterSources& outer) {
	outer_ = outer;
	haveOuter_ = true;
	if (const Lineup* live = live_.load(std::memory_order_relaxed); live != nullptr) {
		for (Branch* b : live->branches) {
			b->chain.setOuterSources(outer);
		}
	}
}

void Rack::setSidechain(const float* left, const float* right, bool sounding) {
	sideL_ = left;
	sideR_ = right;
	sideSounding_ = sounding;
}

bool Rack::beginPerVoiceBlock() {
	// THE HOST'S BEGIN: the rack's drain point, then every chain's (each drains its own edits here). The answer is
	// always NO — see `setVoices`: nothing inside a rack runs per voice, so the host runs the rack on the bus alone.
	hosted_ = true;
	adoptPending();
	if (const Lineup* live = live_.load(std::memory_order_relaxed); live != nullptr) {
		for (Branch* b : live->branches) {
			(void)b->chain.beginPerVoiceBlock();
		}
	}
	return false;
}

void Rack::processVoice(size_t, float*, float*, int, const device::NoteContext&) {
	// NEVER ASKED FOR: `beginPerVoiceBlock` answers no. Declared only because a host takes its begin (the rack's
	// drain point) from a device that has both.
}

void Rack::roleInput(size_t role, int n) noexcept {
	const auto count = static_cast<size_t>(n);
	switch (mode_) {
	case Mode::MidSide:
		// THE EQ'S CONVENTION (§5, form A): M = ½(L+R) handed as (M, M); S = ½(L−R) handed as (S, −S), the side as it
		// really is in L/R — so two empty chains sum to (M+S, M−S) = (L, R).
		for (size_t i = 0; i < count; ++i) {
			const float l = inL_[i];
			const float r = inR_[i];
			if (role == 0) {
				workL_[i] = workR_[i] = 0.5f * (l + r);
			}
			else {
				const float s = 0.5f * (l - r);
				workL_[i] = s;
				workR_[i] = -s;
			}
		}
		return;
	case Mode::LeftRight:
		// (L, 0) and (0, R): a reverb on the left chain spreads into the right, as on a hard-left source.
		if (role == 0) {
			std::memcpy(workL_.data(), inL_.data(), count * sizeof(float));
			std::fill_n(workR_.begin(), count, 0.0f);
		}
		else {
			std::fill_n(workL_.begin(), count, 0.0f);
			std::memcpy(workR_.data(), inR_.data(), count * sizeof(float));
		}
		return;
	case Mode::Bands:
		// BEFORE AND BOTH: the role's band of the input; AFTER: the input itself, its band taken from the output.
		if (filtering_ != Filtering::After && role < static_cast<size_t>(kMaxBands)) {
			std::memcpy(workL_.data(), bandL_[role].data(), count * sizeof(float));
			std::memcpy(workR_.data(), bandR_[role].data(), count * sizeof(float));
			return;
		}
		break;
	case Mode::Parallel:
	case Mode::Select:
	case Mode::Blend:
	case Mode::kCount: break;
	}
	std::memcpy(workL_.data(), inL_.data(), count * sizeof(float));
	std::memcpy(workR_.data(), inR_.data(), count * sizeof(float));
}

bool Rack::structureIsWanted() const noexcept {
	if (modeWanted_ != mode_) {
		return false;
	}
	// IN BANDS, THE COUNT, THE SLOPE AND THE FILTERING ARE STRUCTURE TOO (§4a): each one changes the filters every
	// chain sits between, so a change fades like a mode change. Outside Bands they change nothing heard.
	return mode_ != Mode::Bands
	       || (bandsWanted_ == bands_ && slopeWanted_ == slope_ && filteringWanted_ == filtering_);
}

void Rack::takeWantedStructure() noexcept {
	const bool intoBands = modeWanted_ == Mode::Bands
	                       && (mode_ != Mode::Bands || bandsWanted_ != bands_ || slopeWanted_ != slope_
	                           || filteringWanted_ != filtering_);
	mode_ = modeWanted_;
	bands_ = bandsWanted_;
	slope_ = slopeWanted_;
	filtering_ = filteringWanted_;
	if (intoBands) {
		// A NEW SET OF FILTERS STARTS FROM REST, at the fade's silence: no state of another arrangement rings in it.
		if (slope_ == Slope::Lr4) {
			lr4Engine_.reset();
		}
		else {
			lr2Engine_.reset();
		}
		coeffsValid_ = false;
	}
}

void Rack::aimCorners(int frames) noexcept {
	// THE TARGETS: the cuts, or — KEY TRACK with a note heard — the note moved by each cut's offset (`cornerTargetsLog2`).
	const bool tracking = keyTrack_ && heardNote_;
	double targets[4];
	cornerTargetsLog2(cutLog2_, tracking, keyHzOf(noteSemitones_), targets);
	// A TRACKED NOTE GLIDES THE CORNERS IN 5 ms (the Phaser's `kKeySmoothSeconds`), so a new note or a glide the host
	// folds into the pitch slides them; landing within 1e-7 of an octave snaps. Untracked, they sit on the cuts at once,
	// as before Key Track existed — bit for bit.
	const double alpha = 1.0 - std::exp(-static_cast<double>(std::max(frames, 1)) / (kKeySmoothSeconds * std::max(sampleRate_, 1.0)));
	for (size_t k = 0; k < 4; ++k) {
		if (!tracking || !cornersPrimed_) {
			cornerLog2Now_[k] = targets[k];
			continue;
		}
		cornerLog2Now_[k] += alpha * (targets[k] - cornerLog2Now_[k]);
		if (std::fabs(targets[k] - cornerLog2Now_[k]) < kGlideSnap) {
			cornerLog2Now_[k] = targets[k];
		}
	}
	cornersPrimed_ = true;
	// THE ORDER, ENFORCED HERE (§4a), on the corners as tracked: each at least `kMinCornerOctaves` above the one below,
	// whatever a modulator or a note did. The UI pushes neighbours along; this is what makes it true of the sound.
	orderCorners(cornerLog2Now_, cornerHz_);
	if (coeffSlope_ != slope_) {
		coeffsValid_ = false;
		coeffSlope_ = slope_;
	}
	const auto rate = static_cast<float>(std::max(sampleRate_, 1.0));
	for (int k = 0; k + 1 < bands_; ++k) {
		if (coeffsValid_ && coeffHz_[k] == cornerHz_[k]) {
			continue; // ONLY THE CORNERS THAT MOVED (backlog ~1375: recomputing every call dominated OWLTT's cost)
		}
		const auto hz = static_cast<float>(cornerHz_[k]);
		if (slope_ == Slope::Lr4) {
			lr4_[k] = fx::owltt::Lr4Split::coeffsFor(hz, rate);
		}
		else {
			lr2_[k] = fx::owltt::Lr2Split::coeffsFor(hz, rate);
		}
		coeffHz_[k] = cornerHz_[k];
		++coefficientUpdates_;
	}
	// EVERY CORNER IN USE IS CURRENT NOW; one that comes into use later (a count raised) is computed then.
	for (int k = bands_ - 1; k < 4; ++k) {
		coeffHz_[k] = -1.0;
	}
	coeffsValid_ = true;
}

void Rack::splitBands(int n) noexcept {
	const auto run = [&](auto& engine, const auto* corners) {
		float out[kMaxBands] = {};
		for (int i = 0; i < n; ++i) {
			const auto s = static_cast<size_t>(i);
			// BOTH TAKES HALF THE PAIR HERE AND THE OTHER HALF AFTER THE CHAIN (backlog OS).
			if (filtering_ == Filtering::Both) {
				engine.halfSplit[0].process(inL_[s], corners, bands_, out);
			}
			else {
				engine.split[0].process(inL_[s], corners, bands_, out);
			}
			for (int k = 0; k < bands_; ++k) {
				bandL_[k][s] = out[k];
			}
			if (filtering_ == Filtering::Both) {
				engine.halfSplit[1].process(inR_[s], corners, bands_, out);
			}
			else {
				engine.split[1].process(inR_[s], corners, bands_, out);
			}
			for (int k = 0; k < bands_; ++k) {
				bandR_[k][s] = out[k];
			}
		}
	};
	if (slope_ == Slope::Lr4) {
		run(lr4Engine_, lr4_);
	}
	else {
		run(lr2Engine_, lr2_);
	}
}

void Rack::bandOfOutput(size_t role, int n) noexcept {
	if (filtering_ == Filtering::Before || role >= static_cast<size_t>(bands_)) {
		return;
	}
	const auto band = static_cast<int>(role);
	const auto run = [&](auto& engine, const auto* corners) {
		for (int i = 0; i < n; ++i) {
			const auto s = static_cast<size_t>(i);
			if (filtering_ == Filtering::Both) {
				workL_[s] = engine.halfPath[role][0].process(workL_[s], corners, bands_, band);
				workR_[s] = engine.halfPath[role][1].process(workR_[s], corners, bands_, band);
			}
			else {
				workL_[s] = engine.path[role][0].process(workL_[s], corners, bands_, band);
				workR_[s] = engine.path[role][1].process(workR_[s], corners, bands_, band);
			}
		}
	};
	if (slope_ == Slope::Lr4) {
		run(lr4Engine_, lr4_);
	}
	else {
		run(lr2Engine_, lr2_);
	}
}

void Rack::alignDryToBands(int n) noexcept {
	// ONE Π AP FOR EVERY FILTERING: Both's halves multiply to the LR bands, which sum to it as Before's do (backlog OS).
	const auto run = [&](auto& engine, const auto* corners) {
		for (int i = 0; i < n; ++i) {
			const auto s = static_cast<size_t>(i);
			inL_[s] = engine.dry[0].process(inL_[s], corners, bands_);
			inR_[s] = engine.dry[1].process(inR_[s], corners, bands_);
		}
	};
	if (slope_ == Slope::Lr4) {
		run(lr4Engine_, lr4_);
	}
	else {
		run(lr2Engine_, lr2_);
	}
}

uint32_t Rack::routedLatency(const Lineup* live, double sampleRate) const noexcept {
	// **THE ROUTED CHAINS' SLOWEST** — every chain in Parallel, Select and Blend (never the selection: a switch would move
	// the reported latency and restart the host, §3), the role-holders in Mid/Side and Left/Right. A chain the mode
	// gives no role is not heard, so it sets nothing.
	uint32_t latest = 0;
	const size_t roles = roleCount(mode_, live->branches.size(), bands_);
	for (size_t k = 0; k < live->branches.size() && k < roles; ++k) {
		latest = std::max(latest, live->branches[k]->chain.latencySamples(sampleRate));
	}
	return latest;
}

bool Rack::chainMuted(const Branch* b) const noexcept {
	return slotFieldsLive_ && b->slot < kChainSlots
	           ? slotFields_[b->slot * kSlotControls + static_cast<size_t>(SlotControl::Mute)] >= 0.5f
	           : b->mute.load(std::memory_order_relaxed);
}

bool Rack::chainSoloed(const Branch* b) const noexcept {
	// A ROUTED SOLO ORS WITH THE HAND'S (lead's call 2026-10-29): a route below the hand's solo cannot unsolo it.
	return b->solo.load(std::memory_order_relaxed)
	       || (slotFieldsLive_ && b->slot < kChainSlots
	           && slotFields_[b->slot * kSlotControls + static_cast<size_t>(SlotControl::Solo)] >= 0.5f);
}

//------------------------------------------------------------------------------------------------//
// NOTES THROUGH THE RACK — backlog RN, stage-2 doc §9                           //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE QUIETEST A PASSED NOTE-ON MAY BE: one MIDI step. Zero reads as an off to some hosts.
constexpr double kMinNoteVelocity = 1.0 / 127.0;

bool sameNote(int16_t channelA, int16_t keyA, int32_t idA, int16_t channelB, int16_t keyB, int32_t idB) noexcept {
	return channelA == channelB && keyA == keyB && (idA == idB || idA < 0 || idB < 0);
}

/// THE SAME EVENT FROM TWO SOURCES — a chain that passed its notes through, beside the dry: one note, not two.
bool sameEvent(const device::NoteEvent& a, const device::NoteEvent& b) noexcept {
	return a.kind == b.kind && a.timeSamples == b.timeSamples && a.key == b.key && a.channel == b.channel
	       && a.noteId == b.noteId && a.port == b.port && a.velocity == b.velocity && a.expressionId == b.expressionId
	       && a.expressionValue == b.expressionValue;
}

} // namespace

float Rack::noteFactor(const Lineup* live, size_t k, bool anySolo) const noexcept {
	if (!enabled_ || live == nullptr || k >= live->branches.size()) {
		return 0.0f; // a rack switched off hands its notes on dry, as it hands its audio on dry
	}
	const Branch* b = live->branches[k];
	if (chainMuted(b) || (anySolo && !chainSoloed(b))) {
		return 0.0f;
	}
	// SELECT AND BLEND WEIGH THE CHAINS BY POSITION; every other mode merges them, as Parallel does.
	const Mode mode = modeWanted_;
	const float w = mode == Mode::Select || mode == Mode::Blend
	                    ? positionWeight(mode, position_, k, live->branches.size())
	                    : 1.0f;
	return mix_ * w;
}

void Rack::stageNote(const device::NoteEvent& e, uint16_t source, float factor, bool invented) noexcept {
	if (stagedCount_ < staged_.size()) {
		staged_[stagedCount_++] = StagedNote { e, source, factor, invented, false };
	}
}

size_t Rack::processNotes(const device::NoteEvent* in, size_t count, device::NoteEvent* out, size_t capacity) noexcept {
	const Lineup* live = live_.load(std::memory_order_acquire);
	bool anyNotes = false;
	if (live != nullptr) {
		for (const Branch* b : live->branches) {
			anyNotes = anyNotes || b->chain.engine().holdsNoteDevice();
		}
	}
	// NO CHAIN TAKES NOTES AND NOTHING IS OWED: the rack is transparent to them, exactly as before it took any.
	if (staged_.empty() || (!anyNotes && recordCount_ == 0 && owedCount_ == 0 && !releaseAll_)) {
		const size_t n = std::min(count, capacity);
		std::copy_n(in, n, out);
		return n;
	}
	stagedCount_ = 0;
	const float dry = enabled_ ? 1.0f - mix_ : 1.0f;
	for (size_t i = 0; i < count; ++i) {
		stageNote(in[i], kDrySource, dry, false);
	}
	if (live != nullptr && count > 0) {
		bool anySolo = false;
		for (const Branch* b : live->branches) {
			anySolo = anySolo || chainSoloed(b);
		}
		for (size_t k = 0; k < live->branches.size(); ++k) {
			Branch* b = live->branches[k];
			const size_t made = b->chain.engine().processNotesNow(in, count, chainNotes_.data(), chainNotes_.size());
			const float f = noteFactor(live, k, anySolo);
			for (size_t i = 0; i < made; ++i) {
				stageNote(chainNotes_[i], b->id, f, false);
			}
		}
	}
	return deliverNotes(live, out, capacity);
}

size_t Rack::emitNotes(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
                       size_t capacity, uint32_t* until) noexcept {
	uint32_t soonest = std::numeric_limits<uint32_t>::max();
	const Lineup* live = live_.load(std::memory_order_acquire);
	stagedCount_ = 0;
	if (live != nullptr && !staged_.empty()) {
		bool anySolo = false;
		for (const Branch* b : live->branches) {
			anySolo = anySolo || chainSoloed(b);
		}
		for (size_t k = 0; k < live->branches.size(); ++k) {
			Branch* b = live->branches[k];
			uint32_t next = std::numeric_limits<uint32_t>::max();
			const size_t made =
			    b->chain.engine().emitNotesNow(clock, sampleNow, chainNotes_.data(), chainNotes_.size(), &next);
			soonest = std::min(soonest, next);
			// ITS LAST SELF-CLOCKED DEVICE LEFT: the notes they invented are ended for them, as `emitPiece` ends a chain's.
			const size_t emitters = b->chain.engine().emittersSeen();
			if (b->emitters > 0 && emitters == 0) {
				for (size_t r = 0; r < recordCount_; ++r) {
					if (noteRecords_[r].source == b->id && noteRecords_[r].invented) {
						noteRecords_[r].release = true;
					}
				}
			}
			b->emitters = emitters;
			const float f = noteFactor(live, k, anySolo);
			for (size_t i = 0; i < made; ++i) {
				stageNote(chainNotes_[i], b->id, f, true);
			}
		}
	}
	if (until != nullptr) {
		*until = soonest;
	}
	if (stagedCount_ == 0 && recordCount_ == 0 && owedCount_ == 0) {
		releaseAll_ = false;
		return 0;
	}
	return deliverNotes(live, out, capacity);
}

size_t Rack::deliverNotes(const Lineup* live, device::NoteEvent* out, size_t capacity) noexcept {
	size_t made = 0;
	const auto put = [&](const device::NoteEvent& e) {
		if (made < capacity && out != nullptr) {
			out[made++] = e;
			return true;
		}
		return false;
	};
	const auto owe = [&](const device::NoteEvent& e) {
		if (owedCount_ < owedOffs_.size()) {
			owedOffs_[owedCount_++] = e;
		}
	};
	const auto heard = [&](int16_t channel, int16_t key, int32_t id) {
		size_t n = 0;
		for (size_t r = 0; r < recordCount_; ++r) {
			const NoteRecord& rec = noteRecords_[r];
			n += rec.counts && sameNote(rec.channel, rec.key, rec.noteId, channel, key, id) ? 1 : 0;
		}
		return n;
	};
	const auto removeRecord = [&](size_t r) {
		for (size_t j = r; j + 1 < recordCount_; ++j) {
			noteRecords_[j] = noteRecords_[j + 1];
		}
		--recordCount_;
	};
	const auto offFor = [](const NoteRecord& rec) {
		device::NoteEvent off {};
		off.kind = device::NoteEvent::Kind::Off;
		off.timeSamples = 0;
		off.port = rec.port;
		off.channel = rec.channel;
		off.key = rec.key;
		off.noteId = rec.noteId;
		off.velocity = 0.0;
		return off;
	};

	// FIRST WHAT IS OWED: offs a full output could not take last call.
	size_t owed = owedCount_;
	owedCount_ = 0;
	for (size_t i = 0; i < owed; ++i) {
		if (!put(owedOffs_[i])) {
			owe(owedOffs_[i]);
		}
	}
	// THEN EVERY NOTE WHOSE SOURCE IS GONE — a chain removed, its emitter removed, the rack reset: ended now, so nothing
	// the rack let out is left sounding.
	for (size_t r = 0; r < recordCount_;) {
		NoteRecord& rec = noteRecords_[r];
		bool gone = releaseAll_ || rec.release;
		if (!gone && rec.source != kDrySource) {
			gone = true;
			if (live != nullptr) {
				for (const Branch* b : live->branches) {
					gone = gone && b->id != rec.source;
				}
			}
		}
		if (!gone) {
			++r;
			continue;
		}
		const NoteRecord ended = rec;
		removeRecord(r);
		if (ended.counts && !put(offFor(ended))) {
			owe(offFor(ended));
		}
	}
	releaseAll_ = false;

	// THE STAGED NOTES IN TIME ORDER — stable, so a source's own order at one sample stands (an off before the on it
	// makes room for).
	for (size_t i = 1; i < stagedCount_; ++i) {
		const StagedNote s = staged_[i];
		size_t j = i;
		while (j > 0 && staged_[j - 1].e.timeSamples > s.e.timeSamples) {
			staged_[j] = staged_[j - 1];
			--j;
		}
		staged_[j] = s;
	}
	for (size_t i = 0; i < stagedCount_; ++i) {
		if (staged_[i].done) {
			continue;
		}
		const device::NoteEvent e = staged_[i].e;
		// THE GROUP: this event, and the same event from every other source at this sample.
		float total = 0.0f;
		for (size_t j = i; j < stagedCount_ && staged_[j].e.timeSamples == e.timeSamples; ++j) {
			if (!staged_[j].done && sameEvent(staged_[j].e, e)) {
				total += std::max(staged_[j].factor, 0.0f);
			}
		}
		switch (e.kind) {
		case device::NoteEvent::Kind::On: {
			const size_t first = recordCount_;
			for (size_t j = i; j < stagedCount_ && staged_[j].e.timeSamples == e.timeSamples; ++j) {
				StagedNote& s = staged_[j];
				if (s.done || !sameEvent(s.e, e)) {
					continue;
				}
				s.done = true;
				if (recordCount_ < noteRecords_.size()) {
					noteRecords_[recordCount_++] = NoteRecord { s.source, e.port, e.channel, e.key, e.noteId,
						                                        s.factor > 0.0f, s.invented, false };
				}
			}
			// EVERY SOURCE'S NOTE IS ITS OWN (an Arp's step beside the played note it steps on); a source at 0 starts none.
			if (total > 0.0f) {
				device::NoteEvent on = e;
				on.velocity = std::max(e.velocity * std::min(1.0, static_cast<double>(total)), kMinNoteVelocity);
				if (!put(on)) {
					for (size_t r = first; r < recordCount_; ++r) {
						noteRecords_[r].counts = false; // not let out: its off is not owed either
					}
				}
			}
			break;
		}
		case device::NoteEvent::Kind::Off:
		case device::NoteEvent::Kind::Choke: {
			const bool choke = e.kind == device::NoteEvent::Kind::Choke;
			bool ended = false;
			bool known = false;
			for (size_t j = i; j < stagedCount_ && staged_[j].e.timeSamples == e.timeSamples; ++j) {
				StagedNote& s = staged_[j];
				if (s.done || !sameEvent(s.e, e)) {
					continue;
				}
				s.done = true;
				// ITS OWN NOTE ENDS — the oldest this source started, or every one on the key for a choke.
				for (size_t r = 0; r < recordCount_;) {
					const NoteRecord& rec = noteRecords_[r];
					const bool mine = rec.source == s.source
					                  && sameNote(rec.channel, rec.key, choke ? -1 : rec.noteId, e.channel, e.key,
					                              choke ? -1 : e.noteId);
					if (!mine) {
						++r;
						continue;
					}
					known = true;
					ended = ended || rec.counts;
					removeRecord(r);
					if (!choke) {
						break;
					}
				}
			}
			// LET OUT for a note that was heard, or one nobody knew (a note that began before the rack took notes); an off whose
			// on was swallowed is swallowed with it.
			if ((ended || !known) && !put(e)) {
				owe(e);
			}
			break;
		}
		case device::NoteEvent::Kind::Expression: {
			for (size_t j = i; j < stagedCount_ && staged_[j].e.timeSamples == e.timeSamples; ++j) {
				if (!staged_[j].done && sameEvent(staged_[j].e, e)) {
					staged_[j].done = true;
				}
			}
			if (e.key < 0 || heard(e.channel, e.key, e.noteId) > 0) {
				(void)put(e); // only on a note that is sounding
			}
			break;
		}
		}
	}
	stagedCount_ = 0;
	return made;
}

void Rack::processStereo(float* left, float* right, int frames) {
	// NOT HOSTED, THE BUS IS THE WHOLE BLOCK and the rack drains here.
	if (!hosted_) {
		adoptPending();
	}
	const Lineup* live = live_.load(std::memory_order_relaxed);
	if (live == nullptr || frames <= 0 || left == nullptr || right == nullptr || inL_.empty()) {
		return;
	}
	// EACH CHAIN'S GAIN: its fader, or nothing when muted or when another chain is soloed and this one is not. A chain
	// with a slot is heard through its slot params (backlog NO): the hand's values mirrored, plus whatever is routed there.
	const auto levelOf = [&](const Branch* b) {
		return slotFieldsLive_ && b->slot < kChainSlots
		           ? std::clamp(slotFields_[b->slot * kSlotControls + static_cast<size_t>(SlotControl::Level)], 0.0f, 1.0f)
		           : b->level.load(std::memory_order_relaxed);
	};
	const auto muteOf = [&](const Branch* b) { return chainMuted(b); };
	const auto soloOf = [&](const Branch* b) { return chainSoloed(b); };
	bool anySolo = false;
	for (Branch* b : live->branches) {
		anySolo = anySolo || soloOf(b);
	}
	for (Branch* b : live->branches) {
		const bool silent = muteOf(b) || (anySolo && !soloOf(b));
		const float target = silent ? 0.0f : faderGain(levelOf(b));
		if (!b->primed || !primed_) {
			b->gainNow = target;
			b->gainTarget = target;
			b->remaining = 0;
			b->primed = true;
		}
		else if (target != b->gainTarget) {
			b->gainTarget = target;
			b->gainStep = (target - b->gainNow) / static_cast<float>(rampSamples_);
			b->remaining = rampSamples_;
		}
	}
	// A PHANTOM ROLE IS AN EMPTY CHAIN AT UNITY: another chain's solo silences it, as it would an empty chain.
	for (Phantom& p : phantoms_) {
		const float target = anySolo ? 0.0f : 1.0f;
		if (!p.primed || !primed_) {
			p.gainNow = p.gainTarget = target;
			p.remaining = 0;
			p.primed = true;
		}
		else if (target != p.gainTarget) {
			p.gainTarget = target;
			p.gainStep = (target - p.gainNow) / static_cast<float>(rampSamples_);
			p.remaining = rampSamples_;
		}
	}
	const bool bypassed = !enabled_ && remaining_ == 0 && dryNow_ == 1.0f && wetNow_ == 0.0f;
	// **THE MODE** (stage 2, §2). A change fades the chains' sum to silence over 5 ms, switches what each chain is
	// handed and summed at, and fades back over 5 ms; the chains run on throughout, their state untouched. Where none
	// of the sum is heard — the first block, bypassed, Mix at 0 — there is nothing to fade and the mode lands at once.
	const bool dryChanges = dryKind(mode_, filtering_) != dryKind(modeWanted_, filteringWanted_);
	if (!primed_ || bypassed || (remaining_ == 0 && wetNow_ == 0.0f && !dryChanges)) {
		if (!structureIsWanted()) {
			takeWantedStructure();
		}
		dryFade_ = false;
		modeGain_ = 1.0f;
		modeStep_ = 0.0f;
		modeRemaining_ = 0;
		aimWeights(live, true);
	}
	else {
		if (!structureIsWanted() && (modeRemaining_ == 0 || modeStep_ > 0.0f)) {
			modeStep_ = -modeGain_ / static_cast<float>(rampSamples_); // down from wherever it stands
			modeRemaining_ = rampSamples_;
			// THE DRY FADES TOO WHERE ITS PATH CHANGES (into or out of Bands' allpass): the two cannot be crossfaded.
			dryFade_ = dryFade_ || dryChanges;
		}
		else if (structureIsWanted() && modeRemaining_ > 0 && modeStep_ < 0.0f) {
			modeStep_ = (1.0f - modeGain_) / static_cast<float>(rampSamples_); // asked back before the silence
			modeRemaining_ = rampSamples_;
		}
		aimWeights(live, false);
	}
	primed_ = true;

	const auto scratch = static_cast<int>(inL_.size());
	const size_t count = live->branches.size();
	int done = 0;
	while (done < frames) {
		// **A CHUNK ENDS WHERE A FADE REACHES SILENCE**, so a mode switches only between chunks: everything a chunk does
		// — what each chain is handed, which chains are routed, the latency they align to — is one mode's.
		int n = std::min(frames - done, scratch);
		const bool fadingOut = modeRemaining_ > 0 && modeStep_ < 0.0f;
		if (fadingOut) {
			n = std::min(n, modeRemaining_);
		}
		const auto bytes = static_cast<size_t>(n) * sizeof(float);
		float* l = left + done;
		float* r = right + done;
		std::memcpy(inL_.data(), l, bytes);
		std::memcpy(inR_.data(), r, bytes);
		std::memcpy(tapL_.data(), l, bytes);
		std::memcpy(tapR_.data(), r, bytes);
		std::fill_n(sumL_.begin(), n, 0.0f);
		std::fill_n(sumR_.begin(), n, 0.0f);
		const size_t roles = roleCount(mode_, count, bands_);
		// THE LATENCY THIS CHUNK ALIGNS TO, after the drain: the slowest routed chain's.
		const uint32_t latest = routedLatency(live, sampleRate_);
		if (mode_ == Mode::Bands) {
			// THE CORNERS ONCE PER SUB-BLOCK, recomputed only where they moved; then the input split into its bands
			// (Before, Both), which each role is handed below.
			aimCorners(n);
			if (filtering_ != Filtering::After) {
				splitBands(n);
			}
		}
		for (size_t k = 0; k < count; ++k) {
			Branch* b = live->branches[k];
			const bool routed = k < roles;
			if (!routed) {
				// **NOT ROUTED IN THIS MODE** (§2a): never heard, never in the latency or the tail. Stop: not run from here
				// on, its state kept. Ring out: fed silence until its own tail says it is exactly done, then not run.
				if (b->dormant) {
					continue;
				}
				if (unusedStop_) {
					b->dormant = true;
					b->stoppedHard = true;
					continue;
				}
				std::fill_n(workL_.begin(), n, 0.0f);
				std::fill_n(workR_.begin(), n, 0.0f);
				b->chain.engine().setSidechain(nullptr, nullptr, false);
				b->chain.engine().setCostLane("chain", static_cast<int>(k + 1));
				b->chain.process(workL_.data(), workR_.data(), n);
				++b->runs;
				bool silent = true;
				for (int i = 0; i < n && silent; ++i) {
					silent = workL_[static_cast<size_t>(i)] == 0.0f && workR_[static_cast<size_t>(i)] == 0.0f;
				}
				if (silent && !b->chain.holdsTail()) {
					b->dormant = true; // RANG OUT: exact zero, by the SLEEP rule's own definition of done
				}
				b->quiet = 0;
				continue;
			}
			if (b->dormant) {
				// BACK IN A ROLE. A chain Stop halted is reset first, so a frozen tail does not resume; one that rang out
				// is silent already. Either way its alignment held the last routed audio, which is stale now.
				if (b->stoppedHard) {
					b->chain.reset();
				}
				b->align.reset();
				b->dormant = false;
				b->stoppedHard = false;
				b->quiet = 0;
			}
			roleInput(k, n);
			b->chain.engine().setSidechain(sideL_ != nullptr ? sideL_ + done : nullptr,
			                               sideR_ != nullptr ? sideR_ + done : nullptr, sideSounding_);
			b->chain.engine().setCostLane("chain", static_cast<int>(k + 1)); // `RACK/chain2/DELA`, by its place now (backlog SR)
			b->chain.process(workL_.data(), workR_.data(), n);
			++b->runs;
			const uint32_t own = b->chain.latencySamples(sampleRate_);
			for (int i = 0; i < n; ++i) {
				const auto s = static_cast<size_t>(i);
				b->quiet = workL_[s] == 0.0f && workR_[s] == 0.0f ? (b->quiet < UINT32_MAX ? b->quiet + 1 : b->quiet) : 0;
			}
			b->align.process(workL_.data(), workR_.data(), n, latest - std::min(latest, own));
			if (mode_ == Mode::Bands) {
				bandOfOutput(k, n); // After and Both: the chain's output kept to its band
			}
			if (bypassed) {
				continue; // the chains still drain and publish; what they put out is not heard
			}
			for (int i = 0; i < n; ++i) {
				if (b->remaining > 0) {
					b->gainNow += b->gainStep;
					if (--b->remaining == 0) {
						b->gainNow = b->gainTarget;
					}
				}
				if (b->weightRemaining > 0) {
					b->weightNow += b->weightStep;
					if (--b->weightRemaining == 0) {
						b->weightNow = b->weightTarget;
					}
				}
				const auto s = static_cast<size_t>(i);
				const float g = b->gainNow * b->weightNow;
				const float heardL = g * workL_[s];
				const float heardR = g * workR_[s];
				sumL_[s] += heardL;
				sumR_[s] += heardR;
				// `a > peak` rather than `std::max`, so a NaN sample is skipped instead of carried (as `peakOf`).
				if (const float a = std::fabs(heardL); a > b->chunkPeak) {
					b->chunkPeak = a;
				}
				if (const float a = std::fabs(heardR); a > b->chunkPeak) {
					b->chunkPeak = a;
				}
			}
		}
		// **THE ROLES NO CHAIN FILLS** (§2a): each an empty chain — its share of the split, aligned, at unity.
		for (size_t role = count; role < roles && role < kMaxRoles; ++role) {
			Phantom& p = phantoms_[role];
			roleInput(role, n);
			for (int i = 0; i < n; ++i) {
				const auto s = static_cast<size_t>(i);
				p.quiet = workL_[s] == 0.0f && workR_[s] == 0.0f ? (p.quiet < UINT32_MAX ? p.quiet + 1 : p.quiet) : 0;
			}
			p.align.process(workL_.data(), workR_.data(), n, latest);
			if (mode_ == Mode::Bands) {
				bandOfOutput(role, n);
			}
			if (bypassed) {
				continue;
			}
			for (int i = 0; i < n; ++i) {
				if (p.remaining > 0) {
					p.gainNow += p.gainStep;
					if (--p.remaining == 0) {
						p.gainNow = p.gainTarget;
					}
				}
				const auto s = static_cast<size_t>(i);
				sumL_[s] += p.gainNow * workL_[s];
				sumR_[s] += p.gainNow * workR_[s];
			}
		}
		dryDelay_.process(inL_.data(), inR_.data(), n, latest);
		if (mode_ == Mode::Bands && !bypassed) {
			alignDryToBands(n); // §4d: the dry in the bands' phase, so a Mix does not cancel at the corners
		}
		if (bypassed) {
			std::memcpy(l, inL_.data(), bytes);
			std::memcpy(r, inR_.data(), bytes);
		}
		else if (remaining_ == 0 && wetNow_ == 0.0f && modeRemaining_ == 0) {
			// MIX 0 IS THE DRY, BIT FOR BIT — the sum is not read at all.
			for (int i = 0; i < n; ++i) {
				l[i] = dryNow_ * inL_[static_cast<size_t>(i)];
				r[i] = dryNow_ * inR_[static_cast<size_t>(i)];
			}
		}
		else {
			for (int i = 0; i < n; ++i) {
				if (remaining_ > 0) {
					wetNow_ += wetStep_;
					dryNow_ += dryStep_;
					if (--remaining_ == 0) {
						wetNow_ = wetTarget_;
						dryNow_ = dryTarget_;
					}
				}
				if (modeRemaining_ > 0) {
					modeGain_ += modeStep_;
					if (--modeRemaining_ == 0) {
						if (modeStep_ < 0.0f) {
							// SILENCE, on this chunk's last sample: the wanted mode takes over from the next chunk and fades in.
							modeGain_ = 0.0f;
							takeWantedStructure();
							modeStep_ = 1.0f / static_cast<float>(rampSamples_);
							modeRemaining_ = rampSamples_;
						}
						else {
							modeGain_ = 1.0f;
							modeStep_ = 0.0f;
							dryFade_ = false;
						}
					}
				}
				const auto s = static_cast<size_t>(i);
				const float wet = wetNow_ * modeGain_;
				const float dry = dryFade_ ? dryNow_ * modeGain_ : dryNow_;
				l[i] = wet * sumL_[s] + dry * inL_[s];
				r[i] = wet * sumR_[s] + dry * inR_[s];
			}
		}
		analyse(l, r, n);
		// A FADE THAT REACHED SILENCE AT THIS CHUNK'S END: the new mode's weights, at once, for the chunk after.
		if (fadingOut && modeStep_ > 0.0f) {
			aimWeights(live, true);
		}
		done += n;
	}
	if (mode_ == Mode::Bands) {
		// IIR STATE DECAYS TOWARD A DENORMAL, NOT TOWARD ZERO: flushed once a block, so the rack can sleep (§4b).
		if (slope_ == Slope::Lr4) {
			lr4Engine_.flush();
		}
		else {
			lr2Engine_.flush();
		}
	}
	readings_[static_cast<size_t>(Reading::NoteHz)] =
	    mode_ == Mode::Bands && keyTrack_ && heardNote_ ? static_cast<float>(keyHzOf(noteSemitones_)) : 0.0f;
	readingsLease_.fetch_sub(std::min<int64_t>(frames, readingsLease_.load(std::memory_order_relaxed)), std::memory_order_relaxed);
	if (sink_.publish != nullptr) {
		sink_.publish(sink_.context, readings_.data(), readings_.size());
	}
	advanceMeters(live, frames);
	publishExports(live);
}

void Rack::analyse(const float* left, const float* right, int n) noexcept {
	// ONLY WHERE THE STRIP IS DRAWN, AND ONLY WHILE A VIEW SHOWING IT HAS ASKED (§7a): a multiband rack on every bus of
	// a project must not pay for transforms nobody sees.
	if (spectrum_ == nullptr || (kSpectrumModes & kModeBit(mode_)) == 0
	    || readingsLease_.load(std::memory_order_relaxed) <= 0) {
		return;
	}
	dsp::Analyser<32>& analyser = spectrum_->analyser;
	for (int i = 0; i < n; ++i) {
		const auto s = static_cast<size_t>(i);
		analyser.push(tapL_[s], tapR_[s], left[i], right[i]);
		if (++gridPhase_ >= 32) {
			gridPhase_ = 0;
			analyser.step(); // at most one transform per grid step, the EQ's rule
		}
	}
	readings_[static_cast<size_t>(Reading::FrameA)] = analyser.frame();
	for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
		readings_[static_cast<size_t>(Reading::PreFirst) + k] = analyser.pre()[k];
		readings_[static_cast<size_t>(Reading::PostFirst) + k] = analyser.post()[k];
	}
	readings_[static_cast<size_t>(Reading::FrameB)] = analyser.frame();
}

void Rack::publishExports(const Lineup* live) noexcept {
	// A ROW'S CELL IS IN WHICHEVER CHAIN HOLDS ITS DEVICE (one counter for every chain, gap G3); a macro is the rack's,
	// the same in every chain, so the first chain's reads it.
	exports_.publish(
	    [](const void* ctx, backplane::Source s) -> const float* {
		    for (const Branch* b : static_cast<const Lineup*>(ctx)->branches) {
			    const RackEngine& e = b->chain.engine();
			    if (!s.isDevice() || e.slotOf(s.device) != RackEngine::kNoSlot) {
				    return e.sourceCell(s);
			    }
		    }
		    return nullptr;
	    },
	    live);
}

size_t Rack::exportRow(backplane::Source inner) {
	if (inner.isOuter()) {
		return ExportTable::kNoRow;
	}
	// A HOLDER IN ONE OF THE CHAINS, RE-EXPORTED ONE HOP OUT (NZ, any depth): only a row that holder actually has.
	if (inner.isExport()) {
		const InnerChain* chain = holding(inner.device);
		const RackBridge* b = chain != nullptr ? chain->bridge() : nullptr;
		return b != nullptr && b->namesLiveExport(inner) ? exports_.mint(inner) : ExportTable::kNoRow;
	}
	if (inner.isDevice()) {
		const InnerChain* chain = holding(inner.device);
		const RackBridge* b = chain != nullptr ? chain->bridge() : nullptr;
		const device::Descriptor* d = b != nullptr ? b->descriptorOf(inner.device) : nullptr;
		if (d == nullptr || d->mod == nullptr || inner.index >= d->outputCount()) {
			return ExportTable::kNoRow; // only a modulator inside reaches out
		}
	}
	else if (inner.index >= composite::kMacroCount) {
		return ExportTable::kNoRow;
	}
	return exports_.mint(inner);
}

void Rack::advanceMeters(const Lineup* live, int frames) noexcept {
	// ONE STEP OF EVERY CHAIN'S BALLISTICS PER CALL, `RackEngine::advanceOutputMeters`' rule: a peak at or above the held
	// one restarts the hold; past the hold it falls; a chain that gathered nothing (muted, bypassed) falls with the rest.
	const double rate = std::max(sampleRate_, 1.0);
	const auto holdSamples = static_cast<int64_t>(kMeterHoldSeconds * rate);
	const auto fall = static_cast<float>(std::pow(10.0, -kMeterFallDbPerSecond * (static_cast<double>(frames) / rate) / 20.0));
	for (Branch* b : live->branches) {
		const float chunk = b->chunkPeak;
		b->chunkPeak = 0.0f;
		if (chunk >= b->heldPeak) {
			b->heldPeak = chunk;
			b->holdLeft = holdSamples;
		}
		else if (b->holdLeft > 0) {
			b->holdLeft -= frames;
		}
		else {
			b->heldPeak *= fall;
			if (b->heldPeak < kMeterSilence) {
				b->heldPeak = 0.0f;
			}
		}
		b->peak.store(b->heldPeak, std::memory_order_relaxed);
	}
}

bool Rack::chainPeak(size_t index, float& peak) const noexcept {
	if (index >= chains_.size()) {
		return false;
	}
	const float held = chains_[index]->peak.load(std::memory_order_relaxed);
	if (held < 0.0f) {
		return false; // not run yet: no meter, not a silent one
	}
	peak = held;
	return true;
}

uint32_t Rack::latencySamples(double sampleRate) const noexcept {
	// THE SLOWEST ROUTED CHAIN'S — every routed chain, every phantom role and the dry wait for it. Asked of the live
	// lineup, which is what runs.
	const Lineup* live = live_.load(std::memory_order_acquire);
	return live != nullptr ? routedLatency(live, sampleRate) : 0u;
}

bool Rack::holdsTail() const noexcept {
	// **FROM THE SUM** (CLAUDE.md's host-facing rule): awake while any chain that is HEARD still holds sound to
	// come — inside it, or in the delay that aligns it. Bypassed and settled, only the dry passes.
	if (!enabled_ && remaining_ == 0 && dryNow_ == 1.0f && wetNow_ == 0.0f) {
		return false;
	}
	const Lineup* live = live_.load(std::memory_order_acquire);
	if (live == nullptr) {
		return false;
	}
	const uint32_t latest = routedLatency(live, sampleRate_);
	const size_t count = live->branches.size();
	const size_t roles = roleCount(mode_, count, bands_);
	const bool wetHeard = wetTarget_ > 0.0f || remaining_ > 0;
	for (size_t k = 0; k < count && k < roles; ++k) {
		const Branch* b = live->branches[k];
		// AND THE MODE'S WEIGHT (stage 2, §3): an unselected chain still runs and may still ring, but nothing of it
		// reaches the sum, so it does not keep the plugin awake. One still fading out does. A chain the mode gives no
		// role (§2a) is not in this loop at all, ringing out or not.
		const bool heard = wetHeard && (b->gainTarget > 0.0f || b->remaining > 0)
		                   && (b->weightTarget > 0.0f || b->weightRemaining > 0);
		if (!heard) {
			continue;
		}
		if (b->chain.holdsTail()) {
			return true;
		}
		const uint32_t own = b->chain.latencySamples(sampleRate_);
		if (latest > own && b->quiet < latest - own) {
			return true; // its alignment delay still holds some of its output
		}
	}
	// BANDS: A FILTER STILL RINGING is sound to come, in the wet or the dry — until `flush` has put it at exact zero.
	if (mode_ == Mode::Bands && (wetHeard || dryTarget_ > 0.0f)
	    && !(slope_ == Slope::Lr4 ? lr4Engine_.settled() : lr2Engine_.settled())) {
		return true;
	}
	// A PHANTOM ROLE'S ALIGNMENT holds its share of the input for the latency.
	for (size_t role = count; role < roles && role < kMaxRoles; ++role) {
		const Phantom& p = phantoms_[role];
		if (wetHeard && (p.gainTarget > 0.0f || p.remaining > 0) && latest > 0 && p.quiet < latest) {
			return true;
		}
	}
	return false;
}

float Rack::fieldForTest(Param p) const noexcept {
	switch (p) {
	case Param::Enabled: return enabled_ ? 1.0f : 0.0f;
	case Param::Mix: return mix_;
	case Param::Gain: return gain_;
	case Param::Mode: return static_cast<float>(modeWanted_);
	case Param::Position: return position_;
	case Param::Unused: return unusedStop_ ? 1.0f : 0.0f;
	case Param::BandCount: return static_cast<float>(bandsWanted_ == 2 ? 0 : (bandsWanted_ == 3 ? 1 : 2));
	case Param::Cut1:
	case Param::Cut2:
	case Param::Cut3:
	case Param::Cut4: return cutLog2_[static_cast<size_t>(p) - static_cast<size_t>(Param::Cut1)];
	case Param::Slope: return static_cast<float>(slopeWanted_);
	case Param::Filtering: return static_cast<float>(filteringWanted_);
	case Param::KeyTrack: return keyTrack_ ? 1.0f : 0.0f;
	default: break;
	}
	const auto n = static_cast<size_t>(p) - static_cast<size_t>(Param::Macro1);
	return n < composite::kMacroCount ? macros_[n] : 0.0f;
}

size_t Rack::liveChainsForTest() const noexcept {
	const Lineup* live = live_.load(std::memory_order_acquire);
	return live != nullptr ? live->branches.size() : 0;
}

float Rack::chainGainForTest(size_t index) const noexcept {
	const Lineup* live = live_.load(std::memory_order_acquire);
	return live != nullptr && index < live->branches.size() ? live->branches[index]->gainNow : 0.0f;
}

int Rack::transformsForTest() const noexcept { return spectrum_ != nullptr ? spectrum_->analyser.fftsForTest() : 0; }

uint64_t Rack::runsForTest(size_t index) const noexcept {
	const Lineup* live = live_.load(std::memory_order_acquire);
	return live != nullptr && index < live->branches.size() ? live->branches[index]->runs : 0u;
}

float Rack::chainWeightForTest(size_t index) const noexcept {
	const Lineup* live = live_.load(std::memory_order_acquire);
	return live != nullptr && index < live->branches.size() ? live->branches[index]->weightNow : 0.0f;
}

//------------------------------------------------------------------------------------------------//
// THE POSITION IN WORDS (stage 2, §3: the chain in the DAW)                                        //
//------------------------------------------------------------------------------------------------//

namespace {

constexpr const char* kArrow = "\xE2\x86\x92"; // →

const char* skipSpaces(const char* s) {
	while (*s == ' ' || *s == '\t') {
		++s;
	}
	return s;
}

/// An optional "chain" (any case) and the spaces after it.
const char* skipChainWord(const char* s) {
	s = skipSpaces(s);
	constexpr const char* kWord = "chain";
	size_t i = 0;
	while (kWord[i] != '\0' && std::tolower(static_cast<unsigned char>(s[i])) == kWord[i]) {
		++i;
	}
	return skipSpaces(kWord[i] == '\0' ? s + i : s);
}

/// A 1-based chain number, or 0 for none.
long readChain(const char*& s) {
	char* end = nullptr;
	const long k = std::strtol(s, &end, 10);
	if (end == s) {
		return 0;
	}
	s = end;
	return k;
}

/// The chain count a view's surface holds, or 0 where there is none.
size_t chainsOf(const device::TextView& view) noexcept {
	const auto* rack = static_cast<const Rack*>(view.surface);
	return rack != nullptr ? rack->chainCount() : 0;
}

Mode modeOf(const device::TextView& view) noexcept {
	const float m = view.field != nullptr ? view.field(view.host, static_cast<size_t>(Param::Mode)) : 0.0f;
	return std::isfinite(m) ? static_cast<Mode>(std::lround(std::clamp(m, 0.0f, static_cast<float>(Mode::kCount) - 1.0f)))
	                        : Mode::Parallel;
}

} // namespace

bool formatPosition(Mode mode, size_t chains, double position, char* out, size_t size) noexcept {
	if (out == nullptr || size == 0 || chains == 0 || (mode != Mode::Select && mode != Mode::Blend)) {
		return false;
	}
	if (mode == Mode::Select || chains == 1) {
		std::snprintf(out, size, "Chain %zu", selectedChain(position, chains) + 1);
		return true;
	}
	const double x = std::clamp(position, 0.0, 1.0) * static_cast<double>(chains - 1);
	const size_t lower = std::min(static_cast<size_t>(x), chains - 1);
	const long percent = std::lround((x - static_cast<double>(lower)) * 100.0);
	// AT A CHAIN, OR WITHIN HALF A PERCENT OF ONE, THE TEXT IS THAT CHAIN — "Chain 2 → 3, 0%" says nothing more.
	if (percent <= 0) {
		std::snprintf(out, size, "Chain %zu", lower + 1);
	}
	else if (percent >= 100) {
		std::snprintf(out, size, "Chain %zu", lower + 2);
	}
	else {
		std::snprintf(out, size, "Chain %zu %s %zu, %ld%%", lower + 1, kArrow, lower + 2, percent);
	}
	return true;
}

bool parsePosition(Mode mode, size_t chains, const char* text, double* position) noexcept {
	if (text == nullptr || position == nullptr || chains == 0 || (mode != Mode::Select && mode != Mode::Blend)) {
		return false;
	}
	const char* s = skipChainWord(text);
	const long a = readChain(s);
	if (a < 1 || static_cast<size_t>(a) > chains) {
		return false;
	}
	const double span = chains > 1 ? static_cast<double>(chains - 1) : 1.0;
	s = skipSpaces(s);
	if (*s == '\0') {
		*position = chains > 1 ? static_cast<double>(a - 1) / span : 0.0; // the chain's centre
		return true;
	}
	// "a → b, f%": b must be a's neighbour above it.
	if (std::strncmp(s, kArrow, std::strlen(kArrow)) == 0) {
		s += std::strlen(kArrow);
	}
	else if (s[0] == '-' && s[1] == '>') {
		s += 2;
	}
	else {
		return false;
	}
	s = skipChainWord(s);
	const long b = readChain(s);
	if (b != a + 1 || static_cast<size_t>(b) > chains) {
		return false;
	}
	s = skipSpaces(s);
	if (*s == ',') {
		s = skipSpaces(s + 1);
	}
	char* end = nullptr;
	const double percent = std::strtod(s, &end);
	if (end == s || !std::isfinite(percent) || percent < 0.0 || percent > 100.0) {
		return false;
	}
	s = skipSpaces(end);
	if (*s == '%') {
		s = skipSpaces(s + 1);
	}
	if (*s != '\0') {
		return false;
	}
	*position = (static_cast<double>(a - 1) + percent / 100.0) / span;
	return true;
}

namespace {

/// **WHETHER THE VIEW'S CORNERS ARE TRACKING A NOTE** — the published `NoteHz` reading, non-zero only then (the
/// Phaser's `phaserNoteTracked`). A view with no readings (the DAW's) is never tracking: there a Cut prints in Hz, as
/// the Phaser's Center does.
bool viewTracking(const device::TextView& view) noexcept {
	const auto i = static_cast<size_t>(Reading::NoteHz);
	return view.readingCount != nullptr && view.reading != nullptr && view.readingCount(view.host) > i
	       && view.reading(view.host, i) != 0.0f;
}

bool isCut(size_t control) noexcept {
	return control >= static_cast<size_t>(Param::Cut1) && control <= static_cast<size_t>(Param::Cut4);
}

} // namespace

bool formatCutOffset(double cutLog2, char* out, size_t size) noexcept {
	// `phaserOffsetText`'s form: a tenth of a semitone, signed, "0.0 st" within a twentieth of the note.
	const double raw = cutKeyOffsetSemitones(cutLog2);
	const double st = std::fabs(raw) < 0.05 ? 0.0 : raw;
	std::snprintf(out, size, st == 0.0 ? "%.1f st" : "%+.1f st", st);
	return true;
}

bool parseCutOffset(const char* text, double* cutLog2) noexcept {
	const char* s = skipSpaces(text);
	char* end = nullptr;
	const double st = std::strtod(s, &end);
	if (end == s || !std::isfinite(st)) {
		return false;
	}
	s = skipSpaces(end);
	if (std::strncmp(s, "st", 2) == 0) {
		s = skipSpaces(s + 2);
	}
	if (*s != '\0') {
		return false;
	}
	*cutLog2 = kCutKeyRefLog2 + st / 12.0;
	return true;
}

const device::TextOps kTextOps {
	.format = [](size_t control, double value, const device::TextView& view, char* out, size_t size) {
		if (isCut(control)) {
			// WHILE A NOTE IS TRACKED, A CUT READS AS ITS OFFSET FROM IT, in semitones.
			return viewTracking(view) && formatCutOffset(value, out, size);
		}
		return control == static_cast<size_t>(Param::Position) && formatPosition(modeOf(view), chainsOf(view), value, out, size);
	},
	.parse = [](size_t control, const char* text, const device::TextView& view, double* value) {
		if (isCut(control)) {
			return viewTracking(view) && parseCutOffset(text, value);
		}
		return control == static_cast<size_t>(Param::Position) && parsePosition(modeOf(view), chainsOf(view), text, value);
	},
};

//------------------------------------------------------------------------------------------------//
// THE TABLES                                                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

Rack& rackAt(void* storage) { return *static_cast<Rack*>(storage); }
const Rack& rackAt(const void* storage) { return *static_cast<const Rack*>(storage); }

} // namespace

const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Rack),
	.stateAlign = alignof(Rack),
	.construct = [](void* storage) { new (storage) Rack(); },
	.destruct = [](void* storage) { rackAt(storage).~Rack(); },
	.prepare = [](void* storage, double sampleRate, int maxFrames) { rackAt(storage).prepare(sampleRate, maxFrames); },
	.reset = [](void* storage) { rackAt(storage).reset(); },
	.setSlot = nullptr,
	.setFields = [](void* storage, const float* fields, size_t present) { rackAt(storage).setFields(fields, present); },
	.setNoteContext = [](void* storage, const device::NoteContext& note) { rackAt(storage).setNoteContext(note); },
	.drivePeak = nullptr,
	.setTransportClock =
	    [](void* storage, const device::TransportClock& clock) { rackAt(storage).setTransportClock(clock); },
	.processStereo = [](void* storage, float* left, float* right, int frames) {
		rackAt(storage).processStereo(left, right, frames);
	},
	.processMono = nullptr,
	.setReadingsSink = [](void* storage, device::ReadingsSink sink) { rackAt(storage).setReadingsSink(sink); },
	.setOuterSources =
	    [](void* storage, const device::OuterSources& outer) { rackAt(storage).setOuterSources(outer); },
	.beginPerVoiceBlock = [](void* storage) { return rackAt(storage).beginPerVoiceBlock(); },
	.processVoice = [](void* storage, size_t voice, float* left, float* right, int frames,
	                   const device::NoteContext& note) { rackAt(storage).processVoice(voice, left, right, frames, note); },
	.latencySamples = [](const void* storage, const float*, size_t, double sampleRate) {
		return rackAt(storage).latencySamples(sampleRate);
	},
	.holdsTail = [](const void* storage) { return rackAt(storage).holdsTail(); },
	.setSidechain = [](void* storage, const float* left, const float* right, int, bool sounding) {
		rackAt(storage).setSidechain(left, right, sounding);
	},
};

/// **THE NOTE ROAD** (backlog RN). The storage is the audio road's (a device declaring several capabilities shares one,
/// built by `AudioOps`), so these three are never the ones that build it.
const device::NoteOps kNoteOps {
	.stateBytes = sizeof(Rack),
	.stateAlign = alignof(Rack),
	.construct = [](void* storage) { new (storage) Rack(); },
	.destruct = [](void* storage) { rackAt(storage).~Rack(); },
	.prepare = [](void*, double) {},
	.setFields = [](void* storage, const float* fields, size_t present) { rackAt(storage).setFields(fields, present); },
	.processNotes = [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	                   size_t* outCount) {
		const size_t n = rackAt(storage).processNotes(in, inCount, out, outCapacity);
		if (outCount != nullptr) {
			*outCount = n;
		}
	},
	.busNote = nullptr,
	.emitNotes = [](void* storage, const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
	                size_t outCapacity, uint32_t* samplesUntilNext) {
		return rackAt(storage).emitNotes(clock, sampleNow, out, outCapacity, samplesUntilNext);
	},
};

const device::ContainerOps kContainerOps {
	.adopt = [](void* storage, const device::Catalogue& catalogue) { rackAt(storage).adopt(catalogue); },
	.saveState = [](const void* storage, const device::ByteSink& sink) { return rackAt(storage).saveState(sink); },
	.loadState = [](void* storage, const uint8_t* data, size_t bytes) { return rackAt(storage).loadState(data, bytes); },
	.maxBodyBytes = kMaxRackBodyBytes,
	.setOuterGate = [](void* storage, const device::OuterGate& gate) { rackAt(storage).setOuterGate(gate); },
	.setHostSlots = [](void* storage, const device::HostSlotLink& link) { rackAt(storage).setHostSlots(link); },
	.service = [](void* storage) { return rackAt(storage).service(); },
	.setVoices = [](void* storage, size_t voices) { rackAt(storage).setVoices(voices); },
	.forEachRouteFrom =
	    [](const void* storage, device::Source outer, void* ctx, void (*fn)(void* ctx, device::Destination inner, float amount)) {
		    const Rack& rack = rackAt(storage);
		    for (size_t i = 0; i < rack.chainCount(); ++i) {
			    rack.chainAt(i)->forEachRouteFrom(outer, ctx, fn);
		    }
	    },
	.routeAmountFrom =
	    [](const void* storage, device::Source outer, device::Destination inner) {
		    const InnerChain* c = rackAt(storage).holding(inner.device);
		    return c != nullptr ? c->routeAmountFrom(outer, inner) : 0.0f;
	    },
	.setRouteFrom =
	    [](void* storage, device::Source outer, device::Destination inner, float amount) {
		    InnerChain* c = rackAt(storage).holding(inner.device);
		    return c != nullptr && c->setRouteFrom(outer, inner, amount);
	    },
	.routeTransferFrom =
	    [](const void* storage, device::Source outer, device::Destination inner) {
		    const InnerChain* c = rackAt(storage).holding(inner.device);
		    return c != nullptr ? c->routeTransferFrom(outer, inner) : device::Transfer::Lin;
	    },
	.setRouteTransferFrom =
	    [](void* storage, device::Source outer, device::Destination inner, device::Transfer t) {
		    InnerChain* c = rackAt(storage).holding(inner.device);
		    return c != nullptr && c->setRouteTransferFrom(outer, inner, t);
	    },
	.nameDestination =
	    [](const void* storage, device::Destination inner, char* out, size_t capacity) {
		    const InnerChain* c = rackAt(storage).holding(inner.device);
		    return c != nullptr ? c->nameDestination(inner, out, capacity) : size_t { 0 };
	    },
	.applyState = [](void* storage, const uint8_t* data, size_t bytes) { return rackAt(storage).applyState(data, bytes); },
	.exportRow = [](void* storage, backplane::Source inner) { return rackAt(storage).exportRow(inner); },
	.exportedSource = [](const void* storage, size_t row) { return rackAt(storage).exports().source(row); },
	.exportCell = [](const void* storage, size_t row) { return rackAt(storage).exports().cell(row); },
	.sweepExports = [](void* storage, const void* usedRows, uint32_t published, uint32_t drained) {
		const auto& used = *static_cast<const std::vector<bool>*>(usedRows);
		return rackAt(storage).exportsMutable().sweep([&](size_t r) { return r < used.size() && used[r]; }, published, drained);
	},
	.releasedControls = [](const void* storage, size_t* out, size_t capacity) {
		return rackAt(storage).releasedControls(out, capacity);
	},
	.controlsReleased = [](void* storage) { rackAt(storage).controlsReleased(); },
	.serviceMirror = [](void* storage) { return rackAt(storage).serviceMirror(); },
	.adoptOwnFields = [](void* storage, const float* fields, size_t present) {
		return rackAt(storage).adoptOwnFields(fields, present);
	},
};

std::vector<uint8_t> bodyOfOneChain(std::span<const uint8_t> chain, const std::vector<RackPatch::Export>& exports) {
	std::vector<uint8_t> out;
	// RESERVED, which also quiets g++-14's -Wstringop-overflow false positive on the first small inserts.
	out.reserve(64 + chain.size());
	const device::ByteSink sink = vectorSink(&out);
	put(sink, kBodyMagic);
	put(sink, kBodyVersion);
	put(sink, static_cast<uint32_t>(1));
	put(sink, static_cast<uint16_t>(0));
	const struct {
		const char* key;
		float value;
	} fields[] = {
		{ kLevelKey, static_cast<float>(composite::kFaderUnity) },
		{ kMuteKey, 0.0f },
		{ kSoloKey, 0.0f },
		{ kOpenKey, 1.0f },
	};
	put(sink, static_cast<uint8_t>(std::size(fields)));
	for (const auto& f : fields) {
		const auto len = static_cast<uint8_t>(std::strlen(f.key));
		put(sink, len);
		sink(reinterpret_cast<const uint8_t*>(f.key), len);
		put(sink, f.value);
	}
	put(sink, static_cast<uint32_t>(chain.size()));
	sink(chain.data(), chain.size());
	putExports(sink, exports);
	return out;
}

bool mapRackBody(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out, size_t* droppedExports) {
	Reader in { bytes.data(), bytes.size(), 0 };
	uint32_t magic = 0;
	uint16_t version = 0;
	uint32_t count = 0;
	if (!in.get(magic) || magic != kBodyMagic || !in.get(version) || (version != kBodyVersion && version != 2) || !in.get(count)) {
		return false;
	}
	std::vector<uint8_t> next;
	const device::ByteSink sink = vectorSink(&next);
	put(sink, magic);
	put(sink, kBodyVersion); // rewritten in today's format, whichever it was read in
	put(sink, count);
	for (uint32_t c = 0; c < count; ++c) {
		// THE CHAIN'S HEAD — its id and its own controls — copied as it stands: nothing in it names a device.
		const size_t headAt = in.at;
		uint16_t id = 0;
		uint8_t fields = 0;
		if (!in.get(id) || !in.get(fields)) {
			return false;
		}
		for (uint8_t f = 0; f < fields; ++f) {
			uint8_t len = 0;
			float value = 0.0f;
			if (!in.get(len) || !in.skip(len) || !in.get(value)) {
				return false;
			}
		}
		sink(bytes.data() + headAt, in.at - headAt);
		uint32_t bodyBytes = 0;
		if (!in.get(bodyBytes) || bodyBytes > kMaxBodyBytes || in.size - in.at < bodyBytes) {
			return false;
		}
		std::vector<uint8_t> chain;
		if (bodyBytes > 0 && !map(ctx, bytes.subspan(in.at, bodyBytes), &chain)) {
			return false;
		}
		(void)in.skip(bodyBytes);
		put(sink, static_cast<uint32_t>(chain.size()));
		sink(chain.data(), chain.size());
	}
	// THE EXPORT TABLE names ids inside the chains, which nothing here changes: copied as it stands — or, for a preset, none.
	std::vector<RackPatch::Export> exports;
	if (!getExports(in, &exports, version)) {
		return false;
	}
	if (droppedExports != nullptr) {
		*droppedExports += exports.size();
		exports.clear();
	}
	putExports(sink, exports);
	if (in.at != bytes.size()) {
		return false;
	}
	*out = std::move(next);
	return true;
}

} // namespace feathers::rack::parallel
