/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/shaper_card.h"
#include "plugin/ui/shaper_colour_cell.h"

#include "plugin/ui/expanded_views.h"
#include "plugin/ui/shaper_plot.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <vector>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace shp = fx::shaper;

namespace {

/// THE KNOBS, IN THE ORDER THEY ARE LAID OUT. One table, read by the constructor and by the layout,
/// which is what stops two functions disagreeing about which knob is which.
constexpr std::array<shp::Param, ShaperCard::kKnobCells> kCardKnobs {
	shp::Param::Knee,
	shp::Param::Colour,
	shp::Param::Drive,
	shp::Param::Threshold,
	// **THE SECOND ROW, BENEATH THRESHOLD** (once Mix), by design: the
	// crossfade's width under the control that decides where the crossfade starts.
	shp::Param::Aggression,
	// **AND THE TRIM, BESIDE IT UNDER DRIVE.** The second
	// row is the level row now — what goes in, and how abruptly the shaped path takes over — under the
	// two controls of the row above that decide the same things.
	shp::Param::Trim,
	// **AND THE INPUT FILTERS, LEFT OF THE TRIM (backlog FA)** — the row reads as the input, in signal
	// order: what is filtered out, how hard it goes in, how abruptly the curve takes it. The two columns
	// were empty; the crossfade Mix lives in the window, where the row has room for it.
	shp::Param::InputHpf,
	shp::Param::InputLpf,
};

} // namespace

ShaperCard::ShaperCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                       Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	// THE REGISTRY IS TAKEN AND NOT KEPT, which is what `Knob`'s device-control constructor already
	// did and said: *"a device's control is not a host parameter, so there is no clap_id for the
	// registry to key on; whoever built the device repaints its widgets through the same ModState this
	// knob is later handed."* This card's knobs are all on that road, so a stored reference was only
	// ever passed along to be ignored — and once `Knob` stopped taking it, storing it here became
	// provably dead. Kept in the SIGNATURE so every call site reads alike and a card can move to the
	// parameter road without its surroundings changing.
	//
	// NOT A MISSING CALL, which is the other thing an init-only member usually means and is the
	// dangerous reading: the registry is the parameter->widget REPAINT road, so a card that had meant
	// to subscribe and forgotten would go stale under automation. Ruled out by reading the
	// constructor that took it — it never called `watch()` and said so.
	(void)registry;
	controls_.resize(static_cast<size_t>(shp::Param::kCount));

	for (const shp::Param p : kCardKnobs) {
		const char* label = shp::kParamDescs[static_cast<size_t>(p)].name;
		// THE LABEL IS THE CONTROL'S OWN NAME AND THE ZONE IS ITS VALUE — the retired `ZoneKnob` had
		// bug that comes of conflating them (every zone knob captioned with zone zero's name, frozen
		// and therefore wrong the moment it was turned). The device qualifies its names, so what is
		// passed is the unqualified tail: the card's head already says which device this is.
		const char* shortLabel = label;
		if (std::string_view(label).rfind("Shaper ", 0) == 0) {
			shortLabel = label + 7;
		}
		if (p == shp::Param::Colour) {
			// **COLOUR'S MODE CELL (backlog IW).** The dropdown is a mode selector, not a phase region
			// selector. The dropdown lists Colour's modes — Inflate, then Free — and the box is the live mode's
			// value: the unbounded field in Free, the Inflate amount in Inflate. One builder for card and panel.
			colourCell_ = makeColourModeCell([this](shp::Param q) -> device::Modulatable& { return control(q); },
			                                 kKnobDiameter);
			addChild(colourCell_.get());
			cells_.push_back(colourCell_.get());
			// NOT INTO `knobParams_` (1:1 with `knobs_`, read positionally). The cell reaches all three: the mode,
			// the field and the amount.
			coordParams_.push_back(shp::Param::Colour);
			coordParams_.push_back(shp::Param::ColourMode);
			coordParams_.push_back(shp::Param::InflateAmount);
			continue;
		}
		auto knob = std::make_unique<Knob>(control(p), shortLabel, notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		cells_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		// RECORDED AS THE KNOB IS BUILT, in the one place that knows which control it was built for.
		knobParams_.push_back(p);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A PRESS THAT IS NOT ON A VERB MAY BECOME A CARD DRAG — the lane's reorder gesture. Noted
		// first, and the four verbs below each return, so a press that does something does not also
		// pick the card up.
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		// THE TOGGLE, THE GESTURE AND THE REDRAW LIVE IN THE BASE — see FxCard::enablePressAt.
		// This card keeps only its own voice, which the base holds no notifier to speak with.
		if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Shaper on" : "Shaper off");
			}
			return;
		}
		// TESTED BEFORE THE HEAD, and that order IS load-bearing: the button sits inside the head's
		// rectangle, so a head tested first swallows it and the bug reads as "the button does
		// nothing". An empty rectangle contains no point, so a build that cannot construct the panel
		// falls through to the fold without a second branch asking whether there is one.
		if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			return;
		}
	};
	// THE SIX EXTRAS ARE FADER PILLS (backlog IK) — each an amount AND its power, with the knob's whole
	// modulation surface; the same widget the panel places, on the same controls in the same order. As
	// children they take their own presses, so `FxCard`'s drag and release are untouched by them.
	for (size_t i = 0; i < shp::kExtraSwitchCount; ++i) {
		auto pill = std::make_unique<FaderPill>(control(shp::kExtraSwitches[i]), control(shp::kExtraPowers[i]),
		                                        shp::shortLabelOf(shp::kExtraSwitches[i]));
		pill->setAnnouncer([this](const std::string& line) {
			if (notifier_ != nullptr) {
				notifier_->say(line);
			}
		});
		addChild(pill.get());
		pills_.push_back(std::move(pill));
	}
	poly_ = std::make_unique<SwitchChip>(control(shp::Param::Poly), "Poly");
	addChild(poly_.get());
}

