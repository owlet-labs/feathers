/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include <cstdlib>

#include "plugin/device_registry.h"
#include "plugin/state_block_order.h"
#include "codec_includes.inc"

#include "fx/automod/automod_types.h"
#include "fx/busutil/busutil_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/drawn/drawn_types.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#include "fx/delay/delay_types.h"
#include "fx/pitch/pitch_types.h"
#include "fx/eroder/eroder_types.h"
#include "fx/disperser/disperser_types.h"
#include "fx/phaser/phaser_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/comp/comp_types.h"
#include "fx/clip/clip_types.h"
#include "fx/spectral/spectral_types.h"
#include "fx/smooth/smooth_types.h"
#include "fx/eq/eq_types.h"
#include "fx/resonate/resonate_types.h"
#include "fx/shaper/shaper_types.h"
#include "fx/waypoint/waypoint_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/noteexpr/noteexpr.h"
#include "mod/noteexpr/noteexpr_types.h"
#include "mod/tracker/tracker.h"
#include "mod/tracker/tracker_types.h"
#include "mod/macros/macros.h"
#include "mod/macros/macros_types.h"
#include "mod/math/math.h"
#include "mod/stereo/stereo.h"
#include "mod/rtrig/rtrig_types.h"
#include "mod/samplehold/samplehold_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "mod/follower/follower.h"
#include "mod/math/math_types.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// **THE INSTRUMENT FOR A BUG SHAPE THAT HAS NOW BEEN FOUND FIVE TIMES, EVERY TIME BY HAND.**
//
// Shaper Gamma, Shaper Rotate, Shaper Colour Phase, OWLTT Character Phase, OWLTT Vibe Phase. Three of
// them live in one file. Each was removed in its own pass and each pass left the others standing,
// because the only thing that ever asked the question was a person turning a knob and noticing it
// stopped. **A defect class whose sole detector is a person is a defect class that ships.**
//
// The shape has TWO faces and this file has a case for each. They are different failures with
// different symptoms and neither case can see the other's:
//
//   THE WALL — the DECLARATION is finite where the engine has no end. `ZoneCoordBox::onCarry` clamps
//   each carried lap into `ParamDesc::min..max`, so a declaration of `0..64` is a knob that stops at
//   the 64th lap. **Symptom: the control stops moving, and the readout stops too.** These get
//   noticed, because a knob that will not turn is obvious.
//
//   THE CLAMP — the DECODE is narrower than the declaration. `stateOf` pinned `Settings::gamma` at
//   1.0 while `Param::Gamma` ran to +-2^24. **Symptom: the readout keeps moving and the sound does
//   not.** This one is far worse, and it is the reason this file exists rather than a single case:
//   every automated check we own passed, the readout seemed to show gamma going higher —
//   the number read as believable — and the two halves of the defect sit in a header and
//   a .cpp that do not mention each other.
//
// **NEITHER CASE IS A DIFFERENCE CHECK.** The first reads the declaration and asserts numbers; the
// second asserts that a value put in comes back out. CLAUDE.md's rule, and it bites here in a
// specific way: "the device sounds different at gamma 5 than at gamma 1" would have been GREEN on the
// clamped build for two of the three probe values, because 5 and 1 both clamped to 1.0 but 0.5 did
// not. A partial effect surviving a difference check is exactly the shape that rule warns about.

using namespace feathers;

