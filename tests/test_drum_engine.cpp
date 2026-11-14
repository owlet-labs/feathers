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

// THE DRUM ENGINE IN THE BANK AND THE SYNTH (stages S3-S4): each pad's
// fields at its own place, the kit's note map, chokes, per-pad voices, the Trigger, the derived seeds. Every claim is read
// as a value through a seam and restated independently; a buffer is read only for exact silence.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/character_bank.h"
#include "dsp/drum_voice.h"
#include "dsp/field_override.h"
#include "dsp/slice_build.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "param_routes.h"
#include "mod/arp/pattern.h"
#include "plugin/character_params.h"
#include "plugin/device_registry.h"
#include "rack/rack_bridge.h"
#include "plugin/params.h"
#include "plugin/rotate_sound.h"
#include "plugin/synth.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <numbers>
#include <vector>

namespace {

using feathers::Character;
using feathers::CharacterBank;
using feathers::DrumControl;
using feathers::OscEngine;
using feathers::OscType;
using feathers::ParamId;
using feathers::ParamSet;
using feathers::PreparedCharacter;
using feathers::SoundCoord;
using feathers::Synth;
using feathers::Voice;
using feathers::drumParam;
namespace drum = feathers::drum;

constexpr double kRate = 48000.0;

SoundCoord sound(uint8_t slot, uint16_t zone, float offset, double weight, double travelX = 0.0, double travelY = 0.0) {
	SoundCoord s;
	s.slot = slot;
	s.zone = zone;
	s.phaseOffset = offset;
	s.weight = weight;
	s.travelX = travelX;
	s.travelY = travelY;
	return s;
}

PreparedCharacter& prepared(const Character& c) {
	static PreparedCharacter p; // large: kept off the stack
	feathers::PartialsScratch scratch;
	CharacterBank::buildOne(c, p, scratch);
	return p;
}

/// ONE SOUND'S DRUM FIELDS, restated: its home, its frame (its own pin or the character's, its own residue), its travel,
/// through the guests and its scalers.
drum::Fields soloFields(const Character& c, const SoundCoord& s) {
	const feathers::SliceOrientationPin& pin = s.orientation.pinned ? s.orientation : c.orientation;
	const double home = feathers::sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame =
	    feathers::sliceFrameFor<feathers::kPartialTriangles>(feathers::rotateFor(pin), home, s.residue.partials);
	return feathers::drumFieldsWithOverrides(feathers::readDrumFields(frame, home, s.travelX, s.travelY), s.drumBias);
}

std::array<double, 7> asArray(const drum::Fields& f) {
	return { f.pitch, f.length, f.sweep, f.tone, f.noise, f.snap, f.metal };
}

struct Rig {
	ParamSet params;
	Synth synth { params };
	explicit Rig(bool kit = true) {
		synth.activate(kRate, 4096);
		set(ParamId::OscType, static_cast<double>(OscType::Drum));
		set(ParamId::DrumMode, kit ? 1.0 : 0.0);
		set(ParamId::Level, 1.0);
		set(ParamId::VoiceLevel, 1.0); // a bare Synth has no bridge to route the strip envelope (test_synth's Rig)
	}
	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }
	void pad(size_t slot, DrumControl c, double value) { set(drumParam(slot, c), value); }
	/// Render and answer whether every sample of both ears was exactly zero.
	bool play(uint32_t frames, int block = 256) {
		std::vector<float> l(static_cast<size_t>(block));
		std::vector<float> r(static_cast<size_t>(block));
		bool silent = true;
		for (uint32_t at = 0; at < frames; at += static_cast<uint32_t>(block)) {
			const int n = static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(block), frames - at));
			synth.render(l.data(), r.data(), n);
			for (int i = 0; i < n; ++i) {
				silent = silent && l[static_cast<size_t>(i)] == 0.0f && r[static_cast<size_t>(i)] == 0.0f;
			}
		}
		return silent;
	}
	/// The slots sounding (not Idle, not Stealing), and the one that plays `pad`.
	std::vector<int> playing() const {
		std::vector<int> out;
		for (int s = 0; s < feathers::kVoiceSlots; ++s) {
			const Voice::Stage st = synth.voiceStageForTest(s);
			if (st != Voice::Stage::Idle && st != Voice::Stage::Stealing) {
				out.push_back(s);
			}
		}
		return out;
	}
	int slotOfPad(uint8_t pad) const {
		for (const int s : playing()) {
			if (synth.voiceDrumPadForTest(s) == pad) {
				return s;
			}
		}
		return -1;
	}
};

double noteHz(int key) { return 440.0 * std::pow(2.0, (key - 69) / 12.0); }

} // namespace

TEST_CASE("drum bank: a Kit character builds each pad's fields at its own place, unblended, at its slot") {
	Character c;
	c.engine = OscEngine::Drum;
	c.drumKit = true;
	c.sounds[c.count++] = sound(0, 101, 0.0f, 0.9);
	c.sounds[c.count++] = sound(3, 613, 0.37f, 0.1, 0.21, -0.13);
	c.sounds[1].drumBias.tone = 0.8f;
	PreparedCharacter& p = prepared(c);
	CHECK(p.engine == OscEngine::Drum);
	CHECK(p.gridFor(0) == nullptr);
	CHECK(p.eval(nullptr, 0.3) == 0.0f);
	CHECK(p.drum.kit);
	CHECK(p.drum.pads[0] == soloFields(c, c.sounds[0]));
	CHECK(p.drum.pads[3] == soloFields(c, c.sounds[1]));
	// NOT BLENDED: the weights moved nothing, and the two pads differ (or the claim is vacuous).
	CHECK(p.drum.pads[0] != p.drum.pads[3]);
	// A slot with no sound keeps the default fields.
	CHECK(p.drum.pads[5] == drum::Fields {});
}

TEST_CASE("drum bank: Pitched blends the live sounds' fields by their shares into pad 0") {
	Character c;
	c.engine = OscEngine::Drum;
	c.drumKit = false;
	c.sounds[c.count++] = sound(0, 101, 0.0f, 0.25);
	c.sounds[c.count++] = sound(1, 613, 0.37f, 0.75, 0.21, -0.13);
	const auto a = asArray(soloFields(c, c.sounds[0]));
	const auto b = asArray(soloFields(c, c.sounds[1]));
	const auto got = asArray(feathers::buildDrumFor(c).pads[0]);
	double moved = 0.0;
	for (size_t i = 0; i < 7; ++i) {
		CAPTURE(i);
		CHECK(std::abs(got[i] - (0.25 * a[i] + 0.75 * b[i])) < 1.0e-6);
		moved = std::max(moved, std::abs(a[i] - b[i]));
	}
	CHECK(moved > 1.0e-3); // the two sounds differ, so the blend is a claim
}

TEST_CASE("drum bank: each scaler at neutral is its field to the bit, at 0 nothing and at 1 full") {
	Character c;
	c.engine = OscEngine::Drum;
	c.drumKit = true;
	c.sounds[c.count++] = sound(2, 333, 0.11f, 1.0, 0.05, 0.4);
	const drum::Fields raw = soloFields(c, c.sounds[0]);
	CHECK(feathers::buildDrumFor(c).pads[2] == raw);
	c.sounds[0].drumBias = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	CHECK(feathers::buildDrumFor(c).pads[2] == drum::Fields { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f });
	c.sounds[0].drumBias = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
	CHECK(feathers::buildDrumFor(c).pads[2] == drum::Fields { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f });
}

TEST_CASE("drum kit: the default pads' notes are Pattern Drum's lanes, in its order") {
	for (size_t lane = 0; lane < feathers::mod::arp::pattern::kDrumLanes; ++lane) {
		CAPTURE(lane);
		CHECK(static_cast<int>(feathers::kDrumPadDefaults[lane].note) == feathers::mod::arp::pattern::kDrumDefaultNotes[lane]);
		const feathers::ParamDesc* d = feathers::findParam(static_cast<clap_id>(drumParam(lane, DrumControl::Note)));
		REQUIRE(d != nullptr);
		CHECK(d->def == static_cast<double>(feathers::mod::arp::pattern::kDrumDefaultNotes[lane]));
	}
	// And the closed and open hats share choke group 1 by default; nothing else is choked.
	for (size_t slot = 0; slot < 8; ++slot) {
		CAPTURE(slot);
		CHECK(feathers::kDrumPadDefaults[slot].choke == (slot == 2 || slot == 3 ? 1 : 0));
	}
}

TEST_CASE("drum kit: a note plays the pad mapped to it at that pad's kind; an unmapped note plays nothing") {
	auto rig = std::make_unique<Rig>();
	rig->synth.noteOn(0, 38, 1, 1.0);
	(void)rig->play(256);
	const int snare = rig->slotOfPad(1);
	REQUIRE(snare >= 0);
	CHECK(rig->synth.voiceDrumHitForTest(snare).kind() == drum::Kind::Snare);
	CHECK(rig->synth.voiceDrumStrikesForTest(snare) == 1u);
	CHECK(rig->playing().size() == 1);
	rig->synth.noteOn(0, 60, 2, 1.0); // no pad is on 60
	(void)rig->play(256);
	CHECK(rig->playing().size() == 1);
	rig->synth.noteOn(0, 51, 3, 1.0); // the ride
	(void)rig->play(256);
	const int ride = rig->slotOfPad(7);
	REQUIRE(ride >= 0);
	CHECK(rig->synth.voiceDrumHitForTest(ride).kind() == drum::Kind::Metal);
}

TEST_CASE("drum kit: two pads on one note sound together, as a layer") {
	auto rig = std::make_unique<Rig>();
	rig->pad(4, DrumControl::Note, 36.0); // the clap onto the kick's note
	rig->synth.noteOn(0, 36, 1, 1.0);
	(void)rig->play(256);
	CHECK(rig->playing().size() == 2);
	REQUIRE(rig->slotOfPad(0) >= 0);
	REQUIRE(rig->slotOfPad(4) >= 0);
	CHECK(rig->synth.voiceDrumHitForTest(rig->slotOfPad(0)).kind() == drum::Kind::Kick);
	CHECK(rig->synth.voiceDrumHitForTest(rig->slotOfPad(4)).kind() == drum::Kind::Clap);
}

