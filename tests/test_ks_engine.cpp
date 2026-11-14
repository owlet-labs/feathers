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

// THE KARPLUS-STRONG ENGINE IN THE BANK AND THE SYNTH (stage S3 on): the
// pluck's tone is Additive's spectrum at the same place, the string's material is five guest fields blended by the
// puck, each read by value against a restatement of the road.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/character_bank.h"
#include "dsp/partial_spectrum.h"
#include "dsp/slice_build.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

namespace {

using feathers::Character;
using feathers::CharacterBank;
using feathers::OscEngine;
using feathers::PreparedCharacter;
using feathers::SoundCoord;
using feathers::StringBias;
using feathers::StringFields;
using feathers::partials::PartialSpectrum;

SoundCoord sound(uint16_t zone, float offset, double weight, double travelX = 0.0, double travelY = 0.0) {
	SoundCoord s;
	s.zone = zone;
	s.phaseOffset = offset;
	s.weight = weight;
	s.travelX = travelX;
	s.travelY = travelY;
	return s;
}

Character stringOf(std::initializer_list<SoundCoord> sounds, OscEngine engine = OscEngine::String) {
	Character c;
	c.engine = engine;
	for (const SoundCoord& s : sounds) {
		c.sounds[c.count++] = s;
	}
	return c;
}

PreparedCharacter& prepared(const Character& c) {
	static PreparedCharacter p; // large: kept off the stack
	feathers::PartialsScratch scratch;
	CharacterBank::buildOne(c, p, scratch);
	return p;
}

/// ONE SOUND'S MATERIAL, restated: its home, its frame (its own pin or the character's, its own residue), its travel,
/// read through the guests and its scalers.
StringFields soloFields(const Character& c, const SoundCoord& s) {
	const feathers::SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
	const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(feathers::rotateFor(pin), home,
	                                                                        s.residue.partials);
	return feathers::stringFieldsWithOverrides(feathers::readStringFields(frame, home, s.travelX, s.travelY),
	                                           s.stringBias);
}

std::array<double, 5> asArray(const StringFields& f) {
	return { f.brightness, f.sustain, f.pick, f.noise, f.stretch };
}

} // namespace

TEST_CASE("ks: a String character builds the pluck's tone and the string's material, and never a grid") {
	const Character c = stringOf({ sound(101, 0.0f, 0.4), sound(613, 0.37f, 0.6, 0.21, -0.13) });
	PreparedCharacter& p = prepared(c);
	CHECK(p.engine == OscEngine::String);
	for (int mip = 0; mip < 8; ++mip) {
		CHECK(p.gridFor(mip) == nullptr);
	}
	CHECK(p.eval(nullptr, 0.3) == 0.0f);
	// THE TONE IS ADDITIVE'S SPECTRUM AT THE SAME PLACE, by the one reading of it: the same character built as Additive.
	PartialSpectrum additive;
	feathers::PartialsScratch scratch;
	Character asAdditive = c;
	asAdditive.engine = OscEngine::Partials;
	feathers::buildPartialsFor(asAdditive, additive, scratch);
	CHECK(p.partials == additive);
	CHECK(p.string == feathers::buildStringFieldsFor(c));
	CHECK(p.string != StringFields {}); // not vacuous: the material was read
}

TEST_CASE("ks: the material is the sum of each sound's share times its fields, each through its own frame") {
	SoundCoord turned = sound(613, 0.37f, 0.75, 0.21, -0.13);
	turned.orientation.pinned = true;
	turned.orientation.generator.rotate = 0.3f;
	for (size_t r = 0; r < feathers::kPartialTriangles; ++r) {
		turned.residue.partials.offset[r] = 0.011 * static_cast<double>(r + 1);
	}
	turned.stringBias.brightness = 0.8f;
	const Character c = stringOf({ sound(101, 0.0f, 0.25), turned });
	const auto a = asArray(soloFields(c, c.sounds[0]));
	const auto b = asArray(soloFields(c, c.sounds[1]));
	// The turn, the residue and the scaler each move the second sound, or the frame claim is vacuous.
	SoundCoord plain = turned;
	plain.orientation = {};
	plain.residue = {};
	plain.stringBias = {};
	const auto p = asArray(soloFields(c, plain));
	double moved = 0.0;
	for (size_t i = 0; i < 5; ++i) {
		moved = std::max(moved, std::abs(p[i] - b[i]));
	}
	REQUIRE(moved > 1.0e-3);
	const auto got = asArray(feathers::buildStringFieldsFor(c));
	for (size_t i = 0; i < 5; ++i) {
		CAPTURE(i);
		CHECK(std::abs(got[i] - (0.25 * a[i] + 0.75 * b[i])) < 1.0e-6);
	}
	// SHARES, not raw weights: the same two at 1 and 3 are the same blend.
	Character scaled = c;
	scaled.sounds[0].weight = 1.0;
	scaled.sounds[1].weight = 3.0;
	CHECK(feathers::buildStringFieldsFor(scaled) == feathers::buildStringFieldsFor(c));
}

