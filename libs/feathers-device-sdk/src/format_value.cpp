/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#include "device/format_value.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

// MOVED HERE FROM `plugin/ui/value_box.cpp` (host slots T3), text and all — the comments below were
// written about a value box and are still true of every surface that prints through this, a DAW's
// automation lane included. `tests/test_format_value.cpp` pins the move against strings captured from
// the UI build before it.

namespace feathers::device {

namespace {
/// `Unit::Degrees`: the stored radians to the degrees a hand reads, and back.
constexpr double kDegreesPerRadian = 180.0 / 3.14159265358979323846;
} // namespace

/// Seconds, printed the way a musician reads them — and the precision follows the MAGNITUDE rather
/// than being one format for the whole range. 0.5 ms, 10.9 ms and 841 ms all want a different number
/// of digits, and a single "%.1f" would print "841.0" in five characters this box does not have.
std::string printSeconds(double seconds) {
	char out[32] = {};
	if (seconds >= 1.0) {
		std::snprintf(out, sizeof(out), "%.2gs", seconds);
	}
	else if (seconds >= 0.01) {
		// 10 ms and up: whole milliseconds. Bitwig prints 841 ms, not 841.3.
		std::snprintf(out, sizeof(out), "%.0fms", seconds * 1000.0);
	}
	else {
		// Under 10 ms one decimal is the difference between 1.2 and 9.8, which is most of the useful
		// range of an attack.
		std::snprintf(out, sizeof(out), "%.1fms", seconds * 1000.0);
	}
	return out;
}

std::string printDegrees(double radians) {
	// WHOLE DEGREES, SIGNED: a turn either way from zero. AS STORED, NOT FOLDED — an angle wound past a turn
	// reads the winding, which is what its type-in gives back; a control that folds (Explore's Theta) stores
	// the fold. Rounded first, so a hair under zero reads "+0", not "-0".
	char out[32] = {};
	const double whole = std::round(radians * kDegreesPerRadian);
	std::snprintf(out, sizeof(out), "%+.0f deg", whole == 0.0 ? 0.0 : whole);
	return out;
}

std::string formatValue(const ParamDesc& desc, double value) {
	char out[32] = {};
	switch (desc.unit) {
	case Unit::LogSeconds:
		// THE BOTTOM OF A LogSeconds RANGE MEANS NO TIME AT ALL — device.h states the convention, and
		// this is the host end of it. "0.5ms" would be a card promising a stage the device is about
		// to skip, which is the same class of lie as a unit that describes something other than the
		// value.
		if (value <= desc.min) {
			return "off";
		}
		return printSeconds(std::exp2(value));
	case Unit::LogMultiple: {
		// **NO "off" AT THE BOTTOM, unlike `LogSeconds` beside it.** That convention exists because a
		// logarithmic DURATION has no zero to mean "no time" with, and a device may skip the stage.
		// A multiple of zero is not a thing this control can be asked for, and `windowSeconds` freezes
		// the shape on a non-positive SCALE rather than on a small one — so the bottom of this range is
		// a very slow shape, not an absent one, and printing "off" would promise a stop that does not
		// happen.
		const double times = std::exp2(value);
		// FOUR SIGNIFICANT FIGURES, so the ratios a hand actually lands on read as themselves — "2X",
		// "0.5X", "0.25X" — rather than as 2.00X and 0.25X with trailing noise.
		std::snprintf(out, sizeof(out), "%.4gX", times);
		return out;
	}
	case Unit::LogHertz: {
		if (atOffEnd(desc, value)) {
			return "off"; // its open end is an exact bypass — see `ParamDesc::offAtEnd`
		}
		const double hz = std::exp2(value);
		if (hz >= 1000.0) {
			std::snprintf(out, sizeof(out), "%.2gk", hz / 1000.0);
		}
		else if (hz < 9.95) {
			// BELOW 10 Hz, TWO SIGNIFICANT DIGITS: the filter's knob reaches 0.5 Hz (Z8), which "%.0f" printed as "0".
			std::snprintf(out, sizeof(out), "%.2g", hz);
		}
		else {
			std::snprintf(out, sizeof(out), "%.0f", hz);
		}
		return out;
	}
	case Unit::Percent:
		// SIGNED WHEN THE RANGE IS, unsigned when it is not. A curve runs -1..1 and its sign is the
		// whole of what it says, so "+42%" reads as a direction; a sustain runs 0..1 and a "+" in
		// front of it would be noise.
		std::snprintf(out, sizeof(out), desc.min < 0.0 ? "%+.0f%%" : "%.0f%%", value * 100.0);
		return out;
	case Unit::Choice: {
		// RELATIVE TO THE DECLARED MINIMUM, not the raw value.
		//
		// THIS WAS `lround(value)` AND WAS CORRECT ONLY BECAUSE EVERY Choice PARAM HAPPENED TO START
		// AT ZERO. OWLTT's Mode is the first that does not — it runs 1..10 over ten names, so the raw
		// value read one name too far: mode 1 showed "Quirky" instead of "AP 6dB", the default 6
		// showed "LR4 Fast" instead of "LR2 Full", and mode 10 ran off the end and printed a bare
		// "10". Every mode named the wrong crossover and the last one named nothing.
		//
		// The subtraction changes nothing for any existing device, because `min` is 0 for all of
		// them — which is exactly why nothing caught it. Correct-at-N=1 again, in the lookup rather
		// than in a device.
		const auto index = static_cast<size_t>(std::lround(value - desc.min));
		if (index < desc.names.size() && desc.names[index] != nullptr) {
			return desc.names[index];
		}
		std::snprintf(out, sizeof(out), "%ld", std::lround(value));
		return out;
	}
	case Unit::Note: {
		// **THE REFERENCE'S NAMING, ARITHMETIC INCLUDED** — see `device::Unit::Note`. `octave = n/12 - 2`
		// puts 60 at C3, and the note within the octave is taken with the +120 offset the firmware
		// uses so a negative code cannot index backwards out of the table.
		static const char* const kLetters[] = { "C",  "C#", "D",  "D#", "E",  "F",
			                                    "F#", "G",  "G#", "A",  "A#", "B" };
		const long note = std::lround(value);
		const long within = ((note % 12) + 12) % 12;
		const long octave = static_cast<long>(std::floor(static_cast<double>(note) / 12.0)) - 2;
		std::snprintf(out, sizeof(out), "%s%ld", kLetters[static_cast<size_t>(within)], octave);
		return out;
	}
	case Unit::Decibel:
		// ALWAYS SIGNED, WHERE `Percent` ABOVE TAKES ITS SIGN FROM THE RANGE. A percentage that runs
		// 0..1 has no direction to report and a "+" in front of it is noise; a dB reading is a
		// comparison to unity whatever the declared range, so "6dB" without a sign does not say which
		// way the knob went. One decimal because gain is the one place a tenth is audible sooner than
		// it is legible — +6.0 and +6.5 are different mixes.
		std::snprintf(out, sizeof(out), "%+.1fdB", value);
		return out;
	case Unit::Fader:
		// **THE VALUE IS A POSITION AND THE READOUT IS DECIBELS**, which is the whole point of the
		// unit: `gain = (2v)^2`, so 0.5 prints +0.0dB and 0 prints the silence a dB scale cannot
		// otherwise say. `-inf` rather than a large negative number, because the bottom of this
		// control IS silence and "-96.0dB" would be a figure describing something else.
		if (value <= 0.0) {
			return "-inf";
		}
		std::snprintf(out, sizeof(out), "%+.1fdB", 40.0 * std::log10(2.0 * value));
		return out;
	case Unit::Degrees:
		return printDegrees(value);
	case Unit::Number:
		if (atOffEnd(desc, value)) {
			return "off"; // a count whose bottom is the feature switched off — `ParamDesc::offAtEnd` (S&H's Quantize)
		}
		// **A STEPPED NUMBER IS A COUNT, AND A COUNT HAS NO DECIMALS.** The delay's `Taps` read
		// "4.00" — two digits of precision the control cannot have, since the host quantises it.
		// HARM's Harmonic, the LFO's Seed and the MSEG's segment counts are the same kind of thing
		// and printed the same way. Rounded rather than truncated, so a value a ramp has left at
		// 3.6 reads as the 4 every reader of a stepped control takes it for.
		// A CONTINUOUS ONE THAT ASKS (`ParamDesc::wholeReadout`) prints the same way.
		if (desc.stepped || desc.wholeReadout) {
			std::snprintf(out, sizeof(out), "%.0f", std::round(value));
			return out;
		}
		break;
	}
	std::snprintf(out, sizeof(out), "%.2f", value);
	return out;
}

double toNormal(const ParamDesc& desc, double value) {
	const double span = desc.max - desc.min;
	if (!(span > 0.0) || !std::isfinite(value)) {
		return 0.0;
	}
	return std::clamp((value - desc.min) / span, 0.0, 1.0);
}

double fromNormal(const ParamDesc& desc, double normal) {
	const double span = desc.max - desc.min;
	if (!(span > 0.0) || !std::isfinite(normal)) {
		return desc.min;
	}
	const double t = std::clamp(normal, 0.0, 1.0);
	if (desc.stepped) {
		// COUNTED FROM `min`, so the steps are the ones `formatValue`'s Choice lookup and every
		// device's decode count — `min + index` — whatever `min` is.
		return std::min(desc.min + std::round(t * span), desc.max);
	}
	// `std::lerp` RATHER THAN `min + t * span`, which is the same line everywhere but the ends: MEASURED,
	// three registry controls (a 3.3219 ceiling over -11) came back from 1.0 an ulp short of `max`, so a
	// host's write of the top of the sweep would not have been the top of the control. `lerp` is exact
	// at 0 and 1 by its contract.
	return std::lerp(desc.min, desc.max, t);
}





//---------//
// Type-in //
//---------//

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }

std::string_view trimmed(std::string_view s) {
	while (!s.empty() && isBlank(s.front())) {
		s.remove_prefix(1);
	}
	while (!s.empty() && isBlank(s.back())) {
		s.remove_suffix(1);
	}
	return s;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameWord(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (lower(a[i]) != lower(b[i])) {
			return false;
		}
	}
	return true;
}

/// **A PLAIN DECIMAL AND NOTHING ELSE** — an optional sign, digits, at most one point. No exponent, no
/// hex, no `inf`/`nan`: `strtod` reads all of those, and none of them is a value a hand types into a knob.
std::optional<double> plainNumber(std::string_view s) {
	s = trimmed(s);
	size_t i = 0;
	if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
		++i;
	}
	bool digit = false;
	bool point = false;
	for (; i < s.size(); ++i) {
		if (s[i] >= '0' && s[i] <= '9') {
			digit = true;
		}
		else if (s[i] == '.' && !point) {
			point = true;
		}
		else {
			return std::nullopt;
		}
	}
	if (!digit) {
		return std::nullopt;
	}
	const std::string owned(s);
	char* end = nullptr;
	const double v = std::strtod(owned.c_str(), &end);
	if (end != owned.c_str() + owned.size() || !std::isfinite(v)) {
		return std::nullopt;
	}
	return v;
}

