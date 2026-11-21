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

#pragma once

#include "fx/eroder/eroder_types.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/pair_cell.h"
#include "plugin/ui/route_switch.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// ETCH'S SLOT ROW — one line, four cells wide, the card's and the window's alike:
//
//   ┌ A ───────────────┬────┬ B ───────────────┐
//   │ (knob) [ Type  ] │ ▪  │ (knob) [ Type  ] │   each slot: its amount, and under the Type its Shape and the Type's own
//   │  A     S  34%    │ │  │  B     S  34%    │   second parameter (Rate for the Paunch Types, Bal for Chonk; none otherwise)
//   │      Rate 0.5Hz  │ ▪▪ │      Bal 100%    │   the route between: Series above, Parallel below
//   └──────────────────┴────┴──────────────────┘
//
// THE WINDOW'S ROW IS THREE PIECES, Amount knob | MIX knob | the cell (`SlotTriple`): the slot's Mix is a knob there and is not in the cell. On the
// card the row stays four cells wide and the slot cell is the same cell (its rows Type, S and the second parameter); the Mix is the window's alone.
//
// **ONE OBJECT BOTH FACES BUILD**, so the two cannot drift: the card and the window each own one, seat `outer` in their
// own layout, and hand it the control accessor they already have. Every widget is bound to its control from construction
// (so a route to a Shape or a Mix is made on its box, and the drag is that control's own).

