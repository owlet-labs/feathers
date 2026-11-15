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

#include "fx/drum/drum.h"

#include "dsp/field_override.h"
#include "dsp/slice_build.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <numbers>

namespace feathers::fx::drumdevice {

namespace {

using feathers::drum::Kind;

/// THE FADE A CHOKE OR A STEAL TAKES, the engine's own short voice fade.
constexpr double kFadeSeconds = 0.005;
/// WHERE A HIGH-PASS'S RING IS CALLED OVER and its state set to exact zero.
constexpr float kHpFloor = 1.0e-9f;

[[nodiscard]] double exp2Of(const Settings& s, Param p) {
	return std::exp2(static_cast<double>(s.at(p)));
}

[[nodiscard]] bool stereoWritten(Kind k) noexcept {
	// Membrane, Snare and Clap write the right ear themselves (Width); the others are one signal.
	return k == Kind::Membrane || k == Kind::Snare || k == Kind::Clap;
}

} // namespace

uint64_t strikeSeed(uint64_t seed, int key, Kind kind, int64_t tick, uint32_t count) noexcept {
	const uint64_t when = tick >= 0 ? (static_cast<uint64_t>(tick) << 1) | 1u : static_cast<uint64_t>(count) << 1;
	const auto who = static_cast<uint64_t>(key & 127) | (static_cast<uint64_t>(kind) << 8);
	uint64_t z = (seed + 0xD5A1u) * 0x9E3779B97F4A7C15ull ^ (who << 40) ^ (when * 0xC2B2AE3D27D4EB4Full) ^ 0x4B17ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	z ^= z >> 31;
	return z;
}

feathers::drum::Controls controlsOf(const Settings& s) {
	feathers::drum::Controls c;
	c.decaySeconds = exp2Of(s, Param::Decay);
	c.sweepSeconds = exp2Of(s, Param::SweepTime);
	c.drive = static_cast<double>(s.at(Param::Drive));
	c.count = s.index(Param::Claps);
	c.spreadSeconds = exp2Of(s, Param::Spread);
	// THE KIT'S REACH: the terrain's Pitch field moves a hit up to 3 semitones either side (the engine's kit stance).
	// In Pitched the engine's reach is half a semitone (the key sets the pitch; the terrain only fine-tunes it).
	c.pitchRangeSemitones = s.pitched() ? 0.5 : 3.0;
	c.velocityZone = static_cast<double>(s.at(Param::VelocityZone));
	c.wires = static_cast<double>(s.at(Param::Wires));
	c.bells = static_cast<double>(s.at(Param::Bells));
	c.wireDecaySeconds = exp2Of(s, Param::WireDecay);
	c.clickDecaySeconds = exp2Of(s, Param::ClickDecay);
	c.bellStartHz = exp2Of(s, Param::BellStart);
	c.bellEndHz = exp2Of(s, Param::BellEnd);
	c.bellSeconds = exp2Of(s, Param::BellTime);
	c.kickHoldSeconds = static_cast<double>(s.at(Param::KickHold)) / 1000.0;
	c.kickDip = static_cast<double>(s.at(Param::KickDip));
	c.kickDipSeconds = static_cast<double>(s.at(Param::KickDipTime)) / 1000.0;
	c.clickSweepOctaves = static_cast<double>(s.at(Param::ClickSweep));
	c.clickSweepSeconds = exp2Of(s, Param::ClickSweepTime);
	c.startPhaseDegrees = static_cast<double>(s.at(Param::StartPhase));
	c.driveCurve = s.index(Param::DriveCurve);
	c.driveTone = static_cast<double>(s.at(Param::DriveTone));
	c.clapQ = static_cast<double>(s.at(Param::ClapQ));
	c.clapHighPassHz = exp2Of(s, Param::ClapHighPass);
	c.bell2Hz = exp2Of(s, Param::Bell2Hz);
	c.bell2Seconds = exp2Of(s, Param::Bell2Time);
	c.velocityNoise = static_cast<double>(s.at(Param::VelocityNoise));
	c.width = static_cast<double>(s.at(Param::Width));
	// THE TERRAIN'S PITCH MODE is the kit's alone (the engine's Pitched mode keeps the half-semitone fine pitch as it always was).
	c.pitchMode = s.pitched() ? 0 : s.index(Param::PitchMode);
	return c;
}

feathers::drum::Fields fieldsOf(const Settings& s) {
	// THE HOME, as `sliceHome` states it, and the plane Rotate turns, with no banked residue: this device has none.
	const double home = sliceHome(static_cast<uint16_t>(s.index(Param::Zone)), s.at(Param::ZoneOffset),
	                              static_cast<double>(s.at(Param::Gamma)));
	const PartialBasis frame = sliceFrameFor<kPartialTriangles>(static_cast<double>(s.at(Param::Rotate)), home,
	                                                            SliceResidue<kPartialTriangles> {});
	const feathers::drum::Fields raw = readDrumFields(frame, home, static_cast<double>(s.at(Param::TravelX)),
	                                                  static_cast<double>(s.at(Param::TravelY)));
	DrumBias bias;
	bias.pitch = s.at(Param::FieldPitch);
	bias.length = s.at(Param::FieldLength);
	bias.sweep = s.at(Param::FieldSweep);
	bias.tone = s.at(Param::FieldTone);
	bias.noise = s.at(Param::FieldNoise);
	bias.snap = s.at(Param::FieldSnap);
	bias.metal = s.at(Param::FieldMetal);
	return drumFieldsWithOverrides(raw, bias);
}

double baseHzOf(Kind kind, const Settings& s, double movedSemitones) noexcept {
	return feathers::drum::kindBaseHz(kind)
	       * std::exp2((static_cast<double>(s.at(Param::Tune)) + static_cast<double>(s.at(Param::Bend)) * movedSemitones) / 12.0);
}

double baseHzOf(Kind kind, const Settings& s, int key, double movedSemitones) noexcept {
	if (!s.pitched()) {
		return baseHzOf(kind, s, movedSemitones);
	}
	// PITCHED: the engine's own, the voice's frequency (A4 = 440 on key 69, the whole of a tuning expression moving it) with Tune on top.
	// The kind's resting pitch and the Bend share are not read.
	return 440.0 * std::exp2((static_cast<double>(key) + movedSemitones - 69.0 + static_cast<double>(s.at(Param::Tune))) / 12.0);
}

DrumDsp::DrumDsp() {
	s_ = stateOf(Row {}, 0);
}

void DrumDsp::prepare(double sampleRate) {
	sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	builder_.prepare(sr_);
	for (Slot& s : slots_) {
		s.string.prepare(sr_);
		s.hit.prepare(sr_);
		s.hit.setString(&s.string, &builder_);
	}
	// EVERY KIND, STRUCK AND RENDERED ONCE, so a lazily built table is built here and not on the audio thread.
	Slot& w = slots_[0];
	const Settings defaults = stateOf(Row {}, 0);
	const feathers::drum::Controls c = controlsOf(defaults);
	const feathers::drum::Fields f = fieldsOf(defaults);
	std::array<float, kSpan> a {};
	std::array<float, kSpan> b {};
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		const auto kind = static_cast<Kind>(k);
		w.hit.strike(kind, feathers::drum::lawsFor(kind, f, c, baseHzOf(kind, defaults, 0.0), 1.0), 1u, 1.0f);
		w.hit.release(0.01);
		w.hit.setLaws(feathers::drum::lawsFor(kind, f, c, baseHzOf(kind, defaults, 1.0), 0.5));
		w.hit.render(a.data(), kSpan, b.data());
		w.hit.render(a.data(), kSpan, nullptr);
		w.hit.reset();
	}
	reset();
	terrainValid_ = false;
	setSettings(s_);
}