TEST_CASE("drum kit: a closed hat chokes the open hat, and a pad outside the group is untouched") {
	auto rig = std::make_unique<Rig>();
	rig->synth.noteOn(0, 46, 1, 1.0); // open hat, group 1
	rig->synth.noteOn(0, 36, 2, 1.0); // kick, no group
	(void)rig->play(512);
	const int open = rig->slotOfPad(3);
	const int kick = rig->slotOfPad(0);
	REQUIRE(open >= 0);
	REQUIRE(kick >= 0);
	rig->synth.noteOn(0, 42, 3, 1.0); // closed hat, group 1
	CHECK(rig->synth.voiceStageForTest(open) == Voice::Stage::Stealing);
	CHECK(rig->synth.voiceStageForTest(kick) != Voice::Stage::Stealing);
	CHECK(rig->synth.drumChokesForTest() == 1u);
	// With the open hat's group set Off, the same closed hat chokes nothing.
	auto off = std::make_unique<Rig>();
	off->pad(3, DrumControl::Choke, 0.0);
	off->synth.noteOn(0, 46, 1, 1.0);
	(void)off->play(512);
	const int open2 = off->slotOfPad(3);
	REQUIRE(open2 >= 0);
	off->synth.noteOn(0, 42, 2, 1.0);
	CHECK(off->synth.voiceStageForTest(open2) != Voice::Stage::Stealing);
	CHECK(off->synth.drumChokesForTest() == 0u);
}

TEST_CASE("drum kit: a pad's Voices bounds its own hits, the oldest stolen; another pad is not counted against it") {
	auto rig = std::make_unique<Rig>();
	// The kick is 1 voice by default: a second kick steals the first.
	rig->synth.noteOn(0, 36, 1, 1.0);
	(void)rig->play(256);
	const int first = rig->slotOfPad(0);
	REQUIRE(first >= 0);
	rig->synth.noteOn(0, 36, 2, 1.0);
	(void)rig->play(32);
	CHECK(rig->synth.voiceStageForTest(first) == Voice::Stage::Stealing);
	CHECK(rig->playing().size() == 1);
	// The snare is 2 voices: two ring together, and a third steals the oldest of them.
	auto snare = std::make_unique<Rig>();
	snare->synth.noteOn(0, 38, 1, 1.0);
	(void)snare->play(256);
	const int one = snare->slotOfPad(1);
	snare->synth.noteOn(0, 38, 2, 1.0);
	(void)snare->play(256);
	CHECK(snare->playing().size() == 2);
	snare->synth.noteOn(0, 38, 3, 1.0);
	(void)snare->play(32);
	CHECK(snare->synth.voiceStageForTest(one) == Voice::Stage::Stealing);
	CHECK(snare->playing().size() == 2);
	// And a kick beside them is its own pad's.
	snare->synth.noteOn(0, 36, 4, 1.0);
	(void)snare->play(32);
	CHECK(snare->playing().size() == 3);
}

TEST_CASE("drum kit: a One-shot pad ignores its note-off, plays out, and ends itself at exact silence") {
	auto rig = std::make_unique<Rig>();
	rig->pad(0, DrumControl::Decay, std::log2(0.05));
	rig->synth.noteOn(0, 36, 1, 1.0);
	(void)rig->play(256);
	const int kick = rig->slotOfPad(0);
	REQUIRE(kick >= 0);
	rig->synth.noteOff(0, 36, 1, 0.0);
	(void)rig->play(256);
	CHECK(rig->synth.voiceStageForTest(kick) != Voice::Stage::Release); // not released by its key
	CHECK_FALSE(rig->synth.voiceDrumHitForTest(kick).idle());
	// A 50 ms body (and a click) is exactly silent well inside two seconds; the voice then ends by itself.
	(void)rig->play(96000);
	CHECK(rig->synth.voiceStageForTest(kick) == Voice::Stage::Idle);
	CHECK(rig->synth.drumEndedForTest() >= 1u);
	CHECK(rig->play(4096)); // and the synth's output is exactly zero
	// HELD, it ends just the same: a One-shot never waits for its key.
	auto held = std::make_unique<Rig>();
	held->pad(0, DrumControl::Decay, std::log2(0.05));
	held->synth.noteOn(0, 36, 1, 1.0);
	(void)held->play(96000);
	CHECK(held->playing().empty());
}

TEST_CASE("drum kit: a Gate pad's note-off damps its hit to Release") {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::Trigger, 1.0); // the low tom, Gate
	rig->pad(5, DrumControl::Decay, std::log2(2.0));
	rig->pad(5, DrumControl::Release, std::log2(0.02));
	rig->synth.noteOn(0, 45, 1, 1.0);
	(void)rig->play(256);
	const int tom = rig->slotOfPad(5);
	REQUIRE(tom >= 0);
	const double held = rig->synth.voiceDrumHitForTest(tom).modeDecayPerSample(0);
	rig->synth.noteOff(0, 45, 1, 0.0);
	(void)rig->play(64);
	CHECK(rig->synth.voiceStageForTest(tom) == Voice::Stage::Release);
	const double want = std::pow(10.0, -3.0 / (0.02 * kRate));
	CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).modeDecayPerSample(0) - want) < 1.0e-12);
	CHECK(want < held);
}

TEST_CASE("drum kit: a pad's pitch is its kind's resting pitch at its Tune, whatever key plays it") {
	for (const int key : { 45, 60, 84 }) {
		CAPTURE(key);
		auto rig = std::make_unique<Rig>();
		rig->pad(5, DrumControl::Note, static_cast<double>(key));
		rig->pad(5, DrumControl::Tune, -5.0);
		rig->pad(5, DrumControl::FieldPitch, 0.0); // the terrain's Pitch field held at 0: −3 semitones
		rig->pad(5, DrumControl::FieldSweep, 0.0); // and no sweep, so the modes sit at rest
		rig->synth.noteOn(0, static_cast<int16_t>(key), 1, 1.0);
		(void)rig->play(256);
		const int tom = rig->slotOfPad(5);
		REQUIRE(tom >= 0);
		const double want = drum::kindBaseHz(drum::Kind::Membrane) * std::pow(2.0, (-5.0 - 3.0) / 12.0);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).laws().hz - want) < 1.0e-9);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).currentHz(0) - want) < 1.0e-9);
	}
}

TEST_CASE("drum pitched: every note is the engine's kind, at the key's pitch") {
	auto rig = std::make_unique<Rig>(false);
	rig->set(ParamId::DrumPitchedKind, 1.0); // Membrane
	rig->pad(0, DrumControl::FieldPitch, 0.0); // −0.5 semitones in Pitched mode
	rig->pad(0, DrumControl::FieldSweep, 0.0);
	rig->synth.noteOn(0, 60, 1, 1.0);
	rig->synth.noteOn(0, 67, 2, 1.0);
	(void)rig->play(256);
	const auto slots = rig->playing();
	REQUIRE(slots.size() == 2);
	for (const int s : slots) {
		const int key = rig->synth.voiceKeyForTest(s);
		CAPTURE(key);
		CHECK(rig->synth.voiceDrumHitForTest(s).kind() == drum::Kind::Membrane);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(s).laws().hz - noteHz(key) * std::pow(2.0, -0.5 / 12.0)) < 1.0e-6);
	}
	// The kit's map is not read: 60 is no pad's note and it played.
}

TEST_CASE("drum: a hit's seed is derived — from the song position while playing, from the pad's strike count when "
          "stopped — never from anything incidental") {
	const auto seedAt = [](double songSeconds, bool playing, int strikes, double seedParam) {
		auto rig = std::make_unique<Rig>();
		rig->set(ParamId::DrumSeed, seedParam);
		uint64_t seed = 0;
		for (int i = 0; i < strikes; ++i) {
			rig->synth.setTransport(songSeconds, songSeconds * 2.0, playing);
			rig->synth.noteOn(0, 38, i + 1, 1.0);
			(void)rig->play(256);
			const int snare = rig->slotOfPad(1);
			REQUIRE(snare >= 0);
			seed = rig->synth.voiceDrumHitForTest(snare).seed();
			rig->synth.noteOff(0, 38, i + 1, 0.0);
			(void)rig->play(256);
		}
		return seed;
	};
	CHECK(seedAt(3.25, true, 1, 0.0) == seedAt(3.25, true, 1, 0.0));
	CHECK(seedAt(3.25, true, 1, 0.0) != seedAt(4.5, true, 1, 0.0));
	CHECK(seedAt(3.25, true, 1, 0.0) != seedAt(3.25, true, 1, 7.0));
	CHECK(seedAt(0.0, false, 2, 0.0) == seedAt(0.0, false, 2, 0.0));
	CHECK(seedAt(0.0, false, 2, 0.0) != seedAt(0.0, false, 3, 0.0));
}

TEST_CASE("drum: no other engine strikes a drum, and a Drum voice never plucks Pluck's road") {
	for (const OscType e : { OscType::V5, OscType::Additive, OscType::Pluck }) {
		CAPTURE(static_cast<int>(e));
		auto rig = std::make_unique<Rig>();
		rig->set(ParamId::OscType, static_cast<double>(e));
		rig->synth.noteOn(0, 36, 1, 1.0);
		(void)rig->play(1024);
		for (const int s : rig->playing()) {
			CHECK(rig->synth.voiceDrumStrikesForTest(s) == 0u);
			CHECK(rig->synth.voiceDrumHitForTest(s).idle());
		}
	}
}

TEST_CASE("drum kit: a panned pad takes the stereo road; a centred kit does not") {
	auto rig = std::make_unique<Rig>();
	(void)rig->play(64);
	CHECK_FALSE(rig->synth.drumKitForTest().panned);
	rig->pad(2, DrumControl::Pan, 0.5);
	(void)rig->play(64);
	CHECK(rig->synth.drumKitForTest().panned);
	CHECK(rig->synth.drumKitForTest().pads[2].pan == 0.5f);
}

