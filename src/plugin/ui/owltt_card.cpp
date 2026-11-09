/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/owltt_card.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/diagnostic.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace feathers::ui {

namespace {

namespace owltt = feathers::fx::owltt;

/// The crossover type names, indexed by mode (1-10, no "Off" - Enabled handles bypass).
const char* modeName(int32_t mode) {
	switch (mode) {
	case 1: return "AP 6dB";
	case 2: return "Quirky";
	case 3: return "Twisted";
	case 4: return "Weird";
	case 5: return "LR2 Fast";
	case 6: return "LR2";
	case 7: return "LR4 Fast";
	case 8: return "LR4";
	case 9: return "Inverted";
	case 10: return "Twist3";
	default: return "?";
	}
}















/// Parameter indices for each band's threshold and bandwidth.

} // namespace

OwlttCard::OwlttCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), notifier_(notifier), device_(id) {
	// REGISTRY UNUSED SINCE `Knob` STOPPED TAKING ONE — it was keyed by clap_id and this card's
	// controls are a device's, which have none. The PARAMETER is still in the signature because
	// removing it touches every card, `fx_lane` and the fixtures; that pass is pending. Discarded
	// explicitly so the compiler cannot mistake a deferred cleanup for a forgotten use.
	(void)registry;
	const auto addKnob = [&](size_t paramIndex, const char* label) {
		auto knob = std::make_unique<Knob>(control(paramIndex), label, notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		paramIndices_.push_back(paramIndex);
	};

	// GLOBAL CONTROLS: the remaining params that are not drawn as bars/strips on the graph.
	// THE SIX HOMELESS CONTROLS — global threshold, ratio, bandwidth, up/down skew, and crossovers.
	// These are the offsets the per-band controls sum with, so they read as "all three bands".
	// THREE, AND THE OTHER NINE ARE IN `OwlttExpanded` RATHER THAN GONE.
	//
	// Ratio, threshold and up/down stay on the card. They are one idea — how the
	// compression behaves across all three bands — and `owltt.cpp:648-663` computes
	// `band + (global - 0.5)`, so each is a macro over the per-band control the graph draws. The card
	// is the picture plus the global versions of what the picture shows.
	//
	// UP/DOWN IS THE ONE WITH NO PICTURE, which is why it is here as a circle: the bars ARE threshold
	// and the strips ARE ratio, so those two have handles as well as knobs, and the balance between
	// upward and downward compression has nowhere on the graph to live.
	//
	// AND THIS IS A MOVE, NOT A REMOVAL, WHICH IS THE DISTINCTION THAT MATTERS HERE. The last time
	// six of these left this list they simply stopped being reachable and the reachability table was
	// edited to record it. `OwlttExpanded` exists first; `hasExpanded` is true; the invariant takes the
	// union of the two surfaces. A control is on one of them or the build fails.
	addKnob(static_cast<size_t>(owltt::Param::Threshold), "Threshold");
	addKnob(static_cast<size_t>(owltt::Param::Ratio), "Ratio");
	addKnob(static_cast<size_t>(owltt::Param::UpDownSkew), "Up/Down");

	// THE PICTURE, AS A CHILD. It reads the same device through the same host, so nothing is passed
	// down and nothing can drift: the card and the panel each hold one, over one device.
	graph_ = std::make_unique<OwlttGraph>(host_, device_, notifier);
	addChild(graph_.get());

	// THE STEREO STRIP (backlog LQ): L/R or M/S, and how far each band's two channels share one detector.
	stereo_ = std::make_unique<SwitchChip>(control(static_cast<size_t>(owltt::Param::StereoMode)), "Stereo");
	addChild(stereo_.get());
	link_ = std::make_unique<ValueBox>(control(static_cast<size_t>(owltt::Param::StereoLink)), "Link");
	link_->setCompact(true);
	addChild(link_.get());
	// BACKLOG LR: the global Width beside Link — it scales the Character zone's three band widths (100% is the
	// zone's own); the window adds a Width per band.
	width_ = std::make_unique<ValueBox>(control(static_cast<size_t>(owltt::Param::Width)), "Width");
	width_->setCompact(true);
	addChild(width_.get());
	// BACKLOG NB: THE DRIVE, a bar left of the picture — there is no room for a fourth knob. It speaks its
	// changes as the enable does.
	drive_ = std::make_unique<VerticalFader>(control(static_cast<size_t>(owltt::Param::Drive)), "Drive");
	if (notifier_ != nullptr) {
		drive_->setAnnouncer([this](const std::string& line) { notifier_->say(line); });
	}
	addChild(drive_.get());

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// UNGATED NOW. This used to run only when `hitTestThreshold` said kNone, because one press
		// would otherwise both move a threshold AND pick the card out of the lane. The plot is a
		// child frame and consumes its own presses, so a press that reaches HERE was never on the
		// graph — the ordering became unnecessary rather than merely correct.
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		// THE TOGGLE, THE GESTURE AND THE REDRAW ARE THE BASE'S NOW — FxCard::enablePressAt.
		//
		// **THIS IS THE CARD THAT PROVED WHY.** The defect: the light turned off and never came
		// back — and it was ONLY the light. The value toggled and the device bypassed correctly both
		// ways while the square kept whatever it was last painted as, because this handler alone
		// forgot `redraw()`. It was the ninth spelling of a switch, and the ninth spelling is where
		// the difference hid. There is one spelling now, so that particular bug has nowhere to live.
		else if (const auto nowOn = enablePressAt(e.position)) {
			// SAYS SO, like every other card's enable does. Its absence was not a decision either:
			// the tell was a compiler warning — `notifier_` constructed, stored and never read.
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "OWLTT on" : "OWLTT off");
			}
		}
		else if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
		}
		else {
			// NOTHING HERE ANY MORE. The plot is a CHILD FRAME and receives its own presses, so the
			// card's head handling stops where it always should have: at the head. What used to be
			// here was a hit test, a double-click branch, a route-drag anchor and a drag state
			// machine — all of it now in `OwlttGraph`, once, for both surfaces.
			//
			// AND `noteCardPress` NO LONGER HAS TO BE GATED. It used to run only when
			// `hitTestThreshold` said kNone, because one press would otherwise both move a threshold
			// AND pick the card out of the lane. A child frame that consumes the press makes that
			// ordering unnecessary rather than merely correct.
		}
	};
}

