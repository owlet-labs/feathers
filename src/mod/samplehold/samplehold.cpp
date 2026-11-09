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

#include "samplehold.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <new>
#include <utility>

namespace feathers::mod::samplehold {

namespace {

[[nodiscard]] constexpr uint64_t mix(uint64_t a, uint64_t b) noexcept {
	uint64_t z = a + 0x9e3779b97f4a7c15ull * (b + 1);
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

/// THE CLOCK'S STREAM IS ANOTHER COURSE FROM THE SAME SEED, so the two never share draws.
constexpr uint64_t kEventsCourse = 0x5eed0fc10c4ull;

/// **FREE'S STREAMS COME FROM THE SEED TOO, ON A COURSE OF THEIR OWN** (backlog NM). It was a process-wide count of constructions, so a reload — a fresh
/// instance — played another stream, and how many holds a host had built before decided what this one played: a seed
/// from something incidental is a bug, not a seed. Free differs from Stable in that nothing RESTARTS it, not in where
/// it starts.
constexpr uint64_t kFreeCourse = 0xf2ee5eedc0115eull;

/// **A SYNCED FREE STEP ON A PLAYING TRANSPORT DRAWS FROM THE SONG'S STEP** — so bar 9 holds the same sample however
/// the song got there, and pressing play anywhere is deterministic by construction rather than by a saved position.
constexpr uint64_t kSongCourse = 0x50e65eed57e9ull;

[[nodiscard]] uint64_t baseOf(const Settings& s) noexcept {
	return s.seedMode == SeedMode::Stable ? static_cast<uint64_t>(s.seed)
	                                      : ~mix(static_cast<uint64_t>(s.seed), kFreeCourse);
}

} // namespace

double rateHz(const Settings& s, double secondsPerBeat) noexcept {
	if (s.synced() && secondsPerBeat > 0.0 && std::isfinite(secondsPerBeat)) {
		const size_t div = std::min(s.timeBase - 1, device::kDivisionCount - 1);
		const double seconds = device::kDivisionBeats[div] * timeSeconds(s.timeScale) * secondsPerBeat;
		return seconds > 0.0 ? 1.0 / seconds : 0.0;
	}
	return timeSeconds(s.timeScale);
}

//------------------------------------------------------------------------------------------------//
// Shape                                                                                           //
//------------------------------------------------------------------------------------------------//

void Shape::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	tickSeconds_ = static_cast<double>(kBlockSamples) / sampleRate_;
	// THE RANDOM CLOCK RUNS AT THE TICK RATE: one event is one tick, which is as fine as a held value can change.
	events_.setSampleRate(sampleRate_ / static_cast<double>(kBlockSamples));
	reset();
}

void Shape::reset() {
	ordinal_ = 0;
	reseedOwed_ = false;
	restart();
}

void Shape::start(uint64_t ordinal) {
	ordinal_ = ordinal;
	restart();
	reseedOwed_ = true;
}

void Shape::restart() noexcept {
	restartOwed_ = true;
	firePending_ = false;
	primed_ = false;
	phase_ = 0.0;
	lock_.release();
	relock_ = 0.0;
	haveStep_ = false;
	held_ = 0.0f;
	slewed_ = 0.0;
	pulse_ = false;
	taken_ = 0;
	clocks_ = 0;
}

void Shape::seedStreams(const Settings& s) {
	// STABLE FROM THE SEED AND THE ORDINAL; FREE FROM THE SEED'S OWN COURSE AND AN ORDINAL THAT NEVER REPEATS.
	const uint64_t course = mix(baseOf(s), ordinal_);
	seededSeed_ = s.seed;
	values_.seed(course);
	events_.seed(course ^ kEventsCourse);
	// THE COLOURS' STATE COMES FROM THE STREAM TOO, so a seed fixes the whole staircase from its first sample.
	for (double& row : pinkRows_) {
		row = values_.uniform();
	}
	pinkWhite_ = values_.uniform();
	pinkCount_ = 0;
	brown_ = values_.uniform();
	seeded_ = true;
}

double Shape::noise(const Settings& s, double uA, double uB) noexcept {
	// EVERY COLOUR ADVANCES ON EVERY CLOCK, so switching colour is a different view of one running noise rather than
	// a restart of it.
	++pinkCount_;
	const int row = std::min(std::countr_zero(pinkCount_), kPinkRows - 1);
	pinkRows_[static_cast<size_t>(row)] = uA;
	pinkWhite_ = uB;
	brown_ += (uB - 0.5) * kBrownStep;
	if (brown_ < 0.0) {
		brown_ = -brown_; // REFLECTED, not clamped: a clamp would park the walk on its end
	}
	if (brown_ > 1.0) {
		brown_ = 2.0 - brown_;
	}
	switch (s.source) {
	case Source::Pink: {
		double sum = pinkWhite_;
		for (const double r : pinkRows_) {
			sum += r;
		}
		const double mean = 0.5 * static_cast<double>(kPinkRows + 1);
		return std::clamp(0.5 + kPinkSpread * (sum - mean) / static_cast<double>(kPinkRows + 1), 0.0, 1.0);
	}
	case Source::Brown:
		return brown_;
	case Source::White:
	case Source::Input:
	case Source::kCount:
		break;
	}
	return uA;
}

void Shape::take(const Settings& s, bool first) {
	// **THREE DRAWS A CLOCK, ALWAYS, IN THIS ORDER**: Probability's, then the noise's two. So a Probability turned down
	// skips samples and leaves every taken one where it was, and a colour change cannot move a coin.
	const double coin = values_.uniform();
	const double uA = values_.uniform();
	const double uB = values_.uniform();
	const double n = noise(s, uA, uB);
	if (!first) {
		++clocks_;
		// `coin` is [0, 1): Probability 1 always takes and 0 never does.
		if (!(coin < static_cast<double>(s.probability))) {
			return;
		}
	}
	float v = 0.0f;
	if (s.source == Source::Input) {
		v = std::clamp(std::isfinite(input_) ? input_ : 0.0f, s.bipolar ? -1.0f : 0.0f, 1.0f);
	}
	else {
		v = s.bipolar ? static_cast<float>(2.0 * n - 1.0) : static_cast<float>(n);
	}
	held_ = quantized(v, s.quantize);
	pulse_ = true;
	++taken_;
	recent_[taken_ % device::kRecentEvents] = { taken_, s.bipolar ? 0.5f * (held_ + 1.0f) : held_ };
}

size_t Shape::recentEvents(device::RecentEvent* out, size_t capacity) const noexcept {
	// NEWEST FIRST, and only entries this run of `taken` wrote: a restart counts from zero again, so an entry whose
	// ordinal is not the one asked for is from before it, and ends the list.
	size_t n = 0;
	for (; n < capacity && n < device::kRecentEvents && n < taken_; ++n) {
		const uint32_t want = taken_ - static_cast<uint32_t>(n);
		const device::RecentEvent& e = recent_[want % device::kRecentEvents];
		if (e.ordinal != want) {
			break;
		}
		out[n] = e;
	}
	return n;
}

bool Shape::advanceRate(const Settings& s, const TransportClock& clock, float rateMul) {
	// **SYNCED ON A PLAYING TRANSPORT: THE SONG'S STEP** (the LFO's random walk's rule, `mod::TransportLock`), so the
	// staircase steps on the bar lines and a route into Rate does not scrub it. Stopped, it runs at the rate.
	if (s.synced() && clock.playing && clock.havePositionBeats() && clock.haveTempo()) {
		const size_t div = std::min(s.timeBase - 1, device::kDivisionCount - 1);
		const double stepBeats = device::kDivisionBeats[div] * timeSeconds(s.timeScale);
		if (!(stepBeats > 0.0) || !std::isfinite(stepBeats)) {
			return false;
		}
		const double walked = lock_.follow(clock.songPositionBeats, stepBeats, tickSeconds_ / clock.secondsPerBeat, 1);
		const double step = std::floor(walked);
		phase_ = walked - step;
		const bool crossed = haveStep_ && step != step_;
		step_ = step;
		haveStep_ = true;
		return crossed;
	}
	lock_.release();
	relock_ = 0.0;
	haveStep_ = false;
	const double increment = rateHz(s, clock.haveTempo() ? clock.secondsPerBeat : 0.0) * static_cast<double>(rateMul)
	                         * tickSeconds_;
	if (!std::isfinite(increment) || !(increment > 0.0)) {
		return false; // A RATE OF ZERO HOLDS
	}
	phase_ += increment;
	if (phase_ < 1.0) {
		return false;
	}
	// AT MOST ONE SAMPLE A TICK: a held value can change only once per tick, so two crossings are one.
	phase_ -= std::floor(phase_);
	return true;
}

float Shape::tick(const Settings& s, bool gate, bool retrigger, TransportClock clock, float rateMul) {
	(void)gate; // a hold has no gate: it holds through the release, for whatever voice is still sounding it
	bipolar_ = s.bipolar;
	rated_ = s.clock == Clock::Rate;
	if (std::exchange(restartOwed_, false)) {
		if (s.seedMode == SeedMode::Stable || !seeded_ || reseedOwed_) {
			seedStreams(s);
		}
		reseedOwed_ = false;
	}
	// A FREE STREAM WHOSE SEED MOVED goes on from the new Seed at once: the control is live under Free (backlog NM).
	if (s.seedMode == SeedMode::Free && seeded_ && s.seed != seededSeed_) {
		seedStreams(s);
	}
	pulse_ = false;
	bool clocked = false;
	switch (s.clock) {
	case Clock::Rate: {
		clocked = advanceRate(s, clock, rateMul);
		// **A TRIG RESTARTS RATE'S CLOCK HERE** — the next sample a whole step after the fire, as the sequencer's Advance
		// restarts its step. Synced on a playing transport that is a RE-LOCK: the song's step is counted from the fire
		// (`TransportLock::restartAt`), and the shift is kept until the lock ends, so the staircase steps on the fire's
		// grid rather than the bar's. The fire takes the tick's one sample, whatever the clock crossed.
		const bool refired = firePending_ && primed_;
		if (refired) {
			if (haveStep_) {
				relock_ += step_ + phase_;
				lock_.restartAt(0.0);
				step_ = 0.0;
			}
			phase_ = 0.0;
		}
		// SYNCED, FREE, ON A PLAYING TRANSPORT: this step's draws are the song step's (`kSongCourse`) — the song's own,
		// the fires' shifts added back, so a re-lock moves where the steps fall and not which draws they take. A fire's
		// sample takes the stream's next draws instead: the same song step drawn again would only hold what it holds.
		if (s.seedMode == SeedMode::Free && haveStep_ && !refired && (clocked || !primed_)) {
			const double song = relock_ == 0.0 ? step_ : std::floor(step_ + phase_ + relock_);
			values_.seed(mix(mix(baseOf(s), static_cast<uint64_t>(static_cast<int64_t>(song))), kSongCourse));
		}
		break;
	}
	case Clock::Note:
		// A NOTE'S START IS ITS FIRST SAMPLE (`primed_`); after it, a re-press — or under mono, every new note.
		clocked = retrigger;
		break;
	case Clock::Random: {
		dsp::RandomEventSettings events {};
		events.rateHz = rateHz(s, clock.haveTempo() ? clock.secondsPerBeat : 0.0) * static_cast<double>(rateMul);
		events.timing = dsp::EventTiming::Poisson;
		clocked = events_.next(events) != 0.0f;
		break;
	}
	case Clock::Trig:
	case Clock::kCount:
		break;
	}
	if (std::exchange(firePending_, false)) {
		clocked = true;
	}
	if (!primed_) {
		take(s, true);
		primed_ = true;
	}
	else if (clocked) {
		take(s, false);
	}
	// THE SLEW: the exact one-pole the modulators share, in double and snapped at rest (`mod::smoothingStep`). Off at
	// the bottom of the travel, where the coefficient is 1 and the output IS the hold.
	smoothingStep(slewed_, static_cast<double>(held_), smoothingCoeff(s.slew, tickSeconds_));
	return value();
}

Playhead Shape::playhead() const noexcept {
	Playhead p {};
	p.ordinal = std::min<uint32_t>(taken_, 0x7FFFFFFFu);
	p.phase = rated_ ? static_cast<float>(phase_) : 0.0f;
	// RAW 0..1: where the hold is GOING (before the slew), which the card's staircase is drawn from.
	p.level = bipolar_ ? 0.5f * (held_ + 1.0f) : held_;
	p.haveLevel = true;
	return p;
}

//------------------------------------------------------------------------------------------------//
// Device                                                                                          //
//------------------------------------------------------------------------------------------------//

void Shape::save(device::BlockWriter& w) const noexcept {
	w.u64(values_.state());
	device::putEvents(w, events_.position());
	w.flag(seeded_);
	w.flag(restartOwed_);
	w.flag(reseedOwed_);
	w.u64(ordinal_);
	w.u32(static_cast<uint32_t>(seededSeed_));
	w.flag(primed_);
	w.f64(phase_);
	for (const double row : pinkRows_) {
		w.f64(row);
	}
	w.f64(pinkWhite_);
	w.u64(pinkCount_);
	w.f64(brown_);
	w.f32(held_);
	w.f64(slewed_);
	w.u32(taken_);
	w.u32(clocks_);
}

bool Shape::load(device::BlockReader& r) noexcept {
	Shape s = *this;
	s.values_.setState(r.u64());
	s.events_.setPosition(device::takeEvents<dsp::RandomEvents::Position>(r));
	s.seeded_ = r.flag();
	s.restartOwed_ = r.flag();
	s.reseedOwed_ = r.flag();
	s.ordinal_ = r.u64();
	s.seededSeed_ = static_cast<int>(r.u32());
	s.primed_ = r.flag();
	s.phase_ = r.f64();
	for (double& row : s.pinkRows_) {
		row = r.f64();
	}
	s.pinkWhite_ = r.f64();
	s.pinkCount_ = r.u64();
	s.brown_ = r.f64();
	s.held_ = r.f32();
	s.slewed_ = r.f64();
	s.taken_ = r.u32();
	s.clocks_ = r.u32();
	if (!std::isfinite(s.phase_) || !std::isfinite(s.brown_) || !std::isfinite(s.held_) || !std::isfinite(s.slewed_)) {
		return false;
	}
	*this = s;
	return true;
}

void Device::saveHold(float* out, size_t capacity) const noexcept {
	device::BlockWriter w(out, capacity);
	mono.save(w);
	w.u64(notesSinceRestart);
	w.u64(notesEver);
}

bool Device::loadHold(const float* in, size_t n) noexcept {
	device::BlockReader r(in, n);
	Shape hold = mono;
	if (!hold.load(r)) {
		return false;
	}
	const uint64_t since = r.u64();
	const uint64_t ever = r.u64();
	if (!r.ok()) {
		return false;
	}
	mono = hold;
	notesSinceRestart = since;
	notesEver = ever;
	std::array<float, kHoldWords> words {};
	saveHold(words.data(), words.size());
	holdSnapshot.publish(words.data(), words.size());
	return true;
}

void Device::prepare(double sampleRate) {
	mono.prepare(sampleRate);
	notes.prepare(sampleRate);
}

void Device::reset() {
	mono.reset();
	notes.reset();
	trig.reset();
	notesSinceRestart = 0;
	wasPlaying = false;
}

void Device::tick(const device::ModTickContext& ctx) {
	// **STABLE RESTARTS WHEN THE TRANSPORT STARTS**, so pressing play from the same place plays the same staircase.
	const bool playing = ctx.clock.playing;
	if (settings.seedMode == SeedMode::Stable && playing && !wasPlaying) {
		mono.restart();
		notesSinceRestart = 0;
		for (auto& n : notes.notes) {
			if (n.used) {
				n.shape.restart();
			}
		}
	}
	wasPlaying = playing;

	const bool fired = trig.step(settings.trig);
	if (fired) {
		++trigFires;
		mono.fire();
	}
	mono.setInput(settings.input);
	if (poly()) {
		// **PER NOTE, THE TRIG AND THE INPUT** (`PolyShape::tick`'s rule, for two controls): a per-voice source reaching
		// either is read for the note's own voice; otherwise the rack's one value stands, and the device's edge fires.
		const device::ModTickContext::PerNoteResolver* r = ctx.perNote;
		const auto resolve = [r](size_t control, const PerNoteTable<Shape>::Note& n, float* out) {
			return r != nullptr && r->resolve != nullptr
			       && r->resolve(r->context, control, n.noteId, n.channel, n.key, out);
		};
		for (auto& n : notes.notes) {
			if (!n.used) {
				continue;
			}
			float own = 0.0f;
			const bool resolved = resolve(Settings::kTrigControl, n, &own);
			if (resolved ? n.trig.step(own) : fired) {
				n.shape.fire();
				trigFires += resolved ? 1u : 0u;
			}
			float in = settings.input;
			(void)resolve(Settings::kInputControl, n, &in);
			n.shape.setInput(in);
		}
	}
	mono.tick(settings, ctx.gate, ctx.retrigger, ctx.clock, ctx.rateMul);
	if (poly()) {
		for (auto& n : notes.notes) {
			if (!n.used) {
				continue;
			}
			n.shape.tick(settings, n.gate, n.retrigger, ctx.clock, ctx.rateMul);
			n.retrigger = false;
			if (!n.gate) {
				n.done = true; // still ticked and still holding; done only ranks the slot for reuse
			}
		}
	}
	// THE ONE HOLD, FOR THE PATCH (backlog NM): every tick, since the phase moves every tick — a copy into the snapshot,
	// read only before a save (`StateBlockDesc::harvestOnTick` is off).
	std::array<float, kHoldWords> words {};
	saveHold(words.data(), words.size());
	holdSnapshot.publish(words.data(), words.size());
}

const Shape& Device::sharedShape() const noexcept {
	if (poly()) {
		if (const auto* n = notes.newest(); n != nullptr) {
			return n->shape;
		}
	}
	return mono;
}

const Shape& Device::shapeForNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
	if (poly()) {
		const size_t at = notes.indexOfNote(noteId, channel, key);
		if (at < notes.notes.size()) {
			return notes.notes[at].shape;
		}
	}
	return mono;
}

