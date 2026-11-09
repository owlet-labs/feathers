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

#include "backplane/device_host.h"
#include "backplane/endpoint.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/theme.h" // `FittedName`: the heading's fitted source name
#include "plugin/ui/tooltip.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <visage/ui.h>

namespace feathers::ui {

/// THE FOCUSED SOURCE'S ROUTINGS, ENUMERATED — the mock's `#inspRoutes`, which lives in the Engine rail
/// and appears only while a modulator is in focus.
///
/// It is not redundant with the rings. A ring says "something reaches this knob" to a hand already
/// looking at that knob; the list answers the other question, the one a patch raises after an hour of
/// work — WHAT DOES THIS MACRO DO? — without a sweep of the editor hunting for periwinkle. The mock's
/// own affordances: the amount is dragged in place, a double-click zeroes it, the × takes the routing
/// away — and a triple-click TYPES it (backlog LS: a double-click resets, a triple-click types), as
/// on every knob and value box: the field opens on the amount from before the double-click's
/// zero, and Escape gives it back.
///
/// One departure, and it is the mock's own shape rather than a preference: its section head carries a
/// power switch for its LFO, and a macro has none — a macro at rest already reaches nowhere. So the
/// head names the focused macro and claims nothing it cannot do.
///
/// **ONE WIDGET OVER THE HOST CONTRACT (rack roadmap §7f).** It was written over `ParamBridge`, so the
/// rack plugin — whose editor addresses a `RackBridge` — had no list, and the routes inside a chain in
/// the rack could be seen only from inside the chain's own panel. It asks `backplane::DeviceHost` for
/// everything it needs (the routes from a source, their names, the routes inside the host's chains),
/// and `ModState` for the source's name; Feathers' Engine rail hands it the bridge behind its adapter,
/// the rack's editor hands it the rack's bridge. Implemented beside the panel that first held it,
/// `mod_panel.cpp`.
class RouteList : public visage::Frame, public TooltipSource {
public:
	/// **THE ROWS SAY NOTHING ON HOVER** — they print their source and destination in full already.
	[[nodiscard]] const char* tooltipAt(visage::Point) const override { return nullptr; }
	/// **THE HEADING'S SOURCE NAME, WHEN IT WAS CUT** (a shortened label shows its full name on hover) —
	/// over the name's room in the heading row, the name whole (`ui::LabelFace`); a name drawn whole says nothing.
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point local) const override;
	/// Not a device control: the rows are routes, edited through the host's route verbs.
	void offeredControls(std::vector<Offered>&) const override {}
	/// **THE HEADING'S NAME AS DRAWN** — the focused source's name fitted to the room after "ROUTES" (`fitName`), and
	/// that room. `draw` paints from this and `labelFaceAt` answers from it, so the two cannot disagree.
	struct HeadingName {
		std::string whole;
		FittedName fitted;
		visage::Bounds room;
	};
	[[nodiscard]] HeadingName headingName() const;
	RouteList(backplane::DeviceHost& host, ModState& mod, EditHistory& history, Notifier* notifier = nullptr);

	/// **IT UNREGISTERS ITSELF, BECAUSE IT REGISTERED ITSELF.** See `Knob::~Knob` for the rule and what
	/// it cost to learn: a thing that opts in by itself and opts out only if a distant owner remembers
	/// is a dangling pointer with extra steps, and that surfaced exactly as a crash on clicking an arm
	/// arrow.
	~RouteList() override;

	void draw(visage::Canvas& canvas) override;

	/// How tall this wants to be for what it has to show, so the Engine rail can place it without knowing
	/// what a row is.
	float preferredHeight() const;

	/// **IT SCROLLS BY WHOLE ROWS WHEN IT IS SHORTER THAN ITS ROUTES** (backlog OC: the Chain's Engine rail keeps the window's
	/// height and gives the list what is left). The heading stays; the rows under it start at `firstRow()`. A list as tall as
	/// `preferredHeight()` — Feathers' Engine rail — shows every row and never scrolls. The wheel scrolls it, and answers only when
	/// it moved, so a wheel it cannot use bubbles on.
	[[nodiscard]] size_t visibleRows() const;
	[[nodiscard]] size_t firstRow() const;
	bool scrollRowsBy(int rows);

