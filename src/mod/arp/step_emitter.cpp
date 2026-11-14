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

#include "mod/arp/step_emitter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace feathers::mod::arp {

namespace {
constexpr uint32_t kNever = std::numeric_limits<uint32_t>::max();
/// "CALL AGAIN, NO SOONER THAN ANYTHING ELSE ASKS": what an armed emitter names. Finite, so a host reads it as an event still
/// to come; as late as a call can be, so it limits no chunk.
constexpr uint32_t kArmedWake = kNever - 1;
} // namespace

void StepEmitter::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 && std::isfinite(sampleRate) ? sampleRate : 48000.0;
	reset();
}

void StepEmitter::reset() noexcept {
	// THE HOST HAS ALREADY SILENCED ITS VOICES (an activate or a reset), so what was sounding is forgotten rather than
	// ended: an off for a voice that is gone would only be noise in the stream.
	soundingCount_ = 0;
	pendingCount_ = 0;
	clock_ = Clock::None;
	seated_ = false;
	freeRunning_ = false;
	freeSps_ = 0.0;
	passedCount_ = 0; // the thru notes went with the voices
	restartPending_ = false;
	noteRestart_ = false;
	originU_ = 0.0;
}

//------------------------------------------------------------------------------------------------//
// The held chord                                                                                 //
//------------------------------------------------------------------------------------------------//

