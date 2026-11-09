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

// THE LIKES' PHASE WARP (P-MÖBIUS, backlog ON, shipped provisionally 2026-10-28) on the voice's road: each builder reads
// its kind's sites, the newest like's pin to the float, the audio thread allocating nothing with a warp in every
// character, a save/load into a fresh instance playing the same, and no likes installing no hook at all.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "character/phi_triangle.hpp"
#include "dsp/character_bank.h"
#include "dsp/phase_warp.h"
#include "dsp/slice_build.h"
#include "dsp/vox_bake.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "plugin/terrain_likes.h"
#include "plugin/ui/terrain.h"

#include <array>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <vector>

using namespace feathers;

namespace {

struct Allocs {
	bool armed = false;
	size_t allocations = 0;
};
Allocs allocs;

void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"

namespace {

constexpr double kRate = 48000.0;

struct Rig {
	ParamSet params;
	Synth synth { params };
	std::array<LikeWarp, kMaxSounds> warps {};
	std::array<uint8_t, kMaxSounds> engines {};
	Rig() {
		synth.activate(kRate, 4096);
		set(ParamId::OscType, 2.0); // V5
		set(ParamId::AZone, 17.0);
		set(ParamId::AOffset, 0.0);
		set(ParamId::Path, 0.0);
		set(ParamId::VoiceLevel, 1.0);
		engines.fill(static_cast<uint8_t>(OscType::V5));
	}
	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }
	/// `blocks` of 128, the puck moving every block, the warps handed over each block as process() hands them.
	std::vector<float> play(int blocks, bool withWarps) {
		std::vector<float> out;
		std::vector<float> l(128);
		std::vector<float> r(128);
		synth.noteOn(0, 60, 1, 1.0);
		for (int b = 0; b < blocks; ++b) {
			if (withWarps) {
				synth.setLikeWarps(warps.data(), engines.data(), 1);
			}
			set(ParamId::PosX, -0.3 + 0.002 * b);
			synth.render(l.data(), r.data(), 128);
			out.insert(out.end(), l.begin(), l.end());
		}
		return out;
	}
};

/// `count` likes on `engine`, sound A, on a deterministic scatter over +/-1.5 views of the home, each carrying its
/// ANGLES as heard (as the heart captures them).
ui::TerrainWindow withLikes(OscType engine, int count) {
	ui::TerrainWindow window;
	window.engine = engine;
	std::vector<TerrainLike> likes;
	uint32_t h = 2166136261u;
	const auto next = [&h] {
		h ^= h << 13;
		h ^= h >> 17;
		h ^= h << 5;
		return static_cast<double>(h) / 4294967296.0 - 0.5;
	};
	for (int i = 0; i < count; ++i) {
		TerrainLike like;
		like.x = 3.0 * ui::kLikeRegionUnit * next();
		like.y = 3.0 * ui::kLikeRegionUnit * next();
		like.engine = static_cast<uint16_t>(engine);
		like.slot = 0;
		ui::terrainLikeAngles(window, like.x, like.y, like.angles);
		likes.push_back(like);
	}
	window.likes = std::make_shared<const std::vector<TerrainLike>>(likes);
	return window;
}

} // namespace

TEST_CASE("P-MÖBIUS: each builder reads exactly its kind's sites — V5 57 (table 35, authors 10, vowel 12), Basic Shapes 45, Morph 35, Additive 340, Stair 5, Resonate 22") {
	struct Expect {
		OscType engine;
		std::vector<AngleKind> kinds;
	};
	const std::vector<Expect> engines {
		{ OscType::V5, { AngleKind::Morph, AngleKind::Authors, AngleKind::Vox } },
		{ OscType::BasicShapes, { AngleKind::Morph, AngleKind::Authors } },
		{ OscType::Morph, { AngleKind::Morph } },
		{ OscType::Additive, { AngleKind::Partials } },
		{ OscType::Stair, { AngleKind::Stair } },
		{ OscType::Resonate, { AngleKind::Weave } },
		// PLUCK (2026-10-31): its pluck's tone is the Additive spectrum (the Partials sites) and its material five String sites.
		{ OscType::Pluck, { AngleKind::Partials, AngleKind::String } },
		// DRUM (2026-11-05): its seven guest fields, one set of sites for every kind.
		{ OscType::Drum, { AngleKind::Drum } },
	};
	for (const Expect& e : engines) {
		CAPTURE(static_cast<int>(e.engine));
		ui::TerrainWindow window;
		window.engine = e.engine;
		std::array<float, kTerrainLikeAngles> angles {};
		ui::terrainLikeAngles(window, 0.01, -0.02, angles);
		const std::array<bool, kAngleSites> live = ui::likeLiveSites(e.engine);
		for (size_t k = 0; k < kAngleKinds; ++k) {
			CAPTURE(k);
			const bool mine = std::find(e.kinds.begin(), e.kinds.end(), static_cast<AngleKind>(k)) != e.kinds.end();
			int read = 0;
			int marked = 0;
			for (size_t i = kAngleOffsetOf[k]; i < kAngleOffsetOf[k] + kAngleSitesOf[k]; ++i) {
				read += angles[i] != 0.0f ? 1 : 0;
				marked += live[i] ? 1 : 0;
			}
			// EVERY SITE OF ITS OWN KINDS READ AND LIVE, and none of any other kind.
			CHECK(read == (mine ? kAngleSitesOf[k] : 0));
			CHECK(marked == (mine ? kAngleSitesOf[k] : 0));
		}
	}
}

