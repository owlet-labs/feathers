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

#include "plugin/ui/slot_copy_menu.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/menu_anchor.h"
#include "plugin/ui/drawn_expanded.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace dr = fx::drawn;

namespace {

constexpr size_t kCurvePoints = 224;
constexpr float kCurveTopDb = 15.0f;
constexpr float kCurveBottomDb = -45.0f;

//------------------------------------------------------------------------------------------------
// THE RADIUS WARP — see the long note beside `kCircleSoftness` in the header
//------------------------------------------------------------------------------------------------

/// HOW FAR FROM THE UNIT CIRCLE, IN RATIOS — signed, negative below the circle. A logarithm in
/// `|r - 1|` once that distance exceeds `kCircleSoftness`, a straight line inside it, and one
/// function across the whole axis rather than two branches joined by hand.
float circleWarp(float radius) {
	return std::asinh((radius - 1.0f) / DrawnExpanded::kCircleSoftness);
}

float circleUnwarp(float warped) {
	return 1.0f + DrawnExpanded::kCircleSoftness * std::sinh(warped);
}

/// THE HALF-SPAN, AND ONE NUMBER REALLY DOES DESCRIBE BOTH ENDS: the floor of the plane is r = 0 and
/// the ceiling is r = 2, which are the same distance from the circle, and `asinh` is odd. So the
/// warped coordinate runs symmetrically over ±this — what is NOT symmetric is how many pixels each
/// side gets, which is `kCircleHeight`'s job.
float warpSpan() { return circleWarp(DrawnExpanded::kRadiusCeiling); }

} // namespace

DrawnExpanded::DrawnExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                             Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_.resize(static_cast<size_t>(dr::Param::kCount));

	// **THE CARD'S SIX, IN THE CARD'S OWN ORDER.** By design a window is a superset of its card, and a
	// superset that rearranges is a different kind of unhelpful — a hand that knows the card should
	// find these where it expects them. `drawn_card.cpp` declares the same order.
	//
	// They go in the band already reserved beneath the plane: `minimumSize` has always asked for 90px
	// there, and nothing drew in it.
	for (const dr::Param p : { dr::Param::MorphX, dr::Param::MorphY, dr::Param::Cutoff,
	                           dr::Param::Res, dr::Param::Drive, dr::Param::Wake }) {
		auto knob = std::make_unique<Knob>(control(static_cast<size_t>(p)),
		                                   dr::kParamDescs[static_cast<size_t>(p)].name,
		                                   notifier);
		knob->setFaceDiameter(Knob::kEngineRailDiameter);
		addChild(knob.get());
		knobs_.push_back(std::move(knob));
		knobParams_.push_back(p);
	}
	static_assert(kKnobCells == 6, "the row reserves room for exactly the card's six");

	onMouseDown() = [this](const visage::MouseEvent& e) {
		for (size_t f = 0; f < dr::kFrames; ++f) {
			if (tabBounds(f).contains(e.position)) {
				// A POPUP TRIGGER, NOT A RIGHT BUTTON — the same reason the type menu below gives:
				// on macOS a ctrl-left click is this gesture and visage already knows it, so spelling
				// it as the button makes the menu unreachable on a trackpad.
				if (e.shouldTriggerPopup()) {
					showFrameMenu(f, e.position);
					return;
				}
				selectFrame(f);
				return;
			}
		}
		const Hit hit = hitAt(e.position);
		if (hit.what == Grab::None) {
			return;
		}
		band_ = hit.band;
		// THE TYPE MENU IS A POPUP TRIGGER, not a right press — `shouldTriggerPopup()` rather than
		// `isRightButton()`, because on macOS a ctrl-left click is the same gesture and visage already
		// knows that. Spelling it as the button would have made the menu unreachable on a trackpad.
		if (e.shouldTriggerPopup()) {
			showTypeMenu(hit.band, e.position);
			return;
		}
		held_ = hit.what;
		// THE GRAB OFFSET, LATCHED WITH THE HANDLE — see `grabOffset_`. Taken from the handle this
		// press actually took, because the pole and the zero of a band sit in different places.
		{
			const visage::Point centre =
			    hit.what == Grab::Pole ? poleCentre(hit.band) : zeroCentre(hit.band);
			grabOffset_ = visage::Point { centre.x - e.position.x, centre.y - e.position.y };
		}
		// LATCHED AT THE PRESS: see `heldBinding_`. Read from the STORED TYPE rather than inferred
		// from where the two points happen to sit — two points can coincide without the band being
		// bypassed, and a band can be a Notch with its zero anywhere the last pole drag left it.
		heldBinding_ = bindingOf(band(hit.band).type);
		if (held_ == Grab::Pole) {
			control(fieldIndex(hit.band, dr::PoleRadius)).beginGesture();
			control(fieldIndex(hit.band, dr::PoleAngle)).beginGesture();
			// EXACTLY WHAT THE DRAG WILL WRITE, and no more: bracketing a control this gesture never
			// touches posts a host an empty gesture on it.
			if (heldBinding_ != ZeroBinding::None) {
				control(fieldIndex(hit.band, dr::ZeroAngle)).beginGesture();
			}
			if (heldBinding_ == ZeroBinding::Full) {
				control(fieldIndex(hit.band, dr::ZeroRadius)).beginGesture();
			}
		}
		else {
			// A ZERO GRAB IS ONLY OFFERED WHERE IT IS DRAGGABLE — `hitAt` refuses the rest — so this
			// is Free or Bell. Bell's angle is the pole's and is not this gesture's to move.
			control(fieldIndex(hit.band, dr::ZeroRadius)).beginGesture();
			if (band(hit.band).type == dr::Type::Free) {
				control(fieldIndex(hit.band, dr::ZeroAngle)).beginGesture();
			}
		}
		redraw();
	};
	onMouseDrag() += [this](const visage::MouseEvent& e) {
		// THE POINTER PLUS THE GRAB OFFSET, never the raw pointer — see `grabOffset_`.
		const visage::Point aimed { e.position.x + grabOffset_.x, e.position.y + grabOffset_.y };
		if (held_ == Grab::Pole) {
			dragPole(aimed);
		}
		else if (held_ == Grab::Zero) {
			dragZero(aimed);
		}
	};
	onMouseUp() += [this](const visage::MouseEvent& e) {
		(void)e;
		if (held_ == Grab::Pole) {
			control(fieldIndex(band_, dr::PoleRadius)).endGesture();
			control(fieldIndex(band_, dr::PoleAngle)).endGesture();
			// CLOSED IN THE SAME SHAPE THEY WERE OPENED, off the LATCHED binding rather than off the
			// band's type read again — a type changed mid-drag would otherwise close a bracket that
			// was never opened.
			if (heldBinding_ != ZeroBinding::None) {
				control(fieldIndex(band_, dr::ZeroAngle)).endGesture();
			}
			if (heldBinding_ == ZeroBinding::Full) {
				control(fieldIndex(band_, dr::ZeroRadius)).endGesture();
			}
		}
		else if (held_ == Grab::Zero) {
			control(fieldIndex(band_, dr::ZeroRadius)).endGesture();
			if (band(band_).type == dr::Type::Free) {
				control(fieldIndex(band_, dr::ZeroAngle)).endGesture();
			}
		}
		held_ = Grab::None;
		heldBinding_ = ZeroBinding::None;
		grabOffset_ = {};
	};
}

