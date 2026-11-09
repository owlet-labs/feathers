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
#include "fx/eq/eq_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/resonate/resonate_dsp.h"
#include "fx/resonate/resonate_lanes.h"
#include "fx/resonate/resonate_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

// RESONATE'S TWO FX LANES, BY VALUE.
//
// THE REFERENCE IS AN INDEPENDENT RESTATEMENT: a bare `ResonateDsp` with its lane tap read directly, and the mix
// restated here — so a claim like "with Drone to Wet FX off, a silenced WET FX leaves exactly the drone" is checked
// against the drone the DSP produced, not against another render of the device under test.

namespace rsn = feathers::fx::resonate;
using namespace feathers;

namespace {

constexpr double kRate = 48000.0;
/// NOT A MULTIPLE OF THE OPS' 256-SAMPLE CHUNK, so every host block crosses a chunk boundary.
constexpr int kBlock = 300;

using Row = rsn::Row;

Row rowWith(bool droneToWet, float mix = 0.7f) {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(rsn::kParamDescs[i].def);
	}
	row[static_cast<size_t>(rsn::Param::DroneToWetFx)] = droneToWet ? 1.0f : 0.0f;
	row[static_cast<size_t>(rsn::Param::Mix)] = mix;
	return row;
}

/// A plucked phrase: sawtooth notes from -6 dBFS falling over 0.3 s, the same notes on the note road.
struct Phrase {
	std::vector<float> input;
	struct Note {
		int key;
		int on;
		int off;
	};
	std::vector<Note> notes;
};

Phrase phrase(double seconds, double silenceAfter) {
	Phrase p;
	const int total = static_cast<int>((seconds + silenceAfter) * kRate);
	p.input.assign(static_cast<size_t>(total), 0.0f);
	const int keys[] = { 48, 55, 60, 64 };
	const int spacing = static_cast<int>(seconds * kRate / 4.0);
	for (int n = 0; n < 4; ++n) {
		const int on = n * spacing;
		p.notes.push_back({ keys[n], on, on + spacing / 2 });
		const double hz = 440.0 * std::exp2((keys[n] - 69) / 12.0);
		for (int i = 0; i < spacing; ++i) {
			const double t = i / kRate;
			const double saw = 2.0 * (t * hz - std::floor(t * hz + 0.5));
			p.input[static_cast<size_t>(on + i)] += static_cast<float>(0.5 * saw * std::exp(-t / 0.3));
		}
	}
	return p;
}

std::vector<device::NoteEvent> eventsIn(const Phrase& p, int at, int n) {
	std::vector<device::NoteEvent> out;
	for (const Phrase::Note& note : p.notes) {
		for (const auto& [when, kind] : { std::pair { note.on, device::NoteEvent::Kind::On },
		                                  std::pair { note.off, device::NoteEvent::Kind::Off } }) {
			if (when >= at && when < at + n) {
				device::NoteEvent e;
				e.kind = kind;
				e.key = static_cast<int16_t>(note.key);
				e.noteId = note.key;
				e.velocity = kind == device::NoteEvent::Kind::On ? 0.8 : 0.0;
				out.push_back(e);
			}
		}
	}
	return out;
}

float finiteOr0(float v) {
	return std::isfinite(v) ? v : 0.0f;
}

/// THE DSP'S OWN MIX, restated (resonate_dsp.cpp, "THE WET").
float mixOf(float dry, float wet, float m, float e) {
	const float mixed = (1.0f - m) * dry + m * wet;
	return finiteOr0(dry + e * (mixed - dry));
}

/// THE REFERENCE: a bare DSP, its output and its tap, sample by sample.
struct Reference {
	std::vector<float> outL, outR, dryL, dryR, toneL, toneR, droneL, droneR, mix, enable;
};

