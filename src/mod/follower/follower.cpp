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

#include "mod/follower/follower.h"

#include "mod/modulator.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace feathers::mod::follower {

void Follower::prepare(double sampleRate) noexcept {
	core_.prepare(sampleRate);
}

void Follower::reset() noexcept {
	core_.reset();
}

void Follower::setFields(const float* fields, size_t count) noexcept {
	settings_ = settingsOf(fields, count);
	core_.configure(secondsOf(settings_.attackLog2, kAttackLog2Min), secondsOf(settings_.releaseLog2, kReleaseLog2Min),
	                settings_.running(), gainOf(settings_.gainDb), floorOf(settings_.floorDb), settings_.threshold);
}

void Follower::hear(const float* left, const float* right, int frames) noexcept {
	if (left == nullptr || settings_.mode != Mode::Audio) {
		return;
	}
	const float* r = right != nullptr ? right : left;
	for (int i = 0; i < frames; ++i) {
		core_.push(left[i], r[i]);
	}
}

void Follower::tick(size_t samples) noexcept {
	// IN CONTROL, THE RESOLVED INPUT for this tick's samples (the only thing it follows); in Audio the audio road has
	// already pushed what passed.
	if (settings_.mode == Mode::Control) {
		for (size_t i = 0; i < samples; ++i) {
			core_.push(settings_.input, settings_.input);
		}
	}
	if (core_.tick()) {
		++fires_;
	}
}

float Follower::outputAt(size_t n) const noexcept {
	switch (static_cast<Output>(n)) {
	case Output::Env: return core_.envelope();
	case Output::Trig: return core_.pulse();
	default: return 0.0f;
	}
}

float Follower::drawnLevel() const noexcept {
	const float env = core_.envelope();
	return core_.detector() == Detector::Signed ? 0.5f * (env + 1.0f) : env;
}

namespace {
Follower& asFollower(void* storage) noexcept { return *static_cast<Follower*>(storage); }
const Follower& asFollower(const void* storage) noexcept { return *static_cast<const Follower*>(storage); }
} // namespace

/// **THE AUDIO ROAD HEARS AND WRITES NOTHING** (`listensOnly`): both roads hand it the buffer as it passes, the
/// voice's mono one included, and it leaves the buffer as it came. In Control it does not even listen.
const device::AudioOps kAudioOps {
	.stateBytes = sizeof(Follower),
	.stateAlign = alignof(Follower),
	.construct = [](void* storage) { new (storage) Follower(); },
	.destruct = [](void* storage) { asFollower(storage).~Follower(); },
	.prepare = [](void* storage, double sampleRate, int) { asFollower(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asFollower(storage).reset(); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asFollower(storage).setFields(fields, fieldsPresent);
	    },
	.processStereo =
	    [](void* storage, float* left, float* right, int frames) { asFollower(storage).hear(left, right, frames); },
	.processMono = [](void* storage, float* mono, int frames) { asFollower(storage).hear(mono, nullptr, frames); },
	.listensOnly = true,
};

const device::ModOps kModOps {
	.stateBytes = sizeof(Follower),
	.stateAlign = alignof(Follower),
	.construct = [](void* storage) { new (storage) Follower(); },
	.destruct = [](void* storage) { asFollower(storage).~Follower(); },
	.prepare = [](void* storage, double sampleRate) { asFollower(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asFollower(storage).reset(); },
	.setFields =
	    [](void* storage, const float* fields, size_t fieldsPresent) {
		    asFollower(storage).setFields(fields, fieldsPresent);
	    },
	// THE MODULATORS' GRID (Feathers' control tick), so a fire is published on the tick after its crossing.
	.blockSamples = static_cast<size_t>(mod::kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext&) {
		asFollower(storage).tick(static_cast<size_t>(mod::kBlockSamples));
	},
	.outputAt = [](const void* storage, size_t output) { return asFollower(storage).outputAt(output); },
	// NOT A SHAPE BEING RUN THROUGH, but the card's picture is the LEVEL, and the host carries a level only beside a
	// playhead (`ModOps::playheadLevel`): one stage, always running.
	.playhead =
	    [](const void*, uint32_t* ordinal, float* phase) {
		    *ordinal = 0;
		    *phase = 0.0f;
	    },
	.playheadLevel = [](const void* storage) { return asFollower(storage).drawnLevel(); },
	// THE ROW DECIDES (backlog OP): Control reads no chain audio and may reach every destination; Audio's outputs are
	// computed from the audio through it, and the graph lets them reach only what is downstream.
	.readsAudio = false,
	.readsAudioOf = &readsAudioOf,
	.trigFires = [](const void* storage) { return asFollower(storage).fires(); },
};

} // namespace feathers::mod::follower
