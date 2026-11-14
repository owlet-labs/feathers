/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/format_value.h"
#include "plugin/device_registry.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

// THE VALUE FORMATTER, MOVED OUT OF THE UI (host slots T3), AND THE 0..1 ROAD A HOST SLOT TAKES.
//
// **A COMPARISON OF THE TWO ROADS ALONE WOULD BE VACUOUS**: `ui::formatValue` is now a using-declaration
// of `device::formatValue` (value_box.h says why it cannot be a forwarder), so "they print the same" is
// true of any body the move could have left behind.
// What makes the first case a claim about the MOVE is the golden below it — strings captured from the
// PRE-MOVE UI build (the tree at 448f4599, `ui::formatValue` linked from `feathers-ui` and run over
// these exact values) and pasted here, so a change to what either road prints goes red against text
// nobody computed with the code under test.

using feathers::kDevices;
namespace device = feathers::device;
namespace ui = feathers::ui;

namespace {

// DESCRIPTORS OF OUR OWN, one per unit, so every branch of the formatter is pinned whatever the registry
// holds on the day — the Choice that starts at 1 and runs off both ends, the Note below zero, the
// stepped Number a ramp left at 3.6, the Fader at silence. Built by assignment rather than designated
// initialisers because `ParamDesc` grows fields and -Werror refuses a brace list that misses one.
device::ParamDesc make(const char* key, double min, double max, bool stepped, device::Unit unit,
                       std::span<const char* const> names = {}) {
	device::ParamDesc p {};
	p.key = key;
	p.name = key;
	p.min = min;
	p.max = max;
	p.def = min;
	p.stepped = stepped;
	p.unit = unit;
	p.names = names;
	return p;
}

constexpr const char* kNames3[] = { "One", "Two", "Three" };

const device::ParamDesc kSynth[] = {
	make("s_logsec", -11.0, 3.3219280948873626, false, device::Unit::LogSeconds),
	make("s_loghz", 4.3219280948873617, 14.287712379549449, false, device::Unit::LogHertz),
	make("s_pct", 0.0, 1.0, false, device::Unit::Percent),
	make("s_bipct", -1.0, 1.0, false, device::Unit::Percent),
	make("s_choice", 1.0, 3.0, true, device::Unit::Choice, kNames3),
	make("s_note", -12.0, 127.0, true, device::Unit::Note),
	make("s_db", -24.0, 24.0, false, device::Unit::Decibel),
	make("s_fader", 0.0, 1.0, false, device::Unit::Fader),
	make("s_count", 1.0, 16.0, true, device::Unit::Number),
	make("s_num", -10.0, 10.0, false, device::Unit::Number),
	make("s_mult", -11.0, 3.3219280242919922, false, device::Unit::LogMultiple),
};

struct SynthGolden {
	const char* key;
	double value;
	const char* text;
};

// CAPTURED FROM THE PRE-MOVE `ui::formatValue`, not written by hand.
const SynthGolden kSynthGolden[] = {
	{ "s_logsec", -11, "off" },
	{ "s_logsec", -10.9, "0.5ms" },
	{ "s_logsec", -8, "3.9ms" },
	{ "s_logsec", -5, "31ms" },
	{ "s_logsec", 0.5, "1.4s" },
	{ "s_logsec", -12, "off" },
	{ "s_loghz", 5, "32Hz" },
	{ "s_loghz", 9.9657, "1000Hz" },
	{ "s_loghz", 10.5, "1.4kHz" },
	{ "s_loghz", 12, "4.1kHz" },
	{ "s_loghz", 14, "16kHz" },
	{ "s_loghz", 3, "8Hz" },
	{ "s_pct", 0, "0%" },
	{ "s_pct", 0.33300000000000002, "33%" },
	{ "s_pct", 0.5, "50%" },
	{ "s_pct", 0.996, "100%" },
	{ "s_pct", 1, "100%" },
	{ "s_pct", -0.20000000000000001, "-20%" },
	{ "s_bipct", -1, "-100%" },
	{ "s_bipct", -0.42599999999999999, "-43%" },
	{ "s_bipct", -0.0040000000000000001, "-0%" },
	{ "s_bipct", 0.0040000000000000001, "+0%" },
	{ "s_bipct", 0.69999999999999996, "+70%" },
	{ "s_bipct", 1, "+100%" },
	{ "s_choice", 1, "One" },
	{ "s_choice", 2, "Two" },
	{ "s_choice", 3, "Three" },
	{ "s_choice", 3.3999999999999999, "Three" },
	{ "s_choice", 4, "4" },
	{ "s_choice", 0, "0" },
	{ "s_note", -12, "C-3" },
	{ "s_note", -1, "B-3" },
	{ "s_note", 0, "C-2" },
	{ "s_note", 61, "C#3" },
	{ "s_note", 127, "G8" },
	{ "s_note", 59.600000000000001, "C3" },
	{ "s_db", -24, "-24.0dB" },
	{ "s_db", -0.040000000000000001, "-0.0dB" },
	{ "s_db", 0, "+0.0dB" },
	{ "s_db", 0.050000000000000003, "+0.1dB" },
	{ "s_db", 6.5499999999999998, "+6.5dB" },
	{ "s_db", 24, "+24.0dB" },
	{ "s_fader", -0.10000000000000001, "-inf" },
	{ "s_fader", 0, "-inf" },
	{ "s_fader", 0.25, "-12.0dB" },
	{ "s_fader", 0.5, "+0.0dB" },
	{ "s_fader", 0.70709999999999995, "+6.0dB" },
	{ "s_fader", 1, "+12.0dB" },
	{ "s_count", 1, "1" },
	{ "s_count", 3.3999999999999999, "3" },
	{ "s_count", 3.6000000000000001, "4" },
	{ "s_count", 16, "16" },
	{ "s_count", 2.5, "3" },
	{ "s_count", 0, "0" },
	{ "s_num", -10, "-10.00" },
	{ "s_num", -0.0050000000000000001, "-0.01" },
	{ "s_num", 3.1415899999999999, "3.14" },
	{ "s_num", 9.9990000000000006, "10.00" },
	{ "s_num", 0, "0.00" },
	{ "s_num", 10, "10.00" },
	{ "s_mult", -11, "0.0004883X" },
	{ "s_mult", -1, "0.5X" },
	{ "s_mult", 0, "1X" },
	{ "s_mult", 1, "2X" },
	{ "s_mult", 1.5, "2.828X" },
	{ "s_mult", 3.3219280242919922, "10X" },
};

struct RegistryGolden {
	const char* tag;
	const char* key;
	double value;
	const char* text;
};

// THE SAME CAPTURE OVER REAL CONTROLS — each at 0, 0.1, 0.37, 0.5, 0.83 and 1 of its range and then at
// its default — so the names tables and the ranges a device actually declares are pinned too. The
// value is stored rather than recomputed, so a later change to a device's RANGE does not move these;
// a change to its unit or its names does, and should.
const RegistryGolden kRegistryGolden[] = {
	{ "flt1", "filter_cutoff_wide", 4.3219280948873617, "20Hz" },
	{ "flt1", "filter_cutoff_wide", 5.3185065233535704, "40Hz" },
	{ "flt1", "filter_cutoff_wide", 8.0092682802123338, "258Hz" },
	{ "flt1", "filter_cutoff_wide", 9.3048202372184043, "632Hz" },
	{ "flt1", "filter_cutoff_wide", 12.593529051156894, "6.2kHz" },
	{ "flt1", "filter_cutoff_wide", 14.287712379549449, "20kHz" },
	{ "flt1", "filter_cutoff_wide", 9.965784284662087, "1kHz" },
	{ "flt1", "filter_cutoff_wide", -1.0, "0.5Hz" },              // the knob's bottom since Z8
	{ "flt1", "filter_cutoff_wide", 1.3219280948873622, "2.5Hz" },
	{ "flt1", "filter_cutoff_wide", 14.550746785383243, "24kHz" }, // and its top
	{ "clip", "clip_freq", 8.965784284662087, "500Hz" },
	{ "clip", "clip_freq", 9.4242805347342031, "687Hz" },
	{ "clip", "clip_freq", 10.662220409928915, "1.6kHz" },
	{ "clip", "clip_freq", 11.258265535022666, "2.4kHz" },
	{ "clip", "clip_freq", 12.771303160260647, "7kHz" },
	{ "clip", "clip_freq", 13.550746785383243, "12kHz" },
	{ "clip", "clip_freq", 11.550746785383243, "3kHz" },
	{ "flt1", "filter_res", 0, "0%" },
	{ "flt1", "filter_res", 0.10000000000000001, "10%" },
	{ "flt1", "filter_res", 0.37, "37%" },
	{ "flt1", "filter_res", 0.5, "50%" },
	{ "flt1", "filter_res", 0.82999999999999996, "83%" },
	{ "flt1", "filter_res", 1, "100%" },
	{ "flt1", "filter_res", 0.20000000000000001, "20%" },
	{ "flt1", "filter_macro_1", -1, "-100%" },
	{ "flt1", "filter_macro_1", -0.80000000000000004, "-80%" },
	{ "flt1", "filter_macro_1", -0.26000000000000001, "-26%" },
	{ "flt1", "filter_macro_1", 0, "+0%" },
	{ "flt1", "filter_macro_1", 0.65999999999999992, "+66%" },
	{ "flt1", "filter_macro_1", 1, "+100%" },
	{ "HARM", "harm_attack", -11, "off" },
	{ "HARM", "harm_attack", -9.5678071905112638, "1.3ms" },
	{ "HARM", "harm_attack", -5.700886604891676, "19ms" },
	{ "HARM", "harm_attack", -3.8390359525563191, "70ms" },
	{ "HARM", "harm_attack", 0.88720031875650918, "1.8s" },
	{ "HARM", "harm_attack", 3.3219280948873617, "10s" },
	{ "DHDS", "env_attack", -11, "off" },
	{ "DHDS", "env_attack", -9.5678071975708008, "1.3ms" },
	{ "DHDS", "env_attack", -5.7008866310119632, "19ms" },
	{ "DHDS", "env_attack", -3.8390359878540039, "70ms" },
	{ "DHDS", "env_attack", 0.88720026016235209, "1.8s" },
	{ "DHDS", "env_attack", 3.3219280242919922, "10s" },
	{ "DHDS", "env_attack", -6.6438561897747244, "10ms" },
	{ "AMOD", "rate", -2, "0.25X" },
	{ "AMOD", "rate", -1.6000000000000001, "0.3299X" },
	{ "AMOD", "rate", -0.52000000000000002, "0.6974X" },
	{ "AMOD", "rate", 0, "1X" },
	{ "AMOD", "rate", 1.3199999999999998, "2.497X" },
	{ "AMOD", "rate", 2, "4X" },
	{ "flt1", "filter_type", 0, "Lowpass" },
	{ "flt1", "filter_type", 0.80000000000000004, "Highpass" },
	{ "flt1", "filter_type", 2.96, "Notch" },
	{ "flt1", "filter_type", 4, "Peak" },
	{ "flt1", "filter_type", 6.6399999999999997, "Bell" },
	{ "flt1", "filter_type", 8, "Comb" },
	{ "flt1", "filter_slope", 0, "6 dB" },
	{ "flt1", "filter_slope", 0.30000000000000004, "6 dB" },
	{ "flt1", "filter_slope", 1.1099999999999999, "12 dB" },
	{ "flt1", "filter_slope", 1.5, "18 dB" },
	{ "flt1", "filter_slope", 2.4899999999999998, "18 dB" },
	{ "flt1", "filter_slope", 3, "24 dB" },
	{ "OWLTT", "mode", 1, "AP 6dB" },
	{ "OWLTT", "mode", 1.8999999999999999, "Quirky" },
	{ "OWLTT", "mode", 4.3300000000000001, "Weird" },
	{ "OWLTT", "mode", 5.5, "LR2 Full" },
	{ "OWLTT", "mode", 8.4699999999999989, "LR4 Full" },
	{ "OWLTT", "mode", 10, "Twist3" },
	{ "OWLTT", "mode", 6, "LR2 Full" },
	{ "TRAK", "root_note", 0, "C-2" },
	{ "TRAK", "root_note", 12.700000000000001, "C#-1" },
	{ "TRAK", "root_note", 46.990000000000002, "B1" },
	{ "TRAK", "root_note", 63.5, "E3" },
	{ "TRAK", "root_note", 105.41, "A6" },
	{ "TRAK", "root_note", 127, "G8" },
	{ "TRAK", "root_note", 60, "C3" },
	{ "SHPR", "shaper_emphasis", -12, "-12.0dB" },
	{ "SHPR", "shaper_emphasis", -9.5999999999999996, "-9.6dB" },
	{ "SHPR", "shaper_emphasis", -3.120000000000001, "-3.1dB" },
	{ "SHPR", "shaper_emphasis", 0, "+0.0dB" },
	{ "SHPR", "shaper_emphasis", 7.9199999999999982, "+7.9dB" },
	{ "SHPR", "shaper_emphasis", 12, "+12.0dB" },
	{ "busu", "bus_gain", 0, "-inf" },
	{ "busu", "bus_gain", 0.10000000000000001, "-28.0dB" },
	{ "busu", "bus_gain", 0.37, "-5.2dB" },
	{ "busu", "bus_gain", 0.5, "+0.0dB" },
	{ "busu", "bus_gain", 0.82999999999999996, "+8.8dB" },
	{ "busu", "bus_gain", 1, "+12.0dB" },
	{ "HARM", "harm_harmonic", 0, "0" },
	{ "HARM", "harm_harmonic", 10.100000000000001, "10" },
	{ "HARM", "harm_harmonic", 37.369999999999997, "37" },
	{ "HARM", "harm_harmonic", 50.5, "51" },
	{ "HARM", "harm_harmonic", 83.829999999999998, "84" },
	{ "HARM", "harm_harmonic", 101, "101" },
	{ "HARM", "harm_harmonic", 9, "9" },
	{ "HARM", "harm_fine", -12, "-12.00" },
	{ "HARM", "harm_fine", -9.5999999999999996, "-9.60" },
	{ "HARM", "harm_fine", -3.120000000000001, "-3.12" },
	{ "HARM", "harm_fine", 0, "0.00" },
	{ "HARM", "harm_fine", 7.9199999999999982, "7.92" },
	{ "HARM", "harm_fine", 12, "12.00" },
	{ "flt1", "filter_enabled", 0, "Off" },
	{ "flt1", "filter_enabled", 0.10000000000000001, "Off" },
	{ "flt1", "filter_enabled", 0.37, "Off" },
	{ "flt1", "filter_enabled", 0.5, "On" },
	{ "flt1", "filter_enabled", 0.82999999999999996, "On" },
	{ "flt1", "filter_enabled", 1, "On" },
};

const device::ParamDesc* synthByKey(const char* key) {
	for (const auto& p : kSynth) {
		if (std::strcmp(p.key, key) == 0) {
			return &p;
		}
	}
	return nullptr;
}

const device::ParamDesc* registryControl(const char* tag, const char* key) {
	for (const auto& d : kDevices) {
		if (std::strcmp(d.traits.tag, tag) != 0) {
			continue;
		}
		for (const auto& p : d.params) {
			if (p.key != nullptr && std::strcmp(p.key, key) == 0) {
				return &p;
			}
		}
	}
	return nullptr;
}

} // namespace

