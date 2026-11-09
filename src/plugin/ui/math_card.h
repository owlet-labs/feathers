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

#pragma once

#include "backplane/device_host.h"
#include "mod/math/math_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <memory>
#include <vector>

// THE MATH CARD — `A op B` READ AS IT IS WRITTEN, IN TWO ROWS.
//
// The MATH card is deliberately small. The generic grid gave each of the six controls a
// list row of its own, so the device that combines two inputs was a column of six things with the
// combination nowhere on its face. This card writes the expression instead:
//
//     [ A ]  [ × ]  [ B ]                 — the operator BETWEEN its operands, so the order is the layout
//     [Amount] [Bipolar] [Transfer]       — what happens to the result, a third of the width each
//
// **EVERY CONTROL IS THE GENERIC GRID'S WIDGET FOR IT**, chosen by the same facts: A, B and Amount are
// knobs (continuous and modulatable, so they take routes and draw rings), Operator (and, until PY, Transfer) are
// dropdowns (`ValueBox` opens a menu for a named Choice of three or more — every entry is visible before
// one is applied, the multivalue-toggle rule), and Bipolar is a `SwitchChip` (two states: next and
// which are the same thing). Only the ARRANGEMENT is this card's — EXCEPT TRANSFER, which since backlog PY is
// `MathTransferGlyph` (below): still a menu on a click, so every entry is seen before one is applied, and
// the curve itself drawn where the dropdown's value was, its coefficient a drag on it.
//
// **AMOUNT IS A SMALLER KNOB COLUMN, NOT A `FaderPill` AND NOT A LIST ROW.** The pill is an amount WITH
// a power switch, and MATH declares no power for Amount — seating one would mean inventing a control the
// device does not have. The list row (`LabelPlacement::Right`) would have kept the second row at the
// dropdown's 28px, and was TRIED AND PHOTOGRAPHED: a third of the rail's body is 62px, the face takes
// 20-24 of it, and "AMOUNT" drew as "AMOUN" in what was left — a caption that cannot say its name. So
// it is a column like A and B at a smaller face (it scales the result rather than being an operand),
// the chip and the dropdown sit level with that face, and it is still a knob: it takes a route and
// draws its ring like A and B.
//
// **IT CAN DIE UNDER ITS OWN POPUP.** A dropdown's menu is parented to the root frame and keeps its
// selection callback until a pick, and the strip rebuilds a card on a move or a removal while the menu
// is open. The picks go through `alive_` first (`MacrosCard`'s shape), so a late pick writes nothing.

namespace feathers::ui {

class Notifier;

// **THE TRANSFER GLYPH** (backlog PY: approved it 2026-11-07) — MATH's Transfer drawn as the curve it is,
// in Amount's column shape: the plot where a face would be, "TRANSFER" under it, and the map with its coefficient
// as the reading ("Exp 2.83", "Phi 16"). It is the route chip's control on a card (`transfer_plot.h` is the one
// copy of both): a click opens the Transfer menu, a VERTICAL DRAG sets the coefficient — Phi's periods, Log's and
// Exp's exponent, at the chip's pixels per step — as ONE undo step with the reading in the modulation colour while
// it moves, and a curve with no coefficient (Lin) opens the menu on the press and ignores the drag.
//
// **THE PLOT IS `mod::math::mapCurve`, THE FUNCTION `evaluate` RUNS**, not `device::shapeTransfer`: MATH's
// coefficients are continuous controls (a route's are steps), so the device's own curve is the only one that cannot
// disagree with what it outputs. A drag lands on the route chip's steps; a typed or automated value between them
// is drawn as it is.
//
// **IT REPLACES THE PERIODS AND EXPONENT BOXES** that stood beside the dropdown under Phi/Log/Exp. MATH has no
// window, so there is no superset to keep them in; the glyph is now their face (it prints the value and edits it),
// the row is thirds in every state, and nothing on the card changes shape under the hand when the map changes.
class MathTransferGlyph final : public visage::Frame, public TooltipSource {
public:
	MathTransferGlyph(DeviceControl& map, DeviceControl& periods, DeviceControl& exponent,
	                  const DeviceControl& bipolar, Notifier* notifier);

