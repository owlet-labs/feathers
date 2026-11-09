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

#include "dsp/vocode_bands.h"

#include "character/phi_triangle.hpp"

#include <algorithm>
#include <cmath>

namespace feathers::spectral {

FreeAxes freeAxes(double x) noexcept {
	// **THE FREE COORDINATE THROUGH PHI TRIANGLES** (design §10.7.2, Z4; the Phaser's slicing, phaser_phase_space.h): each named
	// axis reads a unipolar triangle (0 -> 1 -> 0, `triangleSimpleUnipolar`) at its own rate, half a power of phi apart, so the
	// coordinate walks through combinations no two of which repeat. At 0 every axis is 0: Even. Every axis peaks within the
	// coordinate's range (the slowest at 0.62). Harmonic is bipolar: its triangle's first half pulls toward the harmonics, its
	// second toward the gaps.
	const auto tri = [](double phase) { return static_cast<double>(owlet::dsp::triangleSimpleUnipolar(owlet::dsp::phi::wrapPhase(phase), 1.0f)); };
	FreeAxes a;
	a.speech = tri(kFreeRates[0] * x);
	a.low = tri(kFreeRates[1] * x);
	a.high = tri(kFreeRates[2] * x);
	const double ph = kFreeRates[3] * x;
	const double wrapped = ph - std::floor(ph);
	a.harmonic = wrapped < 0.5 ? tri(2.0 * wrapped) : -tri(2.0 * wrapped - 1.0);
	return a;
}

WidthAxes widthAxes(double x) noexcept {
	// THE WIDTH ZONE'S OWN FREE COORDINATE (by design separate from the spacing's): two bipolar phi triangles at 0.5 x phi^1.5 and
	// 0.5 x phi^2.5, each cycle's first half positive, its second negative; 0 is even widths.
	const auto bipolar = [](double ph) {
		const double w = ph - std::floor(ph);
		const auto tri = [](double q) { return static_cast<double>(owlet::dsp::triangleSimpleUnipolar(owlet::dsp::phi::wrapPhase(q), 1.0f)); };
		return w < 0.5 ? tri(2.0 * w) : -tri(2.0 * w - 1.0);
	};
	return WidthAxes { bipolar(kWidthRates[0] * x), bipolar(kWidthRates[1] * x) };
}

bool layoutHarmonic(const BandSpec& spec, bool* inharmonic) noexcept {
	const int zone = std::clamp(spec.zone, 0, 6);
	const FreeAxes free = zone == 6 ? freeAxes(spec.zoneDrive) : FreeAxes {};
	const bool harmonic = zone == 4 || zone == 5 || (zone == 6 && free.harmonic != 0.0);
	if (inharmonic != nullptr) {
		*inharmonic = zone == 5 || (zone == 6 && free.harmonic < 0.0);
	}
	return harmonic;
}

bool BandGrouping::regroup(int count, double low, double high, bool inharmonic, double f0In) noexcept {
	// RE-LAID ONLY WHEN SOMETHING THE GROUPING DEPENDS ON MOVED: the count, the Range, the side, or f0 by more than a cent.
	const double f0 = std::max(f0In, 1.0);
	count = std::clamp(count, 1, kMaxVocodeBands - 1);
	const bool same = bands_ == count && low_ == low && high_ == high && inharmonic_ == inharmonic && f0_ > 0.0
	                  && std::fabs(1200.0 * std::log2(f0 / f0_)) <= 1.0;
	if (same) {
		return false;
	}
	bands_ = count;
	low_ = low;
	high_ = high;
	inharmonic_ = inharmonic;
	f0_ = f0;
	// THE MEMBERS: harmonic k (k f0, k >= 1) or the gap above it ((k + 0.5) f0), inside the Range — none below f0.
	const double offset = inharmonic ? 1.5 : 1.0;
	firstMember_ = std::max(0, static_cast<int>(std::ceil(low / f0 - offset)));
	const int lastMember = static_cast<int>(std::floor(high / f0 - offset));
	members_ = std::max(0, lastMember - firstMember_ + 1);
	const int members = members_;
	grouped_ = members >= count;
	if (grouped_) {
		// GROUPS: their edges even in log2 of the member number, so the bottom members get a band each and the groups widen above;
		// every group holds at least one member and leaves enough for the rest.
		int edge = 0;
		for (int b = 0; b < count; ++b) {
			const double target = std::exp2(std::log2(static_cast<double>(members) + 1.0) * static_cast<double>(b + 1) / count) - 1.0;
			int next = static_cast<int>(std::lround(target));
			next = std::clamp(next, edge + 1, members - (count - b - 1));
			first_[static_cast<size_t>(b)] = edge;
			last_[static_cast<size_t>(b)] = next - 1;
			// UNTIL THE CARRIER IS SEEN, the group's middle member.
			pick_[static_cast<size_t>(b)] = (edge + next - 1) / 2;
			target_[static_cast<size_t>(b)] = std::log2(memberHz(pick_[static_cast<size_t>(b)]));
			edge = next;
		}
		return true;
	}
	// FEWER MEMBERS THAN BANDS (a high note): every member a band, and the surplus in the OTHER kind's positions (the gaps for
	// Harmonic, the harmonics for Inharmonic), lowest first, inside the Range; what still remains, even per octave. Sorted.
	int n = 0;
	for (int m = 0; m < members; ++m) {
		first_[static_cast<size_t>(n)] = m;
		last_[static_cast<size_t>(n)] = m;
		target_[static_cast<size_t>(n++)] = std::log2(memberHz(m));
	}
	const double other = inharmonic ? 1.0 : 1.5;
	for (int k = std::max(0, static_cast<int>(std::ceil(low / f0 - other))); n < count && (k + other) * f0 <= high; ++k) {
		first_[static_cast<size_t>(n)] = -1;
		last_[static_cast<size_t>(n)] = -1;
		target_[static_cast<size_t>(n++)] = std::log2((k + other) * f0);
	}
	for (int b = 0; n < count; ++b) {
		first_[static_cast<size_t>(n)] = -1;
		last_[static_cast<size_t>(n)] = -1;
		target_[static_cast<size_t>(n++)] = std::log2(low) + (std::log2(high) - std::log2(low)) * (b + 0.5) / count;
	}
	std::sort(target_.begin(), target_.begin() + count);
	for (int b = 1; b < count; ++b) { // strictly rising, so no two bands share a centre
		target_[static_cast<size_t>(b)] = std::max(target_[static_cast<size_t>(b)], target_[static_cast<size_t>(b - 1)] + 1e-4);
	}
	return true;
}

bool BandGrouping::setPick(int b, int m) noexcept {
	const auto u = static_cast<size_t>(b);
	if (!grouped_ || b < 0 || b >= bands_ || m == pick_[u]) {
		return false;
	}
	pick_[u] = m;
	target_[u] = std::log2(memberHz(m));
	return true;
}

double widthFactor(const BandSpec& spec, double centreLog2, int b, int count) noexcept {
	// **THE BANDWIDTH ZONE** (design §10.7.2, Z5): how each band's width departs from Bandwidth's share of its local spacing, as
	// a power of two — up to 1.5 octaves of width ratio either way at full amount, so Bandwidth keeps meaning the same thing and
	// the zone only redistributes it. 0 Even; 1 Narrow Low (narrow below, wide above); 2 Narrow High (the mirror); 3 Harmonic
	// (narrow on f0's harmonics, wide between); 4 Inharmonic (the mirror); 5 Free (its own coordinate through two phi-triangle
	// axes, a tilt and a harmonic bias, both bipolar). The partition of unity is per bin, so uneven widths keep it.
	const int zone = std::clamp(spec.widthZone, 0, 5);
	const double a = spec.widthDrive; // a named mode's fenced amount, or Free's unbounded coordinate (Z7)
	if (zone == 0 || a == 0.0) {
		return 1.0;
	}
	const double p = (static_cast<double>(b) + 0.5) / static_cast<double>(count);
	const double tilt = 2.0 * p - 1.0; // -1 at the bottom band, +1 at the top
	const double hz = std::exp2(centreLog2);
	const double k = hz / std::max(spec.f0, 1.0);
	const double d = std::fabs(k - std::round(k)); // 0 on a harmonic, 0.5 halfway between
	const double between = 4.0 * d - 1.0;          // -1 on a harmonic, +1 between
	double e = 0.0;
	switch (zone) {
	case 1: e = a * tilt; break;
	case 2: e = -a * tilt; break;
	case 3: e = a * between; break;
	case 4: e = -a * between; break;
	case 5: {
		const WidthAxes w = widthAxes(a);
		e = w.tilt * tilt + w.harmonic * between;
		break;
	}
	default: break;
	}
	return std::exp2(1.5 * e);
}

int layoutBands(const BandSpec& spec, const BandGrouping* grouping, double* centre, double* sigma) noexcept {
	// THE LAYOUT (design §10.7.2): `count` bands, their centres placed between Range Low and High by the Zone (Z2: Even, Speech, Low,
	// High, each morphed from Even by its amount), each a Gaussian in log2 Hz of width sigma_b = (0.15 + 0.85 · Bandwidth) of its
	// LOCAL spacing, so Bandwidth means the same in every Zone.
	const int count = std::clamp(spec.count, 1, kMaxVocodeBands - 1);
	const double low = spec.low;
	const double high = spec.high;
	const double bw = spec.bandwidth;
	const int zone = std::clamp(spec.zone, 0, 6);
	const double amount = spec.zoneDrive;
	const FreeAxes free = zone == 6 ? freeAxes(amount) : FreeAxes {};
	const auto groupTarget = [&](int b, double even) { return grouping != nullptr ? grouping->target(b) : even; };
	const double lowLog = std::log2(low);
	const double highLog = std::log2(high);
	const double span = highLog - lowLog;
	// THE CENTRES, by band position p in (0, 1).
	const auto bark = [](double hz) { return 26.81 * hz / (1960.0 + hz) - 0.53; }; // Traunmüller
	const auto fromBark = [](double z) { return 1960.0 * (z + 0.53) / (26.28 - z); };
	const double zl = bark(low);
	const double zh = bark(high);
	const double k = 1.0 + 3.0 * amount; // Low and High: the density's exponent, 1 at amount 0 (Even)
	for (int b = 0; b < count; ++b) {
		const double p = (static_cast<double>(b) + 0.5) / static_cast<double>(count);
		const double even = lowLog + span * p;
		double c = even;
		switch (zone) {
		case 1: c = (1.0 - amount) * even + amount * std::log2(fromBark(zl + (zh - zl) * p)); break;
		case 2: c = lowLog + span * std::pow(p, k); break;
		case 3: c = lowLog + span * (1.0 - std::pow(1.0 - p, k)); break;
		case 4:
		case 5: c = (1.0 - amount) * even + amount * groupTarget(b, even); break;
		case 6: {
			// FREE: a mix of the named modes' FULL layouts by the axes' amounts, as offsets from Even, divided by their sum once
			// it passes 1 — a convex mix of rising sequences, so the centres stay inside the Range and in order.
			const double speech = std::log2(fromBark(zl + (zh - zl) * p));
			const double lowFull = lowLog + span * std::pow(p, 4.0);
			const double highFull = lowLog + span * (1.0 - std::pow(1.0 - p, 4.0));
			const double h = std::fabs(free.harmonic);
			const double off = free.speech * (speech - even) + free.low * (lowFull - even) + free.high * (highFull - even)
			                   + h * (groupTarget(b, even) - even);
			c = even + off / std::max(1.0, free.speech + free.low + free.high + h);
			break;
		}
		default: break;
		}
		centre[b] = c;
	}
	for (int b = 0; b < count; ++b) {
		const auto u = static_cast<size_t>(b);
		const double left = b > 0 ? centre[u] - centre[u - 1] : 2.0 * (centre[u] - lowLog);
		const double right = b + 1 < count ? centre[u + 1] - centre[u] : 2.0 * (highLog - centre[u]);
		sigma[u] = (0.15 + 0.85 * bw) * std::max(0.5 * (left + right), 1e-4) * widthFactor(spec, centre[u], b, count);
	}
	return count;
}

} // namespace feathers::spectral