device::Modulatable* ShaperCard::enableModulatable() {
	return hasEnable() ? &control(static_cast<shp::Param>(enableControl())) : nullptr;
}

DeviceControl& ShaperCard::control(shp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] =
		    std::make_unique<DeviceControl>(host_, device_, shp::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

bool ShaperCard::updateContent() {
	// EXACT EQUALITY RATHER THAN A TOLERANCE, deliberately: the question is "is the screen showing
	// these settings", which is a bitwise question. A tolerance would be a rate limiter wearing a
	// correctness argument — `AutomodStrip::update` makes the same argument at more length.
	//
	// AND NOTHING WHILE INVISIBLE: `redraw()` is refused for a frame that is not being drawn, so a
	// `true` returned here would be a frame TOLD and not a frame DRAWN, which is what the tick counts.
	if (!isVisible()) {
		return false;
	}
	// COLOUR'S MODE CELL SHOWS THE LIVE MODE'S VALUE (backlog IW) — a mode change swaps which box it shows.
	const bool modeSwapped = colourCell_ != nullptr && colourCell_->refresh();
	const shp::Settings now = settings();
	// **AND THE DRIVE METER, WHICH IS NOT PART OF `Settings` AND SO WAS NOT WATCHED AT ALL.**
	//
	// The meter did not update properly and froze unless a control changed. Exactly that: this tick returned early whenever the SETTINGS
	// were unchanged, and a meter is not a setting — it is a reading the engine produces. So the card
	// redrew only when a parameter happened to force one, and the meter froze at whatever it held.
	//
	// **A READING NEEDS ITS OWN DIRTY TEST BECAUSE IT IS NOT STATE.** `Settings` is what a patch
	// stores; the peak is what the engine is doing this instant. `drawnSettings_` can never see it,
	// and any future reading drawn on this card — a playhead, a gain reduction — lands in the same
	// hole unless it is added here too.
	//
	// THE EPSILON IS A PIXEL, NOT A TOLERANCE: the meter colours a curve about 200 px wide, so a
	// change under 1/400 of full scale cannot move the boundary and a redraw for it would be work
	// nobody can see. It is deliberately far below the fall rate, so a falling meter redraws every
	// tick — which is what a falling meter is supposed to do.
	float drive = 0.0f;
	const bool metered = host_.drivePeakAt(device_, drive);
	const bool driveMoved = metered != haveDrive_ || std::fabs(drive - drawnDrive_) > 0.0025f;
	if (haveDrawn_ && now == drawnSettings_ && !driveMoved && !modeSwapped) {
		return false;
	}
	drawnDrive_ = drive;
	haveDrive_ = metered;
	drawnSettings_ = now;
	// **AND THE PANEL LEARNS WHETHER IT REACHES THE SOUND, HERE, because this is the one place that
	// knows the settings CHANGED.** Setting it in `drawContent` would recompute it every frame for an
	// answer that moves only when Knee crosses its threshold; setting it in the constructor would
	// freeze it at whatever the patch loaded with.
	showInSignal(!now.linear());
	haveDrawn_ = true;
	redraw();
	return true;
}

shp::Settings ShaperCard::settings() const {
	// THE ROW, READ THROUGH THE SEAM. This was `fxChain()` and then reaching into
	// `chain.device[device_].row` — which is `DeviceHost::fieldsPresentAt` and `fieldAt` spelled the long
	// way round, through a type only the parameter road has. Reading it through the seam is what lets
	// the RACK build this card: the rack holds a `DeviceHost` and no bridge, and that one fact is why
	// every custom UI was unavailable there.
	//
	// SAME NUMBERS, NOT MERELY SIMILAR ONES: `BridgeDeviceHost::fieldAt` reads the same `DeviceRow`
	// this used to walk, so Feathers' behaviour is unchanged by construction rather than by testing.
	shp::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	shp::Settings s = shp::stateOf(row, present);
	// **AND THE FOLDED TRAVEL, WHICH `stateOf` CANNOT SEE.** It takes the params-only `Row` by design,
	// so the eight floats past the params are invisible to it — and a surface that could not see them
	// drew the curve the coordinates alone imply while the device played a different one. That is the
	// same shape as the drive meter freezing and as `sameCurveAs` ignoring travel: a reading that does
	// not reach the surface. Read through the same seam, so the rack gets it too.
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		const size_t at = shp::kTravelFirst + i;
		s.travel[i] = at < host_.fieldsPresentAt(device_) ? host_.fieldAt(device_, at) : 0.0f;
	}
	return s;
}