TEST_CASE("drum kit: every sound is a pad whatever its switch, at its home plus the Walk; Weather's puck term does not reach "
          "it, and Pitched keeps the ordinary blend") {
	ParamSet params; // the defaults: only some sounds switched on
	const auto build = [&params](double walkX, double walkY, bool kit) {
		params.setBase(static_cast<clap_id>(ParamId::DrumMode), kit ? 1.0 : 0.0);
		params.setBase(static_cast<clap_id>(ParamId::DrumWalkX), walkX);
		params.setBase(static_cast<clap_id>(ParamId::DrumWalkY), walkY);
		params.setBase(static_cast<clap_id>(ParamId::Weather), 1.0);
		params.setBase(static_cast<clap_id>(ParamId::PosX), 0.3);
		return feathers::characterFromParams([&params](ParamId id) { return params.base(id); }, OscType::Drum,
		                                     feathers::AuditionTuning {}, feathers::AnchorPoint {}, feathers::SoundTravel {},
		                                     feathers::SoundOrientations {}, feathers::SoundResidue {}, feathers::SoundGamma {},
		                                     feathers::Preview {}, feathers::EarSide::Head);
	};
	const Character kit = build(0.25, -0.5, true);
	CHECK(kit.engine == OscEngine::Drum);
	CHECK(kit.drumKit);
	REQUIRE(kit.count == 8);
	for (uint8_t i = 0; i < 8; ++i) {
		CAPTURE(i);
		CHECK(kit.sounds[i].slot == i);
		CHECK(kit.sounds[i].travelX == 0.25);
		CHECK(kit.sounds[i].travelY == -0.5);
	}
	// At Walk 0 every pad is exactly home.
	const Character still = build(0.0, 0.0, true);
	for (uint8_t i = 0; i < 8; ++i) {
		CHECK(still.sounds[i].travelX == 0.0);
		CHECK(still.sounds[i].travelY == 0.0);
	}
	// PITCHED: the switches decide membership and the puck's Weather reaches the sounds, as on every engine.
	const Character pitched = build(0.25, -0.5, false);
	CHECK_FALSE(pitched.drumKit);
	CHECK(pitched.count < 8);
	CHECK(pitched.sounds[0].travelX != 0.25);
}

TEST_CASE("drum kit: a snare pad's own envelopes and bell reach its hit, through its params, by value") {
	auto rig = std::make_unique<Rig>();
	rig->pad(1, DrumControl::WireDecay, std::log2(0.12));
	rig->pad(1, DrumControl::FieldLength, 1.0); // Length full: ×4 on the body's and the wires' T60 alike
	rig->pad(1, DrumControl::ClickDecay, std::log2(0.003));
	rig->pad(1, DrumControl::Wires, 0.75); // Gated, exactly: its anchor's Shapes (body 0.35, wires 0.45)
	rig->pad(1, DrumControl::FieldSweep, 1.0); // the wires' sweep yields to the Tension field (§14 lever 1)
	rig->pad(1, DrumControl::BellStart, std::log2(4000.0));
	rig->pad(1, DrumControl::BellEnd, std::log2(800.0));
	rig->pad(1, DrumControl::BellTime, std::log2(0.03));
	rig->pad(1, DrumControl::Bells, 0.2); // Body, exactly: the first bell at 0.5
	rig->synth.noteOn(0, 38, 1, 1.0);
	(void)rig->play(64);
	const int snare = rig->slotOfPad(1);
	REQUIRE(snare >= 0);
	const drum::Laws& l = rig->synth.voiceDrumHitForTest(snare).laws();
	CHECK(std::abs(l.wireT60 - 0.48) < 1.0e-9);
	CHECK(std::abs(l.clickT60 - 0.003) < 1.0e-12);
	CHECK(std::abs(l.bodyGate - 0.35 * l.t60) < 1.0e-12);
	CHECK(std::abs(l.wireGate - 0.45 * l.wireT60) < 1.0e-12);
	// The field read back from the head's own glide at velocity 1: sweepOctaves = Sweep × 0.5 (the Snare's range) × its ±2%.
	const double sweepField = l.sweepOctaves / (drum::kindSweepOctaves(drum::Kind::Snare) * rig->synth.voiceDrumHitForTest(snare).tensionVariationForTest());
	CHECK(std::abs(l.wireSweepOctaves - sweepField) < 1.0e-9);
	CHECK(l.wireSweepOctaves > 0.0);
	CHECK(std::abs(l.bellStartHz - 4000.0) < 1.0e-6);
	CHECK(std::abs(l.bellEndHz - 800.0) < 1.0e-6);
	CHECK(std::abs(l.bellTau - 0.01) < 1.0e-12);
	CHECK(l.bellLevel == 0.5);
	// A kick is not a snare: its laws carry the defaults, unread.
	rig->pad(0, DrumControl::WireDecay, std::log2(0.5));
	rig->synth.noteOn(0, 36, 2, 1.0);
	(void)rig->play(64);
	CHECK(rig->synth.voiceDrumHitForTest(rig->slotOfPad(0)).kind() == drum::Kind::Kick);
}

TEST_CASE("drum kit: a kick pad's Hold, fishtail, click sweep and start phase reach its hit, through its params, by value") {
	auto rig = std::make_unique<Rig>();
	rig->pad(0, DrumControl::KickHold, 25.0);
	rig->pad(0, DrumControl::KickDip, 0.5);
	rig->pad(0, DrumControl::KickDipTime, 20.0);
	rig->pad(0, DrumControl::ClickSweep, 2.5);
	rig->pad(0, DrumControl::ClickSweepTime, std::log2(0.002));
	rig->pad(0, DrumControl::StartPhase, 90.0);
	rig->synth.noteOn(0, 36, 1, 1.0);
	(void)rig->play(32);
	const int kick = rig->slotOfPad(0);
	REQUIRE(kick >= 0);
	const drum::Laws& l = rig->synth.voiceDrumHitForTest(kick).laws();
	CHECK(std::abs(l.kickHold - 0.025) < 1.0e-12);
	CHECK(l.kickDip == 0.5);
	CHECK(std::abs(l.kickDipTime - 0.02) < 1.0e-12);
	CHECK(l.clickSweepOctaves == 2.5);
	CHECK(std::abs(l.clickSweepTau - 0.002 / 3.0) < 1.0e-12);
	CHECK(std::abs(l.startPhase - std::numbers::pi / 2.0) < 1.0e-12);
	// The second pad block holds them: past the first block's 32, at 0x0700 + slot·0x20.
	// THE TWO BLOCKS (the rule, not a pinned number: ids are free while nobody keeps patches): index c < 32 at 0x0600 + slot·0x20
	// + c, the rest at 0x0700 + slot·0x20 + (c − 32).
	const auto addr = [](size_t slot, DrumControl c) {
		const auto i = static_cast<uint32_t>(c);
		return i < 32u ? 0x0600u + static_cast<uint32_t>(slot) * 0x20u + i : 0x0700u + static_cast<uint32_t>(slot) * 0x20u + (i - 32u);
	};
	CHECK(static_cast<uint32_t>(drumParam(3, DrumControl::KickHold)) == addr(3, DrumControl::KickHold));
	CHECK(static_cast<uint32_t>(drumParam(3, DrumControl::Mute)) == addr(3, DrumControl::Mute));
	CHECK(static_cast<uint32_t>(DrumControl::Mute) >= 32u); // and the second block is in use
}

TEST_CASE("drum kit: a pad's Drive Curve and Drive Tone, and a clap pad's Q and High Pass, reach its hit, through its params, by value") {
	auto rig = std::make_unique<Rig>();
	// Pad B (the Snare, note 38): Drive on the Tube curve, tilted up.
	rig->pad(1, DrumControl::Drive, 0.4);
	rig->pad(1, DrumControl::DriveCurve, 1.0);
	rig->pad(1, DrumControl::DriveTone, 0.8);
	// Pad E (the Clap, note 39): Soft Clip, its Q and its high-pass.
	rig->pad(4, DrumControl::Drive, 0.2);
	rig->pad(4, DrumControl::DriveCurve, 2.0);
	rig->pad(4, DrumControl::ClapQ, 4.0);
	rig->pad(4, DrumControl::ClapHighPass, std::log2(400.0));
	rig->synth.noteOn(0, 38, 1, 1.0);
	rig->synth.noteOn(0, 39, 2, 1.0);
	(void)rig->play(32);
	const int snare = rig->slotOfPad(1);
	const int clap = rig->slotOfPad(4);
	REQUIRE(snare >= 0);
	REQUIRE(clap >= 0);
	const drum::Hit& s = rig->synth.voiceDrumHitForTest(snare);
	CHECK(s.laws().driveCurve == 1);
	CHECK(std::abs(s.laws().driveTone - 0.8) < 1.0e-12);
	CHECK(s.driveCurveForTest() == 1);
	CHECK(std::abs(static_cast<double>(s.driveGainForTest()) - (1.0 + 9.0 * 0.4)) < 1.0e-5);
	CHECK(std::abs(static_cast<double>(s.driveTiltForTest()) - 0.6) < 1.0e-6);
	const drum::Hit& c = rig->synth.voiceDrumHitForTest(clap);
	CHECK(c.driveCurveForTest() == 2);
	CHECK(std::abs(c.laws().clapQ - 4.0) < 1.0e-12);
	CHECK(std::abs(c.clapQForTest() - 4.0) < 1.0e-5);
	CHECK(std::abs(c.laws().clapHighPassHz - 400.0) < 1.0e-9);
	CHECK(std::abs(static_cast<double>(c.clapHighPassCoefForTest()) - (1.0 - std::exp(-2.0 * std::numbers::pi * 400.0 / 48000.0)))
	      < 1.0e-6);
	// The second pad block holds them, after the Kick's six.
	CHECK(static_cast<uint32_t>(DrumControl::DriveCurve) >= 32u);
	CHECK(static_cast<uint32_t>(drumParam(2, DrumControl::DriveCurve)) == 0x0700u + 2u * 0x20u + (static_cast<uint32_t>(DrumControl::DriveCurve) - 32u));
	CHECK(static_cast<uint32_t>(drumParam(2, DrumControl::ClapHighPass)) == 0x0700u + 2u * 0x20u + (static_cast<uint32_t>(DrumControl::ClapHighPass) - 32u));
}

