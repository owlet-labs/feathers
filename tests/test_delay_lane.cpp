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
#include "doctest.h"

#include "fx/busutil/busutil_types.h"
#include "fx/clip/clip_types.h"
#include "fx/delay/bbd_math.h"
#include "fx/delay/delay.h"
#include "fx/delay/delay_lanes.h"
#include "fx/delay/delay_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// THE DELAY'S FEEDBACK FX LANE (stage S1), BY VALUE.
//
// TWO HALVES. The first drives the DSP (`Delay`) with a FAKE lane — a gain, a plain delay, a NaN, a DC offset — so
// every claim about the loop (Model B, the compensation, the floor, the safety) is checked against arithmetic a
// reader can do by hand rather than against another render. The second drives the device through its own ops with a
// real `rack::InnerChain`, for the claims that are about the holder: laziness, the body, real devices inside.

using namespace feathers;
namespace dl = feathers::fx::delay;

namespace {

constexpr double kRate = 48000.0;
/// 100 ms at 48 kHz — the free time `sync = 1` picks, at a multiplier of one.
constexpr size_t kD = 4800;

dl::Settings wetOnly(float feedback) {
	dl::Settings s = dl::stateOf(dl::rowOf(dl::State {}), static_cast<size_t>(dl::Param::kCount)).settings;
	s.sync = 1;
	s.timeMultiple = 1.0;
	s.mix = 1.0f;
	s.dub = false;
	s.feedback = feedback;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = dl::Mode::Clean;
	return s;
}

/// A LANE THAT IS A GAIN.
struct GainLane {
	float gain = 1.0f;
	static void run(void* ctx, float* l, float* r, int n) {
		const float g = static_cast<GainLane*>(ctx)->gain;
		for (int i = 0; i < n; ++i) {
			l[i] *= g;
			r[i] *= g;
		}
	}
	dl::LoopInsert insert() { return { this, &GainLane::run, 0 }; }
};

/// A LANE THAT IS A PLAIN DELAY OF `latency` SAMPLES, at a gain — and it may LIE about its latency (a known-bad).
struct LateLane {
	std::vector<float> l, r;
	size_t at = 0;
	float gain = 1.0f;
	uint32_t reported = 0;
	explicit LateLane(size_t latency, float g = 1.0f) : l(std::max<size_t>(latency, 1), 0.0f), r(l.size(), 0.0f),
	                                                    gain(g), reported(static_cast<uint32_t>(latency)) {
		if (latency == 0) {
			l.clear();
			r.clear();
		}
	}
	static void run(void* ctx, float* ol, float* orr, int n) {
		auto& self = *static_cast<LateLane*>(ctx);
		for (int i = 0; i < n; ++i) {
			float xl = ol[i] * self.gain;
			float xr = orr[i] * self.gain;
			if (!self.l.empty()) {
				std::swap(xl, self.l[self.at]);
				std::swap(xr, self.r[self.at]);
				self.at = (self.at + 1) % self.l.size();
			}
			ol[i] = xl;
			orr[i] = xr;
		}
	}
	dl::LoopInsert insert() { return { this, &LateLane::run, reported }; }
};

/// RENDER `frames` of `in` (the same on both channels) through `d`, in host blocks of `block`.
std::vector<float> render(dl::Delay& d, const std::vector<float>& in, int block, std::vector<float>* rightOut = nullptr) {
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t at = 0; at < l.size(); at += static_cast<size_t>(block)) {
		const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - at));
		d.processStereo(l.data() + at, r.data() + at, n);
	}
	if (rightOut != nullptr) {
		*rightOut = r;
	}
	return l;
}

std::vector<float> impulse(size_t frames) {
	std::vector<float> v(frames, 0.0f);
	v[0] = 1.0f;
	return v;
}

std::vector<float> burst(size_t frames, size_t burstFrames, float amp, uint32_t seed = 7) {
	std::vector<float> v(frames, 0.0f);
	for (size_t i = 0; i < burstFrames && i < frames; ++i) {
		seed = seed * 1664525u + 1013904223u;
		v[i] = amp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
	}
	return v;
}

std::unique_ptr<dl::Delay> fresh(const dl::Settings& s) {
	auto d = std::make_unique<dl::Delay>();
	d->prepare(kRate);
	d->setTarget(s);
	return d;
}

size_t peakIn(const std::vector<float>& v, size_t from, size_t to) {
	size_t best = from;
	for (size_t i = from; i < to && i < v.size(); ++i) {
		if (std::fabs(v[i]) > std::fabs(v[best])) {
			best = i;
		}
	}
	return best;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE LOOP, WITH A FAKE LANE                                                                         //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("FB: the lane is on what RE-ENTERS the line — the first repeat is untouched, repeat k carries the lane k-1 "
          "times (Model B, by design)") {
	for (const float g : { 0.0f, 0.5f, 1.0f, 1.5f }) {
		CAPTURE(g);
		const float f = 0.5f;
		auto d = fresh(wetOnly(f));
		GainLane lane { g };
		d->setLoopInsert(lane.insert());
		const std::vector<float> out = render(*d, impulse(6 * kD + 10), 256);
		// THE FIRST REPEAT IS THE IMPULSE ITSELF, exactly: nothing has been through the lane yet.
		CHECK(out[kD] == 1.0f);
		for (size_t k = 2; k <= 5; ++k) {
			CAPTURE(k);
			const auto want = static_cast<float>(std::pow(static_cast<double>(f * g), static_cast<double>(k - 1)));
			// The loop's 5 Hz DC blocker leaves a residue of a few parts in ten thousand per pass.
			CHECK(std::fabs(out[k * kD] - want) <= 2.0e-3f * std::max(1.0f, want));
			if (want > 0.0f) {
				CHECK(peakIn(out, k * kD - 200, k * kD + 200) == k * kD);
			}
		}
	}
	// AND WITH THE LANE AT ZERO, THE SECOND REPEAT IS EXACTLY NOTHING.
	auto d = fresh(wetOnly(0.5f));
	GainLane zero { 0.0f };
	d->setLoopInsert(zero.insert());
	const std::vector<float> out = render(*d, impulse(3 * kD), 256);
	CHECK(out[2 * kD] == 0.0f);
}

TEST_CASE("FB: a LATE lane is compensated — every repeat still lands exactly one tap apart, to the sample") {
	for (const size_t latency : { size_t { 1 }, size_t { 37 }, size_t { 500 }, size_t { 2048 } }) {
		CAPTURE(latency);
		auto d = fresh(wetOnly(0.5f));
		LateLane lane(latency);
		d->setLoopInsert(lane.insert());
		const std::vector<float> out = render(*d, impulse(5 * kD + 10), 300);
		for (size_t k = 1; k <= 4; ++k) {
			CAPTURE(k);
			CHECK(peakIn(out, k * kD - kD / 2, k * kD + kD / 2) == k * kD);
			const auto want = static_cast<float>(std::pow(0.5, static_cast<double>(k - 1)));
			CHECK(std::fabs(out[k * kD] - want) <= 2.0e-3f);
		}
	}
	// THE CONTROL: a lane that is late by 37 and SAYS 0 puts the second repeat 37 late — which is what the
	// compensation exists to remove, and shows this case can see it.
	auto d = fresh(wetOnly(0.5f));
	LateLane liar(37);
	liar.reported = 0;
	d->setLoopInsert(liar.insert());
	const std::vector<float> out = render(*d, impulse(3 * kD), 300);
	CHECK(peakIn(out, 2 * kD - kD / 2, 2 * kD + kD / 2) == 2 * kD + 37);
}

TEST_CASE("FB: while the lane is live, Time's floor is the lane's latency plus the shortest chunk (rule 5)") {
	dl::Settings s = wetOnly(0.5f);
	s.sync = 0;               // 10 ms
	s.timeMultiple = 0.25;    // 2.5 ms = 120 frames, under the floor below
	auto d = fresh(s);
	LateLane lane(500);
	d->setLoopInsert(lane.insert());
	(void)render(*d, impulse(512), 256);
	CHECK(d->headSecondsForTest(0) * kRate == doctest::Approx(500.0 + dl::kLoopChunkMin).epsilon(1e-9));
	// AND THE ECHO IS WHERE THE HEAD IS: one floor apart.
	auto e = fresh(s);
	LateLane lane2(500);
	e->setLoopInsert(lane2.insert());
	const std::vector<float> out = render(*e, impulse(4000), 64);
	const size_t floorFrames = 500 + static_cast<size_t>(dl::kLoopChunkMin);
	CHECK(peakIn(out, 1, floorFrames + 100) == floorFrames);
	CHECK(peakIn(out, floorFrames + 100, 2 * floorFrames + 100) == 2 * floorFrames);
	// WITHOUT A LANE THE SAME CONTROLS GIVE THE SHORT TAP, so the floor is the lane's and nobody else's.
	auto bare = fresh(s);
	(void)render(*bare, impulse(512), 256);
	CHECK(bare->headSecondsForTest(0) * kRate == doctest::Approx(120.0).epsilon(1e-9));
}