visage::Bounds ShaperCard::headBounds() const {
	return ownFrame().head;
}

visage::Bounds ShaperCard::plotBounds() const {
	return {bodyLeft() + kPad, kSlotHeadHeight, bodyWidth() - 2.0f * kPad, kPlotHeight};
}


static_assert(static_cast<float>(ShaperCard::kExtrasPerRow) * ShaperCard::kExtraRows
                  >= static_cast<float>(fx::shaper::kExtraSwitchCount),
              "the pill rows hold every extra switch");

float ShaperCard::openHeight() {
	// THE TALLEST IN THE ROW, which is the one carrying a phase box. See the note where Colour is
	// built: +23px against a plain knob row, and the row's other four knobs are unmoved because
	// `Knob::draw` anchors to the TOP of its bounds.
	// BACK TO A PLAIN KNOB COLUMN. The +23px this card grew for a phase box is returned: the
	// coordinate box that replaced the assembly IS a knob column, row for row — see
	// `ZoneCoordCell::setFaceDiameter`. The card is 230px again.
	// AND THE EXTRAS ROW, added by design: toggles go on the card if there is room, otherwise on the
	// panel. MEASURED before it was built rather than after — the card is one unit at 336px and the
	// content above this reaches 225, so a pill row at `kExtraHeight` 20 + `kRowGap` 8 spends 28 of
	// 111 spare. Hysteresis was measured too and stayed on the panel: as its own row it would have
	// left 8px, which is inside the slop between the derived figure and a capture, and would have
	// closed the card to anything else.
	// **AND THE SECOND KNOB ROW.** MEASURED before it was built, as the extras row was: the card is one
	// lane unit at 336px and this sum reaches 253 without it, so a second row at `secondRowHeight()`
	// (67 + 8) takes it to 328 and leaves 8. That is the SAME 8px this file already records being
	// declined for Hysteresis — "inside the slop between the derived figure and a capture" — so the
	// card is now closed to anything else, and the layout case asserts every cell lands inside it
	// rather than trusting the arithmetic.
	// **AND THE PILLS IN TWO ROWS, NOT ONE.** Six pills and Poly in one row left each pill 25px at the lane's width, and
	// six captions ("Feedback", "Integrator") were cut to fragments — the README's card mosaic showed it, and the
	// label-fit sweep in test_gui_widgets_devices now asks it of every card. Three to a row fits them.
	return kSlotHeadHeight + kPlotHeight + kRowGap + Knob::columnHeight(kKnobDiameter) + kRowGap
	       + secondRowHeight() + kExtraRows * kExtraHeight + (kExtraRows - 1.0f) * kExtraGap + kPad;
}

