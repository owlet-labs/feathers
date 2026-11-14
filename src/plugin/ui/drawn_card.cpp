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
#include "plugin/ui/drawn_card.h"

#include "plugin/ui/expanded_views.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace dr = fx::drawn;

namespace {

/// HOW MANY POINTS THE RESPONSE IS DRAWN FROM. The curve is evaluated in closed form, so this is a
/// drawing resolution and nothing else; 192 is a point every few pixels at the widths this lane uses.
constexpr size_t kCurvePoints = 192;

/// THE VERTICAL SCALE OF THE CURVE STRIP, in dB. The cascade is trimmed so it cannot exceed
/// `kCascadeCeiling` (+12 dB), so the top is chosen to hold that with a little air rather than picked
/// to look right; the bottom is where a null stops being worth drawing.
constexpr float kCurveTopDb = 15.0f;
constexpr float kCurveBottomDb = -45.0f;

/// THE KNOBS THIS CARD LAYS OUT, in cell order. One table, read by the constructor, by `layOut` and
/// by `addressedParamsForTest`, so the three cannot disagree about which knobs exist.
constexpr std::array<dr::Param, DrawnCard::kKnobCells> kKnobs {
    dr::Param::MorphX, dr::Param::MorphY, dr::Param::Cutoff,
    dr::Param::Res,    dr::Param::Drive,  dr::Param::Wake,
};

} // namespace

DrawnCard::DrawnCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
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
	controls_.resize(static_cast<size_t>(dr::Param::kCount));
	for (const dr::Param p : kKnobs) {
		auto knob = std::make_unique<Knob>(control(p),
		                                   dr::kParamDescs[static_cast<size_t>(p)].name, notifier_);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		// WHETHER THIS PRESS TAKES THE PUCK, DECIDED FIRST, because it decides whether the press may
		// ALSO become a card drag. The head is never the plane and a folded card has no plane at all.
		const bool onPuck = !folded_ && !headBounds().contains(e.position)
		                    && planeBounds().contains(e.position);
		// A PRESS THAT DID NOT TAKE THE PUCK MAY BECOME A CARD DRAG — and one that DID must not, or
		// the same gesture morphs the filter and picks the card out of the lane. This card draws its
		// plane itself rather than making it a child frame, so the gate is the card's to apply;
		// WaypointCard carries the same gate for the same reason.
		if (!onPuck) {
			noteCardPress(e);
		}
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		// THE ENABLE SWITCH, tested here because every other card tests it here — matching them is
		// what keeps the spellings in step.
		// THE TOGGLE, THE GESTURE AND THE REDRAW LIVE IN THE BASE — see FxCard::enablePressAt.
		// This card keeps only its own voice, which the base holds no notifier to speak with.
		if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Drawn on" : "Drawn off");
			}
			return;
		}
		// THE WAY INTO THE EXPANDED VIEW, TESTED BEFORE THE HEAD'S FOLD. That order IS load-bearing:
		// the button sits inside the head's rectangle, so a head tested first swallows it and the bug
		// reads as "the button does nothing". Empty until phase 4, and an empty rectangle contains no
		// point, so today this simply falls through.
		// THE HEAD'S OWN VERB IS FOLD, last of the four because it is the whole rectangle the other
		// three sit in.
		if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
			return;
		}
		if (onPuck && e.shouldTriggerPopup() && puckMod().showMenu(*this)) {
			return; // a modulated puck's right-click opens the sources menu, as a modulated knob's does
		}
		if (onPuck) {
			// ARMED, AND THE PRESS IS THE ROUTE'S (`ModHandle`): the amount gesture on the nearer axis, no morph drag.
			const visage::Point centre = puckCentre();
			const size_t axis = puckMod().routeAxisFor(e.isAltDown(), e.position, centre.x, centre.y);
			if (axis != ModHandle::npos) {
				routingPuck_ = true;
				puckMod().beginRoute(axis, e);
				redraw();
				return;
			}
			// ONE GESTURE MOVES BOTH AXES, so both gestures open. A morph that opened only X would
			// leave Y's undo record half-written.
			holdingPuck_ = true;
			control(dr::Param::MorphX).beginGesture();
			control(dr::Param::MorphY).beginGesture();
			dragPuck(e.position);
		}
	};
	// `+=`, NOT `=`. `visage::CallbackList::operator=` is `set()`, which CLEARS the list, and
	// `FxCard`'s constructor installs the card-reorder drag and release on these same two. Assigning
	// here would silently delete the base's handlers and the only way left to reorder this card would
	// be the slot menu — which is exactly the bug WaypointCard shipped and its comment records.
	onMouseDrag() += [this](const visage::MouseEvent& e) {
		if (routingPuck_) {
			puckMod().dragRoute(e);
			redraw();
		}
		else if (holdingPuck_) {
			dragPuck(e.position);
		}
	};
	onMouseUp() += [this](const visage::MouseEvent& e) {
		(void)e;
		if (routingPuck_) {
			puckMod().endRoute();
			routingPuck_ = false;
		}
		if (holdingPuck_) {
			control(dr::Param::MorphX).endGesture();
			control(dr::Param::MorphY).endGesture();
			holdingPuck_ = false;
		}
	};
}