TEST_CASE("FB: the host's block size does not change the sound — 1, 7, 64, 300 and 512 frames render the same bits") {
	for (const double multiple : { 0.1, 1.0 }) { // 1 ms (clamped to the floor) and 10 ms
		for (const dl::Mode mode : { dl::Mode::Clean, dl::Mode::MultiTap, dl::Mode::Granular }) {
			CAPTURE(multiple);
			CAPTURE(static_cast<int>(mode));
			dl::Settings s = wetOnly(0.8f);
			s.sync = 0;
			s.timeMultiple = multiple;
			s.mode = mode;
			s.mix = 0.5f;
			const std::vector<float> in = burst(24000, 4800, 0.5f);
			std::vector<float> reference;
			for (const int block : { 512, 1, 7, 64, 300 }) {
				CAPTURE(block);
				auto d = fresh(s);
				LateLane lane(37, 0.7f);
				d->setLoopInsert(lane.insert());
				const std::vector<float> out = render(*d, in, block);
				if (reference.empty()) {
					reference = out;
					// Not vacuous: the loop made repeats.
					CHECK(std::fabs(out[20000]) + std::fabs(out[15000]) + std::fabs(out[10000]) > 0.0f);
				}
				else {
					CHECK(std::memcmp(out.data(), reference.data(), out.size() * sizeof(float)) == 0);
				}
			}
		}
	}
}

TEST_CASE("FB: a lane that pushes the loop past unity is bounded by the loop limiter, and stays finite") {
	auto d = fresh(wetOnly(1.1f));
	GainLane boost { 16.0f }; // +24 dB
	d->setLoopInsert(boost.insert());
	const std::vector<float> out = render(*d, burst(5 * 48000, 4800, 0.5f), 256);
	float late = 0.0f;
	for (size_t i = 2 * 48000; i < out.size(); ++i) {
		REQUIRE(std::isfinite(out[i]));
		late = std::max(late, std::fabs(out[i]));
	}
	// RUNAWAY, BUT A HELD SWELL AT THE CEILING: still sounding, never past it by more than the attack lets through.
	CHECK(late > 0.5f);
	CHECK(late <= dl::kLoopLimitCeiling * 1.05f);
	CHECK(d->loopLimitGainForTest() < 0.2f);
}

TEST_CASE("FB: a lane that hands back NaN cannot poison the line — the sample is replaced by silence and counted") {
	struct NanLane {
		static void run(void*, float* l, float* r, int n) {
			for (int i = 0; i < n; ++i) {
				l[i] *= 0.5f;
				r[i] *= 0.5f;
			}
			l[0] = std::numeric_limits<float>::quiet_NaN();
			r[n - 1] = std::numeric_limits<float>::infinity();
		}
	};
	auto d = fresh(wetOnly(0.9f));
	d->setLoopInsert({ nullptr, &NanLane::run, 0 });
	const std::vector<float> out = render(*d, burst(3 * 48000, 4800, 0.5f), 256);
	for (const float v : out) {
		REQUIRE(std::isfinite(v));
	}
	CHECK(d->loopResetsForTest() > 0);
}

TEST_CASE("FB: a lane that makes DC does not ratchet it up the loop — the 5 Hz blocker takes it out") {
	struct DcLane {
		static void run(void*, float* l, float* r, int n) {
			for (int i = 0; i < n; ++i) {
				l[i] += 0.3f;
				r[i] += 0.3f;
			}
		}
	};
	auto d = fresh(wetOnly(0.9f));
	d->setLoopInsert({ nullptr, &DcLane::run, 0 });
	const std::vector<float> out = render(*d, std::vector<float>(5 * 48000, 0.0f), 256);
	double mean = 0.0;
	for (size_t i = 4 * 48000; i < out.size(); ++i) {
		mean += out[i];
	}
	mean /= 48000.0;
	// Unblocked, 0.3 a pass at 0.9 feedback would sit near 0.3·0.9/(1−0.9) = 2.7, at the clip.
	CHECK(std::fabs(mean) < 0.02);
}

TEST_CASE("FB: with a lane, every mode's tail reaches EXACT zero, and `holdsTail` falls only once it has") {
	for (const dl::Mode mode : { dl::Mode::Clean, dl::Mode::Granular, dl::Mode::Xfade, dl::Mode::Tape, dl::Mode::Bbd,
	                             dl::Mode::MultiTap, dl::Mode::Reverse }) {
		CAPTURE(static_cast<int>(mode));
		dl::Settings s = wetOnly(0.6f);
		s.mode = mode;
		s.mix = 0.5f;
		auto d = fresh(s);
		LateLane lane(300, 0.8f);
		d->setLoopInsert(lane.insert());
		std::vector<float> l(256), r(256);
		const std::vector<float> in = burst(9600, 9600, 0.5f);
		size_t at = 0;
		bool fell = false;
		for (int block = 0; block < 48000 * 60 / 256; ++block, at += 256) {
			for (size_t i = 0; i < 256; ++i) {
				l[i] = r[i] = at + i < in.size() ? in[at + i] : 0.0f;
			}
			d->processStereo(l.data(), r.data(), 256);
			if (fell) {
				for (size_t i = 0; i < 256; ++i) {
					REQUIRE(l[i] == 0.0f);
					REQUIRE(r[i] == 0.0f);
				}
			}
			if (!fell && at > in.size() && !d->holdsTail()) {
				fell = true;
				CHECK(d->silentForTest());
			}
		}
		CHECK(fell);
	}
}

TEST_CASE("FB: the lane is handed chunks no longer than a tick, and short enough for the head to read only what has "
          "landed") {
	dl::Settings s = wetOnly(0.5f);
	s.sync = 0;
	s.timeMultiple = 0.1; // 1 ms, clamped to 37 + 16 = 53 frames
	auto d = fresh(s);
	LateLane lane(37);
	d->setLoopInsert(lane.insert());
	(void)render(*d, burst(4800, 480, 0.5f), 512);
	CHECK(d->laneChunksForTest() > 0);
	// (53 − 37) / 1.5 = 10.67, so 10.
	CHECK(d->laneChunkMaxForTest() == 10);
	auto e = fresh(wetOnly(0.5f));
	GainLane g { 0.5f };
	e->setLoopInsert(g.insert());
	(void)render(*e, burst(4800, 480, 0.5f), 512);
	CHECK(e->laneChunkMaxForTest() == dl::kLoopChunkMax);
}

//--------------------------------------------------------------------------------------------------//
// THE HOLDER, WITH A REAL LANE                                                                       //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE DEVICE THROUGH ITS OWN OPS, adopted with the host's catalogue.
struct Device {
	const device::AudioOps& audio = *dl::kDescriptor.audio;
	const device::ContainerOps& container = *dl::kDescriptor.container;
	void* mem = nullptr;
	dl::Row row {};

	explicit Device(const dl::Settings& s) {
		mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(mem);
		container.adopt(mem, hostCatalogue());
		audio.prepare(mem, kRate, 4096);
		row = dl::rowOf(dl::State { s });
		audio.setFields(mem, row.data(), row.size());
	}
	~Device() {
		audio.destruct(mem);
		::operator delete(mem, std::align_val_t { audio.stateAlign });
	}
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;

	rack::RackBridge& lane() const {
		rack::InnerChain* chain = dl::ensureFeedbackLane(mem);
		REQUIRE(chain != nullptr);
		REQUIRE(chain->bridge() != nullptr);
		return *chain->bridge();
	}
	DeviceInstanceId add(const char* tag) const {
		const DeviceInstanceId id = lane().addDevice(tag);
		REQUIRE(id != kNoInstance);
		return id;
	}
	std::vector<float> render(const std::vector<float>& in, int block = 300) const {
		std::vector<float> l = in;
		std::vector<float> r = in;
		for (size_t at = 0; at < l.size(); at += static_cast<size_t>(block)) {
			const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - at));
			audio.processStereo(mem, l.data() + at, r.data() + at, n);
		}
		return l;
	}
	std::vector<uint8_t> save() const {
		std::vector<uint8_t> body;
		const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
			                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
			                             out->insert(out->end(), d, d + n);
		                             } };
		REQUIRE(container.saveState(mem, sink));
		return body;
	}
};

size_t controlNamed(const device::Descriptor& desc, std::string_view name) {
	for (size_t i = 0; i < desc.params.size(); ++i) {
		if (name == desc.params[i].name) {
			return i;
		}
	}
	FAIL("no control " << name);
	return 0;
}

} // namespace

