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

#include "mod/arp/pattern_device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

namespace feathers::mod::arp::pat {

namespace {

int64_t floorDiv(int64_t a, int64_t b) noexcept {
	const int64_t q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// WHERE A SHARE `gate` OF STEPS FROM SLOT g ENDS, on the grid as it is (swing and nudge included): the whole steps
/// first, then the fraction of the step it ends in.
double gateEnd(const StepEmitter& e, int64_t g, double gate) noexcept {
	const double whole = std::floor(gate);
	const auto i = static_cast<int64_t>(whole);
	const double from = e.onset(g + i);
	return from + (gate - whole) * (e.onset(g + i + 1) - from);
}

} // namespace

void PatternDevice::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : 48000.0;
	em_.prepare(sampleRate_);
}

void PatternDevice::reset() noexcept {
	em_.reset();
	cycleHave_ = false;
	cycleStopped_ = false;
	armedFill_ = kPatterns;
	activeFill_ = kPatterns;
	selectorFill_ = false;
	modPattern_ = kPatterns;
	modOut_.fill(0.0f);
}

ClockSettings PatternDevice::clockOf(size_t p) const noexcept {
	ClockSettings c;
	c.enabled = s_.enabled;
	c.latch = s_.latch;
	c.run = s_.run;
	c.grid = StepGrid { static_cast<double>(s_.nudge), 0.0 };
	// HUMANIZE MOVES SLOTS THROUGH THE GRID (design §12), so a scrub finds every onset where it played.
	if (s_.feel.mode != humanize::Mode::Straight || s_.feel.wobble != 0.0) { // (Wobble moves slots under every feel)
		c.grid.shift = &PatternDevice::shiftOf;
		c.grid.shiftCtx = this;
	}
	const PatternSettings& q = s_.patterns[std::min(p, kPatterns - 1)];
	// THE CLASSIC ARP'S RATE LAW, read through its own functions so the two arps' steps cannot disagree.
	arp::Settings rate;
	rate.timeBase = q.timeBase;
	rate.rate = q.rate;
	c.synced = rate.synced();
	c.stepBeats = stepBeats(rate);
	c.stepHz = stepHz(rate);
	// THRU AND TRIG (design §7.5), the playing pattern's. In Drum the kit's eight notes pass — enabled lanes or not — and
	// any other key is a trigger; in a pitched Mode every key passes, so nothing triggers.
	c.thru = thruOn(q);
	c.trig = trigOf(q);
	if (c.thru) {
		if (q.mode == Mode::Drum) {
			for (const int16_t note : q.drum.note) {
				if (note >= 0 && note < 128) {
					c.thruKeys[static_cast<size_t>(note) >> 6] |= uint64_t { 1 } << (note & 63);
				}
			}
		}
		else {
			c.thruKeys = { ~uint64_t { 0 }, ~uint64_t { 0 } };
		}
	}
	return c;
}

void PatternDevice::setFields(const float* fields, size_t count) noexcept {
	// **DECODED ONLY WHEN THE ROW MOVED** (the idle-overhead pass, 2026-11-05). `settingsOf` is a pure function of the
	// fields it reads, and the rack hands this row on every resolve — MEASURED about 85 times a 1024-frame block on a large
	// patch, 233 fields each, at rest. The same bits decode to the same settings, so a row equal to the last one decoded,
	// compared as bits, keeps `s_` (written nowhere else). The rest of this function still runs every call: it reads
	// `playing_`, which moves on its own. The bits compared are the WHOLE row (`kParamCount`), Cycle's Play Mode, Repeats
	// and Next among them (design §7.6).
	const size_t kept = std::min(count, kParamCount);
	if (fields == nullptr || count != decodedCount_ || std::memcmp(fields, decoded_.data(), kept * sizeof(float)) != 0) {
		const Settings before = s_;
		s_ = settingsOf(fields, count);
		++decodes_;
		decodedCount_ = fields == nullptr ? SIZE_MAX : count;
		if (fields != nullptr) {
			std::memcpy(decoded_.data(), fields, kept * sizeof(float));
		}
		if (!(s_.patterns == before.patterns) || s_.keys != before.keys || s_.velocity != before.velocity
		    || s_.fixedVelocity != before.fixedVelocity) {
			cycleLengthDirty_ = true; // a Progression's length is its decode's
		}
		// **THE FILL TRIGGERS** (design §7.8): a rise arms that fill, if it is a fill and none is armed or sounding (a trigger
		// then is ignored: one fill at a time). The edge sees every row that moved, so a rise between two rows is not lost.
		for (size_t p = 0; p < kPatterns; ++p) {
			if (fillEdge_[p].step(s_.fillTrigger[p]) && s_.phase[p].fill && armedFill_ == kPatterns && activeFill_ == kPatterns) {
				armedFill_ = p;
			}
		}
		// AN ARMED TAB THAT IS NO LONGER A FILL drops its arm: it would otherwise fire as a fill it is not, and its Go (shown only
		// on a fill) would be left pending with no way to see it.
		if (armedFill_ < kPatterns && !s_.phase[armedFill_].fill) {
			armedFill_ = kPatterns;
		}
	}
	// NOTHING PLAYING YET: take the selection now — there is no onset to wait for. Otherwise the pattern playing keeps
	// its grid until its next onset, where `play` switches (design §7.3). On Cycle, the walk's first pattern: the first
	// slot played asks the walk, and moves the grid if the position is another's.
	if (playing_ == kPatterns) {
		playing_ = s_.playMode == PlayMode::Cycle ? cycleFirst() : selected(s_);
	}
	em_.setSettings(clockOf(playing_));
}

genes::Place PatternDevice::placeOf(const PatternSettings& q, const SliceResidue<genes::kRowCount>& residue) noexcept {
	// `sliceHome` (dsp/slice_build.h), restated so this file need not include the character layer.
	const double home = static_cast<double>(q.zone) / 1023.0 + static_cast<double>(q.zoneOffset) + q.gamma;
	// THE PLACE IS THE TRAVEL (the orbit, Radius and Theta, removed 2026-11-05).
	const double tx = static_cast<double>(q.placeX);
	const double ty = static_cast<double>(q.placeY);
	const SliceBasis<genes::kRowCount> basis = sliceFrameFor<genes::kRowCount>(static_cast<double>(q.rotate), home, residue);
	genes::Place place;
	for (size_t i = 0; i < genes::kRowCount; ++i) {
		place.phase[i] = basis.phaseFor(i, home * genes::kRows[i].phi, tx, ty);
		place.rate[i] = static_cast<double>(basis.rateFor(i, tx, ty)) - 1.0;
	}
	return place;
}

pattern::Recipe recipeFor(const Settings& s, size_t p, const std::array<bool, genes::kPinCount>& pinOn) noexcept {
	const PatternSettings& q = s.patterns[std::min(p, kPatterns - 1)];
	pattern::Recipe r;
	r.mode = q.mode;
	r.keys = s.keys;
	r.root = q.root;
	r.scale = q.scale;
	r.degree = q.degree;
	r.size = q.size;
	r.inversion = q.inversion;
	r.spread = q.spread;
	r.length = q.length;
	r.gate = q.gate;
	r.velocity = s.velocity;
	r.fixedVelocity = s.fixedVelocity;
	r.accent = q.accent;
	r.pitchOnNotes = q.pitchOnNotes;
	r.pins.amount = q.pins;
	r.pins.on = pinOn;
	r.drum = q.drum;
	// PROGRESSION COUNTS ITS CHANGES IN BARS (design §11-quater): the step's length in beats, by the classic Arp's rate law.
	arp::Settings rate;
	rate.timeBase = q.timeBase;
	rate.rate = q.rate;
	r.stepBeats = rate.synced() ? stepBeats(rate) : 0.0;
	return r;
}

pattern::Recipe PatternDevice::recipeOf(size_t p) const noexcept {
	return recipeFor(s_, p, handDecoded_[p].pinOn);
}

void PatternDevice::rebuildIfStale(size_t p, const HeldChord& chord) noexcept {
	Built& b = built_[p];
	const PatternSettings& q = s_.patterns[p];
	// THE LIKES' WARP, if it is on and one has been published: the buffer named before it is read, released on the way out.
	const genes::Warp* warp = nullptr;
	uint64_t warpSerial = 0;
	WarpBank* bank = s_.likesWarp ? warpBank_.load(std::memory_order_acquire) : nullptr;
	if (bank != nullptr) {
		uint32_t idx = 0;
		do {
			idx = warpFront_.load(std::memory_order_seq_cst);
			warpReading_.store(idx, std::memory_order_seq_cst);
		} while (warpFront_.load(std::memory_order_seq_cst) != idx);
		const WarpBuffer& wb = bank->buffer[idx];
		warpSerial = wb.serial;
		warp = wb.live[p] ? &wb.warp[p] : nullptr;
	}
	struct Release {
		std::atomic<uint32_t>* reading;
		~Release() {
			if (reading != nullptr) {
				reading->store(kNotReading, std::memory_order_seq_cst);
			}
		}
	} release { bank != nullptr ? &warpReading_ : nullptr };
	if (b.valid && b.from == q && b.chord == chord && b.handSerial == handCopy_.serial && b.editsSerial == editsCopy_.serial && b.keys == s_.keys
	    && b.fixedVelocity == s_.fixedVelocity && b.velocity == s_.velocity && b.warpSerial == warpSerial) {
		return;
	}
	pattern::Recipe recipe = recipeOf(p);
	recipe.warp = warp;
	pattern::build(placeOf(q, handDecoded_[p].residue), recipe, chord, b.cycle);
	b.warpSerial = warpSerial;
	b.from = q;
	b.chord = chord;
	b.handSerial = handCopy_.serial;
	b.editsSerial = editsCopy_.serial;
	// THE HAND'S EDITS, over the steps the terrain still decodes as they were made over (design §15-bis). Made under
	// another Mode, they are not this cycle's.
	if (editsDecoded_[p].mode == q.mode) {
		pattern::applyEdits(editsDecoded_[p].steps, genes::decodesDrum(q.mode), b.cycle, genes::isModulation(q.mode));
	}
	// MODULATION'S LEVELS (design §7.10), from the cycle as it plays (the edits applied).
	if (genes::isModulation(q.mode)) {
		modLevelsOf(b.cycle, q.drum.on, b.levels, &b.live);
	}
	else {
		b.live = 0;
	}
	b.keys = s_.keys;
	b.fixedVelocity = s_.fixedVelocity;
	b.velocity = s_.velocity;
	b.valid = true;
}

size_t PatternDevice::emit(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
                           size_t capacity, uint32_t* samplesUntilNext) noexcept {
	sampleNow_ = sampleNow;
	// THE PLAYHEAD GOES IDLE when nothing is held, or when the clock has not reached a step for two steps' time.
	const double stepSamples = stepSecondsNow_ * sampleRate_;
	playheadIdle_ = em_.chord().empty() || playheadOrdinal_ == 0xFFFFFFFFu
	                || static_cast<double>(sampleNow - lastPlaySample_) > 2.0 * std::max(1.0, stepSamples);
	// THE HAND'S BLOCK, pulled once a call: one bounded attempt, the last good copy kept on a torn read.
	if (handCopy_.pull(hand_, handScratch_)) {
		decodeHand();
	}
	if (editsCopy_.pull(edits_, editsScratch_)) {
		editsDecoded_ = editStateOf(editsCopy_.values.data(), editsCopy_.values.size());
	}
	// THE STEP'S LENGTH, for Humanize: the same law the emitter times the grid by.
	const ClockSettings& c = em_.settings();
	if (clock.haveTempo() && clock.secondsPerBeat > 0.0 && std::isfinite(clock.secondsPerBeat)) {
		spbNow_ = clock.secondsPerBeat;
	}
	regridded_ = false;
	if (c.synced && clock.haveTempo()) {
		stepBeatsNow_ = c.stepBeats;
		stepSecondsNow_ = c.stepBeats * clock.secondsPerBeat;
	}
	else {
		stepBeatsNow_ = 0.0;
		stepSecondsNow_ = c.stepHz > 0.0 ? 1.0 / c.stepHz : 1.0;
	}
	return em_.emit(clock, sampleNow, out, capacity, samplesUntilNext, &PatternDevice::playSlot, this,
	                &PatternDevice::betweenSlots);
}

void PatternDevice::betweenSlots(void* self, double u, double sps, StepEmitter& e) noexcept {
	auto& d = *static_cast<PatternDevice*>(self);
	if (d.regridded_) {
		d.regridded_ = false;
		return; // CYCLE MOVED THE GRID IN THIS CALL: `u` is the old grid's; the next call (one sample on) is the new one's
	}
	d.bendNow(u, sps, e);
	d.sweepNow(u, e);
	d.modNow(u, e);
}

void PatternDevice::sweepNow(double u, const StepEmitter& e) noexcept {
	sweepOut_ = 0.0f;
	if (playing_ >= kPatterns || !built_[playing_].valid) {
		return;
	}
	const pattern::PatternCycle& cycle = built_[playing_].cycle;
	if (!cycle.playable || cycle.length == 0) {
		return;
	}
	const auto length = static_cast<int64_t>(cycle.length);
	double sum = 0.0;
	int found = 0;
	const auto now = static_cast<int64_t>(std::floor(u));
	for (int64_t g = now + 1; g > now - 64 && found < kAccentSweepDepth; --g) {
		const double onset = e.onset(g);
		if (onset > u) {
			continue;
		}
		const int64_t step = g - static_cast<int64_t>(s_.offset);
		const auto k = static_cast<size_t>(step - floorDiv(step, length) * length);
		const pattern::PatternStep& st = cycle.steps[k];
		if (st.kind == pattern::PatternStep::Kind::Note && st.accent) {
			sum += kAccentSweepCharge * std::exp(-(u - onset) * stepSecondsNow_ / kAccentSweepSeconds);
			++found;
		}
	}
	sweepOut_ = static_cast<float>(std::min(1.0, sum));
}

double PatternDevice::Bend::value(double u) const noexcept {
	if (u <= startU) {
		return from;
	}
	// THE 303'S LAG (design §11.2-bis): exponential in semitones, a fixed time constant whatever the interval.
	const double seconds = (u - startU) * stepSeconds;
	return target + (from - target) * std::exp(-seconds / tauSeconds);
}

void PatternDevice::bendNow(double u, double sps, StepEmitter& e) noexcept {
	if (!bend_.active) {
		return;
	}
	// THE NOTE IS THE BEND'S: once it has ended (its off written, a stop, a switch-off, a jump), nothing more is sent —
	// and no reset is sent either, because the next note starts at zero (design §11.3).
	if (!e.sounding(bend_.key, bend_.channel) || u >= bend_.endU) {
		bend_.active = false;
		return;
	}
	if (u < bend_.startU || bend_.target == bend_.from) {
		return;
	}
	const double every = static_cast<double>(kBendEventSamples) / sps;
	const double v = bend_.value(u);
	const bool settled = std::abs(v - bend_.target) <= 0.005;
	if (u + 1e-9 >= bend_.sentU + every || (settled && bend_.sentValue != bend_.target)) {
		device::NoteEvent ev {};
		ev.kind = device::NoteEvent::Kind::Expression;
		ev.noteId = -1; // the held note was invented: addressed by its key, CLAP's wildcard
		ev.channel = bend_.channel;
		ev.key = bend_.key;
		ev.expressionId = kTuningExpression;
		ev.expressionValue = settled ? bend_.target : v;
		if (e.pushEvent(ev)) {
			bend_.sentU = u;
			bend_.sentValue = ev.expressionValue;
		}
	}
	if (bend_.sentValue != bend_.target) {
		const double toNext = (bend_.sentU + every - u) * sps;
		e.wakeIn(static_cast<uint32_t>(std::max(1.0, std::ceil(toNext - 1e-6))));
	}
}

namespace {
/// HOW MUCH OF HUMANIZE'S TIMING EACH LANE TAKES:
/// kick, snare, closed hat, open hat, clap, low tom, high tom, ride.
constexpr std::array<double, pattern::kDrumLanes> kDrumDrift { 0.25, 0.6, 1.0, 1.0, 0.6, 0.7, 0.7, 1.0 };
constexpr double kDrumMinDrift = 0.25;
} // namespace

double PatternDevice::shiftOf(const void* self, int64_t g) noexcept {
	const auto& d = *static_cast<const PatternDevice*>(self);
	const double shift = humanize::shiftSteps(d.s_.feel, g, d.stepSecondsNow_, d.stepBeatsNow_);
	// DRUM: the slot arrives at its EARLIEST lane's share of the shift (each lane takes its own, `kDrumDrift`), so every
	// lane's onset is now or later — a late feel brings the slot at the kick's share, an early one at the hats'.
	if (d.playing_ < kPatterns && d.s_.patterns[d.playing_].mode == Mode::Drum) {
		return shift > 0.0 ? shift * kDrumMinDrift : shift;
	}
	return shift;
}

void PatternDevice::playSlot(void* self, int64_t g, StepEmitter& e) noexcept {
	static_cast<PatternDevice*>(self)->play(g, e);
}

void PatternDevice::play(int64_t g, StepEmitter& e) noexcept {
	if (s_.playMode == PlayMode::Cycle) {
		playCycle(g, e);
		return;
	}
	cycleHave_ = false; // back on Cycle, the walk is found afresh
	cycleStopped_ = false;
	// UNDER A TRIGGERED FILL (design §7.8): the selected pattern is the base, and the fill has its own grid while it sounds.
	if (selectorFill_ || armedFill_ < kPatterns || activeFill_ < kPatterns) {
		const size_t base = selected(s_);
		if (base >= kPatterns) {
			selectorFill_ = false;
			return;
		}
		playOwned(nullptr, base, g, e);
		selectorFill_ = activeFill_ < kPatterns || playing_ != base;
		return;
	}
	// WHICH PATTERN, AT THIS ONSET (design §7.3). The same step length: the new pattern plays this very slot. Another:
	// the grid switches here and the new pattern's first step is its own next onset (the emitter re-seats).
	const size_t want = selected(s_);
	if (want != playing_) {
		const size_t was = playing_;
		playing_ = want;
		if (want == kPatterns) {
			return;
		}
		const ClockSettings now = clockOf(want);
		const ClockSettings before = e.settings();
		e.setSettings(now);
		if (was == kPatterns || now.synced != before.synced || now.stepBeats != before.stepBeats
		    || now.stepHz != before.stepHz) {
			return;
		}
	}
	if (playing_ == kPatterns) {
		return; // NOTHING IN RANGE: silent, and the held keys are not passed through (design §7.3)
	}
	playOwned(nullptr, playing_, g, e);
}

void PatternDevice::playStep(int64_t g, int64_t step, StepEmitter& e) noexcept {
	playStepOf(playing_, g, step, e, std::numeric_limits<double>::infinity());
}

void PatternDevice::playStepOf(size_t pat, int64_t g, int64_t step, StepEmitter& e, double offLimit) noexcept {
	// THE PATTERN THIS STEP IS (the playing one, or a fill over it) and how late any note it strikes may end (a Span slot's
	// end, a fill window's edge: design §7.8; +inf otherwise, which leaves every off where its gate put it).
	stepPattern_ = pat;
	offLimit_ = offLimit;
	modPattern_ = kPatterns; // (a Modulation step takes it back below)
	rebuildIfStale(stepPattern_, e.chord());
	const pattern::PatternCycle& cycle = built_[stepPattern_].cycle;
	if (!cycle.playable || cycle.length == 0) {
		return;
	}
	const auto length = static_cast<int64_t>(cycle.length);
	const int64_t cycleIndex = floorDiv(step, length);
	const auto k = static_cast<size_t>(step - cycleIndex * length);
	// THE PLAYHEAD: this pattern, this step — whatever the step is (a rest is a step the playhead passes).
	playheadOrdinal_ = static_cast<uint32_t>(stepPattern_ << kPlayheadStepBits) | static_cast<uint32_t>(k);
	++slotsPlayed_;
	lastPlaySample_ = sampleNow_;
	playheadIdle_ = false;
	pattern::PatternStep st = cycle.steps[k]; // a copy: Wobble's extras change it for this slot alone (design §12-ter)
	stepOut_ = cycle.length > 1 ? static_cast<float>(k) / static_cast<float>(cycle.length - 1) : 0.0f;
	// MODULATION (design §7.10) plays no notes: its step is held for `modNow`, which reads the lanes between slots.
	if (genes::isModulation(s_.patterns[stepPattern_].mode)) {
		modPattern_ = stepPattern_;
		modStep_ = k;
		modSlot_ = g;
		return;
	}
	if (st.kind == pattern::PatternStep::Kind::Rest) {
		accentOut_ = 0.0f;
		slideOut_ = 0.0f;
		return;
	}
	if (st.kind == pattern::PatternStep::Kind::Tie) {
		return; // THE NOTE BEFORE RUNS ON: its gate already covers this step
	}
	if (s_.patterns[stepPattern_].mode == Mode::Drum) {
		playDrum(st, g, e);
		return;
	}
	// **WOBBLE'S EXTRAS** (design §12-ter): events at play time, in the modes where each does something,
	// never in the cycle (the lane draws the decode and the hand's edits stay). No rests.
	const WobbleExtras extras = wobbleExtrasOf(s_.patterns[stepPattern_].mode, st, g);
	if (extras.slide) {
		st.slide = true;
		st.gate = std::max(st.gate, 1.0f); // as the decode extends a slide: to the next step, then the overlap
	}
	if (extras.burst > st.ratchet) {
		st.ratchet = static_cast<uint8_t>(extras.burst);
	}
	const int16_t channel = e.chord().empty() ? 0 : e.chord()[0].channel;
	const double on = e.onset(g);
	const double len = e.onset(g + 1) - on;
	// WOBBLE CLIPS A NOTE'S LENGTH (design §12-ter) — never a slide's, and never a gate past its own step (a tie runs on).
	const double gate = static_cast<double>(st.gate);
	const double clipped = !st.slide && gate <= 1.0 ? gate * humanize::lengthScale(s_.feel, g, stepBeatsNow_) : gate;
	double off = gateEnd(e, g, clipped);
	if (st.slide) {
		off += kSlideOverlapSeconds * sampleRate_ * e.sampleSteps();
	}
	off = std::max(std::min(off, offLimit_), on + e.sampleSteps());
	const double velocity = std::clamp(static_cast<double>(st.velocity) * humanize::velocityScale(s_.feel, g, stepBeatsNow_), 0.0, 1.0);
	// A FLAM: this slot's onset was moved early by its lead (`shiftOf`); the grace note sounds there and the note itself
	// on the grid, the lead later.
	const double flam = humanize::flamSteps(s_.feel, stepSecondsNow_);
	// **TRANSPOSE** (design §7.7): every key this step sounds, moved by the device-wide steps (in the playing pattern's scale,
	// from the cycle's own root) and octaves — after the decode and the hand's edits, so edits move with it and never clear.
	const theory::Scale transposeScale = theory::scaleOf(s_.patterns[stepPattern_].scale);
	// THE PATTERN'S OWN PLUS THE DEVICE'S: steps add in one scale, octaves add, and the sum is applied once.
	const int steps = s_.transposeSteps + s_.patternTranspose[stepPattern_][0];
	const int octaves = s_.transposeOctaves + s_.patternTranspose[stepPattern_][1];
	const auto tp = [&](int k0) {
		// AND FOLDED INTO THE NOTE RANGE (design §7.12), last: every road a pitched note takes (the decode, Wobble's octave,
		// a pickup, a voicing, Transpose) ends here, so one fold covers them all. Unclamped first, so a far-low note keeps its
		// pitch class as it rises.
		return static_cast<int16_t>(foldToRange(transposeKey(transposeScale, cycle.rootKey, k0, steps, octaves) + extras.octave,
		                                         s_.noteLow, s_.noteHigh));
	};
	int16_t key = tp(st.keys[0]);
	const bool tbGlide = s_.patterns[stepPattern_].mode == Mode::TbGlide;
	// THE GLIDE TIME: the playing pattern's τ, taken at each segment's start, so a turn mid-slide
	// times the next segment and never bends the one in flight off its own curve.
	const double glideSeconds = std::exp2(static_cast<double>(s_.patterns[stepPattern_].glideTime));
	if (st.count <= 1 && tbGlide) {
		key = tp(cycle.keyAt(k, cycleIndex));
		// TB-GLIDE (design §11.3): a note the step before slid into CONTINUES the run — no new on, the held note's off
		// moved to this step's end, and its pitch bent from where it stands toward this step's key. A run that cannot
		// continue (its note ended: a jump, a stop) starts afresh here, on this step's key.
		if (bend_.active && bend_.slideInto && e.sounding(bend_.key, bend_.channel) && bend_.channel == channel) {
			bend_.from = bend_.value(on);
			bend_.tauSeconds = glideSeconds;
			bend_.target = static_cast<double>(key - bend_.key);
			bend_.startU = on;
			bend_.endU = off;
			bend_.slideInto = st.slide;
			bend_.sentU = -1e300;
			e.extend(bend_.key, channel, off);
		}
		else {
			e.sound(key, channel, velocity, on, off);
			bend_ = Bend {};
			bend_.active = st.slide;
			bend_.slideInto = st.slide;
			bend_.key = key;
			bend_.channel = channel;
			bend_.startU = on;
			bend_.endU = off;
			bend_.tauSeconds = glideSeconds;
			bend_.stepSeconds = e.sampleSteps() > 0.0 ? 1.0 / (e.sampleSteps() * sampleRate_) : 0.125;
		}
	}
	else if (st.count <= 1) {
		key = tp(cycle.keyAt(k, cycleIndex));
		if (st.grace) {
			// A GRACE NOTE ON THE BEAT, a scale degree above, then the note an eighth of a step later.
			const double split = on + len / 8.0;
			e.sound(tp(st.graceKey), channel, velocity * 0.8, on, split);
			e.schedule(key, channel, velocity, split, std::max(off, split + e.sampleSteps()));
		}
		else if (st.ratchet > 1) {
			const double hit = len / st.ratchet;
			e.sound(key, channel, velocity, on, on + 0.5 * hit);
			for (int j = 1; j < st.ratchet; ++j) {
				const double at = on + hit * j;
				e.schedule(key, channel, velocity, at, at + 0.5 * hit);
			}
		}
		else if (flam > 0.0) {
			const double main = on + flam;
			e.sound(key, channel, velocity * humanize::kFlamVelocity, on, main);
			e.schedule(key, channel, velocity, main, std::max(off, main + e.sampleSteps()));
		}
		else {
			e.sound(key, channel, velocity, on, off);
		}
	}
	else {
		// A CHORD ON ONE SAMPLE (design §10.1): every tone in this call; a held common tone runs on instead.
		// HUMANIZE'S STRUM AND FLAM ARE THE EXCEPTIONS (design §12): a strum lands tone j a share of the step late, in
		// pitch order up or down, each a little softer; a flam strikes the top tone early as its grace.
		// WOBBLE'S STRUM (design §12-ter) where Humanize is not strumming already: tone j a share of `kStrumSpread` late,
		// up or down by the step's sign.
		const bool humanizeStrums = s_.feel.mode == humanize::Mode::StrumUp || s_.feel.mode == humanize::Mode::StrumDown;
		const bool wobbleStrums = extras.strum && !humanizeStrums;
		const bool down = wobbleStrums ? extras.strumDown : s_.feel.mode == humanize::Mode::StrumDown;
		if (flam > 0.0) {
			const int16_t top = tp(st.keys[static_cast<size_t>(st.count - 1)]);
			e.sound(top, channel, velocity * humanize::kFlamVelocity, on, on + flam);
		}
		const double chordOn = on + flam;
		for (int i = 0; i < st.count; ++i) {
			const int16_t tone = tp(st.keys[static_cast<size_t>(i)]);
			if (((st.holdMask >> i) & 1u) != 0u && e.extend(tone, channel, off)) {
				continue;
			}
			const int j = down ? st.count - 1 - i : i;
			const double strum = (wobbleStrums ? humanize::wobble::kStrumSpread * static_cast<double>(j) / static_cast<double>(st.count - 1)
			                                   : humanize::strumShare(s_.feel, j, st.count))
			                     * len;
			if (strum > 0.0 || flam > 0.0) {
				const double at = chordOn + strum;
				e.schedule(tone, channel, velocity * (1.0 - humanize::kStrumVelocityStep * j), at,
				           std::max(off, at + e.sampleSteps()));
				continue;
			}
			if (st.ratchet > 1) {
				const double hit = len / st.ratchet;
				e.sound(tone, channel, velocity, on, on + 0.5 * hit);
				for (int j = 1; j < st.ratchet; ++j) {
					const double at = on + hit * j;
					e.schedule(tone, channel, velocity, at, at + 0.5 * hit);
				}
			}
			else {
				e.sound(tone, channel, velocity, on, off);
			}
		}
		key = tp(st.keys[static_cast<size_t>(st.count - 1)]); // the Pitch source reads the top tone
	}
	pitchOut_ = static_cast<float>(key) / 127.0f;
	velocityOut_ = static_cast<float>(velocity);
	accentOut_ = st.accent ? s_.patterns[stepPattern_].accent : 0.0f;
	slideOut_ = st.slide ? 1.0f : 0.0f;
	const theory::Scale scale = theory::scaleOf(s_.patterns[stepPattern_].scale);
	const int d = (((st.degree + steps) % scale.count) + scale.count) % scale.count;
	degreeOut_ = scale.count > 1 ? static_cast<float>(d) / static_cast<float>(scale.count - 1) : 0.0f;
	e.noteRecent(static_cast<uint32_t>(g), pitchOut_);
}

//------------------------------------------------------------------------------------------------//
// Cycle (design §7.6)                                                                            //
//------------------------------------------------------------------------------------------------//

size_t PatternDevice::cycleFirst() const noexcept {
	std::array<size_t, kPatterns> range {};
	return inRange(s_, range) > 0 ? range[0] : kPatterns;
}

double PatternDevice::stepSecondsOf(size_t p) const noexcept {
	const PatternSettings& q = s_.patterns[std::min(p, kPatterns - 1)];
	arp::Settings rate;
	rate.timeBase = q.timeBase;
	rate.rate = q.rate;
	const double hz = stepHz(rate);
	const double seconds = rate.synced() ? stepBeats(rate) * spbNow_ : (hz > 0.0 ? 1.0 / hz : 1.0);
	return std::max(seconds, kMinStepSeconds);
}

void PatternDevice::refreshLengths() noexcept {
	if (cycleLengthDirty_) {
		// EACH PATTERN'S CYCLE LENGTH, as `pattern::build` sets it — the Length box, or a Progression's progression (a
		// function of its place and recipe, not of the keys held, so the empty chord's build is the length). The likes'
		// warp is not applied here: a Progression with Likes Warp on times its segment by its plain progression.
		for (size_t p = 0; p < kPatterns; ++p) {
			const PatternSettings& q = s_.patterns[p];
			if (q.mode == Mode::Progression) {
				pattern::build(placeOf(q, handDecoded_[p].residue), recipeOf(p), HeldChord {}, lengthScratch_);
				cycleLength_[p] = static_cast<int64_t>(std::max<size_t>(1, lengthScratch_.length));
			}
			else {
				cycleLength_[p] = static_cast<int64_t>(std::clamp<size_t>(q.length, 1, genes::kMaxSteps));
			}
		}
		cycleLengthDirty_ = false;
	}
}

CycleConfig PatternDevice::cycleConfigNow(bool song) noexcept {
	refreshLengths();
	CycleConfig c;
	std::array<size_t, kPatterns> range {};
	c.count = inRange(s_, range);
	bool anySynced = false;
	for (size_t i = 0; i < c.count; ++i) {
		c.order[i] = static_cast<uint8_t>(range[i]);
		anySynced = anySynced || s_.patterns[range[i]].synced();
	}
	for (size_t p = 0; p < kPatterns; ++p) {
		c.span[p] = static_cast<int64_t>(s_.cycle[p].repeats) * cycleLength_[p];
		c.next[p] = s_.cycle[p].next;
		// THE WALK'S UNIT: on the song clock, BEATS when any pattern in range is synced (a Hz pattern's step converted at
		// the tempo now — exact at a steady tempo), else SECONDS; on the free clock, whose steps run on unbroken across a
		// change of rate, one SLOT.
		if (!song) {
			c.unit[p] = 1.0;
			continue;
		}
		arp::Settings rate;
		rate.timeBase = s_.patterns[p].timeBase;
		rate.rate = s_.patterns[p].rate;
		if (anySynced) {
			c.unit[p] = rate.synced() ? stepBeats(rate) : stepSecondsOf(p) / spbNow_;
		}
		else {
			c.unit[p] = stepSecondsOf(p);
		}
	}
	// SPAN AND FILLS (design §7.8). A Span slot is whole 4/4 bars in the walk's unit: beats, seconds at the tempo now, or on
	// the free clock the pattern's own steps (a Hz pattern there has no bar, so its Span is Auto).
	size_t fills = 0;
	for (size_t i = 0; i < c.count; ++i) {
		fills += s_.phase[c.order[i]].fill ? 1u : 0u;
	}
	for (size_t p = 0; p < kPatterns; ++p) {
		c.repeats[p] = s_.cycle[p].repeats;
		c.fill[p] = s_.phase[p].fill && fills < c.count; // every tab a fill: nothing to merge into, so they play as patterns
		const int bars = s_.phase[p].spanBars;
		if (bars <= 0) {
			continue;
		}
		arp::Settings rate;
		rate.timeBase = s_.patterns[p].timeBase;
		rate.rate = s_.patterns[p].rate;
		const double beats = 4.0 * static_cast<double>(bars);
		if (!song) {
			c.spanSlot[p] = rate.synced() ? beats / stepBeats(rate) : 0.0;
		}
		else {
			c.spanSlot[p] = anySynced ? beats : beats * spbNow_;
		}
	}
	c.seed = s_.feel.seed;
	return c;
}

void PatternDevice::playCycle(int64_t g, StepEmitter& e) noexcept {
	const bool song = e.songClock();
	const CycleConfig want = cycleConfigNow(song);
	// THE GRID g IS ON — the playing pattern's (the walk's first before anything has played).
	const size_t grid = std::min(playing_, kPatterns - 1);
	const auto offset = static_cast<int64_t>(s_.offset);
	const int64_t j = g - offset;
	// A JUMP (a locate, a loop, a clock started or changed) TAKES THE ROW AT ONCE — the walk is found from the origin —
	// and so does a change of the steps' units (a Rate, a tempo under Hz): the grid has already moved under them.
	const bool jump = !cycleHave_ || song != cycleSong_ || g != cycleLastSlot_ + 1;
	if (!walk_.configured() || jump || want.unit != walk_.config().unit) {
		walk_.configure(want);
	}
	double t = static_cast<double>(j) * walk_.config().unit[grid];
	CycleSegment seg = walk_.at(t);
	const auto repeatAt = [&](const CycleSegment& sg, double at) {
		// A SPAN SLOT IS A REPEAT (design §7.8); otherwise a Repeats × Length edge — by TIME, so a fill's grid reads the
		// segment under it the same.
		const CycleConfig& c = walk_.config();
		if (sg.pattern >= kPatterns) {
			return int64_t { 0 };
		}
		if (c.spanSlot[sg.pattern] > 0.0) {
			return static_cast<int64_t>(std::floor((at - sg.base) / c.spanSlot[sg.pattern] + cycleTolerance(at)));
		}
		const double slot = at / c.unit[sg.pattern];
		return static_cast<int64_t>(
		    std::floor((slot - static_cast<double>(sg.first) + cycleTolerance(slot)) / static_cast<double>(std::max<int64_t>(1, c.span[sg.pattern]))));
	};
	if (!(want == walk_.config())) {
		// ANY OTHER ROW CHANGE WAITS FOR THE BOUNDARY (Repeats × Length steps of the segment playing), or for a stop.
		const bool boundary = seg.pattern >= kPatterns || seg.index != cycleMarkIndex_ || repeatAt(seg, t) != cycleMarkRepeat_;
		if (boundary) {
			walk_.configure(want);
			t = static_cast<double>(j) * walk_.config().unit[grid];
			seg = walk_.at(t);
		}
	}
	cycleHave_ = true;
	cycleSong_ = song;
	cycleLastSlot_ = g;
	if (seg.pattern >= kPatterns) {
		// STOPPED (or nothing in range): silent from the Stop's boundary on, until playback starts again before it.
		cycleStopped_ = true;
		cycleMarkIndex_ = seg.index;
		cycleMarkRepeat_ = 0;
		return;
	}
	cycleStopped_ = false;
	cycleMarkIndex_ = seg.index;
	cycleMarkRepeat_ = repeatAt(seg, t);
	playOwned(&seg, seg.pattern, g, e);
}

double PatternDevice::unitOf(size_t p, bool song) const noexcept {
	// ONE STEP OF PATTERN p IN THE POSITIONS THIS SLOT IS MEASURED IN: Cycle's walk units; under Selector the same rule
	// (beats for a division, a Hz step converted at the tempo now; on the free clock one slot).
	if (walk_.configured() && s_.playMode == PlayMode::Cycle) {
		return walk_.config().unit[std::min(p, kPatterns - 1)];
	}
	if (!song) {
		return 1.0;
	}
	arp::Settings rate;
	rate.timeBase = s_.patterns[std::min(p, kPatterns - 1)].timeBase;
	rate.rate = s_.patterns[std::min(p, kPatterns - 1)].rate;
	return rate.synced() ? stepBeats(rate) : stepSecondsOf(p) / spbNow_;
}

PatternDevice::FillWindow PatternDevice::windowAt(size_t f, double end, double floor, bool song) const noexcept {
	// **A FILL'S WINDOW AT ITS OWN RATE**: its cycle laid on its own
	// grid so its LAST step ends at the last of its grid's slots at or before `end`; the [Start, End) steps of it, as
	// positions. What reaches back before `floor` (the slot under it began) is lost.
	FillWindow w;
	const double unit = unitOf(f, song);
	const int64_t lengthF = std::max<int64_t>(1, cycleLength_[f]);
	const int64_t start = std::clamp<int64_t>(s_.phase[f].start, 0, lengthF - 1);
	const int64_t stop = std::clamp<int64_t>(s_.phase[f].end, start + 1, lengthF);
	w.fill = f;
	w.unit = unit;
	w.endSlot = static_cast<int64_t>(std::floor(end / unit + cycleTolerance(end / unit)));
	w.firstSlot = std::max(w.endSlot - lengthF + start, firstSlotAtOrAfter(floor, unit));
	w.lastSlot = w.endSlot - lengthF + stop; // exclusive
	w.from = static_cast<double>(w.firstSlot) * unit;
	w.to = std::max(static_cast<double>(w.lastSlot) * unit, w.from);
	return w;
}

void PatternDevice::switchGrid(size_t to, bool song, StepEmitter& e) noexcept {
	// A NEW PATTERN'S GRID FROM HERE. On the song clock what sounds is carried onto it at its song position (so the notes
	// sounding finish at their offs); the free clock's steps run on unbroken.
	const ClockSettings now = clockOf(to);
	const ClockSettings before = e.settings();
	double factor = 1.0;
	if (song) {
		const auto slotSeconds = [&](const ClockSettings& c) {
			return c.synced ? c.stepBeats * spbNow_ : (c.stepHz > 0.0 ? 1.0 / c.stepHz : 1.0);
		};
		factor = before.synced && now.synced ? before.stepBeats / now.stepBeats : slotSeconds(before) / slotSeconds(now);
		if (!(factor > 0.0) || !std::isfinite(factor)) {
			factor = 1.0;
		}
	}
	playing_ = to;
	e.regrid(now, factor);
	if (song && factor != 1.0) {
		bend_.startU *= factor;
		bend_.endU *= factor;
		bend_.sentU *= factor;
	}
	regridded_ = true;
	e.wakeIn(1);
}

void PatternDevice::playOwned(const CycleSegment* seg, size_t base, int64_t g, StepEmitter& e) noexcept {
	refreshLengths();
	const bool song = e.songClock();
	const size_t grid = std::min(playing_, kPatterns - 1);
	const int64_t offset = s_.offset;
	const double unitG = unitOf(grid, song);
	const int64_t j = g - offset;
	const double t = static_cast<double>(j) * unitG;
	const double tol = cycleTolerance(t);
	const double unitB = unitOf(base, song);
	const int64_t lengthB = std::max<int64_t>(1, cycleLength_[base]);

	// THE BASE PATTERN'S REPEAT AROUND A POSITION: where it began and where it ends, as positions (a Span slot under Cycle;
	// Repeats × Length's edges otherwise; under Selector the cycle's own on the song grid).
	const auto repeatAround = [&](double at, double* from, double* to) {
		if (seg != nullptr && walk_.config().spanSlot[base] > 0.0) {
			const double span = walk_.config().spanSlot[base];
			const double r = std::floor((at - seg->base) / span + cycleTolerance(at));
			*from = seg->base + r * span;
			*to = *from + span;
			return;
		}
		const int64_t first = seg != nullptr ? seg->first : 0;
		const int64_t jb = static_cast<int64_t>(std::floor(at / unitB + cycleTolerance(at / unitB)));
		const int64_t r = floorDiv(jb - first, lengthB);
		*from = static_cast<double>(first + r * lengthB) * unitB;
		*to = *from + static_cast<double>(lengthB) * unitB;
	};

	// A TRIGGERED FILL: one sounding is dropped once its window is past (or the playhead jumped back before it); one armed
	// takes the window that ends with the base pattern's CURRENT repeat — or the next one's, if this one's has begun.
	if (activeFill_ < kPatterns && (t + tol >= activeWindow_.to || t + tol < activeAt_)) {
		activeFill_ = kPatterns;
	}
	if (armedFill_ < kPatterns && activeFill_ == kPatterns) {
		double from = 0.0;
		double to = 0.0;
		repeatAround(t, &from, &to);
		FillWindow w = windowAt(armedFill_, to, from, song);
		if (w.from + cycleTolerance(w.from) < t) {
			w = windowAt(armedFill_, to + (to - from), to, song); // already begun: the next repeat's
		}
		armedFill_ = kPatterns;
		if (w.to > w.from) {
			activeFill_ = w.fill;
			activeWindow_ = w;
			activeAt_ = t;
		}
	}
	// THE WALK'S FILL over this segment's tail (Cycle), where no triggered fill is sounding.
	FillWindow walkWindow;
	if (seg != nullptr && seg->fill < kPatterns && seg->end < CycleSegment::kNever) {
		walkWindow = windowAt(seg->fill, seg->end, static_cast<double>(seg->first) * unitB, song);
	}
	const auto windowAtPos = [&](double at) -> const FillWindow* {
		const double near = at + cycleTolerance(at);
		if (activeFill_ < kPatterns && near >= activeWindow_.from && near < activeWindow_.to) {
			return &activeWindow_;
		}
		if (walkWindow.fill < kPatterns && near >= walkWindow.from && near < walkWindow.to) {
			return &walkWindow;
		}
		return nullptr;
	};
	const FillWindow* here = windowAtPos(t);
	const size_t owner = here != nullptr ? here->fill : base;

	// THE STEP IN HAND, by whoever owns this position, on its own grid.
	const auto playAs = [&](size_t who, const FillWindow* w, int64_t slot, int64_t gs) {
		const double unitW = unitOf(who, song);
		const double at = static_cast<double>(slot) * unitW;
		double limit = std::numeric_limits<double>::infinity();
		if (w != nullptr) {
			// A FILL'S STEP; its notes end at the window's edge (the base carries on from there, unre-struck).
			const int64_t lengthF = std::max<int64_t>(1, cycleLength_[who]);
			limit = w->to / unitW + static_cast<double>(offset);
			playStepOf(who, gs, slot - (w->endSlot - lengthF), e, limit);
			return;
		}
		// THE BASE: its phase (Start), and its Span slot (restart, cut at its end).
		const int64_t phase = std::clamp<int64_t>(s_.phase[who].start, 0, lengthB - 1);
		int64_t step = 0;
		if (seg == nullptr) {
			step = slot + phase;
		}
		else if (walk_.config().spanSlot[who] > 0.0) {
			double from = 0.0;
			double to = 0.0;
			repeatAround(at, &from, &to);
			const int64_t repeatStart = std::max(seg->first, firstSlotAtOrAfter(from, unitW));
			step = slot - repeatStart + phase;
			limit = to / unitW + static_cast<double>(offset); // CUT AT THE SPAN SLOT'S END
		}
		else {
			step = slot - seg->first + phase;
		}
		// CUT WHERE A FILL COMES IN, if one begins before this slot's next.
		for (const FillWindow* next : { activeFill_ < kPatterns ? &activeWindow_ : nullptr,
		                                walkWindow.fill < kPatterns ? &walkWindow : nullptr }) {
			if (next != nullptr && next->from > at + cycleTolerance(at)) {
				limit = std::min(limit, next->from / unitW + static_cast<double>(offset));
			}
		}
		playStepOf(who, gs, step, e, limit);
	};

	if (owner != grid) {
		// THIS POSITION IS ANOTHER'S (a boundary the look-ahead did not meet, a jump): its grid from here, and its first
		// slot at or after the position — now, if it falls here.
		switchGrid(owner, song, e);
		const double unitO = unitOf(owner, song);
		int64_t jo = firstSlotAtOrAfter(t, unitO);
		if (owner == base && seg != nullptr) {
			jo = std::max(jo, seg->first);
		}
		if (here != nullptr) {
			jo = std::max(jo, here->firstSlot);
		}
		const int64_t go = jo + offset;
		if (static_cast<double>(jo) * unitO <= t + tol) {
			e.continueAt(go + 1);
			cycleLastSlot_ = go;
			playAs(owner, here, jo, go);
		}
		else {
			e.continueAt(go);
			cycleLastSlot_ = go - 1;
		}
		return;
	}
	playAs(owner, here, j, g);

	// **THE LOOK-AHEAD**: if the owner changes before this grid's next slot (a fill's window opening or closing, the
	// segment's end), take the next owner's grid NOW and carry on at its first slot there — so a switch lands where it
	// falls, not at this grid's next slot.
	const double next = static_cast<double>(j + 1) * unitG;
	double change = std::numeric_limits<double>::infinity();
	for (const FillWindow* w : { activeFill_ < kPatterns ? &activeWindow_ : nullptr,
	                             walkWindow.fill < kPatterns ? &walkWindow : nullptr }) {
		if (w != nullptr) {
			if (w->from > t + tol) {
				change = std::min(change, w->from);
			}
			if (w->to > t + tol) {
				change = std::min(change, w->to);
			}
		}
	}
	if (seg != nullptr && seg->end < CycleSegment::kNever && seg->end > t + tol) {
		change = std::min(change, seg->end);
	}
	if (!(change < next - cycleTolerance(next))) {
		return;
	}
	const FillWindow* then = windowAtPos(change);
	size_t nextOwner = then != nullptr ? then->fill : base;
	int64_t floorSlot = std::numeric_limits<int64_t>::min();
	if (seg != nullptr && seg->end < CycleSegment::kNever && change + cycleTolerance(change) >= seg->end) {
		// THE SEGMENT ENDS FIRST: the next segment's pattern and its first slot.
		const CycleSegment after = walk_.at(change);
		if (after.pattern >= kPatterns) {
			return; // a Stop: nothing to carry on to
		}
		const FillWindow* atAfter = nullptr; // (the next segment's own fill, if any, is met at its slots)
		then = atAfter;
		nextOwner = after.pattern;
		floorSlot = after.first;
	}
	if (nextOwner == grid) {
		return;
	}
	switchGrid(nextOwner, song, e);
	const double unitN = unitOf(nextOwner, song);
	int64_t jn = std::max(firstSlotAtOrAfter(change, unitN), floorSlot);
	if (then != nullptr) {
		jn = std::max(jn, then->firstSlot);
	}
	e.continueAt(jn + offset);
	cycleLastSlot_ = jn + offset - 1;
}


void PatternDevice::playDrum(const pattern::PatternStep& st, int64_t g, StepEmitter& e) noexcept {
	// GATE ONLY: the held keys started this; their pitch is never read. Each lane sounds its own note, its own velocity,
	// its own length (the open hat's ends at its choke), and its own share of Humanize's timing — the hats drift the
	// most, the kick least; a flam is the snare's and the clap's alone.
	const int16_t channel = e.chord().empty() ? 0 : e.chord()[0].channel;
	const double on = e.onset(g);
	const double len = e.onset(g + 1) - on;
	// HUMANIZE'S WHOLE SHIFT FOR THIS SLOT, and what the slot itself was moved by (`shiftOf`): each lane lands at its own
	// share of the whole, which is never before the slot's own onset.
	const double whole = humanize::shiftSteps(s_.feel, g, stepSecondsNow_, stepBeatsNow_) * len;
	const double slot = shiftOf(this, g) * len;
	const double flam = humanize::flamSteps(s_.feel, stepSecondsNow_);
	// WOBBLE CLIPS A HIT'S LENGTH (design §12-ter) — never a run past its own step (an open hat held into the next).
	const double clip = humanize::lengthScale(s_.feel, g, stepBeatsNow_);
	const auto laneLength = [clip](float length) { return length <= 1.0f ? static_cast<double>(length) * clip : static_cast<double>(length); };
	const pattern::DrumLanes& lanes = s_.patterns[stepPattern_].drum;
	int16_t first = -1;
	size_t firstLane = 0;
	double loudest = 0.0;
	for (size_t l = 0; l < pattern::kDrumLanes; ++l) {
		if (((st.drumHits >> l) & 1u) == 0u) {
			continue;
		}
		const auto lane = static_cast<pattern::DrumLane>(l);
		const int16_t key = lanes.note[l];
		const double laneOn = on + std::max(0.0, kDrumDrift[l] * whole - slot);
		const double velocity = std::clamp(static_cast<double>(st.drumVelocity[l]) * humanize::velocityScale(s_.feel, g, stepBeatsNow_), 0.0, 1.0);
		const double off = std::max(laneOn + e.sampleSteps(), std::min(laneOn + laneLength(st.drumLength[l]) * len, offLimit_));
		const bool flams = flam > 0.0 && (lane == pattern::DrumLane::Snare || lane == pattern::DrumLane::Clap);
		// NOW, OR LATER IN THE STEP (`sound` strikes on this call; `schedule` queues for the call that reaches it).
		const auto strike = [&](double at, double v, double until) {
			if (at <= on) {
				(void)e.sound(key, channel, v, on, until);
			}
			else {
				(void)e.schedule(key, channel, v, at, until);
			}
		};
		if (((st.drumRoll >> l) & 1u) != 0u && st.ratchet > 1) {
			const double hit = len / st.ratchet;
			strike(laneOn, velocity, laneOn + 0.5 * hit);
			for (int j = 1; j < st.ratchet; ++j) {
				const double at = laneOn + hit * j;
				e.schedule(key, channel, velocity, at, at + 0.5 * hit);
			}
		}
		else if (flams) {
			strike(laneOn, velocity * humanize::kFlamVelocity, laneOn + flam);
			strike(laneOn + flam, velocity, std::max(off, laneOn + flam + e.sampleSteps()));
		}
		else {
			strike(laneOn, velocity, off);
		}
		if (first < 0) {
			first = key;
			firstLane = l;
		}
		loudest = std::max(loudest, velocity);
	}
	// THE SOURCES: Pitch reads the lowest lane's note, Degree the lane's index (design §13: Degree IS the lane in Drum).
	pitchOut_ = first >= 0 ? static_cast<float>(first) / 127.0f : pitchOut_;
	velocityOut_ = static_cast<float>(loudest);
	accentOut_ = st.drumAccent != 0 ? s_.patterns[stepPattern_].accent : 0.0f;
	slideOut_ = 0.0f;
	degreeOut_ = static_cast<float>(firstLane) / static_cast<float>(pattern::kDrumLanes - 1);
	e.noteRecent(static_cast<uint32_t>(g), pitchOut_);
}

bool PatternDevice::writeEdits(const float* in, size_t n) noexcept {
	if (n != kEditsWords) {
		return false;
	}
	device::BlockReader check(in, n);
	for (size_t i = 0; i < n; ++i) {
		check.u16();
	}
	if (!check.ok()) {
		return false;
	}
	edits_.write(in, n);
	return true;
}

bool PatternDevice::restoreEditsV3(const float* in, size_t n) {
	// A V3 BLOB, FROM A PATCH SAVED BEFORE CELLS: read into v4 with no cell set. A v4 restored first wins
	// (a patch that carries both is one saved here, whose v3 is a stale copy a host kept), whatever the order they arrive in.
	if (editsV4Restored_ || n != kEditsWordsV3) {
		return n == kEditsWordsV3;
	}
	const EditState state = editStateOf(in, n);
	std::vector<float> v4(kEditsWords);
	writeEditState(state, v4.data());
	return writeEdits(v4.data(), v4.size());
}


//------------------------------------------------------------------------------------------------//
// Identity and Modulation (design §7.10)                                                         //
//------------------------------------------------------------------------------------------------//

void modLevelsOf(const pattern::PatternCycle& cycle, uint8_t laneOn, ModLevels& out, uint8_t* live) noexcept {
	const size_t n = std::min(cycle.length, genes::kMaxSteps);
	uint8_t hit = 0;
	for (size_t l = 0; l < pattern::kDrumLanes; ++l) {
		// THE LEVEL ENTERING THE CYCLE is the level its last hit left, so a loop is seamless: find that hit first.
		float level = 0.0f;
		bool any = false;
		for (size_t k = n; k-- > 0;) {
			const pattern::PatternStep& st = cycle.steps[k];
			if (st.kind == pattern::PatternStep::Kind::Note && ((st.drumHits >> l) & 1u) != 0u) {
				level = st.drumVelocity[l];
				any = true;
				break;
			}
		}
		for (size_t k = 0; k < n; ++k) {
			const pattern::PatternStep& st = cycle.steps[k];
			if (st.kind == pattern::PatternStep::Kind::Note && ((st.drumHits >> l) & 1u) != 0u) {
				level = st.drumVelocity[l];
			}
			out[k][l] = std::clamp(level, 0.0f, 1.0f);
		}
		if (any && ((laneOn >> l) & 1u) != 0u) {
			hit = static_cast<uint8_t>(hit | (1u << l));
		}
	}
	if (live != nullptr) {
		*live = hit;
	}
}

double modLevelAt(const ModLevels& levels, size_t length, size_t k, double frac, size_t lane, ModSmooth smooth,
                  double time) noexcept {
	const size_t n = std::clamp<size_t>(length, 1, genes::kMaxSteps);
	k %= n;
	frac = std::clamp(frac, 0.0, 1.0);
	const double here = levels[k][lane];
	time = std::clamp(time, 0.0, kModTimeMax);
	if (smooth == ModSmooth::Ramp && time > 1e-6) {
		const double span = std::min(time, 1.0);
		const double a = std::clamp((frac - (1.0 - span)) / span, 0.0, 1.0);
		return here + (static_cast<double>(levels[(k + 1) % n][lane]) - here) * a;
	}
	if (smooth == ModSmooth::Glide && time > 1e-6) {
		// THE SLEW'S START is where the cycle's periodic slew stands as this step begins: s_k = (1 − r)·Σ r^(j−1)·L[k−j]
		// over every step before, which wraps into one cycle's sum over (1 − r^n). Derived from the step, so a locate
		// lands on the same line (design §7.10's determinism).
		const double r = std::exp(-1.0 / time);
		double sum = 0.0;
		double w = 1.0;
		for (size_t j = 1; j <= n; ++j) {
			sum += w * static_cast<double>(levels[(k + n - j) % n][lane]);
			w *= r;
		}
		const double start = (1.0 - r) * sum / (1.0 - w);
		return here + (start - here) * std::exp(-frac / time);
	}
	return here;
}

float modOutput(const ModLevels& levels, uint8_t live, size_t length, size_t k, double frac, size_t lane,
                const ModLanes& m) noexcept {
	if (m.neutral || lane >= pattern::kDrumLanes || ((live >> lane) & 1u) == 0u) {
		return 0.0f; // IDENTITY: the value that moves a destination by nothing, in either polarity
	}
	const double v = modLevelAt(levels, length, k, frac, lane, m.smooth, static_cast<double>(m.time));
	return static_cast<float>(m.bipolar ? 2.0 * v - 1.0 : v);
}

void PatternDevice::modNow(double u, const StepEmitter& e) noexcept {
	if (modPattern_ >= kPatterns || playheadIdle_) {
		modOut_.fill(0.0f);
		return;
	}
	const Built& b = built_[modPattern_];
	const double on = e.onset(modSlot_);
	const double len = e.onset(modSlot_ + 1) - on;
	const double frac = len > 0.0 ? (u - on) / len : 0.0;
	for (size_t l = 0; l < pattern::kDrumLanes; ++l) {
		modOut_[l] = modOutput(b.levels, b.live, b.cycle.length, modStep_, frac, l, s_.modLanes[modPattern_]);
	}
}

PatternDevice::WobbleExtras PatternDevice::wobbleExtrasOf(Mode mode, const pattern::PatternStep& st, int64_t g) const noexcept {
	using F = humanize::wobble::Field;
	WobbleExtras x;
	const humanize::Feel& feel = s_.feel;
	if (feel.wobble == 0.0 || st.kind != pattern::PatternStep::Kind::Note) {
		return x;
	}
	const double beats = stepBeatsNow_;
	const auto fires = [&](F f) { return humanize::wobbleFires(feel, f, g, beats); };
	const auto up = [&](F f) { return humanize::wobbleSign(f, g, beats) > 0.0; };
	// A SLIDE CHANCE, the TB modes': the step slides into the next (TB-Mono's overlap, TB-Glide's bend). No tie: a tie would
	// take the next step's strike away, which is a rest by another name.
	if (genes::isTb(mode) && fires(F::Slide)) {
		x.slide = true;
	}
	// A BURST, in the modes that strike ratchets (the Ratchet pin's own: Melody, Chords, TB-Mono) — Wobble's, not the pin's:
	// the pin is the terrain deciding which steps ratchet, Wobble a groove laid over every step, each opted into apart.
	if (genes::pinRead(mode, genes::Pin::Ratchet) && !st.grace && !st.slide && !x.slide && fires(F::Burst)) { // never a slide
		x.burst = up(F::Burst) ? humanize::wobble::kBurstMany : humanize::wobble::kBurstFew;
	}
	// AN OCTAVE JUMP, up or down, in every Mode that plays a pitch but TB-Glide (a bent run would slur a whole octave).
	// Never Drum (fixed note numbers:) or Modulation (no notes).
	const bool pitched = mode == Mode::Melody || mode == Mode::MelodyMono || mode == Mode::Chords || mode == Mode::TbMono
	                     || mode == Mode::Progression;
	if (pitched && fires(F::Octave)) {
		x.octave = up(F::Octave) ? 12 : -12;
	}
	// A STRUM, of a chord in Chords or Progression.
	if ((mode == Mode::Chords || mode == Mode::Progression) && st.count > 1 && fires(F::Strum)) {
		x.strum = true;
		x.strumDown = !up(F::Strum);
	}
	return x;
}

float PatternDevice::tabActive(size_t tab) const noexcept {
	// THE TAB SOUNDING: the pattern whose step is in hand (a fill over another, during its window), while the playhead runs.
	return !playheadIdle_ && tab < kPatterns && stepPattern_ == tab ? 1.0f : 0.0f;
}

float PatternDevice::modLane(size_t tab, size_t lane) const noexcept {
	return tab < kPatterns && tab == modPattern_ && !playheadIdle_ && lane < pattern::kDrumLanes ? modOut_[lane] : 0.0f;
}

void PatternDevice::playhead(uint32_t* ordinal, float* phase) const noexcept {
	if (ordinal != nullptr) {
		*ordinal = playheadIdle_ ? 0xFFFFFFFFu : playheadOrdinal_;
	}
	if (phase != nullptr) {
		*phase = 0.0f;
	}
}

float PatternDevice::pendingLevel() const noexcept {
	// **PATTERN'S PLAYHEAD LEVEL IS ITS ARMED FILL**: the tab whose Go armed a fill still waiting for its boundary, as (tab + 1) / 8 — 0 for
	// none. The device's own state, down the lock-free road every host already carries beside the playhead
	// (`ModOps::playheadLevel`), and published whether or not a step is playing: a fill armed under a stopped transport
	// is still pending.
	return armedFill_ < kPatterns ? static_cast<float>(armedFill_ + 1) / 8.0f : 0.0f;
}

float PatternDevice::outputAt(size_t output) const noexcept {
	switch (static_cast<Output>(output)) {
	case Output::Step: return stepOut_;
	case Output::Pitch: return pitchOut_;
	case Output::Velocity: return velocityOut_;
	case Output::Accent: return accentOut_;
	case Output::Slide: return slideOut_;
	case Output::AccentSweep: return sweepOut_;
	case Output::Degree: return degreeOut_;
	default: break;
	}
	// THE IDENTITY GATES, THEN EACH TAB'S EIGHT LANES (design §7.10).
	if (output >= kFirstTabOutput && output < kFirstLaneOutput) {
		return tabActive(output - kFirstTabOutput);
	}
	if (output >= kFirstLaneOutput && output < static_cast<size_t>(Output::kCount)) {
		const size_t i = output - kFirstLaneOutput;
		return modLane(i / pattern::kDrumLanes, i % pattern::kDrumLanes);
	}
	return 0.0f;
}

//------------------------------------------------------------------------------------------------//
// The hand's block                                                                               //
//------------------------------------------------------------------------------------------------//

static_assert(genes::kPinCount <= 16, "a pattern's pin switches are one u16 of the hand's block");

HandState handStateOf(const float* in, size_t n) noexcept {
	HandState decoded {};
	if (in == nullptr || n != kHandWords) {
		return decoded;
	}
	device::BlockReader r(in, n);
	for (size_t p = 0; p < kPatterns; ++p) {
		for (size_t i = 0; i < 2 * genes::kRowCount; ++i) {
			const double v = r.f64();
			decoded[p].residue.offset[i] = std::isfinite(v) ? v : 0.0;
		}
		const uint32_t bits = r.u16();
		for (size_t i = 0; i < genes::kPinCount; ++i) {
			decoded[p].pinOn[i] = ((bits >> i) & 1u) != 0u;
		}
		const float zoom = r.f32();
		decoded[p].zoom = std::isfinite(zoom) && zoom > 0.0f ? zoom : 1.0f;
	}
	return r.ok() ? decoded : HandState {};
}

void writeHandState(const HandState& state, float* out) noexcept {
	device::BlockWriter w(out, kHandWords);
	for (const HandSlot& slot : state) {
		for (size_t i = 0; i < 2 * genes::kRowCount; ++i) {
			w.f64(slot.residue.offset[i]);
		}
		uint32_t bits = 0;
		for (size_t i = 0; i < genes::kPinCount; ++i) {
			bits |= slot.pinOn[i] ? (1u << i) : 0u;
		}
		w.u16(bits);
		w.f32(slot.zoom);
	}
}

HandSlot foldRotate(const HandSlot& slot, const PatternSettings& before, float rotateAfter) noexcept {
	// THE SAME HOME AND DRIFT `placeOf` READS: `sliceHome`, and the place.
	const double home = static_cast<double>(before.zone) / 1023.0 + static_cast<double>(before.zoneOffset) + before.gamma;
	const double tx = static_cast<double>(before.placeX);
	const double ty = static_cast<double>(before.placeY);
	HandSlot out = slot;
	out.residue = foldResidue<genes::kRowCount>(slot.residue, static_cast<double>(before.rotate),
	                                            static_cast<double>(rotateAfter), tx, ty, home);
	return out;
}

PlaneMark markOn(const PatternSettings& q, const SliceResidue<genes::kRowCount>& residue,
                 const std::array<double, 2 * genes::kRowCount>& point) noexcept {
	// THE PLANE AT ZERO TRAVEL AND ITS TWO DIRECTIONS, from `placeOf` itself: it is affine in the travel (phaseFor and
	// rateFor add the travel's term to the standing), so the differences are exactly the directions, with no restatement.
	PatternSettings at = q;
	at.placeX = 0.0f;
	at.placeY = 0.0f;
	const genes::Place o = PatternDevice::placeOf(at, residue);
	at.placeX = 1.0f;
	const genes::Place ex = PatternDevice::placeOf(at, residue);
	at.placeX = 0.0f;
	at.placeY = 1.0f;
	const genes::Place ey = PatternDevice::placeOf(at, residue);
	double s11 = 0.0;
	double s12 = 0.0;
	double s22 = 0.0;
	double t1 = 0.0;
	double t2 = 0.0;
	const auto visit = [&](auto&& row) {
		for (size_t i = 0; i < 2 * genes::kRowCount; ++i) {
			const bool phase = i < genes::kRowCount;
			const size_t k = phase ? i : i - genes::kRowCount;
			const double base = phase ? o.phase[k] : o.rate[k];
			const double a = (phase ? ex.phase[k] : ex.rate[k]) - base;
			const double b = (phase ? ey.phase[k] : ey.rate[k]) - base;
			row(a, b, point[i] - base);
		}
	};
	visit([&](double a, double b, double r) {
		s11 += a * a;
		s12 += a * b;
		s22 += b * b;
		t1 += a * r;
		t2 += b * r;
	});
	PlaneMark mark;
	const double det = s11 * s22 - s12 * s12;
	if (!(det > 1e-300)) {
		return mark;
	}
	mark.x = (s22 * t1 - s12 * t2) / det;
	mark.y = (s11 * t2 - s12 * t1) / det;
	double off = 0.0;
	visit([&](double a, double b, double r) {
		const double miss = r - (a * mark.x + b * mark.y);
		off += miss * miss;
	});
	mark.offPlane = std::sqrt(off) / std::sqrt(std::max((s11 + s22) * 0.5, 1e-300));
	mark.solved = true;
	return mark;
}

std::array<double, 2 * genes::kRowCount> pointOf(const genes::Place& place) noexcept {
	std::array<double, 2 * genes::kRowCount> out {};
	for (size_t i = 0; i < genes::kRowCount; ++i) {
		out[i] = place.phase[i];
		out[genes::kRowCount + i] = place.rate[i];
	}
	return out;
}

void PatternDevice::decodeHand() noexcept {
	handDecoded_ = handStateOf(handCopy_.values.data(), handCopy_.values.size());
	cycleLengthDirty_ = true; // a residue moves a Progression's place, so its length
}

bool PatternDevice::writeHand(const float* in, size_t n) noexcept {
	if (n != kHandWords) {
		return false;
	}
	device::BlockReader check(in, n);
	for (size_t i = 0; i < n; ++i) {
		check.u16();
	}
	if (!check.ok()) {
		return false;
	}
	hand_.write(in, n);
	return true;
}

bool PatternDevice::writeLikes(const float* in, size_t n) noexcept {
	likes::AllLikes check {};
	if (!likes::read(in, n, &check)) {
		return false;
	}
	likes_.write(in, n);
	// THE WARP IS DERIVED IN THE BACKGROUND (design §14-quater): this returns at once, the pattern plays as it did until
	// the fit is published, and a newer write supersedes a fit still running.
	try {
		requestWarp(check);
	}
	catch (...) {
		// out of memory for the request: the pattern keeps playing the warp it has (or plain)
	}
	return true;
}

bool fitLikesWarp(const likes::SlotLikes& slot, genes::Warp& out, const std::function<bool()>& stop) {
	std::vector<genes::SiteAngles> angles;
	for (size_t i = 0; i < std::min(slot.count, likes::kMaxLikes); ++i) {
		genes::Place place;
		for (size_t r = 0; r < genes::kRowCount; ++r) {
			place.phase[r] = slot.like[i].point[r];
			place.rate[r] = slot.like[i].point[genes::kRowCount + r];
		}
		angles.push_back(genes::anglesOf(place));
	}
	return genes::buildWarp(angles, kLikesWarpStrength, out, {}, stop);
}

void PatternDevice::requestWarp(const likes::AllLikes& all) {
	auto copy = std::make_shared<const likes::AllLikes>(all);
	deriver_.request([this, copy](const device::Deriver::Job& job) { fitAndPublish(*copy, job); });
}

void PatternDevice::fitAndPublish(const likes::AllLikes& all, const device::Deriver::Job& job) {
	auto fresh = std::make_unique<std::array<genes::Warp, kPatterns>>();
	std::array<bool, kPatterns> live {};
	const auto stop = [&job] { return job.stopped(); };
	for (size_t p = 0; p < kPatterns; ++p) {
		live[p] = all[p].count > 0;
		if (live[p] && !fitLikesWarp(all[p], (*fresh)[p], stop)) {
			return; // SUPERSEDED: a newer request will publish
		}
	}
	WarpBank* bank = warpBank_.load(std::memory_order_acquire);
	if (bank == nullptr) {
		warpOwned_ = std::make_unique<WarpBank>();
		bank = warpOwned_.get();
		warpBank_.store(bank, std::memory_order_release);
	}
	// THE BUFFER THE AUDIO THREAD IS NOT IN: the worker is the only writer, so the back stays back until we flip.
	const uint32_t back = 1u - warpFront_.load(std::memory_order_seq_cst);
	while (warpReading_.load(std::memory_order_seq_cst) == back) {
		if (job.stopped()) {
			return;
		}
		std::this_thread::yield();
	}
	WarpBuffer& b = bank->buffer[back];
	b.warp = *fresh;
	b.live = live;
	b.serial = job.serial();
	// ONLY THE NEWEST REQUEST FLIPS: a fit superseded while it copied publishes nothing.
	(void)deriver_.publish(job, [&] {
		warpFront_.store(back, std::memory_order_seq_cst);
		warpPublishes_.fetch_add(1, std::memory_order_relaxed);
	});
}

const genes::Warp* PatternDevice::publishedWarpForTest(size_t p) const noexcept {
	const WarpBank* bank = warpBank_.load(std::memory_order_acquire);
	if (bank == nullptr) {
		return nullptr;
	}
	const WarpBuffer& b = bank->buffer[warpFront_.load()];
	return b.serial != 0 && b.live[p] ? &b.warp[p] : nullptr;
}

uint64_t PatternDevice::publishedSerialForTest() const noexcept {
	const WarpBank* bank = warpBank_.load(std::memory_order_acquire);
	return bank == nullptr ? 0 : bank->buffer[warpFront_.load()].serial;
}

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	const auto& d = *static_cast<const PatternDevice*>(storage);
	switch (index) {
	case 0: return d.readPlay(out, capacity);
	case 1: return d.readHand(out, capacity);
	case kLikesBlock: return d.readLikes(out, capacity);
	case kEditsBlock: return d.readEdits(out, capacity);
	default: return 0;
	}
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	auto& d = *static_cast<PatternDevice*>(storage);
	switch (index) {
	case 0: return d.restorePlay(in, n);
	case 1: return d.writeHand(in, n);
	case kLikesBlock: return d.writeLikes(in, n);
	case kEditsBlockV3: return d.restoreEditsV3(in, n);
	case kEditsBlock: return d.restoreEditsV4(in, n);
	default: return false;
	}
}

