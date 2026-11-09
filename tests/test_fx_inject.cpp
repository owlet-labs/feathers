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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/inject/inject_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

// SIDECHAIN INJECT (backlog NW) — every claim is a value: the output at Mix 1 is the sidechain times Gain, at Mix 0 the input
// to the bit, and the bus road's pointers die with the call.

namespace inj = feathers::fx::inject;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;

class Instance {
public:
	Instance() {
		REQUIRE(inj::kAudioOps.stateBytes <= storage_.size());
		inj::kAudioOps.construct(storage_.data());
		inj::kAudioOps.prepare(storage_.data(), kSampleRate, 4096);
		inj::kAudioOps.setReadingsSink(
		    storage_.data(), feathers::device::ReadingsSink {
		                         this, [](void* context, const float* values, size_t count) {
			                         auto* self = static_cast<Instance*>(context);
			                         for (size_t i = 0; i < std::min(count, self->readings_.size()); ++i) {
				                         self->readings_[i] = values[i];
			                         }
			                         self->count_ = count;
		                         } });
	}
	~Instance() { inj::kAudioOps.destruct(storage_.data()); }
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	void set(const inj::Settings& s) {
		row_ = inj::rowOf(s);
		inj::kAudioOps.setFields(storage_.data(), row_.data(), row_.size());
	}
	/// ONE BLOCK THE WAY THE BUS ROAD RUNS IT: the sidechain handed first (null buffers when `connected` is false).
	void block(float* l, float* r, const float* sl, const float* sr, int frames, bool sounding) {
		inj::kAudioOps.setSidechain(storage_.data(), sl, sr, frames, sounding);
		inj::kAudioOps.processStereo(storage_.data(), l, r, frames);
	}
	/// A CALL WITH NO SIDECHAIN HANDED BEFORE IT — what a host that skipped `setSidechain` does.
	void process(float* l, float* r, int frames) { inj::kAudioOps.processStereo(storage_.data(), l, r, frames); }
	/// INPUT DC and SIDECHAIN DC for `frames`; returns the last output pair.
	std::pair<float, float> runDc(float in, float side, int frames, bool connected = true) {
		std::vector<float> l(kBlock), r(kBlock), sl(kBlock, side), sr(kBlock, side);
		std::pair<float, float> last {};
		for (int done = 0; done < frames; done += kBlock) {
			const int n = std::min(kBlock, frames - done);
			std::fill(l.begin(), l.end(), in);
			std::fill(r.begin(), r.end(), in);
			block(l.data(), r.data(), connected ? sl.data() : nullptr, connected ? sr.data() : nullptr, n, connected && side != 0.0f);
			last = { l[static_cast<size_t>(n - 1)], r[static_cast<size_t>(n - 1)] };
		}
		return last;
	}
	[[nodiscard]] float reading(inj::Reading r) const { return readings_[static_cast<size_t>(r)]; }
	[[nodiscard]] size_t readingsCount() const { return count_; }
	[[nodiscard]] float gainLin() const { return inj::gainLinForTest(storage_.data()); }

private:
	alignas(std::max_align_t) std::array<std::byte, 1024> storage_ {};
	inj::Row row_ = inj::rowOf(inj::Settings {});
	std::array<float, 8> readings_ {};
	size_t count_ = 0;
};

[[nodiscard]] float dbToLin(double db) {
	return static_cast<float>(std::pow(10.0, db / 20.0));
}

TEST_CASE("Inject at Mix 1 outputs the sidechain times Gain, to the bit, and not the input") {
	inj::Settings s;
	s.mix = 1.0f;
	s.gainDb = 6.0f;
	Instance dev;
	dev.set(s);
	const float in = 0.25f, side = 0.125f;
	const auto [l, r] = dev.runDc(in, side, 4800);
	CHECK(dev.gainLin() == dbToLin(6.0));
	CHECK(l == dev.gainLin() * side); // exactly the sidechain x Gain — the input contributes nothing
	CHECK(r == dev.gainLin() * side);
	CHECK(l != in);
}

TEST_CASE("Inject at Mix 0 is the input to the bit, whatever the sidechain and Gain say") {
	inj::Settings s;
	s.mix = 0.0f;
	s.gainDb = 24.0f;
	Instance dev;
	dev.set(s);
	const auto [l, r] = dev.runDc(0.3f, 0.9f, 4800);
	CHECK(l == 0.3f);
	CHECK(r == 0.3f);
}

TEST_CASE("Inject at Mix 50% is the two signals' average, and Gain scales only the sidechain's half") {
	inj::Settings s;
	s.mix = 0.5f;
	s.gainDb = 0.0f;
	Instance dev;
	dev.set(s);
	CHECK(dev.runDc(0.4f, 0.2f, 4800).first == doctest::Approx(0.3f).epsilon(1e-6));
	s.gainDb = static_cast<float>(20.0 * std::log10(2.0));
	dev.set(s);
	CHECK(dev.runDc(0.4f, 0.2f, 4800).first == doctest::Approx(0.4f).epsilon(1e-5)); // 0.5*0.4 + 0.5*(2*0.2)
}

TEST_CASE("Inject with no sidechain connected puts silence in at Mix 1, says so, and leaves the input alone at Mix 0") {
	inj::Settings s;
	s.mix = 1.0f;
	{
		Instance dev;
		dev.set(s);
		const auto [l, r] = dev.runDc(0.5f, 0.0f, 4800, false);
		CHECK(l == 0.0f);
		CHECK(r == 0.0f);
		CHECK(dev.readingsCount() == static_cast<size_t>(inj::Reading::kCount));
		CHECK(dev.reading(inj::Reading::SidechainSounding) == 0.0f);
		CHECK(dev.reading(inj::Reading::Injected) == 0.0f);
	}
	{
		s.mix = 0.0f;
		Instance dev;
		dev.set(s);
		CHECK(dev.runDc(0.5f, 0.0f, 4800, false).first == 0.5f);
	}
	// AND WITH A SIDECHAIN THE READINGS SAY SO, and the injected level is the value the card draws.
	s.mix = 1.0f;
	s.gainDb = 0.0f;
	Instance dev;
	dev.set(s);
	dev.runDc(0.0f, 0.5f, 4800);
	CHECK(dev.reading(inj::Reading::SidechainSounding) == 1.0f);
	CHECK(dev.reading(inj::Reading::Injected) == 0.5f);
}

TEST_CASE("Inject follows the sidechain per sample and drops the pointers with the call") {
	inj::Settings s;
	Instance dev;
	dev.set(s);
	// A RAMP SIDECHAIN: the output at Mix 1 is the ramp itself, sample for sample.
	std::vector<float> l(300, 0.7f), r(300, 0.7f), side(300);
	for (size_t i = 0; i < side.size(); ++i) {
		side[i] = static_cast<float>(i + 1) * 1e-3f;
	}
	dev.block(l.data(), r.data(), side.data(), side.data(), 300, true);
	size_t bad = 0;
	for (size_t i = 0; i < side.size(); ++i) {
		bad += l[i] != side[i] || r[i] != side[i];
	}
	CHECK(bad == 0u);
	// THE NEXT CALL HANDS NONE: it must not read the old buffer.

	std::fill(l.begin(), l.end(), 0.7f);
	std::fill(r.begin(), r.end(), 0.7f);
	dev.process(l.data(), r.data(), 300);
	size_t stale = 0;
	for (size_t i = 0; i < l.size(); ++i) {
		stale += l[i] != 0.0f || r[i] != 0.0f;
	}
	CHECK(stale == 0u); // Mix 1 over a sidechain that is gone is silence, not the last block replayed
}

} // namespace
