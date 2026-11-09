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

#include "mod/arp/pattern_types.h"

#include "device/state_blocks.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp::pat {

namespace {

float valueOf(const float* fields, size_t present, size_t i) {
	const device::ParamDesc& d = kParamDescs[i];
	const float v = fields != nullptr && i < present && std::isfinite(fields[i]) ? fields[i] : static_cast<float>(d.def);
	return std::clamp(v, static_cast<float>(d.min), static_cast<float>(d.max));
}

} // namespace


EditState editStateOf(const float* in, size_t n) noexcept {
	EditState out {};
	// V4, OR V3 (read into v4 with no cell set: a patch saved before cells keeps every edit it had).
	const bool v4 = n == kEditsWords;
	if (in == nullptr || (!v4 && n != kEditsWordsV3)) {
		return out;
	}
	device::BlockReader r(in, n);
	EditState read {};
	for (EditOverlay& o : read) {
		for (float& a : o.anchor) {
			a = r.f32();
		}
		o.mode = static_cast<Mode>(std::min<uint32_t>(r.u16(), static_cast<uint32_t>(Mode::kCount) - 1));
		for (pattern::StepEdit& e : o.steps) {
			const uint32_t flags = r.u16();
			e.on = (flags & 1u) != 0u;
			e.pitched = (flags & 32u) != 0u;
			e.kind = static_cast<pattern::PatternStep::Kind>(std::min<uint32_t>((flags >> 1) & 3u, 2u));
			e.accent = (flags & 8u) != 0u;
			e.slide = (flags & 16u) != 0u;
			e.key = static_cast<int16_t>(static_cast<int>(r.u16()) - 256);
			e.velocity = std::clamp(r.f32(), 0.0f, 1.0f);
			e.gate = std::clamp(r.f32(), 0.0f, 64.0f);
			const uint32_t drum = r.u16();
			e.drumHits = static_cast<uint8_t>(drum & 0xFFu);
			e.drumAccent = static_cast<uint8_t>(drum >> 8);
			const uint32_t orig = r.u16();
			e.origKind = static_cast<pattern::PatternStep::Kind>(std::min<uint32_t>(orig & 3u, 2u));
			e.origHits = static_cast<uint8_t>(orig >> 8);
			e.origDegree = static_cast<int16_t>(static_cast<int>(r.u16()) - 512);
			e.origOctave = static_cast<int16_t>(static_cast<int>(r.u16()) - 512);
			if (v4) {
				for (float& level : e.cellLevel) {
					level = static_cast<float>(r.u16()) / 65535.0f;
				}
				e.cellSet = static_cast<uint8_t>(r.u16() & 0xFFu);
			}
		}
	}
	return r.ok() ? read : out;
}

void writeEditState(const EditState& state, float* out) noexcept {
	device::BlockWriter w(out, kEditsWords);
	for (const EditOverlay& o : state) {
		for (const float a : o.anchor) {
			w.f32(a);
		}
		w.u16(static_cast<uint32_t>(o.mode));
		for (const pattern::StepEdit& e : o.steps) {
			w.u16((e.on ? 1u : 0u) | (static_cast<uint32_t>(e.kind) << 1) | (e.accent ? 8u : 0u) | (e.slide ? 16u : 0u)
			      | (e.pitched ? 32u : 0u));
			w.u16(static_cast<uint32_t>(std::clamp<int>(e.key + 256, 0, 511)));
			w.f32(std::clamp(e.velocity, 0.0f, 1.0f));
			w.f32(std::clamp(e.gate, 0.0f, 64.0f));
			w.u16(static_cast<uint32_t>(e.drumHits) | (static_cast<uint32_t>(e.drumAccent) << 8));
			w.u16(static_cast<uint32_t>(e.origKind) | (static_cast<uint32_t>(e.origHits) << 8));
			w.u16(static_cast<uint32_t>(std::clamp<int>(e.origDegree + 512, 0, 1023)));
			w.u16(static_cast<uint32_t>(std::clamp<int>(e.origOctave + 512, 0, 1023)));
			for (const float level : e.cellLevel) {
				w.u16(static_cast<uint32_t>(std::lround(std::clamp(level, 0.0f, 1.0f) * 65535.0f)));
			}
			w.u16(e.cellSet);
		}
	}
}