DeviceControl& DrawnExpanded::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, dr::kDescriptor, index, mod_,
		                                              history_);
	}
	return *controls_[index];
}

dr::Settings DrawnExpanded::settings() const {
	// THE ROW, READ THROUGH THE SEAM — the same change as the card's `settings()`, for the same reason:
	// `fxChain()` reaches a `DeviceRow` through a type only the parameter road has, and
	// `fieldsPresentAt`/`fieldAt` are that read in the vocabulary both hosts share. It is what lets the
	// RACK open this panel at all.
	dr::Row row {};
	const size_t present = std::min(host_.fieldsPresentAt(device_), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host_.fieldAt(device_, i);
	}
	return dr::stateOf(row, present);
}

const dr::Band& DrawnExpanded::band(size_t index) const {
	static dr::Band fallback {};
	if (index >= dr::kBands) {
		return fallback;
	}
	// A LOCAL COPY PER CALL WOULD DANGLE, so the settings are held for the lifetime of the reference.
	// Cheap: this is an editor, not the audio thread.
	static thread_local dr::Settings held;
	held = settings();
	return held.frame[frame_].band[index];
}

size_t DrawnExpanded::fieldIndex(size_t bandIndex, dr::BandField field) const {
	return dr::bandFieldIndex(frame_, bandIndex, field);
}

/// THE SAME THREE ROADS, ADDRESSED BY FRAME RATHER THAN BY THE ONE ON SCREEN. The frame menu writes
/// to a frame that is deliberately NOT the selected one — "copy to Frame 3" while looking at Frame 1
/// — so every one of these takes the frame explicitly instead of reading `frame_`.
size_t DrawnExpanded::fieldIndex(size_t frameIndex, size_t bandIndex, dr::BandField field) const {
	return dr::bandFieldIndex(frameIndex, bandIndex, field);
}

dr::Band DrawnExpanded::frameBand(size_t frameIndex, size_t bandIndex) const {
	if (frameIndex >= dr::kFrames || bandIndex >= dr::kBands) {
		return {};
	}
	// BY VALUE, unlike `band()` above. That one hands back a reference into a thread_local it refreshes
	// on every call, which is safe for one read and a trap for two — and this is read in pairs, a
	// wanted band beside a current one.
	return settings().frame[frameIndex].band[bandIndex];
}

dr::Frame DrawnExpanded::frameAt(size_t frameIndex) const {
	if (frameIndex >= dr::kFrames) {
		return {};
	}
	return settings().frame[frameIndex];
}

std::string DrawnExpanded::title() const { return dr::kTraits.name; }

visage::Point DrawnExpanded::maximumSize() const {
	return { kNoCap, minimumSize().y + kPad };
}

visage::Point DrawnExpanded::minimumSize() const {
	// THE ROW'S REAL HEIGHT, not the 90 this asked for before it had one. The overlay CLAMPS rather
	// than scrolling, so a band that is short cuts its row off rather than merely crowding it.
	const float row = Knob::columnHeight(Knob::kEngineRailDiameter);
	const float wide = kPad + static_cast<float>(kKnobCells) * kCellWidth + kPad;
	return { std::max(520.0f, wide),
	         kPad + kTabHeight + kRowGap + kPlaneHeight + kRowGap + row + kPad };
}

visage::Bounds DrawnExpanded::tabBounds(size_t frame) const {
	const float each = (width() - 2.0f * kPad) / static_cast<float>(dr::kFrames);
	return { kPad + each * static_cast<float>(frame), kPad, each - 2.0f, kTabHeight };
}

/// THE BAND'S LETTER. Composed here rather than at the draw site so a case can ask for it without
/// reading ink, which is the only way this suite can hold a claim about a label at all.
std::string DrawnExpanded::bandLabelText(size_t bandIndex) {
	const size_t clamped = std::min(bandIndex, dr::kBands - 1);
	return std::string(1, static_cast<char>('a' + clamped));
}

std::string DrawnExpanded::frameTabText(size_t frame) {
	// ONE TABLE, TWO PREFIXES. `dr::kFrameNumerals` holds the name; this panel has room for the word
	// in front of it and `DrawnCard`'s corner marker has room for an `F`. A second table here is how
	// the two surfaces would start calling one frame by two names.
	return std::string("Frame ") + dr::kFrameNumerals[std::min(frame, dr::kFrames - 1)];
}

visage::Bounds DrawnExpanded::planeBounds() const {
	return { kPad, kPad + kTabHeight + kRowGap, width() - 2.0f * kPad, kPlaneHeight };
}

