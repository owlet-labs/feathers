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

#include "fx/busutil/busutil_types.h"

#include "fx/dyn/clip_curve.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::busutil {

namespace {

/// WHERE A CONTROL'S VALUE COMES FROM WHEN THE ROW DOES NOT CARRY IT. device_row.h's rule: an absent
/// field is at its DECLARED DEFAULT, never at zero — which for Pan L is -1 and not 0, and for Width
/// is 1 and not 0. **A row read as zeros would mono a signal and collapse both channels to the
/// centre**, so this device is one where the difference between "absent" and "zero" is audible
/// rather than academic.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

/// `a . b`, in the destination-then-source naming `Matrix` declares.
[[nodiscard]] Matrix mul(const Matrix& a, const Matrix& b) {
	return Matrix {
		.ll = a.ll * b.ll + a.lr * b.rl,
		.lr = a.ll * b.lr + a.lr * b.rr,
		.rl = a.rl * b.ll + a.rr * b.rl,
		.rr = a.rl * b.lr + a.rr * b.rr,
	};
}

} // namespace

Placement placeMono(float position) {
	const float p = std::clamp(position, -1.0f, 1.0f);
	// **THE ENDPOINTS ARE EXACT BY CONSTRUCTION, NOT BY TRIGONOMETRY, AND THE IDENTITY CLAIM RESTS ON
	// IT.** `std::cos(pi/2)` is 6.1e-17, not zero — so a dual pan at its defaults built purely from
	// cos/sin would leak the right channel into the left at -324 dB and make `Settings{}` a matrix
	// that is *nearly* the identity. "Nearly" is the difference between a sample-exact test and a
	// tolerance nobody can justify, and the tolerance is the thing that later hides a real defect.
	if (p <= -1.0f) {
		return Placement { .left = 1.0f, .right = 0.0f };
	}
	if (p >= 1.0f) {
		return Placement { .left = 0.0f, .right = 1.0f };
	}
	// CONSTANT POWER: equal apparent loudness across the field, 0.7071 in both at the centre. See the
	// header for the 3 dB centre boost this gives correlated material, which is inherent rather than
	// a defect.
	const double theta = (static_cast<double>(p) + 1.0) * (std::numbers::pi_v<double> / 4.0);
	return Placement { .left = static_cast<float>(std::cos(theta)),
		               .right = static_cast<float>(std::sin(theta)) };
}

Balance panBalance(float pan) {
	const float p = std::clamp(pan, -1.0f, 1.0f);
	// UNITY AT CENTRE — a different law from `placeMono` on purpose. The header says why at length;
	// the short version is that this one acts on a PAIR that is already placed, and a pan knob that
	// attenuates a bus it has not been moved on is a pan knob nobody trusts.
	return Balance { .left = p <= 0.0f ? 1.0f : 1.0f - p,
		             .right = p >= 0.0f ? 1.0f : 1.0f + p };
}

float clipSample(float x, float knee) {
	// THE CURVE LIVES IN `fx/dyn/clip_curve.h` now, shared with the dynamics devices; its rationale went with it.
	return dyn::softClip(x, knee);
}

