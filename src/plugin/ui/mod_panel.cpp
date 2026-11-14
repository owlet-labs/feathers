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

#include "device/format_value.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/bridge_device_host.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/device_registry.h"
#include "plugin/ui/device_menu.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/route_edit.h"
#include "plugin/ui/stored_edit.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/mod_readout.h"
#include "plugin/ui/emitters_front.h"

#include "plugin/ui/card_factory.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/lfo_card.h"
#include "plugin/ui/macros_card.h"
#include "mod/macros/macros_types.h"
#include "plugin/ui/math_card.h"
#include "plugin/ui/stereo_tool_card.h"
#include "plugin/ui/rtrig_card.h"
#include "plugin/ui/sample_hold_card.h"
#include "plugin/ui/stepseq_card.h"
#include "plugin/ui/follower_card.h"
#include "mod/math/math_types.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/device_edits.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/transfer_plot.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

namespace feathers::ui {

namespace {

/// WHAT A MACRO IS CALLED IN THE HEADER ROW, which is a different question from what it is called in
/// prose — the route list's heading, the tray, anywhere the name has to carry the context alone, which
/// `ParamBridge::macroName` answers (the user's name, or "Macro N") and modSourceName asks.
/// A column is 54px and "Macro 1" is most of that in 8px type; the row's own heading already says
/// what these are, so each knob only has to say WHICH. The long names stay wherever the context does
/// not.
constexpr std::array<const char*, kMacroCount> kMacroShortNames { "M1", "M2", "M3", "M4" };

constexpr float kDotDiameter = 8.0f;
constexpr float kCardPadX = 9.0f;
constexpr float kCornerRadius = 5.0f;

} // namespace

namespace {

/// WHERE A DEVICE STANDS IN ITS OWN CHAIN — the number a player reads beside its name — or
/// `kNoPosition` for an id neither chain holds.
size_t positionOfDevice(const ParamBridge& bridge, DeviceInstanceId device) {
	if (const size_t rack = bridge.rackSlotOf(device); rack != kNoPosition) {
		return rack;
	}
	if (const size_t fx = bridge.fxSlotOf(device); fx != kNoPosition) {
		return fx;
	}
	return kNoPosition;
}

} // namespace

std::string modSourceName(const ParamBridge& bridge, backplane::Source source) {
	if (isMacroSource(source)) {
		return bridge.macroName(source.index); // The user's name, or "Macro N"
	}
	if (source.isDevice()) {
		// THE DEVICE'S OWN NAME, qualified by slot once there is more than one to tell apart — the
		// same division device.h states: the device names itself, the rack says which one.
		//
		// ASKED OF THE REGISTRY. This said `mod::dahdsr::kTraits.name` UNCONDITIONALLY, which was
		// true while a rack slot could hold only an envelope and became a lie the moment a slot could
		// hold either — an MSEG presented as "DAHDSR n" in the route list, the tray, the Engine rail
		// and this panel's own readout.
		//
		// IT IS THE SECOND HALF OF A BUG ALREADY FIXED ONCE IN THIS FILE. `ModPanel::deviceName`
		// twelve functions down asks the registry, and mod_panel.h records why: the card head said
		// DAHDSR over an MSEG's editor and **it was found within minutes of the (+) existing, and
		// read as the wrong device having been added.** That fix landed on the head and not here,
		// so the same misreading was still one surface away — which is what `deviceName`'s own
		// comment forbids: *"one authority, so two surfaces cannot disagree about what a device is
		// called."*
		//
		// AND ANY DEVICE, NOT ONLY THE RACK'S, so an fx device that publishes an output is named rather
		// than called "Source". None declares one today, so nothing can regress on it; the branch simply
		// stops being wrong in advance.
		const device::Descriptor* desc = bridge.descriptorOf(source.device);
		if (desc == nullptr) {
			return "Source"; // an id nothing holds, or a device this build lacks, names nothing
		}
		std::string name = desc->traits.name;
		// NUMBERED BY WHERE THE DEVICE SITS — the label a player reads to know which card a route came
		// from. (It was numbered by position rather than by lane for the same reason, back when a lane
		// was the other number to hand.)
		//
		// **AND BY WHICH CHAIN, ONCE BOTH CAN HOLD ONE** (2026-10-04: the fx lane takes modulators). Numbered by
		// position in its own chain, the strip's first LFO and the lane's first LFO were both "LFO 1" — two sources
		// under one name in the route list. A lane device says where it stands: "LFO (FX 2)".
		const size_t position = positionOfDevice(bridge, source.device);
		if (position != kNoPosition && bridge.rackSlotOf(source.device) == kNoPosition) {
			name += " (FX ";
			name += std::to_string(position + 1);
			name += ")";
		}
		else if (position != kNoPosition) {
			name += " ";
			name += std::to_string(position + 1);
		}
		// AND WHICH OF ITS OUTPUTS, for a device with more than one — "Macros 2 · Wobble", the separator the
		// rack's namer and the chain column's already use. The output as this instance CALLS it
		// (`outputLabel`: the player's label, else the declared name), so a relabelled macro reads the same
		// in the route list, the tray and the Engine rail. A one-output device is unchanged: its name already
		// says which output there is.
		if (desc->outputCount() > 1 && source.index < desc->outputCount()) {
			name += " \xc2\xb7 "; // " · "
			name += bridge.outputLabel(source.device, source.index);
		}
		return name;
	}
	if (isStackSource(source.index)) {
		return "Stack Index"; // the bridge's own name for it (`coreSourceDescs`)
	}
	if (!isWeightSource(source.index)) {
		return "Source";
	}
	// "Sound A weight" — the star's own letter, then what about that sound this source IS. The last
	// word is load-bearing: a player meeting these for the first time has to be told that what
	// modulates is how much of the sound is being HEARD, not the sound itself.
	std::string name = "Sound ";
	name += static_cast<char>('A' + slotOfWeightSource(source.index));
	name += " weight";
	return name;
}

std::string modDestinationName(const ParamBridge& bridge, backplane::Destination destination) {
	// THE BRIDGE'S OWN ANSWER (`ParamBridge::destinationName`, the host contract's `destinationName`):
	// moved there so the route list, which is one widget over `backplane::DeviceHost` in both hosts
	// (rack roadmap §7f), asks the host it is over. Kept here for the surfaces that still ask by bridge.
	return bridge.destinationName(destination);
}

std::vector<backplane::Source> feathersSources(const ParamBridge& bridge) {
	std::vector<backplane::Source> out;
	for (size_t macro = 0; macro < kMacroCount; ++macro) {
		out.push_back(macroSourceAt(macro));
	}
	for (size_t slot = 0; slot < kWeightSourceCount; ++slot) {
		out.push_back(weightSourceAt(slot));
	}
	// THE STACK INDEX, after the weights (backlog KE step 3).
	out.push_back(stackIndexSource());
	// EVERY DEVICE IN EITHER CHAIN, in the order each chain holds them — a modulator in the strip, and
	// any fx device that ever declares an output. Each output is a source.
	const auto walk = [&](DeviceInstanceId device) {
		if (const device::Descriptor* desc = bridge.descriptorOf(device); desc != nullptr) {
			for (size_t output = 0; output < desc->outputs.size(); ++output) {
				out.push_back(backplane::Source::output(device, output));
			}
		}
	};
	for (size_t slot = 0; slot < bridge.chainState().length(kStripChain); ++slot) {
		walk(bridge.rackDeviceAt(slot));
	}
	for (size_t slot = 0; slot < bridge.chainState().length(kLaneChain); ++slot) {
		walk(bridge.fxDeviceAt(slot));
	}
	return out;
}

//-----------//
// RouteList //
//-----------//

RouteList::~RouteList() {
	// OFF THE LIST BEFORE THE MEMORY GOES — the other half of the `mod_.watch(this)` below.
	mod_.unwatch(this);
	// AND THE FIELD CALLS NOBODY, `MacroSlot::~MacroSlot`'s reason: members die in reverse order.
	field_->onCommit(nullptr);
	field_->onCancel(nullptr);
}

RouteList::RouteList(backplane::DeviceHost& host, ModState& mod, EditHistory& history, Notifier* notifier)
    : host_(host), mod_(mod), history_(history), notifier_(notifier) {
	// Both the focus and the amounts arrive through here — see ModState::touched, which exists because
	// an amount is deliberately not a parameter and so has no registry to be watched through.
	watches_.push_back(mod_.watch(this));
	onMouseWheel() = [this](const visage::MouseEvent& e) { return scrollRowsBy(e.wheel_delta_y < 0.0f ? 1 : (e.wheel_delta_y > 0.0f ? -1 : 0)); };

	// **HIDDEN, NEVER DESTROYED, FROM ITS OWN CALLBACKS** (`text_field.h`).
	field_ = std::make_unique<TextField>();
	field_->onCommit([this](std::string text) {
		field_->setVisible(false);
		commitTyped(text);
	});
	field_->onCancel([this] {
		field_->setVisible(false);
		cancelTyped();
	});
	field_->setVisible(false);
	addChild(field_.get());
	placeField(0);

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A PRESS ON THE LIST OUTSIDE AN OPEN FIELD gives the amount back, as a click anywhere else does
		// (`TextField::focusChanged`) — the list is not a focus target, so it says so itself.
		const bool closedField = field_->editing();
		closeField();
		const int clicks = e.repeatClickCount();
		// **A TRIPLE-CLICK TYPES** (backlog LS: no double-click reset; a triple-click types). Asked
		// BEFORE the hit test: the second click zeroed the row, and a route at nothing is gone, so the rows
		// under the pointer may have moved up. The row is the one the second click zeroed.
		if (clicks >= 3) {
			if (!closedField && zeroed_) {
				beginTyping(*zeroed_);
			}
			zeroed_.reset();
			return;
		}
		zeroed_.reset();
		const Hit hit = hitAt(e.position);
		if (hit.part == Part::Remove) {
			remove(Row { hit.chain, hit.destination });
			return;
		}
		// THE CHIP SHOWS EVERY CHOICE BEFORE ANY IS APPLIED — a menu, never a cycle on click (CLAUDE.md's
		// multivalue-toggle rule). Against the chip, through the shared road.
		if (hit.part == Part::Transfer) {
			const std::vector<Row> found = allRows();
			for (size_t index = 0; index < found.size(); ++index) {
				if (found[index] == Row { hit.chain, hit.destination }) {
					// **A CHIP WHOSE CURVE HAS A COEFFICIENT IS ALSO ITS CONTROL** (backlog PY: a click-drag on the
					// glyph changes the coefficients): the row has no room for a second control, so a
					// VERTICAL DRAG on the chip sets it — Log's and Exp's exponent, Phi's periods — the chip
					// redrawing the curve as it moves and a readout in the name's place; a press that does not move
					// still opens the menu on release. A curve with none (Lin, the rectifiers) opens the menu on the
					// press, as it always did.
					const backplane::Transfer t = transferOf(found[index]);
					if (device::hasCoefficient(t)) {
						chipDrag_ = ChipDrag { found[index], index, e.position.y, t, t, false };
						return;
					}
					showMenuAt(transferMenu(found[index]), *this, chipBounds(index));
					break;
				}
			}
			return;
		}
		if (hit.part != Part::Amount) {
			return;
		}
		if (clicks == 2) {
			// **A DOUBLE-CLICK ZEROES IT**, the mock's rule and every other control's (backlog LS): it typed
			// for a while (route-typein), and the triple-click has that job now. Zeroes rather than removes —
			// the row is still the one the hand is over — and remembers what it replaced, for a third click.
			const Row row { hit.chain, hit.destination };
			const std::vector<Row> found = allRows();
			const auto it = std::find(found.begin(), found.end(), row);
			if (it == found.end()) {
				return;
			}
			Typing zeroed { row, static_cast<size_t>(it - found.begin()), amountOf(row), 0, {} };
			if (zeroed.before != 0.0f) {
				host_.beginRouteGesture();
				commitAmount(row, 0.0f);
				host_.endRouteGesture();
				recordEdit(row, zeroed.before, 0.0f);
				zeroed.serial = history_.lastSerial();
			}
			zeroed_ = zeroed;
			return;
		}
		dragging_ = true;
		dragRow_ = Row { hit.chain, hit.destination };
		anchorY_ = e.position.y;
		fine_ = e.isShiftDown();
		before_ = amountOf(dragRow_);
		anchorAmount_ = before_;
		host_.beginRouteGesture();
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (chipDrag_) {
			const float up = chipDrag_->anchorY - e.position.y;
			// A HAND'S TREMBLE IS NOT A DRAG: past the threshold once, and the press is a drag for good.
			if (!chipDrag_->moved && std::abs(up) < kTransferDragThreshold) {
				return;
			}
			chipDrag_->moved = true;
			const backplane::Transfer next = transferDragTarget(chipDrag_->before, up);
			if (next != chipDrag_->now) {
				chipDrag_->now = next;
				writeTransfer(chipDrag_->row, next);
				mod_.touched();
			}
			redraw();
			return;
		}
		if (!dragging_) {
			return;
		}
		// SHIFT CHANGING MID-DRAG RE-ANCHORS, as the knob's does (`kFineAdjust`).
		if (e.isShiftDown() != fine_) {
			fine_ = e.isShiftDown();
			anchorY_ = e.position.y;
			anchorAmount_ = amountOf(dragRow_);
		}
		const float travel = (anchorY_ - e.position.y) / kDragRange * (fine_ ? kFineAdjust : 1.0f);
		commitAmount(dragRow_,
		             std::clamp(anchorAmount_ + travel, -kMaxRouteAmount, kMaxRouteAmount));
	};

	onMouseUp() = [this](const visage::MouseEvent&) {
		if (chipDrag_) {
			const ChipDrag drag = *chipDrag_;
			chipDrag_.reset();
			if (!drag.moved) {
				// A CLICK: the menu, exactly as every other chip opens it.
				showMenuAt(transferMenu(drag.row), *this, chipBounds(drag.index));
				return;
			}
			// THE WHOLE DRAG IS ONE STEP, from where it started to where it ended — none if it came back.
			if (drag.now != drag.before) {
				history_.record(std::make_unique<RouteTransferEdit>(host_, mod_, mod_.source(), drag.row.chain,
				                                                    drag.row.destination, drag.before, drag.now));
				if (notifier_ != nullptr) {
					notifier_->say(nameOf(drag.row) + " curve " + device::transferName(drag.now) + " "
					               + transferCoefficientText(drag.now));
				}
			}
			redraw();
			return;
		}
		if (!dragging_) {
			return;
		}
		dragging_ = false;
		float amount = amountOf(dragRow_);
		if (std::abs(amount) < kRouteUnroutedBelow) {
			// The rule the knob follows on release, and it has to be the same one: dragged back to
			// nothing means gone, not a route sitting at zero.
			amount = 0.0f;
			commitAmount(dragRow_, amount);
		}
		host_.endRouteGesture();
		if (amount != before_) {
			recordEdit(dragRow_, before_, amount);
		}
	};
	// LAST, after every `=` above — see `installTooltipHover`. The heading's cut name is the only thing it says.
	installTooltipHover(*this, *this);
}

std::vector<backplane::Destination> RouteList::rows() const {
	// THE EDGES OUT OF THIS SOURCE, from the ten routes that exist rather than 464 cells.
	std::vector<backplane::Destination> found;
	host_.forEachRouteFrom(mod_.source(), &found, [](void* ctx, backplane::Destination d) {
		static_cast<std::vector<backplane::Destination>*>(ctx)->push_back(d);
	});
	// IN DESTINATION ORDER — the host's parameters first, then each device's controls, device by id —
	// so a row does not jump when an unrelated route is made. The published list is in the graph's own
	// order, which is not the reader's.
	std::sort(found.begin(), found.end());
	return found;
}

std::vector<RouteList::Row> RouteList::allRows() const {
	++rowBuilds_;
	std::vector<Row> out;
	for (const backplane::Destination& d : rows()) {
		out.push_back(Row { kNoInstance, d });
	}
	// AND THE ROUTES INSIDE EVERY CHAIN IN THE LANE, from this same source (rack roadmap §7): a hand that
	// armed a source and routed it into a chain's filter finds that route here, not only inside.
	for (const backplane::DeviceHost::InnerRoute& r : host_.innerRoutesFrom(mod_.source())) {
		out.push_back(Row { r.chain, r.destination });
	}
	return out;
}

float RouteList::preferredHeight() const {
	// The heading is always drawn, so an empty list still says WHICH macro has nothing routed rather
	// than vanishing and leaving a hand to wonder whether it clicked the wrong card.
	const size_t count = std::max<size_t>(allRows().size(), 1);
	return kHeadingHeight + static_cast<float>(count) * kRowHeight;
}

size_t RouteList::visibleRows() const {
	// A LIST NOT YET LAID OUT (no height) shows everything: the first draw is after a layout, and a case before one reads rows.
	if (height() <= 0.0f) {
		return SIZE_MAX;
	}
	return static_cast<size_t>(std::max(1.0f, std::floor((height() - kHeadingHeight) / kRowHeight)));
}

size_t RouteList::firstRow() const { return firstRowIn(allRows().size()); }

size_t RouteList::firstRowIn(size_t count) const {
	const size_t visible = visibleRows();
	const size_t most = visible == SIZE_MAX || count <= visible ? 0 : count - visible;
	return std::min(firstRow_, most);
}

bool RouteList::scrollRowsBy(int rows) {
	const size_t was = firstRow();
	const long next = static_cast<long>(was) + rows;
	firstRow_ = static_cast<size_t>(std::max(0L, next));
	const size_t now = firstRow(); // clamped against the end
	firstRow_ = now;
	if (now != was) {
		redraw();
	}
	return now != was;
}

visage::Bounds RouteList::rowBounds(size_t index) const { return rowBoundsFrom(index, firstRow()); }

visage::Bounds RouteList::rowBoundsFrom(size_t index, size_t first) const {
	// ROWS ABOVE THE FIRST ARE UP UNDER THE HEADING and rows below the last visible one are past the foot: both are outside
	// `hitAt` and `draw`'s window, so their bounds only have to be somewhere honest.
	const float place = static_cast<float>(static_cast<long>(index) - static_cast<long>(first));
	return { 0.0f, kHeadingHeight + place * kRowHeight, width(), kRowHeight };
}

