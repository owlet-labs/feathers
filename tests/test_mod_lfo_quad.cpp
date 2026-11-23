/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// THE QUAD LFO: four tabs, each a whole LFO, behind one device. Tab 1 is the LFO as it was; tabs 2-4 are appended, off.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"
#include "mod/per_note.h"

#include "device/registry.h"

#include <cmath>
#include <cstring>
#include <array>
#include <memory>
#include <vector>

using namespace feathers;
using namespace feathers::mod::lfo;

namespace {

constexpr double kSampleRate = 48000.0;

/// A DEVICE INSTANCE THROUGH ITS OWN OPS, as a host runs it.
struct Instance {
	std::unique_ptr<unsigned char[]> bytes;
	explicit Instance() : bytes(new unsigned char[kModOps.stateBytes + kModOps.stateAlign]) {
		void* at = storage();
		kModOps.construct(at);
		kModOps.prepare(at, kSampleRate);
	}
	~Instance() { kModOps.destruct(storage()); }
	void* storage() const {
		auto p = reinterpret_cast<std::uintptr_t>(bytes.get());
		p = (p + kModOps.stateAlign - 1) & ~(static_cast<std::uintptr_t>(kModOps.stateAlign) - 1);
		return reinterpret_cast<void*>(p);
	}
	void fields(const std::vector<float>& row) { kModOps.setFields(storage(), row.data(), row.size()); }
	void tick(bool gate = false) {
		device::ModTickContext ctx {};
		ctx.gate = gate;
		kModOps.tick(storage(), ctx);
	}
	float out(size_t output) const { return kModOps.outputAt(storage(), output); }
};

/// ONE TAB'S ROW from settings, the way the codec writes it.
void put(std::vector<float>& row, size_t tab, const Settings& s) {
	const Row r = rowOf(s);
	for (size_t i = 0; i < r.size(); ++i) {
		row[tab * kPerTab + i] = r[i];
	}
}

Settings freeSine(float log2Hz) {
	Settings s;
	s.wave = Waveform::Sine;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = 0;
	s.timeScale = log2Hz;
	s.bipolar = true;
	return s;
}

/// THE ROW OF A PATCH THAT NEVER KNEW THE TABS: nineteen fields.
std::vector<float> oldRow(const Settings& s) {
	const Row r = rowOf(s);
	return std::vector<float>(r.begin(), r.end());
}

/// THE FULL ROW: four tabs, tabs 2-4 at their descriptors' defaults (off) unless `put`.
std::vector<float> fullRow(const Settings& first) {
	std::vector<float> row(kTabs * kPerTab + 1, 0.0f);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	put(row, 0, first);
	return row;
}

} // namespace

TEST_CASE("quad lfo: the descriptor is four copies of one tab, tab 1's keys untouched, tabs 2-4 off") {
	REQUIRE(std::size(kParamDescs) == kTabs * kPerTab + 1);
	for (size_t i = 0; i < kPerTab; ++i) {
		const std::string base = kParamDescs[i].key;
		for (size_t t = 1; t < kTabs; ++t) {
			const auto& d = kParamDescs[t * kPerTab + i];
			CHECK(std::string(d.key) == "t" + std::to_string(t + 1) + "_" + base);
			CHECK(d.min == kParamDescs[i].min);
			CHECK(d.max == kParamDescs[i].max);
			CHECK(d.unit == kParamDescs[i].unit);
			// A GATE NAMES A CONTROL OF ITS OWN TAB.
			if (kParamDescs[i].gateControl >= 0) {
				CHECK(d.gateControl == kParamDescs[i].gateControl + static_cast<int>(t * kPerTab));
			}
		}
	}
	CHECK(std::string(kParamDescs[0].key) == "waveform");
	CHECK(kParamDescs[tabParam(0, Param::Enabled)].def == 1.0);
	for (size_t t = 1; t < kTabs; ++t) {
		CHECK(kParamDescs[tabParam(t, Param::Enabled)].def == 0.0);
	}
	REQUIRE(kDescriptor.outputs.size() == kTabs);
	for (size_t t = 0; t < kTabs; ++t) {
		const auto& o = kDescriptor.outputs[t];
		CHECK(o.bipolarControl == static_cast<int>(tabParam(t, Param::Bipolar)));
		CHECK(o.pairWhile == static_cast<int>(tabParam(t, Param::StereoPhase)));
		if (t > 0) {
			CHECK(o.enableControl == static_cast<int>(tabParam(t, Param::Enabled)));
			CHECK(o.amountControl == static_cast<int>(tabParam(t, Param::Amount)));
		}
	}
	MESSAGE("quad LFO instance: ", kModOps.stateBytes, " bytes (four tabs x (shared wave + 16 notes))");
}

