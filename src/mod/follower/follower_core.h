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

#pragma once

#include "fx/automod/automod_envelope.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// THE ENVELOPE FOLLOWER'S CORE (backlog MY) — what both of its modes share: the level, its mapping, the threshold.
//
// **THE ONE-POLE IS AUTOMOD'S, NOT A SECOND ONE** (`fx::automod::EnvelopeFollower`, header-only): per sample,
// times in seconds, asymmetric attack and release, a non-finite sample read as silence. Its reasons are in
// `automod_envelope.h` and hold here unchanged. This file adds only what a modulator needs on top of it:
//
//   - **PEAK OR RMS.** Peak follows max(|L|, |R|). RMS follows the mean POWER, (L² + R²) / 2, through the same
//     one-pole and reads its square root, so a full-scale sine reads 1/√2 where Peak reads 1. The attack and
//     release are then time constants of the POWER, which is the usual RMS detector's meaning.
//   - **OR SIGNED (backlog OP)** — the control
//     detector, and the Control mode's default. NO RECTIFICATION: the mean of the two ears, (L + R) / 2, through a
//     one-pole of its own that takes the ATTACK time while the input is above it and the RELEASE time while below,
//     so it is a lag of a bipolar signal and an LFO's −1..1 stays −1..1. Automod's follower cannot do this — it
//     takes |x| by contract — so the signed one-pole is here, with its coefficient rule restated exactly.
//   - **GAIN AND FLOOR, AFTER THE ONE-POLE**, so moving either changes the output at once rather than through the
//     detector's memory: out = clamp((gain · level − floor) / (1 − floor), 0, 1). The floor is where the output
//     leaves zero; above it the output still reaches 1 at gain · level = 1. UNDER SIGNED THE FLOOR IS NOT
//     APPLIED (a floor on a bipolar signal has no zero to leave): out = clamp(gain · level, −1, 1).
//   - **A THRESHOLD WITH FIXED HYSTERESIS, ON THE OUTPUT, PER SAMPLE.** A FIRE when the output rises to the
//     threshold while armed; it RE-ARMS once the output falls below HALF the threshold (−6 dB), the same ratio as
//     the Trig convention's 0.5 / 0.25 (`mod/trig.h`). Detected per sample, so a crossing shorter than a tick is
//     not missed; the fire is HELD until the next modulation tick turns it into a one-tick pulse.
//
// NO ALLOCATION; a handful of floats.

namespace feathers::mod::follower {

/// THE RE-ARM LEVEL AS A FRACTION OF THE THRESHOLD — fixed, never a control (MV: no input gets a threshold
/// control of its own; this device IS the deliberate threshold, and its hysteresis is the convention's ratio).
inline constexpr float kRearmRatio = 0.5f;

/// WHAT THE DETECTOR MEASURES. Order is the `Detector` menu's order and its stored value (a patch stores the label).
enum class Detector : uint8_t { Peak = 0, Rms, Signed, kCount };

class Core {
public:
	void prepare(double sampleRate) noexcept {
		sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		env_.prepare(sampleRate);
		reset();
	}

	/// Silence, armed, nothing held. A reset is a fresh start, never a fire.
	void reset() noexcept {
		env_.reset();
		signed_ = 0.0f;
		armed_ = true;
		held_ = false;
		pulse_ = 0.0f;
		out_ = 0.0f;
	}

	/// THE SETTINGS, AS RESOLVED. Times in seconds (0 is no smoothing), gain and floor linear, threshold 0..1.
	void configure(float attackSeconds, float releaseSeconds, Detector detector, float gain, float floor,
	               float threshold) noexcept {
		env_.setTimes(attackSeconds, releaseSeconds);
		signedAttack_ = coefficientFor(attackSeconds);
		signedRelease_ = coefficientFor(releaseSeconds);
		detector_ = detector;
		gain_ = std::isfinite(gain) ? std::max(gain, 0.0f) : 1.0f;
		floor_ = std::isfinite(floor) ? std::clamp(floor, 0.0f, 0.999f) : 0.0f;
		threshold_ = std::isfinite(threshold) ? std::clamp(threshold, 0.0f, 1.0f) : 0.5f;
		out_ = map(level());
	}