RouteList::Hit RouteList::hitAt(visage::Point pixels) const {
	const std::vector<Row> found = allRows();
	const size_t first = firstRowIn(found.size());
	const size_t last = visibleRows() == SIZE_MAX ? found.size() : std::min(found.size(), first + visibleRows());
	for (size_t index = first; index < last; ++index) {
		const visage::Bounds row = rowBoundsFrom(index, first);
		if (!row.contains(pixels)) {
			continue;
		}
		// THE × FIRST, for the reason the sound stack tests its switch before its card: it sits inside
		// the row, so a row-first test would swallow every removal.
		if (pixels.x >= row.right() - kRemoveWidth) {
			return { Part::Remove, found[index].destination, found[index].chain };
		}
		// THEN THE CHIP, which sits on the NAME's side of the amount: everything else in the row — the
		// amount's own text included — is still the amount's drag, as it always was.
		if (host_.supportsRouteTransfers() && chipBoundsFrom(index, first).contains(pixels)) {
			return { Part::Transfer, found[index].destination, found[index].chain };
		}
		return { Part::Amount, found[index].destination, found[index].chain };
	}
	return {};
}

visage::Bounds RouteList::amountBounds(size_t index) const { return amountBoundsFrom(index, firstRow()); }

visage::Bounds RouteList::chipBounds(size_t index) const { return chipBoundsFrom(index, firstRow()); }

visage::Bounds RouteList::amountBoundsFrom(size_t index, size_t first) const {
	const visage::Bounds row = rowBoundsFrom(index, first);
	return { row.right() - kRemoveWidth - kRowGap - kAmountWidth, row.y(), kAmountWidth, row.height() };
}

visage::Bounds RouteList::chipBoundsFrom(size_t index, size_t first) const {
	const visage::Bounds row = rowBoundsFrom(index, first);
	const visage::Bounds amount = amountBoundsFrom(index, first);
	return { amount.x() - kRowGap - kChipWidth, row.y() + (row.height() - kChipHeight) * 0.5f, kChipWidth,
		     kChipHeight };
}

backplane::Transfer RouteList::transferOf(const Row& row) const {
	return row.chain == kNoInstance ? host_.routeTransfer(mod_.source(), row.destination)
	                                : host_.innerRouteTransfer(mod_.source(), row.chain, row.destination);
}

visage::PopupMenu RouteList::transferMenu(const Row& row) {
	const backplane::Transfer current = transferOf(row);
	visage::PopupMenu menu;
	// THE ROW'S CURVE — one row per KIND: the kind you are on keeps its coefficient (a pick of it is not an edit),
	// any other is that kind at its default.
	const auto curveOfRow = [current](size_t i) {
		const auto kind = static_cast<backplane::Transfer>(i);
		return device::transferKind(current) == kind ? current : device::transferDefaultOf(kind);
	};
	for (size_t i = 0; i < device::kTransferMenuRows; ++i) {
		const backplane::Transfer t = curveOfRow(i);
		// THE MARK ON THE ONE YOU ARE ON, in the label — `ValueBox::choiceLabelAt`'s reason: `select()` is
		// read by the macOS menu bar and nothing else. The same two-cell column, so the names line up.
		std::string label = t == current ? std::string("X  ") : std::string("     ");
		label += device::transferName(t);
		menu.addOption(static_cast<int>(i), label).enable(host_.transferChoosable(mod_.source(), t));
	}
	// THE PICK IS THE WHOLE EDIT — nothing moves while the menu is merely open. By value, not by `this`
	// alone: the row is copied, so a list that rebuilt its rows while the menu was up still writes the
	// route the hand opened it on.
	menu.onSelection() = [this, row, curveOfRow](int id) {
		if (id < 0 || static_cast<size_t>(id) >= device::kTransferMenuRows) {
			return;
		}
		const backplane::Transfer t = curveOfRow(static_cast<size_t>(id));
		if (!host_.transferChoosable(mod_.source(), t)) {
			return; // a greyed row is not a pick, whoever called this
		}
		pickTransfer(row, t);
	};
	return menu;
}

void RouteList::pickTransfer(const Row& row, backplane::Transfer t) {
	const backplane::Transfer before = transferOf(row);
	if (before == t) {
		return;
	}
	const bool taken = row.chain == kNoInstance
	                       ? host_.setRouteTransfer(mod_.source(), row.destination, t)
	                       : host_.setInnerRouteTransfer(mod_.source(), row.chain, row.destination, t);
	if (!taken) {
		return;
	}
	mod_.touched();
	history_.record(std::make_unique<RouteTransferEdit>(host_, mod_, mod_.source(), row.chain, row.destination,
	                                                    before, t));
	if (notifier_ != nullptr) {
		std::string message = nameOf(row);
		message += " curve ";
		message += device::transferName(t);
		if (device::hasCoefficient(t)) {
			message += " ";
			message += transferCoefficientText(t);
		}
		notifier_->say(message);
	}
	redraw();
}

std::optional<std::string> RouteList::chipDragReadout(size_t index) const {
	if (!chipDrag_ || !chipDrag_->moved || chipDrag_->index != index) {
		return std::nullopt;
	}
	// "Phi", not the menu's "Phi random": the readout has the name's room and no more.
	const char* name = device::isPhiTransfer(chipDrag_->now) ? "Phi" : device::transferName(chipDrag_->now);
	return std::string(name) + " " + transferCoefficientText(chipDrag_->now);
}

std::vector<visage::Point> RouteList::chipPoints(const visage::Bounds& chip, backplane::Transfer t, bool bipolar) {
	// THE SHARED PLOT (`transfer_plot.h`, which the MATH card's glyph draws through too), of the engine's own curve.
	return transferPlotPoints(chip, bipolar, transferPlotSteps(device::isPhiTransfer(t), chip.width()),
	                          [t](float x) { return device::shapeTransfer(t, x); });
}

std::vector<visage::Point> RouteList::chipPointsForTest(size_t index) const {
	const std::vector<Row> found = allRows();
	if (index >= found.size()) {
		return {};
	}
	return chipPoints(chipBounds(index), transferOf(found[index]), host_.sourceIsBipolarNow(mod_.source()));
}

void RouteList::writeTransfer(const Row& row, backplane::Transfer t) {
	(void)(row.chain == kNoInstance ? host_.setRouteTransfer(mod_.source(), row.destination, t)
	                                : host_.setInnerRouteTransfer(mod_.source(), row.chain, row.destination, t));
}

void RouteList::drawChip(visage::Canvas& canvas, const visage::Bounds& chip, backplane::Transfer t,
                         bool bipolar) const {
	// A PLOT, NOT A GLYPH — Lato has none of these shapes, and a picture drawn from `shapeTransfer` itself
	// cannot disagree with what the engine hears. Over the range the source can actually say: −1..1 for a
	// bipolar source, 0..1 for a unipolar one, with y = 0 at the centre either way so a rectifier's floor
	// reads as the floor it is. Lin is drawn dimmer, so a list of ordinary routes stays quiet.
	// THE POINTS ARE `chipPoints`, the case's seam too — so what a case holds is what is stroked, and a drag that
	// moves the coefficient redraws the curve from the same function the engine evaluates.
	strokeTransferPlot(canvas, chip, chipPoints(chip, t, bipolar), t == backplane::Transfer::Lin);
}

float RouteList::amountOf(const Row& row) const {
	return row.chain == kNoInstance ? host_.routeAmount(mod_.source(), row.destination)
	                                : host_.innerRouteAmount(mod_.source(), row.chain, row.destination);
}

std::string RouteList::nameOf(const Row& row) const {
	return row.chain == kNoInstance ? host_.destinationName(row.destination)
	                                : host_.innerDestinationName(row.chain, row.destination);
}

void RouteList::write(const Row& row, float amount) {
	if (row.chain == kNoInstance) {
		host_.setRouteAmount(mod_.source(), row.destination, amount);
		// THE AS-MODULATED READOUT (backlog PU): the box appears beside the destination's face, wherever it is drawn.
		LiveAmount::touch(host_.routeIdentity(), mod_.source(), row.destination);
	}
	else {
		(void)host_.setInnerRouteAmount(mod_.source(), row.chain, row.destination, amount);
	}
}

void RouteList::recordEdit(const Row& row, float before, float after) {
	if (row.chain == kNoInstance) {
		history_.record(std::make_unique<RouteEditOver<backplane::DeviceHost>>(host_, mod_, mod_.source(),
		                                                                    row.destination, before, after));
	}
	else {
		history_.record(
		    std::make_unique<InnerRouteEdit>(host_, mod_, mod_.source(), row.chain, row.destination, before, after));
	}
}

void RouteList::commitAmount(const Row& row, float amount) {
	write(row, amount);
	mod_.touched();
	if (notifier_ != nullptr) {
		const std::string name = nameOf(row);
		char printed[96] = {};
		std::snprintf(printed, sizeof(printed), "%s amount %+d%%", name.c_str(),
		              static_cast<int>(std::lround(amount * 100.0f)));
		notifier_->say(printed);
	}
	redraw();
}

void RouteList::remove(const Row& row) {
	const float before = amountOf(row);
	if (before == 0.0f) {
		return;
	}
	host_.beginRouteGesture();
	write(row, 0.0f);
	host_.endRouteGesture();
	mod_.touched();
	recordEdit(row, before, 0.0f);
	if (notifier_ != nullptr) {
		std::string message = nameOf(row);
		message += " unrouted";
		notifier_->say(message);
	}
	redraw();
}

std::optional<float> RouteList::parseAmount(std::string_view text) {
	// MOVED TO `device::parseAmountPercent` (backlog LS), so a knob in mapping mode types the same thing.
	return device::parseAmountPercent(text, kMaxRouteAmount);
}

std::string RouteList::printAmountForEdit(float amount) { return device::printAmountPercent(amount); }

void RouteList::beginTyping(const Typing& zeroed) {
	if (!isVisible()) {
		return; // a field nobody can see would hold the keyboard invisibly (`MacroSlot::beginRename`)
	}
	typing_ = zeroed;
	placeField(zeroed.index);
	field_->setVisible(true);
	// ON THE AMOUNT THE DOUBLE-CLICK ZEROED, not on the zero.
	field_->begin(printAmountForEdit(zeroed.before));
	typing_->opened = field_->text();
}

void RouteList::commitTyped(const std::string& text) {
	if (!typing_) {
		return;
	}
	const std::optional<float> typed = parseAmount(text);
	// NOT A NUMBER, OR THE TEXT IT OPENED ON: nothing moves, as for an Escape — the zero is taken back.
	if (!typed || text == typing_->opened) {
		cancelTyped();
		return;
	}
	const Typing t = *typing_;
	typing_.reset();
	// THE RELEASE'S RULE, for the release's reason: an amount at nothing is gone, not a route at 0 %.
	float after = *typed;
	if (std::abs(after) < kRouteUnroutedBelow) {
		after = 0.0f;
	}
	const float now = amountOf(t.row);
	if (after == now) {
		return;
	}
	// ONE STEP FOR THE WHOLE TRIPLE-CLICK: folded into the zero's, while that is still the latest thing on
	// the stack, so one undo goes back to the amount the hand started on.
	const bool fold = t.serial != 0 && history_.lastSerial() == t.serial;
	if (fold) {
		history_.beginGroup("Type", t.serial);
	}
	host_.beginRouteGesture();
	commitAmount(t.row, after);
	host_.endRouteGesture();
	recordEdit(t.row, now, after);
	if (fold) {
		history_.endGroup();
	}
	// TYPED BACK ONTO THE AMOUNT IT STARTED AT: a step that does nothing, taken back whole.
	if (fold && after == t.before) {
		(void)history_.retractLast(history_.lastSerial());
	}
}

void RouteList::cancelTyped() {
	if (!typing_) {
		return;
	}
	const Typing t = *typing_;
	typing_.reset();
	// THE ZERO'S STEP TAKEN BACK, undone and gone, so a triple-click then Escape changes nothing at all...
	if (t.serial != 0 && history_.retractLast(t.serial)) {
		mod_.touched();
		redraw();
		return;
	}
	// ...or, when something has been recorded since, the amount put back as an edit of its own.
	const float now = amountOf(t.row);
	if (now != t.before) {
		host_.beginRouteGesture();
		commitAmount(t.row, t.before);
		host_.endRouteGesture();
		recordEdit(t.row, now, t.before);
	}
}

void RouteList::placeField(size_t index) {
	const visage::Bounds amount = amountBounds(index);
	const visage::Bounds chip = chipBounds(index);
	const float left = chip.x();
	field_->setBounds(left, amount.y() + 1.0f, amount.right() - left, amount.height() - 2.0f);
}

void RouteList::resized() {
	// NEVER UNPOSITIONED, even hidden — explore's "create() positions every child before anything can
	// draw" holds every frame in the tree to a real size. At rest it waits over the first row; a
	// triple-click moves it to the row it edits — which, zeroed on the way, may have left the list, and then
	// it stays where it opened. Unconditional, for the scale-change rule in CLAUDE.md.
	size_t index = 0;
	if (field_->editing() && typing_) {
		index = typing_->index;
		const std::vector<Row> found = allRows();
		const auto it = std::find(found.begin(), found.end(), typing_->row);
		if (it != found.end()) {
			index = static_cast<size_t>(it - found.begin());
		}
	}
	placeField(index);
}

void RouteList::closeField() {
	if (field_->isVisible()) {
		field_->setVisible(false);
	}
}

void RouteList::visibilityChanged() {
	// visage tells only the frame whose own flag moved (`MacroSlot::visibilityChanged`), so a list
	// hidden mid-edit gives the amount back itself.
	if (!isVisible()) {
		closeField();
	}
}

RouteList::HeadingName RouteList::headingName() const {
	const float labelWidth = trackedWidth(font(kKnobLabelSize), "ROUTES", kKnobLabelTracking);
	const float room = std::max(0.0f, static_cast<float>(width()) - labelWidth - 8.0f);
	HeadingName out;
	out.whole = mod_.sourceName(mod_.source());
	out.fitted = fitName(out.whole, room, kKnobReadoutSize, kNameFitFloorSize, 0.0f);
	out.room = { static_cast<float>(width()) - room, 0.0f, room, kHeadingHeight };
	return out;
}

std::optional<LabelFace> RouteList::labelFaceAt(visage::Point local) const {
	const HeadingName name = headingName();
	if (name.whole.empty() || !name.room.contains(local)) {
		return std::nullopt;
	}
	// THE NAME IS ITS OWN — a macro's typed one, a sound's — so the face is the name against itself: it says only a cut.
	return LabelFace { .label = name.whole, .full = name.whole, .cut = name.fitted.elided || name.fitted.text != name.whole };
}

void RouteList::draw(visage::Canvas& canvas) {
	canvas.setColor(kMuted);
	drawTracked(canvas, "ROUTES", font(kKnobLabelSize), kKnobLabelTracking, 0.0f, 0.0f, kHeadingHeight);

	// WHOSE ROUTES, named in the head — which this class's own doc has always said it did ("the head
	// names the focused macro") and which it did not, back when the strip's four cards were the only
	// things that could hold the focus and the answer was always on screen a few inches to the left.
	// The arrow grid above broke that: a hand that armed Sound C's weight sees a list of destinations
	// with nothing to say which source they belong to. Right-aligned, so the section still reads as
	// ROUTES first.
	canvas.setColor(mod_.armed() ? kModulation : kMuted);
	// NAMED BY THE STATE'S OWN NAMER — the panel that arms sources installs it (Feathers: `modSourceName`;
	// the rack: its editor), so the head and the arm agree whichever host this list is in.
	// **A MACRO'S NAME IS FITTED, NOT SPILLED** (backlog OT): the room after "ROUTES", shrunk to the floor before it is cut.
	const FittedName headName = headingName().fitted;
	canvas.text(headName.text, font(headName.size), visage::Font::kRight, 0.0f, 0.0f, width(), kHeadingHeight);

	const std::vector<Row> found = allRows();
	if (found.empty()) {
		canvas.setColor(kDisabled);
		canvas.text("nothing routed", font(kKnobReadoutSize), visage::Font::kLeft, 0.0f, kHeadingHeight,
		            width(), kRowHeight);
		return;
	}

	const bool chips = host_.supportsRouteTransfers();
	const bool bipolar = host_.sourceIsBipolarNow(mod_.source());
	const size_t first = firstRowIn(found.size());
	const size_t last = visibleRows() == SIZE_MAX ? found.size() : std::min(found.size(), first + visibleRows());
	for (size_t index = first; index < last; ++index) {
		const visage::Bounds row = rowBoundsFrom(index, first);
		const visage::Bounds amount = amountBoundsFrom(index, first);
		const visage::Bounds chip = chipBoundsFrom(index, first);
		// THE NAME GETS WHAT IS LEFT OF THE CHIP (or of the amount, with no chip), fitted, so a long one
		// stops short of both rather than running under them.
		const float nameRight = (chips ? chip.x() : amount.x()) - kRowGap;
		// **WHILE A CHIP IS DRAGGED, THE NAME'S ROOM IS ITS READOUT** — the coefficient, right-aligned against the
		// chip, in the modulation colour; the name comes back on release.
		if (const std::optional<std::string> readout = chipDragReadout(index); readout.has_value()) {
			canvas.setColor(kModulation);
			canvas.text(fitText(*readout, font(kKnobReadoutSize), nameRight), font(kKnobReadoutSize),
			            visage::Font::kRight, 0.0f, row.y(), nameRight, row.height());
		}
		else {
			canvas.setColor(kMuted);
			canvas.text(fitText(nameOf(found[index]), font(kKnobReadoutSize), nameRight), font(kKnobReadoutSize),
			            visage::Font::kLeft, 0.0f, row.y(), nameRight, row.height());
		}

		if (chips) {
			drawChip(canvas, chip, transferOf(found[index]), bipolar);
		}

		// The amount in the modulation colour, because that is what it is: the one number on this
		// surface that is neither a value nor a name. Right-aligned in its own box, which ends where it
		// always ended — 4 px before the ×.
		char printed[16] = {};
		std::snprintf(printed, sizeof(printed), "%+d%%",
		              static_cast<int>(std::lround(amountOf(found[index]) * 100.0f)));
		canvas.setColor(kModulation);
		canvas.text(printed, font(kKnobReadoutSize), visage::Font::kRight, amount.x(), amount.y(), amount.width(),
		            amount.height());

		// The ×, drawn as two crossed strokes rather than set as a glyph — A16 again.
		const float cx = row.right() - kRemoveWidth * 0.5f;
		const float cy = row.y() + row.height() * 0.5f;
		const float arm = 3.5f;
		canvas.setColor(kDisabled);
		canvas.segment(cx - arm, cy - arm, cx + arm, cy + arm, 1.3f, true);
		canvas.segment(cx - arm, cy + arm, cx + arm, cy - arm, 1.3f, true);
	}
}

