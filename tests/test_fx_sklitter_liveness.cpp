/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

// **LIVENESS UNDER A HAND THAT KEEPS TURNING MODE.** After many mode switches the playhead could stop
// and engage do nothing.
//
// A seeded fuzz drives a real `Sklitter` on a rolling transport — random mode switches, random presses,
// the lock on and off, random block sizes — and after every block asks the two things that were seen to fail:
// an engaged device on a rolling song reaches Playing within its documented wait, and a Playing one's
// cursor moves. The first violation prints the seed and the state.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kSr = 44100.0;
constexpr double kSpb = 0.5; // 120 BPM

struct Rng {
	uint64_t s;
	uint32_t next() {
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return static_cast<uint32_t>(s >> 11);
	}
	double unit() { return static_cast<double>(next() & 0xFFFFFFu) / static_cast<double>(0x1000000); }
	int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

struct Event {
	long long frame;
	std::string what;
};

const char* statusName(Status s) {
	switch (s) {
	case Status::Off: return "Off";
	case Status::Recording: return "Recording";
	case Status::Standby: return "Standby";
	case Status::Playing: return "Playing";
	}
	return "?";
}

/// One fuzz run. Returns an empty string when it stayed live, else the report.
std::string fuzzOnce(uint64_t seed, double seconds) {
	Rng rng { seed * 2654435761ull + 0x9E3779B97F4A7C15ull };
	Sklitter s;
	s.prepare(kSr, 4096);
	Settings cfg;
	cfg.enabled = true;
	cfg.engage = false;
	cfg.mode = Mode::Shuffle;
	cfg.transportLocked = (rng.next() & 1u) != 0u;
	s.setTarget(cfg);

	std::vector<float> l(2048);
	std::vector<float> r(2048);
	double phase = 0.0;
	double beats = 0.0;
	bool rolling = true;
	const bool stops = std::getenv("SKLITTER_FUZZ_STOPS") != nullptr;
	long long frame = 0;
	const auto total = static_cast<long long>(seconds * kSr);
	long long nextChange = 0;
	std::vector<Event> events;
	const auto log = [&](const std::string& w) {
		events.push_back({ frame, w });
		if (events.size() > 40) {
			events.erase(events.begin());
		}
	};

	// Liveness bookkeeping.
	long long engagedSince = -1;       // the frame the current press began, or -1
	long long releasedSince = -1;      // the frame the current release began, or -1
	long long stillSince = -1;         // the frame the playing cursor last moved
	size_t lastPos = SIZE_MAX;
	int32_t lastSlice = -1;
	size_t lastLinear = SIZE_MAX;

	while (frame < total) {
		if (frame >= nextChange) {
			// A CHANGE: mostly Mode, sometimes Engage, sometimes the lock or Rate or a zone.
			const int what = rng.range(0, 9);
			char buf[160];
			if (what <= 4) {
				cfg.mode = static_cast<Mode>(rng.range(0, static_cast<int>(Mode::kCount) - 1));
				if (std::getenv("SKLITTER_FUZZ_NOBURST") != nullptr && cfg.mode == Mode::Burst) {
					cfg.mode = Mode::Shuffle;
				}
				std::snprintf(buf, sizeof buf, "mode -> %s", kModeNames[static_cast<size_t>(cfg.mode)]);
			}
			else if (what <= 6) {
				cfg.engage = !cfg.engage;
				std::snprintf(buf, sizeof buf, "engage -> %d", cfg.engage ? 1 : 0);
			}
			else if (what == 7) {
				cfg.transportLocked = !cfg.transportLocked;
				std::snprintf(buf, sizeof buf, "lock -> %d", cfg.transportLocked ? 1 : 0);
			}
			else if (what == 8 && stops && (rng.next() & 1u) != 0u) {
				rolling = !rolling;
				std::snprintf(buf, sizeof buf, "transport -> %s", rolling ? "play" : "stop");
			}
			else if (what == 8) {
				cfg.rate = static_cast<float>(rng.unit());
				std::snprintf(buf, sizeof buf, "rate -> %.3f", static_cast<double>(cfg.rate));
			}
			else {
				cfg.zoneA = static_cast<float>(rng.unit() * 4.0);
				cfg.zoneB = static_cast<float>(rng.unit() * 4.0);
				std::snprintf(buf, sizeof buf, "zones -> %.3f %.3f", static_cast<double>(cfg.zoneA),
				              static_cast<double>(cfg.zoneB));
			}
			log(buf);
			s.setTarget(cfg);
			nextChange = frame + static_cast<long long>((0.005 + rng.unit() * 0.495) * kSr);
		}

		const int block = rng.range(128, 1024);
		const int n = static_cast<int>(std::min<long long>(block, total - frame));
		for (int k = 0; k < n; ++k) {
			l[static_cast<size_t>(k)] = static_cast<float>(0.5 * std::sin(phase));
			r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
			phase += 2.0 * 3.14159265358979 * 220.0 / kSr;
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(kSpb);
		c.songPositionBeats = beats;
		c.playing = rolling;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), n);
		frame += n;
		if (!rolling) {
			// A PARKED SONG: nothing advances and nothing is owed; the waits restart on the resume.
			engagedSince = cfg.engage ? frame : -1;
			releasedSince = cfg.engage ? -1 : frame;
			stillSince = -1;
			continue;
		}
		beats += static_cast<double>(n) / (kSpb * kSr);
		// A HOST LOOP REGION, when asked for: the song wraps at `loopBeats` as a DAW's cycle does.
		if (const char* loop = std::getenv("SKLITTER_FUZZ_LOOP"); loop != nullptr) {
			const double loopBeats = std::atof(loop);
			if (loopBeats > 0.0 && beats >= loopBeats) {
				beats -= loopBeats;
			}
		}

		// ENGAGE MUST REACH PLAYING within the capture of the buffer plus one beat's wait (bar-long
		// buffers need at most one bar to record and one grid line), with a generous margin.
		const Status st = s.statusForTest();
		if (cfg.engage) {
			if (engagedSince < 0) {
				engagedSince = frame;
			}
		}
		else {
			engagedSince = -1;
		}
		const auto bar = static_cast<long long>(4.0 * kSpb * kSr);
		if (cfg.engage && st != Status::Playing && engagedSince >= 0 && frame - engagedSince > static_cast<long long>(s.bufferFramesForTest()) + bar + 2048) {
			char buf[512];
			std::snprintf(buf, sizeof buf,
			              "seed %llu: ENGAGED FOR %lld FRAMES WITHOUT PLAYING at frame %lld: status=%s mode=%s "
			              "lock=%d buffer=%zu slice=%zu",
			              static_cast<unsigned long long>(seed), frame - engagedSince, frame, statusName(st),
			              kModeNames[static_cast<size_t>(cfg.mode)], cfg.transportLocked ? 1 : 0,
			              s.bufferFramesForTest(), s.sliceFramesForTest());
			std::string out = buf;
			for (const Event& e : events) {
				out += "\n  @" + std::to_string(e.frame) + " " + e.what;
			}
			return out;
		}
		// A RELEASE MUST END IT. Standby may outlive the release until its trigger (the reference way, by
		// design) — the rest of its capture plus a grid line — and must be Off by then; it must never PLAY.
		if (cfg.engage) {
			releasedSince = -1;
		}
		else if (releasedSince < 0) {
			releasedSince = frame;
		}
		if (!cfg.engage && st != Status::Off && releasedSince >= 0
		    && frame - releasedSince > static_cast<long long>(s.bufferFramesForTest()) + bar + 2048) {
			char buf[256];
			std::snprintf(buf, sizeof buf, "seed %llu: RELEASED %lld FRAMES AGO BUT STILL %s at frame %lld mode=%s",
			              static_cast<unsigned long long>(seed), frame - releasedSince, statusName(st), frame,
			              kModeNames[static_cast<size_t>(cfg.mode)]);
			return buf;
		}
		if (!cfg.engage && st == Status::Playing) {
			char buf[256];
			std::snprintf(buf, sizeof buf, "seed %llu: RELEASED BUT %s at frame %lld mode=%s",
			              static_cast<unsigned long long>(seed), statusName(st), frame,
			              kModeNames[static_cast<size_t>(cfg.mode)]);
			return buf;
		}

		// THE CURSOR MUST MOVE while Playing.
		if (st == Status::Playing) {
			bool moved = false;
			if (cfg.mode == Mode::Grain) {
				moved = s.linearBarPosForTest() != lastLinear;
			}
			else if (cfg.mode == Mode::Burst) {
				moved = s.burstCycleForTest() > 0 && s.burstGrainForTest() > 0;
			}
			else {
				moved = s.positionInSliceForTest() != lastPos || s.sliceIndexForTest() != lastSlice
				        || s.linearBarPosForTest() != lastLinear;
			}
			if (moved || stillSince < 0) {
				stillSince = frame;
			}
			else if (frame - stillSince > 4096) {
				char buf[640];
				std::snprintf(buf, sizeof buf,
				              "seed %llu: CURSOR STILL FOR %lld FRAMES at frame %lld: mode=%s lock=%d slice=%d/%d "
				              "pos=%zu linear=%zu buffer=%zu sliceFrames=%zu burstCycle=%zu burstGrain=%zu",
				              static_cast<unsigned long long>(seed), frame - stillSince, frame,
				              kModeNames[static_cast<size_t>(cfg.mode)], cfg.transportLocked ? 1 : 0,
				              s.sliceIndexForTest(), s.sliceCountForTest(), s.positionInSliceForTest(),
				              s.linearBarPosForTest(), s.bufferFramesForTest(), s.sliceFramesForTest(),
				              s.burstCycleForTest(), s.burstGrainForTest());
				std::string out = buf;
				for (const Event& e : events) {
					out += "\n  @" + std::to_string(e.frame) + " " + e.what;
				}
				return out;
			}
		}
		else {
			stillSince = -1;
		}
		lastPos = s.positionInSliceForTest();
		lastSlice = s.sliceIndexForTest();
		lastLinear = s.linearBarPosForTest();
	}
	return {};
}

} // namespace