float Device::outputOf(const Shape& shape, size_t output) const noexcept {
	switch (static_cast<Output>(output)) {
	case Output::Held:
		return shape.value() * std::clamp(settings.amount, -1.0f, 1.0f);
	case Output::TrigOut:
		// UNSMOOTHED AND UNSCALED (`mod::kTrigSmoothingRule`): one tick of exactly 1.
		return shape.pulse() ? 1.0f : 0.0f;
	case Output::kCount:
		break;
	}
	return 0.0f;
}

void Device::processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
                          size_t* outCount) noexcept {
	notes.processNotes(in, inCount, out, outCapacity, outCount, false, false,
	                   [this](Shape& shape, const PerNoteTable<Shape>::Note*) {
		                   const uint64_t ordinal =
		                       settings.seedMode == SeedMode::Stable ? notesSinceRestart : notesEver;
		                   ++notesSinceRestart;
		                   ++notesEver;
		                   shape.start(ordinal);
	                   });
}

//------------------------------------------------------------------------------------------------//
// Row <-> Settings                                                                                //
//------------------------------------------------------------------------------------------------//

Row rowOf(const Settings& s) {
	return Row {
		static_cast<float>(s.source),
		s.input,
		static_cast<float>(s.clock),
		static_cast<float>(s.timeBase),
		s.timeScale,
		s.slew,
		static_cast<float>(s.quantize),
		s.probability,
		static_cast<float>(s.seedMode),
		static_cast<float>(s.seed),
		s.bipolar ? 1.0f : 0.0f,
		s.poly ? 1.0f : 0.0f,
		s.amount,
		s.enabled ? 1.0f : 0.0f,
		s.trig,
	};
}