void DrumDsp::reset() {
	for (Slot& s : slots_) {
		s.hit.reset();
		s.string.reset();
		s.used = s.struck = s.released = s.gateApplied = s.fading = false;
		s.delayLeft = s.fadeLeft = 0;
		s.hpX = {};
		s.hpY = {};
	}
	queued_ = 0;
	keyTuning_.fill(0.0f);
	keyStrikes_.fill(0u);
	phase_ = -1;
}

void DrumDsp::setSettings(const Settings& s) {
	s_ = s;
	controls_ = controlsOf(s);
	// THE TERRAIN IS DECODED ONLY WHEN WHAT IT READS MOVED: the bits of the row's thirteen numbers (home, rotate, travel, scalers).
	const std::array<float, 13> key {
		s.at(Param::Zone), s.at(Param::ZoneOffset), s.at(Param::Gamma), s.at(Param::Rotate), s.at(Param::TravelX),
		s.at(Param::TravelY), s.at(Param::FieldPitch), s.at(Param::FieldLength), s.at(Param::FieldSweep), s.at(Param::FieldTone),
		s.at(Param::FieldNoise), s.at(Param::FieldSnap), s.at(Param::FieldMetal)
	};
	if (!terrainValid_ || std::memcmp(key.data(), terrainKey_.data(), sizeof(key)) != 0) {
		fields_ = fieldsOf(s);
		terrainKey_ = key;
		terrainValid_ = true;
	}
	// LEVEL AND PAN, equal power with the centre at unity in each ear (so a centred drum is the plain hit's level exactly).
	const float gain = static_cast<float>(std::pow(10.0, static_cast<double>(s.at(Param::Level)) / 20.0));
	const double pan = static_cast<double>(s.at(Param::Pan));
	const double theta = (std::clamp(pan, -1.0, 1.0) + 1.0) * std::numbers::pi / 4.0;
	gainL_ = gain * static_cast<float>(std::numbers::sqrt2 * std::cos(theta));
	gainR_ = gain * static_cast<float>(std::numbers::sqrt2 * std::sin(theta));
	if (s.at(Param::Pan) == 0.0f) {
		gainL_ = gain;
		gainR_ = gain;
	}
	// THE ONE-POLE HIGH-PASS, y = a (y' + x - x'); at its bottom (20 Hz) bypassed, so the drum is its hit exactly.
	const double hz = std::exp2(static_cast<double>(s.at(Param::HighPass)));
	hpA_ = hz <= 20.0 + 1.0e-6 ? 1.0f : static_cast<float>(1.0 / (1.0 + 2.0 * std::numbers::pi * hz / sr_));
}