float DrawnExpanded::angleForX(float x) const {
	const visage::Bounds box = planeBounds();
	const float t = box.width() > 0.0f ? std::clamp((x - box.x()) / box.width(), 0.0f, 1.0f) : 0.0f;
	return dr::kMinAngleLog2 + t * (dr::kMaxAngleLog2 - dr::kMinAngleLog2);
}

float DrawnExpanded::radiusAtHeight(float t) {
	const float above = std::clamp(t, 0.0f, 1.0f) - kCircleHeight;
	// THE TWO SIDES SHARE A SHAPE AND DIFFER ONLY IN HOW MANY PIXELS THEY WERE GIVEN. Both branches
	// send `above == 0` to `warped == 0`, so the join at the unit circle is continuous by
	// construction rather than by matching two formulas.
	const float span = above >= 0.0f ? 1.0f - kCircleHeight : kCircleHeight;
	const float warped = above * warpSpan() / span;
	// **CLAMPED, AND THE LOWER CLAMP IS FOR THE TUNING RATHER THAN FOR TODAY** — the header carries the
	// measurement. `sinh(-warpSpan())` recovers `-1/kCircleSoftness` only to within a ulp, so the
	// floor of the plane can hand back a tiny NEGATIVE radius, which `zeroIsSplitReal` reads as a
	// `Bandpass`: a drag to the bottom would change the KIND of filter the band is. At the constants
	// shipped it comes out at +5.36e-7 and this clamp is inert; over a swept grid of the two shaping
	// constants 175 of 304 pairs go negative. Do not delete it because the known-bad is green here.
	return std::clamp(circleUnwarp(warped), 0.0f, kRadiusCeiling);
}

float DrawnExpanded::heightForRadius(float radius) {
	const float warped = circleWarp(std::clamp(radius, 0.0f, kRadiusCeiling));
	const float span = warped >= 0.0f ? 1.0f - kCircleHeight : kCircleHeight;
	return std::clamp(kCircleHeight + warped * span / warpSpan(), 0.0f, 1.0f);
}

float DrawnExpanded::radiusForY(float y) const {
	const visage::Bounds box = planeBounds();
	const float t = box.height() > 0.0f ? std::clamp((box.bottom() - y) / box.height(), 0.0f, 1.0f)
	                                    : 0.0f;
	return radiusAtHeight(t);
}

namespace {

float xForAngleIn(const visage::Bounds& box, float angleLog2) {
	const float t = (std::clamp(angleLog2, dr::kMinAngleLog2, dr::kMaxAngleLog2) - dr::kMinAngleLog2)
	                / (dr::kMaxAngleLog2 - dr::kMinAngleLog2);
	return box.x() + box.width() * t;
}

/// **THE CEILING IS NO LONGER A PARAMETER, AND THAT IS NOT A TIDY-UP.** It used to be passed in and
/// was `kRadiusCeiling` at every call; the axis now derives its span from that same constant, so a
/// caller that passed a different ceiling would get a clamp and a scale that disagreed — a radius
/// drawn at a height its own inverse does not return. One constant, read once.
float yForRadiusIn(const visage::Bounds& box, float radius) {
	return box.bottom() - box.height() * DrawnExpanded::heightForRadius(radius);
}

} // namespace

visage::Point DrawnExpanded::poleCentre(size_t bandIndex) const {
	const visage::Bounds box = planeBounds();
	const dr::Band& b = band(bandIndex);
	return { xForAngleIn(box, b.pole.angleLog2), yForRadiusIn(box, b.pole.radius) };
}

bool DrawnExpanded::zeroIsSplitReal(size_t bandIndex) const { return band(bandIndex).zero.radius < 0.0f; }

visage::Point DrawnExpanded::zeroCentre(size_t bandIndex) const {
	const visage::Bounds box = planeBounds();
	const dr::Band& b = band(bandIndex);
	return { xForAngleIn(box, b.zero.angleLog2), yForRadiusIn(box, b.zero.radius) };
}

std::pair<visage::Point, visage::Point> DrawnExpanded::splitZeroCentres(size_t bandIndex) const {
	(void)bandIndex;
	const visage::Bounds box = planeBounds();
	// **THE TWO ENDS OF THE FREQUENCY AXIS, ON THE UNIT CIRCLE.** A split-real zero pair is one zero at
	// DC and one at Nyquist — which on a frequency axis is not an odd place to draw, it is the only
	// place: they ARE the two ends of the spectrum. A `Bandpass` therefore reads as "this band's nulls
	// are at both extremes", which is what a bandpass is.
	const float y = yForRadiusIn(box, 1.0f);
	return { visage::Point { box.x(), y }, visage::Point { box.right(), y } };
}

DrawnExpanded::Hit DrawnExpanded::hitAt(visage::Point position) const {
	Hit best;
	float bestDistance = kGrabRadius;
	for (size_t b = 0; b < dr::kBands; ++b) {
		const visage::Point pole = poleCentre(b);
		const float dp = std::hypot(position.x - pole.x, position.y - pole.y);
		// **THE POLE IS TESTED FIRST AND WINS A TIE, which is the Bypass case and not a detail.** A
		// bypassed band draws its zero exactly on its pole, and `<` below means the zero can never
		// take a press away from the pole at the same distance. The main handle stays grabbable, and
		// the way to separate the pair is the type menu.
		if (dp < bestDistance) {
			bestDistance = dp;
			best = Hit { Grab::Pole, b };
		}
		// AND A ZERO THE TYPE OWNS IS NOT OFFERED EITHER. The split-real guard below was the first
		// case of this rule — a zero that is not one point cannot be aimed at — and a DERIVED zero is
		// the same refusal for the same reason: the pole is the handle, and a handle that answered a
		// drag it would immediately undo is a control that lies.
		if (!zeroIsSplitReal(b) && zeroDraggable(b)) {
			const visage::Point zero = zeroCentre(b);
			const float dz = std::hypot(position.x - zero.x, position.y - zero.y);
			if (dz < bestDistance) {
				bestDistance = dz;
				best = Hit { Grab::Zero, b };
			}
		}
	}
	return best;
}