TEST_CASE("formatValue prints identically from device and ui") {
	// 1. EVERY CONTROL OF EVERY DEVICE, at its min, default and max, both roads. This half cannot tell a
	// good move from a bad one on its own (see the file's header); it is here so a future edit that
	// gives the UI its own formatter again — a second place for a unit to print — is caught at once.
	size_t controls = 0;
	for (const auto& d : kDevices) {
		for (const auto& p : d.params) {
			++controls;
			for (const double v : { p.min, p.def, p.max }) {
				INFO(d.traits.tag << " / " << (p.key != nullptr ? p.key : p.name) << " at " << v);
				CHECK(device::formatValue(p, v) == ui::formatValue(p, v));
			}
		}
	}
	// NOT VACUOUS: MEASURED at 477 controls the day this was written. A walk that found a handful would
	// pass the loop above on nothing.
	CHECK(controls > 240);

	// 2. THE GOLDEN — what the UI printed BEFORE the move, both roads against it.
	for (const auto& g : kSynthGolden) {
		const device::ParamDesc* p = synthByKey(g.key);
		REQUIRE(p != nullptr);
		INFO(g.key << " at " << g.value);
		CHECK(device::formatValue(*p, g.value) == g.text);
		CHECK(ui::formatValue(*p, g.value) == g.text);
	}
	for (const auto& g : kRegistryGolden) {
		const device::ParamDesc* p = registryControl(g.tag, g.key);
		INFO(g.tag << " / " << g.key << " at " << g.value);
		REQUIRE(p != nullptr);
		CHECK(device::formatValue(*p, g.value) == g.text);
		CHECK(ui::formatValue(*p, g.value) == g.text);
	}
}

