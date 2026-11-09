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

// THE ADDITIVE ENGINE'S BANK BUILD, asserted by VALUE.
//
// Every reference is an independent restatement written again here: a shape's Fourier series, the blend as a
// member-by-member sum, the root-sum-square rescale, the override curve's three landmarks. Where two builds are
// compared with each other, that is the second assertion, there to prove the first is not vacuous.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/character_bank.h"
#include "dsp/partial_spectrum.h"
#include "dsp/slice_build.h"
#include "plugin/character_params.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/mod_rack.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/stereo/stereo_types.h"
#include "param_routes.h"
#include "plugin/params.h"
#include "plugin/synth.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace {

using feathers::Character;
using feathers::CharacterBank;
using feathers::OscEngine;
using feathers::OscType;
using feathers::ParamId;
using feathers::PreparedCharacter;
using feathers::SoundCoord;
using feathers::partials::kEnvelopeLog2Max;
using feathers::partials::kEnvelopeLog2Min;
using feathers::partials::kEnvelopePoints;
using feathers::partials::kMaxPartials;
using feathers::partials::kTargetRss;
using feathers::partials::PartialFields;
using feathers::partials::PartialSpectrum;
using feathers::partials::ShapeKind;

constexpr double kSampleRate = 48000.0;
constexpr std::array<ShapeKind, 4> kShapes { ShapeKind::Sine, ShapeKind::Triangle, ShapeKind::Saw, ShapeKind::Square };

SoundCoord sound(uint16_t zone, float offset, double weight, double travelX = 0.0, double travelY = 0.0) {
	SoundCoord s;
	s.zone = zone;
	s.phaseOffset = offset;
	s.weight = weight;
	s.travelX = travelX;
	s.travelY = travelY;
	return s;
}

Character additive(std::initializer_list<SoundCoord> sounds, ShapeKind shape = ShapeKind::Saw) {
	Character c;
	c.engine = OscEngine::Partials;
	c.additiveShape = shape;
	for (const SoundCoord& s : sounds) {
		c.sounds[c.count++] = s;
	}
	return c;
}

/// ONE SOUND'S SPECTRUM, restated from its parts: the sound's home, its frame (its own pin or the character's,
/// its own residue), its travel, its Smoothness knob and its six bias knobs, handed to buildPartialSound. The bank's
/// road is the same composition; a bank that read a different frame, travel or knob would not match it.
PartialSpectrum soloSpectrum(const Character& c, const SoundCoord& s) {
	const feathers::SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
	const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(feathers::rotateFor(pin), home,
	                                                                        s.residue.partials);
	PartialSpectrum out;
	feathers::buildPartialSound(frame, home, s.travelX, s.travelY, out, s.smoothnessScale, s.partialBias);
	return out;
}

double rssOf(const PartialSpectrum& s) {
	double power = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double a = static_cast<double>(s.envAtRatio[i]) * static_cast<double>(s.fine[i]);
		power += a * a;
	}
	return std::sqrt(power);
}

/// The root-sum-square rescale, restated: ONE factor on the envelope table and on envAtRatio.
PartialSpectrum rescaledToTarget(PartialSpectrum s) {
	const double rss = rssOf(s);
	REQUIRE(rss > 0.0);
	const double scale = kTargetRss / rss;
	for (float& e : s.envelope) {
		e = static_cast<float>(e * scale);
	}
	for (float& e : s.envAtRatio) {
		e = static_cast<float>(e * scale);
	}
	return s;
}

/// Σ w·spectrum, member by member, restated.
PartialSpectrum weightedSum(std::initializer_list<std::pair<double, const PartialSpectrum*>> terms) {
	PartialSpectrum out {};
	for (const auto& [w, s] : terms) {
		for (int j = 0; j < kEnvelopePoints; ++j) {
			out.envelope[j] = static_cast<float>(out.envelope[j] + w * s->envelope[j]);
		}
		for (int i = 0; i < kMaxPartials; ++i) {
			out.fine[i] = static_cast<float>(out.fine[i] + w * s->fine[i]);
			out.ratio[i] = static_cast<float>(out.ratio[i] + w * s->ratio[i]);
			out.envAtRatio[i] = static_cast<float>(out.envAtRatio[i] + w * s->envAtRatio[i]);
		}
	}
	return out;
}

/// The largest difference over every member, RELATIVE where a member is larger than 1 and absolute below: an
/// envelope point under a formant can sit well above 1 and a ratio reaches 320, where a float's last place is
/// already 1e-5, so an absolute bound there would be a bound on rounding.
double maxDiff(const PartialSpectrum& a, const PartialSpectrum& b) {
	double d = 0.0;
	const auto at = [&d](float x, float y) {
		d = std::max(d, std::abs(static_cast<double>(x) - y) / std::max(1.0, std::abs(static_cast<double>(y))));
	};
	for (int j = 0; j < kEnvelopePoints; ++j) {
		at(a.envelope[j], b.envelope[j]);
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		at(a.fine[i], b.fine[i]);
		at(a.ratio[i], b.ratio[i]);
		at(a.envAtRatio[i], b.envAtRatio[i]);
	}
	return d;
}

/// One character into one prepared slot, the bank's own body (CharacterBank::buildOne), with a scratch of the
/// test's own as a bank would hand it its member.
PreparedCharacter prepared(const Character& c) {
	PreparedCharacter p;
	feathers::PartialsScratch scratch;
	CharacterBank::buildOne(c, p, scratch);
	return p;
}

PartialSpectrum built(const Character& c) {
	const PreparedCharacter p = prepared(c);
	REQUIRE(p.engine == OscEngine::Partials);
	return p.partials;
}

/// The raw additive fields at a sound's coordinate, through its frame (no knob yet).
PartialFields rawFields(const SoundCoord& s) {
	const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, s.residue.partials);
	return feathers::readPartialFields(frame, home, s.travelX, s.travelY);
}

/// The raw detail triangles at a sound's coordinate, through the same frame as rawFields.
std::array<float, kMaxPartials> rawDetail(const SoundCoord& s) {
	const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, s.residue.partials);
	std::array<float, kMaxPartials> detail {};
	feathers::readPartialDetail(frame, home, s.travelX, s.travelY, detail);
	return detail;
}

/// THE SIX BIAS KNOBS AND THE FIELDS EACH ONE NAMES (design §4.1; dsp/slice_build.h PartialBias), written out here
/// rather than read from the code: Formant is BOTH formant gains, Comb is the comb's depth, the rest one field each.
struct BiasKnob {
	const char* name;
	float feathers::PartialBias::* knob;
	std::array<float PartialFields::*, 2> fields;
	int fieldCount;
	feathers::ParamId (*param)(size_t slot);
};
const std::array<BiasKnob, 6>& biasKnobs() {
	using B = feathers::PartialBias;
	using F = PartialFields;
	static const std::array<BiasKnob, 6> kKnobs { {
	    { "Rolloff", &B::rolloff, { &F::rolloff, nullptr }, 1, &feathers::additiveRolloffParam },
	    { "Formant", &B::formant, { &F::formant1Gain, &F::formant2Gain }, 2, &feathers::additiveFormantParam },
	    { "Odd/Even", &B::oddEven, { &F::oddEven, nullptr }, 1, &feathers::additiveOddEvenParam },
	    { "Comb", &B::comb, { &F::combDepth, nullptr }, 1, &feathers::additiveCombParam },
	    { "Detail", &B::detail, { &F::detail, nullptr }, 1, &feathers::additiveDetailParam },
	    { "Off-grid", &B::offGrid, { &F::offGrid, nullptr }, 1, &feathers::additiveOffGridParam },
	} };
	return kKnobs;
}

/// `f` with every field `b` names set to `value`: the restatement of "the field forced".
PartialFields forced(PartialFields f, const BiasKnob& b, float value) {
	for (int i = 0; i < b.fieldCount; ++i) {
		f.*(b.fields[static_cast<size_t>(i)]) = value;
	}
	return f;
}

/// The first home on the reachable window (zone 0, offset −8 .. +11 in 1/64 steps, no travel) whose raw fields
/// satisfy `pred`: the fields' periods are 3 to 6.5 homes (design §2.1), so the zone line alone (one home) is too
/// short to find a sparse field's country.
template <typename Pred>
float offsetWhere(Pred&& pred) {
	for (int step = -8 * 64; step <= 11 * 64; ++step) {
		const float offset = static_cast<float>(step) / 64.0f;
		if (pred(rawFields(sound(0, offset, 1.0)))) {
			return offset;
		}
	}
	FAIL("no home on the window satisfies the condition");
	return 0.0f;
}

/// A patch read through the real mapping: every param at its default unless set.
struct Patch {
	std::map<ParamId, double> sets;
	double operator()(ParamId id) const {
		const auto it = sets.find(id);
		if (it != sets.end()) {
			return it->second;
		}
		const feathers::ParamDesc* d = feathers::findParam(static_cast<clap_id>(id));
		REQUIRE(d != nullptr);
		return d->def;
	}
	Character character(OscType type, feathers::EarSide ear = feathers::EarSide::Head) const {
		return feathers::characterFromParams([this](ParamId id) { return (*this)(id); }, type,
		                                     feathers::AuditionTuning {}, feathers::AnchorPoint {},
		                                     feathers::SoundTravel {}, feathers::SoundOrientations {},
		                                     feathers::SoundResidue {}, feathers::SoundGamma {}, feathers::Preview {},
		                                     ear);
	}
};

} // namespace

// ---- the engine --------------------------------------------------------------------------------------------

TEST_CASE("additive: a Partials character builds a Partials table and never a grid") {
	// Built directly: no OscType selects this engine until it sounds (plan Task 9).
	const Character c = additive({ sound(101, 0.0f, 0.4), sound(613, 0.37f, 0.6, 0.21, -0.13) });
	const PreparedCharacter p = prepared(c);
	CHECK(p.engine == OscEngine::Partials);
	for (int mip = 0; mip < 8; ++mip) {
		CHECK(p.gridFor(mip) == nullptr);
	}
	CHECK(p.eval(nullptr, 0.3) == 0.0f);
	// And the payload is the character's spectrum: built, at the target level.
	CHECK(std::abs(rssOf(p.partials) - kTargetRss) < 1e-6);
	CHECK(maxDiff(p.partials, PartialSpectrum {}) > 0.01);

	// THROUGH A BANK, and its scratch reused: a second, different build into the other slot is that character's
	// own spectrum, not one leaning on the first build's working spectra.
	const Character other = additive({ sound(777, -1.5f, 1.0) });
	CharacterBank bank;
	bank.setSampleRate(kSampleRate);
	bank.setCharacter(c);
	CHECK(bank.current().engine == OscEngine::Partials);
	CHECK(maxDiff(bank.current().partials, p.partials) < 1e-6);
	bank.advance(1 << 20); // past any fade guard
	bank.setCharacter(other);
	bank.advance(1 << 20);
	REQUIRE(bank.current().partials != p.partials);
	CHECK(maxDiff(bank.current().partials, built(other)) < 1e-6);
}


TEST_CASE("additive: the blend is the sum of weight times each sound's spectrum, renormalised") {
	// The second sound is TURNED and has BANKED residue, so its spectrum is read through its own frame: a bank that
	// derived the frame another way (the character's pin, no residue) would read a different sound.
	SoundCoord turned = sound(613, 0.37f, 0.75, 0.21, -0.13);
	turned.orientation.pinned = true;
	turned.orientation.generator.rotate = 0.3f;
	for (size_t r = 0; r < feathers::kPartialTriangles; ++r) {
		turned.residue.partials.offset[r] = 0.011 * static_cast<double>(r + 1);
	}
	const Character c = additive({ sound(101, 0.0f, 0.25), turned });
	const PartialSpectrum s0 = soloSpectrum(c, c.sounds[0]);
	const PartialSpectrum s1 = soloSpectrum(c, c.sounds[1]);
	REQUIRE(maxDiff(s0, s1) > 0.01); // two different sounds, or the blend proves nothing
	// And the turn and the residue each move this sound, or the frame claim above is vacuous.
	SoundCoord unturned = turned;
	unturned.orientation = {};
	REQUIRE(maxDiff(soloSpectrum(c, unturned), s1) > 1e-3);
	SoundCoord unbanked = turned;
	unbanked.residue = {};
	REQUIRE(maxDiff(soloSpectrum(c, unbanked), s1) > 1e-3);
	const PartialSpectrum want = rescaledToTarget(weightedSum({ { 0.25, &s0 }, { 0.75, &s1 } }));
	const PartialSpectrum got = built(c);
	CHECK(maxDiff(got, want) < 1e-6);
	// The ratios blend as frequencies: harmonic 1 never leaves the grid, so a convex blend keeps it at 1.
	CHECK(got.ratio[0] == 1.0f);
	// Not vacuous: either sound alone is somewhere else.
	CHECK(maxDiff(got, rescaledToTarget(s0)) > 1e-3);
	CHECK(maxDiff(got, rescaledToTarget(s1)) > 1e-3);
}

TEST_CASE("additive: the shares are the weights over their sum, so weights that do not sum to one stay at pitch") {
	// The constellation hands a partition of one; a caller that does not must not transpose the sound.
	const Character halves = additive({ sound(101, 0.0f, 0.5), sound(613, 0.37f, 0.5) });
	const Character doubled = additive({ sound(101, 0.0f, 1.0), sound(613, 0.37f, 1.0) });
	const PartialSpectrum a = built(halves);
	const PartialSpectrum b = built(doubled);
	CHECK(b.ratio[0] == 1.0f);
	CHECK(b.ratio[1] == 2.0f);
	CHECK(maxDiff(a, b) < 1e-6);
	// A constellation with nothing to share plays its first sound.
	const Character none = additive({ sound(101, 0.0f, 0.0), sound(613, 0.37f, 0.0) });
	CHECK(maxDiff(built(none), rescaledToTarget(soloSpectrum(none, none.sounds[0]))) < 1e-6);
	// And no sounds at all is silence.
	CHECK(built(additive({})) == PartialSpectrum {});
}

TEST_CASE("additive: a 50/50 blend keeps the target root-sum-square where the plain blend dips") {
	// A shaped sine against a BRIGHT terrain sound (Rolloff near its flattest, so its fundamental holds a small
	// share of its power): the two overlap almost nowhere, so the plain 50/50 sum sits well under the target. The
	// blend is rescaled back.
	SoundCoord sine = sound(0, 0.0f, 0.5);
	sine.shapeAmount = 1.0f;
	const float bright = offsetWhere([](const PartialFields& f) { return f.rolloff < 0.1f && f.smoothness < 0.05f; });
	const Character c = additive({ sine, sound(0, bright, 0.5) }, ShapeKind::Sine);
	PartialSpectrum shape;
	feathers::partials::buildShapeSpectrum(ShapeKind::Sine, shape);
	const PartialSpectrum terrain = soloSpectrum(c, c.sounds[1]);
	const PartialSpectrum plain = weightedSum({ { 0.5, &shape }, { 0.5, &terrain } });
	REQUIRE(std::abs(rssOf(plain) - kTargetRss) > 0.02); // the dip this exists for, or the case is vacuous
	const PartialSpectrum got = built(c);
	CHECK(std::abs(rssOf(got) - kTargetRss) < 1e-6);
	CHECK(maxDiff(got, rescaledToTarget(plain)) < 1e-6);
}

TEST_CASE("additive: the two ears' spectra are each ear's own character, and differ when the stands differ") {
	const Character left = additive({ sound(101, 0.0f, 0.4, -0.2, 0.0), sound(613, 0.37f, 0.6, -0.1, 0.05) });
	Character right = left;
	right.sounds[0].travelX = 0.2;
	right.sounds[1].travelX = 0.1;
	right.sounds[0].weight = 0.6;
	right.sounds[1].weight = 0.4;
	CharacterBank bank;
	bank.setSampleRate(kSampleRate);
	bank.setCharacterStereo(left, right);
	REQUIRE(bank.stereoActive());
	const PreparedCharacter& l = bank.current(0);
	const PreparedCharacter& r = bank.current(1);
	CHECK(l.engine == OscEngine::Partials);
	CHECK(r.engine == OscEngine::Partials);
	CHECK(l.buildSerial != r.buildSerial);
	const PartialSpectrum l0 = soloSpectrum(left, left.sounds[0]);
	const PartialSpectrum l1 = soloSpectrum(left, left.sounds[1]);
	const PartialSpectrum r0 = soloSpectrum(right, right.sounds[0]);
	const PartialSpectrum r1 = soloSpectrum(right, right.sounds[1]);
	CHECK(maxDiff(l.partials, rescaledToTarget(weightedSum({ { 0.4, &l0 }, { 0.6, &l1 } }))) < 1e-6);
	CHECK(maxDiff(r.partials, rescaledToTarget(weightedSum({ { 0.6, &r0 }, { 0.4, &r1 } }))) < 1e-6);
	CHECK(maxDiff(l.partials, r.partials) > 1e-3);
}


TEST_CASE("additive: every other engine's Character compares equal to what it was") {
	// THE BASIC SHAPES PRECEDENT. (1) The real mapping never writes the new members, so on every existing engine
	// they sit at their defaults and a Character compares equal to the one built before they existed. (2) The
	// bank never reads them off Partials: the same Character with them moved prepares the same table, by what
	// the voice reads from it.
	Patch patch;
	patch.sets[feathers::soundParam(1, feathers::SoundField::Enable)] = 1.0;
	// The six bias knobs turned on both live sounds: off Additive the mapping must still not read them.
	for (size_t slot = 0; slot < 2; ++slot) {
		for (const BiasKnob& k : biasKnobs()) {
			patch.sets[k.param(slot)] = 1.0;
		}
	}
	for (const OscType type : { OscType::Morph, OscType::Stair, OscType::V5, OscType::Resonate,
	                            OscType::BasicShapes }) {
		INFO(static_cast<int>(type));
		const Character a = patch.character(type);
		REQUIRE(a.count >= 2);
		CHECK(a.engine != OscEngine::Partials);
		CHECK(a.additiveShape == ShapeKind::Saw);
		for (const SoundCoord& s : a.sounds) {
			CHECK(s.shapeAmount == 0.0f);
			CHECK(s.partialBias == feathers::PartialBias {});
		}
		Character b = a;
		b.additiveShape = ShapeKind::Sine;
		for (SoundCoord& s : b.sounds) {
			s.shapeAmount = 0.7f;
			s.partialBias = { 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
		}
		const PreparedCharacter pa = prepared(a);
		const PreparedCharacter pb = prepared(b);
		CHECK(pa.engine == pb.engine);
		CHECK(pa.fmDepth() == pb.fmDepth());
		CHECK(pa.fmFrequencyRatio() == pb.fmFrequencyRatio());
		CHECK(pa.ampNoise() == pb.ampNoise());
		CHECK(pa.phaseJitter() == pb.phaseJitter());
		CHECK(pa.vox.audible == pb.vox.audible);
		const bool shapes = pa.engine == OscEngine::Shapes;
		const float* ga = pa.gridFor(0);
		const float* gb = pb.gridFor(0);
		int differs = 0;
		for (int i = 0; i < 64; ++i) {
			const double phase = (i + 0.37) / 64.0;
			const float va = shapes ? pa.evalShapesAt(0, phase) : pa.eval(ga, phase);
			const float vb = shapes ? pb.evalShapesAt(0, phase) : pb.eval(gb, phase);
			differs += va != vb ? 1 : 0;
		}
		CHECK(differs == 0);
		if (type == OscType::Resonate) {
			// Resonate's voices read the string, whose laws come from buildWeaveFor.
			const feathers::WeaveCharacter wa = feathers::buildWeaveFor(a);
			const feathers::WeaveCharacter wb = feathers::buildWeaveFor(b);
			CHECK(wa.crossfade == wb.crossfade);
			CHECK(wa.a.outGain == wb.a.outGain);
			CHECK(wa.a.stiffness[0] == wb.a.stiffness[0]);
		}
	}
}

// ---- the shape --------------------------------------------------------------------------------------------

TEST_CASE("additive shape: each shape's amplitudes are its Fourier series, on the harmonics, at the target level") {
	for (const ShapeKind kind : kShapes) {
		INFO(static_cast<int>(kind));
		PartialSpectrum s;
		feathers::partials::buildShapeSpectrum(kind, s);
		CHECK(std::abs(rssOf(s) - kTargetRss) < 1e-6);
		// The series, written out again: magnitudes relative to the fundamental's.
		const auto series = [kind](int k) -> double {
			switch (kind) {
				case ShapeKind::Sine: return k == 1 ? 1.0 : 0.0;
				case ShapeKind::Saw: return 1.0 / k;
				case ShapeKind::Square: return k % 2 == 1 ? 1.0 / k : 0.0;
				case ShapeKind::Triangle: return k % 2 == 1 ? 1.0 / (static_cast<double>(k) * k) : 0.0;
				default: return -1.0;
			}
		};
		const double a1 = static_cast<double>(s.envAtRatio[0]) * s.fine[0];
		REQUIRE(a1 > 0.0);
		for (int k = 1; k <= kMaxPartials; ++k) {
			const size_t i = static_cast<size_t>(k - 1);
			CHECK(s.ratio[i] == static_cast<float>(k));
			const double a = static_cast<double>(s.envAtRatio[i]) * s.fine[i];
			CHECK(std::abs(a / a1 - series(k)) < 1e-6);
		}
		// The rolloff is in the ENVELOPE, so Formant Shift can slide it: the table is k^-p at every point.
		const double p = kind == ShapeKind::Triangle ? 2.0 : 1.0;
		const double e1 = static_cast<double>(s.envAtRatio[0]); // envelope at k = 1
		for (int j = 0; j < kEnvelopePoints; ++j) {
			const double l = kEnvelopeLog2Min + j * (kEnvelopeLog2Max - kEnvelopeLog2Min) / (kEnvelopePoints - 1);
			CHECK(std::abs(s.envelope[j] / e1 - std::exp2(-p * l)) < 1e-5 * std::exp2(-p * l) + 1e-12);
		}
	}
}

TEST_CASE("additive shape: an amount of 0 is the terrain to the bit, whichever shape is chosen") {
	const SoundCoord s = sound(613, 0.37f, 1.0, 0.21, -0.13);
	const Character c = additive({ s });
	// THE NO-SHAPE ROAD, restated: the sound alone, at weight 1, renormalised. WHAT THIS COVERS: at amount 0 neither
	// the shape kind nor the blend touches a bit of the result. WHAT IT DOES NOT: that the terrain spectrum itself is
	// right (the blend and smoothness cases own that), or the voice's reading of it (plan Task 9).
	PartialSpectrum want {};
	feathers::partials::accumulate(want, soloSpectrum(c, s), 1.0);
	feathers::partials::normaliseRss(want);
	for (const ShapeKind kind : kShapes) {
		INFO(static_cast<int>(kind));
		CHECK(built(additive({ s }, kind)) == want);
	}
	// And the blend itself leaves the terrain untouched at 0, and at NaN.
	PartialSpectrum shape;
	feathers::partials::buildShapeSpectrum(ShapeKind::Square, shape);
	const PartialSpectrum terrain = soloSpectrum(c, s);
	PartialSpectrum blended = terrain;
	feathers::partials::blendTowardShape(blended, shape, 0.0f);
	CHECK(blended == terrain);
	feathers::partials::blendTowardShape(blended, shape, std::nanf(""));
	CHECK(blended == terrain);
}

TEST_CASE("additive shape: an amount of 1 is the shape exactly, its ratios on the harmonics even off the grid") {
	// A sound whose terrain has walked partials off the grid: past the off-grid start the ratios are not k.
	const float at = offsetWhere([](const PartialFields& f) { return f.offGrid > 0.6f && f.smoothness < 0.05f; });
	SoundCoord s = sound(0, at, 1.0);
	const PartialSpectrum terrain = soloSpectrum(additive({ s }), s);
	REQUIRE(std::abs(terrain.ratio[kMaxPartials - 1] - static_cast<float>(kMaxPartials)) > 0.01f);
	s.shapeAmount = 1.0f;
	for (const ShapeKind kind : kShapes) {
		INFO(static_cast<int>(kind));
		PartialSpectrum shape;
		feathers::partials::buildShapeSpectrum(kind, shape);
		const PartialSpectrum got = built(additive({ s }, kind));
		for (int i = 0; i < kMaxPartials; ++i) {
			CHECK(got.ratio[i] == static_cast<float>(i + 1));
		}
		CHECK(maxDiff(got, shape) < 1e-6);
	}
}

TEST_CASE("additive shape: an amount of 0.5 is the member-wise mean of terrain and shape, renormalised") {
	const SoundCoord s = sound(613, 0.37f, 1.0, 0.21, -0.13);
	const PartialSpectrum terrain = soloSpectrum(additive({ s }), s);
	for (const ShapeKind kind : kShapes) {
		INFO(static_cast<int>(kind));
		PartialSpectrum shape;
		feathers::partials::buildShapeSpectrum(kind, shape);
		const PartialSpectrum mean = weightedSum({ { 0.5, &terrain }, { 0.5, &shape } });
		const PartialSpectrum want = rescaledToTarget(mean);
		PartialSpectrum got = terrain;
		feathers::partials::blendTowardShape(got, shape, 0.5f);
		CHECK(maxDiff(got, want) < 1e-6);
		CHECK(std::abs(rssOf(got) - kTargetRss) < 1e-6);
		// Through the bank, at one sound of weight 1, the same.
		SoundCoord half = s;
		half.shapeAmount = 0.5f;
		CHECK(maxDiff(built(additive({ half }, kind)), want) < 1e-6);
	}
}

TEST_CASE("additive shape: each sound's amount is its own, applied before the puck's weight") {
	// One sound fully shaped, the other not, half and half: the shape reaches only the first sound's share.
	SoundCoord shaped = sound(101, 0.0f, 0.5);
	shaped.shapeAmount = 1.0f;
	const SoundCoord bare = sound(613, 0.37f, 0.5, 0.21, -0.13);
	const Character c = additive({ shaped, bare }, ShapeKind::Square);
	PartialSpectrum shape;
	feathers::partials::buildShapeSpectrum(ShapeKind::Square, shape);
	const PartialSpectrum terrain = soloSpectrum(c, bare);
	const PartialSpectrum want = rescaledToTarget(weightedSum({ { 0.5, &shape }, { 0.5, &terrain } }));
	CHECK(maxDiff(built(c), want) < 1e-6);
	// Not vacuous: the second sound's evens survive, which a square applied to the whole blend would remove.
	CHECK(want.fine[1] > 0.01f);
}

// ---- smoothness -------------------------------------------------------------------------------------------

TEST_CASE("additive smoothness: the knob moves the Smoothness field as the override curve says, and nothing else") {
	const float at = offsetWhere([](const PartialFields& f) { return f.smoothness > 0.2f && f.smoothness < 0.8f; });
	for (const SoundCoord& s : { sound(0, at, 1.0), sound(0, at, 1.0, 0.13, 0.07) }) {
		const PartialFields raw = rawFields(s);
		const float f = raw.smoothness;
		INFO(f);
		// The curve's landmarks, restated: 0 defeats, the middle leaves, 1 lifts to full; a quarter either side is
		// half the way.
		const std::array<std::pair<float, float>, 5> want { { { 0.0f, 0.0f },
			                                                   { 0.25f, 0.5f * f },
			                                                   { 0.5f, f },
			                                                   { 0.75f, f + 0.5f * (1.0f - f) },
			                                                   { 1.0f, 1.0f } } };
		for (const auto& [knob, smooth] : want) {
			INFO(knob);
			const PartialFields got = feathers::partialFieldsWithOverrides(raw, knob);
			CHECK(std::abs(got.smoothness - smooth) < 1e-6f);
			PartialFields rest = got;
			rest.smoothness = raw.smoothness;
			CHECK(rest == raw);
		}
	}
}

TEST_CASE("additive smoothness: at knob 1 the prepared spectrum is the smooth one, wherever the field is") {
	// THE SMOOTH SPECTRUM, restated from design §4.1: σ = 1 zeroes Detail, Comb, Odd/Even, the formant gains and
	// Off-grid, and takes Rolloff to its steepest (p = 2.5). So the fields are Rolloff 1 and nothing else, with no
	// detail, whatever the terrain said: buildSound of THOSE fields, alone at weight 1.
	PartialFields smooth;
	smooth.rolloff = 1.0f;
	PartialSpectrum want;
	feathers::partials::buildSound(smooth, std::array<float, kMaxPartials> {}, want);
	// And that is the spectrum the sentence names, by value: every harmonic on the grid at k^-2.5 (the table read
	// at k, so to the table's interpolation, about 1e-3 relative), at the target level.
	REQUIRE(std::abs(rssOf(want) - kTargetRss) < 1e-6);
	for (int i = 0; i < kMaxPartials; ++i) {
		const double k = i + 1.0;
		REQUIRE(want.ratio[i] == static_cast<float>(k));
		REQUIRE(want.fine[i] == 1.0f);
		REQUIRE(std::abs(want.envAtRatio[i] / want.envAtRatio[0] / std::pow(k, -2.5) - 1.0) < 2e-3);
	}
	// Coordinates spread over the line, with and without travel, whatever their own fields say.
	int rough = 0;
	for (int step = -8; step <= 11; ++step) {
		for (const double tx : { 0.0, 0.31 }) {
			SoundCoord s = sound(0, static_cast<float>(step) + 0.37f, 1.0, tx, -0.5 * tx);
			INFO(step);
			INFO(tx);
			if (maxDiff(built(additive({ s })), want) > 1e-3) {
				++rough; // at the neutral knob this sound is NOT the smooth one
			}
			s.smoothnessScale = 1.0f;
			const PartialSpectrum got = built(additive({ s }));
			CHECK(maxDiff(got, want) < 1e-6);
			CHECK(got.ratio[kMaxPartials - 1] == static_cast<float>(kMaxPartials));
		}
	}
	REQUIRE(rough > 20); // most of the window is not smooth at the neutral knob, or the knob proved nothing
}

// ---- the six bias knobs (design §4.1) ---------------------------------------------------------------------

TEST_CASE("additive bias: each knob moves its own terrain field(s) as the override curve says, and nothing else") {
	for (const BiasKnob& b : biasKnobs()) {
		INFO(b.name);
		// A home where the knob's (first) field is mid-range, so 0, the middle and 1 are three different values.
		const float at = offsetWhere([&](const PartialFields& f) { return f.*(b.fields[0]) > 0.2f && f.*(b.fields[0]) < 0.8f; });
		for (const SoundCoord& s : { sound(0, at, 1.0), sound(0, at, 1.0, 0.13, 0.07) }) {
			const PartialFields raw = rawFields(s);
			for (const float knob : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }) {
				INFO(knob);
				feathers::PartialBias bias;
				bias.*(b.knob) = knob;
				const PartialFields got = feathers::partialFieldsWithOverrides(raw, feathers::kFieldNeutral, bias);
				PartialFields rest = got;
				for (int i = 0; i < b.fieldCount; ++i) {
					float PartialFields::* field = b.fields[static_cast<size_t>(i)];
					const float f = raw.*field;
					// The curve's landmarks, restated: 0 -> 0, a quarter -> half the field, the middle -> the field,
					// three quarters -> half-way to 1, 1 -> 1.
					const float want = knob == 0.0f    ? 0.0f
					                   : knob == 0.25f ? 0.5f * f
					                   : knob == 0.5f  ? f
					                   : knob == 0.75f ? f + 0.5f * (1.0f - f)
					                                   : 1.0f;
					CHECK(std::abs(got.*field - want) < 1e-6f);
					rest.*field = raw.*field;
				}
				// Every other field, Smoothness included, exactly as the terrain authored it.
				CHECK(rest == raw);
			}
		}
	}
}

TEST_CASE("additive bias: at knob 0, neutral and 1 the spectrum is buildSound of its field forced to 0, left, forced to 1") {
	// THE REFERENCE IS buildSound OF FIELDS SET BY HAND, not partialFieldsWithOverrides: the raw fields and detail at the
	// sound's coordinate, with the knob's field(s) overwritten (or not), through the clamping entry point.
	for (const BiasKnob& b : biasKnobs()) {
		INFO(b.name);
		const float at = offsetWhere([&](const PartialFields& f) { return f.*(b.fields[0]) > 0.2f && f.*(b.fields[0]) < 0.8f; });
		const SoundCoord s = sound(0, at, 1.0); // where the search found the field mid-range
		const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, s.residue.partials);
		const PartialFields raw = rawFields(s);
		const std::array<float, kMaxPartials> detail = rawDetail(s);
		const auto biased = [&](float knob) {
			feathers::PartialBias bias;
			bias.*(b.knob) = knob;
			PartialSpectrum out;
			feathers::buildPartialSound(frame, home, s.travelX, s.travelY, out, feathers::kFieldNeutral, bias);
			return out;
		};
		const auto reference = [&](const PartialFields& f) {
			PartialSpectrum out;
			feathers::partials::buildSound(f, detail, out);
			return out;
		};
		const PartialSpectrum zero = biased(0.0f);
		const PartialSpectrum neutral = biased(feathers::kFieldNeutral);
		const PartialSpectrum full = biased(1.0f);
		// 0 is f·0, exactly 0: to the bit. The middle is the field untouched: to the bit.
		CHECK(zero == reference(forced(raw, b, 0.0f)));
		CHECK(neutral == reference(raw));
		// 1 is f + (1 − f)·1, which is 1 to a float's rounding, so the spectrum is buildSound at 1 to that rounding.
		CHECK(maxDiff(full, reference(forced(raw, b, 1.0f))) < 1e-6);
		// Not vacuous: the three are three different spectra.
		CHECK(maxDiff(zero, neutral) > 1e-3);
		CHECK(maxDiff(full, neutral) > 1e-3);
	}
}

TEST_CASE("additive bias: all six at neutral is the spectrum with no bias at all, to the bit, everywhere on the window") {
	// TODAY'S ROAD, restated: the raw fields with only the Smoothness knob applied, through buildSound. At neutral every
	// bias returns its field exactly, so the bank's spectrum is that one bit for bit, at any Smoothness.
	int checked = 0;
	for (int step = -8 * 8; step <= 11 * 8; ++step) {
		for (const double tx : { 0.0, 0.23 }) {
			for (const float smoothKnob : { feathers::kFieldNeutral, 0.8f }) {
				SoundCoord s = sound(0, static_cast<float>(step) / 8.0f + 0.011f, 1.0, tx, 0.4 * tx);
				s.smoothnessScale = smoothKnob;
				PartialFields today = rawFields(s);
				today.smoothness = feathers::fieldWithOverride(today.smoothness, smoothKnob);
				PartialSpectrum want;
				feathers::partials::buildSound(today, rawDetail(s), want);
				REQUIRE(s.partialBias == feathers::PartialBias {});
				const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
				const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, s.residue.partials);
				PartialSpectrum got;
				feathers::buildPartialSound(frame, home, s.travelX, s.travelY, got, smoothKnob, s.partialBias);
				CHECK(got == want);
				++checked;
			}
		}
	}
	CHECK(checked > 600);
}

TEST_CASE("additive bias: Smoothness 1 is the smooth spectrum whatever the six knobs ask for") {
	// DESIGN §4.1's PROMISE: the bias is applied before the Smoothness pull reads the fields, so σ = 1 still zeroes
	// Detail, Comb, Odd/Even, the formant gains and Off-grid and takes Rolloff to 1, wherever the knobs put them.
	PartialFields smooth;
	smooth.rolloff = 1.0f;
	PartialSpectrum want;
	feathers::partials::buildSound(smooth, std::array<float, kMaxPartials> {}, want);
	const std::array<feathers::PartialBias, 4> asks { {
	    { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
	    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
	    { 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	    { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f },
	} };
	for (int step = -8; step <= 11; step += 3) {
		for (const feathers::PartialBias& ask : asks) {
			SoundCoord s = sound(0, static_cast<float>(step) + 0.37f, 1.0, 0.31, -0.15);
			s.partialBias = ask;
			INFO(step);
			INFO(ask.rolloff);
			INFO(ask.formant);
			// Not vacuous: at the neutral Smoothness knob this bias makes something other than the smooth spectrum.
			CHECK(maxDiff(built(additive({ s })), want) > 1e-3);
			s.smoothnessScale = 1.0f;
			CHECK(maxDiff(built(additive({ s })), want) < 1e-6);
		}
	}
}

TEST_CASE("additive bias: each sound's bias is its own, applied before the puck's weight") {
	// Two sounds, two different biases: the blend is the weighted sum of EACH sound built with ITS OWN knobs (the
	// single-sound cases above pin what a sound built with knobs is), renormalised.
	SoundCoord a = sound(3, 1.3f, 0.6);
	SoundCoord b = sound(700, 4.1f, 0.4, 0.2, -0.1);
	a.partialBias.comb = 1.0f;
	a.partialBias.rolloff = 0.1f;
	b.partialBias.oddEven = 1.0f;
	b.partialBias.formant = 0.9f;
	const Character c = additive({ a, b });
	const PartialSpectrum sa = soloSpectrum(c, a);
	const PartialSpectrum sb = soloSpectrum(c, b);
	const PartialSpectrum want = rescaledToTarget(weightedSum({ { 0.6, &sa }, { 0.4, &sb } }));
	CHECK(maxDiff(built(c), want) < 1e-6);
	// Not vacuous: swapping the two biases (each sound wearing the other's) builds something else.
	SoundCoord a2 = a;
	SoundCoord b2 = b;
	std::swap(a2.partialBias, b2.partialBias);
	CHECK(maxDiff(built(additive({ a2, b2 })), want) > 1e-3);
}

// ---- quality round (2026-10-08) ---------------------------------------------------------------------------

TEST_CASE("additive: a slot that held a flagship build and is rebuilt as Partials keeps none of the flagship") {
	// The Partials branch clears the operated flag, the vowel and the recipe a previous build left in the slot: the
	// voice's FM, grid and vowel roads read those, so stale bits there would be V5's FM on an additive sound.
	// A V5 constellation that is OPERATED, IN VOWEL COUNTRY and has FM (bench_partials_build's eight sounds, which
	// measured all three), so every clearing line has something to clear.
	Character v5;
	v5.engine = OscEngine::Morph;
	v5.flagship = true;
	v5.count = 8;
	for (int i = 0; i < 8; ++i) {
		SoundCoord& s = v5.sounds[static_cast<size_t>(i)];
		s.zone = static_cast<uint16_t>(97 * i + 13);
		s.phaseOffset = -6.0f + 2.1f * static_cast<float>(i);
		s.weight = 1.0 / 8.0;
		s.fmDepth = 1.0f;
	}
	PreparedCharacter slot;
	feathers::PartialsScratch scratch;
	CharacterBank::buildOne(v5, slot, scratch);
	REQUIRE(slot.recipe.fmDepth > 0.0f); // the slot really held a flagship recipe with FM in it
	REQUIRE(slot.fmDepth() > 0.0f);
	REQUIRE(slot.flagship.operated);
	REQUIRE(slot.vox.audible);
	CharacterBank::buildOne(additive({ sound(101, 0.0f, 1.0) }), slot, scratch);
	CHECK(slot.engine == OscEngine::Partials);
	CHECK(slot.recipe.fmDepth == 0.0f);
	CHECK(slot.recipe.warpDepth == 0.0f);
	CHECK_FALSE(slot.flagship.operated);
	CHECK_FALSE(slot.vox.audible);
	CHECK(slot.fmDepth() == 0.0f);
	CHECK(slot.ampNoise() == 0.0f);
	CHECK(slot.phaseJitter() == 0.0f);
}

TEST_CASE("additive: a sound with no share is never built, and the blend is unchanged") {
	const SoundCoord a = sound(101, 0.0f, 0.5);
	const SoundCoord b = sound(613, 0.37f, 0.5, 0.21, -0.13);
	SoundCoord idle = sound(777, -1.5f, 0.0);
	idle.shapeAmount = 1.0f; // would change the blend if it were built and leaked in
	feathers::PartialsScratch scratch;
	PartialSpectrum got;
	const Character three = additive({ a, idle, b }, ShapeKind::Square);
	feathers::buildPartialsFor(three, got, scratch);
	CHECK(scratch.soundsBuilt == 2);
	PartialSpectrum two;
	feathers::buildPartialsFor(additive({ a, b }, ShapeKind::Square), two, scratch);
	CHECK(scratch.soundsBuilt == 2);
	CHECK(got == two);
	// Nothing to share plays the first sound, and builds only it.
	feathers::buildPartialsFor(additive({ sound(101, 0.0f, 0.0), sound(613, 0.37f, 0.0), sound(5, 2.0f, 0.0) }), got,
	                           scratch);
	CHECK(scratch.soundsBuilt == 1);
}

TEST_CASE("additive: an infinite or a negative weight still gives a finite, convex blend") {
	const SoundCoord a = sound(101, 0.0f, 0.5);
	const SoundCoord b = sound(613, 0.37f, 0.5, 0.21, -0.13);
	const auto finite = [](const PartialSpectrum& s) {
		for (const float x : s.envelope) {
			if (!std::isfinite(x)) {
				return false;
			}
		}
		for (int i = 0; i < kMaxPartials; ++i) {
			if (!std::isfinite(s.fine[i]) || !std::isfinite(s.ratio[i]) || !std::isfinite(s.envAtRatio[i])) {
				return false;
			}
		}
		return true;
	};
	SUBCASE("infinite: nothing finite to share, so the first sound plays alone") {
		SoundCoord inf = b;
		inf.weight = std::numeric_limits<double>::infinity();
		const Character c = additive({ a, inf });
		const PartialSpectrum got = built(c);
		CHECK(finite(got));
		CHECK(maxDiff(got, rescaledToTarget(soloSpectrum(c, a))) < 1e-6);
	}
	SUBCASE("negative: counts as no share, so the other sound has all of it") {
		SoundCoord neg = a;
		neg.weight = -0.5;
		const Character c = additive({ neg, b });
		const PartialSpectrum got = built(c);
		CHECK(finite(got));
		CHECK(maxDiff(got, rescaledToTarget(soloSpectrum(c, b))) < 1e-6);
		CHECK(got.ratio[0] == 1.0f); // convex: harmonic 1 stays at 1
	}
	SUBCASE("NaN: the same as negative") {
		SoundCoord nan = a;
		nan.weight = std::nan("");
		const Character c = additive({ nan, b });
		const PartialSpectrum got = built(c);
		CHECK(finite(got));
		CHECK(maxDiff(got, rescaledToTarget(soloSpectrum(c, b))) < 1e-6);
	}
}

TEST_CASE("additive shape: one bank's reused scratch builds each character's own shape") {
	SoundCoord s = sound(101, 0.0f, 1.0);
	s.shapeAmount = 1.0f;
	CharacterBank bank;
	bank.setSampleRate(kSampleRate);
	bank.setCharacter(additive({ s }, ShapeKind::Sine));
	PartialSpectrum sine;
	feathers::partials::buildShapeSpectrum(ShapeKind::Sine, sine);
	REQUIRE(maxDiff(bank.current().partials, sine) < 1e-6);
	bank.advance(1 << 20); // past the fade guard, so the next request builds at once
	bank.setCharacter(additive({ s }, ShapeKind::Square));
	bank.advance(1 << 20);
	PartialSpectrum square;
	feathers::partials::buildShapeSpectrum(ShapeKind::Square, square);
	CHECK(maxDiff(bank.current().partials, square) < 1e-6);
	CHECK(maxDiff(sine, square) > 0.01); // not vacuous
}

TEST_CASE("additive shape: an amount above 1 is 1, and a shape kind out of range is a Saw") {
	const SoundCoord s = sound(613, 0.37f, 1.0, 0.21, -0.13);
	const PartialSpectrum terrain = soloSpectrum(additive({ s }), s);
	PartialSpectrum shape;
	feathers::partials::buildShapeSpectrum(ShapeKind::Triangle, shape);
	PartialSpectrum atOne = terrain;
	feathers::partials::blendTowardShape(atOne, shape, 1.0f);
	for (const float over : { 1.5f, 40.0f, std::numeric_limits<float>::infinity() }) {
		INFO(over);
		PartialSpectrum got = terrain;
		feathers::partials::blendTowardShape(got, shape, over);
		CHECK(got == atOne);
	}
	PartialSpectrum saw;
	feathers::partials::buildShapeSpectrum(ShapeKind::Saw, saw);
	for (const int raw : { static_cast<int>(ShapeKind::kCount), 7, 255 }) {
		INFO(raw);
		PartialSpectrum got;
		feathers::partials::buildShapeSpectrum(static_cast<ShapeKind>(raw), got);
		CHECK(got == saw);
	}
}

// ---- the voice (plan Task 9a) -------------------------------------------------------------------------------
//
// OscVoice's Partials road, asserted by VALUE through the voice's seams on its own rotator bank: increments,
// amplitudes and phases read back, never inferred from a buffer. Every reference is restated here: the increment
// 2π·f0·ratio/sr, the linear Nyquist fade, the one-pole glide in closed form, Newman's phases, Tilt's compensation as
// spectral_shaper.h defines it. A rendered buffer is read only where the claim IS about the buffer (the note-on's first
// samples, an engine switch's continuity, exact silence).

#include "dsp/osc_voice.h"
#include "dsp/partial_bank.h"
#include "dsp/spectral_shaper.h"
#include "dsp/vocode_bands.h"
#include "dsp/vox_anchors.h"
#include "voice_harness.h"

#include <memory>
#include <string>
#include <numbers>
#include <vector>

namespace {

using feathers::OscVoice;
using feathers::test::BankedVoice;
namespace pb = feathers::partials;
namespace sp = feathers::spectral;

constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr int kBlock = pb::kControlBlock;

/// The Nyquist fade, restated from partial_bank.h's promise: 1 at or below 0.90π, 0 at or above 0.98π, linear between,
/// and 0 for a zero, negative or NaN increment.
double fadeRestated(double w) {
	const double a = 0.90 * std::numbers::pi, b = 0.98 * std::numbers::pi;
	if (!(w > 0.0) || w >= b) {
		return 0.0;
	}
	return w <= a ? 1.0 : (b - w) / (b - a);
}

double hzOfKey(double key) {
	return 440.0 * std::exp2((key - 69.0) / 12.0);
}

/// One voice and its bank on the heap (a CharacterBank is large), at 48 kHz, the note started as the synth starts one:
/// reset, then the pitch.
std::unique_ptr<BankedVoice> voiceOn(const Character& c, double key) {
	auto v = std::make_unique<BankedVoice>();
	v->setSampleRate(kSampleRate);
	v->setCharacter(c);
	v->reset();
	v->voice.setPitch(key);
	return v;
}

/// The prepared table the voice plays (ear 0 or 1): its spectrum is the input the restatements read.
const PartialSpectrum& playing(const BankedVoice& v, int ear = 0) {
	return v.bank.current(ear).partials;
}

/// Render `n` mono samples in host blocks of `block`, returning them.
std::vector<float> renderMono(BankedVoice& v, int n, int block = kBlock) {
	std::vector<float> out(static_cast<size_t>(n));
	for (int done = 0; done < n; done += block) {
		v.render(out.data() + done, std::min(block, n - done));
	}
	return out;
}

/// The count of partials that render at `hz`: one past the last whose restated fade is not zero.
int renderingCount(const PartialSpectrum& s, double hz) {
	int count = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double w = static_cast<double>(static_cast<float>(kTwoPi * hz * s.ratio[i] / kSampleRate));
		if (fadeRestated(w) > 0.0) {
			count = i + 1;
		}
	}
	return count;
}

/// The amplitude the bank renders partial i at with a neutral drawer, restated: envAtRatio·fine × the fade at the
/// increment `w` (the LEAD ear's), with the bank's silence floor.
double neutralAmplitude(const PartialSpectrum& s, int i, double w) {
	const double a = static_cast<double>(s.envAtRatio[i] * s.fine[i]) * fadeRestated(w);
	return std::abs(a) < pb::kSilenceFloor ? 0.0 : a;
}

/// A sound whose off-grid field is well on, so the ratios are not integers.
SoundCoord offGridSound() {
	return sound(0, offsetWhere([](const PartialFields& f) { return f.offGrid > 0.6f && f.rolloff < 0.4f; }), 1.0);
}

/// THE NOTE-ON'S PHASES, restated by the double sum: φ_k = −2π·Σ_{l<k} (k − l)·p_l, p_l = A_l² / Σ A² over the partials
/// that render, A the neutral amplitude each renders at (both ears' mean power in stereo).
std::vector<double> schroederRestated(const PartialSpectrum& l, const PartialSpectrum* r, double hz) {
	const int n = renderingCount(l, hz);
	std::vector<double> power(static_cast<size_t>(n));
	double total = 0.0;
	for (int i = 0; i < n; ++i) {
		const double w = static_cast<float>(kTwoPi * hz * l.ratio[i] / kSampleRate);
		const double a = neutralAmplitude(l, i, w), b = r != nullptr ? neutralAmplitude(*r, i, w) : a;
		power[size_t(i)] = 0.5 * (a * a + b * b);
		total += power[size_t(i)];
	}
	std::vector<double> phi(static_cast<size_t>(kMaxPartials));
	for (int k = 0; k < kMaxPartials; ++k) {
		double turns = 0.0;
		for (int i = 0; i < std::min(k, n); ++i) {
			turns += double(k - i) * power[size_t(i)] / total;
		}
		phi[size_t(k)] = -kTwoPi * turns;
	}
	return phi;
}

/// The difference of two angles, wrapped to (−π, π].
double angleDiff(double a, double b) {
	return std::remainder(a - b, kTwoPi);
}
} // namespace

TEST_CASE("additive voice: the partials sit at f0 times their ratios, at the host rate") {
	const SoundCoord s = offGridSound();
	int offGrid = 0;
	for (const double key : { 36.0, 69.0, 84.0 }) {
		INFO("key " << key);
		auto v = voiceOn(additive({ s }), key);
		renderMono(*v, kBlock);
		const PartialSpectrum& spec = playing(*v);
		const double f0 = hzOfKey(key);
		const int count = renderingCount(spec, f0);
		REQUIRE(count > 0);
		for (int i = 0; i < count; ++i) {
			const double want = kTwoPi * f0 * spec.ratio[i] / kSampleRate;
			INFO("partial " << i);
			CHECK(std::abs(v->voice.partialIncrementForTest(i) - want) < 2e-6);
			offGrid += std::abs(spec.ratio[i] - std::round(spec.ratio[i])) > 0.05f ? 1 : 0;
		}
		CHECK(offGrid > 0); // the ratios being checked are not all whole numbers
	}
}

TEST_CASE("additive voice: the targets are the spectrum with a neutral drawer, each ear its own") {
	SUBCASE("mono") {
		const SoundCoord s = offGridSound();
		auto v = voiceOn(additive({ s }), 57.0);
		renderMono(*v, kBlock);
		const PartialSpectrum& spec = playing(*v);
		const double f0 = hzOfKey(57.0);
		int sounding = 0;
		for (int i = 0; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
			const double want = neutralAmplitude(spec, i, w);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - want) < 1e-7);
			CHECK(v->voice.partialAmplitudeForTest(i, 1) == v->voice.partialAmplitudeForTest(i, 0));
			sounding += want > 1e-4 ? 1 : 0;
		}
		CHECK(sounding > 20);
	}
	SUBCASE("stereo: each ear's spectrum over the left ear's frequencies") {
		const SoundCoord l = sound(211, 0.3f, 1.0), r = sound(640, -1.7f, 1.0);
		auto v = std::make_unique<BankedVoice>();
		v->setSampleRate(kSampleRate);
		v->setCharacterStereo(additive({ l }), additive({ r }));
		v->reset();
		v->voice.setPitch(60.0);
		float outL[kBlock], outR[kBlock];
		v->renderStereo(outL, outR, kBlock);
		const PartialSpectrum& sl = playing(*v, 0);
		const PartialSpectrum& sr = playing(*v, 1);
		REQUIRE(maxDiff(sl, sr) > 0.01);
		const double f0 = hzOfKey(60.0);
		for (int i = 0; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			const double w = static_cast<float>(kTwoPi * f0 * sl.ratio[i] / kSampleRate);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - neutralAmplitude(sl, i, w)) < 1e-7);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 1) - neutralAmplitude(sr, i, w)) < 1e-7);
		}
	}
}

TEST_CASE("additive voice: a note-on starts at Schroeder's phases weighted by the note's spectrum, amplitudes on target") {
	// The first samples of a note, restated whole: out[n] = Σ amp_i·sin(φ_i + (n+1)·w_i), φ the Schroeder phases weighted
	// by the power each partial renders at this pitch (design §2.4, decided 2026-10-10) and amp_i the target itself
	// from the first sample (no ramp from zero: §5.2). And the phases read back, partial by partial.
	for (const double key : { 40.0, 76.0, 96.0 }) {
		INFO("key " << key);
		auto v = voiceOn(additive({ sound(377, 0.6f, 1.0) }), key);
		const std::vector<float> out = renderMono(*v, kBlock);
		const PartialSpectrum& spec = playing(*v);
		const double f0 = hzOfKey(key);
		const int n = renderingCount(spec, f0);
		CHECK(v->voice.partialsResetCountForTest() == n);
		CHECK(v->voice.partialsResetsForTest() == 1);
		const std::vector<double> phi = schroederRestated(spec, nullptr, f0);
		double maxErr = 0.0, peak = 0.0;
		for (int t = 0; t < 4; ++t) {
			double want = 0.0;
			for (int i = 0; i < kMaxPartials; ++i) {
				const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
				want += neutralAmplitude(spec, i, w) * std::sin(phi[size_t(i)] + double(t + 1) * w);
			}
			maxErr = std::max(maxErr, std::abs(double(out[t]) - want));
			peak = std::max(peak, std::abs(want));
		}
		CHECK(maxErr < 2e-5);
		CHECK(peak > 0.01); // the samples compared are not silence
		for (int i = 0; i < v->voice.partialsLiveForTest(); ++i) {
			INFO("partial " << i);
			const double w = v->voice.partialIncrementForTest(i);
			CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), phi[size_t(i)] + kBlock * w)) < 1e-4);
		}
	}
}
TEST_CASE("additive voice: glide moves the increments at control rate") {
	// From C3 toward C4 with a 50 ms glide. The increments of block b are the pitch after b blocks, restated in closed
	// form: target − (target − start)·exp(−32b/(τ·sr)). And a ramp (a routed pitch's straight line) the same way.
	const SoundCoord s = sound(90, 0.1f, 1.0);
	SUBCASE("the exponential glide") {
		auto v = voiceOn(additive({ s }), 48.0);
		v->voice.setGlideSeconds(0.05);
		v->voice.glideToPitch(60.0);
		const PartialSpectrum& spec = playing(*v);
		for (int b = 0; b < 40; ++b) {
			renderMono(*v, kBlock);
			const double pitch = 60.0 - 12.0 * std::exp(-double(kBlock * b) / (0.05 * kSampleRate));
			const double f0 = hzOfKey(pitch);
			INFO("block " << b);
			for (const int i : { 0, 3, 11 }) {
				CHECK(std::abs(v->voice.partialIncrementForTest(i) - kTwoPi * f0 * spec.ratio[i] / kSampleRate) < 2e-6);
			}
		}
	}
	SUBCASE("a ramp to pitch") {
		auto v = voiceOn(additive({ s }), 48.0);
		v->voice.rampToPitch(55.0, 200);
		const PartialSpectrum& spec = playing(*v);
		for (int b = 0; b < 10; ++b) {
			renderMono(*v, kBlock);
			const double pitch = 48.0 + 7.0 * std::min(1.0, double(kBlock * b) / 200.0);
			INFO("block " << b);
			CHECK(std::abs(v->voice.partialIncrementForTest(0) - kTwoPi * hzOfKey(pitch) * spec.ratio[0] / kSampleRate)
			      < 2e-6);
		}
	}
}

TEST_CASE("additive voice: an engine switch crossfades the two roads with weights that sum to one, and no step") {
	// V5 -> Additive and back, mid-note. Every host sample of the fade: the 4x side's weight and the bank's sum to 1,
	// and the bank's goes 0 -> 1 over the bank's fade length. The output's first difference is held to what the two
	// signals' own slopes and the weights' slope allow: |Δy| <= max|Δa| + max|Δb| + max|Δw|·(max|a| + max|b|).
	// THE WEIGHT ASSERTS ARE THE GUARD; THE SLOPE BOUND IS ONLY A BACKSTOP. Measured: with the complementary weight
	// dropped (the bank at full weight from the flip) the bound still held, because the bank starts on its note-on
	// phases, whose low partials sit near sin(0); only the weights went red.
	const Character v5 = feathers::test::characterAt(OscEngine::Morph, 512, 0.25f);
	const Character add = additive({ sound(300, 0.2f, 1.0) });
	const double key = 52.0;
	// The two signals alone, for their slopes and levels (steady state).
	const auto stats = [&](const Character& c) {
		auto v = voiceOn(c, key);
		const std::vector<float> y = renderMono(*v, 4800);
		double slope = 0.0, level = 0.0;
		for (size_t n = 2400; n < y.size(); ++n) {
			slope = std::max(slope, std::abs(double(y[n]) - double(y[n - 1])));
			level = std::max(level, std::abs(double(y[n])));
		}
		return std::pair { slope, level };
	};
	const auto [slopeV5, levelV5] = stats(v5);
	const auto [slopeAdd, levelAdd] = stats(add);
	REQUIRE(levelV5 > 0.05);
	REQUIRE(levelAdd > 0.05);

	for (const bool intoAdditive : { true, false }) {
		INFO((intoAdditive ? "V5 -> Additive" : "Additive -> V5"));
		auto v = voiceOn(intoAdditive ? v5 : add, key);
		std::vector<float> y = renderMono(*v, 2400);
		const uint32_t chunksBefore = v->voice.partialsChunksForTest();
		v->setCharacter(intoAdditive ? add : v5);
		const int fadeLength = v->bank.fadeLengthSamples();
		std::vector<double> wTable, wBank;
		for (int b = 0; b < 8; ++b) {
			float out[kBlock];
			v->render(out, kBlock);
			y.insert(y.end(), out, out + kBlock);
			if (int(wBank.size()) < fadeLength + kBlock) {
				for (int h = 0; h < kBlock; ++h) {
					float t = 0.0f, p = 0.0f;
					v->voice.mixWeightsForTest(h, t, p);
					wTable.push_back(t);
					wBank.push_back(p);
				}
			}
		}
		CHECK(v->voice.partialsChunksForTest() > chunksBefore);
		REQUIRE(int(wBank.size()) >= fadeLength);
		double maxDw = 0.0;
		for (int h = 0; h < fadeLength; ++h) {
			INFO("host sample " << h);
			CHECK(std::abs(wTable[size_t(h)] + wBank[size_t(h)] - 1.0) < 1e-6);
			// The bank's own share, from the fade's t restated: the mean of t over the four evaluations of sample h.
			double t = 0.0;
			for (int j = 4 * h; j < 4 * h + 4; ++j) {
				t += double(j + 1) / double(4 * fadeLength);
			}
			t *= 0.25;
			CHECK(std::abs(wBank[size_t(h)] - (intoAdditive ? t : 1.0 - t)) < 1e-6);
			if (h > 0) {
				maxDw = std::max(maxDw, std::abs(wBank[size_t(h)] - wBank[size_t(h - 1)]));
			}
		}
		CHECK(std::abs(wBank[size_t(fadeLength - 1)] - (intoAdditive ? 1.0 : 0.0)) < 0.01);
		// Continuity across the whole switch, from before it to after it.
		const double bound = 1.1 * (slopeV5 + slopeAdd + maxDw * (levelV5 + levelAdd));
		double worst = 0.0;
		for (size_t n = 2000; n < y.size(); ++n) {
			worst = std::max(worst, std::abs(double(y[n]) - double(y[n - 1])));
		}
		CAPTURE(worst);
		CAPTURE(bound);
		CHECK(worst <= bound);
	}
}

TEST_CASE("additive voice: other engines never enter the Partials branch") {
	feathers::WeaveString string;
	for (const OscEngine e : { OscEngine::Morph, OscEngine::Stair, OscEngine::Shapes, OscEngine::Weave }) {
		INFO("engine " << int(e));
		Character c = feathers::test::characterAt(e, 401, 0.4f);
		auto v = voiceOn(c, 60.0);
		v->voice.setWeaveString(&string);
		renderMono(*v, 4096, 61);
		float l[128], r[128];
		for (int b = 0; b < 16; ++b) {
			v->renderStereo(l, r, 128);
		}
		// A character change within the engine: a fade, which is the road's other door.
		Character d = feathers::test::characterAt(e, 77, -0.3f);
		v->setCharacter(d);
		renderMono(*v, 1024, 37);
		CHECK(v->voice.partialsChunksForTest() == 0);
	}
}

TEST_CASE("additive voice: Formant Shift at a ratio of exactly 1 gives targets bit-identical to the drawer off") {
	// A BLEND (two sounds), where the blended table read at the blended ratio is NOT the blend of the reads, so a re-read
	// at ratio 1 would move the targets: the fast path is what keeps the switch silent at no shift.
	const Character c = additive({ sound(120, 0.3f, 0.5), sound(860, -2.1f, 0.5) });
	auto off = voiceOn(c, 62.0);
	auto on = voiceOn(c, 62.0);
	sp::ShaperParams p;
	p.formantShift.on = true;
	p.formantShift.lowSemitones = 0.0f;
	p.formantShift.highSemitones = 0.0f;
	on->voice.setShaperParamsForTest(p);
	renderMono(*off, kBlock);
	renderMono(*on, kBlock);
	int differs = 0;
	const PartialSpectrum& spec = playing(*on);
	for (int i = 0; i < kMaxPartials; ++i) {
		INFO("partial " << i);
		CHECK(on->voice.partialAmplitudeForTest(i, 0) == off->voice.partialAmplitudeForTest(i, 0));
		differs += pb::envelopeRead(spec, spec.ratio[i]) != spec.envAtRatio[i] ? 1 : 0;
	}
	CHECK(differs > 0); // the re-read WOULD move them: the identity is not vacuous
}

TEST_CASE("additive voice: Tilt's compensation sees only the partials that render") {
	// A high note, where harmonics climb into the Nyquist fade and past it. Tilt +12 dB/oct, everything else off. The
	// target restated from spectral_shaper.h's definition: amp_i = base_i·T_i·c, base_i = envAtRatio_i·fine_i·fade_i,
	// T_i = tiltGain(hz_i), c = sqrt(Σ base² / Σ (base·T)²) over the partials that render.
	const Character c = additive({ sound(20, 0.0f, 1.0) });
	sp::ShaperParams p;
	p.tilt.on = true;
	p.tilt.dbPerOctave = 12.0f;
	p.tilt.pivotHz = 1000.0f;
	const double key = 98.0; // ~2349 Hz: harmonics 10 and 11 in or past the fade
	auto v = voiceOn(c, key);
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const double f0 = hzOfKey(key);
	std::array<double, kMaxPartials> base {}, tilt {};
	double num = 0.0, den = 0.0, numAll = 0.0, denAll = 0.0;
	int partlyFaded = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double hz = static_cast<float>(f0 * spec.ratio[i]);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		const double raw = double(spec.envAtRatio[i] * spec.fine[i]);
		base[i] = raw * fadeRestated(w);
		tilt[i] = sp::tiltGain(p.tilt, hz);
		num += base[i] * base[i];
		den += base[i] * tilt[i] * base[i] * tilt[i];
		numAll += raw * raw;
		denAll += raw * tilt[i] * raw * tilt[i];
		partlyFaded += fadeRestated(w) > 0.0 && fadeRestated(w) < 1.0 && raw > 1e-4 ? 1 : 0;
	}
	REQUIRE(partlyFaded > 0);
	const double comp = std::sqrt(num / den), compAll = std::sqrt(numAll / denAll);
	CHECK(std::abs(comp / compAll - 1.0) > 0.05); // the culled partials would move the level: not vacuous
	for (int i = 0; i < kMaxPartials; ++i) {
		INFO("partial " << i);
		const double want = base[i] * tilt[i] * comp;
		CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - want) < 1e-5 * std::max(1.0, std::abs(want)) + 1e-7);
	}
}

namespace {
/// THE AMPLITUDES A FILTER + COMB DRAWER RENDERS, restated from spectral_shaper.h's reference (no Tilt, so no
/// compensation): raw_i·fade_i·filterGain(hz_i, f0)·combGain(i + 1), a gain below kFlushGain flushed to 0.
std::vector<double> filterCombRestated(const PartialSpectrum& s, const sp::ShaperParams& p, double f0) {
	std::vector<double> amp(static_cast<size_t>(kMaxPartials), 0.0);
	for (int i = 0; i < kMaxPartials; ++i) {
		const double hz = static_cast<float>(f0 * s.ratio[i]);
		const double w = static_cast<float>(kTwoPi * f0 * s.ratio[i] / kSampleRate);
		const double g = double(sp::filterGain(p.filter, hz, f0)) * double(sp::combGain(p.comb, i + 1.0));
		const double a = double(s.envAtRatio[i] * s.fine[i]) * fadeRestated(w) * (g >= double(sp::kFlushGain) ? g : 0.0);
		amp[size_t(i)] = std::abs(a) < pb::kSilenceFloor ? 0.0 : a;
	}
	return amp;
}
} // namespace

TEST_CASE("additive voice: a neutral drawer does no drawer work, and its targets are the drawer-off targets to the bit") {
	// The default drawer, an explicit neutral one, a DISABLED one with every operator on, and one whose only operator is
	// Formant Shift at a ratio of exactly 1 (no amplitude operator: the prepared shaper is the identity). None recomputes a
	// factor group or takes the product: the identity is bypassed, not computed as 1.
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	auto off = voiceOn(c, 57.0);
	sp::ShaperParams disabled;
	disabled.enabled = false;
	disabled.filter = { true, sp::FilterMode::LowPass, 900.0f, 0.5f, 0.5f, 0.0f };
	disabled.comb = { true, 3.0f, 0.8f, 0.1f };
	disabled.tilt = { true, -6.0f, 1000.0f };
	disabled.vowel = { true, 750.0f, 1150.0f, 1.0f };
	disabled.partialCount = { true, 5.5f }; // the voice's own operators are the master's too
	disabled.damp = { true, 20.0f, 3.0f };
	disabled.stretch = { true, 0.005f };
	disabled.shift = { true, -300.0f };
	sp::ShaperParams formantOnly;
	formantOnly.formantShift.on = true;
	const sp::ShaperParams drawers[] = { sp::ShaperParams {}, disabled, formantOnly };
	renderMono(*off, 8 * kBlock);
	for (const sp::ShaperParams& p : drawers) {
		auto v = voiceOn(c, 57.0);
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 8 * kBlock);
		for (int i = 0; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			CHECK(v->voice.partialAmplitudeForTest(i, 0) == off->voice.partialAmplitudeForTest(i, 0));
		}
		const auto w = v->voice.drawerWorkForTest();
		CHECK(w.harmonic == 0);
		CHECK(w.filter == 0);
		CHECK(w.tilt == 0);
		CHECK(w.applies == 0);
	}
	CHECK(off->voice.drawerWorkForTest().applies == 0);
}

TEST_CASE("additive voice: a held drawer recomputes nothing, and a moved cutoff or pitch reaches the very next block's targets") {
	const Character c = additive({ sound(20, 0.0f, 1.0) });
	sp::ShaperParams p;
	p.filter = { true, sp::FilterMode::LowPass, 1500.0f, 0.5f, 0.5f, 0.5f }; // key track 0.5: f0 moves the cutoff
	p.comb = { true, 3.0f, 0.6f, 0.15f };
	double key = 55.0;
	auto v = voiceOn(c, key);
	v->voice.setShaperParamsForTest(p);
	const auto check = [&](const char* when) {
		INFO(when);
		const std::vector<double> want = filterCombRestated(playing(*v), p, hzOfKey(key));
		int sounding = 0;
		for (int i = 0; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - want[size_t(i)]) <= 1e-5 * std::abs(want[size_t(i)]) + 1e-9);
			sounding += want[size_t(i)] > 1e-6 ? 1 : 0;
		}
		CHECK(sounding > 10);
		return want;
	};
	renderMono(*v, kBlock);
	const std::vector<double> at1500 = check("the first block");
	const auto first = v->voice.drawerWorkForTest();
	CHECK(first.harmonic == 1);
	CHECK(first.filter == 1);
	CHECK(first.tilt == 0); // Tilt is off: it costs nothing
	// HELD: forty blocks, nothing recomputed, the same targets.
	renderMono(*v, 40 * kBlock);
	const auto held = v->voice.drawerWorkForTest();
	CHECK(held.prepares == first.prepares);
	CHECK(held.harmonic == 1);
	CHECK(held.filter == 1);
	check("held");
	// THE CUTOFF MOVES: the next block's targets are the new cutoff's, and only the filter's factors were recomputed.
	p.filter.cutoffHz = 4000.0f;
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, kBlock);
	const std::vector<double> at4000 = check("cutoff 4000");
	CHECK(v->voice.drawerWorkForTest().filter == 2);
	CHECK(v->voice.drawerWorkForTest().harmonic == 1);
	int moved = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		moved += std::abs(at4000[size_t(i)] - at1500[size_t(i)]) > 1e-4 * std::abs(at1500[size_t(i)]) + 1e-6 ? 1 : 0;
	}
	CHECK(moved > 10); // not vacuous: the cutoff moved the targets
	// THE PITCH MOVES (key track): the next block's targets are the new f0's, its key-tracked cutoff included.
	key = 59.0;
	v->voice.setPitch(key);
	renderMono(*v, kBlock);
	check("pitch up four semitones");
	CHECK(v->voice.drawerWorkForTest().harmonic == 1); // the harmonic group reads k alone
}

TEST_CASE("additive voice: a Phase move too big to fold turns each partial by its ratio times the move, at once") {
	// One block at offset 0, then the offset jumps by Δ = 0.137: folded into one block's increments the full-gain top
	// partial would pass π, so it is one step. Every live partial's phase is where it was, plus a block's rotation
	// 32·w_i at the UN-bent increment, plus ratio_i·2π·Δ, restated (mod 2π). Off-grid ratios, so the turn is not a
	// whole-cycle multiple.
	const SoundCoord s = offGridSound();
	auto v = voiceOn(additive({ s }), 50.0);
	v->voice.setReadPhase(0.0f, nullptr, 0);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const double f0 = hzOfKey(50.0);
	const int live = v->voice.partialsLiveForTest();
	REQUIRE(live > 40);
	std::vector<double> before(static_cast<size_t>(live));
	for (int i = 0; i < live; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	const float delta = 0.137f;
	v->voice.setReadPhase(delta, nullptr, 0);
	renderMono(*v, kBlock);
	for (int i = 0; i < live; ++i) {
		INFO("partial " << i);
		// The increment the bank turns by (a partial past the Nyquist fade in a live group keeps whatever it had: it is
		// silent, but it still rotates, so its phase is predicted from its own increment too).
		const double w = v->voice.partialIncrementForTest(i);
		if (i < renderingCount(spec, f0)) {
			CHECK(std::abs(w - kTwoPi * f0 * spec.ratio[i] / kSampleRate) < 2e-6);
		}
		const double want = before[size_t(i)] + kBlock * w + kTwoPi * double(spec.ratio[i]) * double(delta);
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 2e-4);
	}
}

TEST_CASE("additive voice: a small Phase move is a ramp, folded into the block's increments") {
	// Δ = 0.002 cycles over one block: each rendering partial turns by w_i + ratio_i·2π·Δ/32 per sample (wrapped where a
	// partial up in the fade passes π) and lands on its phase + 32·w_i + ratio_i·2π·Δ: the phase moves linearly, as the
	// 4x path's read offset does, rather than in one step.
	const SoundCoord s = offGridSound();
	auto v = voiceOn(additive({ s }), 50.0);
	v->voice.setReadPhase(0.0f, nullptr, 0);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const double f0 = hzOfKey(50.0);
	const int count = renderingCount(spec, f0);
	std::vector<double> before(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	const float delta = 0.002f;
	v->voice.setReadPhase(delta, nullptr, 0);
	renderMono(*v, kBlock);
	for (int i = 0; i < count; ++i) {
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		const double bent = w + double(spec.ratio[i]) * kTwoPi * double(delta) / kBlock;
		CHECK(std::abs(angleDiff(v->voice.partialIncrementForTest(i), bent)) < 4e-6);
		const double want = before[size_t(i)] + kBlock * w + kTwoPi * double(spec.ratio[i]) * double(delta);
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 2e-4);
	}
}

TEST_CASE("additive voice: a note started at a Phase offset, and a Phase run moving inside one render call") {
	// A note at Phase 0.25: after its first block every partial is at its note-on phase + ratio·2π·0.25 + 32·w. Then one
	// render call of two blocks whose run climbs per sample: block A ramps to run[31], block B from run[31] to run[63] —
	// B's increments are w + ratio·2π·(run[63] − run[31])/32, read at B's own index inside the call, and the phases land
	// on ratio·2π·(run[63] − 0.25) past where block A's start left them.
	const SoundCoord s = sound(140, 0.45f, 1.0);
	const double key = 50.0, f0 = hzOfKey(key);
	auto v = voiceOn(additive({ s }), key);
	v->voice.setReadPhase(0.25f, nullptr, 0);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int count = renderingCount(spec, f0);
	const std::vector<double> phi = schroederRestated(spec, nullptr, f0);
	std::vector<double> after1(static_cast<size_t>(count));
	for (int i = 0; i < count; ++i) {
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		const double want = phi[size_t(i)] + kTwoPi * double(spec.ratio[i]) * 0.25 + kBlock * w;
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 2e-4);
		after1[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	std::vector<float> run(2 * kBlock + 8);
	for (size_t j = 0; j < run.size(); ++j) {
		run[j] = 0.25f + 0.00005f * float(j + 1);
	}
	v->voice.setReadPhase(0.0f, run.data(), int(run.size()));
	float out[2 * kBlock];
	v->render(out, 2 * kBlock);
	const double moveB = double(run[2 * kBlock - 1]) - double(run[kBlock - 1]);
	const double moveAll = double(run[2 * kBlock - 1]) - 0.25;
	for (int i = 0; i < count; ++i) {
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		CHECK(std::abs(angleDiff(v->voice.partialIncrementForTest(i), w + double(spec.ratio[i]) * kTwoPi * moveB / kBlock))
		      < 4e-6);
		const double want = after1[size_t(i)] + 2 * kBlock * w + kTwoPi * double(spec.ratio[i]) * moveAll;
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 3e-4);
	}
}

TEST_CASE("additive voice: a spectrum that goes silent leaves the voice output exactly 0.0f, mono and stereo") {
	// The Partials -> Partials fade into a character with no sounds (silence), then the bank's last ramp: from there
	// every sample is EXACTLY zero, not a denormal, and nothing is live (CLAUDE.md: SLEEP is decided by the output).
	// The synth's amp envelope multiplies this; a finite voice at an envelope of 0 is 0, so the voice's half of a
	// note's exact end is that it is finite throughout and exactly silent once its spectrum is.
	Character silent;
	silent.engine = OscEngine::Partials;
	for (const bool stereo : { false, true }) {
		INFO((stereo ? "stereo" : "mono"));
		auto v = voiceOn(additive({ sound(300, 0.2f, 1.0) }), 45.0);
		std::vector<float> l(2048), r(2048);
		const auto run = [&](int n) {
			for (int done = 0; done < n; done += kBlock) {
				if (stereo) {
					v->renderStereo(l.data() + done, r.data() + done, kBlock);
				}
				else {
					v->render(l.data() + done, kBlock);
				}
			}
		};
		run(1024);
		v->setCharacter(silent);
		run(2048);
		const int settled = v->bank.fadeLengthSamples() + 2 * kBlock;
		for (int n = 0; n < 2048; ++n) {
			INFO("sample " << n);
			REQUIRE(std::isfinite(l[size_t(n)]));
			if (n >= settled) {
				CHECK(l[size_t(n)] == 0.0f);
				if (stereo) {
					CHECK(r[size_t(n)] == 0.0f);
				}
			}
		}
		CHECK(v->voice.partialsLiveForTest() == 0);
	}
}

TEST_CASE("additive voice: a Partials -> Partials rebuild retargets over the bank's fade length") {
	// No crossfade of two renders: each block's targets are the two spectra at the fade's t at the block's END, so the
	// amplitude at the end of block b is a + (b' − a)·min(1, 32(b+1)/fadeLength), restated per partial.
	const Character from = additive({ sound(300, 0.2f, 1.0) });
	const Character to = additive({ sound(700, -1.3f, 1.0) });
	const PartialSpectrum a = built(from), b = built(to);
	const double key = 55.0, f0 = hzOfKey(key);
	auto v = voiceOn(from, key);
	renderMono(*v, 1024);
	v->setCharacter(to);
	const int fadeLength = v->bank.fadeLengthSamples();
	for (int blk = 0; blk * kBlock < fadeLength + kBlock; ++blk) {
		renderMono(*v, kBlock);
		const double t = std::min(1.0, double(kBlock * (blk + 1)) / double(fadeLength));
		INFO("block " << blk << " t " << t);
		for (int i = 0; i < 24; ++i) {
			const double w = static_cast<float>(kTwoPi * f0 * b.ratio[i] / kSampleRate);
			const double from_ = double(a.envAtRatio[i] * a.fine[i]), to_ = double(b.envAtRatio[i] * b.fine[i]);
			const double want = (from_ + (to_ - from_) * t) * fadeRestated(w);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - want) < 1e-6);
		}
	}
}

TEST_CASE("additive voice: the output is the same to the bit at host blocks of 32, 61 and 7") {
	// One note with a glide, a Partials -> Partials rebuild and a moving Phase, rendered at three host block sizes. The
	// rebuild lands at the same consumed sample in every run (the last call before it is shortened). THE PHASE RUN IS
	// HANDED OVER AS THE SYNTH HANDS IT (Synth::seatReadPhase): on a fixed 64-sample tick grid, each render call cut at
	// the tick's end and given the run from its first consumed sample to that end (reach = kTick − tickPhase), so a
	// control block straddling a tick reads the tick's last value, as it does in the plugin. The control blocks fall at
	// the same generated samples, and every value they read is the same value, so the outputs are equal bit for bit.
	constexpr int kTick = 64;
	const Character from = additive({ sound(300, 0.2f, 1.0) });
	const Character to = additive({ sound(700, -1.3f, 1.0) });
	const int total = 4096, flipAt = 1024;
	const auto phaseAt = [](int t) { return static_cast<float>(0.05 * std::sin(kTwoPi * double(t) / 3000.0)); };
	std::vector<std::pair<double, double>> states;
	const auto runAt = [&](int block) {
		auto v = voiceOn(from, 48.0);
		v->voice.setGlideSeconds(0.05);
		v->voice.glideToPitch(55.0);
		std::vector<float> out(static_cast<size_t>(total));
		std::vector<float> run(static_cast<size_t>(kTick));
		int done = 0;
		while (done < total) {
			const int reach = kTick - done % kTick;
			int n = std::min({ block, total - done, reach });
			if (done < flipAt && done + n > flipAt) {
				n = flipAt - done;
			}
			for (int j = 0; j < reach; ++j) {
				run[size_t(j)] = phaseAt(done + j);
			}
			v->voice.setReadPhase(0.0f, run.data(), reach);
			v->render(out.data() + done, n);
			done += n;
			if (done == flipAt) {
				v->setCharacter(to);
			}
		}
		CHECK(v->voice.partialsChunksForTest() > 0);
		// And the voice's own state, in double: the pitch the glide reached and the accumulator. A per-chunk advance of either
		// would split one closed-form step into several that round differently in the last bits, which the float
		// increments can hide from the output, so this is asked directly.
		states.push_back({ v->voice.pitchSemitones(), v->voice.phase() });
		return out;
	};
	const std::vector<float> a = runAt(32), b = runAt(61), c = runAt(7);
	REQUIRE(states.size() == 3);
	CHECK(states[1].first == states[0].first);
	CHECK(states[2].first == states[0].first);
	CHECK(states[1].second == states[0].second);
	CHECK(states[2].second == states[0].second);
	int diffB = 0, diffC = 0;
	double peak = 0.0;
	for (int n = 0; n < total; ++n) {
		diffB += a[size_t(n)] != b[size_t(n)] ? 1 : 0;
		diffC += a[size_t(n)] != c[size_t(n)] ? 1 : 0;
		peak = std::max(peak, std::abs(double(a[size_t(n)])));
	}
	CHECK(diffB == 0);
	CHECK(diffC == 0);
	CHECK(peak > 0.05); // not silence
}

TEST_CASE("additive voice: a flip that cuts a Phase ramp short keeps exactly the part of the move that turned") {
	// A Saw exactly (shape amount 1, so the ratios are k and a rebuild changes no amplitude), a Phase run climbing
	// 0.0001 cycles a sample, and a rebuild that lands at consumed sample 1037: the block that began at 1024 has
	// turned 16 of its 32 samples when the flip cuts it. At generated sample 2000 (a block boundary after the flip)
	// every partial's phase is its note-on phase + 2000·w + ratio·2π·run(1999), the commanded offset's whole turn:
	// nothing of the cut block's Δ lost, nothing turned twice.
	Character from = additive({ sound(300, 0.2f, 1.0) });
	Character to = additive({ sound(700, -1.3f, 1.0) });
	from.sounds[0].shapeAmount = 1.0f;
	to.sounds[0].shapeAmount = 1.0f;
	const double key = 48.0, f0 = hzOfKey(key);
	const auto offsetAt = [](int t) { return static_cast<float>(0.0001 * double(t)); };
	auto v = voiceOn(from, key);
	const int flipAt = 1024 + 13, total = 2000;
	const uint32_t generationBefore = v->bank.generation();
	std::vector<float> out(static_cast<size_t>(total)), run(static_cast<size_t>(kBlock + 64));
	int done = 0;
	while (done < total) {
		int n = std::min(kBlock, total - done);
		if (done < flipAt && done + n > flipAt) {
			n = flipAt - done;
		}
		for (size_t j = 0; j < run.size(); ++j) {
			run[j] = offsetAt(done + int(j));
		}
		v->voice.setReadPhase(0.0f, run.data(), int(run.size()));
		v->render(out.data() + done, n);
		done += n;
		if (done == flipAt) {
			v->setCharacter(to);
		}
	}
	REQUIRE(v->bank.generation() != generationBefore); // the rebuild did land
	const PartialSpectrum& spec = playing(*v);
	const std::vector<double> phi = schroederRestated(spec, nullptr, f0);
	const double turn = double(offsetAt(total - 1));
	CHECK(v->voice.partialsResetsForTest() == 1); // one note-on, the flip is a fade
	for (int i = 0; i < 60; ++i) {
		INFO("partial " << i);
		REQUIRE(std::abs(spec.ratio[i] - float(i + 1)) < 1e-4f);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		const double want = phi[size_t(i)] + double(total) * w + kTwoPi * double(spec.ratio[i]) * turn;
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 1e-3);
	}
}

TEST_CASE("additive voice: on the synth's tick, a flip that cuts the SHORT first block keeps the part of its move that turned") {
	// Task 9b's alignment: with the tick phase known (setTickPhase, as Synth::seatReadPhase sets it), a note starting at
	// tick phase 16 opens with a 16-sample block. A rebuild landing 8 samples in cuts it after 8 of its 16, and the part
	// of the Phase ramp that turned is Δ·8/16 — the BLOCK'S OWN length, not kControlBlock. At generated sample 2000 (tick
	// phase 2016, a block boundary) every partial's phase is its note-on phase + 2000·w + ratio·2π·run(1999). The run
	// is indexed from each call's first sample, and each call is told its tick phase, exactly as the synth does.
	Character from = additive({ sound(300, 0.2f, 1.0) });
	Character to = additive({ sound(700, -1.3f, 1.0) });
	from.sounds[0].shapeAmount = 1.0f;
	to.sounds[0].shapeAmount = 1.0f;
	const double key = 48.0, f0 = hzOfKey(key);
	const auto offsetAt = [](int t) { return static_cast<float>(0.0001 * double(t)); };
	auto v = voiceOn(from, key);
	constexpr int kTickStart = 16, flipAt = 8, total = 2000;
	const uint32_t generationBefore = v->bank.generation();
	std::vector<float> out(static_cast<size_t>(total)), run(static_cast<size_t>(kBlock + 64));
	int done = 0;
	bool sawShort = false;
	while (done < total) {
		int n = std::min(kBlock, total - done);
		if (done < flipAt && done + n > flipAt) {
			n = flipAt - done;
		}
		for (size_t j = 0; j < run.size(); ++j) {
			run[j] = offsetAt(done + int(j));
		}
		v->voice.setReadPhase(0.0f, run.data(), int(run.size()));
		v->voice.setTickPhase((kTickStart + done) % 64);
		v->render(out.data() + done, n);
		sawShort = sawShort || (done == 0 && v->voice.partialsBlockLengthForTest() == 16);
		done += n;
		if (done == flipAt) {
			v->setCharacter(to);
		}
	}
	REQUIRE(v->bank.generation() != generationBefore); // the rebuild did land
	CHECK(sawShort);                                   // and the note's first block WAS the short one
	const PartialSpectrum& spec = playing(*v);
	const std::vector<double> phi = schroederRestated(spec, nullptr, f0);
	const double turn = double(offsetAt(total - 1));
	for (int i = 0; i < 60; ++i) {
		INFO("partial " << i);
		REQUIRE(std::abs(spec.ratio[i] - float(i + 1)) < 1e-4f);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
		const double want = phi[size_t(i)] + double(total) * w + kTwoPi * double(spec.ratio[i]) * turn;
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 1e-3);
	}
}

TEST_CASE("additive voice: Additive -> V5 -> Additive on one voice re-primes the bank at its note-on phases") {
	// Leaving the road and coming back is a note-on for the bank: a second reset, its count the partials live at this
	// pitch, and its phases the weighted Schroeder law again, read back after the first block of the return (the mixed
	// road's: the bank renders at full amplitude, the weight is applied outside it).
	const Character add = additive({ sound(300, 0.2f, 1.0) });
	const Character v5 = feathers::test::characterAt(OscEngine::Morph, 512, 0.25f);
	const double key = 52.0, f0 = hzOfKey(key);
	auto v = voiceOn(add, key);
	renderMono(*v, 2048);
	CHECK(v->voice.partialsResetsForTest() == 1);
	v->setCharacter(v5);
	renderMono(*v, 2048);
	CHECK(v->voice.partialsResetsForTest() == 1);
	v->setCharacter(add);
	renderMono(*v, kBlock);
	CHECK(v->voice.partialsResetsForTest() == 2);
	const PartialSpectrum& spec = playing(*v);
	CHECK(v->voice.partialsResetCountForTest() == renderingCount(spec, f0));
	const std::vector<double> phi = schroederRestated(spec, nullptr, f0);
	for (int i = 0; i < v->voice.partialsLiveForTest(); ++i) {
		INFO("partial " << i);
		const double w = v->voice.partialIncrementForTest(i);
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), phi[size_t(i)] + kBlock * w)) < 1e-4);
	}
}

TEST_CASE("additive voice: a stereo Partials -> Partials rebuild walks each ear from its own stale side to its own fresh") {
	// Through setCharacterStereo: ear 1's amplitude at the end of block b is staleR + (freshR − staleR)·t, restated per
	// partial at the fresh LEFT ear's frequencies (the ears share frequencies, design §1.2).
	const SoundCoord fl = sound(300, 0.2f, 1.0), fr = sound(310, 1.9f, 1.0);
	const SoundCoord tl = sound(700, -1.3f, 1.0), tr = sound(705, -3.1f, 1.0);
	const PartialSpectrum aL = built(additive({ fl })), aR = built(additive({ fr }));
	const PartialSpectrum bL = built(additive({ tl })), bR = built(additive({ tr }));
	REQUIRE(maxDiff(aR, aL) > 0.01);
	REQUIRE(maxDiff(bR, bL) > 0.01);
	const double key = 55.0, f0 = hzOfKey(key);
	auto v = std::make_unique<BankedVoice>();
	v->setSampleRate(kSampleRate);
	v->setCharacterStereo(additive({ fl }), additive({ fr }));
	v->reset();
	v->voice.setPitch(key);
	float l[kBlock], r[kBlock];
	for (int b = 0; b < 32; ++b) {
		v->renderStereo(l, r, kBlock);
	}
	v->setCharacterStereo(additive({ tl }), additive({ tr }));
	const int fadeLength = v->bank.fadeLengthSamples();
	for (int blk = 0; blk * kBlock < fadeLength + kBlock; ++blk) {
		v->renderStereo(l, r, kBlock);
		const double t = std::min(1.0, double(kBlock * (blk + 1)) / double(fadeLength));
		INFO("block " << blk << " t " << t);
		for (int i = 0; i < 24; ++i) {
			INFO("partial " << i);
			const double w = static_cast<float>(kTwoPi * f0 * bL.ratio[i] / kSampleRate);
			const auto at = [&](const PartialSpectrum& from, const PartialSpectrum& to) {
				const double x = double(from.envAtRatio[i] * from.fine[i]), y = double(to.envAtRatio[i] * to.fine[i]);
				return (x + (y - x) * t) * fadeRestated(w);
			};
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 0) - at(aL, bL)) < 1e-6);
			CHECK(std::abs(v->voice.partialAmplitudeForTest(i, 1) - at(aR, bR)) < 1e-6);
		}
	}
}

TEST_CASE("additive voice: the spectrum's ratios never decrease, which the voice's partial loop stops on") {
	// The control block stops at the first partial the Nyquist fade silences, which is right only if no later partial
	// sits lower. Swept over homes with the off-grid field on, two- and three-sound blends, and a shape blend.
	int checked = 0;
	for (int step = -8 * 8; step <= 11 * 8; ++step) {
		const float offset = static_cast<float>(step) / 8.0f;
		for (const Character& c : { additive({ sound(0, offset, 1.0) }),
		                            additive({ sound(0, offset, 0.6), sound(311, -offset, 0.4) }),
		                            additive({ sound(0, offset, 0.2), sound(500, 0.5f * offset, 0.5),
		                                       sound(900, 1.0f - offset, 0.3) }) }) {
			const PartialSpectrum s = built(c);
			for (int i = 1; i < kMaxPartials; ++i) {
				if (s.ratio[i] < s.ratio[i - 1]) {
					FAIL_CHECK("ratio falls at partial " << i << " offset " << offset);
				}
			}
			++checked;
		}
	}
	Character shaped = additive({ offGridSound() });
	shaped.sounds[0].shapeAmount = 0.5f;
	const PartialSpectrum s = built(shaped);
	for (int i = 1; i < kMaxPartials; ++i) {
		CHECK(s.ratio[i] >= s.ratio[i - 1]);
	}
	CHECK(checked > 400);
}

TEST_CASE("MEASUREMENT additive voice: loudness and crest factor against pitch, bright and dark (plan Task 9)") {
	// Not a pass/fail on taste: the numbers inform a design decision (compensate per note, or not). The spectrum is
	// normalised over all 320 partials, so a high note, playing only those below Nyquist, is expected to be quieter
	// when the spectrum is bright. Bright and dark are the extremes of the (smoothed) Rolloff field on the reachable
	// window, found by search.
	float bright = 0.0f, dark = 0.0f, lo = 2.0f, hi = -1.0f;
	for (int step = -8 * 64; step <= 11 * 64; ++step) {
		const float offset = static_cast<float>(step) / 64.0f;
		const float r = feathers::partials::smoothed(rawFields(sound(0, offset, 1.0))).rolloff;
		if (r < lo) {
			lo = r;
			bright = offset;
		}
		if (r > hi) {
			hi = r;
			dark = offset;
		}
	}
	MESSAGE("bright home offset " << bright << " rolloff " << lo << "; dark home offset " << dark << " rolloff " << hi);
	struct Spectrum {
		const char* name;
		float offset;
	};
	for (const Spectrum& sp : { Spectrum { "bright", bright }, Spectrum { "dark", dark } }) {
		const char* name = sp.name;
		const float offset = sp.offset;
		for (const double key : { 36.0, 60.0, 84.0 }) {
			auto v = voiceOn(additive({ sound(0, offset, 1.0) }), key);
			const std::vector<float> y = renderMono(*v, 48000 + 4800);
			double sumSq = 0.0, peak = 0.0;
			for (size_t n = 4800; n < y.size(); ++n) {
				sumSq += double(y[n]) * double(y[n]);
				peak = std::max(peak, std::abs(double(y[n])));
			}
			const double rms = std::sqrt(sumSq / double(y.size() - 4800));
			double rss = 0.0;
			for (int i = 0; i < kMaxPartials; ++i) {
				rss += double(v->voice.partialAmplitudeForTest(i, 0)) * double(v->voice.partialAmplitudeForTest(i, 0));
			}
			rss = std::sqrt(rss);
			MESSAGE(std::string(name) << " key " << key << ": partials " << v->voice.partialsResetCountForTest() << ", target RSS "
			             << rss << ", RMS " << rms << " (" << 20.0 * std::log10(rms) << " dBFS), peak " << peak
			             << ", crest " << peak / rms << " (" << 20.0 * std::log10(peak / rms) << " dB)");
			CHECK(std::isfinite(rms));
			CHECK(rms > 0.0);
		}
	}
	// THE COMPARISON ROW: V5 at the default patch (every param at its default), through the same voice, at C4 — the level
	// the engine sits beside, so the additive engine's level can be chosen with data (kTargetRss is a design choice).
	{
		auto v = voiceOn(Patch {}.character(OscType::V5), 60.0);
		const std::vector<float> y = renderMono(*v, 48000 + 4800);
		double sumSq = 0.0, peak = 0.0;
		for (size_t n = 4800; n < y.size(); ++n) {
			sumSq += double(y[n]) * double(y[n]);
			peak = std::max(peak, std::abs(double(y[n])));
		}
		const double rms = std::sqrt(sumSq / double(y.size() - 4800));
		MESSAGE("V5 default patch key 60: RMS " << rms << " (" << 20.0 * std::log10(rms) << " dBFS), peak " << peak << ", crest "
		                                       << peak / rms << " (" << 20.0 * std::log10(peak / rms) << " dB)");
		CHECK(rms > 0.0);
	}
}

// ---- the engine made real to a user (plan Task 9b) -----------------------------------------------------------------
//
// Additive on the real parameter road: chosen by Osc Type, its Shape and per-sound Shape Amount read by the mapping,
// played through the real Synth. Values read through the synth's own seams (the bank's prepared table, the voice's
// rotator bank), the buffer only where the claim is about the buffer (it sounds; it ends at exactly 0.0f).

namespace {

using feathers::ParamSet;
using feathers::Synth;

struct SynthRig {
	ParamSet params;
	Synth synth { params };

	explicit SynthRig(OscType engine) {
		synth.activate(kSampleRate, 4096);
		set(ParamId::OscType, static_cast<double>(engine));
		set(ParamId::Level, 1.0);
		// A bare Synth has no bridge to route the strip envelope into Voice Level (test_synth.cpp Rig says why).
		set(ParamId::VoiceLevel, 1.0);
	}
	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }
	/// `frames` stereo samples in host blocks of `block`.
	void play(uint32_t frames, std::vector<float>& l, std::vector<float>& r, int block = 256) {
		l.assign(frames, 0.0f);
		r.assign(frames, 0.0f);
		for (uint32_t at = 0; at < frames; at += static_cast<uint32_t>(block)) {
			const int n = static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(block), frames - at));
			synth.render(l.data() + at, r.data() + at, n);
		}
	}
	const PreparedCharacter& table(int ear = 0) const { return synth.mainBankForTest().current(ear); }
};

float peakOf(const std::vector<float>& x, size_t from = 0) {
	float p = 0.0f;
	for (size_t i = from; i < x.size(); ++i) {
		p = std::max(p, std::abs(x[i]));
	}
	return p;
}

} // namespace

TEST_CASE("additive synth: Additive chosen by Osc Type builds a Partials character and the note sounds") {
	SynthRig rig(OscType::Additive);
	std::vector<float> l, r;
	rig.play(512, l, r);
	REQUIRE(rig.table().engine == OscEngine::Partials);
	// THE BANK'S TABLE IS THE MAPPING'S CHARACTER BUILT: the same patch through characterFromParams and buildOne.
	Patch patch;
	patch.sets[ParamId::OscType] = static_cast<double>(OscType::Additive);
	const Character expected = patch.character(OscType::Additive);
	CHECK(expected.engine == OscEngine::Partials);
	CHECK(maxDiff(rig.table().partials, built(expected)) == 0.0);
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.play(9600, l, r);
	const OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
	CHECK(osc.partialsChunksForTest() > 0);
	CHECK(osc.partialsLiveForTest() > 0);
	CHECK(std::abs(osc.partialIncrementForTest(0) - kTwoPi * hzOfKey(60.0) * rig.table().partials.ratio[0] / kSampleRate)
	      < 1e-6);
	CHECK(peakOf(l, 4800) > 0.01f);
	CHECK(peakOf(r, 4800) > 0.01f);
}

TEST_CASE("additive synth: the Shape choice and each sound's Shape Amount reach the prepared spectrum through the params") {
	// The bank's own cases (above) pin the blend on a Character; this pins the PARAM ROAD into it: AdditiveShape read
	// by its stepped rule, A Shape Amount at +C, through the real synth's character mapping and bank.
	for (const ShapeKind kind : kShapes) {
		CAPTURE(static_cast<int>(kind));
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::BEnable, 0.0); // one sound, so the blend is that sound
		rig.set(ParamId::AdditiveShape, static_cast<double>(kind));
		rig.set(ParamId::AShapeAmount, 0.5);
		std::vector<float> l, r;
		rig.play(256, l, r);
		Patch patch;
		patch.sets = { { ParamId::BEnable, 0.0 }, { ParamId::AdditiveShape, static_cast<double>(kind) },
			           { ParamId::AShapeAmount, 0.5 } };
		const Character c = patch.character(OscType::Additive);
		REQUIRE(c.count == 1);
		CHECK(c.additiveShape == kind);
		CHECK(c.sounds[0].shapeAmount == 0.5f);
		// THE VALUE, restated as the bank's 0.5 case states it: the member-wise mean of the terrain and the shape,
		// renormalised.
		SoundCoord bare = c.sounds[0];
		bare.shapeAmount = 0.0f;
		const PartialSpectrum terrain = soloSpectrum(c, bare);
		PartialSpectrum shape;
		feathers::partials::buildShapeSpectrum(kind, shape);
		const PartialSpectrum want = rescaledToTarget(weightedSum({ { 0.5, &terrain }, { 0.5, &shape } }));
		CHECK(maxDiff(rig.table().partials, want) < 1e-6);
		// Not vacuous: the shape moved the spectrum away from the terrain's own.
		CHECK(maxDiff(want, rescaledToTarget(terrain)) > 1e-3);
	}
	// AND OFF ADDITIVE THE TWO ARE NOT READ: a V5 character with them set compares equal to one without.
	Patch shaped;
	shaped.sets = { { ParamId::AdditiveShape, 0.0 }, { ParamId::AShapeAmount, 1.0 } };
	CHECK(shaped.character(OscType::V5) == Patch {}.character(OscType::V5));
}

TEST_CASE("additive synth: each bias knob reaches the prepared spectrum through its own param, and off Additive none is read") {
	// THE PARAM ROAD, per knob: A's knob at 1 through the real synth's mapping and bank. The value is restated by hand —
	// the raw fields and detail at sound A's coordinate with that knob's field(s) set to 1, through buildSound — so a
	// param read onto the wrong member, or a member applied to the wrong field, misses it.
	for (const BiasKnob& b : biasKnobs()) {
		INFO(b.name);
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::BEnable, 0.0); // one sound, so the blend is that sound
		rig.set(b.param(0), 1.0);
		std::vector<float> l, r;
		rig.play(256, l, r);
		Patch patch;
		patch.sets = { { ParamId::BEnable, 0.0 }, { b.param(0), 1.0 } };
		const Character c = patch.character(OscType::Additive);
		REQUIRE(c.count == 1);
		feathers::PartialBias wantBias;
		wantBias.*(b.knob) = 1.0f;
		CHECK(c.sounds[0].partialBias == wantBias);
		const SoundCoord& s = c.sounds[0];
		REQUIRE(s.travelX == 0.0); // at zero travel the frame's orientation does not matter (phaseFor's guarantee)
		REQUIRE(s.travelY == 0.0);
		const PartialFields raw = rawFields(s);
		PartialSpectrum want;
		feathers::partials::buildSound(forced(raw, b, 1.0f), rawDetail(s), want);
		CHECK(maxDiff(rig.table().partials, rescaledToTarget(want)) < 1e-5);
		// Not vacuous: at this coordinate the knob moved the spectrum away from the terrain's own.
		PartialSpectrum terrain;
		feathers::partials::buildSound(raw, rawDetail(s), terrain);
		CHECK(maxDiff(want, terrain) > 1e-3);
	}
	// AND OFF ADDITIVE NONE IS READ: a V5 character with all six turned compares equal to one without.
	Patch biased;
	for (const BiasKnob& b : biasKnobs()) {
		biased.sets[b.param(0)] = 0.0;
	}
	CHECK(biased.character(OscType::V5) == Patch {}.character(OscType::V5));
}

TEST_CASE("additive synth: Width on Additive builds two different ears, each its own character's spectrum, and the voice plays them") {
	// SYNTH'S TERRAIN-STEREO PREDICATE INCLUDES ADDITIVE (design §1.4): with the global Width up and two enabled sounds
	// wide, the bank builds each ear from its own character, and the voice renders the two as amplitudes over shared
	// phases.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Width, 1.0);
	rig.set(ParamId::AWidth, 1.0);
	rig.set(ParamId::BWidth, 1.0);
	std::vector<float> l, r;
	rig.play(256, l, r);
	REQUIRE(rig.synth.mainBankForTest().stereoActive());
	Patch patch;
	patch.sets = { { ParamId::Width, 1.0 }, { ParamId::AWidth, 1.0 }, { ParamId::BWidth, 1.0 } };
	const PartialSpectrum left = built(patch.character(OscType::Additive, feathers::EarSide::Left));
	const PartialSpectrum right = built(patch.character(OscType::Additive, feathers::EarSide::Right));
	CHECK(maxDiff(rig.table(0).partials, left) == 0.0);
	CHECK(maxDiff(rig.table(1).partials, right) == 0.0);
	CHECK(maxDiff(left, right) > 1e-3); // two ears, not one twice
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.play(4800, l, r);
	const OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
	CHECK(osc.stereoStreamForTest());
	// The voice's ears are the two spectra: partial i's amplitude per ear is its own ear's restated neutral one.
	const double hz = hzOfKey(60.0);
	int differ = 0;
	for (int i = 0; i < 16; ++i) {
		const double w = static_cast<float>(kTwoPi * hz * left.ratio[i] / kSampleRate);
		CHECK(osc.partialAmplitudeForTest(i, 0) == doctest::Approx(neutralAmplitude(left, i, w)).epsilon(1e-5));
		CHECK(osc.partialAmplitudeForTest(i, 1) == doctest::Approx(neutralAmplitude(right, i, w)).epsilon(1e-5));
		differ += std::abs(osc.partialAmplitudeForTest(i, 0) - osc.partialAmplitudeForTest(i, 1)) > 1e-4f ? 1 : 0;
	}
	CHECK(differ > 0);
	CHECK(l != r);
}

namespace {

/// Side over mid, in dB, of two ears' per-partial amplitudes over SHARED phases: Σ(aL − aR)² / Σ(aL + aR)². With one
/// rotator per partial for both ears this IS the rendered image's side/mid (the partials are orthogonal), which is why
/// the stereo cases below can measure a width without a render, and why the render case checks the two agree.
double sideOverMidDb(const std::vector<double>& l, const std::vector<double>& r) {
	double side = 0.0, mid = 0.0;
	for (size_t i = 0; i < l.size(); ++i) {
		side += (l[i] - r[i]) * (l[i] - r[i]);
		mid += (l[i] + r[i]) * (l[i] + r[i]);
	}
	return 10.0 * std::log10(side / mid);
}

/// Each partial's neutral amplitude at key 48 per ear (neutralAmplitude, the left ear's increments for both, as the voice).
void earAmplitudes(const PartialSpectrum& l, const PartialSpectrum& r, std::vector<double>& al, std::vector<double>& ar) {
	al.clear();
	ar.clear();
	const double hz = hzOfKey(48.0);
	for (int i = 0; i < kMaxPartials; ++i) {
		const double w = static_cast<float>(kTwoPi * hz * l.ratio[i] / kSampleRate); // the bank's float increment
		al.push_back(neutralAmplitude(l, i, w));
		ar.push_back(neutralAmplitude(r, i, w));
	}
}

double earsSideOverMidDb(const Patch& patch) {
	std::vector<double> al, ar;
	earAmplitudes(built(patch.character(OscType::Additive, feathers::EarSide::Left)),
	              built(patch.character(OscType::Additive, feathers::EarSide::Right)), al, ar);
	return sideOverMidDb(al, ar);
}

/// THE MEASURED WIDTH AT THE DEFAULT PUCK, Weather 0, both default sounds at Width 1, with kPartialsEarTravel = 0.65:
/// −11.6 dB (−30.2 dB before the ears read the fields at their own stands). The bound sits 1.4 dB under it, and 17 dB
/// above the shares-only figure, so a lost field offset cannot pass it.
constexpr double kDefaultEarsSideOverMidFloorDb = -13.0;

} // namespace

TEST_CASE("additive terrain stereo: each ear reads the fields at its own stand, the travel moved by max(kPartialsEarTravel, Weather) times its anchor shift, on Additive alone") {
	// THE EAR'S FIELD READ (the stereo diagnosis's option (b)). Restated from the mapping's own
	// definitions: the Head character has no width at all, and an ear's sound k stands at the head's anchor shifted
	// by s·(global·w_k)/2 (s = +1 left, −1 right), so its travel is the head's MINUS Weather's share of that shift
	// (Weather reads puck − anchor) — on every engine — and on Additive alone, minus the top-up to kPartialsEarTravel
	// as well. So every other engine keeps exactly the weather term it had, and Additive's ear stands
	// max(K, weather)·shift away at any Weather, 0 included. Two different per-sound widths, so a shift read off the
	// wrong sound shows.
	for (const double weatherKnob : { 0.0, 0.5, 0.9 }) {
		const double weather = feathers::amountTaper(weatherKnob);
		for (int t = 0; t < static_cast<int>(OscType::kCount); ++t) {
			const auto type = static_cast<OscType>(t);
			CAPTURE(weatherKnob);
			CAPTURE(t);
			Patch patch;
			patch.sets = { { ParamId::Width, 1.0 }, { ParamId::AWidth, 1.0 }, { ParamId::BWidth, 0.5 },
			               { ParamId::Weather, weatherKnob },
			               // THE DRUM ENGINE IN PITCHED MODE: its Kit mode is the one exception to "every engine" here by design (every
			               // sound a pad at its home plus the Walk; test_drum_engine pins that), so the claim is made of Pitched.
			               { ParamId::DrumMode, 0.0 } };
			const Character head = patch.character(type);
			REQUIRE(head.count == 2);
			const double width[2] = { 1.0, 0.5 };
			const double reach = type == OscType::Additive ? std::max(feathers::kPartialsEarTravel, weather) : weather;
			for (const auto& [ear, side] : { std::pair { feathers::EarSide::Left, 1.0 }, std::pair { feathers::EarSide::Right, -1.0 } }) {
				const Character c = patch.character(type, ear);
				for (size_t k = 0; k < 2; ++k) {
					const double shift = side * 0.5 * 1.0 * width[k];
					CHECK(std::abs(c.sounds[k].travelX - (head.sounds[k].travelX - reach * shift)) < 1e-12);
					CHECK(c.sounds[k].travelY == head.sounds[k].travelY);
				}
			}
		}
	}
}

TEST_CASE("additive terrain stereo: at Width 0 the two ears are one spectrum, to the bit, and the bank stays mono") {
	// The shift is exactly 0 there, so the field offset is too: either factor of the algebra at zero.
	for (const auto& sets : { std::map<ParamId, double> { { ParamId::Width, 1.0 } },
	                          std::map<ParamId, double> { { ParamId::Width, 0.0 }, { ParamId::AWidth, 1.0 }, { ParamId::BWidth, 1.0 } } }) {
		Patch patch;
		patch.sets = sets;
		patch.sets[ParamId::Weather] = 0.3;
		const Character head = patch.character(OscType::Additive);
		const Character l = patch.character(OscType::Additive, feathers::EarSide::Left);
		const Character r = patch.character(OscType::Additive, feathers::EarSide::Right);
		for (size_t k = 0; k < head.count; ++k) {
			CHECK(l.sounds[k].travelX == head.sounds[k].travelX);
			CHECK(r.sounds[k].travelX == head.sounds[k].travelX);
		}
		CHECK(built(l) == built(r));
		SynthRig rig(OscType::Additive);
		for (const auto& [id, v] : patch.sets) {
			rig.set(id, v);
		}
		std::vector<float> outL, outR;
		rig.synth.noteOn(0, 48, 1, 1.0);
		rig.play(2048, outL, outR);
		CHECK_FALSE(rig.synth.mainBankForTest().stereoActive());
		CHECK(outL == outR);
	}
}

TEST_CASE("additive synth: Width on the default patch plays each ear's spectrum read at its own stand, restated, and the ears stand at least the stated width apart") {
	// THE VALUE, restated independently of the mapping's ear code: each sound at the HEAD's coordinate with its travel
	// moved by kPartialsEarTravel times the ear's shift (Weather 0, so that is the whole of it), through soloSpectrum;
	// blended by the ear's shares, which are geometry and the same on every engine, so they are read off V5's ear
	// characters, which have no field offset; rescaled. The voice's per-ear partial amplitudes are that, at its fades.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::AWidth, 1.0);
	rig.set(ParamId::BWidth, 1.0);
	// THE DEFAULTS THIS CASE IS ABOUT: the global Width at full and Weather at 0, so the field offset is the whole of it.
	REQUIRE(rig.params.base(ParamId::Width) == 1.0);
	REQUIRE(rig.params.base(ParamId::Weather) == 0.0);
	std::vector<float> l, r;
	rig.synth.noteOn(0, 48, 1, 1.0);
	rig.play(9600, l, r);
	REQUIRE(rig.synth.mainBankForTest().stereoActive());
	Patch patch;
	patch.sets = { { ParamId::AWidth, 1.0 }, { ParamId::BWidth, 1.0 } };
	const Character head = patch.character(OscType::Additive);
	REQUIRE(head.count == 2);
	PartialSpectrum want[2];
	for (const auto& [ear, side, index] : { std::tuple { feathers::EarSide::Left, 1.0, 0 }, std::tuple { feathers::EarSide::Right, -1.0, 1 } }) {
		const Character shares = patch.character(OscType::V5, ear);
		PartialSpectrum solo[2];
		for (size_t k = 0; k < 2; ++k) {
			SoundCoord s = head.sounds[k];
			s.travelX = head.sounds[k].travelX - feathers::kPartialsEarTravel * side * 0.5;
			solo[k] = soloSpectrum(head, s);
		}
		want[index] = rescaledToTarget(weightedSum({ { shares.sounds[0].weight, &solo[0] }, { shares.sounds[1].weight, &solo[1] } }));
		CHECK(maxDiff(rig.table(index).partials, want[index]) < 1e-6);
	}
	const OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
	REQUIRE(osc.stereoStreamForTest());
	const double hz = hzOfKey(48.0);
	std::vector<double> al, ar;
	for (int i = 0; i < osc.partialsLiveForTest(); ++i) {
		const double w = static_cast<float>(kTwoPi * hz * want[0].ratio[i] / kSampleRate);
		CHECK(std::abs(osc.partialAmplitudeForTest(i, 0) - neutralAmplitude(want[0], i, w)) < 2e-6); // absolute: Approx adds a scale of 1.0
		CHECK(std::abs(osc.partialAmplitudeForTest(i, 1) - neutralAmplitude(want[1], i, w)) < 2e-6);
		al.push_back(osc.partialAmplitudeForTest(i, 0));
		ar.push_back(osc.partialAmplitudeForTest(i, 1));
	}
	REQUIRE(al.size() > 16);
	// THE WIDTH, from the voice's own amplitudes.
	const double voiceDb = sideOverMidDb(al, ar);
	CHECK(voiceDb >= kDefaultEarsSideOverMidFloorDb);
	// AND THE MONO SUM IS THE AMPLITUDES' — one rotator per partial for both ears (design §1.4): the render's own
	// side/mid is the amplitudes' over shared phases. Decorrelated phases would put the render's side well above it
	// (V5's own tables, whose harmonics differ in phase, measured −0.7 dB rendered against −7.9 amplitude-only).
	double side = 0.0, mid = 0.0;
	for (size_t i = 4800; i < l.size(); ++i) {
		side += (double(l[i]) - r[i]) * (double(l[i]) - r[i]);
		mid += (double(l[i]) + r[i]) * (double(l[i]) + r[i]);
	}
	CHECK(std::abs(10.0 * std::log10(side / mid) - voiceDb) < 0.5);
}

TEST_CASE("additive terrain stereo: a Width sweep on the default patch widens the ears monotonically, to the stated width at full") {
	// THE TURN-BACK (kPartialsEarTravel's comment): past about 0.7 the anchor shift's A/B shares, which ride on the same
	// knob as the field offset, work against it and turning Width up NARROWS the image. At the shipped value every step widens it.
	double previous = -std::numeric_limits<double>::infinity();
	for (int step = 1; step <= 20; ++step) {
		const double w = step / 20.0;
		CAPTURE(w);
		Patch patch;
		patch.sets = { { ParamId::AWidth, w }, { ParamId::BWidth, w } };
		const double db = earsSideOverMidDb(patch);
		CHECK(db >= previous);
		previous = db;
	}
	CHECK(previous >= kDefaultEarsSideOverMidFloorDb);
	MESSAGE("side/mid at full Width: " << previous << " dB (kPartialsEarTravel " << feathers::kPartialsEarTravel << ")");
}

TEST_CASE("additive synth: a note released to the end of its edge leaves the synth's output exactly 0.0f") {
	// THE SLEEP RULE's premise for this engine (design §5.4): the partial bank reaches EXACT zero, never a denormal, so a
	// silent block is silent to the bit. Both ears, mono and terrain stereo.
	for (const bool wide : { false, true }) {
		CAPTURE(wide);
		SynthRig rig(OscType::Additive);
		if (wide) {
			rig.set(ParamId::AWidth, 1.0);
			rig.set(ParamId::BWidth, 1.0);
		}
		std::vector<float> l, r;
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.play(4800, l, r);
		REQUIRE(peakOf(l) > 0.01f);
		CHECK(rig.synth.mainBankForTest().stereoActive() == wide);
		rig.synth.noteOff(0, 60, 1, 0.0);
		rig.play(4800, l, r); // the 2 ms edge, many times over
		CHECK(rig.synth.soundingVoiceCount() == 0);
		rig.play(1024, l, r);
		CHECK(std::all_of(l.begin(), l.end(), [](float x) { return x == 0.0f; }));
		CHECK(std::all_of(r.begin(), r.end(), [](float x) { return x == 0.0f; }));
	}
}

TEST_CASE("additive voice: the playing cycle the stack's coherence reads is the spectrum summed at flat Newman phases, not silence") {
	// THE STACK'S LIKENESS READING (Synth::updateStackCoherence) reads each part's playing cycle. A Partials side has no
	// table, so it answers with its spectrum as one cycle: the partials that render at the voice's pitch (capped at what
	// 256 points resolve), each at its Nyquist fade, at Newman's flat phases for the FIXED N kPartialsCycleCap. Restated here
	// with a sine per partial.
	const SoundCoord s = offGridSound();
	for (const double key : { 36.0, 72.0, 96.0 }) {
		CAPTURE(key);
		auto v = voiceOn(additive({ s }), key);
		renderMono(*v, 256);
		const PartialSpectrum& spec = playing(*v);
		const double hz = hzOfKey(key);
		const int count = std::min(renderingCount(spec, hz), OscVoice::kPartialsCycleCap);
		std::array<float, 256> got {};
		v->voice.playingCycle(got.data(), 256);
		double peak = 0.0, worst = 0.0;
		for (int j = 0; j < 256; ++j) {
			double want = 0.0;
			for (int k = 0; k < count; ++k) {
				const double w = static_cast<float>(kTwoPi * hz * spec.ratio[k] / kSampleRate);
				const double a = double(spec.envAtRatio[k]) * double(spec.fine[k]) * fadeRestated(w);
				want += a * std::sin(kTwoPi * spec.ratio[k] * j / 256.0
				                     + std::numbers::pi * k * k / double(OscVoice::kPartialsCycleCap));
			}
			peak = std::max(peak, std::abs(want));
			worst = std::max(worst, std::abs(want - double(got[size_t(j)])));
			// And the per-sample road says the same.
			CHECK(std::abs(double(v->voice.playingCycleSample(j / 256.0)) - want) < 1e-5);
		}
		CAPTURE(peak);
		CAPTURE(worst);
		CHECK(peak > 0.05); // not silence
		CHECK(worst < 1e-5);
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// THE SPECTRAL DRAWER, PER SOUND, RESOLVED PER VOICE AND BLENDED BY WEIGHT (by design, per-sound drawers
// MORPH). Every claim is a VALUE: the drawer the synth handed the voice, restated from the params and the weights
// of the character the voice plays, and the voice's per-partial amplitudes restated from that drawer.
namespace {

using feathers::SpectralControl;

/// One note on an Additive rig, settled: 256 samples to adopt, the note, `frames` more (a multiple of the control block,
/// so every partial stands on its target).
void playNote(SynthRig& rig, double key = 48.0, uint32_t frames = 1024) {
	std::vector<float> l, r;
	rig.play(256, l, r);
	rig.synth.noteOn(0, static_cast<int16_t>(key), 1, 1.0);
	rig.play(frames, l, r);
}

const OscVoice& newest(const SynthRig& rig) {
	return rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
}

std::vector<float> amplitudes(const OscVoice& osc, int ear = 0) {
	std::vector<float> a(static_cast<size_t>(kMaxPartials));
	for (int i = 0; i < kMaxPartials; ++i) {
		a[static_cast<size_t>(i)] = osc.partialAmplitudeForTest(i, ear);
	}
	return a;
}

ParamId dp(size_t slot, SpectralControl c) {
	return feathers::spectralParam(slot, c);
}

/// One sound in the blend: the default patch's B parked, so A is the whole blend and its drawer is the voice's to the bit.
void soloA(SynthRig& rig) {
	rig.set(ParamId::BEnable, 0.0);
}

/// EVERY OPERATOR ON in `slot`, each at a setting that acts (so a control that does nothing cannot hide behind a neutral
/// one), Link OFF so the high shift and the crossover are live.
void activeDrawer(SynthRig& rig, size_t slot = 0) {
	rig.set(dp(slot, SpectralControl::FilterOn), 1.0);
	rig.set(dp(slot, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::LowPass));
	rig.set(dp(slot, SpectralControl::FilterCutoff), std::log2(1500.0));
	rig.set(dp(slot, SpectralControl::FilterResonance), 0.5);
	rig.set(dp(slot, SpectralControl::FilterSlope), 0.5);
	rig.set(dp(slot, SpectralControl::FilterKeyTrack), 0.5);
	rig.set(dp(slot, SpectralControl::CombOn), 1.0);
	rig.set(dp(slot, SpectralControl::CombSpacing), 3.0);
	rig.set(dp(slot, SpectralControl::CombDepth), 0.6);
	rig.set(dp(slot, SpectralControl::CombPhase), 0.15);
	rig.set(dp(slot, SpectralControl::FormantShiftOn), 1.0);
	rig.set(dp(slot, SpectralControl::FormantShiftLow), 5.0);
	rig.set(dp(slot, SpectralControl::FormantShiftHigh), -7.0);
	rig.set(dp(slot, SpectralControl::FormantShiftCrossover), std::log2(1200.0));
	rig.set(dp(slot, SpectralControl::FormantShiftLink), 0.0);
	rig.set(dp(slot, SpectralControl::TiltOn), 1.0);
	rig.set(dp(slot, SpectralControl::TiltAmount), -6.0);
	rig.set(dp(slot, SpectralControl::OddEvenOn), 1.0);
	rig.set(dp(slot, SpectralControl::OddEvenBalance), -0.4);
	rig.set(dp(slot, SpectralControl::FormantOn), 1.0);
	rig.set(dp(slot, SpectralControl::FormantVowel), 1.0); // Hum
	rig.set(dp(slot, SpectralControl::FormantDepth), 0.6);
	rig.set(dp(slot, SpectralControl::PartialsOn), 1.0);
	rig.set(dp(slot, SpectralControl::PartialsCount), 64.0);
	rig.set(dp(slot, SpectralControl::DampOn), 1.0);
	rig.set(dp(slot, SpectralControl::DampTime), std::log2(1.0));
	rig.set(dp(slot, SpectralControl::DampTilt), 1.0);
	rig.set(dp(slot, SpectralControl::StretchOn), 1.0);
	rig.set(dp(slot, SpectralControl::StretchAmount), 0.3);
	rig.set(dp(slot, SpectralControl::ShiftOn), 1.0);
	rig.set(dp(slot, SpectralControl::ShiftHz), 50.0);
}

/// Each continuous control, its operator's switch, a value away from activeDrawer's, and where in a ShaperParams it must
/// land (as the engine reads it: Hz controls through exp2).
struct DrawerControl {
	SpectralControl id;
	double value;
	float (*read)(const sp::ShaperParams&);
	bool hz;
	bool vowel = false; ///< the vowel's position, read as the F1 its walk lands on (vowelFormants)
	bool rate = false;  ///< Damp's Time, read as the rate it lands as: 1 / 2^value (1 / T60, per second)
	bool stretch = false; ///< Stretch's Amount, read as the B it lands as (StretchOp::bOf)
};
const std::vector<DrawerControl>& drawerControls() {
	static const std::vector<DrawerControl> list {
		{ SpectralControl::FilterCutoff, std::log2(700.0), [](const sp::ShaperParams& p) { return p.filter.cutoffHz; }, true },
		{ SpectralControl::FilterResonance, 0.9, [](const sp::ShaperParams& p) { return p.filter.resonance; }, false },
		{ SpectralControl::FilterSlope, 0.85, [](const sp::ShaperParams& p) { return p.filter.slope; }, false },
		{ SpectralControl::FilterKeyTrack, 1.0, [](const sp::ShaperParams& p) { return p.filter.keyTrack; }, false },
		{ SpectralControl::CombSpacing, 5.5, [](const sp::ShaperParams& p) { return p.comb.spacing; }, false },
		{ SpectralControl::CombDepth, 0.95, [](const sp::ShaperParams& p) { return p.comb.depth; }, false },
		{ SpectralControl::CombPhase, 0.55, [](const sp::ShaperParams& p) { return p.comb.phase; }, false },
		{ SpectralControl::FormantShiftLow, -9.0, [](const sp::ShaperParams& p) { return p.formantShift.lowSemitones; }, false },
		{ SpectralControl::FormantShiftHigh, 11.0, [](const sp::ShaperParams& p) { return p.formantShift.highSemitones; }, false },
		{ SpectralControl::FormantShiftCrossover, std::log2(400.0),
		  [](const sp::ShaperParams& p) { return p.formantShift.crossoverHz; }, true },
		{ SpectralControl::TiltAmount, 7.5, [](const sp::ShaperParams& p) { return p.tilt.dbPerOctave; }, false },
		{ SpectralControl::OddEvenBalance, 0.7, [](const sp::ShaperParams& p) { return p.oddEven.balance; }, false },
		{ SpectralControl::FormantVowel, 4.5, [](const sp::ShaperParams& p) { return p.vowel.f1Hz; }, false, true },
		{ SpectralControl::FormantDepth, 0.95, [](const sp::ShaperParams& p) { return p.vowel.depth; }, false },
		{ SpectralControl::PartialsCount, 10.5, [](const sp::ShaperParams& p) { return p.partialCount.count; }, false },
		{ SpectralControl::DampTime, std::log2(0.1), [](const sp::ShaperParams& p) { return p.damp.rate; }, false, false, true },
		{ SpectralControl::DampTilt, 2.5, [](const sp::ShaperParams& p) { return p.damp.tilt; }, false },
		{ SpectralControl::StretchAmount, 0.6, [](const sp::ShaperParams& p) { return p.stretch.b; }, false, false, false, true },
		{ SpectralControl::ShiftHz, -120.0, [](const sp::ShaperParams& p) { return p.shift.hz; }, false },
	};
	return list;
}

float expected(const DrawerControl& c, double v) {
	return c.stretch ? static_cast<float>(sp::StretchOp::bOf(v))
	       : c.rate  ? static_cast<float>(std::exp2(-v))
	       : c.vowel ? static_cast<float>(feathers::vowelFormants(v).f1Hz)
	       : c.hz ? static_cast<float>(std::exp2(v)) : static_cast<float>(v);
}

/// A voice's per-partial amplitudes through a drawer whose ONLY live operator is its Filter group (filterCombRestated's
/// arithmetic, each filter to its mix): the single-operator references ignore `on`, and a drawer from the params carries
/// the comb's default depth while the comb is off.
std::vector<double> filterRestated(const PartialSpectrum& s, const sp::ShaperParams& p, double f0) {
	REQUIRE_FALSE(p.comb.on);
	REQUIRE_FALSE(p.tilt.on);
	REQUIRE_FALSE(p.oddEven.on);
	REQUIRE_FALSE(p.formantShift.on);
	REQUIRE_FALSE(p.curve.on);
	std::vector<double> amp(static_cast<size_t>(kMaxPartials), 0.0);
	for (int i = 0; i < kMaxPartials; ++i) {
		const double hz = static_cast<float>(f0 * s.ratio[i]);
		const double w = static_cast<float>(kTwoPi * f0 * s.ratio[i] / kSampleRate);
		double g = 1.0;
		for (const sp::FilterOp* f : { &p.filter, &p.moreFilters[0], &p.moreFilters[1], &p.moreFilters[2] }) {
			if (f->on) {
				g *= std::pow(double(sp::filterGain(*f, hz, f0)), std::clamp(double(f->mix), 0.0, 1.0));
			}
		}
		const double a = double(s.envAtRatio[i] * s.fine[i]) * fadeRestated(w) * (g >= double(sp::kFlushGain) ? g : 0.0);
		amp[size_t(i)] = std::abs(a) < pb::kSilenceFloor ? 0.0 : a;
	}
	return amp;
}

void checkAmplitudes(const OscVoice& osc, const std::vector<double>& want, int ear = 0, int minSounding = 4) {
	int sounding = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		INFO("partial " << i);
		CHECK(std::abs(osc.partialAmplitudeForTest(i, ear) - want[size_t(i)]) <= 1e-5 * std::abs(want[size_t(i)]) + 1e-9);
		sounding += want[size_t(i)] > 1e-6 ? 1 : 0;
	}
	CHECK(sounding > minSounding);
}

int differs(const std::vector<float>& x, const std::vector<float>& y) {
	int n = 0;
	for (size_t i = 0; i < x.size(); ++i) {
		n += std::abs(x[i] - y[i]) > 1e-6f * std::max(std::abs(x[i]), 1e-3f) ? 1 : 0;
	}
	return n;
}

} // namespace

TEST_CASE("additive drawer: each sound's param reaches the voice's drawer through its own param, and no other field") {
	// THE PARAM ROAD, per control and per sound: set through ParamSet, read back from what the synth HANDED THE VOICE. One
	// sound in the blend (the other parked), so the drawer is that sound's to the bit. Every other control is checked at the
	// value activeDrawer gave it, so a param read onto the wrong field fails twice: its own field stays put, another moves.
	for (const size_t slot : { size_t { 0 }, size_t { 1 } }) {
		CAPTURE(slot);
		const auto solo = [slot](SynthRig& rig) { rig.set(slot == 0 ? ParamId::BEnable : ParamId::AEnable, 0.0); };
		SynthRig ref(OscType::Additive);
		solo(ref);
		activeDrawer(ref, slot);
		playNote(ref);
		const sp::ShaperParams base = newest(ref).drawerForTest(0);
		for (const DrawerControl& c : drawerControls()) {
			CHECK(c.read(base) == expected(c, ref.params.base(static_cast<clap_id>(dp(slot, c.id)))));
		}
		CHECK(base.enabled);
		CHECK(base.filter.on);
		CHECK(base.filter.mix == 1.0f);
		CHECK(base.filter.mode == sp::FilterMode::LowPass);
		CHECK_FALSE(base.formantShift.link);
		CHECK(base.comb.on);
		CHECK(base.formantShift.on);
		CHECK(base.tilt.on);
		CHECK(base.oddEven.on);
		CHECK(base.vowel.on);
		CHECK(base.partialCount.on);
		CHECK(base.damp.on);
		CHECK(base.stretch.on);
		CHECK(base.shift.on);
		for (const DrawerControl& c : drawerControls()) {
			INFO(feathers::findParam(static_cast<clap_id>(dp(slot, c.id)))->name);
			SynthRig rig(OscType::Additive);
			solo(rig);
			activeDrawer(rig, slot);
			rig.set(dp(slot, c.id), c.value);
			playNote(rig);
			const sp::ShaperParams got = newest(rig).drawerForTest(0);
			CHECK(c.read(got) == expected(c, c.value));
			CHECK(c.read(got) != c.read(base)); // not vacuous: the value moved
			for (const DrawerControl& other : drawerControls()) {
				if (other.id != c.id) {
					CHECK(other.read(got) == other.read(base));
				}
			}
		}
		// THE STEPPED ONES, by their stepped rule: each mode, Link on, and each switch off.
		for (size_t m = 0; m < feathers::kSpectralFilterModeNames.size(); ++m) {
			SynthRig rig(OscType::Additive);
			solo(rig);
			activeDrawer(rig, slot);
			rig.set(dp(slot, SpectralControl::FilterMode), static_cast<double>(m));
			rig.set(dp(slot, SpectralControl::FilterGain), 6.0); // a Bell at 0 dB is no filter (F2)
			playNote(rig);
			CHECK(newest(rig).drawerForTest(0).filter.mode == static_cast<sp::FilterMode>(m));
		}
		{
			SynthRig rig(OscType::Additive);
			solo(rig);
			activeDrawer(rig, slot);
			rig.set(dp(slot, SpectralControl::FormantShiftLink), 1.0);
			playNote(rig);
			CHECK(newest(rig).drawerForTest(0).formantShift.link);
		}
		for (const SpectralControl sw : feathers::kSpectralSwitches) {
			SynthRig rig(OscType::Additive);
			solo(rig);
			activeDrawer(rig, slot);
			rig.set(dp(slot, sw), 0.0);
			playNote(rig);
			const sp::ShaperParams got = newest(rig).drawerForTest(0);
			CHECK(got.filter.on == (sw != SpectralControl::FilterOn));
			CHECK(got.comb.on == (sw != SpectralControl::CombOn));
			CHECK(got.formantShift.on == (sw != SpectralControl::FormantShiftOn));
			CHECK(got.tilt.on == (sw != SpectralControl::TiltOn));
			CHECK(got.oddEven.on == (sw != SpectralControl::OddEvenOn));
			CHECK(got.vowel.on == (sw != SpectralControl::FormantOn));
			CHECK(got.partialCount.on == (sw != SpectralControl::PartialsOn));
			CHECK(got.damp.on == (sw != SpectralControl::DampOn));
			CHECK(got.stretch.on == (sw != SpectralControl::StretchOn));
			CHECK(got.shift.on == (sw != SpectralControl::ShiftOn));
		}
	}
}

TEST_CASE("additive drawer: a routed envelope moves one voice's cutoff and not another's, at the control tick's cadence") {
	// PLAN TASK 10 STEP 1, restated with filterGain. A DAHDSR in the strip (a per-voice source) routed into sound A's
	// Cutoff: two notes two seconds apart sit at different places on an eight-second attack, so each voice's resolved
	// cutoff is its own — and each voice's partials are its own drawer's filter, restated per voice.
	namespace dsr = feathers::mod::dahdsr;
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(800.0));
	rig.set(dp(0, SpectralControl::FilterSlope), 0.6);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	{
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
		REQUIRE(desc != nullptr);
		rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
		rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = 3.0f; // log2 seconds: eight seconds
		rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
	}
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	std::vector<float> l, r;
	rig.play(256, l, r);
	const int cutoff = feathers::modDestinationIndexOf(dp(0, SpectralControl::FilterCutoff));
	REQUIRE(cutoff >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::core(static_cast<size_t>(cutoff)), 0.4f);
	routes.publish(rig.params);
	rig.play(256, l, r);
	REQUIRE(rig.params.variesPerVoice(dp(0, SpectralControl::FilterCutoff)));

	rig.synth.noteOn(0, 48, 1, 1.0);
	const int slotA = rig.synth.newestVoiceSlotForTest();
	rig.play(96000, l, r); // a quarter of the attack
	rig.synth.noteOn(0, 55, 2, 1.0);
	const int slotB = rig.synth.newestVoiceSlotForTest();
	REQUIRE(slotA != slotB);
	rig.play(4800, l, r, 32);
	const OscVoice& a = rig.synth.voiceStackOscForTest(slotA, 0);
	const OscVoice& b = rig.synth.voiceStackOscForTest(slotB, 0);
	const sp::ShaperParams da = a.drawerForTest(0);
	const sp::ShaperParams db = b.drawerForTest(0);
	// EACH VOICE'S CUTOFF IS ITS OWN RESOLVED ONE. The render ends on a tick, and the envelopes tick at its end, so what
	// `resolved` reads now is one tick (64 samples) past what the voice was handed: on this attack ~0.05%, inside 1e-3.
	const auto resolvedHz = [&](int slot) {
		return std::exp2(rig.params.resolved(static_cast<clap_id>(dp(0, SpectralControl::FilterCutoff)), slot));
	};
	CHECK(static_cast<double>(da.filter.cutoffHz) == doctest::Approx(resolvedHz(slotA)).epsilon(1e-3));
	CHECK(static_cast<double>(db.filter.cutoffHz) == doctest::Approx(resolvedHz(slotB)).epsilon(1e-3));
	// APART: A is further up its attack, so its cutoff is higher, and by more than a rounding.
	CHECK(da.filter.cutoffHz > 1.5f * db.filter.cutoffHz);
	// AND HEARD: each voice's partials are its spectrum through ITS drawer's filter.
	const PartialSpectrum& spec = rig.table().partials;
	checkAmplitudes(a, filterRestated(spec, da, hzOfKey(48.0)));
	checkAmplitudes(b, filterRestated(spec, db, hzOfKey(55.0)));

	SUBCASE("the drawer is handed over once a control tick (64 samples), and a source moving inside a tick waits for the next") {
		// RENDERED 16 SAMPLES AT A TIME across four ticks: the envelope moves every tick (tickEnvelopes), and the voice's
		// handed cutoff changes exactly when a tick begins — never inside one, never two blocks running.
		const uint32_t seatings = rig.synth.drawerSeatingsForTest();
		std::vector<float> cut;
		for (int k = 0; k < 16; ++k) {
			rig.play(16, l, r, 16);
			cut.push_back(rig.synth.voiceStackOscForTest(slotA, 0).drawerForTest(0).filter.cutoffHz);
		}
		int changes = 0;
		for (size_t k = 1; k < cut.size(); ++k) {
			changes += cut[k] != cut[k - 1] ? 1 : 0;
		}
		// Four ticks' worth of 16-sample render calls: at most one change per tick plus the render-call top's re-reading,
		// which reads the same cells inside a tick (they move only at its boundary).
		CHECK(changes >= 3);
		CHECK(changes <= 4);
		for (size_t k = 0; k + 3 < cut.size(); k += 4) {
			// Within one tick's four render calls, at most one change.
			int inTick = 0;
			for (size_t j = k + 1; j < k + 4; ++j) {
				inTick += cut[j] != cut[j - 1] ? 1 : 0;
			}
			CHECK(inTick <= 1);
		}
		CHECK(rig.synth.drawerSeatingsForTest() > seatings);
	}
}

TEST_CASE("additive drawer: every drawer param changes the targets when its operator is on, and nothing when it is off") {
	// THE HONESTY SWEEP, BY VALUE: two rigs differing in ONE control of sound A (the whole blend), the same note, the
	// voice's per-partial amplitudes compared. With the control's operator on they must differ; with it off (every other
	// operator still on) they must be identical to the bit — which is what lets the drawer grey an off operator's controls.
	const auto render = [](const DrawerControl* c, bool operatorOn, bool moved, bool link = false) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		activeDrawer(rig);
		if (link) {
			rig.set(dp(0, SpectralControl::FormantShiftLink), 1.0);
		}
		if (c != nullptr) {
			rig.set(dp(0, feathers::spectralOperatorOf(c->id)), operatorOn ? 1.0 : 0.0);
			if (moved) {
				rig.set(dp(0, c->id), c->value);
			}
		}
		playNote(rig);
		return amplitudes(newest(rig));
	};
	for (const DrawerControl& c : drawerControls()) {
		INFO(feathers::findParam(static_cast<clap_id>(dp(0, c.id)))->name);
		CHECK(differs(render(&c, true, false), render(&c, true, true)) > 0);
		CHECK(render(&c, false, false) == render(&c, false, true));
	}
	// LINKED, the high shift and the crossover change nothing even with Formant Shift on (the drawer greys them), and the
	// low shift still does.
	for (const SpectralControl id : { SpectralControl::FormantShiftHigh, SpectralControl::FormantShiftCrossover }) {
		const auto c = std::find_if(drawerControls().begin(), drawerControls().end(),
		                            [id](const DrawerControl& d) { return d.id == id; });
		REQUIRE(c != drawerControls().end());
		CHECK(render(&*c, true, false, true) == render(&*c, true, true, true));
	}
	{
		const auto low = std::find_if(drawerControls().begin(), drawerControls().end(),
		                              [](const DrawerControl& d) { return d.id == SpectralControl::FormantShiftLow; });
		CHECK(differs(render(&*low, true, false, true), render(&*low, true, true, true)) > 0);
	}
	// NOTCH IGNORES RESONANCE (spectral_shaper.h; the drawer greys it there), and every other mode hears it.
	for (size_t m = 0; m < feathers::kSpectralFilterModeNames.size(); ++m) {
		CAPTURE(m);
		const auto at = [m](double resonance) {
			SynthRig rig(OscType::Additive);
			soloA(rig);
			activeDrawer(rig);
			rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(m));
			rig.set(dp(0, SpectralControl::FilterGain), 6.0); // a Bell at 0 dB is no filter (F2): give it a gain to hear
			rig.set(dp(0, SpectralControl::FilterResonance), resonance);
			playNote(rig);
			return amplitudes(newest(rig));
		};
		const bool notch = m == static_cast<size_t>(sp::FilterMode::Notch);
		CHECK((differs(at(0.1), at(0.9)) > 0) == !notch);
	}
	// THE MODE AND THE LINK, which are controls too.
	{
		SynthRig lp(OscType::Additive), hp(OscType::Additive);
		soloA(lp);
		soloA(hp);
		activeDrawer(lp);
		activeDrawer(hp);
		hp.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::HighPass));
		playNote(lp);
		playNote(hp);
		CHECK(differs(amplitudes(newest(lp)), amplitudes(newest(hp))) > 0);
	}
	CHECK(differs(render(nullptr, true, false, false), render(nullptr, true, false, true)) > 0);
	// AND THE DRAWN CURVE, which is stored state rather than a param: a band moves the targets while its switch is on,
	// and nothing while it is off.
	const auto curved = [](bool on, float band3) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		activeDrawer(rig);
		feathers::SpectralState state;
		state.curve[0].on = on;
		state.curve[0].gain[3] = band3;
		rig.synth.setSpectralState(state);
		playNote(rig);
		return amplitudes(newest(rig));
	};
	CHECK(differs(curved(true, 1.0f), curved(true, 0.25f)) > 0);
	CHECK(curved(false, 1.0f) == curved(false, 0.25f));
}

TEST_CASE("additive drawer: the master switch off is the drawer-off targets to the bit, does no drawer work, and keeps every setting") {
	// DESIGN §3.1: off is the engine's own road exactly, at zero cost, an A/B rather than a reset. Both sounds' drawers on.
	SynthRig plain(OscType::Additive);
	playNote(plain);
	const std::vector<float> neutral = amplitudes(newest(plain));

	SynthRig rig(OscType::Additive);
	activeDrawer(rig, 0);
	activeDrawer(rig, 1);
	feathers::SpectralState off;
	off.on = false;
	off.curve[0].on = true;
	off.curve[0].gain[0] = 0.5f;
	rig.synth.setSpectralState(off);
	playNote(rig);
	const OscVoice& osc = newest(rig);
	CHECK(amplitudes(osc) == neutral); // to the bit
	const auto work = osc.drawerWorkForTest();
	CHECK(work.harmonic == 0);
	CHECK(work.filter == 0);
	CHECK(work.tilt == 0);
	CHECK(work.applies == 0);
	CHECK_FALSE(osc.drawerForTest(0).enabled);
	// SETTINGS KEPT: every sound's params and curve stand as they were.
	CHECK(rig.params.base(static_cast<clap_id>(dp(0, SpectralControl::FilterOn))) == 1.0);
	CHECK(rig.params.base(static_cast<clap_id>(dp(1, SpectralControl::CombOn))) == 1.0);

	// AND BACK ON: the very settings come back, and they are the drawer a fresh rig with the switch on plays.
	feathers::SpectralState on = off;
	on.on = true;
	rig.synth.setSpectralState(on);
	std::vector<float> l, r;
	rig.play(1024, l, r);
	SynthRig fresh(OscType::Additive);
	activeDrawer(fresh, 0);
	activeDrawer(fresh, 1);
	fresh.synth.setSpectralState(on);
	playNote(fresh);
	CHECK(newest(rig).drawerForTest(0) == newest(fresh).drawerForTest(0));
	CHECK(amplitudes(newest(rig)) == amplitudes(newest(fresh)));
	CHECK(amplitudes(newest(rig)) != neutral); // not vacuous: on, the drawer acts
	CHECK(newest(rig).drawerWorkForTest().applies > 0);
}

TEST_CASE("additive drawer: off Additive no voice is handed a drawer") {
	// DESIGN R9: the Spectral effect device supersedes drawers on every other engine, so a V5 voice keeps the neutral
	// default whatever every sound's drawer says.
	for (const OscType engine : { OscType::V5, OscType::BasicShapes }) {
		SynthRig rig(engine);
		activeDrawer(rig, 0);
		activeDrawer(rig, 1);
		playNote(rig);
		CHECK(newest(rig).drawerForTest(0) == sp::ShaperParams {});
		CHECK_FALSE(rig.synth.drawerStereoLiveForTest());
	}
}

TEST_CASE("additive drawer: a pair routed into a sound's Cutoff builds each ear its own drawer, and a mono patch builds one") {
	// DESIGN §4.3: the S controls are stereo destinations. The Stereo Tool's Split at In 0.3 publishes (+0.3, −0.3); routed
	// into sound A's Cutoff at amount 0.5 it moves the left ear's cutoff up by 0.15 of the range and the right's down.
	namespace st = feathers::mod::stereo;
	constexpr float kIn = 0.3f;
	constexpr float kAmount = 0.5f;
	const double kBase = std::log2(1500.0);
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterCutoff), kBase);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(st::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(st::Param::In)] = kIn;
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);

	SUBCASE("mono: no pair, no second drawer and no stereo road") {
		playNote(rig);
		CHECK_FALSE(rig.synth.drawerStereoLiveForTest());
		CHECK_FALSE(rig.synth.lastChunkStereoForTest());
		CHECK(newest(rig).drawerWorkRightForTest().prepares == 0);
	}
	SUBCASE("stereo: each ear its own cutoff, each ear's partials its own filter") {
		std::vector<float> l, r;
		rig.play(64, l, r);
		const int cutoff = feathers::modDestinationIndexOf(dp(0, SpectralControl::FilterCutoff));
		REQUIRE(cutoff >= 0);
		feathers::test::Routes routes;
		routes.set(feathers::backplane::Source::output(rack.instanceAt(0), static_cast<size_t>(st::Output::Split)),
		           feathers::backplane::Destination::core(static_cast<size_t>(cutoff)), kAmount);
		routes.publish(rig.params);
		playNote(rig);
		REQUIRE(rig.params.stereoLive(dp(0, SpectralControl::FilterCutoff)));
		CHECK(rig.synth.drawerStereoLiveForTest());
		CHECK(rig.synth.lastChunkStereoForTest());
		const OscVoice& osc = newest(rig);
		CHECK(osc.stereoStreamForTest());
		const feathers::ParamDesc* cut = feathers::findParam(static_cast<clap_id>(dp(0, SpectralControl::FilterCutoff)));
		const double m = static_cast<double>(kAmount) * static_cast<double>(kIn) * (cut->max - cut->min);
		const sp::ShaperParams left = osc.drawerForTest(0);
		const sp::ShaperParams right = osc.drawerForTest(1);
		CHECK(static_cast<double>(left.filter.cutoffHz) == doctest::Approx(std::exp2(kBase + m)).epsilon(1e-4));
		CHECK(static_cast<double>(right.filter.cutoffHz) == doctest::Approx(std::exp2(kBase - m)).epsilon(1e-4));
		const PartialSpectrum& spec = rig.table().partials;
		checkAmplitudes(osc, filterRestated(spec, left, hzOfKey(48.0)), 0);
		checkAmplitudes(osc, filterRestated(spec, right, hzOfKey(48.0)), 1);
		CHECK(osc.drawerWorkRightForTest().filter > 0);
	}
}

// ---- THE MORPH: one drawer per voice, the sounds' settings blended by the character's weights --------------------

namespace {
/// The two sounds' weights the voice's drawer is blended by (the default patch's A and B), asserted to be a real morph.
std::pair<double, double> abWeights(const SynthRig& rig) {
	const feathers::SoundWeights& w = rig.synth.drawerWeightsForTest(0);
	REQUIRE(w.weight[0] > 0.1);
	REQUIRE(w.weight[1] > 0.1);
	CHECK(w.weight[0] + w.weight[1] == doctest::Approx(1.0));
	return { w.weight[0], w.weight[1] };
}
} // namespace

TEST_CASE("additive drawer morph: the cutoff blends in log-frequency by the sounds' weights — at 50/50, the log midpoint") {
	// A at 400 Hz, B at 3.6 kHz, both LP. The blend is exp2(Σ w·log2 f): the geometric mean when the weights are equal, and
	// never the arithmetic one. The weights are the character's, read from the synth.
	const auto blended = [](double path) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, path); // the tour from A to B: both sounds in the blend
		for (const auto& [slot, hz] : { std::pair { size_t { 0 }, 400.0 }, std::pair { size_t { 1 }, 3600.0 } }) {
			rig.set(dp(slot, SpectralControl::FilterOn), 1.0);
			rig.set(dp(slot, SpectralControl::FilterCutoff), std::log2(hz));
		}
		playNote(rig);
		const double wa = rig.synth.drawerWeightsForTest(0).weight[0];
		const double wb = rig.synth.drawerWeightsForTest(0).weight[1];
		REQUIRE(wa > 0.0);
		REQUIRE(wb > 0.0);
		const sp::ShaperParams d = newest(rig).drawerForTest(0);
		CHECK(d.filter.on);
		CHECK(d.filter.mix == 1.0f); // on in every sound: the whole filter
		CHECK_FALSE(d.moreFilters[0].on);
		CHECK(static_cast<double>(d.filter.cutoffHz)
		      == doctest::Approx(std::exp2(wa * std::log2(400.0) + wb * std::log2(3600.0))).epsilon(1e-5));
		return std::tuple { wa, wb, static_cast<double>(d.filter.cutoffHz) };
	};
	// A PUCK PART-WAY ALONG, wherever its weights fall, and one found to sit at 50/50 by bisection on Path.
	(void)blended(0.3);
	double lo = 0.05, hi = 0.95;
	for (int k = 0; k < 40; ++k) {
		const double mid = 0.5 * (lo + hi);
		const auto [wa, wb, hz] = blended(mid);
		(void)hz;
		(wa > wb ? lo : hi) = mid; // A's share falls as the puck tours toward B
	}
	const auto [wa, wb, hz] = blended(0.5 * (lo + hi));
	REQUIRE(std::abs(wa - wb) < 1e-3);
	CHECK(hz == doctest::Approx(std::sqrt(400.0 * 3600.0)).epsilon(2e-3)); // 1200 Hz, the log midpoint
	CHECK(std::abs(hz - 2000.0) > 500.0);                                 // and nowhere near the linear one
}

TEST_CASE("additive drawer morph: an operator on in A and off in B fades by A's weight — mix = w_A") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(900.0));
	rig.set(dp(0, SpectralControl::CombOn), 1.0);
	rig.set(dp(0, SpectralControl::CombDepth), 0.8);
	rig.set(dp(0, SpectralControl::TiltOn), 1.0);
	rig.set(dp(0, SpectralControl::TiltAmount), -9.0);
	rig.set(dp(1, SpectralControl::CombDepth), 0.1); // B's settings are B's, and B's comb is OFF: they must not leak in
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	(void)wb;
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.filter.on);
	CHECK(static_cast<double>(d.filter.mix) == doctest::Approx(wa).epsilon(1e-6));
	CHECK(static_cast<double>(d.filter.cutoffHz) == doctest::Approx(900.0).epsilon(1e-5)); // A's alone: B is not in the mean
	CHECK(static_cast<double>(d.comb.depth) == doctest::Approx(0.8 * wa).epsilon(1e-6));
	CHECK(static_cast<double>(d.tilt.dbPerOctave) == doctest::Approx(-9.0 * wa).epsilon(1e-6));
	// AND HEARD: the voice's targets are the filter to the power w_A (the comb and Tilt switched off for the restatement).
	SynthRig filterOnly(OscType::Additive);
	filterOnly.set(ParamId::Path, 0.4);
	filterOnly.set(dp(0, SpectralControl::FilterOn), 1.0);
	filterOnly.set(dp(0, SpectralControl::FilterCutoff), std::log2(900.0));
	playNote(filterOnly);
	const sp::ShaperParams f = newest(filterOnly).drawerForTest(0);
	CHECK(static_cast<double>(f.filter.mix) == doctest::Approx(abWeights(filterOnly).first).epsilon(1e-6));
	checkAmplitudes(newest(filterOnly), filterRestated(filterOnly.table().partials, f, hzOfKey(48.0)));
}

TEST_CASE("additive drawer morph: all weight on sounds whose operators are all off gives the drawer-off targets to the bit") {
	// A has every operator on and a drawn curve, and is PARKED (weight 0); B, the whole blend, has its drawer off.
	SynthRig plain(OscType::Additive);
	plain.set(ParamId::AEnable, 0.0);
	playNote(plain);
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::AEnable, 0.0);
	activeDrawer(rig, 0);
	feathers::SpectralState state;
	state.curve[0].on = true;
	state.curve[0].gain[1] = 0.2f;
	rig.synth.setSpectralState(state);
	playNote(rig);
	REQUIRE(rig.synth.drawerWeightsForTest(0).weight[0] == 0.0);
	CHECK(amplitudes(newest(rig)) == amplitudes(newest(plain))); // to the bit
	CHECK(newest(rig).drawerWorkForTest().applies == 0);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK_FALSE(d.filter.on);
	CHECK_FALSE(d.comb.on);
	CHECK_FALSE(d.curve.on);
}

TEST_CASE("additive drawer morph: LP in A and HP in B gives the product of the two filters, each to its sound's weight") {
	// FILTER MODE CANNOT BE AVERAGED (lead's call, 2026-10-10): each mode present is a filter of its own at its group's mix,
	// and the gains multiply. Restated per partial as LP^wA · HP^wB.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::LowPass));
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(1200.0));
	rig.set(dp(1, SpectralControl::FilterOn), 1.0);
	rig.set(dp(1, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::HighPass));
	rig.set(dp(1, SpectralControl::FilterCutoff), std::log2(300.0));
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	REQUIRE(d.filter.on);
	REQUIRE(d.moreFilters[0].on);
	CHECK_FALSE(d.moreFilters[1].on);
	CHECK(d.filter.mode == sp::FilterMode::LowPass);
	CHECK(d.moreFilters[0].mode == sp::FilterMode::HighPass);
	CHECK(static_cast<double>(d.filter.mix) == doctest::Approx(wa).epsilon(1e-6));
	CHECK(static_cast<double>(d.moreFilters[0].mix) == doctest::Approx(wb).epsilon(1e-6));
	CHECK(static_cast<double>(d.filter.cutoffHz) == doctest::Approx(1200.0).epsilon(1e-5));
	CHECK(static_cast<double>(d.moreFilters[0].cutoffHz) == doctest::Approx(300.0).epsilon(1e-5));
	checkAmplitudes(newest(rig), filterRestated(rig.table().partials, d, hzOfKey(48.0)));
}

TEST_CASE("additive drawer morph: a wide patch, whose ears' weights differ, builds each ear its own blend") {
	// TERRAIN STEREO (Width up, both sounds wide): each ear is its own character with its own weights, so each ear's
	// drawer is its own blend. A's comb on, B's off: each ear's comb depth is A's depth times THAT ear's weight of A.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Width, 1.0);
	rig.set(ParamId::AWidth, 1.0);
	rig.set(ParamId::BWidth, 1.0);
	rig.set(dp(0, SpectralControl::CombOn), 1.0);
	rig.set(dp(0, SpectralControl::CombDepth), 0.9);
	playNote(rig);
	REQUIRE(rig.synth.mainBankForTest().stereoActive());
	const double wl = rig.synth.drawerWeightsForTest(0).weight[0];
	const double wr = rig.synth.drawerWeightsForTest(1).weight[0];
	REQUIRE(std::abs(wl - wr) > 1e-3); // the ears' blends really differ
	CHECK(rig.synth.drawerStereoLiveForTest());
	const OscVoice& osc = newest(rig);
	CHECK(osc.stereoStreamForTest());
	CHECK(static_cast<double>(osc.drawerForTest(0).comb.depth) == doctest::Approx(0.9 * wl).epsilon(1e-6));
	CHECK(static_cast<double>(osc.drawerForTest(1).comb.depth) == doctest::Approx(0.9 * wr).epsilon(1e-6));
	CHECK(osc.drawerWorkRightForTest().harmonic > 0); // the right ear's drawer was really computed
}

TEST_CASE("additive drawer morph: a linked sound's crossover is inert — in a mixed-link morph the crossover is the unlinked sound's alone") {
	// THE HONESTY RULE THROUGH THE BLEND (spec review, 2026-10-10): a linked sound's Crossover knob is greyed because Link
	// makes it inert, so it may not move the morph. A linked at 100 Hz, B unlinked at 8 kHz: the blend is unlinked and its
	// crossover is B's EXACTLY (a lone contributor), and moving A's crossover changes neither the drawer nor the sound.
	const auto rigAt = [](double crossoverA) {
		auto rig = std::make_unique<SynthRig>(OscType::Additive);
		rig->set(ParamId::Path, 0.4);
		for (const size_t slot : { size_t { 0 }, size_t { 1 } }) {
			rig->set(dp(slot, SpectralControl::FormantShiftOn), 1.0);
			rig->set(dp(slot, SpectralControl::FormantShiftLow), slot == 0 ? 4.0 : -3.0);
			rig->set(dp(slot, SpectralControl::FormantShiftHigh), slot == 0 ? 9.0 : 6.0);
		}
		rig->set(dp(0, SpectralControl::FormantShiftLink), 1.0);
		rig->set(dp(0, SpectralControl::FormantShiftCrossover), std::log2(crossoverA));
		rig->set(dp(1, SpectralControl::FormantShiftLink), 0.0);
		rig->set(dp(1, SpectralControl::FormantShiftCrossover), std::log2(8000.0));
		playNote(*rig);
		return rig;
	};
	const auto low = rigAt(100.0);
	const auto high = rigAt(3000.0);
	const sp::ShaperParams d = newest(*low).drawerForTest(0);
	REQUIRE(d.formantShift.on);
	CHECK_FALSE(d.formantShift.link); // one contributor is unlinked
	CHECK(d.formantShift.crossoverHz == static_cast<float>(std::exp2(std::log2(8000.0)))); // B's alone, exactly
	CHECK(newest(*high).drawerForTest(0) == d);
	CHECK(amplitudes(newest(*high)) == amplitudes(newest(*low)));
	// NOT VACUOUS: B's crossover, the live one, does move the sound.
	const auto liveMoved = [&] {
		auto rig = std::make_unique<SynthRig>(OscType::Additive);
		rig->set(ParamId::Path, 0.4);
		for (const size_t slot : { size_t { 0 }, size_t { 1 } }) {
			rig->set(dp(slot, SpectralControl::FormantShiftOn), 1.0);
			rig->set(dp(slot, SpectralControl::FormantShiftLow), slot == 0 ? 4.0 : -3.0);
			rig->set(dp(slot, SpectralControl::FormantShiftHigh), slot == 0 ? 9.0 : 6.0);
		}
		rig->set(dp(0, SpectralControl::FormantShiftLink), 1.0);
		rig->set(dp(0, SpectralControl::FormantShiftCrossover), std::log2(100.0));
		rig->set(dp(1, SpectralControl::FormantShiftCrossover), std::log2(600.0));
		playNote(*rig);
		return rig;
	}();
	CHECK(differs(amplitudes(newest(*liveMoved)), amplitudes(newest(*low))) > 0);
}

TEST_CASE("additive drawer morph: a unanimous patch — every sound the same drawer — is the single sound's drawer to the bit, at any weights") {
	// EVERY OPERATOR ON IN EVERY SOUND, with the same settings: each mix is the literal 1.0 and each mean a weighted mean of
	// equal values. Pinned against the single-sound drawer bit for bit (operator== on ShaperParams, whose float fields
	// equal) at several weights; the targets are a deterministic function of that drawer.
	SynthRig solo(OscType::Additive);
	soloA(solo);
	activeDrawer(solo, 0);
	playNote(solo);
	const sp::ShaperParams single = newest(solo).drawerForTest(0);
	for (const double path : { 0.15, 0.4, 0.55, 0.85 }) {
		CAPTURE(path);
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, path);
		activeDrawer(rig, 0);
		activeDrawer(rig, 1);
		playNote(rig);
		REQUIRE(rig.synth.drawerWeightsForTest(0).weight[0] > 0.0);
		REQUIRE(rig.synth.drawerWeightsForTest(0).weight[1] > 0.0);
		CHECK(newest(rig).drawerForTest(0) == single);
	}
}

TEST_CASE("additive drawer morph: the drawn curves blend per band in linear amplitude, an off curve counting as unity") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	feathers::SpectralState state;
	state.curve[0].on = true;
	state.curve[1].on = false;
	for (size_t b = 0; b < sp::kCurveBands; ++b) {
		state.curve[0].gain[b] = 0.1f + 0.02f * static_cast<float>(b);
		state.curve[1].gain[b] = 3.0f; // B's curve is OFF: its gains must not reach the blend
	}
	rig.synth.setSpectralState(state);
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	REQUIRE(d.curve.on);
	for (size_t b = 0; b < sp::kCurveBands; ++b) {
		CAPTURE(b);
		// (1 − w_A)·1 + w_A·g_A: B, off, counts as unity.
		const double want = (1.0 - wa) + wa * double(state.curve[0].gain[b]);
		CHECK(double(d.curve.gain[b]) == doctest::Approx(want).epsilon(1e-6));
	}
	// BOTH ON: w_A·g_A + w_B·g_B, linear in amplitude (a drawn zero stays a zero's share, not −inf dB).
	state.curve[1].on = true;
	state.curve[1].gain.fill(0.0f);
	rig.synth.setSpectralState(state);
	std::vector<float> l, r;
	rig.play(256, l, r);
	const sp::ShaperParams both = newest(rig).drawerForTest(0);
	for (size_t b = 0; b < sp::kCurveBands; ++b) {
		CAPTURE(b);
		CHECK(double(both.curve.gain[b]) == doctest::Approx(wa * double(state.curve[0].gain[b]) + wb * 0.0).epsilon(1e-6));
	}
}

TEST_CASE("additive drawer morph: a stacked part on a stand has its drawer blended by its stand's weights") {
	// VOICE STACKING STEP 4: the Stack Index routed into Pos X stands each part apart, so each part hears its own blend —
	// and its drawer is that blend's: A's comb on at depth 0.9, B's off, so a part's comb depth is 0.9 × its stand's w_A.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(ParamId::StackCount, 4.0);
	rig.set(dp(0, SpectralControl::CombOn), 1.0);
	rig.set(dp(0, SpectralControl::CombDepth), 0.9);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::core(feathers::kStackIndexSource), feathers::test::param(ParamId::PosX), 0.25f);
	routes.publish(rig.params);
	playNote(rig, 48.0, 4096);
	const int slot = rig.synth.newestVoiceSlotForTest();
	std::vector<double> depths;
	int onStands = 0;
	for (int part = 0; part < 4; ++part) {
		const int stand = rig.synth.voiceStackStandForTest(slot, part);
		if (stand < 0) {
			continue;
		}
		++onStands;
		CAPTURE(part);
		const double wa = rig.synth.standWeightsForTest(stand).weight[0];
		const double depth = rig.synth.voiceStackOscForTest(slot, part).drawerForTest(0).comb.depth;
		CHECK(depth == doctest::Approx(0.9 * wa).epsilon(1e-6));
		depths.push_back(depth);
	}
	REQUIRE(onStands >= 2);
	// NOT VACUOUS: the stands' blends differ, so the parts' drawers do.
	CHECK(*std::max_element(depths.begin(), depths.end()) - *std::min_element(depths.begin(), depths.end()) > 1e-3);
}

// ---- THE SPECTRUM DRAWER'S PLOT (D2): what the engine publishes for the drawer to draw ----------------------------------

namespace {

using feathers::SpectralPlotFrame;

/// Sound A alone, its Filter on as a low-pass at 1500 Hz and nothing else: a drawer whose result filterRestated states.
void plotPatch(SynthRig& rig) {
	soloA(rig);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::LowPass));
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(1500.0));
}

SpectralPlotFrame plotOf(const SynthRig& rig) {
	SpectralPlotFrame f;
	rig.synth.plotFrame(f);
	return f;
}

/// Every partial past the frame's count is zero, so two frames compare by value.
void checkTailZero(const SpectralPlotFrame& f) {
	for (int i = f.count; i < SpectralPlotFrame::kPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		REQUIRE(f.ratio[u] == 0.0f);
		REQUIRE(f.terrain[u] == 0.0f);
		REQUIRE(f.left[u] == 0.0f);
		REQUIRE(f.right[u] == 0.0f);
	}
}

bool near(double got, double want) {
	return std::abs(got - want) <= 1e-5 * std::abs(want) + 1e-9;
}

} // namespace

TEST_CASE("additive plot: a held note's frame is the newest voice's partials, its terrain the drawer-off amplitude and its result the bank's targets") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	playNote(rig, 48.0);
	const SpectralPlotFrame f = plotOf(rig);
	const double f0 = hzOfKey(48.0);
	const PartialSpectrum& spec = rig.table().partials;
	CHECK(f.playing == 1);
	CHECK(static_cast<double>(f.f0) == doctest::Approx(f0).epsilon(1e-6));
	CHECK(f.count == renderingCount(spec, f0));
	CHECK(f.stereo == 0);
	const std::vector<double> want = filterRestated(spec, newest(rig).drawerForTest(0), f0);
	int shaped = 0;
	for (int i = 0; i < f.count; ++i) {
		const auto u = static_cast<size_t>(i);
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[u] / kSampleRate);
		CHECK(f.ratio[u] == spec.ratio[u]);
		CHECK(near(f.terrain[u], neutralAmplitude(spec, i, w)));
		CHECK(near(f.left[u], want[u]));
		CHECK(f.right[u] == f.left[u]);
		shaped += f.terrain[u] - f.left[u] > 1e-4f * f.terrain[u] ? 1 : 0;
	}
	checkTailZero(f);
	CHECK(f.count > 8);
	CHECK(shaped > 4); // not vacuous: the result is the terrain shaped, not the terrain
}

TEST_CASE("additive plot: with no note the frame is the main bank's spectrum at the last note's pitch, through the drawer the next note gets") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	std::vector<float> l, r;
	rig.play(512, l, r);
	SUBCASE("before any note: A2") {
		const SpectralPlotFrame f = plotOf(rig);
		const PartialSpectrum& spec = rig.table().partials;
		CHECK(f.playing == 0);
		CHECK(f.f0 == 110.0f);
		CHECK(f.count == renderingCount(spec, 110.0));
		const std::vector<double> want = filterRestated(spec, rig.synth.drawerForTest(-1, false), 110.0);
		for (int i = 0; i < f.count; ++i) {
			INFO("partial " << i);
			CHECK(near(f.left[static_cast<size_t>(i)], want[static_cast<size_t>(i)]));
		}
		checkTailZero(f);
		CHECK(f.count > 8);
	}
	SUBCASE("after a note has ended: its pitch, and the next note at that pitch plays exactly the frame") {
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.play(1024, l, r);
		rig.synth.noteOff(0, 60, 1, 0.0);
		rig.play(8192, l, r);
		REQUIRE(rig.synth.soundingVoiceCount() == 0);
		const SpectralPlotFrame ref = plotOf(rig);
		CHECK(ref.playing == 0);
		CHECK(static_cast<double>(ref.f0) == doctest::Approx(hzOfKey(60.0)).epsilon(1e-6));
		rig.synth.noteOn(0, 60, 2, 1.0);
		rig.play(1024, l, r);
		const SpectralPlotFrame held = plotOf(rig);
		REQUIRE(held.playing == 1);
		CHECK(held.count == ref.count);
		int sounding = 0;
		for (int i = 0; i < ref.count; ++i) {
			const auto u = static_cast<size_t>(i);
			INFO("partial " << i);
			CHECK(held.ratio[u] == ref.ratio[u]);
			CHECK(near(held.terrain[u], ref.terrain[u]));
			CHECK(near(held.left[u], ref.left[u]));
			sounding += ref.left[u] > 1e-6f ? 1 : 0;
		}
		CHECK(sounding > 4);
	}
}

TEST_CASE("additive plot: a wide patch publishes each ear's result, the voice's own two rows") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Width, 1.0);
	rig.set(ParamId::AWidth, 1.0);
	rig.set(ParamId::BWidth, 1.0);
	rig.set(dp(0, SpectralControl::CombOn), 1.0);
	rig.set(dp(0, SpectralControl::CombDepth), 0.9);
	playNote(rig);
	REQUIRE(rig.synth.drawerStereoLiveForTest());
	const SpectralPlotFrame f = plotOf(rig);
	const OscVoice& osc = newest(rig);
	CHECK(f.stereo == 1);
	int apart = 0;
	for (int i = 0; i < f.count; ++i) {
		const auto u = static_cast<size_t>(i);
		INFO("partial " << i);
		CHECK(near(f.left[u], osc.partialAmplitudeForTest(i, 0)));
		CHECK(near(f.right[u], osc.partialAmplitudeForTest(i, 1)));
		apart += std::abs(f.left[u] - f.right[u]) > 1e-4f * std::max(f.left[u], f.right[u]) ? 1 : 0;
	}
	CHECK(apart > 4);
}

TEST_CASE("additive plot: off Additive the frame is empty") {
	for (const OscType engine : { OscType::V5, OscType::BasicShapes }) {
		SynthRig rig(engine);
		activeDrawer(rig, 0);
		playNote(rig);
		CHECK(plotOf(rig).sameAs(SpectralPlotFrame {}));
	}
}

TEST_CASE("additive plot: with no note a held drawer recomputes nothing however often the plot is read, and a moved one is read at once") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	std::vector<float> l, r;
	rig.play(512, l, r);
	const SpectralPlotFrame first = plotOf(rig);
	const auto once = rig.synth.plotWorkForTest(0);
	CHECK(once.prepares == 1);
	CHECK(once.filter == 1);
	for (int k = 0; k < 8; ++k) {
		rig.play(256, l, r);
		CHECK(plotOf(rig).sameAs(first));
	}
	const auto held = rig.synth.plotWorkForTest(0);
	CHECK(held.prepares == once.prepares);
	CHECK(held.filter == once.filter);
	CHECK(rig.synth.plotWorkForTest(1).prepares == 0); // a mono patch never asks the right ear's memory
	// A MOVED CUTOFF: the very next frame is the new drawer's, and the filter group recomputed once.
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(600.0));
	rig.play(256, l, r);
	const SpectralPlotFrame moved = plotOf(rig);
	CHECK(rig.synth.plotWorkForTest(0).filter == once.filter + 1);
	const std::vector<double> want = filterRestated(rig.table().partials, rig.synth.drawerForTest(-1, false), 110.0);
	int changed = 0;
	for (int i = 0; i < moved.count; ++i) {
		const auto u = static_cast<size_t>(i);
		INFO("partial " << i);
		CHECK(near(moved.left[u], want[u]));
		changed += moved.left[u] != first.left[u] ? 1 : 0;
	}
	CHECK(changed > 4);
}

TEST_CASE("additive plot: the engine publishes only while the drawer is open, and closing it stops publishing while the drawer still moves") {
	// THE EDITOR'S DRAWER FLAG (ParamBridge::plotWanted) gates the engine's publish (publishEngineReadings). A held note and
	// a cutoff swept every block (a drawer LFO by hand): closed, nothing is published; open, every block's frame is; a held
	// drawer publishes nothing new; closed again, the serial stands still though the drawer keeps moving.
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	feathers::ParamBridge bridge;
	playNote(rig, 48.0);
	std::vector<float> l, r;
	double cutoff = std::log2(1500.0);
	const auto block = [&](bool sweep) {
		if (sweep) {
			cutoff = cutoff > std::log2(3000.0) ? std::log2(700.0) : cutoff + 0.1;
			rig.set(dp(0, SpectralControl::FilterCutoff), cutoff);
		}
		rig.play(256, l, r);
		feathers::publishEngineReadings(bridge, rig.synth);
	};
	for (int k = 0; k < 4; ++k) {
		block(true);
	}
	CHECK(bridge.plotSerial() == 0); // closed: nothing published
	bridge.setPlotWanted(true);
	for (int k = 0; k < 4; ++k) {
		const uint32_t before = bridge.plotSerial();
		block(true);
		CHECK(bridge.plotSerial() == before + 1);
		SpectralPlotFrame got;
		uint32_t serial = 0;
		REQUIRE(bridge.readPlot(got, serial));
		CHECK(serial == bridge.plotSerial());
		CHECK(got.sameAs(plotOf(rig))); // what the engine renders, whole
		CHECK(got.playing == 1);
	}
	const uint32_t settled = bridge.plotSerial();
	block(false); // the first held block lets the bank's ramp land
	const uint32_t held = bridge.plotSerial();
	CHECK(held <= settled + 1);
	for (int k = 0; k < 4; ++k) {
		block(false);
	}
	CHECK(bridge.plotSerial() == held); // held: nothing new to publish
	bridge.setPlotWanted(false);
	for (int k = 0; k < 4; ++k) {
		block(true);
	}
	CHECK(bridge.plotSerial() == held); // closed again: publishing stopped while the drawer still moves
}

// ---- The vowel (the Formant operator; spectral-ops 2026-10-12) -------------------------------------------------------

namespace {
/// A voice's per-partial amplitudes through a drawer whose ONLY live operator is the vowel, restated from the design's
/// words: base_i = the terrain amplitude × the Nyquist fade, each partial's gain the restated peaks to the depth (the
/// voice's frequencies: f0 × ratio, as floats), then ONE scale c holding the power of base (the vowel is level-compensated,
/// lead's call 2026-10-12), then the flush.
std::vector<double> vowelRestated(const PartialSpectrum& s, const sp::VowelOp& v, double f0) {
	std::vector<double> base(static_cast<size_t>(kMaxPartials), 0.0), g(static_cast<size_t>(kMaxPartials), 1.0);
	double before = 0.0, after = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double hz = static_cast<float>(f0 * s.ratio[i]);
		const double w = static_cast<float>(kTwoPi * f0 * s.ratio[i] / kSampleRate);
		base[size_t(i)] = double(s.envAtRatio[i] * s.fine[i]) * fadeRestated(w);
		const auto peak = [hz](double c) {
			const double b = 50.0 + 0.1 * c;
			const double x = (hz * hz - c * c) / (hz * b);
			return 1.0 / (1.0 + x * x);
		};
		g[size_t(i)] = std::pow(1.0 + 12.0 * (0.62 * peak(v.f1Hz) + 0.372 * peak(v.f2Hz)), double(v.depth));
		before += base[size_t(i)] * base[size_t(i)];
		after += base[size_t(i)] * g[size_t(i)] * base[size_t(i)] * g[size_t(i)];
	}
	const double c = std::sqrt(before / after);
	std::vector<double> amp(static_cast<size_t>(kMaxPartials), 0.0);
	for (int i = 0; i < kMaxPartials; ++i) {
		const double gc = g[size_t(i)] * c;
		const double a = base[size_t(i)] * (gc >= double(sp::kFlushGain) ? gc : 0.0);
		amp[size_t(i)] = std::abs(a) < pb::kSilenceFloor ? 0.0 : a;
	}
	return amp;
}
} // namespace

TEST_CASE("additive drawer: the vowel is heard — the voice's amplitudes are the restated peaks over the terrain, level held") {
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(dp(0, SpectralControl::FormantOn), 1.0);
	rig.set(dp(0, SpectralControl::FormantVowel), 3.0); // Open: 750 / 1150 Hz
	rig.set(dp(0, SpectralControl::FormantDepth), 0.8);
	playNote(rig);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	REQUIRE(d.vowel.on);
	CHECK(d.vowel.f1Hz == 750.0f);
	CHECK(d.vowel.f2Hz == 1150.0f);
	CHECK(d.vowel.depth == 0.8f);
	const std::vector<double> want = vowelRestated(rig.table().partials, d.vowel, hzOfKey(48.0));
	checkAmplitudes(newest(rig), want);
	// Not vacuous: the drawer-off targets differ, and partial 5 (6 × 130.8 = 785 Hz, on F1) rose against partial 0.
	SynthRig plain(OscType::Additive);
	soloA(plain);
	playNote(plain);
	const std::vector<float> off = amplitudes(newest(plain));
	const std::vector<float> on = amplitudes(newest(rig));
	CHECK(differs(off, on) > 0);
	CHECK(on[5] / on[0] > 1.5f * off[5] / off[0]);
}

TEST_CASE("additive drawer morph: two vowels glide in log-Hz by the weights, and a vowel on in A alone fades by A's weight") {
	{
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		for (const auto& [slot, pos] : { std::pair { size_t { 0 }, 1.0 }, std::pair { size_t { 1 }, 6.5 } }) {
			rig.set(dp(slot, SpectralControl::FormantOn), 1.0);
			rig.set(dp(slot, SpectralControl::FormantVowel), pos);
			rig.set(dp(slot, SpectralControl::FormantDepth), slot == 0 ? 0.4 : 0.9);
		}
		playNote(rig);
		const auto [wa, wb] = abWeights(rig);
		const sp::ShaperParams d = newest(rig).drawerForTest(0);
		const feathers::VowelFormants a = feathers::vowelFormants(1.0), b = feathers::vowelFormants(6.5);
		CHECK(d.vowel.on);
		// Each sound's formants weighted by w·Depth (A at 0.4, B at 0.9): how much of its vowel the blend hears.
		const double ka = wa * 0.4, kb = wb * 0.9;
		CHECK(double(d.vowel.f1Hz) == doctest::Approx(std::exp2((ka * std::log2(a.f1Hz) + kb * std::log2(b.f1Hz)) / (ka + kb))).epsilon(1e-6));
		CHECK(double(d.vowel.f2Hz) == doctest::Approx(std::exp2((ka * std::log2(a.f2Hz) + kb * std::log2(b.f2Hz)) / (ka + kb))).epsilon(1e-6));
		CHECK(double(d.vowel.depth) == doctest::Approx(wa * 0.4 + wb * 0.9).epsilon(1e-6)); // on in both: mix 1
		// NOT THE POSITION'S MEAN walked: that lands on a vowel in between (Open/Bright country), far from the glide.
		const feathers::VowelFormants walked = feathers::vowelFormants(wa * 1.0 + wb * 6.5);
		CHECK(std::abs(double(d.vowel.f2Hz) - walked.f2Hz) > 100.0);
	}
	{
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		rig.set(dp(0, SpectralControl::FormantOn), 1.0);
		rig.set(dp(0, SpectralControl::FormantVowel), 5.0);
		rig.set(dp(0, SpectralControl::FormantDepth), 0.7);
		rig.set(dp(1, SpectralControl::FormantVowel), 0.0); // B's settings are B's, and B's vowel is OFF: they must not leak
		playNote(rig);
		const auto [wa, wb] = abWeights(rig);
		(void)wb;
		const sp::ShaperParams d = newest(rig).drawerForTest(0);
		CHECK(d.vowel.f1Hz == 900.0f); // Nasal, A's alone
		CHECK(d.vowel.f2Hz == 1600.0f);
		CHECK(double(d.vowel.depth) == doctest::Approx(0.7 * wa).epsilon(1e-6));
	}
}

// ---- Partials (the count; spectral-ops 2026-10-12) -------------------------------------------------------------------

TEST_CASE("additive drawer: Partials plays ⌈c⌉ partials, the top one at the fraction, each one below at the drawer-off amplitude to the bit") {
	SynthRig plain(OscType::Additive);
	soloA(plain);
	playNote(plain);
	const std::vector<float> off = amplitudes(newest(plain));
	REQUIRE(off[40] > 0.0f); // C3 renders well past the counts below
	const auto at = [](double count, bool master = true) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		rig.set(dp(0, SpectralControl::PartialsOn), 1.0);
		rig.set(dp(0, SpectralControl::PartialsCount), count);
		if (!master) {
			feathers::SpectralState state;
			state.on = false;
			rig.synth.setSpectralState(state);
		}
		playNote(rig);
		if (master) {
			CHECK(newest(rig).drawerForTest(0).partialCount.count == static_cast<float>(count));
		}
		return amplitudes(newest(rig));
	};
	{
		const std::vector<float> a = at(20.25);
		for (int i = 0; i < 20; ++i) {
			INFO("partial " << i);
			CHECK(a[size_t(i)] == off[size_t(i)]);
		}
		CHECK(a[20] == 0.25f * off[20]); // a power of two: exact
		for (int i = 21; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			CHECK(a[size_t(i)] == 0.0f);
		}
	}
	{
		const std::vector<float> a = at(20.0); // a whole count: no partial at a fraction
		CHECK(a[19] == off[19]);
		CHECK(a[20] == 0.0f);
	}
	CHECK(at(320.0) == off);        // every partial: the operator's no-op, to the bit
	CHECK(at(20.0, false) == off);  // the master switch off: the engine's own road
}

TEST_CASE("additive drawer morph: the partial count blends by weight, an off sound counting as every partial") {
	{
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		rig.set(dp(0, SpectralControl::PartialsOn), 1.0);
		rig.set(dp(0, SpectralControl::PartialsCount), 40.0);
		rig.set(dp(1, SpectralControl::PartialsCount), 8.0); // B's count is B's, and B's Partials is OFF
		playNote(rig);
		const auto [wa, wb] = abWeights(rig);
		const sp::ShaperParams d = newest(rig).drawerForTest(0);
		CHECK(d.partialCount.on);
		CHECK(double(d.partialCount.count) == doctest::Approx(wa * 40.0 + wb * 320.0).epsilon(1e-6));
	}
	{
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		for (const auto& [slot, n] : { std::pair { size_t { 0 }, 40.0 }, std::pair { size_t { 1 }, 100.0 } }) {
			rig.set(dp(slot, SpectralControl::PartialsOn), 1.0);
			rig.set(dp(slot, SpectralControl::PartialsCount), n);
		}
		playNote(rig);
		const auto [wa, wb] = abWeights(rig);
		CHECK(double(newest(rig).drawerForTest(0).partialCount.count) == doctest::Approx(wa * 40.0 + wb * 100.0).epsilon(1e-6));
	}
}

TEST_CASE("additive plot: Partials reaches the plot — a held note's frame and the no-note reference both end at ⌈c⌉, the top partial at its fraction") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	rig.set(dp(0, SpectralControl::PartialsOn), 1.0);
	rig.set(dp(0, SpectralControl::PartialsCount), 12.5);
	rig.set(dp(1, SpectralControl::PartialsOn), 1.0);
	rig.set(dp(1, SpectralControl::PartialsCount), 12.5);
	std::vector<float> l, r;
	rig.play(512, l, r);
	const SpectralPlotFrame ref = plotOf(rig);
	CHECK(ref.playing == 0);
	CHECK(ref.count == 13);
	playNote(rig, 48.0);
	const SpectralPlotFrame held = plotOf(rig);
	REQUIRE(held.playing == 1);
	CHECK(held.count == 13);
	// THE TOP PARTIAL AT HALF what the same patch without Partials gives it (a filter is not compensated, so the fraction
	// passes straight through), and every one below it the same.
	SynthRig whole(OscType::Additive);
	plotPatch(whole);
	whole.play(512, l, r);
	const SpectralPlotFrame allRef = plotOf(whole); // the no-note reference without Partials, at the same A2
	REQUIRE(allRef.playing == 0);
	REQUIRE(allRef.count > 13);
	CHECK(ref.left[12] > 0.0f);
	CHECK(ref.left[12] == 0.5f * allRef.left[12]); // the reference's top partial at its fraction too
	for (int i = 0; i < 12; ++i) {
		CHECK(ref.left[size_t(i)] == allRef.left[size_t(i)]);
	}
	playNote(whole, 48.0);
	const SpectralPlotFrame all = plotOf(whole);
	REQUIRE(all.count > 13);
	CHECK(held.left[12] > 0.0f);
	CHECK(held.left[12] == 0.5f * all.left[12]); // a power of two: exact
	for (int i = 0; i < 12; ++i) {
		CHECK(held.left[size_t(i)] == all.left[size_t(i)]);
	}
	checkTailZero(held);
}

// ---- Damp (the note's age; spectral-ops 2026-10-12) -----------------------------------------------------------------

namespace {
/// Damp's gain on a partial at `hz` after `ageFrames` host samples of the note, restated from the design's words: 60 dB
/// in a T60 of 1 / (rate · (hz / 1 kHz)^tilt) seconds, i.e. exp(−ln(1000) · rate · (hz / 1000)^tilt · age).
double dampRestated(double rate, double tilt, double hz, int ageFrames) {
	return std::exp(-std::log(1000.0) * rate * std::pow(hz / 1000.0, tilt) * double(ageFrames) / kSampleRate);
}
} // namespace

TEST_CASE("additive voice: Damp decays each partial by its restated T60 from 1 at the note-on, after the drawer's compensation") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 57.0, f0 = hzOfKey(key);
	sp::ShaperParams shaped;
	shaped.tilt = { true, -3.0f, 1000.0f }; // a compensated operator beside it: the compensation must not undo the decay
	sp::ShaperParams damped = shaped;
	damped.damp = { true, 2.0f, 1.0f };   // T60 0.5 s at 1 kHz, twice as fast an octave up
	auto plain = voiceOn(c, key);
	auto v = voiceOn(c, key);
	plain->voice.setShaperParamsForTest(shaped);
	v->voice.setShaperParamsForTest(damped);
	constexpr int kBlocks = 64;
	renderMono(*plain, kBlocks * kBlock);
	renderMono(*v, kBlocks * kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int age = (kBlocks - 1) * kBlock; // the first block is age 0
	double powerPlain = 0.0, powerDamped = 0.0;
	int checked = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const float a0 = plain->voice.partialAmplitudeForTest(i, 0);
		if (!(a0 > 1e-6f)) {
			continue;
		}
		INFO("partial " << i);
		const double hz = static_cast<float>(f0 * spec.ratio[i]);
		const double want = dampRestated(2.0, 1.0, hz, age);
		CHECK(double(v->voice.dampGainForTest(i)) == doctest::Approx(want).epsilon(1e-4));
		CHECK(double(v->voice.partialAmplitudeForTest(i, 0)) == doctest::Approx(double(a0) * want).epsilon(1e-4));
		powerPlain += double(a0) * a0;
		powerDamped += double(v->voice.partialAmplitudeForTest(i, 0)) * v->voice.partialAmplitudeForTest(i, 0);
		++checked;
	}
	CHECK(checked > 8);
	CHECK(powerDamped < 0.9 * powerPlain); // the level fell: the compensation did not lift the decay back up
	// HELD, THE RATIO POWERS ARE KEPT: one computation for the note, however many blocks follow.
	CHECK(v->voice.dampRecomputesForTest() == 1);
	// A PITCH MOVE IS A NEW FREQUENCY, so a new rate from the next block: the gain is the old rate over the old age times
	// the new rate over the new (tilt 1: an octave up decays twice as fast). It costs NO new ratio powers: the pitch
	// moves only the block's scale (the split that makes vibrato cheap, Damp review 2026-10-12).
	{
		const float before = v->voice.dampGainForTest(3);
		const double hzBefore = static_cast<float>(f0 * spec.ratio[3]);
		v->voice.setPitch(key + 12.0);
		renderMono(*v, 16 * kBlock);
		CHECK(v->voice.dampRecomputesForTest() == 1);
		const double hzAfter = static_cast<float>(hzOfKey(key + 12.0) * spec.ratio[3]);
		CHECK(hzAfter == doctest::Approx(2.0 * hzBefore).epsilon(1e-6));
		CHECK(double(v->voice.dampGainForTest(3))
		      == doctest::Approx(double(before) * dampRestated(2.0, 1.0, hzAfter, 16 * kBlock)).epsilon(1e-4));
	}
	// A STRETCH MOVES THE RATIOS THEMSELVES: new ratio powers, and partial 3 decays at its stretched frequency.
	{
		const float before = v->voice.dampGainForTest(3);
		sp::ShaperParams stretched = damped;
		stretched.stretch = { true, 2.0e-3f };
		v->voice.setShaperParamsForTest(stretched);
		renderMono(*v, 16 * kBlock);
		CHECK(v->voice.dampRecomputesForTest() == 2);
		const float k = 4.0f;
		const double hz = static_cast<float>(hzOfKey(key + 12.0) * double(spec.ratio[3] * std::sqrt(1.0f + 2.0e-3f * k * k)));
		CHECK(double(v->voice.dampGainForTest(3)) == doctest::Approx(double(before) * dampRestated(2.0, 1.0, hz, 16 * kBlock)).epsilon(1e-4));
	}
	// A NOTE-ON STARTS AGAIN AT 1.
	v->reset();
	renderMono(*v, kBlock);
	CHECK(v->voice.dampGainForTest(0) == 1.0f);
	CHECK(v->voice.dampGainForTest(5) == 1.0f);
}

TEST_CASE("additive voice: a long damped note reaches exact zero, partial by partial, and leaves the live count") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	auto v = voiceOn(c, 45.0);
	sp::ShaperParams p;
	p.damp = { true, 20.0f, 3.0f }; // T60 50 ms at 1 kHz, eight times faster an octave up
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 4 * kBlock);
	const int liveEarly = v->voice.partialsLiveForTest();
	renderMono(*v, 48000);
	int zeros = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const float g = v->voice.dampGainForTest(i);
		CHECK((g == 0.0f || g >= pb::kSilenceFloor)); // never a denormal or a value below the floor
		zeros += g == 0.0f ? 1 : 0;
		if (g == 0.0f) {
			CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f);
		}
	}
	CHECK(zeros > 20);
	CHECK(v->voice.partialsLiveForTest() < liveEarly);
}

TEST_CASE("additive drawer morph: Damp's rate blends by weight with an off sound as no decay, and the tilt is the on-sounds' mean") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(dp(0, SpectralControl::DampOn), 1.0);
	rig.set(dp(0, SpectralControl::DampTime), std::log2(0.25)); // rate 4
	rig.set(dp(0, SpectralControl::DampTilt), 2.0);
	rig.set(dp(1, SpectralControl::DampTime), std::log2(0.05)); // B's settings are B's, and B's Damp is OFF
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	(void)wb;
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.damp.on);
	CHECK(double(d.damp.rate) == doctest::Approx(4.0 * wa).epsilon(1e-6));
	CHECK(d.damp.tilt == 2.0f);
}

// ---- Stretch (a frequency operator, the same in both ears; spectral-ops 2026-10-12) -----------------------------------

namespace {
/// Partial i's ratio under Stretch, restated: ratio_i × √(1 + B·k²), k = i + 1 (design §3.2), in the voice's float.
double stretchedRatio(const PartialSpectrum& s, int i, double b) {
	const float k = static_cast<float>(i + 1);
	return static_cast<double>(s.ratio[i] * std::sqrt(1.0f + static_cast<float>(b) * k * k));
}
} // namespace

TEST_CASE("additive voice: Stretch moves partial i to ratio × √(1 + B·k²) — its increment, and its amplitude travelling with it") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 45.0, f0 = hzOfKey(key), b = 1.0e-3;
	auto v = voiceOn(c, key);
	sp::ShaperParams p;
	p.stretch = { true, static_cast<float>(b) };
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 4 * kBlock);
	const PartialSpectrum& spec = playing(*v);
	int rendering = 0, moved = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double r = stretchedRatio(spec, i, b);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		if (!(fadeRestated(w) > 0.0)) {
			break;
		}
		INFO("partial " << i);
		CHECK(std::abs(v->voice.partialIncrementForTest(i) - w) < 2e-6);
		// AMPLITUDES TRAVEL WITH THE PARTIAL (lead's call, 2026-10-12): the terrain's amplitude for partial i, faded at its
		// NEW frequency, with no re-read of the envelope there.
		CHECK(std::abs(double(v->voice.partialAmplitudeForTest(i, 0)) - neutralAmplitude(spec, i, w))
		      <= 1e-5 * neutralAmplitude(spec, i, w) + 1e-9);
		moved += std::abs(r - spec.ratio[i]) > 0.01 * spec.ratio[i] ? 1 : 0;
		++rendering;
	}
	CHECK(rendering > 20);
	CHECK(moved > 5); // not vacuous: the high partials really moved
	// AND B = 0, on, is the operator's no-op: the increments are the unstretched ones, to the bit.
	auto plain = voiceOn(c, key);
	auto zero = voiceOn(c, key);
	sp::ShaperParams none;
	none.stretch = { true, 0.0f };
	zero->voice.setShaperParamsForTest(none);
	renderMono(*plain, 4 * kBlock);
	renderMono(*zero, 4 * kBlock);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(zero->voice.partialIncrementForTest(i) == plain->voice.partialIncrementForTest(i));
	}
}

TEST_CASE("additive voice: with Stretch on, a Phase move too big to fold turns each partial by its STRETCHED ratio") {
	// The phase control turns partial i by ratio_i·2π·Δ, so that the move is one TIME shift for every partial; a stretched
	// partial must turn by the ratio it sounds at, or its phase lands where the unstretched partial would have been.
	const SoundCoord s = offGridSound();
	const double b = 2.0e-3;
	auto v = voiceOn(additive({ s }), 50.0);
	sp::ShaperParams p;
	p.stretch = { true, static_cast<float>(b) };
	v->voice.setShaperParamsForTest(p);
	v->voice.setReadPhase(0.0f, nullptr, 0);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int live = v->voice.partialsLiveForTest();
	REQUIRE(live > 20);
	std::vector<double> before(static_cast<size_t>(live));
	for (int i = 0; i < live; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	const float delta = 0.137f;
	v->voice.setReadPhase(delta, nullptr, 0);
	renderMono(*v, kBlock);
	int differsFromPlain = 0;
	for (int i = 0; i < live; ++i) {
		INFO("partial " << i);
		const double w = v->voice.partialIncrementForTest(i);
		const double r = stretchedRatio(spec, i, b);
		const double want = before[size_t(i)] + kBlock * w + kTwoPi * r * double(delta);
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 2e-4);
		const double plainWant = before[size_t(i)] + kBlock * w + kTwoPi * double(spec.ratio[i]) * double(delta);
		differsFromPlain += std::abs(angleDiff(want, plainWant)) > 1e-2 ? 1 : 0;
	}
	CHECK(differsFromPlain > 5); // not vacuous: the stretched turn is not the plain one
}

TEST_CASE("additive plot: the plot's ratios are the voice's — under Stretch, each partial's plotted ratio × f0 is the frequency it renders at") {
	// SPECTRAL-OPS CONDITION 4: the voice and the plot share one pitch function (pitchPartials), so the plot cannot draw a
	// partial where the voice does not play it. Held note: plotted ratio × 2π·f0 / sr is the bank's own increment; and the
	// no-note reference at the same pitch draws the same ratios.
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	rig.set(dp(0, SpectralControl::StretchOn), 1.0);
	rig.set(dp(0, SpectralControl::StretchAmount), 0.5);
	playNote(rig, 48.0);
	const SpectralPlotFrame held = plotOf(rig);
	REQUIRE(held.playing == 1);
	const double f0 = hzOfKey(48.0);
	const OscVoice& osc = newest(rig);
	REQUIRE(held.count > 20);
	int stretched = 0;
	for (int i = 0; i < held.count; ++i) {
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * double(held.ratio[size_t(i)]) / kSampleRate);
		CHECK(std::abs(osc.partialIncrementForTest(i) - w) < 2e-6);
		stretched += held.ratio[size_t(i)] > rig.table().partials.ratio[i] * 1.01f ? 1 : 0;
	}
	CHECK(stretched > 5);
	checkTailZero(held);
	rig.synth.noteOff(0, 48, 1, 0.0);
	std::vector<float> l, r;
	rig.play(16384, l, r);
	REQUIRE(rig.synth.soundingVoiceCount() == 0);
	const SpectralPlotFrame ref = plotOf(rig);
	REQUIRE(ref.playing == 0);
	CHECK(ref.count == held.count);
	for (int i = 0; i < ref.count; ++i) {
		CHECK(ref.ratio[size_t(i)] == held.ratio[size_t(i)]);
	}
}

TEST_CASE("additive drawer morph: Stretch blends B itself by weight × the mix, an off sound pulling toward harmonic") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(dp(0, SpectralControl::StretchOn), 1.0);
	rig.set(dp(0, SpectralControl::StretchAmount), 0.5);
	rig.set(dp(1, SpectralControl::StretchAmount), 1.0); // B's settings are B's, and B's Stretch is OFF
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	(void)wb;
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.stretch.on);
	// B is ~1e-3: a RELATIVE bound, as doctest's Approx would add a scale of 1 and pass anything within 1e-6 of it.
	CHECK(std::abs(double(d.stretch.b) - wa * 0.01 * 0.125) <= 1e-6 * wa * 0.01 * 0.125);
	// BOTH ON: the mean of the two B's, not the B of the mean Amount (the cube makes them differ by ~2x here).
	SynthRig both(OscType::Additive);
	both.set(ParamId::Path, 0.4);
	for (const auto& [slot, amount] : { std::pair { size_t { 0 }, 0.2 }, std::pair { size_t { 1 }, 0.8 } }) {
		both.set(dp(slot, SpectralControl::StretchOn), 1.0);
		both.set(dp(slot, SpectralControl::StretchAmount), amount);
	}
	playNote(both);
	const auto [va, vb] = abWeights(both);
	const double want = va * sp::StretchOp::bOf(0.2) + vb * sp::StretchOp::bOf(0.8);
	CHECK(std::abs(double(newest(both).drawerForTest(0).stretch.b) - want) <= 1e-6 * want);
	CHECK(std::abs(want - sp::StretchOp::bOf(va * 0.2 + vb * 0.8)) > 0.2 * want);
}

// ---- Shift (a frequency operator, the same in both ears; spectral-ops 2026-10-12) -------------------------------------

namespace {
/// Partial i's frequency under Shift, restated as the voice computes it: the ratio moved by shift / f0 (in float), then × f0.
double shiftedRatio(const PartialSpectrum& s, int i, double f0, double shift) {
	return static_cast<double>(s.ratio[i] + static_cast<float>(shift / f0));
}
/// Shift's low-edge fade, restated from spectral_shaper.h: 1 at and above 20 Hz, 0 at and below 5 Hz, linear between.
double lowEdgeRestated(double hz) {
	return hz >= 20.0 ? 1.0 : (hz > 5.0 ? (hz - 5.0) / 15.0 : 0.0);
}
} // namespace

TEST_CASE("additive voice: Shift moves every partial by the same Hz — its increment, its amplitude travelling with it, and its Phase turn") {
	const SoundCoord s = offGridSound();
	const double key = 57.0, f0 = hzOfKey(key), shift = 37.0;
	auto v = voiceOn(additive({ s }), key);
	sp::ShaperParams p;
	p.shift = { true, static_cast<float>(shift) };
	v->voice.setShaperParamsForTest(p);
	v->voice.setReadPhase(0.0f, nullptr, 0);
	renderMono(*v, 4 * kBlock);
	const PartialSpectrum& spec = playing(*v);
	int rendering = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double r = shiftedRatio(spec, i, f0, shift);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		if (!(fadeRestated(w) > 0.0)) {
			break;
		}
		INFO("partial " << i);
		CHECK(std::abs(v->voice.partialIncrementForTest(i) - w) < 2e-6);
		CHECK(std::abs(w - static_cast<float>(kTwoPi * (f0 * spec.ratio[i] + shift) / kSampleRate)) < 2e-6); // + 37 Hz each
		CHECK(std::abs(double(v->voice.partialAmplitudeForTest(i, 0)) - neutralAmplitude(spec, i, w))
		      <= 1e-5 * neutralAmplitude(spec, i, w) + 1e-9);
		++rendering;
	}
	CHECK(rendering > 20);
	// A PHASE MOVE TOO BIG TO FOLD turns each partial by the ratio it SOUNDS at, (f0·r + shift) / f0: one time shift.
	const int live = v->voice.partialsLiveForTest();
	std::vector<double> before(static_cast<size_t>(live));
	for (int i = 0; i < live; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	const float delta = 0.137f;
	v->voice.setReadPhase(delta, nullptr, 0);
	renderMono(*v, kBlock);
	for (int i = 0; i < live; ++i) {
		INFO("partial " << i);
		const double want = before[size_t(i)] + kBlock * v->voice.partialIncrementForTest(i)
		                    + kTwoPi * shiftedRatio(spec, i, f0, shift) * double(delta);
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), want)) < 2e-4);
	}
}

TEST_CASE("additive voice: Shift −1000 Hz on a low note — every partial at or below 0 Hz is silent, the low edge fades, and the partials above still sound") {
	// SPECTRAL-OPS CONDITION 3. A1 (55 Hz): the first ~18 partials land at or below 0 Hz. Each must be EXACTLY silent, a
	// partial between 5 and 20 Hz at the restated low-edge fraction, and every one above at its travelling amplitude — so
	// the partial loop ran ON past the silent ones instead of stopping at the first (it used to stop at the first partial
	// whose fade is 0, which is right only while the ratios start above 0 Hz).
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 33.0, f0 = hzOfKey(key), shift = -1000.0;
	auto v = voiceOn(c, key);
	sp::ShaperParams p;
	p.shift = { true, static_cast<float>(shift) };
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 4 * kBlock);
	const PartialSpectrum& spec = playing(*v);
	int silentBelow = 0, sounding = 0, firstSounding = -1;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double r = shiftedRatio(spec, i, f0, shift);
		const double hz = static_cast<float>(f0 * r);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		if (w > 0.0 && !(fadeRestated(w) > 0.0)) {
			break; // above the band
		}
		INFO("partial " << i << " at " << hz << " Hz");
		const float a = v->voice.partialAmplitudeForTest(i, 0);
		if (hz <= 0.0) {
			CHECK(a == 0.0f);
			++silentBelow;
			continue;
		}
		const double want = neutralAmplitude(spec, i, w) * lowEdgeRestated(hz);
		CHECK(std::abs(double(a) - want) <= 1e-5 * want + 1e-9);
		if (a > 0.0f) {
			++sounding;
			firstSounding = firstSounding < 0 ? i : firstSounding;
		}
	}
	CHECK(silentBelow >= 15);
	CHECK(sounding > 20);
	CHECK(firstSounding >= silentBelow);
	// AND THE EDGE ITSELF: a shift chosen so partial 10 lands at ~12.5 Hz, inside the fade, plays at its restated fraction —
	// quieter than its travelling amplitude and not silent.
	{
		const double toEdge = 12.5 - f0 * double(spec.ratio[10]);
		auto e = voiceOn(c, key);
		sp::ShaperParams q;
		q.shift = { true, static_cast<float>(toEdge) };
		e->voice.setShaperParamsForTest(q);
		renderMono(*e, 4 * kBlock);
		const double r = shiftedRatio(spec, 10, f0, toEdge);
		const double hz = static_cast<float>(f0 * r);
		REQUIRE(hz > 6.0);
		REQUIRE(hz < 19.0);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		const double full = neutralAmplitude(spec, 10, w);
		REQUIRE(full > 1e-6);
		CHECK(double(e->voice.partialAmplitudeForTest(10, 0)) == doctest::Approx(full * lowEdgeRestated(hz)).epsilon(1e-5));
		CHECK(lowEdgeRestated(hz) < 0.95);
		CHECK(lowEdgeRestated(hz) > 0.05);
	}
	// AND NO SHIFT, NO EDGE: a fundamental below 20 Hz (MIDI 15, 19.4 Hz) is the terrain's amplitude, even with the
	// Partials operator's fraction live (the edge gains then exist, for the count, and must not fade the low end).
	{
		auto low = voiceOn(c, 15.0);
		auto plain = voiceOn(c, 15.0);
		sp::ShaperParams q;
		q.partialCount = { true, 30.5f };
		low->voice.setShaperParamsForTest(q);
		renderMono(*low, 4 * kBlock);
		renderMono(*plain, 4 * kBlock);
		REQUIRE(hzOfKey(15.0) * double(playing(*low).ratio[0]) < 20.0);
		CHECK(plain->voice.partialAmplitudeForTest(0, 0) > 0.0f);
		CHECK(low->voice.partialAmplitudeForTest(0, 0) == plain->voice.partialAmplitudeForTest(0, 0));
	}
}

TEST_CASE("additive plot: under Shift the plot's ratios are the voice's too — ratio × f0 is each partial's shifted frequency") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	rig.set(dp(0, SpectralControl::ShiftOn), 1.0);
	rig.set(dp(0, SpectralControl::ShiftHz), 120.0);
	playNote(rig, 48.0);
	const SpectralPlotFrame held = plotOf(rig);
	REQUIRE(held.playing == 1);
	REQUIRE(held.count > 20);
	const double f0 = hzOfKey(48.0);
	for (int i = 0; i < held.count; ++i) {
		INFO("partial " << i);
		const double w = static_cast<float>(kTwoPi * f0 * double(held.ratio[size_t(i)]) / kSampleRate);
		CHECK(std::abs(newest(rig).partialIncrementForTest(i) - w) < 2e-6);
		CHECK(double(held.ratio[size_t(i)]) * f0 == doctest::Approx(f0 * rig.table().partials.ratio[i] + 120.0).epsilon(1e-5));
	}
}

TEST_CASE("additive drawer morph: Shift's Hz blends by weight × the mix, an off sound counting as no shift") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	rig.set(dp(0, SpectralControl::ShiftOn), 1.0);
	rig.set(dp(0, SpectralControl::ShiftHz), -200.0);
	rig.set(dp(1, SpectralControl::ShiftHz), 900.0); // B's settings are B's, and B's Shift is OFF
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	(void)wb;
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.shift.on);
	CHECK(double(d.shift.hz) == doctest::Approx(-200.0 * wa).epsilon(1e-6));
}

// ---- Review fixes (Formant and Partials, 2026-10-12) ----------------------------------------------------------------

namespace {
/// THREE SOUNDS IN THE BLEND: the default patch's A and B, and C switched on above the A-B line, so a puck part-way along
/// the tour weighs all three. An operator on in two of them has W = their summed weight < the total.
void threeSounds(SynthRig& rig) {
	rig.set(ParamId::CEnable, 1.0);
	rig.set(ParamId::CPlaceX, 0.0);
	rig.set(ParamId::CPlaceY, 0.4);
	rig.set(ParamId::CZone, 300.0);
	rig.set(ParamId::Path, 0.4);
}
std::array<double, 3> abcWeights(const SynthRig& rig) {
	const feathers::SoundWeights& w = rig.synth.drawerWeightsForTest(0);
	for (size_t s = 0; s < 3; ++s) {
		CAPTURE(s);
		REQUIRE(w.weight[s] > 0.05);
	}
	return { w.weight[0], w.weight[1], w.weight[2] };
}
} // namespace

TEST_CASE("additive drawer morph: an operator on in two sounds of three — the mean is over the on-sounds' weight W, the fade W over the total") {
	// THE GAP THE FORMANT REVIEW FOUND: every multi-contributor morph case had every live sound on, so W == total and a
	// mean divided by the total instead of W stayed green. A and C on, B off: f = exp2(Σ (w/W)·log2 f) over A and C, and
	// Depth = (Σ (w/W)·depth) × W/total. The same 2-of-3 check for Tilt, an older operator's mean.
	SynthRig rig(OscType::Additive);
	threeSounds(rig);
	for (const auto& [slot, pos, depth, tilt] : { std::tuple { size_t { 0 }, 1.0, 0.4, -9.0 }, std::tuple { size_t { 2 }, 6.0, 0.9, 6.0 } }) {
		rig.set(dp(slot, SpectralControl::FormantOn), 1.0);
		rig.set(dp(slot, SpectralControl::FormantVowel), pos);
		rig.set(dp(slot, SpectralControl::FormantDepth), depth);
		rig.set(dp(slot, SpectralControl::TiltOn), 1.0);
		rig.set(dp(slot, SpectralControl::TiltAmount), tilt);
	}
	rig.set(dp(1, SpectralControl::FormantVowel), 7.0); // B's are B's, and B's operators are OFF
	rig.set(dp(1, SpectralControl::TiltAmount), 24.0);
	playNote(rig);
	const auto [wa, wb, wc] = abcWeights(rig);
	const double total = wa + wb + wc;
	const double W = wa + wc;
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	const feathers::VowelFormants a = feathers::vowelFormants(1.0), c = feathers::vowelFormants(6.0);
	CHECK(d.vowel.on);
	// The formants weigh each on-sound by w·Depth (A 0.4, C 0.9): B, off, is in neither the sum nor the divisor.
	const double ka = wa * 0.4, kc = wc * 0.9;
	CHECK(double(d.vowel.f1Hz) == doctest::Approx(std::exp2((ka * std::log2(a.f1Hz) + kc * std::log2(c.f1Hz)) / (ka + kc))).epsilon(1e-6));
	CHECK(double(d.vowel.f2Hz) == doctest::Approx(std::exp2((ka * std::log2(a.f2Hz) + kc * std::log2(c.f2Hz)) / (ka + kc))).epsilon(1e-6));
	CHECK(double(d.vowel.depth) == doctest::Approx(((wa / W) * 0.4 + (wc / W) * 0.9) * W / total).epsilon(1e-6));
	CHECK(d.tilt.on);
	CHECK(double(d.tilt.dbPerOctave) == doctest::Approx(((wa / W) * -9.0 + (wc / W) * 6.0) * W / total).epsilon(1e-6));
	// Not vacuous: dividing the vowel's log-mean by anything but its own weights (here, B's too) lands somewhere else.
	CHECK(std::abs(std::exp2((ka * std::log2(a.f1Hz) + kc * std::log2(c.f1Hz)) / (ka + kc + wb * 0.9)) - double(d.vowel.f1Hz)) > 5.0);
}

TEST_CASE("additive voice: the Partials fraction is in base, so a compensated drawer's level is continuous as the count crosses an integer") {
	// THE FRACTION SCALES THE RAW AMPLITUDE BEFORE BASE (partialTargets): with Tilt on, the compensation must see the top
	// partial at its fraction. Restated: base_i = envAtRatio·fine·fade·e_i (e_k = c − k for the top partial k), T_i Tilt's
	// reference factor, c = √(Σbase² / Σ(base·T)²), amplitude_i = base_i·T_i·c. And the rendered power, which the
	// compensation holds at Σbase², moves continuously as the count passes through an integer.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	const double key = 45.0, f0 = hzOfKey(key);
	sp::TiltOp tilt { true, -6.0f, 1000.0f };
	const auto run = [&](float count) {
		auto v = voiceOn(ch, key);
		sp::ShaperParams p;
		p.tilt = tilt;
		p.partialCount = { true, count };
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 4 * kBlock);
		return v;
	};
	const auto powerOf = [](const BankedVoice& v) {
		double sum = 0.0;
		for (int i = 0; i < kMaxPartials; ++i) {
			sum += double(v.voice.partialAmplitudeForTest(i, 0)) * v.voice.partialAmplitudeForTest(i, 0);
		}
		return sum;
	};
	constexpr int k = 20;
	constexpr float eps = 1.0e-3f;
	for (const float count : { k + eps, k + 0.25f, k + 1.0f - eps }) {
		CAPTURE(count);
		auto v = run(count);
		const PartialSpectrum& spec = playing(*v);
		const float frac = count - static_cast<float>(k);
		std::vector<double> base(size_t(k + 1)), t(size_t(k + 1));
		double before = 0.0, after = 0.0;
		for (int i = 0; i <= k; ++i) {
			const double hz = static_cast<float>(f0 * spec.ratio[i]);
			const double w = static_cast<float>(kTwoPi * f0 * spec.ratio[i] / kSampleRate);
			base[size_t(i)] = double(spec.envAtRatio[i] * spec.fine[i] * (i == k ? frac : 1.0f)) * fadeRestated(w);
			t[size_t(i)] = sp::tiltGain(tilt, hz);
			before += base[size_t(i)] * base[size_t(i)];
			after += base[size_t(i)] * t[size_t(i)] * base[size_t(i)] * t[size_t(i)];
		}
		const double c = std::sqrt(before / after);
		for (int i = 0; i <= k; ++i) {
			INFO("partial " << i);
			const double want = base[size_t(i)] * t[size_t(i)] * c;
			CHECK(std::abs(double(v->voice.partialAmplitudeForTest(i, 0)) - want) <= 1e-5 * want + 1e-9);
		}
		CHECK(v->voice.partialAmplitudeForTest(k + 1, 0) == 0.0f);
		CHECK(powerOf(*v) == doctest::Approx(before).epsilon(1e-5)); // the compensation held the power of base
	}
	// CONTINUOUS ACROSS THE INTEGER: just below k + 1 and just above it, the power differs by the sliver of one partial.
	const double below = powerOf(*run(k + 1.0f - eps));
	const double above = powerOf(*run(k + 1.0f + eps));
	CHECK(std::abs(above - below) <= 2e-3 * below);
}

TEST_CASE("additive voice: the Partials count clamps — NaN is every partial, below 1 is one") {
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	auto plain = voiceOn(ch, 45.0);
	renderMono(*plain, 4 * kBlock);
	const auto at = [&](float count) {
		auto v = voiceOn(ch, 45.0);
		sp::ShaperParams p;
		p.partialCount = { true, count };
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 4 * kBlock);
		return v;
	};
	auto nan = at(std::numeric_limits<float>::quiet_NaN());
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(nan->voice.partialAmplitudeForTest(i, 0) == plain->voice.partialAmplitudeForTest(i, 0));
	}
	auto one = at(0.3f);
	CHECK(one->voice.partialAmplitudeForTest(0, 0) == plain->voice.partialAmplitudeForTest(0, 0));
	CHECK(plain->voice.partialAmplitudeForTest(1, 0) > 0.0f);
	for (int i = 1; i < kMaxPartials; ++i) {
		CHECK(one->voice.partialAmplitudeForTest(i, 0) == 0.0f);
	}
}

TEST_CASE("additive voice: a moving Count, Damp, Stretch or Shift never re-prepares the drawer (GainCache keys on the amplitude operators alone)") {
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	auto v = voiceOn(ch, 45.0);
	sp::ShaperParams p;
	p.partialCount = { true, 40.0f };
	p.damp = { true, 0.5f, 1.0f };
	p.stretch = { true, 1.0e-4f };
	p.shift = { true, 10.0f };
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, kBlock);
	const uint32_t prepares = v->voice.drawerWorkForTest().prepares;
	for (int b = 0; b < 8; ++b) {
		p.partialCount.count = 40.0f + 3.3f * float(b);
		p.damp.rate = 0.5f + 0.1f * float(b);
		p.stretch.b = 1.0e-4f * float(b + 1);
		p.shift.hz = 10.0f + 7.0f * float(b);
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, kBlock);
	}
	CHECK(v->voice.drawerWorkForTest().prepares == prepares);
}

// ---- Damp review fixes (2026-10-12) ---------------------------------------------------------------------------------

TEST_CASE("additive drawer: a retrigger starts Damp again at 1 — Legato onto a releasing voice and a Mono restrike — and a gated Legato move lets it run on") {
	// THE SAME-VOICE RETRIGGER THIS SYNTH CAN REACH is the manager's moveAndRetriggerVoice: Legato, a note landing on a voice
	// that is already RELEASING (test_synth's "a legato restrike of a releasing voice" says how that was established). It
	// restarts the envelope but not the bank (the phases run on), so without restartDamp the fresh pluck would start with
	// its upper partials already dead. A GATED Legato move is the pure moveVoice, and Damp runs on through it. A Mono
	// restrike takes a fresh voice, whose note-on starts Damp anyway.
	const auto rigFor = [](double mode) {
		auto rig = std::make_unique<SynthRig>(OscType::Additive);
		soloA(*rig);
		rig->set(ParamId::VoiceMode, mode);
		rig->set(dp(0, SpectralControl::DampOn), 1.0);
		rig->set(dp(0, SpectralControl::DampTime), std::log2(0.5));
		rig->synth.setVoiceEdgesForTest(feathers::kVoiceEdgeSeconds, 1.0); // a second of release to land inside
		return rig;
	};
	std::vector<float> l, r;
	{
		INFO("Legato onto a releasing voice: moveAndRetriggerVoice");
		auto rig = rigFor(2.0);
		rig->play(256, l, r);
		rig->synth.noteOn(0, 48, 1, 1.0);
		rig->play(24000, l, r); // half a second: the upper partials well down
		const int slot = rig->synth.newestVoiceSlotForTest();
		REQUIRE(newest(*rig).dampGainForTest(10) < 0.2f);
		rig->synth.noteOff(0, 48, 1, 0.0);
		rig->play(512, l, r);
		rig->synth.noteOn(0, 55, 2, 1.0);
		rig->play(32, l, r); // one control block
		REQUIRE(rig->synth.newestVoiceSlotForTest() == slot);
		CHECK(newest(*rig).dampGainForTest(10) == 1.0f);
		CHECK(newest(*rig).dampGainForTest(0) == 1.0f);
	}
	{
		INFO("a gated Legato move: moveVoice, Damp runs on");
		auto rig = rigFor(2.0);
		rig->play(256, l, r);
		rig->synth.noteOn(0, 48, 1, 1.0);
		rig->play(24000, l, r);
		const int slot = rig->synth.newestVoiceSlotForTest();
		rig->synth.noteOn(0, 55, 2, 1.0); // overlapping the held note
		rig->play(32, l, r);
		REQUIRE(rig->synth.newestVoiceSlotForTest() == slot);
		CHECK(newest(*rig).dampGainForTest(10) < 0.2f);
	}
	{
		INFO("a Mono restrike: the sounding voice starts at 1");
		auto rig = rigFor(1.0);
		rig->play(256, l, r);
		rig->synth.noteOn(0, 48, 1, 1.0);
		rig->play(24000, l, r);
		rig->synth.noteOn(0, 55, 2, 1.0);
		rig->play(32, l, r);
		CHECK(newest(*rig).dampGainForTest(10) == 1.0f);
	}
}

TEST_CASE("additive voice: Damp after a sample-rate change decays at the restated T60 for the new rate") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 57.0, f0 = hzOfKey(key);
	auto v = voiceOn(c, key);
	sp::ShaperParams p;
	p.damp = { true, 2.0f, 1.0f };
	p.partialCount = { true, 8.0f }; // the same partials at both rates (more would fit under 96 kHz's Nyquist and recompute)
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 16 * kBlock); // the factors computed at 48 kHz
	constexpr double kNewRate = 96000.0;
	v->setSampleRate(kNewRate);
	v->reset();
	v->voice.setPitch(key);
	renderMono(*v, 16 * kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int age = 15 * kBlock;
	for (int i : { 0, 3, 6 }) {
		CAPTURE(i);
		const double hz = static_cast<float>(f0 * spec.ratio[i]);
		const double want = std::exp(-std::log(1000.0) * 2.0 * (hz / 1000.0) * double(age) / kNewRate);
		CHECK(double(v->voice.dampGainForTest(i)) == doctest::Approx(want).epsilon(1e-4));
	}
}

TEST_CASE("additive voice: a partial entering the count mid-note takes Damp's gain from the note's age, as if it had decayed all along") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 45.0, f0 = hzOfKey(key);
	auto v = voiceOn(c, key);
	sp::ShaperParams p;
	p.damp = { true, 1.5f, 1.0f };
	p.partialCount = { true, 10.0f };
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 32 * kBlock);
	p.partialCount.count = 30.0f; // twenty partials come in
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int age = 32 * kBlock; // 33 blocks, the first at age 0
	for (int i = 0; i < 30; ++i) {
		INFO("partial " << i);
		const double hz = static_cast<float>(f0 * spec.ratio[i]);
		const double want = std::exp(-std::log(1000.0) * 1.5 * (hz / 1000.0) * double(age) / kSampleRate);
		CHECK(double(v->voice.dampGainForTest(i)) == doctest::Approx(want).epsilon(1e-4));
	}
	CHECK(v->voice.dampGainForTest(25) < 0.9f); // not vacuous: an entrant arrives decayed, not at 1
}

TEST_CASE("additive drawer morph: Damp with both sounds on — the rate is wa·ra + wb·rb, linear in the rates, not in T60") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	for (const auto& [slot, t60] : { std::pair { size_t { 0 }, 0.25 }, std::pair { size_t { 1 }, 1.0 } }) {
		rig.set(dp(slot, SpectralControl::DampOn), 1.0);
		rig.set(dp(slot, SpectralControl::DampTime), std::log2(t60));
	}
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	const double rate = double(newest(rig).drawerForTest(0).damp.rate);
	CHECK(rate == doctest::Approx(wa * 4.0 + wb * 1.0).epsilon(1e-6));
	CHECK(std::abs(rate - 1.0 / (wa * 0.25 + wb * 1.0)) > 0.1); // not the blend of the T60s
}

TEST_CASE("additive voice: Damp scales both ears — the right ear's amplitudes are its undamped ones times the same gain") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 57.0;
	sp::ShaperParams left, right;
	left.tilt = { true, -3.0f, 1000.0f };
	right.tilt = { true, 6.0f, 1000.0f }; // the ears' drawers differ, so the voice renders two ears
	sp::ShaperParams leftD = left, rightD = right;
	leftD.damp = rightD.damp = { true, 2.0f, 1.0f };
	auto plain = voiceOn(c, key);
	auto v = voiceOn(c, key);
	plain->voice.setDrawer(left, &right);
	v->voice.setDrawer(leftD, &rightD);
	std::vector<float> l(size_t(16 * kBlock)), r(size_t(16 * kBlock));
	for (int b = 0; b < 16; ++b) {
		plain->renderStereo(l.data() + b * kBlock, r.data() + b * kBlock, kBlock);
		v->renderStereo(l.data() + b * kBlock, r.data() + b * kBlock, kBlock);
	}
	int checked = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const float a = plain->voice.partialAmplitudeForTest(i, 1);
		if (!(a > 1e-6f)) {
			continue;
		}
		INFO("partial " << i);
		CHECK(double(v->voice.partialAmplitudeForTest(i, 1)) == doctest::Approx(double(a) * v->voice.dampGainForTest(i)).epsilon(1e-5));
		CHECK(plain->voice.partialAmplitudeForTest(i, 1) != plain->voice.partialAmplitudeForTest(i, 0)); // two ears really
		++checked;
	}
	CHECK(checked > 8);
}

// ---- Shift review fixes (2026-10-12) --------------------------------------------------------------------------------

/// The fold case's partial count: 8 AUDIBLE partials, so the top one (A1, −1000 Hz, index ~25) has
/// a ratio near 8 against the silent bottom's near −17.
constexpr float kFoldCaseCount = 8.0f;

TEST_CASE("additive voice: a Phase move that FOLDS under a deep downward Shift lands every partial's phase exactly, silent ones past −π and below the bank's lowest sounding group included") {
	// A1, Shift −1000 Hz, the count capped so the top partial's ratio is small: the silent low partials' ratios (down to
	// ~−17) are more negative than the top's is positive, so a fold the top partial allows bends them past −π. The bank
	// wraps that angle as it wraps one past +π, so each partial's block-end phase is its phase + 32·(w + ratio·bend),
	// exactly, mod 2π.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	const double key = 33.0, f0 = hzOfKey(key), shift = -1000.0;
	auto v = voiceOn(ch, key);
	sp::ShaperParams p;
	p.shift = { true, static_cast<float>(shift) };
	p.partialCount = { true, kFoldCaseCount };
	v->voice.setShaperParamsForTest(p);
	v->voice.setReadPhase(0.0f, nullptr, 0);
	renderMono(*v, kBlock);
	const PartialSpectrum& spec = playing(*v);
	const int live = v->voice.partialsLiveForTest();
	REQUIRE(live > 16);
	std::vector<double> before(static_cast<size_t>(live));
	for (int i = 0; i < live; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
	}
	const float delta = 1.2f;
	const double bend = kTwoPi * double(delta) / kBlock;
	v->voice.setReadPhase(delta, nullptr, 0);
	renderMono(*v, kBlock);
	int pastMinusPi = 0, checked = 0;
	int first = 0; // the first audible partial, where the count starts
	while (shiftedRatio(spec, first, f0, shift) <= 0.0) {
		++first;
	}
	const int named = std::min(live, first + static_cast<int>(kFoldCaseCount)); // the partials the block names
	for (int i = 0; i < named; ++i) {
		const double r = shiftedRatio(spec, i, f0, shift);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		if (w > 0.0 && !(fadeRestated(w) > 0.0)) {
			break;
		}
		INFO("partial " << i << " ratio " << r);
		const double turn = w + r * bend;
		pastMinusPi += turn < -std::numbers::pi ? 1 : 0;
		CHECK(std::abs(angleDiff(v->voice.partialIncrementForTest(i), turn)) < 4e-5); // folded: the bent angle, wrapped
		// And the block-end phase, for EVERY partial: the groups below the bank's lowest sounding one are not rendered, and
		// the bank turns them by the block's whole bent angle at once (E/F/G re-review N-1), so they land exactly too.
		CHECK(std::abs(angleDiff(v->voice.partialPhaseForTest(i), before[size_t(i)] + kBlock * turn)) < 2e-3);
		++checked;
	}
	CHECK(pastMinusPi > 0); // not vacuous: some silent partial was bent past −π
	CHECK(v->voice.partialsFirstForTest() > 0); // and some partials sat in groups the bank did not render
	CHECK(checked > 16);
}

TEST_CASE("additive voice: Shift's low edge ramps in with the shift — continuous at 0, the restated fraction at −2.5 Hz") {
	// A 19.4 Hz fundamental (MIDI 15) sits inside the edge. Unshifted it is the terrain's amplitude; at ±0.001 Hz it must be
	// all but that (the edge reaches in over the shift's first 5 Hz), and at −2.5 Hz exactly half the edge's effect.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	const double key = 15.0, f0 = hzOfKey(key);
	const auto at = [&](double shift) {
		auto v = voiceOn(ch, key);
		sp::ShaperParams p;
		p.shift = { true, static_cast<float>(shift) };
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 4 * kBlock);
		return v;
	};
	auto plain = voiceOn(ch, key);
	renderMono(*plain, 4 * kBlock);
	const PartialSpectrum& spec = playing(*plain);
	REQUIRE(f0 * double(spec.ratio[0]) < 20.0);
	const double a0 = plain->voice.partialAmplitudeForTest(0, 0);
	REQUIRE(a0 > 0.0);
	for (const double eps : { 1.0e-3, -1.0e-3 }) {
		CAPTURE(eps);
		CHECK(std::abs(double(at(eps)->voice.partialAmplitudeForTest(0, 0)) - a0) <= 1e-4 * a0);
	}
	{
		auto v = at(-2.5);
		const double r = shiftedRatio(spec, 0, f0, -2.5);
		const double hz = static_cast<float>(f0 * r);
		const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
		const double want = neutralAmplitude(spec, 0, w) * (1.0 + (lowEdgeRestated(hz) - 1.0) * 0.5);
		CHECK(double(v->voice.partialAmplitudeForTest(0, 0)) == doctest::Approx(want).epsilon(1e-5));
		CHECK(lowEdgeRestated(hz) < 0.95); // not vacuous: the edge acts there
	}
}

TEST_CASE("additive drawer morph: Shift with both sounds on blends the Hz by weight") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	for (const auto& [slot, hz] : { std::pair { size_t { 0 }, -200.0 }, std::pair { size_t { 1 }, 600.0 } }) {
		rig.set(dp(slot, SpectralControl::ShiftOn), 1.0);
		rig.set(dp(slot, SpectralControl::ShiftHz), hz);
	}
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	CHECK(double(newest(rig).drawerForTest(0).shift.hz) == doctest::Approx(wa * -200.0 + wb * 600.0).epsilon(1e-6));
}

TEST_CASE("additive plot: under a downward Shift the plot's ratios are the voice's, and the no-note reference draws the same") {
	SynthRig rig(OscType::Additive);
	plotPatch(rig);
	rig.set(dp(0, SpectralControl::ShiftOn), 1.0);
	rig.set(dp(0, SpectralControl::ShiftHz), -300.0);
	playNote(rig, 48.0);
	const SpectralPlotFrame held = plotOf(rig);
	REQUIRE(held.playing == 1);
	REQUIRE(held.count > 20);
	const double f0 = hzOfKey(48.0);
	int below = 0;
	for (int i = 0; i < held.count; ++i) {
		INFO("partial " << i);
		CHECK(double(held.ratio[size_t(i)]) * f0 == doctest::Approx(f0 * rig.table().partials.ratio[i] - 300.0).epsilon(1e-4).scale(300.0));
		if (held.ratio[size_t(i)] <= 0.0f) {
			CHECK(held.left[size_t(i)] == 0.0f); // at or below 0 Hz: drawn silent, as it plays
			++below;
			continue;
		}
		const double w = static_cast<float>(kTwoPi * f0 * double(held.ratio[size_t(i)]) / kSampleRate);
		CHECK(std::abs(newest(rig).partialIncrementForTest(i) - w) < 2e-6);
	}
	CHECK(below >= 2);
	rig.synth.noteOff(0, 48, 1, 0.0);
	std::vector<float> l, r;
	rig.play(16384, l, r);
	REQUIRE(rig.synth.soundingVoiceCount() == 0);
	const SpectralPlotFrame ref = plotOf(rig);
	REQUIRE(ref.playing == 0);
	CHECK(ref.count == held.count);
	for (int i = 0; i < ref.count; ++i) {
		CHECK(ref.ratio[size_t(i)] == held.ratio[size_t(i)]);
	}
}

TEST_CASE("additive voice: Partials counts AUDIBLE partials — Count 8 under Shift −1000 Hz on A1 sounds exactly 8, from the first above 0 Hz") {
	// By design, the count starts at the first partial above 0 Hz (those inside the low edge's fade count). So the
	// silenced prefix does not eat the count: 8 sound, then nothing; with 7.5, the eighth plays at half.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	const double key = 33.0, f0 = hzOfKey(key), shift = -1000.0;
	for (const float count : { 8.0f, 7.5f }) {
		CAPTURE(count);
		auto v = voiceOn(ch, key);
		sp::ShaperParams p;
		p.shift = { true, static_cast<float>(shift) };
		p.partialCount = { true, count };
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 4 * kBlock);
		const PartialSpectrum& spec = playing(*v);
		int first = 0;
		while (shiftedRatio(spec, first, f0, shift) <= 0.0) {
			++first;
		}
		REQUIRE(first >= 15);
		for (int i = 0; i < kMaxPartials; ++i) {
			INFO("partial " << i);
			const float a = v->voice.partialAmplitudeForTest(i, 0);
			if (i < first || i >= first + 8) {
				CHECK(a == 0.0f);
				continue;
			}
			const double r = shiftedRatio(spec, i, f0, shift);
			const double hz = static_cast<float>(f0 * r);
			const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
			const double frac = (count == 7.5f && i == first + 7) ? 0.5 : 1.0;
			const double want = neutralAmplitude(spec, i, w) * lowEdgeRestated(hz) * frac;
			CHECK(std::abs(double(a) - want) <= 1e-5 * want + 1e-9);
			if (hz > 5.0) {
				CHECK(a > 0.0f); // sounding: Count silenced none of these
			}
		}
	}
}

TEST_CASE("additive drawer morph: a sound's Vowel at Depth 0 is inert — moving it moves nothing, even beside a sound whose vowel is heard") {
	// THE PANEL GREYS IT (spectralControlInert, with Resonance under Notch), so the blend may not read it: the formants are
	// blended over the sounds whose vowel is heard, and a greyed knob never moves the sound (the honesty rule).
	const auto drawerWith = [](double aPosition) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		rig.set(dp(0, SpectralControl::FormantOn), 1.0);
		rig.set(dp(0, SpectralControl::FormantDepth), 0.0);
		rig.set(dp(0, SpectralControl::FormantVowel), aPosition);
		rig.set(dp(1, SpectralControl::FormantOn), 1.0);
		rig.set(dp(1, SpectralControl::FormantDepth), 0.8);
		rig.set(dp(1, SpectralControl::FormantVowel), 6.0);
		playNote(rig);
		return std::pair { newest(rig).drawerForTest(0), amplitudes(newest(rig)) };
	};
	const auto [a, ampA] = drawerWith(1.0);
	const auto [b, ampB] = drawerWith(4.0);
	CHECK(a.vowel == b.vowel);
	CHECK(ampA == ampB);
	CHECK(a.vowel.f1Hz == 250.0f); // B's vowel alone: Growl
	CHECK(feathers::spectralControlInert(SpectralControl::FormantVowel, 0, false, true));
	CHECK_FALSE(feathers::spectralControlInert(SpectralControl::FormantVowel, 0, false, false));
}

TEST_CASE("additive drawer morph: the vowel blend is continuous as a sound's Depth crosses 0 — its formants weigh w·Depth") {
	// E/F/G REVIEW, 2026-10-12: gathering only the sounds whose vowel is heard made the formants JUMP the moment a Depth left
	// 0 (f1 250 -> 306 Hz between 0 and 1e-6). Weighted by w·Depth, the sound comes in from nothing.
	const auto drawerAt = [](double aDepth) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4);
		rig.set(dp(0, SpectralControl::FormantOn), 1.0);
		rig.set(dp(0, SpectralControl::FormantVowel), 3.0); // Open
		rig.set(dp(0, SpectralControl::FormantDepth), aDepth);
		rig.set(dp(1, SpectralControl::FormantOn), 1.0);
		rig.set(dp(1, SpectralControl::FormantVowel), 6.0); // Growl
		rig.set(dp(1, SpectralControl::FormantDepth), 0.8);
		playNote(rig);
		return newest(rig).drawerForTest(0).vowel;
	};
	const sp::VowelOp zero = drawerAt(0.0);
	const sp::VowelOp tiny = drawerAt(1.0e-6);
	const sp::VowelOp some = drawerAt(0.5);
	CHECK(zero.f1Hz == 250.0f); // B's alone
	CHECK(std::abs(double(tiny.f1Hz) - 250.0) < 1e-3);
	CHECK(std::abs(double(tiny.f2Hz) - 600.0) < 1e-3);
	CHECK(std::abs(double(some.f1Hz) - 250.0) > 50.0); // not vacuous: A's vowel does come in
}

TEST_CASE("additive voice: Partials counts by audibility — a partial crossing 0 Hz under a downward Shift hands its count to the top one smoothly") {
	// E/F/G REVIEW, 2026-10-12: counting a partial in (0, 5] Hz as one whole audible partial while it plays at an edge gain of
	// 0 made a partial POP in at the top the moment the bottom one crossed 0 Hz. Each partial now counts as its edge gain, and
	// the partials run on until they have counted c, the last carrying the remainder: total audible count == c, and a
	// ±0.01 Hz sweep through a crossing moves no partial by more than the edge's own slope allows.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	const double key = 33.0, f0 = hzOfKey(key);
	constexpr float kCount = 8.5f;
	auto probe = voiceOn(ch, key);
	renderMono(*probe, kBlock);
	const PartialSpectrum& spec = playing(*probe);
	const double crossing = -f0 * double(spec.ratio[10]); // partial 10 at 0 Hz
	const auto run = [&](double shift) {
		auto v = voiceOn(ch, key);
		sp::ShaperParams p;
		p.shift = { true, static_cast<float>(shift) };
		p.partialCount = { true, kCount };
		v->voice.setShaperParamsForTest(p);
		renderMono(*v, 4 * kBlock);
		return v;
	};
	std::vector<std::vector<float>> amps;
	for (const double d : { -0.01, 0.0, 0.01 }) {
		CAPTURE(d);
		auto v = run(crossing + d);
		// THE AUDIBLE COUNT: each partial's gain over its plain travelling amplitude, summed.
		double counted = 0.0, peak = 0.0;
		for (int i = 0; i < kMaxPartials; ++i) {
			const double r = shiftedRatio(spec, i, f0, crossing + d);
			const double w = static_cast<float>(kTwoPi * f0 * r / kSampleRate);
			const double plain = w > 0.0 ? neutralAmplitude(spec, i, w) : 0.0;
			const double a = v->voice.partialAmplitudeForTest(i, 0);
			if (plain > 1e-7) {
				counted += a / plain;
			}
			peak = std::max(peak, a);
		}
		CHECK(counted == doctest::Approx(double(kCount)).epsilon(1e-4));
		amps.push_back(amplitudes(v->voice));
		REQUIRE(peak > 0.0);
	}
	float peak = 0.0f;
	for (const float a : amps[1]) {
		peak = std::max(peak, a);
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		INFO("partial " << i);
		CHECK(std::abs(amps[2][size_t(i)] - amps[0][size_t(i)]) <= 2e-3f * peak);
	}
}

TEST_CASE("additive voice: under a downward Shift the bank renders from its lowest sounding group, so Count bounds the work") {
	// E/F/G REVIEW M-3: the partials at or below 0 Hz fill the bank's low groups at amplitude 0; rendering starts above them,
	// so the groups the bank turns every sample are the count's (and the ramp-outs'), not the count plus that silent prefix.
	const Character ch = additive({ sound(120, 0.3f, 1.0) });
	auto v = voiceOn(ch, 24.0); // C1, 32.7 Hz: a −500 Hz shift silences its first ~15 partials
	sp::ShaperParams p;
	p.shift = { true, -500.0f };
	p.partialCount = { true, 16.0f };
	v->voice.setShaperParamsForTest(p);
	renderMono(*v, 8 * kBlock);
	const int first = v->voice.partialsFirstForTest();
	const int live = v->voice.partialsLiveForTest();
	CHECK(first >= 12);
	CHECK(live - first <= 16 + 4 + 4); // the count's groups, one ragged at each end
	for (int i = 0; i < first; ++i) {
		CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f);
	}
}

TEST_CASE("Z7d: the drawer's filter is exactly open at its end — Low Pass at the top, High Pass at the bottom — fades in as the device's does, and a sound there moves nothing in a blend") {
	const double f0 = sp::kKeyTrackPivotHz; // the corner IS the control here, so the fade below is the control's alone
	const auto drawer = [](double mode, double cutoff, bool solo) {
		SynthRig rig(OscType::Additive);
		if (solo) {
			soloA(rig);
		}
		activeDrawer(rig, 0);
		rig.set(dp(0, SpectralControl::FilterMode), mode);
		rig.set(dp(0, SpectralControl::FilterCutoff), cutoff);
		playNote(rig);
		return newest(rig).drawerForTest(0);
	};
	const double top = std::log2(sp::kFilterOpenHighHz);
	const double bottom = std::log2(sp::kFilterOpenLowHz);
	// AT THE END: no filter at all, so the prepared drawer has none and every partial passes it at 1.
	for (const auto& [mode, cutoff] : { std::pair { 0.0, top }, std::pair { 1.0, bottom } }) {
		CAPTURE(mode);
		const sp::ShaperParams p = drawer(mode, cutoff, true);
		CHECK_FALSE(p.filter.on);
		CHECK_FALSE(sp::prepare(p, f0).filter.on);
	}
	// HALF WAY THROUGH THE FADE: the op at full mix, and `prepare` halves it in dB — exactly the device's filter (test_fx_spectral Z7c).
	{
		const sp::ShaperParams p = drawer(0.0, top - 0.5 * sp::kFilterOpenOctaves, true);
		REQUIRE(p.filter.on);
		CHECK(p.filter.mix == doctest::Approx(1.0));
		CHECK(sp::prepare(p, f0).filter.mix == doctest::Approx(0.5).epsilon(1e-4));
	}
	// AWAY FROM THE ENDS, THE OLD BLEND TO THE BIT: the literal mix 1 and the control's own cutoff.
	{
		const sp::ShaperParams p = drawer(0.0, std::log2(1500.0), true);
		CHECK(p.filter.mix == 1.0f);
		CHECK(p.filter.cutoffHz == static_cast<float>(std::exp2(std::log2(1500.0))));
		CHECK(sp::prepare(p, f0).filter.mix == 1.0);
	}
	// IN A BLEND: A at its open end, B a Low Pass at 1 kHz. A's Resonance, Slope and Key Track (greyed on its panel) move nothing,
	// and the filter is B's, at B's share.
	const auto blended = [&](double res, double slope, double track) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4); // both sounds in the blend
		activeDrawer(rig, 0);
		activeDrawer(rig, 1);
		rig.set(dp(0, SpectralControl::FilterCutoff), top);
		rig.set(dp(0, SpectralControl::FilterResonance), res);
		rig.set(dp(0, SpectralControl::FilterSlope), slope);
		rig.set(dp(0, SpectralControl::FilterKeyTrack), track);
		rig.set(dp(1, SpectralControl::FilterCutoff), std::log2(1000.0));
		playNote(rig);
		return std::pair { newest(rig).drawerForTest(0), abWeights(rig).second };
	};
	const auto [a, wb] = blended(0.0, 0.0, 0.0);
	const sp::ShaperParams b = blended(1.0, 1.0, 1.0).first;
	REQUIRE(a.filter.on);
	CHECK(a.filter == b.filter);
	CHECK(a.filter.cutoffHz == doctest::Approx(1000.0).epsilon(1e-5));
	CHECK(static_cast<double>(a.filter.mix) == doctest::Approx(wb).epsilon(1e-5)); // B's share: A is "no filter" in the blend
}

// ---- A-PH (design §8): the measurements behind the phase-optimisation proposal ----------------------------------------

namespace {
namespace aph {

/// One period of Σ A_k cos(k·θ + φ_k), k = 1..K, sampled at N points: its peak |x| and its RMS.
struct Shape {
	std::vector<double> amp; ///< A_k, k = 1..K at [k - 1]
	std::vector<double> phase;
};

struct Table {
	int n = 0;
	std::vector<double> c, s;
	explicit Table(int points) : n(points), c(static_cast<size_t>(points)), s(static_cast<size_t>(points)) {
		for (int m = 0; m < points; ++m) {
			c[size_t(m)] = std::cos(2.0 * std::numbers::pi * m / points);
			s[size_t(m)] = std::sin(2.0 * std::numbers::pi * m / points);
		}
	}
};

void synth(const Table& t, const Shape& sh, std::vector<double>& x) {
	x.assign(size_t(t.n), 0.0);
	for (size_t k = 0; k < sh.amp.size(); ++k) {
		if (sh.amp[k] == 0.0) {
			continue;
		}
		const double re = sh.amp[k] * std::cos(sh.phase[k]);
		const double im = sh.amp[k] * std::sin(sh.phase[k]);
		const size_t h = k + 1;
		for (int i = 0; i < t.n; ++i) {
			const size_t m = (h * size_t(i)) % size_t(t.n);
			x[size_t(i)] += re * t.c[m] - im * t.s[m];
		}
	}
}

double peakOf(const Table& t, const Shape& sh) {
	std::vector<double> x;
	synth(t, sh, x);
	double p = 0.0;
	for (const double v : x) {
		p = std::max(p, std::abs(v));
	}
	return p;
}

double rmsOf(const Shape& sh) {
	double e = 0.0;
	for (const double a : sh.amp) {
		e += 0.5 * a * a; // Parseval: the phases do not enter
	}
	return std::sqrt(e);
}

double crestDb(const Table& t, const Shape& sh) {
	return 20.0 * std::log10(peakOf(t, sh) / rmsOf(sh));
}

Shape withPhases(const Shape& sh, std::vector<double> phase) {
	Shape out = sh;
	out.phase = std::move(phase);
	return out;
}

std::vector<double> zeroPhases(const Shape& sh) { return std::vector<double>(sh.amp.size(), 0.0); }

std::vector<double> newman(const Shape& sh) {
	std::vector<double> p(sh.amp.size());
	const double n = static_cast<double>(sh.amp.size());
	for (size_t k = 0; k < p.size(); ++k) {
		const double h = static_cast<double>(k + 1);
		p[k] = std::numbers::pi * h * h / n;
	}
	return p;
}

/// The engine's note-on law (design §2.4): φ_k = −2π Σ_{l<k} (k − l) p_l, p the power shares.
std::vector<double> weightedSchroeder(const Shape& sh) {
	double total = 0.0;
	for (const double a : sh.amp) {
		total += a * a;
	}
	std::vector<double> p(sh.amp.size(), 0.0);
	for (size_t k = 0; k < p.size(); ++k) {
		double sum = 0.0;
		for (size_t l = 0; l < k; ++l) {
			sum += static_cast<double>(k - l) * sh.amp[l] * sh.amp[l] / total;
		}
		p[k] = -2.0 * std::numbers::pi * sum;
	}
	return p;
}

/// THE ITERATIVE REDUCER (clip, then restore the amplitudes — Van der Ouderaa's L∞ iteration): from `start`, `iters`
/// rounds; the best peak found is kept. `fixed` pins harmonic 1's phase (it does not change the peak to rotate it).
Shape iterate(const Table& t, Shape sh, int iters) {
	Shape best = sh;
	double bestPeak = peakOf(t, sh);
	std::vector<double> x;
	for (int it = 0; it < iters; ++it) {
		synth(t, sh, x);
		double p = 0.0;
		for (const double v : x) {
			p = std::max(p, std::abs(v));
		}
		if (p < bestPeak) {
			bestPeak = p;
			best = sh;
		}
		const double level = 0.9 * p;
		for (double& v : x) {
			v = std::clamp(v, -level, level);
		}
		for (size_t k = 0; k < sh.amp.size(); ++k) {
			const size_t h = k + 1;
			double re = 0.0, im = 0.0;
			for (int i = 0; i < t.n; ++i) {
				const size_t m = (h * size_t(i)) % size_t(t.n);
				re += x[size_t(i)] * t.c[m];
				im += x[size_t(i)] * t.s[m];
			}
			sh.phase[k] = std::atan2(-im, re); // x ≈ A cos(hθ + φ): its DFT bin is (A/2)e^{iφ}·N
		}
	}
	return best;
}

/// The harmonic complex amplitudes of a steady render at f0 (Hann-windowed correlation over the buffer): A_k, φ_k such
/// that y ≈ Σ A_k cos(2π k f0 t + φ_k).
Shape analyse(const std::vector<float>& y, double f0, int harmonics) {
	Shape sh;
	double wsum = 0.0;
	std::vector<double> w(y.size());
	for (size_t n = 0; n < y.size(); ++n) {
		w[n] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(n) / double(y.size() - 1));
		wsum += w[n];
	}
	for (int k = 1; k <= harmonics; ++k) {
		const double omega = 2.0 * std::numbers::pi * k * f0 / kSampleRate;
		double re = 0.0, im = 0.0;
		for (size_t n = 0; n < y.size(); ++n) {
			re += w[n] * double(y[n]) * std::cos(omega * double(n));
			im -= w[n] * double(y[n]) * std::sin(omega * double(n));
		}
		sh.amp.push_back(2.0 * std::hypot(re, im) / wsum);
		sh.phase.push_back(std::atan2(im, re));
	}
	return sh;
}

/// THE SUB-PRIORITY QUESTION: with harmonic 1 pinned at `fund` (1 = a full-scale sine's amplitude, 0 dBFS), how loud can
/// the rest be — its own spectral shape kept, one gain g on all of it — with the summed peak ≤ 1? Bisected on g; at each
/// g the phases are `law` (natural: the sound's own) or optimised from it.
double restGainDb(const Table& t, const Shape& natural, double fund, bool optimise) {
	const auto peakAt = [&](double g) {
		Shape sh = natural;
		sh.amp[0] = fund;
		for (size_t k = 1; k < sh.amp.size(); ++k) {
			sh.amp[k] = natural.amp[k] * g;
		}
		if (optimise) {
			sh.phase = weightedSchroeder(sh);
			sh = iterate(t, sh, 150);
		}
		return peakOf(t, sh);
	};
	// THE REST'S NATURAL GAIN, relative to harmonic 1: g = fund / A_1 keeps the sound's own balance.
	const double unity = fund / natural.amp[0];
	double lo = 0.0, hi = unity * 8.0;
	if (peakAt(lo) > 1.0 + 1e-9) {
		return -std::numeric_limits<double>::infinity();
	}
	for (int i = 0; i < 18; ++i) {
		const double mid = 0.5 * (lo + hi);
		(peakAt(mid) <= 1.0 ? lo : hi) = mid;
	}
	return 20.0 * std::log10(lo / unity);
}

/// THE SUB/REST BOOST: the sound scaled so its NATURAL phases peak at 1, then harmonic 1 raised by B (the rest untouched)
/// with the phases optimised at each B: the largest B whose peak stays ≤ 1, in dB.
double subBoostDb(const Table& t, const Shape& natural) {
	const double scale = 1.0 / peakOf(t, natural);
	const auto peakAt = [&](double b) {
		Shape sh = natural;
		for (double& a : sh.amp) {
			a *= scale;
		}
		sh.amp[0] *= b;
		sh.phase = weightedSchroeder(sh);
		sh = iterate(t, sh, 150);
		return peakOf(t, sh);
	};
	double lo = 1.0, hi = 64.0;
	for (int i = 0; i < 18; ++i) {
		const double mid = std::sqrt(lo * hi);
		(peakAt(mid) <= 1.0 ? lo : hi) = mid;
	}
	return 20.0 * std::log10(lo);
}

} // namespace aph
} // namespace

TEST_CASE("MEASUREMENT A-PH: crest factor by phase law, and how loud the fundamental can be under a 0 dBFS peak" * doctest::skip()) {
	const aph::Table t(2048);
	struct Sound {
		std::string name;
		aph::Shape natural;
	};
	std::vector<Sound> sounds;
	const double key = 60.0;
	const double f0 = hzOfKey(key);
	const int kMax = static_cast<int>(std::floor(20000.0 / f0)); // the audible harmonics at C4: 76
	// THE ENGINES' DEFAULT PATCHES, analysed from their own steady renders (their phases are their natural ones).
	for (const auto& [name, type] : { std::pair { "V5 default", OscType::V5 }, std::pair { "Morph default", OscType::Morph },
	                                   std::pair { "Additive default", OscType::Additive } }) {
		auto v = voiceOn(Patch {}.character(type), key);
		const std::vector<float> all = renderMono(*v, 48000 + 9600);
		const std::vector<float> y(all.begin() + 9600, all.end());
		sounds.push_back({ name, aph::analyse(y, f0, kMax) });
		if (type == OscType::V5) {
			// V5'S DEFAULT CYCLE IS EVEN-HEAVY: its odd harmonics of C4 sit near -29 dB, so it is heard an octave up. A second row
			// takes C5 as its fundamental, which is what Sub priority would treat as "the sub".
			sounds.push_back({ "V5 default (as C5)", aph::analyse(y, 2.0 * f0, kMax / 2) });
		}
	}
	// A SAW (1/k, sine phase: the natural saw), and A SPARSE VOWEL ("ah" on 110 Hz: F1 750, F2 1150, Lorentzian peaks of
	// 90 and 110 Hz half-width over a -12 dB floor, harmonics to 4 kHz).
	{
		aph::Shape saw;
		for (int k = 1; k <= kMax; ++k) {
			saw.amp.push_back(1.0 / k);
			saw.phase.push_back(-std::numbers::pi / 2.0);
		}
		sounds.push_back({ "Saw 1/k (76)", saw });
		aph::Shape vowel;
		for (int k = 1; 110.0 * k <= 4000.0; ++k) {
			const double hz = 110.0 * k;
			const auto peak = [&](double fc, double bw) { return 1.0 / (1.0 + ((hz - fc) / bw) * ((hz - fc) / bw)); };
			vowel.amp.push_back(0.25 + peak(750.0, 90.0) + 0.7 * peak(1150.0, 110.0));
			vowel.phase.push_back(0.0);
		}
		sounds.push_back({ "Vowel ah (36)", vowel });
	}
	for (const Sound& s : sounds) {
		const aph::Shape& sh = s.natural;
		MESSAGE(s.name << ": A1..A6 dB re the loudest: " << [&] {
			const double top = *std::max_element(sh.amp.begin(), sh.amp.end());
			std::string o;
			for (size_t k = 0; k < 6; ++k) {
				o += std::to_string(20.0 * std::log10(sh.amp[k] / top)) + " ";
			}
			return o;
		}());
		const double natural = aph::crestDb(t, sh);
		const double zero = aph::crestDb(t, aph::withPhases(sh, aph::zeroPhases(sh)));
		const double newman = aph::crestDb(t, aph::withPhases(sh, aph::newman(sh)));
		const aph::Shape schroeder = aph::withPhases(sh, aph::weightedSchroeder(sh));
		const double ws = aph::crestDb(t, schroeder);
		const double it = aph::crestDb(t, aph::iterate(t, schroeder, 400));
		MESSAGE(s.name << ": crest natural " << natural << " dB, zero-phase " << zero << ", Newman " << newman << ", weighted Schroeder "
		               << ws << ", iterated " << it << " | headroom gained over natural: Schroeder " << natural - ws << " dB, iterated "
		               << natural - it << " dB");
		// THE FUNDAMENTAL'S LEVEL at the sound's own balance, peak normalised to 1: natural phases, then optimised.
		const auto fundDb = [&](double crest) { return 20.0 * std::log10(sh.amp[0] / (aph::rmsOf(sh) * std::pow(10.0, crest / 20.0))); };
		const auto t0 = std::chrono::steady_clock::now();
		(void)aph::iterate(t, schroeder, 400);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		MESSAGE(s.name << ": at a 0 dBFS peak the fundamental sits at " << fundDb(natural) << " dBFS (natural), " << fundDb(it)
		               << " dBFS (iterated); 400 iterations took " << ms << " ms");
		// SUB PRIORITY: harmonic 1 at 0 dBFS; the loudest the rest can be, relative to the sound's own balance (optimised).
		const double opt = aph::restGainDb(t, sh, 1.0, true);
		const double boost = aph::subBoostDb(t, sh);
		MESSAGE(s.name << ": fundamental pinned at 0 dBFS — the rest can sit at " << opt << " dB against its own balance (optimised); "
		               << "from the natural peak at 1, harmonic 1 can rise " << boost << " dB with the rest untouched; rest/fundamental energy "
		               << 10.0 * std::log10((aph::rmsOf(sh) * aph::rmsOf(sh) - 0.5 * sh.amp[0] * sh.amp[0]) / (0.5 * sh.amp[0] * sh.amp[0])) << " dB");
		CHECK(std::isfinite(natural));
	}
	// THE SQUARE'S FACT, as the instrument sees it: a square of peak 1 has a fundamental of 4/π (+2.1 dB over a full-scale sine).
	aph::Shape square;
	for (int k = 1; k <= 255; ++k) {
		square.amp.push_back(k % 2 == 1 ? 4.0 / (std::numbers::pi * k) : 0.0);
		square.phase.push_back(-std::numbers::pi / 2.0);
	}
	MESSAGE("square to k=255: peak " << aph::peakOf(t, square) << " (Gibbs), fundamental " << 20.0 * std::log10(4.0 / std::numbers::pi) << " dB over a full-scale sine");
}

// ---- A-PH: the phases, built (design §8) ------------------------------------------------------------------------------

namespace {
namespace aphb {

/// THE PATCH'S ADDITIVE CHARACTER with the phases set: mode 0 Natural, 1 Headroom, 2 Sub priority (Sub, Ceiling dB).
Character phased(double mode, double subDb = 0.0, double ceilingDb = -6.0) {
	Patch p;
	p.sets[ParamId::AdditivePhases] = mode;
	p.sets[ParamId::AdditiveSub] = subDb;
	p.sets[ParamId::AdditiveCeiling] = ceilingDb;
	return p.character(OscType::Additive);
}

/// The spectrum the synth posts for a note at `key` (Synth::postPhaseRequest, A-PH3): the voice's own road to its first
/// block, after the drawer (`drawer`, neutral by default), the sub pinned and levelled under Sub priority.
std::pair<std::array<float, kMaxPartials>, int> postedSpectrum(const PreparedCharacter& table, double key = 60.0,
                                                               const sp::ShaperParams& drawer = {}) {
	std::array<float, kMaxPartials> amp {};
	auto cache = std::make_unique<sp::GainCache<kMaxPartials>>();
	const int count = OscVoice::sketchTargets(table, drawer, hzOfKey(key), kSampleRate, *cache, amp.data());
	return { amp, count };
}

/// A store holding the optimiser's profile for `table`, stamped with its build serial.
void publishFor(feathers::partials::PhaseProfileStore& store, const PreparedCharacter& table,
                int rounds = feathers::PhaseService::kRounds) {
	feathers::partials::PhaseOptimiser opt;
	feathers::partials::PhaseProfile prof;
	const auto [amp, count] = postedSpectrum(table);
	opt.optimise(amp.data(), count, prof, rounds);
	prof.serial = table.buildSerial;
	store.publish(prof);
}

struct Crest {
	double peak = 0.0, rms = 0.0;
	double db() const { return 20.0 * std::log10(peak / rms); }
};

/// The steady crest of a voice: one second after the first 0.1 s.
Crest crestOf(BankedVoice& v) {
	const std::vector<float> y = renderMono(v, 48000 + 4800);
	Crest c;
	double sum = 0.0;
	for (size_t n = 4800; n < y.size(); ++n) {
		sum += double(y[n]) * double(y[n]);
		c.peak = std::max(c.peak, std::abs(double(y[n])));
	}
	c.rms = std::sqrt(sum / double(y.size() - 4800));
	return c;
}

} // namespace aphb
} // namespace

TEST_CASE("A-PH: Headroom — the optimiser's profile takes the default patch's rendered crest from the closed form's ~5 dB to at most 2.64 dB") {
	// THE CLOSED FORM, the note-on law the engine had: the default patch at C4 measured 5.04 dB (design §8.3).
	auto natural = voiceOn(aphb::phased(0.0), 60.0);
	const aphb::Crest closed = aphb::crestOf(*natural);
	// HEADROOM, with the optimiser's profile for this very table in the store before the note-on.
	auto v = voiceOn(aphb::phased(1.0), 60.0);
	feathers::partials::PhaseProfileStore store;
	aphb::publishFor(store, v->bank.current(0));
	v->voice.setPhaseProfiles(&store);
	const aphb::Crest optimised = aphb::crestOf(*v);
	MESSAGE("default patch, C4: crest closed form " << closed.db() << " dB, Headroom " << optimised.db() << " dB; RMS " << closed.rms
	                                                << " / " << optimised.rms);
	CHECK(v->voice.profileNoteOnsForTest() == 1u);
	CHECK(optimised.db() <= 2.64 + 0.3);
	CHECK(closed.db() - optimised.db() > 2.0); // not vacuous: the closed form is what the voice would have done
	CHECK(optimised.rms == doctest::Approx(closed.rms).epsilon(0.01)); // Parseval: the energy did not move
}

namespace {
namespace aphb {
/// THE SHAPE'S REST, restated: Σ (envAtRatio·fine·gain)² over partials 1.. of the WHOLE spectrum, upstream of every drawer
/// operator and the Nyquist cull (option (b), 2026-10-18). `gain(i)`: Damp's, 1 without.
template <class Gain>
double shapeRest(const PreparedCharacter& table, Gain gain) {
	double rest = 0.0;
	for (int i = 1; i < kMaxPartials; ++i) {
		const double a = double(table.partials.envAtRatio[size_t(i)]) * double(table.partials.fine[size_t(i)]) * gain(i);
		rest += a * a;
	}
	return std::sqrt(rest);
}
double shapeRest(const PreparedCharacter& table) {
	return shapeRest(table, [](int) { return 1.0; });
}
/// The sub's level relation, read from the bank: partial 0's amplitude (through its Nyquist fade, `fade0`, and the voice's
/// ceiling scale) against the shape's rest, in dB.
double subAgainstShapeDb(const OscVoice& v, const PreparedCharacter& table, double fade0 = 1.0) {
	return 20.0 * std::log10(double(v.partialAmplitudeForTest(0, 0)) / (fade0 * double(v.ceilingScaleForTest()) * shapeRest(table)));
}
/// A heavy drawer: Odd/Even at full Even (which silences every odd harmonic, the fundamental with them), a Comb, a Low Pass.
sp::ShaperParams heavyDrawer() {
	sp::ShaperParams d;
	d.oddEven.on = true;
	d.oddEven.balance = 1.0f;
	d.comb.on = true;
	d.comb.spacing = 3.0f;
	d.comb.depth = 0.8f;
	d.filter.on = true;
	d.filter.cutoffHz = 2500.0f;
	return d;
}
} // namespace aphb
} // namespace

TEST_CASE("A-PH3: under Sub priority the sub is Sub dB against the shape's rest root-sum-square, by value, at every setting") {
	for (const double subDb : { -18.0, -6.0, 0.0, 6.0, 12.0 }) {
		CAPTURE(subDb);
		auto v = voiceOn(aphb::phased(2.0, subDb, 0.0), 60.0); // a 0 dBFS ceiling and no profile: no scale
		(void)renderMono(*v, 4800);
		REQUIRE(v->voice.ceilingScaleForTest() == 1.0f);
		CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) == doctest::Approx(subDb).epsilon(1e-4));
	}
}

TEST_CASE("A-PH3: full Even keeps the sub at its set level — no drawer operator touches it under Sub priority") {
	const sp::ShaperParams heavy = aphb::heavyDrawer();
	auto plain = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	(void)renderMono(*plain, 4800);
	auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	v->voice.setDrawer(heavy, nullptr);
	(void)renderMono(*v, 4800);
	CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) == doctest::Approx(-6.0).epsilon(1e-4));
	// THE SAME SUB AS THE UNDRAWN VOICE'S, by value: the drawer moved nothing of it.
	CHECK(v->voice.partialAmplitudeForTest(0, 0) == doctest::Approx(plain->voice.partialAmplitudeForTest(0, 0)).epsilon(1e-6));
	// THE REST IS STILL SHAPED: partial 2 (the third harmonic, odd) is silenced by full Even as before.
	CHECK(v->voice.partialAmplitudeForTest(2, 0) == 0.0f);
	// NATURAL AND HEADROOM ARE UNCHANGED: the same drawer silences the fundamental there, as the shaper says.
	for (const double mode : { 0.0, 1.0 }) {
		CAPTURE(mode);
		auto n = voiceOn(aphb::phased(mode), 60.0);
		n->voice.setDrawer(heavy, nullptr);
		(void)renderMono(*n, 4800);
		CHECK(n->voice.partialAmplitudeForTest(0, 0) == 0.0f);
	}
}

TEST_CASE("A-PH3: a drawer Low Pass below twice the fundamental keeps the sub at its set level while the rest goes") {
	sp::ShaperParams lp;
	lp.filter.on = true;
	lp.filter.cutoffHz = static_cast<float>(1.5 * hzOfKey(60.0)); // between the sub and partial 1
	auto plain = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	(void)renderMono(*plain, 4800);
	auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	v->voice.setDrawer(lp, nullptr);
	(void)renderMono(*v, 4800);
	CHECK(v->voice.partialAmplitudeForTest(0, 0) == doctest::Approx(plain->voice.partialAmplitudeForTest(0, 0)).epsilon(1e-6));
	CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) == doctest::Approx(-6.0).epsilon(1e-4));
	// AND THE REST DID GO (the case is not vacuous): partial 4 is well down on the undrawn voice's.
	CHECK(v->voice.partialAmplitudeForTest(4, 0) < 0.1f * plain->voice.partialAmplitudeForTest(4, 0));
}

TEST_CASE("A-PH3: a note so high only the sub fits below Nyquist keeps the sub at its set level") {
	const double key = 128.0; // ~13.3 kHz: partial 1 at ~26.6 kHz is past Nyquist at 48 kHz
	auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), key);
	(void)renderMono(*v, 4800);
	for (int i = 1; i < 4; ++i) {
		CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f); // the sub alone renders (the bank counts in fours)
	}
	const double fade0 = double(feathers::partials::nyquistFade(v->voice.partialIncrementForTest(0)));
	CHECK(fade0 == 1.0);
	CHECK(v->voice.partialAmplitudeForTest(0, 0) > 0.0f);
	CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0), fade0) == doctest::Approx(-6.0).epsilon(1e-4));
}

TEST_CASE("A-PH3: Damp decays the sub in step with the rest — the shape's rest through Damp's own gains, by value") {
	// C1, so every one of the spectrum's partials renders (320 × 32.7 Hz is under the Nyquist fade) and the bank's own Damp
	// gains cover the whole rest: the reference's gains can be read off them.
	const double key = 24.0;
	sp::ShaperParams damped;
	damped.damp.on = true;
	damped.damp.rate = 2.0f; // a 0.5 s T60 at 1 kHz
	damped.damp.tilt = 1.0f;
	auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), key);
	v->voice.setDrawer(damped, nullptr);
	(void)renderMono(*v, 128);
	REQUIRE(v->voice.partialsLiveForTest() == kMaxPartials); // every partial renders (the bank's count shrinks later, as Damp floors the top ones)
	const float early = v->voice.partialAmplitudeForTest(0, 0);
	(void)renderMono(*v, 48000);
	const PreparedCharacter& table = v->bank.current(0);
	const double want = feathers::partials::subLevelOf(table.phasing)
	                    * aphb::shapeRest(table, [&](int i) { return double(v->voice.dampGainForTest(i)); });
	CHECK(double(v->voice.partialAmplitudeForTest(0, 0)) == doctest::Approx(want).epsilon(1e-4));
	// AND IT DID DECAY (not vacuous): a second in, the sub is well below where it started (at C1 the low partials that
	// carry the rest decay slowly: Damp scales with (hz / 1 kHz)^tilt).
	CHECK(v->voice.partialAmplitudeForTest(0, 0) < 0.5f * early);
}

TEST_CASE("A-PH3: an operator sweep leaves the sub's level relation intact on every block") {
	auto v = voiceOn(aphb::phased(2.0, -3.0, 0.0), 60.0);
	sp::ShaperParams d = aphb::heavyDrawer();
	double worst = 0.0;
	for (int b = 0; b < 48; ++b) {
		d.filter.cutoffHz = static_cast<float>(300.0 * std::exp2(b / 8.0)); // 300 Hz .. 19 kHz
		d.comb.phase = static_cast<float>(b) / 48.0f;
		d.oddEven.balance = static_cast<float>(std::sin(0.3 * b));
		v->voice.setDrawer(d, nullptr);
		(void)renderMono(*v, 128);
		worst = std::max(worst, std::abs(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) - (-3.0)));
	}
	MESSAGE("operator sweep: the sub's relation off its -3 dB by at most " << worst << " dB");
	CHECK(worst < 1e-3);
}

TEST_CASE("A-PH3: Shift +500 Hz (and Stretch) move the rest and leave the sub at the note's own pitch") {
	sp::ShaperParams d;
	d.shift.on = true;
	d.shift.hz = 500.0f;
	d.stretch.on = true;
	d.stretch.b = 0.002f;
	auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	v->voice.setDrawer(d, nullptr);
	(void)renderMono(*v, 4800);
	const double w0 = 2.0 * std::numbers::pi * hzOfKey(60.0) / kSampleRate * double(v->bank.current(0).partials.ratio[0]);
	CHECK(double(v->voice.partialIncrementForTest(0)) == doctest::Approx(w0).epsilon(1e-6));
	// THE REST MOVED: partial 1 sits above its harmonic by the shift (and a little stretch).
	const double w1 = 2.0 * std::numbers::pi * (2.0 * hzOfKey(60.0) + 500.0) / kSampleRate;
	CHECK(double(v->voice.partialIncrementForTest(1)) > w1);
	// AND UNDER HEADROOM THE SUB MOVES WITH IT (Natural and Headroom unchanged).
	auto h = voiceOn(aphb::phased(1.0), 60.0);
	h->voice.setDrawer(d, nullptr);
	(void)renderMono(*h, 4800);
	CHECK(double(h->voice.partialIncrementForTest(0)) > w0 * 1.5);
}

TEST_CASE("A-PH3: the ceiling holds on a heavily drawn Sub-priority sound, its balance kept") {
	const sp::ShaperParams heavy = aphb::heavyDrawer();
	auto v = voiceOn(aphb::phased(2.0, 12.0, -12.0), 60.0); // the drawn voice peaks near -7 dBFS: -12 binds
	v->voice.setDrawer(heavy, nullptr);
	feathers::partials::PhaseProfileStore store;
	{
		feathers::partials::PhaseOptimiser opt;
		feathers::partials::PhaseProfile prof;
		const auto [amp, count] = aphb::postedSpectrum(v->bank.current(0), 60.0, heavy);
		opt.optimise(amp.data(), count, prof, feathers::PhaseService::kRounds);
		prof.serial = v->bank.current(0).buildSerial;
		store.publish(prof);
	}
	v->voice.setPhaseProfiles(&store);
	const aphb::Crest c = aphb::crestOf(*v);
	const double ceiling = std::pow(10.0, -12.0 / 20.0);
	MESSAGE("heavy drawer, Sub +12 under -12 dBFS: scale " << v->voice.ceilingScaleForTest() << ", peak " << c.peak);
	REQUIRE(v->voice.profileNoteOnsForTest() == 1u); // the post-shaper profile fits the drawn note
	CHECK(v->voice.ceilingScaleForTest() < 1.0f);
	CHECK(c.peak <= ceiling * 1.02);
	CHECK(c.peak >= ceiling * 0.9);
	CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) == doctest::Approx(12.0).epsilon(1e-4)); // the scale is whole
}

TEST_CASE("A-PH3: a heavily drawn sound now gets a profile — the synth posts the spectrum after the drawer") {
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(ParamId::AdditivePhases, 1.0); // Headroom
	rig.set(dp(0, SpectralControl::OddEvenOn), 1.0);
	rig.set(dp(0, SpectralControl::OddEvenBalance), 1.0); // full Even
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(2500.0));
	std::vector<float> l, r;
	rig.play(256, l, r);
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.play(512, l, r);
	// THE PROFILE FOR THIS DRAWN SPECTRUM, computed by the service's thread (a few ms).
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (rig.synth.phaseServiceForTest().computedForTest() == 0u && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	REQUIRE(rig.synth.phaseServiceForTest().computedForTest() >= 1u);
	rig.synth.noteOff(0, 60, 1, 0.0);
	rig.play(48000, l, r); // the first voice gone
	rig.synth.noteOn(0, 60, 2, 1.0);
	rig.play(256, l, r);
	CHECK(newest(rig).profileNoteOnsForTest() == 1u);
}
TEST_CASE("A-PH: note-on only — a held note's phases run on untouched when a new profile arrives, and a fresh note-on takes it") {
	auto v = voiceOn(aphb::phased(1.0), 60.0);
	feathers::partials::PhaseProfileStore store;
	aphb::publishFor(store, v->bank.current(0));
	v->voice.setPhaseProfiles(&store);
	(void)renderMono(*v, 4800);
	REQUIRE(v->voice.profileNoteOnsForTest() == 1u);
	constexpr int kWatch = 24;
	std::array<double, kWatch> before {}, w {};
	for (int i = 0; i < kWatch; ++i) {
		before[size_t(i)] = v->voice.partialPhaseForTest(i);
		w[size_t(i)] = v->voice.partialIncrementForTest(i);
	}
	// A DIFFERENT PROFILE (all phases zero) lands mid-note, for the same spectrum, under a newer serial.
	feathers::partials::PhaseProfile other;
	REQUIRE(store.read(other));
	other.phase.fill(0.0f);
	other.serial += 1;
	store.publish(other);
	constexpr int kRun = 960;
	(void)renderMono(*v, kRun);
	double worst = 0.0;
	for (int i = 0; i < kWatch; ++i) {
		double expect = before[size_t(i)] + kRun * w[size_t(i)];
		double d = v->voice.partialPhaseForTest(i) - expect;
		d -= 2.0 * std::numbers::pi * std::round(d / (2.0 * std::numbers::pi));
		worst = std::max(worst, std::abs(d));
	}
	MESSAGE("held note: largest phase deviation from free running after a new profile " << worst << " rad");
	CHECK(worst < 2e-3);
	CHECK(v->voice.profileNoteOnsForTest() == 1u);
	// A FRESH NOTE-ON (reset) DOES TAKE IT: the new profile's zero phases.
	v->voice.reset();
	v->voice.setPitch(60.0);
	(void)renderMono(*v, 32);
	CHECK(v->voice.profileNoteOnsForTest() == 2u);
}

TEST_CASE("A-PH: Legato and a glide never re-phase — a sounding voice that takes a new note keeps its bank and its running phases") {
	// THE SYNTH'S OWN ROADS: Legato onto a held voice (moveVoice) and onto a releasing one (moveAndRetriggerVoice) keep the bank
	// (test "a retrigger starts Damp again at 1" established the latter). With Headroom on and a fresh profile published
	// between the two notes, the bank is not reset and no note-on takes a profile.
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(ParamId::VoiceMode, 2.0); // Legato
	rig.set(ParamId::AdditivePhases, 1.0);
	std::vector<float> l, r;
	rig.play(256, l, r);
	rig.synth.noteOn(0, 48, 1, 1.0);
	rig.play(9600, l, r); // the profile is computed meanwhile (the service's thread)
	const OscVoice& osc = newest(rig);
	const uint32_t resets = osc.partialsResetsForTest();
	const uint32_t profiled = osc.profileNoteOnsForTest();
	const int slot = rig.synth.newestVoiceSlotForTest();
	rig.synth.noteOn(0, 55, 2, 1.0); // legato over the held note
	rig.play(4800, l, r);
	REQUIRE(rig.synth.newestVoiceSlotForTest() == slot);
	CHECK(newest(rig).partialsResetsForTest() == resets);
	CHECK(newest(rig).profileNoteOnsForTest() == profiled);
}

TEST_CASE("MEASUREMENT A-PH: modulation upstream of the bake — a held note's crest drift, and the optimiser thread's load and hit rate" * doctest::skip()) {
	// AN LFO ON THE SOUND'S OFFSET (upstream of the bake, so the table re-bakes every block): 0.5 Hz, ±0.5 of an offset unit,
	// over four seconds, on the default patch at C4, Headroom. Each block: the bank takes the moved character (a new build
	// serial), and the request is posted as Synth::postPhaseRequest posts it, to a PhaseService whose thread runs.
	const Character base = aphb::phased(1.0);
	feathers::PhaseService service;
	// HOW MANY ROUNDS: the default patch's profile crest at 100, 300 and 400 (the service runs 300).
	{
		auto probe = voiceOn(base, 60.0);
		const auto [amp, count] = aphb::postedSpectrum(probe->bank.current(0));
		feathers::partials::PhaseOptimiser opt;
		for (const int rounds : { 100, 300, 400 }) {
			feathers::partials::PhaseProfile prof;
			const auto t0 = std::chrono::steady_clock::now();
			opt.optimise(amp.data(), count, prof, rounds);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			MESSAGE(rounds << " rounds: profile crest " << prof.crestDb << " dB, " << ms << " ms (" << count << " partials)");
		}
	}
	service.start();
	auto v = voiceOn(base, 60.0);
	// THE NOTE-ON'S PROFILE, computed first so the held note starts optimised.
	aphb::publishFor(const_cast<feathers::partials::PhaseProfileStore&>(service.profiles()), v->bank.current(0));
	v->voice.setPhaseProfiles(&service.profiles());
	const int block = 256;
	const int blocks = static_cast<int>(4.0 * kSampleRate / block);
	std::vector<float> y;
	y.reserve(static_cast<size_t>(blocks * block));
	uint64_t lastSerial = 0;
	int noteOns = 0, profiled = 0;
	// PACED IN REAL TIME, as a host calls the audio thread: the thread's load is a wall-clock question (an offline loop would
	// post 750 requests in a few milliseconds and measure nothing).
	const auto start = std::chrono::steady_clock::now();
	std::vector<float> chunk(static_cast<size_t>(block));
	for (int b = 0; b < blocks; ++b) {
		std::this_thread::sleep_until(start + std::chrono::microseconds(static_cast<int64_t>(1e6 * b * block / kSampleRate)));
		Character c = base;
		const double t = static_cast<double>(b * block) / kSampleRate;
		c.sounds[0].phaseOffset = base.sounds[0].phaseOffset + 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 0.5 * t));
		v->bank.setCharacter(c);
		const PreparedCharacter& table = v->bank.current(0);
		if (table.buildSerial != lastSerial) {
			lastSerial = table.buildSerial;
			const auto [amp, count] = aphb::postedSpectrum(table);
			service.post(table.buildSerial, amp.data(), count);
		}
		v->render(chunk.data(), block);
		y.insert(y.end(), chunk.begin(), chunk.end());
		// A FRESH NOTE-ON EVERY QUARTER SECOND on a second voice: does the latest profile fit the note it would start?
		if (b % static_cast<int>(0.25 * kSampleRate / block) == 0 && b > 0) {
			auto probe = voiceOn(c, 60.0);
			probe->voice.setPhaseProfiles(&service.profiles());
			(void)renderMono(*probe, 64);
			++noteOns;
			profiled += static_cast<int>(probe->voice.profileNoteOnsForTest());
		}
	}
	service.stop();
	// THE HELD NOTE'S CREST, in 0.1 s windows: the first against the worst.
	const size_t win = static_cast<size_t>(0.1 * kSampleRate);
	double first = 0.0, worst = 0.0;
	for (size_t at = 0; at + win <= y.size(); at += win) {
		double sum = 0.0, peak = 0.0;
		for (size_t n = at; n < at + win; ++n) {
			sum += double(y[n]) * double(y[n]);
			peak = std::max(peak, std::abs(double(y[n])));
		}
		const double crest = 20.0 * std::log10(peak / std::sqrt(sum / double(win)));
		if (at == win) {
			first = crest; // the second window: past the note-on's own transient
		}
		worst = std::max(worst, crest);
	}
	MESSAGE("held note under the LFO: crest " << first << " dB just after note-on, worst window " << worst << " dB");
	MESSAGE("optimiser: " << service.requests().postedForTest() << " requests posted, " << service.requests().takenForTest()
	                      << " taken (the rest overwritten by newer ones), " << service.computedForTest() << " computed, longest "
	                      << service.longestMsForTest() << " ms");
	MESSAGE("fresh note-ons during the sweep: " << profiled << " of " << noteOns << " took a profile");
	// THE SAME SWEEP UNDER NATURAL (the closed form at note-on), for comparison.
	{
		auto n = voiceOn(aphb::phased(0.0), 60.0);
		std::vector<float> z;
		for (int b = 0; b < blocks; ++b) {
			Character c = aphb::phased(0.0);
			const double t = static_cast<double>(b * block) / kSampleRate;
			c.sounds[0].phaseOffset += 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 0.5 * t));
			n->bank.setCharacter(c);
			n->render(chunk.data(), block);
			z.insert(z.end(), chunk.begin(), chunk.end());
		}
		double nFirst = 0.0, nWorst = 0.0;
		for (size_t at = 0; at + win <= z.size(); at += win) {
			double sum = 0.0, peak = 0.0;
			for (size_t m = at; m < at + win; ++m) {
				sum += double(z[m]) * double(z[m]);
				peak = std::max(peak, std::abs(double(z[m])));
			}
			const double crest = 20.0 * std::log10(peak / std::sqrt(sum / double(win)));
			nFirst = at == win ? crest : nFirst;
			nWorst = std::max(nWorst, crest);
		}
		MESSAGE("the same sweep under Natural: crest " << nFirst << " dB just after note-on, worst window " << nWorst << " dB");
	}
	CHECK(service.computedForTest() > 0);
}

TEST_CASE("MEASUREMENT A-PH: the closeness bound — what a profile made for a nearby spectrum is worth, by distance" * doctest::skip()) {
	// A PROFILE OPTIMISED FOR THE DEFAULT PATCH (offset +0), used on the spectra the offset reaches as it moves away: for each
	// step, the distance the voice would compute, and the crest of the moved spectrum under that stale profile, under the
	// closed form (what the voice falls back to), and under its own optimum. The bound should sit where the stale profile
	// stops beating the closed form.
	namespace pp = feathers::partials;
	const Character base = aphb::phased(1.0);
	auto v0 = voiceOn(base, 60.0);
	const auto [a0, n0] = aphb::postedSpectrum(v0->bank.current(0));
	pp::PhaseOptimiser opt;
	pp::PhaseProfile stale;
	opt.optimise(a0.data(), n0, stale, feathers::PhaseService::kRounds);
	const auto crestDb = [](const float* amp, const float* phase, int count) {
		double e = 0.0;
		for (int k = 0; k < count; ++k) {
			e += 0.5 * double(amp[k]) * double(amp[k]);
		}
		return 20.0 * std::log10(pp::periodPeak(amp, phase, count, 2048) / std::sqrt(e));
	};
	for (const float delta : { 0.0f, 0.02f, 0.05f, 0.1f, 0.2f, 0.3f, 0.5f, 0.8f }) {
		Character c = base;
		c.sounds[0].phaseOffset += delta;
		auto v = voiceOn(c, 60.0);
		const auto [a, n] = aphb::postedSpectrum(v->bank.current(0));
		const float d = pp::profileDistance(stale, a.data(), n);
		std::array<float, kMaxPartials> closed {};
		pp::schroederPhases(a.data(), n, closed.data());
		pp::PhaseProfile own;
		opt.optimise(a.data(), n, own, feathers::PhaseService::kRounds);
		MESSAGE("offset +" << delta << ": distance " << d << " | crest: stale profile " << crestDb(a.data(), stale.phase.data(), n)
		                   << " dB, closed form " << crestDb(a.data(), closed.data(), n) << " dB, own optimum " << own.crestDb << " dB");
	}
}

TEST_CASE("MEASUREMENT A-PH3: the hit rate across the keyboard — a profile sketched at the last note (C4), notes C1..C7" * doctest::skip()) {
	// THE ONE SLOT: the synth posts the spectrum after the drawer at the last note's pitch; a note elsewhere takes the profile
	// only if its own first-block spectrum is within the closeness bound. Measured here by the voice's own road
	// (sketchTargets), Headroom, the default patch, for four drawers.
	namespace pp = feathers::partials;
	const Character c = aphb::phased(1.0);
	auto v = voiceOn(c, 60.0);
	const PreparedCharacter& table = v->bank.current(0);
	struct Case {
		const char* name;
		sp::ShaperParams drawer;
	};
	std::vector<Case> cases;
	cases.push_back({ "neutral drawer", {} });
	{
		sp::ShaperParams d;
		d.oddEven.on = true;
		d.oddEven.balance = 0.8f;
		d.comb.on = true;
		d.comb.spacing = 3.0f;
		d.comb.depth = 0.7f;
		cases.push_back({ "static drawer (Odd/Even, Comb: harmonic-indexed)", d });
	}
	{
		sp::ShaperParams d;
		d.filter.on = true;
		d.filter.cutoffHz = 1500.0f;
		d.filter.keyTrack = 1.0f;
		d.filter.resonance = 0.5f;
		cases.push_back({ "keytracked filter (LP 1.5 kHz at C4, Key Track 1)", d });
	}
	{
		sp::ShaperParams d;
		d.filter.on = true;
		d.filter.cutoffHz = 1500.0f;
		d.filter.keyTrack = 0.0f;
		d.filter.resonance = 0.5f;
		cases.push_back({ "fixed filter (LP 1.5 kHz, no Key Track)", d });
	}
	pp::PhaseOptimiser opt;
	for (const Case& k : cases) {
		const auto [a4, n4] = aphb::postedSpectrum(table, 60.0, k.drawer);
		pp::PhaseProfile prof;
		opt.optimise(a4.data(), n4, prof, feathers::PhaseService::kRounds);
		std::string row;
		int hits = 0, notes = 0;
		for (int key = 24; key <= 96; key += 12) {
			const auto [a, n] = aphb::postedSpectrum(table, double(key), k.drawer);
			const float d = pp::profileDistance(prof, a.data(), n);
			row += "C" + std::to_string(key / 12 - 1) + " " + std::to_string(d).substr(0, 5) + (d < pp::kProfileTolerance ? "* " : "  ");
			hits += d < pp::kProfileTolerance ? 1 : 0;
			++notes;
		}
		MESSAGE(k.name << ": " << hits << " of " << notes << " octaves hit | " << row);
	}
}

// ---- F2 (additive design §3.6): the Bell, the second filter and the routing, through the drawer ------------------------

TEST_CASE("F2 drawer: filter 2, each Gain and the routing reach the voice's drawer through their own params") {
	SynthRig rig(OscType::Additive);
	soloA(rig);
	activeDrawer(rig, 0);
	rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::Bell));
	rig.set(dp(0, SpectralControl::FilterGain), 7.5);
	rig.set(dp(0, SpectralControl::Filter2On), 1.0);
	rig.set(dp(0, SpectralControl::Filter2Mode), static_cast<double>(sp::FilterMode::BandPass));
	rig.set(dp(0, SpectralControl::Filter2Cutoff), std::log2(900.0));
	rig.set(dp(0, SpectralControl::Filter2Resonance), 0.3);
	rig.set(dp(0, SpectralControl::Filter2Slope), 0.6);
	rig.set(dp(0, SpectralControl::Filter2KeyTrack), 0.4);
	rig.set(dp(0, SpectralControl::Filter2Gain), -5.0); // not a Bell: carried, and read by nothing
	playNote(rig);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.filter.mode == sp::FilterMode::Bell);
	CHECK(d.filter.gainDb == 7.5f);
	REQUIRE(d.filter2.on);
	CHECK(d.filter2.mode == sp::FilterMode::BandPass);
	CHECK(d.filter2.cutoffHz == static_cast<float>(std::exp2(std::log2(900.0))));
	CHECK(d.filter2.resonance == 0.3f);
	CHECK(d.filter2.slope == 0.6f);
	CHECK(d.filter2.keyTrack == 0.4f);
	CHECK(d.filter2.gainDb == 0.0f); // only a Bell's Gain is blended into the op
	CHECK(d.filter2.mix == 1.0f);
	CHECK(d.parallel == 0.0f);
	CHECK(d.filterPresence[0] == 1.0f);
	CHECK(d.filterPresence[1] == 1.0f);
	rig.set(dp(0, SpectralControl::FilterRouting), 1.0);
	playNote(rig);
	CHECK(newest(rig).drawerForTest(0).parallel == 1.0f);
	// AND THE VOICE PLAYS IT: the amplitudes are the shaper's over the partials with both filters, routed in parallel (the shaper's
	// own cases pin parallelGain; this pins that the drawer reaches it) — a Series drawer differs.
	rig.set(dp(0, SpectralControl::FilterRouting), 0.0);
	playNote(rig);
	const std::vector<float> series = amplitudes(newest(rig));
	rig.set(dp(0, SpectralControl::FilterRouting), 1.0);
	playNote(rig);
	CHECK(amplitudes(newest(rig)) != series);
}

TEST_CASE("F2 drawer: filter 2 off, Series and Gain 0 are the old drawer to the bit — whatever filter 2's other controls say") {
	const auto play = [](bool touchF2) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		activeDrawer(rig, 0);
		if (touchF2) {
			rig.set(dp(0, SpectralControl::FilterGain), 12.0); // filter 1 is a Low Pass: Gain is inert
			rig.set(dp(0, SpectralControl::Filter2Mode), static_cast<double>(sp::FilterMode::Bell));
			rig.set(dp(0, SpectralControl::Filter2Cutoff), std::log2(300.0));
			rig.set(dp(0, SpectralControl::Filter2Resonance), 0.8);
			rig.set(dp(0, SpectralControl::Filter2Gain), -12.0);
			rig.set(dp(0, SpectralControl::FilterRouting), 1.0); // inert with one filter
		}
		playNote(rig);
		return std::pair { newest(rig).drawerForTest(0), amplitudes(newest(rig)) };
	};
	const auto [before, a] = play(false);
	const auto [after, b] = play(true);
	CHECK_FALSE(after.filter2.on);
	CHECK(after.parallel == 0.0f); // the routing of a sound with one filter does not reach the blend (spectralRoutingInert)
	CHECK(after.filter == before.filter);
	CHECK(a == b); // the voice's amplitudes, value for value
	// AND A FRESH SOUND'S DRAWER HAS FILTER 2 OFF AND SERIES: the params' defaults.
	SynthRig fresh(OscType::Additive);
	playNote(fresh);
	CHECK_FALSE(newest(fresh).drawerForTest(0).filter2.on);
	CHECK(newest(fresh).drawerForTest(0).parallel == 0.0f);
}

TEST_CASE("F2 drawer morph: filter 2 blends as filter 1 does, each side's presence is its share, and the routing blends by the sounds that route") {
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4); // both sounds in the blend
	for (size_t s : { size_t { 0 }, size_t { 1 } }) {
		rig.set(dp(s, SpectralControl::FilterOn), 1.0);
		rig.set(dp(s, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::BandPass));
		rig.set(dp(s, SpectralControl::FilterCutoff), std::log2(400.0));
	}
	// A: both filters, Parallel, filter 2 a Bell at +6 dB. B: filter 1 only (its routing set, and inert).
	rig.set(dp(0, SpectralControl::Filter2On), 1.0);
	rig.set(dp(0, SpectralControl::Filter2Mode), static_cast<double>(sp::FilterMode::Bell));
	rig.set(dp(0, SpectralControl::Filter2Cutoff), std::log2(3000.0));
	rig.set(dp(0, SpectralControl::Filter2Gain), 6.0);
	rig.set(dp(0, SpectralControl::FilterRouting), 1.0);
	rig.set(dp(1, SpectralControl::FilterRouting), 1.0);
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	REQUIRE(wa > 0.0);
	REQUIRE(wb > 0.0);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	REQUIRE(d.filter2.on);
	CHECK(d.filter2.mode == sp::FilterMode::Bell);
	CHECK(d.filter2.gainDb == doctest::Approx(6.0));
	CHECK(static_cast<double>(d.filter2.mix) == doctest::Approx(wa / (wa + wb)).epsilon(1e-6));
	CHECK(d.filterPresence[0] == 1.0f); // every sound's filter 1 is on: the literal 1
	CHECK(static_cast<double>(d.filterPresence[1]) == doctest::Approx(wa / (wa + wb)).epsilon(1e-6));
	// ONLY A ROUTES (B has one filter, so its routing is inert and weighs nothing): r = 1.
	CHECK(d.parallel == 1.0f);
	// B GAINS FILTER 2 IN SERIES: now both sounds route, A in Parallel and B in Series, so r is A's share of them.
	rig.set(dp(1, SpectralControl::Filter2On), 1.0);
	rig.set(dp(1, SpectralControl::FilterRouting), 0.0);
	playNote(rig);
	const auto [wa2, wb2] = abWeights(rig);
	CHECK(static_cast<double>(newest(rig).drawerForTest(0).parallel) == doctest::Approx(wa2 / (wa2 + wb2)).epsilon(1e-6));
}

TEST_CASE("F2 drawer: a Bell at 0 dB is open — no filter, its Res, Slope and Key move nothing — and in Parallel an open filter is a wire") {
	const auto drawer = [](double gain, double res) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		activeDrawer(rig, 0);
		rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::Bell));
		rig.set(dp(0, SpectralControl::FilterGain), gain);
		rig.set(dp(0, SpectralControl::FilterResonance), res);
		playNote(rig);
		return newest(rig).drawerForTest(0);
	};
	CHECK_FALSE(drawer(0.0, 0.2).filter.on);
	CHECK(drawer(0.0, 0.2) == drawer(0.0, 0.9));
	CHECK(drawer(3.0, 0.2).filter.on);
	CHECK(drawer(3.0, 0.2).filter.gainDb == 3.0f);
	// THE PANEL'S RULE IS THE ENGINE'S: Res, Slope and Key inert at 0 dB, Gain live in Bell and inert in every other mode.
	for (const SpectralControl c : { SpectralControl::FilterResonance, SpectralControl::FilterSlope, SpectralControl::FilterKeyTrack }) {
		CHECK(feathers::spectralControlInert(c, 4, false, false, true));
		CHECK_FALSE(feathers::spectralControlInert(c, 4, false, false, false));
	}
	CHECK_FALSE(feathers::spectralControlInert(SpectralControl::FilterGain, 4, false, false, true));
	CHECK(feathers::spectralControlInert(SpectralControl::Filter2Gain, 0, false, false, false));
	CHECK(feathers::spectralRoutingInert(true, false));
	CHECK_FALSE(feathers::spectralRoutingInert(true, true));
	// A LOW PASS 2 AT ITS TOP IN PARALLEL WITH A BAND-PASS: a wire beside the band-pass, so the voice's partials pass at ~unity;
	// in Series the band-pass alone.
	const auto amps = [](double routing) {
		SynthRig rig(OscType::Additive);
		soloA(rig);
		rig.set(dp(0, SpectralControl::FilterOn), 1.0);
		rig.set(dp(0, SpectralControl::FilterMode), static_cast<double>(sp::FilterMode::BandPass));
		rig.set(dp(0, SpectralControl::FilterCutoff), std::log2(2000.0));
		rig.set(dp(0, SpectralControl::Filter2On), 1.0);
		rig.set(dp(0, SpectralControl::Filter2Cutoff), std::log2(sp::kFilterOpenHighHz));
		rig.set(dp(0, SpectralControl::FilterRouting), routing);
		playNote(rig);
		const sp::ShaperParams d = newest(rig).drawerForTest(0);
		std::vector<float> g(static_cast<size_t>(kMaxPartials));
		std::vector<float> k(g.size()), hz(g.size()), base(g.size(), 1.0f);
		for (size_t i = 0; i < g.size(); ++i) {
			k[i] = static_cast<float>(i + 1);
			hz[i] = static_cast<float>((i + 1) * 110.0);
		}
		sp::gains(sp::prepare(d, 110.0), k.data(), hz.data(), base.data(), static_cast<int>(g.size()), g.data());
		return g;
	};
	const std::vector<float> series = amps(0.0);
	const std::vector<float> parallel = amps(1.0);
	CHECK(series[0] < 0.1f);               // 110 Hz, far below the band-pass at 2 kHz
	CHECK(parallel[0] == doctest::Approx(1.0f)); // the wire passes it
}

// ---- OH (backlog): the Partials operator's schemes — Peaks (the N loudest) beside Harmonics (the lowest N) ----

TEST_CASE("OH Partials: Peaks keeps the N loudest partials (the weakest kept at N's fraction) and silences the rest; Harmonics the lowest N") {
	// A SPECTRUM WHOSE LOUDNESS ORDER IS NOT ITS INDEX ORDER: partial i's amplitude is envAtRatio·fine, set so the four loudest
	// are partials 7, 2, 11 and 4, in that order, every other one quieter.
	PartialSpectrum spec {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		spec.ratio[u] = static_cast<float>(i + 1);
		spec.envAtRatio[u] = 1.0f;
		spec.fine[u] = 0.01f / static_cast<float>(i + 1);
	}
	spec.fine[7] = 0.9f;
	spec.fine[2] = 0.8f;
	spec.fine[11] = 0.7f;
	spec.fine[4] = 0.6f;
	sp::ShaperParams drawer {};
	drawer.partialCount = { true, 3.5f, 1.0f }; // all Peaks
	alignas(16) std::array<float, kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	constexpr double kF0 = 110.0;
	const feathers::PartialPitches peaks =
	    feathers::pitchPartials(spec, drawer, kF0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
	REQUIRE(peaks.edge != nullptr);
	CHECK(peaks.count > 100); // every partial below Nyquist renders: Peaks is a timbre control, not the CPU knob
	for (int i = 0; i < peaks.count; ++i) {
		CAPTURE(i);
		const float want = (i == 7 || i == 2 || i == 11) ? 1.0f : i == 4 ? 0.5f : 0.0f;
		CHECK(peaks.edge[i] == want);
	}
	// HARMONICS, THE SAME COUNT: the lowest three whole and the fourth at a half, whatever their loudness — the operator as it was.
	drawer.partialCount.peaks = 0.0f;
	const feathers::PartialPitches lowest =
	    feathers::pitchPartials(spec, drawer, kF0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
	CHECK(lowest.count == 4);
	REQUIRE(lowest.edge != nullptr);
	CHECK(lowest.edge[0] == 1.0f);
	CHECK(lowest.edge[2] == 1.0f);
	CHECK(lowest.edge[3] == 0.5f);
	// AND A COUNT OF 1 IN PEAKS KEEPS THE ONE LOUDEST.
	drawer.partialCount = { true, 1.0f, 1.0f };
	const feathers::PartialPitches one =
	    feathers::pitchPartials(spec, drawer, kF0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
	for (int i = 0; i < one.count; ++i) {
		CHECK(one.edge[i] == (i == 7 ? 1.0f : 0.0f));
	}
}

TEST_CASE("OH drawer: the Partials scheme reaches the voice's drawer by its own param, a fresh sound is Harmonics, the count goes down to 1, and Peaks silences the culled partials") {
	using SC = SpectralControl;
	SynthRig fresh(OscType::Additive);
	soloA(fresh);
	fresh.set(dp(0, SC::PartialsOn), 1.0);
	playNote(fresh);
	CHECK(newest(fresh).drawerForTest(0).partialCount.peaks == 0.0f);
	// THE FLOOR IS 1 (it was 8 on the drawer: the param's MINIMUM; the default stays 64).
	const feathers::ParamDesc* count = feathers::findParam(static_cast<clap_id>(dp(0, SC::PartialsCount)));
	REQUIRE(count != nullptr);
	CHECK(count->min == 1.0);
	CHECK(count->def == 64.0);
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(dp(0, SC::PartialsOn), 1.0);
	rig.set(dp(0, SC::PartialsCount), 1.0);
	rig.set(dp(0, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Peaks));
	playNote(rig);
	const sp::ShaperParams d = newest(rig).drawerForTest(0);
	CHECK(d.partialCount.peaks == 1.0f);
	CHECK(d.partialCount.count == 1.0f);
	// THE VOICE: exactly one partial sounds, and it is the loudest of the drawer-off spectrum.
	SynthRig off(OscType::Additive);
	soloA(off);
	playNote(off);
	const std::vector<float> all = amplitudes(newest(off));
	const std::vector<float> kept = amplitudes(newest(rig));
	const auto loudest = static_cast<size_t>(std::max_element(all.begin(), all.end(),
	                                                          [](float a, float b) { return std::fabs(a) < std::fabs(b); })
	                                         - all.begin());
	int sounding = 0;
	for (size_t i = 0; i < kept.size(); ++i) {
		sounding += kept[i] != 0.0f ? 1 : 0;
	}
	CHECK(sounding == 1);
	CHECK(kept[loudest] != 0.0f);
}

// ---- P1b (2026-10-19): schemes blend in a morph; Peaks ranks both ears together ---------------------------------------

TEST_CASE("P1b Partials: between the schemes the keep masks blend by Peaks' share, so a morph never steps") {
	PartialSpectrum spec {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		spec.ratio[u] = static_cast<float>(i + 1);
		spec.envAtRatio[u] = 1.0f;
		spec.fine[u] = 0.01f / static_cast<float>(i + 1);
	}
	spec.fine[7] = 0.9f;
	spec.fine[2] = 0.8f;
	spec.fine[11] = 0.7f;
	spec.fine[4] = 0.6f;
	alignas(16) std::array<float, kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	const auto masks = [&](float share) {
		sp::ShaperParams drawer {};
		drawer.partialCount = { true, 3.5f, share };
		const feathers::PartialPitches p =
		    feathers::pitchPartials(spec, drawer, 110.0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
		std::vector<float> out(static_cast<size_t>(kMaxPartials), 0.0f);
		for (int i = 0; i < p.count; ++i) {
			out[static_cast<size_t>(i)] = p.edge != nullptr ? p.edge[i] : 1.0f;
		}
		return out;
	};
	const std::vector<float> harmonics = masks(0.0f);
	const std::vector<float> peaks = masks(1.0f);
	// THE RESTATEMENT: (1 − s)·Harmonics' mask + s·Peaks', partial by partial.
	for (const float s : { 0.3f, 0.75f }) {
		CAPTURE(s);
		const std::vector<float> mixed = masks(s);
		for (size_t i = 0; i < mixed.size(); ++i) {
			CAPTURE(i);
			CHECK(mixed[i] == doctest::Approx((1.0f - s) * harmonics[i] + s * peaks[i]).epsilon(1e-6));
		}
	}
	// NO STEP AT EITHER END: a hair of Peaks is Harmonics within that hair, a hair short of all Peaks is Peaks within it.
	const std::vector<float> nearH = masks(1e-6f);
	const std::vector<float> nearP = masks(1.0f - 1e-6f);
	for (size_t i = 0; i < nearH.size(); ++i) {
		CHECK(std::abs(nearH[i] - harmonics[i]) <= 2e-6f);
		CHECK(std::abs(nearP[i] - peaks[i]) <= 2e-6f);
	}
	CHECK(harmonics[7] == 0.0f); // not vacuous: the two masks differ where the blend is checked
	CHECK(peaks[7] == 1.0f);
}

TEST_CASE("P1b drawer morph: Peaks' share is its weight among the sounds whose Partials are on") {
	using SC = SpectralControl;
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4); // both sounds in the blend
	for (size_t s : { size_t { 0 }, size_t { 1 } }) {
		rig.set(dp(s, SC::PartialsOn), 1.0);
		rig.set(dp(s, SC::PartialsCount), 6.0);
	}
	rig.set(dp(0, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Peaks));
	rig.set(dp(1, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Harmonics));
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	REQUIRE(wa > 0.0);
	REQUIRE(wb > 0.0);
	CHECK(static_cast<double>(newest(rig).drawerForTest(0).partialCount.peaks) == doctest::Approx(wa / (wa + wb)).epsilon(1e-6));
	rig.set(dp(1, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Peaks));
	playNote(rig);
	CHECK(newest(rig).drawerForTest(0).partialCount.peaks == 1.0f); // unanimous: the literal 1
}

TEST_CASE("P1b Partials: Peaks ranks on both ears' energy together, so a wide patch keeps the same partials in each, chosen by both") {
	// pitchPartials ITSELF: the left ear's loudest is partial 7, the right ear's partial 20 by far. Alone the left ranks 7 first;
	// with the right ear given, the two ears' energy √(aL² + aR²) ranks 20 first.
	PartialSpectrum left {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		left.ratio[u] = static_cast<float>(i + 1);
		left.envAtRatio[u] = 1.0f;
		left.fine[u] = 0.01f / static_cast<float>(i + 1);
	}
	PartialSpectrum right = left;
	left.fine[7] = 0.9f;
	right.fine[20] = 2.0f;
	sp::ShaperParams drawer {};
	drawer.partialCount = { true, 1.0f, 1.0f };
	alignas(16) std::array<float, kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	const feathers::PartialPitches alone =
	    feathers::pitchPartials(left, drawer, 110.0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
	CHECK(alone.edge[7] == 1.0f);
	CHECK(alone.edge[20] == 0.0f);
	const feathers::PartialPitches both = feathers::pitchPartials(left, drawer, 110.0, kSampleRate, inc.data(), hz.data(), fade.data(),
	                                                              ratio.data(), edge.data(), 0, &right);
	CHECK(both.edge[20] == 1.0f);
	CHECK(both.edge[7] == 0.0f);
	// AND THE VOICE HANDS IT THE RIGHT EAR: a wide patch (each ear its own spectrum) keeps, in both ears, the N partials loudest
	// by the two ears' drawer-off energy together.
	const auto wide = [](bool partials, size_t n) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Width, 1.0);
		rig.set(ParamId::AWidth, 1.0);
		rig.set(ParamId::BWidth, 1.0);
		if (partials) {
			for (size_t s : { size_t { 0 }, size_t { 1 } }) {
				rig.set(dp(s, SpectralControl::PartialsOn), 1.0);
				rig.set(dp(s, SpectralControl::PartialsCount), static_cast<double>(n));
				rig.set(dp(s, SpectralControl::PartialsMode), static_cast<double>(sp::PartialsScheme::Peaks));
			}
		}
		playNote(rig);
		REQUIRE(rig.synth.mainBankForTest().stereoActive());
		return std::pair { amplitudes(newest(rig), 0), amplitudes(newest(rig), 1) };
	};
	const auto [l0, r0] = wide(false, 0);
	const auto topBy = [](const std::vector<float>& score, size_t n) {
		std::vector<size_t> idx(score.size());
		for (size_t i = 0; i < idx.size(); ++i) {
			idx[i] = i;
		}
		std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return score[a] > score[b]; });
		idx.resize(n);
		std::sort(idx.begin(), idx.end());
		return idx;
	};
	std::vector<float> energy(l0.size()), leftOnly(l0.size());
	for (size_t i = 0; i < l0.size(); ++i) {
		energy[i] = std::sqrt(l0[i] * l0[i] + r0[i] * r0[i]);
		leftOnly[i] = std::fabs(l0[i]);
	}
	// A COUNT WHERE THE TWO RANKINGS DISAGREE, so the case can tell them apart.
	size_t n = 0;
	for (size_t k = 2; k <= 40 && n == 0; ++k) {
		n = topBy(energy, k) != topBy(leftOnly, k) ? k : 0;
	}
	REQUIRE(n > 0);
	CAPTURE(n);
	const auto [l1, r1] = wide(true, n);
	std::vector<size_t> keptL, keptR;
	for (size_t i = 0; i < l1.size(); ++i) {
		if (l1[i] != 0.0f) {
			keptL.push_back(i);
		}
		if (r1[i] != 0.0f) {
			keptR.push_back(i);
		}
	}
	CHECK(keptL == keptR);
	CHECK(keptL == topBy(energy, n));
}

// ---- PAL (the Additive engine's palette, 2026-10-19): the terrain's timbre corner, MEASURED ------------------------------

#include <cstdio>
#include <cstdlib>

namespace {

/// One position's timbre descriptors, over the terrain's own spectrum (partial i at amplitude envAtRatio·fine, ratio r), before
/// the Shape blend, the drawer and the voice.
struct Timbre {
	double centroid = 0.0;    ///< Σ r·a² / Σ a², in harmonics
	double flatness = 0.0;    ///< geometric over arithmetic mean of a² over partials 1..64 (a floor of 1e-12): 0 tonal-peaky, 1 flat
	double oddEvenDb = 0.0;   ///< 10·log10(Σ odd a² / Σ even a²) over partials 1..320
	double inharmonic = 0.0;  ///< Σ a²·|r − k| / Σ a²: the power-weighted mean drift off the harmonic grid, in harmonics
	double effective = 0.0;   ///< the participation ratio (Σ a²)² / Σ a⁴: how many partials carry the energy
	int formantPeaks = 0;     ///< local maxima of the envelope (log-k table, k 1..320) standing ≥ 3 dB above both neighbouring minima
	double above7Db = 0.0;    ///< 10·log10 of the energy in partials 8..320 over the total
	int lowPartials = 0;      ///< how many of partials 2..7 stand within 12 dB of the loudest partial
};

Timbre timbreOf(const PartialSpectrum& s) {
	Timbre t;
	double p = 0.0, pr = 0.0, p2 = 0.0, odd = 0.0, even = 0.0, drift = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		const double a = static_cast<double>(s.envAtRatio[u]) * static_cast<double>(s.fine[u]);
		const double e = a * a;
		p += e;
		p2 += e * e;
		pr += e * static_cast<double>(s.ratio[u]);
		drift += e * std::abs(static_cast<double>(s.ratio[u]) - (i + 1));
		((i + 1) % 2 == 1 ? odd : even) += e;
	}
	t.centroid = p > 0.0 ? pr / p : 0.0;
	t.inharmonic = p > 0.0 ? drift / p : 0.0;
	double high = 0.0, loudest = 0.0;
	std::array<double, 8> low {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		const double a = static_cast<double>(s.envAtRatio[u]) * static_cast<double>(s.fine[u]);
		loudest = std::max(loudest, a * a);
		if (i >= 7) {
			high += a * a;
		}
		else {
			low[u] = a * a;
		}
	}
	t.above7Db = p > 0.0 && high > 0.0 ? 10.0 * std::log10(high / p) : -200.0;
	for (size_t i = 1; i < 7; ++i) {
		t.lowPartials += loudest > 0.0 && low[i] >= loudest * std::pow(10.0, -1.2) ? 1 : 0;
	}
	t.effective = p2 > 0.0 ? p * p / p2 : 0.0;
	t.oddEvenDb = even > 0.0 && odd > 0.0 ? 10.0 * std::log10(odd / even) : (odd > 0.0 ? 60.0 : -60.0);
	double logSum = 0.0, sum = 0.0;
	for (int i = 0; i < 64; ++i) {
		const auto u = static_cast<size_t>(i);
		const double a = static_cast<double>(s.envAtRatio[u]) * static_cast<double>(s.fine[u]);
		const double e = std::max(a * a, 1e-12);
		logSum += std::log(e);
		sum += e;
	}
	t.flatness = std::exp(logSum / 64.0) / (sum / 64.0);
	// THE ENVELOPE'S PEAKS over k 1..320: the table is linear amplitude on an even log2 k grid from 2^−1 to 2^10.
	const auto at = [&](int j) { return 20.0 * std::log10(std::max(1e-9, static_cast<double>(s.envelope[static_cast<size_t>(j)]))); };
	const int first = static_cast<int>(std::ceil((0.0 - kEnvelopeLog2Min) / (kEnvelopeLog2Max - kEnvelopeLog2Min) * (kEnvelopePoints - 1)));
	const int last = static_cast<int>(std::floor((std::log2(320.0) - kEnvelopeLog2Min) / (kEnvelopeLog2Max - kEnvelopeLog2Min) * (kEnvelopePoints - 1)));
	for (int j = first + 1; j < last; ++j) {
		if (!(at(j) > at(j - 1) && at(j) >= at(j + 1))) {
			continue;
		}
		double leftMin = at(j), rightMin = at(j);
		for (int m = j - 1; m >= first && at(m) <= at(m + 1); --m) {
			leftMin = at(m);
		}
		for (int m = j + 1; m <= last && at(m) <= at(m - 1); ++m) {
			rightMin = at(m);
		}
		t.formantPeaks += (at(j) - std::max(leftMin, rightMin) >= 3.0) ? 1 : 0;
	}
	return t;
}

double percentile(std::vector<double> v, double q) {
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0 : v[static_cast<size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1) + 0.5)];
}

} // namespace

TEST_CASE("MEASUREMENT PAL: the terrain's timbre over the reachable window — centroid, flatness, odd/even, inharmonicity, effective count, formant peaks" * doctest::skip()) {
	// THE WINDOW: homes zone 0 offset −8 .. +11 (offsetWhere's reach) in 1/8 steps, at no travel and at travel on two rings (radius
	// 0.5 and 1) of eight bearings; and the same at zones 341 and 682 (other homes on the zone line). The terrain's own
	// spectrum (raw fields and detail, buildSound): no Shape blend, no knobs, no drawer.
	std::vector<Timbre> all;
	std::vector<PartialFields> fields;
	for (const uint16_t zone : { uint16_t { 0 }, uint16_t { 341 }, uint16_t { 682 } }) {
		for (int step = -8 * 8; step <= 11 * 8; ++step) {
			const float offset = static_cast<float>(step) / 8.0f;
			std::vector<std::pair<double, double>> travels { { 0.0, 0.0 } };
			for (const double r : { 0.5, 1.0 }) {
				for (int b = 0; b < 8; ++b) {
					const double th = 2.0 * std::numbers::pi * b / 8.0;
					travels.emplace_back(r * std::cos(th), r * std::sin(th));
				}
			}
			for (const auto& [tx, ty] : travels) {
				const SoundCoord s = sound(zone, offset, 1.0, tx, ty);
				PartialSpectrum spec;
				const PartialFields f = rawFields(s);
				feathers::partials::buildSound(f, rawDetail(s), spec);
				all.push_back(timbreOf(spec));
				fields.push_back(f);
			}
		}
	}
	REQUIRE(all.size() > 1000);
	const auto column = [&](double Timbre::* f) {
		std::vector<double> v;
		for (const Timbre& t : all) {
			v.push_back(t.*f);
		}
		return v;
	};
	const auto report = [&](const char* name, const std::vector<double>& v) {
		MESSAGE(std::string(name) << ": p5 " << percentile(v, 0.05) << "  p25 " << percentile(v, 0.25) << "  p50 " << percentile(v, 0.5) << "  p75 "
		             << percentile(v, 0.75) << "  p95 " << percentile(v, 0.95) << "  (min " << percentile(v, 0.0) << ", max "
		             << percentile(v, 1.0) << ")");
	};
	MESSAGE(all.size() << " positions");
	report("centroid (harmonics)", column(&Timbre::centroid));
	report("flatness (1..64)", column(&Timbre::flatness));
	report("odd/even (dB)", column(&Timbre::oddEvenDb));
	report("inharmonicity (harmonics)", column(&Timbre::inharmonic));
	report("effective partial count", column(&Timbre::effective));
	std::vector<double> peaks;
	for (const Timbre& t : all) {
		peaks.push_back(t.formantPeaks);
	}
	report("formant peaks", peaks);
	// AND THE RAW FIELDS BEHIND THEM (before buildSound's Smoothness pull), so a corner can be traced to the field that makes it.
	for (const auto& [name, field] : std::initializer_list<std::pair<const char*, float PartialFields::*>> {
	         { "field rolloff (0 = p 0.5 .. 1 = p 2.5)", &PartialFields::rolloff },
	         { "field smoothness", &PartialFields::smoothness },
	         { "field formant1Gain", &PartialFields::formant1Gain },
	         { "field oddEven", &PartialFields::oddEven },
	         { "field combDepth", &PartialFields::combDepth },
	         { "field detail", &PartialFields::detail },
	         { "field offGrid", &PartialFields::offGrid } }) {
		std::vector<double> v;
		for (const PartialFields& f : fields) {
			v.push_back(static_cast<double>(f.*field));
		}
		report(name, v);
	}
	// THE 2-D OCCUPANCY of centroid × flatness: centroid on log2 harmonics 0..6 (1 to 64) in 12 bins, flatness 0..1 in 10 bins.
	constexpr int kCx = 12, kFy = 10;
	std::array<std::array<int, kFy>, kCx> grid {};
	for (const Timbre& t : all) {
		const int cx = std::clamp(static_cast<int>(std::log2(std::max(1.0, t.centroid)) / 6.0 * kCx), 0, kCx - 1);
		const int fy = std::clamp(static_cast<int>(t.flatness * kFy), 0, kFy - 1);
		++grid[static_cast<size_t>(cx)][static_cast<size_t>(fy)];
	}
	int occupied = 0;
	std::string rows;
	for (int fy = kFy - 1; fy >= 0; --fy) {
		rows += "\n  flat " + std::to_string(fy) + " |";
		for (int cx = 0; cx < kCx; ++cx) {
			const int n = grid[static_cast<size_t>(cx)][static_cast<size_t>(fy)];
			occupied += n > 0 ? 1 : 0;
			char cell[8];
			std::snprintf(cell, sizeof(cell), "%5d", n);
			rows += cell;
		}
	}
	MESSAGE("centroid × flatness occupancy: " << occupied << " of " << kCx * kFy << " cells (columns log2 centroid 0..6 in 12; rows "
	                                          << "flatness 0..1 in 10, top = flattest)" << rows);
	// THE DARK CORNER'S SHARE (the lowest cell: centroid under 2^0.5 harmonics, flatness under 0.1), and THE SPARSE DESCRIPTOR (PAL
	// stage 1, the lead's): positions carrying their energy in at most four partials with a centroid between harmonics 1.5 and 4.
	size_t sparse = 0;
	size_t drawbar = 0;
	for (const Timbre& t : all) {
		const bool few = t.effective <= 4.0 && t.centroid >= 1.5 && t.centroid <= 4.0;
		sparse += few ? 1 : 0;
		drawbar += few && t.above7Db <= -30.0 && t.lowPartials >= 2 ? 1 : 0;
	}
	MESSAGE("dark corner share: " << static_cast<double>(grid[0][0]) / static_cast<double>(all.size()) << "; sparse share (effective ≤ 4, "
	                              << "centroid 1.5..4): " << static_cast<double>(sparse) / static_cast<double>(all.size())
	                              << "; of which DRAWBAR-sparse (partials past 7 under −30 dB of the total, and at least two of 2..7 "
	                              << "within 12 dB of the loudest): " << static_cast<double>(drawbar) / static_cast<double>(all.size()));
	// AND A SCATTER, centroid (log) across, flatness up, written as a PPM to PAL_SHOT (convert it to PNG by hand).
	if (const char* path = std::getenv("PAL_SHOT"); path != nullptr) {
		constexpr int W = 480, H = 320, M = 30;
		std::vector<uint8_t> img(static_cast<size_t>(W * H * 3), 16);
		const auto plot = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
			if (x >= 0 && x < W && y >= 0 && y < H) {
				const auto o = static_cast<size_t>((y * W + x) * 3);
				img[o] = r;
				img[o + 1] = g;
				img[o + 2] = b;
			}
		};
		for (int x = M; x < W - 10; ++x) {
			plot(x, H - M, 90, 90, 110);
		}
		for (int y = 10; y < H - M; ++y) {
			plot(M, y, 90, 90, 110);
		}
		for (int oct = 0; oct <= 6; ++oct) { // ticks at 1, 2, 4 ... 64 harmonics
			const int x = M + static_cast<int>(oct / 6.0 * (W - M - 10));
			for (int d = 0; d < 5; ++d) {
				plot(x, H - M + d, 150, 150, 170);
			}
		}
		for (const Timbre& t : all) {
			const int x = M + static_cast<int>(std::log2(std::max(1.0, t.centroid)) / 6.0 * (W - M - 10));
			const int y = H - M - static_cast<int>(std::clamp(t.flatness, 0.0, 1.0) * (H - M - 10));
			plot(x, y, 229, 169, 122);
		}
		if (FILE* f = std::fopen(path, "wb"); f != nullptr) {
			std::fprintf(f, "P6\n%d %d\n255\n", W, H);
			std::fwrite(img.data(), 1, img.size(), f);
			std::fclose(f);
			MESSAGE("wrote " << std::string(path));
		}
	}
}

TEST_CASE("PAL sparse country exists on the reachable window: a home whose spectrum is the fundamental and a few low partials, and it glides as you travel") {
	// FOUND, NOT PLACED: the first home on the window (offsetWhere) where the sparsity field is near its peak and Smoothness leaves it.
	const float offset = offsetWhere([](const PartialFields& f) { return f.sparsity > 0.9f && f.smoothness < 0.05f; });
	CAPTURE(offset);
	const auto spectrumAt = [](float at, double tx) {
		const SoundCoord s = sound(0, at, 1.0, tx, 0.0);
		PartialSpectrum spec;
		feathers::partials::buildSound(rawFields(s), rawDetail(s), spec);
		return spec;
	};
	const PartialSpectrum spec = spectrumAt(offset, 0.0);
	double total = 0.0, above7 = 0.0, loudest = 0.0;
	std::array<double, 7> low {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const double a = static_cast<double>(spec.envAtRatio[static_cast<size_t>(i)]) * static_cast<double>(spec.fine[static_cast<size_t>(i)]);
		total += a * a;
		loudest = std::max(loudest, a * a);
		(i >= 7 ? above7 : low[static_cast<size_t>(i)]) += a * a;
	}
	CHECK(10.0 * std::log10(above7 / total) < -25.0); // past harmonic 7, next to nothing
	int strong = 0;
	for (size_t i = 0; i < low.size(); ++i) {
		strong += low[i] >= loudest * 0.063 ? 1 : 0; // within 12 dB of the loudest
	}
	CHECK(strong >= 2); // the fundamental and at least one more: a drawbar's handful, not a sine
	CHECK(strong <= 5);
	// IT GLIDES: travelling a unit across the country, the worst change of any of the first 16 partials over one step SHRINKS WITH
	// THE STEP — ten times finer, about ten times smaller — which a jump (a step that stays its size however fine you look) cannot do.
	const auto worstStep = [&](int steps) {
		double worst = 0.0;
		PartialSpectrum previous = spectrumAt(offset, 0.0);
		for (int step = 1; step <= steps; ++step) {
			const PartialSpectrum now = spectrumAt(offset, static_cast<double>(step) / steps);
			for (int i = 0; i < 16; ++i) {
				const auto u = static_cast<size_t>(i);
				worst = std::max(worst, std::abs(static_cast<double>(now.envAtRatio[u] * now.fine[u])
				                                 - static_cast<double>(previous.envAtRatio[u] * previous.fine[u])));
			}
			previous = now;
		}
		return worst;
	};
	const double coarse = worstStep(100);
	const double fine = worstStep(1000);
	MESSAGE("sparse home offset " << offset << ": worst partial step " << coarse << " at 0.01 travel, " << fine << " at 0.001");
	CHECK(fine < 0.2 * coarse);
	CHECK(fine < 0.01); // against kTargetRss 0.35
}

TEST_CASE("OI Partials: Centers keeps the fundamental and the set's few of 2..7 by the generator the terrain shares, blends with the other schemes by its share, and ignores the count") {
	PartialSpectrum spec {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		spec.ratio[u] = static_cast<float>(i + 1);
		spec.envAtRatio[u] = 1.0f;
		spec.fine[u] = 1.0f / static_cast<float>(i + 1);
	}
	alignas(16) std::array<float, kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	const auto mask = [&](float count, float peaks, float centers, float set) {
		sp::ShaperParams drawer {};
		drawer.partialCount = { true, count, peaks, centers, set };
		const feathers::PartialPitches p =
		    feathers::pitchPartials(spec, drawer, 110.0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
		std::vector<float> out(static_cast<size_t>(kMaxPartials), 0.0f);
		for (int i = 0; i < p.count; ++i) {
			out[static_cast<size_t>(i)] = p.edge != nullptr ? p.edge[i] : 1.0f;
		}
		return out;
	};
	for (const float set : { 0.0f, 0.4f, 1.7f, 5.25f }) {
		CAPTURE(set);
		// ALL CENTERS: partial i at the shared generator's weight for harmonic i + 1, and the count does not move it.
		const std::vector<float> a = mask(3.5f, 0.0f, 1.0f, set);
		const std::vector<float> b = mask(200.0f, 0.0f, 1.0f, set);
		CHECK(a == b);
		for (int i = 0; i < 40; ++i) {
			CAPTURE(i);
			CHECK(a[static_cast<size_t>(i)] == static_cast<float>(feathers::partials::sparseKeep(i + 1, set)));
		}
	}
	// A BLEND: 0.3 Centers, 0.2 Peaks, 0.5 Harmonics is those masks mixed by those shares, partial by partial.
	const std::vector<float> h = mask(5.0f, 0.0f, 0.0f, 0.9f), p = mask(5.0f, 1.0f, 0.0f, 0.9f), c = mask(5.0f, 0.0f, 1.0f, 0.9f);
	const std::vector<float> mixed = mask(5.0f, 0.2f, 0.3f, 0.9f);
	for (size_t i = 0; i < 40; ++i) {
		CAPTURE(i);
		CHECK(mixed[i] == doctest::Approx(0.5f * h[i] + 0.2f * p[i] + 0.3f * c[i]).epsilon(1e-6));
	}
}

TEST_CASE("OI drawer morph: Centers' share is its weight among the on-sounds, its set their weighted mean, and a Centers sound's count moves nothing") {
	using SC = SpectralControl;
	const auto morph = [](double centersCount) {
		SynthRig rig(OscType::Additive);
		rig.set(ParamId::Path, 0.4); // both sounds in the blend
		for (size_t s : { size_t { 0 }, size_t { 1 } }) {
			rig.set(dp(s, SC::PartialsOn), 1.0);
		}
		rig.set(dp(0, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Centers));
		rig.set(dp(0, SC::PartialsSet), 2.5);
		rig.set(dp(0, SC::PartialsCount), centersCount); // inert: A is in Centers
		rig.set(dp(1, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Harmonics));
		rig.set(dp(1, SC::PartialsCount), 6.0);
		playNote(rig);
		return std::tuple { newest(rig).drawerForTest(0), abWeights(rig) };
	};
	const auto [d, w] = morph(12.0);
	const auto [wa, wb] = w;
	CHECK(static_cast<double>(d.partialCount.centers) == doctest::Approx(wa / (wa + wb)).epsilon(1e-6));
	CHECK(d.partialCount.centersSet == 2.5f);
	CHECK(d.partialCount.count == 6.0f); // B's count alone: A's is not read
	CHECK(std::get<0>(morph(300.0)) == d); // moving A's inert count moves nothing
}

// ---- OE: the engine vocoder, the voice's half (additive design §9) -------------------------------------------------
//
// Every gain restated here from §9.2's words, never through dsp/vocode.h's reference: the grid point u of a frequency, the
// envelope linear between points in dB and held outside the Range, the calibration (2/π)·sqrt(2·f0/binHz)·10^((dB + 2.5066)/20),
// voc = S / (ā^(1−F)·s^F), Depth in log2. Read off the voice's own rotator amplitudes (values, not a buffer).

namespace oe {

constexpr double kLog2Low = 4.321928094887363; // log2(20)
constexpr double kCells = 127.0 / 10.0;

/// A grid with structure everywhere (a falling tilt with ripples), so a read at the wrong frequency is a wrong value.
sp::VocodeEnvelope rippled(double binHz = 100.0) {
	sp::VocodeEnvelope e;
	for (int g = 0; g < sp::VocodeEnvelope::kPoints; ++g) {
		e.db[static_cast<size_t>(g)] = static_cast<float>(-30.0 - 0.3 * g + 6.0 * std::sin(g / 5.0));
	}
	e.binHz = binHz;
	return e;
}

/// The envelope's dB at `hz`, held to [low, high] and the grid's ends.
double dbAt(const sp::VocodeEnvelope& e, double hz, double low = 20.0, double high = 20000.0) {
	const double held = std::clamp(hz, low, high);
	const double u = std::clamp((std::log2(held) - kLog2Low) * kCells, 0.0, 127.0);
	const int i = std::min(static_cast<int>(u), 126);
	const double f = u - i;
	return e.db[static_cast<size_t>(i)] + f * (e.db[static_cast<size_t>(i + 1)] - e.db[static_cast<size_t>(i)]);
}

/// The calibrated line amplitude (§9.2).
double lineAt(double db, double f0, double binHz) {
	return std::pow(10.0, (db + 2.5066) / 20.0) * (2.0 / std::numbers::pi) * std::sqrt(2.0 * f0 / binHz);
}

/// THE PARTIALS A VOICE RENDERS at `f0`, with their frequencies (float, as the voice computes them) and fades.
struct Partials {
	int count = 0;
	std::array<double, kMaxPartials> hz {};
	std::array<double, kMaxPartials> fade {};
};
Partials partialsOf(const PartialSpectrum& s, double f0) {
	Partials p;
	p.count = renderingCount(s, f0);
	for (int i = 0; i < p.count; ++i) {
		const double w = static_cast<double>(static_cast<float>(kTwoPi * f0 * s.ratio[i] / kSampleRate));
		p.fade[static_cast<size_t>(i)] = fadeRestated(w);
		p.hz[static_cast<size_t>(i)] = static_cast<double>(static_cast<float>(f0 * s.ratio[i]));
	}
	return p;
}

/// ā (§9.2): the RSS of shape·fade over partials [first, count), per partial.
double meanShape(const PartialSpectrum& s, const Partials& p, int first = 0) {
	double power = 0.0;
	for (int i = first; i < p.count; ++i) {
		const double a = static_cast<double>(s.envAtRatio[i] * s.fine[i]) * p.fade[static_cast<size_t>(i)];
		power += a * a;
	}
	return std::sqrt(power / (p.count - first));
}

} // namespace oe

TEST_CASE("OE: Vocode reads the sidechain's envelope at each partial's own frequency — Flatten exact, Depth in log, Gate and Range, by value") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 52.0, f0 = hzOfKey(key);
	const sp::VocodeEnvelope env = oe::rippled();
	struct Setting {
		const char* name;
		float depth, flatten, gateDb, lowHz, highHz;
	};
	for (const Setting& s : { Setting { "classic", 1.0f, 1.0f, -96.0f, 20.0f, 20000.0f },
	                          Setting { "half flat, depth 0.6", 0.6f, 0.5f, -96.0f, 20.0f, 20000.0f },
	                          Setting { "unflattened", 1.0f, 0.0f, -96.0f, 20.0f, 20000.0f },
	                          Setting { "depth past 1", 1.5f, 1.0f, -96.0f, 20.0f, 20000.0f },
	                          Setting { "gated", 1.0f, 1.0f, -60.0f, 20.0f, 20000.0f },
	                          Setting { "ranged", 1.0f, 1.0f, -96.0f, 300.0f, 2500.0f } }) {
		CAPTURE(s.name);
		auto plain = voiceOn(c, key);
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = { true, s.depth, s.flatten, s.gateDb, s.lowHz, s.highHz };
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		renderMono(*plain, 4 * kBlock);
		renderMono(*v, 4 * kBlock);
		const PartialSpectrum& spec = playing(*v);
		const oe::Partials p = oe::partialsOf(spec, f0);
		const double mean = oe::meanShape(spec, p);
		// THE PEAK Depth past 1 measures from: the envelope's highest point over the range it is read in.
		double peak = std::max(oe::dbAt(env, s.lowHz), oe::dbAt(env, s.highHz));
		for (int g = 0; g < sp::VocodeEnvelope::kPoints; ++g) {
			const double gu = (std::log2(s.lowHz) - oe::kLog2Low) * oe::kCells, hu = (std::log2(s.highHz) - oe::kLog2Low) * oe::kCells;
			if (g > gu && g < hu) {
				peak = std::max(peak, static_cast<double>(env.db[static_cast<size_t>(g)]));
			}
		}
		int checked = 0, closed = 0;
		for (int i = 0; i < p.count; ++i) {
			const float a0 = plain->voice.partialAmplitudeForTest(i, 0);
			if (!(a0 > 1e-6f)) {
				continue;
			}
			INFO("partial " << i);
			const double shape = static_cast<double>(spec.envAtRatio[i] * spec.fine[i]);
			const double db = oe::dbAt(env, p.hz[static_cast<size_t>(i)], s.lowHz, s.highHz);
			const double line = oe::lineAt(db, f0, env.binHz);
			double l = std::log2(line / (std::pow(mean, 1.0 - s.flatten) * std::pow(shape, s.flatten)));
			l = s.depth < 1.0f ? l * s.depth : l + (s.depth - 1.0) * (db - peak) / 20.0 * std::log2(10.0);
			const double want = db < s.gateDb && s.gateDb > -96.0f ? 0.0 : std::exp2(l);
			const double got = static_cast<double>(v->voice.partialAmplitudeForTest(i, 0)) / static_cast<double>(a0);
			if (want == 0.0) {
				CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f);
				++closed;
			}
			else {
				CHECK(got == doctest::Approx(want).epsilon(2e-3).scale(0.0));
			}
			++checked;
		}
		CHECK(checked > 20);
		if (s.gateDb > -96.0f) {
			CHECK(closed > 0);
			CHECK(closed < checked);
		}
	}
}

TEST_CASE("OE: Flatten 1 is exact — a vocoded partial is the sidechain's level at its frequency, whatever the terrain, and Formant Shift moves the envelope read") {
	const sp::VocodeEnvelope env = oe::rippled();
	for (const uint16_t zone : { uint16_t(40), uint16_t(120), uint16_t(400) }) {
		CAPTURE(zone);
		const Character c = additive({ sound(zone, 0.2f, 1.0) });
		const double key = 45.0, f0 = hzOfKey(key);
		for (const bool shifted : { false, true }) {
			CAPTURE(shifted);
			auto v = voiceOn(c, key);
			sp::ShaperParams d;
			d.vocode = { true, 1.0f, 1.0f, -96.0f, 20.0f, 20000.0f };
			if (shifted) {
				d.formantShift = { true, 7.0f, 7.0f, 1000.0f, true };
			}
			v->voice.setShaperParamsForTest(d);
			v->voice.setVocodeEnvelope(&env);
			renderMono(*v, 4 * kBlock);
			const PartialSpectrum& spec = playing(*v);
			const oe::Partials p = oe::partialsOf(spec, f0);
			int checked = 0;
			for (int i = 0; i < p.count; ++i) {
				if (!(spec.envAtRatio[i] * spec.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
					continue;
				}
				INFO("partial " << i);
				const double hz = p.hz[static_cast<size_t>(i)];
				const double read = shifted ? hz / sp::formantRatio(d, hz) : hz;
				CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0))
				      == doctest::Approx(oe::lineAt(oe::dbAt(env, read), f0, env.binHz)).epsilon(2e-3).scale(0.0));
				++checked;
			}
			CHECK(checked > 20);
		}
	}
}

TEST_CASE("OE: Freeze's Key Track reads the held envelope at f · frozenHz / f0 — each voice by its own note") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	sp::VocodeEnvelope env = oe::rippled();
	env.keyTrack = true;
	env.frozenHz = hzOfKey(48.0);
	for (const double key : { 48.0, 55.0, 60.0 }) {
		CAPTURE(key);
		const double f0 = hzOfKey(key);
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = { true, 1.0f, 1.0f, -96.0f, 20.0f, 20000.0f };
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		renderMono(*v, 4 * kBlock);
		const PartialSpectrum& spec = playing(*v);
		const oe::Partials p = oe::partialsOf(spec, f0);
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			if (!(spec.envAtRatio[i] * spec.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
				continue;
			}
			const double read = p.hz[static_cast<size_t>(i)] * env.frozenHz / f0;
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0))
			      == doctest::Approx(oe::lineAt(oe::dbAt(env, read), f0, env.binHz)).epsilon(2e-3).scale(0.0));
			++checked;
		}
		CHECK(checked > 20);
	}
}

TEST_CASE("OE: a silent sidechain silences the rest exactly; Vocode off, or the drawer's master off, is the voice to the bit") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	sp::ShaperParams on;
	on.vocode = { true, 1.0f, 1.0f, -96.0f, 20.0f, 20000.0f };
	// SILENT: the default envelope (every point at the silence floor), and no envelope at all, both exactly zero.
	for (const bool handed : { true, false }) {
		CAPTURE(handed);
		const sp::VocodeEnvelope silent {};
		auto v = voiceOn(c, 57.0);
		v->voice.setShaperParamsForTest(on);
		if (handed) {
			v->voice.setVocodeEnvelope(&silent);
		}
		const std::vector<float> out = renderMono(*v, 8 * kBlock);
		for (int i = 0; i < kMaxPartials; ++i) {
			CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f);
		}
		CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0.0f; }));
	}
	// THE IDENTITY: Vocode off with an envelope handed, and Vocode on under the master switch's off, render the plain voice's bits.
	const sp::VocodeEnvelope env = oe::rippled();
	auto plain = voiceOn(c, 57.0);
	const std::vector<float> want = renderMono(*plain, 16 * kBlock);
	{
		auto v = voiceOn(c, 57.0);
		v->voice.setVocodeEnvelope(&env);
		CHECK(renderMono(*v, 16 * kBlock) == want);
	}
	{
		sp::ShaperParams master = on;
		master.enabled = false;
		auto v = voiceOn(c, 57.0);
		v->voice.setShaperParamsForTest(master);
		v->voice.setVocodeEnvelope(&env);
		CHECK(renderMono(*v, 16 * kBlock) == want);
	}
	{
		sp::ShaperParams none = on;
		none.vocode.depth = 0.0f; // Depth 0: the voice as it is (§9.2)
		auto v = voiceOn(c, 57.0);
		v->voice.setShaperParamsForTest(none);
		v->voice.setVocodeEnvelope(&env);
		CHECK(renderMono(*v, 16 * kBlock) == want);
	}
}

TEST_CASE("OE: under Sub priority the sub overrides Vocode — it keeps its set level, silent sidechain or not, and Vocode shapes the rest") {
	const sp::VocodeEnvelope silent {};
	const sp::VocodeEnvelope env = oe::rippled();
	auto plain = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
	(void)renderMono(*plain, 4800);
	for (const sp::VocodeEnvelope* e : { &silent, &env }) {
		CAPTURE(e == &silent);
		auto v = voiceOn(aphb::phased(2.0, -6.0, 0.0), 60.0);
		sp::ShaperParams d;
		d.vocode = { true, 1.0f, 1.0f, -96.0f, 20.0f, 20000.0f };
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(e);
		(void)renderMono(*v, 4800);
		REQUIRE(v->voice.ceilingScaleForTest() == 1.0f);
		CHECK(v->voice.partialAmplitudeForTest(0, 0) == doctest::Approx(plain->voice.partialAmplitudeForTest(0, 0)).epsilon(1e-6).scale(0.0));
		CHECK(aphb::subAgainstShapeDb(v->voice, v->bank.current(0)) == doctest::Approx(-6.0).epsilon(1e-4).scale(0.0));
		int moved = 0;
		for (int i = 1; i < 40; ++i) {
			if (e == &silent) {
				CHECK(v->voice.partialAmplitudeForTest(i, 0) == 0.0f);
			}
			else {
				moved += v->voice.partialAmplitudeForTest(i, 0) != plain->voice.partialAmplitudeForTest(i, 0);
			}
		}
		if (e == &env) {
			CHECK(moved > 30);
		}
	}
}

// ---- OF: Partials Depth on the drawer -------------------------------------------------------------------------------------

TEST_CASE("OF Partials Depth: a culled partial plays at 1 - depth, a kept one is untouched, depth 1 is the cull as it was, and every scheme reads it") {
	PartialSpectrum spec {};
	for (int i = 0; i < kMaxPartials; ++i) {
		const auto u = static_cast<size_t>(i);
		spec.ratio[u] = static_cast<float>(i + 1);
		spec.envAtRatio[u] = 1.0f;
		spec.fine[u] = 1.0f / static_cast<float>(i + 1);
	}
	alignas(16) std::array<float, kMaxPartials> inc {}, hz {}, fade {}, ratio {}, edge {};
	const auto mask = [&](float count, float peaks, float depth, int& n) {
		sp::ShaperParams drawer {};
		drawer.partialCount = { true, count, peaks, 0.0f, 0.0f, depth };
		const feathers::PartialPitches p =
		    feathers::pitchPartials(spec, drawer, 110.0, kSampleRate, inc.data(), hz.data(), fade.data(), ratio.data(), edge.data(), 0);
		n = p.count;
		std::vector<float> out(static_cast<size_t>(kMaxPartials), 0.0f);
		for (int i = 0; i < p.count; ++i) {
			out[static_cast<size_t>(i)] = p.edge != nullptr ? p.edge[i] : 1.0f;
		}
		return out;
	};
	int n1 = 0, nd = 0;
	// HARMONICS, count 5: the first five are kept, the rest sound at 1 - depth, and the loop runs on past the count to Nyquist.
	const std::vector<float> was = mask(5.0f, 0.0f, 1.0f, n1);
	CHECK(n1 == 5); // the cull as it always was: the loop stops at the count
	for (const float depth : { 0.0f, 0.25f, 0.6f }) {
		CAPTURE(depth);
		const std::vector<float> m = mask(5.0f, 0.0f, depth, nd);
		CHECK(nd > 5);
		for (int i = 0; i < nd; ++i) {
			CAPTURE(i);
			CHECK(m[static_cast<size_t>(i)] == doctest::Approx(i < 5 ? 1.0f : 1.0f - depth).epsilon(1e-6));
		}
	}
	// A FRACTIONAL COUNT: the last kept partial carries the fraction, and depth returns the rest of it, 1 - depth*(1 - w).
	{
		const std::vector<float> m = mask(4.5f, 0.0f, 0.5f, nd);
		CHECK(m[3] == 1.0f);
		CHECK(m[4] == doctest::Approx(1.0f - 0.5f * 0.5f).epsilon(1e-6));
		CHECK(m[5] == doctest::Approx(0.5f).epsilon(1e-6));
	}
	// PEAKS reads it too: the loudest N (fine[i] falls with i, so the lowest) are kept, the rest sit at 1 - depth.
	{
		const std::vector<float> m = mask(3.0f, 1.0f, 0.4f, nd);
		for (int i = 0; i < 12; ++i) {
			CAPTURE(i);
			CHECK(m[static_cast<size_t>(i)] == doctest::Approx(i < 3 ? 1.0f : 0.6f).epsilon(1e-6));
		}
	}
	// DEPTH 1 AND A NaN ARE THE OLD ROAD: the same mask, to the bit, as a drawer that never heard of depth.
	int nn = 0;
	CHECK(mask(5.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), nn) == was);
	CHECK(nn == n1);
	sp::PartialsOp fresh {};
	CHECK(fresh.depth == 1.0f);
}

TEST_CASE("OF drawer: Partials Depth is a keyed per-sound param defaulting to 100%, and the morph blends it by weight with an off sound counting as 100%") {
	using SC = SpectralControl;
	const feathers::ParamDesc* depth = feathers::findParam(static_cast<clap_id>(dp(0, SC::PartialsDepth)));
	REQUIRE(depth != nullptr);
	CHECK(depth->def == 1.0);
	CHECK(depth->min == 0.0);
	CHECK(depth->max == 1.0);
	// A fresh sound's drawer is unchanged: depth 1 reaches the voice's drawer.
	SynthRig fresh(OscType::Additive);
	soloA(fresh);
	fresh.set(dp(0, SC::PartialsOn), 1.0);
	playNote(fresh);
	CHECK(newest(fresh).drawerForTest(0).partialCount.depth == 1.0f);
	// ONE SOUND: its own value.
	SynthRig one(OscType::Additive);
	soloA(one);
	one.set(dp(0, SC::PartialsOn), 1.0);
	one.set(dp(0, SC::PartialsDepth), 0.25);
	playNote(one);
	CHECK(newest(one).drawerForTest(0).partialCount.depth == doctest::Approx(0.25f).epsilon(1e-6));
	// A MORPH: A at 0.2 and B at 0.8, both on, is their weighted mean.
	SynthRig rig(OscType::Additive);
	rig.set(ParamId::Path, 0.4);
	for (size_t s : { size_t { 0 }, size_t { 1 } }) {
		rig.set(dp(s, SC::PartialsOn), 1.0);
	}
	rig.set(dp(0, SC::PartialsDepth), 0.2);
	rig.set(dp(1, SC::PartialsDepth), 0.8);
	playNote(rig);
	const auto [wa, wb] = abWeights(rig);
	CHECK(static_cast<double>(newest(rig).drawerForTest(0).partialCount.depth)
	      == doctest::Approx((wa * 0.2 + wb * 0.8) / (wa + wb)).epsilon(1e-5));
}

// ---- OE2: the engine vocoder's banded road, Stereo, Spread, and the gains' memory (additive design §9.8, §9.9) -------------
//
// The Even layout restated here from the Spectral effect's words (§10.7.2: centres even in log2 Hz across the Range, each a
// Gaussian of (0.15 + 0.85·Bandwidth) of its local spacing; partition of unity over the nearest band and four either side); the
// other zones through dsp/vocode_bands.h, the effect's own layout, pinned by test_fx_spectral.

namespace oe2 {

struct Layout {
	int count = 0;
	std::array<double, 129> centre {};
	std::array<double, 129> sigma {};
};

/// The Even layout over [low, high] (Hz): `count` centres, sigmas by local spacing.
Layout even(int count, double low, double high, double bandwidth) {
	Layout l;
	l.count = count;
	const double lo = std::log2(low), hi = std::log2(high);
	for (int b = 0; b < count; ++b) {
		l.centre[static_cast<size_t>(b)] = lo + (hi - lo) * (b + 0.5) / count;
	}
	for (int b = 0; b < count; ++b) {
		const double left = b > 0 ? l.centre[b] - l.centre[b - 1] : 2.0 * (l.centre[b] - lo);
		const double right = b + 1 < count ? l.centre[b + 1] - l.centre[b] : 2.0 * (hi - l.centre[b]);
		l.sigma[static_cast<size_t>(b)] = (0.15 + 0.85 * bandwidth) * 0.5 * (left + right);
	}
	return l;
}

/// The partition-of-unity mix at log2 Hz `x` of per-band linear levels `level`, and the band coordinate there.
std::pair<double, double> mixAt(const Layout& l, const std::array<double, 129>& level, double xIn) {
	const double x = std::clamp(xIn, l.centre[0], l.centre[static_cast<size_t>(l.count - 1)]);
	int nearest = 0;
	for (int b = 1; b < l.count; ++b) {
		if (std::fabs(l.centre[b] - x) <= std::fabs(l.centre[nearest] - x)) {
			nearest = b;
		}
	}
	double sum = 0.0, mix = 0.0;
	for (int b = std::max(0, nearest - 4); b <= std::min(l.count - 1, nearest + 4); ++b) {
		const double d = (x - l.centre[b]) / l.sigma[b];
		const double g = std::exp(-0.5 * d * d);
		sum += g;
		mix += g * level[b];
	}
	const int j = x >= l.centre[nearest] ? nearest : std::max(nearest - 1, 0);
	const double gap = j + 1 < l.count ? l.centre[j + 1] - l.centre[j] : 0.0;
	const double u = j + (gap > 0.0 ? (x - l.centre[j]) / gap : 0.0);
	return { mix / sum, u };
}

/// Each band's level: the envelope at its centre, linear.
std::array<double, 129> levelsAt(const Layout& l, const std::array<float, 128>& grid) {
	sp::VocodeEnvelope e;
	e.db = grid;
	std::array<double, 129> level {};
	for (int b = 0; b < l.count; ++b) {
		level[static_cast<size_t>(b)] = std::pow(10.0, oe::dbAt(e, std::exp2(l.centre[b])) / 20.0);
	}
	return level;
}

sp::VocodeOp classic() {
	sp::VocodeOp op;
	op.on = true;
	return op;
}

} // namespace oe2

TEST_CASE("OE2: on the banded road each partial takes the partition-of-unity mix of the bands within reach, each band the envelope at its centre") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 45.0, f0 = hzOfKey(key);
	sp::VocodeEnvelope env = oe::rippled();
	env.bands = 16.0;
	for (const float bandwidth : { 0.0f, 0.4f }) {
		CAPTURE(bandwidth);
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = oe2::classic();
		d.vocode.bandwidth = bandwidth;
		d.vocode.lowHz = 100.0f;
		d.vocode.highHz = 8000.0f;
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		renderMono(*v, 4 * kBlock);
		const PartialSpectrum& spec = playing(*v);
		const oe::Partials p = oe::partialsOf(spec, f0);
		const oe2::Layout l = oe2::even(16, 100.0, 8000.0, bandwidth);
		const auto level = oe2::levelsAt(l, env.db);
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			if (!(spec.envAtRatio[i] * spec.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
				continue;
			}
			INFO("partial " << i);
			const double hz = p.hz[static_cast<size_t>(i)];
			const double mixed = oe2::mixAt(l, level, std::log2(hz)).first;
			const double want = oe::lineAt(20.0 * std::log10(mixed), f0, env.binHz);
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0)) == doctest::Approx(want).epsilon(2e-3).scale(0.0));
			++checked;
		}
		CHECK(checked > 40);
	}
	// AND THE OTHER ZONES ARE THE EFFECT'S OWN LAYOUT: Low at full amount, through dsp/vocode_bands.h.
	{
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = oe2::classic();
		d.vocode.bandwidth = 0.2f;
		d.vocode.zone = 2;
		d.vocode.zoneAmount = 1.0f;
		d.vocode.lowHz = 100.0f;
		d.vocode.highHz = 8000.0f;
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		renderMono(*v, 4 * kBlock);
		sp::BandSpec spec;
		spec.count = 16;
		spec.low = 100.0;
		spec.high = 8000.0;
		spec.bandwidth = 0.2;
		spec.zone = 2;
		spec.zoneDrive = 1.0;
		oe2::Layout l;
		l.count = sp::layoutBands(spec, nullptr, l.centre.data(), l.sigma.data());
		const auto level = oe2::levelsAt(l, env.db);
		const PartialSpectrum& s = playing(*v);
		const oe::Partials p = oe::partialsOf(s, f0);
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			if (!(s.envAtRatio[i] * s.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
				continue;
			}
			const double mixed = oe2::mixAt(l, level, std::log2(p.hz[static_cast<size_t>(i)])).first;
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0))
			      == doctest::Approx(oe::lineAt(20.0 * std::log10(mixed), f0, env.binHz)).epsilon(2e-3).scale(0.0));
			++checked;
		}
		CHECK(checked > 40);
	}
}

TEST_CASE("OE2: Harmonic spacing lays a narrow band on each of the note's harmonics, so each partial reads the envelope at its own frequency") {
	// UNGROUPED (fewer harmonics in the Range than bands): every harmonic is a band's centre, and at Bandwidth 0 a band's neighbours
	// weigh e^(−0.5·(1/0.15)²) ≈ 2e-10 there — the partial reads its own frequency, for each note.
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	sp::VocodeEnvelope env = oe::rippled();
	env.bands = 24.0;
	for (const double key : { 45.0, 50.0 }) {
		CAPTURE(key);
		const double f0 = hzOfKey(key);
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = oe2::classic();
		d.vocode.bandwidth = 0.0f;
		d.vocode.zone = 4;
		d.vocode.zoneAmount = 1.0f;
		d.vocode.lowHz = 100.0f;
		d.vocode.highHz = 2000.0f;
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		renderMono(*v, 4 * kBlock);
		const PartialSpectrum& s = playing(*v);
		const oe::Partials p = oe::partialsOf(s, f0);
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			const double hz = p.hz[static_cast<size_t>(i)];
			if (hz < 120.0 || hz > 1900.0 || std::fabs(hz / f0 - std::round(hz / f0)) > 1e-4 || !(s.envAtRatio[i] * s.fine[i] > 1e-6f)) {
				continue; // a harmonic partial inside the Range
			}
			INFO("partial " << i);
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0))
			      == doctest::Approx(oe::lineAt(oe::dbAt(env, std::round(hz / f0) * f0), f0, env.binHz)).epsilon(3e-3).scale(0.0));
			++checked;
		}
		CHECK(checked >= 10);
	}
}

TEST_CASE("OE2: Stereo vocodes each ear by its own sidechain channel; Spread sends alternate bands to alternate ears, the power kept") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	const double key = 45.0, f0 = hzOfKey(key);
	sp::VocodeEnvelope env = oe::rippled();
	env.bands = 16.0;
	env.stereo = true;
	for (size_t g = 0; g < env.dbRight.size(); ++g) {
		env.dbRight[g] = static_cast<float>(-50.0 + 0.1 * static_cast<double>(g)); // a different envelope on the right
	}
	const auto run = [&](int stereo) {
		auto v = voiceOn(c, key);
		sp::ShaperParams d;
		d.vocode = oe2::classic();
		d.vocode.stereo = stereo;
		d.vocode.lowHz = 100.0f;
		d.vocode.highHz = 8000.0f;
		v->voice.setShaperParamsForTest(d);
		v->voice.setVocodeEnvelope(&env);
		std::vector<float> l(size_t(4 * kBlock)), r(size_t(4 * kBlock));
		for (int b = 0; b < 4; ++b) {
			v->renderStereo(l.data() + b * kBlock, r.data() + b * kBlock, kBlock);
		}
		return v;
	};
	sp::VocodeEnvelope right = env;
	right.db = env.dbRight;
	{
		auto v = run(1);
		const PartialSpectrum& s = playing(*v);
		const oe::Partials p = oe::partialsOf(s, f0);
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			if (!(s.envAtRatio[i] * s.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
				continue;
			}
			const double hz = std::clamp(p.hz[static_cast<size_t>(i)], 100.0, 8000.0);
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0)) == doctest::Approx(oe::lineAt(oe::dbAt(env, hz), f0, env.binHz)).epsilon(2e-3).scale(0.0));
			CHECK(static_cast<double>(v->voice.partialAmplitudeForTest(i, 1)) == doctest::Approx(oe::lineAt(oe::dbAt(right, hz), f0, env.binHz)).epsilon(2e-3).scale(0.0));
			++checked;
		}
		CHECK(checked > 40);
	}
	{
		auto mono = run(0);
		auto v = run(2);
		const PartialSpectrum& s = playing(*v);
		const oe::Partials p = oe::partialsOf(s, f0);
		const oe2::Layout l = oe2::even(16, 100.0, 8000.0, 1.0);
		const std::array<double, 129> none {};
		int checked = 0;
		for (int i = 0; i < p.count; ++i) {
			if (!(s.envAtRatio[i] * s.fine[i] > 1e-6f) || p.fade[static_cast<size_t>(i)] < 1.0) {
				continue;
			}
			INFO("partial " << i);
			const double u = oe2::mixAt(l, none, std::log2(p.hz[static_cast<size_t>(i)])).second;
			const double a = static_cast<double>(mono->voice.partialAmplitudeForTest(i, 0));
			// AT A BAND'S CENTRE ONE EAR'S SHARE IS EXACTLY 0 IN THE RESTATEMENT, which the bank's silence floor writes as 0.
			const auto near = [](double got, double want) {
				return want < 1e-6 ? got < 1e-6 : std::fabs(got - want) <= 2e-3 * want;
			};
			const double wl = a * std::sqrt(2.0) * std::fabs(std::cos(std::numbers::pi / 2.0 * u));
			const double wr = a * std::sqrt(2.0) * std::fabs(std::sin(std::numbers::pi / 2.0 * u));
			CAPTURE(wl);
			CAPTURE(wr);
			CHECK(near(static_cast<double>(v->voice.partialAmplitudeForTest(i, 0)), wl));
			CHECK(near(static_cast<double>(v->voice.partialAmplitudeForTest(i, 1)), wr));
			++checked;
		}
		CHECK(checked > 40);
	}
}

TEST_CASE("OE2: the gains' memory — computed again only when what they are made from moves, and the same gains to the bit") {
	const Character c = additive({ sound(120, 0.3f, 1.0) });
	sp::VocodeEnvelope env = oe::rippled();
	auto v = voiceOn(c, 52.0);
	auto fresh = voiceOn(c, 52.0); // its envelope's serial moves every block: it computes every block
	sp::ShaperParams d;
	d.vocode = oe2::classic();
	d.vocode.flatten = 0.6f;
	for (auto* x : { &v, &fresh }) {
		(*x)->voice.setShaperParamsForTest(d);
	}
	v->voice.setVocodeEnvelope(&env);
	sp::VocodeEnvelope moving = env;
	fresh->voice.setVocodeEnvelope(&moving);
	for (int b = 0; b < 12; ++b) {
		++moving.serial;
		renderMono(*v, kBlock);
		renderMono(*fresh, kBlock);
	}
	CHECK(v->voice.vocodeComputesForTest() == 1u);
	CHECK(fresh->voice.vocodeComputesForTest() == 12u);
	for (int i = 0; i < kMaxPartials; ++i) {
		CHECK(v->voice.partialAmplitudeForTest(i, 0) == fresh->voice.partialAmplitudeForTest(i, 0));
	}
	// A NEW ENVELOPE, A NEW PITCH, A NEW SETTING: each computes once more, and only once.
	++env.serial;
	renderMono(*v, 4 * kBlock);
	CHECK(v->voice.vocodeComputesForTest() == 2u);
	v->voice.setPitch(53.0);
	renderMono(*v, 4 * kBlock);
	CHECK(v->voice.vocodeComputesForTest() >= 3u);
	const uint32_t afterPitch = v->voice.vocodeComputesForTest();
	renderMono(*v, 4 * kBlock);
	CHECK(v->voice.vocodeComputesForTest() == afterPitch);
	d.vocode.depth = 0.5f;
	v->voice.setShaperParamsForTest(d);
	renderMono(*v, 4 * kBlock);
	CHECK(v->voice.vocodeComputesForTest() == afterPitch + 1u);
}

// ---- Backlog OZ: the right ear's own partial phase ------------------------------------------------------------------
namespace {
namespace ozb {

/// FNV-1a over the bits of a buffer: a render's identity, to the bit.
uint64_t bitsHash(const std::vector<float>& x, uint64_t h = 1469598103934665603ull) {
	for (const float f : x) {
		uint32_t u = 0;
		std::memcpy(&u, &f, sizeof u);
		for (int b = 0; b < 4; ++b) {
			h ^= (u >> (8 * b)) & 0xffu;
			h *= 1099511628211ull;
		}
	}
	return h;
}

/// A Phase run that moves every sample (a slow sine about 0.3 cycles), long enough for any call.
std::vector<float> movingPhase(int n) {
	std::vector<float> run(static_cast<size_t>(n));
	for (int j = 0; j < n; ++j) {
		run[size_t(j)] = 0.3f + 0.05f * static_cast<float>(std::sin(kTwoPi * 3.0 * j / kSampleRate));
	}
	return run;
}

/// The four renders of patches whose Phase is NOT stereo-live, each hashed L then R: (0) mono, Phase moving; (1) stereo,
/// one spectrum, Phase moving, no width, no ear route; (2) stereo, two ears' spectra, Phase moving; (3) stereo, Phase
/// Width at 0.5 with Phase ON its knob (nothing to mirror). 64-sample calls, 4096 samples.
std::array<uint64_t, 4> notStereoLiveHashes() {
	constexpr int kN = 4096, kCall = 64;
	std::array<uint64_t, 4> out {};
	for (int patch = 0; patch < 4; ++patch) {
		auto v = std::make_unique<BankedVoice>();
		v->setSampleRate(kSampleRate);
		if (patch == 2) {
			v->setCharacterStereo(additive({ sound(211, 0.3f, 1.0) }), additive({ sound(640, -1.7f, 1.0) }));
		}
		else {
			v->setCharacter(additive({ offGridSound() }));
		}
		v->reset();
		v->voice.setPitch(50.0);
		const std::vector<float> run = movingPhase(kN + kCall);
		std::vector<float> l(kN), r(kN);
		for (int done = 0; done < kN; done += kCall) {
			if (patch == 3) {
				v->voice.setReadPhaseWidth(0.3f, 0.5f);
				v->voice.setReadPhase(0.3f, nullptr, 0);
			}
			else {
				v->voice.setReadPhase(0.0f, run.data() + done, kCall);
			}
			if (patch == 0) {
				v->render(l.data() + done, kCall);
			}
			else {
				v->renderStereo(l.data() + done, r.data() + done, kCall);
			}
		}
		out[size_t(patch)] = bitsHash(r, bitsHash(l));
	}
	return out;
}

} // namespace ozb
} // namespace

TEST_CASE("backlog OZ: a patch whose Phase is not stereo-live renders the bits it rendered before the right ear had a phase") {
	// THE BIT-EXACT PROMISE, by value: four renders through every road an Additive voice takes with Phase moving but not
	// stereo-live (mono; stereo on one spectrum; stereo on two; Phase Width with nothing to mirror), hashed to the bit and
	// pinned at the hashes the same renders gave at db3f08fc, the commit before backlog OZ. Linux x86-64 only: the bank's
	// note-on phases go through libm, whose last bits are the platform's (as vox-voice and flagship are pinned).
	const std::array<uint64_t, 4> got = ozb::notStereoLiveHashes();
	for (size_t i = 0; i < got.size(); ++i) {
		MESSAGE("patch " << i << " hash " << got[i]);
	}
#if defined(__x86_64__) && defined(__linux__)
	const std::array<uint64_t, 4> before = { 12599633901170770067ull, 7095632569927499907ull, 8792885168817650767ull,
	                                     17290406804018961795ull };
	for (size_t i = 0; i < got.size(); ++i) {
		CAPTURE(i);
		CHECK(got[i] == before[i]);
	}
#endif
}

namespace {
namespace ozb {

/// The voice's right-ear phases against its left: each live partial's right phase is its left phase plus
/// ratio_i·2π·`ear` cycles (mod 2π), for the partials that render. Returns how many were checked.
int checkEarPhases(const BankedVoice& v, double ear, double tolerance) {
	const PartialSpectrum& spec = playing(v);
	const int count = std::min(renderingCount(spec, hzOfKey(50.0)), v.voice.partialsLiveForTest());
	for (int i = 0; i < count; ++i) {
		INFO("partial " << i);
		const double want = double(v.voice.partialPhaseForTest(i)) + kTwoPi * double(spec.ratio[i]) * ear;
		CHECK(std::abs(angleDiff(v.voice.partialEarPhaseForTest(i), want)) < tolerance);
	}
	return count;
}

/// The right ear's last sample restated from the bank's values: Σ_i a_i(R)·sin(φ_i + ratio_i·2π·ear), φ_i the LEFT ear's
/// phase after the sample. Not a render comparison: the sum is built from the left ear's phases and the restated offset.
double rightSampleRestated(const BankedVoice& v, double ear) {
	const PartialSpectrum& spec = playing(v);
	double sum = 0.0;
	for (int i = 0; i < v.voice.partialsLiveForTest(); ++i) {
		sum += double(v.voice.partialAmplitudeForTest(i, 1))
		       * std::sin(double(v.voice.partialPhaseForTest(i)) + kTwoPi * double(spec.ratio[i]) * ear);
	}
	return sum;
}

} // namespace ozb
} // namespace

TEST_CASE("backlog OZ: an ear route into Phase turns each partial's right ear by its ratio times the ears' difference, by value") {
	// R = base + m + (mR − mL) on Additive as on the other engines: with Phase at 0.25 and an ear difference of 0.1 cycles
	// handed over (setReadPhaseEar, the synth's road for a stereo route), every partial's right-ear phase is its left phase
	// + ratio_i·2π·0.1 after the note's first block. Then the difference RAMPS to 0.13 over the next block (both ears land
	// on it), then to 0, and the block after that the right ear is the shared rotator again. Off-grid ratios, so the turn
	// is not a whole-cycle multiple of anything.
	auto v = voiceOn(additive({ offGridSound() }), 50.0);
	auto plain = voiceOn(additive({ offGridSound() }), 50.0); // the same note, no ear: the left ear's reference
	float l[kBlock], r[kBlock], pl[kBlock], pr[kBlock];
	const auto block = [&](float ear) {
		v->voice.setReadPhase(0.25f, nullptr, 0);
		v->voice.setReadPhaseEar(ear, nullptr, 0);
		plain->voice.setReadPhase(0.25f, nullptr, 0);
		v->renderStereo(l, r, kBlock);
		plain->renderStereo(pl, pr, kBlock);
	};
	block(0.1f);
	REQUIRE(v->voice.partialsEarLiveForTest());
	CHECK(v->voice.partialsEarOffsetForTest() == doctest::Approx(0.1));
	CHECK(ozb::checkEarPhases(*v, 0.1, 2e-4) > 40);
	CHECK(std::abs(double(r[kBlock - 1]) - ozb::rightSampleRestated(*v, 0.1)) < 2e-5);
	// THE LEFT EAR IS UNTOUCHED, to the bit: the ear difference is the right ear's alone.
	CHECK(std::equal(l, l + kBlock, pl));
	// NOT VACUOUS: the right ear is not the left.
	CHECK(std::abs(double(r[kBlock - 1]) - double(l[kBlock - 1])) > 1e-3);
	block(0.13f);
	CHECK(v->voice.partialsEarLiveForTest());
	CHECK(ozb::checkEarPhases(*v, 0.13, 3e-4) > 40);
	CHECK(std::abs(double(r[kBlock - 1]) - ozb::rightSampleRestated(*v, 0.13)) < 3e-5);
	CHECK(std::equal(l, l + kBlock, pl));
	block(0.0f); // the route let go: the difference ramps home over a block, not a step
	CHECK(v->voice.partialsEarLiveForTest());
	CHECK(ozb::checkEarPhases(*v, 0.0, 3e-4) > 40);
	block(0.0f);
	CHECK_FALSE(v->voice.partialsEarLiveForTest());
	CHECK(std::equal(r, r + kBlock, l)); // one spectrum, no ear: the right ear IS the left again
}

TEST_CASE("backlog OZ: Phase Width mirrors what the routes add onto the right ear's partials, as the other engines' R = base + m·(1 − 2w)") {
	// The knob at 0.25, the routes adding m = 0.15 (Phase read at 0.4), width 0.5: the right ear reads at 0.25 + 0.15·0 =
	// 0.25, so its partials stand at the left's less ratio_i·2π·0.15. At width 1 the mirror is whole: less 2·0.15.
	for (const float width : { 0.5f, 1.0f }) {
		CAPTURE(width);
		auto v = voiceOn(additive({ offGridSound() }), 50.0);
		float l[kBlock], r[kBlock];
		for (int b = 0; b < 3; ++b) {
			v->voice.setReadPhaseWidth(0.25f, width);
			v->voice.setReadPhase(0.4f, nullptr, 0);
			v->renderStereo(l, r, kBlock);
		}
		const double ear = -2.0 * double(width) * (double(0.4f) - double(0.25f));
		REQUIRE(v->voice.partialsEarLiveForTest());
		CHECK(v->voice.partialsEarOffsetForTest() == doctest::Approx(ear).epsilon(1e-6));
		CHECK(ozb::checkEarPhases(*v, ear, 3e-4) > 40);
		CHECK(std::abs(double(r[kBlock - 1]) - ozb::rightSampleRestated(*v, ear)) < 3e-5);
	}
}

// BACKLOG PD (suspected artifacts when moving Path between two smooth patches on Additive).
namespace {

/// One voice playing a held note while its character is walked from sound `a` to sound `b` (the two weights (1 − t, t), as
/// Path walks a two-sound tour) over `sweepBlocks` control blocks, then held at `b` for as long again. The bank's values are
/// read at every block's end, where the bank's ramp lands exactly on its target.
struct PathSweep {
	std::vector<std::vector<float>> amp, inc; ///< per block, every partial, ear 0
	std::vector<float> out;
	/// The most any sounding partial's increment differs between two consecutive builds of the bank, restated from their ratios
	/// (2π·f0·ratio / sr, as the voice computes it): the step a partial would take if its pitch followed the flips.
	double buildIncDelta{0.0};
	/// The same, for partial 0 alone (the sub, under Sub priority).
	double subBuildIncDelta{0.0};
	uint32_t resets{0};
	static constexpr int kRead = kMaxPartials;
};

PathSweep sweepVoice(const SoundCoord& a, const SoundCoord& b, int sweepBlocks, const sp::ShaperParams* drawer, double key = 48.0,
                     feathers::partials::PhaseMode mode = feathers::partials::PhaseMode::Natural) {
	SoundCoord from = a, to = b;
	from.weight = 1.0;
	to.weight = 0.0;
	const auto make = [&] {
		Character c = additive({ from, to });
		c.phasing.mode = mode;
		return c;
	};
	auto v = voiceOn(make(), key);
	if (drawer != nullptr) {
		v->voice.setShaperParamsForTest(*drawer);
	}
	PathSweep s;
	renderMono(*v, 4096); // the note's first table and its priming behind it
	const uint32_t resets0 = v->voice.partialsResetsForTest();
	uint32_t generation = v->bank.generation();
	std::array<float, kMaxPartials> built = v->bank.current(0).partials.ratio;
	const double perRatio = kTwoPi * hzOfKey(key) / kSampleRate;
	const int total = 2 * std::max(sweepBlocks, 64);
	for (int blk = 0; blk < total; ++blk) {
		const double t = std::min(1.0, double(blk + 1) / double(std::max(1, sweepBlocks)));
		from.weight = 1.0 - t;
		to.weight = t;
		v->setCharacter(make());
		const std::vector<float> o = renderMono(*v, kBlock);
		if (v->bank.generation() != generation) {
			generation = v->bank.generation();
			const std::array<float, kMaxPartials>& now = v->bank.current(0).partials.ratio;
			for (int i = 0; i < kMaxPartials; ++i) {
				const double was = perRatio * double(built[size_t(i)]), is = perRatio * double(now[size_t(i)]);
				if (fadeRestated(was) > 0.0 || fadeRestated(is) > 0.0) {
					s.buildIncDelta = std::max(s.buildIncDelta, std::abs(is - was));
					if (i == 0) {
						s.subBuildIncDelta = std::max(s.subBuildIncDelta, std::abs(is - was));
					}
				}
			}
			built = now;
		}
		s.out.insert(s.out.end(), o.begin(), o.end());
		std::vector<float> am(PathSweep::kRead), in(PathSweep::kRead);
		for (int i = 0; i < PathSweep::kRead; ++i) {
			am[size_t(i)] = v->voice.partialAmplitudeForTest(i, 0);
			in[size_t(i)] = v->voice.partialIncrementForTest(i);
		}
		s.amp.push_back(std::move(am));
		s.inc.push_back(std::move(in));
	}
	s.resets = v->voice.partialsResetsForTest() - resets0;
	return s;
}

/// The largest block-to-block move of any partial's amplitude, as a fraction of the most that partial reaches in the run.
double worstStepShare(const PathSweep& s) {
	double worst = 0.0;
	for (int i = 0; i < PathSweep::kRead; ++i) {
		double peak = 0.0, step = 0.0;
		for (size_t blk = 0; blk < s.amp.size(); ++blk) {
			peak = std::max(peak, double(std::abs(s.amp[blk][size_t(i)])));
			if (blk > 0) {
				step = std::max(step, double(std::abs(s.amp[blk][size_t(i)] - s.amp[blk - 1][size_t(i)])));
			}
		}
		if (peak > 1e-6) {
			worst = std::max(worst, step / peak);
		}
	}
	return worst;
}

/// The largest block-to-block move of any sounding partial's increment, in radians per sample.
double worstIncrementStep(const PathSweep& s) {
	double worst = 0.0;
	for (size_t blk = 1; blk < s.inc.size(); ++blk) {
		for (int i = 0; i < PathSweep::kRead; ++i) {
			if (s.amp[blk][size_t(i)] != 0.0f) {
				worst = std::max(worst, double(std::abs(s.inc[blk][size_t(i)] - s.inc[blk - 1][size_t(i)])));
			}
		}
	}
	return worst;
}

/// THE AUDIO, AS A SECOND READING: the output's second difference against what a sum of sinusoids at the bank's amplitudes
/// and increments can reach, Σ a_k·(2·sin(w_k/2))², a_k the larger of the block's two ends. A click or a reset (anything that
/// is not a partial) exceeds it; a ramp adds only its slope's share. The worst ratio over the run.
double worstSecondDifference(const PathSweep& s) {
	double worst = 0.0;
	for (size_t n = 2; n < s.out.size(); ++n) {
		const size_t blk = n / kBlock;
		double bound = 0.0;
		for (int i = 0; i < PathSweep::kRead; ++i) {
			const double am = std::max(std::abs(s.amp[blk][size_t(i)]), blk > 0 ? std::abs(s.amp[blk - 1][size_t(i)]) : 0.0f);
			const double h = 2.0 * std::sin(0.5 * s.inc[blk][size_t(i)]);
			bound += am * h * h;
		}
		const double d2 = std::abs(double(s.out[n]) - 2.0 * double(s.out[n - 1]) + double(s.out[n - 2]));
		if (bound > 1e-6) {
			worst = std::max(worst, d2 / bound);
		}
	}
	return worst;
}

sp::ShaperParams peaksDrawer(float count) {
	sp::ShaperParams p;
	p.partialCount.on = true;
	p.partialCount.count = count;
	p.partialCount.peaks = 1.0f;
	return p;
}

} // namespace

TEST_CASE("additive voice: a character walked between two places moves each partial by at most a fade's share per block, Peaks included, and never re-primes") {
	// THE BOUND, derived: a block moves the fade's t by at most kBlock / fade (1/8 at 48 kHz), so a partial's spectral amplitude
	// moves by at most 1/8 of |stale − fresh|, which is at most the most it reaches; and Peaks' keep glides by at most the same
	// 1/8 (PeaksGlide). Together at most 1/4 of the partial's own peak, at any speed. A partial gated in one block moves by all
	// of it (backlog PD: 1.0 measured at Peaks 8 before the glide). The increments follow the fresh side at each rebuild, so on
	// these two sounds they move only by their small off-grid difference.
	const SoundCoord a = sound(300, 0.2f, 1.0), b = sound(700, -1.3f, 1.0);
	const sp::ShaperParams peaks = peaksDrawer(8.0f);
	for (const sp::ShaperParams* drawer : { static_cast<const sp::ShaperParams*>(nullptr), &peaks }) {
		for (const int blocks : { 1, 15, 150, 1500 }) {
			CAPTURE(drawer != nullptr);
			CAPTURE(blocks);
			const PathSweep s = sweepVoice(a, b, blocks, drawer);
			CHECK(s.resets == 0);
			CHECK(worstStepShare(s) <= 0.25 + 1e-4);
			CHECK(worstIncrementStep(s) < 1e-5);
			// THE AUDIO, SECOND: nothing in the output that a sum of the bank's partials cannot make.
			CHECK(worstSecondDifference(s) <= 1.05);
		}
	}
}

TEST_CASE("additive voice: Peaks' keep glides — a partial the ranking drops ramps out over one character fade, and a note-on ranks at once") {
	// The ranking at rest is the one the note-on snaps to (to the bit, as before the glide); a walk that swaps two partials'
	// places ramps the leaver down by exactly kBlock / fade of its keep per block until it is gone.
	const SoundCoord a = sound(300, 0.2f, 1.0), b = sound(700, -1.3f, 1.0);
	const sp::ShaperParams peaks = peaksDrawer(8.0f);
	const PathSweep s = sweepVoice(a, b, 1, &peaks);
	const double share = double(kBlock) / std::round(feathers::kCharacterFadeSeconds * kSampleRate);
	// Some partial is dropped by the jump (it sounds before it and is silent at the end), and its amplitude takes at least
	// 1 / share blocks to reach 0 (the first block, 0, is already the fade's first) — the glide, not a gate.
	int dropped = 0;
	for (int i = 0; i < PathSweep::kRead; ++i) {
		if (s.amp.front()[size_t(i)] != 0.0f && s.amp.back()[size_t(i)] == 0.0f) {
			++dropped;
			size_t last = 0;
			for (size_t blk = 0; blk < s.amp.size(); ++blk) {
				if (s.amp[blk][size_t(i)] != 0.0f) {
					last = blk;
				}
			}
			CAPTURE(i);
			CHECK(double(last) + 2.0 >= 1.0 / share - 1e-9); // block `last + 1` is its first silent one
		}
	}
	CHECK(dropped > 0);
	// A NOTE-ON RANKS AT ONCE: exactly eight partials sound in its first block, nothing gliding in.
	auto v = voiceOn(additive({ a }), 48.0);
	v->voice.setShaperParamsForTest(peaks);
	renderMono(*v, kBlock);
	int sounding = 0;
	for (int i = 0; i < kMaxPartials; ++i) {
		sounding += v->voice.partialAmplitudeForTest(i, 0) != 0.0f ? 1 : 0;
	}
	CHECK(sounding == 8);
}

TEST_CASE("additive synth: Path swept between the default patch's two sounds, plain and at Peaks 8, steps no partial past a fade's share and never re-primes") {
	// The synth's road (Path -> the tour -> the bank's rate-limited rebuilds -> the voice), read at every 32 samples. The bound is
	// the voice case's 1/4 of each partial's own peak. Swept in 0.1 s and in 1 s, Path written every 64 samples as a host would.
	using SC = SpectralControl;
	for (const bool peaks : { false, true }) {
		for (const int frames : { 4800, 48000 }) {
			CAPTURE(peaks);
			CAPTURE(frames);
			SynthRig rig(OscType::Additive);
			if (peaks) {
				for (size_t slot : { size_t(0), size_t(1) }) {
					rig.set(dp(slot, SC::PartialsOn), 1.0);
					rig.set(dp(slot, SC::PartialsCount), 8.0);
					rig.set(dp(slot, SC::PartialsMode), static_cast<double>(sp::PartialsScheme::Peaks));
				}
			}
			rig.set(ParamId::Path, 0.0);
			std::vector<float> l, r;
			rig.play(512, l, r);
			rig.synth.noteOn(0, 48, 1, 1.0);
			rig.play(9600, l, r);
			const feathers::OscVoice& osc = rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), 0);
			const uint32_t resets0 = osc.partialsResetsForTest();
			std::vector<std::vector<float>> amp;
			float lb[kBlock], rb[kBlock];
			for (int at = 0; at < frames + 4800; at += kBlock) {
				if (at % 64 == 0) {
					rig.set(ParamId::Path, std::min(1.0, double(at) / double(frames)));
				}
				std::fill_n(lb, kBlock, 0.0f);
				std::fill_n(rb, kBlock, 0.0f);
				rig.synth.render(lb, rb, kBlock);
				std::vector<float> a(kMaxPartials);
				for (int i = 0; i < kMaxPartials; ++i) {
					a[size_t(i)] = osc.partialAmplitudeForTest(i, 0);
				}
				amp.push_back(std::move(a));
			}
			CHECK(osc.partialsResetsForTest() == resets0);
			PathSweep s;
			s.amp = std::move(amp);
			CHECK(worstStepShare(s) <= 0.25 + 1e-4);
		}
	}
}

TEST_CASE("additive voice: an off-grid ratio a rebuild moves glides across the fade — no partial's increment steps by more than a fade's share per block") {
	// THE BOUND, derived: the voice's ratios walk from the stale build's to the fresh one's by the fade's t, which moves by at
	// most kBlock / fade (1/8 at 48 kHz) per block, so a partial's increment moves by at most 1/8 of the largest difference
	// between two consecutive builds (restated from the built ratios). Following the flips instead, the step is that whole
	// difference: measured 2.3e-3 rad/sample (~17 Hz on a high partial) on this 10 ms sweep before the glide (backlog PD). The
	// slack is the read-back's (atan2 of the rotator's coefficient).
	const SoundCoord a = sound(300, 0.2f, 1.0), og = offGridSound();
	const double share = double(kBlock) / std::round(feathers::kCharacterFadeSeconds * kSampleRate);
	for (const int blocks : { 15, 150 }) {
		CAPTURE(blocks);
		const PathSweep s = sweepVoice(a, og, blocks, nullptr);
		REQUIRE(s.buildIncDelta > 1e-4); // the sweep moves ratios, or the bound below is vacuous
		CAPTURE(s.buildIncDelta);
		CHECK(worstIncrementStep(s) <= s.buildIncDelta * share * 1.001 + 2e-6);
		CHECK(s.resets == 0);
		CHECK(worstSecondDifference(s) <= 1.05);
	}
}

TEST_CASE("additive voice: a held note's increments never move, and a harmonic walk's move only by the builds' own rounding") {
	// THE GLIDE ACTS ONLY DURING A REBUILD'S FADE: a held note has none, so every partial's rotator coefficient stays put to the
	// bit. Two shapes at Shape Amount 1 (every ratio a whole harmonic) walked into each other: the builds' ratios agree up to the
	// blend's own float rounding (MEASURED 4.8e-7 to 7.2e-7 rad/sample of read-back step with the glide removed, and the same
	// with it; the output differs from the glide-free road by at most 4.1e-6 on a peak of 0.51, −102 dB), so the bound is the
	// read-back's slack, three orders below the off-grid case's 2.3e-3.
	SoundCoord a = sound(300, 0.2f, 1.0), b = sound(700, -1.3f, 1.0);
	a.shapeAmount = 1.0f;
	b.shapeAmount = 1.0f;
	for (const int blocks : { 15, 1500 }) {
		CAPTURE(blocks);
		const PathSweep s = sweepVoice(a, b, blocks, nullptr);
		CHECK(worstIncrementStep(s) <= 1e-6);
	}
	const PathSweep held = sweepVoice(a, a, 64, nullptr);
	CHECK(worstIncrementStep(held) == 0.0);
}

TEST_CASE("additive voice: under Sub priority a rebuild's ratios glide across its fade, the sub's included, and the sub never steps") {
	// pinSubPitch rewrites partial 0 from the spectrum's own ratio, exempt from Stretch and Shift; it now reads the same glided
	// ratio the rest do (glideFrom, glideT). THE SUB'S OWN RATIO IS 1 ON EVERY BUILD — off-grid drift starts at harmonic
	// kOffGridLowestStart = 6 and a shape's ratios are whole — so before this it could jump only by the blend's float rounding,
	// and no character can make it jump further: this case pins that the sub does not step (≤ the read-back's slack) and that
	// Sub priority's other partials glide by the off-grid case's bound (1/8 of a build's difference per block).
	const SoundCoord a = sound(300, 0.2f, 1.0), og = offGridSound();
	const double share = double(kBlock) / std::round(feathers::kCharacterFadeSeconds * kSampleRate);
	const PathSweep s = sweepVoice(a, og, 15, nullptr, 48.0, feathers::partials::PhaseMode::SubPriority);
	REQUIRE(s.buildIncDelta > 1e-4);
	CHECK(worstIncrementStep(s) <= s.buildIncDelta * share * 1.001 + 2e-6);
	CHECK(s.subBuildIncDelta < 1e-6); // the sub's ratio is 1 to rounding on both builds
	double subStep = 0.0;
	for (size_t blk = 1; blk < s.inc.size(); ++blk) {
		subStep = std::max(subStep, double(std::abs(s.inc[blk][0] - s.inc[blk - 1][0])));
	}
	CHECK(subStep <= 1e-6);
	CHECK(s.resets == 0);
}

// **BACKLOG MA, CLOSED BY A GUARD RATHER THAN BY AN AUDIT** (2026-10-31). A bare-Synth rig that renders silence makes every
// "unchanged"/"differs" comparison on it vacuous: green whatever the code does. The audit of 2026-10-11 found none silent
// and put a REQUIRE in each comparing case; THIS asks the rig itself, so a rig edited to stop sounding (its Voice Level
// line deleted, its engine left silent) fails here by name, before any case that leans on it.
TEST_CASE("backlog MA: the bare rig's reference render SOUNDS, so a difference test on it can see a difference") {
	const auto peakOf = [](const std::vector<float>& x) { float p = 0.0f; for (const float v : x) { p = std::max(p, std::fabs(v)); } return p; };
	SynthRig rig(OscType::Additive);
	rig.synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> l;
	std::vector<float> r;
	rig.play(4096, l, r);
	CAPTURE(peakOf(l));
	REQUIRE(peakOf(l) > 0.05f);
}

// BACKLOG LY, THE OSC MODES ON ADDITIVE (phase drift in detune).
namespace {

constexpr int kModeParts = 4;
constexpr int kModePartials = 8;

/// A stacked, detuned Additive voice in `mode`, the song playing from `songSeconds`.
struct ModeRig {
	SynthRig rig { OscType::Additive };
	ModeRig(feathers::OscMode mode, double songSeconds, bool legato = false) {
		rig.set(ParamId::StackCount, kModeParts);
		rig.set(ParamId::StackDetune, 0.3);
		rig.set(ParamId::OscMode, static_cast<double>(mode));
		if (legato) {
			rig.set(ParamId::VoiceMode, static_cast<double>(feathers::VoiceMode::Legato));
		}
		rig.synth.setTransport(songSeconds, songSeconds * 2.0, true);
		std::vector<float> l, r;
		rig.play(256, l, r, 64);
	}
	void play(int frames) {
		std::vector<float> l, r;
		rig.play(static_cast<uint32_t>(frames), l, r, 64);
	}
	const feathers::OscVoice& part(int p) const {
		return rig.synth.voiceStackOscForTest(rig.synth.newestVoiceSlotForTest(), p);
	}
};

/// THE RELATION THE MODES ARE MADE OF, restated: two renders of the same note that differ only in what moved the
/// accumulators differ, partial by partial, by 2π·ratio_i times the accumulators' difference — the waveform read where the
/// oscillator is. (Both accumulators lag the bank by the same owed samples, so the difference is exact to rounding.) The
/// worst angle error over the first kModePartials partials of every part.
double worstRelation(const ModeRig& a, const ModeRig& b) {
	double worst = 0.0;
	const PartialSpectrum& spec = a.rig.table().partials;
	for (int p = 0; p < kModeParts; ++p) {
		for (int i = 0; i < kModePartials; ++i) {
			const double bank = double(a.part(p).partialPhaseForTest(i)) - double(b.part(p).partialPhaseForTest(i));
			const double acc = kTwoPi * double(spec.ratio[size_t(i)]) * (a.part(p).phase() - b.part(p).phase());
			worst = std::max(worst, std::abs(angleDiff(bank, acc)));
		}
	}
	return worst;
}

/// The accumulators' difference, part p, in cycles, nearest whole cycle removed.
double accumulatorGap(const ModeRig& a, const ModeRig& b, int p) {
	return std::remainder(a.part(p).phase() - b.part(p).phase(), 1.0);
}

/// Every part's first partials, as floats, for a bit-for-bit comparison.
std::vector<float> partialPhases(const ModeRig& m) {
	std::vector<float> out;
	for (int p = 0; p < kModeParts; ++p) {
		for (int i = 0; i < kModePartials; ++i) {
			out.push_back(m.part(p).partialPhaseForTest(i));
		}
	}
	return out;
}

} // namespace

TEST_CASE("additive osc mode: a note-on — Digi and Catchup start the detuned parts together, Analog starts each at the song's phase for its pitch") {
	// Digi (and Catchup with nothing to adopt) start every part at accumulator phase 0, so every part's partials leave
	// the same Schroeder phases: a coherent flam, and the two modes are the same note to the bit. Analog starts each part
	// at the song's phase for ITS pitch (lockedPhaseFor): against the Digi note, each part's partials stand 2π·ratio·gap
	// away, the gap is that part's accumulator gap, and the detuned parts' gaps differ — they beat from different places.
	ModeRig digi(feathers::OscMode::Digi, 1.25), catchup(feathers::OscMode::Catchup, 1.25), analog(feathers::OscMode::Analog, 1.25);
	for (ModeRig* m : { &digi, &catchup, &analog }) {
		m->rig.synth.noteOn(0, 48, 1, 1.0);
		m->play(64);
	}
	CHECK(partialPhases(catchup) == partialPhases(digi));
	CHECK(worstRelation(analog, digi) < 2e-3);
	for (int p = 0; p < kModeParts; ++p) {
		CAPTURE(p);
		CHECK(std::abs(accumulatorGap(analog, digi, p)) > 1e-3);
		for (int q = 0; q < p; ++q) {
			CHECK(std::abs(std::remainder(accumulatorGap(analog, digi, p) - accumulatorGap(analog, digi, q), 1.0)) > 1e-3);
		}
	}
}

TEST_CASE("additive osc mode: Analog is deterministic — the same song position starts every part's partials at the same phases, bit for bit, and another position does not") {
	const auto run = [](double seconds) {
		ModeRig m(feathers::OscMode::Analog, seconds);
		m.rig.synth.noteOn(0, 48, 1, 1.0);
		m.play(64);
		return partialPhases(m);
	};
	const std::vector<float> a = run(1.25), b = run(1.25), c = run(1.31);
	CHECK(a == b);
	CHECK(a != c);
}

TEST_CASE("additive osc mode: a restrike inside the release — Digi restarts the partials, Catchup bends them home, Analog re-locks them, each where its accumulator goes") {
	// Against a twin whose note is held through (no restrike), the struck note's partials stand 2π·ratio·gap away, the gap
	// the accumulators' — the bank follows the accumulator's departure (partialsAccum_), it is not re-primed. The three are
	// told apart by the gap: Digi's is the whole phase it threw away at once and stays; Catchup's keeps moving block by block
	// toward it (the §5.8 bend); Analog's is the song's re-lock for the new key, at once, and then unbent.
	for (const feathers::OscMode mode : { feathers::OscMode::Digi, feathers::OscMode::Analog, feathers::OscMode::Catchup }) {
		CAPTURE(static_cast<int>(mode));
		ModeRig struck(mode, 1.25, true), held(mode, 1.25, true);
		for (ModeRig* m : { &struck, &held }) {
			m->rig.synth.noteOn(0, 48, 1, 1.0);
			m->play(64 * 38);
		}
		const uint32_t resets = struck.part(0).partialsResetsForTest();
		const int slot = struck.rig.synth.newestVoiceSlotForTest();
		// LET GO AND STRUCK AGAIN ON ANOTHER KEY INSIDE THE RELEASE: Legato moves the voice it has and retriggers it
		// (moveAndRetriggerVoice, the one same-voice retrigger this synth reaches — test_synth's "same-voice road"), so the
		// bank runs on and only the mode says what its phase does. The twin, still held, takes the same key as a plain legato
		// move, which touches no phase in any mode.
		struck.rig.synth.noteOff(0, 48, 1, 0.0);
		struck.rig.synth.noteOn(0, 55, 2, 1.0);
		held.rig.synth.noteOn(0, 55, 2, 1.0);
		struck.play(64);
		held.play(64);
		REQUIRE(struck.rig.synth.newestVoiceSlotForTest() == slot); // the same voice
		REQUIRE(struck.part(0).partialsResetsForTest() == resets);  // and its bank was not re-primed
		CHECK(worstRelation(struck, held) < 2e-3);
		const double early = accumulatorGap(struck, held, 0);
		struck.play(64 * 20);
		held.play(64 * 20);
		CHECK(worstRelation(struck, held) < 2e-3);
		const double late = accumulatorGap(struck, held, 0);
		CAPTURE(early);
		CAPTURE(late);
		if (mode == feathers::OscMode::Catchup) {
			CHECK(late - early > 0.05); // still bending toward the phase it aims at, block by block
		}
		else if (mode == feathers::OscMode::Digi) {
			CHECK(std::abs(early) > 0.05);        // the restart, at once
			CHECK(std::abs(late - early) < 1e-6); // and no bend after it
		}
		else {
			// ANALOG RE-LOCKS TO THE SONG for the new key (Synth::lockPhaseToSong): at once, and then it runs on unbent.
			CHECK(std::abs(early) > 1e-3);
			CHECK(std::abs(late - early) < 1e-6);
		}
	}
}

TEST_CASE("additive osc mode: heard — a stacked note's attack is the same sound in Digi and Catchup and a different one in Analog, as on V5") {
	// THE SECOND READING, in the audio, of the note-on case's values (a key feature that Additive was missing): the
	// first 10 ms of a four-copy detuned note. Catchup with nothing to adopt IS Digi,
	// sample for sample; Analog's copies start apart, so its attack differs from Digi's by a large fraction of its own peak.
	// The same three renders on V5 part the same way, so the modes are at parity in what they are heard to do.
	for (const OscType engine : { OscType::Additive, OscType::V5 }) {
		CAPTURE(static_cast<int>(engine));
		const auto attack = [&](feathers::OscMode mode) {
			SynthRig rig(engine);
			rig.set(ParamId::StackCount, kModeParts);
			rig.set(ParamId::StackDetune, 0.3);
			rig.set(ParamId::OscMode, static_cast<double>(mode));
			rig.synth.setTransport(1.25, 2.5, true);
			std::vector<float> l, r;
			rig.play(256, l, r, 64);
			rig.synth.noteOn(0, 48, 1, 1.0);
			rig.play(480, l, r, 64);
			return l;
		};
		const std::vector<float> digi = attack(feathers::OscMode::Digi), catchup = attack(feathers::OscMode::Catchup),
		                         analog = attack(feathers::OscMode::Analog);
		CHECK(catchup == digi);
		float peak = 0.0f, diff = 0.0f;
		for (size_t i = 0; i < digi.size(); ++i) {
			peak = std::max(peak, std::abs(digi[i]));
			diff = std::max(diff, std::abs(analog[i] - digi[i]));
		}
		REQUIRE(peak > 0.01f);
		CAPTURE(peak);
		CAPTURE(diff);
		CHECK(diff > 0.25f * peak);
	}
}

TEST_CASE("idle overhead: at rest the drawer's route scan and the strip's modulated copy run zero times, and their answers are the ones a fresh scan and copy give") {
	// THE IDLE-OVERHEAD PASS (2026-11-05). `applyParamsRoad` scanned every sound's drawer controls for per-voice and pair
	// inputs on every tick, and `tickEnvelopes` copied the whole strip (`modulated_ = rack_`) on every tick, at rest as
	// anywhere. The scan now runs when the routes are compiled again and the copy when `rack_`, the lane or the row count
	// moved. Pinned BY VALUE: the counters do not move at rest; the drawer's answers are a full scan's; the modulated row
	// is `rack_`'s row with the device's controls read from the engine; and a route, then a rack, written later is seen.
	namespace st = feathers::mod::stereo;
	SynthRig rig(OscType::Additive);
	soloA(rig);
	rig.set(dp(0, SpectralControl::FilterOn), 1.0);
	feathers::SpectralState on;
	on.on = true;
	rig.synth.setSpectralState(on);
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	const feathers::device::Descriptor* desc = feathers::deviceForTag(st::kTraits.tag);
	REQUIRE(desc != nullptr);
	rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
	rack.device[0].field[static_cast<size_t>(st::Param::In)] = 0.3f;
	rig.synth.provisionDevices(feathers::FxChain {}, rack);
	rig.synth.setModRack(rack);
	std::vector<float> l, r;
	rig.play(1024, l, r);
	// THE INDEPENDENT RESTATEMENTS: a full scan of the drawer's destinations, and `rack_`'s row with the engine's controls.
	const auto scanVaries = [&] {
		bool any = false;
		for (size_t s = 0; s < feathers::kMaxSounds; ++s) {
			for (const SpectralControl c : feathers::kSpectralContinuousControls) {
				any = any || rig.params.variesPerVoice(dp(s, c));
			}
		}
		return any;
	};
	const auto scanPairs = [&] {
		bool any = false;
		for (size_t s = 0; s < feathers::kMaxSounds; ++s) {
			for (const SpectralControl c : feathers::kSpectralStereoControls) {
				any = any || rig.params.stereoLive(dp(s, c));
			}
		}
		return any;
	};
	const auto expectedRow = [&] {
		feathers::DeviceRow row = rig.synth.modRackForTest().device[0];
		const feathers::LaneRack& lane = rig.synth.laneForTest();
		const size_t engineSlot = lane.slotOfStatePosition(feathers::kStripChain, 0);
		const size_t controls = std::min(row.fieldsPresent, desc->params.size());
		for (size_t c = 0; c < controls; ++c) {
			row.field[c] = lane.engineForTest().modulatedFieldAt(engineSlot, c);
		}
		return row;
	};
	const uint64_t scans = rig.synth.drawerScansForTest();
	const uint64_t copies = rig.synth.modulatedCopiesForTest();
	for (int b = 0; b < 16; ++b) {
		rig.play(256, l, r);
		CHECK(rig.synth.drawerVariesForTest() == scanVaries());
		CHECK(rig.synth.drawerPairLiveForTest() == scanPairs());
		CHECK(rig.synth.modulatedRack().device[0] == expectedRow());
	}
	CHECK(rig.synth.drawerScansForTest() == scans);   // AT REST: no scan
	CHECK(rig.synth.modulatedCopiesForTest() == copies); // and no copy
	CHECK_FALSE(rig.synth.drawerPairLiveForTest());
	// A ROUTE WRITTEN LATER is a compile, so a scan, and the pair it makes is seen at once.
	const int cutoff = feathers::modDestinationIndexOf(dp(0, SpectralControl::FilterCutoff));
	REQUIRE(cutoff >= 0);
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), static_cast<size_t>(st::Output::Split)),
	           feathers::backplane::Destination::core(static_cast<size_t>(cutoff)), 0.5f);
	routes.publish(rig.params);
	rig.play(256, l, r);
	CHECK(rig.synth.drawerScansForTest() > scans);
	REQUIRE(scanPairs());
	CHECK(rig.synth.drawerPairLiveForTest()); // NOT VACUOUS: the answer moved, and the cache followed it
	// A RACK HANDED IN LATER is copied again: the row's In is the new one, read through the engine.
	const uint64_t copiesBefore = rig.synth.modulatedCopiesForTest();
	rack.device[0].field[static_cast<size_t>(st::Param::In)] = 0.6f;
	// AND A WORD PAST THE DEVICE'S CONTROLS, which only the copy carries (the engine reads none of it): a skipped copy
	// would leave the old row's there however the engine's controls read.
	rack.device[0].field[feathers::kMaxDeviceFields - 1] = 0.25f;
	REQUIRE(desc->params.size() < feathers::kMaxDeviceFields - 1);
	rig.synth.setModRack(rack);
	rig.play(256, l, r);
	CHECK(rig.synth.modulatedCopiesForTest() > copiesBefore);
	CHECK(rig.synth.modulatedRack().device[0] == expectedRow());
	CHECK(rig.synth.modRackForTest().device[0].field[static_cast<size_t>(st::Param::In)] == 0.6f);
}