device::Modulatable* OwlttCard::enableModulatable() {
	return hasEnable() ? &control(enableControl()) : nullptr;
}

DeviceControl& OwlttCard::control(size_t index) const {
	// REFUSED RATHER THAN TRUSTED. An index past the end used to be an out-of-bounds write into the
	// object; now it is a clamp to a real slot, so a future miscount is a wrong control rather than
	// a corrupted card. Same refusal `FxCard::enableControl` already makes about a declared index.
	if (index >= controls_.size()) {
		index = 0;
	}
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, owltt::kDescriptor, index);
	}
	return *controls_[index];
}



/// WHERE A BAND ENDS ON THE GRAPH — one definition, asked by the drawing, the hit test and a case.
///
/// THIS WAS WRITTEN TWICE, IDENTICALLY, in `hitTestThreshold` and in `draw`. Nothing made them agree
/// and the strips beside them had already drifted -- drawn at 0.70..1.00 and hit-tested at
/// 0.33..0.67 -- which is what made the level bars undraggable. A geometry stated twice is a
/// geometry that will disagree; this is the seventh instance this week and the fix is the same one
/// every time.
///
/// The bands are split by crossover FREQUENCY on a log axis, not into equal thirds -- a test cannot
/// compute this from the width, which is why it is exposed rather than duplicated a third time.







float OwlttCard::openHeight() {
	const float column = Knob::columnHeight(kKnobDiameter);
	// Head + crossover viz + however many rows the knob list needs. THE SAME `kKnobRows` `layOut`
	// divides by, because a card whose height disagrees with its layout clips its own bottom row —
	// and this function is static, so it cannot ask `knobs_` and must be told by the same constant.
	return kHeadHeight + kRowGap + kCrossoverHeight + kRowGap + kStripHeight + kRowGap
	       + static_cast<float>(kKnobRows) * column + static_cast<float>(kKnobRows - 1) * kRowGap
	       + kPad;
}