TEST_CASE("drum kit: a snare pad's Bells zone reaches its hit by value; the bells' Q is the place's Snap and a linked ratio its Metal") {
	auto rig = std::make_unique<Rig>();
	rig->pad(1, DrumControl::Bells, 4.0 / 5.0); // Knock, exactly: both bells, the second Free
	rig->pad(1, DrumControl::Bell2Hz, std::log2(800.0));
	rig->pad(1, DrumControl::Bell2Time, std::log2(0.05));
	rig->synth.noteOn(0, 38, 1, 0.5); // velocity 0.5: the hit's Snap is the place's
	(void)rig->play(32);
	const int snare = rig->slotOfPad(1);
	REQUIRE(snare >= 0);
	const drum::Hit& h = rig->synth.voiceDrumHitForTest(snare);
	const drum::Laws& l = h.laws();
	CHECK_FALSE(l.bell2Linked);
	CHECK(l.bellLevel == doctest::Approx(0.6));
	CHECK(l.bell2Level == doctest::Approx(0.4));
	CHECK(std::abs(l.bell2Hz - 800.0) < 1.0e-9);
	CHECK(std::abs(l.bell2T60 - 0.05) < 1.0e-12);
	// THE Q IS THE PLACE'S SNAP (at velocity 0.5 the hit's Snap is the field's) × the zone's scale: Knock's 0.6, written out.
	CHECK(l.bellQ == doctest::Approx((2.0 + 10.0 * static_cast<double>(l.snap)) * 0.6));
	CHECK(l.bell2Q == doctest::Approx((1.0 + 6.0 * static_cast<double>(l.snap)) * 0.6));
	CHECK(std::abs(h.bell2HzForTest() - 800.0) < 1.0e-9);
	// LINKED (Shell): the second bell at Metal's ratio, 1.3 + 1.2 × Metal, above the first.
	auto shell = std::make_unique<Rig>();
	shell->pad(1, DrumControl::Bells, 2.0 / 5.0);
	shell->synth.noteOn(0, 38, 1, 0.5); // velocity 0.5: the hit's Snap is the place's
	(void)shell->play(32);
	const drum::Laws& s = shell->synth.voiceDrumHitForTest(shell->slotOfPad(1)).laws();
	CHECK(s.bell2Linked);
	CHECK(s.bell2Ratio == doctest::Approx(1.3 + 1.2 * static_cast<double>(s.metal)));
	CHECK(s.bell2Level == doctest::Approx(0.35));
}

TEST_CASE("drum kit: a pad's Velocity zone and Velocity Noise reach its hit, through its params, by value") {
	// Two hits of pad B at half velocity, the tension law in full and then not at all: the depths (each over its own ±2%)
	// stand at velocity² = a quarter of each other.
	const auto hit = [](double velZone, double velNoise, double velocity, double& depth, float& noise) {
		auto rig = std::make_unique<Rig>();
		rig->pad(1, DrumControl::FieldSweep, 1.0);
		rig->pad(1, DrumControl::VelocityZone, velZone);
		rig->pad(1, DrumControl::VelocityNoise, velNoise);
		rig->synth.noteOn(0, 38, 1, velocity);
		(void)rig->play(32);
		const int snare = rig->slotOfPad(1);
		REQUIRE(snare >= 0);
		const drum::Hit& h = rig->synth.voiceDrumHitForTest(snare);
		depth = h.laws().sweepOctaves / h.tensionVariationForTest();
		noise = h.laws().noise;
	};
	double full = 0.0;
	double none = 0.0;
	float n0 = 0.0f;
	float n1 = 0.0f;
	hit(2.0 / 3.0, 0.0, 0.5, full, n0); // Tension: the law in full
	hit(1.0 / 3.0, 0.0, 0.5, none, n0); // Tone: no tension
	REQUIRE(none > 1.0e-3);
	CHECK(std::abs(full / none - 0.25) < 1.0e-9);
	// Velocity Noise at full velocity moves the noise share by depth × 0.5.
	hit(2.0 / 3.0, 0.0, 1.0, full, n0);
	hit(2.0 / 3.0, 0.4, 1.0, full, n1);
	CHECK(std::abs(static_cast<double>(n1 - n0) - 0.4 * 0.5) < 1.0e-6);

}

namespace {

/// WOOD, as the design describes it: a short hit (its body's T60 at most 0.6 of Decay), modes off the
/// membrane's Bessel ratios (the second mode at least 2.2 times the first: past the clamped plate's 2.08, toward the bar's
/// 2.76), and damping rising with frequency (the second mode's T60 at most a quarter of the first's). Read from the laws a
/// Membrane pad plays at those fields, at the default Decay.
bool woody(const drum::Fields& f) {
	drum::Controls c;
	const drum::Laws l = drum::lawsFor(drum::Kind::Membrane, f, c, 110.0, 0.8);
	const double r1 = drum::modeRatio(1, l.metal);
	return l.t60 <= 0.6 * c.decaySeconds && r1 >= 2.2 && drum::modeT60(l.t60, r1, l.metal) <= 0.25 * l.t60;
}

} // namespace

TEST_CASE("woody country exists on the terrain: short, non-Bessel, high-frequency-damped hits, measured over zones and travel") {
	Character c;
	c.engine = OscEngine::Drum;
	c.drumKit = true;
	int places = 0;
	int wood = 0;
	SoundCoord first {};
	bool found = false;
	for (int zone = 0; zone < 1024; zone += 16) {
		for (int ix = -4; ix <= 4; ++ix) {
			for (int iy = -4; iy <= 4; ++iy) {
				const SoundCoord s = sound(0, static_cast<uint16_t>(zone), 0.0f, 1.0, 0.25 * ix, 0.25 * iy);
				++places;
				if (woody(soloFields(c, s))) {
					++wood;
					if (!found) {
						first = s;
						found = true;
					}
				}
			}
		}
	}
	MESSAGE("woody: " << wood << " of " << places << " places (" << 100.0 * wood / places << "%); first at zone " << first.zone
	                  << ", travel " << first.travelX << ", " << first.travelY);
	CHECK(found);
	CHECK(wood * 100 >= places); // at least 1% of the field: country, not a point
}

TEST_CASE("woody country, at a defined place (zone 0, travel 0.5, -1): a Membrane pad there plays wood's damping profile, by value") {
	Character c;
	c.engine = OscEngine::Drum;
	c.drumKit = true;
	const SoundCoord s = sound(0, 0, 0.0f, 1.0, 0.5, -1.0);
	const drum::Fields f = soloFields(c, s);
	REQUIRE(woody(f));
	// The voice at those fields: its modes where the structure morph puts them, each decaying at T60 · ratio^−m with m the
	// Material exponent from the place's Metal, all written out here from the laws.
	drum::Controls k;
	const drum::Laws l = drum::lawsFor(drum::Kind::Membrane, f, k, 110.0, 0.8);
	drum::Hit hit;
	hit.prepare(kRate);
	hit.strike(drum::Kind::Membrane, l, 5, 1.0f);
	const double m = 0.6 + 1.4 * static_cast<double>(f.metal);
	CHECK(m >= 1.37); // damping rising steeply with frequency
	for (const int i : { 1, 2, 4 }) {
		CAPTURE(i);
		const double ratio = hit.currentHz(i) / hit.currentHz(0);
		CHECK(std::abs(ratio - drum::modeRatio(i, f.metal)) < 1.0e-9);
		const double t60 = l.t60 * std::pow(ratio, -m);
		CHECK(std::abs(hit.modeDecayPerSample(i) - std::pow(10.0, -3.0 / (t60 * kRate))) < 1.0e-12);
	}
	CHECK(hit.currentHz(1) / hit.currentHz(0) > 2.2); // off the membrane's 1.59
	CHECK(l.t60 <= 0.6 * k.decaySeconds);              // short
}

TEST_CASE("drum kit: a pad's Wires zone and Width reach its hit by value, and Width takes the stereo road") {
	auto rig = std::make_unique<Rig>();
	rig->pad(1, DrumControl::Wires, 0.5); // Buzzy, exactly
	rig->pad(1, DrumControl::Width, 0.6);
	rig->synth.noteOn(0, 38, 1, 1.0);
	std::vector<float> l(256);
	std::vector<float> r(256);
	rig->synth.render(l.data(), r.data(), 256);
	const int snare = rig->slotOfPad(1);
	REQUIRE(snare >= 0);
	const drum::Laws& laws = rig->synth.voiceDrumHitForTest(snare).laws();
	CHECK(laws.wireContour == 2);
	CHECK(laws.buzz == doctest::Approx(1.0));
	CHECK(laws.buzzThreshold == doctest::Approx(0.3));
	CHECK(laws.width == 0.6);
	// THE STEREO ROAD: a centred pad with Width has two ears that differ.
	bool differ = false;
	for (size_t i = 0; i < l.size(); ++i) {
		differ = differ || l[i] != r[i];
	}
	CHECK(differ);
}

TEST_CASE("drum kit: with no Width and a centred kit the two ears are one (the control for Width's stereo road)") {
	auto rig = std::make_unique<Rig>();
	rig->pad(1, DrumControl::Wires, 0.5); // Buzzy, exactly
	rig->synth.noteOn(0, 38, 1, 1.0);
	std::vector<float> l(256);
	std::vector<float> r(256);
	rig->synth.render(l.data(), r.data(), 256);
	CHECK(l == r);
}

namespace {

using feathers::kMaxSounds;
using feathers::SoundField;
using feathers::soundParam;
Character kitCharacter(ParamSet& params) {
	params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Drum));
	params.setBase(static_cast<clap_id>(ParamId::DrumMode), 1.0);
	return feathers::characterFromParams([&params](ParamId id) { return params.base(id); }, OscType::Drum,
	                                     feathers::AuditionTuning {}, feathers::AnchorPoint {}, feathers::SoundTravel {},
	                                     feathers::SoundOrientations {}, feathers::SoundResidue {}, feathers::SoundGamma {},
	                                     feathers::Preview {}, feathers::EarSide::Head);
}

} // namespace

TEST_CASE("kit layer: at its defaults it is neutral to the bit, every pad's place and fields as they were") {
	// THE LAW AT NEUTRAL, over many fields: the trim's own law at 0.5 is the field itself, bit for bit.
	for (int i = 0; i <= 1000; ++i) {
		const float f = static_cast<float>(i) / 1000.0f;
		CHECK(feathers::fieldWithOverride(f, feathers::kFieldNeutral) == f);
	}
	ParamSet params;
	params.setBase(static_cast<clap_id>(soundParam(2, SoundField::Zone)), 300.0);
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::FieldTone)), 0.8);
	const Character c = kitCharacter(params);
	REQUIRE(c.count == 8);
	for (size_t i = 0; i < c.count; ++i) {
		const SoundCoord& s = c.sounds[i];
		CAPTURE(i);
		CHECK(s.drumKitBias == feathers::DrumBias {});
		CHECK(s.zone == static_cast<uint16_t>(std::lround(params.base(soundParam(s.slot, SoundField::Zone)))));
		CHECK(s.phaseOffset
		      == static_cast<float>(params.base(soundParam(s.slot, SoundField::Offset))
		                            + params.base(soundParam(s.slot, SoundField::Gamma)) + params.base(ParamId::GlobalGamma)));
		CHECK(feathers::buildDrumFor(c).pads[s.slot] == soloFields(c, s)); // the restatement knows no kit layer
	}
}