TEST_CASE("ks: each scaler at neutral is its field to the bit, at 0 is nothing and at 1 is full") {
	const SoundCoord s = sound(345, 0.11f, 1.0, 0.4, 0.2);
	const Character c = stringOf({ s });
	const StringFields raw = soloFields(c, s);
	CHECK(feathers::stringFieldsWithOverrides(raw, StringBias {}) == raw);
	StringBias zero { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	StringBias full { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	for (const double v : asArray(feathers::stringFieldsWithOverrides(raw, zero))) {
		CHECK(v == 0.0);
	}
	for (const double v : asArray(feathers::stringFieldsWithOverrides(raw, full))) {
		CHECK(v == 1.0);
	}
}

TEST_CASE("ks: across the reachable plane every field reaches its range, and Noise and Stretch rest at nothing over "
          "the share their duty gives") {
	// The reachable window, zone 0 at offsets −8..+11 and travel in both axes (Additive design §2.1).
	std::array<double, 5> lo { 1, 1, 1, 1, 1 };
	std::array<double, 5> hi { 0, 0, 0, 0, 0 };
	int noiseRest = 0;
	int stretchRest = 0;
	int n = 0;
	for (double offset = -8.0; offset <= 11.0; offset += 0.05) {
		for (const double tx : { -0.6, 0.0, 0.6 }) {
			const SoundCoord s = sound(0, static_cast<float>(offset), 1.0, tx, 0.3);
			const auto f = asArray(soloFields(stringOf({ s }), s));
			for (size_t i = 0; i < 5; ++i) {
				lo[i] = std::min(lo[i], f[i]);
				hi[i] = std::max(hi[i], f[i]);
			}
			noiseRest += f[3] == 0.0 ? 1 : 0;
			stretchRest += f[4] == 0.0 ? 1 : 0;
			++n;
		}
	}
	for (size_t i = 0; i < 5; ++i) {
		CAPTURE(i);
		CHECK(lo[i] < 0.05);
		CHECK(hi[i] > 0.95);
	}
	const double noiseShare = static_cast<double>(noiseRest) / n;
	const double stretchShare = static_cast<double>(stretchRest) / n;
	CAPTURE(noiseShare);
	CAPTURE(stretchShare);
	CHECK(noiseShare > 0.45);  // duty 0.45: rests over the other ~55%
	CHECK(noiseShare < 0.65);
	CHECK(stretchShare > 0.65); // duty 0.25: rests over ~75%, so a stretched string is a place you find
	CHECK(stretchShare < 0.85);
}

TEST_CASE("ks: every other engine's prepared character is untouched by the string's scalers") {
	for (const OscEngine e : { OscEngine::Morph, OscEngine::Stair, OscEngine::Weave, OscEngine::Shapes,
	                           OscEngine::Partials }) {
		CAPTURE(static_cast<int>(e));
		Character a = stringOf({ sound(101, 0.0f, 0.4), sound(613, 0.37f, 0.6) }, e);
		Character b = a;
		for (SoundCoord& s : b.sounds) {
			s.stringBias = { 0.0f, 1.0f, 0.0f, 1.0f, 1.0f };
		}
		const PreparedCharacter pa = prepared(a);
		const PreparedCharacter& pb = prepared(b);
		CHECK(pa.engine == pb.engine);
		if (e == OscEngine::Partials) {
			CHECK(pa.partials == pb.partials);
		}
		else if (e != OscEngine::Weave) {
			const float* ga = pa.gridFor(0);
			const float* gb = pb.gridFor(0);
			int differs = 0;
			for (int i = 0; i < 64; ++i) {
				const double phase = (i + 0.37) / 64.0;
				const bool shapes = e == OscEngine::Shapes;
				differs += (shapes ? pa.evalShapesAt(0, phase) : pa.eval(ga, phase))
				                   != (shapes ? pb.evalShapesAt(0, phase) : pb.eval(gb, phase))
				               ? 1
				               : 0;
			}
			CHECK(differs == 0);
		}
	}
}

// ---- the voice, through the real synth (S4) ---------------------------------------------------------------------

#include "dsp/ks_string.h"
#include "plugin/params.h"
#include "plugin/synth.h"

#include <memory>
#include <numbers>
#include <vector>

namespace {

using feathers::OscType;
using feathers::OscVoice;
using feathers::ParamId;
using feathers::ParamSet;
using feathers::Synth;

constexpr double kRate = 48000.0;

struct SynthRig {
	ParamSet params;
	Synth synth { params };
	explicit SynthRig(OscType engine) {
		synth.activate(kRate, 4096);
		set(ParamId::OscType, static_cast<double>(engine));
		set(ParamId::Level, 1.0);
		set(ParamId::VoiceLevel, 1.0); // a bare Synth has no bridge to route the strip envelope (test_synth's Rig)
	}
	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }
	float play(uint32_t frames, int block = 256) {
		std::vector<float> l(static_cast<size_t>(block));
		std::vector<float> r(static_cast<size_t>(block));
		float peak = 0.0f;
		for (uint32_t at = 0; at < frames; at += static_cast<uint32_t>(block)) {
			const int n = static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(block), frames - at));
			synth.render(l.data(), r.data(), n);
			for (int i = 0; i < n; ++i) {
				peak = std::max({ peak, std::abs(l[static_cast<size_t>(i)]), std::abs(r[static_cast<size_t>(i)]) });
			}
		}
		return peak;
	}
	const OscVoice& newest() const { return synth.voiceStackOscForTest(synth.newestVoiceSlotForTest(), 0); }
};