void DrawnExpanded::selectFrame(size_t frame) {
	if (frame >= dr::kFrames || frame == frame_) {
		return;
	}
	frame_ = frame;
	redraw();
}

void DrawnExpanded::dragPole(visage::Point position) {
	const float angle = angleForX(position.x);
	// CLAMPED TO THE POLE CEILING RATHER THAN TO THE PLANE'S. The plane goes to 2 so an allpass mirror
	// has somewhere to sit; a POLE above 1 is an unstable filter, and the editor must not be able to
	// ask for one — §3b's asymmetry, enforced where the gesture happens as well as in `sanitisePole`.
	const float radius = std::min(radiusForY(position.y), dr::kMaxPoleRadius);
	control(fieldIndex(band_, dr::PoleAngle)).setValue(static_cast<double>(angle));
	control(fieldIndex(band_, dr::PoleRadius)).setValue(static_cast<double>(radius));
	// **THE TYPE CARRIES THE ZERO — this is what "types should stick" means.** Recomputed from the
	// pole the drag has just written, through the same `zeroPlacementFor` the menu uses, so a band
	// that is a Notch stays a Notch as it moves instead of becoming a Notch-shaped Free.
	if (heldBinding_ != ZeroBinding::None) {
		const dr::Point moved { radius, angle };
		const std::optional<dr::Point> placed = dr::zeroPlacementFor(band(band_).type, moved);
		if (placed.has_value()) {
			control(fieldIndex(band_, dr::ZeroAngle)).setValue(static_cast<double>(placed->angleLog2));
			// BELL KEEPS ITS RADIUS AND ONLY BELL DOES. `zeroPlacementFor` would hand back a radius
			// derived from `kBellDefaultGain`, and writing it would reset a gain the hand had set —
			// the one control that makes a bell a bell. By design, Bell keeps its radius drag.
			if (heldBinding_ == ZeroBinding::Full) {
				control(fieldIndex(band_, dr::ZeroRadius))
				    .setValue(static_cast<double>(placed->radius));
			}
		}
	}
	redraw();
}

void DrawnExpanded::dragZero(visage::Point position) {
	const float angle = angleForX(position.x);
	// THE POSITIVE BRANCH ONLY. Dragging places a CONJUGATE pair; the split-real branch — a negative
	// radius, which is what a `Bandpass` needs — is not something a hand can aim at on this plane,
	// because it is not one point. The type menu is how a band gets into and out of it.
	const float radius = radiusForY(position.y);
	control(fieldIndex(band_, dr::ZeroRadius)).setValue(static_cast<double>(radius));
	// THE ANGLE IS THE POLE'S FOR A BELL, so a bell's zero slides along its own corner rather than
	// away from it. `hitAt` offers this grab for Free and Bell only, and Free is the one that owns
	// both coordinates.
	if (band(band_).type == dr::Type::Free) {
		control(fieldIndex(band_, dr::ZeroAngle)).setValue(static_cast<double>(angle));
	}
	redraw();
}

DrawnExpanded::ZeroBinding DrawnExpanded::bindingOf(dr::Type type) {
	switch (type) {
	// THE POLE CARRIES THE WHOLE ZERO. All three derive both coordinates from the pole, so a pole
	// drag has to rewrite both or the band stops being what it says it is.
	case dr::Type::Bypass:
	case dr::Type::Notch:
	case dr::Type::Allpass:
		return ZeroBinding::Full;

	// THE ANGLE ONLY — see `ZeroBinding::AngleOnly`. Bell's radius is its gain and belongs to the
	// hand.
	case dr::Type::Bell:
		return ZeroBinding::AngleOnly;

	// NOTHING TO CARRY. `Free` binds nothing by definition, and the other three place their zeros
	// independently of the pole: a lowpass's zeros sit at Nyquist and a highpass's at DC whatever
	// the corner frequency does, and a bandpass's are at both ends at once. Their zeros are still
	// not draggable — they are DERIVED, just not from the pole's position.
	case dr::Type::Lowpass:
	case dr::Type::Highpass:
	case dr::Type::Bandpass:
	case dr::Type::Free:
	case dr::Type::kCount:
		break;
	}
	return ZeroBinding::None;
}

bool DrawnExpanded::zeroDraggable(size_t bandIndex) const {
	if (bandIndex >= dr::kBands) {
		return false;
	}
	// FREE OWNS BOTH COORDINATES AND BELL OWNS ONE. Everything else derives its zero, and a derived
	// handle must not offer a gesture — the honesty rule, applied to a drag rather than to a knob.
	const dr::Type type = band(bandIndex).type;
	return type == dr::Type::Free || type == dr::Type::Bell;
}

void DrawnExpanded::applyType(size_t bandIndex, dr::Type type) {
	if (bandIndex >= dr::kBands) {
		return;
	}
	const std::optional<dr::Point> zero = dr::zeroPlacementFor(type, band(bandIndex).pole);
	// **ONE EDIT, AND NOW ACTUALLY ONE UNDO STEP.** The comment here already said "bracketed as one
	// edit ... a type is up to three parameter changes and a host recording automation wants one step
	// rather than three bare sets" — and the bracketing it described produced THREE
	// `DeviceValueEdit`s, because nesting `DeviceControl` gestures nests nothing: each `endGesture`
	// records its own. The intent was right and the mechanism could not express it. The type menu is a
	// menu click like the frame menu's, so it takes the frame menu's answer.
	std::vector<FieldChange> changes;
	const auto put = [&](dr::BandField field, float value) {
		const size_t index = fieldIndex(bandIndex, field);
		auto& c = control(index);
		const auto before = static_cast<float>(c.value());
		c.setValue(static_cast<double>(value));
		const auto after = static_cast<float>(c.value());
		if (before != after) {
			changes.push_back(FieldChange { index, before, after });
		}
	};
	put(dr::TypeIndex, static_cast<float>(static_cast<int>(type)));
	// **`Free` WRITES THE TYPE AND NO PLACEMENT, and that is the whole of what it does.** It used to
	// return here having written nothing, which was right while the type was not state: there was
	// then no difference between "free" and "whatever the numbers happen to be". Now the difference
	// is the stored field, so choosing Free RELEASES the zero — Free releases the
	// constraint — and leaves it exactly where the last binding left it.
	if (zero.has_value()) {
		put(dr::ZeroRadius, zero->radius);
		put(dr::ZeroAngle, zero->angleLog2);
	}
	recordFields(std::move(changes), "Type");
	redraw();
}