void StepEmitter::processNotes(const device::NoteEvent* in, size_t inCount, device::NoteEvent* out,
                               size_t outCapacity, size_t* outCount) noexcept {
	// THE CHORD IS FOLDED, AND NOTHING IS EMITTED: a host hands the lane one note per call, so a chord arrives as several
	// calls and a step emitted here would play its first note alone. Off, the stream passes on untouched; on, the keys
	// are swallowed, except an off for a key whose on went straight through while the arp was off — and, under THRU, the
	// keys it names, which pass untouched and never reach the chord (design §7.5).
	size_t n = 0;
	const auto pass = [&](const device::NoteEvent& e) {
		if (n < outCapacity && out != nullptr) {
			out[n++] = e;
		}
	};
	for (size_t i = 0; i < inCount; ++i) {
		const device::NoteEvent& e = in[i];
		switch (e.kind) {
		case device::NoteEvent::Kind::On: {
			if (c_.enabled && c_.passes(e.key)) {
				// A THRU NOTE: out as it came, and remembered so its off follows it whatever Thru says by then.
				if (c_.run == RunMode::Note && physical_.empty() && passedCount_ == 0 && chord_.empty()) {
					noteRestart_ = true; // NOTE: a key passing Thru starts it too, from step 1
				}
				if (passedCount_ < kPassedCapacity) {
					passed_[passedCount_++] = Passed { e.key, e.channel, e.noteId };
				}
				pass(e);
				break;
			}
			const bool fresh = physical_.empty();
			const auto velocity = static_cast<float>(e.velocity);
			// ONE PRESS PER GROUP (backlog QR): keys struck between two clock calls (one block, a chord played at once) share an
			// order, so "the newest press" is the whole chord, and Transpose takes its lowest key (`pattern::transposeRootOf`).
			if (!pressGroupOpen_) {
				++pressCount_;
				pressGroupOpen_ = true;
			}
			physical_.press(e.key, e.channel, velocity, pressCount_);
			// LATCH: a chord played after every key was let go replaces the latched one; while any key is still down,
			// a press adds to it.
			if (c_.latch && fresh) {
				chord_.clear();
			}
			// NOTE (Run): the first key after every key was let go starts the pattern over from step 1 — Latch's held chord
			// included, which never empties for the clock to restart on by itself.
			if (c_.run == RunMode::Note && fresh && passedCount_ == 0) {
				noteRestart_ = true;
			}
			chord_.press(e.key, e.channel, velocity, pressCount_);
			if (!c_.enabled) {
				physical_.find(e.key)->passed = true;
				pass(e);
			}
			else if (c_.trig == TrigMode::Restart) {
				restartPending_ = true; // RESTART: step 0 lands on this note's sample
			}
			break;
		}
		case device::NoteEvent::Kind::Off: {
			// AN OFF PASSES when its on did — or Thru names the key and the arp never took its on (a key past the table). A key
			// the arp HOLDS is released here even when Thru names it by now (a switch to a pattern with Thru under held keys):
			// passing its off would leave it held in the chord for ever and send the synth an off it never had an on for.
			// A PASSED NOTE'S OFF goes where the on went, carrying the on's id and channel where the host's off is a wildcard
			// (backlog QU: the voices end exactly that note, never an arp note on the same key).
			if (Passed was {}; takePassed(e, &was)) {
				device::NoteEvent o = e;
				o.noteId = o.noteId < 0 ? was.noteId : o.noteId;
				o.channel = o.channel < 0 ? was.channel : o.channel;
				pass(o);
				break;
			}
			if (c_.enabled && c_.passes(e.key) && physical_.find(e.key) == nullptr) {
				pass(e);
				break;
			}
			const HeldNote* held = physical_.find(e.key);
			// **AN OFF FOR A KEY THIS EMITTER NEVER TOOK** (backlog QU, the fallback behind the lane's seeding): its on reached
			// the voices some other way, so the off goes there too — swallowing it left a voice on for ever.
			if (held == nullptr && c_.enabled) {
				pass(e);
				break;
			}
			const bool passed = held != nullptr && held->passed;
			physical_.release(e.key);
			if (!c_.latch) {
				chord_.release(e.key);
			}
			if (!c_.enabled || passed) {
				pass(e);
			}
			break;
		}
		case device::NoteEvent::Kind::Choke:
			// A HOST'S CHOKE ENDS THE NOTE (CLAP's meaning): a wildcard clears the chord, a keyed one lets that key go as an
			// Off would (Latch keeps its chord, as it does for an Off). THE SYNTH'S OWN STEAL (`fromSteal`,
			// `Synth::chokeLaneNote`) is about a voice playing a STEP, not a held key — so it moves nothing here. Told apart
			// by source: before 2026-11-04 a keyed host choke was taken for a steal, and a choked key played on unheld.
			if (e.key < 0) {
				physical_.clear();
				chord_.clear();
			}
			else if (!e.fromSteal) {
				physical_.release(e.key);
				if (!c_.latch) {
					chord_.release(e.key);
				}
			}
			if (!c_.enabled) {
				pass(e);
			}
			else if (e.key < 0) {
				// A WILDCARD CHOKE ends every passed note downstream too.
				if (c_.thru || passedCount_ > 0) {
					passedCount_ = 0;
					pass(e);
				}
			}
			else if (Passed was {}; takePassed(e, &was)) {
				// A CHOKE OF A PASSED NOTE goes where the note went (its id and channel where the host's are wildcards).
				device::NoteEvent o = e;
				o.noteId = o.noteId < 0 ? was.noteId : o.noteId;
				o.channel = o.channel < 0 ? was.channel : o.channel;
				pass(o);
			}
			break;
		case device::NoteEvent::Kind::Expression:
			pass(e);
			break;
		}
	}
	if (outCount != nullptr) {
		*outCount = n;
	}
}

bool StepEmitter::takePassed(const device::NoteEvent& e, Passed* found) noexcept {
	// CLAP'S WILDCARDS: a channel or note id of −1 on the off matches any.
	for (size_t i = 0; i < passedCount_; ++i) {
		const Passed& p = passed_[i];
		const bool channel = e.channel < 0 || p.channel == e.channel;
		const bool id = e.noteId < 0 || p.noteId < 0 || p.noteId == e.noteId;
		if (p.key == e.key && channel && id) {
			if (found != nullptr) {
				*found = p;
			}
			passed_[i] = passed_[--passedCount_];
			return true;
		}
	}
	return false;
}