//----------//
// ModPanel //
//----------//

ModPanel::~ModPanel() {
	mod_.unwatch(this);
	// AND THE ARM LISTENER, for the same reason as the watch: it captures `this`.
	mod_.setArmListener(nullptr);
	// THE NAMER AND THE LISTER TOO — each captures `this`, so a knob outliving this panel would call
	// into a dead object the first time somebody right-clicked it.
	mod_.setSourceNamer(nullptr);
	mod_.setSourceLister(nullptr);
	// AND THE RENAME SUBSCRIPTION, which captures `this` — ended here rather than left to the member's
	// own destructor so the order is written down: the bridge outlives the editor, and a rename heard
	// after this panel went would refresh slots that are gone. Only OUR subscription ends; the host's
	// (A9) is its own token and hears on.
	renameSubscription_.reset();
	labelSubscription_.reset();
}

ModPanel::ModPanel(ParamBridge& bridge, backplane::DeviceHost& host, WidgetRegistry& registry, ModState& mod, EditHistory& history,
                   Notifier* notifier)
    : Rail("MODULATORS", Rail::Side::Left), bridge_(bridge), host_(host), registry_(registry), mod_(mod),
      history_(history), notifier_(notifier) {
	relativeTurn_ = std::make_unique<RelativeMacroGesture>(bridge, host);
	// **THE SELECTION OVER THE STRIP** (backlog PN) — the fx lane's `LaneSelection` over the bridge's strip site,
	// built FIRST because every layout ends in `applySelection`.
	LaneSelection::Hooks selectionHooks;
	selectionHooks.order = [this] { return stripOrder(); };
	selectionHooks.changed = [this] {
		applySelection();
		redraw();
	};
	selectionHooks.cardBounds = [this](DeviceInstanceId id) -> std::optional<visage::Bounds> {
		const std::vector<DeviceInstanceId> order = stripOrder();
		const auto at = std::find(order.begin(), order.end(), id);
		if (at == order.end() || collapsed()) {
			return std::nullopt;
		}
		return sourceBounds(sourceOfOutput(static_cast<size_t>(at - order.begin()), 0));
	};
	selection_ = std::make_unique<LaneSelection>(bridge_.stripFragmentSite(), history_, notifier_,
	                                             std::move(selectionHooks));
	selectionOverlay_ = std::make_unique<SelectionOverlay>(*selection_);
	for (size_t source = 0; source < kMacroCount; ++source) {
		// SHORT CAPTIONS IN THE ROW. "Macro 1" is 54px of text in a 54px column; the row's own heading
		// says what these are, so each knob only has to say which. modSourceName still answers with the
		// full name everywhere a source is named in prose — the route list, the tray — because there
		// the context is missing.
		// **THROUGH ITS POLARITY (backlog EG):** the knob is built over a `BipolarMacro` view of the
		// parameter's control, which reads the bridge's mode each call — so a switch in the menu below
		// re-ranges the knob, its readout and its default at once, and the parameter underneath, which
		// the DAW automates, never changes shape. `paramKnob` would have owned the control inside the
		// knob; here the panel owns both layers, and subscribes the knob for redraws the way it does.
		const ParamDesc* desc = findParam(static_cast<clap_id>(macroParam(source)));
		macroParams_.push_back(std::make_unique<ParamControl>(bridge, desc));
		macroViews_.push_back(std::make_unique<BipolarMacro>(
		    *macroParams_.back(), [&bridge, source] { return bridge.macroBipolar(source); }));
		// **A SLOT, NOT A BARE KNOB (A7):** the knob, the chip a Latch or Momentary macro becomes, and
		// the field Rename opens, over the one view — the type picks which is shown (`macro_slot.h`).
		// THE MODE SWITCH IS ON THE CONTROL — a mode switch or context menu — and the type and
		// the name beside it (`macro_menu.h`). The verbs are the bridge's, so a case flips them without
		// a menu; the panel repaints every knob because each one reading this macro's routes draws a
		// different reach now, and the macro's own slot because it reads through the new polarity —
		// found by index at the pick rather than captured, the shape the rack's row needs (it rebuilds).
		// `this` IS SAFE TO HOLD HERE, unlike the rack editor's (`RackEditor::alive_`): the panel dies only
		// with the root frame, which is the frame visage parents the popup to, so no pick can outlive it.
		// Rename and the re-seat after a type pick are the slot's own, and guard themselves.
		auto slot = std::make_unique<MacroSlot>(
		    *macroViews_.back(), kMacroShortNames[source],
		    MacroSlot::Naming {
		        .name = [&bridge, source] { return bridge.macroName(source); },
		        .renamed = [&bridge, source] { return bridge.macroRenamed(source); },
		        // EACH OF THE THREE VERBS IS ONE UNDO STEP (backlog JB), written back through the same verb by
		        // index — the name as the bridge reads it, which the verb normalises the same way going back.
		        .rename =
		            [&bridge, source](const std::string& name) {
			            (void)writeStoredAsEdit<std::string>(
			                bridge.editHistory(), [&bridge, source] { return bridge.macroName(source); },
			                [&bridge, source](const std::string& n) { bridge.setMacroName(source, n); }, name,
			                "Rename");
		            },
		    },
		    MacroMenuHooks {
		        .readBipolar = [&bridge, source] { return bridge.macroBipolar(source); },
		        .writeBipolar =
		            [this, &bridge, source](bool bipolar) {
			            (void)writeStoredAsEdit<bool>(
			                bridge.editHistory(), [&bridge, source] { return bridge.macroBipolar(source); },
			                [&bridge, source](const bool& b) { bridge.setMacroBipolar(source, b); }, bipolar,
			                "Polarity");
			            if (source < slots_.size()) {
				            slots_[source]->knob().redraw();
				            slots_[source]->chip().redraw();
			            }
			            redraw();
		            },
		        .readType = [&bridge, source] { return bridge.macroType(source); },
		        .writeType =
		            [this, &bridge, source](device::MacroType type) {
			            (void)writeStoredAsEdit<device::MacroType>(
			                bridge.editHistory(), [&bridge, source] { return bridge.macroType(source); },
			                [&bridge, source](const device::MacroType& t) { bridge.setMacroType(source, t); }, type,
			                "Macro type");
			            redraw();
		            },
		        .beginRename = [] {}, // the slot's own
		        // FEATHERS' HEADER MACROS HAVE THE RELATIVE ROAD (backlog PQ): the slot's encoder, below.
		        .offerRelative = true,
		    },
		    notifier);
		// BOTH CONTROLS WATCH THE PARAMETER: whichever is on show has to follow the host's automation.
		if (desc != nullptr) {
			slot->holdWatch(registry.watchToken(static_cast<clap_id>(desc->id), &slot->knob()));
			slot->holdWatch(registry.watchToken(static_cast<clap_id>(desc->id), &slot->chip()));
		}
		// **A RELATIVE MACRO'S TURN EDITS EVERY BASE IT IS ROUTED TO** — one undo step per drag, into the history
		// every other macro verb above writes to (`relative_macro.h`).
		slot->setRelativeHooks(EncoderFace::Hooks {
		    .begin = [this, source] { relativeTurn_->begin(source); },
		    .turn = [this](double delta) { relativeTurn_->turn(delta); },
		    .end = [this, &bridge] { relativeTurn_->end(bridge.editHistory()); },
		});
		slot->setFaceDiameter(kMacroDiameter);
		// A MACRO IS NOT ITSELF A DESTINATION, so its own knob is never mappable and never draws a ring
		// — but it is still handed the state, because the panel's cards redraw when the arm moves and a
		// knob that watched nothing would keep the old focus ring for a frame.
		slot->setModulation(&mod, &history);
		addChild(slot.get());
		slots_.push_back(std::move(slot));
	}
	// **A NAME HEARD FROM THE HOST RELABELS ITS SLOT** — a rename from anywhere (the slot's own field
	// included, which is harmless: the caption is already right) and each name a patch restore moved.
	// Ended in the destructor: the bridge outlives this panel.
	// THE TOOLTIP LAYER, over the whole rail — `EngineRail`'s arrangement (see `tips_`).
	addChild(&tips_);
	tips_.watch(*this);
	renameSubscription_ = bridge_.subscribeMacroRename([this](size_t macro) {
		if (macro < slots_.size()) {
			slots_[macro]->refresh();
		}
	});
	// **A RELABELLED OUTPUT RE-LAYS THE STRIP**: `pruneOutputArms` drops the arm whose label moved and
	// `resized` builds it again with the new word at the new word's width. Heard inside the MACROS card's
	// rename commit, which is safe because nothing here destroys a card — the card's key is its kind and
	// its device, and a label is neither.
	//
	// **TWO NAMES FOR ONE AUTHORITY, AND THIS PANEL ASSUMES THEY ARE ONE.** The strip reads and hears
	// labels through `bridge_` (here, `outputName`); a `MacrosCard` in it reads and hears them through
	// `host_`, the `DeviceHost` it was built with. The two agree only because `host_` IS `bridge_`'s
	// adapter (`BridgeDeviceHost`, whose label verbs are one line each onto the bridge) — the one call
	// site, `Gui`, builds it so. Handed a host over some other store, a rename typed on a card would
	// relabel that store while the arm under the same card went on printing the bridge's word. Checked
	// where it can be: a `BridgeDeviceHost` over a different bridge is the mistake, stated.
	assert(dynamic_cast<const BridgeDeviceHost*>(&host_) == nullptr
	       || &dynamic_cast<const BridgeDeviceHost*>(&host_)->bridge() == &bridge_);
	labelSubscription_ = bridge_.subscribeOutputLabel([this](DeviceInstanceId, size_t) {
		resized();
		redraw();
	});

	// ONE CARD PER RACK SLOT, built for the POOL rather than for the count. A slot the rack does not
	// currently hold keeps a real card that is simply not shown — the dormant-widget discipline — so
	// the browser turning one on is a visibility flip rather than a construction, and nothing has to
	// be built on the audio thread's schedule.
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		ensureCard(slot);
	}

	// The borders are what says which source an arrow would arm, so they follow the flag in the same
	// frame it moves.
	watches_.push_back(mod_.watch(this));

	// **THE LIT CARD IS THE SELECTED CARD — Delete takes the lit card, by design.** Arming a DEVICE
	// source selects its card, whichever road armed it (`ModState::arm` says
	// why it is said there and not per caller). The chip already selects on its own press, so for the
	// chip this is the same answer twice; for the Engine rail's grid, or any road written tomorrow that
	// arms a device source without touching its card, this is the only road. A macro or a sound
	// weight armed here holds no card and selects nothing. Disarming is not heard, so it deselects
	// nothing. And the card takes the keyboard focus, so Delete after such an arm walks from the lit
	// card up to this panel's handler exactly as it does after a press on its face — a road that arms
	// from outside the strip would otherwise leave the focus somewhere the walk never reaches us.
	// **AND WHAT TO CALL A SOURCE, INSTALLED HERE FOR THE SAME REASON THE ARM LISTENER IS.** This
	// panel is the layer that holds both the routing vocabulary and the host's slots; a `Knob` holds
	// neither, and backlog DI puts a list of source NAMES under the pointer on any destination in the
	// editor. Asked live rather than captured as a table, because a device source is named after the
	// device in its slot and that changes when the rack is edited — which is exactly when somebody is
	// reading the list.
	mod_.setSourceNamer([this](backplane::Source source) { return modSourceName(bridge_, source); });
	// **AND WHAT SOURCES THERE ARE, FOR THE SAME REASON** — the inbound-route menu walks them, and only
	// this layer walks the devices (`feathersSources`). Asked live, like the name.
	mod_.setSourceLister([this] { return feathersSources(bridge_); });

	mod_.setArmListener([this](backplane::Source source) {
		const size_t slot = rackSlotOfSource(source);
		if (slot == kNoPosition) {
			if (onArmedElsewhere) {
				onArmedElsewhere(source); // the fx lane's modulator, say — see the declaration
			}
			return;
		}
		selectSlot(slot);
		if (slot < cards_.size() && cards_[slot] != nullptr) {
			cards_[slot]->requestKeyboardFocus();
		}
	});

	// **DELETE AND BACKSPACE REMOVE THE SELECTED MODULATOR.** `FxLane`'s handler, the same three
	// codes, for the reason it
	// gives: a gesture that works in one lane and not the other is worse than one that works in
	// neither, because the hand learns it and then it fails somewhere.
	//
	// **WHERE THIS SITS, AND WHY A KEY FINDS IT.** `WindowEventHandler::handleKeyDown` starts at the
	// keyboard-focused frame and walks UP through ancestors that `acceptsKeystrokes()`. A press
	// anywhere on a mod card — its head, a value box, a knob — focuses the CARD, because `FxCard`
	// accepts keystrokes precisely so that the focus lands on it (fx_card.h, the constructor's note);
	// the card handles no key, so `processKeyPress` there returns false and the walk moves on to the
	// card's PARENT, which is this panel. The editor root accepts keystrokes too (`gui.cpp`, for
	// Escape) and is the terminus AFTER this one: a Delete answered here never reaches it, and an
	// Escape declined here does. The handler on the panel is the handler the walk cannot miss from
	// any card in the strip and cannot reach from a card in the fx lane.
	//
	// **THROUGH `removeDeviceAtLane`, THE HEAD MENU'S OWN VERB — NEVER A SECOND REMOVAL ROAD.** That
	// verb is measured clean: the removed device's route cells read zero and the survivors' are
	// untouched. A second road is a second chance to skip the route clear, which is what the week's
	// ghost-modulation reports were (backlog CN).
	//
	// **THE TARGET IS RESOLVED THE INSTANT BEFORE IT IS USED.** `selectedSlot()` turns the selected
	// device's ID into its CURRENT position; nothing here holds a position across anything. The fx
	// lane, keyed by position, has to reset its focus before the removal so that a held key does not
	// walk the chain — here the removal clears the id itself, and a stale id names nothing.
	//
	// A KEY NOBODY WANTS IS NOT SWALLOWED: `false` hands it on.
	setAcceptsKeystrokes(true);
	onKeyPress() = [this](const visage::KeyEvent& e) {
		// THE SELECTION'S KEYS FIRST (backlog PN, the fx lane's order): Ctrl/Cmd + A C X V D, and Delete over two
		// or more. A single selected card's Delete falls through to the strip's own road below.
		if (selection_->key(e)) {
			afterVerb();
			return true;
		}
		const visage::KeyCode code = e.keyCode();
		if (code != visage::KeyCode::Delete && code != visage::KeyCode::Backspace
		    && code != visage::KeyCode::KPBackspace) {
			return false;
		}
		const size_t going = selectedSlot();
		if (going >= deviceCount()) {
			return false;
		}
		return removeDeviceAtLane(going);
	};

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// THE CLICK COUNT, RECORDED FOR `focusOrArm` — the fold reads it and nothing else does.
		foldClicks_ = e.repeat_click_count;
		const Hit hit = hitAt(e.position);
		// A PRESS ON A CARD'S HEAD MAY BECOME A DRAG. Recorded before the verbs are dispatched,
		// because the parts that win here — fold, the dots — are the ones a drag has to override, and
		// below the threshold they still do.
		if (hit.part == Part::Fold || hit.part == Part::SlotMenu) {
			dragSlot_ = rackSlotOfSource(hit.source);
			dragAnchorY_ = e.position.y;
			dragging_ = false;
			// **A PRESS ON A FOLDED CARD'S HEAD SELECTS IT TOO.** An OPEN card's head is the card
			// frame's and the card reports the press through `onCardPressed`; a FOLDED card is its
			// head and nothing else, and `foldBounds` records that the frame at a folded head is the
			// PANEL. "Anywhere on their face" has to include the only face a folded card has.
			// WITH THE MODIFIER THE HAND HELD (backlog PN): the same two roads a card's own frame takes.
			if (const DeviceInstanceId id = instanceAtSlot(*dragSlot_); id != kNoInstance) {
				selection_->focusPress(id);
				selection_->cardPress(id, e.isShiftDown()        ? CardSelection::Press::Range
				                          : e.isMainModifier() ? CardSelection::Press::Toggle
				                                               : CardSelection::Press::Plain);
			}
		}
		else if (hit.part == Part::None && !collapsed()) {
			// THE BARE STRIP: a lasso (backlog PN). A click that does not drag clears the selection.
			selection_->lassoBegin(e.position, e.isShiftDown());
		}
		focusOrArm(hit);
	};

	onMouseDrag() = [this](const visage::MouseEvent& e) {
		if (selection_->lassoRect().has_value()) {
			selection_->lassoMove(e.position);
			return;
		}
		if (!dragSlot_.has_value() || !dragAnchorY_.has_value()) {
			return;
		}
		if (!dragging_ && std::abs(e.position.y - *dragAnchorY_) < kDragThreshold) {
			return;
		}
		dragging_ = true;
		dropGap_ = dropGapAt(e.position);
		redraw();
	};

	onMouseUp() = [this](const visage::MouseEvent& e) {
		if (selection_->lassoRect().has_value()) {
			selection_->lassoMove(e.position);
			selection_->lassoEnd();
			return;
		}
		const std::optional<size_t> from = dragSlot_;
		const std::optional<size_t> gap = dropGap_;
		const bool moved = dragging_;
		dragSlot_.reset();
		dropGap_.reset();
		dragAnchorY_.reset();
		dragging_ = false;
		if (moved && from.has_value() && gap.has_value()) {
			dropCard(*from, *gap);
		}
		else if (!moved && from.has_value()) {
			// A FOLDED HEAD'S PRESS THAT DID NOT DRAG: inside a selection of several, it collapses to the card here.
			selection_->cardClick(instanceAtSlot(*from));
		}
		redraw();
	};

	// DRAGGING OUT IS THE CANCEL, the sound rail's rule: the pointer leaving drops the TARGET rather
	// than the gesture, so a hand that wanders out and back is still holding the card.
	onMouseExit() = [this](const visage::MouseEvent&) {
		if (dropGap_.has_value()) {
			dropGap_.reset();
			redraw();
		}
	};
}