/// A NUMBER AND THE WORD AFTER IT — "2.5 kHz" is 2.5 and "khz" — split where the number's own characters
/// stop. The word is trimmed and lower-cased; an empty one is a bare number.
struct Typed {
	double number = 0.0;
	std::string unit;
};

std::optional<Typed> numberThenUnit(std::string_view s) {
	s = trimmed(s);
	size_t cut = 0;
	if (cut < s.size() && (s[cut] == '+' || s[cut] == '-')) {
		++cut;
	}
	while (cut < s.size() && ((s[cut] >= '0' && s[cut] <= '9') || s[cut] == '.')) {
		++cut;
	}
	const std::optional<double> number = plainNumber(s.substr(0, cut));
	if (!number) {
		return std::nullopt;
	}
	Typed out { *number, {} };
	for (const char c : trimmed(s.substr(cut))) {
		out.unit += lower(c);
	}
	return out;
}

/// `v` at `significant` digits, in plain decimal (no exponent — `plainNumber` reads none), with trailing
/// zeros and a bare point taken off: "1", "0.25", "1234.5".
std::string decimal(double v, int significant) {
	if (v == 0.0 || !std::isfinite(v)) {
		return "0";
	}
	const int magnitude = static_cast<int>(std::floor(std::log10(std::abs(v))));
	const int places = std::clamp(significant - 1 - magnitude, 0, 17);
	char out[64] = {};
	std::snprintf(out, sizeof(out), "%.*f", places, v);
	std::string s = out;
	if (s.find('.') != std::string::npos) {
		while (s.back() == '0') {
			s.pop_back();
		}
		if (s.back() == '.') {
			s.pop_back();
		}
	}
	return s == "-0" ? "0" : s;
}

std::string signedDecimal(double v, int significant) {
	const std::string s = decimal(v, significant);
	return s.front() == '-' ? s : "+" + s;
}