visage::Bounds ShaperCard::extraBounds(size_t i) const {
	if (i >= shp::kExtraSwitchCount) {
		return {};
	}
	// UNDER THE KNOB ROW, and the top is derived from the same terms `openHeight` sums rather than
	// from a number typed twice — `shaper_expanded.cpp` states the rule these two must both obey: a
	// layout and a height that disagree leave a row drawn off the bottom of the card.
	const float top = kSlotHeadHeight + kPlotHeight + kRowGap + Knob::columnHeight(kKnobDiameter)
	                  + kRowGap + secondRowHeight();

	// **FITTED TO THE CARD, NOT ASSUMED.** This laid the five switches out at a fixed `kExtraWidth`
	// whatever the card was, and in the rack the card is 236px wide while the row needs
	// `kPad + 5*(46+4) - 4 + kPad` = 266 — so the fifth switch was drawn 20px past the right edge and
	// clipped in half, as a screenshot showed.
	//
	// **NOTHING CHECKED CONTAINMENT, WHICH IS WHY IT SHIPPED.** Every geometry case this card has asks
	// about the row's INTERNAL consistency — laid out, not overlapping, inside the height budget. A row
	// can be internally perfect and still hang off its parent, and that is a different question that
	// nobody was asking. The case beside this one asks it now, for every child.
	//
	// THE FIXED WIDTH IS A CEILING RATHER THAN A SIZE: a card with room to spare keeps the switches at
	// the size they were designed at, and a narrow one shrinks them instead of losing one.
	const auto perRow = static_cast<float>(kExtrasPerRow);
	const size_t row = i / kExtrasPerRow;
	const size_t col = i % kExtrasPerRow;
	// **FROM THE BODY, NOT THE CARD** — `bodyLeft()` and `bodyWidth()`, as the plot and the knob rows are.
	// This measured from the card's left edge, so in the rack, whose head is a spine down the LEFT, the
	// pills sat on the spine: painted over it while they were drawings, and — once they became widgets
	// (backlog IK) — swallowing the presses meant for its marks. `test_rack_editor` caught it the day they
	// became children; it had been wrong since the extras reached the card.
	// THE POLY CHIP TAKES THE ROW'S RIGHT END (backlog OM), so the pills share what is left.
	// THE LAST ROW CARRIES POLY, so a row's pills share what Poly leaves of it; the first row keeps the same cells.
	const float usable =
	    std::max(bodyWidth() - 2.0f * kPad - (perRow - 1.0f) * kExtraGap - kExtraGap - kPolyWidth, perRow);
	const float cell = std::min(kExtraWidth, usable / perRow);
	return {bodyLeft() + kPad + static_cast<float>(col) * (cell + kExtraGap),
	        top + static_cast<float>(row) * (kExtraHeight + kExtraGap), cell, kExtraHeight};
}

visage::Bounds ShaperCard::polyBounds() const {
	const visage::Bounds last = extraBounds(shp::kExtraSwitchCount - 1);
	const float x = bodyLeft() + bodyWidth() - kPad - kPolyWidth;
	return {std::max(x, last.right() + kExtraGap), last.y() + (kExtraHeight - SwitchChip::kHeight) * 0.5f, kPolyWidth,
	        SwitchChip::kHeight};
}

float ShaperCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

