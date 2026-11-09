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

#include "plugin/ui/param_edit.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/controls.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace feathers::ui {

namespace {

/// The mock's `.seg` and `.vcount` share a 7px radius and a 1px border; at this size a border is a
/// one-pixel fill and the radius is not worth faking.
constexpr float kBorder = 1.0f;

/// How much of a stepper's width each button takes. The label needs the middle; the buttons only
/// need to be comfortably hittable.
constexpr float kStepperButtonFraction = 0.22f;

void drawPanel(visage::Canvas& canvas, float w, float h, uint32_t fill) {
	canvas.setColor(kLine);
	canvas.fill(0.0f, 0.0f, w, h);
	canvas.setColor(fill);
	canvas.fill(kBorder, kBorder, w - 2.0f * kBorder, h - 2.0f * kBorder);
}

} // namespace

//------------------//
// SegmentedControl //
//------------------//

SegmentedControl::SegmentedControl(ParamBridge& bridge, WidgetRegistry& registry,
                                   const ParamDesc& desc, std::span<const char* const> names,
                                   Notifier* notifier)
    : owned_(std::make_unique<ParamControl>(bridge, &desc)), names_(names),
      notifier_(notifier) {
	control_ = owned_.get();
	// THE REGISTRY IS KEYED BY HOST PARAMETER, so only this road can subscribe to it. Knob's pair
	// carries the same note for the same reason.
	watches_.push_back(registry.watchToken(static_cast<clap_id>(desc.id), this));
	buildMouse();
}

SegmentedControl::SegmentedControl(device::Modulatable& control, WidgetRegistry& registry,
                                   std::span<const char* const> names, Notifier* notifier)
    : control_(&control), names_(names), notifier_(notifier) {
	// NO SUBSCRIPTION AND NO OWNED CONTROL, so there is no token to hold either. `registry` is taken
	// all the same so the two constructors
	// read alike at their call sites — Knob's pair does this too, and for the same reason: a control
	// should be movable from one road to the other without its surroundings changing.
	(void)registry;
	buildMouse();
}

void SegmentedControl::buildMouse() {
	// SHARED BY BOTH ROADS, and written out rather than reached by DELEGATION — which was the first
	// draft and was wrong. A delegating constructor's target initialises every member, so
	// `owned_ = make_unique(...)` evaluated in the delegation's argument list is destroyed the moment
	// the target runs, leaving control_ pointing at freed storage. Knob avoids it by having two real
	// constructors and one shared builder; so does this.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!usable()) {
			return;
		}
		// `position`, not `relativePosition()` — the latter is mouse movement, which resolved every
		// click to whatever sits at a delta of zero. See the note in param_row.cpp.
		select(segmentAt(e.position));
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void SegmentedControl::setVertical(bool vertical) {
	if (vertical_ != vertical) {
		vertical_ = vertical;
		redraw();
	}
}

void SegmentedControl::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

void SegmentedControl::setActive(bool active) {
	if (active_ != active) {
		active_ = active;
		redraw();
	}
}

int SegmentedControl::segmentAt(visage::Point position) const {
	if (names_.empty() || width() <= 0.0f || height() <= 0.0f || position.x < 0.0f ||
	    position.x >= width() || position.y < 0.0f || position.y >= height()) {
		return -1;
	}
	// The options divide whichever axis they are stacked along.
	const float span = vertical_ ? height() : width();
	const float along = vertical_ ? position.y : position.x;
	const float each = span / static_cast<float>(names_.size());
	const int index = static_cast<int>(along / each);
	return std::min(index, static_cast<int>(names_.size()) - 1);
}

int SegmentedControl::selectedIndex() const {
	// The parameter table's own stepped rule. Rounding here and clamping nowhere would let a value the
	// engine reads as the last option draw as an option past the end of the list.
	return static_cast<int>(steppedIndex(control_->value(), names_.size()));
}

