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

// THE RESONATE ENGINE, end to end through the synth: OscType::Resonate plays the ONE WeaveString the
// synth owns, every voice scanning it at its own pitch, every note plucking it. The string itself is
// test_weave_string's; its laws' decode is test_slice_build's. This is the joining: that the synth
// feeds the string the laws its character describes, plucks it per note, reads it identically at
// every block size, and plays it at a loudness that sits beside the other engines'.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "character/phi_weave.hpp"
#include "dsp/weave_models.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "spectrum.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <array>
#include <chrono>
#include <cstring>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using feathers::OscType;
using feathers::ParamId;
using feathers::ParamSet;
using feathers::Synth;
using feathers::WeaveModel;

constexpr double kSampleRate = 48000.0;
constexpr int16_t kKeyC3 = 48;
constexpr int16_t kKeyC4 = 60;
constexpr int16_t kKeyG4 = 67;
/// The Resonate-vs-Stair loudness window, measured (see the case).
constexpr double kResonateVsStairDb = 4.5;
/// The 3 kHz note's energy above 12 kHz on its band-limited mip, against the whole: -43.2 dB measured on mip 2,
/// -38.5 dB on mip 0 (the old placement), 2026-09-26. The bar sits between them.
constexpr double kResonateAbove12kDb = -41.0;

struct Event {
	enum class Kind { NoteOn, NoteOff, Param } kind;
	uint32_t frame;
	int16_t key {0};
	int32_t noteId {-1};
	ParamId param {ParamId::AZone};
	double value {0.0};
};

Event noteOn(uint32_t frame, int16_t key, int32_t id) {
	return { Event::Kind::NoteOn, frame, key, id };
}
Event noteOff(uint32_t frame, int16_t key, int32_t id) {
	return { Event::Kind::NoteOff, frame, key, id };
}
Event param(uint32_t frame, ParamId id, double value) {
	return { Event::Kind::Param, frame, 0, -1, id, value };
}

struct Rig {
	ParamSet params;
	Synth synth {params};

	explicit Rig(OscType engine = OscType::Resonate, double sampleRate = kSampleRate) {
		synth.activate(sampleRate, 4096);
		set(ParamId::OscType, static_cast<double>(engine));
		set(ParamId::AZone, 17.0);
		set(ParamId::AOffset, 0.0);
		set(ParamId::Path, 0.0);
		set(ParamId::VoiceLevel, 1.0);
	}

	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }

	void apply(const Event& e) {
		switch (e.kind) {
		case Event::Kind::NoteOn:
			synth.noteOn(0, e.key, e.noteId, 1.0);
			break;
		case Event::Kind::NoteOff:
			synth.noteOff(0, e.key, e.noteId, 0.0);
			break;
		case Event::Kind::Param:
			set(e.param, e.value);
			break;
		}
	}

	/// Renders `frames` in host blocks of `block`, cutting at every event as a CLAP host does.
	std::vector<float> play(const std::vector<Event>& script, uint32_t frames, int block) {
		std::vector<float> left(frames, 0.0f);
		std::vector<float> right(frames, 0.0f);
		size_t next = 0;
		uint32_t at = 0;
		while (at < frames) {
			const uint32_t blockEnd = std::min(frames, at + static_cast<uint32_t>(block));
			while (at < blockEnd) {
				while (next < script.size() && script[next].frame <= at) {
					apply(script[next]);
					next++;
				}
				uint32_t until = blockEnd;
				if (next < script.size()) {
					until = std::min(blockEnd, script[next].frame);
				}
				synth.render(left.data() + at, right.data() + at, static_cast<int>(until - at));
				at = until;
			}
		}
		return left;
	}
};

double rmsOf(const std::vector<float>& x, size_t from, size_t to) {
	double sum = 0.0;
	for (size_t i = from; i < to; ++i) {
		sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
	}
	return std::sqrt(sum / static_cast<double>(to - from));
}

/// The largest |sample|: what an identity claim here REQUIREs of its reference first, because two silent renders
/// agree whatever the code does (backlog MA).
float peakOf(const std::vector<float>& x) {
	float peak = 0.0f;
	for (const float v : x) {
		peak = std::max(peak, std::abs(v));
	}
	return peak;
}

long firstDifference(const std::vector<float>& a, const std::vector<float>& b) {
	if (a.size() != b.size()) {
		return 0;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i] != b[i]) {
			return static_cast<long>(i);
		}
	}
	return -1;
}

} // namespace

TEST_CASE("resonate engine: the engine list and the model list name what ships") {
	CHECK(feathers::kOscTypeNames[static_cast<size_t>(OscType::Resonate)] == std::string("Resonate"));
	CHECK(static_cast<int>(OscType::Resonate) == 3);
	CHECK(feathers::oscTypeFromValue(3.0) == OscType::Resonate);
	// The Choice's labels are the physics' own names, entry for entry — rename a model in
	// weave_models.cpp and this is where the menu is told.
	REQUIRE(feathers::kResonateModelNames.size() == static_cast<size_t>(feathers::kWeaveModelCount));
	for (int i = 0; i < feathers::kWeaveModelCount; ++i) {
		CAPTURE(i);
		CHECK(std::string(feathers::kResonateModelNames[static_cast<size_t>(i)])
		      == feathers::weave::modelName(static_cast<WeaveModel>(i)));
	}
	ParamSet params;
	char text[CLAP_NAME_SIZE] = {};
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::OscType), 3.0, text, sizeof(text)));
	CHECK(std::string(text) == "Resonate");
	REQUIRE(params.valueToText(static_cast<clap_id>(ParamId::ResonateModel), 3.0, text, sizeof(text)));
	CHECK(std::string(text) == "Membrane");
}

TEST_CASE("resonate engine: renders a finite, non-silent note") {
	Rig rig;
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 48000, 256);
	bool finite = true;
	for (const float s : out) {
		finite = finite && std::isfinite(s);
	}
	CHECK(finite);
	// Well above the floor any engine's quiet zone reaches; the loudness itself is the Stair case's.
	CHECK(rmsOf(out, 4800, out.size()) > 0.02);
}

TEST_CASE("resonate engine: ONE string — a note plucks the string every voice is reading") {
	// THE SHARED-STRING DECISION AS VALUES (task-43 decision 4). Two synths play the same first note; one
	// then plays a second. The string's energy at the same instant differs between them by what the
	// second note's pluck put in — so the pluck reached the ONE string, not a string of the second
	// voice's own. (That every voice reads the synth's string is true by construction: the synth hands every
	// voice the one pointer at construction, and nothing else ever sets it.)
	constexpr uint32_t kSecond = 24000;
	constexpr uint32_t kAfter = kSecond + 400; // past the string's next tick (~139 samples at 48 kHz)
	Rig one;
	Rig two;
	one.play({ noteOn(0, kKeyC4, 1) }, kSecond, 256);
	two.play({ noteOn(0, kKeyC4, 1) }, kSecond, 256);
	const float before = two.synth.weaveForTest().energyForTest();
	CHECK(before == one.synth.weaveForTest().energyForTest()); // the same history so far, bit for bit

	one.play({}, kAfter - kSecond, 256);
	two.play({ noteOn(0, kKeyG4, 2) }, kAfter - kSecond, 256);
	const float alone = one.synth.weaveForTest().energyForTest();
	const float plucked = two.synth.weaveForTest().energyForTest();
	MESSAGE("string energy at +400 samples: one note " << alone << ", two notes " << plucked);
	CHECK(plucked > alone + 0.05f);
}

TEST_CASE("resonate engine: a note on another engine does not pluck the string") {
	// The pluck is Resonate's: a Morph note leaves the string where its own history put it.
	Rig morph(OscType::Morph);
	Rig quiet(OscType::Morph);
	morph.play({ noteOn(0, kKeyC4, 1) }, 4800, 256);
	quiet.play({}, 4800, 256);
	CHECK(morph.synth.weaveForTest().energyForTest() == quiet.synth.weaveForTest().energyForTest());
}

TEST_CASE("resonate engine: output identical at any host block size") {
	// The string ticks on its own clock and the render loop cuts at that tick; the voices read one fade
	// array per chunk and generate up to three samples past it. None of that may depend on where the
	// host's buffers fall. Notes, a chord, a character move (Path) and a model switch, at odd frames.
	constexpr uint32_t kFrames = 24000;
	const std::vector<Event> script {
		noteOn(0, kKeyC4, 1),
		noteOn(37, kKeyG4, 2),
		param(3001, ParamId::Path, 0.4),
		noteOff(5003, kKeyC4, 1),
		param(9007, ParamId::ResonateModel, 3.0),
		noteOn(12011, kKeyC3, 3),
		param(15013, ParamId::Path, 0.9),
		noteOff(20021, kKeyG4, 2),
	};
	Rig reference;
	const std::vector<float> first = reference.play(script, kFrames, 256);
	REQUIRE(peakOf(first) > 0.05f); // NOT SILENCE (backlog MA)
	for (const int block : { 1, 37, 128, 441, 4096 }) {
		CAPTURE(block);
		Rig rig;
		CHECK(firstDifference(rig.play(script, kFrames, block), first) == -1);
	}
}

TEST_CASE("resonate engine: its laws follow the character") {
	// A VALUE, read off the string: one sound, standing on it (zero travel), so the laws the string was
	// handed are the firmware builder's at that sound's home phase — computed here by the linked port,
	// not by the synth's road. Ring: the per-node stiffness and home the string integrates with ARE the laws'.
	const auto lawsOf = [](double zone) {
		Rig rig;
		rig.set(ParamId::AZone, zone);
		rig.set(ParamId::BEnable, 0.0);
		rig.play({}, 2048, 256); // a few string ticks, so the laws have been taken
		const feathers::WeaveString& s = rig.synth.weaveForTest();
		std::vector<float> out(s.nodeStiffnessForTest(), s.nodeStiffnessForTest() + feathers::WeaveString::kNodes);
		out.insert(out.end(), s.nodeHomeForTest(), s.nodeHomeForTest() + feathers::WeaveString::kNodes);
		return out;
	};
	for (const double zone : { 17.0, 600.0 }) {
		CAPTURE(zone);
		const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(zone / 1023.0);
		const std::vector<float> got = lawsOf(zone);
		constexpr size_t kN = feathers::WeaveString::kNodes;
		for (size_t i = 0; i < kN; ++i) {
			CAPTURE(i);
			CHECK(got[i] == laws.stiffness[i]);
			CHECK(got[kN + i] == laws.home[i]);
		}
	}
	// The second assertion, proving the first can fail: two characters, two different law sets.
	// (Stiffness sits on its 0.016 floor at both of these zones; the home shape is what differs.)
	CHECK(lawsOf(17.0) != lawsOf(600.0));
	CHECK(lawsOf(17.0) == lawsOf(600.0 - 583.0));
}