Settings settingsOf(const Row& row, size_t fieldsPresent) {
	const Settings d = defaults();
	const size_t fields = std::min(fieldsPresent, row.size());
	const auto read = [&](Param p, float fallback) {
		const auto i = static_cast<size_t>(p);
		return i < fields && std::isfinite(row[i]) ? row[i] : fallback;
	};
	const auto stepped = [&](Param p, int fallback, int max) {
		return std::clamp(static_cast<int>(std::lround(read(p, static_cast<float>(fallback)))), 0, max);
	};
	const auto flag = [&](Param p, bool fallback) { return read(p, fallback ? 1.0f : 0.0f) >= 0.5f; };
	const auto range = [](Param p) { return kParamDescs[static_cast<size_t>(p)]; };
	const auto clamped = [&](Param p, float fallback) {
		return std::clamp(read(p, fallback), static_cast<float>(range(p).min), static_cast<float>(range(p).max));
	};

	Settings s;
	s.source = static_cast<Source>(stepped(Param::Source, static_cast<int>(d.source), static_cast<int>(Source::kCount) - 1));
	s.input = clamped(Param::Input, d.input);
	s.clock = static_cast<Clock>(stepped(Param::Clock, static_cast<int>(d.clock), static_cast<int>(Clock::kCount) - 1));
	s.timeBase = static_cast<size_t>(stepped(Param::TimeBase, static_cast<int>(d.timeBase),
	                                         static_cast<int>(kTimeBaseCount) - 1));
	s.timeScale = clamped(Param::Rate, d.timeScale);
	s.slew = clamped(Param::Slew, d.slew);
	s.quantize = stepped(Param::Quantize, d.quantize, kQuantizeMax);
	s.probability = clamped(Param::Probability, d.probability);
	s.seedMode = static_cast<SeedMode>(stepped(Param::SeedMode, static_cast<int>(d.seedMode),
	                                           static_cast<int>(SeedMode::kCount) - 1));
	s.seed = stepped(Param::Seed, d.seed, 999);
	s.bipolar = flag(Param::Bipolar, d.bipolar);
	s.poly = flag(Param::Poly, d.poly);
	s.amount = clamped(Param::Amount, d.amount);
	s.enabled = flag(Param::Enabled, d.enabled);
	s.trig = clamped(Param::Trig, d.trig);
	return s;
}