TEST_CASE("FB (holder): the lane is LAZY — a fresh Delay holds no chain, and one built but EMPTY is the old loop, "
          "bit for bit") {
	dl::Settings s = wetOnly(0.7f);
	s.mix = 0.4f;
	const std::vector<float> in = burst(3 * 48000, 9600, 0.5f);
	// THE REFERENCE: the bare DSP, no holder at all.
	auto bare = fresh(s);
	const std::vector<float> want = render(*bare, in, 300);

	Device lazy(s);
	CHECK(dl::feedbackLaneOf(lazy.mem) == nullptr);
	const std::vector<float> unbuilt = lazy.render(in);
	CHECK(dl::feedbackLaneOf(lazy.mem) == nullptr);
	CHECK(std::memcmp(unbuilt.data(), want.data(), want.size() * sizeof(float)) == 0);

	Device built(s);
	(void)built.lane();
	CHECK(dl::feedbackLaneOf(built.mem) != nullptr);
	const std::vector<float> empty = built.render(in);
	CHECK(std::memcmp(empty.data(), want.data(), want.size() * sizeof(float)) == 0);
}

TEST_CASE("FB (holder): Bus Utility in the lane scales each LATER repeat by its gain, and the first not at all") {
	Device dev(wetOnly(0.5f));
	const DeviceInstanceId util = dev.add(fx::busutil::kDescriptor.traits.tag);
	const size_t gain = controlNamed(fx::busutil::kDescriptor, "Bus Gain");
	dev.lane().setField(util, gain, 0.4f);
	const float g = fx::busutil::faderGain(0.4f);
	(void)dev.render(std::vector<float>(600, 0.0f)); // the drain installs the device
	const std::vector<float> out = dev.render(impulse(5 * kD + 10), 256);
	CHECK(out[kD] == 1.0f);
	for (size_t k = 2; k <= 4; ++k) {
		CAPTURE(k);
		const auto want = static_cast<float>(std::pow(0.5 * g, static_cast<double>(k - 1)));
		CHECK(std::fabs(out[k * kD] - want) <= 2.0e-3f);
	}
}

TEST_CASE("FB (holder): a LATENT device in the lane — CLIP at 4x — still puts every repeat on the grid") {
	Device dev(wetOnly(0.5f));
	const DeviceInstanceId clip = dev.add(fx::clip::kDescriptor.traits.tag);
	dev.lane().setField(clip, controlNamed(fx::clip::kDescriptor, "Clip Oversample"), 1.0f);
	(void)dev.render(std::vector<float>(600, 0.0f));
	REQUIRE(dl::feedbackLaneOf(dev.mem)->latencySamples(kRate) == fx::clip::kOversampleLatency);
	const std::vector<float> out = dev.render(impulse(4 * kD + 10), 256);
	for (size_t k = 1; k <= 3; ++k) {
		CAPTURE(k);
		CHECK(peakIn(out, k * kD - kD / 2, k * kD + kD / 2) == k * kD);
	}
	// THE DEVICE'S OWN LATENCY STAYS ZERO: the lane's is absorbed inside the loop.
	CHECK(dev.audio.latencySamples == nullptr);
}

TEST_CASE("FB (holder): the body carries the lane — a body with a device in it builds the lane in a FRESH instance, "
          "and one without leaves it unbuilt") {
	const dl::Settings s = wetOnly(0.5f);
	std::vector<uint8_t> withDevice;
	{
		Device dev(s);
		const DeviceInstanceId util = dev.add(fx::busutil::kDescriptor.traits.tag);
		dev.lane().setField(util, controlNamed(fx::busutil::kDescriptor, "Bus Gain"), 0.3f);
		(void)dev.render(std::vector<float>(600, 0.0f));
		withDevice = dev.save();
	}
	{
		Device fresh2(s);
		REQUIRE(fresh2.container.loadState(fresh2.mem, withDevice.data(), withDevice.size()));
		rack::InnerChain* lane = dl::feedbackLaneOf(fresh2.mem);
		REQUIRE(lane != nullptr);
		(void)fresh2.render(std::vector<float>(600, 0.0f));
		CHECK(lane->engine().chainLength(0) == 1);
		// BY VALUE: the loaded gain is the saved gain.
		const std::vector<float> out = fresh2.render(impulse(3 * kD), 256);
		CHECK(std::fabs(out[2 * kD] - 0.5f * fx::busutil::faderGain(0.3f)) <= 2.0e-3f);
	}
	std::vector<uint8_t> unbuilt;
	{
		Device dev(s);
		unbuilt = dev.save();
	}
	Device fresh3(s);
	REQUIRE(fresh3.container.loadState(fresh3.mem, unbuilt.data(), unbuilt.size()));
	CHECK(dl::feedbackLaneOf(fresh3.mem) == nullptr);
}

TEST_CASE("FB (holder): a reverb in the lane keeps the device awake until the OUTPUT is exactly silent") {
	Device dev(wetOnly(0.3f));
	(void)dev.add("FVERB");
	(void)dev.render(std::vector<float>(600, 0.0f));
	REQUIRE(dl::feedbackLaneOf(dev.mem)->engine().chainLength(0) == 1);
	std::vector<float> l(256), r(256);
	bool fell = false;
	bool sounded = false;
	for (int block = 0; block < 48000 * 90 / 256; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		if (block < 20) {
			l[0] = r[0] = 0.5f;
		}
		dev.audio.processStereo(dev.mem, l.data(), r.data(), 256);
		for (size_t i = 0; i < 256; ++i) {
			sounded = sounded || l[i] != 0.0f;
			if (fell) {
				REQUIRE(l[i] == 0.0f);
				REQUIRE(r[i] == 0.0f);
			}
		}
		if (!fell && block > 20 && !dev.audio.holdsTail(dev.mem)) {
			fell = true;
		}
	}
	CHECK(sounded);
	CHECK(fell);
}

TEST_CASE("FB (holder): a lane that holds sound its OUTPUT has not yet let out keeps the device awake — a second Delay "
          "inside, wet only, 1 s") {
	// THE LINE GOES QUIET LONG BEFORE THE INNER DELAY SPEAKS: the outer repeat enters the lane at 100 ms and the inner
	// line holds it, silently, for a second. Only the lane's own `holdsTail` knows — so this is the case for the OR.
	Device dev(wetOnly(0.1f));
	const DeviceInstanceId inner = dev.add(dl::kTraits.tag);
	dev.lane().setField(inner, controlNamed(dl::kDescriptor, "Delay Sync"), 2.0f); // 1 s
	dev.lane().setField(inner, controlNamed(dl::kDescriptor, "Delay Mix"), 1.0f);
	dev.lane().setField(inner, controlNamed(dl::kDescriptor, "Delay Feedback"), 0.0f);
	(void)dev.render(std::vector<float>(600, 0.0f));
	std::vector<float> l(256), r(256);
	bool fell = false;
	bool late = false;
	for (int block = 0; block < 48000 * 60 / 256; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		if (block == 0) {
			l[0] = r[0] = 0.5f;
		}
		dev.audio.processStereo(dev.mem, l.data(), r.data(), 256);
		for (size_t i = 0; i < 256; ++i) {
			late = late || (block * 256 > 48000 && l[i] != 0.0f);
			if (fell) {
				REQUIRE(l[i] == 0.0f);
				REQUIRE(r[i] == 0.0f);
			}
		}
		if (!fell && block > 0 && !dev.audio.holdsTail(dev.mem)) {
			fell = true;
		}
	}
	// Not vacuous: something came out after the first second, which is the inner Delay speaking.
	CHECK(late);
	CHECK(fell);
}

TEST_CASE("FB (S3): the lane's pass gain is measured and published, and the published loop gain is Feedback times it") {
	for (const float g : { 0.25f, 0.8f, 1.6f }) {
		CAPTURE(g);
		dl::Settings s = wetOnly(0.5f);
		auto d = fresh(s);
		struct Sink {
			std::vector<float> last;
		} sink;
		d->setReadingsSink({ &sink, [](void* ctx, const float* v, size_t n) {
			                    static_cast<Sink*>(ctx)->last.assign(v, v + n);
		                    } });
		GainLane lane { g };
		d->setLoopInsert(lane.insert());
		(void)render(*d, burst(48000, 48000, 0.3f), 256);
		CHECK(d->lanePassForTest() == doctest::Approx(g).epsilon(1e-4));
		REQUIRE(sink.last.size() == dl::kReadingCount);
		CHECK(sink.last[dl::reading::kLanePass] == doctest::Approx(g).epsilon(1e-4));
		CHECK(sink.last[dl::reading::kFeedback] == doctest::Approx(0.5f * g).epsilon(1e-4));
		CHECK(sink.last[dl::reading::kTapFloor] == doctest::Approx(dl::kLoopChunkMin / kRate).epsilon(1e-6));
	}
	// WITHOUT A LANE: unity, and no floor — the readings the card drew before the lane existed.
	auto bare = fresh(wetOnly(0.5f));
	std::vector<float> last;
	bare->setReadingsSink({ &last, [](void* ctx, const float* v, size_t n) {
		                       static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                       } });
	(void)render(*bare, burst(4800, 4800, 0.3f), 256);
	REQUIRE(last.size() == dl::kReadingCount);
	CHECK(last[dl::reading::kLanePass] == 1.0f);
	CHECK(last[dl::reading::kFeedback] == 0.5f);
	CHECK(last[dl::reading::kTapFloor] == 0.0f);
}