TEST_CASE("resonate engine: two sounds blend their laws and crossfade their bow modes") {
	// Path half way between two sounds: the string's A and B sides carry the same blended fields, and
	// the crossfade is the second-heaviest sound's share of the pair (buildWeaveBlend).
	std::vector<owlet::dsp::PhiWeaveParams> laws { owlet::dsp::buildPhiWeaveLaws(0.1),
		                                             owlet::dsp::buildPhiWeaveLaws(0.7) };
	const std::vector<double> weights { 0.25, 0.75 };
	const feathers::WeaveCharacter w = feathers::buildWeaveBlend(laws, weights);
	CHECK(w.crossfade == doctest::Approx(0.25).epsilon(1e-9));
	CHECK(w.a.bowMode == laws[1].bowMode);
	CHECK(w.b.bowMode == laws[0].bowMode);
	for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
		CAPTURE(i);
		const float k = 0.25f * laws[0].stiffness[i] + 0.75f * laws[1].stiffness[i];
		CHECK(std::abs(w.a.stiffness[i] - k) <= 1e-7f);
		CHECK(w.a.stiffness[i] == w.b.stiffness[i]);
	}
	// All the weight on one sound is that sound, to the bit.
	const feathers::WeaveCharacter solo = feathers::buildWeaveBlend(laws, std::vector<double> { 0.0, 1.0 });
	for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
		CHECK(solo.a.home[i] == laws[1].home[i]);
	}
	CHECK(solo.crossfade == 0.0f);
}

TEST_CASE("resonate engine: the Model param reaches the string") {
	for (int m = 0; m < feathers::kWeaveModelCount; ++m) {
		CAPTURE(m);
		Rig rig;
		rig.set(ParamId::ResonateModel, static_cast<double>(m));
		rig.play({ noteOn(0, kKeyC4, 1) }, 2048, 256);
		CHECK(rig.synth.weaveForTest().model() == static_cast<WeaveModel>(m));
	}
}

TEST_CASE("resonate engine: a note sits within a measured window of a Stair note's loudness") {
	// THE GAIN CONVENTION, APPLIED ONCE AND MEASURED. The string's tables are on the Stair renderer's
	// scale (a nominal zone at half the q31 rail, the rail at 1.0 — weave_string.cpp, stair_renderer.cpp);
	// the firmware's render-time doubling is on neither side. So a Resonate note and a Stair note of the
	// same character coordinate at the same level should land near each other. The window is what was
	// MEASURED on 2026-09-25 across these zones at C3 (worst 3.67 dB, zone 600; spread -3.67..+3.13 dB), widened
	// to 4.5 dB — not a model's prediction. RE-MEASURED 2026-09-26 at C2, C4 and C6 too: pitch moves it by
	// hundredths of a dB up to C4, and C6 is the worst at 3.81 dB (zone 600; spread -3.81..+3.34 dB). The gap
	// is the character's, not the pitch's, and the 4.5 dB bound holds without widening.
	double worst = 0.0;
	for (const int16_t key : { int16_t { 36 }, kKeyC3, kKeyC4, int16_t { 84 } }) { // C2, C3, C4, C6
		for (const double zone : { 17.0, 200.0, 400.0, 600.0, 800.0, 1000.0 }) {
			const auto rms = [&](OscType engine) {
				Rig rig(engine);
				rig.set(ParamId::AZone, zone);
				rig.set(ParamId::BEnable, 0.0);
				const std::vector<float> out = rig.play({ noteOn(0, key, 1) }, 96000, 256);
				return rmsOf(out, 24000, out.size());
			};
			const double resonate = rms(OscType::Resonate);
			const double stair = rms(OscType::Stair);
			const double db = 20.0 * std::log10(resonate / stair);
			MESSAGE("key " << key << " zone " << zone << ": Resonate RMS " << resonate << ", Stair RMS " << stair
			               << ", " << db << " dB");
			worst = std::max(worst, std::abs(db));
			CHECK(db > -kResonateVsStairDb);
			CHECK(db < kResonateVsStairDb);
		}
	}
	MESSAGE("worst |Resonate - Stair| = " << worst << " dB");
}

TEST_CASE("resonate engine: the picture's rest cycle is what a freshly reset string's table holds") {
	// weaveRestCycle restates the string's settle (rest pose, AGC without its slew) for the pictures,
	// which have no string to ask. This is what keeps that restatement honest: a reset string's cur
	// table IS the parked scan, and the picture must be it to the bit, for every model.
	for (int m = 0; m < feathers::kWeaveModelCount; ++m) {
		for (const double phase : { 0.0, 0.137, 0.5, 0.618, 0.9 }) {
			CAPTURE(m);
			CAPTURE(phase);
			const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(phase);
			feathers::WeaveString string;
			string.prepare(kSampleRate);
			string.setModel(static_cast<WeaveModel>(m));
			string.setLaws(laws, laws);
			string.reset();
			float cycle[feathers::WeaveString::kNodes];
			feathers::weaveRestCycle(static_cast<WeaveModel>(m), { laws, laws, 0.0f }, cycle);
			for (int i = 0; i < feathers::WeaveString::kNodes; ++i) {
				CAPTURE(i);
				CHECK(cycle[i] == string.tableForTest(0, i));
			}
		}
	}
}

namespace {

/// The string's effective laws for a two-sided WeaveCharacter, in the string's own form
/// ((1 - cf) a + cf b), and the bow force as a weight per MODE — (1 - cf) on A's mode plus cf on B's —
/// which is what `(1 - cf) shapeA + cf shapeB` in weave_string.cpp applies.
struct Effective {
	std::vector<float> fields;
	std::array<float, owlet::dsp::kWeaveBowModeCount> bow {};
};

Effective effectiveOf(const feathers::WeaveCharacter& w) {
	Effective e;
	const float cf = w.crossfade;
	const float inv = 1.0f - cf;
	const auto add = [&](float a, float b) { e.fields.push_back(inv * a + cf * b); };
	for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
		add(w.a.stiffness[i], w.b.stiffness[i]);
		add(w.a.coupling[i], w.b.coupling[i]);
		add(w.a.damping[i], w.b.damping[i]);
		add(w.a.bowBalance[i], w.b.bowBalance[i]);
		add(w.a.home[i], w.b.home[i]);
	}
	add(w.a.bowDepth, w.b.bowDepth);
	add(w.a.bowRate, w.b.bowRate);
	add(w.a.bowPos, w.b.bowPos);
	add(w.a.bowSpread, w.b.bowSpread);
	add(w.a.travelRate, w.b.travelRate);
	add(w.a.pluckPos, w.b.pluckPos);
	add(w.a.pluckWidth, w.b.pluckWidth);
	add(w.a.pluckAmp, w.b.pluckAmp);
	add(w.a.morphBowGain, w.b.morphBowGain);
	add(w.a.outGain, w.b.outGain);
	add(w.a.shimmerAlpha, w.b.shimmerAlpha);
	e.bow[static_cast<size_t>(w.a.bowMode)] += inv;
	e.bow[static_cast<size_t>(w.b.bowMode)] += cf;
	return e;
}

/// Two law sets whose bow modes differ, so the mode half of the construction is exercised.
std::pair<owlet::dsp::PhiWeaveParams, owlet::dsp::PhiWeaveParams> twoModes() {
	const owlet::dsp::PhiWeaveParams first = owlet::dsp::buildPhiWeaveLaws(0.1);
	for (int k = 1; k < 200; ++k) {
		const owlet::dsp::PhiWeaveParams other = owlet::dsp::buildPhiWeaveLaws(0.1 + 0.013 * k);
		if (other.bowMode != first.bowMode) {
			return { first, other };
		}
	}
	FAIL("no second bow mode found");
	return { first, first };
}

} // namespace

TEST_CASE("resonate engine: two sounds are the firmware's A/B lerp at the crossfade") {
	// THE TWO-SOUND REDUCTION as values: at weights (1 - w, w) the string's effective laws are the
	// firmware's lerp (1 - w) s0 + w s1, field for field, and its bow force weighs mode0 by 1 - w and
	// mode1 by w — whichever sound the construction put on side A.
	const auto [s0, s1] = twoModes();
	REQUIRE(s0.bowMode != s1.bowMode);
	for (const double w : { 0.1, 0.3, 0.5, 0.7, 0.9 }) {
		CAPTURE(w);
		const std::vector<owlet::dsp::PhiWeaveParams> laws { s0, s1 };
		const Effective got = effectiveOf(feathers::buildWeaveBlend(laws, std::vector<double> { 1.0 - w, w }));
		const Effective firmware = effectiveOf({ s0, s1, static_cast<float>(w) });
		REQUIRE(got.fields.size() == firmware.fields.size());
		float worst = 0.0f;
		for (size_t i = 0; i < got.fields.size(); ++i) {
			worst = std::max(worst, std::abs(got.fields[i] - firmware.fields[i]));
		}
		CHECK(worst <= 1e-6f);
		for (size_t m = 0; m < got.bow.size(); ++m) {
			CAPTURE(m);
			CHECK(std::abs(got.bow[m] - firmware.bow[m]) <= 1e-6f);
		}
	}
}

TEST_CASE("resonate engine: the dominance swap puts no step in any law or in the bow force") {
	// Sweep the pair's weights through the crossing where the heavier sound changes. A step anywhere —
	// a field, or the bow's per-mode weight — would show as one adjacent difference far larger than the
	// weight step itself. The bound is the smooth one: a field moves at most dw times its span, a mode
	// weight at most dw.
	const auto [s0, s1] = twoModes();
	const std::vector<owlet::dsp::PhiWeaveParams> laws { s0, s1 };
	constexpr double kStep = 0.001;
	const Effective ends0 = effectiveOf({ s0, s0, 0.0f });
	const Effective ends1 = effectiveOf({ s1, s1, 0.0f });
	Effective previous = effectiveOf(feathers::buildWeaveBlend(laws, std::vector<double> { 0.56, 0.44 }));
	bool crossed = false;
	float worstField = 0.0f;
	float worstBow = 0.0f;
	for (double w = 0.44 + kStep; w <= 0.56 + 1e-12; w += kStep) {
		const feathers::WeaveCharacter c = feathers::buildWeaveBlend(laws, std::vector<double> { 1.0 - w, w });
		crossed = crossed || c.a.bowMode == s1.bowMode;
		const Effective now = effectiveOf(c);
		for (size_t i = 0; i < now.fields.size(); ++i) {
			const float span = std::abs(ends1.fields[i] - ends0.fields[i]);
			worstField = std::max(worstField, std::abs(now.fields[i] - previous.fields[i]) - span * static_cast<float>(kStep));
		}
		for (size_t m = 0; m < now.bow.size(); ++m) {
			worstBow = std::max(worstBow, std::abs(now.bow[m] - previous.bow[m]));
		}
		previous = now;
	}
	REQUIRE(crossed); // the sweep did go through the swap
	MESSAGE("largest field step beyond smooth: " << worstField << ", largest bow-weight step: " << worstBow);
	CHECK(worstField <= 1e-6f);
	CHECK(worstBow <= static_cast<float>(kStep) + 1e-6f);
}