void ShaperCard::foldedChanged() {
	// **EVERY CELL, NOT EVERY KNOB — and this loop said `knobs_` until the zoned control stopped
	// being one.** A folded card left its Colour coordinate box drawn over whatever the lane had put
	// underneath it: a control a hand can still reach on a card that is supposed to be closed. Caught
	// by `EVERY card in the lane folds to its head when the head is pressed`, which counts what is
	// VISIBLE below the head rather than trusting a list.
	//
	// So the walk is over `cells_`, which is every widget this card built regardless of kind. A list
	// that has to be remembered separately is a list that will be forgotten again the next time a
	// widget kind arrives.
	for (visage::Frame* cell : cells_) {
		// A CONTROL DRAWN OUTSIDE THE RECTANGLE ITS CARD NOW OCCUPIES is a control a hand can still
		// reach, over whatever the lane put underneath it.
		cell->setVisible(!folded_);
	}
	// AND THE SIX FADER PILLS — their own list, because `cells_` is indexed by layout position and a pill
	// has no knob position. The fold test counts what is VISIBLE under the head, so a pill forgotten here
	// goes red there rather than hovering over the next card.
	for (auto& pill : pills_) {
		pill->setVisible(!folded_);
	}
	poly_->setVisible(!folded_);
	// AND THE LANE IS TOLD, because a fold changes this card's HEIGHT and the lane places cards by
	// asking for it. `redraw()` alone repaints the card at the size it already had, which is the whole
	// of the defect where the UI went away but the card size did not change.
}

void ShaperCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the enable switch is edited by a gesture
	// rather than by a widget, and an adapter that never learned the panel's state reports itself
	// unarmed forever. FilterCard's rule: cheap, total, and it cannot fall behind a widget list.
	for (size_t i = 0; i < static_cast<size_t>(shp::Param::kCount); ++i) {
		control(static_cast<shp::Param>(i)).setModulation(mod, history);
	}
}


