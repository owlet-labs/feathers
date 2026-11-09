/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/decisions.h"
#include "fx/sklitter/sklitter_types.h"

#include <iterator>
#include <string>
#include <vector>

using namespace feathers::fx::sklitter;

TEST_CASE("the first four params keep the reference's order, less the two that are deleted") {
	// **THE ASSERTION THAT CATCHES A WELL-MEANING TIDY-UP.** The reference's `processStutter` takes a
	// `modulatedValues` array indexed [0]=ZONE_A [1]=ZONE_B [2]=MACRO_CONFIG [3]=MACRO [4]=PWRITE
	// [5]=DENSITY. Sort these alphabetically, or "group the macros together", and every modulated
	// value lands on the wrong control -- with no crash, no warning, and nothing else in this suite
	// going red.
	//
	// **IT ASKED ABOUT SIX AND ASKS ABOUT FOUR, because MACRO_CONFIG and MACRO ARE DELETED (2026-09-19)**
	// — the perform circle's centre is a zero push and a configuration with no macro goes with it. The
	// four that remain keep their relative order, which is the part of the parity that still means
	// something; the two absent indices are asserted as absent by the row being 24 wide, below.
	CHECK(static_cast<int>(Param::ZoneA) == 0);
	CHECK(static_cast<int>(Param::ZoneB) == 1);
	CHECK(static_cast<int>(Param::PWrite) == 2);
	CHECK(static_cast<int>(Param::Density) == 3);
}

TEST_CASE("the tag is four bytes and the name is the one that renders") {
	// The field is `device::kTagBytes` wide on the wire; a tag longer than that truncates on write
	// and mismatches on read, silently, surfacing only as a patch that will not load. It was four
	// bytes until it was widened on 2026-09-14, which is why this used to assert "SCAT".
	CHECK(std::string_view(kTraits.tag).size() <= feathers::device::kTagBytes);
	CHECK(std::string_view(kTraits.tag) == "SKLITTER");
	CHECK(std::string_view(kTraits.name) == "SKLITTER");
	// THE TAG AND THE NAME AGREE NOW, and that is the point of the widening.
	CHECK(std::string_view(kTraits.tag) == std::string_view(kTraits.name));
}

TEST_CASE("enableControl is the BYPASS, not the gesture") {
	// Point this at Engage and the card's enable square silently becomes a stutter trigger: the user
	// bypasses the device and instead starts stuttering. Both are 0/1 controls, so nothing type-checks
	// the difference.
	CHECK(kTraits.enableControl == static_cast<int>(Param::Enabled));
	CHECK(kTraits.enableControl != static_cast<int>(Param::Engage));

	// And they default oppositely, which is the other half of the distinction.
	CHECK(kParamDescs[static_cast<size_t>(Param::Enabled)].def == 1.0);
	CHECK(kParamDescs[static_cast<size_t>(Param::Engage)].def == 0.0);
}

TEST_CASE("every control declares itself, and the descriptors line up with the enum") {
	REQUIRE(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));
	// 19 UNTIL THE PERFORM CIRCLE APPENDED ELEVEN (30), AND 24 SINCE THE MACROS WENT (2026-09-19) — the
	// `Macro Cfg` and `Macro` knobs and the four waypoint macros, six controls deleted in one change
	// because the circle's centre is a zero push and a waypoint is a configuration at macro 1.0. The
	// literal is the point of the assertion: it is what makes a control added or lost a deliberate edit
	// here rather than a number that follows silently.
	// **25 SINCE `Send Override` (2026-09-19, backlog EW)** — the delay send's always/never/phi knob,
	// INSERTED after `Mix` rather than appended, so it is a knob among the knobs and the perform block
	// stays contiguous at the end. `kStateVersion` moved to 17 with it; `state_block_order.h` carries
	// the argument for why an insert costs the byte and an append does not.
	// **26 SINCE `Transport Locked` (2026-09-21, backlog GT)** — the switch that hands playback to the
	// host's transport, INSERTED at 15 beside the other two loop settings rather than appended after the
	// perform circle. `kStateVersion` moved to 19 with it, for the same reason `Send Override` cost 17.
	// **29 SINCE THE FEEDBACK CIRCUIT (2026-09-21, backlog GU/GV)** — `Dry Write`, `Feedback` and
	// `FB Tap`, INSERTED after `Send Override` because they are knobs and the knobs run to there.
	// `kStateVersion` moved to 20 with them.
	// **30 SINCE `Grain Order` (2026-09-21, backlog HU)** — an AUDITION control, so this literal is
	// expected to go back to 29 the day it is decided and deleted.
	// **31 SINCE `Groove` (2026-09-22, backlog IG)** — INSERTED before the perform block, which moved one
	// column down. `kStateVersion` moved to 28 with it.
	// **32 SINCE `Groove` SPLIT into `Groove Mode` and `Groove Amount` (2026-09-23, backlog IO)** — the perform
	// block one further down. `kStateVersion` moved to 31 with it.
	// **33 SINCE `Free Phase`** (backlog IO), inserted after `Groove Amount`; `kStateVersion` 32.
	// **34 SINCE `Input Gain` (backlog KZ, 2026-10-10)**, APPENDED after the perform block; the row is keyed now (the
	// schema ledger), so no `kStateVersion` move.
	// **35 SINCE `Freeze` (backlog KX, 2026-10-10)**, appended after `Input Gain`.
	// **36 SINCE `Record Gate` (backlog MJ, 2026-10-17)**, appended after `Freeze`.
	// **35 SINCE `Direction` WAS REMOVED (2026-11-09)**; its key is retired.
	CHECK(static_cast<size_t>(Param::kCount) == 35u);

	for (size_t i = 0; i < std::size(kParamDescs); ++i) {
		CAPTURE(i);
		const auto& d = kParamDescs[i];
		CHECK(d.name != nullptr);
		CHECK(d.tooltip != nullptr); // a control with no tooltip is a control nobody can explain
		CHECK(d.min <= d.def);
		CHECK(d.def <= d.max);
		// A Choice must carry names, and a non-Choice must not pretend to.
		if (d.unit == feathers::device::Unit::Choice) {
			CHECK(d.names.size() > 0u);
			CHECK(d.stepped == true);
			CHECK(d.max == static_cast<double>(d.names.size() - 1));
		}
	}
}