TEST_CASE("toNormal/fromNormal invert; a Choice rounds to its index") {
	size_t choices = 0;
	size_t continuous = 0;
	for (const auto& d : kDevices) {
		for (const auto& p : d.params) {
			const double span = p.max - p.min;
			if (!(span > 0.0)) {
				continue;
			}
			INFO(d.traits.tag << " / " << (p.key != nullptr ? p.key : p.name));
			// THE ENDS AND THE DEFAULT, against an independent restatement of the linear sweep.
			CHECK(device::toNormal(p, p.min) == 0.0);
			CHECK(device::toNormal(p, p.max) == 1.0);
			CHECK(std::abs(device::toNormal(p, p.def) - (p.def - p.min) / span) < 1e-12);
			CHECK(device::fromNormal(p, 0.0) == p.min);
			CHECK(device::fromNormal(p, 1.0) == p.max);
			// OUTSIDE THE SWEEP IS CLAMPED, on both roads.
			CHECK(device::toNormal(p, p.max + span) == 1.0);
			CHECK(device::fromNormal(p, -0.5) == p.min);
			CHECK(device::fromNormal(p, 1.5) == p.max);

			if (p.stepped) {
				// EVERY STEP COMES BACK TO ITSELF EXACTLY, and a position within half a step of one
				// lands on it. Bounded: a stepped range this project declares is a count, not a window.
				const long steps = std::lround(span);
				if (steps > 4096) {
					continue;
				}
				for (long k = 0; k <= steps; ++k) {
					const double v = p.min + static_cast<double>(k);
					const double n = device::toNormal(p, v);
					CHECK(device::fromNormal(p, n) == v);
					CHECK(device::fromNormal(p, n + 0.4 / span) == std::min(v, p.max));
					CHECK(device::fromNormal(p, n - 0.4 / span) == std::max(v, p.min));
				}
				if (p.unit == device::Unit::Choice) {
					++choices;
					// A CHOICE'S STEP IS ITS INDEX, and the index names the entry: the value a host's
					// normalised write lands on prints the name at that index — OWLTT's Mode, which
					// starts at 1, included.
					for (size_t i = 0; i < p.names.size() && static_cast<long>(i) <= steps; ++i) {
						const double n = device::toNormal(p, p.min + static_cast<double>(i));
						const double v = device::fromNormal(p, n);
						CHECK(v - p.min == static_cast<double>(i));
						if (p.names[i] != nullptr) {
							CHECK(device::formatValue(p, v) == p.names[i]);
						}
					}
				}
			}
			else {
				++continuous;
				for (const double t : { 0.1, 0.25, 0.37, 0.5, 0.75, 0.9 }) {
					const double v = device::fromNormal(p, t);
					CHECK(std::abs(v - (p.min + t * span)) <= 1e-12 * std::max(1.0, std::abs(span)));
					CHECK(std::abs(device::toNormal(p, v) - t) < 1e-9);
				}
			}
		}
	}
	// NOT VACUOUS, both halves: MEASURED at 124 Choice controls and 344 continuous ones the day this was
	// written; the floors sit at half, so a device retired does not redden this and a walk gone empty does.
	CHECK(choices > 60);
	CHECK(continuous > 150);
}