void SegmentedControl::select(int index) {
	// A CONTROL THAT CHANGES NOTHING WHERE IT STANDS TAKES NO CHOICE (a Poly switch inside a Rack, backlog NO).
	if (control_ != nullptr && control_->unavailable() != nullptr) {
		return;
	}
	if (index < 0 || index >= static_cast<int>(names_.size()) || index == selectedIndex()) {
		return;
	}
	// One complete edit, bracketed, so the host records a single undoable step. The index IS the
	// control's value, which holds because every choice control runs 0..n-1 in steps of 1; if one ever
	// does not, this and selectedIndex() both want desc().min + index * step.
	//
	// BRACKETED ON BOTH ROADS, and on the swallowed one the brackets do nothing — a device with no
	// clap_id has no host automation to latch. FxControl says so in as many words. The widget brackets
	// regardless rather than asking which kind of control it holds, which is the point of the
	// interface.
	control_->beginGesture();
	control_->setValue(static_cast<double>(index));
	control_->endGesture();
	if (notifier_ != nullptr) {
		notifier_->say(std::string(control_->desc().name) + ' '
		               + names_[static_cast<size_t>(index)]);
	}
	redraw();
}

void SegmentedControl::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	drawPanel(canvas, w, h, kVoid);

	if (names_.empty()) {
		return;
	}
	const float each = (vertical_ ? h : w) / static_cast<float>(names_.size());
	const int selected = selectedIndex();
	const visage::Font& face = font(kControlSize);

	for (size_t i = 0; i < names_.size(); ++i) {
		const float offset = static_cast<float>(i) * each;
		const float x = vertical_ ? 0.0f : offset;
		const float y = vertical_ ? offset : 0.0f;
		const float cellW = vertical_ ? w : each;
		const float cellH = vertical_ ? each : h;
		const bool on = static_cast<int>(i) == selected;
		if (on) {
			canvas.setColor(kSurfaceRaised);
			canvas.fill(x + kBorder, y + kBorder, cellW - 2.0f * kBorder, cellH - 2.0f * kBorder);
		}
		// Disabled wins over selected: a greyed control that still highlights its choice reads as
		// available, which is the one thing the honesty rule is about. Inactive greys the same way —
		// what differs is that the hand may still move it. See setActive.
		canvas.setColor(!usable() || !active_ ? kDisabled : (on ? kInk : kMuted));

		const std::string label = names_[i];
		const float textWidth = trackedWidth(face, label, kControlTracking);
		drawTracked(canvas, label, face, kControlTracking, x + (cellW - textWidth) * 0.5f, y, cellH);
	}
}

bool SegmentedControl::labelsFit() const {
	if (names_.empty()) {
		return true;
	}
	const float each = (vertical_ ? height() : width()) / static_cast<float>(names_.size());
	const float room = (vertical_ ? width() : each) - 2.0f * kBorder;
	const visage::Font& face = font(kControlSize);
	for (const char* name : names_) {
		if (trackedWidth(face, name, kControlTracking) > room) {
			return false;
		}
	}
	return true;
}

//---------//
// Stepper //
//---------//

Stepper::Stepper(ParamBridge& bridge, WidgetRegistry& registry, const ParamDesc& desc, double step,
                 Notifier* notifier)
    : bridge_(bridge), desc_(desc), step_(step), notifier_(notifier) {
	watches_.push_back(registry.watchToken(static_cast<clap_id>(desc.id), this));

	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (!enabled_) {
			return;
		}
		// `position`, not `relativePosition()` — see the note above and in param_row.cpp.
		nudge(buttonAt(e.position));
	};
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void Stepper::setRowLabel(const char* label) {
	rowLabel_ = label;
	redraw();
}

void Stepper::setEnabled(bool enabled) {
	if (enabled_ != enabled) {
		enabled_ = enabled;
		redraw();
	}
}

void Stepper::setCeiling(double ceiling) {
	if (ceiling_ != ceiling) {
		ceiling_ = ceiling;
		redraw();
	}
}