/// One rendering of `editText` at a given precision — the loop below picks the precision.
std::string renderForEdit(const ParamDesc& desc, double value, int significant) {
	switch (desc.unit) {
	case Unit::LogSeconds: {
		if (value <= desc.min) {
			return "off";
		}
		const double seconds = std::exp2(value);
		return seconds >= 1.0 ? decimal(seconds, significant) + "s" : decimal(seconds * 1000.0, significant) + "ms";
	}
	case Unit::LogMultiple:
		return decimal(std::exp2(value), significant) + "X";
	case Unit::LogHertz: {
		if (atOffEnd(desc, value)) {
			return "off";
		}
		const double hz = std::exp2(value);
		return hz >= 1000.0 ? decimal(hz / 1000.0, significant) + "k" : decimal(hz, significant);
	}
	case Unit::Percent:
		return (desc.min < 0.0 ? signedDecimal(value * 100.0, significant) : decimal(value * 100.0, significant)) + "%";
	case Unit::Choice:
	case Unit::Note:
		// A NAME WHEN THE VALUE IS ONE OF THE NAMED STEPS; a switch stored at 0.4 (never rounded) is
		// typed as the number it is.
		if (value == std::round(value)) {
			return formatValue(desc, value);
		}
		return decimal(value, significant);
	case Unit::Decibel:
		return signedDecimal(value, significant) + "dB";
	case Unit::Fader:
		if (value <= 0.0) {
			return "-inf";
		}
		return signedDecimal(40.0 * std::log10(2.0 * value), significant) + "dB";
	case Unit::Degrees:
		return signedDecimal(value * kDegreesPerRadian, significant) + " deg";
	case Unit::Number:
		if (atOffEnd(desc, value)) {
			return "off";
		}
		break;
	}
	return decimal(value, significant);
}

} // namespace

std::string editText(const ParamDesc& desc, double value) {
	// THE FEWEST DIGITS THAT COME BACK AS THE SAME FLOAT — a default reads "1k", a dragged value reads all
	// of itself. Asked of `parseValue` itself, so the two cannot disagree about what "comes back" means.
	const float stored = static_cast<float>(value);
	for (int significant = 3; significant <= 17; ++significant) {
		const std::string text = renderForEdit(desc, value, significant);
		const std::optional<double> back = parseValue(desc, text, value);
		if (back && static_cast<float>(*back) == stored) {
			return text;
		}
	}
	return renderForEdit(desc, value, 17);
}

