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

// THE PATTERN LIKES' PHASE WARP, MEASURED BEFORE IT SHIPS (design §14-bis; Feathers' own likes instrument, scaled to
// this device). A simulated listener with a target pattern likes the
// best of 16 places within ±4 views, n times; then on RANDOM PLANES (a turn anywhere, the home within one gamma unit)
// a place is a HIT when its fingerprint is within the plain plane's 10th percentile of distance to the target. LIFT = the
// hit share with the warp minus without. MODELLED: the listener stands in for a real player, so the ranking is evidence
// about mechanism and the magnitudes are not a player's.
//
// Under ctest: the quick checks only (no likes is the plain decode to the bit; a like's own place, on its own plane,
// decodes to what it was heard as). FEATHERS_STUDY=1 runs the sweep and prints its table.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/arp/arp_core.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_likes.h"
#include "mod/arp/pattern_types.h"
#include "mod/arp/pattern_warp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

namespace pat = feathers::mod::arp::pat;
namespace pn = feathers::mod::arp::pattern;
namespace gn = feathers::mod::arp::genes;
namespace likes = feathers::mod::arp::likes;

struct Plane {
	pat::PatternSettings q;
};

pat::PatternSettings startPlane(gn::Mode mode) {
	pat::PatternSettings q;
	q.mode = mode;
	q.zone = 300;
	q.length = gn::kLikeLength;
	return q;
}

gn::Place placeAt(const pat::PatternSettings& plane, double x, double y) {
	pat::PatternSettings c = plane;
	c.placeX = static_cast<float>(x);
	c.placeY = static_cast<float>(y);
	return pat::PatternDevice::placeOf(c, feathers::SliceResidue<gn::kRowCount> {});
}

likes::Fingerprint heardAt(const pat::PatternSettings& plane, const gn::Place& place, const gn::Warp* warp) {
	pat::Settings s;
	s.patterns[0] = plane;
	std::array<bool, gn::kPinCount> on {};
	on.fill(true);
	pn::Recipe r = pat::recipeFor(s, 0, on);
	r.voice = false;
	r.length = gn::kLikeLength;
	r.warp = warp;
	feathers::mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	pn::PatternCycle cycle;
	pn::build(place, r, held, cycle);
	return likes::fingerprintOf(cycle, plane.mode, 60);
}

struct Row {
	double plainHit = 0.0;
	double warpHit = 0.0;
	double ownPlaneScoreGain = 0.0;
};

} // namespace

TEST_CASE("pattern warp: no likes is the plain decode to the bit, and a like's own place decodes to what it was heard as") {
	const pat::PatternSettings plane = startPlane(gn::Mode::Melody);
	auto warp = std::make_unique<gn::Warp>();
	std::vector<gn::SiteAngles> none;
	gn::buildWarp(none, 1.0, *warp);
	CHECK(warp->identity());
	std::mt19937 rng(7);
	std::uniform_real_distribution<double> u(-4.0, 4.0);
	for (int i = 0; i < 20; ++i) {
		const gn::Place p = placeAt(plane, u(rng), u(rng));
		CHECK(heardAt(plane, p, warp.get()) == heardAt(plane, p, nullptr));
	}
	// THE CHAIN'S PIN: each like's own angles map to what they were heard as under the warp before it, so the newest
	// like's place, under the warp it ends, decodes as it did under the one before (and the first like, as plain).
	std::vector<gn::SiteAngles> liked;
	std::vector<gn::Place> places;
	for (int i = 0; i < 4; ++i) {
		places.push_back(placeAt(plane, u(rng), u(rng)));
		liked.push_back(gn::anglesOf(places.back()));
	}
	auto before = std::make_unique<gn::Warp>();
	for (size_t n = 1; n <= liked.size(); ++n) {
		gn::buildWarp(std::span(liked.data(), n), 1.0, *warp);
		CAPTURE(n);
		CHECK(heardAt(plane, places[n - 1], warp.get()) == heardAt(plane, places[n - 1], n == 1 ? nullptr : before.get()));
		*before = *warp;
	}
}