TEST_CASE("kit layer: Kit Zone and Kit Gamma are each every pad moved by the same amount") {
	ParamSet moved;
	ParamSet kit;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const double z = moved.base(soundParam(slot, SoundField::Zone));
		moved.setBase(static_cast<clap_id>(soundParam(slot, SoundField::Zone)), std::clamp(z + 37.0, 0.0, 1023.0));
		moved.setBase(static_cast<clap_id>(soundParam(slot, SoundField::Gamma)), moved.base(soundParam(slot, SoundField::Gamma)) + 0.3);
	}
	kit.setBase(static_cast<clap_id>(ParamId::DrumKitZone), 37.0);
	kit.setBase(static_cast<clap_id>(ParamId::DrumKitGamma), 0.3);
	const Character a = kitCharacter(moved);
	const Character b = kitCharacter(kit);
	REQUIRE(a.count == b.count);
	const auto fa = feathers::buildDrumFor(a);
	const auto fb = feathers::buildDrumFor(b);
	for (size_t i = 0; i < a.count; ++i) {
		CAPTURE(i);
		CHECK(a.sounds[i].zone == b.sounds[i].zone);
		CHECK(std::abs(a.sounds[i].phaseOffset - b.sounds[i].phaseOffset) <= 1.0e-6f); // the same sum, added in another order
		const auto x = asArray(fa.pads[i]);
		const auto y = asArray(fb.pads[i]);
		for (size_t k = 0; k < x.size(); ++k) {
			CHECK(std::abs(x[k] - y[k]) < 1.0e-4);
		}
	}
	// AND IT DID MOVE THEM (the case is not about two unmoved kits).
	ParamSet plain;
	CHECK(kitCharacter(plain).sounds[1].zone != b.sounds[1].zone);
	// HELD INSIDE THE ZONES THAT EXIST: a pad at 1000 shifted by +100 stands at 1023.
	ParamSet edge;
	edge.setBase(static_cast<clap_id>(soundParam(0, SoundField::Zone)), 1000.0);
	edge.setBase(static_cast<clap_id>(ParamId::DrumKitZone), 100.0);
	CHECK(kitCharacter(edge).sounds[0].zone == 1023);
}

TEST_CASE("kit layer: a kit trim composes with each pad's scaler by the scaler's own law, applied again on top") {
	// The law written out: k ≤ 0.5 scales toward 0 by 2k; above, lifts toward 1 by 2(k − 0.5).
	const auto law = [](double f, double k) { return k <= 0.5 ? f * (2.0 * k) : f + (1.0 - f) * (2.0 * (k - 0.5)); };
	ParamSet params;
	params.setBase(static_cast<clap_id>(drumParam(3, DrumControl::FieldTone)), 0.7);
	params.setBase(static_cast<clap_id>(drumParam(3, DrumControl::FieldNoise)), 0.2);
	params.setBase(static_cast<clap_id>(ParamId::DrumKitTone), 0.3);
	params.setBase(static_cast<clap_id>(ParamId::DrumKitNoise), 0.9);
	const Character c = kitCharacter(params);
	const SoundCoord* s = nullptr;
	for (size_t i = 0; i < c.count; ++i) {
		if (c.sounds[i].slot == 3) {
			s = &c.sounds[i];
		}
	}
	REQUIRE(s != nullptr);
	// The raw fields at the pad's place: its restatement with every scaler neutral.
	SoundCoord bare = *s;
	bare.drumBias = {};
	bare.drumKitBias = {};
	const drum::Fields raw = soloFields(c, bare);
	const drum::Fields got = feathers::buildDrumFor(c).pads[3];
	CHECK(std::abs(got.tone - law(law(raw.tone, 0.7), 0.3)) < 1.0e-6);
	CHECK(std::abs(got.noise - law(law(raw.noise, 0.2), 0.9)) < 1.0e-6);
	CHECK(got.pitch == raw.pitch); // the untouched ones are the raw fields
	CHECK(std::abs(got.tone - law(raw.tone, 0.7)) > 1.0e-3); // and the kit trim did act
}

TEST_CASE("kit layer: every kit control is a modulation destination, and a modulated Kit Tone reaches a pad's hit") {
	for (const ParamId id : { ParamId::DrumKitGamma, ParamId::DrumKitZone, ParamId::DrumKitOffset, ParamId::DrumKitPitch,
	                          ParamId::DrumKitLength, ParamId::DrumKitSweep, ParamId::DrumKitTone, ParamId::DrumKitNoise,
	                          ParamId::DrumKitSnap, ParamId::DrumKitMetal }) {
		CAPTURE(static_cast<int>(id));
		CHECK(feathers::modDestinationIndexOf(id) >= 0);
	}
	// THE MODULATED ROAD (a host modulation offset, resolved as a route's would be): Kit Tone at 0.5 + 0.4 plays the same
	// hit as Kit Tone at 0.9.
	const auto toneOf = [](double base, double offset) {
		auto rig = std::make_unique<Rig>();
		rig->set(ParamId::DrumKitTone, base);
		rig->params.setModOffset(static_cast<clap_id>(ParamId::DrumKitTone), offset);
		rig->synth.noteOn(0, 36, 1, 0.8);
		(void)rig->play(64);
		const int kick = rig->slotOfPad(0);
		REQUIRE(kick >= 0);
		return rig->synth.voiceDrumHitForTest(kick).laws().tone / static_cast<float>(1.0); // tone with the hit's own variation
	};
	const float modulated = toneOf(0.5, 0.4);
	const float based = toneOf(0.9, 0.0);
	const float neutral = toneOf(0.5, 0.0);
	CHECK(modulated == doctest::Approx(based).epsilon(1.0e-6));
	CHECK(std::abs(modulated - neutral) > 1.0e-3);
}

TEST_CASE("drum kit (audition): a muted pad takes no hit and does not count one; a soloed card leaves only its pad struck") {
	// MUTE: B (the Snare, note 38) muted; A (the Kick, 36) not.
	{
		auto rig = std::make_unique<Rig>();
		rig->pad(1, DrumControl::Mute, 1.0);
		CHECK_FALSE(rig->synth.padAudible(1));
		CHECK(rig->synth.padAudible(0));
		rig->synth.noteOn(0, 38, 1, 0.9);
		rig->synth.noteOn(0, 36, 2, 0.7);
		const bool silent = rig->play(512);
		CHECK_FALSE(silent);
		CHECK(rig->slotOfPad(1) < 0);  // no snare voice
		CHECK(rig->slotOfPad(0) >= 0); // the kick plays
		const feathers::PadHits hits = rig->synth.padHits();
		CHECK(hits.count[1] == 0u);    // a muted pad never pulses
		CHECK(hits.count[0] == 1u);
		CHECK(hits.velocity[0] == doctest::Approx(0.7f));
		for (size_t pad = 2; pad < feathers::kMaxSounds; ++pad) {
			CHECK(hits.count[pad] == 0u); // that pad only
		}
	}
	// SOLO: card A's S (the preview) latched: B's note strikes nothing, A's plays.
	{
		auto rig = std::make_unique<Rig>();
		feathers::Preview solo;
		solo.slot = 0;
		rig->synth.setPreview(solo);
		CHECK_FALSE(rig->synth.padAudible(1));
		rig->synth.noteOn(0, 38, 1, 0.9);
		rig->synth.noteOn(0, 36, 2, 0.9);
		(void)rig->play(256);
		CHECK(rig->slotOfPad(1) < 0);
		CHECK(rig->slotOfPad(0) >= 0);
		CHECK(rig->synth.padHits().count[1] == 0u);
	}
	// AND BY DEFAULT EVERY PAD SOUNDS, whatever its sound's switch (the kit's eight are always present).
	auto rig = std::make_unique<Rig>();
	for (size_t pad = 0; pad < feathers::kMaxSounds; ++pad) {
		CAPTURE(pad);
		CHECK(rig->synth.padAudible(pad));
	}
}

TEST_CASE("rotate on drums: a pad's seven fields (and so its hit) are bit-identical under a turn at home and at a drift; the Walk is what a turn reaches") {
	for (const bool kit : { true, false }) {
		CAPTURE(kit);
		ParamSet params;
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Drum));
		params.setBase(static_cast<clap_id>(ParamId::DrumMode), kit ? 1.0 : 0.0);
		params.setBase(static_cast<clap_id>(soundParam(0, SoundField::Enable)), 1.0);
		const auto build = [&params](const feathers::SoundTravel& drift, const feathers::SoundOrientations& planes,
		                             const feathers::SoundResidue& residue) {
			return feathers::characterFromParams([&params](ParamId id) { return params.base(id); }, OscType::Drum,
			                                     feathers::AuditionTuning {}, feathers::AnchorPoint {}, drift, planes, residue,
			                                     feathers::SoundGamma {}, feathers::Preview {}, feathers::EarSide::Head);
		};
		feathers::SliceRotation turn;
		turn.rotate = 0.7f;
		const auto padA = [&](const feathers::SoundTravel& drift) {
			const Character flat = build(drift, {}, {});
			const SoundCoord& s = flat.sounds[0];
			REQUIRE(s.slot == 0);
			const feathers::RotatedSound turned = feathers::rotateSoundBy(feathers::SoundOrientations {}, feathers::SoundResidue {}, 0,
			                                                              s.zone, s.phaseOffset, feathers::SliceOrientationPin {},
			                                                              turn, drift.offset[0], 0.0);
			const Character folded = build(drift, turned.planes, turned.residue);
			const Character unfolded = build(drift, turned.planes, {});
			return std::array<drum::Fields, 3> { feathers::buildDrumFor(flat).pads[0], feathers::buildDrumFor(folded).pads[0],
				                                 feathers::buildDrumFor(unfolded).pads[0] };
		};
		// AT HOME: nothing to turn.
		{
			const auto f = padA({});
			CHECK(f[0] == f[1]);
		}
		// AT A DRIFT: the fold holds the pad's sound to the bit, and dropping the fold moves it (the contrast is live).
		{
			feathers::SoundTravel drift;
			drift.offset[0] = { 0.37, 0.3 };
			const auto f = padA(drift);
			CHECK(f[0] == f[1]);
			CHECK_FALSE(f[0] == f[2]);
		}
		// THE VOICE IS ITS FIELDS: equal fields give equal laws (lawsFor is a function of them), so the hit is identical.
		{
			const auto f = padA({});
			drum::Controls c;
			CHECK(drum::lawsFor(drum::Kind::Snare, f[0], c, 180.0, 0.8) == drum::lawsFor(drum::Kind::Snare, f[1], c, 180.0, 0.8));
		}
	}
	// IN A KIT THE WALK IS TRAVEL THE FOLD DOES NOT HOLD (as the puck's Weather is on the other engines): a turn with the
	// Walk off zero moves the pad, which is what was heard (the per-pad pivot).
	ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Drum));
	params.setBase(static_cast<clap_id>(ParamId::DrumMode), 1.0);
	params.setBase(static_cast<clap_id>(ParamId::DrumWalkX), 0.25);
	const auto read = [&params](ParamId id) { return params.base(id); };
	const Character flat = feathers::characterFromParams(read, OscType::Drum, feathers::AuditionTuning {}, feathers::AnchorPoint {},
	                                                     feathers::SoundTravel {}, feathers::SoundOrientations {},
	                                                     feathers::SoundResidue {}, feathers::SoundGamma {}, feathers::Preview {},
	                                                     feathers::EarSide::Head);
	feathers::SliceRotation turn;
	turn.rotate = 0.7f;
	const feathers::RotatedSound turned = feathers::rotateSoundBy(feathers::SoundOrientations {}, feathers::SoundResidue {}, 0,
	                                                              flat.sounds[0].zone, flat.sounds[0].phaseOffset,
	                                                              feathers::SliceOrientationPin {}, turn, feathers::AnchorPoint {}, 0.0);
	const Character walked = feathers::characterFromParams(read, OscType::Drum, feathers::AuditionTuning {}, feathers::AnchorPoint {},
	                                                       feathers::SoundTravel {}, turned.planes, turned.residue,
	                                                       feathers::SoundGamma {}, feathers::Preview {}, feathers::EarSide::Head);
	CHECK_FALSE(feathers::buildDrumFor(flat).pads[0] == feathers::buildDrumFor(walked).pads[0]);
}