device::Modulatable* DrawnCard::enableModulatable() {
	return hasEnable() ? &control(static_cast<dr::Param>(enableControl())) : nullptr;
}

DeviceControl& DrawnCard::control(dr::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, dr::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

bool DrawnCard::updateContent() {
	// ONE READ, ONE COMPARISON, A `redraw()` ONLY WHEN THEY DIFFER — `OwlttGraph::update` is the shipped
	// version of this shape and `AutomodStrip::update` argues at length for exact equality over a
	// tolerance: the question is "is the screen showing these settings", which is bitwise.
	if (!isVisible()) {
		return false; // `redraw()` is refused for an invisible frame, so a true here would be a lie.
	}
	const dr::Settings now = settings();
	if (haveDrawn_ && now == drawnSettings_) {
		return false;
	}
	drawnSettings_ = now;
	haveDrawn_ = true;
	redraw();
	return true;
}

dr::Settings DrawnCard::settings() const {
	// THE ROW, READ THROUGH THE SEAM — see `ShaperCard::settings` for the same change and the same
	// reason: `fxChain()` reaches into a `DeviceRow` through a type only the parameter road has, and
	// `fieldsPresentAt`/`fieldAt` are that read expressed in the vocabulary both hosts share.
	dr::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return dr::stateOf(row, present);
}

visage::Bounds DrawnCard::headBounds() const { return ownFrame().head; }


/// **THE SAME BOX AS THE PLANE.** Drawn's card was too wide and its graph too tall. As on automod, the
/// filter curve is overlaid on the xy pad, so the card fits without scrolling and is one standard unit
/// wide.
///
/// The two pictures were stacked, which cost `kCurveHeight + kRowGap` of card for a second rectangle
/// saying something about the same device state. Overlaid they cost nothing and read better: the puck
/// is WHERE you are in the morph and the curve is WHAT that sounds like, and moving the puck now moves
/// both inside one frame instead of changing a picture somewhere else on the card.
visage::Bounds DrawnCard::curveBounds() const { return planeBounds(); }

visage::Bounds DrawnCard::planeBounds() const {
	return {bodyLeft() + kPad, kSlotHeadHeight, bodyWidth() - 2.0f * kPad, kPlaneHeight };
}

visage::Point DrawnCard::puckCentre() const {
	const visage::Bounds box = planeBounds();
	const dr::Settings s = settings();
	// Y IS DRAWN UP, so the top of the plane is morphY == 1. The blend's corner order is
	// (x0y0, x1y0, x0y1, x1y1) and nothing here is allowed to disagree with it silently — the case
	// "the puck's corners are the frames the blend calls corners" asserts exactly this mapping.
	return { box.x() + box.width() * std::clamp(s.morphX, 0.0f, 1.0f),
		     box.bottom() - box.height() * std::clamp(s.morphY, 0.0f, 1.0f) };
}

std::string DrawnCard::frameLabelText(dr::Corner corner) {
	return std::string("F") + dr::kFrameNumerals[static_cast<size_t>(corner)];
}

visage::Bounds DrawnCard::frameLabelBounds(dr::Corner corner) const {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return {};
	}
	const std::string text = frameLabelText(corner);
	const float w = trackedWidth(font(kFrameLabelSize), text, kKnobLabelTracking);
	const float h = kFrameLabelSize;
	// THE CORNER ORDER IS THE BLEND'S, AND Y IS DRAWN UP — `X0Y0` is the BOTTOM-left, exactly as
	// `puckCentre` reads it. A table that ran the other way would put F3's mark where the puck morphs
	// toward frame 1, and every assertion about insets and clearances would still pass; the case that
	// drives the puck to each extreme and asks `realisedAt` which frame it landed on is the one that
	// can tell the difference.
	const bool right = corner == dr::X1Y0 || corner == dr::X1Y1;
	const bool top = corner == dr::X0Y1 || corner == dr::X1Y1;
	return { right ? box.right() - kFrameLabelInset - w : box.x() + kFrameLabelInset,
		     top ? box.y() + kFrameLabelInset : box.bottom() - kFrameLabelInset - h, w, h };
}