void DrumDsp::queueNotes(const device::NoteEvent* in, size_t count) {
	for (size_t i = 0; i < count; ++i) {
		const device::NoteEvent& e = in[i];
		if (e.kind == device::NoteEvent::Kind::Expression) {
			// A KEY'S TUNING (semitones, absolute); a negative key is CLAP's wildcard. Read when the laws next refresh.
			if (e.expressionId == kTuningExpression && std::isfinite(e.expressionValue)) {
				const auto semis = static_cast<float>(std::clamp(e.expressionValue, -48.0, 48.0));
				if (e.key < 0) {
					keyTuning_.fill(semis);
				}
				else if (e.key < 128) {
					keyTuning_[static_cast<size_t>(e.key)] = semis;
				}
			}
			continue;
		}
		if (e.kind == device::NoteEvent::Kind::On && e.velocity > 0.0 && e.key >= 0 && e.key < 128) {
			keyTuning_[static_cast<size_t>(e.key)] = 0.0f; // a fresh note is untuned
		}
		// ONLY THE KEY IT ANSWERS (a choke may be a wildcard). A dropped event is a later one: an off before an on is kept first.
		// PITCHED answers every key (the key is the pitch); One Key only the one Note names.
		const bool mine = s_.pitched() || e.key == s_.index(Param::Note);
		const bool wildcardChoke = e.kind == device::NoteEvent::Kind::Choke && e.key < 0;
		if ((mine || wildcardChoke) && queued_ < queue_.size()) {
			queue_[queued_++] = e;
		}
	}
}

