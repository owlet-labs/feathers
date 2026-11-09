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

#include "dsp/drum_picture.h"

#include <algorithm>
#include <cmath>

namespace feathers::drum {

void heardSpectrum(Kind kind, const Fields& f, int count, partials::PartialSpectrum& out, double maxRatio) {
	out = {};
	const int n = std::clamp(count, 0, partials::kMaxPartials);
	int at = 0;
	const auto put = [&](double ratio, double amplitude) {
		if (at >= n || !(ratio > 0.0) || ratio >= maxRatio || !(amplitude > 0.0)) {
			return;
		}
		const auto i = static_cast<size_t>(at);
		out.ratio[i] = static_cast<float>(ratio);
		out.envAtRatio[i] = static_cast<float>(amplitude);
		out.fine[i] = 1.0f;
		++at;
	};
	// A NOISE BAND: seven partials evenly across an octave around `centre`, sharing `amplitude`'s energy.
	const auto band = [&](double centre, double amplitude) {
		constexpr int kBand = 7;
		for (int j = 0; j < kBand; ++j) {
			put(centre * std::exp2((static_cast<double>(j) - 3.0) / 6.0), amplitude / std::sqrt(static_cast<double>(kBand)));
		}
	};
	const double base = kindBaseHz(kind);
	const Laws l = lawsFor(kind, f, Controls {}, base, 0.5);
	const double p = l.hz / base; // the Pitch field's offset, as a ratio
	const double tone = static_cast<double>(l.tone);
	const double noise = static_cast<double>(l.noise);
	switch (kind) {
	case Kind::Kick:
		put(p, 1.0);
		put(2.0 * p, 0.5 * tone);
		put(3.0 * p, 0.25 * tone);
		band(4000.0 / base, 0.5 * static_cast<double>(l.snap) * std::sqrt(0.01 / std::max(1.0e-3, l.t60)));
		if (noise > 0.0) {
			band(8.0 * p, 0.25 * noise * std::sqrt(0.5));
		}
		break;
	case Kind::Membrane:
	case Kind::Snare: {
		const int modes = kind == Kind::Membrane ? kMembraneModes : kSnareHeadModes;
		double sum = 0.0;
		for (int k = 0; k < modes; ++k) {
			sum += std::pow(modeRatio(k, l.metal), -(2.2 - 2.0 * tone));
		}
		const double head = kind == Kind::Snare ? 1.0 - 0.6 * noise : 1.0;
		for (int k = 0; k < modes; ++k) {
			const double ratio = modeRatio(k, l.metal);
			const double a = head * std::pow(ratio, -(2.2 - 2.0 * tone)) / sum;
			put(p * ratio, a * std::sqrt(modeT60(l.t60, ratio, l.metal) / l.t60));
		}
		if (kind == Kind::Snare) {
			band(wireCentreHz(l.tone, l.hz) / base, (0.3 + 0.7 * noise) * std::sqrt(0.8));
		}
		else if (noise > 0.0) {
			band(2.0 * p, 0.5 * noise * std::sqrt(0.3));
		}
		break;
	}
	case Kind::Metal:
		for (int i = 0; i < kMetalOscillators; ++i) {
			const double r = p * metalRatio(i, l.metal);
			for (const int h : { 1, 3, 5 }) {
				put(r * h, (1.0 - noise) / (6.0 * h));
			}
		}
		band(metalCentreHz(l.tone) / base, std::max(0.05, noise));
		break;
	case Kind::Clap: band(clapCentreHz(l.tone) / base * p, 1.0); break;
	case Kind::String:
		for (int k = 1; k <= 16; ++k) {
			put(p * k, std::pow(static_cast<double>(k), -1.0 - 0.25 - 0.25 * (1.0 - tone)));
		}
		break;
	default: break;
	}
}

} // namespace feathers::drum
