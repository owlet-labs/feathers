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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "fx/automod/automod.h"
#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "fx/filter/filter_types.h"
#include "fx/filter/multimode_filter.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/tracker/tracker.h"
#include "mod/tracker/tracker_types.h"
#include "plugin/device_registry.h"
#include "plugin/fx_chain.h"
#include "plugin/lane_rack.h"
#include "plugin/mod_rack.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// FEATHERS' FX LANE ON THE RACK ENGINE — every claim `FxRunner`'s suite made about the lane, asked of
// the lane that replaced it, plus the ones the rack's shape makes possible.
//
// WHAT CHANGED UNDER THE CLAIMS, SAID ONCE HERE SO NO CASE HAS TO APOLOGISE FOR IT:
//   - `FxRunner` was keyed by POSITION and rebuilt a device whenever a different one arrived there, so
//     a reorder rebuilt. The rack keys by IDENTITY and a reorder MOVES — a device keeps its instance and
//     its memory wherever it is dragged. The reorder cases assert that now, as values.
//   - One engine runs the bus AND the per-voice arrangement; there are no two runners to divide the
//     chain between, only each device's own switch deciding where it runs.
//   - `setSlot` tells a device the ENGINE'S OWN SLOT for it (kill-lanes step 3a: the engine chooses where
//     it keeps a device, and nothing outside the engine may name that number), and the meter connector is
//     still told its POSITION in the chain (the bridge's meter rows are keyed on it).
//   - **A PLACE IN A CHAIN IS `(chain, position)`, NOT A LANE** (kill-lanes step 3b). `deviceFx` and
//     `deviceStrip` below are the only two roads from a place to a device, and every per-device call on
//     `LaneRack` takes what they answer.

//------------------------------------------------------------------------------------------------//
// THE INSTRUMENT — a counter on every global allocation, armed only where a claim is being made.   //
//------------------------------------------------------------------------------------------------//

namespace {

struct Counter {
	static inline bool armed = false;
	static inline size_t allocations = 0;
	static inline size_t frees = 0;
	/// THE OVER-ALIGNED FORM — device storage, which is what a chain's cost is made of.
	static inline size_t alignedBytes = 0;
	/// EVERY FORM, IN BYTES — the whole heap a claim asks for (backlog NS).
	static inline size_t bytes = 0;
	static void arm() {
		allocations = 0;
		frees = 0;
		alignedBytes = 0;
		bytes = 0;
		armed = true;
	}
	static void disarm() { armed = false; }
};

struct Armed {
	Armed() { Counter::arm(); }
	~Armed() { Counter::disarm(); }
};

} // namespace

// THE FALSE POSITIVE, SILENCED FOR THE REPLACED ALLOCATORS ONLY — the same push/ignore test_rack_engine.cpp
// uses. Freeing directly (below) was enough for g++-14 locally; GitHub's Linux runner is GCC 13.3, which
// still inlines a sized delete into a std::vector destructor, sees `free` after `operator new`, and raises
// -Werror=mismatched-new-delete (CI run 35156338285). The pairing is correct by construction: every
// `new` here is malloc/aligned_alloc and every `delete` is free.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(size_t size) {
	if (Counter::armed) {
		++Counter::allocations;
		Counter::bytes += size;
	}
	void* p = feathers::test::heapAlloc(size == 0 ? 1 : size);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size) { return ::operator new(size); }
void* operator new(size_t size, std::align_val_t align) {
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h (macOS CI run 35172215178).
	if (Counter::armed) {
		++Counter::allocations;
		Counter::bytes += size;
		Counter::alignedBytes += size;
	}
	void* p = feathers::test::portableAlignedAlloc(size, static_cast<size_t>(align));
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size, std::align_val_t align) { return ::operator new(size, align); }
// EVERY FORM FREES DIRECTLY rather than forwarding to another `operator delete`: g++-14 -O3 inlines a
// forwarded sized delete into doctest's own context and raises a false -Werror=mismatched-new-delete
// against it — the false positive `test_fx_devices.cpp`'s `Raw` records, which moves as a file grows.
namespace {
void countedFree(void* p) noexcept {
	if (p != nullptr && Counter::armed) {
		++Counter::frees;
	}
	feathers::test::heapFree(p);
}
} // namespace
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

namespace {

using namespace feathers;

constexpr double kSampleRate = 48000.0;
constexpr size_t kVoices = 8;
constexpr int kFrames = 64;
/// FEATHERS' CONTROL TICK — the samples the strip is owed per tick (`PuckMotion::kTickSamples`).
constexpr int kTickFrames = 64;

/// WHAT A std::vector OF `bytes` ASKS operator new FOR. libstdc++ and libc++ ask for exactly the bytes. MSVC's
/// std::allocator pads a request of 4096 bytes or more by `sizeof(void*) + 31` (39 on x64) to align it to 32 for
/// vector units (`_Big_allocation_threshold`, `_Non_user_size` in <xmemory>, release builds), MEASURED on the first
/// Windows run as two 6279-byte asks for two 6240-byte tick-window vectors, 78 bytes over.
constexpr size_t vectorHeapBytes(size_t bytes) {
#if defined(_MSC_VER) && !defined(_DEBUG)
	return bytes >= 4096 ? bytes + sizeof(void*) + 31 : bytes;
#else
	return bytes;
#endif
}
const device::NoteContext kNote { 60.0f, true };

std::vector<const device::Descriptor*> audioDevices() {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		if (d.audio != nullptr) {
			out.push_back(&d);
		}
	}
	return out;
}

std::string deviceName(const device::Descriptor& desc) {
	return desc.traits.name == nullptr ? std::string("<unnamed>") : std::string(desc.traits.name);
}

/// A CHAIN OF `count` DEVICES, cycling through the registry from `first`.
///
/// **`device[slot]` IS THE DEVICE DRAWN AT `slot` (kill-lanes step 3c)**, which is what this loop
/// always meant: it wrote positions 0..count and then called `assignDenseFxLanes` to hand each row the
/// routing address it had none of. The array is the order now, so writing the row at an index puts the
/// device there, and there is no address left to hand out.
FxChain chainOf(size_t count, size_t first = 0) {
	const std::vector<const device::Descriptor*> devices = audioDevices();
	FxChain chain;
	for (size_t slot = 0; slot < count && slot < kMaxFxSlots; ++slot) {
		const device::Descriptor* desc = devices[(first + slot) % devices.size()];
		chain.device[slot].row = defaultRowFor(*desc, { chain.nextInstanceId });
		chain.nextInstanceId = chain.nextInstanceId + 1;
		chain.count = slot + 1;
	}
	return chain;
}

/// A ONE-DEVICE CHAIN HOLDING `row`.
FxChain chainWith(const DeviceRow& row) {
	FxChain chain;
	chain.device[0].row = row;
	chain.count = 1;
	return chain;
}

std::vector<float> probeSignal(size_t n, float amplitude, uint32_t seed) {
	std::vector<float> out(n, 0.0f);
	uint32_t s = seed;
	for (size_t i = 0; i < n; ++i) {
		s = s * 1664525u + 1013904223u;
		const float noise = static_cast<float>(static_cast<int32_t>(s >> 8) % 2001 - 1000) / 1000.0f;
		out[i] = amplitude
		         * (0.6f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.011
		                                               * static_cast<double>(i)))
		            + 0.4f * noise);
	}
	return out;
}

DeviceRow enabledRowFor(const device::Descriptor& desc, DeviceInstanceId id = 1) {
	DeviceRow row = defaultRowFor(desc, { id });
	const size_t at = device::enableControlIn(desc.traits, desc.params);
	REQUIRE_MESSAGE(at != device::kNoControl, "no enable control declared by ", deviceName(desc));
	row.field[at] = static_cast<float>(desc.params[at].max);
	return row;
}

/// EVERY CONTINUOUS CONTROL SOMEWHERE ELSE — capped in its own units, for `test_fx_devices.cpp`'s
/// measured reason (an unbounded counter's range is a drag window, not a domain).
DeviceRow variedRowFor(const device::Descriptor& desc, DeviceInstanceId id = 1) {
	DeviceRow row = enabledRowFor(desc, id);
	for (size_t i = 0; i < desc.params.size() && i < row.fieldsPresent; ++i) {
		const device::ParamDesc& p = desc.params[i];
		if (p.stepped) {
			continue;
		}
		constexpr double kSweepExcursion = 8.0;
		const double away = p.def + std::min(0.6 * (p.max - p.def), kSweepExcursion);
		const double back = p.def - std::min(0.6 * (p.def - p.min), kSweepExcursion);
		row.field[i] = static_cast<float>(away == p.def ? back : away);
	}
	return row;
}