TEST_CASE("the mode names come from the enum, and a stored Classic loads as Burst") {
	REQUIRE(std::size(kModeNames) == 7u);
	CHECK(std::string_view(kModeNames[0]) == "Burst");
	CHECK(std::string_view(kModeNames[1]) == "Repeat");
	CHECK(std::string_view(kModeNames[4]) == "Grain");
	CHECK(std::string_view(kModeNames[5]) == "Pattern");
	CHECK(std::string_view(kModeNames[6]) == "Pitch");
	const auto& mode = kParamDescs[static_cast<size_t>(Param::Mode)];
	CHECK(mode.max == 6.0);
	REQUIRE(mode.choiceAliases.size() == 1u);
	CHECK(std::string_view(mode.choiceAliases[0].from) == "Classic");
	CHECK(std::string_view(mode.choiceAliases[0].to) == "Burst");

	CHECK(static_cast<int>(Mode::Burst) == 0);
	CHECK(static_cast<int>(Mode::Repeat) == 1);
	CHECK(static_cast<int>(Mode::Grain) == 4);
	CHECK(static_cast<int>(Mode::Pattern) == 5);
	CHECK(static_cast<int>(Mode::Pitch) == 6);
}

TEST_CASE("the zones are unbounded coordinates and their names are deliberately empty") {
	// None of the zones is bounded, by design. A zoned control is one infinite line with
	// eight named regions on it, which is what offsetsMayLeaveRange plus zones/modSpan express.
	for (const Param p : { Param::ZoneA, Param::ZoneB }) {
		CAPTURE(static_cast<int>(p));
		const auto& d = kParamDescs[static_cast<size_t>(p)];
		CHECK(d.offsetsMayLeaveRange == true);
		CHECK(d.zones == static_cast<uint32_t>(kZones));
		CHECK(d.modSpan == kOnePhaseUnit);
		CHECK(d.modulatable == true);
		CHECK(d.unit == feathers::device::Unit::Number); // a coordinate is not a percentage
	}

	// Zone names would mean nothing in most modes, so every entry is
	// empty ON PURPOSE -- ParamDesc carries one name list per control and Sklitter's zones are read
	// differently by each mode, so a name would be right for at most one and misleading in seven.
	// This case exists so that emptiness reads as a decision; delete it the day a region is measured.
	REQUIRE(std::size(kZoneAZoneNames) == 8u);
	REQUIRE(std::size(kZoneBZoneNames) == 8u);
	for (int z = 0; z < kZones; ++z) {
		CAPTURE(z);
		CHECK(std::string_view(kZoneAZoneNames[z].name).empty());
		CHECK(std::string_view(kZoneBZoneNames[z].name).empty());
	}
}

