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
#include "test_heap.h"
#include "doctest.h"

#include "dsp/drum_voice.h"
#include "fx/drum/drum.h"
#include "fx/drum/drum_likes.h"
#include "fx/drum/drum_types.h"
#include "persist_rig.h"
#include "plugin/ui/drum_device_pad.h"
#include "plugin/ui/drum_like_surface.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// THE DRUM'S LIKES, BY VALUE: the stored block (saved, loaded into a fresh instance, refused when malformed), the terrain as a like
// surface (the fingerprint is the fields the Kind reads, restated here from `fieldsOf`), the warp the pad is drawn through (the policy's
// arithmetic equals the derived warp's, and the liked place is given room), and Mutate's roll steered by it (nearer the liked fields
// than the plain roll, by an independent measure over the fields).

namespace dd = feathers::fx::drumdevice;
namespace fd = feathers::drum;
using namespace feathers;

namespace {

dd::Settings defaults() {
	return dd::stateOf(dd::Row {}, 0);
}

dd::Settings at(dd::Settings s, double x, double y) {
	s.v[static_cast<size_t>(dd::Param::TravelX)] = static_cast<float>(x);
	s.v[static_cast<size_t>(dd::Param::TravelY)] = static_cast<float>(y);
	return s;
}

/// The seven fields as `fieldsOf` decodes them at a place, restated as an array.
std::array<float, 7> heard(const dd::Settings& s, double x, double y) {
	return dd::fieldsArray(dd::fieldsOf(at(s, x, y)));
}

/// THE DISTANCE BETWEEN TWO DRUMS over the fields the Kind reads: the root of the mean squared difference.
double fieldDistance(const dd::Settings& s, const std::array<float, 7>& a, const std::array<float, 7>& b) {
	const fd::FieldUse r = fd::kindReads(static_cast<fd::Kind>(s.index(dd::Param::Kind)));
	const std::array<bool, 7> read = { r.pitch, r.length, r.sweep, r.tone, r.noise, r.snap, r.metal };
	double sum = 0.0;
	int n = 0;
	for (size_t i = 0; i < 7; ++i) {
		if (read[i]) {
			sum += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
			++n;
		}
	}
	return n > 0 ? std::sqrt(sum / n) : 0.0;
}

std::vector<dd::DrumLike> sampleLikes(const dd::Settings& s) {
	std::vector<dd::DrumLike> likes;
	likes.push_back(ui::likeOf(at(s, 0.5, -0.25)));
	likes.push_back(ui::likeOf(at(s, 3.0, 2.0))); // (a lens needs room: likes closer than ~0.8 of a x1 view keep none)
	return likes;
}

struct Dice {
	uint32_t state = 0x2545f491u;
	double next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return static_cast<double>(state) / 4294967296.0 * 2.0 - 1.0;
	}
};

} // namespace

TEST_CASE("drum likes: the block round-trips every field exactly, and a malformed one is refused whole") {
	const dd::Settings s = defaults();
	std::vector<dd::DrumLike> likes = sampleLikes(s);
	likes[1].x = 1.0e7 + 0.123456789; // a double keeps what a float would not
	likes[1].zone = 777;
	likes[1].kind = 3;
	std::vector<float> block(dd::kLikesBlockWords);
	dd::writeLikeBlock(likes, block.data());
	std::vector<dd::DrumLike> back;
	REQUIRE(dd::readLikeBlock(block.data(), block.size(), &back));
	CHECK(back == likes);
	CHECK(back[1].x == 1.0e7 + 0.123456789);
	// THE REFUSALS, each by itself: the count past the cap, a kind outside the six, a field outside [0, 1], a wrong length, a word that is not whole.
	std::vector<float> bad = block;
	bad[0] = static_cast<float>(dd::kMaxDrumLikes + 1);
	CHECK_FALSE(dd::readLikeBlock(bad.data(), bad.size(), nullptr));
	std::vector<dd::DrumLike> wild = likes;
	wild[0].kind = static_cast<uint16_t>(fd::kKindCount);
	dd::writeLikeBlock(wild, bad.data());
	CHECK_FALSE(dd::readLikeBlock(bad.data(), bad.size(), nullptr));
	wild = likes;
	wild[0].fields[3] = 1.5f;
	dd::writeLikeBlock(wild, bad.data());
	CHECK_FALSE(dd::readLikeBlock(bad.data(), bad.size(), nullptr));
	CHECK_FALSE(dd::readLikeBlock(block.data(), block.size() - 1, nullptr));
	bad = block;
	bad[5] = 0.5f;
	CHECK_FALSE(dd::readLikeBlock(bad.data(), bad.size(), nullptr));
	// A DEVICE REFUSES IT TOO, and keeps what it had.
	dd::DrumDsp device;
	CHECK_FALSE(device.writeLikes(bad.data(), bad.size()));
	std::vector<float> read(dd::kLikesBlockWords);
	CHECK(device.readLikes(read.data(), read.size()) == 0);
}