//--------------------------------------------------------------------------------------------------//
// S4: THE DUCK, THE FREEZE, THE CLEAR                                                                //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A LOUD STEADY HALF SECOND (DC at 0.5, so the follower sits still), then silence.
std::vector<float> steadyThenSilence(size_t frames, size_t loud) {
	std::vector<float> v(frames, 0.0f);
	std::fill(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(std::min(loud, frames)), 0.5f);
	return v;
}

} // namespace

TEST_CASE("S4 Duck: right of centre ducks only what is HEARD, by its amount, while the input plays — and the line is "
          "untouched, so the repeats come back whole") {
	dl::Settings s = wetOnly(0.6f);
	const std::vector<float> in = steadyThenSilence(4 * 48000, 24000);
	auto plain = fresh(s);
	const std::vector<float> ref = render(*plain, in, 256);
	s.duck = 0.8f;
	auto ducked = fresh(s);
	const std::vector<float> out = render(*ducked, in, 256);
	// WHILE THE INPUT PLAYS (after the 5 ms attack, at full engagement): the heard wet is 1 − 0.8 of what it was.
	for (size_t i = 6000; i < 24000; i += 97) {
		CAPTURE(i);
		CHECK(out[i] == doctest::Approx(0.2f * ref[i]).epsilon(1e-5));
	}
	// LONG AFTER IT STOPS: exactly the undocked device — the repeats were in the line all along.
	for (size_t i = 2 * 48000; i < out.size(); ++i) {
		REQUIRE(out[i] == ref[i]);
	}
	CHECK(ducked->duckGainForTest() == 1.0f);
}

TEST_CASE("S4 Duck: left of centre GATES — the repeats sound only while the input plays") {
	dl::Settings s = wetOnly(0.6f);
	s.duck = -1.0f;
	const std::vector<float> in = steadyThenSilence(3 * 48000, 24000);
	auto plain = fresh(wetOnly(0.6f));
	const std::vector<float> ref = render(*plain, in, 256);
	auto gated = fresh(s);
	const std::vector<float> out = render(*gated, in, 256);
	// WHILE IT PLAYS, fully open: what the plain device plays.
	for (size_t i = 6000; i < 24000; i += 97) {
		CHECK(out[i] == doctest::Approx(ref[i]).epsilon(1e-5));
	}
	// ONCE THE FOLLOWER HAS LET GO (Release 250 ms, to the silence floor), the repeats still in the line are not heard.
	bool refHasRepeats = false;
	for (size_t i = 2 * 48000; i < out.size(); ++i) {
		refHasRepeats = refHasRepeats || ref[i] != 0.0f;
		REQUIRE(out[i] == 0.0f);
	}
	CHECK(refHasRepeats);
}

TEST_CASE("S4 Freeze: the loop holds to the bit for ever — the output repeats every tap exactly, and new input does "
          "not get in") {
	for (const bool withLane : { false, true }) {
		CAPTURE(withLane);
		dl::Settings s = wetOnly(0.3f);
		auto d = fresh(s);
		GainLane killer { 0.0f }; // a lane that would kill every repeat: frozen, it is out of the loop (rule 6)
		if (withLane) {
			d->setLoopInsert(killer.insert());
		}
		std::vector<float> l(256), r(256);
		std::vector<float> out;
		uint32_t seed = 9;
		const auto block = [&](bool noise) {
			for (size_t i = 0; i < 256; ++i) {
				seed = seed * 1664525u + 1013904223u;
				l[i] = r[i] = noise ? 0.4f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) : 0.0f;
			}
			d->processStereo(l.data(), r.data(), 256);
			out.insert(out.end(), l.begin(), l.end());
		};
		for (int b = 0; b < 48000 / 256; ++b) {
			block(true);
		}
		s.freeze = true;
		d->setTarget(s);
		const size_t frozenAt = out.size();
		// INPUT KEEPS COMING — loud — and must not get in.
		for (int b = 0; b < 3 * 48000 / 256; ++b) {
			block(true);
		}
		REQUIRE(d->frozenTapForTest(0) == kD);
		CHECK(d->freezeForTest() == 1.0f);
		const size_t from = frozenAt + 480 + 2 * kD; // past the ramp and two whole laps
		bool sounding = false;
		for (size_t i = from; i < out.size(); ++i) {
			REQUIRE(out[i] == out[i - kD]);
			sounding = sounding || out[i] != 0.0f;
		}
		CHECK(sounding);
		CHECK(d->holdsTail());
		// UNFROZEN AND SILENT, IT DECAYS AGAIN AND REACHES EXACT ZERO.
		s.freeze = false;
		d->setTarget(s);
		bool fell = false;
		for (int b = 0; b < 40 * 48000 / 256 && !fell; ++b) {
			block(false);
			fell = !d->holdsTail();
		}
		CHECK(fell);
	}
}

TEST_CASE("S4 Freeze: a Delay that LOADS frozen holds an EMPTY loop — the switch persists, the audio does not") {
	dl::Settings s = wetOnly(0.5f);
	s.freeze = true;
	// THROUGH THE ROW, as a load arrives: the switch is a field like any other.
	const dl::Settings loaded = dl::stateOf(dl::rowOf(dl::State { s }), static_cast<size_t>(dl::Param::kCount)).settings;
	REQUIRE(loaded.freeze);
	auto d = fresh(loaded);
	const std::vector<float> out = render(*d, burst(48000, 48000, 0.5f), 256);
	for (const float v : out) {
		REQUIRE(v == 0.0f);
	}
}

TEST_CASE("S4 Clear: a rising edge empties the line within the fade, a held level clears once, and a load with it held "
          "clears nothing") {
	dl::Settings s = wetOnly(0.9f);
	auto d = fresh(s);
	(void)render(*d, burst(48000, 24000, 0.5f), 256);
	CHECK(d->holdsTail());
	s.clear = true;
	d->setTarget(s);
	const std::vector<float> after = render(*d, std::vector<float>(48000, 0.0f), 256);
	CHECK(d->clearsForTest() == 1);
	// SILENT FROM THE FADE'S END: 5 ms, plus the block in which the line is emptied.
	for (size_t i = 240 + 256; i < after.size(); ++i) {
		REQUIRE(after[i] == 0.0f);
	}
	CHECK_FALSE(d->holdsTail());
	// HELD HIGH, NO SECOND CLEAR; LOW AND HIGH AGAIN, ONE MORE.
	(void)render(*d, burst(24000, 24000, 0.5f), 256);
	CHECK(d->clearsForTest() == 1);
	s.clear = false;
	d->setTarget(s);
	(void)render(*d, std::vector<float>(256, 0.0f), 256);
	s.clear = true;
	d->setTarget(s);
	(void)render(*d, std::vector<float>(1024, 0.0f), 256);
	CHECK(d->clearsForTest() == 2);
	// A PATCH SAVED WITH CLEAR HIGH: the first block takes the level as it finds it.
	dl::Settings held = wetOnly(0.9f);
	held.clear = true;
	auto e = fresh(held);
	(void)render(*e, burst(48000, 24000, 0.5f), 256);
	CHECK(e->clearsForTest() == 0);
	CHECK(e->holdsTail());
}

//--------------------------------------------------------------------------------------------------//
// S6b: SYNC = NOTE, AND FEEDBACK TO 200%                                                             //
//--------------------------------------------------------------------------------------------------//

namespace {

device::NoteContext held(float pitch, bool gate = true) {
	device::NoteContext n;
	n.pitchSemitones = pitch;
	n.gate = gate;
	return n;
}

dl::Settings noteSynced(float feedback, double multiple = 1.0) {
	dl::Settings s = wetOnly(feedback);
	s.sync = dl::kSyncNoteChoice;
	s.timeMultiple = multiple;
	return s;
}

} // namespace

TEST_CASE("S6b Note: the tap is the held note's period times Time — the tempo plays no part") {
	REQUIRE(std::string_view(dl::kSyncNames[dl::kSyncNoteChoice]) == "Note");
	for (const double multiple : { 1.0, 2.0, 0.5 }) {
		CAPTURE(multiple);
		auto d = fresh(noteSynced(0.5f, multiple));
		d->setTransportClock(device::TransportClock::fromSecondsPerBeat(0.37));
		d->setNoteContext(held(57.0f)); // A3, 220 Hz
		(void)render(*d, impulse(512), 256);
		CHECK(d->tapSecondsForTest() == doctest::Approx(multiple / 220.0).epsilon(1e-9));
		CHECK(d->headSecondsForTest(0) == doctest::Approx(multiple / 220.0).epsilon(1e-9));
	}
	// BEFORE ANY NOTE: middle C.
	auto d = fresh(noteSynced(0.5f));
	(void)render(*d, impulse(512), 256);
	CHECK(d->tapSecondsForTest() == doctest::Approx(dl::notePeriodSeconds(dl::kDefaultNotePitch)).epsilon(1e-9));
}