namespace {

/// Drives `s` for `frames` on a 120 BPM song that wraps at `loopBeats` (0 for no loop); `beats` is
/// the song position, carried between calls.
void playLooped(Sklitter& s, double* beats, double loopBeats, long long frames, int block = 512) {
	std::vector<float> l(static_cast<size_t>(block));
	std::vector<float> r(static_cast<size_t>(block));
	for (long long done = 0; done < frames; done += block) {
		for (int k = 0; k < block; ++k) {
			l[static_cast<size_t>(k)] = static_cast<float>(0.5 * std::sin(0.03 * static_cast<double>(done + k)));
			r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(kSpb);
		c.songPositionBeats = *beats;
		c.playing = true;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), block);
		*beats += static_cast<double>(block) / (kSpb * kSr);
		if (loopBeats > 0.0 && *beats >= loopBeats) {
			*beats -= loopBeats;
		}
	}
}

} // namespace

TEST_CASE("a press inside a ONE-BAR host loop plays: the grid line it waits for is inside the loop") {
	const auto bar = static_cast<long long>(4.0 * kSpb * kSr);
	for (const Mode mode : { Mode::Shuffle, Mode::Repeat, Mode::Grain }) {
		const std::string name = kModeNames[static_cast<size_t>(mode)];
		CAPTURE(name);
		Sklitter s;
		s.prepare(kSr, 4096);
		Settings cfg;
		cfg.enabled = true;
		cfg.mode = mode;
		s.setTarget(cfg);
		double beats = 0.0;
		// PRESSED IN THE LOOP'S LAST BEAT, where the next grid line is the loop's end — the one place a
		// warm press still waited for ever.
		playLooped(s, &beats, 4.0, 3 * bar + 7 * bar / 8);
		REQUIRE(beats >= 3.0);
		cfg.engage = true;
		s.setTarget(cfg);
		playLooped(s, &beats, 4.0, 8 * bar);
		CHECK(s.statusForTest() == Status::Playing);
	}
}