namespace feathers::ui {

/// **WHICH SECOND-PARAMETER ROW A TYPE SHOWS** — the one small table (`fx::eroder::driveSecondParam`) read by the Drive's cell and the slots'.
struct StageView {
	fx::eroder::DriveMode type = fx::eroder::DriveMode::Soft;
	bool track = false;
};
struct StageParams {
	fx::eroder::Param type, shape, angle, bal, rate, rateMult;
};
inline StageParams driveStageParams() {
	using P = fx::eroder::Param;
	return { P::DriveMode, P::DriveShape, P::DriveAngle, P::DriveBal, P::DriveRate, P::DriveRateMult };
}
inline StageParams slotStageParams(int slot) {
	using P = fx::eroder::Param;
	return slot == 0 ? StageParams { P::SlotAType, P::SlotAShape, P::SlotAAngle, P::SlotABal, P::SlotARate, P::SlotARateMult }
	                 : StageParams { P::SlotBType, P::SlotBShape, P::SlotBAngle, P::SlotBBal, P::SlotBRate, P::SlotBRateMult };
}
/// THE STAGE VIEW from a row: its Type, and whether Track is on in an engine that has it (the Comb has none).
inline StageView stageViewOf(const fx::eroder::Row& row, const StageParams& p) {
	namespace ero = fx::eroder;
	StageView v;
	const int t = std::clamp(static_cast<int>(row[static_cast<size_t>(p.type)] + 0.5f), 0, static_cast<int>(ero::DriveMode::kCount) - 1);
	v.type = static_cast<ero::DriveMode>(t);
	const int mode = std::clamp(static_cast<int>(row[static_cast<size_t>(ero::Param::Mode)] + 0.5f), 0, static_cast<int>(ero::Mode::kCount) - 1);
	v.track = row[static_cast<size_t>(ero::Param::Track)] >= 0.5f && static_cast<ero::Mode>(mode) != ero::Mode::Comb;
	return v;
}
/// THE ROW GATE for a stage's cell: Shape always; Bal for Chonk; Rate (Hz with Track off, the multiple with it on) for the four Paunch Types;
/// every other follower hidden. HIDDEN, NOT GREYED: a row that only a Type switch could enable is not drawn (the honesty rule).
inline std::function<bool(size_t)> stageRowGate(std::shared_ptr<StageView> view, StageParams p) {
	namespace ero = fx::eroder;
	return [view, p](size_t param) {
		const ero::SecondParam second = ero::driveSecondParam(view->type);
		// THE S ROW IS ONE PLACE WITH TWO CONTROLS: Shape for every Type but the four Paunch Types, whose Angle (a key of its own) takes the place.
		if (param == static_cast<size_t>(p.shape)) {
			return !ero::driveIsPaunchLaw(view->type) && !ero::driveIsPaunchOdd(view->type);
		}
		if (param == static_cast<size_t>(p.angle)) {
			return ero::driveIsPaunchLaw(view->type) || ero::driveIsPaunchOdd(view->type);
		}
		if (param == static_cast<size_t>(p.bal)) {
			return second == ero::SecondParam::Bal;
		}
		if (param == static_cast<size_t>(p.rate)) {
			return second == ero::SecondParam::Rate && !view->track;
		}
		if (param == static_cast<size_t>(p.rateMult)) {
			return second == ero::SecondParam::Rate && view->track;
		}
		return false;
	};
}
/// THE TYPE BOX'S FACE NAMES: what the cell prints between its arrows, one per `DriveMode`. DISPLAY ONLY: the Choice labels
/// (`kDriveModeNames`, what a patch stores) and the dropdown menu keep the full names, and the tooltip says the full name.
inline constexpr const char* kDriveModeFaceNames[] = { "Soft", "Tube", "Fold", "Wrk", "Jag", "Lean", "Chnk", "P-S", "P-H", "P-OS", "P-OH",
	                                                    "Crsh", "Dcm", "Slew", "Lops" };
static_assert(sizeof kDriveModeFaceNames / sizeof kDriveModeFaceNames[0] == static_cast<size_t>(fx::eroder::DriveMode::kCount));
/// THE TYPE BOX'S NAMES for a `ZoneSelect`: the full label, with the short face name beside it.
inline std::vector<ZoneSelect::ZoneName> driveTypeZoneNames() {
	std::vector<ZoneSelect::ZoneName> types;
	size_t i = 0;
	for (const char* name : fx::eroder::kDriveModeNames) {
		types.push_back({ name, {}, kDriveModeFaceNames[i++] });
	}
	return types;
}
/// THE MULTIPLE'S TEXT: three significant digits without trailing zeros ("0.5", "0.25", "0.333", "0.0625", "1"), "off" at 0.
inline std::string rateMultText(double v) {
	if (v <= 1.0e-4) {
		return "off";
	}
	char buf[32];
	std::snprintf(buf, sizeof buf, "%.3g", v);
	return buf;
}
/// THE ROWS UNDER A STAGE'S TYPE, in the order they pack: Shape ("S"), then whichever second parameter the Type has (Bal, Rate in hertz, or Rate
/// as a multiple of the note).
template <class ControlOf>
void addStageRows(ModeValueCell& cell, ControlOf&& control, const StageParams& p, bool narrow = false) {
	// A NARROW CELL (the card's, a fifth of a row) cannot print "Rate" beside "0.5Hz" without the two overlapping: its Rate row is labelled "Rt".
	const char* rateLabel = narrow ? "Rt" : "Rate";
	(void)cell.addFollower(static_cast<size_t>(p.shape), std::make_unique<ValueBox>(control(p.shape), ""), "S");
	(void)cell.addFollower(static_cast<size_t>(p.angle), std::make_unique<ValueBox>(control(p.angle), ""), "S");
	(void)cell.addFollower(static_cast<size_t>(p.bal), std::make_unique<ValueBox>(control(p.bal), ""), "Bal");
	(void)cell.addFollower(static_cast<size_t>(p.rate), std::make_unique<ValueBox>(control(p.rate), ""), rateLabel);
	auto mult = std::make_unique<ValueBox>(control(p.rateMult), "");
	// A NUMBER THE HAND DRAGS TO A MUSICAL PLACE AND TYPES ANYTHING INTO: detents for the drag, the number's own text, the plain number typed back.
	mult->setDetents(std::vector<double>(std::begin(fx::eroder::kRateMultDetents), std::end(fx::eroder::kRateMultDetents)));
	mult->setReadoutFormatter(&rateMultText, true);
	(void)cell.addFollower(static_cast<size_t>(p.rateMult), std::move(mult), rateLabel);
}

/// THE WINDOW'S ONE SLOT ROW: the Amount knob, the MIX knob, and the Type's cell, left to right in thirds of the row.
class SlotTriple final : public visage::Frame {
public:
	SlotTriple(visage::Frame& amount, visage::Frame& mix, visage::Frame& cell) : amount_(amount), mix_(mix), cell_(cell) {
		addChild(&amount);
		addChild(&mix);
		addChild(&cell);
	}
	void resized() override {
		const float w = static_cast<float>(width());
		const float h = static_cast<float>(height());
		const float third = w / 3.0f;
		amount_.setBounds(0.0f, 0.0f, third, h);
		mix_.setBounds(third, 0.0f, third, h);
		cell_.setBounds(2.0f * third, 0.0f, w - 2.0f * third, h);
	}
	[[nodiscard]] visage::Frame& amount() const noexcept { return amount_; }
	[[nodiscard]] visage::Frame& mix() const noexcept { return mix_; }
	[[nodiscard]] visage::Frame& cell() const noexcept { return cell_; }

private:
	visage::Frame& amount_;
	visage::Frame& mix_;
	visage::Frame& cell_;
};

/// THE WINDOW'S SLOT BLOCK: slot A's row over slot B's, and the route switch to the right of both, centred on them under a
/// small "ROUTE" caption. It draws nothing else: the Engine group's box is the outline.
class SlotStack final : public visage::Frame {
public:
	static constexpr float kRowGap = 6.0f;
	/// THE ROUTE COLUMN'S WIDTH, right of the rows: the switch and its caption are centred in it.
	static constexpr float kRouteColumn = 40.0f;
	static constexpr float kCaptionHeight = 11.0f;
	SlotStack(visage::Frame& a, visage::Frame& b, RouteSwitch& route) : a_(a), b_(b), route_(route) {
		addChild(&a);
		addChild(&b);
		addChild(&route);
	}
	/// THE BLOCK'S SIZE for rows of `rowWidth` by `rowHeight`.
	static float widthFor(float rowWidth) { return rowWidth + kRouteColumn; }
	static float heightFor(float rowHeight) { return 2.0f * rowHeight + kRowGap; }
	void resized() override;
	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] visage::Frame& routeFrame() const noexcept { return route_; }

private:
	visage::Frame& a_;
	visage::Frame& b_;
	RouteSwitch& route_;
};