TEST_CASE("resonate engine: one sound gives A = B and no crossfade") {
	const owlet::dsp::PhiWeaveParams s = owlet::dsp::buildPhiWeaveLaws(0.37);
	const feathers::WeaveCharacter w =
	    feathers::buildWeaveBlend(std::vector<owlet::dsp::PhiWeaveParams> { s }, std::vector<double> { 1.0 });
	CHECK(w.crossfade == 0.0f);
	CHECK(w.a.bowMode == w.b.bowMode);
	const Effective a = effectiveOf({ w.a, w.a, 0.0f });
	const Effective b = effectiveOf({ w.b, w.b, 0.0f });
	CHECK(a.fields == b.fields);
	CHECK(a.fields == effectiveOf({ s, s, 0.0f }).fields);
}

TEST_CASE("resonate engine: moving between sounds bows the string") {
	// THE MORPH-BOW, reached through the construction: two sounds with the SAME laws, so a weight move
	// changes no law and no home — any energy that appears is cf's motion alone, as in the core's
	// "crossfade motion bows the string". The laws' own bow is off.
	const owlet::dsp::PhiWeaveParams s = owlet::dsp::buildPhiWeaveLaws(0.3);
	const std::vector<owlet::dsp::PhiWeaveParams> laws { s, s };
	const auto run = [&](bool moving) {
		feathers::WeaveString string;
		string.prepare(kSampleRate);
		feathers::WeaveCharacter c = feathers::buildWeaveBlend(laws, std::vector<double> { 1.0, 0.0 });
		string.setLaws(c.a, c.b);
		string.setCrossfade(c.crossfade);
		string.setExcitation({ 0.0f });
		string.reset();
		string.pluck(0.0f);
		const int ticks = static_cast<int>(0.5 * feathers::WeaveString::kTickHz);
		float peak = 0.0f;
		for (int t = 0; t < ticks; ++t) {
			if (moving) {
				const double w = static_cast<double>(t + 1) / static_cast<double>(ticks);
				c = feathers::buildWeaveBlend(laws, std::vector<double> { 1.0 - w, w });
				string.setLaws(c.a, c.b);
				string.setCrossfade(c.crossfade);
			}
			string.advance(string.samplesUntilTick());
			peak = std::max(peak, string.energyForTest());
		}
		return peak;
	};
	const float parked = run(false);
	const float moving = run(true);
	MESSAGE("string energy peak: parked " << parked << ", weights swept A -> B in 0.5 s " << moving);
	CHECK(parked < 1e-9f);
	CHECK(moving > 1e-6f);
}

TEST_CASE("resonate engine: the puck's Path moves the string's crossfade") {
	// The synth half: the crossfade the string is handed follows the puck. Standing on A it is 0 (A has
	// all the weight); walking toward B it rises; half way it is the pair's even split.
	const auto cfAt = [](double path) {
		Rig rig;
		rig.set(ParamId::AZone, 17.0);
		rig.set(ParamId::BZone, 600.0);
		rig.set(ParamId::Path, path);
		rig.play({}, 512, 256);
		return rig.synth.weaveCrossfadeForTest();
	};
	const float at0 = cfAt(0.0);
	const float at15 = cfAt(0.15);
	const float at30 = cfAt(0.3);
	const float at50 = cfAt(0.5);
	MESSAGE("crossfade at Path 0 / 0.15 / 0.3 / 0.5: " << at0 << " " << at15 << " " << at30 << " " << at50);
	CHECK(at0 == 0.0f);
	CHECK(at15 > at0);
	CHECK(at30 > at15);
	CHECK(at50 <= 0.5f);
	CHECK(at50 > at30);
}

TEST_CASE("resonate engine: which of the Explore and tray controls reach the string — the honesty table as values") {
	// THE HONESTY RULE READ OFF THE ENGINE, per control. Each is moved from its default on a two-sound
	// patch standing off both anchors, and the string's laws (every node's stiffness, coupling, damping
	// and home, plus the crossfade) are compared with the unmoved patch — and for the dead ones the
	// rendered note too, to the bit. A live control must move the laws; a dead one must move nothing,
	// and the editor greys it (test_gui_widgets_dahdsr's Resonate subcase for the four operators; the
	// tray's Relief is dead on every engine and built disabled).
	struct Probe {
		const char* name;
		ParamId id;
		double value;
		bool live;
	};
	const Probe probes[] = {
		{ "Quantize", feathers::quantizeParam(0), 0.95, false }, // flagship.cpp buildFlagshipFor only
		{ "Sync", feathers::syncParam(0), 0.95, false },
		{ "Feedback", feathers::feedbackParam(0), 0.95, false },
		{ "Points", feathers::pointsParam(0), 0.95, false },
		{ "Relief", ParamId::Relief, 0.9, false },          // read by no engine at all
		{ "Weather", ParamId::Weather, 0.8, true },         // character_params.h travelX/Y -> buildWeaveSliced
		{ "Gamma", ParamId::GlobalGamma, 0.37, true },      // character_params.h phaseOffset -> sliceHome
		{ "Path", ParamId::Path, 0.7, true },               // the tour point -> the weights -> the blend and cf
	};
	const auto run = [](const Probe* moved) {
		Rig rig;
		rig.set(ParamId::AZone, 17.0);
		rig.set(ParamId::BZone, 600.0);
		rig.set(ParamId::Path, 0.3);
		rig.set(ParamId::PosY, 0.2);
		if (moved != nullptr) {
			rig.set(moved->id, moved->value);
		}
		const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 9600, 256);
		const feathers::WeaveString& s = rig.synth.weaveForTest();
		std::vector<float> laws;
		for (const float* p : { s.nodeStiffnessForTest(), s.nodeGainForTest(), s.nodeHomeForTest() }) {
			laws.insert(laws.end(), p, p + feathers::WeaveString::kNodes);
		}
		laws.push_back(rig.synth.weaveCrossfadeForTest());
		return std::make_pair(laws, out);
	};
	const auto [baseLaws, baseOut] = run(nullptr);
	// NOT SILENCE (backlog MA): the dead controls' "the note, to the bit" half compares against this render.
	REQUIRE(peakOf(baseOut) > 0.05f);
	for (const Probe& p : probes) {
		const std::string name = p.name;
		CAPTURE(name);
		const auto [laws, out] = run(&p);
		if (p.live) {
			CHECK(laws != baseLaws);
		}
		else {
			CHECK(laws == baseLaws);
			CHECK(firstDifference(out, baseOut) == -1);
		}
	}
}

TEST_CASE("resonate engine: a voice scans the string on the mip its note asks for, and a glide moves it") {
	// THE MIP IS A VALUE, read off the voice. 700 / 1800 Hz are the thresholds (WeaveString::mipForFrequency).
	const auto mipOf = [](int16_t key) {
		Rig rig;
		rig.play({ noteOn(0, key, 1) }, 512, 256);
		return rig.synth.voiceWeaveMipForTest(rig.synth.slotOfKeyForTest(key));
	};
	CHECK(mipOf(71) == 0); // 493.9 Hz
	CHECK(mipOf(83) == 1); // 987.8 Hz
	CHECK(mipOf(95) == 2); // 1975.5 Hz

	// A mono glide from 493.9 Hz to 1975.5 Hz starts on mip 0 and ends on mip 2.
	Rig rig;
	rig.set(ParamId::VoiceMode, 1.0); // Mono
	rig.set(ParamId::GlideTime, std::cbrt(0.05 / feathers::kGlideMaxSeconds));
	rig.play({ noteOn(0, 71, 1) }, 4800, 256);
	CHECK(rig.synth.voiceWeaveMipForTest(rig.synth.slotOfKeyForTest(71)) == 0);
	rig.play({ noteOn(0, 95, 2) }, 48000, 256);
	const int slot = rig.synth.slotOfKeyForTest(95);
	REQUIRE(slot >= 0);
	CHECK(rig.synth.voicePitchForTest(slot) == doctest::Approx(95.0).epsilon(1e-4));
	CHECK(rig.synth.voiceWeaveMipForTest(slot) == 2);
}

TEST_CASE("resonate engine: MEASURED — a 3 kHz note's upper harmonics and its off-harmonic energy, on the band-limited mip") {
	// THE VALUE THE MIP IS FOR. A note at 2959.96 Hz (key 102) at 48 kHz, the string settled, one
	// 65536-sample window: the share of the spectrum's energy that lies more than 100 Hz from any
	// harmonic (the bow's haptic-rate sidebands sit well inside that), and the energy above 12 kHz, both
	// against the whole. MEASURED 2026-09-26 on mip 2 (the fix) and on mip 0 (the old placement's
	// known-bad).
	constexpr int16_t kKey = 102;
	constexpr size_t kN = 65536;
	Rig rig;
	const std::vector<float> out = rig.play({ noteOn(0, kKey, 1) }, 24000 + kN, 256);
	CHECK(rig.synth.voiceWeaveMipForTest(rig.synth.slotOfKeyForTest(kKey)) == 2);
	const std::vector<float> window(out.begin() + 24000, out.end());
	const feathers::test::Window win(static_cast<int>(kN));
	const std::vector<double> spectrum = feathers::test::amplitudeSpectrum(window, win);
	const double f0 = feathers::test::noteToHz(kKey);
	double total = 0.0;
	double off = 0.0;
	double high = 0.0;
	for (size_t k = 1; k < spectrum.size(); ++k) {
		const double hz = static_cast<double>(k) * kSampleRate / static_cast<double>(kN);
		const double e = spectrum[k] * spectrum[k];
		total += e;
		const double nearest = std::max(1.0, std::round(hz / f0)) * f0;
		if (std::abs(hz - nearest) > 100.0) {
			off += e;
		}
		if (hz > 12000.0) {
			high += e;
		}
	}
	MESSAGE("3 kHz note: off-harmonic energy " << feathers::test::dB(std::sqrt(off / total))
	                                            << " dB, energy above 12 kHz "
	                                            << feathers::test::dB(std::sqrt(high / total)) << " dB, of the whole");
	// WHAT THE MIP BUYS, AND WHAT IT DOES NOT. Mip 2 takes the string's upper harmonics down: above 12 kHz,
	// -43.2 dB against -38.5 dB on mip 0. The off-harmonic share hardly moves (-30.8 against -30.9 dB).
	// That is expected: the 4x path's halfband already removes what would fold, so what sits off the
	// harmonics is the string's own motion (bow and travel sidebands wider than 100 Hz), not aliasing.
	// That part is inferred from the numbers, not separately measured. So the bar is on the upper harmonics.
	CHECK(feathers::test::dB(std::sqrt(high / total)) < kResonateAbove12kDb);
}