std::optional<size_t> ModPanel::dropGapAt(visage::Point position) const {
	const size_t rows = deviceCount();
	if (rows == 0) {
		return std::nullopt;
	}
	if (position.x < 0.0f || position.x >= width() || position.y < 0.0f || position.y >= height()) {
		return std::nullopt;
	}
	std::vector<RowSpan> spans;
	spans.reserve(rows);
	for (size_t slot = 0; slot < rows; ++slot) {
		const visage::Bounds box = envCardBounds(slot);
		spans.push_back({ box.y(), box.bottom() });
	}
	return gapAlong(position.y, spans);
}

visage::Bounds ModPanel::gapBounds(size_t gap) const {
	const size_t rows = deviceCount();
	if (gap > rows) {
		return {};
	}
	const float thickness = 2.0f;
	float y = 0.0f;
	if (gap < rows) {
		y = envCardBounds(gap).y() - kCardGap * 0.5f - thickness * 0.5f;
	}
	else {
		y = envCardBounds(rows - 1).bottom() + kCardGap * 0.5f - thickness * 0.5f;
	}
	return { kPadX, y, kCardWidth, thickness };
}

const char* ModPanel::deviceName(size_t slot) const {
	// THROUGH THE REGISTRY, from the tag the row actually carries — the same road modDestinationName
	// takes to name a control, and for the same reason: one authority, so two surfaces cannot
	// disagree about what a device is called.
	const ModRack rack = bridge_.modRack();
	if (slot >= rack.count || slot >= rack.device.size()) {
		return "";
	}
	const device::Descriptor* desc = deviceForTag(rack.device[slot].tag.data());
	// AN UNKNOWN TAG NAMES NOTHING RATHER THAN NAMING THE ENVELOPE. A slot holding a device this
	// build does not have is exactly the case the old line got wrong, one step further out.
	return desc != nullptr ? desc->traits.name : "Device";
}

size_t ModPanel::updateCards() {
	size_t told = 0;
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		FxCard* card = cards_[slot].get();
		// **VISIBILITY IS ASKED HERE AS WELL AS INSIDE THE CARD**, which reads as belt and braces and
		// is not: a card in a slot the rack has shrunk past is still allocated and still holds its
		// last position, and `FxLane::updateCards()` guards the same way for the same reason.
		if (card != nullptr && card->isVisible() && card->update()) {
			++told;
		}
	}
	return told;
}

size_t ModPanel::syncMacroSlots() {
	size_t moved = 0;
	for (const std::unique_ptr<MacroSlot>& slot : slots_) {
		moved += slot->syncType() ? 1u : 0u;
	}
	return moved;
}

size_t ModPanel::deviceCount() const {
	// THE RACK DECIDES HOW MANY CARDS THERE ARE, not the panel. One today; the browser will make it
	// more without this file changing.
	return std::min(bridge_.modRack().count, cards_.size());
}

const device::Descriptor* ModPanel::descriptorOf(size_t slot) const {
	// **ONE LOOKUP, AND IT ANSWERS NULL FOR EVERY WAY A SLOT CAN HOLD NOTHING** — past the rack's
	// count, past the array, or a tag this build cannot name. Three call sites spelled this out
	// separately and each had to remember all three ways.
	const ModRack rack = bridge_.modRack();
	if (slot >= rack.count || slot >= rack.device.size()) {
		return nullptr;
	}
	return deviceForTag(rack.device[slot].tag.data());
}

size_t ModPanel::outputCountOf(size_t slot) const {
	const device::Descriptor* desc = descriptorOf(slot);
	// A DEVICE THIS BUILD CANNOT NAME PUBLISHES NOTHING AS FAR AS WE KNOW, which is the honest answer
	// and gives it no arrow rather than a guess.
	return desc != nullptr ? desc->outputCount() : 0;
}

float ModPanel::outputRowHeight(size_t slot) const {
	// ZERO FOR A DEVICE WITH NO OUTPUTS, so the row costs nothing to a device that has none — and an
	// fx device dropped in here one day would get no arrow FROM THE RULE rather than from a check on
	// which lane it is. OWLTT is the case that proves it.
	// **TIMES THE ROWS, so a device whose chips wrap gets a foot tall enough to hold them.**
	return outputCountOf(slot) > 0
	           ? kOutputRowHeight * static_cast<float>(std::max<size_t>(outputRowsOf(slot), 1))
	           : 0.0f;
}

visage::Bounds ModPanel::outputRowBounds(size_t slot) const {
	const float height = outputRowHeight(slot);
	if (height <= 0.0f) {
		return {};
	}
	// **AT THE BOTTOM, WHICH IS THE CONVENTION MADE A CONTRACT.** It sat directly under the head, so
	// a card's outputs interrupted the head and its body; by design they sit at the foot of the card,
	// in one frame shared with the fx lane. `ui::cardFrameOf` is where that split now lives, and
	// both hosts read it — a rack card's arms land in the same strip by construction rather than by
	// each host being careful.
	const visage::Bounds card = envCardBounds(slot);
	return cardFrameOf(card, outputCountOf(slot), CardHeadEdge::Top, outputRowsOf(slot)).outputs;
}

std::string ModPanel::outputName(size_t slot, size_t output) const {
	const device::Descriptor* desc = descriptorOf(slot);
	if (desc == nullptr || output >= desc->outputs.size()) {
		return {};
	}
	// **THE INSTANCE'S LABEL, NOT THE DECLARATION** (macro-modulation B3): a MACROS card's renamed
	// output is called by its new name on its chip, as in every route list. The bridge answers the
	// declared name for an output nobody relabelled.
	return bridge_.outputLabel(bridge_.rackDeviceAt(slot), output);
}

std::vector<std::string> ModPanel::outputNamesOf(size_t slot) const {
	std::vector<std::string> names;
	const size_t count = outputCountOf(slot);
	names.reserve(count);
	for (size_t output = 0; output < count; ++output) {
		names.push_back(outputName(slot, output));
	}
	return names;
}

float ModPanel::outputChipWidth(size_t slot, size_t output) const {
	// **ONE COMPUTATION, AND IT DOES NOT NEED THE WIDGET TO EXIST YET.** This measured the label
	// itself and added its own padding — a second definition of a width the arm decides, and the two
	// disagreed the moment the chip became a `ui::SourceArm`. Asking the live widget fixed that and
	// broke something else: a caller may ask BEFORE a layout has placed anything, and it answered
	// zero. `widthFor` is the shared answer either way.
	return SourceArm::widthFor(outputName(slot, output));
}

size_t ModPanel::rackSlotOfSource(backplane::Source source) const {
	// A HOST SOURCE NAMES NO DEVICE AND BELONGS TO NO SLOT — and the rack is asked whether it holds the
	// device the endpoint names, so an fx device's output or an id nothing holds answers "not mine" by
	// the same lookup that answers "mine" (kill-lanes step 2: there is no source index to do arithmetic
	// on, and so no plausible wrong number to get from one).
	return source.isDevice() ? bridge_.rackSlotOf(source.device) : kNoPosition;
}


size_t ModPanel::outputRowsOf(size_t slot) const {
	// **HOW MANY ROWS THIS SLOT'S CHIPS NEED — the same walk `outputChipBounds` does, and it has to
	// be, because chips are different widths and where one lands depends on every chip before it.**
	//
	// **THE WALK IS `ui::outputChipRowsFor` NOW, WHICH IS THE SAME FUNCTION `outputChipBounds` PLACES
	// WITH.** It was two walks in this file kept in step by both calling `outputChipWidth`, which is a
	// discipline and not a guarantee — and the fx lane and the rack, which had to do the same work,
	// had neither walk at all. One function, three hosts (backlog FL).
	const device::Descriptor* desc = descriptorOf(slot);
	if (desc == nullptr || desc->outputCount() == 0) {
		return 0;
	}
	// **FROM `kCardWidth`, NOT FROM `envCardBounds` — AND THAT IS A CYCLE, NOT A SHORTCUT.**
	// `envCardBounds` sums the heights of the cards above this one, each height comes from
	// `sourceHeight`, and that now includes the outputs strip — which is what this function computes.
	// Asking it here recursed until the stack ran out; the first draft did exactly that and every case
	// touching a chip died with SIGSEGV rather than a wrong number.
	//
	// The width is what this needs and the width is a constant: a rack card is `kCardWidth` wide and
	// the strip spans it (`cardFrameOf`, head at the top). So there is nothing to ask.
	return outputChipRowsFor(*desc, kCardWidth, outputNamesOf(slot), host_.outputsShown(bridge_.rackDeviceAt(slot)));
}
visage::Bounds ModPanel::outputChipBounds(backplane::Source source) const {
	// **COMPUTED FROM STATE, NOT READ OFF THE WIDGET — and that is a contract, not a preference.**
	//
	// I made this ask the live `ui::SourceArm` for its rectangle, which is right about there being one
	// definition and wrong about WHERE it lives. These accessors are pure functions of the rack's
	// state, and callers rely on that: a device added after the last layout has no arm yet, so asking
	// the widget answered "nowhere" for a chip that certainly exists. MEASURED — `outputCountOf(1)`
	// was 0 through every `resized()` in the case that caught it, and 1 by the time it asked.
	//
	// So the geometry is worked out here and `resized` places the arm INTO it. The widget follows the
	// rectangle; it does not define it.
	const size_t slot = rackSlotOfSource(source);
	if (slot == kNoPosition) {
		return {};
	}
	const size_t output = source.index;
	const size_t outputs = outputCountOf(slot);
	if (output >= outputs) {
		return {};
	}
	// A FOLDED CARD IS ITS HEAD AND NOTHING ELSE — the outputs strip is body, so it goes with it.
	// `sourceHeight`'s note: *"an arrow that survived a fold would be the head carrying an output
	// again by the back door."*
	if (cardMinimised(sourceOfOutput(slot, 0))) {
		return {};
	}
	const CardFrame frame = cardFrameOf(envCardBounds(slot), outputs, CardHeadEdge::Top,
	                                    outputRowsOf(slot));
	if (frame.outputs.height() <= 0.0f) {
		return {};
	}
	// **LAID FROM THE LEFT AND WRAPPED, BY THE SHARED WALK** — `ui::outputChipBoundsFor`. The names
	// are the devices' and a fixed step is a guess about their width that is wrong for somebody, so
	// which row a chip lands on depends on every chip before it; `outputRowsOf` reserves the rows
	// through the SAME function, so a count and a placement cannot drift apart.
	const device::Descriptor* desc = descriptorOf(slot);
	if (desc == nullptr) {
		return {};
	}
	const std::vector<visage::Bounds> chips = outputChipBoundsFor(*desc, frame.outputs, outputNamesOf(slot), host_.outputsShown(bridge_.rackDeviceAt(slot)));
	return output < chips.size() ? chips[output] : visage::Bounds {};
}

float ModPanel::sourceHeight(backplane::Source source) const {
	if (rackSlotOfSource(source) != kNoPosition) {
		// A folded card is its head row and nothing else — the mock's `.mod-card.min > *:not(.head)`.
		// The head is the strip's, so an open card is the head PLUS the device's own body.
		// A FOLDED CARD IS ITS HEAD AND NOTHING ELSE — the outputs row is body, so it folds away with
		// the device's own surface. An arrow that survived a fold would be the head carrying an
		// output again by the back door.
		if (cardMinimised(source)) {
			return kHeadHeight;
		}
		const size_t slot = rackSlotOfSource(source);
		// THE CARD'S OWN HEIGHT, NOT A CONSTANT NAMED AFTER ONE DEVICE — the same sentence `resized`
		// already carried, four lines from the call that honoured it. This one did not.
		//
		// MEASURED, 2026-08-24: EnvCard 144, MsegCard 140. So the strip reserved 188 for a slot whose
		// card `resized` then sized at 184, and every card below an MSEG sat 4 px low. Small, live,
		// and exactly the seam stage 3 was bitten by — two places that must agree about one geometry,
		// disagreeing because only one of them was updated when the rack stopped holding one device.
		// **THE CARD'S WHOLE HEIGHT, HEAD AND OUTPUTS STRIP INCLUDED.** It used to be assembled here —
		// head plus output row plus body — because the strip drew the first two. The card frames
		// itself now, so `laneHeight()` already counts all three and adding them again put this
		// panel's outputs strip BELOW the card it belonged to, with a gap where the double-count was.
		// The note above about two places agreeing on one geometry applies to itself: this is now one
		// place, asked of the card.
		return bodyHeightOf(slot);
	}
	// **A DEVICE SOURCE NO CHAIN HOLDS HAS NO HEIGHT** — the same third arm `arrowBounds` below just
	// grew, for the same reason. It used to reach `bodyHeightOf` with an out-of-range slot and get a
	// dormant card's height back, which is a reservation for a card that does not exist; falling
	// through to the macro row instead would be the same guess wearing a different number. Nothing is
	// drawn for it, so nothing is reserved for it. (A dormant slot's head source is one of these — see
	// `sourceOfOutput`.)
	if (source.isDevice()) {
		return 0.0f;
	}
	// EVERY MACRO REPORTS THE WHOLE ROW, because a macro's rectangle IS a column of a row they share
	// and the row's height is what any caller asking "how tall is this" wants back.
	return kMacroRowHeight;
}

float ModPanel::contentHeight() const {
	// SUMMED RATHER THAN MULTIPLIED, because the pieces have different heights and a device card can
	// fold. This is the number the scroll is clamped against, so a fold that did not reach it would
	// leave the strip able to scroll past its own end.
	float total = kPadTop + kHeadingHeight + kCardGap + kMacroRowHeight + kCardGap;
	for (size_t slot = 0; slot < deviceCount(); ++slot) {
		total += sourceHeight(sourceOfOutput(slot, 0)) + kCardGap;
	}
	// The heading's own padding is repeated at the bottom, so the strip ends the way it begins rather
	// than with the CV card's border against the frame.
	//
	// AND THE (+) IS IN THE SUM. A row the scroll extent did not know about is a row a full strip
	// cannot be scrolled far enough to reach — which is the same class of bug as the fold that did
	// not reach this number, one row further down.
	return total + kAddHeight + kCardGap + kCvCardHeight + kPadTop;
}

float ModPanel::maxScroll() const {
	return std::max(0.0f, contentHeight() - height());
}

void ModPanel::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped == scroll_) {
		return;
	}
	scroll_ = clamped;
	// THE KNOBS ARE CHILD FRAMES, which is the one way this list differs from the sound rail: they are
	// placed rather than drawn, so the scroll has to move them itself. A card that slid up the strip
	// with its dial left behind is a failure mode a self-drawn list cannot have.
	resized();
	redraw();
}

bool ModPanel::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		return false;
	}
	// ONE NOTCH MOVES A ROW OF CONTROLS, which used to be "a whole card" — the sound rail's unit, for
	// the sound rail's reason: the thing the eye is tracking. A device card is now most of the strip's
	// height, so a notch of one card would be the whole view and the wheel would teleport rather than
	// scroll. The macro row is the smaller unit and the better one.
	setScroll(scroll_ - e.precise_wheel_delta_y * (kMacroRowHeight + kCardGap));
	return true;
}

visage::Bounds ModPanel::macroRowBounds() const {
	// THE SCROLL IS IN THE GEOMETRY, so a hit test, a knob's placement and the drawing all follow it
	// without one of them being told: what sits under a coordinate changes when the strip moves.
	return { kPadX, kPadTop + kHeadingHeight + kCardGap - scroll_, kCardWidth, kMacroRowHeight };
}

visage::Bounds ModPanel::macroColumnBounds(size_t macro) const {
	const visage::Bounds row = macroRowBounds();
	const float columnWidth = row.width() / static_cast<float>(kMacroColumns);
	return { row.x() + static_cast<float>(macro) * columnWidth, row.y(), columnWidth, row.height() };
}

visage::Bounds ModPanel::envCardBounds(size_t slot) const {
	// WALKED RATHER THAN INDEXED, because the cards above this one may be folded and a card's place
	// is the sum of what is actually above it.
	float top = kPadTop + kHeadingHeight + kCardGap + kMacroRowHeight + kCardGap;
	for (size_t above = 0; above < slot; ++above) {
		top += sourceHeight(sourceOfOutput(above, 0)) + kCardGap;
	}
	return { kPadX, top - scroll_, kCardWidth, sourceHeight(sourceOfOutput(slot, 0)) };
}

visage::Bounds ModPanel::sourceBounds(backplane::Source source) const {
	if (const size_t slot = rackSlotOfSource(source); slot != kNoPosition) {
		return envCardBounds(slot);
	}
	if (isMacroSource(source)) {
		return macroColumnBounds(source.index);
	}
	// A weight source has no rectangle in this strip at all — it is armed from the Engine rail's arrow
	// grid. Answering an empty rectangle is the honest reply; answering a plausible one would put a
	// hit test somewhere nothing is drawn.
	return {};
}

DeviceInstanceId ModPanel::instanceOfSource(backplane::Source source) const {
	// THE DEVICE THIS SOURCE BELONGS TO — which the endpoint names outright (kill-lanes step 2), so the
	// only question left is whether the strip holds it. It used to be lane -> slot -> row -> id, every
	// hop taken in one pass because the lane and the slot both move; the id is what was always KEPT.
	return rackSlotOfSource(source) != kNoPosition ? source.device : kNoInstance;
}

