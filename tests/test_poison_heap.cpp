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

// SKLITTER ON THE POISONED HEAP, through the rack device's own ops — see `poison_heap.h` for why.
// `test_poison_heap_clap.cpp` is the same question one level up, through the plugin a host loads.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "poison_heap.h"

using feathers::test::kPoison;
using namespace feathers;
namespace sk = feathers::fx::sklitter;

namespace {

constexpr double kRate = 48000.0;

size_t controlNamed(const device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	return desc.params.size();
}

/// A rack device holding one SKLITTER, all of it on the poisoned heap — the storage too.
struct Box {
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	std::byte* mem = nullptr;
	rack::RackBridge* bridge = nullptr;
	DeviceInstanceId sk = kNoInstance;
	double beats = 0.0;
	Box() {
		// The allocation function itself: the aligned new-EXPRESSION is refused by MSVC (C2956, see rack_engine.cpp).
		mem = static_cast<std::byte*>(::operator new[](ops.stateBytes, std::align_val_t { ops.stateAlign }));
		ops.construct(mem);
		ops.prepare(mem, kRate, 4096);
		rack::kDeviceContainerOps.adopt(mem, hostCatalogue());
		bridge = rack::bridgeOf(mem);
		REQUIRE(bridge != nullptr);
		sk = bridge->addDevice("SKLITTER");
		REQUIRE(sk != kNoInstance);
		bridge->drain();
	}
	~Box() {
		ops.destruct(mem);
		::operator delete[](mem, std::align_val_t { ops.stateAlign });
	}
	Box(const Box&) = delete;
	Box& operator=(const Box&) = delete;

	sk::Sklitter* device() {
		rack::RackEngine& e = rack::engineForTest(mem);
		return reinterpret_cast<sk::Sklitter*>(e.slotAtLane(e.slotOf(sk)).storage.get());
	}
	void field(sk::Param p, float v) {
		bridge->setField(sk, static_cast<size_t>(p), v);
		bridge->drain();
	}

	/// Silence in, `seconds` of it, 64-frame pieces as the rack's host cuts them. Counts what came out.
	struct Heard {
		size_t nonzero = 0;
		size_t first = SIZE_MAX;
		float peak = 0.0f;
	};
	Heard silence(double seconds, int block = 256) {
		Heard h;
		std::vector<float> l(static_cast<size_t>(block));
		std::vector<float> r(static_cast<size_t>(block));
		device::TransportClock clk = device::TransportClock::fromSecondsPerBeat(0.5);
		clk.playing = true;
		const auto total = static_cast<size_t>(seconds * kRate);
		for (size_t n = 0; n < total; n += static_cast<size_t>(block)) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			clk.songPositionBeats = beats;
			ops.setTransportClock(mem, clk);
			bridge->drain();
			ops.processStereo(mem, l.data(), r.data(), block);
			beats += block / (kRate * 0.5);
			for (int i = 0; i < block; ++i) {
				const float v = std::max(std::fabs(l[i]), std::fabs(r[i]));
				if (!(v == 0.0f)) {
					++h.nonzero;
					h.first = std::min(h.first, n + static_cast<size_t>(i));
					h.peak = std::isfinite(v) ? std::max(h.peak, v) : 1e30f;
				}
			}
		}
		return h;
	}
};

std::string said(const Box::Heard& h) {
	return "nonzero=" + std::to_string(h.nonzero) + " first=" + std::to_string(h.first) + " peak=" + std::to_string(h.peak);
}

} // namespace

TEST_CASE("the poison is in place: an allocation this binary makes arrives full of it") {
	auto* p = new float[64];
	CHECK(p[0] == kPoison);
	CHECK(p[63] == kPoison);
	delete[] p;
	std::vector<float> reserved;
	reserved.reserve(16);
	CHECK(reserved.data()[0] == kPoison);
}

TEST_CASE("POISONED HEAP: SKLITTER captures silence and plays silence, in every mode") {
	for (int m = 0; m < static_cast<int>(sk::Mode::kCount); ++m) {
		CAPTURE(m);
		Box box;
		box.field(sk::Param::Mode, static_cast<float>(m));
		box.field(sk::Param::Engage, 0.0f);
		const Box::Heard idle = box.silence(2.0);
		CAPTURE(said(idle));
		CHECK(idle.nonzero == 0u);
		box.field(sk::Param::Engage, 1.0f);
		const Box::Heard engaged = box.silence(6.0);
		CAPTURE(said(engaged));
		CHECK(engaged.nonzero == 0u);
		// NOT VACUOUS: the device is playing its buffer, not bypassed or waiting.
		CAPTURE(static_cast<int>(box.device()->statusForTest()));
		CHECK(box.device()->statusForTest() == sk::Status::Playing);
	}
}

TEST_CASE("POISONED HEAP: the reported Grain lane — Featherverb (Owl), Delay and Automod — on silence, then a switch into Grain, a Buffer Length change, and reset") {
	Box box;
	sk::Sklitter* s = box.device();
	REQUIRE(s != nullptr);
	rack::RackBridge* lane = s->grainChain()->bridge();
	REQUIRE(lane != nullptr);
	const DeviceInstanceId fv = lane->addDevice("FVERB");
	const DeviceInstanceId dl = lane->addDevice("DELAY");
	const DeviceInstanceId am = lane->addDevice("AMOD");
	REQUIRE(fv != kNoInstance);
	REQUIRE(dl != kNoInstance);
	REQUIRE(am != kNoInstance);
	const device::Descriptor* fvd = deviceForTag("FVERB");
	lane->setField(fv, controlNamed(*fvd, "Featherverb Size Mode"), 6.0f);
	lane->setField(fv, controlNamed(*fvd, "Featherverb Decay"), 1.0f);
	lane->drain();
	box.field(sk::Param::SendOverride, 1.0f);

	box.field(sk::Param::Mode, static_cast<float>(sk::Mode::Shuffle));
	box.field(sk::Param::Engage, 1.0f);
	const Box::Heard shuffle = box.silence(4.0);
	CAPTURE(said(shuffle));
	CHECK(shuffle.nonzero == 0u);

	box.field(sk::Param::Mode, static_cast<float>(sk::Mode::Grain));
	const Box::Heard grain = box.silence(6.0);
	CAPTURE(said(grain));
	CHECK(grain.nonzero == 0u);

	for (float b = 0.0f; b <= 4.0f; b += 1.0f) {
		CAPTURE(b);
		box.field(sk::Param::BufferLength, b);
		const Box::Heard resized = box.silence(3.0);
		CAPTURE(said(resized));
		CHECK(resized.nonzero == 0u);
	}

	s->reset();
	box.field(sk::Param::Mode, static_cast<float>(sk::Mode::Shuffle));
	const Box::Heard after = box.silence(4.0, 64);
	CAPTURE(said(after));
	CHECK(after.nonzero == 0u);
}