TEST_CASE("S6b Note: at high Feedback the impulse rings at the note — its repeats are one period apart") {
	auto d = fresh(noteSynced(0.95f));
	d->setNoteContext(held(57.0f));
	const std::vector<float> out = render(*d, impulse(48000), 256);
	const double period = 48000.0 / 220.0; // 218.18 frames
	for (int k = 1; k <= 40; k += 13) {
		CAPTURE(k);
		const auto at = static_cast<size_t>(std::lround(period * k));
		const size_t found = peakIn(out, at - 20, at + 20);
		CHECK(std::abs(static_cast<long>(found) - static_cast<long>(at)) <= 1);
	}
}

TEST_CASE("S6b Note: letting go keeps the last note's period; a note past the floor sits on it") {
	auto d = fresh(noteSynced(0.5f));
	d->setNoteContext(held(57.0f));
	d->setNoteContext(held(80.0f, false)); // released: the pitch it reports is not taken
	CHECK(d->notePitchForTest() == 57.0f);
	(void)render(*d, impulse(512), 256);
	CHECK(d->tapSecondsForTest() == doctest::Approx(1.0 / 220.0).epsilon(1e-9));
	// C#7 is a 0.45 ms period — under the 1 ms floor, so the tap sits on the floor.
	d->setNoteContext(held(97.0f));
	(void)render(*d, impulse(512), 256);
	CHECK(d->tapSecondsForTest() == dl::kMinDelaySeconds);
	// AND RESET FORGETS IT (the Phaser's key-track rule).
	d->reset();
	CHECK(d->notePitchForTest() == dl::kDefaultNotePitch);
}

TEST_CASE("S6b Note (holder): the device's note road reaches the DSP") {
	Device dev(noteSynced(0.5f));
	dev.audio.setNoteContext(dev.mem, held(64.0f));
	CHECK(dl::dspOf(dev.mem).notePitchForTest() == 64.0f);
}

TEST_CASE("S6b Feedback to 200%: past 110% the loop limiter holds it bounded and finite; it sustains (and says so), "
          "and with nothing ever in it stays exactly silent") {
	REQUIRE(dl::kParamDescs[static_cast<size_t>(dl::Param::Feedback)].max == 2.0);
	auto d = fresh(wetOnly(2.0f));
	const std::vector<float> out = render(*d, burst(6 * 48000, 4800, 0.5f), 256);
	float late = 0.0f;
	for (size_t i = 2 * 48000; i < out.size(); ++i) {
		REQUIRE(std::isfinite(out[i]));
		late = std::max(late, std::fabs(out[i]));
	}
	CHECK(late > 0.5f);                                  // it sustains
	CHECK(late <= dl::kLoopLimitCeiling * 1.0005f);      // at the ceiling, not past it
	CHECK(d->loopLimitGainForTest() < 0.75f);            // because the limiter is holding it
	CHECK(d->holdsTail());                               // a sustaining device stays awake (CLAUDE.md, rule 3)
	auto quiet = fresh(wetOnly(2.0f));
	const std::vector<float> none = render(*quiet, std::vector<float>(48000, 0.0f), 256);
	for (const float v : none) {
		REQUIRE(v == 0.0f);
	}
	CHECK_FALSE(quiet->holdsTail());
}