//---------------------------------------------------------------------------------------------------//
// TYPE-IN (backlog LS): `parseValue` reads what `editText` and `formatValue` print.                  //
//---------------------------------------------------------------------------------------------------//

namespace {

/// A declaration of one unit over a range, for the cases below — values from the unit's own contract
/// (device.h), never from what the parser happens to do.
device::ParamDesc unitDesc(device::Unit unit, double min, double max, double def = 0.0) {
	device::ParamDesc p {};
	p.name = "Probe";
	p.key = "probe";
	p.unit = unit;
	p.min = min;
	p.max = max;
	p.def = def;
	return p;
}

/// ABSOLUTE bounds, never `Approx` — its scale term passes small values against small targets (memory:
/// doctest-approx-scale-trap).
bool near(std::optional<double> got, double want, double tolerance = 1e-9) {
	return got.has_value() && std::abs(*got - want) <= tolerance;
}

} // namespace

TEST_CASE("parseValue reads each unit in the unit it displays, and a suffix overrides it") {
	using device::Unit;
	using device::parseValue;

	SUBCASE("a frequency: a bare number is hertz, a k is a thousand of them") {
		const device::ParamDesc hz = unitDesc(Unit::LogHertz, std::log2(20.0), std::log2(20000.0), std::log2(1000.0));
		const double at = std::log2(1200.0); // shows "1.2k"
		CHECK(near(parseValue(hz, "800", at), std::log2(800.0)));
		CHECK(near(parseValue(hz, "2k", at), std::log2(2000.0)));
		CHECK(near(parseValue(hz, "2.5 kHz", at), std::log2(2500.0)));
		CHECK(near(parseValue(hz, "440Hz", at), std::log2(440.0)));
		CHECK(near(parseValue(hz, "1.2k", at), std::log2(1200.0)));
		// CLAMPED TO THE RANGE, never refused for being outside it.
		CHECK(near(parseValue(hz, "5", at), std::log2(20.0)));
		CHECK(near(parseValue(hz, "90k", at), std::log2(20000.0)));
		CHECK_FALSE(parseValue(hz, "0", at).has_value()); // log2 of nothing is not a frequency
		CHECK_FALSE(parseValue(hz, "-100", at).has_value());
	}

	SUBCASE("a duration: a bare number is in whichever of ms and s the control is showing") {
		const device::ParamDesc t = unitDesc(Unit::LogSeconds, std::log2(0.0005), std::log2(20.0), std::log2(0.1));
		const double showingMs = std::log2(0.5);  // "500ms"
		const double showingS = std::log2(1.5);   // "1.5s"
		CHECK(near(parseValue(t, "800", showingMs), std::log2(0.8)));
		CHECK(near(parseValue(t, "2", showingS), std::log2(2.0)));
		CHECK(near(parseValue(t, "250ms", showingS), std::log2(0.25)));
		CHECK(near(parseValue(t, "1.5s", showingMs), std::log2(1.5)));
		CHECK(near(parseValue(t, "1.5 S", showingMs), std::log2(1.5)));
		// THE BOTTOM MEANS "NO TIME AT ALL" (device.h), and it is typed the way it prints.
		CHECK(near(parseValue(t, "off", showingS), t.min));
		CHECK(near(parseValue(t, "OFF", showingS), t.min));
		CHECK(near(parseValue(t, "0", showingMs), t.min));
		// AT "off" A BARE NUMBER IS IN MILLISECONDS — the unit the next reading up prints in.
		CHECK(near(parseValue(t, "3", t.min), std::log2(0.003)));
	}

	SUBCASE("a percent, signed or not") {
		const device::ParamDesc pct = unitDesc(Unit::Percent, 0.0, 1.0, 0.5);
		const device::ParamDesc signedPct = unitDesc(Unit::Percent, -1.0, 1.0, 0.0);
		CHECK(near(parseValue(pct, "50", 0.2), 0.5));
		CHECK(near(parseValue(pct, "12.5%", 0.2), 0.125));
		CHECK(near(parseValue(pct, " 7 % ", 0.2), 0.07));
		CHECK(near(parseValue(pct, "150", 0.2), 1.0));
		CHECK(near(parseValue(signedPct, "-42%", 0.0), -0.42));
		CHECK(near(parseValue(signedPct, "+42", 0.0), 0.42));
		// A PERCENT, NEVER A GUESSED FRACTION: 0.5 is half a percent.
		CHECK(near(parseValue(pct, "0.5", 0.2), 0.005));
	}

	SUBCASE("decibels, and a fader's -inf") {
		const device::ParamDesc db = unitDesc(Unit::Decibel, -24.0, 24.0, 0.0);
		CHECK(near(parseValue(db, "-6", 0.0), -6.0));
		CHECK(near(parseValue(db, "+3.5dB", 0.0), 3.5));
		CHECK(near(parseValue(db, "-6 db", 0.0), -6.0));
		const device::ParamDesc fader = unitDesc(Unit::Fader, 0.0, 1.0, 0.5);
		// gain = (2v)^2, so the reading is 40·log10(2v) dB, and 0 dB is v = 0.5 (format_value.cpp).
		CHECK(near(parseValue(fader, "0", 0.5), 0.5));
		CHECK(near(parseValue(fader, "-6dB", 0.5), 0.5 * std::pow(10.0, -6.0 / 40.0)));
		CHECK(near(parseValue(fader, "-inf", 0.5), 0.0));
		CHECK(near(parseValue(fader, "-INF dB", 0.5), 0.0));
		// BEYOND THE TOP IS THE TOP: +40·log10(2) dB at v = 1.
		CHECK(near(parseValue(fader, "+30", 0.5), 1.0));
	}

	SUBCASE("a multiple, with its X on either side") {
		const device::ParamDesc x = unitDesc(Unit::LogMultiple, -4.0, 4.0, 0.0);
		CHECK(near(parseValue(x, "2", 0.0), 1.0));
		CHECK(near(parseValue(x, "2X", 0.0), 1.0));
		CHECK(near(parseValue(x, "x0.5", 0.0), -1.0));
		CHECK(near(parseValue(x, "0.25x", 0.0), -2.0));
		CHECK_FALSE(parseValue(x, "0", 0.0).has_value());
	}

	SUBCASE("a note by name (the reference's, 60 = C3) or by number") {
		const device::ParamDesc note = unitDesc(Unit::Note, 0.0, 127.0, 60.0);
		CHECK(near(parseValue(note, "C3", 60.0), 60.0));
		CHECK(near(parseValue(note, "c#3", 60.0), 61.0));
		CHECK(near(parseValue(note, "Db3", 60.0), 61.0));
		CHECK(near(parseValue(note, "A4", 60.0), 81.0));
		CHECK(near(parseValue(note, "C-2", 60.0), 0.0));
		CHECK(near(parseValue(note, "64", 60.0), 64.0));
		CHECK_FALSE(parseValue(note, "H3", 60.0).has_value());
	}

	SUBCASE("a choice by its name, any case, or by its raw value") {
		static constexpr const char* kNames[] = { "Low", "Band", "High" };
		device::ParamDesc choice = unitDesc(Unit::Choice, 1.0, 3.0, 1.0); // starts at 1, as OWLTT's Mode does
		choice.stepped = true;
		choice.names = kNames;
		CHECK(near(parseValue(choice, "band", 1.0), 2.0));
		CHECK(near(parseValue(choice, "HIGH", 1.0), 3.0));
		CHECK(near(parseValue(choice, "3", 1.0), 3.0));
		CHECK_FALSE(parseValue(choice, "Notch", 1.0).has_value());
	}

	SUBCASE("a plain number, and NOT rounded to a step: that is the control's setValue's job") {
		device::ParamDesc n = unitDesc(Unit::Number, 0.0, 16.0, 4.0);
		n.stepped = true;
		CHECK(near(parseValue(n, "3.6", 4.0), 3.6));
		CHECK(near(parseValue(n, "-3", 4.0), 0.0));
	}

	SUBCASE("anything that is not a number is no value") {
		const device::ParamDesc pct = unitDesc(Unit::Percent, 0.0, 1.0, 0.5);
		const device::ParamDesc n = unitDesc(Unit::Number, -10.0, 10.0, 0.0);
		for (const char* junk : { "", " ", "%", "abc", "12x", "1/16", "inf", "nan", "0x10", "1e400", "--3", "3 3" }) {
			INFO("typed: \"" << junk << "\"");
			CHECK_FALSE(parseValue(pct, junk, 0.5).has_value());
			CHECK_FALSE(parseValue(n, junk, 0.0).has_value());
		}
		// A UNIT THE CONTROL DOES NOT HAVE IS JUNK TOO: a percent is not typed in hertz.
		CHECK_FALSE(parseValue(pct, "50Hz", 0.5).has_value());
		CHECK_FALSE(parseValue(n, "5%", 0.0).has_value());
	}
}