TEST_CASE("the pre-warm completes inside a ONE-BAR host loop, anchored on its downbeat, so a press waits a beat") {
	// THE VALUES: ready, the anchor is the loop's downbeat (0), and a press plays within about a beat —
	// where before it never warmed and every press recorded a whole pass of the loop first.
	const auto bar = static_cast<long long>(4.0 * kSpb * kSr);
	const auto beat = bar / 4;
	Sklitter s;
	s.prepare(kSr, 4096);
	Settings cfg;
	cfg.enabled = true;
	cfg.mode = Mode::Shuffle;
	s.setTarget(cfg);
	double beats = 0.5; // armed mid-bar, so the next downbeat (4) is the loop's end
	playLooped(s, &beats, 4.0, 3 * bar);
	REQUIRE(s.prewarmReadyForTest());
	CHECK(s.prewarmBeatForTest() == 0);
	cfg.engage = true;
	s.setTarget(cfg);
	playLooped(s, &beats, 4.0, 2 * beat);
	CHECK(s.statusForTest() == Status::Playing);
}

namespace {

/// A LOOPER GESTURE HELD TO PLAYING, then Mode turned to `into`. The input is a function of the song's
/// phase in the one-bar buffer — `g(k) = 0.1 + 0.5 k / buffer` — so every ring frame holds its own index,
/// and a read names the frame it came from. Returns the expected origin (the frame after the looper's
/// last read) and fills `out` with the first `n` output frames after the switch.
struct EnteredLive {
	Sklitter s;
	size_t buffer = 0;
	size_t cursor = 0;
	std::vector<float> out;
	long long songFrame = 0;

