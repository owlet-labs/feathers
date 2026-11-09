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

#include "plugin/ui/source_grid.h"

#include "plugin/ui/theme.h"

#include <cmath>
#include <string>

namespace feathers::ui {

namespace {

constexpr float kCornerRadius = 4.0f;

} // namespace

SourceGrid::SourceGrid(ParamBridge& bridge, ModState& mod, Notifier* notifier)
    : bridge_(bridge), mod_(mod), notifier_(notifier) {
	// The arm and the focus both live on the shared flag, so this frame redraws whenever either moves
	// — including when a CARD in the strip takes the focus away from us, which has to un-draw our
	// armed border in the same frame it draws theirs.
	watch_ = mod_.watch(this);

	onMouseDown() = [this](const visage::MouseEvent& e) { focusOrArm(hitAt(e.position)); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

float SourceGrid::preferredHeight() const {
	// The sounds' rows. (The Stack Index's row left for the stack page, backlog MO.)
	return kHeadingHeight + static_cast<float>(kRows) * (kCellHeight + kCellGap);
}

visage::Bounds SourceGrid::cellBounds(size_t slot) const {
	if (slot >= kMaxSounds) {
		return {};
	}
	// FROM THE FAMILY'S OWN ORIGIN rather than from the frame's, which is the growth seam in its
	// cheapest form: a second family appended below this one does not move a single cell of it.
	const float familyTop = kHeadingHeight;
	const size_t row = slot / kColumns;
	const size_t column = slot % kColumns;
	const float cellWidth = (width() - static_cast<float>(kColumns - 1) * kCellGap)
	                        / static_cast<float>(kColumns);
	return { static_cast<float>(column) * (cellWidth + kCellGap),
		     familyTop + static_cast<float>(row) * (kCellHeight + kCellGap), cellWidth, kCellHeight };
}

SourceGrid::Hit SourceGrid::hitAt(visage::Point pixels) const {
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (cellBounds(slot).contains(pixels)) {
			return { Part::Arrow, slot };
		}
	}
	return {};
}

bool SourceGrid::slotEnabled(size_t slot) const {
	if (slot >= kMaxSounds) {
		return false;
	}
	// READ FROM THE PARAMETER, not from any state of our own, so this grid and the rail's switch cannot
	// come to disagree about whether a sound is on. The same reading the sound stack's own card makes.
	const double value = bridge_.value(static_cast<clap_id>(soundParam(slot, SoundField::Enable)));
	return steppedIndex(value, kEnableNames.size()) != 0;
}

bool SourceGrid::slotArmed(size_t slot) const {
	return mod_.armed() && mod_.source() == weightSourceAt(slot);
}

void SourceGrid::focusOrArm(const Hit& hit) {
	if (hit.part != Part::Arrow) {
		return;
	}
	if (!slotEnabled(hit.slot)) {
		// A GREYED ARROW REFUSES, and says why rather than doing nothing visible. A switched-off sound
		// takes no place in the blend, so its weight is absent and there is nothing to route from —
		// arming it would light every destination for a source that reaches none of them.
		//
		// AND THE ASYMMETRY IS DELIBERATE, decided rather than overlooked: a sound switched off AFTER its
		// arrow was armed KEEPS the arming. Refusing to start something meaningless and destroying a
		// gesture already under way because an unrelated switch moved are different acts, and the second
		// is the worse one. A route made from a source that is currently dark is legitimate — it reads
		// zero until the sound comes back — and that is more use than making a hand arm the same thing
		// twice. Pinned by a case, so the next reader meets a decision rather than an oversight.
		if (notifier_ != nullptr) {
			notifier_->say(modSourceName(bridge_, weightSourceAt(hit.slot)) + " is switched off");
		}
		return;
	}

	const backplane::Source source = weightSourceAt(hit.slot);
	const bool wasFocused = mod_.source() == source;
	// Either way this becomes the focused source — the card's own rule, and for the card's own reason:
	// arming one source while the rings stay drawn for another is the surface disagreeing with itself.
	mod_.focus(source);

	// Clicking an arrow that was NOT focused arms it rather than toggling, so the hand that meant
	// "point this one" does not disarm a session another arrow started.
	const bool arm = !(wasFocused && mod_.armed());
	mod_.arm(arm);
	if (notifier_ != nullptr) {
		std::string message = modSourceName(bridge_, source);
		message += arm ? " — drag a highlighted knob to route" : " mapping off";
		notifier_->say(message);
	}
	redraw();
}

void SourceGrid::draw(visage::Canvas& canvas) {
	canvas.setColor(kMuted);
	drawTracked(canvas, "SOURCES", font(kKnobLabelSize), kKnobLabelTracking, 0.0f, 0.0f,
	            kHeadingHeight);

	const SoundHues& hues = bridge_.soundHues();

	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		const visage::Bounds cell = cellBounds(slot);
		const bool live = slotEnabled(slot);
		const bool armed = slotArmed(slot);
		const bool focused = mod_.source() == weightSourceAt(slot);

		// THE SOUND'S OWN COLOUR, which is how an arrow and its star on the circle are known to be the
		// same sound without being anywhere near each other. Faded rather than swapped when the sound is
		// off, so a greyed cell is still recognisably that sound's.
		const uint32_t hue = kSeedHues[hueIndexOf(hues, slot)];

		canvas.setColor(kSurface);
		canvas.roundedRectangle(cell.x(), cell.y(), cell.width(), cell.height(), kCornerRadius);
		// The border is the focus and turns to the modulation colour only while armed — the strip's own
		// grammar, because this is the strip's own verb.
		canvas.setColor(armed ? kModulation : (focused && live ? kAccent : kLine));
		canvas.roundedRectangleBorder(cell.x(), cell.y(), cell.width(), cell.height(), kCornerRadius,
		                              1.0f);

		// The letter is the star's own label — Perform draws its anchors as 'A' + slot, so a hand can
		// read which sound this is by the same mark it reads on the circle.
		const char letter[2] = { static_cast<char>('A' + slot), '\0' };
		canvas.setColor(live ? hue : withAlpha(hue, 0.35f));
		canvas.text(letter, font(kKnobReadoutSize), visage::Font::kLeft, cell.x() + 5.0f, cell.y(),
		            cell.width(), cell.height());

		// THE ARROW, the modulator card's exactly: a stem and two strokes for the head, drawn rather
		// than set. Same mark, same verb — this is the affordance the macro cards taught, and a second
		// glyph for the same action would read as a second action.
		const float cx = cell.right() - kArrowSize * 0.5f - 5.0f;
		const float cy = cell.y() + cell.height() * 0.5f;
		canvas.setColor(!live ? kDisabled : (armed ? kModulation : kMuted));
		drawArmArrow(canvas, cx, cy, kArrowSize);
	}

}

StackIndexArrow::StackIndexArrow(ParamBridge& bridge, ModState& mod, Notifier* notifier)
    : bridge_(bridge), mod_(mod), notifier_(notifier) {
	// The arm and the focus live on the shared flag, as the grid's do.
	watch_ = mod_.watch(this);
	onMouseDown() = [this](const visage::MouseEvent&) { focusOrArm(); };
	// LAST, after every `=` above — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

bool StackIndexArrow::stackEnabled() const {
	// The parameter's own stepped reading, as `ParamSet::stackCount` makes it.
	return std::lround(bridge_.value(static_cast<clap_id>(ParamId::StackCount))) > 1;
}

bool StackIndexArrow::stackArmed() const {
	return mod_.armed() && mod_.source() == stackIndexSource();
}

void StackIndexArrow::focusOrArm() {
	// THE SAME VERB AS A SOUND'S ARROW, and the same refusal: a stack of one gives its index nowhere to go.
	if (!stackEnabled()) {
		if (notifier_ != nullptr) {
			notifier_->say("Stack Index needs a Stack of 2 or more");
		}
		return;
	}
	const backplane::Source source = stackIndexSource();
	const bool wasFocused = mod_.source() == source;
	mod_.focus(source);
	const bool arm = !(wasFocused && mod_.armed());
	mod_.arm(arm);
	if (notifier_ != nullptr) {
		notifier_->say(arm ? "Stack Index — drag Fine or Level to route" : "Stack Index mapping off");
	}
	redraw();
}

void StackIndexArrow::draw(visage::Canvas& canvas) {
	// Named in words where a sound has its letter, with the grid's arrow and border grammar.
	const float w = width();
	const float h = height();
	const bool live = stackEnabled();
	const bool armed = stackArmed();
	const bool focused = mod_.source() == stackIndexSource();
	canvas.setColor(kSurface);
	canvas.roundedRectangle(0.0f, 0.0f, w, h, kCornerRadius);
	canvas.setColor(armed ? kModulation : (focused && live ? kAccent : kLine));
	canvas.roundedRectangleBorder(0.0f, 0.0f, w, h, kCornerRadius, 1.0f);
	canvas.setColor(live ? kInk : kDisabled);
	canvas.text("Stack index", font(kKnobReadoutSize), visage::Font::kLeft, 5.0f, 0.0f, w, h);
	canvas.setColor(!live ? kDisabled : (armed ? kModulation : kMuted));
	drawArmArrow(canvas, w - SourceGrid::kArrowSize * 0.5f - 5.0f, h * 0.5f, SourceGrid::kArrowSize);
}

} // namespace feathers::ui

namespace feathers::ui {

const char* StackIndexArrow::tooltipAt(visage::Point) const {
	return "The Stack Index: a value from -1 to +1 for each stacked oscillator, set by the Shape, Mapping and Curve "
	       "above. Route it to Fine, Level or the puck's position to spread the stack's oscillators apart. It needs a "
	       "Stack of 2 or more.";
}

const char* SourceGrid::tooltipAt(visage::Point local) const {
	const Hit hit = hitAt(local);
	if (hit.part == Part::Arrow) {
		return "How much of this sound is heard where the puck stands. Click the arrow, then drag a highlighted knob "
		       "to route it.";
	}
	return nullptr;
}

} // namespace feathers::ui