/// EVERY `sync` GOES THROUGH HERE — "sync allocates nothing and frees nothing" is a property of the
/// function, so it is asserted at the call rather than in one case. Paid for on the retired
/// `FxRunner`: its second retire site (an arrival nobody wanted) was mutated to destruct on the audio
/// thread and the whole repository stayed green, because the only case reaching that branch was not
/// one of the few that armed the counter. Guarding the call covers whatever road a sync grows.
void syncGuarded(LaneRack& lane, const FxChain& chain,
                 const device::TransportClock& clock = device::TransportClock::unknown()) {
	{
		const Armed armed;
		lane.sync(chain, kNote, clock);
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
}

/// ONE RENDER: every voice through the voice pass, then the bus. Guarded, for the same reason.
void renderGuarded(LaneRack& lane, uint32_t seed) {
	std::array<float, kFrames> left {};
	std::array<float, kFrames> right {};
	const std::vector<float> signal = probeSignal(static_cast<size_t>(kFrames), 0.5f, seed);
	{
		const Armed armed;
		for (size_t voice = 0; voice < kVoices; ++voice) {
			std::copy(signal.begin(), signal.end(), left.begin());
			std::copy(signal.begin(), signal.end(), right.begin());
			lane.processVoice(voice, left.data(), right.data(), kFrames,
			                  device::NoteContext { 48.0f + static_cast<float>(voice), true });
		}
		std::copy(signal.begin(), signal.end(), left.begin());
		std::copy(signal.begin(), signal.end(), right.begin());
		lane.processBus(left.data(), right.data(), kFrames);
		lane.finish();
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
}

/// **WHICH DEVICE THE CHAINS NAME AT A POSITION** — every per-device road on `LaneRack` takes the device
/// (kill-lanes step 3a), because the engine chooses its own slot and a slot and a record lane are both a
/// `size_t`. The state the engine converged to is what names each device, whichever road it came from.
///
/// **A PLACE IN A CHAIN IS (chain, position) NOW, AND NOTHING ELSE** (kill-lanes step 3b). These took the
/// view's lane and turned it into a state index; the state is each chain's ordered run, so the view's own
/// position IS the state's position (`feathers_chains.h`'s `stateOfChains`) and there is nothing left to
/// translate. That is why neither takes the view any more: passing one would suggest the answer depended
/// on it.
DeviceInstanceId deviceFx(const LaneRack& lane, size_t position) {
	return lane.deviceOfStatePosition(kLaneChain, position);
}

DeviceInstanceId deviceStrip(const LaneRack& lane, size_t slot) {
	return lane.deviceOfStatePosition(kStripChain, slot);
}

// (`LaneRack::slotOfStatePosition` answers where the ENGINE keeps the device standing at a place, for the
// one reader that needs a slot — `ParamSet`'s coordinate space. Only the routing case below does.)

/// THE ENGINE'S ORDER, AS THE SLOTS IT RUNS — compared against what the chains say through
/// `deviceFx`/`deviceStrip`, so neither side has to name a device itself.
std::vector<DeviceInstanceId> engineOrder(const LaneRack& lane) {
	std::vector<DeviceInstanceId> out;
	const rack::RackEngine& e = lane.engineForTest();
	for (size_t at = 0; at < e.chainLength(); ++at) {
		out.push_back(lane.deviceAt(e.deviceAtPosition(at)));
	}
	return out;
}

std::vector<DeviceInstanceId> chainOrder(const LaneRack& lane, const FxChain& chain) {
	std::vector<DeviceInstanceId> out;
	for (size_t at = 0; at < fxChainLength(chain); ++at) {
		out.push_back(deviceFx(lane, at));
	}
	return out;
}

FxChain movedChain(const FxChain& in, size_t from, size_t to) {
	FxChain out = in;
	REQUIRE(moveFxDevice(&out, from, to));
	return out;
}

/// ONE DEVICE, ALONE, DRIVEN IN THE ORDER THE RACK'S BUS PASS DRIVES IT — the independent restatement
/// the reorder case compares against. Fields (the whole declared row, absent controls at their
/// defaults), the clock, the note, then audio: `RackEngine::processBus`'s sequence, written out again
/// rather than borrowed from it.
struct Solo {
	const device::Descriptor* desc = nullptr;
	const device::AudioOps* ops = nullptr;
	void* mem = nullptr;
	std::vector<float> fields;
	/// EVERY SAMPLE THIS REFERENCE WAS FED — the history a declared latency reads its dry from.
	std::vector<float> fedL, fedR;

	Solo(const device::Descriptor& d, const DeviceRow& row) : desc(&d), ops(d.audio) {
		const size_t align = std::max<size_t>(ops->stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((ops->stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		ops->construct(mem);
		ops->prepare(mem, kSampleRate, 4096);
		if (d.container != nullptr && d.container->adopt != nullptr) {
			d.container->adopt(mem, hostCatalogue());
		}
		const size_t width = d.params.size() + d.traits.extraState;
		fields.assign(width, 0.0f);
		for (size_t c = 0; c < width; ++c) {
			const float fallback = c < d.params.size() ? static_cast<float>(d.params[c].def) : 0.0f;
			fields[c] = c < row.fieldsPresent ? row.field[c] : fallback;
		}
	}
	~Solo() {
		ops->destruct(mem);
		feathers::test::heapFree(mem);
	}
	Solo(const Solo&) = delete;
	Solo& operator=(const Solo&) = delete;

	void process(float* l, float* r, int n) {
		ops->setFields(mem, fields.data(), fields.size());
		if (ops->setTransportClock != nullptr) {
			ops->setTransportClock(mem, device::TransportClock::unknown());
		}
		if (ops->setNoteContext != nullptr) {
			ops->setNoteContext(mem, kNote);
		}
		// **AND THE SLOT'S FURNITURE, which the lane applies around a device whose Mix is the slot's** (backlog NR): the
		// device alone hands back its wet, so the reference blends it as the slot does — at settings that do not move,
		// the ramp is already where it is aimed.
		const int mixAt = desc->traits.mixControl;
		const bool blends = !desc->traits.mixInDevice && mixAt >= 0 && static_cast<size_t>(mixAt) < fields.size();
		// THE DRY LATE BY THE DEVICE'S DECLARED LATENCY, as the slot delays it (CLIP with 4x: six samples), read from
		// everything this reference has been fed so it is continuous across calls.
		const uint32_t late = blends && ops->latencySamples != nullptr
		                          ? ops->latencySamples(mem, fields.data(), fields.size(), kSampleRate)
		                          : 0u;
		fedL.insert(fedL.end(), l, l + n);
		fedR.insert(fedR.end(), r, r + n);
		std::vector<float> dryL, dryR;
		if (blends) {
			for (int i = 0; i < n; ++i) {
				const size_t at = fedL.size() - static_cast<size_t>(n) + static_cast<size_t>(i);
				dryL.push_back(at >= late ? fedL[at - late] : 0.0f);
				dryR.push_back(at >= late ? fedR[at - late] : 0.0f);
			}
		}
		ops->processStereo(mem, l, r, n);
		if (blends) {
			const float mix = std::clamp(fields[static_cast<size_t>(mixAt)], 0.0f, 1.0f);
			const int gainAt = desc->traits.wetGainControl;
			const float p = gainAt >= 0 ? std::clamp(fields[static_cast<size_t>(gainAt)], 0.0f, 1.0f) : 0.5f;
			const float wet = mix * ((2.0f * p) * (2.0f * p));
			const float dry = 1.0f - mix;
			if (wet != 1.0f || dry != 0.0f) {
				for (int i = 0; i < n; ++i) {
					l[i] = l[i] * wet + dryL[static_cast<size_t>(i)] * dry;
					r[i] = r[i] * wet + dryR[static_cast<size_t>(i)] * dry;
				}
			}
		}
	}
};

} // namespace

//==================================================================================================
// ADOPTION, AND WHAT IT MAY NOT DO ON THE AUDIO THREAD
//==================================================================================================

TEST_CASE("sync adopts what provision built, and asks for no memory to do it") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);

	const FxChain first = chainOf(kMaxFxSlots, 0);
	CHECK(lane.provision(first) == kMaxFxSlots);
	CHECK(lane.pendingCountForTest() == kMaxFxSlots);
	syncGuarded(lane, first);
	CHECK(lane.pendingCountForTest() == 0);
	// AND IT REALLY SEATED THEM — otherwise the zeros are the zeros of a function that did nothing. The
	// chain is dense, so the device at position `p` is the row `chainOf` put at slot `p`.
	for (size_t p = 0; p < kMaxFxSlots; ++p) {
		CAPTURE(p);
		CHECK(lane.descriptorOfForTest(deviceFx(lane, p)) == descriptorForRow(first.device[p].row));
	}
	CHECK(engineOrder(lane) == chainOrder(lane, first));

	// EVERY POSITION'S DEVICE CHANGED AT ONCE — eight removals and eight adoptions in one drain.
	const FxChain second = chainOf(kMaxFxSlots, 1);
	CHECK(lane.provision(second) == kMaxFxSlots);
	syncGuarded(lane, second);
	for (size_t p = 0; p < kMaxFxSlots; ++p) {
		CAPTURE(p);
		CHECK(lane.descriptorOfForTest(deviceFx(lane, p)) == descriptorForRow(second.device[p].row));
	}
	lane.finish();
	lane.reap();
}

TEST_CASE("a lane costs the devices it holds — and a per-voice device one instance more per voice while its Poly is on") {
	// THE MEMORY CLAIM AS A VALUE, for every audio device: the bytes of device storage asked for while
	// provisioning one device are its own `stateBytes`, times one for the bus and one more per voice if
	// it declares a per-voice road AND its row has the switch on (backlog NS: a device whose Poly is off
	// builds its voices only when the switch goes on). Restated from the declarations, never read back off
	// the lane.
	for (const device::Descriptor* desc : audioDevices()) {
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		LaneRack lane;
		lane.prepare(kSampleRate, kVoices, 4096);
		const FxChain chain = chainWith(defaultRowFor(*desc, { 1 }));
		size_t built = 0;
		size_t asked = 0;
		{
			const Armed armed;
			built = lane.provision(chain);
			asked = Counter::alignedBytes;
		}
		CHECK(built == 1);
		const int poly = desc->traits.perVoiceControl;
		const bool on = poly >= 0 && desc->params[static_cast<size_t>(poly)].def >= 0.5;
		const size_t instances = 1 + (on ? kVoices : 0);
		CHECK(asked == desc->audio->stateBytes * instances);
		syncGuarded(lane, chain);
		lane.finish();
	}
}

TEST_CASE("backlog NS: Poly off costs a per-voice device NO voice instances anywhere on the heap, and switching it "
          "on costs exactly what the instances always cost") {
	// THE WHOLE HEAP, not only device storage: a device's own buffers come from its `prepare` (Spectral's frames,
	// the filters' tables), and those are what the eager road paid 20 times over. Three numbers per device, all
	// measured on this run's lanes: Poly off in a lane WITH voices against the same device in a lane with NONE (the
	// drop — equal, to the byte), and the cost of the switch going on against the eager road's surplus (equal, but for
	// the one post that carries the instances to the drain).
	int swept = 0;
	size_t postBytes = 0;
	for (const device::Descriptor* desc : audioDevices()) {
		if (desc->traits.perVoiceControl < 0) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		++swept;
		const auto poly = static_cast<size_t>(desc->traits.perVoiceControl);
		DeviceRow offRow = defaultRowFor(*desc, { 1 });
		offRow.field[poly] = 0.0f;
		DeviceRow onRow = offRow;
		onRow.field[poly] = 1.0f;
		const FxChain off = chainWith(offRow);
		const FxChain on = chainWith(onRow);
		const auto heapOf = [](LaneRack& lane, const FxChain& chain) {
			const Armed armed;
			(void)lane.provision(chain);
			return Counter::bytes;
		};
		LaneRack none;
		none.prepare(kSampleRate, 0, 4096);
		const size_t alone = heapOf(none, off);
		LaneRack lazy;
		lazy.prepare(kSampleRate, kVoices, 4096);
		const size_t lazyOff = heapOf(lazy, off);
		syncGuarded(lazy, off);
		LaneRack eager;
		eager.setEagerVoicesForTest(true);
		eager.prepare(kSampleRate, kVoices, 4096);
		const size_t eagerOff = heapOf(eager, off);
		// **AND ITS PER-VOICE OUTPUT CELLS, WHICH ARE NOT VOICE INSTANCES** (2026-11-07, per-device output counts): a device
		// that declares a `perVoice` output (Noise) keeps `voices × outputCount()` cells and tick windows in each ear, sized at
		// its provision from its own count — they were every slot's, at the bound, paid by `setVoices` before this.
		// Restated from the declaration: four float vectors, two of cells and two of `kTickWindowStride` windows.
		bool voicedOutputs = false;
		for (const device::SourceDesc& out : desc->outputs) {
			voicedOutputs = voicedOutputs || out.perVoice;
		}
		const size_t voicedCount = voicedOutputs ? desc->outputCount() : 0;
		const size_t cellBytes = sizeof(float) * kVoices * voicedCount;
		const size_t windowBytes = cellBytes * static_cast<size_t>(device::kTickWindowStride);
		const size_t pools = voicedCount == 0 ? 0 : 2 * (vectorHeapBytes(cellBytes) + vectorHeapBytes(windowBytes));
		CAPTURE(alone);
		CAPTURE(lazyOff);
		CAPTURE(eagerOff);
		CAPTURE(pools);
		// THE DROP: Poly off in a lane with voices costs what the device costs in a lane with none, plus those cells.
		CHECK(lazyOff == alone + pools);
		// NON-VACUITY: the eager road paid at least one storage per voice more for the same row.
		CHECK(eagerOff >= alone + pools + kVoices * desc->audio->stateBytes);
		// THE SWITCH GOING ON builds what the eager road built up front — no more, no less — and seats it. The one
		// thing on top is the post that carries them to the drain, and it is the same size for every device.
		const size_t flip = heapOf(lazy, on);
		REQUIRE(flip >= eagerOff - alone - pools);
		const size_t post = flip - (eagerOff - alone - pools);
		if (postBytes == 0) {
			postBytes = post;
		}
		CHECK(post == postBytes);
		CHECK(post < 1024);
		syncGuarded(lazy, on);
		CHECK(lazy.engineForTest().slotAtLane(lazy.engineForTest().slotOf(1)).voices.size() == kVoices);
		// THE FURNITURE'S RAMPS CAME WITH THEM: as many as the eager road seats, per voice (none for a device whose Mix
		// is its own). Without them a slot-blended device's Mix and wet Gain do nothing under Poly.
		syncGuarded(eager, off);
		syncGuarded(eager, on);
		CHECK(lazy.engineForTest().slotAtLane(lazy.engineForTest().slotOf(1)).furnitureVoice.size()
		      == eager.engineForTest().slotAtLane(eager.engineForTest().slotOf(1)).furnitureVoice.size());
		lazy.finish();
		none.finish();
		eager.finish();
	}
	CHECK(swept >= 5);
}
TEST_CASE("a replaced device is parked, never destroyed on the audio thread, and the message thread "
          "frees it") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain first = chainOf(kMaxFxSlots, 0);
	lane.provision(first);
	syncGuarded(lane, first);
	const FxChain second = chainOf(kMaxFxSlots, 1);
	lane.provision(second);
	syncGuarded(lane, second); // eight replaced — and not one free, which the guard asserted
	lane.finish();
	size_t frees = 0;
	{
		const Armed armed;
		lane.reap();
		frees = Counter::frees;
	}
	// AND THEY WENT SOMEWHERE — a pointer dropped on the floor also frees nothing.
	CHECK(frees > 0);
}

TEST_CASE("an arrival the chain no longer wants is freed by the message thread, and the mailbox is "
          "emptied every sync") {
	// A -> B -> A BETWEEN TWO SYNCS IS ONE HAND ON A DEVICE BROWSER.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain a = chainOf(1, 0);
	const FxChain b = chainOf(1, 1);
	REQUIRE(descriptorForRow(a.device[0].row) != descriptorForRow(b.device[0].row));

	lane.provision(a);
	syncGuarded(lane, a);
	const void* seated = lane.storageOfForTest(deviceFx(lane, 0));
	REQUIRE(seated != nullptr);

	lane.provision(b);
	lane.provision(a);
	// BOTH POSTS WAIT, AND THE LATER ONE WINS AT THE DRAIN (kill-lanes step 3a). The mailbox was one per
	// slot and a second post for that slot replaced the first in place; it is one list naming devices now,
	// and the message thread cannot take back something the drain may already be holding — so the
	// superseded post is handed back by `converge` and freed by the reap below, which is what this case
	// measures. What matters is unchanged: the sync leaves nothing waiting.
	CHECK(lane.pendingCountForTest() == 2);
	syncGuarded(lane, a);
	// THE LANE STILL HOLDS THE SAME INSTANCE — a device that did not change keeps its memory.
	CHECK(lane.storageOfForTest(deviceFx(lane, 0)) == seated);
	CHECK(lane.pendingCountForTest() == 0);
	lane.finish();
	size_t frees = 0;
	{
		const Armed armed;
		lane.reap();
		frees = Counter::frees;
	}
	CHECK(frees > 0); // the unwanted arrival, freed here and not on the audio thread

	// AND THE NEXT REAL CHANGE STILL LANDS.
	lane.provision(b);
	syncGuarded(lane, b);
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) == descriptorForRow(b.device[0].row));
	lane.finish();
}

