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

// THE LIKES' WARP, REPLAYED OVER REAL SESSIONS (backlog ON, 2026-10-28). The editor logs every like edit and the walk
// (plugin/ui/likes_log.h); this replays a log offline and scores the P-MÖBIUS fit at each strength, so a hyperparameter
// sweep runs over what was actually liked rather than over random planes (the study, phase-warp-study @ d8bed47c).
//
// RUN BY HAND:
//   FEATHERS_LIKES_REPLAY=<file.jsonl or a directory of them> [FEATHERS_LIKES_REPLAY_STRENGTHS=1,2,4]
//   [FEATHERS_LIKES_REPLAY_K0=…] [FEATHERS_LIKES_REPLAY_KMAX=…] [FEATHERS_LIKES_REPLAY_KNN=…] [FEATHERS_LIKES_REPLAY_FLOOR=…]
//   scripts/run-cases.sh build/tests/study_likes_replay "STUDY likes replay: sweep the fit's hyperparameters over a logged session"
//
// THE MEASURE, per terrain and in log order: at each like that has likes before it, the warp is fitted to the likes before
// it (deriveLikeWarp, the editor's own road), and scored by the mean over the engine's live sites of log f(θ) — f the
// fitted density the warp transports the uniform onto, read off each site's LUT as du/dθ — at the NEW like's angles
// (held out: a prediction) against the walk lines logged on that terrain since the edit before (implicit negatives).
// lift = like − walk; a hyperparameter that raises it puts the density where the next like lands and not where the player only
// walked. The newest-like pin's Gaussian correction is left out of f (it is ±kPhaseWarpPinWidth wide and exact at one
// angle). Every axis is a comma list and the sweep is their product (PhaseWarpParams: k0, kMax, the k-NN, the width floor).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "dsp/phase_warp.h"
#include "plugin/terrain_likes.h"
#include "plugin/ui/likes_log.h"
#include "plugin/ui/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace feathers;
using namespace feathers::ui;

