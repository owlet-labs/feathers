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
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/widget_registry.h"
#include "plugin/ui/waypoint_card.h"

#include "plugin/ui/expanded_views.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace feathers::ui {

namespace wp = fx::waypoint;
namespace flt = fx::filter;

namespace {

constexpr float kDotRadius = 5.0f;
/// A GRAB RADIUS LARGER THAN THE DOT, because a target you can see and cannot hit is OWLTT's
/// "clunky" in miniature. Generous enough for four dots that may sit close together, and the hit
/// test resolves ties by distance so overlapping dots still pick the nearer one.
constexpr float kGrabRadius = 11.0f;
constexpr size_t kCurvePoints = 128;

/// THE DEVICE-WIDE KNOBS, in the order they are laid out. One table, read by `layOut` and by
/// `addressedParamsForTest`, which is what stops two functions disagreeing about which knob is which.
constexpr std::array<wp::Param, 4> kDeviceKnobs {
	wp::Param::Morph,
	wp::Param::Type,
	wp::Param::Keytrack,
	wp::Param::Poly,
};

[[nodiscard]] wp::Param macroParam(size_t waypoint, size_t which) {
	const size_t first = static_cast<size_t>(wp::Param::Cutoff0) + waypoint * 4 + 2 + which;
	return static_cast<wp::Param>(first);
}

[[nodiscard]] wp::Param cutoffParam(size_t waypoint) {
	return static_cast<wp::Param>(static_cast<size_t>(wp::Param::Cutoff0) + waypoint * 4);
}

[[nodiscard]] wp::Param resonanceParam(size_t waypoint) {
	return static_cast<wp::Param>(static_cast<size_t>(wp::Param::Cutoff0) + waypoint * 4 + 1);
}

} // namespace

