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
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

// ETCH'S SLOT ROW — one line, four cells wide, the card's and the window's alike:
//
//   ┌ A ───────────────┬────┬ B ───────────────┐
//   │ (knob) [ Type  ] │ ▪  │ (knob) [ Type  ] │   each slot: its amount, and under the Type its Shape and Mix
//   │  A     S  34%    │ │  │  B     S  34%    │   the route between: Series above, Parallel below
//   │        M 100%    │ ▪▪ │        M 100%    │
//   └──────────────────┴────┴──────────────────┘
//
// **ONE OBJECT BOTH FACES BUILD**, so the two cannot drift: the card and the window each own one, seat `outer` in their
// own layout, and hand it the control accessor they already have. Every widget is bound to its control from construction
// (so a route to a Shape or a Mix is made on its box, and the drag is that control's own).

namespace feathers::ui {

/// THE WINDOW'S SLOT BLOCK: slot A's row over slot B's, and the route switch to the right of both, centred on them under a
/// small "ROUTE" caption. It draws nothing else: the Engine group's box is the outline.
class SlotStack final : public visage::Frame {
public:
	static constexpr float kRowGap = 6.0f;
	/// THE ROUTE COLUMN'S WIDTH, right of the rows: the switch and its caption are centred in it.
	static constexpr float kRouteColumn = 40.0f;
	static constexpr float kCaptionHeight = 11.0f;
	SlotStack(PairCell& a, PairCell& b, RouteSwitch& route) : a_(a), b_(b), route_(route) {
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
	PairCell& a_;
	PairCell& b_;
	RouteSwitch& route_;
};

struct EtchSlots {
	std::unique_ptr<Knob> knobA, knobB;
	std::unique_ptr<ModeValueCell> cellA, cellB;
	std::unique_ptr<PairCell> innerA, innerB, outer;
	std::unique_ptr<RouteSwitch> route;
	/// THE WINDOW'S ARRANGEMENT (`buildStacked`); null on the card, where `outer` is made instead.
	std::unique_ptr<SlotStack> stack;

	/// @param control   the face's own accessor for a parameter's control
	/// @param caption   the outer pair's caption; empty for a bare row (no caption band, no outline)
	template <class ControlOf>
	void build(ControlOf&& control, float faceDiameter, Notifier* notifier, const std::string& caption) {
		makeParts(control, faceDiameter, notifier);
		outer = std::make_unique<PairCell>(caption, *innerA, *innerB);
		outer->setMiddle(*route, RouteSwitch::kWidth);
	}
	/// **THE WINDOW'S ARRANGEMENT: SLOT A OVER SLOT B, THE ROUTE SWITCH TO THE RIGHT OF BOTH**, centred on them, so the top-down
	/// order of the two slots is the order the switch's glyphs and the topology picture draw. The same parts as `build`; the
	/// card's `outer` is not made.
	template <class ControlOf>
	void buildStacked(ControlOf&& control, float faceDiameter, Notifier* notifier) {
		makeParts(control, faceDiameter, notifier);
		stack = std::make_unique<SlotStack>(*innerA, *innerB, *route);
	}
	template <class ControlOf>
	void makeParts(ControlOf&& control, float faceDiameter, Notifier* notifier) {
		namespace ero = fx::eroder;
		using P = ero::Param;
		const auto slot = [&](std::unique_ptr<Knob>& knob, std::unique_ptr<ModeValueCell>& mode, std::unique_ptr<PairCell>& pair,
		                      P amount, P type, P shape, P mix, const char* name) {
			// THE KNOB IS CAPTIONED WITH ITS SLOT'S NAME, one letter: a word does not fit half a quarter-row.
			knob = std::make_unique<Knob>(control(amount), name, notifier);
			knob->setFaceDiameter(faceDiameter);
			std::vector<ZoneSelect::ZoneName> types;
			for (const char* typeName : ero::kDriveModeNames) {
				types.push_back({ typeName, {} });
			}
			mode = std::make_unique<ModeValueCell>(control(type), "Type", std::move(types), [](size_t) { return true; });
			mode->setFaceDiameter(faceDiameter);
			(void)mode->addFollower(static_cast<size_t>(shape), std::make_unique<ValueBox>(control(shape), ""), "S");
			(void)mode->addFollower(static_cast<size_t>(mix), std::make_unique<ValueBox>(control(mix), ""), "M");
			mode->setStacked(true);
			(void)mode->refresh();
			pair = std::make_unique<PairCell>("", *knob, *mode);
		};
		slot(knobA, cellA, innerA, P::SlotAAmount, P::SlotAType, P::SlotAShape, P::SlotAMix, "A");
		slot(knobB, cellB, innerB, P::SlotBAmount, P::SlotBType, P::SlotBShape, P::SlotBMix, "B");
		route = std::make_unique<RouteSwitch>(control(P::SlotRoute), notifier);
	}
	void setModulation(ModState* mod, EditHistory* history) {
		knobA->setModulation(mod, history);
		knobB->setModulation(mod, history);
	}

	/// **SHOWN OR HIDDEN AS A WHOLE, EVERY FRAME ON ITS OWN FLAG**: a frame's own visibility is what a walk of the tree reads
	/// (a hidden parent does not clear its children's), so a row that is merely a hidden parent would still offer its knobs
	/// to a hand-turning sweep at their stale places.
	void setShown(bool shown) {
		for (visage::Frame* f : std::initializer_list<visage::Frame*> { outer.get(), stack.get(), innerA.get(), innerB.get(),
		                                                                 knobA.get(), knobB.get(), cellA.get(), cellB.get(),
		                                                                 route.get() }) {
			if (f != nullptr) {
				f->setVisible(shown);
			}
		}
	}

	/// The row's height at a knob column of `knobHeight`: the bare row is the knob's, a captioned one has the band above.
	[[nodiscard]] float heightFor(float knobHeight) const { return outer == nullptr ? knobHeight : outer->bare() ? knobHeight : PairCell::heightFor(knobHeight); }
};

} // namespace feathers::ui
