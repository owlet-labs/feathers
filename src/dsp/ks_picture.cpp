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

#include "dsp/ks_picture.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace feathers::ks {

void heardSpectrum(const partials::PartialSpectrum& tone, const StringFields& material, int count,
                   partials::PartialSpectrum& out, const StringControls& controls, double maxRatio) {
	out = {};
	const int n = std::clamp(count, 0, partials::kMaxPartials);
	const StringLaws laws = lawsFor(material.brightness, material.sustain, material.pick, material.noise, material.stretch,
	                                controls, kPictureHz, false);
	const Tuning tuning = solveTuning(kPictureRate, kPictureHz, laws.dispersion);
	const double g = loopGainFor(kPictureRate, kPictureHz, laws.t60, laws.damping);
	const double period = kPictureRate / kPictureHz;
	const double w0 = 2.0 * std::numbers::pi / period;
	const auto t60At = [&](double w) {
		const double perTrip = g * loopFirMagnitude(laws.damping, w);
		return perTrip < 1.0 ? std::min(60.0, -3.0 * std::log(10.0) / (kPictureHz * std::log(perTrip))) : 60.0;
	};
	const double t60One = t60At(w0);
	// THE NOISE'S SPECTRUM at the default Hardness, its one-pole's magnitude, scaled to the tone's power below.
	const double h = std::clamp(static_cast<double>(controls.hardness), 0.0, 1.0);
	const double alpha = 0.05 + 0.95 * h * h;
	double tonePower = 0.0;
	double noisePower = 0.0;
	for (int i = 0; i < n; ++i) {
		const double a = static_cast<double>(tone.envAtRatio[static_cast<size_t>(i)]) * tone.fine[static_cast<size_t>(i)];
		tonePower += a * a;
		const double w = w0 * (i + 1);
		const double m = alpha / std::abs(1.0 - (1.0 - alpha) * std::polar(1.0, -w));
		noisePower += m * m;
	}
	const double noiseScale = noisePower > 0.0 ? std::sqrt(std::max(tonePower, 1.0e-12) / noisePower) : 0.0;
	const double noise = laws.noise;
	for (int i = 0; i < n; ++i) {
		const int k = i + 1;
		// THE LOOP'S PARTIAL k by fixed point on ω = 2πk / L(ω), from the harmonic: L moves slowly, so three steps land it.
		double w = w0 * k;
		for (int it = 0; it < 3; ++it) {
			w = 2.0 * std::numbers::pi * k / loopPhaseDelay(tuning, w);
		}
		if (!(w < std::numbers::pi) || !(w / w0 < maxRatio)) {
			out.ratio[static_cast<size_t>(i)] = static_cast<float>(k);
			continue;
		}
		const double a = static_cast<double>(tone.envAtRatio[static_cast<size_t>(i)]) * tone.fine[static_cast<size_t>(i)];
		const double nk = noiseScale * alpha / std::abs(1.0 - (1.0 - alpha) * std::polar(1.0, -w0 * k));
		const double pluck = (1.0 - noise) * a + noise * nk;
		const double pick = std::abs(1.0 - std::polar(1.0, -2.0 * std::numbers::pi * k * laws.pick)) / std::numbers::sqrt2;
		const double pickup =
		    std::abs(1.0 - std::polar(1.0, -2.0 * std::numbers::pi * k * controls.pickup)) / std::numbers::sqrt2;
		const double ring = std::sqrt(t60At(w) / t60One);
		out.envAtRatio[static_cast<size_t>(i)] = static_cast<float>(pluck * pick * pickup * ring);
		out.fine[static_cast<size_t>(i)] = 1.0f;
		out.ratio[static_cast<size_t>(i)] = static_cast<float>(w / w0);
	}
	partials::normaliseRss(out);
	// DIMMED BY THE FUNDAMENTAL'S RING: √(T60 / (T60 + 1 s)), so a string that rings for seconds reads near full and a thud
	// reads darker. The scale is applied to envAtRatio, the member cycleOf reads.
	const double dim = std::sqrt(t60One / (t60One + 1.0));
	for (float& v : out.envAtRatio) {
		v = static_cast<float>(v * dim);
	}
}

} // namespace feathers::ks