Reference reference(const Row& row, const Phrase& p) {
	auto dsp = std::make_unique<rsn::ResonateDsp>();
	dsp->prepare(kRate);
	dsp->setSettings(rsn::stateOf(row, row.size()));
	std::vector<float> b[8];
	for (auto& v : b) {
		v.assign(kBlock, 0.0f);
	}
	std::vector<float> m(kBlock), e(kBlock);
	const rsn::ResonateDsp::LaneTap tap { { b[0].data(), b[1].data() }, { b[2].data(), b[3].data() },
		                                  { b[4].data(), b[5].data() }, m.data(), e.data() };
	dsp->setLaneTap(&tap);
	Reference r;
	const int total = static_cast<int>(p.input.size());
	std::vector<float> l(kBlock), rr(kBlock);
	for (int at = 0; at < total; at += kBlock) {
		const int n = std::min(kBlock, total - at);
		const auto events = eventsIn(p, at, n);
		dsp->queueNotes(events.data(), events.size());
		std::copy_n(p.input.begin() + at, n, l.begin());
		std::copy_n(p.input.begin() + at, n, rr.begin());
		dsp->process(l.data(), rr.data(), n);
		r.outL.insert(r.outL.end(), l.begin(), l.begin() + n);
		r.outR.insert(r.outR.end(), rr.begin(), rr.begin() + n);
		r.dryL.insert(r.dryL.end(), b[0].begin(), b[0].begin() + n);
		r.dryR.insert(r.dryR.end(), b[1].begin(), b[1].begin() + n);
		r.toneL.insert(r.toneL.end(), b[2].begin(), b[2].begin() + n);
		r.toneR.insert(r.toneR.end(), b[3].begin(), b[3].begin() + n);
		r.droneL.insert(r.droneL.end(), b[4].begin(), b[4].begin() + n);
		r.droneR.insert(r.droneR.end(), b[5].begin(), b[5].begin() + n);
		r.mix.insert(r.mix.end(), m.begin(), m.begin() + n);
		r.enable.insert(r.enable.end(), e.begin(), e.begin() + n);
	}
	return r;
}

/// THE DEVICE UNDER TEST, through its own ops, adopted with the host's catalogue so its lanes take devices.
struct Device {
	const device::AudioOps& audio = *rsn::kDescriptor.audio;
	const device::NoteOps& notes = *rsn::kDescriptor.notes;
	void* mem = nullptr;
	Row row;

	explicit Device(const Row& r) : row(r) {
		mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(mem);
		rsn::kDescriptor.container->adopt(mem, hostCatalogue());
		audio.prepare(mem, kRate, 4096);
		audio.setFields(mem, row.data(), row.size());
	}
	~Device() {
		audio.destruct(mem);
		::operator delete(mem, std::align_val_t { audio.stateAlign });
	}
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;

	rack::RackBridge& lane(rsn::Lane which) const {
		rack::InnerChain* chain = rsn::laneChainOf(mem, which);
		REQUIRE(chain != nullptr);
		REQUIRE(chain->bridge() != nullptr);
		return *chain->bridge();
	}
	DeviceInstanceId add(rsn::Lane which, const char* tag) const {
		const DeviceInstanceId id = lane(which).addDevice(tag);
		REQUIRE(id != kNoInstance);
		return id;
	}
	[[nodiscard]] uint32_t latency() const { return audio.latencySamples(mem, row.data(), row.size(), kRate); }