	static float g(size_t k, size_t buffer) {
		return 0.1f + 0.5f * static_cast<float>(k) / static_cast<float>(buffer);
	}

	void block(int n, Sklitter::ReadTraceEntry* trace = nullptr) {
		std::vector<float> l(static_cast<size_t>(n));
		std::vector<float> r(static_cast<size_t>(n));
		const auto bar = static_cast<long long>(4.0 * kSpb * kSr);
		for (int k = 0; k < n; ++k) {
			l[static_cast<size_t>(k)] = g(static_cast<size_t>((songFrame + k) % bar), static_cast<size_t>(bar));
			r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
		}
		if (trace != nullptr) {
			s.setReadTraceForTest(trace, static_cast<size_t>(n));
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(kSpb);
		c.songPositionBeats = static_cast<double>(songFrame) / (kSpb * kSr);
		c.playing = true;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), n);
		if (trace != nullptr) {
			s.setReadTraceForTest(nullptr, 0);
		}
		out = l;
		songFrame += n;
	}

	EnteredLive(Mode into, int n) {
		s.prepare(kSr, 4096);
		Settings cfg;
		cfg.enabled = true;
		cfg.mode = Mode::Shuffle;
		cfg.bufferBars = 1.0;
		cfg.rate = 0.5f; // a sixteenth: 5512 frames at 120 BPM
		s.setTarget(cfg);
		for (int k = 0; k < 800; ++k) {
			block(256);
		}
		cfg.engage = true;
		s.setTarget(cfg);
		for (int k = 0; k < 300; ++k) {
			block(256);
		}
		REQUIRE(s.statusForTest() == Status::Playing);
		buffer = s.bufferFramesForTest();
		// THE LOOPER'S LAST READ, from its own trace — the frame the cursor is on.
		std::vector<Sklitter::ReadTraceEntry> trace(256);
		block(256, trace.data());
		REQUIRE(trace.back().at >= 0);
		REQUIRE_FALSE(s.planForTest().shouldReverse);
		REQUIRE_FALSE(s.planForTest().shouldPitchUp);
		REQUIRE(s.pitchRatioForTest() == 1.0);
		cursor = static_cast<size_t>(trace.back().at + 1) % buffer;
		cfg.mode = into;
		s.setTarget(cfg);
		block(n);
	}
};

} // namespace

TEST_CASE("Mode turned to BURST during a looper gesture arms from the cursor: the grain reads the frame under the playhead, then the gate is silent") {
	EnteredLive e(Mode::Burst, 8192);
	const size_t division = static_cast<size_t>(std::llround(0.25 * kSpb * kSr)); // a sixteenth
	CHECK(e.s.sliceOriginForTest() == e.cursor);
	CHECK(e.s.capturedSliceFramesForTest() == division);
	CHECK(e.s.burstGrainForTest() == division / 2);
	CHECK(e.s.burstCycleForTest() == division);
	// THE VALUE READ: the first sample is the cursor's frame, the next the one after it.
	CHECK(e.out[0] == doctest::Approx(EnteredLive::g(e.cursor, e.buffer)).epsilon(1e-6));
	CHECK(e.out[1] == doctest::Approx(EnteredLive::g((e.cursor + 1) % e.buffer, e.buffer)).epsilon(1e-6));
	// AND THE GATE AFTER THE GRAIN IS SILENCE, where a device passing the dry would read 0.1 or more.
	CHECK(e.out[division / 2 + 100] == 0.0f);
}