TEST_CASE("a lane whose device has not been provisioned is not run, rather than built on the audio "
          "thread") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain chain = chainOf(1, 0);
	syncGuarded(lane, chain); // no provision at all
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) == nullptr);
	CHECK(engineOrder(lane).empty());
	lane.provision(chain);
	syncGuarded(lane, chain);
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) != nullptr);
	lane.finish();
}

TEST_CASE("a device built from the editor's newer chain while the block syncs the chain it copied is not lost") {
	// **THE INTERLEAVING, IN FEATHERS' ORDER.** The audio thread copies the bridge's chain at the top of a
	// block (`publishEditorLanes`) and syncs from that copy later in the same block; the editor's gesture
	// lands in the bridge, and `provisionAndDrain` builds from the bridge — the NEWER chain — on the same
	// main-thread visit. When both fall inside one block, the sync meets an arrival its chain does not
	// want and hands it back. Read from the code while fixing the rack's version of it (the rack's case
	// lost one add in three); not observed in Feathers, and this case is the unit-level proof that the
	// interleaving, if it happens, no longer loses the device.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain copied {};               // what the block copied: an empty lane
	const FxChain edited = chainOf(1, 0); // what the hand made in the meantime
	REQUIRE(lane.provision(edited) == 1);
	syncGuarded(lane, copied); // the block's sync, on its copy: the arrival goes back
	lane.finish();
	syncGuarded(lane, edited); // the next block copies the new chain — and has nothing to adopt
	lane.finish();
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) == nullptr);

	// THE NEXT MAIN-THREAD VISIT BUILDS IT AGAIN, and the block after runs it.
	CHECK(lane.provision(edited) == 1);
	syncGuarded(lane, edited);
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) != nullptr);
	lane.finish();
}

TEST_CASE("a tag this build does not know is kept in the chain and not run") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain = chainOf(3, 0);
	chain.device[1].row.tag = { 'z', 'z', 'z', 'z' };
	CHECK(lane.provision(chain) == 2);
	syncGuarded(lane, chain);
	// THE STATE KEEPS IT (a tag this build cannot run is kept in its chain); the ENGINE does not run it.
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 1)) == nullptr);
	// THE OTHERS RUN, IN THE CHAIN'S ORDER WITH THE STRANGER LEFT OUT — not shifted into its place. BY
	// DEVICE: the engine packs what it runs into its own slots, so the stranger's absence is a missing
	// DEVICE in the order, not a hole in the numbering.
	CHECK(engineOrder(lane) == std::vector<DeviceInstanceId> { deviceFx(lane, 0), deviceFx(lane, 2) });
	CHECK(chain.count == 3);
	lane.finish();
}

TEST_CASE("the same device is kept across every edit that is not a replacement, and a new device of "
          "the same kind on the same lane is a new instance") {
	// `FxRunner`'s two-of-a-kind lesson, asked of the lane: identity is the INSTANCE ID, never the kind.
	const device::Descriptor* filter = deviceForTag(fx::filter::kTraits.tag);
	REQUIRE(filter != nullptr);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain = chainWith(enabledRowFor(*filter, 7));
	lane.provision(chain);
	syncGuarded(lane, chain);
	// THE ENGINE'S OWN IDENTITY FOR WHAT IS SEATED — issued at adoption, never reused. An address is not
	// an identity here: a freed instance's address may be handed to the next one.
	const DeviceInstanceId first = lane.engineForTest().slotAtLane(0).id;
	REQUIRE(first != kNoInstance);

	// A KNOB MOVES: nothing is built, the instance stays.
	chain.device[0].row.field[static_cast<size_t>(fx::filter::Param::Cutoff)] = 9.0f;
	CHECK(lane.provision(chain) == 0);
	syncGuarded(lane, chain);
	CHECK(lane.engineForTest().slotAtLane(0).id == first);

	// THE DEVICE IS REPLACED BY ANOTHER OF ITS KIND IN THE SAME PLACE: a new instance.
	chain.device[0].row.instanceId = 8;
	CHECK(lane.provision(chain) == 1);
	syncGuarded(lane, chain);
	CHECK(lane.engineForTest().slotAtLane(0).id != first);
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) == filter);
	CHECK(lane.provision(chain) == 0); // and it is now the device the chain names
	lane.finish();
}

//==================================================================================================
// THE ARRANGEMENT — one engine, each device's own switch
//==================================================================================================

TEST_CASE("one lane runs a device on the bus or inside the voices by its own switch, and the flip asks "
          "for nothing") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	fx::filter::State state;
	state.settings.enabled = true;
	FxChain chain;
	setFxFilterAt(&chain, 0, state);
	chain.count = 1;

	lane.provision(chain);
	syncGuarded(lane, chain);
	const void* bus = lane.busStorageForTag(fx::filter::kTraits.tag, true);
	CHECK(bus != nullptr);
	CHECK(lane.voiceStorageForTag(0, fx::filter::kTraits.tag) == nullptr);
	CHECK_FALSE(lane.anyPerVoice());
	// NOT BUILT PER VOICE WHILE THE SWITCH IS OFF (backlog NS): the provision that turns it on builds them, on the
	// message thread, and the sync below only seats them — `syncGuarded` asserts it allocates nothing.
	CHECK(lane.engineForTest().slotAtLane(0).voices.empty());

	fx::filter::setValueOf(&state, fx::filter::Param::Poly, 1.0f);
	setFxFilterAt(&chain, 0, state);
	CHECK(lane.provision(chain) == 0); // a switch is a setting: no device is rebuilt
	CHECK(lane.pendingCountForTest() == 1); // its voices, built and waiting
	syncGuarded(lane, chain);
	CHECK(lane.busStorageForTag(fx::filter::kTraits.tag, true) == nullptr);
	CHECK(lane.anyPerVoice());
	for (size_t v = 0; v < kVoices; ++v) {
		CAPTURE(v);
		CHECK(lane.voiceStorageForTag(v, fx::filter::kTraits.tag) != nullptr);
	}
	// AND THE BUS INSTANCE IS THE SAME ONE, waiting for the switch to come back.
	CHECK(lane.busStorageForTag(fx::filter::kTraits.tag, false) == bus);
	renderGuarded(lane, 3);
}

TEST_CASE("the engine tells each per-voice instance which voice it is, and never the bus one") {
	// `AudioOps::setVoice` (backlog MZ): NOISE folds the slot into its Free seed so a unison does not play one noise in
	// every voice. Asked of the engine through a device that answers it — the slot it was told, or -1 for none.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain chain = chainWith(defaultRowFor(fx::noise::kDescriptor, { 1 }));
	lane.provision(chain);
	syncGuarded(lane, chain);
	REQUIRE(lane.engineForTest().slotAtLane(0).voices.size() == kVoices);
	for (size_t v = 0; v < kVoices; ++v) {
		CAPTURE(v);
		const void* storage = lane.voiceStorageForTag(v, fx::noise::kTraits.tag);
		REQUIRE(storage != nullptr); // NOISE's Poly is on by default
		CHECK(fx::noise::voiceForTest(storage) == static_cast<int64_t>(v));
	}
	const void* bus = lane.busStorageForTag(fx::noise::kTraits.tag, false);
	REQUIRE(bus != nullptr);
	CHECK(fx::noise::voiceForTest(bus) == -1);
}