TEST_CASE("editText opens on the fewest digits that read back to the same stored value, for every device control") {
	// THE GUARD'S DEVICE HALF (backlog LS): for every control of every device, at its ends, its default and
	// a sweep between, the text a field opens on parses back to the value it was opened on — to the float
	// the value is stored as. The widget half (tests/gui_widgets, "every knob and value box on every device
	// opens its field on a text that reads back to its value, or refuses") asks the same of each control as
	// the editor seats it, through its adapter, which is where a device's own TextOps can refuse.
	size_t controls = 0;
	size_t checked = 0;
	for (const auto& d : kDevices) {
		for (const auto& p : d.params) {
			const double span = p.max - p.min;
			if (!(span > 0.0)) {
				continue;
			}
			++controls;
			std::vector<double> values { p.min, p.def, p.max };
			for (int k = 1; k < 64; ++k) {
				const double v = p.min + span * static_cast<double>(k) / 64.0;
				values.push_back(p.stepped ? std::round(v) : v);
			}
			for (const double raw : values) {
				// STORED AS A FLOAT (a device row is floats), so the value a field opens on is one.
				const double v = static_cast<double>(static_cast<float>(raw));
				const std::string text = device::editText(p, v);
				const std::optional<double> back = device::parseValue(p, text, v);
				// A NAMED STRING, NOT TEMPORARIES: INFO captures by reference and reads them only on failure.
				const std::string where = std::string(d.traits.tag) + " / " + (p.key != nullptr ? p.key : p.name)
				                          + " at " + std::to_string(v) + " opens on \"" + text + "\"";
				INFO(where);
				REQUIRE(back.has_value());
				CHECK(static_cast<float>(*back) == static_cast<float>(v));
				// AND THE READING IS THE SAME once stored — a float, so asked of the float: the field says what the
				// control was showing.
				// (`+ 0.0` makes a -0 a 0: the same value, which a stepped sweep can land on as either.)
				CHECK(device::formatValue(p, static_cast<double>(static_cast<float>(*back)) + 0.0)
				      == device::formatValue(p, v + 0.0));
				++checked;
			}
		}
	}
	CHECK(controls > 400);
	MESSAGE("controls: " << controls << ", values checked: " << checked);

	// READABLE, NOT JUST EXACT: a clean value opens on its clean text.
	using device::Unit;
	const device::ParamDesc hz = unitDesc(Unit::LogHertz, std::log2(20.0), std::log2(20000.0));
	CHECK(device::editText(hz, std::log2(1000.0)) == "1kHz");
	CHECK(device::editText(hz, std::log2(440.0)) == "440Hz");
	const device::ParamDesc pct = unitDesc(Unit::Percent, 0.0, 1.0);
	CHECK(device::editText(pct, 0.5) == "50%");
	const device::ParamDesc t = unitDesc(Unit::LogSeconds, std::log2(0.0005), std::log2(20.0));
	CHECK(device::editText(t, std::log2(0.25)) == "250ms");
	CHECK(device::editText(t, std::log2(2.0)) == "2s");
	CHECK(device::editText(t, t.min) == "off");
	const device::ParamDesc db = unitDesc(Unit::Decibel, -24.0, 24.0);
	CHECK(device::editText(db, -6.0) == "-6dB");
	const device::ParamDesc fader = unitDesc(Unit::Fader, 0.0, 1.0);
	CHECK(device::editText(fader, 0.0) == "-inf");
	CHECK(device::editText(fader, 0.5) == "+0dB");
	const device::ParamDesc note = unitDesc(Unit::Note, 0.0, 127.0);
	CHECK(device::editText(note, 60.0) == "C3");
}