void ShaperCard::showInSignal(bool live) {
	//---------------------------------------------------------------------------------------------//
	// THE HONESTY RULE AT KNEE 0, WHERE THE DEVICE IS A TRUE BYPASS.                               //
	//---------------------------------------------------------------------------------------------//
	//
	// `Shaper::processSample` returns `clamp(x, -1, 1)` and consults NO table when `Settings::linear`
	// — so at Knee 0 it is not only Drive that does nothing: Colour, Gamma, Mix, Harmonics, the
	// coordinate and all five extras are equally unreachable. The backlog item asked for Drive alone;
	// reading the engine says the honest scope is everything but Knee. The rule is "a control is
	// enabled iff it changes sound through a real param", and it does not have a Drive clause.
	//
	// **KNEE ITSELF STAYS LIT, AND THAT IS THE SAME ARGUMENT `FxCard` MAKES ABOUT THE HEAD**: it does
	// not dim the head because *"the switch that turns the device back on lives there, and greying the
	// way back out is how a control becomes hard to find at the moment it matters."* Knee IS the way
	// back out of the bypass. A card that greyed it would grey the only control that can change
	// anything.
	//
	// **`setActive`, NOT `setEnabled`** — FilterCard's distinction and its reasoning applies unchanged:
	// grey says "not in the signal", not "hands off". Dialling in a Colour before turning Knee up is a
	// thing a player should be able to do, and the host still saves what they dial.
	for (size_t i = 0; i < knobs_.size() && i < knobParams_.size(); ++i) {
		// TRIM AND THE INPUT FILTERS STAY LIT TOO (backlog FA): they run ahead of the bypass, so at Knee 0 they
		// still change the sound — see `shp::actsAtKneeZero`.
		knobs_[i]->setActive(live || shp::actsAtKneeZero(knobParams_[i]));
	}
	// THE COORDINATE BOX SPELLS IT `setEnabled`, and it means the same thing this does — its flag only
	// recolours, its `ValueBox` children still take the hand. See `ZoneCoordCell::setEnabled`.
	for (auto& coord : coords_) {
		coord->setEnabled(live);
	}
	// COLOUR'S MODE CELL GREYS WITH THE REST (backlog IW) — the coordinate box it replaced did.
	if (colourCell_ != nullptr) {
		colourCell_->setEnabled(live);
	}
	for (auto& pill : pills_) {
		pill->setActive(live);
	}
	inSignal_ = live;
}
void ShaperCard::resized() { layOut(); }
void ShaperCard::layOut() {
	const float usable = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float even = usable / static_cast<float>(kTopRowCells);
	const float top = kSlotHeadHeight + kPlotHeight + kRowGap;
	const float column = Knob::columnHeight(kKnobDiameter);

	//---------------------------------------------------------------------------------------------//
	// AN EVEN ROW IS THE WRONG ROW when one cell holds twice what the others do.                    //
	//---------------------------------------------------------------------------------------------//
	//
	// **THIS IS THE CLIPPED COLOUR READOUT A SCREENSHOT SHOWED.** Four equal cells gave the
	// coordinate box the same 43px a knob gets, and a coordinate spends 23 of that on chrome before
	// a digit is drawn — leaving **10px per half against the 15px `"127"` measures**. See
	// `ZoneCoordCell::minimumWidth`, and `test_rack_editor.cpp` for the case that reproduces it.
	//
	// **THE KNOBS PAY ONLY WHAT THEY CAN SPARE, WHICH IS DOWN TO THEIR FACE AND NOT A PIXEL FURTHER.**
	// A knob cell narrower than `kKnobDiameter` does not squeeze the dial, it CROPS it — trading a
	// clipped number for a clipped circle is not a fix. So the transfer is bounded by `spare`, and
	// where the row cannot cover what the coordinate asks for, the coordinate gets everything
	// available and is still short. MEASURED on the rack card: 172px of row against a want of
	// 69 + 3 x 38 = 183, so it lands 11px over-committed and the widest signed phase still crops.
	// That residue is a CARD-WIDTH question — a smaller face on this lane, or one fewer control in
	// the row — and it is a design decision, not something to bury in a layout constant.
	std::vector<float> widths(cells_.size(), even);
	float wanted = 0.0f;
	float spare = 0.0f;
	// A WIDE CELL — a coordinate or, since backlog IW, Colour's mode cell — answers its minimum; a knob answers -1.
	const auto wideMinimum = [](const visage::Frame* cell) {
		if (const auto* box = dynamic_cast<const ZoneCoordCell*>(cell)) {
			return box->minimumWidth();
		}
		if (const auto* mode = dynamic_cast<const ModeValueCell*>(cell)) {
			return mode->minimumWidth();
		}
		return -1.0f;
	};
	// **THE TOP ROW ONLY** — it is the row that must sum to `usable`; the second row borrows its columns. Counting
	// the second row's knobs as spare (they were, until backlog FA) made the top-row knobs give up less than the
	// Colour cell took, and with four of them under it the row ran 4px past the rack's narrow card.
	for (size_t i = 0; i < cells_.size() && i < kTopRowCells; ++i) {
		visage::Frame* cell = cells_[i];
		if (const float min = wideMinimum(cell); min >= 0.0f) {
			wanted += std::max(0.0f, min - even);
		} else {
			spare += std::max(0.0f, even - kKnobDiameter);
		}
	}

	// **AND WHAT THE KNOBS CANNOT GIVE, THE PADDING DOES, up to one pad's width (backlog FA).** On the rack's
	// 216px card three knobs at their face diameter leave the Colour cell 10px short of its readout. The row
	// used to cover that by counting the second row's knobs as spare too, which silently ran the top row into
	// the right-hand pad; with four knobs under it that ran past the card. Now it is said: the row widens into
	// both pads equally, by no more than one pad in all, and only when the knobs are already at their faces.
	float fromPad = 0.0f;
	if (wanted > 0.0f) {
		const float fromKnobs = std::min(wanted, spare);
		fromPad = std::min(wanted - fromKnobs, kPad);
		const float moved = fromKnobs + fromPad;
		for (size_t i = 0; i < cells_.size() && i < kTopRowCells; ++i) {
			if (const float min = wideMinimum(cells_[i]); min >= 0.0f) {
				const float share = std::max(0.0f, min - even) / wanted;
				widths[i] = even + moved * share;
			} else if (spare > 0.0f) {
				const float share = std::max(0.0f, even - kKnobDiameter) / spare;
				widths[i] = even - fromKnobs * share;
			}
		}
	}

	// LAID BY RUNNING TOTAL rather than by index x cell, so the row's total width is exactly `usable`
	// however the shares fall out — an index walk over unequal cells leaves a gap or an overlap.
	// **THE TOP ROW BY RUNNING TOTAL, THE SECOND AT MIX'S OWN x.** The running total is what makes the
	// row exactly `usable` wide however the unequal shares fall out; the second row cannot join that
	// walk, so it takes the left edge the walk gave Mix — recorded as it passes rather than recomputed,
	// because the shares above are not a function anyone can invert.
	// **THE TOP ROW BY RUNNING TOTAL, THE SECOND UNDER THE COLUMNS IT BELONGS TO.** The running total
	// is what makes the row exactly `usable` wide however the unequal shares fall out; the second row
	// cannot join that walk, so each of its cells takes the left edge the walk gave the column it sits
	// under — recorded as the walk passes rather than recomputed, because the shares above are not a
	// function anyone can invert.
	float x = bodyLeft() + kPad - 0.5f * fromPad;
	std::array<float, kTopRowCells> columnLeft {};
	std::array<float, kTopRowCells> columnWidth {};
	for (size_t i = 0; i < cells_.size() && i < kTopRowCells; ++i) {
		columnLeft[i] = x;
		columnWidth[i] = widths[i];
		cells_[i]->setBounds(x, top, widths[i], column);
		x += widths[i];
	}
	// IN DECLARATION ORDER, each under the column `kSecondRowColumns` names — see `kCardKnobs`. The pairing
	// with a column is by index here and by name there, and the two must agree, which the layout case
	// asserts by asking where each one landed.
	for (size_t i = kTopRowCells; i < cells_.size(); ++i) {
		const size_t which = i - kTopRowCells;
		if (which >= kSecondRowColumns.size()) {
			continue;
		}
		const size_t under = kSecondRowColumns[which];
		cells_[i]->setBounds(columnLeft[under], top + column + kRowGap, columnWidth[under], column);
	}
	// THE SIX FADER PILLS, where the pills were drawn before they were widgets.
	for (size_t i = 0; i < pills_.size(); ++i) {
		pills_[i]->setBounds(extraBounds(i));
	}
	poly_->setBounds(polyBounds());
}