TEST_CASE("backlog NS: voice instances built when the switch goes on are told their voice too") {
	// THE LATE ROAD (`RackEngine::provisionVoices`) MUST BUILD WHAT THE EARLY ONE BUILT: each instance told which voice
	// it is before any field reaches it — or a unison plays one noise in every voice, the bug `setVoice` fixed.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const auto poly = static_cast<size_t>(fx::noise::kDescriptor.traits.perVoiceControl);
	DeviceRow row = defaultRowFor(fx::noise::kDescriptor, { 1 });
	row.field[poly] = 0.0f;
	const FxChain off = chainWith(row);
	row.field[poly] = 1.0f;
	const FxChain on = chainWith(row);
	lane.provision(off);
	syncGuarded(lane, off);
	REQUIRE(lane.engineForTest().slotAtLane(0).voices.empty());
	lane.provision(on);
	syncGuarded(lane, on);
	REQUIRE(lane.engineForTest().slotAtLane(0).voices.size() == kVoices);
	for (size_t v = 0; v < kVoices; ++v) {
		CAPTURE(v);
		const void* storage = lane.voiceStorageForTag(v, fx::noise::kTraits.tag);
		REQUIRE(storage != nullptr);
		CHECK(fx::noise::voiceForTest(storage) == static_cast<int64_t>(v));
	}
	lane.finish();
}

TEST_CASE("EVERY device with a POLY switch: flipping it away and back does not replay a frozen tail") {
	// `FxRunner`'s measured rule, asked of the engine's arrangement: the instances that stop being
	// responsible are reset, so a round trip cannot resume from a tail frozen at the first flip.
	constexpr int kBurstFrames = 256;
	std::vector<float> burst(kBurstFrames, 0.0f);
	for (int i = 0; i < kBurstFrames; ++i) {
		burst[static_cast<size_t>(i)] =
		    0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.03 * static_cast<double>(i)));
	}
	int swept = 0;
	for (const device::Descriptor* desc : audioDevices()) {
		// A LISTENER HAS NO TAIL TO FREEZE (`AudioOps::listensOnly`, the Follower): it writes nothing.
		if (desc->traits.perVoiceControl < 0 || desc->audio->listensOnly) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const auto poly = static_cast<size_t>(desc->traits.perVoiceControl);
		++swept;
		const auto chainAt = [&](float polyValue) {
			DeviceRow row = variedRowFor(*desc);
			row.field[poly] = polyValue;
			return chainWith(row);
		};
		const FxChain onBus = chainAt(0.0f);
		const FxChain perVoice = chainAt(1.0f);

		const auto run = [&](bool withBurst, bool withFlip) {
			LaneRack lane;
			lane.prepare(kSampleRate, 2, 4096);
			lane.provision(onBus);
			syncGuarded(lane, onBus);
			std::vector<float> l(kBurstFrames, 0.0f);
			std::vector<float> r(kBurstFrames, 0.0f);
			if (withBurst) {
				std::copy(burst.begin(), burst.end(), l.begin());
				std::copy(burst.begin(), burst.end(), r.begin());
				lane.processBus(l.data(), r.data(), kBurstFrames);
			}
			if (withFlip) {
				lane.provision(perVoice);
				syncGuarded(lane, perVoice);
				lane.provision(onBus);
				syncGuarded(lane, onBus);
			}
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			lane.processBus(l.data(), r.data(), kBurstFrames);
			lane.finish();
			return l;
		};
		const std::vector<float> reference = run(false, true);
		const std::vector<float> afterFlip = run(true, true);
		const std::vector<float> noFlip = run(true, false);

		// NON-VACUITY: the burst must leave a tail, or "no tail replayed" says nothing.
		bool rings = false;
		for (int i = 0; i < kBurstFrames; ++i) {
			rings = rings || noFlip[static_cast<size_t>(i)] != reference[static_cast<size_t>(i)];
		}
		REQUIRE_MESSAGE(rings, "the burst leaves no tail on ", who);
		float worst = 0.0f;
		for (int i = 0; i < kBurstFrames; ++i) {
			worst = std::max(worst, std::fabs(afterFlip[static_cast<size_t>(i)]
			                                  - reference[static_cast<size_t>(i)]));
		}
		CAPTURE(worst);
		CHECK_MESSAGE(worst == 0.0f, "a poly round trip replayed a frozen tail on ", who);
	}
	CHECK(swept >= 3);
}

TEST_CASE("reorder: a device crossing the per-voice/bus boundary while voices sound") {
	int swept = 0;
	for (const device::Descriptor* desc : audioDevices()) {
		if (desc->traits.perVoiceControl < 0) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		const auto poly = static_cast<size_t>(desc->traits.perVoiceControl);
		++swept;
		LaneRack lane;
		lane.prepare(kSampleRate, kVoices, 4096);
		for (size_t step = 0; step < 8; ++step) {
			DeviceRow row = variedRowFor(*desc);
			row.field[poly] = (step % 2 == 0) ? 1.0f : 0.0f;
			// THE CONTROL POLY NEEDS ON, ON (`Traits::perVoiceRequires`: the Follower's Mode = Audio).
			if (const int req = desc->traits.perVoiceRequires; req >= 0) {
				row.field[static_cast<size_t>(req)] = 1.0f;
			}
			const FxChain chain = chainWith(row);
			lane.provision(chain);
			syncGuarded(lane, chain);
			CHECK(lane.anyPerVoice() == (step % 2 == 0));
			renderGuarded(lane, static_cast<uint32_t>(step) + 1);
		}
	}
	CHECK(swept > 0);
}

//==================================================================================================
// REORDER — a device keeps its identity, its instance and its memory; only its POSITION moves
//==================================================================================================
//
// THE PLUGIN CRASHED DOING THIS, 2026-08-28 — reordering audio fx with audio playing — with no stack
// and no core (the flatpak is a container apport ignores). It was
// a DRAG: the card passed through eleven positions in under a second, so the chain was re-published
// many times while voices rendered. These are separate cases rather than subcases so a crash in one
// cannot hide the others; run them with `scripts/run-cases.sh --each`.

TEST_CASE("reorder: a chain permuted between renders keeps every device's instance, voices sounding") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain = chainOf(4);
	lane.provision(chain);
	syncGuarded(lane, chain);
	// **EACH DEVICE BY ITS ID, AND THE MEMORY IT HAS** — the pair the reorder must not break. The lane
	// number used to stand in for the identity here (it did not move when a device was dragged); with no
	// lane left, the id IS the identity, and the claim reads as what it always meant: THIS device, whose
	// storage is THIS, still has it wherever the drag put it.
	std::array<DeviceInstanceId, 4> id {};
	std::array<const void*, 4> instance {};
	for (size_t p = 0; p < 4; ++p) {
		id[p] = deviceFx(lane, p);
		REQUIRE(id[p] != kNoInstance);
		instance[p] = lane.storageOfForTest(id[p]);
		REQUIRE(instance[p] != nullptr);
	}
	renderGuarded(lane, 0);
	// ELEVEN STEPS, the number of positions the crashing drag passed through.
	for (size_t step = 0; step < 11; ++step) {
		chain = movedChain(chain, 0, chain.count - 1);
		CHECK(lane.provision(chain) == 0); // a reorder builds nothing
		syncGuarded(lane, chain);
		CHECK(engineOrder(lane) == chainOrder(lane, chain));
		for (size_t p = 0; p < 4; ++p) {
			CAPTURE(p);
			CHECK(lane.storageOfForTest(id[p]) == instance[p]);
		}
		renderGuarded(lane, static_cast<uint32_t>(step) * 32u);
	}
}

TEST_CASE("reorder: two devices of one kind swap, and each carries its OWN state to its new place") {
	// **THE RESTATEMENT IS TWO DEVICES ALONE, DRIVEN IN THE NEW ORDER.** Block one runs A then B; block
	// two runs B then A. The lane must render exactly that — bit for bit — which is only true if each
	// device's memory travelled with it. `FxRunner` rebuilt on a reorder, so its reference was a chain
	// freshly BUILT in the swapped order; the rack moves instead, and the reference moves with it.
	//
	// SWEPT OVER EVERY DEVICE WITH AN ENABLE SWITCH, both live and set differently, so order is audible.
	constexpr int kBurstFrames = 256;
	std::vector<float> burst(kBurstFrames, 0.0f);
	for (int i = 0; i < kBurstFrames; ++i) {
		burst[static_cast<size_t>(i)] =
		    0.9f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * 0.03 * static_cast<double>(i)));
	}
	int swept = 0;
	int heard = 0;
	for (const device::Descriptor* desc : audioDevices()) {
		if (desc->traits.enableControl < 0) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		++swept;
		const DeviceRow rowA = variedRowFor(*desc, 1);
		const DeviceRow rowB = enabledRowFor(*desc, 2);
		FxChain chain;
		chain.device[0].row = rowA;
		chain.device[1].row = rowB;
		chain.count = 2;
		chain.nextInstanceId = 3;

		LaneRack lane;
		lane.prepare(kSampleRate, 0, 4096);
		lane.provision(chain);
		syncGuarded(lane, chain);
		std::vector<float> l1(burst), r1(burst);
		lane.processBus(l1.data(), r1.data(), kBurstFrames);
		const FxChain swapped = movedChain(chain, 0, 1);
		lane.provision(swapped);
		syncGuarded(lane, swapped);
		std::vector<float> l2(burst), r2(burst);
		lane.processBus(l2.data(), r2.data(), kBurstFrames);
		lane.finish();

		// THE REFERENCE: two devices alone.
		Solo a(*desc, rowA);
		Solo b(*desc, rowB);
		std::vector<float> ref1(burst), ref1r(burst);
		a.process(ref1.data(), ref1r.data(), kBurstFrames);
		b.process(ref1.data(), ref1r.data(), kBurstFrames);
		std::vector<float> ref2(burst), ref2r(burst);
		b.process(ref2.data(), ref2r.data(), kBurstFrames);
		a.process(ref2.data(), ref2r.data(), kBurstFrames);

		float worst1 = 0.0f;
		float worst2 = 0.0f;
		for (size_t i = 0; i < burst.size(); ++i) {
			worst1 = std::max(worst1, std::fabs(l1[i] - ref1[i]));
			worst2 = std::max(worst2, std::fabs(l2[i] - ref2[i]));
		}
		CAPTURE(worst1);
		CAPTURE(worst2);
		CHECK(worst1 == 0.0f); // the lane IS two devices in a row, before any move
		CHECK(worst2 == 0.0f); // and after the swap, each with its own memory

		// NON-VACUITY: the swapped order must sound different from the unswapped one continuing, or
		// the case cannot tell a swap from no swap. Named rather than silently counted.
		std::vector<float> stay(burst), stayR(burst);
		{
			Solo a2(*desc, rowA);
			Solo b2(*desc, rowB);
			std::vector<float> x(burst), xr(burst);
			a2.process(x.data(), xr.data(), kBurstFrames);
			b2.process(x.data(), xr.data(), kBurstFrames);
			a2.process(stay.data(), stayR.data(), kBurstFrames);
			b2.process(stay.data(), stayR.data(), kBurstFrames);
		}
		float moved = 0.0f;
		for (size_t i = 0; i < burst.size(); ++i) {
			moved = std::max(moved, std::fabs(stay[i] - ref2[i]));
		}
		if (moved > 1e-9f) {
			++heard;
		}
	}
	CHECK(swept > 0);
	CHECK(heard > 0);
}

