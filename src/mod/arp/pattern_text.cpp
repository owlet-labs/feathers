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
#include "mod/arp/pattern_types.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// THE PATTERN DEVICE'S WORDS IN THE DAW. Pure: they read the value and the view
// and allocate nothing. A control they have nothing to say about is handed back to its declared unit.

namespace feathers::mod::arp::pat {

namespace {

/// THE ROW THE VIEW SEES, decoded.
Settings settingsOfView(const device::TextView& view) {
	std::array<float, kParamCount> f {};
	for (size_t i = 0; i < kParamCount; ++i) {
		f[i] = view.field != nullptr ? view.field(view.host, i) : static_cast<float>(kParamDescs[i].def);
	}
	return settingsOf(f.data(), f.size());
}

/// WHICH PATTERN AND WHICH OF ITS CONTROLS a control index is; false for a global.
bool fieldOf(size_t control, size_t* pattern, Field* field) {
	if (control < kGlobalCount || control >= kParamCount) {
		return false;
	}
	const size_t rel = control - kGlobalCount;
	*pattern = rel / kFieldCount;
	*field = static_cast<Field>(rel % kFieldCount);
	return true;
}

int whole(double v) {
	return static_cast<int>(std::lround(v));
}

bool write(char* out, size_t size, const char* text) {
	if (out == nullptr || size == 0) {
		return false;
	}
	std::snprintf(out, size, "%s", text);
	return true;
}

/// Lower-cased, trimmed copy of `text` into `buf`.
void normalise(const char* text, char* buf, size_t size) {
	size_t n = 0;
	const char* p = text;
	while (*p == ' ') {
		++p;
	}
	for (; *p != '\0' && n + 1 < size; ++p) {
		buf[n++] = static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
	}
	while (n > 0 && buf[n - 1] == ' ') {
		--n;
	}
	buf[n] = '\0';
}

constexpr const char* kRoman[] = { "I", "II", "III", "IV", "V", "VI", "VII" };
constexpr const char* kRomanLower[] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };

/// **A DEGREE AS A NUMERAL CASED BY ITS CHORD'S QUALITY IN THIS SCALE** — "iv", "V7", "vii°", "iiø7", "IIImaj7".
void numeral(const PatternSettings& q, int degree, char* out, size_t size) {
	const theory::Scale s = theory::scaleOf(q.scale);
	const int d = ((degree % s.count) + s.count) % s.count;
	const auto at = [&](int step) { return theory::keyOfDegree(s, 0, degree + step) - theory::keyOfDegree(s, 0, degree); };
	const int third = at(2);
	const int fifth = at(4);
	const bool tertian = q.size == theory::ChordSize::Triad || q.size == theory::ChordSize::Sixth
	                     || q.size == theory::ChordSize::Seventh || q.size == theory::ChordSize::Add9
	                     || q.size == theory::ChordSize::Ninth || q.size == theory::ChordSize::Eleventh
	                     || q.size == theory::ChordSize::Thirteenth;
	const bool minor = tertian && third == 3;
	const char* base = minor ? kRomanLower[d] : kRoman[d];
	const char* quality = "";
	if (tertian && third == 3 && fifth == 6) {
		quality = "°";
	}
	else if (tertian && third == 4 && fifth == 8) {
		quality = "+";
	}
	const char* suffix = "";
	const bool seventh = q.size == theory::ChordSize::Seventh || q.size == theory::ChordSize::Ninth
	                     || q.size == theory::ChordSize::Eleventh || q.size == theory::ChordSize::Thirteenth;
	if (seventh) {
		const int s7 = at(6);
		if (third == 3 && fifth == 6) {
			quality = "";
			suffix = s7 == 10 ? "ø7" : "°7";
		}
		else {
			suffix = (third == 4 && s7 == 11) ? "maj7" : "7";
		}
	}
	else if (q.size == theory::ChordSize::Power) {
		suffix = "5";
	}
	else if (q.size == theory::ChordSize::Sus2) {
		suffix = "sus2";
	}
	else if (q.size == theory::ChordSize::Sus4) {
		suffix = "sus4";
	}
	std::snprintf(out, size, "%s%s%s", base, quality, suffix);
}

constexpr const char* kOrdinal[] = { "root", "1st", "2nd", "3rd" };

/// INVERSION `index` (0 Auto, 1 Root, 2..4 the 1st..3rd) of a chord of this Size, naming what plays when the inversion
/// is past the chord's tones: "3rd → root (Triad)".
void inversionText(const PatternSettings& q, int index, char* out, size_t size) {
	if (index <= 0) {
		write(out, size, "Auto");
		return;
	}
	if (index == 1) {
		write(out, size, "Root");
		return;
	}
	std::array<int, theory::kMaxTones> degrees {};
	const auto tones = static_cast<int>(theory::chordDegrees(q.size, 0, degrees.data()));
	const int inv = index - 1;
	if (tones > 0 && inv >= tones) {
		std::snprintf(out, size, "%s → %s (%s)", kOrdinal[inv], kOrdinal[inv % tones],
		              theory::kChordSizeNames[static_cast<size_t>(q.size)]);
		return;
	}
	std::snprintf(out, size, "%s inv", kOrdinal[inv]);
}

/// **A PROGRESSION, IN NUMERALS** (design §11-quater): "I → V → vi → IV", decoded as the window's lane decodes it — over a
/// held C4 (Pool: its Root), the switches a fresh pattern has, no banked Rotate residue (the view carries neither the hand's switches
/// nor its residue; a pattern Rotate has folded reads the place without the fold).
void progressionWords(const Settings& s, size_t p, char* out, size_t size) {
	std::array<bool, genes::kPinCount> on {};
	on = genes::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	const pattern::Recipe recipe = recipeFor(s, p, on);
	HeldChord held;
	held.press(60, 0, 0.8f, 1);
	pattern::PatternCycle cycle;
	pattern::build(PatternDevice::placeOf(s.patterns[p], SliceResidue<genes::kRowCount> {}), recipe, held, cycle);
	size_t used = 0;
	int shown = 0;
	out[0] = '\0';
	for (size_t k = 0; k < cycle.length && used + 1 < size; ++k) {
		if (!cycle.steps[k].chordStart) {
			continue;
		}
		if (shown == 6) {
			used += static_cast<size_t>(std::snprintf(out + used, size - used, " → ..."));
			break;
		}
		char numeral[16];
		pattern::numeralOf(cycle.steps[k], numeral, sizeof numeral);
		used += static_cast<size_t>(std::snprintf(out + used, size - used, "%s%s", shown == 0 ? "" : " → ", numeral));
		++shown;
		used = std::min(used, size - 1);
	}
}

/// WHAT A PATTERN IS, in a few words: "D Dorian" (the Root only under Pool — Transpose takes it from the key), and in
/// Chords its degree and inversion: "C Minor, iv, 1st inv".
void describe(const Settings& s, const PatternSettings& q, char* out, size_t size) {
	char root[8] = "";
	if (s.keys == Keys::Pool) {
		std::snprintf(root, sizeof root, "%s ", kRootNames[static_cast<size_t>(((q.root % 12) + 12) % 12)]);
	}
	const char* scale = theory::kScales[std::min(q.scale, theory::kScaleCount - 1)].name;
	if (q.mode == Mode::Progression) {
		char words[96];
		progressionWords(s, static_cast<size_t>(&q - s.patterns.data()), words, sizeof words);
		std::snprintf(out, size, "%s%s, %s", root, scale, words);
		return;
	}
	if (q.mode != Mode::Chords) {
		std::snprintf(out, size, "%s%s", root, scale);
		return;
	}
	char deg[16];
	numeral(q, q.degree, deg, sizeof deg);
	char inv[48];
	inversionText(q, q.inversion == theory::kAuto ? 0 : q.inversion + 1, inv, sizeof inv);
	std::snprintf(out, size, "%s%s, %s, %s", root, scale, deg, inv);
}

/// TRANSPOSE'S WORDS (design §7.7): "+2 steps", "−1 step", "0 steps"; "+1 oct", "−2 oct". A true minus sign.
void transposeText(int n, bool octaves, char* out, size_t size) {
	const char* sign = n > 0 ? "+" : (n < 0 ? "−" : "");
	const int mag = n < 0 ? -n : n;
	if (octaves) {
		std::snprintf(out, size, "%s%d oct", sign, mag);
	}
	else {
		std::snprintf(out, size, "%s%d step%s", sign, mag, mag == 1 ? "" : "s");
	}
}

/// "+2", "-2", "−2", "2 steps", "−1 oct", "+3 st": a signed whole number, then optionally a word naming the unit.
bool transposeOfText(const char* text, bool octaves, double* value) {
	char buf[48];
	normalise(text, buf, sizeof buf);
	const char* p = buf;
	int sign = 1;
	if (*p == '+') {
		++p;
	}
	else if (*p == '-') {
		sign = -1;
		++p;
	}
	else if (std::strncmp(p, "−", std::strlen("−")) == 0) {
		sign = -1;
		p += std::strlen("−");
	}
	if (!std::isdigit(static_cast<unsigned char>(*p))) {
		return false;
	}
	char* end = nullptr;
	const long n = std::strtol(p, &end, 10);
	while (*end == ' ') {
		++end;
	}
	if (*end != '\0') {
		const bool word = octaves ? std::strncmp(end, "oct", 3) == 0 : std::strncmp(end, "st", 2) == 0;
		if (!word) {
			return false;
		}
	}
	const double reach = octaves ? kTransposeOctavesReach : kTransposeStepsReach;
	*value = std::clamp(static_cast<double>(sign * n), -reach, reach);
	return true;
}

bool formatText(size_t control, double value, const device::TextView& view, char* out, size_t size) {
	if (control == at(Global::TransposeSteps) || control == at(Global::TransposeOctaves)) {
		if (out == nullptr || size == 0) {
			return false;
		}
		transposeText(whole(value), control == at(Global::TransposeOctaves), out, size);
		return true;
	}
	Settings s = settingsOfView(view);
	if (control == at(Global::Selector)) {
		s.selector = static_cast<float>(value);
		const size_t p = selected(s);
		if (p >= kPatterns) {
			return write(out, size, "\u2014 (none enabled)"); // decided 2026-10-30
		}
		bool solo = false;
		for (const PatternSettings& q : s.patterns) {
			solo = solo || q.solo;
		}
		char what[160];
		describe(s, s.patterns[p], what, sizeof what);
		std::snprintf(out, size, "%zu · %s%s", p + 1, what, solo ? " (solo)" : "");
		return true;
	}
	size_t p = 0;
	Field f {};
	if (!fieldOf(control, &p, &f)) {
		return false;
	}
	PatternSettings& q = s.patterns[p];
	switch (f) {
	case Field::Root: return write(out, size, kRootNames[static_cast<size_t>(std::clamp(whole(value), 0, 11))]);
	case Field::Scale:
		return write(out, size, theory::kScales[static_cast<size_t>(std::clamp(whole(value), 0, static_cast<int>(theory::kScaleCount) - 1))].name);
	case Field::Mode: return write(out, size, kModeNames[static_cast<size_t>(std::clamp(whole(value), 0, static_cast<int>(kModeChoices) - 1))]);
	case Field::Degree: {
		const int d = std::clamp(whole(value), 0, 6);
		if (q.mode != Mode::Chords) {
			std::snprintf(out, size, "+%d degree%s", d, d == 1 ? "" : "s");
			return true;
		}
		numeral(q, d, out, size);
		return true;
	}
	case Field::Inversion: inversionText(q, std::clamp(whole(value), 0, 4), out, size); return true;
	// THRU AND TRIG: Auto says what it resolves to here — "Auto (On)", "Auto (Transport)" (design §7.5).
	case Field::Thru: {
		q.thru = static_cast<Thru>(std::clamp(whole(value), 0, static_cast<int>(kThruCount) - 1));
		if (q.thru != Thru::Auto) {
			return write(out, size, kThruNames[static_cast<size_t>(q.thru)]);
		}
		std::snprintf(out, size, "Auto (%s)", thruOn(q) ? "On" : "Off");
		return true;
	}
	case Field::Trig: {
		q.trig = static_cast<uint8_t>(std::clamp(whole(value), 0, static_cast<int>(kTrigCount) - 1));
		if (q.trig != 0) {
			return write(out, size, kTrigNames[q.trig]);
		}
		std::snprintf(out, size, "Auto (%s)", kTrigNames[static_cast<size_t>(trigOf(q)) + 1]);
		return true;
	}
	case Field::TransposeSteps:
	case Field::TransposeOctaves: transposeText(whole(value), f == Field::TransposeOctaves, out, size); return true;
	// CYCLE'S REPEATS (design §7.6): "3×" — how many times the pattern plays through.
	case Field::Repeats: {
		std::snprintf(out, size, "%d×", std::clamp(whole(value), 1, kMaxRepeats));
		return true;
	}
	case Field::GlideTime: {
		// IN MILLISECONDS, and the 303's own time named.
		const double ms = 1000.0 * std::exp2(value);
		const bool tb = std::abs(value - kGlideTimeDef) < 1e-4;
		std::snprintf(out, size, ms < 10.0 ? "%.1f ms%s" : "%.0f ms%s", ms, tb ? " (TB)" : "");
		return true;
	}
	// MODULATION'S SMOOTH TIME (design §7.10), in steps.
	case Field::ModTime: {
		std::snprintf(out, size, "%.2f steps", std::clamp(static_cast<double>(value), 0.0, kModTimeMax));
		return true;
	}
	default: return false;
	}
}

/// A ROMAN NUMERAL I..VII (any case, any quality marks after it), or a number 1..7; the degree 0..6, or −1.
int degreeOfText(const char* text) {
	char buf[32];
	normalise(text, buf, sizeof buf);
	const char* p = buf;
	if (*p == '+') {
		++p;
		return std::isdigit(static_cast<unsigned char>(*p)) ? std::clamp(std::atoi(p), 0, 6) : -1;
	}
	if (std::isdigit(static_cast<unsigned char>(*p))) {
		const int n = std::atoi(p);
		return n >= 1 && n <= 7 ? n - 1 : -1;
	}
	size_t n = 0;
	while (p[n] == 'i' || p[n] == 'v') {
		++n;
	}
	for (int d = 6; d >= 0; --d) {
		if (std::strlen(kRomanLower[d]) == n && std::strncmp(p, kRomanLower[d], n) == 0) {
			return d;
		}
	}
	return -1;
}

int rootOfText(const char* text) {
	char buf[16];
	normalise(text, buf, sizeof buf);
	static constexpr const char* kLetters = "c d ef g a b";
	if (buf[0] == '\0') {
		return -1;
	}
	const char* at = std::strchr(kLetters, buf[0]);
	if (at == nullptr || buf[0] == ' ') {
		return -1;
	}
	int pc = static_cast<int>(at - kLetters);
	const char* rest = buf + 1;
	if (std::strcmp(rest, "#") == 0 || std::strcmp(rest, "♯") == 0 || std::strcmp(rest, "sharp") == 0) {
		++pc;
	}
	else if (std::strcmp(rest, "b") == 0 || std::strcmp(rest, "♭") == 0 || std::strcmp(rest, "flat") == 0) {
		--pc;
	}
	else if (*rest != '\0') {
		return -1;
	}
	return ((pc % 12) + 12) % 12;
}

/// A NAME FROM A LIST, case-insensitive: exact, or a prefix only one name has.
int nameOfText(const char* text, const char* const* names, size_t count) {
	char buf[48];
	normalise(text, buf, sizeof buf);
	const size_t n = std::strlen(buf);
	if (n == 0) {
		return -1;
	}
	int found = -1;
	for (size_t i = 0; i < count; ++i) {
		char name[48];
		normalise(names[i], name, sizeof name);
		if (std::strcmp(name, buf) == 0) {
			return static_cast<int>(i);
		}
		if (std::strncmp(name, buf, n) == 0) {
			if (found >= 0) {
				found = -2;
			}
			else if (found == -1) {
				found = static_cast<int>(i);
			}
		}
	}
	return found >= 0 ? found : -1;
}

bool parseText(size_t control, const char* text, const device::TextView& view, double* value) {
	if (text == nullptr || value == nullptr) {
		return false;
	}
	if (control == at(Global::TransposeSteps) || control == at(Global::TransposeOctaves)) {
		return transposeOfText(text, control == at(Global::TransposeOctaves), value);
	}
	const Settings s = settingsOfView(view);
	if (control == at(Global::Selector)) {
		// ITS OWN WORDS FOR WHERE IT STANDS read back as where it stands: a band holds many values under one name, so
		// typing back what the field shows must not move the selector to the band's centre.
		char shown[128];
		if (formatText(control, static_cast<double>(s.selector), view, shown, sizeof shown) && std::strcmp(shown, text) == 0) {
			*value = static_cast<double>(s.selector);
			return true;
		}
		// "2", "P2", "Pattern 2", or another pattern's words "2 · D Dorian": that pattern's band, if it is in range.
		char buf[64];
		normalise(text, buf, sizeof buf);
		const char* p = buf;
		if (std::strncmp(p, "pattern", 7) == 0) {
			p += 7;
		}
		else if (*p == 'p') {
			++p;
		}
		while (*p == ' ') {
			++p;
		}
		if (!std::isdigit(static_cast<unsigned char>(*p))) {
			return false;
		}
		const int want = std::atoi(p) - 1;
		std::array<size_t, kPatterns> range {};
		const size_t n = inRange(s, range);
		for (size_t i = 0; i < n; ++i) {
			if (static_cast<int>(range[i]) == want) {
				*value = (static_cast<double>(i) + 0.5) / static_cast<double>(n);
				return true;
			}
		}
		return false;
	}
	size_t pattern = 0;
	Field f {};
	if (!fieldOf(control, &pattern, &f)) {
		return false;
	}
	if (f == Field::TransposeSteps || f == Field::TransposeOctaves) {
		return transposeOfText(text, f == Field::TransposeOctaves, value);
	}
	if (f == Field::Repeats) {
		// "3", "3x", "3×", "3 ×": a whole number of times, and nothing else after it.
		char* end = nullptr;
		const long n = std::strtol(text, &end, 10);
		if (end == text) {
			return false;
		}
		while (*end == ' ') {
			++end;
		}
		if (*end == 'x' || *end == 'X') {
			++end;
		}
		else if (std::strncmp(end, "×", std::strlen("×")) == 0) {
			end += std::strlen("×");
		}
		while (*end == ' ') {
			++end;
		}
		if (*end != '\0') {
			return false;
		}
		*value = static_cast<double>(std::clamp<long>(n, 1, kMaxRepeats));
		return true;
	}
	if (f == Field::GlideTime) {
		// "TB" is the 303's time; otherwise a number, in ms unless it says s ("0.1 s", "100", "100 ms", "22 ms (TB)").
		// ITS OWN WORDS FOR WHERE IT STANDS read back as where it stands (the readout rounds to the millisecond).
		if (view.field != nullptr) {
			const double now = static_cast<double>(view.field(view.host, control));
			char shown[32];
			if (formatText(control, now, view, shown, sizeof shown) && std::strcmp(shown, text) == 0) {
				*value = now;
				return true;
			}
		}
		char buf[32];
		normalise(text, buf, sizeof buf);
		if (std::strcmp(buf, "tb") == 0) {
			*value = kGlideTimeDef;
			return true;
		}
		char* end = nullptr;
		const double n = std::strtod(buf, &end);
		if (end == buf || !(n > 0.0)) {
			return false;
		}
		while (*end == ' ') {
			++end;
		}
		const double seconds = (end[0] == 's') ? n : n / 1000.0;
		*value = std::clamp(std::log2(seconds), kGlideTimeMin, kGlideTimeMax);
		return true;
	}
	int v = -1;
	switch (f) {
	case Field::Root: v = rootOfText(text); break;
	case Field::Scale: {
		v = nameOfText(text, kScaleNames.data(), kScaleNames.size());
		break;
	}
	case Field::Mode: v = nameOfText(text, kModeNames, kModeChoices); break;
	case Field::Thru:
	case Field::Trig: {
		// "Auto (On)" READS BACK AS AUTO: the words before the parenthesis name the choice.
		char head[48];
		size_t n = 0;
		for (; text[n] != '\0' && text[n] != '(' && n + 1 < sizeof head; ++n) {
			head[n] = text[n];
		}
		head[n] = '\0';
		v = f == Field::Thru ? nameOfText(head, kThruNames, kThruCount) : nameOfText(head, kTrigNames, kTrigCount);
		break;
	}
	case Field::Degree: v = degreeOfText(text); break;
	case Field::Inversion: {
		char buf[32];
		normalise(text, buf, sizeof buf);
		if (std::strncmp(buf, "auto", 4) == 0) {
			v = 0;
		}
		else if (std::strncmp(buf, "root", 4) == 0) {
			v = 1;
		}
		else {
			for (int i = 1; i < 4; ++i) {
				if (std::strncmp(buf, kOrdinal[i], 3) == 0) {
					v = i + 1;
				}
			}
			if (v < 0 && std::isdigit(static_cast<unsigned char>(buf[0])) && buf[1] == '\0') {
				v = std::clamp(buf[0] - '0', 0, 3) + 1;
			}
		}
		break;
	}
	default: return false;
	}
	if (v < 0) {
		return false;
	}
	*value = static_cast<double>(v);
	return true;
}

} // namespace

const device::TextOps kTextOps { .format = &formatText, .parse = &parseText };

} // namespace feathers::mod::arp::pat