WaypointCard::WaypointCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
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
	controls_.resize(static_cast<size_t>(wp::Param::kCount));
	const auto addKnob = [&](wp::Param p, const char* label = nullptr) {
		auto knob = std::make_unique<Knob>(
		    control(p), label != nullptr ? label : wp::kParamDescs[static_cast<size_t>(p)].name,
		    notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	};
	for (const wp::Param p : kDeviceKnobs) {
		addKnob(p);
	}
	// EVERY WAYPOINT'S MACROS GET THEIR OWN KNOB, and only the selected pair is VISIBLE.
	//
	// THE ALTERNATIVE WAS RETARGETING ONE PAIR, and it was the first thing I wrote: `Knob` has no
	// `setControl` and adding one would mutate the most shared widget in the editor to serve a single
	// card. Eight knobs of which two are shown costs a little memory and buys a much stronger claim —
	// every per-waypoint control has its OWN widget, so "reached" is a fact about the widget list
	// rather than a statement about a selection that happened to be set when a case looked.
	// **WITHOUT THE WAYPOINT'S NUMBER ON THE FACE.** The descriptor calls these `Macro 1 1` and
	// `Macro 2 1` — macro 1 and 2 OF WAYPOINT 1 — which is right for a host's automation list, where
	// all eight are visible at once and the index is the only thing telling them apart.
	//
	// ON THE CARD IT DISTINGUISHES NOTHING. `retarget` shows exactly ONE waypoint's pair at a time and
	// the selected dot says which, so the trailing digit is a second answer to a question already
	// answered — and it was wide enough to push `MACRO 1 1` into its neighbour's cell at one unit,
	// which is what the backlog recorded as labels colliding.
	//
	// THE DESCRIPTOR IS UNCHANGED. A host still sees the disambiguated name; only the caption differs,
	// which is exactly the split between an automation identity and a thing to read.
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		addKnob(macroParam(i, 0), "Macro 1");
		addKnob(macroParam(i, 1), "Macro 2");
	}
	retarget();

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// WHICH DOT THIS PRESS IS ON, DECIDED FIRST, because it decides whether the press may ALSO
		// become a card drag. The head is never the plane and a folded card has no plane at all, so
		// both are ruled out before the plane is asked — which is also where the fold's "nothing below
		// the head exists" refusal now lives.
		const size_t hit = (folded_ || headBounds().contains(e.position)) ? wp::kWaypoints
		                                                                 : dotAt(e.position);
		// A PRESS THAT DID NOT TAKE A DOT MAY BECOME A CARD DRAG — and one that DID must not, or the
		// same gesture moves a waypoint and picks the card out of the lane. OWLTT does not need this
		// gate because its plot is a child frame and consumes its own presses; this card draws its
		// plane itself, so the gate is the card's to apply.
		if (hit >= wp::kWaypoints) {
			noteCardPress(e);
		}
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		// THE ENABLE SWITCH, TESTED BEFORE THE DOTS. It sits in the head and the dots sit in the
		// plane, so the order is not load-bearing for hit priority — it is here because every other
		// card tests it here, and matching them is what keeps the five spellings in step.
		// THE TOGGLE, THE GESTURE AND THE REDRAW LIVE IN THE BASE — see FxCard::enablePressAt.
		// This card keeps only its own voice, which the base holds no notifier to speak with.
		if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Waypoint on" : "Waypoint off");
			}
			return;
		}
		// THE WAY INTO THE EXPANDED VIEW, TESTED BEFORE THE HEAD'S FOLD. That order IS load-bearing:
		// the button sits inside the head's rectangle, so a head tested first swallows it and the bug
		// reads as "the button does nothing". OWLTT's and HARM's hit tests carry the same note.
		//
		// AND AN EMPTY RECTANGLE CONTAINS NO POINT, so a build that cannot construct the panel falls
		// through to the fold without a second branch asking whether there is one.
		// THE HEAD'S OWN VERB IS FOLD, and it is last of the four because it is the whole rectangle
		// the other three sit in.
		if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			return;
		}
		if (hit < wp::kWaypoints) {
			// SELECTING AND GRABBING ARE ONE PRESS. A hand that reaches for a dot means that
			// waypoint, so the knob row follows without a second gesture — the selection IS what the
			// press was about.
			select(hit);
			held_ = hit;
			// THE GRAB OFFSET, LATCHED WITH THE DOT — see `grabOffset_`.
			{
				const visage::Point centre = dotCentre(hit);
				grabOffset_ = visage::Point { centre.x - e.position.x, centre.y - e.position.y };
			}
			control(cutoffParam(hit)).beginGesture();
			control(resonanceParam(hit)).beginGesture();
		}
	};
	// `+=`, NOT `=`, AND THAT IS THE WHOLE OF A THIRD MISSING AFFORDANCE.
	//
	// `visage::CallbackList::operator=` is `set()`, which CLEARS the list. `FxCard`'s constructor
	// installs the card-reorder drag and release on these same two lists, so this card assigning to
	// them did not add a handler — it REPLACED the base's. The card reported no drag, the lane never
	// learned a slot had been picked up, and the only way left to reorder a Waypoint was the slot
	// menu. A drag is the intended way to reorder, not "move earlier".
	//
	// NO OTHER CARD IN THIS LANE TOUCHES THESE LISTS, which is why the one existing case for the
	// gesture — driven through a HARM — could not see it. A sweep across every card type could.
	onMouseDrag() += [this](const visage::MouseEvent& e) {
		if (held_ < wp::kWaypoints) {
			// THE POINTER PLUS THE GRAB OFFSET, never the raw pointer — see `grabOffset_`.
			dragDot({ e.position.x + grabOffset_.x, e.position.y + grabOffset_.y });
		}
	};
	onMouseUp() += [this](const visage::MouseEvent& e) {
		(void)e;
		if (held_ < wp::kWaypoints) {
			control(cutoffParam(held_)).endGesture();
			control(resonanceParam(held_)).endGesture();
			held_ = wp::kWaypoints;
			grabOffset_ = {};
		}
	};
}

device::Modulatable* WaypointCard::enableModulatable() {
	return hasEnable() ? &control(static_cast<wp::Param>(enableControl())) : nullptr;
}

DeviceControl& WaypointCard::control(wp::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, wp::kDescriptor, i, mod_,
		                                           history_);
	}
	return *controls_[i];
}

bool WaypointCard::updateContent() {
	// ONE READ, ONE COMPARISON, A `redraw()` ONLY WHEN THEY DIFFER — see `FilterCard::update` beside it
	// and `AutomodStrip::update` for why the comparison is exact rather than tolerant.
	if (!isVisible()) {
		return false;
	}
	const wp::Settings now = settings();
	if (haveDrawn_ && now == drawnSettings_) {
		return false;
	}
	drawnSettings_ = now;
	haveDrawn_ = true;
	redraw();
	return true;
}