void DrawnExpanded::showTypeMenu(size_t bandIndex, visage::Point at) {
	visage::PopupMenu menu;
	for (size_t i = 0; i < std::size(dr::kTypeNames); ++i) {
		menu.addOption(static_cast<int>(i), dr::kTypeNames[i]);
	}
	menu.onSelection() = [this, bandIndex](int id) {
		if (id < 0 || id >= static_cast<int>(std::size(dr::kTypeNames))) {
			return;
		}
		applyType(bandIndex, static_cast<dr::Type>(id));
		if (notifier_ != nullptr) {
			notifier_->say(dr::kTypeNames[static_cast<size_t>(id)]);
		}
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
}

void DrawnExpanded::flattenFrame(size_t frameIndex) {
	// FLAT WITHOUT MOVING ANYTHING — each band's zero goes onto its own pole, which is the `Bypass`
	// placement, so the section contributes nothing and the six handles stay exactly where a hand put
	// them. That is the whole difference from `resetFrame` below, and it is the reason both exist.
	//
	// **AND THE TYPE SAYS `Bypass` TOO, OR THE FLATTEN DOES NOT SURVIVE THE NEXT DRAG.** This wrote the
	// Bypass PLACEMENT and left the stored type alone, which is `writeFrame`'s omission (backlog CM1)
	// in a second spelling: a band flattened while it stored `Notch` was flat only until a hand touched
	// its pole, at which point `dragPole` re-asserted the Notch and put the zero back on the circle.
	// MEASURED before the fix: flatten took the zero radius to 0.8 (on the pole) with the type still 3,
	// and one drag took it back to 1. A verb whose whole promise is *"the section contributes nothing"*
	// cannot leave behind the one field that will undo it.
	//
	// **AND IT IS SAID ONCE NOW, THROUGH `writeFrame`, WHICH IS WHY THE OMISSION COULD HAPPEN TWICE.**
	// This function used to write its own fields by hand — a second spelling of "these are a band's
	// fields", beside the list in `writeFrame` — and the type was missing from BOTH. Two spellings of
	// one rule is how the next field gets missed as well, so flatten now says what it WANTS as a
	// `dr::Frame` and hands it to the single road. A field added to a band is then added in one place.
	if (frameIndex >= dr::kFrames) {
		return;
	}
	dr::Frame flat = frameAt(frameIndex);
	for (dr::Band& band : flat.band) {
		band.zero = band.pole;
		band.type = dr::Type::Bypass;
	}
	writeFrame(frameIndex, flat, "Flatten");
}

void DrawnExpanded::resetFrame(size_t frameIndex) {
	// BACK TO THE SHIPPED FRAME, handles and all. `kDefaultFrames` is four identical flat frames with
	// the six poles spread across the spectrum, so this is flat AND re-spread — which is what makes it
	// a different offer from `flattenFrame`: one keeps your layout, the other starts over.
	if (frameIndex >= dr::kFrames) {
		return;
	}
	writeFrame(frameIndex, dr::kDefaultFrames[frameIndex], "Reset");
}

void DrawnExpanded::copyFrame(size_t from, size_t to) {
	if (from >= dr::kFrames || to >= dr::kFrames || from == to) {
		return;
	}
	writeFrame(to, frameAt(from), "Copy");
}

void DrawnExpanded::copyFrameToAll(size_t from) {
	if (from >= dr::kFrames) {
		return;
	}
	// **THE SOURCE IS READ ONCE, BEFORE ANY WRITE.** Reading it inside the loop would be reading a
	// frame while writing its neighbours — harmless today, because a write to frame 2 cannot change
	// frame 1, but it is the shape CLAUDE.md's double-buffer rule is about and it costs nothing to
	// take the snapshot first. What it also buys is plain: all three destinations get the SAME frame,
	// which is what "copy to all others" says.
	const dr::Frame source = frameAt(from);
	// ONE LIST ACROSS ALL THREE, so the whole verb is one undo step — three separate `writeFrame`
	// calls would be three, which is the defect the previous commit fixed arriving again through a new
	// door. `kFrames` is enumerated rather than "the other three" named, so a fifth frame joins this
	// without anybody editing it.
	std::vector<FieldChange> changes;
	for (size_t to = 0; to < dr::kFrames; ++to) {
		if (to == from) {
			continue;
		}
		writeFrameInto(to, source, changes);
	}
	recordFields(std::move(changes), "Copy");
}

void DrawnExpanded::writeFrame(size_t frameIndex, const dr::Frame& wanted, const char* verb) {
	std::vector<FieldChange> changes;
	writeFrameInto(frameIndex, wanted, changes);
	recordFields(std::move(changes), verb);
}

void DrawnExpanded::recordFields(std::vector<FieldChange> changes, const char* verb) {
	// **ONE MENU CLICK, ONE UNDO STEP.** Decided after CM2's review: a menu click is one action and
	// must be one undo step. Each of these verbs used to bracket every changed field
	// as its own `DeviceControl` gesture, and `endGesture` records a `DeviceValueEdit` apiece — so
	// "Copy to Frame 2" cost up to twenty-four presses of undo to take back, "copy to all others" would
	// have cost seventy-two, and neither could be taken back WHOLE. Half an undone copy is not a state
	// the editor should be able to be in.
	//
	// NOTHING IS LOST BY DROPPING THE GESTURES. `DeviceControl::beginGesture` stores the old value and
	// `endGesture` records the step — read them: they do nothing else, no host notify, no repaint. The
	// bracketing on this road was the history and only the history.
	//
	// AN EMPTY LIST RECORDS NOTHING, which is what keeps an identical copy off the stack — the property
	// the sibling case asserts through `canUndo()`, now guarded here instead of falling out of
	// `endGesture`'s own refusal.
	if (changes.empty() || history_ == nullptr) {
		return;
	}
	history_->record(std::make_unique<DeviceFieldsEdit>(host_, device_, std::move(changes), verb));
}

void DrawnExpanded::writeFrameInto(size_t frameIndex, const dr::Frame& wanted,
                                   std::vector<FieldChange>& into) {
	// A FIELD THAT IS ALREADY RIGHT IS NOT WRITTEN AT ALL.
	//
	// THE SKIP DOES NOT BUY THE UNDO GUARANTEE, AND I BELIEVED IT DID UNTIL A KNOWN-BAD SAID
	// OTHERWISE. It used to be `DeviceControl::endGesture` that refused to record an unmoved value —
	// *"none for a gesture that changed nothing"* — so an identical copy recorded no step whether this
	// skip was here or not. MEASURED: disabling this `continue` left the case green. That is no longer
	// true of the history (`recordFields` refuses an empty list instead), but the note stays, because
	// the reason the skip is KEPT is unchanged and is not about undo.
	//
	// WHAT IT DOES BUY is the write itself. `setValue` goes to the bridge and marks the surfaces that
	// draw it, so writing 120 unchanged fields would be 120 pointless bridge writes and a repaint on
	// every copy — including the common one of copying a frame onto its neighbour to compare them.
	// Kept for that, and stated plainly rather than left to look like the thing that guards undo.
	if (frameIndex >= dr::kFrames) {
		return;
	}
	for (size_t b = 0; b < dr::kBands; ++b) {
		const dr::Band& want = wanted.band[b];
		const dr::Band have = frameBand(frameIndex, b);
		// **THE TYPE IS ONE OF THE FIVE, AND LEAVING IT OUT WAS THE WHOLE OF BACKLOG CM1.** This wrote
		// four fields — the two points — and the fifth field of every band stayed behind. So a copy
		// moved a frame's GEOMETRY onto a frame that went on storing its OWN types, and the two then
		// disagreed: the destination was DRAWN as a Notch and STORED as a Bypass.
		//
		// THAT DISAGREEMENT IS INVISIBLE UNTIL THE NEXT DRAG, AND THEN IT IS LOUD. `dragPole` re-asserts
		// the stored type through `zeroPlacementFor` — which is "types stick" working exactly as
		// designed — so the first press-and-drag on a copied handle snapped the zero back onto the pole
		// and the Notch was gone. It looked like the gesture's fault — a click-and-drag on Drawn reset
		// the waypoint type. The gesture is innocent; nothing in this device's mouse
		// handling writes a type at all. What it did was reveal a frame this function had left in two
		// minds.
		//
		// MEASURED, before the fix: frame 1 band d a Notch (type 3, zero radius 1 at the pole's angle),
		// copied to frame 2 -> the geometry arrived and type read 6 (Bypass); one click-and-drag and the
		// zero sat on the pole. After it, the type travels and the drag leaves the Notch a Notch.
		//
		// SO `resetFrame` RESETS THE TYPE TOO, which is the other half of the same omission: a frame
		// reset to the shipped default had default POINTS and whatever types a hand had chosen.
		const std::array<std::pair<dr::BandField, float>, 5> fields { {
		    { dr::PoleRadius, want.pole.radius },
		    { dr::PoleAngle, want.pole.angleLog2 },
		    { dr::ZeroRadius, want.zero.radius },
		    { dr::ZeroAngle, want.zero.angleLog2 },
		    { dr::TypeIndex, static_cast<float>(static_cast<int>(want.type)) },
		} };
		const std::array<float, 5> current {
			have.pole.radius,  have.pole.angleLog2,
			have.zero.radius,  have.zero.angleLog2,
			static_cast<float>(static_cast<int>(have.type)),
		};
		for (size_t f = 0; f < fields.size(); ++f) {
			if (current[f] == fields[f].second) {
				continue;
			}
			const size_t index = fieldIndex(frameIndex, b, fields[f].first);
			auto& c = control(index);
			// THE BEFORE AND THE AFTER ARE BOTH READ FROM THE STORE, not taken from `wanted` — so the
			// record replays exactly what the write produced. `setField` coerces against the control's
			// own declaration (a stepped choice, a clamped radius), and a redo that wrote the UNCOERCED
			// number would land somewhere the original edit never did. `DeviceValueEdit`'s note makes
			// the same point about the road an undo must take.
			const auto before = static_cast<float>(c.value());
			// THE HOST'S HALF OF A GESTURE, per param the verb moves (host slots, T6b review) — not the full
			// pair, whose `endGesture` would record a step per field; the one undo step is `recordFields`'.
			c.beginHostGesture();
			c.setValue(static_cast<double>(fields[f].second));
			c.endHostGesture();
			const auto after = static_cast<float>(c.value());
			// AND A WRITE THAT COERCED BACK TO WHERE IT STARTED IS NOT A CHANGE. The skip above
			// compares WANTED against HAVE; this compares what the store actually holds either side of
			// the write, so a value the declaration rounds or clamps onto its own old value does not
			// put an inert entry in the undo step.
			if (before != after) {
				into.push_back(FieldChange { index, before, after });
			}
		}
	}
}

std::vector<std::pair<int, std::string>> DrawnExpanded::frameMenuOptions(size_t frameIndex) {
	// COPY-TO ENUMERATES `kFrames` rather than naming three, so a fifth frame appears here without
	// anybody editing this function.
	//
	// **AND "COPY TO ALL OTHERS" IS THE PLURAL OF THE LINES ABOVE IT.** It copies the WHOLE frame,
	// which is what the singular copies: all six
	// bands' pole, zero and type. Two adjacent entries spelling the same verb with different scopes
	// would be worse than not having the second, and there is nothing else it could mean here — a
	// frame IS its six bands, and Cutoff, Res, Drive, Wake and the two Morph axes are device-global
	// rather than per-frame.
	//
	// PLACED AFTER THE PER-FRAME COPIES, because it is the wider version of them and a verb that
	// overwrites three frames should not sit where a hand aiming for one lands.
	std::vector<std::pair<int, std::string>> options;
	options.emplace_back(kFrameMenuFlatten, "Flatten (keep handles)");
	options.emplace_back(kFrameMenuReset, "Reset to default");
	// THE COPY HALF IS SHARED with the Pattern's tabs (`slot_copy_menu.h`), so the two menus read alike.
	appendCopyToOptions(options, dr::kFrames, frameIndex, [](size_t f) { return std::string(frameTabText(f)); },
	                    kFrameMenuCopyAll);
	return options;
}

void DrawnExpanded::showFrameMenu(size_t frameIndex, visage::Point at) {
	// FLATTEN AND RESET ARE BOTH HERE BECAUSE THEY DIFFER ONCE A HAND HAS MOVED SOMETHING, which is
	// the case the menu exists for. They look like the same thing given the default
	// frame is already flat — it is, so on an untouched frame they agree, and the moment the six
	// handles have been arranged they part company: flatten keeps the arrangement, reset discards it.
	//
	// COPY-TO ENUMERATES `kFrames` rather than naming three, so a fifth frame appears here without
	// anybody editing this function.
	visage::PopupMenu menu;
	for (const auto& option : frameMenuOptions(frameIndex)) {
		menu.addOption(option.first, option.second);
	}
	menu.onSelection() = [this, frameIndex](int id) {
		if (id == kFrameMenuFlatten) {
			flattenFrame(frameIndex);
			if (notifier_ != nullptr) {
				notifier_->say("Flattened");
			}
			return;
		}
		if (id == kFrameMenuReset) {
			resetFrame(frameIndex);
			if (notifier_ != nullptr) {
				notifier_->say("Reset");
			}
			return;
		}
		if (id == kFrameMenuCopyAll) {
			copyFrameToAll(frameIndex);
			if (notifier_ != nullptr) {
				notifier_->say("Copied to all others");
			}
			return;
		}
		if (id < 0 || id >= static_cast<int>(dr::kFrames)) {
			return;
		}
		copyFrame(frameIndex, static_cast<size_t>(id));
		if (notifier_ != nullptr) {
			notifier_->say(std::string("Copied to ") + frameTabText(static_cast<size_t>(id)));
		}
	};
	showMenuAt(std::move(menu), *this, { at.x, at.y, 0.0f, 0.0f });
	redraw();
}

void DrawnExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	// EVERY ADAPTER, INCLUDING THE ONES NO WIDGET HOLDS — every control on this panel is edited by a
	// GESTURE rather than by a widget, so all 103 of them are in that category. FilterCard's rule:
	// cheap, total, and it cannot fall behind a widget list.
	for (size_t i = 0; i < static_cast<size_t>(dr::Param::kCount); ++i) {
		control(i).setModulation(mod, history);
	}
	// AND THE KNOBS THEMSELVES, which is a different list: `Knob::setModulation` is what puts a knob on
	// `ModState`'s watchers so an arm repaints it, and what lets its halo read the Alt bypass. The
	// adapters had the state and the six knobs did not — MEASURED 2026-09-14 by the every-panel-kind
	// glow case: six destination knobs on this panel unwatched, every other panel's watched.
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
}