	/// The destinations the focused source reaches, in the destination list's own order.
	/// THIS HOST'S OWN ROUTES from the armed source — the rows that were always here.
	std::vector<backplane::Destination> rows() const;
	/// **ONE ROW, OWN OR INSIDE A CHAIN** (rack roadmap §7): `chain == kNoInstance` is this host's route
	/// to `destination`; otherwise `destination` is in the CHAIN's id space and the row is that chain's
	/// inner route from the armed source, shown and edited from here through the chain's gate.
	struct Row {
		DeviceInstanceId chain = kNoInstance;
		backplane::Destination destination {};
		friend bool operator==(const Row&, const Row&) = default;
	};
	/// Every row drawn: the host's own first, then each chain's inner routes.
	std::vector<Row> allRows() const;
	/// What a row is drawn as — for a venue that reads the list rather than the pixels.
	std::string nameOfForTest(const Row& row) const { return nameOf(row); }

	static constexpr float kRowHeight = 18.0f;
	static constexpr float kHeadingHeight = 14.0f;
	static constexpr float kRemoveWidth = 14.0f;
	/// The mock's `/80` for an amount dragged in the list — slower than the same amount on the knob,
	/// because here the hand is aiming at an 18px row rather than at a dial.
	static constexpr float kDragRange = 80.0f;

	/// **THE TRANSFER CHIP**. Right to left a row is
	/// `× | gap | amount | gap | chip | name`: the chip takes its room from the NAME's side, never from the
	/// amount's text, and a press anywhere but the chip and the × is still the amount's drag.
	static constexpr float kChipWidth = 18.0f;
	static constexpr float kChipHeight = 12.0f;
	/// Room for the widest amounts a route prints ("+100%", "-100%", "+200%") in the readout face —
	/// asserted against the font's own measurement in `test_gui_widgets_lane`'s chip case.
	static constexpr float kAmountWidth = 30.0f;
	static constexpr float kRowGap = 4.0f;

	enum class Part { None, Amount, Remove, Transfer };

	struct Hit {
		Part part = Part::None;
		backplane::Destination destination {};
		DeviceInstanceId chain = kNoInstance; ///< the chain whose inner route this is, or none
	};

	Hit hitAt(visage::Point pixels) const;
	visage::Bounds rowBounds(size_t index) const;
	/// Where row `index`'s amount text is drawn, and where its chip is — disjoint by construction, and a
	/// case holds them so.
	visage::Bounds amountBounds(size_t index) const;
	visage::Bounds chipBounds(size_t index) const;

	/// THE ROW'S CURVE, own or inside a chain.
	backplane::Transfer transferOf(const Row& row) const;
	/// **THE MENU THE CHIP OPENS, BUILT AND NOT SHOWN** — every curve, named, the current one marked, the
	/// ones that would change nothing for this source greyed (`DeviceHost::transferChoosable`). Its
	/// `onSelection` is the whole pick, so a case calling it is on the road a hand takes. Nothing is
	/// applied until it is called.
	[[nodiscard]] visage::PopupMenu transferMenu(const Row& row);

	/// **A CHIP'S COEFFICIENT DRAG** (backlog PY) — its threshold, its pixels per step, where it lands and how its
	/// readout prints are `transfer_plot.h`'s, shared with the MATH card's Transfer glyph.
	/// WHAT ROW `index` SHOWS IN ITS NAME'S PLACE WHILE ITS CHIP IS DRAGGED ("Exp exponent 2.83"), or nothing at
	/// rest. Public so a case reads the readout as text.
	[[nodiscard]] std::optional<std::string> chipDragReadout(size_t index) const;
	/// **THE POINTS THE CHIP STROKES** for curve `t` in `chip` — `device::shapeTransfer`, the engine's function,
	/// sampled over the source's range. `drawChip` strokes exactly these; `chipPointsForTest` is the case's seam
	/// onto row `index`'s, as drawn now.
	[[nodiscard]] static std::vector<visage::Point> chipPoints(const visage::Bounds& chip, backplane::Transfer t,
	                                                           bool bipolar);
	[[nodiscard]] std::vector<visage::Point> chipPointsForTest(size_t index) const;