bool DrumDsp::hpRinging(const Slot& s) const noexcept {
	return hpA_ < 1.0f && (std::fabs(s.hpY[0]) > kHpFloor || std::fabs(s.hpY[1]) > kHpFloor);
}

bool DrumDsp::holdsTail() const noexcept {
	if (queued_ > 0) {
		return true;
	}
	for (const Slot& s : slots_) {
		if (s.used) {
			return true;
		}
	}
	return false;
}

int DrumDsp::soundingForTest() const noexcept {
	int n = 0;
	for (const Slot& s : slots_) {
		n += (s.used && !s.fading) ? 1 : 0;
	}
	return n;
}

int DrumDsp::fadingForTest() const noexcept {
	int n = 0;
	for (const Slot& s : slots_) {
		n += (s.used && s.fading) ? 1 : 0;
	}
	return n;
}

const feathers::drum::Hit* DrumDsp::newestForTest() const noexcept {
	const Slot* best = nullptr;
	for (const Slot& s : slots_) {
		if (s.used && !s.fading && s.struck && (best == nullptr || s.order > best->order)) {
			best = &s;
		}
	}
	return best != nullptr ? &best->hit : nullptr;
}

uint64_t DrumDsp::newestSeedForTest() const noexcept {
	const Slot* best = nullptr;
	for (const Slot& s : slots_) {
		if (s.used && (best == nullptr || s.order > best->order)) {
			best = &s;
		}
	}
	return best != nullptr ? best->seed : 0;
}

void DrumDsp::fade(Slot& s) {
	if (!s.used || s.fading) {
		return;
	}
	if (!s.struck) {
		s.used = false; // waiting its Delay: nothing to fade, the strike is simply never made
		return;
	}
	s.fadeTotal = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sr_)));
	s.fadeLeft = s.fadeTotal;
	s.fading = true;
}

DrumDsp::Slot* DrumDsp::acquire(int key) {
	const int limit = kVoicesCounts[std::clamp(s_.index(Param::Voices), 0, 2)];
	if (s_.pitched()) {
		// PITCHED: VOICES IS PER KEY (a chord is not one drum): the same key struck again past it fades that key's oldest hit, and
		// the whole drum holds at most `kMaxVoices` live hits across keys, the oldest of them fading when another key comes in.
		for (;;) {
			int sameKey = 0;
			int live = 0;
			Slot* oldestSame = nullptr;
			Slot* oldest = nullptr;
			for (Slot& s : slots_) {
				if (s.used && !s.fading) {
					++live;
					if (oldest == nullptr || s.order < oldest->order) {
						oldest = &s;
					}
					if (s.key == key) {
						++sameKey;
						if (oldestSame == nullptr || s.order < oldestSame->order) {
							oldestSame = &s;
						}
					}
				}
			}
			if (sameKey >= limit && oldestSame != nullptr) {
				fade(*oldestSame);
			}
			else if (live >= kMaxVoices && oldest != nullptr) {
				fade(*oldest);
			}
			else {
				break;
			}
		}
		return claimSlot();
	}
	// ONE MORE THAN VOICES FADES THE OLDEST: count the live ones and fade from the oldest until there is room for this one.
	for (;;) {
		int live = 0;
		Slot* oldest = nullptr;
		for (Slot& s : slots_) {
			if (s.used && !s.fading) {
				++live;
				if (oldest == nullptr || s.order < oldest->order) {
					oldest = &s;
				}
			}
		}
		if (live < limit || oldest == nullptr) {
			break;
		}
		fade(*oldest);
	}
	return claimSlot();
}