TEST_CASE("FB: AT THE FLOOR (L + 16), under carried chunks and any host block, the MAIN repeat carries the lane's "
          "feedback exactly once per pass, by value") {
	// THE LEAD'S CONDITION (2): the main repeat must never miss feedback. Repeat k of an impulse must be (f·g)^(k−1)
	// exactly where it lands — a pass that missed the lane's output would read 0 there, one that took it twice (f·g)^k.
	for (const size_t latency : { size_t { 0 }, size_t { 37 }, size_t { 500 } }) {
		for (const int block : { 1, 7, 64, 300, 512 }) {
			CAPTURE(latency);
			CAPTURE(block);
			dl::Settings s = wetOnly(0.8f);
			s.sync = 0;            // 10 ms
			s.timeMultiple = 0.25; // 2.5 ms = 120 frames, under every floor below but L = 0's (16): clamped there
			auto d = fresh(s);
			LateLane lane(latency, 0.75f);
			d->setLoopInsert(lane.insert());
			const size_t floorFrames = std::max<size_t>(latency + static_cast<size_t>(dl::kLoopChunkMin), 120);
			const std::vector<float> out = render(*d, impulse(10 * floorFrames + 64), block);
			REQUIRE(d->headSecondsForTest(0) * kRate == doctest::Approx(static_cast<double>(floorFrames)).epsilon(1e-9));
			for (size_t k = 1; k <= 8; ++k) {
				CAPTURE(k);
				const auto want = static_cast<float>(std::pow(0.8 * 0.75, static_cast<double>(k - 1)));
				CHECK(std::fabs(out[k * floorFrames] - want) <= 2.0e-3f);
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// BBD AND SWARM (2026-11-05: "go bbd and swarm as a setting on grain")                               //
//--------------------------------------------------------------------------------------------------//

namespace {

dl::Settings bbdBare(size_t sync, double multiple, int stages = 4096) {
	dl::Settings s = wetOnly(0.0f);
	s.mode = dl::Mode::Bbd;
	s.sync = sync;
	s.timeMultiple = multiple;
	s.bbdStages = stages;
	s.bbdCompander = 0.0f;
	s.bbdNoise = 0.0f;
	s.bbdDrive = 0.0f;
	return s;
}

std::vector<float> sine(double hz, size_t frames, float amp) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = amp * static_cast<float>(std::sin(6.283185307179586 * hz * static_cast<double>(i) / kRate));
	}
	return v;
}

double rmsOf(const std::vector<float>& v, size_t from, size_t to) {
	double a = 0.0;
	for (size_t i = from; i < to; ++i) {
		a += static_cast<double>(v[i]) * v[i];
	}
	return std::sqrt(a / static_cast<double>(to - from));
}

/// THE FIRST REPEAT'S GAIN FOR A STEADY SINE, dB: wet-only, no feedback, so the second half of the render is the sine
/// through the chip once.
double bbdGainDb(const dl::Settings& s, double hz) {
	auto d = fresh(s);
	const double seconds = std::max(0.02, s.timeMultiple * (s.sync == 0 ? 0.01 : s.sync == 1 ? 0.1 : 1.0));
	const auto frames = static_cast<size_t>((seconds + 0.5) * kRate);
	const std::vector<float> in = sine(hz, frames, 0.25f);
	const std::vector<float> out = render(*d, in, 256);
	const size_t from = frames - static_cast<size_t>(0.3 * kRate);
	return 20.0 * std::log10(rmsOf(out, from, frames) / rmsOf(in, from, frames));
}

} // namespace

TEST_CASE("BBD: the clock and the corner are the formula's — f_clk = N / 2D and f_c = min(0.8 f_clk / 2, 0.45 sr) — "
          "Time by Time and Stages by Stages") {
	struct Row {
		size_t sync;
		double multiple;
		double seconds;
	};
	for (const Row row : { Row { 0, 2.0, 0.02 }, Row { 1, 1.0, 0.1 }, Row { 1, 4.0, 0.4 }, Row { 2, 1.0, 1.0 } }) {
		for (const int stages : { 1024, 4096, 8192 }) {
			CAPTURE(row.seconds);
			CAPTURE(stages);
			auto d = fresh(bbdBare(row.sync, row.multiple, stages));
			(void)render(*d, impulse(512), 256);
			const double clock = stages / (2.0 * row.seconds); // restated, not called
			const double corner = std::min(0.8 * clock / 2.0, 0.45 * kRate);
			CHECK(d->bbdClockForTest(0) == doctest::Approx(clock).epsilon(1e-9));
			CHECK(d->bbdCutoffForTest(0) == doctest::Approx(corner).epsilon(1e-9));
		}
	}
}

TEST_CASE("BBD: the bandwidth falls as Time grows — a 2 kHz sine through the chip, measured, against the corner") {
	// 4096 stages: 100 ms leaves the corner at 8192 Hz (2 kHz passes), 400 ms at 2048 Hz (2 kHz sits ON it: two
	// Butterworths, -3 dB each, and the hold's droop), 1 s at 819 Hz (well past it).
	const double at100 = bbdGainDb(bbdBare(1, 1.0), 2000.0);
	const double at400 = bbdGainDb(bbdBare(1, 4.0), 2000.0);
	const double at1s = bbdGainDb(bbdBare(2, 1.0), 2000.0);
	CAPTURE(at100);
	CAPTURE(at400);
	CAPTURE(at1s);
	CHECK(at100 > -1.0);
	CHECK(at400 < -4.5);
	CHECK(at400 > -8.5);
	CHECK(at1s < -15.0);
	// AND MORE STAGES ARE BRIGHTER AT THE SAME TIME: 8192 at 400 ms is 4096 at 200 ms.
	CHECK(bbdGainDb(bbdBare(1, 4.0, 8192), 2000.0) > at400 + 3.0);
	// A SINE WELL UNDER THE CORNER PASSES WHOLE at any length (the anti-alias, the hold and the reconstruction all flat).
	CHECK(bbdGainDb(bbdBare(2, 1.0), 150.0) > -1.0);
}

TEST_CASE("BBD: moving Time re-clocks the chip — the head GLIDES, so the pitch bends, and the clock follows it") {
	dl::Settings s = bbdBare(1, 1.0);
	auto d = fresh(s);
	(void)render(*d, sine(440.0, 9600, 0.25f), 256);
	const double before = d->headSecondsForTest(0);
	REQUIRE(before == doctest::Approx(0.1));
	s.timeMultiple = 2.0;
	d->setTarget(s);
	(void)render(*d, sine(440.0, 1024, 0.25f), 256);
	const double moving = d->headSecondsForTest(0);
	CHECK(moving > before);
	CHECK(moving < 0.2);
	CHECK(d->bbdClockForTest(0) < 4096.0 / (2.0 * before));
	CHECK(d->bbdClockForTest(0) > 4096.0 / (2.0 * 0.2));
}

TEST_CASE("BBD: the compander restores the level it squeezed — a quiet steady tone comes back at its own level") {
	for (const float c : { 0.5f, 1.0f }) {
		CAPTURE(c);
		dl::Settings s = bbdBare(1, 1.0);
		s.bbdCompander = c;
		const double with = bbdGainDb(s, 300.0);
		const double without = bbdGainDb(bbdBare(1, 1.0), 300.0);
		CHECK(std::fabs(with - without) < 1.5);
	}
}

TEST_CASE("BBD: the noise rides the signal — heard while it sounds, and the tail still reaches EXACT zero") {
	dl::Settings s = bbdBare(2, 1.0); // 1 s: the clock is 2048 Hz, its whine under the corner
	s.feedback = 0.5f;
	s.bbdNoise = 1.0f;
	s.bbdCompander = 0.5f;
	s.bbdDrive = 0.3f;
	auto noisy = fresh(s);
	dl::Settings quietS = s;
	quietS.bbdNoise = 0.0f;
	auto quiet = fresh(quietS);
	const std::vector<float> in = burst(48000 * 40, 48000, 0.25f);
	const std::vector<float> a = render(*noisy, in, 256);
	const std::vector<float> b = render(*quiet, in, 256);
	double diff = 0.0;
	for (size_t i = 48000; i < 3 * 48000; ++i) {
		diff += std::fabs(static_cast<double>(a[i]) - b[i]);
	}
	CHECK(diff > 0.0); // the noise is there while the repeats are
	// AND ONCE THE REPEATS ARE GONE, SO IS THE NOISE: the device falls silent, exactly, and says so.
	CHECK_FALSE(noisy->holdsTail());
	for (size_t i = a.size() - 48000; i < a.size(); ++i) {
		REQUIRE(a[i] == 0.0f);
	}
}

//--------------------------------------------------------------------------------------------------//
// BUCKETS' COST LEVERS (backlog PV)                                                                //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("PV: the whine's phasor stays on sin(2π·phase) — ten minutes of blocks re-anchored, and the device's own after "
          "odd host blocks") {
	// THE HELPER, AS THE DEVICE DRIVES IT: anchored at each block's exact phase, stepped a frame at a time, the phase
	// wrapped by whole cycles. Ten minutes at 48 kHz in 1024-frame blocks, every frame checked against the library sine.
	{
		const double ratio = 2048.0 / kRate + 1.0e-7; // a 1 s Time at 4096 stages, nudged off any rational period
		double phase = 0.0;
		double worst = 0.0;
		double worstNorm = 0.0;
		dl::WhineRotor rotor;
		for (int block = 0; block < 600 * 48000 / 1024; ++block) {
			rotor.anchor(phase, ratio);
			for (int i = 0; i < 1024; ++i) {
				phase += ratio;
				if (phase >= 1.0) {
					phase -= std::floor(phase);
				}
				const double s = rotor.step();
				worst = std::max(worst, std::fabs(s - std::sin(6.283185307179586 * phase)));
			}
			worstNorm = std::max(worstNorm, std::fabs(rotor.s * rotor.s + rotor.c * rotor.c - 1.0));
		}
		CAPTURE(worst);
		CHECK(worst < 1.0e-9);
		CHECK(worstNorm < 1.0e-12);
	}
	// THE DEVICE'S PHASOR AFTER HOST BLOCKS OF 333 FRAMES: the whine on (1 s, 4096 stages, a 2048 Hz clock), both sides.
	dl::Settings s = bbdBare(2, 1.0);
	s.feedback = 0.5f;
	s.bbdNoise = 1.0f;
	s.bbdCompander = 0.5f;
	auto d = fresh(s);
	(void)render(*d, burst(48000 * 3 + 17, 48000, 0.25f), 333);
	for (const int c : { 0, 1 }) {
		CAPTURE(c);
		const double phase = d->bbdPhaseForTest(c);
		CHECK(phase > 0.0);
		CHECK(std::fabs(d->bbdWhineForTest(c) - std::sin(6.283185307179586 * phase)) < 1.0e-9);
	}
}

TEST_CASE("PV: Buckets hands the lane chunks of up to 256 where Time's floor allows, and its repeats land where the "
          "loop without a lane puts them, Time by Time down to the floor") {
	constexpr size_t kLatency = 37;
	// From just over the floor (37 + 16 = 53) to 100 ms: the chunk the floor allows is min(256, ⌊(D − 37) / 1.5⌋).
	for (const size_t frames : { size_t { 60 }, size_t { 120 }, size_t { 200 }, size_t { 400 }, size_t { 1000 }, kD }) {
		CAPTURE(frames);
		dl::Settings s = bbdBare(0, static_cast<double>(frames) / 480.0); // sync 0 is 10 ms = 480 frames
		s.feedback = 0.7f;
		auto laned = fresh(s);
		LateLane lane(kLatency);
		laned->setLoopInsert(lane.insert());
		auto bare = fresh(s);
		const std::vector<float> in = impulse(5 * frames + 64);
		const std::vector<float> a = render(*laned, in, 512);
		const std::vector<float> b = render(*bare, in, 512);
		const int room = static_cast<int>(std::floor(static_cast<double>(frames - kLatency) / 1.5)); // restated
		CHECK(laned->laneChunkMaxForTest() == std::min(256, room));
		for (size_t k = 1; k <= 4; ++k) {
			CAPTURE(k);
			const size_t from = k * frames - frames / 2;
			const size_t to = k * frames + frames / 2;
			const size_t where = peakIn(b, from, to);
			CHECK(peakIn(a, from, to) == where);
			CHECK(std::fabs(b[where]) > 0.01f); // a repeat is there to find
		}
	}
	// AND EVERY OTHER MODE KEEPS THE TICK: Clean at 100 ms is handed 64.
	auto clean = fresh(wetOnly(0.5f));
	GainLane g { 0.5f };
	clean->setLoopInsert(g.insert());
	(void)render(*clean, burst(4800, 480, 0.5f), 512);
	CHECK(clean->laneChunkMaxForTest() == dl::kLoopChunkMax);
}

TEST_CASE("Swarm: each grain is thrown in time, pan and pitch by a uniform draw over ±amount — by value") {
	for (const float amount : { 1.0f, 0.5f }) {
		CAPTURE(amount);
		dl::Settings s = wetOnly(0.4f);
		s.mode = dl::Mode::Granular;
		s.spray = 0.0f;
		s.swarm = amount;
		auto d = fresh(s);
		(void)render(*d, burst(10 * 48000, 10 * 48000, 0.3f), 256);
		const size_t n = d->swarmDrawCountForTest();
		REQUIRE(n >= 400);
		const double spreadMax = amount * dl::kSwarmSpreadSeconds * kRate;
		double m[3] = {};
		double v[3] = {};
		for (size_t i = 0; i < n; ++i) {
			const double x[3] = { d->swarmOffsetForTest(i) / spreadMax, d->swarmPanForTest(i) / amount,
				                  d->swarmSemitonesForTest(i) / (amount * dl::kSwarmPitchSemitones) };
			for (int k = 0; k < 3; ++k) {
				REQUIRE(std::fabs(x[k]) <= 1.0 + 1e-6);
				m[k] += x[k];
				v[k] += x[k] * x[k];
			}
		}
		for (int k = 0; k < 3; ++k) {
			CAPTURE(k);
			const double mean = m[k] / static_cast<double>(n);
			const double sd = std::sqrt(v[k] / static_cast<double>(n) - mean * mean);
			CHECK(std::fabs(mean) < 0.08);
			// UNIFORM OVER ±1: a standard deviation of 1/sqrt(3).
			CHECK(sd == doctest::Approx(1.0 / std::sqrt(3.0)).epsilon(0.08));
		}
		// AND MORE OF THEM: the density goes up by 1 + 3·Swarm (20/s by default), over the ten seconds.
		CHECK(static_cast<double>(d->swarmSerialForTest())
		      == doctest::Approx(10.0 * 20.0 * (1.0 + dl::kSwarmDensityGain * amount)).epsilon(0.05));
	}
}

TEST_CASE("Swarm: deterministic — two fresh Delays draw the same cloud and render the same bits; at 0 it draws nothing") {
	dl::Settings s = wetOnly(0.5f);
	s.mode = dl::Mode::Granular;
	s.swarm = 0.7f;
	auto a = fresh(s);
	auto b = fresh(s);
	const std::vector<float> in = burst(4 * 48000, 48000, 0.3f);
	const std::vector<float> x = render(*a, in, 256);
	const std::vector<float> y = render(*b, in, 333);
	CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) == 0);
	REQUIRE(a->swarmDrawCountForTest() == b->swarmDrawCountForTest());
	for (size_t i = 0; i < a->swarmDrawCountForTest(); ++i) {
		REQUIRE(a->swarmOffsetForTest(i) == b->swarmOffsetForTest(i));
	}
	s.swarm = 0.0f;
	auto off = fresh(s);
	(void)render(*off, in, 256);
	CHECK(off->swarmSerialForTest() == 0);
}