//------------------------------------------------------------------------------------------------//
// The sounding table                                                                             //
//------------------------------------------------------------------------------------------------//

bool StepEmitter::push(const device::NoteEvent& e) noexcept {
	if (out_ == nullptr || outCount_ >= outCapacity_) {
		return false;
	}
	out_[outCount_++] = e;
	return true;
}

bool StepEmitter::endSounding(size_t i) noexcept {
	const Sounding& note = sounding_[i];
	device::NoteEvent off {};
	off.kind = device::NoteEvent::Kind::Off;
	off.noteId = -1;
	off.channel = note.channel;
	off.key = note.key;
	off.velocity = 0.0;
	if (!push(off)) {
		// NO ROOM: kept, and due at once, so the next call (one sample on) writes it. Delayed, never lost.
		sounding_[i].offU = -std::numeric_limits<double>::infinity();
		return false;
	}
	std::copy(sounding_.begin() + static_cast<std::ptrdiff_t>(i) + 1,
	          sounding_.begin() + static_cast<std::ptrdiff_t>(soundingCount_), sounding_.begin() + static_cast<std::ptrdiff_t>(i));
	--soundingCount_;
	return true;
}

void StepEmitter::endAll() noexcept {
	size_t i = 0;
	while (i < soundingCount_) {
		if (!endSounding(i)) {
			++i;
		}
	}
}

bool StepEmitter::sound(int16_t key, int16_t channel, double velocity, double onU, double offU) noexcept {
	// A TIE INTO THE SAME KEY RE-STRIKES IT: the off first. A note with no id is addressed by its key, so two
	// overlapping on one key would be ended by whichever off came first.
	bool room = true;
	for (size_t i = 0; i < soundingCount_; ++i) {
		if (sounding_[i].key == key && sounding_[i].channel == channel) {
			room = endSounding(i);
			break;
		}
	}
	if (room && soundingCount_ >= limit_) {
		room = endSounding(0);
	}
	device::NoteEvent ev {};
	ev.kind = device::NoteEvent::Kind::On;
	ev.noteId = -1; // invented: CLAP's "unspecified", matched by key (rack-note-lane §3.1)
	ev.channel = channel;
	ev.key = key;
	ev.velocity = velocity;
	// A FULL BUFFER DROPS THIS ON — the later event — and nothing sounds that would need an off.
	if (!room || !push(ev)) {
		return false;
	}
	sounding_[soundingCount_++] = Sounding { key, channel, onU, offU };
	return true;
}

void StepEmitter::regrid(const ClockSettings& c, double factor) noexcept {
	c_ = c;
	if (clock_ == Clock::Free) {
		// THE FREE CLOCK RE-ORIGINS HERE, AT THIS CALL'S SAMPLE, so the new step length runs from the switch: left to the
		// next call's re-origin, the sample between would be counted at the old length (half a sample lost a switch).
		freeOriginU_ = lastU_;
		freeStartSample_ = callSample_;
		freeSps_ = 0.0;
		return;
	}
	if (clock_ != Clock::Song || !(factor > 0.0) || !std::isfinite(factor) || factor == 1.0) {
		return;
	}
	for (size_t i = 0; i < soundingCount_; ++i) {
		sounding_[i].onU *= factor;
		sounding_[i].offU *= factor;
	}
	for (size_t i = 0; i < pendingCount_; ++i) {
		pending_[i].onU *= factor;
		pending_[i].offU *= factor;
	}
	originU_ *= factor;
	lastU_ *= factor;
}

bool StepEmitter::extend(int16_t key, int16_t channel, double offU) noexcept {
	for (size_t i = 0; i < soundingCount_; ++i) {
		if (sounding_[i].key == key && sounding_[i].channel == channel) {
			sounding_[i].offU = std::max(sounding_[i].offU, offU);
			return true;
		}
	}
	return false;
}

bool StepEmitter::schedule(int16_t key, int16_t channel, double velocity, double onU, double offU) noexcept {
	if (pendingCount_ >= kPendingCapacity) {
		return false;
	}
	pending_[pendingCount_++] = Pending { key, channel, velocity, onU, offU };
	return true;
}

