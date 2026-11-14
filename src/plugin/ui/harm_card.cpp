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

#include "plugin/ui/harm_card.h"

#include "plugin/ui/expanded_views.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace feathers::ui {

namespace {

namespace hrm = feathers::fx::harm;


} // namespace

HarmCard::HarmCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), notifier_(notifier), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — it was keyed by clap_id and a device's
	// controls have none. The PARAMETER stays in the signature because removing it touches every
	// card, `fx_lane` and the fixtures; that pass is pending. Discarded explicitly so a deferred
	// cleanup cannot be mistaken for a forgotten use.
	(void)registry;
	const auto addKnob = [&](hrm::Param param, const char* label) {
		auto knob = std::make_unique<Knob>(control(param), label, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		// Recorded as the knob is built, in the one place that knows which control it was built for —
		// FilterCard's rule, and for its reason: a Knob addresses its value through Modulatable and
		// does not publish which control that was.
		knobParams_.push_back(param);
	};

	// ONE ROW OF THREE, FROM THE DECLARED LIST. The split by design: only Level, Harmonic Attack and
	// Release on the card — and Harmonic is the ROW above rather than a knob, so what is left is three.
	//
	// IT WENT NINE -> THREE AND THE CARD IS NOT MERELY SHORTER. The nine-knob card needed five cells
	// across, which made them 43px for a 38px knob; three knobs in the same width are ~68px, so the
	// crowding that was reported is FIXED BY THE REDUCTION rather than traded against it.
	//
	// THE OTHER SIX KNOBS AND THE POLY SWITCH ARE NOT GONE, they are in `HarmExpanded` behind the
	// head's expand button. The reachability invariant is what holds that to account: every control
	// HARM declares must be offered by at least one surface, swept from `kParamDescs`.
	for (const CardKnob& knob : kCardKnobs) {
		addKnob(knob.param, knob.label);
	}

	// THE HARMONIC ROW, SHARED WITH THE EXPANDED VIEW. It was four rectangles and a 102-entry popup
	// on this card, which is why the panel could not have it; extracting it closed a real gap — the
	// panel PLOTS the harmonic and a hand could not move it from there.
	row_ = std::make_unique<HarmonicRow>(control(hrm::Param::Harmonic));
	// THE CARD'S HEAD SHOWS THE RATIO WHILE FOLDED, so the card repaints when the row moves. The row
	// does not know that and should not: it is the host that knows what else is looking.
	row_->onChanged = [this] { redraw(); };
	addChild(row_.get());

	// NOTCH OR HIGH-PASS (backlog NL), beside the row whose harmonic it acts on.
	mode_ = std::make_unique<SwitchChip>(control(hrm::Param::Mode), "Mode");
	addChild(mode_.get());

	// THE FOUR PARTIALS' ACTIVE SWITCHES. Two states each, so a chip each; named by the partial's number.
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		auto chip = std::make_unique<SwitchChip>(control(hrm::partialParam(k, hrm::PartialField::Active)), std::to_string(k + 1));
		addChild(chip.get());
		activeChips_[static_cast<size_t>(k)] = std::move(chip);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// A PRESS MAY BECOME A DRAG — recorded before the parts are dispatched, because the part that
		// wins here (fold, on the head) is the one a drag has to be able to override.
		noteCardPress(e);
		switch (partAt(e.position)) {
		case Part::Fold:
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			break;
		case Part::Expand:
			// **ALREADY ACTED ON**, by `noteCardPress` above — which is where every card's expand press
			// now lands. Firing again here would open the panel twice. The PART survives because
			// `partAt` is the seam two cases use to ask what a coordinate means, and that question is
			// still worth answering.
			break;
		case Part::Enable:
			toggleEnabled();
			break;
		case Part::SlotMenu:
			pressSlotMenu();
			break;
		case Part::None:
			break;
		}
	};
}