std::array<float, kAnchorFields> anchorNow(size_t p, const std::function<float(size_t)>& fieldAt) noexcept {
	std::array<float, kAnchorFields> a {};
	for (size_t i = 0; i < kAnchorFields; ++i) {
		a[i] = fieldAt(at(p, kAnchorOf[i]));
	}
	return a;
}

bool editsStale(const EditOverlay& overlay, size_t p, const std::function<float(size_t)>& fieldAt) noexcept {
	if (!overlay.any()) {
		return false;
	}
	const auto mode = static_cast<long>(std::lround(fieldAt(at(p, Field::Mode))));
	return overlay.anchor != anchorNow(p, fieldAt) || mode != static_cast<long>(overlay.mode);
}

bool drumHides(size_t control, Mode mode) noexcept {
	if (control < kGlobalCount || control >= kParamCount) {
		return false;
	}
	const auto field = static_cast<Field>((control - kGlobalCount) % kFieldCount);
	// MODULATION'S TIME is read by Glide and Ramp (its declared gate) and only in Modulation (design §7.10).
	if (field == Field::ModTime) {
		return !genes::isModulation(mode);
	}
	if (!genes::decodesDrum(mode)) {
		return false;
	}
	// MODULATION PLAYS NO NOTES (design §7.10): what only shapes a note — its pass-through, its trigger — is not drawn.
	if (genes::isModulation(mode) && (field == Field::Thru || field == Field::Trig)) {
		return true;
	}
	switch (field) {
	case Field::Root:
	case Field::Scale:
	case Field::Degree:
	case Field::Size:
	case Field::Inversion:
	case Field::Spread:
	case Field::PitchOnNotes:
	case Field::GlideTime:
	case Field::TransposeSteps: // a lane is a fixed note number: nothing to move (design §7.7)
	case Field::TransposeOctaves: return true;
	default: return false;
	}
}

