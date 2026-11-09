/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/device.h"

#include <optional>
#include <string>
#include <string_view>

// A CONTROL'S VALUE AS TEXT AND AS A POSITION, from its declaration alone.
//
// **THE FORMATTER LIVED IN THE UI LIBRARY** (`plugin/ui/value_box.cpp`), behind a header that pulls in
// visage, and it is needed by something that must not: a host slot shows its target control's value in
// the DAW's automation lane as the control's own text — "1.2 kHz", "Saw" — and that is CLAP code
// (host slots, T3). Nothing in it was ever about drawing: a declared
// UNIT is all it needs, which is the promise of declaring units at all. So it lives with the
// vocabulary, and `ui::formatValue` is now a using-declaration of it (`plugin/ui/value_box.h`).
//
// It depends on `device/device.h` and the standard library and on nothing else — the rule every file
// in `src/device/` keeps, because this is the directory a third-party device compiles against.

namespace feathers::device {

/// WHAT A CONTROL'S VALUE READS AS, given only what the control declares about itself.
///
/// Handles the LogSeconds convention that the bottom of a range means no time at all, printing
/// "off" — see device.h, where that convention is stated for both ends of the contract.
std::string formatValue(const ParamDesc& desc, double value);

/// A DURATION IN SECONDS AS `formatValue` PRINTS ONE — precision following the magnitude ("0.5ms",
/// "841ms", "1.2s"). Public for a device's own `TextOps` that must print a LogSeconds control without
/// the "off" at its bottom (the Phaser/Flanger's 0.2 ms delay is a delay, not an absence).
std::string printSeconds(double seconds);

/// AN ANGLE IN RADIANS AS `Unit::Degrees` PRINTS ONE — whole signed degrees, "+90 deg". Public for a readout
/// drawn without a descriptor (Explore's Rotate corner), so the two cannot print the same turn differently.
std::string printDegrees(double radians);

/// A VALUE AS A 0..1 POSITION ALONG ITS CONTROL'S DECLARED RANGE — linear in device units.
///
/// **LINEAR, AND THAT IS NOT A SIMPLIFICATION.** device.h: *"there is no separate 'normalised' domain
/// anywhere in this project"* — a logarithmic control declares its value AS the logarithm (`LogHertz`
/// is log2 Hz, `LogSeconds` log2 s), so the curve is already in the value and a straight line through
/// [min, max] is the knob's own sweep. This is the arithmetic `Modulatable::normalized()` draws with
/// (it now calls this), and the inverse of `Knob::dragTo`'s `min + t * (max - min)`.
///
/// Clamped to the sweep. A zero or inverted span, or a non-finite value, answers 0 — a misdeclared
/// descriptor, not a runtime condition, and the bottom of the sweep rather than a NaN walking on.
///
/// A STEPPED VALUE IS NOT ROUNDED ON THE WAY IN: `coerceControl` stores a switch at 0.4 as 0.4
/// (a stepped value is stored where the hand left it, never rounded), and its position is where the hand left it, as the widget draws it.
double toNormal(const ParamDesc& desc, double value);

/// A 0..1 POSITION BACK TO DEVICE UNITS — `toNormal`'s inverse, clamped to [min, max].
///
/// **A STEPPED CONTROL LANDS ON A STEP**, counted from `min` — so a Choice lands on `min + index`,
/// the value `formatValue` prints a name for, and a Choice whose range does not start at zero (OWLTT's
/// Mode, 1..10) lands on its own steps rather than the ones a bare `lround` would pick. This is the
/// road a host's normalised write takes into a control, and a host that sends 0.51 for a two-entry
/// choice means one entry or the other, not a fraction between them.
///
/// A position outside 0..1 is clamped; a non-finite one answers `min`, the bottom of the sweep, for
/// `toNormal`'s reason.
double fromNormal(const ParamDesc& desc, double normal);

//---------//
// Type-in //
//---------//
//
// Backlog LS: a triple-click on a knob or a value box opens a field to type a value
// into. These two are the unit's half of that, and they are each other's inverse: `editText` is what the
// field opens on, `parseValue` reads what was typed. Neither knows about a device's `TextOps` — a control a
// device prints its own way is the control adapter's to refuse (`Modulatable::editText`).

/// **WHAT THE FIELD OPENS ON** — `formatValue`'s unit and suffix ("800", "1.2k", "500ms", "50%", "-6dB",
/// "2X", "C3", a Choice's name), at the FEWEST digits that read back to the same stored value. Values are
/// stored as floats, so "the same" is float equality: a value the hand left at 1234.567 Hz opens on all of
/// that, and a default of 1 kHz opens on "1k", not "1.00000k".
std::string editText(const ParamDesc& desc, double value);

/// **A TYPED VALUE, READ IN THE UNIT THE CONTROL DISPLAYS** — `editText`'s inverse, and so `formatValue`'s
/// for every unit it prints. A bare number is in the displayed unit: Hz for a frequency ("800" is 800 Hz,
/// "2k" is 2 kHz), percent for a percent, dB for a gain, and for a duration whichever of ms and s the
/// control is showing at `current` (so "800" at a reading of "500ms" is 800 ms). A suffix overrides it:
/// `k`/`kHz`/`Hz`, `ms`/`s`, `%`, `dB`, `X` (either side). `off` is the bottom of a duration, `-inf` the
/// bottom of a fader. A Note reads "C3" (60, the reference's naming) or a note number; a Choice its name, any
/// case, or its raw value. Clamped to [min, max]; NOT rounded to a step, which is the control's own
/// `setValue`'s job. Empty, non-numeric, non-finite, or trailing junk is no value at all.
std::optional<double> parseValue(const ParamDesc& desc, std::string_view text, double current);

/// **A ROUTE AMOUNT, TYPED AS A PERCENT** — "50", "+50", "50%", " -12.5 % " — the unit a route prints.
/// A fraction is never guessed at: "0.5" is half a percent. Clamped to ±`limit`; empty, non-numeric or
/// trailing junk is no value. Returns the AMOUNT (a fraction). Moved here from `ui::RouteList`, which still
/// answers with it, so a knob in mapping mode types the same thing the route list does.
std::optional<float> parseAmountPercent(std::string_view text, float limit);
/// What an amount's field opens on: a percent, the sign only when negative, a decimal only when it has one.
std::string printAmountPercent(float amount);

} // namespace feathers::device
