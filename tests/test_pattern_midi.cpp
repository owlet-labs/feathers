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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_midi.h"
#include "mod/arp/pattern_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <cstdint>
#include <map>
#include <vector>

// PATTERN → .mid, BY VALUE (design §7.9). The bytes are parsed back by a reader written here from the SMF spec, and
// compared with what a separate `PatternDevice` emits for the same patch: every note's key, velocity, start and length in
// ticks; under TB-Glide, each note's own MPE channel and its bends.

namespace pat = feathers::mod::arp::pat;
namespace genes = feathers::mod::arp::genes;
namespace device = feathers::device;

namespace {

/// A NOTE AS THE FILE HOLDS IT.
struct FileNote {
	int channel = 0;
	int key = 0;
	int velocity = 0;
	uint32_t on = 0;
	uint32_t off = 0;
};

/// **THE READER**, from the SMF spec: header, one track, delta times, running status refused (the writer never uses it),
/// metas skipped but tempo kept, channel messages decoded.
struct Parsed {
	int format = -1;
	int tracks = 0;
	int ppq = 0;
	uint32_t usPerBeat = 0;
	uint32_t endTick = 0;
	std::vector<FileNote> notes;
	/// Per channel: the controller writes (cc, value) in order, and the bends (tick, value).
	std::map<int, std::vector<std::pair<int, int>>> cc;
	std::map<int, std::vector<std::pair<uint32_t, int>>> bends;
	bool ok = false;
};

Parsed parse(const std::vector<uint8_t>& b) {
	Parsed p;
	size_t i = 0;
	const auto u32 = [&] {
		const uint32_t v = (uint32_t { b[i] } << 24) | (uint32_t { b[i + 1] } << 16) | (uint32_t { b[i + 2] } << 8) | b[i + 3];
		i += 4;
		return v;
	};
	const auto u16 = [&] {
		const auto v = static_cast<int>((b[i] << 8) | b[i + 1]);
		i += 2;
		return v;
	};
	const auto varLen = [&] {
		uint32_t v = 0;
		for (int n = 0; n < 4; ++n) {
			const uint8_t c = b[i++];
			v = (v << 7) | (c & 0x7Fu);
			if ((c & 0x80u) == 0) {
				break;
			}
		}
		return v;
	};
	REQUIRE(b.size() > 22);
	REQUIRE(std::string(b.begin(), b.begin() + 4) == "MThd");
	i = 4;
	REQUIRE(u32() == 6);
	p.format = u16();
	p.tracks = u16();
	p.ppq = u16();
	REQUIRE(std::string(b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i) + 4) == "MTrk");
	i += 4;
	const uint32_t length = u32();
	REQUIRE(i + length == b.size());
	uint32_t tick = 0;
	std::map<std::pair<int, int>, FileNote> open;
	while (i < b.size()) {
		tick += varLen();
		const uint8_t status = b[i++];
		if (status == 0xFF) {
			const uint8_t type = b[i++];
			const uint32_t n = varLen();
			if (type == 0x51) {
				p.usPerBeat = (uint32_t { b[i] } << 16) | (uint32_t { b[i + 1] } << 8) | b[i + 2];
			}
			i += n;
			if (type == 0x2F) {
				p.endTick = tick;
				p.ok = i == b.size() && open.empty();
				break;
			}
			continue;
		}
		REQUIRE((status & 0x80) != 0); // no running status
		const int ch = status & 0x0F;
		const int kind = status & 0xF0;
		const int d1 = b[i++];
		const int d2 = b[i++];
		if (kind == 0x90 && d2 > 0) {
			open[{ ch, d1 }] = FileNote { ch, d1, d2, tick, 0 };
		}
		else if (kind == 0x80 || (kind == 0x90 && d2 == 0)) {
			auto it = open.find({ ch, d1 });
			REQUIRE(it != open.end());
			it->second.off = tick;
			p.notes.push_back(it->second);
			open.erase(it);
		}
		else if (kind == 0xB0) {
			p.cc[ch].emplace_back(d1, d2);
		}
		else if (kind == 0xE0) {
			p.bends[ch].emplace_back(tick, d1 | (d2 << 7));
		}
	}
	std::sort(p.notes.begin(), p.notes.end(), [](const FileNote& a, const FileNote& c) {
		return a.on != c.on ? a.on < c.on : a.key < c.key;
	});
	return p;
}

