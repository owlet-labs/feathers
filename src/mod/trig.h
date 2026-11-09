/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "mod/modulator.h"

#include <array>
#include <cstddef>
#include <cstdint>

// **A TRIGGER RIDES THE MODULATION DOMAIN BY CONVENTION** (backlog MV, by design: the existing modulation
// base is mature, so a trigger rides this same domain; a separate gate-level
// trigger built into every input would be bad UX).
//
// A device that can be FIRED declares one ordinary modulatable control, Trig, 0..1. It fires when its RESOLVED value
// rises to `kTrigFireLevel` and re-arms only once it has fallen below `kTrigRearmLevel`. The two levels are fixed for
// the whole project and no input gets a threshold control: where a source crosses the fire level is the ROUTE's
// business (its depth), which is where a modulation's scaling already lives. The gap between the two levels is the
// hysteresis that keeps a noisy source hovering at the fire level from chattering.
//
// **ONE COMPARE PER TICK** — the edge is evaluated where the device's row is resolved, once per control tick
// (`mod::kBlockSamples`, 64 samples: 1.45 ms at 44.1 kHz, 1.33 ms at 48 kHz). A fire therefore lands on the tick
// that first reads the crossing; how late that is behind the source's true crossing is at most one tick.
//
// **A TRIGGER OUTPUT IS A ONE-TICK PULSE TO 1** (S&H, the sequencer, the follower's threshold, MW–MY): at depth 1 it
// reads 1 for one tick and 0 after, which crosses the fire level and falls below the re-arm level on the next tick.
// A route carrying one must not be smoothed, or the pulse blurs — see `kTrigSmoothingRule` below.

namespace feathers::mod {

/// WHERE A TRIG FIRES: its resolved value rising to this, inclusive — so a one-tick pulse to 1 at route depth 0.5
/// still lands exactly on it and fires, rather than missing by a rounding.
inline constexpr float kTrigFireLevel = 0.5f;
/// WHERE IT RE-ARMS: strictly below this. A quarter of the range under the fire level, so a source must genuinely
/// come back down before it can fire again.
inline constexpr float kTrigRearmLevel = 0.25f;
static_assert(kTrigRearmLevel < kTrigFireLevel, "the hysteresis gap is what stops a noisy source chattering");

/// **THE EDGE DETECTOR, AND THE WHOLE OF THE CONVENTION'S STATE** — one per Trig (per device under mono; per device
/// too under Poly today, see `PolyShape::fireTrig`). Armed from construction: a Trig that reads high on the first
/// tick a device ever runs has risen from nothing, and fires.
struct TrigEdge {
	bool armed = true;

	/// ONE TICK'S READING. True exactly on the tick the reading reaches the fire level while armed. A non-finite
	/// reading neither fires nor re-arms (both comparisons are false), so a NaN holds the detector where it was.
	[[nodiscard]] bool step(float value) noexcept {
		if (armed && value >= kTrigFireLevel) {
			armed = false;
			return true;
		}
		if (!armed && value < kTrigRearmLevel) {
			armed = true;
		}
		return false;
	}