namespace {

using codecs::Codec;
using codecs::RowVec;

/// **ONE ENTRY PER REGISTERED DEVICE, AND THE WALK REQUIRES ONE FOR EVERY TAG IN `kDevices`.** That
/// requirement is what makes a hand-written table mechanical rather than a list somebody has to
/// remember: a device added to the registry with no entry here FAILS this suite, and the failure
/// names its tag.
///
/// **IT IS A TABLE AND NOT A REGISTRY WALK BECAUSE THE VOCABULARY HAS NO SEAM FOR IT.** `AudioOps`
/// and `ModOps` carry `setFields` — a device can be TOLD its row generically — and nothing anywhere
/// reads one back, so there is no generic road from a row to a row. Adding one to the ops structs
/// would be putting a test's needs into the device contract every third-party guest must implement,
/// for a check three lines of adapter buy. If a `getFields` ever arrives for a real reason, this
/// table collapses into a loop.
const Codec kCodecs[] = {
#include "codec_table.inc"
};

[[nodiscard]] RowVec (*codecFor(const char* tag))(const RowVec&) {
	for (const Codec& c : kCodecs) {
		if (device::sameTag(c.tag, tag)) {
			return c.trip;
		}
	}
	return nullptr;
}

[[nodiscard]] std::string nameOf(const device::Descriptor& d, size_t p) {
	const char* n = d.params[p].name;
	return std::string(d.traits.name) + " :: " + (n == nullptr ? "<unnamed>" : n);
}

/// **CONTROLS WHOSE DECODE IS DELIBERATELY NOT THE IDENTITY, WITH THE REASON.** Each of these was
/// found by running the case below and then reading the engine; none is a clamp against a wider
/// declaration, which is the thing the case exists to catch.
///
/// **AN EXEMPTION IS A CLAIM, SO IT IS PINNED BOTH WAYS**: the case fails on a listed control that
/// round-trips cleanly, exactly as it fails on an unlisted one that does not. A list that only ever
/// silenced failures would rot into a description of the code.
struct Exemption {
	const char* control;
	const char* why;
};

const Exemption kExemptions[] = {
	// **`Automod :: Rotate` WAS EXEMPT HERE AND IS NOT ANY MORE**, and the removal is the point rather
	// than housekeeping. The exemption read *"wrapped into [0, 2pi): a turn has no ends and the
	// endpoints coincide"*, resting on `settingsOf`'s claim that "a patch carrying 7 radians means the
	// same basis as one carrying 7 - 2pi". MEASURED 2026-09-08: false — `basisFor(0.4, 0.25)` and
	// `basisFor(0.4 + 2pi, 0.25)` differ 1.607 to 1.140, because the basis is phi triangles and phi is
	// irrational. The wrap was discarding planes, not identifying them; it is gone, the range is the
	// shaper's own window, and this control now round-trips like every other. **This case is what
	// noticed** — it asserts an exemption is still NEEDED, so a wrap removed without touching this
	// list fails here rather than leaving a lie in the table.
	// **THE THREE MSEG ENTRIES ARE A CROSS-CONTROL RELATIONSHIP AND NOT A CAP, AND THE CASE BELOW
	// PROVES IT RATHER THAN ASSERTING IT.** All three are declared `0 .. kMaxSegments - 1`, which is
	// honest — with eight segments, index 7 is a real index. `sanitize` then clamps them against the
	// LIVE `Segments`, and this sweep probes every control against the DEFAULT row, where `Segments`
	// is 3. So index 3 is refused because there is no segment 3 today, not because the decode is
	// narrower than the declaration.
	//
	// `mseg.cpp` says exactly this at the clamp: *"THE RELATIONSHIPS THE DECLARATIONS CANNOT STATE...
	// they are here rather than in the declarations because a range is a fact about ONE control."*
	// An exemption that only quoted that would be taking the code's word for it; the companion case
	// raises `Segments` to its own declared maximum and shows all three round-trip at 3 AND at 7.
	{ "MSEG :: Sustain At", "bounded by the live Segments, not by a cap — see the companion case" },
	{ "MSEG :: Loop From", "bounded by the live Segments, not by a cap — see the companion case" },
	{ "MSEG :: Loop To", "bounded by the live Segments, not by a cap — see the companion case" },
	// **NOT DECODED AT ALL BY THE DEVICE.** A modulator is switched by the host through
	// `Traits::enableControl`; `Tracker::setFields` never reads this field, so there is no decode here
	// to be narrower than anything. See the note on the test-only accessors in `tracker.h`.
	{ "Tracker :: Enabled", "the device does not read it; the host switches a modulator itself" },
	{ "Tracker :: Amount", "the device does not read it; the host scales a modulator's outputs (backlog DH)" },
	{ "Note Expressions :: Amount", "the device does not read it; the host scales a modulator's outputs (backlog DH)" },
	// A MACRO'S TYPE is how the card's control writes the value (turn, toggle, hold); what reaches the
	// device is the value, so it never decodes the type. Kept in the row so a patch keeps it.
	{ "Macros :: Type 1", "the device does not read it; the card's control drives the value by it" },
	{ "Macros :: Type 2", "the device does not read it; the card's control drives the value by it" },
	{ "Macros :: Type 3", "the device does not read it; the card's control drives the value by it" },
	{ "Macros :: Type 4", "the device does not read it; the card's control drives the value by it" },
	{ "Macros :: Enabled", "the device does not read it; the host switches a modulator itself" },
	{ "Math :: Enabled", "the device does not read it; the host switches a modulator itself" },
	{ "Stereo Tool :: Enabled", "the device does not read it; the host switches a modulator itself" },
	// THE CHAIN FURNITURE (backlog NR): the slot blends these around the device, which hands back the wet,
	// so the device's own codec never reads them. They sit in the row so a patch keeps them.
	{ "Waypoint :: Waypoint Mix", "the slot applies it; the device does not read it" },
	{ "Waypoint :: Waypoint Wet Gain", "the slot applies it; the device does not read it" },
	{ "Bus Utility :: Bus Mix", "the slot applies it; the device does not read it" },
	{ "Bus Utility :: Bus Wet Gain", "the slot applies it; the device does not read it" },
	{ "EQ :: EQ Mix", "the slot applies it; the device does not read it" },
	{ "EQ :: EQ Wet Gain", "the slot applies it; the device does not read it" },
	{ "Disperser :: Disperser Mix", "the slot applies it; the device does not read it" },
	{ "Disperser :: Disperser Wet Gain", "the slot applies it; the device does not read it" },
	{ "Clipper :: Clip Mix", "the slot applies it; the device does not read it" },
	{ "Clipper :: Clip Wet Gain", "the slot applies it; the device does not read it" },
	{ "Drawn :: Drawn Mix", "the slot applies it; the device does not read it" },
	{ "Drawn :: Drawn Wet Gain", "the slot applies it; the device does not read it" },
	{ "Harm :: Harm Mix", "the slot applies it; the device does not read it" },
	{ "Harm :: Harm Wet Gain", "the slot applies it; the device does not read it" },
};

[[nodiscard]] const Exemption* exemptionFor(const std::string& control) {
	for (const Exemption& e : kExemptions) {
		if (control == e.control) {
			return &e;
		}
	}
	return nullptr;
}

} // namespace