	/// ONE SAMPLE, BOTH EARS. The level moves, the output is mapped, the edge is watched.
	void push(float left, float right) noexcept {
		if (detector_ == Detector::Signed) {
			const float l = std::isfinite(left) ? left : 0.0f;
			const float r = std::isfinite(right) ? right : 0.0f;
			const float in = 0.5f * (l + r);
			// RISING TAKES THE ATTACK, FALLING THE RELEASE — the asymmetry, without the magnitude.
			signed_ += (in - signed_) * (in > signed_ ? signedAttack_ : signedRelease_);
		}
		else if (detector_ == Detector::Rms) {
			env_.process(0.5f * (left * left + right * right), 0.0f);
		}
		else {
			const float l = std::isfinite(left) ? std::fabs(left) : 0.0f;
			const float r = std::isfinite(right) ? std::fabs(right) : 0.0f;
			env_.process(std::max(l, r), 0.0f);
		}
		out_ = map(level());
		watch(out_);
	}

	/// THE MODULATION TICK: a fire held since the last tick becomes this tick's pulse, and the pulse lasts one tick.
	/// Returns whether it fired.
	bool tick() noexcept {
		const bool fired = held_;
		held_ = false;
		pulse_ = fired ? 1.0f : 0.0f;
		return fired;
	}

	/// THE DETECTOR'S LEVEL, before gain and floor: the peak follower's, the square root of the power's, or the
	/// signed one-pole's (−1..1 and beyond).
	[[nodiscard]] float level() const noexcept {
		if (detector_ == Detector::Signed) {
			return signed_;
		}
		const float l = env_.level(fx::automod::Channel::Left);
		return detector_ == Detector::Rms ? std::sqrt(std::max(l, 0.0f)) : l;
	}
	[[nodiscard]] Detector detector() const noexcept { return detector_; }
	/// THE ENVELOPE OUTPUT, 0..1 — gain and floor applied; −1..1 under Signed, gain applied.
	[[nodiscard]] float envelope() const noexcept { return out_; }
	/// THE TRIG OUTPUT — 1 on the tick after a crossing, 0 otherwise.
	[[nodiscard]] float pulse() const noexcept { return pulse_; }
	[[nodiscard]] float threshold() const noexcept { return threshold_; }
	[[nodiscard]] bool armed() const noexcept { return armed_; }

	/// gain · level, floored and normalised, clamped to 0..1. See the header.
	[[nodiscard]] float map(float level) const noexcept {
		if (detector_ == Detector::Signed) {
			const float s = gain_ * level;
			return std::isfinite(s) ? std::clamp(s, -1.0f, 1.0f) : 0.0f;
		}
		const float v = (gain_ * level - floor_) / (1.0f - floor_);
		return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
	}

private:
	/// `fx::automod::EnvelopeFollower`'s coefficient rule, restated for the signed one-pole: below its quantum the
	/// follower tracks its input exactly.
	[[nodiscard]] float coefficientFor(float seconds) const noexcept {
		if (!std::isfinite(seconds) || seconds < fx::automod::kEnvMinSeconds) {
			return 1.0f;
		}
		return 1.0f - std::exp(-1.0f / (seconds * static_cast<float>(sampleRate_)));
	}

	void watch(float out) noexcept {
		if (armed_ && out >= threshold_) {
			armed_ = false;
			held_ = true;
			return;
		}
		if (!armed_ && out < threshold_ * kRearmRatio) {
			armed_ = true;
		}
	}

	fx::automod::EnvelopeFollower env_;
	double sampleRate_ = 48000.0;
	float signed_ = 0.0f;
	float signedAttack_ = 1.0f;
	float signedRelease_ = 1.0f;
	Detector detector_ = Detector::Peak;
	float gain_ = 1.0f;
	float floor_ = 0.0f;
	float threshold_ = 0.5f;
	float out_ = 0.0f;
	float pulse_ = 0.0f;
	bool armed_ = true;
	bool held_ = false;
};

} // namespace feathers::mod::follower
