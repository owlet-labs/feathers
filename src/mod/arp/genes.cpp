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

#include "mod/arp/genes.h"

#include "character/util.hpp"
#include "dsp/field_override.h"
#include "mod/arp/pattern_warp.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp::genes {

namespace {
/// THE FRACTION, in double to the wrap (backlog EH's lesson: a float base past 2^22 has no fraction left to move).
double frac(double x) noexcept {
	return x - std::floor(x);
}
} // namespace

float readRow(Row r, double phase) noexcept {
	const RowDef& d = kRows[at(r)];
	return owlet::dsp::triangleSimpleUnipolar(static_cast<float>(frac(phase + d.offset)), d.duty);
}

double angleOf(Row r, double phase) noexcept {
	return frac(phase + kRows[at(r)].offset);
}

float readRow(Row r, double phase, const Warp* warp, size_t site) noexcept {
	if (warp == nullptr) {
		return readRow(r, phase);
	}
	// THE SAME FLOAT ANGLE `readRow` READS, bent on its site's circle (a site the likes never reached reads it back as is).
	const auto theta = static_cast<float>(frac(phase + kRows[at(r)].offset));
	return owlet::dsp::triangleSimpleUnipolar(warp->at(site, theta), kRows[at(r)].duty);
}

Wide wideOf(const Place& place, const Pins& pins, const Warp* warp) noexcept {
	const auto read = [&](Row row) { return readRow(row, place.phase[at(row)], warp, wideSite(row)); };
	Wide w;
	for (size_t p = 0; p < kPinCount; ++p) {
		const Row row = pinRow(static_cast<Pin>(p));
		const float field = read(row);
		// OFF IS THE NEUTRAL, 0 for every pin here; ON, the house law — exactly the field at 0.5.
		w.pinned[p] = pins.on[p] ? fieldWithOverride(field, pins.amount[p]) : 0.0f;
	}
	w.direction = read(Row::WideDirection);
	w.registerCentre = read(Row::WideRegister);
	w.motif = read(Row::WideMotif);
	w.size = read(Row::WideSize);
	w.pitchLength = read(Row::WidePitchLength);
	w.phrase = read(Row::WidePhrase);
	return w;
}

double stepPhase(const Place& place, size_t row, size_t k, size_t length) noexcept {
	const double l = static_cast<double>(std::max<size_t>(length, 1));
	const double stride = kRows[row].stride + place.rate[row] / l;
	return place.phase[row] + static_cast<double>(k) * stride;
}

StepRaw stepOf(const Place& place, size_t k, size_t length, const Warp* warp) noexcept {
	StepRaw out {};
	const size_t site = std::min(k, kMaxSteps - 1);
	for (size_t i = 0; i < kStepRowCount; ++i) {
		out[i] = readRow(static_cast<Row>(i), stepPhase(place, i, k, length), warp, stepSite(i, site));
	}
	return out;
}

} // namespace feathers::mod::arp::genes