TEST_CASE("isLooperMode, and what is left of the reference's latch rule") {
	// **THE PREDICATE SURVIVES; NOTHING CONSULTS IT ANY MORE.** The reference latches every looper
	// mode and never Burst. That is still the honest description of the
	// REFERENCE and is pinned so the port can be read against its source — but `EngageMachine` stopped
	// asking, because latch exists to turn a MOMENTARY gesture into a sustained one and a DAW
	// parameter is already sustained. See `engage.h`. The toggle went with it.
	Settings s;
	SUBCASE("Burst is not a looper mode") {
		s.mode = Mode::Burst;
		CHECK(s.isLooperMode() == false);
		CHECK(s.isLatched() == false);
	}
	SUBCASE("every other mode runs on the looper buffer") {
		for (const Mode m : { Mode::Repeat, Mode::Time, Mode::Shuffle, Mode::Grain, Mode::Pattern,
		                      Mode::Pitch }) {
			CAPTURE(static_cast<int>(m));
			s.mode = m;
			CHECK(s.isLooperMode() == true);
			CHECK(s.isLatched() == true);
		}
	}
}

TEST_CASE("stateOf and rowOf round-trip") {
	Settings s;
	s.zoneA = 3.25f;
	s.zoneB = -1.5f; // a coordinate may be negative; it is a line, not a range
	s.pWrite = 0.2f;
	s.density = 0.9f;
	s.rate = 0.4f;
	s.enabled = false;
	s.engage = true;
	s.mode = Mode::Shuffle;
	s.quantized = false;
	s.gamma = 0.4f;
	s.pitchScale = 5.0f;
	s.stereoWidth = 0.8f;
	// **BIPOLAR, AND THE NEGATIVE HALF IS THE HALF WORTH ROUND-TRIPPING.** A codec that clamped or
	// rectified on the way through would still return 0.6 for 0.6; -0.6 is what catches it, and it is
	// also the end that means NEVER, which is the promise a lost sign would quietly break.
	s.sendOverride = -0.6f;

	const Settings back = stateOf(rowOf(s));
	CHECK(back.zoneA == s.zoneA);
	CHECK(back.zoneB == s.zoneB);
	CHECK(back.pWrite == s.pWrite);
	CHECK(back.density == s.density);
	CHECK(back.rate == s.rate);
	CHECK(back.enabled == s.enabled);
	CHECK(back.engage == s.engage);
	CHECK(back.mode == s.mode);
	CHECK(back.quantized == s.quantized);
	// AND THE TWO BOOLEANS THE DSP READS COME BACK WITH IT, which is the point of the collapse: one
	// control, four combinations, derived rather than stored twice.
	CHECK(back.gamma == s.gamma);
	CHECK(back.pitchScale == s.pitchScale);
	CHECK(back.stereoWidth == s.stereoWidth);
	CHECK(back.sendOverride == s.sendOverride);
}

TEST_CASE("the perform controls round-trip through the row, every one of them") {
	// `cornerMacro` was set and checked here too, and is deleted (2026-09-19): a waypoint is its
	// configuration, built at macro 1.0, with the puck's radius supplying the amount.
	Settings s;
	s.cornerCfg = {1.5f, -2.25f, 0.125f, 7.75f};
	s.perfX = -0.4f;
	s.perfY = 0.65f;
	s.perfDepth = 0.3f;
	const Settings back = stateOf(rowOf(s));
	CHECK(back.cornerCfg == s.cornerCfg);
	CHECK(back.perfX == s.perfX);
	CHECK(back.perfY == s.perfY);
	CHECK(back.perfDepth == s.perfDepth);

	// **THE TWO CLAMP CONTRACTS, ASSERTED RATHER THAN ONLY STATED.** `stateOf` says in strong terms that
	// the puck is deliberately NOT clamped there — `performWeightsOf` owns the disc and the de-NaN, and a
	// second road to one rule is how two roads come to disagree — and that depth IS clamped, the way
	// `mix` is. Every value above is in range, so a tidy-up ADDING a clamp to the puck, or DELETING
	// depth's, left this case green and the comments were the only thing holding either contract. The
	// `cornerCfg` line above already does exactly this job for the coordinate, which is where the shape
	// is borrowed from: an out-of-range value is the only kind that can tell a clamp from its absence.
	Settings wide;
	wide.perfX = 3.0f; // past the rim on purpose
	wide.perfY = -2.5f;
	const Settings wideBack = stateOf(rowOf(wide));
	CHECK(wideBack.perfX == 3.0f);
	CHECK(wideBack.perfY == -2.5f);

	Settings over;
	over.perfDepth = 2.0f;
	CHECK(stateOf(rowOf(over)).perfDepth == 1.0f);
	Settings under;
	under.perfDepth = -1.0f;
	CHECK(stateOf(rowOf(under)).perfDepth == 0.0f);
}