DrumDsp::Slot* DrumDsp::claimSlot() {
	Slot* taken = nullptr;
	for (Slot& s : slots_) {
		if (!s.used) {
			taken = &s;
			break;
		}
	}
	if (taken == nullptr) {
		// THE POOL IS FULL OF FADING HITS: the oldest is stopped outright (it was already on its way out).
		for (Slot& s : slots_) {
			if (taken == nullptr || s.order < taken->order) {
				taken = &s;
			}
		}
		taken->hit.reset();
		taken->string.reset();
	}
	taken->used = true;
	taken->struck = false;
	taken->released = false;
	taken->gateApplied = false;
	taken->fadeLeft = 0;
	taken->fading = false;
	taken->hpX = {};
	taken->hpY = {};
	return taken;
}

void DrumDsp::strikeNow(Slot& s) {
	const double moved = static_cast<double>(keyTuning_[static_cast<size_t>(s.key & 127)]);
	const feathers::drum::Laws laws = feathers::drum::lawsFor(s.kind, fields_, controls_, baseHzOf(s.kind, s_, s.key, moved), s.velocity);
	s.hit.strike(s.kind, laws, s.seed, static_cast<float>(std::clamp(s.velocity, 0.0, 1.0)));
	s.struck = true;
	++strikes_;
	if (s.released && s_.index(Param::Trigger) == 1) {
		s.hit.release(exp2Of(s_, Param::Release));
		s.gateApplied = true;
	}
}

void DrumDsp::release(Slot& s) {
	s.released = true;
	// THE GATE: a Gate drum's released key damps the hit to Release; a One-shot's never does. A hit still waiting its Delay is
	// damped as it strikes (strikeNow).
	if (s.struck && !s.gateApplied && s_.index(Param::Trigger) == 1 && !s.hit.idle()) {
		s.hit.release(exp2Of(s_, Param::Release));
		s.gateApplied = true;
	}
}

void DrumDsp::endKey(const device::NoteEvent& e) {
	for (Slot& s : slots_) {
		if (s.used && !s.fading && s.key == e.key && (e.noteId < 0 || s.noteId < 0 || s.noteId == e.noteId)) {
			release(s);
		}
	}
}

void DrumDsp::handle(const device::NoteEvent& e, int pos, int /*base*/) {
	switch (e.kind) {
	case device::NoteEvent::Kind::On: {
		if (e.velocity <= 0.0) {
			endKey(e); // a zero-velocity on is an off
			break;
		}
		Slot* s = acquire(e.key);
		s->kind = static_cast<Kind>(std::clamp(s_.index(Param::Kind), 0, static_cast<int>(feathers::drum::kKindCount) - 1));
		s->key = e.key;
		s->noteId = e.noteId;
		s->velocity = std::clamp(e.velocity, 0.0, 1.0);
		s->order = ++order_;
		// THE SONG'S PLACE, if the transport plays: the tick this sample is in. Else the key's own strike count.
		int64_t tick = -1;
		if (clock_.playing && clock_.havePositionSeconds()) {
			const double ticks = std::floor((clock_.songPositionSeconds * sr_ + static_cast<double>(pos)) / static_cast<double>(kSpan));
			tick = std::isfinite(ticks) && ticks >= 0.0 ? static_cast<int64_t>(ticks) : -1;
		}
		const uint32_t count = keyStrikes_[static_cast<size_t>(e.key & 127)];
		if (tick < 0) {
			++keyStrikes_[static_cast<size_t>(e.key & 127)];
		}
		s->seed = strikeSeed(static_cast<uint64_t>(std::lround(s_.at(Param::Seed))), e.key, s->kind, tick, count);
		s->delayLeft = static_cast<int>(std::lround(std::max(0.0, static_cast<double>(s_.at(Param::Delay))) * sr_ / 1000.0));
		if (s->delayLeft == 0) {
			strikeNow(*s);
		}
		break;
	}
	case device::NoteEvent::Kind::Off: endKey(e); break;
	case device::NoteEvent::Kind::Choke:
		for (Slot& s : slots_) {
			if (s.used && (e.key < 0 || s.key == e.key)) {
				fade(s);
			}
		}
		break;
	case device::NoteEvent::Kind::Expression: break;
	}
}