std::string ShaperCard::foldedSummaryForTest() const {
	const shp::Settings s = settings();
	if (s.linear()) {
		// THE HONEST WORD FOR KNEE 0. The device is in the chain and is doing nothing to the signal,
		// and a folded head reading a colour zone name would be naming a character nobody can hear.
		return "Bypass";
	}
	// THE MODE, NOT A ZONE (backlog IW): Colour's zones were labels over a continuum; its dropdown names modes now.
	return shp::kColourModeNames[std::min<uint32_t>(s.colourMode, shp::kColourFree)];
}

bool ShaperCard::headEnabled() const {
	return !hasEnable() || control(static_cast<fx::shaper::Param>(enableControl())).value() >= 0.5;
}

void ShaperCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void ShaperCard::drawContent(visage::Canvas& canvas) {
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.

	const shp::Settings s = settings();

	if (folded_) {
		// FOLDED IS A WHOLE DRAWING, not a cut-off version of the open one: the head, and the one fact
		// that says what this Shaper sounds like — which zone of the colour field it is standing in.
		canvas.setColor(kMuted);
		// THE LEFTMOST HEAD OCCUPANT, which is what the folded text must stop short of.
		const visage::Bounds ex = expandBounds();
		const float right = ex.width() > 0.0f ? ex.x() : enableBounds().x();
		canvas.text(foldedSummaryForTest(), font(kControlSize), visage::Font::kRight, 0.0f, 0.0f,
		            std::max(right - 8.0f, 0.0f), kSlotHeadHeight);
		return;
	}

	// THE CURVE. It is the whole picture of a waveshaper and it is drawn through the same
	// `buildTable` the engine builds its own table with — see shaper_plot.h for why that sharing is
	// not an optimisation but the only way the plot can be trusted.
	// **HOW HARD THE TABLE IS ACTUALLY BEING DRIVEN**, asked of the host rather than computed here —
	// it is a READING the engine produces, and a card cannot derive it from the field row. The host
	// declines when nobody publishes, and `drawShaperCurve` then draws one colour: an unmetered device
	// must not read as an unreached one.
	float drive = 0.0f;
	if (!host_.drivePeakAt(device_, drive)) {
		drive = 0.0f;
	}
	drawShaperCurve(canvas, plotBounds(), s, false, drive);


}

} // namespace feathers::ui