	/// **THE TYPED AMOUNT, READ AS A PERCENT** — the unit the row prints — so "50", "+50", "50%" and
	/// " -12.5 % " all parse, and a fraction is never guessed at: "0.5" is half a percent. Clamped to
	/// the amount's range; empty, non-numeric or trailing junk is no value at all, and the edit is
	/// dropped rather than read as zero. Returns the AMOUNT (a fraction), not the percent.
	[[nodiscard]] static std::optional<float> parseAmount(std::string_view text);
	/// What the field opens on: the amount as a percent, the sign only when negative, and a decimal
	/// only when the amount has one ("50", "-12.5").
	[[nodiscard]] static std::string printAmountForEdit(float amount);

	/// THE FIELD a triple-click opens over the row's amount (backlog LS) — one for the list's lifetime, only
	/// ever hidden (`text_field.h`'s rule). Hidden at rest.
	[[nodiscard]] TextField& amountField() const noexcept { return *field_; }
	/// The row the open field is editing, or none.
	[[nodiscard]] std::optional<Row> typingRowForTest() const {
		return field_->editing() && typing_ ? std::optional<Row>(typing_->row) : std::nullopt;
	}

	void visibilityChanged() override;
	void resized() override;

private:
	/// **THE SUBSCRIPTION, HELD** — see `ui::Watch`. Declared last so it is released FIRST.
	std::vector<Watch> watches_;
	/// The first row drawn (see `scrollRowsBy`); clamped against the rows there are whenever it is read.
	size_t firstRow_ = 0;
	void commitAmount(const Row& row, float amount);
	void remove(const Row& row);
	/// The row's amount, name and write, own or inside a chain — the one place the two roads meet.
	float amountOf(const Row& row) const;
	std::string nameOf(const Row& row) const;
	void write(const Row& row, float amount);
	void recordEdit(const Row& row, float before, float after);
	/// A pick from the chip's menu: written, recorded as one step, said.
	void pickTransfer(const Row& row, backplane::Transfer t);
	/// The curve written, own or inside a chain — no undo step, no notice: a drag's moves.
	void writeTransfer(const Row& row, backplane::Transfer t);
	/// **A PRESS ON A CHIP WITH A COEFFICIENT** — armed on the press, a drag once it moves past the threshold, the menu if it
	/// never does. `before` is the curve at the press; `now` the one written last.
	struct ChipDrag {
		Row row {};
		size_t index = 0;
		float anchorY = 0.0f;
		backplane::Transfer before = backplane::Transfer::Lin;
		backplane::Transfer now = backplane::Transfer::Lin;
		bool moved = false;
	};
	std::optional<ChipDrag> chipDrag_;
	/// The chip: a small plot of the curve over the source's range, from `device::shapeTransfer` itself.
	void drawChip(visage::Canvas& canvas, const visage::Bounds& chip, backplane::Transfer t, bool bipolar) const;
	/// **WHAT A DOUBLE-CLICK ZEROED, FOR THE THIRD CLICK** — the row, where it was, the amount it had, and
	/// the undo step the zero recorded (0 when there was nothing to zero). And, once the field is open on
	/// it, the text it opened on.
	struct Typing {
		Row row {};
		size_t index = 0;
		float before = 0.0f;
		uint64_t serial = 0;
		std::string opened;
	};
	/// Open the field over the zeroed row's amount and its chip (the amount's own 30 px is too narrow to
	/// type in), on the amount from before the zero, all of it selected.
	void beginTyping(const Typing& zeroed);
	/// Enter: the typed amount written as ONE gesture, folded into the zero's undo step, through the road
	/// the drag's release takes, including its rule that an amount below the unrouted threshold is gone.
	void commitTyped(const std::string& text);
	/// Escape, a press elsewhere, junk, or the opening text unchanged: the zero taken back, no step left.
	void cancelTyped();
	/// Hiding the field is what cancels it (`TextField::visibilityChanged`).
	void closeField();
	/// Over row `index`'s amount and chip — where the field opens, and where it waits at rest.
	void placeField(size_t index);

	backplane::DeviceHost& host_;
	ModState& mod_;
	EditHistory& history_;
	Notifier* notifier_ = nullptr;

	bool dragging_ = false;
	Row dragRow_ {};
	float anchorY_ = 0.0f;
	float anchorAmount_ = 0.0f;
	float before_ = 0.0f;

	std::unique_ptr<TextField> field_;
	/// The double-click's zero, armed until the next press; and the edit the triple-click opened on it.
	std::optional<Typing> zeroed_;
	std::optional<Typing> typing_;
};

} // namespace feathers::ui