//------------------------------------------------------------------------------------------------//
// SWARM IS THE SONG'S WHILE THE TRANSPORT PLAYS (2026-11-05; CLAUDE.md, "determinism comes first")  //
//------------------------------------------------------------------------------------------------//

namespace {

constexpr int64_t kSongFrame0 = 480000; // ten seconds into the song

dl::Settings swarmed() {
	dl::Settings s = wetOnly(0.5f);
	s.mode = dl::Mode::Granular;
	s.swarm = 0.7f;
	s.spray = 0.5f;
	// A 400 ms TAP AND ~12 GRAINS SOUNDING AT ONCE: dense enough that the ORDER the grains are summed in moves bits (two
	// terms commute, three do not), and short enough (95 ms) that a grain born in a silent pre-roll dies reading silence.
	s.timeMultiple = 4.0;
	s.grainSeconds = 0.2;
	s.densityHz = 40.0;
	return s;
}

/// RENDER `in` STARTING AT SONG FRAME `songFrame`, the host's clock set before every block as a host sets it.
std::vector<float> renderAtSong(dl::Delay& d, const std::vector<float>& in, int block, int64_t songFrame, bool playing,
                                std::vector<float>* rightOut = nullptr) {
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t at = 0; at < l.size(); at += static_cast<size_t>(block)) {
		device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
		clock.playing = playing;
		clock.songPositionSeconds = static_cast<double>(songFrame + static_cast<int64_t>(at)) / kRate;
		clock.songPositionBeats = clock.songPositionSeconds / 0.5;
		d.setTransportClock(clock);
		const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), l.size() - at));
		d.processStereo(l.data() + at, r.data() + at, n);
	}
	if (rightOut != nullptr) {
		*rightOut = r;
	}
	return l;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("Swarm on a playing transport: the same song position is the same cloud to the bit — whatever the host's "
          "block size, and however long, or whether, it ran before") {
	const dl::Settings s = swarmed();
	const std::vector<float> in = burst(3 * 48000, 24000, 0.3f);
	const std::vector<float> silence(65760, 0.0f); // 1.37 s

	auto a = fresh(s);
	std::vector<float> aR;
	const std::vector<float> aL = renderAtSong(*a, in, 256, kSongFrame0, true, &aR);

	// PLAYED UP TO IT: the transport rolling 1.37 s earlier, over silence, in odd blocks.
	auto b = fresh(s);
	(void)renderAtSong(*b, silence, 333, kSongFrame0 - static_cast<int64_t>(silence.size()), true);
	std::vector<float> bR;
	const std::vector<float> bL = renderAtSong(*b, in, 333, kSongFrame0, true, &bR);

	// AUDITIONED STOPPED FIRST (free-running, its own stream advanced), then play pressed at the position.
	auto c = fresh(s);
	(void)renderAtSong(*c, std::vector<float>(24000, 0.0f), 64, kSongFrame0, false);
	REQUIRE(c->swarmSerialForTest() > 0); // the stopped audition did spawn — its stream is not at its start
	std::vector<float> cR;
	const std::vector<float> cL = renderAtSong(*c, in, 512, kSongFrame0, true, &cR);

	// NOT VACUOUS: the cloud sounds.
	REQUIRE(rmsOf(aL, 4800, aL.size()) > 1e-3);
	CHECK(sameBits(aL, bL));
	CHECK(sameBits(aR, bR));
	CHECK(sameBits(aL, cL));
	CHECK(sameBits(aR, cR));
	// AND THE PANS SEPARATE THE CHANNELS — the right side is a cloud of its own, not a copy.
	CHECK_FALSE(sameBits(aL, aR));
}

TEST_CASE("Swarm on a playing transport: grain k is born where floor(song frame / spawn period) steps to k, and its "
          "draws are grain k's wherever the instance started — by value") {
	const dl::Settings s = swarmed();
	// THE SPAWN PERIOD, RESTATED: Swarm multiplies the density by 1 + 3·Swarm.
	const double period = kRate / (s.densityHz * (1.0 + dl::kSwarmDensityGain * static_cast<double>(s.swarm)));
	const auto index = [period](int64_t n) { return static_cast<int64_t>(std::floor(static_cast<double>(n) / period)); };
	const std::vector<float> in = burst(48000, 24000, 0.3f);
	const auto frames = static_cast<int64_t>(in.size());

	auto a = fresh(s);
	(void)renderAtSong(*a, in, 256, kSongFrame0, true);
	CHECK(a->songSpawnsForTest() == static_cast<uint64_t>(index(kSongFrame0 + frames - 1) - index(kSongFrame0 - 1)));
	CHECK(a->songLastIndexForTest() == index(kSongFrame0 + frames - 1));

	// AN INSTANCE STARTED 0.3 s EARLIER spawned `before` grains first; from there its draws ARE a's, grain for grain.
	const int64_t early = kSongFrame0 - 14400;
	auto e = fresh(s);
	(void)renderAtSong(*e, std::vector<float>(14400, 0.0f), 100, early, true);
	const auto before = static_cast<size_t>(e->swarmDrawCountForTest());
	CHECK(before == static_cast<size_t>(index(kSongFrame0 - 1) - index(early - 1)));
	(void)renderAtSong(*e, in, 100, kSongFrame0, true);
	REQUIRE(a->swarmDrawCountForTest() > 20);
	for (size_t j = 0; j < a->swarmDrawCountForTest() && before + j < e->swarmDrawCountForTest(); ++j) {
		CAPTURE(j);
		REQUIRE(e->swarmOffsetForTest(before + j) == a->swarmOffsetForTest(j));
		REQUIRE(e->swarmPanForTest(before + j) == a->swarmPanForTest(j));
		REQUIRE(e->swarmSemitonesForTest(before + j) == a->swarmSemitonesForTest(j));
	}

	// A DIFFERENT POSITION IS A DIFFERENT CLOUD: the same input 13.7 ms later in the song draws other grains.
	auto later = fresh(s);
	const std::vector<float> x = renderAtSong(*later, in, 256, kSongFrame0 + 658, true);
	auto again = fresh(s);
	const std::vector<float> y = renderAtSong(*again, in, 256, kSongFrame0, true);
	CHECK_FALSE(sameBits(x, y));
	CHECK(later->swarmPanForTest(0) != again->swarmPanForTest(0));

	// STOPPED, NOTHING LOCKS: the free-running road, as before.
	auto stopped = fresh(s);
	(void)renderAtSong(*stopped, in, 256, kSongFrame0, false);
	CHECK(stopped->songSpawnsForTest() == 0);
	CHECK(stopped->swarmSerialForTest() > 0);

}