float DrawnCard::openHeight() {
	const float rows = static_cast<float>((kKnobCells + kKnobsPerRow - 1) / kKnobsPerRow);
	// NO CURVE ROW: the curve is drawn ON the plane now, so the card is shorter by exactly the row it
	// used to occupy.
	return kSlotHeadHeight + kPlaneHeight + kRowGap + rows * (Knob::kHeight + kRowGap) + kPad;
}

float DrawnCard::laneHeightAt(float) const {
	// A CARD THAT CAN FOLD ANSWERS WITH ITS CURRENT HEIGHT, not its open one — the lane stacks what it
	// is told, and a fold that did not reach this number would leave a hole where the card used to be.
	return folded() ? foldedHeight() : openHeight();
}

void DrawnCard::foldedChanged() {
	// EVERYTHING BELOW THE HEAD IS GONE, not merely undrawn — a control still visible outside the
	// rectangle its card now occupies is a control a hand can reach over whatever is underneath.
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
}

void DrawnCard::resized() { layOut(); }

void DrawnCard::layOut() {
	const float cell = (bodyWidth() - 2.0f * kPad) / static_cast<float>(kKnobsPerRow);
	const float top = kSlotHeadHeight + kPlaneHeight + kRowGap;
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const size_t row = i / kKnobsPerRow;
		const size_t col = i % kKnobsPerRow;
		knobs_[i]->setBounds(bodyLeft() + kPad + cell * static_cast<float>(col),
		                     top + static_cast<float>(row) * (Knob::kHeight + kRowGap), cell,
		                     Knob::kHeight);
	}
}

void DrawnCard::dragPuck(visage::Point position) {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 0.0f || box.height() <= 0.0f) {
		return;
	}
	const double tx = std::clamp(static_cast<double>(position.x - box.x())
	                                 / static_cast<double>(box.width()),
	                             0.0, 1.0);
	// UP IS MORE, matching `puckCentre`. The two are the same mapping read in opposite directions and
	// a case drives a press at a known point and asserts both numbers.
	const double ty = std::clamp(static_cast<double>(box.bottom() - position.y)
	                                 / static_cast<double>(box.height()),
	                             0.0, 1.0);
	control(dr::Param::MorphX).setValue(tx);
	control(dr::Param::MorphY).setValue(ty);
	redraw();
}