TEST_CASE("a route amount is typed as a percent, and anything that is not a number is no value") {
	using device::parseAmountPercent;
	using device::printAmountPercent;
	CHECK(std::abs(*parseAmountPercent("50", 2.0f) - 0.5f) < 1e-6f);
	CHECK(std::abs(*parseAmountPercent(" -12.5 % ", 2.0f) + 0.125f) < 1e-6f);
	CHECK(std::abs(*parseAmountPercent("0.5", 2.0f) - 0.005f) < 1e-6f);
	CHECK(*parseAmountPercent("-900", 2.0f) == -2.0f);
	for (const char* junk : { "", "%", "abc", "12x", "inf", "0x10" }) {
		INFO("typed: \"" << junk << "\"");
		CHECK_FALSE(parseAmountPercent(junk, 2.0f).has_value());
	}
	CHECK(printAmountPercent(0.5f) == "50");
	CHECK(printAmountPercent(-0.125f) == "-12.5");
	CHECK(printAmountPercent(0.0f) == "0");
}

TEST_CASE("FA: the Shaper's input filters read \"off\" at their open ends, and \"off\" types back to them") {
	// THE READOUT SAYS WHAT THE ENGINE DOES (backlog FA): at 20 Hz the HPF, and at 20 kHz the LPF, are an exact
	// bypass, so naming a corner there would describe a filter that is not running. The engine's test and this
	// one are the same function, `device::atOffEnd`.
	namespace shp = feathers::fx::shaper;
	const feathers::device::ParamDesc& hpf = shp::kParamDescs[static_cast<size_t>(shp::Param::InputHpf)];
	const feathers::device::ParamDesc& lpf = shp::kParamDescs[static_cast<size_t>(shp::Param::InputLpf)];
	CHECK(feathers::device::formatValue(hpf, hpf.min) == "off");
	CHECK(feathers::device::formatValue(lpf, lpf.max) == "off");
	// Within the slack is still the end — and the engine agrees it is off.
	CHECK(feathers::device::formatValue(hpf, hpf.min + 0.5 * feathers::device::kOffEndSlack) == "off");
	CHECK_FALSE(shp::inputHighpassOn(static_cast<float>(hpf.min + 0.5 * feathers::device::kOffEndSlack)));
	// ONLY THE DECLARED END: the HPF's top and the LPF's bottom are corners like any other.
	CHECK(feathers::device::formatValue(hpf, hpf.max) == "20kHz");
	CHECK(feathers::device::formatValue(lpf, lpf.min) == "20Hz");
	CHECK(feathers::device::formatValue(hpf, std::log2(250.0)) == "250Hz");
	CHECK(feathers::device::formatValue(hpf, hpf.min + 0.01) == "20Hz"); // a hundredth of an octave in is a filter
	CHECK(shp::inputHighpassOn(static_cast<float>(hpf.min + 0.01)));
	// THE EDIT TEXT TOO, and typing "off" lands on the end that is off.
	CHECK(feathers::device::editText(hpf, hpf.min) == "off");
	CHECK(feathers::device::editText(lpf, lpf.max) == "off");
	CHECK(feathers::device::parseValue(hpf, "off", 10.0) == hpf.min);
	CHECK(feathers::device::parseValue(lpf, "Off", 10.0) == lpf.max);
	// AND A CONTROL THAT DECLARES NO OFF END IS UNTOUCHED: Featherverb's LPF at 20 kHz is a 20 kHz filter.
	bool sawPlain = false;
	for (const auto& d : kDevices) {
		for (const auto& p : d.params) {
			if (p.unit == feathers::device::Unit::LogHertz && p.offAtEnd == 0) {
				CHECK(feathers::device::formatValue(p, p.max) != "off");
				CHECK(feathers::device::formatValue(p, p.min) != "off");
				sawPlain = true;
			}
		}
	}
	CHECK(sawPlain);
}