TEST_CASE("reorder: reordered with the mailbox EMPTY, the engine follows the chain anyway") {
	// **`FxRunner` LEFT SLOTS EMPTY HERE** — keyed by position, a reorder with no provision found no
	// arrival and ran a chain whose slots were not what the chain said. The rack's order pass needs no
	// provision at all, so every step is the chain's own order, asserted.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain = chainOf(4);
	lane.provision(chain);
	syncGuarded(lane, chain);
	for (size_t step = 0; step < 11; ++step) {
		chain = movedChain(chain, 0, chain.count - 1); // deliberately no provision()
		syncGuarded(lane, chain);
		CHECK(engineOrder(lane) == chainOrder(lane, chain));
		renderGuarded(lane, static_cast<uint32_t>(step) + 3);
	}
}

TEST_CASE("reorder: the drag — many syncs per render, provisions for only some of them") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain = chainOf(5);
	lane.provision(chain);
	syncGuarded(lane, chain);
	for (size_t block = 0; block < 6; ++block) {
		for (size_t nudge = 0; nudge < 4; ++nudge) {
			chain = movedChain(chain, 0, chain.count - 1);
			if (nudge % 2 == 0) {
				lane.provision(chain);
			}
			syncGuarded(lane, chain);
		}
		CHECK(engineOrder(lane) == chainOrder(lane, chain));
		renderGuarded(lane, static_cast<uint32_t>(block) + 5);
	}
}

//==================================================================================================
// THE CONNECTOR, THE LANE A DEVICE IS TOLD, RESET, THE CLOCK
//==================================================================================================

TEST_CASE("a device is told its LANE and keeps it across a reorder; the connector is told its POSITION") {
	const device::Descriptor* owltt = deviceForTag(fx::owltt::kTraits.tag);
	const device::Descriptor* filter = deviceForTag(fx::filter::kTraits.tag);
	REQUIRE(owltt != nullptr);
	REQUIRE(filter != nullptr);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	std::vector<size_t> owlttPositions;
	std::vector<std::string> tags;
	owlttPositions.reserve(64); // the connector runs inside a guarded sync, so the probe may not grow
	tags.reserve(64);
	lane.setDspConnector([&](const char* tag, size_t position, void*) {
		tags.emplace_back(tag, 4);
		if (device::sameTag(tag, fx::owltt::kTraits.tag)) {
			owlttPositions.push_back(position);
		}
	});
	FxChain chain;
	chain.device[0].row = defaultRowFor(*filter, { 1 });
	chain.device[1].row = defaultRowFor(*filter, { 2 });
	chain.device[2].row = defaultRowFor(*owltt, { 3 });
	chain.count = 3;
	chain.nextInstanceId = 4;
	lane.provision(chain);
	syncGuarded(lane, chain);
	const DeviceInstanceId owltt3 = deviceFx(lane, 2);
	REQUIRE(owltt3 != kNoInstance);
	const auto* seated = static_cast<const fx::owltt::Owltt*>(lane.storageOfForTest(owltt3));
	REQUIRE(seated != nullptr);
	// WHAT A DEVICE IS TOLD IS THE ENGINE'S OWN SLOT (kill-lanes step 3a), so the expected number is asked
	// of the engine rather than written out — a literal here would be the record's lane by coincidence.
	const size_t owlttSlot = lane.slotOf(owltt3);
	REQUIRE(owlttSlot != rack::RackEngine::kNoSlot);
	CHECK(seated->slotForTest() == owlttSlot);
	CHECK(owlttPositions == std::vector<size_t> { 2 });
	CHECK(std::any_of(tags.begin(), tags.end(),
	                  [](const std::string& t) { return !device::sameTag(t.c_str(), fx::owltt::kTraits.tag); }));

	owlttPositions.clear();
	tags.clear();
	const FxChain moved = movedChain(chain, 2, 0);
	CHECK(lane.provision(moved) == 0);
	syncGuarded(lane, moved);
	// THE SAME DEVICE, MOVED — the same instance, still in the engine's same slot and still told it; the
	// connector is the one thing that says where it is DRAWN, and that is what moved.
	CHECK(deviceFx(lane, 0) == owltt3); // the move put it at the front of the chain
	CHECK(lane.storageOfForTest(owltt3) == seated);
	CHECK(lane.slotOf(owltt3) == owlttSlot);
	CHECK(seated->slotForTest() == owlttSlot);
	// AND THE ENGINE RUNS IT FIRST NOW — its slot did not move, its place in the run did.
	CHECK(lane.engineForTest().positionOfLane(owlttSlot) == 0);
	CHECK(owlttPositions == std::vector<size_t> { 0 });
	lane.finish();
}

TEST_CASE("the meter connector runs on the audio thread, and the shape the plugin installs is free") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	size_t lastPosition = kMaxFxSlots;
	// THE PLUGIN'S SHAPE, DELIBERATELY COPIED: an inner callback capturing a pointer and the position —
	// sixteen bytes, `std::function`'s small buffer on the nose.
	lane.setDspConnector([&lastPosition](const char* tag, size_t position, void* storage) {
		if (storage == nullptr || !device::sameTag(tag, fx::owltt::kTraits.tag)) {
			return;
		}
		size_t* self = &lastPosition;
		static_cast<fx::owltt::Owltt*>(storage)->setMeterCallback(
		    [self, position](size_t, const std::array<float, 3>&, const std::array<float, 3>&,
		                     const std::array<float, 3>&, bool, const fx::owltt::Owltt::MeterExtras&) {
			    *self = position;
		    });
	});
	const device::Descriptor* owltt = deviceForTag(fx::owltt::kTraits.tag);
	REQUIRE(owltt != nullptr);
	DeviceRow row = defaultRowFor(*owltt, { 1 });
	row.field[static_cast<size_t>(fx::owltt::Param::Enabled)] = 1.0f;
	const FxChain chain = chainWith(row);
	lane.provision(chain);
	syncGuarded(lane, chain);
	std::vector<float> left(64, 0.1f);
	std::vector<float> right(64, 0.1f);
	lane.processBus(left.data(), right.data(), 64);
	CHECK(lastPosition == 0); // it really ran
	syncGuarded(lane, chain); // and a second sync re-installs it, still free
	lane.finish();
}

TEST_CASE("the connector reaches an automodulator, and the phase it publishes is the device's own") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	void* automodStorage = nullptr;
	size_t automodPosition = kMaxFxSlots;
	std::vector<float> reported;
	reported.reserve(4096);
	lane.setDspConnector([&](const char* tag, size_t position, void* storage) {
		if (!device::sameTag(tag, fx::automod::kTraits.tag) || storage == nullptr) {
			return;
		}
		automodStorage = storage;
		automodPosition = position;
		static_cast<fx::automod::Automod*>(storage)->setReadingsCallback(
		    [&reported](float phase, float) { reported.push_back(phase); });
	});
	const device::Descriptor* automod = deviceForTag(fx::automod::kTraits.tag);
	REQUIRE(automod != nullptr);
	DeviceRow row = defaultRowFor(*automod, { 1 });
	row.field[static_cast<size_t>(fx::automod::Param::Enabled)] = 1.0f;
	const FxChain chain = chainWith(row);
	lane.provision(chain);
	syncGuarded(lane, chain);
	REQUIRE(automodStorage != nullptr);
	CHECK(automodPosition == 0);

	constexpr int kRun = 4096;
	std::vector<float> left = probeSignal(static_cast<size_t>(kRun), 0.6f, 3);
	std::vector<float> right = left;
	lane.processBus(left.data(), right.data(), kRun);
	lane.finish();

	const auto* device = static_cast<const fx::automod::Automod*>(automodStorage);
	REQUIRE(reported.size() > 8);
	CHECK(reported.front() == 0.0f);
	// ONE TICK BEHIND THE DEVICE, restated from the rate it says it runs at.
	const auto inc = static_cast<uint32_t>(
	    std::llround(static_cast<double>(device->effectiveRateHz()) / kSampleRate * 4294967296.0));
	const double perTick =
	    static_cast<double>(static_cast<uint64_t>(inc) * fx::automod::kTickSamples) / 4294967296.0;
	CHECK(perTick > 1e-5);
	CHECK(std::fabs((device->lfo().phase() - static_cast<double>(reported.back())) - perTick) < 1e-7);
	for (size_t i = 1; i < reported.size(); ++i) {
		CAPTURE(i);
		CHECK(reported[i] > reported[i - 1]);
	}
}

TEST_CASE("reorder: with the OWLTT meter connector installed, which every sync calls") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	size_t handed = 0;
	size_t handedNull = 0;
	size_t lastPosition = kMaxFxSlots;
	lane.setDspConnector([&](const char* tag, size_t position, void* storage) {
		if (!device::sameTag(tag, fx::owltt::kTraits.tag)) {
			return;
		}
		++handed;
		handedNull += storage == nullptr ? 1 : 0;
		lastPosition = position;
	});
	FxChain chain = chainOf(4);
	bool hasOwltt = false;
	for (size_t l = 0; l < 4; ++l) {
		hasOwltt = hasOwltt || device::sameTag(chain.device[l].row.tag.data(), fx::owltt::kTraits.tag);
	}
	REQUIRE(hasOwltt); // without OWLTT in the lane the connector is never called
	lane.provision(chain);
	syncGuarded(lane, chain);
	for (size_t step = 0; step < 11; ++step) {
		chain = movedChain(chain, 0, chain.count - 1);
		lane.provision(chain);
		syncGuarded(lane, chain);
		renderGuarded(lane, static_cast<uint32_t>(step) + 1);
		// THE POSITION IT WAS TOLD IS WHERE THE CHAIN DRAWS IT.
		for (size_t at = 0; at < fxChainLength(chain); ++at) {
			if (device::sameTag(fxDeviceAtPosition(chain, at).row.tag.data(), fx::owltt::kTraits.tag)) {
				CHECK(lastPosition == at);
			}
		}
	}
	CHECK(handed > 0);
	CHECK(handedNull == 0);
}

TEST_CASE("reset forgets the state, keeps the device, and asks for nothing") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	fx::filter::State state;
	state.settings.enabled = true;
	fx::filter::setValueOf(&state, fx::filter::Param::Cutoff, 9.0f);
	FxChain chain;
	setFxFilterAt(&chain, 0, state);
	chain.count = 1;
	lane.provision(chain);
	syncGuarded(lane, chain);
	renderGuarded(lane, 1);
	const void* before = lane.storageOfForTest(deviceFx(lane, 0));
	REQUIRE(before != nullptr);
	const float corner = fx::filter::opsTargetForTest(before).cutoffLog2Hz;
	{
		const Armed armed;
		lane.reset();
		for (size_t v = 0; v < kVoices; ++v) {
			lane.resetVoice(v);
		}
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
	CHECK(lane.storageOfForTest(deviceFx(lane, 0)) == before);
	CHECK(fx::filter::opsTargetForTest(before).cutoffLog2Hz == corner);
}