TEST_CASE("resonate engine: off Resonate the string freezes, and switching back resumes without a click") {
	// Off Resonate nothing reads the string, so the synth does not tick it: its clock stands still (the
	// tick count, a value) and it resumes where it stopped. The switch back must not click: the largest
	// sample-to-sample step in the 20 ms after it is held against the largest step of the same note
	// playing steadily on either engine — the engine switch itself crossfades the two over the
	// bank's fade, so anything a frozen-then-resumed string added would show as a step above that.
	Rig rig;
	std::vector<float> steady = rig.play({ noteOn(0, kKeyC3, 1) }, 24000, 256);
	REQUIRE(peakOf(steady) > 0.05f); // NOT SILENCE (backlog MA): over silence both steps are 0 and 0 <= 0
	float steadyStep = 0.0f;
	for (size_t i = 12001; i < steady.size(); ++i) {
		steadyStep = std::max(steadyStep, std::abs(steady[i] - steady[i - 1]));
	}
	const uint64_t before = rig.synth.weaveForTest().ticksForTest();
	rig.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	const std::vector<float> morphed = rig.play({}, 24000, 256);
	CHECK(rig.synth.weaveForTest().ticksForTest() == before); // frozen
	// AND THE OTHER ENGINE'S OWN ROUGHNESS, because the switch back crossfades FROM it: for the bank's fade the output
	// is a mix of the two, and its largest step can be either engine's. Measured on the Morph half's settled second
	// half. Before the fade was the rebuild interval (backlog PB (c), 5.3 ms) the 2 ms fade happened to miss the Morph
	// cycle's steepest edge (C3's period is 7.6 ms), so a bound on Resonate's roughness alone held by luck.
	float morphStep = 0.0f;
	for (size_t i = 12001; i < morphed.size(); ++i) {
		morphStep = std::max(morphStep, std::abs(morphed[i] - morphed[i - 1]));
	}
	rig.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	const std::vector<float> back = rig.play({}, 960, 256);
	REQUIRE(peakOf(back) > 0.05f); // and the note is still sounding after the switch back
	float backStep = 0.0f;
	for (size_t i = 1; i < back.size(); ++i) {
		backStep = std::max(backStep, std::abs(back[i] - back[i - 1]));
	}
	CHECK(rig.synth.weaveForTest().ticksForTest() > before); // and resumed
	MESSAGE("largest step: steady Resonate " << steadyStep << ", steady Morph " << morphStep << ", first 20 ms after switching back " << backStep);
	CHECK(backStep <= 1.5f * std::max(steadyStep, morphStep));
}

TEST_CASE("MEASURE: what a frozen string saves per render chunk" * doctest::skip()) {
	// The work the render loop skips off Resonate, timed in isolation: the fade fill (m + 3 fadeAt), the
	// per-voice setter loop (24 slots), and advance(m), per 32-sample chunk at 48 kHz — against a whole
	// Morph synth chunk with one note, for scale. Rough by design (a shared host; alternate and repeat).
	constexpr int kChunks = 200000;
	constexpr int kM = 32;
	feathers::WeaveString string;
	string.prepare(kSampleRate);
	std::array<feathers::WeaveString::Fade, kM + 3> fades {};
	std::array<const feathers::WeaveString::Fade*, 24> setters {};
	const auto t0 = std::chrono::steady_clock::now();
	int left = 0;
	for (int c = 0; c < kChunks; ++c) {
		int m = std::min(kM, string.samplesUntilTick());
		for (int k = 0; k < m + 3; ++k) {
			fades[static_cast<size_t>(k)] = string.fadeAt(k);
		}
		for (auto& s : setters) {
			s = fades.data();
		}
		string.advance(m);
		left += m;
	}
	const auto t1 = std::chrono::steady_clock::now();
	Rig rig(OscType::Morph);
	rig.play({ noteOn(0, kKeyC3, 1) }, 256, 256);
	std::vector<float> l(kM);
	std::vector<float> r(kM);
	const auto t2 = std::chrono::steady_clock::now();
	for (int c = 0; c < 20000; ++c) {
		rig.synth.render(l.data(), r.data(), kM);
	}
	const auto t3 = std::chrono::steady_clock::now();
	const double stringNs = std::chrono::duration<double, std::nano>(t1 - t0).count() / kChunks;
	const double chunkNs = std::chrono::duration<double, std::nano>(t3 - t2).count() / 20000.0;
	MESSAGE("string work per chunk (fill + setters + advance): " << stringNs << " ns; a Morph synth chunk: "
	                                                              << chunkNs << " ns (" << left << " samples)");
}

TEST_CASE("resonate engine: the string advances AFTER the voices read it — no step at the tick") {
	// THE ORDER, OBSERVED. The render loop fills the chunk's fades, lets every voice read the string,
	// then advance(m) — which runs the tick when the chunk ends on one. Advanced first, the last chunk of
	// every window would read the NEXT tick's tables at the old fade positions, then drop back to them
	// at the window's start: a step at every tick boundary. So: a low note right after its pluck (the
	// string moving hardest), and the largest second difference of the output near tick boundaries held
	// against the largest anywhere else. In order, nothing marks the boundaries.
	Rig rig;
	const std::vector<float> out = rig.play({ noteOn(0, 36, 1) }, 24000, 256); // C2
	REQUIRE(peakOf(out) > 0.05f); // NOT SILENCE (backlog MA): a silent output has no roughness anywhere
	const int tick = rig.synth.weaveForTest().tickLength();
	float nearTick = 0.0f;
	float elsewhere = 0.0f;
	for (size_t i = 2400; i + 1 < out.size(); ++i) {
		const float d2 = std::abs(out[i + 1] - 2.0f * out[i] + out[i - 1]);
		const int phase = static_cast<int>(i % static_cast<size_t>(tick));
		const bool near = phase < 12 || phase > tick - 12;
		(near ? nearTick : elsewhere) = std::max(near ? nearTick : elsewhere, d2);
	}
	MESSAGE("largest second difference: within 12 samples of a tick " << nearTick << ", elsewhere " << elsewhere);
	// MEASURED 2026-09-26: in order, 9.3e-4 near a tick and 1.6e-3 elsewhere. Advanced first (the known-bad),
	// 1.1e-2 and 1.0e-2: the step does not stay at the boundaries (the fade positions and the tables
	// disagree for the whole last chunk of each window, and the halfband spreads it), so the bar is on
	// the output's roughness as a whole. The block-size case goes red on the same mistake too.
	CHECK(std::max(nearTick, elsewhere) < 4e-3f);
}

TEST_CASE("resonate engine: with three sounds, both kinds of dominance swap are seamless") {
	// The third-sound term (2026-09-26): cf = (w2 - w3) / ((w1 - w3) + (w2 - w3)). Two sweeps, each a
	// straight line in the weights through one kind of swap:
	//  - the 2nd and 3rd heaviest trading (0.5, 0.25 + e, 0.25 - e): B's mode changes there, and must
	//    carry no weight when it does;
	//  - the 1st and 2nd trading (0.4 + e, 0.4 - e, 0.2): A and B exchange modes at cf = 0.5.
	// Along a straight line every field moves at a constant rate, so no step may exceed that rate times
	// the step; the bow's per-mode weights may not jump either (the old w2 / (w1 + w2) stepped by about
	// 1/3 at the first kind).
	std::vector<owlet::dsp::PhiWeaveParams> laws;
	for (int k = 0; laws.size() < 3 && k < 400; ++k) {
		const owlet::dsp::PhiWeaveParams p = owlet::dsp::buildPhiWeaveLaws(0.05 + 0.011 * k);
		bool fresh = true;
		for (const auto& q : laws) {
			fresh = fresh && q.bowMode != p.bowMode;
		}
		if (fresh) {
			laws.push_back(p);
		}
	}
	REQUIRE(laws.size() == 3);
	constexpr double kStep = 0.0005;
	const auto sweep = [&](auto weightsAt, const char* name) {
		CAPTURE(std::string(name));
		const std::vector<double> w0 = weightsAt(-0.02);
		const std::vector<double> w1 = weightsAt(0.02);
		// The fields' own rate along this line: |sum_k dw_k f_k| per unit of e, per field.
		std::vector<Effective> ends;
		for (const auto& l : laws) {
			ends.push_back(effectiveOf({ l, l, 0.0f }));
		}
		Effective previous = effectiveOf(feathers::buildWeaveBlend(laws, w0));
		float worstField = 0.0f;
		float worstBow = 0.0f;
		for (double e = -0.02 + kStep; e <= 0.02 + 1e-12; e += kStep) {
			const Effective now = effectiveOf(feathers::buildWeaveBlend(laws, weightsAt(e)));
			for (size_t i = 0; i < now.fields.size(); ++i) {
				double rate = 0.0;
				for (size_t k = 0; k < 3; ++k) {
					rate += (w1[k] - w0[k]) / 0.04 * static_cast<double>(ends[k].fields[i]);
				}
				const double smooth = std::abs(rate) * kStep;
				worstField = std::max(worstField, static_cast<float>(std::abs(now.fields[i] - previous.fields[i]) - smooth));
			}
			for (size_t m = 0; m < now.bow.size(); ++m) {
				worstBow = std::max(worstBow, std::abs(now.bow[m] - previous.bow[m]));
			}
			previous = now;
		}
		MESSAGE(std::string(name) << ": largest field step beyond smooth " << worstField << ", largest bow-weight step " << worstBow);
		CHECK(worstField <= 1e-6f);
		CHECK(worstBow <= 50.0f * static_cast<float>(kStep)); // 0.025: a smooth crossing, not a 1/3 jump
	};
	sweep([](double e) { return std::vector<double> { 0.5, 0.25 + e, 0.25 - e }; }, "2nd/3rd swap");
	sweep([](double e) { return std::vector<double> { 0.4 + e, 0.4 - e, 0.2 }; }, "1st/2nd swap");
}