TEST_CASE("drum kit (decision 8, the 808 bend): a pad follows the pitch bend by its Bend share, held at the strike and moved "
          "while it rings; the kit's transpose moves every pad in full; Bend 0 ignores the wheel") {
	const double range = static_cast<double>(feathers::kBendRangeSemitones[1]); // 2 semitones
	const double rest = drum::kindBaseHz(drum::Kind::Membrane);
	const auto hzOf = [&](double semitones) { return rest * std::pow(2.0, semitones / 12.0); };
	for (const double share : { 1.0, 0.5, 0.0 }) {
		CAPTURE(share);
		auto rig = std::make_unique<Rig>();
		rig->set(ParamId::BendRange, 1.0);
		rig->pad(5, DrumControl::Tune, -5.0);
		rig->pad(5, DrumControl::FieldPitch, 0.0); // −3 semitones from the terrain's Pitch
		rig->pad(5, DrumControl::FieldSweep, 0.0);
		rig->pad(5, DrumControl::Bend, share);
		// HELD AT THE STRIKE: the wheel half up before the hit.
		rig->synth.pitchBend(0, 0.5);
		rig->synth.noteOn(0, 45, 1, 1.0);
		(void)rig->play(256);
		const int tom = rig->slotOfPad(5);
		REQUIRE(tom >= 0);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).laws().hz - hzOf(-8.0 + share * 0.5 * range)) < 1.0e-6);
		// MOVED WHILE IT RINGS: the wheel right down.
		rig->synth.pitchBend(0, -1.0);
		(void)rig->play(256);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).laws().hz - hzOf(-8.0 - share * range)) < 1.0e-6);
		// THE KIT'S TRANSPOSE, in full whatever the share: +1 octave and +2 semitones, the wheel back at rest.
		rig->synth.pitchBend(0, 0.0);
		rig->set(ParamId::TransposeOct, 1.0);
		rig->set(ParamId::TransposeSemi, 2.0);
		(void)rig->play(256);
		CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).laws().hz - hzOf(-8.0 + 14.0)) < 1.0e-6);
	}
}

TEST_CASE("drum pitched (decision 8, the 808 glide): Mono with Glide strikes the next note's hit at the last note's pitch and "
          "slides it to its own; a kit ignores Glide") {
	auto rig = std::make_unique<Rig>(false);
	rig->set(ParamId::DrumPitchedKind, 0.0); // Kick
	rig->set(ParamId::VoiceMode, static_cast<double>(feathers::VoiceMode::Mono));
	rig->set(ParamId::GlideTime, 0.3); // 2 s · 0.3³ = 54 ms
	rig->pad(0, DrumControl::FieldPitch, 0.0); // −0.5 semitones in Pitched mode
	rig->pad(0, DrumControl::Decay, std::log2(4.0));
	const double fine = std::pow(2.0, -0.5 / 12.0);
	rig->synth.noteOn(0, 36, 1, 1.0);
	(void)rig->play(4096);
	rig->synth.noteOn(0, 43, 2, 1.0);
	(void)rig->play(64);
	const auto slotOf43 = [&] {
		for (const int q : rig->playing()) {
			if (rig->synth.voiceKeyForTest(q) == 43) {
				return q;
			}
		}
		return -1;
	};
	const int s = slotOf43();
	REQUIRE(s >= 0);
	CHECK(rig->synth.voiceDrumStrikesForTest(s) == 1); // struck: a new hit, not the old one moved
	// STRUCK AT THE LAST NOTE'S PITCH: the hit's pitch is the voice's, and the voice starts from 36.
	CHECK(rig->synth.voicePitchForTest(s) < 36.5);
	CHECK(rig->synth.voiceDrumHitForTest(s).laws().hz < noteHz(37) * fine);
	(void)rig->play(static_cast<uint32_t>(kRate * 0.03));
	const double mid = rig->synth.voiceDrumHitForTest(s).laws().hz;
	CHECK(mid > noteHz(37) * fine);
	CHECK(mid < noteHz(42) * fine);
	// AND LANDS ON ITS OWN.
	(void)rig->play(static_cast<uint32_t>(kRate * 1.0));
	REQUIRE(slotOf43() == s);
	CHECK(std::abs(rig->synth.voiceDrumHitForTest(s).laws().hz - noteHz(43) * fine) < 1.0e-3);
	// A KIT GLIDES NOTHING: the same Mono and Glide, a pad's pitch at once its own.
	auto kit = std::make_unique<Rig>();
	kit->set(ParamId::VoiceMode, static_cast<double>(feathers::VoiceMode::Mono));
	kit->set(ParamId::GlideTime, 0.3);
	kit->pad(5, DrumControl::Tune, 0.0);
	kit->pad(5, DrumControl::FieldPitch, 0.0);
	kit->pad(5, DrumControl::FieldSweep, 0.0);
	kit->synth.noteOn(0, 36, 1, 1.0);
	(void)kit->play(256);
	kit->synth.noteOn(0, 45, 2, 1.0);
	(void)kit->play(256);
	const int tom = kit->slotOfPad(5);
	REQUIRE(tom >= 0);
	CHECK(std::abs(kit->synth.voiceDrumHitForTest(tom).laws().hz
	               - drum::kindBaseHz(drum::Kind::Membrane) * std::pow(2.0, -3.0 / 12.0)) < 1.0e-6);
}

// ---- THE LAYER SLOT (drum proposal §12; stage (c), 2026-11-07) -----------------------------------------------------------

TEST_CASE("drum layer slot: a layer's fields are its pad's place moved by its offset vector, through the pad's scalers; a pad "
          "without one reaches the character unchanged") {
	ParamSet params;
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::LayerKind)), 3.0); // Snare
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::LayerOffsetX)), 0.3);
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::LayerOffsetY)), -0.2);
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::FieldTone)), 0.8);
	params.setBase(static_cast<clap_id>(ParamId::DrumWalkX), 0.1);
	const Character c = kitCharacter(params);
	REQUIRE(c.count == 8);
	const feathers::DrumCharacter built = feathers::buildDrumFor(c);
	for (size_t i = 0; i < c.count; ++i) {
		const SoundCoord& s = c.sounds[i];
		CAPTURE(s.slot);
		if (s.slot != 2) {
			CHECK_FALSE(s.drumLayer);
			CHECK(s.drumLayerX == 0.0);
			CHECK(built.layers[s.slot] == drum::Fields {});
			continue;
		}
		REQUIRE(s.drumLayer);
		CHECK(s.drumLayerX == 0.3);
		CHECK(s.drumLayerY == -0.2);
		// THE RESTATEMENT: the pad's own coordinate with its travel moved by the vector (the Walk's 0.1 included).
		SoundCoord at = s;
		at.travelX = 0.1 + 0.3;
		at.travelY = -0.2;
		at.drumLayer = false;
		CHECK(built.layers[2] == soloFields(c, at));
		CHECK_FALSE(built.layers[2] == built.pads[2]); // the vector moved it, so the claim is not the pad's own fields
	}
	// OFF, the offset is not read at all.
	params.setBase(static_cast<clap_id>(drumParam(2, DrumControl::LayerKind)), 0.0);
	const Character off = kitCharacter(params);
	for (size_t i = 0; i < off.count; ++i) {
		CHECK_FALSE(off.sounds[i].drumLayer);
	}
}

namespace {
/// Render `frames` of a fresh kit rig playing the low tom (pad 5), with `setup` applied first, into one ear.
std::vector<float> renderTom(const std::function<void(Rig&)>& setup, uint32_t frames) {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::Tune, -5.0);
	rig->pad(5, DrumControl::FieldPitch, 0.0);
	setup(*rig);
	rig->synth.noteOn(0, 45, 1, 1.0);
	std::vector<float> out(frames);
	std::vector<float> r(frames);
	rig->synth.render(out.data(), r.data(), static_cast<int>(frames));
	return out;
}
} // namespace