TEST_CASE("the lane holds the song's clock, replaces it every sync, and hands it to a device built later") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	CHECK_FALSE(lane.clockForTest().haveTempo());
	const FxChain empty {};
	syncGuarded(lane, empty, device::TransportClock::fromSecondsPerBeat(0.5));
	CHECK(lane.clockForTest().secondsPerBeat == doctest::Approx(0.5));
	syncGuarded(lane, empty, device::TransportClock::fromSecondsPerBeat(1.0));
	CHECK(lane.clockForTest().secondsPerBeat == doctest::Approx(1.0));
	syncGuarded(lane, empty, device::TransportClock::unknown());
	CHECK_FALSE(lane.clockForTest().haveTempo());

	// AN AUTOMOD ADDED MID-SONG, synced to the first division: its rate is the tempo's, from its first
	// block — asked of the device, against the free fallback, which is a different number.
	fx::automod::Settings s;
	s.enabled = true;
	s.syncDivision = static_cast<int>(fx::automod::kFreeRateCount);
	const fx::automod::Row automodRow = fx::automod::rowOf(s);
	FxChain chain;
	chain.device[0].row.tag = { 'A', 'M', 'O', 'D' };
	for (size_t i = 0; i < automodRow.size(); ++i) {
		chain.device[0].row.field[i] = automodRow[i];
	}
	chain.device[0].row.fieldsPresent = automodRow.size();
	chain.device[0].row.instanceId = 1;
	chain.count = 1;
	lane.provision(chain);
	syncGuarded(lane, chain, device::TransportClock::fromSecondsPerBeat(0.5));
	std::vector<float> left(64, 0.0f);
	std::vector<float> right(64, 0.0f);
	lane.processBus(left.data(), right.data(), 64);
	lane.finish();
	const auto* dev = static_cast<const fx::automod::Automod*>(lane.storageOfForTest(deviceFx(lane, 0)));
	REQUIRE(dev != nullptr);
	const float synced = dev->effectiveRateHz();
	CHECK(synced == doctest::Approx(1.0f / (device::kDivisionBeats[0] * 0.5)).epsilon(1e-3));
	CHECK(fx::automod::nominalRateHz(s) != doctest::Approx(synced));
}

TEST_CASE("a device's row state past its parameters reaches it — automod's folded travel, the shaper's "
          "phase space") {
	// **`FxRunner` HANDED THE WHOLE ROW; THE ENGINE HELD ONLY THE PARAMETERS** until this move found
	// it. `params.size() + extraState` is the row rule, and the engine's row is that wide now.
	int swept = 0;
	for (const device::Descriptor* desc : audioDevices()) {
		if (desc->traits.extraState == 0) {
			continue;
		}
		const std::string who = deviceName(*desc);
		CAPTURE(who);
		++swept;
		DeviceRow row = defaultRowFor(*desc, { 1 });
		REQUIRE(row.fieldsPresent == desc->params.size() + desc->traits.extraState);
		for (size_t k = 0; k < desc->traits.extraState; ++k) {
			row.field[desc->params.size() + k] = 0.25f + 0.125f * static_cast<float>(k % 5);
		}
		LaneRack lane;
		lane.prepare(kSampleRate, kVoices, 4096);
		const FxChain chain = chainWith(row);
		lane.provision(chain);
		syncGuarded(lane, chain);
		const rack::RackEngine& e = lane.engineForTest();
		REQUIRE(e.fieldsPresentAt(0) == row.fieldsPresent);
		for (size_t k = 0; k < desc->traits.extraState; ++k) {
			CAPTURE(k);
			CHECK(e.fieldAt(0, desc->params.size() + k) == row.field[desc->params.size() + k]);
		}
		lane.finish();
	}
	CHECK(swept >= 2);
}

TEST_CASE("a segment is a block: in flight from a sync to the next sync or the end of the render, and "
          "never after") {
	// **THE EPOCH'S PARITY IS THE HANDOFF**, and an idle audio thread must read as idle — a reader of it
	// that waited on a block nobody was running would wait forever. Several syncs in one render (the
	// motion tick re-deciding the lane) are several blocks, each closed by the next.
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain chain = chainOf(2);
	lane.provision(chain);
	const rack::RackEngine& e = lane.engineForTest();
	CHECK_FALSE(e.inBlockForTest());
	for (size_t syncs = 1; syncs <= 3; ++syncs) {
		CAPTURE(syncs);
		for (size_t i = 0; i < syncs; ++i) {
			syncGuarded(lane, chain);
			CHECK(e.inBlockForTest());
		}
		lane.finish();
		CHECK_FALSE(e.inBlockForTest());
		lane.finish(); // a second finish is harmless — it does not open what it closes
		CHECK_FALSE(e.inBlockForTest());
	}
}

TEST_CASE("the lane resolves through its host's routes, each device reading its OWN inputs") {
	// FEATHERS' MODULATION, RESOLVED BY THE ENGINE: the routes come from a list the host published (in
	// Feathers, `ParamSet::routeEdges()`), in Feathers' coordinates, and each source is read through the
	// bus of the device doing the reading — the mechanism the upstream rule rides on (`Synth::fillBusFor`
	// zeroes what a device cannot see). Two filters, one source routed to both, visible to only one.
	//
	// **THE COORDINATE A DEVICE IS RESOLVED AT IS THE ENGINE'S OWN SLOT** (kill-lanes step 3a), which is
	// what `busFor`, `modulatedFieldAt` and the published destination are all indexed by. This case used to
	// write 0 and 1 for all three, which was right only while the record's lane and the engine's slot
	// happened to agree. It ASKS now — `slotOfStatePosition` is the seam `LaneRack` exposes for exactly
	// this reader — and pins the pair it ended up with, so a change in how the engine places devices comes
	// back as a red here rather than as a case silently measuring the other filter.
	const device::Descriptor* filter = deviceForTag(fx::filter::kTraits.tag);
	REQUIRE(filter != nullptr);
	const size_t cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	REQUIRE(filter->params[cutoff].modulatable);
	const size_t ordinal = filter->nthModulatableOfControl(cutoff);

	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	FxChain chain;
	chain.device[0].row = enabledRowFor(*filter, 1);
	chain.device[1].row = enabledRowFor(*filter, 2);
	chain.count = 2;
	chain.nextInstanceId = 3;
	lane.provision(chain);
	// THE FIRST CONVERGE IS WHAT DECIDES THE SLOTS, so the routes and the buses are built against what it
	// chose rather than against a guess. A second sync below runs the block they apply to.
	(void)lane.converge(chain, LaneRack::noStrip());
	const size_t seen = lane.slotOfStatePosition(kLaneChain, 0);
	const size_t unseen = lane.slotOfStatePosition(kLaneChain, 1);
	REQUIRE(seen != rack::RackEngine::kNoSlot);
	REQUIRE(unseen != rack::RackEngine::kNoSlot);
	REQUIRE(seen != unseen);

	// **THE ROUTES BY IDENTITY, AND THE HOST NODE'S CELLS WITH THEM (kill-lanes §2.4b).** This built a
	// compiled edge list by coordinate and a BUS per device — one device's bus carrying the macro at
	// full, the other's at nothing — to prove each device resolved through its own bus. There is no bus:
	// a device's inputs point at the cells its sources own, and here both point at the same macro cell.
	// What survives is the half that was never about the mask: a route reaches the device it names, and
	// only that device, resolved against the source's published value.
	std::array<float, 12> coreCells {}; // the host node's outputs — macros then weights, as Feathers lays them
	coreCells[0] = 1.0f;                 // macro 0 at full
	const DeviceInstanceId first = chain.device[0].row.instanceId;
	const DeviceInstanceId second = chain.device[1].row.instanceId;
	const std::array<backplane::Route, 1> routes {
		backplane::Route { backplane::Source::core(0), backplane::Destination::control(first, cutoff), 0.25f },
	};
	lane.setHostRoutes(routes);
	lane.setCoreSources(coreCells); // the host node.s outputs, pushed into the engine.s own cells
	syncGuarded(lane, chain);

	const rack::RackEngine& e = lane.engineForTest();
	const float base = chain.device[0].row.field[cutoff];
	const double span = device::routeSpanOf(filter->params[cutoff]);
	const double expected = device::withRouteOffset(filter->params[cutoff], base, 0.25 * 1.0 * span);
	CHECK(e.modulatedFieldAt(seen, cutoff) == doctest::Approx(expected));
	CHECK(e.modulatedFieldAt(seen, cutoff) != doctest::Approx(base)); // non-vacuity: it moved
	// THE DEVICE NO ROUTE NAMES SITS AT ITS BASE — not because a mask hid the source from it, but because
	// it has no input. That is the whole of "does not reach here" now.
	CHECK(e.modulatedFieldAt(unseen, cutoff) == chain.device[1].row.field[cutoff]);
	(void)second;
	(void)ordinal;
	lane.finish();
}

TEST_CASE("prepare drops a provision not yet adopted, and the next provision builds it again") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const FxChain chain = chainOf(2);
	CHECK(lane.provision(chain) == 2);
	CHECK(lane.pendingCountForTest() == 2);
	lane.prepare(96000.0, kVoices, 4096);
	CHECK(lane.pendingCountForTest() == 0);
	CHECK(lane.provision(chain) == 2); // the mirror forgot them, so they are built at the new rate
	syncGuarded(lane, chain);
	CHECK(engineOrder(lane) == chainOrder(lane, chain));
	lane.finish();
}

//==================================================================================================
// THE MOD STRIP — chain 0 on the same engine, where `ModRunner` used to run it
//==================================================================================================
//
// Every claim `test_mod_runner.cpp` made about the strip, asked of the engine that replaced the runner,
// plus two it could not make: identity is the INSTANCE ID (`ModRunner` compared the tag, so two of a
// kind swapping places each kept the other's stage), and a modulator ticks at its OWN declared rate
// (`ModRunner` ticked every device once per control tick).

namespace {

std::vector<const device::Descriptor*> modDevices() {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		if (d.mod != nullptr) {
			out.push_back(&d);
		}
	}
	return out;
}

/// A STRIP DEVICE AS EVERY ADD ROAD MAKES ONE: the device's defaults and an instance id.
///
/// **THE THIRD THING IS GONE, AND IT IS THE THING THIS COMMENT WAS ABOUT (kill-lanes step 3c).** It
/// stamped `deviceSlotOfRack(slot)` into `DeviceRow::routeLane` — the address the VIEW still carried
/// while the state built from these chains already kept none, and the note here said step 3c would
/// retire it. It did. So the `slot` argument went with it: it existed only to compute the address, and
/// a parameter kept for the sake of a symmetry it no longer has is a place for a caller to pass the
/// wrong number and never learn.
DeviceRow stripRow(const device::Descriptor& desc, DeviceInstanceId id) {
	return defaultRowFor(desc, { id });
}

ModRack stripOf(std::initializer_list<const device::Descriptor*> devices, DeviceInstanceId firstId = 100) {
	ModRack strip {};
	strip.count = 0;
	for (const device::Descriptor* d : devices) {
		strip.device[strip.count] = stripRow(*d, firstId + strip.count);
		++strip.count;
	}
	return strip;
}

const FxChain kNoChain {};