visage::Bounds DrawnExpanded::knobRowBounds() const {
	const visage::Bounds plane = planeBounds();
	return { kPad, plane.bottom() + kRowGap, width() - 2.0f * kPad,
	         Knob::columnHeight(Knob::kEngineRailDiameter) };
}

void DrawnExpanded::resized() {
	// **THIS WAS EMPTY, AND THAT IS WHY THE WINDOW HAD NO CONTROLS TO LAY OUT.** The panel drew
	// everything by hand in `draw` and owned no widgets at all, so its card's six were unreachable
	// while it was open.
	const visage::Bounds row = knobRowBounds();
	const float cell = std::max(kCellWidth, row.width() / static_cast<float>(kKnobCells));
	for (size_t i = 0; i < knobs_.size(); ++i) {
		knobs_[i]->setBounds(row.x() + cell * static_cast<float>(i), row.y(), cell, row.height());
	}
}

void DrawnExpanded::drawPlane(visage::Canvas& canvas) {
	const visage::Bounds box = planeBounds();
	if (box.width() <= 1.0f || box.height() <= 1.0f) {
		return;
	}
	canvas.setColor(withAlpha(kLine, 0.25f));
	canvas.rectangleBorder(box.x(), box.y(), box.width(), box.height(), 1.0f);

	// THE RESPONSE OF THE FRAME BEING EDITED, behind the handles — the curve the hand is drawing. Not
	// the blended one the card shows: this panel edits ONE frame, and drawing the blend here would
	// show a curve the handles do not control.
	const dr::Settings s = settings();
	const dr::Realised realised = dr::realise(dr::withGlobals(s.frame[frame_], s.cutoff, s.res));
	canvas.setColor(withAlpha(kAccent, 0.35f));
	float prevX = box.x();
	float prevY = box.bottom();
	for (size_t i = 0; i < kCurvePoints; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kCurvePoints - 1);
		const float nu = std::exp2(dr::kMinAngleLog2 * (1.0f - t));
		const float db = 20.0f * std::log10(std::max(dr::magnitudeAt(realised, nu), 1e-6f));
		const float ty = std::clamp((kCurveTopDb - db) / (kCurveTopDb - kCurveBottomDb), 0.0f, 1.0f);
		const float x = box.x() + box.width() * t;
		const float y = box.y() + ty * box.height();
		if (i > 0) {
			canvas.segment(prevX, prevY, x, y, 1.5f, true);
		}
		prevX = x;
		prevY = y;
	}

	// **THE UNIT CIRCLE, AS A LINE.** Everything below it is stable; a pole above it would not be, and
	// cannot be dragged there. See the header: this is §3b's asymmetry drawn as geometry.
	const float unitY = yForRadiusIn(box, 1.0f);
	canvas.setColor(withAlpha(kInk, 0.45f));
	canvas.segment(box.x(), unitY, box.right(), unitY, 1.0f, true);

	for (size_t b = 0; b < dr::kBands; ++b) {
		const visage::Point pole = poleCentre(b);
		const bool selected = b == band_;

		if (zeroIsSplitReal(b)) {
			// TWO MARKS AT THE ENDS OF THE SPECTRUM, joined to the pole so the pair reads as belonging
			// to this band rather than as decoration on the border.
			const auto [left, right] = splitZeroCentres(b);
			canvas.setColor(withAlpha(kInk, 0.35f));
			canvas.segment(pole.x, pole.y, left.x, left.y, 1.0f, true);
			canvas.segment(pole.x, pole.y, right.x, right.y, 1.0f, true);
			canvas.setColor(kAccent);
			canvas.ring(left.x - kZeroRadius, left.y - kZeroRadius, kZeroRadius * 2.0f, 1.5f);
			canvas.ring(right.x - kZeroRadius, right.y - kZeroRadius, kZeroRadius * 2.0f, 1.5f);
		}
		else {
			const visage::Point zero = zeroCentre(b);
			canvas.setColor(withAlpha(kInk, 0.35f));
			canvas.segment(pole.x, pole.y, zero.x, zero.y, 1.0f, true);
			canvas.setColor(kAccent);
			canvas.ring(zero.x - kZeroRadius, zero.y - kZeroRadius, kZeroRadius * 2.0f, 1.5f);
		}

		canvas.setColor(selected ? kAccent : kInk);
		canvas.circle(pole.x - kPoleRadius, pole.y - kPoleRadius, kPoleRadius * 2.0f);

		// THE BAND'S OWN LETTER, BESIDE ITS POLE — a floating label near each curve-editor point, so
		// points can be lined up between frames.
		//
		// LETTERS RATHER THAN NUMERALS, DELIBERATELY, so they cannot collide with the frame ids.
		// The four frame tabs above this plane are numbered,
		// and six band numerals beside them would put two number systems on one surface with nothing
		// but position to tell them apart.
		//
		// THE BAND INDEX IS THE THING WORTH NAMING, because it is what a morph pairs: the puck blends
		// band `b` of one frame with band `b` of another, so the same letter sits on the same band in
		// every frame and switching tabs shows which handle becomes which. That is the question the label
		// answers — not "which handle is this" but "what will this turn into".
		//
		// ON THE POLE AND NOT THE ZERO. A bypassed band draws its zero exactly on its pole, so two
		// labels would land on one pixel; the pole is also the handle the type menu and the drag road
		// both address, so it is the one an identity belongs to.
		//
		// OFFSET UP-LEFT, CLEAR OF `kGrabRadius`. The label must not sit under the cursor during a
		// drag and must not eat the 13px grab region — it is a legend, not a target.
		canvas.setColor(withAlpha(selected ? kAccent : kInk, selected ? 1.0f : 0.55f));
		const std::string letter = bandLabelText(b);
		drawTracked(canvas, letter, font(kBandLabelSize), kControlTracking,
		            pole.x + kBandLabelDx, pole.y + kBandLabelDy, kBandLabelSize);
	}
}