TEST_CASE("an angle reads in whole degrees and types in degrees: 0, a quarter, a half and a wound turn") {
	// THE ROTATE ENTRY ("Rotate reads 3.14", 2026-09-01): the stored number is radians and a hand reads
	// degrees. Display-only — the stored value is untouched, which is what the parse round-trip pins.
	using device::Unit;
	using device::parseValue;
	constexpr double kPi = 3.14159265358979323846;
	// A COMPASS'S DRAG WINDOW, unbounded both ways, and Explore's Theta's folded half-turn.
	const device::ParamDesc compass = unitDesc(Unit::Degrees, -8388608.0, 8388608.0);
	const device::ParamDesc folded = unitDesc(Unit::Degrees, -kPi, kPi);

	CHECK(device::formatValue(compass, 0.0) == "+0 deg");
	CHECK(device::formatValue(compass, kPi / 2.0) == "+90 deg");
	CHECK(device::formatValue(compass, kPi) == "+180 deg");
	CHECK(device::formatValue(compass, -kPi / 2.0) == "-90 deg");
	// UNBOUNDED: a wound value reads its winding, not its fold — the type-in gives back what is shown.
	CHECK(device::formatValue(compass, 2.5 * kPi) == "+450 deg");
	CHECK(device::formatValue(compass, -5.0 * kPi) == "-900 deg");
	// A hair under zero reads as zero, unsigned by the rounding.
	CHECK(device::formatValue(compass, -1e-6) == "+0 deg");
	// Whole degrees: 0.3 rad is 17.19 degrees.
	CHECK(device::formatValue(compass, 0.3) == "+17 deg");

	// TYPED IN DEGREES, with or without the word or the sign.
	CHECK(near(parseValue(compass, "90", 0.0), kPi / 2.0));
	CHECK(near(parseValue(compass, "90 deg", 0.0), kPi / 2.0));
	CHECK(near(parseValue(compass, "-45deg", 0.0), -kPi / 4.0));
	CHECK(near(parseValue(compass, "180\xc2\xb0", 0.0), kPi));
	CHECK(near(parseValue(compass, "720", 0.0), 4.0 * kPi));
	CHECK_FALSE(parseValue(compass, "1.57 rad", 0.0).has_value());
	CHECK_FALSE(parseValue(compass, "50%", 0.0).has_value());
	// A DECLARED RANGE STILL CLAMPS: Theta's own widget unwinds it (TravelControl::parseText).
	CHECK(near(parseValue(folded, "270", 0.0), kPi));

	// THE ROUND TRIP: what the field opens on reads back to the same stored float, at every value above.
	for (const double v : { 0.0, kPi / 2.0, kPi, -kPi / 2.0, 2.5 * kPi, -5.0 * kPi, 0.3, 1e-3 }) {
		CAPTURE(v);
		const std::string opened = device::editText(compass, v);
		CAPTURE(opened);
		CHECK(opened.find(" deg") != std::string::npos);
		const std::optional<double> back = parseValue(compass, opened, v);
		REQUIRE(back.has_value());
		CHECK(static_cast<float>(*back) == static_cast<float>(v));
	}
}