void DrumDsp::refreshLaws() {
	for (Slot& s : slots_) {
		if (!s.used || !s.struck || s.hit.idle() || s.kind != static_cast<Kind>(std::clamp(s_.index(Param::Kind), 0, 5))) {
			continue;
		}
		// THE CONTROL TICK'S REFRESH: a sounding hit follows the row (a route on Decay, Tone, Zone; the key's tuning) and keeps its velocity.
		const double moved = static_cast<double>(keyTuning_[static_cast<size_t>(s.key & 127)]);
		s.hit.setLaws(feathers::drum::lawsFor(s.kind, fields_, controls_, baseHzOf(s.kind, s_, s.key, moved), s.velocity));
	}
}

void DrumDsp::renderSpan(float* left, float* right, int n) {
	for (Slot& s : slots_) {
		if (!s.used || (!s.struck && s.delayLeft > 0)) {
			continue;
		}
		if (!s.struck) {
			strikeNow(s);
		}
		s.hit.render(tl_.data(), n, tr_.data());
		if (!stereoWritten(s.kind)) {
			std::copy(tl_.begin(), tl_.begin() + n, tr_.begin());
		}
		if (s.fading) {
			for (int i = 0; i < n; ++i) {
				const float g = s.fadeLeft > 0 ? static_cast<float>(s.fadeLeft) / static_cast<float>(s.fadeTotal) : 0.0f;
				tl_[static_cast<size_t>(i)] *= g;
				tr_[static_cast<size_t>(i)] *= g;
				if (s.fadeLeft > 0) {
					--s.fadeLeft;
				}
			}
		}
		if (hpA_ < 1.0f) {
			const std::array<float*, 2> ears { tl_.data(), tr_.data() };
			for (size_t ear = 0; ear < 2; ++ear) {
				float px = s.hpX[ear];
				float py = s.hpY[ear];
				for (int i = 0; i < n; ++i) {
					const float y = hpA_ * (py + ears[ear][i] - px);
					px = ears[ear][i];
					py = y;
					ears[ear][i] = y;
				}
				s.hpX[ear] = px;
				s.hpY[ear] = py;
			}
		}
		for (int i = 0; i < n; ++i) {
			left[i] += gainL_ * tl_[static_cast<size_t>(i)];
			right[i] += gainR_ * tr_[static_cast<size_t>(i)];
		}
		// DONE: faded out, or silent to the exact sample with its high-pass rung out. Its state is then exact zero.
		const bool done = s.fading ? s.fadeLeft == 0 : (s.hit.idle() && !hpRinging(s));
		if (done) {
			s.hit.reset();
			s.string.reset();
			s.used = s.struck = s.fading = false;
			s.hpX = {};
			s.hpY = {};
		}
	}
}

void DrumDsp::process(float* left, float* right, int frames) {
	if (left == nullptr || right == nullptr || frames <= 0) {
		return;
	}
	// A HOST THAT SAID WHERE THE CALL STARTS IN ITS TICK (`NoteContext::tickPhase`) gets each note on its sample; one that said nothing
	// is read as "from the start of this call". The stamp is for this call alone.
	const bool known = phase_ >= 0;
	const int base = known ? phase_ : 0;
	phase_ = -1;
	// A NOTE BEFORE THIS CALL'S START was queued for a call that never ran (a parked device): it is dropped, not struck late.
	size_t head = 0;
	while (head < queued_ && static_cast<int64_t>(queue_[head].timeSamples) < base) {
		++head;
		++staleDropped_;
	}
	refreshLaws();
	int pos = 0;
	while (pos < frames) {
		// THE NOTES DUE AT THIS SAMPLE, in the order the host gave them.
		while (head < queued_ && static_cast<int64_t>(queue_[head].timeSamples) - base <= pos) {
			handle(queue_[head++], pos, base);
		}
		int span = std::min(frames - pos, kSpan);
		if (head < queued_) {
			span = std::min(span, std::max(1, static_cast<int>(queue_[head].timeSamples) - base - pos));
		}
		for (const Slot& s : slots_) {
			if (s.used && !s.struck && s.delayLeft > 0) {
				span = std::min(span, s.delayLeft);
			}
		}
		// A SAMPLE THE HOST HANDED US THAT IS NOT A NUMBER IS NOT PASSED ON.
		for (int i = 0; i < span; ++i) {
			if (!std::isfinite(left[pos + i])) {
				left[pos + i] = 0.0f;
			}
			if (!std::isfinite(right[pos + i])) {
				right[pos + i] = 0.0f;
			}
		}
		renderSpan(left + pos, right + pos, span);
		for (Slot& s : slots_) {
			if (s.used && !s.struck && s.delayLeft > 0) {
				s.delayLeft -= span;
			}
		}
		pos += span;
	}
	// WHAT IS LEFT QUEUED waits for the next call; in a host with no tick phase it is rebased onto that call's start.
	size_t kept = 0;
	for (size_t i = head; i < queued_; ++i) {
		device::NoteEvent e = queue_[i];
		if (!known) {
			e.timeSamples -= std::min<uint32_t>(e.timeSamples, static_cast<uint32_t>(frames));
		}
		queue_[kept++] = e;
	}
	queued_ = kept;
}