TEST_CASE("every declared default agrees with the struct's, all twenty-five and not only the circle's seven") {
	// **THE HOUSE FORM IS THE WIDE ONE** — `test_fx_harm.cpp`'s "every declared default agrees with the
	// struct's, not just the five with a static_assert", with matching cases in `test_fx_automod.cpp` and
	// `test_mod_mseg.cpp`. The first draft of this walked the circle's block alone, asking the question
	// only of the controls one commit happened to add — and a default that drifts is no likelier in new
	// code. (The `MacroN` it started from is itself deleted now, which is its own argument.)
	// **The ones most able to drift here are the OLD ones**, because four of them are not stored as the
	// control's own value at all.
	//
	// **WHICH IS ALSO WHY THE COMPARISON CARRIES NO ARITHMETIC.** Min Env is LogSeconds in the descriptor
	// and SECONDS in the field; Seq Length and Buffer Length are ladder INDICES against bars; Mode is an
	// enumerator. `rowOf` is the device's own indexed view of itself and already holds every one of those
	// mappings, in production, where `stateOf` inverts them — so `rowOf(Settings {})` against the declared
	// defaults asks the question in ONE unit by construction. A test that converted for itself would be a
	// second copy of the thing under test, which is the trap `test_fx_harm.cpp` records falling into.
	constexpr size_t kCount = static_cast<size_t>(Param::kCount);
	static_assert(std::size(kParamDescs) == kCount, "the walk below is driven from kCount");

	Row declared {};
	for (size_t i = 0; i < kCount; ++i) {
		declared[i] = static_cast<float>(kParamDescs[i].def);
	}
	const Row fresh = rowOf(Settings {});

	CHECK(fresh == declared); // the whole row at once, so nothing escapes

	// AND EACH ONE BY NAME, so a failure says WHICH rather than only THAT.
	std::vector<std::string> disagreed;
	for (size_t i = 0; i < kCount; ++i) {
		if (fresh[i] != declared[i]) {
			disagreed.push_back(std::string(kParamDescs[i].name) + ": struct " + std::to_string(fresh[i])
			                    + " vs declared " + std::to_string(declared[i]));
		}
	}
	for (const std::string& s : disagreed) {
		MESSAGE("DEFAULT DISAGREES: " << s);
	}
	CHECK(disagreed.empty());

	// **NON-VACUITY, AND IT IS NOT DECORATION.** If `rowOf` returned zeros, or every declared default were
	// zero, every assertion above would hold while saying nothing at all.
	int nonZero = 0;
	for (size_t i = 0; i < kCount; ++i) {
		if (declared[i] != 0.0f) {
			++nonZero;
		}
	}
	CAPTURE(nonZero);
	CHECK(nonZero >= 4);

	// AND DEPTH BY NAME ON TOP OF THE SWEEP, because 1.0 is the one default in the circle that is a
	// DECISION rather than a placeholder: the waypoints above it are provisional and chosen by ear later,
	// while this one is argued from the centre being neutral (design §2.2). A sweep that only says "the
	// two tables agree" would stay green if both moved to 0 together.
	CHECK(stateOf(fresh).perfDepth == 1.0f);
}

TEST_CASE("a Mode out of range lands on a real mode rather than past the end") {
	// Automation and modulation can both hand over a value outside the declared range. An index past
	// the end reads a mode that does not exist -- and `Mode::kCount` is a valid float to arrive.
	Row row = rowOf(Settings {});
	row[static_cast<size_t>(Param::Mode)] = 99.0f;
	CHECK(static_cast<int>(stateOf(row).mode) == 6);
	row[static_cast<size_t>(Param::Mode)] = -5.0f;
	CHECK(static_cast<int>(stateOf(row).mode) == 0);
	row[static_cast<size_t>(Param::Mode)] = 7.0f; // exactly kCount
	CHECK(static_cast<int>(stateOf(row).mode) == 6);
	// And it ROUNDS rather than truncating, so a host's 3.9 is Grain and not Shuffle.
	row[static_cast<size_t>(Param::Mode)] = 3.9f;
	CHECK(stateOf(row).mode == Mode::Grain);
}

TEST_CASE("the pitch scale list indexes the table it names") {
	// kPitchScaleNames is what the UI shows; kScaleSemitones is what the DSP reads. They are declared
	// in different files and nothing but this pins them together.
	CHECK(std::size(kPitchScaleNames) == 25u);
	CHECK(std::string_view(kPitchScaleNames[0]) == "Chromatic");
	CHECK(std::string_view(kPitchScaleNames[24]) == "+13");
	// Row 24 of the semitone table is a constant +13, which is what its name promises.
	for (int col = 0; col < 8; ++col) {
		CAPTURE(col);
		CHECK(kScaleSemitones[24][col] == 13);
	}
	CHECK(kParamDescs[static_cast<size_t>(Param::PitchScale)].max == 24.0);
}