void DrawnExpanded::draw(visage::Canvas& canvas) {
	for (size_t f = 0; f < dr::kFrames; ++f) {
		const visage::Bounds tab = tabBounds(f);
		const bool on = f == frame_;
		canvas.setColor(on ? kAccent : withAlpha(kLine, 0.35f));
		if (on) {
			canvas.roundedRectangle(tab.x(), tab.y(), tab.width(), tab.height(), 3.0f);
		}
		else {
			canvas.roundedRectangleBorder(tab.x(), tab.y(), tab.width(), tab.height(), 3.0f, 1.0f);
		}
		canvas.setColor(on ? kSurface : kMuted);
		drawTracked(canvas, frameTabText(f), font(kControlSize), kControlTracking, tab.x() + 8.0f,
		            tab.y(), tab.height());
	}
	drawPlane(canvas);
}

std::vector<size_t> DrawnExpanded::addressedParamsForTest() const {
	std::vector<size_t> out;
	// THE HUNDRED AND TWENTY, AND NOTHING ELSE. Every frame's every band's five fields: the poles and
	// zeros by dragging, the zeros again through the type menu, and **the type itself, which the menu
	// now writes** — it was ninety-six until the type became state. The seven globals belong to the
	// card, and claiming them here would be this panel taking credit for a surface it is not.
	//
	// THE LIST IS THE `BandField` ENUM ITSELF rather than four names typed out, because the four were
	// a copy of an order that had already moved once: `bandFieldIndex` is documented as the only
	// thing that should encode it, and a hand-written list here was a second encoding waiting to go
	// stale. It went stale the moment a fifth field existed, and the surface-reachability case is
	// what caught it.
	for (size_t f = 0; f < dr::kFrames; ++f) {
		for (size_t b = 0; b < dr::kBands; ++b) {
			for (size_t field = 0; field < dr::kBandFields; ++field) {
				out.push_back(dr::bandFieldIndex(f, b, static_cast<dr::BandField>(field)));
			}
		}
	}
	return out;
}

void DrawnExpanded::offeredControls(std::vector<Offered>& out) const {
	// THE POLES AND ZEROS, dragged on the plane, and the type its menu writes — `addressedParamsForTest`'s list.
	for (const size_t i : addressedParamsForTest()) {
		out.push_back({ &control(i), Road::Gesture });
	}
}

} // namespace feathers::ui