TEST_CASE("resonate engine: a voice fading out of Resonate reads ONE frozen fade, not a replayed chunk") {
	// Switch a sounding Resonate note to Morph and render the bank's 2 ms fade in 8-sample host blocks.
	// Every render call restarts a voice's fade index, so a voice handed the last live chunk's array
	// would replay that chunk's string path from its top at each call: a sawtooth at the block rate.
	// VALUES: the voice is handed exactly one entry, and the fade it reads (segment, position, travel) is
	// the same after every render call — a static cycle.
	Rig rig;
	rig.play({ noteOn(0, kKeyC3, 1) }, 24000 + 57, 256); // off a chunk boundary, so a live chunk was part-read
	rig.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	const int slot = rig.synth.slotOfKeyForTest(kKeyC3);
	REQUIRE(slot >= 0);
	rig.play({}, 8, 8);
	const feathers::WeaveString::Fade first = rig.synth.voiceWeaveFadeReadForTest(slot);
	CHECK(rig.synth.voiceWeaveFadeCountForTest(slot) == 1);
	for (int call = 0; call < 11; ++call) {
		CAPTURE(call);
		rig.play({}, 8, 8);
		const feathers::WeaveString::Fade now = rig.synth.voiceWeaveFadeReadForTest(slot);
		CHECK(now.segment == first.segment);
		CHECK(now.t == first.t);
		CHECK(now.travel == first.travel);
	}
}

TEST_CASE("resonate engine: a Path move made while the string was frozen does not jolt it on the way back") {
	// Off Resonate the string does not tick, but its laws and crossfade keep following the character, the
	// crossfade RE-SEATED so no motion is owed. So a Path sweep made on Morph arrives at the first live
	// tick as no crossfade motion at all: the morph-bow force that tick applies is zero (a value, read off
	// the string).
	Rig rig;
	rig.set(ParamId::AZone, 17.0);
	rig.set(ParamId::BZone, 600.0);
	rig.set(ParamId::Path, 0.0);
	rig.play({ noteOn(0, kKeyC3, 1) }, 24000, 256);
	rig.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	rig.play({}, 4800, 256);
	rig.set(ParamId::Path, 0.9);
	rig.play({}, 4800, 256);
	// The frozen string's crossfade followed Path: 0 standing on A, 0.1 at Path 0.9 (B now the heavier, A's
	// share 0.1 of the pair).
	CHECK(rig.synth.weaveCrossfadeForTest() == doctest::Approx(0.1).epsilon(0.02));
	rig.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	// Exactly to the first live tick, so the seam reports THAT tick's morph-bow and not a later one's.
	rig.play({}, static_cast<uint32_t>(rig.synth.weaveForTest().samplesUntilTick()), 256);
	REQUIRE(rig.synth.weaveForTest().ticksForTest() > 0);
	MESSAGE("morph-bow at the first live tick: " << rig.synth.weaveForTest().lastMorphBowForTest());
	CHECK(rig.synth.weaveForTest().lastMorphBowForTest() < 1e-6f);
}

TEST_CASE("resonate engine: a Resonate patch starts at its own rest pose") {
	// The character is pushed BEFORE the string is prepared, so activate() seats it at this patch's pose:
	// the laws it integrates with are the patch's (zone 600, not phase 0's), and it is at rest about them.
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Resonate));
	params.setBase(static_cast<clap_id>(ParamId::AZone), 600.0);
	params.setBase(static_cast<clap_id>(ParamId::BEnable), 0.0);
	Synth synth { params };
	synth.activate(kSampleRate, 4096);
	const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(600.0 / 1023.0);
	for (int i = 0; i < feathers::WeaveString::kNodes; ++i) {
		CAPTURE(i);
		CHECK(synth.weaveForTest().nodeHomeForTest()[i] == laws.home[i]);
	}
	CHECK(synth.weaveForTest().energyForTest() < 1e-9f);
}

TEST_CASE("resonate engine: a note mono priority rejects does not pluck, and an accepted one does") {
	// The pluck belongs to a note that STARTS SOUNDING. Mono with Low priority holds C3; a higher note
	// is rejected, so the string's energy one tick later is exactly that of the rig which only held C3.
	// With Last priority the same note is accepted (the voice moves to it) and it plucks.
	const auto energy = [](double priority, bool second) {
		Rig rig;
		rig.set(ParamId::VoiceMode, 1.0); // Mono
		rig.set(ParamId::NotePriority, priority);
		std::vector<Event> script { noteOn(0, kKeyC3, 1) };
		if (second) {
			script.push_back(noteOn(12000, kKeyC4, 2));
		}
		rig.play(script, 12400, 256);
		return rig.synth.weaveForTest().energyForTest();
	};
	const float held = energy(1.0, false);        // Low priority, C3 alone
	const float rejected = energy(1.0, true);     // Low priority, C4 rejected
	const float heldLast = energy(0.0, false);    // Last priority, C3 alone
	const float accepted = energy(0.0, true);     // Last priority, C4 accepted
	MESSAGE("string energy: Low held " << held << ", rejected " << rejected << "; Last held " << heldLast
	                                   << ", accepted " << accepted);
	CHECK(rejected == held);
	CHECK(accepted > heldLast + 0.05f);
}

//------------------------------------------------------------------------------------------------//
// VOICE STACKING (backlog KE) ON RESONATE: stacked parts are detuned scan heads on the ONE string    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("resonate engine: a stacked note's parts all read the one string, at the same place in its fade") {
	// Decided 2026-09-26, following the shared-string decision: a stack's parts are detuned scan heads on
	// the ONE string. VALUES: every part of a four-part stack was handed the synth's fade for the chunk
	// (the same count) and read the same fade entry at the same moment — one string, one clock — and each
	// scans on the mip its own frequency asks for.
	Rig rig;
	rig.set(ParamId::StackCount, 4.0);
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 4800 + 57, 256);
	const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
	REQUIRE(slot >= 0);
	const feathers::WeaveString::Fade first = rig.synth.voiceWeaveFadeReadForTest(slot, 0);
	for (int part = 0; part < 4; ++part) {
		CAPTURE(part);
		CHECK(rig.synth.voiceWeaveFadeCountForTest(slot, part) == rig.synth.voiceWeaveFadeCountForTest(slot, 0));
		const feathers::WeaveString::Fade f = rig.synth.voiceWeaveFadeReadForTest(slot, part);
		CHECK(f.segment == first.segment);
		CHECK(f.t == first.t);
		CHECK(f.travel == first.travel);
		CHECK(rig.synth.voiceWeaveMipForTest(slot, part) == feathers::WeaveString::mipForFrequency(261.63));
	}
	CHECK(rmsOf(out, 2400, out.size()) > 0.02);
}

TEST_CASE("resonate engine: a stacked note plucks once — the string's energy is the unstacked note's") {
	// One note, one pluck, however many parts sound it: the pluck is the NOTE's (pluckForNoteOn), and
	// the string cannot tell how many heads read it. VALUES: string energy one tick after the note is the
	// same, to the bit, at Stack 1 and Stack 4 — and a second note's pluck is the control that shows a
	// pluck would have been visible.
	const auto energyAt = [](double stack, bool secondNote) {
		Rig rig;
		rig.set(ParamId::StackCount, stack);
		std::vector<Event> script { noteOn(0, kKeyC3, 1) };
		if (secondNote) {
			script.push_back(noteOn(12000, kKeyC4, 2));
		}
		rig.play(script, 12400, 256);
		return rig.synth.weaveForTest().energyForTest();
	};
	const float one = energyAt(1.0, true);
	const float four = energyAt(4.0, true);
	const float noSecond = energyAt(4.0, false);
	MESSAGE("string energy after a second note: Stack 1 " << one << ", Stack 4 " << four << "; no second note " << noSecond);
	CHECK(four == one);
	CHECK(four > noSecond + 0.05f);
}

TEST_CASE("resonate engine: a stacked Resonate note renders identically at any host block size") {
	constexpr uint32_t kFrames = 12000;
	const std::vector<Event> script {
		noteOn(0, kKeyC4, 1),
		noteOn(37, kKeyG4, 2),
		param(3001, ParamId::StackCount, 3.0),
		noteOff(5003, kKeyC4, 1),
		param(7007, ParamId::Path, 0.6),
		noteOn(9011, kKeyC3, 3),
	};
	Rig reference;
	reference.set(ParamId::StackCount, 5.0);
	const std::vector<float> first = reference.play(script, kFrames, 256);
	REQUIRE(peakOf(first) > 0.05f); // NOT SILENCE (backlog MA)
	for (const int block : { 1, 37, 441 }) {
		CAPTURE(block);
		Rig rig;
		rig.set(ParamId::StackCount, 5.0);
		CHECK(firstDifference(rig.play(script, kFrames, block), first) == -1);
	}
}

TEST_CASE("resonate engine: a moving puck rebuilds the string's laws every tick and never solves its rest pose") {
	// THE SYNTH NEVER ASKS for the string's motion energy or rest pose, so a crossfade moving every tick —
	// the laws rebuilt every tick — costs it no rest-pose solve at all (weave_string.cpp's lazy solve; the
	// dense solve in the rebuild cost up to ~20 us a tick, bench_weave_models).
	Rig rig;
	rig.set(ParamId::AZone, 17.0);
	rig.set(ParamId::BZone, 600.0);
	rig.play({ noteOn(0, kKeyC4, 1) }, 256, 256);
	const float cfBefore = rig.synth.weaveCrossfadeForTest();
	const uint64_t ticksBefore = rig.synth.weaveForTest().ticksForTest();
	constexpr int kBlocks = 400;
	for (int b = 0; b < kBlocks; ++b) {
		rig.set(ParamId::Path, 0.5 * static_cast<double>(b + 1) / kBlocks);
		rig.play({}, 128, 128);
	}
	const feathers::WeaveString& s = rig.synth.weaveForTest();
	CAPTURE(s.ticksForTest() - ticksBefore);
	// NOT VACUOUS: the string ticked through the sweep and its crossfade moved.
	CHECK(s.ticksForTest() - ticksBefore > 300);
	CHECK(rig.synth.weaveCrossfadeForTest() > cfBefore + 0.1f);
	CHECK(s.restPoseSolvesForTest() == 0);
}