void OwlttCard::foldedChanged() {
	// `folded_ = folded`, AND IT USED TO BE `folded_ = false`. OWLTT could not be folded at all: the
	// guard above returns whenever the two agree, so the only assignment that ever ran wrote the value
	// the card already had. Every line after it then did the right thing with the wrong number — the
	// knobs were shown, the lane was told, and the card was repainted at its open height.
	//
	// FOUND BY A SWEEP AND NOT BY READING, which is the same instrument and the same week as the dead
	// enable switch: ask every card type the same question instead of the one card every other case
	// drives, and the one that does nothing names itself. Nothing else in this suite folded a OWLTT.
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
	// AND THE PLOT, WHICH IS A CHILD FRAME. `knobs_` is not everything this card holds any more — the
	// crossover graph moved out of `draw` and into its own frame, and a child left visible outside the
	// rectangle its card now occupies still answers presses, over whatever the lane has put there.
	// FilterCard's fold hides its switches for exactly this reason and OWLTT's stopped at the knobs.
	if (graph_ != nullptr) {
		graph_->setVisible(!folded_);
	}
	// AND THE STEREO STRIP, for the same reason: a hidden card's children must not answer presses.
	if (stereo_ != nullptr) {
		stereo_->setVisible(!folded_);
	}
	if (link_ != nullptr) {
		link_->setVisible(!folded_);
	}
	if (width_ != nullptr) {
		width_->setVisible(!folded_);
	}
	if (drive_ != nullptr) {
		drive_->setVisible(!folded_);
	}
}

void OwlttCard::setModulation(ModState* state, EditHistory* history) {
	// AND THE PLOT'S OWN ADAPTERS. Its handles are modulation destinations exactly as the knobs are;
	// a picture whose handles could not be armed would be a set of controls that silently is not one.
	if (graph_ != nullptr) {
		graph_->setModulation(state, history);
	}
	for (auto& knob : knobs_) {
		knob->setModulation(state, history);
	}
	// AND EVERY ADAPTER, WHICH THE KNOBS ABOVE DO NOT COVER — three of this card's knobs shipped
	// unmappable because of the gap between those two claims.
	//
	// THE DEFECT: knobs on the OWLTT card could no longer be mapped, while the filter card and the OWLTT
	// pop-up were fine. Three facts, and the pair is the whole diagnosis: the same device is mappable
	// through its PANEL and not through its CARD, so it is neither the declarations nor the routing
	// authority — both surfaces share those.
	//
	// **`Knob::setModulation` FORWARDS TO ITS CONTROL ONLY IF IT OWNS IT** — `if (owned_ != nullptr)`
	// — and it says why: a knob built over somebody else's `Modulatable` was handed one by whoever
	// built it. This card's knobs are built over `control(paramIndex)`, which is THIS card's adapter
	// and not the knob's, so nothing here told them anything. `FilterCard::setModulation` has carried
	// the matching loop from the start; that is the whole of why its card works and this one did not.
	//
	// WHAT IT LOOKS LIKE WHEN IT IS WRONG, because the symptom points away from the cause:
	// `DeviceControl::display()` computes `isDestination` without `mod_` and `routable` only WITH it, so
	// the knob reports itself a destination and refuses to light. Every check that asks "is this a
	// destination" passes; only the one that asks "would a drag work" fails.
	//
	// EVERY ADAPTER RATHER THAN THE THREE THE KNOBS HOLD, for FilterCard's reason: the bar and strip
	// adapters on the graph are covered above, but an adapter no widget holds is covered by nothing,
	// and it would report itself unarmed forever. Cheap, total, and it cannot fall behind a widget
	// list — which is what this bug was.
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(state, history);
	}
}

visage::Bounds OwlttCard::headBounds() const {
	return ownFrame().head;
}

visage::Bounds OwlttCard::crossoverBounds() const {
	// RIGHT OF THE DRIVE BAR (backlog NB), which takes its width out of the picture's.
	const float left = bodyLeft() + kPad + kDriveWidth + kDriveGap;
	return {left, bodyTopEdge() + kRowGap, std::max(0.0f, bodyLeft() + bodyWidth() - kPad - left), kCrossoverHeight};
}

visage::Bounds OwlttCard::driveBounds() const {
	return {bodyLeft() + kPad, bodyTopEdge() + kRowGap, kDriveWidth, kCrossoverHeight};
}

bool OwlttCard::updateContent() {
	// THE GRAPH, NOT THIS CARD. Nothing on the card's own surface draws a meter, so redrawing the
	// card would repaint a head and a set of knobs to move three bars inside a child.
	return graph_ != nullptr && graph_->update();
}