TEST_CASE("drum layer slot: a layered pad strikes a second hit of the layer's kind its Delay after the pad's, to the sample, "
          "at the pad's pitch on its kind's resting pitch; Off and a Level at its bottom add nothing, to the bit") {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::Tune, -5.0);
	rig->pad(5, DrumControl::FieldPitch, 0.0); // −3 semitones, the pad's scaler and so its layer's
	rig->pad(5, DrumControl::LayerKind, 1.0); // Kick
	rig->pad(5, DrumControl::LayerDelay, 5.0); // 240 samples at 48 kHz
	rig->synth.noteOn(0, 45, 1, 1.0);
	(void)rig->play(224, 16);
	const int tom = rig->slotOfPad(5);
	REQUIRE(tom >= 0);
	CHECK(rig->synth.voiceDrumLayerStrikesForTest(tom) == 0); // not yet
	(void)rig->play(32, 16);
	CHECK(rig->synth.voiceDrumLayerStrikesForTest(tom) == 1);
	CHECK(rig->synth.voiceDrumLayerForTest(tom).kind() == drum::Kind::Kick);
	CHECK(std::abs(rig->synth.voiceDrumLayerForTest(tom).laws().hz
	               - drum::kindBaseHz(drum::Kind::Kick) * std::pow(2.0, (-5.0 - 3.0) / 12.0))
	      < 1.0e-9);
	// TO THE SAMPLE: the layered render is the plain one, bit for bit, before sample 240, and not after.
	const auto plain = renderTom([](Rig&) {}, 1024);
	const auto layered = renderTom(
	    [](Rig& g) {
		    g.pad(5, DrumControl::LayerKind, 5.0); // Clap: its noise speaks at once
		    g.pad(5, DrumControl::LayerDelay, 5.0);
	    },
	    1024);
	size_t first = plain.size();
	for (size_t i = 0; i < plain.size(); ++i) {
		if (plain[i] != layered[i]) {
			first = i;
			break;
		}
	}
	CHECK(first >= 240);
	CHECK(first < 260);
	// OFF, AND A LEVEL AT ITS BOTTOM: nothing added.
	CHECK(renderTom([](Rig& g) { g.pad(5, DrumControl::LayerDelay, 5.0); }, 1024) == plain);
	CHECK(renderTom(
	          [](Rig& g) {
		          g.pad(5, DrumControl::LayerKind, 5.0);
		          g.pad(5, DrumControl::LayerLevel, -60.0);
	          },
	          1024)
	      == plain);
}

TEST_CASE("drum layer slot: its high-pass's coefficient is the one-pole law, bypassed at its bottom; a Gate pad damps its "
          "layer too; the voice lives until the layer is silent") {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::LayerKind, 2.0); // Membrane
	rig->pad(5, DrumControl::LayerHighPass, std::log2(2000.0));
	rig->pad(5, DrumControl::LayerDelay, 10.0);
	rig->pad(5, DrumControl::Trigger, 1.0); // Gate
	rig->pad(5, DrumControl::Decay, std::log2(2.0));
	rig->pad(5, DrumControl::Release, std::log2(0.02));
	rig->synth.noteOn(0, 45, 1, 1.0);
	(void)rig->play(1024, 16);
	const int tom = rig->slotOfPad(5);
	REQUIRE(tom >= 0);
	CHECK(rig->synth.voiceDrumLayerHighPassForTest(tom)
	      == doctest::Approx(1.0 / (1.0 + 2.0 * std::numbers::pi * 2000.0 / kRate)).epsilon(1.0e-6));
	rig->pad(5, DrumControl::LayerHighPass, std::log2(20.0));
	(void)rig->play(32, 16);
	CHECK(rig->synth.voiceDrumLayerHighPassForTest(tom) == 1.0f);
	// THE GATE: both hits damped to Release.
	rig->synth.noteOff(0, 45, 1, 0.0);
	(void)rig->play(64, 16);
	const double want = std::pow(10.0, -3.0 / (0.02 * kRate));
	CHECK(std::abs(rig->synth.voiceDrumHitForTest(tom).modeDecayPerSample(0) - want) < 1.0e-12);
	CHECK(std::abs(rig->synth.voiceDrumLayerForTest(tom).modeDecayPerSample(0) - want) < 1.0e-12);
}

TEST_CASE("drum layer slot: a voice whose pad has fallen silent lives on while its delayed layer still rings, and ends when "
          "both are") {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::LayerKind, 2.0); // Membrane
	rig->pad(5, DrumControl::LayerDelay, 10.0);
	rig->pad(5, DrumControl::Decay, std::log2(0.05));
	rig->synth.noteOn(0, 45, 1, 1.0);
	(void)rig->play(16, 16);
	const int tom = rig->slotOfPad(5);
	REQUIRE(tom >= 0);
	bool sawPadSilentLayerRinging = false;
	for (int b = 0; b < 48000 / 16 && rig->synth.voiceStageForTest(tom) != Voice::Stage::Idle; ++b) {
		if (rig->synth.voiceDrumHitForTest(tom).idle() && !rig->synth.voiceDrumLayerForTest(tom).idle()) {
			sawPadSilentLayerRinging = true;
			CHECK(rig->synth.voiceStageForTest(tom) != Voice::Stage::Idle);
		}
		(void)rig->play(16, 16);
	}
	CHECK(sawPadSilentLayerRinging);
	CHECK(rig->synth.voiceStageForTest(tom) == Voice::Stage::Idle);
}

TEST_CASE("drum layer slot: a pad whose hit is over before its layer's Delay still strikes the layer — the silent wait is not "
          "the end of the voice") {
	auto rig = std::make_unique<Rig>();
	rig->pad(5, DrumControl::Decay, -6.643856189774724); // 10 ms, the shortest
	rig->pad(5, DrumControl::FieldLength, 0.0);          // and a quarter of it
	rig->pad(5, DrumControl::FieldSweep, 0.0);
	rig->pad(5, DrumControl::LayerKind, 2.0);            // Membrane
	rig->pad(5, DrumControl::LayerDelay, 10.0);          // 480 samples
	rig->synth.noteOn(0, 45, 1, 1.0);
	(void)rig->play(16, 16);
	const int tom = rig->slotOfPad(5);
	REQUIRE(tom >= 0);
	(void)rig->play(448, 16); // sample 464: the layer still waits
	REQUIRE(rig->synth.voiceDrumLayerStrikesForTest(tom) == 0);
	REQUIRE(rig->synth.voiceDrumHitForTest(tom).idle()); // the precondition: the pad's own hit is already over
	(void)rig->play(32, 16);
	CHECK(rig->synth.voiceDrumLayerStrikesForTest(tom) == 1);
	CHECK_FALSE(rig->synth.voiceDrumLayerForTest(tom).idle());
	// NOT ENDED: still struck, never released to its edge (an ended voice fades the layer out over the release edge).
	const Voice::Stage stage = rig->synth.voiceStageForTest(tom);
	CHECK((stage == Voice::Stage::Attack || stage == Voice::Stage::Sustain));
}

// ---- THE PER-PAD FX LANES (drum proposal §17, stage (d)) -----------------------------------------------------------------------

namespace {
/// Render `frames` of a kit rig in one-chunk blocks, the left ear, after `setup`; the host's catalogue seated so a lane builds.
std::vector<float> renderKit(const std::function<void(Rig&)>& setup, const std::vector<int16_t>& keys, uint32_t frames) {
	auto rig = std::make_unique<Rig>();
	rig->synth.setCatalogue(feathers::hostCatalogue());
	setup(*rig);
	(void)rig->play(256, 64); // the lanes' drains land
	for (const int16_t key : keys) {
		rig->synth.noteOn(0, key, static_cast<int32_t>(key), 1.0);
	}
	std::vector<float> out(frames);
	std::vector<float> r(frames);
	for (uint32_t at = 0; at < frames; at += 64) {
		rig->synth.render(out.data() + at, r.data() + at, 64);
	}
	return out;
}
/// Put a Bus Utility at Bus Gain `position` (its fader: (2p)², so 0.25 is a quarter) in pad `pad`'s lane.
void quarterGainLane(Rig& rig, size_t pad, float position) {
	feathers::rack::InnerChain* lane = rig.synth.ensurePadLane(pad);
	REQUIRE(lane != nullptr);
	REQUIRE(lane->bridge() != nullptr);
	const auto id = lane->bridge()->addDevice("busu");
	REQUIRE(id != 0u);
	lane->bridge()->setField(id, 1, position); // Bus Gain
}
} // namespace

TEST_CASE("drum pad lanes (§17 P1): a kit whose pads all have EMPTY lanes renders what a kit without lanes renders, to the bit, "
          "and no lane is live") {
	const std::vector<int16_t> keys { 36, 38, 42, 45 };
	const auto plain = renderKit([](Rig&) {}, keys, 4096);
	auto rig = std::make_unique<Rig>();
	const auto withLanes = renderKit(
	    [](Rig& g) {
		    for (size_t pad = 0; pad < feathers::kMaxSounds; ++pad) {
			    REQUIRE(g.synth.ensurePadLane(pad) != nullptr);
		    }
	    },
	    keys, 4096);
	CHECK(withLanes == plain);
	// AND IT IS NOT LIVE (the road not taken, rather than a road that happens to sum the same).
	auto probe = std::make_unique<Rig>();
	probe->synth.setCatalogue(feathers::hostCatalogue());
	(void)probe->synth.ensurePadLane(5);
	(void)probe->play(64, 64);
	CHECK_FALSE(probe->synth.padLaneLiveForTest(5));
}

TEST_CASE("drum pad lanes (§17 P2): a pad's lane holding a Bus Utility at a quarter gain scales THAT pad's voices by a quarter, "
          "by value; the other pads and Pitched mode are untouched") {
	// THE TOM ALONE AND THE KICK ALONE, plain, then both with the tom's lane at a quarter.
	const auto tom = renderKit([](Rig&) {}, { 45 }, 8192);
	const auto kick = renderKit([](Rig&) {}, { 36 }, 8192);
	const auto both = renderKit([](Rig& g) { quarterGainLane(g, 5, 0.25f); }, { 36, 45 }, 8192);
	double moved = 0.0;
	for (size_t i = 2048; i < both.size(); ++i) { // past the fader's own smoothing from where it was built
		CAPTURE(i);
		const double want = static_cast<double>(kick[i]) + 0.25 * static_cast<double>(tom[i]);
		CHECK(std::abs(static_cast<double>(both[i]) - want) < 1.0e-5);
		moved = std::max(moved, std::abs(static_cast<double>(tom[i])));
		if (std::abs(static_cast<double>(both[i]) - want) >= 1.0e-5) {
			break;
		}
	}
	CHECK(moved > 1.0e-3); // the tom sounds there, so the quarter is a claim
	auto rig = std::make_unique<Rig>();
	rig->synth.setCatalogue(feathers::hostCatalogue());
	quarterGainLane(*rig, 5, 0.25f);
	(void)rig->play(64, 64);
	CHECK(rig->synth.padLaneLiveForTest(5));
	CHECK_FALSE(rig->synth.padLaneLiveForTest(0));
	// PITCHED MODE: a pad lane is not the engine's; nothing goes through it.
	rig->set(ParamId::DrumMode, 0.0);
	(void)rig->play(64, 64);
	CHECK_FALSE(rig->synth.padLaneLiveForTest(5));
}