void StepEmitter::noteRecent(uint32_t slot, float value) noexcept {
	recent_[slot % device::kRecentEvents] = { slot, value };
	recentCount_ = recentCount_ > 0 && lastSlot_ == slot - 1u ? std::min(recentCount_ + 1, device::kRecentEvents) : 1;
	lastSlot_ = slot;
}

//------------------------------------------------------------------------------------------------//
// The clock                                                                                      //
//------------------------------------------------------------------------------------------------//

bool StepEmitter::sounding(int16_t key, int16_t channel) const noexcept {
	for (size_t i = 0; i < soundingCount_; ++i) {
		if (sounding_[i].key == key && sounding_[i].channel == channel) {
			return true;
		}
	}
	return false;
}

size_t StepEmitter::emit(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out,
                         size_t capacity, uint32_t* samplesUntilNext, PlaySlot play, void* ctx,
                         Between between) noexcept {
	wake_ = kNever;
	callSample_ = sampleNow;
	pressGroupOpen_ = false; // the next press starts a new group
	out_ = out;
	outCapacity_ = out == nullptr ? 0 : capacity;
	outCount_ = 0;
	uint32_t next = kNever;
	const auto finish = [&]() {
		// ANYTHING STILL SOUNDING HERE IS AN OFF THAT DID NOT FIT: back next sample.
		if (soundingCount_ > 0 && next == kNever) {
			next = 1;
		}
		// WHILE THE FREE CLOCK RUNS, ITS PHASE IS PUBLISHED HERE, at every clock call (each render chunk), and not only
		// at the modulator tick: the tick comes every 64 samples and carries the phase of the call before it, so a save
		// would land up to two chunks late. From here it is at most one chunk (≤ 32 samples) — measured by the
		// whole-plugin case in test_engine_restart, which read 64 before this.
		if (freeRunning_) {
			publish();
		}
		if (samplesUntilNext != nullptr) {
			*samplesUntilNext = std::max<uint32_t>(next, 1);
		}
		return outCount_;
	};

	// SWITCHED OFF: every step it sounds gets its off, and the clock forgets where it was.
	if (!c_.enabled) {
		pendingCount_ = 0;
		if (enabledWas_ || soundingCount_ > 0) {
			endAll();
			clock_ = Clock::None;
			seated_ = false;
			freeRunning_ = false;
		}
		enabledWas_ = false;
		return finish();
	}
	if (!enabledWas_) {
		// SWITCHED ON WITH KEYS DOWN: their ons went straight to the voices, so their offs go now and the arp plays them.
		bool allEnded = true;
		for (size_t i = 0; i < physical_.size(); ++i) {
			HeldNote& h = physical_[i];
			if (!h.passed) {
				continue;
			}
			device::NoteEvent off {};
			off.kind = device::NoteEvent::Kind::Off;
			off.noteId = -1;
			off.channel = h.channel;
			off.key = h.key;
			off.velocity = 0.0;
			if (push(off)) {
				h.passed = false;
			}
			else {
				allEnded = false;
			}
		}
		if (!allEnded) {
			next = 1;
		}
		enabledWas_ = allEnded;
	}
	// LATCH LET GO: the chord is the keys still down, and when none are, the release below ends the arp.
	if (latchWas_ && !c_.latch) {
		chord_ = physical_;
	}
	latchWas_ = c_.latch;

	// WHICH CLOCK. The song's whenever the transport plays and the timeline this rate is measured in is known (beats
	// for a division, seconds for Hz); otherwise a free clock, unless Stop with Transport holds it silent while stopped.
	const bool synced = c_.synced;
	const bool songKnown = clock.playing
	                       && (synced ? clock.havePositionBeats() && clock.haveTempo() : clock.havePositionSeconds());
	Clock want = Clock::None;
	// NOTE (Run): it runs while a KEY is held — a trigger (or Latch's chord) or a key passing Thru — whatever Trig says.
	const bool running = c_.run == RunMode::Note ? !chord_.empty() || passedCount_ > 0 : !chord().empty();
	if (running) { // (Transport and Restart: never empty — no trigger gates them)
		if (c_.run == RunMode::Note) {
			want = Clock::Free; // NOTE: its own clock from the key, whatever the transport says
		}
		else if (songKnown) {
			want = Clock::Song;
		}
		else if (clock.playing || c_.run != RunMode::Transport) {
			want = Clock::Free;
		}
	}
	const StepGrid& grid = c_.grid;
	if (want != clock_) {
		// A CHANGE OF CLOCK (a chord released, the transport started or stopped): what sounds ends here, and the next
		// step is the new clock's. A transport (re)start therefore always lands on the song's grid.
		endAll();
		pendingCount_ = 0;
		seated_ = false;
		if (want == Clock::Free) {
			if (haveFreeRestore_) {
				freeOriginU_ = freeRestore_; // a reload carries on where the saved free clock was
				haveFreeRestore_ = false;
			}
			else if (clock_ == Clock::Song) {
				// THE TRANSPORT STOPPED UNDER HELD KEYS: carry on from the song's phase AT THIS SAMPLE — a stopped host still
				// says where it stopped. The last call's position is up to a chunk old, and would replay its step.
				freeOriginU_ = lastU_;
				const double stepSecs = synced && clock.haveTempo() ? c_.stepBeats * clock.secondsPerBeat
				                                                    : (c_.stepHz > 0.0 ? 1.0 / c_.stepHz : 0.0);
				if (stepSecs > 0.0 && std::isfinite(stepSecs)) {
					const double where = synced ? clock.songPositionBeats * clock.secondsPerBeat : clock.songPositionSeconds;
					if (std::isfinite(where)) {
						freeOriginU_ = where / std::max(stepSecs, kMinStepSeconds);
					}
				}
			}
			else {
				freeOriginU_ = grid.onset(0); // the first key press: step 0 lands on this sample
			}
			freeStartSample_ = sampleNow;
			freeSps_ = 0.0;
		}
		originU_ = 0.0; // A NEW CLOCK STARTS ON ITS OWN GRID: a restart's re-phasing belonged to the clock before
		freeRunning_ = want == Clock::Free;
		clock_ = want;
	}
	if (clock_ == Clock::None) {
		restartPending_ = false; // nothing runs, so nothing restarts: a trigger while silent is not saved for later
		noteRestart_ = false;
		// **ARMED: IT WOULD PLAY, AND ONLY THE STOPPED TRANSPORT HOLDS IT** (Run Transport over a chord, or Trig Transport
		// or Restart with none held). The transport starting is no input event, and a host told SLEEP need not call again
		// until one, so the first steps of the take would be lost. It names a call (`kArmedWake`, no time at all) and so
		// keeps the plugin awake (`RackEngine::notesScheduled`); with nothing that could play, it names none.
		if (running && next == kNever) {
			next = kArmedWake;
		}
		return finish();
	}

	// WHERE THE GRID IS, in steps, and how many samples one step lasts.
	double stepSeconds = 0.0;
	if (synced && clock.haveTempo()) {
		stepSeconds = c_.stepBeats * clock.secondsPerBeat;
	}
	else {
		const double hz = c_.stepHz;
		stepSeconds = hz > 0.0 ? 1.0 / hz : 1.0;
	}
	if (!(stepSeconds >= kMinStepSeconds) || !std::isfinite(stepSeconds)) {
		stepSeconds = std::isfinite(stepSeconds) && stepSeconds > 0.0 ? kMinStepSeconds : 1.0;
	}
	const double sps = stepSeconds * sampleRate_;
	sps_ = sps;
	double u = 0.0;
	if (clock_ == Clock::Song) {
		u = synced ? clock.songPositionBeats * clock.secondsPerBeat / stepSeconds
		           : clock.songPositionSeconds / stepSeconds;
	}
	else {
		if (freeSps_ > 0.0 && freeSps_ != sps) {
			// THE RATE MOVED: re-origin where the clock stands, so it changes speed rather than jumping.
			freeOriginU_ += static_cast<double>(sampleNow - freeStartSample_) / freeSps_;
			freeStartSample_ = sampleNow;
		}
		freeSps_ = sps;
		u = freeOriginU_ + static_cast<double>(sampleNow - freeStartSample_) / sps;
	}
	lastU_ = u;
	// **RESTART** (Trig, design §7.5): a trigger's on moves the grid's origin so slot 0's onset lands on this sample. What
	// sounds ends, what was queued is forgotten, and the grid re-seats on slot 0. From then on the slots are the running
	// clock's less the origin, so the same restart sample gives the same notes.
	if (restartPending_ || noteRestart_) {
		const bool note = noteRestart_;
		restartPending_ = false;
		noteRestart_ = false;
		// NOTE'S RESTART (Run): the same re-seat, on the first key after a full release.
		if (c_.trig == TrigMode::Restart || (note && c_.run == RunMode::Note)) {
			originU_ = u - grid.onset(0);
			endAll();
			pendingCount_ = 0;
			seated_ = false;
		}
	}
	u -= originU_;
	// HOW MANY SAMPLES FROM NOW TO A POINT ON THE GRID: the first sample at or after it, so 0 means "this one" (and an
	// onset up to a sample behind is still this sample's — the call lands on the first sample after it).
	const auto samplesTo = [&](double target) { return std::ceil((target - u) * sps - 1e-6); };
	const auto toNext = [&](double samples) {
		if (samples >= static_cast<double>(kNever)) {
			return;
		}
		next = std::min(next, static_cast<uint32_t>(std::max(samples, 1.0)));
	};

	// THE OFFS THAT ARE DUE — and any whose ON is now in the future, because the song jumped back past it.
	for (size_t i = 0; i < soundingCount_;) {
		const Sounding& n = sounding_[i];
		if (samplesTo(n.offU) <= 0.0 || samplesTo(n.onU) > 0.0) {
			if (endSounding(i)) {
				continue;
			}
		}
		++i;
	}

	// SEATED ON THE GRID: the slot expected next is the first whose onset is less than a sample behind now. A jump
	// (a scrub, a loop, a Rate change on the song clock) re-seats; a loop landing on an onset plays it.
	const int64_t g0 = grid.firstAtOrAfter(u, (1.0 - 1e-6) / sps);
	if (!seated_ || nextSlot_ < g0 || nextSlot_ > g0 + 1) {
		nextSlot_ = g0;
		seated_ = true;
		pendingCount_ = 0; // A JUMP: what was queued belonged to the steps it jumped from
	}

	// THE QUEUED ONS THAT ARE DUE — after the offs, so a ratchet's hit on its own key is struck after the hit before ends.
	for (size_t i = 0; i < pendingCount_;) {
		if (samplesTo(pending_[i].onU) <= 0.0) {
			const Pending p = pending_[i];
			std::copy(pending_.begin() + static_cast<std::ptrdiff_t>(i) + 1,
			          pending_.begin() + static_cast<std::ptrdiff_t>(pendingCount_), pending_.begin() + static_cast<std::ptrdiff_t>(i));
			--pendingCount_;
			sound(p.key, p.channel, p.velocity, p.onU, p.offU);
			continue;
		}
		++i;
	}

	if (samplesTo(grid.onset(nextSlot_)) <= 0.0) {
		const int64_t g = nextSlot_;
		++nextSlot_;
		if (play != nullptr) {
			play(ctx, g, *this);
		}
	}

	if (between != nullptr) {
		between(ctx, u, sps, *this);
		if (wake_ != kNever) {
			toNext(static_cast<double>(wake_));
		}
	}

	toNext(samplesTo(grid.onset(nextSlot_)));
	for (size_t i = 0; i < soundingCount_; ++i) {
		toNext(samplesTo(sounding_[i].offU));
	}
	for (size_t i = 0; i < pendingCount_; ++i) {
		toNext(samplesTo(pending_[i].onU));
	}
	return finish();
}