void stripSync(LaneRack& lane, const ModRack& strip, const device::NoteContext& busNote = kNote) {
	{
		const Armed armed;
		lane.sync(kNoChain, strip, busNote, device::TransportClock::unknown());
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
}

void tickGuarded(LaneRack& lane, bool gate, bool retrigger) {
	device::ModTickContext context {};
	context.gate = gate;
	context.retrigger = retrigger;
	{
		const Armed armed;
		lane.tickModulators(context, kTickFrames);
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
}

//--------------------------------------------------------------------------------------------//
// PROBES: a modulator that counts its ticks, and note devices that lie about their counts.     //
//--------------------------------------------------------------------------------------------//

struct TickCounter {
	int ticks = 0;
};

template <size_t kBlock>
constexpr device::ModOps kCounterOps {
	.stateBytes = sizeof(TickCounter),
	.stateAlign = alignof(TickCounter),
	.construct = [](void* s) { new (s) TickCounter(); },
	.destruct = [](void* s) { static_cast<TickCounter*>(s)->~TickCounter(); },
	.prepare = [](void*, double) {},
	.reset = [](void* s) { static_cast<TickCounter*>(s)->ticks = 0; },
	.setFields = [](void*, const float*, size_t) {},
	.blockSamples = kBlock,
	.tick = [](void* s, const device::ModTickContext&) { ++static_cast<TickCounter*>(s)->ticks; },
	.outputAt = [](const void* s, size_t) { return static_cast<float>(static_cast<const TickCounter*>(s)->ticks); },
	.playhead = nullptr,
};

const std::array<device::SourceDesc, 1> kCounterOutputs { { { "ticks", "Ticks" } } };

const device::Descriptor kCounter32 {
	.traits = { .tag = "CT32", .name = "Counter 32", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = kCounterOutputs,
	.mod = &kCounterOps<32>,
};
const device::Descriptor kCounter64 {
	.traits = { .tag = "CT64", .name = "Counter 64", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = kCounterOutputs,
	.mod = &kCounterOps<64>,
};

/// A NOTE DEVICE THAT WRITES ONE EVENT AND CLAIMS 999999 — the host must clamp before anyone reads it.
constexpr device::NoteOps kLiarOps {
	.stateBytes = sizeof(int),
	.stateAlign = alignof(int),
	.construct = [](void* s) { new (s) int(0); },
	.destruct = [](void*) {},
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes =
	    [](void*, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) {
		    if (outCapacity > 0 && inCount > 0 && in != nullptr && out != nullptr) {
			    out[0] = in[0];
		    }
		    *outCount = 999999;
	    },
};
const device::Descriptor kLiar {
	.traits = { .tag = "LIAR", .name = "Liar", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.notes = &kLiarOps,
};

/// A NOTE DEVICE THAT RECORDS THE COUNT IT WAS HANDED — the party a lie would reach — and PASSES THE
/// STREAM ON, so a stream left behind by one pass is visible to the next (a device that swallowed it
/// would hide exactly the replay the "not played again" case is about; the first draft did).
size_t g_nosySaw = 0;
constexpr device::NoteOps kNosyOps {
	.stateBytes = sizeof(int),
	.stateAlign = alignof(int),
	.construct = [](void* s) { new (s) int(0); },
	.destruct = [](void*) {},
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes =
	    [](void*, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) {
		    g_nosySaw = inCount;
		    const size_t n = std::min(inCount, outCapacity);
		    for (size_t i = 0; i < n; ++i) {
			    out[i] = in[i];
		    }
		    *outCount = n;
	    },
};
const device::Descriptor kNosy {
	.traits = { .tag = "NOSY", .name = "Nosy", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.notes = &kNosyOps,
};

/// FEATHERS' CATALOGUE PLUS THE PROBES — the road a third party's device takes into the strip.
const device::Descriptor* findWithProbes(void*, const char* tag) {
	for (const device::Descriptor* d : { &kCounter32, &kCounter64, &kLiar, &kNosy }) {
		if (device::sameTag(d->traits.tag, tag)) {
			return d;
		}
	}
	return deviceForTag(tag);
}
const device::Catalogue kWithProbes { nullptr, &findWithProbes };

device::NoteEvent noteOnEvent(int32_t id, int16_t key, double velocity = 1.0) {
	return { device::NoteEvent::Kind::On, 0, id, 0, 0, key, velocity };
}

} // namespace

TEST_CASE("strip: a device runs only in the half that can run it — no audio device in the strip, and a "
          "modulator in the fx lane too") {
	// FEATHERS' POLICY (`LaneRack::fits`), which `FxRunner` and `ModRunner` each kept for their own half
	// and nothing asked of the lane until the converger made it a host's stated rule. MEASURED: with the
	// policy bypassed, all 35 lane cases passed — this is the case that was missing.
	//
	// INVERTED IN ITS LANE HALF, 2026-10-04 (every mod effect is in scope in the Feathers fx rack): the envelope in the fx lane RUNS now. The strip half stands — a filter in the strip does not.
	const device::Descriptor* filter = deviceForTag(fx::filter::kTraits.tag);
	const device::Descriptor* env = deviceForTag(mod::dahdsr::kTraits.tag);
	REQUIRE(filter != nullptr);
	REQUIRE(env != nullptr);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ filter, env });
	FxChain chain;
	chain.device[0].row = defaultRowFor(*env, { 7 });
	chain.device[1].row = defaultRowFor(*filter, { 8 });
	chain.count = 2;
	lane.provision(chain, strip);
	{
		const Armed armed;
		lane.sync(chain, strip, kNote, device::TransportClock::unknown());
	}
	CHECK(Counter::allocations == 0);
	// ASKED BY DEVICE (kill-lanes step 3a): one that may not run is nowhere in the engine, and one that may
	// is wherever the engine put it.
	// THE STATE HOLDS ALL FOUR; the ENGINE runs every one but the filter in the strip.
	CHECK(lane.descriptorOfForTest(deviceStrip(lane, 0)) == nullptr); // the filter, in the strip
	CHECK(lane.descriptorOfForTest(deviceStrip(lane, 1)) == env);
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 0)) == env); // the envelope, in the fx lane — runs now
	CHECK(lane.descriptorOfForTest(deviceFx(lane, 1)) == filter);
	// AND THE ENGINE WALKS THE STRIP'S CHAIN AHEAD OF THE LANE'S, the lane in its own order — whatever order
	// they were adopted in.
	CHECK(engineOrder(lane)
	      == std::vector<DeviceInstanceId> { deviceStrip(lane, 1), deviceFx(lane, 0), deviceFx(lane, 1) });
	lane.finish();
}

TEST_CASE("strip: every modulator in the registry is built, adopted and advanced, on its own lane") {
	const std::vector<const device::Descriptor*> devices = modDevices();
	REQUIRE(devices.size() >= 2);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	ModRack strip {};
	strip.count = 0;
	for (size_t slot = 0; slot < devices.size() && slot < strip.device.size(); ++slot) {
		strip.device[slot] = stripRow(*devices[slot], 100 + slot);
		strip.count = slot + 1;
	}
	CHECK(lane.provision(kNoChain, strip) == strip.count);
	stripSync(lane, strip);
	for (size_t slot = 0; slot < strip.count; ++slot) {
		CAPTURE(slot);
		CHECK(lane.descriptorOfForTest(deviceStrip(lane, slot)) == devices[slot]);
	}
	// THE STRIP RANKS AHEAD OF THE LANE, in its own order — the engine's first positions are its devices.
	for (size_t slot = 0; slot < strip.count; ++slot) {
		CAPTURE(slot);
		CHECK(lane.deviceAt(lane.engineForTest().deviceAtPosition(slot)) == deviceStrip(lane, slot));
	}
	tickGuarded(lane, true, true);
	lane.finish();
}

TEST_CASE("strip: each modulator ticks at ITS OWN declared rate — a 32-sample device twice per control tick") {
	// The declared rate. `ModRunner` ticked every device once per control tick, so
	// Tracker and Note Expressions (32) ran at half the rate they declare. Counted, not inferred.
	LaneRack lane;
	lane.setCatalogue(kWithProbes);
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ &kCounter32, &kCounter64 });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	tickGuarded(lane, false, false);
	CHECK(lane.outputOf(deviceStrip(lane, 0), 0) == 2.0f);
	CHECK(lane.outputOf(deviceStrip(lane, 1), 0) == 1.0f);
	for (int i = 0; i < 3; ++i) {
		tickGuarded(lane, false, false);
	}
	CHECK(lane.outputOf(deviceStrip(lane, 0), 0) == 8.0f);
	CHECK(lane.outputOf(deviceStrip(lane, 1), 0) == 4.0f);
	// AND THE REGISTRY'S OWN 32-SAMPLE DEVICES ARE WHAT THIS RULE REACHES — named so a change to their
	// declarations is a change somebody sees.
	REQUIRE(deviceForTag(mod::tracker::kTraits.tag) != nullptr);
	CHECK(deviceForTag(mod::tracker::kTraits.tag)->mod->blockSamples == 32);
	lane.finish();
}

TEST_CASE("strip: a settings edit keeps the instance, a new device replaces it, two of a kind keep their own") {
	const std::vector<const device::Descriptor*> devices = modDevices();
	REQUIRE(devices.size() >= 2);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	ModRack strip = stripOf({ devices[0] });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	const rack::RackEngine& e = lane.engineForTest();
	// **THE INSTANCE ITSELF, NOT THE NUMBER BESIDE IT.** A chain handed in as a value names its devices by
	// lane (kill-lanes step 3a, `standInIdOf`), so an id cannot tell "kept" from "rebuilt" here — the
	// STORAGE can, and it is what the claim was always about.
	const void* first = lane.storageOfForTest(deviceStrip(lane, 0));
	REQUIRE(first != nullptr);

	// A SETTINGS EDIT: same device, nothing built, same instance.
	REQUIRE(strip.device[0].fieldsPresent > 0);
	strip.device[0].field[0] += 0.01f;
	CHECK(lane.provision(kNoChain, strip) == 0);
	stripSync(lane, strip);
	CHECK(lane.storageOfForTest(deviceStrip(lane, 0)) == first);

	// A DIFFERENT DEVICE IN THE SLOT: replaced, and the old one parked for the message thread.
	strip.device[0] = stripRow(*devices[1], 200);
	CHECK(lane.provision(kNoChain, strip) == 1);
	stripSync(lane, strip);
	CHECK(lane.descriptorOfForTest(deviceStrip(lane, 0)) == devices[1]);
	CHECK(lane.storageOfForTest(deviceStrip(lane, 0)) != first);

	// **TWO OF A KIND SWAPPING PLACES KEEP THEIR OWN INSTANCES** — the bug `ModRunner` had (it compared
	// the tag). Each device's IDENTITY rides in its row, so a swap is a reorder, and the instances follow.
	ModRack pair = stripOf({ devices[0], devices[0] });
	lane.provision(kNoChain, pair);
	stripSync(lane, pair);
	// NAMED BEFORE THE SWAP, because after it "position 0" is the OTHER device — which is the whole claim.
	const DeviceInstanceId idA = deviceStrip(lane, 0);
	const DeviceInstanceId idB = deviceStrip(lane, 1);
	REQUIRE(idA != kNoInstance);
	REQUIRE(idA != idB);
	const void* a = lane.storageOfForTest(idA);
	const void* b = lane.storageOfForTest(idB);
	REQUIRE(a != nullptr);
	REQUIRE(a != b);
	ModRack swapped = pair;
	std::swap(swapped.device[0], swapped.device[1]);
	CHECK(lane.provision(kNoChain, swapped) == 0); // a reorder builds nothing
	stripSync(lane, swapped);
	// EACH DEVICE STILL ITS OWN INSTANCE, and each still where the engine put it — a swap moves the ORDER,
	// not the devices' homes, and never rebuilds one.
	CHECK(lane.storageOfForTest(idA) == a);
	CHECK(lane.storageOfForTest(idB) == b);
	// AND THE STRIP'S NEW ORDER IS WHAT THE STATE SAYS: the two devices have traded places, and the engine
	// runs the one the swap put at the front first.
	CHECK(deviceStrip(lane, 0) == idB);
	CHECK(deviceStrip(lane, 1) == idA);
	CHECK(lane.deviceAt(e.deviceAtPosition(0)) == idB);
	lane.finish();
}