	/// What a click asks for; the card shows its guarded menu against this glyph.
	std::function<void()> onMenu;

	/// THE PLOT'S BOX, in this glyph's coordinates: the face's height at the top, inset.
	[[nodiscard]] visage::Bounds plotBounds() const;
	/// **THE POINTS `draw` STROKES** — `mapCurve` at the controls' current values, sampled over −1..1 when Bipolar is
	/// on and 0..1 when off. The case's seam: what it holds is what is stroked.
	[[nodiscard]] std::vector<visage::Point> plotPoints() const;
	/// THE READING under the caption — "Lin", "Exp 2.83", "Phi 16".
	[[nodiscard]] std::string readout() const;
	/// Whether a press has become a coefficient drag (the reading is then in the modulation colour).
	[[nodiscard]] bool dragging() const noexcept { return drag_.has_value() && drag_->moved; }

	/// THE CURVE AS A ROUTE CODE — MATH's map at its coefficient, nearest step; the drag's arithmetic runs on it.
	[[nodiscard]] device::Transfer transferCode() const;
	/// The Map control this glyph shows and its menu writes.
	[[nodiscard]] DeviceControl& mapControl() const noexcept { return map_; }
	/// THE CONTROL HOLDING THE CURRENT MAP'S COEFFICIENT (the one a drag writes), or null for Lin.
	[[nodiscard]] DeviceControl* coefficientControl() const;

	const char* tooltipAt(visage::Point) const override { return tooltipOf(&map_); }
	void offeredControls(std::vector<Offered>& out) const override {
		// THE MAP AND BOTH COEFFICIENTS ARE FACES HERE: shown (the curve, the reading) and edited (the menu, the drag).
		out.push_back({ &map_, Road::Face });
		out.push_back({ &periods_, Road::Face });
		out.push_back({ &exponent_, Road::Face });
	}

	void draw(visage::Canvas& canvas) override;

private:
	[[nodiscard]] mod::math::Map map() const;

	DeviceControl& map_;
	DeviceControl& periods_;
	DeviceControl& exponent_;
	const DeviceControl& bipolar_;
	Notifier* notifier_ = nullptr;
	struct Drag {
		float anchorY = 0.0f;
		device::Transfer start = device::Transfer::Lin;
		DeviceControl* control = nullptr;
		bool moved = false;
	};
	std::optional<Drag> drag_;
};

class MathCard final : public FxCard {
public:
	/// `mod` and `history` may be null — a host with no modulation state gets knobs that take no route;
	/// `setModulation` hands them over later either way.
	MathCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr, ModState* mod = nullptr,
	         EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	MathCard(backplane::DeviceHost&, size_t, Notifier* = nullptr, ModState* = nullptr, EditHistory* = nullptr) = delete;
	/// **THE TOKEN FIRST**, so a menu still open on the root frame finds no card behind its pick.
	~MathCard() override;

	void resized() override;
	/// THE SLOT MENU'S DOTS, WHEN A HOST BINDS ONE (a lane: Feathers' fx lane holds modulators since 2026-10-04) —
	/// `drawSlotMenu` draws nothing where none is bound, so the strip, which draws its own head mark, is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	/// THE HEAD LIGHTS WHILE THIS DEVICE'S OUTPUT IS THE FOCUSED SOURCE — every modulator card's rule.
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::math::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	/// A AND B: the Engine rail's face, the generic grid's — the size a modulator knob already is.
	static constexpr float kFaceDiameter = Knob::kEngineRailDiameter;
	/// **THE OPERATOR'S WIDTH, SET BY ITS LONGEST ENTRY** — "max" and the caret, with the box's padding.
	/// Narrow on purpose: the wider the box, the further apart A and B stand, and the expression is
	/// meant to read as one line.
	static constexpr float kOperatorWidth = 40.0f;
	/// AMOUNT'S FACE — smaller than A's and B's, because it scales the result rather than being an operand.
	static constexpr float kAmountDiameter = 24.0f;