bool ModPanel::cardMinimised(backplane::Source source) const {
	// A MACRO HAS NO FOLD since it stopped being a card — see the header. False rather than an
	// assertion, because "is this folded" is a reasonable question to ask of any source and the
	// honest answer for a macro is no.
	const DeviceInstanceId id = instanceOfSource(source);
	// **ASKED OF THE BRIDGE, WHICH IS WHERE A FOLD LIVES** (a fold lives in the patch). This was
	// a set on this panel, and a set here cannot be right across a load: ids are patch-scoped, so the
	// incoming patch's device 1 would have inherited the outgoing one's fold. The bridge's map is emptied
	// and refilled by the load itself, so the question cannot be asked of the wrong patch.
	return id != kNoInstance && bridge_.deviceFolded(id);
}

bool ModPanel::setCardMinimised(backplane::Source source, bool minimised) {
	const DeviceInstanceId id = instanceOfSource(source);
	if (id == kNoInstance || cardMinimised(source) == minimised) {
		return false;
	}
	bridge_.setDeviceFolded(id, minimised);
	// Every card below this one has moved, so the whole strip is re-placed and the scroll re-clamped
	// against a content height that just changed. resized() does both.
	resized();
	redraw();
	if (foldChanged_) {
		foldChanged_(source);
	}
	return true;
}

visage::Bounds ModPanel::slotMenuBounds(backplane::Source source) const {
	// EMPTY FOR ANYTHING THAT IS NOT A DEVICE CARD — see `foldBounds` beside it.
	if (rackSlotOfSource(source) == kNoPosition) {
		return {};
	}
	// TOP, STATED. This panel's cards are a vertical column and their heads are along the top; the
	// default that used to supply this silently is gone precisely because a spine head existed
	// elsewhere and nothing made these two sites say which they were.
	return cardMenuBounds(sourceBounds(source), CardHeadEdge::Top);
}

std::vector<ModPanel::SlotOption> ModPanel::slotOptionsForTest(size_t slot) const {
	const size_t devices = deviceCount();
	// "ADD BEFORE" FIRST AND REMOVE LAST — the fx lane's order, for the fx lane's reason (backlog CR).
	const bool room = slot < devices; // the strip grows (unbounded devices): there is always room
	return { { SlotVerb::AddBefore, room },
		     { SlotVerb::AddAfter, room },
		     { SlotVerb::MoveUp, slot > 0 && slot < devices },
		     { SlotVerb::MoveDown, devices > 0 && slot + 1 < devices },
		     { SlotVerb::Remove, slot < devices } };
}

bool ModPanel::slotChoiceForTest(size_t slot, SlotVerb verb) {
	for (const SlotOption& option : slotOptionsForTest(slot)) {
		if (option.verb != verb) {
			continue;
		}
		if (!option.enabled) {
			return false;
		}
		switch (verb) {
		case SlotVerb::AddBefore:
			// THE PICKER, NOT AN INSERT — the device is chosen there. `insertChoiceForTest` is the
			// whole gesture for a case. Same road as the fx lane's (backlog CR).
			showAddMenu(slot);
			return true;
		case SlotVerb::AddAfter:
			// THE SAME PICKER AT THE SAME CARD; the pick goes behind it (`insertDeviceAfter`).
			showAddMenu(slot, true);
			return true;
		case SlotVerb::MoveUp:
			return moveDevice(slot, slot - 1);
		case SlotVerb::MoveDown:
			return moveDevice(slot, slot + 1);
		case SlotVerb::Remove:
			return removeDeviceAtLane(slot);
		}
	}
	return false;
}

size_t ModPanel::insertChoiceForTest(size_t slot, size_t index) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	if (index >= offered.size() || slot >= deviceCount()) {
		return kNoPosition;
	}
	// THE SAME CAPTURE THE PICKER MAKES: the card's DEVICE at the moment the gesture starts.
	return insertDeviceBefore(bridge_.rackDeviceAt(slot), offered[index]->traits.tag);
}

size_t ModPanel::insertDeviceBefore(DeviceInstanceId anchor, const char* tag) {
	const size_t slot = bridge_.insertRackDeviceBefore(anchor, tag);
	reserveCards(bridge_.chainState().length(kStripChain));
	if (slot == kNoPosition) {
		if (notifier_ != nullptr) {
			notifier_->say("Rack full");
		}
		return slot;
	}
	if (notifier_ != nullptr) {
		const device::Descriptor* desc = deviceForTag(tag);
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " added");
	}
	// ONE UNDO STEP, THE SAME RECORD AN APPEND MAKES — see `addDevice`.
	const ModRack rack = bridge_.modRack();
	if (slot < rack.device.size()) {
		history_.record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Rack, slot,
		                                                 rack.device[slot], std::vector<backplane::Route> {},
		                                                 true));
	}
	resized();
	redraw();
	return slot;
}

bool ModPanel::removeDeviceAtLane(size_t slot) {
	// LIFTED BEFORE THE REMOVAL, because afterwards they do not exist — and for a RACK device the list
	// carries both directions. A modulator publishes outputs, so removing one drops every route leading
	// OUT of it as well as every route into it; `routesOf` names both, by the device's id.
	const ModRack rack = bridge_.modRack();
	if (slot >= rack.count || slot >= rack.device.size()) {
		return false;
	}
	const DeviceRow removed = rack.device[slot];
	std::vector<backplane::Route> routes = bridge_.routesOf(bridge_.rackDeviceAt(slot));
	const device::Descriptor* desc = deviceForTag(removed.tag.data());
	if (!bridge_.removeRackDevice(slot)) {
		return false;
	}
	history_.record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Rack, slot, removed,
	                                                 std::move(routes), false));
	if (notifier_ != nullptr) {
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " removed");
	}
	// THE SELECTION DIES WITH ITS DEVICE. An id naming a removed device is already inert — the strip's order no
	// longer holds it — but an undo puts the device back *id and all* (`mod_rack.h`), and a device that came back
	// selected would be a key's target the hand never chose again. Pruned here, on the verb, so the menu and the
	// key agree.
	selection_->prune();
	// REBUILT, NOT JUST REDRAWN. Every card from the hole down is now the face of a different device.
	resized();
	redraw();
	return true;
}

DeviceInstanceId ModPanel::instanceAtSlot(size_t slot) const {
	const ModRack rack = bridge_.modRack();
	if (slot >= rack.count || slot >= rack.device.size()) {
		return kNoInstance;
	}
	return rack.device[slot].instanceId;
}

std::vector<DeviceInstanceId> ModPanel::stripOrder() const {
	std::vector<DeviceInstanceId> out;
	const rack::ChainRecord& record = bridge_.chainRecord();
	for (size_t p = 0; p < record.length(kStripChain); ++p) {
		out.push_back(record.deviceAt(kStripChain, p));
	}
	return out;
}

size_t ModPanel::selectedSlot() const {
	// RESOLVED NOW, FROM THE RACK AS IT IS — the selection's ANCHOR, which is the card a single key acts on. A
	// position is never stored — see the header — so the answer moves with the device across a removal above it or
	// a reorder, and reads `kNoFocus` the moment the device is gone.
	const DeviceInstanceId anchor = selection_->selection().anchor();
	if (anchor == kNoInstance) {
		return kNoFocus;
	}
	const std::vector<DeviceInstanceId> order = stripOrder();
	const auto at = std::find(order.begin(), order.end(), anchor);
	return at == order.end() ? kNoFocus : static_cast<size_t>(at - order.begin());
}

void ModPanel::selectSlot(size_t slot) {
	// A PRESS THAT DID NOT SAY HOW (an output chip, an arm from elsewhere): keep a selection that already holds the
	// card, else select it alone — the touch rule a knob's press follows (`CardSelection::touch`).
	const DeviceInstanceId id = instanceAtSlot(slot);
	if (id == kNoInstance || selection_->selection().contains(id)) {
		return;
	}
	const std::array<DeviceInstanceId, 1> one { id };
	selection_->select(one);
}

void ModPanel::clearSelection() {
	if (selection_->selection().empty()) {
		return;
	}
	selection_->clear();
}

void ModPanel::applySelection() {
	// ONE READ OF THE RACK FOR THE WHOLE PASS, so every card is told against the same snapshot — the
	// contemporaneous-handles rule; comparing ids read in one pass is fine, and this is one pass.
	const ModRack rack = bridge_.modRack();
	const size_t live = std::min(rack.count, rack.device.size());
	const CardSelection& s = selection_->selection();
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		if (cards_[slot] == nullptr) {
			continue;
		}
		cards_[slot]->setFocused(slot < live && s.contains(rack.device[slot].instanceId));
	}
	if (selectionOverlay_ != nullptr) {
		selectionOverlay_->redraw();
	}
}

void ModPanel::afterVerb() {
	// THE CARDS FOLLOW THE CHAIN — room for what a paste or a duplicate added, then the layout every edit ends in.
	reserveCards(bridge_.chainState().length(kStripChain));
	selection_->prune();
	resized();
	redraw();
}

void ModPanel::dropCard(size_t from, size_t gap) {
	const DeviceInstanceId dragged = instanceAtSlot(from);
	if (dragged != kNoInstance && selection_->movesBlock(dragged)) {
		// **A SELECTED CARD CARRIES THE SELECTION** (backlog PN, the fx lane's OA rule): the block lands at the gap,
		// in order, one undo step.
		if (selection_->verbs().moveBlock(selection_->selection().inOrder(stripOrder()), gap)) {
			afterVerb();
		}
		return;
	}
	// THE SHARED OFF-BY-ONE, not a fourth copy of it — `rowForGap` answers nullopt for both gaps either side of the
	// dragged row, so a drop where it already is moves nothing rather than spending an edit that changes nothing.
	if (const std::optional<size_t> to = rowForGap(from, gap, deviceCount()); to.has_value()) {
		moveDevice(from, *to);
	}
}

bool ModPanel::slotVerbForTest(size_t slot, FragmentVerb verb) {
	const DeviceInstanceId id = instanceAtSlot(slot);
	if (id == kNoInstance) {
		return false;
	}
	if (!selection_->chooseVerb(kVerbIds + static_cast<int>(verb), kVerbIds, id)) {
		return false;
	}
	afterVerb();
	return true;
}

bool ModPanel::moveDevice(size_t from, size_t to) {
	if (!bridge_.moveRackDevice(from, to)) {
		return false;
	}
	history_.record(std::make_unique<DeviceMoveEdit>(bridge_, DeviceLane::Rack, from, to));
	if (notifier_ != nullptr) {
		notifier_->say(from < to ? "Moved later" : "Moved earlier");
	}
	resized();
	redraw();
	return true;
}

void ModPanel::showSlotMenu(size_t slot) {
	// RECORDED BEFORE THE GUARD — see `lastSlotMenuForTest`. What a case needs to know is what the
	// press RESOLVED TO, and a refusal is one of the answers it can resolve to.
	lastSlotMenu_ = slot;
	if (slot >= deviceCount()) {
		return;
	}
	// A MENU RATHER THAN A BARE REMOVE BUTTON, AND THE MENU IS THE CONFIRMATION — the fx lane's
	// reasoning, and the same conclusion for the same reason. A remove is destructive; undo now
	// exists for it, but two deliberate presses at two different places still cannot happen by
	// accident, and the menu has to exist anyway to hold the two reorder verbs.
	const std::vector<SlotOption> options = slotOptionsForTest(slot);
	visage::PopupMenu menu;
	for (size_t i = 0; i < options.size(); ++i) {
		const char* label = "Remove";
		if (options[i].verb == SlotVerb::AddBefore) {
			label = "Add before…";
		}
		else if (options[i].verb == SlotVerb::AddAfter) {
			label = "Add after…";
		}
		else if (options[i].verb == SlotVerb::MoveUp) {
			label = "Move earlier";
		}
		else if (options[i].verb == SlotVerb::MoveDown) {
			label = "Move later";
		}
		auto& option = menu.addOption(static_cast<int>(i), label);
		option.enable(options[i].enabled);
	}
	// AND THE SELECTION'S VERBS (backlog PN, the fx lane's): over the selection when it holds this card, else this
	// card alone.
	selection_->addVerbs(menu, kVerbIds, instanceAtSlot(slot));
	menu.onSelection() = [this, slot, options](int id) {
		if (id >= kVerbIds) {
			(void)slotVerbForTest(slot, static_cast<FragmentVerb>(id - kVerbIds));
		}
		else if (id >= 0 && static_cast<size_t>(id) < options.size()) {
			slotChoiceForTest(slot, options[static_cast<size_t>(id)].verb);
		}
	};
	const visage::Bounds box = slotMenuBounds(sourceOfOutput(slot, 0));
	showMenuAt(std::move(menu), *this, box);
}

visage::Bounds ModPanel::foldBounds(backplane::Source source) const {
	// **A DEVICE CARD'S MARK, AND NOTHING ELSE'S.** A macro column has no fold, so it must answer an
	// EMPTY rectangle rather than a plausible one — a hit test somewhere nothing is drawn is the
	// failure `sourceBounds` already documents for weight sources.
	if (rackSlotOfSource(source) == kNoPosition) {
		return {};
	}
	// **THE WHOLE HEAD IS THE FOLD TARGET NOW — not a mark, and not nothing.**
	//
	// This returned `{}` when the `(-)` rule was retired (the double-click fold replaces it), on the
	// reasoning that the CARD would take the head press and fold itself.
	// **MEASURED, and that reasoning was wrong in one direction: a MINIMISED card's head is not the
	// card's to take.** `frameAtPoint` at a folded card's head answers the PANEL — the card frame no
	// longer covers it — so with the mark gone there was no road back and a collapsed modulator could
	// not be reopened by clicking it at all.
	//
	// So the panel keeps a fold target and it is the head itself, which is exactly the double-click
	// gesture expressed as a rectangle. It is not an invisible control: the head is drawn, and the
	// whole of it is the button.
	const visage::Bounds card = sourceBounds(source);
	return { card.x(), card.y(), card.width(), kHeadHeight };
}

visage::Bounds ModPanel::arrowBounds(backplane::Source source) const {
	const float size = kArrowSize + 6.0f;
	if (rackSlotOfSource(source) != kNoPosition) {
		// THE GLYPH INSIDE THE CHIP. The whole chip is the button — a press on the label arms too —
		// but a caller asking for the ARROW must get the arrow: a case proves a point is PAST the
		// glyph and still on the control, and handing it the chip would make that check vacuous.
		const visage::Bounds chip = outputChipBounds(source);
		if (chip.width() <= 0.0f) {
			return {};
		}
		// THE SAME INSET `ui::SourceArm` DRAWS AT. A number of this panel's own here would be a second
		// opinion about where the arrow sits inside its chip, which is the fault this whole change has
		// been about — so it is asked of the widget when there is one and matches its constant when
		// there is not.
		return { chip.x() + SourceArm::kChipInset, chip.y() + (chip.height() - size) * 0.5f, size,
			     size };
	}
	// **AND A SOURCE THAT IS NEITHER GETS NOTHING — the branch this needed and did not have.**
	//
	// MEASURED, by the case that went red when the test above became an occupancy test: `arrowBounds`
	// had no macro guard at all, because `isRackSource` answered YES for a device source on an EMPTY
	// rack lane and the chip road below then declined it. So the number-test was LOAD-BEARING here —
	// it was what kept a dead device source off the macro road — and swapping in an occupancy test
	// sent that source down to `macroColumnBounds`, which does arithmetic on the index without
	// bounding it and handed back a 19-wide rectangle for a source that names nothing.
	//
	// The dispatch was always three-way and only said two of them. `sourceBounds` five functions up
	// has carried the third arm the whole time — *"a weight source has no rectangle in this strip at
	// all... answering a plausible one would put a hit test somewhere nothing is drawn"* — and this
	// is that sentence applied to the arrow.
	if (!isMacroSource(source)) {
		return {};
	}
	// A MACRO'S ARROW IS STILL THIS PANEL'S OWN, drawn under its column — macros are not devices and
	// have no card to carry a strip.
	const visage::Bounds column = macroColumnBounds(source.index);
	if (column.width() <= 0.0f) {
		return {};
	}
	return { column.x() + (column.width() - size) * 0.5f, column.bottom() - size, size, size };
}

visage::Bounds ModPanel::addBounds() const {
	// UNDER THE LAST CARD, which is where the next device will actually go. The strip's order is the
	// rack's order, so a `(+)` at the end is a picture of an append; anywhere else would draw an
	// insertion, and there is no insertion behind it.
	const size_t devices = deviceCount();
	const visage::Bounds last = devices > 0 ? envCardBounds(devices - 1) : macroRowBounds();
	return { kPadX, last.bottom() + kCardGap, kCardWidth, kAddHeight };
}

visage::Bounds ModPanel::cvCardBounds() const {
	// BELOW THE (+), so the placeholder that waits on a cable is not sitting between the rack and the
	// button that grows it.
	return { kPadX, addBounds().bottom() + kCardGap, kCardWidth, kCvCardHeight };
}

std::vector<const device::Descriptor*> ModPanel::addableForTest() const {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& d : kDevices) {
		// BOTH, AND THEY ANSWER DIFFERENT QUESTIONS. `maySitInModRack` is the CAPABILITY — the rack will
		// physically take this device — and `isTaggedModulator` is the device's own statement that it
		// belongs here. A capable device that does not call itself a modulator is a device the rack could
		// hold and a player did not come to this strip looking for; the tag is what keeps it out of the
		// menu without making the rack refuse it.
		if (maySitInModRack(d) && belongsInModulationRack(d)) {
			out.push_back(&d);
		}
	}
	return out;
}

size_t ModPanel::addChoiceForTest(size_t index) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	if (index >= offered.size()) {
		return kNoPosition;
	}
	return addDevice(offered[index]->traits.tag);
}