TEST_CASE("strip: an empty lane publishes nothing and cannot say where it stands") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	stripSync(lane, LaneRack::noStrip());
	CHECK(lane.outputOf(deviceStrip(lane, 0), 0) == 0.0f);
	uint32_t ordinal = 0xABCDEF01u;
	float phase = -7.5f;
	CHECK_FALSE(lane.playheadOf(deviceStrip(lane, 0), &ordinal, &phase));
	CHECK(ordinal == 0xABCDEF01u); // FALSE LEAVES THE OUTPUTS ALONE — registry.h's rule
	CHECK(phase == -7.5f);
	CHECK(lane.outputOf(9999, 0) == 0.0f);
	CHECK_FALSE(lane.playheadOf(9999, &ordinal, &phase));
	lane.finish();
}

TEST_CASE("strip: a gated envelope climbs, says where it stands, and reset puts it back without a free") {
	const device::Descriptor* dahdsr = deviceForTag(mod::dahdsr::kTraits.tag);
	REQUIRE(dahdsr != nullptr);
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ dahdsr });
	lane.provision(kNoChain, strip);
	// THE BUS NOTE CLOSED, so the only gate that can open the envelope is the TICK's — the road this
	// case is about. (A sync hands the engine the bus note too, and its gate would open it regardless.)
	stripSync(lane, strip, device::NoteContext { 60.0f, false });
	const DeviceInstanceId at = deviceStrip(lane, 0);
	CHECK(lane.outputOf(at, 0) == 0.0f);
	tickGuarded(lane, false, false);
	CHECK(lane.outputOf(at, 0) == 0.0f); // ungated: it stays at rest — non-vacuity for the climb below
	tickGuarded(lane, true, true);
	for (int i = 0; i < 8; ++i) {
		tickGuarded(lane, true, false);
	}
	CHECK(lane.outputOf(at, 0) > 0.0f); // a gated DAHDSR at its defaults climbs from zero
	uint32_t ordinal = mod::Playhead::kIdle;
	float phase = 0.0f;
	CHECK(lane.playheadOf(at, &ordinal, &phase));
	CHECK(ordinal != mod::Playhead::kIdle);
	{
		const Armed armed;
		lane.reset();
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
	CHECK(lane.outputOf(at, 0) == 0.0f);
	CHECK(lane.descriptorOfForTest(at) == dahdsr); // the instance survived it
	lane.finish();
}

TEST_CASE("strip: with no note device the note lane is a pass-through, field by field") {
	const std::vector<const device::Descriptor*> devices = modDevices();
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	ModRack strip {};
	strip.count = 0;
	for (const device::Descriptor* d : devices) {
		if (d->notes == nullptr && strip.count < strip.device.size()) {
			strip.device[strip.count] = stripRow(*d, 100 + strip.count);
			++strip.count;
		}
	}
	REQUIRE(strip.count >= 1); // a rack of modulators, none of which claims the stream
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	lane.finish();
	std::array<device::NoteEvent, 3> in {};
	in[0] = noteOnEvent(7, 60);
	in[1] = noteOnEvent(8, 64, 0.5);
	in[2] = { device::NoteEvent::Kind::Off, 32, 7, 0, 0, 60, 0.0 };
	std::array<device::NoteEvent, 8> out {};
	size_t produced = 0;
	{
		const Armed armed;
		produced = lane.processNotes(in.data(), in.size(), out.data(), out.size());
	}
	CHECK(Counter::allocations == 0);
	CHECK(Counter::frees == 0);
	REQUIRE(produced == 3);
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(out[i].kind == in[i].kind);
		CHECK(out[i].key == in[i].key);
		CHECK(out[i].noteId == in[i].noteId);
		CHECK(out[i].timeSamples == in[i].timeSamples);
		CHECK(out[i].velocity == in[i].velocity);
	}
}

TEST_CASE("strip: a registered tracker hears the note and publishes its pitch, from its own defaults") {
	namespace trk = mod::tracker;
	const device::Descriptor* tracker = deviceForTag(trk::kTraits.tag);
	REQUIRE(tracker != nullptr);
	REQUIRE(tracker->notes != nullptr);
	REQUIRE(tracker->mod != nullptr); // one object behind both tables — notes in, a source out
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ tracker });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	lane.finish();
	std::array<device::NoteEvent, 1> in { noteOnEvent(1, 72) };
	std::array<device::NoteEvent, 4> out {};
	REQUIRE(lane.processNotes(in.data(), in.size(), out.data(), out.size()) == 1);
	CHECK(out[0].key == 72); // it reads and passes on
	tickGuarded(lane, true, true);
	const double root = trk::kDescriptor.params[static_cast<size_t>(trk::Param::RootNote)].def;
	const double spread = trk::kDescriptor.params[static_cast<size_t>(trk::Param::Spread)].def;
	const float expected = static_cast<float>((72.0 - root) / (spread * 12.0));
	REQUIRE(expected != 0.0f); // off the root, so a flat zero cannot satisfy it
	CHECK(lane.outputOf(deviceStrip(lane, 0), 0) == doctest::Approx(expected).epsilon(1e-4));
}

TEST_CASE("strip: the note lane clamps to what the caller can hold, and an empty stream is nothing") {
	LaneRack lane;
	lane.prepare(kSampleRate, kVoices, 4096);
	std::array<device::NoteEvent, 4> in {};
	for (size_t i = 0; i < in.size(); ++i) {
		in[i] = noteOnEvent(static_cast<int32_t>(i), static_cast<int16_t>(60 + i));
	}
	std::array<device::NoteEvent, 4> out {};
	out[2].key = 99;
	out[3].key = 98;
	CHECK(lane.processNotes(in.data(), in.size(), out.data(), 2) == 2);
	CHECK(out[0].key == 60);
	CHECK(out[1].key == 61);
	CHECK(out[2].key == 99); // past the capacity: untouched
	CHECK(out[3].key == 98);
	CHECK(lane.processNotes(in.data(), in.size(), out.data(), 0) == 0);
	CHECK(lane.processNotes(in.data(), in.size(), nullptr, 4) == 0);
	CHECK(lane.processNotes(nullptr, 0, out.data(), out.size()) == 0);
}

TEST_CASE("strip: a device that overstates its note count cannot make the host write past the buffer") {
	LaneRack lane;
	lane.setCatalogue(kWithProbes);
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ &kLiar });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	lane.finish();
	std::array<device::NoteEvent, 1> in { noteOnEvent(3, 60) };
	constexpr size_t kCapacity = 4;
	std::array<device::NoteEvent, 16> backing {};
	for (device::NoteEvent& ev : backing) {
		ev.key = 77;
	}
	const size_t produced = lane.processNotes(in.data(), in.size(), backing.data(), kCapacity);
	CHECK(produced <= kCapacity);
	CHECK(backing[0].key == 60);
	for (size_t i = kCapacity; i < backing.size(); ++i) {
		CAPTURE(i);
		CHECK(backing[i].key == 77);
	}
}

TEST_CASE("strip: what one note device reports is clamped before the next one is handed it") {
	// INVISIBLE WITH ONE DEVICE — the copy out is bounded by the caller — and plain with two: the next
	// device receives the count as its `inCount` over a fixed buffer.
	LaneRack lane;
	lane.setCatalogue(kWithProbes);
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ &kLiar, &kNosy });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	lane.finish();
	g_nosySaw = SIZE_MAX;
	std::array<device::NoteEvent, 1> in { noteOnEvent(3, 60) };
	std::array<device::NoteEvent, 4> out {};
	lane.processNotes(in.data(), in.size(), out.data(), out.size());
	REQUIRE(g_nosySaw != SIZE_MAX); // the second device was reached at all
	CHECK(g_nosySaw <= rack::RackEngine::kMaxNotes);
}

TEST_CASE("strip: a note played through the lane is not played again by the next block") {
	// `processNotesNow` empties the stream after it: the block's own note pass must not hand the same
	// notes to the chain a second time. Counted at a device that records what it is handed.
	LaneRack lane;
	lane.setCatalogue(kWithProbes);
	lane.prepare(kSampleRate, kVoices, 4096);
	const ModRack strip = stripOf({ &kNosy });
	lane.provision(kNoChain, strip);
	stripSync(lane, strip);
	lane.finish();
	std::array<device::NoteEvent, 2> in { noteOnEvent(1, 60), noteOnEvent(2, 64) };
	std::array<device::NoteEvent, 4> out {};
	lane.processNotes(in.data(), in.size(), out.data(), out.size());
	REQUIRE(g_nosySaw == 2);
	stripSync(lane, strip); // the next segment's block, with its own (empty) note pass
	CHECK(g_nosySaw == 0);
	lane.finish();
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 0 — WHAT ONE DEVICE COSTS                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 0: what one device costs, measured rather than modelled") {
	// THE BOOKKEEPING a device pays wherever a chain is kept — the numbers the design doc's §3.4 had to
	// model. Printed so a reader can see them; asserted only where a value has a contract (a row must
	// hold every field), because a size is not a behaviour.
	MESSAGE("sizeof(rack::Slot) = " << sizeof(rack::Slot));
	MESSAGE("sizeof(rack::DeviceState) = " << sizeof(rack::DeviceState));
	MESSAGE("sizeof(FxDevice) = " << sizeof(FxDevice) << ", sizeof(DeviceRow) = " << sizeof(DeviceRow));
	CHECK(sizeof(rack::DeviceState) >= device::kMaxDeviceFields * sizeof(float));
	CHECK(sizeof(DeviceRow) >= device::kMaxDeviceFields * sizeof(float));

	// AND EACH DEVICE'S OWN STATE — `stateBytes`, the term that dominates. What `prepare` takes on top
	// (delay lines, an impulse response) is not declared anywhere and is not in this number.
	size_t total = 0;
	size_t largest = 0;
	for (const device::Descriptor& d : kDevices) {
		const size_t bytes = d.audio != nullptr ? d.audio->stateBytes
		                     : d.mod != nullptr ? d.mod->stateBytes
		                     : d.notes != nullptr ? d.notes->stateBytes
		                                          : 0;
		bool perVoice = false;
		for (const device::SourceDesc& s : d.outputs) {
			perVoice = perVoice || s.perVoice;
		}
		const std::string line = std::string(d.traits.name) + ": stateBytes " + std::to_string(bytes)
		                         + (perVoice ? " (per-voice sources)" : "");
		MESSAGE(line);
		total += bytes;
		largest = std::max(largest, bytes);
	}
	MESSAGE("devices " << std::size(kDevices) << ", stateBytes total " << total << ", largest " << largest);
	CHECK(total > 0);
}