TEST_CASE("P-MÖBIUS: Stair's table, Resonate's string laws and V5's vowel are built through the sound's warp, by value — and Resonate's law memo rebuilds when the warp changes") {
	std::array<LikeWarp, kMaxSounds> table {};
	Character c;
	c.count = 1;
	SoundCoord& s = c.sounds[0];
	s.slot = 0;
	s.weight = 1.0;
	s.zone = 300;
	s.travelX = 0.031;
	s.travelY = -0.017;
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const double rotate = rotateFor(c.orientation);
	Character plain = c;
	c.likes = { &table, 7 };
	// STAIR.
	{
		table[0] = ui::deriveLikeWarp(withLikes(OscType::Stair, 4));
		REQUIRE(table[0].warp != nullptr);
		owlet::dsp::PhiStairParams expected;
		{
			const AngleWarpScope warp(table[0].warp);
			expected = buildStairSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
			                            sliceFrameFor<kStairTriangles>(rotate, home, s.residue.stair), s.gammaBase);
		}
		const owlet::dsp::PhiStairParams got = buildStairFor(c);
		CHECK(std::memcmp(got.heights, expected.heights, sizeof(got.heights)) == 0);
		CHECK(std::memcmp(got.widths, expected.widths, sizeof(got.widths)) == 0);
		CHECK(got.slope == expected.slope);
		// NOT VACUOUS: the plain build differs.
		CHECK(buildStairFor(plain).slope != got.slope);
	}
	// RESONATE, through the memo the synth keeps.
	{
		table[0] = ui::deriveLikeWarp(withLikes(OscType::Resonate, 4));
		REQUIRE(table[0].warp != nullptr);
		owlet::dsp::PhiWeaveParams expected;
		{
			const AngleWarpScope warp(table[0].warp);
			expected = buildWeaveSliced(s.zone, s.phaseOffset, s.travelX, s.travelY,
			                            sliceFrameFor<kWeaveTriangles>(rotate, home, s.residue.weave), s.gammaBase);
		}
		WeaveLawMemo memo;
		const WeaveCharacter before = buildWeaveFor(plain, &memo);
		REQUIRE(memo.built == 1);
		const WeaveCharacter liked = buildWeaveFor(c, &memo);
		CHECK(memo.built == 2); // the warp is one of the memo's inputs
		CHECK(std::memcmp(&liked.a, &expected, sizeof(expected)) == 0);
		CHECK(std::memcmp(&before.a, &expected, sizeof(expected)) != 0);
		(void)buildWeaveFor(c, &memo);
		CHECK(memo.built == 2); // and the same warp again is a hit
	}
	// V5'S VOWEL, at a place in vowel country (searched along the travel: most of the plane has none).
	{
		table[0] = ui::deriveLikeWarp(withLikes(OscType::V5, 4));
		REQUIRE(table[0].warp != nullptr);
		c.engine = OscEngine::Morph;
		c.flagship = true;
		plain.engine = OscEngine::Morph;
		plain.flagship = true;
		bool found = false;
		for (int z = 0; z < 1024 && !found; z += 37) {
			for (int i = -60; i <= 60 && !found; ++i) {
				for (Character* each : { &c, &plain }) {
					each->sounds[0].zone = static_cast<uint16_t>(z);
					each->sounds[0].travelX = 0.01 * i;
				}
				found = buildVoxFor(plain).audible && buildVoxFor(c).audible;
			}
		}
		REQUIRE(found);
		const SoundCoord& v = c.sounds[0];
		const double vHome = sliceHome(v.zone, v.phaseOffset, v.gammaBase);
		const VoxBasis frame = sliceFrameFor<kVoxTriangles>(rotate, vHome, v.residue.vox);
		VoxParams expected;
		{
			const AngleWarpScope warp(table[0].warp);
			expected = buildVoxSliced(v.zone, v.phaseOffset, v.travelX, v.travelY, frame, v.gammaBase);
		}
		const VoxParams plainVox = buildVoxSliced(v.zone, v.phaseOffset, v.travelX, v.travelY, frame, v.gammaBase);
		REQUIRE(expected.formant[0].frequencyHz != plainVox.formant[0].frequencyHz);
		// ONE SOUND COMES BACK AS ITSELF, TO THE BIT (vox_bake's anchoring): the bake's formants are the warped build's.
		CHECK(buildVoxFor(c).params.formant[0].frequencyHz == expected.formant[0].frequencyHz);
		CHECK(buildVoxFor(c).params.formant[1].frequencyHz == expected.formant[1].frequencyHz);
		CHECK(buildVoxFor(plain).params.formant[0].frequencyHz == plainVox.formant[0].frequencyHz);
	}
}