TEST_CASE("the ribbon moves in BURST: the head is where the burst cycle is") {
	for (const Mode mode : { Mode::Burst }) {
		const std::string name = kModeNames[static_cast<size_t>(mode)];
		CAPTURE(name);
		EnteredLive e(mode, 256);
		std::vector<float> published;
		e.s.setReadingsSink(feathers::device::ReadingsSink {
		    &published, [](void* ctx, const float* v, size_t n) {
			    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		    } });
		std::vector<float> phases;
		for (int k = 0; k < 16; ++k) {
			e.block(300);
			e.s.publishReadings();
			REQUIRE(published.size() == kReadingCount);
			CHECK(published[static_cast<size_t>(Reading::Step)] == 0.0f);
			const float phase = published[static_cast<size_t>(Reading::StepPhase)];
			phases.push_back(phase);
		}
		// AND IT MOVES: sixteen blocks of 300 frames, never the same head twice in a row.
		for (size_t k = 1; k < phases.size(); ++k) {
			CHECK(phases[k] != phases[k - 1]);
		}
	}
}

namespace {

/// THE REPORTED SESSION, REPLAYED FROM THE TRIPWIRE LOG: 44.1k, 80 BPM, calls of 1..32 frames (the lane's), a
/// two-bar buffer. `beats` is the song position, `playing` the transport; `until` in beats.
struct Replay {
	static constexpr double kReplaySpb = 60.0 / 80.0;
	Sklitter s;
	Settings cfg;
	double beats = 0.0;
	bool playing = true;
	Rng rng { 0x5EEDull };
	std::vector<float> l = std::vector<float>(32);
	std::vector<float> r = std::vector<float>(32);
	long long frame = 0;

	Replay() {
		s.prepare(kSr, 4096);
		cfg.enabled = true;
		cfg.bufferBars = 2.0;
		cfg.mode = Mode::Shuffle;
		s.setTarget(cfg);
	}
	void call(int n) {
		for (int k = 0; k < n; ++k) {
			l[static_cast<size_t>(k)] = static_cast<float>(0.5 * std::sin(0.031 * static_cast<double>(frame + k)));
			r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
		}
		TransportClock c = TransportClock::fromSecondsPerBeat(kReplaySpb);
		c.songPositionBeats = beats;
		c.playing = playing;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), n);
		frame += n;
		if (playing) {
			beats += static_cast<double>(n) / (kReplaySpb * kSr);
		}
	}
	/// Plays until the song reaches `until` (or, stopped, for that many beats of wall time).
	void to(double until) {
		if (playing) {
			while (beats < until) {
				call(rng.range(1, 32));
			}
			return;
		}
		const auto frames = static_cast<long long>(until * kReplaySpb * kSr);
		for (long long done = 0; done < frames;) {
			const int n = rng.range(1, 32);
			call(n);
			done += n;
		}
	}
	void set(Mode m) {
		cfg.mode = m;
		s.setTarget(cfg);
	}
	void engage(bool on) {
		cfg.engage = on;
		s.setTarget(cfg);
	}
	void stopThenRestart(double stoppedBeats) {
		playing = false;
		to(stoppedBeats);
		playing = true;
		beats = 0.0;
	}
};

} // namespace