double noteHz(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

} // namespace


TEST_CASE("ks voice: a Pluck note's string is the note to a tenth of a cent, plucked once, and it sounds") {
	auto rig = std::make_unique<SynthRig>(OscType::Pluck);
	rig->synth.noteOn(0, 48, 1, 1.0);
	const float peak = rig->play(12000);
	const OscVoice& osc = rig->newest();
	const auto& s = osc.stringForTest();
	CHECK(std::abs(osc.frequencyHz() - noteHz(48)) < 1.0e-9);
	CHECK(s.frequencyHz() == osc.frequencyHz());
	const double w0 = 2.0 * std::numbers::pi * s.frequencyHz() / kRate;
	CHECK(std::abs(1200.0 * std::log2((kRate / s.frequencyHz()) / feathers::ks::loopPhaseDelay(s.tuning(), w0))) < 0.1);
	CHECK(osc.stringPlucksForTest() == 1);
	CHECK(osc.stringChunksForTest() > 0);
	CAPTURE(peak);
	CHECK(peak > 0.05f);
}

TEST_CASE("ks voice: no other engine enters the String road or plucks a string") {
	for (const OscType e : { OscType::V5, OscType::Additive, OscType::BasicShapes, OscType::Resonate }) {
		CAPTURE(static_cast<int>(e));
		auto rig = std::make_unique<SynthRig>(e);
		rig->synth.noteOn(0, 60, 1, 1.0);
		(void)rig->play(4096);
		CHECK(rig->newest().stringChunksForTest() == 0);
		CHECK(rig->newest().stringPlucksForTest() == 0);
		CHECK(rig->newest().stringForTest().idle());
	}
}

TEST_CASE("ks voice: legato slides the ringing string to the new note unplucked; mono plucks a fresh string") {
	// LEGATO keeps the voice, so the string it rang with is the one that slides (KS design §3). MONO is the house's
	// retrigger: a fresh voice takes the note (the old one leaves through the steal fade), so a fresh string is plucked.
	for (const double mode : { 2.0, 1.0 }) { // Legato, Mono (kVoiceModeNames)
		CAPTURE(mode);
		auto rig = std::make_unique<SynthRig>(OscType::Pluck);
		rig->set(ParamId::VoiceMode, mode);
		rig->synth.noteOn(0, 48, 1, 1.0);
		(void)rig->play(4800);
		const int first = rig->synth.newestVoiceSlotForTest();
		rig->synth.noteOn(0, 55, 2, 1.0);
		(void)rig->play(4800);
		const int now = rig->synth.newestVoiceSlotForTest();
		const OscVoice& osc = rig->synth.voiceStackOscForTest(now, 0);
		CHECK(std::abs(osc.stringForTest().frequencyHz() - noteHz(55)) < 1.0e-6);
		CHECK(osc.stringPlucksForTest() == 1u);
		CHECK(!osc.stringForTest().idle());
		if (mode == 2.0) {
			CHECK(now == first); // one voice: the first note's string, slid
		}
		else {
			CHECK(now != first); // a fresh voice, so a fresh string, plucked once
		}
	}
}