/// THE PATCH: one tab set up by `tune`, the rest as they come.
pat::MidiSource sourceOf(const std::function<void(pat::Settings&)>& tune) {
	pat::Settings s;
	for (auto& q : s.patterns) {
		q.pins[static_cast<size_t>(genes::Pin::Detail)] = 0.0f;
	}
	tune(s);
	const pat::Row row = pat::rowOf(s);
	pat::MidiSource src;
	src.row.assign(row.begin(), row.end());
	return src;
}

/// **THE REFERENCE**: a separate device, the same patch with tab `p` soloed, C4 held, the transport at `bpm` from 0; every
/// note it sounds before `beats`, in ticks (a note still sounding at the end ends there), and its tuning moves.
struct Reference {
	std::vector<FileNote> notes;
	std::vector<std::pair<uint32_t, double>> tunings; // (tick, semitones)
};

Reference reference(const pat::MidiSource& src, size_t p, double bpm, double beats) {
	pat::Settings s = pat::settingsOf(src.row.data(), src.row.size());
	for (size_t t = 0; t < pat::kPatterns; ++t) {
		s.patterns[t].solo = t == p;
	}
	pat::PatternDevice d;
	d.prepare(48000.0);
	const pat::Row row = pat::rowOf(s);
	d.setFields(row.data(), row.size());
	device::NoteEvent on {};
	on.kind = device::NoteEvent::Kind::On;
	on.key = 60;
	on.velocity = 0.8;
	std::array<device::NoteEvent, 8> passed {};
	size_t n = 0;
	d.processNotes(&on, 1, passed.data(), passed.size(), &n);
	const double spb = 60.0 / bpm;
	const auto end = static_cast<int64_t>(std::llround(beats * spb * 48000.0));
	const auto tickOf = [&](int64_t sample) {
		return static_cast<uint32_t>(std::llround(static_cast<double>(sample) / 48000.0 / spb * 960.0));
	};
	Reference r;
	std::map<int, FileNote> open;
	std::vector<device::NoteEvent> out(512);
	int64_t now = 0;
	while (now < end) {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(spb);
		c.songPositionBeats = static_cast<double>(now) / 48000.0 / spb;
		c.songPositionSeconds = static_cast<double>(now) / 48000.0;
		c.playing = true;
		uint32_t next = 0;
		const size_t got = d.emit(c, static_cast<uint64_t>(now), out.data(), out.size(), &next);
		for (size_t i = 0; i < got; ++i) {
			const auto& e = out[i];
			if (e.kind == device::NoteEvent::Kind::On) {
				open[e.key] = FileNote { 0, e.key, static_cast<int>(std::clamp<long>(std::lround(e.velocity * 127.0), 1, 127)), tickOf(now), 0 };
			}
			else if (e.kind == device::NoteEvent::Kind::Off && open.count(e.key) != 0) {
				FileNote fn = open[e.key];
				fn.off = tickOf(now);
				r.notes.push_back(fn);
				open.erase(e.key);
			}
			else if (e.kind == device::NoteEvent::Kind::Expression && e.expressionId == pat::kTuningExpression) {
				r.tunings.emplace_back(tickOf(now), e.expressionValue);
			}
		}
		now += std::min<int64_t>({ static_cast<int64_t>(std::max<uint32_t>(next, 1)), 64, end - now });
	}
	for (auto& [key, fn] : open) {
		fn.off = static_cast<uint32_t>(std::llround(beats * 960.0));
		r.notes.push_back(fn);
	}
	std::sort(r.notes.begin(), r.notes.end(), [](const FileNote& a, const FileNote& c) {
		return a.on != c.on ? a.on < c.on : a.key < c.key;
	});
	return r;
}

} // namespace

