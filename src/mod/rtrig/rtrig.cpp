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

#include "mod/rtrig/rtrig.h"
#include "mod/per_note.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <utility>

namespace feathers::mod::rtrig {

namespace {

/// A CONTROL'S VALUE, or its DECLARED default when the row does not carry it (device_row.h's rule).
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const size_t i = static_cast<size_t>(p);
	const float v = i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
	return std::isfinite(v) ? v : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] float rangedOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(fieldOr(row, fieldsPresent, p), static_cast<float>(d.min), static_cast<float>(d.max));
}

[[nodiscard]] int choiceOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto& d = kParamDescs[static_cast<size_t>(p)];
	return std::clamp(static_cast<int>(std::lround(fieldOr(row, fieldsPresent, p))), static_cast<int>(d.min),
	                  static_cast<int>(d.max));
}

/// **FREE'S STREAMS START FROM ONE FIXED SEED**, NOISE's reasoning (`noise.cpp`): what makes Free free is that nothing
/// restarts it, and seeding from the instance would make a patch render differently by how many were built before it.
constexpr uint64_t kFreeSeed = 0x5eedf4ee5eedf4eeull;

/// splitmix64, to turn a note's start count into a course change that shares nothing with its neighbour's.
[[nodiscard]] constexpr uint64_t scramble(uint64_t z) {
	z += 0x9e3779b97f4a7c15ull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	return z ^ (z >> 31);
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOr(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.density = rangedOr(row, fieldsPresent, Param::Density);
	s.sync = choiceOr(row, fieldsPresent, Param::Sync);
	s.timing = choiceOr(row, fieldsPresent, Param::Timing);
	s.jitter = rangedOr(row, fieldsPresent, Param::Jitter);
	s.cluster = rangedOr(row, fieldsPresent, Param::Cluster);
	s.hitSize = choiceOr(row, fieldsPresent, Param::HitSize);
	s.randomPolarity = choiceOr(row, fieldsPresent, Param::Polarity) == 1;
	s.decay = rangedOr(row, fieldsPresent, Param::Decay);
	s.seedMode = choiceOr(row, fieldsPresent, Param::SeedMode);
	s.seed = choiceOr(row, fieldsPresent, Param::Seed);
	s.poly = fieldOr(row, fieldsPresent, Param::Poly) >= 0.5f;
	s.trig = rangedOr(row, fieldsPresent, Param::Trig);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	const auto put = [&row](Param p, float v) { row[static_cast<size_t>(p)] = v; };
	put(Param::Enabled, s.enabled ? 1.0f : 0.0f);
	put(Param::Density, s.density);
	put(Param::Sync, static_cast<float>(s.sync));
	put(Param::Timing, static_cast<float>(s.timing));
	put(Param::Jitter, s.jitter);
	put(Param::Cluster, s.cluster);
	put(Param::HitSize, static_cast<float>(s.hitSize));
	put(Param::Polarity, s.randomPolarity ? 1.0f : 0.0f);
	put(Param::Decay, s.decay);
	put(Param::SeedMode, static_cast<float>(s.seedMode));
	put(Param::Seed, static_cast<float>(s.seed));
	put(Param::Poly, s.poly ? 1.0f : 0.0f);
	put(Param::Trig, s.trig);
	return row;
}

double hitsPerSecond(const Settings& s, double secondsPerBeat) {
	if (s.sync <= 0 || static_cast<size_t>(s.sync) > device::kDivisionCount) {
		return std::exp2(static_cast<double>(s.density));
	}
	const double spb = secondsPerBeat > 0.0 ? secondsPerBeat : kFallbackSecondsPerBeat;
	return 1.0 / (device::kDivisionBeats[static_cast<size_t>(s.sync - 1)] * spb);
}

dsp::RandomEventSettings eventSettingsOf(const Settings& s, double secondsPerBeat) {
	dsp::RandomEventSettings e;
	e.rateHz = hitsPerSecond(s, secondsPerBeat);
	e.timing = static_cast<dsp::EventTiming>(s.timing);
	e.jitter = s.jitter;
	e.cluster = s.cluster;
	e.size = static_cast<dsp::HitSize>(s.hitSize);
	e.randomPolarity = s.randomPolarity;
	return e;
}

double decayFactor(float decayLog2, double tickSeconds) {
	// THE BOTTOM OF THE TRAVEL IS "NO TIME" (`timeSeconds`): the hit's value stands for its own tick and is gone at
	// the next.
	const double seconds = timeSeconds(decayLog2);
	if (!(seconds > 0.0) || !(tickSeconds > 0.0)) {
		return 0.0;
	}
	// -60 dB AT `seconds`: 10^(-3 tick / seconds) per tick.
	return std::pow(10.0, kDecayFloorDb / 20.0 * tickSeconds / seconds);
}

//------------------------------------------------------------------------------------------------//
// ONE STREAM                                                                                       //
//------------------------------------------------------------------------------------------------//

void Shape::prepare(double sampleRate) noexcept {
	events_.setSampleRate(sampleRate > 0.0 ? sampleRate : 48000.0);
	reset();
}

void Shape::reset() noexcept {
	trig_ = false;
	size_ = 0.0f;
	decay_.reset();
	hits_ = 0;
	lastOffset_ = -1;
	hitsLastTick_ = 0;
	firePending_ = false;
}

void Shape::start(uint64_t seed) noexcept {
	events_.seed(seed);
	fire_.seed(fireSeed(seed));
}

void Shape::tick(const TickSettings& s, bool, bool retrigger, TransportClock, float) noexcept {
	// A RE-PRESS UNDER THIS NOTE'S HELD KEY is a new start, which under Stable is the pattern from its top.
	if (retrigger && s.stable) {
		start(s.stableSeed);
	}
	// THE DECAY FALLS FIRST (`HitDecay::fall`), so a hit in this tick stands at its full value for the tick it lands in.
	decay_.fall(s.decay);
	trig_ = false;
	lastOffset_ = -1;
	hitsLastTick_ = 0;
	// A FIRED HIT (the Trig) leads the tick's own, so a generator hit later in the same tick is still the newest.
	if (std::exchange(firePending_, false)) {
		const float v = fire_.next(s.events);
		trig_ = true;
		size_ = v;
		decay_.hit(v);
		record(v);
		++hitsLastTick_;
		lastOffset_ = 0;
	}
	// ONE SAMPLE AT A TIME, exactly as NOISE's left ear draws them, so the hits land on the same samples.
	for (int i = 0; i < kBlockSamples; ++i) {
		const float v = events_.next(s.events);
		if (v != 0.0f) {
			trig_ = true;
			size_ = v;
			decay_.hit(v);
			record(v);
			++hitsLastTick_;
			lastOffset_ = i;
		}
	}
}

void Shape::record(float size) noexcept {
	++hits_;
	recent_[hits_ % device::kRecentEvents] = { hits_, size };
}

size_t Shape::recentEvents(device::RecentEvent* out, size_t capacity) const noexcept {
	// NEWEST FIRST, and only entries this run of `hits` wrote (a reset counts from zero again): S&H's rule.
	size_t n = 0;
	for (; n < capacity && n < device::kRecentEvents && n < hits_; ++n) {
		const uint32_t want = hits_ - static_cast<uint32_t>(n);
		const device::RecentEvent& e = recent_[want % device::kRecentEvents];
		if (e.ordinal != want) {
			break;
		}
		out[n] = e;
	}
	return n;
}

void Shape::save(device::BlockWriter& w) const noexcept {
	device::putEvents(w, events_.position());
	w.u64(fire_.state());
	w.f32(size_);
	w.f32(decay_.value());
	w.u32(hits_);
}

bool Shape::load(device::BlockReader& r) noexcept {
	const dsp::RandomEvents::Position events = device::takeEvents<dsp::RandomEvents::Position>(r);
	const uint64_t fireState = r.u64();
	const float size = r.f32();
	const float decay = r.f32();
	const uint32_t hits = r.u32();
	if (!std::isfinite(size) || !std::isfinite(decay) || fireState == 0) {
		return false;
	}
	events_.setPosition(events);
	fire_.setState(fireState);
	size_ = size;
	decay_.hit(decay);
	hits_ = hits;
	return true;
}


float Shape::output(Output o) const noexcept {
	switch (o) {
	case Output::Trig: return trig_ ? 1.0f : 0.0f;
	case Output::Size: return size_;
	case Output::Decay: return decay_.value();
	case Output::kCount: break;
	}
	return 0.0f;
}

//------------------------------------------------------------------------------------------------//
// THE DEVICE — which streams exist, and when they start                                           //
//------------------------------------------------------------------------------------------------//

namespace {

struct OpsTrig {
	Settings settings {};
	TickSettings tick {};
	/// THE GLOBAL STREAM — what every destination reads under global, and under Poly the answer for a note the device
	/// never heard start (`mod::PolyShape::mono`'s reason).
	Shape global;
	PerNoteTable<Shape> notes;
	double tickSeconds = static_cast<double>(kBlockSamples) / 48000.0;
	/// WHAT THE STREAMS WERE LAST SEEDED FROM: the Stable Seed, or -1 for Free's.
	int seededWith = -2;
	/// HOW MANY NOTES HAVE STARTED UNDER FREE — folded into each one's course, so a chord's notes differ.
	uint64_t starts = 0;
	/// THE TRIG'S EDGE (backlog MV), the device's own; each note carries one too (`PerNoteTable::Note::trig`).
	TrigEdge edge;
	/// HOW MANY TIMES THE TRIG HAS FIRED — what lights the card's pad (`ModOps::trigFires`).
	uint32_t fires = 0;
	/// THE GLOBAL STREAM AS LAST PUBLISHED, for the patch (`kStateOps`; backlog NM): written after every tick.
	device::BlockSnapshot<kStreamWords> streamSnapshot;

	void saveStream(float* out, size_t capacity) const noexcept {
		device::BlockWriter w(out, capacity);
		global.save(w);
		w.u32(static_cast<uint32_t>(seededWith));
		w.u64(starts);
	}
	/// RESTORED WITH WHAT IT WAS SEEDED FROM, so the row the drain hands it next (the same Seed) reseeds nothing.
	[[nodiscard]] bool loadStream(const float* in, size_t n) noexcept {
		device::BlockReader r(in, n);
		Shape stream = global;
		if (!stream.load(r)) {
			return false;
		}
		const int seeded = static_cast<int>(r.u32());
		const uint64_t started = r.u64();
		if (!r.ok()) {
			return false;
		}
		global = stream;
		seededWith = seeded;
		starts = started;
		publishStream();
		return true;
	}
	void publishStream() noexcept {
		std::array<float, kStreamWords> words {};
		saveStream(words.data(), words.size());
		streamSnapshot.publish(words.data(), words.size());
	}

	[[nodiscard]] bool stable() const noexcept { return settings.seedMode == static_cast<int>(SeedMode::Stable); }
	[[nodiscard]] uint64_t stableSeed() const noexcept { return eventStreamSeed(static_cast<uint64_t>(settings.seed)); }

	/// EVERY STREAM FROM THE TOP — the Stable Seed's, or Free's fixed one (each note turned onto its own course).
	void reseedAll() {
		const uint64_t seed = stable() ? stableSeed() : kFreeSeed;
		global.start(seed);
		uint64_t k = 0;
		for (auto& n : notes.notes) {
			n.shape.start(seed);
			if (!stable()) {
				n.shape.mix(scramble(++k));
			}
		}
		seededWith = stable() ? settings.seed : -1;
	}

	/// THE STABLE SEED MOVED, OR THE MODE DID: the pattern starts again from it now (NOISE's `reseedIfMoved`).
	void reseedIfMoved() {
		if ((stable() && settings.seed != seededWith) || (!stable() && seededWith != -1)) {
			reseedAll();
		}
	}

	void prepare(double sampleRate) {
		const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
		tickSeconds = static_cast<double>(kBlockSamples) / sr;
		global.prepare(sr);
		notes.prepare(sr);
		reseedAll();
	}

	void reset() {
		global.reset();
		edge.reset();
		notes.reset();
		reseedAll();
	}

	void setFields(const float* fields, size_t fieldsPresent) {
		Row row {};
		const size_t take = std::min(fieldsPresent, row.size());
		for (size_t i = 0; i < take; ++i) {
			row[i] = fields[i];
		}
		settings = stateOf(row, take);
		reseedIfMoved();
	}

	void advance(const device::ModTickContext& ctx) {
		const double spb = ctx.clock.haveTempo() ? ctx.clock.secondsPerBeat : 0.0;
		tick.events = eventSettingsOf(settings, spb);
		// THE HOST'S RATE SCALE (`ModTickContext::rateMul`), on the mean rate as on every modulator's.
		if (std::isfinite(ctx.rateMul) && ctx.rateMul > 0.0f) {
			tick.events.rateHz *= static_cast<double>(ctx.rateMul);
		}
		tick.decay = decayFactor(settings.decay, tickSeconds);
		tick.stable = stable();
		tick.stableSeed = stableSeed();
		// UNDER GLOBAL, STABLE STARTS AGAIN AT EVERY NOTE — "the same pattern from the Seed at every note".
		if (!settings.poly && ctx.retrigger && tick.stable) {
			global.start(tick.stableSeed);
		}
		// THE TRIG, READ BEFORE THE STREAMS TICK (`mod::PolyShape::tick`'s order and rule): the device's edge fires the
		// global stream and every note the host cannot resolve for; a note a per-voice source reaches fires on its own.
		const bool fired = edge.step(settings.trig);
		if (fired) {
			++fires;
			global.fire();
		}
		if (settings.poly) {
			const device::ModTickContext::PerNoteResolver* r = ctx.perNote;
			for (auto& n : notes.notes) {
				if (!n.used || n.done) {
					continue;
				}
				float own = 0.0f;
				const bool resolved = r != nullptr && r->resolve != nullptr
				                      && r->resolve(r->context, Settings::kTrigControl, n.noteId, n.channel, n.key, &own);
				if (resolved ? n.trig.step(own) : fired) {
					n.shape.fire();
					fires += resolved ? 1u : 0u;
				}
			}
		}
		global.tick(tick, ctx.gate, false, ctx.clock, ctx.rateMul);
		if (settings.poly) {
			notes.tick(tick, ctx);
		}
		// THE GLOBAL STREAM, FOR THE PATCH: every tick, since the generator moves every tick — read only before a save.
		publishStream();
	}

	[[nodiscard]] const Shape& shared() const noexcept {
		if (settings.poly) {
			if (const auto* n = notes.newest(); n != nullptr) {
				return n->shape;
			}
		}
		return global;
	}

	[[nodiscard]] const Shape* forNote(int32_t noteId, int16_t channel, int16_t key) const noexcept {
		if (!settings.poly) {
			return nullptr;
		}
		const size_t at = notes.indexOfNote(noteId, channel, key);
		return at < notes.notes.size() ? &notes.notes[at].shape : nullptr;
	}
};

OpsTrig& asTrig(void* storage) {
	return *static_cast<OpsTrig*>(storage);
}
const OpsTrig& asTrig(const void* storage) {
	return *static_cast<const OpsTrig*>(storage);
}

[[nodiscard]] Output outputOf(size_t output) {
	return output < static_cast<size_t>(Output::kCount) ? static_cast<Output>(output) : Output::kCount;
}

} // namespace

const Shape& sharedShapeForTest(const void* storage) {
	return asTrig(storage).shared();
}

const Shape* noteShapeForTest(const void* storage, int32_t noteId, int16_t channel, int16_t key) {
	return asTrig(storage).forNote(noteId, channel, key);
}

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	return index == 0 ? asTrig(storage).streamSnapshot.read(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 0 && n == kStreamWords && asTrig(storage).loadStream(in, n);
}

const device::ModOps kModOps {
	.stateBytes = sizeof(OpsTrig),
	.stateAlign = alignof(OpsTrig),
	.construct =
	    [](void* storage) {
		    new (storage) OpsTrig();
		    asTrig(storage).reseedAll();
	    },
	.destruct = [](void* storage) { asTrig(storage).~OpsTrig(); },
	.prepare = [](void* storage, double sampleRate) { asTrig(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asTrig(storage).reset(); },
	.setFields = [](void* storage, const float* fields, size_t fieldsPresent) { asTrig(storage).setFields(fields, fieldsPresent); },
	.blockSamples = static_cast<size_t>(kBlockSamples),
	.tick = [](void* storage, const device::ModTickContext& ctx) { asTrig(storage).advance(ctx); },
	.outputAt = [](const void* storage, size_t output) { return asTrig(storage).shared().output(outputOf(output)); },
	.outputForNote =
	    [](const void* storage, size_t output, int32_t noteId, int16_t channel, int16_t key) {
		    const OpsTrig& t = asTrig(storage);
		    const Shape* s = t.forNote(noteId, channel, key);
		    return (s != nullptr ? *s : t.global).output(outputOf(output));
	    },
	// **THE CARD'S READING, ON THE PLAYHEAD'S ROAD**: the ordinal is the hit count (so a card polling at 60 Hz sees
	// every tick that hit, as a count that moved) and the phase the newest hit's size, -1..1 mapped onto 0..1 — one
	// word, so the two cannot come from different ticks. The count is kept under the idle ordinal.
	.playhead =
	    [](const void* storage, uint32_t* ordinal, float* phase) {
		    const Shape& s = asTrig(storage).shared();
		    *ordinal = s.hits() & 0x7FFFFFFFu;
		    *phase = 0.5f * (std::clamp(s.size(), -1.0f, 1.0f) + 1.0f);
	    },
	// AND THE DECAY OUTPUT'S MAGNITUDE BESIDE IT, for the card's falling line.
	.playheadLevel = [](const void* storage) { return std::fabs(asTrig(storage).shared().decayValue()); },
	.trigFires = [](const void* storage) { return asTrig(storage).fires; },
	.recentEvents = [](const void* storage, device::RecentEvent* out, size_t capacity) {
		return asTrig(storage).shared().recentEvents(out, capacity);
	},
};

const device::NoteOps kNoteOps {
	.stateBytes = sizeof(OpsTrig),
	.stateAlign = alignof(OpsTrig),
	.construct =
	    [](void* storage) {
		    new (storage) OpsTrig();
		    asTrig(storage).reseedAll();
	    },
	.destruct = [](void* storage) { asTrig(storage).~OpsTrig(); },
	.prepare = [](void* storage, double sampleRate) { asTrig(storage).prepare(sampleRate); },
	.setFields = [](void* storage, const float* fields, size_t fieldsPresent) { asTrig(storage).setFields(fields, fieldsPresent); },
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) {
		    OpsTrig& t = asTrig(storage);
		    // A NOTE'S STREAM OPENS AT ITS ON: Stable from the Seed's top, Free turned onto a course of its own.
		    t.notes.processNotes(in, inCount, out, outCapacity, outCount, false, false,
		                         [&t](Shape& shape, const PerNoteTable<Shape>::Note*) {
			                         shape.reset();
			                         if (t.stable()) {
				                         shape.start(t.stableSeed());
			                         }
			                         else {
				                         shape.mix(scramble(kFreeSeed ^ ++t.starts));
			                         }
		                         });
	    },
	.listensOnly = true,
};

} // namespace feathers::mod::rtrig