Matrix matrixOf(const Settings& s) {
	if (!s.enabled) {
		// THE IDENTITY, NOT A SKIP. The caller ramps toward whatever this returns, so bypass arrives
		// over a block instead of as a step — and a step in a gain coefficient is a click.
		return Matrix {};
	}

	// I — POLARITY. Two independent switches; see `Param::InvertL` for why one combined switch would
	// be a control nobody can hear.
	const Matrix invert { .ll = s.invertL ? -1.0f : 1.0f,
		                  .lr = 0.0f,
		                  .rl = 0.0f,
		                  .rr = s.invertR ? -1.0f : 1.0f };

	// W — WIDTH, as mid/side scale. `M = (L+R)/2`, `S = (L-R)/2`, `out = M +- w*S`, which collects
	// into the matrix below. At w == 1 it is EXACTLY the identity ((1+1)/2 = 1, (1-1)/2 = 0), which
	// is the second half of the sample-exact default.
	const float w = std::clamp(s.width, 0.0f, 2.0f);
	const float sum = (1.0f + w) * 0.5f;
	const float dif = (1.0f - w) * 0.5f;
	const Matrix width { .ll = sum, .lr = dif, .rl = dif, .rr = sum };

	// D — DUAL PAN. Each INPUT CHANNEL is a mono source placed at its own position, so the left
	// input's placement fills the left COLUMN and the right input's fills the right one. Getting this
	// transposed is the bug `Matrix`'s naming note exists to prevent: it sounds like swapped channels
	// and reads like nothing.
	const Placement left = placeMono(s.panL);
	const Placement right = placeMono(s.panR);
	const Matrix dual { .ll = left.left, .lr = right.left, .rl = left.right, .rr = right.right };

	// P — PAN, a diagonal scale of the pair.
	const Balance bal = panBalance(s.pan);
	const Matrix pan { .ll = bal.left, .lr = 0.0f, .rl = 0.0f, .rr = bal.right };

	// G — THE FADER. `(2 * 0.5)^2` is exactly 1.0, so the centre of the travel is unity to the bit —
	// which is what the header's static_assert pins and what every sample-exact claim rests on.
	const float g = faderGain(s.gain);
	const Matrix gain { .ll = g, .lr = 0.0f, .rl = 0.0f, .rr = g };

	// THE DM STAGES (2026-10-30) — swap, the mid/side trims, monitor — are each composed ONLY WHEN OFF THEIR DEFAULT.
	// Multiplying by an identity is not exact in every case (a 0 times a negative entry is -0, and -0 + +0 is +0), so
	// skipping them is what keeps a device at its defaults the expression it was before they existed, to the bit.
	Matrix front = mul(width, invert);
	// S — SWAP, FIRST: the input pair's columns exchanged, so every later control names the channel now on its side.
	if (s.swap) {
		front = mul(front, Matrix { .ll = 0.0f, .lr = 1.0f, .rl = 1.0f, .rr = 0.0f });
	}
	// T — THE MID/SIDE TRIMS, after Width: `out = m*M +- s*S`, the general case of which Width is the symmetric one.
	if (s.midTrim != 0.0f || s.sideTrim != 0.0f) {
		const float m = std::pow(10.0f, std::clamp(s.midTrim, static_cast<float>(-kTrimDb), static_cast<float>(kTrimDb)) / 20.0f);
		const float sd = std::pow(10.0f, std::clamp(s.sideTrim, static_cast<float>(-kTrimDb), static_cast<float>(kTrimDb)) / 20.0f);
		const float a = (m + sd) * 0.5f;
		const float b = (m - sd) * 0.5f;
		front = mul(Matrix { .ll = a, .lr = b, .rl = b, .rr = a }, front);
	}

	// **THE ORDER, IN ONE LINE, WHICH IS THE REASON THIS DEVICE IS A MATRIX AND NOT FIVE STAGES.**
	// Read right to left as signal flow: (swap), polarity, width, (trims), placement, balance, trim, (monitor).
	Matrix out = mul(gain, mul(pan, mul(dual, front)));

	// M — MONITOR, LAST: one signal to both outputs, or the stereo pair untouched.
	switch (static_cast<Monitor>(s.monitor)) {
	case Monitor::Stereo:
		break;
	case Monitor::Mono:
		out = mul(Matrix { .ll = 0.5f, .lr = 0.5f, .rl = 0.5f, .rr = 0.5f }, out);
		break;
	case Monitor::Left:
		out = mul(Matrix { .ll = 1.0f, .lr = 0.0f, .rl = 1.0f, .rr = 0.0f }, out);
		break;
	case Monitor::Right:
		out = mul(Matrix { .ll = 0.0f, .lr = 1.0f, .rl = 0.0f, .rr = 1.0f }, out);
		break;
	case Monitor::Side:
		out = mul(Matrix { .ll = 0.5f, .lr = -0.5f, .rl = 0.5f, .rr = -0.5f }, out);
		break;
	}
	return out;
}

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.gain = std::clamp(fieldOr(row, fieldsPresent, Param::Gain), 0.0f, 1.0f);
	s.pan = std::clamp(fieldOr(row, fieldsPresent, Param::Pan), -1.0f, 1.0f);
	s.panL = std::clamp(fieldOr(row, fieldsPresent, Param::PanL), -1.0f, 1.0f);
	s.panR = std::clamp(fieldOr(row, fieldsPresent, Param::PanR), -1.0f, 1.0f);
	s.width = std::clamp(fieldOr(row, fieldsPresent, Param::Width), 0.0f, 2.0f);
	s.invertL = fieldOr(row, fieldsPresent, Param::InvertL) >= 0.5f;
	s.invertR = fieldOr(row, fieldsPresent, Param::InvertR) >= 0.5f;
	// CLAMPED INTO THE LADDER, so a row from a build with more steps lands on a real one rather than
	// indexing past the table.
	s.clip = std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, Param::Clip))), 0,
	                    static_cast<int>(std::size(kClipNames)) - 1);
	s.highpass = std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, Param::Highpass))),
	                        0, static_cast<int>(std::size(kHighpassNames)) - 1);
	s.midTrim = std::clamp(fieldOr(row, fieldsPresent, Param::MidTrim), static_cast<float>(-kTrimDb),
	                       static_cast<float>(kTrimDb));
	s.sideTrim = std::clamp(fieldOr(row, fieldsPresent, Param::SideTrim), static_cast<float>(-kTrimDb),
	                        static_cast<float>(kTrimDb));
	s.monitor = std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, Param::Monitor))), 0,
	                       static_cast<int>(std::size(kMonitorNames)) - 1);
	s.swap = fieldOr(row, fieldsPresent, Param::Swap) >= 0.5f;
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Gain)] = s.gain;
	row[static_cast<size_t>(Param::Pan)] = s.pan;
	row[static_cast<size_t>(Param::PanL)] = s.panL;
	row[static_cast<size_t>(Param::PanR)] = s.panR;
	row[static_cast<size_t>(Param::Width)] = s.width;
	row[static_cast<size_t>(Param::InvertL)] = s.invertL ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::InvertR)] = s.invertR ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Highpass)] = static_cast<float>(s.highpass);
	row[static_cast<size_t>(Param::Clip)] = static_cast<float>(s.clip);
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = static_cast<float>(kFaderUnity);
	row[static_cast<size_t>(Param::MidTrim)] = s.midTrim;
	row[static_cast<size_t>(Param::SideTrim)] = s.sideTrim;
	row[static_cast<size_t>(Param::Monitor)] = static_cast<float>(s.monitor);
	row[static_cast<size_t>(Param::Swap)] = s.swap ? 1.0f : 0.0f;
	return row;
}

} // namespace feathers::fx::busutil