//------------------------------------------------------------------------------------------------//
// RESONATE'S OWN SHAPING: the nine knobs round the Explore disk on Resonate                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("resonate shaping: each scaler moves its law by the stated factor, clamped to the builder's range") {
	// shapeWeaveLaws, restated independently: every law times its factor, then clamped to the range the
	// laws' builder clamps it to (phi_weave.cpp); damping DIVIDED by Decay (up = a longer ring) with the bow
	// balance recomputed by the builder's own law; a forced bow mode; and a factor of exactly 1 is the laws
	// to the bit.
	for (const double phase : { 0.1, 0.37, 0.72 }) {
		CAPTURE(phase);
		const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(phase);
		for (const float f : { 0.25f, 0.5f, 2.0f, 4.0f }) {
			CAPTURE(f);
			feathers::ResonateShaping s;
			s.stiffness = f;
			s.coupling = f;
			s.decay = f;
			s.shimmer = f;
			s.pluck = f;
			s.travel = f;
			s.bowMode = 3; // Noise
			owlet::dsp::PhiWeaveParams got = laws;
			feathers::shapeWeaveLaws(s, got);
			for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
				CAPTURE(i);
				CHECK(got.stiffness[i] == std::clamp(laws.stiffness[i] * f, 0.016f, 0.12f));
				CHECK(got.coupling[i] == std::clamp(laws.coupling[i] * f, 0.004f, 0.45f));
				const float d = std::clamp(laws.damping[i] / f, 0.006f, 0.09f);
				CHECK(got.damping[i] == d);
				CHECK(got.bowBalance[i] == 0.60f * std::min(1.0f, std::sqrt(d * (1.0f / 0.0255f))));
				CHECK(got.home[i] == laws.home[i]);
			}
			CHECK(got.shimmerAlpha == std::clamp(laws.shimmerAlpha * f, 0.07f * 0.25f, 1.0f));
			CHECK(got.pluckAmp == std::clamp(laws.pluckAmp * f, 0.0f, 2.0f));
			CHECK(got.travelRate == laws.travelRate * f);
			CHECK(got.bowMode == owlet::dsp::WeaveBow::Noise);
		}
		owlet::dsp::PhiWeaveParams same = laws;
		feathers::shapeWeaveLaws(feathers::ResonateShaping {}, same);
		CHECK(std::memcmp(&same, &laws, sizeof(same)) == 0);
	}
}

TEST_CASE("resonate shaping: the synth hands the string the shaped laws, and the knobs' values are the stated factors") {
	// The param -> factor mapping and the road into the string, as values read off the string: the laws
	// it was handed (side A), and the bow scale.
	const auto shaped = [](ParamId id, double value) {
		Rig rig;
		rig.set(ParamId::BEnable, 0.0);
		rig.set(id, value);
		rig.play({}, 1024, 256);
		return std::make_pair(rig.synth.weaveForTest().lawsAForTest(), rig.synth.weaveForTest().bowScaleForTest());
	};
	const auto [plain, plainBow] = shaped(ParamId::ResonateStiffness, 0.0);
	CHECK(plainBow == 1.0f);
	const owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(17.0 / 1023.0);
	CHECK(std::memcmp(&plain, &laws, sizeof(laws)) == 0); // every knob at default: the laws, to the bit

	const auto check = [&](ParamId id, double value, auto expected) {
		const auto [got, bow] = shaped(id, value);
		owlet::dsp::PhiWeaveParams want = laws;
		feathers::ResonateShaping s;
		float wantBow = 1.0f;
		expected(s, wantBow);
		feathers::shapeWeaveLaws(s, want);
		CHECK(std::memcmp(&got, &want, sizeof(want)) == 0);
		CHECK(bow == wantBow);
	};
	check(ParamId::ResonateStiffness, 1.0, [](auto& s, float&) { s.stiffness = 2.0f; });
	check(ParamId::ResonateCoupling, -1.0, [](auto& s, float&) { s.coupling = 0.5f; });
	check(ParamId::ResonateDecay, 1.0, [](auto& s, float&) { s.decay = 2.0f; }); // up = twice as long
	check(ParamId::ResonateShimmer, 2.0, [](auto& s, float&) { s.shimmer = 4.0f; });
	check(ParamId::ResonatePluck, 3.0, [](auto& s, float&) { s.pluck = 3.0f; });
	check(ParamId::ResonateTravel, 0.5, [](auto& s, float&) { s.travel = 0.5f; });
	check(ParamId::ResonateBowMode, 4.0, [](auto& s, float&) { s.bowMode = 4; }); // Pulses
	check(ParamId::ResonateBow, 2.5, [](auto&, float& bow) { bow = 2.5f; });
}

TEST_CASE("resonate shaping: at every extreme the string stays inside the stability bound and stays bounded") {
	// The clamps make k + 4c < 4 hold whatever the knobs do; this measures it over the 4096-phase sweep at
	// the scale extremes, then runs the string itself — ten simulated seconds at every 64th phase, with
	// the longest ring, the hardest bow and the stiffest springs together — and requires it bounded.
	float worst = 0.0f;
	for (int p = 0; p < 4096; ++p) {
		for (const float f : { 0.25f, 4.0f }) {
			owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(p / 4096.0);
			feathers::ResonateShaping s;
			s.stiffness = f;
			s.coupling = f;
			s.decay = 4.0f;
			feathers::shapeWeaveLaws(s, laws);
			for (int i = 0; i < owlet::dsp::kPhiWeaveNumNodes; ++i) {
				worst = std::max(worst, laws.stiffness[i] + 4.0f * laws.coupling[i]);
			}
		}
	}
	MESSAGE("max k + 4c at the shaping extremes: " << worst);
	CHECK(worst < 4.0f);

	for (int p = 0; p < 64; ++p) {
		CAPTURE(p);
		owlet::dsp::PhiWeaveParams laws = owlet::dsp::buildPhiWeaveLaws(p / 64.0);
		feathers::ResonateShaping s;
		s.stiffness = 4.0f;
		s.coupling = 4.0f;
		s.decay = 4.0f;
		s.shimmer = 4.0f;
		s.pluck = 4.0f;
		s.travel = 4.0f;
		feathers::shapeWeaveLaws(s, laws);
		feathers::WeaveString string;
		string.prepare(kSampleRate);
		string.setLaws(laws, laws);
		string.setExcitation({ 4.0f });
		string.reset();
		const int ticks = static_cast<int>(10.0 * feathers::WeaveString::kTickHz);
		bool finite = true;
		float peak = 0.0f;
		for (int t = 0; t < ticks; ++t) {
			if (t % 344 == 0) {
				string.pluck();
			}
			string.advance(string.samplesUntilTick());
			const float* x = string.nodesForTest();
			for (int i = 0; i < string.nodeCount(); ++i) {
				finite = finite && std::isfinite(x[i]);
				peak = std::max(peak, std::abs(x[i]));
			}
		}
		CHECK(finite);
		CHECK(peak <= 2.0f);
	}
}

TEST_CASE("resonate shaping: Width makes the ears differ, and at 0 the patch is mono") {
	// Width 0: one signal in both channels, bit for bit (the mono road). Width 1: the right ear reads the
	// ring half a turn round (a value, off the voice), and the channels differ.
	const auto run = [](double width) {
		Rig rig;
		rig.set(ParamId::ResonateWidth, width);
		std::vector<float> left(256 * 94); // a whole number of 256-sample blocks
		std::vector<float> right(256 * 94);
		rig.synth.noteOn(0, kKeyC3, 1, 1.0);
		for (uint32_t at = 0; at < left.size(); at += 256) {
			rig.synth.render(left.data() + at, right.data() + at, 256);
		}
		const int slot = rig.synth.slotOfKeyForTest(kKeyC3);
		return std::make_tuple(left, right, rig.synth.voiceWeaveTapForTest(slot));
	};
	const auto [monoL, monoR, monoTap] = run(0.0);
	CHECK(monoL == monoR);
	CHECK(monoTap == 0.0);
	const auto [wideL, wideR, wideTap] = run(1.0);
	CHECK(wideTap == doctest::Approx(0.5).epsilon(1e-12));
	CHECK(firstDifference(wideL, wideR) >= 0);
	// And the left ear is the mono patch's left, to the bit: Width moves only the right head.
	CHECK(firstDifference(wideL, monoL) == -1);
}

TEST_CASE("resonate shaping: V5 is untouched by every Resonate knob") {
	// On V5 (and Morph, Stair) nothing reads the string; the nine knobs at their extremes leave a V5 note's
	// samples exactly as they were.
	const auto run = [](bool extremes) {
		Rig rig(OscType::V5);
		if (extremes) {
			rig.set(ParamId::ResonateWidth, 1.0);
			rig.set(ParamId::ResonateBow, 4.0);
			rig.set(ParamId::ResonateBowMode, 5.0);
			rig.set(ParamId::ResonateShimmer, 2.0);
			rig.set(ParamId::ResonateStiffness, -2.0);
			rig.set(ParamId::ResonateCoupling, 2.0);
			rig.set(ParamId::ResonateDecay, 2.0);
			rig.set(ParamId::ResonatePluck, 0.0);
			rig.set(ParamId::ResonateTravel, 4.0);
			rig.set(ParamId::ResonateSpeed, 3.0);
		}
		return rig.play({ noteOn(0, kKeyC4, 1) }, 12000, 256);
	};
	const std::vector<float> plain = run(false);
	REQUIRE(peakOf(plain) > 0.05f); // NOT SILENCE (backlog MA)
	CHECK(firstDifference(run(true), plain) == -1);
}

// ---------------------------------------------------------------------------------------------------------
// RESONATE SPEED: the string's tick rate, x1..x8 on a fixed clock, default x2. The firmware
// ticked once per render WINDOW and the reference's windows run under 128 samples, so the string it played moved
// faster than the port's first clock (WeaveString::setSpeed).
// ---------------------------------------------------------------------------------------------------------

namespace {

/// rms(x[n] - x[n - P]) / rms(x) over [from, to): how much the scanned cycle changes from one period to the
/// next — the "aliveness" the parity investigation measured (2026-10-03). P is the note's exact period.
double perCycleChange(const std::vector<float>& x, size_t from, size_t to, size_t period) {
	double d = 0.0;
	double e = 0.0;
	for (size_t i = from; i < to; ++i) {
		const double a = static_cast<double>(x[i]) - static_cast<double>(x[i - period]);
		d += a * a;
		e += static_cast<double>(x[i]) * static_cast<double>(x[i]);
	}
	return std::sqrt(d / e);
}

} // namespace

