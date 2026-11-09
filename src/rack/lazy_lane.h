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

#pragma once

#include "backplane/axes.h"
#include "device/device.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/inner_chain_lanes.h"

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

// ONE LAZY INNER LANE, AS A DEVICE HOLDS IT — the Delay's FEEDBACK FX lane's machinery (delay-love §1, decision 9),
// FACTORED OUT UNCHANGED so COMP's encode/decode lane (backlog QG) is the same component, not a copy of it.
//
// **LAZY**: no chain until something asks for one — `ensure` from an editor, or a body whose lane holds devices — so a
// device that never uses its lane pays nothing for it. Built on the MESSAGE thread from what the host handed this
// storage (catalogue, gate, slots, voices, rate), then PUBLISHED through one atomic pointer; the audio thread takes it
// at its next block (`take`) and hands it the audio-thread state it caches (outer sources, clock, note). Never freed
// while the device lives, so the audio thread's pointer cannot dangle; never keyed by address.
//
// WHAT IS NOT HERE, because it is the holder's: WHERE the lane sits in the holder's signal, when it runs, and what its
// latency does to the holder's output (the Delay absorbs it in its loop; COMP adds it to its own).

namespace feathers::rack {

class LazyLane {
public:
	LazyLane(const backplane::Axes& axes, const char* costLane) : axes_(axes), costLane_(costLane) {}
	LazyLane(const LazyLane&) = delete;
	LazyLane& operator=(const LazyLane&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                              //
	//--------------------------------------------------------------------------------------------//

	/// BUILD THE LANE IF IT IS NOT BUILT, AND RETURN IT. Idempotent.
	InnerChain* ensure() {
		if (owned_ == nullptr) {
			auto lane = std::make_unique<InnerChain>(axes_);
			lane->engine().setCostLane(costLane_); // the spike recorder's path (backlog SR)
			if (haveCatalogue_) {
				lane->adopt(catalogue_);
			}
			if (haveGate_) {
				lane->setOuterGate(gate_);
			}
			if (haveSlots_) {
				lane->setHostSlots(slots_);
			}
			lane->setVoices(voices_);
			if (havePrepared_) {
				lane->prepare(sampleRate_, maxFrames_);
			}
			owned_ = std::move(lane);
			published_.store(owned_.get(), std::memory_order_release);
		}
		return owned_.get();
	}
	/// THE LANE ONCE BUILT, else null — the message thread's handle (the editor's, the body codec's).
	[[nodiscard]] InnerChain* owned() const noexcept { return owned_.get(); }

	// **THE HOST'S HANDOVERS ARE KEPT**, so a lane built later is built exactly as one built now would have been.
	void adopt(const device::Catalogue& catalogue) {
		catalogue_ = catalogue;
		haveCatalogue_ = true;
		if (owned_ != nullptr) {
			owned_->adopt(catalogue);
		}
	}
	void setOuterGate(const device::OuterGate& gate) {
		gate_ = gate;
		haveGate_ = true;
		if (owned_ != nullptr) {
			owned_->setOuterGate(gate);
		}
	}
	/// STAMPED AS LANE 0 — the holder's one lane.
	void setHostSlots(const device::HostSlotLink& link) {
		slots_ = link;
		slots_.lane = 0;
		haveSlots_ = true;
		if (owned_ != nullptr) {
			owned_->setHostSlots(slots_);
		}
	}
	void setVoices(size_t voices) {
		voices_ = voices;
		if (owned_ != nullptr) {
			owned_->setVoices(voices);
		}
	}
	void prepare(double sampleRate, int maxFrames) {
		sampleRate_ = sampleRate;
		maxFrames_ = maxFrames;
		havePrepared_ = true;
		if (owned_ != nullptr) {
			owned_->prepare(sampleRate_, maxFrames_);
		}
	}
	[[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

	/// WHETHER A BODY PUTS ANYTHING IN THE LANE — so a load builds the lane only when there is something to hold.
	static bool bodyHoldsDevices(const uint8_t* data, size_t bytes) {
		if (data == nullptr || bytes == 0) {
			return false;
		}
		bool any = false;
		std::vector<uint8_t> scratch;
		const auto map = [](void* ctx, std::span<const uint8_t> in, std::vector<uint8_t>* out) {
			*static_cast<bool*>(ctx) = true;
			out->assign(in.begin(), in.end());
			return true;
		};
		(void)mapChainBundle(std::span<const uint8_t>(data, bytes), map, &any, &scratch);
		return any;
	}

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD                                                                                //
	//--------------------------------------------------------------------------------------------//

	/// THE LANE FOR THIS BLOCK: a newly published one is handed the cached audio-thread state. Null until built.
	InnerChain* take() {
		InnerChain* lane = published_.load(std::memory_order_acquire);
		if (lane != running_) {
			running_ = lane;
			if (lane != nullptr) {
				if (haveOuter_) {
					lane->setOuterSources(outer_);
				}
				lane->setTransportClock(clock_);
				if (haveNote_) {
					lane->setNoteContext(note_);
				}
			}
		}
		return running_;
	}
	[[nodiscard]] InnerChain* running() const noexcept { return running_; }

	void setOuterSources(const device::OuterSources& outer) {
		outer_ = outer;
		haveOuter_ = true;
		if (running_ != nullptr) {
			running_->setOuterSources(outer);
		}
	}
	void setTransportClock(const device::TransportClock& clock) {
		clock_ = clock;
		if (running_ != nullptr) {
			running_->setTransportClock(clock);
		}
	}
	void setNoteContext(const device::NoteContext& note) {
		note_ = note;
		haveNote_ = true;
		if (running_ != nullptr) {
			running_->setNoteContext(note);
		}
	}
	/// WHETHER THE PUBLISHED LANE STILL HOLDS SOUND TO COME (`InnerChain::holdsTail`); false while unbuilt.
	[[nodiscard]] bool holdsTail() const noexcept {
		const InnerChain* lane = published_.load(std::memory_order_acquire);
		return lane != nullptr && lane->holdsTail();
	}

private:
	backplane::Axes axes_;
	const char* costLane_;

	// MESSAGE THREAD: the lane once built, and what the host handed this storage before it was.
	std::unique_ptr<InnerChain> owned_;
	bool haveCatalogue_ = false;
	device::Catalogue catalogue_ {};
	bool haveGate_ = false;
	device::OuterGate gate_ {};
	bool haveSlots_ = false;
	device::HostSlotLink slots_ {};
	size_t voices_ = 0;
	bool havePrepared_ = false;
	double sampleRate_ = 48000.0;
	int maxFrames_ = device::kDefaultMaxFrames;

	// THE HANDOFF.
	std::atomic<InnerChain*> published_ { nullptr };

	// AUDIO THREAD: the lane it has taken, and what it caches for a lane it has not taken yet.
	InnerChain* running_ = nullptr;
	bool haveOuter_ = false;
	device::OuterSources outer_ {};
	device::TransportClock clock_ {};
	bool haveNote_ = false;
	device::NoteContext note_ {};
};

/// THE CONTAINER TABLE FOR A HOLDER OF ONE `LazyLane` (at `LaneOf(storage)`, saved under the stable id `Id`):
/// `laneContainerOps` over that one lane, with the host's handovers KEPT for a lane not built yet, and a body that holds
/// devices building the lane before it loads. Written once for the Delay's feedback lane and COMP's encode/decode lane.
template <LazyLane& (*LaneOf)(void*), uint16_t Id>
struct LazyLaneOps {
	static std::array<BundleLane, 1> lanesOf(void* storage) { return { BundleLane { Id, LaneOf(storage).owned() } }; }

	static device::ContainerOps make() {
		static const device::ContainerOps base = laneContainerOps<&lanesOf>();
		device::ContainerOps ops = base;
		ops.adopt = [](void* storage, const device::Catalogue& catalogue) { LaneOf(storage).adopt(catalogue); };
		ops.setOuterGate = [](void* storage, const device::OuterGate& gate) { LaneOf(storage).setOuterGate(gate); };
		ops.setHostSlots = [](void* storage, const device::HostSlotLink& link) { LaneOf(storage).setHostSlots(link); };
		ops.setVoices = [](void* storage, size_t voices) { LaneOf(storage).setVoices(voices); };
		// **A BODY WITH DEVICES IN THE LANE BUILDS IT FIRST**; one without leaves an unbuilt lane unbuilt (and loads into a
		// built one as `readChainBundle` does: an empty lane body leaves it as it was).
		ops.loadState = [](void* storage, const uint8_t* data, size_t bytes) {
			if (LazyLane::bodyHoldsDevices(data, bytes)) {
				(void)LaneOf(storage).ensure();
			}
			return base.loadState(storage, data, bytes);
		};
		ops.applyState = [](void* storage, const uint8_t* data, size_t bytes) {
			LazyLane& lane = LaneOf(storage);
			if (LazyLane::bodyHoldsDevices(data, bytes)) {
				(void)lane.ensure();
			}
			// AN UNBUILT LANE IS ALREADY EMPTY, so emptying it (or applying a body that names nothing) succeeds.
			if (lane.owned() == nullptr) {
				return bytes == 0 || readChainBundle(lanesOf(storage), data, bytes);
			}
			return base.applyState(storage, data, bytes);
		};
		return ops;
	}
};

} // namespace feathers::rack