size_t StepEmitter::recentEvents(device::RecentEvent* out, size_t capacity) const noexcept {
	if (recentCount_ == 0) {
		return 0;
	}
	// NEWEST FIRST: the run of slots ending at the newest, as long as it is unbroken.
	size_t n = 0;
	uint32_t want = lastSlot_;
	for (; n < capacity && n < recentCount_; ++n, --want) {
		const device::RecentEvent& e = recent_[want % device::kRecentEvents];
		if (e.ordinal != want) {
			break;
		}
		out[n] = e;
	}
	return n;
}

void StepEmitter::playhead(uint32_t* ordinal, float* phase) const noexcept {
	if (ordinal != nullptr) {
		*ordinal = recentCount_ > 0 ? lastSlot_ : mod::Playhead::kIdle;
	}
	if (phase != nullptr) {
		*phase = 0.0f;
	}
}

//------------------------------------------------------------------------------------------------//
// What persists                                                                                  //
//------------------------------------------------------------------------------------------------//

void StepEmitter::publish() noexcept {
	// LATCH'S CHORD only while Latch holds it — a chord under the fingers is the host's to send again, not the patch's;
	// and the FREE CLOCK'S PHASE only while it runs — on the song clock the position is the song's (derived).
	std::array<float, kPlayWords> words {};
	device::BlockWriter w(words.data(), words.size());
	const bool latched = c_.latch && !chord_.empty();
	w.flag(latched);
	const size_t count = latched ? chord_.size() : 0;
	w.u16(static_cast<uint32_t>(count));
	for (size_t i = 0; i < kMaxHeld; ++i) {
		const HeldNote h = i < count ? chord_[i] : HeldNote {};
		w.u16(static_cast<uint32_t>(std::clamp<int>(h.key, 0, 127)));
		w.u16(static_cast<uint32_t>(std::clamp<int>(h.channel, 0, 15)));
		w.f32(h.velocity);
		w.u32(h.order);
	}
	w.flag(freeRunning_);
	w.f64(freeRunning_ ? lastU_ : 0.0);
	play_.publish(words.data(), words.size());
}