TEST_CASE("midi: the loop is the fewest whole cycles that fill whole bars, at most 8, else padded to the next bar") {
	CHECK(pat::midiLoopBeats(4.0) == 4.0);         // 16 sixteenths: one bar
	CHECK(pat::midiLoopBeats(1.75) == 28.0);       // 7 sixteenths: 16 cycles, 7 bars
	CHECK(pat::midiLoopBeats(5.0 / 6.0) == 20.0);  // 5 sixteenth-triplets: 24 cycles, 5 bars
	CHECK(pat::midiLoopBeats(8.0) == 8.0);
	CHECK(pat::midiLoopBeats(3.75) == 4.0);        // 15 sixteenths: 16 cycles (15 bars) pass the cap, so one cycle padded to a bar
	CHECK(pat::midiLoopBeats(0.6) == 12.0);        // 0.6 × 20 = 12: 3 bars
	CHECK(pat::midiLoopBeats(64.0) == 32.0);       // a cycle past the cap: the cap
}

TEST_CASE("midi: the file parses back to exactly the notes the device plays — keys, velocities, ticks and lengths, chords and ties — in every pitched line Mode and Drum") {
	const genes::Mode modes[] = { genes::Mode::Melody, genes::Mode::MelodyMono, genes::Mode::Chords, genes::Mode::TbMono,
		                          genes::Mode::Progression, genes::Mode::Drum };
	for (const genes::Mode mode : modes) {
		for (const size_t p : { size_t { 0 }, size_t { 2 } }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(p);
			const pat::MidiSource src = sourceOf([&](pat::Settings& s) {
				s.patterns[p].enable = true;
				s.patterns[p].mode = mode;
				s.patterns[p].zone = 420 + 37 * static_cast<int>(p);
				s.patterns[p].length = 12;
				s.patternTranspose[p] = { 2, 0 };
			});
			const double bpm = 132.0;
			const std::vector<uint8_t> bytes = pat::patternMidi(src, p, bpm);
			const Parsed f = parse(bytes);
			REQUIRE(f.ok);
			CHECK(f.format == 0);
			CHECK(f.tracks == 1);
			CHECK(f.ppq == 960);
			CHECK(f.usPerBeat == static_cast<uint32_t>(std::llround(60.0 / bpm * 1e6)));
			// 12 sixteenths = 3 beats: 4 cycles fill 3 bars (unless Progression's own length differs).
			const double cycleBeats = mode == genes::Mode::Progression ? -1.0 : 3.0;
			if (cycleBeats > 0.0) {
				CHECK(f.endTick == 12u * 960u);
			}
			const double beats = static_cast<double>(f.endTick) / 960.0;
			const Reference r = reference(src, p, bpm, beats);
			REQUIRE(f.notes.size() == r.notes.size());
			REQUIRE(f.notes.size() > 4);
			for (size_t i = 0; i < r.notes.size(); ++i) {
				CAPTURE(i);
				CHECK(f.notes[i].key == r.notes[i].key);
				CHECK(f.notes[i].velocity == r.notes[i].velocity);
				CHECK(f.notes[i].on == r.notes[i].on);
				CHECK(f.notes[i].off == r.notes[i].off);
				CHECK(f.notes[i].channel == 0); // notes only, channel 1
			}
		}
	}
}

