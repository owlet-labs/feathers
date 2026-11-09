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

#include "dsp/puck_motion.h"

#include <cmath>

namespace feathers {

void PuckMotion::setSampleRate(double hostSampleRate) {
	const double rate = hostSampleRate > 0.0 ? hostSampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kTickSamples) / rate;
	retain_ = std::exp(-tickSeconds_ / kTimeConstantSeconds);
	reset();
}

void PuckMotion::reset() {
	velocity_ = {};
	lastTickPosition_ = position_;
	havePosition_ = false;
	samplesIntoTick_ = 0;
}

void PuckMotion::advance(int hostSamples) {
	if (hostSamples <= 0) {
		return;
	}
	samplesIntoTick_ += hostSamples;
	// Every whole tick the report crossed, in order. A caller that hands over a thousand samples at
	// once gets the same sequence of steps as one that hands over a hundred ten times, which is the
	// invariance this class exists for — and it is why the loop counts ticks rather than dividing.
	while (samplesIntoTick_ >= kTickSamples) {
		samplesIntoTick_ -= kTickSamples;
		step();
	}
}

void PuckMotion::step() {
	if (!havePosition_) {
		// The first tick has nothing to differentiate against. Taking the difference from a
		// value-initialised origin would read a puck placed away from the centre as an infinite lurch
		// towards it — the loudest possible sound at the quietest possible moment, when a patch loads.
		lastTickPosition_ = position_;
		havePosition_ = true;
		return;
	}

	// Both of these are exactly ±0.0 while the puck is parked, and a pole fed exact zeros from an exact
	// zero stays exactly zero. That is what makes "nothing happens at rest" a property of the
	// arithmetic rather than a threshold somebody has to pick.
	const double vx = (position_.x - lastTickPosition_.x) / tickSeconds_;
	const double vy = (position_.y - lastTickPosition_.y) / tickSeconds_;
	lastTickPosition_ = position_;

	velocity_.x = velocity_.x * retain_ + vx * (1.0 - retain_);
	velocity_.y = velocity_.y * retain_ + vy * (1.0 - retain_);

	// AND THE GESTURE IS ALLOWED TO END. Without this the decay never arrives: it walks into the
	// denormals and sticks 28 of them above zero, keeping every motion effect switched on for the rest
	// of the session and rebuilding the character on every tick for a minute — see kRestThreshold for
	// the measurement. Per axis, so a gesture along one leaves the other exactly where it already was.
	if (std::abs(velocity_.x) < kRestThreshold) {
		velocity_.x = 0.0;
	}
	if (std::abs(velocity_.y) < kRestThreshold) {
		velocity_.y = 0.0;
	}
}

} // namespace feathers