TEST_CASE("P-MÖBIUS: Pluck's material and its pluck's tone are built through the sound's warp, by value") {
	// THE BANK'S ROAD (buildStringFieldsFor, buildPartialsFor) against a restatement: the same sound's five fields and its spectrum
	// read under that sound's warp. And the warp moves them, or the restatement proves nothing.
	std::array<LikeWarp, kMaxSounds> table {};
	table[0] = ui::deriveLikeWarp(withLikes(OscType::Pluck, 4));
	REQUIRE(table[0].warp != nullptr);
	Character c;
	c.engine = OscEngine::String;
	c.count = 1;
	SoundCoord& s = c.sounds[0];
	s.slot = 0;
	s.weight = 1.0;
	s.zone = 300;
	s.travelX = 0.031;
	s.travelY = -0.017;
	Character plain = c;
	c.likes = { &table, 7 };
	const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
	const auto frame = sliceFrameFor<kPartialTriangles>(rotateFor(c.orientation), home, s.residue.partials);
	const StringFields expected = readStringFields(frame, home, s.travelX, s.travelY, table[0].warp);
	const StringFields unwarped = readStringFields(frame, home, s.travelX, s.travelY);
	REQUIRE(!(expected == unwarped));
	CHECK(buildStringFieldsFor(c) == expected);
	CHECK(buildStringFieldsFor(plain) == unwarped);
	partials::PartialSpectrum tone;
	partials::PartialSpectrum plainTone;
	PartialsScratch scratch;
	buildPartialsFor(c, tone, scratch);
	buildPartialsFor(plain, plainTone, scratch);
	CHECK(!(tone == plainTone));
}

TEST_CASE("P-MÖBIUS: the newest like's pin is exact — each site maps the like's own angle to what it was heard as, to the float, on every engine") {
	for (const OscType engine :
	     { OscType::V5, OscType::Additive, OscType::Stair, OscType::Resonate, OscType::BasicShapes, OscType::Pluck,
	       OscType::Drum }) {
		CAPTURE(static_cast<int>(engine));
		const ui::TerrainWindow window = withLikes(engine, 4);
		const LikeWarp warp = ui::deriveLikeWarp(window);
		REQUIRE(warp.warp != nullptr);
		ui::TerrainWindow three = window;
		three.likes = std::make_shared<const std::vector<TerrainLike>>(window.likes->begin(), window.likes->end() - 1);
		const LikeWarp before = ui::deriveLikeWarp(three);
		REQUIRE(before.warp != nullptr);
		const TerrainLike& newest = window.likes->back();
		int live = 0;
		int exact = 0;
		int moved = 0;
		for (size_t i = 0; i < kAngleSites; ++i) {
			if (!warp.warp->sites[i].live) {
				continue;
			}
			++live;
			const float heard = before.warp->at(i, newest.angles[i]);
			exact += warp.warp->at(i, newest.angles[i]) == heard ? 1 : 0;
			// NOT VACUOUS: a place a little way off on the circle is moved.
			const float off = newest.angles[i] + 0.1f < 1.0f ? newest.angles[i] + 0.1f : newest.angles[i] - 0.1f;
			moved += warp.warp->at(i, off) != off ? 1 : 0;
		}
		const std::array<bool, kAngleSites> sites = ui::likeLiveSites(engine);
		CHECK(live == std::count(sites.begin(), sites.end(), true));
		CHECK(exact == live);
		CHECK(moved > live / 2);
	}
}