size_t ModPanel::addDevice(const char* tag) {
	// ARPS SENT TO FRONT OF CHAIN: an arp's steps are what the devices after it hear, so a picked
	// emitter goes in front of every modulator while the setting is on — through the insert road, one undo step.
	if (pickGoesToFront(deviceForTag(tag), false, deviceCount())) {
		return insertDeviceBefore(bridge_.rackDeviceAt(0), tag);
	}
	return appendDevice(tag);
}

size_t ModPanel::appendDevice(const char* tag) {
	const size_t slot = bridge_.addRackDevice(tag);
	reserveCards(bridge_.chainState().length(kStripChain)); // a card for every device, at once (unbounded devices)
	if (slot == kNoPosition) {
		// REFUSED, AND SAID SO. A full rack is the case a hand will actually meet, and a button that
		// answered a press with nothing at all is the one thing worse than a disabled one.
		if (notifier_ != nullptr) {
			notifier_->say("Rack full");
		}
		return slot;
	}
	if (notifier_ != nullptr) {
		const device::Descriptor* desc = deviceForTag(tag);
		notifier_->say(std::string(desc != nullptr ? desc->traits.name : "Device") + " added");
	}
	// RECORDED AS THE DEVICE THAT WAS ACTUALLY MADE, serial and all, rather than as "an add of tag T".
	// addRackDevice mints a fresh serial, so a redo that re-added would hand the player a device with
	// a new identity; re-INSERTING the recorded one survives a round trip. See device_edits.h.
	//
	// THIS IS THE GAP THE (+) SHIPPED WITH. An add was not undoable because undoing one means removing
	// a device, and until this commit no road did that for the rack.
	const ModRack rack = bridge_.modRack();
	if (slot < rack.device.size()) {
		history_.record(std::make_unique<DeviceSlotEdit>(bridge_, DeviceLane::Rack, slot,
		                                                 rack.device[slot], std::vector<backplane::Route> {},
		                                                 true));
	}
	// LAID OUT, NOT JUST REDRAWN — and the difference is the whole of the defect where a new card did not
	// appear until something else (a scroll, say) disturbed the UI.
	//
	// The cards exist from construction; what a new one does NOT have is a size, a place and its
	// visible flag, and all three come from resized(). redraw() alone marks the strip's own frame
	// stale, and the strip duly redrew — around a card that was still invisible and zero-sized. A
	// scroll calls resized(), which is why scrolling made the device appear and why it looked like a
	// repaint problem rather than a layout one.
	resized();
	redraw();
	return slot;
}

size_t ModPanel::insertDeviceAfter(DeviceInstanceId anchor, const char* tag) {
	const size_t devices = deviceCount();
	for (size_t at = 0; at < devices; ++at) {
		if (bridge_.rackDeviceAt(at) == anchor) {
			return at + 1 < devices ? insertDeviceBefore(bridge_.rackDeviceAt(at + 1), tag) : appendDevice(tag);
		}
	}
	return kNoPosition; // the card the hand meant is gone
}

void ModPanel::showAddMenu(size_t before, bool after) {
	// THE DEVICES THE RACK WILL TAKE **AND THAT CALL THEMSELVES MODULATORS**, ASKED OF THE DEVICES. Not
	// a list kept here: `maySitInModRack` reads the descriptor's own declarations and `isTaggedModulator`
	// reads its own tags, so a device joins this menu by what it declares, in both halves. A menu written
	// out by hand is the shape that let FilterPoly ship with no control.
	//
	// THE SECOND HALF IS THE NEW ONE: `addableForTest` filters on the `"modulator"` tag
	// (`isTaggedModulator`, device_row.h) as well as on the capability, so a device that CAN sit in the
	// rack without claiming to be a modulator is not offered here. SKLITTER is not a mod effect.
	//
	// **AND SKLITTER IS EXACTLY THAT DEVICE, SO THE FILTER IS THE ONLY THING KEEPING IT OUT.** This
	// comment used to say the opposite — *"NO DEVICE IN THIS BUILD IS IN THAT CLASS YET … the filter
	// changes nothing a hand can see"* — and every clause of it was false by the time it was read.
	// `maySitInModRack` is `outputCount() > 0` and SKLITTER publishes seven (Perf X/Y, the four
	// weights, Perf Speed); its `kTags` are `{"glitch", "fx"}` and do not include `"modulator"`. So
	// the capable set and the tagged set differ by precisely this device, the filter's whole visible
	// effect is excluding it from this menu, and that exclusion IS the intended behaviour rather than
	// an accident to be tidied. **The case the old comment cited as evidence for the coincidence was
	// deleted in the commit that broke it** (written d1c50642, invalidated 96ed8299), so the sentence
	// outlived its own proof — which is why a comment asserting a coincidence between two sets is
	// worth less than the assertion `isTaggedModulator ⟹ maySitInModRack` that the suite still makes.
	//
	// **WHY THIS MATTERED ENOUGH TO REWRITE**: read as it stood, it says the filter is a no-op, which
	// would make deleting it look safe. It is load-bearing.
	//
	// Backlog FM is the browser that turns this baked filter into a CLEARABLE default; until it
	// lands, this menu is the whole of the strip's picker and the filter has no off switch.
	if (addableForTest().empty()) {
		return;
	}
	visage::PopupMenu menu = addMenuForTest(before, after);
	// AT THE (+) FOR AN APPEND; AT THE CARD'S DOTS FOR AN INSERT, where the hand already is — the
	// same box the slot menu itself opens under.
	if (before < deviceCount()) {
		const visage::Bounds box = slotMenuBounds(sourceOfOutput(before, 0));
		showMenuAt(std::move(menu), *this, box);
	}
	else {
		showMenuAt(std::move(menu), *this, addBounds());
	}
}

visage::PopupMenu ModPanel::addMenuForTest(size_t before, bool after) {
	const std::vector<const device::Descriptor*> offered = addableForTest();
	visage::PopupMenu menu;
	// FILED BY CATEGORY (`device_menu.h`) — which for this strip, offering modulators only, is one
	// category and so a flat list, alphabetical. Every row is live: the strip grows (unbounded devices step 6).
	addCategorisedDevices(menu, offered, [](size_t) { return true; });
	// THE ANCHOR IS CAPTURED NOW, AS THE CARD'S DEVICE — its identity, not the position the menu was
	// drawn at (backlog CR, by design); a card that moves between this open and the
	// pick is still the card the hand meant.
	const bool inserting = before < deviceCount();
	const DeviceInstanceId anchor = inserting ? bridge_.rackDeviceAt(before) : kNoInstance;
	menu.onSelection() = [this, offered, inserting, anchor, after](int id) {
		if (id >= 0 && static_cast<size_t>(id) < offered.size()) {
			const char* tag = offered[static_cast<size_t>(id)]->traits.tag;
			if (inserting && after) {
				insertDeviceAfter(anchor, tag);
			}
			else if (inserting) {
				insertDeviceBefore(anchor, tag);
			}
			else {
				addDevice(tag);
			}
		}
	};
	return menu;
}

ModPanel::Hit ModPanel::hitAt(visage::Point pixels) const {
	// THE TOGGLE BEFORE ANYTHING ELSE, and before the early return below, because it is the only part
	// a closed rail has: everything under it is a card that is not being drawn.
	if (toggleHit(pixels)) {
		return { Part::Collapse, {} };
	}
	if (collapsed()) {
		return {};
	}
	// THE DEVICE CARDS FIRST, because their small verbs are the ones that would be swallowed. The
	// macro row has no buttons inside a larger target, so its order does not matter.
	for (size_t slot = 0; slot < deviceCount(); ++slot) {
		const backplane::Source source = sourceOfOutput(slot, 0);
		// THE SMALL VERBS BEFORE THE CARD THEY SIT ON, as the sound stack tests its own: both buttons
		// are inside the card, so a card-first test would swallow every one of them.
		if (slotMenuBounds(source).contains(pixels)) {
			return { Part::SlotMenu, source };
		}
		if (foldBounds(source).contains(pixels)) {
			return { Part::Fold, source };
		}
		// EVERY OUTPUT THIS DEVICE DECLARES, not just the first. `arrowBounds` takes a SOURCE, and a
		// source is (device, output) — so walking the count is what makes a second output reachable.
		for (size_t output = 0; output < outputCountOf(slot); ++output) {
			const backplane::Source at = sourceOfOutput(slot, output);
			// THE WHOLE CHIP, NOT THE GLYPH. The name beside the arrow is what tells one output from
			// another, so it is part of the control rather than a caption near it — a label that
			// named a button and did not press it would be the affordance lying about its own edge.
			if (outputChipBounds(at).contains(pixels)) {
				return { Part::Arrow, at };
			}
		}
		// AND THE HEAD NO LONGER ARMS — it folds, as an audio effect's head does. `Part::Card` was
		// the head rectangle arming the source; arming attaches to an OUTPUT and outputs are in the
		// body now, so the head has one verb and the fold owns it. Falling through to the fold below
		// rather than returning early is the whole change.
		const visage::Bounds card = sourceBounds(source);
		if (visage::Bounds { card.x(), card.y(), card.width(), kHeadHeight }.contains(pixels)) {
			return { Part::Fold, source };
		}
	}
	// THE (+) BEFORE THE MACRO ROW, though they cannot overlap — the button is below every card and
	// the row is above them. Ordered with the cards rather than after everything for the same reason
	// they are: it is a small target and the things near it are large.
	if (addBounds().contains(pixels)) {
		return { Part::Add, {} };
	}
	for (size_t macro = 0; macro < kMacroCount; ++macro) {
		if (arrowBounds(macroSourceAt(macro)).contains(pixels)) {
			return { Part::Arrow, macroSourceAt(macro) };
		}
		// THE COLUMN MINUS ITS KNOB. The dial is a child frame with its own mouse handling, so a
		// column-wide target here would sit under it and never see a press anyway — but the caption
		// and the space around the arrow should still focus the macro, which is what this catches.
		if (macroColumnBounds(macro).contains(pixels)) {
			return { Part::Macro, macroSourceAt(macro) };
		}
	}
	return {};
}

void ModPanel::focusOrArm(const Hit& hit) {
#ifdef FEATHERS_DIAGNOSTIC
	// SAID BEFORE ANY BRANCH IS TAKEN, so a crash inside one still leaves the line that names it.
	diagnostic::modPressed(static_cast<int>(hit.part), hit.source.device, hit.source.index, mod_.armed());
#endif
	if (hit.part == Part::None) {
		return;
	}
	if (hit.part == Part::SlotMenu) {
		// NEITHER FOCUSES NOR ARMS, the same as the fold beside it: a hand reaching for the strip's
		// verbs on a card has not said anything about which source it is pointing.
		showSlotMenu(rackSlotOfSource(hit.source));
		return;
	}
	if (hit.part == Part::Fold) {
		// **ONLY ON THE SECOND CLICK** — the head is the whole target now, so a single click here must
		// not fold or every press on a card's title bar would collapse it. Same rule and same reason as
		// `FxCard::foldOnHeadPress`: one gesture, so a double-click toggles once rather than twice.
		if (foldClicks_ < 2) {
			return;
		}
		// NEITHER FOCUSES NOR ARMS, which is the mock's own `e.stopPropagation()` on this button: a
		// hand tidying a card away has not said anything about which macro it is pointing.
		if (setCardMinimised(hit.source, !cardMinimised(hit.source)) && notifier_ != nullptr) {
			// modSourceName RATHER THAN A MACRO TABLE. The one this read had four entries and hit.source is
			// now any source in the strip — an envelope card's arrow arrives here with 12 or above, which
			// read past the end of it. The one naming authority answers for every kind of source.
			std::string message = modSourceName(bridge_, hit.source);
			message += cardMinimised(hit.source) ? " minimised" : " shown";
			notifier_->say(message);
		}
		return;
	}
	if (hit.part == Part::Add) {
		// NEITHER FOCUSES NOR ARMS, for the reason the fold gives above: a hand reaching for a device
		// it does not have yet has said nothing about which source it is pointing.
		showAddMenu();
		return;
	}
	if (hit.part == Part::Collapse) {
		// ANNOUNCED, the way the mock announces it — a panel that leaves the screen without a word is
		// indistinguishable from one that broke. It says what happened rather than what to do next,
		// because the way back is the button the hand is already on.
		if (toggleCollapsed() && notifier_ != nullptr) {
			notifier_->say(collapsed() ? "Modulators minimised" : "Modulators shown");
		}
		return;
	}
	// Either way the card becomes the focused one: arming a source you were not looking at and leaving
	// the rings drawn for another would be the panel disagreeing with itself.
	const bool wasFocused = mod_.source() == hit.source;
	mod_.focus(hit.source);

	if (hit.part == Part::Arrow) {
		// Clicking the arrow of a card that was NOT focused arms it rather than toggling: the hand
		// meant "point this one", and a toggle would have disarmed a session the other card started.
		const bool arm = !(wasFocused && mod_.armed());
		mod_.arm(arm);
		if (notifier_ != nullptr) {
			// modSourceName RATHER THAN A MACRO TABLE. The one this read had four entries and hit.source is
			// now any source in the strip — an envelope card's arrow arrives here with 12 or above, which
			// read past the end of it. The one naming authority answers for every kind of source.
			std::string message = modSourceName(bridge_, hit.source);
			message += arm ? " — drag a highlighted knob to route" : " mapping off";
			notifier_->say(message);
		}
	}
	redraw();
}

ModPanel::CardKind ModPanel::cardKindOf(size_t slot) const {
	// WHAT THE SLOT ACTUALLY HOLDS, asked of the rack rather than remembered. A patch load can put a
	// different device in a slot without the panel being told, and the browser does it on purpose.
	const ModRack rack = bridge_.modRack();

	// A SLOT THE RACK DOES NOT REACH IS DORMANT, NOT UNKNOWN, and the difference is load-bearing.
	// The strip keeps a real card in every slot so the browser turning one on is a VISIBILITY FLIP
	// rather than a construction — the dormant-widget discipline stated at the top of `draw`. An
	// empty slot answering NoEditor would build a card that had to be thrown away the moment a device
	// arrived, which is the thing that discipline exists to avoid.
	if (slot >= rack.count || slot >= rack.device.size()) {
		return CardKind::Dormant;
	}
	if (rack.holds(slot, mod::mseg::kTraits.tag)) {
		return CardKind::Mseg;
	}
	if (rack.holds(slot, mod::dahdsr::kTraits.tag)) {
		return CardKind::Envelope;
	}
	// STRUCTURAL ELSE, THE FX LANE'S RULE: chosen by what the strip can BUILD, not by a list of
	// devices that are missing cards, so it needs no maintenance when one arrives.
	//
	// **A DEVICE THIS BUILD KNOWS GETS THE GENERIC GRID; ONLY AN UNKNOWN ONE GETS `Missing`.** The
	// two used to be the same answer, which is what held the tracker out of the registry: a device in
	// the menu must be controllable in the strip (the honesty rule), and with no generic arm that
	// meant hand-writing a card before registering anything. `GenericCard` draws every control the
	// device declares, so the honesty rule is satisfied structurally rather than per device.
	if (descriptorForRow(rack.device[slot]) != nullptr) {
		return CardKind::Generic;
	}
	// AND A ROW NAMING A DEVICE THIS BUILD DOES NOT HAVE STILL GETS A CARD — "device missing", kept in
	// the record with its settings, rather than a gap.
	return CardKind::Missing;
}

void ModPanel::pruneOutputArms() {
	// **AN ARM THAT OUTLIVES ITS OUTPUT IS A WATCHER LEAK AND A WRONG SOURCE.** `ensureOutputArm` adds
	// lazily; without this, a device removed or replaced left its arms on `ModState`'s watcher list
	// for ever — caught by a case that adds, replaces and removes a device and then asks the list to
	// be the length it started at. Each arm unwatches itself in its own destructor, so dropping it is
	// enough; the rule is whoever registers unregisters, and here the owner is this vector.
	//
	// **AND THE LABEL IS PART OF THE IDENTITY**, because a slot whose device changed keeps its slot
	// number: an arm kept across that swap would name the old device's output beside the new one's
	// arrow.
	//
	// **AND THE SOURCE IS DELIBERATELY NOT ON THIS LIST, WHICH IS THE FIX AND NOT AN OMISSION.** The
	// comment above this loop used to predict its own defect — an arm holds a source index computed
	// from its lane, so one that outlived a chain change would arm the wrong source silently — and
	// then not implement the prediction. It bit: `removeRackDevice` and `moveRackDevice` shift ROWS
	// while a lane travels INSIDE the row, so a slot changes LANE with its number, its output ordinal
	// and its label all unmoved, and two DAHDSRs both call their output "Envelope".
	//
	// THE ANSWER WAS NOT A FOURTH KEY. A lane is an address in a pool that exists so the thing at it
	// can be replaced, so adding it here would have made the cache correct for the removal that was
	// found and left the next lane-mover to find it again. `SourceArm` now ASKS for its source at use
	// (`setSourceProvider`, wired in `ensureOutputArm`), so there is no remembered address for this
	// loop to check.
	const size_t live = deviceCount();
	for (auto it = outputArms_.begin(); it != outputArms_.end();) {
		const bool stale = it->arm == nullptr || it->slot >= live
		                   || it->output >= outputCountOf(it->slot)
		                   || it->label != outputName(it->slot, it->output);
		if (stale) {
			if (it->arm != nullptr) {
				removeChild(it->arm.get());
			}
			it = outputArms_.erase(it);
		}
		else {
			++it;
		}
	}
}

backplane::Source ModPanel::sourceOfOutput(size_t slot, size_t output) const {
	// **THE ONE PLACE A SLOT'S SOURCE IS WORKED OUT, AND IT IS ASKED RATHER THAN CACHED.** A source names
	// a DEVICE (kill-lanes step 2), and which device stands at a slot changes whenever the rack is
	// shortened or reordered — so every reader asks at the moment it needs it; see
	// `SourceArm::source()`, which calls this through the provider wired below.
	//
	// **A SLOT HOLDING NOTHING ANSWERS `dormantOutput`, NOT A HOST SOURCE** — `kNoInstance` is the host's
	// own device number, so `Source::output(kNoInstance, 0)` would be MACRO 1 and a dormant card's
	// geometry would be asked about the macro row. The constant and the whole of that reasoning live
	// once, in `device/endpoint.h`: this and `FxLane::sourceOfOutput` had each spelled it out.
	const DeviceInstanceId device = bridge_.rackDeviceAt(slot);
	return device != kNoInstance ? backplane::Source::output(device, output)
	                             : backplane::Source::dormantOutput(output);
}