wp::Settings WaypointCard::settings() const {
	// THE ROW, READ THROUGH THE SEAM — `fieldsPresentAt`/`fieldAt` are the chain read without the
	// container only the parameter road has. Same numbers, by construction.
	wp::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return wp::stateOf(row, present);
}

visage::Bounds WaypointCard::planeBounds() const {
	return { bodyLeft() + kPad, bodyTopEdge(), bodyWidth() - 2.0f * kPad, planeHeight() };
}

float WaypointCard::planeHeight() const {
	// FROM THE BODY'S TOP, NOT THE TOP HEAD'S FOOT: in the rack the head is a left spine and the body starts
	// at 0. The knob rows keep their full height and the plane gives way, as Smooth's curve does.
	const float rows = static_cast<float>((kKnobCells + kKnobsPerRow - 1) / kKnobsPerRow);
	const float room = ownFrame().body.bottom() - bodyTopEdge() - kRowGap - rows * (Knob::kHeight + kRowGap) - kPad;
	return std::clamp(room, 0.0f, kPlaneHeight);
}

visage::Bounds WaypointCard::headBounds() const {
	return ownFrame().head;
}


float WaypointCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	if (axis_ != LaneAxis::Horizontal) {
		return FxCard::laneExtent(cross);
	}
	// **THE WIDTH THE CAPTIONS ACTUALLY NEED, MEASURED RATHER THAN GUESSED.** Every knob this card
	// builds is asked how wide its own name draws; the widest sets the cell. A constant here would be
	// a number chosen from one font at one size, and this file has been caught by exactly that before.
	//
	// The base's floor still applies — a card narrower than a unit would break the neat rows the unit
	// rule exists for, so this only ever grows.
	float widest = 0.0f;
	for (const auto& knob : knobs_) {
		if (knob != nullptr) {
			widest = std::max(widest, knob->captionWidth());
		}
	}
	const float cell = widest + kKnobLabelGutter;
	const float needed =
	    kCardHeadHeight + 2.0f * kPad + cell * static_cast<float>(kKnobsPerRow);
	return std::max(needed, FxCard::laneExtent(cross));
}

float WaypointCard::openHeight() {
	const float rows = static_cast<float>((kKnobCells + kKnobsPerRow - 1) / kKnobsPerRow);
	return kSlotHeadHeight + kPlaneHeight + kRowGap + rows * (Knob::kHeight + kRowGap) + kPad;
}

void WaypointCard::foldedChanged() {
	// THROUGH `retarget`, NOT THROUGH A SECOND VISIBILITY LOOP. Which knobs this card shows is
	// already two questions — is the card open, and which waypoint is selected — and answering them
	// in two places is how a fold-then-select leaves a hidden macro knob visible. `retarget` now owns
	// both.
	retarget();
	// AND THE LANE IS TOLD, because a fold changes this card's HEIGHT and the lane places cards by
	// asking for it. redraw() alone repaints the card at the size it already had.
}

visage::Point WaypointCard::dotCentre(size_t waypoint) const {
	const visage::Bounds box = planeBounds();
	const wp::Settings s = settings();
	const wp::Waypoint& w = s.point[std::min(waypoint, wp::kWaypoints - 1)];
	// X FROM LOG FREQUENCY, which is the axis the filter card already uses, so the two devices read
	// as one family.
	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	const double tx = std::clamp((static_cast<double>(w.cutoffLog2Hz) - flt::kCutoffMinLog2) / span,
	                             0.0, 1.0);
	// Y FROM RESONANCE, INVERTED so that up is more — the direction every other vertical drag in this
	// editor goes.
	const float ty = 1.0f - std::clamp(w.resonance, 0.0f, 1.0f);
	return { box.x() + static_cast<float>(tx) * box.width(), box.y() + ty * box.height() };
}

size_t WaypointCard::dotAt(visage::Point position) const {
	size_t best = wp::kWaypoints;
	float bestDist = kGrabRadius;
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const visage::Point c = dotCentre(i);
		const float d = std::hypot(position.x - c.x, position.y - c.y);
		// NEAREST WINS RATHER THAN FIRST, so two dots close together resolve to the one the hand is
		// actually on. First-wins would make waypoint 0 unreachable whenever another sat on top of it.
		if (d <= bestDist) {
			bestDist = d;
			best = i;
		}
	}
	return best;
}