	void render(const Phrase& p, std::vector<float>& outL, std::vector<float>& outR) const {
		const int total = static_cast<int>(p.input.size());
		outL.assign(static_cast<size_t>(total), 0.0f);
		outR.assign(static_cast<size_t>(total), 0.0f);
		std::vector<device::NoteEvent> passed(64);
		for (int at = 0; at < total; at += kBlock) {
			const int n = std::min(kBlock, total - at);
			const auto events = eventsIn(p, at, n);
			size_t count = 0;
			notes.processNotes(mem, events.data(), events.size(), passed.data(), passed.size(), &count);
			std::copy_n(p.input.begin() + at, n, outL.begin() + at);
			std::copy_n(p.input.begin() + at, n, outR.begin() + at);
			audio.processStereo(mem, outL.data() + at, outR.data() + at, n);
		}
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

float peak(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
	float p = 0.0f;
	for (size_t i = from; i < std::min(to, v.size()); ++i) {
		p = std::max(p, std::fabs(v[i]));
	}
	return p;
}

} // namespace

TEST_CASE("RESONATE's fx lanes, EMPTY: the output is the DSP's own, bit for bit, in both routings") {
	const Phrase p = phrase(2.0, 0.5);
	for (const bool on : { true, false }) {
		CAPTURE(on);
		const Row row = rowWith(on);
		const Reference ref = reference(row, p);
		// NON-VACUITY: both halves of the wet sound, and the output is not the input.
		CHECK(peak(ref.toneL) > 1e-3f);
		CHECK(peak(ref.droneL) > 1e-3f);
		Device dev(row);
		std::vector<float> l, r;
		dev.render(p, l, r);
		REQUIRE(l.size() == ref.outL.size());
		CHECK(std::memcmp(l.data(), ref.outL.data(), l.size() * sizeof(float)) == 0);
		CHECK(std::memcmp(r.data(), ref.outR.data(), r.size() * sizeof(float)) == 0);
		CHECK(rsn::droneToWetFxForTest(dev.mem) == on);
		CHECK(dev.latency() == 0);
	}
}

TEST_CASE("RESONATE's fx lanes: Drone to Wet FX decides whether WET FX hears the drone, by value") {
	// A BUS UTILITY AT GAIN 0 IN WET FX silences whatever goes through that lane. ON, the drone goes through it, so the
	// whole wet is silent; OFF, the drone joins after it, so the wet is EXACTLY the drone. The reference is the bare
	// DSP's drone and dry, mixed as the DSP mixes.
	const Phrase p = phrase(2.0, 0.5);
	const auto gain = static_cast<size_t>(fx::busutil::Param::Gain);
	const size_t from = static_cast<size_t>(0.05 * kRate); // past the utility's own gain ramp
	std::vector<float> onL, offL;
	for (const bool on : { true, false }) {
		CAPTURE(on);
		const Row row = rowWith(on);
		const Reference ref = reference(row, p);
		Device dev(row);
		const DeviceInstanceId util = dev.add(rsn::Lane::Wet, fx::busutil::kTraits.tag);
		dev.lane(rsn::Lane::Wet).setField(util, gain, 0.0f);
		std::vector<float> l, r;
		dev.render(p, l, r);
		float worst = 0.0f;
		for (size_t i = from; i < l.size(); ++i) {
			const float wantL = mixOf(ref.dryL[i], on ? 0.0f : ref.droneL[i], ref.mix[i], ref.enable[i]);
			const float wantR = mixOf(ref.dryR[i], on ? 0.0f : ref.droneR[i], ref.mix[i], ref.enable[i]);
			worst = std::max({ worst, std::fabs(l[i] - wantL), std::fabs(r[i] - wantR) });
		}
		CAPTURE(worst);
		CHECK(worst < 1e-6f);
		(on ? onL : offL) = l;
	}
	// AND THE TWO ROUTINGS ARE AUDIBLY DIFFERENT HERE — the drone is there in one and gone in the other.
	float apart = 0.0f;
	for (size_t i = from; i < onL.size(); ++i) {
		apart = std::max(apart, std::fabs(onL[i] - offL[i]));
	}
	CHECK(apart > 1e-2f);

	// THE SAME UTILITY IN DRONE FX takes the drone out under either routing: the wet is exactly the tone.
	for (const bool on : { true, false }) {
		CAPTURE(on);
		const Row row = rowWith(on);
		const Reference ref = reference(row, p);
		Device dev(row);
		const DeviceInstanceId util = dev.add(rsn::Lane::Drone, fx::busutil::kTraits.tag);
		dev.lane(rsn::Lane::Drone).setField(util, gain, 0.0f);
		std::vector<float> l, r;
		dev.render(p, l, r);
		float worst = 0.0f;
		for (size_t i = from; i < l.size(); ++i) {
			worst = std::max(worst, std::fabs(l[i] - mixOf(ref.dryL[i], ref.toneL[i], ref.mix[i], ref.enable[i])));
		}
		CAPTURE(worst);
		CHECK(worst < 1e-6f);
	}
}

TEST_CASE("RESONATE's fx lanes: a latent device in either lane, either routing — reported, and every path aligned") {
	// A FLAT LINEAR-PHASE EQ is a pure delay of L samples. Whatever lane it sits in and however the drone is routed, the
	// device's output must be the lane-free output L' samples late — L' what it REPORTS — with the dry, the tone and the
	// drone all held to meet. ON with an EQ in both lanes the drone passes through both: 2L. OFF the lanes are parallel: L.
	const uint32_t L = fx::eq::linearLatency(kRate);
	const Phrase p = phrase(1.0, 2.0 * L / kRate + 0.2);
	const auto phase = static_cast<size_t>(fx::eq::Param::Phase);
	struct Config {
		bool drone;
		bool wet;
	};
	for (const bool on : { true, false }) {
		const Reference ref = reference(rowWith(on), p);
		for (const Config c : { Config { true, false }, Config { false, true }, Config { true, true } }) {
			CAPTURE(on);
			CAPTURE(c.drone);
			CAPTURE(c.wet);
			Device dev(rowWith(on));
			if (c.drone) {
				const DeviceInstanceId eq = dev.add(rsn::Lane::Drone, fx::eq::kTraits.tag);
				dev.lane(rsn::Lane::Drone).setField(eq, phase, 1.0f);
			}
			if (c.wet) {
				const DeviceInstanceId eq = dev.add(rsn::Lane::Wet, fx::eq::kTraits.tag);
				dev.lane(rsn::Lane::Wet).setField(eq, phase, 1.0f);
			}
			std::vector<float> l, r;
			dev.render(p, l, r);
			const uint32_t want = on && c.drone && c.wet ? 2 * L : L;
			CHECK(dev.latency() == want);
			float worst = 0.0f;
			const size_t from = static_cast<size_t>(0.05 * kRate);
			for (size_t i = from; i + want < l.size(); ++i) {
				worst = std::max(worst, std::fabs(l[i + want] - ref.outL[i]));
			}
			CAPTURE(worst);
			CHECK(worst < 1e-5f);
			CHECK(peak(l) > 1e-2f); // non-vacuity: something came out
		}
	}
}

TEST_CASE("RESONATE's fx lanes: a reverb in DRONE FX rings on after the string has parked, then reaches exact zero") {
	// THE SLEEP RULE'S PREMISE, AT THE DEVICE: the plugin sleeps only on an exactly silent block, so the device's OUTPUT
	// must carry a lane's tail after its own DSP has parked — and must still end in exact zero, or the plugin never sleeps.
	// Resonate's own drone made short so the reverb's tail is the longer of the two.
	const Phrase p = phrase(1.0, 20.0);
	Row row = rowWith(true, 1.0f);
	row[static_cast<size_t>(rsn::Param::DroneDecay)] = -3.0f; // 1/8 s
	const auto lastSounding = [](const std::vector<float>& v) {
		for (size_t i = v.size(); i-- > 0;) {
			if (v[i] != 0.0f) {
				return i;
			}
		}
		return size_t { 0 };
	};
	std::vector<float> l, r;
	{
		Device dev(row);
		dev.render(p, l, r);
	}
	const size_t alone = lastSounding(l);
	{
		Device dev(row);
		const DeviceInstanceId verb = dev.add(rsn::Lane::Drone, fx::featherverb::kTraits.tag);
		dev.lane(rsn::Lane::Drone).setField(verb, controlNamed(fx::featherverb::kDescriptor, "Featherverb Mix"), 1.0f);
		dev.lane(rsn::Lane::Drone).setField(verb, controlNamed(fx::featherverb::kDescriptor, "Featherverb Decay"), 0.6f);
		dev.render(p, l, r);
	}
	const size_t withVerb = lastSounding(l);
	MESSAGE("last sounding sample: alone " << alone / kRate << " s, with a reverb in DRONE FX " << withVerb / kRate << " s");
	CHECK(withVerb > alone + static_cast<size_t>(kRate)); // the tail rang on a second and more past the string
	CHECK(withVerb + static_cast<size_t>(kRate) < l.size()); // and then ended: a second of exact zero at the end
}