struct EtchSlots {
	std::unique_ptr<Knob> knobA, knobB;
	/// THE WINDOW'S MIX KNOBS (null on the card, where Mix is not shown).
	std::unique_ptr<Knob> mixKnobA, mixKnobB;
	std::unique_ptr<ModeValueCell> cellA, cellB;
	/// THE CARD'S ROW (`build`): a pair of the Amount knob and the cell. THE WINDOW'S (`buildStacked`): a `SlotTriple` of three.
	std::unique_ptr<PairCell> innerA, innerB, outer;
	std::unique_ptr<SlotTriple> rowA, rowB;
	std::unique_ptr<RouteSwitch> route;
	/// THE WINDOW'S ARRANGEMENT (`buildStacked`); null on the card, where `outer` is made instead.
	std::unique_ptr<SlotStack> stack;
	/// WHAT EACH CELL'S ROW GATE READS (`stageRowGate`), refreshed from the row (`refreshRows`).
	std::shared_ptr<StageView> viewA = std::make_shared<StageView>();
	std::shared_ptr<StageView> viewB = std::make_shared<StageView>();

	/// @param control   the face's own accessor for a parameter's control
	/// @param caption   the outer pair's caption; empty for a bare row (no caption band, no outline)
	template <class ControlOf>
	void build(ControlOf&& control, float faceDiameter, Notifier* notifier, const std::string& caption) {
		makeParts(control, faceDiameter, notifier, false);
		outer = std::make_unique<PairCell>(caption, *innerA, *innerB);
		outer->setMiddle(*route, RouteSwitch::kWidth);
	}
	/// **THE WINDOW'S ARRANGEMENT: SLOT A OVER SLOT B, THE ROUTE SWITCH TO THE RIGHT OF BOTH**, centred on them, so the top-down
	/// order of the two slots is the order the switch's glyphs and the topology picture draw. Each row is Amount knob | MIX knob |
	/// the cell; the card's `outer` is not made.
	template <class ControlOf>
	void buildStacked(ControlOf&& control, float faceDiameter, Notifier* notifier) {
		makeParts(control, faceDiameter, notifier, true);
		stack = std::make_unique<SlotStack>(*rowA, *rowB, *route);
	}
	template <class ControlOf>
	void makeParts(ControlOf&& control, float faceDiameter, Notifier* notifier, bool windowRow) {
		namespace ero = fx::eroder;
		using P = ero::Param;
		const auto slot = [&](int index, std::unique_ptr<Knob>& knob, std::unique_ptr<Knob>& mixKnob, std::unique_ptr<ModeValueCell>& mode,
		                      std::unique_ptr<PairCell>& pair, std::unique_ptr<SlotTriple>& row, std::shared_ptr<StageView>& view, P amount,
		                      P mix, const char* name) {
			// THE KNOB IS CAPTIONED WITH ITS SLOT'S NAME, one letter: a word does not fit half a quarter-row.
			knob = std::make_unique<Knob>(control(amount), name, notifier);
			knob->setFaceDiameter(faceDiameter);
			const StageParams params = slotStageParams(index);
			std::vector<ZoneSelect::ZoneName> types = driveTypeZoneNames();
			mode = std::make_unique<ModeValueCell>(control(params.type), "Type", std::move(types), stageRowGate(view, params));
			mode->setFaceDiameter(faceDiameter);
			addStageRows(*mode, control, params, !windowRow);
			mode->setStacked(true);
			(void)mode->refresh();
			if (windowRow) {
				mixKnob = std::make_unique<Knob>(control(mix), "Mix", notifier);
				mixKnob->setFaceDiameter(faceDiameter);
				row = std::make_unique<SlotTriple>(*knob, *mixKnob, *mode);
			}
			else {
				pair = std::make_unique<PairCell>("", *knob, *mode);
			}
		};
		slot(0, knobA, mixKnobA, cellA, innerA, rowA, viewA, P::SlotAAmount, P::SlotAMix, "A");
		slot(1, knobB, mixKnobB, cellB, innerB, rowB, viewB, P::SlotBAmount, P::SlotBMix, "B");
		route = std::make_unique<RouteSwitch>(control(P::SlotRoute), notifier);
	}
	void setModulation(ModState* mod, EditHistory* history) {
		knobA->setModulation(mod, history);
		knobB->setModulation(mod, history);
		if (mixKnobA != nullptr) {
			mixKnobA->setModulation(mod, history);
			mixKnobB->setModulation(mod, history);
		}
	}
	/// THE SECOND-PARAMETER ROWS FOLLOW EACH SLOT'S TYPE AND TRACK (the row read modulated, as the honesty gates read it); true if a row moved.
	bool refreshRows(const fx::eroder::Row& row) {
		*viewA = stageViewOf(row, slotStageParams(0));
		*viewB = stageViewOf(row, slotStageParams(1));
		const bool a = cellA->refresh();
		const bool b = cellB->refresh();
		return a || b;
	}

	/// **SHOWN OR HIDDEN AS A WHOLE, EVERY FRAME ON ITS OWN FLAG**: a frame's own visibility is what a walk of the tree reads
	/// (a hidden parent does not clear its children's), so a row that is merely a hidden parent would still offer its knobs
	/// to a hand-turning sweep at their stale places.
	void setShown(bool shown) {
		for (visage::Frame* f : std::initializer_list<visage::Frame*> { outer.get(), stack.get(), innerA.get(), innerB.get(), rowA.get(), rowB.get(),
		                                                                 knobA.get(), knobB.get(), mixKnobA.get(), mixKnobB.get(), cellA.get(),
		                                                                 cellB.get(), route.get() }) {
			if (f != nullptr) {
				f->setVisible(shown);
			}
		}
	}

	/// The row's height at a knob column of `knobHeight`: the bare row is the knob's, a captioned one has the band above.
	[[nodiscard]] float heightFor(float knobHeight) const { return outer == nullptr ? knobHeight : outer->bare() ? knobHeight : PairCell::heightFor(knobHeight); }
};

} // namespace feathers::ui