void WaypointCard::dragDot(visage::Point position) {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
	const double tx = std::clamp(static_cast<double>(position.x - box.x())
	                                 / static_cast<double>(box.width()),
	                             0.0, 1.0);
	const float ty =
	    1.0f - std::clamp((position.y - box.y()) / box.height(), 0.0f, 1.0f);
	control(cutoffParam(held_)).setValue(flt::kCutoffMinLog2 + tx * span);
	control(resonanceParam(held_)).setValue(static_cast<double>(ty));
	redraw();
}

void WaypointCard::select(size_t waypoint) {
	if (waypoint >= wp::kWaypoints || waypoint == selected_) {
		return;
	}
	selected_ = waypoint;
	retarget();
	redraw();
}

void WaypointCard::retarget() {
	// A FOLDED CARD SHOWS NONE OF THEM, for the reason a closed rail does: a control drawn outside
	// the rectangle its card now occupies is a control a hand can still reach, over whatever the lane
	// put underneath.
	//
	// THE SELECTED WAYPOINT'S PAIR IS SHOWN AND THE OTHER SIX ARE NOT. This is the whole of "the knob
	// row follows the selection": the card gains a selection and nothing else changes shape.
	//
	// BOTH QUESTIONS ANSWERED HERE, in one place. Folding through a second visibility loop is how a
	// fold-then-select ends up showing a knob the fold hid.
	for (size_t i = 0; i < kDeviceKnobs.size(); ++i) {
		knobs_[i]->setVisible(!folded_);
	}
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const bool shown = !folded_ && i == selected_;
		knobs_[kDeviceKnobs.size() + i * 2]->setVisible(shown);
		knobs_[kDeviceKnobs.size() + i * 2 + 1]->setVisible(shown);
	}
	layOut();
}

void WaypointCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the dots' cutoff and resonance controls
	// are edited by a gesture rather than by a widget, and an adapter that never learned the panel's
	// state reports itself unarmed forever. FilterCard's rule, and the reason it says "cheap, total,
	// and it cannot fall behind a widget list".
	for (size_t i = 0; i < static_cast<size_t>(wp::Param::kCount); ++i) {
		control(static_cast<wp::Param>(i)).setModulation(mod, history);
	}
}

float WaypointCard::laneHeightAt(float) const {
	// A CARD THAT CAN FOLD ANSWERS WITH ITS CURRENT HEIGHT, not its open one — `FxCard::laneHeight`
	// states it: the lane stacks what it is told, and a fold that did not reach this number would
	// leave a hole where the card used to be.
	return folded() ? foldedHeight() : openHeight();
}

void WaypointCard::resized() { layOut(); }

void WaypointCard::layOut() {
	const float cell = (bodyWidth() - 2.0f * kPad) / static_cast<float>(kKnobsPerRow);
	const float top = planeBounds().bottom() + kRowGap;
	const auto place = [&](Knob& knob, size_t cellIndex) {
		const size_t row = cellIndex / kKnobsPerRow;
		const size_t col = cellIndex % kKnobsPerRow;
		knob.setBounds(bodyLeft() + kPad + cell * static_cast<float>(col),
		               top + static_cast<float>(row) * (Knob::kHeight + kRowGap), cell,
		               Knob::kHeight);
	};
	// MORPH, THEN THE SELECTED WAYPOINT'S TWO MACROS, THEN THE THREE DEVICE-WIDE CHOICES. The hidden
	// macro knobs are given the same bounds as the shown ones rather than none: an invisible frame
	// with a real rectangle answers `frameAtPoint` the same way a visible one does, so placing them
	// nowhere would make the visible pair's own hit test depend on which was selected.
	place(*knobs_[0], 0); // Morph
	place(*knobs_[kDeviceKnobs.size() + selected_ * 2], 1);
	place(*knobs_[kDeviceKnobs.size() + selected_ * 2 + 1], 2);
	for (size_t i = 1; i < kDeviceKnobs.size(); ++i) {
		place(*knobs_[i], 2 + i);
	}
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		if (i == selected_) {
			continue;
		}
		place(*knobs_[kDeviceKnobs.size() + i * 2], 1);
		place(*knobs_[kDeviceKnobs.size() + i * 2 + 1], 2);
	}
}