TEST_CASE("drum likes: likes set on a device are saved with the patch and come back into a FRESH rack, by value") {
	persist::Rig a;
	a.seat(dd::kTraits.tag);
	a.block();
	const dd::Settings s = defaults();
	std::vector<dd::DrumLike> likes = sampleLikes(s);
	likes.push_back(ui::likeOf(at(s, -2.0, 3.5)));
	std::vector<float> words(dd::kLikesBlockWords);
	dd::writeLikeBlock(likes, words.data());
	REQUIRE(a.bridge->setBlock(a.id, dd::kLikesBlock, words));
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block();
	std::vector<float> got(dd::kLikesBlockWords);
	REQUIRE(b.bridge->blockAt(b.id, dd::kLikesBlock, got));
	std::vector<dd::DrumLike> back;
	REQUIRE(dd::readLikeBlock(got.data(), got.size(), &back));
	CHECK(back == likes);
	// THE INSTANCE ITSELF HOLDS THEM (what a Drum preset's copy carries), not only the host's store.
	std::vector<float> inside(dd::kLikesBlockWords);
	REQUIRE(b.as<dd::DrumDsp>().readLikes(inside.data(), inside.size()) != 0);
	CHECK(inside == words);
	// NOT VACUOUS: a patch saved before any like carries none.
	persist::Rig blank;
	blank.seat(dd::kTraits.tag);
	blank.block();
	persist::Rig blankBack;
	blankBack.load(blank.save());
	blankBack.block();
	std::vector<float> none(dd::kLikesBlockWords);
	std::vector<dd::DrumLike> noLikes;
	const bool have = blankBack.bridge->blockAt(blankBack.id, dd::kLikesBlock, none);
	CHECK((!have || (dd::readLikeBlock(none.data(), none.size(), &noLikes) && noLikes.empty())));
}

TEST_CASE("drum likes: the surface's fingerprint is the fields the Kind reads, in order, and zero elsewhere") {
	for (int kind = 0; kind < static_cast<int>(fd::kKindCount); ++kind) {
		dd::Settings s = defaults();
		s.v[static_cast<size_t>(dd::Param::Kind)] = static_cast<float>(kind);
		const ui::DrumLikeSurface surface(s);
		const std::array<ui::WarpPoint, 2> where = { ui::drumWarpOf(0.375, -0.5), ui::drumWarpOf(2.0, 1.25) };
		std::vector<ui::LikeVector> prints;
		surface.fingerprintsAt(where, prints);
		REQUIRE(prints.size() == 2);
		const fd::FieldUse r = fd::kindReads(static_cast<fd::Kind>(kind));
		const std::array<bool, 7> read = { r.pitch, r.length, r.sweep, r.tone, r.noise, r.snap, r.metal };
		for (size_t p = 0; p < 2; ++p) {
			const std::array<float, 7> f = p == 0 ? heard(s, 0.375, -0.5) : heard(s, 2.0, 1.25);
			for (size_t i = 0; i < 7; ++i) {
				CHECK(prints[p][i] == (read[i] ? static_cast<double>(f[i]) : 0.0));
			}
			for (size_t i = 7; i < prints[p].size(); ++i) {
				CHECK(prints[p][i] == 0.0);
			}
		}
	}
	CHECK(ui::DrumLikeSurface(defaults()).controlCount() == 0);
}

TEST_CASE("drum likes: a like applies only on its own Kind and plane") {
	const dd::Settings s = defaults();
	const dd::DrumLike like = ui::likeOf(at(s, 0.5, 0.5));
	CHECK(ui::likeApplies(like, s));
	dd::Settings other = s;
	other.v[static_cast<size_t>(dd::Param::Kind)] = 2.0f;
	CHECK_FALSE(ui::likeApplies(like, other));
	other = s;
	other.v[static_cast<size_t>(dd::Param::Zone)] = 12.0f;
	CHECK_FALSE(ui::likeApplies(like, other));
	other = s;
	other.v[static_cast<size_t>(dd::Param::Gamma)] = 0.25f;
	CHECK_FALSE(ui::likeApplies(like, other));
	// A TRAVEL MOVE is not a change of plane: the like still applies where the drum has walked to.
	CHECK(ui::likeApplies(like, at(s, 3.0, -2.0)));
	// ITS FIELDS ARE THE ONES HEARD WHEN IT WAS MADE.
	CHECK(like.fields == heard(s, 0.5, 0.5));
	CHECK(ui::deriveDrumLikes(other, { like }) == nullptr);
}