//--------------------------------------------------------------------------------------------//
// THE WALL — a declaration that ends where the engine does not                                 //
//--------------------------------------------------------------------------------------------//

TEST_CASE("MSEG's loop and sustain are bounded by the live segment count, not by a hidden cap") {
	// **THE COMPANION TO THREE ENTRIES ON THE SWEEP'S EXEMPTION LIST, and the reason they are
	// exemptions rather than findings.** `Sustain At`, `Loop From` and `Loop To` all declare
	// `0 .. kMaxSegments - 1` and all three refuse index 3 at the default row — which is the exact
	// signature of the gamma defect, a decode narrower than its declaration.
	//
	// IT IS NOT THAT. `sanitize` clamps them against the LIVE `Segments`, which the default row puts
	// at 3, so index 3 does not exist yet. **Raising `Segments` to its own declared maximum must make
	// every one of them reachable** — and if it does not, the exemption above is hiding a real cap and
	// this case is what says so.
	namespace ms = mod::mseg;
	constexpr auto kAll = static_cast<size_t>(ms::Param::kCount);

	const auto roundTrip = [](size_t segments, ms::Param p, float v) {
		ms::Row row {};
		for (size_t i = 0; i < kAll; ++i) {
			row[i] = static_cast<float>(ms::kParamDescs[i].def);
		}
		row[static_cast<size_t>(ms::Param::SegmentCount)] = static_cast<float>(segments);
		row[static_cast<size_t>(p)] = v;
		return ms::rowOf(ms::settingsOf(row, kAll))[static_cast<size_t>(p)];
	};

	const ms::Param loops[] = { ms::Param::SustainSegment, ms::Param::LoopStart, ms::Param::LoopEnd };
	for (const ms::Param p : loops) {
		CAPTURE(std::string(ms::kParamDescs[static_cast<size_t>(p)].name));
		const double declaredMax = ms::kParamDescs[static_cast<size_t>(p)].max;
		REQUIRE(declaredMax == static_cast<double>(ms::kMaxSegments) - 1.0);

		// **AT THE FULL SEGMENT COUNT, THE WHOLE DECLARED RANGE SURVIVES** — including the declared
		// maximum, which is the value a cap would land on.
		CHECK(roundTrip(ms::kMaxSegments, p, 3.0f) == doctest::Approx(3.0f));
		CHECK(roundTrip(ms::kMaxSegments, p, static_cast<float>(declaredMax))
		      == doctest::Approx(declaredMax));

		// AND THE OTHER HALF, so this is not the vacuous reading that everything always passes: at
		// three segments the same value IS refused, which is what the sweep saw.
		CHECK_FALSE(roundTrip(3, p, 3.0f) == doctest::Approx(3.0f));
	}

	// `Loop To` NEEDS `Loop From` BELOW IT, and the clamp says so — `loopEnd` is clamped to
	// `[loopStart, count - 1]`. Checked here because the loop above moves one control at a time and
	// would read a `Loop To` refused for the OTHER reason as the same fact.
	{
		ms::Row row {};
		for (size_t i = 0; i < kAll; ++i) {
			row[i] = static_cast<float>(ms::kParamDescs[i].def);
		}
		row[static_cast<size_t>(ms::Param::SegmentCount)] = static_cast<float>(ms::kMaxSegments);
		row[static_cast<size_t>(ms::Param::LoopStart)] = 5.0f;
		row[static_cast<size_t>(ms::Param::LoopEnd)] = 2.0f;
		const ms::Settings s = ms::settingsOf(row, kAll);
		CHECK(s.loopStart == 5u);
		CHECK(s.loopEnd == 5u); // pulled up to the start, not down to 2
	}
}