namespace {

/// log f(θ) at one site: the fitted density is du/dθ, one LUT step of u over the θ it spans.
double logDensity(const PhaseWarpSite& site, float theta) {
	const auto it = std::upper_bound(site.lut.begin(), site.lut.end(), theta);
	const size_t k = std::clamp<size_t>(static_cast<size_t>(it - site.lut.begin()), 1, kPhaseWarpLut - 1) - 1;
	const double span = std::max(static_cast<double>(site.lut[k + 1]) - static_cast<double>(site.lut[k]), 1e-9);
	return std::log((1.0 / (kPhaseWarpLut - 1)) / span);
}

/// The mean over a warp's live sites of log f at a stance's angles.
double score(const PhaseWarp& warp, const TerrainLike& stance) {
	double sum = 0.0;
	int live = 0;
	for (size_t i = 0; i < kAngleSites; ++i) {
		if (warp.sites[i].live) {
			sum += logDensity(warp.sites[i], stance.angles[i]);
			++live;
		}
	}
	return live > 0 ? sum / live : 0.0;
}

struct Row {
	int events = 0;
	int walks = 0;
	double like = 0.0;
	double walk = 0.0;
	double liftSum = 0.0;
	int liftEvents = 0;
};

/// ONE POINT OF THE SWEEP: the strength and the fit's other hyperparameters.
struct Setting {
	double strength = 1.0;
	PhaseWarpParams params {};
};
using Rows = std::map<std::pair<uint16_t, size_t>, Row>;

/// The sweep over `records`: one row per (engine, setting index). The set is rebuilt line by line (likeslog::Replay), so each
/// like is scored against exactly the likes that stood before it.
Rows sweep(const std::vector<likeslog::Record>& records, const std::vector<Setting>& settings) {
	Rows rows;
	std::map<std::pair<uint16_t, uint16_t>, std::vector<TerrainLike>> walks;
	likeslog::Replay set;
	for (const likeslog::Record& r : records) {
		if (r.event == "walk") {
			if (r.like.has_value()) {
				walks[{ r.engine, r.slot }].push_back(*r.like);
			}
			continue;
		}
		if (r.event == "like" && r.cause == "like" && r.like.has_value()) {
			const std::vector<TerrainLike> before = set.likesOf(r.engine, r.slot);
			const std::vector<TerrainLike>& walked = walks[{ r.engine, r.slot }];
			for (size_t k = 0; k < settings.size(); ++k) {
				if (before.empty()) {
					break;
				}
				TerrainWindow window;
				window.engine = static_cast<OscType>(r.engine);
				window.likeStrength = settings[k].strength;
				window.likeParams = settings[k].params;
				window.likes = std::make_shared<const std::vector<TerrainLike>>(before);
				const LikeWarp warp = deriveLikeWarp(window);
				if (warp.warp == nullptr) {
					continue;
				}
				Row& row = rows[{ r.engine, k }];
				const double liked = score(*warp.warp, *r.like);
				row.like += liked;
				++row.events;
				// THE NEGATIVES: the stances walked since the last edit, less the one this like stands on.
				double walkedSum = 0.0;
				int counted = 0;
				for (const TerrainLike& w : walked) {
					if (likeslog::soundId(w) != likeslog::soundId(*r.like)) {
						walkedSum += score(*warp.warp, w);
						++counted;
					}
				}
				if (counted > 0) {
					row.walk += walkedSum;
					row.walks += counted;
					row.liftSum += liked - walkedSum / counted;
					++row.liftEvents;
				}
			}
		}
		set.apply(r);
		if (r.event == "like" || r.event == "unlike") {
			walks[{ r.engine, r.slot }].clear();
		}
	}
	return rows;
}

std::vector<likeslog::Record> readPath(const std::filesystem::path& path) {
	std::vector<std::filesystem::path> files;
	if (std::filesystem::is_directory(path)) {
		for (const auto& entry : std::filesystem::directory_iterator(path)) {
			if (entry.path().extension() == ".jsonl") {
				files.push_back(entry.path());
			}
		}
		std::sort(files.begin(), files.end());
	}
	else {
		files.push_back(path);
	}
	std::vector<likeslog::Record> out;
	for (const auto& file : files) {
		size_t skipped = 0;
		for (likeslog::Record& r : likeslog::read(file, &skipped)) {
			out.push_back(std::move(r));
		}
		if (skipped > 0) {
			MESSAGE(file.string() << ": " << skipped << " lines did not parse");
		}
	}
	return out;
}

void report(const Rows& rows, const std::vector<Setting>& settings) {
	for (const auto& [key, row] : rows) {
		const size_t engine = key.first;
		const std::string name = engine < kOscTypeNames.size() ? kOscTypeNames[engine] : "?";
		MESSAGE(name
		        << " strength x" << settings[key.second].strength << " k0 " << settings[key.second].params.prior << " kMax "
		        << settings[key.second].params.maxComponents << " knn " << settings[key.second].params.neighbours << " floor "
		        << settings[key.second].params.widthFloor << ": " << row.events << " held-out likes, mean log f "
		        << (row.events > 0 ? row.like / row.events : 0.0) << "; " << row.walks << " walk stances, mean log f "
		        << (row.walks > 0 ? row.walk / row.walks : 0.0) << "; lift "
		        << (row.liftEvents > 0 ? row.liftSum / row.liftEvents : 0.0) << " over " << row.liftEvents << " likes");
	}
}

} // namespace

TEST_CASE("STUDY likes replay: sweep the fit's hyperparameters over a logged session" * doctest::skip()) {
	const char* path = std::getenv("FEATHERS_LIKES_REPLAY");
	REQUIRE_MESSAGE(path != nullptr, "set FEATHERS_LIKES_REPLAY to a .jsonl file or a directory of them");
	// EACH AXIS A COMMA LIST (unset: the shipped value alone, strength the menu's three); the sweep is their product.
	const auto list = [](const char* name, std::vector<double> fallback) {
		const char* text = std::getenv(name);
		if (text == nullptr || text[0] == '\0') {
			return fallback;
		}
		std::vector<double> out;
		std::stringstream in(text);
		std::string item;
		while (std::getline(in, item, ',')) {
			out.push_back(std::strtod(item.c_str(), nullptr));
		}
		return out;
	};
	std::vector<Setting> settings;
	for (const double s : list("FEATHERS_LIKES_REPLAY_STRENGTHS", { 1.0, 2.0, 4.0 })) {
		for (const double k0 : list("FEATHERS_LIKES_REPLAY_K0", { kPhaseWarpPrior })) {
			for (const double kMax : list("FEATHERS_LIKES_REPLAY_KMAX", { static_cast<double>(kPhaseWarpMaxComponents) })) {
				for (const double knn : list("FEATHERS_LIKES_REPLAY_KNN", { static_cast<double>(kPhaseWarpNeighbours) })) {
					for (const double floor : list("FEATHERS_LIKES_REPLAY_FLOOR", { kPhaseWarpWidthFloor })) {
						Setting setting;
						setting.strength = s;
						setting.params.prior = k0;
						setting.params.maxComponents = static_cast<int>(kMax);
						setting.params.neighbours = static_cast<int>(knn);
						setting.params.widthFloor = floor;
						settings.push_back(setting);
					}
				}
			}
		}
	}
	const std::vector<likeslog::Record> records = readPath(path);
	const likeslog::Replay replayed = likeslog::replay(records);
	MESSAGE(records.size() << " lines, " << replayed.records << " edits (" << replayed.snapshots << " snapshots), " << replayed.mismatches << " deltas that did not fit");
	report(sweep(records, settings), settings);
}