bool StepEmitter::restorePlay(const float* in, size_t n) noexcept {
	if (n != kPlayWords) {
		return false;
	}
	device::BlockReader r(in, n);
	const bool latched = r.flag();
	const size_t count = std::min<size_t>(r.u16(), kMaxHeld);
	HeldChord chord;
	uint32_t newest = 0;
	for (size_t i = 0; i < kMaxHeld; ++i) {
		const auto key = static_cast<int16_t>(r.u16());
		const auto channel = static_cast<int16_t>(r.u16());
		const float velocity = r.f32();
		const uint32_t order = r.u32();
		if (i < count && latched) {
			chord.press(key, channel, std::isfinite(velocity) ? std::clamp(velocity, 0.0f, 1.0f) : 1.0f, order);
			newest = std::max(newest, order);
		}
	}
	const bool free = r.flag();
	const double phase = r.f64();
	if (!r.ok()) {
		return false;
	}
	chord_ = chord;
	latchWas_ = latched;
	pressCount_ = std::max(pressCount_, newest);
	haveFreeRestore_ = free && std::isfinite(phase);
	freeRestore_ = haveFreeRestore_ ? phase : 0.0;
	// THE RESTORED WORDS, VERBATIM — NOT `publish()`, which derives "latched" from the settings, and a restore runs
	// before the row's `setFields`: Latch would read Off, and a host that rebuilds the instance at activate refreshes
	// its store from this snapshot (measured: the whole-plugin case lost the chord exactly that way).
	play_.publish(in, n);
	return true;
}

} // namespace feathers::mod::arp