	/// THE FIRST ROW: a knob column at `kFaceDiameter`.
	static constexpr float firstRowHeight() { return Knob::columnHeight(kFaceDiameter); }
	/// THE SECOND ROW: Amount's knob column, which the chip and the dropdown fit inside.
	static constexpr float secondRowHeight() { return Knob::columnHeight(kAmountDiameter); }
	static_assert(ValueBox::kHeight <= Knob::columnHeight(kAmountDiameter), "the dropdown must fit Amount's row");
	/// THE BODY: a gap, the expression, a gap, what happens to it, a gap.
	static constexpr float bodyHeight() { return kGap + firstRowHeight() + kGap + secondRowHeight() + kGap; }
	/// OPEN, IN A VERTICAL LANE — head, body and `outputRows` rows of output chips. Static so a host can
	/// reserve the card before it has built one (`ModPanel::bodyHeightOf`).
	static float openHeight(size_t outputRows);

	[[nodiscard]] Knob& a() const { return *a_; }
	[[nodiscard]] Knob& b() const { return *b_; }
	[[nodiscard]] Knob& amount() const { return *amount_; }
	[[nodiscard]] ValueBox& op() const { return *op_; }
	/// **THE TRANSFER GLYPH** (backlog PY) — the curve drawn, a click its menu, a vertical drag its coefficient.
	[[nodiscard]] MathTransferGlyph& transfer() const { return *transfer_; }
	[[nodiscard]] SwitchChip& bipolar() const { return *bipolar_; }

	/// The parameter road under a control of this device, by its index in the descriptor.
	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// **THE MENU A PRESS ON `box` OPENS** — the box's own list, its pick guarded by `alive_`. Public so a
	/// case can hold one across the card's destruction, which is the thing the guard is for.
	[[nodiscard]] visage::PopupMenu guardedMenuFor(ValueBox& box);
	/// Every control this card puts a hand on — the six on its face; the enable is the head's.
	[[nodiscard]] std::vector<mod::math::Param> addressedParamsForTest() const;
	/// **THE MENU A CLICK ON THE GLYPH OPENS** — every map, the current one marked, its pick ONE gesture on Map and
	/// guarded by `alive_`. Built and not shown, so a case reads it and picks from it on the road a hand takes.
	[[nodiscard]] visage::PopupMenu transferMenu();

private:
	device::Modulatable* enableModulatable() override;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// **THE CARD, FOR ANYTHING THAT MAY OUTLIVE IT** — the dropdowns' menus capture it weakly.
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	/// THE PARAMETER ROAD, ONE PER DECLARED CONTROL, built on first ask. Declared before the widgets so
	/// it outlives them.
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(mod::math::Param::kCount)> controls_;
	std::unique_ptr<Knob> a_;
	std::unique_ptr<ValueBox> op_;
	std::unique_ptr<Knob> b_;
	std::unique_ptr<Knob> amount_;
	std::unique_ptr<SwitchChip> bipolar_;
	std::unique_ptr<MathTransferGlyph> transfer_;
	/// Grey Bipolar while the operator is a comparison (a 0/1 gate is the same either way). True when anything changed.
	bool applyGates();
	/// WHAT EACH CONTROL LAST DREW, so `updateContent` repaints only a widget an undo, a load or the host
	/// moved under it. Indexed by `Param`; NaN until the first poll, so the first poll always paints.
	std::array<double, static_cast<size_t>(mod::math::Param::kCount)> drawn_ {};
};

} // namespace feathers::ui