void DrawnCard::drawCurve(visage::Canvas& canvas) {
	const visage::Bounds box = curveBounds();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}
	// NO BORDER OF ITS OWN — the plane drew one around this same rectangle, and a second would be a
	// double line rather than a frame.

	// UNITY, so a flat response has something to be flat AGAINST. Without it a bypassed device draws
	// a line in the middle of an empty box and says nothing about which line it is.
	const float unityY = box.y() + box.height() * (kCurveTopDb / (kCurveTopDb - kCurveBottomDb));
	canvas.setColor(withAlpha(kLine, 0.35f));
	canvas.segment(box.x(), unityY, box.right(), unityY, 1.0f, true);

	// THE DEVICE'S OWN CLAIM ABOUT WHAT IT DOES, evaluated in closed form through the same function
	// the audio path realises its coefficients from — blend, then globals, then realise. A picture
	// drawn from a second implementation would be a second thing to keep in step.
	const dr::Realised realised = dr::realisedAt(settings());
	canvas.setColor(withAlpha(kAccent, 0.9f));
	float prevX = box.x();
	float prevY = unityY;
	for (size_t i = 0; i < kCurvePoints; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
		// LOG FREQUENCY ACROSS THE STRIP, over the same span the device's own coordinate spans: the
		// left edge is `kMinAngleLog2` octaves below Nyquist and the right edge IS Nyquist.
		const float nu = std::exp2(dr::kMinAngleLog2 * (1.0f - t));
		const float db = 20.0f * std::log10(std::max(dr::magnitudeAt(realised, nu), 1e-6f));
		const float ty = std::clamp((kCurveTopDb - db) / (kCurveTopDb - kCurveBottomDb), 0.0f, 1.0f);
		const float x = box.x() + box.width() * t;
		const float y = box.y() + ty * box.height();
		if (i > 0) {
			canvas.segment(prevX, prevY, x, y, 2.0f, true);
		}
		prevX = x;
		prevY = y;
	}

	// **THE CURVE'S OWN AXIS, BECAUSE IT IS NOT THE PAD'S.** The rectangle now carries two pictures
	// whose X means different things: the plane's is a morph weight between F1/F2 and F3/F4, the
	// curve's is log frequency. That objection is why these were stacked, and it is a real one —
	// unlabelled, the overlay invites a reader to line the response up against the frame corners,
	// which would be a relationship that does not exist.
	//
	// Automod's strip already answers it and this borrows the answer rather than inventing a second
	// one: decade marks in the RESPONSE'S colour, on plates because a full-width curve goes wherever
	// a label is. Two axes on one box is fine once each says which one it is.
	// **IN OCTAVES BELOW NYQUIST, NOT IN HERTZ** — which is the device's own coordinate and the unit
	// its band parameters are already spelled in (*"in octaves below Nyquist — 0 IS Nyquist"*). Hz
	// would need a sample rate, and nothing reaches this card with one: the curve is plotted in
	// normalised frequency, so a `1k` mark here would be a label for a rate nobody stated. Automod can
	// say `100 / 1k / 10k` because its span IS in Hz.
	const visage::Font& face = font(kKnobLabelSize);
	for (const auto& [octavesBelowNyquist, text] :
	     { std::pair { -8.0f, "-8" }, std::pair { -4.0f, "-4 oct" } }) {
		// THE SAME COORDINATE THE CURVE IS PLOTTED IN — `kMinAngleLog2` octaves below Nyquist at the
		// left edge, Nyquist at the right — so a mark cannot drift from the line it is marking.
		const float t = 1.0f - octavesBelowNyquist / dr::kMinAngleLog2;
		if (t < 0.02f || t > 0.98f) {
			continue;
		}
		const float x = box.x() + box.width() * t;
		canvas.setColor(withAlpha(kAccent, 0.5f));
		canvas.segment(x, box.y() + 1.0f, x, box.y() + 4.0f, 1.0f, true);
		const std::string label = text;
		const float w = trackedWidth(face, label, kKnobLabelTracking);
		canvas.setColor(withAlpha(kSurface, 0.9f));
		canvas.roundedRectangle(x - 3.0f, box.y() + 4.0f, w + 6.0f, Knob::kLabelHeight, 3.0f);
		canvas.setColor(withAlpha(kAccent, 0.75f));
		drawTracked(canvas, label, face, kKnobLabelTracking, x, box.y() + 4.0f, Knob::kLabelHeight);
	}
}