TEST_CASE("a readout carries its unit: frequencies print Hz and kHz, a plain Number prints its declared suffix, and typing reads it with or without") {
	using device::Unit;
	// A FREQUENCY'S UNIT IS ON THE READOUT, the card, the window, the tooltip and the field alike — one formatter. Expected
	// strings are restated from the frequency, not read off the formatter.
	const device::ParamDesc hz = unitDesc(Unit::LogHertz, std::log2(20.0), std::log2(20000.0), std::log2(1000.0));
	CHECK(device::formatValue(hz, std::log2(1000.0)) == "1kHz");
	CHECK(device::formatValue(hz, std::log2(150.0)) == "150Hz");
	CHECK(device::formatValue(hz, std::log2(20000.0)) == "20kHz");
	CHECK(device::formatValue(hz, std::log2(1200.0)) == "1.2kHz");
	CHECK(device::formatValue(hz, std::log2(4.0)) == "4Hz"); // under 10 Hz, two significant digits
	CHECK(device::editText(hz, std::log2(1200.0)) == "1.2kHz");
	// AND THE FIELD READS BACK what it opened on, and what a hand types without the unit.
	CHECK(near(device::parseValue(hz, "1.2kHz", 0.0), std::log2(1200.0)));
	CHECK(near(device::parseValue(hz, "1200", 0.0), std::log2(1200.0)));
	CHECK(near(device::parseValue(hz, "150hz", 0.0), std::log2(150.0)));

	// A PLAIN NUMBER WITH A DECLARED SUFFIX (`ParamDesc::suffix`): the crossovers' hertz, the grain size's milliseconds.
	device::ParamDesc xover = make("xover", 20.0, 2000.0, false, Unit::Number);
	xover.wholeReadout = true;
	xover.suffix = "Hz";
	CHECK(device::formatValue(xover, 200.0) == "200Hz"); // whole hertz, not "200.00"
	CHECK(device::formatValue(xover, 2000.0) == "2000Hz");
	CHECK(device::formatValue(xover, 199.6) == "200Hz");
	CHECK(device::editText(xover, 200.0) == "200Hz");
	CHECK(near(device::parseValue(xover, "350", 200.0), 350.0));
	CHECK(near(device::parseValue(xover, "350Hz", 200.0), 350.0));
	CHECK(near(device::parseValue(xover, "350 HZ", 200.0), 350.0));
	CHECK_FALSE(device::parseValue(xover, "350ms", 200.0).has_value()); // another unit is no value
	// AND A CONTINUOUS NUMBER KEEPS ITS DECIMALS, then the unit.
	device::ParamDesc oct = make("oct", -4.0, 4.0, false, Unit::Number);
	oct.suffix = "oct";
	CHECK(device::formatValue(oct, 0.5) == "0.50oct");
	CHECK(near(device::parseValue(oct, "0.5oct", 0.0), 0.5));
	CHECK(near(device::parseValue(oct, "0.5", 0.0), 0.5));
	// NO SUFFIX, NO CHANGE: a bare Number still prints bare and still refuses a unit.
	const device::ParamDesc bare = make("bare", 0.0, 10.0, false, Unit::Number);
	CHECK(device::formatValue(bare, 2.5) == "2.50");
	CHECK_FALSE(device::parseValue(bare, "2.5Hz", 0.0).has_value());

	// THE REGISTRY'S OWN DECLARATIONS: the controls the audit named carry theirs.
	const auto readout = [](const char* tag, const char* key, double value) {
		for (const device::Descriptor& d : kDevices) {
			if (std::string(d.traits.tag) != tag) {
				continue;
			}
			for (const device::ParamDesc& p : d.params) {
				if (std::string(p.key) == key) {
					return device::formatValue(p, value);
				}
			}
		}
		return std::string("(no such control)");
	};
	CHECK(readout("OWLTT", "low_crossover", 200.0) == "200Hz");
	CHECK(readout("OWLTT", "high_crossover", 2000.0) == "2000Hz");
	CHECK(readout("PITCH", "pitch_size", 60.0) == "60ms");
	CHECK(readout("ERODER", "eroder_cutoff", 0.5) == "0.50oct");
	CHECK(readout("drwn", "cutoff", 1.0) == "1.00oct");
	CHECK(readout("flt1", "filter_cutoff_wide", std::log2(1000.0)) == "1kHz");
	CHECK(readout("FVERB", "featherverb_hpf_hz", std::log2(150.0)) == "150Hz");
}