TEST_CASE("quad lfo: a patch from before the tabs is tab 1, bit for bit, and tabs 2-4 are off") {
	Settings s = freeSine(std::log2(3.0f));
	s.skewPreCross = 0.7f;
	s.smoothing = -7.0f;
	Instance before; // nineteen fields: an old patch
	before.fields(oldRow(s));
	Instance after; // the whole row, tabs 2-4 at their defaults
	after.fields(fullRow(s));
	// THE REFERENCE: the same settings run by a bare shape, which is what this device was.
	Shape reference;
	reference.prepare(kSampleRate);
	for (int t = 0; t < 600; ++t) {
		before.tick();
		after.tick();
		reference.tick(s, false, false, feathers::mod::TransportClock {});
		const float want = reference.value();
		CHECK(before.out(0) == want);
		CHECK(after.out(0) == want);
		for (size_t tab = 1; tab < kTabs; ++tab) {
			CHECK(before.out(tab) == 0.0f);
			CHECK(after.out(tab) == 0.0f);
		}
	}
}

TEST_CASE("quad lfo: each tab is independent — tab 2 at its own rate and shape leaves tab 1 bit-identical, and equals a bare shape") {
	const Settings one = freeSine(std::log2(3.0f));
	Settings two = freeSine(std::log2(5.0f));
	two.wave = Waveform::Triangle;
	two.enabled = true;
	Instance alone;
	alone.fields(fullRow(one));
	Instance both;
	std::vector<float> row = fullRow(one);
	put(row, 1, two);
	both.fields(row);
	Shape ref2;
	ref2.prepare(kSampleRate);
	double travelled = 0.0;
	for (int t = 0; t < 800; ++t) {
		alone.tick();
		both.tick();
		ref2.tick(two, false, false, feathers::mod::TransportClock {});
		CHECK(both.out(0) == alone.out(0)); // tab 1 does not know tab 2 exists
		CHECK(both.out(1) == ref2.value());
		travelled += std::abs(static_cast<double>(both.out(1)));
	}
	CHECK(travelled > 100.0); // and tab 2 really ran
	// A THIRD AND FOURTH, each its own, none disturbing the others.
	std::vector<float> all = row;
	put(all, 2, freeSine(std::log2(7.0f)));
	all[tabParam(2, Param::Enabled)] = 1.0f;
	put(all, 3, freeSine(std::log2(9.0f)));
	all[tabParam(3, Param::Enabled)] = 1.0f;
	Instance four;
	four.fields(all);
	Instance reuse;
	reuse.fields(row);
	for (int t = 0; t < 300; ++t) {
		four.tick();
		reuse.tick();
		CHECK(four.out(0) == reuse.out(0));
		CHECK(four.out(1) == reuse.out(1));
	}
	CHECK(four.out(2) != four.out(3));
}

TEST_CASE("quad lfo: a tab that is off is not run at all — its value never moves, and it starts afresh when switched on") {
	const Settings one = freeSine(std::log2(3.0f));
	Instance inst;
	std::vector<float> row = fullRow(one);
	Settings two = freeSine(std::log2(4.0f));
	put(row, 1, two);
	row[tabParam(1, Param::Enabled)] = 0.0f;
	inst.fields(row);
	for (int t = 0; t < 200; ++t) {
		inst.tick();
		CHECK(inst.out(1) == 0.0f); // not ticked: nothing moved it
	}
	CHECK_FALSE(kModOps.renderWindow(inst.storage(), 1, false, -1, -1, -1, false, std::array<float, 65> {}.data()));
	row[tabParam(1, Param::Enabled)] = 1.0f;
	inst.fields(row);
	Shape fresh;
	fresh.prepare(kSampleRate);
	for (int t = 0; t < 50; ++t) {
		inst.tick();
		fresh.tick(two, false, false, feathers::mod::TransportClock {});
		CHECK(inst.out(1) == fresh.value()); // the tab began on its first tick, as a new shape does
	}
}