TEST_CASE("pattern warp: STUDY — lift on random planes, by mode, likes and strength; and the costs (FEATHERS_STUDY=1)") {
	const char* on = std::getenv("FEATHERS_STUDY");
	if (on == nullptr || std::string(on) != "1") {
		MESSAGE("skipped: set FEATHERS_STUDY=1 to run the sweep");
		return;
	}
	constexpr int kSeeds = 3;
	constexpr int kPlanes = 8;
	constexpr int kPoints = 200;
	constexpr int kCandidates = 16;
	const gn::Mode modes[] = { gn::Mode::Melody, gn::Mode::Chords, gn::Mode::TbMono };
	const char* modeNames[] = { "Melody", "Chords", "TB-Mono" };
	const size_t counts[] = { 4, 8, 16 };
	const double strengths[] = { 1.0, 2.0, 4.0 };
	auto warp = std::make_unique<gn::Warp>();
	double buildMsAt16 = 0.0;
	int builds16 = 0;
	for (size_t m = 0; m < std::size(modes); ++m) {
		const pat::PatternSettings start = startPlane(modes[m]);
		for (const double strength : strengths) {
			for (const size_t n : counts) {
				double lift = 0.0;
				double plainShare = 0.0;
				double warpShare = 0.0;
				std::vector<double> perSeed;
				for (int seed = 0; seed < kSeeds; ++seed) {
					std::mt19937 rng(11 + seed);
					std::uniform_real_distribution<double> near(-2.0, 2.0);
					std::uniform_real_distribution<double> wide(-4.0, 4.0);
					const likes::Fingerprint target = heardAt(start, placeAt(start, near(rng), near(rng)), nullptr);
					// THE LISTENER, GREEDY: each like the best of 16 places within ±4, judged plain.
					std::vector<gn::SiteAngles> liked;
					for (size_t i = 0; i < n; ++i) {
						double best = 1e300;
						gn::Place bestPlace;
						for (int c = 0; c < kCandidates; ++c) {
							const gn::Place p = placeAt(start, wide(rng), wide(rng));
							const double d = likes::distance(heardAt(start, p, nullptr), target);
							if (d < best) {
								best = d;
								bestPlace = p;
							}
						}
						liked.push_back(gn::anglesOf(bestPlace));
					}
					const auto t0 = std::chrono::steady_clock::now();
					gn::buildWarp(liked, strength, *warp);
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
					if (n == 16) {
						buildMsAt16 += ms;
						++builds16;
					}
					// THE RANDOM PLANES: a turn anywhere, the home within one gamma unit.
					std::mt19937 planes(101 + seed);
					std::uniform_real_distribution<double> turn(-3.14159265358979, 3.14159265358979);
					std::uniform_real_distribution<double> home(-1.0, 1.0);
					std::uniform_real_distribution<double> spread(-8.0, 8.0);
					double seedLift = 0.0;
					for (int j = 0; j < kPlanes; ++j) {
						pat::PatternSettings plane = start;
						plane.rotate = static_cast<float>(turn(planes));
						plane.gamma = std::abs(home(planes));
						std::vector<double> plain;
						std::vector<double> warped;
						std::mt19937 points(77);
						for (int k = 0; k < kPoints; ++k) {
							const gn::Place p = placeAt(plane, spread(points), spread(points));
							plain.push_back(likes::distance(heardAt(plane, p, nullptr), target));
							warped.push_back(likes::distance(heardAt(plane, p, warp.get()), target));
						}
						std::vector<double> sorted = plain;
						std::sort(sorted.begin(), sorted.end());
						const double threshold = sorted[static_cast<size_t>(kPoints / 10)];
						const auto share = [&](const std::vector<double>& d) {
							return static_cast<double>(std::count_if(d.begin(), d.end(), [&](double v) { return v <= threshold; }))
							       / kPoints;
						};
						plainShare += share(plain);
						warpShare += share(warped);
						seedLift += share(warped) - share(plain);
					}
					perSeed.push_back(seedLift / kPlanes);
					lift += seedLift / kPlanes;
				}
				const auto [lo, hi] = std::minmax_element(perSeed.begin(), perSeed.end());
				std::printf("%-8s strength %.0f n %2zu: lift %+.3f [%+.3f, %+.3f]  hit plain %.3f warp %.3f\n", modeNames[m],
				            strength, n, lift / kSeeds, *lo, *hi, plainShare / (kSeeds * kPlanes), warpShare / (kSeeds * kPlanes));
			}
		}
	}
	// THE COSTS: a 16-like chain on the message thread, and one decode with and without the warp.
	std::printf("build, 16 likes (the whole chain): %.1f ms mean over %d\n", buildMsAt16 / builds16, builds16);
	const pat::PatternSettings start = startPlane(gn::Mode::Melody);
	std::mt19937 rng(5);
	std::uniform_real_distribution<double> u(-4.0, 4.0);
	std::vector<gn::Place> places;
	for (int i = 0; i < 2000; ++i) {
		places.push_back(placeAt(start, u(rng), u(rng)));
	}
	for (int pass = 0; pass < 2; ++pass) {
		for (const gn::Warp* w : { static_cast<const gn::Warp*>(nullptr), static_cast<const gn::Warp*>(warp.get()) }) {
			const auto t0 = std::chrono::steady_clock::now();
			double sink = 0.0;
			for (const gn::Place& p : places) {
				sink += heardAt(start, p, w)[0];
			}
			const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
			std::printf("decode %s: %.2f us (sink %.3f)\n", w == nullptr ? "plain" : "warped", us, sink);
		}
	}
}
