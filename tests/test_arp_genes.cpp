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

#include "mod/arp/genes.h"

#include <cmath>
#include <cstdint>
#include <string>

// THE PATTERN DEVICE'S GENE SPACE, BY VALUE (terrain-arp design §4–§6). The continuity bound is checked with a pitch
// restated here, `round(7·(2·contour − 1))` (a 15-degree span), over many places: the shape of design §6's model, held
// by the code that ships.

namespace genes = feathers::mod::arp::genes;
using genes::Pin;
using genes::Row;

namespace {

/// A REPRODUCIBLE SPREAD OF PLACES: xorshift from a fixed seed (a case's own dice, never the wall clock).
struct Dice {
	uint64_t h = 0x9E3779B97F4A7C15ull;
	double unit() {
		h ^= h >> 12;
		h ^= h << 25;
		h ^= h >> 27;
		return static_cast<double>((h * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
	}
	genes::Place place() {
		genes::Place p;
		for (size_t i = 0; i < genes::kRowCount; ++i) {
			p.phase[i] = unit() * 37.0;
			p.rate[i] = (unit() - 0.5) * 0.2;
		}
		return p;
	}
};

int pitchOf(float contour) {
	return static_cast<int>(std::lround(7.0 * (2.0 * static_cast<double>(contour) - 1.0)));
}

/// THE SHARE OF AN L-STEP PATTERN'S PITCHES THAT CHANGE between two places, averaged over many.
double pitchChange(double dPhase, double dRate, size_t length) {
	Dice dice;
	long changed = 0;
	long total = 0;
	for (int t = 0; t < 2000; ++t) {
		const genes::Place a = dice.place();
		genes::Place b = a;
		for (size_t i = 0; i < genes::kRowCount; ++i) {
			b.phase[i] += dPhase;
			b.rate[i] += dRate;
		}
		for (size_t k = 0; k < length; ++k) {
			const int pa = pitchOf(genes::stepOf(a, k, length)[genes::at(Row::ContourSlow)]);
			const int pb = pitchOf(genes::stepOf(b, k, length)[genes::at(Row::ContourSlow)]);
			changed += pa != pb ? 1 : 0;
			++total;
		}
	}
	return static_cast<double>(changed) / static_cast<double>(total);
}

} // namespace

TEST_CASE("genes: a place decodes to the same pattern every time, and a whole turn of every phase is the same place") {
	Dice dice;
	for (int t = 0; t < 200; ++t) {
		const genes::Place p = dice.place();
		const genes::Pins pins;
		const genes::Wide a = genes::wideOf(p, pins);
		const genes::Wide b = genes::wideOf(p, pins);
		CHECK(a.pinned == b.pinned);
		CHECK(a.direction == b.direction);
		genes::Place turned = p;
		for (size_t i = 0; i < genes::kRowCount; ++i) {
			turned.phase[i] += 3.0;
		}
		for (size_t k = 0; k < 16; ++k) {
			const genes::StepRaw x = genes::stepOf(p, k, 16);
			CHECK(x == genes::stepOf(p, k, 16));
			const genes::StepRaw y = genes::stepOf(turned, k, 16);
			for (size_t i = 0; i < genes::kStepRowCount; ++i) {
				CHECK(std::abs(x[i] - y[i]) < 1e-5f);
				CHECK(x[i] >= 0.0f);
				CHECK(x[i] <= 1.0f);
			}
		}
	}
}

TEST_CASE("genes: a pin at 50% is the field bit for bit; at 0 or switched off it is the neutral 0; at 100% it is 1") {
	Dice dice;
	for (int t = 0; t < 200; ++t) {
		const genes::Place p = dice.place();
		for (size_t pin = 0; pin < genes::kPinCount; ++pin) {
			const Row row = genes::pinRow(static_cast<Pin>(pin));
			const float field = genes::readRow(row, p.phase[genes::at(row)]);
			genes::Pins pins;
			pins.on[pin] = true;      // RATCHET IS OFF BY DEFAULT (opt in); switched on, it keeps the house law like the rest
			pins.amount[pin] = 0.5f; // and VOICE LEADING sits at 1 by default; at 0.5 it is the field, like the rest
			CHECK(genes::wideOf(p, pins).pinned[pin] == field);
			pins.amount[pin] = 0.0f;
			CHECK(genes::wideOf(p, pins).pinned[pin] == 0.0f);
			pins.amount[pin] = 1.0f;
			CHECK(genes::wideOf(p, pins).pinned[pin] == 1.0f);
			pins.amount[pin] = 0.8f;
			pins.on[pin] = false;
			CHECK(genes::wideOf(p, pins).pinned[pin] == 0.0f);
			// A PIN MOVES ONLY ITS OWN VALUE.
			for (size_t other = 0; other < genes::kPinCount; ++other) {
				if (other != pin) {
					CHECK(genes::wideOf(p, pins).pinned[other] == genes::wideOf(p, genes::Pins {}).pinned[other]);
				}
			}
		}
	}
}

TEST_CASE("genes: moving the place MORPHS the pattern — a small phase move changes few steps, and a rate move is held to the same scale at any length") {
	// DESIGN §6. A phase move is Lipschitz: about (2R / duty)·d of the pitches change, at any step. A rate move reaches
	// step k as k·d / L, so its worst step moves as far as a phase move moves every step: on average half as far.
	const double phase16 = pitchChange(0.001, 0.0, 16);
	const double rate16 = pitchChange(0.0, 0.001, 16);
	const double rate64 = pitchChange(0.0, 0.001, 64);
	const double reroll = pitchChange(0.31, 0.0, 16);
	MESSAGE("phase 0.001: " << phase16 << "  rate 0.001, L16: " << rate16 << "  rate 0.001, L64: " << rate64
	                        << "  a move of 0.31: " << reroll);
	CHECK(phase16 < 0.05);
	CHECK(rate16 <= phase16);
	CHECK(rate64 <= phase16);
	// AND IT IS NOT VACUOUS: a big move does re-roll.
	CHECK(reroll > 0.5);
}

TEST_CASE("genes: which pins each Mode reads — design §5's table, the source the window's honesty reads") {
	using M = genes::Mode;
	for (size_t m = 0; m < static_cast<size_t>(M::kCount); ++m) {
		const auto mode = static_cast<M>(m);
		CAPTURE(genes::kModeNames[m]);
		const bool tb = mode == M::TbMono || mode == M::TbGlide;
		// DRUM (design §11-ter) reads nine: no note Length, no pitch Range, no Glide.
		const bool drum = mode == M::Drum;
		// PROGRESSION (design §11-quater) reads eight: no Glide, Length, Range or Octave.
		const bool progression = mode == M::Progression;
		// MODULATION (design §7.10) reads Drum's nine but Octave (the rolls: several values in one step).
		const bool modulation = mode == M::Modulation;
		CHECK(genes::pinRead(mode, Pin::Glide) == tb);
		CHECK(genes::pinRead(mode, Pin::Length) == (!tb && !drum && !progression && !modulation));
		CHECK(genes::pinRead(mode, Pin::Answer) == !tb);
		CHECK(genes::pinRead(mode, Pin::Range) == (!drum && !progression && !modulation));
		CHECK(genes::pinRead(mode, Pin::Octave) == (!progression && !modulation));
		for (const Pin p : { Pin::Motion, Pin::Leap, Pin::Density, Pin::Hold, Pin::Accent, Pin::Repeat }) {
			CHECK(genes::pinRead(mode, p));
		}
		// RATCHET, SPLIT OUT OF DETAIL (opt-in, and a field scaler): the modes that strike ratchets; Detail
		// keeps what is left — Mono's grace notes, Drum's ghosts, Progression's extensions. Never both in one Mode.
		const bool ratchets = mode == M::Melody || mode == M::Chords || mode == M::TbMono;
		CHECK(genes::pinRead(mode, Pin::Ratchet) == ratchets);
		CHECK(genes::pinRead(mode, Pin::Detail) == (mode == M::MelodyMono || drum || progression || modulation));
		CHECK(!(genes::pinRead(mode, Pin::Ratchet) && genes::pinRead(mode, Pin::Detail)));
		// VOICE LEADING: the chord-voicing modes; never beside Glide, whose row it shares.
		CHECK(genes::pinRead(mode, Pin::VoiceLeading) == (mode == M::Chords || progression));
		CHECK(!(genes::pinRead(mode, Pin::VoiceLeading) && genes::pinRead(mode, Pin::Glide)));
		// APPROACH: Chords and Progression first, by design; never beside a TB Mode, whose
		// pitch length shares its row.
		CHECK(genes::pinRead(mode, Pin::Approach) == (mode == M::Chords || progression));
	}
}

TEST_CASE("genes: Ratchet and Approach are OFF by default, reading Detail's and the pitch length's rows; Voice Leading is ON at 1 and reads Glide's; every other pin is on at 0.5") {
	const genes::Pins pins;
	for (size_t pin = 0; pin < genes::kPinCount; ++pin) {
		CAPTURE(genes::kPinNames[pin]);
		CHECK(pins.on[pin] == (static_cast<Pin>(pin) != Pin::Ratchet && static_cast<Pin>(pin) != Pin::Approach));
		CHECK(pins.amount[pin] == (static_cast<Pin>(pin) == Pin::VoiceLeading ? 1.0f : 0.5f));
	}
	CHECK(genes::pinRow(Pin::Ratchet) == genes::pinRow(Pin::Detail));
	CHECK(genes::pinRow(Pin::VoiceLeading) == genes::pinRow(Pin::Glide));
	CHECK(genes::pinRow(Pin::Approach) == genes::Row::WidePitchLength); // the TB polymeter's, which no chord Mode reads
	CHECK(std::string(genes::kPinNames[static_cast<size_t>(Pin::Ratchet)]) == "Ratchet");
}