TEST_CASE("REPLAY: the stuck Standby, from the tripwire log — the logged statuses, at the logged beats") {
	Replay p;
	// The first half of the log: Shuffle and Repeat engaged from a warm ring, each Playing at the next beat.
	p.to(39.9);
	p.engage(true);
	p.to(40.1);
	CHECK(p.s.statusForTest() == Status::Playing);
	p.engage(false);
	p.set(Mode::Burst);
	p.to(47.99);
	p.engage(true); // Burst: Recording, then Playing
	p.to(48.5); // a sixteenth at 80 BPM is a quarter beat to record
	CHECK(p.s.statusForTest() == Status::Playing);
	p.set(Mode::Burst);
	p.to(67.51);
	p.engage(false);
	p.to(72.01);
	p.engage(true);
	p.to(72.3);
	CHECK(p.s.statusForTest() == Status::Playing);
	p.set(Mode::Burst);
	p.to(127.08);
	p.engage(false);
	p.stopThenRestart(2.0);
	p.to(4.675);
	p.set(Mode::Pattern);
	p.to(7.648);
	p.engage(true);
	p.to(14.47);
	// NOT WARM: the Burst gestures re-lengthed the ring, and the two-bar buffer at 80 BPM
	// cannot have been recorded since the switch at 4.675. Still capturing, as in the reported log.
	REQUIRE_FALSE(p.s.prewarmReadyForTest());
	CHECK(p.s.statusForTest() == Status::Standby);
	// THE RELEASE KEEPS IT IN STANDBY UNTIL ITS TRIGGER — the reference way, by design — and it goes on
	// recording there.
	p.engage(false);
	p.to(14.5);
	CHECK(p.s.statusForTest() == Status::Standby);
	p.to(15.87);
	// THE RE-PRESS KEEPS WHAT WAS RECORDED — 8.2 beats since 7.648, over the buffer's 8 — so it plays at
	// the NEXT BEAT. The reported log: the count restarted, Standby until 24, then Off, never Playing.
	const size_t before = p.s.capturedFramesForTest();
	p.engage(true);
	p.call(1);
	CHECK(p.s.capturedFramesForTest() == before + 1);
	// THE BEAT IT PLAYS AT: the first call that ends Playing, which is the first call past beat 16.
	while (p.s.statusForTest() != Status::Playing && p.beats < 24.5) {
		p.call(p.rng.range(1, 32));
	}
	MESSAGE("re-pressed at 15.87; Playing at beat " << p.beats);
	CHECK(p.s.statusForTest() == Status::Playing);
	CHECK(p.beats >= 16.0);
	CHECK(p.beats < 16.01);
	p.to(16.05);
	p.to(16.87);
	p.engage(false);
	p.to(16.9);
	CHECK(p.s.statusForTest() == Status::Off);
	p.to(22.77);
	p.set(Mode::Shuffle);
	p.to(24.27);
	p.stopThenRestart(2.0);
	// AND THE BURST OF TOGGLES AFTER THE RESTART: every press plays at its next beat, every release is Off.
	for (const double at : { 3.08, 5.03, 6.38 }) {
		CAPTURE(at);
		p.to(at);
		p.engage(true);
		p.to(std::floor(at) + 1.05);
		CHECK(p.s.statusForTest() == Status::Playing);
		p.to(std::floor(at) + 1.3);
		p.engage(false);
		p.to(std::floor(at) + 1.35);
		CHECK(p.s.statusForTest() == Status::Off);
	}
}

TEST_CASE("a re-press during a released capture keeps its count, and a capture that ends unplayed warms the pre-warm") {
	// decision B, by value. 80 BPM, a two-bar buffer (8 beats); a cold press at 8.5 (the pre-warm
	// is re-lengthed by a Burst gesture first, so it has nothing).
	Replay p;
	p.to(5.0); // Shuffle, pre-warm anchored on the downbeat at 4
	p.set(Mode::Burst);
	p.engage(true);
	p.to(6.0);
	p.engage(false);
	p.to(8.5);
	p.set(Mode::Shuffle);
	p.engage(true);
	p.to(12.5);
	const size_t atRelease = p.s.capturedFramesForTest();
	REQUIRE(atRelease > 0u);
	p.engage(false);
	p.to(13.5);
	REQUIRE(p.s.statusForTest() == Status::Standby);
	// THE RE-PRESS: the count is at least what was recorded, and one call later exactly one call more.
	const size_t atRepress = p.s.capturedFramesForTest();
	CHECK(atRepress > atRelease);
	p.engage(true);
	p.call(7);
	CHECK(p.s.capturedFramesForTest() == atRepress + 7);
	// RELEASED AGAIN AND LEFT: it ends unplayed at its trigger, and the pre-warm it hands to is WARM, so a
	// press after that plays at the next beat.
	p.engage(false);
	p.to(18.5);
	REQUIRE(p.s.statusForTest() == Status::Off);
	CHECK(p.s.prewarmReadyForTest());
	p.engage(true);
	p.to(19.05);
	CHECK(p.s.statusForTest() == Status::Playing);
}