TEST_CASE("ks voice: a release moves the loop gain toward Release's T60") {
	auto rig = std::make_unique<SynthRig>(OscType::Pluck);
	rig->set(ParamId::KsRelease, std::log2(0.1));
	rig->synth.noteOn(0, 48, 1, 1.0);
	(void)rig->play(4800);
	const OscVoice& osc = rig->newest();
	const double held = osc.stringForTest().loopGainTarget();
	const feathers::ks::StringLaws laws = osc.stringLawsForTest();
	rig->synth.noteOff(0, 48, 1, 0.0);
	(void)rig->play(64); // the bare synth's voice edge ends the note soon after: the target is set at the next block
	CHECK(osc.stringReleasedForTest());
	const double want = feathers::ks::loopGainFor(kRate, noteHz(48), std::min(laws.t60, 0.1), laws.damping);
	CHECK(std::abs(osc.stringForTest().loopGainTarget() - want) < 1.0e-12);
	CHECK(want < held);
}

TEST_CASE("ks voice: a pluck's seed is derived — from the song position while the transport plays, from the key's strike "
          "count while it is stopped — never from anything incidental") {
	const auto seedAt = [](double songSeconds, bool playing, int strikes, double seedParam) {
		auto rig = std::make_unique<SynthRig>(OscType::Pluck);
		rig->set(ParamId::KsSeed, seedParam);
		uint64_t seed = 0;
		for (int i = 0; i < strikes; ++i) {
			rig->synth.setTransport(songSeconds, songSeconds * 2.0, playing);
			rig->synth.noteOn(0, 52, i + 1, 1.0);
			(void)rig->play(256);
			seed = rig->newest().stringSeedForTest();
			rig->synth.noteOff(0, 52, i + 1, 0.0);
			(void)rig->play(256);
		}
		return seed;
	};
	// PLAYING: the same place in the song is the same pluck, in a fresh synth; another place, another pluck.
	CHECK(seedAt(3.25, true, 1, 0.0) == seedAt(3.25, true, 1, 0.0));
	CHECK(seedAt(3.25, true, 1, 0.0) != seedAt(4.5, true, 1, 0.0));
	// AND THE SEED PARAM picks another set of plucks.
	CHECK(seedAt(3.25, true, 1, 0.0) != seedAt(3.25, true, 1, 7.0));
	// STOPPED: the n-th strike of a key is the same in every fresh synth, and differs from the (n+1)-th.
	CHECK(seedAt(0.0, false, 2, 0.0) == seedAt(0.0, false, 2, 0.0));
	CHECK(seedAt(0.0, false, 2, 0.0) != seedAt(0.0, false, 3, 0.0));
}

TEST_CASE("ks voice: above the string's top a note plays silence, never a clamped wrong pitch") {
	auto rig = std::make_unique<SynthRig>(OscType::Pluck);
	REQUIRE(noteHz(118) > kRate / feathers::ks::kShortestPeriod);
	rig->synth.noteOn(0, 118, 1, 1.0);
	CHECK(rig->play(4800) == 0.0f);
	CHECK(rig->newest().stringForTest().idle());
	auto ok = std::make_unique<SynthRig>(OscType::Pluck); // not vacuous: a note under the top sounds
	ok->synth.noteOn(0, 116, 1, 1.0);
	CHECK(ok->play(4800) > 0.01f);
}