SourceArm* ModPanel::ensureOutputArm(size_t slot, size_t output) {
	for (OutputArm& entry : outputArms_) {
		if (entry.slot == slot && entry.output == output && entry.arm != nullptr) {
			return entry.arm.get();
		}
	}
	OutputArm entry;
	entry.slot = slot;
	entry.output = output;
	entry.label = outputName(slot, output);
	entry.arm = std::make_unique<SourceArm>(mod_, sourceOfOutput(slot, output), entry.label);
	// **AND THE WAY TO ASK AGAIN.** The constructor's value is right at this instant and will not stay
	// right: this arm belongs to a SLOT, and which device stands at that slot changes under it. The arm's
	// identity is {slot, output} — its position in the strip — and the source is DERIVED from that at
	// every press and every paint, so a shift moves the answer without anything needing to notice.
	entry.arm->setSourceProvider(
	    [this, slot, output] { return sourceOfOutput(slot, output); });
	// **AND A PRESS ON THE CHIP SELECTS THE CARD IT SITS ON** — see `SourceArm::onPressed` for the
	// defect. The chip is `setOnTop` of the card, so the card's own `onCardPressed` never hears this
	// press; the arm belongs to a SLOT, exactly as the card at that position does, so the position is
	// the right thing to hand `selectSlot`, and it becomes an id the same instant.
	entry.arm->onPressed = [this, slot] { selectSlot(slot); };
	SourceArm* raw = entry.arm.get();
	// **ABOVE THE CARD IT SHARES A RECTANGLE WITH, STATED RATHER THAN INHERITED FROM CALL ORDER.**
	//
	// The card frame covers the whole slot — head, body AND the outputs strip these arms sit in — so
	// something has to be on top, and until now it was only that `resized()` happened to build every
	// card before every arm, so the arms landed later in `children_` and `frameAtPoint` (which walks
	// in reverse) reached them first.
	//
	// **THAT HELD ONLY WHILE A CARD WAS NEVER REBUILT, AND MEASURED THE MOMENT ONE WAS.** Making
	// `ensureCard`'s key notice a lane change (see it) means a removal or a reorder now re-`addChild`s
	// the card, which puts it at the END of the list and therefore ON TOP of its own arrows. Two
	// cases went red at once — "a modulator's arrow arms ITS OWN output after the rack has been
	// shortened or reordered" and its REORDERED twin — with `mod->armed()` false, because the press
	// aimed at the arrow reached the card instead.
	//
	// `setOnTop` puts the arm in `frameAtPoint`'s FIRST pass, so it wins over the card whatever order
	// the two were added in. This is the third time this file's own notes record a card frame
	// swallowing the buttons inside it (`EnvCard`'s first version, then the head marks, now the
	// arrows); an invariant that a future call-order change cannot quietly undo is the answer to it.
	raw->setOnTop(true);
	addChild(raw);
	outputArms_.push_back(std::move(entry));
	return raw;
}

float ModPanel::bodyHeightOf(size_t slot) const {
	// THE CARD ITSELF WHEN THERE IS ONE. `resized` clamps the scroll — which asks contentHeight,
	// which asks this — BEFORE it has built anything, so the kind has to be able to answer alone.
	//
	// **AT `kCardWidth`, NOT AT WHATEVER WIDTH THE CARD HAPPENS TO HAVE.** This read
	// `cards_[slot]->bodyHeight()`, which is `laneHeight()`, which reads `width()` — so a card this
	// pass has just constructed answered about a card zero pixels wide. The strip is a fixed-width
	// rail and this is the only width it has; naming it is what makes the answer independent of when
	// it is asked. Same reasoning as `outputRowsOf` above, which names the same constant for the same
	// reason and says why it may not ask `envCardBounds` for it.
	if (slot < cards_.size() && cards_[slot] != nullptr) {
		return cards_[slot]->laneExtent(kCardWidth);
	}
	switch (cardKindOf(slot)) {
		case CardKind::Mseg:
			return MsegCard::height();
		case CardKind::Missing:
			return MissingDeviceCard::height();
		case CardKind::Generic: {
			// **ASKED OF THE DEVICE, BECAUSE A GENERIC CARD'S HEIGHT IS A PROPERTY OF THE DEVICE.**
			const ModRack rack = bridge_.modRack();
			const device::Descriptor* desc = slot < rack.device.size()
			                                     ? descriptorForRow(rack.device[slot])
			                                     : nullptr;
			// **A DEVICE WITH A CUSTOM STRIP CARD NAMES ITS OWN OPEN HEIGHT** (`card_factory`'s table, backlog NY.1).
			// THE OUTPUTS FOOT AT THIS STRIP'S WIDTH, measured by the labels the chips will print.
			if (desc != nullptr) {
				if (const StripHeightFn height = stripHeightFor(*desc)) {
					return height(outputChipRowsFor(*desc, kCardWidth, outputNamesOf(slot), host_.outputsShown(bridge_.rackDeviceAt(slot))));
				}
			}
			// AT THIS STRIP'S CARD WIDTH, which is what decides how many rows the device's output chips
			// wrap onto and therefore how tall its foot is. Named rather than defaulted: this is the
			// only caller, and the number is the panel's own.
			return desc != nullptr ? GenericCard::heightFor(*desc, kCardWidth)
			                       : MissingDeviceCard::height();
		}
		case CardKind::Dormant:
		case CardKind::Envelope:
			break;
	}
	return EnvCard::height();
}

bool ModPanel::ensureCard(size_t slot) {
	reserveCards(slot + 1);
	const CardKind kind = cardKindOf(slot);
	// THE CACHE KEY IS THE KIND, NOT A TAG WE SUBSTITUTED. It used to be `isMseg ? mseg : dahdsr`,
	// which quietly stamped the ENVELOPE's tag on a slot holding something else — so the cache agreed
	// the wrong card was the right one and never rebuilt it.
	const auto key = static_cast<char>(static_cast<int>(kind) + 1);
	// **EXCEPT FOR A MISSING DEVICE, WHOSE KEY IS ITS OWN TAG** — its card says the tag, so a different
	// missing device in the same place is a different card. (A tag is printable; the kind codes are not,
	// so the two cannot collide.)
	// THE ROW'S OWN TAG, bounds-checked once — what a missing device's card is named by.
	device::Tag rowTag {};
	{
		const ModRack held = bridge_.modRack();
		if (slot < held.device.size()) {
			rowTag = held.device[slot].tag;
		}
	}
	const auto rowTagAt = [&rowTag](size_t) { return std::string_view(rowTag.data(), rowTag.size()); };
	const device::Tag tag = kind == CardKind::Missing ? rowTag : device::Tag { key };
	// **THE DEVICE IS PART OF THE KEY, AND LEAVING IT OUT WAS A REGRESSION FROM BX STEP 4.**
	//
	// A card is addressed by its DEVICE and `cards_` is keyed by POSITION — see the crossing below, and
	// `cardDevice_` for the whole of it. Before step 4 a card held its POSITION and a translating host
	// followed whatever moved into it, so this key was complete. Step 4 moved the address into the card
	// (a lane then, the device's id since kill-lanes step 2) and left the key alone.
	//
	// MEASURED on 84e70bb, three DAHDSRs: remove position 1 and the card still at position 1 went on
	// addressing the device that had been there. Every card below a removal was one row stale, drawing
	// and editing the device that used to be above it. The card holds a perfectly valid address — it is
	// just no longer the device at its position.
	const DeviceInstanceId builtFor = bridge_.rackDeviceAt(slot);
	if (cards_[slot] != nullptr && cardTag_[slot] == tag && cardDevice_[slot] == builtFor) {
		return false;
	}

	// A NEW CARD RATHER THAN A RECONFIGURED ONE. The devices have different controls, different
	// heights and different gestures; a card that tried to become another one would be the generic
	// knob-list this design exists to avoid, wearing a device's name.
	// **RETIRE RATHER THAN DESTROY, AND FREE NOWHERE NEAR HERE.** This read `retiredCards_.clear()`
	// under "free last round's first", which is the fx lane's rule as it stood BEFORE that lane paid
	// for it — see `releaseRetiredCards` for the crash and `FxLane::ensureCard` for the long note.
	//
	// **IT WAS DORMANT HERE AND THE LANE-AWARE KEY ABOVE WOULD HAVE WOKEN IT.** A reorder never
	// rebuilt a card in this panel while the key was the kind alone, so `clear()` never ran with a
	// card's own handler on the stack. It does now: a drag-reorder arrives through
	// `FxCard::noteCardRelease` -> `onCardDrag(End)` -> `moveDevice` -> `resized()`, at least two
	// lanes move, and the second slot's `clear()` would free the card the first retired — which is
	// the card that is dispatching. Fixing the address without moving this would have traded a wrong
	// device for a use-after-free.
	if (cards_[slot] != nullptr) {
		// OFF BOTH REDRAW LISTS, SUBTREE AND ALL — backlog 19, and the same two lines as the fx lane
		// for the same reason. A rack card's value boxes each put themselves on `mod_`'s list
		// (`env_card.cpp:153-155`, `mseg_card.cpp:126-128` — the card AND every box), so this side
		// leaves MORE stale pointers per death than the fx lane does, not fewer.
		// AND THE NULL IN THE MIDDLE IS THE POINT OF THE THIRD PARAMETER. This strip is on NO state
		// channel — it holds no `FrameWatchers*` and never did — and under the old two-argument
		// signature it passed the MODULATION list into the slot `FxLane` used for the STATE one. The
		// same argument meant two different lists at two call sites, and neither said so.
		forgetFrameTree(cards_[slot].get(), &registry_, nullptr, &mod_.watchers());
		removeChild(cards_[slot].get());
		cards_[slot]->retired();
		retiredCards_.push_back(std::move(cards_[slot]));
	}
	// **THE ONE CROSSING, AT THE ONE PLACE THAT HOLDS BOTH.** `cards_` is keyed by rack SLOT — a
	// position, which is what the strip draws and what the tag cache is keyed by — and the host is
	// addressed by the device's ID, like every host (kill-lanes step 2). This is the line where the two
	// meet, and it is deliberately the only one: `FxLane::ensureCard` does exactly this and says the same
	// thing, *"the card knows its device and the strip knows its position"*.
	const DeviceInstanceId device = builtFor;
	switch (kind) {
		case CardKind::Mseg:
			cards_[slot] = std::make_unique<MsegCard>(host_, device, mod_, history_);
			break;
		case CardKind::Generic: {
			const ModRack rack = bridge_.modRack();
			const device::Descriptor* desc = slot < rack.device.size()
			                                     ? descriptorForRow(rack.device[slot])
			                                     : nullptr;
			// **A DEVICE WITH A CUSTOM STRIP CARD REGISTERS IT ITSELF** (`card_factory`'s table, assembled from each
			// device's `device.cmake`; backlog NY.1). These were a chain of one `sameTag` per modulator here, each
			// building the class the factory already builds for the fx lane and the rack; a device the strip does
			// not build from the table (one with no strip height) takes the generic grid below.
			std::unique_ptr<FxCard> custom;
			if (desc != nullptr && stripHeightFor(*desc) != nullptr) {
				custom = makeCustomCard(host_, registry_, device, *desc, notifier_, &mod_, &history_);
			}
			if (custom != nullptr) {
				cards_[slot] = std::move(custom);
			}
			else if (desc != nullptr) {
				// **HANDED THE PANEL'S MODULATION STATE, AS THE CHAIN COLUMN HANDS ITS OWN.** Without it
				// `DeviceControl::display` never reports `routable` (it reads its own `mod_`), so a
				// modulatable knob here — the tracker's, the note expressions' (and MATH's, before its own card) —
				// was drawn as a destination and could not become one: the honesty rule's failure.
				auto card = std::make_unique<GenericCard>(host_, device, *desc, notifier_);
				card->setModulation(&mod_, &history_);
				cards_[slot] = std::move(card);
			}
			else {
				cards_[slot] = std::make_unique<MissingDeviceCard>(rowTagAt(slot));
			}
			break;
		}
		case CardKind::Missing:
			cards_[slot] = std::make_unique<MissingDeviceCard>(rowTagAt(slot));
			break;
		case CardKind::Dormant:
		case CardKind::Envelope:
			cards_[slot] = std::make_unique<EnvCard>(host_, device, mod_, history_);
			break;
	}
	cardTag_[slot] = tag;
	// BOTH HALVES OF THE KEY, WRITTEN TOGETHER — see `cardDevice_`. Recording the device anywhere other
	// than beside the tag is how the two come apart again.
	cardDevice_[slot] = device;
	addChild(cards_[slot].get());
	return true;
}

size_t ModPanel::syncCards() {
	reserveCards(bridge_.chainState().length(kStripChain));
	// EVERY SLOT HOLDS A CARD HERE (the dormant-widget discipline), so `ensureCard`'s own key is the whole
	// question and its answer is whether it built. See `FxLane::syncCards` for the load this is about.
	size_t rebuilt = 0;
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		rebuilt += ensureCard(slot) ? 1u : 0u;
		// **AND A FOLD THE PATCH MOVED UNDER A CARD IT KEPT.** A load that holds the same devices keeps
		// every card, and the fold is the bridge's (`cardMinimised`), applied only by `resized()` — so a
		// fold-only load left the old heights until something laid the strip out.
		rebuilt += cards_[slot]->folded() != cardMinimised(sourceOfOutput(slot, 0)) ? 1u : 0u;
	}
	if (rebuilt > 0) {
		resized();
		redraw();
	}
	return rebuilt;
}


void ModPanel::disarmIfArmedSourceIsGone() {
	// **AN ARMED SOURCE WHOSE DEVICE HAS LEFT IS A GESTURE THE EDITOR OFFERS AND CANNOT PERFORM.**
	// `ModState::source_` is never revalidated against liveness, so removing the modulator that is
	// currently armed left the strip saying ARMED with its source naming a device nothing holds.
	//
	// IT WAS NEVER A WRONG WRITE — the routing authority refuses a source with no device behind it, so
	// the knobs reported `mappable() == false` and a drag wrote nothing. What it left was an INERT
	// ARMED STATE, which is the honesty rule's failure case wearing the opposite sign: a control lit
	// for a verb that does nothing.
	//
	// **DISARM RATHER THAN RE-POINT.** Falling the focus back to macro 0, or to the nearest surviving
	// device source, ARMS SOMETHING THE HAND DID NOT CHOOSE and the next click routes to it.
	// `ModState` has no "no source" state by construction — the rings must be about something — but
	// ARMED is a separate flag, so the source is left where it was, harmlessly, and nothing is armed
	// until a hand arms it.
	//
	// ONLY A DEVICE SOURCE CAN GO AWAY. A macro and a per-sound weight are always live, so they are
	// not asked about — which is also what keeps this from disarming the world on every layout.
	if (!mod_.armed() || !mod_.source().isDevice()) {
		return;
	}
	// LIVE MEANS THE DEVICE THE ENDPOINT NAMES IS HELD AND DECLARES THAT OUTPUT — the record's answer.
	if (backplane::SourceView({}, bridge_.devices()).isLive(mod_.source())) {
		return;
	}
	mod_.arm(false);
}

void ModPanel::collapsedChanged() {
	Rail::collapsedChanged();
	tips_.clear();
	tips_.setVisible(false);
}

