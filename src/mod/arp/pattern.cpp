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

#include "mod/arp/pattern.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace feathers::mod::arp::pattern {

using genes::Pin;
using genes::Row;

namespace {

int floorDiv(int a, int b) noexcept {
	const int q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/// A KEY FOLDED INTO MIDI BY OCTAVES, so an out-of-range step keeps its pitch class.
int16_t fold(int key) noexcept {
	while (key > 127) {
		key -= 12;
	}
	while (key < 0) {
		key += 12;
	}
	return static_cast<int16_t>(key);
}

float raw(const genes::StepRaw& s, Row r) noexcept {
	return s[genes::at(r)];
}

/// THE CONTOUR at step k of an L-step cycle, −1..1 (design §5, Motion and Leap): the slow arc and the fast jumps,
/// mixed by Leap, scaled by Motion, tilted across the cycle by Direction.
float contourOf(const genes::StepRaw& s, const genes::Wide& w, size_t k, size_t length) noexcept {
	const float lambda = w.of(Pin::Leap);
	const float m = w.of(Pin::Motion);
	float c = (1.0f - lambda) * (2.0f * raw(s, Row::ContourSlow) - 1.0f) + lambda * (2.0f * raw(s, Row::ContourFast) - 1.0f);
	c *= m;
	if (length > 1) {
		const float across = 2.0f * static_cast<float>(k) / static_cast<float>(length - 1) - 1.0f;
		c += (2.0f * w.direction - 1.0f) * across * m * 0.5f;
	}
	return std::clamp(c, -1.0f, 1.0f);
}

/// The octave the Octave gene puts on a step: up for the lower half of the density, down for the upper.
int octaveOf(const genes::StepRaw& s, const genes::Wide& w) noexcept {
	const float o = raw(s, Row::Octave);
	const float d = w.of(Pin::Octave);
	return o < 0.5f * d ? 12 : (o < d ? -12 : 0);
}

} // namespace

int PatternCycle::keyAt(size_t k, int64_t cycle) const noexcept {
	const PatternStep& s = steps[k];
	if (s.edited) {
		return s.keys[0]; // A HAND SET THIS STEP'S KEY: the polymeter does not move it
	}
	if (pitchLength > 0 && s.noteIndex >= 0 && notesPerCycle > 0) {
		const int64_t i = (cycle * notesPerCycle + s.noteIndex) % pitchLength;
		const int64_t at = i < 0 ? i + pitchLength : i;
		return fold(pitchSeq[static_cast<size_t>(at)] + s.octave);
	}
	return s.keys[0];
}

namespace {

/// **DRUM'S METRIC WEIGHT** of a sixteenth in its bar: the downbeat, the half bar, the backbeats, the eighths, the rest.
float metricWeight(size_t pos) noexcept {
	if (pos == 0) {
		return 1.0f;
	}
	if (pos == 8) {
		return 0.85f;
	}
	if (pos == 4 || pos == 12) {
		return 0.7f;
	}
	return pos % 2 == 0 ? 0.45f : 0.2f;
}

/// **DRUM** (design §11-ter, DECIDED 2026-10-31: gate only, eight lanes). The held keys only GATE (pitch is ignored); the
/// place decides every lane's hits through the same genes the line modes read — so a drag morphs the groove step by step
/// (each lane's decision is a threshold on a row's triangle, continuous in the place), and the same place, pins and lane
/// map always give the same beat. The shared rules that make it a groove and not noise:
///   - THE KICK KEEPS THE DOWNBEAT (step 0 of every bar, at any density); more kicks come with Density, pushed off the
///     strong beats by Motion (syncopation);
///   - THE SNARE AND CLAP PREFER THE BACKBEAT (4 and 12); Detail adds snare ghosts on the weak sixteenths;
///   - THE HATS SUBDIVIDE — quarters, eighths or sixteenths as Density rises; an OPEN hat (Hold's share, on the off-beat
///     eighths) runs until the next closed hat CHOKES it;
///   - Leap brings the toms and the ride (the ride takes a closed hat's place); Answer fills the last quarter of every
///     fourth bar (the phrase's end) with a descending tom run; Octave rolls (the ratchet); Accent accents (velocity);
///     Repeat is how much a bar after the first varies from the first;
///   - A LANE NEVER HITS TWICE ON ONE STEP (one bit), and a lane switched off never sounds.
void buildDrum(const genes::Place& place, const Recipe& r, PatternCycle& out) noexcept {
	using genes::Pin;
	using genes::Row;
	const size_t length = out.length;
	const genes::Wide w = genes::wideOf(place, r.pins, r.warp);
	const float density = w.of(Pin::Density);
	const float motion = w.of(Pin::Motion);
	const float accentP = w.of(Pin::Accent);
	const float detail = w.of(Pin::Detail);
	const float repeatV = w.of(Pin::Repeat);
	const float answer = w.of(Pin::Answer);
	const float roll = w.of(Pin::Octave);
	const float hold = w.of(Pin::Hold);
	const float leap = w.of(Pin::Leap);
	const auto bit = [](DrumLane l) { return static_cast<uint8_t>(1u << static_cast<unsigned>(l)); };
	const auto at = [](Row row) { return genes::at(row); };
	// THE HATS' SUBDIVISION, by Density.
	const size_t hatEvery = density < 0.33f ? 4 : (density < 0.66f ? 2 : 1);
	for (size_t k = 0; k < length; ++k) {
		const size_t pos = k % 16;
		const size_t bar = k / 16;
		const float mw = metricWeight(pos);
		// REPEAT: a bar after the first reads the first bar's genes at this position unless Repeat varies it here.
		const genes::StepRaw own = genes::stepOf(place, k, length, r.warp);
		const genes::StepRaw raw = (bar == 0 || own[at(Row::Repeat)] < repeatV) ? own
		                                                                       : genes::stepOf(place, pos, length, r.warp);
		PatternStep& st = out.steps[k];
		uint8_t hits = 0;
		uint8_t ghosts = 0;
		// KICK.
		if (pos == 0) {
			hits |= bit(DrumLane::Kick);
		}
		else if (pos != 4 && pos != 12) {
			const float p = density * 0.55f * (mw * (1.0f - motion) + (1.0f - mw) * motion * 0.7f);
			if (raw[at(Row::ContourSlow)] < p) {
				hits |= bit(DrumLane::Kick);
			}
		}
		// SNARE AND CLAP: the backbeat; a syncopated snare with Motion; ghosts with Detail.
		if (pos == 4 || pos == 12) {
			if (density > 0.05f) {
				hits |= bit(DrumLane::Snare);
			}
			if (raw[at(Row::Length)] < 0.15f + 0.6f * density) {
				hits |= bit(DrumLane::Clap);
			}
		}
		else if (raw[at(Row::ContourFast)] < motion * density * 0.2f * (1.0f - mw)) {
			hits |= bit(DrumLane::Snare);
		}
		else if (raw[at(Row::Detail)] < detail * 0.35f * (1.0f - mw)) {
			hits |= bit(DrumLane::Snare);
			ghosts |= bit(DrumLane::Snare);
		}
		// HATS (or the ride, with Leap): on the subdivision; open on the off-beat eighths with Hold.
		if (pos % hatEvery == 0) {
			const bool offEighth = pos % 4 == 2;
			if (offEighth && raw[at(Row::Hold)] < hold * 0.6f) {
				hits |= bit(DrumLane::OpenHat);
			}
			else if (raw[at(Row::Voicing)] < leap * 0.4f) {
				hits |= bit(DrumLane::Ride);
			}
			else {
				hits |= bit(DrumLane::ClosedHat);
			}
		}
		else if (raw[at(Row::Octave)] < motion * 0.25f) {
			hits |= bit(DrumLane::ClosedHat);
		}
		// TOMS, with Leap, off the strong beats.
		if (raw[at(Row::Slide)] < leap * 0.15f * (1.0f - mw)) {
			hits |= raw[at(Row::Velocity)] < 0.5f ? bit(DrumLane::LowTom) : bit(DrumLane::HighTom);
		}
		// THE FILL: the last quarter of every fourth bar, with Answer — the hats give way to a descending tom run.
		const bool fill = bar % 4 == 3 && pos >= 12 && raw[at(Row::Answer)] < answer;
		if (fill) {
			hits &= static_cast<uint8_t>(~(bit(DrumLane::ClosedHat) | bit(DrumLane::OpenHat) | bit(DrumLane::Ride)));
			hits |= pos < 14 ? bit(DrumLane::HighTom) : bit(DrumLane::LowTom);
			if (pos == 15) {
				hits |= bit(DrumLane::Snare);
			}
		}
		// THE LANE MAP: a lane switched off never sounds.
		hits &= r.drum.on;
		ghosts &= hits;
		// ROLLS (the ratchet engine), with Octave: on the snare and toms, more in a fill.
		uint8_t rolls = 0;
		const float rollP = roll * (fill ? 0.6f : 0.15f);
		if (raw[at(Row::Repeat)] < rollP) {
			rolls = hits & static_cast<uint8_t>(bit(DrumLane::Snare) | bit(DrumLane::LowTom) | bit(DrumLane::HighTom));
		}
		st.drumHits = hits;
		st.drumGhost = ghosts;
		st.drumRoll = rolls;
		st.ratchet = rolls != 0 ? static_cast<uint8_t>(2 + std::min(2, static_cast<int>(raw[at(Row::Detail)] * 3.0f))) : 1;
		// ACCENTS AND VELOCITY: strong beats accent more; a ghost is soft.
		const float base = r.fixedVelocity ? r.velocity
		                                   : std::clamp(r.velocity * (0.85f + 0.3f * (raw[at(Row::Velocity)] - 0.5f)), 0.0f, 1.0f);
		const bool accented = raw[at(Row::Accent)] < accentP * (0.4f + 0.6f * mw);
		st.drumAccent = accented ? static_cast<uint8_t>(hits & ~ghosts) : 0;
		for (size_t l = 0; l < kDrumLanes; ++l) {
			if (((hits >> l) & 1u) == 0u) {
				continue;
			}
			float v = base;
			if (((st.drumAccent >> l) & 1u) != 0u) {
				v = std::min(1.0f, v + r.accent);
			}
			if (((ghosts >> l) & 1u) != 0u) {
				v *= 0.35f;
			}
			st.drumVelocity[l] = v;
			st.drumLength[l] = r.gate;
		}
		st.accent = st.drumAccent != 0;
		st.kind = hits != 0 ? PatternStep::Kind::Note : PatternStep::Kind::Rest;
		// FOR THE FINGERPRINT AND THE PITCH SOURCE: the lanes' notes, low lane first (as many as a step holds).
		st.count = 0;
		for (size_t l = 0; l < kDrumLanes && st.count < static_cast<int>(theory::kMaxTones); ++l) {
			if (((hits >> l) & 1u) != 0u) {
				st.keys[static_cast<size_t>(st.count++)] = r.drum.note[l];
			}
		}
		st.degree = st.count > 0 ? std::countr_zero(static_cast<unsigned>(hits)) : 0;
		st.velocity = st.count > 0 ? st.drumVelocity[static_cast<size_t>(st.degree)] : 0.0f;
		st.gate = r.gate;
	}
	// THE CHOKE: an open hat runs `0.5 + 3·Hold` steps, or until the next closed hat, whichever is first (the cycle wraps).
	const float holdSteps = 0.5f + 3.0f * hold;
	for (size_t k = 0; k < length; ++k) {
		PatternStep& st = out.steps[k];
		if ((st.drumHits & static_cast<uint8_t>(1u << static_cast<unsigned>(DrumLane::OpenHat))) == 0u) {
			continue;
		}
		float run = holdSteps;
		for (size_t d = 1; d <= length; ++d) {
			const PatternStep& next = out.steps[(k + d) % length];
			if ((next.drumHits & static_cast<uint8_t>(1u << static_cast<unsigned>(DrumLane::ClosedHat))) != 0u) {
				run = std::min(run, static_cast<float>(d));
				break;
			}
		}
		st.drumLength[static_cast<size_t>(DrumLane::OpenHat)] = run;
	}
}


//-- PROGRESSION (design §11-quater) --------------------------------------------------------------------------------//

/// `theory::kScales` indices a Leap borrows from: the parallel major and minor, and Mixolydian for a secondary dominant.
constexpr int kScaleMajor = 0;
constexpr int kScaleMixolydian = 4;
constexpr int kScaleMinor = 5;
/// HOW FAR, IN STEPS, A PROGRESSION CHORD RUNS PAST THE NEXT STRIKE.
constexpr float kLegatoOverlap = 0.02f;

/// THE TONIC THE KEYS GIVE (design §8): the newest press's lowest key under Transpose (`transposeRootOf`), the Root box's
/// under Pool.
int rootKeyOf(const Recipe& r, const HeldChord& held) noexcept {
	return r.keys == Keys::Transpose ? transposeRootOf(held) : 48 + ((r.root % 12) + 12) % 12;
}

/// A CHORD'S ROOT, in semitones above the pattern's tonic (0..11).
int rootSemitones(const ProgressionChord& c, const theory::Scale& scale) noexcept {
	const int semis = c.scale < 0 ? theory::keyOfDegree(scale, 0, c.degree)
	                              : c.tonic + theory::keyOfDegree(theory::scaleOf(static_cast<size_t>(c.scale)), 0, c.degree);
	return ((semis % 12) + 12) % 12;
}

/// THE PITCH CLASSES OF A SCALE CHORD on `degree` (a triad), absolute.
theory::PitchClasses triadClasses(const theory::Scale& scale, int rootKey, int degree) noexcept {
	return theory::pitchClassesOf(
	    theory::voiceChord(scale, rootKey, degree, theory::ChordSize::Triad, 0, 0, theory::Spread::Close, 0));
}

//-- APPROACH (design §10-ter) --------------------------------------------------------------------------------------//

/// THE SCALE KEY `n` STEPS FROM `target` on one side (n ≥ 1): the n-th scale key strictly below it, or strictly above.
/// A target outside the scale (a borrowed chord's tone) is approached from the scale keys either side of it.
int scaleStepFrom(const theory::Scale& scale, int rootKey, int target, int n, bool below) noexcept {
	if (below) {
		return theory::keyOfDegree(scale, rootKey, theory::degreeAtOrBelow(scale, rootKey, target - 1) - (n - 1));
	}
	int d = theory::degreeAtOrBelow(scale, rootKey, target);
	while (theory::keyOfDegree(scale, rootKey, d) <= target) {
		++d;
	}
	return theory::keyOfDegree(scale, rootKey, d + (n - 1));
}

/// **PICKUPS INTO THE CHORD CHANGES** (the Approach pin). Run LAST, on a finished cycle: every change the pin reaches
/// has its last step(s) before it REPLACED by single notes that walk into one tone of the new chord as voiced (so Voice
/// Leading's choice is the target), and the chord before is cut where the first pickup starts. Every choice is a reading
/// of the per-step Slide row, which no chord Mode reads otherwise, so it is the place's alone — deterministic, and the
/// same at any song position (design §6):
///   - at the change k: approached when it reads under the pin; its share under it gives the count (1 / 2 / 3);
///   - at k − 1: the kind (scale step / chromatic / anticipation); at k − 2: the voice (top / bass / any, and which).
/// The side is the musical one: from the side the previous chord's same voice stands on (below when level).
/// A change keeps the chord before it struck: at most (steps since the previous change − 1) pickups.
void addApproaches(const genes::Place& place, const Recipe& r, const genes::Wide& w, const theory::Scale& scale,
                   int rootKey, bool progression, PatternCycle& out) noexcept {
	const float amount = w.of(Pin::Approach);
	const size_t length = out.length;
	if (!(amount > 0.0f) || !genes::pinRead(r.mode, Pin::Approach) || length < 3) {
		return;
	}
	const auto slide = [&](size_t k) {
		return genes::stepOf(place, k % length, length, r.warp)[genes::at(Row::Slide)];
	};
	// THE CHANGES, read off the finished cycle before anything is replaced: Progression's own (`chordStart`); in Chords a
	// struck step whose degree differs from the struck step before it (cyclically).
	std::array<bool, genes::kMaxSteps> change {};
	size_t changes = 0;
	for (size_t k = 0; k < length; ++k) {
		const PatternStep& st = out.steps[k];
		if (st.kind != PatternStep::Kind::Note || st.count < 1) {
			continue;
		}
		if (progression) {
			change[k] = st.chordStart;
		}
		else {
			for (size_t back = 1; back < length; ++back) {
				const PatternStep& p = out.steps[(k + length - back) % length];
				if (p.kind == PatternStep::Kind::Note && p.count >= 1) {
					change[k] = p.degree != st.degree;
					break;
				}
			}
		}
		changes += change[k] ? 1 : 0;
	}
	if (changes == 0) {
		return;
	}
	struct Plan {
		size_t at = 0;
		int count = 0;
		ApproachKind kind = ApproachKind::Scale;
		ApproachVoice voice = ApproachVoice::Top;
		int index = 0;
		int target = 0;
		bool below = true;
	};
	std::array<Plan, genes::kMaxSteps> plans {};
	size_t planned = 0;
	for (size_t k = 0; k < length; ++k) {
		if (!change[k]) {
			continue;
		}
		size_t gap = 1;
		while (gap < length && !change[(k + length - gap) % length]) {
			++gap;
		}
		const float s = slide(k);
		if (!(s < amount)) {
			continue;
		}
		const float share = s / amount;
		int count = share < kApproachOne ? 1 : (share < kApproachTwo ? 2 : 3);
		count = std::min<int>(count, static_cast<int>(gap) - 1);
		if (count < 1) {
			continue;
		}
		const PatternStep& to = out.steps[k];
		Plan p;
		p.at = k;
		p.count = count;
		const float kindReading = slide(k + length - 1);
		p.kind = kindReading < kApproachScale ? ApproachKind::Scale
		         : kindReading < kApproachChromatic ? ApproachKind::Chromatic
		                                            : ApproachKind::Anticipation;
		const float voiceReading = slide(k + length - 2);
		p.voice = voiceReading < kApproachTop ? ApproachVoice::Top
		          : voiceReading < kApproachBass ? ApproachVoice::Bass
		                                         : ApproachVoice::Any;
		const auto indexIn = [&](int n) {
			switch (p.voice) {
			case ApproachVoice::Top: return n - 1;
			case ApproachVoice::Bass: return 0;
			case ApproachVoice::Any: break;
			}
			const float within = (voiceReading - kApproachBass) / (1.0f - kApproachBass);
			return std::clamp(static_cast<int>(within * static_cast<float>(n)), 0, n - 1);
		};
		p.index = indexIn(to.count);
		p.target = to.keys[static_cast<size_t>(p.index)];
		// THE SIDE: where the chord before (the last struck step ahead of the pickups) has this voice.
		p.below = true;
		for (size_t back = static_cast<size_t>(count) + 1; back < length; ++back) {
			const PatternStep& prev = out.steps[(k + length - back) % length];
			if (prev.kind == PatternStep::Kind::Note && prev.count >= 1) {
				p.below = prev.keys[static_cast<size_t>(std::min(indexIn(prev.count), prev.count - 1))] <= p.target;
				break;
			}
		}
		plans[planned++] = p;
	}
	for (size_t i = 0; i < planned; ++i) {
		const Plan& p = plans[i];
		PatternStep& to = out.steps[p.at];
		for (int j = p.count; j >= 1; --j) {
			const size_t at = (p.at + length - static_cast<size_t>(j)) % length;
			PatternStep& st = out.steps[at];
			int key = p.target;
			switch (p.kind) {
			case ApproachKind::Scale: key = scaleStepFrom(scale, rootKey, p.target, j, p.below); break;
			case ApproachKind::Chromatic: key = p.target + (p.below ? -j : j); break;
			case ApproachKind::Anticipation:
				key = j == 1 ? p.target : scaleStepFrom(scale, rootKey, p.target, j - 1, p.below);
				break;
			case ApproachKind::None: break;
			}
			st.kind = PatternStep::Kind::Note;
			st.count = 1;
			st.keys = {};
			st.keys[0] = fold(key);
			st.holdMask = 0;
			st.ratchet = 1; // NO RATCHET ON A PICKUP: it is a lead-in, not a hit
			st.grace = false;
			st.slide = false;
			st.accent = false;
			st.chordStart = false;
			st.noteIndex = -1;
			st.velocity = std::clamp(to.velocity * kApproachVelocity, 0.0f, 1.0f);
			// AN ANTICIPATION'S LAST NOTE IS THE TARGET ITSELF, sounded early: it runs a hair into the change, which holds
			// it (`holdMask`) rather than striking it again.
			st.gate = p.kind == ApproachKind::Anticipation && j == 1 ? 1.0f + kLegatoOverlap : kApproachGate;
			st.approach = p.kind;
			st.approachVoice = p.voice;
			st.approachIndex = static_cast<uint8_t>(j);
			st.approachTarget = fold(p.target);
		}
		if (p.kind == ApproachKind::Anticipation) {
			to.holdMask = static_cast<uint16_t>(to.holdMask | (1u << p.index));
		}
		// THE CHORD BEFORE IS CUT WHERE THE FIRST PICKUP STARTS: its gate ran through ties that are now pickups.
		const size_t first = (p.at + length - static_cast<size_t>(p.count)) % length;
		for (size_t back = 1; back < length; ++back) {
			const size_t at = (first + length - back) % length;
			PatternStep& prev = out.steps[at];
			if (prev.kind == PatternStep::Kind::Note) {
				if (prev.approach == ApproachKind::None) {
					prev.gate = std::min(prev.gate, static_cast<float>(back));
				}
				break;
			}
			if (prev.kind == PatternStep::Kind::Rest) {
				break;
			}
		}
	}
}

void buildProgression(const genes::Place& place, const Recipe& r, const HeldChord& held, PatternCycle& out) noexcept {
	const Progression pr = progressionOf(place, r, held);
	const size_t length = std::clamp<size_t>(pr.count * pr.block, 1, genes::kMaxSteps);
	out.length = length;
	const genes::Wide w = genes::wideOf(place, r.pins, r.warp);
	const theory::Scale scale = theory::scaleOf(r.scale);
	const int rootKey = rootKeyOf(r, held);
	out.rootKey = rootKey;

	// WHERE EACH CHORD STRIKES: on its change, or — Accent, a PUSH — an eighth early (half a beat, a step at least).
	const size_t push = std::max<size_t>(1, pr.stepsPerBeat / 2);
	std::array<size_t, kMaxChords> start {};
	std::array<bool, kMaxChords> pushed {};
	for (size_t i = 0; i < pr.count; ++i) {
		start[i] = i * pr.block;
		const genes::StepRaw c = genes::stepOf(place, i, kMaxChords, r.warp);
		if (i > 0 && pr.block > push && raw(c, Row::Accent) < w.of(Pin::Accent)) {
			start[i] -= push;
			pushed[i] = true;
		}
	}
	// HOLD: within a chord, a beat re-strikes it unless it is held; Hold 1 sustains every chord whole (pads), 0 re-strikes
	// every beat (keys). The share re-struck is (1 − Hold)², so the default leans sustained.
	const float hold = w.of(Pin::Hold);
	const float restrike = (1.0f - hold) * (1.0f - hold);
	std::array<size_t, genes::kMaxSteps> chordAt {};
	std::array<theory::ChordSpec, genes::kMaxSteps> specs {};
	std::array<const theory::ChordSpec*, genes::kMaxSteps> ptr {};
	std::array<genes::StepRaw, genes::kMaxSteps> rawSteps {};
	size_t c = 0;
	for (size_t k = 0; k < length; ++k) {
		while (c + 1 < pr.count && start[c + 1] <= k) {
			++c;
		}
		chordAt[k] = c;
		rawSteps[k] = genes::stepOf(place, k, length, r.warp);
		PatternStep& step = out.steps[k];
		const bool change = k == start[c];
		const bool beat = pr.stepsPerBeat > 0 && k % pr.stepsPerBeat == 0;
		step.kind = change || (beat && raw(rawSteps[k], Row::Hold) < restrike) ? PatternStep::Kind::Note : PatternStep::Kind::Tie;
		step.chordStart = change;
		step.accent = change && pushed[c];
		const ProgressionChord& ch = pr.chord[c];
		theory::ChordSpec& spec = specs[k];
		spec.degree = ch.degree;
		spec.size = ch.size;
		spec.inversion = r.inversion;
		spec.spread = r.spread;
		spec.scale = ch.scale;
		spec.tonic = ch.tonic;
		spec.leading = w.of(Pin::VoiceLeading); // VOICE LEADING: 1 (the default) is the least motion below, exactly
		ptr[k] = step.kind == PatternStep::Kind::Note ? &spec : nullptr;
	}
	// THE EXISTING LEAST-MOTION VOICE LEADING (design §10.2), over the whole cycle so the wrap is led too. No held-note
	// pull here: Pool's steering already chose the chords (`progressionOf`), and a pull would move a family's degrees.
	const int centre = rootKey + static_cast<int>(std::lround(24.0f * w.registerCentre)) - 12;
	const theory::Window window { centre - 9, centre + 16 };
	std::array<theory::Voicing, genes::kMaxSteps> voiced {};
	theory::voiceCycle(scale, rootKey, ptr.data(), length, window, 0, voiced.data());

	const PatternStep* before = nullptr;
	for (size_t k = 0; k < length; ++k) {
		PatternStep& step = out.steps[k];
		const ProgressionChord& ch = pr.chord[chordAt[k]];
		const int semis = rootSemitones(ch, scale);
		step.chordRoot = static_cast<int8_t>(semis);
		// THE DEGREE SOURCE: the chord's own, or a chord from outside the scale's nearest scale degree at or below its root.
		step.degree = ch.scale < 0 ? ch.degree : theory::degreeAtOrBelow(scale, rootKey, rootKey + semis);
		if (step.kind == PatternStep::Kind::Note) {
			const theory::Voicing& v = voiced[k];
			step.count = v.count;
			for (int i = 0; i < v.count; ++i) {
				step.keys[static_cast<size_t>(i)] = fold(v.keys[static_cast<size_t>(i)]);
				step.chordMask = static_cast<uint16_t>(step.chordMask | (1u << (((v.keys[static_cast<size_t>(i)] - rootKey - semis) % 12 + 12) % 12)));
			}
			// A NEW CHORD HOLDS THE TONES IT SHARES WITH THE ONE BEFORE, as Chords does (design §10.1), where Hold says.
			if (step.chordStart && before != nullptr && raw(rawSteps[k], Row::Hold) < hold) {
				for (int i = 0; i < step.count; ++i) {
					for (int j = 0; j < before->count; ++j) {
						if (step.keys[static_cast<size_t>(i)] == before->keys[static_cast<size_t>(j)]) {
							step.holdMask = static_cast<uint16_t>(step.holdMask | (1u << i));
						}
					}
				}
			}
			before = &step;
		}
		else if (before != nullptr) {
			step.chordMask = before->chordMask; // a tie sounds the chord before it
		}
		const genes::StepRaw& s = rawSteps[k];
		const float base = r.fixedVelocity ? r.velocity : r.velocity * (0.85f + 0.3f * raw(s, Row::Velocity));
		step.velocity = std::clamp(base + (step.accent ? r.accent : 0.0f), 0.0f, 1.0f);
		step.gate = 1.0f; // LEGATO: a chord sounds until the next strike
	}
	// A STRIKE'S GATE RUNS THROUGH THE TIES AFTER IT.
	for (size_t k = 0; k < length; ++k) {
		PatternStep& step = out.steps[k];
		if (step.kind != PatternStep::Kind::Note) {
			continue;
		}
		size_t ties = 0;
		while (k + ties + 1 < length && out.steps[k + ties + 1].kind == PatternStep::Kind::Tie) {
			++ties;
		}
		// A HAIR PAST THE NEXT STRIKE (legato): an off due ON the next onset is written before it, so a common tone the next
		// chord holds would have ended already and been struck again (the device case caught it).
		step.gate = static_cast<float>(ties) + 1.0f + kLegatoOverlap;
	}
	// PICKUPS INTO THE CHANGES (the Approach pin; Off by default, and then nothing here runs).
	addApproaches(place, r, w, scale, rootKey, true, out);
}
} // namespace

void build(const genes::Place& place, const Recipe& r, const HeldChord& held, PatternCycle& out) noexcept {
	const size_t length = std::clamp<size_t>(r.length, 1, genes::kMaxSteps);
	out = PatternCycle {};
	out.length = length;
	if (genes::isProgression(r.mode)) {
		// PROGRESSION'S CYCLE IS ITS PROGRESSION, not the Length box (design §11-quater) — so the playhead loops there even
		// before a key is held.
		const Progression pr = progressionOf(place, r, held);
		out.length = std::clamp<size_t>(pr.count * pr.block, 1, genes::kMaxSteps);
	}
	if (held.empty()) {
		return; // NOTHING HELD, NOTHING PLAYS (design §17).
	}
	out.playable = true;
	if (genes::isProgression(r.mode)) {
		buildProgression(place, r, held, out);
		return;
	}
	// DRUM: the held keys only gate (pitch is ignored), and the lanes are decided by the place alone.
	if (genes::decodesDrum(r.mode)) {
		buildDrum(place, r, out);
		return;
	}
	const genes::Wide w = genes::wideOf(place, r.pins, r.warp);
	const theory::Scale scale = theory::scaleOf(r.scale);
	const bool tb = genes::isTb(r.mode);
	const bool chords = r.mode == Mode::Chords;
	const bool mono = r.mode == Mode::MelodyMono;

	// THE KEYS (design §8). Transpose: the newest press's lowest key is the root (`transposeRootOf`). Pool: the held keys snapped into the scale on the
	// Root box's tonic, sorted, de-duplicated — a degree indexes into them, each `n` of them an octave.
	theory::PitchClasses heldClasses = 0;
	for (size_t i = 0; i < held.size(); ++i) {
		heldClasses = static_cast<theory::PitchClasses>(heldClasses | (1u << (((held[i].key % 12) + 12) % 12)));
	}
	std::array<int, kMaxHeld> pool {};
	int poolCount = 0;
	if (r.keys == Keys::Pool) {
		for (size_t i = 0; i < held.size(); ++i) {
			const int k = theory::snapToScale(scale, r.root, held[i].key);
			if (std::find(pool.begin(), pool.begin() + poolCount, k) == pool.begin() + poolCount) {
				pool[static_cast<size_t>(poolCount++)] = k;
			}
		}
		std::sort(pool.begin(), pool.begin() + poolCount);
	}
	const int rootKey = r.keys == Keys::Transpose ? transposeRootOf(held) : 48 + ((r.root % 12) + 12) % 12;
	out.rootKey = rootKey;
	const auto lineKey = [&](int degree) {
		if (r.keys == Keys::Pool && poolCount > 0) {
			const int q = floorDiv(degree, poolCount);
			return pool[static_cast<size_t>(degree - q * poolCount)] + 12 * q;
		}
		return theory::keyOfDegree(scale, rootKey, degree);
	};

	std::array<genes::StepRaw, genes::kMaxSteps> rawSteps {};
	for (size_t k = 0; k < length; ++k) {
		rawSteps[k] = genes::stepOf(place, k, length, r.warp);
	}

	//-- THE KINDS: rest, tie (or a held chord), note -----------------------------------------------------------------//
	const float density = w.of(Pin::Density);
	const float hold = w.of(Pin::Hold);
	const size_t phrase = 4 * (1 + static_cast<size_t>(std::min(2.0f, std::floor(3.0f * w.phrase))));
	for (size_t k = 0; k < length; ++k) {
		const genes::StepRaw& s = rawSteps[k];
		PatternStep& step = out.steps[k];
		step.kind = PatternStep::Kind::Note;
		if (raw(s, Row::Rest) < density) {
			step.kind = PatternStep::Kind::Rest;
		}
		// MELODY MONO BREATHES: the last step of each phrase rests whenever there are rests at all.
		if (mono && density > 0.0f && (k + 1) % phrase == 0) {
			step.kind = PatternStep::Kind::Rest;
		}
		const bool afterSound = k > 0 && out.steps[k - 1].kind != PatternStep::Kind::Rest;
		if (step.kind == PatternStep::Kind::Note && afterSound && raw(s, Row::Hold) < hold && !mono) {
			step.kind = PatternStep::Kind::Tie;
		}
		step.contour = 0.5f * (contourOf(s, w, k, length) + 1.0f);
	}

	//-- PITCH --------------------------------------------------------------------------------------------------------//
	if (!chords) {
		const int span = tb ? std::min(2 + static_cast<int>(std::lround(12.0f * w.of(Pin::Range))), scale.count)
		                    : 2 + static_cast<int>(std::lround(12.0f * w.of(Pin::Range)));
		// THE REGISTER MOVES WITH MOTION, so Motion off is the drone on the root (design §5) wherever the register sat.
		const int registerShift =
		    tb ? 0
		       : static_cast<int>(std::lround((2.0f * w.registerCentre - 1.0f) * w.of(Pin::Motion)
		                                      * static_cast<float>(scale.count) * 0.5f));
		std::array<int, genes::kMaxSteps> degree {};
		const size_t motif = 2 + static_cast<size_t>(std::min(6.0f, std::floor(6.0f * w.motif)));
		for (size_t k = 0; k < length; ++k) {
			const float c = 2.0f * out.steps[k].contour - 1.0f;
			degree[k] = static_cast<int>(std::lround(c * static_cast<float>(span) * 0.5f)) + registerShift;
			// REPEAT: a step copies the motif's step before it, which is what turns a walk into a TUNE.
			if (k >= motif && raw(rawSteps[k], Row::Repeat) < w.of(Pin::Repeat)) {
				degree[k] = degree[k - motif];
			}
			// ANSWER: the second half inverts the first about the root — call and response.
			if (!tb && length >= 4 && k >= length / 2 && raw(rawSteps[k], Row::Answer) < w.of(Pin::Answer)) {
				degree[k] = -degree[k - length / 2];
			}
			out.steps[k].octave = octaveOf(rawSteps[k], w);
			out.steps[k].degree = degree[k] + r.degree;
			out.steps[k].keys[0] = fold(lineKey(degree[k] + r.degree) + out.steps[k].octave);
			out.steps[k].graceKey = fold(lineKey(degree[k] + r.degree + 1) + out.steps[k].octave);
			out.steps[k].count = 1;
		}
		// TB'S POLYMETER (design §11.4): the notes walk a pitch sequence of their own length P.
		if (tb && r.pitchOnNotes) {
			const int p = 3 + static_cast<int>(std::min(13.0f, std::floor(13.0f * w.pitchLength)));
			out.pitchLength = p;
			for (int i = 0; i < p; ++i) {
				const genes::StepRaw s = genes::stepOf(place, static_cast<size_t>(i), static_cast<size_t>(p), r.warp);
				const float c = 2.0f * (0.5f * (contourOf(s, w, static_cast<size_t>(i), static_cast<size_t>(p)) + 1.0f)) - 1.0f;
				const int d = static_cast<int>(std::lround(c * static_cast<float>(span) * 0.5f));
				out.pitchSeq[static_cast<size_t>(i)] = static_cast<int16_t>(lineKey(d + r.degree));
			}
			int n = 0;
			for (size_t k = 0; k < length; ++k) {
				if (out.steps[k].kind == PatternStep::Kind::Note) {
					out.steps[k].noteIndex = n++;
				}
			}
			out.notesPerCycle = n;
		}
	}
	else {
		//-- CHORDS (design §10): a degree per harmonic block, voiced over the whole cycle ---------------------------//
		const size_t block = size_t { 1 } << static_cast<size_t>(std::min(3.0f, std::floor(4.0f * w.phrase)));
		const float m = w.of(Pin::Motion);
		const float lambda = w.of(Pin::Leap);
		const int thin = w.size < 0.6f ? 0 : (w.size < 0.85f ? 1 : 2);
		// THE WINDOW SITS ON THE ROOT, so a transposition moves every voicing with it (design §8: a held key moves the
		// whole pattern by its interval).
		const int centre = rootKey + static_cast<int>(std::lround(24.0f * w.registerCentre)) - 12;
		const int half = 5 + static_cast<int>(std::lround(10.0f * w.of(Pin::Range)));
		const theory::Window window { centre - half, centre + half + 7 };
		std::array<theory::ChordSpec, genes::kMaxSteps> specs {};
		std::array<const theory::ChordSpec*, genes::kMaxSteps> ptr {};
		for (size_t k = 0; k < length; ++k) {
			const size_t b = k - k % block;
			const genes::StepRaw& s = rawSteps[b];
			int d = r.degree + static_cast<int>(std::lround(m * 3.0f * (2.0f * raw(s, Row::ContourSlow) - 1.0f)));
			const float fast = raw(s, Row::ContourFast);
			if (fast < lambda) {
				d += fast < 0.5f * lambda ? 3 : -3; // ROOT MOTION BY A FOURTH, either way
			}
			// ANSWER — THE CADENCE: the cycle's last block pulls home to the Degree box.
			if (length >= 4 && b + block >= length && raw(s, Row::Answer) < w.of(Pin::Answer)) {
				d = r.degree;
			}
			specs[k].degree = d;
			specs[k].size = r.size;
			specs[k].thin = thin;
			specs[k].inversion = r.inversion;
			specs[k].spread = r.spread;
			specs[k].leading = w.of(Pin::VoiceLeading); // VOICE LEADING: 1 (the default) is the least motion, exactly
			specs[k].second = r.inversion == theory::kAuto && raw(rawSteps[k], Row::Voicing) < 0.2f;
			out.steps[k].degree = d;
			// A HOLD ON A NEW DEGREE IS NOT A TIE: it strikes the new chord and holds the tones the two share (below).
			if (out.steps[k].kind == PatternStep::Kind::Tie && k > 0 && out.steps[k - 1].degree != d) {
				out.steps[k].kind = PatternStep::Kind::Note;
			}
			ptr[k] = out.steps[k].kind == PatternStep::Kind::Note ? &specs[k] : nullptr;
		}
		std::array<theory::Voicing, genes::kMaxSteps> voiced {};
		const theory::PitchClasses pull = held.size() >= 2 ? heldClasses : 0;
		if (r.voice) {
			theory::voiceCycle(scale, rootKey, ptr.data(), length, window, pull, voiced.data());
		}
		else {
			for (size_t k = 0; k < length; ++k) {
				if (ptr[k] != nullptr) {
					voiced[k] = theory::voiceChord(scale, rootKey, ptr[k]->degree, ptr[k]->size, ptr[k]->thin, 0,
					                               ptr[k]->spread, 0);
				}
			}
		}
		// THE CHORD BEFORE, as played (its keys after the octave), for a held step's common tones.
		PatternStep* before = nullptr;
		for (size_t k = 0; k < length; ++k) {
			PatternStep& step = out.steps[k];
			if (step.kind == PatternStep::Kind::Rest) {
				before = nullptr;
				continue;
			}
			if (step.kind != PatternStep::Kind::Note) {
				continue; // a tie holds the chord before it whole
			}
			const theory::Voicing& v = voiced[k];
			step.octave = octaveOf(rawSteps[k], w);
			step.count = v.count;
			for (int i = 0; i < v.count; ++i) {
				step.keys[static_cast<size_t>(i)] = fold(v.keys[static_cast<size_t>(i)] + step.octave);
			}
			// A HELD CHORD STEP: the tones it shares with the chord before hold instead of re-striking (design §10.1).
			if (before != nullptr && raw(rawSteps[k], Row::Hold) < hold) {
				for (int i = 0; i < step.count; ++i) {
					for (int j = 0; j < before->count; ++j) {
						if (step.keys[static_cast<size_t>(i)] == before->keys[static_cast<size_t>(j)]) {
							step.holdMask = static_cast<uint16_t>(step.holdMask | (1u << i));
						}
					}
				}
			}
			before = &step;
		}
	}

	//-- GATE, ACCENT, VELOCITY, SLIDE, DETAIL ------------------------------------------------------------------------//
	for (size_t k = 0; k < length; ++k) {
		const genes::StepRaw& s = rawSteps[k];
		PatternStep& step = out.steps[k];
		step.gate = tb ? 0.5f
		               : std::clamp(r.gate * (1.0f + (2.0f * raw(s, Row::Length) - 1.0f) * 0.75f * w.of(Pin::Length)),
		                            0.05f, 1.0f);
		step.accent = step.kind == PatternStep::Kind::Note && raw(s, Row::Accent) < w.of(Pin::Accent);
		const float base = r.fixedVelocity || tb ? r.velocity : r.velocity * (0.85f + 0.3f * raw(s, Row::Velocity));
		step.velocity = std::clamp(base + (step.accent ? r.accent : 0.0f), 0.0f, 1.0f);
		// MELODY MONO'S GRACE NOTES ARE DETAIL'S; EVERY OTHER MODE'S RATCHETS ARE RATCHET'S, read off the same per-step row the ratchets always read, so a Ratchet switched on strikes the
		// steps and the hits it did when they were Detail's. A Mode that does not read the pin strikes none (TB-Glide).
		if (step.kind == PatternStep::Kind::Note) {
			const float d = raw(s, Row::Detail);
			if (mono) {
				step.grace = d < w.of(Pin::Detail);
			}
			else if (const float ratchet = w.of(Pin::Ratchet); genes::pinRead(r.mode, Pin::Ratchet) && d < ratchet) {
				step.ratchet = static_cast<uint8_t>(2 + std::min(2, static_cast<int>(3.0f * d / ratchet)));
			}
		}
	}
	// SLIDES AND SLURS overlap the NEXT note, so they are decided once every step's kind is known. TB: the Glide gene.
	// Melody Mono: the Hold gene, as a slur. Neither crosses into a rest, or past the cycle's end into the next cycle's
	// first step unless that step sounds.
	for (size_t k = 0; k < length; ++k) {
		PatternStep& step = out.steps[k];
		const PatternStep& next = out.steps[(k + 1) % length];
		if (step.kind == PatternStep::Kind::Rest || next.kind != PatternStep::Kind::Note || length < 2) {
			continue;
		}
		const genes::StepRaw& s = rawSteps[k];
		if ((tb && raw(s, Row::Slide) < w.of(Pin::Glide)) || (mono && raw(s, Row::Hold) < hold)) {
			step.slide = true;
		}
	}
	// TIES AND SLIDES EXTEND THE NOTE THAT STARTED THEM: a note's gate runs through every tie after it.
	for (size_t k = 0; k < length; ++k) {
		PatternStep& step = out.steps[k];
		if (step.kind != PatternStep::Kind::Note) {
			continue;
		}
		size_t ties = 0;
		while (k + ties + 1 < length && out.steps[k + ties + 1].kind == PatternStep::Kind::Tie) {
			++ties;
		}
		if (ties > 0) {
			step.gate = static_cast<float>(ties) + out.steps[k + ties].gate;
			// A tied note's slide belongs to its last tied step.
			step.slide = step.slide || out.steps[k + ties].slide;
		}
		if (step.slide) {
			step.gate = std::max(step.gate, static_cast<float>(ties) + 1.0f);
		}
	}
	// PICKUPS INTO THE CHANGES (the Approach pin; Off by default, and then nothing here runs) — after every gate is set,
	// so the chord before a pickup is cut where it starts.
	if (chords) {
		addApproaches(place, r, w, scale, rootKey, false, out);
	}
}

bool editMatches(const StepEdit& e, const PatternStep& terrain, bool drum) noexcept {
	if (drum) {
		return terrain.drumHits == e.origHits;
	}
	// IN WHAT THE HELD KEYS DO NOT MOVE: kind, degree and octave — not the key, which is the held key's transposition.
	return terrain.kind == e.origKind
	       && (terrain.kind != PatternStep::Kind::Note || (terrain.degree == e.origDegree && terrain.octave == e.origOctave));
}

StepEdit editOf(const PatternStep& terrain, bool drum) noexcept {
	StepEdit e;
	e.on = true;
	e.kind = terrain.kind;
	e.pitched = false;
	e.key = 0;
	e.velocity = terrain.velocity;
	e.gate = terrain.gate;
	e.accent = terrain.accent;
	e.slide = terrain.slide;
	e.drumHits = terrain.drumHits;
	e.drumAccent = terrain.drumAccent;
	e.origKind = terrain.kind;
	e.origDegree = static_cast<int16_t>(terrain.degree);
	e.origOctave = static_cast<int16_t>(terrain.octave);
	e.origHits = terrain.drumHits;
	(void)drum;
	return e;
}

void applyEdits(const std::array<StepEdit, genes::kMaxSteps>& edits, bool drum, PatternCycle& cycle, bool levelsAreHits) noexcept {
	for (size_t k = 0; k < cycle.length; ++k) {
		const StepEdit& e = edits[k];
		PatternStep& st = cycle.steps[k];
		if (!e.on || !editMatches(e, st, drum)) {
			continue;
		}
		// THE TERRAIN'S KEY HERE, as this cycle decodes it (the polymeter included) — read before the step is marked.
		const int terrainKey = st.kind == PatternStep::Kind::Note ? cycle.keyAt(k, 0) : cycle.rootKey;
		st.edited = true;
		if (drum) {
			// THE HITS AS SET; a hit added takes the step's base velocity, an accent the accent's.
			const float base = st.velocity > 0.0f ? st.velocity : e.velocity;
			for (size_t l = 0; l < kDrumLanes; ++l) {
				const bool hit = ((e.drumHits >> l) & 1u) != 0u;
				if (hit && ((st.drumHits >> l) & 1u) == 0u) {
					st.drumVelocity[l] = base;
					st.drumLength[l] = st.gate > 0.0f ? st.gate : 0.5f;
				}
				// AN ACCENT A HAND ADDED boosts the hit; one the terrain already accented is boosted already (an untouched copy of
				// the step — `editOf` — must leave it exactly as it was, not boost it twice).
				if (hit && ((e.drumAccent >> l) & 1u) != 0u && ((st.drumAccent >> l) & 1u) == 0u) {
					st.drumVelocity[l] = std::min(1.0f, std::max(st.drumVelocity[l], base) + 0.25f);
				}
			}
			// **A CELL'S OWN LEVEL**: where a hand set one, the hit's velocity is that level — Accent still boosts on
			// top, by the same quarter a hand's accent always added. In Modulation a set cell IS a hit (the lane takes the level
			// there and holds it); in Drum it shapes only a lane that hits. A cell nobody set is exactly as before.
			const uint8_t hits = static_cast<uint8_t>(e.drumHits | (levelsAreHits ? e.cellSet : 0u));
			for (size_t l = 0; l < kDrumLanes; ++l) {
				const auto b = static_cast<uint8_t>(1u << l);
				if ((e.cellSet & b) == 0u || (hits & b) == 0u) {
					continue;
				}
				const float level = std::clamp(e.cellLevel[l], 0.0f, 1.0f);
				st.drumVelocity[l] = (e.drumAccent & b) != 0u ? std::min(1.0f, level + 0.25f) : level;
				if ((st.drumHits & b) == 0u && (e.drumHits & b) == 0u) {
					st.drumLength[l] = st.gate > 0.0f ? st.gate : 0.5f;
				}
			}
			st.drumHits = hits;
			st.drumAccent = static_cast<uint8_t>(e.drumAccent & hits);
			st.drumGhost &= e.drumHits;
			st.drumRoll &= e.drumHits;
			st.accent = st.drumAccent != 0;
			st.kind = st.drumHits != 0 ? PatternStep::Kind::Note : PatternStep::Kind::Rest;
			continue;
		}
		// A PITCHED STEP: its kind, its key (a chord moves whole, by the root's move), its feel. The key a hand set is spelled
		// from THIS cycle's root; one it did not set is the terrain's own, as this cycle decodes it.
		const int want = e.pitched ? std::clamp(cycle.rootKey + e.key, 0, 127) : terrainKey;
		const int shift = want - (st.count > 0 ? st.keys[0] : want);
		if (e.kind == PatternStep::Kind::Note && st.count == 0) {
			st.count = 1;
		}
		for (int i = 0; i < st.count; ++i) {
			st.keys[static_cast<size_t>(i)] = static_cast<int16_t>(std::clamp(st.keys[static_cast<size_t>(i)] + shift, 0, 127));
		}
		st.keys[0] = static_cast<int16_t>(want);
		st.kind = e.kind;
		st.velocity = e.velocity;
		st.gate = e.gate;
		st.accent = e.accent;
		st.slide = e.slide;
	}
}

void setEditKey(StepEdit& e, const PatternCycle& cycle, int key) noexcept {
	e.kind = PatternStep::Kind::Note;
	e.pitched = true;
	e.key = static_cast<int16_t>(key - cycle.rootKey);
}

//-- PROGRESSION (design §11-quater) --------------------------------------------------------------------------------//

FamilyDegrees familyDegrees(Family f) noexcept {
	const auto of = [](std::initializer_list<int> ds) {
		FamilyDegrees out;
		for (const int d : ds) {
			out.degree[out.count++] = static_cast<int8_t>(d);
		}
		return out;
	};
	switch (f) {
	case Family::PopLoop: return of({ 0, 4, 5, 3 });
	case Family::Deceptive: return of({ 0, 3, 4, 5 });
	case Family::Plagal: return of({ 0, 5, 3, 0 });
	case Family::VampFour: return of({ 0, 3 });
	case Family::VampSeven: return of({ 0, 6 });
	case Family::Descending: return of({ 0, 6, 5, 4 });
	case Family::Blues: return of({ 0, 0, 0, 0, 3, 3, 0, 0, 4, 3, 0, 4 });
	case Family::Turnaround: return of({ 0, 5, 1, 4 });
	case Family::TwoFiveOne: return of({ 1, 4, 0, 0 });
	case Family::Circle: return of({ 0, 3, 6, 2, 5, 1, 4, 0 });
	case Family::kCount: break;
	}
	return of({ 0 });
}

FamilyReading familyOf(const genes::Wide& w) noexcept {
	// THE MOTIF ROW (pattern-wide, no pin) WALKS THE FAMILIES: a bin each, the next family across its width.
	const float x = std::clamp(w.motif, 0.0f, 1.0f) * static_cast<float>(kFamilies);
	const size_t from = std::min(static_cast<size_t>(x), kFamilies - 1);
	FamilyReading out;
	out.from = static_cast<Family>(from);
	out.to = static_cast<Family>(std::min(from + 1, kFamilies - 1));
	out.across = std::clamp(x - static_cast<float>(from), 0.0f, 1.0f);
	return out;
}

Progression progressionOf(const genes::Place& place, const Recipe& r, const HeldChord& held) noexcept {
	Progression pr;
	const genes::Wide w = genes::wideOf(place, r.pins, r.warp);
	const theory::Scale scale = theory::scaleOf(r.scale);
	// THE BAR (4/4) IN STEPS, and DENSITY's changes a bar: one every two bars, one a bar, or two.
	const double stepBeats = r.stepBeats > 0.0 ? r.stepBeats : 0.25;
	pr.stepsPerBeat = static_cast<size_t>(std::max(1L, std::lround(1.0 / stepBeats)));
	const double perBar = 4.0 / stepBeats;
	const float density = w.of(Pin::Density);
	const double bars = density < 1.0f / 3.0f ? 2.0 : (density < 2.0f / 3.0f ? 1.0 : 0.5);
	pr.block = static_cast<size_t>(std::clamp(std::lround(perBar * bars), 1L, static_cast<long>(genes::kMaxSteps)));

	// THE FAMILY, AND THE NEXT ONE ACROSS: chord j takes the next family's chord once the reading is (j+1)/(n+1) across,
	// so a puck move changes the progression a chord at a time (design §11-quater, continuity).
	const FamilyReading fr = familyOf(w);
	const FamilyDegrees a = familyDegrees(fr.from);
	const FamilyDegrees b = familyDegrees(fr.to);
	const size_t n = fr.across < 0.5f ? a.count : b.count;
	// REPEAT: loop the family, or DEVELOP it — twice as long, the second time through varied.
	const bool develop = w.of(Pin::Repeat) >= 0.5f;
	size_t total = develop ? 2 * n : n;
	total = std::min({ total, kMaxChords, std::max<size_t>(1, genes::kMaxSteps / pr.block) });
	pr.count = total;
	std::array<genes::StepRaw, kMaxChords> rows {};
	for (size_t i = 0; i < total; ++i) {
		rows[i] = genes::stepOf(place, i, kMaxChords, r.warp); // a chord's own rows, whatever the progression's length
	}
	const float motion = w.of(Pin::Motion);
	for (size_t i = 0; i < total; ++i) {
		const size_t j = i % n;
		const bool next = fr.across > static_cast<float>(j + 1) / static_cast<float>(n + 1);
		int d = next ? b.degree[j % b.count] : a.degree[j % a.count];
		// MOTION: the root LEAPS — a fourth or a fifth on from the chord before — where the family would have stepped.
		if (i > 0 && raw(rows[i], Row::ContourSlow) < 0.5f * motion) {
			d = pr.chord[i - 1].degree + (raw(rows[i], Row::Octave) < 0.5f ? 3 : 4);
		}
		// DEVELOP: the second time through, a chord may give way to its third-related substitute (two tones shared).
		if (i >= n && raw(rows[i], Row::Repeat) < 0.5f) {
			d += raw(rows[i], Row::Rest) < 0.5f ? 2 : -2;
		}
		pr.chord[i].degree = ((d % 7) + 7) % 7;
	}
	// POOL STEERS (design §8): a chord that holds none of the held notes gives way to the third-related one that holds the
	// most; the tonic chord stays. Transpose only moves the tonic.
	if (r.keys == Keys::Pool && !held.empty()) {
		const int rootKey = rootKeyOf(r, held);
		theory::PitchClasses heldClasses = 0;
		for (size_t i = 0; i < held.size(); ++i) {
			heldClasses = static_cast<theory::PitchClasses>(heldClasses | (1u << (((held[i].key % 12) + 12) % 12)));
		}
		for (size_t i = 1; i < total; ++i) {
			int& d = pr.chord[i].degree;
			int best = d;
			int bestHolds = theory::countIn(triadClasses(scale, rootKey, d), heldClasses);
			for (const int alt : { d + 2, d - 2 }) {
				const int h = theory::countIn(triadClasses(scale, rootKey, alt), heldClasses);
				if (h > bestHolds) {
					best = alt;
					bestHolds = h;
				}
			}
			d = ((best % 7) + 7) % 7;
		}
	}
	// ANSWER — THE PHRASE-END CADENCE: none, authentic (V–I), plagal (IV–I) or deceptive (V–vi) as it rises.
	const float answer = w.of(Pin::Answer);
	if (total >= 2 && answer > 0.0f) {
		const int penult = answer < 2.0f / 3.0f && answer >= 1.0f / 3.0f ? 3 : 4;
		const int last = answer >= 2.0f / 3.0f ? 5 : 0;
		pr.chord[total - 2].degree = penult;
		pr.chord[total - 1].degree = last;
		pr.chord[total - 2].cadence = true;
		pr.chord[total - 1].cadence = true;
	}
	// LEAP: a chord borrowed from the parallel scale, or the secondary dominant of the chord after it. Never the tonic
	// chord that opens, and never the cadence. WALKED BACKWARDS, so the chord a secondary dominant resolves to is already
	// the one that will play (a case caught it aiming at a chord Leap borrowed afterwards).
	const float leap = w.of(Pin::Leap);
	const bool majorThird = scale.count >= 3 && std::find(scale.steps.begin(), scale.steps.begin() + scale.count, 4)
	                                                != scale.steps.begin() + scale.count;
	for (size_t i = total - 1; i >= 1; --i) {
		ProgressionChord& ch = pr.chord[i];
		const float f = raw(rows[i], Row::ContourFast);
		if (ch.cadence || f >= leap) {
			continue;
		}
		if (f < 0.5f * leap) {
			const ProgressionChord& to = pr.chord[(i + 1) % total];
			ch.scale = kScaleMixolydian;
			ch.tonic = (rootSemitones(to, scale) + 7) % 12;
			ch.degree = 0;
			ch.size = theory::ChordSize::Seventh;
		}
		else {
			ch.scale = majorThird ? kScaleMinor : kScaleMajor;
			ch.tonic = 0;
		}
	}
	// DETAIL: extensions — a ninth, a seventh or a sus4 — on the chords it reaches.
	const float detail = w.of(Pin::Detail);
	for (size_t i = 0; i < total; ++i) {
		const float t = raw(rows[i], Row::Detail);
		if (t >= detail) {
			continue;
		}
		ProgressionChord& ch = pr.chord[i];
		if (t < detail / 3.0f) {
			ch.size = theory::ChordSize::Ninth;
		}
		else if (t < 2.0f * detail / 3.0f || ch.size == theory::ChordSize::Seventh) {
			ch.size = theory::ChordSize::Seventh;
		}
		else {
			ch.size = theory::ChordSize::Sus4;
		}
	}
	return pr;
}

void numeralOf(const PatternStep& step, char* out, size_t size) noexcept {
	if (size == 0) {
		return;
	}
	out[0] = '\0';
	if (step.chordRoot < 0) {
		return;
	}
	static constexpr const char* kNames[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
	const auto has = [&](int i) { return ((step.chordMask >> i) & 1u) != 0u; };
	const bool minor = has(3) && !has(4);
	const bool major = has(4);
	const bool diminished = minor && has(6) && !has(7);
	const bool augmented = major && has(8) && !has(7);
	char name[8] {};
	const char* src = kNames[static_cast<size_t>(step.chordRoot % 12)];
	size_t n = 0;
	for (; src[n] != '\0' && n + 1 < sizeof name; ++n) {
		// THE ACCIDENTAL STAYS AS IT IS; THE NUMERAL IS LOWER CASE FOR A MINOR CHORD.
		name[n] = minor && src[n] >= 'A' && src[n] <= 'Z' ? static_cast<char>(src[n] - 'A' + 'a') : src[n];
	}
	name[n] = '\0';
	const char* quality = diminished ? "o" : (augmented ? "+" : "");
	const char* ext = "";
	if (!minor && !major) {
		ext = has(5) ? "sus4" : (has(2) ? "sus2" : "");
	}
	else if (has(10) || has(11) || (diminished && has(9))) {
		const bool ninth = has(2) || has(1);
		ext = has(11) && major ? (ninth ? "maj9" : "maj7") : (ninth ? "9" : "7");
	}
	std::snprintf(out, size, "%s%s%s", name, quality, ext);
}

int transposeRootOf(const HeldChord& held) noexcept {
	uint32_t newest = 0;
	for (size_t i = 0; i < held.size(); ++i) {
		newest = std::max(newest, held[i].order);
	}
	int root = 127;
	for (size_t i = 0; i < held.size(); ++i) {
		if (held[i].order == newest) {
			root = std::min<int>(root, held[i].key);
		}
	}
	return root;
}

} // namespace feathers::mod::arp::pattern