//--------------------------------------------------------------------------------------------//
// THE CLAMP — a decode narrower than the declaration it serves                                 //
//--------------------------------------------------------------------------------------------//

TEST_CASE("every device's row codec carries the whole of every control's declared range") {
	// **THE MECHANICAL FORM OF THE GAMMA DEFECT, ASKED OF ALL 220-ODD CONTROLS.** A declaration says
	// what a host may send and what a knob may reach; the decode is what the DSP is handed. When they
	// disagree the READOUT moves and the SOUND does not, and nothing in either file mentions the
	// other. The claim here is the narrowest one that catches it: a value inside `[min, max]` comes
	// back as itself.
	//
	// **STEPPED CONTROLS ARE PROBED ONLY AT THEIR STEPS.** A switch handed 0.3 correctly answers 0,
	// and probing between steps would make this case fail on every choice in the registry — a bar so
	// noisy nobody could read a real failure off it.
	const auto tolerance = [](double v) {
		// SCALED, because a phase counter's range runs to 2^24 where a float's ULP is 2, and a fixed
		// epsilon would report the top of every unbounded declaration as a clamp. Not a way of
		// excusing a narrowing: a clamp lands on the BOUND, which is nowhere near one ULP away.
		return std::max(1e-5, std::fabs(v) * 1e-6);
	};

	int examined = 0;
	int devices = 0;
	for (const device::Descriptor& d : kDevices) {
		++devices;
		CAPTURE(std::string(d.traits.name));
		// **EVERY REGISTERED DEVICE OR THE SUITE FAILS.** This is the line that makes the table above
		// mechanical: a device cannot join the registry and quietly escape the sweep.
		RowVec (*trip)(const RowVec&) = codecFor(d.traits.tag);
		REQUIRE(trip != nullptr);

		// THE DEFAULT ROW, so every control but the one under test sits where the device declares it.
		RowVec defaults(d.params.size(), 0.0f);
		for (size_t i = 0; i < d.params.size(); ++i) {
			defaults[i] = static_cast<float>(d.params[i].def);
		}

		for (size_t p = 0; p < d.params.size(); ++p) {
			const device::ParamDesc& desc = d.params[p];
			const std::string control = nameOf(d, p);
			CAPTURE(control);
			const Exemption* exempt = exemptionFor(control);

			std::vector<double> probes;
			if (desc.stepped) {
				for (double v = std::round(desc.min); v <= desc.max; v += 1.0) {
					probes.push_back(v);
				}
			}
			else {
				// THE ENDS AND THE INTERIOR. The ends are where a clamp lands and the interior is what
				// proves a clamp was not simply moved: the shaper's gamma clamped 5 and 1 to the same
				// 1.0 while leaving 0.5 alone, so a probe set of interior points ALONE would have
				// reported it clean.
				for (const double t : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
					probes.push_back(desc.min + (desc.max - desc.min) * t);
				}
			}

			bool allSurvived = true;
			for (const double v : probes) {
				RowVec in = defaults;
				in[p] = static_cast<float>(v);
				const RowVec out = trip(in);
				REQUIRE(out.size() > p);
				const double want = static_cast<double>(in[p]);
				const double got = static_cast<double>(out[p]);
				if (std::fabs(got - want) > tolerance(want)) {
					allSurvived = false;
					if (exempt == nullptr) {
						// THE VALUE, NAMED. A failure here must say which control and which number, or
						// it is a report that something is wrong somewhere.
						CAPTURE(want);
						CAPTURE(got);
						CHECK_MESSAGE(got == doctest::Approx(want).epsilon(1e-6),
						              "decode narrowed " << control << " inside its declared range");
					}
					break;
				}
			}
			++examined;

			// **THE EXEMPTION LIST IS PINNED BOTH WAYS.** A listed control that round-trips cleanly is
			// a list that has gone stale, and a stale exemption is a hole nobody can see.
			if (exempt != nullptr) {
				CAPTURE(std::string(exempt->why));
				CHECK_FALSE(allSurvived);
			}
		}
	}

	// FLOORS, so a walk that examined nothing cannot pass by doing nothing.
	CAPTURE(devices);
	CAPTURE(examined);
	CHECK(devices >= 10);
	CHECK(examined >= 200);
}

// **THE STORED-MEANING FINGERPRINT (backlog DR) WAS HERE, AND IS DELETED (backlog JF step 1d, 2026-09-25).**
// A patch names every value by key now, so a control's POSITION no longer carries its meaning; what does
// is its key's unit, range, steps and labels, which `test_state_ledger.cpp` checks line by line against
// `tests/state_schema.ledger` — including the rescale this digest could not see. See `state_block_order.h`.
