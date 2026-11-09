/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "mod/stepseq/stepseq.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <new>

#include "device/extra_state.h"

namespace feathers::mod::stepseq {

namespace {

/// Bits of the packed word — see `packStep`.
constexpr uint32_t kProbBits = 0;
constexpr uint32_t kSlewBits = 7;
constexpr uint32_t kRatchetBits = 14;
constexpr uint32_t kGateBits = 17;
constexpr uint32_t kLockBit = 19;

[[nodiscard]] int percentOf(float unit) noexcept {
	return std::isfinite(unit) ? static_cast<int>(std::lround(std::clamp(unit, 0.0f, 1.0f) * 100.0f)) : 0;
}

[[nodiscard]] float quantisePercent(float unit) noexcept {
	return static_cast<float>(percentOf(unit)) / 100.0f;
}

/// A CONTROL AS THE ENGINE READS IT: its field, or its declared default past the row; never non-finite; in range.
[[nodiscard]] float fieldOf(const float* fields, size_t present, Param p) noexcept {
	const device::ParamDesc& d = kParamDescs[at(p)];
	const float v = at(p) < present ? fields[at(p)] : static_cast<float>(d.def);
	return device::coerceControl(d, std::isfinite(v) ? v : static_cast<float>(d.def));
}

[[nodiscard]] size_t choiceOf(const float* fields, size_t present, Param p) noexcept {
	const device::ParamDesc& d = kParamDescs[at(p)];
	const long i = std::lround(fieldOf(fields, present, p));
	return static_cast<size_t>(std::clamp(i, static_cast<long>(d.min), static_cast<long>(d.max)));
}

[[nodiscard]] bool switchOf(const float* fields, size_t present, Param p) noexcept {
	return fieldOf(fields, present, p) >= 0.5f;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// The steps and the row                                                                          //
//------------------------------------------------------------------------------------------------//

float packStep(const Step& s) noexcept {
	const uint32_t prob = static_cast<uint32_t>(100 - percentOf(s.probability));
	const uint32_t slew = static_cast<uint32_t>(percentOf(s.slew));
	const uint32_t ratchet = static_cast<uint32_t>(std::clamp(s.ratchet, 1, kMaxRatchet) - 1);
	const uint32_t gate = std::min(static_cast<uint32_t>(s.gate), static_cast<uint32_t>(Gate::kCount) - 1);
	const uint32_t word = (prob << kProbBits) | (slew << kSlewBits) | (ratchet << kRatchetBits) | (gate << kGateBits)
	                      | ((s.locked ? 1u : 0u) << kLockBit);
	return static_cast<float>(word);
}

Step unpackStep(float value, float word) noexcept {
	Step s;
	s.value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
	const float w = std::isfinite(word) ? std::clamp(word, 0.0f, static_cast<float>((1u << 20) - 1)) : 0.0f;
	const auto bits = static_cast<uint32_t>(std::lround(w));
	s.probability = static_cast<float>(100 - std::min<uint32_t>((bits >> kProbBits) & 0x7Fu, 100u)) / 100.0f;
	s.slew = static_cast<float>(std::min<uint32_t>((bits >> kSlewBits) & 0x7Fu, 100u)) / 100.0f;
	s.ratchet = static_cast<int>((bits >> kRatchetBits) & 0x7u) + 1;
	s.gate = static_cast<Gate>(std::min<uint32_t>((bits >> kGateBits) & 0x3u, static_cast<uint32_t>(Gate::kCount) - 1));
	s.locked = ((bits >> kLockBit) & 1u) != 0;
	return s;
}

Pattern patternOf(const float* fields, size_t present) {
	Pattern p;
	for (size_t s = 0; s < kMaxSteps; ++s) {
		const float value = valueIndex(s) < present ? fields[valueIndex(s)] : 0.0f;
		const float word = wordIndex(s) < present ? fields[wordIndex(s)] : 0.0f;
		p.step[s] = unpackStep(value, word);
	}
	const float g = generationIndex() < present ? fields[generationIndex()] : 0.0f;
	p.generation = std::isfinite(g) ? static_cast<uint32_t>(std::clamp(g, 0.0f, static_cast<float>(0xFFFFFF))) : 0u;
	const float b = basisIndex() < present ? fields[basisIndex()] : 0.0f;
	p.basis = std::isfinite(b) ? b : 0.0f;
	return p;
}

void writePattern(const Pattern& pattern, float* fields) {
	for (size_t s = 0; s < kMaxSteps; ++s) {
		fields[valueIndex(s)] = std::clamp(pattern.step[s].value, 0.0f, 1.0f);
		fields[wordIndex(s)] = packStep(pattern.step[s]);
	}
	fields[generationIndex()] = static_cast<float>(pattern.generation & 0xFFFFFFu);
	fields[basisIndex()] = pattern.basis;
}

Settings settingsOf(const float* fields, size_t present) {
	Settings s;
	s.steps = choiceOf(fields, present, Param::Steps);
	s.direction = static_cast<Direction>(choiceOf(fields, present, Param::Direction));
	s.clock = static_cast<Clock>(choiceOf(fields, present, Param::Clock));
	s.timeBase = choiceOf(fields, present, Param::TimeBase);
	s.rate = fieldOf(fields, present, Param::Rate);
	s.gateLength = fieldOf(fields, present, Param::GateLength);
	s.retrig = switchOf(fields, present, Param::Retrig);
	s.poly = switchOf(fields, present, Param::Poly);
	s.bipolar = switchOf(fields, present, Param::Bipolar);
	s.seed = static_cast<int>(choiceOf(fields, present, Param::Seed));
	s.randomAmount = fieldOf(fields, present, Param::RandomAmount);
	s.turing = switchOf(fields, present, Param::Turing);
	s.turingLock = fieldOf(fields, present, Param::TuringLock);
	s.enabled = switchOf(fields, present, Param::Enabled);
	s.trig = fieldOf(fields, present, Param::Trig);
	s.reset = fieldOf(fields, present, Param::Reset);
	s.amount = fieldOf(fields, present, Param::Amount);
	for (size_t l = 0; l < kLaneCount; ++l) {
		s.randTrig[l] = fieldOf(fields, present, randomiseParam(static_cast<Lane>(l)));
	}
	s.pattern = patternOf(fields, present);
	s.mode = s.retrig ? Mode::LfoRetrig : Mode::Free;
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[at(Param::Steps)] = static_cast<float>(s.steps);
	row[at(Param::Direction)] = static_cast<float>(s.direction);
	row[at(Param::Clock)] = static_cast<float>(s.clock);
	row[at(Param::TimeBase)] = static_cast<float>(s.timeBase);
	row[at(Param::Rate)] = s.rate;
	row[at(Param::GateLength)] = s.gateLength;
	row[at(Param::Retrig)] = s.retrig ? 1.0f : 0.0f;
	row[at(Param::Poly)] = s.poly ? 1.0f : 0.0f;
	row[at(Param::Bipolar)] = s.bipolar ? 1.0f : 0.0f;
	row[at(Param::Seed)] = static_cast<float>(s.seed);
	row[at(Param::RandomAmount)] = s.randomAmount;
	row[at(Param::Turing)] = s.turing ? 1.0f : 0.0f;
	row[at(Param::TuringLock)] = s.turingLock;
	row[at(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[at(Param::Trig)] = s.trig;
	row[at(Param::Reset)] = s.reset;
	row[at(Param::Amount)] = s.amount;
	for (size_t l = 0; l < kLaneCount; ++l) {
		row[at(randomiseParam(static_cast<Lane>(l)))] = s.randTrig[l];
	}
	writePattern(s.pattern, row.data());
	return row;
}

double stepHz(const Settings& s, double secondsPerBeat) noexcept {
	if (s.synced() && secondsPerBeat > 0.0 && std::isfinite(secondsPerBeat)) {
		const double beats = device::kDivisionBeats[std::min(s.timeBase - 1, device::kDivisionCount - 1)]
		                     * timeSeconds(s.rate);
		const double seconds = beats * secondsPerBeat;
		return seconds > 0.0 ? 1.0 / seconds : 0.0;
	}
	return timeSeconds(s.rate);
}

//------------------------------------------------------------------------------------------------//
// The act                                                                                        //
//------------------------------------------------------------------------------------------------//

float laneValue(const Step& step, Lane lane) noexcept {
	switch (lane) {
	case Lane::Value: return step.value;
	case Lane::Gate: return step.gate == Gate::On ? 1.0f : step.gate == Gate::Tie ? 0.5f : 0.0f;
	case Lane::Probability: return step.probability;
	case Lane::Ratchet: return static_cast<float>(step.ratchet - 1) / static_cast<float>(kMaxRatchet - 1);
	case Lane::Slew: return step.slew;
	case Lane::kCount: break;
	}
	return 0.0f;
}

void setLaneValue(Step* step, Lane lane, float unit) noexcept {
	const float u = std::isfinite(unit) ? std::clamp(unit, 0.0f, 1.0f) : 0.0f;
	switch (lane) {
	case Lane::Value: step->value = u; break;
	case Lane::Gate: step->gate = u >= 2.0f / 3.0f ? Gate::On : u >= 1.0f / 3.0f ? Gate::Tie : Gate::Off; break;
	case Lane::Probability: step->probability = quantisePercent(u); break;
	case Lane::Ratchet:
		step->ratchet = 1 + static_cast<int>(std::lround(u * static_cast<float>(kMaxRatchet - 1)));
		break;
	case Lane::Slew: step->slew = quantisePercent(u); break;
	case Lane::kCount: break;
	}
}

Pattern randomise(const Pattern& pattern, size_t steps, Lane lane, float amount, int seed) noexcept {
	const float a = std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f;
	if (!(a > 0.0f)) {
		return pattern; // nothing moves, so it is no act and counts nothing
	}
	Pattern out = pattern;
	Dice dice;
	dice.seed((static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) ^ (static_cast<uint64_t>(pattern.generation) << 4)
	          ^ static_cast<uint64_t>(lane) ^ 0xA5A5'0000'0000'0000ull);
	const size_t n = std::min(std::max<size_t>(steps, 1), kMaxSteps);
	for (size_t i = 0; i < n; ++i) {
		// TWO DRAWS FOR EVERY STEP, LOCKED OR NOT, so locking one step never moves what another one draws.
		const float draw = dice.unit();
		const float chance = dice.unit();
		Step& st = out.step[i];
		if (st.locked) {
			continue;
		}
		switch (lane) {
		case Lane::Value: st.value = std::clamp(st.value + a * (draw - st.value), 0.0f, 1.0f); break;
		case Lane::Probability: st.probability = quantisePercent(st.probability + a * (draw - st.probability)); break;
		case Lane::Slew: st.slew = quantisePercent(st.slew + a * (draw - st.slew)); break;
		case Lane::Gate:
			if (chance < a) {
				st.gate = draw < 0.7f ? Gate::On : draw < 0.9f ? Gate::Off : Gate::Tie;
			}
			break;
		case Lane::Ratchet:
			// MOSTLY ONE HIT: the square leans the draw toward the bottom, so a fresh lane is a pattern with a few rolls.
			if (chance < a) {
				st.ratchet = 1 + std::min(3, static_cast<int>(draw * draw * 4.0f));
			}
			break;
		case Lane::kCount: break;
		}
	}
	out.generation = (pattern.generation + 1u) & 0xFFFFFFu;
	return out;
}

//------------------------------------------------------------------------------------------------//
// The running pattern                                                                            //
//------------------------------------------------------------------------------------------------//

void Shape::prepare(double sampleRate) {
	const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kBlockSamples) / sr;
	reset();
}

void Shape::reset() {
	const double tick = tickSeconds_;
	*this = Shape {};
	tickSeconds_ = tick;
}

void Shape::hit() noexcept {
	hitThisTick_ = true;
	++hits_;
}

int Shape::pick(const Settings& s, int from, float draw) noexcept {
	const int n = static_cast<int>(std::clamp<size_t>(s.steps, 1, kMaxSteps));
	const int i = std::clamp(from, 0, n - 1);
	switch (s.direction) {
	case Direction::Forward: return (i + 1) % n;
	case Direction::Backward: return (i - 1 + n) % n;
	case Direction::PingPong: {
		if (n == 1) {
			return 0;
		}
		int to = i + bounce_;
		if (to >= n) {
			bounce_ = -1;
			to = i - 1;
		}
		else if (to < 0) {
			bounce_ = 1;
			to = i + 1;
		}
		return to;
	}
	case Direction::Random: return std::min(static_cast<int>(draw * static_cast<float>(n)), n - 1);
	case Direction::RandomWalk: return ((draw < 0.5f ? i - 1 : i + 1) + n) % n;
	case Direction::kCount: break;
	}
	return 0;
}

void Shape::restart(const Settings& s) {
	dice_.seed(playSeed(s.seed, ordinal_));
	bounce_ = 1;
	plays_ = false;
	started_ = true;
	stepPos_ = 0.0;
	const int n = static_cast<int>(std::clamp<size_t>(s.steps, 1, kMaxSteps));
	// ONE DRAW FOR WHERE IT STARTS, whatever the direction, so the first step's own draws are the same in every one.
	const float first = dice_.unit();
	switch (s.direction) {
	case Direction::Backward: next_ = n - 1; break;
	case Direction::Random: next_ = std::min(static_cast<int>(first * static_cast<float>(n)), n - 1); break;
	default: next_ = 0; break;
	}
	enter(s);
}

void Shape::enter(const Settings& s) {
	const int n = static_cast<int>(std::clamp<size_t>(s.steps, 1, kMaxSteps));
	// A PATTERN SHORTENED UNDER IT: the step it meant to play is folded back into the live ones.
	const int idx = next_ >= 0 ? next_ % n : 0;
	step_ = idx;
	++arrivals_;
	// FOUR DRAWS, ALWAYS, IN THIS ORDER — the probability, the Turing roll, the Turing value, the next step's pick.
	const float rollPlays = dice_.unit();
	const float rollTuring = dice_.unit();
	const float turingValue = dice_.unit();
	const float rollPick = dice_.unit();

	const Step& st = s.pattern.step[static_cast<size_t>(idx)];
	const uint32_t bit = 1u << static_cast<uint32_t>(idx);
	if (s.turing && !st.locked && rollTuring >= s.turingLock) {
		register_[static_cast<size_t>(idx)] = turingValue;
		mutated_ |= bit;
		++rewrites_;
	}
	const float value = (mutated_ & bit) != 0 ? register_[static_cast<size_t>(idx)] : st.value;

	const bool before = plays_;
	gateKind_ = st.gate;
	switch (st.gate) {
	case Gate::On: plays_ = rollPlays < st.probability; break;
	case Gate::Tie: plays_ = before; break; // a tie continues a note, and there is none to continue after a rest
	default: plays_ = false; break;
	}
	from_ = raw_;
	target_ = plays_ ? value : raw_;
	slew_ = plays_ ? st.slew : 0.0f;
	ratchet_ = (plays_ && st.gate == Gate::On) ? std::clamp(st.ratchet, 1, kMaxRatchet) : 1;
	sub_ = 0;
	if (plays_ && st.gate == Gate::On) {
		hit();
	}
	next_ = pick(s, idx, rollPick);
	nextTies_ = plays_ && s.pattern.step[static_cast<size_t>(next_ % n)].gate == Gate::Tie;
	if (ticksInStep_ > 0) {
		intervalTicks_ = ticksInStep_;
	}
	ticksInStep_ = 0;
}

float Shape::tick(const Settings& s, bool /*gate*/, bool retrigger, TransportClock clock, float rateMul) {
	hitThisTick_ = false;
	const bool restartNow = !started_ || (s.retrig && retrigger) || resetPending_;
	const bool advance = advancePending_ && !restartNow;
	advancePending_ = false;
	resetPending_ = false;
	if (restartNow) {
		restart(s);
	}

	// THE SONG'S GRID, under Free on a playing transport: the steps land where the song says, and a jump (a play
	// start, a locate) restarts the pattern on it. A synced base follows the song's beats, Hz its seconds (backlog NJ).
	double window = 0.0;
	double position = 0.0;
	double tickLength = 0.0;
	int unit = 0;
	if (!s.retrig && s.clock == Clock::Rate && clock.playing) {
		if (s.synced() && clock.haveTempo() && clock.havePositionBeats()) {
			window = device::kDivisionBeats[std::min(s.timeBase - 1, device::kDivisionCount - 1)] * timeSeconds(s.rate);
			position = clock.songPositionBeats;
			tickLength = tickSeconds_ / clock.secondsPerBeat;
			unit = 1;
		}
		else if (!s.synced() && clock.havePositionSeconds()) {
			const double hz = stepHz(s, 0.0) * static_cast<double>(rateMul);
			window = hz > 0.0 ? 1.0 / hz : 0.0;
			position = clock.songPositionSeconds;
			tickLength = tickSeconds_;
			unit = 2;
		}
	}
	const bool songRoad = unit != 0 && window > 0.0 && std::isfinite(window);

	if (songRoad) {
		const double step = position - songWas_;
		const bool jumped = !songAnchored_ || unit != songUnit_ || !(step >= 0.0)
		                    || step > TransportLock::kJumpTicks * tickLength;
		double turns = lock_.follow(position, window, tickLength, unit);
		songWas_ = position;
		songUnit_ = unit;
		songAnchored_ = true;
		if (jumped) {
			if (!restartNow) {
				restart(s);
			}
			lastWhole_ = std::floor(turns);
		}
		else if (restartNow || advance) {
			// A TRIG'S STEP BEGINS NOW, and the grid after it is counted from here.
			lock_.restartAt(0.0);
			turns = 0.0;
			lastWhole_ = 0.0;
			if (advance) {
				enter(s);
			}
		}
		else {
			const double whole = std::floor(turns);
			for (int k = 0; k < 8 && lastWhole_ < whole; ++k) {
				lastWhole_ += 1.0;
				enter(s);
			}
			lastWhole_ = whole;
		}
		phase_ = static_cast<float>(turns - std::floor(turns));
	}
	else {
		if (songAnchored_) {
			lock_.release();
			songAnchored_ = false;
		}
		if (s.clock == Clock::Rate) {
			if (restartNow || advance) {
				if (advance) {
					enter(s);
				}
				stepPos_ = 0.0;
			}
			else {
				const double inc = stepHz(s, clock.haveTempo() ? clock.secondsPerBeat : 0.0)
				                   * static_cast<double>(rateMul) * tickSeconds_;
				if (std::isfinite(inc) && inc > 0.0) {
					stepPos_ += inc;
					for (int k = 0; k < 8 && stepPos_ >= 1.0; ++k) {
						stepPos_ -= 1.0;
						enter(s);
					}
					stepPos_ -= std::floor(stepPos_);
				}
			}
			phase_ = static_cast<float>(stepPos_);
		}
		else {
			if (advance) {
				enter(s);
			}
			// HOW FAR THROUGH, AGAINST THE LAST STEP'S LENGTH — the only clock a Trig-driven pattern has.
			phase_ = intervalTicks_ > 0
			             ? std::min(static_cast<float>(ticksInStep_) / static_cast<float>(intervalTicks_), 0.999f)
			             : 0.0f;
		}
	}

	// THE RATCHET'S LATER HITS, as the phase reaches each subdivision.
	const float p = std::clamp(phase_, 0.0f, 0.999999f);
	int sub = 0;
	float subPhase = p;
	if (ratchet_ > 1) {
		const float scaled = p * static_cast<float>(ratchet_);
		sub = std::min(static_cast<int>(scaled), ratchet_ - 1);
		subPhase = scaled - static_cast<float>(sub);
		for (; sub_ < sub; ++sub_) {
			hit();
		}
	}
	const bool lastSub = sub == ratchet_ - 1;
	const bool held = s.gateLength >= 1.0f || subPhase < s.gateLength;
	gate_ = plays_ && (held || (lastSub && nextTies_)) ? 1.0f : 0.0f;

	// A TRIG CLOCK THAT HAS NOT YET HEARD TWO TRIGS HAS NO STEP LENGTH, so its slew has arrived rather than waiting.
	const bool noLength = !songRoad && s.clock == Clock::Trig && intervalTicks_ == 0;
	const float glide = (slew_ > 0.0f && !noLength) ? std::min(1.0f, p / slew_) : 1.0f;
	raw_ = from_ + (target_ - from_) * glide;
	value_ = s.bipolar ? 2.0f * raw_ - 1.0f : raw_;
	trigger_ = hitThisTick_ ? 1.0f : 0.0f;
	++ticksInStep_;
	return value_;
}

//------------------------------------------------------------------------------------------------//
// The device                                                                                     //
//------------------------------------------------------------------------------------------------//

uint32_t Device::ordinalForNewNote(const Shape& starting) const noexcept {
	// ONE BIT PER ORDINAL A HELD NOTE CARRIES; the table holds sixteen notes, so the lowest clear bit is below 17.
	uint32_t taken = 0;
	for (const auto& n : poly.notes.notes) {
		if (&n.shape == &starting || !n.used || n.done || !n.gate || n.shape.ordinal() >= 32) {
			continue;
		}
		taken |= 1u << n.shape.ordinal();
	}
	return static_cast<uint32_t>(std::countr_one(taken));
}

void Device::tick(const device::ModTickContext& ctx) {
	// THE RECORD'S STEPS TAKE OVER when its basis moves (a hand's edit, an undo, a load): every rewrite is dropped, and
	// the record's pattern — which already holds every rewrite it was sent — is what plays. Compared to the bit.
	const float basis = poly.settings.pattern.basis;
	if (!(std::bit_cast<uint32_t>(basis) == std::bit_cast<uint32_t>(takenBasis))) {
		takenBasis = basis;
		poly.mono.clearRewrites();
		for (auto& n : poly.notes.notes) {
			n.shape.clearRewrites();
		}
		publishedRewrites = poly.mono.rewrites();
		haveRolled = false; // and a roll the record had not caught up with: a hand's edit outranks it
	}
	// **A FIRED RANDOMISE PLAYS AT ONCE** (backlog NT): until the record holds it — the harvest of `publish` — the rolled
	// pattern stands in for the record's, which `setFields` goes on handing back unchanged until then.
	if (haveRolled) {
		// CAUGHT UP: the record holds the roll, with or without the rewrites the machine has made on it since.
		Pattern rewritten = rolled;
		poly.mono.applyRewrites(&rewritten);
		if (poly.settings.pattern == rolled || poly.settings.pattern == rewritten) {
			haveRolled = false;
		}
		else {
			poly.settings.pattern = rolled;
		}
	}
	// RANDOMISE BEFORE THE RESET AND THE ADVANCE, so a step entered on this tick is entered on the new pattern — each
	// lane's own Trig its own lane, in `Lane` order when two fire on one tick (each a press, one after the other).
	for (size_t l = 0; l < kLaneCount; ++l) {
		if (randomEdges[l].step(poly.settings.randTrig[l])) {
			++randomFires[l];
			roll(static_cast<Lane>(l));
		}
	}
	// THE RESET FIRST, the Advance's way (`mod::PolyShape::tick`): the device's edge fires the one pattern and every
	// note the host cannot resolve for; a note it can resolve watches its own value.
	const bool fired = resetEdge.step(poly.settings.reset);
	if (fired) {
		++resetFires;
		poly.mono.fireReset();
	}
	if (poly.poly()) {
		const device::ModTickContext::PerNoteResolver* r = ctx.perNote;
		for (auto& n : poly.notes.notes) {
			if (!n.used || n.done) {
				continue;
			}
			float own = 0.0f;
			const bool resolved = r != nullptr && r->resolve != nullptr
			                      && r->resolve(r->context, Settings::kResetControl, n.noteId, n.channel, n.key, &own);
			if (resolved ? n.shape.resetEdge.step(own) : fired) {
				n.shape.fireReset();
				resetFires += resolved ? 1u : 0u;
			}
		}
	}
	poly.tick(ctx);
	// THE ONE MACHINE'S REWRITES ARE PUBLISHED; a note's end with the note (approved 2026-10-17). Under Poly the one
	// machine is only the answer for a note the table never saw, so it publishes nothing then.
	if (!poly.poly() && poly.mono.rewrites() != publishedRewrites) {
		publishedRewrites = poly.mono.rewrites();
		serial = device::nextExtraStateSerial();
	}
	publishPlay();
}

void Device::publishPlay() noexcept {
	std::array<float, kPlayWords> words {};
	device::BlockWriter w(words.data(), words.size());
	poly.mono.save(w);
	playSnapshot.publish(words.data(), words.size());
}

void Shape::save(device::BlockWriter& w) const noexcept {
	w.u64(dice_.state);
	w.flag(started_);
	w.u32(static_cast<uint32_t>(step_));
	w.u32(static_cast<uint32_t>(next_));
	w.u32(static_cast<uint32_t>(bounce_));
	w.flag(plays_);
	w.flag(playedBefore_);
	w.u16(static_cast<uint32_t>(gateKind_));
	w.flag(nextTies_);
	w.u32(static_cast<uint32_t>(ratchet_));
	w.u32(static_cast<uint32_t>(sub_));
	w.f32(slew_);
	w.f64(stepPos_);
	w.u32(ticksInStep_);
	w.u32(intervalTicks_);
	w.f32(phase_);
	w.f32(from_);
	w.f32(target_);
	w.f32(raw_);
	w.f32(value_);
	w.f32(gate_);
	w.u32(arrivals_);
	w.u32(hits_);
	w.u32(rewrites_);
}

bool Shape::load(device::BlockReader& r) noexcept {
	Shape s = *this;
	s.dice_.state = r.u64();
	s.started_ = r.flag();
	s.step_ = static_cast<int>(r.u32());
	s.next_ = static_cast<int>(r.u32());
	s.bounce_ = static_cast<int>(r.u32());
	s.plays_ = r.flag();
	s.playedBefore_ = r.flag();
	const uint32_t gate = r.u16();
	s.nextTies_ = r.flag();
	s.ratchet_ = static_cast<int>(r.u32());
	s.sub_ = static_cast<int>(r.u32());
	s.slew_ = r.f32();
	s.stepPos_ = r.f64();
	s.ticksInStep_ = r.u32();
	s.intervalTicks_ = r.u32();
	s.phase_ = r.f32();
	s.from_ = r.f32();
	s.target_ = r.f32();
	s.raw_ = r.f32();
	s.value_ = r.f32();
	s.gate_ = r.f32();
	s.arrivals_ = r.u32();
	s.hits_ = r.u32();
	s.rewrites_ = r.u32();
	if (!r.ok() || gate >= static_cast<uint32_t>(Gate::kCount) || s.step_ < -1 || s.step_ >= static_cast<int>(kMaxSteps)
	    || s.next_ < 0 || s.next_ >= static_cast<int>(kMaxSteps) || (s.bounce_ != 1 && s.bounce_ != -1)
	    || s.ratchet_ < 1 || s.ratchet_ > kMaxRatchet || !std::isfinite(s.stepPos_) || !std::isfinite(s.raw_)
	    || !std::isfinite(s.from_) || !std::isfinite(s.target_)) {
		return false;
	}
	s.gateKind_ = static_cast<Gate>(gate);
	*this = s;
	return true;
}

void Device::roll(Lane lane) noexcept {
	const Settings& s = poly.settings;
	Pattern base = s.pattern;
	poly.mono.applyRewrites(&base);
	Pattern next = randomise(base, s.steps, lane, s.randomAmount, s.seed);
	if (next == base) {
		return; // Rand Amt at zero: no act, as a press with nothing to move is none
	}
	next.basis = takenBasis;
	// THROUGH THE ROW, so the copy the record gets back compares equal to this one to the bit.
	Row row {};
	writePattern(next, row.data());
	rolled = patternOf(row.data(), row.size());
	haveRolled = true;
	poly.settings.pattern = rolled;
	// THE REWRITES ARE IN `base`, and so in what was rolled: the machines start again from it, as after a press.
	poly.mono.clearRewrites();
	for (auto& n : poly.notes.notes) {
		n.shape.clearRewrites();
	}
	publishedRewrites = poly.mono.rewrites();
	serial = device::nextExtraStateSerial();
}

uint64_t Device::publish(float* extra, size_t capacity) const noexcept {
	if (serial == 0 || extra == nullptr) {
		return 0;
	}
	// THE ROLLED PATTERN WHILE ONE STANDS: `setFields` may have put the record's older one back since the last tick.
	Pattern p = haveRolled ? rolled : poly.settings.pattern;
	poly.mono.applyRewrites(&p);
	p.basis = takenBasis;
	Row row {};
	writePattern(p, row.data());
	const size_t n = std::min(capacity, kExtraFloats);
	for (size_t i = 0; i < n; ++i) {
		extra[i] = row[kParamCount + i];
	}
	return serial;
}

float Device::outputForNote(size_t output, int32_t noteId, int16_t channel, int16_t key) const noexcept {
	if (poly.poly()) {
		const size_t i = poly.notes.indexOfNote(noteId, channel, key);
		if (i < mod::PerNoteTable<Shape>::kMaxNotes) {
			return scaled(output, poly.notes.notes[i].shape.output(output));
		}
	}
	return scaled(output, poly.mono.output(output));
}

namespace {

Device& asDevice(void* storage) {
	return *static_cast<Device*>(storage);
}
const Device& asDevice(const void* storage) {
	return *static_cast<const Device*>(storage);
}

void setFieldsOf(void* storage, const float* fields, size_t fieldsPresent) {
	asDevice(storage).poly.settings = settingsOf(fields, fieldsPresent);
}

} // namespace

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	return index == 0 ? asDevice(storage).playSnapshot.read(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	if (index != 0 || n != kPlayWords) {
		return false;
	}
	device::BlockReader r(in, n);
	Device& d = asDevice(storage);
	if (!d.poly.mono.load(r)) {
		return false;
	}
	d.publishPlay();
	return true;
}

const device::ModOps kModOps {
	.stateBytes = sizeof(Device),
	.stateAlign = alignof(Device),
	.construct = [](void* storage) { new (storage) Device(); },
	.destruct = [](void* storage) { asDevice(storage).~Device(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asDevice(storage).reset(); },
	.setFields = setFieldsOf,
	.blockSamples = static_cast<size_t>(kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext& ctx) { asDevice(storage).tick(ctx); },
	.outputAt = [](const void* storage, size_t output) { return asDevice(storage).outputAt(output); },
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    return asDevice(storage).outputForNote(output, noteId, channel, key);
	    },
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const mod::Playhead where = asDevice(storage).poly.sharedShape().playhead();
		    *ordinal = where.ordinal;
		    *phase = where.phase;
	    },
	.trigFires = [](const void* storage) { return asDevice(storage).fireWord(); },
	.publishExtraState = [](const void* storage, float* extra, size_t capacity) {
		return asDevice(storage).publish(extra, capacity);
	},
	.extraStateBasis = static_cast<int>(kBasisFloat),
};

const device::NoteOps kNoteOps {
	.stateBytes = sizeof(Device),
	.stateAlign = alignof(Device),
	.construct = [](void* storage) { new (storage) Device(); },
	.destruct = [](void* storage) { asDevice(storage).~Device(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.setFields = setFieldsOf,
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) {
		    // EVERY NOTE'S PATTERN STARTS FRESH: reset, so its first tick starts it from the first step — on the dice of
		    // its own ordinal among the notes held with it (`playSeed`).
		    Device& d = asDevice(storage);
		    d.poly.processNotes(in, inCount, out, outCapacity, outCount, false,
		                        [&d](Shape& shape, const mod::PerNoteTable<Shape>::Note*) {
			                        const uint32_t ordinal = d.ordinalForNewNote(shape);
			                        shape.reset();
			                        shape.setOrdinal(ordinal);
		                        });
	    },
	.listensOnly = true,
};

static_assert(std::size(kOutputs) == 3, "Shape::output answers value, gate and trigger, and nothing else");

} // namespace feathers::mod::stepseq