TEST_CASE("resonate speed: the string's tick is round(sampleRate / (44100/128 x speed)) at 44.1, 48 and 96 kHz") {
	// THE WINDOW AS A VALUE, read off the synth's string, against the formula worked out by hand here (not by
	// calling WeaveString::tickLengthFor): 44100/128 = 344.53 Hz is the firmware's per-buffer rate.
	struct Row {
		double rate;
		double speedLog2; // the param's value: log2 of the multiple
		int window;
	};
	const Row rows[] = {
		{ 44100.0, 0.0, 128 }, { 44100.0, 1.0, 64 }, { 44100.0, 3.0, 16 },  // 128, 64, 16 exactly
		{ 48000.0, 0.0, 139 }, { 48000.0, 1.0, 70 }, { 48000.0, 3.0, 17 },  // 139.32, 69.66, 17.42
		{ 96000.0, 0.0, 279 }, { 96000.0, 1.0, 139 }, { 96000.0, 3.0, 35 }, // 278.64, 139.32, 34.83
	};
	for (const Row& row : rows) {
		CAPTURE(row.rate);
		CAPTURE(row.speedLog2);
		Rig rig(OscType::Resonate, row.rate);
		rig.set(ParamId::ResonateSpeed, row.speedLog2);
		// Past one window of the old length, so a change taken at the boundary has landed.
		rig.play({ noteOn(0, kKeyC4, 1) }, 600, 64);
		CHECK(rig.synth.weaveForTest().tickLength() == row.window);
		CHECK(rig.synth.weaveForTest().speed() == doctest::Approx(std::exp2(row.speedLog2)));
	}
	// AND THE DEFAULT IS x2 (by design): a fresh patch, untouched, ticks every 64 samples at 44.1 kHz.
	ParamSet params;
	Synth synth { params };
	synth.activate(44100.0, 4096);
	CHECK(params.base(static_cast<clap_id>(ParamId::ResonateSpeed)) == 1.0);
	CHECK(synth.weaveForTest().tickLength() == 64);
}

TEST_CASE("resonate speed: at x2 the scanned cycle changes twice as much per period as at x1") {
	// THE METRIC FROM THE PARITY INVESTIGATION (2026-10-03), through the synth: a held note, the string bowed
	// by its laws, the per-period change in the output measured over a steady stretch. The firmware's string
	// at 64-sample windows read 1.996x its 128-sample figure there (0.07394 against 0.03704 at phase 0); the
	// synth at x2 must read the same doubling. At 44 kHz A4 is exactly 100 samples a period, and the window is
	// round(44000 / 344.53) = 128 at x1 and 64 at x2. AZone 0: phase 0, the bowed zone the investigation used.
	constexpr double kRate = 44000.0;
	constexpr size_t kPeriod = 100;
	constexpr int16_t kKeyA4 = 69;
	const auto change = [&](double speedLog2) {
		Rig rig(OscType::Resonate, kRate);
		rig.set(ParamId::AZone, 0.0);
		rig.set(ParamId::ResonateSpeed, speedLog2);
		const std::vector<float> out = rig.play({ noteOn(0, kKeyA4, 1) }, static_cast<uint32_t>(10 * kRate), 256);
		REQUIRE(rig.synth.weaveForTest().tickLength() == (speedLog2 == 0.0 ? 128 : 64));
		return perCycleChange(out, static_cast<size_t>(6 * kRate), out.size(), kPeriod);
	};
	const double slow = change(0.0);
	const double fast = change(1.0);
	MESSAGE("per-cycle change: x1 " << slow << ", x2 " << fast << " (ratio " << fast / slow << ")");
	CHECK(slow > 0.005); // the case is not vacuous: the string moves at x1 too (0.0093 measured 2026-10-03)
	CHECK(fast / slow > 1.8);
	CHECK(fast / slow < 2.2);
}

TEST_CASE("resonate speed: output identical at any host block size, with Speed moving mid-note") {
	// The window changes length only at a tick boundary, on the string's own clock, so a Speed move lands at
	// the same sample whatever the host's blocks. x2 throughout, then x8 and back to x1 by param events.
	constexpr uint32_t kFrames = 24000;
	const std::vector<Event> script {
		param(0, ParamId::ResonateSpeed, 1.0),
		noteOn(0, kKeyC4, 1),
		noteOn(37, kKeyG4, 2),
		param(4001, ParamId::ResonateSpeed, 3.0),
		param(9007, ParamId::Path, 0.6),
		param(13013, ParamId::ResonateSpeed, 0.0),
		noteOn(15011, kKeyC3, 3),
		param(19019, ParamId::ResonateSpeed, 2.3),
	};
	Rig reference;
	const std::vector<float> first = reference.play(script, kFrames, 256);
	REQUIRE(peakOf(first) > 0.05f); // NOT SILENCE (backlog MA)
	for (const int block : { 1, 37, 128, 441, 4096 }) {
		CAPTURE(block);
		Rig rig;
		CHECK(firstDifference(rig.play(script, kFrames, block), first) == -1);
	}
}

TEST_CASE("resonate speed: x1 is the clock before Speed existed, to the bit") {
	// The ring's output at x1 is pinned against the code before Speed (test_weave_ring_pin, which never sets
	// one); here the setter itself: a string told x1 — at prepare, and again mid-run — renders exactly what a
	// string never told anything renders, and its window is round(sampleRate / kTickHz) as before.
	const auto render = [](bool tell) {
		feathers::WeaveString s;
		const auto laws = owlet::dsp::buildPhiWeaveLaws(0.37);
		s.setLaws(laws, laws);
		if (tell) {
			s.setSpeed(1.0);
		}
		s.prepare(kSampleRate);
		std::vector<float> out;
		int done = 0;
		while (done < 48000) {
			const int m = std::min(s.samplesUntilTick(), 48000 - done);
			for (int k = 0; k < m; ++k) {
				out.push_back(s.sample(0.01 * static_cast<double>(done + k), 0, k));
			}
			if (tell && done > 20000 && done < 20200) {
				s.setSpeed(1.0);
			}
			s.advance(m);
			done += m;
		}
		return std::make_pair(out, s.tickLength());
	};
	const auto [plain, plainLen] = render(false);
	const auto [told, toldLen] = render(true);
	CHECK(plainLen == static_cast<int>(std::lround(kSampleRate / feathers::WeaveString::kTickHz)));
	CHECK(toldLen == plainLen);
	CHECK(firstDifference(told, plain) == -1);
}

