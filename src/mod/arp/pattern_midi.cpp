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

#include "mod/arp/pattern_midi.h"

#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace feathers::mod::arp::pat {

namespace {

constexpr double kRenderRate = 48000.0;

/// ONE TIMED MESSAGE, in the order the device made it (a stable sort keeps it so within a tick).
struct Timed {
	uint32_t tick = 0;
	std::vector<uint8_t> bytes;
};

void putVarLen(std::vector<uint8_t>& out, uint32_t v) {
	std::array<uint8_t, 5> buf {};
	size_t n = 0;
	buf[n++] = static_cast<uint8_t>(v & 0x7Fu);
	while ((v >>= 7) != 0) {
		buf[n++] = static_cast<uint8_t>((v & 0x7Fu) | 0x80u);
	}
	while (n > 0) {
		out.push_back(buf[--n]);
	}
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
	for (int s = 24; s >= 0; s -= 8) {
		out.push_back(static_cast<uint8_t>(v >> s));
	}
}

uint8_t velocityByte(double v) {
	return static_cast<uint8_t>(std::clamp<long>(std::lround(v * 127.0), 1, 127));
}

/// The cycle's length in steps, as `pattern::build` sets it (a Progression's progression; else the Length box).
size_t cycleLengthOf(const Settings& s, size_t p, const HandState& hand) {
	auto cycle = std::make_unique<pattern::PatternCycle>();
	pattern::build(PatternDevice::placeOf(s.patterns[p], hand[p].residue), recipeFor(s, p, hand[p].pinOn), HeldChord {},
	               *cycle);
	return std::max<size_t>(1, cycle->length);
}

} // namespace

double midiLoopBeats(double cycleBeats) noexcept {
	const double maxBeats = 4.0 * kMidiMaxBars;
	if (!(cycleBeats > 0.0) || !std::isfinite(cycleBeats)) {
		return 4.0;
	}
	// THE FEWEST WHOLE CYCLES THAT FILL WHOLE BARS, within the cap.
	for (int k = 1; static_cast<double>(k) * cycleBeats <= maxBeats + 1e-9; ++k) {
		const double beats = static_cast<double>(k) * cycleBeats;
		const double bars = beats / 4.0;
		if (std::abs(bars - std::round(bars)) < 1e-6) {
			return std::round(bars) * 4.0;
		}
	}
	// NONE LANDS ON A BAR: one cycle (the cap at most), padded to the next bar.
	return std::min(maxBeats, std::ceil(std::min(cycleBeats, maxBeats) / 4.0 - 1e-9) * 4.0);
}

std::string patternMidiName(size_t pattern) {
	return "Pattern " + std::to_string(pattern + 1) + ".mid";
}