	void reset() noexcept { armed = true; }
};

/// **SMOOTHING ON A TRIGGER ROUTE IS DOCUMENTED, NOT ENFORCED.** A route in this rack has no smoothing of its own —
/// the only smoothing is a modulator's own Smoothing control, which shapes that modulator's OUTPUT for every route it
/// feeds. A pulse-emitting device (MW–MY) therefore owns the rule: its trigger output is emitted unsmoothed whatever
/// its Smoothing says. A continuous source routed into a Trig is the player's choice to smooth, and a smoothed edge
/// only moves the tick it crosses on. Nothing to enforce at the route; this constant names the decision for a reader.
inline constexpr const char* kTrigSmoothingRule =
    "A trigger output is emitted unsmoothed; routes carry no smoothing, so nothing is enforced at the route.";

/// **WHAT EVERY TRIG'S TOOLTIP ENDS WITH**: a pad routes any
/// source at depth 1, so a bipolar one crosses the fire level only above +0.5. One sentence, shared by every Trig through
/// `trigTooltip`, so no device can word it differently or leave it out.
inline constexpr char kTrigBipolarNote[] = " A bipolar source fires when it rises past +0.5.";

/// A STRING LITERAL AS A TEMPLATE ARGUMENT — what `trigTooltip` joins at compile time.
template <size_t N>
struct TrigText {
	char text[N] {};
	constexpr TrigText(const char (&s)[N]) {
		for (size_t i = 0; i < N; ++i) {
			text[i] = s[i];
		}
	}
};

/// THE TOOLTIP `T` FOLLOWED BY `kTrigBipolarNote`, built once at compile time in static storage.
template <TrigText T>
struct TrigTooltip {
	static constexpr size_t kLength = sizeof(T.text) - 1 + sizeof(kTrigBipolarNote);
	static constexpr std::array<char, kLength> value = [] {
		std::array<char, kLength> out {};
		size_t at = 0;
		for (size_t i = 0; i + 1 < sizeof(T.text); ++i) {
			out[at++] = T.text[i];
		}
		for (const char c : kTrigBipolarNote) {
			out[at++] = c;
		}
		return out;
	}();
};

/// **A TRIG'S TOOLTIP**: its own words, then the shared sentence. `mod::trigTooltip<"Fires the shape.">()`.
template <TrigText T>
[[nodiscard]] constexpr const char* trigTooltip() {
	return TrigTooltip<T>::value.data();
}

/// **THE TRIG CONTROL'S DECLARATION**, one shape for every fireable device so the range, the default and the flags
/// cannot drift between them. NOT STEPPED: a stepped control lands each modulated reading on a step, which would
/// round a continuous source to 0 or 1 at 0.5 and throw the hysteresis away. Its base rests at 0; a hand on the pad
/// holds it at 1 for as long as the press, which is a manual fire (the edge sees it rise) and nothing else.
constexpr device::ParamDesc trigParamDesc(const char* key, const char* name, const char* tooltip) {
	return device::ParamDesc { key, name, 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false, tooltip };
}

/// **NOTES FIRE TRIG — THE NORMAL, AND THE SWITCH THAT BREAKS IT** (backlog NU, DECISION: an option that can be enabled or disabled
/// regardless of other mappings). A note-on restarts the DAHDSR, the MSEG and the LFO as though the note were patched
/// into Trig, like a normalled jack. This stored switch says whether it is. ON (the default) is that behaviour bit for
/// bit; OFF, only the Trig fires the shape — a route, or a hand on the pad. NO ROUTE MOVES IT: adding or deleting a
/// route into Trig never changes it, and with it ON a note and a route both fire.
///
/// **WHAT IT DOES PER MODE (off)** — the shape's own `tick` applies it, one rule in each of the three:
///   - GATE, LOOP: the key still holds the shape open, so the gate is still heard — a key up still releases (the
///     DAHDSR's release, the LFO's rest, the MSEG's gate) — but a key going down no longer STARTS or retriggers it.
///     A Trig starts it while a key is held, as it always has; with no key held a fire does nothing, as before.
///   - ONCE: a note no longer fires the shot. A Trig fires it, key held or not, as before.
///   - LFO RETRIG: notes no longer restart it, so it runs free — it is Free without the song lock (which only Free
///     has), and so mono as Free is (`PolyShape::poly`). A Trig restarts it.
///   - FREE: unchanged — it never heard notes.
/// Under Poly a note still gets its own shape (its slot, its gate), which sits at rest until a Trig fires it.
/// THE STEP SEQUENCER AND S&H HAVE NO SWITCH OF THEIR OWN, because a control they already store IS the normal: the
/// sequencer's `retrig` (a note restarting the pattern is a note firing Reset) and S&H's Clock (Note is notes firing
/// the Trig, Trig is only the Trig). Their pads' "Notes fire Trig" rows drive those (`ui::TrigPad::setNormal`).
constexpr device::ParamDesc notesTrigParamDesc(const char* key, const char* name) {
	return device::ParamDesc { key, name, 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
		                       "On: every note fires the Trig, as though patched into it. Off: only the Trig fires the "
		                       "shape; a held key still gates it under Gate and Loop. Routes never change this" };
}

/// **WHETHER A SHAPE PER NOTE MEANS ANYTHING** under this mode and normal — `PolyShape::poly`'s rule, and the cards'
/// (their Poly and Carry chips are live exactly where it holds). Free ignores notes; LFO Retrig with the normal broken
/// is Free less the song lock, so neither has anything per note to start.
[[nodiscard]] constexpr bool polyRuns(Mode mode, bool notesTrig) noexcept {
	return mode != Mode::Free && !(mode == Mode::LfoRetrig && !notesTrig);
}

/// THE SETTING A SHAPE ASKS, for a `Settings` that may not have one (the step sequencer's): on, unless it says off.
template <typename Settings>
[[nodiscard]] constexpr bool notesFireTrig(const Settings& s) noexcept {
	if constexpr (requires { s.notesTrig; }) {
		return s.notesTrig;
	}
	else {
		return true;
	}
}

} // namespace feathers::mod