namespace {

DrumDsp& asDrum(void* storage) {
	return *static_cast<DrumDsp*>(storage);
}

void construct(void* storage) {
	new (storage) DrumDsp();
}
void destruct(void* storage) {
	asDrum(storage).~DrumDsp();
}
void setFields(void* storage, const float* fields, size_t fieldsPresent) {
	// THE DEVICE DECODES ITS OWN ROW, so a field the row does not carry lands on its DECLARED default.
	Row row {};
	const size_t take = std::min(fieldsPresent, row.size());
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	asDrum(storage).setSettings(stateOf(row, take));
}

} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(DrumDsp),
    .stateAlign = alignof(DrumDsp),
    .construct = construct,
    .destruct = destruct,
    .prepare = [](void* storage, double sampleRate, int /*maxFrames*/) { asDrum(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asDrum(storage).reset(); },
    .setFields = setFields,
    // WHERE THE CALL STARTS IN ITS TICK: the notes of this piece are placed on their samples with it (drum.h).
    .setNoteContext = [](void* storage, const device::NoteContext& note) { asDrum(storage).setTickPhase(note.tickPhase); },
    .setTransportClock = [](void* storage, const device::TransportClock& clock) { asDrum(storage).setClock(clock); },
    .processStereo = [](void* storage, float* left, float* right, int frames) { asDrum(storage).process(left, right, frames); },
    // NO MONO ROAD: a drum may be panned, so the host hands a mono buffer as two.
    .processMono = nullptr,
    .setReadingsSink = nullptr,
    .setOuterSources = nullptr,
    .beginPerVoiceBlock = nullptr,
    .processVoice = nullptr,
    .windowedControls = {},
    .setFieldWindows = nullptr,
    .latencySamples = nullptr,
    .setFieldsR = nullptr,
    .holdsTail = [](const void* storage) { return static_cast<const DrumDsp*>(storage)->holdsTail(); },
};

const device::NoteOps kNoteOps {
    .stateBytes = sizeof(DrumDsp),
    .stateAlign = alignof(DrumDsp),
    .construct = construct,
    .destruct = destruct,
    // THE NOTE TABLE CARRIES NO LONGEST BLOCK: a rack prepares this object once, through the audio table.
    .prepare = [](void* storage, double sampleRate) { asDrum(storage).prepare(sampleRate); },
    .setFields = setFields,
    // **A READER, SO A PASS-THROUGH**: every event continues down the chain untouched (so drums on one key layer), and the ones this
    // device answers are queued for the next process call, where each lands on its sample.
    .processNotes =
        [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
           size_t* outCount) {
	        const size_t n = std::min(inCount, outCapacity);
	        std::copy(in, in + n, out);
	        *outCount = n;
	        asDrum(storage).queueNotes(in, n);
        },
};

} // namespace feathers::fx::drumdevice