//------------------------------------------------------------------------------------------------//
// The ops tables                                                                                  //
//------------------------------------------------------------------------------------------------//

namespace {

Device& asDevice(void* storage) {
	return *static_cast<Device*>(storage);
}

const Device& asDevice(const void* storage) {
	return *static_cast<const Device*>(storage);
}

void setFieldsOf(void* storage, const float* fields, size_t fieldsPresent) {
	Row row {};
	const size_t take = std::min(fieldsPresent, row.size());
	for (size_t i = 0; i < take; ++i) {
		row[i] = fields[i];
	}
	asDevice(storage).settings = settingsOf(row, fieldsPresent);
}

} // namespace

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	return index == 0 ? asDevice(storage).holdSnapshot.read(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 0 && n == kHoldWords && asDevice(storage).loadHold(in, n);
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
	.outputAt = [](const void* storage, size_t output) {
		const Device& d = asDevice(storage);
		return d.outputOf(d.sharedShape(), output);
	},
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    const Device& d = asDevice(storage);
		    return d.outputOf(d.shapeForNote(noteId, channel, key), output);
	    },
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const Playhead where = asDevice(storage).sharedShape().playhead();
		    *ordinal = where.ordinal;
		    *phase = where.phase;
	    },
	.playheadLevel = [](const void* storage) { return asDevice(storage).sharedShape().playhead().level; },
	.trigFires = [](const void* storage) { return asDevice(storage).trigFires; },
	.recentEvents = [](const void* storage, device::RecentEvent* out, size_t capacity) {
		return asDevice(storage).sharedShape().recentEvents(out, capacity);
	},
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
	       size_t* outCount) { asDevice(storage).processNotes(in, inCount, out, outCapacity, outCount); },
	.listensOnly = true,
};

} // namespace feathers::mod::samplehold