DeviceControl& HarmCard::control(hrm::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		// THE GENERIC ROAD — a descriptor and a control index, never a device type. HARM has no clap
		// param ids and never will, so there is no host-parameter spelling of this to fall back to.
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, hrm::kDescriptor, index);
	}
	return *controls_[index];
}

float HarmCard::openHeight() {
	// DERIVED FROM THE DECLARED LIST, so a knob added back moves the card instead of overflowing it —
	// and so the height and the layout cannot disagree, which is the seam that has bitten twice this
	// week in this very lane.
	const float column = Knob::columnHeight(kKnobDiameter);
	const size_t rows = (kCardKnobCount + static_cast<size_t>(kRowCells) - 1)
	                    / static_cast<size_t>(kRowCells);
	const float knobRows = static_cast<float>(rows) * column
	                       + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	return kHeadHeight + kRowGap + kHarmonicRowHeight + kRowGap + knobRows + kActiveRowGap + SwitchChip::kHeight + kPad;
}

void HarmCard::foldedChanged() {
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
	// AND THE HARMONIC ROW, WHICH IS A CHILD FRAME. `knobs_` stopped being everything this card holds
	// the day the row became a widget both surfaces build, and this line did not follow it: a folded
	// HARM left its row visible and pressable below a 24px head, over whatever card the lane had moved
	// up into that space. FilterCard's fold hides its switches for exactly this reason.
	if (row_ != nullptr) {
		row_->setVisible(!folded_);
	}
	if (mode_ != nullptr) {
		mode_->setVisible(!folded_);
	}
	// AND THE FOUR ACTIVE CHIPS — a folded card is its head and nothing below it.
	for (auto& chip : activeChips_) {
		if (chip != nullptr) {
			chip->setVisible(!folded_);
		}
	}
	// AND THE LANE IS TOLD, because a fold changes this card's HEIGHT and the lane places cards by
	// asking for it. redraw() alone repaints the card at the size it already had — which is the whole
	// of the defect: the UI went away but the card size did not change.
}

void HarmCard::setModulation(ModState* state, EditHistory* history) {
	for (auto& knob : knobs_) {
		knob->setModulation(state, history);
	}
	// POLY IS NOT A DESTINATION and is not offered one — the device declares it unmodulatable because
	// it decides how many of the device there are rather than what one of them does.
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS.
	//
	// THIS LINE WAS MISSING AND HARM COULD NOT BE MODULATED AT ALL. A Knob forwards to the control it
	// was given, so the knobs' adapters were covered — but `DeviceControl::setAmount` and
	// `DeviceControl::display` both ask the adapter's OWN `mod_`, and every one of these was built by
	// `control()` with none. Ten declared destinations, `isModDestination()` true on all of them, and
	// `mappable()` false forever: the arrow armed, the knobs stayed dark, and nothing said why.
	//
	// `FilterCard::setModulation` HAS DONE BOTH SINCE IT WAS WRITTEN and its comment says exactly why
	// — *"an adapter that never learned the panel's state reports itself unarmed forever. Cheap,
	// total, and it cannot fall behind a widget list."* The rule was written down in the sibling file
	// and not followed here, which is the shape this repo keeps paying for: a comment stating a rule
	// is not the rule being followed.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<hrm::Param>(i)).setModulation(state, history);
	}
}

visage::Bounds HarmCard::headBounds() const {
	return ownFrame().head;
}


visage::Bounds HarmCard::harmonicBounds() const {
	// SHORT OF THE MODE CHIP, which takes the right end of the line.
	return {bodyLeft() + kPad, bodyTopEdge() + kRowGap,
	        std::max(0.0f, bodyWidth() - 2.0f * kPad - kModeChipWidth - kRowGap), kHarmonicRowHeight };
}

visage::Bounds HarmCard::modeBounds() const {
	const visage::Bounds row = harmonicBounds();
	return {row.right() + kRowGap, row.y() + (row.height() - SwitchChip::kHeight) * 0.5f, kModeChipWidth,
	        SwitchChip::kHeight};
}