void WaypointCard::drawPlane(visage::Canvas& canvas) {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}
	const wp::Settings s = settings();

	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(box.x(), box.y(), box.width(), box.height(), 1.0f);

	// THE FOUR WAYPOINT CURVES, GHOSTED, AND THE LIVE ONE OVER THEM. Every one of these is drawn
	// through `flt::gainProfile` from a `flt::Settings` — the four stored states and the interpolated
	// one — which is the whole reason `settingsAtMorph` returns the filter's own type. No new maths.
	//
	// AT THE RATE THE ENGINE IS RUNNING, not a hardcoded 48000: filter_card.cpp measured up to 1.67 dB
	// between the drawn curve and the filter that plays on an ordinary 44.1k session.
	const auto drawCurve = [&](const flt::Settings& fs, uint32_t colour, float thickness) {
		std::array<float, kCurvePoints> freq {};
		std::array<float, kCurvePoints> gain {};
		flt::gainProfile(fs, host_.sampleRate(), freq.data(), gain.data(), kCurvePoints);
		canvas.setColor(colour);
		float prevX = box.x();
		float prevY = box.bottom();
		for (size_t i = 0; i < kCurvePoints; ++i) {
			const float x = box.x() + box.width() * (static_cast<float>(i)
			                                         / static_cast<float>(kCurvePoints - 1));
			// dB into the box, +12 at the top and -36 at the bottom — a window that shows a resonant
			// peak and a lowpass skirt in the same picture.
			const float db = 20.0f * std::log10(std::max(gain[i], 1e-6f));
			const float ty = std::clamp((12.0f - db) / 48.0f, 0.0f, 1.0f);
			const float y = box.y() + ty * box.height();
			if (i > 0) {
				canvas.segment(prevX, prevY, x, y, thickness, true);
			}
			prevX = x;
			prevY = y;
		}
	};

	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		flt::Settings fs;
		fs.enabled = true;
		fs.type = s.type;
		fs.cutoffLog2Hz = s.point[i].cutoffLog2Hz;
		fs.resonance = s.point[i].resonance;
		fs.macro1 = s.point[i].macro1;
		fs.macro2 = s.point[i].macro2;
		drawCurve(fs, withAlpha(kLine, 0.35f), 1.0f);
	}
	drawCurve(wp::settingsAtMorph(s, static_cast<double>(s.morph)), withAlpha(kAccent, 0.9f), 2.0f);

	// THE POLYLINE, IN INDEX ORDER, because that is the order Morph walks. It is a picture of the
	// visiting order and nothing more: the morph does NOT travel it by arc length, so this line's
	// LENGTH means nothing and only its sequence does.
	canvas.setColor(withAlpha(kInk, 0.5f));
	for (size_t i = 0; i + 1 < wp::kWaypoints; ++i) {
		const visage::Point a = dotCentre(i);
		const visage::Point b = dotCentre(i + 1);
		canvas.segment(a.x, a.y, b.x, b.y, 1.0f, true);
	}

	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const visage::Point c = dotCentre(i);
		canvas.setColor(i == selected_ ? kAccent : kInk);
		canvas.circle(c.x - kDotRadius, c.y - kDotRadius, kDotRadius * 2.0f);
	}
}

bool WaypointCard::headEnabled() const {
	return !hasEnable() || control(static_cast<fx::waypoint::Param>(enableControl())).value() >= 0.5;
}

void WaypointCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void WaypointCard::drawContent(visage::Canvas& canvas) {
	// THE CARD'S OWN GROUND AND ITS NAME, which this card did not draw either.
	//
	// EVERY OTHER CARD IN THIS LANE DRAWS BOTH and this one drew neither, so a Waypoint was a plane
	// and a knob row floating on the lane's background with nothing saying which device they were.
	// It is listed here with the fold rather than separately because the FOLD IS WHAT MAKES IT
	// LOAD-BEARING: a folded card is its head and nothing else, and a head with no name and no ground
	// is a 24px blank strip — the affordance would be complete and unusable.
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.

	// THE ENABLE SWITCH, WHICH THIS CARD DID NOT DRAW AND DID NOT ANSWER. `hasEnable()` is true
	// because the DEVICE declares an Enabled control; the card simply never rendered it and never
	// tested a press against it, and no other surface reaches it either — the expanded view has no
	// toggle. So a Waypoint added to a chain could not be switched into the signal by any means, and
	// since Enabled defaults to 0 it was PERMANENTLY SILENT. From a player's chair that is
	// indistinguishable from "the effect does not process", which is how it was reported.
	//
	// Drawn exactly as OwlttCard and HarmCard draw theirs, because a fifth spelling of a switch is a
	// fifth thing to keep in step.

	// THE WAY INTO THE EXPANDED VIEW — the same diagonal with an arrowhead at each end that HARM and
	// OWLTT draw, because it is the same verb and a player should not have to learn it twice.
	//
	// GEOMETRY AND NOT A GLYPH, the standing rule: this font has no guaranteed expand character and a
	// missing glyph is an invisible control. HARM's own comment records what happened to the first
	// version of this mark — four corner brackets on a 12px box met in the middle and drew as a
	// closed square two pixels from the enable square, one filled and one outlined, meaning entirely
	// different things. Copied rather than re-invented for exactly that reason.

	if (folded_) {
		// FOLDED IS A WHOLE DRAWING, not a cut-off version of the open one: the head, and the one fact
		// that says what this Waypoint is set to.
		//
		// THE TYPE, because it is the device's one shared setting — the types are constant for all
		// morph points by design, which is what makes a single name honest here. Morph and the
		// sixteen waypoint fields are four states and a position between them; none of them reduces to
		// a line of text, and the picture that does show them is exactly what the fold puts away.
		//
		// CLEAR OF THE HEAD'S BUTTONS, and measured from the LEFTMOST of them — which is the expand
		// button, not the square. `foldedLabelBounds` takes that argument precisely so a new occupant
		// moves the text; handing it the square while the expand button sits further left would print
		// the type name straight over the button, which is the filter card's old defect
		// reintroduced by the fix's own mechanism going unused.
		canvas.setColor(kDisabled);
		// THE LEFTMOST HEAD OCCUPANT, which is what the folded text must stop short of.
		const visage::Bounds ex = expandBounds();
		const visage::Bounds label =
		    foldedLabelBounds(ex.width() > 0.0f ? ex.x() : enableBounds().x());
		canvas.text(flt::kTypeNames[static_cast<size_t>(settings().type)], font(kKnobReadoutSize),
		            visage::Font::kRight, label.x(), label.y(), label.width(), label.height());
		return;
	}

	drawPlane(canvas);
}

std::vector<wp::Param> WaypointCard::addressedParamsForTest() const {
	std::vector<wp::Param> out;
	// THE DEVICE-WIDE KNOBS, asked of the one table rather than restated.
	for (const wp::Param p : kDeviceKnobs) {
		out.push_back(p);
	}
	// THE DOTS: dragging dot n writes its cutoff and its resonance. One gesture, four dots, and a
	// case asserts the VALUE each writes.
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		out.push_back(cutoffParam(i));
		out.push_back(resonanceParam(i));
		// THE MACROS OF EVERY WAYPOINT, reached by SELECTING it — the knob row retargets, so the road
		// exists for all four and not only for whichever is selected right now. A case selects each
		// and asserts the knob writes to that waypoint.
		out.push_back(macroParam(i, 0));
		out.push_back(macroParam(i, 1));
	}
	// ENABLED is the square in the head, drawn and answered by THIS CARD — `draw` renders it and the
	// press handler above tests a hit against it. Not a knob, for the reason the filter's is not.
	//
	// THIS LINE USED TO SAY "which FxCard owns and draws", AND FxCard DOES NEITHER. `FxCard` derives
	// the square's RECTANGLE and stops there; every card draws and answers its own. So this accessor
	// asserted the very thing it was written to verify, and the reachability guard — which takes this
	// list at its word — reported Enabled reached while no hand could reach it. That is how the dead
	// switch survived the card's whole implementation and shipped as "only the first effect in the
	// chain processes audio".
	//
	// A LIST OF CLAIMS IS ONLY WORTH THE CASES BEHIND IT. Enabled belongs here now because the card
	// handles it and "EVERY card that declares an enable switch can actually be switched" drives a
	// real press at real window coordinates to prove it.
	out.push_back(wp::Param::Enabled);
	return out;
}

void WaypointCard::offeredControls(std::vector<Offered>& out) const {
	FxCard::offeredControls(out);
	// THE DOTS ON THE PLANE: dragging dot n writes its cutoff across and its resonance up (side roads).
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		out.push_back({ &control(cutoffParam(i)), Road::Gesture });
		out.push_back({ &control(resonanceParam(i)), Road::Gesture });
	}
}

} // namespace feathers::ui