TEST_CASE("a press on a PARTLY warm ring keeps what the pre-warm has, and waits only for the rest") {
	// 80 BPM, a two-bar buffer (8 beats). A Burst gesture re-lengths the ring; Pattern then pre-warms from
	// the downbeat at 8, and a press at 14 finds six beats of it. It needs two more, so it plays by 17 —
	// where a press that threw the six away recorded eight from scratch and could not play before 22.
	Replay p;
	p.set(Mode::Burst);
	p.to(1.0);
	p.engage(true);
	p.to(2.0);
	REQUIRE(p.s.statusForTest() == Status::Playing);
	p.engage(false);
	p.to(4.675);
	p.set(Mode::Pattern);
	p.to(14.0);
	REQUIRE(p.s.prewarmedFramesForTest() > 0u);
	REQUIRE_FALSE(p.s.prewarmReadyForTest());
	p.engage(true);
	p.to(17.05);
	CHECK(p.s.statusForTest() == Status::Playing);
}

TEST_CASE("FUZZ: an engaged device on a rolling song plays, and its cursor moves, whatever Mode does") {
	const char* env = std::getenv("SKLITTER_FUZZ_SEEDS");
	const int seeds = env != nullptr ? std::atoi(env) : 20;
	const char* first = std::getenv("SKLITTER_FUZZ_FIRST");
	const uint64_t base = first != nullptr ? std::strtoull(first, nullptr, 10) : 1;
	int failures = 0;
	for (int k = 0; k < seeds; ++k) {
		const std::string report = fuzzOnce(base + static_cast<uint64_t>(k), 60.0);
		if (!report.empty()) {
			++failures;
			if (failures <= 5) {
				MESSAGE(report);
			}
		}
	}
	CHECK(failures == 0);
}

TEST_CASE("a host tempo ramp restarts the pre-warm every block, and each restart clears what was written, not the capacity") {
	// The bar length changes with every tempo step, and each change restarts the warm ring. The restart used to fill the
	// whole capacity (16 s of two channels) per block. Counted: frames overwritten by resets, against the frames fed.
	Sklitter s;
	s.prepare(kSr, 4096);
	Settings cfg;
	cfg.enabled = true;
	cfg.mode = Mode::Shuffle;
	s.setTarget(cfg);
	constexpr int kBlock = 128;
	constexpr int kSteady = 700; // long enough to reach a downbeat and write
	constexpr int kRamp = 400;
	constexpr int kBlocks = kSteady + kRamp;
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	double beats = 0.25;
	for (int b = 0; b < kBlocks; ++b) {
		for (int k = 0; k < kBlock; ++k) {
			l[static_cast<size_t>(k)] = static_cast<float>(0.5 * std::sin(0.03 * static_cast<double>(b * kBlock + k)));
			r[static_cast<size_t>(k)] = l[static_cast<size_t>(k)];
		}
		const double spb = b < kSteady ? 0.5 : 0.5 - 0.0004 * (b - kSteady); // steady, then a ramp: a new bar length every block
		TransportClock c = TransportClock::fromSecondsPerBeat(spb);
		c.songPositionBeats = beats;
		c.playing = true;
		s.setTransportClock(c);
		s.processStereo(l.data(), r.data(), kBlock);
		beats += static_cast<double>(kBlock) / (spb * kSr);
	}
	const size_t cleared = s.ringForTest().clearedFramesForTest();
	CHECK(s.ringForTest().landed() > 0u); // the steady stretch wrote into the ring
	const size_t capacity = s.ringForTest().capacity();
	CAPTURE(cleared);
	CAPTURE(capacity);
	CHECK(cleared > 0u); // NON-VACUITY: restarts happened and wrote
	CHECK(cleared <= static_cast<size_t>(kBlock) * kBlocks);
	CHECK(cleared < capacity); // a single whole-capacity fill per block would be 400 times this
}