void DrawnCard::drawPlane(visage::Canvas& canvas) {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}
	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(box.x(), box.y(), box.width(), box.height(), 1.0f);

	// THE FOUR CORNERS ARE THE FOUR FRAMES, marked so the plane reads as a morph between named things
	// rather than as an empty square. Small and dim: they are not targets — this card does not edit
	// frames, and a corner drawn like a handle would promise a gesture phase 4 owns.
	canvas.setColor(withAlpha(kInk, 0.35f));
	constexpr float kCorner = 3.0f;
	for (const auto& c : { std::pair{box.x(), box.bottom()}, std::pair{box.right(), box.bottom()},
	                       std::pair{box.x(), box.y()}, std::pair{box.right(), box.y()} }) {
		canvas.circle(c.first - kCorner, c.second - kCorner, kCorner * 2.0f);
	}
	drawFrameLabels(canvas);

	// CROSSHAIRS TO THE EDGES, because a puck in the middle of a square is hard to read against and
	// the two axes are independent quantities a hand sets separately.
	const visage::Point p = puckCentre();
	canvas.setColor(withAlpha(kLine, 0.30f));
	canvas.segment(box.x(), p.y, box.right(), p.y, 1.0f, true);
	canvas.segment(p.x, box.y(), p.x, box.bottom(), 1.0f, true);

	// THE HANDLE STAYS AT THE BASE; its halo and amount strokes go under it, the live dots over it.
	puckMod().drawUnder(canvas, p.x, p.y, kPuckRadius);
	canvas.setColor(kAccent);
	canvas.circle(p.x - kPuckRadius, p.y - kPuckRadius, kPuckRadius * 2.0f);
	puckMod().drawOver(canvas, p.x, p.y, kPuckRadius);
}

ModHandle& DrawnCard::puckMod() const {
	if (!puckModBuilt_) {
		// A CONTROL VALUE TO A PLANE COORDINATE: the same mapping `puckCentre` draws by (x grows right, y is drawn up).
		puckMod_ = ModHandle({
		    ModHandleAxis { &control(dr::Param::MorphX), true,
		                    [this](double v) {
			                    const visage::Bounds box = planeBounds();
			                    return box.x() + box.width() * std::clamp(static_cast<float>(v), 0.0f, 1.0f);
		                    } },
		    ModHandleAxis { &control(dr::Param::MorphY), false,
		                    [this](double v) {
			                    const visage::Bounds box = planeBounds();
			                    return box.bottom() - box.height() * std::clamp(static_cast<float>(v), 0.0f, 1.0f);
		                    } },
		});
		puckModBuilt_ = true;
	}
	return puckMod_;
}

void DrawnCard::drawFrameLabels(visage::Canvas& canvas) {
	// WHICH FRAME EACH CORNER IS, SAID OUT LOUD: the waypoint id floats near its waypoint, so it is clear
	// what is going to morph into what. The four corner
	// dots above already said "there are frames here"; they did not say WHICH, so the one question a
	// hand on the puck is actually asking — what am I morphing between — had no answer on this card
	// and the answer only existed behind the expand button.
	//
	// `kMuted` AND THE SMALL FACE, like every other piece of non-interactive ink on this card. A
	// frame marker is a hint: it is not a target, and drawing it at control weight would promise a
	// gesture that belongs to the expanded editor.
	canvas.setColor(kMuted);
	const visage::Font& face = font(kFrameLabelSize);
	for (size_t i = 0; i < dr::kFrames; ++i) {
		const auto corner = static_cast<dr::Corner>(i);
		const visage::Bounds at = frameLabelBounds(corner);
		if (at.width() <= 0.0f) {
			continue;
		}
		drawTracked(canvas, frameLabelText(corner), face, kKnobLabelTracking, at.x(), at.y(),
		            at.height());
	}
}

bool DrawnCard::headEnabled() const {
	return !hasEnable() || control(static_cast<fx::drawn::Param>(enableControl())).value() >= 0.5;
}