Row rowOf(const Settings& s) {
	Row row {};
	row[at(Global::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[at(Global::Selector)] = s.selector;
	row[at(Global::Keys)] = static_cast<float>(s.keys);
	row[at(Global::Latch)] = s.latch ? 1.0f : 0.0f;
	row[at(Global::HumanizeMode)] = static_cast<float>(s.feel.mode);
	row[at(Global::HumanizeAmount)] = static_cast<float>(s.feel.amount);
	row[at(Global::Seed)] = static_cast<float>(s.feel.seed);
	row[at(Global::FreePhase)] = static_cast<float>(s.freePhase);
	row[at(Global::Wobble)] = static_cast<float>(s.feel.wobble);
	row[at(Global::NoteLow)] = static_cast<float>(s.noteLow);
	row[at(Global::NoteHigh)] = static_cast<float>(s.noteHigh);
	row[at(Global::Offset)] = static_cast<float>(s.offset);
	row[at(Global::Nudge)] = s.nudge;
	row[at(Global::Run)] = static_cast<float>(s.run);
	row[at(Global::FixedVelocity)] = s.fixedVelocity ? 1.0f : 0.0f;
	row[at(Global::Velocity)] = s.velocity;
	row[at(Global::LikesWarp)] = s.likesWarp ? 1.0f : 0.0f;
	row[at(Global::PlayMode)] = static_cast<float>(s.playMode);
	row[at(Global::TransposeSteps)] = static_cast<float>(s.transposeSteps);
	row[at(Global::TransposeOctaves)] = static_cast<float>(s.transposeOctaves);
	for (size_t p = 0; p < kPatterns; ++p) {
		row[at(p, Field::Repeats)] = static_cast<float>(s.cycle[p].repeats);
		row[at(p, Field::Next)] = static_cast<float>(s.cycle[p].next);
		row[at(p, Field::TransposeSteps)] = static_cast<float>(s.patternTranspose[p][0]);
		row[at(p, Field::TransposeOctaves)] = static_cast<float>(s.patternTranspose[p][1]);
		{
			const auto bars = std::find(std::begin(kSpanBars), std::end(kSpanBars), s.phase[p].spanBars);
			row[at(p, Field::Span)] = static_cast<float>(bars == std::end(kSpanBars) ? 0 : bars - std::begin(kSpanBars));
		}
		row[at(p, Field::Fill)] = s.phase[p].fill ? 1.0f : 0.0f;
		row[at(p, Field::StartPhase)] = static_cast<float>(s.phase[p].start);
		row[at(p, Field::EndPhase)] = static_cast<float>(s.phase[p].end);
		row[at(p, Field::FillTrigger)] = s.fillTrigger[p];
		row[at(p, Field::ModSmooth)] = static_cast<float>(s.modLanes[p].smooth);
		row[at(p, Field::ModTime)] = s.modLanes[p].time;
		row[at(p, Field::ModPolarity)] = s.modLanes[p].bipolar ? 1.0f : 0.0f;
		row[at(p, Field::ModNeutral)] = s.modLanes[p].neutral ? 1.0f : 0.0f;
		const PatternSettings& q = s.patterns[p];
		row[at(p, Field::Enable)] = q.enable ? 1.0f : 0.0f;
		row[at(p, Field::Solo)] = q.solo ? 1.0f : 0.0f;
		row[at(p, Field::Mode)] = static_cast<float>(q.mode);
		row[at(p, Field::Root)] = static_cast<float>(q.root);
		row[at(p, Field::Scale)] = static_cast<float>(q.scale);
		row[at(p, Field::Degree)] = static_cast<float>(q.degree);
		row[at(p, Field::Size)] = static_cast<float>(q.size);
		row[at(p, Field::Inversion)] = static_cast<float>(q.inversion == theory::kAuto ? 0 : q.inversion + 1);
		row[at(p, Field::Spread)] = static_cast<float>(q.spread);
		row[at(p, Field::TimeBase)] = static_cast<float>(q.timeBase);
		row[at(p, Field::Rate)] = q.rate;
		row[at(p, Field::Length)] = static_cast<float>(q.length);
		row[at(p, Field::Gate)] = q.gate;
		row[at(p, Field::Accent)] = q.accent;
		row[at(p, Field::PitchOnNotes)] = q.pitchOnNotes ? 1.0f : 0.0f;
		row[at(p, Field::GlideTime)] = q.glideTime;
		row[at(p, Field::Thru)] = static_cast<float>(q.thru);
		row[at(p, Field::Trig)] = static_cast<float>(q.trig);
		row[at(p, Field::Zone)] = static_cast<float>(q.zone);
		row[at(p, Field::ZoneOffset)] = q.zoneOffset;
		row[at(p, Field::Gamma)] = static_cast<float>(q.gamma);
		row[at(p, Field::PlaceX)] = q.placeX;
		row[at(p, Field::PlaceY)] = q.placeY;
		row[at(p, Field::Rotate)] = q.rotate;
		for (size_t i = 0; i < genes::kPinCount; ++i) {
			row[pinAt(p, static_cast<genes::Pin>(i))] = q.pins[i];
		}
		for (size_t l = 0; l < pattern::kDrumLanes; ++l) {
			row[drumNoteAt(p, l)] = static_cast<float>(q.drum.note[l]);
			row[drumOnAt(p, l)] = q.drum.laneOn(l) ? 1.0f : 0.0f;
		}
	}
	return row;
}

Settings settingsOf(const float* fields, size_t present) {
	const auto value = [&](size_t i) { return valueOf(fields, present, i); };
	const auto whole = [&](size_t i) { return static_cast<int>(std::lround(value(i))); };
	Settings s;
	s.enabled = value(at(Global::Enabled)) >= 0.5f;
	s.selector = value(at(Global::Selector));
	s.keys = static_cast<Keys>(whole(at(Global::Keys)));
	s.latch = value(at(Global::Latch)) >= 0.5f;
	s.feel.mode = static_cast<humanize::Mode>(whole(at(Global::HumanizeMode)));
	s.feel.amount = static_cast<double>(value(at(Global::HumanizeAmount)));
	s.feel.seed = whole(at(Global::Seed));
	// FREE'S PHASE (design §12-bis): unbounded, read by the Free feels in place of Amount.
	s.freePhase = static_cast<double>(value(at(Global::FreePhase)));
	s.feel.freePhase = s.freePhase;
	s.feel.wobble = static_cast<double>(value(at(Global::Wobble)));
	s.noteLow = std::clamp(whole(at(Global::NoteLow)), 0, 127);
	s.noteHigh = std::clamp(whole(at(Global::NoteHigh)), 0, 127);
	s.offset = whole(at(Global::Offset));
	s.nudge = value(at(Global::Nudge));
	s.run = static_cast<arp::RunMode>(std::clamp(whole(at(Global::Run)), 0, 2));
	s.fixedVelocity = value(at(Global::FixedVelocity)) >= 0.5f;
	s.velocity = value(at(Global::Velocity));
	s.likesWarp = value(at(Global::LikesWarp)) >= 0.5f;
	s.playMode = whole(at(Global::PlayMode)) == 1 ? PlayMode::Cycle : PlayMode::Selector;
	s.transposeSteps = whole(at(Global::TransposeSteps));
	s.transposeOctaves = whole(at(Global::TransposeOctaves));
	for (size_t p = 0; p < kPatterns; ++p) {
		s.cycle[p].repeats = std::clamp(whole(at(p, Field::Repeats)), 1, kMaxRepeats);
		s.cycle[p].next = static_cast<NextAction>(std::clamp(whole(at(p, Field::Next)), 0, static_cast<int>(kNextCount) - 1));
		s.patternTranspose[p] = { whole(at(p, Field::TransposeSteps)), whole(at(p, Field::TransposeOctaves)) };
		s.phase[p].spanBars = kSpanBars[static_cast<size_t>(std::clamp(whole(at(p, Field::Span)), 0, static_cast<int>(kSpanCount) - 1))];
		s.phase[p].fill = value(at(p, Field::Fill)) >= 0.5f;
		s.phase[p].start = whole(at(p, Field::StartPhase));
		s.phase[p].end = whole(at(p, Field::EndPhase));
		s.fillTrigger[p] = value(at(p, Field::FillTrigger));
		s.modLanes[p].smooth = static_cast<ModSmooth>(std::clamp(whole(at(p, Field::ModSmooth)), 0, static_cast<int>(kModSmoothCount) - 1));
		s.modLanes[p].time = value(at(p, Field::ModTime));
		s.modLanes[p].bipolar = value(at(p, Field::ModPolarity)) >= 0.5f;
		s.modLanes[p].neutral = value(at(p, Field::ModNeutral)) >= 0.5f;
		PatternSettings& q = s.patterns[p];
		q.enable = value(at(p, Field::Enable)) >= 0.5f;
		q.solo = value(at(p, Field::Solo)) >= 0.5f;
		q.mode = static_cast<Mode>(whole(at(p, Field::Mode)));
		q.root = whole(at(p, Field::Root));
		q.scale = static_cast<size_t>(whole(at(p, Field::Scale)));
		q.degree = whole(at(p, Field::Degree));
		q.size = static_cast<theory::ChordSize>(whole(at(p, Field::Size)));
		const int inversion = whole(at(p, Field::Inversion));
		q.inversion = inversion == 0 ? theory::kAuto : inversion - 1;
		q.spread = static_cast<theory::Spread>(whole(at(p, Field::Spread)));
		q.timeBase = static_cast<size_t>(whole(at(p, Field::TimeBase)));
		q.rate = value(at(p, Field::Rate));
		q.length = static_cast<size_t>(whole(at(p, Field::Length)));
		q.gate = value(at(p, Field::Gate));
		q.accent = value(at(p, Field::Accent));
		q.pitchOnNotes = value(at(p, Field::PitchOnNotes)) >= 0.5f;
		q.glideTime = value(at(p, Field::GlideTime));
		q.thru = static_cast<Thru>(std::clamp(whole(at(p, Field::Thru)), 0, static_cast<int>(kThruCount) - 1));
		q.trig = static_cast<uint8_t>(std::clamp(whole(at(p, Field::Trig)), 0, static_cast<int>(kTrigCount) - 1));
		q.zone = whole(at(p, Field::Zone));
		q.zoneOffset = value(at(p, Field::ZoneOffset));
		// GAMMA IS READ AS THE FIELD HOLDS IT — a float, as every house gamma is in a row.
		q.gamma = static_cast<double>(value(at(p, Field::Gamma)));
		q.placeX = value(at(p, Field::PlaceX));
		q.placeY = value(at(p, Field::PlaceY));
		q.rotate = value(at(p, Field::Rotate));
		for (size_t i = 0; i < genes::kPinCount; ++i) {
			q.pins[i] = value(pinAt(p, static_cast<genes::Pin>(i)));
		}
		q.drum.on = 0;
		for (size_t l = 0; l < pattern::kDrumLanes; ++l) {
			q.drum.note[l] = static_cast<int16_t>(std::clamp(std::lround(value(drumNoteAt(p, l))), 0L, 127L));
			if (value(drumOnAt(p, l)) >= 0.5f) {
				q.drum.on = static_cast<uint8_t>(q.drum.on | (1u << l));
			}
		}
	}
	return s;
}

size_t inRange(const Settings& s, std::array<size_t, kPatterns>& out) noexcept {
	bool anySolo = false;
	for (const PatternSettings& q : s.patterns) {
		anySolo = anySolo || q.solo;
	}
	size_t n = 0;
	for (size_t p = 0; p < kPatterns; ++p) {
		const PatternSettings& q = s.patterns[p];
		if (anySolo ? q.solo : q.enable) {
			out[n++] = p;
		}
	}
	return n;
}

size_t selected(const Settings& s) noexcept {
	std::array<size_t, kPatterns> range {};
	const size_t n = inRange(s, range);
	if (n == 0) {
		return kPatterns;
	}
	const auto band = static_cast<size_t>(std::floor(std::clamp(s.selector, 0.0f, 1.0f) * static_cast<float>(n)));
	return range[std::min(band, n - 1)];
}

// THE DEFAULTS AGREE: a default-constructed Settings is what an empty row decodes to.
static_assert(PatternSettings {}.length == 16);

} // namespace feathers::mod::arp::pat

namespace feathers::mod::arp::pat {

int transposeKey(const theory::Scale& scale, int rootKey, int key, int steps, int octaves) noexcept {
	if (steps == 0 && octaves == 0) {
		return key;
	}
	int moved = key;
	if (steps != 0 && scale.count > 0) {
		// THE DEGREE AT OR BELOW THE KEY, and what the key stands above it (0 for a key in the scale).
		const int degree = theory::degreeAtOrBelow(scale, rootKey, key);
		const int off = key - theory::keyOfDegree(scale, rootKey, degree);
		moved = theory::keyOfDegree(scale, rootKey, degree + steps) + off;
	}
	return std::clamp(moved + 12 * octaves, 0, 127);
}

bool transposeLive(const std::function<float(size_t)>& fieldAt) noexcept {
	bool anySolo = false;
	for (size_t p = 0; p < kPatterns; ++p) {
		anySolo = anySolo || fieldAt(at(p, Field::Solo)) >= 0.5f;
	}
	size_t in = 0;
	for (size_t p = 0; p < kPatterns; ++p) {
		if (!(anySolo ? fieldAt(at(p, Field::Solo)) >= 0.5f : fieldAt(at(p, Field::Enable)) >= 0.5f)) {
			continue;
		}
		++in;
		if (!genes::decodesDrum(static_cast<Mode>(std::clamp<long>(std::lround(fieldAt(at(p, Field::Mode))), 0, static_cast<long>(kModeChoices) - 1)))) {
			return true; // a pitched pattern in range: the transpose moves what it plays
		}
	}
	return in == 0; // nothing in range: nothing plays either way, so the controls stay where the hand expects them
}

int foldToRange(int key, int low, int high) noexcept {
	low = std::clamp(low, 0, 127);
	high = std::clamp(std::max(high, low), 0, 127); // Lowest wins over a Highest below it
	int k = key;
	while (k < low) {
		k += 12;
	}
	while (k > high) {
		k -= 12;
	}
	if (k < low) {
		// NO OCTAVE OF IT FITS (a range under an octave): the nearer bound.
		k = (low - k) <= (k + 12 - high) ? low : high;
	}
	return std::clamp(k, 0, 127);
}

bool noteRangeLive(const std::function<float(size_t)>& fieldAt) noexcept {
	bool anySolo = false;
	for (size_t p = 0; p < kPatterns; ++p) {
		anySolo = anySolo || fieldAt(at(p, Field::Solo)) >= 0.5f;
	}
	size_t in = 0;
	for (size_t p = 0; p < kPatterns; ++p) {
		if (!(anySolo ? fieldAt(at(p, Field::Solo)) >= 0.5f : fieldAt(at(p, Field::Enable)) >= 0.5f)) {
			continue;
		}
		++in;
		const auto m = static_cast<Mode>(std::clamp<long>(std::lround(fieldAt(at(p, Field::Mode))), 0, static_cast<long>(kModeChoices) - 1));
		if (!genes::decodesDrum(m) && !genes::isModulation(m)) {
			return true; // a pitched pattern in range: the range folds what it plays
		}
	}
	return in == 0;
}

} // namespace feathers::mod::arp::pat