void ModPanel::resized() {
	// **THE ARMING IS CHECKED BEFORE ANYTHING IS LAID OUT**, because every road that can remove a
	// device — the head menu, an undo, a patch arriving — ends in a layout, and none of them shares
	// any other line with the others. Putting it on the road they all reach is what makes it cover
	// them all rather than covering the one that was being debugged.
	disarmIfArmedSourceIsGone();

	// A rail that got taller has room for more of the strip, and a scroll left where it was would hold
	// content off the top of a frame that no longer needs it held anywhere. Clamped here rather than
	// through setScroll, which calls this.
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	for (size_t macro = 0; macro < slots_.size(); ++macro) {
		const visage::Bounds column = macroColumnBounds(macro);
		// SIZED WHETHER SHOWN OR NOT, the discipline the Explore width knobs already follow: a
		// zero-size child frame is one setVisible away from painting at the window origin.
		slots_[macro]->setBounds(column.x(), column.y(), column.width(),
		                         MacroSlot::columnHeight(kMacroDiameter));
		// The macro row is permanent — it is the mod-IN, not a device — so only the rail's own
		// collapse can hide it. A collapse mid-rename abandons the edit and lets a held momentary go:
		// the slot does both when it is hidden.
		slots_[macro]->setVisible(!collapsed());
	}
	for (size_t slot = 0; slot < cards_.size(); ++slot) {
		const visage::Bounds card = envCardBounds(slot);
		// BELOW THE HEAD ROW *AND* THE OUTPUTS ROW, both of which the strip owns and draws. A card
		// frame that covered either would sit on top of the buttons in it and swallow every press
		// aimed at them — they would be drawn, look right, and do nothing.
		//
		// THAT IS NOT HYPOTHETICAL: adding the outputs row to `sourceHeight` without adding it here
		// left the body starting at the old offset, so the card covered the arrows it had just made
		// room for, and the arming case went red with the arrow present and unpressable.
		// THE CARD'S OWN HEIGHT, not a constant named after one device. The MSEG's editor is taller
		// than the envelope's curve because a breakpoint needs room to be grabbed in.
		ensureCard(slot);
		// **A MINIMISED CARD IS FOLDED, NOT HIDDEN — backlog CZ.** This strip hid a minimised card
		// (`setVisible(false)` below) from the day the card frame took over drawing the head, so a
		// folded modulator was a BLANK BAND: no head, no name, no square, nothing to double-click but
		// a rectangle `hitAt` still answered for. MEASURED on the installed f33e3d3 with a real
		// pointer. The card already knows how to be its head alone — `FxCard::setFolded`, the fx
		// lane's fold — so the strip tells it, keeps it visible, and sizes it to what it now reports.
		// Told BEFORE the extent is read, or the bounds would be last frame's.
		const backplane::Source headSource = sourceOfOutput(slot, 0);
		cards_[slot]->setFolded(cardMinimised(headSource));
		// **THE WHOLE RECTANGLE, because the card draws its own frame now.** It used to be positioned
		// beneath a head this panel drew, which is exactly why it had no frame anywhere else — see
		// `ui::drawCardFrame`. Its own geometry measures from `bodyTop()`.
		//
		// **AT THE WIDTH BEING SET, THROUGH `laneExtent(cross)` — AND `laneHeight()` HERE WAS A REAL
		// DEFECT, NOT A STYLE.** Both arguments of a call are evaluated before it, so a `laneHeight()`
		// in this position asked a card its height at the width it had BEFORE this line — zero for a
		// card `ensureCard` had just constructed. `GenericCard`'s height ends in
		// `outputStripHeightAt(width())`, the chip-wrap walk cannot wrap in a strip of zero width, and
		// so a device with seven outputs was given a ONE-ROW foot: 146px where it needed 190, with the
		// last four chips laid into a strip `envCardBounds` derived at the real width and therefore
		// 44px below the card's own bottom edge. PHOTOGRAPHED on xvfb — two rows of chips drawn, the
		// third clipped to a sliver at the border.
		//
		// **THIS IS THE SECOND INSTANCE ON THE SAME TRAP AND THE OTHER TWO HOSTS WERE ALREADY FIXED.**
		// `FxLane::slotTop` and `RackEditor` both ask `laneExtent(cross)`; this strip was the one
		// caller in `src/` still asking the width-blind form. `FxCard::outputRowsAt` names the trap in
		// writing — *"answering it from `width()` would read the width the card had LAST layout — zero
		// on the first"* — and the fix that wrote that sentence reached `laneExtent` and stopped.
		cards_[slot]->setBounds(card.x(), card.y(), card.width(),
		                        cards_[slot]->laneExtent(card.width()));
		// THIS HOST OFFERS BOTH MARKS. The rack offers neither, and it is the same card class — which
		// is exactly why the card is told rather than left to infer.
		cards_[slot]->setHeadMarks({ true, cardMinimised(headSource) });
		// **AND WHAT THE MARKS DO, because the card is drawn over them and therefore takes the press.**
		// `partAt` still knows these rectangles for the strip's own hit test, but a press on the head
		// never reaches it now — a child frame gets it first, which is the bug `EnvCard`'s note records
		// from the first version whose frame covered its head.
		// **THE CARD'S DRAG, WIRED TO THE PANEL'S REORDER — it never was.**
		//
		// The defect: collapsing mod cards worked, dragging them did not. The panel has had a complete
		// reorder road all along — `dragSlot_`, `dropGapAt`, `rowForGap`, `moveDevice` — armed from its
		// OWN `onMouseDown`. But a press on an OPEN card goes to the CARD: `frameAtPoint` answers the
		// card frame, so the panel's handler never runs and `dragSlot_` is never set. The road was
		// reachable only on the strip's bare background.
		//
		// The fx lane and the rack both bind `onCardDrag` for exactly this reason. This is the third
		// host and it was the one that did not.
		cards_[slot]->onCardDrag = [this, slot](ui::FxCard::DragPhase phase, visage::Point inCard) {
			// THE CARD REPORTS IN ITS OWN COORDINATES and only the panel knows where the card sits —
			// the same conversion the fx lane makes, and the reason the card cannot do it itself.
			const visage::Bounds box = sourceBounds(sourceOfOutput(slot, 0));
			const visage::Point inPanel { box.x() + inCard.x, box.y() + inCard.y };
			switch (phase) {
			case ui::FxCard::DragPhase::Begin:
				dragSlot_ = slot;
				dragAnchorY_ = inPanel.y;
				dragging_ = false;
				break;
			case ui::FxCard::DragPhase::Move:
				if (!dragSlot_.has_value()) {
					return;
				}
				dragging_ = true;
				dropGap_ = dropGapAt(inPanel);
				redraw();
				break;
			case ui::FxCard::DragPhase::Cancel:
				dragSlot_.reset();
				dropGap_.reset();
				dragAnchorY_.reset();
				dragging_ = false;
				redraw();
				break;
			case ui::FxCard::DragPhase::End: {
				const std::optional<size_t> from = dragSlot_;
				const std::optional<size_t> gap = dropGap_;
				const bool moved = dragging_;
				dragSlot_.reset();
				dropGap_.reset();
				dragAnchorY_.reset();
				dragging_ = false;
				if (moved && from.has_value() && gap.has_value()) {
					dropCard(*from, *gap); // the block when the card is in a selection of several (backlog PN)
				}
				redraw();
				break;
			}
			}
		};

		cards_[slot]->onHeadFold = [this, headSource] {
			// NEITHER FOCUSES NOR ARMS, the mock's `stopPropagation` on this button: a hand tidying a
			// card away has not said anything about which source it is pointing.
			if (setCardMinimised(headSource, !cardMinimised(headSource)) && notifier_ != nullptr) {
				std::string message = modSourceName(bridge_, headSource);
				message += cardMinimised(headSource) ? " minimised" : " shown";
				notifier_->say(message);
			}
		};
		cards_[slot]->onHeadMenu = [this, headSource] {
			showSlotMenu(rackSlotOfSource(headSource));
		};
		// **THE PRESS, BOUND AT LAST — see `selectSlot`.** Fired for any press the card sees and for a
		// press on one of its controls that moved the keyboard focus up to it; `slot` is the POSITION
		// this card is the face of, which is exactly what a press means, and the panel turns it into
		// an id the same instant.
		// AND THE SELECTION'S TWO ROADS (backlog PN, `LaneSelection`'s banner): any press on the card, a knob's
		// included, touches; the card's own press then applies the modifier the hand held; a release that did not drag
		// collapses a block to the card. The id is resolved at the press, from the position this card is the face of.
		cards_[slot]->onCardPressed = [this, slot] {
			if (const DeviceInstanceId id = instanceAtSlot(slot); id != kNoInstance) {
				selection_->focusPress(id);
			}
		};
		cards_[slot]->onCardSelectPress = [this, slot](CardSelection::Press kind) {
			if (const DeviceInstanceId id = instanceAtSlot(slot); id != kNoInstance) {
				selection_->cardPress(id, kind);
			}
		};
		cards_[slot]->onCardClicked = [this, slot] { selection_->cardClick(instanceAtSlot(slot)); };
		// AND THE WINDOW ROADS, asked of the card at press time by its own device — the lane's binding,
		// for the same reason: a card rebuilt for another device must open and raise THAT device's.
		cards_[slot]->onExpand = [this, card = cards_[slot].get()] {
			if (onExpandDevice) {
				onExpandDevice(card->device());
			}
		};
		cards_[slot]->onHeadPressed = [this, card = cards_[slot].get()] {
			if (onFocusDeviceWindow) {
				(void)onFocusDeviceWindow(card->device(), card->nest());
			}
		};

		// STALE ARMS GO FIRST — see `pruneOutputArms`. It sat in the CONSTRUCTOR by mistake, where it
		// had nothing to prune and never ran again.
		pruneOutputArms();

		// THE ARMS FOR THIS DEVICE'S OUTPUTS, IN THE STRIP THE FRAME RESERVES ALONG THE BOTTOM.
		// Rebuilt when the device changes under a slot — an arm asks its source of the slot (see
		// `sourceOfOutput`), so one kept across a chain change arms the device now standing there.
		// NO FRAME COMPUTED HERE — `outputChipBounds` works it out from the rack's state, and this loop
		// only places the widget into what it returns. A second `cardFrameOf` call here would be the
		// same duplication one layer down.
		const size_t outputs = outputCountOf(slot);
		for (size_t out = 0; out < outputs; ++out) {
			SourceArm* arm = ensureOutputArm(slot, out);
			if (arm == nullptr) {
				continue;
			}
			// INTO THE RECTANGLE THE GEOMETRY ALREADY DECIDED — see `outputChipBounds`. Placing it from
			// its own `preferredWidth` here would be a second layout of one thing.
			const visage::Bounds at =
			    outputChipBounds(sourceOfOutput(slot, out));
			arm->setBounds(at.x(), at.y(), at.width(), at.height());
			arm->setVisible(!collapsed() && slot < deviceCount() && at.width() > 0.0f);
		}
		// A CARD IS SHOWN WHEN THE RAIL IS OPEN AND THE RACK HOLDS IT — and a FOLDED card is shown,
		// as its head, which is the whole of backlog CZ: "and it is not folded" used to be the third
		// condition here, and it hid the head with the body.
		cards_[slot]->setVisible(!collapsed() && slot < deviceCount());
	}
	// AFTER EVERY CARD IS THE FACE OF ITS CURRENT DEVICE, tell each whether it is the selected one.
	// A removal, a reorder, an undo and a patch load all end here, and all of them can move the
	// selected device to another position or take it away — so this is the one line that keeps the
	// outline on the device rather than on the slot.
	// THE SELECTION'S OVERLAY OVER EVERY CARD (backlog PN) — a card built above was added after it; it ignores the
	// mouse, so re-adding it cannot drop a press being delivered. And what the strip no longer holds is pruned.
	if (selectionOverlay_ != nullptr) {
		// NEVER `removeChild` A FRAME THAT IS NOT A CHILD: visage erases `find()`'s answer unchecked, and for an absent
		// child that is `end()` — the first layout, before the overlay was ever added, crashed every teardown after.
		const int at = indexOfChild(selectionOverlay_.get());
		if (at < 0) {
			addChild(selectionOverlay_.get());
		}
		else if (at != static_cast<int>(children().size()) - 1) {
			removeChild(selectionOverlay_.get());
			addChild(selectionOverlay_.get());
		}
		selectionOverlay_->setBounds(0, 0, width(), height());
	}
	selection_->prune();
	applySelection();
}

void ModPanel::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurfaceRaised);
	canvas.rectangle(0.0f, 0.0f, width(), height());

	// CLOSED IS A WHOLE DRAWING, not the open one with pieces skipped: the label goes upright, the
	// cards are not drawn at all, and what is left is the strip and the way back out of it.
	if (collapsed()) {
		drawRailChrome(canvas);
		return;
	}

	const visage::Font& heading = font(kControlSize);
	canvas.setColor(kMuted);
	drawTracked(canvas, "MODULATORS", heading, kControlTracking, kPadX, kPadTop - scroll_,
	            kHeadingHeight);
	drawRailChrome(canvas);

	// THE MACRO HEADER ROW. No card behind it, deliberately — a card is the unit of "a device the
	// rack holds", and these are parameters the host can automate. The row draws a rule under itself
	// instead, which is a divider rather than a container.
	const visage::Bounds row = macroRowBounds();
	for (size_t macro = 0; macro < kMacroCount; ++macro) {
		const visage::Bounds column = macroColumnBounds(macro);
		const bool focused = mod_.source() == macroSourceAt(macro);
		const bool armed = focused && mod_.armed();

		// The focused macro gets a faint ground behind its column, which is what a card's border used
		// to say. Faint because the row is furniture: four lit boxes would read as four buttons.
		if (focused) {
			canvas.setColor(withAlpha(armed ? kModulation : kAccent, 0.16f));
			canvas.roundedRectangle(column.x() + 1.0f, column.y(), column.width() - 2.0f,
			                        column.height(), kCornerRadius);
		}

		// THE ARROW, drawn rather than set: a chevron and a stem, in the modulation colour when it is
		// armed and in the muted one when it is not, so the strip states its own mode without a word.
		// A16's lesson — this font has no `➤` and a missing glyph is an invisible control.
		const visage::Bounds arrow = arrowBounds(macroSourceAt(macro));
		const float cx = arrow.x() + arrow.width() * 0.5f;
		const float cy = arrow.y() + arrow.height() * 0.5f;
		canvas.setColor(armed ? kModulation : (focused ? kAccent : kMuted));
		drawArmArrow(canvas, cx, cy, kArrowSize);
	}
	// The divider under the row: this is where the mod-IN ends and the devices begin, and the two
	// tiers should not read as one list.
	canvas.setColor(withAlpha(kLine, 0.9f));
	canvas.rectangle(row.x(), row.bottom() + kCardGap * 0.5f, row.width(), 1.0f);

	// WHAT IS LEFT FOR THE STRIP: THE OUTPUTS ROW AND THE FOLD.
	//
	// **THE FRAME AND THE HEAD ARE THE CARD'S OWN NOW.** This loop drew the surface, the border, the
	// dot and the name for every card in it — which is precisely why those cards had no frame in any
	// other host, and why one dropped into the rack's lane arrived bare. `ui::drawCardFrame` is the
	// single drawer and the card calls it, so a card is the same object wherever it is shown rather
	// than two drawings that agree. What remains here are the STRIP's affordances rather than the
	// device's: the chips that arm its outputs, and the fold.
	for (size_t slot = 0; slot < deviceCount(); ++slot) {
		// **THE OUTPUT ARMS ARE WIDGETS NOW AND ARE PLACED IN `resized`, NOT PAINTED HERE.** This loop
		// drew a bordered chip and an arrow per output and hit-tested rectangles of its own; the rack
		// built `SourceArm` frames for the same verb. Two implementations of one thing, and the
		// painted one lost the moment a card drew its own frame over it. See `ui::SourceArm`.

		// **THE FOLD AND THE SLOT MENU ARE DRAWN BY THE FRAME NOW, NOT HERE.** A card is a CHILD frame,
		// so this panel draws BENEATH it — and the moment the card's own frame covered the head, both
		// marks vanished under it. They are passed to the card as `CardHeadMarks` and drawn by
		// `ui::drawCardFrame`; the hit tests below still belong to the strip, and read the same
		// rectangles from `ui::cardFoldBounds` / `ui::cardMenuBounds` so a press and a mark cannot
		// disagree about where they are.

	}
	// THE MOCK'S CV CARD, and it is a placeholder there too — *"wakes when the host routes audio
	// here"*. Drawn dimmed and answering nothing, which is the honest state of a source that has no
	// port behind it yet: the alternative is a live-looking control that does nothing, which is the one
	// thing the honesty rule refuses.
	// THE BUMPER, if a card is being dragged — where it would land, drawn by the STRIP because a gap
	// belongs to the list rather than to either card beside it.
	if (dragging_ && dropGap_.has_value()) {
		const visage::Bounds bumper = gapBounds(*dropGap_);
		if (bumper.height() > 0.0f) {
			canvas.setColor(kValue);
			canvas.roundedRectangle(bumper.x(), bumper.y(), bumper.width(), bumper.height(), 1.0f);
		}
	}

	// THE (+), drawn as an outline rather than as a filled card, because it is a verb and not a thing
	// in the rack. The row it sits in is where the next device will appear, which is what makes the
	// button a picture of what pressing it does.
	//
	// DIMMED WHEN THE RACK IS FULL, and still drawn: a control that vanished at the bound would leave
	// a player wondering where it went, and the menu behind it still says what the devices are.
	const visage::Bounds add = addBounds();
	const bool rackFull = false; // the strip grows (unbounded devices step 6)
	canvas.setColor(withAlpha(kLine, rackFull ? 0.35f : 0.75f));
	canvas.roundedRectangleBorder(add.x(), add.y(), add.width(), add.height(), kCornerRadius, 1.0f);
	canvas.setColor(rackFull ? kDisabled : kValue);
	// GEOMETRY RATHER THAN A GLYPH — A16's lesson, the same one the macro arrows are drawn by: this
	// font has no guarantee of a "+" that centres, and a missing glyph is an invisible control.
	const float plus = 7.0f;
	const float cx = add.x() + kCardPadX + plus * 0.5f;
	const float cy = add.y() + add.height() * 0.5f;
	canvas.segment(cx - plus * 0.5f, cy, cx + plus * 0.5f, cy, 1.4f, true);
	canvas.segment(cx, cy - plus * 0.5f, cx, cy + plus * 0.5f, 1.4f, true);
	canvas.text(rackFull ? "Rack full" : "Add modulator", font(kKnobReadoutSize),
	            visage::Font::kLeft, cx + plus, add.y(), add.width(), add.height());

	const visage::Bounds cv = cvCardBounds();
	canvas.setColor(kSurface);
	canvas.roundedRectangle(cv.x(), cv.y(), cv.width(), cv.height(), kCornerRadius);
	canvas.setColor(withAlpha(kLine, 0.6f));
	canvas.roundedRectangleBorder(cv.x(), cv.y(), cv.width(), cv.height(), kCornerRadius, 1.0f);
	canvas.setColor(kDisabled);
	canvas.circle(cv.x() + kCardPadX, cv.y() + kHeadHeight * 0.5f - kDotDiameter * 0.5f, kDotDiameter);
	canvas.text("CV In", font(kControlSize), visage::Font::kLeft,
	            cv.x() + kCardPadX + kDotDiameter + 7.0f, cv.y(), cv.width(), kHeadHeight);
	canvas.text("2 lanes · waiting on a cable", font(kKnobReadoutSize), visage::Font::kLeft,
	            cv.x() + kCardPadX, cv.y() + kHeadHeight, cv.width(), 18.0f);

	// The scroll indicator, on the sound rail's terms: only while there is something to scroll, because
	// a track that is always drawn is furniture rather than information.
	if (maxScroll() > 0.0f) {
		const float thumbHeight = std::max(24.0f, height() * (height() / contentHeight()));
		const float thumbY = (height() - thumbHeight) * (scroll_ / maxScroll());
		canvas.setColor(withAlpha(kLine, 0.9f));
		canvas.roundedRectangle(width() - 4.0f, thumbY, 2.0f, thumbHeight, 1.0f);
	}
}

} // namespace feathers::ui

namespace feathers::ui {

void ModPanel::reserveCards(size_t count) {
	if (count <= cards_.size()) {
		return;
	}
	const size_t want = std::max(count, 2 * cards_.size());
	cards_.resize(want);
	cardTag_.resize(want);
	cardDevice_.resize(want);
}

} // namespace feathers::ui