TEST_CASE("study likes replay: the sweep scores a log written here — every held-out like and walk, a finite lift, and the likes' own country denser than the walk's") {
	// THE HARNESS KEPT HONEST: a log written through the real writer (a temporary directory, never the user's), three
	// likes in one corner of Additive's plane and walk stances far away, swept at two strengths.
	const std::filesystem::path dir = std::filesystem::temp_directory_path()
	                                  / ("feathers-likes-replay-" + std::to_string(static_cast<long>(::getpid())));
	std::filesystem::remove_all(dir);
	REQUIRE(setenv(likeslog::kEnvVar, dir.c_str(), 1) == 0);
	TerrainWindow window;
	window.engine = OscType::Additive;
	std::vector<TerrainLike> set;
	double t = 1.0e9;
	const auto stance = [&window](double x, double y) {
		TerrainLike s;
		s.x = x;
		s.y = y;
		s.engine = static_cast<uint16_t>(OscType::Additive);
		s.slot = 0;
		s.pointCount = 1;
		s.point[0] = x + 2.0 * y;
		terrainLikeAngles(window, x, y, s.angles);
		return s;
	};
	const auto write = [&](const char* event, const TerrainLike& like, int index) {
		likeslog::Record r;
		r.time = (t += 1.0);
		r.session = likeslog::session();
		r.event = event;
		r.cause = index >= 0 ? event : "";
		r.engine = like.engine;
		r.slot = like.slot;
		r.index = index;
		r.like = like;
		REQUIRE(likeslog::append(r));
	};
	likeslog::Record snapshot;
	snapshot.time = t;
	snapshot.session = likeslog::session();
	snapshot.event = "snapshot";
	snapshot.likes.emplace();
	REQUIRE(likeslog::append(snapshot));
	for (int i = 0; i < 4; ++i) {
		write("walk", stance(0.4 - 0.05 * i, -0.4), -1);
		const TerrainLike like = stance(0.01 * i, 0.012 * i);
		set.push_back(like);
		write("like", like, i);
	}
	unsetenv(likeslog::kEnvVar);
	const std::vector<likeslog::Record> records = readPath(dir);
	std::filesystem::remove_all(dir);
	REQUIRE(records.size() == 9);
	const likeslog::Replay replayed = likeslog::replay(records);
	CHECK(replayed.mismatches == 0);
	CHECK(replayed.likes.likes == set);
	std::vector<Setting> settings(3);
	settings[1].strength = 4.0;
	settings[2].params.prior = 30.0;
	const Rows rows = sweep(records, settings);
	report(rows, settings);
	const auto additive = static_cast<uint16_t>(OscType::Additive);
	for (size_t k = 0; k < settings.size(); ++k) {
		CAPTURE(k);
		const auto it = rows.find({ additive, k });
		REQUIRE(it != rows.end());
		CHECK(it->second.events == 3);
		CHECK(it->second.walks == 3);
		CHECK(it->second.liftEvents == 3);
		CHECK(std::isfinite(it->second.liftSum));
		CHECK(it->second.liftSum > 0.0);
	}
	// AND THE KNOBS MOVE IT: ×4 weighs the likes over the uniform more, so the held-out likes score higher; a k0 of 30 weighs
	// the uniform more, so they score lower.
	CHECK(rows.at({ additive, 1 }).like > rows.at({ additive, 0 }).like);
	CHECK(rows.at({ additive, 2 }).like < rows.at({ additive, 0 }).like);
}