std::optional<double> parseValue(const ParamDesc& desc, std::string_view text, double current) {
	const std::string_view s = trimmed(text);
	if (s.empty()) {
		return std::nullopt;
	}
	const auto clamped = [&](double v) -> std::optional<double> {
		if (!std::isfinite(v)) {
			return std::nullopt;
		}
		return std::clamp(v, desc.min, desc.max);
	};

	// THE WORDS FIRST: what a control prints when it prints no number.
	if (desc.unit == Unit::Choice) {
		for (size_t i = 0; i < desc.names.size(); ++i) {
			if (desc.names[i] != nullptr && sameWord(s, desc.names[i])) {
				return clamped(desc.min + static_cast<double>(i));
			}
		}
	}
	if (desc.unit == Unit::LogSeconds && sameWord(s, "off")) {
		return desc.min;
	}
	// AND A DECLARED OFF END (backlog FA): typing what the readout says lands on that end.
	if (desc.offAtEnd != 0 && sameWord(s, "off")) {
		return desc.offAtEnd < 0 ? desc.min : desc.max;
	}
	if (desc.unit == Unit::Fader || desc.unit == Unit::Decibel) {
		std::string_view word = s;
		if (word.size() > 2 && sameWord(word.substr(word.size() - 2), "db")) {
			word = trimmed(word.substr(0, word.size() - 2));
		}
		if (sameWord(word, "-inf")) {
			return desc.min;
		}
	}
	if (desc.unit == Unit::Note) {
		// "C3", "C#3", "Db3", "C-2" — the reference's naming, `formatValue`'s inverse: 60 is C3.
		static constexpr int kSemitone[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
		const char letter = lower(s.front());
		if (letter >= 'a' && letter <= 'g') {
			int note = kSemitone[letter - 'a'];
			std::string_view rest = s.substr(1);
			if (!rest.empty() && rest.front() == '#') {
				++note;
				rest.remove_prefix(1);
			}
			else if (!rest.empty() && rest.front() == 'b') {
				--note;
				rest.remove_prefix(1);
			}
			const std::optional<double> octave = plainNumber(rest);
			if (!octave || *octave != std::round(*octave)) {
				return std::nullopt;
			}
			return clamped((*octave + 2.0) * 12.0 + static_cast<double>(note));
		}
	}

	// A MULTIPLE MAY CARRY ITS X IN FRONT ("x2"), as a host's factor prints.
	std::string_view numeric = s;
	bool leadingX = false;
	if (desc.unit == Unit::LogMultiple && (numeric.front() == 'x' || numeric.front() == 'X')) {
		numeric.remove_prefix(1);
		leadingX = true;
	}
	const std::optional<Typed> typed = numberThenUnit(numeric);
	if (!typed) {
		return std::nullopt;
	}
	const double n = typed->number;
	const std::string& unit = typed->unit;
	switch (desc.unit) {
	case Unit::LogHertz: {
		double hz = n;
		if (unit == "k" || unit == "khz") {
			hz = n * 1000.0;
		}
		else if (!unit.empty() && unit != "hz") {
			return std::nullopt;
		}
		if (!(hz > 0.0)) {
			return std::nullopt; // log2 of nothing is not a frequency
		}
		return clamped(std::log2(hz));
	}
	case Unit::LogSeconds: {
		bool seconds = false;
		if (unit == "s") {
			seconds = true;
		}
		else if (unit.empty()) {
			// IN THE UNIT THE CONTROL WAS SHOWING: seconds from 1 s up, milliseconds below (and at "off",
			// whose next reading up is in milliseconds) — `printSeconds`'s own split.
			seconds = current > desc.min && std::exp2(current) >= 1.0;
		}
		else if (unit != "ms") {
			return std::nullopt;
		}
		const double s = seconds ? n : n * 0.001;
		if (s < 0.0) {
			return std::nullopt;
		}
		if (s == 0.0) {
			return desc.min; // no time at all IS the bottom of the range (device.h)
		}
		return clamped(std::log2(s));
	}
	case Unit::LogMultiple:
		if (!(unit.empty() || (unit == "x" && !leadingX))) {
			return std::nullopt;
		}
		if (!(n > 0.0)) {
			return std::nullopt;
		}
		return clamped(std::log2(n));
	case Unit::Percent:
		if (!unit.empty() && unit != "%") {
			return std::nullopt;
		}
		return clamped(n / 100.0);
	case Unit::Decibel:
		if (!unit.empty() && unit != "db") {
			return std::nullopt;
		}
		return clamped(n);
	case Unit::Fader:
		// THE READING IS 40·log10(2v) dB (`formatValue`), so v = 10^(dB/40) / 2.
		if (!unit.empty() && unit != "db") {
			return std::nullopt;
		}
		return clamped(0.5 * std::pow(10.0, n / 40.0));
	case Unit::Degrees:
		// DEGREES, WITH OR WITHOUT THE WORD OR THE SIGN — "90", "90 deg", "90°" — never radians: the box
		// shows degrees, so a bare number is read in what it shows.
		if (!unit.empty() && unit != "deg" && unit != "\xc2\xb0") {
			return std::nullopt;
		}
		return clamped(n / kDegreesPerRadian);
	case Unit::Choice:
	case Unit::Note:
	case Unit::Number:
		if (!unit.empty()) {
			return std::nullopt;
		}
		return clamped(n);
	}
	return std::nullopt;
}

std::optional<float> parseAmountPercent(std::string_view text, float limit) {
	std::string_view s = trimmed(text);
	if (!s.empty() && s.back() == '%') {
		s.remove_suffix(1);
	}
	const std::optional<double> percent = plainNumber(s);
	if (!percent) {
		return std::nullopt;
	}
	return std::clamp(static_cast<float>(*percent / 100.0), -limit, limit);
}

std::string printAmountPercent(float amount) {
	// To a tenth of a percent, which is finer than a route row prints and coarser than a drag can land: the
	// field opens on what the hand will recognise. An unchanged Enter puts the amount back exactly, so the
	// rounding here never writes.
	const float tenths = std::round(amount * 1000.0f);
	char printed[24] = {};
	if (std::fmod(tenths, 10.0f) == 0.0f) {
		std::snprintf(printed, sizeof(printed), "%d", static_cast<int>(tenths / 10.0f));
	}
	else {
		std::snprintf(printed, sizeof(printed), "%.1f", static_cast<double>(tenths / 10.0f));
	}
	return printed;
}

} // namespace feathers::device