TEST_CASE("drum likes: the pad's warp is the derived warp, scaled, and the liked place is drawn larger") {
	const dd::Settings s = defaults();
	const std::vector<dd::DrumLike> likes = sampleLikes(s);
	const auto state = ui::deriveDrumLikes(s, likes);
	REQUIRE(state != nullptr);
	CHECK(state->applied.size() == 2);
	ui::DrumPadContext c;
	c.settings = s;
	c.likes = state;
	const ui::DrumDevicePadPolicy::Phases origin = { 0.0, 0.0 };
	// THE POLICY'S MAP, restated from the derivation: the view place in warp units, through worldAt, back to travel.
	for (const std::array<double, 2> v : { std::array<double, 2> { 0.5, -0.25 }, { 0.8, -0.1 }, { -1.0, 2.0 }, { 0.55, -0.2 } }) {
		const ui::WarpPoint w = state->derivation.warp.worldAt({ v[0] * ui::kDrumWarpPerTravel, v[1] * ui::kDrumWarpPerTravel });
		const auto got = ui::DrumDevicePadPolicy::worldFromView(c, origin, v[0], v[1]);
		CHECK(got.first == doctest::Approx(w.x / ui::kDrumWarpPerTravel).epsilon(1e-12));
		CHECK(got.second == doctest::Approx(w.y / ui::kDrumWarpPerTravel).epsilon(1e-12));
		const auto back = ui::DrumDevicePadPolicy::viewFromWorld(c, origin, got.first, got.second);
		CHECK(back.first == doctest::Approx(v[0]).epsilon(1e-6));
		CHECK(back.second == doctest::Approx(v[1]).epsilon(1e-6));
	}
	// THE LIKED PLACE IS GIVEN ROOM: a step of a hundredth of a travel unit in the world is drawn many times longer at the like (the lens's
	// magnification, which the cartogram under it only adds to), and the far field is the identity, exactly.
	const double step = 0.01;
	const std::array<double, 2> a = state->viewAt(0.5, -0.25);
	const std::array<double, 2> b = state->viewAt(0.5 + step, -0.25);
	const double drawn = std::hypot(b[0] - a[0], b[1] - a[1]);
	CHECK(drawn / step > 4.0);
	const std::array<double, 2> far = state->worldAt(400.0, -300.0);
	CHECK(far[0] == 400.0);
	CHECK(far[1] == -300.0);
	// NO LIKES, NO WARP: the policy is the identity, to the bit.
	const ui::DrumPadContext none;
	const auto same = ui::DrumDevicePadPolicy::worldFromView(none, origin, 0.5, -0.25);
	CHECK(same.first == 0.5);
	CHECK(same.second == -0.25);
}

TEST_CASE("drum likes: a roll of Mutate on the warped view lands nearer the liked fields than the plain roll") {
	const dd::Settings s = defaults();
	const std::vector<dd::DrumLike> likes = sampleLikes(s);
	const auto state = ui::deriveDrumLikes(s, likes);
	REQUIRE(state != nullptr);
	// THE PLAIN ROLL IS THE PLAIN SUM, to the bit, with no state.
	CHECK(ui::drumMutate(nullptr, 1.0, 2.0, 0.25, -0.5, 3.0)[0] == 1.0 + 0.25 * 3.0);
	CHECK(ui::drumMutate(nullptr, 1.0, 2.0, 0.25, -0.5, 3.0)[1] == 2.0 + -0.5 * 3.0);
	// FROM NEXT TO THE LIKES, a reach of two travel units: the rolls' mean distance, over the fields the Kind reads, to the liked fields.
	const double startX = 0.9;
	const double startY = -0.1;
	const double reach = 2.0;
	Dice dicePlain;
	Dice diceWarped;
	double plain = 0.0;
	double warped = 0.0;
	const int rolls = 400;
	for (int i = 0; i < rolls; ++i) {
		const std::array<double, 2> p = ui::drumMutate(nullptr, startX, startY, dicePlain.next(), dicePlain.next(), reach);
		const std::array<double, 2> w = ui::drumMutate(state.get(), startX, startY, diceWarped.next(), diceWarped.next(), reach);
		double dp = 1e9;
		double dw = 1e9;
		for (const dd::DrumLike& like : likes) {
			dp = std::min(dp, fieldDistance(s, heard(s, p[0], p[1]), like.fields));
			dw = std::min(dw, fieldDistance(s, heard(s, w[0], w[1]), like.fields));
		}
		plain += dp;
		warped += dw;
	}
	plain /= rolls;
	warped /= rolls;
	CHECK(warped < 0.9 * plain);
	// AND THE ROLL IS NOT A NO-OP: the warped rolls do not equal the plain ones.
	CHECK(ui::drumMutate(state.get(), startX, startY, 0.5, 0.5, reach)[0] != ui::drumMutate(nullptr, startX, startY, 0.5, 0.5, reach)[0]);
}