void DrawnCard::drawHeadExtras(visage::Canvas& canvas) {
	// **THE HEAD'S MARKS ARE DRAWN HERE, OUTSIDE THE BODY'S CLAMP.** `drawContent` is clamped to the
	// body — which is exactly what stops a card painting over its own head — and in a TOP-head lane
	// the head is outside that rectangle. So a slot menu drawn in `drawContent` is silently CLIPPED:
	// Feathers' Filter card lost its "..." entirely, and the vertical sanity check is what found it.
	drawSlotMenu(canvas, kMuted);
}

void DrawnCard::drawContent(visage::Canvas& canvas) {
	// THE CARD'S OWN GROUND AND ITS NAME. A folded card is its head and nothing else, so a head with
	// no name and no ground would be a 24px blank strip — the affordance complete and unusable.
	// **NO SURFACE AND NO BORDER — `FxCard::draw` painted them.** This card drew its own for as
	// long as there were eight cards each drawing the same two shapes; see `ui::drawCardFrame`.


	// THE ENABLE SWITCH, drawn exactly as the sibling cards draw theirs, because a fifth spelling of a
	// switch is a fifth thing to keep in step. A card that does not draw it and does not test a press
	// against it has a control the device declares and no hand can reach — which is what the waypoint
	// filter shipped as "only first effect in chain is processing audio".

	// THE WAY INTO THE EDITOR — the same diagonal with an arrowhead at each end that HARM, OWLTT and
	// Waypoint draw, because it is the same verb and a player should not have to learn it twice.
	//
	// GEOMETRY AND NOT A GLYPH, the standing rule: this font has no guaranteed expand character and a
	// missing glyph is an invisible control. **This was missing entirely for one build** — the button
	// existed, hit-tested and opened the panel, and drew nothing at all, which is the honesty failure
	// in its purest form: an affordance a hand can press and an eye cannot find. The head sweep does
	// not catch it, because geometry and a press are both satisfied by an invisible button.

	if (folded_) {
		return;
	}
	// **THE PLANE FIRST, THEN THE CURVE ON IT.** They share a rectangle now, and the curve is the
	// thing being read — a puck drawn over it would hide the answer at exactly the point the hand is
	// pointing at.
	drawPlane(canvas);
	drawCurve(canvas);
}

void DrawnCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// AND EVERY ADAPTER, INCLUDING THE ONES NO KNOB HOLDS — the puck edits MorphX and MorphY through
	// a gesture as well as through their knobs, and an adapter that never learned the panel's state
	// reports itself unarmed forever. FilterCard's rule: cheap, total, and it cannot fall behind a
	// widget list.
	for (size_t i = 0; i < static_cast<size_t>(dr::Param::kCount); ++i) {
		control(static_cast<dr::Param>(i)).setModulation(mod, history);
	}
}

std::vector<dr::Param> DrawnCard::addressedParamsForTest() const {
	std::vector<dr::Param> out;
	// ALL SIX, INCLUDING `Wake`. It used to be excluded here — drawn but disabled, and recorded as a
	// gap — because the device declared it and nothing read it. Design §8 gave it a job, so the knob
	// answers the hand and the claim is now true. The reachability table's `drwn` entry lost its
	// `Smooth` gap in the same change; a claim here without that is how a dead control passes a guard.
	for (const dr::Param p : kKnobs) {
		out.push_back(p);
	}
	// THE PUCK reaches the same two the first two knobs do, by a different gesture. Listed once — the
	// guard asks whether SOMETHING reaches a control, not how many things do — and a case drives a
	// real press at real coordinates and asserts both values.
	out.push_back(dr::Param::Enabled);
	return out;
}

void DrawnCard::offeredControls(std::vector<Offered>& out) const {
	FxCard::offeredControls(out);
	// THE HAND-DRAWN PUCK: one drag writes Morph X and Morph Y (a side road; the boxes are the faces).
	out.push_back({ &control(dr::Param::MorphX), Road::Gesture });
	out.push_back({ &control(dr::Param::MorphY), Road::Gesture });
}

} // namespace feathers::ui