std::string HarmCard::harmonicText() const {
	// ASKED OF THE ROW rather than recomputed. Two derivations of one label is two places for the
	// card's folded head and its open row to start disagreeing about what the device is set to.
	return row_ != nullptr ? row_->label() : std::string {};
}

std::string HarmCard::ratioText() const {
	return row_ != nullptr ? row_->ratioText() : std::string {};
}

std::vector<fx::harm::Param> HarmCard::addressedParamsForTest() const {
	// THE KNOBS ANSWER FOR THEMSELVES, from the list recorded as each was built, and the two non-knob
	// controls are named here because they are drawn as bespoke parts. There is no third place a
	// control could hide: everything this card puts a hand on is a knob or the arrow pair.
	std::vector<fx::harm::Param> out = knobParams_;
	out.push_back(hrm::Param::Enabled);  // the square in the head
	out.push_back(hrm::Param::Harmonic); // the two arrows and the ratio box between them
	out.push_back(hrm::Param::Mode);     // the Notch / High-pass chip beside them
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		out.push_back(hrm::partialParam(k, hrm::PartialField::Active)); // the row of four chips under the knobs
	}
	// POLY IS NOT HERE ANY MORE — it moved to `HarmExpanded` with the six knobs. Claiming it would
	// make the reachability invariant pass on a control no hand can reach from this card, which is
	// the one failure that invariant exists to prevent.
	return out;
}

device::Modulatable* HarmCard::enableModulatable() {
	// BY INDEX, from the descriptor — the same answer today and the one that keeps working for a
	// device whose enable is not its first control.
	return hasEnable() ? &control(static_cast<hrm::Param>(enableControl())) : nullptr;
}

void HarmCard::toggleEnabled() {
	// THE TOGGLE, THE UNDO BRACKETING AND THE REDRAW ARE THE BASE'S — FxCard::toggleEnable. This
	// card reaches it from a `Part` switch rather than a position, which is why it calls that rather
	// than `enablePressAt`: the hit-test already decided.
	if (const auto nowOn = toggleEnable()) {
		if (notifier_ != nullptr) {
			notifier_->say(*nowOn ? "Harm on" : "Harm off");
		}
	}
}

HarmCard::Part HarmCard::partAt(visage::Point position) const {
	// THE DOTS AND THE SWITCH BEFORE THE HEAD THEY SIT IN, because the head's own verb is FOLD and a
	// hit test in the other order makes both unclickable — FilterCard's enable square records the
	// same trap, and this card now has the same two occupants.
	if (slotMenuAt(position)) {
		return Part::SlotMenu;
	}
	if (enableBounds().contains(position)) {
		return Part::Enable;
	}
	if (expandBounds().contains(position)) {
		return Part::Expand;
	}
	if (headBounds().contains(position)) {
		return Part::Fold;
	}
	// AND NOTHING BELOW THE HEAD. The harmonic row is a CHILD FRAME now, so its presses are delivered
	// to it directly and never arrive here — which is the point of extracting it, and is why the
	// three harmonic parts left this enum rather than being kept as forwarding cases.
	return Part::None;
}

void HarmCard::resized() {
	layOut();
}