TEST_CASE("MEASURE: Resonate's CPU at Speed x1, x2 and x8" * doctest::skip()) {
	// A shared host: the three speeds ALTERNATE, five rounds, and the minimum of each is reported. Eight held
	// notes, one second of audio at 48 kHz in 256-sample blocks per round.
	constexpr int kRounds = 5;
	const double speeds[] = { 0.0, 1.0, 3.0 };
	double best[3] = { 1e30, 1e30, 1e30 };
	for (int round = 0; round < kRounds; ++round) {
		for (int s = 0; s < 3; ++s) {
			Rig rig;
			rig.set(ParamId::ResonateSpeed, speeds[s]);
			std::vector<Event> notes;
			for (int k = 0; k < 8; ++k) {
				notes.push_back(noteOn(0, static_cast<int16_t>(48 + 3 * k), k + 1));
			}
			rig.play(notes, 4800, 256);
			std::vector<float> l(256);
			std::vector<float> r(256);
			const auto t0 = std::chrono::steady_clock::now();
			for (int b = 0; b < 48000 / 256; ++b) {
				rig.synth.render(l.data(), r.data(), 256);
			}
			const auto t1 = std::chrono::steady_clock::now();
			best[s] = std::min(best[s], std::chrono::duration<double, std::milli>(t1 - t0).count());
		}
	}
	MESSAGE("ms (wall, min of 5) per second of audio, 8 voices: x1 " << best[0] << ", x2 " << best[1] << ", x8 " << best[2]);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG LZ (2026-10-10): the laws are built on every character update on every engine, so a still 8-sound patch paid
// for eight laws builds per tick whatever its weights. `WeaveLawMemo` hands a sound whose inputs have not moved the laws
// those inputs built. These cases say it is the same answer, to the bit, and that it really does skip.
namespace {

/// Every member of two law sets, compared as bits (a struct memcmp would read the padding after `bowMode`).
bool sameLaws(const owlet::dsp::PhiWeaveParams& a, const owlet::dsp::PhiWeaveParams& b) {
	const auto same = [](const auto& x, const auto& y) { return std::memcmp(&x, &y, sizeof x) == 0; };
	return same(a.stiffness, b.stiffness) && same(a.coupling, b.coupling) && same(a.damping, b.damping)
	       && same(a.bowBalance, b.bowBalance) && a.bowMode == b.bowMode && same(a.home, b.home)
	       && same(a.bowDepth, b.bowDepth) && same(a.bowRate, b.bowRate) && same(a.bowPos, b.bowPos)
	       && same(a.bowSpread, b.bowSpread) && same(a.travelRate, b.travelRate) && same(a.pluckPos, b.pluckPos)
	       && same(a.pluckWidth, b.pluckWidth) && same(a.pluckAmp, b.pluckAmp) && same(a.morphBowGain, b.morphBowGain)
	       && same(a.outGain, b.outGain) && same(a.shimmerAlpha, b.shimmerAlpha);
}

bool sameCharacter(const feathers::WeaveCharacter& a, const feathers::WeaveCharacter& b) {
	return sameLaws(a.a, b.a) && sameLaws(a.b, b.b) && std::memcmp(&a.crossfade, &b.crossfade, sizeof a.crossfade) == 0;
}

} // namespace

TEST_CASE("LZ: the law memo hands back what a rebuild builds, to the bit, and rebuilds exactly the sounds whose inputs moved") {
	feathers::Character c;
	c.engine = feathers::OscEngine::Weave;
	c.count = 5;
	for (size_t i = 0; i < c.count; ++i) {
		feathers::SoundCoord& s = c.sounds[i];
		s.slot = static_cast<uint8_t>(i);
		s.zone = static_cast<uint16_t>(40 + 170 * i);
		s.phaseOffset = 0.3f * static_cast<float>(i);
		s.gammaBase = 0.05 * static_cast<double>(i);
		s.travelX = 0.1 * static_cast<double>(i);
		s.travelY = -0.07 * static_cast<double>(i);
		s.weight = 1.0 / static_cast<double>(c.count);
	}
	c.sounds[3].orientation.pinned = true;
	c.sounds[3].orientation.generator.rotate = 0.4f;
	c.orientation.generator.rotate = 0.15f;

	feathers::WeaveLawMemo memo;
	uint64_t built = 0;
	// Each step: the memo's answer against the plain build's, and how many sounds the memo had to build.
	const auto step = [&](const std::string& what, uint64_t expectBuilt) {
		CAPTURE(what);
		const feathers::WeaveCharacter plain = feathers::buildWeaveFor(c);
		const feathers::WeaveCharacter memoed = feathers::buildWeaveFor(c, &memo);
		CHECK(sameCharacter(plain, memoed));
		// AND EACH SOUND'S OWN LAWS, which the blend above cannot see for a sound at weight 0: restated by the firmware's
		// two-end road, which hands a sound's laws back unblended.
		for (size_t i = 0; i < c.count; ++i) {
			CAPTURE(i);
			feathers::Character alone = c;
			alone.count = 2;
			alone.v1Crossfade = true;
			alone.sounds[0] = c.sounds[i];
			alone.sounds[1] = c.sounds[i];
			CHECK(sameLaws(memo.entries[i].laws, feathers::buildWeaveFor(alone).a));
		}
		CHECK(memo.built - built == expectBuilt);
		built = memo.built;
	};
	step("first build: every sound", 5);
	step("nothing moved: nothing built", 0);
	for (size_t i = 0; i < c.count; ++i) {
		c.sounds[i].weight = i == 1 ? 1.0 : 0.0; // the puck: weights are not an input to a sound's laws
	}
	step("weights only: nothing built, and the blend still follows them", 0);
	for (size_t i = 0; i < c.count; ++i) {
		c.sounds[i].weight = 0.1 + 0.05 * static_cast<double>(i); // every sound heard again, so the blend sees each one
	}
	step("weights back: still nothing built", 0);
	c.sounds[2].travelX += 0.01;
	step("one sound's travel X", 1);
	c.sounds[4].travelY = 0.0;
	step("travel Y to +0", 1);
	c.sounds[4].travelY = -0.0;
	step("travel Y to -0: a different bit pattern rebuilds", 1);
	c.sounds[1].residue.weave.offset[7] = 0.02;
	step("one sound's weave residue", 1);
	c.sounds[0].residue.morph.offset[0] = 0.5;
	step("another plane's residue is not a weave input", 0);
	c.sounds[3].orientation.generator.rotate = 0.41f;
	step("a pinned sound's own rotate", 1);
	c.orientation.generator.rotate = 0.2f;
	step("the character's rotate: every unpinned sound", 4);
	c.sounds[0].zone = static_cast<uint16_t>(c.sounds[0].zone + 1);
	step("a zone", 1);
	c.sounds[2].phaseOffset += 0.125f;
	step("an offset", 1);
	c.sounds[3].gammaBase += 0.01;
	step("a gamma", 1);
	std::swap(c.sounds[0], c.sounds[1]);
	step("two sounds trade places in the constellation", 2);
	c.count = 2;
	c.v1Crossfade = true;
	c.xf = 0.3f;
	step("the firmware's two ends: the same memo, positions 0 and 1 unchanged", 0);
	CHECK(built == 5 + 1 + 1 + 1 + 1 + 1 + 4 + 1 + 1 + 1 + 2);
}

TEST_CASE("LZ: a still 8-sound patch builds no laws while it stands, on any engine, and a moving puck alone builds none either") {
	for (const OscType type : { OscType::V5, OscType::Resonate }) {
		CAPTURE(static_cast<int>(type));
		Rig rig;
		rig.set(ParamId::OscType, static_cast<double>(type));
		for (size_t s = 0; s < 8; ++s) {
			const double angle = 6.283185307179586 * static_cast<double>(s) / 8.0;
			rig.set(feathers::soundParam(s, feathers::SoundField::Enable), 1.0);
			rig.set(feathers::soundParam(s, feathers::SoundField::Zone), 97.0 * static_cast<double>(s) + 13.0);
			rig.set(feathers::soundParam(s, feathers::SoundField::PlaceX), 0.5 * std::cos(angle));
			rig.set(feathers::soundParam(s, feathers::SoundField::PlaceY), 0.5 * std::sin(angle));
		}
		rig.play({ noteOn(0, kKeyC3, 1) }, 4096, 256);
		const uint64_t settled = rig.synth.weaveLawsBuiltForTest();
		// NOT VACUOUS on Resonate: the memo is the road the laws are built on, and it built all eight. OFF RESONATE THE
		// RENDERS BUILD NONE AT ALL (the idle-overhead pass, 2026-11-05): a frozen string is owed its laws until a reset or
		// the switch to Resonate reads them (`Synth::settleWeaveLaws`) — only activate's reset built any here, before the
		// eight sounds were seated — so no count below moves there, a sound's own input included.
		if (type == OscType::Resonate) {
			CHECK(settled >= 8u);
		}
		else {
			CHECK(settled < 8u);
		}
		rig.play({}, 8192, 256);
		CHECK(rig.synth.weaveLawsBuiltForTest() == settled); // still: dozens of character updates, no laws built
		// The puck moves the WEIGHTS and the crossfade, never a sound's own inputs, so moving it builds nothing either.
		for (int b = 0; b < 16; ++b) {
			rig.set(ParamId::PosX, 0.3 * std::sin(0.2 * static_cast<double>(b)));
			rig.play({}, 256, 256);
		}
		CHECK(rig.synth.weaveLawsBuiltForTest() == settled);
		// And a sound's own input does: one zone, one sound rebuilt.
		rig.set(feathers::soundParam(5, feathers::SoundField::Zone), 600.0);
		rig.play({}, 512, 256);
		CHECK(rig.synth.weaveLawsBuiltForTest() == settled + (type == OscType::Resonate ? 1u : 0u));
	}
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	Rig rig;
	const std::vector<float> out = rig.play({ noteOn(0, kKeyC4, 1) }, 4096, 256);
	CAPTURE(peakOf(out));
	REQUIRE(peakOf(out) > 0.05f);
}

namespace {

/// THREE SOUNDS WHOSE OWN INPUTS MOVE (sound 0's Offset, a law input) and whose weights move (Pos X), so a frozen string
/// is owed different laws at every tick and its crossfade moves — `step` is the block number.
void moveFrozenInputs(Rig& rig, int step) {
	rig.set(ParamId::PosX, 0.4 * std::sin(0.17 * static_cast<double>(step)));
	rig.set(feathers::soundParam(0, feathers::SoundField::Offset), -3.0 + 0.37 * static_cast<double>(step % 13));
}

void seatThreeSounds(Rig& rig) {
	for (size_t s = 0; s < 3; ++s) {
		const double angle = 6.283185307179586 * static_cast<double>(s) / 3.0;
		rig.set(feathers::soundParam(s, feathers::SoundField::Enable), 1.0);
		rig.set(feathers::soundParam(s, feathers::SoundField::Zone), 97.0 * static_cast<double>(s) + 13.0);
		rig.set(feathers::soundParam(s, feathers::SoundField::PlaceX), 0.5 * std::cos(angle));
		rig.set(feathers::soundParam(s, feathers::SoundField::PlaceY), 0.5 * std::sin(angle));
	}
}

} // namespace

TEST_CASE("frozen string: off Resonate the laws are owed, and the first Resonate tick hands over exactly the laws a string on Resonate all along holds") {
	// THE IDLE-OVERHEAD PASS (2026-11-05): off Resonate `pushWeaveLaws` keeps the character instead of building its laws,
	// and re-seats the crossfade from the weights alone. The claim is by VALUE against a run that never left Resonate, fed
	// the same parameter history: at every frozen block the crossfade is that run's, and on the first block back on
	// Resonate the string holds that run's laws, both sides, bit for bit — although the sound's own inputs moved at every
	// block in between, so laws owed and never built would be stale.
	Rig onAll; // Resonate throughout
	Rig away;  // Additive while the inputs move, then Resonate
	for (Rig* rig : { &onAll, &away }) {
		seatThreeSounds(*rig);
	}
	away.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	for (Rig* rig : { &onAll, &away }) {
		rig->play({ noteOn(0, kKeyC3, 1) }, 512, 256);
	}
	const uint64_t lawsBefore = away.synth.weaveLawsBuiltForTest();
	int moved = 0;
	for (int b = 0; b < 24; ++b) {
		const owlet::dsp::PhiWeaveParams was = onAll.synth.weaveForTest().lawsAForTest();
		for (Rig* rig : { &onAll, &away }) {
			moveFrozenInputs(*rig, b);
			rig->play({}, 256, 256);
		}
		CHECK(away.synth.weaveCrossfadeForTest() == onAll.synth.weaveCrossfadeForTest());
		moved += sameLaws(was, onAll.synth.weaveForTest().lawsAForTest()) ? 0 : 1;
	}
	CHECK(moved > 12);                                                 // NOT VACUOUS: the laws moved under the frozen string
	CHECK(away.synth.weaveLawsBuiltForTest() == lawsBefore);           // and none were built while it was frozen
	CHECK_FALSE(sameLaws(away.synth.weaveForTest().lawsAForTest(), onAll.synth.weaveForTest().lawsAForTest())); // owed, not held
	// BACK ON RESONATE, the first block: the laws the string holds are the all-along run's.
	away.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	for (Rig* rig : { &onAll, &away }) {
		moveFrozenInputs(*rig, 24);
		rig->play({}, 64, 64);
	}
	CHECK(sameLaws(away.synth.weaveForTest().lawsAForTest(), onAll.synth.weaveForTest().lawsAForTest()));
	CHECK(sameLaws(away.synth.weaveForTest().lawsBForTest(), onAll.synth.weaveForTest().lawsBForTest()));
	CHECK(away.synth.weaveCrossfadeForTest() == onAll.synth.weaveCrossfadeForTest());
}

TEST_CASE("frozen string: a reset while frozen seats the string at the rest pose of the laws it is owed") {
	// A reset reads the laws (`WeaveString::reset`'s rest pose), so `Synth::reset` settles the owed laws first. By value
	// against a run on Resonate all along, reset at the same moment: the same laws, and the same rest pose node for node.
	Rig onAll;
	Rig away;
	for (Rig* rig : { &onAll, &away }) {
		seatThreeSounds(*rig);
	}
	away.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	for (int b = 0; b < 12; ++b) {
		for (Rig* rig : { &onAll, &away }) {
			moveFrozenInputs(*rig, b);
			rig->play({}, 256, 256);
		}
	}
	REQUIRE_FALSE(sameLaws(away.synth.weaveForTest().lawsAForTest(), onAll.synth.weaveForTest().lawsAForTest())); // owed
	onAll.synth.reset();
	away.synth.reset();
	CHECK(sameLaws(away.synth.weaveForTest().lawsAForTest(), onAll.synth.weaveForTest().lawsAForTest()));
	CHECK(sameLaws(away.synth.weaveForTest().lawsBForTest(), onAll.synth.weaveForTest().lawsBForTest()));
	const feathers::WeaveString& a = away.synth.weaveForTest();
	const feathers::WeaveString& o = onAll.synth.weaveForTest();
	bool samePose = true;
	for (int i = 0; i < a.nodeCount(); ++i) {
		samePose = samePose && std::memcmp(&a.nodeHomeForTest()[i], &o.nodeHomeForTest()[i], sizeof(float)) == 0;
	}
	CHECK(samePose);
}