TEST_CASE("Granular at Swarm 0 on a playing transport: the spray is the song's too — same position, same bits; grain k's "
          "spray is grain k's wherever the instance started; stopped it is the stream, as before — by value") {
	dl::Settings s = swarmed();
	s.swarm = 0.0f;
	const double period = kRate / s.densityHz; // no Swarm: the density as set
	const auto index = [period](int64_t n) { return static_cast<int64_t>(std::floor(static_cast<double>(n) / period)); };
	const std::vector<float> in = burst(3 * 48000, 24000, 0.3f);
	const auto frames = static_cast<int64_t>(in.size());

	auto a = fresh(s);
	std::vector<float> aR;
	const std::vector<float> aL = renderAtSong(*a, in, 256, kSongFrame0, true, &aR);
	REQUIRE(rmsOf(aL, 4800, aL.size()) > 1e-3);
	CHECK(a->swarmSerialForTest() == 0); // no Swarm draw taken
	CHECK(a->songSpawnsForTest() == static_cast<uint64_t>(index(kSongFrame0 + frames - 1) - index(kSongFrame0 - 1)));
	CHECK(a->songLastIndexForTest() == index(kSongFrame0 + frames - 1));

	// PLAYED UP TO IT over silence in odd blocks, and AUDITIONED STOPPED first: the same bits from the position.
	auto b = fresh(s);
	const int64_t early = kSongFrame0 - 65760;
	(void)renderAtSong(*b, std::vector<float>(65760, 0.0f), 333, early, true);
	const size_t before = b->sprayDrawCountForTest();
	CHECK(before == static_cast<size_t>(index(kSongFrame0 - 1) - index(early - 1)));
	std::vector<float> bR;
	const std::vector<float> bL = renderAtSong(*b, in, 333, kSongFrame0, true, &bR);
	auto c = fresh(s);
	(void)renderAtSong(*c, std::vector<float>(24000, 0.0f), 64, kSongFrame0, false);
	REQUIRE(c->sprayDrawCountForTest() > 0);
	std::vector<float> cR;
	const std::vector<float> cL = renderAtSong(*c, in, 512, kSongFrame0, true, &cR);
	CHECK(sameBits(aL, bL));
	CHECK(sameBits(aR, bR));
	CHECK(sameBits(aL, cL));
	CHECK(sameBits(aR, cR));

	// GRAIN k'S SPRAY IS GRAIN k'S: the earlier-started instance's draws from `before` on are a's, one for one.
	REQUIRE(a->sprayDrawCountForTest() > 20);
	for (size_t j = 0; j < a->sprayDrawCountForTest() && before + j < b->sprayDrawCountForTest(); ++j) {
		CAPTURE(j);
		REQUIRE(b->sprayDrawForTest(before + j) == a->sprayDrawForTest(j));
	}

	// A DIFFERENT POSITION IS A DIFFERENT SPRAY.
	auto later = fresh(s);
	const std::vector<float> x = renderAtSong(*later, in, 256, kSongFrame0 + 658, true);
	CHECK_FALSE(sameBits(x, aL));
	CHECK(later->sprayDrawForTest(0) != a->sprayDrawForTest(0));

	// STOPPED IS TODAY'S GRANULAR: the same bits as with no clock at all, nothing on the song's grid.
	auto stopped = fresh(s);
	auto plain = fresh(s);
	const std::vector<float> withClock = renderAtSong(*stopped, in, 256, kSongFrame0, false);
	const std::vector<float> without = render(*plain, in, 256);
	CHECK(sameBits(withClock, without));
	CHECK(stopped->songSpawnsForTest() == 0);
}

//------------------------------------------------------------------------------------------------//
// A LANE THAT PASSES THROUGH IS NO LANE (the defect: adding even a disabled effect to the feedback  //
// chain, at least on Buckets, caused a huge increase in output)                                    //
//------------------------------------------------------------------------------------------------//

namespace {

dl::Settings looping(dl::Mode m, float feedback) {
	dl::Settings s = wetOnly(feedback);
	s.mode = m;
	return s;
}

/// THE THIRD PROBE (Feedback 130%, a full-scale burst) is where Buckets measured +4 dB with a switched-off device in the lane
/// before the fix (+12.5 dB at Compander 100%): the read side's expander lifted returns the compressor had never seen.
/// SILENCE FIRST, so the lane has drained and a switched-off device has finished fading and parked; then a burst.
std::vector<float> laneProbe(float amp) {
	std::vector<float> v(4800, 0.0f);
	const std::vector<float> b = burst(3 * 48000, 4800, amp);
	v.insert(v.end(), b.begin(), b.end());
	return v;
}

/// THE TAIL'S LEVEL, dB: from the second repeat on, so it is the LOOP's gain that is measured, not the first tap's.
double tailDb(const std::vector<float>& v) {
	return 20.0 * std::log10(std::max(1e-12, rmsOf(v, 4800 + 2 * kD, v.size())));
}

} // namespace

TEST_CASE("FB (holder): a lane holding only SWITCHED-OFF devices is no lane — every mode, the bare loop's bits") {
	for (const auto& [feedback, amp] : { std::pair { 0.6f, 0.3f }, std::pair { 0.9f, 0.9f }, std::pair { 1.3f, 1.0f } }) {
	const std::vector<float> in = laneProbe(amp);
	for (int m = 0; m < static_cast<int>(dl::Mode::kCount); ++m) {
		const std::string mode = dl::kModeNames[m];
		CAPTURE(mode);
		CAPTURE(feedback);
		const dl::Settings s = looping(static_cast<dl::Mode>(m), feedback);
		auto bare = fresh(s);
		const std::vector<float> want = render(*bare, in, 300);
		Device dev(s);
		const DeviceInstanceId util = dev.add(fx::busutil::kDescriptor.traits.tag);
		const DeviceInstanceId clip = dev.add(fx::clip::kDescriptor.traits.tag); // a LATENT one too, at 4x
		dev.lane().setField(clip, controlNamed(fx::clip::kDescriptor, "Clip Oversample"), 1.0f);
		dev.lane().setField(util, static_cast<size_t>(fx::busutil::kDescriptor.traits.enableControl), 0.0f);
		dev.lane().setField(clip, static_cast<size_t>(fx::clip::kDescriptor.traits.enableControl), 0.0f);
		const std::vector<float> got = dev.render(in);
		MESSAGE(mode << " fb " << feedback << ": bare " << tailDb(want) << " dB, switched-off lane " << tailDb(got) << " dB");
		REQUIRE(rmsOf(want, 4800 + 2 * kD, want.size()) > 1e-4); // the loop sounds: not vacuous
		CHECK(std::memcmp(got.data(), want.data(), want.size() * sizeof(float)) == 0);
	}
	}
}

TEST_CASE("FB (holder): a UNITY device in the lane keeps every mode's loop gain — Buckets included — within 0.5 dB") {
	for (const auto& [feedback, amp] : { std::pair { 0.6f, 0.3f }, std::pair { 0.9f, 0.9f }, std::pair { 1.3f, 1.0f } }) {
	const std::vector<float> in = laneProbe(amp);
	for (int m = 0; m < static_cast<int>(dl::Mode::kCount); ++m) {
		const std::string mode = dl::kModeNames[m];
		CAPTURE(mode);
		CAPTURE(feedback);
		const dl::Settings s = looping(static_cast<dl::Mode>(m), feedback);
		auto bare = fresh(s);
		const std::vector<float> want = render(*bare, in, 300);
		Device dev(s);
		(void)dev.add(fx::busutil::kDescriptor.traits.tag); // at its defaults: the fader at unity, centred, on
		const std::vector<float> got = dev.render(in);
		const double a = tailDb(want);
		const double b = tailDb(got);
		MESSAGE(mode << " fb " << feedback << ": bare " << a << " dB, unity lane " << b << " dB, difference " << b - a << " dB");
		CHECK(std::fabs(b - a) < 0.5);
	}
	}
}

TEST_CASE("FB (holder): a switched-off device switched back ON is heard again — the bypassed lane re-arms") {
	Device dev(wetOnly(0.5f));
	const DeviceInstanceId util = dev.add(fx::busutil::kDescriptor.traits.tag);
	const auto enable = static_cast<size_t>(fx::busutil::kDescriptor.traits.enableControl);
	dev.lane().setField(util, controlNamed(fx::busutil::kDescriptor, "Bus Gain"), 0.4f);
	dev.lane().setField(util, enable, 0.0f);
	(void)dev.render(std::vector<float>(4800, 0.0f)); // faded out and parked: the lane is bypassed now
	const std::vector<float> off = dev.render(impulse(3 * kD + 10), 256);
	CHECK(std::fabs(off[2 * kD] - 0.5f) <= 2.0e-3f); // the bare loop: Feedback alone
	dev.lane().setField(util, enable, 1.0f);
	(void)dev.render(std::vector<float>(4800, 0.0f)); // the bare loop's own tail drains; the device fades back in
	const std::vector<float> on = dev.render(impulse(3 * kD + 10), 256);
	CHECK(std::fabs(on[2 * kD] - 0.5f * fx::busutil::faderGain(0.4f)) <= 2.0e-3f);
}