std::vector<uint8_t> patternMidi(const MidiSource& source, size_t pattern, double bpm) {
	pattern = std::min(pattern, kPatterns - 1);
	if (!(bpm > 0.0) || !std::isfinite(bpm)) {
		bpm = 120.0;
	}
	// THE PATCH, MADE TO PLAY THIS ONE PATTERN from the song's start: Selector, this tab soloed (whatever is enabled).
	Settings s = settingsOf(source.row.data(), source.row.size());
	s.playMode = PlayMode::Selector;
	for (size_t p = 0; p < kPatterns; ++p) {
		s.patterns[p].solo = p == pattern;
	}
	s.phase[pattern].fill = false; // played as the pattern it is
	const HandState hand = handStateOf(source.hand.data(), source.hand.size());
	const PatternSettings& q = s.patterns[pattern];
	arp::Settings rate;
	rate.timeBase = q.timeBase;
	rate.rate = q.rate;
	const double stepBeat = rate.synced() ? stepBeats(rate) : (stepHz(rate) > 0.0 ? bpm / 60.0 / stepHz(rate) : 0.25);
	const double loopBeats = midiLoopBeats(static_cast<double>(cycleLengthOf(s, pattern, hand)) * stepBeat);
	const double secondsPerBeat = 60.0 / bpm;
	const auto loopSamples = static_cast<int64_t>(std::llround(loopBeats * secondsPerBeat * kRenderRate));

	auto d = std::make_unique<PatternDevice>();
	d->prepare(kRenderRate);
	const Row row = rowOf(s);
	d->setFields(row.data(), row.size());
	if (!source.hand.empty()) {
		(void)d->writeHand(source.hand.data(), source.hand.size());
	}
	if (!source.edits.empty()) {
		(void)d->writeEdits(source.edits.data(), source.edits.size());
	}
	// THE CHORD IT PLAYS OVER: the latched one, if Latch holds one; else C4 (what the lane draws over).
	bool latched = false;
	if (s.latch && !source.play.empty() && d->restorePlay(source.play.data(), source.play.size())) {
		latched = !d->chordForTest().empty();
	}
	if (!latched) {
		device::NoteEvent on {};
		on.kind = device::NoteEvent::Kind::On;
		on.noteId = 1;
		on.key = 60;
		on.velocity = 0.8;
		std::array<device::NoteEvent, 8> passed {};
		size_t n = 0;
		d->processNotes(&on, 1, passed.data(), passed.size(), &n);
	}

	// PLAY THE LOOP, every event at its sample.
	struct Heard {
		int64_t sample;
		device::NoteEvent e;
	};
	std::vector<Heard> heard;
	std::vector<device::NoteEvent> out(512);
	int64_t now = 0;
	while (now < loopSamples) {
		device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(secondsPerBeat);
		clock.songPositionBeats = static_cast<double>(now) / kRenderRate / secondsPerBeat;
		clock.songPositionSeconds = static_cast<double>(now) / kRenderRate;
		clock.playing = true;
		uint32_t next = 0;
		const size_t n = d->emit(clock, static_cast<uint64_t>(now), out.data(), out.size(), &next);
		for (size_t i = 0; i < n; ++i) {
			heard.push_back({ now, out[i] });
		}
		now += std::min<int64_t>({ static_cast<int64_t>(std::max<uint32_t>(next, 1)), 64, loopSamples - now });
	}

	const auto tickOf = [&](int64_t sample) {
		const double beats = static_cast<double>(sample) / kRenderRate / secondsPerBeat;
		return static_cast<uint32_t>(std::llround(beats * kMidiTicksPerQuarter));
	};
	const auto endTick = static_cast<uint32_t>(std::llround(loopBeats * kMidiTicksPerQuarter));
	// TB-GLIDE'S GLIDE IS CHANNEL PITCH BEND, on the one channel (backlog QW): it holds one note at a time, so the channel's
	// bend is the note's. Its range is the clip's widest glide, rounded up to 12, 24 or 48 semitones (48 the cap; wider is
	// clamped there), set once at the start by RPN 0.
	const bool bends = q.mode == Mode::TbGlide;
	double widest = 0.0;
	for (const Heard& h : heard) {
		if (h.e.kind == device::NoteEvent::Kind::Expression && h.e.expressionId == kTuningExpression) {
			widest = std::max(widest, std::fabs(h.e.expressionValue));
		}
	}
	const int range = midiBendRangeFor(widest);

	std::vector<Timed> events;
	// TEMPO and 4/4.
	{
		const auto usPerBeat = static_cast<uint32_t>(std::llround(secondsPerBeat * 1e6));
		events.push_back({ 0, { 0xFF, 0x51, 0x03, static_cast<uint8_t>(usPerBeat >> 16), static_cast<uint8_t>(usPerBeat >> 8),
		                        static_cast<uint8_t>(usPerBeat) } });
		events.push_back({ 0, { 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08 } });
	}
	if (bends) {
		// RPN 0 (PITCH BEND SENSITIVITY) on channel 1: MSB semitones, LSB cents, the RPN closed after. No MPE zone (RPN 6).
		events.push_back({ 0, { 0xB0, 0x65, 0x00, 0xB0, 0x64, 0x00, 0xB0, 0x06, static_cast<uint8_t>(range), 0xB0, 0x26, 0x00,
		                        0xB0, 0x65, 0x7F, 0xB0, 0x64, 0x7F } });
	}
	// THE NOTES, every one on channel 1; a glide as the channel's pitch bend.
	std::map<int, uint8_t> channelOf; // key -> channel of the note sounding on it
	const auto bendBytes = [range](uint8_t ch, double semitones) {
		const long v = std::clamp<long>(std::lround(8192.0 + semitones / static_cast<double>(range) * 8192.0), 0, 16383);
		return std::vector<uint8_t> { static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(v & 0x7F),
			                          static_cast<uint8_t>((v >> 7) & 0x7F) };
	};
	for (const Heard& h : heard) {
		const device::NoteEvent& e = h.e;
		const uint32_t tick = tickOf(h.sample);
		const auto key = static_cast<uint8_t>(std::clamp<int>(e.key, 0, 127));
		if (e.kind == device::NoteEvent::Kind::On) {
			const uint8_t ch = 0;
			if (bends) {
				events.push_back({ tick, bendBytes(ch, 0.0) }); // each new note starts unbent: the bend back to centre
			}
			channelOf[key] = ch;
			events.push_back({ tick, { static_cast<uint8_t>(0x90 | ch), key, velocityByte(e.velocity) } });
		}
		else if (e.kind == device::NoteEvent::Kind::Off || e.kind == device::NoteEvent::Kind::Choke) {
			const auto it = channelOf.find(key);
			if (it == channelOf.end()) {
				continue;
			}
			events.push_back({ tick, { static_cast<uint8_t>(0x80 | it->second), key, 0x40 } });
			channelOf.erase(it);
		}
		else if (bends && e.kind == device::NoteEvent::Kind::Expression && e.expressionId == kTuningExpression) {
			const auto it = channelOf.find(key);
			if (it != channelOf.end()) {
				events.push_back({ tick, bendBytes(it->second, e.expressionValue) });
			}
		}
	}
	// WHAT STILL SOUNDS AT THE LOOP'S END ENDS THERE.
	for (const auto& [key, ch] : channelOf) {
		events.push_back({ endTick, { static_cast<uint8_t>(0x80 | ch), static_cast<uint8_t>(key), 0x40 } });
	}
	std::stable_sort(events.begin(), events.end(), [](const Timed& a, const Timed& b) { return a.tick < b.tick; });

	// THE TRACK: each message after its delta (a run of channel messages written as one event shares the first's delta,
	// the rest at 0), then the end of the track at the loop's end.
	std::vector<uint8_t> track;
	uint32_t at = 0;
	for (const Timed& t : events) {
		const uint32_t tick = std::min(t.tick, endTick);
		if (t.bytes[0] == 0xFF) {
			putVarLen(track, tick - at);
			track.insert(track.end(), t.bytes.begin(), t.bytes.end());
			at = tick;
			continue;
		}
		for (size_t i = 0; i < t.bytes.size(); i += 3) {
			putVarLen(track, i == 0 ? tick - at : 0);
			track.insert(track.end(), t.bytes.begin() + static_cast<std::ptrdiff_t>(i),
			             t.bytes.begin() + static_cast<std::ptrdiff_t>(i + 3));
		}
		at = tick;
	}
	putVarLen(track, endTick - at);
	track.insert(track.end(), { 0xFF, 0x2F, 0x00 });

	std::vector<uint8_t> file { 'M', 'T', 'h', 'd' };
	putU32(file, 6);
	file.insert(file.end(), { 0x00, 0x00, 0x00, 0x01 }); // format 0, one track
	file.push_back(static_cast<uint8_t>(kMidiTicksPerQuarter >> 8));
	file.push_back(static_cast<uint8_t>(kMidiTicksPerQuarter & 0xFF));
	file.insert(file.end(), { 'M', 'T', 'r', 'k' });
	putU32(file, static_cast<uint32_t>(track.size()));
	file.insert(file.end(), track.begin(), track.end());
	return file;
}

} // namespace feathers::mod::arp::pat