TEST_CASE("midi: TB-Glide is ONE channel — its glide that channel's pitch bend, the range set once by RPN 0 sized to the widest glide, the bend centred before each new note, and no MPE zone (backlog QW)") {
	const pat::MidiSource src = sourceOf([](pat::Settings& s) {
		s.patterns[0].mode = genes::Mode::TbGlide;
		s.patterns[0].zone = 640;
		s.patterns[0].pins[static_cast<size_t>(genes::Pin::Glide)] = 0.6f; // slides: the glide the bends carry
	});
	const std::vector<uint8_t> bytes = pat::patternMidi(src, 0, 120.0);
	const Parsed f = parse(bytes);
	REQUIRE(f.ok);
	const double beats = static_cast<double>(f.endTick) / 960.0;
	const Reference r = reference(src, 0, 120.0, beats);
	REQUIRE(!r.tunings.empty());
	// THE RANGE: the widest glide the device sent, rounded up to 12 / 24 / 48.
	double widest = 0.0;
	for (const auto& [tick, semis] : r.tunings) {
		widest = std::max(widest, std::fabs(semis));
	}
	const int range = pat::midiBendRangeFor(widest);
	MESSAGE("widest glide " << widest << " semitones: the file states a bend range of " << range);
	// ONE CHANNEL THROUGHOUT: controllers, bends and notes all on channel 1.
	CHECK(f.cc.size() == 1);
	CHECK(f.cc.count(0) == 1);
	for (const auto& [ch, list] : f.bends) {
		CHECK(ch == 0);
	}
	// RPN 0 on channel 1 and nothing else: no RPN 6 (the MPE configuration).
	REQUIRE(f.cc.count(0) == 1);
	const auto& c = f.cc.at(0);
	REQUIRE(c.size() >= 6);
	CHECK(c[0] == std::pair { 0x65, 0x00 });
	CHECK(c[1] == std::pair { 0x64, 0x00 });
	CHECK(c[2] == std::pair { 0x06, range });
	CHECK(c[3] == std::pair { 0x26, 0x00 });
	CHECK(c[4] == std::pair { 0x65, 0x7F });
	CHECK(c[5] == std::pair { 0x64, 0x7F });
	for (const auto& [cc, v] : c) {
		CHECK_FALSE((cc == 0x64 && v == 0x06)); // never the MPE configuration's RPN
	}
	// THE NOTES: as the device played them, every one on channel 1.
	REQUIRE(f.notes.size() == r.notes.size());
	REQUIRE(f.notes.size() > 2);
	for (size_t i = 0; i < f.notes.size(); ++i) {
		CHECK(f.notes[i].channel == 0);
		CHECK(f.notes[i].key == r.notes[i].key);
		CHECK(f.notes[i].on == r.notes[i].on);
	}
	const std::vector<std::pair<uint32_t, int>> bends = f.bends.count(0) == 1 ? f.bends.at(0) : std::vector<std::pair<uint32_t, int>> {};
	// THE GLIDE: every tuning the device sent is a channel bend at its tick, by value, against the stated range.
	size_t found = 0;
	for (const auto& [tick, semis] : r.tunings) {
		const int want = static_cast<int>(std::clamp<long>(std::lround(8192.0 + semis / static_cast<double>(range) * 8192.0), 0, 16383));
		for (const auto& [t, v] : bends) {
			if (t == tick && v == want) {
				++found;
				break;
			}
		}
	}
	CHECK(found == r.tunings.size());
	// EACH NEW NOTE STARTS UNBENT: a centre bend at every note-on's tick.
	for (const FileNote& n : f.notes) {
		bool centred = false;
		for (const auto& [t, v] : bends) {
			centred = centred || (t == n.on && v == 8192);
		}
		CAPTURE(n.on);
		CHECK(centred);
	}
}

TEST_CASE("midi: the groove is baked in — every note-on lands on its straight sixteenth plus the MPC swing at that step, restated here, and the swing's odd steps really are late") {
	// BACKLOG QA: MIDI export bakes the groove in.
	namespace hz = feathers::mod::arp::humanize;
	const pat::MidiSource src = sourceOf([](pat::Settings& s) {
		s.patterns[0].zone = 333;
		s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f; // every step a note
		s.patterns[0].pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
		s.feel = { hz::Mode::Swing16, 0.8, 0 };
	});
	const Parsed f = parse(pat::patternMidi(src, 0, 120.0));
	REQUIRE(f.ok);
	REQUIRE(f.notes.size() >= 16);
	// THE AMOUNT AS THE ROW HOLDS IT (a float field), and the MPC's swing: odd sixteenths late by 2·(0.5 + 0.25·a) − 1.
	const double a = static_cast<double>(static_cast<float>(0.8));
	const double late = 2.0 * (0.5 + 0.25 * a) - 1.0;
	REQUIRE(late > 0.1);
	size_t odd = 0;
	for (size_t g = 0; g < 16; ++g) {
		CAPTURE(g);
		const double shift = g % 2 == 1 ? late : 0.0;
		// 6000 samples a sixteenth at 120 bpm and 48 kHz, the onset on the first sample at or after the point; 0.04 ticks
		// a sample (960 a quarter).
		const auto sample = static_cast<int64_t>(std::ceil((static_cast<double>(g) + shift) * 6000.0 - 1e-6));
		const auto want = static_cast<uint32_t>(std::llround(static_cast<double>(sample) * 0.04));
		CHECK(f.notes[g].on == want);
		if (g % 2 == 1) {
			CHECK(f.notes[g].on > static_cast<uint32_t>(g) * 240u); // and not on the straight grid
			++odd;
		}
	}
	CHECK(odd == 8);
}