TEST_CASE("P-MÖBIUS: with every sound's warp in its character, the voice's road allocates nothing — the puck moving, the bank rebuilding") {
	Rig rig;
	const LikeWarp warp = ui::deriveLikeWarp(withLikes(OscType::V5, 5));
	REQUIRE(warp.warp != nullptr);
	rig.warps.fill(warp);
	(void)rig.play(16, true);
	std::vector<float> l(128);
	std::vector<float> r(128);
	allocs.allocations = 0;
	allocs.armed = true;
	for (int b = 0; b < 64; ++b) {
		rig.synth.setLikeWarps(rig.warps.data(), rig.engines.data(), static_cast<uint64_t>(2 + b / 16));
		rig.set(ParamId::PosX, 0.1 + 0.003 * b);
		rig.synth.render(l.data(), r.data(), 128);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	Rig plain;
	Rig liked;
	liked.warps.fill(warp);
	CHECK(liked.play(32, true) != plain.play(32, false));
}

TEST_CASE("P-MÖBIUS: likes saved and loaded into a fresh instance derive the same warp and the voice plays the same, to the bit") {
	const ui::TerrainWindow window = withLikes(OscType::V5, 6);
	TerrainLikes stored;
	stored.likes = *window.likes;
	std::vector<float> words(kTerrainLikesFloats, 0.0f);
	writeTerrainLikes(stored, words.data());
	TerrainLikes loaded;
	REQUIRE(readTerrainLikes(words.data(), words.size(), &loaded));
	REQUIRE(loaded == stored);
	ui::TerrainWindow fresh = window;
	fresh.likes = std::make_shared<const std::vector<TerrainLike>>(loaded.likes);
	const LikeWarp a = ui::deriveLikeWarp(window);
	const LikeWarp b = ui::deriveLikeWarp(fresh);
	REQUIRE(a.warp != nullptr);
	CHECK(a == b);
	Rig first;
	first.warps.fill(a);
	Rig second;
	second.warps.fill(b);
	CHECK(first.play(32, true) == second.play(32, true));
}

TEST_CASE("P-MÖBIUS: no likes is plain by construction — a sound with no warp (or an empty one) installs no angle hook, so every read is the plain read") {
	CHECK(owlet::dsp::phi::angleHook == nullptr);
	{
		const AngleWarpScope none(nullptr);
		const AngleKindScope sites(AngleKind::Morph);
		CHECK(owlet::dsp::phi::angleHook == nullptr);
	}
	const PhaseWarp empty {};
	{
		const AngleWarpScope warp(&empty);
		const AngleKindScope sites(AngleKind::Partials);
		CHECK(owlet::dsp::phi::angleHook == nullptr);
	}
	CHECK(ui::deriveLikeWarp(ui::TerrainWindow {}).identity());
	// NOT VACUOUS: a real warp does install it, and the scope takes it away again.
	const LikeWarp real = ui::deriveLikeWarp(withLikes(OscType::V5, 2));
	REQUIRE(real.warp != nullptr);
	{
		const AngleWarpScope warp(real.warp);
		const AngleKindScope sites(AngleKind::Morph);
		CHECK(owlet::dsp::phi::angleHook != nullptr);
	}
	CHECK(owlet::dsp::phi::angleHook == nullptr);
}

TEST_CASE("P-MÖBIUS: a drum pad's seven fields are built through its sound's warp, by value, in Kit and in Pitched") {
	// THE BANK'S ROAD (buildDrumFor) against a restatement: the same sound's seven fields read under that sound's warp, with its
	// scalers; and the warp moves them, or the restatement proves nothing.
	std::array<LikeWarp, kMaxSounds> table {};
	table[3] = ui::deriveLikeWarp(withLikes(OscType::Drum, 4));
	REQUIRE(table[3].warp != nullptr);
	for (const bool kit : { true, false }) {
		CAPTURE(kit);
		Character c;
		c.engine = OscEngine::Drum;
		c.drumKit = kit;
		c.count = 1;
		SoundCoord& s = c.sounds[0];
		s.slot = 3;
		s.weight = 1.0;
		s.zone = 300;
		s.travelX = 0.031;
		s.travelY = -0.017;
		Character plain = c;
		c.likes = { &table, 7 };
		const double home = sliceHome(s.zone, s.phaseOffset, s.gammaBase);
		const auto frame = sliceFrameFor<kPartialTriangles>(rotateFor(c.orientation), home, s.residue.partials);
		const drum::Fields expected = drumFieldsWithOverrides(readDrumFields(frame, home, s.travelX, s.travelY, table[3].warp), {});
		const drum::Fields unwarped = drumFieldsWithOverrides(readDrumFields(frame, home, s.travelX, s.travelY), {});
		REQUIRE(!(expected == unwarped));
		const size_t pad = kit ? 3 : 0;
		CHECK(buildDrumFor(c).pads[pad] == expected);
		CHECK(buildDrumFor(plain).pads[pad] == unwarped);
	}
}