TEST_CASE("drum pad lanes (§17 P2): a pad lane's tail keeps the synth holding a tail after its voices end (SLEEP waits for it)") {
	auto rig = std::make_unique<Rig>();
	rig->synth.setCatalogue(feathers::hostCatalogue());
	feathers::rack::InnerChain* lane = rig->synth.ensurePadLane(5);
	REQUIRE(lane != nullptr);
	REQUIRE(lane->bridge()->addDevice("DELAY") != 0u);
	rig->pad(5, DrumControl::Decay, std::log2(0.05));
	(void)rig->play(256, 64);
	rig->synth.noteOn(0, 45, 1, 1.0);
	// PLAY UNTIL THE TOM'S VOICE IS GONE, then the delay still holds its repeats.
	for (int b = 0; b < 2000 && !rig->playing().empty(); ++b) {
		(void)rig->play(64, 64);
	}
	REQUIRE(rig->playing().empty());
	CHECK(rig->synth.holdsTail());
}

// ---- `latN` — A PURE DELAY THAT SAYS HOW LONG IT IS (test_latency.cpp's, copied: its seat is per binary) --------------------

namespace {
namespace device = feathers::device;

constexpr size_t kRing = 1u << 14;

struct Latent {
	std::vector<float> l;
	std::vector<float> r;
	size_t write = 0;
	uint32_t delay = 0;
};

Latent& latentAt(void* s) { return *static_cast<Latent*>(s); }

uint32_t declared(const float* fields, size_t present) {
	if (fields == nullptr || present == 0 || !(fields[0] > 0.0f)) {
		return 0;
	}
	return static_cast<uint32_t>(std::lround(std::min(fields[0], static_cast<float>(kRing - 1))));
}

void delayOne(Latent& d, float* x, std::vector<float>& ring, int frames, size_t start) {
	size_t w = start;
	for (int i = 0; i < frames; ++i) {
		ring[w] = x[i];
		x[i] = ring[(w - d.delay) & (kRing - 1)];
		w = (w + 1) & (kRing - 1);
	}
}

constexpr device::AudioOps kLatentOps {
	.stateBytes = sizeof(Latent),
	.stateAlign = alignof(Latent),
	.construct = [](void* s) { new (s) Latent {}; },
	.destruct = [](void* s) { static_cast<Latent*>(s)->~Latent(); },
	.prepare = [](void* s, double, int) {
		latentAt(s).l.assign(kRing, 0.0f);
		latentAt(s).r.assign(kRing, 0.0f);
	},
	.reset = [](void* s) {
		std::fill(latentAt(s).l.begin(), latentAt(s).l.end(), 0.0f);
		std::fill(latentAt(s).r.begin(), latentAt(s).r.end(), 0.0f);
	},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { latentAt(s).delay = declared(f, n); },
	.processStereo = [](void* s, float* l, float* r, int frames) {
		Latent& d = latentAt(s);
		if (d.l.size() < kRing) {
			return;
		}
		delayOne(d, l, d.l, frames, d.write);
		delayOne(d, r, d.r, frames, d.write);
		d.write = (d.write + static_cast<size_t>(frames)) & (kRing - 1);
	},
	.processMono = [](void* s, float* mono, int frames) {
		Latent& d = latentAt(s);
		if (d.l.size() < kRing) {
			return;
		}
		delayOne(d, mono, d.l, frames, d.write);
		d.write = (d.write + static_cast<size_t>(frames)) & (kRing - 1);
	},
	.latencySamples = [](const void*, const float* fields, size_t present, double) { return declared(fields, present); },
};

constexpr std::array<device::ParamDesc, 2> kLatentParams { {
	{ "latency", "Latency", 0.0, 8192.0, 0.0, true, device::Unit::Number, {} },
	{ "poly", "Poly", 0.0, 1.0, 0.0, true, device::Unit::Number, {} },
} };

constexpr size_t kLatentLatencyControl = 0;

const device::Descriptor kLatent {
	.traits = { .tag = "latN", .name = "Latent", .hasCard = false, .hasExpanded = false, .perVoiceControl = 1 },
	.params = kLatentParams,
	.outputs = {},
	.audio = &kLatentOps,
};
struct LatentSeat {
	LatentSeat() { feathers::seatedDeviceForTest() = &kLatent; }
	~LatentSeat() { feathers::seatedDeviceForTest() = nullptr; }
	LatentSeat(const LatentSeat&) = delete;
	LatentSeat& operator=(const LatentSeat&) = delete;
};
/// A `latN` at `samples` in pad `pad`'s lane.
void latentLane(Rig& rig, size_t pad, float samples) {
	feathers::rack::InnerChain* lane = rig.synth.ensurePadLane(pad);
	REQUIRE(lane != nullptr);
	const auto id = lane->bridge()->addDevice("latN");
	REQUIRE(id != 0u);
	lane->bridge()->setField(id, kLatentLatencyControl, samples);
}
} // namespace

TEST_CASE("drum pad lanes (§17 P4): a latent pad lane delays the WHOLE kit to meet it, so no pad is late against another, and "
          "the synth reports that latency; two latent lanes meet at the later") {
	const LatentSeat seat;
	const auto kick = renderKit([](Rig&) {}, { 36 }, 4096);
	const auto tom = renderKit([](Rig&) {}, { 45 }, 4096);
	const auto check = [&](const std::vector<float>& both, uint32_t latency) {
		double moved = 0.0;
		bool held = true;
		for (size_t i = 0; i < both.size() && held; ++i) {
			const double want = i < latency ? 0.0 : static_cast<double>(kick[i - latency]) + static_cast<double>(tom[i - latency]);
			held = std::abs(static_cast<double>(both[i]) - want) < 1.0e-6;
			CAPTURE(i);
			CHECK(held);
			moved = std::max(moved, std::abs(want));
		}
		CHECK(moved > 1.0e-3);
	};
	// THE TOM'S LANE 100 LATE: the kick (no lane) waits 100 for it.
	check(renderKit([](Rig& g) { latentLane(g, 5, 100.0f); }, { 36, 45 }, 4096), 100);
	// AND THE KICK'S LANE 40 LATE AS WELL: it waits the other 60.
	check(renderKit(
	          [](Rig& g) {
		          latentLane(g, 5, 100.0f);
		          latentLane(g, 0, 40.0f);
	          },
	          { 36, 45 }, 4096),
	      100);
	auto rig = std::make_unique<Rig>();
	rig->synth.setCatalogue(feathers::hostCatalogue());
	latentLane(*rig, 5, 100.0f);
	(void)rig->play(64, 64);
	CHECK(rig->synth.latencySamples() == 100u);
}

TEST_CASE("drum kit: the kit is rebuilt once per render call and once per control tick, not per chunk, and the values follow") {
	constexpr uint32_t kTick = 64; // the control tick, in samples: where a routed source moves
	SUBCASE("a static patch: one build per tick, and the pad's values stand") {
		Rig rig;
		rig.pad(0, DrumControl::Level, -6.0);
		const uint64_t before = rig.synth.drumKitBuildsForTest();
		// Eight calls of 128 frames, each on the tick grid: two ticks a call, so two builds a call, where a build per
		// 32-frame chunk was four.
		for (int call = 0; call < 8; ++call) {
			(void)rig.play(128, 128);
		}
		CHECK(rig.synth.drumKitBuildsForTest() - before == 8 * (128 / kTick));
		CHECK(rig.synth.drumKitForTest().pads[0].gain == doctest::Approx(static_cast<float>(std::pow(10.0, -6.0 / 20.0))).epsilon(1.0e-6));
	}
	SUBCASE("a host offset set between calls is in the next call's kit") {
		Rig rig;
		rig.pad(0, DrumControl::Level, -6.0);
		(void)rig.play(96, 96); // ends inside a tick, so the next call is not on a tick's edge
		const float first = rig.synth.drumKitForTest().pads[0].gain;
		rig.params.setModOffset(static_cast<clap_id>(drumParam(0, DrumControl::Level)), -12.0);
		(void)rig.play(32, 32); // a call of one chunk: only the call's own build can carry the offset
		const float second = rig.synth.drumKitForTest().pads[0].gain;
		CHECK(second == doctest::Approx(static_cast<float>(std::pow(10.0, -18.0 / 20.0))).epsilon(1.0e-6));
		CHECK(second < first * 0.9f); // and the offset did move it
	}
	SUBCASE("a routed source that moves at the tick: one long call ends on the kit that tick-sized calls end on") {
		namespace dsr = feathers::mod::dahdsr;
		const auto run = [&](int call) {
			auto rig = std::make_unique<Rig>();
			feathers::ModRack rack = feathers::defaultModRack();
			rack.count = 1;
			const feathers::device::Descriptor* desc = feathers::deviceForTag(dsr::kTraits.tag);
			REQUIRE(desc != nullptr);
			rack.device[0] = feathers::defaultRowFor(*desc, { 1 });
			rack.device[0].field[static_cast<size_t>(dsr::Param::Attack)] = std::log2(0.05f);
			rack.device[0].field[static_cast<size_t>(dsr::Param::Sustain)] = 1.0f;
			rig->synth.provisionDevices(feathers::FxChain {}, rack);
			rig->synth.setModRack(rack);
			(void)rig->play(64, 64);
			feathers::test::Routes routes;
			routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
			           feathers::test::param(drumParam(0, DrumControl::Level)), 0.5f);
			routes.publish(rig->params);
			rig->pad(0, DrumControl::Level, -30.0);
			rig->synth.noteOn(0, 36, 1, 1.0);
			(void)rig->play(64, 64);
			const float early = rig->synth.drumKitForTest().pads[0].gain;
			const uint64_t builds = rig->synth.drumKitBuildsForTest();
			for (int done = 0; done < 1024; done += call) {
				(void)rig->play(static_cast<uint32_t>(call), call);
			}
			return std::tuple<float, float, uint64_t> { early, rig->synth.drumKitForTest().pads[0].gain,
				                                       rig->synth.drumKitBuildsForTest() - builds };
		};
		const auto [earlyA, ticks, ticksBuilds] = run(64);
		const auto [earlyB, one, oneBuilds] = run(1024);
		CHECK(ticksBuilds == 1024 / kTick);
		CHECK(oneBuilds == 1024 / kTick); // a build per tick inside the one call
		CHECK(one == ticks);
		CHECK(ticks > earlyA * 1.05f); // the source moved the pad's level across the run: the equality above is not vacuous
		(void)earlyB;
	}
}