std::string Stepper::reading() const {
	char printed[32] = {};
	if (!valueToText(id(), bridge_.value(id()), printed, sizeof(printed))) {
		return {};
	}
	std::string text = rowLabel_ != nullptr ? std::string(rowLabel_) + ' ' + printed : std::string(printed);
	// HELD BELOW WHAT IT SAYS: the setting stands, and what the engine plays is the ceiling — both are true, so
	// both are drawn, rather than the stepper pretending the setting moved or pretending it is played.
	if (bridge_.value(id()) > ceiling_) {
		char held[32] = {};
		if (valueToText(id(), ceiling_, held, sizeof(held))) {
			// "16 (plays 6)", in words: the first draft drew "16 → 6" and the control font has no arrow glyph, so
			// the photograph showed "16   6" — a reading that says nothing. Words are in every font.
			text += std::string(" (plays ") + held + ")";
		}
	}
	return text;
}

int Stepper::buttonAt(visage::Point position) const {
	if (width() <= 0.0f || position.x < 0.0f || position.x >= width() || position.y < 0.0f ||
	    position.y >= height()) {
		return 0;
	}
	const float button = width() * kStepperButtonFraction;
	if (position.x < button) {
		return -1;
	}
	if (position.x >= width() - button) {
		return 1;
	}
	return 0;
}

void Stepper::nudge(int direction) {
	if (direction == 0) {
		return;
	}
	// NOT PAST THE CEILING: a value above it is the same sound as the ceiling, so a press that would put it there
	// changes nothing anybody hears (the honesty rule).
	//
	// AND A PRESS DOWN FROM ABOVE IT STEPS FROM WHAT IS HEARD, not from what is stored: "16 (plays 6)" pressed down is 5.
	// Stepping from 16 would spend ten presses on changes nobody hears, which is the choice the control shows not
	// being the choice it makes (voice-stacking doc §7, assumption 8).
	const double top = direction > 0 ? std::min(desc_.max, std::max(ceiling_, bridge_.value(id()))) : desc_.max;
	const double from = direction < 0 ? std::min(bridge_.value(id()), ceiling_) : bridge_.value(id());
	const double target = std::clamp(from + static_cast<double>(direction) * step_, desc_.min, top);
	if (target == bridge_.value(id())) {
		// Already at the end of its range. Posting anyway would spend a gesture on nothing and give
		// the host an undo step that changes no sound.
		return;
	}
	// ONE HOST GESTURE AND ONE UNDO STEP (backlog JB).
	(void)writeParamAsEdit(bridge_, id(), target, "Step");
	if (notifier_ != nullptr) {
		char printed[32] = {};
		if (valueToText(id(), target, printed, sizeof(printed))) {
			notifier_->say(std::string(desc_.name) + ' ' + printed);
		}
	}
	redraw();
}

void Stepper::draw(visage::Canvas& canvas) {
	const float w = width();
	const float h = height();
	drawPanel(canvas, w, h, kVoid);

	const visage::Font& face = font(kControlSize);
	const float button = w * kStepperButtonFraction;
	const bool atMin = bridge_.value(id()) <= desc_.min;
	const bool atMax = bridge_.value(id()) >= std::min(desc_.max, ceiling_);

	// The ends grey out when there is nowhere further to go, so the control says where its limits are
	// rather than silently refusing.
	canvas.setColor(!enabled_ || atMin ? kDisabled : kMuted);
	canvas.text("\xe2\x88\x92", face, visage::Font::kCenter, 0.0f, 0.0f, button, h);
	canvas.setColor(!enabled_ || atMax ? kDisabled : kMuted);
	canvas.text("+", face, visage::Font::kCenter, w - button, 0.0f, button, h);

	const std::string text = reading();
	if (!text.empty()) {
		canvas.setColor(enabled_ ? kInk : kDisabled);
		canvas.text(text, face, visage::Font::kCenter, button, 0.0f, w - 2.0f * button, h);
	}
}

} // namespace feathers::ui