TEST_CASE("quad lfo: seeds are per tab and persist as the tab's own row (capture, save, load into a fresh instance, compare)") {
	Settings a = freeSine(std::log2(2.0f));
	a.wave = Waveform::Random;
	a.seed = 17;
	Settings b = a;
	b.seed = 523;
	std::vector<float> row = fullRow(a);
	put(row, 1, b);
	row[tabParam(1, Param::Enabled)] = 1.0f;
	// SAVE: the row; LOAD: into a fresh instance and read each tab's settings back.
	for (size_t tab = 0; tab < 2; ++tab) {
		const Settings back = settingsOfTab(row.data(), row.size(), tab);
		CHECK(back.seed == (tab == 0 ? 17 : 523));
	}
	Instance first;
	first.fields(row);
	Instance second;
	second.fields(row);
	bool differ = false;
	for (int t = 0; t < 2000; ++t) {
		first.tick();
		second.tick();
		CHECK(first.out(0) == second.out(0));
		CHECK(first.out(1) == second.out(1)); // a reload reproduces both walks by construction
		differ = differ || first.out(0) != first.out(1);
	}
	CHECK(differ); // two seeds, two walks
}

TEST_CASE("quad lfo: under Poly a tab runs a wave per note of its own") {
	Settings one = freeSine(std::log2(3.0f));
	one.mode = feathers::mod::Mode::LfoRetrig;
	Settings two = one;
	two.poly = true;
	two.timeScale = std::log2(5.0f);
	std::vector<float> row = fullRow(one);
	put(row, 1, two);
	row[tabParam(1, Param::Enabled)] = 1.0f;
	Instance inst;
	inst.fields(row);
	const device::NoteEvent in[2] = {
		{ device::NoteEvent::Kind::On, 0, 1, 0, 0, 60, 1.0 },
		{ device::NoteEvent::Kind::On, 0, 2, 0, 0, 64, 1.0 },
	};
	device::NoteEvent out[4] {};
	size_t made = 0;
	// THE NOTE LANE'S HALF holds its own instance, as a host builds it.
	std::unique_ptr<unsigned char[]> noteBytes(new unsigned char[kNoteOps.stateBytes + kNoteOps.stateAlign]);
	auto p = reinterpret_cast<std::uintptr_t>(noteBytes.get());
	p = (p + kNoteOps.stateAlign - 1) & ~(static_cast<std::uintptr_t>(kNoteOps.stateAlign) - 1);
	void* notes = reinterpret_cast<void*>(p);
	kNoteOps.construct(notes);
	kNoteOps.prepare(notes, kSampleRate);
	kNoteOps.setFields(notes, row.data(), row.size());
	kNoteOps.processNotes(notes, in, 2, out, 4, &made);
	CHECK(made == 2);
	kNoteOps.destruct(notes);
}

TEST_CASE("quad lfo: the view field picks the tab whose playhead the device publishes") {
	const Settings one = freeSine(std::log2(3.0f));
	Settings two = freeSine(std::log2(5.0f));
	two.wave = Waveform::Triangle;
	std::vector<float> row = fullRow(one);
	put(row, 1, two);
	row[tabParam(1, Param::Enabled)] = 1.0f;
	Instance inst;
	inst.fields(row);
	Shape ref1, ref2;
	ref1.prepare(kSampleRate);
	ref2.prepare(kSampleRate);
	const auto phaseOf = [&] {
		uint32_t ordinal = 0;
		float phase = 0.0f;
		kModOps.playhead(inst.storage(), &ordinal, &phase);
		return phase;
	};
	for (int t = 0; t < 40; ++t) {
		inst.tick();
		ref1.tick(one, false, false, feathers::mod::TransportClock {});
		ref2.tick(two, false, false, feathers::mod::TransportClock {});
	}
	CHECK(phaseOf() == ref1.playhead().phase); // view 0 (the default row): tab 1
	row[kViewTabField] = 1.0f;
	inst.fields(row);
	CHECK(phaseOf() == ref2.playhead().phase); // view 1: tab 2's own
	CHECK(ref1.playhead().phase != ref2.playhead().phase);
	row[kViewTabField] = 9.0f; // out of range clamps to the last tab
	inst.fields(row);
	CHECK(std::isfinite(phaseOf()));
}