TEST_CASE("ks voice: switching a held note's engine to Pluck plucks nothing, and the fade's String weight runs 0 to 1") {
	// K12, BY DESIGN: an engine switch is the existing crossfade and no more. The held note fades
	// into its silent string; the next note plucks.
	auto rig = std::make_unique<SynthRig>(OscType::V5);
	rig->synth.noteOn(0, 57, 1, 1.0);
	(void)rig->play(2048);
	const OscVoice& osc = rig->newest();
	rig->set(ParamId::OscType, static_cast<double>(OscType::Pluck));
	float last = -1.0f;
	bool monotone = true;
	bool sawMixed = false;
	for (int b = 0; b < 32; ++b) {
		const uint32_t before = osc.stringChunksForTest();
		(void)rig->play(32, 32);
		if (osc.stringChunksForTest() > before) {
			const float w = osc.mixStringWeightForTest(31);
			if (w > 0.0f && w < 1.0f) {
				sawMixed = true;
				monotone = monotone && w >= last;
				last = w;
			}
		}
	}
	CHECK(osc.stringPlucksForTest() == 0);
	CHECK(osc.stringForTest().idle());
	CHECK(sawMixed);
	CHECK(monotone);
	rig->synth.noteOn(0, 60, 2, 1.0); // and the next note does pluck
	(void)rig->play(256);
	CHECK(rig->newest().stringPlucksForTest() == 1);
}

TEST_CASE("ks voice: below C0 a note plays silence too, never a clamped wrong pitch") {
	auto rig = std::make_unique<SynthRig>(OscType::Pluck);
	REQUIRE(noteHz(11) < feathers::ks::kLowestHz);
	rig->synth.noteOn(0, 11, 1, 1.0);
	CHECK(rig->play(4800) == 0.0f);
	CHECK(rig->newest().stringForTest().idle());
	auto ok = std::make_unique<SynthRig>(OscType::Pluck); // not vacuous: C0 itself sounds
	ok->synth.noteOn(0, 12, 1, 1.0);
	CHECK(ok->play(9600) > 0.005f);
}

TEST_CASE("ks voice: velocity moves the pluck's Hardness by the Velocity depth, and at depth 0 not at all") {
	const auto hardnessAt = [](double velocity, double depth) {
		auto rig = std::make_unique<SynthRig>(OscType::Pluck);
		rig->set(ParamId::KsHardness, 0.6);
		rig->set(ParamId::KsVelocity, depth);
		rig->synth.noteOn(0, 52, 1, velocity);
		(void)rig->play(256);
		return rig->newest().stringPluckHardnessForTest();
	};
	CHECK(hardnessAt(1.0, 0.5) == doctest::Approx(0.85f).epsilon(1e-6)); // 0.6 + 0.5·(1 − 0.5)
	CHECK(hardnessAt(0.2, 0.5) == doctest::Approx(0.45f).epsilon(1e-6)); // 0.6 + 0.5·(0.2 − 0.5)
	CHECK(hardnessAt(1.0, 0.0) == doctest::Approx(0.6f).epsilon(1e-6));
	CHECK(hardnessAt(0.05, 1.0) == doctest::Approx(0.15f).epsilon(1e-6));
}


TEST_CASE("ks voice: each pluck varies subtly and deterministically — its pick within 5% of the material's, from its own seed") {
	const auto pickAt = [](double songSeconds) {
		auto rig = std::make_unique<SynthRig>(OscType::Pluck);
		rig->synth.setTransport(songSeconds, songSeconds * 2.0, true);
		rig->synth.noteOn(0, 52, 1, 1.0);
		(void)rig->play(256);
		const OscVoice& osc = rig->newest();
		return std::pair { osc.stringPluckPickForTest(), osc.stringLawsForTest().pick };
	};
	const auto [a, lawA] = pickAt(1.0);
	const auto [b, lawB] = pickAt(2.0);
	const auto [a2, lawA2] = pickAt(1.0);
	CHECK(a == a2); // deterministic: the same place in the song is the same pluck
	CHECK(a != b);  // and another place another
	CHECK(lawA == lawB);
	CHECK(std::abs(a / lawA - 1.0) <= feathers::ks::kPluckPickVariation);
	CHECK(std::abs(b / lawB - 1.0) <= feathers::ks::kPluckPickVariation);
}

TEST_CASE("ks voice: the string controls are handed to the oscillators that render, not to every slot and part every chunk") {
	auto rig = std::make_unique<SynthRig>(OscType::Pluck);
	// No voice sounds: nothing renders, so nothing is handed the controls (it was every slot and every part, each chunk).
	(void)rig->play(32 * 20, 32);
	CHECK(rig->synth.stringControlSetsForTest() == 0);
	// One held note, one part: one hand-out per chunk, and the pluck still reads its Hardness control by value.
	rig->synth.noteOn(0, 48, 1, 1.0);
	(void)rig->play(32 * 100, 32);
	CHECK(rig->synth.stringControlSetsForTest() == 100);
	CHECK(rig->newest().stringPlucksForTest() == 1);
	CHECK(rig->synth.soundingVoiceCount() == 1);
}