bool applyBlock(void* storage, size_t index, const float* in, size_t n) {
	auto& d = *static_cast<PatternDevice*>(storage);
	switch (index) {
	case 1: return d.writeHand(in, n);
	case kLikesBlock: return d.writeLikes(in, n);
	case kEditsBlock: return d.writeEdits(in, n);
	default: return false;
	}
}

//------------------------------------------------------------------------------------------------//
// The tables                                                                                     //
//------------------------------------------------------------------------------------------------//

namespace {
PatternDevice& asDevice(void* storage) noexcept { return *static_cast<PatternDevice*>(storage); }
const PatternDevice& asDevice(const void* storage) noexcept { return *static_cast<const PatternDevice*>(storage); }
void setFieldsOf(void* storage, const float* fields, size_t count) { asDevice(storage).setFields(fields, count); }
} // namespace

const device::NoteOps kNoteOps {
	.stateBytes = sizeof(PatternDevice),
	.stateAlign = alignof(PatternDevice),
	.construct = [](void* storage) { new (storage) PatternDevice(); },
	.destruct = [](void* storage) { asDevice(storage).~PatternDevice(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.setFields = setFieldsOf,
	.processNotes =
	    [](void* storage, const device::NoteEvent* in, size_t inCount, device::NoteEvent* out, size_t outCapacity,
	       size_t* outCount) { asDevice(storage).processNotes(in, inCount, out, outCapacity, outCount); },
	.emitNotes =
	    [](void* storage, const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
	       size_t outCapacity, uint32_t* samplesUntilNext) {
		    return asDevice(storage).emit(clock, sampleNow, out, outCapacity, samplesUntilNext);
	    },
	.seedPassed = [](void* storage, const device::NoteEvent* ons, size_t count) { asDevice(storage).seedPassed(ons, count); },
};

const device::ModOps kModOps {
	.stateBytes = sizeof(PatternDevice),
	.stateAlign = alignof(PatternDevice),
	.construct = [](void* storage) { new (storage) PatternDevice(); },
	.destruct = [](void* storage) { asDevice(storage).~PatternDevice(); },
	.prepare = [](void* storage, double sampleRate) { asDevice(storage).prepare(sampleRate); },
	.reset = [](void* storage) { asDevice(storage).reset(); },
	.setFields = setFieldsOf,
	.blockSamples = 32,
	.tick = [](void* storage, const device::ModTickContext&) { asDevice(storage).tick(); },
	.outputAt = [](const void* storage, size_t output) { return asDevice(storage).outputAt(output); },
	.playhead = [](const void* storage, uint32_t* ordinal, float* phase) { asDevice(storage).playhead(ordinal, phase); },
	.playheadLevel = [](const void* storage) { return asDevice(storage).pendingLevel(); },
	.recentEvents = [](const void* storage, device::RecentEvent* out, size_t capacity) {
		return asDevice(storage).recentEvents(out, capacity);
	},
};

} // namespace feathers::mod::arp::pat
