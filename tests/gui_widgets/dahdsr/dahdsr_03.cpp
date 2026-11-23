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

// PIECE 3 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

namespace {
/// EVERY CONTROL OF `desc` A FRAME TREE REACHES — every widget under `root`, shown or not (a gate hides a control; it
/// does not unseat it), asked what it offers (`ui::ControlFace`), by descriptor identity. For a composed window.
std::set<size_t> reachedUnder(const visage::Frame* root, const feathers::device::Descriptor& desc) {
	std::set<size_t> out;
	std::vector<ui::ControlFace::Offered> list;
	std::vector<const visage::Frame*> stack { root };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr); face != nullptr && fr != root) {
			list.clear();
			face->offeredControls(list);
			for (const ui::ControlFace::Offered& o : list) {
				for (size_t i = 0; o.control != nullptr && i < desc.params.size(); ++i) {
					if (&o.control->desc() == &desc.params[i]) {
						out.insert(i);
					}
				}
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	return out;
}
} // namespace

TEST_CASE("every control a DEVICE declares has something on one of its surfaces that reaches it") {
	// THE DIRECTION THE HOST-PARAMETER GUARD ABOVE IS BLIND TO, AND SegNShape IS THE PROOF.
	//
	// "every declared parameter a hand should be able to reach has a control that reaches it" walks
	// `paramDescs()` and asks the widget registry for watchers — which works because a host parameter
	// has a clap_id and a widget registers under it. A DEVICE control has neither. It is addressed by
	// its index in its own descriptor, no widget registers for it, and `watchers()` correctly answers
	// empty for every one of them. So the whole device vocabulary — the filter's twenty, HARM's ten,
	// the envelope's fifteen, the MSEG's thirty-seven — sits outside that walk entirely.
	//
	// WHICH IS HOW `SegNShape` SHIPPED. Declared eight times over, carried in the row, read by the
	// engine's `warp`, and reachable by nothing at all — found by hand on the installed build rather than by any of
	// twenty-six suites. This case is deliberately in the same commit as the fix, because shipping
	// curvature and THEN adding the check that would have found it missing is the wrong order.
	//
	// WHAT "REACHES" MEANS HERE, and why it is asked of the CARD. A device control is reached by a
	// value box, a chip, or a GESTURE — and a gesture is invisible to a frame walk. `FilterCard` set
	// the pattern with `addressedParamsForTest`; `EnvCard` and `MsegCard` grew one for this case. The
	// card's claim is not taken on trust: every gesture named in those lists has a case in this file
	// that asserts the VALUE it writes, which is what stops the list being a wish.
	FilterFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;

	// SLOT 0 KEEPS ITS ENVELOPE AND SLOT 1 TAKES AN MSEG, so each card is built over a rack row of
	// the kind it draws. A card reading another device's row would still answer this question, but it
	// would be answering it from a state no editor ever holds.
	// SEATED, not merely written: a count that reaches it and an identity on it. This wrote the
	// row past the rack's count, which the flattened channel stored and the engine never ran; the
	// bridge's record holds what the rack HAS, so the fixture now says the rack has it.
	// (The identity was `{ id, deviceSlotOfRack(1) }` until kill-lanes step 3c; a device's identity is
	// its id and nothing else now, and its place is the slot it is written to.)
	feathers::ModRack rack = f.bridge.modRack();
	rack.setMsegAt(1, mseg::Settings {});
	rack.setIdentityAt(1, { rack.nextInstanceId });
	rack.nextInstanceId = rack.nextInstanceId + 1;
	rack.count = 2;
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, dsr::kTraits.tag));
	REQUIRE(f.bridge.modRack().holds(1, mseg::kTraits.tag));

	ui::EnvCard envCard { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	ui::MsegCard msegCard { f.deviceHost, f.bridge.rackDeviceAt(1), mod, history };

	// AND HARM, WHICH NOW HAS A CARD. This block used to assert the opposite — that the lane built a
	// `NoEditorCard` for HARM, because nothing on screen reached its ten controls — and it was
	// written to go RED the day `HarmCard` landed rather than to rot quietly. It landed, this went
	// red on the merge, and the exception below came out. That is the case doing its job, so it is
	// INVERTED rather than deleted: if HARM ever loses its card the lane falls back to a
	// `NoEditorCard` and this line says so, instead of the entry below silently passing on an empty
	// `reached` set.
	const size_t harmSlot = f.bridge.addFxDevice(hrm::kTraits.tag);
	REQUIRE(harmSlot == 1);
	f.lane->resized();
	REQUIRE(f.lane->cardAt(harmSlot) != nullptr);
	CHECK(dynamic_cast<ui::MissingDeviceCard*>(f.lane->cardAt(harmSlot)) == nullptr);
	auto* harmCard = dynamic_cast<ui::HarmCard*>(f.lane->cardAt(harmSlot));
	REQUIRE(harmCard != nullptr);

	struct Entry {
		const char* tag;
		std::set<size_t> reached;
		/// Declared, drawn perhaps, and reached by NOTHING — each named with its reason. Pinned both
		/// ways below, so an entry that gains a control fails as loudly as a control that loses one.
		std::map<size_t, const char*> knownGaps;
		/// True when this build has no editor for the device at all, in which case `reached` is empty
		/// by construction and the per-control loop would say the same thing thirty times.
		bool noEditorInThisBuild = false;
	};

	std::vector<Entry> table;

	// THE WAYPOINT FILTER, REACHED BY ITS OWN CARD. It had the generic one for a commit — a knob per
	// declared control, honest but ugly — and `WaypointCard` replaced it in phase 3b.
	//
	// NOT `noEditorInThisBuild`, which was available and would have been the quick way through at
	// either stage. That flag means nothing reaches any control, and an entry carrying it keeps
	// passing no matter what a card fails to reach — so the shortcut costs the table its job.
	//
	// AND THE SET IS ASKED OF THE CARD, not restated. `addressedParamsForTest()` is the card's own
	// claim, and the cases below assert the VALUE each gesture it names actually writes, which is what
	// stops the claim being a wish.
	{
		const size_t waySlot = f.bridge.addFxDevice(fx::waypoint::kTraits.tag);
		REQUIRE(waySlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* wayCard = dynamic_cast<ui::WaypointCard*>(f.lane->cardAt(waySlot));
		REQUIRE(wayCard != nullptr);

		Entry e { fx::waypoint::kTraits.tag, {}, {}, false };
		for (const fx::waypoint::Param p : wayCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
	}

	// THE DRAWN-CURVE FILTER, ACROSS BOTH ITS SURFACES — and this entry used to be mostly GAPS.
	//
	// It carried 96 of them, one per pole and zero position, each naming phase 4, because the card
	// deliberately does not edit bands. **Phase 4 built the editor and every one of those gaps closed
	// in the same change.** The entry is pinned both ways, so had the panel landed and this list stayed
	// as it was, the ninety-six would have flipped from gap to reached and this case would have failed
	// — which is the mechanism working rather than a chore, and it is why the gap list was written
	// with a reason rather than a shrug.
	//
	// THE TWO SURFACES ARE MERGED, because the guard asks whether SOMETHING reaches a control. The
	// card owns the seven globals and the panel owns the ninety-six; neither claims the other's, and
	// the totals below assert exactly that split rather than only the sum.
	{
		const size_t drawnSlot = f.bridge.addFxDevice(fx::drawn::kTraits.tag);
		REQUIRE(drawnSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* drawnCard = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(drawnSlot));
		REQUIRE(drawnCard != nullptr);

		// THE PANEL BUILT THE WAY THE EDITOR BUILDS IT, through `makeExpandedView`, so this also
		// asserts the entry table has an arm rather than constructing the class behind its back.
		ui::WidgetRegistry drawnRegistry;
		auto drawnView = ui::makeExpandedView(f.deviceHost, &f.bridge, drawnRegistry, f.bridge.fxDeviceAt(drawnSlot),
		                                      fx::drawn::kDescriptor);
		REQUIRE(drawnView != nullptr);
		auto* drawnPanel = dynamic_cast<ui::DrawnExpanded*>(drawnView.get());
		REQUIRE(drawnPanel != nullptr);

		Entry e { fx::drawn::kTraits.tag, {}, {}, false };
		for (const fx::drawn::Param p : drawnCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		const size_t fromCard = e.reached.size();
		for (const size_t index : drawnPanel->addressedParamsForTest()) {
			e.reached.insert(index);
		}

		// THE SPLIT, NOT ONLY THE TOTAL. Seven from the card and a hundred and twenty from the panel,
		// and the two sets do not overlap — a panel that claimed a global, or a card that claimed a
		// band field, would still add to the total and would be a lie about which surface reaches
		// what.
		//
		// IT WAS NINETY-SIX UNTIL THE TYPE BECAME STATE (2026-08-29). The panel's count is
		// `kFrames * kBands * kBandFields` and is written that way rather than as a literal, because
		// a literal here is the same number the device already states and the two drifted the moment
		// a band grew a fifth field — which is what this case caught.
		CHECK(fromCard == 7);
		CHECK(drawnPanel->addressedParamsForTest().size()
		      == fx::drawn::kFrames * fx::drawn::kBands * fx::drawn::kBandFields);
		CHECK(e.reached.size() == 7 + fx::drawn::kFrames * fx::drawn::kBands * fx::drawn::kBandFields);
		// AND THE SLOT'S TWO (backlog NR, 2026-10-28): Mix and wet Gain are the site panel's head row, reached by
		// neither surface here — the generic case below lets them through as furniture, and the nest case counts them.
		CHECK(e.reached.size() + 2 == static_cast<size_t>(fx::drawn::Param::kCount));
		CHECK(e.reached.count(static_cast<size_t>(fx::drawn::Param::Mix)) == 0);
		CHECK(e.reached.count(static_cast<size_t>(fx::drawn::Param::WetGain)) == 0);
		// NO GAPS LEFT. Spelled as an assertion rather than as an empty initialiser, because "this
		// device has nothing unreachable" is the claim phase 4 exists to make true.
		CHECK(e.knownGaps.empty());

		// AND EVERY BAND FIELD IS THE PANEL'S, checked through `bandFieldIndex` rather than as a
		// numeric range so it follows the row's own layout.
		for (size_t frame = 0; frame < fx::drawn::kFrames; ++frame) {
			for (size_t band = 0; band < fx::drawn::kBands; ++band) {
				for (const fx::drawn::BandField field :
				     { fx::drawn::PoleRadius, fx::drawn::PoleAngle, fx::drawn::ZeroRadius,
				       fx::drawn::ZeroAngle }) {
					CHECK(e.reached.count(fx::drawn::bandFieldIndex(frame, band, field)) == 1);
				}
			}
		}

		table.push_back(std::move(e));
	}

	// THE TABLE SHAPER, REACHED BY TWO SURFACES THAT SPLIT ITS TWELVE CONTROLS ON PURPOSE.
	//
	// The card is the gamma-zero line: Knee, Colour, Drive, Mix and the enable — five, and every one
	// of them is a control whose zone names mean what they say while Gamma is 0. The panel is the
	// plane: all twelve, because the standing directive is that the expanded view is the COMPLETE
	// device. Gamma, Emphasis and the extras switches exist on the panel and NOWHERE ELSE, so
	// this entry is what says the panel is genuinely reachable rather than merely constructible —
	// which is OWLTT's exact shipped failure, and Waypoint's after it.
	//
	// NO GAPS, and that is asserted rather than left as an empty initialiser.
	{
		const size_t shaperSlot = f.bridge.addFxDevice(fx::shaper::kTraits.tag);
		REQUIRE(shaperSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* shaperCard = dynamic_cast<ui::ShaperCard*>(f.lane->cardAt(shaperSlot));
		REQUIRE(shaperCard != nullptr);

		// THE PANEL BUILT THE WAY THE EDITOR BUILDS IT, through `makeExpandedView`, so this also
		// asserts the entry table has an arm rather than constructing the class behind its back.
		ui::WidgetRegistry shaperRegistry;
		auto shaperView =
		    ui::makeExpandedView(f.deviceHost, &f.bridge, shaperRegistry, f.bridge.fxDeviceAt(shaperSlot),
		                         fx::shaper::kDescriptor);
		REQUIRE(shaperView != nullptr);
		auto* shaperPanel = dynamic_cast<ui::ShaperExpanded*>(shaperView.get());
		REQUIRE(shaperPanel != nullptr);

		Entry e { fx::shaper::kTraits.tag, {}, {}, false };
		for (const fx::shaper::Param p : shaperCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		const size_t fromCard = e.reached.size();
		for (const fx::shaper::Param p : shaperPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}

		// THE SPLIT, NOT ONLY THE TOTAL — the drawn filter's rule, and for its reason: a card that
		// claimed one of the panel's seven would still add to twelve and would be a lie about which
		// surface reaches what.
		// SIX, NOT FIVE — the card's Colour is a coordinate BOX now, and a box is two editors. The
		// coarse half is reachable on the card as well as on the panel, which is a widening of what a
		// hand can touch rather than a bookkeeping change.
		// ELEVEN, NOT SIX — the five extras pills joined the card. Toggles go on the card when there is
		// room and on the panel otherwise. There was room: the card's content reached 225 of its 336, and a
		// pill row costs 28.
		// TWELVE, NOT ELEVEN — Aggression joined the card, in a second knob row beneath Mix, as a
		// dedicated modulatable knob. It is on BOTH
		// surfaces rather than panel-only because it is a mix-shaping control and Mix is on the card;
		// a crossfade width with the crossfade's own knob two clicks away would be the split this
		// case exists to catch.
		// THIRTEEN: Trim joined the card's second row beside Aggression, under Drive.
		// On the card because it is an INPUT gain and the card already
		// carries what decides level — a trim two clicks away from Drive would be the split this case
		// exists to catch.
		// **TWELVE SINCE THE COLLAPSE (BJ piece 3).** `Shaper Colour Phase` is gone — it was measurably
		// a second gamma knob, and Colour absorbed the lapping it existed to receive — so the card
		// reaches one control fewer without losing a widget: its coordinate cell holds one box where
		// it held two.
		// THIRTEEN: the Deadzone switch (2026-09-22) joined the extras pills, which the card draws from
		// `kExtraSwitches` like the other five.
		// NINETEEN (backlog IK): each of the six pills reaches its POWER too — its click.
		// +2 FOR COLOUR'S MODE CELL (backlog IW): the mode and the Inflate amount, beside Colour itself.
		// TWENTY-THREE (backlog FA): the input HPF and LPF, in the card's second row under Knee and Colour.
		// TWENTY-FOUR (backlog OM): the Poly chip at the right end of the extras row.
		CHECK(fromCard == 24);
		// FOURTEEN, NOT THIRTEEN — Hysteresis joined the knob row. It is the field scaler in front of
		// the one per-sample term that had neither a switch nor a knob, and the panel is where it
		// belongs: the card carries the coordinate and what is done with it, the panel carries the
		// biases on the field. (Thirteen was Colour's coarse half, which the panel reaches through the
		// second box of its coordinate widget. **THE CARD REACHES IT TOO**, which is why `fromCard` is
		// 6 rather than 5: an earlier arc had the card wire the carry road and show no box, so it
		// deliberately did NOT claim this control. Both surfaces carry the same widget today.)
		// FIFTEEN NOW: the plane's `Rotate` joined the panel's knob row when the shaper gained a pad
		// that travels. It is panel-only, like automod's own rotate, because a basis turn with no plane
		// in front of it is a control whose effect cannot be seen.
		// SIXTEEN NOW: Aggression, the crossfade's width, on the panel's second knob row as well as on
		// the card. The firmware had this as `blendAggression` folded into Knee; it is a control of its
		// own here so Knee bends the curve and Mix chooses where the bend starts.
		// SEVENTEEN NOW: Trim, on the panel's second row as well as on the card.
		// **SIXTEEN AGAIN (BJ piece 3), and it is the first time this number has gone DOWN.** Every
		// step above added a control; this one removed `Shaper Colour Phase`, which the panel reached
		// through the second box of its coordinate widget. The device declares one param fewer and the
		// panel addresses one fewer — the two moving together is the whole point of asserting both.
		// SEVENTEEN: the Deadzone switch, on the panel's extras row from the same list.
		// TWENTY-TWO: the five of backlog IE, all on the panel's second row.
		// TWENTY-EIGHT (backlog IK): each of the six pills reaches its power by its click.
		// TWENTY-NINE (2026-09-26): `Enabled` left the panel's body for the window header's chip, which
		// every window carries (`ExpandedView::deviceEnableControl`); the card's head still reaches it.
		// THIRTY-TWO (backlog FA): the input HPF and LPF and the crossfade Mix.
		// THIRTY-THREE (backlog OM): Poly, the chip after the extras, as on the card.
		CHECK(shaperPanel->addressedParamsForTest().size() == 33); // +2: Colour Mode and Inflate (backlog IW)
		CHECK(e.reached.size() == static_cast<size_t>(fx::shaper::Param::kCount));
		CHECK(e.knownGaps.empty());

			// AND THE THREE THAT ARE THE PANEL'S ALONE, named rather than counted, so a control migrating
		// onto the card fails here instead of passing on an unchanged total.
		//
		// **IT DID EXACTLY THAT, AND THE FIRST GREEN WAS A LIE.** The five extras were added to the
		// card's drawing and hit test before they were added to its `addressedParamsForTest`, and in
		// that window this case PASSED — it asks each surface what it reaches, and the card was still
		// answering "not these five" while its own hit test toggled them. A guard that takes a surface
		// at its word is worth exactly what the word is worth. Hysteresis stays here: measured, its
		// own row would have left the card 8px, which is inside the slop between a derived height and
		// a capture.
		for (const fx::shaper::Param p : { fx::shaper::Param::Gamma, fx::shaper::Param::Emphasis,
		                                   fx::shaper::Param::Hysteresis, fx::shaper::Param::Bloom,
		                                   fx::shaper::Param::Sag, fx::shaper::Param::Loop,
		                                   fx::shaper::Param::Cheby, fx::shaper::Param::Stair }) {
			CAPTURE(fx::shaper::kParamDescs[static_cast<size_t>(p)].name);
			const auto card = shaperCard->addressedParamsForTest();
			CHECK(std::find(card.begin(), card.end(), p) == card.end());
			CHECK(e.reached.count(static_cast<size_t>(p)) == 1);
		}

		table.push_back(std::move(e));
	}

	{
		Entry e { flt::kTraits.tag, {}, {}, false };
		for (const flt::Param p : f.card->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// The same three the filter's own case names, with the same reasons — they are drawn as
		// bespoke parts of the card rather than as controls, which is a road and not an absence.
		e.knownGaps = {
			{ static_cast<size_t>(flt::Param::Enabled), "the square in the head — Part::Enable" },
			{ static_cast<size_t>(flt::Param::Type), "the dropdown and its arrows" },
			{ static_cast<size_t>(flt::Param::Slope), "the slope dropdown — Part::SlopeMenu" },
		};
		table.push_back(std::move(e));
	}
	{
		// HARM HAS TWO SURFACES, so its entry is the UNION of them — the card and the expanded view
		// behind the head's expand button. WIDENED, NOT NARROWED: the card reduction took six knobs
		// and the Poly switch off the card, and this case went red for all seven within the minute.
		// The tempting fix was to add them to `knownGaps`, which would have recorded seven reachable
		// controls as unreachable and turned the guard into a description of the change it was meant
		// to police.
		Entry e { hrm::kTraits.tag, {}, {}, false };
		for (const hrm::Param p : harmCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		ui::WidgetRegistry harmRegistry;
		ui::HarmExpanded harmPanel { f.deviceHost, harmRegistry, f.bridge.fxDeviceAt(harmSlot) };
		for (const hrm::Param p : harmPanel.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
	}
	{
		// OWLTT's card, in full:
		//   - 12 knobs: attack, release, character, vibe, output, blend, and the six globals
		//     (threshold, ratio, bandwidth, up/down skew, low/high crossover)
		//   - 6 threshold bars on the graph: low/mid/high threshold and bandwidth
		//   - 6 vertical strips: low/mid/high ratio and level
		//
		// EVERY CONTROL BUT TWO, and the two are drawn rather than missing. This entry previously
		// listed six of the twelve knobs as gaps -- Attack, Release, Character, Vibe, Output, Blend --
		// which was accurate for a card that had swapped them out to make room, and that is exactly
		// the problem: THE TABLE WAS EDITED TO MATCH THE CODE. This table is the only guard this repo
		// has against a control a hand cannot reach, so a gap added here has to be a decision, never
		// a description of what the code happens to do today.
		//
		// **ASKED OF THE WINDOW, NOT WRITTEN OUT** (backlog FY, 2026-10-30). This entry was `2..kCount` by hand,
		// so it said "reached" whatever OWLTT's surfaces did: the one seam that answers for this device was
		// never read here. `OwlttCard` keeps no seam; its window does, and is the surface that holds every
		// control (*"OWLTT's panel alone reaches every control"), so the window's own claim is what goes in.
		Entry e { "OWLTT", {}, {}, false };
		{
			const size_t owlttSlot = f.bridge.addFxDevice(fx::owltt::kTraits.tag);
			REQUIRE(owlttSlot < feathers::kMaxFxSlots);
			f.bridge.refresh();
			ui::WidgetRegistry owlttRegistry;
			ui::OwlttExpanded owlttPanel { f.deviceHost, owlttRegistry, f.bridge.fxDeviceAt(owlttSlot) };
			for (const auto p : owlttPanel.addressedParamsForTest()) {
				e.reached.insert(static_cast<size_t>(p));
			}
			REQUIRE(f.bridge.removeFxDevice(owlttSlot));
			f.bridge.refresh();
			f.lane->resized();
		}
		// MODE IS NOT A GAP: the window draws it (its own claim says so), so the old entry's gap was a description.
		e.knownGaps = {
		    {1, "Enabled — bypass switch, drawn in the shared head after stage 4"},
		};
		table.push_back(std::move(e));
	}
	{
		// THE AUTOMODULATOR. Three coordinate boxes (BOTH HALVES of each — the box is two editors,
		// not a knob with a gesture), a lone gamma box, five knobs, and the enable square in the head.
		//
		// ASKED OF THE CARD IN THE LANE rather than of a card built here, and rather than restated as
		// indices: the lane's tag dispatch is part of what this checks, so a card constructed directly
		// would pass even if `ensureCard` had never learned the tag.
		namespace amd = feathers::fx::automod;
		const size_t automodSlot = f.bridge.addFxDevice(amd::kTraits.tag);
		f.lane->resized();
		REQUIRE(f.lane->cardAt(automodSlot) != nullptr);
		auto* automodCard = dynamic_cast<ui::AutomodCard*>(f.lane->cardAt(automodSlot));
		REQUIRE(automodCard != nullptr);

		Entry e { amd::kTraits.tag, {}, {}, false };
		for (const amd::Param p : automodCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		const size_t fromCard = e.reached.size();

		// **AND THE PANEL, WHICH AUTOMOD HAS ONLY JUST GROWN**, modelled on Shaper's window. Before it,
		// four of this device's controls — the three phase offsets and
		// the coordinates' own gamma — had nowhere to be touched from, and the three phases had no
		// widget at all. The panel is where they live now, so the sweep has to ask it too or it is
		// asking half the device.
		ui::WidgetRegistry automodRegistry;
		std::unique_ptr<ui::ExpandedView> automodView = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, automodRegistry, f.bridge.fxDeviceAt(automodSlot), amd::kDescriptor);
		REQUIRE(automodView != nullptr);
		auto* automodPanel = dynamic_cast<ui::AutomodExpanded*>(automodView.get());
		REQUIRE(automodPanel != nullptr);
		for (const amd::Param p : automodPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}

		// THE SPLIT, NOT ONLY THE TOTAL — the shaper's rule, for its reason: a card that claimed one
		// of the panel's controls would still add to the same total and would be a lie about WHICH
		// surface reaches what.
		//
		// **FIFTEEN FROM THE CARD, AND THAT NUMBER HAS NOW MOVED IN BOTH DIRECTIONS**, which is the
		// argument for pinning it rather than checking the total. It was sixteen while `Env` sat on
		// the card; `Env` moved to the window by design and the card is one lighter. A check on
		// `reached.size()` alone would have been green through both changes and through a card that
		// silently claimed one of the panel's controls.
		//
		// **AND IT HAS MOVED A THIRD TIME: TWELVE SINCE THE COLLAPSE (BJ piece 3).** Automod's three
		// coordinates absorbed their phase halves, so the device has three fewer params and the card
		// reaches three fewer — the cells that carried two controls each now carry one.
		//
		// AND THE OLDER NOTE STILL HOLDS, because it corrected a premise of mine: the three phase
		// offsets were NOT unreachable before the window existed — the card's coordinate widgets
		// always carried them. The panel does not unlock them; it gives the coordinates a PLANE
		// instead of separate boxes, which is a different and smaller claim than the one I started
		// with. The collapse does not change that either: it changes how many params say it.
		CHECK(fromCard == 12);
		CHECK(e.reached.size() == static_cast<size_t>(amd::Param::kCount));

		// AND TWO CONTROLS ARE THE PANEL'S ALONE. `Rotate` re-aims the basis the pad projects onto, so
		// it is meaningless on a surface with no pad. `Env` is the panel's because it was moved it there
		// — a different reason, and worth keeping distinct: one control CANNOT live on the card, the
		// other simply no longer does, and a later change may move `Env` back without touching
		// `Rotate`.
		{
			const auto card = automodCard->addressedParamsForTest();
			CAPTURE(amd::kParamDescs[static_cast<size_t>(amd::Param::Rotate)].name);
			for (const amd::Param p : { amd::Param::Rotate, amd::Param::EnvTrim }) {
				CAPTURE(amd::kParamDescs[static_cast<size_t>(p)].name);
				CHECK(std::find(card.begin(), card.end(), p) == card.end());
				CHECK(e.reached.count(static_cast<size_t>(p)) == 1);
				const auto panel = automodPanel->addressedParamsForTest();
				CHECK(std::find(panel.begin(), panel.end(), p) != panel.end());
			}
		}

		// AND THE PAD REACHES THE THREE COORDINATES THE CELLS ALSO REACH, which is a widening of what a
		// hand can touch rather than a duplication to be removed: a cell sets one coordinate by typing,
		// the pad sets all three by aiming. The shaper's Colour makes the same argument about its card
		// and its pad both carrying the same control.
		//
		// **THE COORDINATES THEMSELVES SINCE THE COLLAPSE (BJ piece 3), where this named their phase
		// halves.** Same three controls; there is simply one param each now.
		for (const amd::Param p : { amd::Param::Type, amd::Param::Flavor, amd::Param::Mod }) {
			CAPTURE(amd::kParamDescs[static_cast<size_t>(p)].name);
			const auto panel = automodPanel->addressedParamsForTest();
			CHECK(std::find(panel.begin(), panel.end(), p) != panel.end());
		}

		table.push_back(std::move(e));
	}
	{
		Entry e { dsr::kTraits.tag, {}, {}, false };
		for (const dsr::Param p : envCard.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// **THE SECOND GAP THIS GUARD CLOSED, AND THE FIRST ONE IT WAS LIED TO ABOUT.** The entry read
		// *"Legato (Single Trigger) — internal, set by the synth for legato behavior", and no part of
		// that was true: `grep -rn singleTrigger src/` returned four hits, all of them in
		// `mod/dahdsr/`, and not one writer in `src/plugin` or `src/rack`. It was an ordinary stored
		// control whose chip had been deleted beside `Loop`'s in the Mode collapse (`fd07c7f4`) — so
		// the hole was real and the reason recorded for it was an invention. **A KNOWN GAP IS A CLAIM,
		// AND AN UNCHECKED ONE TURNS A GUARD INTO A LIST OF THINGS IT HAS AGREED NOT TO SEE.** When
		// the chip came back this entry went red, which is the mechanism working; what it could not do
		// was question its own excuse.
		//
		// Emptied rather than kept with a softer reason, for the same argument the MSEG's entry makes
		// below: a table that keeps an entry after its reason expires is how a guard turns into a
		// description.
		table.push_back(std::move(e));
	}
	{
		Entry e { mseg::kTraits.tag, {}, {}, false };
		for (const mseg::Param p : msegCard.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// THE GUARD'S FIRST FINDING, AND IT IS CLOSED. This entry used to hold one known gap —
		// `SustainSegment`, "drawn as a marker, draggable by nothing" — left open because a sustain
		// gesture was not in the original scope. It was added on 2026-08-26 (the marker drags to a
		// different breakpoint), so the gap became a drag and the entry became empty. Emptied rather than kept with a softer reason: a table that keeps an entry
		// after the reason expires is how a guard turns into a description.
		table.push_back(std::move(e));
	}

	// **THE TRACKER, REACHED BY THE GENERIC CARD** — the tracker uses the generic card for now, with
	// its own card to come later. It is the first device in this table whose surface is the
	// generic grid rather than a hand-written card, and that is exactly why registering it was held
	// up: a device in the menu must be controllable in the strip, and until the strip had a generic
	// arm that meant hand-writing a card before registering anything.
	//
	// ASKED OF THE CARD, like every other entry. `paramIndicesForTest()` is `GenericCard`'s own claim
	// about which controls it put a knob on; restating the rule here would let the two drift and this
	// table would go on passing while the card stopped drawing something.
	{
		ui::GenericCard trackerCard { f.deviceHost, f.bridge.rackDeviceAt(0), feathers::mod::tracker::kDescriptor,
			                          nullptr };
		Entry e { feathers::mod::tracker::kTraits.tag, {}, {}, false };
		for (const size_t index : trackerCard.paramIndicesForTest()) {
			e.reached.insert(index);
		}
		// AND THE ENABLE, WHICH THE HEAD DRAWS RATHER THAN THE GRID. `GenericCard` deliberately omits
		// it from its knobs — a device listing its enable as a knob had one field with two controls —
		// so the head's square is what reaches it, and leaving it out here would report a gap that is
		// really a division of labour.
		if (feathers::mod::tracker::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::tracker::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}

	// THE STEREO TOOL (backlog KU) — ON ITS OWN CARD SINCE plan Task 14b (`StereoToolCard`), reached THROUGH THE
	// STRIP like Math: the `dynamic_cast` is what fails if the strip falls back to the generic grid. Pan, In, Width
	// and Swap across its three tabs; the head's square for the enable.
	{
		const size_t stereoSlot = f.bridge.addRackDevice(feathers::mod::stereo::kTraits.tag);
		REQUIRE(stereoSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* stereoCard = dynamic_cast<const ui::StereoToolCard*>(f.panel->cardFrameForTest(stereoSlot));
		REQUIRE(stereoCard != nullptr);
		Entry e { feathers::mod::stereo::kTraits.tag, {}, {}, false };
		for (const feathers::mod::stereo::Param p : stereoCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		if (feathers::mod::stereo::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::stereo::kTraits.enableControl));
		}
		table.push_back(std::move(e));
		// AND OUT OF THE STRIP AGAIN — it holds four, and Math's entry below seats its own device.
		REQUIRE(f.bridge.removeRackDevice(stereoSlot));
		f.bridge.refresh();
		f.panel->resized();
	}

	// RANDOM TRIGGER (backlog NA (a)) — ITS OWN CARD (`RandomTriggerCard`), reached through the strip like the Stereo
	// Tool. Every control is on a widget; the gates (Sync, Timing, Seed Mode) reveal the hidden ones.
	{
		const size_t slot = f.bridge.addRackDevice(feathers::mod::rtrig::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = dynamic_cast<const ui::RandomTriggerCard*>(f.panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		Entry e { feathers::mod::rtrig::kTraits.tag, {}, {}, false };
		for (const feathers::mod::rtrig::Param p : card->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(slot));
	}
	// SAMPLE & HOLD (backlog MW) — ON ITS OWN CARD (`SampleHoldCard`), reached THROUGH THE STRIP like the Stereo Tool.
	// Every box (the gated ones included: the card seats a widget for each, and draws it where its gate is open), the
	// three chips, the Trig pad; the head's square for the enable.
	{
		const size_t shSlot = f.bridge.addRackDevice(feathers::mod::samplehold::kTraits.tag);
		REQUIRE(shSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* shCard = dynamic_cast<const ui::SampleHoldCard*>(f.panel->cardFrameForTest(shSlot));
		REQUIRE(shCard != nullptr);
		Entry e { feathers::mod::samplehold::kTraits.tag, {}, {}, false };
		for (const size_t p : shCard->addressedFieldsForTest()) {
			e.reached.insert(p);
		}
		e.reached.insert(static_cast<size_t>(feathers::mod::samplehold::kTraits.enableControl));
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(shSlot));
	}
	// THE STEP SEQUENCER (backlog MX) — ON ITS OWN CARD (`StepSeqCard`, the step grid), reached THROUGH THE STRIP like
	// the Stereo Tool: the cast fails if the strip falls back to the generic grid. Every control but the enable on the
	// card's widgets (the two Trig pads among them); the head's square for the enable.
	{
		const size_t seqSlot = f.bridge.addRackDevice(feathers::mod::stepseq::kTraits.tag);
		REQUIRE(seqSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* seqCard = dynamic_cast<const ui::StepSeqCard*>(f.panel->cardFrameForTest(seqSlot));
		REQUIRE(seqCard != nullptr);
		Entry e { feathers::mod::stepseq::kTraits.tag, {}, {}, false };
		for (const size_t p : seqCard->addressedParamsForTest()) {
			e.reached.insert(p);
		}
		if (feathers::mod::stepseq::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::stepseq::kTraits.enableControl));
		}
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(seqSlot));
		f.bridge.refresh();
		f.panel->resized();
	}
	// THE FOLLOWER (backlog MY) — ON ITS OWN CARD (`FollowerCard`), reached through the strip like the Stereo Tool:
	// Mode, Poly, the Detector, Input, the four knobs, and Threshold as the plot's line.
	{
		const size_t followerSlot = f.bridge.addRackDevice(feathers::mod::follower::kTraits.tag);
		REQUIRE(followerSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* followerCard = dynamic_cast<const ui::FollowerCard*>(f.panel->cardFrameForTest(followerSlot));
		REQUIRE(followerCard != nullptr);
		Entry e { feathers::mod::follower::kTraits.tag, {}, {}, false };
		for (const size_t p : followerCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		e.reached.insert(static_cast<size_t>(feathers::mod::follower::kTraits.enableControl));
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(followerSlot));
		f.bridge.refresh();
		f.panel->resized();
	}
	// THE ARP — ON ITS OWN CARD (`ArpCard`), reached through the strip: Order, Time
	// Base, Latch and four knobs around the step lane, the head's Enabled; and its WINDOW (`GenericExpanded`, through
	// `makeExpandedView`) every control. The card's seven are asserted inside the window's set: window ⊇ card.
	{
		namespace ap = feathers::mod::arp;
		const size_t arpSlot = f.bridge.addRackDevice(ap::kTraits.tag);
		REQUIRE(arpSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* arpCard = dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(arpSlot));
		REQUIRE(arpCard != nullptr);
		Entry e { ap::kTraits.tag, {}, {}, false };
		std::set<size_t> card;
		for (const size_t p : arpCard->addressedParamsForTest()) {
			card.insert(p);
			e.reached.insert(p);
		}
		CHECK(card.size() == 10); // the Feel's list joined the card where Swing's knob was (2026-10-30); Free Phase its follower (backlog QA); Wobble beside it (by design)
		e.reached.insert(static_cast<size_t>(ap::kTraits.enableControl));
		ui::ModState arpMod; // above the view: its handles watch this state and must die first
		ui::WidgetRegistry arpRegistry;
		auto arpView = ui::makeExpandedView(f.deviceHost, &f.bridge, arpRegistry, arpCard->device(), ap::kDescriptor);
		REQUIRE(arpView != nullptr);
		auto* arpPanel = dynamic_cast<ui::ArpExpanded*>(arpView.get());
		REQUIRE(arpPanel != nullptr);
		// AND WHAT THE CARD SHOWS BESIDE ITS CONTROLS: the step lane and every source handle (window ⊇ card, 2026-10-30).
		arpPanel->setModulation(&arpMod, nullptr);
		arpPanel->setBounds(0.0f, 0.0f, 640.0f, 480.0f);
		CHECK(arpPanel->lane().width() > 200);
		CHECK(arpPanel->lane().height() > ui::ArpCard::kLaneHeight);
		for (size_t out = 0; out < ap::kDescriptor.outputs.size(); ++out) {
			CAPTURE(out);
			const ui::SourceArm* arm = arpPanel->armForTest(out);
			REQUIRE(arm != nullptr);
			CHECK(arm->isVisible());
			CHECK(arm->width() > 0);
		}
		std::set<size_t> panel(arpPanel->paramIndicesForTest().begin(), arpPanel->paramIndicesForTest().end());
		for (const size_t p : card) {
			CHECK(panel.count(p) == 1);
		}
		e.reached.insert(panel.begin(), panel.end());
		CHECK(e.reached.size() == ap::kParamCount);
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(arpSlot));
		f.bridge.refresh();
		f.panel->resized();
	}
	// THE PATTERN DEVICE — ON ITS OWN CARD (`PatternCard`: the tabs with their E and
	// S chips, Keys, Humanize's zone cell and its Amount, Latch, the focused pattern's Mode, Scale, Rate and Length, its
	// Cycle cell (Next, Repeats) and the Play Mode chip, the
	// selector), reached through the strip; its window (`PatternExpanded`, composed, through `makeExpandedView`) reaches
	// every control at SOME focus — each tab's own under that tab — so the reach is the union over the four tabs.
	// Window ⊇ card, asserted as for the arp.
	{
		namespace pt = feathers::mod::arp::pat;
		const size_t patSlot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(patSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* patCard = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(patSlot));
		REQUIRE(patCard != nullptr);
		Entry e { pt::kTraits.tag, {}, {}, false };
		const std::vector<size_t> addressed = patCard->addressedParamsForTest();
		std::set<size_t> card(addressed.begin(), addressed.end());
		CHECK(card.size() == 4 * 8 + 8); // each tab: E, S, Mode, Scale, Rate, Length, and Cycle's Next and Repeats; Keys, Humanize, Amount, Free Phase, Wobble (by design), Latch, the selector and Play Mode
		e.reached.insert(card.begin(), card.end());
		e.reached.insert(static_cast<size_t>(pt::kTraits.enableControl));
		ui::WidgetRegistry patRegistry;
		auto patView = ui::makeExpandedView(f.deviceHost, &f.bridge, patRegistry, patCard->device(), pt::kDescriptor);
		REQUIRE(patView != nullptr);
		auto* patPanel = dynamic_cast<ui::PatternExpanded*>(patView.get());
		REQUIRE(patPanel != nullptr);
		patPanel->setBounds(0.0f, 0.0f, 900.0f, 1200.0f);
		std::set<size_t> panel;
		for (size_t page = 0; page < pt::kPatterns; ++page) {
			patPanel->showPage(page);
			for (const size_t p : reachedUnder(patPanel, pt::kDescriptor)) {
				panel.insert(p);
			}
		}
		for (const size_t p : card) {
			CHECK(panel.count(p) == 1);
		}
		e.reached.insert(panel.begin(), panel.end());
		CHECK(e.reached.size() == pt::kParamCount);
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(patSlot));
		f.bridge.refresh();
		f.panel->resized();
	}

	// NOTE EXPRESSIONS — the second device on the generic grid, and the reason it needs no more than
	// that: it declares a Rule and an Enabled, and its whole substance is seven OUTPUTS, which are a
	// modulation source's business rather than a card's. It has no per-output defaults by design —
	// the patch's modulation handles that — so there is nothing to draw beyond the two controls, and a hand-written card
	// would be a surface with nothing on it the grid does not already put there.
	{
		ui::GenericCard nexpCard { f.deviceHost, f.bridge.rackDeviceAt(0),
			                       feathers::mod::noteexpr::kDescriptor, nullptr };
		Entry e { feathers::mod::noteexpr::kTraits.tag, {}, {}, false };
		for (const size_t index : nexpCard.paramIndicesForTest()) {
			e.reached.insert(index);
		}
		// THE HEAD'S SQUARE, as the tracker's entry above — the grid omits the enable and the head
		// draws it, so leaving it out would report a division of labour as a gap.
		if (feathers::mod::noteexpr::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::noteexpr::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}

	// MACROS — ON ITS OWN CARD SINCE macro-modulation B3, reached THROUGH THE STRIP as bus utility is
	// through the lane: the `dynamic_cast` is the claim that the strip builds `MacrosCard` for the tag,
	// which matters more here than for most devices because the card is the only reader of `Type n` —
	// a strip that fell back to the generic grid would still reach every control and show a Type knob
	// that changes nothing. Four values, four polarities, four types; the head's square for the enable.
	{
		const size_t macrosSlot = f.bridge.addRackDevice(feathers::mod::macros::kTraits.tag);
		REQUIRE(macrosSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* macrosCard = dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(macrosSlot));
		REQUIRE(macrosCard != nullptr);
		Entry e { feathers::mod::macros::kTraits.tag, {}, {}, false };
		for (const feathers::mod::macros::Param p : macrosCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		if (feathers::mod::macros::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::macros::kTraits.enableControl));
		}
		table.push_back(std::move(e));
		// THE STRIP HOLDS FOUR: free this seat before the variants take theirs.
		REQUIRE(f.bridge.removeRackDevice(macrosSlot));
		f.bridge.refresh();
		f.panel->resized();
		// MACRO1 AND MACROS8 — THE SAME CARD OVER THEIR OWN DESCRIPTORS, reached through the strip the same way:
		// three controls per macro and the head's square for the enable.
		for (const feathers::device::Descriptor* variant : { &feathers::mod::macro1::kDescriptor, &feathers::mod::macros8::kDescriptor }) {
			const size_t variantSlot = f.bridge.addRackDevice(variant->traits.tag);
			REQUIRE(variantSlot < feathers::kMaxRackSlots);
			f.bridge.refresh();
			f.panel->resized();
			auto* variantCard = dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(variantSlot));
			REQUIRE(variantCard != nullptr);
			Entry e { variant->traits.tag, {}, {}, false };
			for (const feathers::mod::macros::Param p : variantCard->addressedParamsForTest()) {
				e.reached.insert(static_cast<size_t>(p));
			}
			e.reached.insert(static_cast<size_t>(variant->traits.enableControl));
			table.push_back(std::move(e));
			// THE STRIP HAS ONLY SO MANY SEATS: free this one for the next.
			REQUIRE(f.bridge.removeRackDevice(variantSlot));
			f.bridge.refresh();
			f.panel->resized();
		}
	}

	// MATH (macro-modulation C) — ON ITS OWN CARD SINCE 2026-09-25 (`MathCard`, the compact layout),
	// reached THROUGH THE STRIP, asked of the card the strip built: the `dynamic_cast` is what fails if the
	// strip falls back to the generic grid. A, Operator, B, Amount, Bipolar, Transfer; the head's square
	// for the enable.
	{
		const size_t mathSlot = f.bridge.addRackDevice(feathers::mod::math::kTraits.tag);
		REQUIRE(mathSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* mathCard = const_cast<ui::MathCard*>(dynamic_cast<const ui::MathCard*>(f.panel->cardFrameForTest(mathSlot)));
		REQUIRE(mathCard != nullptr);
		Entry e { feathers::mod::math::kTraits.tag, {}, {}, false };
		// IN EVERY TRANSFER (backlog PY): Periods is drawn only under Phi and Exponent only under Log and Exp, so
		// the card is asked once per map, through its own Transfer control.
		namespace fmth = feathers::mod::math;
		for (int map = 0; map < static_cast<int>(fmth::Map::kCount); ++map) {
			mathCard->control(static_cast<size_t>(fmth::Param::Map)).setValue(static_cast<double>(map));
			(void)mathCard->updateContent();
			for (const fmth::Param p : mathCard->addressedParamsForTest()) {
				e.reached.insert(static_cast<size_t>(p));
			}
		}
		mathCard->control(static_cast<size_t>(fmth::Param::Map)).setValue(0.0);
		(void)mathCard->updateContent();
		if (feathers::mod::math::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::mod::math::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}

	// TWIN FIELD, ON ITS OWN CARD THROUGH THE STRIP: both layers' rows exist for
	// the card's life (the tab shows one), the solos on the tabs, the crossfade on the pad; the head for the enable.
	{
		// THE STRIP HOLDS FOUR: free the first slot if it is full, and leave it empty again after.
		if (f.bridge.rackDeviceAt(feathers::kMaxRackSlots - 1) != kNoInstance) {
			REQUIRE(f.bridge.removeRackDevice(0));
			f.bridge.refresh();
		}
		const size_t tfSlot = f.bridge.addRackDevice(feathers::mod::twinfield::kTraits.tag);
		REQUIRE(tfSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* tfCard = dynamic_cast<const ui::TwinFieldCard*>(f.panel->cardFrameForTest(tfSlot));
		REQUIRE(tfCard != nullptr);
		Entry e { feathers::mod::twinfield::kTraits.tag, {}, {}, false };
		for (const feathers::mod::twinfield::Param p : tfCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		e.reached.insert(static_cast<size_t>(feathers::mod::twinfield::kTraits.enableControl));
		CHECK(e.reached.size() == feathers::mod::twinfield::at(feathers::mod::twinfield::Param::kCount));
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeRackDevice(tfSlot));
		f.bridge.refresh();
		f.panel->resized();
	}

	// BUS UTILITY, REACHED BY ITS OWN CARD — and reached THROUGH THE LANE rather than by building
	// a card here, which is the stronger claim of the two available.
	//
	// It ran on `GenericCard` for one commit and has `BusUtilCard` now, because a METER is what earns
	// a hand-written surface and a knob grid cannot draw one. The `dynamic_cast` is the assertion: it
	// fails if the factory entry is ever lost, in which case the lane silently falls back to the
	// generic grid and every control here stays reachable — a regression this table could not
	// otherwise see, since `reached` would be just as full.
	{
		const size_t busSlot = f.bridge.addFxDevice(feathers::fx::busutil::kTraits.tag);
		REQUIRE(busSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* busCard = dynamic_cast<ui::BusUtilCard*>(f.lane->cardAt(busSlot));
		REQUIRE(busCard != nullptr);

		Entry e { feathers::fx::busutil::kTraits.tag, {}, {}, false };
		for (const feathers::fx::busutil::Param p : busCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND THE ENABLE, WHICH THE HEAD DRAWS RATHER THAN THE GRID — the same division of labour the
		// tracker entry above describes.
		if (feathers::fx::busutil::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::busutil::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}

	// COMP, REACHED BY ITS OWN CARD THROUGH THE LANE — Bus Utility's entry above, for Bus Utility's
	// reason: the `dynamic_cast` is what fails if the factory entry is lost and the lane falls back to
	// the generic grid. EVERY control is listed, hidden or not: the Mode control reveals the rest.
	{
		const size_t compSlot = f.bridge.addFxDevice(feathers::fx::comp::kTraits.tag);
		REQUIRE(compSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* compCard = dynamic_cast<ui::CompCard*>(f.lane->cardAt(compSlot));
		REQUIRE(compCard != nullptr);
		Entry e { feathers::fx::comp::kTraits.tag, {}, {}, false };
		for (const feathers::fx::comp::Param p : compCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND ITS WINDOW (backlog QG), built the way the editor builds it: Link and the second half's three are there, not on
		// the card (the card is a subset of the window, never the other way round).
		ui::WidgetRegistry compRegistry;
		auto compView = ui::makeExpandedView(f.deviceHost, &f.bridge, compRegistry, f.bridge.fxDeviceAt(compSlot),
		                                     feathers::fx::comp::kDescriptor);
		auto* compPanel = dynamic_cast<ui::CompExpanded*>(compView.get());
		REQUIRE(compPanel != nullptr);
		for (const feathers::fx::comp::Param p : compPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		compView.reset();
		if (feathers::fx::comp::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::comp::kTraits.enableControl));
		}
		table.push_back(std::move(e));
		// AND THE SLOT GIVEN BACK: `kMaxFxSlots` is 8 and the chain entry below needs one (the delay's
		// note below records what running out costs). The card is not touched after this line.
		REQUIRE(f.bridge.removeFxDevice(compSlot));
		f.lane->resized();
	}

	// SIDECHAIN INJECT (backlog NW), REACHED BY ITS OWN CARD THROUGH THE LANE — COMP's road, for COMP's reason.
	{
		const size_t injectSlot = f.bridge.addFxDevice(feathers::fx::inject::kTraits.tag);
		REQUIRE(injectSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* injectCard = dynamic_cast<ui::InjectCard*>(f.lane->cardAt(injectSlot));
		REQUIRE(injectCard != nullptr);
		Entry e { feathers::fx::inject::kTraits.tag, {}, {}, false };
		for (const feathers::fx::inject::Param p : injectCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeFxDevice(injectSlot));
		f.lane->resized();
	}

	// THE EQ, REACHED BY ITS OWN CARD THROUGH THE LANE — COMP's road, for COMP's reason: the `dynamic_cast`
	// is what fails if the factory entry is lost. Every band's controls are listed, shown or not: selecting
	// a band (a handle) reveals its strip, and the tilt pivot is the diamond on the plot.
	{
		const size_t eqSlot = f.bridge.addFxDevice(feathers::fx::eq::kTraits.tag);
		REQUIRE(eqSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* eqCard = dynamic_cast<ui::EqCard*>(f.lane->cardAt(eqSlot));
		REQUIRE(eqCard != nullptr);
		Entry e { feathers::fx::eq::kTraits.tag, {}, {}, false };
		for (const size_t index : eqCard->addressedParamsForTest()) {
			e.reached.insert(index);
		}
		// AND ITS PANEL, built the way the editor builds it: Phase is a panel-only chip (step 7, A29), so the
		// entry is the union of the two surfaces, as HARM's is.
		ui::WidgetRegistry eqRegistry;
		auto eqView = ui::makeExpandedView(f.deviceHost, &f.bridge, eqRegistry, f.bridge.fxDeviceAt(eqSlot),
		                                   feathers::fx::eq::kDescriptor);
		REQUIRE(eqView != nullptr);
		auto* eqPanel = dynamic_cast<ui::EqExpanded*>(eqView.get());
		REQUIRE(eqPanel != nullptr);
		for (const size_t index : eqPanel->addressedParamsForTest()) {
			e.reached.insert(index);
		}
		eqView.reset(); // before its device goes
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeFxDevice(eqSlot));
		f.lane->resized();
	}

	// CLIP, THE SAME WAY — seated through the lane, cast to its own card, every control listed, and the
	// slot given back.
	{
		const size_t clipSlot = f.bridge.addFxDevice(feathers::fx::clip::kTraits.tag);
		REQUIRE(clipSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* clipCard = dynamic_cast<ui::ClipCard*>(f.lane->cardAt(clipSlot));
		REQUIRE(clipCard != nullptr);
		Entry e { feathers::fx::clip::kTraits.tag, {}, {}, false };
		for (const feathers::fx::clip::Param p : clipCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		if (feathers::fx::clip::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::clip::kTraits.enableControl));
		}
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeFxDevice(clipSlot));
		f.lane->resized();
	}

	// NOISE, CLIP'S WAY — seated through the lane, cast to its own card, every control listed (the Source and the other
	// gates reveal the hidden ones; Cutoff and Resonance are the plot's handle), and the slot given back.
	{
		const size_t noiseSlot = f.bridge.addFxDevice(feathers::fx::noise::kTraits.tag);
		REQUIRE(noiseSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* noiseCard = dynamic_cast<ui::NoiseCard*>(f.lane->cardAt(noiseSlot));
		REQUIRE(noiseCard != nullptr);
		Entry e { feathers::fx::noise::kTraits.tag, {}, {}, false };
		for (const feathers::fx::noise::Param p : noiseCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeFxDevice(noiseSlot));
		f.lane->resized();
	}

	// THE DISPERSER, REACHED BY ITS OWN CARD THROUGH THE LANE — COMP's pattern: the `dynamic_cast` fails if
	// the factory entry is lost. Freq and Offset are both listed: Key Track reveals whichever is hidden.
	{
		const size_t slot = f.bridge.addFxDevice(feathers::fx::disperser::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* card = dynamic_cast<ui::DisperserCard*>(f.lane->cardAt(slot));
		REQUIRE(card != nullptr);
		Entry e { feathers::fx::disperser::kTraits.tag, {}, {}, false };
		for (const feathers::fx::disperser::Param p : card->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND ITS WINDOW, EQ's pattern: the Q offset (2026-10-03) is the window plot's vertical drag and a box
		// there, and the card does not seat it, so the entry is the union of the two surfaces.
		ui::WidgetRegistry dispRegistry;
		auto dispView = ui::makeExpandedView(f.deviceHost, &f.bridge, dispRegistry, f.bridge.fxDeviceAt(slot),
		                                     feathers::fx::disperser::kDescriptor);
		REQUIRE(dispView != nullptr);
		auto* dispPanel = dynamic_cast<ui::DisperserExpanded*>(dispView.get());
		REQUIRE(dispPanel != nullptr);
		for (const feathers::fx::disperser::Param p : dispPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		dispView.reset(); // before its device goes
		table.push_back(std::move(e));
		REQUIRE(f.bridge.removeFxDevice(slot));
		f.lane->resized();
	}

	// THE PHASER/FLANGER, by the disperser's road — its own card through the lane, cast (the cast fails if the factory
	// entry is lost) — and, as HARM's, the UNION with its window: the card leaves Rate, Sync, the follower's shape, Key
	// Offset and Rotate to `PhaserExpanded` (which holds the card's controls too — a device's window is a superset of its
	// card, guarded in the tooltips suite), built through `makeExpandedView` so the entry table's arm is asserted too.
	// Center and Delay are both listed: the Mode reveals whichever is hidden.
	{
		namespace ph = feathers::fx::phaser;
		const size_t slot = f.bridge.addFxDevice(ph::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* card = dynamic_cast<ui::PhaserCard*>(f.lane->cardAt(slot));
		REQUIRE(card != nullptr);
		Entry e { ph::kTraits.tag, {}, {}, false };
		for (const ph::Param p : card->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		ui::WidgetRegistry phaserRegistry;
		auto phaserView = ui::makeExpandedView(f.deviceHost, &f.bridge, phaserRegistry, f.bridge.fxDeviceAt(slot),
		                                       ph::kDescriptor);
		auto* phaserPanel = dynamic_cast<ui::PhaserExpanded*>(phaserView.get());
		REQUIRE(phaserPanel != nullptr);
		for (const ph::Param p : phaserPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		table.push_back(std::move(e));
		phaserView.reset();
		REQUIRE(f.bridge.removeFxDevice(slot));
		f.lane->resized();
	}

	// THE RACK (backlog NO), REACHED THROUGH THE LANE, the Chain's road: its own card (the cast fails if the factory
	// entry is lost) seats Mix and Gain, its window the macros as well — the union of the two surfaces, built through
	// `makeExpandedView` so the window's arm is asserted too — and the head draws the enable. A chain's level, mute
	// and solo stay in the body, but each chain's slot params (their destinations) are reached through its strip, below.
	{
		namespace pr = feathers::rack::parallel;
		const size_t slot = f.bridge.addFxDevice(pr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* card = dynamic_cast<ui::ParallelRackCard*>(f.lane->cardAt(slot));
		REQUIRE(card != nullptr);
		Entry e { pr::kTraits.tag, {}, {}, false };
		for (const pr::Param p : card->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		ui::WidgetRegistry rackRegistry;
		auto rackView = ui::makeExpandedView(f.deviceHost, &f.bridge, rackRegistry, f.bridge.fxDeviceAt(slot), pr::kDescriptor);
		auto* rackPanel = dynamic_cast<ui::ParallelRackExpanded*>(rackView.get());
		REQUIRE(rackPanel != nullptr);
		for (const pr::Param p : rackPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND THE WINDOW'S BANDS ROW: Key Track and the four corners' knobs, which only the window draws since
		// 2026-10-29 (the card keeps the corners as drags on its spectrum strip) — each its own widget, asked by name.
		(void)rackPanel->keyTrackForTest();
		e.reached.insert(static_cast<size_t>(pr::Param::KeyTrack));
		// AND THE VIEW'S POINTERS, the window's alone: which chain it is looking at, and which page of pads.
		(void)rackPanel->viewedBoxForTest();
		(void)rackPanel->pageBoxForTest();
		(void)rackPanel->followChipForTest(); // Follow played note: the switch under the Pads panel
		e.reached.insert(static_cast<size_t>(pr::Param::FollowPlayed));
		e.reached.insert(static_cast<size_t>(pr::Param::ViewedChain));
		// AND FOLLOW PLAYED NOTE, the window's switch under the Pads panel.
		(void)rackPanel->followChipForTest();
		e.reached.insert(static_cast<size_t>(pr::Param::FollowPlayed));
		e.reached.insert(static_cast<size_t>(pr::Param::PadsPage));
		// WHAT TABS PUTS IN FRONT (Cards or the first device's window): the window's own two-state control, in Tabs.
		(void)rackPanel->frontSegForTest();
		e.reached.insert(static_cast<size_t>(pr::Param::Front));
		for (size_t k = 0; k < 4; ++k) {
			REQUIRE(rackPanel->cutKnobForTest(k).control() != nullptr);
			e.reached.insert(static_cast<size_t>(pr::Param::Cut1) + k);
		}
		// THE TWO PANES' FOLD STRIPS declare the fold they change through `ControlFace`, each offering exactly its own control.
		for (const auto& [rail, param] : { std::pair<ui::Rail*, pr::Param> { &rackPanel->containerRailForTest(), pr::Param::InspectorFold },
		                                   std::pair<ui::Rail*, pr::Param> { &rackPanel->macrosRailForTest(), pr::Param::MacrosFold } }) {
			const auto* face = dynamic_cast<const ui::ControlFace*>(rail);
			REQUIRE(face != nullptr);
			std::vector<ui::ControlFace::Offered> offered;
			face->offeredControls(offered);
			REQUIRE(offered.size() == 1);
			CHECK(offered[0].control != nullptr);
			e.reached.insert(static_cast<size_t>(param));
		}
		// THE VIEWED CHAIN AND THE PADS' PAGE are reached by GESTURES: a press on a tab views its chain (the rack's `ViewedChain`) and a press on
		// a page thumbnail changes the page (`PadsPage`) — asserted by value in `test_gui_widgets_rackviews` ("a press on a tab views that
		// chain", "a press on page 5 changes the rack's own PadsPage").
		e.reached.insert(static_cast<size_t>(pr::Param::ViewedChain));
		e.reached.insert(static_cast<size_t>(pr::Param::PadsPage));
		e.reached.insert(static_cast<size_t>(pr::kTraits.enableControl));
		rackView.reset(); // before its device goes
		REQUIRE(f.bridge.removeFxDevice(slot));
		f.lane->resized();

		// AND THE CHAIN SLOTS (backlog NO item 2): a slot param is reached by the level, mute and solo of the chain that
		// holds it — so a rack with `kChainSlots` chains, a body to hold them, and the window's strip for each, asked which slot
		// param its three controls route to.
		feathers::rack::RackEngine bodies { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		bodies.prepare(48000.0, 4096);
		const size_t at = bodies.addDevice(pr::kTraits.tag);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		void* body = bodies.slotAtLane(at).storage.get();
		REQUIRE(body != nullptr);
		const size_t slotted = f.bridge.addFxDevice(pr::kTraits.tag);
		REQUIRE(slotted < feathers::kMaxFxSlots);
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slotted);
		f.bridge.fxBodies().attach(id, body);
		while (pr::rackOf(body)->chainCount() < pr::kChainSlots) {
			REQUIRE(pr::rackOf(body)->addChain() != SIZE_MAX);
		}
		ui::WidgetRegistry slotRegistry;
		auto slotView = ui::makeExpandedView(f.deviceHost, &f.bridge, slotRegistry, id, pr::kDescriptor);
		auto* slotPanel = dynamic_cast<ui::ParallelRackExpanded*>(slotView.get());
		REQUIRE(slotPanel != nullptr);
		slotPanel->setBounds(0.0f, 0.0f, 900.0f, 600.0f);
		(void)slotPanel->update();
		REQUIRE(slotPanel->railCountForTest() == pr::kChainSlots);
		for (size_t i = 0; i < slotPanel->railCountForTest(); ++i) {
			ui::ChainStrip& strip = slotPanel->stripForTest(i);
			const std::pair<ui::ChainControl*, pr::SlotControl> three[] = {
				{ &strip.levelControlForTest(), pr::SlotControl::Level },
				{ &strip.muteControlForTest(), pr::SlotControl::Mute },
				{ &strip.soloControlForTest(), pr::SlotControl::Solo },
			};
			for (const auto& [control, which] : three) {
				if (control->desc().modulatable && control->slot() < pr::kChainSlots) {
					e.reached.insert(pr::slotParam(control->slot(), which));
				}
			}
		}
		slotView.reset();
		f.bridge.fxBodies().detach(id, body);
		REQUIRE(f.bridge.removeFxDevice(slotted));
		f.lane->resized();
		table.push_back(std::move(e));
	}

	// THE CHAIN, REACHED THROUGH THE LANE — the first device that holds other devices,
	// on D6's OWN CARD since 2026-09-13: the summary as the
	// head, the eight macros and mix and gain as the body (`ChainCard`). It fell to `GenericCard` before,
	// and this entry's cast said so; the cast changing type here is the record of the card arriving.
	//
	// THROUGH THE LANE, AS BUS UTILITY ABOVE, because it is the stronger of the two claims: it proves
	// a hand adding a chain gets a card at all, and the `dynamic_cast` proves which one.
	//
	// ALL ELEVEN: the ten knobs the card lists and the enable in the head.
	{
		const size_t chainSlot = f.bridge.addFxDevice(feathers::composite::kTraits.tag);
		REQUIRE(chainSlot < feathers::kMaxFxSlots);
		f.lane->resized();
		auto* chainCard = dynamic_cast<ui::ChainCard*>(f.lane->cardAt(chainSlot));
		REQUIRE(chainCard != nullptr);
		Entry e { feathers::composite::kTraits.tag, {}, {}, false };
		for (const feathers::composite::Param p : chainCard->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// THE HEAD'S SQUARE, as the tracker's and note-expressions' entries: the grid omits the enable
		// and the head draws it, so leaving it out would report a division of labour as a gap.
		if (feathers::composite::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::composite::kTraits.enableControl));
		}
		table.push_back(std::move(e));

	{
		// SKLITTER reaches its controls through `GenericCard`, which draws one knob per descriptor row
		// — so every control is reachable EXCEPT the two the shared head draws for itself.
		//
		// **NO GAPS BEYOND THOSE TWO, and that is worth stating rather than assuming.** A bespoke card
		// is what made OWLTT unreachable (fifteen of twenty-five controls, above); the generic one
		// cannot, because it is derived from the declaration rather than hand-laid. The day SKLITTER
		// gets a card of its own, this entry is where the loss would show.
		Entry e { "SKLITTER", {}, {}, false };
		// **ASKED OF THE CARD THE LANE BUILDS** (backlog FY, 2026-10-30): this was every index by hand, so a SKLITTER
		// card that dropped a row could not show here. `SklitterCard` is a `GenericCard`; the rows it built are its own
		// claim (`paramIndicesForTest`, one per control it laid a widget for).
		{
			// A FIXTURE OF ITS OWN: this table has filled the eight fx slots by here (see the Delay's note below).
			FilterFixture g;
			const size_t sklitterSlot = g.bridge.addFxDevice(fx::sklitter::kTraits.tag);
			REQUIRE(sklitterSlot < feathers::kMaxFxSlots);
			g.bridge.refresh();
			g.lane->resized();
			auto* sklitterCard = dynamic_cast<ui::SklitterCard*>(g.lane->cardAt(sklitterSlot));
			REQUIRE(sklitterCard != nullptr);
			for (const size_t p : sklitterCard->paramIndicesForTest()) {
				e.reached.insert(p);
			}
			// AND ITS WINDOW, the other surface: the coordinate cells, the grain and perform controls live there.
			ui::WidgetRegistry sklitterRegistry;
			ui::SklitterExpanded sklitterPanel { g.deviceHost, sklitterRegistry, g.bridge.fxDeviceAt(sklitterSlot),
				                                    fx::sklitter::kDescriptor };
			for (const size_t p : sklitterPanel.paramIndicesForTest()) {
				e.reached.insert(p);
			}
			REQUIRE(g.bridge.removeFxDevice(sklitterSlot));
			g.bridge.refresh();
			g.lane->resized();
		}
		e.knownGaps = {
		    {static_cast<size_t>(fx::sklitter::Param::Enabled),
		     "Enabled — the bypass, drawn as the shared head's enable square rather than a knob"},
		};
		table.push_back(std::move(e));
	}

	{
		// **THE LFO, ASKED OF THE CARD THE STRIP BUILDS** (backlog FY, 2026-10-30). This entry said the LFO went through
		// `GenericCard` and wrote every index out by hand; the strip builds `LfoCard`, whose seam was the one that once
		// returned `{}` — and THIS table, the master guard, never read it. Seated like the Stereo Tool, and taken out again.
		Entry e { feathers::mod::lfo::kTraits.tag, {}, {}, false };
		// A FIXTURE OF ITS OWN: the strip holds four and this table has filled it by here.
		FilterFixture g;
		const size_t lfoSlot = g.bridge.addRackDevice(feathers::mod::lfo::kTraits.tag);
		REQUIRE(lfoSlot < feathers::kMaxRackSlots);
		g.bridge.refresh();
		g.panel->resized();
		auto* lfoCard = dynamic_cast<const ui::LfoCard*>(g.panel->cardFrameForTest(lfoSlot));
		REQUIRE(lfoCard != nullptr);
		for (const size_t p : lfoCard->addressedFieldsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// PING-PONG IS HIDDEN (2026-11-22), ON EVERY TAB: the engine never reads it, so no card reaches it. Its key stays.
		for (size_t t = 0; t < feathers::mod::lfo::kTabs; ++t) {
			e.knownGaps.emplace(feathers::mod::lfo::tabParam(t, feathers::mod::lfo::Param::PingPong),
			                    "Ping-Pong — hidden: the engine never reads it, so no card reaches it");
		}
		table.push_back(std::move(e));
		REQUIRE(g.bridge.removeRackDevice(lfoSlot));
		g.bridge.refresh();
		g.panel->resized();
	}

	// THE DELAY reaches its controls through `GenericCard`, asked of the card exactly as the tracker
	// and the LFO above are.
	//
	// **BUILT DIRECTLY RATHER THAN SEATED IN THE LANE, and that is forced rather than preferred.**
	// Bus Utility's entry takes the stronger road — seat it and `dynamic_cast` what the lane chose,
	// which also proves the factory did or did not answer — and this one cannot: `kMaxFxSlots` is 8
	// and this case has already filled all eight by the time it gets here. A ninth `addFxDevice`
	// returns 8 and the REQUIRE fires. So this is the tracker's idiom, for the tracker's reason.
	//
	// **WHAT THAT COSTS, SAID PLAINLY: this entry cannot see a card factory entry being ADDED for
	// the delay.** If one ever is, the lane would draw that card and this table would go on
	// reporting the generic one's controls — passing while describing a surface nobody sees. The
	// four grain controls are the ones that would hide, since a bespoke face is exactly where they
	// would move. Whoever writes a `DelayCard` re-points this entry at it.
	//
	// **`kNoInstance`, NOT A BORROWED DEVICE.** `paramIndicesForTest` is a claim about the
	// DESCRIPTOR — which controls the card put a knob on — and handing it another device's instance
	// would make the gated controls' visibility depend on that device's field 7. The knob list is
	// built from the descriptor and does not need an instance to be enumerated.
	{
		ui::GenericCard delayCard { f.deviceHost, kNoInstance, feathers::fx::delay::kDescriptor,
			                        nullptr };
		Entry e { feathers::fx::delay::kTraits.tag, {}, {}, false };
		for (const size_t index : delayCard.paramIndicesForTest()) {
			e.reached.insert(index);
		}
		// AND THE ENABLE, WHICH THE HEAD DRAWS RATHER THAN THE GRID — the division of labour the
		// tracker entry above describes.
		if (feathers::fx::delay::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::delay::kTraits.enableControl));
		}
		// AND THE WINDOW (delay-love S2-S4), the union, built through `makeExpandedView` so the entry table's arm is asserted
		// too: it holds the card's controls (the superset guard) and CLEAR, which the card does not draw — a momentary a grid
		// cell would latch.
		ui::WidgetRegistry delayRegistry;
		auto delayView = ui::makeExpandedView(f.deviceHost, &f.bridge, delayRegistry, kNoInstance,
		                                      feathers::fx::delay::kDescriptor);
		auto* delayPanel = dynamic_cast<ui::DelayExpanded*>(delayView.get());
		REQUIRE(delayPanel != nullptr);
		for (const feathers::fx::delay::Param p : delayPanel->addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		CHECK(e.reached.count(static_cast<size_t>(feathers::fx::delay::Param::Clear)) == 1);
		table.push_back(std::move(e));
	}
	// THE ERODER: its own card (`EroderCard`) AND its window, each asked what its widgets offer — read off the tree, every
	// engine's controls and the plot's handles included (`ControlFace`). The shared section and Etch's slots are on both.
	{
		ui::WidgetRegistry eroderRegistry;
		ui::EroderCard eroderCard { f.deviceHost, eroderRegistry, kNoInstance };
		ui::EroderExpanded eroderPanel { f.deviceHost, eroderRegistry, kNoInstance };
		Entry e { feathers::fx::eroder::kTraits.tag, {}, {}, false };
		for (const feathers::fx::eroder::Param p : eroderCard.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		for (const feathers::fx::eroder::Param p : eroderPanel.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND THE ENABLE, WHICH THE CARD'S HEAD DRAWS (and the overlay's header, for the window).
		if (feathers::fx::eroder::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::eroder::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}
	// THE SMOOTH SHAPER: its own card AND its panel, each listing what a hand reaches on it — the card leaves the
	// three phases to the panel, and Drive is the curve's ceiling gesture on both. Built directly, the lane
	// being full by here.
	{
		ui::WidgetRegistry smoothRegistry;
		ui::SmoothCard smoothCard { f.deviceHost, smoothRegistry, kNoInstance };
		ui::SmoothExpanded smoothPanel { f.deviceHost, smoothRegistry, kNoInstance };
		Entry e { feathers::fx::smooth::kTraits.tag, {}, {}, false };
		for (const feathers::fx::smooth::Param p : smoothCard.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		for (const feathers::fx::smooth::Param p : smoothPanel.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		if (feathers::fx::smooth::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::smooth::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}
	// SPECTRAL: its own card (`SpectralCard`), the plot's handles among what it reaches — by the delay's road.
	{
		ui::WidgetRegistry spectralRegistry;
		ui::SpectralCard spectralCard { f.deviceHost, spectralRegistry, kNoInstance, nullptr };
		Entry e { feathers::fx::spectral::kTraits.tag, {}, {}, false };
		for (const feathers::fx::spectral::Param p : spectralCard.addressedParamsForTest()) {
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND THE WINDOW'S OWN PANELS (the Vocode panel's knobs are the window's alone).
		ui::SpectralExpanded spectralWindow { f.deviceHost, spectralRegistry, kNoInstance, nullptr };
		for (size_t p = 0; p < feathers::fx::spectral::kParamCount; ++p) {
			if (spectralWindow.boxWidgetForTest(static_cast<feathers::fx::spectral::Param>(p)) != nullptr) {
				e.reached.insert(p);
			}
		}
		// AND THE DRAWER'S PANELS THE WINDOW SHARES (Formant Shift's knobs and Link are there, not on the card).
		for (size_t c = 0; c < static_cast<size_t>(feathers::SpectralControl::kCount); ++c) {
			const auto sc = static_cast<feathers::SpectralControl>(c);
			const auto p = feathers::fx::spectral::DeviceSpectrumSource::paramOf(sc);
			if (p.has_value() && spectralWindow.frameForTest(0, sc) != nullptr) {
				e.reached.insert(static_cast<size_t>(*p));
			}
		}
		// AND THE CURVE'S SWITCH, ON THE WINDOW'S PLOT (C1: the card has no switches).
		e.reached.insert(static_cast<size_t>(feathers::fx::spectral::Param::CurveOn));
		e.reached.insert(static_cast<size_t>(feathers::fx::spectral::kTraits.enableControl));
		table.push_back(std::move(e));
	}
	// THE PITCH SHIFTER, by the delay's road and for the delay's reason — the lane is full.
	{
		ui::GenericCard pitchCard { f.deviceHost, kNoInstance, feathers::fx::pitch::kDescriptor,
			                        nullptr };
		Entry e { feathers::fx::pitch::kTraits.tag, {}, {}, false };
		for (const size_t index : pitchCard.paramIndicesForTest()) {
			e.reached.insert(index);
		}
		if (feathers::fx::pitch::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(feathers::fx::pitch::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}
	{
		// FEATHERVERB: `GenericCard` on the face (six), and `GenericExpanded` as the panel — built
		// through `makeExpandedView`, so this also asserts the entry table has its arm. The panel is
		// the only surface that seats `Size Mode` as a zone knob with `Size` inside it (by design), and
		// that is asserted too: one mode cell, not a Choice box and a stray knob.
		namespace fv = feathers::fx::featherverb;
		ui::GenericCard verbCard { f.deviceHost, kNoInstance, fv::kDescriptor, nullptr };
		ui::WidgetRegistry verbRegistry;
		// No lane slot: this fixture's lane is full by here, and the card above is built the same way.
		auto verbView = ui::makeExpandedView(f.deviceHost, &f.bridge, verbRegistry, kNoInstance,
		                                     fv::kDescriptor);
		REQUIRE(verbView != nullptr);
		auto* verbPanel = dynamic_cast<ui::GenericExpanded*>(verbView.get());
		REQUIRE(verbPanel != nullptr);
		// TWO: Size Mode, and Conv Mode with Blend as its box (backlog JD).
		CHECK(verbPanel->modeCellsForTest().size() == 2);
		CHECK(verbPanel->followsModeCell(static_cast<size_t>(fv::Param::ConvBlend)));
		CHECK(verbPanel->followsModeCell(static_cast<size_t>(fv::Param::Size)));
		// CHARACTER IS UNGATED — a zone knob of its own in every mode, never a box inside Size's cell.
		CHECK_FALSE(verbPanel->followsModeCell(static_cast<size_t>(fv::Param::Character)));

		Entry e { fv::kTraits.tag, {}, {}, false };
		for (const size_t index : verbCard.paramIndicesForTest()) {
			e.reached.insert(index);
		}
		for (const size_t index : verbPanel->paramIndicesForTest()) {
			e.reached.insert(index);
		}
		if (fv::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(fv::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}
	// RESONATE: its OWN card (`ResonateCard`) and its panel (`ResonateExpanded`), both over one
	// `ResonateFace`. BUILT DIRECTLY rather than seated in the lane, for the delay's reason — the lane is
	// full by here — and the panel through `makeExpandedView`, so this also asserts the entry table's arm.
	{
		namespace rs = feathers::fx::resonate;
		ui::WidgetRegistry resonateRegistry;
		ui::ResonateCard resonateCard { f.deviceHost, resonateRegistry, kNoInstance, nullptr };
		auto resonateView = ui::makeExpandedView(f.deviceHost, &f.bridge, resonateRegistry, kNoInstance,
		                                         rs::kDescriptor);
		REQUIRE(resonateView != nullptr);
		auto* resonatePanel = dynamic_cast<ui::ResonateExpanded*>(resonateView.get());
		REQUIRE(resonatePanel != nullptr);
		Entry e { rs::kTraits.tag, {}, {}, false };
		// THE SPLIT, NOT ONLY THE TOTAL ("card is far too big, lets greatly simplify it"):
		// the card reaches TEN — its six knobs, the Model, Character, Morph, and the head's Enabled — and the
		// panel every control but Enabled. Together, every control.
		std::set<size_t> card;
		for (const rs::Param p : resonateCard.addressedParamsForTest()) {
			card.insert(static_cast<size_t>(p));
			e.reached.insert(static_cast<size_t>(p));
		}
		CHECK(card.size() == 10);
		std::set<size_t> panel;
		for (const rs::Param p : resonatePanel->addressedParamsForTest()) {
			panel.insert(static_cast<size_t>(p));
			e.reached.insert(static_cast<size_t>(p));
		}
		// AND THE ROUTING SWITCH, which the panel's DRONE FX rail seats rather than the face (2026-10-05). That its
		// chip WRITES that control is asserted by value in `test_gui_widgets_devices` ("RESONATE's panel: DRONE FX
		// and WET FX rails ..."); here, that the panel has it.
		REQUIRE(resonatePanel->routeChipForTest().label() == ui::ResonateExpanded::kRouteLabel);
		panel.insert(static_cast<size_t>(rs::Param::DroneToWetFx));
		e.reached.insert(static_cast<size_t>(rs::Param::DroneToWetFx));
		CHECK(panel.size() == static_cast<size_t>(rs::Param::kCount) - 1);
		CHECK(e.reached.size() == static_cast<size_t>(rs::Param::kCount));
		table.push_back(std::move(e));
	}
	// THE DRUM: its OWN card (`DrumDeviceCard`) and window (`DrumDeviceExpanded`), asked what each OFFERS through `ui::ControlFace`,
	// which every widget that writes a device control declares (value boxes, pills, segmented controls, the picture's handles, the
	// terrain pad, the pitch readout). Built directly, over no instance: what a widget declares does not depend on a row. The head's
	// Enabled chip is the header's, as for every device.
	{
		namespace ddv2 = feathers::fx::drumdevice;
		ui::WidgetRegistry drumRegistry;
		ui::DrumDeviceCard drumCard { f.deviceHost, drumRegistry, kNoInstance, nullptr };
		ui::DrumDeviceExpanded drumWindow { f.deviceHost, drumRegistry, kNoInstance, nullptr };
		drumWindow.setBounds(0.0f, 0.0f, ui::DrumDeviceExpanded::kWidth, ui::DrumDeviceExpanded::kMinimumHeight);
		Entry e { ddv2::kTraits.tag, {}, {}, false };
		const auto offered = [&](visage::Frame* root, std::set<size_t>& into) {
			const std::function<void(visage::Frame*)> walk = [&](visage::Frame* fr) {
				if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr)) {
					std::vector<ui::ControlFace::Offered> list;
					face->offeredControls(list);
					for (const auto& o : list) {
						const auto* d = &o.control->desc();
						if (d >= std::begin(ddv2::kParamDescs) && d < std::end(ddv2::kParamDescs)) {
							into.insert(static_cast<size_t>(d - std::begin(ddv2::kParamDescs)));
						}
					}
				}
				for (visage::Frame* c : fr->children()) {
					walk(c);
				}
			};
			walk(root);
		};
		std::set<size_t> card;
		std::set<size_t> window;
		offered(&drumCard, card);
		offered(&drumWindow, window);
		CHECK(card.size() >= 4);
		e.reached = card;
		e.reached.insert(window.begin(), window.end());
		if (ddv2::kTraits.enableControl >= 0) {
			e.reached.insert(static_cast<size_t>(ddv2::kTraits.enableControl));
		}
		table.push_back(std::move(e));
	}
	}

	size_t visited = 0;
	size_t controlsChecked = 0;
	for (const feathers::device::Descriptor& desc : feathers::kDevices) {
		// **THE WHOLE TAG, NOT FOUR BYTES OF IT.** `device::kTagBytes` went from four to eight on
		// 2026-09-14, and this truncated "SCATTER" (now "SKLITTER") to "SCAT" -- so the lookup missed and the REQUIRE
		// below reported a device with no entry when the entry was right there. `traits.tag` is a
		// NUL-terminated literal, so the plain constructor is the right one.
		const std::string tag(desc.traits.tag);
		const auto entry = std::find_if(table.begin(), table.end(), [&](const Entry& e) {
			return std::string(e.tag) == tag;
		});
		// EVERY DEVICE NEEDS AN ENTRY, which is what makes a NEW device fail here rather than sail
		// past. A table keyed on a list it does not have to cover is a table that stops covering it.
		CAPTURE(tag);
		REQUIRE(entry != table.end());
		++visited;

		CHECK_FALSE(desc.params.empty());
		if (entry->noEditorInThisBuild) {
			// NAMED, NOT SKIPPED. Nothing reaches any of them, and the assertion says so in a form
			// that fails the moment something does.
			CHECK(entry->reached.empty());
			continue;
		}

		for (size_t i = 0; i < desc.params.size(); ++i) {
			CAPTURE(std::string(desc.params[i].name));
			const bool reached = entry->reached.count(i) > 0;
			// **THE CHAIN FURNITURE IS REACHED BY THE SITE PANEL'S HEAD ROW, NOT BY THE CARD** (backlog NR,
			// 2026-10-26: the custom cards are left alone). A control the slot applies (`Traits::mixControl` /
			// `wetGainControl`, and not a mix the device makes itself) that the card does not draw is the head row's;
			// that it IS drawn there, one knob per declared role, is asserted by value in test_gui_widgets_nest
			// ("NR: the seven custom-card devices ...").
			const bool furniture = !desc.traits.mixInDevice
			                       && (static_cast<int>(i) == desc.traits.mixControl
			                           || static_cast<int>(i) == desc.traits.wetGainControl);
			const auto gap = entry->knownGaps.find(i);
			if (furniture && !reached && gap == entry->knownGaps.end()) {
				++controlsChecked;
				continue;
			}
			if (gap != entry->knownGaps.end()) {
				CAPTURE(std::string(gap->second));
				CHECK_FALSE(reached);
			}
			else {
				CHECK(reached);
			}
			++controlsChecked;
		}
	}

	// NOT VACUOUS, THREE WAYS.
	CHECK(visited == feathers::kDeviceCount);
	// The loop really ran over every control of every device that has a card — 20 + 15 + 37 on the
	// day this was written, and stated as a floor so adding a control does not need this number
	// changed while removing all of them still fails.
	CHECK(controlsChecked > 60);
	// AND `reached` IS NOT ANSWERING YES TO EVERYTHING, which is the way this case would rot into a
	// formality. The MSEG's own table is the demonstration: it contains a control it CAN reach and
	// not the one it cannot, one enumerator apart in the same declaration block.
	const Entry& msegEntry = *std::find_if(table.begin(), table.end(), [&](const Entry& e) {
		return std::string(e.tag, 4) == std::string(mseg::kTraits.tag, 4);
	});
	CHECK(msegEntry.reached.count(static_cast<size_t>(mseg::Param::SegmentCount)) == 1);
	CHECK(msegEntry.reached.count(static_cast<size_t>(mseg::Param::SustainSegment)) == 1);
	// THE NEGATIVE HAS MOVED DEVICES, because the MSEG no longer has one to give. `SustainSegment`
	// was the demonstration that `reached` says no to something — it says yes now, so the filter's
	// `Type` takes the job: declared, drawn as a dropdown in the head rather than as a control the
	// card addresses, and listed in that entry's own knownGaps two hundred lines above. If this ever
	// starts reading 1, the sets have stopped discriminating and every CHECK above is vacuous.
	const Entry& filterEntry = *std::find_if(table.begin(), table.end(), [&](const Entry& e) {
		return std::string(e.tag, 4) == std::string(flt::kTraits.tag, 4);
	});
	CHECK(filterEntry.reached.count(static_cast<size_t>(flt::Param::Type)) == 0);
	// And an index past the end of any device's declarations is reached by nothing, or the sets are
	// answering yes to arithmetic rather than to controls.
	CHECK(msegEntry.reached.count(9999) == 0);
}
TEST_CASE("an MSEG's controls address the MSEG's own destinations, not an envelope's") {
	// A SHIPPED BUG, FOUND BY LOOKING AT SOMETHING ELSE. `deviceSlotDescriptor(slot)` answers
	// `isFxDeviceSlot(slot) ? filter : dahdsr` — a COMPILE-TIME branch that never consults the rack —
	// so every rack slot claims to be a DAHDSR whatever it actually holds.
	//
	// `deviceDestinationOfControl` looks the descriptor up that way, so it converted an MSEG's control
	// index through the ENVELOPE's modulatable mapping. Measured before the fix: a slot holding an
	// MSEG reported 15 params and 10 modulatable, where the MSEG declares 37 and 4. `TimeScale` is
	// control 5, so it took the DAHDSR's ordinal for control 5 — RELEASE — and a route aimed at an
	// MSEG's Rate landed on the column reserved for an envelope's Release.
	//
	// The device-agnostic half is genuinely agnostic: `deviceDestinationOfOrdinal` is
	// `kFirstDeviceDestination + slot * stride + ordinal`, positional arithmetic that does not care
	// what is in the slot. Only the ORDINAL had to come from the right device.
	namespace mseg = feathers::mod::mseg;
	ModFixture f;
	feathers::ui::ModState mod;

	feathers::ModRack rack = f.bridge.modRack();
	rack.setMsegAt(0, mseg::Settings {});
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

	const DeviceInstanceId msegId = f.bridge.rackDeviceAt(0);
	// BY TAG — the host's catalogue answers its own copy of the declaration, so the pointer is not
	// `&mseg::kDescriptor`; the device is.
	const feathers::device::Descriptor* msegDesc = f.bridge.descriptorOf(msegId);
	REQUIRE(msegDesc != nullptr);
	REQUIRE(feathers::device::sameTag(msegDesc->traits.tag, mseg::kTraits.tag));

	// **THE DESTINATION IS THE MSEG AND ITS OWN CONTROL INDEX (kill-lanes step 2)** — no ordinal is
	// computed through anybody's mapping any more, which is the class of bug this case was written for.
	// Rate is the sixth declared control; on an envelope index 5 would be Release, and the address now
	// cannot say "envelope" because it names this device by id.
	feathers::ui::MsegControl rate { f.bridge, msegId, mseg::Param::TimeScale, &mod };
	CHECK(rate.destination()
	      == backplane::Destination::control(msegId, static_cast<size_t>(mseg::Param::TimeScale)));
	// AND THE MODULATABLE-OR-NOT ANSWER IS THE MSEG'S, asked of the device under the id — which is where
	// the envelope's answer came from under the old compile-time branch.
	CHECK(rate.isDestination());

	SUBCASE("and a control the MSEG does NOT expose is refused an address rather than given one") {
		// THE OTHER HALF OF THE SAME BUG, and the half that hands an address to something that should
		// have none. `SustainSegment` is control 2 and is stepped — which breakpoint holds the gate —
		// so the MSEG declines to make it a destination. Control 2 on the ENVELOPE is Hold, one of its
		// ten continuous controls, so the envelope's mapping answers with a real column.
		//
		// Deliberately NOT a segment breakpoint. Every segment control is index 13 or above; the
		// envelope declares 15 controls of which the last two are switches, so its mapping refuses
		// those anyway and the check would pass under either descriptor. MEASURED — the first draft of
		// this subcase used Seg0Level and was green against the known-bad.
		REQUIRE_FALSE(
		    mseg::kDescriptor.params[static_cast<size_t>(mseg::Param::SustainSegment)].modulatable);
		feathers::ui::MsegControl sustainAt { f.bridge, msegId, mseg::Param::SustainSegment, &mod };
		CHECK_FALSE(sustainAt.isDestination());
		CHECK_FALSE(sustainAt.display().isDestination);
		CHECK_FALSE(f.bridge.mayRoute(backplane::Source::core(0), sustainAt.destination()));

		// And the envelope's declaration WOULD make its control 2 one, or the check above is about a
		// distinction nothing draws.
		CHECK(feathers::mod::dahdsr::kDescriptor.params[static_cast<size_t>(mseg::Param::SustainSegment)]
		          .modulatable);
	}

	SUBCASE("and every modulatable MSEG control gets a distinct destination") {
		// Four controls, four columns, no collisions — which a mapping through the wrong device could
		// easily violate by folding two of them onto one ordinal.
		std::vector<backplane::Destination> seen;
		for (size_t ordinal = 0; ordinal < mseg::kDescriptor.modulatableCount(); ++ordinal) {
			const size_t control = mseg::kDescriptor.controlOfNthModulatable(ordinal);
			feathers::ui::MsegControl c { f.bridge, msegId, static_cast<mseg::Param>(control), &mod };
			const backplane::Destination dest = c.destination();
			CAPTURE(ordinal);
			CAPTURE(dest.index);
			CHECK(c.isDestination());
			CHECK(std::find(seen.begin(), seen.end(), dest) == seen.end());
			seen.push_back(dest);
		}
		CHECK(seen.size() == mseg::kDescriptor.modulatableCount());
	}
}
TEST_CASE("the strip's (+) offers only devices the RACK will take, and adding one puts it there") {
	// THE BROWSER'S GESTURE, END TO END. Everything under it exists — addRackDevice, the seating
	// predicates, the registry — and none of it was reachable from a hand until this button.
	//
	// **THIS CASE PINS THE CAPABILITY HALF AND THE GESTURE, NOT THE WHOLE RULE.** Since 2026-09-17 the
	// menu is `maySitInModRack(d) && isTaggedModulator(d)`, so "offers the devices the rack will take"
	// is a SUBSET claim and no longer an identity — the identity, tag and all, is the case below
	// ("the strip offers exactly the devices that may sit there AND are tagged a modulator"). The title
	// and the checks here were rewritten to say only what stays true, because this is the older and more
	// established-looking case: a reader landing here first was learning a rule the panel had stopped
	// following, and at Task 8 it would have reddened with a message that read as "SKLITTER should have
	// been offered" — the opposite of the change that reddened it.
	namespace flt = feathers::fx::filter;
	ModFixture f;
	visage::Canvas canvas;

	const size_t before = f.panel->deviceCount();
	REQUIRE(before > 0); // the default rack, so "it grew" is not "it appeared"

	SUBCASE("what it offers is derived from the devices, not listed here") {
		const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();

		// ONLY DEVICES THAT DECLARE A MODULATION OUTPUT — the honesty rule's direction, and the one that
		// survives the tag. Everything offered must be seatable, or the click looks like it worked and
		// nothing appears. Restated from the registry rather than compared against a number, so a device
		// added to the build joins this claim without anyone editing it.
		//
		// **`capable` IS DELIBERATELY NOT COMPARED FOR EQUALITY, AND THAT IS THE DECISION THIS REVIEW
		// ASKED FOR.** It used to be: `offered.size() == count(outputCount() > 0)`. That equality is
		// exactly the claim the tag filter ends — it is green today only by the coincidence the case
		// below documents — so making it the conjunction would just duplicate that case, and leaving it
		// as an equality would make this case red for the wrong reason the day a capable device declines
		// the tag. A SUBSET is the permanent capability claim, and it is not vacuous: it is what fails if
		// the menu ever offers something the rack refuses.
		size_t capable = 0;
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			if (d.outputCount() > 0) {
				++capable;
			}
		}
		CHECK(offered.size() <= capable);
		CHECK(offered.size() > 0); // a subset claim is satisfied by an empty list; this says it is not one
		for (const feathers::device::Descriptor* d : offered) {
			CAPTURE(std::string(d->traits.name));
			CHECK(d->outputCount() > 0);
			CHECK(feathers::maySitInModRack(*d));
		}

		// AND THE ONES IT MUST NOT OFFER, named rather than left to the count. A filter renders audio
		// and publishes nothing, so a rack that accepted it would hold a source that is never a
		// source — and the picker would be offering a device the road under it refuses.
		const auto offers = [&](const char* tag) {
			for (const feathers::device::Descriptor* d : offered) {
				if (feathers::device::sameTag(d->traits.tag, tag)) {
					return true;
				}
			}
			return false;
		};
		CHECK(offers(mod::dahdsr::kTraits.tag));
		CHECK(offers(mod::mseg::kTraits.tag));
		CHECK_FALSE(offers(flt::kTraits.tag));
		CHECK_FALSE(offers(fx::harm::kTraits.tag));

		// THE PICKER IS SHORTER THAN THE REGISTRY, which is the derived rule doing visible work rather
		// than a coincidence worth asserting: if every device were offered everywhere, none of the
		// checks above would be distinguishable from "the list is the table".
		CHECK(offered.size() < std::size(feathers::kDevices));
	}

	SUBCASE("choosing one appends it, and the strip grows a card for it") {
		const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();
		REQUIRE(offered.size() >= 2);

		// The SECOND entry, deliberately: choosing the first would agree with an implementation that
		// ignored the choice and always added whatever the rack's default device is.
		const char* chosen = offered[1]->traits.tag;
		const size_t slot = f.panel->addChoiceForTest(1);
		REQUIRE(slot == before);

		CHECK(f.bridge.modRack().count == before + 1);
		CHECK(f.bridge.modRack().holds(slot, chosen));
		CHECK(f.panel->deviceCount() == before + 1);

		// AND THE STRIP DREW IT. deviceCount is the rack's number; a card is what a hand sees, and the
		// two came apart once already when the panel cached its cards by index.
		f.panel->draw(canvas);
		CHECK(f.panel->cardFrameForTest(slot) != nullptr);
		CHECK(f.panel->envCardBounds(slot).height() > 0.0f);
	}

	SUBCASE("the button sits where the next device will go, and the strip can be scrolled to it") {
		const visage::Bounds add = f.panel->addBounds();
		CHECK(add.height() == doctest::Approx(ui::ModPanel::kAddHeight));

		// BELOW THE LAST CARD AND ABOVE THE CV PLACEHOLDER — the order that makes it a picture of an
		// append rather than of an insertion.
		CHECK(add.y() >= f.panel->envCardBounds(before - 1).bottom());
		CHECK(f.panel->cvCardBounds().y() >= add.bottom());

		// AND THE SCROLL EXTENT KNOWS ABOUT IT — ASKED AT THE BOTTOM OF THE STRIP, which is where a
		// forgotten row actually shows. A first draft checked that the (+) itself stays on screen at
		// full scroll and was GREEN against a contentHeight that had never heard of it: the (+) is
		// not the last row, so the CV card and the bottom padding below it were slack enough to hide
		// 32 missing pixels. What the sum really guarantees is that the LAST row can be reached, so
		// that is what this asks.
		f.panel->setBounds(f.panel->x(), f.panel->y(), ui::ModPanel::kWidth, 120.0f);
		f.panel->setScroll(f.panel->maxScroll());
		REQUIRE(f.panel->maxScroll() > 0.0f); // or the strip fits and there is nothing to reach
		const visage::Bounds bottomRow = f.panel->cvCardBounds();
		CAPTURE(bottomRow.y());
		CAPTURE(f.panel->height());
		CHECK(bottomRow.bottom() <= f.panel->height() + 1.0f);
	}

	SUBCASE("the press lands on the (+) and not on whatever is under it") {
		// ASKED OF THE HIT TEST, WHICH IS WHERE THIS FILE ASKS IT. The button opens a PopupMenu and a
		// popup wants a window, so the gesture is driven no further here — the same line the filter
		// card's dropdown cases stop at, for the same reason. What is not being claimed is that a
		// press opens the menu; what IS claimed is that the press reaches this part rather than the
		// card above it or the placeholder below.
		const visage::Bounds add = f.panel->addBounds();
		CHECK(f.panel->hitAt({ add.x() + add.width() * 0.5f, add.y() + add.height() * 0.5f }).part
		      == ui::ModPanel::Part::Add);

		// AND ITS NEIGHBOURS STILL ANSWER FOR THEMSELVES, which is the half that catches a target
		// drawn one row too tall: a (+) that had swallowed the card above it would pass the check
		// above and take every press meant for the last device's head.
		const visage::Bounds lastCard = f.panel->envCardBounds(before - 1);
		CHECK(f.panel->hitAt({ lastCard.x() + lastCard.width() * 0.5f,
		                       lastCard.y() + ui::ModPanel::kHeadHeight * 0.5f })
		          .part
		      != ui::ModPanel::Part::Add);
		const visage::Bounds cv = f.panel->cvCardBounds();
		CHECK(f.panel->hitAt({ cv.x() + cv.width() * 0.5f, cv.y() + cv.height() * 0.5f }).part
		      != ui::ModPanel::Part::Add);
	}
}
// **THE TAG, ANCHORED BY NAME AT COMPILE TIME — and this is not belt-and-braces, it closes a live hole.**
//
// `isTaggedModulator` compares against a STRING, so a misspelling is SILENT ABSENCE and not a build error:
// the device simply stops being offered and nothing says why. That failure mode already exists in the tree
// one letter away — `fx::automod` declares `{"modulation", "fx"}`, on the device whose whole name is
// modulation — and it is excluded today only because it publishes no outputs, which is luck rather than
// design. A runtime loop in the case below used to anchor the predicate against `outputCount()` while the
// two sets coincided; SKLITTER's seven outputs ended that coincidence and the loop is gone, so the anchor
// has to exist somewhere that does not depend on it. These do: each names ONE device and ONE expected
// answer, and none of them moved when SKLITTER declared outputs.
//
// THEY LIVE IN THE TEST TU RATHER THAN BESIDE THE PREDICATE. `device_row.h` does include
// `plugin/device_registry.h` and so CAN see these descriptors, but putting them there would make the one
// generic file that knows nothing about any particular device assert about four of them.
static_assert(feathers::isTaggedModulator(feathers::mod::dahdsr::kDescriptor));
static_assert(feathers::isTaggedModulator(feathers::mod::mseg::kDescriptor));
static_assert(feathers::isTaggedModulator(feathers::mod::tracker::kDescriptor));
static_assert(feathers::isTaggedModulator(feathers::mod::noteexpr::kDescriptor));
// THE FIFTH, ADDED BY THE MERGE OF 2026-09-19 — the LFO landed on `lfo-review` while the perform circle was
// on `sklitter-perform`, and the two met here. It declares `{"lfo", "modulator"}` and one output, so it is
// exactly what this predicate is for; the runtime list below reddened naming it, which is the red this case
// was written to produce.
static_assert(feathers::isTaggedModulator(feathers::mod::lfo::kDescriptor));
// AND THE ONE THE FILTER EXISTS FOR. It was excluded by having no outputs; it declares seven now, so this
// line is the whole point — and it needed no edit to become it.
static_assert(!feathers::isTaggedModulator(feathers::fx::sklitter::kDescriptor));
// THE MISSPELLING, PINNED AS A MISSPELLING so that fixing automod's tag is a deliberate act with a red
// line pointing at it, rather than a device quietly appearing in the strip.
static_assert(!feathers::isTaggedModulator(feathers::fx::automod::kDescriptor));
TEST_CASE("the strip offers exactly the devices that may sit there AND are tagged a modulator") {
	// **TWO PREDICATES, AND THE MENU IS THEIR CONJUNCTION.** The case above pins what the RACK will take;
	// this pins what the STRIP OFFERS, which is narrower on purpose: a `modulator` tag that the mod
	// panel's browser filters on by default, which a device such as SKLITTER does not carry.
	// Capability stays non-exclusive (device_row.h); the TAG is the device's own statement about itself.
	ModFixture f;

	// RESTATED FROM THE REGISTRY, not listed here — the same argument the case above makes, so a device
	// added to the build joins this claim without anyone editing it. The order is `kDevices`' order
	// because that is the order `addableForTest` walks, so this compares the sequence and not a set.
	//
	// **AND IT IS NOT AN INDEPENDENT RESTATEMENT OF THE PREDICATE — SAY SO RATHER THAN LET IT READ AS
	// ONE.** Both sides call `maySitInModRack && belongsInModulationRack` (a modulator or a note effect, 2026-11-04), so they move together and this
	// check is blind to the predicate being WRONG. What it pins is the PANEL'S APPLICATION of it: no
	// filter at all, one predicate where two belong, the wrong one, a different registry, a different
	// order, a dropped or duplicated entry. MEASURED, and it is this blindness in the flesh: a
	// known-bad stripping `"modulator"` from the DAHDSR left this check GREEN (both sides dropped it)
	// and reddened only the loop at the bottom. The predicate itself is anchored there instead, against
	// `outputCount()`, which shares no code with it.
	std::vector<const feathers::device::Descriptor*> expected;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		if (feathers::maySitInModRack(d) && feathers::belongsInModulationRack(d)) {
			expected.push_back(&d);
		}
	}
	CHECK(f.panel->addableForTest() == expected);

	// **AND THE FIVE BY NAME, because everything above is computed and a computed list can be computed
	// wrong in both places at once.** This is the runtime half of the static_asserts overhead: it says
	// which devices the strip actually offers TODAY, so a sixth appearing or one of these five vanishing
	// is a red that names the device rather than a count that does not. The order is `kDevices`' own and
	// is NOT the order device_registry.h includes their headers in — the first draft of this line assumed
	// it was and the check said so, which is the check earning its place before it was even committed.
	//
	// **IT WAS FOUR UNTIL THE MERGE OF 2026-09-19, AND THE FIFTH IS WHY THIS LINE EXISTS.** The LFO was
	// built on `lfo-review` while the circle was built on `sklitter-perform`; neither branch's suite could
	// see the other's device, and both were green. The merge reddened exactly here, reading
	// `{DHDS, MSEG, TRAK, NEXP, LFO} == {DHDS, MSEG, TRAK, NEXP}` — a new device joining the strip's
	// offered set, named, rather than a count nobody would have chased. That is the sentence above,
	// happening.
	std::vector<std::string> offeredTags;
	for (const feathers::device::Descriptor* d : f.panel->addableForTest()) {
		offeredTags.push_back(std::string(d->traits.tag));
	}
	CHECK(offeredTags
	      == std::vector<std::string> { feathers::mod::dahdsr::kTraits.tag,
	                                    feathers::mod::mseg::kTraits.tag,
	                                    feathers::mod::tracker::kTraits.tag,
	                                    feathers::mod::noteexpr::kTraits.tag,
	                                    feathers::mod::lfo::kTraits.tag,
	                                    // SIXTH, 2026-09-25: MACROS (macro-modulation B1), four outputs and
	                                    // tagged a modulator — offered by the strip's own picker.
	                                    feathers::mod::macros::kTraits.tag,
	                                    // The one- and eight-macro variants, offered beside it.
	                                    feathers::mod::macro1::kTraits.tag,
	                                    feathers::mod::macros8::kTraits.tag,
	                                    // SEVENTH, 2026-09-25: MATH (macro-modulation C), one output and
	                                    // tagged a modulator.
	                                    feathers::mod::math::kTraits.tag,
	                                    feathers::mod::stereo::kTraits.tag,
	                                    // 2026-10-17: RANDOM TRIGGER (backlog NA (a)).
	                                    feathers::mod::rtrig::kTraits.tag,
	                                    // 2026-10-17: SAMPLE & HOLD (backlog MW), two outputs, tagged a modulator.
	                                    feathers::mod::samplehold::kTraits.tag,
	                                    // NINTH, 2026-10-17: THE STEP SEQUENCER (backlog MX), three outputs.
	                                    feathers::mod::stepseq::kTraits.tag,
	                                    // 2026-10-17: THE FOLLOWER (backlog MY), two outputs and tagged a modulator.
	                                    feathers::mod::follower::kTraits.tag,
	                                    // 2026-10-30: THE ARP, three outputs — tagged a modulator after it was found it in
	                                    // the rack and not in this picker.
	                                    feathers::mod::arp::kTraits.tag,
	                                    // 2026-10-30: THE PATTERN DEVICE (terrain-arp), seven outputs, tagged a modulator.
	                                    feathers::mod::arp::pat::kTraits.tag,
	                                    feathers::mod::twinfield::kTraits.tag });

	// A TAG WITHOUT THE CAPABILITY WOULD OFFER SOMETHING THE RACK REFUSES — the honesty rule for a list.
	// The conjunction hides that case (it would simply drop out), so it is asserted separately: a device
	// that calls itself a modulator and publishes nothing is a declaration bug in that device.
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		INFO(d.traits.tag); // the declared literal, NUL-terminated - not the 8-byte record field
		if (feathers::isTaggedModulator(d)) {
			CHECK(feathers::maySitInModRack(d));
		}
	}

	// **THE TWO LISTS ARE NO LONGER ONE LIST, AND SKLITTER IS THE DEVICE THAT SPLITS THEM.** The loop
	// that stood here asserted `maySitInModRack(d) == isTaggedModulator(d)` for every device and said in
	// its own comment that Task 8 would delete it — that equality was true only while the four
	// modulators were exactly the devices with outputs, and it is false the moment an fx publishes.
	// Deleted and replaced IN THE SAME COMMIT as the outputs, because leaving the predicate unanchored
	// for even one commit is what this half of the case exists to prevent.
	//
	// **AND THIS IS WHAT MAKES THE TAG TEST IN `addableForTest` LIVE RATHER THAN INERT.** Removing
	// `&& isTaggedModulator(d)` from the panel's filter changed nothing while the lists coincided; it now
	// puts SKLITTER in the menu, and `CHECK_FALSE(offers(sklitter))` is the red it earns.
	CHECK(feathers::maySitInModRack(fx::sklitter::kDescriptor));      // capability: it publishes seven
	CHECK_FALSE(feathers::isTaggedModulator(fx::sklitter::kDescriptor)); // and does not call itself one

	// **BY TAG, NOT BY ADDRESS, AND THAT IS NOT A STYLE CHOICE.** `kDevices` holds COPIES of each
	// device's `kDescriptor` (`device_registry.h`'s `inline constexpr device::Descriptor kDevices[]`),
	// so `&fx::sklitter::kDescriptor` is not a pointer `addableForTest()` can ever return and a
	// `std::find` for it can never find one. MEASURED: the first draft of this line WAS that find, and
	// the known-bad that removes `&& isTaggedModulator(d)` from the panel's filter left it green while
	// reddening `addableForTest() == expected` and `offeredTags == {the four}` — an assertion that
	// cannot fail, standing exactly where the case's whole new claim was supposed to be.
	//
	// **NAMED BY WHAT THEY ASSERT, NOT BY POSITION**, and the first draft of THIS sentence said "the two
	// checks above": by then the two checks physically above were `maySitInModRack` and
	// `isTaggedModulator`, two pure predicate calls that a mutation of `ModPanel::addableForTest` cannot
	// touch. The mechanism was right and the referent had rotted — which is what "above" and "below" do
	// the moment a case is reordered or a line is inserted between them.
	const auto offers = [&](const char* tag) {
		for (const feathers::device::Descriptor* d : f.panel->addableForTest()) {
			if (feathers::device::sameTag(d->traits.tag, tag)) {
				return true;
			}
		}
		return false;
	};
	CHECK_FALSE(offers(fx::sklitter::kTraits.tag));
	// AND THE SAME WALK FINDS THE DAHDSR, so "not offered" is an answer about SKLITTER rather than
	// about a comparison that matches nothing.
	CHECK(offers(feathers::mod::dahdsr::kTraits.tag));
}