bool OwlttCard::headEnabled() const {
	return !hasEnable() || control(enableControl()).value() >= 0.5;
}

void OwlttCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void OwlttCard::drawContent(visage::Canvas& canvas) {
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.



	// THE EXPAND MARK — GEOMETRY AND NOT A GLYPH, the standing rule: this font has no guaranteed
	// expand character and a missing glyph is an invisible control. HARM's arrow, drawn the same way,
	// because it is the same verb and a player should not have to learn it twice.

	if (folded_) {
		canvas.setColor(kDisabled);
		const visage::Bounds label = foldedLabelBounds(slotMenuBounds().x());
		canvas.text(modeName(static_cast<int32_t>(std::lround(control(static_cast<size_t>(owltt::Param::Mode)).value()))), font(kKnobReadoutSize), visage::Font::kRight, label.x(), label.y(),
		            label.width(), label.height());
		return;
	}

	// THE PLOT DRAWS ITSELF. Its frame, its crossover lines, its bars, strips, rings and meters are
	// all `OwlttGraph`'s now — the card contributes a rectangle and nothing inside it, which is what
	// lets the panel hold a bigger one without a second copy of any of it.
}

void OwlttCard::resized() {
	layOut();
}

void OwlttCard::layOut() {
	if (graph_ != nullptr) {
		const visage::Bounds plot = crossoverBounds();
		graph_->setBounds(plot.x(), plot.y(), plot.width(), plot.height());
#ifdef FEATHERS_DIAGNOSTIC
		diagnostic::plotLaidOut("card", graph_->positionInWindow().x, graph_->positionInWindow().y,
		                        graph_->width(), graph_->height(), graph_->isVisible());
#endif
	}
	if (drive_ != nullptr) {
		const visage::Bounds bar = driveBounds();
		drive_->setBounds(bar.x(), bar.y(), bar.width(), bar.height());
	}

	const float column = Knob::columnHeight(kKnobDiameter);
	const float inner = bodyWidth() - 2.0f * kPad;
	const float cell = inner / static_cast<float>(kKnobsPerRow);

	// AS MANY ROWS AS THERE ARE KNOBS, rather than two.
	//
	// This was `(row == 0) ? row1Y : row2Y`, which silently piles every knob past the sixth onto row
	// two — so the grid did not hold six, it held six SAFELY and drew any further ones on top of each
	// other. That is why the twelfth control could not simply be added, and the reason it was not
	// obvious is that the failure is a layout that still renders.
	// THE STEREO STRIP, UNDER THE PICTURE: the chip at the left, the Link box beside it. `setBounds`
	// unconditionally (CLAUDE.md's DPI rule).
	const float stripY = bodyTopEdge() + kRowGap + kCrossoverHeight + kRowGap;
	if (stereo_ != nullptr) {
		stereo_->setBounds(bodyLeft() + kPad, stripY + (kStripHeight - SwitchChip::kHeight) * 0.5f, kChipWidth,
		                   SwitchChip::kHeight);
	}
	if (link_ != nullptr) {
		link_->setBounds(bodyLeft() + kPad + kChipWidth + kRowGap,
		                 stripY + (kStripHeight - ValueBox::kCompactHeight) * 0.5f,
		                 std::min(kLinkBoxWidth, std::max(0.0f, inner - kChipWidth - kRowGap)), ValueBox::kCompactHeight);
	}
	// AND WIDTH AFTER LINK (backlog LR), in whatever of the strip is left.
	if (width_ != nullptr) {
		const float x = bodyLeft() + kPad + kChipWidth + kRowGap + kLinkBoxWidth + kRowGap;
		width_->setBounds(x, stripY + (kStripHeight - ValueBox::kCompactHeight) * 0.5f,
		                  std::min(kLinkBoxWidth, std::max(0.0f, bodyLeft() + kPad + inner - x)), ValueBox::kCompactHeight);
	}
	const float topY = stripY + kStripHeight + kRowGap;

	for (size_t i = 0; i < knobs_.size(); ++i) {
		const auto row = static_cast<float>(i / kKnobsPerRow);
		const auto col = static_cast<float>(i % kKnobsPerRow);
		knobs_[i]->setBounds(bodyLeft() + kPad + cell * col, topY + row * (column + kRowGap), cell, column);
	}
}

} // namespace feathers::ui
