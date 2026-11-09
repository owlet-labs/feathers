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

#include "mod/arp/arp.h"

#include "mod/arp/humanize.h"

#include <algorithm>
#include <array>
#include <new>

namespace feathers::mod::arp {

void Arp::setFields(const float* fields, size_t count) noexcept {
	s_ = settingsOf(fields, count);
	ClockSettings c;
	c.enabled = s_.enabled;
	c.latch = s_.latch;
	c.run = s_.run;
	c.synced = s_.synced();
	c.stepBeats = stepBeats(s_);
	c.stepHz = stepHz(s_);
	c.grid = StepGrid { static_cast<double>(s_.nudge), 0.0 };
	// THE FEEL MOVES SLOTS THROUGH THE GRID, by the Pattern device's own function.
	if (s_.feel > 0 || s_.wobble != 0.0) { // (Wobble moves slots under every feel, Straight included)
		c.grid.shift = &Arp::shiftOf;
		c.grid.shiftCtx = this;
	}
	em_.setSettings(c);
}

humanize::Mode Arp::feelModeOf(size_t feel) noexcept {
	// THE FEEL LIST IS SKLITTER'S TEN, which are Humanize's first ten in the same order.
	static_assert(kFeelCount == humanize::kGrooveFeels);
	return static_cast<humanize::Mode>(std::min(feel, kFeelCount - 1));
}

double Arp::shiftOf(const void* self, int64_t g) noexcept {
	const Settings& s = static_cast<const Arp*>(self)->s_;
	// THE BAR'S FEELS READ BEATS, NOT SECONDS: a synced step is measured in beats; under Hz a bar is read as sixteen
	// steps (`humanize::shiftSteps`). The seconds argument is unused by every feel offered here.
	const double beats = s.synced() ? stepBeats(s) : 0.0;
	return humanize::shiftSteps(feelOf(s.feel, static_cast<double>(s.feelAmount), s.freePhase, s.wobble), g, 0.125, beats);
}

void Arp::playSlot(void* self, int64_t g, StepEmitter& e) noexcept {
	Arp& a = *static_cast<Arp*>(self);
	const Settings& s = a.s_;
	std::array<PatternNote, kMaxPattern> pattern {};
	const size_t len = buildPattern(e.chord(), s.order, s.octaves, s.octaveMode, pattern.data());
	if (len == 0) {
		return;
	}
	const int64_t step = g - static_cast<int64_t>(s.offset);
	const size_t entry = entryAt(s.order, step, len, s.seed);
	const PatternNote& p = pattern[entry];
	const double on = e.onset(g);
	const double length = e.onset(g + 1) - on;
	// WOBBLE (design §12-ter) clips a note and ghosts or rings it — never a gate over 100% (a tie into the next step).
	const double beats = s.synced() ? stepBeats(s) : 0.0;
	const humanize::Feel feel = feelOf(s.feel, static_cast<double>(s.feelAmount), s.freePhase, s.wobble);
	const double gate = static_cast<double>(s.gate);
	const double clipped = gate <= 1.0 ? gate * humanize::lengthScale(feel, g, beats) : gate;
	const double off = std::max(on + clipped * length, on + e.sampleSteps());
	const double played = s.fixedVelocity ? static_cast<double>(s.velocity) : static_cast<double>(p.velocity);
	const double velocity = s.wobble != 0.0 ? std::clamp(played * humanize::velocityScale(feel, g, beats), 0.0, 1.0) : played;
	// A GATE OVER 100% INTO THE SAME KEY HOLDS ONE NOTE: the note still sounding runs on to this
	// step's end — no off, no new on. A different key still overlaps.
	const bool held = e.extend(p.key, p.channel, off);
	if (held || e.sound(p.key, p.channel, velocity, on, off)) {
		a.stepOut_ = len > 1 ? static_cast<float>(entry) / static_cast<float>(len - 1) : 0.0f;
		a.pitchOut_ = static_cast<float>(p.key) / 127.0f;
		a.velocityOut_ = static_cast<float>(velocity);
		e.noteRecent(static_cast<uint32_t>(g), a.pitchOut_);
	}
}

float Arp::outputAt(size_t output) const noexcept {
	switch (output) {
	case kStepOutput: return stepOut_;
	case kPitchOutput: return pitchOut_;
	case kVelocityOutput: return velocityOut_;
	default: return 0.0f;
	}
}

size_t Arp::patternForTest(PatternNote* out) const noexcept {
	return buildPattern(em_.chord(), s_.order, s_.octaves, s_.octaveMode, out);
}

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	return index == 0 ? static_cast<const Arp*>(storage)->readPlay(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 0 && static_cast<Arp*>(storage)->restorePlay(in, n);
}

//------------------------------------------------------------------------------------------------//
// The tables                                                                                     //
//------------------------------------------------------------------------------------------------//

namespace {
Arp& asArp(void* storage) noexcept { return *static_cast<Arp*>(storage); }
const Arp& asArp(const void* storage) noexcept { return *static_cast<const Arp*>(storage); }
void setFieldsOf(void* storage, const float* fields, size_t count) { asArp(storage).setFields(fields, count); }
} // namespace

const device::NoteOps kNoteOps {
	.stateBytes = sizeof(Arp),
	.stateAlign = alignof(Arp),
	.construct = [](void* storage) { new (storage) Arp(); },
	.destruct = [](void* storage) { asArp(storage).~Arp(); },
	.prepare = [](void* storage, double sampleRate) { asArp(storage).prepare(sampleRate); },
	.setFields = setFieldsOf,
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) { asArp(storage).processNotes(in, inCount, out, outCapacity, outCount); },
	.emitNotes =
	    [](void* storage, const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
	       size_t outCapacity, uint32_t* samplesUntilNext) {
		    return asArp(storage).emit(clock, sampleNow, out, outCapacity, samplesUntilNext);
	    },
	.seedPassed = [](void* storage, const device::NoteEvent* ons, size_t count) { asArp(storage).seedPassed(ons, count); },
};

const device::ModOps kModOps {
	.stateBytes = sizeof(Arp),
	.stateAlign = alignof(Arp),
	.construct = [](void* storage) { new (storage) Arp(); },
	.destruct = [](void* storage) { asArp(storage).~Arp(); },
	.prepare = [](void* storage, double sampleRate) { asArp(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asArp(storage).reset(); },
	.setFields = setFieldsOf,
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asArp(storage).tick(); },
	.outputAt = [](const void* storage, size_t output) { return asArp(storage).outputAt(output); },
	.playhead = [](const void* storage, uint32_t* ordinal, float* phase) { asArp(storage).playhead(ordinal, phase); },
	.recentEvents = [](const void* storage, device::RecentEvent* out, size_t capacity) {
		return asArp(storage).recentEvents(out, capacity);
	},
};

} // namespace feathers::mod::arp