void HarmCard::layOut() {
	const visage::Bounds row = harmonicBounds();
	if (row_ != nullptr) {
		row_->setBounds(row.x(), row.y(), row.width(), row.height());
	}
	if (mode_ != nullptr) {
		const visage::Bounds mode = modeBounds();
		mode_->setBounds(mode.x(), mode.y(), mode.width(), mode.height());
	}

	const float column = Knob::columnHeight(kKnobDiameter);
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float cell = inner / static_cast<float>(kRowCells);
	const float top = row.bottom() + kRowGap;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		if (activeChips_[static_cast<size_t>(k)] != nullptr) {
			const visage::Bounds b = activeBounds(k);
			activeChips_[static_cast<size_t>(k)]->setBounds(b.x(), b.y(), b.width(), b.height());
		}
	}

	// COUNTED, NOT ARITHMETIC ON LITERALS — and this is a CORRECTION to the reduction that preceded
	// it. That commit's message said "there is no arithmetic left to break"; the constructor and
	// `openHeight` had been converted and THIS HAD NOT. It read `place(0, 4, rowA); place(4, 5, rowB)`
	// and every case stayed green, because three knobs fit inside the first call and the second
	// placed nothing.
	//
	// THAT IS THE SHAPE WORTH REMEMBERING: dead arithmetic does not fail, it just stops being
	// exercised. A fourth knob returning to this card would have been placed on a second row the
	// height does not reserve, and the first thing to go wrong would have been a control drawn off
	// the bottom of the card.
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const size_t line = i / static_cast<size_t>(kRowCells);
		const size_t at = i % static_cast<size_t>(kRowCells);
		knobs_[i]->setBounds(bodyLeft() + kPad + cell * static_cast<float>(at),
		                     top + static_cast<float>(line) * (column + kRowGap), cell, column);
	}
}

visage::Bounds HarmCard::activeBounds(int partial) const {
	// UNDER THE KNOB ROW, four equal chips across the body.
	const float column = Knob::columnHeight(kKnobDiameter);
	const size_t rows = (kCardKnobCount + static_cast<size_t>(kRowCells) - 1) / static_cast<size_t>(kRowCells);
	const float knobRows = static_cast<float>(rows) * column + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	const float top = harmonicBounds().bottom() + kRowGap + knobRows + kActiveRowGap;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float gap = kRowGap;
	const float each = (inner - gap * static_cast<float>(hrm::kPartialCount - 1)) / static_cast<float>(hrm::kPartialCount);
	return { bodyLeft() + kPad + (each + gap) * static_cast<float>(partial), top, std::max(0.0f, each), SwitchChip::kHeight };
}

bool HarmCard::updateContent() {
	if (!isVisible() || folded()) {
		return false; // `redraw()` is refused for a frame that is not drawn; a true would be a lie.
	}
	// **READ THE FIELD, DO NOT ASK FOR A CONTROL.** `control()` LAZILY CONSTRUCTS a `DeviceControl`,
	// and a tick is the wrong moment to be building widgets: the lane keeps DORMANT cards for slots
	// that hold some other device, so an update that reached for a control would construct one against
	// a slot whose fields belong to a neighbour — and then repaint a card that should not be drawn.
	//
	// MEASURED: adding this update with `control()` in it turned `gui-windowed`'s OWLTT-popout case red
	// — 659,295 pixels differing on a case that has nothing to do with HARM. Reading the host's field
	// directly has no side effect and answers the same number.
	const double harmonic = host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Harmonic));
	const bool enabled = host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Enabled)) >= 0.5f;
	if (haveDrawnHarm_ && harmonic == drawnHarmonic_ && enabled == drawnEnabled_) {
		return false;
	}
	drawnHarmonic_ = harmonic;
	drawnEnabled_ = enabled;
	haveDrawnHarm_ = true;
	// THE ROW AND THE CARD: the row draws the bands, the card draws the head that greys with the
	// switch. Both move on this control, and only the row moves on the harmonic.
	if (row_ != nullptr) {
		row_->redraw();
	}
	redraw();
	return true;
}

void HarmCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void HarmCard::drawContent(visage::Canvas& canvas) {
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.

	// WHETHER THIS DEVICE IS IN THE SIGNAL — one control, the same one every other effect has.
	//
	// THIS ASKED TWO CONTROLS UNTIL A DECISION CHANGED IT, and the old predicate is worth remembering rather
	// than just deleting: it dimmed on `Level > 0 || Width > 0`, because HARM declared no switch and
	// drawing one the device did not have is what the honesty rule forbids. That reading was retired
	// as a hardware-specific convention (the 'level = 0 means off' semantics) — so the DEVICE grew the switch and the card asks it.
	//
	// LEVEL AND WIDTH STILL MEAN WHAT THEY MEANT. An enabled HARM with both down does not touch a
	// sample; that is an identity guarantee about its controls, asserted to the bit in
	// tests/test_fx_harm.cpp, and it is a different claim from "the device is off".
	const bool on = control(hrm::Param::Enabled).value() >= 0.5;
	for (auto& knob : knobs_) {
		knob->setActive(on);
	}

	// THE DIMMING HELPER WENT WITH THE HARMONIC ROW. Everything on this card that receded while the
	// device was switched out now recedes on its own: the knobs through `setActive`, the row through
	// its own copy of the same rule. What is left here is head chrome, which never dimmed.
	// THE LANE'S VERBS ON THIS SLOT — drawn whether the card is folded or open, because removing a
	// device you have folded away is exactly when you want to.
	// THE ONE THING THAT NEVER DIMS, the filter card's rule: everything else on a switched-out card
	// can afford to recede because none of it is doing anything, and this square is the only way
	// back. Filled when on, outlined when off — the same two states, in the same place.
	const visage::Bounds sw = enableBounds();
	canvas.setColor(on ? kAccent : kMuted);
	if (on) {
		canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
	}

	// THE WAY INTO THE EXPANDED VIEW: a diagonal with an arrowhead at each end, pushing apart.
	//
	// IT WAS FOUR CORNER BRACKETS AND THE RENDER KILLED THAT. On a 12px box with a 2px inset the
	// edges are 8px and the arms were 4px, so each pair MET — the mark drew as a closed square, two
	// pixels from the enable square, one outlined and one filled and meaning entirely different
	// things. Every geometry assertion passed; it was legible only as the wrong control. This
	// editor's own rule, from the rails and the fold and the slot menu: A DIFFERENT VERB GETS A
	// DIFFERENT KIND OF MARK, not a variation on somebody else's.
	//
	// GEOMETRY AND NOT A GLYPH, the standing rule: this font has no guaranteed expand character and
	// a missing glyph is an invisible control.

	if (folded_) {
		// FOLDED IS A WHOLE DRAWING: the head, and the one fact that says what this HARM is set to.
		// CLEAR OF THE HEAD'S BUTTONS, same as the filter's and for the same reason — worse here,
		// because this ran to the full padding and so crossed the slot menu as well as the switch.
		//
		// AND THE LEFTMOST IS NOW THE EXPAND BUTTON, NOT THE SQUARE. `foldedLabelBounds` takes the
		// leftmost occupant precisely so a new one moves the text; handing it the square while the
		// expand button sits further left would have put the ratio under the button on the first
		// frame — which is the bug reported on the filter, reintroduced by the fix's own
		// mechanism going unused.
		canvas.setColor(kDisabled);
		// THE LEFTMOST HEAD OCCUPANT, which is what the folded text must stop short of.
		const visage::Bounds ex = expandBounds();
		const visage::Bounds label =
		    foldedLabelBounds(ex.width() > 0.0f ? ex.x() : enableBounds().x());
		canvas.text(ratioText(), font(kKnobReadoutSize), visage::Font::kRight, label.x(), label.y(),
		            label.width(), label.height());
		return;
	}

	// THE HARMONIC ROW IS A CHILD FRAME AND DRAWS ITSELF — the box, the chevron, the two steppers
	// and their dimming at the ends of the table all moved to `HarmonicRow` so the expanded view
	// could have them too. Nothing about that drawing was generic; what made it worth sharing is
	// that three separate pieces of hard-won behaviour live on it, and two copies is two places for
	// one of them to stop being true.
	//
	// IT IS TOLD WHETHER THE DEVICE IS ON, because a row has no way to reach its device's enable and
	// a switched-out card recedes as a whole.
	if (row_ != nullptr) {
		row_->setActive(on);
	}

	// POLY IS DRAWN BY `HarmExpanded` NOW, and its caption and its dead/live reasoning went with it
	// rather than being deleted. The switch was never the card's to keep once the card was three
	// knobs — it is the control furthest from "what this HARM is set to at a glance".
}

} // namespace feathers::ui
